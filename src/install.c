/*
 * Darkpad installer.
 *
 * Single self-contained executable that installs a dark theme for the
 * Windows Notepad by placing two payload files on disk and registering an
 * Image File Execution Options (IFEO) debugger launcher for notepad.exe.
 *
 * Usage:
 *   install.exe               Install (self-elevates to admin).
 *   install.exe /uninstall    Remove the install (self-elevates to admin).
 *   install.exe /verify       Print embedded payload lengths and exit (no admin,
 *                             no filesystem or registry writes).
 *   install.exe /regtest      Round-trip a throwaway value under HKCU and exit
 *                             (no admin, does not touch HKLM or notepad).
 *
 * The two payloads are compiled directly into this binary as byte arrays
 * (see dll_bytes.h and launch_bytes.h).
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>

#include "dll_bytes.h"     /* DLL_BYTES,    DLL_LEN    */
#include "launch_bytes.h"  /* LAUNCH_BYTES, LAUNCH_LEN */

/* Registry location of the IFEO entry for notepad.exe. */
static const wchar_t *IFEO_KEY =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\notepad.exe";

static const wchar_t *APP_TITLE = L"Darkpad";

/* ---- helpers ----------------------------------------------------------- */

/* Report a failure with the specific Win32 error, then return 1. */
static int fail(const wchar_t *what, DWORD err)
{
    wchar_t msg[512];
    wchar_t buf[256];
    buf[0] = L'\0';
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), NULL);
    _snwprintf(msg, sizeof(msg) / sizeof(msg[0]),
               L"%ls\nError %lu: %ls", what, (unsigned long)err, buf);
    MessageBoxW(NULL, msg, APP_TITLE, MB_OK | MB_ICONERROR);
    return 1;
}

/* Return TRUE if the current process is running elevated (admin). */
static BOOL is_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    BOOL elevated = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated ? TRUE : FALSE;
        }
        CloseHandle(token);
    }
    return elevated;
}

/*
 * Re-launch this same executable elevated via the "runas" verb, forwarding
 * the given arguments. Returns TRUE if the elevated process was launched
 * (the caller should then exit).
 */
static BOOL relaunch_elevated(const wchar_t *args)
{
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0)
        return FALSE;
    HINSTANCE r = ShellExecuteW(NULL, L"runas", self, args, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32);
}

/* Resolve %ProgramFiles%; fall back to CSIDL_PROGRAM_FILES. */
static BOOL get_program_files(wchar_t *out, DWORD cch)
{
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", out, cch);
    if (n > 0 && n < cch)
        return TRUE;
    return SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL,
                                      SHGFP_TYPE_CURRENT, out));
}

/* Write a buffer to a file, overwriting any existing file. */
static BOOL write_file(const wchar_t *path, const unsigned char *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, NULL) && (written == len);
    CloseHandle(h);
    return ok;
}

/* ---- install ----------------------------------------------------------- */

static int do_install(void)
{
    wchar_t progfiles[MAX_PATH];
    wchar_t instdir[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    wchar_t exe_path[MAX_PATH];
    wchar_t debugger[MAX_PATH + 4];

    if (!get_program_files(progfiles, MAX_PATH))
        return fail(L"Could not resolve the Program Files folder.", GetLastError());

    _snwprintf(instdir, MAX_PATH, L"%ls\\Darkpad", progfiles);

    /* Create the install directory (ok if it already exists). */
    if (!CreateDirectoryW(instdir, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
            return fail(L"Could not create the install directory.", e);
    }

    _snwprintf(dll_path, MAX_PATH, L"%ls\\darkpad.dll", instdir);
    _snwprintf(exe_path, MAX_PATH, L"%ls\\darkpad_launch.exe", instdir);

    /* Write the two embedded payloads to disk. */
    if (!write_file(dll_path, DLL_BYTES, DLL_LEN))
        return fail(L"Could not write darkpad.dll.", GetLastError());
    if (!write_file(exe_path, LAUNCH_BYTES, LAUNCH_LEN))
        return fail(L"Could not write darkpad_launch.exe.", GetLastError());

    /*
     * Register the launcher as the IFEO debugger for notepad.exe. The value
     * is the quoted full path to the launcher executable.
     */
    _snwprintf(debugger, MAX_PATH + 4, L"\"%ls\"", exe_path);

    HKEY key = NULL;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, IFEO_KEY, 0, NULL,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                                &key, NULL);
    if (s != ERROR_SUCCESS)
        return fail(L"Could not open the registry key.", (DWORD)s);

    DWORD bytes = (DWORD)((wcslen(debugger) + 1) * sizeof(wchar_t));
    s = RegSetValueExW(key, L"Debugger", 0, REG_SZ,
                       (const BYTE *)debugger, bytes);
    RegCloseKey(key);
    if (s != ERROR_SUCCESS)
        return fail(L"Could not set the Debugger value.", (DWORD)s);

    MessageBoxW(NULL, L"Dark Notepad installed. Open Notepad to see it.",
                APP_TITLE, MB_OK | MB_ICONINFORMATION);
    return 0;
}

/* ---- uninstall --------------------------------------------------------- */

static int do_uninstall(void)
{
    wchar_t progfiles[MAX_PATH];
    wchar_t instdir[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    wchar_t exe_path[MAX_PATH];

    /* Delete the Debugger value; then remove the key if it is now empty. */
    HKEY key = NULL;
    LSTATUS s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, IFEO_KEY, 0,
                              KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (s == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"Debugger");

        /* Count remaining values and subkeys. */
        DWORD subkeys = 0, values = 0;
        RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL,
                         &values, NULL, NULL, NULL, NULL);
        RegCloseKey(key);

        if (subkeys == 0 && values == 0)
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, IFEO_KEY);
    } else if (s != ERROR_FILE_NOT_FOUND) {
        return fail(L"Could not open the registry key.", (DWORD)s);
    }

    /* Remove the payload files and the install directory. */
    if (get_program_files(progfiles, MAX_PATH)) {
        _snwprintf(instdir, MAX_PATH, L"%ls\\Darkpad", progfiles);
        _snwprintf(dll_path, MAX_PATH, L"%ls\\darkpad.dll", instdir);
        _snwprintf(exe_path, MAX_PATH, L"%ls\\darkpad_launch.exe", instdir);
        DeleteFileW(dll_path);
        DeleteFileW(exe_path);
        RemoveDirectoryW(instdir);
    }

    MessageBoxW(NULL, L"Dark Notepad removed.", APP_TITLE,
                MB_OK | MB_ICONINFORMATION);
    return 0;
}

