"""Offline tests for the Bluetooth helper service; no Bluetooth or D-Bus required."""

import json
import os
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

from ancs_bridge import (ADAPTER, ADVERTISING_MANAGER, DEVICE, OBJECT_MANAGER, PROPERTIES)
import ancs_service
from ancs_service import (BRIDGE_RESTART_SECONDS, COMMANDS, PERMISSION_RETRY_SECONDS,
                          REPAIR_AFTER_SECONDS, CommandReader, PhoneConfig, Service,
                          find_phone_devices, run_service, state_directory)

ADAPTER_PATH = "/org/bluez/hci0"
PHONE = "AA:BB:CC:DD:EE:01"
OTHER = "AA:BB:CC:DD:EE:02"


def device_path(address):
    return ADAPTER_PATH + "/dev_" + address.replace(":", "_")


def device(address, alias="iPhone", paired=True, icon=None):
    props = {"Address": address, "Alias": alias, "Paired": paired}
    if icon:
        props["Icon"] = icon
    return device_path(address), {DEVICE: props}


def adapter(powered=True):
    return ADAPTER_PATH, {ADAPTER: {"Powered": powered}, ADVERTISING_MANAGER: {}}


class FakeDBus:
    """Just enough of dbus-python for the service: a BlueZ object tree and a call log."""

    class DBusException(Exception):
        pass

    def __init__(self, objects):
        self.objects = dict(objects)
        self.calls = []
        self.manager_error = None
        self.remove_error = None

    @staticmethod
    def Boolean(value):
        return bool(value)

    def Interface(self, proxy, name):
        return FakeInterface(self, proxy, name)

    def get_object(self, _service, path):
        return SimpleProxy(path)


class SimpleProxy:
    def __init__(self, path):
        self.path = path


class FakeBus:
    def get_object(self, _service, path):
        return SimpleProxy(path)


class FakeInterface:
    def __init__(self, dbus, proxy, name):
        self.dbus, self.proxy, self.name = dbus, proxy, name

    def GetManagedObjects(self):
        if self.dbus.manager_error:
            raise FakeDBus.DBusException(self.dbus.manager_error)
        return self.dbus.objects

    def RemoveDevice(self, path):
        self.dbus.calls.append(("RemoveDevice", path))
        if self.dbus.remove_error:
            raise FakeDBus.DBusException(self.dbus.remove_error)
        self.dbus.objects.pop(path, None)

    def Set(self, interface, name, value):
        self.dbus.calls.append(("Set", self.proxy.path, interface, name, value))
        if interface == ADAPTER and name == "Powered":
            self.dbus.objects[self.proxy.path][ADAPTER]["Powered"] = value


class FakeGLib:
    def __init__(self):
        self.next_id = 1
        self.timers = {}

    def timeout_add_seconds(self, seconds, callback):
        source_id, self.next_id = self.next_id, self.next_id + 1
        self.timers[source_id] = (seconds, callback)
        return source_id

    def source_remove(self, source_id):
        del self.timers[source_id]    # removing an unknown source would be a GLib critical

    def fire(self, source_id):
        _seconds, callback = self.timers[source_id]
        if not callback():
            self.timers.pop(source_id, None)

    def delays(self):
        return sorted(seconds for seconds, _callback in self.timers.values())


class FakeBridge:
    instances = []

    def __init__(self, _bus, _glib, _dbus, address, socket_path, solicit=False):
        self.address, self.socket_path, self.solicit = address, socket_path, solicit
        self.on_status = None
        self.loop = None
        self.started = self.stopped = False
        self.start_error = None
        FakeBridge.instances.append(self)

    def start(self):
        self.started = True
        if self.start_error:
            raise self.start_error

    def stop(self):
        self.stopped = True


class FakePair:
    instances = []

    def __init__(self, _bus, _glib, _dbus, adapter_path, hooks=None):
        self.adapter_path, self.hooks = adapter_path, hooks
        self.loop = None
        self.result = self.address = self.failure = None
        self.started = self.stopped = False
        self.start_error = None
        self.answers = []
        self.answer_result = True
        FakePair.instances.append(self)

    def start(self):
        self.started = True
        if self.start_error:
            raise self.start_error

    def stop(self):
        self.stopped = True

    def answer(self, accepted):
        self.answers.append(accepted)
        return self.answer_result

    def seconds_left(self):
        return 210


