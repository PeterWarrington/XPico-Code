#!/bin/bash
# Build XPico Code for Mac OS X, one .app and one .zip per architecture.
#
#   tools/build_mac_cocoa.sh                 arm64 and x86_64, on this machine
#   XPCODE_ARCHS="arm64 x86_64 ppc" tools/build_mac_cocoa.sh
#   tools/build_mac_cocoa.sh /path/to/gcc    cross-compile with an old gcc
#
# Environment:
#   XPCODE_ARCHS   architectures to build (default: "arm64 x86_64"; add ppc)
#   XPCODE_CC      the compiler to use (default: clang, or gcc-4.x for ppc)
#   XPCODE_SDK     an SDK to build against (default: none, i.e. this system's)
#   XPCODE_VM      the UTM VM to build ppc in (default: "Mac OS X 10.4")
#   XPCODE_VM_SSH  the ssh host for that VM (default: tiger, from ~/.ssh/config)
#
# Each architecture is built on its own, into build/mac-cocoa/<arch>, and
# packaged as build/dist/XPicoCode-v<version>-<arch>-osx.zip. Nothing is ever
# lipo'd together: a 10.4 PowerPC binary and an arm64 one have no business in
# one file, since no machine can run both and the Finder on neither system
# would thank you for it.
#
# arm64 and x86_64 build here with clang. The code is unchanged -- it is still
# written to the 10.4 API surface -- so the only concessions are two warning
# flags: everything this shim uses was deprecated years after Tiger, and
# Objective-C's BOOL is bool on a modern system where 10.4 had it as signed
# char.
#
# ppc cannot be built here at all: clang dropped the backend, and the SDK has
# no PowerPC slice. It is instead built inside the UTM VM -- see build_ppc()
# below, which starts it, copies the tree in over ssh, runs this same script
# there, and brings the bundle back. On the guest the ppc branch compiles
# natively with gcc-4.0 against /Developer/SDKs/MacOSX10.4u.sdk.
#
# For the record, i386 is not on the menu either, and not for want of a Mac
# that can host it: "ld: linking for i386 is no longer supported", and the
# SDK's AppKit.tbd lists only x86_64 and arm64e. A 32-bit Intel build needs
# Xcode 10 / the 10.14 SDK at the newest.
#
# There is no split include path here, unlike the Carbon build: the shim's .m
# files include Cocoa themselves and the app's .c files only ever see
# src/mac-cocoa/win32.h, so one set of flags does for both halves.
set -e
cd "$(dirname "$0")/.."

# --host used to mean "build for this machine rather than for Tiger", which is
# now simply what the default archs do. Still accepted so old invocations and
# muscle memory keep working.
if [ "$1" = "--host" ]; then shift; fi

ARCHS="${XPCODE_ARCHS:-arm64 x86_64}"
OUT=build/mac-cocoa
# Every shippable archive, from every target, lands in one directory: that is
# what gets uploaded, and nothing else in build/ does.
DIST=build/dist

# APP_VERSION in src/xpcode.h is the only place the version lives.
VERSION=$(sed -n 's/^#define APP_VERSION "\([^"]*\)".*/\1/p' src/xpcode.h)
[ -n "$VERSION" ] || { echo "no APP_VERSION in src/xpcode.h" >&2; exit 1; }

VM="${XPCODE_VM:-Mac OS X 10.4}"
VM_SSH="${XPCODE_VM_SSH:-tiger}"
VM_DIR="${XPCODE_VM_DIR:-xpcode-build}"
UTMCTL="${XPCODE_UTMCTL:-/Applications/UTM.app/Contents/MacOS/utmctl}"

# The shim, which talks to the frameworks.
SHIM_M="cocoamain wnd gdi sys compat"
SHIM_C="term_pty"
# The app, plus the two shim files written against the app's own helpers.
APP_C="main editor explorer palette"
APP_MAC="edit panel_cocoa"


