#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "System_HeapLowCore.h"  // heap_low_core::fragPercent: the one fragmentation figure

// The summary numbers of `memreport`: one snapshot, one field table, and the
// two walks (text and json) that print it. No Arduino, ESP-IDF, FreeRTOS or
// ArduinoJson dependency, so the host tests run the same table and the same
// walks the device runs.
//
// Before this header the text form and the json form each kept their own list
// of numbers and each read the heap at its own moments, so one reply could
// disagree with itself and a number added to one form was missing in the
// other. Now:
//
//   - memReportGather (System_Utils.cpp, where the heap calls and the linker
//     symbols live) fills one MemReportSnapshot: one heap_caps_get_info per
//     pool, the static section sizes and the allocation tracker totals.
//   - kFields names every summary number once: its json group and key, its
//     text label, its unit and where it sits in the snapshot.
//   - writeJson and emitTextGroup walk kFields. A row added here appears in
//     both forms; neither has a list of its own.
//
// Text-only parts of the report (the top allocation tags and the per-task
// live attribution) are streamed by printMemoryReport and are not in the
// table.
namespace memreport_core {

// One moment of the device's memory. Every member is 64-bit so one member
// pointer type serves the whole table; flags are 0 or 1. Filled by the device
// (or a test) and then completed by derive().
struct MemReportSnapshot {
  // Internal 8-bit pool (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), one
  // heap_caps_get_info.
  uint64_t dramTotal;      // physical bytes, reserve pool counted once (hw1InternalTotalBytes)
  uint64_t dramSpanRaw;    // registered span as the allocator counts it (reserve pool twice)
  uint64_t dramFree;
  uint64_t dramAllocated;
  uint64_t dramMinFree;    // low-water mark since boot
  uint64_t dramLargest;    // largest free block
  // DMA-capable pool, one heap_caps_get_info.
  uint64_t dmaFree;
  uint64_t dmaLargest;
  // PSRAM: the chip size and one heap_caps_get_info of the SPIRAM pool.
  uint64_t psramAvailable; // flag
  uint64_t psramTotal;     // physical chip size (ESP.getPsramSize)
  uint64_t psramFree;
  uint64_t psramLargest;
  // Static sections, from the linker symbols.
  uint64_t bssInternal;
  uint64_t bssPsram;
  uint64_t noinitInternal;
  uint64_t noinitPsram;
  // Allocation tracker totals (cumulative requests since its reset, not live).
  uint64_t trafficAvailable;  // flag: the tracker snapshot was taken
  uint64_t trafficEnabled;    // flag
  uint64_t trafficGeneration;
  uint64_t trafficTags;
  uint64_t trafficCapacity;
  uint64_t trafficSuccesses;
  uint64_t trafficFailures;
  uint64_t trafficFallbacks;
  uint64_t trafficDramBytes;
  uint64_t trafficPsramBytes;
  uint64_t trafficContentionDrops;
  uint64_t trafficInvalidTagEvents;
  uint64_t trafficOverflowEvents;
  // Set by derive().
  uint64_t dramUsed;         // dramTotal - dramFree
  uint64_t dramPeakUsed;     // dramTotal - dramMinFree
  uint64_t allocatorMeta;    // dramSpanRaw - dramFree - dramAllocated
  uint64_t internalFragPct;  // heap_low_core::fragPercent(dramFree, dramLargest)
  uint64_t psramUsed;        // psramTotal - psramFree
  uint64_t trafficAttempts;  // successes + failures
};

inline constexpr uint64_t saturatingSub(uint64_t a, uint64_t b) {
  return a > b ? a - b : 0;
}

// part as a whole percent of total, rounded down. 0 when total is 0: a pool
// that reports no size (a board without it, a failed query) prints 0 %, it
// does not divide. The product is 64-bit; a part above 2^57 bytes is clamped
// instead of wrapping.
inline constexpr unsigned percentOf(uint64_t part, uint64_t total) {
  if (total == 0) return 0;
  const uint64_t limit = UINT64_MAX / 100u;
  if (part > limit) part = limit;
  return (unsigned)((part * 100u) / total);
}

// Fill the members that are computed from the measured ones. Differences
// saturate at 0: the figures of one pool come from one call, but the physical
// totals come from a second source and must not wrap if the two ever disagree.
inline void derive(MemReportSnapshot& s) {
  s.dramUsed = saturatingSub(s.dramTotal, s.dramFree);
  s.dramPeakUsed = saturatingSub(s.dramTotal, s.dramMinFree);
  // Per heap, span = control structures + allocated + free + 4 B per live
  // block, so span - free - allocated is the allocator's own bookkeeping.
  s.allocatorMeta = saturatingSub(s.dramSpanRaw, s.dramFree + s.dramAllocated);
  s.internalFragPct =
      heap_low_core::fragPercent((size_t)s.dramFree, (size_t)s.dramLargest);
  s.psramUsed = saturatingSub(s.psramTotal, s.psramFree);
  s.trafficAttempts = s.trafficSuccesses + s.trafficFailures;
}

// ── The field table ─────────────────────────────────────────────────────────

enum class Group : uint8_t { Dram, Psram, Sections, HeapCaps, Traffic };

struct GroupInfo {
  const char* jsonName;   // the json object the group's keys live in
  const char* textTitle;  // the heading line of the text form
};

inline constexpr GroupInfo kGroups[] = {
    {"dram", "-- DRAM (Internal Heap) --"},
    {"psram", "-- PSRAM (External) --"},
    {"sections", "-- STATIC SECTIONS --"},
    {"heapCaps", "-- HEAP CAPS (allocator-eye view) --"},
    {"allocationTraffic",
     "[1] ALLOCATION TRAFFIC (cumulative since tracker reset; not live):"},
};

inline constexpr const GroupInfo& groupInfo(Group g) { return kGroups[(size_t)g]; }

// What a value is. Bytes and Percent rows take a text line each; Count and
// Flag rows are short and share lines as label=value.
enum class Unit : uint8_t { Bytes, Percent, Count, Flag };

inline constexpr const char* unitText(Unit u) {
  return u == Unit::Bytes ? "B" : u == Unit::Percent ? "%" : "";
}

// When a row exists at all.
enum : uint8_t {
  kAlways = 0,
  kNeedsPsram = 1,    // only on a board with PSRAM
  kNeedsTraffic = 2,  // only when the tracker snapshot was taken
};

using Member = uint64_t MemReportSnapshot::*;

struct Field {
  Group group;
  const char* key;    // json key inside the group's object
  const char* label;  // text label, at most 18 characters
  Unit unit;
  Member value;
  Member shareOf;     // text only: prints "[NN%]" of this total; nullptr = none
  uint8_t needs;
};

// Existing json keys keep their name, place and meaning (docs/
// APP_JSON_CONTRACT.md, tools/p4/ram_monitor.py). Add rows; do not rename.
inline constexpr Field kFields[] = {
    // dram: byte-addressable internal RAM.
    {Group::Dram, "total", "Total", Unit::Bytes, &MemReportSnapshot::dramTotal, nullptr, kAlways},
    {Group::Dram, "used", "Used", Unit::Bytes, &MemReportSnapshot::dramUsed, &MemReportSnapshot::dramTotal, kAlways},
    {Group::Dram, "free", "Free", Unit::Bytes, &MemReportSnapshot::dramFree, &MemReportSnapshot::dramTotal, kAlways},
    {Group::Dram, "minFree", "Min free", Unit::Bytes, &MemReportSnapshot::dramMinFree, nullptr, kAlways},
    {Group::Dram, "peakUsed", "Peak used", Unit::Bytes, &MemReportSnapshot::dramPeakUsed, &MemReportSnapshot::dramTotal, kAlways},
    {Group::Dram, "allocatorMeta", "Allocator metadata", Unit::Bytes, &MemReportSnapshot::allocatorMeta, nullptr, kAlways},
    // psram
    {Group::Psram, "available", "Available", Unit::Flag, &MemReportSnapshot::psramAvailable, nullptr, kAlways},
    {Group::Psram, "total", "Total", Unit::Bytes, &MemReportSnapshot::psramTotal, nullptr, kNeedsPsram},
    {Group::Psram, "used", "Used", Unit::Bytes, &MemReportSnapshot::psramUsed, &MemReportSnapshot::psramTotal, kNeedsPsram},
    {Group::Psram, "free", "Free", Unit::Bytes, &MemReportSnapshot::psramFree, &MemReportSnapshot::psramTotal, kNeedsPsram},
    // sections: statically placed data.
    {Group::Sections, "bssInternal", "BSS (internal)", Unit::Bytes, &MemReportSnapshot::bssInternal, nullptr, kAlways},
    {Group::Sections, "bssPsram", "BSS (PSRAM)", Unit::Bytes, &MemReportSnapshot::bssPsram, nullptr, kAlways},
    {Group::Sections, "noinitInternal", "NOINIT (internal)", Unit::Bytes, &MemReportSnapshot::noinitInternal, nullptr, kAlways},
    {Group::Sections, "noinitPsram", "NOINIT (PSRAM)", Unit::Bytes, &MemReportSnapshot::noinitPsram, nullptr, kAlways},
    // heapCaps: where allocations can land and how fragmented each pool is.
    // internalFree is the same number as dram.free (one read serves both).
    {Group::HeapCaps, "internalFree", "Internal free", Unit::Bytes, &MemReportSnapshot::dramFree, nullptr, kAlways},
    {Group::HeapCaps, "internalLargest", "Internal largest", Unit::Bytes, &MemReportSnapshot::dramLargest, nullptr, kAlways},
    {Group::HeapCaps, "internalFragPct", "Internal frag", Unit::Percent, &MemReportSnapshot::internalFragPct, nullptr, kAlways},
    {Group::HeapCaps, "dmaFree", "DMA free", Unit::Bytes, &MemReportSnapshot::dmaFree, nullptr, kAlways},
    {Group::HeapCaps, "dmaLargest", "DMA largest", Unit::Bytes, &MemReportSnapshot::dmaLargest, nullptr, kAlways},
    {Group::HeapCaps, "spiramFree", "SPIRAM free", Unit::Bytes, &MemReportSnapshot::psramFree, nullptr, kNeedsPsram},
    {Group::HeapCaps, "spiramLargest", "SPIRAM largest", Unit::Bytes, &MemReportSnapshot::psramLargest, nullptr, kNeedsPsram},
    // allocationTraffic: cumulative requests since the tracker's reset.
    {Group::Traffic, "enabled", "Enabled", Unit::Flag, &MemReportSnapshot::trafficEnabled, nullptr, kNeedsTraffic},
    {Group::Traffic, "generation", "Generation", Unit::Count, &MemReportSnapshot::trafficGeneration, nullptr, kNeedsTraffic},
    {Group::Traffic, "tags", "Tags", Unit::Count, &MemReportSnapshot::trafficTags, nullptr, kNeedsTraffic},
    {Group::Traffic, "capacity", "Capacity", Unit::Count, &MemReportSnapshot::trafficCapacity, nullptr, kNeedsTraffic},
    {Group::Traffic, "attempts", "Attempts", Unit::Count, &MemReportSnapshot::trafficAttempts, nullptr, kNeedsTraffic},
    {Group::Traffic, "successes", "Successes", Unit::Count, &MemReportSnapshot::trafficSuccesses, nullptr, kNeedsTraffic},
    {Group::Traffic, "failures", "Failures", Unit::Count, &MemReportSnapshot::trafficFailures, nullptr, kNeedsTraffic},
    {Group::Traffic, "fallbacks", "Fallbacks", Unit::Count, &MemReportSnapshot::trafficFallbacks, nullptr, kNeedsTraffic},
    {Group::Traffic, "dramBytes", "DRAM requested", Unit::Bytes, &MemReportSnapshot::trafficDramBytes, nullptr, kNeedsTraffic},
    {Group::Traffic, "psramBytes", "PSRAM requested", Unit::Bytes, &MemReportSnapshot::trafficPsramBytes, nullptr, kNeedsTraffic},
    {Group::Traffic, "contentionDrops", "Lock drops", Unit::Count, &MemReportSnapshot::trafficContentionDrops, nullptr, kNeedsTraffic},
    {Group::Traffic, "invalidTagEvents", "Invalid tags", Unit::Count, &MemReportSnapshot::trafficInvalidTagEvents, nullptr, kNeedsTraffic},
    {Group::Traffic, "overflowEvents", "Registry full", Unit::Count, &MemReportSnapshot::trafficOverflowEvents, nullptr, kNeedsTraffic},
};

inline constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

inline bool fieldPresent(const Field& f, const MemReportSnapshot& s) {
  if ((f.needs & kNeedsPsram) && !s.psramAvailable) return false;
  if ((f.needs & kNeedsTraffic) && !s.trafficAvailable) return false;
  return true;
}

// ── json walk ───────────────────────────────────────────────────────────────

// Writes every present row as doc[group][key]. Doc is ArduinoJson's
// JsonDocument on the device; any type with that operator[] chain works.
// Flags are written as booleans, everything else as an unsigned integer.
template <typename Doc>
inline void writeJson(Doc& doc, const MemReportSnapshot& s,
                      const Field* fields = kFields, size_t count = kFieldCount) {
  for (size_t i = 0; i < count; ++i) {
    const Field& f = fields[i];
    if (!fieldPresent(f, s)) continue;
    const char* group = groupInfo(f.group).jsonName;
    if (f.unit == Unit::Flag) {
      doc[group][f.key] = (s.*(f.value) != 0);
    } else {
      doc[group][f.key] = (unsigned long long)(s.*(f.value));
    }
  }
}

// ── text walk ───────────────────────────────────────────────────────────────

// One broadcast record. Same size as BROADCAST_PRINTF's buffer: a record is
// at most 255 characters, and a remote `memreport` is carried one record at a
// time (the 16 KB received-output ring holds under two reports), so the walk
// packs a group's lines into as few records as fit instead of one per line.
inline constexpr size_t kEnvelopeBytes = 256;
// Longest line of label=value pairs before the walk starts a new one.
inline constexpr size_t kPairLineColumns = 96;

inline const char* reportTitle(bool atBoot) {
  return atBoot ? "\n========== BOOT MEMORY REPORT (Task Manager) =========="
                : "\n========== MEMORY REPORT (Task Manager) ==========";
}

inline const char* reportFooter() {
  return "\n========== END MEMORY REPORT ==========\n";
}

// One Bytes or Percent row as a text line.
inline void formatRowLine(const Field& f, const MemReportSnapshot& s,
                          char* out, size_t outSize) {
  const uint64_t v = s.*(f.value);
  if (f.unit == Unit::Percent) {
    snprintf(out, outSize, "  %-18s %9llu %%", f.label, (unsigned long long)v);
  } else if (f.shareOf) {
    snprintf(out, outSize, "  %-18s %9llu B (%6llu KB) [%2u%%]", f.label,
             (unsigned long long)v, (unsigned long long)(v / 1024u),
             percentOf(v, s.*(f.shareOf)));
  } else {
    snprintf(out, outSize, "  %-18s %9llu B (%6llu KB)", f.label,
             (unsigned long long)v, (unsigned long long)(v / 1024u));
  }
}

// One Count or Flag row as label=value.
inline void formatRowPair(const Field& f, const MemReportSnapshot& s,
                          char* out, size_t outSize) {
  const uint64_t v = s.*(f.value);
  if (f.unit == Unit::Flag) {
    snprintf(out, outSize, "%s=%s", f.label, v ? "yes" : "no");
  } else {
    snprintf(out, outSize, "%s=%llu", f.label, (unsigned long long)v);
  }
}

// Collects lines into records of at most kEnvelopeBytes - 1 characters and
// hands each to emit(const char*). An empty line is kept (it is the blank
// line before a heading).
template <typename Emit>
class EnvelopeWriter {
 public:
  explicit EnvelopeWriter(Emit& emit) : emit_(emit) {}
  ~EnvelopeWriter() { flush(); }
  EnvelopeWriter(const EnvelopeWriter&) = delete;
  EnvelopeWriter& operator=(const EnvelopeWriter&) = delete;

