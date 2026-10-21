#!/bin/sh
# arch.sh - build the .pkg.tar.zst with makepkg; runs inside an
# archlinux:base-devel container as root (tools/package.sh invokes it) or
# natively as any non-root user with gcc/make/makepkg installed.
#
# proper pacman package (pacman -U), not a plain tarball: has .PKGINFO,
# .MTREE, license and docs. for AUR, publish a PKGBUILD pointing at the
# release tarball instead.
#
# ported from llmkit (dgdevel/llmkit tools/pkg/arch.sh).
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

if [ "$(id -u)" = 0 ]; then
    pacman -Sy --needed --noconfirm curl gcc make ca-certificates >/dev/null
fi

sh tools/pkg/cjson.sh
# no make clean (must not wipe dist/): drop binaries and the host's
# stale objects alike - .o reuse would lose the version stamp
rm -f flower test/check src/*.o
make "VERSION=$VER" "CJSON=/tmp/cj/libcjson.a" "EXTRA_CFLAGS=-I/tmp/cj/include"
./flower version | grep -q "$VER" || { echo "arch.sh: version stamp not picked up" >&2; exit 1; }

# makepkg refuses to run as root; in a container build as a throwaway user
build=/tmp/pkgbuild
rm -rf "$build"
mkdir -p "$build"
cat > "$build/PKGBUILD" <<EOF
pkgname=flower
pkgver=$VER
pkgrel=1
pkgdesc='llm-assisted project tracker: projects, tasks and actions in plain files worked by llm conversations'
arch=(x86_64)
url='https://github.com/dgdevel/flower'
license=('EUPL-1.2')
depends=(curl)
options=(!debug)

package() {
    install -Dm755 "$PWD/flower" "\$pkgdir/usr/bin/flower"
    install -Dm644 "$PWD/LICENSE.md" "\$pkgdir/usr/share/licenses/flower/LICENSE.md"
    install -Dm644 "$PWD/README.md" "\$pkgdir/usr/share/doc/flower/README.md"
}
EOF

if [ "$(id -u)" = 0 ]; then
    useradd -m builder 2>/dev/null || true
    chown -R builder "$build"
    chmod -R a+rX .
    su builder -c "cd '$build' && PKGEXT='.pkg.tar.zst' makepkg -f"
    chown -R "$(stat -c '%u:%g' Makefile)" "$build"
else
    (cd "$build" && PKGEXT='.pkg.tar.zst' makepkg -f)
fi
mkdir -p dist
mv "$build"/flower-*.pkg.tar.zst dist/
echo "arch: $(ls dist/flower-*.pkg.tar.zst)"
