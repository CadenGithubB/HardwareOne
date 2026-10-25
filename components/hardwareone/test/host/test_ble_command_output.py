#!/usr/bin/env python3
"""Run a Bluetooth command's own broadcastOutput lines to its session, as shipped.

`help` renders its menu with broadcastOutput and returns "OK". Serial prints
those lines and the web terminal folds them into its reply; a Bluetooth session
received them only through the console mirror, off until `outble 1`, so `help`
and `help all` answered an app with a bare "OK", in server mode and on the phone
pipe's link alike.

System_Debug.cpp's broadcastOutputCore and its overloads, System_CLI.cpp's help
menu (renderHelpMain) and Bluetooth.cpp's command tail, reply callback, output
batch, senders, central TX gate class, mirror collector and slot and session
helpers run here, over doubles of cmd_exec, the debug queue, the Secure
Channel, the gate, IDF GATTS and transport sessions. Cases, in server mode and
on the phone link (the G2 client host): the session that sent `help` or
`help all` gets the menu in whole-line batches of at most 512 bytes, then "OK",
the same messages in both modes, as reliable gated sends on the phone link;
another session gets nothing; a JSON result stays one byte-exact message; an
over-long line keeps its start, marked [CUT]; with `outble` on a mirror
recipient is not sent the lines twice; a plaintext server session gets
notifications of at most 512 bytes; a session that ends during the command gets
nothing more; lines from outside a Bluetooth command and the send traces never
reach a session; a line printed under the lifecycle lock (Bluetooth.cpp's own
BLE_DEBUGF, spliced in) joins the batch when it fits and never sends it under
the lock. On the phone link's console row (phone link v2) `help` and `help all`
are reliable TEXT chains with the same messages, and a line printed while the
task holds the pipe owner or console chain mutex never flushes the batch.
On the console row a batch whose send fails (a stream that takes nothing)
drops the command's later batches, and the result is still a reliable write
(the driver's stall latch makes it one try: phone_console). A server-mode
session, plaintext or Secure Channel, and design A's slot whose GATT post is
refused for one batch's six tries lose that batch only: the later batches and
"OK" still arrive. bleLifecycleHeldByCurrentTask() reads this task's hold,
false with no lock or no task yet.
Source checks keep the hook on Bluetooth-routed
lines, the debug drain's mirror condition, and the reply context on the command.
--component points the suite at another copy of the sources: one without the
batch still compiles (the hook is a no-op) and fails the delivery cases.
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
import ble_pipe_console_host as pipe_host

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# The output batch: absent from a source without the fix.
BATCH = (
    "static bool bleMirrorReachesSession(",
    "static void bleCommandOutputFlush(BleCommandReplyContext* reply) ",
    "void bleCommandOutputLine(",
)


def code_only(text: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def found(pattern: str, text: str, what: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    require(match is not None, what)
    return match[0]


def generated(component: Path) -> tuple[str, bool]:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    debug = (component / "System_Debug.cpp").read_text()
    debug_h = (component / "System_Debug.h").read_text()
    cli = (component / "System_CLI.cpp").read_text()
    main_cpp = (component / "HardwareOne.cpp").read_text()
    utils = (component / "System_Utils.cpp").read_text()
    user_h = (component / "System_User.h").read_text()
    types_h = (component / "System_CommandTypes.h").read_text()
    identity_h = (component / "System_AuthIdentity.h").read_text()
    registry_h = (component / "System_CommandRegistry.h").read_text()

    types = [
        found(r"^using TransportSessionEpoch = uint32_t;", user_h, "System_User.h has the epoch type"),
        extract_block(user_h, "enum CommandSource") + ";",
        extract_block(user_h, "struct AuthContext") + ";",
        "struct httpd_req;\ntypedef struct httpd_req httpd_req_t;",
        extract_block(types_h, "enum CommandOrigin") + ";",
        extract_block(types_h, "enum CommandContextBehavior") + ";",
        extract_block(types_h, "struct CommandContext") + ";",
        found(r"^struct Command \{[^}]*\};", types_h, "System_CommandTypes.h has Command"),
        found(r"^typedef void \(\*ExecAsyncCallback\)\(.*\);", types_h, "the async callback type"),
        "\n".join(re.findall(r"^#define MSG_ROUTE_\w+\s+0x[0-9A-Fa-f]+.*$", debug_h, re.MULTILINE)),
        found(r"^#define BROADCAST_PRINTF\(fmt, \.\.\.\) \\\n(?:.*\\\n)*.*$", debug_h,
              "System_Debug.h defines BROADCAST_PRINTF"),
        extract_block(identity_h, "struct CaptureBufState") + ";",
        "struct CommandEntry;",
        "\n".join(re.findall(r"^#define CMD_MODULE_\w+\s+0x[0-9A-Fa-f]+.*$", registry_h, re.MULTILINE)),
        extract_block(registry_h, "struct CommandModule") + ";",
        (component / "BLE_ConsoleAttachCore.h").read_text().replace("#pragma once\n", "", 1),
        (component / "BLE_Types.h").read_text(),
        extract_block(header, "struct BLEConnection") + ";",
        extract_block(header, "struct BLESystemState") + ";",
        found(r"^static std::atomic<hw1console::Mode> sBleConsoleMode\{[^}]*\};", owner,
              "Bluetooth.cpp keeps the console mode"),
        found(r"^static std::atomic<int> sBleConsoleSendsInFlight\{[^}]*\};", owner,
              "Bluetooth.cpp counts the client host's posts in flight"),
        # The phone link's console row (phone link v2): its types and holder bits.
        pipe_host.pipe_types(component),
        pipe_host.link_record(component),
        pipe_host.holder_state(owner),
    ]

    definitions = [
        extract_block(main_cpp, "uint32_t getCurrentCommandOutputMask()"),
        extract_block(utils, "bool currentCommandValidateOnly()"),
        extract_block(debug, "static void broadcastOutputCore("),
        extract_block(debug, "void broadcastOutput(const String& s)"),
        extract_block(debug, "void broadcastOutput(const char* s)"),
        extract_block(debug, "void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route)"),
        extract_block(owner, "static inline bool bleConsoleClientHost() "),
        extract_block(owner, "class BleLifecycleGuard") + ";",
        found(r"^std::atomic<TransportSessionEpoch> sBleSessionEpoch\[BLE_MAX_CONNECTIONS\];", owner,
              "Bluetooth.cpp keeps a session epoch per slot"),
        # The console driver's doubles; the console row's functions and the
        # output router are declared here and defined last.
        "#define PIPE_HOST_LIFECYCLE_HELD() (gLifecycle.depth != 0)",
        pipe_host.PIPE_DOUBLES,
        pipe_host.declarations(owner, pipe_host.ROW_MARKERS + pipe_host.ROUTER_MARKERS),
    ]
    definitions += [extract_block(owner, marker) for marker in (
        "int findConnectionSlotByConnIdLocked(",
        "bool bleConnectionEpochMatchesLocked(")]
    definitions += [pipe_host.slot_state(owner), pipe_host.row_helpers(owner)]
    definitions += [extract_block(owner, marker) for marker in (
        "void bleInvalidateSessionSlotLocked(",
        "TransportSessionEpoch blePublishSessionSlotLocked(",
        "TransportSessionEpoch bleSessionEpochForConnection(",
        "bool bleSessionEpochMatches(",
        "static BLESystemState* bleNewState(",
        "const char* bleDeviceTypeToString(",
        "void bleClearConnectionByConnId(",
        "static bool bleConsoleMakeSlotLocked(",
        "static void bleConsoleJoinLocked(",
        "static void bleMarkActivity(",
        "static bool bleBindSession(",
        "static TransportSessionEpoch bleLogout(",
        "static bool bleIsAuthed(",
        "bool isBLEConnected(",
        "static bool bleSecureChannelOnly(")]
    # The pipe side's probe of this task's lifecycle hold (phone link v2).
    definitions.append(extract_block(owner, "bool bleLifecycleHeldByCurrentTask("))
    definitions.append(extract_block(owner, "class BleConsoleTxGate") + ";")
    definitions += [extract_block(owner, marker) for marker in (
        "bool bleRawNotifyToSession(uint16_t connId,\n",
        "bool sendBLEResponseToSession(",
        "static void bleSendAuthRequired(")]
    definitions.append(extract_block(owner, "struct BleSessionFence") + ";")
    definitions.append(extract_block(owner, "static int bleCollectMirrorSessions("))
    definitions.append(extract_block(owner, "struct BleCommandReplyContext") + ";")
    has_batch = all(marker in owner for marker in BATCH)
    if has_batch:
        definitions.append("static void bleCommandOutputFlush(BleCommandReplyContext* reply);")
    definitions += [extract_block(owner, marker) for marker in (
        "static bool bleOutputHasPrivateSTT(",
        "static void bleTxTrace(",
        "static void bleCommandResultCallback(")]
    if has_batch:
        definitions += [extract_block(owner, marker) for marker in BATCH]
    else:
        definitions.append("void bleCommandOutputLine(const char*, size_t) {}")
    definitions += [
        extract_block(owner, "static bool bleRequiresLogin("),
        extract_block(owner, "static bool bleIsFileBrowserCommand("),
        extract_block(owner, "struct BleLoginAsyncJob") + ";",
        extract_block(owner, "static void processBleCommandLine("),
        extract_block(cli, "const char* renderHelpMain(bool showAll)"),
        extract_block(owner, "bool bleHasAuthenticatedSession("),
    ]
    definitions += pipe_host.row_definitions(owner) + pipe_host.router_definitions(owner)
    ble_debugf = found(r"^#define BLE_DEBUGF\(flag, fmt, \.\.\.\) \\\n(?:.*\\\n)*.*$", owner,
                       "Bluetooth.cpp defines BLE_DEBUGF")
    harness = (HERE / "ble_command_output_harness.cpp").read_text()
    return (harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_BLE_DEBUGF", ble_debugf).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions)), has_batch)


def check_sources(component: Path) -> None:
    owner = (component / "Bluetooth.cpp").read_text()
    debug = (component / "System_Debug.cpp").read_text()
    # The hook takes only lines routed to Bluetooth, after they are queued, so
    # the send traces (routed off Bluetooth) cannot feed their own batch.
    core = code_only(extract_block(debug, "static void broadcastOutputCore("))
    if "bleCommandOutputLine(" in core:
        require("if (route & MSG_ROUTE_BLE) bleCommandOutputLine(text, len);" in core and
                core.index("enqueueChunk(") < core.index("bleCommandOutputLine("),
                "broadcastOutputCore hands only Bluetooth-routed lines to the batch, after queueing them")
    # The debug drain's mirror condition, which bleMirrorReachesSession mirrors.
    gate = ("(msg->routing & MSG_ROUTE_BLE) && (gOutputFlags & MSG_ROUTE_BLE) &&\n"
            "          isBLEConnected() && bleMirrorRecipientCount() > 0")
    require(gate in debug, "the debug drain mirrors Bluetooth lines on outble, a connection "
                           "and a recipient (bleMirrorReachesSession checks the same)")
    reaches = code_only(extract_block(owner, "static bool bleMirrorReachesSession(")) \
        if "static bool bleMirrorReachesSession(" in owner else ""
    if reaches:
        require("gOutputFlags & MSG_ROUTE_BLE" in reaches and "isBLEConnected()" in reaches and
                "bleCollectMirrorSessions(" in reaches,
                "bleMirrorReachesSession reads the drain's condition and sendBLEResponse's recipients")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose sources are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    check_sources(args.component)
    source_text, has_batch = generated(args.component)
    if not has_batch:
        print("no command output batch in this source: the hook is a no-op here", flush=True)
    with tempfile.TemporaryDirectory(prefix="hw1-ble-command-output-") as directory:
        work = Path(directory)
        shutil.copyfile(HERE / "web_batch_stubs" / "Arduino.h", work / "Arduino.h")
        source = work / "command_output.cpp"
        binary = work / "command_output"
        source.write_text(source_text)
        built = subprocess.run([args.cxx, *flags, "-I", str(work), "-DENABLE_BLE_PHONE_PIPE=1",
                                str(source), "-o", str(binary)],
                               text=True, capture_output=True, timeout=180)
        if built.returncode != 0:
            print(built.stdout + built.stderr)
            raise SystemExit("FAIL: the BLE command output harness does not compile")
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=120)
        print((ran.stdout + ran.stderr).strip())
        require(ran.returncode == 0, "the BLE command output harness failed")
    print("BLE command output: `help` and `help all` reach the session that sent them, then OK, "
          "in server mode and on the phone link")


if __name__ == "__main__":
    main()
