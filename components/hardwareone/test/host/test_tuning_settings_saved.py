#!/usr/bin/env python3
"""ESP-SR tuning and the G2 stream size are saved settings.

`srtuninggain`, `srtuningagc`, `srtuningvad` and `g2streamres` set fields that
no settings table named, so the values were back at their defaults after a
reboot. Each field now has a row. This test checks, on the production tables
and the production load/save code (System_Settings.cpp), that

- every `setSetting(gSettings.X, ...)` target in System_ESPSR.cpp and
  G2_Glasses.cpp is named by a row of some settings table;
- the five new rows' defaults equal the Settings constructor's, because
  applyRegisteredDefaults() writes the row default over the constructor's;
- the values survive a save and a load, 0.5 stays 0.5 (the gain row's lower
  bound is 0, not 1), and out-of-range file values are clamped;
- auto-tune's trial gain is never what a save writes;
- the two stream-size rows are display-only (no generic editor offers a row
  whose command takes "WxH" as two number fields).

Compiled: the real tables, jsonPathCreate/Read, the load type gate and clamps,
readRegisteredSettings, writeRegisteredSettings, applyRegisteredDefaults and
srEffectiveAfeGain, with the real vendored ArduinoJson.
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

NEW_ROWS = {  # field -> (file, table, json key, constructor default as C++ text)
    "srAfeGain":      ("System_ESPSR.cpp", "espsrSettingsEntries", "srAfeGain"),
    "srAgcMode":      ("System_ESPSR.cpp", "espsrSettingsEntries", "srAgcMode"),
    "srVadMode":      ("System_ESPSR.cpp", "espsrSettingsEntries", "srVadMode"),
    "g2StreamWidth":  ("G2_Glasses.cpp", "g2DeviceSettingEntries", "streamWidth"),
    "g2StreamHeight": ("G2_Glasses.cpp", "g2DeviceSettingEntries", "streamHeight"),
}

HARNESS = r'''
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Arduino.h"
#include <ArduinoJson.h>
#define EXT_RAM_BSS_ATTR
#define ENABLE_LLM_BACKEND 0
void webBatchTestStringAppend() {}

static std::vector<std::string> logs;
static void logSystemEvent(const char*, const char* format, ...) {
  char out[512]; va_list arguments; va_start(arguments, format);
  vsnprintf(out, sizeof(out), format, arguments); va_end(arguments);
  logs.emplace_back(out);
}
#define ERROR_STORAGEF(...) logSystemEvent("STORAGE", __VA_ARGS__)
constexpr int SYSEVT_SECRET_DECRYPT_FAILED = 1;
static void systemEventPost(int, const char*, const char*) {}
static unsigned gSecretLoadFailures;
static String decryptString(const char* text, bool) { return String(text); }
static bool isDeviceCipherText(const char*) { return false; }
static void secureClearString(String&) {}
static void putSecret(JsonObject object, const char* key, const String& value) { object[key] = value.c_str(); }

using ConnectionCheckFunc = bool (*)();
// INSERT_TYPES
// INSERT_SETTINGS
static bool isEspSrConnected() { return true; }
// INSERT_TABLES
static const SettingsModule* gSettingsModules[] = { &espsrSettingsModule, &g2DeviceSettingsModule };
static const size_t gSettingsModuleCount = 2;
// INSERT_PRODUCTION

static bool gSrAutoTuneActive = false;
// INSERT_AUTOTUNE

static const SettingEntry* row(const char* key) {
  for (size_t m = 0; m < gSettingsModuleCount; ++m) {
    for (size_t i = 0; i < gSettingsModules[m]->count; ++i) {
      if (std::strcmp(gSettingsModules[m]->entries[i].jsonKey, key) == 0) return &gSettingsModules[m]->entries[i];
    }
  }
  return nullptr;
}
static void scramble() {
  gSettings.srAfeGain = -77.0f; gSettings.srAgcMode = 99; gSettings.srVadMode = 99;
  gSettings.g2StreamWidth = 7; gSettings.g2StreamHeight = 7;
}
static void load(const char* json) {
  JsonDocument doc;
  assert(!deserializeJson(doc, json));
  readRegisteredSettings(doc, nullptr, false);
}

int main() {
  // Row defaults are what the firmware runs with when the file has no key:
  // they must be the constructor's (CTOR_* come from System_Settings.h).
  scramble();
  applyRegisteredDefaults();
  assert(gSettings.srAfeGain == CTOR_srAfeGain && gSettings.srAgcMode == CTOR_srAgcMode);
  assert(gSettings.srVadMode == CTOR_srVadMode);
  assert(gSettings.g2StreamWidth == CTOR_g2StreamWidth && gSettings.g2StreamHeight == CTOR_g2StreamHeight);
  assert(gSettings.srAfeGain == 1.0f && gSettings.srAgcMode == 2 && gSettings.srVadMode == 3);
  assert(gSettings.g2StreamWidth == 96 && gSettings.g2StreamHeight == 96);

  // An already-set-up device: its file has none of the keys. Values stay at
  // the defaults, and nothing is logged.
  load(R"({"apps":{"espsr":{"srEnabled":true,"srCommandTimeout":6000}},"network":{"bluetooth":{"g2":{"headUpDesired":0}}}})");
  assert(gSettings.srAfeGain == 1.0f && gSettings.srAgcMode == 2 && gSettings.srVadMode == 3);
  assert(gSettings.g2StreamWidth == 96 && gSettings.g2StreamHeight == 96 && logs.empty());

  // Save, then load into a fresh state: every value comes back.
  gSettings.srAfeGain = 2.5f; gSettings.srAgcMode = 1; gSettings.srVadMode = 4;
  gSettings.g2StreamWidth = 160; gSettings.g2StreamHeight = 120;
  JsonDocument disk;
  disk.to<JsonObject>();
  writeRegisteredSettings(disk, nullptr, false);
  assert(disk["apps"]["espsr"]["srAfeGain"].as<float>() == 2.5f);
  assert(disk["apps"]["espsr"]["srAgcMode"] == 1 && disk["apps"]["espsr"]["srVadMode"] == 4);
  assert(disk["network"]["bluetooth"]["g2"]["streamWidth"] == 160);
  assert(disk["network"]["bluetooth"]["g2"]["streamHeight"] == 120);
  std::string text;
  serializeJson(disk, text);
  applyRegisteredDefaults();
  assert(gSettings.srAfeGain == 1.0f && gSettings.g2StreamWidth == 96);
  load(text.c_str());
  assert(gSettings.srAfeGain == 2.5f && gSettings.srAgcMode == 1 && gSettings.srVadMode == 4);
  assert(gSettings.g2StreamWidth == 160 && gSettings.g2StreamHeight == 120 && logs.empty());

  // The command allows 0.1..10.0. A saved gain below 1 must not be pushed up
  // to 1 at load (the row's bounds are whole numbers: 0..10).
  for (float gain : {0.1f, 0.5f, 0.99f, 1.0f, 9.9f, 10.0f}) {
    char json[96]; std::snprintf(json, sizeof(json), R"({"apps":{"espsr":{"srAfeGain":%.2f}}})", gain);
    applyRegisteredDefaults(); load(json);
    assert(std::fabs(gSettings.srAfeGain - gain) < 0.0001f && logs.empty());
  }
  // The serializer drops trailing zeros: a saved 2.0 is read back from "2".
  applyRegisteredDefaults(); load(R"({"apps":{"espsr":{"srAfeGain":2}}})");
  assert(gSettings.srAfeGain == 2.0f && logs.empty());
  // Hand-edited values outside the range are clamped and logged.
  applyRegisteredDefaults();
  load(R"({"apps":{"espsr":{"srAfeGain":12.5,"srAgcMode":7,"srVadMode":-1}},"network":{"bluetooth":{"g2":{"streamWidth":500,"streamHeight":2}}}})");
  assert(gSettings.srAfeGain == 10.0f && gSettings.srAgcMode == 3 && gSettings.srVadMode == 0);
  assert(gSettings.g2StreamWidth == 288 && gSettings.g2StreamHeight == 16 && logs.size() == 5);
  logs.clear();
  applyRegisteredDefaults(); load(R"({"apps":{"espsr":{"srAfeGain":-3}}})");
  assert(gSettings.srAfeGain == 0.0f);  // The AFE setup clamps to 0.1 where it is used.
  logs.clear();
  // A wrong type keeps the default.
  applyRegisteredDefaults(); load(R"({"apps":{"espsr":{"srAfeGain":"loud","srAgcMode":"x"}}})");
  assert(gSettings.srAfeGain == 1.0f && gSettings.srAgcMode == 2);
  logs.clear();

  // Auto-tune tries gains without touching the saved one: a save taken while
  // it runs writes the saved gain, and the trial is what the AFE would use.
  applyRegisteredDefaults();
  gSettings.srAfeGain = 2.5f;
  assert(srEffectiveAfeGain() == 2.5f);
  gSrAutoTuneActive = true; gSrAfeGainTrial = 4.0f;
  assert(srEffectiveAfeGain() == 4.0f && gSettings.srAfeGain == 2.5f);
  JsonDocument during;
  during.to<JsonObject>();
  writeRegisteredSettings(during, nullptr, false);
  assert(during["apps"]["espsr"]["srAfeGain"].as<float>() == 2.5f);
  gSrAutoTuneActive = false;                       // Finished or stopped: the saved gain again,
  assert(srEffectiveAfeGain() == 2.5f);            // even if the trial were left behind.
  gSrAutoTuneActive = true; gSrAfeGainTrial = 0.0f;  // No trial set yet.
  assert(srEffectiveAfeGain() == 2.5f);

  // Editors: the three ESP-SR rows are editable through their own commands;
  // the two stream rows are display-only and name no command.
  for (const char* key : {"srAfeGain", "srAgcMode", "srVadMode"}) {
    const SettingEntry* e = row(key);
    assert(e && !e->readOnly && !e->isSecret && e->cmdKey && e->label);
  }
  assert(std::strcmp(row("srAfeGain")->cmdKey, "srtuninggain") == 0 && row("srAfeGain")->type == SETTING_FLOAT);
  assert(row("srAfeGain")->minVal == 0 && row("srAfeGain")->maxVal == 10);
  assert(std::strcmp(row("srAgcMode")->cmdKey, "srtuningagc") == 0 && row("srAgcMode")->maxVal == 3);
  assert(std::strcmp(row("srVadMode")->cmdKey, "srtuningvad") == 0 && row("srVadMode")->maxVal == 4);
  for (const char* key : {"streamWidth", "streamHeight"}) {
    const SettingEntry* e = row(key);
    assert(e && e->readOnly && e->cmdKey == nullptr && e->type == SETTING_INT && e->minVal == 16);
  }
  assert(row("streamWidth")->maxVal == 288 && row("streamHeight")->maxVal == 144);
  std::puts("Tuning settings passed (rows, defaults, save/load round trip, clamps, auto-tune trial kept out of the save)");
}
'''


def setting_targets(text: str) -> set[str]:
    return set(re.findall(r"\bsetSetting\(\s*gSettings\.(\w+)", text))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    espsr = (COMPONENT / "System_ESPSR.cpp").read_text()
    glasses = (COMPONENT / "G2_Glasses.cpp").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    settings_header = (COMPONENT / "System_Settings.h").read_text()

    # 1. Every setSetting target in the two files has a settings row somewhere.
    registered = set()
    for path in sorted(COMPONENT.glob("*.cpp")):
        text = path.read_text()
        for table in re.finditer(r"\bconst SettingEntry \w+\[\]\s*=\s*\{", text):
            body = extract_block(text[table.start():], "const SettingEntry")
            registered |= set(re.findall(r"&gSettings\.(\w+)", body))
    for name, text in (("System_ESPSR.cpp", espsr), ("G2_Glasses.cpp", glasses)):
        targets = setting_targets(text)
        require(bool(targets), f"{name}: found no setSetting(gSettings.X targets; the pattern is stale")
        missing = sorted(targets - registered)
        require(not missing, f"{name}: setSetting writes fields no settings table names "
                             f"(they would not be saved): {', '.join(missing)}")
    for field, (name, table, _) in NEW_ROWS.items():
        text = espsr if name == "System_ESPSR.cpp" else glasses
        body = extract_block(text, f"static const SettingEntry {table}[]")
        require(f"&gSettings.{field}" in body, f"{name}: {table} lost its {field} row")

    # 2. No raw write to the saved gain: auto-tune uses its own trial value.
    code = "\n".join(re.sub(r"//.*", "", line) for line in espsr.split("\n"))
    raw = re.findall(r"gSettings\.(srAfeGain|srAgcMode|srVadMode)\s*=[^=]", code)
    require(not raw, f"System_ESPSR.cpp writes a saved tuning field without setSetting: {raw}")
    raw = re.findall(r"gSettings\.(g2StreamWidth|g2StreamHeight)\s*=[^=]",
                     "\n".join(re.sub(r"//.*", "", line) for line in glasses.split("\n")))
    require(not raw, f"G2_Glasses.cpp writes a stream size field without setSetting: {raw}")
    require("clampFloat(srEffectiveAfeGain(), 0.1f, 10.0f)" in code,
            "the AFE setup must take its gain from srEffectiveAfeGain(), clamped to 0.1..10")
    require(len(re.findall(r"gSrAfeGainTrial\s*=\s*0\.0f;", code)) == 2,
            "auto-tune must drop its trial gain when it completes and when it is stopped")
    # The three row commands exist in this file's command table.
    for command in ("srtuninggain", "srtuningagc", "srtuningvad"):
        require(re.search(r'\{\s*"%s"\s*,' % command, espsr) is not None,
                f"System_ESPSR.cpp: no command row for {command}")
    require(re.search(r'\{\s*"g2streamres"\s*,', glasses) is not None, "G2_Glasses.cpp: g2streamres command missing")
    # The lens page keeps writing through the command (it only reads the fields).
    page = (COMPONENT / "G2_Page_CameraSettings.cpp").read_text()
    require('"g2streamres %dx%d"' in page, "the Camera Settings lens page must submit g2streamres")
    require(not re.search(r"gSettings\.g2Stream(Width|Height)\s*=[^=]", page),
            "the Camera Settings lens page must not write the stream size directly")

    # 3. Constructor defaults, for the compiled comparison.
    constructor = {}
    for field in NEW_ROWS:
        match = re.search(r"^\s*%s\(([^)]+)\),?\s*(?://.*)?$" % field, settings_header, re.M)
        require(match is not None, f"System_Settings.h: no constructor initialiser for {field}")
        constructor[field] = match.group(1).strip()

    def definition(opening: str) -> str:
        # `opening` ends at the definition's own brace (a forward declaration
        # of the same name may come first).
        at = settings_header.index(opening)
        return extract_block(settings_header[at:], opening[:-1].rstrip()) + ";"
    types = "\n".join(definition(opening) for opening in (
        "enum SettingType {", "struct SettingEntry {", "struct SettingsModule {"))
    tables = "\n".join((
        extract_block(espsr, "static const SettingEntry espsrSettingsEntries[]") + ";",
        extract_block(espsr, "extern const SettingsModule espsrSettingsModule") + ";",
        extract_block(glasses, "static const SettingEntry g2DeviceSettingEntries[]") + ";",
        extract_block(glasses, "extern const SettingsModule g2DeviceSettingsModule") + ";"))
    field_types = {"INT": "int", "BOOL": "bool", "FLOAT": "float", "U8": "uint8_t"}
    fields = re.findall(r"SETTING_(\w+),\s*&gSettings\.(\w+)", tables)
    state = "static struct TestSettings {\n" + "\n".join(
        f"  {field_types[kind]} {name}{{}};" for kind, name in fields) + "\n} gSettings;"
    production = "\n\n".join([
        extract_block(settings, "static JsonObject jsonPathCreate("),
        extract_block(settings, "static JsonVariantConst jsonPathRead("),
        extract_block(settings, "void applyRegisteredDefaults()"),
        extract_block(settings, "static int settingsLoadClampInt("),
        re.search(r"^static uint32_t gSettingsTypeMismatches[^\n]+", settings, re.M)[0],
        extract_block(settings, "enum SettingLoadVerdict") + ";",
        extract_block(settings, "static SettingLoadVerdict settingsTypeCheck("),
        extract_block(settings, "static float settingsLoadClampFloat("),
        extract_block(settings, "size_t readRegisteredSettings("),
        extract_block(settings, "size_t writeRegisteredSettings("),
    ])
    autotune = "\n".join((
        re.search(r"^EXT_RAM_BSS_ATTR static float gSrAfeGainTrial;", espsr, re.M)[0],
        extract_block(espsr, "static float srEffectiveAfeGain()")))
    unit = HARNESS
    for marker, content in (("TYPES", types), ("SETTINGS", state), ("TABLES", tables),
                            ("PRODUCTION", production), ("AUTOTUNE", autotune)):
        require(unit.count("// INSERT_" + marker + "\n") == 1, f"expected one insertion point: {marker}")
        unit = unit.replace("// INSERT_" + marker + "\n", content + "\n")

    with tempfile.TemporaryDirectory(prefix="hw1-tuning-settings-") as directory:
        tmp = Path(directory)
        (tmp / "Arduino.h").write_text((HERE / "web_batch_stubs/Arduino.h").read_text())
        (tmp / "test.cpp").write_text(unit)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-variable", "-Wno-sign-compare",
                   *[f"-DCTOR_{field}={value}" for field, value in constructor.items()],
                   "-I", str(tmp), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(tmp / "test.cpp"), "-o", str(tmp / "test")]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(tmp / "test")], check=True, timeout=60)


if __name__ == "__main__":
    main()
