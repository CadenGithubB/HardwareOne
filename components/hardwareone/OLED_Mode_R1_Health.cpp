// ============================================================================
// OLED R1 Health — vitals, local logging, ring controls, typed history
// ============================================================================
// Hybrid surface: connect stays under Bluetooth → R1 Ring. This mode shows
// live HR/HRV/SpO2/temp/battery/wear from g2RingGetTelemetry, entry/Poll
// bursts via g2RingPollVital. Ring collection/low-power controls use the
// transport's async desired/observed contract; local logging is independent.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH

#include <Adafruit_SSD1306.h>
#include <cstdio>

#include "G2_Ring.h"
#include "G2_Health.h"
#include "HAL_Input.h"
#include "OLED_Utils.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "System_User.h"
#include "Bluetooth.h"   // bleSubsystemActive

enum HealthAction : uint8_t {
  HEALTH_ACTION_POLL = 0,
  HEALTH_ACTION_LOGGING,
  HEALTH_ACTION_COLLECTION,
  HEALTH_ACTION_HISTORY,
  HEALTH_ACTION_HISTORY_FORCE,
  HEALTH_ACTION_LOW_POWER,
  HEALTH_ACTION_COUNT,
};

static uint8_t  sHealthSel        = 0;
static uint8_t  sHealthActionTop  = 0;
static uint8_t  sHealthPollCursor = G2_RING_POLL_VITAL_COUNT;  // idle when == COUNT
static bool     sHealthWasConn    = false;
static uint32_t sHealthLastPollMs = 0;
static uint32_t sHealthAdminDeniedUntilMs = 0;

static bool healthAdminAllowed() {
  return gLocalDisplayAuthed && isAdminUser(gLocalDisplayUser);
}

static bool healthActionRequiresAdmin(uint8_t action) {
  return action == HEALTH_ACTION_COLLECTION ||
         action == HEALTH_ACTION_HISTORY_FORCE ||
         action == HEALTH_ACTION_LOW_POWER;
}

static bool healthRequireAdmin() {
  if (healthAdminAllowed()) return true;
  sHealthAdminDeniedUntilMs = millis() + 1500;
  oledMarkDirty();
  return false;
}

static void healthOnEnter(bool isForward) {
  if (!isForward) return;
  sHealthSel        = 0;
  sHealthActionTop  = 0;
  sHealthPollCursor = g2RingIsConnected() ? 0 : G2_RING_POLL_VITAL_COUNT;
  sHealthWasConn    = g2RingIsConnected();
  sHealthLastPollMs = 0;
}

