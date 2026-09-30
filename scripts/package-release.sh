#!/usr/bin/env bash
# Packs a built Frame Notify into the release archive that scripts/install.sh installs:
#
#   package-release.sh BUILD_DIR [VERSION] [OUT_DIR]
#
# BUILD_DIR holds the built `frame-notify`. The archive and its checksum land in OUT_DIR (default
# ./dist) as frame-notify-linux-aarch64.tar.gz and ....tar.gz.sha256; the names carry no version so
# that ".../releases/latest/download/<name>" always finds the newest one.
set -euo pipefail

build_dir="${1:?usage: package-release.sh BUILD_DIR [VERSION] [OUT_DIR]}"
version="${2:-unknown}"
out_dir="${3:-dist}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
asset="frame-notify-linux-aarch64.tar.gz"

[ -f "$build_dir/frame-notify" ] || { echo "error: no frame-notify in $build_dir" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
package="$stage/frame-notify"
mkdir -p "$package/scripts" "$package/fonts"

install -m 755 "$build_dir/frame-notify" "$package/frame-notify"
for script in ancs_bridge.py ancs_protocol.py ancs_service.py send-test-notification.py; do
    install -m 644 "$root/scripts/$script" "$package/scripts/$script"
done
for font in Inter-Regular.ttf Inter-SemiBold.ttf OFL.txt README.md; do
    install -m 644 "$root/src/ui/fonts/$font" "$package/fonts/$font"
done
install -m 755 "$root/scripts/install.sh" "$package/install.sh"
install -m 644 "$root/LICENSE" "$package/LICENSE"
install -m 644 "$root/README.md" "$package/README.md"
printf '%s\n' "$version" > "$package/VERSION"

mkdir -p "$out_dir"
# Owner and group are normalised, so the archive says nothing about who built it.
tar --owner=0 --group=0 --numeric-owner -C "$stage" -czf "$out_dir/$asset" frame-notify
(cd "$out_dir" && sha256sum "$asset" > "$asset.sha256")
echo "Packed $out_dir/$asset ($version)"
