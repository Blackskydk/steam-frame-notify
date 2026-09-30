"""Offline ANCS protocol tests; no Bluetooth or D-Bus required."""

import json
import os
import pathlib
from collections import deque
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, call, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

from ancs_protocol import (AppAttributeResponse, AttributeResponse, SERVICE_UUID,
                           app_attributes_request, attribute_request, parse_event)
from ancs_bridge import (ADAPTER, AGENT_MANAGER, AGENT_PATH, ADVERTISING_MANAGER,
                         CHARACTERISTIC, DEVICE, OBJECT_MANAGER, PROPERTIES, REJECTED, SERVICE,
                         AdapterProblem, Bridge, PairSession, diagnose_device, format_passkey,
                         link_status, make_agent, parse_answer, pick_adapter)
from ancs_protocol import (CONTROL_POINT_UUID, DATA_SOURCE_UUID, NOTIFICATION_SOURCE_UUID)


class ProtocolTests(unittest.TestCase):
    def test_notification_source_event_is_little_endian(self):
        event = parse_event(bytes((0, 4, 6, 2, 0x78, 0x56, 0x34, 0x12)))
        self.assertEqual((event.kind, event.flags, event.category, event.count, event.uid),
                         (0, 4, 6, 2, 0x12345678))
        with self.assertRaises(ValueError):
            parse_event(b"short")

    def test_attribute_request(self):
        request = attribute_request(0x12345678)
        self.assertEqual(request[:5], bytes((0, 0x78, 0x56, 0x34, 0x12)))
        self.assertEqual(request[5:], bytes((0, 1, 128, 0, 2, 128, 0,
                                             3, 0, 2, 5)))

    def test_fragmented_response(self):
        uid = 42
        response = bytearray(struct.pack("<BI", 0, uid))
        for attribute_id, value in ((0, b"com.apple.MobileSMS"), (1, b"Hello"),
                                    (2, b""), (3, "Caf\u00e9".encode()),
                                    (5, b"20260929T170200")):
            response.extend(struct.pack("<BH", attribute_id, len(value)))
            response.extend(value)
        parser = AttributeResponse(uid)
        for byte in response[:-1]:
            self.assertIsNone(parser.feed(bytes((byte,))))
        attributes = parser.feed(response[-1:])
        self.assertEqual(attributes[1], "Hello")
        self.assertEqual(attributes[2], "")
        self.assertEqual(attributes[3], "Caf\u00e9")

    def test_wrong_uid_is_rejected(self):
        with self.assertRaises(ValueError):
            AttributeResponse(1).feed(struct.pack("<BI", 0, 2))

    def test_bridge_skips_preexisting_and_removes_pending_uid(self):
        bridge = Bridge.__new__(Bridge)
        bridge.queue = deque()
        bridge.queued = set()
        bridge.removed = set()
        bridge.active = None
        bridge.next_request = lambda: None
        bridge.on_event(struct.pack("<BBBBI", 0, 4, 0, 0, 10))
        self.assertEqual(len(bridge.queue), 0)
        bridge.on_event(struct.pack("<BBBBI", 0, 1, 0, 0, 11))
        self.assertEqual(list(bridge.queue), [(11, True)])
        bridge.on_event(struct.pack("<BBBBI", 2, 0, 0, 0, 11))
        self.assertEqual(len(bridge.queue), 0)
        self.assertEqual(len(bridge.queued), 0)

    def test_read_only_diagnosis_reports_gatt_services(self):
        device = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
        objects = {
            device: {"org.bluez.Device1": {"Address": "AA:BB:CC:DD:EE:FF",
                                           "Paired": True, "Connected": True,
                                           "ServicesResolved": True,
                                           "Adapter": "/org/bluez/hci0"}},
            device + "/service001": {"org.bluez.GattService1": {
                "UUID": "1801", "Device": device}},
            "/org/bluez/hci0": {"org.bluez.LEAdvertisingManager1": {
                "ActiveInstances": 0, "SupportedInstances": 2}},
        }
        output = StringIO()
        with redirect_stdout(output):
            diagnose_device(objects, "AA:BB:CC:DD:EE:FF")
        self.assertIn("ServicesResolved: True", output.getvalue())
        self.assertIn("ANCS: not exposed by BlueZ", output.getvalue())
        self.assertIn("Advertising instances: 0 active / 2 supported", output.getvalue())


class FakeGLib:
    PRIORITY_DEFAULT = 0
    IO_IN = 1
    IO_HUP = 2

    def __init__(self):
        self.next_id = 1
        self.sources = set()

    def io_add_watch(self, _fd, _priority, _condition, _callback):
        return self.add()

    def timeout_add_seconds(self, _seconds, _callback):
        return self.add()

    def add(self):
        source_id, self.next_id = self.next_id, self.next_id + 1
        self.sources.add(source_id)
        return source_id

    def source_remove(self, source_id):
        self.sources.remove(source_id)   # a double removal is a GLib critical


