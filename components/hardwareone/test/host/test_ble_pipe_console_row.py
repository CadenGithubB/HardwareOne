#!/usr/bin/env python3
"""Run the phone link's console row (phone link v2) as shipped.

The app console is stream slot 2 of the phone pipe. Bluetooth.cpp keeps its
session as a gBLEState row with its own id (0x8000 | generation), bound signed
out when the stream opens and ended when it closes; its output goes to the
console driver (BLE_PhonePipeConsole.h) as records. This suite splices the
session table, the epoch helpers, the console row's lifecycle (holders, bind,
end, identity snapshot, reconcile), the auth rule, revocation, the disconnect
owner functions, the command tail (processBleCommandLine), the record ingress
(blePipeConsoleInbound), the output router (sendBLEResponseToSession,
sendBLEResponse, blePushNotification, bleSendBinaryToSession,
bleSessionConfidential), design A's slot helpers and System_OTA.cpp's
currentSecureBleConnection into one harness, over recording doubles of the
driver, transport sessions (12 slots), the Secure Channel and the executor.

Cases: bind, rebind and end with the snapshot following every login, logout,
revoke and end; a bind that finds no transport session leaves nothing; the
row must log in even with blerequireauth off (whoami, the gate, never
AuthBypass) while a server row keeps AuthBypass; design A's slot and the row on
the same link, signed in as different users, never authorise each other (the
command stamp, whoami, the OTA superadmin check, confidentiality, revocation,
bledisconnect); revocation ends the row and asks for CLOSE 5, bledisconnect for
CLOSE 2, a blerequireauth toggle for none; the record ingress (COMMAND as an
encrypted line, BINARY to OTA after the activity stamp, a stale epoch, another
row's id, an unknown type); the output mapping (TEXT reliable only from
cmd_exec, MIRROR in records of at most 512 bytes, CARD as the bare JSON object,
BLOB for confidential sessions only, fenced on the command's epoch); the main
loop's reconcile; start/stop cycles return every transport session; and a
lock-order oracle: no call into the pipe while the lifecycle lock is held.
gBLEState's readers (isBLEConnected, isBleServerInitialized, the state word,
the stream settings, bleUpdateStreams) read it only under the lifecycle lock:
each runs with `phone stop`'s release armed to preempt its first unlocked read
(--sanitize: a heap-use-after-free otherwise); the main loop keeps the console
row's session tick in phone-client mode and samples data streams only for the
server role, each sample sent with the lock released.
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


def found(pattern: str, text: str, what: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    require(match is not None, what)
    return match[0]


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    ota = (component / "System_OTA.cpp").read_text()
    user_h = (COMPONENT / "System_User.h").read_text()
    types_h = (COMPONENT / "System_CommandTypes.h").read_text()
    identity_h = (COMPONENT / "System_AuthIdentity.h").read_text()
    debug_h = (COMPONENT / "System_Debug.h").read_text()
    utils_h = (COMPONENT / "System_Utils.h").read_text()

    types = [
        extract_block(user_h, "enum CommandSource") + ";",
        extract_block(user_h, "struct AuthContext") + ";",
        found(r"^using TransportSessionEpoch = uint32_t;", user_h, "System_User.h has the epoch type"),
        "struct httpd_req;\ntypedef struct httpd_req httpd_req_t;",
        extract_block(types_h, "enum CommandOrigin") + ";",
        extract_block(types_h, "enum CommandContextBehavior") + ";",
        extract_block(identity_h, "struct CaptureBufState") + ";",
        extract_block(types_h, "struct CommandContext") + ";",
        found(r"^struct Command \{[^}]*\};", types_h, "System_CommandTypes.h has Command"),
        found(r"^typedef void \(\*ExecAsyncCallback\)\(.*\);", types_h, "the async callback type"),
        "\n".join(re.findall(r"^#define MSG_ROUTE_\w+\s+0x[0-9A-Fa-f]+.*$", debug_h, re.MULTILINE)),
        (COMPONENT / "BLE_Types.h").read_text(),
        (component / "BLE_ConsoleAttachCore.h").read_text().replace("#pragma once\n", "", 1),
        extract_block(header, "enum class BleDisconnectResult") + ";",
        extract_block(header, "struct BLEConnection") + ";",
        extract_block(header, "struct BLESystemState") + ";",
        pipe_host.pipe_types(component),
        pipe_host.link_record(COMPONENT),
        pipe_host.holder_state(owner),
    ]
    for pattern in (r"^static std::atomic<hw1console::Mode> sBleConsoleMode\{[^}]*\};",
                    r"^static std::atomic<bool> sBleConsoleAdmissionOpen\{[^}]*\};",
                    r"^static hw1console::Attach sBleConsoleAttach;",
                    r"^static hw1console::Slot sBleConsoleLogged;",
                    r"^static std::atomic<int> sBleConsoleSlotConnId\{[^}]*\};"):
        types.append(found(pattern, owner, f"Bluetooth.cpp declares {pattern}"))
    types.append(extract_block(owner, "static inline bool bleConsoleClientHost() "))

    definitions = [
        extract_block(owner, "class BleLifecycleGuard") + ";",
        found(r"^std::atomic<TransportSessionEpoch> sBleSessionEpoch\[BLE_MAX_CONNECTIONS\];", owner,
              "Bluetooth.cpp keeps a session epoch per slot"),
        "#define PIPE_HOST_LIFECYCLE_HELD() (gLifecycle.depth != 0)",
        pipe_host.PIPE_DOUBLES,
        pipe_host.declarations(owner, pipe_host.ROW_MARKERS + pipe_host.ROUTER_MARKERS),
        extract_block(utils_h, "inline void macToDisplay("),
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
        "bool bleSessionEpochIsLive(",
        "static void bleMarkActivity(",
        "void bleClearConnectionByConnId(",
        "static bool bleIsAuthed(",
        "bool bleGetAuthenticatedUser(",
        "static String blePeerIpTag(",
        "static bool bleRequiresLogin(",
        "static bool bleFillCommandAuth(",
        "static TransportSessionEpoch bleLogout(",
        "bool bleHasAuthenticatedSession(",
        "bool bleGetAuthenticatedSessionInfo(",
        "static void bleRevokeSlotLocked(",
        "int bleRevokeUserSessions(",
        "int bleRevokeAllSessions(",
        "void bleAuthPolicyChanged(",
        "static bool bleBindSession(",
        "static void bleSendAuthRequired(",
        "const char* bleDeviceTypeToString(",
        "static hw1console::PipeLink bleConsolePipeLink(",
        "static void bleConsoleEndSlotLocked(",
        "static bool bleConsoleMakeSlotLocked(",
        "static void bleConsoleJoinLocked(",
        "static void bleConsoleStoreLocked(",
        "static void bleConsoleApplyLocked(",
        "static void bleConsoleReconcileLocked(")]
    definitions += [extract_block(owner, "struct BleLoginAsyncJob") + ";",
                    extract_block(owner, "struct BleCommandReplyContext") + ";"]
    definitions += [extract_block(owner, marker) for marker in (
        "static bool bleIsFileBrowserCommand(",
        "static void processBleCommandLine(",
        "static bool bleSecureChannelOnly(")]
    definitions.append(found(r"^constexpr uint8_t kBlePipeRecordCommand = [^;]+;", owner,
                             "Bluetooth.cpp names the console's inbound records"))
    definitions.append(extract_block(owner, "void blePipeConsoleInbound("))
    definitions.append(extract_block(owner, "static BLESystemState* bleNewState("))
    definitions += pipe_host.row_definitions(owner)
    # gBLEState's readers (K1): the server check, the state word, the stream
    # settings and the main loop's stream pass.
    definitions += [extract_block(owner, marker) for marker in (
        "bool isBleServerInitialized() ",
        "static const char* bleStateTextLocked(",
        "const char* getBLEStateString(",
        "void bleEnableStream(",
        "void bleDisableStream(",
        "void bleSetStreamInterval(",
        "bool bleIsStreamEnabled(")]
    definitions += [extract_block(owner, marker) for marker in (
        "bool isBLEConnected(",
        "static bool bleIsPeerLinkType(BLEDeviceType type) ",
        "BleDisconnectResult bleDisconnectConnection(",
        "int bleDisconnectAppClients(",
        "void bleUpdateStreams(")]
    definitions.append(extract_block(owner, "struct BleSessionFence") + ";")
    definitions += pipe_host.router_definitions(owner)
    definitions += [extract_block(owner, marker) for marker in (
        "bool sendBLEResponseToSession(",
        "static int bleCollectMirrorSessions(",
        "bool sendBLEResponse(",
        "static size_t jsonEscapeInto(",
        "bool blePushNotification(")]
    definitions.append(extract_block(ota, "bool currentSecureBleConnection("))
    harness = (HERE / "ble_pipe_console_row_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def check_sources(component: Path) -> None:
    owner = (component / "Bluetooth.cpp").read_text()
    code = "\n".join(line.split("//", 1)[0] for line in owner.splitlines())
    # Every call into the pipe's console driver or owner goes through the
    # functions the harness runs (whose lock order the oracle checks), and the
    # owner's lock probe only guards the output batch.
    for call, allowed in (
            ("pipeConsoleWrite(", ("static bool blePipeWrite(",)),
            ("pipeConsoleRequestClose(", ("int bleRevokeUserSessions(", "int bleRevokeAllSessions(",
                                          "BleDisconnectResult bleDisconnectConnection(",
                                          "int bleDisconnectAppClients(")),
            ("phonePipeOwnerHeldByCurrentTask(", ("void bleCommandOutputLine(",)),
            ("pipeConsoleHeldByCurrentTask(", ("void bleCommandOutputLine(",))):
        inside = sum(code_only_count(owner, marker, call) for marker in allowed)
        require(code.count(call) == inside,
                f"{call} is called outside {', '.join(allowed)} ({code.count(call)} calls, {inside} there)")
    # The holders: the server role takes its bit with the state; the client
    # host joins a state the pipe holds; the teardown hooks release by holder.
    init = extract_block(owner, "bool initBluetooth(")
    require("sBleStateHolders = kBleHoldServer;" in init, "initBluetooth takes the server's hold")
    register = extract_block(owner, "bool bleConsoleRegisterOnClientHost(")
    require("bleStateHeldByHostRole()" in register and
            "sBleStateHolders |= kBleHoldClientConsole;" in register and
            "bleStateReleaseLocked(kBleHoldClientConsole);" in register,
            "registration joins a pipe-held state and releases only its own hold")
    end = extract_block(owner, "void bleConsoleHostTeardownEnd(")
    require("bleStateReleaseLocked(kBleHoldClientConsole);" in end and "ps_delete" not in end,
            "the client host's teardown releases its hold, never the pipe's")
    begin = extract_block(owner, "void bleConsoleHostTeardownBegin(")
    require("blePipeHostTeardown();" in begin and "(void)blePipeRowEndLocked();" in begin,
            "every non-server host teardown ends the console row")
    streams = extract_block(owner, "void bleUpdateStreams(")
    require(streams.index("blePipeConsoleReconcile();") <
            streams.index("if (!isBLEConnected()) return;"),
            "the main loop reconciles the console row before its no-client return")


def code_only_count(owner: str, marker: str, call: str) -> int:
    body = extract_block(owner, marker)
    return "\n".join(line.split("//", 1)[0] for line in body.splitlines()).count(call)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp/.h and System_OTA.cpp are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    check_sources(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-ble-pipe-console-row-") as directory:
        work = Path(directory)
        source = work / "pipe_console_row.cpp"
        binary = work / "pipe_console_row"
        source.write_text(generated(args.component))
        built = subprocess.run([args.cxx, *flags, "-I", str(work), "-DENABLE_G2_GLASSES=1",
                                "-DENABLE_BLE_PHONE_PIPE=1", str(source), "-o", str(binary)],
                               text=True, capture_output=True, timeout=180)
        if built.returncode != 0:
            print(built.stdout + built.stderr)
            raise SystemExit("FAIL: the console-row harness does not compile")
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=120)
        print((ran.stdout + ran.stderr).strip())
        require(ran.returncode == 0, "the console-row harness failed")
    print("BLE console row: bind/end and its snapshot, login always required, no identity "
          "crossing with design A's slot, CLOSE 5 on revoke and 2 on bledisconnect, the record "
          "ingress and output mapping, reconcile, no leaked sessions, no pipe call under the "
          "lifecycle lock")


if __name__ == "__main__":
    main()
