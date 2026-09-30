#!/usr/bin/env bash
# Runs the real packaging and install scripts against a fake program and a fake systemctl, in a
# throwaway home directory: packing, installing from an unpacked release, installing by download
# (with checksum), updating, refusing a damaged download, and uninstalling.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failures=0

check() {   # check "description" command...
    local description="$1"; shift
    if "$@" >/dev/null 2>&1; then :; else echo "FAILED: $description"; failures=$((failures + 1)); fi
}
check_not() {
    local description="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "FAILED: $description"; failures=$((failures + 1)); fi
}
contains() { grep -qF -- "$2" "$1"; }

# Windows' Git Bash cannot make real symbolic links, and its curl needs Windows paths in file URLs;
# neither matters on the Frame.
windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
file_url() {
    if [ "$windows" -eq 1 ] && command -v cygpath >/dev/null 2>&1; then echo "file:///$(cygpath -m "$1")"
    else echo "file://$1"; fi
}

# A fake program, and a fake systemctl that logs what it is asked and says "enabled".
mkdir -p "$work/build" "$work/fakebin" "$work/home"
cat > "$work/build/frame-notify" <<'EOF'
#!/bin/sh
echo "frame-notify $*" >> "$FAKE_LOG"
exit 0
EOF
chmod 755 "$work/build/frame-notify"
cat > "$work/fakebin/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$FAKE_LOG"
exit 0
EOF
chmod 755 "$work/fakebin/systemctl"
export FAKE_LOG="$work/calls.log"
: > "$FAKE_LOG"

# ---- packing ----
out="$work/dist"
bash "$root/scripts/package-release.sh" "$work/build" "9.9.9" "$out" >/dev/null
asset="$out/frame-notify-linux-aarch64.tar.gz"
check "the archive exists" test -f "$asset"
check "the checksum verifies" bash -c "cd '$out' && sha256sum -c frame-notify-linux-aarch64.tar.gz.sha256"
tar -tzf "$asset" > "$work/listing.txt"
for entry in frame-notify/frame-notify frame-notify/install.sh frame-notify/VERSION frame-notify/LICENSE \
             frame-notify/scripts/ancs_bridge.py frame-notify/scripts/ancs_protocol.py \
             frame-notify/scripts/ancs_service.py frame-notify/fonts/Inter-Regular.ttf \
             frame-notify/fonts/Inter-SemiBold.ttf frame-notify/fonts/OFL.txt; do
    check "the archive holds $entry" contains "$work/listing.txt" "$entry"
done
tar -tvzf "$asset" --numeric-owner > "$work/verbose.txt"
check "the archive names no builder" bash -c "! grep -v ' 0/0 ' '$work/verbose.txt' | grep -q '/'"

# ---- installing from an unpacked release ----
mkdir -p "$work/unpacked"
tar -xzf "$asset" -C "$work/unpacked"
run_install() {   # run_install script args...
    HOME="$work/home" XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/home/.config" \
    XDG_STATE_HOME="$work/home/.local/state" PATH="$work/fakebin:$PATH" FRAME_NOTIFY_ALLOW_ROOT=1 \
    FRAME_NOTIFY_ALLOW_ANY_ARCH=1 "$@"
}
mkdir -p "$work/run"
run_install bash "$work/unpacked/frame-notify/install.sh" > "$work/install1.txt" 2>&1
check "the install succeeded" contains "$work/install1.txt" "Installed Frame Notify 9.9.9"
app="$work/home/.local/share/frame-notify"
check "the program is installed" test -x "$app/frame-notify"
check "the helper scripts are installed" test -f "$app/scripts/ancs_service.py"
check "the fonts are installed" test -f "$app/fonts/Inter-Regular.ttf"
check "an uninstaller is kept" test -x "$app/install.sh"
if [ "$windows" -eq 0 ]; then check "the program is linked onto the path" test "$(readlink "$work/home/.local/bin/frame-notify")" = "$app/frame-notify"; fi
check "the program can be run by name" test -e "$work/home/.local/bin/frame-notify"
check "autostart was turned on" contains "$FAKE_LOG" "frame-notify --enable-autostart"
check "the service was started" contains "$FAKE_LOG" "systemctl --user restart frame-notify.service"
check "the service was stopped before files were replaced" contains "$FAKE_LOG" "systemctl --user stop frame-notify.service"
check "it says what to do next" contains "$work/install1.txt" "Pair an iPhone"

# ---- updating ----
echo "changed" >> "$work/unpacked/frame-notify/scripts/ancs_service.py"
printf '10.0.0\n' > "$work/unpacked/frame-notify/VERSION"
echo "stale" > "$app/scripts/old_file.py"
run_install bash "$work/unpacked/frame-notify/install.sh" > "$work/install2.txt" 2>&1
check "it reports the update" contains "$work/install2.txt" "Updated Frame Notify from 9.9.9 to 10.0.0"
check "new files replace the old" contains "$app/scripts/ancs_service.py" "changed"
check "files from the old version are gone" test ! -e "$app/scripts/old_file.py"

# ---- no autostart ----
: > "$FAKE_LOG"
run_install bash "$work/unpacked/frame-notify/install.sh" --no-autostart > "$work/install3.txt" 2>&1
check "autostart can be skipped" bash -c "! grep -q enable-autostart '$FAKE_LOG'"
check "it says how to turn it on later" contains "$work/install3.txt" "--enable-autostart"

# The download needs curl (wget cannot read file: URLs); without it only this part is skipped.
if command -v curl >/dev/null 2>&1; then
    # ---- installing by download, through a pipe, with the checksum checked ----
    rm -rf "$work/home/.local"
    : > "$FAKE_LOG"
    cat "$root/scripts/install.sh" | FRAME_NOTIFY_DOWNLOAD_BASE="$(file_url "$out")" run_install bash -s -- > "$work/install4.txt" 2>&1
    check "the download installs" contains "$work/install4.txt" "Installed Frame Notify 9.9.9"
    if ! contains "$work/install4.txt" "Installed Frame Notify 9.9.9"; then echo "--- download install output"; cat "$work/install4.txt"; fi
    check "the downloaded program is in place" test -x "$app/frame-notify"

    # The installed copy updates from the download; it must not try to install from itself, which
    # would delete the very files it copies.
    echo "stale" > "$app/scripts/old_file.py"
    printf '0.0.1\n' > "$app/VERSION"
    FRAME_NOTIFY_DOWNLOAD_BASE="$(file_url "$out")" run_install bash "$app/install.sh" > "$work/install4b.txt" 2>&1
    check "the installed copy downloads the newest release" contains "$work/install4b.txt" "Downloading Frame Notify"
    check "  and reports the update" contains "$work/install4b.txt" "Updated Frame Notify from 0.0.1 to 9.9.9"
    check "  replacing the old files" test ! -e "$app/scripts/old_file.py"
    check "  and leaving itself whole" test -f "$app/scripts/ancs_service.py" -a -f "$app/fonts/Inter-Regular.ttf" -a -x "$app/install.sh"
    # --download also works from inside an unpacked release.
    FRAME_NOTIFY_DOWNLOAD_BASE="$(file_url "$out")" run_install bash "$work/unpacked/frame-notify/install.sh" --download > "$work/install4c.txt" 2>&1
    check "--download downloads even from an unpacked release" contains "$work/install4c.txt" "Downloading Frame Notify"

    # A damaged download is refused and nothing is installed.
    rm -rf "$work/home/.local" "$work/damaged"
    mkdir -p "$work/damaged"
    cp "$asset" "$asset.sha256" "$work/damaged/"
    printf 'junk' >> "$work/damaged/frame-notify-linux-aarch64.tar.gz"
    cat "$root/scripts/install.sh" | FRAME_NOTIFY_DOWNLOAD_BASE="$(file_url "$work/damaged")" run_install bash -s -- > "$work/install5.txt" 2>&1
    check_not "a damaged download is refused" test "$?" -eq 0
    check "it says why" contains "$work/install5.txt" "checksum"
    check "nothing was installed" test ! -e "$app/frame-notify"

fi

# Not on a Frame's architecture, and not as a download of the wrong thing.
if [ "$(uname -m)" != "aarch64" ] && [ "$(uname -m)" != "arm64" ]; then
    cat "$root/scripts/install.sh" | HOME="$work/home" FRAME_NOTIFY_ALLOW_ROOT=1 bash -s -- > "$work/install6.txt" 2>&1
    check "an other architecture is refused" contains "$work/install6.txt" "Steam Frame"
fi

# ---- uninstalling ----
rm -rf "$work/home/.local"
run_install bash "$work/unpacked/frame-notify/install.sh" > /dev/null 2>&1
mkdir -p "$work/home/.local/state/frame-notify"
echo "{}" > "$work/home/.local/state/frame-notify/history.jsonl"
mkdir -p "$work/home/.config/systemd/user"
echo "[Unit]" > "$work/home/.config/systemd/user/frame-notify.service"
: > "$FAKE_LOG"
run_install bash "$app/install.sh" --uninstall > "$work/uninstall1.txt" 2>&1
check "autostart is turned off" contains "$FAKE_LOG" "frame-notify --disable-autostart"
check "the program is removed" test ! -e "$app"
if [ "$windows" -eq 0 ]; then check "the link is removed" test ! -e "$work/home/.local/bin/frame-notify"; fi
check "a leftover service file is removed" test ! -e "$work/home/.config/systemd/user/frame-notify.service"
check "the history is kept" test -f "$work/home/.local/state/frame-notify/history.jsonl"
check "it says the history was kept" contains "$work/uninstall1.txt" "Kept the notification history"
run_install bash "$work/unpacked/frame-notify/install.sh" > /dev/null 2>&1
run_install bash "$app/install.sh" --uninstall --purge > "$work/uninstall2.txt" 2>&1
check "purging deletes the history" test ! -e "$work/home/.local/state/frame-notify"
run_install bash "$root/scripts/install.sh" --uninstall > "$work/uninstall3.txt" 2>&1
check "uninstalling twice is harmless" contains "$work/uninstall3.txt" "Frame Notify is removed"

# ---- options ----
check_not "an unknown option is an error" run_install bash "$root/scripts/install.sh" --frobnicate
check "help works" run_install bash "$root/scripts/install.sh" --help
run_install bash "$root/scripts/install.sh" --help > "$work/help.txt" 2>&1
check "help shows the options, and only the options" bash -c "grep -q -- '--uninstall' '$work/help.txt' && grep -q -- '--download' '$work/help.txt' && ! grep -q 'set -euo' '$work/help.txt' && ! grep -q '^REPO=' '$work/help.txt'"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    echo "--- last install output"; cat "$work/install1.txt" 2>/dev/null
    exit 1
fi
echo "install scripts OK"
