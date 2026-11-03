#!/bin/sh
# rpm.sh - build the .rpm; runs inside a fedora/rockylinux container as root
# (tools/package.sh invokes it; fedora:41 -> .fc41, rockylinux:9 -> .el9).
#
# build on the oldest distro you support: a binary needs the glibc of the
# build host or newer, so the el9 build covers every rhel-9+ derivative and
# the fc41 build covers fedora 41+.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

if [ "$(id -u)" = 0 ]; then
    dnf install -y -q gcc make ca-certificates rpm-build >/dev/null
fi

rm -f flower test/selfcheck   # no make clean: must not wipe dist/
make "VERSION=$VER"
./flower version | grep -q "$VER" || { echo "rpm.sh: version stamp not picked up" >&2; exit 1; }

top=/tmp/rpmbuild
rm -rf "$top"
mkdir -p "$top/BUILD" "$top/BUILDROOT" "$top/RPMS" "$top/SOURCES" "$top/SPECS" "$top/SRPMS"
cat > "$top/SPECS/flower.spec" <<EOF
Name: flower
Version: $VER
Release: 1%{?dist}
Summary: template c project with utilities
License: EUPL-1.2
URL: https://github.com/YOURNAME/flower

%description
flower is a template c project with utilities - greeting, echo,
reverse, wc, self-update subcommands.

%files
%{_bindir}/flower
/usr/share/licenses/flower/LICENSE.md
/usr/share/doc/flower/README.md

%install
install -Dm755 '%{flowersrc}/flower' '%{buildroot}%{_bindir}/flower'
install -Dm644 '%{flowersrc}/LICENSE.md' '%{buildroot}/usr/share/licenses/flower/LICENSE.md'
install -Dm644 '%{flowersrc}/README.md' '%{buildroot}/usr/share/doc/flower/README.md'
EOF

rpmbuild --define "_topdir $top" --define "flowersrc $PWD" \
    -bb "$top/SPECS/flower.spec" >/dev/null
mkdir -p dist
mv "$top"/RPMS/*/*.rpm dist/
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "rpm: $(ls dist/flower-*.rpm)"