class ServiceTestCase(unittest.TestCase):
    def setUp(self):
        FakeBridge.instances = []
        FakePair.instances = []
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.config = PhoneConfig(self.directory.name)
        self.events = []
        self.glib = FakeGLib()
        self.bus = FakeBus()

    def build(self, *entries, phone=None, powered=True):
        objects = dict([adapter(powered)] + list(entries))
        self.dbus = FakeDBus(objects)
        if phone:
            self.config.save(*phone)
        self.service = Service(self.bus, self.glib, self.dbus, "/run/user/1/frame-notify.sock",
                               self.config, self.events.append, bridge_factory=FakeBridge,
                               pair_factory=FakePair)
        self.service.loop = MagicMock()
        return self.service

    def command(self, name, **fields):
        self.service.handle_line(json.dumps(dict(command=name, **fields)).encode())

    def states(self):
        return [(event["state"], event) for event in self.events if event["event"] == "state"]

    def state(self):
        return self.states()[-1][1]

    def names(self):
        return [name for name, _event in self.states()]


class StartupTests(ServiceTestCase):
    def test_a_fresh_install_waits_for_a_phone_to_be_paired(self):
        self.build().start()
        self.assertEqual(self.events[0], {"event": "hello", "version": "1"})
        self.assertEqual(self.names(), ["unpaired"])
        self.assertEqual(FakeBridge.instances, [])

    def test_a_remembered_phone_is_reconnected_and_progress_is_reported(self):
        self.build(device(PHONE), phone=(PHONE, "Travel iPhone")).start()
        bridge = FakeBridge.instances[0]
        self.assertEqual((bridge.address, bridge.solicit, bridge.started), (PHONE, True, True))
        self.assertEqual(bridge.socket_path, "/run/user/1/frame-notify.sock")
        self.assertEqual(self.state()["state"], "connecting")
        self.assertEqual((self.state()["phone"], self.state()["address"]), ("Travel iPhone", PHONE))

        bridge.on_status("waiting", "")
        bridge.on_status("listening", "")
        self.assertEqual(self.state()["state"], "connected")
        bridge.on_status("disconnected", "")
        self.assertEqual(self.state()["state"], "connecting")
        bridge.on_status("listening", "")
        self.assertEqual(self.state()["state"], "connected")

    def test_the_same_state_is_not_announced_twice(self):
        self.build(device(PHONE), phone=(PHONE, "iPhone")).start()
        bridge = FakeBridge.instances[0]
        bridge.on_status("listening", "")
        count = len(self.events)
        bridge.on_status("listening", "")
        self.assertEqual(len(self.events), count)
        self.command("status")                      # but a status request always answers
        self.assertEqual(len(self.events), count + 1)
        self.assertEqual(self.events[-1], self.events[-2])

    def test_bluetooth_problems_are_explained(self):
        self.build(powered=False).start()
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("no_bluetooth", "powered_off"))

        self.events.clear()
        service = self.build()
        del self.dbus.objects[ADAPTER_PATH]
        service.start()
        self.assertEqual(self.state()["reason"], "no_adapter")

        self.events.clear()
        service = self.build()
        self.dbus.manager_error = "org.freedesktop.DBus.Error.ServiceUnknown"
        service.start()
        self.assertEqual(self.state()["reason"], "no_bluez")
        self.assertIn("ServiceUnknown", self.state()["detail"])

        self.events.clear()
        service = self.build(device(PHONE), phone=(PHONE, "iPhone"))
        self.dbus.objects[ADAPTER_PATH][ADVERTISING_MANAGER] = {}
        service.start()
        FakeBridge.instances[-1].start_error = None
        service.stop_bridge()
        bridge_error = RuntimeError("selected adapter has no BlueZ LE advertising manager")
        with patch.object(FakeBridge, "start", side_effect=bridge_error):
            service.begin()
        self.assertEqual(self.state()["reason"], "no_le")

    def test_turning_bluetooth_on_is_only_done_on_request(self):
        self.build(powered=False).start()
        self.assertEqual(self.dbus.calls, [])       # starting never touches the adapter's power
        self.command("power_on")
        self.assertEqual(self.dbus.calls, [("Set", ADAPTER_PATH, ADAPTER, "Powered", True)])
        self.assertEqual(self.glib.delays(), [ancs_service.POWER_ON_SETTLE_SECONDS])
        self.glib.fire(next(iter(self.glib.timers)))
        self.assertEqual(self.state()["state"], "unpaired")   # re-checked, now powered

    def test_a_missing_phone_in_bluez_needs_a_new_pairing(self):
        service = self.build(phone=(PHONE, "iPhone"))
        with patch.object(FakeBridge, "start",
                          side_effect=RuntimeError("selected iPhone is not uniquely paired")):
            service.start()
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("needs_repair", "not_paired"))
        self.assertTrue(FakeBridge.instances[0].stopped)


