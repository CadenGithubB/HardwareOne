// System_ESPNow_SessionCapsCore.h - the session capability word
//
// Each side of a session handshake states what its firmware image can do, in
// one 32-bit word (little endian on the wire): the initiator in SESSION_OPEN,
// the responder in SESSION_CONFIRM. The word sits inside the frame's signed
// transcript (System_ESPNow_Wire.h, "SESSION establishment payloads"), so a
// relay or an attacker cannot change it. Each side keeps the other side's word
// with the session (SessionState.peerSessionCaps); a SESSION_REKEY keeps it.
//
// Rules:
//   * A bit describes the image's build and is fixed for the boot. A bit never
//     depends on a setting: runtime choices (mesh relay on or off, mesh time,
//     bond mode, accepting sensor control) stay in each request's own answer.
//     Because of this the word never goes stale during a session, a REKEY
//     can keep it, and the glare re-send of an OPEN signs the same value.
//   * No negotiation: each side only reads the other's word. Frames sent
//     without a session (handshake, group-signed, plaintext) never depend on it.
//   * Bits 8 and 9 are named for back-capture: backlog_serve (serves its
//     outbox, BACKLOG_* 156-158; never set until that package is built) and
//     files_serve (serves file catch-up, CATCHUP_* 82-85; set by an
//     ENABLE_ESPNOW_SEND build since 2026-11-19). The puller is kCollect, set
//     by an ENABLE_ESPNOW_COLLECT build. Both sides show them by name.
//   * Bits 10-31 are reserved: sent 0, ignored on receipt, kept as received and
//     shown as "+0x..." so a later firmware's bits stay visible.
//   * "Known" caps exist only for an ACTIVE or REKEYING session. The responder
//     learns the initiator's word from a SESSION_OPEN, whose signature covers
//     nothing fresh from the responder, so a recorded OPEN replayed after the
//     responder restarted is accepted with its old word. That word counts as
//     confirmed only once a sealed frame of the session has opened (the sender
//     then holds the session's keys); a gate that refuses on a missing bit
//     waits for that. The initiator's view, from a CONFIRM bound to its own
//     fresh nonceA, is confirmed at once.
//
// Pure logic: no heap, no OS, no logging (test/host/test_espnow_session_caps_core.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace sessioncaps {

// Bit values at 1.0. A bit's meaning never changes; a new one takes the next free bit.
constexpr uint32_t kBond           = 1u << 0;  // built with bond mode (ENABLE_BONDED_MODE)
constexpr uint32_t kRemoteFs       = 1u << 1;  // answers FS_LIST / FS_STAT / FS_GET
constexpr uint32_t kFileReceive    = 1u << 2;  // accepts FILE_START .. FILE_END
constexpr uint32_t kSensorSend     = 1u << 3;  // built with at least one sensor whose readings it can send
constexpr uint32_t kCollect        = 1u << 4;  // a collector build (ENABLE_ESPNOW_COLLECT)
constexpr uint32_t kRelayForward   = 1u << 5;  // can forward routed and flooded frames
constexpr uint32_t kTimeMaster     = 1u << 6;  // can send TIME_SYNC
constexpr uint32_t kSessionUnknown = 1u << 7;  // understands SESSION_UNKNOWN (opcode 18); informational:
                                               // the sender of SESSION_UNKNOWN holds no session to read it from
constexpr uint32_t kBacklogServe   = 1u << 8;  // serves its outbox (BACKLOG_*); named, not set yet
constexpr uint32_t kFilesServe     = 1u << 9;  // serves file catch-up (CATCHUP_*): an ENABLE_ESPNOW_SEND build
constexpr uint32_t kKnownMask      = 0x3FFu;   // the named bits
constexpr uint32_t kSentMask       = 0xFFu | kFilesServe;  // the bits this image may set (buildCaps)

constexpr unsigned kBitCount = 10;  // named bits, from bit 0

// Names for display and JSON, by bit.
inline const char* bitName(unsigned bit) {
  static const char* const kNames[kBitCount] = {
    "bond", "remote_fs", "file_receive", "sensor_send",
    "collect", "relay_forward", "time_master", "session_unknown",
    "backlog_serve", "files_serve",
  };
  return bit < kBitCount ? kNames[bit] : nullptr;
}

// This image's word. The bits every 1.0 image has are always set; the four
// build-dependent ones come from the caller. Never kBacklogServe (its outbox
// is not built yet).
constexpr uint32_t buildCaps(bool bond, bool sensors, bool collect, bool filesServe = false) {
  return (bond ? kBond : 0u) | kRemoteFs | kFileReceive | (sensors ? kSensorSend : 0u) |
         (collect ? kCollect : 0u) | kRelayForward | kTimeMaster | kSessionUnknown |
         (filesServe ? kFilesServe : 0u);
}

inline bool has(uint32_t caps, uint32_t bit) { return (caps & bit) != 0; }

// "bond,remote_fs,..." with reserved bits as "+0x...", "none" for 0. Always
// NUL-terminated when outLen > 0; a short buffer cuts the text. Returns the
// length the whole text needs (like snprintf).
inline size_t capsText(uint32_t caps, char* out, size_t outLen) {
  size_t need = 0;
  auto put = [&](const char* s) {
    for (; *s; ++s, ++need) {
      if (outLen && need < outLen - 1) out[need] = *s;
    }
  };
  if (outLen) out[0] = '\0';
  bool first = true;
  for (unsigned bit = 0; bit < kBitCount; ++bit) {
    if (!(caps & (1u << bit))) continue;
    if (!first) put(",");
    put(bitName(bit));
    first = false;
  }
  const uint32_t rest = caps & ~kKnownMask;
  if (rest) {
    char hex[16];
    snprintf(hex, sizeof hex, "+0x%lX", (unsigned long)rest);
    if (!first) put(",");
    put(hex);
    first = false;
  }
  if (first) put("none");
  if (outLen) out[need < outLen - 1 ? need : outLen - 1] = '\0';
  return need;
}

// What a sensor request (espnowsensorreq) needs from the receiver's build.
// Asked for its own readings, the device must have a sensor to send
// (kSensorSend). Asked through a bond master for its partner's ('via',
// fwdLink != 0), only the master's bond mode counts (kBond), never its own
// sensors. A word not known (no session) or not yet confirmed refuses nothing.
enum class SensorReqRefusal : uint8_t { None, NoSensors, NoBond };
inline SensorReqRefusal sensorRequestRefusal(bool known, bool confirmed, uint32_t caps, bool forwarded) {
  if (!known || !confirmed) return SensorReqRefusal::None;
  if (forwarded) return has(caps, kBond) ? SensorReqRefusal::None : SensorReqRefusal::NoBond;
  return has(caps, kSensorSend) ? SensorReqRefusal::None : SensorReqRefusal::NoSensors;
}

}  // namespace sessioncaps
