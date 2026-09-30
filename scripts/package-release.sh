#!/usr/bin/env bash
# Packs a built Frame Notify into the release archive that scripts/install.sh installs:
#
#   package-release.sh BUILD_DIR [VERSION] [OUT_DIR]
#
# BUILD_DIR holds the built `frame-notify`. The archives land in OUT_DIR (default ./dist):
#   frame-notify-linux-aarch64.tar.gz (+ .sha256)   what scripts/install.sh downloads
#   frame-notify-linux-aarch64.zip                  the same plus a `setup-on-launch` marker, for
#                                                   FrameDrop (see framedrop.json and the README)
# The names carry no version, so that ".../releases/latest/download/<name>" always finds the newest.
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

# The zip is for tools that unpack it on another machine and launch the program. The marker tells
# the program to run the installer next to it instead of running from where it was dropped.
python=""
for candidate in python3 python py; do
    if "$candidate" -c 'import zipfile' >/dev/null 2>&1; then python="$candidate"; break; fi
done
if [ -n "$python" ]; then
    printf 'Frame Notify sets itself up when it is started from this folder: it runs install.sh.\n' > "$package/setup-on-launch"
    "$python" "$root/scripts/make-zip.py" "$package" "$out_dir/frame-notify-linux-aarch64.zip"
else
    echo "warning: no Python found; the FrameDrop zip was not made" >&2
fi
