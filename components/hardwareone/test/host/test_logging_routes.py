#!/usr/bin/env python3
"""Log lines go through the project log unless a stated reason applies.

The rule (CONTRIBUTING.md, "Logging"; System_Logging.h has the short form):

1. The project log is the default: `DEBUG_<AREA>F`, `INFO_<AREA>F`,
   `WARN_<AREA>F`, `ERROR_<AREA>F` and `logSystemEvent` (System_Debug.h). Its
   lines follow the debug flags and the log level and reach the serial port,
   the web log and the log files.
2. `ESP_LOGx` writes to the serial port only. It stays for three kinds of line:
   a bench record that a tool or a test reads from the console, a path where
   the project log is not running yet or cannot be trusted (early boot, update
   safety, filesystem mount, memory diagnostics), and a driver unit that a host
   harness compiles without the project log.
3. `Serial.print`, `Serial.println`, `Serial.printf` and `Serial.write` stay in
   the setup wizards, the serial console itself, the writer of the project log
   and fatal paths before the log exists. `esp_rom_printf` stays in paths that
   run before or without the C library output.
4. `DEBUGF(flag, ...)` and `DEBUGF_BROADCAST(flag, ...)` are gone. A debug line
   uses the area macro of its flag.

Checks 2 and 3 are ratchets: the tables below hold the number of calls per
file. A file may not go above its number, and a file not listed has none. When
a file drops below its number the test asks for the smaller number, so the
tables stay exact. A new call of these kinds is added to its table on purpose,
with one of the reasons above. Check 4 fails on every use.

Reads the working tree. --component points the test at another component
directory; --print-allowances prints the tables for the current tree.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

IDF_LOG = re.compile(r"(?<![A-Za-z0-9_])ESP_(?:EARLY_|DRAM_)?LOG[EWIDV]\s*\(")
SERIAL_WRITE = re.compile(r"(?<![A-Za-z0-9_])Serial[0-9]?\s*\.\s*(?:print|println|printf|write)\s*\(")
ROM_PRINT = re.compile(r"(?<![A-Za-z0-9_])(?:esp_rom_printf|ets_printf)\s*\(")
REMOVED_MACRO = re.compile(r"(?<![A-Za-z0-9_])(DEBUGF|DEBUGF_BROADCAST)\s*\(")

# Calls per file. Regenerate with --print-allowances after removing some.
IDF_LOG_ALLOWED: dict[str, int] = {
    "BLE_CoexRecords.cpp": 5,
    "BLE_PhonePipe.cpp": 4,
    "BLE_SetupWorker.cpp": 2,
    "G2_Glasses.cpp": 12,
    "G2_Page_Transcription.cpp": 1,
    "HAL_Camera_P4.cpp": 3,
    "HAL_Display_P4Eye.cpp": 5,
    "HAL_Display_Waveshare.cpp": 5,
    "HAL_Radio.cpp": 16,      # C6 bring-up at boot, before the queued log runs; 16th: transport not up N ms after the start
    "HAL_SDCard.cpp": 4,
    "Input_GT911.cpp": 2,
    "System_Filesystem.cpp": 10,
    "System_LLMHttpTerminal.inc": 1,
    "System_LLMHttpTransport.cpp": 3,
    "System_MemDiag.cpp": 16,
    "System_MemUtil.cpp": 1,
    "System_OTA.cpp": 23,
    "System_OTASafety.cpp": 12,
    "System_RadioCompanion.cpp": 2,
    "System_STT.cpp": 38,     # TL bench trace lines; +4 for the phrase leveler and the glasses gain (TL phrasegain, TL g2autogain)
    "System_STTLocal.cpp": 22,
    "System_STTRawCapture.cpp": 2,
    "System_Utils.cpp": 2,    # rebootDevice: the SDMMC quiesce record, printed just before esp_restart(); a queued line would not drain
    "stt/quartznet_runtime.cpp": 5,
    "stt/citrinet_runtime.cpp": 7,   # QuartzNet's runtime's five model-load/resident-model/admission lines, the image-trim record (TL e3t) and the bench self-check (HW1_STT_SELFCHECK builds)
    "stt/stt_heap_watch.inc": 5,     # diagnostic builds only (HW1_STT_HEAP_WATCH): the low-point report
    "stt/stt_dl_metadata.cpp": 1,
    "stt/stt_internal_memory.cpp": 1,
}
SERIAL_WRITE_ALLOWED: dict[str, int] = {
    "HardwareOne.cpp": 19,
    "OLED_FirstTimeSetup.cpp": 20,
    "OLED_SetupWizard.cpp": 37,
    "System_Debug.cpp": 11,
    "System_Debug.h": 1,
    "System_FactoryWipe.cpp": 7,
    "System_Filesystem.cpp": 3,
    "System_I2C_Manager.cpp": 3,
    "System_Microphone.cpp": 4,
    "System_Mutex.cpp": 1,
    "System_Settings.cpp": 1,
    "System_SetupWizard.cpp": 62,   # the serial wizard asks the display label and the device name separately
}
ROM_PRINT_ALLOWED: dict[str, int] = {
    "System_CrashRecord.cpp": 13,
    "System_MemDiag.cpp": 10,
}

RATCHETS = (
    ("IDF_LOG_ALLOWED", IDF_LOG_ALLOWED, IDF_LOG,
     "ESP_LOGx", "use the area macros of System_Debug.h, or add the call to the table with its reason"),
    ("SERIAL_WRITE_ALLOWED", SERIAL_WRITE_ALLOWED, SERIAL_WRITE,
     "Serial write", "use the project log; Serial is for the wizards, the console and fatal paths"),
    ("ROM_PRINT_ALLOWED", ROM_PRINT_ALLOWED, ROM_PRINT,
     "esp_rom_printf", "use the project log; the ROM printer is for paths without the C library output"),
)


def measure(component: Path):
    counters = [Counter() for _ in RATCHETS]
    hard = []
    checked = 0
    for relative, text in sources(component, (".h", ".cpp", ".c", ".inc")):
        checked += 1
        code = blank_comments(text, keep_strings=False)
        for counter, (_, _, pattern, _, _) in zip(counters, RATCHETS):
            counter[relative] = len(pattern.findall(code))
        for match in REMOVED_MACRO.finditer(code):
            if code[:match.start()].rstrip().endswith("#define"):
                what = "is defined again"
            else:
                what = "is used"
            number = code.count("\n", 0, match.start()) + 1
            hard.append(f"{match.group(1)} {what}, use the area macro of the flag "
                        f"(DEBUG_<AREA>F in System_Debug.h)\n    {relative}:{number}")
    return checked, hard, counters


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--print-allowances", action="store_true")
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures, counters = measure(component)
    if not checked:
        raise SystemExit(f"logging routes: no sources under {component}")

    if args.print_allowances:
        for (name, _, _, _, _), counter in zip(RATCHETS, counters):
            print(f"{name}: dict[str, int] = {{")
            for relative in sorted(counter):
                if counter[relative]:
                    print(f'    "{relative}": {counter[relative]},')
            print("}")
        return

    totals = []
    for (name, allowed, _, what, advice), counter in zip(RATCHETS, counters):
        totals.append(f"{sum(counter.values())} {what}")
        for relative in sorted(set(counter) | set(allowed)):
            found, limit = counter.get(relative, 0), allowed.get(relative, 0)
            if found > limit:
                failures.append(f"{what}: {relative} has {found}, allowed {limit}; {advice}")
            elif found < limit:
                failures.append(f"{what}: {relative} has {found}, the table says {limit}; "
                                f"lower {name} to {found}")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"logging routes: {len(failures)} violation(s)")
    print(f"logging routes: {checked} source files, listed calls: {', '.join(totals)}")


if __name__ == "__main__":
    main()
