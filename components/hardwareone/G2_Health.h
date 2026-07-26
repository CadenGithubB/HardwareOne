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

// True while Health Track is actively logging R1 vitals (setting + mask + running).
bool g2HealthTrackIsActive(void);

// Selected metric. In Trends nav: OVERVIEW = Trends landing; HR/HRV/SPO2 =
// day graphs. Live Temp/Battery are MAIN-only.
G2HealthMetric g2HealthSelectedMetric(void);

// True when the current view is list+text only (MAIN Overview or Trends landing).
bool g2HealthWantTextOnlyShape(void);

// True when the worker should kick a vitals poll burst (entry or Poll Now).
bool g2HealthConsumePollRequest(void);

// Worker lifecycle: suppress parallel ring polls (sensorlog) while open, and
// drive "Polling…/Updated" status in Overview/metric text so Poll Now is visible
// even when vitals values are unchanged.
void g2HealthSetPageActive(bool active);
bool g2HealthPageIsActive(void);
void g2HealthNotePollStarted(void);
void g2HealthNotePollFinished(void);

// One-shot graph push scheduling. Metric views push the sparkline once
// (after a short settle so daily backfill can land), not on every live
// sample. Poll Now / Trends Refresh re-arms. Worker: arm → wait → consume → BLE push.
void g2HealthArmGraphPush(uint32_t delayMs);
bool g2HealthConsumeGraphPush(void);
void g2HealthClearGraphPush(void);

// True when the worker should fire one daily-history query (live thin backfill
// or Trends queue). Cleared when consumed. `outCmd` is R1_CMD_*.
bool g2HealthConsumeDailyRequest(uint8_t* outCmd /* R1_CMD_* */);

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
inline bool g2HealthTrackIsActive(void) { return false; }
inline G2HealthMetric g2HealthSelectedMetric(void) { return HEALTH_METRIC_OVERVIEW; }
inline bool g2HealthWantTextOnlyShape(void) { return true; }
inline bool g2HealthConsumePollRequest(void) { return false; }
inline void g2HealthSetPageActive(bool) {}
inline bool g2HealthPageIsActive(void) { return false; }
inline void g2HealthNotePollStarted(void) {}
inline void g2HealthNotePollFinished(void) {}
inline void g2HealthArmGraphPush(uint32_t) {}
inline bool g2HealthConsumeGraphPush(void) { return false; }
inline void g2HealthClearGraphPush(void) {}
inline bool g2HealthConsumeDailyRequest(uint8_t*) { return false; }
inline void g2HealthNoteSample(G2HealthMetric, int16_t, uint32_t) {}
inline void g2HealthApplyDailyBackfill(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {}
inline void g2HealthApplyTrendDaily(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {}
inline size_t g2HealthHistoryCount(G2HealthMetric) { return 0; }
inline size_t g2RenderHealthBmp(uint8_t*, size_t) { return 0; }
inline void g2HealthBuildOverviewText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildMetricText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildTrendsOverviewText(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2HealthBuildInfo(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }

#endif  // ENABLE_R1_HEALTH
#endif  // G2_HEALTH_H