class BridgeSupervisionTests(ServiceTestCase):
    def setUp(self):
        super().setUp()
        self.build(device(PHONE), phone=(PHONE, "iPhone")).start()
        self.bridge = FakeBridge.instances[0]

    def test_a_failed_bridge_is_restarted_after_a_pause(self):
        self.bridge.on_status("listening", "")
        self.bridge.loop.quit()
        self.assertTrue(self.bridge.stopped)
        self.assertEqual((self.state()["state"], self.state()["detail"]), ("connecting", "restarting"))
        self.assertEqual(self.glib.delays(), [BRIDGE_RESTART_SECONDS])
        self.glib.fire(next(iter(self.glib.timers)))
        self.assertEqual(len(FakeBridge.instances), 2)
        self.assertTrue(FakeBridge.instances[1].started)

    def test_a_phone_without_notifications_is_flagged_after_a_while(self):
        self.bridge.on_status("waiting", "")
        self.bridge.on_status("no_ancs", "")
        self.assertEqual(self.state()["state"], "connecting")
        self.assertEqual(self.glib.delays(), [REPAIR_AFTER_SECONDS])
        self.glib.fire(next(iter(self.glib.timers)))
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("needs_repair", "no_ancs"))

    def test_recovering_in_time_cancels_the_warning(self):
        self.bridge.on_status("no_gatt", "")
        self.bridge.on_status("listening", "")
        self.assertEqual(self.glib.timers, {})
        self.assertEqual(self.state()["state"], "connected")

    def test_a_refused_subscription_is_retried_quietly(self):
        self.bridge.on_status("subscribe_failed", "org.bluez.Error.NotPermitted")
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("needs_repair", "not_allowed"))
        self.bridge.loop.quit()
        self.assertEqual(self.glib.delays(), [PERMISSION_RETRY_SECONDS])
        shown = len(self.events)
        self.glib.fire(next(iter(self.glib.timers)))
        retry = FakeBridge.instances[1]
        retry.on_status("connecting", "")
        retry.on_status("waiting", "")
        self.assertEqual(len(self.events), shown)            # the message stays while retrying
        retry.on_status("subscribe_failed", "again")
        self.assertEqual(self.state()["state"], "needs_repair")
        retry.on_status("listening", "")                     # the phone finally allowed it
        self.assertEqual(self.state()["state"], "connected")
        retry.on_status("disconnected", "")
        self.assertEqual(self.state()["state"], "connecting")


