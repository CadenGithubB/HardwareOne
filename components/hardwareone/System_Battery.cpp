// Shared battery telemetry. Board wiring selects ADC or MAX17048; consumers
// use one coherent, explicitly valid snapshot. A BAT node is not proof that a
// cell is installed, and voltage never establishes USB or charging state.
#include "System_Battery.h"
#include "System_Events.h"
#include "System_Command.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "System_Debug.h"
#include "System_Notifications.h"
#include "System_Settings.h"
#include "System_VFS.h"
#include "System_AuthIdentity.h"
#include "System_Mutex.h"
#include <esp_err.h>
#include <time.h>

#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#endif
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
#include "i2csensor_max17048.h"
#endif

BatteryState gBatteryState;
static portMUX_TYPE sStateMux = portMUX_INITIALIZER_UNLOCKED;
// Created once by setup's initBattery(), before the first battery tick. All
// hardware init/read/recalibration and transition detection share this mutex.
static SemaphoreHandle_t sBackendMutex = nullptr;
static bool sBackendInitialized = false;

struct BatteryGuard {
  bool held;
  BatteryGuard() : held(sBackendMutex && xSemaphoreTake(sBackendMutex, portMAX_DELAY) == pdTRUE) {}
  ~BatteryGuard() { if (held) xSemaphoreGive(sBackendMutex); }
};
static BatteryState storedSnapshot() {
  portENTER_CRITICAL(&sStateMux);
  const BatteryState result = gBatteryState;
  portEXIT_CRITICAL(&sStateMux);
  return result;
}
static void publishSnapshot(const BatteryState& state) {
  portENTER_CRITICAL(&sStateMux);
  gBatteryState = state;
  portEXIT_CRITICAL(&sStateMux);
}
BatteryState getBatterySnapshot() {
  BatteryState state = storedSnapshot();
  BatteryPolicy::expireSample(state, millis());
  return state;
}

#if ENABLE_BATTERY_MONITOR && BATTERY_VBUS_SENSE_PIN >= 0
static constexpr bool kHasVbusSense = true;
static void vbusSenseInit() { pinMode(BATTERY_VBUS_SENSE_PIN, INPUT); }
static void sampleVbus(BatteryState& state) {
  state.usbPresent = digitalRead(BATTERY_VBUS_SENSE_PIN) == HIGH;
  state.usbKnown = true;
}
#else
static constexpr bool kHasVbusSense = false;
static void vbusSenseInit() {}
static void sampleVbus(BatteryState& state) {
  state.usbKnown = false;
  state.usbPresent = false;
}
#endif

#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
static adc_oneshot_unit_handle_t sAdcUnit = nullptr;
static adc_cali_handle_t sAdcCalibration = nullptr;
static adc_unit_t sAdcUnitId;
static adc_channel_t sAdcChannel;
static esp_err_t sAdcInitError = ESP_ERR_INVALID_STATE;
static bool sAdcEfuseCalibrated = false;

static esp_err_t adcBackendDeinit() {
  if (sAdcCalibration) {
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    err = adc_cali_delete_scheme_curve_fitting(sAdcCalibration);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    err = adc_cali_delete_scheme_line_fitting(sAdcCalibration);
#endif
    if (err != ESP_OK) return err;
    sAdcCalibration = nullptr;
  }
  if (sAdcUnit) {
    const esp_err_t err = adc_oneshot_del_unit(sAdcUnit);
    if (err != ESP_OK) return err;
    sAdcUnit = nullptr;
  }
  sAdcEfuseCalibrated = false;
  return ESP_OK;
}

