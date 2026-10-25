#!/usr/bin/env python3
"""Run the production BLE server stop and the `closeconsole` / `blemode` commands.

deinitBluetooth(), its session and advertising helpers, the role-claim and
lifecycle-guard classes and both command handlers are extracted unchanged from
Bluetooth.cpp; the role transition, Ring owner, host/controller teardown,
transport sessions and settings writes are host boundaries. A stop the Ring
owner or a busy role transition defers, or a host teardown that fails, must be
reported to the caller: both commands answer an error and leave `bleMode`
alone, and a deferred stop leaves the app sessions and advertising running,
except under a companion reset's RECOVERING claim, which retries the stop and,
if it never completes, lets the companion reboot under a host it does not force
down (companion_ble_shutdown): there a Ring-deferred stop closes session
admission (epochs, GATT interface id) and advertising before it returns.
Built with and without the G2 client; the disabled-Bluetooth header stub is
compiled too. --component points the suite at another copy of the BLE sources
(Bluetooth.cpp/.h) to show the pre-fix behavior.

`blemode` is the saved BOOT role only (client|server; the phone-client mode
is gone, the pipe is a device on the shared client stack): it stops nothing
and claims no role. It answers in the link-status words (BLE_LinkStatusCore.h):
the query names the saved mode and the role running now through the shipped
bleLinkStatus() (BLE_LinkStatus.cpp) over host role/pipe/console inputs, the
set replies name the stored role, what runs now (app console server, client
stack, nothing) and the verb that starts the stored role; the old `phone`
alias (which meant server) and the removed phone-client keywords are refused
without touching any role.

The app console on the G2 client host runs here too: the shipped
bleConsoleRegisterOnClientHost(), its shared state and command-service
builders, the GATT registration tap, ServerCallbacks and the two host-teardown
hooks, over recording Arduino BLE objects. Registration builds only the command
service (never advertising, the MTU or TX power), checks the GATT registration
(also when the tap sees it after createServer returns) and refuses before
creating anything outside a role claim, under the lifecycle lock, on a host
that is not ready, with a link held or registered, or with no free GATT app id;
the server app registers an id from the reserved 15-bit client allocator, also
once the raw counter has passed ESP_APP_ID_MAX. The request characteristic is
write-only (any link could otherwise read back the last command).
No link Bluedroid reports to it gets a slot, a count, an event or an
advertising call, while server mode still admits its apps; once the phone pipe
is READY the shipped reconcile makes exactly its link's slot, which a temple or
ring drop leaves and the phone link's drop ends for good (its stale record gets
no slot back). The harness HAL stop calls the hooks as HAL_Bluetooth.cpp does
(test_hal_bluetooth checks that): admission, epochs, pointers and the tap are
closed and the phone-link slot ended before the stop, the attach state starts
the next host empty, the state is freed only after a terminal one and kept
(closed) after a failed one, and the server role's state is left to
deinitBluetooth. The phone link's console row (phone link v2) runs with
gBLEState's holders: the pipe's hold allocates the state in phone-client mode,
registration joins a pipe-held state, each teardown releases only its own hold
(the state goes with the last), a host teardown ends the row and closes its
session before the stop with no CLOSE, the server's state is never the pipe's,
binding is idempotent per stream, End matches generation and stream, and
start/stop cycles return every transport session. Source checks keep the command
service built in one place and the client-host path free of advertising, MTU
and TX-power calls.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require, JSON_INCLUDE
import ble_pipe_console_host as pipe_host

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def link_status_types(component: Path) -> list[str]:
    """BLE_LinkStatusCore.h, BleConsoleView and BleLinkStatus, as shipped."""
    core = (component / "BLE_LinkStatusCore.h").read_text().replace("#pragma once\n", "", 1)
    header = (component / "Bluetooth.h").read_text()
    link = (component / "BLE_LinkStatus.h").read_text()
    return [core, extract_block(header, "struct BleConsoleView") + ";",
            extract_block(link, "struct BleLinkStatus") + ";"]


def link_status_definitions(component: Path, json: bool = False) -> list[str]:
    """bleLinkStatus() (and bleLinkStatusJson) from BLE_LinkStatus.cpp. The caller
    supplies its inputs: the role probes, the phonePipe* accessors,
    blePeerReconnectSnapshot (pipe builds), bleConsoleView and gSettings.bleMode."""
    source = (component / "BLE_LinkStatus.cpp").read_text()
    out = [extract_block(source, "BleLinkStatus bleLinkStatus() ")]
    if json:
        out.append(extract_block(source, "void bleLinkStatusJson("))
    return out


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    settings = (COMPONENT / "System_Settings.h").read_text()
    hal = (COMPONENT / "HAL_Bluetooth.h").read_text()

    modes = re.findall(r"^#define BLE_MODE_(?:SERVER|G2_CLIENT)\s+\d+", header, re.MULTILINE)
    require(len(modes) == 2, "BLE mode constants (server, client) are present in Bluetooth.h")
    require("BLE_MODE_PHONE_CLIENT" not in header, "the phone-client mode is gone from Bluetooth.h")
    default = re.search(r"static constexpr int kBleModeDefaultForBuild = [^;]+;", settings)
    require(default is not None, "kBleModeDefaultForBuild is present in System_Settings.h")
    console = (component / "BLE_ConsoleAttachCore.h").read_text().replace("#pragma once\n", "", 1)
    uuids = re.findall(r"^#define BLE_\w+_UUID\s+\"[^\"]+\"", header, re.MULTILINE)
    require(len(uuids) >= 7, "BLE service and characteristic UUIDs are present in Bluetooth.h")
    types = [console, *modes, extract_block(header, "inline bool bleModeIsAvailable("),
             *uuids, default[0], (COMPONENT / "BLE_Types.h").read_text(),
             extract_block(header, "enum class BleRoleTransition") + ";",
             extract_block(hal, "struct BluetoothHalShutdownResult") + ";",
             "using TransportSessionEpoch = uint32_t;",
             extract_block(header, "struct BLEConnection") + ";",
             extract_block(header, "struct BLESystemState") + ";"]
    pipe = (COMPONENT / "BLE_PhonePipe.h").read_text()
    record = re.search(r"^struct PhonePipeLinkRecord \{[^}]*\};", pipe, re.MULTILINE)
    require(record is not None, "BLE_PhonePipe.h declares PhonePipeLinkRecord")
    types.append(record[0])
    # The phone link's console row (phone link v2): its types and the holder bits.
    types += [pipe_host.pipe_types(component), pipe_host.holder_state(owner)]
    types += link_status_types(COMPONENT)
    peers = (COMPONENT / "BLE_Peers.h").read_text()
    types += [extract_block(peers, "enum BlePeerKind") + ";",
              extract_block(peers, "struct BlePeerReconnectSnapshot") + ";"]

    stop = "bool deinitBluetooth(" if "bool deinitBluetooth(" in owner else "void deinitBluetooth("
    if stop.startswith("void"):
        print("deinitBluetooth returns void (pre-fix source)")
    definitions = [extract_block(owner, "class BleRoleTransitionClaim") + ";",
                   extract_block(owner, "class BleLifecycleGuard") + ";"]
    # The console's state flags and the GATT registration tap, as shipped.
    for pattern in (r"^static std::atomic<bool> sBleServerUp\{[^}]*\};",
                    r"^static std::atomic<hw1console::Mode> sBleConsoleMode\{[^}]*\};",
                    r"^static std::atomic<bool> sBleConsoleAdmissionOpen\{[^}]*\};",
                    r"^static hw1console::Attach sBleConsoleAttach;",
                    r"^static hw1console::Slot sBleConsoleLogged;",
                    r"^static std::atomic<int> sBleConsoleSlotConnId\{[^}]*\};",
                    r"^static std::atomic<int> sBleConsoleSendsInFlight\{[^}]*\};",
                    r"^constexpr int kBleGattsRegPending = [^;]+;",
                    r"^std::atomic<int> sBleGattsRegStatus\{[^}]*\};"):
        found = re.search(pattern, owner, re.MULTILINE)
        require(found is not None, f"Bluetooth.cpp declares {pattern}")
        definitions.append(found[0])
    # The client-host check every console path reads (folds away without the pipe).
    definitions.append(extract_block(owner, "static inline bool bleConsoleClientHost() "))
    # The console driver's doubles and the console row's functions, declared
    # here and defined after the hooks that call them.
    definitions += ["#define PIPE_HOST_LIFECYCLE_HELD() (gLifecycleMutex.depth != 0)",
                    pipe_host.PIPE_DOUBLES, pipe_host.declarations(owner, pipe_host.ROW_MARKERS)]
    definitions += [extract_block(owner, marker) for marker in (
        "void bleGattsEventTap(",
        "int findConnectionSlotByConnIdLocked(")]
    definitions += [pipe_host.slot_state(owner), pipe_host.row_helpers(owner)]
    definitions += [extract_block(owner, marker) for marker in (
        "void bleInvalidateSessionSlotLocked(",
        "TransportSessionEpoch blePublishSessionSlotLocked(",
        "TransportSessionEpoch bleSessionEpochForConnection(",
        "bool bleSessionEpochMatches(",
        "void bleClearConnectionByConnId(",
        "const char* bleDeviceTypeToString(",
        "static bool bleAdvertiseIfRoomLocked(",
        "bool bleAdvertising(",
        "bool bleServerUp(",
        "hw1console::Mode bleConsoleMode(",
        # The phone-link console's plan helpers ServerCallbacks and the
        # teardown hook apply (test_ble_console_phone_link runs them in full).
        "static hw1console::PipeLink bleConsolePipeLink(",
        "static void bleConsoleEndSlotLocked(",
        "static bool bleConsoleMakeSlotLocked(",
        "static void bleConsoleJoinLocked(",
        "static void bleConsoleStoreLocked(",
        "static void bleConsoleApplyLocked(",
        "static void bleConsoleReconcileLocked(",
        "static bool bleConsoleLinkEventMayMatter(",
        "static bool bleConsoleLinkEventLocked(")]
    definitions.append(extract_block(owner, "class ServerCallbacks") + ";")
    callbacks = re.search(r"^static ServerCallbacks sServerCallbacks;", owner, re.MULTILINE)
    require(callbacks is not None, "Bluetooth.cpp owns the server callbacks object")
    definitions.append(callbacks[0])
    definitions += [extract_block(owner, marker) for marker in (
        "static BLESystemState* bleNewState(",
        "static void bleBuildCommandService(",
        "void stopBLEAdvertising(",
        "bool isBleServerInitialized(",
        stop,
        "bool bleConsoleRegisterOnClientHost(",
        "void bleConsoleHostTeardownBegin(",
        "void bleConsoleHostTeardownEnd(",
        "static const char* cmd_closeconsole(",
        "const char* getBleModeString(",
        "bool bleHasAuthenticatedSession(")]
    definitions += pipe_host.row_definitions(owner)
    definitions += link_status_definitions(COMPONENT)
    definitions.append(extract_block(owner, "static const char* cmd_blemode("))
    harness = (HERE / "ble_server_stop_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


DISABLED_CHECK = '''#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "Bluetooth.h"
#include <cstdio>
#include <type_traits>
void webBatchTestStringAppend() {}
static_assert(std::is_same<decltype(deinitBluetooth()), bool>::value,
              "disabled deinitBluetooth() must report its result like the real one");
int main() {
  if (!deinitBluetooth()) {
    std::puts("FAIL [disabled]: deinitBluetooth() with Bluetooth compiled out must report stopped");
    return 1;
  }
  bleConsoleHostTeardownBegin();
  bleConsoleHostTeardownEnd(true);
  if (bleConsoleRegisterOnClientHost() || bleConsoleMode() != hw1console::Mode::Off) {
    std::puts("FAIL [disabled]: the app console must report unavailable with Bluetooth compiled out");
    return 1;
  }
  return 0;
}
'''


def check_blemode_row(owner: str) -> None:
    """The blemode row names the two boot roles and the verbs that run them:
    client = the shared stack (glasses, ring, phone; default), server = the app
    console server (legacy); no phone-client mode, no aliases."""
    row = re.search(r'\{ "blemode", "([^"]*)", true, cmd_blemode,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', owner)
    require(row is not None, "the blemode registry row is present")
    help_text = row.group(1)
    usage = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', row.group(2)))
    require("client = the shared stack for glasses, ring and phone (default); "
            "server = the app console server (legacy)" in help_text,
            f"blemode help names the two boot roles: {help_text}")
    require(usage.startswith("Usage: blemode [client|server]\\n"),
            f"blemode usage lists client|server only: {usage[:80]}")
    for verb in ("openg2", "openring", "openphone", "openconsole"):
        require(verb in usage, f"blemode usage names the verb that runs the role: {verb}")
    for gone in ("phone-client", "phonelink", "glasses ="):
        require(gone not in help_text and gone not in usage,
                f"blemode row still names the removed mode or alias: {gone}")
    require("Usage: blemode [client|server]" in extract_block(owner, "static const char* cmd_blemode("),
            "the usage error lists client|server only")


def check_sources(component: Path) -> None:
    """The command service has one builder, and the client-host path never
    advertises, raises the MTU, sets TX power or builds another service."""
    owner = (component / "Bluetooth.cpp").read_text()
    check_blemode_row(owner)
    require(owner.count("createService(BLE_COMMAND_SERVICE_UUID)") == 1 and
            "createService(BLE_COMMAND_SERVICE_UUID)" in
            extract_block(owner, "static void bleBuildCommandService("),
            "the command service is built only by bleBuildCommandService")
    init = extract_block(owner, "bool initBluetooth(")
    require("bleBuildCommandService();" in init and "bleNewState()" in init,
            "initBluetooth builds its state and command service with the shared helpers")
    published = init[init.index("gBLEState->initialized = true;"):]
    require(published.index("sBleConsoleMode.store(hw1console::Mode::Server);") <
            published.index("}"),
            "initBluetooth publishes Mode::Server with initialized, under the same lock")
    register = extract_block(owner, "bool bleConsoleRegisterOnClientHost(")
    for banned in ("setMTU", "Advertising", "TxPower", "createService(",
                   "BLE_DATA_SERVICE_UUID", "BLE_DEVICE_INFO_SERVICE_UUID", "initialized = true",
                   "sBleServerUp", "esp_ble_gatts_open", "setCustomGapHandler"):
        code = "\n".join(line.split("//", 1)[0] for line in register.splitlines())
        require(banned not in code, f"bleConsoleRegisterOnClientHost must not use {banned}")
    require(register.index("sBleGattsRegStatus") < register.index("bleBuildCommandService();"),
            "the GATT registration is checked before the command service is created")
    require(register.index("bleRoleTransitionHeldByCurrentTask()") <
            register.index("bleNewState()") < register.index("createServer()"),
            "every precondition is checked before the state or the server app is created")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp/.h are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    check_sources(args.component)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hw1-ble-server-stop-") as directory:
        work = Path(directory)
        shutil.copyfile(HERE / "web_batch_stubs" / "Arduino.h", work / "Arduino.h")

        def compile_and_run(name: str, source: str, extra: list[str]) -> bool:
            path = work / (name + ".cpp")
            path.write_text(source)
            built = subprocess.run([args.cxx, *flags, "-I", str(work), *extra, str(path),
                                    "-o", str(work / name)],
                                   text=True, capture_output=True, timeout=180)
            if built.returncode != 0:
                print(built.stdout + built.stderr)
                print(f"FAIL [{name}]: does not compile")
                return False
            ran = subprocess.run([str(work / name)], text=True, capture_output=True, timeout=60)
            print(ran.stdout + ran.stderr, end="")
            return ran.returncode == 0

        source = generated(args.component)
        for g2 in (1, 0):
            for phone in (1, 0):
                if not compile_and_run(f"server-stop-g2-{g2}-phone-{phone}", source,
                                       [f"-DENABLE_G2_GLASSES={g2}", f"-DENABLE_BLE_PHONE_PIPE={phone}"]):
                    failures.append(f"server stop (G2={g2}, phone={phone})")

        disabled = work / "disabled-include"
        disabled.mkdir()
        shutil.copyfile(args.component / "Bluetooth.h", disabled / "Bluetooth.h")
        shutil.copyfile(args.component / "BLE_ConsoleAttachCore.h",
                        disabled / "BLE_ConsoleAttachCore.h")
        (disabled / "System_BuildConfig.h").write_text("#define ENABLE_BLUETOOTH 0\n#define ENABLE_G2_GLASSES 0\n#define ENABLE_BLE_PHONE_PIPE 0\n")
        (disabled / "System_User.h").write_text(
            "#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n")
        if not compile_and_run("disabled", DISABLED_CHECK,
                               ["-I", str(disabled), "-I", str(JSON_INCLUDE)]):
            failures.append("disabled-Bluetooth deinitBluetooth() stub")
    require(not failures, "BLE server stop failed: " + "; ".join(failures))
    print("BLE server stop: deferred/failed stops reported, mode kept, sessions kept on deferral "
          "(closed under a companion reset); success, not-running and disabled builds passed; "
          "blemode stores the boot role only (client|server), names what runs now, "
          "phone-client/phonelink/`phone`/`phone-only` refused")
    print("App console on the client host: command service only, registration checked, "
          "links ignored, closed before the host stops and freed only after a terminal stop")


if __name__ == "__main__":
    main()
