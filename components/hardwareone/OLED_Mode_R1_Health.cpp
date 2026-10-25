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
static bool     sHealthWasConn    = false;
static uint32_t sHealthDeniedUntilMs = 0;

// Preserve -> On -> Off -> Preserve: the order A cycles a ring control.
static const char* healthNextDesiredToken(G2RingDesiredState current) {
  return current == G2_RING_PRESERVE ? "on"
       : current == G2_RING_ON       ? "off"
       : "preserve";
}

// Runs a ring control command as the display's identity. The command owns the
// admin rule and the executor audits it; a refusal (role, or the ring not
// ready) flashes [DENIED] in the header.
static void healthRunCommand(const char* line) {
  char out[96];
  if (executeOLEDCommandWithResult(line, out, sizeof(out)) &&
      strncmp(out, "Error", 5) != 0) {
    return;
  }
  sHealthDeniedUntilMs = millis() + 1500;
  oledMarkDirty();
}

static void healthOnEnter(bool isForward) {
  if (!isForward) return;
  sHealthSel        = 0;
  sHealthActionTop  = 0;
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

  const bool conn = g2RingIsConnected();
  if (conn && !sHealthWasConn &&
      g2RingHealthPageRefreshSupported()) {
    (void)healthStartPollBurst();
  }
  sHealthWasConn = conn;

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print("R1 HEALTH ");
  if ((int32_t)(sHealthDeniedUntilMs - millis()) > 0)
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
  const char* wear = g2RingWearName(t.wearValid, t.wear);
  if (age[0]) snprintf(v, sizeof(v), "%s %s", wear, age);
  else        snprintf(v, sizeof(v), "Wear %s", wear);
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
    if (action == HEALTH_ACTION_POLL) {
      if (conn && !g2RingHealthPageRefreshSupported()) {
        oledDisplay->print("Poll unsupported");
      } else {
        oledDisplay->print("Poll Now");
        if (healthPollBurstActive()) oledDisplay->print("...");
      }
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
        executeOLEDCommand("healthlogging toggle");
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

static const OLEDModeEntry r1HealthOLEDModes[] = {
  { OLED_R1_HEALTH, "R1 Health", "bt_idle", displayR1Health,
    nullptr, healthInputHandler, true, 46, "A:Select B:Back", healthOnEnter },
};

REGISTER_OLED_MODE_MODULE(r1HealthOLEDModes, sizeof(r1HealthOLEDModes) / sizeof(r1HealthOLEDModes[0]), "R1 Health");

void oledR1HealthModeInit() {}

#endif  // ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH
