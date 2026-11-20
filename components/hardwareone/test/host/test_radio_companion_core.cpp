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
using hw1::radio::ConsoleMirrorPolicy;
using hw1::radio::MirrorStep;

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

// A fake slave link for waitForTransportUp: up from `upAt` on (0 = never).
struct FakeLink {
  uint32_t now = 0;
  uint32_t upAt = 0;
  uint32_t slept = 0;
  int sleeps = 0;
  bool wait(uint32_t startedAt, uint32_t budgetMs, uint32_t pollMs) {
    return hw1::radio::waitForTransportUp(
        startedAt, budgetMs, pollMs, [this] { return upAt != 0 && now >= upAt; }, [this] { return now; },
        [this](uint32_t ms) {
          now += ms;
          slept += ms;
          sleeps++;
        });
  }
};

// Bench 2026-11-08, first companion retry after the boot spent Hosted's
// counter (retry[100]): Hosted started at 45403 ms and gave up at 47138 ms, in
// the same millisecond as "Waiting for esp_hosted slave to be ready", right
// after the card init; it opened the data path at the slave at 47211 ms. That
// SDMMC host was wedged, so TRANSPORT_UP never came: the wait runs out the
// budget, counted from the start.
static void the_bench_retry_with_a_wedged_host_waits_out_the_budget() {
  FakeLink l;
  l.now = 47138;  // hostedInitWiFi() returned when Hosted gave up
  assert(!l.wait(45403, 20000, 50));
  assert(l.now == 45403 + 20000);
}

// The same timing on a healthy data path: TRANSPORT_UP follows the data path
// opening as it did at that boot's first, good start (Open data path at 3215
// ms, Transport active at 3244 ms: 29 ms). The start now waits for it.
static void a_start_waits_for_the_slave_after_hosted_gives_up_at_once() {
  FakeLink l;
  l.now = 47138;
  l.upAt = 47211 + 29;
  assert(l.wait(45403, 20000, 50));
  assert(l.now >= l.upAt && l.now < l.upAt + 50);
  assert(l.slept < 200);
}

static void a_slave_that_never_answers_waits_only_the_budget() {
  FakeLink l;
  l.now = 1000 + 5;
  assert(!l.wait(1000, 20000, 50));
  assert(l.now == 1000 + 20000);  // not a poll interval past it
}

static void hosted_already_spent_the_budget_so_no_second_wait() {
  // Hosted waited its full 100 x 200 ms itself and failed: no extra 20 s.
  FakeLink l;
  l.now = 3000 + 20000;
  assert(!l.wait(3000, 20000, 50));
  assert(l.sleeps == 0);
}

static void an_up_link_does_not_wait() {
  FakeLink l;
  l.now = 10;
  l.upAt = 1;
  assert(l.wait(0, 20000, 50));
  assert(l.sleeps == 0);
}

static void the_wait_survives_a_clock_wrap() {
  FakeLink l;
  const uint32_t startedAt = 0xFFFFFF00u;
  l.now = startedAt + 10;
  l.upAt = 0;  // never: must still stop at the budget across the wrap
  assert(!l.wait(startedAt, 1000, 50));
  assert(l.now - startedAt == 1000);
}

// ---- incident and fault counts ------------------------------------------------
// `outages` is what the lifetime count adds up: one per incident. `faults`
// moves on every fault, retries included; the owner arms the console mirror
// on a change.
static void an_incident_counts_once_and_its_retries_do_not() {
  CompanionMonitor m(policy());
  m.onBootResult(0, false);  // the companion did not come up: one incident
  assert(m.stats().outages == 1 && m.stats().faults == 1);
  uint32_t now = 0;
  for (int retry = 1; retry <= 6; retry++) {  // a wedged companion: every retry fails
    now = m.stats().nextRetryAt;
    assert(m.tick(now) == CompanionAction::StartTransport);
    m.onStartAttempt(now);
    m.onStartResult(now + 100, false);
    assert(m.stats().outages == 1);                               // still the same incident
    assert(m.stats().faults == 1u + static_cast<uint32_t>(retry));  // but a fault each time
  }
  m.onStartAttempt(now + 1000);
  m.onStartResult(now + 2000, true);
  assert(m.stats().outages == 1);

  // An outage while running is the second incident; its failed recovery is a retry.
  m.onHeartbeat(now + 3000, 1);
  const uint32_t faults = m.stats().faults;
  assert(m.tick(now + 3000 + 3000) == CompanionAction::Recover);
  assert(m.stats().outages == 2 && m.stats().faults == faults + 1);
  m.onRecoveryStarted(now + 6000);
  m.onRecoveryResult(now + 7000, false);
  assert(m.stats().outages == 2 && m.stats().faults == faults + 2);
}

