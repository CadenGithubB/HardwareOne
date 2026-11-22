// ============================================================================
// OLED R1 Health — vitals, local logging, ring controls, typed history
// ============================================================================
// Hybrid surface: connect stays under Bluetooth → R1 Ring. This mode shows
// live HR/HRV/SpO2/temp/battery/wear from g2RingGetTelemetry, with entry/Poll
// refresh delegated to the exact-profile coordinator. Ring collection/low-power controls and the
// forced history refresh run as admin commands (`ring control`, `ring history force`), so
// the command layer decides and audits; local logging is independent.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH

#include <Adafruit_SSD1306.h>
#include <cstdio>
#include <cstring>
#include <esp_attr.h>

#include "G2_Ring.h"
#include "G2_Health.h"
#include "HAL_Input.h"
#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "System_User.h"
#include "BLE_Server.h"   // bleSubsystemActive

enum HealthAction : uint8_t {
  HEALTH_ACTION_POLL = 0,
  HEALTH_ACTION_LOGGING,
  HEALTH_ACTION_COLLECTION,
  HEALTH_ACTION_HISTORY,
  HEALTH_ACTION_HISTORY_FORCE,
  HEALTH_ACTION_LOW_POWER,
  HEALTH_ACTION_GRAPHS,
  HEALTH_ACTION_COUNT,
};

static uint8_t  sHealthSel        = 0;
EXT_RAM_BSS_ATTR static OledScrollState sHealthActions;
EXT_RAM_BSS_ATTR static char sHealthActionLabels[HEALTH_ACTION_COUNT][32];
static bool     sHealthWasConn    = false;
enum HealthView : uint8_t { HEALTH_OVERVIEW, HEALTH_GRAPHS, HEALTH_GRAPH };
static HealthView sHealthView = HEALTH_OVERVIEW;
struct HealthGraphChoice { const char* label; G2HealthMetric metric; bool daily; };
static const HealthGraphChoice kHealthGraphs[] = {
  {"Heart rate", HEALTH_METRIC_HR, false},
  {"HRV", HEALTH_METRIC_HRV, false},
  {"SpO2", HEALTH_METRIC_SPO2, false},
  {"Temperature", HEALTH_METRIC_TEMP, false},
  {"Battery", HEALTH_METRIC_BATTERY, false},
  {"Daily: Heart rate", HEALTH_METRIC_HR, true},
  {"Daily: HRV", HEALTH_METRIC_HRV, true},
  {"Daily: SpO2", HEALTH_METRIC_SPO2, true},
};
static constexpr int kHealthGraphCount = sizeof(kHealthGraphs) / sizeof(kHealthGraphs[0]);
EXT_RAM_BSS_ATTR static OledScrollState sHealthGraphs;
EXT_RAM_BSS_ATTR static G2HealthGraphSnapshot sHealthGraphSnapshot;  // OLED owner; no borrowed data

static void healthOpenGraphs() {
  sHealthView = HEALTH_GRAPHS;
  oledScrollInit(&sHealthGraphs, nullptr, oledVisibleRows(8));
  for (const auto& graph : kHealthGraphs) oledScrollAddItem(&sHealthGraphs, graph.label);
}

static bool healthHandleGraphInput(uint32_t newlyPressed) {
  if (sHealthView == HEALTH_OVERVIEW) return false;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    sHealthView = sHealthView == HEALTH_GRAPH ? HEALTH_GRAPHS : HEALTH_OVERVIEW;
    oledMarkDirty();
    return true;
  }
  if (sHealthView == HEALTH_GRAPHS) {
    if (oledScrollHandleNav(&sHealthGraphs)) return true;
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      sHealthView = HEALTH_GRAPH;
      oledMarkDirty();
      return true;
    }
  } else {
    if (gNavEvents.left || gNavEvents.up) {
      sHealthGraphs.selectedIndex = (sHealthGraphs.selectedIndex + kHealthGraphCount - 1) % kHealthGraphCount;
      oledMarkDirty(); return true;
    }
    if (gNavEvents.right || gNavEvents.down) {
      sHealthGraphs.selectedIndex = (sHealthGraphs.selectedIndex + 1) % kHealthGraphCount;
      oledMarkDirty(); return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      if (!oledGuestBlocksMutate()) (void)healthStartPollBurst();
      return true;
    }
  }
  // Graph/list input belongs to this subview, not the overview actions.
  return true;
}

