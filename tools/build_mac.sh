#!/bin/bash
# Build XPico Code for Mac OS 9 / Carbon with Retro68.
#
#   tools/build_mac.sh [path/to/Retro68-build/toolchain]
#
# The two halves of the build use different include paths on purpose:
#
#   - the shim (src/mac-carbon/*.c) wants the Toolbox, so it is compiled with
#     Apple's Universal Interfaces on the include path;
#   - the app (src/*.c) must NOT see the Toolbox. Retro68 symlinks the
#     Universal Interfaces into the target include directory, strings.h among
#     them, and on a case-insensitive filesystem newlib's <string.h> then pulls
#     in Apple's Strings.h and with it QuickDraw, whose Line() collides with
#     editor.c's own Line struct. src/mac-carbon/override/strings.h shadows that.
set -e
cd "$(dirname "$0")/.."

TOOLCHAIN="${1:-$HOME/Retro68-build/toolchain}"
GCC="$TOOLCHAIN/bin/powerpc-apple-macos-gcc"
REZ="$TOOLCHAIN/bin/Rez"
# Apple's Universal Interfaces when the toolchain was built with them, and
# otherwise the open-source multiversal ones, which is what a default Retro68
# build installs now (Apple's are an optional extra you have to supply). Both
# provide the same Toolbox headers; only the directory name differs.
CINCLUDES="$TOOLCHAIN/universal/CIncludes"
[ -d "$CINCLUDES" ] || CINCLUDES="$TOOLCHAIN/multiversal/CIncludes"

if [ ! -x "$GCC" ]; then
    echo "Retro68 not found at $TOOLCHAIN" >&2
    echo "Pass the toolchain directory: tools/build_mac.sh /path/to/Retro68-build/toolchain" >&2
    exit 1
fi

OUT=build/mac
mkdir -p "$OUT"

# TARGET_API_MAC_CARBON is what selects the Carbon API surface in the Toolbox
# headers, and gcc's -carbon flag does not define it -- it only picks the
# target and the runtime. Without it the whole Carbon accessor family
# (GetPortBitMapForCopyBits, GetPortBounds, GetRegionBounds and the rest) is
# #if'd out, and the shim's calls to them compile as implicit int-returning
# functions. Rez has always been told; the compiler has to be told too.
CFLAGS="-carbon -DTARGET_API_MAC_CARBON=1 -Os -DMAC_OS -Wall -Wno-unused-parameter ${XPCODE_DIAG:+-DXPCODE_DIAG}"

# The shim: Toolbox visible.
SHIM="gdi wnd sys compat macmain term_ot"
# The app, plus the shim's EDIT control, which uses the app's own helpers.
APP="main editor explorer palette"
APP_MAC="edit panel_mac"

OBJS=()
for f in $SHIM; do
    echo "  CC   src/mac-carbon/$f.c"
    $GCC $CFLAGS -I"$CINCLUDES" -c "src/mac-carbon/$f.c" -o "$OUT/shim_$f.o"
    OBJS+=("$OUT/shim_$f.o")
done
for f in $APP; do
    echo "  CC   src/$f.c"
    $GCC $CFLAGS -Isrc/mac-carbon/override -Isrc -c "src/$f.c" -o "$OUT/app_$f.o"
    OBJS+=("$OUT/app_$f.o")
done
for f in $APP_MAC; do
    echo "  CC   src/mac-carbon/$f.c"
    $GCC $CFLAGS -Isrc/mac-carbon/override -Isrc -c "src/mac-carbon/$f.c" -o "$OUT/app_$f.o"
    OBJS+=("$OUT/app_$f.o")
done

echo "  LD   $OUT/XPicoCode.xcoff"
$GCC -carbon -o "$OUT/XPicoCode.xcoff" "${OBJS[@]}"

# gcc emits XCOFF; a Carbon application is a PEF binary wrapped in a Mac file
# with a 'cfrg' resource, so MakePEF converts it and Rez builds the container.
# The outputs are a MacBinary file, a bare APPL, and a mountable disk image.
echo "  PEF  $OUT/XPicoCode.pef"
"$TOOLCHAIN/bin/MakePEF" "$OUT/XPicoCode.xcoff" -o "$OUT/XPicoCode.pef"

echo "  REZ  $OUT/XPicoCode.bin"
"$REZ" -DTARGET_API_MAC_CARBON=1 \
    src/mac-carbon/XPicoCode.r \
    -I"$TOOLCHAIN/powerpc-apple-macos/RIncludes" \
    -DCFRAG_NAME='"XPicoCode"' \
    -o "$OUT/XPicoCode.bin" \
    --cc "$OUT/XPicoCode.dsk" --cc "$OUT/XPicoCode.APPL" \
    -t APPL -c XPCD \
    --data "$OUT/XPicoCode.pef"

# The Finder reads an application's BNDL only when its bundle bit is set, and
# it resolves icons when the volume is mounted -- so this has to be set in the
# image rather than by the app at startup.
echo "  BIT  bundle bit"
python3 tools/set_bundle_bit.py "$OUT/XPicoCode.dsk" APPL XPCD

# The release archive. A Carbon application is only an application because of
# its resource fork, and zip, tar and every web download drop that fork -- so
# the one thing a 9.x machine can be handed is a .sit, which it can expand with
# the StuffIt Expander it already has. APP_VERSION in src/xpcode.h is the only
# place the version lives.
VERSION=$(sed -n 's/^#define APP_VERSION "\([^"]*\)".*/\1/p' src/xpcode.h)
[ -n "$VERSION" ] || { echo "no APP_VERSION in src/xpcode.h" >&2; exit 1; }
# Every shippable archive, from every target, lands in build/dist.
DIST=build/dist
mkdir -p "$DIST"
SIT="$DIST/XPicoCode-v$VERSION-ppc-macos9.sit"

# The MacBinary is the input rather than the two-fork APPL: it carries both
# forks, the type, the creator and the dates in one ordinary file, so it does
# not depend on the filesystem having kept a resource fork.
echo "  SIT  $SIT"
python3 tools/make_sit.py "$OUT/XPicoCode.bin" -o "$SIT"

echo
echo "Build succeeded:"
ls -la "$SIT" "$OUT"/XPicoCode.bin "$OUT"/XPicoCode.dsk "$OUT"/XPicoCode.APPL 2>/dev/null
echo
echo "  $(basename "$SIT")  expand on a Mac OS 9 machine"
echo "  XPicoCode.dsk  mount in a Mac OS 9 VM"
