#!/usr/bin/env python3
"""Keep OTA journal policy with its owner and run System_OTA's emergency recovery arm.

Source guards: neither System_OTA.cpp nor the recovery updater spells out the
"IDLE or terminal" phase test (hw1_ota_phase_is_idle_or_terminal owns it); both
early escapes in System_OTA go through armEmergencyRecovery; the updater's
recovery upload takes its flags from hw1_ota_replacement_request_flags and its
allow-downgrade action asks hw1_ota_phase_accepts_downgrade_permission; the
never-set BLE-resume-after-OTA latch is gone from System_OTA and the boot path.

Behavior: phaseName, commitRecord, transitionAndCommit, armRecoveryBoot,
bootRecoveryForRecord, resumeRecoveryBootIntent and armEmergencyRecovery come
from System_OTA.cpp unchanged and run against the real hw1_ota_protocol.c. The
NVS slot store, factory-image check and boot-partition switch are fakes.
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
REPOSITORY = COMPONENT.parents[1]
PROTOCOL = REPOSITORY / "components" / "hw1_ota_protocol"
UPDATER_MAIN = REPOSITORY / "updater" / "main" / "updater_main.c"

OTA_MARKERS = (
    "const char* phaseName(",
    "bool commitRecord(",
    "bool transitionAndCommit(",
    "bool armRecoveryBoot(",
    "bool bootRecoveryForRecord(",
    "bool resumeRecoveryBootIntent(",
    "bool armEmergencyRecovery(",
)
SPELLED_OUT_IDLE_OR_TERMINAL = re.compile(r"phase\s*!=\s*HW1_OTA_PHASE_IDLE\s*&&")


def block_or_none(source: str, marker: str) -> str | None:
    if source.count(marker) != 1:
        return None
    return extract_block(source, marker)


def source_guard_failures() -> list[str]:
    ota = (COMPONENT / "System_OTA.cpp").read_text()
    ota_h = (COMPONENT / "System_OTA.h").read_text()
    main = (COMPONENT / "HardwareOne.cpp").read_text()
    updater = UPDATER_MAIN.read_text()
    failures: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    for name, text in (("System_OTA.cpp", ota), ("updater_main.c", updater)):
        expect(SPELLED_OUT_IDLE_OR_TERMINAL.search(text) is None,
               f"{name} spells out the IDLE-or-terminal phase test instead of "
               "hw1_ota_phase_is_idle_or_terminal()")
    expect(ota.count("hw1_ota_phase_is_idle_or_terminal(") >= 3,
           "System_OTA.cpp asks hw1_ota_phase_is_idle_or_terminal() at its start gates")
    expect(updater.count("hw1_ota_phase_is_idle_or_terminal(") >= 2,
           "updater_main.c asks hw1_ota_phase_is_idle_or_terminal()")

    for marker in ("void otaSystemCrashLoopEscapeEarly(", "bool otaSystemRecoverFromStorageFailure("):
        body = block_or_none(ota, marker)
        expect(body is not None, f"System_OTA.cpp defines {marker.strip('(')} once")
        if body is None:
            continue
        expect("armEmergencyRecovery(" in body,
               f"{marker.strip('(')} arms recovery through armEmergencyRecovery()")
        expect("hw1_ota_begin(" not in body and "SUPERSEDE_PENDING_RESULT" not in body,
               f"{marker.strip('(')} does not open its own recovery operation")
        expect(re.search(r"PHASE_RECOVERY_BOOT_ARMED\)\s*\{\s*if \(esp_ota_set_boot_partition", body)
               is None, f"{marker.strip('(')} has no unjournaled armed-intent switch")

    accept = block_or_none(updater, "static esp_err_t accept_manifest(")
    expect(accept is not None, "updater_main.c defines accept_manifest once")
    if accept is not None:
        expect("hw1_ota_replacement_request_flags(&record)" in accept,
               "accept_manifest takes a new upload's flags from hw1_ota_replacement_request_flags()")
        expect(re.search(r"\?\s*record\.request_flags", accept) is None and "retry_flags" not in accept,
               "accept_manifest does not carry the replaced record's request flags")
    allow = block_or_none(updater, "static esp_err_t allow_downgrade_for_transaction(void)\n")
    expect(allow is not None, "updater_main.c defines allow_downgrade_for_transaction once")
    if allow is not None:
        expect("hw1_ota_phase_accepts_downgrade_permission(record.phase)" in allow,
               "allow-downgrade asks hw1_ota_phase_accepts_downgrade_permission() and so refuses FAILED")

    for name, text in (("System_OTA.cpp", ota), ("System_OTA.h", ota_h), ("HardwareOne.cpp", main)):
        expect("otaSystemResumeBleRequested" not in text and "gResumeBleAfterOta" not in text,
               f"{name} still carries the never-set BLE-resume-after-OTA latch")
    return failures


def generate() -> str:
    ota = (COMPONENT / "System_OTA.cpp").read_text()
    functions = []
    for marker in OTA_MARKERS:
        require(ota.count(marker) == 1, f"System_OTA.cpp defines {marker.strip('(')} once")
        functions.append(extract_block(ota, marker))
    harness = (HERE / "ota_journal_policy_harness.cpp").read_text()
    require(harness.count("// INSERT_FUNCTIONS") == 1, "one insertion marker")
    return harness.replace("// INSERT_FUNCTIONS", "\n\n".join(functions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++ compiler is required")

    failures = source_guard_failures()
    require(not failures, "OTA journal policy guards failed:\n  " + "\n  ".join(failures))

    sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
    warnings = ["-Wall", "-Wextra", "-Werror", "-pedantic"]
    include = ["-I", str(PROTOCOL / "include")]
    with tempfile.TemporaryDirectory(prefix="hw1-ota-journal-policy-") as temporary:
        protocol_object = Path(temporary) / "hw1_ota_protocol.o"
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        # The protocol core is C; compile it as C and link the real object.
        subprocess.run([args.cxx, "-x", "c", "-std=c11", *warnings, *sanitize, *include, "-c",
                        str(PROTOCOL / "hw1_ota_protocol.c"), "-o", str(protocol_object)],
                       check=True, timeout=120)
        # System_OTA.cpp uses designated initializers (C++20; the firmware builds
        # with gnu++2b), so this unit is compiled as C++20.
        subprocess.run([args.cxx, "-std=c++20", *warnings, *sanitize, *include, str(unit),
                        str(protocol_object), "-o", str(binary)], check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0,
                f"OTA emergency recovery arm failed:\n{result.stdout}{result.stderr}")
        print(result.stdout.strip() + "; journal-policy source guards passed", flush=True)


if __name__ == "__main__":
    main()
