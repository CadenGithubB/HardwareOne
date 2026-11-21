// Stock G2 Health panel insights page from R1 ring history. See
// G2_HealthInsights.h.
#include "G2_HealthInsights.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>

#include "G2_Health.h"
#include "G2_HealthInsightsCore.h"
#include "G2_HealthPush.h"
#include "R1_HealthHistoryStore.h"
#include "System_Clock.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_Utils.h"

namespace hi = hw1_g2_health_insights;
namespace feed = hw1_g2_feed;

namespace {

constexpr uint32_t kDaySec = 86400;
constexpr uint32_t kRefreshMs = 30000;  // re-read today's RAM day at most this often
constexpr uint8_t kLookbackDays = 14;   // calendar days searched for days with data
constexpr uint32_t kManualFloorMs = 1000;  // `ring insights send` rate bound
// `ring insights send` sends only lines computed this recently. While the
// feed runs they are at most kRefreshMs old; with the feed off nothing
// recomputes them, so the command asks for a recompute and refuses older ones.
constexpr uint32_t kManualFreshMs = kRefreshMs + 15000;

bool insightsSwitchOn() { return gSettings.g2HealthInsightsFeed; }
// The insights feed only runs while the Health panel feed runs.
bool feedOn() { return gSettings.g2HealthPanelFeed && insightsSwitchOn(); }
uint8_t windowDays() { return hi::clampDays(gSettings.g2HealthInsightsDays); }

// ------------------------------------------------ published message (PSRAM)
// Written by the main loop, read by the G2 control task and commands under
// sPubMux. The version moves only when the body bytes change; it is the send
// policy's key.
struct Published {
  bool have;
  uint32_t version;
  uint32_t computedMs;
  uint8_t daysUsed;
  uint16_t bodyLen;
  uint8_t body[hi::kMaxBodyBytes];
  hi::Insights lines;
};
std::atomic<Published*> sPub{nullptr};
portMUX_TYPE sPubMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> sComputeRequest{false};  // commands: recompute on the next main-loop pass

// ------------------------------------------------ main loop state
struct DayKey {
  char peer[18];
  uint32_t dayStart;
  int16_t tz;
};

struct MainState {
  R1HealthHistoryDay day;  // RAM snapshot or a day loaded from the store
  hi::DaySummary ramSum;   // the RAM day, refreshed every kRefreshMs
  DayKey ram;
  bool haveRam;
  uint32_t todayStart;
  // Older days with data (never the RAM day), newest first, from the last
  // completed scan of the store, and what that scan depended on.
  hi::DaySummary past[hi::kMaxDays];
  uint8_t pastCount;
  bool pastValid;
  hi::ScanKey pastKey;
  // Scan in progress: one calendar day per main-loop pass.
  bool scanning;
  uint8_t scanK;
  uint8_t scanFound;
  hi::ScanKey scanKey;
  hi::DaySummary scanDays[hi::kMaxDays];
  uint32_t lastRefreshMs;
  uint32_t loadErrors;
  // publish() scratch, kept here (PSRAM) instead of on the main-loop stack.
  hi::DaySummary all[hi::kMaxDays + 1];
  hi::DaySummary win[hi::kMaxDays];
  hi::Insights ins;
  uint8_t body[hi::kMaxBodyBytes];
};
MainState* sMain = nullptr;

// ------------------------------------------------ G2 control task state
hi::Policy sPolicy;
std::atomic<bool> sForce{false};
struct OwnerBufs {
  uint8_t body[hi::kMaxBodyBytes];
  uint8_t msg[hi::kMaxPbBytes];
};
OwnerBufs* sOwner = nullptr;

// Read by commands.
struct Status {
  feed::Counters feed;
  uint32_t lastSentVersion;
  uint8_t lastSentCount;
  uint16_t lastSentLen;
  bool lastWasManual;
  uint8_t lastAckItems;
  uint8_t lastAckStatus;
  bool lastAckHaveStatus;
  uint32_t scans;
  uint32_t loadErrors;
};
Status sStatus{};
portMUX_TYPE sStatusMux = portMUX_INITIALIZER_UNLOCKED;

// ------------------------------------------------ main loop: history -> text

// isToday is set later by hi::mergeDays, from the calendar day.
hi::DaySummary summarize(const R1HealthHistoryDay& d) {
  hi::DaySummary s;
  s.dayStart = d.dayStart;
  struct M {
    const R1HealthHourlyMetricDay& m;
    hi::MetricAgg& agg;
    const hi::Range& range;
  } metrics[] = {{d.heartRate, s.hr, hi::kHrRange},
                 {d.hrv, s.hrv, hi::kHrvRange},
                 {d.spo2, s.spo2, hi::kSpo2Range}};
  for (const M& m : metrics) {
    if (!m.m.have) continue;
    for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; i++) {
      const R1HealthHourlyBucket& b = m.m.slots[i];
      hi::addHour(m.agg, m.range, b.valid, b.average, b.minimum, b.maximum);
    }
  }
  R1HealthActivitySummary a{};
  r1HealthHistoryActivitySummary(d, a);
  hi::setSteps(s, a.available && a.bucketCount > 0, a.steps);
  R1HealthSleepSummary sl{};
  r1HealthHistorySleepSummary(d, sl);
  hi::setSleep(s, sl.available, sl.asleepSeconds);
  return s;
}

