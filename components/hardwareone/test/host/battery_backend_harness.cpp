// Firmware backend/lifecycle code is inserted by test_battery.py. Only SDK,
// GPIO, time, event delivery and the MAX17048 transport boundary are mocked.
#include "BatteryPolicy.h"
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using String = std::string;
using esp_err_t = int;
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=257,
  ESP_ERR_NOT_SUPPORTED=258, ESP_ERR_INVALID_RESPONSE=259, ESP_ERR_NOT_FOUND=260;
constexpr int pdTRUE=1, portMAX_DELAY=-1, HIGH=1, INPUT=0;
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
using SemaphoreHandle_t=std::mutex*;
static std::mutex backendMutex;
SemaphoreHandle_t xSemaphoreCreateMutex() { return &backendMutex; }
int xSemaphoreTake(SemaphoreHandle_t handle, int) { handle->lock(); return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t handle) { handle->unlock(); }
static std::atomic<uint32_t> clockMs{100};
uint32_t millis() { return clockMs.load(); }
void delayMicroseconds(unsigned) {}
static bool vbusHigh=false;
int digitalRead(int pin) { assert(pin==BATTERY_VBUS_SENSE_PIN); return vbusHigh; }
void pinMode(int, int) {}
#define INFO_SYSTEMF(...) do {} while (0)
const char* esp_err_to_name(esp_err_t) { return "mock-error"; }
enum { SYSEVT_USB_ON=1, SYSEVT_USB_OFF, SYSEVT_CHARGING_STARTED,
  SYSEVT_CHARGING_STOPPED, SYSEVT_BATTERY_LOW, SYSEVT_BATTERY_CRITICAL, SYSEVT_BATTERY_FULL };
static std::vector<int> events;
void systemEventPost(int event, const char* = nullptr) { events.push_back(event); }

using adc_unit_t=int;
using adc_channel_t=int;
using adc_oneshot_unit_handle_t=int*;
using adc_cali_handle_t=int*;
constexpr int ADC_ULP_MODE_DISABLE=0, ADC_BITWIDTH_12=12;
struct adc_oneshot_unit_init_cfg_t { int unit_id; int ulp_mode; };
struct adc_oneshot_chan_cfg_t { int atten; int bitwidth; };
struct adc_cali_curve_fitting_config_t { int unit_id; int chan; int atten; int bitwidth; };
struct adc_cali_line_fitting_config_t { int unit_id; int atten; int bitwidth; int default_vref; };
using adc_cali_line_fitting_efuse_val_t=int;
constexpr int ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF=0;
static int unitStorage=1, calibrationStorage=2;
static bool unitLive=false, calibrationLive=false, mappingFail=false, calibrationFail=false;
static bool configFail=false, conversionFail=false, negativeMillivolts=false, efusePresent=true;
static int rawCode=1000, readCount=0, readFailAt=0, createCount=0, deleteCount=0;
static int configuredUnit=-1, configuredChannel=-1, configuredAttenuation=-1;
static std::mutex blockMutex;
static std::condition_variable blockCv;
static bool blockRead=false, readBlocked=false, releaseRead=false;

esp_err_t adc_oneshot_io_to_channel(int pin, adc_unit_t* unit, adc_channel_t* channel) {
  assert(pin==BATTERY_ADC_PIN);
  if (mappingFail) return ESP_ERR_NOT_FOUND;
  *unit=EXPECTED_UNIT; *channel=EXPECTED_CHANNEL; return ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t* cfg, adc_oneshot_unit_handle_t* handle) {
  assert(!unitLive); assert(cfg->unit_id==EXPECTED_UNIT && cfg->ulp_mode==ADC_ULP_MODE_DISABLE);
  configuredUnit=cfg->unit_id; unitLive=true; ++createCount; *handle=&unitStorage; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle) {
  assert(handle==&unitStorage && unitLive && !calibrationLive);
  unitLive=false; ++deleteCount; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle, adc_channel_t channel,
                                    const adc_oneshot_chan_cfg_t* cfg) {
  assert(handle==&unitStorage && unitLive && channel==EXPECTED_CHANNEL);
  configuredChannel=channel; configuredAttenuation=cfg->atten;
  assert(cfg->bitwidth==12 && cfg->atten==BATTERY_ADC_ATTEN);
  return configFail?ESP_FAIL:ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t* cfg,
                                               adc_cali_handle_t* handle) {
  assert(unitLive && !calibrationLive && cfg->unit_id==EXPECTED_UNIT && cfg->chan==EXPECTED_CHANNEL);
  assert(cfg->atten==BATTERY_ADC_ATTEN && cfg->bitwidth==12);
  if (calibrationFail) return ESP_ERR_NOT_SUPPORTED;
  calibrationLive=true; *handle=&calibrationStorage; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle) {
  assert(handle==&calibrationStorage && calibrationLive);
  calibrationLive=false; return ESP_OK;
}
esp_err_t adc_cali_scheme_line_fitting_check_efuse(adc_cali_line_fitting_efuse_val_t* source) {
  *source=efusePresent?1:ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t* cfg,
                                              adc_cali_handle_t* handle) {
  assert(unitLive && !calibrationLive && cfg->unit_id==EXPECTED_UNIT);
  assert(cfg->atten==BATTERY_ADC_ATTEN && cfg->bitwidth==12);
  if (!efusePresent) assert(cfg->default_vref==BATTERY_ADC_DEFAULT_VREF_MV);
  if (calibrationFail) return ESP_ERR_NOT_SUPPORTED;
  calibrationLive=true; *handle=&calibrationStorage; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t handle) {
  return adc_cali_delete_scheme_curve_fitting(handle);
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int* raw) {
  assert(handle==&unitStorage && unitLive && calibrationLive && channel==EXPECTED_CHANNEL);
  {
    std::unique_lock<std::mutex> lock(blockMutex);
    if (blockRead) {
      readBlocked=true; blockCv.notify_all();
      blockCv.wait(lock, [] { return releaseRead; });
      blockRead=false;
    }
  }
  ++readCount;
  if (readFailAt && readCount==readFailAt) return ESP_FAIL;
  *raw=rawCode; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int* mv) {
  assert(handle==&calibrationStorage && calibrationLive && unitLive);
  if (conversionFail) return ESP_FAIL;
  *mv=negativeMillivolts?-1:raw; return ESP_OK;
}
struct FuelGaugeReading { float voltage; float socPct; float cratePctPerHr; uint16_t version; };
static FuelGaugeReading gaugeReading{3.9f,72.0f,-2.0f,0x12};
static bool gaugeProbeOk=true, gaugeReadOk=true;
static int gaugeProbes=0;
bool fuelGaugeProbe() { ++gaugeProbes; return gaugeProbeOk; }
bool fuelGaugeRead(FuelGaugeReading* result) { if (!gaugeReadOk) return false; *result=gaugeReading; return true; }

// INSERT_PRODUCTION_BACKEND_AND_ACCESSORS
// INSERT_PRODUCTION_CALIBRATION
// INSERT_PRODUCTION_JSON

static void checkUnknown() {
  const auto s=getBatterySnapshot();
  assert(!s.voltageValid && !s.percentageValid && !s.chargingKnown);
  assert(std::isnan(getBatteryVoltage()) && std::isnan(getBatteryPercentage()));
  assert(!isBatteryCharging());
}

static void assertJsonFits(const JsonDocument& doc) {
  char output[1536];
  assert(measureJson(doc)<sizeof(output));
  assert(serializeJson(doc,output,sizeof(output))==measureJson(doc));
  JsonDocument parsed;
  assert(!deserializeJson(parsed,output));
}

static void testJsonValidity() {
  JsonDocument doc;
#if ENABLE_BATTERY_MONITOR
  BatteryState s;
  s.voltageAvailable=true;
  BatteryPolicy::applyAdcVoltage(s,4.2f,2000,millis());
  // Stale physical values must stay null until their independent signal is known.
  s.usbPresent=s.isCharging=true;
  publishSnapshot(s);
  buildBatteryJson(doc);
  assert(doc["present"].isNull() && doc["charging"].isNull() && doc["usbPresent"].isNull());
  assert(doc["voltage"].is<float>() && doc["percentage"].is<float>());
  assert(doc["percentageEstimated"].as<bool>() && doc["statusEstimated"].as<bool>());
  assert(strstr(doc["status"].as<const char*>(), "estimated"));
  assert(doc["lastReadMsAgo"].as<unsigned>()==0);
  assertJsonFits(doc);
  BatteryPolicy::invalidateSample(s,ESP_FAIL,millis()+1);
  publishSnapshot(s); doc.clear(); buildBatteryJson(doc);
  assert(doc["voltage"].isNull() && doc["percentage"].isNull());
  assert(!doc["voltageValid"].as<bool>() && doc["hasSample"].as<bool>());
  assert(doc["lastError"].as<int>()==ESP_FAIL);
  assertJsonFits(doc);
  BatteryPolicy::applyAdcVoltage(s,3.8f,1900,millis());
  s.usbKnown=s.usbPresent=true;
  publishSnapshot(s); clockMs+=30001; doc.clear(); buildBatteryJson(doc);
  assert(doc["voltage"].isNull() && doc["usbPresent"].isNull());
  assert(doc["stale"].as<bool>());
  assertJsonFits(doc);
#else
  buildBatteryJson(doc);
  assert(doc["voltage"].isNull() && doc["percentage"].isNull() && doc["usbPresent"].isNull());
  assert(doc["present"].isNull() && doc["lastReadMsAgo"].isNull());
  assert(!strcmp(doc["backend"].as<const char*>(),"disabled"));
  assertJsonFits(doc);
#endif
}

