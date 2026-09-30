#!/usr/bin/env bash
# Installs Frame Notify on a Steam Frame, for the current user, in one go:
#
#   curl -fsSL https://raw.githubusercontent.com/Blackskydk/steam-frame-notify/main/scripts/install.sh | bash
#
# It needs no root and changes nothing outside the home directory. It puts the program in
# ~/.local/share/frame-notify, links it as ~/.local/bin/frame-notify, and sets Frame Notify up to
# start by itself when the Frame starts (a systemd user service) and to wait in the background
# for SteamVR.
#
# Run from inside an unpacked release, it installs that release; run on its own, or from the
# installed copy (~/.local/share/frame-notify/install.sh), it downloads the latest one from GitHub
# and checks its checksum first, which is also how it updates.
#
#   install.sh [--no-autostart] [--version vX.Y.Z]      install or update
#   install.sh --download                               download even from inside an unpacked release
#   install.sh --uninstall [--purge]                    remove it (--purge also deletes the
#                                                       notification history and the paired phone)
set -euo pipefail

REPO="${FRAME_NOTIFY_REPO:-Blackskydk/steam-frame-notify}"
ASSET="frame-notify-linux-aarch64.tar.gz"
APP_DIR="${FRAME_NOTIFY_APP_DIR:-$HOME/.local/share/frame-notify}"
BIN_DIR="${FRAME_NOTIFY_BIN_DIR:-$HOME/.local/bin}"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/frame-notify"
UNIT="frame-notify.service"

AUTOSTART=1
DOWNLOAD=0
UNINSTALL=0
PURGE=0
VERSION=""

say() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    # The comment block at the top of this file.
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]:-$0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-autostart) AUTOSTART=0 ;;
        --download) DOWNLOAD=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --purge) PURGE=1 ;;
        --version) shift; VERSION="${1:-}"; [ -n "$VERSION" ] || fail "--version needs a value such as v0.1.0" ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1 (try --help)" ;;
    esac
    shift
done

[ -n "${HOME:-}" ] || fail "HOME is not set"
[ "$(id -u)" -ne 0 ] || [ "${FRAME_NOTIFY_ALLOW_ROOT:-0}" = 1 ] || fail "run this as the normal user of the Frame, not as root: Frame Notify is installed per user"

# `systemctl --user` finds the user's systemd through these; a shell started over SSH may lack them.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

stop_service() {
    if have systemctl; then
        systemctl --user stop "$UNIT" >/dev/null 2>&1 || true
    fi
}

# --- removing --------------------------------------------------------------------------------

uninstall() {
    case "$APP_DIR" in ""|"/"|"$HOME"|"$HOME/") fail "refusing to remove '$APP_DIR'" ;; esac
    say "Removing Frame Notify..."
    if [ -x "$APP_DIR/frame-notify" ]; then
        "$APP_DIR/frame-notify" --disable-autostart || true
    elif have systemctl; then
        systemctl --user disable "$UNIT" >/dev/null 2>&1 || true
    fi
    stop_service
    local config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
    rm -f "$config_home/systemd/user/$UNIT" "$config_home/autostart/frame-notify.desktop"
    if have systemctl; then systemctl --user daemon-reload >/dev/null 2>&1 || true; fi
    if [ -L "$BIN_DIR/frame-notify" ]; then rm -f "$BIN_DIR/frame-notify"; fi
    rm -rf "$APP_DIR"
    if [ "$PURGE" -eq 1 ]; then
        rm -rf "$STATE_DIR"
        say "Deleted the notification history and the remembered phone ($STATE_DIR)."
    else
        say "Kept the notification history and the remembered phone in $STATE_DIR"
        say "(run with --uninstall --purge to delete them too)."
    fi
    say "Frame Notify is removed. The iPhone's pairing with the Frame is untouched; forget it in"
    say "the iPhone's Bluetooth settings if you no longer want it."
}

if [ "$UNINSTALL" -eq 1 ]; then
    uninstall
    exit 0
fi

# --- where the files come from ---------------------------------------------------------------

SOURCE_DIR=""
if [ "$DOWNLOAD" -eq 0 ] && [ -n "${BASH_SOURCE[0]:-}" ]; then
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null && pwd || true)"
    installed="$(cd "$APP_DIR" 2>/dev/null && pwd || true)"
    # The installed copy is not a release to install from: copying it onto itself would destroy it.
    if [ -n "$here" ] && [ "$here" != "$installed" ] && [ -f "$here/frame-notify" ] &&
       [ -d "$here/scripts" ] && [ -d "$here/fonts" ]; then
        SOURCE_DIR="$here"
    fi
fi