static void healthGraphValue(char* out, size_t cap, int value, G2HealthMetric metric) {
  if (metric == HEALTH_METRIC_TEMP) {
    const int magnitude = value < 0 ? -value : value;
    snprintf(out, cap, "%s%d.%d", value < 0 ? "-" : "", magnitude / 10, magnitude % 10);
  } else snprintf(out, cap, "%d", value);
}

static void displayHealthGraph() {
  oledScrollClampSelection(&sHealthGraphs);
  const auto& choice = kHealthGraphs[sHealthGraphs.selectedIndex];
  const int top = OLED_CONTENT_START_Y;
  oledDisplay->setCursor(0, top);
  oledDisplay->print(choice.label);
  if (!g2HealthGraphSnapshot(choice.metric, choice.daily, sHealthGraphSnapshot)) {
    oledDisplay->setCursor(0, top + 12); oledDisplay->print("Graph unavailable");
    return;
  }
  const auto& data = sHealthGraphSnapshot;
  oledDisplay->setCursor(0, top + 8);
  if (!data.count) {
    oledDisplay->print(g2RingIsConnected() ? "No samples. X: Poll" : "No samples. Ring offline");
    return;
  }
  int lo = data.points[0].value, hi = lo;
  for (size_t i = 1; i < data.count; ++i) {
    if (data.points[i].value < lo) lo = data.points[i].value;
    if (data.points[i].value > hi) hi = data.points[i].value;
  }
  if (lo == hi) { --lo; ++hi; }
  char value[16];
  healthGraphValue(value, sizeof(value), data.points[data.count-1].value, choice.metric);
  oledDisplay->print(value);
  oledDisplay->print(choice.metric == HEALTH_METRIC_HR ? " bpm" :
                     choice.metric == HEALTH_METRIC_HRV ? " ms" :
                     choice.metric == HEALTH_METRIC_TEMP ? " C" : "%");
  oledDisplay->print(g2RingIsConnected() ? "" : " offline");
  const int x0 = 25, y0 = top + 16;
  const int width = SCREEN_WIDTH - x0 - 3;
  const int height = OLED_CONTENT_HEIGHT - 24;
  if (width < 4 || height < 4) return;
  oledDisplay->drawLine(x0, y0, x0, y0 + height - 1, DISPLAY_FG);
  oledDisplay->drawLine(x0, y0 + height - 1, x0 + width - 1, y0 + height - 1, DISPLAY_FG);
  healthGraphValue(value, sizeof(value), hi, choice.metric);
  oledDisplay->setCursor(0, y0); oledDisplay->print(value);
  healthGraphValue(value, sizeof(value), lo, choice.metric);
  oledDisplay->setCursor(0, y0 + height - 8); oledDisplay->print(value);
  const uint32_t oldestAge = data.points[0].ageMs;
  const uint32_t newestAge = data.points[data.count-1].ageMs;
  const bool dailyTime = choice.daily && data.endEpoch > data.startEpoch;
  const uint32_t span = dailyTime ? data.endEpoch - data.startEpoch : oldestAge - newestAge;
  int prevX = 0, prevY = 0;
  for (size_t i = 0; i < data.count; ++i) {
    uint32_t position = dailyTime ? (data.points[i].epoch >= data.startEpoch ? data.points[i].epoch - data.startEpoch : 0)
                                  : oldestAge - data.points[i].ageMs;
    if (position > span) position = span;
    const int x = x0 + (span ? static_cast<uint64_t>(position) * (width-1) / span : (width-1)/2);
    const int y = y0 + height - 1 - (data.points[i].value - lo) * (height-1) / (hi-lo);
    if (i) oledDisplay->drawLine(prevX, prevY, x, y, DISPLAY_FG);
    oledDisplay->drawPixel(x, y, DISPLAY_FG);
    prevX = x; prevY = y;
  }
  oledDisplay->setCursor(x0, top + OLED_CONTENT_HEIGHT - 8);
  if (choice.daily) oledDisplay->print("Daily history");
  else if (data.backfilled) oledDisplay->print("Samples + history");
  else {
    char range[24];
    snprintf(range, sizeof(range), "%lum-%lum ago", (unsigned long)(oldestAge/60000), (unsigned long)(newestAge/60000));
    oledDisplay->print(range);
  }
}

// Preserve -> On -> Off -> Preserve: the order A cycles a ring control.
static const char* healthDesiredLabel(G2RingDesiredState value) {
  return value == G2_RING_PRESERVE ? "keep" : g2RingDesiredStateName(value);
}
static const char* healthObservedLabel(G2RingObservedState value) {
  return value == G2_RING_OBS_UNKNOWN ? "?" : g2RingObservedStateName(value);
}

static const char* healthNextDesiredToken(G2RingDesiredState current) {
  return current == G2_RING_PRESERVE ? "on"
       : current == G2_RING_ON       ? "off"
       : "preserve";
}

// The row's command runs in the background; the owner's explanation (or what
// it did) is the toast when it ends.
static void healthRunCommand(const char* line) { executeOledCommand(line); }

static void healthOnEnter(bool isForward) {
  if (!isForward) return;
  sHealthSel        = 0;
  sHealthView = HEALTH_OVERVIEW;
  oledScrollInit(&sHealthActions, nullptr, oledVisibleRows(8, 32));
  sHealthWasConn    = g2RingIsConnected();
  if (sHealthWasConn && g2RingHealthPageRefreshSupported()) {
    (void)healthStartPollBurst();
  }
}

static void healthKickPollBurst() {
  (void)healthStartPollBurst();
}

static void fmtAgeShort(char* out, size_t cap, int32_t ageSec) {
  if (!out || !cap) return;
  out[0] = '\0';
  if (ageSec < 0) return;
  if (ageSec < 5) snprintf(out, cap, "now");
  else if (ageSec < 60) snprintf(out, cap, "%lds", (long)(((ageSec / 5) * 5)));
  else if (ageSec < 3600) snprintf(out, cap, "%ldm", (long)(ageSec / 60));
  else snprintf(out, cap, "%ldh", (long)(ageSec / 3600));
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

static void displayR1Health() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);

  if (sHealthView == HEALTH_GRAPHS) {
    oledScrollRenderSimple(oledDisplay, &sHealthGraphs);
    return;
  }
  if (sHealthView == HEALTH_GRAPH) {
    displayHealthGraph();
    oledMarkDirty();
    return;
  }
  const bool conn = g2RingIsConnected();
  if (conn && !sHealthWasConn &&
      g2RingHealthPageRefreshSupported()) {
    (void)healthStartPollBurst();
  }
  sHealthWasConn = conn;

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print("R1 HEALTH ");
  if (!bleSubsystemActive()) oledDisplay->println("[BLE]");
  else if (conn)             oledDisplay->println("[OK]");
  else                       oledDisplay->println("[--]");

  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  char v[20], age[8];
  fmtAgeShort(age, sizeof(age), freshestVitalAge(t));
  const int r1 = OLED_CONTENT_START_Y + 8;
  const int r2 = OLED_CONTENT_START_Y + 16;
  const int r3 = OLED_CONTENT_START_Y + 24;

  oledDisplay->setCursor(0, r1);
  if (t.hrValid) {
    snprintf(v, sizeof(v), "HR %u", (unsigned)t.hr);
    oledDisplay->print(v);
  } else {
    oledDisplay->print("HR --");
  }
  oledDisplay->setCursor(64, r1);
  if (t.hrvValid) {
    snprintf(v, sizeof(v), "V %d", (int)t.hrv);
    oledDisplay->print(v);
  } else {
    oledDisplay->print("V --");
  }

  oledDisplay->setCursor(0, r2);
  if (t.spo2Valid) {
    snprintf(v, sizeof(v), "O2 %u%%", (unsigned)t.spo2);
    oledDisplay->print(v);
  } else {
    oledDisplay->print("O2 --");
  }
  oledDisplay->setCursor(64, r2);
  if (t.batteryValid) {
    snprintf(v, sizeof(v), "B %u%%", (unsigned)t.battery);
    oledDisplay->print(v);
  } else {
    oledDisplay->print("B --");
  }

  oledDisplay->setCursor(0, r3);
  if (t.tempValid) {
    const int whole = t.tempTenths / 10;
    const int frac  = t.tempTenths < 0 ? -(t.tempTenths % 10) : (t.tempTenths % 10);
    snprintf(v, sizeof(v), "T %d.%dC", whole, frac);
    oledDisplay->print(v);
  } else {
    oledDisplay->print("T --");
  }
  oledDisplay->setCursor(64, r3);
  // Wear + one shared recentness (not per-vital ages).
  const char* wear = g2RingWearName(t.wearValid, t.wear);
  if (age[0]) snprintf(v, sizeof(v), "%s %s", wear, age);
  else        snprintf(v, sizeof(v), "Wear %s", wear);
  oledDisplay->print(v);

  G2RingControlStatus control = {};
  g2RingGetControlStatus(control);
  G2HealthHistorySummary history = {};
  g2HealthHistoryGetSummary(history);

  const int a0 = OLED_CONTENT_START_Y + 32;
  sHealthActions.visibleLines = oledVisibleRows(8, 32);
  oledScrollClearKeepSelection(&sHealthActions);
  sHealthActions.selectedIndex = sHealthSel;
  for (uint8_t action = 0; action < HEALTH_ACTION_COUNT; ++action) {
    char* label = sHealthActionLabels[action];
    const size_t cap = sizeof(sHealthActionLabels[action]);
    switch (action) {
      case HEALTH_ACTION_POLL:
        snprintf(label, cap, "%s%s", conn && !g2RingHealthPageRefreshSupported() ? "Poll unsupported" : "Poll Now",
                 healthPollBurstActive() ? "..." : ""); break;
      case HEALTH_ACTION_LOGGING:
        snprintf(label, cap, "Logging %s", healthLoggingIsActive() ? "ON" : (gSettings.healthLoggingEnabled ? "armed" : "off")); break;
      case HEALTH_ACTION_COLLECTION:
        snprintf(label, cap, "Collect %s/%s%s", healthDesiredLabel(control.healthDesired),
                 healthObservedLabel(control.healthObserved), control.healthPending ? "..." : control.healthLastError != G2_RING_ERR_NONE ? " !" : ""); break;
      case HEALTH_ACTION_HISTORY:
        snprintf(label, cap, "History %s", r1HealthHistoryFetchStateName(history.fetchState)); break;
      case HEALTH_ACTION_HISTORY_FORCE: snprintf(label, cap, "Force History"); break;
      case HEALTH_ACTION_LOW_POWER:
        snprintf(label, cap, "LowP %s/%s%s", healthDesiredLabel(control.lowPowerDesired),
                 healthObservedLabel(control.lowPowerObserved), control.lowPowerPending ? "..." : control.lowPowerLastError != G2_RING_ERR_NONE ? " !" : ""); break;
      default: snprintf(label, cap, "Graphs"); break;
    }
    oledScrollAddItem(&sHealthActions, label);
  }
  oledScrollRenderSimple(oledDisplay, &sHealthActions, true, a0);

  // Telemetry arrives via BLE notify — keep redrawing while on this page.
  oledMarkDirty();
}

