#!/usr/bin/env python3
"""Exercise shipping settings mutation and feature-command paths on the host.

The generic handler, three mutator overloads, shared notification/finalization,
feature command and persistence adapter are extracted intact. The batching core
and ArduinoJson are production headers; registry entries are controlled fixtures.
Host String allocation-failure controls model failed operations, not ESP heap
placement or timing. Flash, task identity, lifecycle commands and UI are observed
boundaries. No wire, device, or filesystem durability qualification is implied.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def generic_setting_definitions(source: str, utils: str) -> list[str]:
    """Shared extraction keeps dependent logger tests on the actual parser."""
    return [extract_block(utils, "inline int parseBoolArg("),
            extract_block(source, "const char* handleSettingCommand(")]


def generated() -> str:
    settings_h = (COMPONENT / "System_Settings.h").read_text()
    settings_cpp = (COMPONENT / "System_Settings.cpp").read_text()
    feature_h = (COMPONENT / "System_FeatureRegistry.h").read_text()
    feature_cpp = (COMPONENT / "System_FeatureRegistry.cpp").read_text()
    utils = (COMPONENT / "System_Utils.h").read_text()
    command_h = (COMPONENT / "System_Command.h").read_text()
    command_cpp = (COMPONENT / "System_Command.cpp").read_text()
    types = [extract_block(settings_h, marker) + ";" for marker in
             ("enum SettingType", "struct SettingEntry ")]
    detail_limit = re.search(r"(?m)^#define SYSEVT_DETAIL_LEN .+$",
                             (COMPONENT / "System_Events.h").read_text())
    require(detail_limit is not None, "missing production event payload capacity")
    types.append(detail_limit.group())
    types += ["typedef bool (*ConnectionCheckFunc)();",
              extract_block(settings_h, "struct SettingsModule") + ";"]
    types += [extract_block(feature_h, marker) + ";" for marker in
              ("enum FeatureCategory", "enum FeatureFlags", "struct FeatureEntry")]
    types += [extract_block(command_h, "class CommandArgs") + ";"]
    definitions = [extract_macro(utils, "RETURN_VALID_IF_VALIDATE_CSTR"),
                   "const String CommandArgs::empty_;", *[
        extract_block(command_cpp, marker) for marker in
        ("CommandArgs::CommandArgs(", "void CommandArgs::parse()",
         "const String& CommandArgs::arg(", "String CommandArgs::remaining(")],
        extract_block(settings_cpp, "bool requestSettingsPersist("),
        extract_block(settings_cpp, "static void applyMasterEnableDisable("),
        extract_block(settings_cpp, "void notifySettingChanged("),
        extract_block(settings_cpp, "void finishSettingChange("),
        extract_block(settings_h, "template<typename T>\ninline void setSetting("),
        extract_block(settings_h, "inline void setSetting(String& field, const String&"),
        extract_block(settings_h, "inline void setSetting(String& field, const char*"),
        *generic_setting_definitions(settings_cpp, utils)]
    definitions += [extract_block(feature_cpp, marker) for marker in
                    ("const FeatureEntry* getFeatureById(",
                     "bool isFeatureCompiled(", "bool isFeatureEnabled(",
                     "bool canToggleFeature(",
                     "uint32_t getEnabledFeaturesHeapEstimate(",
                     "uint32_t getTotalPossibleHeapCost(",
                     "static constexpr const char* getCategoryName(",
                     "static inline uint16_t reportedHeapKB(",
                     "const char* cmd_features(")]
    harness = (HERE / "settings_mutation_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-settings-mutation-") as temporary:
        directory = Path(temporary)
        source = directory / "settings.cpp"
        source.write_text(generated())
        arduino = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
        arduino = arduino.replace("class String {", "bool settingsTestStringCopyAllowed();\n"
                                  "bool settingsTestStringReserveAllowed();\nclass String {")
        arduino = arduino.replace("  String() = default;", "  String() = default;\n"
                                  "  String(const String& other) : text_(settingsTestStringCopyAllowed() ? other.text_ : std::string{}) {}\n"
                                  "  String& operator=(const String&) = default;")
        arduino = arduino.replace("  char operator[]", "  bool reserve(size_t count) { if (!settingsTestStringReserveAllowed()) return false; text_.reserve(count); return true; }\n  char operator[]")
        (directory / "Arduino.h").write_text(arduino)
        for oled in (0, 1):
            executable = directory / f"settings-{oled}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", "-Wno-sign-compare", f"-DENABLE_OLED_DISPLAY={oled}",
                       "-I", str(directory), "-I", str(COMPONENT),
                       "-I", str(JSON_INCLUDE), str(source), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
