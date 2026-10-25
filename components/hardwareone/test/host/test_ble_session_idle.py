#!/usr/bin/env python3
"""Run the BLE idle logout (bleSessionTick) as shipped.

Bluetooth.cpp's lifecycle guard, slot and session-epoch helpers, bleNewState,
the phone-link console slot maker, bleBindSession, bleMarkActivity and
bleSessionTick run here with System_User.cpp's shared idle policy, over doubles
of the clock, the lifecycle mutex, transport sessions, the directed sender and
OTA.

The main loop's tick and cmd_exec_task's command stamp meet at the lifecycle
lock. The tick must read the clock under that lock, after any stamp made while
it waited: a clock read before the lock is older than such a stamp, the idle
time underflows and the session that just ran a command is logged out (the
phone-link "Session expired" reply to `phone status`, and the OTA upload logged
out mid-transfer). Cases: login, commands at +60 s and +120 s and one landing
during the tick at +180 s keep the session; 15 minutes idle expires it once,
on the new session; server mode's paced expiry notice for one session does not
expire another whose command lands meanwhile; an OTA upload in flight outranks
the window; 0 disables it. The phone link's console row (phone link v2) sends
its notice TryNow from the main loop; a full stream keeps it pending, offered
each tick while that session lasts, and a new session drops it. Source checks
pin the command and OTA frame stamps and keep every lastActivityMs writer under
the lifecycle lock.
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

# Every function that writes lastActivityMs. Each holds the lifecycle lock
# (or runs before the state is published), which the tick's clock read relies on.
ACTIVITY_WRITERS = (
    "static void bleMarkActivity(",
    "void bleClearConnectionByConnId(",
    "static bool bleBindSession(",
    "static bool bleConsoleMakeSlotLocked(",
    "class ServerCallbacks",
    "static BLESystemState* bleNewState(",
    "bool blePipeConsoleBind(",
)


def code_only(text: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    user_cpp = (component / "System_User.cpp").read_text()
    user_h = (component / "System_User.h").read_text()
    types = [
        (component / "BLE_Types.h").read_text(),
        extract_block(user_h, "enum CommandSource") + ";",
        extract_block(header, "struct BLEConnection") + ";",
        extract_block(header, "struct BLESystemState") + ";",
        # The phone link's console row (phone link v2).
        pipe_host.pipe_types(component),
    ]
    epochs = re.search(r"^std::atomic<TransportSessionEpoch> sBleSessionEpoch\[BLE_MAX_CONNECTIONS\];",
                       owner, re.MULTILINE)
    require(epochs is not None, "Bluetooth.cpp keeps a session epoch per slot")
    definitions = [
        extract_block(user_cpp, "unsigned long sessionIdleWindowMs(CommandSource transport)"),
        extract_block(user_cpp, "bool sessionIdleExpired(unsigned long lastInteractionMs, "
                                "unsigned long nowMs, unsigned long windowMs)"),
        extract_block(user_cpp, "bool sessionIdleExpired(CommandSource transport, "
                                "unsigned long lastInteractionMs, unsigned long nowMs)"),
        extract_block(owner, "class BleLifecycleGuard") + ";",
        epochs[0],
    ]
    definitions += [extract_block(owner, marker) for marker in (
        "int findConnectionSlotByConnIdLocked(",
        "bool bleConnectionEpochMatchesLocked(")]
    definitions += [pipe_host.slot_state(owner), pipe_host.row_helpers(owner)]
    definitions += [extract_block(owner, marker) for marker in (
        "void bleInvalidateSessionSlotLocked(",
        "TransportSessionEpoch blePublishSessionSlotLocked(",
        "TransportSessionEpoch bleSessionEpochForConnection(",
        "static BLESystemState* bleNewState(",
        "const char* bleDeviceTypeToString(",
        "void bleClearConnectionByConnId(",
        "static bool bleConsoleMakeSlotLocked(",
        "static void bleMarkActivity(",
        "bool bleHasAuthenticatedSession(",
        "static bool bleBindSession(",
        "void bleSessionTick(")]
    harness = (HERE / "ble_session_idle_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def check_sources(component: Path) -> None:
    owner = (component / "Bluetooth.cpp").read_text()
    code = code_only(owner)
    # Every command line stamps its slot before it is parsed or run, and so
    # does every decrypted OTA frame: bleMarkActivity is what the harness lands.
    line = code_only(extract_block(owner, "static void processBleCommandLine("))
    require("bleMarkActivity(connId);" in line and
            line.index("bleMarkActivity(connId);") < line.index("previewArgs.arg(0).equalsIgnoreCase(\"login\")"),
            "processBleCommandLine stamps activity before handling the command")
    inbound = code_only(extract_block(owner, "static void bleScDeferredInbound("))
    binary = inbound[inbound.index("BLE_SC_BINARY_READY"):]
    require(binary.index("bleMarkActivity(d->connId);") < binary.index("otaBleHandleEncryptedFrame("),
            "an encrypted OTA frame stamps activity before it is handled")
    # The tick compares each stamp with a clock read under the lifecycle lock
    # (the cases above); that holds only while every writer takes that lock.
    writes = len(re.findall(r"\blastActivityMs\s*=[^=]", code))
    covered = sum(len(re.findall(r"\blastActivityMs\s*=[^=]", code_only(extract_block(owner, marker))))
                  for marker in ACTIVITY_WRITERS)
    require(writes == covered,
            f"lastActivityMs is written only by {', '.join(ACTIVITY_WRITERS)} "
            f"({writes} writes, {covered} in those): a new writer must hold the lifecycle lock")
    for marker in ("static void bleMarkActivity(", "void bleClearConnectionByConnId(",
                   "static bool bleBindSession("):
        body = code_only(extract_block(owner, marker))
        require("BleLifecycleGuard guard;" in body and "lastActivityMs =" in body and
                body.index("BleLifecycleGuard guard;") < body.index("lastActivityMs ="),
                f"{marker} stamps under the lifecycle lock")
    # The console row's notice leaves the main loop as one TEXT record offered
    # now (TryNow), never a blocking send; the harness runs its retry.
    tick = code_only(extract_block(owner, "void bleSessionTick("))
    require("pipe = blePipeRow(conn);" in tick and
            "sendBLEResponseToSession(connId, logoutEpoch, msg, strlen(msg),\n"
            "                                                 /*blocking=*/false);" in
            extract_block(owner, "void bleSessionTick("),
            "the console row's idle notice is sent TryNow (blocking=false)")
    connect = code_only(extract_block(owner, "class ServerCallbacks"))
    connect = connect[connect.index("void onConnect("):connect.index("void onDisconnect(")]
    require("lastActivityMs =" in connect and
            connect.index("BleLifecycleGuard lifecycle;") < connect.index("lastActivityMs ="),
            "onConnect stamps under the lifecycle lock")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp and System_User.cpp are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    check_sources(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-ble-session-idle-") as directory:
        work = Path(directory)
        shutil.copyfile(HERE / "web_batch_stubs" / "Arduino.h", work / "Arduino.h")
        source = work / "session_idle.cpp"
        binary = work / "session_idle"
        source.write_text(generated(args.component))
        built = subprocess.run([args.cxx, *flags, "-I", str(work), str(source), "-o", str(binary)],
                               text=True, capture_output=True, timeout=180)
        if built.returncode != 0:
            print(built.stdout + built.stderr)
            raise SystemExit("FAIL: the BLE idle-logout harness does not compile")
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=120)
        print((ran.stdout + ran.stderr).strip())
        require(ran.returncode == 0, "the BLE idle-logout harness failed")
    print("BLE idle logout: commands keep the phone-link and server sessions; 15 minutes idle expires them")


if __name__ == "__main__":
    main()