class FakeStdin:
    def __init__(self, *lines):
        self.lines = list(lines)

    def fileno(self):
        return 0

    def readline(self):
        return self.lines.pop(0)


class PairingTests(unittest.TestCase):
    ADAPTER = "/org/bluez/hci0"
    PHONE = ADAPTER + "/dev_AA_BB_CC_DD_EE_FF"

    def session(self):
        session = PairSession.__new__(PairSession)
        session.glib = FakeGLib()
        session.pending = session.watch_id = session.prompt_timer = None
        session.describe = lambda device: "iPhone (AA:BB:CC:DD:EE:FF)"
        return session

    def ask(self, session, *answers):
        events = []
        with patch.object(sys, "stdin", FakeStdin(*answers)), redirect_stdout(StringIO()):
            session.ask(self.PHONE, "code?", lambda: events.append("accept"),
                        lambda: events.append("reject"))
        return events

    def test_answers_and_passkeys(self):
        self.assertEqual(format_passkey(42), "000042")
        self.assertEqual([parse_answer(text) for text in ("y\n", " YES ", "n", "\n", "maybe")],
                         [True, True, False, False, None])

    def test_yes_accepts_and_leaves_no_source_behind(self):
        session = self.session()
        events = []
        with patch.object(sys, "stdin", FakeStdin("y\n")), redirect_stdout(StringIO()):
            session.ask(self.PHONE, "code?", lambda: events.append("accept"),
                        lambda: events.append("reject"))
            self.assertFalse(session.on_answer(0, 0))
        self.assertEqual(events, ["accept"])
        self.assertIsNone(session.pending)
        self.assertEqual(session.glib.sources, {1})   # only the watch GLib removes itself

    def test_unclear_answer_asks_again_and_no_rejects(self):
        session = self.session()
        events = []
        with patch.object(sys, "stdin", FakeStdin("what\n", "n\n")), \
                redirect_stdout(StringIO()):
            session.ask(self.PHONE, "code?", lambda: events.append("accept"),
                        lambda: events.append("reject"))
            self.assertTrue(session.on_answer(0, 0))
            self.assertEqual(events, [])
            self.assertFalse(session.on_answer(0, 0))
        self.assertEqual(events, ["reject"])

    def test_closed_terminal_rejects(self):
        session = self.session()
        events = []
        with patch.object(sys, "stdin", FakeStdin("")), redirect_stdout(StringIO()):
            session.ask(self.PHONE, "code?", lambda: events.append("accept"),
                        lambda: events.append("reject"))
            self.assertFalse(session.on_answer(0, 0))
        self.assertEqual(events, ["reject"])

    def test_second_request_is_rejected_while_one_is_pending(self):
        session = self.session()
        first = self.ask(session, "y\n")
        second = self.ask(session, "y\n")
        self.assertEqual((first, second), ([], ["reject"]))
        self.assertIsNotNone(session.pending)

    def test_timeout_rejects_and_removes_the_stdin_watch(self):
        session = self.session()
        events = []
        with patch.object(sys, "stdin", FakeStdin()), redirect_stdout(StringIO()), \
                redirect_stderr(StringIO()):
            session.ask(self.PHONE, "code?", lambda: events.append("accept"),
                        lambda: events.append("reject"))
            self.assertFalse(session.on_prompt_timeout())
        self.assertEqual(events, ["reject"])
        self.assertEqual(session.glib.sources, {2})   # the expired timer, which GLib removes

    def test_link_status_distinguishes_classic_gatt_and_ancs(self):
        objects = {self.PHONE: {"org.bluez.Device1": {"Address": "AA:BB:CC:DD:EE:FF"}}}
        self.assertEqual(link_status(objects, self.PHONE), "none")
        objects[self.PHONE + "/service0001"] = {"org.bluez.GattService1": {
            "UUID": "1801", "Device": self.PHONE}}
        self.assertEqual(link_status(objects, self.PHONE), "gatt")
        objects[self.PHONE + "/service0002"] = {"org.bluez.GattService1": {
            "UUID": SERVICE_UUID.upper(), "Device": self.PHONE}}
        self.assertEqual(link_status(objects, self.PHONE), "ancs")
        self.assertEqual(link_status(objects, self.ADAPTER + "/dev_11_22_33_44_55_66"), "none")

    @staticmethod
    def fake_dbus(interfaces):
        class Error(Exception):
            def __init__(self, message="", name=None):
                super().__init__(message)
                self.name = name

        class Object:
            def __init__(self, bus, path):
                pass

        return SimpleNamespace(
            DBusException=Error, exceptions=SimpleNamespace(DBusException=Error),
            service=SimpleNamespace(Object=Object,
                                    method=lambda *args, **kwargs: (lambda function: function)),
            Boolean=bool, UInt32=int, Dictionary=dict, String=str,
            Array=lambda value, signature=None: list(value),
            Interface=lambda _object, name: interfaces.setdefault(name, MagicMock()))

    def open_session(self, interfaces):
        dbus = self.fake_dbus(interfaces)
        session = PairSession(MagicMock(), FakeGLib(), dbus, self.ADAPTER)
        interfaces[OBJECT_MANAGER].GetManagedObjects.return_value = {}
        interfaces[PROPERTIES].Get.side_effect = lambda _i, name: {
            "Pairable": False, "PairableTimeout": 30}[name]
        return dbus, session

    def test_agent_asks_the_terminal_and_refuses_everything_else(self):
        dbus = self.fake_dbus({})
        session = MagicMock()
        agent = make_agent(MagicMock(), dbus, session)
        errors = []
        agent.RequestConfirmation("/dev", 123456, lambda: None, errors.append)
        question, reject = session.ask.call_args.args[1], session.ask.call_args.args[3]
        self.assertIn("123456", question)
        reject()
        self.assertEqual(errors[0].name, REJECTED)
        for refused in (lambda: agent.RequestPinCode("/dev"),
                        lambda: agent.RequestPasskey("/dev"),
                        lambda: agent.AuthorizeService("/dev", "0000110b")):
            with self.assertRaises(dbus.DBusException) as caught, \
                    redirect_stdout(StringIO()):
                refused()
            self.assertEqual(caught.exception.name, REJECTED)

    def test_pair_window_restores_adapter_and_agent(self):
        interfaces = {}
        _, session = self.open_session(interfaces)
        with redirect_stdout(StringIO()):
            session.start()
            session.on_advertisement_ready()
            session.stop()
        interfaces[AGENT_MANAGER].RegisterAgent.assert_called_once_with(AGENT_PATH,
                                                                        "DisplayYesNo")
        interfaces[AGENT_MANAGER].UnregisterAgent.assert_called_once_with(AGENT_PATH)
        interfaces[ADVERTISING_MANAGER].UnregisterAdvertisement.assert_called_once()
        self.assertEqual(interfaces[PROPERTIES].Set.call_args_list, [
            call(ADAPTER, "Pairable", True), call(ADAPTER, "PairableTimeout", 0),
            call(ADAPTER, "Pairable", False), call(ADAPTER, "PairableTimeout", 30)])
        self.assertEqual(session.glib.sources, set())

    def test_failed_start_still_restores_what_it_changed(self):
        interfaces = {}
        dbus, session = self.open_session(interfaces)

        def refuse_timeout(_interface, name, value):
            if name == "PairableTimeout":
                raise dbus.DBusException("refused")

        interfaces[PROPERTIES].Set.side_effect = refuse_timeout
        with redirect_stdout(StringIO()), self.assertRaises(dbus.DBusException):
            session.start()
        with redirect_stdout(StringIO()):
            session.stop()
        interfaces[AGENT_MANAGER].UnregisterAgent.assert_called_once_with(AGENT_PATH)
        self.assertEqual(interfaces[PROPERTIES].Set.call_args_list, [
            call(ADAPTER, "Pairable", True), call(ADAPTER, "PairableTimeout", 0),
            call(ADAPTER, "Pairable", False)])

    def test_adapter_selection(self):
        def adapter(name, powered=True, advertising=True):
            interfaces = {"org.bluez.Adapter1": {"Powered": powered}}
            if advertising:
                interfaces["org.bluez.LEAdvertisingManager1"] = {}
            return "/org/bluez/" + name, interfaces

        self.assertEqual(pick_adapter(dict([adapter("hci0")])), self.ADAPTER)
        with self.assertRaises(RuntimeError):
            pick_adapter(dict([adapter("hci0"), adapter("hci1")]))
        self.assertEqual(pick_adapter(dict([adapter("hci0"), adapter("hci1")]), "hci1"),
                         "/org/bluez/hci1")
        with self.assertRaises(RuntimeError):
            pick_adapter(dict([adapter("hci0", powered=False)]))
        with self.assertRaises(RuntimeError):
            pick_adapter(dict([adapter("hci0", advertising=False)]))


