#!/usr/bin/env python3
"""Opt-in BlueZ/ANCS-to-Frame-Notify bridge for a paired iPhone.

Requires the Linux distribution's dbus-python and PyGObject bindings. The bridge
itself never pairs devices or changes adapter configuration; use --list to
inspect candidates. Only the explicit, time-limited --pair mode opens a pairing
window, and it restores the adapter settings it touched when it exits.
"""

import argparse
from collections import deque
from datetime import datetime, timezone
import json
import os
import re
import signal
import socket
import sys
import time
import uuid

from ancs_protocol import (ACTION_NEGATIVE, ADDED, FLAG_NEGATIVE_ACTION, FLAG_PRE_EXISTING,
                           FLAG_SILENT, MODIFIED, REMOVED,
                           AppAttributeResponse, AttributeResponse, CONTROL_POINT_UUID,
                           DATA_SOURCE_UUID, NOTIFICATION_SOURCE_UUID,
                           SERVICE_UUID, app_attributes_request, attribute_request,
                           notification_key, parse_event, perform_action_request)

BLUEZ = "org.bluez"
OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager"
PROPERTIES = "org.freedesktop.DBus.Properties"
DEVICE = "org.bluez.Device1"
SERVICE = "org.bluez.GattService1"
CHARACTERISTIC = "org.bluez.GattCharacteristic1"
ADVERTISING_MANAGER = "org.bluez.LEAdvertisingManager1"
ADVERTISEMENT = "org.bluez.LEAdvertisement1"
ADAPTER = "org.bluez.Adapter1"
AGENT = "org.bluez.Agent1"
AGENT_MANAGER = "org.bluez.AgentManager1"
AGENT_PATH = "/com/frame_notify/ancs_agent"
REJECTED = "org.bluez.Error.Rejected"

PAIR_WINDOW_SECONDS = 300   # --pair closes itself; it must not leave the adapter pairable
PROMPT_SECONDS = 40         # answer before BlueZ gives up on the agent call
LINK_CHECK_SECONDS = 2
LINK_CHECKS = 15            # wait ~30 s after pairing for the iPhone's GATT services
NOTIFICATION_TIMEOUT_SECONDS = 15
APP_NAME_TIMEOUT_SECONDS = 8
APP_NAME_COOLDOWN_SECONDS = 3   # let a late app-name fragment arrive before the next request
ID_PREFIX = "ancs-"             # the ids this bridge gives the notifications it forwards
KEY_ID = re.compile(r"[0-9a-f]{24}")
SESSION_ID = re.compile(r"[0-9a-f]{12}-([0-9]{1,10})")
LOOKUP_LIMIT = 300              # older iPhone notifications that may be looked up to clear one


def paired_devices(objects):
    return [(str(path), props[DEVICE]) for path, props in objects.items()
            if DEVICE in props and bool(props[DEVICE].get("Paired", False))]


def remote_services(objects, device_path):
    """Sorted (uuid, object path) pairs for the GATT services BlueZ resolved on a device."""
    return sorted((str(interfaces[SERVICE].get("UUID", "")).lower(), str(path))
                  for path, interfaces in objects.items()
                  if SERVICE in interfaces and
                  (str(interfaces[SERVICE].get("Device", "")) == device_path or
                   str(path).startswith(device_path + "/")))


def link_status(objects, device_path):
    """'ancs', 'gatt' or 'none': how much of an LE/GATT link BlueZ currently exposes."""
    services = remote_services(objects, device_path)
    if any(service_uuid == SERVICE_UUID for service_uuid, _ in services):
        return "ancs"
    return "gatt" if services else "none"


def format_passkey(passkey):
    return f"{int(passkey):06d}"


def parse_answer(text):
    """True for yes, False for no or an empty line (the default), None if unclear."""
    answer = text.strip().lower()
    if answer in ("y", "yes"):
        return True
    if answer in ("", "n", "no"):
        return False
    return None


class AdapterProblem(RuntimeError):
    """The Bluetooth adapter cannot be used; `reason` says why in a form a UI can act on."""

    def __init__(self, reason, message):
        super().__init__(message)
        self.reason = reason   # "no_adapter", "ambiguous" or "powered_off"


def pick_adapter(objects, name=None):
    adapters = sorted(str(path) for path, interfaces in objects.items()
                      if ADAPTER in interfaces and ADVERTISING_MANAGER in interfaces)
    if name:
        path = "/org/bluez/" + name
        if path not in adapters:
            raise AdapterProblem("no_adapter",
                                 f"adapter {name} was not found or has no LE advertising manager")
    elif len(adapters) == 1:
        path = adapters[0]
    elif not adapters:
        raise AdapterProblem("no_adapter", "no Bluetooth adapter with LE advertising was found")
    else:
        raise AdapterProblem("ambiguous",
                             f"found {len(adapters)} LE-capable adapters; pass --adapter hciN")
    if not bool(objects[path][ADAPTER].get("Powered", False)):
        raise AdapterProblem("powered_off",
                             f"adapter {path} is not powered; this script does not power it on")
    return path


def diagnose_device(objects, address):
    matches = [(str(path), interfaces[DEVICE]) for path, interfaces in objects.items()
               if DEVICE in interfaces and
               str(interfaces[DEVICE].get("Address", "")).upper() == address.upper()]
    if len(matches) != 1:
        raise RuntimeError("device is not uniquely present in BlueZ; run --list")
    path, props = matches[0]
    print("BlueZ device:", path)
    for name in ("Alias", "AddressType", "Paired", "Bonded", "Trusted", "Connected",
                 "ServicesResolved", "PreferredBearer"):
        value = props.get(name, "not reported")
        if name in ("Paired", "Bonded", "Trusted", "Connected", "ServicesResolved") and \
                value != "not reported":
            value = bool(value)
        print(f"  {name}: {value}")
    uuids = sorted(str(value).lower() for value in props.get("UUIDs", ()))
    print("  Device UUIDs:", ", ".join(uuids) if uuids else "none reported")
    services = remote_services(objects, path)
    print("  Remote GATT services:", len(services))
    for service_uuid, service_path in services:
        print(f"    {service_uuid}  {service_path}")
    print("  ANCS:", "present" if any(item[0] == SERVICE_UUID for item in services)
          else "not exposed by BlueZ")
    adapter_path = str(props.get("Adapter", ""))
    advertising = objects.get(adapter_path, {}).get(ADVERTISING_MANAGER)
    if advertising is None:
        print("  LE advertising manager: not exposed by BlueZ")
    else:
        print("  LE advertising manager:", adapter_path)
        active = advertising.get("ActiveInstances")
        supported = advertising.get("SupportedInstances")
        print("  Advertising instances:", int(active) if active is not None else "?",
              "active /", int(supported) if supported is not None else "?", "supported")
    if bool(props.get("Connected", False)) and not services:
        print("  Note: connected with no remote GATT objects; an LE/GATT link is not confirmed.")
        print("  This usually means a classic-only pairing. See docs/ancs.md and --pair.")


