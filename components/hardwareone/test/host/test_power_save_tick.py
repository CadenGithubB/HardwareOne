#!/usr/bin/env python3
"""Exercise the production clock policy and local-panel ticks with controlled boundaries.

powerPolicyTick (System_Power.cpp) runs on every build; powerDisplayTick
(HardwareOne.cpp) only where there is a local display. Both are compiled from
the real sources. Each scenario runs in a fresh process so the actual
function-static state is never rewritten or replaced. Only clock admission, UI,
activity sources, workload busy flags and platform time are mocked.

Also checks, textually: the Settings default agrees with POWER_MODE_DEFAULT,
the main loop calls the policy outside the display gate, and the worst-case
`power json` fits its 1 KB buffers.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
# System_Power.h only tells the ESP32-P4 apart from every other target, so the
# S3 compiles the same code as the classic ESP32.
PROFILES = (
    ("esp32", []),
    ("p4", ["-DCONFIG_IDF_TARGET_ESP32P4=1"]),
    ("p4-old", ["-DCONFIG_IDF_TARGET_ESP32P4=1", "-DCONFIG_ESP32P4_SELECTS_REV_LESS_V3=1"]),
)
# Every build: the clock policy alone.
HEADLESS_SCENARIOS = (
    "normal", "boot_late", "timeout_zero", "remote_keeps_panel_dark", "adaptive_default", "already_idle",
    "boost_stt", "boost_camera", "boost_recording", "uart_fast", "uart_slow", "uart_stopped", "locked",
    "raise_refusal", "lower_refusal", "persistent_refusal", "foreign_change_refusal", "own_partial_failure",
    "manual_override", "mode_change_no_double_apply", "mode_change_during_boost", "cpufreq_in_locked",
    "partial_failure_then_in_use", "wrap_uptime", "wake_disabled",
)
# With a local display: the panel tick as well.
DISPLAY_SCENARIOS = HEADLESS_SCENARIOS + (
    "capture_holds_panel", "stt_does_not_hold_panel", "cooldown_keeps_panel_and_clock",
    "ultra_lit_panel", "wake_input",
)
JSON_BUFFER_BYTES = 1024   # cmd_power and handlePowerStatus both serialize into 1 KB


def check_sources(main: str, power: str, header: str) -> None:
    settings = (COMPONENT / "System_Settings.h").read_text()
    default = re.search(r"#define POWER_MODE_DEFAULT\s+(\w+)", header)
    require(default is not None, "System_Power.h must name POWER_MODE_DEFAULT")
    value = re.search(rf"#define {default.group(1)}\s+(\d+)", header)
    require(value is not None, f"{default.group(1)} must be a numbered mode")
    literal = re.search(r"(?m)^\s*powerMode\((\d+)\),", settings)
    require(literal is not None, "System_Settings.h constructor must initialize powerMode")
    require(literal.group(1) == value.group(1),
            f"Settings default powerMode({literal.group(1)}) != {default.group(1)} ({value.group(1)})")

    loop = extract_block(main, "void hardwareone_loop()")
    require(loop.count("powerPolicyTick();") == 1, "the main loop must run the clock policy once per lap")
    before = loop[:loop.index("powerPolicyTick();")]
    gate = before.rfind("#if ENABLE_OLED_DISPLAY")
    require(gate == -1 or "#endif" in before[gate:],
            "powerPolicyTick must run outside the ENABLE_OLED_DISPLAY gate")
    require(loop.count("powerDisplayTick();") == 1, "the main loop must run the panel tick once per lap")
    require(loop.index("powerDisplayTick();") < loop.index("powerPolicyTick();"),
            "the panel tick must run before the clock policy on each lap")
    require("powerSetCpuFrequencyMhz(" not in extract_block(main, "static void powerDisplayTick("),
            "the panel tick must not change the clock")


def worst_case_json_bytes(power: str) -> int:
    """Mirror buildPowerJson's keys with their widest values, minified as ArduinoJson emits them."""
    builder = extract_block(power, "void buildPowerJson(JsonDocument& doc)")
    keys = re.findall(r'doc\["(\w+)"\]', builder)
    widest = {
        "schema": 2, "mode": 4, "modeName": "Performance", "cpuMhz": 400, "targetMhz": 400,
        "boost": "recording", "xtalMhz": 40, "apbMhz": 80, "activeMhz": 400, "idleMhz": 400,
        "interactiveFloorMhz": 100, "maxMhz": 400, "supportedCpuMhz": [100, 200, 400],
        "autoMode": False, "autoModeSupported": False, "displayBrightness": 255, "displayDimLevel": 100,
        "powerSaveMinutes": 1440, "displayPowerSave": False, "localDisplayAwake": False,
        "idleMsAgo": 4294967295, "localIdleMsAgo": 4294967295, "cooldownMs": 60000,
        "cooldownRemainingMs": 60000, "otaProbation": False, "sleepAllowed": False,
        "modes": [{"index": i, "name": "Performance", "activeMhz": 400, "idleMhz": 400, "brightness": 100}
                  for i in range(5)],
    }
    missing = [key for key in keys if key not in widest]
    require(not missing, f"buildPowerJson emits keys this estimate does not size: {missing}")
    snapshot = {key: widest[key] for key in keys}
    return len(json.dumps(snapshot, separators=(",", ":")))


