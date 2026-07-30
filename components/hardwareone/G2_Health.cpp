#include "G2_Health.h"

#if ENABLE_R1_HEALTH

#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "G2_Ring.h"
#include "System_Debug.h"
#include "System_R1_Protocol.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "System_Clock.h"   // isValidEpoch — honest day labels on Trends

#include <time.h>

extern uint32_t gBootCounter;  // session id — bumps each boot (HardwareOne.cpp)

// =============================================================================
// Health history + graph renderer. Portable aside from reading live telemetry
// via g2RingGetTelemetry() for the Overview tile and syncing samples in Tick.
// =============================================================================

static constexpr size_t kThinHistory = 8;   // below this → request daily backfill

struct HealthSample {
  uint32_t ms;       // millis() at capture (or synthesised for backfill)
  int16_t  value;
  uint32_t ringTs;   // ring epoch ts when known; 0 if unknown
};

struct HealthSeries {
  HealthSample buf[HEALTH_HIST_CAP];
  size_t       head;   // next write index
  size_t       count;
  uint32_t     lastRingTs;
  int16_t      lastValue;
  uint32_t     lastMs;
};

struct TrendMeta {
  uint32_t startTs;
  uint32_t endTs;
  uint8_t  count;
  bool     have;
  int16_t  avg;
  int16_t  minV;
  int16_t  maxV;
};

static HealthSeries sHr, sHrv, sSpo2, sTemp, sBat;
static HealthSeries sTrendHr, sTrendHrv, sTrendSpo2;
static TrendMeta sTrendMetaHr, sTrendMetaHrv, sTrendMetaSpo2;

static G2HealthNav sNav = HEALTH_NAV_MAIN;
static uint32_t sMenuGen = 1;
static G2HealthMetric sSelected = HEALTH_METRIC_OVERVIEW;
static bool sPollRequest = false;
static bool sDailyRequest = false;
static uint8_t sDailyCmd = 0;
// Trends Refresh (and enter) queues up to 3 daily cmds; drained one-at-a-time.
static uint8_t sDailyQueue[4];
static uint8_t sDailyQueueLen = 0;
static uint8_t sDailyQueueIdx = 0;
static uint32_t sLastSyncMs = 0;
static bool sLoggedHint = false;
static bool sPageActive = false;
// Lens feedback for Poll Now / entry burst (values often unchanged → text
// strcmp would otherwise skip UPDATE_TEXT and look like a dead tap).
enum : uint8_t { POLL_UI_IDLE = 0, POLL_UI_ACTIVE, POLL_UI_DONE };
static uint8_t sPollUi = POLL_UI_IDLE;
static uint32_t sPollUiMs = 0;
// One-shot metric graph: armed on metric select / Poll Now; consumed by
// the glasses worker once. Live 1 Hz sync must NOT re-push (~20 KB BLE).
static bool sGraphPushPending = false;
static uint32_t sGraphPushAfterMs = 0;

// Pixel grid in PSRAM BSS (~41 KB).
EXT_RAM_BSS_ATTR static uint8_t sGrid[HEALTH_TILE_H][HEALTH_TILE_W];

static HealthSeries* seriesFor(G2HealthMetric m) {
  switch (m) {
    case HEALTH_METRIC_HR:      return &sHr;
    case HEALTH_METRIC_HRV:     return &sHrv;
    case HEALTH_METRIC_SPO2:    return &sSpo2;
    case HEALTH_METRIC_TEMP:    return &sTemp;
    case HEALTH_METRIC_BATTERY: return &sBat;
    default: return nullptr;
  }
}

static HealthSeries* trendSeriesFor(G2HealthMetric m) {
  switch (m) {
    case HEALTH_METRIC_HR:   return &sTrendHr;
    case HEALTH_METRIC_HRV:  return &sTrendHrv;
    case HEALTH_METRIC_SPO2: return &sTrendSpo2;
    default: return nullptr;
  }
}

static TrendMeta* trendMetaFor(G2HealthMetric m) {
  switch (m) {
    case HEALTH_METRIC_HR:   return &sTrendMetaHr;
    case HEALTH_METRIC_HRV:  return &sTrendMetaHrv;
    case HEALTH_METRIC_SPO2: return &sTrendMetaSpo2;
    default: return nullptr;
  }
}

static void seriesClear(HealthSeries* s) {
  if (!s) return;
  s->head = 0;
  s->count = 0;
  s->lastRingTs = 0;
  s->lastValue = 0;
  s->lastMs = 0;
}

static void seriesPush(HealthSeries* s, int16_t value, uint32_t ringTs, uint32_t ms) {
  if (!s) return;
  // Dedupe: same ring timestamp, or same value within 5 s when ts unknown.
  if (ringTs != 0 && s->lastRingTs == ringTs) return;
  if (ringTs == 0 && s->count > 0 && s->lastValue == value &&
      (long)(ms - s->lastMs) < 5000) return;

  s->buf[s->head].ms = ms;
  s->buf[s->head].value = value;
  s->buf[s->head].ringTs = ringTs;
  s->head = (s->head + 1) % HEALTH_HIST_CAP;
  if (s->count < HEALTH_HIST_CAP) s->count++;
  s->lastRingTs = ringTs;
  s->lastValue = value;
  s->lastMs = ms;
}

static void seriesPushRaw(HealthSeries* s, int16_t value, uint32_t ringTs, uint32_t ms) {
  // No dedupe — used when replacing a Trends day series from a full payload.
  if (!s) return;
  s->buf[s->head].ms = ms;
  s->buf[s->head].value = value;
  s->buf[s->head].ringTs = ringTs;
  s->head = (s->head + 1) % HEALTH_HIST_CAP;
  if (s->count < HEALTH_HIST_CAP) s->count++;
  s->lastRingTs = ringTs;
  s->lastValue = value;
  s->lastMs = ms;
}

static void bumpMenuGen(void) { sMenuGen++; }

static void queueDailyCmd(uint8_t cmd) {
  for (uint8_t i = sDailyQueueIdx; i < sDailyQueueLen; i++) {
    if (sDailyQueue[i] == cmd) return;
  }
  if (sDailyQueueLen >= sizeof(sDailyQueue)) return;
  sDailyQueue[sDailyQueueLen++] = cmd;
}

static void queueTrendsRefreshAll(void) {
  sDailyQueueLen = 0;
  sDailyQueueIdx = 0;
  queueDailyCmd(R1_CMD_HEARTRATE);
  queueDailyCmd(R1_CMD_HRV);
  queueDailyCmd(R1_CMD_SPO2);
}

static void enterTrendsNav(void) {
  sNav = HEALTH_NAV_TRENDS;
  sSelected = HEALTH_METRIC_OVERVIEW;
  bumpMenuGen();
  g2HealthClearGraphPush();
  if (g2RingIsConnected()) queueTrendsRefreshAll();
}

static void leaveTrendsNav(void) {
  sNav = HEALTH_NAV_MAIN;
  sSelected = HEALTH_METRIC_OVERVIEW;
  bumpMenuGen();
  g2HealthClearGraphPush();
  sDailyQueueLen = 0;
  sDailyQueueIdx = 0;
}

void g2HealthNoteSample(G2HealthMetric metric, int16_t value, uint32_t ringTs) {
  seriesPush(seriesFor(metric), value, ringTs, millis());
}

// Synthesised .ms for a daily payload. The old flat 60 s ladder asserted a
// span of (count-1) minutes for a payload that nominally covers a day, so a
// 24 h backfill occupied ~52% of the plot instead of ~99%. Use the payload's
// own window when it is sane and fall back to the nominal step otherwise.
// No clamp against nowMs: .ms has exactly two consumers — seriesPush's
// (long)(ms - lastMs) signed delta and drawSparkline's unsigned modular
// delta — and both are correct across an underflow at early uptime. Clamping
// would make the plotted scale a function of uptime.
static constexpr uint32_t kBackfillNominalStepMs = 60000u;      // window unusable
static constexpr uint32_t kBackfillMaxSpanSec    = 48u * 3600u; // reject garbage windows

static uint32_t backfillSampleMs(size_t i, size_t count, uint32_t nowMs,
                                 uint32_t startTs, uint32_t endTs) {
  if (count < 2) return nowMs;
  uint32_t spanMs = (uint32_t)((count - 1) * kBackfillNominalStepMs);
  if (endTs > startTs && (endTs - startTs) <= kBackfillMaxSpanSec) {
    spanMs = (endTs - startTs) * 1000u;
  }
  return nowMs - (uint32_t)((uint64_t)spanMs * (count - 1 - i) / (count - 1));
}

void g2HealthApplyDailyBackfill(G2HealthMetric metric,
                                const uint8_t* values, size_t count,
                                uint32_t startTs, uint32_t endTs) {
  HealthSeries* s = seriesFor(metric);
  if (!s || !values || count == 0) return;
  // Only backfill when live series is thin — don't clobber a rich live trail.
  if (s->count >= kThinHistory) return;

  // Temp daily record[0] is hypothesized whole °C; series stores °C × 10.
  const int16_t scale = (metric == HEALTH_METRIC_TEMP) ? 10 : 1;

  const uint32_t nowMs = millis();
  const uint32_t span = (endTs > startTs && count > 1) ? (endTs - startTs) : 0;
  for (size_t i = 0; i < count && i < HEALTH_HIST_CAP; i++) {
    uint32_t rts = startTs;
    if (span > 0) rts = startTs + (uint32_t)((uint64_t)span * i / (count - 1));
    // .ms carries the payload's real duration; drawSparkline sorts by it, so
    // these land before the live trail regardless of insertion order.
    uint32_t ms = backfillSampleMs(i, count, nowMs, startTs, endTs);
    seriesPush(s, (int16_t)((int16_t)values[i] * scale), rts, ms);
  }
  DEBUG_G2F("[HEALTH] daily backfill metric=%u count=%u",
            (unsigned)metric, (unsigned)count);
}

void g2HealthApplyTrendDaily(G2HealthMetric metric,
                             const uint8_t* values, size_t count,
                             uint32_t startTs, uint32_t endTs) {
  HealthSeries* s = trendSeriesFor(metric);
  TrendMeta* meta = trendMetaFor(metric);
  if (!s || !meta || !values || count == 0) return;

  seriesClear(s);
  const uint32_t nowMs = millis();
  const uint32_t span = (endTs > startTs && count > 1) ? (endTs - startTs) : 0;
  int32_t sum = 0;
  int16_t minV = 0, maxV = 0;
  size_t n = 0;
  for (size_t i = 0; i < count && i < HEALTH_HIST_CAP; i++) {
    const int16_t v = (int16_t)values[i];
    uint32_t rts = startTs;
    if (span > 0) rts = startTs + (uint32_t)((uint64_t)span * i / (count - 1));
    uint32_t ms = backfillSampleMs(i, count, nowMs, startTs, endTs);
    seriesPushRaw(s, v, rts, ms);
    if (n == 0) { minV = maxV = v; }
    else {
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
    }
    sum += v;
    n++;
  }
  meta->startTs = startTs;
  meta->endTs = endTs;
  meta->count = (uint8_t)n;
  meta->have = (n > 0);
  meta->avg = n ? (int16_t)(sum / (int32_t)n) : 0;
  meta->minV = minV;
  meta->maxV = maxV;
  // Refresh graph if this is the Trends metric currently on screen.
  if (sNav == HEALTH_NAV_TRENDS && sSelected == metric) {
    g2HealthArmGraphPush(200);
  }
  DEBUG_G2F("[HEALTH] trend daily metric=%u count=%u avg=%d",
            (unsigned)metric, (unsigned)n, (int)meta->avg);
}

size_t g2HealthHistoryCount(G2HealthMetric metric) {
  HealthSeries* s = seriesFor(metric);
  return s ? s->count : 0;
}

// Live-cache resample. The cache has no TTL, so a re-read of an unchanged
// reading must not append: passing millis()-now refreshed seriesPush's lastMs
// on every accepted push, so an unchanging value cleared the 5 s gate again
// every 5 s — one fabricated sample per metric per 5 s, forever. That refilled
// all 96 slots in eight minutes and evicted a whole day of real history.
// Stamping each sample with its own RECEIVE time makes the value+time gate do
// the right thing: an unchanged cache keeps producing the same stamp and is
// dropped for good, while a genuinely new reading lands.
static void syncPush(HealthSeries* s, int16_t value, uint32_t rxMs) {
  if (rxMs == 0) return;   // no receive time known — don't invent one
  seriesPush(s, value, 0, rxMs);
}

static void syncFromTelemetry(void) {
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  if (!t.connected) return;
  // Ring timestamps live in the private cache; pass 0 and let value+time gate
  // dedupe — the notify path also calls g2HealthNoteSample with real ringTs.
  // *RxMs (not *AgeSec) is mandatory here: ringSampleAgeSec prefers the ring
  // epoch, which custody steps both directions, and a ring running ahead pins
  // its age at 0 — which would resurrect the phantom appends above.
  if (t.hrValid)      syncPush(&sHr,   (int16_t)t.hr,      t.hrRxMs);
  if (t.hrvValid)     syncPush(&sHrv,  t.hrv,              t.hrvRxMs);
  if (t.spo2Valid)    syncPush(&sSpo2, (int16_t)t.spo2,    t.spo2RxMs);
  if (t.tempValid)    syncPush(&sTemp, t.tempTenths,       t.tempRxMs);
  if (t.batteryValid) syncPush(&sBat,  (int16_t)t.battery, t.batteryRxMs);
}

const char* const* g2HealthMenuRows(size_t* count) {
  // MAIN vs TRENDS row sets. Changing nav bumps menu generation so the
  // worker SHUTDOWN+CREATEs with the new labels (can't hot-swap list rows).
  static const char* const mainRows[] = {
    "<- Apps",
    "Overview",
    "Trends",
    "Heart Rate",
    "HRV",
    "SpO2",
    "Temperature",
    "Battery",
    "Poll Now",
    "Toggle Track",
  };
  // Rows carry no day claim — the ring's daily window is only "today" when
  // its clock is real; the text panel shows the honest label (date / boot N).
  static const char* const trendRows[] = {
    "<- Health",
    "HR",
    "HRV",
    "SpO2",
    "Refresh",
  };
  if (sNav == HEALTH_NAV_TRENDS) {
    if (count) *count = sizeof(trendRows) / sizeof(trendRows[0]);
    return trendRows;
  }
  if (count) *count = sizeof(mainRows) / sizeof(mainRows[0]);
  return mainRows;
}

uint32_t g2HealthMenuGeneration(void) { return sMenuGen; }
G2HealthNav g2HealthNav(void) { return sNav; }
bool g2HealthInTrendsNav(void) { return sNav == HEALTH_NAV_TRENDS; }

bool g2HealthWantTextOnlyShape(void) {
  return sSelected == HEALTH_METRIC_OVERVIEW;
}

bool g2HealthTrackIsActive(void) {
  return healthTrackIsActive();
}

void g2HealthOpen(void) {
  sNav = HEALTH_NAV_MAIN;
  sSelected = HEALTH_METRIC_OVERVIEW;
  bumpMenuGen();
  sPollRequest = true;
  sDailyRequest = false;
  sDailyCmd = 0;
  sDailyQueueLen = 0;
  sDailyQueueIdx = 0;
  sLastSyncMs = 0;
  g2HealthClearGraphPush();
  syncFromTelemetry();
  if (!sLoggedHint && !healthTrackIsActive()) {
    DEBUG_G2F("[HEALTH] tip: tap Toggle Track (or 'healthtrack on') to persist vitals");
    sLoggedHint = true;
  }
}

void g2HealthTick(void) {
  const uint32_t now = millis();
  if (sLastSyncMs == 0 || (long)(now - sLastSyncMs) >= 1000) {
    syncFromTelemetry();
    sLastSyncMs = now;
  }
}

static void maybeRequestDaily(G2HealthMetric m) {
  uint8_t cmd = 0;
  HealthSeries* s = nullptr;
  switch (m) {
    case HEALTH_METRIC_HR:   cmd = R1_CMD_HEARTRATE;   s = &sHr;   break;
    case HEALTH_METRIC_HRV:  cmd = R1_CMD_HRV;         s = &sHrv;  break;
    case HEALTH_METRIC_SPO2: cmd = R1_CMD_SPO2;        s = &sSpo2; break;
    case HEALTH_METRIC_TEMP: cmd = R1_CMD_TEMPERATURE; s = &sTemp; break;
    default: return;
  }
  if (!s || s->count >= kThinHistory) return;
  if (!g2RingIsConnected()) return;
  sDailyCmd = cmd;
  sDailyRequest = true;
}

static void requestTrendDaily(G2HealthMetric m) {
  uint8_t cmd = 0;
  switch (m) {
    case HEALTH_METRIC_HR:   cmd = R1_CMD_HEARTRATE; break;
    case HEALTH_METRIC_HRV:  cmd = R1_CMD_HRV;       break;
    case HEALTH_METRIC_SPO2: cmd = R1_CMD_SPO2;      break;
    default: return;
  }
  if (!g2RingIsConnected()) return;
  queueDailyCmd(cmd);
}

void g2HealthArmGraphPush(uint32_t delayMs) {
  const uint32_t t = millis() + delayMs;
  if (!sGraphPushPending) {
    sGraphPushPending = true;
    sGraphPushAfterMs = t;
    return;
  }
  // Already armed — keep the later deadline (metric enter waits for daily;
  // a later Poll arm shouldn't pull the first push forward).
  if ((long)(t - sGraphPushAfterMs) > 0) sGraphPushAfterMs = t;
}

bool g2HealthConsumeGraphPush(void) {
  if (!sGraphPushPending) return false;
  if ((long)(millis() - sGraphPushAfterMs) < 0) return false;
  sGraphPushPending = false;
  return true;
}

void g2HealthClearGraphPush(void) {
  sGraphPushPending = false;
  sGraphPushAfterMs = 0;
}

void g2HealthAction(uint32_t rowIdx) {
  if (sNav == HEALTH_NAV_TRENDS) {
    // 0 <- Health · 1 HR · 2 HRV · 3 SpO2 · 4 Refresh
    switch (rowIdx) {
      case 0:
        leaveTrendsNav();
        DEBUG_G2F("[HEALTH] Trends → main");
        break;
      case 1:
        sSelected = HEALTH_METRIC_HR;
        requestTrendDaily(sSelected);
        g2HealthArmGraphPush(1200);
        break;
      case 2:
        sSelected = HEALTH_METRIC_HRV;
        requestTrendDaily(sSelected);
        g2HealthArmGraphPush(1200);
        break;
      case 3:
        sSelected = HEALTH_METRIC_SPO2;
        requestTrendDaily(sSelected);
        g2HealthArmGraphPush(1200);
        break;
      case 4:
        sSelected = HEALTH_METRIC_OVERVIEW;
        g2HealthClearGraphPush();
        queueTrendsRefreshAll();
        DEBUG_G2F("[HEALTH] Trends Refresh queued");
        break;
      default: break;
    }
    return;
  }

  // MAIN: 0 Back (worker) · 1 Overview · 2 Trends · 3 HR · 4 HRV · 5 SpO2 ·
  // 6 Temp · 7 Bat · 8 Poll · 9 Toggle Track
  switch (rowIdx) {
    case 1:
      sSelected = HEALTH_METRIC_OVERVIEW;
      g2HealthClearGraphPush();
      break;
    case 2:
      enterTrendsNav();
      DEBUG_G2F("[HEALTH] → Trends");
      break;
    case 3:
      sSelected = HEALTH_METRIC_HR;
      maybeRequestDaily(sSelected);
      g2HealthArmGraphPush(1200);
      break;
    case 4:
      sSelected = HEALTH_METRIC_HRV;
      maybeRequestDaily(sSelected);
      g2HealthArmGraphPush(1200);
      break;
    case 5:
      sSelected = HEALTH_METRIC_SPO2;
      maybeRequestDaily(sSelected);
      g2HealthArmGraphPush(1200);
      break;
    case 6:
      sSelected = HEALTH_METRIC_TEMP;
      maybeRequestDaily(sSelected);
      g2HealthArmGraphPush(1200);
      break;
    case 7:
      sSelected = HEALTH_METRIC_BATTERY;
      g2HealthArmGraphPush(400);  // no daily for battery
      break;
    case 8:
      sPollRequest = true;
      DEBUG_G2F("[HEALTH] Poll Now requested");
      break;
    case 9: {
      const char* r = healthTrackSet(!gSettings.healthTrackingEnabled);
      DEBUG_G2F("[HEALTH] Toggle Track → %s", r ? r : "(null)");
      break;
    }
    default: break;
  }
}

G2HealthMetric g2HealthSelectedMetric(void) { return sSelected; }

bool g2HealthConsumePollRequest(void) {
  if (!sPollRequest) return false;
  sPollRequest = false;
  return true;
}

void g2HealthSetPageActive(bool active) {
  sPageActive = active;
  if (!active) {
    sPollUi = POLL_UI_IDLE;
    sPollUiMs = 0;
  }
}

bool g2HealthPageIsActive(void) { return sPageActive; }

void g2HealthNotePollStarted(void) {
  sPollUi = POLL_UI_ACTIVE;
  sPollUiMs = millis();
}

void g2HealthNotePollFinished(void) {
  sPollUi = POLL_UI_DONE;
  // Refresh the sparkline once after a poll burst — not on every live sample.
  if (sNav == HEALTH_NAV_MAIN && sSelected != HEALTH_METRIC_OVERVIEW) {
    g2HealthArmGraphPush(300);
  }
  sPollUiMs = millis();
}

// Status line under vitals / metric readout. Ages "Updated" back to Track.
static const char* healthPollStatusLine(const char* fallback) {
  const uint32_t now = millis();
  if (sPollUi == POLL_UI_ACTIVE) return "Polling...";
  if (sPollUi == POLL_UI_DONE) {
    if ((long)(now - sPollUiMs) < 4000) return "Updated";
    sPollUi = POLL_UI_IDLE;
  }
  return fallback ? fallback : "";
}

bool g2HealthConsumeDailyRequest(uint8_t* outCmd) {
  // Trends Refresh queue first (paced by the worker between consumes).
  if (sDailyQueueIdx < sDailyQueueLen) {
    if (outCmd) *outCmd = sDailyQueue[sDailyQueueIdx++];
    if (sDailyQueueIdx >= sDailyQueueLen) {
      sDailyQueueLen = 0;
      sDailyQueueIdx = 0;
    }
    return true;
  }
  if (!sDailyRequest) return false;
  sDailyRequest = false;
  if (outCmd) *outCmd = sDailyCmd;
  return true;
}

// ── Tiny 3×5 font (digits + letters used on the tile) ────────────────────────
static const char* healthGlyph(char c) {
  switch (c) {
    case '0': return "###" "#.#" "#.#" "#.#" "###";
    case '1': return ".#." "##." ".#." ".#." "###";
    case '2': return "###" "..#" "###" "#.." "###";
    case '3': return "###" "..#" "###" "..#" "###";
    case '4': return "#.#" "#.#" "###" "..#" "..#";
    case '5': return "###" "#.." "###" "..#" "###";
    case '6': return "###" "#.." "###" "#.#" "###";
    case '7': return "###" "..#" "..#" "..#" "..#";
    case '8': return "###" "#.#" "###" "#.#" "###";
    case '9': return "###" "#.#" "###" "..#" "###";
    case 'A': return ".#." "#.#" "###" "#.#" "#.#";
    case 'B': return "##." "#.#" "##." "#.#" "##.";
    case 'D': return "##." "#.#" "#.#" "#.#" "##.";
    case 'F': return "###" "#.." "##." "#.." "#..";
    case 'G': return "###" "#.." "#.#" "#.#" "###";
    case 'H': return "#.#" "#.#" "###" "#.#" "#.#";
    case 'I': return "###" ".#." ".#." ".#." "###";
    case 'L': return "#.." "#.." "#.." "#.." "###";
    case 'N': return "#.#" "##." "###" "#.#" "#.#";
    case 'O': return "###" "#.#" "#.#" "#.#" "###";
    case 'P': return "###" "#.#" "###" "#.." "#..";
    case 'R': return "##." "#.#" "##." "#.#" "#.#";
    case 'S': return "###" "#.." "###" "..#" "###";
    case 'T': return "###" ".#." ".#." ".#." ".#.";
    case 'U': return "#.#" "#.#" "#.#" "#.#" "###";
    case 'V': return "#.#" "#.#" "#.#" "#.#" ".#.";
    case 'W': return "#.#" "#.#" "###" "###" "#.#";
    case 'Y': return "#.#" "#.#" ".#." ".#." ".#.";
    case 'a': return "..." "###" "#.#" "#.#" "###";
    case 'b': return "#.." "#.." "###" "#.#" "###";
    case 'c': return "..." "###" "#.." "#.." "###";
    case 'd': return "..#" "..#" "###" "#.#" "###";
    case 'e': return "..." "###" "###" "#.." "###";
    case 'f': return ".##" ".#." "###" ".#." ".#.";
    case 'g': return "..." "###" "#.#" "###" "..#";
    case 'h': return "#.." "#.." "###" "#.#" "#.#";
    case 'i': return ".#." "..." ".#." ".#." ".#.";
    case 'k': return "#.." "#.#" "##." "#.#" "#.#";
    case 'l': return "##." ".#." ".#." ".#." "###";
    case 'm': return "..." "###" "###" "#.#" "#.#";
    case 'n': return "..." "##." "#.#" "#.#" "#.#";
    case 'o': return "..." "###" "#.#" "#.#" "###";
    case 'p': return "..." "##." "#.#" "##." "#..";
    case 'r': return "..." "##." "#.." "#.." "#..";
    case 's': return "..." "###" "#.." "###" "###";
    case 't': return ".#." "###" ".#." ".#." ".##";
    case 'u': return "..." "#.#" "#.#" "#.#" "###";
    case 'v': return "..." "#.#" "#.#" "#.#" ".#.";
    case 'w': return "..." "#.#" "#.#" "###" "#.#";
    case '%': return "#.#" "..#" ".#." "#.." "#.#";
    case '-': return "..." "..." "###" "..." "...";
    case ' ': return "..." "..." "..." "..." "...";
    case ':': return "..." ".#." "..." ".#." "...";
    case '.': return "..." "..." "..." "..." ".#.";
    case '/': return "..#" ".#." ".#." ".#." "#..";
    default:  return nullptr;
  }
}

static inline void px(int x, int y, int v) {
  if (x >= 0 && x < HEALTH_TILE_W && y >= 0 && y < HEALTH_TILE_H)
    sGrid[y][x] = (uint8_t)v;
}

static void clearGrid(uint8_t v) {
  memset(sGrid, v, sizeof(sGrid));
}

static void drawChar(char c, int x, int y, int shade) {
  const char* g = healthGlyph(c);
  if (!g) return;
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 3; col++) {
      if (g[row * 3 + col] == '#') px(x + col, y + row, shade);
    }
  }
}

static void drawText(const char* s, int x, int y, int shade) {
  if (!s) return;
  int cx = x;
  for (; *s; s++) {
    drawChar(*s, cx, y, shade);
    cx += 4;
  }
}

// Integer-scale blit of the 3×5 glyph (axis labels use scale=2 → 6×10).
static void drawCharScaled(char c, int x, int y, int shade, int scale) {
  const char* g = healthGlyph(c);
  if (!g || scale < 1) return;
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 3; col++) {
      if (g[row * 3 + col] != '#') continue;
      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
          px(x + col * scale + dx, y + row * scale + dy, shade);
        }
      }
    }
  }
}

static void drawTextScaled(const char* s, int x, int y, int shade, int scale) {
  if (!s || scale < 1) return;
  const int advance = 3 * scale + scale;  // glyph width + 1-cell gap
  int cx = x;
  for (; *s; s++) {
    drawCharScaled(*s, cx, y, shade, scale);
    cx += advance;
  }
}

static void drawHLine(int x0, int x1, int y, int shade) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) px(x, y, shade);
}

static void drawVLine(int x, int y0, int y1, int shade) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) px(x, y, shade);
}

// Oldest-first walk over the ring buffer, indexed in place. The old version
// copied 96 int16_t values into a caller stack array and discarded .ms, which
// is exactly what the renderer needs for a time axis; a 96-byte draw-order
// index costs less frame than the 192-byte value copy it replaces.
static size_t seriesOrderedStart(const HealthSeries* s, size_t* outCount) {
  const size_t n = s->count;
  if (outCount) *outCount = n;
  if (n == 0) return 0;
  return (s->head + HEALTH_HIST_CAP - n) % HEALTH_HIST_CAP;
}

static inline const HealthSample& seriesAt(const HealthSeries* s, size_t start, size_t i) {
  return s->buf[(start + i) % HEALTH_HIST_CAP];
}

// Unsigned modular age, so a millis() rollover inside the window needs no
// special case (96 slots can never span the 49.7-day modulus). A stamp in the
// future — benign race with the notify task writing buf[] — reads as "now"
// rather than as a ~49.7-day age that would stretch the domain to a month.
static constexpr uint32_t kAgePlausibleMs = 30u * 24u * 3600u * 1000u;

static inline uint32_t sampleAgeMs(const HealthSeries* s, size_t start, size_t i,
                                  uint32_t nowMs) {
  const uint32_t age = (uint32_t)(nowMs - seriesAt(s, start, i).ms);
  return (age > kAgePlausibleMs) ? 0u : age;
}

// Plot span snaps UP to a rung, so appending a sample doesn't re-space what is
// already drawn — the picture only rescales when the trail outgrows its rung.
// Roughly doubling on purpose: a finer ladder rescales more often (which is the
// motion this removes) and each step compresses the trace by at most 2x.
static const uint32_t kSpanRungsMs[] = {
     30u*1000u,    60u*1000u,   120u*1000u,   240u*1000u,   480u*1000u,
    900u*1000u,  1800u*1000u,  3600u*1000u,  7200u*1000u, 14400u*1000u,
  28800u*1000u, 43200u*1000u, 86400u*1000u,172800u*1000u,345600u*1000u,
 604800u*1000u,
};

static uint32_t seriesSpanWindowMs(uint32_t spanMs) {
  for (size_t i = 0; i < sizeof(kSpanRungsMs) / sizeof(kSpanRungsMs[0]); i++) {
    if (spanMs <= kSpanRungsMs[i]) return kSpanRungsMs[i];
  }
  return spanMs;  // past the ladder — exact fit rather than clipping
}