static void testNotifications() {
  events.clear();
  BatteryState previous, next;
  BatteryPolicy::applyAdcVoltage(previous, 4.2f, 2000, 100);
  next=previous;
  next.usbPresent=true; // Unknown presence does not generate a USB event.
  notifyTransitions(previous,next);
  assert(events.empty());
  previous=next;
  next.usbKnown=true;
  notifyTransitions(previous,next);
  // An unknown->known signal is initialization, not a measured plug event.
  for (int event: events) assert(event!=SYSEVT_USB_ON);
  events.clear();
  previous=next;
  next.usbPresent=false;
  notifyTransitions(previous,next);
  assert(events.size()==1 && events[0]==SYSEVT_USB_OFF);
  previous=next;
  next.usbPresent=true;
  next.percentage=98.0f;
  notifyTransitions(previous,next);
  events.clear();
  previous=next;
  next.percentage=100.0f;
  notifyTransitions(previous,next);
  assert(events.size()==1 && events[0]==SYSEVT_BATTERY_FULL);
  previous=next; next.percentage=99.99f;
  notifyTransitions(previous,next);
  previous=next; next.percentage=100.0f;
  notifyTransitions(previous,next);
  assert(events.size()==1); // Noise at 100% cannot emit repeated full events.
  previous=next;
  BatteryPolicy::invalidateSample(next, ESP_FAIL, 200);
  notifyTransitions(previous,next);
  previous=next;
  BatteryPolicy::applyAdcVoltage(next,4.2f,2000,300);
  next.usbKnown=next.usbPresent=true;
  next.percentage=99.99f;
  notifyTransitions(previous,next);
  previous=next; next.percentage=100.0f;
  notifyTransitions(previous,next);
  assert(events.size()==1); // A failed sample cannot reset the full-event latch.
  previous=next; next.percentage=94.0f;
  notifyTransitions(previous,next);
  previous=next; next.percentage=100.0f;
  notifyTransitions(previous,next);
  assert(events.size()==2 && events[1]==SYSEVT_BATTERY_FULL);
}

