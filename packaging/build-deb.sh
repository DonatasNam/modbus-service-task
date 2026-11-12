#!/usr/bin/env bash
# Builds dist/modbus-poller_<version>_<arch>.deb without root: a Release build, a DESTDIR install into a staging
# tree, DEBIAN/ from packaging/debian, then dpkg-deb with root ownership recorded in the archive.
set -euo pipefail
umask 022

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

version="$(sed -n 's/^project(modbus-poller VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
arch="$(dpkg --print-architecture)"
build="build/package"
stage="$root/$build/stage"
deb="dist/modbus-poller_${version}_${arch}.deb"

rm -rf "$stage"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build" -j "$(nproc)"
DESTDIR="$stage" cmake --install "$build"

install -d -m 0755 "$stage/DEBIAN"
sed -e "s/@VERSION@/$version/" -e "s/@ARCH@/$arch/" packaging/debian/control.in > "$stage/DEBIAN/control"
chmod 0644 "$stage/DEBIAN/control"
install -m 0644 packaging/debian/conffiles "$stage/DEBIAN/conffiles"
for script in postinst prerm postrm; do
    install -m 0755 "packaging/debian/$script" "$stage/DEBIAN/$script"
done

mkdir -p dist
dpkg-deb --root-owner-group --build "$stage" "$deb"
echo "$deb"
