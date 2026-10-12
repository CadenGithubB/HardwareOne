#!/usr/bin/env python3
"""Exercise the complete production power-save tick with controlled boundaries.

Each scenario runs in a fresh process so the actual function-static state is
never rewritten or replaced. Clock policy and activity tracking are production
code; only clock admission, UI, activity sources and platform time are mocked.
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


def generate() -> str:
    main = (COMPONENT / "HardwareOne.cpp").read_text()
    power = (COMPONENT / "System_Power.cpp").read_text()
    types = extract_block(power, "struct PowerModeConfig") + ";"
    state = [extract_block(power, "static const PowerModeConfig gPowerModes[]") + ";",
             re.search(r"(?m)^static volatile unsigned long sPowerSaveLastActivityMs[^\n]+", power)[0],
             re.search(r"(?m)^#define UART_LINK_REF_TICK_BAUD_LIMIT[^\n]+",
                       (COMPONENT / "System_UartLink.h").read_text())[0]]
    definitions = [extract_block(power, marker) for marker in (
        "uint32_t getPowerModeCpuFreq(", "uint32_t getPowerModeActiveCpuFreq(",
        "uint32_t getPowerModeIdleCpuFreq(", "void powerSaveNoteActivity(",
        "unsigned long powerSaveLastActivityMs(")]
    definitions += [extract_block(main, "static bool powerSaveInhibited()"),
                    extract_block(main, "static void powerSaveTick()")]
    harness = (HERE / "power_save_tick_harness.cpp").read_text()
    for marker, code in {"// INSERT_TYPES": types,
                         "// INSERT_STATE": "\n".join(state),
                         "// INSERT_FUNCTIONS": "\n\n".join(definitions)}.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, code)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    scenarios = ("normal", "idle_refusal", "persistent_refusal", "foreign_change_refusal", "own_partial_failure",
                 "wake_input", "wake_command", "wake_disabled", "wake_stt", "locked",
                 "already_idle", "uart_fast", "uart_slow", "uart_stopped", "inhibited")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-power-save-tick-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(generate())
        for uart in (0, 1):
            for stt in (0, 1):
                binary = Path(directory) / f"test-{uart}-{stt}"
                subprocess.run([args.cxx, *flags, f"-DSOC_UART_SUPPORT_REF_TICK={uart}",
                                f"-DENABLE_LOCAL_STT={stt}", "-I", str(HERE / "i2c_stubs"),
                                "-I", str(COMPONENT), str(source), "-o", str(binary)],
                               check=True, timeout=60)
                for scenario in scenarios:
                    subprocess.run([str(binary), scenario], check=True, timeout=15)
    print("Power-save tick: 60 fresh-process scenarios passed across UART/STT gates")


if __name__ == "__main__":
    main()
