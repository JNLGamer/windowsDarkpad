@echo off
REM Build Darkpad from source. Needs Zig (ships a C compiler) and Python 3 on PATH.
REM Produces darkpad.dll, darkpad_launch.exe and the self-contained install.exe.

setlocal
cd /d "%~dp0"

echo [1/4] Building darkpad.dll (the dark-mode payload)...
zig cc -target x86_64-windows-gnu -O2 -s -shared -o darkpad.dll src\darkpad.c ^
    -luser32 -lgdi32 -ldwmapi -luxtheme -lcomctl32 || goto :err

echo [2/4] Building darkpad_launch.exe (the injector)...
zig cc -target x86_64-windows-gnu -O2 -s -fno-unwind-tables -o darkpad_launch.exe src\darkpad_launch.c ^
    -luser32 || goto :err

echo [3/4] Embedding payloads into byte-array headers...
python src\gen_header.py darkpad.dll        DLL    src\dll_bytes.h    || goto :err
python src\gen_header.py darkpad_launch.exe LAUNCH src\launch_bytes.h || goto :err

echo [4/4] Building install.exe (self-contained installer)...
zig cc -target x86_64-windows-gnu -O2 -s -municode -I src -o install.exe src\install.c ^
    -luser32 -lshell32 -ladvapi32 -lole32 || goto :err

echo.
echo Done. Run install.exe to install, or install.exe /uninstall to remove.
goto :eof

:err
echo.
echo Build failed.
exit /b 1
