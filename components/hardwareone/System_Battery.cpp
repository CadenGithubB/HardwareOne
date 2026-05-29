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
}

#endif  // BATTERY_BACKEND_FUEL_GAUGE

// ============================================================================
// Backend-agnostic status classification + notification
// ============================================================================
// Maps the populated BatteryState fields to a status enum and fires
// notifications on state transitions. Pure function of the inputs — no I/O.
//
// Priority order matters:
//   1. voltage < 2V    → NOT_PRESENT (no cell wired)
//   2. usbPresent      → FULL or CHARGING (USB is the authoritative signal;
//                        cell at >= VBAT_FULL-0.1V counts as FULL because USB
//                        is just holding it at the float plateau)
//   3. voltage tiers   → DISCHARGING / LOW / CRITICAL (running on cell alone)
//
// Pre-VBUS_SENSE this used isCharging (CRATE-derived) as the gate, which broke
// when USB was plugged in but CRATE had decayed to 0 — the status would slip
// to DISCHARGING even though VBUS was still connected. usbPresent is the
// correct gate; isCharging is preserved as a SUB-state ("USB+" vs "USB" on
// the display) but doesn't drive the status enum.
static void classifyAndNotify(BatteryState& state) {
  const BatteryStatus prevStatus = state.status;
  const bool wasCharging = (prevStatus == BATTERY_CHARGING || prevStatus == BATTERY_FULL);

  if (state.voltage < 2.0f) {
    state.status = BATTERY_NOT_PRESENT;
  } else if (state.usbPresent) {
    state.status = (state.voltage >= VBAT_FULL - 0.1f) ? BATTERY_FULL : BATTERY_CHARGING;
  } else if (state.voltage <= VBAT_CRITICAL) {
    state.status = BATTERY_CRITICAL;
  } else if (state.voltage <= VBAT_LOW) {
    state.status = BATTERY_LOW;
  } else {
    state.status = BATTERY_DISCHARGING;
  }

  // Notifications fire only on transitions, and never on the first read
  // (prevStatus == UNKNOWN) — avoids spamming "USB connected" at boot.
  if (prevStatus != BATTERY_UNKNOWN) {
    const bool nowCharging = (state.status == BATTERY_CHARGING || state.status == BATTERY_FULL);
    if (nowCharging && !wasCharging) {
      notifyPowerUSBConnected();
    } else if (!nowCharging && wasCharging) {
      notifyPowerUSBDisconnected();
    }
    if (state.status == BATTERY_LOW && prevStatus != BATTERY_LOW) {
      notifyBatteryLow((int)state.percentage);
    }
    if (state.status == BATTERY_CRITICAL && prevStatus != BATTERY_CRITICAL) {
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
    case BATTERY_DISCHARGING: return "Discharging";
    case BATTERY_LOW:         return "Low";
    case BATTERY_CRITICAL:    return "Critical";
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

  char line[80];
  snprintf(line, sizeof(line), "║ Voltage:     %.2fV                    ║", gBatteryState.voltage);
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ Percentage:  %.0f%%                    ║", gBatteryState.percentage);
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ Status:      %-20s ║", getBatteryStatusString());
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ Charging:    %-20s ║", gBatteryState.isCharging ? "Yes" : "No");
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ USB Power:   %-20s ║",
           gBatteryState.usbPresent
             ? (kHasVbusSense ? "Yes (VBUS pin)" : "Yes (inferred)")
             : (kHasVbusSense ? "No  (VBUS pin)" : "No  (inferred)"));
  broadcastOutput(line);

  broadcastOutput("║                                        ║");

#if BATTERY_BACKEND_ADC
  snprintf(line, sizeof(line), "║ Backend:     ADC                       ║");
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ Raw ADC:     %4d / 4095               ║", gBatteryState.rawADC);
  broadcastOutput(line);
#elif BATTERY_BACKEND_FUEL_GAUGE
  snprintf(line, sizeof(line), "║ Backend:     MAX17048 fuel gauge       ║");
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║ CRATE:       %+.2f %%/hr                ║", gBatteryState.cratePctPerHr);
  broadcastOutput(line);
#else
  snprintf(line, sizeof(line), "║ Backend:     USB-only (no battery HW)  ║");
  broadcastOutput(line);
#endif

  snprintf(line, sizeof(line), "║ Last Read:   %lu ms ago               ║",
           (unsigned long)(millis() - gBatteryState.lastReadMs));
  broadcastOutput(line);

  broadcastOutput("╠════════════════════════════════════════╣");
  broadcastOutput("║ LiPo Voltage Reference:                ║");
  snprintf(line, sizeof(line), "║   Full:      %.2fV                    ║", VBAT_FULL);
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║   Nominal:   %.2fV                    ║", VBAT_NOMINAL);
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║   Low:       %.2fV                    ║", VBAT_LOW);
  broadcastOutput(line);
  snprintf(line, sizeof(line), "║   Critical:  %.2fV                    ║", VBAT_CRITICAL);
  broadcastOutput(line);
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

// Command registration handled in System_Utils.cpp.
