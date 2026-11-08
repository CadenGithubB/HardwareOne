#!/usr/bin/env python3
"""The P4 CPU clock switch runs its frozen-cache window from internal RAM only.

Bench 2026-11-08 (P4X-EYE, image f9d931bb): 2 of 3 reboots during `stt start`
ended in rst:0x7 (HP_SYS_HP_WDT_RESET) before the reboot command ran, with
Core0 saved at _vector_table and Core1 in esp_ipc_isr_waiting_for_finish_cmd.
The failed STT start raised the clock 100 -> 400 MHz and dropped it again.
p4SwitchCpuClock (System_Power.cpp) is `static IRAM_ATTR`, but GCC inlined it
into powerSetCpuFrequencyMhz, which is in flash: the instructions between
esp_cache_freeze_ext_mem_cache and esp_cache_unfreeze_ext_mem_cache were
fetched from flash with the cache frozen, the other core stalled and this
core's interrupts masked. On the P4 such a miss never completes.

Default mode compiles the shipping p4SwitchCpuClock and powerSetCpuFrequencyMhz
on the host against fakes of the IDF calls (power_p4_clock_switch_harness.cpp):
call order and state, the configuration read from the switch's own static copy
whatever the caller's stack, and a refusal for a stack outside internal DRAM.
It also checks the declaration in the source.

--placement cross-compiles the same production code with ESP-IDF's
riscv32-esp-elf-g++ and the build's P4 flags at -Os and -O2 (object files only,
no firmware build) and reads the relocations: the stall, freeze, set and
unfreeze calls must sit in a .iram1 function named p4SwitchCpuClock, not in
the flash owner, and the window must reference no flash data. It exits 77
(skipped) without the cross compiler or an ESP-IDF tree.

--source-dir points the suite at another copy of the component (the pre-fix
tree fails both modes).
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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
WINDOW = ("esp_ipc_isr_stall_other_cpu", "esp_cache_freeze_ext_mem_cache",
          "rtc_clk_cpu_freq_set_config_fast", "esp_cache_unfreeze_ext_mem_cache")


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def production(src: Path) -> tuple[str, str]:
    power = (src / "System_Power.cpp").read_text()
    start = power.index("static portMUX_TYPE sP4ClockSwitchMux")
    head = re.search(r"^static[^\n;{]*\bp4SwitchCpuClock\(", power[start:], re.M)
    require(head is not None, "System_Power.cpp defines p4SwitchCpuClock")
    marker = head.group(0)
    switch = power[start:start + head.start()] + extract_block(power[start:], marker) + "\n"
    owner_start = power.index("static StaticSemaphore_t sCpuFrequencyMutexStorage;")
    owner_head = power.index("bool powerSetCpuFrequencyMhz(uint32_t mhz, bool* outClockChanged) {")
    owner = power[owner_start:owner_head] + extract_block(power, "bool powerSetCpuFrequencyMhz(") + "\n"
    return switch, owner


def source_checks(switch: str) -> None:
    plain = strip_comments(switch)
    decl = plain[plain.index("p4SwitchCpuClock(") - 80:plain.index("p4SwitchCpuClock(")]
    decl = decl[decl.rfind("static"):]
    require("IRAM_ATTR" in decl, "p4SwitchCpuClock is IRAM_ATTR")
    require("NOINLINE_ATTR" in decl or "noinline" in decl,
            "p4SwitchCpuClock is NOINLINE_ATTR (an inlined copy runs from the caller's flash)")
    window = plain[plain.index("esp_cache_freeze_ext_mem_cache();") + len("esp_cache_freeze_ext_mem_cache();"):
                   plain.index("esp_cache_unfreeze_ext_mem_cache();")]
    calls = re.findall(r"\b(\w+)\s*\(", window)
    require(calls == ["rtc_clk_cpu_freq_set_config_fast"], f"only the clock set runs while frozen, not {calls}")
    require(re.search(r"static DRAM_ATTR rtc_cpu_freq_config_t (\w+);", plain) is not None,
            "the window's configuration is an internal-RAM static")


def stub_headers(work: Path) -> None:
    (work / "Arduino.h").write_text("#pragma once\n#include <cstdint>\n#include <cstddef>\n")


def behaviour(args, switch: str, owner: str) -> None:
    harness = (HERE / "power_p4_clock_switch_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "harness marker")
    code = "#define HW1_SYNC_MSPI_WITH_CPU_CLOCK 1\n#define ENABLE_I2C_SYSTEM 0\n" + harness.replace(
        "// INSERT_PRODUCTION", "#if HW1_SYNC_MSPI_WITH_CPU_CLOCK\n" + switch + "#endif\n" + owner)
    with tempfile.TemporaryDirectory(prefix="hw1-p4-clock-") as tmp:
        work = Path(tmp)
        stub_headers(work)
        unit = work / "test.cpp"
        unit.write_text(code)
        exe = work / "test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-Wno-unused-parameter", "-DCONFIG_IDF_TARGET_ESP32P4=1", "-I" + str(work),
                   "-I" + str(args.source_dir), str(unit), "-o", str(exe)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


PLACEMENT_PROLOGUE = r"""
#include "sdkconfig.h"
#include "esp_attr.h"
#include <cstdint>
#include <cstddef>
#define HW1_SYNC_MSPI_WITH_CPU_CLOCK 1
#define ENABLE_I2C_SYSTEM 0
struct hal_utils_clk_div_t { uint32_t integer, denominator, numerator; };
struct rtc_cpu_freq_config_t { int source; uint32_t source_freq_mhz; hal_utils_clk_div_t div; uint32_t freq_mhz; };
extern "C" {
bool rtc_clk_cpu_freq_mhz_to_config(uint32_t, rtc_cpu_freq_config_t*);
void rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t*);
void esp_clk_utils_mspi_speed_mode_sync_before_cpu_freq_switching(uint32_t, uint32_t);
void esp_clk_utils_mspi_speed_mode_sync_after_cpu_freq_switching(uint32_t, uint32_t);
void esp_ipc_isr_stall_other_cpu(void);
void esp_ipc_isr_release_other_cpu(void);
void esp_cache_freeze_ext_mem_cache(void);
void esp_cache_unfreeze_ext_mem_cache(void);
void vPortEnterCriticalSafe(int*);
void vPortExitCriticalSafe(int*);
}
// esp_cpu.h / esp_memory_utils.h (P4): the same inline forms.
static inline __attribute__((always_inline)) void* esp_cpu_get_sp() {
  void* sp; asm volatile("mv %0, sp" : "=r"(sp)); return sp; }
