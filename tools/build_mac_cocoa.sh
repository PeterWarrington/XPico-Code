#!/bin/bash
# Build XPico Code for Mac OS X 10.4 (Tiger) on PowerPC.
#
#   tools/build_mac_cocoa.sh [path/to/gcc]
#   tools/build_mac_cocoa.sh --host          build for this machine instead
#
# Environment:
#   XPCODE_CC     the compiler to use (default: the first gcc-4.x found)
#   XPCODE_SDK    the 10.4 SDK (default: /Developer/SDKs/MacOSX10.4u.sdk)
#   XPCODE_ARCHS  architectures to build (default: ppc; e.g. "ppc i386")
#
# --host builds the same sources with the system compiler for whatever machine
# you are on, which is how to try a change without a Tiger box to hand. The
# code is unchanged -- it is still written to the 10.4 API surface -- so the
# only concessions are two warning flags: everything this shim uses was
# deprecated years later, and Objective-C's BOOL is bool on a modern system
# where 10.4 had it as signed char. A --host build is for testing, not for
# shipping: it will not run on Tiger.
#
# There is no split include path here, unlike the Carbon build: the shim's .m
# files include Cocoa themselves and the app's .c files only ever see
# src/mac-cocoa/win32.h, so one set of flags does for both halves.
#
# What this needs is a toolchain that can still emit PowerPC code, which means
# Xcode 2.x or 3.x and the 10.4u SDK -- either natively on a PowerPC Mac, or
# cross-compiling from an Intel Mac running 10.5 or 10.6. Clang cannot do it:
# PowerPC support was removed long ago.
set -e
cd "$(dirname "$0")/.."

HOST=0
if [ "$1" = "--host" ]; then HOST=1; shift; fi

GCC="${1:-${XPCODE_CC:-}}"
SDK="${XPCODE_SDK:-}"
ARCHS="${XPCODE_ARCHS:-ppc}"
EXTRA=""

if [ "$HOST" = 1 ]; then
    [ -n "$GCC" ] || GCC=$(command -v clang || command -v cc)
    ARCHS="$(uname -m)"
    SDK=""
    EXTRA="-Wno-deprecated-declarations -Wno-mismatched-return-types"
fi

if [ -z "$GCC" ]; then
    for c in /usr/bin/gcc-4.0 /Developer/usr/bin/gcc-4.0 \
             /usr/bin/gcc-4.2 /Developer/usr/bin/gcc-4.2 /usr/bin/gcc; do
        [ -x "$c" ] && GCC="$c" && break
    done
fi
if [ -z "$GCC" ] || [ ! -x "$GCC" ]; then
    echo "No compiler found. Pass one: tools/build_mac_cocoa.sh /path/to/gcc-4.0" >&2
    exit 1
fi

if [ -z "$SDK" ]; then
    for s in /Developer/SDKs/MacOSX10.4u.sdk /Developer/SDKs/MacOSX10.5.sdk; do
        [ -d "$s" ] && SDK="$s" && break
    done
fi

ARCHFLAGS=""
for a in $ARCHS; do ARCHFLAGS="$ARCHFLAGS -arch $a"; done

SYSFLAGS=""
if [ "$HOST" = 1 ]; then
    :
elif [ -n "$SDK" ]; then
    SYSFLAGS="-isysroot $SDK -mmacosx-version-min=10.4"
elif [ "$(uname -p)" != "powerpc" ]; then
    echo "No 10.4 SDK found, and this is not a PowerPC Mac." >&2
    echo "Install Xcode 2.x/3.x, or point XPCODE_SDK at MacOSX10.4u.sdk." >&2
    echo "To try the build on this machine instead: tools/build_mac_cocoa.sh --host" >&2
    exit 1
fi

# Fail here rather than halfway through the build if the compiler cannot
# actually target the requested architecture.
if ! $GCC $ARCHFLAGS $SYSFLAGS -E -x c /dev/null >/dev/null 2>&1; then
    echo "$GCC cannot build for '$ARCHS'." >&2
    echo "Clang dropped PowerPC; use gcc-4.0 from Xcode 2.x or 3.x," >&2
    echo "or build for this machine: tools/build_mac_cocoa.sh --host" >&2
    exit 1
fi

OUT=build/mac-cocoa
APP="$OUT/XPicoCode.app"
mkdir -p "$OUT" "$APP/Contents/MacOS" "$APP/Contents/Resources"

CFLAGS="$ARCHFLAGS $SYSFLAGS -Os -DMAC_OS -DMAC_COCOA -Wall -Wno-unused-parameter $EXTRA -Isrc"

# The shim, which talks to the frameworks.
SHIM_M="cocoamain wnd gdi sys compat"
SHIM_C="term_pty"
# The app, plus the two shim files written against the app's own helpers.
APP_C="main editor explorer palette"
APP_MAC="edit panel_cocoa"

# Appended by index rather than with "+=": Tiger ships bash 2.05b, where the
# array append operator does not exist yet.
OBJS=()
for f in $SHIM_M; do
    echo "  CC   src/mac-cocoa/$f.m"
    $GCC $CFLAGS -c "src/mac-cocoa/$f.m" -o "$OUT/shim_$f.o"
    OBJS[${#OBJS[@]}]="$OUT/shim_$f.o"
done
for f in $SHIM_C; do
    echo "  CC   src/mac-cocoa/$f.c"
    $GCC $CFLAGS -c "src/mac-cocoa/$f.c" -o "$OUT/shim_$f.o"
    OBJS[${#OBJS[@]}]="$OUT/shim_$f.o"
done
for f in $APP_C; do
    echo "  CC   src/$f.c"
    $GCC $CFLAGS -c "src/$f.c" -o "$OUT/app_$f.o"
    OBJS[${#OBJS[@]}]="$OUT/app_$f.o"
done
for f in $APP_MAC; do
    echo "  CC   src/mac-cocoa/$f.c"
    $GCC $CFLAGS -c "src/mac-cocoa/$f.c" -o "$OUT/app_$f.o"
    OBJS[${#OBJS[@]}]="$OUT/app_$f.o"
done

echo "  LD   $APP/Contents/MacOS/XPicoCode"
$GCC $ARCHFLAGS $SYSFLAGS -o "$APP/Contents/MacOS/XPicoCode" "${OBJS[@]}" \
    -framework Cocoa -framework ApplicationServices

# The bundle. APP_VERSION in src/xpcode.h is the only place the version lives.
VERSION=$(sed -n 's/^#define APP_VERSION "\([^"]*\)".*/\1/p' src/xpcode.h)
echo "  GEN  $APP/Contents/Info.plist   (version $VERSION)"
sed "s/@VERSION@/$VERSION/g" src/mac-cocoa/Info.plist > "$APP/Contents/Info.plist"

# Classic metadata, which the Finder on 10.4 still reads.
printf 'APPLXPCD' > "$APP/Contents/PkgInfo"

if [ -f src/mac-cocoa/XPicoCode.icns ]; then
    echo "  CP   XPicoCode.icns"
    cp src/mac-cocoa/XPicoCode.icns "$APP/Contents/Resources/"
else
    echo "  --   no XPicoCode.icns (run tools/make_icns.py); the app will use the generic icon"
fi

echo
echo "Build succeeded:"
echo "  $APP"
echo
if [ "$HOST" = 1 ]; then
    echo "  Built for $ARCHS on this machine, for testing: open $APP"
else
    echo "  Copy the .app to a 10.4 machine and double-click it."
fi
