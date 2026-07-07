// darkpad.dll (FULL) - whole-window dark mode for genuine Windows Notepad.
// Injected before entrypoint by darkpad_launch.exe (GENUINE_DEBUG method).
// On DLL_PROCESS_ATTACH it spawns a worker thread that finds this process's
// top-level "Notepad" window, subclasses it, and paints:
//   - editor bg dark + light text (WM_CTLCOLOREDIT/STATIC)      [was working]
//   - dark title bar (DwmSetWindowAttribute)                    [was working]
//   - dark scrollbars on the EDIT child (DarkMode_Explorer)     [was working]
//   - DARK MENU BAR via undocumented UAH messages               [NEW]
//   - DARK STATUS BAR by subclassing msctls_statusbar32 and     [NEW]
//     painting over each part with its real text                [NEW]
//
// Build (64-bit, MSVC-ABI PE):
//   zig cc -target x86_64-windows-gnu -shared -o darkpad_new.dll darkpad_full.c \
//        -luser32 -lgdi32 -ldwmapi -luxtheme -lcomctl32

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>

// ---- theme colors ---------------------------------------------------------
#define DARK_BG      RGB(30, 30, 30)     // editor bg (matches existing)
#define DARK_TXT     RGB(220, 220, 220)  // all light text
#define BAR_BG       RGB(43, 43, 43)     // menu bar / status bar bg
#define BAR_HOT      RGB(60, 60, 60)     // hot/selected menu item
#define BAR_EDGE     RGB(43, 43, 43)     // 1px underline sliver

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ---- undocumented UAH menu messages --------------------------------------
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU        0x0091
#define WM_UAHDRAWMENUITEM    0x0092
#define WM_UAHINITMENU        0x0090
#define WM_UAHMEASUREMENUITEM 0x0093
#define WM_UAHNCPAINTMENUPOPUP 0x0095
#endif

// UAH structs (not in public headers; layout per win32-darkmode / Notepad++).
typedef union tagUAHMENUITEMMETRICS {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct tagUAHMENUPOPUPMETRICS {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct tagUAHMENU {
    HMENU hmenu;
    HDC   hdc;
    DWORD dwFlags;
} UAHMENU;

typedef struct tagUAHMENUITEM {
    int                 iPosition;
    UAHMENUITEMMETRICS  umim;
    UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

// WM_UAHDRAWMENUITEM payload: a DRAWITEMSTRUCT-like block + UAHMENU + UAHMENUITEM.
typedef struct tagUAHDRAWMENUITEM {
    DRAWITEMSTRUCT dis;   // itemState / hDC / rcItem valid
    UAHMENU        um;
    UAHMENUITEM    umi;
} UAHDRAWMENUITEM;

// ---- globals --------------------------------------------------------------
static HBRUSH  g_darkBrush = NULL;   // editor bg brush RGB(30,30,30)
static HBRUSH  g_barBrush  = NULL;   // bar bg brush RGB(43,43,43)
static HBRUSH  g_hotBrush  = NULL;   // hot item brush RGB(60,60,60)
static WNDPROC g_origProc  = NULL;   // original Notepad WndProc
static WNDPROC g_origStatus = NULL;  // original status bar WndProc
static HWND    g_notepad   = NULL;   // subclassed top-level window
static HWND    g_status    = NULL;   // status bar child (subclassed)

// SetPreferredAppMode enum
enum PreferredAppMode { PAM_Default = 0, PAM_AllowDark = 1, PAM_ForceDark = 2,
                        PAM_ForceLight = 3 };
typedef enum PreferredAppMode (WINAPI *SetPreferredAppMode_t)(enum PreferredAppMode);
typedef void (WINAPI *FlushMenuThemes_t)(void);

static HBRUSH bar_brush(void)  { if (!g_barBrush)  g_barBrush  = CreateSolidBrush(BAR_BG);  return g_barBrush; }
static HBRUSH hot_brush(void)  { if (!g_hotBrush)  g_hotBrush  = CreateSolidBrush(BAR_HOT); return g_hotBrush; }
static HBRUSH dark_brush(void) { if (!g_darkBrush) g_darkBrush = CreateSolidBrush(DARK_BG); return g_darkBrush; }

// Enable dark menus/theming app-wide via uxtheme private ordinals.
static void enable_dark_app_mode(void) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    // ordinal 135 = SetPreferredAppMode (Win10 1903+), ordinal 136 = FlushMenuThemes
    SetPreferredAppMode_t setMode =
        (SetPreferredAppMode_t)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemes_t flush =
        (FlushMenuThemes_t)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(PAM_ForceDark);
    if (flush)   flush();
    // keep uxtheme loaded (do not FreeLibrary) for the process lifetime
}

// ---- status bar: dark via a direct subclass of msctls_statusbar32 ----------
// We subclass the status bar child itself and fully own its painting. On
// WM_PAINT we walk every part, read that part's REAL text with SB_GETTEXTW,
// and draw it ourselves with a light color on a dark fill. This keeps each
// segment's own text (Ln/Col, zoom %, line-ending, encoding) correct and
// distinct -- unlike SBT_OWNERDRAW, which would discard Notepad's text -- and
// it updates live as the caret moves because Notepad keeps calling SB_SETTEXT
// on the control, which invalidates it and re-enters this WM_PAINT.

// Left inset (px) so text is not glued to each part's divider.
#define SB_TEXT_INSET 6

// Paint every status-bar part with its real text on a dark background.
static void status_paint(HWND sb, HDC hdc) {
    RECT client;
    GetClientRect(sb, &client);
    // Fill the whole client first so the size-grip corner and any gaps are dark.
    FillRect(hdc, &client, bar_brush());

    HFONT font = (HFONT)SendMessageW(sb, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : NULL;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, DARK_TXT);

    int parts = (int)SendMessageW(sb, SB_GETPARTS, 0, 0);
    if (parts < 1) parts = 1;
    for (int i = 0; i < parts; i++) {
        RECT rc;
        if (!SendMessageW(sb, SB_GETRECT, (WPARAM)i, (LPARAM)&rc)) continue;
        // Re-fill this part's rect (harmless; keeps dividers clean).
        FillRect(hdc, &rc, bar_brush());

        DWORD lr = (DWORD)SendMessageW(sb, SB_GETTEXTLENGTHW, (WPARAM)i, 0);
        int len = (int)LOWORD(lr);
        if (len <= 0) continue;
        if (len > 511) len = 511;
        WCHAR buf[512];
        buf[0] = 0;
        SendMessageW(sb, SB_GETTEXTW, (WPARAM)i, (LPARAM)buf);
        buf[len] = 0;

        RECT tr = rc;
        tr.left += SB_TEXT_INSET;
        DrawTextW(hdc, buf, len, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    }
    if (oldFont) SelectObject(hdc, oldFont);
}

// Subclass proc for the status bar: dark erase + custom part painting.
static LRESULT CALLBACK status_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        // Paint the whole client dark; report handled so the control's own
        // (light) erase never shows through.
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, bar_brush());
        }
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        status_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCDESTROY: {
        WNDPROC op = g_origStatus;
        if (op) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)op);
        g_origStatus = NULL;
        g_status = NULL;
        return op ? CallWindowProcW(op, hwnd, msg, wp, lp)
                  : DefWindowProcW(hwnd, msg, wp, lp);
    }
    }
    return g_origStatus ? CallWindowProcW(g_origStatus, hwnd, msg, wp, lp)
                        : DefWindowProcW(hwnd, msg, wp, lp);
}