if [ -z "$SOURCE_DIR" ]; then
    case "$(uname -m)" in
        aarch64|arm64) ;;
        *) [ "${FRAME_NOTIFY_ALLOW_ANY_ARCH:-0}" = 1 ] ||
               fail "this is for the Steam Frame (aarch64); this machine is $(uname -m)" ;;
    esac
    have tar || fail "tar is needed"
    have sha256sum || fail "sha256sum is needed"
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    if [ -n "${FRAME_NOTIFY_DOWNLOAD_BASE:-}" ]; then
        BASE="$FRAME_NOTIFY_DOWNLOAD_BASE"            # a mirror, or a test
    elif [ -n "$VERSION" ]; then
        BASE="https://github.com/$REPO/releases/download/$VERSION"
    else
        BASE="https://github.com/$REPO/releases/latest/download"
    fi
    fetch() {
        if have curl; then curl -fsSL --retry 3 -o "$2" "$1"
        elif have wget; then wget -q -O "$2" "$1"
        else fail "curl or wget is needed to download Frame Notify"
        fi
    }
    say "Downloading Frame Notify from $REPO..."
    fetch "$BASE/$ASSET" "$TMP/$ASSET" || fail "could not download $BASE/$ASSET"
    fetch "$BASE/$ASSET.sha256" "$TMP/$ASSET.sha256" || fail "could not download the checksum"
    (cd "$TMP" && sha256sum -c "$ASSET.sha256" >/dev/null) || fail "the download does not match its checksum"
    tar -xzf "$TMP/$ASSET" -C "$TMP"
    [ -f "$TMP/frame-notify/frame-notify" ] || fail "the download does not contain the program"
    SOURCE_DIR="$TMP/frame-notify"
fi

# --- installing ------------------------------------------------------------------------------

case "$APP_DIR" in ""|"/"|"$HOME"|"$HOME/") fail "refusing to install into '$APP_DIR'" ;; esac
NEW_VERSION="$(cat "$SOURCE_DIR/VERSION" 2>/dev/null || echo unknown)"
OLD_VERSION="$(cat "$APP_DIR/VERSION" 2>/dev/null || true)"

stop_service
mkdir -p "$APP_DIR" "$BIN_DIR"
rm -rf "$APP_DIR/scripts" "$APP_DIR/fonts"
cp -R "$SOURCE_DIR/scripts" "$APP_DIR/scripts"
cp -R "$SOURCE_DIR/fonts" "$APP_DIR/fonts"
for file in LICENSE README.md VERSION install.sh; do
    if [ -f "$SOURCE_DIR/$file" ]; then cp "$SOURCE_DIR/$file" "$APP_DIR/$file"; fi
done
# A program that is running can be replaced by renaming a new file over it.
cp "$SOURCE_DIR/frame-notify" "$APP_DIR/frame-notify.new"
chmod 755 "$APP_DIR/frame-notify.new" "$APP_DIR/install.sh" 2>/dev/null || true
mv -f "$APP_DIR/frame-notify.new" "$APP_DIR/frame-notify"
ln -sfn "$APP_DIR/frame-notify" "$BIN_DIR/frame-notify"

if [ -n "$OLD_VERSION" ]; then
    say "Updated Frame Notify from $OLD_VERSION to $NEW_VERSION in $APP_DIR"
else
    say "Installed Frame Notify $NEW_VERSION in $APP_DIR"
fi

# --- what it needs ---------------------------------------------------------------------------

if have python3 && python3 -c 'import dbus; from gi.repository import GLib' >/dev/null 2>&1; then
    say "Python with dbus-python and PyGObject was found: pairing an iPhone will work."
else
    warn "Python 3 with the modules dbus-python and PyGObject was not found."
    warn "Frame Notify still runs, but the iPhone Bluetooth helper cannot: pairing will say so."
    warn "Install your system's packages for them (often python-dbus and python-gobject) and run"
    warn "this installer again, or restart Frame Notify."
fi

# --- starting with the Frame -----------------------------------------------------------------

if [ "$AUTOSTART" -eq 1 ]; then
    if "$APP_DIR/frame-notify" --enable-autostart; then
        if have systemctl && systemctl --user is-enabled "$UNIT" >/dev/null 2>&1; then
            if systemctl --user restart "$UNIT"; then
                say "Frame Notify is running in the background."
            else
                warn "could not start the service now; it will start the next time the Frame starts"
            fi
        else
            # No systemd user service here: a desktop autostart entry was made instead.
            if have setsid; then setsid -f "$APP_DIR/frame-notify" >/dev/null 2>&1 || true
            else nohup "$APP_DIR/frame-notify" >/dev/null 2>&1 &
            fi
            say "Frame Notify is running in the background."
        fi
    else
        warn "could not set up starting with the Frame; start it yourself with: frame-notify"
    fi
else
    say "Autostart was skipped. Start Frame Notify with: $BIN_DIR/frame-notify"
    say "Turn autostart on later in its Settings (the gear in the dashboard) or with: frame-notify --enable-autostart"
fi

cat <<EOF

Next:
  1. Start SteamVR and open its dashboard: "Phone Notifications" is in the list.
  2. Tap "Pair an iPhone" and follow the steps on the panel.

Useful:
  frame-notify --autostart-status            is it set to start with the Frame?
  journalctl --user -u frame-notify -f       what it is doing
  $APP_DIR/install.sh --uninstall            remove it again
EOF
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) say "(Add $BIN_DIR to your PATH to run 'frame-notify' by name.)" ;;
esac
