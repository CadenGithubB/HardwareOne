// Host test for Radio_CompanionCore.h: the C6 companion health policy with a
// fake clock. Pins the transitions System_RadioCompanion.cpp relies on.
#include "../../Radio_CompanionCore.h"

#include <cassert>
#include <cstdio>

using hw1::radio::CompanionAction;
using hw1::radio::CompanionMonitor;
using hw1::radio::CompanionOutage;
using hw1::radio::CompanionPolicy;
using hw1::radio::CompanionState;

static CompanionPolicy policy() {
  CompanionPolicy p;
  p.heartbeatMs = 1000;
  p.missedHeartbeatsLimit = 3;
  p.idleHoldDelayMs = 500;
  return p;
}

static void heartbeats_keep_online_and_silence_recovers() {
  CompanionMonitor m(policy());
  m.onBootResult(1000, true);
  assert(m.state() == CompanionState::Online);
  uint32_t now = 1000;
  for (uint32_t n = 1; n <= 20; n++) {
    now += 1000;
    m.onHeartbeat(now, n);
    assert(m.tick(now) == CompanionAction::None);
  }
  assert(m.missedHeartbeats(now) == 0);
  // Two missed beats: still online. Three: recover.
  assert(m.tick(now + 2900) == CompanionAction::None);
  assert(m.missedHeartbeats(now + 2900) == 2);
  assert(m.tick(now + 3000) == CompanionAction::Recover);
  assert(m.state() == CompanionState::Recovering);
  assert(m.stats().lastOutage == CompanionOutage::HeartbeatLost);
  m.onRecoveryStarted(now + 3000);
  // While recovering, driver noise is ignored.
  m.onTransportFailure(now + 3100);
  m.onCompanionBoot(now + 3200, 1);
  assert(m.state() == CompanionState::Recovering);
  m.onRecoveryResult(now + 5000, true);
  assert(m.state() == CompanionState::Online);
  assert(m.stats().softRecoveries == 1);
  assert(m.stats().bootCount == 1);
  // Fresh grace period: no immediate second recovery.
  assert(m.tick(now + 5500) == CompanionAction::None);
}

static void manual_mode_waits_for_the_operator() {
  CompanionPolicy p = policy();
  p.autoRecover = false;
  CompanionMonitor m(p);
  m.onBootResult(0, true);
  m.onHeartbeat(1000, 1);
  assert(m.tick(10000) == CompanionAction::None);
  assert(m.state() == CompanionState::Offline);
  assert(!m.stats().retryPending);
  assert(m.tick(100000) == CompanionAction::None);
  m.onManualStartRequested();
  m.onStartAttempt(100000);
  assert(m.state() == CompanionState::Starting);
  m.onStartResult(101000, true);
  assert(m.state() == CompanionState::Online);
}

static void boot_failure_retries_with_backoff() {
  CompanionMonitor m(policy());
  m.onBootResult(0, false);
  assert(m.state() == CompanionState::Offline);
  assert(m.stats().lastOutage == CompanionOutage::BootFailed);
  assert(m.stats().retryPending && m.stats().nextRetryAt == 5000);
  assert(m.tick(4999) == CompanionAction::None);
  assert(m.tick(5000) == CompanionAction::StartTransport);
  m.onStartAttempt(5000);
  m.onStartResult(25000, false);
  assert(m.stats().nextRetryAt == 25000 + 15000);  // second backoff step
  m.onStartAttempt(40000);
  m.onStartResult(60000, false);
  assert(m.stats().nextRetryAt == 60000 + 60000);
  m.onStartAttempt(120000);
  m.onStartResult(120500, true);
  assert(m.state() == CompanionState::Online);
  assert(m.stats().consecutiveFailures == 0);
  assert(m.stats().startAttempts == 4 && m.stats().startFailures == 3);
}

static void repeated_outages_escalate_to_a_reboot() {
  CompanionPolicy p = policy();
  p.softRecoveriesBeforeHard = 2;
  p.softRecoveryWindowMs = 100000;
  CompanionMonitor m(p);
  m.onBootResult(0, true);
  uint32_t now = 1000;
  for (int i = 0; i < 2; i++) {
    m.onTransportFailure(now);
    assert(m.tick(now) == CompanionAction::Recover);
    m.onRecoveryStarted(now);
    now += 3000;
    m.onRecoveryResult(now, true);
    assert(m.state() == CompanionState::Online);
  }
  m.onTransportFailure(now + 100);
  assert(m.tick(now + 100) == CompanionAction::HardReboot);
  assert(m.stats().hardRebootsRequested == 1);
  // Outside the window the same sequence recovers softly again.
  CompanionMonitor late(p);
  late.onBootResult(0, true);
  late.onTransportFailure(1000);
  assert(late.tick(1000) == CompanionAction::Recover);
  // With rebooting disallowed (the previous boot already was one), the same
  // overflow goes Offline on the slowest retry step instead of rebooting.
  CompanionPolicy q = p;
  q.hardRebootAllowed = false;
  CompanionMonitor n(q);
  n.onBootResult(0, true);
  now = 1000;
  for (int i = 0; i < 2; i++) {
    n.onTransportFailure(now);
    assert(n.tick(now) == CompanionAction::Recover);
    n.onRecoveryStarted(now);
    now += 3000;
    n.onRecoveryResult(now, true);
  }
  n.onTransportFailure(now + 100);
  assert(n.tick(now + 100) == CompanionAction::None);
  assert(n.state() == CompanionState::Offline);
  assert(n.stats().hardRebootsRequested == 0);
  assert(n.stats().retryPending);
  assert(n.stats().nextRetryAt == now + 100 + q.retryBackoffMs[4]);
  assert(n.tick(now + 100 + q.retryBackoffMs[4]) == CompanionAction::StartTransport);
  late.onRecoveryStarted(1000);
  late.onRecoveryResult(2000, true);
  late.onTransportFailure(200000);
  assert(late.tick(200000) == CompanionAction::Recover);
}

static void orderly_down_is_idle_and_may_hold() {
  CompanionPolicy p = policy();
  p.autoHold = true;
  CompanionMonitor m(p);
  m.onBootResult(0, true);
  m.onTransportDown(1000);
  assert(m.state() == CompanionState::Idle);
  assert(m.tick(1400) == CompanionAction::None);
  assert(m.tick(1500) == CompanionAction::Hold);
  m.onHold(1500);
  assert(m.state() == CompanionState::Held);
  assert(m.tick(100000) == CompanionAction::None);  // nothing retries a held companion
  m.onTransportUp(200000);                           // a feature started the transport
  assert(m.state() == CompanionState::Online);
  // Without autoHold the idle state just sits there.
  CompanionMonitor quiet(policy());
  quiet.onBootResult(0, true);
  quiet.onTransportDown(1000);
  assert(quiet.tick(100000) == CompanionAction::None);
  assert(quiet.state() == CompanionState::Idle);
}

static void sleep_pauses_the_watchdog() {
  CompanionMonitor m(policy());
  m.onBootResult(0, true);
  m.onHeartbeat(1000, 1);
  m.onWatchdogPaused(1500);
  assert(m.tick(60000) == CompanionAction::None);
  m.onWatchdogResumed(60000);
  assert(m.tick(62000) == CompanionAction::None);
  assert(m.tick(63000) == CompanionAction::Recover);
}

static void unexpected_reset_recovers_and_update_suppresses() {
  CompanionMonitor m(policy());
  m.onBootResult(0, true);
  m.onCompanionBoot(5000, 3);
  assert(m.stats().lastOutage == CompanionOutage::UnexpectedReset);
  assert(m.tick(5000) == CompanionAction::Recover);
  m.onRecoveryStarted(5000);
  m.onRecoveryResult(9000, true);
  m.onUpdateStarted();
  assert(m.state() == CompanionState::Updating);
  m.onTransportFailure(10000);
  m.onTransportDown(10001);
  m.onTransportUp(10002);
  assert(m.state() == CompanionState::Updating);
  assert(m.tick(20000) == CompanionAction::None);
  m.onUpdateFinished(21000, true);
  assert(m.state() == CompanionState::Online);
  m.onUpdateStarted();
  m.onUpdateFinished(30000, false);
  assert(m.state() == CompanionState::Offline && m.stats().retryPending);
}

int main() {
  heartbeats_keep_online_and_silence_recovers();
  manual_mode_waits_for_the_operator();
  boot_failure_retries_with_backoff();
  repeated_outages_escalate_to_a_reboot();
  orderly_down_is_idle_and_may_hold();
  sleep_pauses_the_watchdog();
  unexpected_reset_recovers_and_update_suppresses();
  puts("radio companion core tests passed");
  return 0;
}
