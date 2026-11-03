#!/bin/sh
# win.sh - build flower.exe; runs inside the mingw cross container
# (africanfuture/msys2-base:linux-latest, tools/package.sh invokes it).
#
# that image is arch linux with the mingw-w64 cross toolchain
# (/usr/bin/x86_64-w64-mingw32-gcc, sysroot /usr/x86_64-w64-mingw32,
# static winpthreads) - a linux-hosted cross compiler, not an msys2
# runtime. the project has zero external dependencies, so no third-party
# code is cross-built: pacman installs the toolchain only and make runs
# once. the result is a plain 64-bit windows console exe for windows 10+;
# update check + install go through the curl *binary* at runtime, not
# a statically linked http library.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

# ---- toolchain ---------------------------------------------------------------
# the image also carries a native linux gcc - never that one; CC= overrides
cc=${CC:-x86_64-w64-mingw32-gcc}
command -v "$cc" >/dev/null 2>&1 || { echo "win.sh: no $cc on PATH" >&2; exit 1; }
trip=$("$cc" -dumpmachine)
case "$trip" in
    *w64-mingw*) ;;
    *) echo "win.sh: $cc targets $trip, want *-w64-mingw32" >&2; exit 1 ;;
esac
command -v make >/dev/null 2>&1 || { echo "win.sh: no make" >&2; exit 1; }
strip_cmd="$trip-strip"
command -v "$strip_cmd" >/dev/null 2>&1 || strip_cmd=""

if [ "$(id -u)" = 0 ]; then
    pacman -Sy --needed --noconfirm mingw-w64-cross-gcc make >/dev/null
fi

# ---- build -------------------------------------------------------------------
rm -f flower flower.exe test/selfcheck test/selfcheck.exe  # no make clean: must not wipe dist/
# -static: no dlls next to the exe. -mconsole is the default subsystem;
# __USE_MINGW_ANSI_STDIO keeps printf fully c11-conformant.
make "VERSION=$VER" CC="$cc" \
    "EXTRA_CFLAGS=-static -D__USE_MINGW_ANSI_STDIO=1"
exe=flower.exe
[ -f "$exe" ] || exe=flower
# a PE binary does not run on the linux host - the stamp check survives
# without wine, it just cannot verify
out=$(./"$exe" version 2>/dev/null || true)
if [ -n "$out" ]; then
    printf '%s' "$out" | grep -q "$VER" \
        || { echo "win.sh: version stamp not picked up" >&2; exit 1; }
else
    echo "win.sh: warning: cannot run $exe on this host - stamp not verified" >&2
fi
[ -z "$strip_cmd" ] || "$strip_cmd" "$exe"

# ---- pack --------------------------------------------------------------------
arch=$(printf '%s' "$trip" | cut -d- -f1)
stage="flower-$VER-windows-$arch"
rm -rf "dist/$stage"
mkdir -p "dist/$stage"
cp "$exe" LICENSE.md README.md "dist/$stage/"
if command -v zip >/dev/null 2>&1; then
    (cd dist && zip -q -r "$stage.zip" "$stage")
else
    (cd dist && python3 -c "import shutil; shutil.make_archive('$stage', 'zip', '.', '$stage')")
fi
rm -rf "dist/$stage"
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "win: dist/$stage.zip"
