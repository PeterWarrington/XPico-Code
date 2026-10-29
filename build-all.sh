#!/bin/bash
# Build every XPico Code release artifact, for every platform, into build/dist.
#
#   ./build-all.sh                  all three targets (four platforms)
#   ./build-all.sh cocoa win32      just those
#
# Environment:
#   XPCODE_ARCHS      Cocoa architectures (default: "arm64 x86_64 ppc")
#   XPCODE_RETRO68    the Retro68 toolchain, for the Carbon build
#   XPCODE_VM         the UTM VM that builds Cocoa ppc  (default "Mac OS X 10.4")
#   XPCODE_WIN_VM     the UTM VM that builds Windows    (default "Windows XP 2")
#
# The targets are:
#
#   cocoa    Mac OS X, via tools/build_mac_cocoa.sh. arm64 and x86_64 compile
#            here; ppc is built inside the Tiger VM over ssh.
#            -> XPicoCode-v<version>-{arm64,x86_64,ppc}-osx.zip
#
#   carbon   Mac OS 9, via tools/build_mac.sh, which needs Retro68. The
#            application only works as a file with a resource fork, so it
#            ships as a StuffIt archive.
#            -> XPicoCode-v<version>-ppc-macos9.sit
#
#   win32    Windows, via tools/build_win32.py, inside the XP VM -- the only
#            machine that can run Tiny C Compiler and msilib. Builds twice:
#            once for Windows 2000 and up, and once with WIN95 defined, which
#            gates away every call Windows 95 does not export (see the
#            Windows 95 section at the foot of src/xpcode.h). The installer is
#            for the first of those only; Windows 95 gets the standalone .exe.
#            -> XPicoCode-v<version>-x86-win32.exe and .msi
#            -> XPicoCode-v<version>-x86-win95.exe
#
# Two of the three need a VM, and the slow one is emulated PowerPC, so a full
# run is minutes rather than seconds. A target that cannot build does not stop
# the others: each one's result is reported at the end, and the exit status is
# non-zero if any of them failed. That way a release is never half-announced
# because the one toolchain you forgot to install aborted the script early.
set -u
cd "$(dirname "$0")"

DIST=build/dist
VERSION=$(sed -n 's/^#define APP_VERSION "\([^"]*\)".*/\1/p' src/xpcode.h)
[ -n "$VERSION" ] || { echo "no APP_VERSION in src/xpcode.h" >&2; exit 1; }

TARGETS="$*"
[ -n "$TARGETS" ] || TARGETS="cocoa carbon win32"

for t in $TARGETS; do
    case "$t" in
        cocoa|carbon|win32) ;;
        *) echo "unknown target '$t' (cocoa, carbon, win32)" >&2; exit 2 ;;
    esac
done

mkdir -p "$DIST"

# Anything already in build/dist is from an earlier run. Rather than delete it
# -- which would throw away the artifacts of a target not being rebuilt -- mark
# the moment the build starts, so the summary can say which files are fresh.
STAMP=$(mktemp "${TMPDIR:-/tmp}/xpcode-build-all.XXXXXX")
trap 'rm -f "$STAMP"' EXIT

echo "XPico Code $VERSION -> $DIST"
echo "targets: $TARGETS"
echo

OK=""
FAILED=""

banner() {
    echo
    echo "=============================================================="
    echo "  $1"
    echo "=============================================================="
}

# Run one target, remember whether it worked, and keep going either way.
attempt() {
    name="$1"
    shift
    banner "$name"
    if "$@"; then
        OK="$OK $name"
    else
        FAILED="$FAILED $name"
        echo "  !!   $name FAILED" >&2
    fi
}

build_cocoa() {
    XPCODE_ARCHS="${XPCODE_ARCHS:-arm64 x86_64 ppc}" tools/build_mac_cocoa.sh
}

build_carbon() {
    if [ -n "${XPCODE_RETRO68:-}" ]; then
        tools/build_mac.sh "$XPCODE_RETRO68"
    else
        tools/build_mac.sh
    fi
}

build_win32() {
    python3 tools/build_win32.py
}

for t in $TARGETS; do
    case "$t" in
        cocoa)  attempt cocoa  build_cocoa ;;
        carbon) attempt carbon build_carbon ;;
        win32)  attempt win32  build_win32 ;;
    esac
done

banner "build/dist"

# -newer against the stamp separates this run's output from what was already
# there, so a stale file is never mistaken for a rebuilt one.
# Everything is listed, not just this version's files, so that an artifact
# left over from an older release cannot sit in build/dist unmentioned and get
# uploaded with the rest.
found=0
for f in "$DIST"/XPicoCode-*; do
    [ -e "$f" ] || continue
    found=1
    case "$f" in
        *-v$VERSION-*)
            if [ "$f" -nt "$STAMP" ]; then
                note=""
            else
                note="   (not rebuilt by this run)"
            fi
            ;;
        *) note="   (not $VERSION -- from an earlier release)" ;;
    esac
    printf '  %9s  %s%s\n' "$(wc -c < "$f" | tr -d ' ')" "$f" "$note"
done
[ "$found" = 1 ] || echo "  (nothing)"

echo
[ -n "$OK" ] && echo "built:  ${OK# }"
if [ -n "$FAILED" ]; then
    echo "failed: ${FAILED# }" >&2
    exit 1
fi
echo
echo "All targets built."