# Compile and link one architecture into $OUT/<arch>/XPicoCode.app.
compile() {
    arch="$1"
    archdir="$OUT/$arch"
    app="$archdir/XPicoCode.app"

    cc="${XPCODE_CC:-}"
    sdk="${XPCODE_SDK:-}"
    extra="-Wno-deprecated-declarations -Wno-mismatched-return-types"

    if [ "$arch" = ppc ]; then
        # Only reached on the PowerPC guest, or when someone has pointed
        # XPCODE_CC/XPCODE_SDK at a cross toolchain; build_ppc() handles the
        # case where neither is true.
        if [ -z "$cc" ]; then
            for c in /usr/bin/gcc-4.0 /Developer/usr/bin/gcc-4.0 \
                     /usr/bin/gcc-4.2 /Developer/usr/bin/gcc-4.2; do
                [ -x "$c" ] && cc="$c" && break
            done
        fi
        if [ -z "$sdk" ]; then
            for s in /Developer/SDKs/MacOSX10.4u.sdk \
                     /Developer/SDKs/MacOSX10.5.sdk; do
                [ -d "$s" ] && sdk="$s" && break
            done
        fi
        # Nothing the 10.4 shim touches was deprecated in 2005, and gcc-4.0
        # does not know these flags anyway.
        extra=""
    fi
    [ -n "$cc" ] || cc=$(command -v clang || command -v cc)
    if [ -z "$cc" ] || [ ! -x "$cc" ]; then
        echo "No compiler found for $arch. Pass one in XPCODE_CC." >&2
        exit 1
    fi

    sysflags=""
    if [ -n "$sdk" ]; then
        sysflags="-isysroot $sdk -mmacosx-version-min=10.4"
    fi

    # Fail here rather than halfway through the build if the compiler cannot
    # actually target this architecture.
    if ! $cc -arch "$arch" $sysflags -E -x c /dev/null >/dev/null 2>&1; then
        echo "$cc cannot build for '$arch'." >&2
        exit 1
    fi

    rm -rf "$archdir"
    mkdir -p "$archdir" "$app/Contents/MacOS" "$app/Contents/Resources"

    cflags="-arch $arch $sysflags -Os -DMAC_OS -DMAC_COCOA -Wall \
-Wno-unused-parameter $extra -Isrc"

    # Appended by index rather than with "+=": Tiger ships bash 2.05b, where
    # the array append operator does not exist yet.
    objs=()
    for f in $SHIM_M; do
        echo "  CC   [$arch] src/mac-cocoa/$f.m"
        $cc $cflags -c "src/mac-cocoa/$f.m" -o "$archdir/shim_$f.o"
        objs[${#objs[@]}]="$archdir/shim_$f.o"
    done
    for f in $SHIM_C; do
        echo "  CC   [$arch] src/mac-cocoa/$f.c"
        $cc $cflags -c "src/mac-cocoa/$f.c" -o "$archdir/shim_$f.o"
        objs[${#objs[@]}]="$archdir/shim_$f.o"
    done
    for f in $APP_C; do
        echo "  CC   [$arch] src/$f.c"
        $cc $cflags -c "src/$f.c" -o "$archdir/app_$f.o"
        objs[${#objs[@]}]="$archdir/app_$f.o"
    done
    for f in $APP_MAC; do
        echo "  CC   [$arch] src/mac-cocoa/$f.c"
        $cc $cflags -c "src/mac-cocoa/$f.c" -o "$archdir/app_$f.o"
        objs[${#objs[@]}]="$archdir/app_$f.o"
    done

    echo "  LD   [$arch] $app/Contents/MacOS/XPicoCode"
    $cc -arch "$arch" $sysflags -o "$app/Contents/MacOS/XPicoCode" \
        "${objs[@]}" -framework Cocoa -framework ApplicationServices

    echo "  GEN  [$arch] $app/Contents/Info.plist   (version $VERSION)"
    sed "s/@VERSION@/$VERSION/g" src/mac-cocoa/Info.plist \
        > "$app/Contents/Info.plist"

    # Classic metadata, which the Finder on 10.4 still reads.
    printf 'APPLXPCD' > "$app/Contents/PkgInfo"

    if [ -f src/mac-cocoa/XPicoCode.icns ]; then
        cp src/mac-cocoa/XPicoCode.icns "$app/Contents/Resources/"
    else
        echo "  --   no XPicoCode.icns (run tools/make_icns.py); the app will"\
             "use the generic icon"
    fi
}


# Build ppc inside the UTM VM: start it, copy this tree in, run this same
# script on the guest, and bring the finished bundle back.
#
# The guest does the work because it is the only PowerPC machine available --
# and conveniently it is also a real 10.4 box, so the build it produces is the
# one that gets shipped, compiled against the SDK it will actually run on.
build_ppc() {
    if [ "$(uname -p)" = powerpc ] || [ -n "$XPCODE_CC$XPCODE_SDK" ]; then
        compile ppc
        return
    fi

    [ -x "$UTMCTL" ] || {
        echo "utmctl not found at $UTMCTL -- is UTM installed?" >&2
        echo "To skip the PowerPC build: XPCODE_ARCHS=\"arm64 x86_64\"" >&2
        exit 1
    }

    status=$("$UTMCTL" status "$VM" 2>/dev/null || echo missing)
    if [ "$status" = missing ]; then
        echo "No UTM VM called '$VM'. Set XPCODE_VM to one of:" >&2
        "$UTMCTL" list >&2
        exit 1
    fi
    if [ "$status" != started ]; then
        echo "  VM   starting '$VM' ($status)"
        "$UTMCTL" start "$VM" || true
        # Resuming a suspended VM does not always work -- UTM drops a saved
        # state it cannot restore, leaving the VM stopped and the start
        # looking like it succeeded. Check, rather than sit in the ssh loop
        # below for five minutes waiting for a machine that is not running;
        # and if the saved state is what is broken, throw it away and boot
        # from cold instead, which always works.
        if [ "$("$UTMCTL" status "$VM" 2>/dev/null)" != started ]; then
            echo "  VM   '$VM' did not come up; discarding its saved state"
            "$UTMCTL" discard-state "$VM" 2>/dev/null || true
            "$UTMCTL" start "$VM" || true
        fi
        if [ "$("$UTMCTL" status "$VM" 2>/dev/null)" != started ]; then
            echo "'$VM' will not start. Open it in UTM and look at its" >&2
            echo "console; to build everything else: XPCODE_ARCHS=\"arm64 x86_64\"" >&2
            exit 1
        fi
    else
        echo "  VM   '$VM' already running"
    fi

    # An emulated PowerPC Tiger takes its time to boot, and sshd comes up near
    # the end of it, so wait on ssh answering rather than on the VM's state.
    echo "  VM   waiting for ssh ($VM_SSH)"
    i=0
    while [ "$i" -lt 60 ]; do
        if ssh -o ConnectTimeout=5 -o BatchMode=yes "$VM_SSH" true \
               >/dev/null 2>&1; then
            break
        fi
        i=$((i + 1))
        sleep 5
    done
    if [ "$i" -ge 60 ]; then
        echo "'$VM_SSH' did not answer ssh within five minutes." >&2
        echo "Check the VM's console, and that ~/.ssh/config has a '$VM_SSH'" >&2
        echo "host pointed at its forwarded port." >&2
        exit 1
    fi

    # COPYFILE_DISABLE stops tar turning any xattr a source file has picked up
    # into an AppleDouble "._name" member, which Tiger's tar would unpack as a
    # junk file beside the real one.
    echo "  VM   copying sources to $VM_SSH:$VM_DIR"
    COPYFILE_DISABLE=1 tar czf - src tools/build_mac_cocoa.sh \
        | ssh "$VM_SSH" "rm -rf '$VM_DIR' && mkdir -p '$VM_DIR' \
            && cd '$VM_DIR' && tar xzf -"

    echo "  VM   building ppc on the guest (this takes a while under emulation)"
    ssh "$VM_SSH" "cd '$VM_DIR' && XPCODE_ARCHS=ppc \
        /bin/bash tools/build_mac_cocoa.sh" 2>&1 | sed 's/^/       /'

    echo "  VM   fetching XPicoCode.app"
    rm -rf "$OUT/ppc"
    mkdir -p "$OUT/ppc"
    ssh "$VM_SSH" "cd '$VM_DIR/$OUT/ppc' && tar czf - XPicoCode.app" \
        | tar xzf - -C "$OUT/ppc"
    [ -x "$OUT/ppc/XPicoCode.app/Contents/MacOS/XPicoCode" ] || {
        echo "the guest build produced no binary" >&2
        exit 1
    }
}


# Zip one built bundle under its release name.
package() {
    arch="$1"
    app="$OUT/$arch/XPicoCode.app"
    zip="$DIST/XPicoCode-v$VERSION-$arch-osx.zip"
    echo "  ZIP  $zip"
    rm -f "$zip"
    # ditto rather than zip: it keeps the bundle's symlinks and metadata, and
    # is what every other Mac app is shipped with.
    ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
}


mkdir -p "$OUT" "$DIST"
for arch in $ARCHS; do
    case "$arch" in
        ppc) build_ppc ;;
        *)   compile "$arch" ;;
    esac
done

# Packaging is a separate pass so that a failure in any architecture stops the
# build before a half-finished set of zips is left lying around with the
# version number on it.
#
# On the PowerPC guest this is skipped: the host zips what it fetches, so all
# the archives come out of one ditto, and Tiger's zip need not be involved.
if [ "$(uname -p)" != powerpc ]; then
    for arch in $ARCHS; do package "$arch"; done
fi

echo
echo "Build succeeded:"
for arch in $ARCHS; do
    app="$OUT/$arch/XPicoCode.app"
    printf '  %-8s %s\n' "$arch" "$app"
    if [ "$(uname -p)" != powerpc ]; then
        printf '  %-8s %s\n' "" "$DIST/XPicoCode-v$VERSION-$arch-osx.zip"
    fi
done
echo
echo "  arm64/x86_64: open the .app here. ppc: copy the zip to a 10.4 machine."
