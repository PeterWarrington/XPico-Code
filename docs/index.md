---
title: XPico Code - A VS Code-style editor for Windows XP
description: A VS Code-style editor in under 200kb and compatible with Windows XP. No Electron, no Chromium - all Win32 C, built with Tiny C Compiler.
---
<p align="center">
  <img src="icon.png" width="96" height="96" alt="XPico Code icon">
</p>

<h1 align="center">XPico Code</h1>

<p align="center">
  A VS Code-style editor in under 200kb and compatible with Windows XP.<br>
  Small, and all Win32.
</p>

<p align="center">
<a class="button" href="http://lilpete.me/xpcode/XPCode-1.1.0.msi">Download installer .msi</a>
  <br/>
<a class="button" href="http://lilpete.me/xpcode/XPCode-1.1.0.exe">Download standalone .exe</a>
  <br/>
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
  copy/paste, and scrollback. Killing a terminal also kills every process it started.
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


