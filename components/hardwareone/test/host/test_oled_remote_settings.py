#!/usr/bin/env python3
"""Execute the production OLED bonded-peer settings editor against host doubles.

Compiles OLED_RemoteSettings.cpp whole (schema-driven module builder,
non-blocking schema fetch, remote apply and peer-reply outcome), the settings
editor's module source / commit routing (OLED_SettingsEditor.cpp), the
remote-settings mode adapter (OLED_Mode_RemoteSettings.cpp), the ESP-NOW
command-result record (System_ESPNow) and BondedPeer's schema sync and
remote-edit policy. The OLED command executor, the peer caches, the clock,
PSRAM allocation and the display are doubles.

The cases check that the remote screen lists only the peer's schema rows that
carry a setter (cmdKey), sends `remote:<cmdKey> <value>`, reports the peer's
CMD_RESP failure or silence instead of the local "sent" result, never runs a
local setter, fetches a missing schema once without blocking, and releases
everything it allocated; on the local screen a refused slider/pick-list (INT)
save shows the executor's reply on the editor line like the STRING path. A13:
System_Utils' remote: reply owner (formatRemoteCommandSent /
parseRemoteCommandSentReqId) is spliced in; the executor double answers with it,
and the commit waits for the CMD_RESP whose reqId is that request's msgId, so the
bonded peer's reply to another remote: command does not decide it. A
static check confirms the CMD_RESP drain publishes the record. `--component
<dir>` runs the same cases against saved sources (the base `remote:set
<jsonKey>` editor fails them). The radio link and the real display are not
exercised.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import JSON_INCLUDE, extract_block, require  # noqa: E402


def strip_includes(text: str) -> str:
    return re.sub(r"(?m)^#include.*\n", "", text)


def static_line(source: str, pattern: str) -> str:
    match = re.search(r"(?m)^" + pattern + r"[^\n]*;[^\n]*$", source)
    require(match is not None, f"missing file-scope declaration: {pattern}")
    return match[0]


def settings_types(read) -> str:
    header = read("System_Settings.h")
    entry = header[header.index("struct SettingEntry {"):]
    limit = re.search(r"(?m)^#define MAX_SETTINGS_MODULES\b.*$", header)
    require(limit is not None, "System_Settings.h: MAX_SETTINGS_MODULES not found")
    return "\n".join([
        "typedef bool (*ConnectionCheckFunc)();",
        extract_block(header, "enum SettingType") + ";",
        extract_block(entry, "struct SettingEntry") + ";",
        extract_block(header, "struct SettingsModule") + ";",
        limit[0],
    ])


def editor_types(read) -> str:
    header = read("OLED_SettingsEditor.h")
    return "\n".join(extract_block(header, marker) + ";" for marker in (
        "enum SettingsEditorState", "enum SettingsEditKind", "struct SettingsEditorContext"))


ESPNOW_STANDIN = """
// Stand-in: this tree has no command-result record (the drain publishes nothing).
struct EspNowCmdResult {
  static constexpr size_t TEXT_MAX = 96;
  uint32_t seq; uint32_t reqId; uint32_t receivedMs; bool success; uint8_t srcMac[6]; char text[TEXT_MAX];
};
static void espnowPublishCmdResult(const uint8_t*, uint32_t, bool, const char*) {}
uint32_t espnowCmdResultSeq() { return 0; }
bool espnowCmdResultGet(EspNowCmdResult& out) { out = EspNowCmdResult(); return false; }
"""
ESPNOW_MARKERS = ("static void espnowPublishCmdResult(", "uint32_t espnowCmdResultSeq(",
                  "bool espnowCmdResultGet(")


def espnow_record(read) -> str:
    header, source = read("System_ESPNow.h"), read("System_ESPNow.cpp")
    if "struct EspNowCmdResult" not in header or any(m not in source for m in ESPNOW_MARKERS):
        return ESPNOW_STANDIN
    return "\n".join([
        extract_block(header, "struct EspNowCmdResult") + ";",
        static_line(source, r"(?:EXT_RAM_BSS_ATTR )?static EspNowCmdResult gCmdResult\b"),
        static_line(source, r"static portMUX_TYPE gCmdResultLock\b"),
        *(extract_block(source, marker) for marker in ESPNOW_MARKERS),
    ])


BONDED_STANDINS = {
    "bool beginSchemaSync(": "bool beginSchemaSync() { clearError(); return true; }  // stand-in",
    "bool schemaSyncDone(": "bool schemaSyncDone() { return gEspNow->bondSchemaReceived; }  // stand-in",
    "bool remoteEditAllowed(": "bool remoteEditAllowed(const char*, const char*) { return true; }  // stand-in",
}


def bonded(read) -> str:
    source = read("System_BondedPeer.cpp")
    return "\n".join(extract_block(source, marker) if marker in source else standin
                     for marker, standin in BONDED_STANDINS.items())


BASE_API_SHIM = """
// The saved tree's API: applyRemoteSettingChange(module, jsonKey, value) sends at
// once and has no load step or outcome. These adapters let the same cases run.
enum RemoteSettingsLoadState { REMOTE_LOAD_IDLE, REMOTE_LOAD_SYNCING, REMOTE_LOAD_READY, REMOTE_LOAD_FAILED };
enum RemoteSettingsOutcome { REMOTE_OUTCOME_NONE, REMOTE_OUTCOME_PENDING, REMOTE_OUTCOME_APPLIED,
                             REMOTE_OUTCOME_FAILED, REMOTE_OUTCOME_TIMEOUT };
