<p align="center">
  <img src="res/icon.png" width="96" height="96" alt="XPico Code icon">
</p>

<h1 align="center">XPico Code</h1>

<p align="center">
  A VS Code-style editor in under 200kb. <br>Compatible with Windows 95 and higher.
  Compatible with MacOS 9, OSX, and higher.<br>
  Small, lightweight, modern, compatible.
</p>

<p align="center">
  <b>Windows 2000 and higher</b><br/>
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-x86-win32.msi">Download installer .msi</a>
  <br/>
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-x86-win32.exe">Download standalone .exe</a>
  <br/>
  <b>Windows 95, 98 and Me</b><br/>
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-x86-win95.exe">Download standalone .exe</a>
</p>

<p align="center">
  <b>Mac OS 9 (PowerPC)</b><br/>
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-ppc-macos9.sit">Download .sit</a>
</p>

<p align="center">
  <b>Mac OS X</b><br/>
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-arm64-osx.zip">Apple Silicon (arm64)</a> &middot;
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-x86_64-osx.zip">Intel (x86_64)</a> &middot;
  <a href="https://github.com/PeterWarrington/XP-Code/releases/download/1.2.0/XPicoCode-v1.2.0-ppc-osx.zip">PowerPC (10.4 Tiger)</a>
</p>

<p align="center">
  Created by Peter Warrington (<a href="http://lilpete.me">lilpete.me</a>)
</p>

<p align="center">
  Donate to support me: <a href="https://www.buymeacoffee.com/peterwarrington">buymeacoffee.com/peterwarrington</a>
</p>

![XPico Code editing its own source while rebuilding itself in the integrated terminal](docs/screenshot.png)

## Features

- **Integrated terminal** – a real `cmd.exe` in the bottom panel, with multiple terminals, command
  history, Tab completion of file names, Ctrl+C to interrupt the running program, mouse selection,
  copy/paste, and scrollback. Killing a terminal also kills every process it started. On Windows
  95, 98 and Me it runs one command at a time instead – see Limitations.
- **Problems panel** – compiler errors printed in the terminal (`file:line: error: …` from TCC/GCC,
  `file(line) : error …` from MSVC, Python tracebacks) are collected into the Problems panel and the
  status bar; click one, or Ctrl+click the line in the terminal, to jump to it.
- **Editor** – syntax highlighting for C/C++, Python, JavaScript, Batch, INI, HTML/XML and Markdown;
  line numbers, indent guides, bracket matching, auto-indent, auto-closing brackets and quotes,
  multi-level undo/redo, move/copy/delete line, toggle comment, find and replace, a minimap, and
  automatic reload when a file changes on disk.
- **Workbench** – activity bar, Explorer file tree (create, rename, delete, reveal), Search across
  files, tabs with unsaved-change markers, breadcrumbs, status bar, resizable side bar and panel.
- **Command Palette** (Ctrl+Shift+P / F1), **Quick Open** (Ctrl+P) and **Go to Line** (Ctrl+G) with
  fuzzy matching.
- **Run and build** – F5 runs the active file in the terminal (`.c` is compiled with TCC first,
  `.py` runs with Python, `.bat` with `call`, `.js` with `cscript`); Ctrl+Shift+B runs the
  folder's `build.bat`.
- Remembers the open folder, open files, window layout and zoom level between sessions.

## Install

Download `XPicoCode-v1.2.0-x86-win32.msi` from the releases page and run it. The installer puts XPico Code in
`Program Files\XPico Code`, adds a Start Menu shortcut and an optional desktop shortcut, and registers
in Add or Remove Programs. Installing a newer version upgrades the old one in place.

Silent install and removal work the usual Windows Installer way:

```
msiexec /i XPicoCode-v1.2.0-x86-win32.msi /qn
msiexec /x XPicoCode-v1.2.0-x86-win32.msi /qn
```

Tested on Windows XP Professional (32-bit).

On Windows 95, 98 and Me, download `XPicoCode-v1.2.0-x86-win95.exe` and put it wherever you like.
There is no installer for these: the `.msi` wants Windows Installer, which they do not ship with.
It is the same editor from the same sources, built with every call that Windows 95 does not export
routed around (see `build.bat win95`), and it needs `msvcrt.dll` – which Windows 95 OSR2 and
anything later already have, but the original 4.00.950 release does not.

On Mac OS 9, download the `.sit` and expand it with StuffIt Expander, which every 9.x system has.
It is a StuffIt archive rather than a zip because the application only works as a file with its
resource fork intact, and a zip drops it.

On Mac OS X, download the `.zip` for your machine -- `arm64` for Apple Silicon, `x86_64` for Intel,
`ppc` for a 10.4 Tiger PowerPC -- and drag `XPicoCode.app` out of it.

## Author's note

Is this vibe-coded? Yes. 

Am I proud of this project? I cannot be given I lazily vibe-coded this.

Is it cool and useful though? Undeniably, so I am sharing it.

I did the logo though, so I did do something.

## Compilers and interpreters

XPico Code does not ship a compiler. For F5 and the terminal it adds these to `PATH` when they exist:

- Tiny C Compiler in `<XPico Code folder>\tcc`, `Program Files\tcc`, `C:\tcc` or `C:\dev\tools\tcc`
- Python in any `C:\Python*` folder

Other locations can be listed in the settings file (`%APPDATA%\XPico Code\xpcode.ini`):

```ini
[tools]
path=D:\tools\tcc;D:\Python34
```

## Keyboard shortcuts

| Action | Keys |
| --- | --- |
| Command Palette | Ctrl+Shift+P, F1 |
| Quick Open file | Ctrl+P |
| Go to line | Ctrl+G |
| Toggle terminal / new terminal | Ctrl+\` / Ctrl+Shift+\` |
| Toggle side bar / panel | Ctrl+B / Ctrl+J |
| Explorer / Search / Run | Ctrl+Shift+E / Ctrl+Shift+F / Ctrl+Shift+D |
| Run active file | F5 |
| Run build task | Ctrl+Shift+B |
| Find / replace | Ctrl+F / Ctrl+H |
| Toggle line comment | Ctrl+/ |
| Move / copy line | Alt+Up/Down / Shift+Alt+Up/Down |
| Delete line | Ctrl+Shift+K |
| Next / previous editor | Ctrl+Tab / Ctrl+Shift+Tab |
| Open folder / save all | Ctrl+K Ctrl+O / Ctrl+K S |
| Zoom in / out / reset | Ctrl+= / Ctrl+- / Ctrl+0 |

**Help → Keyboard Shortcuts Reference** lists every command.

## Building from source

Requirements:

- [Tiny C Compiler 0.9.27](https://download.savannah.gnu.org/releases/tinycc/) for Win32
  (`tcc-0.9.27-win32-bin.zip`) with the full Windows headers from `winapi-full-for-0.9.27.zip`
  copied over its `include` folder
- Python 3.4 to 3.12 (used for the resource and installer steps; standard library only -
  the installer needs `msilib`, which was removed in Python 3.13)

```
build.bat [C:\path\to\tcc.exe]        -> build\xpico-code.exe
build.bat win95 [C:\path\to\tcc.exe]  -> build\xpico-code-win95.exe
python installer\build_msi.py         -> dist\XPicoCode-<version>.msi
python tools\make_icon.py             -> res\icon.ico from res\icon.png (only when changing the icon)
```

`build.bat` uses `tcc.exe` from `PATH` when no path is given. A running `build\xpico-code.exe` is moved
aside before linking, so XPico Code can rebuild itself from its own terminal with Ctrl+Shift+B.

The version number lives in one place: `APP_VERSION` in `src/xpcode.h`. The resource embedder and
the installer both read it from there.

## Project layout

```
src/            the application (C, Win32 API)
  main.c        main window, layout, activity bar, status bar, commands, shortcuts, settings
  editor.c      text engine, syntax highlighting, tabs, breadcrumbs, minimap, find/replace
  explorer.c    side bar: file tree, search across files, run view
  terminal.c    panel: integrated terminal, problems, output
  palette.c     command palette, quick open, go to line, input prompts
  win95.c       the Windows 95 build's fallbacks (compiled only into it)
  xpcode.h      shared declarations, colours, command IDs
res/            icon.svg (artwork source), icon.png (1024 px export), icon.ico (16-48 px)
tools/          make_icon.py, embed_resources.py
installer/      build_msi.py
docs/           screenshot
build.bat       build script
```

## How it works

- **Drawing.** Everything except the menu bar and the text boxes is drawn with GDI into
  off-screen bitmaps: the activity bar icons are vector paths, the file-type badges are text, and
  the minimap draws each token as a small coloured rectangle.
- **Terminal.** Windows XP has no pseudo-console API, so each terminal runs `cmd.exe` with
  redirected pipes and its own hidden console. Output is read on a background thread and parsed for
  carriage returns, backspaces, form feeds (`cls`) and error locations. Ctrl+C attaches to the
  hidden console with `AttachConsole` and sends `GenerateConsoleCtrlEvent`. Each terminal runs
  inside a Job Object, so killing it, or closing XPico Code, also ends its child processes.
- **Windows 95.** One source, two builds, gated on `WIN95` – see the Windows 95 section at the
  foot of `src/xpcode.h` for the switches and `src/win95.c` for the fallbacks. It does not lower
  `WINVER`: what decides whether the program loads on Windows 95 is only which names reach the
  import table, so each call Windows 95 lacks is swapped for one it has (`InitCommonControls` for
  `InitCommonControlsEx`, `FindFirstFile` for `GetFileAttributesEx`, `SHGetSpecialFolderPath` for
  `SHGetFolderPath`, a cursor-watching timer for `TrackMouseEvent`, the `MSWHEEL_ROLLMSG`
  registered message for `WM_MOUSEWHEEL`), and the faces the editor draws with are chosen from
  what is actually installed. The terminal is the one part that could not be kept whole: see
  Limitations.
- **Resources.** Tiny C Compiler cannot compile `.rc` scripts, so `tools/embed_resources.py` builds
  the icon, version and manifest resources and appends them to the linked executable as a new
  `.rsrc` section.
- **Installer.** `installer/build_msi.py` writes the MSI database directly through Windows
  Installer, using Python's standard `msilib` module, so it needs neither WiX nor .NET.

## Limitations

- Programs run in the terminal see a pipe rather than a console. Many buffer their output until
  they exit (use `python -u`), and full-screen console programs such as `edit` do not work.
- On Windows 95, 98 and Me the terminal runs one command at a time rather than holding a shell
  open: `COMSPEC` there is `command.com`, a 16-bit MS-DOS program, and Windows 9x will not
  redirect a DOS program's output into a pipe. Each line is run from a generated batch file with
  its output collected in a file, so the screen, scrollback, history, Tab completion, prompt and
  the Problems panel all work as usual, but there is no standard input – and so no interactive
  programs – environment changes do not carry from one command to the next (the working directory
  does), and Ctrl+C ends the command rather than interrupting it.
- Text is single-byte Windows-1252; UTF-8 and UTF-16 files are not decoded.
- TCC stops at the first error, so a build reports one problem at a time.
- One editor group (no split view), no word wrap, no language server or debugger.

## License

[MIT](LICENSE)
# XP-Code