static void healthKickPollBurst() {
  if (!g2RingIsConnected()) return;
  sHealthPollCursor = 0;
  sHealthLastPollMs = 0;
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

  const bool conn = g2RingIsConnected();
  if (conn && !sHealthWasConn) sHealthPollCursor = 0;
  sHealthWasConn = conn;
  if (conn && sHealthPollCursor < G2_RING_POLL_VITAL_COUNT) {
    const uint32_t now = millis();
    if (sHealthLastPollMs == 0 || now - sHealthLastPollMs >= 800) {
      g2RingPollVital(sHealthPollCursor++);
      sHealthLastPollMs = now;
      if (sHealthPollCursor >= G2_RING_POLL_VITAL_COUNT) {
        healthLoggingNotePageRefresh();
      }
    }
  }

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print("R1 HEALTH ");
  if ((int32_t)(sHealthAdminDeniedUntilMs - millis()) > 0)
                               oledDisplay->println("[DENIED]");
  else if (!bleSubsystemActive()) oledDisplay->println("[BLE]");
  else if (conn)             oledDisplay->println("[OK]");
  else                       oledDisplay->println("[--]");

  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  char v[20], age[8];
  fmtAgeShort(age, sizeof(age), freshestVitalAge(t));
  const int r1 = OLED_CONTENT_START_Y + 9;
  const int r2 = OLED_CONTENT_START_Y + 18;
  const int r3 = OLED_CONTENT_START_Y + 27;

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
  if (!t.wearValid) {
    if (age[0]) snprintf(v, sizeof(v), "-- %s", age);
    else        snprintf(v, sizeof(v), "Wear --");
  } else if (t.wear == 2) {
    if (age[0]) snprintf(v, sizeof(v), "on %s", age);
    else        snprintf(v, sizeof(v), "Wear on");
  } else if (t.wear == 1) {
    if (age[0]) snprintf(v, sizeof(v), "off %s", age);
    else        snprintf(v, sizeof(v), "Wear off");
  } else {
    if (age[0]) snprintf(v, sizeof(v), "? %s", age);
    else        snprintf(v, sizeof(v), "Wear ?");
  }
  oledDisplay->print(v);

  G2RingControlStatus control = {};
  g2RingGetControlStatus(control);
  G2HealthHistorySummary history = {};
  g2HealthHistoryGetSummary(history);

  const int a0 = OLED_CONTENT_START_Y + 38;
  constexpr uint8_t kVisibleActions = 2;
  if (sHealthSel < sHealthActionTop) sHealthActionTop = sHealthSel;
  if (sHealthSel >= sHealthActionTop + kVisibleActions)
    sHealthActionTop = sHealthSel - kVisibleActions + 1;
  for (uint8_t row = 0; row < kVisibleActions; ++row) {
    const uint8_t action = sHealthActionTop + row;
    if (action >= HEALTH_ACTION_COUNT) break;
    oledDisplay->setCursor(0, a0 + row * 8);
    oledDisplay->print(action == sHealthSel ? "> " : "  ");
    if (healthActionRequiresAdmin(action) && !healthAdminAllowed()) {
      if (action == HEALTH_ACTION_COLLECTION) oledDisplay->print("Collect [admin]");
      else if (action == HEALTH_ACTION_HISTORY_FORCE) oledDisplay->print("Force Hist [admin]");
      else oledDisplay->print("LowP [admin]");
    } else if (action == HEALTH_ACTION_POLL) {
      oledDisplay->print("Poll Now");
      if (sHealthPollCursor < G2_RING_POLL_VITAL_COUNT) oledDisplay->print("...");
    } else if (action == HEALTH_ACTION_LOGGING) {
      oledDisplay->print("Logging ");
      oledDisplay->print(healthLoggingIsActive() ? "ON" :
                         (gSettings.healthLoggingEnabled ? "armed" : "off"));
    } else if (action == HEALTH_ACTION_COLLECTION) {
      oledDisplay->print("Collect ");
      oledDisplay->print(g2RingDesiredStateName(control.healthDesired));
      oledDisplay->print("/");
      oledDisplay->print(g2RingObservedStateName(control.healthObserved));
      if (control.healthPending) oledDisplay->print("...");
      else if (control.healthLastError != G2_RING_ERR_NONE) oledDisplay->print(" !");
    } else if (action == HEALTH_ACTION_HISTORY) {
      oledDisplay->print("History ");
      oledDisplay->print(r1HealthHistoryFetchStateName(history.fetchState));
    } else if (action == HEALTH_ACTION_HISTORY_FORCE) {
      oledDisplay->print("Force History");
    } else if (action == HEALTH_ACTION_LOW_POWER) {
      oledDisplay->print("LowP ");
      oledDisplay->print(g2RingDesiredStateName(control.lowPowerDesired));
      oledDisplay->print("/");
      oledDisplay->print(g2RingObservedStateName(control.lowPowerObserved));
      if (control.lowPowerPending) oledDisplay->print("...");
      else if (control.lowPowerLastError != G2_RING_ERR_NONE) oledDisplay->print(" !");
    }
  }

  // Telemetry arrives via BLE notify — keep redrawing while on this page.
  oledMarkDirty();
}

static bool healthInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (gNavEvents.up   && sHealthSel > 0) { sHealthSel--; return true; }
  if (gNavEvents.down && sHealthSel + 1 < HEALTH_ACTION_COUNT) { sHealthSel++; return true; }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sHealthSel) {
      case HEALTH_ACTION_POLL:
        healthKickPollBurst();
        break;
      case HEALTH_ACTION_LOGGING:
        executeOLEDCommand(gSettings.healthLoggingEnabled
                           ? "healthlogging off"
                           : "healthlogging on");
        break;
      case HEALTH_ACTION_COLLECTION: {
        if (!healthRequireAdmin()) break;
        G2RingControlStatus status = {};
        g2RingGetControlStatus(status);
        const G2RingDesiredState next = status.healthDesired == G2_RING_PRESERVE
            ? G2_RING_ON : (status.healthDesired == G2_RING_ON
                ? G2_RING_OFF : G2_RING_PRESERVE);
        (void)g2RingSetHealthCollectionDesired(next);
        break;
      }
      case HEALTH_ACTION_HISTORY:
        (void)g2RingRequestHistoryRefresh(false);
        break;
      case HEALTH_ACTION_HISTORY_FORCE:
        if (!healthRequireAdmin()) break;
        (void)g2RingRequestHistoryRefresh(true);
        break;
      case HEALTH_ACTION_LOW_POWER: {
        if (!healthRequireAdmin()) break;
        G2RingControlStatus status = {};
        g2RingGetControlStatus(status);
        const G2RingDesiredState next = status.lowPowerDesired == G2_RING_PRESERVE
            ? G2_RING_ON : (status.lowPowerDesired == G2_RING_ON
                ? G2_RING_OFF : G2_RING_PRESERVE);
        (void)g2RingSetLowPowerDesired(next);
        break;
      }
      default:
        break;
    }
    return true;
  }
  return false;  // B: global pop
}

static const OLEDModeEntry r1HealthOLEDModes[] = {
  { OLED_R1_HEALTH, "R1 Health", "bt_idle", displayR1Health,
    nullptr, healthInputHandler, true, 46, "A:Select B:Back", healthOnEnter },
};

REGISTER_OLED_MODE_MODULE(r1HealthOLEDModes, sizeof(r1HealthOLEDModes) / sizeof(r1HealthOLEDModes[0]), "R1 Health");

void oledR1HealthModeInit() {}

#endif  // ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH
