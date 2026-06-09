// System_Battery.cpp — battery subsystem dispatcher
//
// One BatteryState (gBatteryState), three possible backends, picked at compile
// time by the board's BATTERY_BACKEND_* flag in System_BuildConfig.h:
//
//   BATTERY_BACKEND_ADC        → Adafruit Feather V1/V2, XIAO Plus, etc.
//                                Reads VBAT/2 via ADC1 + voltage divider.
//                                Charging is heuristic (voltage delta).
//   BATTERY_BACKEND_FUEL_GAUGE → Unexpected Maker FeatherS3[D].
//                                Reads MAX17048G over I2C. Charging is
//                                derived from the signed CRATE register.
//   neither                    → USB-only stub. gBatteryState seeded with
//                                status=NOT_PRESENT, percentage=100, voltage=5V
//                                so the OLED/G2 widgets render "USB" cleanly.
//
// updateBattery() is called every 10 seconds from HardwareOne.cpp's main
// loop. No dedicated task — see the rationale in i2csensor_max17048.h.

#include "System_Battery.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Notifications.h"
#include "System_Settings.h"   // gSettings (battery-log enable/interval) + setSetting
#include "System_VFS.h"        // guarded LittleFS access for the CSV log
#include "System_AuthIdentity.h" // currentAuthContext() — CLI handler file auth
#include "System_Mutex.h"      // fsLock/fsUnlock
#include <time.h>              // epoch timestamp column

#if BATTERY_BACKEND_ADC
  #include <driver/adc.h>
  #include <esp_adc_cal.h>
#endif

#if BATTERY_BACKEND_FUEL_GAUGE
  #include "i2csensor_max17048.h"
#endif

// Global battery state — public, shared by all consumers.
BatteryState gBatteryState = {
  .voltage = 0.0f,
  .percentage = 0.0f,
  .status = BATTERY_UNKNOWN,
  .isCharging = false,
  .usbPresent = false,
  .lastReadMs = 0,
  .rawADC = 0,
  .cratePctPerHr = 0.0f
};

// USB-presence inference threshold (fallback heuristic only — used on boards
// without BATTERY_VBUS_SENSE_PIN wired). A LiPo cell can't physically hold
// above ~4.15V without an external charge source pushing current into it,
// so vBatt > this threshold infers USB presence at the float plateau when
// CRATE is too quiet to flag charging. Boards with a real VBUS GPIO ignore
// this and read the pin directly — see vbusSensePresent() below.
static constexpr float USB_PRESENT_VOLTAGE_THRESHOLD = 4.15f;

// VBUS sense GPIO — authoritative USB-present signal when wired. The FeatherS3[D]
// routes USB VBUS to GPIO 34 through a divider. The MAX17048's CRATE register
// lags 30-60s on USB plug/unplug; this pin responds in microseconds. When
// BATTERY_VBUS_SENSE_PIN is -1 (board doesn't have it), the fuel-gauge backend
// falls back to the CRATE/voltage heuristic via vbusSensePresent() returning
// the OR of those signals.
#if BATTERY_VBUS_SENSE_PIN >= 0
static void vbusSenseInit() {
  pinMode(BATTERY_VBUS_SENSE_PIN, INPUT);
  // Log the initial pin state so a wrong pin assignment surfaces at boot
  // instead of being inferred from later misbehavior. On the FeatherS3[D]
  // this should read HIGH whenever USB is plugged in (whether or not a
  // host is enumerated) and LOW when only the cell is supplying power.
  const int level = digitalRead(BATTERY_VBUS_SENSE_PIN);
  INFO_SYSTEMF("Battery: VBUS sense GPIO %d initialized, reads %s",
               BATTERY_VBUS_SENSE_PIN, level == HIGH ? "HIGH (USB present)" : "LOW (no USB)");
}
static inline bool vbusSenseRead() {
  return digitalRead(BATTERY_VBUS_SENSE_PIN) == HIGH;
}
static constexpr bool kHasVbusSense = true;
#else
static inline void vbusSenseInit() {}
static inline bool vbusSenseRead() { return false; }
static constexpr bool kHasVbusSense = false;
#endif

