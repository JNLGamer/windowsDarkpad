# Darkpad — a dark mode for the real Windows Notepad

Windows Notepad, but dark mode — so you don't get flashbanged when you
accidentally open a `.txt` file at 2am.

Windows 11 finally gave Notepad a dark mode. Windows 10 never got it. Darkpad
fixes that for the **actual** `notepad.exe` — the real one in
`C:\Windows\System32`, not a replacement and not a clone — so it opens dark
every time, no matter who or what opens it.

<!-- Screenshot goes here: save one as docs/screenshot.png and use ![Darkpad](docs/screenshot.png) -->

## What you get

- Dark editor, light text.
- Dark title bar.
- Dark menu bar and dark menus.
- Dark status bar (line/column, zoom, encoding — all still correct).
- Works on every launch: double-click a `.txt`, open it from another program,
  type `notepad` in Run — all dark.

## Install

1. Download `install.exe`.
2. Run it. Click **Yes** on the admin prompt.
3. Open Notepad.

That's it. To undo it, run `install.exe /uninstall` (or just delete the one
registry value — see below). It puts two small files in
`C:\Program Files\Darkpad` and nothing else.

## Custom colors

Darkpad isn't locked to my grey. The first time you open Notepad after
installing, it drops a little settings file here:

```
%APPDATA%\Darkpad\darkpad.ini
```

Quick way to get there: press **Win + R**, paste `%APPDATA%\Darkpad`, hit Enter.

Open `darkpad.ini` (in Notepad, obviously — it'll be dark) and you'll see:

```
[colors]
background=1e1e1e
text=dcdcdc
bar=2b2b2b
```

Three values, each a hex color like you'd use on a web page (`RRGGBB`):

- **background** — the area behind your text.
- **text** — the text itself.
- **bar** — the menu bar and the status bar.

Change one, save, and **restart Notepad**. That's the whole loop.

A few to steal:

```
; Ink blue
background=10131a
text=cdd6f4
bar=1b2030
```

```
; Warm sepia
background=1c1813
text=e8d8b8
bar=2a2018
```

```
; Max contrast
background=000000
text=ffffff
bar=1a1a1a
```

Delete a line to fall back to that item's default. Delete the whole file and
Darkpad recreates it with the defaults the next time Notepad opens. There's a
copy of this file at [`docs/darkpad.ini`](docs/darkpad.ini) if you want a
starting point.

**The one thing you can't recolor is the title bar.** On Windows 10 the title
bar belongs to Windows — it only does "dark" or "light," not a custom color — so
Darkpad keeps it dark and leaves it out of the file. Background, text and bar are
yours.

## The honest limitation: no syntax highlighting

People ask. The answer is no, and it's not laziness — it's the wall Notepad is
built against.

Notepad's text area is a plain old Win32 **EDIT control**. That control was
designed in the early 90s to hold one blob of text in exactly **one** color.
It has no idea what a "word" is, let alone a keyword. There is no message you
can send it, no flag you can set, to make one word green and another blue. To
get real syntax colors you'd have to rip that control out and bolt in a proper
code-editor engine — and at that point you haven't modded Notepad, you've
written a different program wearing Notepad's title bar. Even the shiny new
Windows 11 Notepad still doesn't highlight code. If you want colors, use a code
editor. Notepad is Notepad.

## How it actually works (for the curious)

I went into this assuming there'd be a clean, supported way to recolor Notepad.
There isn't. So here's the scenic route I ended up taking.

**You can't theme it from the outside.** Notepad paints its own window. The only
way to change how it draws is to run code *inside* Notepad's own process. So
Darkpad is a small DLL that gets loaded into Notepad, and from in there it:

- Answers the `WM_CTLCOLOREDIT` message with a dark brush + light text — that's
  what recolors the editor. This is the one "intended" hook Windows gives you,
  and it's the whole reason the trick is even possible.
- Flips the title bar dark with `DwmSetWindowAttribute` (the immersive dark-mode
  attribute the modern apps use).
- Paints the **menu bar** using the undocumented `WM_UAHDRAWMENU` /
  `WM_UAHDRAWMENUITEM` messages. The classic menu bar is drawn by Windows itself
  and flatly ignores dark mode, so you have to intercept those internal draw
  messages and paint every item by hand. This was the fiddliest part.
- Subclasses the **status bar** and repaints it, reading each segment's real
  text so it stays correct instead of turning into gibberish.
- Themes the scrollbars with `SetWindowTheme(..., "DarkMode_Explorer")`.

**Getting it in, on every launch, was the real puzzle.** How do you make your
DLL load into a program you don't control, automatically, forever?

Windows has a feature called **Image File Execution Options** (IFEO) — a
registry key where you can name a "debugger" for any program. When someone
launches that program, Windows launches your debugger instead and hands it the
real program to run. It's meant for developers. It's also a perfect, permanent
hook.

So Darkpad registers a tiny launcher as Notepad's "debugger." Every Notepad
launch now goes through the launcher first.

**And here's the trap I walked straight into:** the obvious launcher just starts
Notepad and injects the DLL. But starting `notepad.exe` re-triggers the same
IFEO hook... which launches the launcher... which starts `notepad.exe`... which
re-triggers the hook. A fork bomb. Notepad windows breeding until the machine
tips over.

The fix is a genuinely neat piece of Windows trivia: if you start the process as
a **debuggee** (`CreateProcess` with `DEBUG_ONLY_THIS_PROCESS`), Windows *skips*
the IFEO redirect — because it figures a debugger knows what it's doing. So the
launcher starts the genuine Notepad as its debuggee, waits for it to exist,
injects the dark-mode DLL, then quietly **detaches** so Notepad runs like normal
— except now it's dark. No recursion, no fork bomb, and it's the real
`System32\notepad.exe` the entire time.

I tested the fork-bomb case on a throwaway copy first, because I'm not launching
an untested fork bomb on my own machine, thanks.

## Build it yourself

You need [Zig](https://ziglang.org/) (it ships a C compiler) and Python 3.
Then:

```
build.bat
```

That compiles the DLL, the launcher, and the installer from the `src` folder.
It's all plain C, a couple hundred lines each — read it, it's short.

## Files

- `install.exe` — the installer (carries the two files below inside it).
- `darkpad.dll` — the dark-mode code that runs inside Notepad.
- `darkpad_launch.exe` — the launcher that injects it.
- `src/` — full source for all three, plus the header generator.

## Undo / safety

Darkpad changes exactly one thing on your system: it sets a `Debugger` value on

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\notepad.exe
```

and drops two files in `C:\Program Files\Darkpad`. `install.exe /uninstall`
removes both. Delete that registry value by hand and Notepad is instantly back
to normal. It never touches `notepad.exe` itself.

## License

MIT. Do whatever you want with it.
