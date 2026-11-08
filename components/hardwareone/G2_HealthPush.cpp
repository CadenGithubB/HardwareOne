// Stock G2 Health panel feed from R1 data. See G2_HealthPush.h.
#include "G2_HealthPush.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>

#include "G2_Health.h"
#include "G2_HealthPushCore.h"
#include "G2_Ring.h"
#include "R1_HealthHistoryStore.h"
#include "System_R1_Protocol.h"
#include "System_Clock.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_Utils.h"

namespace hp = hw1_g2_health_push;

namespace {

constexpr uint32_t kRefreshMs = 10000;       // rebuild the snapshot at most this often
constexpr int32_t kLiveSampleMaxAgeSec = 3600;
constexpr uint32_t kDayFreshSec = 25u * 3600u;  // local day + DST slack
constexpr uint32_t kManualFloorMs = 1000;    // `ring panel send` rate bound
// Ring history (steps, kcal, sleep) refresh while the feed runs: the first one
// shortly after the right temple and the ring are both up, then every
// g2HealthPanelHistoryMin.
constexpr uint32_t kHistoryFirstDelayMs = 15000;
constexpr uint32_t kHistoryRetryMs = 2u * 60u * 1000u;  // after a refusal with the ring up
constexpr int kHistoryMinMinutes = 10, kHistoryMaxMinutes = 1440;

std::atomic<uint8_t> sForce{0};  // 0 none, 1 current snapshot, 2 test snapshot

bool feedOn() { return gSettings.g2HealthPanelFeed; }

uint32_t historyIntervalMs() {
  int m = gSettings.g2HealthPanelHistoryMin;
  if (m < kHistoryMinMinutes) m = kHistoryMinMinutes;
  if (m > kHistoryMaxMinutes) m = kHistoryMaxMinutes;
  return (uint32_t)m * 60u * 1000u;
}

// Owned by the G2 control task: link, send policy, writes and echoes.
hp::Policy sPolicy;
uint8_t sMagic = 0;

// Owned by the main loop (g2HealthPushMainTick): building the snapshot from
// the R1 data and scheduling ring history refreshes. The G2 task never takes
// the history lock or starts ring work.
uint32_t sLastRefreshMs = 0;
R1HealthHistoryDay* sDay = nullptr;
uint32_t sLinkUpMs = 0;          // when the right temple was first seen ready (0 = down)
uint32_t sRingUpMs = 0;          // when the R1 link came up (0 = down)
uint32_t sNextHistoryMs = 0;     // next history request is due at/after this
bool sHistoryScheduled = false;

// Read by commands on other tasks.
struct Status {
  bool linkReady;
  bool busy;
  uint32_t sends;
  uint32_t sendFailures;
  uint32_t acks;
  uint32_t lastSendMs;
  uint32_t lastAckMs;
  uint8_t lastMagic;
  bool awaitingAck;
  bool lastWasTest;
  uint32_t historyRequests;
  uint32_t historyRefused;
  uint32_t lastHistoryMs;
  hp::Snapshot lastSent;
  hp::Snapshot current;  // published by the main loop
  bool haveCurrent;
};
Status sStatus{};
portMUX_TYPE sStatusMux = portMUX_INITIALIZER_UNLOCKED;

// Recognisable synthetic values for the first device check (E1).
hp::Snapshot testSnapshot() {
  hp::Input in;
  in.steps = 1234;
  in.stepGoal = 10000;
  in.kcal = 210;
  in.kcalGoal = 300;
  in.sleepScore = 80;  // 0-100; a recognisable test value
  in.sleepSeconds = 27000;
  in.heartRate = 72;
  in.heartRateAvg = 68;
  in.spo2 = 98;
  in.spo2Avg = 97;
  return hp::sanitize(in);
}

int32_t hourlyMean(const R1HealthHourlyMetricDay& m) {
  if (!m.have) return -1;
  uint32_t sum = 0, n = 0;
  for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; i++) {
    const R1HealthHourlyBucket& b = m.slots[i];
    if (b.valid && b.average > 0) {
      sum += b.average;
      n++;
    }
  }
  return n ? (int32_t)((sum + n / 2) / n) : -1;
}

// Today's last measured value: the day record's latest sample, else the
// newest hourly bucket (the ring does not always send a latest sample).
struct Measured {
  int32_t value = -1;
  uint32_t epoch = 0;
};
Measured lastMeasured(const R1HealthHourlyMetricDay& m) {
  Measured out;
  if (m.latestValid) {
    out.value = m.latestValue;
    out.epoch = m.latestTimestamp;
  }
  if (!m.have) return out;
  for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; i++) {
    const R1HealthHourlyBucket& b = m.slots[i];
    if (b.valid && b.average > 0 && (out.value < 0 || b.bucketEpoch > out.epoch)) {
      out.value = b.average;
      out.epoch = b.bucketEpoch;
    }
  }
  return out;
}

