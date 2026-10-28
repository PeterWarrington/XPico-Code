/* Deliberately empty.
 *
 * Retro68 symlinks every Universal Interface header into the target include
 * directory, including strings.h -> Apple's Strings.h. macOS is case-insensitive,
 * so newlib's <string.h> (which includes <strings.h>) ends up dragging MacTypes,
 * QuickDraw and the rest of the Toolbox into every translation unit. That breaks
 * the app sources, whose own identifiers collide with Toolbox ones -- editor.c's
 * "Line" struct against QuickDraw's Line(), for instance.
 *
 * The app code only needs the Win32 shim, never the Toolbox, so this file
 * shadows the symlink when src/mac/override is first on the include path.
 * It is used ONLY for the app sources: the shim's own files (mac/*.c) include
 * <Carbon.h> on purpose and are compiled without this directory. */
#ifndef XPCODE_OVERRIDE_STRINGS_H
#define XPCODE_OVERRIDE_STRINGS_H
#endif
