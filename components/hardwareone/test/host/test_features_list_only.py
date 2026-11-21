#!/usr/bin/env python3
"""`features` only lists (owner decision 2026-11-20).

Runtime half: the shipping cmd_features, its text views and the registry
helpers they read are extracted into features_list_only_harness.cpp with
fixture rows. Every query and every refused `features <id> <on|off>` leaves
each row's flags as they were; the refusal names the row's own commands; the
validate pass refuses too; `features json` keeps schema 1 byte for byte.

Source half: the registry posts no event and calls no owner; nothing in the
firmware posts SYSEVT_FEATURE_TOGGLED; every command the shipping switch table
(kFeatureSwitches) names is a registered command; where a flag's settings row
has a command key, the table names that same command for that flag; the help
row no longer offers the on/off form.
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

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"
sys.path.insert(0, str(REPO / "tools"))
import command_registry  # noqa: E402

# The command and the text views it returns. They must only read.
VIEW_MARKERS = ("static const char* featureOnOff(", "static const char* featuresListing(",
                "static bool featureSwitchLine(", "static const char* featureDetail(",
                "static const char* featureSwitchRefusal(", "const char* cmd_features(")
WRITERS = ("setSetting(", "setDebugSetting(", "systemEventPost(", "writeSettingsJson(",
           "automationSetSystemEnabled", "SYSEVT_FEATURE_TOGGLED", "executeCommand",
           "enabledSetting =", "enabledSetting=")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def generated(component: Path) -> str:
    feature_h = (component / "System_FeatureRegistry.h").read_text()
    feature_cpp = (component / "System_FeatureRegistry.cpp").read_text()
    command_h = (component / "System_Command.h").read_text()
    command_cpp = (component / "System_Command.cpp").read_text()
    utils_h = (component / "System_Utils.h").read_text()
    utils_cpp = (component / "System_Utils.cpp").read_text()
    types = [extract_block(feature_h, marker) + ";" for marker in
             ("enum FeatureCategory", "enum FeatureFlags", "struct FeatureEntry")]
    types.append(extract_block(feature_cpp, "struct FeatureSwitches") + ";")
    types.append(extract_block(command_h, "class CommandArgs") + ";")
    # The attribute-carrying declaration comes first; take the definition after it.
    append_at = feature_cpp.index("__attribute__((format(printf, 4, 5)));")
    definitions = [
        extract_macro(utils_h, "RETURN_VALID_IF_VALIDATE_CSTR"),
        "const String CommandArgs::empty_;",
        *[extract_block(command_cpp, marker) for marker in
          ("CommandArgs::CommandArgs(", "void CommandArgs::parse()",
           "const String& CommandArgs::arg(", "String CommandArgs::remaining(")],
        extract_block(utils_cpp, "bool argWantsJson("),
        *[extract_block(feature_cpp, marker) for marker in
          ("const FeatureEntry* getFeatureById(", "bool isFeatureCompiled(",
           "bool isFeatureEnabled(", "bool canToggleFeature(",
           "uint32_t getTotalPossibleHeapCost(", "static constexpr const char* getCategoryName(",
           "static inline uint16_t reportedHeapKb(")],
        extract_block(feature_cpp[append_at:], "static bool featuresAppend("),
        *[extract_block(feature_cpp, marker) for marker in VIEW_MARKERS],
    ]
    harness = (HERE / "features_list_only_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def switch_rows(feature_cpp: str) -> list[dict]:
    """The shipping kFeatureSwitches rows as {id, allowed, auto, now, boot, allow}."""
    start = feature_cpp.index("static constexpr FeatureSwitches kFeatureSwitches[] = {")
    body = strip_comments(feature_cpp[start:feature_cpp.index("\n};", start)])
    body = body[body.index("{") + 1:]
    rows = []
    for match in re.finditer(r'FEATURE_SENSOR_SWITCHES\("(\w+)",\s*(\w+),\s*\w+\)|\{([^{}]*)\}', body):
        if match.group(1):
            ident, field = match.group(1), match.group(2)
            rows.append({"id": ident, "allowed": field + "Enabled", "auto": field + "AutoStart",
                         "now": f"open{ident} / close{ident}", "boot": f"{ident}autostart <0|1>",
                         "allow": f"{ident}enabled <0|1>", "reboot": False})
            continue
        fields = [f.strip() for f in re.split(r",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", match.group(3))]
        require(len(fields) in (7, 8), f"unexpected kFeatureSwitches row: {match.group(3)!r}")
        require(len(fields) == 7 or fields[7] in ("true", "false"),
                f"unexpected allowAtReboot value: {match.group(3)!r}")

        def text(value: str):
            return None if value == "nullptr" else value.strip('"')

        def field_name(value: str):
            return None if value == "nullptr" else value.replace("&gSettings.", "")

        rows.append({"id": text(fields[0]), "allowed": field_name(fields[1]),
                     "auto": field_name(fields[2]), "now": text(fields[4]),
                     "boot": text(fields[5]), "allow": text(fields[6]),
                     "reboot": len(fields) == 8 and fields[7] == "true"})
    return rows


def registry_ids(feature_cpp: str) -> list[str]:
    start = feature_cpp.index("static constexpr FeatureEntry featureRegistry[] = {")
    end = feature_cpp.index("static const size_t featureRegistryCount", start)
    body = strip_comments(feature_cpp[start:end])
    # #if picks between two `llm` rows; count the id once.
    ids = re.findall(r'\{\s*"(\w+)",', body)
    return list(dict.fromkeys(ids))


def command_names(spec: str) -> list[str]:
    return [part.split()[0] for part in spec.split(" / ")]


def source_checks(component: Path) -> None:
    feature_cpp = (component / "System_FeatureRegistry.cpp").read_text()
    code = strip_comments(feature_cpp)
    for writer in ("systemEventPost(", "SYSEVT_FEATURE_TOGGLED", "automationSetSystemEnabled"):
        require(writer not in code, f"System_FeatureRegistry.cpp still uses {writer}")
    for marker in VIEW_MARKERS:
        block = strip_comments(extract_block(feature_cpp, marker))
        for writer in WRITERS:
            require(writer not in block, f"{marker} calls {writer}: features must only list")

    # The catalog row stays (frozen v1 catalog) but nothing posts it any more.
    for path in sorted(component.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".c") or "test" in path.relative_to(component).parts:
            continue
        text = strip_comments(path.read_text(errors="replace"))
        require("SYSEVT_FEATURE_TOGGLED" not in text or path.name == "System_EventCatalogRows.h",
                f"{path.name} posts SYSEVT_FEATURE_TOGGLED again")

    rows = switch_rows(feature_cpp)
    require([row["id"] for row in rows] == registry_ids(feature_cpp),
            "kFeatureSwitches ids differ from featureRegistry ids")
    registered = {row["name"]: row for array in command_registry.parse_commands().values()
                  for row in array}
    for row in rows:
        for key in ("now", "boot", "allow"):
            if row[key]:
                for name in command_names(row[key]):
                    require(name in registered,
                            f"{row['id']}: '{name}' ({key}) is not a registered command")
    require(any(row["now"] for row in rows), "switch table parsed no commands")

    # The "after a reboot" note goes exactly on rows whose allowed command says
    # so itself (i2cbusenabled: "(reboot required)"). wifienabled and oledenabled
    # act at once, so their rows carry no note.
    for row in rows:
        if not row["allow"]:
            continue
        name = command_names(row["allow"])[0]
        says = "reboot required" in (registered[name]["help"] + registered[name]["usage"]).lower()
        require(row["reboot"] == says,
                f"{row['id']}: allowAtReboot is {row['reboot']} but '{name}' "
                f"{'says' if says else 'does not say'} a reboot is required")
    require(any(row["reboot"] for row in rows), "no row carries the reboot note")

    # A flag whose settings row carries a command key must be switched by that
    # command: the table may not name a command for the wrong flag.
    keys: dict[str, set[str]] = {}
    for path in component.glob("*.cpp"):
        for match in re.finditer(r'SETTING_BOOL,\s*&gSettings\.(\w+),[^}\n]*?"(\w+)"\s*\}',
                                 path.read_text(errors="replace")):
            keys.setdefault(match.group(1), set()).add(match.group(2).lower())
    compared = 0
    for row in rows:
        for flag, key in (("allowed", "allow"), ("auto", "boot")):
            if row[flag] and row[key] and row[flag] in keys:
                compared += 1
                require(command_names(row[key])[0] in keys[row[flag]],
                        f"{row['id']}: {row[key]} does not switch gSettings.{row[flag]} "
                        f"(its command key is {sorted(keys[row[flag]])})")
    require(compared >= 10, f"only {compared} switch-table flags were matched to settings rows")

    usage = registered["features"]["usage"] + registered["features"]["help"]
    require("on|off" not in usage and "toggle" not in usage.lower(),
            "the features help row still offers the on/off form")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component tree to extract from")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    source_checks(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-features-list-only-") as temporary:
        directory = Path(temporary)
        source = directory / "features.cpp"
        source.write_text(generated(args.component))
        shutil.copy(HERE / "web_batch_stubs" / "Arduino.h", directory / "Arduino.h")
        executable = directory / "features"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-sign-compare", "-I", str(directory), "-I", str(JSON_INCLUDE),
                   str(source), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
    print("features list-only source checks: ok")


if __name__ == "__main__":
    main()
