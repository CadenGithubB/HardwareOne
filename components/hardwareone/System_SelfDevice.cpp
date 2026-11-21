#include "System_SelfDevice.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>

#include <esp_mac.h>          // esp_efuse_mac_get_default
#include <freertos/FreeRTOS.h>  // portMUX_TYPE: the kept station MAC
#include "System_BuildConfig.h"
#include "System_Debug.h"     // WARN_SYSTEMF
#include "System_DeviceName.h" // the device-name rule and the fallback
#include "System_MacText.h"   // macToDisplay
#include "System_MemUtil.h"   // hw1InternalMinFreeBytes
#include "System_MetricsCore.h"  // temperatureBelievable (pure header)
#include "System_Settings.h"  // gSettings.espnowDeviceName
#if ENABLE_WIFI
#include <WiFi.h>             // WiFi.setHostname, WiFi.STA
#endif

namespace SelfDevice {

uint32_t uptimeSeconds() {
  // From the 64-bit microsecond timer, not from millis(): millis() is that
  // same timer cut to 32 bits of milliseconds, so millis() / 1000 starts again
  // at 0 after 49.7 days. In seconds a uint32_t lasts 136 years.
  return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

uint32_t freeHeapBytes() {
  // Internal 8-bit RAM only — identical to dramFreeBytes() below.
  // The old comment here claimed ESP.getFreeHeap() returns "internal+SPIRAM";
  // that has not been true since Arduino defined it as
  // heap_caps_get_free_size(MALLOC_CAP_INTERNAL). It excludes PSRAM but INCLUDES
  // the IRAM-only heap, which no malloc/new/task stack can use — so it read
  // ~25.8 KB high on ESP32 classic.
  return dramFreeBytes();
}

uint32_t dramFreeBytes() {
  // Internal DRAM only. heap_caps_get_free_size with MALLOC_CAP_INTERNAL
  // | MALLOC_CAP_8BIT is what cmd_stats uses for the "DRAM free" line.
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t psramFreeBytes() {
  // SPIRAM only. Returns 0 on boards without PSRAM (heap_caps returns 0 for
  // empty capability pools).
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t minFreeHeapBytes() {
  // Internal 8-bit RAM, the pool freeHeapBytes() reports. Not
  // esp_get_minimum_free_heap_size(): that is MALLOC_CAP_DEFAULT, which takes
  // in PSRAM on a PSRAM board and read megabytes above freeHeapBytes().
  return (uint32_t)hw1InternalMinFreeBytes();
}

bool chipTemperatureC(float& out) {
  // No state here: the main loop, the command executor, the automation
  // scheduler and the G2 session task all ask.
  const float c = temperatureRead();
#ifdef CONFIG_IDF_TARGET_ESP32
  // The classic ESP32's undocumented sensor reads raw 128 ((128-32)/1.8 =
  // 53.3 C) when it has nothing: not a temperature.
  if (c > 53.3f && c < 53.4f) return false;
#endif
  // A failed read on the other chips is NAN, which is not believable either.
  if (!system_metrics_core::temperatureBelievable(c)) return false;
  out = c;
  return true;
}

bool macBytes(uint8_t out[6]) {
  if (!out) return false;
  // The first good answer is kept for the boot: the driver query is a blocking
  // round trip to the C6 on a hosted radio (P4X-EYE), and nothing in this
  // firmware sets the station address at run time. A few bytes of internal
  // RAM under a short lock, because any task may ask.
  static portMUX_TYPE sKeptLock = portMUX_INITIALIZER_UNLOCKED;
  static uint8_t sKept[6] = {0, 0, 0, 0, 0, 0};
  static bool sKeptValid = false;

  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  portENTER_CRITICAL(&sKeptLock);
  const bool kept = sKeptValid;
  if (kept) memcpy(mac, sKept, sizeof(mac));
  portEXIT_CRITICAL(&sKeptLock);
  if (kept) {
    memcpy(out, mac, sizeof(mac));
    return true;
  }

  // Asked outside the lock: the query may block.
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
    // Early boot or WiFi-uninitialized path. Don't leave garbage in out[],
    // and keep nothing: the next call asks again.
    memset(out, 0, 6);
    return false;
  }
  memcpy(out, mac, sizeof(mac));
  static const uint8_t kNoMac[6] = {0, 0, 0, 0, 0, 0};
  if (memcmp(mac, kNoMac, sizeof(mac)) != 0) {
    // An all-zero answer is handed on as before but is not an address to keep.
    portENTER_CRITICAL(&sKeptLock);
    memcpy(sKept, mac, sizeof(sKept));
    sKeptValid = true;
    portEXIT_CRITICAL(&sKeptLock);
  }
  return true;
}

String macString() {
  uint8_t m[6] = {0};
  // If macBytes fails the buffer is already zeroed by the macBytes contract,
  // so we still produce a deterministic "00:00:00:00:00:00" instead of an
  // empty string — callers can format without NULL-checking.
  macBytes(m);
  char buf[18];
  macToDisplay(m, buf, sizeof(buf));
  return String(buf);
}

// The name in use into `out`; returns whether it is the stored name (true)
// or the fallback (false). No String, no heap: the display helpers below and
// deviceName() share it.
static bool nameInUse(char (&out)[devicename::kFieldLen]) {
  const char* stored = gSettings.espnowDeviceName.c_str();
  const devicename::Fault fault = devicename::check(stored);
  if (fault == devicename::Fault::None) {
    strlcpy(out, stored, sizeof(out));
    return true;
  }
  if (stored[0]) {
    static bool sWarned = false;
    if (!sWarned) {
      sWarned = true;
      WARN_SYSTEMF("[Device] stored device name is not valid (%s); using the fallback. Set it with 'devicename <name>'.",
                   devicename::faultText(fault));
    }
  }
  uint8_t mac[6] = {0};
  (void)esp_efuse_mac_get_default(mac);
  devicename::formatWithMacSuffix(devicename::kFallbackPrefix, mac, out);
  return false;
}

String deviceName() {
  if (devicename::valid(gSettings.espnowDeviceName.c_str())) return gSettings.espnowDeviceName;
  char name[devicename::kFieldLen];
  (void)nameInUse(name);
  return String(name);
}

bool deviceNameSet() { return devicename::valid(gSettings.espnowDeviceName.c_str()); }

bool deviceNameDisplay(char* out, size_t cap) {
  char name[devicename::kFieldLen];
  const bool set = nameInUse(name);
  devicename::displayText(name, set, out, cap);
  return set;
}

void applyHostname() {
#if ENABLE_WIFI
  const String name = deviceName();
  WiFi.setHostname(name.c_str());
  if (WiFi.STA.started()) (void)WiFi.STA.setHostname(name.c_str());
#endif
}

const char* firmwareVersion() {
  const esp_app_desc_t* desc = esp_app_get_description();
  if (!desc || !desc->version[0]) return "unknown";
  return desc->version;
}

} // namespace SelfDevice
