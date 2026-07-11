// Darkpad - a small, dark, syntax-highlighting text editor that looks and
// behaves like Windows Notepad, but is our own program. Because we own the
// window and the file I/O, saving is byte-faithful by construction and we are
// free to add a dark RichEdit, syntax colors, a car-window tint, and custom
// chrome without fighting anyone else's assumptions.
//
// Build (64-bit):
//   zig cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows \
//       -o darkpad.exe darkpad_app.c \
//       -luser32 -lgdi32 -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lole32 -lshell32
//
// The editor control is a RICHEDIT50W (Msftedit.dll), loaded at startup.
// Headless save-integrity self-test:  darkpad.exe --selftest <in.txt> <out.txt>

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <stdarg.h>
#include <windowsx.h>
#include <math.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define DP_DEBUG 0
#if DP_DEBUG
static void dbg(const char *fmt, ...){
    char b[512]; va_list ap; va_start(ap,fmt); wvsprintfA(b,fmt,ap); va_end(ap);
    HANDLE h=CreateFileA("darkpad-debug.log",FILE_APPEND_DATA,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h!=INVALID_HANDLE_VALUE){ SetFilePointer(h,0,NULL,FILE_END); DWORD w;
        WriteFile(h,b,lstrlenA(b),&w,NULL); WriteFile(h,"\r\n",2,&w,NULL); CloseHandle(h); }
}
#else
#define dbg(...) ((void)0)
#endif

// ---- theme colors (overridable from %APPDATA%\Darkpad\darkpad.ini) ---------
static COLORREF DARK_BG  = RGB(30, 30, 30);
static COLORREF DARK_TXT = RGB(220, 220, 220);
static COLORREF BAR_BG   = RGB(43, 43, 43);
static COLORREF BAR_HOT  = RGB(60, 60, 60);
static COLORREF BAR_EDGE = RGB(43, 43, 43);
static int      OPACITY  = 100;   // window opacity %, 100 = solid, 95 = car tint

#define COL_COMMENT RGB(106, 153, 85)
#define COL_STRING  RGB(206, 145, 120)
#define COL_NUMBER  RGB(181, 206, 168)
#define COL_KEYWORD RGB(86, 156, 214)

// ---- menu command ids ------------------------------------------------------
#define IDM_NEW        1
#define IDM_OPEN       2
#define IDM_SAVE       3
#define IDM_SAVEAS     4
#define IDM_EXIT       5
#define IDM_UNDO      10
#define IDM_CUT       11
#define IDM_COPY      12
#define IDM_PASTE     13
#define IDM_DELETE    14
#define IDM_SELECTALL 15
#define IDM_WORDWRAP  20
#define IDM_ABOUT     30
#define IDM_FIND      40
#define IDM_REPLACE   41
#define IDM_FONT      42
#define IDM_ZOOMIN    43
#define IDM_ZOOMOUT   44
#define IDM_ZOOMRESET 45

// ---- custom chrome geometry -----------------------------------------------
#define TITLE_H    36    // custom title bar height
#define MENU_H     28    // our own dark menu row height
#define BTN_W      46    // width of each window button (min / max / close)
#define ANIM_TIMER 0xA411

