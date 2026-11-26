#!/usr/bin/env python3
"""The system metrics sampler reads cached figures only.

systemMetricsTick() runs on the main loop (System_Metrics.cpp). Source
contract, nothing is compiled:

- the sampler's sources (comments and string literals blanked) never call the
  C6 memory RPC (radioHalQueryMemory), a fresh filesystem stat (VFS::getStats,
  esp_littlefs_info, LittleFS/SD capacity getters, statvfs, f_getfree) or
  WiFi.RSSI(); nor walk the heap again for the largest block, take their own
  task snapshot, or start a task;
- nor anything that takes the filesystem lock: VFS::getStatsSnapshot (it
  locks and samples on a cache miss, whatever the max age), VFS::isSdAvailable,
  FsLockGuard. Storage figures come from the cache-only
  VFS::peekStatsSnapshot;
- allocation is PSRAM-only (ps_alloc with AllocPolicy::RequirePsram), no raw
  libc allocation;
- the main loop ticks it exactly once, right after periodicMemorySample();
- the component builds System_Metrics.cpp, and System_MemoryMonitor exports the
  Tier 1 largest block the sampler reuses;
- every SystemMetricId gets its raw reading in takeSample (an id left out
  would be read uninitialised);
- the CPU clock (CLK) comes from getCpuFrequencyMhz() (the clock registers,
  O(1)), the sampler never sets the clock, and its chart top is the chip's top
  clock per target (400 MHz P4, 240 ESP32/S3, 160 C6).

--component points the suite at another component directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

SAMPLER_FILES = ("System_Metrics.cpp", "System_Metrics.h", "System_MetricsCore.h")

BANNED = {
    r"\bradioHalQueryMemory\s*\(": "C6 memory RPC",
    r"\b(?:RSSI|wifiStaRssi)\s*\(": "wifiStaRssi() / WiFi.RSSI() (blocking RPC on the P4)",
    r"\bVFS::getStats\s*\(": "fresh VFS stat",
    r"(?<![\w:])getStats\s*\(": "fresh VFS stat",
    r"\besp_littlefs_info\s*\(": "fresh LittleFS stat",
    r"\bLittleFS\s*\.\s*(?:totalBytes|usedBytes|info)\s*\(": "fresh LittleFS stat",
    r"\bSD(?:_MMC)?\s*\.\s*(?:totalBytes|usedBytes|cardSize)\s*\(": "fresh SD stat",
    r"\bstatvfs\s*\(": "fresh filesystem stat",
    r"\bf_getfree\s*\(": "fresh FAT stat",
    r"\bheap_caps_get_largest_free_block\s*\(": "second heap walk",
    r"\bhw1InternalLargestBlock\s*\(": "second heap walk",
    r"\bheap_caps_check_integrity\w*\s*\(": "heap walk",
    r"\bgetStatsSnapshot\s*\(": "VFS::getStatsSnapshot (locks and samples on a cache miss)",
    r"\bisSdAvailable\s*\(": "VFS::isSdAvailable (takes the filesystem lock)",
    r"\bFsLockGuard\b": "filesystem lock",
    r"\buxTaskGetSystemState\s*\(": "own task snapshot (use taskCpuShares)",
    r"\bxTaskCreate\w*\s*\(": "new task",
    r"(?<![\w.])(?:malloc|calloc|realloc)\s*\(": "raw allocation",
    r"\bsetCpuFrequencyMhz\s*\(": "clock change (System_Power owns the clock)",
    r"\brtc_clk_cpu_freq_set\w*\s*\(": "clock change (System_Power owns the clock)",
}


def blank_comments_and_strings(source: str) -> str:
    out = []
    i, n = 0, len(source)
    while i < n:
        if source.startswith("//", i):
            j = source.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in source[i:j]))
            i = j
        elif source[i] in "\"'":
            quote = source[i]
            j = i + 1
            while j < n and source[j] != quote:
                j += 2 if source[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(quote + " " * max(0, j - i - 2) + (quote if j - i >= 2 else ""))
            i = j
        else:
            out.append(source[i])
            i += 1
    return "".join(out)


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def check(component: Path) -> list[str]:
    errors: list[str] = []
    for name in SAMPLER_FILES:
        path = component / name
        if not path.is_file():
            errors.append(f"{name}: missing")
            continue
        code = blank_comments_and_strings(path.read_text(encoding="utf-8"))
        for pattern, why in BANNED.items():
            for m in re.finditer(pattern, code):
                errors.append(f"{name}:{line_of(code, m.start())}: {why}: {m.group(0).strip()}")
        for m in re.finditer(r"\bps_alloc\s*\(", code):
            stmt = code[m.end(): code.find(";", m.end())]
            if "RequirePsram" not in stmt:
                errors.append(f"{name}:{line_of(code, m.start())}: ps_alloc without "
                              "AllocPolicy::RequirePsram")

    cpp = component / "System_Metrics.cpp"
    if cpp.is_file():
        code = blank_comments_and_strings(cpp.read_text(encoding="utf-8"))
        if "VFS::peekStatsSnapshot(" not in code:
            errors.append("System_Metrics.cpp: storage figures no longer come from the "
                          "cache-only snapshot")
        if "memoryMonitorInternalLargestBlock" not in code:
            errors.append("System_Metrics.cpp: largest block no longer reuses the Tier 1 reading")
        if "taskCpuShares" not in code:
            errors.append("System_Metrics.cpp: CPU no longer comes from the shared sampler")
        errors += check_metric_coverage(component, code)
        errors += check_clock(cpp.read_text(encoding="utf-8"), code)

    main = component / "HardwareOne.cpp"
    code = blank_comments_and_strings(main.read_text(encoding="utf-8"))
    ticks = [m.start() for m in re.finditer(r"\bsystemMetricsTick\s*\(\s*\)\s*;", code)]
    if len(ticks) != 1:
        errors.append(f"HardwareOne.cpp: systemMetricsTick() called {len(ticks)} times, want 1")
    else:
        loop = code.find("void hardwareone_loop()")
        sample = code.find("periodicMemorySample();", loop)
        if loop < 0 or sample < 0 or not (loop < sample < ticks[0]):
            errors.append("HardwareOne.cpp: systemMetricsTick() is not in hardwareone_loop "
                          "after periodicMemorySample()")
        else:
            between = code[sample + len("periodicMemorySample();"):ticks[0]]
            if between.strip():
                errors.append("HardwareOne.cpp: systemMetricsTick() does not follow "
                              "periodicMemorySample() directly")

    cmake = (component / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(r"^\s+System_Metrics\.cpp\s*$", cmake, re.MULTILINE):
        errors.append("CMakeLists.txt: System_Metrics.cpp is not in the source list")

    header = (component / "System_MemoryMonitor.h").read_text(encoding="utf-8")
    if "bool memoryMonitorInternalLargestBlock(" not in header:
        errors.append("System_MemoryMonitor.h: the Tier 1 largest-block getter is gone")
    return errors


def check_metric_coverage(component: Path, code: str) -> list[str]:
    core = blank_comments_and_strings(
        (component / "System_MetricsCore.h").read_text(encoding="utf-8"))
    enum = re.search(r"enum\s+class\s+SystemMetricId\s*:\s*uint8_t\s*\{(.*?)\};", core, re.S)
    if not enum:
        return ["System_MetricsCore.h: SystemMetricId enum not found"]
    ids = re.findall(r"^\s*(\w+)\s*=\s*\d+\s*,", enum.group(1), re.M)
    count = re.search(r"\bkMetricCount\s*=\s*(\d+)\s*;", core)
    errors = []
    if not count or int(count.group(1)) != len(ids):
        errors.append(f"System_MetricsCore.h: kMetricCount is not the {len(ids)} ids of SystemMetricId")
    take = code.find("void takeSample(")
    body = code[take:code.find("portENTER_CRITICAL", take)] if take >= 0 else ""
    filled = set(re.findall(r"raw\[smc::index\(SystemMetricId::(\w+)\)\]", body))
    for name in ids:
        if name not in filled:
            errors.append(f"System_Metrics.cpp: takeSample does not read SystemMetricId::{name}")
    return errors


def check_clock(raw: str, code: str) -> list[str]:
    errors = []
    m = re.search(r"Raw\s+readClock\s*\(\s*\)\s*\{(.*?)\n\}", code, re.S)
    if not m or not re.search(r"\bgetCpuFrequencyMhz\s*\(\s*\)", m.group(1)):
        errors.append("System_Metrics.cpp: readClock does not read getCpuFrequencyMhz()")
    elif "unavailable()" in m.group(1):
        errors.append("System_Metrics.cpp: the CPU clock is never unavailable")
    elif "kChipMaxCpuMhz" not in m.group(1):
        errors.append("System_Metrics.cpp: readClock's chart top is not kChipMaxCpuMhz")
    want = {"ESP32P4": 400, "ESP32S3": 240, "ESP32": 240, "ESP32C6": 160}
    for target, mhz in want.items():
        branch = re.search(r"#(?:el)?if\b[^\n]*(?:\\\n[^\n]*)*\bCONFIG_IDF_TARGET_" + target +
                           r"\b[^\n]*(?:\\\n[^\n]*)*\n\s*constexpr\s+uint32_t\s+kChipMaxCpuMhz\s*=\s*(\d+)\s*;",
                           raw)
        if not branch or int(branch.group(1)) != mhz:
            errors.append(f"System_Metrics.cpp: kChipMaxCpuMhz for {target} is not {mhz}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    errors = check(args.component.resolve())
    for e in errors:
        print(e, file=sys.stderr)
    if errors:
        print(f"system_metrics_source: {len(errors)} problem(s)", file=sys.stderr)
        return 1
    print("system_metrics_source: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
