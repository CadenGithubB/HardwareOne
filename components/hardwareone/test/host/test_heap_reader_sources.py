#!/usr/bin/env python3
"""One meaning for the internal heap's free, minimum and fragmentation figures.

Source contracts for the readers that have no harness of their own: the BLE
system stream, the task-create debug line, the DEBUG_MEMORY task report,
memsample and the Tier 1 pressure watch. Each must read the byte-addressable
internal heap (System_MemUtil.h) and take the low-heap line, the fragmentation
figure and its warning line from System_HeapLowCore.h. The arithmetic itself is
tested in test_heap_low_core.cpp.
"""
import argparse
import re
from pathlib import Path

from test_web_batch_handlers import extract_block

COMPONENT = Path(__file__).resolve().parents[2]
ARDUINO_HEAP = re.compile(r"ESP\s*\.\s*get(Free|MinFree|MaxAlloc)Heap\s*\(|ESP\s*\.\s*getHeapSize\s*\(")


def code_only(block: str) -> str:
    """Drop // comments so a comment may name what the code no longer calls."""
    return "\n".join(line.split("//", 1)[0] for line in block.splitlines())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", type=Path, default=COMPONENT)
    component = parser.parse_args().component

    ble = (component / "BLE_Server.cpp").read_text()
    tasks = (component / "System_TaskUtils.cpp").read_text()
    monitor = (component / "System_MemoryMonitor.cpp").read_text()

    # BLE system stream: internal 8-bit figures, the same keys in the same order.
    stream = code_only(extract_block(ble, "static void buildSystemStatusJson("))
    assert not ARDUINO_HEAP.search(stream), "BLE system stream reads the Arduino heap calls"
    assert "hw1InternalFreeBytes()" in stream and "hw1InternalMinFreeBytes()" in stream
    assert (r'{\"system\":{\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,'
            r'\"uptime\":%lu},\"ts\":%lu}') in stream, "BLE system stream keys or order changed"

    # Task-create debug line: the heap trio comes from the internal reads.
    create = code_only(extract_block(tasks, "BaseType_t xTaskCreateLogged("))
    assert not ARDUINO_HEAP.search(create), "task-create line reads the Arduino heap calls"
    assert "heapBefore = internalBefore" in create and "heapAfter = internalAfter" in create
    for field in (' heapBefore=', ' heapAfter=', ' heapDelta=', ' internalBefore=', ' internalAfter='):
        assert field in create, f"task-create line lost the field{field}"

    # DEBUG_MEMORY task report: one pool, the shared lines, no literal of its own.
    report = code_only(extract_block(tasks, "void reportAllTaskStacks()"))
    assert not ARDUINO_HEAP.search(report), "task report reads the Arduino heap calls"
    assert "hw1InternalFreeBytes()" in report and "hw1InternalLargestBlock()" in report
    assert "40960" not in report and "40KB" not in report, "task report keeps its own low-heap line"
    assert "heapFree < heap_low_core::kFreeBytes" in report
    assert report.count("heap_low_core::fragPercent(") == 1
    assert report.count("heap_low_core::fragWarn(") == 1
    assert "heapFree / 2" not in report and "* 100) / heapFree" not in report, \
        "task report computes fragmentation on its own"

    # memsample and Tier 1: one [HEAP_PRESSURE] line, one min_ever source.
    assert monitor.count('"[HEAP_PRESSURE]') == 1, "more than one [HEAP_PRESSURE] format"
    helper = code_only(extract_block(monitor, "static void printHeapPressure("))
    assert "hw1InternalMinFreeBytes()" in helper and "gLowestHeapSeen" not in helper
    assert "threshold=%u, min_ever=%u" in helper
    sample = code_only(extract_block(monitor, "void sampleMemoryState(bool forceFullScan)"))
    tier1 = code_only(extract_block(monitor, "void periodicMemorySample()"))
    assert sample.count("printHeapPressure(dramFree)") == 1
    assert tier1.count("printHeapPressure(dramFree)") == 1
    assert "heap_low_core::fragWarn(dramFree, dramLargest)" in sample
    assert "heap_low_core::fragPercent(dramFree, dramLargest)" in sample
    assert "fragPct" not in sample, "memsample computes fragmentation on its own"
    # The poll's own low mark stays: it drives the "New DRAM low" debug line.
    assert "gLowestHeapSeen = dramFree" in sample and "gLowestHeapSeen = dramFree" in tier1

    print("heap reader sources: BLE stream, task-create line, task report, memsample and Tier 1 share one definition")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
