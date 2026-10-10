#!/usr/bin/env python3
"""Execute production logger settings handlers and health session transitions.

The real generic settings editor, changed-only setting mutator and argument
parser are compiled with owner handlers. Persistence, setting events, session
path shaping and session startup are controlled host boundaries. --baseline
replays the assertions against the old settings-side wrappers.
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
from test_settings_mutation import generic_setting_definitions

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(baseline: Path | None) -> str:
    source_dir = baseline or COMPONENT
    owner = (source_dir / "System_SensorLogging.cpp").read_text()
    settings_cpp = (source_dir / "System_Settings.cpp").read_text()
    header = (COMPONENT / "System_SensorLogging.h").read_text()
    settings = (COMPONENT / "System_Settings.h").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    types = ["\n".join(re.findall(r"(?m)^#define LOG_.*$", header)),
             extract_block(header, "enum SensorLogFormat") + ";",
             extract_block(settings, "enum SettingType") + ";",
             extract_block(settings, "struct SettingEntry ") + ";",
             extract_block((COMPONENT / "System_Command.h").read_text(), "class CommandArgs") + ";"]
    entries = []
    for key in ("sensorLogMask", "sensorLogFormat"):
        match = re.search(r'  \{ "' + key + r'",[\s\S]*?\},', owner)
        require(match is not None, f"missing production setting entry {key}")
        entries.append(match.group())
    definitions = [extract_macro((COMPONENT / "System_Utils.h").read_text(), "RETURN_VALID_IF_VALIDATE_CSTR"),
                   "const String CommandArgs::empty_;", *[
        extract_block(command, marker) for marker in (
            "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
            "const String& CommandArgs::arg(", "String CommandArgs::remaining(")],
        extract_block(settings, "template<typename T>\ninline void setSetting("),
        *generic_setting_definitions(settings_cpp, (COMPONENT / "System_Utils.h").read_text())]
    if "const char* cmd_sensorlogmask(" in owner:
        definitions += [extract_block(owner, marker) for marker in (
            "uint8_t sensorLogSelectedMask(", "SensorLogFormat sensorLogCurrentFormat(",
            "static const char* sensorLogSelectionChangeError(",
            "static void sensorLogApplyFormat(", "const char* cmd_sensorlogmask(",
            "const char* cmd_sensorlogformat(")]
    else:
        require(baseline is not None, "logger does not own numeric settings handlers")
        definitions += [extract_block(settings_cpp, "static const char* cmd_set_sensorlog" + suffix + "(").replace(
            "cmd_set_sensorlog" + suffix, "cmd_sensorlog" + suffix) for suffix in ("mask", "format")]
        definitions += ["static uint8_t sensorLogSelectedMask() { return gSensorLogMask; }",
                        "static SensorLogFormat sensorLogCurrentFormat() { return gSensorLogFormat; }"]
    definitions.append('static const char* textFormat(const String& input) {\n'
                       '  RETURN_VALID_IF_VALIDATE_CSTR();\n'
                       '  const CommandArgs a(input);\n'
                       '  const String subCmd = "format";\n' +
                       extract_block(owner, 'if (subCmd == "format")') +
                       '\n  return "Error: unreachable";\n}')
    # Older saved baselines still form a command string for health restarts.
    # Parse that historical boundary with the real parser so the same path and
    # interval assertions remain meaningful after the typed-start migration.
    if "return cmd_sensorlog(String(cmd));" in extract_block(owner, "static const char* healthLoggingRestartWithCurrentMask("):
        start = extract_block(owner, 'if (subCmd == "start")')
        start = start[:start.index("    if (filepath.length() == 0")]
        definitions += ["#if ENABLE_R1_HEALTH", extract_block(command, "int CommandArgs::argInt("),
                        "static const char* cmd_sensorlog(const String& input) {\n"
                        "  const CommandArgs a(input);\n"
                        "  const String subCmd = \"start\";\n" + start +
                        "    return sensorLogStart(filepath, interval);\n  }\n"
                        "  return \"Error: unreachable\";\n}",
                        "#endif"]
    if "static uint32_t sensorLogClampInterval(" in owner:
        definitions += ["#if ENABLE_R1_HEALTH", extract_block(owner, "static uint32_t sensorLogClampInterval("), "#endif"]
    if "static bool sensorLogMarkStopped(" in owner:
        definitions += ["#if ENABLE_R1_HEALTH", extract_block(owner, "static bool sensorLogMarkStopped("), "#endif"]
    definitions += ["#if ENABLE_R1_HEALTH", *[
        extract_block(owner, marker) for marker in (
            "static const char* healthLoggingRestartWithCurrentMask(",
            "const char* healthLoggingSet(")], "#endif"]
    if baseline is None:
        for name in ("gSensorLogMask", "gSensorLogFormat", "healthLoggingSet"):
            require(re.search(r"\b" + name + r"\b", header) is None,
                    f"private logger state/mutation still public: {name}")
        for name in ("mask", "format"):
            require(re.search(r'\{\s*"sensorlog' + name + r'",[^\n]*\btrue,\s*cmd_sensorlog' + name + r',', settings_cpp),
                    f"sensorlog{name} lost registered settings-editor admin policy")
    harness = (HERE / "sensorlog_settings_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_ENTRIES", "\n".join(entries)),
                         ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"bad marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    results = []
    with tempfile.TemporaryDirectory(prefix="hw1-sensorlog-settings-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(generated(args.baseline))
        # Extend the shared String boundary only for numeric parsing.
        # Production source is never rewritten.
        arduino = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
        arduino = arduino.replace("  char operator[]", "  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }\n  char operator[]")
        arduino = arduino.replace("  char operator[]", "  bool reserve(size_t count) { text_.reserve(count); return true; }\n  char operator[]")
        arduino = arduino.replace("#include <algorithm>", "#include <algorithm>\n#include <cstdlib>")
        (Path(directory) / "Arduino.h").write_text(arduino)
        for health in (0, 1):
            executable = Path(directory) / f"test-{health}"
            subprocess.run([args.cxx, *flags, f"-DENABLE_R1_HEALTH={health}",
                            "-I", directory, str(source), "-o", str(executable)],
                           check=True, timeout=120)
            results.append(subprocess.run([str(executable)], timeout=60).returncode)
    raise SystemExit(1 if any(results) else 0)


if __name__ == "__main__":
    main()