inline static bool esp_ptr_in_dram(const void* p) {
  return ((intptr_t)p >= 0x4ff00000 && (intptr_t)p < 0x4ffc0000); }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_SAFE(m) vPortEnterCriticalSafe(m)
#define portEXIT_CRITICAL_SAFE(m) vPortExitCriticalSafe(m)
struct StaticSemaphore_t { int x[20]; };
typedef StaticSemaphore_t* SemaphoreHandle_t;
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
bool displayRequiresFullSpeed();
uint32_t getCpuFrequencyMhz();
bool setCpuFrequencyMhz(uint32_t);
#define POWER_CPU_PERFORMANCE_MHZ 400
"""


def find_flags(idf: Path) -> list[str]:
    return ["-mabi=ilp32f", "-march=rv32imafc_zicsr_zifencei_xesploop_xespv", "-fno-rtti", "-fno-exceptions",
            "-ffunction-sections", "-fdata-sections", "-freorder-blocks", "-fstack-protector",
            "-fstrict-volatile-bitfields", "-fno-jump-tables", "-fno-tree-switch-conversion", "-std=gnu++2b"]


def placement(args, switch: str, owner: str) -> int:
    cxx = args.cross_cxx or shutil.which("riscv32-esp-elf-g++")
    idf = args.idf
    if not cxx or not idf or not (Path(idf) / "components/esp_common/include/esp_attr.h").is_file():
        print("SKIP: --placement needs riscv32-esp-elf-g++ and an ESP-IDF 5.5.5 tree (export.sh)")
        return 77
    objdump = Path(cxx).with_name("riscv32-esp-elf-objdump")
    require(objdump.is_file(), f"{objdump} missing")
    with tempfile.TemporaryDirectory(prefix="hw1-p4-clock-iram-") as tmp:
        work = Path(tmp)
        (work / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_IDF_TARGET_ESP32P4 1\n")
        unit = work / "power.cpp"
        unit.write_text(PLACEMENT_PROLOGUE + switch + owner)
        for opt in ("-Os", "-O2"):
            obj = work / f"power{opt}.o"
            subprocess.run([cxx, opt, *find_flags(Path(idf)), "-I" + str(work),
                            "-I" + str(Path(idf) / "components/esp_common/include"),
                            "-c", str(unit), "-o", str(obj)], check=True, timeout=120)
            dump = subprocess.run([str(objdump), "-dr", "-C", str(obj)], check=True, capture_output=True,
                                  text=True).stdout
            if os.environ.get("HW1_DUMP"):
                print(dump)
            sections: dict[str, list[str]] = {}
            name = ""
            for line in dump.splitlines():
                m = re.match(r"Disassembly of section (\S+):", line)
                if m:
                    name = m.group(1)
                    sections[name] = []
                    continue
                r = re.search(r"\bR_RISCV_(\w+)\s+(.+)$", line)
                if r and name and r.group(1) != "RELAX":   # a linker-relaxation marker, no target
                    sections[name].append(r.group(2).strip())
            holders = {sec for sec, relocs in sections.items() if any(w in relocs for w in WINDOW)}
            require(holders and all(sec.startswith(".iram1") for sec in holders),
                    f"{opt}: the frozen-cache window is in {sorted(holders)}, not only IRAM "
                    "(p4SwitchCpuClock was inlined into flash code)")
            require(re.search(r"<p4SwitchCpuClock\b[^>]*>:", dump) is not None,
                    f"{opt}: p4SwitchCpuClock exists as its own function")
            owner_secs = [sec for sec in sections if "powerSetCpuFrequencyMhz" in sec]
            require(owner_secs and all(not sec.startswith(".iram1") for sec in owner_secs),
                    f"{opt}: the owner stays in flash ({owner_secs})")
            for sec in holders:
                relocs = sections[sec]
                first = relocs.index("esp_cache_freeze_ext_mem_cache")
                last = relocs.index("esp_cache_unfreeze_ext_mem_cache")
                inside = [r for r in relocs[first + 1:last] if r not in ("rtc_clk_cpu_freq_set_config_fast",)]
                # Only the internal-RAM configuration copy (hi/lo halves of its address).
                bad = [r for r in inside if not re.match(r"^(\.dram1\.\d+|\.L\w+|.*sP4ClockTarget.*)(\+0x[0-9a-f]+)?$", r)]
                require(not bad, f"{opt}: the frozen window references {bad}")
    print("PASS: p4SwitchCpuClock is a separate IRAM function holding the whole frozen-cache window (-Os, -O2)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-dir", "--component", dest="source_dir", type=Path, default=COMPONENT)
    parser.add_argument("--placement", action="store_true", help="cross-compile and check code placement")
    parser.add_argument("--cross-cxx", help="riscv32-esp-elf-g++ (default: on PATH)")
    parser.add_argument("--idf", default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    switch, owner = production(args.source_dir)
    if args.placement:
        return placement(args, switch, owner)
    source_checks(switch)
    behaviour(args, switch, owner)
    return 0


if __name__ == "__main__":
    sys.exit(main())
