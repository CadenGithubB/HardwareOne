// The rules of the frames that wait for a session (System_ESPNow_PendingQueue.h),
// on the production header with a slot of the shape System_ESPNow_Sessions.cpp
// uses. Bench 2026-11-18: two espnowsend in a row after a restart, before the
// session had opened; the table kept one frame per peer, so the second evicted
// the first while both had answered "sent".
#include "System_ESPNow_PendingQueue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    ++checks;                                                                     \
    if (!(condition)) {                                                           \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);        \
      std::exit(1);                                                               \
    }                                                                             \
  } while (0)

struct Slot {
  bool inUse;
  uint8_t peerMac[6];
  uint32_t msgId;
  uint32_t seq;
  uint32_t queuedAtMs;
};

struct Table {
  Slot s[kPendingFrameSlots] = {};
  uint32_t seq = 0;

  // What pendingFrameQueue does with the rules: admit, then fill the slot.
  PendingAdmit queue(const uint8_t mac[6], uint32_t msgId, uint32_t nowMs) {
    int idx = -1;
    const PendingAdmit admit = pendingAdmit(s, kPendingFrameSlots, mac, kPendingFramesPerPeer, &idx);
    if (admit != PendingAdmit::Queued) {
      CHECK(idx == -1);
      return admit;
    }
    CHECK(idx >= 0 && idx < kPendingFrameSlots && !s[idx].inUse);
    s[idx].inUse = true;
    std::memcpy(s[idx].peerMac, mac, 6);
    s[idx].msgId = msgId;
    s[idx].seq = ++seq;
    s[idx].queuedAtMs = nowMs;
    return admit;
  }
  // What the drain does: oldest first, one slot freed per pass.
  std::vector<uint32_t> drain(const uint8_t mac[6]) {
    std::vector<uint32_t> out;
    for (int i = pendingOldestFor(s, kPendingFrameSlots, mac); i >= 0; i = pendingOldestFor(s, kPendingFrameSlots, mac)) {
      out.push_back(s[i].msgId);
      std::memset(&s[i], 0, sizeof(s[i]));
    }
    return out;
  }
};

static const uint8_t kA[6] = {0x02, 0, 0, 0, 0, 0x0A};
static const uint8_t kB[6] = {0x02, 0, 0, 0, 0, 0x0B};

static void limits() {
  // The sizes the reasons spell out and the firmware comments justify.
  CHECK(kPendingFramesPerPeer == 4);
  CHECK(kPendingFrameSlots == 16);
  CHECK(std::strcmp(pendingAdmitReason(PendingAdmit::PeerFull),
                    "4 messages already wait for this device's session") == 0);
  CHECK(std::strcmp(pendingAdmitReason(PendingAdmit::TableFull), "16 messages already wait for sessions") == 0);
}

// The bench case: two sends before the session opens both wait, and both go
// out, in order.
static void twoInARow() {
  Table t;
  CHECK(t.queue(kA, 15, 100) == PendingAdmit::Queued);
  CHECK(t.queue(kA, 18, 300) == PendingAdmit::Queued);
  CHECK(pendingCountFor(t.s, kPendingFrameSlots, kA) == 2);
  CHECK(pendingFind(t.s, kPendingFrameSlots, 15, kA) >= 0 && pendingFind(t.s, kPendingFrameSlots, 18, kA) >= 0);
  CHECK((t.drain(kA) == std::vector<uint32_t>{15, 18}));
  CHECK(pendingCountAll(t.s, kPendingFrameSlots) == 0);
  CHECK(pendingFind(t.s, kPendingFrameSlots, 15, kA) == -1);
}

// Per peer: four wait; the fifth is refused and nothing already waiting moves.
static void peerFull() {
  Table t;
  for (uint32_t id = 1; id <= 4; ++id) CHECK(t.queue(kA, id, id) == PendingAdmit::Queued);
  int idx = 7;
  CHECK(pendingAdmit(t.s, kPendingFrameSlots, kA, kPendingFramesPerPeer, &idx) == PendingAdmit::PeerFull && idx == -1);
  CHECK(t.queue(kA, 5, 5) == PendingAdmit::PeerFull);
  CHECK(pendingFind(t.s, kPendingFrameSlots, 5, kA) == -1 && pendingFind(t.s, kPendingFrameSlots, 1, kA) >= 0);
  // Another peer still has room.
  CHECK(t.queue(kB, 6, 6) == PendingAdmit::Queued);
  CHECK((t.drain(kA) == std::vector<uint32_t>{1, 2, 3, 4}));
  CHECK((t.drain(kB) == std::vector<uint32_t>{6}));
}

