// Unit test of espnowhist::NameCache (System_ESPNow_HistoryNames.h), the
// per-call name cache of the ESP-NOW history readers. Compiles the production
// header as is.
#include "System_ESPNow_HistoryNames.h"

#include <cstdio>
#include <cstring>
#include <string>

static int gFailures = 0, gChecks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++gChecks;                                                               \
    if (!(cond)) {                                                           \
      ++gFailures;                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                        \
  } while (0)

using espnowhist::NameCache;
using espnowhist::kNameCap;

static int gCalls = 0;
static size_t gLastCap = 0;
static bool gOutWasEmpty = false;

// A resolver that encodes its key, so a wrong cache hit shows.
static const char* nameOf(NameCache& c, uint8_t last, bool sent, bool unattributed) {
  uint8_t mac[6] = {1, 2, 3, 4, 5, last};
  return c.get(mac, sent, unattributed, [&](char* out, size_t cap) {
    ++gCalls;
    gLastCap = cap;
    gOutWasEmpty = out[0] == '\0';
    std::snprintf(out, cap, "%s-%u", sent ? "S" : (unattributed ? "U" : "A"), static_cast<unsigned>(last));
  });
}

static std::string expect(uint8_t last, bool sent, bool unattributed) {
  char b[16];
  std::snprintf(b, sizeof b, "%s-%u", sent ? "S" : (unattributed ? "U" : "A"), static_cast<unsigned>(last));
  return b;
}

int main() {
  // Misses resolve, hits do not; each kind of a peer is its own entry.
  {
    NameCache c;
    gCalls = 0;
    CHECK(expect(1, false, false) == nameOf(c, 1, false, false));
    CHECK(gCalls == 1 && gLastCap == kNameCap && gOutWasEmpty);
    CHECK(expect(1, false, false) == nameOf(c, 1, false, false));
    CHECK(gCalls == 1);
    CHECK(expect(1, false, true) == nameOf(c, 1, false, true));
    CHECK(expect(1, true, false) == nameOf(c, 1, true, false));
    CHECK(expect(1, true, true) == nameOf(c, 1, true, true));   // sent wins over unattributed
    CHECK(gCalls == 3);
    CHECK(expect(2, false, false) == nameOf(c, 2, false, false));
    CHECK(gCalls == 4);
    CHECK(c.hits() == 2 && c.misses() == 4);
    // clear() forgets the entries (renames show on the next call), not the counters.
    c.clear();
    CHECK(expect(1, false, false) == nameOf(c, 1, false, false));
    CHECK(gCalls == 5 && c.misses() == 5);
  }
  // More distinct keys than entries: round-robin replacement, always the right name.
  {
    NameCache c;
    gCalls = 0;
    for (int round = 0; round < 3; ++round)
      for (uint8_t k = 0; k < 20; ++k) CHECK(expect(k, false, false) == nameOf(c, k, false, false));
    CHECK(gCalls == 60);
    // Exactly kEntries keys after a wrap all hit.
    c.clear();
    gCalls = 0;
    for (uint8_t k = 0; k < 12; ++k) nameOf(c, k, false, false);       // 0..3 replaced by 8..11
    CHECK(gCalls == 12);
    for (uint8_t k = 4; k < 12; ++k) CHECK(expect(k, false, false) == nameOf(c, k, false, false));
    CHECK(gCalls == 12);
    CHECK(expect(0, false, false) == nameOf(c, 0, false, false));
    CHECK(gCalls == 13);
  }
  // Resolver that writes nothing -> ""; one that fills every byte -> terminated.
  {
    NameCache c;
    uint8_t mac[6] = {9, 9, 9, 9, 9, 9};
    const char* n = c.get(mac, false, false, [](char*, size_t) {});
    CHECK(n && n[0] == '\0');
    const char* f = c.get(mac, false, true, [](char* out, size_t cap) { std::memset(out, 'x', cap); });
    CHECK(std::strlen(f) == kNameCap - 1);
    // Null MAC is the all-zero key, distinct from a real peer.
    uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    const char* z1 = c.get(nullptr, false, false, [](char* out, size_t cap) { std::snprintf(out, cap, "zero"); });
    CHECK(std::strcmp(z1, "zero") == 0);
    int calls = 0;
    const char* z2 = c.get(zero, false, false, [&](char* out, size_t cap) { ++calls; std::snprintf(out, cap, "no"); });
    CHECK(calls == 0 && std::strcmp(z2, "zero") == 0);
  }
  // A fresh cache (static storage, as the readers keep it) starts empty.
  {
    static NameCache s;
    CHECK(s.hits() == 0 && s.misses() == 0);
    gCalls = 0;
    nameOf(s, 7, false, false);
    CHECK(gCalls == 1);
  }
  // The one-peer size the OLED uses: two kinds fit, a third replaces round robin.
  {
    espnowhist::BasicNameCache<2> c;
    static_assert(espnowhist::BasicNameCache<2>::kEntries == 2, "two entries");
    uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    int calls = 0;
    auto r = [&](char* out, size_t cap) { ++calls; std::snprintf(out, cap, "n%d", calls); };
    CHECK(std::strcmp(c.get(mac, false, false, r), "n1") == 0);
    CHECK(std::strcmp(c.get(mac, false, true, r), "n2") == 0);
    CHECK(std::strcmp(c.get(mac, false, false, r), "n1") == 0);
    CHECK(std::strcmp(c.get(mac, false, true, r), "n2") == 0);
    CHECK(calls == 2);
    CHECK(std::strcmp(c.get(mac, true, false, r), "n3") == 0);   // replaces the attributed entry
    CHECK(std::strcmp(c.get(mac, false, true, r), "n2") == 0);
    CHECK(std::strcmp(c.get(mac, false, false, r), "n4") == 0);
    CHECK(calls == 4);
  }
  std::printf("espnow_history_names_tests: %d/%d checks passed\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