static void restarts_on_purpose_are_not_incidents() {
  CompanionMonitor m(policy());
  m.onBootResult(0, true);
  // c6restart: the owner starts a recovery itself and it succeeds.
  m.onManualStartRequested();
  m.onRecoveryStarted(100);
  m.onCompanionBoot(200, 1, /*expected=*/true);
  m.onRecoveryResult(300, true);
  // c6hold on / off, an idle close, light sleep, a good update.
  m.onHold(400);
  m.onRelease(500);
  m.onStartAttempt(500);
  m.onStartResult(600, true);
  m.onTransportDown(700);
  m.onTransportUp(800);
  m.onWatchdogPaused(900);
  m.onWatchdogResumed(1000);
  m.onUpdateStarted();
  m.onTransportFailure(1100);
  m.onUpdateFinished(1200, true);
  assert(m.stats().outages == 0 && m.stats().faults == 0);
  // The ones that fail are faults (the mirror should show why), not incidents.
  m.onRecoveryStarted(1300);
  m.onRecoveryResult(1400, false);
  assert(m.stats().outages == 0 && m.stats().faults == 1);
}

static void every_way_out_of_an_outage_counts_it_once() {
  // Soft recovery, the reboot request, the slow retry when a reboot is not
  // allowed, and manual mode: one incident and one fault each.
  CompanionPolicy p = policy();
  p.softRecoveriesBeforeHard = 1;
  p.softRecoveryWindowMs = 60000;
  CompanionMonitor m(p);
  m.onBootResult(0, true);
  m.onTransportFailure(1000);  // soft recovery
  assert(m.tick(1000) == CompanionAction::Recover);
  assert(m.stats().outages == 1 && m.stats().faults == 1);
  m.onRecoveryStarted(1000);
  m.onRecoveryResult(2000, true);
  m.onTransportFailure(3000);  // one recovery in the window already: reboot
  assert(m.tick(3000) == CompanionAction::HardReboot);
  assert(m.stats().outages == 2 && m.stats().faults == 2);

  p.hardRebootAllowed = false;
  CompanionMonitor n(p);
  n.onBootResult(0, true);
  n.onTransportFailure(1000);
  assert(n.tick(1000) == CompanionAction::Recover);
  n.onRecoveryStarted(1000);
  n.onRecoveryResult(2000, true);
  n.onTransportFailure(3000);  // no reboot allowed: offline on the slowest retry
  assert(n.tick(3000) == CompanionAction::None && n.state() == CompanionState::Offline);
  assert(n.stats().outages == 2 && n.stats().faults == 2);

  p.autoRecover = false;
  CompanionMonitor manual(p);
  manual.onBootResult(0, true);
  manual.onTransportFailure(1000);
  assert(manual.state() == CompanionState::Offline);
  assert(manual.stats().outages == 1 && manual.stats().faults == 1);
}

// ---- console mirror policy ----------------------------------------------------
constexpr uint32_t kHealthy = 600000;  // the shipped 10 minutes

static void the_mirror_is_not_wanted_without_a_fault_or_the_setting() {
  ConsoleMirrorPolicy mirror(kHealthy);
  for (uint32_t now = 0; now < 3 * kHealthy; now += 1000) {
    assert(mirror.tick(now, CompanionState::Online, /*userOn=*/false, /*running=*/false) == MirrorStep::None);
  }
  assert(!mirror.armed());
}

