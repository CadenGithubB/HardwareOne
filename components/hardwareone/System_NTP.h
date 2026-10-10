#ifndef SYSTEM_NTP_H
#define SYSTEM_NTP_H

#include <stdint.h>
#include "System_BuildConfig.h"

// Network time client. Clock validity, source tracking, timezone, and shared
// post-step duties remain in System_Clock. Platform headers stay in the .cpp.

// What actually happened during a foreground NTP attempt. Clock::isSynced()
// cannot distinguish "a server replied" from "the clock was already set";
// this can.
enum class NtpSyncOutcome : uint8_t {
  Reply,        // an SNTP server actually answered during the wait window
  KeptPrior,    // no reply yet, but the clock was already valid — kept it
  RtcFallback,  // dark timeout, adopted the DS3231's time instead
  Failed,       // Wi-Fi unavailable, or no reply and no usable fallback
};

// Runtime facts captured from the real lwIP SNTP reply callback. "Armed"
// means esp_sntp_enabled() sees a client PCB; it deliberately does not claim a
// packet is in flight, retries are progressing, or the network can deliver one.
struct NtpRuntimeStatus {
  uint32_t replyCount;       // actual server replies observed this boot
  uint32_t foregroundTimeoutCount;
  uint32_t lateReplyCount;
  int64_t lastReplyEpoch;    // reply-supplied Unix epoch; 0 until first reply
  uint64_t lastReplyAgeMs;   // monotonic age at snapshot time
  bool clientArmed;          // client PCB exists; not proof of traffic/reachability
  bool awaitingLateReply;    // a foreground wait elapsed with no reply
};

#if ENABLE_WIFI
// Start/restart the client using the current server and timezone settings.
void setupNTP();

// Synchronous; runs in the calling task's stack. Returns true for Reply,
// KeptPrior, or RtcFallback. Disconnected/disabled Wi-Fi returns false.
bool syncNTPAndResolve(NtpSyncOutcome* outcomeOut = nullptr);
NtpRuntimeStatus getNtpRuntimeStatus();
// Main-loop drain (HardwareOne loop is the ONLY caller — a second consumer
// would race the pending flag): hands stored SNTP-reply facts to
// Clock::clockStepped(). Covers both foreground syncs and later corrections
// the lwIP daemon can make after this foreground caller has returned.
void ntpSyncDrainTick();
#else
inline void setupNTP() {}
inline bool syncNTPAndResolve(NtpSyncOutcome* outcomeOut = nullptr) {
  if (outcomeOut) *outcomeOut = NtpSyncOutcome::Failed;
  return false;
}
inline NtpRuntimeStatus getNtpRuntimeStatus() { return {}; }
inline void ntpSyncDrainTick() {}
#endif

#endif // SYSTEM_NTP_H
