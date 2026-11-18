#pragma once
// ESP-NOW message history: the per-call name cache of the history readers
// (web /api/espnow/messages, the OLED conversation, the G2 inbox and chat).
//
// History records carry no name. A reader asks espnowHistoryDisplayName()
// (System_ESPNow.h) for the name of each row it shows: "" for a sent record,
// the MAC text for an unattributed received record, else the peer's current
// name. That resolver takes the ESP-NOW group lock and walks the peer tables,
// so a reader resolves each distinct (peer, kind) once per call, not once per
// row. This cache is that "once": the reader clears it at the start of a call
// (one web poll, one OLED refresh, one G2 page build), asks it for every row
// and passes the real resolver as a callback, which runs only on a miss.
//
//   kind  sent, unattributed received, attributed received: the three cases
//         of the resolver; each has its own entry for a peer, so a peer whose
//         rows mix authenticated and plaintext frames shows both correctly.
//
// A full cache replaces its entries round robin, so a call with more distinct
// peers than kEntries still shows the right names; it only resolves more often.
// NameCache keeps 8 entries (~330 B; web and G2, several peers per call); a
// view of one peer can use BasicNameCache<2> (attributed + unattributed).
// The pointer get() returns is valid until the next get() or clear(): copy or
// print it before asking for the next row.
//
// No allocation, no locking, no logging. The cache is not thread safe: one
// owner per instance (each reader keeps its own and runs on one task). The host
// test compiles this file as is (espnow_history_names_tests).
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "System_ESPNow_HistoryCore.h"

namespace espnowhist {

template <int N>
class BasicNameCache {
  static_assert(N >= 1 && N <= 64, "a small cache");

 public:
  static constexpr int kEntries = N;

  // Forgets every entry. Call at the start of each reader call, so names
  // follow renames between calls.
  void clear() {
    used_ = 0;
    victim_ = 0;
  }

  // The display name for a record of peer `mac` (null counts as 00:..:00).
  // On a miss calls resolve(char* out, size_t cap) with cap == kNameCap and
  // out pre-set to "", then forces a NUL at out[kNameCap - 1]. Never returns
  // null; the result is always NUL-terminated and at most kNameCap - 1 chars.
  template <class Resolve>
  const char* get(const uint8_t* mac, bool sent, bool unattributed, Resolve&& resolve) {
    static const uint8_t kZeroMac[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t* key = mac ? mac : kZeroMac;
    const uint8_t kind = sent ? kKindSent : (unattributed ? kKindUnattributed : kKindAttributed);
    for (int i = 0; i < used_; ++i) {
      if (entries_[i].kind == kind && std::memcmp(entries_[i].mac, key, 6) == 0) {
        ++hits_;
        return entries_[i].name;
      }
    }
    int at;
    if (used_ < kEntries) {
      at = used_++;
    } else {
      at = victim_;
      victim_ = (victim_ + 1) % kEntries;
    }
    Entry& e = entries_[at];
    std::memcpy(e.mac, key, 6);
    e.kind = kind;
    e.name[0] = '\0';
    resolve(e.name, sizeof(e.name));
    e.name[sizeof(e.name) - 1] = '\0';
    ++misses_;
    return e.name;
  }

  // Counters for the host test (never reset by clear()).
  uint32_t hits() const { return hits_; }
  uint32_t misses() const { return misses_; }

 private:
  static constexpr uint8_t kKindAttributed = 0;
  static constexpr uint8_t kKindUnattributed = 1;
  static constexpr uint8_t kKindSent = 2;
  struct Entry {
    uint8_t mac[6];
    uint8_t kind;
    char name[kNameCap];
  };
  Entry entries_[kEntries] = {};
  int used_ = 0;
  int victim_ = 0;
  uint32_t hits_ = 0;
  uint32_t misses_ = 0;
};

using NameCache = BasicNameCache<8>;

}  // namespace espnowhist