// Unknown inputs stay negative; sanitize() turns them into "-" on the lens.
hp::Input buildInput() {
  hp::Input in;
  G2HealthHistorySummary sum{};
  g2HealthHistoryGetSummary(sum);
  const time_t now = Clock::epochSeconds();
  const bool today = sum.available && Clock::isSynced() && sum.dayStart != 0 &&
                     (uint32_t)now >= sum.dayStart &&
                     (uint32_t)now - sum.dayStart < kDayFreshSec;
  if (today && sum.activity.available) {
    in.steps = (int32_t)sum.activity.steps;
    in.kcal = (int32_t)sum.activity.activeKcal;  // the app's goal (300) is active kcal
  }
  if (today && sum.sleep.available) {
    // Sleep is filed under the day its period ends, so "today" is last night.
    hp::SleepStages st;
    st.asleepSec = sum.sleep.asleepSeconds;
    st.awakeSec = sum.sleep.stageSeconds[R1_SLEEP_STAGE_AWAKE];
    st.remSec = sum.sleep.stageSeconds[R1_SLEEP_STAGE_REM];
    st.lightSec = sum.sleep.stageSeconds[R1_SLEEP_STAGE_LIGHT];
    st.deepSec = sum.sleep.stageSeconds[R1_SLEEP_STAGE_DEEP];
    in.sleepScore = (int32_t)hp::sleepScore(st);
    in.sleepSeconds = (int32_t)sum.sleep.asleepSeconds;
  }

  bool haveDay = false;
  if (today) {
    if (!sDay)
      sDay = static_cast<R1HealthHistoryDay*>(
          ps_alloc(sizeof(R1HealthHistoryDay), AllocPolicy::PreferPSRAM, "g2.healthpush.day"));
    haveDay = sDay && g2HealthHistorySnapshot(*sDay) && sDay->dayStart == sum.dayStart;
  }
  Measured hr, spo2;
  if (haveDay) {
    in.heartRateAvg = hourlyMean(sDay->heartRate);
    in.spo2Avg = hourlyMean(sDay->spo2);
    hr = lastMeasured(sDay->heartRate);
    spo2 = lastMeasured(sDay->spo2);
  }
  // Like the Even app, the value shown is the last one measured today (the
  // ring takes SpO2 only every few hours): a live reading wins when it is
  // newer than the day record's. Without a synced clock or a record for
  // today, only a live reading from the last hour counts.
  G2RingTelemetry tel{};
  g2RingGetTelemetry(tel);
  const uint32_t nowSec = (uint32_t)now;
  auto pick = [&](bool valid, int32_t ageSec, int32_t live, const Measured& day) {
    const bool liveOk = valid && ageSec >= 0 &&
                        (today ? (uint32_t)ageSec <= nowSec - sum.dayStart
                               : ageSec <= kLiveSampleMaxAgeSec);
    if (liveOk && (day.value < 0 || nowSec - (uint32_t)ageSec >= day.epoch)) return live;
    return day.value;
  };
  in.heartRate = pick(tel.hrValid, tel.hrAgeSec, tel.hr, hr);
  in.spo2 = pick(tel.spo2Valid, tel.spo2AgeSec, tel.spo2, spo2);
  return in;
}

void publishStatus(bool ready, bool busy) {
  portENTER_CRITICAL(&sStatusMux);
  sStatus.linkReady = ready;
  sStatus.busy = busy;
  sStatus.awaitingAck = sPolicy.awaitingAck;
  portEXIT_CRITICAL(&sStatusMux);
}

