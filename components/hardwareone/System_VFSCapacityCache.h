#ifndef SYSTEM_VFS_CAPACITY_CACHE_H
#define SYSTEM_VFS_CAPACITY_CACHE_H

#include <stddef.h>
#include <stdint.h>

// Dependency-free state machine behind VFS's opt-in capacity snapshots.
// System_VFS.cpp supplies the FreeRTOS synchronization; keeping the cache core
// free of Arduino/ESP-IDF types lets the exact generation and expiry semantics
// run in the host test suite.
namespace VFS {

struct CapacitySnapshot {
  uint64_t totalBytes;
  uint64_t usedBytes;
  uint64_t freeBytes;
  uint32_t sampledAtMs;
};

namespace detail {

static constexpr size_t kCapacityTierCount = 2;

struct CapacityCacheSlot {
  CapacitySnapshot snapshot;
  uint32_t generation;
  bool valid;
};

class CapacitySnapshotCache {
 public:
  // maxAgeMs == 0 deliberately never hits: callers use that value to request
  // an authoritative filesystem sample. Unsigned subtraction is millis-wrap
  // safe, and age == maxAge is expired to match the former G2 cache contract.
  bool read(size_t tier, uint32_t now, uint32_t maxAgeMs,
            CapacitySnapshot* out) const {
    if (!out || tier >= kCapacityTierCount || maxAgeMs == 0) return false;
    const CapacityCacheSlot& slot = slots_[tier];
    if (!slot.valid || (uint32_t)(now - slot.snapshot.sampledAtMs) >= maxAgeMs) {
      return false;
    }
    *out = slot.snapshot;
    return true;
  }

  uint32_t generation(size_t tier) const {
    return tier < kCapacityTierCount ? slots_[tier].generation : 0;
  }

  // A refresh samples outside the cache lock. Publishing is conditional on the
  // generation captured before that sample, so an invalidation that lands
  // during a slow LittleFS walk cannot be overwritten by the older result.
  bool publish(size_t tier, uint32_t expectedGeneration,
               const CapacitySnapshot& snapshot) {
    if (tier >= kCapacityTierCount) return false;
    CapacityCacheSlot& slot = slots_[tier];
    if (slot.generation != expectedGeneration) return false;
    slot.snapshot = snapshot;
    slot.valid = true;
    return true;
  }

  void invalidate(size_t tier) {
    if (tier >= kCapacityTierCount) return;
    CapacityCacheSlot& slot = slots_[tier];
    slot.valid = false;
    slot.generation++;
  }

 private:
  CapacityCacheSlot slots_[kCapacityTierCount] = {};
};

}  // namespace detail
}  // namespace VFS

#endif  // SYSTEM_VFS_CAPACITY_CACHE_H