/* ---- self-tests (no admin, non-destructive) ---------------------------- */

/*
 * Print the embedded payload lengths so they can be compared against the
 * source file sizes. Writes nothing to disk or registry.
 */
static int do_verify(void)
{
    /* ASCII output via narrow stdio so it flushes cleanly to files and pipes. */
    printf("DLL_LEN=%u\n", DLL_LEN);
    printf("LAUNCH_LEN=%u\n", LAUNCH_LEN);
    fflush(stdout);
    return 0;
}

/*
 * Prove the Reg*ValueExW code paths work by writing, reading back, and
 * deleting a throwaway REG_SZ value under HKCU. Never touches HKLM or the
 * notepad IFEO key.
 */
static int do_regtest(void)
{
    const wchar_t *sub = L"Software\\DarkpadInstallTest";
    const wchar_t *val = L"Debugger";
    const wchar_t *data = L"\"C:\\dummy\\darkpad_launch.exe\"";

    HKEY key = NULL;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE,
                                NULL, &key, NULL);
    if (s != ERROR_SUCCESS) {
        printf("REGTEST FAIL create=%ld\n", (long)s);
        fflush(stdout);
        return 1;
    }

    DWORD bytes = (DWORD)((wcslen(data) + 1) * sizeof(wchar_t));
    s = RegSetValueExW(key, val, 0, REG_SZ, (const BYTE *)data, bytes);
    if (s != ERROR_SUCCESS) {
        printf("REGTEST FAIL set=%ld\n", (long)s);
        fflush(stdout);
        RegCloseKey(key);
        return 1;
    }

    wchar_t readback[256];
    DWORD rb_bytes = sizeof(readback);
    DWORD type = 0;
    s = RegQueryValueExW(key, val, NULL, &type, (BYTE *)readback, &rb_bytes);
    RegCloseKey(key);
    if (s != ERROR_SUCCESS) {
        printf("REGTEST FAIL query=%ld\n", (long)s);
        fflush(stdout);
        return 1;
    }

    /* Remove the throwaway key entirely. */
    RegDeleteKeyW(HKEY_CURRENT_USER, sub);

    /* Compare the read-back value against what was written. */
    int matched = (wcscmp(readback, data) == 0);
    printf("REGTEST OK type=%lu match=%d readback_len=%u\n",
           (unsigned long)type, matched,
           (unsigned)(rb_bytes / sizeof(wchar_t)));
    fflush(stdout);
    return 0;
}

/* ---- entry ------------------------------------------------------------- */

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *arg = (argc > 1) ? argv[1] : L"";

    /* Non-elevated diagnostics run directly and exit. */
    if (_wcsicmp(arg, L"/verify") == 0)
        return do_verify();
    if (_wcsicmp(arg, L"/regtest") == 0)
        return do_regtest();

    BOOL uninstall = (_wcsicmp(arg, L"/uninstall") == 0);

    /* Both install and uninstall require admin; self-elevate if needed. */
    if (!is_elevated()) {
        if (!relaunch_elevated(uninstall ? L"/uninstall" : L""))
            return fail(L"Administrator rights are required.", GetLastError());
        return 0;
    }

    return uninstall ? do_uninstall() : do_install();
}