// The table: 16 frames in all; a new peer is then refused as TableFull, and a
// peer at its own limit as PeerFull.
static void tableFull() {
  Table t;
  for (uint8_t p = 0; p < 4; ++p) {
    const uint8_t mac[6] = {0x02, 0, 0, 0, 1, p};
    for (uint32_t n = 0; n < 4; ++n) CHECK(t.queue(mac, 100u * p + n + 1, 0) == PendingAdmit::Queued);
  }
  CHECK(pendingCountAll(t.s, kPendingFrameSlots) == 16);
  CHECK(t.queue(kA, 999, 0) == PendingAdmit::TableFull);
  const uint8_t first[6] = {0x02, 0, 0, 0, 1, 0};
  CHECK(t.queue(first, 998, 0) == PendingAdmit::PeerFull);
  // Sixteen peers with one frame each also fit (a room command to every
  // paired device, none with a session yet).
  Table u;
  for (uint8_t p = 0; p < 16; ++p) {
    const uint8_t mac[6] = {0x02, 0, 0, 0, 2, p};
    CHECK(u.queue(mac, p + 1u, 0) == PendingAdmit::Queued);
  }
  CHECK(u.queue(kA, 17, 0) == PendingAdmit::TableFull);
}

// Order survives slot reuse (a freed slot early in the table is taken by a
// newer frame) and the queue counter wrapping.
static void orderAcrossReuseAndWrap() {
  Table t;
  t.seq = 0xFFFFFFFDu;   // the counter wraps between kA's first two frames
  CHECK(t.queue(kB, 50, 0) == PendingAdmit::Queued);   // slot 0, seq 0xFFFFFFFE
  CHECK(t.queue(kA, 1, 0) == PendingAdmit::Queued);    // slot 1, seq 0xFFFFFFFF
  CHECK(t.queue(kA, 2, 0) == PendingAdmit::Queued);    // slot 2, seq 0 (wrapped)
  CHECK((t.drain(kB) == std::vector<uint32_t>{50}));   // frees slot 0
  CHECK(t.queue(kA, 3, 0) == PendingAdmit::Queued);    // slot 0, seq 1
  CHECK(pendingOldestFor(t.s, kPendingFrameSlots, kA) == 1);
  CHECK((t.drain(kA) == std::vector<uint32_t>{1, 2, 3}));
  CHECK(pendingOldestFor(t.s, kPendingFrameSlots, kA) == -1);
}

// Expiry: kPendingFrameTimeoutMs after it was queued, across millis() wrap.
static void expiry() {
  CHECK(!pendingExpired(1000, 5999, 5000));
  CHECK(pendingExpired(1000, 6000, 5000));
  CHECK(!pendingExpired(0xFFFFF000u, 0x00000387u, 5000));   // 4999 ms across the wrap
  CHECK(pendingExpired(0xFFFFF000u, 0x00000388u, 5000));    // 5000 ms
}

// A session this device answered carries its waiting frames once it is
// kPendingSessionSettleMs old (the other side has handled the SESSION_CONFIRM
// by then), across the millis() wrap too, and well before they time out.
static void settle() {
  CHECK(kPendingSessionSettleMs == 1000);
  CHECK(kPendingSessionSettleMs < 5000 / 2);
  CHECK(!pendingSessionSettled(1000, 1999));
  CHECK(pendingSessionSettled(1000, 2000));
  CHECK(!pendingSessionSettled(0xFFFFFF00u, 0x000002E7u));   // 999 ms across the wrap
  CHECK(pendingSessionSettled(0xFFFFFF00u, 0x000002E8u));    // 1000 ms
}

// Lookups are per peer and msgId; null and msgId 0 find nothing.
static void lookups() {
  Table t;
  CHECK(t.queue(kA, 7, 0) == PendingAdmit::Queued);
  CHECK(pendingFind(t.s, kPendingFrameSlots, 7, kB) == -1);
  CHECK(pendingFind(t.s, kPendingFrameSlots, 0, kA) == -1);
  CHECK(pendingFind<Slot>(nullptr, kPendingFrameSlots, 7, kA) == -1);
  CHECK(pendingCountFor<Slot>(nullptr, kPendingFrameSlots, kA) == 0);
  CHECK(pendingCountAll<Slot>(nullptr, kPendingFrameSlots) == 0);
  CHECK(pendingOldestFor(t.s, kPendingFrameSlots, nullptr) == -1);
  int idx = 3;
  CHECK(pendingAdmit<Slot>(nullptr, kPendingFrameSlots, kA, kPendingFramesPerPeer, &idx) == PendingAdmit::TableFull &&
        idx == -1);
}

int main() {
  limits();
  twoInARow();
  peerFull();
  tableFull();
  orderAcrossReuseAndWrap();
  expiry();
  settle();
  lookups();
  std::printf("espnow_pending_queue: %u checks passed\n", checks);
  return 0;
}
