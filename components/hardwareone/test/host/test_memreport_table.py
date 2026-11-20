#!/usr/bin/env python3
"""memreport: one snapshot and one field table behind the text and json forms.

Part 1 compiles System_MemReportCore.h with the real vendored ArduinoJson and
runs the two production walks (writeJson, emitTextGroup):

  - every row of kFields appears in the json document and in the text;
  - a row added to a table (the production rows plus one) appears in both
    forms with no other change, and a row limited to PSRAM boards or to a
    taken tracker snapshot is absent from both when that does not apply;
  - a snapshot whose totals are all zero prints 0 % and no division is
    executed (built with -fsanitize=undefined, traps on integer division by
    zero; arm64 would otherwise return 0 silently);
  - differences saturate instead of wrapping, and fragmentation is
    heap_low_core::fragPercent;
  - the heading says MEMORY REPORT for the command and BOOT MEMORY REPORT at
    boot;
  - the summary takes at most 6 records with PSRAM and 5 without (10 and 6
    before this change) and no record exceeds 255 characters, also with the
    largest values the fields can hold;
  - the json keys documented before this change are all still there with
    the same values, and tools/p4/ram_monitor.py accepts the reply.

Part 2 reads the shipping sources: cmd_memreport's json branch and the
summary part of the text form call the walks and name no summary key or
label of their own; the gather calls heap_caps_get_info once per pool and
never the separate free/largest calls; the boot entry keeps its signature;
the OLED Memory page reads no figure it does not draw and uses the guarded
percent.

Not covered: the heap calls themselves, the streamed per-task attribution,
the cost of the SPIRAM walk, or anything on a device.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

# The json keys of docs/APP_JSON_CONTRACT.md before this change, plus the
# allocationTraffic object the code already sent.
OLD_KEYS = {
    "dram": ["total", "used", "free", "minFree", "peakUsed"],
    "psram": ["available", "total", "used", "free"],
    "heapCaps": ["internalFree", "internalLargest", "dmaFree", "dmaLargest"],
    "allocationTraffic": ["enabled", "generation", "tags", "capacity", "attempts",
                          "successes", "failures", "fallbacks", "dramBytes", "psramBytes",
                          "contentionDrops", "invalidTagEvents", "overflowEvents"],
}
NEW_KEYS = {
    "dram": ["allocatorMeta"],
    "heapCaps": ["internalFragPct", "spiramFree", "spiramLargest"],
    "sections": ["bssInternal", "bssPsram", "noinitInternal", "noinitPsram"],
}

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "System_MemReportCore.h"

namespace mr = memreport_core;

static mr::MemReportSnapshot typical(bool psram, bool traffic) {
  mr::MemReportSnapshot s{};
  s.dramTotal = 500000;
  s.dramSpanRaw = 532768;   // reserve pool registered twice
  s.dramFree = 240000;
  s.dramAllocated = 255000;
  s.dramMinFree = 180000;
  s.dramLargest = 200000;
  s.dmaFree = 230000;
  s.dmaLargest = 190000;
  if (psram) {
    s.psramAvailable = 1;
    s.psramTotal = 33554432;
    s.psramFree = 20000000;
    s.psramLargest = 19000000;
  }
  s.bssInternal = 61000;
  s.bssPsram = 3100000;
  s.noinitInternal = 512;
  s.noinitPsram = 0;
  if (traffic) {
    s.trafficAvailable = 1;
    s.trafficEnabled = 1;
    s.trafficGeneration = 3;
    s.trafficTags = 41;
    s.trafficCapacity = 64;
    s.trafficSuccesses = 9000;
    s.trafficFailures = 2;
    s.trafficFallbacks = 1;
    s.trafficDramBytes = 123456;
    s.trafficPsramBytes = 98765432;
    s.trafficContentionDrops = 4;
    s.trafficInvalidTagEvents = 0;
    s.trafficOverflowEvents = 5;
  }
  mr::derive(s);
  return s;
}

struct Text {
  std::vector<std::string> records;
  std::string all;
};

static Text textOf(const mr::MemReportSnapshot& s, bool atBoot,
                   const mr::Field* fields, size_t count) {
  Text t;
  auto emit = [&t](const char* record) {
    assert(std::strlen(record) <= 255);
    t.records.push_back(record);
    t.all += record;
    t.all += "\n";
  };
  mr::emitTextGroup(s, mr::Group::Dram, mr::reportTitle(atBoot), emit, fields, count);
  mr::emitTextGroup(s, mr::Group::Psram, nullptr, emit, fields, count);
  mr::emitTextGroup(s, mr::Group::Sections, nullptr, emit, fields, count);
  mr::emitTextGroup(s, mr::Group::HeapCaps, nullptr, emit, fields, count);
  return t;
}

static std::string jsonOf(const mr::MemReportSnapshot& s, const mr::Field* fields,
                          size_t count) {
  JsonDocument doc;
  doc["schema"] = 1;
  mr::writeJson(doc, s, fields, count);
  std::string out;
  serializeJson(doc, out);
  return out;
}

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The text line or label=value pair a row produces.
static std::string rowText(const mr::Field& f, const mr::MemReportSnapshot& s) {
  char buf[128];
  if (f.unit == mr::Unit::Count || f.unit == mr::Unit::Flag) {
    mr::formatRowPair(f, s, buf, sizeof(buf));
    return std::string("  ") + buf;
  }
  mr::formatRowLine(f, s, buf, sizeof(buf));
  return buf;
}

int main(int argc, char** argv) {
  static_assert(mr::percentOf(1, 0) == 0, "zero total is 0 %, not a division");
  static_assert(mr::percentOf(0, 0) == 0, "");
  static_assert(mr::percentOf(50, 200) == 25, "");
  static_assert(mr::percentOf(199, 200) == 99, "rounded down");
  static_assert(mr::percentOf(UINT64_MAX, UINT64_MAX) <= 100, "no wrap");
  static_assert(mr::saturatingSub(3, 5) == 0 && mr::saturatingSub(5, 3) == 2, "");

  // ── every production row is in both forms ───────────────────────────────
  for (int psram = 0; psram <= 1; ++psram) {
    for (int traffic = 0; traffic <= 1; ++traffic) {
      const mr::MemReportSnapshot s = typical(psram != 0, traffic != 0);
      JsonDocument doc;
      mr::writeJson(doc, s);
      Text text = textOf(s, false, mr::kFields, mr::kFieldCount);
      auto emit = [&text](const char* record) {
        assert(std::strlen(record) <= 255);
        text.all += record;
        text.all += "\n";
      };
      mr::emitTextGroup(s, mr::Group::Traffic, nullptr, emit);
      for (const mr::Field& f : mr::kFields) {
        const char* group = mr::groupInfo(f.group).jsonName;
        const bool present = mr::fieldPresent(f, s);
        assert(present == !doc[group][f.key].isNull());
        // Labels repeat across groups ("Total"), so look for the whole row.
        if (present) assert(has(text.all, rowText(f, s)));
        if (present && f.unit == mr::Unit::Flag) {
          assert(doc[group][f.key].is<bool>());
        } else if (present) {
          assert(doc[group][f.key].as<unsigned long long>() == s.*(f.value));
        }
        assert(std::strlen(f.label) <= 18);
      }
      // A group with no present row has no json object.
      assert(doc["allocationTraffic"].isNull() == (traffic == 0));
      assert(doc["psram"]["available"].as<bool>() == (psram != 0));
      assert(doc["psram"]["total"].isNull() == (psram == 0));
      assert(doc["heapCaps"]["spiramLargest"].isNull() == (psram == 0));
      // Summary records: 10 with PSRAM and 6 without before this change.
      const Text summary = textOf(s, false, mr::kFields, mr::kFieldCount);
      assert(summary.records.size() <= (psram ? 6u : 5u));
    }
  }

  // ── a row added to the table appears in both forms ──────────────────────
  {
    std::vector<mr::Field> table(mr::kFields, mr::kFields + mr::kFieldCount);
    table.push_back({mr::Group::HeapCaps, "addedForTheTest", "Added for test",
                     mr::Unit::Bytes, &mr::MemReportSnapshot::bssPsram, nullptr,
                     mr::kAlways});
    table.push_back({mr::Group::Psram, "addedPsramOnly", "Added PSRAM only",
                     mr::Unit::Count, &mr::MemReportSnapshot::psramLargest, nullptr,
                     mr::kNeedsPsram});
    const mr::MemReportSnapshot withPsram = typical(true, true);
    const mr::MemReportSnapshot without = typical(false, true);

    const std::string before = jsonOf(withPsram, mr::kFields, mr::kFieldCount);
    assert(!has(before, "addedForTheTest"));
    assert(!has(textOf(withPsram, false, mr::kFields, mr::kFieldCount).all, "Added for test"));

    const std::string after = jsonOf(withPsram, table.data(), table.size());
    assert(has(after, "\"addedForTheTest\":3100000"));
    assert(has(after, "\"addedPsramOnly\":19000000"));
    const Text text = textOf(withPsram, false, table.data(), table.size());
    assert(has(text.all, "  Added for test       3100000 B (  3027 KB)"));
    assert(has(text.all, "Added PSRAM only=19000000"));

    const std::string afterNoPsram = jsonOf(without, table.data(), table.size());
    assert(has(afterNoPsram, "\"addedForTheTest\":3100000"));
    assert(!has(afterNoPsram, "addedPsramOnly"));
    const Text textNoPsram = textOf(without, false, table.data(), table.size());
    assert(has(textNoPsram.all, "Added for test"));
    assert(!has(textNoPsram.all, "Added PSRAM only"));
  }

  // ── zero totals: no division, 0 % ───────────────────────────────────────
  {
    mr::MemReportSnapshot zero{};
    zero.psramAvailable = 1;  // PSRAM rows present with a total of 0
    zero.trafficAvailable = 1;
    mr::derive(zero);
    assert(zero.dramUsed == 0 && zero.dramPeakUsed == 0 && zero.psramUsed == 0);
    assert(zero.internalFragPct == 0 && zero.allocatorMeta == 0);
    const Text text = textOf(zero, false, mr::kFields, mr::kFieldCount);
    assert(has(text.all, "  Used                       0 B (     0 KB) [ 0%]"));
    assert(has(text.all, "  Free                       0 B (     0 KB) [ 0%]"));
    assert(has(text.all, "  Internal frag              0 %"));
    const std::string json = jsonOf(zero, mr::kFields, mr::kFieldCount);
    assert(has(json, "\"dram\":{\"total\":0,\"used\":0,\"free\":0"));

    // Free above total (two sources disagree): saturates, never wraps.
    mr::MemReportSnapshot odd{};
    odd.dramTotal = 1000;
    odd.dramFree = 4000;
    odd.dramMinFree = 5000;
    odd.dramLargest = 6000;  // larger than free: fragmentation reads 0
    odd.dramSpanRaw = 10;
    odd.dramAllocated = 50;
    odd.psramAvailable = 1;
    odd.psramTotal = 10;
    odd.psramFree = 20;
    mr::derive(odd);
    assert(odd.dramUsed == 0 && odd.dramPeakUsed == 0 && odd.psramUsed == 0);
    assert(odd.allocatorMeta == 0 && odd.internalFragPct == 0);
  }

  // ── derived figures ─────────────────────────────────────────────────────
  {
    const mr::MemReportSnapshot s = typical(true, true);
    assert(s.dramUsed == 260000 && s.dramPeakUsed == 320000);
    assert(s.allocatorMeta == 532768 - 240000 - 255000);
    assert(s.internalFragPct ==
           heap_low_core::fragPercent((size_t)240000, (size_t)200000));
    assert(s.internalFragPct == 17);
    assert(s.psramUsed == 13554432 && s.trafficAttempts == 9002);
    const Text text = textOf(s, false, mr::kFields, mr::kFieldCount);
    assert(has(text.all, "  Total                 500000 B (   488 KB)\n"));
    assert(has(text.all, "  Used                  260000 B (   253 KB) [52%]"));
    assert(has(text.all, "  Free                  240000 B (   234 KB) [48%]"));
    assert(has(text.all, "  Peak used             320000 B (   312 KB) [64%]"));
    assert(has(text.all, "  Internal frag             17 %"));
    assert(has(text.all, "  SPIRAM largest      19000000 B ( 18554 KB)"));
    assert(has(text.all, "  Available=yes"));
    assert(has(text.all, "-- STATIC SECTIONS --"));
  }

  // ── heading: command vs boot ────────────────────────────────────────────
  {
    const mr::MemReportSnapshot s = typical(true, false);
    const Text command = textOf(s, false, mr::kFields, mr::kFieldCount);
    const Text boot = textOf(s, true, mr::kFields, mr::kFieldCount);
    assert(has(command.records[0], "========== MEMORY REPORT (Task Manager) =========="));
    assert(!has(command.all, "BOOT"));
    assert(has(boot.records[0], "========== BOOT MEMORY REPORT (Task Manager) =========="));
    assert(has(command.records[0], "-- DRAM (Internal Heap) --"));
    assert(has(mr::reportFooter(), "END MEMORY REPORT"));
  }

  // ── record sizes with the largest values ────────────────────────────────
  {
    mr::MemReportSnapshot big{};
    std::memset(&big, 0xFF, sizeof(big));  // every member UINT64_MAX
    unsigned records = 0;
    size_t longest = 0;
    auto emit = [&](const char* record) {
      const size_t n = std::strlen(record);
      assert(n <= 255);
      if (n > longest) longest = n;
      ++records;
    };
    for (mr::Group g : {mr::Group::Dram, mr::Group::Psram, mr::Group::Sections,
                        mr::Group::HeapCaps, mr::Group::Traffic}) {
      mr::emitTextGroup(big, g, g == mr::Group::Dram ? mr::reportTitle(true) : nullptr, emit);
    }
    assert(records > 0 && longest > 0);

    // One over-long line is cut, not overrun.
    std::string huge(600, 'x');
    unsigned cut = 0;
    auto emitCut = [&](const char* record) {
      assert(std::strlen(record) <= 255);
      ++cut;
    };
    {
      mr::EnvelopeWriter<decltype(emitCut)> out(emitCut);
      out.line("first");
      out.line(huge.c_str());
      out.line("");
      out.line("last");
    }
    assert(cut == 3);
  }

  // Hand the typical reply to the Python side (ram_monitor, key lists).
  if (argc > 1) {
    const bool psram = std::strcmp(argv[1], "psram") == 0;
    std::puts(jsonOf(typical(psram, true), mr::kFields, mr::kFieldCount).c_str());
    return 0;
  }
  std::puts("memreport core: one table behind both forms; added rows appear in "
            "both; zero totals print 0 %; records fit");
  return 0;
}
'''


def check_sources() -> None:
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    core = (COMPONENT / "System_MemReportCore.h").read_text()

    # Host-buildable core: no device headers.
    for forbidden in ("Arduino.h", "esp_", "freertos", "ArduinoJson"):
        require(f"#include <{forbidden}" not in core and f'#include "{forbidden}' not in core,
                f"System_MemReportCore.h must not include {forbidden}")
    require("heap_low_core::fragPercent(" in core, "fragmentation comes from heap_low_core")

    # The json branch walks the table and names no summary key itself.
    command = extract_block(utils, "const char* cmd_memreport(const String& argsInput)")
    require(command.count("memreport_core::writeJson(doc, snap)") == 1,
            "cmd_memreport must write the json form through the table walk")
    require(command.count("memReportGather(") == 1, "one gather per json reply")
    require('doc["schema"] = 1;' in command, "schema stays 1")
    for group, keys in {**OLD_KEYS, **NEW_KEYS}.items():
        for key in keys + [group]:
            require(f'"{key}"' not in command,
                    f'cmd_memreport still names the summary key "{key}" by hand')
    for call in ("heap_caps_get", "hw1Internal", "ESP.get", "memTrackerSnapshot("):
        require(call not in command, f"cmd_memreport must not read memory itself ({call})")
    require("printMemoryReportImpl(false);" in command,
            "the command prints the report without the boot heading")

    # The boot entry keeps its signature (HardwareOne.cpp and System_Utils.h
    # are not changed) and is the only caller with the boot heading.
    boot = extract_block(utils, "void printMemoryReport()")
    require("printMemoryReportImpl(true);" in boot, "boot entry prints the boot heading")
    require(utils.count("printMemoryReportImpl(true)") == 1, "one boot-heading caller")
    require("void printMemoryReport();" in (COMPONENT / "System_Utils.h").read_text(),
            "boot entry declaration")
    require("MEMORY REPORT (Task Manager)" not in utils,
            "the heading text lives in the core only")

    # The text form: one gather, the summary through the table walk.
    text = extract_block(utils, "static void printMemoryReportImpl(bool atBoot)")
    require(text.count("memReportGather(") == 1, "one gather per text report")
    summary = text[:text.index("-- MEMORY BREAKDOWN")]
    for group in ("Dram", "Psram", "Sections", "HeapCaps"):
        require(summary.count(f"memreport_core::Group::{group}") == 1,
                f"summary group {group} goes through emitTextGroup once")
    require("memreport_core::reportTitle(atBoot)" in summary, "heading from the core")
    require("BROADCAST_PRINTF" not in summary, "no hand-printed summary row")
    require(text.count("memreport_core::Group::Traffic") == 1, "traffic totals from the table")
    require("* 100) /" not in summary and "*100)/" not in summary,
            "no unguarded percent in the summary")
    for call in ("heap_caps_get", "hw1Internal", "ESP.getPsram", "ESP.getFreePsram",
                 "memTrackerSnapshot("):
        require(call not in text, f"the text form must not read memory itself ({call})")

    # The gather: one get_info per pool, no separate free/largest reads.
    gather = extract_block(utils, "static void memReportGather(")
    for caps in ("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", "MALLOC_CAP_DMA", "MALLOC_CAP_SPIRAM"):
        require(gather.count(f"heap_caps_get_info(&info, {caps})") == 1,
                f"one heap_caps_get_info for {caps}")
    require(gather.count("heap_caps_get_info(") == 3, "three pools, three walks")
    for call in ("heap_caps_get_free_size(", "heap_caps_get_largest_free_block(",
                 "heap_caps_get_minimum_free_size(", "hw1InternalFreeBytes(",
                 "hw1InternalLargestBlock(", "hw1InternalMinFreeBytes(", "ESP.getFreePsram("):
        require(call not in gather, f"gather reads a pool a second time: {call}")
    # Meaning of the existing totals.
    require("snap.dramTotal     = hw1InternalTotalBytes();" in gather,
            "dram.total stays the reserve-adjusted physical total")
    require("snap.psramTotal = ESP.getPsramSize();" in gather,
            "psram.total stays the chip size")
    require("memreport_core::derive(snap);" in gather, "derived figures from the core")

    # OLED Memory page.
    oled = (COMPONENT / "OLED_Mode_System.cpp").read_text()
    prepare = extract_block(oled, "void prepareMemoryData()")
    for call in ("hw1InternalLargestBlock(", "hw1InternalMinFreeBytes(", "heap_caps_get",
                 "memReportGather", "MemReportSnapshot"):
        require(call not in prepare, f"OLED Memory page must not call {call}")
    require(prepare.count("memreport_core::percentOf(") == 2, "both bars use the guarded percent")
    require("* 100) /" not in prepare, "no unguarded division on the OLED Memory page")
    data = extract_block(oled, "struct MemoryRenderData")
    require("minFreeHeap" not in data and "largestBlock" not in data,
            "the two undrawn figures are gone from the render data")


def check_reply(binary: Path) -> None:
    spec = importlib.util.spec_from_file_location(
        "ram_monitor", REPO / "tools" / "p4" / "ram_monitor.py")
    ram_monitor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ram_monitor)
    for arg, psram in (("psram", True), ("nopsram", False)):
        line = subprocess.run([str(binary), arg], check=True, capture_output=True,
                              text=True).stdout.strip()
        require(line.startswith('{"schema":1,"dram":{'), "schema then dram lead the reply")
        reply = json.loads(line)
        for group, keys in OLD_KEYS.items():
            expect = keys if (psram or group != "psram") else ["available"]
            require(list(reply[group])[:len(expect)] == expect or
                    all(k in reply[group] for k in expect),
                    f"existing keys of {group} are all present")
            for key in expect:
                require(key in reply[group], f"{group}.{key} is still sent")
        for group, keys in NEW_KEYS.items():
            for key in keys:
                gone = (not psram) and key.startswith("spiram")
                require((key in reply[group]) != gone, f"new key {group}.{key}")
        require(reply["dram"]["free"] == reply["heapCaps"]["internalFree"],
                "dram.free and heapCaps.internalFree are one read")
        require(reply["dram"]["used"] == reply["dram"]["total"] - reply["dram"]["free"], "used")
        require(reply["psram"]["available"] is psram, "psram.available is a boolean")
        require(reply["allocationTraffic"]["enabled"] is True, "enabled is a boolean")
        sample = ram_monitor.parse_sample("noise " + line, 12.5)
        require(sample is not None, "tools/p4/ram_monitor.py accepts the reply")
        require(sample["internalTotal"] == 500000 and sample["internalFree"] == 240000 and
                sample["internalMin"] == 180000 and sample["internalLargest"] == 200000,
                "ram_monitor reads the same four internal figures")
        require(sample["psramTotal"] == (33554432 if psram else 0), "ram_monitor psram total")
        require(len(line) < 1024, "reply stays far below the 4 KB reply buffer")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    check_sources()
    with tempfile.TemporaryDirectory(prefix="hw1-memreport-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(HARNESS)
        subprocess.run(
            [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
             "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
             "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
             "-DARDUINOJSON_ENABLE_STD_STRING=1",
             "-I", str(COMPONENT), "-I", str(JSON_INCLUDE), str(source), "-o", str(binary)],
            check=True, timeout=300)
        subprocess.run([str(binary)], check=True, timeout=60)
        check_reply(binary)
    print("memreport: sources use the one table; ram_monitor accepts the reply")


if __name__ == "__main__":
    main()
