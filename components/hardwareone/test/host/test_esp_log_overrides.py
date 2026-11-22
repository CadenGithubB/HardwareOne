#!/usr/bin/env python3
"""ESP-IDF log levels by tag (esplog), saved, and the field-log quieting.

Executes the shipping System_EspLogCore.h and the apply/command block of
System_Debug.cpp (esp_log_level_set and persistence recorded), then pins the
wiring around them in source:

* the setting row (debug module, cmdKey esplog) and the command row (admin);
* the boot applies the overrides, wildcard included, after the firmware's own
  esp_log_level_set callers and before the radios; the two log-level owners
  and each companion TRANSPORT_UP (Hosted's init resets its RPC tags)
  re-apply the tag entries after setting their tags;
* the saved string is read null-safe and saved only after its room is
  reserved; the LLM worker only lowers HTTP_CLIENT; the OLED and web editors
  send an emptied field as `esplog clear`;
* the rest of the 2026-11-18 field log: heartbeats ask the AP only while
  joined, the BLE_COEX lines print only while their tag is at info, the G2
  scans skip a stop after a scan that ended itself, rpc_utils is lowered with
  the other Hosted RPC tags, and `c6console` persists.
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
START = "// ---- esplog: ESP-IDF log levels by tag, saved"
COMMAND = "const char* cmd_esplog(const String& argsInput)"


def generate() -> str:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    require(debug.count(START) == 1 and debug.count(COMMAND) == 1, "one esplog block in System_Debug.cpp")
    start = debug.index(START)
    command = extract_block(debug, COMMAND)
    end = debug.index(command) + len(command)
    production = debug[start:end]
    harness = (HERE / "esp_log_overrides_harness.cpp").read_text()
    for marker, value in (("// INSERT_PRODUCTION", production),):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def string_double() -> str:
    """The shared host String plus a reserve() the harness can fail, as the
    ESP String's does when its allocation fails."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1 and stub.count("class String {") == 1, "the shared String double's shape")
    extra = "  bool reserve(size_t size) { if (gHostStringReserveFails) return false; text_.reserve(size); return true; }\n"
    stub = stub.replace(anchor, extra + anchor)
    return stub.replace("class String {", "inline bool gHostStringReserveFails = false;\n\nclass String {")


def run_harness(cxx: str, sanitize: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="hw1-esplog-") as temporary:
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
        require(result.returncode == 0, f"esplog harness failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


def order(text: str, *parts: str) -> None:
    at = -1
    for part in parts:
        found = text.find(part, at + 1)
        require(found > at, f"{part!r} not after the previous part")
        at = found


def source_checks() -> None:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    settings_h = (COMPONENT / "System_Settings.h").read_text()
    boot = (COMPONENT / "HardwareOne.cpp").read_text()

    # Setting and command rows.
    require(re.search(r'\{ "espLogLevels",\s+SETTING_STRING, &gSettings\.espLogLevels,.*"esplog" \},', settings),
            "espLogLevels is a debug-module string row bound to esplog")
    require("kDbgRegHandRows = 4;" in settings, "the debug registry counts the esplog hand row")
    require("String espLogLevels;" in settings_h and 'espLogLevels(""),' in settings_h, "the field and its default")
    require(re.search(r'\{ "esplog", "[^"]+", true, cmd_esplog,', debug), "esplog is an admin debug command")

    # The owners re-apply the tag entries (not the wildcard) after their tags.
    order(extract_block(debug, "void applyLogLevel(uint8_t level)"),
          'esp_log_level_set("phy_init", espLevel);', "applyEspLogOverrides(false);")
    order(extract_block(debug, "void applyHttpsLogLevels(bool verbose)"),
          'esp_log_level_set("httpd_txrx", lvl);', "applyEspLogOverrides(false);")
    # Boot: after the I2C tag (hardware phase) and applySettings, before the
    # radio companion and Wi-Fi; HAL_Radio's RPC tags are set before setup().
    order(boot, "applySettings();", 'esp_log_level_set("i2c.master", ESP_LOG_WARN);',
          "crashRecordSetPhase(CRASH_PHASE_NETWORK);", "applyEspLogOverrides(true);", "radioCompanionBootInit();")
    require(boot.count("applyEspLogOverrides(") == 1, "one boot apply")
    # Every fresh Hosted session resets rpc_core/rpc_rsp/rpc_evt to WARN in
    # esp_hosted_init(); the companion re-applies the saved tags on TRANSPORT_UP.
    companion_source = (COMPONENT / "System_RadioCompanion.cpp").read_text()
    handler = extract_block(companion_source, "void handleEvent(const RadioHalEventData& event, uint32_t now)")
    up = handler[handler.index("case RadioHalEvent::TransportUp:"):handler.index("case RadioHalEvent::TransportDown:")]
    order(up, "m.onTransportUp(now);", "applyRadioStackLogLevels();", "break;")

    # A failed String allocation leaves a null buffer: the stored value is read
    # through the null-safe accessor and saved only after the room is reserved.
    block = debug[debug.index(START):]
    block = block[:block.index(extract_block(debug, COMMAND)) + len(extract_block(debug, COMMAND))]
    require(block.count("gSettings.espLogLevels.c_str()") == 1 and "return stored ? stored : \"\";" in block,
            "espLogLevels is read only through espLogStored()")
    # The reserve and the assignment run under the esplog lock (the loop task
    # applies the radio tags from the same string); debug.json is written after.
    save = extract_block(debug, "static bool espLogSave(const char* next)")
    order(save, "if (!value.c_str()) return false;", "espLogLock();",
          "const bool saved = !changed || gSettings.espLogLevels.reserve(value.length());",
          "if (changed && saved) gSettings.espLogLevels = value;", "espLogUnlock();",
          "if (changed && saved) (void)requestDebugSettingsPersist();", "return saved;")
    require(block.count("gSettings.espLogLevels = ") == 1 and "setDebugSetting(gSettings.espLogLevels" not in debug,
            "every esplog save goes through espLogSave")
    lock = debug[debug.index("// ---- esplog: the saved string's lock"):debug.index(START)]
    order(lock, "static StaticSemaphore_t gEspLogMutexStorage;", "static void espLogLock() {",
          "xSemaphoreCreateMutexStatic(&gEspLogMutexStorage);", "xSemaphoreTake(gEspLogMutex, portMAX_DELAY);",
          "static void espLogUnlock() { xSemaphoreGive(gEspLogMutex); }")
    require(debug.count("espLogLock();") == 2 and debug.count("espLogUnlock();") == 2,
            "the lock is taken only by the save and the radio apply")
    require(block.count("if (!espLogSave(") == 4, "each esplog save checks its result")

    # The LLM worker only lowers HTTP_CLIENT, so a quieter saved level stays.
    llm = (COMPONENT / "System_LLMHttp.cpp").read_text()
    require('if (esp_log_level_get("HTTP_CLIENT") > ESP_LOG_WARN) esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);'
            in llm and llm.count('esp_log_level_set("HTTP_CLIENT"') == 1, "the LLM worker only lowers HTTP_CLIENT")

    # Settings editors: an emptied esplog field is 'clear' (the bare command
    # only lists); the web debug page shows the string setting as a text field.
    oled = (COMPONENT / "OLED_SettingsEditor.cpp").read_text()
    require('if (!strcasecmp(cmdName, "esplog") && !arg[strspn(arg, " \\t\\r\\n")]) arg = "clear";' in oled,
            "the OLED editor sends an emptied esplog field as clear")
    web = (COMPONENT / "WebPage_Settings.h").read_text()
    require("} else if(e.type==='string'){" in web, "the debug page renders string settings as text fields")
    require("if(el.type==='text'&&!val.trim())val='clear';" in web, "the web page sends an emptied esplog field as clear")

    # rpc_utils is one of the Hosted tags held quiet (System_RadioLogCore.h,
    # run by test_radio_stack_logs); its only line is a WARN.
    core = (COMPONENT / "System_RadioLogCore.h").read_text()
    require('{"rpc_utils", Family::WifiDriver, Stack::Hosted, kInfo},' in core, "rpc_utils is a quiet Hosted tag")
    hosted = COMPONENT.parents[1] / ".sdk-overrides/esp32p4/espressif__esp_hosted/host/drivers/rpc/core/rpc_utils.c"
    if hosted.exists():
        logs = re.findall(r"ESP_LOG[EWIDV]\(", hosted.read_text())
        require(logs == ["ESP_LOGW("], f"rpc_utils.c logs only its one WARN line (found {logs})")

    # Heartbeats ask the AP only while joined: no direct get_ap_info elsewhere.
    espnow = (COMPONENT / "System_ESPNow.cpp").read_text()
    rssi = extract_block(espnow, "static int8_t heartbeatStaRssi()")
    order(rssi, "WiFi.status() != WL_CONNECTED", "return (int8_t)-127;", "esp_wifi_sta_get_ap_info(&ap)")
    require(espnow.count("esp_wifi_sta_get_ap_info(") == 1, "System_ESPNow.cpp asks the AP in one place")
    require("hb.rssi      = heartbeatStaRssi();" in extract_block(espnow, "static void v4FillHeartbeat("),
            "the heartbeat builder uses the joined-only lookup")
    require("phb.rssi = heartbeatStaRssi();" in espnow, "the bond heartbeat uses the joined-only lookup")
    snap = settings[settings.index("// wifiPrimarySSID: runtime-only"):]
    order(snap, "if (WiFi.isConnected()) {", "String cur = WiFi.SSID();")

    # BLE_COEX lines only while the tag is at info (debugbluetoothcore); the record is kept.
    records = (COMPONENT / "BLE_CoexRecords.cpp").read_text()
    runtime = records.split("#if ENABLE_BLE_COEX_RECORDS\n", 1)[1].split("\n#else\n", 1)[0]
    scan = extract_block(runtime, "void appendScan(const ScanRecord& record)")
    order(scan, "gLog.addScan(record);", "if (linesOn()) {", 'ESP_LOGI(kTag, "scan #%lu %s"',
          'ESP_LOGI(kTag, "scan #%lu%s"', "unlockLog();")

    # G2: no stop after a scan that ended itself (or was stopped early).
    g2 = (COMPONENT / "G2_Glasses.cpp").read_text()
    stop = extract_block(g2, "static void g2ScanStopIfActive()")
    require("if (!gG2ScanStoppedEarly && !gG2ScanEnded) gScan->stop();" in stop, "the stop skips an ended scan")
    require("static void g2ScanEndedCallback(BLEScanResults) { gG2ScanEnded = true; }" in g2, "the end callback")
    starts = re.findall(r"gScan->start\([^;]*\);", g2)
    require(len(starts) == 2 and all("g2ScanEndedCallback" in s for s in starts),
            f"both G2 scans note their own end ({starts})")
    require(g2.count("gG2ScanEnded = false;") == 2, "each G2 scan clears the end flag before its start")
    for marker in ("gScan->start(kScanSec, g2ScanEndedCallback, false);", "gScan->start(scanSec, g2ScanEndedCallback, false);"):
        body = g2[:g2.index(marker)]
        clear = body.rfind("gG2ScanEnded = false;")
        # The clear sits after any earlier start and within this start's setup
        # (no function boundary between them).
        require(clear >= 0 and clear > body.rfind("gScan->start(") and "\n}\n" not in body[clear:],
                f"cleared before {marker}")

    # c6console persists: the command saves. The mirror is off by default
    # (UP-14, 2026-11-20): the boot starts and stops none itself and hands the
    # decision to the fault service, which follows the saved setting
    # (test_companion_fault_mirror.py runs it).
    companion = (COMPONENT / "System_RadioCompanion.cpp").read_text()
    require(re.search(r'\{ "c6ConsoleMirror", SETTING_BOOL, &gSettings\.c6ConsoleMirror, 0,.*"c6console" \},', companion),
            "c6ConsoleMirror is a companion row bound to c6console, default off")
    order(extract_block(companion, "const char* cmd_c6console(const String& argsInput)"),
          "radioHalConsoleMirror(on);", "if (err != ESP_OK) {", "setSetting(gSettings.c6ConsoleMirror, on);")
    boot = extract_block(companion, "void radioCompanionBootInit()")
    order(boot, "applyPolicyFromSettings();", "m.onBootResult(", "serviceFaults(")
    require("radioHalConsoleMirror(" not in re.sub(r"//[^\n]*", "", boot),
            "boot init neither starts nor stops the mirror itself")
    service = extract_block(companion, "void serviceFaults(uint32_t now, bool quiet)")
    require("sMirror.tick(now, state, gSettings.c6ConsoleMirror || bootDebugInProgress(), running)" in service,
            "the fault service follows the saved setting (and the boot window, B13)")
    require("c6ConsoleMirror(false)," in settings_h, "the mirror is off by default")
    print("esplog wiring, joined-only AP lookups, BLE_COEX scan lines, G2 scan stop, rpc_utils, c6console: OK")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    run_harness(args.cxx, args.sanitize)
    source_checks()


if __name__ == "__main__":
    main()
