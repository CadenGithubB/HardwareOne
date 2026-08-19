#include "System_SelfDevice.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>

#include "System_Settings.h"  // gSettings.espnowDeviceName

namespace SelfDevice {

uint32_t uptimeSeconds() {
  // millis() returns unsigned long (uint32_t on ESP-IDF). Divide by 1000 for
  // seconds. Rolls over at ~49.7 days — every other caller had this same
  // behavior so no behavior change.
  return (uint32_t)(millis() / 1000UL);
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
  return (uint32_t)esp_get_minimum_free_heap_size();
}

bool macBytes(uint8_t out[6]) {
  if (!out) return false;
  esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, out);
  if (err != ESP_OK) {
    // Early boot or WiFi-uninitialized path. Don't leave garbage in out[].
    memset(out, 0, 6);
    return false;
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
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

String deviceName() {
  // Settings name is the user-set label. Fall back to MAC so the result is
  // always non-empty and uniquely identifies the device on the network.
  if (gSettings.espnowDeviceName.length() > 0) {
    return gSettings.espnowDeviceName;
  }
  return macString();
}

const char* firmwareVersion() {
  const esp_app_desc_t* desc = esp_app_get_description();
  if (!desc || !desc->version[0]) return "unknown";
  return desc->version;
}

} // namespace SelfDevice
