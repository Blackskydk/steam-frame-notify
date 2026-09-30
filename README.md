# Frame Notify

Frame Notify shows the notifications of your iPhone inside SteamVR on a Valve Steam Frame: a toast
when one arrives, and a **Phone Notifications** panel in the dashboard with the recent ones. You pair
the iPhone from the panel, in the headset. Frame Notify is an open-source, windowless OpenVR
overlay; the current version implements:

1. OpenVR initialization as `VRApplication_Overlay`, with runtime diagnostics.
2. A native SteamVR dashboard entry named **Phone Notifications**, created through the public
   `IVROverlay::CreateDashboardOverlay` API.
3. A generated 256x256 RGBA bell tile uploaded with `SetOverlayRaw`, with a red badge showing the
   number of unread notifications that updates as they arrive and are read.
4. A modern notification panel drawn by a small built-in software renderer: anti-aliased Inter
   typography (kerning, accented Latin letters, an emoji placeholder), rounded cards with
   per-app colours and monogram avatars, unread state, relative times, notifications grouped by
   day, an expander for long messages, and smooth crop-based scrolling without repeated texture
   uploads.
5. An opt-in test of SteamVR's public native notification API.
6. A user-local Unix socket that accepts newline-delimited JSON notifications, deduplicates IDs,
   shows native SteamVR toasts, and updates the dashboard history.
7. Atomic persistent history under the user's XDG state directory, with count and age retention.
8. Dashboard pointer interaction: tap a card to expand a long message, tap the round × to clear
   one notification, or use **Clear all**. Unread state clears when the dashboard closes.
9. Notifications from an iPhone over Bluetooth (BlueZ/ANCS), with no terminal involved: Frame
   Notify starts a small Bluetooth helper itself, shows the phone's status in the panel's header,
   and pairs a phone from the panel, including the code comparison. The helper also asks the
   phone for each app's real display name ("Messages", "ntfy", ...).
10. Plug-and-play installation: one command installs a prebuilt release for the current user, and
    Frame Notify then starts with the Frame and keeps running in the background. It keeps the iPhone
    connected and collects notifications whether SteamVR is running or not, and the panel and toasts
    appear whenever SteamVR is. Autostart can also be switched in the panel's Settings.

The Bluetooth helper has been verified on a physical iPhone and Steam Frame. See
[the Bluetooth and ANCS guide](docs/ancs.md) and [the API review](docs/openvr-api-notes.md).

## Install on the Steam Frame

In a terminal on the Frame (desktop mode, or over SSH), run:

```bash
curl -fsSL https://raw.githubusercontent.com/Blackskydk/steam-frame-notify/main/scripts/install.sh | bash
```

That downloads the latest release, checks its checksum, installs it for your user (no root, nothing
outside your home directory), and sets Frame Notify up to **start by itself when the Frame starts and
keep running in the background**, waiting for SteamVR. To update, run the same command again.

