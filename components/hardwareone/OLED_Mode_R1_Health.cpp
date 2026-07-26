// ============================================================================
// OLED R1 Health — vitals, Poll Now, Health Track
// ============================================================================
// Hybrid surface: connect stays under Bluetooth → R1 Ring. This mode shows
// live HR/HRV/SpO2/temp/battery/wear from g2RingGetTelemetry, entry/Poll
// bursts via g2RingPollVital, and Track on/off via healthtrack.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH

#include <Adafruit_SSD1306.h>
#include <cstdio>

#include "G2_Ring.h"
#include "HAL_Input.h"
#include "OLED_Utils.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "Bluetooth.h"   // bleSubsystemActive

static int      sHealthSel        = 0;  // 0 Poll Now, 1 Track toggle
static uint8_t  sHealthPollCursor = G2_RING_POLL_VITAL_COUNT;  // idle when == COUNT
static bool     sHealthWasConn    = false;
static uint32_t sHealthLastPollMs = 0;

static void healthOnEnter(bool isForward) {
  if (!isForward) return;
  sHealthSel        = 0;
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
        healthTrackNotePageRefresh();
      }
    }
  }

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print("R1 HEALTH ");
  if (!bleSubsystemActive()) oledDisplay->println("[BLE]");
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

  const int a0 = OLED_CONTENT_START_Y + 38;
  for (int i = 0; i < 2; i++) {
    oledDisplay->setCursor(0, a0 + i * 8);
    oledDisplay->print(i == sHealthSel ? "> " : "  ");
    if (i == 0) {
      oledDisplay->print("Poll Now");
      if (sHealthPollCursor < G2_RING_POLL_VITAL_COUNT) oledDisplay->print("...");
    } else {
      oledDisplay->print("Track ");
      oledDisplay->print(healthTrackIsActive() ? "ON" :
                         (gSettings.healthTrackingEnabled ? "arm" : "off"));
    }
  }

  // Telemetry arrives via BLE notify — keep redrawing while on this page.
  oledMarkDirty();
}

static bool healthInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (gNavEvents.up   && sHealthSel > 0) { sHealthSel--; return true; }
  if (gNavEvents.down && sHealthSel < 1) { sHealthSel++; return true; }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (sHealthSel == 0) {
      healthKickPollBurst();
    } else {
      executeOLEDCommand(gSettings.healthTrackingEnabled
                         ? "healthtrack off"
                         : "healthtrack on");
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