// ============================================================================
// Backend: ADC (Feather V1/V2 voltage divider)
// ============================================================================
#if BATTERY_BACKEND_ADC

static esp_adc_cal_characteristics_t* adc_chars = nullptr;

#define BATTERY_SAMPLES 10
static float voltageHistory[BATTERY_SAMPLES] = {0};
static uint8_t voltageIndex = 0;
static bool historyFilled = false;

static void adcBackendInit() {
  vbusSenseInit();
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);

  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
    ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);

  if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
    INFO_SYSTEMF("Battery ADC calibrated using Two Point Value");
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    INFO_SYSTEMF("Battery ADC calibrated using eFuse Vref");
  } else {
    INFO_SYSTEMF("Battery ADC calibrated using Default Vref");
  }
  INFO_SYSTEMF("Battery monitor: ADC backend (pin=%d)", BATTERY_PIN);
}

// Returns voltage in volts. Sets `state.rawADC` for diagnostics. Charging
// flag is derived heuristically from voltage delta — the divider can't tell
// us whether VBUS is connected, only what the cell terminal sits at.
static void adcBackendSample(BatteryState& state) {
  uint32_t adcSum = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) {
    adcSum += adc1_get_raw(BATTERY_ADC_CHANNEL);
    delayMicroseconds(100);
  }
  const uint16_t adcValue = adcSum / samples;
  state.rawADC = adcValue;

  const uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adcValue, adc_chars);
  const float batteryVoltage = (voltage_mv / 1000.0f) * VBAT_DIVIDER;

  voltageHistory[voltageIndex] = batteryVoltage;
  voltageIndex = (voltageIndex + 1) % BATTERY_SAMPLES;
  if (voltageIndex == 0) historyFilled = true;

  float sum = 0;
  const int count = historyFilled ? BATTERY_SAMPLES : voltageIndex;
  for (int i = 0; i < count; i++) sum += voltageHistory[i];
  state.voltage = sum / count;

  // Charging heuristic: cell voltage near top, or rising fast. Imperfect —
  // a board on USB at 4.05V looks the same as one off USB at 4.05V. The
  // fuel-gauge backend solves this via CRATE; for ADC we accept the noise.
  static float lastVoltage = 0;
  if (state.voltage > 4.1f) {
    state.isCharging = true;
  } else if (state.voltage > lastVoltage + 0.05f) {
    state.isCharging = true;
  } else {
    state.isCharging = false;
  }
  lastVoltage = state.voltage;

  // USB inference. When a hardware VBUS sense pin is wired, trust it
  // exclusively — it's deterministic and lag-free. Otherwise fall back to
  // the voltage/charging heuristic: active charging OR float-plateau (cell
  // can't sit above ~4.15V without USB).
  if (kHasVbusSense) {
    state.usbPresent = vbusSenseRead();
    // With a real VBUS signal, isCharging is "USB in AND CRATE going up".
    // The ADC has no CRATE, so approximate: USB in AND voltage below float.
    state.isCharging = state.usbPresent && (state.voltage < USB_PRESENT_VOLTAGE_THRESHOLD);
  } else {
    state.usbPresent = state.isCharging || (state.voltage > USB_PRESENT_VOLTAGE_THRESHOLD);
  }

  state.cratePctPerHr = 0.0f;  // not derived from ADC
}

#endif  // BATTERY_BACKEND_ADC

// ============================================================================
// Backend: Fuel gauge (MAX17048G over I2C)
// ============================================================================
#if BATTERY_BACKEND_FUEL_GAUGE

static bool fuelGaugePresent = false;

static void fuelGaugeBackendInit() {
  // Don't probe here — initBattery() runs BEFORE initI2CBuses() in the boot
  // sequence, so I2C isn't ready yet. The first updateBattery() call from
  // the main loop (10s after boot) will lazy-probe via the sample path.
  fuelGaugePresent = false;
  vbusSenseInit();
  INFO_SYSTEMF("Battery monitor: MAX17048 fuel gauge backend (probe deferred to first tick)");
}