  void line(const char* text) {
    size_t n = strlen(text);
    if (n > kEnvelopeBytes - 1) n = kEnvelopeBytes - 1;  // one over-long line: cut, never overrun
    if (open_ && len_ + 1 + n > kEnvelopeBytes - 1) flush();
    if (open_) buf_[len_++] = '\n';
    memcpy(buf_ + len_, text, n);
    len_ += n;
    buf_[len_] = '\0';
    open_ = true;
  }

  void flush() {
    if (!open_) return;
    emit_(static_cast<const char*>(buf_));
    ++envelopes_;
    open_ = false;
    len_ = 0;
    buf_[0] = '\0';
  }

  unsigned envelopes() const { return envelopes_; }

 private:
  Emit& emit_;
  char buf_[kEnvelopeBytes] = {};
  size_t len_ = 0;
  bool open_ = false;
  unsigned envelopes_ = 0;
};

// Prints one group of the table: an optional lead line (the report title), a
// blank line, the group's heading and its present rows. Returns the number of
// records handed to emit. Uses about 400 bytes of the caller's stack.
template <typename Emit>
inline unsigned emitTextGroup(const MemReportSnapshot& s, Group group,
                              const char* lead, Emit&& emit,
                              const Field* fields = kFields,
                              size_t count = kFieldCount) {
  EnvelopeWriter<Emit> out(emit);
  if (lead) out.line(lead);
  out.line("");
  out.line(groupInfo(group).textTitle);

  char pairs[kPairLineColumns + 32];
  size_t pairsLen = 0;
  auto flushPairs = [&]() {
    if (pairsLen) out.line(pairs);
    pairsLen = 0;
  };

  char text[96];
  for (size_t i = 0; i < count; ++i) {
    const Field& f = fields[i];
    if (f.group != group || !fieldPresent(f, s)) continue;
    if (f.unit == Unit::Count || f.unit == Unit::Flag) {
      formatRowPair(f, s, text, sizeof(text));
      const size_t n = strlen(text);
      if (pairsLen && pairsLen + 2 + n > kPairLineColumns) flushPairs();
      // "  " leads the line and separates pairs alike.
      if (pairsLen + 2 + n < sizeof(pairs)) {
        pairs[pairsLen++] = ' ';
        pairs[pairsLen++] = ' ';
        memcpy(pairs + pairsLen, text, n + 1);
        pairsLen += n;
      }
    } else {
      flushPairs();
      formatRowLine(f, s, text, sizeof(text));
      out.line(text);
    }
  }
  flushPairs();
  out.flush();
  return out.envelopes();
}

}  // namespace memreport_core