// ---- undocumented UAH menu messages (for the dark menu bar) ----------------
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU     0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#endif
typedef union { struct { DWORD cx, cy; } a[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD f : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

// ---- globals ---------------------------------------------------------------
static HINSTANCE g_inst;
static HWND   g_main, g_edit, g_status;
static HBRUSH g_barBrush, g_hotBrush;
static WNDPROC g_origStatus, g_origEdit;
static WCHAR  g_path[MAX_PATH] = L"";     // current file, empty = untitled
static int    g_dirty = 0;
static int    g_wordwrap = 1;
static int    g_inHighlight = 0;          // suppress the dirty flag while recoloring
// file encoding we loaded / will save as
enum { ENC_UTF8, ENC_UTF8BOM, ENC_UTF16LE, ENC_ANSI };
static int    g_enc = ENC_UTF8;
static HFONT  g_titleFont=NULL, g_menuFont=NULL, g_glyphFont=NULL;
static HMENU  g_mFile=NULL, g_mEdit=NULL, g_mFormat=NULL, g_mHelp=NULL;
static int    g_btnHover=-1, g_menuHover=-1;   // -1 = none hovered
static HWND   g_findDlg=NULL;                  // modeless Find/Replace dialog
static FINDREPLACEW g_fr;                       // its persistent state
static WCHAR  g_findBuf[128]=L"", g_replBuf[128]=L"";
static UINT   g_findMsg=0;                       // RegisterWindowMessage(FINDMSGSTRING)

static HBRUSH bar_brush(void){ if(!g_barBrush) g_barBrush=CreateSolidBrush(BAR_BG); return g_barBrush; }
static HBRUSH hot_brush(void){ if(!g_hotBrush) g_hotBrush=CreateSolidBrush(BAR_HOT); return g_hotBrush; }

// ===========================================================================
//  Config: %APPDATA%\Darkpad\darkpad.ini
// ===========================================================================
static COLORREF lighten(COLORREF c, int d){
    int r=GetRValue(c)+d, g=GetGValue(c)+d, b=GetBValue(c)+d;
    if(r>255)r=255; if(g>255)g=255; if(b>255)b=255; return RGB(r,g,b);
}
static COLORREF parse_hex(const WCHAR *s, COLORREF fb){
    while(*s==L'#'||*s==L' '||*s==L'\t') s++;
    unsigned v=0; int n=0;
    for(; *s && n<6; s++){ WCHAR c=*s; int d;
        if(c>=L'0'&&c<=L'9') d=c-L'0';
        else if(c>=L'a'&&c<=L'f') d=c-L'a'+10;
        else if(c>=L'A'&&c<=L'F') d=c-L'A'+10;
        else break;
        v=(v<<4)|(unsigned)d; n++; }
    if(n!=6) return fb;
    return RGB((v>>16)&0xff,(v>>8)&0xff,v&0xff);
}
static void load_config(void){
    WCHAR appdata[MAX_PATH];
    if(!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    wsprintfW(dir, L"%s\\Darkpad", appdata);
    wsprintfW(ini, L"%s\\darkpad.ini", dir);
    if(GetFileAttributesW(ini)==INVALID_FILE_ATTRIBUTES){
        CreateDirectoryW(dir, NULL);
        static const char *tmpl =
            ";\r\n; Darkpad settings. Colors are hex RRGGBB, like web colors.\r\n"
            "; opacity is a window tint: 100 = solid, 95 = faint see-through (car tint).\r\n"
            "; Change a value, save, then reopen Darkpad.\r\n;\r\n"
            "[colors]\r\nbackground=1e1e1e\r\ntext=dcdcdc\r\nbar=2b2b2b\r\n"
            "\r\n[window]\r\nopacity=100\r\n";
        HANDLE h=CreateFileW(ini,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(h!=INVALID_HANDLE_VALUE){ DWORD w; WriteFile(h,tmpl,(DWORD)lstrlenA(tmpl),&w,NULL); CloseHandle(h); }
        return;
    }
    WCHAR buf[64];
    GetPrivateProfileStringW(L"colors",L"background",L"",buf,64,ini); if(buf[0]) DARK_BG=parse_hex(buf,DARK_BG);
    GetPrivateProfileStringW(L"colors",L"text",L"",buf,64,ini);       if(buf[0]) DARK_TXT=parse_hex(buf,DARK_TXT);
    GetPrivateProfileStringW(L"colors",L"bar",L"",buf,64,ini);        if(buf[0]) BAR_BG=parse_hex(buf,BAR_BG);
    int op=(int)GetPrivateProfileIntW(L"window",L"opacity",100,ini);
    if(op<20) op=20; if(op>100) op=100; OPACITY=op;
    BAR_HOT=lighten(BAR_BG,17); BAR_EDGE=BAR_BG;
}

// ===========================================================================
//  Syntax highlighting (display-only; never mutates the text bytes)
// ===========================================================================
static const WCHAR *const KEYWORDS[] = {
    L"if",L"else",L"for",L"while",L"do",L"return",L"function",L"def",L"class",
    L"int",L"void",L"const",L"let",L"var",L"new",L"import",L"from",L"public",
    L"private",L"static",L"true",L"false",L"null",L"None",L"struct",L"switch",
    L"case",L"break",L"continue",L"float",L"double",L"char",L"bool",L"else",
};
#define NUM_KEYWORDS (int)(sizeof(KEYWORDS)/sizeof(KEYWORDS[0]))
static int is_word_ch(WCHAR c){ return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'_'; }
static int is_ident_start(WCHAR c){ return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||c==L'_'; }
static int is_digit(WCHAR c){ return c>=L'0'&&c<=L'9'; }
static int is_keyword(const WCHAR *s, int len){
    for(int i=0;i<NUM_KEYWORDS;i++){ const WCHAR *k=KEYWORDS[i]; int j=0;
        while(j<len&&k[j]&&k[j]==s[j]) j++;
        if(j==len&&k[j]==0) return 1; }
    return 0;
}
static void color_run(HWND h, int start, int end, COLORREF col){
    if(end<=start) return;
    CHARRANGE cr={start,end};
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
    CHARFORMAT2W cf; ZeroMemory(&cf,sizeof(cf));
    cf.cbSize=sizeof(cf); cf.dwMask=CFM_COLOR; cf.crTextColor=col;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}
static void highlight_text(HWND h, const WCHAR *t, int n){
    color_run(h, 0, n, DARK_TXT);
    int i=0;
    while(i<n){ WCHAR c=t[i];
        if((c==L'/'&&i+1<n&&t[i+1]==L'/')||c==L'#'){ int s=i; while(i<n&&t[i]!=L'\r'&&t[i]!=L'\n') i++; color_run(h,s,i,COL_COMMENT); continue; }
        if(c==L'/'&&i+1<n&&t[i+1]==L'*'){ int s=i; i+=2; while(i<n&&!(t[i]==L'*'&&i+1<n&&t[i+1]==L'/')) i++; if(i<n) i+=2; color_run(h,s,i,COL_COMMENT); continue; }
        if(c==L'"'||c==L'\''){ WCHAR q=c; int s=i; i++; while(i<n&&t[i]!=q&&t[i]!=L'\r'&&t[i]!=L'\n'){ if(t[i]==L'\\'&&i+1<n) i++; i++; } if(i<n&&t[i]==q) i++; color_run(h,s,i,COL_STRING); continue; }
        if(is_digit(c)){ int s=i; while(i<n&&(is_word_ch(t[i])||t[i]==L'.')) i++; color_run(h,s,i,COL_NUMBER); continue; }
        if(is_ident_start(c)){ int s=i; while(i<n&&is_word_ch(t[i])) i++; if(is_keyword(t+s,i-s)) color_run(h,s,i,COL_KEYWORD); continue; }
        i++;
    }
}
#define MAX_HL_CHARS (400*1024)
static void do_highlight(HWND h){
    GETTEXTLENGTHEX gtl={GTL_DEFAULT|GTL_NUMCHARS,1200};
    LONG n=(LONG)SendMessageW(h,EM_GETTEXTLENGTHEX,(WPARAM)&gtl,0);
    if(n<=0||n>MAX_HL_CHARS) return;
    WCHAR *buf=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(size_t)(n+1)*sizeof(WCHAR));
    if(!buf) return;
    GETTEXTEX gt; ZeroMemory(&gt,sizeof(gt));
    gt.cb=(DWORD)((n+1)*sizeof(WCHAR)); gt.flags=GT_DEFAULT; gt.codepage=1200;
    LONG got=(LONG)SendMessageW(h,EM_GETTEXTEX,(WPARAM)&gt,(LPARAM)buf);
    if(got<0) got=0; buf[got]=0;
    CHARRANGE sel; SendMessageW(h,EM_EXGETSEL,0,(LPARAM)&sel);
    int firstVisible=(int)SendMessageW(h,EM_GETFIRSTVISIBLELINE,0,0);
    g_inHighlight=1;   // our EM_SETCHARFORMAT calls raise EN_CHANGE; ignore them
    SendMessageW(h,WM_SETREDRAW,FALSE,0);
    highlight_text(h,buf,got);
    SendMessageW(h,EM_EXSETSEL,0,(LPARAM)&sel);
    int nowFirst=(int)SendMessageW(h,EM_GETFIRSTVISIBLELINE,0,0);
    if(nowFirst!=firstVisible) SendMessageW(h,EM_LINESCROLL,0,(LPARAM)(firstVisible-nowFirst));
    SendMessageW(h,WM_SETREDRAW,TRUE,0);
    g_inHighlight=0;
    InvalidateRect(h,NULL,TRUE);
    HeapFree(GetProcessHeap(),0,buf);
}
static UINT_PTR g_hlTimer=0;
#define HL_TIMER_ID 0xD00D
#define HL_DEBOUNCE 180

// ===========================================================================
//  Dark status bar (owns its own painting; shows the real part text)
// ===========================================================================
#define SB_TEXT_INSET 6
static void status_paint(HWND sb, HDC hdc){
    RECT client; GetClientRect(sb,&client);
    FillRect(hdc,&client,bar_brush());
    HFONT font=(HFONT)SendMessageW(sb,WM_GETFONT,0,0);
    HGDIOBJ oldFont=font?SelectObject(hdc,font):NULL;
    SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,DARK_TXT);
    int parts=(int)SendMessageW(sb,SB_GETPARTS,0,0); if(parts<1) parts=1;
    for(int i=0;i<parts;i++){ RECT rc;
        if(!SendMessageW(sb,SB_GETRECT,(WPARAM)i,(LPARAM)&rc)) continue;
        FillRect(hdc,&rc,bar_brush());
        DWORD lr=(DWORD)SendMessageW(sb,SB_GETTEXTLENGTHW,(WPARAM)i,0);
        int len=(int)LOWORD(lr); if(len<=0) continue; if(len>511) len=511;
        WCHAR buf[512]; buf[0]=0; SendMessageW(sb,SB_GETTEXTW,(WPARAM)i,(LPARAM)buf); buf[len]=0;
        RECT tr=rc; tr.left+=SB_TEXT_INSET;
        DrawTextW(hdc,buf,len,&tr,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_NOPREFIX);
    }
    if(oldFont) SelectObject(hdc,oldFont);
}
static LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    if(m==WM_ERASEBKGND){ RECT rc; GetClientRect(h,&rc); FillRect((HDC)w,&rc,bar_brush()); return 1; }
    if(m==WM_PAINT){ PAINTSTRUCT ps; HDC hdc=BeginPaint(h,&ps); status_paint(h,hdc); EndPaint(h,&ps); return 0; }
    if(m==WM_NCDESTROY){ WNDPROC op=g_origStatus; if(op) SetWindowLongPtrW(h,GWLP_WNDPROC,(LONG_PTR)op);
        return op?CallWindowProcW(op,h,m,w,l):DefWindowProcW(h,m,w,l); }
    return g_origStatus?CallWindowProcW(g_origStatus,h,m,w,l):DefWindowProcW(h,m,w,l);
}

// ===========================================================================
//  Dark menu bar (UAH draw)
// ===========================================================================
static void uah_draw_menu(HWND hwnd, UAHMENU *pum){
    if(!pum) return;
    MENUBARINFO mbi={sizeof(mbi)};
    if(!GetMenuBarInfo(hwnd,OBJID_MENU,0,&mbi)) return;
    RECT wr; GetWindowRect(hwnd,&wr);
    RECT bar=mbi.rcBar; OffsetRect(&bar,-wr.left,-wr.top);
    FillRect(pum->hdc,&bar,bar_brush());
}
static void uah_draw_menuitem(UAHDRAWMENUITEM *p){
    if(!p) return;
    WCHAR cap[256]; cap[0]=0;
    MENUITEMINFOW mii={sizeof(mii)}; mii.fMask=MIIM_STRING|MIIM_STATE; mii.dwTypeData=cap; mii.cch=255;
    GetMenuItemInfoW(p->um.hmenu,(UINT)p->umi.iPosition,TRUE,&mii);
    UINT st=p->dis.itemState;
    int hot=(st&(ODS_HOTLIGHT|ODS_SELECTED))?1:0;
    int dis=(st&(ODS_GRAYED|ODS_DISABLED))?1:0;
    FillRect(p->dis.hDC,&p->dis.rcItem,hot?hot_brush():bar_brush());
    SetBkMode(p->dis.hDC,TRANSPARENT);
    SetTextColor(p->dis.hDC,dis?RGB(120,120,120):DARK_TXT);
    UINT f=DT_CENTER|DT_SINGLELINE|DT_VCENTER; if(st&ODS_NOACCEL) f|=DT_HIDEPREFIX;
    DrawTextW(p->dis.hDC,cap,-1,&p->dis.rcItem,f);
}
static void paint_menu_underline(HWND hwnd){
    MENUBARINFO mbi={sizeof(mbi)};
    if(!GetMenuBarInfo(hwnd,OBJID_MENU,0,&mbi)) return;
    RECT wr; GetWindowRect(hwnd,&wr);
    RECT bar=mbi.rcBar; OffsetRect(&bar,-wr.left,-wr.top);
    RECT sliver={bar.left,bar.bottom,bar.right,bar.bottom+1};
    HDC hdc=GetWindowDC(hwnd);
    if(hdc){ HBRUSH b=CreateSolidBrush(BAR_EDGE); FillRect(hdc,&sliver,b); DeleteObject(b); ReleaseDC(hwnd,hdc); }
}
static void enable_dark_app_mode(void){
    HMODULE ux=LoadLibraryW(L"uxtheme.dll"); if(!ux) return;
    typedef int (WINAPI *SPAM)(int); typedef void (WINAPI *FMT)(void);
    SPAM setMode=(SPAM)GetProcAddress(ux,MAKEINTRESOURCEA(135));
    FMT flush=(FMT)GetProcAddress(ux,MAKEINTRESOURCEA(136));
    if(setMode) setMode(2); if(flush) flush();
}

// ===========================================================================
//  Editor: dark RichEdit + syntax subclass
// ===========================================================================
static void apply_edit_visuals(HWND h){
    SendMessageW(h,EM_SETBKGNDCOLOR,0,(LPARAM)DARK_BG);
    CHARFORMAT2W cf; ZeroMemory(&cf,sizeof(cf)); cf.cbSize=sizeof(cf);
    cf.dwMask=CFM_COLOR|CFM_FACE|CFM_SIZE; cf.crTextColor=DARK_TXT;
    cf.yHeight=220; lstrcpyW(cf.szFaceName,L"Consolas");
    SendMessageW(h,EM_SETCHARFORMAT,SCF_ALL,(LPARAM)&cf);
    SendMessageW(h,EM_SETCHARFORMAT,SCF_DEFAULT,(LPARAM)&cf);
    SendMessageW(h,EM_AUTOURLDETECT,FALSE,0);
    SendMessageW(h,EM_SETEVENTMASK,0,ENM_CHANGE|ENM_SELCHANGE);
    SetWindowTheme(h,L"DarkMode_Explorer",NULL);
    SendMessageW(h,EM_EXLIMITTEXT,0,(LPARAM)0x7FFFFFFE);
    SendMessageW(h,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(6,4));
}
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_CHAR: case WM_KEYUP: case WM_PASTE: case WM_CUT: case WM_CLEAR: {
        LRESULT r=CallWindowProcW(g_origEdit,h,m,w,l);
        if(g_hlTimer) KillTimer(h,HL_TIMER_ID);
        g_hlTimer=SetTimer(h,HL_TIMER_ID,HL_DEBOUNCE,NULL);
        return r;
    }
    case WM_TIMER:
        if(w==HL_TIMER_ID){ KillTimer(h,HL_TIMER_ID); g_hlTimer=0; do_highlight(h); return 0; }
        break;
    case WM_NCDESTROY: {
        WNDPROC op=g_origEdit; if(op) SetWindowLongPtrW(h,GWLP_WNDPROC,(LONG_PTR)op);
        return op?CallWindowProcW(op,h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    }
    return CallWindowProcW(g_origEdit,h,m,w,l);
}

// ===========================================================================
//  File I/O (we own it -> byte-faithful)
// ===========================================================================
static WCHAR *get_all_text(int *out_len){
    // Returns heap WCHAR with CRLF line endings; caller frees with HeapFree.
    GETTEXTLENGTHEX gtl={GTL_USECRLF|GTL_NUMCHARS,1200};
    LONG n=(LONG)SendMessageW(g_edit,EM_GETTEXTLENGTHEX,(WPARAM)&gtl,0); if(n<0) n=0;
    WCHAR *buf=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(size_t)(n+1)*sizeof(WCHAR));
    if(!buf){ if(out_len)*out_len=0; return NULL; }
    GETTEXTEX gt; ZeroMemory(&gt,sizeof(gt));
    gt.cb=(DWORD)((n+1)*sizeof(WCHAR)); gt.flags=GT_USECRLF; gt.codepage=1200;
    LONG got=(LONG)SendMessageW(g_edit,EM_GETTEXTEX,(WPARAM)&gt,(LPARAM)buf);
    if(got<0) got=0; buf[got]=0; if(out_len)*out_len=(int)got;
    return buf;
}
static void set_title(void){
    const WCHAR *name=g_path[0]?g_path:L"Untitled";
    const WCHAR *base=name;
    for(const WCHAR *p=name;*p;p++) if(*p==L'\\'||*p==L'/') base=p+1;
    WCHAR t[MAX_PATH+32];
    wsprintfW(t,L"%s%s - Darkpad", g_dirty?L"*":L"", base);
    SetWindowTextW(g_main,t);   // taskbar label
    if(g_main){ RECT tr; GetClientRect(g_main,&tr); tr.bottom=TITLE_H; InvalidateRect(g_main,&tr,FALSE); }
}
static void load_file(const WCHAR *path){
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE){ MessageBoxW(g_main,L"Could not open the file.",L"Darkpad",MB_OK|MB_ICONERROR); return; }
    DWORD size=GetFileSize(f,NULL);
    BYTE *bytes=(BYTE*)HeapAlloc(GetProcessHeap(),0,size+2);
    DWORD read=0; ReadFile(f,bytes,size,&read,NULL); CloseHandle(f);
    WCHAR *w=NULL; int wlen=0;
    if(read>=3&&bytes[0]==0xEF&&bytes[1]==0xBB&&bytes[2]==0xBF){
        g_enc=ENC_UTF8BOM;
        int need=MultiByteToWideChar(CP_UTF8,0,(char*)bytes+3,read-3,NULL,0);
        w=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(need+1)*sizeof(WCHAR));
        wlen=MultiByteToWideChar(CP_UTF8,0,(char*)bytes+3,read-3,w,need); w[wlen]=0;
    } else if(read>=2&&bytes[0]==0xFF&&bytes[1]==0xFE){
        g_enc=ENC_UTF16LE; wlen=(read-2)/2;
        w=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(wlen+1)*sizeof(WCHAR));
        memcpy(w,bytes+2,wlen*2); w[wlen]=0;
    } else {
        // No BOM: treat as UTF-8 (matches modern Notepad). Falls back cleanly for ASCII.
        g_enc=ENC_UTF8;
        int need=MultiByteToWideChar(CP_UTF8,0,(char*)bytes,read,NULL,0);
        w=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(need+1)*sizeof(WCHAR));
        wlen=MultiByteToWideChar(CP_UTF8,0,(char*)bytes,read,w,need); w[wlen]=0;
    }
    HeapFree(GetProcessHeap(),0,bytes);
    SetWindowTextW(g_edit, w?w:L"");
    if(w) HeapFree(GetProcessHeap(),0,w);
    lstrcpynW(g_path,path,MAX_PATH);
    do_highlight(g_edit);
    g_dirty=0; SendMessageW(g_edit,EM_SETMODIFY,FALSE,0); set_title();
}
static int write_file(const WCHAR *path){
    int wlen=0; WCHAR *w=get_all_text(&wlen); if(!w) return 0;
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE){ HeapFree(GetProcessHeap(),0,w); MessageBoxW(g_main,L"Could not save the file.",L"Darkpad",MB_OK|MB_ICONERROR); return 0; }
    DWORD wrote;
    if(g_enc==ENC_UTF16LE){
        BYTE bom[2]={0xFF,0xFE}; WriteFile(f,bom,2,&wrote,NULL);
        WriteFile(f,w,wlen*2,&wrote,NULL);
    } else {
        int need=WideCharToMultiByte(CP_UTF8,0,w,wlen,NULL,0,NULL,NULL);
        char *mb=(char*)HeapAlloc(GetProcessHeap(),0,need+1);
        WideCharToMultiByte(CP_UTF8,0,w,wlen,mb,need,NULL,NULL);
        if(g_enc==ENC_UTF8BOM){ BYTE bom[3]={0xEF,0xBB,0xBF}; WriteFile(f,bom,3,&wrote,NULL); }
        WriteFile(f,mb,need,&wrote,NULL);
        HeapFree(GetProcessHeap(),0,mb);
    }
    CloseHandle(f);
    HeapFree(GetProcessHeap(),0,w);
    g_dirty=0; SendMessageW(g_edit,EM_SETMODIFY,FALSE,0); set_title();
    return 1;
}
static int do_saveas(void){
    WCHAR path[MAX_PATH]=L""; if(g_path[0]) lstrcpynW(path,g_path,MAX_PATH);
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn)); ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=g_main; ofn.lpstrFilter=L"Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.lpstrDefExt=L"txt";
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if(!GetSaveFileNameW(&ofn)) return 0;
    lstrcpynW(g_path,path,MAX_PATH);
    return write_file(g_path);
}
static int do_save(void){ return g_path[0]?write_file(g_path):do_saveas(); }
static void do_open(void){
    WCHAR path[MAX_PATH]=L"";
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn)); ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=g_main; ofn.lpstrFilter=L"Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    if(GetOpenFileNameW(&ofn)) load_file(path);
}