bool sendSnapshot(const hp::Snapshot& s, uint32_t generation, uint32_t now, bool test) {
  uint8_t pb[hp::kMaxPbBytes];
  sMagic = hp::nextMagic(sMagic);
  const size_t n = hp::encodeSnapshot(s, sMagic, pb, sizeof pb);
  sPolicy.pending = s;
  const bool ok = n && hp::validateSnapshotPb(pb, n) && g2HealthWireSend(generation, pb, n);
  if (ok) hp::policySent(sPolicy, now, sMagic);
  else hp::policySendFailed(sPolicy, now);
  DEBUG_G2F("[G2-HEALTH] %s snapshot magic=%u len=%u steps=%u kcal=%u hr=%u/%u spo2=%u/%u sleep=%us %s",
            test ? "test" : "live", (unsigned)sMagic, (unsigned)n, (unsigned)s.steps,
            (unsigned)s.kcal, (unsigned)s.heartRate, (unsigned)s.heartRateAvg,
            (unsigned)s.spo2, (unsigned)s.spo2Avg, (unsigned)s.sleepSeconds,
            ok ? "sent" : "NOT sent");
  portENTER_CRITICAL(&sStatusMux);
  if (ok) {
    sStatus.sends++;
    sStatus.lastSendMs = now;
    sStatus.lastMagic = sMagic;
    sStatus.lastSent = s;
    sStatus.lastWasTest = test;
  } else {
    sStatus.sendFailures++;
  }
  portEXIT_CRITICAL(&sStatusMux);
  return ok;
}

void formatSnapshot(char* out, size_t cap, const hp::Snapshot& s) {
  char score[12] = "-";  // the glasses draw a score of 0 as "-"
  if (s.sleepScore) snprintf(score, sizeof score, "%u", (unsigned)s.sleepScore);
  snprintf(out, cap,
           "steps %u/%u  kcal %u/%u  sleep %uh%02um score %s  HR %u (avg %u)  SpO2 %u (avg %u)  "
           "productivity -",
           (unsigned)s.steps, (unsigned)s.stepGoal, (unsigned)s.kcal, (unsigned)s.kcalGoal,
           (unsigned)(s.sleepSeconds / 3600), (unsigned)((s.sleepSeconds % 3600) / 60), score,
           (unsigned)s.heartRate, (unsigned)s.heartRateAvg, (unsigned)s.spo2,
           (unsigned)s.spo2Avg);
}

// Ask the ring owner for a normal (freshness-gated) history sweep on the
// feed's schedule: the first one shortly after BOTH the right temple and the
// ring are up (after a boot the glasses usually connect first), then every
// g2HealthPanelHistoryMin. It waits while the health logger's vitals read is
// in flight, so the two never start together. While the ring is down nothing
// is requested or counted; a refusal with the ring up (e.g. health collection
// off) is retried.
void historyTick(uint32_t now, bool vitalsReadActive) {
  const bool ringUp = g2RingIsConnected();
  if (!ringUp) sRingUpMs = 0;
  else if (!sRingUpMs) sRingUpMs = now ? now : 1;
  if (!feedOn() || !sLinkUpMs || !sRingUpMs) {
    sHistoryScheduled = false;
    return;
  }
  if (!sHistoryScheduled) {
    const uint32_t since = (int32_t)(sRingUpMs - sLinkUpMs) > 0 ? sRingUpMs : sLinkUpMs;
    sNextHistoryMs = since + kHistoryFirstDelayMs;
    sHistoryScheduled = true;
  }
  if ((int32_t)(now - sNextHistoryMs) < 0 || vitalsReadActive) return;
  const bool accepted = g2RingRequestHistoryRefresh(false);
  sNextHistoryMs = now + (accepted ? historyIntervalMs() : kHistoryRetryMs);
  portENTER_CRITICAL(&sStatusMux);
  if (accepted) {
    sStatus.historyRequests++;
    sStatus.lastHistoryMs = now;
  } else {
    sStatus.historyRefused++;
  }
  portEXIT_CRITICAL(&sStatusMux);
  DEBUG_G2F("[G2-HEALTH] ring history refresh %s", accepted ? "requested" : "refused");
}

bool linkReadyPublished() {
  portENTER_CRITICAL(&sStatusMux);
  const bool ready = sStatus.linkReady;
  portEXIT_CRITICAL(&sStatusMux);
  return ready;
}

}  // namespace

bool g2HealthPushWantsRingData() { return feedOn() && linkReadyPublished(); }

void g2HealthPushMainTick(bool vitalsReadActive) {
  const uint32_t now = millis();
  const bool ready = linkReadyPublished();
  if (!ready) sLinkUpMs = 0;
  else if (!sLinkUpMs) sLinkUpMs = now ? now : 1;
  // The snapshot is only needed while the glasses are there (status and
  // preview show the last one otherwise).
  bool have;
  portENTER_CRITICAL(&sStatusMux);
  have = sStatus.haveCurrent;
  portEXIT_CRITICAL(&sStatusMux);
  if (ready && (!have || (uint32_t)(now - sLastRefreshMs) >= kRefreshMs)) {
    const hp::Snapshot snap = hp::sanitize(buildInput());
    sLastRefreshMs = now;
    portENTER_CRITICAL(&sStatusMux);
    sStatus.current = snap;
    sStatus.haveCurrent = true;
    portEXIT_CRITICAL(&sStatusMux);
  }
  historyTick(now, vitalsReadActive);
}

void g2HealthPushTick() {
  const uint32_t now = millis();
  uint32_t generation = 0;
  bool busy = false;
  const bool ready = g2HealthRightLink(&generation, &busy);
  if (!ready) {
    // A one-shot request never outlives the link it was made for.
    if (sForce.exchange(0, std::memory_order_acq_rel)) {
      portENTER_CRITICAL(&sStatusMux);
      sStatus.sendFailures++;
      portEXIT_CRITICAL(&sStatusMux);
    }
    hp::policyTick(sPolicy, false, false, 0, now, hp::Snapshot{});
    publishStatus(false, busy);
    return;
  }
  // Exclusive glasses sessions pause the feed without resetting the link
  // state, so nothing bursts out when they end.
  if (busy) {
    publishStatus(true, true);
    return;
  }
  hp::Snapshot current;
  bool have;
  portENTER_CRITICAL(&sStatusMux);
  current = sStatus.current;
  have = sStatus.haveCurrent;
  portEXIT_CRITICAL(&sStatusMux);
  const uint8_t force = sForce.exchange(0, std::memory_order_acq_rel);
  const hp::Action action =
      hp::policyTick(sPolicy, feedOn() && have, true, generation, now, current);
  if (force && sPolicy.sentThisLink &&
      (uint32_t)(now - sPolicy.lastSendMs) < kManualFloorMs) {
    portENTER_CRITICAL(&sStatusMux);
    sStatus.sendFailures++;  // manual sends are bounded too
    portEXIT_CRITICAL(&sStatusMux);
  } else if (force == 1 && (!have || !hp::hasRingData(current))) {
    portENTER_CRITICAL(&sStatusMux);
    sStatus.sendFailures++;  // never blank the panel, not even by hand
    portEXIT_CRITICAL(&sStatusMux);
  } else if (force) {
    (void)sendSnapshot(force == 2 ? testSnapshot() : current, generation, now, force == 2);
  } else if (action == hp::Action::Send) {
    (void)sendSnapshot(sPolicy.pending, generation, now, false);
  }
  publishStatus(true, false);
}

void g2HealthPushOnRx(uint32_t rightGeneration, const uint8_t* pb, size_t len) {
  uint8_t cmd = 0, magic = 0;
  if (rightGeneration != sPolicy.generation || !hp::parseAck(pb, len, cmd, magic)) return;
  const bool matched = hp::policyAck(sPolicy, cmd, magic);
  DEBUG_G2F("[G2-HEALTH] echo cmd=%u magic=%u %s", (unsigned)cmd, (unsigned)magic,
            matched ? "acked" : "unmatched");
  if (!matched) return;
  portENTER_CRITICAL(&sStatusMux);
  sStatus.acks++;
  sStatus.lastAckMs = millis();
  sStatus.awaitingAck = false;
  portEXIT_CRITICAL(&sStatusMux);
}

// ------------------------------------------------------------------ commands

const char* cmd_ringpanelstatus(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  Status s;
  portENTER_CRITICAL(&sStatusMux);
  s = sStatus;
  portEXIT_CRITICAL(&sStatusMux);
  EXT_RAM_BSS_ATTR static char buf[1024];
  char cur[160], last[160], sentAgo[24], ackAgo[24], histAgo[24];
  formatSnapshot(cur, sizeof cur, s.current);
  formatSnapshot(last, sizeof last, s.lastSent);
  const uint32_t now = millis();
  if (s.sends) snprintf(sentAgo, sizeof sentAgo, "%u s ago", (unsigned)((now - s.lastSendMs) / 1000));
  else snprintf(sentAgo, sizeof sentAgo, "never");
  if (s.acks) snprintf(ackAgo, sizeof ackAgo, "%u s ago", (unsigned)((now - s.lastAckMs) / 1000));
  else snprintf(ackAgo, sizeof ackAgo, "never");
  if (s.historyRequests)
    snprintf(histAgo, sizeof histAgo, "%u min ago", (unsigned)((now - s.lastHistoryMs) / 60000));
  else snprintf(histAgo, sizeof histAgo, "never");
  snprintf(buf, sizeof buf,
           "Glasses Health panel feed: %s (saved)\n"
           "Right temple: %s%s; ring: %s\n"
           "Current:   %s%s\n"
           "Last sent: %s%s\n"
           "Sends %u, acked %u, failed %u; last send %s, last ack %s%s\n"
           "Ring history refresh every %u min while the feed runs: %u requested (last %s), "
           "%u refused; vitals read every %u s (the healthlogging interval setting)",
           feedOn() ? "ON" : "off", s.linkReady ? "ready" : "not ready",
           s.busy ? " (paused: another glasses session is active)" : "",
           g2RingIsConnected() ? "connected" : "not connected", cur,
           hp::hasRingData(s.current) ? "" : "\n           (no ring data yet; nothing is sent until there is)",
           s.sends ? last : "(nothing)", s.sends && s.lastWasTest ? " [test values]" : "",
           (unsigned)s.sends, (unsigned)s.acks, (unsigned)s.sendFailures, sentAgo, ackAgo,
           s.awaitingAck ? "; waiting for the glasses' echo" : "",
           (unsigned)(historyIntervalMs() / 60000), (unsigned)s.historyRequests, histAgo,
           (unsigned)s.historyRefused, (unsigned)gSettings.healthLoggingPollIntervalSec);
  return buf;
}

