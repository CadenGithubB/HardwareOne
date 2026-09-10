#!/usr/bin/env python3
"""Test actual OTA reply macros/representative handlers with real ArduinoJson.

Extracts both production memory macros, cmdOtaStatus, cmdOtaResetJournal, and
bleUploadJson. Other OTA commands have capacity/ordering source guards, not a
test-local OTA state machine. Hardware/NVS/status data and allocation backends
are mocks; real PreferPSRAM fallback policy is covered by MemUtil host tests.
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


def macro_definition(source: str, name: str) -> str:
    start = source.index(f"#define {name}(")
    result = []
    for line in source[start:].splitlines():
        result.append(line)
        if not line.rstrip().endswith("\\"):
            return "\n".join(result)
    raise AssertionError(f"unterminated macro: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "System_OTA.cpp").read_text()
    memory = (COMPONENT / "System_MemUtil.h").read_text()
    capacities = {
        "cmdOtaWrite": {"error": 192, "response": 320},
        "cmdOtaStatus": {"output": 1536},
        "cmdOtaPin": {"error": 160},
        "cmdOtaStage": {"response": 240},
        "cmdOtaUpdate": {"response": 240},
        "cmdOtaRecovery": {"response": 220},
        "cmdOtaCancel": {"response": 192},
        "cmdOtaResetJournal": {"response": 192},
        "cmdOtaAcknowledge": {"response": 192},
    }
    handlers = {name: extract_block(source, f"const char* {name}(") for name in capacities}
    require(sum(sum(items.values()) for items in capacities.values()) == 3484,
            "expected ten original response capacities totaling 3484 bytes")
    require(source.count("PSRAM_STATIC_BUF(") == 10,
            "OTA reply migration must retain exactly ten lazy buffer sites")
    for name, buffers in capacities.items():
        handler = handlers[name]
        for buffer, size in buffers.items():
            pattern = rf"PSRAM_STATIC_BUF\(\s*{buffer}\s*,\s*{size}\s*\)"
            require(len(re.findall(pattern, handler)) == 1,
                    f"{name}/{buffer} must preserve its {size}-byte capacity")
            require(not re.search(rf"sizeof\s*\(\s*{buffer}\s*\)", handler),
                    f"{name} uses pointer sizeof instead of {buffer}_SIZE")
            require(not re.search(rf"static\s+char\s+{buffer}\s*\[", handler),
                    f"{name} restored internal BSS reply array")
        first_buffer = handler.index("PSRAM_STATIC_BUF(")
        mutation_boundary = ("currentSecureBleConnection(" if name == "cmdOtaWrite" else
                             "String passphrase" if name == "cmdOtaPin" else
                             "nvs_open(" if name == "cmdOtaResetJournal" else "loadRecord(")
        require(first_buffer < handler.index(mutation_boundary),
                f"{name} lazy allocation occurs after an OTA state/secret-copy boundary")
    write = handlers["cmdOtaWrite"]
    require(write.index("PSRAM_STATIC_BUF(response, 320)") < write.index("currentSecureBleConnection("),
            "BLE response storage must be allocated before upload work, not inside reply formatter")
    require(len(re.findall(r"bleUploadJson\(\s*response\s*,\s*response_SIZE\s*,", write)) == 5,
            "every BLE reply call must pass explicit response pointer and original capacity")
    ble = extract_block(source, "const char* bleUploadJson(")
    require("PSRAM_STATIC_BUF" not in ble, "BLE formatter must not allocate its reply after upload mutation")
    require("serializeJson(doc, response, responseSize)" in ble,
            "BLE formatter must use caller-supplied capacity")
    for name, definition in (
        ("manifest", extract_block(source, "bool loadVerifiedManifest(")),
        ("BLE reply", ble), ("status", handlers["cmdOtaStatus"]),
    ):
        require(definition.count("PSRAM_JSON_DOC(doc)") == 1 and "JsonDocument doc;" not in definition,
                f"{name} JSON must use the production PSRAM allocator")
    require(source.count("PSRAM_JSON_DOC(doc)") == 3, "expected all three OTA JSON sites")
    for definition in (ble, handlers["cmdOtaStatus"]):
        require(definition.index("doc.overflowed()") < definition.index("serializeJson("),
                "response JSON OOM must be reported before serialization")
    macros = "\n\n".join(macro_definition(memory, name) for name in ("PSRAM_STATIC_BUF", "PSRAM_JSON_DOC"))
    declarations = "\n\n".join((handlers["cmdOtaStatus"], handlers["cmdOtaResetJournal"], ble))
    harness = (HERE / "ota_psram_reply_harness.cpp").read_text()
    for marker, definition in {
        "// INSERT_PRODUCTION_MEMORY_MACROS_HERE": macros,
        "// INSERT_PRODUCTION_OTA_REPLIES_HERE": declarations,
    }.items():
        require(harness.count(marker) == 1, f"expected one marker: {marker}")
        harness = harness.replace(marker, definition)
    with tempfile.TemporaryDirectory(prefix="hw1-ota-replies-host-") as temp:
        generated = Path(temp) / "ota_replies.cpp"
        executable = Path(temp) / "ota_replies"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(HERE / "web_batch_stubs"), "-I", str(JSON_INCLUDE),
                   str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
