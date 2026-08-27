// System_DictationPolicy.h — dependency-free dictation admission/publication
// predicates. Kept separate so the host tests compile the exact firmware logic.
#ifndef SYSTEM_DICTATION_POLICY_H
#define SYSTEM_DICTATION_POLICY_H

#include <stddef.h>
#include <stdint.h>

// One host exchange includes framed WAV transfer, queued STT, and terminal
// delivery. Firmware itself allows up to 45 s for a framed transfer and the
// host's serialized command ceiling is 65 s, so the old 15 s deadline could
// not carry a max-length recording on slower supported UART targets.
constexpr uint32_t kDictationHostResultTimeoutMs = 90000u;

// A daemon capability is authority for one authenticated UART login only. The
// CM5 heartbeat is an independent freshness signal; neither one can substitute
// for the other.
constexpr bool dictationHostCapabilityReady(uint32_t liveEpoch,
                                            uint32_t capabilityEpoch,
                                            bool presenceSeenForSession,
                                            bool presenceFresh,
                                            bool presenceReadyOrBusy) {
  return liveEpoch != 0 && capabilityEpoch == liveEpoch &&
         presenceSeenForSession && presenceFresh && presenceReadyOrBusy;
}

// The request event is the host's permission to voicefetch immediately. Do not
// publish it merely because the WAV was closed: the recorder must first publish
// both its owner-scoped result and IDLE.
constexpr bool dictationRequestPublicationReady(bool exactOwnerStillCurrent,
                                                bool captureSaved,
                                                bool resultPublished,
                                                bool recorderIdle) {
  return exactOwnerStillCurrent && captureSaved && resultPublished &&
         recorderIdle;
}

// A result/failure is authority-bearing input into a user's field. Only the
// UART incarnation that received the request event may complete it.
constexpr bool dictationResponseSessionReady(uint32_t requestEpoch,
                                             uint32_t commandEpoch) {
  return requestEpoch != 0 && requestEpoch == commandEpoch;
}

// A request that never crossed the final owner fence needs no host tombstone.
// Once transmission starts, however, a false return cannot prove the frame was
// invisible to the peer, so both in-flight and completed pushes require cancel.
constexpr bool dictationCancelEventNeeded(bool requestPushInFlight,
                                          bool requestWasPushed) {
  return requestPushInFlight || requestWasPushed;
}

constexpr bool dictationRequestPushFenceReady(bool exactWaitingOwner,
                                              bool requestEpochMatches) {
  return exactWaitingOwner && requestEpochMatches;
}

constexpr bool dictationWaitingHostReady(bool linkRunning,
                                         uint32_t requestEpoch,
                                         uint32_t liveEpoch,
                                         bool capabilityAndPresenceReady) {
  return linkRunning && requestEpoch != 0 && requestEpoch == liveEpoch &&
         capabilityAndPresenceReady;
}

constexpr bool dictationHostResultExpired(uint32_t elapsedMs) {
  return elapsedMs >= kDictationHostResultTimeoutMs;
}

// Dependency-free wire predicates. System_UartLink claims only the exact
// `dictate` namespace before cmd_exec; a lookalike such as `dictatefoo` must
// remain an ordinary command. Keeping these here lets the host tests exercise
// the exact production decisions without Arduino/ESP-IDF stubs.
constexpr bool dictationAsciiSpace(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

constexpr char dictationAsciiLower(char value) {
  return value >= 'A' && value <= 'Z'
             ? static_cast<char>(value + ('a' - 'A'))
             : value;
}

constexpr bool dictationUartLineIsProtocol(const char* line) {
  if (!line) return false;
  while (*line && dictationAsciiSpace(*line)) ++line;
  constexpr char kVerb[] = "dictate";
  for (size_t i = 0; i < sizeof(kVerb) - 1; ++i) {
    if (!line[i] || dictationAsciiLower(line[i]) != kVerb[i]) return false;
  }
  const char boundary = line[sizeof(kVerb) - 1];
  return boundary == '\0' || dictationAsciiSpace(boundary);
}

constexpr bool dictationParseWireId(const char* text, size_t len,
                                    uint64_t* out) {
  if (!text || len != 16) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = text[i];
    uint8_t nibble = 0;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<uint8_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<uint8_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<uint8_t>(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | nibble;
  }
  if (static_cast<uint32_t>(value >> 32) == 0 ||
      static_cast<uint32_t>(value) == 0) {
    return false;
  }
  if (out) *out = value;
  return true;
}

constexpr bool dictationPrintableAscii(const char* text, size_t len,
                                       bool allowEmpty) {
  if (!text || (!allowEmpty && len == 0)) return false;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t value = static_cast<uint8_t>(text[i]);
    if (value < 0x20 || value > 0x7e) return false;
  }
  return true;
}

#endif  // SYSTEM_DICTATION_POLICY_H