namespace {
const char* setFeed(bool on) {
  setSetting(gSettings.g2HealthPanelFeed, on);
  return on ? "Glasses Health panel feed ON (saved): pushes on connect and when the ring "
              "data changes (at most once a minute)"
            : "Glasses Health panel feed off (saved). The glasses keep the last snapshot "
              "until they reboot or the Even app sends one";
}
}  // namespace

const char* cmd_ringpanelon(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setFeed(true);
}

const char* cmd_ringpaneloff(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setFeed(false);
}

const char* cmd_ringpanelfeed(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  a.toLowerCase();
  if (!a.length())
    return feedOn() ? "Glasses Health panel feed: on" : "Glasses Health panel feed: off";
  if (a == "on" || a == "1" || a == "true") return setFeed(true);
  if (a == "off" || a == "0" || a == "false") return setFeed(false);
  return "Error: invalid arguments — Usage: ring panel feed [on|off]";
}

const char* cmd_ringpanelhistory(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[96];
  String a = args;
  a.trim();
  if (!a.length()) {
    snprintf(buf, sizeof buf, "Ring history refresh for the Health panel: every %u min",
             (unsigned)(historyIntervalMs() / 60000));
    return buf;
  }
  for (size_t i = 0; i < a.length(); i++)
    if (a[i] < '0' || a[i] > '9')
      return "Error: invalid arguments — Usage: ring panel history [minutes 10-1440]";
  const long m = a.length() <= 4 ? a.toInt() : -1;
  if (m < kHistoryMinMinutes || m > kHistoryMaxMinutes)
    return "Error: invalid arguments — Usage: ring panel history [minutes 10-1440]";
  setSetting(gSettings.g2HealthPanelHistoryMin, (int)m);
  snprintf(buf, sizeof buf, "Ring history refresh for the Health panel: every %ld min (saved)", m);
  return buf;
}

const char* cmd_ringpanelsend(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length() && a != "test")
    return "Error: invalid arguments — Usage: ring panel send [test]";
  sForce.store(a == "test" ? 2 : 1);
  return a == "test"
             ? "Queued one test snapshot (1234 steps, 210 kcal, sleep score 80 / 7h30m, 72 bpm, 98%) "
               "to the right temple"
             : "Queued one snapshot of the current ring data to the right temple";
}

const char* cmd_ringpanelpreview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length() && a != "test")
    return "Error: invalid arguments — Usage: ring panel preview [test]";
  hp::Snapshot s;
  if (a == "test") {
    s = testSnapshot();
  } else {
    portENTER_CRITICAL(&sStatusMux);
    s = sStatus.current;
    portEXIT_CRITICAL(&sStatusMux);
  }
  uint8_t pb[hp::kMaxPbBytes];
  const size_t n = hp::encodeSnapshot(s, 0x01, pb, sizeof pb);
  const bool valid = n && hp::validateSnapshotPb(pb, n);
  EXT_RAM_BSS_ATTR static char buf[160 + 2 * hp::kMaxPbBytes + 96];
  char line[160];
  formatSnapshot(line, sizeof line, s);
  int off = snprintf(buf, sizeof buf, "%s\nsid 0x0E, %u B, whitelist %s (not sent):\n",
                     line, (unsigned)n, valid ? "pass" : "FAIL");
  for (size_t i = 0; i < n && off > 0 && (size_t)off + 3 < sizeof buf; i++)
    off += snprintf(buf + off, sizeof buf - off, "%02x", pb[i]);
  return buf;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH
