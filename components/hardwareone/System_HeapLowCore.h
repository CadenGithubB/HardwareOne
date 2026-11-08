#pragma once

#include <stddef.h>
#include <stdint.h>

// Dependency-light policy for the `heap_low` system event. No Arduino,
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
namespace heap_low_core {

// Same line as the [HEAP_PRESSURE] console warning (HEAP_WARNING_THRESHOLD).
inline constexpr size_t kFreeBytes = 25600;
// Largest contiguous block line: the 16 KB figure the G2 image viewers already
// use as their heap-low guard (g2ShowBmpFile in G2_Glasses.cpp).
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

}  // namespace heap_low_core
