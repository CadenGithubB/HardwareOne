// System_ESPNow_PendingQueue.h - the rules of the frames that wait for a session
//
// An encrypted frame to a peer without an ACTIVE session is parked while the
// handshake (KEY_EX and/or SESSION_OPEN) runs, and goes out when the session
// opens (System_ESPNow_Sessions.cpp owns the table, its lock and the sends).
// Until 2026-11-18 the table kept one frame per peer, so a second send before
// the session opened evicted the first while both commands had answered
// "sent" (bench 2026-11-18: two espnowsend in a row after a restart).
//
// The rules, pure on a fixed table (no heap, no lock, no clock):
//   - Each peer keeps up to kPendingFramesPerPeer frames, oldest first; they go
//     out in the order they were queued.
//   - The table holds kPendingFrameSlots frames in all.
//   - A frame that does not fit (its peer's queue or the table full) is
//     refused: the newest frame is turned away, never an older one. Every
//     frame already in the queue was answered "queued"; dropping one of them
//     would break that answer after the fact and send the rest out of order,
//     while a refusal reaches the caller of the new frame at once, who can say
//     so and retry (the queue empties within kPendingFrameTimeoutMs either way).
//   - A frame that waited kPendingFrameTimeoutMs is dropped (the owner marks
//     it failed).
//
// The slot type the templates take has the fields inUse, peerMac[6], msgId,
// seq and queuedAtMs. seq is the queue order the owner stamps from a counter;
// order compares are wrap-safe differences, like every time compare here.
#pragma once

#include <stdint.h>
#include <string.h>

// Two or three sends in a row while a session opens is the common case (a user
// typing, an app retrying, a bond resync chain); 4 holds them with room to
// spare, and the 5 s timeout bounds how long any of them waits.
constexpr uint8_t kPendingFramesPerPeer = 4;
// One frame for every paired device (MESH_PEER_MAX = 16): a room or tag
// command to every device at once, none with a session yet, still fits.
// 16 x ~224 B = ~3.6 KB, in PSRAM.
constexpr uint8_t kPendingFrameSlots = 16;

enum class PendingAdmit : uint8_t {
  Queued    = 0,  // a slot was found
  PeerFull  = 1,  // kPendingFramesPerPeer frames already wait for this peer
  TableFull = 2,  // every slot holds a frame for some peer
};

// Frames waiting for `mac`.
template <class Slot>
inline uint8_t pendingCountFor(const Slot* slots, uint8_t n, const uint8_t mac[6]) {
  uint8_t count = 0;
  if (!slots || !mac) return 0;
  for (uint8_t i = 0; i < n; ++i) {
    if (slots[i].inUse && memcmp(slots[i].peerMac, mac, 6) == 0) ++count;
  }
  return count;
}

// Frames waiting for any peer.
template <class Slot>
inline uint8_t pendingCountAll(const Slot* slots, uint8_t n) {
  uint8_t count = 0;
  if (!slots) return 0;
  for (uint8_t i = 0; i < n; ++i) {
    if (slots[i].inUse) ++count;
  }
  return count;
}

// Where a new frame for `mac` goes. On Queued, *slotOut is a free slot; on a
// refusal it is -1 and nothing in the table changed.
template <class Slot>
inline PendingAdmit pendingAdmit(const Slot* slots, uint8_t n, const uint8_t mac[6], uint8_t perPeer,
                                 int* slotOut) {
  if (slotOut) *slotOut = -1;
  if (!slots || !mac) return PendingAdmit::TableFull;
  if (pendingCountFor(slots, n, mac) >= perPeer) return PendingAdmit::PeerFull;
  for (uint8_t i = 0; i < n; ++i) {
    if (!slots[i].inUse) {
      if (slotOut) *slotOut = (int)i;
      return PendingAdmit::Queued;
    }
  }
  return PendingAdmit::TableFull;
}

// The frame for `mac` that was queued first (the next to go out), or -1.
template <class Slot>
inline int pendingOldestFor(const Slot* slots, uint8_t n, const uint8_t mac[6]) {
  int oldest = -1;
  if (!slots || !mac) return -1;
  for (uint8_t i = 0; i < n; ++i) {
    if (!slots[i].inUse || memcmp(slots[i].peerMac, mac, 6) != 0) continue;
    if (oldest < 0 || (int32_t)(slots[i].seq - slots[oldest].seq) < 0) oldest = (int)i;
  }
  return oldest;
}

// The slot holding the frame `msgId` for `mac`, or -1 (sent, dropped or never
// queued). msgId 0 is no frame.
template <class Slot>
inline int pendingFind(const Slot* slots, uint8_t n, uint32_t msgId, const uint8_t mac[6]) {
  if (!slots || !mac || msgId == 0) return -1;
  for (uint8_t i = 0; i < n; ++i) {
    if (slots[i].inUse && slots[i].msgId == msgId && memcmp(slots[i].peerMac, mac, 6) == 0) return (int)i;
  }
  return -1;
}

// A frame queued at queuedAtMs has waited its time out at nowMs.
inline bool pendingExpired(uint32_t queuedAtMs, uint32_t nowMs, uint32_t timeoutMs) {
  return (uint32_t)(nowMs - queuedAtMs) >= timeoutMs;
}

// A frame can still wait while its peer's session is ACTIVE: this device
// answered the handshake (the responder of the peer's SESSION_OPEN, glare
// included, where the B side yields to the A side's OPEN), which drains
// nothing, or the frame was queued just after the drain of a session that had
// just opened. The periodic tick sends it once the session is this old. The
// side that answered is ACTIVE before the other side has handled its
// SESSION_CONFIRM, and a frame sealed before then is dropped there as
// undecryptable, so it waits the settle time the bond agreement request waits
// for the same reason (kSessionSettleMs in System_ESPNow_BondAgreeCore.h),
// well inside the 5 s a frame may wait (kPendingFrameTimeoutMs).
constexpr uint32_t kPendingSessionSettleMs = 1000;

// The ACTIVE session that went up at establishedAtMs may carry waiting frames.
inline bool pendingSessionSettled(uint32_t establishedAtMs, uint32_t nowMs) {
  return (uint32_t)(nowMs - establishedAtMs) >= kPendingSessionSettleMs;
}

// Why a frame was refused, for a status string or a log line.
inline const char* pendingAdmitReason(PendingAdmit admit) {
  switch (admit) {
    case PendingAdmit::PeerFull:  return "4 messages already wait for this device's session";
    case PendingAdmit::TableFull: return "16 messages already wait for sessions";
    default:                      return "queued";
  }
}

static_assert(kPendingFramesPerPeer == 4 && kPendingFrameSlots == 16,
              "pendingAdmitReason spells the two limits out");
