---
title: XPico Code - A VS Code-style editor in under 200kb. Compatible with Windows 95 and higher. Compatible with MacOS 9, OSX, and higher.
description: A VS Code-style editor in under 200kb. Compatible with Windows 95 and higher. Compatible with MacOS 9, OSX, and higher.
---
<p align="center">
  <img src="icon.png" width="96" height="96" alt="XPico Code icon">
</p>

<h1 align="center">XPico Code</h1>

<p align="center">
  A VS Code-style editor in under 200kb. <br> Compatible with Windows 95 and higher. Compatible with MacOS 9, OSX, and higher.<br>
  Small, lightweight, modern, compatible.
</p>

<table class="downloads">
  <caption>Version 1.2.0</caption>
  <tr>
    <td>
      <span class="platform">Windows</span>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-x86-win32.msi">Installer .msi</a>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-x86-win32.exe">Standalone .exe</a>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-x86-win95.exe">Windows 95 .exe</a>
      <span class="note">32-bit &middot; 2000 and up, or 95/98/Me</span>
    </td>
    <td>
      <span class="platform">Mac OS 9</span>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-ppc-macos9.sit">PowerPC .sit</a>
      <span class="note">Expand with StuffIt</span>
    </td>
    <td>
      <span class="platform">Mac OS X</span>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-arm64-osx.zip">Apple Silicon .zip</a>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-x86_64-osx.zip">Intel .zip</a>
      <a class="button" href="http://lilpete.me/xpcode/XPicoCode-v1.2.0-ppc-osx.zip">PowerPC .zip</a>
      <span class="note">arm64 &middot; x86_64 &middot; 10.4 Tiger</span>
    </td>
  </tr>
</table>

<p align="center">
  <a class="button" href="http://github.com/PeterWarrington/XPico-Code">See GitHub Repo</a>
</p>

<p align="center">
  Created by Peter Warrington (<a href="http://lilpete.me">lilpete.me</a>)
</p>

<p align="center">
  Donate to support me: <a href="https://www.buymeacoffee.com/peterwarrington">buymeacoffee.com/peterwarrington</a>
</p>

<p class="screenshot"><img src="screenshot.png" alt="XPico Code editing its own source while rebuilding itself in the integrated terminal" width="750px"></p>

## Features

- **Integrated terminal** – a real `cmd.exe` in the bottom panel, with multiple terminals, command
  history, Tab completion of file names, Ctrl+C to interrupt the running program, mouse selection,
  copy/paste, and scrollback. Killing a terminal also kills every process it started. On Windows
  95, 98 and Me, where `COMSPEC` is a 16-bit `command.com` whose output Windows will not put into
  a pipe, it runs one command at a time from a generated batch file instead – everything above the
  process is the same, but there is no standard input and Ctrl+C ends the command.
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