bool ensureMain() {
  if (sMain) return true;
  sMain = static_cast<MainState*>(
      ps_calloc(1, sizeof(MainState), AllocPolicy::PreferPsram, "g2.insights.main"));
  if (!sMain) return false;
  Published* p = static_cast<Published*>(
      ps_calloc(1, sizeof(Published), AllocPolicy::PreferPsram, "g2.insights.pub"));
  if (!p) {
    ps_free(sMain);
    sMain = nullptr;
    return false;
  }
  sPub.store(p, std::memory_order_release);
  return true;
}

// Window = RAM day (fresh) merged into the scanned older days, newest first;
// a stored copy of the RAM day is dropped and today's copy, whichever it is,
// is the partial day.
void publish() {
  MainState& m = *sMain;
  const size_t n = hi::mergeDays(m.haveRam ? &m.ramSum : nullptr, m.past, m.pastCount,
                                 m.todayStart, m.all, hi::kMaxDays + 1);
  const size_t wn = hi::selectWindow(m.all, n, windowDays(), m.win);
  hi::buildInsightsInto(m.win, wn, m.ins);
  const hi::Insights& ins = m.ins;
  uint8_t* body = m.body;
  const size_t len = hi::encodeBody(ins, body, sizeof m.body);
  if (!len) return;  // cannot happen for built lines; keep the last message
  Published* p = sPub.load(std::memory_order_acquire);
  bool changed;
  portENTER_CRITICAL(&sPubMux);
  changed = !p->have || p->bodyLen != len || memcmp(p->body, body, len) != 0;
  if (changed) {
    memcpy(p->body, body, len);
    p->bodyLen = (uint16_t)len;
    p->lines = ins;
    p->version++;
    p->have = true;
  }
  p->daysUsed = (uint8_t)wn;
  p->computedMs = millis();
  portEXIT_CRITICAL(&sPubMux);
  if (changed)
    DEBUG_G2F("[G2-INSIGHTS] %u line(s) over %u day(s), body %u B", (unsigned)ins.count,
              (unsigned)wn, (unsigned)len);
}

// The calendar day k days before today, in up to three spellings: the RAM
// day's offset, and an hour either way for a DST change in between (a day's
// local midnight moves by the offset change).
void scanStep() {
  MainState& m = *sMain;
  if (m.scanK >= kLookbackDays || m.scanFound >= hi::kMaxDays) {
    memcpy(m.past, m.scanDays, sizeof m.past);
    m.pastCount = m.scanFound;
    m.pastValid = true;
    m.pastKey = m.scanKey;
    m.scanning = false;
    portENTER_CRITICAL(&sStatusMux);
    sStatus.scans++;
    sStatus.loadErrors = m.loadErrors;
    portEXIT_CRITICAL(&sStatusMux);
    publish();
    return;
  }
  const uint32_t base = m.todayStart - (uint32_t)m.scanK * kDaySec;
  const struct {
    uint32_t dayStart;
    int tz;
  } variants[] = {{base, m.ram.tz}, {base - 3600u, m.ram.tz + 60}, {base + 3600u, m.ram.tz - 60}};
  for (const auto& v : variants) {
    if (v.tz < R1_HEALTH_TIMEZONE_MINUTES_MIN || v.tz > R1_HEALTH_TIMEZONE_MINUTES_MAX) continue;
    // The RAM day is merged fresh in publish(), never from the store.
    if (v.dayStart == m.ram.dayStart && v.tz == m.ram.tz) break;
    // Quiet: this read-only scan never changes the store's reported status.
    const R1HealthHistoryStoreError e = r1HealthHistoryStoreLoadExact(
        m.ram.peer, v.dayStart, (int16_t)v.tz, m.day, /*reportStatus=*/false);
    if (e == R1_HISTORY_STORE_BUSY) return;  // same day again on the next pass
    if (e == R1_HISTORY_STORE_NOT_FOUND) continue;
    if (e == R1_HISTORY_STORE_OK) {
      const hi::DaySummary s = summarize(m.day);
      if (s.hasData()) m.scanDays[m.scanFound++] = s;
    } else {
      m.loadErrors++;  // a stored day that cannot be read counts as no data
    }
    break;
  }
  m.scanK++;
}

void refresh(uint32_t now) {
  MainState& m = *sMain;
  m.lastRefreshMs = now;
  G2HealthHistorySummary sum{};
  g2HealthHistoryGetSummary(sum);
  // The RAM day is the ring's latest history day this boot; without it the
  // ring's identity and day key are unknown and nothing is computed.
  if (!sum.available) return;
  if (!g2HealthHistorySnapshot(m.day) || m.day.dayStart != sum.dayStart || !m.day.peerId[0])
    return;
  const uint32_t now32 = (uint32_t)Clock::epochSeconds();
  uint32_t today = m.day.dayStart;
  if (Clock::isSynced() && now32 >= m.day.dayStart)
    today = m.day.dayStart + ((now32 - m.day.dayStart) / kDaySec) * kDaySec;
  memcpy(m.ram.peer, m.day.peerId, sizeof m.ram.peer);
  m.ram.peer[sizeof m.ram.peer - 1] = '\0';
  m.ram.dayStart = m.day.dayStart;
  m.ram.tz = m.day.timezoneMinutes;
  m.todayStart = today;
  m.ramSum = summarize(m.day);
  m.haveRam = true;

  R1HealthHistoryStoreStatus store{};
  r1HealthHistoryStoreGetStatus(store);
  hi::ScanKey key;
  memcpy(key.peer, m.ram.peer, sizeof key.peer);
  key.todayStart = today;
  key.ramDayStart = m.ram.dayStart;
  key.ramTz = m.ram.tz;
  key.committedGeneration = store.committedGeneration;
  if (!m.pastValid || !hi::sameScanKey(key, m.pastKey)) {
    // Older days changed (a sweep was committed, the day rolled over, the RAM
    // day moved, another ring): search the store again, one calendar day per
    // pass.
    m.scanning = true;
    m.scanK = 0;
    m.scanFound = 0;
    m.scanKey = key;
    return;
  }
  publish();
}

// ------------------------------------------------ G2 control task

void countFailure() {
  portENTER_CRITICAL(&sStatusMux);
  feed::countFailed(sStatus.feed);
  portEXIT_CRITICAL(&sStatusMux);
}

void publishLink(bool ready, bool busy) {
  portENTER_CRITICAL(&sStatusMux);
  feed::countLink(sStatus.feed, sPolicy, ready, busy);
  portEXIT_CRITICAL(&sStatusMux);
}

bool readPublished(uint32_t& version, uint32_t* computedMs = nullptr) {
  Published* p = sPub.load(std::memory_order_acquire);
  if (!p) return false;
  bool have;
  portENTER_CRITICAL(&sPubMux);
  have = p->have;
  version = p->version;
  if (computedMs) *computedMs = p->computedMs;
  portEXIT_CRITICAL(&sPubMux);
  return have;
}

bool sendNow(uint32_t generation, uint32_t now, bool manual) {
  Published* p = sPub.load(std::memory_order_acquire);
  if (!sOwner)
    sOwner = static_cast<OwnerBufs*>(
        ps_calloc(1, sizeof(OwnerBufs), AllocPolicy::PreferPsram, "g2.insights.tx"));
  if (!p || !sOwner) {
    hw1_g2_feed::sendFailed(sPolicy, now);
    countFailure();
    return false;
  }
  uint32_t version = 0;
  size_t bodyLen = 0;
  uint8_t count = 0;
  portENTER_CRITICAL(&sPubMux);
  if (p->have) {
    bodyLen = p->bodyLen;
    memcpy(sOwner->body, p->body, bodyLen);
    version = p->version;
    count = p->lines.count;
  }
  portEXIT_CRITICAL(&sPubMux);
  const uint8_t magic = g2HealthSid0eNextMagic();
  const size_t n = bodyLen ? hi::encodeMessage(sOwner->body, bodyLen, magic, sOwner->msg,
                                               sizeof sOwner->msg)
                           : 0;
  sPolicy.pending = version;
  const bool ok = n && hi::validateMessagePb(sOwner->msg, n) &&
                  g2HealthInsightsWireSend(generation, sOwner->msg, n);
  if (ok) hw1_g2_feed::sent(sPolicy, now, magic);
  else hw1_g2_feed::sendFailed(sPolicy, now);
  DEBUG_G2F("[G2-INSIGHTS] %s v%u magic=%u lines=%u len=%u %s", manual ? "manual" : "feed",
            (unsigned)version, (unsigned)magic, (unsigned)count, (unsigned)n,
            ok ? "sent" : "NOT sent");
  portENTER_CRITICAL(&sStatusMux);
  if (ok) {
    feed::countSent(sStatus.feed, now, magic);
    sStatus.lastSentVersion = version;
    sStatus.lastSentCount = count;
    sStatus.lastSentLen = (uint16_t)n;
    sStatus.lastWasManual = manual;
  } else {
    feed::countFailed(sStatus.feed);
  }
  portEXIT_CRITICAL(&sStatusMux);
  return ok;
}

}  // namespace

// ---------------------------------------------------------------- main loop

void g2HealthInsightsMainTick(bool linkReady) {
  const bool requested = sComputeRequest.exchange(false, std::memory_order_acq_rel);
  if (!sMain && !((feedOn() && linkReady) || requested)) return;
  if (!ensureMain()) return;
  MainState& m = *sMain;
  if (m.scanning) {
    scanStep();
    return;
  }
  const uint32_t now = millis();
  if (requested) {
    refresh(now);
    return;
  }
  if (!feedOn() || !linkReady) return;
  if (m.lastRefreshMs && (uint32_t)(now - m.lastRefreshMs) < kRefreshMs) return;
  refresh(now);
}

// ---------------------------------------------------------------- G2 control task

bool g2HealthInsightsInFlight(uint32_t generation) {
  return sPolicy.awaitingAck && generation != 0 && sPolicy.generation == generation;
}

void g2HealthInsightsOwnerTick(feed::Gate gate, uint32_t generation, uint32_t nowMs,
                               bool snapshotInFlight) {
  switch (gate) {
    case feed::Gate::Down:
      // A one-shot request never outlives the link it was made for.
      if (sForce.exchange(false, std::memory_order_acq_rel)) countFailure();
      feed::linkDown(sPolicy);
      publishLink(false, false);
      return;
    case feed::Gate::Paused:
      publishLink(true, true);
      return;
    case feed::Gate::Open:
      break;
  }
  uint32_t version = 0;
  const bool have = readPublished(version);
  // Hold the automatic send while a MULT_DATA snapshot on this link waits for
  // its echo, unless an insights message is itself in flight (its timeout and
  // retry must still run).
  const bool hold = snapshotInFlight && !g2HealthInsightsInFlight(generation);
  const feed::Action action =
      hi::policyTick(sPolicy, feedOn() && !hold, have, true, generation, nowMs, version);
  const bool force = sForce.exchange(false, std::memory_order_acq_rel);
  if (force && feed::tooSoon(sPolicy, nowMs, kManualFloorMs)) {
    countFailure();  // manual sends are bounded too
  } else if (force && !have) {
    countFailure();
  } else if (force || action == feed::Action::Send) {
    (void)sendNow(generation, nowMs, force);
  }
  publishLink(true, false);
}

void g2HealthInsightsOnRx(uint32_t rightGeneration, const uint8_t* pb, size_t len) {
  hi::Ack a;
  if (rightGeneration != sPolicy.generation || !hi::parseAck(pb, len, a)) return;
  const bool matched = hi::policyAck(sPolicy, rightGeneration, a);
  DEBUG_G2F("[G2-INSIGHTS] echo magic=%u items=%u status=%u %s", (unsigned)a.magic,
            (unsigned)a.itemCount, (unsigned)a.status, matched ? "acked" : "unmatched");
  if (!matched) return;
  portENTER_CRITICAL(&sStatusMux);
  feed::countAck(sStatus.feed, millis());
  sStatus.lastAckItems = a.itemCount;
  sStatus.lastAckStatus = a.status;
  sStatus.lastAckHaveStatus = a.haveStatus;
  portEXIT_CRITICAL(&sStatusMux);
}

// ---------------------------------------------------------------- commands

namespace {

// One line for the console: newlines shown as " / ".
void lineForConsole(char* out, size_t cap, const hi::Line& l) {
  size_t o = 0;
  for (size_t i = 0; i < l.len && o + 4 < cap; i++) {
    if (l.text[i] == '\n') {
      out[o++] = ' ';
      out[o++] = '/';
      out[o++] = ' ';
    } else {
      out[o++] = l.text[i];
    }
  }
  out[o] = '\0';
}

const char* typeName(uint8_t t) {
  switch (t) {
    case hi::kSteps: return "2 steps";
    case hi::kCalories: return "3 calories";
    case hi::kSleep: return "4 sleep";
    case hi::kHeartRate: return "5 heart rate";
    case hi::kSpo2: return "6 SpO2";
    case hi::kTemperature: return "7 temperature";
    case hi::kHrv: return "8 HRV";
    default: return "?";
  }
}

// Appends the published lines to buf at *off; returns false if nothing has
// been computed yet.
bool appendLines(char* buf, size_t cap, int& off, Published& snap, bool& have) {
  Published* p = sPub.load(std::memory_order_acquire);
  have = false;
  if (p) {
    portENTER_CRITICAL(&sPubMux);
    snap = *p;
    portEXIT_CRITICAL(&sPubMux);
    have = snap.have;
  }
  if (!have) return false;
  if (snap.lines.count == 0) {
    off += snprintf(buf + off, cap - off,
                    "  (no ring history in the window: the page shows \"gathering insights\")\n");
    return true;
  }
  EXT_RAM_BSS_ATTR static char line[hi::kMaxTextBytes * 3 + 4];
  for (size_t i = 0; i < snap.lines.count && off > 0 && (size_t)off < cap; i++) {
    lineForConsole(line, sizeof line, snap.lines.lines[i]);
    off += snprintf(buf + off, cap - off, "  %u [icon %s] %s\n", (unsigned)(i + 1),
                    typeName(snap.lines.lines[i].type), line);
  }
  return true;
}

const char* kNotComputed =
    "  (not computed yet: it needs the ring's history day from this boot; asked for a "
    "recompute, run again in a few seconds)\n";

}  // namespace

const char* cmd_ringinsightsstatus(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  Status s;
  portENTER_CRITICAL(&sStatusMux);
  s = sStatus;
  portEXIT_CRITICAL(&sStatusMux);
  EXT_RAM_BSS_ATTR static char buf[2048];
  EXT_RAM_BSS_ATTR static Published snap;
  const uint32_t now = millis();
  const feed::Counters& f = s.feed;
  char sentAgo[24], ackAgo[24], ack[64];
  if (f.sends) snprintf(sentAgo, sizeof sentAgo, "%u s ago", (unsigned)((now - f.lastSendMs) / 1000));
  else snprintf(sentAgo, sizeof sentAgo, "never");
  if (f.acks) snprintf(ackAgo, sizeof ackAgo, "%u s ago", (unsigned)((now - f.lastAckMs) / 1000));
  else snprintf(ackAgo, sizeof ackAgo, "never");
  if (f.acks && s.lastAckHaveStatus)
    snprintf(ack, sizeof ack, " (%u line(s) stored, status %u)", (unsigned)s.lastAckItems,
             (unsigned)s.lastAckStatus);
  else ack[0] = '\0';
  int off = snprintf(
      buf, sizeof buf,
      "Glasses Health insights feed: %s (saved)%s; window %u day(s) (saved)\n"
      "Right temple: %s%s\n",
      insightsSwitchOn() ? "ON" : "off",
      insightsSwitchOn() && !gSettings.g2HealthPanelFeed ? ", inactive: ring panel feed is off" : "",
      (unsigned)windowDays(), f.linkReady ? "ready" : "not ready",
      f.busy ? " (paused: another glasses session is active)" : "");
  bool have = false;
  off += snprintf(buf + off, sizeof buf - off, "Current lines:\n");
  if (!appendLines(buf, sizeof buf, off, snap, have)) {
    sComputeRequest.store(true, std::memory_order_release);
    off += snprintf(buf + off, sizeof buf - off, "%s", kNotComputed);
  } else if (off > 0 && (size_t)off < sizeof buf) {
    off += snprintf(buf + off, sizeof buf - off, "  over %u day(s), version %u, computed %u s ago\n",
                    (unsigned)snap.daysUsed, (unsigned)snap.version,
                    (unsigned)((now - snap.computedMs) / 1000));
  }
  if (off > 0 && (size_t)off < sizeof buf)
    snprintf(buf + off, sizeof buf - off,
             "Sends %u, acked %u, failed %u; last send %s (magic %u, version %u%s%s), last ack "
             "%s%s%s\nHistory scans %u, unreadable stored days %u",
             (unsigned)f.sends, (unsigned)f.acks, (unsigned)f.sendFailures, sentAgo,
             (unsigned)f.lastMagic, (unsigned)s.lastSentVersion,
             f.sends && have && s.lastSentVersion != snap.version ? ", older than current" : "",
             f.sends && s.lastWasManual ? ", by hand" : "", ackAgo, ack,
             f.awaitingAck ? "; waiting for the glasses' echo" : "", (unsigned)s.scans,
             (unsigned)s.loadErrors);
  return buf;
}

const char* cmd_ringinsightspreview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: ring insights preview";
  EXT_RAM_BSS_ATTR static char buf[640 + 2 * hi::kMaxPbBytes + 160];
  EXT_RAM_BSS_ATTR static Published snap;
  EXT_RAM_BSS_ATTR static uint8_t msg[hi::kMaxPbBytes];
  int off = snprintf(buf, sizeof buf, "Health insights (window %u day(s)):\n", (unsigned)windowDays());
  bool have = false;
  sComputeRequest.store(true, std::memory_order_release);  // fresh for the next look
  if (!appendLines(buf, sizeof buf, off, snap, have)) {
    snprintf(buf + off, sizeof buf - off, "%s", kNotComputed);
    return buf;
  }
  // Shown with magic 1; a real send uses the next sid 0x0E magic.
  const size_t n = hi::encodeMessage(snap.body, snap.bodyLen, 1, msg, sizeof msg);
  const bool valid = n && hi::validateMessagePb(msg, n);
  off += snprintf(buf + off, sizeof buf - off,
                  "sid 0x0E cmd 4, %u B, whitelist %s (magic 1 shown; not sent):\n", (unsigned)n,
                  valid ? "pass" : "FAIL");
  for (size_t i = 0; i < n && off > 0 && (size_t)off + 3 < sizeof buf; i++)
    off += snprintf(buf + off, sizeof buf - off, "%02x", msg[i]);
  return buf;
}

const char* cmd_ringinsightssend(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: ring insights send";
  if (!g2HealthRightLink(nullptr, nullptr))
    return "Error: Right temple is not ready, nothing sent. Run openg2, then retry.";
  uint32_t version = 0, computedMs = 0;
  // Always ask for fresh lines: with the feed off nothing else recomputes them.
  sComputeRequest.store(true, std::memory_order_release);
  if (!readPublished(version, &computedMs))
    return "Error: No insights computed yet (they need the ring's history day from this boot), "
           "nothing sent. Run ring history, wait a few seconds, then retry.";
  if ((uint32_t)(millis() - computedMs) > kManualFreshMs)
    return "Error: The insights lines are out of date, nothing sent. Asked for a recompute; run "
           "ring insights send again in a few seconds.";
  bool attempted;
  uint32_t lastAttemptMs;
  portENTER_CRITICAL(&sStatusMux);
  attempted = sStatus.feed.attemptedThisLink;
  lastAttemptMs = sStatus.feed.lastAttemptMs;
  portEXIT_CRITICAL(&sStatusMux);
  if (feed::tooSoon(attempted, lastAttemptMs, millis(), kManualFloorMs))
    return "Error: Insights were sent or tried less than a second ago. Wait a second and retry.";
  sForce.store(true, std::memory_order_release);
  return "Queued the current insights to the right temple (ring insights status shows the echo)";
}

const char* cmd_ringinsightsfeed(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  a.toLowerCase();
  if (!a.length())
    return insightsSwitchOn() ? "Glasses Health insights feed: on" : "Glasses Health insights feed: off";
  if (argIsOn(a)) {
    setSetting(gSettings.g2HealthInsightsFeed, true);
    sComputeRequest.store(true, std::memory_order_release);
    return gSettings.g2HealthPanelFeed
               ? "Glasses Health insights feed ON (saved): sends after each connect and when the "
                 "lines change (at most once a minute)"
               : "Glasses Health insights feed ON (saved); it runs once ring panel feed is on";
  }
  if (argIsOff(a)) {
    setSetting(gSettings.g2HealthInsightsFeed, false);
    return "Glasses Health insights feed off (saved). The glasses keep the last lines until they "
           "reboot or the Even app sends its own";
  }
  return "Error: invalid arguments — Usage: ring insights feed [on|off]";
}

const char* cmd_ringinsightsdays(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[96];
  String a = args;
  a.trim();
  if (!a.length()) {
    snprintf(buf, sizeof buf, "Health insights window: %u day(s)", (unsigned)windowDays());
    return buf;
  }
  uint8_t d = 0;
  if (!hi::parseDaysArg(a.c_str(), d))
    return "Error: invalid arguments — Usage: ring insights days [1-7]";
  setSetting(gSettings.g2HealthInsightsDays, (int)d);
  sComputeRequest.store(true, std::memory_order_release);
  snprintf(buf, sizeof buf, "Health insights window: %u day(s) (saved)", (unsigned)d);
  return buf;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH
