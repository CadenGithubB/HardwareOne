#!/usr/bin/env python3
"""Run the app console on the phone pipe's link (G2 client host) as shipped.

In G2 client mode the command service sits on the client host and Bluedroid
reports every link to it: both temples, the ring and the phone. Bluetooth.cpp's
server callbacks, write callback, console plan helpers (BLE_ConsoleAttachCore.h),
directed senders, console view, `bledisconnect`, `bleadv`, `bleconsole`, the
host-teardown hooks and bleUpdateStreams run here with BLE_LinkStatus.cpp's
bleLinkStatus(), over recording doubles of the pipe's link record, the Secure
Channel, the central TX gate, transport sessions and the Arduino GATT objects.

Cases: a table over every link id and pipe record that the temples and the ring
never become a console slot and that a reused connection id never inherits one
(a later link on the id, a missed disconnect, READY racing a drop); the main
loop reconciles before its no-client return; every write reconciles first, so a
slot whose pipe left READY takes no command; a READY with no app is no connected
app until the app's first write joins the slot; Secure Channel only, never
plaintext; every console frame one central TX gate try before the lifecycle
lock, posted with that lock released and counted in flight, noted as pipe
activity; `bledisconnect` ends a joined session without a GATT close, and on
the pipe's console row (phone link v2) answers the same, renewing its stream
(CLOSE 2); `bleadv`
refuses; `bleconsole on|off|status` and its persisted setting; the status read
answers the slot's link only; the teardown hook ends the slot, resets the attach
state and waits for a post in flight; server mode keeps its path; the console
words on the status surfaces; and a random walk over a model of the four C6
links. Source checks keep the
BTC-side helpers free of logging, waits, advertising, MTU and GATT open/close
calls, and pin the registry row and the settings row.
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

# The BTC-side helpers: they run in the server callbacks and the write fast path.
BTC_HELPERS = (
    "static hw1console::PipeLink bleConsolePipeLink(",
    "static void bleConsoleEndSlotLocked(",
    "static bool bleConsoleMakeSlotLocked(",
    "static void bleConsoleJoinLocked(",
    "static void bleConsoleStoreLocked(",
    "static void bleConsoleApplyLocked(",
    "static void bleConsoleReconcileLocked(",
    "static bool bleConsoleLinkEventMayMatter(",
    "static bool bleConsoleLinkEventLocked(",
    "static bool bleConsoleAdmitWriteLocked(",
)


def code_only(text: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    link = (component / "BLE_LinkStatus.cpp").read_text()
    link_header = (component / "BLE_LinkStatus.h").read_text()
    peers = (COMPONENT / "BLE_Peers.h").read_text()
    pipe = (COMPONENT / "BLE_PhonePipe.h").read_text()
    settings = (COMPONENT / "System_Settings.h").read_text()
    utils = (COMPONENT / "System_Utils.h").read_text()

    record = re.search(r"^struct PhonePipeLinkRecord \{[^}]*\};", pipe, re.MULTILINE)
    require(record is not None, "BLE_PhonePipe.h declares PhonePipeLinkRecord")
    default = re.search(r"static constexpr int kBleModeDefaultForBuild = [^;]+;", settings)
    require(default is not None, "System_Settings.h has kBleModeDefaultForBuild")
    types = [
        (component / "BLE_ConsoleAttachCore.h").read_text().replace("#pragma once\n", "", 1),
        (component / "BLE_LinkStatusCore.h").read_text().replace("#pragma once\n", "", 1),
        (COMPONENT / "BLE_Types.h").read_text(),
        "\n".join(re.findall(r"^#define BLE_MODE_\w+\s+\d+.*$", header, re.MULTILINE)),
        "\n".join(re.findall(r'^#define BLE_\w+_UUID\s+"[^"]+".*$', header, re.MULTILINE)),
        default[0],
        record[0],
        extract_block(header, "enum class BleDisconnectResult") + ";",
        extract_block(header, "struct BLEConnection") + ";",
        extract_block(header, "struct BLESystemState") + ";",
        extract_block(header, "struct BleConsoleView") + ";",
        extract_block(peers, "enum BlePeerKind") + ";",
        extract_block(peers, "struct BlePeerReconnectSnapshot") + ";",
        extract_block(link_header, "struct BleLinkStatus") + ";",
        extract_block(owner, "struct BleScDeferred") + ";",
        extract_block(settings, "enum SettingType") + ";",
        extract_block(settings, "struct SettingEntry {") + ";",
    ]

    # The console's state, before the host boundaries (the GATT send double
    # reads the mode and the posts in flight).
    for pattern in (r"^static std::atomic<hw1console::Mode> sBleConsoleMode\{[^}]*\};",
                    r"^static std::atomic<bool> sBleConsoleAdmissionOpen\{[^}]*\};",
                    r"^static hw1console::Attach sBleConsoleAttach;",
                    r"^static hw1console::Slot sBleConsoleLogged;",
                    r"^static std::atomic<int> sBleConsoleSlotConnId\{[^}]*\};",
                    r"^static std::atomic<int> sBleConsoleSendsInFlight\{[^}]*\};"):
        found = re.search(pattern, owner, re.MULTILINE)
        require(found is not None, f"Bluetooth.cpp declares {pattern}")
        types.append(found[0])
    # The client-host check every console path reads (folds away without the pipe).
    types.append(extract_block(owner, "static inline bool bleConsoleClientHost() "))
    # The phone link's console row (phone link v2): its types and holder bits.
    types += [pipe_host.pipe_types(component), pipe_host.holder_state(owner)]
    definitions = []
    definitions.append(extract_block(owner, "class BleLifecycleGuard") + ";")
    # The console driver's doubles, and every console-row and router function
    # declared up front (their definitions come last).
    definitions += ["#define PIPE_HOST_LIFECYCLE_HELD() (gLifecycle.depth != 0)",
                    pipe_host.PIPE_DOUBLES,
                    pipe_host.declarations(owner, pipe_host.ROW_MARKERS + pipe_host.ROUTER_MARKERS)]
    definitions += [extract_block(owner, marker) for marker in (
        "int findConnectionSlotByConnIdLocked(",
        "bool bleConnectionEpochMatchesLocked(")]
    definitions += [pipe_host.slot_state(owner), pipe_host.row_helpers(owner)]
    definitions += [extract_block(owner, marker) for marker in (
        "void bleInvalidateSessionSlotLocked(",
        "TransportSessionEpoch blePublishSessionSlotLocked(",
        "TransportSessionEpoch bleSessionEpochForConnection(",
        "bool bleSessionEpochMatches(",
        "void bleClearConnectionByConnId(",
        "const char* bleDeviceTypeToString(",
        "static bool bleAdvertiseIfRoomLocked(",
        "hw1console::Mode bleConsoleMode(",
        "static BLESystemState* bleNewState(",
        *BTC_HELPERS,
        "static void bleConsoleReconcile(")]
    definitions.append(extract_block(owner, "class ServerCallbacks") + ";")
    definitions.append(extract_block(owner, "class CmdRequestCallbacks") + ";")
    definitions.append(extract_block(owner, "class CmdStatusCallbacks") + ";")
    for pattern in (r"^static ServerCallbacks sServerCallbacks;",
                    r"^static CmdRequestCallbacks sCmdRequestCallbacks;",
                    r"^static CmdStatusCallbacks sCmdStatusCallbacks;"):
        found = re.search(pattern, owner, re.MULTILINE)
        require(found is not None, f"Bluetooth.cpp owns {pattern}")
        definitions.append(found[0])
    definitions += [extract_block(owner, marker) for marker in (
        "static bool bleSecureChannelOnly(",
        "static void blePlaintextDeferredInbound(")]
    definitions.append(extract_block(owner, "class BleConsoleTxGate") + ";")
    definitions += [extract_block(owner, marker) for marker in (
        "bool bleRawNotifyToSession(uint16_t connId,\n",
        "bool sendBLEResponseToSession(",
        "static bool bleIsPeerLinkType(BLEDeviceType type) ",
        "BleConsoleView bleConsoleView() ",
        "bool isBLEConnected(",
        "BleDisconnectResult bleDisconnectConnection(",
        "int bleDisconnectAppClients(",
        "static const char* bleConsoleSwitchHint(")]
    # The audit identity a phone-link command carries (no address).
    definitions += [extract_block(utils, "inline void macToDisplay("),
                    extract_block(owner, "static String blePeerIpTag(")]
    definitions.append(extract_block(link, "BleLinkStatus bleLinkStatus()"))
    definitions.append(extract_block(utils, "inline int parseBoolArg("))
    definitions += re.findall(r"(?ms)^static const char kBlePhoneLink\w+\[\] =.*?;$", owner)
    definitions += [extract_block(owner, marker) for marker in (
        "static const char* cmd_bledisconnect(",
        "static const char* cmd_bleadv(",
        "static const char* cmd_bleconsole(",
        "void bleConsoleHostTeardownBegin(",
        "void bleConsoleHostTeardownEnd(",
        "void bleUpdateStreams(")]
    definitions.append(extract_block(owner, "bool bleHasAuthenticatedSession("))
    definitions += pipe_host.row_definitions(owner) + pipe_host.router_definitions(owner)
    # The settings table, compiled whole: the new row's type, default and cmdKey.
    start = owner.index("static constexpr const char* kBleModeChoices")
    count = owner.index("const size_t bluetoothSettingsCount", start)
    table = owner[start:owner.index("\n", count)]
    definitions.append(table)
    harness = (HERE / "ble_console_phone_link_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def check_sources(component: Path) -> None:
    owner = (component / "Bluetooth.cpp").read_text()
    # The BTC-side helpers wait only for the lifecycle lock and never log,
    # advertise, change the MTU or open/close a GATT connection.
    for marker in BTC_HELPERS:
        code = code_only(extract_block(owner, marker))
        for banned in ("BLE_DEBUGF", "broadcastOutput", "BROADCAST_PRINTF", "vTaskDelay",
                       "Advertising", "setMTU", "esp_ble_gatts_open", "esp_ble_gatts_close",
                       "disconnect(", "BleLifecycleGuard", "bleCentralTx"):
            require(banned not in code, f"{marker} must not use {banned}")
    require("esp_ble_gatts_open" not in code_only(owner), "the console never holds the link (no gatts_open)")
    require(code_only(owner).count("setMTU(") == 1 and
            "setMTU(" in code_only(extract_block(owner, "bool initBluetooth(")),
            "only the server start raises the local MTU")
    # The main loop reconciles before its no-client return.
    streams = code_only(extract_block(owner, "void bleUpdateStreams("))
    require(streams.index("bleConsoleReconcile();") <
            streams.index("if (!isBLEConnected()) return;"),
            "bleUpdateStreams reconciles the phone-link console before its early return")
    # Both link callbacks take the plan under the lifecycle lock before the server path.
    callbacks = code_only(extract_block(owner, "class ServerCallbacks"))
    connect = callbacks[callbacks.index("void onConnect("):callbacks.index("void onDisconnect(")]
    require(connect.index("bleConsoleLinkEventMayMatter(param->connect.conn_id)") <
            connect.index("BleLifecycleGuard lifecycle;") <
            connect.index("bleConsoleLinkEventLocked(/*up=*/true") <
            connect.index("gBLEState->connections[slot].active = true;"),
            "onConnect decides by the attach core under the lock before the server path")
    disconnect = callbacks[callbacks.index("void onDisconnect("):]
    require(disconnect.index("bleConsoleLinkEventMayMatter(param->disconnect.conn_id)") <
            disconnect.index("BleLifecycleGuard lifecycle;") <
            disconnect.index("bleConsoleLinkEventLocked(/*up=*/false") <
            disconnect.index("bleClearConnectionByConnId("),
            "onDisconnect decides by the attach core under the lock before the server path")
    write = code_only(extract_block(owner, "class CmdRequestCallbacks"))
    require(write.index("bleConsoleAdmitWriteLocked(param->write.conn_id)") <
            write.index("gBLEState->commandsReceived++;"),
            "the write callback admits a client-host write before counting it")
    # The raw sender tries the gate before its lifecycle lock, per attempt.
    raw = code_only(extract_block(owner, "bool bleRawNotifyToSession(uint16_t connId,\n"))
    require(raw.index("BleConsoleTxGate gate(blocking);") < raw.index("BleLifecycleGuard lifecycle;") <
            raw.index("vTaskDelay(kBackoff)"),
            "each send attempt takes the central TX gate, then the lifecycle lock, and backs off unheld")
    # The client host's post runs counted in flight; the teardown waits it out
    # after closing the epochs and before the host stops.
    require(raw.index("sBleConsoleSendsInFlight.fetch_add(1);") <
            raw.index("sBleConsoleSendsInFlight.fetch_sub(1);"),
            "the client host's post is counted in flight")
    register = code_only(extract_block(owner, "bool bleConsoleRegisterOnClientHost("))
    require("bleConsoleStoreLocked(hw1console::Attach{});" in register,
            "registration starts the host with an empty attach state")
    begin = code_only(extract_block(owner, "void bleConsoleHostTeardownBegin("))
    require(begin.index("sBleConsoleAdmissionOpen.store(false);") <
            begin.index("bleConsoleReconcileLocked();") <
            begin.index("bleConsoleStoreLocked(hw1console::Attach{});") <
            begin.index("bleInvalidateSessionSlotLocked(i);") <
            begin.index("while (sBleConsoleSendsInFlight.load() > 0)"),
            "teardown closes admission, ends the slot, resets the attach state, closes the epochs, "
            "then waits for posts in flight")
    row = re.search(r'\{ "bleconsole",[^}]*\}', owner)
    require(row is not None and ", true, cmd_bleconsole," in row[0] and
            '"Usage: bleconsole [on|off|status]"' in row[0],
            "bleconsole is an admin command with its usage")
    setting = re.search(r'\{ "bleConsoleOnPipe", SETTING_BOOL, &gSettings\.bleConsoleOnPipe, true,[^}]*'
                        r'"bleconsole" \}', owner)
    require(setting is not None, "the bleConsoleOnPipe setting row defaults on and edits through bleconsole")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp/.h and link status are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    check_sources(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-ble-console-phone-link-") as directory:
        work = Path(directory)
        shutil.copyfile(HERE / "web_batch_stubs" / "Arduino.h", work / "Arduino.h")
        source = work / "phone_link.cpp"
        binary = work / "phone_link"
        source.write_text(generated(args.component))
        built = subprocess.run([args.cxx, *flags, "-I", str(work), "-DENABLE_G2_GLASSES=1",
                                "-DENABLE_BLE_PHONE_PIPE=1", str(source), "-o", str(binary)],
                               text=True, capture_output=True, timeout=180)
        if built.returncode != 0:
            print(built.stdout + built.stderr)
            raise SystemExit("FAIL: the phone-link console harness does not compile")
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=120)
        print((ran.stdout + ran.stderr).strip())
        require(ran.returncode == 0, "the phone-link console harness failed")
    print("BLE console on the phone link: only the ready pipe's link is a slot, by connection id; "
          "Secure Channel only, gated sends, session-only disconnect, bleconsole, teardown and the "
          "status words passed")


if __name__ == "__main__":
    main()