// Auto-scale Y from samples so small variations aren't flat against a
// huge fixed axis (e.g. HR 72→78 on a 40–180 scale).
static void seriesAutoY(const HealthSeries* s, size_t start, size_t n,
                        int16_t softMin, int16_t softMax,
                        int16_t minSpan, int16_t* outMin, int16_t* outMax) {
  int16_t lo = seriesAt(s, start, 0).value, hi = lo;
  for (size_t i = 1; i < n; i++) {
    const int16_t v = seriesAt(s, start, i).value;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  int16_t span = (int16_t)(hi - lo);
  if (span < minSpan) {
    int16_t mid = (int16_t)(lo + span / 2);
    lo = (int16_t)(mid - minSpan / 2);
    hi = (int16_t)(lo + minSpan);
  }
  // ~12% padding so the line doesn't hug the frame.
  int16_t pad = (int16_t)((hi - lo) / 8);
  if (pad < 1) pad = 1;
  lo = (int16_t)(lo - pad);
  hi = (int16_t)(hi + pad);
  if (lo < softMin) lo = softMin;
  if (hi > softMax) hi = softMax;
  if (hi <= lo) hi = (int16_t)(lo + minSpan);
  *outMin = lo;
  *outMax = hi;
}

static_assert(HEALTH_HIST_CAP <= 255, "drawSparkline draw-order index is uint8_t");

static void drawSparkline(const HealthSeries* s, int x, int y, int w, int h,
                          int16_t softMin, int16_t softMax, int16_t minSpan,
                          int shade, int16_t* usedMin, int16_t* usedMax,
                          uint32_t* outOldestAgeMs, uint32_t* outWindowMs) {
  // Out-params default to "no time domain" so every early return leaves the
  // caller's X axis suppressed rather than labelling stale values.
  if (outOldestAgeMs) *outOldestAgeMs = 0;
  if (outWindowMs)    *outWindowMs = 0;
  if (!s || s->count < 2 || w < 4 || h < 4) return;

  size_t n = 0;
  const size_t start = seriesOrderedStart(s, &n);
  if (n < 2) return;

  int16_t yMin = softMin, yMax = softMax;
  seriesAutoY(s, start, n, softMin, softMax, minSpan, &yMin, &yMax);
  if (usedMin) *usedMin = yMin;
  if (usedMax) *usedMax = yMax;
  if (yMax <= yMin) yMax = (int16_t)(yMin + 1);

  // Frame
  drawHLine(x, x + w - 1, y, 3);
  drawHLine(x, x + w - 1, y + h - 1, 3);
  drawVLine(x, y, y + h - 1, 3);
  drawVLine(x + w - 1, y, y + h - 1, 3);

  const uint32_t nowMs = millis();

  // Draw order is TIME order, not insertion order: daily backfill appends
  // records OLDER than the live samples already in the buffer, so an
  // index-ordered polyline jumped from the newest live point back to the start
  // of history. Sorting here makes the renderer correct for any insertion
  // order, which is a stronger guarantee than an ingest-side prepend.
  uint8_t order[HEALTH_HIST_CAP];
  for (size_t i = 0; i < n; i++) order[i] = (uint8_t)i;
  for (size_t i = 1; i < n; i++) {          // insertion sort, oldest first
    const uint8_t v = order[i];
    const uint32_t va = sampleAgeMs(s, start, v, nowMs);
    size_t j = i;
    while (j > 0 && sampleAgeMs(s, start, order[j - 1], nowMs) < va) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  // X is elapsed time from the oldest sample over a rung-snapped window, so
  // the mapping is independent of the render instant: an unchanged buffer
  // reproduces the picture exactly, and an append inside the current rung
  // moves nothing.
  const uint32_t ageMax = sampleAgeMs(s, start, order[0], nowMs);
  const uint32_t ageMin = sampleAgeMs(s, start, order[n - 1], nowMs);
  const uint32_t spanMs = ageMax - ageMin;
  // A zero span (degenerate daily payload, or one tick's worth of samples) has
  // no domain at all — space those evenly so the trail is still legible.
  const bool uniform      = (spanMs == 0);
  const uint32_t windowMs = uniform ? 1u : seriesSpanWindowMs(spanMs);  // 1 = unused
  // For the X axis: window 0 = uniform spacing, nothing truthful to label.
  if (outOldestAgeMs) *outOldestAgeMs = ageMax;
  if (outWindowMs)    *outWindowMs = uniform ? 0u : windowMs;
  const int plotW = w - 3;
  constexpr int kDotMinGapPx = 4;

  int prevX = -1, prevY = -1;
  for (size_t k = 0; k < n; k++) {
    const size_t i = order[k];
    int px_;
    if (uniform) {
      px_ = x + 1 + (int)((int64_t)plotW * (int)k / (int)(n - 1));
    } else {
      const uint32_t off = ageMax - sampleAgeMs(s, start, i, nowMs);
      px_ = x + 1 + (int)((uint64_t)(uint32_t)plotW * off / windowMs);
    }
    int clamped = seriesAt(s, start, i).value;
    if (clamped < yMin) clamped = yMin;
    if (clamped > yMax) clamped = yMax;
    const int py = y + h - 2 - (int)((int64_t)(h - 3) * (clamped - yMin) / (yMax - yMin));
    if (prevX >= 0) {
      if (px_ == prevX && py == prevY) continue;   // same pixel — nothing to add
      const int dx = px_ - prevX, dy = py - prevY;
      const int adx = dx < 0 ? -dx : dx;
      const int ady = dy < 0 ? -dy : dy;
      int steps = adx > ady ? adx : ady;
      if (steps < 1) steps = 1;
      // From step 1: step 0 is the previous sample's own pixel, and repainting
      // it at stroke shade was erasing that sample's bright dot.
      for (int sstep = 1; sstep <= steps; sstep++) {
        const int xx = prevX + dx * sstep / steps;
        const int yy = prevY + dy * sstep / steps;
        px(xx, yy, shade);
        px(xx, yy + 1, shade);
        // Thicken only runs that travel sideways; on a dense trail the segments
        // are near-vertical min/max bars and widening those bleeds into the
        // next sample's column.
        if (shade > 2 && adx > 1) px(xx + 1, yy, shade - 2);
      }
    }
    // Sample dots so sparse trails still read as a line graph; suppressed on a
    // dense trail where every column would otherwise be a dot.
    if (prevX < 0 || px_ - prevX >= kDotMinGapPx) {
      px(px_, py, 15);
      px(px_ + 1, py, 12);
      px(px_, py + 1, 12);
    }
    prevX = px_;
    prevY = py;
  }
}

static void fmtVal(char* out, size_t cap, bool valid, int v, const char* unit) {
  if (!valid) {
    snprintf(out, cap, "--%s", unit ? unit : "");
  } else {
    snprintf(out, cap, "%d%s", v, unit ? unit : "");
  }
}

// Compact sample age: "now" / "12s" / "3m" / "2h" / "1d". Empty when unknown.
// Under 60s, bucket to 5s so Overview UPDATE_TEXT isn't every second.
static void fmtAge(char* out, size_t cap, int32_t ageSec) {
  if (!out || !cap) return;
  out[0] = '\0';
  if (ageSec < 0) return;
  if (ageSec < 5) {
    snprintf(out, cap, "now");
  } else if (ageSec < 60) {
    const long bucket = ((long)ageSec / 5) * 5;
    snprintf(out, cap, "%lds", bucket);
  } else if (ageSec < 3600) {
    snprintf(out, cap, "%ldm", (long)(ageSec / 60));
  } else if (ageSec < 86400) {
    snprintf(out, cap, "%ldh", (long)(ageSec / 3600));
  } else {
    snprintf(out, cap, "%ldd", (long)(ageSec / 86400));
  }
}

// Freshest among valid vitals (min age). −1 if none.
static int32_t freshestVitalAge(const G2RingTelemetry& t) {
  int32_t best = -1;
  auto consider = [&](bool valid, int32_t age) {
    if (!valid || age < 0) return;
    if (best < 0 || age < best) best = age;
  };
  consider(t.hrValid, t.hrAgeSec);
  consider(t.hrvValid, t.hrvAgeSec);
  consider(t.spo2Valid, t.spo2AgeSec);
  consider(t.tempValid, t.tempAgeSec);
  consider(t.batteryValid, t.batteryAgeSec);
  return best;
}

static void fmtValAge(char* out, size_t cap, bool valid, int v,
                      const char* unit, int32_t ageSec) {
  char base[20], age[8];
  fmtVal(base, sizeof(base), valid, v, unit);
  fmtAge(age, sizeof(age), valid ? ageSec : -1);
  if (age[0]) snprintf(out, cap, "%s · %s", base, age);
  else        snprintf(out, cap, "%s", base);
}

static void fmtTempAge(char* out, size_t cap, bool valid, int16_t tenths,
                       int32_t ageSec) {
  char base[20], age[8];
  if (!valid) snprintf(base, sizeof(base), "-- C");
  else        snprintf(base, sizeof(base), "%d.%d C",
                       (int)(tenths / 10), (int)(tenths < 0 ? -(tenths % 10) : (tenths % 10)));
  fmtAge(age, sizeof(age), valid ? ageSec : -1);
  if (age[0]) snprintf(out, cap, "%s · %s", base, age);
  else        snprintf(out, cap, "%s", base);
}

static const char* wearLabel(const G2RingTelemetry& t) {
  if (!t.wearValid) return "Wear --";
  if (t.wear == 2) return "Wear on";
  if (t.wear == 1) return "Wear off";
  return "Wear ?";
}

void g2HealthBuildOverviewText(char* out, size_t cap) {
  if (!out || !cap) return;
  G2RingTelemetry t;
  g2RingGetTelemetry(t);

  if (!t.connected) {
    snprintf(out, cap,
             "Ring offline\n"
             "Pair via\n"
             "Network / BT / R1");
    return;
  }

  const char* track =
      healthTrackIsActive() ? "TRACK on" :
      (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) ? "LOG on" : "Track off";
  const char* poll = healthPollStatusLine(nullptr);

  // One shared recentness (freshest vital), not per-line ages — avoids
  // five independent second-ticks redrawing the whole Overview pane.
  char age[8];
  fmtAge(age, sizeof(age), freshestVitalAge(t));

  char status[48];
  if (poll && poll[0]) {
    snprintf(status, sizeof(status), "%s", poll);
  } else if (age[0]) {
    snprintf(status, sizeof(status), "%s · %s · %s", wearLabel(t), track, age);
  } else {
    snprintf(status, sizeof(status), "%s · %s", wearLabel(t), track);
  }

  char hr[20], hrv[20], spo2[20], temp[20], bat[20];
  fmtVal(hr,   sizeof(hr),   t.hrValid,      t.hr,      " bpm");
  fmtVal(hrv,  sizeof(hrv),  t.hrvValid,     t.hrv,     " ms");
  fmtVal(spo2, sizeof(spo2), t.spo2Valid,    t.spo2,    "%");
  if (!t.tempValid) snprintf(temp, sizeof(temp), "-- C");
  else snprintf(temp, sizeof(temp), "%d.%d C",
                (int)(t.tempTenths / 10),
                (int)(t.tempTenths < 0 ? -(t.tempTenths % 10) : (t.tempTenths % 10)));
  fmtVal(bat,  sizeof(bat),  t.batteryValid, t.battery, "%");

  snprintf(out, cap,
           "HR   %s\n"
           "HRV  %s\n"
           "SpO2 %s\n"
           "T    %s\n"
           "Bat  %s\n"
           "%s",
           hr, hrv, spo2, temp, bat, status);
}

// Right-align a 2× axis label in the left gutter (3×5 → 6×10 px).
// tenths=true → value is °C×10, label as N.N.
static void drawAxisLabel(int value, int rightX, int y, int shade, bool tenths) {
  constexpr int kScale = 2;
  constexpr int kAdvance = 3 * kScale + kScale;  // 8
  char buf[8];
  if (tenths) {
    const int whole = value / 10;
    int frac = value % 10;
    if (frac < 0) frac = -frac;
    snprintf(buf, sizeof(buf), "%d.%d", whole, frac);
  } else {
    snprintf(buf, sizeof(buf), "%d", value);
  }
  const int w = (int)strlen(buf) * kAdvance;
  drawTextScaled(buf, rightX - w, y, shade, kScale);
}

// ── X (time) axis ────────────────────────────────────────────────────────────

// Elapsed-duration label: "15s" / "2m" / "7.5m" / "4h" / "3.5d". Not fmtAge:
// that truncates (7.5m → "7m"), fine for a sample age, wrong for an axis mark
// that claims a position. Rung values and their halves are exact in one
// decimal everywhere the ladder can put a tick.
static void fmtElapsedLabel(char* out, size_t cap, uint32_t ms) {
  static const struct { uint32_t ms; char suffix; } kUnits[] = {
    { 86400000u, 'd' }, { 3600000u, 'h' }, { 60000u, 'm' }, { 1000u, 's' },
  };
  for (size_t i = 0; i < sizeof(kUnits) / sizeof(kUnits[0]); i++) {
    if (ms < kUnits[i].ms) continue;
    const uint32_t whole = ms / kUnits[i].ms;
    const uint32_t tenth = (ms % kUnits[i].ms) * 10u / kUnits[i].ms;
    if (tenth == 0)
      snprintf(out, cap, "%lu%c", (unsigned long)whole, kUnits[i].suffix);
    else
      snprintf(out, cap, "%lu.%lu%c", (unsigned long)whole,
               (unsigned long)tenth, kUnits[i].suffix);
    return;
  }
  snprintf(out, cap, "0");
}

// Hours:minutes into the ring's UTC day for Trends ("0:00" … "24:00"). 24:00
// is deliberate: the right edge of a full ring day IS the next midnight, and
// wrapping it to "0:00" would relabel the window's end as its start.
static void fmtDayOffsetLabel(char* out, size_t cap, uint32_t secIntoDay) {
  snprintf(out, cap, "%lu:%02lu",
           (unsigned long)(secIntoDay / 3600u),
           (unsigned long)((secIntoDay % 3600u) / 60u));
}

// Local wall-clock label for the live views. H:MM inside two days; the
// multi-day rungs label M/D instead (an hour means nothing at 4 px per hour).
static void fmtWallLabel(char* out, size_t cap, int64_t epochMs, uint32_t windowMs) {
  time_t t = (time_t)(epochMs / 1000);
  struct tm tm;
  localtime_r(&t, &tm);
  if (windowMs >= 48u * 3600u * 1000u)
    snprintf(out, cap, "%d/%d", tm.tm_mon + 1, tm.tm_mday);
  else
    snprintf(out, cap, "%d:%02d", tm.tm_hour, tm.tm_min);
}

// Labels the plot WINDOW (left edge = oldest sample, right edge = oldest +
// rung), never the trace tip: all three tick positions are constants, and the
// text changes only when the data genuinely reframes (eviction, rung crossing,
// backfill) — the same repaint-moves-nothing contract as the trace itself.
// Every mode is a pure function of buffer + window, never of the render
// instant:
//
//  - Trends (trendMeta != null): hours into the ring's UTC day, from the meta
//    window. Trend .ms is FETCH-anchored (backfillSampleMs pins the newest
//    record at the fetch instant), so wall time derived from .ms would print
//    the fetch time; the meta is the only honest source. Skipped when the
//    window fails backfill's own honoured-window gate — the trace was laid on
//    the nominal 60 s ladder and day labels would misplace it.
//  - Live, synced host, window ≥ 5 min: local wall clock. epochMillis() and
//    millis() advance in lockstep, so epochNow − age is repaint-stable; it
//    steps only when the clock itself steps (NTP / ring-clock custody), which
//    is exactly when the frame SHOULD reframe.
//  - Otherwise elapsed-from-start ("0 / 2h / 4h"): dark host, or a window too
//    small for distinct H:MM marks. Anchored to the window start, not "now",
//    so even a dark-host repaint stays pixel-identical.
static void drawTimeAxis(int plotX, int plotW, int tickY, int labelY,
                         uint32_t oldestAgeMs, uint32_t windowMs,
                         const TrendMeta* trendMeta) {
  if (windowMs == 0) return;  // uniform spacing — no time domain to label

  // Real contents are ≤ 6 chars ("23:59", "59.9m", "12/31"); 24 covers the
  // compiler's format-worst-case so -Wformat-truncation stays quiet.
  char left[24], mid[24], right[24];
  if (trendMeta) {
    if (!trendMeta->have) return;
    const uint32_t span = (trendMeta->endTs > trendMeta->startTs)
                              ? (trendMeta->endTs - trendMeta->startTs) : 0;
    if (span == 0 || span > kBackfillMaxSpanSec) return;  // .ms on 60 s ladder
    if (!Clock::isValidEpoch((time_t)trendMeta->startTs)) return;
    const uint32_t base = trendMeta->startTs % 86400u;
    fmtDayOffsetLabel(left, sizeof(left), base);
    fmtDayOffsetLabel(mid, sizeof(mid), base + windowMs / 2000u);
    fmtDayOffsetLabel(right, sizeof(right), base + windowMs / 1000u);
  } else if (Clock::isSynced() && windowMs >= 5u * 60u * 1000u) {
    const int64_t leftEpochMs = Clock::epochMillis() - (int64_t)oldestAgeMs;
    fmtWallLabel(left, sizeof(left), leftEpochMs, windowMs);
    fmtWallLabel(mid, sizeof(mid), leftEpochMs + (int64_t)(windowMs / 2u), windowMs);
    fmtWallLabel(right, sizeof(right), leftEpochMs + (int64_t)windowMs, windowMs);
  } else {
    snprintf(left, sizeof(left), "0");
    fmtElapsedLabel(mid, sizeof(mid), windowMs / 2u);
    fmtElapsedLabel(right, sizeof(right), windowMs);
  }

  // Tick stubs under the bottom border, mirroring the Y ticks' 6/5/6 shades.
  const int midX = plotX + plotW / 2;
  const int rightX = plotX + plotW - 1;
  drawVLine(plotX, tickY, tickY + 2, 6);
  drawVLine(midX, tickY, tickY + 2, 5);
  drawVLine(rightX, tickY, tickY + 2, 6);

  constexpr int kScale = 2;
  constexpr int kAdvance = 3 * kScale + kScale;             // 8 px per glyph
  const int wMid   = (int)strlen(mid) * kAdvance - kScale;  // drop last gap
  const int wRight = (int)strlen(right) * kAdvance - kScale;
  drawTextScaled(left, plotX, labelY, 14, kScale);
  drawTextScaled(mid, midX - wMid / 2, labelY, 12, kScale);
  drawTextScaled(right, rightX - wRight + 1, labelY, 14, kScale);
}

// Graph-only tile — title/value live in the native text pane above.
// Y axis uses each metric's auto-scaled bounds (HR ≠ battery 0–100).
static void composeMetricGraph(const HealthSeries* s,
                               int16_t softMin, int16_t softMax, int16_t minSpan,
                               bool tenthsAxis = false,
                               const char* emptyHint = "Poll Now",
                               const TrendMeta* trendMeta = nullptr) {
  clearGrid(0);
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  // Empty / offline copy uses the same 2× glyphs as the Y-axis numbers
  // (3×5 → 6×10). Unscaled 1× text was hard to read on the lens.
  constexpr int kEmptyScale = 2;
  constexpr int kEmptyH     = 5 * kEmptyScale;
  constexpr int kEmptyGap   = 6;
  if (!t.connected) {
    drawTextScaled("offline", 8, HEALTH_TILE_H / 2 - kEmptyH / 2, 14, kEmptyScale);
    return;
  }
  if (!s || s->count < 2) {
    const char* hint = emptyHint ? emptyHint : "Poll Now";
    const int y1 = HEALTH_TILE_H / 2 - kEmptyH - kEmptyGap / 2;
    const int y2 = HEALTH_TILE_H / 2 + kEmptyGap / 2;
    drawTextScaled("no samples", 8, y1, 14, kEmptyScale);
    drawTextScaled(hint, 8, y2, 12, kEmptyScale);
    return;
  }

  // Left gutter for 2× Y labels (3 digits × 8 px ≈ 24); bottom strip for the
  // 2× time axis (3 px tick stubs + 10 px labels); plot uses the rest.
  constexpr int kGutter = 34;
  constexpr int kPlotX  = kGutter;
  constexpr int kPlotY  = 4;
  constexpr int kPlotW  = HEALTH_TILE_W - kGutter - 4;
  constexpr int kPlotH  = HEALTH_TILE_H - 22;  // was -8; 14 px ceded to X axis
  constexpr int kLabelH = 10;  // 5×2

  int16_t usedMin = softMin, usedMax = softMax;
  uint32_t oldestAgeMs = 0, windowMs = 0;
  drawSparkline(s, kPlotX, kPlotY, kPlotW, kPlotH,
                softMin, softMax, minSpan, 13, &usedMin, &usedMax,
                &oldestAgeMs, &windowMs);

  const int16_t mid = (int16_t)(usedMin + (usedMax - usedMin) / 2);
  const int labelRight = kGutter - 2;
  drawAxisLabel((int)usedMax, labelRight, kPlotY, 14, tenthsAxis);
  drawAxisLabel((int)mid,     labelRight, kPlotY + kPlotH / 2 - kLabelH / 2, 12, tenthsAxis);
  drawAxisLabel((int)usedMin, labelRight, kPlotY + kPlotH - kLabelH, 14, tenthsAxis);

  // Tick marks on the plot's left edge at max / mid / min.
  drawHLine(kPlotX, kPlotX + 3, kPlotY, 6);
  drawHLine(kPlotX, kPlotX + 3, kPlotY + kPlotH / 2, 5);
  drawHLine(kPlotX, kPlotX + 3, kPlotY + kPlotH - 1, 6);

  drawTimeAxis(kPlotX, kPlotW, kPlotY + kPlotH, kPlotY + kPlotH + 5,
               oldestAgeMs, windowMs, trendMeta);
}

static void metricParams(G2HealthMetric m,
                         const char** title, const HealthSeries** s,
                         int16_t* softMin, int16_t* softMax, int16_t* minSpan,
                         const char** unit, int16_t* cur, bool* valid,
                         int32_t* ageSec, bool* tenths) {
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  *title = "";
  *s = nullptr;
  *softMin = 0; *softMax = 100; *minSpan = 5;
  *unit = "";
  *cur = 0; *valid = false;
  *ageSec = -1;
  *tenths = false;
  switch (m) {
    case HEALTH_METRIC_HR:
      *title = "Heart Rate"; *s = &sHr;
      *softMin = 30; *softMax = 200; *minSpan = 12; *unit = " bpm";
      *cur = t.hr; *valid = t.hrValid; *ageSec = t.hrAgeSec;
      break;
    case HEALTH_METRIC_HRV:
      *title = "HRV"; *s = &sHrv;
      *softMin = 0; *softMax = 250; *minSpan = 10; *unit = " ms";
      *cur = t.hrv; *valid = t.hrvValid; *ageSec = t.hrvAgeSec;
      break;
    case HEALTH_METRIC_SPO2:
      *title = "SpO2"; *s = &sSpo2;
      *softMin = 70; *softMax = 100; *minSpan = 4; *unit = "%";
      *cur = t.spo2; *valid = t.spo2Valid; *ageSec = t.spo2AgeSec;
      break;
    case HEALTH_METRIC_TEMP:
      *title = "Temp"; *s = &sTemp;
      *softMin = 250; *softMax = 420; *minSpan = 10; *unit = " C";
      *cur = t.tempTenths; *valid = t.tempValid; *ageSec = t.tempAgeSec;
      *tenths = true;
      break;
    case HEALTH_METRIC_BATTERY:
      *title = "Battery"; *s = &sBat;
      *softMin = 0; *softMax = 100; *minSpan = 5; *unit = "%";
      *cur = t.battery; *valid = t.batteryValid; *ageSec = t.batteryAgeSec;
      break;
    default: break;
  }
}

// Two-column line spanning the metric text pane width (same 288 px as
// the graph below): left flush, right flush. Lens text is roughly
// proportional so gaps aren't perfect monospace, but the pair sits
// centered over the image instead of bunched on the left.
static void healthTwoCol(char* out, size_t cap,
                         const char* left, const char* right) {
  // ~30 columns fits the 288 px pane at the glasses' default text size.
  constexpr int kTotalW = 30;
  const int llen = left  ? (int)strlen(left)  : 0;
  const int rlen = right ? (int)strlen(right) : 0;
  int gap = kTotalW - llen - rlen;
  if (gap < 2) gap = 2;
  snprintf(out, cap, "%s%*s%s", left ? left : "", gap, "", right ? right : "");
}

static void fmtHmUtc(char* out, size_t cap, uint32_t ts) {
  if (!out || !cap) return;
  if (!Clock::isValidEpoch((time_t)ts)) {
    snprintf(out, cap, "--:--");
    return;
  }
  time_t t = (time_t)ts;
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// Day identity for the Trends views. A valid epoch renders as "Jul 29";
// anything else (dark host + dark/never-synced ring) falls back to the boot
// counter — "boot 123" — the only honest day identity a dark boot has.
// Mirrors the sensor-capture naming scheme (boot-<N>/ folders, retro-dated
// by System_TimeAnchors once real time arrives).
static void fmtTrendDayLabel(char* out, size_t cap, uint32_t ts) {
  if (!out || !cap) return;
  if (Clock::isValidEpoch((time_t)ts)) {
    static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
    time_t t = (time_t)ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, cap, "%s %d", kMon[tm.tm_mon], tm.tm_mday);
  } else {
    snprintf(out, cap, "boot %lu", (unsigned long)gBootCounter);
  }
}

static void fmtTrendLine(char* out, size_t cap, const char* tag, const TrendMeta* m) {
  if (!out || !cap) return;
  if (!m || !m->have) {
    snprintf(out, cap, "%s --", tag);
    return;
  }
  char a[8], b[8];
  fmtHmUtc(a, sizeof(a), m->startTs);
  fmtHmUtc(b, sizeof(b), m->endTs);
  snprintf(out, cap, "%s avg %d n=%u %s-%s",
           tag, (int)m->avg, (unsigned)m->count, a, b);
}

void g2HealthBuildTrendsOverviewText(char* out, size_t cap) {
  if (!out || !cap) return;
  if (!g2RingIsConnected()) {
    snprintf(out, cap,
             "Trends\n"
             "Ring offline\n"
             "Pair via BT / R1");
    return;
  }
  char hr[48], hrv[48], spo2[48];
  fmtTrendLine(hr, sizeof(hr), "HR", &sTrendMetaHr);
  fmtTrendLine(hrv, sizeof(hrv), "HRV", &sTrendMetaHrv);
  fmtTrendLine(spo2, sizeof(spo2), "O2", &sTrendMetaSpo2);
  // Day identity: freshest valid ring day-window end across the metas; when
  // none, local now — which itself degrades to "boot N" when the host is
  // dark too (fmtTrendDayLabel).
  uint32_t dayTs = 0;
  if (sTrendMetaHr.have   && sTrendMetaHr.endTs   > dayTs) dayTs = sTrendMetaHr.endTs;
  if (sTrendMetaHrv.have  && sTrendMetaHrv.endTs  > dayTs) dayTs = sTrendMetaHrv.endTs;
  if (sTrendMetaSpo2.have && sTrendMetaSpo2.endTs > dayTs) dayTs = sTrendMetaSpo2.endTs;
  if (!Clock::isValidEpoch((time_t)dayTs)) dayTs = (uint32_t)time(nullptr);
  char day[16];
  fmtTrendDayLabel(day, sizeof(day), dayTs);
  const bool fetching = (sDailyQueueIdx < sDailyQueueLen);
  snprintf(out, cap,
           "Trends (%s)\n"
           "%s\n"
           "%s\n"
           "%s\n"
           "%s",
           day, hr, hrv, spo2,
           fetching ? "Fetching..." : "Tap metric / Refresh");
}

void g2HealthBuildMetricText(char* out, size_t cap) {
  if (!out || !cap) return;
  if (sSelected == HEALTH_METRIC_OVERVIEW) {
    if (sNav == HEALTH_NAV_TRENDS) g2HealthBuildTrendsOverviewText(out, cap);
    else                          g2HealthBuildOverviewText(out, cap);
    return;
  }

  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  if (!t.connected) {
    snprintf(out, cap, "Ring offline\nPair via\nNetwork / BT / R1");
    return;
  }

  // Trends day graph: title/avg/range from last daily payload.
  if (sNav == HEALTH_NAV_TRENDS) {
    const HealthSeries* s = trendSeriesFor(sSelected);
    const TrendMeta* meta = trendMetaFor(sSelected);
    const char* tag = "";
    const char* unit = "";
    switch (sSelected) {
      case HEALTH_METRIC_HR:   tag = "HR";   unit = " bpm"; break;
      case HEALTH_METRIC_HRV:  tag = "HRV";  unit = " ms";  break;
      case HEALTH_METRIC_SPO2: tag = "SpO2"; unit = "%";    break;
      default: break;
    }
    // Honest day label: the payload's own window end when it carries a real
    // date, else local now / "boot N" (fmtTrendDayLabel).
    uint32_t dayTs = (meta && meta->have) ? meta->endTs : 0;
    if (!Clock::isValidEpoch((time_t)dayTs)) dayTs = (uint32_t)time(nullptr);
    char day[16];
    fmtTrendDayLabel(day, sizeof(day), dayTs);
    char title[28];
    snprintf(title, sizeof(title), "%s %s", tag, day);
    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "n=%u", (unsigned)(meta && meta->have ? meta->count : 0));
    char val[28];
    if (meta && meta->have) {
      snprintf(val, sizeof(val), "avg %d%s", (int)meta->avg, unit);
    } else {
      snprintf(val, sizeof(val), "--%s", unit);
    }
    char win[40];
    if (meta && meta->have) {
      char a[8], b[8];
      fmtHmUtc(a, sizeof(a), meta->startTs);
      fmtHmUtc(b, sizeof(b), meta->endTs);
      snprintf(win, sizeof(win), "%s-%s", a, b);
    } else {
      snprintf(win, sizeof(win), "no data");
    }
    // Sized for healthTwoCol's compiler-visible worst case (kTotalW pad +
    // both columns maxed); real lines stay ≤ ~44 chars.
    char line1[76], line2[100];
    healthTwoCol(line1, sizeof(line1), title, nbuf);
    healthTwoCol(line2, sizeof(line2), val, win);
    if (s && meta && s->count >= 2) {
      snprintf(out, cap, "%s\n%s\nmin %d max %d",
               line1, line2, (int)meta->minV, (int)meta->maxV);
    } else {
      snprintf(out, cap, "%s\n%s\nTap Refresh", line1, line2);
    }
    return;
  }

  const char* title = "";
  const HealthSeries* s = nullptr;
  int16_t softMin = 0, softMax = 100, minSpan = 5;  // axis soft limits (unused here)
  const char* unit = "";
  int16_t cur = 0;
  bool valid = false;
  int32_t ageSec = -1;
  bool tenths = false;
  metricParams(sSelected, &title, &s, &softMin, &softMax, &minSpan, &unit, &cur,
               &valid, &ageSec, &tenths);
  (void)softMin; (void)softMax; (void)minSpan;

  char val[32];  // fmtValAge worst case: base[20] + " · " + age[8]
  if (tenths) fmtTempAge(val, sizeof(val), valid, cur, ageSec);
  else        fmtValAge(val, sizeof(val), valid, cur, unit, ageSec);

  // Range lives on the graph Y-axis now — right column is n= / TRACK / wear.
  const char* track =
      healthTrackIsActive() ? "TRACK on" :
      (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) ? "LOG on" : "Track off";
  const char* poll = healthPollStatusLine(nullptr);
  char status[40];
  if (poll && poll[0]) snprintf(status, sizeof(status), "%s", poll);
  else                 snprintf(status, sizeof(status), "%s", track);

  char nbuf[16];
  snprintf(nbuf, sizeof(nbuf), "n=%u", (unsigned)(s ? s->count : 0));

  // Sized for healthTwoCol's compiler-visible worst case, as above.
  char line1[76], line2[104];
  //   Heart Rate              n=24
  //   72 bpm · 3m         TRACK on / Polling... / Updated
  healthTwoCol(line1, sizeof(line1), title, nbuf);
  healthTwoCol(line2, sizeof(line2), val, status);
  if (s && s->count >= 2) {
    snprintf(out, cap, "%s\n%s", line1, line2);
  } else {
    snprintf(out, cap, "%s\n%s\nTap Poll Now", line1, line2);
  }
}

static void composeGrid(void) {
  // Overview has no image; metric / Trends BMP is graph-only.
  if (sSelected == HEALTH_METRIC_OVERVIEW) {
    clearGrid(0);
    return;
  }
  if (sNav == HEALTH_NAV_TRENDS) {
    switch (sSelected) {
      case HEALTH_METRIC_HR:
        composeMetricGraph(&sTrendHr, 30, 200, 12, false, "Refresh", &sTrendMetaHr);
        break;
      case HEALTH_METRIC_HRV:
        composeMetricGraph(&sTrendHrv, 0, 250, 10, false, "Refresh", &sTrendMetaHrv);
        break;
      case HEALTH_METRIC_SPO2:
        composeMetricGraph(&sTrendSpo2, 70, 100, 4, false, "Refresh", &sTrendMetaSpo2);
        break;
      default:
        clearGrid(0);
        break;
    }
    return;
  }
  switch (sSelected) {
    case HEALTH_METRIC_HR:
      composeMetricGraph(&sHr, 30, 200, 12);
      break;
    case HEALTH_METRIC_HRV:
      composeMetricGraph(&sHrv, 0, 250, 10);
      break;
    case HEALTH_METRIC_SPO2:
      composeMetricGraph(&sSpo2, 70, 100, 4);
      break;
    case HEALTH_METRIC_TEMP:
      composeMetricGraph(&sTemp, 250, 420, 10, true);
      break;
    case HEALTH_METRIC_BATTERY:
      composeMetricGraph(&sBat, 0, 100, 5);
      break;
    default:
      clearGrid(0);
      break;
  }
}

static size_t healthPackBmp(uint8_t* out, size_t cap) {
  const uint32_t w = HEALTH_TILE_W, h = HEALTH_TILE_H;
  const uint32_t rowStride = ((w * 4 + 31) / 32) * 4;
  const uint32_t pixelSize = rowStride * h;
  const uint32_t headerSize = 14 + 40 + 64;
  const uint32_t total = headerSize + pixelSize;
  if (!out || total > cap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
  };
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2, total); wr16(out + 6, 0); wr16(out + 8, 0); wr32(out + 10, headerSize);
  wr32(out + 14, 40); wr32(out + 18, w); wr32(out + 22, (uint32_t)(-(int32_t)h));
  wr16(out + 26, 1); wr16(out + 28, 4); wr32(out + 30, 0); wr32(out + 34, pixelSize);
  wr32(out + 38, 2835); wr32(out + 42, 2835); wr32(out + 46, 16); wr32(out + 50, 0);
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i * 4 + 0] = v; out[54 + i * 4 + 1] = v; out[54 + i * 4 + 2] = v; out[54 + i * 4 + 3] = 0;
  }
  uint8_t* pixels = out + headerSize;
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* dst = pixels + y * rowStride;
    for (uint32_t x = 0; x < w; x += 2)
      dst[x / 2] = (uint8_t)((sGrid[y][x] << 4) | (sGrid[y][x + 1] & 0x0f));
    // Pad stride tail
    for (uint32_t p = w / 2; p < rowStride; p++) dst[p] = 0;
  }
  return total;
}

size_t g2RenderHealthBmp(uint8_t* out, size_t cap) {
  if (sSelected == HEALTH_METRIC_OVERVIEW) return 0;
  composeGrid();
  return healthPackBmp(out, cap);
}

size_t g2HealthBmpCap(void) {
  const uint32_t rowStride = ((HEALTH_TILE_W * 4 + 31) / 32) * 4;
  return (size_t)(14 + 40 + 64) + (size_t)rowStride * HEALTH_TILE_H;
}

void g2HealthBuildInfo(char* out, size_t cap) {
  if (!out || !cap) return;
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  char temp[12];
  if (!t.tempValid) snprintf(temp, sizeof(temp), "?");
  else snprintf(temp, sizeof(temp), "%d.%d",
                (int)(t.tempTenths / 10),
                (int)(t.tempTenths < 0 ? -(t.tempTenths % 10) : (t.tempTenths % 10)));
  snprintf(out, cap,
           "Health — %s\nHR %s%u  HRV %s%d\nSpO2 %s%u  T %s\nBat %s%u  %s\nn=%u/%u/%u/%u/%u",
           t.connected ? "online" : "offline",
           t.hrValid ? "" : "?", (unsigned)t.hr,
           t.hrvValid ? "" : "?", (int)t.hrv,
           t.spo2Valid ? "" : "?", (unsigned)t.spo2,
           temp,
           t.batteryValid ? "" : "?", (unsigned)t.battery,
           wearLabel(t),
           (unsigned)sHr.count, (unsigned)sHrv.count,
           (unsigned)sSpo2.count, (unsigned)sTemp.count, (unsigned)sBat.count);
}

#endif  // ENABLE_R1_HEALTH
