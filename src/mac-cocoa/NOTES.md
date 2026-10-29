# Cocoa port (Mac OS X 10.4, PowerPC) - design notes

Same approach as `src/mac-carbon`: the app in `src/*.c` is never touched, and
this directory implements the Win32 subset it is written against. Only the
substrate differs -- Cocoa, Quartz and POSIX instead of QuickDraw and the
Carbon Event Manager.

Status: `win32.h` is written. The rest of the files below are still to do.

## Files

| file | what it holds |
| --- | --- |
| `win32.h` | done - the Win32 surface, the name traps, `MAX_PATH` 512 |
| `cocoamain.m` | `main()`: autorelease pool, `NSApplication`, `finishLaunching`, then `WinMain` |
| `wnd.m` | window/child table, the `XPView`/delegate, the event pump behind `GetMessage` |
| `gdi.m` | GDI over `CGContext`: DCs, offscreen bitmaps, text, shapes, the caret |
| `sys.m` | ini files, clipboard (`NSPasteboard`), menus (`NSMenu`), dialogs, file watches |
| `compat.m` | file I/O, memory, env, cursors (`NSCursor`), `NSWorkspace`, string spellings |
| `edit.c` | the single-line EDIT control - copy of the Carbon one, app-side only |
| `panel_cocoa.c` | Problems/Output/Terminal panel - the Carbon `panel_mac.c` minus the OS 9 branches |
| `term_pty.c` | the terminal transport: `forkpty` in-process, same `term_transport_*` API as `term_ot.c` |
| `Info.plist` | bundle metadata; `LSMinimumSystemVersion` 10.4 |

## Decisions already made

**The window model is unchanged.** One `NSWindow` with one `NSView`; the six
child HWNDs stay lightweight rectangles drawn into that view with a clipped,
origin-shifted HDC. Keeping this identical to the Carbon port is what makes the
rest of the port small.

**`BOOL` is the trap of this port.** `<objc/objc.h>` defines it as `signed
char`, and a second typedef is an error rather than a shadowed name, so
`win32.h` makes it a macro (`#define BOOL w32_BOOL`) alongside the QuickDraw
renames. That keeps one definition of the type in every translation unit. The
cost: inside the shim's `.m` files, Objective-C methods that override AppKit
ones (`isFlipped`, `acceptsFirstResponder`, `application:openFile:`, the drag
ones) must be written as `signed char`, which is what AppKit declares.

**Coordinates.** `XPView` is `isFlipped`, so the window context is already
top-left origin, y down, and the app's rects go straight through. Offscreen
`CGBitmapContext`s get `translate(0,h); scale(1,-1)` at creation so every
primitive is written once. Text then needs
`CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1))` or the glyphs
come out mirrored.

**Text.** `CGContextSelectFont` + `CGContextShowTextAtPoint` with
`kCGEncodingMacRoman`, which suits the editor's byte-oriented text and is far
cheaper per draw than `NSString` drawing. Metrics come from `NSFont`, measured
once in `CreateFont`, so they are valid before the window exists -- `main.c`
builds its fonts first and lays the whole editor out from the code font's cell.

The caret-drift trap from the Carbon port applies here too, in a different
form: Quartz advances are fractional (Monaco at 14pt is 8.4px) while the editor
assumes an integer cell. For a `FIXED_PITCH` font, set
`CGContextSetCharacterSpacing(ctx, cw - advance)` so each cell is exactly the
`cw` the editor was told; for proportional text keep the per-character
advances in a float table and round only the total.

**Bitmaps.** `CreateCompatibleBitmap` owns a malloc'd buffer with a
`CGBitmapContext` over it, `kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big`
so the memory layout is XRGB whatever the endianness -- `GetDIBits` then builds
its `0x00RRGGBB` DWORDs byte by byte, as the Carbon port does. `BitBlt` should
wrap the buffer in a `CGImage` through a *direct* data provider rather than
`CGBitmapContextCreateImage`: the editor's buffer is the size of the window and
a per-frame copy of it is exactly what a G4 cannot afford. Blit by clipping to
the destination rect, translating by `(dx - sx, dy - sy)` and drawing the whole
image flipped - that avoids relying on `CGImageCreateWithImageInRect`'s
orientation.

**The event loop.** `[NSApp finishLaunching]`, then `GetMessage` pumps
`nextEventMatchingMask:untilDate:inMode:dequeue:` itself and calls
`[NSApp sendEvent:]`, so the Win32 loop in `WinMain` stays in charge. One
autorelease pool per pass. The view's `keyDown:`/mouse methods queue Win32
messages rather than returning them, which keeps menu key equivalents working:
`sendEvent:` offers Cmd keys to the menu bar first, and only what the menu does
not claim reaches `keyDown:`. Wait 20ms a pass while the caret is blinking, the
mouse is down or the terminal is connected, 100ms otherwise. Painting is
`InvalidateRect` -> `setNeedsDisplayInRect:`, with `paint_tree` running inside
`drawRect:`.

**The terminal is real.** 10.4 has `fork`, so `term_pty.c` runs the shell in a
pty in-process and `xpcode-termd` is not needed. The frame protocol disappears;
keep the `term_transport_*` API so `panel_cocoa.c` stays a near-copy of
`panel_mac.c`. `tools/termd/xpcode-termd.c` already has the `forkpty`,
`TIOCSWINSZ` and `waitpid` code to lift.

## Still to wire up outside this directory

- `src/xpcode.h`: a `MAC_COCOA` branch inside the existing `MAC_OS` one -
  this header, `TERMINAL_AVAILABLE()` 1, `PATH_SEP` `/`, `LINE_ENDING` `"\n"`
  (`"LF"`), `PLATFORM_BLURB` for Mac OS X. Build with `-DMAC_OS -DMAC_COCOA` so
  the shared `#ifdef MAC_OS` paths (the `sh` run recipes, `mac_lang_sniff`,
  Reveal in Finder) still apply.
- `src/main.c`: the About text under `MAC_OS` says "just QuickDraw and Carbon";
  move that string to an `ABOUT_TECH` macro per platform. `locate_settings()`
  hardcodes `"\\"` - use `PATH_SEPS` so the settings file lands in
  `~/Library/Application Support/XPico Code/`.
- `tools/build_mac_cocoa.sh`: `gcc-4.0 -arch ppc -isysroot
  /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4`, shim and app
  compiled the same way (no split include path is needed here - the app sources
  never see Cocoa), link `-framework Cocoa`, then assemble
  `build/mac-cocoa/XPicoCode.app`. Modern clang cannot target ppc, so the
  script must find Xcode 2.x/3.x and say so plainly when it cannot.
- `tools/make_icns.py`: `is32/il32/ih32/it32` plus their masks from
  `res/icon-mac.png`, RLE-compressed. 10.4 does not read PNG-based `ic08`
  entries. Checked in like `src/mac-carbon/icons.r` is.