// Subclass the status bar child so we own its painting; force a repaint.
static void hook_status_bar(HWND status) {
    if (!status || status == g_status) return;
    g_status = status;
    g_origStatus = (WNDPROC)SetWindowLongPtrW(status, GWLP_WNDPROC,
                                              (LONG_PTR)status_proc);
    InvalidateRect(status, NULL, TRUE);
}

// ---- menu bar: UAH drawing ------------------------------------------------
// Fill the whole menu bar rect dark.
static LRESULT uah_draw_menu(HWND hwnd, UAHMENU *pum) {
    if (!pum) return 0;
    MENUBARINFO mbi = { sizeof(mbi) };
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return 0;
    RECT wr; GetWindowRect(hwnd, &wr);
    // mbi.rcBar is in screen coords; make it relative to the window origin
    // (the UAH hdc is window-DC based).
    RECT bar = mbi.rcBar;
    OffsetRect(&bar, -wr.left, -wr.top);
    FillRect(pum->hdc, &bar, bar_brush());
    return 0;
}

// Draw one menu-bar item (top-level caption): dark bg (hot if selected) + text.
static LRESULT uah_draw_menuitem(HWND hwnd, UAHDRAWMENUITEM *pmdi) {
    (void)hwnd;
    if (!pmdi) return 0;

    // Fetch the item caption via the menu handle + position.
    WCHAR caption[256]; caption[0] = 0;
    MENUITEMINFOW mii = { sizeof(mii) };
    mii.fMask = MIIM_STRING | MIIM_STATE;
    mii.dwTypeData = caption;
    mii.cch = 255;
    GetMenuItemInfoW(pmdi->um.hmenu, (UINT)pmdi->umi.iPosition, TRUE, &mii);

    UINT state = pmdi->dis.itemState;
    int hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) ? 1 : 0;
    int disabled = (state & (ODS_GRAYED | ODS_DISABLED)) ? 1 : 0;

    FillRect(pmdi->dis.hDC, &pmdi->dis.rcItem, hot ? hot_brush() : bar_brush());

    // Text.
    SetBkMode(pmdi->dis.hDC, TRANSPARENT);
    SetTextColor(pmdi->dis.hDC, disabled ? RGB(120,120,120) : DARK_TXT);

    UINT dtflags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    // Show mnemonic underline only when keyboard cues are active.
    if (state & ODS_NOACCEL) dtflags |= DT_HIDEPREFIX;

    DrawTextW(pmdi->dis.hDC, caption, -1, &pmdi->dis.rcItem, dtflags);
    return 0;
}

// Paint the 1px sliver under the menu bar (the gap the theme leaves light).
static void paint_menu_underline(HWND hwnd) {
    MENUBARINFO mbi = { sizeof(mbi) };
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
    RECT wr; GetWindowRect(hwnd, &wr);
    RECT bar = mbi.rcBar;
    OffsetRect(&bar, -wr.left, -wr.top);
    // sliver just below the bar
    RECT sliver = { bar.left, bar.bottom, bar.right, bar.bottom + 1 };
    HDC hdc = GetWindowDC(hwnd);
    if (hdc) {
        HBRUSH b = CreateSolidBrush(BAR_EDGE);
        FillRect(hdc, &sliver, b);
        DeleteObject(b);
        ReleaseDC(hwnd, hdc);
    }
}

// Apply dark decorations to the top-level window and its children.
static void apply_dark(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    HWND edit = FindWindowExW(hwnd, NULL, L"Edit", NULL);
    if (edit) {
        SetWindowTheme(edit, L"DarkMode_Explorer", NULL);
        SendMessageW(edit, WM_THEMECHANGED, 0, 0);
        InvalidateRect(edit, NULL, TRUE);
    }
    HWND status = FindWindowExW(hwnd, NULL, L"msctls_statusbar32", NULL);
    if (status) {
        // Strip the visual style so the themed control never repaints its own
        // (light) grip/parts under our subclass -- our WM_PAINT fully owns the
        // client, so classic mode just avoids any themed pixels leaking in.
        SetWindowTheme(status, L"", L"");
        hook_status_bar(status);   // subclass the control; we paint the parts
        InvalidateRect(status, NULL, TRUE);
    }
    // repaint whole non-client (menu bar) + client
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, NULL, TRUE);
    DrawMenuBar(hwnd);
}

// Subclass proc: colorize edit, draw dark menu bar + status bar.
static LRESULT CALLBACK dark_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, DARK_BG);
        SetTextColor(dc, DARK_TXT);
        SetDCBrushColor(dc, DARK_BG);
        return (LRESULT)dark_brush();
    }

    case WM_UAHDRAWMENU:
        return uah_draw_menu(hwnd, (UAHMENU *)lp);

    case WM_UAHDRAWMENUITEM:
        return uah_draw_menuitem(hwnd, (UAHDRAWMENUITEM *)lp);

    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        // Let default draw the frame, then patch the menu-bar underline sliver.
        LRESULT r = g_origProc ? CallWindowProcW(g_origProc, hwnd, msg, wp, lp)
                               : DefWindowProcW(hwnd, msg, wp, lp);
        paint_menu_underline(hwnd);
        return r;
    }

    case WM_NCDESTROY: {
        if (g_origProc)
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
        LRESULT r = g_origProc
            ? CallWindowProcW(g_origProc, hwnd, msg, wp, lp)
            : DefWindowProcW(hwnd, msg, wp, lp);
        if (g_darkBrush) { DeleteObject(g_darkBrush); g_darkBrush = NULL; }
        if (g_barBrush)  { DeleteObject(g_barBrush);  g_barBrush  = NULL; }
        if (g_hotBrush)  { DeleteObject(g_hotBrush);  g_hotBrush  = NULL; }
        return r;
    }
    }
    return g_origProc ? CallWindowProcW(g_origProc, hwnd, msg, wp, lp)
                      : DefWindowProcW(hwnd, msg, wp, lp);
}

// Worker: wait for our Notepad window, then hook it.
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    enable_dark_app_mode();  // before first menu is themed

    DWORD myPid = GetCurrentProcessId();
    HWND found = NULL;

    for (int i = 0; i < 150 && !found; i++) {
        HWND h = NULL;
        while ((h = FindWindowExW(NULL, h, L"Notepad", NULL)) != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == myPid) { found = h; break; }
        }
        if (!found) Sleep(20);
    }
    if (!found) return 0;

    g_notepad = found;
    g_origProc = (WNDPROC)SetWindowLongPtrW(found, GWLP_WNDPROC,
                                            (LONG_PTR)dark_proc);
    apply_dark(found);
    for (int i = 0; i < 4; i++) { Sleep(70); apply_dark(found); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
