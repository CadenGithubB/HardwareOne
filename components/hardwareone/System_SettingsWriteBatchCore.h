// System_SettingsWriteBatchCore.h - owner-scoped settings-write batching.
//
// The firmware adapter supplies synchronization and performs filesystem I/O.
// Keeping this state machine free of Arduino, FreeRTOS, and heap-backed types
// lets the exact ownership and retry policy run in the native host suite.
#ifndef SYSTEM_SETTINGS_WRITE_BATCH_CORE_H
#define SYSTEM_SETTINGS_WRITE_BATCH_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace hw1_settings_batch {

// A value identity rather than a pointer identity: queued commands may outlive
// the request object that assigned their owner. `domain` distinguishes ingress
// kinds/sessions; `token` distinguishes requests within one domain.
struct Owner {
  uint64_t domain = 0;
  uint64_t token = 0;

  constexpr bool operator==(const Owner& other) const {
    return domain == other.domain && token == other.token;
  }

  constexpr bool operator!=(const Owner& other) const {
    return !(*this == other);
  }
};

// Settings and debug settings are persisted to different files. A batch only
// flushes the files it actually dirtied.
static constexpr uint8_t MAIN = static_cast<uint8_t>(1u << 0);
static constexpr uint8_t DEBUG = static_cast<uint8_t>(1u << 1);
static constexpr uint8_t ALL = static_cast<uint8_t>(MAIN | DEBUG);

enum class BeginMode : uint8_t {
  // Repeating a public `beginwrite` for the same request keeps one finish
  // boundary, making retries and duplicate delivery harmless.
  Idempotent,

  // Internal scopes may deliberately nest; every successful nested begin
  // requires a matching finish before the dirty mask is released.
  Nested,
};

enum class NoteResult : uint8_t {
  WriteNow,
  Deferred,
};

struct FinishResult {
  bool found = false;
  bool final = false;
  uint8_t dirtyMask = 0;
};

struct ExpiredResult {
  bool found = false;
  Owner owner{};
  uint8_t dirtyMask = 0;
};

// Not internally synchronized. The firmware adapter must serialize calls with
// its short-lived state mutex and perform all filesystem work after releasing
// that mutex.
template <size_t SlotCount>
class Core final {
  static_assert(SlotCount > 0, "settings batching needs at least one slot");

 public:
  // Returns false only if every slot belongs to another owner or a nested
  // depth cannot be represented. An idempotent repeat never consumes depth.
  bool begin(Owner owner, BeginMode mode, uint32_t nowMs = 0) {
    Slot* existing = find(owner);
    if (existing) {
      if (mode == BeginMode::Idempotent) {
        existing->lastTouchedMs = nowMs;
        return true;
      }
      if (existing->depth == static_cast<size_t>(-1)) return false;
      ++existing->depth;
      existing->lastTouchedMs = nowMs;
      return true;
    }

    Slot* available = findAvailable();
    if (!available) return false;
    available->owner = owner;
    available->depth = 1;
    available->dirtyMask = 0;
    available->lastTouchedMs = nowMs;
    return true;
  }

  // A mutation is deferred only for its exact active owner. In particular, an
  // unrelated transport/request must still persist immediately while another
  // owner has a batch open.
  NoteResult note(Owner owner, uint8_t dirtyMask, uint32_t nowMs = 0) {
    Slot* slot = find(owner);
    if (!slot) return NoteResult::WriteNow;
    slot->dirtyMask = static_cast<uint8_t>(
        slot->dirtyMask | static_cast<uint8_t>(dirtyMask & ALL));
    slot->lastTouchedMs = nowMs;
    return NoteResult::Deferred;
  }

  // A non-final nested finish consumes one depth and releases no dirty state.
  // A final finish removes the slot and transfers its complete dirty mask to
  // the caller, which may then flush outside the state mutex.
  FinishResult finish(Owner owner) {
    Slot* slot = find(owner);
    if (!slot) return {};
    if (slot->depth > 1) {
      --slot->depth;
      return {true, false, 0};
    }

    const uint8_t dirtyMask = slot->dirtyMask;
    clear(*slot);
    return {true, true, dirtyMask};
  }

  // Abort cancels the entire owner scope, including every nested level and its
  // pending dirty bits. This is intentionally different from finish().
  bool abort(Owner owner) {
    Slot* slot = find(owner);
    if (!slot) return false;
    clear(*slot);
    return true;
  }

  // If a final flush fails, restore only the file bits that remain dirty.
  // Merging into a newly opened scope does not alter its nesting depth. When
  // there is no active scope, the restored state starts at depth one so a
  // later finish retries it. False means no slot was available; the adapter
  // must surface that failure rather than silently losing durability intent.
  bool restore(Owner owner, uint8_t dirtyMask, uint32_t nowMs = 0) {
    dirtyMask = static_cast<uint8_t>(dirtyMask & ALL);
    Slot* slot = find(owner);
    if (dirtyMask == 0) {
      if (slot) slot->lastTouchedMs = nowMs;
      return true;
    }

    if (!slot) {
      slot = findAvailable();
      if (!slot) return false;
      slot->owner = owner;
      slot->depth = 1;
      slot->dirtyMask = 0;
    }
    slot->dirtyMask = static_cast<uint8_t>(slot->dirtyMask | dirtyMask);
    slot->lastTouchedMs = nowMs;
    return true;
  }

  // Remove at most one inactive-for-too-long owner per call. Unsigned
  // subtraction is wrap-safe for millis()-style clocks. Equality expires, so
  // maxAgeMs == 0 deliberately makes the first active slot immediately due.
  ExpiredResult takeExpired(uint32_t nowMs, uint32_t maxAgeMs) {
    for (size_t i = 0; i < SlotCount; ++i) {
      Slot& slot = slots_[i];
      if (slot.depth == 0 ||
          static_cast<uint32_t>(nowMs - slot.lastTouchedMs) < maxAgeMs) {
        continue;
      }

      const ExpiredResult result{true, slot.owner, slot.dirtyMask};
      clear(slot);
      return result;
    }
    return {};
  }

 private:
  struct Slot {
    Owner owner{};
    size_t depth = 0;
    uint8_t dirtyMask = 0;
    uint32_t lastTouchedMs = 0;
  };

  Slot* find(Owner owner) {
    for (size_t i = 0; i < SlotCount; ++i) {
      if (slots_[i].depth != 0 && slots_[i].owner == owner) {
        return &slots_[i];
      }
    }
    return nullptr;
  }

  Slot* findAvailable() {
    for (size_t i = 0; i < SlotCount; ++i) {
      if (slots_[i].depth == 0) return &slots_[i];
    }
    return nullptr;
  }

  static void clear(Slot& slot) {
    slot.owner = {};
    slot.depth = 0;
    slot.dirtyMask = 0;
    slot.lastTouchedMs = 0;
  }

  Slot slots_[SlotCount] = {};
};

}  // namespace hw1_settings_batch

#endif  // SYSTEM_SETTINGS_WRITE_BATCH_CORE_H
