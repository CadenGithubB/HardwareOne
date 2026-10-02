// Radio_CompanionCore.h - health, retry and recovery policy for a radio
// co-processor (the P4X-EYE's ESP32-C6 behind ESP-Hosted).
//
// Dependency-free on purpose: no ESP-IDF, Arduino or project headers, so the
// host test suite (test_radio_companion_core.cpp) drives it with a fake clock.
// The owner (System_RadioCompanion.cpp) feeds it transport events, heartbeats
// and operation results, calls tick() from the main loop, and performs the
// action it returns. The monitor never does I/O and never blocks.
//
// States
//   Offline     the transport is down after a failure; retries are scheduled
//   Starting    a transport start is in flight
//   Online      the companion answers; the heartbeat watchdog is armed
//   Idle        the transport was taken down on purpose (no feature needs it)
//   Held        the companion is held in reset (radio fully off)
//   Updating    a firmware update is in progress; failures are expected
//   Recovering  a soft recovery (teardown + restart) is in flight
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hw1 {
namespace radio {

enum class CompanionState : uint8_t { Offline = 0, Starting, Online, Idle, Held, Updating, Recovering };
enum class CompanionAction : uint8_t { None = 0, StartTransport, Recover, HardReboot, Hold };
enum class CompanionOutage : uint8_t {
  None = 0, BootFailed, StartFailed, TransportFailure, HeartbeatLost, UnexpectedReset, RecoveryFailed
};

struct CompanionPolicy {
  uint32_t heartbeatMs = 5000;        // 0 disables the heartbeat watchdog
  uint8_t missedHeartbeatsLimit = 3;  // consecutive missed beats before recovery
  uint32_t retryBackoffMs[5] = {5000, 15000, 60000, 300000, 900000};
  uint8_t softRecoveriesBeforeHard = 3;  // soft recoveries inside the window...
  uint32_t softRecoveryWindowMs = 600000; // ...before the next outage asks for a reboot
  uint32_t idleHoldDelayMs = 30000;   // idle this long before holding the companion
  bool autoRecover = true;            // false: every outage waits for a manual restart
  bool autoHold = false;              // true: hold the companion in reset while idle
};

struct CompanionStats {
  uint32_t heartbeatsSeen = 0;
  uint32_t lastHeartbeatNumber = 0;
  uint32_t lastHeartbeatAt = 0;
  uint32_t bootCount = 0;             // CP_INIT events (companion boots) observed
  uint8_t lastResetReason = 0;
  uint32_t startAttempts = 0;
  uint32_t startFailures = 0;
  uint32_t consecutiveFailures = 0;
  uint32_t softRecoveries = 0;
  uint32_t recoveriesFailed = 0;
  uint32_t hardRebootsRequested = 0;
  CompanionOutage lastOutage = CompanionOutage::None;
  uint32_t lastOutageAt = 0;
  uint32_t onlineSince = 0;
  uint32_t idleSince = 0;
  uint32_t nextRetryAt = 0;
  bool retryPending = false;
};

class CompanionMonitor {
 public:
  explicit CompanionMonitor(const CompanionPolicy& policy = CompanionPolicy()) : policy_(policy) {}

  void setPolicy(const CompanionPolicy& policy) { policy_ = policy; }
  const CompanionPolicy& policy() const { return policy_; }
  CompanionState state() const { return state_; }
  const CompanionStats& stats() const { return stats_; }
  bool watchdogPaused() const { return watchdogPaused_; }

  // Missed heartbeats right now, for status displays. Zero unless Online with
  // the watchdog armed and at least one beat seen.
  uint32_t missedHeartbeats(uint32_t now) const {
    if (state_ != CompanionState::Online || watchdogPaused_ || policy_.heartbeatMs == 0 ||
        stats_.heartbeatsSeen == 0)
      return 0;
    const uint32_t since = now - stats_.lastHeartbeatAt;
    return since / policy_.heartbeatMs;
  }

  // ---- boot --------------------------------------------------------------
  void onBootResult(uint32_t now, bool transportUp) {
    stats_.startAttempts++;
    if (transportUp) {
      enterOnline(now);
    } else {
      stats_.startFailures++;
      stats_.consecutiveFailures++;
      failed(now, CompanionOutage::BootFailed);
    }
  }

  // ---- transport events from the host driver ------------------------------
  void onTransportUp(uint32_t now) {
    if (state_ == CompanionState::Updating) return;  // the update owner reports the end state
    if (state_ != CompanionState::Online) enterOnline(now);
  }

  // Orderly: esp_hosted_deinit ran (the Arduino layer closes the transport when
  // no feature uses it, or an owner-driven stop). Not a fault, no retries.
  void onTransportDown(uint32_t now) {
    if (state_ == CompanionState::Recovering || state_ == CompanionState::Updating ||
        state_ == CompanionState::Starting || state_ == CompanionState::Held)
      return;
    state_ = CompanionState::Idle;
    stats_.idleSince = now;
    stats_.retryPending = false;
  }

  void onTransportFailure(uint32_t now) {
    if (state_ == CompanionState::Updating || state_ == CompanionState::Recovering ||
        state_ == CompanionState::Starting)
      return;
    outage(now, CompanionOutage::TransportFailure);
  }

  void onHeartbeat(uint32_t now, uint32_t number) {
    stats_.heartbeatsSeen++;
    stats_.lastHeartbeatNumber = number;
    stats_.lastHeartbeatAt = now;
  }

  // CP_INIT: the companion (re)booted. Expected while we are starting or
  // recovering (we reset it); anything else means it reset underneath us.
  void onCompanionBoot(uint32_t now, uint8_t resetReason) {
    stats_.bootCount++;
    stats_.lastResetReason = resetReason;
    if (state_ == CompanionState::Online) outage(now, CompanionOutage::UnexpectedReset);
  }

  // ---- owner-driven operations ------------------------------------------
  void onStartAttempt(uint32_t now) {
    (void)now;
    stats_.startAttempts++;
    stats_.retryPending = false;
    state_ = CompanionState::Starting;
  }

  void onStartResult(uint32_t now, bool ok) {
    if (ok) {
      enterOnline(now);
      return;
    }
    stats_.startFailures++;
    stats_.consecutiveFailures++;
    failed(now, CompanionOutage::StartFailed);
  }

  void onRecoveryStarted(uint32_t now) {
    (void)now;
    state_ = CompanionState::Recovering;
    pendingAction_ = CompanionAction::None;
  }

  void onRecoveryResult(uint32_t now, bool ok) {
    if (ok) {
      stats_.softRecoveries++;
      recordRecovery(now);
      enterOnline(now);
      return;
    }
    stats_.recoveriesFailed++;
    stats_.consecutiveFailures++;
    failed(now, CompanionOutage::RecoveryFailed);
  }

  void onUpdateStarted() {
    state_ = CompanionState::Updating;
    pendingAction_ = CompanionAction::None;
  }

  void onUpdateFinished(uint32_t now, bool transportUp) {
    if (transportUp) {
      enterOnline(now);
    } else {
      stats_.consecutiveFailures++;
      failed(now, CompanionOutage::StartFailed);
    }
  }

  void onHold(uint32_t now) {
    (void)now;
    state_ = CompanionState::Held;
    pendingAction_ = CompanionAction::None;
    stats_.retryPending = false;
  }

  // Leaving Held without a transport start: back to Idle (the transport is still down).
  void onRelease(uint32_t now) {
    if (state_ != CompanionState::Held) return;
    state_ = CompanionState::Idle;
    stats_.idleSince = now;
  }

  // Around host light sleep: no beats arrive while asleep, so the watchdog
  // must not count them as missed.
  void onWatchdogPaused(uint32_t now) {
    (void)now;
    watchdogPaused_ = true;
  }
  void onWatchdogResumed(uint32_t now) {
    watchdogPaused_ = false;
    stats_.lastHeartbeatAt = now;  // fresh grace period
  }

  // Manual start/recover requests clear the outage bookkeeping they supersede.
  void onManualStartRequested() { stats_.retryPending = false; }

  // ---- periodic -----------------------------------------------------------
  CompanionAction tick(uint32_t now) {
    if (pendingAction_ != CompanionAction::None) {
      const CompanionAction action = pendingAction_;
      pendingAction_ = CompanionAction::None;
      return action;
    }
    switch (state_) {
      case CompanionState::Offline:
        if (policy_.autoRecover && stats_.retryPending &&
            static_cast<int32_t>(now - stats_.nextRetryAt) >= 0) {
          stats_.retryPending = false;
          return CompanionAction::StartTransport;  // owner calls onStartAttempt()
        }
        return CompanionAction::None;
      case CompanionState::Online: {
        if (watchdogPaused_ || policy_.heartbeatMs == 0 || stats_.heartbeatsSeen == 0)
          return CompanionAction::None;
        const uint32_t since = now - stats_.lastHeartbeatAt;
        if (since / policy_.heartbeatMs >= policy_.missedHeartbeatsLimit) {
          outage(now, CompanionOutage::HeartbeatLost);
          const CompanionAction action = pendingAction_;
          pendingAction_ = CompanionAction::None;
          return action;
        }
        return CompanionAction::None;
      }
      case CompanionState::Idle:
        if (policy_.autoHold && policy_.idleHoldDelayMs > 0 &&
            now - stats_.idleSince >= policy_.idleHoldDelayMs)
          return CompanionAction::Hold;  // owner calls onHold()
        return CompanionAction::None;
      default:
        return CompanionAction::None;
    }
  }

 private:
  void enterOnline(uint32_t now) {
    state_ = CompanionState::Online;
    stats_.consecutiveFailures = 0;
    stats_.onlineSince = now;
    stats_.lastHeartbeatAt = now;  // grace period before the first beat counts
    stats_.retryPending = false;
    watchdogPaused_ = false;
    pendingAction_ = CompanionAction::None;
  }

  void failed(uint32_t now, CompanionOutage why) {
    state_ = CompanionState::Offline;
    stats_.lastOutage = why;
    stats_.lastOutageAt = now;
    const uint32_t index = stats_.consecutiveFailures == 0 ? 0 : stats_.consecutiveFailures - 1;
    const uint32_t capped = index >= 5 ? 4 : index;
    stats_.nextRetryAt = now + policy_.retryBackoffMs[capped];
    stats_.retryPending = policy_.autoRecover;
  }

  // An outage while running: decide between soft recovery and a reboot. The
  // owner performs the action; this only records and routes it.
  void outage(uint32_t now, CompanionOutage why) {
    stats_.lastOutage = why;
    stats_.lastOutageAt = now;
    if (!policy_.autoRecover) {
      state_ = CompanionState::Offline;
      stats_.retryPending = false;
      return;
    }
    if (recoveriesInWindow(now) >= policy_.softRecoveriesBeforeHard) {
      stats_.hardRebootsRequested++;
      state_ = CompanionState::Offline;
      stats_.retryPending = false;
      pendingAction_ = CompanionAction::HardReboot;
      return;
    }
    state_ = CompanionState::Recovering;
    pendingAction_ = CompanionAction::Recover;  // owner calls onRecoveryStarted()
  }

  void recordRecovery(uint32_t now) {
    recoveryAt_[recoveryIndex_] = now;
    recoveryValid_[recoveryIndex_] = true;
    recoveryIndex_ = static_cast<uint8_t>((recoveryIndex_ + 1) % kRecoveryRing);
  }

  uint32_t recoveriesInWindow(uint32_t now) const {
    uint32_t count = 0;
    for (size_t i = 0; i < kRecoveryRing; i++) {
      if (recoveryValid_[i] && now - recoveryAt_[i] <= policy_.softRecoveryWindowMs) count++;
    }
    return count;
  }

  static constexpr size_t kRecoveryRing = 8;
  CompanionPolicy policy_;
  CompanionState state_ = CompanionState::Offline;
  CompanionStats stats_;
  CompanionAction pendingAction_ = CompanionAction::None;
  bool watchdogPaused_ = false;
  uint32_t recoveryAt_[kRecoveryRing] = {};
  bool recoveryValid_[kRecoveryRing] = {};
  uint8_t recoveryIndex_ = 0;
};

inline const char* companionStateName(CompanionState state) {
  switch (state) {
    case CompanionState::Offline: return "offline";
    case CompanionState::Starting: return "starting";
    case CompanionState::Online: return "online";
    case CompanionState::Idle: return "idle";
    case CompanionState::Held: return "held";
    case CompanionState::Updating: return "updating";
    case CompanionState::Recovering: return "recovering";
  }
  return "?";
}

inline const char* companionOutageName(CompanionOutage outage) {
  switch (outage) {
    case CompanionOutage::None: return "none";
    case CompanionOutage::BootFailed: return "boot_failed";
    case CompanionOutage::StartFailed: return "start_failed";
    case CompanionOutage::TransportFailure: return "transport_failure";
    case CompanionOutage::HeartbeatLost: return "heartbeat_lost";
    case CompanionOutage::UnexpectedReset: return "unexpected_reset";
    case CompanionOutage::RecoveryFailed: return "recovery_failed";
  }
  return "?";
}

}  // namespace radio
}  // namespace hw1