static bool healthInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (healthHandleGraphInput(newlyPressed)) return true;
  if (gNavEvents.up   && sHealthSel > 0) { sHealthSel--; return true; }
  if (gNavEvents.down && sHealthSel + 1 < HEALTH_ACTION_COUNT) { sHealthSel++; return true; }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (sHealthSel != HEALTH_ACTION_GRAPHS && oledGuestBlocksMutate()) return true;
    switch (sHealthSel) {
      case HEALTH_ACTION_GRAPHS:
        healthOpenGraphs();
        break;
      case HEALTH_ACTION_POLL:
        healthKickPollBurst();
        break;
      case HEALTH_ACTION_LOGGING:
        healthRunCommand("healthlogging toggle");
        break;
      case HEALTH_ACTION_COLLECTION: {
        G2RingControlStatus status = {};
        g2RingGetControlStatus(status);
        char line[40];
        snprintf(line, sizeof(line), "ring control collection %s",
                 healthNextDesiredToken(status.healthDesired));
        healthRunCommand(line);
        break;
      }
      case HEALTH_ACTION_HISTORY:
        (void)g2RingRequestHistoryRefresh(false);
        break;
      case HEALTH_ACTION_HISTORY_FORCE:
        healthRunCommand("ring history force");
        break;
      case HEALTH_ACTION_LOW_POWER: {
        G2RingControlStatus status = {};
        g2RingGetControlStatus(status);
        char line[40];
        snprintf(line, sizeof(line), "ring control lowpower %s",
                 healthNextDesiredToken(status.lowPowerDesired));
        healthRunCommand(line);
        break;
      }
      default:
        break;
    }
    return true;
  }
  return false;  // B: global pop
}

static const OledModeEntry r1HealthOledModes[] = {
  { OLED_R1_HEALTH, "R1 Health", "bt_idle", displayR1Health,
    nullptr, healthInputHandler, true, 46, "A:Select B:Back", healthOnEnter },
};

REGISTER_OLED_MODE_MODULE(r1HealthOledModes, sizeof(r1HealthOledModes) / sizeof(r1HealthOledModes[0]), "R1 Health");

void oledR1HealthModeInit() {}

#endif  // ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH
