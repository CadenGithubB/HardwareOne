#!/usr/bin/env python3
"""`versions`: the production collector and formatter, per feature selection.

versionsCollect() is spliced from System_Versions.cpp into versions_harness.cpp
over the real System_VersionsCore.h. Constants come from the production
sources (the dependency-free headers directly; the rest from their source
lines), runtime reads are fixtures. Each selection is its own compile, so a
boundary that is compiled out must leave no item (no updater line without the
OTA layout, no companion or STT line off the P4, no phone lines without the
pipe, no automations or backup format line without that feature). Checked: the
exact key list per selection, the text form (one aligned line per labelled
item, JSON-only items not printed), the JSON form (schema 1, string values,
stable keys) and that tools/gen_build_info.py's BUILD_INFO.md
Versions table uses the same keys and values for the source-known items.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"
# `status json` is about 2 KB on a P4 before its "versions" object; the
# document is refused whole at CMD_RESULT_MAX (4096). Keep the object within
# this so the largest build keeps headroom.
VERSIONS_OBJECT_BUDGET = 1024

GATES = ("HW1_OTA_LAYOUT", "ENABLE_ESPNOW", "ENABLE_AUTOMATION", "ENABLE_MIGRATION_TOOL",
         "ENABLE_BLUETOOTH", "ENABLE_BLE_PHONE_PIPE",
         "ENABLE_RASPBERRY_PI_HOST_POWER", "ENABLE_RASPBERRY_PI_HOST_FAN",
         "HW1_RADIO_COMPANION", "ENABLE_LOCAL_STT")
FIXTURES = ("ENABLE_STT_MODEL_15X5_GEN2", "FIXTURE_UPDATER_PRESENT", "FIXTURE_ESPNOW_STACK",
            "FIXTURE_COMPANION_KNOWN")

OTA = ["ota_layout", "updater", "ota_manifest_format", "ota_data_schema"]
ESPNOW = ["espnow", "espnow_wire", "espnow_stack", "espnow_identity_file", "espnow_peer_file"]
FILES = ["automations_file", "backup_format"]
PHONE = ["phone_link", "phone_pipe_channel", "phone_console_records"]
C6 = ["c6_hosted", "c6_hosted_qualified", "c6_bridge_wire", "c6_bridge_wire_expected"]
TAIL = ["idf", "arduino"]

# name: (definitions, expected keys in order, spot-checked values)
PROFILES = {
    "bare": ({}, ["firmware"] + TAIL, {}),
    "ota": ({"HW1_OTA_LAYOUT": 1, "FIXTURE_UPDATER_PRESENT": 1},
            ["firmware"] + OTA + TAIL, {"updater": "1.1.0+fixture"}),
    "ota_no_updater": ({"HW1_OTA_LAYOUT": 1}, ["firmware"] + OTA + TAIL, {"updater": "unavailable"}),
    "s3_mesh": ({"HW1_OTA_LAYOUT": 1, "FIXTURE_UPDATER_PRESENT": 1, "ENABLE_ESPNOW": 1,
                 "FIXTURE_ESPNOW_STACK": 2, "ENABLE_AUTOMATION": 1, "ENABLE_MIGRATION_TOOL": 1,
                 "ENABLE_BLUETOOTH": 1, "ENABLE_RASPBERRY_PI_HOST_FAN": 1},
                ["firmware"] + OTA + ESPNOW + FILES + ["ble_console_channel", "cm5_control"] + TAIL,
                {"espnow": "mesh v4 on ESP-NOW v1 frames", "espnow_stack": "v2", "automations_file": "2",
                 "backup_format": "1"}),
    # Each persisted-file item is gated by its own feature alone.
    "automation_only": ({"ENABLE_AUTOMATION": 1}, ["firmware", "automations_file"] + TAIL,
                        {"automations_file": "2"}),
    "migration_only": ({"ENABLE_MIGRATION_TOOL": 1}, ["firmware", "backup_format"] + TAIL,
                       {"backup_format": "1"}),
    "espnow_not_started": ({"ENABLE_ESPNOW": 1}, ["firmware"] + ESPNOW + TAIL,
                           {"espnow_stack": "unknown"}),
    "p4": ({"ENABLE_ESPNOW": 1, "FIXTURE_ESPNOW_STACK": 2, "ENABLE_AUTOMATION": 1,
            "ENABLE_MIGRATION_TOOL": 1, "ENABLE_BLUETOOTH": 1, "ENABLE_BLE_PHONE_PIPE": 1,
            "HW1_RADIO_COMPANION": 1, "FIXTURE_COMPANION_KNOWN": 1, "ENABLE_LOCAL_STT": 1,
            "ENABLE_STT_MODEL_15X5_GEN2": 1, "ENABLE_RASPBERRY_PI_HOST_POWER": 1},
           ["firmware"] + ESPNOW + FILES + ["ble_console_channel"] + PHONE + ["cm5_control"] + C6 +
           ["stt_model"] + TAIL,
           {"espnow_stack": "v2", "c6_hosted": "v2.12.13", "c6_bridge_wire": "1"}),
    # The companion answered at transport start, ESP-NOW has not started: the
    # stack is read at ESP-NOW init, never from the companion's identity cache.
    "p4_espnow_not_started": ({"ENABLE_ESPNOW": 1, "HW1_RADIO_COMPANION": 1,
                               "FIXTURE_COMPANION_KNOWN": 1},
                              ["firmware"] + ESPNOW + C6 + TAIL,
                              {"espnow_stack": "unknown", "c6_hosted": "v2.12.13"}),
    "p4_companion_down": ({"ENABLE_ESPNOW": 1, "HW1_RADIO_COMPANION": 1, "ENABLE_LOCAL_STT": 1},
                          ["firmware"] + ESPNOW + C6 + ["stt_model"] + TAIL,
                          {"espnow_stack": "unknown", "c6_hosted": "unknown",
                           "c6_bridge_wire": "unknown",
                           "stt_model": re.compile(r"quartznet5x5\.p4\.stt sha [0-9a-f]{8}$")}),
}


def find(pattern, path):
    text = path.read_text()
    match = re.search(pattern, text, re.M)
    if not match:
        raise SystemExit(f"{path.name}: no match for {pattern!r}")
    return match.group(1)


def constants():
    """The values the device takes from SDK-dependent sources, from their lines."""
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block
    wire = COMPONENT / "System_ESPNow_Wire.h"
    lines = [m.group(0) for m in re.finditer(
        r"^#define\s+(?:ESPNOW_V4_VERSION|ESPNOW_V4_HEADER_LEN|ESPNOW_V4_MAX_PAYLOAD)\b.*$",
        wire.read_text(), re.M)]
    if len(lines) != 3:
        raise SystemExit(f"System_ESPNow_Wire.h: expected 3 version defines, found {len(lines)}")
    identity = COMPONENT / "System_ESPNow_Identity.cpp"
    values = {
        "kFixtureDataSchema": ("uint32_t", find(r"^constexpr uint32_t kDataSchemaV1 = (\d+);",
                                                COMPONENT / "System_OTA.cpp")),
        "kFixtureIdentityFileVersion": ("uint8_t", find(r"\bkIdentityFileVersion = (\d+);", identity)),
        "kFixturePeerFileVersion": ("uint8_t", find(r"\bkPeerFileVersion = (\d+);", identity)),
        "kFixtureScLabel": ("const char*", '"%s"' % find(r'SC_INFO\[\]\s*=\s*"([^"]+)"',
                                                         COMPONENT / "System_BleSecureChannel.cpp")),
        "kFixtureCm5Version": ("const char*", '"%s"' % find(r'kProtocolVersion = "(\d+)";',
                                                            COMPONENT / "System_Cm5HostControl.cpp")),
        "kFixturePipeLabel": ("const char*", '"%s"' % find(r'kPipeLabel\[\]\s*=\s*"([^"]+)"',
                                                           COMPONENT / "BLE_PhoneSecureChannel.h")),
    }
    for name, header in (("kAutomationsFileVersion", "System_Automation.h"),
                         ("kBackupFormatVersion", "WebServer_MigrationTool.h")):
        line = re.findall(rf"^constexpr uint32_t {name} = \d+;$", (COMPONENT / header).read_text(), re.M)
        if len(line) != 1:
            raise SystemExit(f"{header}: expected one {name} line, found {len(line)}")
        lines.append("inline " + line[0])  # inline: unused in selections without the feature
    radio = (COMPONENT / "HAL_Radio.h").read_text()
    types = [extract_block(radio, marker) + ";" for marker in
             ("enum class RadioBridgeState : uint8_t", "struct RadioCompanionIdentity")]
    return "\n".join(lines + [f"inline constexpr {t} {name} = {v};" for name, (t, v) in values.items()]
                     + types)


def collector():
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block
    source = (COMPONENT / "System_Versions.cpp").read_text()
    marker = "void versionsCollect(hw1versions::List& out)"
    if source.count(marker) != 1:
        raise SystemExit(f"System_Versions.cpp: {marker!r} found {source.count(marker)} times")
    body = extract_block(source, marker)
    used = set(re.findall(r"^\s*#\s*(?:el)?if\b(.*)$", body, re.M))
    gates = set(re.findall(r"\b(?:ENABLE_\w+|HW1_\w+)\b", " ".join(used)))
    if not gates <= set(GATES):
        raise SystemExit(f"versionsCollect gains gates this test does not select: {sorted(gates - set(GATES))}")
    return body


def add_json():
    """versionsAddJson and the list allocation it uses, as shipped."""
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block
    source = (COMPONENT / "System_Versions.cpp").read_text()
    return "\n\n".join(extract_block(source, marker) for marker in (
        "hw1versions::List* newList()", "void freeList(hw1versions::List* list)",
        "void versionsAddJson(JsonObject out)"))


def check_status_json(component, failures):
    """`status json` adds the object after the system info and caps the whole
    document at the command reply limit (the size guard itself is checked by
    test_phone_status.py)."""
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block
    body = extract_block((component / "System_Utils.cpp").read_text(), "const char* cmd_status(")
    path = body[body.index("if (argWantsJson(argsInput))"):]
    if "static constexpr size_t kStatusJsonCap = CMD_RESULT_MAX;" not in path:
        failures.append("status json: kStatusJsonCap is not CMD_RESULT_MAX")
    add = path.find('versionsAddJson(doc["versions"].to<JsonObject>());')
    if not (path.index("buildSystemInfoJson(doc") < add < path.index("measureJson(doc) >= kStatusJsonCap")):
        failures.append("status json: the versions object is not added before the size guard")


def check_add_json(name, exe, doc, failures):
    """The "versions" object: the same keys and values as `versions json`, in order."""
    text = run(exe, "addjson")
    obj = json.loads(text).get("versions")
    want = {k: v for k, v in doc.items() if k != "schema"}
    if not isinstance(obj, dict) or list(obj.items()) != list(want.items()):
        failures.append(f"{name}: status json versions {obj!r} != versions json {want!r}")
    size = len(json.dumps({"versions": obj}, separators=(",", ":"))) if isinstance(obj, dict) else 0
    if size > VERSIONS_OBJECT_BUDGET:
        failures.append(f"{name}: status json versions object is {size} bytes, budget {VERSIONS_OBJECT_BUDGET}")
    return size


def build(cxx, sanitize, work, name, definitions, harness):
    exe = work / f"versions_{name}"
    cmd = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-I", str(COMPONENT), "-I", str(JSON_INCLUDE), "-I", str(COMPONENT.parent / "esp_now_hosted" / "include"),
           "-I", str(COMPONENT.parent / "hw1_ota_protocol" / "include")]
    for macro in GATES + FIXTURES:
        cmd.append(f"-D{macro}={int(definitions.get(macro, 0))}")
    if sanitize:
        cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    cmd += [str(harness), "-o", str(exe)]
    subprocess.run(cmd, check=True, timeout=180)
    return exe


def run(exe, mode):
    return subprocess.run([str(exe), mode], check=True, text=True, capture_output=True,
                          timeout=60).stdout.rstrip("\n")


def check_profile(name, exe, keys, spot, failures):
    doc = json.loads(run(exe, "json"))
    got = list(doc)
    if got != ["schema"] + keys:
        failures.append(f"{name}: keys {got[1:]} != {keys}")
    if doc.get("schema") != 1:
        failures.append(f"{name}: schema {doc.get('schema')!r}")
    for key in keys:
        if not isinstance(doc.get(key), str) or not doc.get(key):
            failures.append(f"{name}: {key} is not a non-empty string: {doc.get(key)!r}")
    for key, want in spot.items():
        value = doc.get(key, "")
        ok = want.match(value) if hasattr(want, "match") else value == want
        if not ok:
            failures.append(f"{name}: {key} = {value!r}, want {want if isinstance(want, str) else want.pattern!r}")
    text = run(exe, "text").split("\n")
    shown = [k for k in keys if k != "espnow_wire"]  # the wire byte is JSON only
    if len(text) != len(shown):
        failures.append(f"{name}: {len(text)} text lines for {len(shown)} labelled items")
        return doc
    column = {len(line) - len(doc[key]) for line, key in zip(text, shown)}
    if len(column) != 1:
        failures.append(f"{name}: values do not start in one column: {text}")
    for line, key in zip(text, shown):
        if not line.endswith("  " + doc[key]) or line.startswith(" "):
            failures.append(f"{name}: line {line!r} does not end with {key}'s value")
    if "espnow" in keys and not any(re.fullmatch(r"ESP-NOW\s+mesh v4 on ESP-NOW v1 frames", l) for l in text):
        failures.append(f"{name}: no 'ESP-NOW  mesh v4 on ESP-NOW v1 frames' line: {text}")
    if any("wire byte" in l.lower() for l in text):
        failures.append(f"{name}: the wire byte is printed")
    return doc


def check_build_info(doc, failures):
    """BUILD_INFO.md's Versions rows: same keys, same values where the source knows them."""
    sys.path.insert(0, str(REPO / "tools"))
    import gen_build_info
    argv = ['-DHW1_OTA_LAYOUT_ID="fixture-layout"', '-DIDF_VER=\\"v5.5.5-fixture\\"']
    rows = gen_build_info.source_versions(
        lambda flag: True, "0.99.99-fixture", argv, companion=True)
    runtime = {"firmware", "arduino"}  # fixtures on the device side
    build_only = {"updater_built"}  # the tree's updater; the device reports its own as `updater`
    keys = [key for key, _label, _value in rows]
    if "updater_built" not in keys or "updater" in keys:
        failures.append(f"BUILD_INFO: the tree's updater must be `updater_built`, never `updater`: {keys}")
    for key, _label, value in rows:
        if key in build_only:
            if key in doc:
                failures.append(f"BUILD_INFO key {key!r} is also a device key")
        elif key not in doc:
            failures.append(f"BUILD_INFO key {key!r} is not a device key")
        elif key not in runtime and doc[key] != value:
            failures.append(f"BUILD_INFO {key} = {value!r}, device {doc[key]!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("a host C++17 compiler is required")
    harness_text = (HERE / "versions_harness.cpp").read_text()
    for tag, text in (("// INSERT_CONSTANTS", constants()), ("// INSERT_COLLECT", collector()),
                      ("// INSERT_ADDJSON", add_json())):
        if harness_text.count(tag) != 1:
            raise SystemExit(f"harness: {tag} must appear once")
        harness_text = harness_text.replace(tag, text)
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-versions-") as temp:
        work = Path(temp)
        harness = work / "versions_harness.cpp"
        harness.write_text(harness_text)
        exe = build(args.cxx, args.sanitize, work, "selftest", {}, harness)
        if subprocess.run([str(exe), "selftest"], timeout=60).returncode:
            failures.append("formatter selftest")
        docs = {}
        sizes = {}
        for name, (definitions, keys, spot) in PROFILES.items():
            exe = build(args.cxx, args.sanitize, work, name, definitions, harness)
            docs[name] = check_profile(name, exe, keys, spot, failures)
            sizes[name] = check_add_json(name, exe, docs[name], failures)
        merged = dict(docs["p4"])
        merged.update({k: v for k, v in docs["s3_mesh"].items() if k not in merged})
        check_build_info(merged, failures)
        check_status_json(COMPONENT, failures)
    if failures:
        print("versions: FAILED\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print(f"versions: OK ({len(PROFILES)} feature selections + formatter selftest; "
          f"status json versions object at most {max(sizes.values())} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