// Charging detection threshold for CRATE (%/hr). The MAX17048's quiescent
// noise floor is around ±0.2 %/hr; anything beyond ±0.5 is real motion of
// charge. Picked conservatively so we don't flip BATTERY_CHARGING on/off
// every read when the cell is sitting at the float-charge plateau.
static constexpr float CHARGING_CRATE_THRESHOLD_PCT_HR = 0.5f;

// Below this cell voltage, treat the gauge as "no cell installed" — the
// MAX17048 powers off VDD (the 3.3V rail) and will report a wandering low
// voltage with no actual battery wired to its B+ pin.
static constexpr float NO_CELL_VOLTAGE_THRESHOLD = 2.5f;

static void fuelGaugeBackendSample(BatteryState& state) {
  state.rawADC = 0;

  // If the chip wasn't detected at boot, re-probe once per call — handles
  // the case where I2C came up after initBattery() (rare but possible if
  // boot ordering changes).
  if (!fuelGaugePresent) {
    fuelGaugePresent = fuelGaugeProbe();
    if (!fuelGaugePresent) {
      // Stay in NOT_PRESENT — don't blast voltage/percentage to stale data.
      // No cell detected → the device must be running on USB to even be
      // executing this code, so usbPresent is unambiguously true (and the
      // VBUS GPIO will confirm it when wired).
      state.voltage = 0.0f;
      state.percentage = 0.0f;
      state.cratePctPerHr = 0.0f;
      state.isCharging = false;
      state.usbPresent = kHasVbusSense ? vbusSenseRead() : true;
      return;
    }
  }

  FuelGaugeReading r;
  if (!fuelGaugeRead(&r)) {
    DEBUG_SYSTEMF("[BATT] fuelGaugeRead failed; keeping previous state");
    return;
  }

  state.voltage       = r.voltage;
  state.percentage    = r.socPct;
  state.cratePctPerHr = r.cratePctPerHr;

  // Charging classification:
  //   CRATE > +threshold   → charging (cell taking on charge)
  //   CRATE < -threshold   → discharging
  //   |CRATE| ≤ threshold  → keep previous classification (avoids flapping
  //                          at the float-charge plateau where CRATE ≈ 0
  //                          but USB is still connected)
  // USB-present detection. With a hardware VBUS sense pin (FeatherS3[D] →
  // GPIO 34) the answer is deterministic and lag-free; without it we fall
  // back to the CRATE/voltage heuristic.
  //
  // Why VBUS-sense matters: the MAX17048's CRATE register is a heavily-
  // smoothed estimate of %/hr that lags 30-60s behind reality. When the
  // user unplugs USB, CRATE stays positive for almost a minute before
  // flipping sign, so the OLED would keep displaying "USB+ 82%" long
  // after the cable is gone. The GPIO responds in microseconds.
  if (kHasVbusSense) {
    state.usbPresent = vbusSenseRead();
    // isCharging is "USB is in AND cell is taking on charge". With VBUS as
    // an authoritative signal, isCharging only flips when BOTH are true:
    // USB connected, AND CRATE clearly positive. Float-plateau case
    // (USB in, CRATE ≈ 0) correctly reports isCharging=false → "USB" not "USB+".
    if (!state.usbPresent) {
      state.isCharging = false;  // USB unplugged → cannot be charging
    } else if (r.cratePctPerHr >  CHARGING_CRATE_THRESHOLD_PCT_HR) {
      state.isCharging = true;
    } else if (r.cratePctPerHr < -CHARGING_CRATE_THRESHOLD_PCT_HR) {
      state.isCharging = false;
    }
    // else (USB in, CRATE in deadband): leave isCharging unchanged.
  } else {
    if (r.cratePctPerHr >  CHARGING_CRATE_THRESHOLD_PCT_HR) {
      state.isCharging = true;
    } else if (r.cratePctPerHr < -CHARGING_CRATE_THRESHOLD_PCT_HR) {
      state.isCharging = false;
    }
    // Fallback heuristic: active charge OR float-plateau voltage.
    state.usbPresent = (r.cratePctPerHr > CHARGING_CRATE_THRESHOLD_PCT_HR)
                    || (r.voltage > USB_PRESENT_VOLTAGE_THRESHOLD);
  }

  // "Topped off" clamp. The MAX17048's ModelGauge assumes a 4.20V full-charge
  // point, but this board's charge IC terminates / floats the cell around
  // ~4.17V — so the gauge's SOC plateaus a couple percent short and never
  // reads 100%. When USB is present AND charging has terminated (not actively
  // charging) AND the cell is sitting in the topped-off band, the pack is full
  // for all practical purposes — report 100% so the UI matches the existing
  // "USB (full)" state instead of sticking at 98%. Discharge and active-charge
  // readings still come straight from the gauge's coulomb-counted SOC.
  if (state.usbPresent && !state.isCharging && state.voltage >= VBAT_BAND_FULL) {
    state.percentage = 100.0f;
  }
}

#endif  // BATTERY_BACKEND_FUEL_GAUGE

// ============================================================================
// Backend-agnostic status classification + notification
// ============================================================================
// Maps the populated BatteryState fields to a status enum and fires
// notifications on state transitions. Pure function of the inputs — no I/O.
//
// Classification (voltage-based, so it works with ANY LiPo regardless of
// capacity — no fuel-gauge SOC required):
//   1. voltage < 2V → NOT_PRESENT (no cell wired)
//   2. usbPresent   → CHARGING, ALWAYS. On the charger the cell sits near 4.2V
//                     even at low SOC (CV phase), so voltage can't tell "topped
//                     off" from "still filling" — so we just say "Charging"
//                     until it's unplugged.
//   3. off charger  → voltage ladder: FULL >= 4.15, then HIGH / GOOD / MEDIUM /
//                     LOW / CRITICAL / EMPTY (see VBAT_BAND_* in the header).
//
// usbPresent is the authoritative power gate (VBUS-sense GPIO, lag-free);
// isCharging stays a display sub-state ("USB+" vs "USB"), not a status driver.
static void classifyAndNotify(BatteryState& state) {
  const BatteryStatus prevStatus = state.status;
  // Under the rule below, "on charger" maps 1:1 to BATTERY_CHARGING.
  const bool wasOnCharger = (prevStatus == BATTERY_CHARGING);

  if (state.voltage < 2.0f) {
    state.status = BATTERY_NOT_PRESENT;
  } else if (state.usbPresent) {
    state.status = BATTERY_CHARGING;                              // on charger → always Charging
  } else if (state.voltage >= VBAT_BAND_FULL) {
    state.status = BATTERY_FULL;                                  // off charger, topped off
  } else if (state.voltage >= VBAT_BAND_HIGH) {
    state.status = BATTERY_HIGH;
  } else if (state.voltage >= VBAT_BAND_GOOD) {
    state.status = BATTERY_GOOD;
  } else if (state.voltage >= VBAT_BAND_MEDIUM) {
    state.status = BATTERY_MEDIUM;
  } else if (state.voltage >= VBAT_BAND_LOW) {
    state.status = BATTERY_LOW;
  } else if (state.voltage >= VBAT_BAND_CRITICAL) {
    state.status = BATTERY_CRITICAL;
  } else {
    state.status = BATTERY_EMPTY;
  }

  // Notifications fire only on transitions, and never on the first read
  // (prevStatus == UNKNOWN) — avoids spamming "USB connected" at boot.
  if (prevStatus != BATTERY_UNKNOWN) {
    const bool nowOnCharger = (state.status == BATTERY_CHARGING);
    if (nowOnCharger && !wasOnCharger) {
      notifyPowerUSBConnected();
    } else if (!nowOnCharger && wasOnCharger) {
      notifyPowerUSBDisconnected();
    }
    if (state.status == BATTERY_LOW && prevStatus != BATTERY_LOW) {
      notifyBatteryLow((int)state.percentage);
    }
    if ((state.status == BATTERY_CRITICAL || state.status == BATTERY_EMPTY) &&
        prevStatus != BATTERY_CRITICAL && prevStatus != BATTERY_EMPTY) {
      notifyBatteryCritical((int)state.percentage);
    }
  }
}

// ============================================================================
// Public lifecycle — dispatch to selected backend
// ============================================================================

void initBattery() {
#if !ENABLE_BATTERY_MONITOR
  // Subsystem disabled (USB-only build). Seed a stable "no cell" state.
  // usbPresent=true because the device is running, and the only way it can
  // be running in a USB-only build is from VBUS.
  gBatteryState.voltage = 5.0f;
  gBatteryState.percentage = 100.0f;
  gBatteryState.status = BATTERY_NOT_PRESENT;
  gBatteryState.isCharging = false;
  gBatteryState.usbPresent = true;
  INFO_SYSTEMF("Battery monitoring disabled (USB power assumed)");
  return;
#else
  #if BATTERY_BACKEND_ADC
    adcBackendInit();
    updateBattery();
  #elif BATTERY_BACKEND_FUEL_GAUGE
    // Defer initial read — I2C bus may not be up yet at initBattery() time.
    // The probe inside updateBattery() handles this lazily on first tick.
    fuelGaugeBackendInit();
  #else
    // Subsystem enabled but board claims no backend. Treat as USB-only.
    gBatteryState.voltage = 5.0f;
    gBatteryState.percentage = 100.0f;
    gBatteryState.status = BATTERY_NOT_PRESENT;
    gBatteryState.isCharging = false;
    gBatteryState.usbPresent = true;
    INFO_SYSTEMF("Battery monitor enabled but no backend selected — USB assumed");
  #endif
#endif
}

void updateBattery() {
#if !ENABLE_BATTERY_MONITOR
  return;
#else
  #if BATTERY_BACKEND_ADC
    adcBackendSample(gBatteryState);
    classifyAndNotify(gBatteryState);
  #elif BATTERY_BACKEND_FUEL_GAUGE
    fuelGaugeBackendSample(gBatteryState);
    if (gBatteryState.voltage >= NO_CELL_VOLTAGE_THRESHOLD ||
        gBatteryState.status == BATTERY_UNKNOWN) {
      classifyAndNotify(gBatteryState);
    } else {
      gBatteryState.status = BATTERY_NOT_PRESENT;
    }
  #endif
  gBatteryState.lastReadMs = millis();
#endif
}

// ============================================================================
// Accessors
// ============================================================================

float getBatteryPercentage() { return gBatteryState.percentage; }
float getBatteryVoltage()    { return gBatteryState.voltage; }
bool  isBatteryCharging()    { return gBatteryState.isCharging; }
bool  isUsbPresent()         { return gBatteryState.usbPresent; }

const char* getBatteryStatusString() {
#if !ENABLE_BATTERY_MONITOR
  return "USB Power";
#endif
  switch (gBatteryState.status) {
    case BATTERY_CHARGING:    return "Charging";
    case BATTERY_FULL:        return "Full";
    case BATTERY_HIGH:        return "High";
    case BATTERY_GOOD:        return "Good";
    case BATTERY_MEDIUM:      return "Medium";
    case BATTERY_DISCHARGING: return "Discharging";
    case BATTERY_LOW:         return "Low";
    case BATTERY_CRITICAL:    return "Critical";
    case BATTERY_EMPTY:       return "Empty";
    case BATTERY_NOT_PRESENT: return "Not Present";
    default:                  return "Unknown";
  }
}

char getBatteryIcon() {
  if (gBatteryState.status == BATTERY_NOT_PRESENT) return '?';
  if (gBatteryState.isCharging) return '+';
  if (gBatteryState.percentage >= 75) return 'F';
  if (gBatteryState.percentage >= 50) return 'H';
  if (gBatteryState.percentage >= 25) return 'M';
  if (gBatteryState.percentage >= 10) return 'L';
  return 'E';
}

// ============================================================================
// CLI commands
// ============================================================================

const char* cmd_battery_status(const String& /*argsInput*/) {
  updateBattery();

  broadcastOutput("");
  broadcastOutput("╔════════════════════════════════════════╗");
  broadcastOutput("║         BATTERY STATUS                 ║");
  broadcastOutput("╠════════════════════════════════════════╣");

  char line[80];  // still used by the Last-Read line and the USB-only backend below
  // Content rows grouped (was one broadcast each). %-20.20s caps the strings so
  // the envelope is bounded; the box borders stay single (each ~126 B in UTF-8).
  BROADCAST_PRINTF(
    "║ Voltage:     %.2fV                    ║\n"
    "║ Percentage:  %.0f%%                    ║\n"
    "║ Status:      %-20.20s ║",
    gBatteryState.voltage, gBatteryState.percentage, getBatteryStatusString());
  BROADCAST_PRINTF(
    "║ Charging:    %-20.20s ║\n"
    "║ USB Power:   %-20.20s ║",
    gBatteryState.isCharging ? "Yes" : "No",
    gBatteryState.usbPresent
      ? (kHasVbusSense ? "Yes (VBUS pin)" : "Yes (inferred)")
      : (kHasVbusSense ? "No  (VBUS pin)" : "No  (inferred)"));

  broadcastOutput("║                                        ║");

#if BATTERY_BACKEND_ADC
  BROADCAST_PRINTF(
    "║ Backend:     ADC                       ║\n"
    "║ Raw ADC:     %4d / 4095               ║",
    gBatteryState.rawADC);
#elif BATTERY_BACKEND_FUEL_GAUGE
  BROADCAST_PRINTF(
    "║ Backend:     MAX17048 fuel gauge       ║\n"
    "║ CRATE:       %+.2f %%/hr                ║",
    gBatteryState.cratePctPerHr);
#else
  snprintf(line, sizeof(line), "║ Backend:     USB-only (no battery HW)  ║");
  broadcastOutput(line);
#endif

  snprintf(line, sizeof(line), "║ Last Read:   %lu ms ago               ║",
           (unsigned long)(millis() - gBatteryState.lastReadMs));
  broadcastOutput(line);

  broadcastOutput("╠════════════════════════════════════════╣");
  BROADCAST_PRINTF(
    "║ LiPo Voltage Reference:                ║\n"
    "║   Full:      %.2fV                    ║\n"
    "║   Nominal:   %.2fV                    ║",
    VBAT_FULL, VBAT_NOMINAL);
  BROADCAST_PRINTF(
    "║   Low:       %.2fV                    ║\n"
    "║   Critical:  %.2fV                    ║",
    VBAT_LOW, VBAT_CRITICAL);
  broadcastOutput("╚════════════════════════════════════════╝");

  return "Battery status displayed above";
}

const char* cmd_battery_calibrate(const String& /*argsInput*/) {
#if BATTERY_BACKEND_ADC
  if (adc_chars) free(adc_chars);
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);

  for (int i = 0; i < BATTERY_SAMPLES; i++) voltageHistory[i] = 0;
  voltageIndex = 0;
  historyFilled = false;
  for (int i = 0; i < BATTERY_SAMPLES; i++) { updateBattery(); delay(100); }
  return "Battery calibration complete (ADC). Check 'batterystatus' for new readings.";
#elif BATTERY_BACKEND_FUEL_GAUGE
  // The MAX17048's ModelGauge self-calibrates continuously — there's no
  // ADC reference to recharacterize. Re-probe so a hot-plugged cell or
  // recently-powered chip shows up without a reboot.
  fuelGaugePresent = fuelGaugeProbe();
  updateBattery();
  return fuelGaugePresent
    ? "Battery: MAX17048 re-probed; readings refreshed."
    : "Battery: MAX17048 not detected on the configured bus.";
#else
  return "Battery: no calibration applicable (USB-only build).";
#endif
}

// ============================================================================
// Battery time-series logging (CSV → /battery.csv, rotating)
// ============================================================================
// Appends one CSV row per sample so a full discharge curve can be pulled off
// the device and graphed. On by default (gSettings.batteryLogEnabled), slow
// interval (gSettings.batteryLogIntervalMs, default 60s) since battery state
// changes slowly. Rotates at kBatteryLogMaxSize keeping a couple generations.
// Independent of the sensor log, so it runs even when that's off.

static const char*  kBatteryLogPath         = "/battery.csv";
static const size_t kBatteryLogMaxSize       = 65536;  // 64 KB before rotation
static const int    kBatteryLogMaxRotations  = 2;      // keep .1 and .2

