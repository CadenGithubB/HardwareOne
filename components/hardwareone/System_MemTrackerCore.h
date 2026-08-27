#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Dependency-light allocation-tracker state machine.  This file deliberately
// has no Arduino, ESP-IDF, FreeRTOS, logging, or allocator dependencies so the
// real registry/hash/top-K logic can be exercised by host tests.

inline constexpr size_t kMemTrackerTagBytes = 32;

struct MemTrackerEntry {
  char tag[kMemTrackerTagBytes];
  uint64_t successCount;
  uint64_t failureCount;
  uint64_t fallbackCount;
  uint64_t dramBytes;
  uint64_t psramBytes;
};

struct MemTrackerSnapshot {
  bool initialized;
  bool enabled;
  uint32_t generation;
  uint32_t resetAtMs;
  size_t entryCount;
  size_t capacity;
  size_t topCount;
  uint64_t successCount;
  uint64_t failureCount;
  uint64_t fallbackCount;
  uint64_t dramBytes;
  uint64_t psramBytes;
  uint64_t contentionDrops;
  uint64_t invalidTagEvents;
  uint64_t overflowEvents;
};

namespace hw1_memtracker_detail {

inline uint64_t entryBytes(const MemTrackerEntry& entry) {
  return entry.dramBytes + entry.psramBytes;
}

inline bool ranksBefore(const MemTrackerEntry& lhs,
                        const MemTrackerEntry& rhs) {
  const uint64_t lhsBytes = entryBytes(lhs);
  const uint64_t rhsBytes = entryBytes(rhs);
  if (lhsBytes != rhsBytes) return lhsBytes > rhsBytes;
  if (lhs.successCount != rhs.successCount) {
    return lhs.successCount > rhs.successCount;
  }
  return strcmp(lhs.tag, rhs.tag) < 0;
}

inline uint32_t hashTag(const char* tag, size_t length) {
  uint32_t hash = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint8_t>(tag[i]);
    hash *= 16777619u;
  }
  return hash;
}

template <size_t Capacity>
class Registry {
 public:
  static_assert(Capacity > 0, "allocation tracker needs at least one slot");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "allocation tracker capacity must be a power of two");

  void reset(uint32_t generation, uint32_t resetAtMs) {
    memset(entries_, 0, sizeof(entries_));
    entryCount_ = 0;
    successCount_ = 0;
    failureCount_ = 0;
    fallbackCount_ = 0;
    dramBytes_ = 0;
    psramBytes_ = 0;
    overflowEvents_ = 0;
    generation_ = generation;
    resetAtMs_ = resetAtMs;
  }

  // The caller validates tag length and serializes access.  Returns false only
  // when a new tag cannot be attributed because the fixed registry is full;
  // aggregate traffic counters still include that event.
  bool record(const char* tag, size_t tagLength, size_t size,
              bool success, bool usedPS, bool fellBack) {
    if (success) {
      ++successCount_;
      if (usedPS) {
        psramBytes_ += static_cast<uint64_t>(size);
      } else {
        dramBytes_ += static_cast<uint64_t>(size);
      }
      if (fellBack) ++fallbackCount_;
    } else {
      ++failureCount_;
    }

    const size_t mask = Capacity - 1;
    size_t slotIndex = static_cast<size_t>(hashTag(tag, tagLength)) & mask;
    for (size_t probe = 0; probe < Capacity; ++probe) {
      MemTrackerEntry& entry = entries_[slotIndex];
      if (entry.tag[0] == '\0') {
        memcpy(entry.tag, tag, tagLength);
        entry.tag[tagLength] = '\0';
        ++entryCount_;
        updateEntry(entry, size, success, usedPS, fellBack);
        return true;
      }
      if (strcmp(entry.tag, tag) == 0) {
        updateEntry(entry, size, success, usedPS, fellBack);
        return true;
      }
      slotIndex = (slotIndex + 1) & mask;
    }

    ++overflowEvents_;
    return false;
  }

  void snapshot(MemTrackerSnapshot& out, bool initialized, bool enabled,
                uint64_t contentionDrops, uint64_t invalidTagEvents,
                MemTrackerEntry* topEntries, size_t topCapacity) const {
    out = {};
    out.initialized = initialized;
    out.enabled = enabled;
    out.generation = generation_;
    out.resetAtMs = resetAtMs_;
    out.entryCount = entryCount_;
    out.capacity = Capacity;
    out.successCount = successCount_;
    out.failureCount = failureCount_;
    out.fallbackCount = fallbackCount_;
    out.dramBytes = dramBytes_;
    out.psramBytes = psramBytes_;
    out.contentionDrops = contentionDrops;
    out.invalidTagEvents = invalidTagEvents;
    out.overflowEvents = overflowEvents_;

    if (!topEntries || topCapacity == 0) return;
    memset(topEntries, 0, topCapacity * sizeof(*topEntries));

    size_t topCount = 0;
    for (size_t i = 0; i < Capacity; ++i) {
      const MemTrackerEntry& candidate = entries_[i];
      if (candidate.tag[0] == '\0') continue;

      if (topCount < topCapacity) {
        topEntries[topCount++] = candidate;
      } else if (ranksBefore(candidate, topEntries[topCount - 1])) {
        topEntries[topCount - 1] = candidate;
      } else {
        continue;
      }

      size_t pos = topCount - 1;
      while (pos > 0 && ranksBefore(topEntries[pos], topEntries[pos - 1])) {
        const MemTrackerEntry tmp = topEntries[pos - 1];
        topEntries[pos - 1] = topEntries[pos];
        topEntries[pos] = tmp;
        --pos;
      }
    }
    out.topCount = topCount;
  }

 private:
  static void updateEntry(MemTrackerEntry& entry, size_t size,
                          bool success, bool usedPS, bool fellBack) {
    if (success) {
      ++entry.successCount;
      if (usedPS) {
        entry.psramBytes += static_cast<uint64_t>(size);
      } else {
        entry.dramBytes += static_cast<uint64_t>(size);
      }
      if (fellBack) ++entry.fallbackCount;
    } else {
      ++entry.failureCount;
    }
  }

  MemTrackerEntry entries_[Capacity];
  size_t entryCount_;
  uint64_t successCount_;
  uint64_t failureCount_;
  uint64_t fallbackCount_;
  uint64_t dramBytes_;
  uint64_t psramBytes_;
  uint64_t overflowEvents_;
  uint32_t generation_;
  uint32_t resetAtMs_;
};

}  // namespace hw1_memtracker_detail
