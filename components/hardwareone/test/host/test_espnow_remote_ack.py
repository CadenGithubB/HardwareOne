#!/usr/bin/env python3
"""The acknowledgement of `espnowremote` always ends with its request id.

cmd_espnow_remote answers a command it handed to the radio with
"Sent '<command>' to <target>. Output is ASYNC - read it with:  espnowmessages
json 0 <MAC>  (match reqId <n>)". The id is how a caller finds the peer's
answer, and it is what the web page and the OLED remote form read to tell an
accepted command from a refusal. The reply buffer holds 255 bytes; a command
of more than about 125 bytes used to push the id off its end.

The production formatter espnowFormatRemoteAck (System_ESPNow.cpp) is compiled
for the host and printed for a short command, a 200-byte command, the largest
id and a small buffer:

- the short form is byte-identical to the reply before the formatter existed;
- a long command shortens the echo and keeps "(match reqId <n>)" at the end;
- nothing is written past the buffer.

--component points the suite at another component directory. A tree without
the formatter runs the same cases on its inline snprintf (the pre-fix tree
fails the long-command case).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys
import tempfile

from test_web_batch_handlers import require
import espnow_remote_replies as replies

HERE = Path(__file__).resolve().parent
MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])
MAC_TEXT = "02:00:00:00:00:01"
TAIL = ". Output is ASYNC - read it with:  espnowmessages json 0 " + MAC_TEXT + "  (match reqId %d)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []

    long_command = "espnowsend lab " + "0123456789" * 19          # 205 bytes
    cases = [("reboot", "kitchen", MAC, 77),
             (long_command, MAC_TEXT, MAC, 123456),
             (long_command, "kitchen", MAC, 4294967295),
             ("", "kitchen", MAC, 1)]
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-remote-ack-") as directory:
        work = Path(directory)
        acks = replies.render_text_acks(args.component, cases, args.cxx, work, flags=flags)
        small = replies.render_text_acks(args.component, cases[:1], args.cxx, work, size=40, flags=flags)

    if acks[0] != "Sent 'reboot' to kitchen" + TAIL % 77:
        failures.append(f"the short acknowledgement changed: {acks[0]!r}")
    for ack, (command, target, _, request) in zip(acks, cases):
        tail = f"' to {target}" + TAIL % request
        if len(ack.encode()) > 255:
            failures.append(f"acknowledgement of {len(ack)} bytes does not fit the 255-byte reply")
        if not ack.endswith(f"(match reqId {request})"):
            failures.append(f"a {len(command)}-byte command loses the request id: ...{ack[-48:]!r}")
            continue
        if not (ack.startswith("Sent '") and ack.endswith(tail)):
            failures.append(f"acknowledgement lost its fixed text: {ack!r}")
            continue
        echo = ack[len("Sent '"):len(ack) - len(tail)]
        if not command.startswith(echo):
            failures.append(f"the echo is not the start of the command: {echo!r}")
        if len(command) <= 100 and echo != command:
            failures.append(f"a short command was shortened: {echo!r}")
    if len(small[0]) > 39:
        failures.append(f"a 40-byte buffer received {len(small[0])} bytes")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"espnow_remote_ack: {len(failures)} failure(s)")
        return 1
    print("PASS espnow_remote_ack: the request id always ends the acknowledgement")
    return 0


if __name__ == "__main__":
    sys.exit(main())
