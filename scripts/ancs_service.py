"""Bluetooth helper that Frame Notify starts and drives: pairing and the iPhone bridge.

Run it with `ancs_bridge.py --service`. It is meant to be started by Frame Notify, not by hand.
It talks to its parent with one JSON object per line, every value a string:

  stdout (events)   {"event":"hello","version":"1"}
                    {"event":"state","state":<state>, ...fields}    the current state, in full
                    {"event":"fatal","message":...}                 then it exits

  stdin (commands)  {"command":"pair"}  pair_anyway  cancel  confirm  reject  dismiss  forget
                    power_on  retry  status  quit  and
                    {"command":"remove_conflict","address":"AA:BB:CC:DD:EE:FF"}
                    {"command":"clear_notifications","ids":"ancs-<key>,ancs-<key>"}   clear on the
                    iPhone what the Frame has cleared (ids as the notifications were sent)

States, in the order a phone normally goes through them:

  no_bluetooth      reason = no_bluez | no_adapter | ambiguous | powered_off | no_le, detail
  unpaired          no phone is paired yet
  connecting        phone, address, detail   looking for the paired phone
  connected         phone, address           listening for notifications
  needs_repair      phone, address, reason = not_allowed | no_ancs | not_paired, detail
  pair_conflict     count, name1/address1 ... name3/address3   paired phones that may be in the way
  pair_open         seconds, name, detail    the pairing window is open; the phone taps "Frame"
  pair_confirm      phone, code, seconds     compare the code with the one on the phone
  pair_verify       phone, address           paired; checking for a notification-capable link
  pair_done         phone, address, ancs = yes | no          (stays until "dismiss")
  pair_failed       reason = timeout | classic_only | advertising | agent | error, detail, phone
                                                             (stays until "dismiss")

The phone that completed pairing is remembered in phone.json in Frame Notify's state directory, so
the next start connects to it without asking. The keys of the notifications already sent to the
Frame are kept in seen_notifications.json beside it. Human-readable log lines go to stderr.
"""

import json
import os
import re
import signal
import sys

from ancs_bridge import (ADAPTER, BLUEZ, DEVICE, OBJECT_MANAGER, PROPERTIES, AdapterProblem,
                         Bridge, ID_PREFIX, PairSession, pick_adapter)
from ancs_protocol import SeenNotifications

PROTOCOL_VERSION = "1"
BRIDGE_RESTART_SECONDS = 5
PERMISSION_RETRY_SECONDS = 15
REPAIR_AFTER_SECONDS = 30
POWER_ON_SETTLE_SECONDS = 2
MAXIMUM_LINE_BYTES = 65536
MAXIMUM_CONFLICTS = 3
MAXIMUM_CLEARED = 200
SEEN_FILE = "seen_notifications.json"
ADDRESS_PATTERN = re.compile(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}")
COMMANDS = ("pair", "pair_anyway", "cancel", "confirm", "reject", "remove_conflict", "dismiss",
            "forget", "power_on", "retry", "status", "clear_notifications", "quit")


def state_directory(environ=None):
    """Where Frame Notify keeps its state; the same place its notification history lives."""
    environ = os.environ if environ is None else environ
    base = environ.get("XDG_STATE_HOME")
    if base:
        return os.path.join(base, "frame-notify")
    home = environ.get("HOME")
    if not home:
        raise RuntimeError("neither XDG_STATE_HOME nor HOME is set")
    return os.path.join(home, ".local", "state", "frame-notify")