bool applyRemoteSettingChange(const SettingEntry* entry, const String& value, String& errOut) {
  errOut = "";
  return entry && applyRemoteSettingChange("module", entry->jsonKey, value);
}
RemoteSettingsOutcome remoteSettingsPollOutcome(String& textOut) { textOut = ""; return REMOTE_OUTCOME_NONE; }
RemoteSettingsLoadState remoteSettingsLoadStep() {
  return loadRemoteSettingsModules() ? REMOTE_LOAD_READY : REMOTE_LOAD_FAILED;
}
const char* remoteSettingsLastError() { return ""; }
"""


REPLY_FUNCTIONS = ("void formatRemoteCommandSent(", "bool parseRemoteCommandSentReqId(")


def remote_reply(read) -> str:
    """System_Utils' remote: reply owner (A13), or nothing on a tree without it."""
    utils = read("System_Utils.cpp")
    if not all(marker in utils for marker in REPLY_FUNCTIONS):
        return "// this tree's remote: reply has no request id"
    constants = re.findall(r'(?m)^static constexpr char kRemoteSent\w+\[\] = "[^"\n]*";$', utils)
    require(len(constants) == 2, "System_Utils.cpp: the remote: reply constants changed")
    return "\n".join([*constants, *(extract_block(utils, marker) for marker in REPLY_FUNCTIONS),
                      "#define HW1_REMOTE_REPLY_OWNER 1"])


def remote(read) -> str:
    header = strip_includes(read("OLED_RemoteSettings.h"))
    source = strip_includes(read("OLED_RemoteSettings.cpp"))
    base_api = "bool applyRemoteSettingChange(const char* moduleName" in source
    return "\n".join([header, source, BASE_API_SHIM if base_api else ""])


# (marker, stand-in for a tree without it; None = required on every tree)
EDITOR_FUNCTIONS = (
    ("void initSettingsEditor(", None),
    ("void settingsEditorShowError(",
     "void settingsEditorShowError(const String& m, uint32_t d) {\n"
     "  gSettingsEditor.errorMessage = m; gSettingsEditor.errorDisplayUntil = millis() + d;\n}"),
    ("static bool editorIsRemote(", "static bool editorIsRemote() { return false; }  // stand-in"),
    ("static const SettingsModule** editorModules(",
     "static const SettingsModule** editorModules(size_t& n) { return getSettingsModules(n); }  // stand-in"),
    ("static bool commitRemoteSettingValue(", ""),
    ("static void editorDropForeignModule(", "static void editorDropForeignModule() {}  // stand-in"),
    # The INT twin calls the STRING commit, so the STRING commit comes first.
    # setSettingValue returns bool (void on older trees).
    ("static bool setSettingValueStr(", None),
    (("bool setSettingValue(", "void setSettingValue("), None),
)