def make_advertisement(bus, dbus):
    """Export a minimal, process-lifetime ANCS service-solicitation advert."""
    class AncsAdvertisement(dbus.service.Object):
        path = "/com/frame_notify/ancs_solicitation"

        def __init__(self):
            super().__init__(bus, self.path)

        @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
        def GetAll(self, interface):
            if interface != ADVERTISEMENT:
                raise dbus.exceptions.DBusException("Unknown interface")
            return {"Type": dbus.String("peripheral"),
                    "SolicitUUIDs": dbus.Array([SERVICE_UUID], signature="s"),
                    "Discoverable": dbus.Boolean(True),
                    "LocalName": dbus.String("Frame")}

        @dbus.service.method(ADVERTISEMENT, in_signature="", out_signature="")
        def Release(self):
            print("[ANCS] Solicitation advertisement released", flush=True)

    return AncsAdvertisement()


def make_agent(bus, dbus, session):
    """Export a BlueZ pairing agent that only accepts what the user confirms in the terminal."""
    def rejected(message):
        return dbus.exceptions.DBusException(message, name=REJECTED)

    class AncsAgent(dbus.service.Object):
        path = AGENT_PATH

        def __init__(self):
            super().__init__(bus, self.path)

        @dbus.service.method(AGENT, in_signature="", out_signature="")
        def Release(self):
            session.on_agent_released()

        @dbus.service.method(AGENT, in_signature="o", out_signature="s")
        def RequestPinCode(self, device):
            raise rejected("PIN entry is not supported")

        @dbus.service.method(AGENT, in_signature="os", out_signature="")
        def DisplayPinCode(self, device, pincode):
            print(f"[ANCS] PIN code for {session.describe(device)}: {pincode}", flush=True)

        @dbus.service.method(AGENT, in_signature="o", out_signature="u")
        def RequestPasskey(self, device):
            raise rejected("passkey entry is not supported")

        @dbus.service.method(AGENT, in_signature="ouq", out_signature="")
        def DisplayPasskey(self, device, passkey, entered):
            print(f"[ANCS] Passkey for {session.describe(device)}: {format_passkey(passkey)}",
                  flush=True)

        @dbus.service.method(AGENT, in_signature="ou", out_signature="",
                             async_callbacks=("reply", "error"))
        def RequestConfirmation(self, device, passkey, reply, error):
            session.ask(device, f"does the iPhone show the code {format_passkey(passkey)}?",
                        reply, lambda: error(rejected("pairing code not confirmed")),
                        code=format_passkey(passkey))

        @dbus.service.method(AGENT, in_signature="o", out_signature="",
                             async_callbacks=("reply", "error"))
        def RequestAuthorization(self, device, reply, error):
            session.ask(device, "allow this device to pair?",
                        reply, lambda: error(rejected("pairing not allowed")))

        @dbus.service.method(AGENT, in_signature="os", out_signature="")
        def AuthorizeService(self, device, service_uuid):
            print(f"[ANCS] Refused service {service_uuid} for {session.describe(device)}; "
                  "only pairing is handled here", flush=True)
            raise rejected("service authorization is not supported")

        @dbus.service.method(AGENT, in_signature="", out_signature="")
        def Cancel(self):
            session.on_agent_cancelled()

    return AncsAgent()