def generate(main: str, power: str) -> str:
    types = [extract_block(power, "struct PowerModeConfig") + ";",
             extract_block(power, "struct PowerDecision") + ";"]
    state = [extract_block(power, "static const PowerModeConfig gPowerModes[]") + ";"]
    for name in ("sPowerSaveLastActivityMs", "sPowerSaveLastLocalActivityMs", "sLocalDisplayAwake"):
        line = re.search(rf"(?m)^static volatile \w+(?: \w+)? {name}[^\n]+", power)
        require(line is not None, f"System_Power.cpp must keep {name} on one line")
        state.append(line[0])
    state.append(re.search(r"(?m)^#define UART_LINK_REF_TICK_BAUD_LIMIT[^\n]+",
                           (COMPONENT / "System_UartLink.h").read_text())[0])
    definitions = [extract_block(power, marker) for marker in (
        "uint32_t getPowerModeCpuFreq(", "uint32_t getPowerModeActiveCpuFreq(",
        "uint32_t getPowerModeIdleCpuFreq(", "void powerSaveNoteActivity(",
        "unsigned long powerSaveLastActivityMs(", "unsigned long powerSaveLastLocalActivityMs(",
        "const char* powerWorkloadBoost(", "void powerSetLocalDisplayAwake(",
        "bool powerLocalDisplayAwake(", "static PowerDecision powerDecide(",
        "uint32_t powerTargetCpuMhz(", "void powerPolicyTick(")]
    display = [extract_block(main, "static bool powerSaveInhibited()"),
               extract_block(main, "static void powerDisplayTick()")]
    functions = "\n\n".join(definitions) + "\n\n#if ENABLE_OLED_DISPLAY\n" + "\n\n".join(display) + "\n#endif\n"
    harness = (HERE / "power_save_tick_harness.cpp").read_text()
    for marker, code in {"// INSERT_TYPES": "\n".join(types),
                         "// INSERT_STATE": "\n".join(state),
                         "// INSERT_FUNCTIONS": functions}.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, code)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    main_source = (COMPONENT / "HardwareOne.cpp").read_text()
    power = (COMPONENT / "System_Power.cpp").read_text()
    header = (COMPONENT / "System_Power.h").read_text()
    check_sources(main_source, power, header)
    json_bytes = worst_case_json_bytes(power)
    require(json_bytes <= JSON_BUFFER_BYTES - 64,
            f"worst-case power JSON is {json_bytes} B; the 1 KB buffers need at least 64 B of headroom")

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    runs = 0
    with tempfile.TemporaryDirectory(prefix="hw1-power-save-tick-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(generate(main_source, power))
        for profile, target_flags in PROFILES:
            for display in (0, 1):
                for uart in (0, 1):
                    for stt in (0, 1):
                        binary = Path(directory) / f"test-{profile}-{display}-{uart}-{stt}"
                        subprocess.run([args.cxx, *flags, *target_flags,
                                        f"-DENABLE_OLED_DISPLAY={display}",
                                        f"-DSOC_UART_SUPPORT_REF_TICK={uart}",
                                        f"-DENABLE_LOCAL_STT={stt}", "-I", str(HERE / "i2c_stubs"),
                                        "-I", str(COMPONENT), str(source), "-o", str(binary)],
                                       check=True, timeout=60)
                        for scenario in DISPLAY_SCENARIOS if display else HEADLESS_SCENARIOS:
                            subprocess.run([str(binary), scenario], check=True, timeout=15)
                            runs += 1
    print(f"Power policy + panel ticks: {runs} fresh-process scenarios passed across ESP32/P4 revisions, "
          f"display on/off and UART/STT gates; worst-case power JSON {json_bytes} B of {JSON_BUFFER_BYTES}")


if __name__ == "__main__":
    main()
