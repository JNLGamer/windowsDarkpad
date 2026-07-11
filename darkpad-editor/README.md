# Darkpad editor

A small, dark, syntax-highlighting text editor for Windows — its **own** program,
not a Notepad mod. Because it owns its own window and file I/O, it does **not**
touch `notepad.exe`, the registry, or your file associations. Nothing system-wide,
nothing to break.

> **Status: experimental.** The core (dark theme, syntax highlighting, and
> byte-faithful save across UTF-8 / UTF-8-BOM / UTF-16 LE with CRLF preserved) is
> tested. The menu and dialog features below are implemented but only lightly
> tested — expect the odd rough edge.

## Features
- Dark editor with generic syntax highlighting (comments, strings, numbers, keywords)
- Byte-faithful open / save — UTF-8, UTF-8 BOM, UTF-16 LE, line endings preserved
- Custom animated title bar, own dark menu row, rounded corners, optional window tint
- Find / Replace, Font picker, Zoom, Word Wrap, drag-and-drop to open
- Config at `%APPDATA%\Darkpad\darkpad.ini` — `[colors]` background/text/bar and
  `[window]` opacity (set `opacity=95` for a faint see-through tint)

## Run
Run `darkpad.exe`, or drop a text file onto it.

## Build
Needs [Zig](https://ziglang.org/) (it ships a C compiler):

```
zig cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows \
    -o darkpad.exe src/darkpad_app.c \
    -luser32 -lgdi32 -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lole32 -lshell32
```

The editor control is a RichEdit (`Msftedit.dll`). It's a single C file.

## License
MIT.