class PhoneConfig:
    """The one phone this Frame listens to, stored as phone.json with user-only permissions."""

    def __init__(self, directory):
        self.directory = directory
        self.path = os.path.join(directory, "phone.json")

    def load(self):
        try:
            with open(self.path, encoding="utf-8") as stream:
                data = json.load(stream)
        except (OSError, ValueError):
            return None
        address = data.get("address") if isinstance(data, dict) else None
        if not isinstance(address, str) or not ADDRESS_PATTERN.fullmatch(address):
            return None
        name = data.get("name")
        return {"address": address.upper(), "name": name if isinstance(name, str) and name else "iPhone"}

    def save(self, address, name):
        os.makedirs(self.directory, mode=0o700, exist_ok=True)
        temporary = self.path + ".tmp"
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump({"address": address.upper(), "name": name}, stream)
        os.replace(temporary, self.path)

    def clear(self):
        try:
            os.remove(self.path)
        except FileNotFoundError:
            pass


def find_phone_devices(objects, stored_address=None):
    """Paired devices that could be in the way of a fresh LE pairing.

    That is every paired device BlueZ classes as a phone, plus the remembered phone whatever its
    class. Headphones, mice and keyboards are never included, so they can never be offered for
    removal.
    """
    found = []
    for path, interfaces in objects.items():
        props = interfaces.get(DEVICE)
        if props is None or not bool(props.get("Paired", False)):
            continue
        address = str(props.get("Address", ""))
        remembered = bool(stored_address) and address.upper() == stored_address.upper()
        if str(props.get("Icon", "")) == "phone" or remembered:
            found.append({"path": str(path), "address": address,
                          "name": str(props.get("Alias", address))})
    found.sort(key=lambda device: (device["address"].upper() != (stored_address or "").upper(),
                                   device["address"]))
    return found


class CallbackLoop:
    """Stands in for a GLib main loop: whoever would quit it calls us instead."""

    def __init__(self, callback):
        self.callback = callback

    def quit(self):
        self.callback()


class PairHooks:
    """Connects a PairSession's progress to the service."""

    def __init__(self, service):
        self.service = service

    def opened(self, seconds):
        self.service.on_pair_opened(seconds)

    def confirm(self, phone, code, seconds):
        self.service.on_pair_confirm(phone, code, seconds)

    def prompt_expired(self):
        self.service.on_pair_prompt_expired()

    def paired(self, address, alias):
        self.service.on_pair_paired(address, alias)