class PairSession:
    """Time-limited window in which an iPhone can start a fresh LE pairing with the Frame.

    Advertises the ANCS solicitation as "Frame" so the phone lists it under Settings >
    Bluetooth, answers the pairing prompt through a terminal confirmation, and afterwards
    reports whether BlueZ sees an LE/GATT link and the ANCS service. Adapter Pairable is
    enabled only for the window and restored on exit; adapter Discoverable is left alone so
    the phone is not offered a classic Bluetooth pairing instead.

    `hooks` lets a program drive the session instead of a terminal. Any of these methods, if the
    object has them, are called: opened(seconds), confirm(phone, code, seconds) and
    prompt_expired() for the pairing code (answer with answer()), paired(address, alias),
    finished(result, address) and failed(reason, detail).
    """

    hooks = None   # class-level defaults, so partially built objects in tests behave
    match = None

    def __init__(self, bus, glib, dbus, adapter_path, hooks=None):
        self.bus = bus
        self.glib = glib
        self.dbus = dbus
        self.hooks = hooks
        self.adapter_path = adapter_path
        adapter = bus.get_object(BLUEZ, adapter_path)
        self.adapter_properties = dbus.Interface(adapter, PROPERTIES)
        self.advertising_manager = dbus.Interface(adapter, ADVERTISING_MANAGER)
        self.agent_manager = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), AGENT_MANAGER)
        self.manager = dbus.Interface(bus.get_object(BLUEZ, "/"), OBJECT_MANAGER)
        self.advertisement = None
        self.advertising = False
        self.agent = None
        self.agent_registered = False
        self.saved_properties = {}
        self.pending = None
        self.watch_id = None
        self.prompt_timer = None
        self.window_timer = None
        self.device_path = None
        self.address = None
        self.status = None
        self.checks = 0
        self.result = None
        self.failed = False
        self.failure = None
        self.window_started = None
        self.loop = None
        self.match = bus.add_signal_receiver(self.on_properties_changed,
                                             signal_name="PropertiesChanged",
                                             dbus_interface=PROPERTIES, bus_name=BLUEZ,
                                             path_keyword="path")

    def notify(self, name, *arguments):
        handler = getattr(self.hooks, name, None) if self.hooks is not None else None
        if handler is not None:
            handler(*arguments)

    def start(self):
        objects = self.manager.GetManagedObjects()
        known = paired_devices(objects)
        if known:
            print("[ANCS] Already paired in BlueZ:", flush=True)
            for _, props in known:
                print(f"         {props.get('Address', '?')}  {props.get('Alias', '?')}",
                      flush=True)
            print("[ANCS] If your iPhone is listed, that pairing is probably classic-only and will\n"
                  "       keep the phone off LE. Forget the Frame on the iPhone, run\n"
                  "       `bluetoothctl remove <address>` here (this script never removes\n"
                  "       pairings), then start --pair again.", flush=True)
        self.agent = make_agent(self.bus, self.dbus, self)
        self.agent_manager.RegisterAgent(self.agent.path, "DisplayYesNo")
        self.agent_registered = True
        try:
            self.agent_manager.RequestDefaultAgent(self.agent.path)
        except self.dbus.DBusException as error:
            print("[ANCS] Could not become the default pairing agent; another agent may "
                  "answer instead:", error, file=sys.stderr, flush=True)
        for name, value in (("Pairable", self.dbus.Boolean(True)),
                            ("PairableTimeout", self.dbus.UInt32(0))):
            current = self.adapter_properties.Get(ADAPTER, name)
            self.adapter_properties.Set(ADAPTER, name, value)
            self.saved_properties[name] = current
        self.advertisement = make_advertisement(self.bus, self.dbus)
        self.advertising_manager.RegisterAdvertisement(
            self.advertisement.path, self.dbus.Dictionary({}, signature="sv"),
            reply_handler=self.on_advertisement_ready,
            error_handler=self.on_advertisement_error)

    def on_advertisement_ready(self):
        self.advertising = True
        self.window_started = time.monotonic()
        self.window_timer = self.glib.timeout_add_seconds(PAIR_WINDOW_SECONDS,
                                                          self.on_window_closed)
        print("[ANCS] Pairing window open for "
              f"{PAIR_WINDOW_SECONDS // 60} minutes. On the iPhone open Settings > Bluetooth\n"
              "       and tap Frame under Other Devices. Press Ctrl+C to stop.", flush=True)
        self.notify("opened", PAIR_WINDOW_SECONDS)

    def seconds_left(self):
        """Seconds until the pairing window closes (0 before it opened)."""
        if self.window_started is None:
            return 0
        return max(0, int(PAIR_WINDOW_SECONDS - (time.monotonic() - self.window_started)))

    def on_advertisement_error(self, error):
        print("[ANCS] Could not advertise ANCS solicitation:", error,
              file=sys.stderr, flush=True)
        self.fail("advertising", str(error))

    def on_window_closed(self):
        self.window_timer = None
        print("[ANCS] No pairing completed within the window; stopping", file=sys.stderr,
              flush=True)
        self.fail("timeout")
        return False

    def on_agent_released(self):
        print("[ANCS] BlueZ released the pairing agent", file=sys.stderr, flush=True)
        self.agent_registered = False
        self.fail("agent")

    def on_agent_cancelled(self):
        if self.clear_prompt() is not None:
            print("\n[ANCS] BlueZ cancelled the pairing request", file=sys.stderr, flush=True)
            self.notify("prompt_expired")

    def fail(self, reason="error", detail=""):
        self.failed = True
        self.failure = (reason, detail)
        self.notify("failed", reason, detail)
        if self.loop:
            self.loop.quit()

    def device_props(self, device):
        try:
            return self.dbus.Interface(self.bus.get_object(BLUEZ, device),
                                       PROPERTIES).GetAll(DEVICE)
        except self.dbus.DBusException:
            return {}

    def describe(self, device):
        props = self.device_props(device)
        return f"{props.get('Alias', 'device')} ({props.get('Address', device)})"

    def ask(self, device, question, accept, reject, code=None):
        if self.pending is not None:
            reject()
            return
        self.pending = (accept, reject)
        if self.hooks is not None and hasattr(self.hooks, "confirm"):
            # Driven by a program: it shows the code and answers through answer().
            props = self.device_props(device)
            self.prompt_timer = self.glib.timeout_add_seconds(PROMPT_SECONDS,
                                                              self.on_prompt_timeout)
            self.notify("confirm", str(props.get("Alias", "device")), code, PROMPT_SECONDS)
            return
        print(f"[ANCS] Pairing request from {self.describe(device)}: {question} [y/N] ",
              end="", flush=True)
        self.watch_id = self.glib.io_add_watch(
            sys.stdin.fileno(), self.glib.PRIORITY_DEFAULT,
            self.glib.IO_IN | self.glib.IO_HUP, self.on_answer)
        self.prompt_timer = self.glib.timeout_add_seconds(PROMPT_SECONDS, self.on_prompt_timeout)

    def clear_prompt(self):
        """Drop the prompt on screen and return its (accept, reject) pair, if any."""
        if self.watch_id:
            self.glib.source_remove(self.watch_id)
            self.watch_id = None
        if self.prompt_timer:
            self.glib.source_remove(self.prompt_timer)
            self.prompt_timer = None
        pending, self.pending = self.pending, None
        return pending

    def answer(self, accepted):
        """Answers the pending pairing request; False when none is waiting."""
        pending = self.clear_prompt()
        if pending is None:
            return False
        pending[0 if accepted else 1]()
        return True

    def on_answer(self, _fd, _condition):
        line = sys.stdin.readline()
        answer = parse_answer(line) if line else False   # empty read: the terminal closed
        if answer is None:
            print("Please answer y or n: ", end="", flush=True)
            return True
        self.watch_id = None   # returning False below removes this source
        pending = self.clear_prompt()
        if pending:
            pending[0 if answer else 1]()
        return False

    def on_prompt_timeout(self):
        self.prompt_timer = None
        pending = self.clear_prompt()
        print("\n[ANCS] No answer in time; pairing request rejected", file=sys.stderr, flush=True)
        if pending:
            pending[1]()
            self.notify("prompt_expired")
        return False

    def on_properties_changed(self, interface, changed, _invalidated, path=None):
        path = str(path)
        if (interface != DEVICE or not bool(changed.get("Paired", False)) or
                self.device_path or not path.startswith(self.adapter_path + "/")):
            return
        self.device_path = path
        props = self.device_props(path)
        self.address = str(props.get("Address", path))
        print(f"[ANCS] Paired with {props.get('Alias', 'device')} ({self.address}). "
              "Checking for an LE/GATT link...", flush=True)
        if self.window_timer:
            self.glib.source_remove(self.window_timer)
            self.window_timer = None
        self.notify("paired", self.address, str(props.get("Alias", "device")))
        self.glib.timeout_add_seconds(LINK_CHECK_SECONDS, self.check_link)

    def check_link(self):
        self.checks += 1
        try:
            status = link_status(self.manager.GetManagedObjects(), self.device_path)
        except self.dbus.DBusException as error:
            print("[ANCS] Could not inspect the paired device:", error,
                  file=sys.stderr, flush=True)
            self.fail("error", str(error))
            return False
        if status != self.status:
            print("[ANCS] Link:", {"none": "no GATT services yet",
                                   "gatt": "GATT services resolved, no ANCS yet",
                                   "ancs": "ANCS service resolved"}[status], flush=True)
            self.status = status
        if status != "ancs" and self.checks < LINK_CHECKS:
            return True
        self.result = status
        self.report()
        self.notify("finished", status, self.address)
        if self.loop:
            self.loop.quit()
        return False

    def report(self):
        address = self.address
        run =f"python3 ./scripts/ancs_bridge.py --device {address} --solicit"
        if self.result == "ancs":
            print("[ANCS] Done: the iPhone exposes ANCS over LE. If Settings > Bluetooth >\n"
                  "       Frame offers Share System Notifications, turn it on. Then run:\n"
                  f"       {run}", flush=True)
        elif self.result == "gatt":
            print("[ANCS] Paired, and an LE/GATT link exists, but ANCS is not exposed yet.\n"
                  "       Turn on Share System Notifications for Frame in Settings > Bluetooth,\n"
                  f"       then check with: python3 ./scripts/ancs_bridge.py --diagnose {address}",
                  flush=True)
        else:
            print("[ANCS] Paired, but BlueZ sees no GATT services, so the phone probably paired\n"
                  "       over classic Bluetooth. Forget the Frame on the iPhone, run\n"
                  f"       `bluetoothctl remove {address}`, and start --pair again from the iPhone.",
                  flush=True)

    def exit_code(self):
        if self.result == "ancs":
            return 0
        return 2 if self.result in ("gatt", "none") else 1

    def stop(self):
        self.clear_prompt()
        if self.window_timer:
            self.glib.source_remove(self.window_timer)
            self.window_timer = None
        if self.advertising:
            try:
                self.advertising_manager.UnregisterAdvertisement(self.advertisement.path,
                                                                  timeout=3)
            except self.dbus.DBusException as error:
                print("[ANCS] Could not explicitly unregister advertisement:", error,
                      file=sys.stderr, flush=True)
            self.advertising = False
        if self.agent_registered:
            try:
                self.agent_manager.UnregisterAgent(self.agent.path)
            except self.dbus.DBusException as error:
                print("[ANCS] Could not unregister pairing agent:", error,
                      file=sys.stderr, flush=True)
            self.agent_registered = False
        for name, value in self.saved_properties.items():
            try:
                self.adapter_properties.Set(ADAPTER, name, value)
            except self.dbus.DBusException as error:
                print(f"[ANCS] Could not restore adapter {name}:", error,
                      file=sys.stderr, flush=True)
        if self.saved_properties:
            print("[ANCS] Adapter pairing settings restored", flush=True)
        self.saved_properties.clear()
        # A long-running program starts several sessions: release what this one registered.
        if self.match is not None:
            try:
                self.match.remove()
            except Exception:  # never let cleanup stop the caller
                pass
            self.match = None
        for exported in (self.agent, self.advertisement):
            remove = getattr(exported, "remove_from_connection", None)
            if remove is not None:
                try:
                    remove()
                except Exception:
                    pass
        self.agent = self.advertisement = None