static esp_err_t adcBackendInit() {
  esp_err_t err = adcBackendDeinit();
  if (err != ESP_OK) return sAdcInitError = err;
  err = adc_oneshot_io_to_channel(BATTERY_ADC_PIN, &sAdcUnitId, &sAdcChannel);
  if (err != ESP_OK) return sAdcInitError = err;
  adc_oneshot_unit_init_cfg_t unit = {};
  unit.unit_id = sAdcUnitId;
  unit.ulp_mode = ADC_ULP_MODE_DISABLE;
  err = adc_oneshot_new_unit(&unit, &sAdcUnit);
  if (err != ESP_OK) return sAdcInitError = err;
  adc_oneshot_chan_cfg_t channel = {};
  channel.atten = BATTERY_ADC_ATTEN;
  channel.bitwidth = ADC_BITWIDTH_12;
  err = adc_oneshot_config_channel(sAdcUnit, sAdcChannel, &channel);
  if (err == ESP_OK) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration = {};
    calibration.unit_id = sAdcUnitId;
    calibration.chan = sAdcChannel;
    calibration.atten = BATTERY_ADC_ATTEN;
    calibration.bitwidth = ADC_BITWIDTH_12;
    err = adc_cali_create_scheme_curve_fitting(&calibration, &sAdcCalibration);
    sAdcEfuseCalibrated = err == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t calibration = {};
    calibration.unit_id = sAdcUnitId;
    calibration.atten = BATTERY_ADC_ATTEN;
    calibration.bitwidth = ADC_BITWIDTH_12;
#if CONFIG_IDF_TARGET_ESP32
    adc_cali_line_fitting_efuse_val_t source;
    err = adc_cali_scheme_line_fitting_check_efuse(&source);
    if (err == ESP_OK) {
      sAdcEfuseCalibrated = source != ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF;
      calibration.default_vref = BATTERY_ADC_DEFAULT_VREF_MV;
      if (!sAdcEfuseCalibrated && calibration.default_vref == 0) err = ESP_ERR_NOT_SUPPORTED;
    }
#else
    sAdcEfuseCalibrated = true;
#endif
    if (err == ESP_OK) err = adc_cali_create_scheme_line_fitting(&calibration, &sAdcCalibration);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
  }
  if (err != ESP_OK) {
    // Never substitute a raw-code/3.3V estimate when calibration fails.
    adcBackendDeinit();
    INFO_SYSTEMF("Battery ADC unavailable: %s", esp_err_to_name(err));
  } else {
    INFO_SYSTEMF("Battery ADC: GPIO%d, unit%d channel%d, divider %.5f, %s calibration",
      BATTERY_ADC_PIN, (int)sAdcUnitId + 1, (int)sAdcChannel,
      (double)BATTERY_ADC_DIVIDER, sAdcEfuseCalibrated ? "eFuse" : "nominal Vref");
  }
  return sAdcInitError = err;
}

static esp_err_t adcBackendSample(BatteryState& state) {
  if (sAdcInitError != ESP_OK) return sAdcInitError;
  if (!sAdcUnit || !sAdcCalibration) return ESP_ERR_INVALID_STATE;
  int raw = 0;
  // Discard the first conversion after idle, then average calibrated values.
  // This reduces noise, but resistor/ADC accuracy still needs meter validation.
  esp_err_t err = adc_oneshot_read(sAdcUnit, sAdcChannel, &raw);
  if (err != ESP_OK) return err;
  delayMicroseconds(200);
  uint32_t rawSum = 0;
  int32_t mvSum = 0;
  constexpr unsigned count = 16;
  for (unsigned i = 0; i < count; ++i) {
    err = adc_oneshot_read(sAdcUnit, sAdcChannel, &raw);
    if (err != ESP_OK) return err;
    // A clipped upper rail cannot establish terminal voltage. Zero remains
    // a meaningful BAT-node reading, but is not classified as an absent cell.
    if (raw < 0 || raw >= 4095) return ESP_ERR_INVALID_RESPONSE;
    int mv = 0;
    err = adc_cali_raw_to_voltage(sAdcCalibration, raw, &mv);
    if (err != ESP_OK) return err;
    if (mv < 0) return ESP_ERR_INVALID_RESPONSE;
    rawSum += raw;
    mvSum += mv;
    delayMicroseconds(100);
  }
  const float volts = (mvSum / (1000.0f * count)) * BATTERY_ADC_DIVIDER;
  if (!BatteryPolicy::applyAdcVoltage(state, volts, rawSum / count, millis()))
    return ESP_ERR_INVALID_RESPONSE;
  state.voltageCalibrated = sAdcEfuseCalibrated;
  return ESP_OK;
}
#endif