def notification_response(uid, app, title="Jane", message="Hello"):
    """A complete Get Notification Attributes response, as one Data Source value."""
    response = bytearray(struct.pack("<BI", 0, uid))
    for attribute_id, value in ((0, app), (1, title), (2, ""), (3, message), (5, "20260929T170200")):
        encoded = value.encode()
        response.extend(struct.pack("<BH", attribute_id, len(encoded)))
        response.extend(encoded)
    return bytes(response)


def app_response(app, name):
    encoded = name.encode()
    return (bytes((1,)) + app.encode() + b"\x00" + struct.pack("<BH", 0, len(encoded)) + encoded)


class RecordingGLib:
    """Keeps timer callbacks so a test can fire them by hand."""

    def __init__(self):
        self.next_id = 1
        self.timers = {}

    def timeout_add_seconds(self, _seconds, callback):
        source_id, self.next_id = self.next_id, self.next_id + 1
        self.timers[source_id] = callback
        return source_id

    def source_remove(self, source_id):
        del self.timers[source_id]   # removing an unknown or fired source is a GLib critical

    def fire(self, source_id):
        callback = self.timers[source_id]
        if not callback():
            self.timers.pop(source_id, None)


class AppNameTests(unittest.TestCase):
    APP = "com.apple.MobileSMS"

    def bridge(self):
        bridge = Bridge.__new__(Bridge)
        bridge.glib = RecordingGLib()
        bridge.queue = deque()
        bridge.queued = set()
        bridge.removed = set()
        bridge.active = None
        bridge.response = None
        bridge.timeout_id = None
        bridge.hold_timer_id = None
        bridge.pending_attributes = None
        bridge.app_names = {}
        bridge.subscribed = True
        bridge.failed = False
        bridge.loop = None
        bridge.writes = []
        bridge.forwarded = []
        bridge.write_control_point = lambda request: bridge.writes.append(bytes(request))
        bridge.forward = lambda uid, silent, attributes: bridge.forwarded.append(
            (uid, silent, dict(attributes)))
        return bridge

    def begin(self, bridge, uid, silent=False):
        bridge.active = (uid, silent)
        bridge.response = AttributeResponse(uid)
        bridge.timeout_id = bridge.glib.timeout_add_seconds(15, bridge.on_timeout)

    def test_app_attribute_request_is_nul_terminated_and_validated(self):
        self.assertEqual(app_attributes_request(self.APP),
                         b"\x01" + self.APP.encode() + b"\x00\x00")
        for invalid in ("", "a\x00b", "x" * 256):
            with self.assertRaises(ValueError):
                app_attributes_request(invalid)

    def test_app_attribute_response_reassembles_fragments(self):
        payload = app_response(self.APP, "Nachrichten")
        parser = AppAttributeResponse(self.APP)
        for byte in payload[:-1]:
            self.assertIsNone(parser.feed(bytes((byte,))))
        self.assertEqual(parser.feed(payload[-1:]), "Nachrichten")
        self.assertEqual(AppAttributeResponse(self.APP).feed(app_response(self.APP, "")), "")

    def test_app_attribute_response_rejects_anything_unexpected(self):
        good = app_response(self.APP, "Messages")
        for bad in (b"\x00" + good[1:],                          # wrong command
                    good.replace(b"MobileSMS", b"MobileSMT"),    # another app
                    good + b"!",                                 # trailing bytes
                    good[:len(self.APP) + 2] + b"\x01" + good[len(self.APP) + 3:],  # wrong attribute
                    bytes((1,)) + self.APP.encode() + b"X" + good[len(self.APP) + 2:]):  # no terminator
            with self.assertRaises(ValueError):
                AppAttributeResponse(self.APP).feed(bad)

    def test_name_is_requested_once_per_app_and_used_after(self):
        bridge = self.bridge()
        self.begin(bridge, 7)
        bridge.on_data(notification_response(7, self.APP))
        self.assertEqual(bridge.writes, [app_attributes_request(self.APP)])
        self.assertEqual(bridge.forwarded, [])                   # waits for the name
        payload = app_response(self.APP, "Messages")
        bridge.on_data(payload[:5])
        self.assertEqual(bridge.forwarded, [])
        bridge.on_data(payload[5:])
        self.assertEqual(bridge.app_names, {self.APP: "Messages"})
        self.assertEqual([item[0] for item in bridge.forwarded], [7])
        self.assertEqual(bridge.forwarded[0][2][1], "Jane")     # the original attributes survive
        self.assertIsNone(bridge.active)
        self.assertEqual(bridge.glib.timers, {})                 # no timer is left behind

        self.begin(bridge, 8)
        bridge.on_data(notification_response(8, self.APP, "Erik"))
        self.assertEqual(len(bridge.writes), 1)                  # cached: no second request
        self.assertEqual([item[0] for item in bridge.forwarded], [7, 8])
        self.assertEqual(bridge.glib.timers, {})

    def test_notifications_without_an_app_identifier_skip_the_lookup(self):
        bridge = self.bridge()
        self.begin(bridge, 3)
        bridge.on_data(notification_response(3, ""))
        self.assertEqual(bridge.writes, [])
        self.assertEqual([item[0] for item in bridge.forwarded], [3])

    def test_notification_removed_during_the_lookup_is_not_shown(self):
        bridge = self.bridge()
        self.begin(bridge, 4)
        bridge.on_data(notification_response(4, self.APP))
        bridge.removed.add(4)
        bridge.on_data(app_response(self.APP, "Messages"))
        self.assertEqual(bridge.forwarded, [])
        self.assertEqual(bridge.app_names[self.APP], "Messages")  # the name is still learned
        self.assertNotIn(4, bridge.removed)

    def test_timeout_shows_the_notification_and_pauses_before_the_next_request(self):
        bridge = self.bridge()
        self.begin(bridge, 9)
        bridge.on_data(notification_response(9, self.APP))
        with redirect_stderr(StringIO()):
            bridge.glib.fire(bridge.timeout_id)
        self.assertEqual([item[0] for item in bridge.forwarded], [9])
        self.assertEqual(bridge.app_names[self.APP], "")
        self.assertFalse(bridge.failed)                          # a slow name never stops the bridge
        self.assertIsNone(bridge.active)
        self.assertIsNotNone(bridge.hold_timer_id)

        bridge.queue.append((10, False))
        bridge.queued.add(10)
        bridge.on_data(b"late fragment")                         # ignored: nothing is active
        bridge.next_request()
        self.assertEqual(len(bridge.writes), 1)                  # held back during the cool-down
        bridge.glib.fire(bridge.hold_timer_id)
        self.assertEqual(bridge.writes[-1], attribute_request(10))
        self.assertEqual(bridge.active, (10, False))

        # The failed lookup is remembered: the same app is not asked about again.
        bridge.on_data(notification_response(10, self.APP))
        self.assertEqual([item[0] for item in bridge.forwarded], [9, 10])
        self.assertEqual(len(bridge.writes), 2)

    def test_malformed_name_response_is_not_fatal(self):
        bridge = self.bridge()
        self.begin(bridge, 11)
        bridge.on_data(notification_response(11, self.APP))
        with redirect_stderr(StringIO()):
            bridge.on_data(b"\x07 this is not an app response")
        self.assertEqual([item[0] for item in bridge.forwarded], [11])
        self.assertFalse(bridge.failed)
        self.assertIsNotNone(bridge.hold_timer_id)

    def test_malformed_notification_response_is_still_fatal(self):
        bridge = self.bridge()
        bridge.loop = MagicMock()
        self.begin(bridge, 12)
        with redirect_stderr(StringIO()):
            bridge.on_data(struct.pack("<BI", 0, 99) + b"junk")  # response for another UID
        self.assertTrue(bridge.failed)
        bridge.loop.quit.assert_called_once()

    def test_failed_write_of_the_name_request_still_shows_the_notification(self):
        bridge = self.bridge()
        self.begin(bridge, 13)
        bridge.on_data(notification_response(13, self.APP))
        with redirect_stderr(StringIO()):
            bridge.on_write_error("boom")
        self.assertEqual([item[0] for item in bridge.forwarded], [13])
        self.assertEqual(bridge.app_names[self.APP], "")
        self.assertIsNone(bridge.active)

    def test_session_reset_cancels_the_cool_down(self):
        bridge = self.bridge()
        self.begin(bridge, 14)
        bridge.on_data(notification_response(14, self.APP))
        with redirect_stderr(StringIO()):
            bridge.glib.fire(bridge.timeout_id)
        bridge.subscribed = True
        bridge.subscribing = False
        bridge.service_path = bridge.data_path = bridge.source_path = bridge.control_path = "x"
        bridge.reported_unavailable = False
        bridge.session = "old"
        with redirect_stdout(StringIO()):
            bridge.reset_session()
        self.assertIsNone(bridge.hold_timer_id)
        self.assertEqual(bridge.glib.timers, {})
        self.assertEqual(bridge.app_names[self.APP], "")         # names survive a reconnect

    def sent_event(self, bridge, attributes, silent=False):
        sent = []

        class FakeSocket:
            def __init__(self, *_arguments):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_arguments):
                return False

            def settimeout(self, _seconds):
                pass

            def connect(self, _path):
                pass

            def sendall(self, data):
                sent.append(data)

        # A stand-in for the whole module: Windows has no AF_UNIX to look up.
        fake_socket_module = SimpleNamespace(AF_UNIX=1, SOCK_STREAM=1, socket=FakeSocket)
        real_forward = Bridge.forward
        with patch("ancs_bridge.socket", fake_socket_module), redirect_stdout(StringIO()):
            real_forward(bridge, 5, silent, attributes)
        return json.loads(sent[0].decode("utf-8"))

    def test_forwarded_event_carries_the_friendly_name_and_identifier(self):
        bridge = self.bridge()
        bridge.socket_path = "unused"
        bridge.session = "abc"
        bridge.app_names = {self.APP: "Nachrichten"}
        event = self.sent_event(bridge, {0: self.APP, 1: "Jane", 2: "", 3: "Hallo", 5: ""}, silent=True)
        self.assertEqual((event["app"], event["app_id"]), ("Nachrichten", self.APP))
        # Silent notifications are flagged as such, and still get a toast by default.
        self.assertEqual((event["title"], event["message"], event["toast"], event.get("silent")),
                         ("Jane", "Hallo", "true", "true"))
        self.assertNotIn("silent", self.sent_event(bridge, {0: self.APP, 1: "Jane", 3: "Hallo"}, silent=False))
        with patch.dict(os.environ, {"FRAME_NOTIFY_QUIET_NO_TOAST": "1"}):
            quiet = self.sent_event(bridge, {0: self.APP, 1: "Jane", 3: "Hallo"}, silent=True)
            loud = self.sent_event(bridge, {0: self.APP, 1: "Jane", 3: "Hallo"}, silent=False)
        self.assertEqual((quiet["toast"], quiet["silent"], loud["toast"]), ("false", "true", "true"))
        with patch.dict(os.environ, {"FRAME_NOTIFY_QUIET_NO_TOAST": "0"}):
            self.assertEqual(self.sent_event(bridge, {0: self.APP, 1: "Jane", 3: "Hallo"}, silent=True)["toast"],
                             "true")
        self.assertEqual(event["id"], "ancs-abc-5")

        bridge.app_names = {self.APP: ""}                         # lookup failed: fall back to the id
        self.assertEqual(self.sent_event(bridge, {0: self.APP, 1: "A", 2: "", 3: "B", 5: ""})["app"],
                         self.APP)
        bridge.app_names = {}
        event = self.sent_event(bridge, {0: "", 1: "", 2: "", 3: "", 5: ""})
        self.assertEqual((event["app"], event["title"]), ("iPhone", "iPhone"))
        self.assertNotIn("app_id", event)