class Bridge:
    on_status = None   # optional callable(state, detail) for a program that supervises the bridge
    stopped = False    # class-level defaults keep partially built objects in tests working
    periodic_id = None
    matches = ()
    seen = None        # optional SeenNotifications: what the Frame has been sent before

    def __init__(self, bus, glib, dbus, address, socket_path, solicit=False):
        self.bus = bus
        self.glib = glib
        self.dbus = dbus
        self.address = address.upper()
        self.socket_path = socket_path
        self.solicit = solicit
        self.session = uuid.uuid4().hex[:12]
        self.device_path = None
        self.device_proxy = None
        self.device_props = None
        self.advertising_manager = None
        self.advertisement = None
        self.advertising = False
        self.service_path = None
        self.data_path = None
        self.source_path = None
        self.control_path = None
        self.subscribed = False
        self.subscribing = False
        self.connecting = False
        self.queue = deque()
        self.queued = set()
        self.removed = set()
        self.active = None
        self.response = None
        self.timeout_id = None
        self.pending_attributes = None   # notification attributes waiting for the app's name
        self.app_names = {}              # bundle identifier -> display name ("" if unknown)
        self.hold_timer_id = None
        self.loop = None
        self.failed = False
        self.reported_unavailable = False
        self.init_tracking()

        self.manager = dbus.Interface(bus.get_object(BLUEZ, "/"), OBJECT_MANAGER)
        self.matches = [
            bus.add_signal_receiver(self.on_properties_changed, signal_name="PropertiesChanged",
                                    dbus_interface=PROPERTIES, bus_name=BLUEZ,
                                    path_keyword="path"),
            bus.add_signal_receiver(self.on_interfaces_added, signal_name="InterfacesAdded",
                                    dbus_interface=OBJECT_MANAGER, bus_name=BLUEZ),
            bus.add_signal_receiver(self.on_interfaces_removed, signal_name="InterfacesRemoved",
                                    dbus_interface=OBJECT_MANAGER, bus_name=BLUEZ),
        ]

    def init_tracking(self):
        """What is known about the iPhone's notifications; UIDs are only good for one connection."""
        self.uid_flags = {}        # uid -> Notification Source event flags
        self.uid_by_key = {}       # notification key -> uid, for what has been read this connection
        self.preexisting = set()   # uids that were already on the iPhone when this connection began
        self.indexed = set()       # ...of which these have been read (so they are found by key)
        self.lookups = deque()     # older uids waiting to be read, after the live notifications
        self.lookup_uids = set()   # uids being read only to find one to clear
        self.pending_clear = set() # keys the Frame cleared that have no uid yet

    def status(self, state, detail=""):
        if self.on_status is not None:
            self.on_status(state, detail)

    def start(self):
        objects = self.manager.GetManagedObjects()
        matches = [(path, props) for path, props in paired_devices(objects)
                   if str(props.get("Address", "")).upper() == self.address]
        if len(matches) != 1:
            raise RuntimeError("selected iPhone is not uniquely paired in BlueZ; run --list")
        self.device_path, props = matches[0]
        self.device_props = props
        self.device_proxy = self.bus.get_object(BLUEZ, self.device_path)
        print("[ANCS] Selected paired device", self.device_path, flush=True)
        if self.solicit:
            adapter_path = str(props.get("Adapter", ""))
            if ADVERTISING_MANAGER not in objects.get(adapter_path, {}):
                raise RuntimeError("selected adapter has no BlueZ LE advertising manager")
            self.advertising_manager = self.dbus.Interface(
                self.bus.get_object(BLUEZ, adapter_path), ADVERTISING_MANAGER)
            self.advertisement = make_advertisement(self.bus, self.dbus)
            self.advertising_manager.RegisterAdvertisement(
                self.advertisement.path, self.dbus.Dictionary({}, signature="sv"),
                reply_handler=self.on_advertisement_ready,
                error_handler=self.on_advertisement_error)
        else:
            self.begin_connection()

    def begin_connection(self):
        self.refresh(self.device_props)
        self.periodic_id = self.glib.timeout_add_seconds(10, self.periodic_refresh)

    def on_advertisement_ready(self):
        self.advertising = True
        if self.stopped:
            return
        print("[ANCS] Advertising temporary ANCS service solicitation", flush=True)
        self.begin_connection()

    def on_advertisement_error(self, error):
        print("[ANCS] Could not advertise ANCS solicitation:", error,
              file=sys.stderr, flush=True)
        self.failed = True
        if self.loop:
            self.loop.quit()

    def stop(self):
        """Stops for good: no timers, signals or advertisement are left behind."""
        self.stopped = True
        for name in ("timeout_id", "hold_timer_id", "periodic_id"):
            source = getattr(self, name)
            if source:
                self.glib.source_remove(source)
                setattr(self, name, None)
        for match in self.matches:
            try:
                match.remove()
            except Exception:  # cleanup must never stop the caller
                pass
        self.matches = ()
        if self.subscribed:
            for path in (self.source_path, self.data_path):
                try:
                    self.dbus.Interface(self.bus.get_object(BLUEZ, path),
                                        CHARACTERISTIC).StopNotify(timeout=2)
                except Exception:
                    pass
        self.subscribed = self.subscribing = False
        self.queue.clear()
        self.queued.clear()
        self.lookups.clear()
        self.lookup_uids.clear()
        self.pending_clear.clear()
        self.active = self.response = self.pending_attributes = None
        if self.advertising:
            try:
                self.advertising_manager.UnregisterAdvertisement(self.advertisement.path,
                                                                  timeout=3)
            except self.dbus.DBusException as error:
                print("[ANCS] Could not explicitly unregister advertisement:", error,
                      file=sys.stderr, flush=True)
            self.advertising = False
        remove = getattr(self.advertisement, "remove_from_connection", None)
        if remove is not None:
            try:
                remove()
            except Exception:
                pass

    def periodic_refresh(self):
        if self.stopped:
            return False
        if self.device_path:
            try:
                props = self.dbus.Interface(self.device_proxy, PROPERTIES).GetAll(DEVICE)
                self.refresh(props)
            except self.dbus.DBusException as error:
                print("[ANCS] Device check failed:", error, file=sys.stderr, flush=True)
        return True

    def refresh(self, props):
        if self.stopped:
            return
        if not bool(props.get("Connected", False)):
            self.reported_unavailable = False
            self.reset_session()
            if not self.connecting:
                self.connecting = True
                print("[ANCS] Connecting to paired iPhone...", flush=True)
                self.status("connecting")
                self.dbus.Interface(self.device_proxy, DEVICE).Connect(
                    reply_handler=self.on_connected, error_handler=self.on_connect_error)
            return
        if bool(props.get("ServicesResolved", False)):
            self.discover()

    def on_connected(self):
        self.connecting = False
        print("[ANCS] Bluetooth connected; waiting for GATT services", flush=True)
        self.status("waiting")

    def on_connect_error(self, error):
        self.connecting = False
        print("[ANCS] Connect failed:", error, file=sys.stderr, flush=True)

    def reset_session(self):
        if not (self.subscribed or self.subscribing or self.active or self.queue):
            return
        print("[ANCS] Session ended; waiting to reconnect", flush=True)
        self.status("disconnected")
        if self.timeout_id:
            self.glib.source_remove(self.timeout_id)
            self.timeout_id = None
        if self.hold_timer_id:
            self.glib.source_remove(self.hold_timer_id)
            self.hold_timer_id = None
        self.pending_attributes = None
        self.subscribed = False
        self.subscribing = False
        self.service_path = self.data_path = self.source_path = self.control_path = None
        self.queue.clear()
        self.queued.clear()
        self.removed.clear()
        self.init_tracking()
        self.active = self.response = None
        self.session = uuid.uuid4().hex[:12]
        self.reported_unavailable = False

    def discover(self):
        if self.stopped or self.subscribed or self.subscribing:
            return
        objects = self.manager.GetManagedObjects()
        gatt_services = [(str(path), interfaces[SERVICE]) for path, interfaces in objects.items()
                         if SERVICE in interfaces and
                         str(interfaces[SERVICE].get("Device", "")) == self.device_path]
        services = [path for path, props in gatt_services
                    if str(props.get("UUID", "")).lower() == SERVICE_UUID]
        if len(services) != 1:
            self.status("no_ancs" if gatt_services else "no_gatt")
            if not self.reported_unavailable:
                message = ("Bluetooth connected, but no remote GATT services; an LE link "
                           "is not confirmed (likely a classic-only pairing; see --pair)"
                           if not gatt_services else
                           "No ANCS GATT service; run --diagnose to inspect the UUIDs")
                print("[ANCS] " + message,
                      file=sys.stderr, flush=True)
                self.reported_unavailable = True
            return
        paths = {}
        for path, interfaces in objects.items():
            if CHARACTERISTIC not in interfaces:
                continue
            props = interfaces[CHARACTERISTIC]
            if str(props.get("Service", "")) == services[0]:
                paths[str(props.get("UUID", "")).lower()] = str(path)
        if any(key not in paths for key in (NOTIFICATION_SOURCE_UUID, DATA_SOURCE_UUID,
                                            CONTROL_POINT_UUID)):
            self.status("no_ancs", "characteristics")
            if not self.reported_unavailable:
                print("[ANCS] Required ANCS characteristics are not available",
                      file=sys.stderr, flush=True)
                self.reported_unavailable = True
            return
        self.reported_unavailable = False
        self.service_path = services[0]
        self.source_path = paths[NOTIFICATION_SOURCE_UUID]
        self.data_path = paths[DATA_SOURCE_UUID]
        self.control_path = paths[CONTROL_POINT_UUID]
        self.subscribing = True
        # Data Source must be ready before Notification Source starts sending UIDs.
        self.dbus.Interface(self.bus.get_object(BLUEZ, self.data_path), CHARACTERISTIC).StartNotify(
            reply_handler=self.on_data_ready, error_handler=self.on_subscribe_error)

    def on_data_ready(self):
        self.dbus.Interface(self.bus.get_object(BLUEZ, self.source_path), CHARACTERISTIC).StartNotify(
            reply_handler=self.on_source_ready, error_handler=self.on_subscribe_error)

    def on_source_ready(self):
        self.subscribing = False
        self.subscribed = True
        print("[ANCS] Listening for iPhone notifications", flush=True)
        self.status("listening")

    def on_subscribe_error(self, error):
        self.subscribing = False
        print("[ANCS] Could not subscribe:", error, file=sys.stderr, flush=True)
        self.status("subscribe_failed", str(error))
        self.failed = True
        if self.loop:
            self.loop.quit()

    def on_properties_changed(self, interface, changed, _invalidated, path=None):
        if self.stopped:
            return
        path = str(path)
        if path == self.device_path and interface == DEVICE:
            try:
                props = self.dbus.Interface(self.device_proxy, PROPERTIES).GetAll(DEVICE)
                self.refresh(props)
            except self.dbus.DBusException as error:
                print("[ANCS] Device state unavailable:", error,
                      file=sys.stderr, flush=True)
        if interface != CHARACTERISTIC or "Value" not in changed:
            return
        value = bytes(changed["Value"])
        if path == self.source_path:
            self.on_event(value)
        elif path == self.data_path:
            self.on_data(value)

    def on_interfaces_added(self, path, _interfaces):
        if not self.stopped and self.device_path and str(path).startswith(self.device_path + "/"):
            self.glib.idle_add(self.discover)

    def on_interfaces_removed(self, path, _interfaces):
        if not self.stopped and self.service_path and (str(path) == self.service_path or
                                  str(path).startswith(self.service_path + "/")):
            self.reset_session()

    def on_event(self, value):
        try:
            event = parse_event(value)
        except ValueError as error:
            print("[ANCS] Ignoring malformed event:", error, file=sys.stderr, flush=True)
            return
        if event.kind == REMOVED:
            if self.active and self.active[0] == event.uid:
                self.removed.add(event.uid)
            else:
                self.lookup_uids.discard(event.uid)
            self.queue = deque(item for item in self.queue if item[0] != event.uid)
            self.lookups = deque(item for item in self.lookups if item[0] != event.uid)
            self.queued.discard(event.uid)
            self.forget_uid(event.uid)
            self.end_lookup_if_done()
            return
        # ANCS UIDs are session-local. Ignore modifications and pre-existing
        # notifications so reconnecting does not replay old iPhone alerts.
        if event.kind != ADDED or event.flags & FLAG_PRE_EXISTING:
            # Say so: this is the only sign that the iPhone is sending anything at all.
            reason = "modified" if event.kind != ADDED else "already there when connecting"
            print(f"[ANCS] Notification event uid={event.uid} flags=0x{event.flags:02x} ignored ({reason})",
                  flush=True)
            # Remember it all the same: clearing it on the iPhone needs its uid and flags.
            if event.kind == MODIFIED and event.uid in self.uid_flags:
                self.uid_flags[event.uid] = event.flags
            elif event.kind == ADDED:
                self.uid_flags[event.uid] = event.flags
                self.preexisting.add(event.uid)
            return
        print(f"[ANCS] New notification event uid={event.uid} flags=0x{event.flags:02x}" +
              (" (marked silent by the iPhone)" if event.flags & FLAG_SILENT else ""), flush=True)
        self.uid_flags[event.uid] = event.flags
        if event.uid in self.queued or (self.active and self.active[0] == event.uid):
            return
        if len(self.queue) >= 100:
            print("[ANCS] Pending queue full; dropping event", file=sys.stderr, flush=True)
            return
        self.removed.discard(event.uid)
        self.queue.append((event.uid, bool(event.flags & FLAG_SILENT)))
        self.queued.add(event.uid)
        self.next_request()

    def next_request(self):
        if self.active is not None or self.hold_timer_id is not None:
            return
        # What just arrived comes first; reading old notifications only finds one to clear.
        while self.queue or self.lookups:
            uid, silent = (self.queue or self.lookups).popleft()
            self.queued.discard(uid)
            if uid in self.removed:
                self.lookup_uids.discard(uid)
                continue
            self.active = (uid, silent)
            self.response = AttributeResponse(uid)
            self.timeout_id = self.glib.timeout_add_seconds(NOTIFICATION_TIMEOUT_SECONDS,
                                                            self.on_timeout)
            self.write_control_point(attribute_request(uid))
            return
        self.end_lookup_if_done()

    def write_control_point(self, request, error_handler=None):
        self.dbus.Interface(self.bus.get_object(BLUEZ, self.control_path),
                            CHARACTERISTIC).WriteValue(
            self.dbus.Array(request, signature="y"),
            self.dbus.Dictionary({"type": self.dbus.String("request")}, signature="sv"),
            reply_handler=lambda: None, error_handler=error_handler or self.on_write_error)

    def request_app_name(self, app_identifier, attributes):
        """Asks the iPhone for an app's display name; False if the request cannot be made."""
        try:
            request = app_attributes_request(app_identifier)
        except ValueError:
            self.app_names[app_identifier] = ""
            return False
        if self.timeout_id:
            self.glib.source_remove(self.timeout_id)
        self.pending_attributes = attributes
        self.response = AppAttributeResponse(app_identifier)
        self.timeout_id = self.glib.timeout_add_seconds(APP_NAME_TIMEOUT_SECONDS,
                                                        self.on_app_name_timeout)
        self.write_control_point(request)
        return True

    def forward_pending(self):
        uid, silent = self.active
        attributes, self.pending_attributes = self.pending_attributes, None
        if uid not in self.removed:
            self.forward(uid, silent, attributes)

    def abandon_app_name(self, reason):
        """Gives up on a display name: the notification is still shown, under its identifier."""
        print(f"[ANCS] {reason}; showing the app identifier instead", file=sys.stderr, flush=True)
        if self.timeout_id:
            self.glib.source_remove(self.timeout_id)
            self.timeout_id = None
        self.app_names[self.response.app_identifier] = ""
        self.forward_pending()
        self.removed.discard(self.active[0])
        self.active = self.response = None
        # Late fragments are ignored while nothing is active; pausing keeps one from landing in
        # the next request's response.
        self.hold_timer_id = self.glib.timeout_add_seconds(APP_NAME_COOLDOWN_SECONDS,
                                                           self.release_hold)

    def release_hold(self):
        self.hold_timer_id = None
        if self.subscribed:
            self.next_request()
        return False

    def on_app_name_timeout(self):
        self.timeout_id = None
        self.abandon_app_name("App name request timed out")
        return False

    def finish_request(self):
        if self.active:
            self.removed.discard(self.active[0])
            self.lookup_uids.discard(self.active[0])
        if self.timeout_id:
            self.glib.source_remove(self.timeout_id)
            self.timeout_id = None
        self.active = self.response = None
        self.next_request()

    def on_write_error(self, error):
        print("[ANCS] Attribute request failed:", error, file=sys.stderr, flush=True)
        if self.pending_attributes is not None:
            # Only the app-name lookup failed: the notification itself is still worth showing.
            self.app_names[self.response.app_identifier] = ""
            self.forward_pending()
        self.finish_request()

    def on_timeout(self):
        self.timeout_id = None
        print("[ANCS] Attribute request timed out; restart the bridge to resynchronize",
              file=sys.stderr, flush=True)
        # A late fragment cannot safely be distinguished from a new response.
        self.failed = True
        if self.loop:
            self.loop.quit()
        return False

    def on_data(self, value):
        if self.response is None:
            return
        try:
            result = self.response.feed(value)
        except ValueError as error:
            if isinstance(self.response, AppAttributeResponse):
                self.abandon_app_name(f"Invalid app name response ({error})")
                return
            print("[ANCS] Invalid attribute response:", error, file=sys.stderr, flush=True)
            self.failed = True
            if self.loop:
                self.loop.quit()
            return
        if result is None:
            return
        if isinstance(self.response, AppAttributeResponse):
            self.app_names[self.response.app_identifier] = result.strip()
            self.forward_pending()
            self.finish_request()
            return
        if self.active and self.active[0] in self.lookup_uids:
            self.finish_lookup(self.active[0], result)
            self.finish_request()
            return
        # The notification's attributes are complete. Ask for the app's display name once per app.
        app_identifier = result.get(0)
        if (app_identifier and app_identifier not in self.app_names and
                self.request_app_name(app_identifier, result)):
            return
        uid, silent = self.active
        if uid not in self.removed:
            self.forward(uid, silent, result)
        self.finish_request()

    def forward(self, uid, silent, attributes):
        key = notification_key(attributes)
        if key is not None:
            self.uid_by_key[key] = uid
            if self.seen is not None and key in self.seen:
                # Sent before, whether or not it is still on the Frame: it is not brought back.
                print(f"[ANCS] Notification uid={uid} was sent before; not sending it again",
                      flush=True)
                return
        app_id = attributes.get(0) or ""
        app = self.app_names.get(app_id) or app_id or "iPhone"
        title = attributes.get(1) or attributes.get(2) or app
        message = attributes.get(3) or attributes.get(2) or title
        timestamp = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
        # The iPhone marks some notifications silent (delivered quietly, for example while it is
        # muted or in a Focus). In the headset they are worth seeing all the same, so they get a
        # toast too, unless FRAME_NOTIFY_QUIET_NO_TOAST is set.
        quiet = silent and os.environ.get("FRAME_NOTIFY_QUIET_NO_TOAST", "") not in ("", "0")
        # The key is the same on every connection, which is what lets the Frame recognise a
        # notification it already has. Without one, the id is only good for this connection.
        notification_id = (f"{ID_PREFIX}{key}" if key is not None
                           else f"{ID_PREFIX}{self.session}-{uid}")
        event = {"type": "notification", "id": notification_id,
                 "app": app, "title": title, "message": message,
                 "timestamp": timestamp, "toast": "false" if quiet else "true"}
        if silent:
            event["silent"] = "true"
        if app_id:
            event["app_id"] = app_id
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(2)
                connection.connect(self.socket_path)
                connection.sendall((json.dumps(event, ensure_ascii=False) + "\n").encode("utf-8"))
            print(f"[ANCS] Forwarded notification uid={uid} toast={event['toast']}", flush=True)
        except OSError as error:
            print("[ANCS] Frame Notify socket unavailable:", error,
                  file=sys.stderr, flush=True)
            return
        if key is not None and self.seen is not None:
            self.seen.add(key)

    # ---- clearing on the iPhone ---------------------------------------------------------------

    def forget_uid(self, uid):
        self.uid_flags.pop(uid, None)
        self.preexisting.discard(uid)
        self.indexed.discard(uid)
        for key in [key for key, known in self.uid_by_key.items() if known == uid]:
            del self.uid_by_key[key]

    def find_uid(self, notification_id):
        """(uid, key) for one of the ids this bridge handed out; each is None when not known."""
        if not notification_id.startswith(ID_PREFIX):
            return None, None
        name = notification_id[len(ID_PREFIX):]
        if KEY_ID.fullmatch(name):
            return self.uid_by_key.get(name), name
        match = SESSION_ID.fullmatch(name)
        if match and name.startswith(self.session + "-") and int(match.group(1)) <= 0xFFFFFFFF:
            return int(match.group(1)), None     # the form without a key, good for this connection
        return None, None

    def clear_on_phone(self, notification_ids):
        """Clears the notifications the Frame has just cleared on the iPhone too, where it allows."""
        if not self.subscribed or self.control_path is None:
            print("[ANCS] Not connected to the iPhone; its notifications are left as they are",
                  flush=True)
            return
        unknown = set()
        for notification_id in notification_ids:
            uid, key = self.find_uid(notification_id)
            if uid is not None:
                self.perform_negative_action(uid)
            elif key is not None:
                unknown.add(key)
        if unknown:
            # Sent in an earlier connection, so this one has another uid for it, if the iPhone
            # still has it at all: read the older notifications to find out.
            self.pending_clear |= unknown
            self.start_lookup()

    def perform_negative_action(self, uid):
        flags = self.uid_flags.get(uid)
        if flags is None or not flags & FLAG_NEGATIVE_ACTION:
            print(f"[ANCS] The iPhone offers no way to clear notification uid={uid}", flush=True)
            return
        try:
            request = perform_action_request(uid, ACTION_NEGATIVE)
        except ValueError:
            return
        print(f"[ANCS] Clearing notification uid={uid} on the iPhone", flush=True)
        self.write_control_point(request, self.on_action_error)

    def on_action_error(self, error):
        # Nothing else depends on this: the notification simply stays on the iPhone.
        print("[ANCS] The iPhone did not clear a notification:", error, file=sys.stderr, flush=True)

    def start_lookup(self):
        waiting = sorted((uid for uid in self.preexisting
                          if uid not in self.indexed and uid not in self.lookup_uids), reverse=True)
        for uid in waiting[:LOOKUP_LIMIT]:
            self.lookups.append((uid, False))
            self.lookup_uids.add(uid)
        self.end_lookup_if_done()
        self.next_request()

    def finish_lookup(self, uid, attributes):
        self.indexed.add(uid)
        if uid in self.removed:
            return
        key = notification_key(attributes)
        if key is None:
            return
        self.uid_by_key[key] = uid
        if key in self.pending_clear:
            self.pending_clear.discard(key)
            self.perform_negative_action(uid)

    def end_lookup_if_done(self):
        """Gives up on clearing what was not found once there is nothing left to read."""
        if self.pending_clear and not self.lookup_uids:
            print(f"[ANCS] {len(self.pending_clear)} cleared notification(s) are not on the iPhone "
                  "any more", flush=True)
            self.pending_clear.clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true", help="list already paired BlueZ devices")
    group.add_argument("--diagnose", metavar="MAC",
                       help="inspect a device and its resolved GATT services without connecting")
    group.add_argument("--device", metavar="MAC", help="paired iPhone Bluetooth address")
    group.add_argument("--pair", action="store_true",
                       help="open a 5-minute window in which the iPhone can start a new LE "
                            "pairing; asks you to confirm the code in this terminal")
    group.add_argument("--service", action="store_true",
                       help="run as the helper that Frame Notify starts: pairing and the iPhone "
                            "bridge, controlled with JSON lines on stdin/stdout "
                            "(see ancs_service.py)")
    parser.add_argument("--solicit", action="store_true",
                        help="temporarily advertise ANCS service solicitation while running")
    parser.add_argument("--adapter", metavar="HCI",
                        help="with --pair or --service: adapter to use, such as hci0 "
                             "(default: the only one)")
    args = parser.parse_args()
    if args.solicit and not args.device:
        parser.error("--solicit requires --device")
    if args.adapter and not ((args.pair or args.service) and
                             re.fullmatch(r"hci\d+", args.adapter)):
        parser.error("--adapter requires --pair or --service and a name such as hci0")
    address = args.device or args.diagnose
    if address and not re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", address):
        parser.error("address must be a Bluetooth address such as AA:BB:CC:DD:EE:FF")
    try:
        import dbus
        import dbus.service
        from dbus.mainloop.glib import DBusGMainLoop
        from gi.repository import GLib
    except ImportError as error:
        message = f"ANCS bridge needs dbus-python and PyGObject on the Frame: {error}"
        if args.service:
            # The program that started the helper reads this line and shows it to the user.
            print(json.dumps({"event": "fatal", "message": message}), flush=True)
            sys.exit(1)
        parser.exit(1, message + "\n")
    DBusGMainLoop(set_as_default=True)
    if args.service:
        from ancs_service import run_service
        return run_service(dbus, GLib, args.adapter)
    try:
        bus = dbus.SystemBus()
        manager = dbus.Interface(bus.get_object(BLUEZ, "/"), OBJECT_MANAGER)
        if args.list:
            for _, props in paired_devices(manager.GetManagedObjects()):
                print(f"{props.get('Address', '?')}  {props.get('Alias', '?')}"
                      f"  connected={bool(props.get('Connected', False))}")
            return 0
        if args.diagnose:
            diagnose_device(manager.GetManagedObjects(), args.diagnose)
            return 0
        if args.pair:
            if not sys.stdin.isatty():
                parser.exit(1, "--pair asks you to confirm a pairing code; run it in an "
                               "interactive terminal (for example ssh -t)\n")
            session = PairSession(bus, GLib, dbus,
                                  pick_adapter(manager.GetManagedObjects(), args.adapter))
            session.loop = GLib.MainLoop()
            signal.signal(signal.SIGINT, lambda _signum, _frame: session.loop.quit())
            signal.signal(signal.SIGTERM, lambda _signum, _frame: session.loop.quit())
            try:
                session.start()
                session.loop.run()
            except KeyboardInterrupt:
                pass
            finally:
                session.stop()
            return session.exit_code()
        runtime_dir =os.environ.get("XDG_RUNTIME_DIR")
        if not runtime_dir:
            parser.exit(1, "XDG_RUNTIME_DIR is not set; start Frame Notify in a user session\n")
        bridge = Bridge(bus, GLib, dbus, args.device,
                        os.path.join(runtime_dir, "frame-notify.sock"), solicit=args.solicit)
        bridge.loop = GLib.MainLoop()
        bridge.start()
        signal.signal(signal.SIGINT, lambda _signum, _frame: bridge.loop.quit())
        signal.signal(signal.SIGTERM, lambda _signum, _frame: bridge.loop.quit())
        try:
            bridge.loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            bridge.stop()
        return 1 if bridge.failed else 0
    except (RuntimeError, dbus.DBusException) as error:
        parser.exit(1, f"ANCS bridge: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