#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
static bool sFuelGaugePresent = false;
static esp_err_t fuelGaugeBackendSample(BatteryState& state) {
  if (!sFuelGaugePresent) sFuelGaugePresent = fuelGaugeProbe();
  if (!sFuelGaugePresent) return ESP_ERR_NOT_FOUND;
  FuelGaugeReading reading;
  if (!fuelGaugeRead(&reading)) {
    sFuelGaugePresent = false;
    return ESP_FAIL;
  }
  if (!std::isfinite(reading.voltage) || reading.voltage < 0 || reading.voltage > 6.0f ||
      !std::isfinite(reading.socPct) || reading.socPct < 0 || reading.socPct > 100.0f ||
      !std::isfinite(reading.cratePctPerHr)) return ESP_ERR_INVALID_RESPONSE;
  state.voltage = reading.voltage;
  state.percentage = reading.socPct;
  state.cratePctPerHr = reading.cratePctPerHr;
  state.rawADC = 0;
  state.voltageValid = state.voltageCalibrated = state.hasSample = true;
  state.percentageValid = std::isfinite(BatteryPolicy::estimatePercentage(reading.voltage));
  state.percentageEstimated = true; // MAX17048 ModelGauge estimates SOC too.
  state.rateValid = state.percentageValid;
  state.presenceKnown = false; // Gauge communication is not a cell detector.
  state.chargingKnown = false;
  state.chargingEstimated = true;
  if (state.usbKnown && !state.usbPresent) {
    state.chargingKnown = true;
    state.isCharging = false;
    state.chargingEstimated = false;
  } else if (state.rateValid && fabsf(reading.cratePctPerHr) > 0.5f) {
    state.chargingKnown = true;
    state.isCharging = reading.cratePctPerHr > 0.5f;
  }
  // CRATE's deadband means unknown, not indefinite retention of "charging".
  // A high terminal voltage does not establish USB or force SOC to 100%.
  state.lastReadMs = state.lastAttemptMs = millis();
  state.lastError = ESP_OK;
  state.stale = false;
  state.status = BatteryPolicy::classify(state);
  state.statusEstimated = state.status != BATTERY_UNKNOWN &&
    (state.status != BATTERY_CHARGING || state.chargingEstimated);
  return ESP_OK;
}
#endif

static void notifyTransitions(const BatteryState& prev, const BatteryState& state) {
  if (prev.usbKnown && state.usbKnown && prev.usbPresent != state.usbPresent)
    systemEventPost(state.usbPresent ? SYSEVT_USB_ON : SYSEVT_USB_OFF);
  char pct[12];
  if (state.percentageValid)
    snprintf(pct, sizeof(pct), "%s%d", state.percentageEstimated ? "~" : "", (int)state.percentage);
  else snprintf(pct, sizeof(pct), "unknown");
  if (prev.chargingKnown && state.chargingKnown && prev.isCharging != state.isCharging)
    systemEventPost(state.isCharging ? SYSEVT_CHARGING_STARTED : SYSEVT_CHARGING_STOPPED, pct);
  if (prev.status != BATTERY_UNKNOWN && state.percentageValid) {
    if (state.status == BATTERY_LOW && prev.status != BATTERY_LOW)
      systemEventPost(SYSEVT_BATTERY_LOW, pct);
    if ((state.status == BATTERY_CRITICAL || state.status == BATTERY_EMPTY) &&
        prev.status != BATTERY_CRITICAL && prev.status != BATTERY_EMPTY)
      systemEventPost(SYSEVT_BATTERY_CRITICAL, pct);
  }
  // Preserve the full-charge latch across short read errors and SOC jitter.
  // Lifecycle mutex serializes this edge detector with recalibration too.
  static bool wasFull = false;
  if (state.usbKnown && !state.usbPresent) wasFull = false;
  if (state.percentageValid && state.percentage < 95.0f) wasFull = false;
  if (!wasFull && state.percentageValid && state.usbKnown && state.usbPresent &&
      state.percentage >= 100.0f) {
    if (prev.percentageValid) systemEventPost(SYSEVT_BATTERY_FULL, pct);
    wasFull = true;
  }

}