class HookRecorder:
    """Stands in for the program that drives a PairSession."""

    def __init__(self):
        self.calls = []

    def opened(self, seconds):
        self.calls.append(("opened", seconds))

    def confirm(self, phone, code, seconds):
        self.calls.append(("confirm", phone, code, seconds))

    def prompt_expired(self):
        self.calls.append(("prompt_expired",))

    def paired(self, address, alias):
        self.calls.append(("paired", address, alias))

    def finished(self, result, address):
        self.calls.append(("finished", result, address))

    def failed(self, reason, detail):
        self.calls.append(("failed", reason, detail))


class PairHookTests(unittest.TestCase):
    ADAPTER_PATH = "/org/bluez/hci0"
    DEVICE_PATH = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01"
    ADDRESS = "AA:BB:CC:DD:EE:01"

    def setUp(self):
        self.interfaces = {}
        self.dbus = PairingTests.fake_dbus(self.interfaces)
        self.glib = RecordingGLib()
        self.hooks = HookRecorder()
        self.session = PairSession(MagicMock(), self.glib, self.dbus, self.ADAPTER_PATH,
                                   hooks=self.hooks)
        self.session.loop = MagicMock()
        self.interfaces[PROPERTIES].GetAll.return_value = {"Alias": "Kitchen iPhone",
                                                           "Address": self.ADDRESS}

    def test_the_window_and_the_code_are_handed_to_the_program(self):
        with redirect_stdout(StringIO()):
            self.session.on_advertisement_ready()
        self.assertEqual(self.hooks.calls, [("opened", 300)])
        self.assertTrue(298 <= self.session.seconds_left() <= 300)

        accepted, rejected = [], []
        with redirect_stdout(StringIO()):
            # The fake GLib has no io_add_watch: a driven session must never watch stdin.
            self.session.ask(self.DEVICE_PATH, "question", lambda: accepted.append(1),
                             lambda: rejected.append(1), code="628640")
            self.assertEqual(self.hooks.calls[-1], ("confirm", "Kitchen iPhone", "628640", 40))
            self.session.ask(self.DEVICE_PATH, "second", lambda: accepted.append(2),
                             lambda: rejected.append(2))
            self.assertEqual(rejected, [2])                       # one question at a time
            self.assertTrue(self.session.answer(True))
            self.assertEqual(accepted, [1])
            self.assertFalse(self.session.answer(True))           # nothing is waiting any more
            self.assertEqual(self.glib.timers.keys(), {self.session.window_timer})

    def test_an_unanswered_code_expires_and_the_program_is_told(self):
        rejected = []
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            self.session.ask(self.DEVICE_PATH, "question", lambda: None,
                             lambda: rejected.append(1), code="111111")
            self.glib.fire(self.session.prompt_timer)
        self.assertEqual(rejected, [1])
        self.assertEqual(self.hooks.calls[-1], ("prompt_expired",))
        self.assertIsNone(self.session.pending)

    def test_pairing_and_the_final_link_are_reported(self):
        with redirect_stdout(StringIO()):
            self.session.on_advertisement_ready()
            self.session.on_properties_changed(DEVICE, {"Paired": True}, [],
                                               path=self.DEVICE_PATH)
        self.assertEqual(self.hooks.calls[-1], ("paired", self.ADDRESS, "Kitchen iPhone"))
        self.assertIsNone(self.session.window_timer)

        self.interfaces[OBJECT_MANAGER].GetManagedObjects.return_value = {
            self.DEVICE_PATH + "/service0001": {SERVICE: {"UUID": SERVICE_UUID,
                                                           "Device": self.DEVICE_PATH}}}
        with redirect_stdout(StringIO()):
            self.assertFalse(self.session.check_link())
        self.assertEqual(self.hooks.calls[-1], ("finished", "ancs", self.ADDRESS))
        self.assertEqual(self.session.result, "ancs")
        self.session.loop.quit.assert_called_once()

    def test_every_way_a_session_can_fail_carries_a_reason(self):
        for trigger, expected in (
                (lambda: self.session.on_window_closed(), ("failed", "timeout", "")),
                (lambda: self.session.on_advertisement_error("boom"), ("failed", "advertising", "boom")),
                (lambda: self.session.on_agent_released(), ("failed", "agent", ""))):
            with redirect_stderr(StringIO()):
                trigger()
            self.assertEqual(self.hooks.calls[-1], expected)
            self.assertEqual(self.session.failure, expected[1:])
        self.assertTrue(self.session.failed)

        self.session.device_path = self.DEVICE_PATH
        self.interfaces[OBJECT_MANAGER].GetManagedObjects.side_effect = \
            self.dbus.DBusException("gone")
        with redirect_stderr(StringIO()):
            self.assertFalse(self.session.check_link())
        self.assertEqual(self.hooks.calls[-1][:2], ("failed", "error"))

    def test_stopping_releases_what_a_long_running_program_would_otherwise_leak(self):
        match = self.session.match
        agent = MagicMock(path=AGENT_PATH)
        advertisement = MagicMock(path="/com/frame_notify/ancs_solicitation")
        self.session.agent, self.session.advertisement = agent, advertisement
        self.session.agent_registered = self.session.advertising = True
        with redirect_stdout(StringIO()):
            self.session.stop()
        match.remove.assert_called_once()
        agent.remove_from_connection.assert_called_once()
        advertisement.remove_from_connection.assert_called_once()
        self.assertIsNone(self.session.agent)
        with redirect_stdout(StringIO()):
            self.session.stop()                                   # stopping twice is harmless