// ===========================================================================
//  Status bar Ln/Col
// ===========================================================================
static void update_status(void){
    if(!g_status) return;
    CHARRANGE cr; SendMessageW(g_edit,EM_EXGETSEL,0,(LPARAM)&cr);
    int line=(int)SendMessageW(g_edit,EM_EXLINEFROMCHAR,0,(LPARAM)cr.cpMin);
    int lineStart=(int)SendMessageW(g_edit,EM_LINEINDEX,(WPARAM)line,0);
    int col=cr.cpMin-lineStart;
    WCHAR s[64];
    wsprintfW(s,L"Ln %d, Col %d",line+1,col+1); SendMessageW(g_status,SB_SETTEXTW,0,(LPARAM)s);
    int zn=0,zd=0; SendMessageW(g_edit,EM_GETZOOM,(WPARAM)&zn,(LPARAM)&zd);
    int zp=(zd!=0)?(zn*100/zd):100; if(zp<=0) zp=100;
    WCHAR zs[16]; wsprintfW(zs,L"%d%%",zp); SendMessageW(g_status,SB_SETTEXTW,1,(LPARAM)zs);
    SendMessageW(g_status,SB_SETTEXTW,2,(LPARAM)L"Windows (CRLF)");
    SendMessageW(g_status,SB_SETTEXTW,3,(LPARAM)(g_enc==ENC_UTF16LE?L"UTF-16 LE":(g_enc==ENC_UTF8BOM?L"UTF-8 with BOM":L"UTF-8")));
}
static void layout(void){
    RECT rc; GetClientRect(g_main,&rc);
    SendMessageW(g_status,WM_SIZE,0,0);
    RECT sr; GetWindowRect(g_status,&sr);
    int sh=sr.bottom-sr.top;
    // status parts
    int w=rc.right; int parts[4]={w-360,w-300,w-150,w};
    SendMessageW(g_status,SB_SETPARTS,4,(LPARAM)parts);
    int top=TITLE_H+MENU_H;   // reserve the custom title bar + menu row
    MoveWindow(g_edit,0,top,rc.right,rc.bottom-sh-top,TRUE);
    update_status();
}

// ===========================================================================
//  Zoom / Find / Replace / Font
// ===========================================================================
static void adjust_zoom(int dir){
    int zn=0,zd=0; SendMessageW(g_edit,EM_GETZOOM,(WPARAM)&zn,(LPARAM)&zd);
    int pct=(zd!=0)?(zn*100/zd):100;
    pct += dir*10; if(pct<10)pct=10; if(pct>500)pct=500;
    SendMessageW(g_edit,EM_SETZOOM,pct,100);
    update_status();
}
// Select the next match of fr->lpstrFindWhat, honouring the dialog's flags.
static void find_next(FINDREPLACEW *fr){
    CHARRANGE sel; SendMessageW(g_edit,EM_EXGETSEL,0,(LPARAM)&sel);
    FINDTEXTEXW ft; ZeroMemory(&ft,sizeof(ft));
    if(fr->Flags&FR_DOWN){ ft.chrg.cpMin=sel.cpMax; ft.chrg.cpMax=-1; }  // forward: to end
    else                 { ft.chrg.cpMin=sel.cpMin; ft.chrg.cpMax=0;  }  // backward: to start
    ft.lpstrText=fr->lpstrFindWhat;
    DWORD ff=0;
    if(fr->Flags&FR_DOWN)      ff|=FR_DOWN;
    if(fr->Flags&FR_MATCHCASE) ff|=FR_MATCHCASE;
    if(fr->Flags&FR_WHOLEWORD) ff|=FR_WHOLEWORD;
    LONG pos=(LONG)SendMessageW(g_edit,EM_FINDTEXTEXW,ff,(LPARAM)&ft);
    if(pos<0){ MessageBoxW(g_main,L"Cannot find the text.",L"Darkpad",MB_OK|MB_ICONINFORMATION); return; }
    SendMessageW(g_edit,EM_EXSETSEL,0,(LPARAM)&ft.chrgText);
    SendMessageW(g_edit,EM_SCROLLCARET,0,0);
}
// The FINDMSGSTRING message from the modeless Find/Replace dialog.
static void handle_findreplace(FINDREPLACEW *fr){
    if(fr->Flags&FR_DIALOGTERM){ g_findDlg=NULL; return; }
    if(fr->Flags&FR_FINDNEXT){ find_next(fr); return; }
    if(fr->Flags&FR_REPLACE){
        SendMessageW(g_edit,EM_REPLACESEL,TRUE,(LPARAM)fr->lpstrReplaceWith);
        find_next(fr);
        return;
    }
    if(fr->Flags&FR_REPLACEALL){
        CHARRANGE top={0,0}; SendMessageW(g_edit,EM_EXSETSEL,0,(LPARAM)&top);
        int count=0;
        for(;;){
            CHARRANGE sel; SendMessageW(g_edit,EM_EXGETSEL,0,(LPARAM)&sel);
            FINDTEXTEXW ft; ZeroMemory(&ft,sizeof(ft));
            ft.chrg.cpMin=sel.cpMax; ft.chrg.cpMax=-1; ft.lpstrText=fr->lpstrFindWhat;
            DWORD ff=FR_DOWN;
            if(fr->Flags&FR_MATCHCASE) ff|=FR_MATCHCASE;
            if(fr->Flags&FR_WHOLEWORD) ff|=FR_WHOLEWORD;
            LONG pos=(LONG)SendMessageW(g_edit,EM_FINDTEXTEXW,ff,(LPARAM)&ft);
            if(pos<0) break;
            SendMessageW(g_edit,EM_EXSETSEL,0,(LPARAM)&ft.chrgText);
            SendMessageW(g_edit,EM_REPLACESEL,TRUE,(LPARAM)fr->lpstrReplaceWith);
            if(++count>200000) break;   // safety
        }
        do_highlight(g_edit);
        if(count){ g_dirty=1; set_title(); update_status(); }
        return;
    }
}
// Format > Font: pick a face/size, apply to all text (colours are preserved).
static void choose_font(void){
    CHOOSEFONTW cf; ZeroMemory(&cf,sizeof(cf)); cf.lStructSize=sizeof(cf);
    LOGFONTW lf; ZeroMemory(&lf,sizeof(lf));
    CHARFORMAT2W cur; ZeroMemory(&cur,sizeof(cur)); cur.cbSize=sizeof(cur);
    cur.dwMask=CFM_FACE|CFM_SIZE;
    SendMessageW(g_edit,EM_GETCHARFORMAT,SCF_DEFAULT,(LPARAM)&cur);
    lstrcpynW(lf.lfFaceName,cur.szFaceName,32);
    lf.lfHeight=-(int)(cur.yHeight/15);   // twips -> ~px at 96 dpi
    cf.hwndOwner=g_main; cf.lpLogFont=&lf;
    cf.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT|CF_NOSCRIPTSEL;
    if(!ChooseFontW(&cf)) return;
    CHARFORMAT2W nc; ZeroMemory(&nc,sizeof(nc)); nc.cbSize=sizeof(nc);
    nc.dwMask=CFM_FACE|CFM_SIZE|CFM_BOLD|CFM_ITALIC;
    lstrcpynW(nc.szFaceName,lf.lfFaceName,32);
    nc.yHeight=cf.iPointSize*2;            // iPointSize = 1/10 pt; yHeight = twips (1pt = 20 twips)
    if(lf.lfWeight>=FW_BOLD) nc.dwEffects|=CFE_BOLD;
    if(lf.lfItalic)          nc.dwEffects|=CFE_ITALIC;
    SendMessageW(g_edit,EM_SETCHARFORMAT,SCF_ALL,(LPARAM)&nc);
    SendMessageW(g_edit,EM_SETCHARFORMAT,SCF_DEFAULT,(LPARAM)&nc);
    do_highlight(g_edit);
}