// Build + append one CSV row. `event` is the event-column token: "" for a
// periodic sample, or a short tag (e.g. "powersave:enter", "cpufreq:80MHz") for
// a power-state change. Every row carries the live battery snapshot, so an
// event is automatically annotated with the battery state at that instant.
static void batteryLogAppend(const char* event) {
  extern uint32_t gBootCounter;            // session id — bumps each boot
  const time_t epoch = time(nullptr);
  char dt[20];
  struct tm tmv;
  localtime_r(&epoch, &tmv);
  strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", &tmv);

  // Columns — time first, then state, then event:
  //   boot,uptime_ms,epoch_s,datetime,pct[%],voltage[V],crate[%/hr],status,charging,usb,event
  // Kept as clean comma-CSV for machine parsing / standard tools; visual
  // formatting is the web UI's job, not the storage format's.
  char line[208];
  snprintf(line, sizeof(line), "%lu,%lu,%ld,%s,%.1f,%.3f,%.2f,%s,%d,%d,%s",
           (unsigned long)gBootCounter,
           (unsigned long)millis(),
           (long)epoch,
           dt,
           gBatteryState.percentage,
           gBatteryState.voltage,
           gBatteryState.cratePctPerHr,
           getBatteryStatusString(),
           gBatteryState.isCharging ? 1 : 0,
           gBatteryState.usbPresent ? 1 : 0,
           event ? event : "");

  fsLock("batlog.append");
  // trusted: the system owns its own infrastructure log. Both the periodic
  // sampler and the power-event annotations write as system, regardless of
  // whether a CLI command happened to trigger the event.
  auto ctx = VFS::systemAuth("batlog");
  const bool fresh = !VFS::existsGuarded(String(kBatteryLogPath), ctx);
  File f = VFS::openGuarded(String(kBatteryLogPath), "a", ctx, true);
  if (f) {
    if (fresh) {
      static const char* kHeader =
        "boot,uptime_ms,epoch_s,datetime,pct[%],voltage[V],crate[%/hr],status,charging,usb,event\n";
      f.write((const uint8_t*)kHeader, strlen(kHeader));
    }
    f.write((const uint8_t*)line, strlen(line));
    f.write((uint8_t)'\n');
    const size_t sz = f.size();
    f.close();

    // Size rotation: drop oldest, shift .i → .(i+1), then base → .1.
    if (sz > kBatteryLogMaxSize) {
      char p[80], q[80];
      snprintf(p, sizeof(p), "%s.%d", kBatteryLogPath, kBatteryLogMaxRotations);
      if (VFS::existsGuarded(String(p), ctx)) VFS::removeGuarded(String(p), ctx);
      for (int i = kBatteryLogMaxRotations - 1; i >= 1; i--) {
        snprintf(p, sizeof(p), "%s.%d", kBatteryLogPath, i);
        snprintf(q, sizeof(q), "%s.%d", kBatteryLogPath, i + 1);
        if (VFS::existsGuarded(String(p), ctx)) VFS::renameGuarded(String(p), String(q), ctx);
      }
      snprintf(q, sizeof(q), "%s.1", kBatteryLogPath);
      if (VFS::existsGuarded(String(kBatteryLogPath), ctx))
        VFS::renameGuarded(String(kBatteryLogPath), String(q), ctx);
    }
  }
  fsUnlock();
}

void batteryLogTick() {
  if (!gSettings.batteryLogEnabled) return;
  static unsigned long lastLogMs = 0;
  const unsigned long nowMs = millis();
  uint32_t interval = gSettings.batteryLogIntervalMs;
  if (interval < 5000) interval = 5000;  // floor: never hammer the flash
  if (lastLogMs != 0 && (nowMs - lastLogMs) < interval) return;
  lastLogMs = nowMs;
  batteryLogAppend("");  // periodic sample — empty event column
}

void batteryLogEvent(const char* event) {
  // Discrete power-state annotation (sleep/wake, CPU freq, power mode, power-
  // save enter/wake). Not interval-gated — always recorded so the discharge
  // curve can be read against exactly when the system changed power state.
  if (!gSettings.batteryLogEnabled) return;
  batteryLogAppend(event);
}