int main() {
  (void)kHasVbusSense;
  initBattery();
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  auto s=getBatterySnapshot();
  assert(s.voltageAvailable && s.voltageValid && s.hasSample);
  assert(std::fabs(s.voltage-rawCode*BATTERY_ADC_DIVIDER/1000.0f)<0.001f);
  assert(s.rawADC==rawCode && readCount==17);
  assert(!s.usbKnown && !s.chargingKnown && !s.presenceKnown);
  assert(configuredUnit==EXPECTED_UNIT && configuredChannel==EXPECTED_CHANNEL);
  assert(configuredAttenuation==BATTERY_ADC_ATTEN && createCount==1);
  initBattery();
  assert(createCount==1); // Calling init twice does not reserve another unit.
  const auto successfulAt=s.lastReadMs;
  clockMs=1000; readCount=0; readFailAt=8;
  updateBattery(); checkUnknown();
  s=getBatterySnapshot();
  assert(s.lastReadMs==successfulAt && s.lastAttemptMs==1000 && s.lastError==ESP_FAIL);
  readFailAt=0; rawCode=4095; clockMs=2000;
  updateBattery(); checkUnknown();
  assert(getBatterySnapshot().lastReadMs==successfulAt);
  rawCode=1000; conversionFail=true; clockMs=3000;
  updateBattery(); checkUnknown();
  conversionFail=false; negativeMillivolts=true; clockMs=4000;
  updateBattery(); checkUnknown();
  negativeMillivolts=false; clockMs=5000;
  updateBattery(); assert(getBatterySnapshot().voltageValid);
  assert(getBatterySnapshot().lastReadMs==5000);
  // No valid calibrated conversion means unavailable, never raw/4095*3.3V.
  calibrationFail=true;
  assert(strstr(cmd_battery_calibrate(""), "failed"));
  checkUnknown(); assert(!unitLive && !calibrationLive);
  calibrationFail=false; mappingFail=true;
  cmd_battery_calibrate(""); checkUnknown(); assert(!unitLive);
  mappingFail=false; configFail=true;
  cmd_battery_calibrate(""); checkUnknown(); assert(!unitLive);
  configFail=false;
  cmd_battery_calibrate(""); assert(getBatterySnapshot().voltageValid);
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  efusePresent=false;
  cmd_battery_calibrate("");
#if BATTERY_ADC_DEFAULT_VREF_MV == 0
  checkUnknown(); assert(!unitLive);
  efusePresent=true; cmd_battery_calibrate("");
#else
  assert(getBatterySnapshot().voltageValid && !getBatterySnapshot().voltageCalibrated);
#endif
#endif
  // Hold a real update inside the mocked hardware read. Concurrent calibration
  // must wait for the production lifecycle mutex, not delete a handle in use.
  { std::lock_guard<std::mutex> lock(blockMutex); blockRead=true; readBlocked=false; releaseRead=false; }
  auto update=std::async(std::launch::async, [] { updateBattery(); });
  { std::unique_lock<std::mutex> lock(blockMutex); blockCv.wait(lock, [] { return readBlocked; }); }
  const auto duringRead=getBatterySnapshot();
  assert(duringRead.voltageValid); // Readers still see the complete previous sample.
  std::promise<void> attemptingCalibration;
  auto attempted=attemptingCalibration.get_future();
  auto calibration=std::async(std::launch::async, [&] {
    attemptingCalibration.set_value();
    return cmd_battery_calibrate("");
  });
  attempted.get();
  assert(calibration.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
  { std::lock_guard<std::mutex> lock(blockMutex); releaseRead=true; blockCv.notify_all(); }
  update.get(); (void)calibration.get();
  assert(getBatterySnapshot().voltageValid && unitLive && calibrationLive);
  assert(adcBackendDeinit()==ESP_OK);
  assert(!unitLive && !calibrationLive && createCount==deleteCount);
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  assert(gaugeProbes==0); // Boot does not touch I2C before buses are ready.
  updateBattery(); auto s=getBatterySnapshot();
  assert(s.voltageValid && s.percentageValid && s.percentage==72.0f);
  assert(s.rateValid && s.chargingKnown && !s.isCharging && !s.presenceKnown);
  assert(s.usbKnown && !s.usbPresent);
  vbusHigh=true; gaugeReading.cratePctPerHr=4.0f; clockMs=200;
  updateBattery(); s=getBatterySnapshot();
  assert(s.usbKnown && s.usbPresent && s.chargingKnown && s.isCharging);
  gaugeReading.voltage=4.17f; gaugeReading.socPct=98.0f; gaugeReading.cratePctPerHr=0.0f;
  clockMs=300; updateBattery(); s=getBatterySnapshot();
  assert(!s.chargingKnown && s.percentage==98.0f); // No false 100% clamp/deadband latch.
  gaugeReadOk=false; clockMs=400; updateBattery(); checkUnknown();
  s=getBatterySnapshot(); assert(s.lastReadMs==300 && s.lastAttemptMs==400);
  assert(s.usbKnown && s.usbPresent); // Independent VBUS survived failed I2C.
  gaugeReadOk=true; clockMs=500; updateBattery();
  assert(gaugeProbes==2 && getBatterySnapshot().voltageValid);
  gaugeReading.voltage=NAN; clockMs=600; updateBattery(); checkUnknown();
  assert(getBatterySnapshot().lastReadMs==500);
  gaugeReading.voltage=0; gaugeReading.socPct=0; clockMs=700; updateBattery();
  s=getBatterySnapshot(); assert(s.voltageValid && !s.percentageValid && !s.presenceKnown);
#else
  checkUnknown();
  auto s=getBatterySnapshot();
  assert(!s.voltageAvailable && !s.hasSample && !s.usbKnown && !s.usbPresent);
  clockMs=60000; updateBattery(); checkUnknown();
  assert(!isUsbPresent()); // Disabled monitoring is not fabricated 5V/100% USB.
#endif
#if ENABLE_BATTERY_MONITOR
  testNotifications();
#endif
  testJsonValidity();
  std::cout << "Battery backend: ADC=" << BATTERY_BACKEND_ADC
            << " gauge=" << BATTERY_BACKEND_FUEL_GAUGE
            << " enabled=" << ENABLE_BATTERY_MONITOR << " passed\n";
}
