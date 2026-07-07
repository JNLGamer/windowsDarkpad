// darkpad_launch.exe - IFEO Debugger launcher for genuine Notepad dark mode.
// Method: GENUINE_DEBUG (proven by spike).
//   CreateProcess the REAL System32\notepad.exe with DEBUG_ONLY_THIS_PROCESS,
//   wait for the first CREATE_PROCESS_DEBUG_EVENT (image mapped, pre-entrypoint),
//   inject darkpad.dll via VirtualAllocEx+WriteProcessMemory+CreateRemoteThread
//   (LoadLibraryW), then DebugSetProcessKillOnExit(FALSE) + DebugActiveProcessStop
//   to detach and let notepad run free. DEBUG_ONLY_THIS_PROCESS also suppresses
//   IFEO re-entry, so no fork bomb.
//
// IFEO invokes us as: argv[0]=this.exe  argv[1]=<real image path>  argv[2..]=orig args.
//
// Build (64-bit):
//   zig cc -target x86_64-windows-gnu -o darkpad_launch.exe darkpad_launch.c -luser32

#include <windows.h>
#include <stdio.h>

#define REAL_NOTEPAD "C:\\Windows\\System32\\notepad.exe"
#define DLL_NAME L"darkpad.dll"

// Build the full path to darkpad.dll sitting next to THIS launcher executable,
// so the pair is relocatable (installable to any folder). Result is written to
// 'out' (wide, NUL-terminated). Returns the char count, or 0 on failure.
static DWORD dll_path_beside_exe(wchar_t *out, DWORD cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    if (n == 0 || n >= cap) return 0;  // failed or truncated
    // Strip the executable filename, keeping the trailing backslash.
    wchar_t *slash = out;
    for (wchar_t *p = out; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    slash[1] = 0;  // cut right after the last separator
    // Append the DLL name; guard against overflow.
    size_t used = (size_t)(slash + 1 - out);
    size_t need = used + (sizeof(DLL_NAME) / sizeof(wchar_t)); // incl. NUL
    if (need > cap) return 0;
    for (const wchar_t *s = DLL_NAME; ; s++) {
        out[used++] = *s;
        if (*s == 0) break;
    }
    return (DWORD)(used - 1);  // chars excluding NUL
}

// Inject darkpad.dll (found beside this launcher) into the debuggee (image
// already mapped) via LoadLibraryW.
static void inject(HANDLE hProc) {
    wchar_t wpath[MAX_PATH];
    if (dll_path_beside_exe(wpath, MAX_PATH) == 0) return;
    // bytes = full wide string including the NUL terminator.
    SIZE_T bytes = (wcslen(wpath) + 1) * sizeof(wchar_t);

    LPVOID remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!remote) return;
    if (!WriteProcessMemory(hProc, remote, wpath, bytes, NULL)) return;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadlib) return;

    HANDLE th = CreateRemoteThread(hProc, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)loadlib, remote,
                                   0, NULL);
    if (th) {
        // let the DLL land before we detach (its own worker polls for the window)
        WaitForSingleObject(th, 4000);
        CloseHandle(th);
    }
    // leave 'remote' allocated; process manages its own lifetime after detach
}

int main(int argc, char** argv) {
    // Rebuild notepad's command line: real image path + original args, each quoted.
    char cmd[32768];
    int n = snprintf(cmd, sizeof(cmd), "\"%s\"",
                     (argc >= 2) ? argv[1] : REAL_NOTEPAD);
    for (int i = 2; i < argc && n > 0 && n < (int)sizeof(cmd); i++)
        n += snprintf(cmd + n, sizeof(cmd) - n, " \"%s\"", argv[i]);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi))
        return 1;

    DEBUG_EVENT de;
    for (int ev = 0; ev < 64; ev++) {
        if (!WaitForDebugEvent(&de, 5000)) break;
        if (de.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (de.u.CreateProcessInfo.hFile)
                CloseHandle(de.u.CreateProcessInfo.hFile);
            // Detach FIRST so the debuggee resumes and can run our remote thread.
            // (While attached and stopped at this event, the debuggee's threads are
            //  frozen, so CreateRemoteThread would never execute.) We keep pi.hProcess
            //  open across the detach, so injection still targets the live process.
            DebugSetProcessKillOnExit(FALSE);       // keep notepad alive on detach
            DebugActiveProcessStop(pi.dwProcessId); // detach -> notepad runs free
            inject(pi.hProcess);                    // now the remote thread can run
            break;
        }
        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
