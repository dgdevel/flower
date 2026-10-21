#!/bin/sh
# deb.sh - build the .deb; runs inside an ubuntu/debian container as root
# (tools/package.sh invokes it; can also run natively with dpkg-deb present).
#
# built on ubuntu:22.04 (glibc 2.35): installs on ubuntu 22.04/24.04+ and
# debian 12+ - newer glibc runs older binaries, so build on the oldest
# distro you want to support.
#
# ported from llmkit (dgdevel/llmkit tools/pkg/deb.sh).
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

export DEBIAN_FRONTEND=noninteractive
if [ "$(id -u)" = 0 ]; then
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential libcurl4-openssl-dev curl ca-certificates >/dev/null
fi

sh tools/pkg/cjson.sh
# no make clean (must not wipe dist/): drop binaries and the host's
# stale objects alike - .o reuse would lose the version stamp
rm -f flower test/check src/*.o
make "VERSION=$VER" "CJSON=/tmp/cj/libcjson.a" "EXTRA_CFLAGS=-I/tmp/cj/include"
./flower version | grep -q "$VER" || { echo "deb.sh: version stamp not picked up" >&2; exit 1; }

arch=$(dpkg --print-architecture)
root=/tmp/debroot
rm -rf "$root"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/flower"
install -m 755 flower "$root/usr/bin/flower"
install -m 644 LICENSE.md "$root/usr/share/doc/flower/copyright"
install -m 644 README.md "$root/usr/share/doc/flower/README.md"
cat > "$root/DEBIAN/control" <<EOF
Package: flower
Version: $VER-1
Architecture: $arch
Maintainer: dgdevel <dgdevel@users.noreply.github.com>
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Depends: libcurl4
Section: utils
Priority: optional
Homepage: https://github.com/dgdevel/flower
Description: llm-assisted project tracker
 flower keeps project data in plain files on your computer and works tasks
 and actions in llm conversations: project scan, task create and update,
 action resume, every write approved by you.
EOF

mkdir -p dist
dpkg-deb --build --root-owner-group "$root" "dist/flower_${VER}-1_${arch}.deb" >/dev/null
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "deb: dist/flower_${VER}-1_${arch}.deb"
