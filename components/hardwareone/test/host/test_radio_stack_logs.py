#!/usr/bin/env python3
"""The radio stacks' own log tags: quiet unless their debug flag is on.

Executes the shipping System_RadioLogCore.h (the tag table, the level each tag
gets, the C6 console's line levels), the esplog block and the radio stack
apply of System_Debug.cpp, and HAL_Radio's quietHostedLogs() against a model
of ESP-IDF's tag levels:

* default: every tag of the build at ERROR, real errors kept;
* debugwifidriver brings ESP-Hosted's tags and C6> back to their stock level,
  debugbluetoothcore Bluedroid's, BLE_COEX and C6>; off again, quiet again;
* a saved esplog level wins over both, a saved wildcard takes the quiet
  default's place, and removing either takes effect at once;
* the boot order (quiet before the settings, applySettings, the boot's esplog
  apply) and a new Hosted session (esp_hosted_init's WARN) end on the same
  levels.

Then pins in source what the harness takes as given: the flag readers, every
caller (the two flag commands on both paths, debugverbose, applySettings, the
wildcard, the paths that replace the whole debug mask, the companion's
TRANSPORT_UP and failure), the esplog lock around the apply, HAL's quiet start and C6
line levels, the BLE_COEX lines following their tag, the help text, and that
every tag in the table is one this build's sources log with.
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
from test_esp_log_overrides import COMMAND, START, order, string_double

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ROOT = COMPONENT.parents[1]
APPLY = "// ---- radio stack logs: apply"
APPLY_FN = "void applyRadioStackLogLevels()"


def generate() -> str:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    radio = (COMPONENT / "HAL_Radio.cpp").read_text()
    require(debug.count(START) == 1 and debug.count(COMMAND) == 1, "one esplog block in System_Debug.cpp")
    require(debug.count(APPLY) == 1 and debug.count(APPLY_FN + " {") == 1, "one radio stack apply")
    command = extract_block(debug, COMMAND)
    esplog_block = debug[debug.index(START):debug.index(command) + len(command)]
    apply_fn = extract_block(debug, APPLY_FN)
    apply_block = debug[debug.index(APPLY):debug.index(apply_fn) + len(apply_fn)]
    require(debug.index(APPLY) > debug.index(command), "the apply follows the esplog block (espLogStored)")
    quiet = extract_block(radio, "void quietHostedLogs()")
    harness = (HERE / "radio_stack_logs_harness.cpp").read_text()
    for marker, value in (("// INSERT_ESPLOG", esplog_block),
                          ("// INSERT_RADIO_APPLY", apply_block), ("// INSERT_QUIET_HOSTED", quiet)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def run_harness(cxx: str, sanitize: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="hw1-radiolog-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        (Path(temporary) / "Arduino.h").write_text(string_double())
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
                   "-I", temporary, "-I", str(COMPONENT), str(unit), "-o", str(binary)]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"radio stack logs harness failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


def table_tags() -> list[tuple[str, str]]:
    core = (COMPONENT / "System_RadioLogCore.h").read_text()
    rows = re.findall(r'^\s*\{("[^"]+"|kC6ConsoleTag), Family::\w+, Stack::(\w+), k\w+\},', core, re.M)
    require(len(rows) == 24, f"24 table rows ({len(rows)})")
    return [(tag.strip('"') if tag != "kC6ConsoleTag" else "C6>", stack) for tag, stack in rows]


def tag_sources() -> None:
    """Every tag in the table is one this build's sources log with."""
    hosted_dir = ROOT / ".sdk-overrides/esp32p4/espressif__esp_hosted"
    bt_dir = ROOT / ".sdk-overrides/esp32p4/bt"
    idf = Path(os.environ.get("IDF_PATH") or Path.home() / "esp/esp-idf")
    found: dict[str, set[str]] = {"Hosted": set(), "Bluedroid": set(), "BleController": set(),
                                  "BtdmController": set(), "CoexRecords": set()}
    for path in list((hosted_dir / "host").rglob("*.[ch]")) + [hosted_dir / "common/utils/esp_hosted_cli.c"]:
        text = path.read_text(errors="replace")
        found["Hosted"].update(re.findall(r"DEFINE_LOG_TAG\((\w+)\)", text))
        found["Hosted"].update(re.findall(r'\bTAG\s*(?:\[\])?\s*=\s*"([^"]+)"', text))
    remote = ROOT / "managed_components/espressif__esp_wifi_remote"
    if remote.exists():
        for path in remote.glob("*.c"):
            found["Hosted"].update(re.findall(r'ESP_LOG[EWIDV]\("([^"]+)"', path.read_text(errors="replace")))
    else:
        found["Hosted"].add("esp_wifi_remote")  # managed components not fetched here
    radio = (COMPONENT / "HAL_Radio.cpp").read_text()
    if "radiolog::kC6ConsoleTag" in radio:
        found["Hosted"].add("C6>")
    for name in ("host/bluedroid/common/include/common/bt_trace.h", "common/include/bt_common.h"):
        found["Bluedroid"].update(re.findall(r'BT_PRINT_[EWIDV]\("(BT_\w+)"', (bt_dir / name).read_text()))
    controller = idf / "components/bt/controller"
    if controller.exists():
        found["BleController"].update(re.findall(r'"(BLE_INIT)"', (controller / "esp32c3/bt.c").read_text()))
        found["BtdmController"].update(re.findall(r'"(BTDM_INIT)"', (controller / "esp32/bt.c").read_text()))
    else:
        found["BleController"].add("BLE_INIT")
        found["BtdmController"].add("BTDM_INIT")
    if 'const char* const kTag = "BLE_COEX";' in (COMPONENT / "BLE_CoexRecords.cpp").read_text():
        found["CoexRecords"].add("BLE_COEX")
    for tag, stack in table_tags():
        require(tag in found[stack], f"{tag} is logged with by the {stack} sources")
    # Bluedroid's trace levels stop at WARNING in the P4 configuration, so its
    # tags print W and E lines only either way.
    defaults = (ROOT / "config/sdkconfig.defaults.esp32p4").read_text()
    require("CONFIG_BT_NIMBLE_ENABLED=y" not in defaults and "CONFIG_BT_BLUEDROID_ENABLED=y" in defaults,
            "the P4 runs Bluedroid, not NimBLE")