class PairingTests(ServiceTestCase):
    def pair_flow(self):
        """Starts pairing on a fresh install and returns the FakePair."""
        self.command("pair")
        return FakePair.instances[-1]

    def complete(self, pair, result="ancs", address=PHONE, alias="Kitchen iPhone"):
        pair.hooks.opened(300)
        pair.hooks.confirm(alias, "628640", 40)
        self.command("confirm")
        pair.hooks.paired(address, alias)
        pair.result, pair.address = result, address
        pair.loop.quit()

    def test_the_happy_path_ends_with_a_remembered_phone_and_a_bridge(self):
        self.build().start()
        pair = self.pair_flow()
        self.assertTrue(pair.started)
        self.assertEqual(pair.adapter_path, ADAPTER_PATH)
        self.assertEqual(self.names(), ["unpaired"])          # nothing shown until the window opens

        pair.hooks.opened(300)
        self.assertEqual((self.state()["state"], self.state()["seconds"], self.state()["name"]),
                         ("pair_open", "300", "Frame"))
        pair.hooks.confirm("Kitchen iPhone", "628640", 40)
        self.assertEqual((self.state()["state"], self.state()["code"], self.state()["phone"],
                          self.state()["seconds"]), ("pair_confirm", "628640", "Kitchen iPhone", "40"))
        self.command("confirm")
        self.assertEqual(pair.answers, [True])
        pair.hooks.paired(PHONE, "Kitchen iPhone")
        self.assertEqual((self.state()["state"], self.state()["address"]), ("pair_verify", PHONE))

        pair.result, pair.address = "ancs", PHONE
        pair.loop.quit()
        done = self.state()
        self.assertEqual((done["state"], done["phone"], done["address"], done["ancs"]),
                         ("pair_done", "Kitchen iPhone", PHONE, "yes"))
        self.assertTrue(pair.stopped)
        self.assertEqual(self.config.load(), {"address": PHONE, "name": "Kitchen iPhone"})
        bridge = FakeBridge.instances[-1]
        self.assertEqual((bridge.address, bridge.solicit), (PHONE, True))

        # The result stays on screen: what the bridge does meanwhile is held back until dismissed.
        shown = len(self.events)
        bridge.on_status("waiting", "")
        bridge.on_status("listening", "")
        self.assertEqual(len(self.events), shown)
        self.command("dismiss")
        self.assertEqual(self.state()["state"], "connected")
        self.command("dismiss")                               # nothing left to dismiss
        self.assertEqual(self.events[-1]["state"], "connected")

    def test_a_phone_that_exposes_no_ancs_yet_is_still_remembered(self):
        self.build().start()
        pair = self.pair_flow()
        self.complete(pair, result="gatt")
        self.assertEqual((self.state()["state"], self.state()["ancs"]), ("pair_done", "no"))
        self.assertEqual(self.config.load()["address"], PHONE)

    def test_a_classic_only_pairing_is_removed_and_explained(self):
        self.build(device(PHONE, "Kitchen iPhone", icon="phone")).start()
        self.command("pair")
        self.command("pair_anyway")
        pair = FakePair.instances[-1]
        self.complete(pair, result="none")
        self.assertEqual((self.state()["state"], self.state()["reason"], self.state()["phone"]),
                         ("pair_failed", "classic_only", "Kitchen iPhone"))
        self.assertIn(("RemoveDevice", device_path(PHONE)), self.dbus.calls)
        self.assertIsNone(self.config.load())
        self.assertEqual(FakeBridge.instances, [])

    def test_timeouts_and_errors_are_reported_and_keep_the_old_phone(self):
        self.build(device(OTHER), phone=(OTHER, "Old iPhone")).start()
        self.command("pair")
        self.command("pair_anyway")
        pair = FakePair.instances[-1]
        pair.failure = ("timeout", "")
        pair.loop.quit()
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("pair_failed", "timeout"))
        self.assertEqual(self.config.load()["address"], OTHER)   # the remembered phone is untouched
        self.assertEqual(len(FakeBridge.instances), 2)           # and listening resumes behind it
        self.assertFalse(FakeBridge.instances[-1].stopped)

    def test_start_failures_are_reported(self):
        self.build().start()
        with patch.object(FakePair, "start", side_effect=RuntimeError("no agent manager")):
            self.command("pair")
        self.assertEqual((self.state()["state"], self.state()["reason"], self.state()["detail"]),
                         ("pair_failed", "error", "no agent manager"))
        self.assertTrue(FakePair.instances[-1].stopped)
        self.command("dismiss")
        self.assertEqual(self.state()["state"], "unpaired")
        self.command("pair")                                     # and pairing can simply be retried
        self.assertEqual(len(FakePair.instances), 2)

    def test_rejecting_or_missing_the_code_reopens_the_window(self):
        self.build().start()
        pair = self.pair_flow()
        pair.hooks.opened(300)
        pair.hooks.confirm("iPhone", "111111", 40)
        self.command("reject")
        self.assertEqual(pair.answers, [False])
        self.assertEqual((self.state()["state"], self.state()["detail"], self.state()["seconds"]),
                         ("pair_open", "rejected", "210"))
        pair.hooks.confirm("iPhone", "222222", 40)
        pair.hooks.prompt_expired()
        self.assertEqual((self.state()["state"], self.state()["detail"]), ("pair_open", "expired"))
        pair.answer_result = False                                # nothing pending any more
        shown = len(self.events)
        self.command("reject")
        self.assertEqual(len(self.events), shown)

    def test_cancel_returns_to_where_things_were(self):
        self.build().start()
        pair = self.pair_flow()
        pair.hooks.opened(300)
        self.command("cancel")
        self.assertTrue(pair.stopped)
        self.assertEqual(self.state()["state"], "unpaired")
        self.assertIsNone(self.service.pair)
        self.command("cancel")                                    # harmless when nothing is open

        # With a remembered phone the listening resumes, without flashing a stale "connected".
        self.events.clear()
        FakeBridge.instances.clear()
        self.build(device(PHONE), phone=(PHONE, "iPhone")).start()
        FakeBridge.instances[0].on_status("listening", "")
        self.command("pair")
        self.command("pair_anyway")
        self.assertTrue(FakeBridge.instances[0].stopped)
        self.command("cancel")
        self.assertEqual(self.state()["state"], "connecting")
        self.assertNotIn("connected", self.names()[-2:])
        self.assertEqual(len(FakeBridge.instances), 2)

    def test_pairing_cannot_be_started_twice(self):
        self.build().start()
        self.pair_flow()
        self.command("pair")
        self.assertEqual(len(FakePair.instances), 1)

    def test_pairing_without_bluetooth_asks_the_adapter_again_first(self):
        self.build(powered=False).start()
        self.command("pair")
        self.assertEqual(FakePair.instances, [])
        self.assertEqual(self.state()["reason"], "powered_off")
        self.dbus.objects[ADAPTER_PATH][ADAPTER]["Powered"] = True    # the user turned it on
        self.command("pair")
        self.assertEqual(len(FakePair.instances), 1)


class ConflictTests(ServiceTestCase):
    def test_only_phones_and_the_remembered_phone_can_be_in_the_way(self):
        objects = dict([device(PHONE, "Pixel", icon="phone"),
                        device(OTHER, "Headphones", icon="audio-headset"),
                        device("AA:BB:CC:DD:EE:03", "Mouse", icon="input-mouse"),
                        device("AA:BB:CC:DD:EE:04", "Unpaired phone", paired=False, icon="phone"),
                        device("AA:BB:CC:DD:EE:05", "Old iPhone")])
        found = find_phone_devices(objects)
        self.assertEqual([entry["name"] for entry in found], ["Pixel"])
        found = find_phone_devices(objects, "aa:bb:cc:dd:ee:05")    # remembered, whatever its class
        self.assertEqual([entry["name"] for entry in found], ["Old iPhone", "Pixel"])
        self.assertEqual(found[0]["path"], device_path("AA:BB:CC:DD:EE:05"))

    def test_the_user_chooses_what_to_remove_and_nothing_else_can_be(self):
        self.build(device(PHONE, "Pixel", icon="phone"),
                   device(OTHER, "Headphones", icon="audio-headset")).start()
        self.command("pair")
        conflict = self.state()
        self.assertEqual((conflict["state"], conflict["count"], conflict["name1"], conflict["address1"]),
                         ("pair_conflict", "1", "Pixel", PHONE))
        self.assertNotIn("name2", conflict)
        self.assertEqual(FakePair.instances, [])

        self.command("remove_conflict", address=OTHER)            # not offered: refused
        self.command("remove_conflict", address="nonsense")
        self.command("remove_conflict")
        self.assertEqual(self.dbus.calls, [])
        self.assertEqual(self.state()["state"], "pair_conflict")

        self.command("remove_conflict", address=PHONE.lower())
        self.assertEqual(self.dbus.calls, [("RemoveDevice", device_path(PHONE))])
        self.assertEqual(len(FakePair.instances), 1)              # nothing left in the way: opens
        self.assertIn(device_path(OTHER), self.dbus.objects)      # the headphones were never touched

    def test_the_user_can_carry_on_without_removing_anything(self):
        self.build(device(PHONE, "Pixel", icon="phone")).start()
        self.command("pair")
        self.command("pair_anyway")
        self.assertEqual(self.dbus.calls, [])
        self.assertEqual(len(FakePair.instances), 1)
        self.command("pair_anyway")                               # only meaningful at that prompt
        self.assertEqual(len(FakePair.instances), 1)

    def test_at_most_three_are_offered_and_the_rest_cannot_be_removed(self):
        addresses = [f"AA:BB:CC:DD:EE:1{number}" for number in range(5)]
        self.build(*[device(address, f"Phone {index}", icon="phone")
                     for index, address in enumerate(addresses)]).start()
        self.command("pair")
        conflict = self.state()
        self.assertEqual(conflict["count"], "5")
        self.assertIn("name3", conflict)
        self.assertNotIn("name4", conflict)
        self.command("remove_conflict", address=addresses[4])
        self.assertEqual(self.dbus.calls, [])
        self.command("remove_conflict", address=addresses[0])
        self.assertEqual(self.state()["count"], "4")              # asks again about the rest

    def test_a_remembered_phone_is_offered_first_and_a_failed_removal_is_not_fatal(self):
        self.build(device(OTHER, "Pixel", icon="phone"), device(PHONE, "Old iPhone"),
                   phone=(PHONE, "Old iPhone")).start()
        self.command("pair")
        self.assertEqual(self.state()["name1"], "Old iPhone")
        self.dbus.remove_error = "org.bluez.Error.Failed"
        with redirect_stderr(StringIO()):
            self.command("remove_conflict", address=PHONE)
        self.assertEqual(self.state()["state"], "pair_conflict")  # still waiting for the user

    def test_cancelling_at_the_conflict_prompt_closes_it(self):
        self.build(device(PHONE, "Pixel", icon="phone")).start()
        self.command("pair")
        self.command("cancel")
        self.assertEqual(self.state()["state"], "unpaired")
        self.command("remove_conflict", address=PHONE)            # too late to remove anything
        self.assertEqual(self.dbus.calls, [])


class ForgetAndCommandTests(ServiceTestCase):
    def test_forgetting_removes_the_phone_everywhere(self):
        self.build(device(PHONE, "iPhone"), phone=(PHONE, "iPhone")).start()
        FakeBridge.instances[0].on_status("listening", "")
        self.command("forget")
        self.assertTrue(FakeBridge.instances[0].stopped)
        self.assertEqual(self.dbus.calls, [("RemoveDevice", device_path(PHONE))])
        self.assertIsNone(self.config.load())
        self.assertFalse(os.path.exists(self.config.path))
        self.assertEqual(self.state()["state"], "unpaired")
        self.command("forget")                                    # nothing left: still fine
        self.assertEqual(self.state()["state"], "unpaired")

    def test_forgetting_works_without_bluetooth_too(self):
        self.build(phone=(PHONE, "iPhone"), powered=False).start()
        self.command("forget")
        self.assertIsNone(self.config.load())
        self.assertEqual(self.dbus.calls, [])

    def test_retry_starts_over_and_cancel_ignores_a_paused_refusal(self):
        self.build(powered=False).start()
        self.dbus.objects[ADAPTER_PATH][ADAPTER]["Powered"] = True
        self.command("retry")
        self.assertEqual(self.state()["state"], "unpaired")

    def test_bad_input_never_breaks_the_service(self):
        self.build().start()
        shown = len(self.events)
        with redirect_stderr(StringIO()) as log:
            for line in (b"", b"   ", b"not json", b"[1, 2]", b'"pair"', b'{"command":"explode"}',
                         b'{"nothing":"here"}', b"\xff\xfe\x00", b'{"command": null}'):
                self.service.handle_line(line)
        self.assertEqual(len(self.events), shown)
        self.assertIn("not JSON", log.getvalue())
        self.assertIn("unknown command", log.getvalue())
        for name in COMMANDS:                                     # every command copes with no context
            if name not in ("pair", "forget", "power_on", "retry", "quit"):
                self.command(name)
        self.service.handle_line('{"command":"status","extra":5}')  # str lines and numbers are fine
        self.assertEqual(self.events[-1]["state"], "unpaired")

    def test_quit_and_a_closed_stdin_stop_the_loop(self):
        self.build().start()
        self.command("quit")
        self.service.loop.quit.assert_called_once()
        self.service.on_stdin_closed()
        self.assertEqual(self.service.loop.quit.call_count, 2)

    def test_stopping_releases_everything(self):
        self.build(device(PHONE, "iPhone"), phone=(PHONE, "iPhone")).start()
        self.bridge_timer = self.glib.timeout_add_seconds(5, lambda: False)
        self.command("pair")
        self.command("pair_anyway")
        pair = FakePair.instances[-1]
        self.service.stop()
        self.assertTrue(pair.stopped)
        self.assertTrue(FakeBridge.instances[0].stopped)

    def test_events_are_flat_maps_of_strings(self):
        self.build(device(PHONE, "Pixel", icon="phone")).start()
        self.command("pair")
        self.command("pair_anyway")
        pair = FakePair.instances[-1]
        pair.hooks.opened(300)
        pair.hooks.confirm("iPhone", None, 40)                   # no code: a plain "allow?" request
        self.assertEqual(self.state()["code"], "")
        pair.hooks.paired(PHONE, "iPhone")
        pair.result, pair.address = "ancs", PHONE
        pair.loop.quit()
        self.assertGreater(len(self.events), 5)
        for event in self.events:
            self.assertTrue(all(isinstance(key, str) and isinstance(value, str)
                                for key, value in event.items()), event)
            json.loads(json.dumps(event))


class ConfigAndReaderTests(unittest.TestCase):
    def test_the_remembered_phone_survives_restarts_and_bad_files(self):
        with tempfile.TemporaryDirectory() as directory:
            config = PhoneConfig(os.path.join(directory, "nested", "frame-notify"))
            self.assertIsNone(config.load())
            config.save("aa:bb:cc:dd:ee:01", "Kitchen iPhone")
            self.assertEqual(config.load(), {"address": "AA:BB:CC:DD:EE:01", "name": "Kitchen iPhone"})
            if os.name == "posix":
                self.assertEqual(os.stat(config.path).st_mode & 0o777, 0o600)
                self.assertEqual(os.stat(config.directory).st_mode & 0o777, 0o700)
            config.save("AA:BB:CC:DD:EE:02", "iPhone")
            self.assertEqual(config.load()["address"], "AA:BB:CC:DD:EE:02")
            for text in ("", "{", "[]", '{"address": 5}', '{"address": "not-an-address"}', "null"):
                with open(config.path, "w", encoding="utf-8") as stream:
                    stream.write(text)
                self.assertIsNone(config.load(), text)
            with open(config.path, "w", encoding="utf-8") as stream:
                stream.write('{"address": "AA:BB:CC:DD:EE:03"}')
            self.assertEqual(config.load()["name"], "iPhone")     # a missing name has a default
            config.clear()
            config.clear()
            self.assertIsNone(config.load())

    def test_the_state_directory_matches_the_overlays(self):
        self.assertEqual(state_directory({"XDG_STATE_HOME": "/x/state", "HOME": "/h"}),
                         os.path.join("/x/state", "frame-notify"))
        self.assertEqual(state_directory({"HOME": "/home/user"}),
                         os.path.join("/home/user", ".local", "state", "frame-notify"))
        with self.assertRaises(RuntimeError):
            state_directory({})

    def test_commands_are_reassembled_from_arbitrary_chunks(self):
        service = MagicMock()
        reader = CommandReader(service)
        self.assertTrue(reader.feed(b'{"command":"sta'))
        service.handle_line.assert_not_called()
        self.assertTrue(reader.feed(b'tus"}\n{"command":"can'))
        self.assertTrue(reader.feed(b'cel"}\n\n'))
        self.assertEqual([call.args[0] for call in service.handle_line.call_args_list],
                         [b'{"command":"status"}', b'{"command":"cancel"}', b""])
        self.assertFalse(reader.feed(b""))                        # end of input: the parent is gone
        service.on_stdin_closed.assert_called_once()

    def test_a_runaway_sender_cannot_grow_the_buffer(self):
        reader = CommandReader(MagicMock())
        reader.feed(b"x" * (ancs_service.MAXIMUM_LINE_BYTES + 1))
        self.assertEqual(reader.buffer, b"")
        reader.on_readable(-1, 0)                                 # a read error counts as closed input

    def test_the_service_reports_fatal_problems_to_its_parent(self):
        class BrokenDBus(FakeDBus):
            def SystemBus(self):
                raise FakeDBus.DBusException("no system bus")

        output = StringIO()
        with patch.object(sys, "stdout", sys.stdout), patch.dict(
                os.environ, {"XDG_RUNTIME_DIR": "/run/user/1", "HOME": "/home/x"}):
            self.assertEqual(run_service(BrokenDBus({}), MagicMock(), output=output), 1)
        self.assertEqual(json.loads(output.getvalue()), {"event": "fatal", "message": "no system bus"})

        output = StringIO()
        environment = {key: value for key, value in os.environ.items() if key != "XDG_RUNTIME_DIR"}
        with patch.object(sys, "stdout", sys.stdout), patch.dict(os.environ, environment, clear=True):
            self.assertEqual(run_service(BrokenDBus({}), MagicMock(), output=output), 1)
        self.assertIn("XDG_RUNTIME_DIR", json.loads(output.getvalue())["message"])


if __name__ == "__main__":
    unittest.main()