// Caller holds sBackendMutex. Readers see the previous complete snapshot while
// hardware I/O is in progress, then atomically see either success or failure.
static void updateBatteryLocked() {
  BatteryState state = storedSnapshot();
  const BatteryState previous = getBatterySnapshot();
  state.lastAttemptMs = millis();
  sampleVbus(state);
  esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  err = adcBackendSample(state);
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  err = fuelGaugeBackendSample(state);
#endif
  if (err != ESP_OK) BatteryPolicy::invalidateSample(state, err, millis());
  state.status = BatteryPolicy::classify(state);
  publishSnapshot(state);
  notifyTransitions(previous, state);
}

void initBattery() {
  // Setup calls once before periodic and command access. Subsequent calls use
  // the same lifecycle lock and do not allocate another ADC unit.
  if (!sBackendMutex) sBackendMutex = xSemaphoreCreateMutex();
  BatteryGuard guard;
  if (!guard.held || sBackendInitialized) return;
  sBackendInitialized = true;
  BatteryState state;
  state.voltageAvailable = ENABLE_BATTERY_MONITOR && (BATTERY_BACKEND_ADC || BATTERY_BACKEND_FUEL_GAUGE);
  publishSnapshot(state);
  vbusSenseInit();
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  adcBackendInit();
  updateBatteryLocked();
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  // I2C initializes after initBattery(); first loop tick performs the probe.
  INFO_SYSTEMF("Battery: MAX17048 probe deferred until first battery tick");
#else
  INFO_SYSTEMF("Battery measurement unavailable for this build");
#endif
}

void updateBattery() {
#if ENABLE_BATTERY_MONITOR && (BATTERY_BACKEND_ADC || BATTERY_BACKEND_FUEL_GAUGE)
  BatteryGuard guard;
  if (guard.held && sBackendInitialized) updateBatteryLocked();
#endif
}

float getBatteryPercentage() {
  const auto state = getBatterySnapshot();
  return state.percentageValid ? state.percentage : NAN;
}
float getBatteryVoltage() {
  const auto state = getBatterySnapshot();
  return state.voltageValid ? state.voltage : NAN;
}
bool isBatteryCharging() {
  const auto state = getBatterySnapshot();
  return state.chargingKnown && state.isCharging;
}
bool isUsbPresent() {
  const auto state = getBatterySnapshot();
  return state.usbKnown && state.usbPresent;
}
const char* batteryStatusString(BatteryStatus status) {
  switch (status) {
    case BATTERY_CHARGING: return "Charging";
    case BATTERY_FULL: return "Full";
    case BATTERY_HIGH: return "High";
    case BATTERY_GOOD: return "Good";
    case BATTERY_MEDIUM: return "Medium";
    case BATTERY_DISCHARGING: return "Discharging";
    case BATTERY_LOW: return "Low";
    case BATTERY_CRITICAL: return "Critical";
    case BATTERY_EMPTY: return "Empty";
    case BATTERY_NOT_PRESENT: return "Not Present";
    default: return "Unknown";
  }
}
const char* batteryStatusString(const BatteryState& state) {
  if (!state.statusEstimated) return batteryStatusString(state.status);
  switch (state.status) {
    case BATTERY_CHARGING: return "Charging (estimated)";
    case BATTERY_FULL: return "Full (estimated)";
    case BATTERY_HIGH: return "High (estimated)";
    case BATTERY_GOOD: return "Good (estimated)";
    case BATTERY_MEDIUM: return "Medium (estimated)";
    case BATTERY_LOW: return "Low (estimated)";
    case BATTERY_CRITICAL: return "Critical (estimated)";
    case BATTERY_EMPTY: return "Empty (estimated)";
    default: return batteryStatusString(state.status);
  }
}
const char* getBatteryStatusString() { return batteryStatusString(getBatterySnapshot()); }
char getBatteryIcon() {
  const auto state = getBatterySnapshot();
  if (!state.percentageValid) return '?';
  if (state.chargingKnown && state.isCharging) return '+';
  if (state.percentage >= 75) return 'F';
  if (state.percentage >= 50) return 'H';
  if (state.percentage >= 25) return 'M';
  if (state.percentage >= 10) return 'L';
  return 'E';
}

void buildBatteryJson(JsonDocument& doc) {
  const auto state = getBatterySnapshot();
  doc["schema"] = 1;
#if !ENABLE_BATTERY_MONITOR
  doc["backend"] = "disabled";
#elif BATTERY_BACKEND_FUEL_GAUGE
  doc["backend"] = "fuelgauge";
#elif BATTERY_BACKEND_ADC
  doc["backend"] = "adc";
#else
  doc["backend"] = "unavailable";
#endif
  if (state.presenceKnown) doc["present"] = state.present; else doc["present"] = nullptr;
  if (state.voltageValid) doc["voltage"] = state.voltage; else doc["voltage"] = nullptr;
  if (state.percentageValid) doc["percentage"] = state.percentage; else doc["percentage"] = nullptr;
  if (state.chargingKnown) doc["charging"] = state.isCharging; else doc["charging"] = nullptr;
  if (state.usbKnown) doc["usbPresent"] = state.usbPresent; else doc["usbPresent"] = nullptr;
  doc["status"] = batteryStatusString(state);
  doc["vbusSense"] = kHasVbusSense;
  doc["voltageAvailable"] = state.voltageAvailable;
  doc["voltageValid"] = state.voltageValid;
  doc["voltageCalibrated"] = state.voltageCalibrated;
  doc["percentageValid"] = state.percentageValid;
  doc["percentageEstimated"] = state.percentageEstimated;
  doc["statusEstimated"] = state.statusEstimated;
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  doc["percentageSource"] = "voltage";
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  doc["percentageSource"] = "fuelgauge";
#else
  doc["percentageSource"] = nullptr;
#endif
  doc["presenceKnown"] = state.presenceKnown;
  doc["chargingKnown"] = state.chargingKnown;
  doc["chargingEstimated"] = state.chargingEstimated;
  doc["usbKnown"] = state.usbKnown;
  doc["rateValid"] = state.rateValid;
  doc["stale"] = state.stale;
  doc["hasSample"] = state.hasSample;
  doc["lastError"] = state.lastError;
  if (state.hasSample) doc["lastReadMsAgo"] = (uint32_t)(millis() - state.lastReadMs);
  else doc["lastReadMsAgo"] = nullptr;
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  if (state.rateValid) doc["ratePctPerHr"] = state.cratePctPerHr;
  else doc["ratePctPerHr"] = nullptr;
  // CRATE is a delayed estimate. Do not provide a fabricated runtime when
  // there is no valid negative rate; preserve the existing optional key.
  if (state.percentageValid && state.rateValid && state.cratePctPerHr < -0.01f)
    doc["etaMinutes"] = (long)((state.percentage / -state.cratePctPerHr) * 60.0f);
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  if (state.voltageValid) doc["rawADC"] = state.rawADC; else doc["rawADC"] = nullptr;
  doc["adcPin"] = BATTERY_ADC_PIN;
  doc["divider"] = BATTERY_ADC_DIVIDER;
#endif
}

const char* cmd_battery_status(const String& argsInput) {
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    buildBatteryJson(doc);
    static char* jbuf = nullptr;
    constexpr size_t size = 1536;
    if (!jbuf) jbuf = (char*)ps_alloc(size, AllocPref::PreferPSRAM, "battery.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    if (measureJson(doc) >= size) return "{\"error\":\"battery_json_too_large\"}";
    serializeJson(doc, jbuf, size);
    return jbuf;
  }
  const auto state = getBatterySnapshot();
  char voltage[24] = "Unavailable", percentage[24] = "Unavailable";
  if (state.voltageValid) snprintf(voltage, sizeof(voltage), "%.3f V", state.voltage);
  if (state.percentageValid) snprintf(percentage, sizeof(percentage), "%s%.0f%%", state.percentageEstimated ? "~" : "", state.percentage);
  BROADCAST_PRINTF("Battery: %s\nVoltage: %s\nPercentage: %s\nCell present: %s\nCharging: %s\nUSB power: %s",
    batteryStatusString(state), voltage, percentage,
    state.presenceKnown ? (state.present ? "Yes" : "No") : "Unknown",
    state.chargingKnown ? (state.isCharging ? "Yes" : "No") : "Unknown",
    state.usbKnown ? (state.usbPresent ? "Yes" : "No") : "Unknown");
  if (state.percentageEstimated)
    broadcastOutput("SOC is an estimate; BAT voltage alone cannot confirm a connected cell.");
  if (state.hasSample) BROADCAST_PRINTF("Last successful sample: %lu ms ago%s",
    (unsigned long)(millis() - state.lastReadMs), state.stale ? " (stale)" : "");
  if (state.lastError != ESP_OK) BROADCAST_PRINTF("Last read error: %s", esp_err_to_name(state.lastError));
  return "Battery status displayed above";
}

const char* cmd_battery_calibrate(const String&) {
  BatteryGuard guard;
  if (!guard.held || !sBackendInitialized) return "Battery monitor is not initialized.";
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  const esp_err_t err = adcBackendInit();
  updateBatteryLocked();
  if (err != ESP_OK || !storedSnapshot().voltageValid)
    return "Battery ADC calibration/read failed; telemetry is unavailable. See batterystatus json.";
  return "Battery ADC calibration reloaded and reading refreshed. Divider accuracy still requires comparison with a meter.";
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  sFuelGaugePresent = false;
  updateBatteryLocked();
  return storedSnapshot().voltageValid
    ? "Battery: MAX17048 re-probed; readings refreshed."
    : "Battery: MAX17048 unavailable or read failed.";
#else
  return "Battery: no measurement hardware enabled; calibration unavailable.";
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
  const auto state = getBatterySnapshot();
  // Preserve the existing 11-column CSV schema. Unknown values are empty,
  // never fabricated zero/full/USB; the status names explicitly mark estimates.
  char pct[24] = "", volts[24] = "", rate[24] = "";
  if (state.percentageValid) snprintf(pct, sizeof(pct), "%.1f", state.percentage);
  if (state.voltageValid) snprintf(volts, sizeof(volts), "%.3f", state.voltage);
  if (state.rateValid) snprintf(rate, sizeof(rate), "%.2f", state.cratePctPerHr);
  char line[256];
  snprintf(line, sizeof(line), "%lu,%lu,%ld,%s,%s,%s,%s,%s,%s,%s,%s",
           (unsigned long)gBootCounter, (unsigned long)millis(), (long)epoch, dt,
           pct, volts, rate, batteryStatusString(state),
           state.chargingKnown ? (state.isCharging ? "1" : "0") : "",
           state.usbKnown ? (state.usbPresent ? "1" : "0") : "",
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
    if (sec < 5 || sec > 3600) return "Error: invalid arguments — Usage: batterylog interval <5..3600>  (seconds)";
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
  const auto state = getBatterySnapshot();
  char reading[96];
  if (state.voltageValid && state.percentageValid)
    snprintf(reading, sizeof(reading), "%s%.1f%%  %.3fV (%s)",
      state.percentageEstimated ? "~" : "", state.percentage, state.voltage, batteryStatusString(state));
  else if (state.voltageValid)
    snprintf(reading, sizeof(reading), "%.3fV, SOC unknown", state.voltage);
  else snprintf(reading, sizeof(reading), "Measurement unavailable%s", state.stale ? " (stale)" : "");
  snprintf(getDebugBuffer(), 1024,
           "Battery log: %s | file %s (%u B) | interval %lu s\n"
           "  now: %s\n"
           "  cmds: batterylog [on|off|interval <s>|tail|clear]",
           gSettings.batteryLogEnabled ? "ON" : "OFF",
           kBatteryLogPath, (unsigned)sz,
           (unsigned long)(gSettings.batteryLogIntervalMs / 1000), reading);
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