def source_checks() -> None:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    boot = (COMPONENT / "HardwareOne.cpp").read_text()
    companion = (COMPONENT / "System_RadioCompanion.cpp").read_text()
    radio = (COMPONENT / "HAL_Radio.cpp").read_text()
    records = (COMPONENT / "BLE_CoexRecords.cpp").read_text()

    # The flags: the runtime layers (temp writes them), verbose forces both;
    # the parents are not read.
    require("static bool radioLogWifiDriverOn() { return gDebugVerbose || gDebugSubFlags.wifiDriver; }" in debug,
            "debugwifidriver's runtime layer (or debugverbose)")
    require("static bool radioLogBluetoothCoreOn() { return isDebugFlagSet(DEBUG_BLUETOOTH_CORE); }" in debug,
            "debugbluetoothcore's live bit (isDebugFlagSet includes debugverbose)")
    present = extract_block(debug, "static bool radioLogStackPresent(radiolog::Stack stack)")
    for part in ("return HW1_RADIO_COMPANION;", "CONFIG_BT_BLUEDROID_ENABLED", "return ENABLE_BLE_COEX_RECORDS;",
                 "!CONFIG_IDF_TARGET_ESP32", "&& CONFIG_IDF_TARGET_ESP32"):
        require(part in present, f"stack presence: {part}")

    # The flag commands apply on both paths (temp and saved), after the
    # runtime layer is written; debugverbose too.
    sub = extract_block(debug, "static const char* dbgSubCmd(DbgSubIdx sidx, const String& argsInput)")
    order(sub, "*kDbgSubRuntimePtr[sidx] = (v != 0);", "dbgRecomputeParent(kDbgSubParentIdx[sidx]);",
          "if (sidx == DBG_SUB_WIFI_DRIVER) applyRadioStackLogLevels();", "if (modeTemp) {")
    hook = extract_block(debug, "static void dbgApplyHook(DbgFlagIdx idx, bool modeTemp, int v, bool* settingPtr)")
    bt = hook[hook.index("case DBG_BLUETOOTH_CORE:"):]
    bt = bt[:bt.index("break;")]
    order(bt, "dbgRecomputeParent(DBG_BLUETOOTH);",
          "if (idx == DBG_BLUETOOTH_CORE && !gCliValidateOnly) applyRadioStackLogLevels();")
    flag = extract_block(debug, "static const char* dbgFlagCmd(DbgFlagIdx idx, const String& argsInput)")
    require(flag.count("dbgApplyHook(idx, true, v, settingPtr);") == 1 and
            flag.count("dbgApplyHook(idx, false, v, settingPtr);") == 1, "the hook runs on both paths")
    order(flag, "if (v) setDebugFlag(flagBit); else clearDebugFlag(flagBit);", "dbgApplyHook(idx, true, v, settingPtr);")
    order(extract_block(debug, "const char* cmd_debugverbose(const String& argsInput)"),
          "gDebugVerbose = (v == 1);", "applyRadioStackLogLevels();")
    # The wildcard drops every tag's level: the radio tags are set again.
    overrides = extract_block(debug, "void applyEspLogOverrides(bool includeWildcard)")
    order(overrides, 'esp_log_level_set("*", static_cast<esp_log_level_t>(all));', "applyRadioStackLogLevels();",
          "esplog::forEach(stored,")

    # The apply runs whole under the esplog lock (the loop task applies too).
    order(extract_block(debug, APPLY_FN), "espLogLock();", "const char* stored = espLogStored();",
          "const bool bluetoothCore = radioLogBluetoothCoreOn();", "esp_log_level_set(row.tag,", "espLogUnlock();")
    # The paths that replace the whole live mask run the flag hooks:
    # `log start flags=`, and the saved mask (log start, log autostart,
    # systemlogflags) through systemLogApplyPersistedFlags.
    mask = extract_block(debug, "void applyDebugMaskLogLevels(DebugFlagMask before)")
    order(mask, "applyRadioStackLogLevels();",
          "if (((before ^ gDebugFlags) & DEBUG_HTTPS) != (DebugFlagMask)0) applyHttpsLogLevels(isDebugFlagSet(DEBUG_HTTPS));")
    order(extract_block(debug, "bool systemLogApplyPersistedFlags()"), "const DebugFlagMask before = gDebugFlags;",
          "gDebugFlags = restored;", "applyDebugMaskLogLevels(before);", "return true;")
    start = debug[debug.index("    if (flagsSet) {\n      const DebugFlagMask before"):]
    order(start, "const DebugFlagMask before = gDebugFlags;", "gDebugFlags = debugFlags;",
          "applyDebugMaskLogLevels(before);", "} else {", "systemLogApplyPersistedFlags();")
    whole = re.findall(r"(?m)^\s*gDebugFlags = (\w+);", debug)
    require(sorted(whole) == ["debugFlags", "restored"], f"the whole-mask writers are the two above ({whole})")
    order(extract_block(settings, "static const char* cmd_set_systemlogflags(const String& a)"),
          "if (!commandReplyIsFailure(r)) systemLogApplyPersistedFlags();")
    # The boot's log autostart restores its mask after applySettings' apply.
    order(boot, "applySettings();", "applyEspLogOverrides(true);",
          "gSettings.systemLogAutoStart)) systemLogAutoStart();")
    order(extract_block(debug, "void systemLogAutoStart()"), "systemLogApplyPersistedFlags();")

    # Boot: applySettings applies after the flags are rebuilt and the other
    # owners ran; the boot's esplog apply comes after applySettings.
    apply_settings = extract_block(settings, "void applySettings()")
    order(apply_settings, "DBG_SUBBOOL_LIST(DBG_SUB_MIRROR)", "dbgRecomputeAllParents();",
          "applyLogLevel((uint8_t)lvl);", "applyHttpsLogLevels(gSettings.debugHttps);", "applyRadioStackLogLevels();")
    order(boot, "applySettings();", "applyEspLogOverrides(true);", "radioCompanionBootInit();")

    # Each new Hosted session (esp_hosted_init resets the RPC tags to WARN).
    handler = extract_block(companion, "void handleEvent(const RadioHalEventData& event, uint32_t now)")
    up = handler[handler.index("case RadioHalEvent::TransportUp:"):handler.index("case RadioHalEvent::TransportDown:")]
    order(up, "m.onTransportUp(now);", "applyRadioStackLogLevels();", "break;")
    failure = handler[handler.index("case RadioHalEvent::TransportFailure:"):handler.index("case RadioHalEvent::Heartbeat:")]
    order(failure, "m.onTransportFailure(now);", "applyRadioStackLogLevels();", "break;")
    hosted = (ROOT / ".sdk-overrides/esp32p4/espressif__esp_hosted/host/api/src/esp_hosted_api.c").read_text()
    reset = extract_block(hosted, "static void set_host_modules_log_level(void)")
    require(re.findall(r'esp_log_level_set\("(\w+)", ESP_LOG_WARN\);', reset) == ["rpc_core", "rpc_rsp", "rpc_evt"],
            "esp_hosted_init sets exactly the three RPC tags to WARN (the harness's model)")

    # HAL: quiet before anything of Hosted's runs; the C6 console by level.
    # The mirror runs for every boot (B13, 2026-11-21): started before the
    # first attempt, still after the quieting.
    prepare = extract_block(radio, "esp_err_t radioHalPrepare()")
    order(prepare, "quietHostedLogs();", "prepareBoard()", "radioHalConsoleMirror(true);", "for (int attempt = 1;",
          "radioHalStartTransport();")
    require(prepare.count("radioHalConsoleMirror(") == 1, "radioHalPrepare has one mirror start, for the boot")
    # The C6's boot lines are I lines: C6> shows INFO for the boot's first
    # start (applySettings sets the saved levels after it).
    order(prepare, "quietHostedLogs();", "esp_log_level_set(radiolog::kC6ConsoleTag, "
          "static_cast<esp_log_level_t>(radiolog::kInfo));", "radioHalConsoleMirror(true);")
    require("quietRpcLogs" not in radio and radio.count('"C6>"') == 0, "the C6 tag comes from the table")
    task = extract_block(radio, "void consoleTask(void*)")
    order(task, "radiolog::C6ConsoleLevels levels;", "if (n) logC6Line(levels, line, split);", "split = c != '\\n';")
    require("ESP_LOGI" not in task, "the console task logs only through logC6Line")
    line = extract_block(radio, "void logC6Line(radiolog::C6ConsoleLevels& levels, const char* line, bool continued)")
    for part in ("ESP_LOGE(radiolog::kC6ConsoleTag", "ESP_LOGW(radiolog::kC6ConsoleTag", "ESP_LOGI(radiolog::kC6ConsoleTag"):
        require(part in line, f"C6 line level: {part}")

    # BLE_COEX lines follow their tag (debugbluetoothcore sets it).
    runtime = records.split("#if ENABLE_BLE_COEX_RECORDS\n", 1)[1].split("\n#else\n", 1)[0]
    require("bool linesOn() { return esp_log_level_get(kTag) >= ESP_LOG_INFO; }" in runtime, "the tag's level gates")
    order(extract_block(runtime, "void appendScan(const ScanRecord& record)"), "gLog.addScan(record);",
          "if (linesOn()) {", 'ESP_LOGI(kTag, "scan #%lu %s"')
    order(extract_block(runtime, "void appendConnect(const ConnectRecord& record)"), "gLog.addConnect(record);",
          "if (linesOn()) {", 'ESP_LOGI(kTag, "connect #%lu %s"')
    require("isDebugFlagSet" not in runtime, "no flag read of its own")

    # Help text names what the flags bring back.
    for name in ("debugwifidriver", "debugbluetoothcore"):
        row = re.search(r'\{ "%s", "([^"]+)", true, cmd_%s,\s*"([^}]+)" \},' % (name, name), debug)
        require(row is not None, f"the {name} row")
        require("errors" in row[1] and "esplog" in row[2] and "C6>" in row[2], f"{name} help names the tags")
    print("radio stack log wiring, callers, C6 line levels, BLE_COEX gate, help text, tag sources: OK")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    run_harness(args.cxx, args.sanitize)
    source_checks()
    tag_sources()


if __name__ == "__main__":
    main()
