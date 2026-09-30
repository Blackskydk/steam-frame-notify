# iPhone notifications over Bluetooth (ANCS)

`scripts/ancs_bridge.py` subscribes to Apple's Notification Source and Data Source through BlueZ,
requests title/message attributes, and sends new notifications to Frame Notify's user-local
socket. The whole path, from LE pairing to notifications appearing in the dashboard, has been
verified on a physical iPhone and Steam Frame.

There are two ways to use it:

- **From Frame Notify (normal use).** The overlay starts the script as a helper
  (`--service`), shows what it reports in the panel, and pairs a phone from the panel. Nothing needs
  a terminal. See [Pairing from the panel](#pairing-from-the-panel).
- **From a terminal (diagnostics and advanced use).** The `--list`, `--diagnose`, `--pair`,
  `--device` and `--solicit` modes below still work on their own. Do not run `--pair` or
  `--device` while Frame Notify's helper is running: both would fight it over the same Bluetooth
  connection and pairing agent. `--list` and `--diagnose` only read and are always safe.

## Prerequisites

- Frame Notify running on the Frame (the process serving `$XDG_RUNTIME_DIR/frame-notify.sock`).
- An iPhone paired with the Frame **over Bluetooth LE**, with notification sharing allowed. ANCS is
  a GATT service on an LE link, so a classic-only pairing (for example one made from the Frame's
  normal Bluetooth screen) can never provide it. Pairing from the panel (or `--pair`) sets this up.
- `python3`, `dbus-python` (`import dbus`), and PyGObject (`from gi.repository import GLib`) on the
  Frame. The script reports missing bindings (the panel shows them); it does not install packages
  or change SteamOS.

## Pairing from the panel

The panel's header shows the phone's state; tapping it opens a screen with the matching actions.
The helper reports one of these states (they are what `scripts/ancs_service.py` documents):

| State | What the panel shows |
| --- | --- |
| no Bluetooth | Bluetooth is off (with **Turn on Bluetooth**), missing, ambiguous, or without LE |
| unpaired | **Pair an iPhone** |
| connecting / connected | looking for the remembered phone / listening for its notifications |
| needs attention | the phone did not allow notification sharing, is classic-only, or lost its pairing; with the steps to fix it |
| already paired | phones that may be in the way of a fresh LE pairing, each with a **Remove** button |
| waiting | the five-minute pairing window: the iPhone taps **Frame** under Other Devices |
| confirm | the six-digit code to compare; **Yes, it matches** or **No, it doesn't** |
| paired / failed | the result, kept on screen until you tap **Done** or **Close** |

While a pairing is in progress the panel opens that screen by itself, and a SteamVR toast asks you
to open the dashboard if it is closed when the code appears. Only paired devices BlueZ classes as
phones, and the phone Frame Notify remembers, are ever offered for removal; headsets, mice and
keyboards never are, and a device is removed only when you press its button. A pairing that turns
out to be classic-only is removed again, because it cannot carry notifications.

The phone that completes pairing is stored as `phone.json` (user-only permissions) in the same
state directory as the notification history, so the next start reconnects without asking.
**Forget this phone** removes that file and the phone's BlueZ pairing. A helper that crashes is
restarted with a growing pause; one that cannot run at all (for example without `dbus-python`) is
reported in the panel with a **Try again** button rather than retried in a loop.

The service protocol is one JSON object per line, all values strings, on the helper's stdin and
stdout; the state and command names are listed at the top of `scripts/ancs_service.py`. Human
readable log lines go to stderr, which is the terminal Frame Notify runs in.

A phone that was paired with the terminal tools before shows up in the "already paired" list the
first time you pair from the panel; remove it there (and forget the Frame on the iPhone) and pair
again, which takes about a minute.

List paired devices without connecting:

```bash
cd ~/frame-notify
python3 ./scripts/ancs_bridge.py --list
```

If a paired device connects but the bridge reports that ANCS is missing, inspect it without
changing the connection:

```bash
python3 ./scripts/ancs_bridge.py --diagnose AA:BB:CC:DD:EE:FF
```

This prints `Connected`, `ServicesResolved`, the remote GATT services, and any reported bearer
preference. A classic Bluetooth connection can show `Connected: True` and even
`ServicesResolved: True` (BlueZ also sets it after classic SDP) without any GATT service. **Zero**
remote GATT services on a connected phone means the link is classic-only: no ANCS, and the iPhone
will not offer **Share System Notifications** for the Frame. Do not rely on `AddressType` to tell
the two apart; the remote GATT service list is the reliable signal. The fix is to forget the
pairing on both ends and pair again over LE with `--pair`.

## Pair the iPhone over LE from a terminal

This is what the panel does for you; use it to see Bluetooth's side of things when that does not
work. `--pair` opens a five-minute window in which the iPhone can start a fresh LE pairing. Run it
in an interactive SSH session (`ssh -t`), because you confirm the pairing code in that terminal:

```bash
python3 ./scripts/ancs_bridge.py --pair
```

1. **Remove any existing pairing on both ends first.** On the iPhone: Settings > Bluetooth >
   the Frame's (i) > Forget This Device. On the Frame: `bluetoothctl remove AA:BB:CC:DD:EE:FF`.
   The script lists the devices BlueZ already has paired and warns about them, but it never
   removes a pairing itself. A leftover classic bond keeps the phone on classic Bluetooth.
2. Start `--pair`. It advertises the ANCS service solicitation as **Frame**, registers a
   temporary BlueZ pairing agent, and sets the adapter's `Pairable` on.
3. Start the pairing **from the iPhone**: Settings > Bluetooth, then tap **Frame** under Other
   Devices. Do not start it from the Frame's own Bluetooth screen.
4. When the terminal asks whether the iPhone shows the same code, compare and answer `y`. Any
   other answer, no answer for 40 seconds, or a closed terminal rejects the request.
5. The script waits up to 30 seconds after pairing and reports what BlueZ sees: `ANCS service
   resolved`, `GATT services resolved, no ANCS yet`, or `no GATT services yet` (still classic).
   It then prints the exact `--device` command to run next. In the iPhone's Bluetooth details for
   the Frame, turn on **Share System Notifications**. This is required: until it is on, the
   iPhone lets the bridge subscribe ("Listening for iPhone notifications") but sends nothing, and
   the bridge logs no `Notification event` lines. The switch starts off after every new pairing.

Exit status is 0 when ANCS was seen, 2 when the device paired but ANCS was not exposed, and 1
otherwise. On exit (including `Ctrl+C`) it removes the advertisement, unregisters its agent, and
restores the adapter's `Pairable` and `PairableTimeout` values. It does not touch `Discoverable`,
so the phone is not offered a classic pairing, and it does not power the adapter or trust the
device. While it runs it is the default BlueZ pairing agent, so it answers any pairing request
that arrives in the window; confirm only the code shown for your own iPhone. If it is killed
without cleanup, run `bluetoothctl pairable off` if you want the previous setting back.

Run the bridge in a separate SSH session, specifying only the intended paired device:

```bash
python3 ./scripts/ancs_bridge.py --device AA:BB:CC:DD:EE:FF
```

If the phone is an iPhone but BlueZ reports no ANCS service, try the explicitly opt-in
solicitation mode:

```bash
python3 ./scripts/ancs_bridge.py --device AA:BB:CC:DD:EE:FF --solicit
```

This registers a temporary LE advertisement with the ANCS **SolicitUUID** through BlueZ. The
advertisement is discoverable as **Frame** to nearby devices (it contains no notification
contents) and occupies
one advertising instance while the bridge runs. Press `Ctrl+C` to stop the bridge and remove the
advertisement. It does not change adapter settings, forget bonds, or re-pair the phone. It only
helps a phone that is already LE-bonded reconnect over LE. If BlueZ still shows no GATT services
after a reconnect, the bond is classic-only; use `--pair` instead of repeating this.

Leave both processes running. The bridge prints `Listening for iPhone notifications` after it
subscribes. Send a new notification to the iPhone, then look for `Forwarded notification` in the
bridge and an `[IPC] Notification` line in Frame Notify. It retries a lost connection to the
selected device but never scans for or connects to other devices.

If ANCS is missing, check that the phone is paired, connected, and has granted notification
access; then inspect its BlueZ GATT services. If the Python D-Bus bindings are missing, use the
packages provided by the Frame's environment rather than modifying SteamOS blindly. The bridge
stops on a protocol timeout so delayed fragments cannot be mistaken for another notification.

## Current behavior and limits

- New iPhone notifications are forwarded; notifications flagged *pre-existing* are skipped to
  avoid replaying old alerts after reconnect. Modified and removed ANCS events do not rewrite
  the persistent local history.
- Every notification is forwarded at most once, however often the iPhone lists it. An ANCS UID
  is valid only for its Bluetooth connection, so it cannot tell a notification seen before from a
  new one. The bridge names a notification by a *key* instead: the first 24 hex digits of the
  SHA-256 of its app identifier, title, subtitle, message and date (the date is when the iPhone
  posted it). The forwarded ID is `ancs-<key>`, the same on every connection. The keys of what has
  been sent are kept in `seen_notifications.json` in the state directory (keys and times only,
  user-only permissions, at most 2000 of them and 90 days old), and a notification whose key is
  in it is read but not forwarded again. That is why a notification cleared on the Frame stays
  cleared even if it is still on the iPhone, and why the iPhone listing its unread notifications
  again on every start (whatever flags it puts on them) brings nothing back. A key is added only
  after the notification has reached Frame Notify, so one that could not be delivered is sent next
  time. A notification without a date has no key: it gets `ancs-<connection>-<uid>` and is not
  remembered.
- Clearing on the iPhone: when a card is cleared on the Frame (or **Clear all** is used), Frame
  Notify sends the helper `{"command":"clear_notifications","ids":"ancs-<key>,..."}` and the
  bridge writes ANCS *Perform Notification Action* with the negative action, for each notification
  whose event flags include *NegativeAction* (0x10). Most notifications have it: it is the
  iPhone's own "Clear". For a notification seen in this connection the UID is known. For an
  older one (sent in an earlier connection) the bridge reads the attributes of the notifications
  the iPhone listed as pre-existing, newest first and only after anything newly arrived, until it
  finds the one with that key; it gives up when it has read them all (at most 300) or when the
  iPhone no longer has it. Nothing is done while the iPhone is not connected, and a notification
  the iPhone does not allow to be cleared stays there. `FRAME_NOTIFY_KEEP_ON_PHONE=1` turns the
  whole thing off. The other direction (clearing on the iPhone clears on the Frame) is not done.
- Silent iPhone alerts (those the iPhone flags as low priority, for example while it is muted or
  in a Focus) are logged as such and get a SteamVR toast like the others; with
  `FRAME_NOTIFY_QUIET_NO_TOAST=1` in Frame Notify's environment they go to the history only.
- After a notification's title and message arrive, the bridge asks the iPhone once per app for
  its display name (ANCS *Get App Attributes*) and remembers the answer until the bridge stops.
  Frame Notify then shows "Messages" (or "Nachrichten" on a German-language iPhone) instead of
  `com.apple.MobileSMS`, and the bundle identifier is forwarded as `app_id` to pick the app's
  colour. A name request that fails, times out (8 s) or returns something unexpected never drops
  the notification: it is shown under the identifier, that app is not asked about again, and the
  bridge waits three seconds before its next request so a late fragment cannot be mistaken for
  another response. Frame Notify also turns a raw identifier into a readable name on its own.
- The current history file contains notification content. Review its retention settings in the
  README before forwarding private phone alerts.

The implementation follows [Apple's ANCS specification](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html)
and [BlueZ's GATT characteristic API](https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattCharacteristic.rst).