class Service:
    def __init__(self, bus, glib, dbus, socket_path, config, emit, adapter_name=None,
                 bridge_factory=Bridge, pair_factory=PairSession):
        self.bus = bus
        self.glib = glib
        self.dbus = dbus
        self.socket_path = socket_path
        self.config = config
        self.emit = emit
        self.adapter_name = adapter_name
        self.bridge_factory = bridge_factory
        self.pair_factory = pair_factory
        self.manager = dbus.Interface(bus.get_object(BLUEZ, "/"), OBJECT_MANAGER)
        self.adapter_path = None
        self.phone = None            # {"address", "name"} of the remembered phone
        self.bridge = None
        self.pair = None
        self.phase = None            # "conflict" while waiting for the user to choose
        self.conflicts = []
        self.skip_conflicts = False
        self.paired_alias = None
        self.background = None       # latest state that is not part of a pairing
        self.current = None          # latest state sent to the parent
        self.sticky = None           # a pairing result that stays on screen until dismissed
        self.repair_timer = None
        self.restart_timer = None
        self.power_timer = None
        self.last_bridge_problem = None
        self.quiet_retry = False     # retrying a refused subscription without changing the screen
        self.loop = None
        # What the Frame has been sent, so a notification that was cleared there stays cleared.
        self.seen = SeenNotifications(os.path.join(config.directory, SEEN_FILE))

    # ---- state -------------------------------------------------------------------------------

    def set_state(self, state, **fields):
        """A state of the normal (non-pairing) life of the helper; hidden while pairing is shown."""
        self.background = (state, self.clean(fields))
        if not self.showing_pairing():
            self.publish(state, self.background[1])

    def set_pairing_state(self, state, **fields):
        self.publish(state, self.clean(fields))

    def showing_pairing(self):
        return self.pair is not None or self.phase is not None or self.sticky is not None

    @staticmethod
    def clean(fields):
        return {name: str(value) for name, value in fields.items() if value is not None}

    def publish(self, state, fields, force=False):
        if not force and self.current == (state, fields):
            return
        self.current = (state, fields)
        event = {"event": "state", "state": state}
        event.update(fields)
        self.emit(event)

    def phone_fields(self):
        if not self.phone:
            return {}
        return {"phone": self.phone["name"], "address": self.phone["address"]}

    # ---- start-up and recovery ---------------------------------------------------------------

    def start(self):
        self.emit({"event": "hello", "version": PROTOCOL_VERSION})
        self.begin()

    def begin(self):
        """(Re)check Bluetooth and, when a phone is remembered, start looking for it."""
        self.stop_bridge()
        try:
            objects = self.manager.GetManagedObjects()
        except self.dbus.DBusException as error:
            self.set_state("no_bluetooth", reason="no_bluez", detail=str(error))
            return
        try:
            self.adapter_path = pick_adapter(objects, self.adapter_name)
        except AdapterProblem as problem:
            self.adapter_path = None
            self.set_state("no_bluetooth", reason=problem.reason, detail=str(problem))
            return
        self.phone = self.config.load()
        if self.phone is None:
            self.set_state("unpaired")
            return
        self.start_bridge()

    def start_bridge(self):
        self.stop_bridge()
        bridge = self.bridge_factory(self.bus, self.glib, self.dbus, self.phone["address"],
                                     self.socket_path, solicit=True)
        bridge.on_status = self.on_bridge_status
        bridge.loop = CallbackLoop(self.on_bridge_failed)
        bridge.seen = self.seen
        self.bridge = bridge
        if not self.quiet_retry:
            self.last_bridge_problem = None
            self.set_state("connecting", **self.phone_fields())
        try:
            bridge.start()
        except RuntimeError as error:
            self.drop_bridge()
            if "advertising manager" in str(error):
                self.set_state("no_bluetooth", reason="no_le", detail=str(error))
            else:
                self.set_state("needs_repair", reason="not_paired", detail=str(error),
                               **self.phone_fields())
        except self.dbus.DBusException as error:
            self.drop_bridge()
            self.set_state("no_bluetooth", reason="no_bluez", detail=str(error))

    def drop_bridge(self):
        bridge, self.bridge = self.bridge, None
        if bridge is not None:
            bridge.on_status = None
            bridge.stop()

    def stop_bridge(self):
        self.drop_bridge()
        for name in ("repair_timer", "restart_timer"):
            source = getattr(self, name)
            if source:
                self.glib.source_remove(source)
                setattr(self, name, None)

    def on_bridge_status(self, state, detail=""):
        fields = self.phone_fields()
        if state == "listening":
            self.cancel_repair_timer()
            self.last_bridge_problem = None
            self.quiet_retry = False
            self.set_state("connected", **fields)
        elif self.quiet_retry and state != "subscribe_failed":
            return   # still waiting for the phone to allow notifications: keep that message
        elif state in ("connecting", "waiting", "disconnected"):
            if state == "disconnected":
                self.cancel_repair_timer()
            self.set_state("connecting", detail=state, **fields)
        elif state in ("no_gatt", "no_ancs"):
            self.last_bridge_problem = state
            self.set_state("connecting", detail=state, **fields)
            if self.repair_timer is None:
                self.repair_timer = self.glib.timeout_add_seconds(REPAIR_AFTER_SECONDS,
                                                                  self.on_repair_timeout)
        elif state == "subscribe_failed":
            self.cancel_repair_timer()
            self.last_bridge_problem = state
            self.quiet_retry = True
            self.set_state("needs_repair", reason="not_allowed", detail=detail, **fields)

    def cancel_repair_timer(self):
        if self.repair_timer:
            self.glib.source_remove(self.repair_timer)
            self.repair_timer = None

    def on_repair_timeout(self):
        self.repair_timer = None
        if self.bridge is not None and self.last_bridge_problem in ("no_gatt", "no_ancs"):
            self.set_state("needs_repair", reason="no_ancs", detail=self.last_bridge_problem,
                           **self.phone_fields())
        return False

    def on_bridge_failed(self):
        """The bridge gave up (a timeout, a refused subscription...): start it again shortly."""
        permission = self.last_bridge_problem == "subscribe_failed"
        self.drop_bridge()
        if not permission:
            self.set_state("connecting", detail="restarting", **self.phone_fields())
        if self.restart_timer is None:
            delay = PERMISSION_RETRY_SECONDS if permission else BRIDGE_RESTART_SECONDS
            self.restart_timer = self.glib.timeout_add_seconds(delay, self.restart_bridge)

    def restart_bridge(self):
        self.restart_timer = None
        if self.phone is not None and self.pair is None and self.phase is None:
            self.start_bridge()
        return False

    def reset_retry(self):
        self.quiet_retry = False
        self.last_bridge_problem = None

    # ---- pairing -----------------------------------------------------------------------------

    def cmd_pair(self, _message):
        if self.pair is not None or self.phase is not None:
            return
        if self.adapter_path is None:
            self.begin()
            if self.adapter_path is None:
                return
        self.sticky = None
        self.skip_conflicts = False
        self.reset_retry()
        self.stop_bridge()
        # The bridge is stopped now, so the state to return to after a cancel is not "connected".
        self.background = (("connecting", self.clean(self.phone_fields())) if self.phone
                           else ("unpaired", {}))
        self.check_conflicts()

    def check_conflicts(self):
        stored = self.phone["address"] if self.phone else None
        self.conflicts = [] if self.skip_conflicts else find_phone_devices(
            self.manager.GetManagedObjects(), stored)
        if not self.conflicts:
            self.open_pairing()
            return
        self.phase = "conflict"
        fields = {"count": len(self.conflicts)}
        for index, device in enumerate(self.conflicts[:MAXIMUM_CONFLICTS], start=1):
            fields[f"name{index}"] = device["name"]
            fields[f"address{index}"] = device["address"]
        self.set_pairing_state("pair_conflict", **fields)

    def cmd_pair_anyway(self, _message):
        if self.phase == "conflict":
            self.skip_conflicts = True
            self.check_conflicts()

    def cmd_remove_conflict(self, message):
        address = message.get("address", "")
        # Only a device that was offered on screen can be removed.
        match = next((device for device in self.conflicts[:MAXIMUM_CONFLICTS]
                      if device["address"].upper() == address.upper()), None)
        if self.phase != "conflict" or match is None:
            return
        self.remove_device(match["address"])
        self.check_conflicts()

    def remove_device(self, address):
        """Removes a paired device from BlueZ; returns whether BlueZ accepted."""
        for path, interfaces in self.manager.GetManagedObjects().items():
            props = interfaces.get(DEVICE)
            if props is not None and str(props.get("Address", "")).upper() == address.upper():
                try:
                    self.dbus.Interface(self.bus.get_object(BLUEZ, self.adapter_path),
                                        ADAPTER).RemoveDevice(path)
                    return True
                except self.dbus.DBusException as error:
                    print(f"[ANCS] Could not remove {address}: {error}", file=sys.stderr,
                          flush=True)
                    return False
        return False

    def open_pairing(self):
        self.phase = None
        self.paired_alias = None
        session = self.pair_factory(self.bus, self.glib, self.dbus, self.adapter_path,
                                    hooks=PairHooks(self))
        session.loop = CallbackLoop(self.on_pair_ended)
        self.pair = session
        try:
            session.start()
        except (RuntimeError, self.dbus.DBusException) as error:
            session.failure = ("error", str(error))
            self.on_pair_ended()

    def on_pair_opened(self, seconds):
        self.set_pairing_state("pair_open", seconds=seconds, name="Frame")

    def on_pair_confirm(self, phone, code, seconds):
        self.set_pairing_state("pair_confirm", phone=phone, code=code or "", seconds=seconds)

    def on_pair_prompt_expired(self):
        self.set_pairing_state("pair_open", seconds=self.pair.seconds_left() if self.pair else 0,
                               name="Frame", detail="expired")

    def on_pair_paired(self, address, alias):
        self.paired_alias = alias
        self.set_pairing_state("pair_verify", phone=alias, address=address)

    def on_pair_ended(self):
        session = self.pair
        if session is None:
            return
        result, address, failure = session.result, session.address, session.failure
        alias = self.paired_alias or "iPhone"
        self.end_pairing()
        if result in ("ancs", "gatt"):
            try:
                self.config.save(address, alias)
            except OSError as error:
                print(f"[ANCS] Could not remember the phone: {error}", file=sys.stderr, flush=True)
            self.phone = {"address": address.upper(), "name": alias}
            self.finish_with("pair_done", phone=alias, address=address,
                             ancs="yes" if result == "ancs" else "no")
        elif result == "none":
            # Only a classic-Bluetooth pairing came out of it, which cannot carry notifications.
            # Remove it so the next attempt starts clean.
            self.remove_device(address)
            self.finish_with("pair_failed", reason="classic_only", phone=alias)
        else:
            reason, detail = failure or ("error", "")
            self.finish_with("pair_failed", reason=reason, detail=detail)
        if self.phone is not None:
            self.start_bridge()

    def end_pairing(self):
        session, self.pair = self.pair, None
        self.phase = None
        self.conflicts = []
        if session is not None:
            session.stop()

    def finish_with(self, state, **fields):
        """A pairing result stays on screen until the user dismisses it."""
        fields = self.clean(fields)
        self.sticky = (state, fields)
        self.publish(state, fields)

    def cmd_confirm(self, _message):
        if self.pair is not None:
            self.pair.answer(True)

    def cmd_reject(self, _message):
        if self.pair is not None and self.pair.answer(False):
            self.set_pairing_state("pair_open", seconds=self.pair.seconds_left(), name="Frame",
                                   detail="rejected")

    def cmd_cancel(self, _message):
        if self.pair is None and self.phase is None:
            return
        self.end_pairing()
        self.sticky = None
        self.resume()

    def resume(self):
        """Back to normal life after a pairing screen closes."""
        self.publish_background()
        if self.phone is not None and self.bridge is None:
            self.start_bridge()

    def publish_background(self):
        if self.background is not None:
            self.publish(self.background[0], self.background[1])

    def cmd_dismiss(self, _message):
        if self.sticky is None:
            return
        self.sticky = None
        self.publish_background()

    def cmd_forget(self, _message):
        self.end_pairing()
        self.sticky = None
        self.stop_bridge()
        phone = self.phone or self.config.load()
        if phone is not None and self.adapter_path is not None:
            self.remove_device(phone["address"])
        self.config.clear()
        self.phone = None
        self.reset_retry()
        self.set_state("unpaired")

    # ---- Bluetooth power ---------------------------------------------------------------------

    def cmd_power_on(self, _message):
        """Turns the adapter on, but only because the user pressed the button for it."""
        try:
            objects = self.manager.GetManagedObjects()
            adapters = sorted(str(path) for path, interfaces in objects.items()
                              if ADAPTER in interfaces)
            path = "/org/bluez/" + self.adapter_name if self.adapter_name else (
                adapters[0] if adapters else None)
            if path is None or path not in adapters:
                self.set_state("no_bluetooth", reason="no_adapter",
                               detail="no Bluetooth adapter was found")
                return
            self.dbus.Interface(self.bus.get_object(BLUEZ, path), PROPERTIES).Set(
                ADAPTER, "Powered", self.dbus.Boolean(True))
        except self.dbus.DBusException as error:
            self.set_state("no_bluetooth", reason="powered_off", detail=str(error))
            return
        if self.power_timer is None:
            self.power_timer = self.glib.timeout_add_seconds(POWER_ON_SETTLE_SECONDS,
                                                             self.after_power_on)

    def after_power_on(self):
        self.power_timer = None
        self.begin()
        return False

    def cmd_retry(self, _message):
        if self.pair is None and self.phase is None:
            self.reset_retry()
            self.begin()

    def cmd_status(self, _message):
        if self.current is not None:
            self.publish(self.current[0], self.current[1], force=True)

    def cmd_clear_notifications(self, message):
        """The Frame cleared these notifications (`ids`, separated by commas): clear them on the iPhone."""
        if self.bridge is None:
            return
        ids = [name for name in message.get("ids", "").split(",") if name.startswith(ID_PREFIX)]
        if ids:
            self.bridge.clear_on_phone(ids[:MAXIMUM_CLEARED])

    def cmd_quit(self, _message):
        if self.loop is not None:
            self.loop.quit()

    # ---- commands from the parent ------------------------------------------------------------

    def handle_line(self, line):
        try:
            text = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
            if not text.strip():
                return
            message = json.loads(text)
        except ValueError:
            print("[ANCS] Ignoring a command that is not JSON", file=sys.stderr, flush=True)
            return
        if not isinstance(message, dict):
            print("[ANCS] Ignoring a command that is not an object", file=sys.stderr, flush=True)
            return
        message = {str(key): str(value) for key, value in message.items()}
        command = message.get("command", "")
        if command not in COMMANDS:
            print(f"[ANCS] Ignoring unknown command {command!r}", file=sys.stderr, flush=True)
            return
        getattr(self, "cmd_" + command)(message)

    def on_stdin_closed(self):
        """The parent went away: nothing is left to serve, so stop and release Bluetooth."""
        if self.loop is not None:
            self.loop.quit()

    def stop(self):
        self.end_pairing()
        self.stop_bridge()
        if self.power_timer:
            self.glib.source_remove(self.power_timer)
            self.power_timer = None


class CommandReader:
    """Splits what arrives on stdin into lines and hands each to the service."""

    def __init__(self, service):
        self.service = service
        self.buffer = b""

    def feed(self, data):
        if not data:
            self.service.on_stdin_closed()
            return False
        self.buffer += data
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            self.service.handle_line(line)
        if len(self.buffer) > MAXIMUM_LINE_BYTES:
            self.buffer = b""   # never let a runaway sender grow it without bound
        return True

    def on_readable(self, descriptor, _condition):
        try:
            data = os.read(descriptor, 4096)
        except OSError:
            data = b""
        return self.feed(data)


def run_service(dbus, glib_module, adapter_name=None, output=None):
    """Runs the helper until it is told to quit or its parent goes away. Returns the exit code."""
    output = output or sys.stdout
    sys.stdout = sys.stderr   # the human-readable logging must not mix into the event stream

    def emit(event):
        output.write(json.dumps(event, ensure_ascii=False) + "\n")
        output.flush()

    try:
        runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
        if not runtime_dir:
            raise RuntimeError("XDG_RUNTIME_DIR is not set; start Frame Notify in a user session")
        config = PhoneConfig(state_directory())
        bus = dbus.SystemBus()
    except (RuntimeError, dbus.DBusException) as error:
        emit({"event": "fatal", "message": str(error)})
        return 1

    service = Service(bus, glib_module, dbus, os.path.join(runtime_dir, "frame-notify.sock"),
                      config, emit, adapter_name)
    service.loop = glib_module.MainLoop()
    reader = CommandReader(service)
    glib_module.io_add_watch(sys.stdin.fileno(), glib_module.PRIORITY_DEFAULT,
                             glib_module.IO_IN | glib_module.IO_HUP | glib_module.IO_ERR,
                             reader.on_readable)
    signal.signal(signal.SIGINT, lambda _signum, _frame: service.loop.quit())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: service.loop.quit())
    try:
        service.start()
        service.loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()
    return 0
