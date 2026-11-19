#!/bin/bash
set -euo pipefail

project=$(cd "$(dirname "$0")/.." && pwd)
architecture=$(dpkg --print-architecture)
version=$(dpkg-parsechangelog --file "$project/debian/changelog" --show-field Version)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/source"
tar -C "$project" --exclude=.git --exclude=build --exclude=dist --exclude=__pycache__ \
    --exclude='*.deb' --exclude='preview*.png' -cf - . | tar -C "$stage/source" -xf -
cd "$stage/source"
dpkg-checkbuilddeps
dpkg-buildpackage -us -uc -b

package="dock-hardware-tuning-plugin_${version}_${architecture}.deb"
dpkg-deb -f "$stage/$package" Package Version Architecture Depends
manifest=$(dpkg-deb --fsys-tarfile "$stage/$package" | tar -tf -)
for file in ./usr/lib/dde-dock/plugins/libdock-hardware-tuning-plugin.so \
            ./usr/libexec/dock-hardware-tuning-plugin-helper \
            ./lib/udev/rules.d/60-dock-hardware-tuning-plugin.rules \
            ./lib/systemd/system/dock-hardware-tuning.service \
            ./usr/share/dbus-1/system.d/io.github.DockHardwareTuning.conf; do
    grep -Fxq "$file" <<< "$manifest"
done
mkdir -p "$project/dist"
install -m644 "$stage/$package" "$project/dist/$package"
cd "$project/dist"
sha256sum "$package" > "${package}.sha256"
