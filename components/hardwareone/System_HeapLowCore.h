#pragma once

#include <stddef.h>
#include <stdint.h>

// Dependency-light policy for the `heap_low` system event (G-71). No Arduino,
// ESP-IDF or FreeRTOS dependencies, so the host tests run the same decision the
// Tier 1 memory sample (periodicMemorySample in System_MemoryMonitor.cpp) runs.
//
// The device's internal 8-bit heap is "low" when its free total falls under
// kFreeBytes or its largest free block falls under kLargestBytes. One event is
// posted per low episode: the watch latches when it posts and re-arms only once
// both figures are back above their line plus kRearmMarginBytes, and never
// posts twice within kCooldownMs. A device that sits just under the line (the
// classic ESP32 with the G2 hijack page open runs at ~21 KB, by design) posts
// once, not every minute, so events.log and automations see the transition
// without a flash write per sample.
//
// The same header holds the two other internal-heap definitions every surface
// shares: the low-heap line (kFreeBytes) and the fragmentation figure with its
// warning line (fragPercent, fragWarn). The fragmentation pair is display
// only: it never enters isLow or update, so the heap_low event stays on
// kFreeBytes and kLargestBytes.
namespace heap_low_core {

// Same line as the [HEAP_PRESSURE] console warning (HEAP_WARNING_THRESHOLD)
// and the low-heap line of the DEBUG_MEMORY task report (System_TaskUtils.cpp).
inline constexpr size_t kFreeBytes = 25600;
// Largest contiguous block line: the 16 KB figure the G2 image viewers already
// use as their heap-low guard (g2ShowBmpFile in G2_ImageViewers.cpp).
inline constexpr size_t kLargestBytes = 16384;
inline constexpr size_t kRearmMarginBytes = 4096;
inline constexpr uint32_t kCooldownMs = 60000;

struct Watch {
  bool latched = false;     // posted for the current episode
  bool everPosted = false;  // lastPostMs is meaningful
  uint32_t lastPostMs = 0;
};

inline bool isLow(size_t freeBytes, size_t largestBytes) {
  return freeBytes < kFreeBytes || largestBytes < kLargestBytes;
}

// Feed one sample. Returns true when the caller should post heap_low now.
// nowMs may wrap; the cooldown uses unsigned subtraction.
inline bool update(Watch& watch, size_t freeBytes, size_t largestBytes, uint32_t nowMs) {
  if (!isLow(freeBytes, largestBytes)) {
    if (watch.latched && freeBytes >= kFreeBytes + kRearmMarginBytes &&
        largestBytes >= kLargestBytes + kRearmMarginBytes) {
      watch.latched = false;
    }
    return false;
  }
  if (watch.latched) return false;
  if (watch.everPosted && (uint32_t)(nowMs - watch.lastPostMs) < kCooldownMs) return false;
  watch.latched = true;
  watch.everPosted = true;
  watch.lastPostMs = nowMs;
  return true;
}

// Fragmentation of a heap pool, in percent: the share of its free bytes that
// lies outside the largest free block, 100 - largest * 100 / free with the
// quotient rounded down (the figure memreport prints). 0 means the free memory
// is one block. Both arguments must describe the same pool; the callers pass
// the internal 8-bit heap (hw1InternalFreeBytes, hw1InternalLargestBlock).
// That pool is several regions, so a healthy board does not read 0: bench
// memreport rows show 15-17 % on the ESP32-P4 and 38-48 % on the ESP32-S3.
//
// Returns 0 when free is 0, and when largest >= free: the two figures are read
// at two instants, so largest can exceed free, and unsigned arithmetic would
// otherwise wrap to a huge percentage. The product is 64-bit so a 32-bit size_t
// cannot overflow whatever pool is passed.
inline constexpr unsigned fragPercent(size_t freeBytes, size_t largestBytes) {
  if (freeBytes == 0 || largestBytes >= freeBytes) return 0;
  return 100u - (unsigned)(((uint64_t)largestBytes * 100u) / freeBytes);
}

// The one fragmentation warning line: memsample's "DRAM fragmentation" line
// and the "[!] FRAGMENTATION" alert of the DEBUG_MEMORY task report both print
// above it. 50 keeps the report's earlier rule (largest block under half of
// free); memsample used 30, which the ESP32-S3 bench figures above exceed.
inline constexpr unsigned kFragWarnPercent = 50;

inline constexpr bool fragWarn(size_t freeBytes, size_t largestBytes) {
  return fragPercent(freeBytes, largestBytes) > kFragWarnPercent;
}

}  // namespace heap_low_core
