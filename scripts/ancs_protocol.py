"""Small, dependency-free decoder for Apple's ANCS wire format."""

from dataclasses import dataclass
import struct

SERVICE_UUID = "7905f431-b5ce-4e99-a40f-4b1e122d00d0"
NOTIFICATION_SOURCE_UUID = "9fbf120d-6301-42d9-8c58-25e699a21dbd"
CONTROL_POINT_UUID = "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9"
DATA_SOURCE_UUID = "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb"

COMMAND_GET_NOTIFICATION_ATTRIBUTES = 0
COMMAND_GET_APP_ATTRIBUTES = 1
APP_ATTRIBUTE_DISPLAY_NAME = 0

ADDED = 0
MODIFIED = 1
REMOVED = 2
FLAG_SILENT = 1
FLAG_PRE_EXISTING = 4

# Attribute IDs, in exactly the same order as the Control Point request.
REQUESTED_ATTRIBUTES = (0, 1, 2, 3, 5)


@dataclass(frozen=True)
class Event:
    kind: int
    flags: int
    category: int
    count: int
    uid: int


def parse_event(value):
    if len(value) != 8:
        raise ValueError("ANCS Notification Source event must be exactly 8 bytes")
    return Event(*struct.unpack("<BBBBI", bytes(value)))


def attribute_request(uid):
    if not 0 <= uid <= 0xFFFFFFFF:
        raise ValueError("invalid ANCS notification UID")
    # App identifier, title (128 B), subtitle (128 B), message (512 B), date.
    return (struct.pack("<BI", 0, uid) + bytes((0, 1)) +
            struct.pack("<H", 128) + bytes((2,)) + struct.pack("<H", 128) +
            bytes((3,)) + struct.pack("<H", 512) + bytes((5,)))


def app_attributes_request(app_identifier):
    """Get App Attributes request for an app's display name (a NUL-terminated identifier)."""
    encoded = app_identifier.encode("utf-8")
    if not encoded or len(encoded) > 255 or b"\x00" in encoded:
        raise ValueError("invalid ANCS app identifier")
    return (bytes((COMMAND_GET_APP_ATTRIBUTES,)) + encoded + b"\x00" +
            bytes((APP_ATTRIBUTE_DISPLAY_NAME,)))


class AppAttributeResponse:
    """Reassembles one fragmented Get App Attributes response for a single app."""

    def __init__(self, app_identifier):
        self.app_identifier = app_identifier
        self.expected = app_identifier.encode("utf-8")
        self.buffer = bytearray()

    def feed(self, fragment):
        """Returns the display name (possibly empty) once complete, otherwise None."""
        self.buffer.extend(fragment)
        if len(self.buffer) > 4096:
            raise ValueError("ANCS app attribute response exceeded 4096 bytes")
        if not self.buffer:
            return None
        if self.buffer[0] != COMMAND_GET_APP_ATTRIBUTES:
            raise ValueError("unexpected ANCS app attribute response")
        seen = bytes(self.buffer[1:1 + len(self.expected)])
        if seen != self.expected[:len(seen)]:
            raise ValueError("ANCS app attribute response is for a different app")
        tuple_start = 2 + len(self.expected)          # command byte, identifier, terminator
        if len(self.buffer) < tuple_start + 3:
            return None
        if self.buffer[tuple_start - 1] != 0:
            raise ValueError("ANCS app identifier is not terminated as expected")
        attribute_id, length = struct.unpack_from("<BH", self.buffer, tuple_start)
        if attribute_id != APP_ATTRIBUTE_DISPLAY_NAME or length > 2048:
            raise ValueError("invalid ANCS app attribute tuple")
        end = tuple_start + 3 + length
        if len(self.buffer) < end:
            return None
        if len(self.buffer) != end:
            raise ValueError("trailing bytes in ANCS app attribute response")
        return bytes(self.buffer[tuple_start + 3:end]).decode("utf-8", errors="replace")


class AttributeResponse:
    """Reassembles one fragmented Get Notification Attributes response."""

    def __init__(self, uid):
        self.uid = uid
        self.buffer = bytearray()

    def feed(self, fragment):
        self.buffer.extend(fragment)
        if len(self.buffer) > 4096:
            raise ValueError("ANCS attribute response exceeded 4096 bytes")
        if len(self.buffer) < 5:
            return None
        command, uid = struct.unpack_from("<BI", self.buffer)
        if command != 0 or uid != self.uid:
            raise ValueError("unexpected ANCS attribute response")
        position = 5
        values = {}
        for expected_id in REQUESTED_ATTRIBUTES:
            if len(self.buffer) - position < 3:
                return None
            attribute_id, length = struct.unpack_from("<BH", self.buffer, position)
            if attribute_id != expected_id or length > 2048:
                raise ValueError("invalid ANCS attribute tuple")
            position += 3
            if len(self.buffer) - position < length:
                return None
            values[attribute_id] = bytes(self.buffer[position:position + length]).decode(
                "utf-8", errors="replace")
            position += length
        if position != len(self.buffer):
            raise ValueError("trailing bytes in ANCS attribute response")
        return values
