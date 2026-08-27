#ifndef G2_HEALTH_H
#define G2_HEALTH_H

// =============================================================================
// G2 glasses — "Health" app (R1 ring vitals + sparklines + Trends daily)
// =============================================================================
// Reached from the Apps launcher (Apps -> Health). Mode-switch compound:
// Overview = list + native text vitals; metric rows = list + text (title/value)
// + 288×144 graph-only image (Q30-proven 3-pane). List name is "lstHealth".
// Trends is a submenu (nav MAIN ↔ TRENDS) that graphs ring daily-history
// separately from the live sparklines.
//
// Portable TU: history buffers + BMP/text builders only. No BLE/hijack — the
// worker in G2_Glasses.cpp owns transport and calls into here.
// =============================================================================

#include "System_BuildConfig.h"
#include "R1_HealthHistoryStore.h"
#include "System_R1_Protocol.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_R1_HEALTH

// Tile geometry — Maps-sized so sparklines stay readable. W must be even (4bpp).
static constexpr int HEALTH_TILE_W = 288;
static constexpr int HEALTH_TILE_H = 144;

// Per-metric sample capacity for in-RAM sparklines / Trends day series.
static constexpr size_t HEALTH_HIST_CAP = 96;

enum G2HealthMetric : uint8_t {
  HEALTH_METRIC_OVERVIEW = 0,
  HEALTH_METRIC_HR,
  HEALTH_METRIC_HRV,
  HEALTH_METRIC_SPO2,
  HEALTH_METRIC_TEMP,
  HEALTH_METRIC_BATTERY,
  HEALTH_METRIC_ACTIVITY,
};

// MAIN = live Overview / metrics; TRENDS = daily-history submenu.
enum G2HealthNav : uint8_t {
  HEALTH_NAV_MAIN = 0,
  HEALTH_NAV_TRENDS,
};

size_t g2HealthBmpCap(void);
const char* const* g2HealthMenuRows(size_t* count);

// Bumps when MAIN ↔ TRENDS menu rows change (worker recreates the list).
uint32_t g2HealthMenuGeneration(void);
G2HealthNav g2HealthNav(void);
bool g2HealthInTrendsNav(void);

void g2HealthOpen(void);
void g2HealthTick(void);
void g2HealthAction(uint32_t rowIdx);

// True while HardwareOne Health logging is actively writing R1 vitals.
bool g2HealthLoggingIsActive(void);

// Selected metric. In Trends nav: OVERVIEW = Trends landing; HR/HRV/SPO2 =
// day graphs. Live Temp/Battery are MAIN-only.
G2HealthMetric g2HealthSelectedMetric(void);

// True when the current view is list+text only (MAIN Overview or Trends landing).
bool g2HealthWantTextOnlyShape(void);

// True when the worker should start an explicit Poll Now refresh. Page entry
// uses the separate normal, freshness-throttled history lane.
bool g2HealthConsumePollRequest(void);

// Worker lifecycle: suppress parallel ring traffic (sensorlog) while open, and
// drive "Refreshing…/Refreshed" status in Overview/metric text so Poll Now is
// visible even when vitals values are unchanged.
void g2HealthSetPageActive(bool active);
bool g2HealthPageIsActive(void);
void g2HealthNotePollStarted(void);
void g2HealthNotePollFinished(void);
void g2HealthNotePollFailed(bool partial);
void g2HealthNotePollUnsupported(void);

// A new direct-ring connection generation must not inherit live point-series
// samples from the prior peer/profile. Typed persisted trends are separate.
void g2HealthResetLiveTelemetry(void);

// One-shot graph push scheduling. Metric views push the sparkline once
// (after a short settle so daily backfill can land), not on every live
// sample. Poll Now / Trends Refresh re-arms. Worker: arm → wait → consume → BLE push.
void g2HealthArmGraphPush(uint32_t delayMs);
bool g2HealthConsumeGraphPush(void);
void g2HealthClearGraphPush(void);

// One request for the single transport-owned history coordinator. `force`
// distinguishes explicit bypass of normal freshness gates from normal refresh.
bool g2HealthConsumeHistoryRefreshRequest(bool* force);

// Append a live sample. metric: HEALTH_METRIC_HR..BATTERY. ringTs is the
// ring's epoch timestamp when known (0 = unknown → value+time gate dedupe).
void g2HealthNoteSample(G2HealthMetric metric, int16_t value, uint32_t ringTs);

// Apply a parsed daily-history payload into the matching *live* series when
// that series is still thin (used when a daily query response arrives).
void g2HealthApplyDailyBackfill(G2HealthMetric metric,
                                const uint8_t* values, size_t count,
                                uint32_t startTs, uint32_t endTs);

// Replace the Trends day series for HR/HRV/SpO2 (always, when count > 0).
void g2HealthApplyTrendDaily(G2HealthMetric metric,
                             const uint8_t* values, size_t count,
                             uint32_t startTs, uint32_t endTs);

size_t g2HealthHistoryCount(G2HealthMetric metric);

// Render selected metric / Trends day graph as graph-only 4bpp BMP.
size_t g2RenderHealthBmp(uint8_t* out, size_t cap);

// Native-font Overview readout for list+text compound (UPDATE_TEXT).
void g2HealthBuildOverviewText(char* out, size_t cap);

// Native-font metric / Trends readout for list+text+image.
void g2HealthBuildMetricText(char* out, size_t cap);

// Trends landing (list+text) summary of last daily fetches.
void g2HealthBuildTrendsOverviewText(char* out, size_t cap);

// Text-only dispatcher for Overview, Trends landing, and Activity summary.
void g2HealthBuildTextOnly(char* out, size_t cap);

// Typed daily-history boundary. Ring-owner calls only validate and enqueue into
// fixed-capacity PSRAM. The normal main-loop loads the exact persisted
// peer/day/timezone key before merging or displaying a page, then stages only
// a terminal COMPLETE/PARTIAL/ERROR generation for the secure store.
// Pure boot invariant test for desired-peer deferral/reassertion ordering; it
// does not read or mutate the live history model.
bool g2HealthHistoryPeerCustodySelfTest(void);
void g2HealthSetHistoryPeerId(const char* canonicalMac);
bool g2HealthApplyCommonDaily(const R1CommonDailyResult& result);
bool g2HealthApplyHrvDaily(const R1HrvDailyResult& result);
bool g2HealthApplyActivityDaily(const R1ActivityDailyResult& result);
void g2HealthHistoryFetchStarted(void);
void g2HealthHistoryFetchFinished(bool successfulSweep, uint8_t errorCode);
void g2HealthHistorySetSleepState(R1HealthSleepState state);
struct G2HealthHistorySummary {
  bool available;
  uint8_t protocolProfile;  // persisted R1HealthHistoryLayout ID (legacy name)
  uint32_t dayStart;
  int16_t timezoneMinutes;
  R1HealthHistoryFetchState fetchState;
  uint32_t lastSuccessEpoch;
  uint32_t lastPartialEpoch;
  uint8_t fetchError;
  R1HealthSleepState sleepState;
  R1HealthActivitySummary activity;
};
bool g2HealthHistorySnapshot(R1HealthHistoryDay& out);
void g2HealthHistoryGetSummary(G2HealthHistorySummary& out);
void g2HealthActivitySummary(R1HealthActivitySummary& out);
// Main-loop coordinator: snapshots a dirty generation, stages it to the
// secure store, then advances store recovery/commit. Never call from notify.
void g2HealthHistoryCommitTick(void);

void g2HealthBuildInfo(char* out, size_t cap);

#else  // stubs

static constexpr int HEALTH_TILE_W = 288;
static constexpr int HEALTH_TILE_H = 144;
enum G2HealthMetric : uint8_t {
  HEALTH_METRIC_OVERVIEW = 0,
  HEALTH_METRIC_HR,
  HEALTH_METRIC_HRV,
  HEALTH_METRIC_SPO2,
  HEALTH_METRIC_TEMP,
  HEALTH_METRIC_BATTERY,
  HEALTH_METRIC_ACTIVITY,
};
enum G2HealthNav : uint8_t {
  HEALTH_NAV_MAIN = 0,
  HEALTH_NAV_TRENDS,
};
inline size_t g2HealthBmpCap(void) { return 0; }
inline const char* const* g2HealthMenuRows(size_t* count) { if (count) *count = 0; return nullptr; }
inline uint32_t g2HealthMenuGeneration(void) { return 0; }
inline G2HealthNav g2HealthNav(void) { return HEALTH_NAV_MAIN; }
inline bool g2HealthInTrendsNav(void) { return false; }
inline void g2HealthOpen(void) {}
inline void g2HealthTick(void) {}
inline void g2HealthAction(uint32_t) {}
inline bool g2HealthLoggingIsActive(void) { return false; }
inline G2HealthMetric g2HealthSelectedMetric(void) { return HEALTH_METRIC_OVERVIEW; }
inline bool g2HealthWantTextOnlyShape(void) { return true; }
inline bool g2HealthConsumePollRequest(void) { return false; }
inline void g2HealthSetPageActive(bool) {}
inline bool g2HealthPageIsActive(void) { return false; }
inline void g2HealthNotePollStarted(void) {}
inline void g2HealthNotePollFinished(void) {}
inline void g2HealthNotePollFailed(bool) {}
inline void g2HealthNotePollUnsupported(void) {}
inline void g2HealthResetLiveTelemetry(void) {}
inline void g2HealthArmGraphPush(uint32_t) {}
inline bool g2HealthConsumeGraphPush(void) { return false; }
inline void g2HealthClearGraphPush(void) {}
inline bool g2HealthConsumeHistoryRefreshRequest(bool*) { return false; }
inline void g2HealthNoteSample(G2HealthMetric, int16_t, uint32_t) {}
inline void g2HealthApplyDailyBackfill(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {}
inline void g2HealthApplyTrendDaily(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {}
inline size_t g2HealthHistoryCount(G2HealthMetric) { return 0; }
inline size_t g2RenderHealthBmp(uint8_t*, size_t) { return 0; }
inline void g2HealthBuildOverviewText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildMetricText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildTrendsOverviewText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildTextOnly(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline bool g2HealthHistoryPeerCustodySelfTest(void) { return true; }
inline void g2HealthSetHistoryPeerId(const char*) {}
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
inline bool g2HealthApplyCommonDaily(const R1CommonDailyResult&) { return false; }
inline bool g2HealthApplyHrvDaily(const R1HrvDailyResult&) { return false; }
inline bool g2HealthApplyActivityDaily(const R1ActivityDailyResult&) { return false; }
#endif
inline void g2HealthHistoryFetchStarted(void) {}
inline void g2HealthHistoryFetchFinished(bool, uint8_t) {}
inline void g2HealthHistorySetSleepState(R1HealthSleepState) {}
struct G2HealthHistorySummary {
  bool available;
  uint8_t protocolProfile;  // persisted R1HealthHistoryLayout ID (legacy name)
  uint32_t dayStart;
  int16_t timezoneMinutes;
  R1HealthHistoryFetchState fetchState;
  uint32_t lastSuccessEpoch;
  uint32_t lastPartialEpoch;
  uint8_t fetchError;
  R1HealthSleepState sleepState;
  R1HealthActivitySummary activity;
};
inline bool g2HealthHistorySnapshot(R1HealthHistoryDay&) { return false; }
inline void g2HealthHistoryGetSummary(G2HealthHistorySummary& out) { out = G2HealthHistorySummary{}; }
inline void g2HealthActivitySummary(R1HealthActivitySummary& out) { out = R1HealthActivitySummary{}; }
inline void g2HealthHistoryCommitTick(void) {}
inline void g2HealthBuildInfo(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }

#endif  // ENABLE_R1_HEALTH
#endif  // G2_HEALTH_H