Then start SteamVR, open the dashboard, select **Phone Notifications** and tap **Pair an iPhone**
(see [Pair an iPhone](#pair-an-iphone-and-receive-its-notifications)). The iPhone Bluetooth helper
needs Python 3 with `dbus-python` and PyGObject, which the installer checks for and tells you about;
everything else works without them.

| To | Do |
| --- | --- |
| Turn starting with the Frame on or off | The gear in the panel's header (Settings), or `frame-notify --enable-autostart` / `--disable-autostart` |
| See whether it starts with the Frame | `frame-notify --autostart-status` |
| Watch what it is doing | `journalctl --user -u frame-notify -f` |
| Stop it for now | `systemctl --user stop frame-notify` |
| Remove it | `~/.local/share/frame-notify/install.sh --uninstall` (add `--purge` to delete the notification history and the remembered phone too) |

How it runs: the program is a systemd *user* service (`~/.config/systemd/user/frame-notify.service`),
so it starts when your user session does, which is at boot on the Frame, and is restarted if it
crashes. Where there is no systemd user manager it uses a desktop autostart entry instead. While
SteamVR is not running it only keeps the iPhone connection and the history; it asks now and then
whether SteamVR is up, never starts SteamVR itself, and shows the panel as soon as SteamVR runs. When
SteamVR quits it starts over and waits for the next time. The close button on the panel's control bar
only removes the dashboard entry until SteamVR restarts; it does not stop Frame Notify.

If you would rather build it yourself, see [Build on Steam Frame](#build-on-steam-frame).

## Safety and scope

The overlay runs as the current user. It does not modify SteamOS, BlueZ configuration, FFmpeg,
SteamVR, or system libraries, and it opens no X11 or Wayland window. The installer writes only in
your home directory (`~/.local/share/frame-notify`, a link in `~/.local/bin`, and, for autostart, a
user service file in `~/.config/systemd/user`); `install.sh --uninstall` removes all of it. It never
starts SteamVR.

The Bluetooth helper (`scripts/ancs_bridge.py --service`, started and stopped by Frame Notify)
talks only to the one phone you paired through the panel, which it remembers in `phone.json` next
to the notification history. It opens a pairing window (a temporary BlueZ pairing agent, a
temporary advertisement named "Frame", and the adapter's `Pairable` setting for five minutes) only
while you have pressed **Pair an iPhone**, and puts everything back afterwards. The only pairings
it removes are one you chose to remove or forget by pressing that phone's button, and a pairing
made in the same session that turned out unusable (classic-only); headsets, mice and other devices
are never offered. Start Frame Notify with `--no-bluetooth` (or
`FRAME_NOTIFY_NO_BLUETOOTH=1`) to leave Bluetooth alone entirely.

## Build on Steam Frame

Place this repository at `~/frame-notify`, then use either Valve's supplied ARM64 OpenVR SDK or
the pinned public OpenVR source.

With a supplied SDK root containing `headers/openvr.h` and an ARM64 OpenVR library:

```bash
cd ~/frame-notify
OPENVR_SDK_ROOT="$HOME/path/to/openvr-sdk" bash ./scripts/build-frame.sh
```

If the header and library were supplied separately:

```bash
cd ~/frame-notify
OPENVR_INCLUDE_DIR="$HOME/path/to/openvr/headers" \
OPENVR_LIBRARY="$HOME/path/to/libopenvr_api.so" \
bash ./scripts/build-frame.sh
```

The CMake configuration inspects an explicitly supplied ELF library and refuses to link an
x86-64 library into an AArch64 build. If no SDK variables are set, CMake fetches the pinned
OpenVR v2.15.6 tag and builds `openvr_api` from source for the Frame's native architecture. That
fallback needs Git/network access but still installs nothing system-wide.

Output:

```text
~/frame-notify/build-frame/frame-notify
```

## Run on Steam Frame

This section is for running a build from the source tree by hand. If Frame Notify is installed and
running in the background (see above), stop it first with `systemctl --user stop frame-notify`: only
one can run at a time, and a second one says so and exits. The development scripts
(`push-frame.ps1 -Run`) stop the service for you.

Frame Notify waits for SteamVR when it is not running, and connects when it starts. First prove
initialization and inspect the reported runtime path/version:

```bash
cd ~/frame-notify
./build-frame/frame-notify --diagnostics-only
```

Expected fields include:

```text
[OpenVR] Runtime initialized
[OpenVR] Runtime version: ...
[OpenVR] HMD connected: yes
[OpenVR] Application type: Overlay
```

Only after that succeeds, start Frame Notify and leave it running:

```bash
cd ~/frame-notify
./build-frame/frame-notify
```

In the headset, open the SteamVR Dashboard and select **Phone Notifications**. SteamVR decides its
ordering. The main view displays notifications received through the local socket, newest first and
grouped by day; use the dashboard pointer's scroll input to reach older entries. Unread cards are
brighter and show a coloured dot; they count as read once the dashboard is closed. To scroll, hold
the trigger anywhere on the panel and drag; let go while moving and the list keeps gliding, and
touching it stops it. A quick tap, without dragging, acts on what is under the pointer: on a card
it expands a long message (a small chevron marks cards that have more text) and a second tap
collapses it. The round **×** on a card clears that notification and **Clear all** in the header
clears every visible one. The gear in the header opens the settings. The SteamVR control bar below
the panel can be grabbed to move the window and includes a close button; using close removes the
dashboard entry until SteamVR restarts, and Frame Notify itself keeps running.
Clearing is persisted locally but does not dismiss the corresponding notification on the phone.

The panel needs the bundled Inter fonts. The build copies them to `build-frame/fonts/`; the program
also looks in `src/ui/fonts/` of the source tree, or in `$FRAME_NOTIFY_FONT_DIR` if you set it. If
they cannot be found it prints a warning and falls back to a plain built-in bitmap font.
Press `Ctrl+C` in SSH to stop from the terminal instead.

To ask SteamVR to create a real native test notification, start the application with:

```bash
cd ~/frame-notify
./build-frame/frame-notify --native-notification
```

The program prints the exact `CreateNotification` result and notification ID, then remains running
so notification-related OpenVR events can be observed. Testing on SteamVR 2.17.10 confirmed that
the toast appears, but it is not interactive and is not retained in SteamVR's notification list.

## Send a local notification

While Frame Notify is running, open a second SSH session to the Frame and send a test event:

```bash
cd ~/frame-notify
python3 ./scripts/send-test-notification.py \
  "Messages" "Jane Doe" "Hello from the local notification API"
```

The helper connects to `$XDG_RUNTIME_DIR/frame-notify.sock`. A new ID produces a native SteamVR
toast and is inserted at the top of the dashboard history. Reusing an ID with `--id test-001` is
ignored, preventing duplicate notifications.

Fill the panel with eight distinct demo notifications for interaction and scrolling tests:

```bash
python3 ./scripts/send-test-notification.py --demo
```

Pass a number from 1 through 50 to choose a different amount, for example `--demo 12`.

The socket accepts one JSON object per line:

```json
{"type":"notification","id":"test-001","app":"Messages","title":"Jane Doe","message":"Are you coming home soon?","timestamp":"2026-09-29T17:02:00+02:00"}
```

An optional string field `"toast":"false"` keeps a notification in local history without a
native SteamVR toast. The ANCS bridge marks notifications the iPhone flagged as silent with
`"silent":"true"` and still shows a toast for them; set `FRAME_NOTIFY_QUIET_NO_TOAST=1` to keep
those silent, in the history only.

`"timestamp"` is shown on the card when it is an ISO 8601 date-time such as
`2026-09-29T17:02:00+02:00` or `...Z` (a time without a zone is read as the Frame's local time). It
is never shown as later than now. Anything else, such as `"now"`, falls back to the moment Frame
Notify received the notification. Recent notifications read "Just now" or "12 min ago", older ones
show the clock time in the Frame's time zone. The startup log prints that zone's idea of the time,
for example `[UI] Local time: 2026-09-30 09:41:05 (UTC+02:00)`. If it is wrong, either start Frame
Notify with the zone you want, which changes nothing else on the Frame:

```bash
TZ=Europe/Berlin ./build-frame/frame-notify
```

or fix the Frame's own time zone in its system settings (with sudo,
`timedatectl set-timezone Europe/Berlin`).

An optional string field `"app_id"` carries the sending app's bundle identifier, for example
`"com.apple.MobileSMS"`. `"app"` should be the readable name; `"app_id"` only selects the card's
colour, so a localized name such as "Nachrichten" still gets Messages' green. If a sender passes a
bundle identifier as `"app"`, the panel turns it into a readable name.

The socket is created with user-only permissions and removed when the process exits. History is
stored atomically in `$XDG_STATE_HOME/frame-notify/history.jsonl`, falling back to
`~/.local/state/frame-notify/history.jsonl`. The directory is user-only, as is the history file.

Retention defaults to the newest 20 notifications and 30 days. Override either value before
launching the application:

```bash
export FRAME_NOTIFY_MAX_NOTIFICATIONS=30
export FRAME_NOTIFY_MAX_AGE_DAYS=14
./build-frame/frame-notify
```

Allowed ranges are 1–50 notifications and 1–365 days. Stored records contain the notification ID,
source application, title, message, timestamp, received time, and future-facing read/dismissed
flags. No unrelated device or account data is stored.

If initialization fails, the executable prints both the public OpenVR error symbol and its English
description. If overlay creation or thumbnail upload fails, it prints the numeric overlay error and
Valve's error name.

## Pair an iPhone and receive its notifications

Everything happens in the headset. Start Frame Notify as usual, open the dashboard and select
**Phone Notifications**. The header shows the phone's state: **Pair an iPhone** while none is
paired, then **Connecting…**, **iPhone connected**, or **Needs attention**. Tap it at any time for
details and actions.

1. Tap **Pair an iPhone** (the big button on an empty panel, or the status in the header).
2. If the Frame already has paired phones, the panel lists them. A leftover pairing keeps an iPhone
   on classic Bluetooth, which cannot carry notifications, so remove yours with **Remove …** (and
   then choose **Forget This Device** for the Frame in the iPhone's Bluetooth settings), or choose
   **My iPhone isn't listed**.
3. On the iPhone open Settings > Bluetooth and tap **Frame** under Other Devices. The panel counts
   down the five minutes it waits.
4. The panel shows a six-digit code. If it matches the one on the iPhone, tap **Yes, it matches**
   on the panel and accept on the iPhone. If the dashboard is closed when the code appears, a
   SteamVR toast tells you to open it.
5. The panel reports the result and one last step, which is not optional: the iPhone sends nothing
   until you switch it on. In Settings > Bluetooth tap the (i) next to **Frame** and turn on
   **Share System Notifications**. The iPhone never asks for this by itself, and after pairing
   again the switch starts off. If it is not there yet, turn the iPhone's Bluetooth off and on.
   Then tap **Done**. The phone is remembered, so later starts connect on their own.

If something is missing, the panel says what and what to do: turn Bluetooth on (one tap), switch
on notification sharing for the Frame in the iPhone's Bluetooth settings, or pair again. **Forget
this phone** (on the phone's screen) stops listening and removes its pairing from the Frame.

The helper needs `python3` with `dbus-python` and PyGObject on the Frame, like the rest of the
Frame's Python tooling; the panel reports it when they are missing. Settings, all optional:

| Setting | Effect |
| --- | --- |
| `--no-bluetooth` or `FRAME_NOTIFY_NO_BLUETOOTH=1` | Do not start the helper. |
| `FRAME_NOTIFY_PYTHON=/path/to/python3` | Interpreter for the helper (default `python3`). |
| `FRAME_NOTIFY_BRIDGE=/path/to/ancs_bridge.py` | Helper script, when it is not in `scripts/` beside the build directory. |
| `FRAME_NOTIFY_PANEL_WIDTH=1.5` | Width of the dashboard panel in metres (0.8 to 4; the height follows). |
| `FRAME_NOTIFY_QUIET_NO_TOAST=1` | No toast for notifications the iPhone flags as silent (they still go in the list). |

The pairing screens hang from the top of the panel, so their buttons stay high in the dashboard.
Like the notification list, a screen can also be dragged up (hold the trigger and pull) if the
panel sits low in your view.

The helper's log lines appear in the terminal Frame Notify runs in. The command-line modes of the
script in [docs/ancs.md](docs/ancs.md) are still there for diagnosing Bluetooth problems; do not
run `--pair` or `--device` while Frame Notify's helper is running.

## Deploy sources over SSH

### Recommended Windows workflow

From PowerShell in the repository, one command packages only the source files, uploads them,
rebuilds on the Frame, and runs the tests:

```powershell
cd "C:\path\to\frame-notify"
.\scripts\push-frame.ps1
```

Add `-Run` to start the newly built overlay in the same interactive terminal:

```powershell
.\scripts\push-frame.ps1 -Run
```

For the native SteamVR notification experiment, push, build, test, and run it with one command:

```powershell
.\scripts\push-frame.ps1 -NativeNotification
```

The defaults are `steamos@frame` and `~/frame-notify`. They can be overridden with environment
variables without editing the script:

```powershell
$env:FRAME_HOST = "steamos@192.0.2.10"
$env:FRAME_REMOTE_DIR = "frame-notify"
.\scripts\push-frame.ps1 -Run
```

The script uses the Windows-provided `tar`, `scp`, and `ssh` commands. It does not require rsync,
Git Bash, root access, or changes to SteamOS. The remote `build-frame` directory is retained, so
later builds are incremental. A `push-frame.cmd` wrapper is also included if Windows execution
policy prevents launching the `.ps1` directly.

To stop repeated password prompts, run the one-time key setup from PowerShell:

```powershell
.\scripts\setup-frame-ssh.ps1
```

It creates a dedicated key in your Windows `.ssh` directory, asks for the Frame password once to
install the public key, and verifies key login. Press Enter at both key passphrase prompts if you
want unattended pushes. Keep the private key on your Windows computer and never share it. Later
`push-frame.ps1` runs use that key for both upload and build/run. The script also reuses the same
SSH connection for building and running, so before key setup it needs at most two password prompts.

### Bash/rsync alternative

From a development machine with `ssh` and `rsync`:

```bash
FRAME_HOST=steamos@frame bash ./scripts/deploy-frame.sh
```

The host is never hardcoded. `FRAME_REMOTE_DIR` can override the default `frame-notify` directory.
The script copies sources without deleting remote files and prints the remote build command.

## Development build and tests

On a secondary platform, opt in explicitly:

```bash
FRAME_ALLOW_NON_AARCH64=1 bash ./scripts/build-frame.sh
```

The automated tests cover the vector rasterizer, TrueType parsing and kerning, text wrapping,
app names and colours, time formatting, panel layout and hit-testing, the pairing screens, message
parsing, history persistence, the supervision of the Bluetooth helper process, the single-instance
lock, autostart management (against a fake `systemctl`), when the program connects to SteamVR, the
packaging and install scripts, and the ANCS protocol and pairing service (against a fake BlueZ).
SteamVR and BlueZ integration themselves must be verified on the physical Frame.

## License

Frame Notify is available under the MIT License. OpenVR is fetched from Valve's separate SDK and
remains under Valve's OpenVR license. The bundled Inter fonts in `src/ui/fonts` are licensed
separately under the SIL Open Font License 1.1 (see `src/ui/fonts/OFL.txt`).
