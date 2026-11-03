#!/bin/sh
# mac.sh - build flower for macOS x86_64; runs inside the osxcross
# container (image built by tools/pkg/mac.Dockerfile, tools/package.sh
# invokes it).
#
# the container is ubuntu + the osxcross toolchain from crazymax/osxcross
# (clang wrapper o64-clang, cctools ld64/ar/ranlib/strip/lipo, the
# packaged MacOSX 11.3 SDK; osxcross built with OSX_VERSION_MIN=10.13).
# the project has zero external dependencies, so no third-party code is
# cross-built: make runs once against the cross compiler.
#
# the result is not fully static - no darwin binary is - it links the
# system libSystem only, so the zip holds a portable binary: macOS
# 10.13+ x86_64, apple silicon via rosetta 2.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

# ---- toolchain ---------------------------------------------------------------
# never the native linux clang; CC= overrides
cc=${CC:-o64-clang}
command -v "$cc" >/dev/null 2>&1 || { echo "mac.sh: no $cc on PATH" >&2; exit 1; }
trip=$("$cc" -dumpmachine)
case "$trip" in
    *-apple-darwin*) ;;
    *) echo "mac.sh: $cc targets $trip, want *-apple-darwin" >&2; exit 1 ;;
esac
command -v make >/dev/null 2>&1 || { echo "mac.sh: no make" >&2; exit 1; }
strip_cmd="$trip-strip"
command -v "$strip_cmd" >/dev/null 2>&1 || strip_cmd=""
minver=10.13

# ---- build -------------------------------------------------------------------
rm -f flower flower.exe test/selfcheck test/selfcheck.exe  # no make clean: must not wipe dist/
# the SDK ships libSystem and nothing else is needed; -mmacosx-version-min
# stamps the 10.13 floor into every translation unit. the ld64 driver in
# the wrapper resolves the -pthread flag to the darwin way (no separate
# libpthread), so the Makefile's LDLIBS needs no override.
make "VERSION=$VER" CC="$cc" "EXTRA_CFLAGS=-mmacosx-version-min=$minver"
bin=flower
[ -f "$bin" ] || { echo "mac.sh: $bin missing after build" >&2; exit 1; }
# a mach-o binary does not run on the linux host - the stamp check
# survives without a mac, it just cannot verify
out=$(./"$bin" version 2>/dev/null || true)
if [ -n "$out" ]; then
    printf '%s' "$out" | grep -q "$VER" \
        || { echo "mac.sh: version stamp not picked up" >&2; exit 1; }
else
    echo "mac.sh: warning: cannot run $bin on this host - stamp not verified" >&2
fi
[ -z "$strip_cmd" ] || "$strip_cmd" "$bin"
file_out=$(file "$bin" 2>/dev/null || true)
printf '%s\n' "$file_out" | grep -q "Mach-O" \
    || { echo "mac.sh: $bin is not a Mach-O binary: $file_out" >&2; exit 1; }

# ---- pack --------------------------------------------------------------------
arch=$(printf '%s' "$trip" | cut -d- -f1)
stage="flower-$VER-macos-$arch"
rm -rf "dist/$stage"
mkdir -p "dist/$stage"
cp "$bin" LICENSE.md README.md "dist/$stage/"
if command -v zip >/dev/null 2>&1; then
    (cd dist && zip -q -r "$stage.zip" "$stage")
else
    (cd dist && python3 -c "import shutil; shutil.make_archive('$stage', 'zip', '.', '$stage')")
fi
rm -rf "dist/$stage"
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "mac: dist/$stage.zip"
