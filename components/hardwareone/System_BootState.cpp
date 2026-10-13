#include "System_BootState.h"
#include "System_Debug.h"   // ERROR_SYSTEMF / DEBUG_SYSTEMF
#include "nvs_flash.h"
#include "nvs.h"

// One tiny namespace holds all cross-power-cycle boot state. Keys are <=15 chars
// (NVS limit). Kept deliberately separate from any file-backed store.
static const char* kNvsNamespace = "bootstate";
static const char* kKeyBootCount = "bootcount";
static const char* kKeyAccountIdHighWater = "acctidhw";

static bool sNvsReady = false;

void bootStateInit() {
  if (sNvsReady) return;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS is shared with WiFi credentials, BLE bonds, OTA state, and
    // capture-sealing material. Automatic bulk erase would turn a
    // recoverable version/capacity problem into permanent data loss, so fail
    // closed on every build and leave repair to a narrow, explicit tool.
    ERROR_SYSTEMF("[BootState] NVS needs recovery (0x%x) — preserving partition; automatic erase is forbidden", err);
    return;
  }
  if (err != ESP_OK) {
    // Non-fatal: accessors degrade to returning 0 / no-op. The device still
    // boots; only the persistent boot counter is unavailable.
    ERROR_SYSTEMF("[BootState] nvs_flash_init failed: 0x%x", err);
    return;
  }
  sNvsReady = true;
}

static uint32_t readCountLocked() {
  nvs_handle_t h;
  // NVS_READONLY open fails with NOT_FOUND until the namespace exists (first
  // boot); treat that as 0 rather than an error.
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
  uint32_t v = 0;
  nvs_get_u32(h, kKeyBootCount, &v);  // leaves v untouched (0) on NOT_FOUND
  nvs_close(h);
  return v;
}

static void writeCount(uint32_t v) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ERROR_SYSTEMF("[BootState] nvs_open(rw) failed: 0x%x", err);
    return;
  }
  if (nvs_set_u32(h, kKeyBootCount, v) == ESP_OK) {
    nvs_commit(h);
  }
  nvs_close(h);
}

uint32_t bootStateIncrementBootCount() {
  bootStateInit();
  if (!sNvsReady) return 0;
  uint32_t next = readCountLocked() + 1;
  writeCount(next);
  DEBUG_SYSTEMF("[BootState] boot count -> %lu", (unsigned long)next);
  return next;
}

uint32_t bootStateGetBootCount() {
  bootStateInit();
  if (!sNvsReady) return 0;
  return readCountLocked();
}

void bootStateResetBootCount() {
  bootStateInit();
  if (!sNvsReady) return;
  writeCount(0);
  DEBUG_SYSTEMF("[BootState] boot count reset to 0");
}

// A missing key reads as 0; any other error is a failed read.
static bool readAccountIdHighWater(nvs_handle_t h, uint32_t& out) {
  uint32_t v = 0;
  const esp_err_t err = nvs_get_u32(h, kKeyAccountIdHighWater, &v);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    v = 0;
  } else if (err != ESP_OK) {
    return false;
  }
  out = v;
  return true;
}

bool bootStateGetAccountIdHighWater(uint32_t& out) {
  bootStateInit();
  if (!sNvsReady) return false;
  nvs_handle_t h;
  // The namespace does not exist until its first write.
  const esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    out = 0;
    return true;
  }
  if (err != ESP_OK) return false;
  const bool ok = readAccountIdHighWater(h, out);
  nvs_close(h);
  return ok;
}

bool bootStateRaiseAccountIdHighWater(uint32_t id) {
  bootStateInit();
  if (!sNvsReady) return false;
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ERROR_SYSTEMF("[BootState] nvs_open(rw) failed: 0x%x", err);
    return false;
  }
  uint32_t current = 0;
  bool ok = readAccountIdHighWater(h, current);
  if (ok && current < id) {
    err = nvs_set_u32(h, kKeyAccountIdHighWater, id);
    if (err == ESP_OK) err = nvs_commit(h);
    ok = (err == ESP_OK);
    if (!ok) ERROR_SYSTEMF("[BootState] account id mark write failed: 0x%x", err);
  }
  nvs_close(h);
  return ok;
}