// ===========================================================================
//  Main window
// ===========================================================================
// ---- custom chrome: animated title bar + our own dark menu row ------------
static COLORREF inverse(COLORREF c){ return RGB(255-GetRValue(c),255-GetGValue(c),255-GetBValue(c)); }

// The "sway": the title bar colour eases smoothly between a subtle accent and
// the editor background, and back, on a slow cycle. Driven by real time so it
// keeps moving on its own.
static COLORREF title_color(void){
    DWORD t=GetTickCount();
    double ph=(double)(t%9000)/9000.0*6.2831853;   // one full cycle ~9s
    double e=(1.0-cos(ph))*0.5;                     // 0..1..0, cosine-eased
    COLORREF a=RGB(46,58,86);                        // subtle blue accent
    COLORREF b=DARK_BG;                              // drifts toward the background
    int r =(int)(GetRValue(a)+(GetRValue(b)-GetRValue(a))*e);
    int g =(int)(GetGValue(a)+(GetGValue(b)-GetGValue(a))*e);
    int bl=(int)(GetBValue(a)+(GetBValue(b)-GetBValue(a))*e);
    return RGB(r,g,bl);
}

static const WCHAR *const MENU_LABELS[4]={L"File",L"Edit",L"Format",L"Help"};
static void menu_rects(HWND h, RECT out[4]){
    HDC dc=GetDC(h); HGDIOBJ o=SelectObject(dc,g_menuFont);
    int x=10;
    for(int i=0;i<4;i++){
        SIZE s; GetTextExtentPoint32W(dc,MENU_LABELS[i],lstrlenW(MENU_LABELS[i]),&s);
        out[i].left=x; out[i].right=x+s.cx+18; out[i].top=TITLE_H; out[i].bottom=TITLE_H+MENU_H;
        x=out[i].right;
    }
    SelectObject(dc,o); ReleaseDC(h,dc);
}
static void button_rects(HWND h, RECT out[3]){
    RECT rc; GetClientRect(h,&rc);
    for(int i=0;i<3;i++){ out[i].top=0; out[i].bottom=TITLE_H;
        out[i].right=rc.right-(2-i)*BTN_W; out[i].left=out[i].right-BTN_W; }
}
static void build_popups(void){
    g_mFile=CreatePopupMenu();
    AppendMenuW(g_mFile,MF_STRING,IDM_NEW,L"New\tCtrl+N");
    AppendMenuW(g_mFile,MF_STRING,IDM_OPEN,L"Open...\tCtrl+O");
    AppendMenuW(g_mFile,MF_STRING,IDM_SAVE,L"Save\tCtrl+S");
    AppendMenuW(g_mFile,MF_STRING,IDM_SAVEAS,L"Save As...");
    AppendMenuW(g_mFile,MF_SEPARATOR,0,NULL);
    AppendMenuW(g_mFile,MF_STRING,IDM_EXIT,L"Exit");
    g_mEdit=CreatePopupMenu();
    AppendMenuW(g_mEdit,MF_STRING,IDM_UNDO,L"Undo\tCtrl+Z");
    AppendMenuW(g_mEdit,MF_SEPARATOR,0,NULL);
    AppendMenuW(g_mEdit,MF_STRING,IDM_CUT,L"Cut\tCtrl+X");
    AppendMenuW(g_mEdit,MF_STRING,IDM_COPY,L"Copy\tCtrl+C");
    AppendMenuW(g_mEdit,MF_STRING,IDM_PASTE,L"Paste\tCtrl+V");
    AppendMenuW(g_mEdit,MF_STRING,IDM_DELETE,L"Delete\tDel");
    AppendMenuW(g_mEdit,MF_SEPARATOR,0,NULL);
    AppendMenuW(g_mEdit,MF_STRING,IDM_FIND,L"Find...\tCtrl+F");
    AppendMenuW(g_mEdit,MF_STRING,IDM_REPLACE,L"Replace...\tCtrl+H");
    AppendMenuW(g_mEdit,MF_SEPARATOR,0,NULL);
    AppendMenuW(g_mEdit,MF_STRING,IDM_SELECTALL,L"Select All\tCtrl+A");
    g_mFormat=CreatePopupMenu();
    AppendMenuW(g_mFormat,MF_STRING|MF_CHECKED,IDM_WORDWRAP,L"Word Wrap");
    AppendMenuW(g_mFormat,MF_STRING,IDM_FONT,L"Font...");
    AppendMenuW(g_mFormat,MF_SEPARATOR,0,NULL);
    AppendMenuW(g_mFormat,MF_STRING,IDM_ZOOMIN,L"Zoom In\tCtrl++");
    AppendMenuW(g_mFormat,MF_STRING,IDM_ZOOMOUT,L"Zoom Out\tCtrl+-");
    AppendMenuW(g_mFormat,MF_STRING,IDM_ZOOMRESET,L"Restore Zoom\tCtrl+0");
    g_mHelp=CreatePopupMenu();
    AppendMenuW(g_mHelp,MF_STRING,IDM_ABOUT,L"About Darkpad");
}
// Round the window (aliased on Win10 - the only per-window rounding there).
static void set_region(HWND h){
    if(IsZoomed(h)){ SetWindowRgn(h,NULL,TRUE); return; }
    RECT rc; GetWindowRect(h,&rc);
    SetWindowRgn(h,CreateRoundRectRgn(0,0,rc.right-rc.left+1,rc.bottom-rc.top+1,14,14),TRUE);
}
// Paint the title bar + menu row (the client area not covered by the children).
static void paint_chrome(HWND h, HDC hdc){
    RECT rc; GetClientRect(h,&rc);
    COLORREF tc=title_color();
    RECT title={0,0,rc.right,TITLE_H};
    HBRUSH tb=CreateSolidBrush(tc); FillRect(hdc,&title,tb); DeleteObject(tb);
    RECT menu={0,TITLE_H,rc.right,TITLE_H+MENU_H};
    FillRect(hdc,&menu,bar_brush());
    SetBkMode(hdc,TRANSPARENT);

    // title text: custom font, drawn in the inverse of the drifting bar colour
    COLORREF ink=inverse(tc);
    const WCHAR *nm=g_path[0]?g_path:L"Untitled"; const WCHAR *base=nm;
    for(const WCHAR *p=nm;*p;p++) if(*p==L'\\'||*p==L'/') base=p+1;
    WCHAR ttl[MAX_PATH+40]; wsprintfW(ttl,L"%s%s  -  Darkpad", g_dirty?L"* ":L"", base);
    HGDIOBJ of=SelectObject(hdc,g_titleFont); SetTextColor(hdc,ink);
    RECT tr={14,0,rc.right-3*BTN_W-8,TITLE_H};
    DrawTextW(hdc,ttl,-1,&tr,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS|DT_NOPREFIX);

    // window buttons: min / max / close, glyphs in the inverse colour
    RECT br[3]; button_rects(h,br);
    const WCHAR *glyph[3]={L"–",L"□",L"✕"};
    SelectObject(hdc,g_glyphFont);
    for(int i=0;i<3;i++){
        if(g_btnHover==i){ HBRUSH hb=CreateSolidBrush(i==2?RGB(200,50,50):BAR_HOT); FillRect(hdc,&br[i],hb); DeleteObject(hb); }
        SetTextColor(hdc, (g_btnHover==i&&i==2)?RGB(255,255,255):ink);
        DrawTextW(hdc,glyph[i],-1,&br[i],DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
    }
    // menu labels
    RECT mr[4]; menu_rects(h,mr);
    SelectObject(hdc,g_menuFont);
    for(int i=0;i<4;i++){
        if(g_menuHover==i) FillRect(hdc,&mr[i],hot_brush());
        SetTextColor(hdc,DARK_TXT);
        DrawTextW(hdc,MENU_LABELS[i],-1,&mr[i],DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
    }
    SelectObject(hdc,of);
}
static int confirm_discard(void){
    if(!g_dirty) return 1;
    int r=MessageBoxW(g_main,L"Save changes?",L"Darkpad",MB_YESNOCANCEL|MB_ICONWARNING);
    if(r==IDCANCEL) return 0;
    if(r==IDYES) return do_save();
    return 1;
}
static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    if(g_findMsg && m==g_findMsg){ handle_findreplace((FINDREPLACEW*)l); return 0; }
    switch(m){
    case WM_CREATE: {
        g_status=CreateWindowExW(0,STATUSCLASSNAMEW,NULL,WS_CHILD|WS_VISIBLE,0,0,0,0,h,NULL,g_inst,NULL);
        g_origStatus=(WNDPROC)SetWindowLongPtrW(g_status,GWLP_WNDPROC,(LONG_PTR)status_proc);
        SetWindowTheme(g_status,L"",L"");
        g_edit=CreateWindowExW(0,L"RICHEDIT50W",L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_NOHIDESEL|ES_WANTRETURN,
            0,0,0,0,h,(HMENU)100,g_inst,NULL);
        g_origEdit=(WNDPROC)SetWindowLongPtrW(g_edit,GWLP_WNDPROC,(LONG_PTR)edit_proc);
        apply_edit_visuals(g_edit);
        g_titleFont=CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Bahnschrift");
        g_menuFont =CreateFontW(-14,0,0,0,FW_NORMAL, 0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_glyphFont=CreateFontW(-15,0,0,0,FW_NORMAL, 0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        BOOL dark=TRUE; DwmSetWindowAttribute(h,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        MARGINS mg={0,0,1,0}; DwmExtendFrameIntoClientArea(h,&mg);   // keep a drop shadow
        if(OPACITY<100){
            SetWindowLongPtrW(h,GWL_EXSTYLE,GetWindowLongPtrW(h,GWL_EXSTYLE)|WS_EX_LAYERED);
            SetLayeredWindowAttributes(h,0,(BYTE)(OPACITY*255/100),LWA_ALPHA);
        }
        SetTimer(h,ANIM_TIMER,33,NULL);   // ~30fps sway
        DragAcceptFiles(h,TRUE);          // open files dropped onto the window
        return 0;
    }
    case WM_NCCALCSIZE:
        if(w){   // reclaim the standard caption into the client area
            NCCALCSIZE_PARAMS *p=(NCCALCSIZE_PARAMS*)l;
            WINDOWPLACEMENT wp={sizeof(wp)}; GetWindowPlacement(h,&wp);
            if(wp.showCmd==SW_SHOWMAXIMIZED){   // inset by the frame so nothing is clipped
                int fx=GetSystemMetrics(SM_CXFRAME)+GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy=GetSystemMetrics(SM_CYFRAME)+GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left+=fx; p->rgrc[0].right-=fx; p->rgrc[0].top+=fy; p->rgrc[0].bottom-=fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h,&pt);
        RECT rc; GetClientRect(h,&rc);
        int mrg=6, x=pt.x, y=pt.y;
        if(!IsZoomed(h)){
            int L=x<mrg, R=x>=rc.right-mrg, T=y<mrg, B=y>=rc.bottom-mrg;
            if(T&&L)return HTTOPLEFT; if(T&&R)return HTTOPRIGHT;
            if(B&&L)return HTBOTTOMLEFT; if(B&&R)return HTBOTTOMRIGHT;
            if(L)return HTLEFT; if(R)return HTRIGHT; if(T)return HTTOP; if(B)return HTBOTTOM;
        }
        if(y<TITLE_H && x>=rc.right-3*BTN_W) return HTCLIENT;   // window buttons: we handle
        if(y<TITLE_H) return HTCAPTION;                          // drag the title bar
        return HTCLIENT;
    }
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h,&rc); FillRect((HDC)w,&rc,bar_brush()); return 1; }
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc=BeginPaint(h,&ps); paint_chrome(h,hdc); EndPaint(h,&ps); return 0; }
    case WM_TIMER:
        if(w==ANIM_TIMER){ RECT tr; GetClientRect(h,&tr); tr.bottom=TITLE_H; InvalidateRect(h,&tr,FALSE); return 0; }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        RECT br[3]; button_rects(h,br);
        for(int i=0;i<3;i++) if(PtInRect(&br[i],pt)){
            if(i==0) ShowWindow(h,SW_MINIMIZE);
            else if(i==1) ShowWindow(h,IsZoomed(h)?SW_RESTORE:SW_MAXIMIZE);
            else SendMessageW(h,WM_CLOSE,0,0);
            return 0;
        }
        RECT mr[4]; menu_rects(h,mr);
        for(int i=0;i<4;i++) if(PtInRect(&mr[i],pt)){
            HMENU pm=i==0?g_mFile:i==1?g_mEdit:i==2?g_mFormat:g_mHelp;
            POINT sp={mr[i].left,mr[i].bottom}; ClientToScreen(h,&sp);
            TrackPopupMenu(pm,TPM_LEFTALIGN|TPM_TOPALIGN|TPM_LEFTBUTTON,sp.x,sp.y,0,h,NULL);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        int bh=-1,mh=-1;
        RECT br[3]; button_rects(h,br); for(int i=0;i<3;i++) if(PtInRect(&br[i],pt)) bh=i;
        RECT mr[4]; menu_rects(h,mr);   for(int i=0;i<4;i++) if(PtInRect(&mr[i],pt)) mh=i;
        if(bh!=g_btnHover||mh!=g_menuHover){
            g_btnHover=bh; g_menuHover=mh;
            RECT tr; GetClientRect(h,&tr); tr.bottom=TITLE_H+MENU_H; InvalidateRect(h,&tr,FALSE);
            TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,h,0}; TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if(g_btnHover!=-1||g_menuHover!=-1){ g_btnHover=-1; g_menuHover=-1;
            RECT tr; GetClientRect(h,&tr); tr.bottom=TITLE_H+MENU_H; InvalidateRect(h,&tr,FALSE); }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi=(MINMAXINFO*)l;
        MONITORINFO mi={sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&mi);
        mmi->ptMaxPosition.x=mi.rcWork.left-mi.rcMonitor.left;
        mmi->ptMaxPosition.y=mi.rcWork.top-mi.rcMonitor.top;
        mmi->ptMaxSize.x=mi.rcWork.right-mi.rcWork.left;
        mmi->ptMaxSize.y=mi.rcWork.bottom-mi.rcWork.top;
        mmi->ptMinTrackSize.x=420; mmi->ptMinTrackSize.y=280;
        return 0;
    }
    case WM_SIZE: layout(); set_region(h); return 0;
    case WM_SETFOCUS: SetFocus(g_edit); return 0;
    case WM_DROPFILES: {
        HDROP hd=(HDROP)w; WCHAR p[MAX_PATH];
        if(DragQueryFileW(hd,0,p,MAX_PATH)){ if(confirm_discard()) load_file(p); }
        DragFinish(hd); return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(w);
        if(HIWORD(w)==EN_CHANGE && (HWND)l==g_edit){
            if(g_inHighlight) return 0;
            if(!SendMessageW(g_edit,EM_GETMODIFY,0,0)) return 0;  // control isn't modified (e.g. just loaded) -> not dirty
            if(!g_dirty){ g_dirty=1; set_title(); } update_status(); return 0;
        }
        if(HIWORD(w)==EN_SELCHANGE){ update_status(); return 0; }
        switch(id){
        case IDM_NEW: if(confirm_discard()){ SetWindowTextW(g_edit,L""); g_path[0]=0; g_enc=ENC_UTF8; g_dirty=0; set_title(); } return 0;
        case IDM_OPEN: if(confirm_discard()) do_open(); return 0;
        case IDM_SAVE: do_save(); return 0;
        case IDM_SAVEAS: do_saveas(); return 0;
        case IDM_EXIT: SendMessageW(h,WM_CLOSE,0,0); return 0;
        case IDM_UNDO: SendMessageW(g_edit,EM_UNDO,0,0); return 0;
        case IDM_CUT: SendMessageW(g_edit,WM_CUT,0,0); return 0;
        case IDM_COPY: SendMessageW(g_edit,WM_COPY,0,0); return 0;
        case IDM_PASTE: SendMessageW(g_edit,WM_PASTE,0,0); return 0;
        case IDM_DELETE: SendMessageW(g_edit,WM_CLEAR,0,0); return 0;
        case IDM_SELECTALL: { CHARRANGE cr={0,-1}; SendMessageW(g_edit,EM_EXSETSEL,0,(LPARAM)&cr); return 0; }
        case IDM_WORDWRAP:
            g_wordwrap=!g_wordwrap;
            CheckMenuItem(g_mFormat,IDM_WORDWRAP,g_wordwrap?MF_CHECKED:MF_UNCHECKED);
            SendMessageW(g_edit,EM_SETTARGETDEVICE,0,g_wordwrap?0:1);   // 0=wrap to window, 1=no wrap
            return 0;
        case IDM_FONT: choose_font(); return 0;
        case IDM_ZOOMIN: adjust_zoom(+1); return 0;
        case IDM_ZOOMOUT: adjust_zoom(-1); return 0;
        case IDM_ZOOMRESET: SendMessageW(g_edit,EM_SETZOOM,0,0); update_status(); return 0;
        case IDM_FIND:
        case IDM_REPLACE:
            if(g_findDlg){ SetFocus(g_findDlg); return 0; }
            ZeroMemory(&g_fr,sizeof(g_fr)); g_fr.lStructSize=sizeof(g_fr);
            g_fr.hwndOwner=h; g_fr.Flags=FR_DOWN;
            g_fr.lpstrFindWhat=g_findBuf; g_fr.wFindWhatLen=128;
            g_fr.lpstrReplaceWith=g_replBuf; g_fr.wReplaceWithLen=128;
            g_findDlg=(id==IDM_FIND)?FindTextW(&g_fr):ReplaceTextW(&g_fr);
            return 0;
        case IDM_ABOUT: MessageBoxW(h,L"Darkpad - a dark, syntax-highlighting Notepad.\nYour text, your colors, your tint.",L"About Darkpad",MB_OK|MB_ICONINFORMATION); return 0;
        }
        return 0;
    }
    case WM_CLOSE: if(confirm_discard()) DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h,ANIM_TIMER); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show){
    (void)prev;
    g_inst=inst;
    LoadLibraryW(L"Msftedit.dll");
    InitCommonControls();
    load_config();
    enable_dark_app_mode();
    build_popups();
    g_findMsg=RegisterWindowMessageW(FINDMSGSTRINGW);

    WNDCLASSEXW wc; ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc);
    wc.lpfnWndProc=main_proc; wc.hInstance=inst; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName=L"DarkpadWindow";
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
    ATOM atom=RegisterClassExW(&wc);
    dbg("=== wWinMain start regclass atom=%u err=%lu ===",(unsigned)atom,GetLastError());

    g_main=CreateWindowExW(0,L"DarkpadWindow",L"Untitled - Darkpad",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,900,650,NULL,NULL,inst,NULL);
    dbg("g_main=%p err=%lu",g_main,GetLastError());
    if(!g_main) return 1;
    set_title();

    int argc=0; LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    // Headless save-integrity self-test: load <in>, save to <out>, exit. Never
    // shows a window. Exercises the exact load_file/write_file round-trip.
    if(argv && argc>=4 && lstrcmpW(argv[1],L"--selftest")==0){
        load_file(argv[2]);
        lstrcpynW(g_path,argv[3],MAX_PATH);
        write_file(argv[3]);
        LocalFree(argv);
        return 0;
    }

    ShowWindow(g_main,show); UpdateWindow(g_main);
    // Open a file passed on the command line (this is how the launcher hands us
    // the file the user double-clicked).
    if(argv && argc>=2 && argv[1][0]) load_file(argv[1]);
    if(argv) LocalFree(argv);
    update_status();

    // Accelerators: the common Notepad shortcuts.
    ACCEL acc[]={
        {FCONTROL|FVIRTKEY,'N',IDM_NEW},{FCONTROL|FVIRTKEY,'O',IDM_OPEN},
        {FCONTROL|FVIRTKEY,'S',IDM_SAVE},{FCONTROL|FVIRTKEY,'A',IDM_SELECTALL},
        {FCONTROL|FVIRTKEY,'F',IDM_FIND},{FCONTROL|FVIRTKEY,'H',IDM_REPLACE},
        {FCONTROL|FVIRTKEY,VK_OEM_PLUS,IDM_ZOOMIN},{FCONTROL|FVIRTKEY,VK_ADD,IDM_ZOOMIN},
        {FCONTROL|FVIRTKEY,VK_OEM_MINUS,IDM_ZOOMOUT},{FCONTROL|FVIRTKEY,VK_SUBTRACT,IDM_ZOOMOUT},
        {FCONTROL|FVIRTKEY,'0',IDM_ZOOMRESET},
    };
    HACCEL ha=CreateAcceleratorTableW(acc,11);

    MSG msg;
    while(GetMessageW(&msg,NULL,0,0)>0){
        if(g_findDlg && IsDialogMessageW(g_findDlg,&msg)) continue;   // route keys to the Find dialog
        if(!TranslateAcceleratorW(g_main,ha,&msg)){
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