def editor(read) -> str:
    source = read("OLED_SettingsEditor.cpp")
    parts = []
    for markers, standin in EDITOR_FUNCTIONS:
        if isinstance(markers, str):
            markers = (markers,)
        marker = next((m for m in markers if m in source), markers[0])
        if marker in source:
            require(source.count(marker) == 1, f"OLED_SettingsEditor.cpp: marker not unique: {marker}")
            parts.append(extract_block(source, marker))
        else:
            require(standin is not None, f"OLED_SettingsEditor.cpp: missing {marker}")
            parts.append(standin)
    return "\n\n".join(parts)


ADAPTER_STANDIN = """
// Stand-ins: this tree's adapter has no entry hook and edits through its own
// input path, so the adapter cases have nothing to drive.
[[maybe_unused]] static bool gRemoteSettingsActive = false;
static void onEnterRemoteSettings(bool) {}
static void displayRemoteSettingsMode() {}
static bool handleRemoteSettingsInput(int, int, uint32_t) { return true; }
"""
ADAPTER_MARKERS = ("static void onEnterRemoteSettings(", "static void displayRemoteSettingsMode(",
                   "static bool handleRemoteSettingsInput(")


def adapter(read) -> str:
    source = read("OLED_Mode_RemoteSettings.cpp")
    if ADAPTER_MARKERS[0] not in source:
        return ADAPTER_STANDIN
    return "\n\n".join([static_line(source, r"static bool gRemoteSettingsActive\b"),
                        *(extract_block(source, marker) for marker in ADAPTER_MARKERS)])


def generated(read) -> str:
    harness = (HERE / "oled_remote_settings_harness.cpp").read_text()
    for marker, body in (("// INSERT_SETTINGS_TYPES", settings_types(read)),
                         ("// INSERT_ESPNOW", espnow_record(read)),
                         ("// INSERT_BONDED", bonded(read)),
                         ("// INSERT_REMOTE_REPLY", remote_reply(read)),
                         ("// INSERT_EDITOR_TYPES", editor_types(read)),
                         ("// INSERT_REMOTE", remote(read)),
                         ("// INSERT_EDITOR", editor(read)),
                         ("// INSERT_ADAPTER", adapter(read))):
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)
    return harness


def static_checks(read) -> list[str]:
    failures = []
    source = read("System_ESPNow.cpp")
    start = source.index("// 8. Process deferred CMD_RESP")
    drain = source[start:source.index("#if ENABLE_BONDED_MODE", start)]
    call = drain.find("espnowPublishCmdResult(")
    if call < 0:
        failures.append("the CMD_RESP drain does not publish espnowPublishCmdResult(...): "
                        "no reader can see the peer's answer")
    elif "safeDeferredResult.c_str()" not in drain[call:drain.find(";", call)]:
        failures.append("the CMD_RESP drain publishes something other than the redacted result")
    bonded_source = read("System_BondedPeer.cpp")
    if "bool beginSchemaSync(" in bonded_source and \
            "beginSchemaSync()" not in extract_block(bonded_source, "bool requestSchemaSync("):
        failures.append("requestSchemaSync() does not start through beginSchemaSync()")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (point at a saved tree for a base run)")
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-remote-settings-") as directory:
        work = Path(directory)
        (work / "Arduino.h").write_text("// ArduinoJson includes <Arduino.h>; String is the harness's.\n")
        unit, executable = work / "test.cpp", work / "test"
        unit.write_text(generated(read))
        subprocess.run([args.cxx, *flags, "-I", str(work), "-I", str(JSON_INCLUDE),
                        str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    failures = static_checks(read)
    for failure in failures:
        print(f"FAIL [static] {failure}", file=sys.stderr)
    if run.returncode != 0 or failures:
        raise SystemExit("oled_remote_settings: FAILED")
    print("oled_remote_settings: schema setters, peer outcome, no local write, schema fetch, cleanup")


if __name__ == "__main__":
    main()
