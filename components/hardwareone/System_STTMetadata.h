#pragma once
#include <cstddef>
#include <cstdint>

// Bounded, run-local speaker assignments. Text delivery never waits for these
// results. Sequence is the original final text chunk, even after its ACK.
constexpr size_t STT_SPEAKER_HISTORY = 16;
struct STTSpeakerEntry {
  uint32_t sequence = 0;
  int8_t speaker = -1;  // 0 = Person 1; -1 = no assignment.
  bool ready = false;  // false = pending; true with -1 = skipped/unknown.
};
struct STTSpeakerSnapshot {
  uint32_t version = 0;
  bool available = false;  // The optional speaker model loaded for this run.
  uint8_t count = 0;
  STTSpeakerEntry entries[STT_SPEAKER_HISTORY]{}; // oldest to newest
};
