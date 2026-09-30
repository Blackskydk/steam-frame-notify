#!/usr/bin/env python3

import argparse
import datetime
import json
import os
import socket
import sys
import uuid


DEMO_NOTIFICATIONS = (
    ("Messages", "Jane Doe", "Can you buy milk on your way home?"),
    ("Home", "Front Door", "The front door was unlocked"),
    ("Discord", "#general", "Are you joining us later?"),
    ("Mail", "Frame Notify", "Your test notification panel is ready"),
    ("Calendar", "Dentist", "Appointment tomorrow at 09:30"),
    ("Phone", "Missed Call", "You missed a call"),
    ("Photos", "Shared Album", "Three new photos were added"),
    ("Reminders", "Shopping", "Remember coffee and batteries"),
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Send notifications to Frame Notify")
    parser.add_argument("app", nargs="?", help="source application, for example Messages")
    parser.add_argument("title", nargs="?", help="notification title or sender")
    parser.add_argument("message", nargs="?", help="notification body")
    parser.add_argument("--id", dest="notification_id", help="stable ID for deduplication")
    parser.add_argument(
        "--demo",
        nargs="?",
        type=int,
        const=len(DEMO_NOTIFICATIONS),
        metavar="COUNT",
        help="send a set of demo notifications (default: 8)",
    )
    arguments = parser.parse_args()

    if arguments.demo is None and not all((arguments.app, arguments.title, arguments.message)):
        parser.error("app, title, and message are required unless --demo is used")
    if arguments.demo is not None and not 1 <= arguments.demo <= 50:
        parser.error("--demo COUNT must be between 1 and 50")

    runtime_directory = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime_directory:
        print("error: XDG_RUNTIME_DIR is not set", file=sys.stderr)
        return 1

    now = datetime.datetime.now().astimezone()
    if arguments.demo is not None:
        items = [DEMO_NOTIFICATIONS[index % len(DEMO_NOTIFICATIONS)]
                 for index in range(arguments.demo)]
    else:
        items = [(arguments.app, arguments.title, arguments.message)]

    batch_id = uuid.uuid4()
    payloads = []
    for index, (app, title, message) in enumerate(items):
        payloads.append({
            "type": "notification",
            "id": (arguments.notification_id if len(items) == 1 and arguments.notification_id
                   else f"test-{batch_id}-{index + 1}"),
            "app": app,
            "title": title,
            "message": message,
            "timestamp": (now - datetime.timedelta(minutes=index * 3)).isoformat(timespec="seconds"),
        })

    socket_path = os.path.join(runtime_directory, "frame-notify.sock")
    encoded = "".join(
        json.dumps(payload, ensure_ascii=False) + "\n" for payload in payloads
    ).encode("utf-8")

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(socket_path)
            client.sendall(encoded)
    except OSError as error:
        print(f"error: could not send to {socket_path}: {error}", file=sys.stderr)
        return 1

    if len(payloads) == 1:
        print(f"sent {payloads[0]['id']} to {socket_path}")
    else:
        print(f"sent {len(payloads)} demo notifications to {socket_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
