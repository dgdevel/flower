#!/bin/sh
# deb.sh - build the .deb; runs inside an ubuntu/debian container as root
# (tools/package.sh invokes it; can also run natively with dpkg-deb present).
#
# built on ubuntu:22.04 (glibc 2.35): installs on ubuntu 22.04/24.04+ and
# debian 12+ - newer glibc runs older binaries, so build on the oldest
# distro you want to support.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

export DEBIAN_FRONTEND=noninteractive
if [ "$(id -u)" = 0 ]; then
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential >/dev/null
fi

rm -f flower test/selfcheck   # no make clean: must not wipe dist/
make "VERSION=$VER"
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
Maintainer: YOURNAME <YOURNAME@users.noreply.github.com>
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Depends: libc6
Section: utils
Priority: optional
Homepage: https://github.com/YOURNAME/flower
Description: template c project with utilities
 flower is a template c project with utilities - greeting, echo,
 reverse, wc, self-update subcommands.
EOF

mkdir -p dist
dpkg-deb --build --root-owner-group "$root" "dist/flower_${VER}-1_${arch}.deb" >/dev/null
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "deb: dist/flower_${VER}-1_${arch}.deb"