static void a_fault_starts_it_and_ten_healthy_minutes_stop_it() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(1000);
  assert(mirror.armed());
  assert(mirror.tick(1000, CompanionState::Recovering, false, false) == MirrorStep::Start);
  // Recovering for 20 s, then online: the healthy time starts at the return.
  assert(mirror.tick(11000, CompanionState::Recovering, false, true) == MirrorStep::None);
  assert(mirror.tick(21000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(21000 + kHealthy - 1, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(21000 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
  assert(!mirror.armed());
  // Handed out once, also while the stop has not run yet.
  assert(mirror.tick(21000 + kHealthy + 10, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(21000 + kHealthy + 20, CompanionState::Online, false, false) == MirrorStep::None);
}

static void a_second_fault_restarts_the_healthy_time() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Recovering, false, false) == MirrorStep::Start);
  assert(mirror.tick(5000, CompanionState::Online, false, true) == MirrorStep::None);
  const uint32_t second = 5000 + kHealthy - 60000;  // nine minutes in
  mirror.onFault(second);
  assert(mirror.tick(second, CompanionState::Recovering, false, true) == MirrorStep::None);
  assert(mirror.tick(second + 5000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(5000 + kHealthy + 1000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(second + 5000 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
}

static void a_wedged_companion_keeps_it_and_counts_from_its_return() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Offline, false, false) == MirrorStep::Start);
  // Offline for an hour, with no further fault reported in the last 15 minutes.
  for (uint32_t now = 1000; now <= 3600000; now += 1000) {
    assert(mirror.tick(now, CompanionState::Offline, false, true) == MirrorStep::None);
  }
  // Back: not stopped at once although the last fault is long past.
  assert(mirror.tick(3601000, CompanionState::Starting, false, true) == MirrorStep::None);
  assert(mirror.tick(3602000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(3602000 + kHealthy - 1, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(3602000 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
}

static void restarts_on_purpose_do_not_break_the_healthy_time() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Recovering, false, false) == MirrorStep::Start);
  assert(mirror.tick(1000, CompanionState::Online, false, true) == MirrorStep::None);
  // c6restart, a hold and release, an update, an idle close: no fault, no reset.
  assert(mirror.tick(100000, CompanionState::Recovering, false, true) == MirrorStep::None);
  assert(mirror.tick(110000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(200000, CompanionState::Held, false, true) == MirrorStep::None);
  assert(mirror.tick(300000, CompanionState::Starting, false, true) == MirrorStep::None);
  assert(mirror.tick(400000, CompanionState::Updating, false, true) == MirrorStep::None);
  assert(mirror.tick(500000, CompanionState::Idle, false, true) == MirrorStep::None);
  // Not stopped in the middle of a restart, even past the time.
  assert(mirror.tick(1000 + kHealthy, CompanionState::Recovering, false, true) == MirrorStep::None);
  assert(mirror.tick(1000 + kHealthy + 5000, CompanionState::Held, false, true) == MirrorStep::Stop);
}

static void a_failed_start_is_not_repeated_until_the_next_arming() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Offline, false, false) == MirrorStep::Start);
  // The start failed (no memory): running stays false. No second attempt,
  // also not for the failed retries of the same outage.
  for (uint32_t now = 10; now < 100000; now += 10) {
    if (now % 5000 == 0) mirror.onFault(now);
    assert(mirror.tick(now, CompanionState::Offline, false, false) == MirrorStep::None);
  }
  // Healthy for ten minutes: let go. The next fault is a new arming.
  assert(mirror.tick(100000, CompanionState::Online, false, false) == MirrorStep::None);
  assert(mirror.tick(100000 + kHealthy, CompanionState::Online, false, false) == MirrorStep::None);
  assert(!mirror.armed());
  mirror.onFault(100000 + kHealthy + 1000);
  assert(mirror.tick(100000 + kHealthy + 1000, CompanionState::Recovering, false, false) == MirrorStep::Start);
}

static void a_step_the_worker_did_not_take_is_offered_again() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Recovering, false, false) == MirrorStep::Start);
  mirror.notTaken(MirrorStep::Start);  // the command queue was full
  assert(mirror.tick(10, CompanionState::Recovering, false, false) == MirrorStep::Start);
  assert(mirror.tick(20, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(20 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
  mirror.notTaken(MirrorStep::Stop);
  assert(mirror.tick(30 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
}

static void the_saved_setting_keeps_it_on() {
  ConsoleMirrorPolicy mirror(kHealthy);
  // Boot with `c6console on` saved: one start, then nothing, however long.
  assert(mirror.tick(0, CompanionState::Online, true, false) == MirrorStep::Start);
  for (uint32_t now = 1000; now < 3 * kHealthy; now += 1000) {
    assert(mirror.tick(now, CompanionState::Online, true, true) == MirrorStep::None);
  }
  // A fault and its ten healthy minutes do not stop a mirror the user keeps on.
  mirror.onFault(3 * kHealthy);
  assert(mirror.tick(3 * kHealthy, CompanionState::Recovering, true, true) == MirrorStep::None);
  assert(mirror.tick(3 * kHealthy + 1000, CompanionState::Online, true, true) == MirrorStep::None);
  assert(mirror.tick(5 * kHealthy, CompanionState::Online, true, true) == MirrorStep::None);
  assert(!mirror.armed());
  // Setting off without the command (the settings editor): stopped once.
  assert(mirror.tick(5 * kHealthy + 1000, CompanionState::Online, false, true) == MirrorStep::Stop);
  assert(mirror.tick(5 * kHealthy + 2000, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(5 * kHealthy + 3000, CompanionState::Online, false, false) == MirrorStep::None);
  // Setting on again without the command: started once, also after a failed start.
  assert(mirror.tick(5 * kHealthy + 4000, CompanionState::Online, true, false) == MirrorStep::Start);
  assert(mirror.tick(5 * kHealthy + 5000, CompanionState::Online, true, false) == MirrorStep::None);
}

static void off_by_hand_ends_a_fault_mirror_until_the_next_fault() {
  ConsoleMirrorPolicy mirror(kHealthy);
  mirror.onFault(0);
  assert(mirror.tick(0, CompanionState::Recovering, false, false) == MirrorStep::Start);
  assert(mirror.tick(1000, CompanionState::Online, false, true) == MirrorStep::None);
  mirror.onUserOff();  // `c6console off`: the owner stopped the mirror itself
  assert(!mirror.armed());
  for (uint32_t now = 2000; now < 2 * kHealthy; now += 1000) {
    assert(mirror.tick(now, CompanionState::Online, false, false) == MirrorStep::None);
  }
  mirror.onFault(2 * kHealthy);
  assert(mirror.tick(2 * kHealthy, CompanionState::Recovering, false, false) == MirrorStep::Start);
}

static void the_healthy_time_survives_a_clock_wrap() {
  ConsoleMirrorPolicy mirror(kHealthy);
  const uint32_t fault = 0xFFFFFFFFu - 1000;
  mirror.onFault(fault);
  assert(mirror.tick(fault, CompanionState::Recovering, false, false) == MirrorStep::Start);
  assert(mirror.tick(fault + 500, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(fault + 500 + kHealthy - 1, CompanionState::Online, false, true) == MirrorStep::None);
  assert(mirror.tick(fault + 500 + kHealthy, CompanionState::Online, false, true) == MirrorStep::Stop);
}

int main() {
  the_bench_retry_with_a_wedged_host_waits_out_the_budget();
  a_start_waits_for_the_slave_after_hosted_gives_up_at_once();
  a_slave_that_never_answers_waits_only_the_budget();
  hosted_already_spent_the_budget_so_no_second_wait();
  an_up_link_does_not_wait();
  the_wait_survives_a_clock_wrap();
  heartbeats_keep_online_and_silence_recovers();
  manual_mode_waits_for_the_operator();
  boot_failure_retries_with_backoff();
  repeated_outages_escalate_to_a_reboot();
  orderly_down_is_idle_and_may_hold();
  sleep_pauses_the_watchdog();
  unexpected_reset_recovers_and_update_suppresses();
  an_incident_counts_once_and_its_retries_do_not();
  restarts_on_purpose_are_not_incidents();
  every_way_out_of_an_outage_counts_it_once();
  the_mirror_is_not_wanted_without_a_fault_or_the_setting();
  a_fault_starts_it_and_ten_healthy_minutes_stop_it();
  a_second_fault_restarts_the_healthy_time();
  a_wedged_companion_keeps_it_and_counts_from_its_return();
  restarts_on_purpose_do_not_break_the_healthy_time();
  a_failed_start_is_not_repeated_until_the_next_arming();
  a_step_the_worker_did_not_take_is_offered_again();
  the_saved_setting_keeps_it_on();
  off_by_hand_ends_a_fault_mirror_until_the_next_fault();
  the_healthy_time_survives_a_clock_wrap();
  puts("radio companion core tests passed");
  return 0;
}
