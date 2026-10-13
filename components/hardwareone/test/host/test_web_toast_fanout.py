#!/usr/bin/env python3
"""Execute the production web toast path: payload rendering and session fan-out.

The harness compiles, from the shipping sources on every run, the toast block
of systemEventsNotifyTick with notifFormatEvent and the payload renderer
(System_Notifications.cpp), the session-table fence, slot invalidation,
broadcast helpers and fenced queue accessors (WebServer_Server.cpp) and the
event queue primitives (WebServer_Events.cpp), with the real vendored
ArduinoJson. FreeRTOS's recursive mutex is a std::recursive_mutex double.

It checks that a LOGIN_FAIL toast whose attempted name holds a quote, a
backslash and a newline is queued as JSON that parses back to the same text
on one SSE data line (and that an all-control-character name still yields a
complete document inside one queue slot), and, with a producer thread against
an httpd-role thread that logs a slot out and in between an admin and a guest
while draining the queues, that the viewer predicate runs without the session
fence, no admin-only toast reaches the guest, and the stable session's queue
accounts for every event exactly once. Not covered: the socket writes in
handleEvents, the browser, BLE/G2 producers' own state, or ESP-IDF scheduling.
Run with --tsan for ThreadSanitizer (--sanitize is ASan + UBSan).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

FENCE_START = "std::atomic<TransportSessionEpoch> sWebSessionEpoch[MAX_SESSIONS];"
FENCE_CLASS = "class WebSessionFenceGuard"
BROADCAST_MARKERS = ("void broadcastEventToAllSessions(",
                     "void broadcastEventToSessionsIf(", "uint32_t sseEventDropsTotal(")
DEQUEUE_MARKERS = ("bool webSessionDequeueSseEvent(", "void webSessionSettleSseQueues(")
RENDERER = "static size_t notifRenderToastJson("


def production(server: str, events: str, header: str, notifications: str,
               notif_header: str) -> tuple[dict, bool]:
    fenced = all(server.count(marker) == 1 for marker in DEQUEUE_MARKERS)
    handler = extract_block(events, "esp_err_t handleEvents(")
    if fenced:
        require("webSessionDequeueSseEvent(sessIdx, sid," in handler,
                "handleEvents must drain through the fenced accessor")
    else:
        require("sseDequeueEvent(gSessions[sessIdx]" in handler,
                "fallback dequeue no longer mirrors handleEvents")

    define = re.search(r"^#define MAX_SESSIONS\s+\d+\s*$", header, re.M)
    require(define is not None, "MAX_SESSIONS definition")
    types = define.group(0) + "\n" + extract_block(header, "struct SessionEntry") + ";"

    primitives = "\n\n".join([
        extract_block(events, "void sseEnqueueEvent("),
        extract_block(events, "bool sseDequeueEvent("),
    ])

    require(server.count(FENCE_START) == 1 and server.count(FENCE_CLASS) == 1, "one session fence")
    start = server.index(FENCE_START)
    fence = server[start:server.index(FENCE_CLASS, start)]
    fence += extract_block(server, FENCE_CLASS) + ";\n\n"
    fence += extract_block(server, "void webInvalidateSessionSlotLocked(") + "\n\n"
    fence += extract_block(server, "void webInvalidateSessionSlot(")

    markers = BROADCAST_MARKERS + (DEQUEUE_MARKERS if fenced else ())
    for marker in markers:
        require(server.count(marker) == 1, f"one definition: {marker}")
    broadcast = "\n\n".join(extract_block(server, marker) for marker in markers)

    require(notifications.count("if (gSettings.notifToasts)") == 1, "one toast block")
    notif = [extract_block(notifications, "void notifFormatEvent("),
             extract_block(notifications, "static const char* levelName(")]
    if notifications.count(RENDERER) == 1:
        notif.append(extract_block(notifications, RENDERER))
    toast = extract_block(notifications, "if (gSettings.notifToasts)")

    return ({"// INSERT_SESSION_TYPES": types,
             "// INSERT_QUEUE_PRIMITIVES": primitives,
             "// INSERT_FENCE": fence,
             "// INSERT_BROADCAST": broadcast,
             "// INSERT_NOTIF_TYPES": extract_block(notif_header, "struct NotifRule") + ";",
             "// INSERT_NOTIF_PRODUCTION": "\n\n".join(notif),
             "// INSERT_TOAST_BLOCK": toast}, fenced)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--tsan", action="store_true", help="build with ThreadSanitizer instead")
    parser.add_argument("--case", help="run one harness case by name (toast_payload, fanout_race)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    replacements, fenced = production(
        (COMPONENT / "WebServer_Server.cpp").read_text(),
        (COMPONENT / "WebServer_Events.cpp").read_text(),
        (COMPONENT / "WebServer_Server.h").read_text(),
        (COMPONENT / "System_Notifications.cpp").read_text(),
        (COMPONENT / "System_Notifications.h").read_text())
    harness = (HERE / "web_toast_fanout_harness.cpp").read_text()
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-web-toast-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text((HERE / "web_batch_stubs" / "Arduino.h").read_text())
        unit = directory / "web_toast_fanout.cpp"
        unit.write_text(harness)
        binary = directory / "web_toast_fanout"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   f"-DHAVE_FENCED_DEQUEUE={int(fenced)}",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(binary), "-pthread"]
        if args.tsan:
            command[1:1] = ["-fsanitize=thread", "-fno-omit-frame-pointer", "-g"]
        elif args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=120)


if __name__ == "__main__":
    main()
