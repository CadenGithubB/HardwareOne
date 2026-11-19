#ifndef SYSTEM_ESPNOW_CHANNEL_CORE_H
#define SYSTEM_ESPNOW_CHANNEL_CORE_H

// ============================================================================
// The ESP-NOW channel for the current Wi-Fi state, and the 'espnowchannel'
// checker's verdict. Pure header (no Arduino, no ESP-IDF): the host tests
// compile it directly; System_ESPNow.cpp reads the radio and Wi-Fi state and
// passes it in.
//
// ESP-NOW has one radio: while the station is joined to an AP the AP owns the
// channel. So the channel is
//   - the AP's channel while joined (whatever the setting says);
//   - else the setting when it is 1..13 (two off-grid devices meet on it);
//   - else the last channel the station knew, else 1.
// ============================================================================

#include <stdint.h>

namespace espnowch {

inline bool validChannel(uint8_t ch) { return ch >= 1 && ch <= 13; }

// `joined`: the station is connected. `staChannel`: the station's channel as
// the driver reports it (0 when unknown). `pinned`: the setting (0 = auto).
inline uint8_t expected(bool joined, uint8_t staChannel, uint8_t pinned) {
  if (joined && validChannel(staChannel)) return staChannel;
  if (validChannel(pinned)) return pinned;
  return validChannel(staChannel) ? staChannel : 1;
}

enum class Verdict : uint8_t {
  Ok,          // the radio is on the expected channel
  Mismatch,    // it is not
  NotPinned,   // offline with ESP-NOW stopped: nothing holds the radio on a channel yet
};

// The checker. The expected channel does not depend on ESP-NOW running: while
// joined it is the AP's channel either way (the bench line "Expected 0 ...
// MISMATCH" while joined on channel 11 came from treating a stopped ESP-NOW as
// "expected 0"). Offline, only a running ESP-NOW pins the radio, so a stopped
// one is not a mismatch.
inline Verdict check(bool joined, bool espnowRunning, uint8_t radioChannel, uint8_t expectedChannel) {
  if (!joined && !espnowRunning) return Verdict::NotPinned;
  return radioChannel == expectedChannel ? Verdict::Ok : Verdict::Mismatch;
}

}  // namespace espnowch

#endif  // SYSTEM_ESPNOW_CHANNEL_CORE_H