// CLI: batterylog [status|on|off|interval <s>|tail|clear]
const char* cmd_batterylog(const String& argsInput) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String a = argsInput; a.trim();
  // CLI handler: act as the invoking user (per-task identity) so battery-log
  // file reads/clears are permission-checked against the caller, not system.
  const AuthContext& ctx = currentAuthContext();

  if (a.equalsIgnoreCase("on"))  { setSetting(gSettings.batteryLogEnabled, true);  return "Battery log: ON"; }
  if (a.equalsIgnoreCase("off")) { setSetting(gSettings.batteryLogEnabled, false); return "Battery log: OFF"; }

  if (a.startsWith("interval")) {
    String v = a.substring(8); v.trim();
    long sec = v.toInt();
    if (sec < 5 || sec > 3600) return "Usage: batterylog interval <5..3600>  (seconds)";
    setSetting(gSettings.batteryLogIntervalMs, (uint32_t)(sec * 1000));
    snprintf(getDebugBuffer(), 1024, "Battery log interval set to %ld s", sec);
    return getDebugBuffer();
  }

  if (a.equalsIgnoreCase("clear")) {
    fsLock("batlog.clear");
    char p[80];
    for (int i = 0; i <= kBatteryLogMaxRotations; i++) {
      if (i == 0) snprintf(p, sizeof(p), "%s", kBatteryLogPath);
      else        snprintf(p, sizeof(p), "%s.%d", kBatteryLogPath, i);
      if (VFS::existsGuarded(String(p), ctx)) VFS::removeGuarded(String(p), ctx);
    }
    fsUnlock();
    return "Battery log cleared.";
  }

  if (a.equalsIgnoreCase("tail")) {
    fsLock("batlog.tail");
    File f = VFS::openGuarded(String(kBatteryLogPath), "r", ctx, false);
    if (!f) { fsUnlock(); return "Battery log: empty (no file yet)."; }
    const size_t sz = f.size();
    // Print roughly the last ~2KB; drop the first (partial) line when seeking.
    bool dropPartial = false;
    if (sz > 2048) { f.seek(sz - 2048); dropPartial = true; }
    broadcastOutput("Battery log (recent):");
    int shown = 0;
    while (f.available()) {
      String ln = f.readStringUntil('\n');
      if (dropPartial) { dropPartial = false; continue; }
      ln.trim();
      if (ln.length()) { broadcastOutput(ln.c_str()); shown++; }
    }
    f.close();
    fsUnlock();
    return shown ? "[Battery] log tail shown" : "Battery log: empty.";
  }

  // Default / "status": summarize state + current reading.
  size_t sz = 0;
  fsLock("batlog.stat");
  File f = VFS::openGuarded(String(kBatteryLogPath), "r", ctx, false);
  if (f) { sz = f.size(); f.close(); }
  fsUnlock();
  snprintf(getDebugBuffer(), 1024,
           "Battery log: %s | file %s (%u B) | interval %lu s\n"
           "  now: %.1f%%  %.3fV  %+.2f%%/hr  (%s)\n"
           "  cmds: batterylog [on|off|interval <s>|tail|clear]",
           gSettings.batteryLogEnabled ? "ON" : "OFF",
           kBatteryLogPath, (unsigned)sz,
           (unsigned long)(gSettings.batteryLogIntervalMs / 1000),
           gBatteryState.percentage, gBatteryState.voltage,
           gBatteryState.cratePctPerHr, getBatteryStatusString());
  return getDebugBuffer();
}

// Battery-log settings module (registered in System_Settings.cpp).
static bool batteryLogModuleConnected() { return true; }
static const SettingEntry batteryLogSettingEntries[] = {
  { "enabled",    SETTING_BOOL, &gSettings.batteryLogEnabled,    true,  0, nullptr, 0, 1,       "Battery log enabled",       nullptr, false, nullptr, nullptr },
  { "intervalMs", SETTING_INT,  &gSettings.batteryLogIntervalMs, 60000, 0, nullptr, 5000, 3600000, "Battery log interval (ms)", nullptr, false, nullptr, nullptr }
};
extern const SettingsModule batteryLogSettingsModule = {
  "batteryLog",
  "system.batteryLog",
  batteryLogSettingEntries,
  sizeof(batteryLogSettingEntries) / sizeof(batteryLogSettingEntries[0]),
  batteryLogModuleConnected,
  "Battery time-series CSV logging"
};

// Command registration handled in System_Utils.cpp.