class BridgeStatusTests(unittest.TestCase):
    DEVICE_PATH = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01"
    SERVICE_PATH = DEVICE_PATH + "/service0010"

    def setUp(self):
        self.glib = RecordingGLib()
        bus = MagicMock()
        bus.add_signal_receiver.side_effect = lambda *args, **kwargs: MagicMock()  # distinct each
        self.bridge = Bridge(bus, self.glib, MagicMock(), "aa:bb:cc:dd:ee:01", "/x.sock",
                             solicit=True)
        self.bridge.device_path = self.DEVICE_PATH
        self.bridge.device_proxy = MagicMock()
        self.bridge.loop = MagicMock()
        self.statuses = []
        self.bridge.on_status = lambda state, detail="": self.statuses.append((state, detail))

    def objects(self, *, gatt=False, ancs=False, characteristics=False):
        objects = {}
        if gatt:
            objects[self.DEVICE_PATH + "/service0001"] = {SERVICE: {
                "UUID": "1801", "Device": self.DEVICE_PATH}}
        if ancs:
            objects[self.SERVICE_PATH] = {SERVICE: {"UUID": SERVICE_UUID, "Device": self.DEVICE_PATH}}
        if characteristics:
            for index, uuid in enumerate((NOTIFICATION_SOURCE_UUID, DATA_SOURCE_UUID,
                                          CONTROL_POINT_UUID)):
                objects[f"{self.SERVICE_PATH}/char{index}"] = {CHARACTERISTIC: {
                    "UUID": uuid, "Service": self.SERVICE_PATH}}
        self.bridge.manager.GetManagedObjects.return_value = objects

    def test_progress_is_reported_to_a_supervising_program(self):
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            self.bridge.refresh({"Connected": False})
            self.bridge.refresh({"Connected": False})             # already connecting: no repeat
            self.assertEqual(self.statuses, [("connecting", "")])
            self.bridge.on_connected()
            self.assertEqual(self.statuses[-1], ("waiting", ""))

            connected = {"Connected": True, "ServicesResolved": True}
            self.objects()
            self.bridge.refresh(connected)
            self.assertEqual(self.statuses[-1], ("no_gatt", ""))
            self.objects(gatt=True)
            self.bridge.refresh(connected)
            self.assertEqual(self.statuses[-1], ("no_ancs", ""))
            self.objects(gatt=True, ancs=True)
            self.bridge.refresh(connected)
            self.assertEqual(self.statuses[-1], ("no_ancs", "characteristics"))

            self.objects(gatt=True, ancs=True, characteristics=True)
            self.bridge.refresh(connected)
            self.assertTrue(self.bridge.subscribing)              # the subscription was requested
            self.bridge.on_data_ready()
            self.bridge.on_source_ready()
            self.assertEqual(self.statuses[-1], ("listening", ""))
            self.bridge.reset_session()
            self.assertEqual(self.statuses[-1], ("disconnected", ""))

    def test_a_refused_subscription_is_reported_with_its_reason(self):
        with redirect_stderr(StringIO()):
            self.bridge.on_subscribe_error("org.bluez.Error.NotPermitted")
        self.assertEqual(self.statuses[-1], ("subscribe_failed", "org.bluez.Error.NotPermitted"))
        self.assertTrue(self.bridge.failed)
        self.bridge.loop.quit.assert_called_once()

    def test_without_a_supervisor_nothing_changes(self):
        self.bridge.on_status = None
        with redirect_stdout(StringIO()):
            self.bridge.on_source_ready()
        self.assertTrue(self.bridge.subscribed)

    def test_stop_leaves_nothing_running(self):
        bridge = self.bridge
        bridge.timeout_id = self.glib.timeout_add_seconds(15, lambda: False)
        bridge.hold_timer_id = self.glib.timeout_add_seconds(3, lambda: False)
        bridge.periodic_id = self.glib.timeout_add_seconds(10, lambda: True)
        matches = list(bridge.matches)
        self.assertEqual(len(matches), 3)
        bridge.subscribed = True
        bridge.source_path, bridge.data_path = "/source", "/data"
        bridge.queue.append((1, False))
        bridge.advertising = True
        advertisement = MagicMock(path="/com/frame_notify/ancs_solicitation")
        bridge.advertisement = advertisement
        bridge.advertising_manager = MagicMock()

        with redirect_stdout(StringIO()):
            bridge.stop()
        self.assertEqual(self.glib.timers, {})
        for match in matches:
            match.remove.assert_called_once()
        bridge.advertising_manager.UnregisterAdvertisement.assert_called_once_with(
            "/com/frame_notify/ancs_solicitation", timeout=3)
        advertisement.remove_from_connection.assert_called_once()
        self.assertEqual(bridge.dbus.Interface.return_value.StopNotify.call_count, 2)
        self.assertEqual((bridge.stopped, bridge.subscribed, len(bridge.queue)), (True, False, 0))

        # A stopped bridge ignores late callbacks instead of reconnecting in the background.
        self.assertFalse(bridge.periodic_refresh())
        bridge.dbus.Interface.reset_mock()
        bridge.refresh({"Connected": False})
        bridge.on_properties_changed(DEVICE, {}, [], path=self.DEVICE_PATH)
        bridge.on_interfaces_added(self.DEVICE_PATH + "/x", {})
        bridge.discover()
        bridge.on_advertisement_ready()
        bridge.dbus.Interface.assert_not_called()
        self.assertEqual(self.statuses, [])
        with redirect_stdout(StringIO()):
            bridge.stop()                                         # and stopping twice is harmless


class AdapterProblemTests(unittest.TestCase):
    def test_problems_carry_a_reason_the_dashboard_can_act_on(self):
        def adapters(*entries):
            objects = {}
            for name, powered in entries:
                objects["/org/bluez/" + name] = {ADAPTER: {"Powered": powered}, ADVERTISING_MANAGER: {}}
            return objects

        cases = ((adapters(), None, "no_adapter"),
                 (adapters(("hci0", True), ("hci1", True)), None, "ambiguous"),
                 (adapters(("hci0", True)), "hci5", "no_adapter"),
                 (adapters(("hci0", False)), None, "powered_off"))
        for objects, name, reason in cases:
            with self.assertRaises(AdapterProblem) as caught:
                pick_adapter(objects, name)
            self.assertEqual(caught.exception.reason, reason)
            self.assertIsInstance(caught.exception, RuntimeError)   # older callers still work
        self.assertEqual(pick_adapter(adapters(("hci0", True)), None), "/org/bluez/hci0")


if __name__ == "__main__":
    unittest.main()
