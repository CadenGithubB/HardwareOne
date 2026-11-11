#include "System_FactoryWipe.h"

#include <Arduino.h>
#include <string.h>

#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "System_FactoryWipePolicy.h"
#include "System_BootState.h"
#include "System_Filesystem.h"  // kLittleFsPartitionLabel
#include "System_OTASafety.h"   // otaSafetyEraseNvsForFactoryWipe

namespace {

// The request has a namespace of its own so the entry clear below can spare
// it by name. The value is 1 + the number of boots that have started the wipe.
const char* const kWipeNamespace = "hw1wipe";
const char* const kWipeKey = "armed";

const char* sBootReport = nullptr;

bool readRequest(uint8_t& value) {
  nvs_handle_t handle;
  if (nvs_open(kWipeNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t stored = 0;
  const esp_err_t err = nvs_get_u8(handle, kWipeKey, &stored);
  nvs_close(handle);
  if (err != ESP_OK || stored == 0) return false;
  value = stored;
  return true;
}

bool writeRequest(uint8_t value) {
  nvs_handle_t handle;
  if (nvs_open(kWipeNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t err = nvs_set_u8(handle, kWipeKey, value);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  uint8_t stored = 0;
  return err == ESP_OK && readRequest(stored) && stored == value;
}

void eraseRequest() {
  nvs_handle_t handle;
  if (nvs_open(kWipeNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  if (nvs_erase_all(handle) == ESP_OK) (void)nvs_commit(handle);
  nvs_close(handle);
}

// Deletes every entry outside the request's namespace. Names are collected
// first: erasing while the iterator is open is not allowed.
bool clearOtherNamespaces() {
  constexpr size_t kBatch = 16;
  for (int pass = 0; pass < 8; ++pass) {
    char names[kBatch][NVS_NS_NAME_MAX_SIZE];
    size_t count = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && count < kBatch) {
      nvs_entry_info_t info;
      if (nvs_entry_info(it, &info) == ESP_OK && strcmp(info.namespace_name, kWipeNamespace) != 0) {
        bool seen = false;
        for (size_t i = 0; i < count && !seen; ++i) seen = strcmp(names[i], info.namespace_name) == 0;
        if (!seen) strlcpy(names[count++], info.namespace_name, NVS_NS_NAME_MAX_SIZE);
      }
      err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    if (count == 0) return true;
    for (size_t i = 0; i < count; ++i) {
      nvs_handle_t handle;
      if (nvs_open(names[i], NVS_READWRITE, &handle) != ESP_OK) return false;
      esp_err_t cleared = nvs_erase_all(handle);
      if (cleared == ESP_OK) cleared = nvs_commit(handle);
      nvs_close(handle);
      if (cleared != ESP_OK) return false;
    }
  }
  return false;
}

struct FlashOps {
  uint32_t accountIdMark = 0;
  bool haveAccountIdMark = false;

  bool armed(uint8_t& attempts) {
    uint8_t value = 0;
    if (!readRequest(value)) return false;
    attempts = static_cast<uint8_t>(value - 1);
    return true;
  }
  bool recordAttempt(uint8_t count) { return writeRequest(static_cast<uint8_t>(count + 1)); }
  void disarm() { eraseRequest(); }

  bool eraseFilesystem() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLittleFsPartitionLabel);
    if (!part) {
      Serial.println("[WIPE] filesystem partition not found");
      return false;
    }
    Serial.printf("[WIPE] erasing the filesystem partition (%lu KB); this can take a few minutes\n",
                  static_cast<unsigned long>(part->size / 1024));
    constexpr size_t kChunk = 64 * 1024;       // a multiple of the 4 KB erase sector
    constexpr size_t kProgressEvery = 1024 * 1024;
    for (size_t offset = 0; offset < part->size; offset += kChunk) {
      const size_t length = (part->size - offset < kChunk) ? part->size - offset : kChunk;
      const esp_err_t err = esp_partition_erase_range(part, offset, length);
      if (err != ESP_OK) {
        Serial.printf("[WIPE] erase failed at offset 0x%lx: 0x%x\n",
                      static_cast<unsigned long>(offset), static_cast<unsigned>(err));
        return false;
      }
      if (offset && offset % kProgressEvery == 0) {
        Serial.printf("[WIPE] %lu of %lu KB\n", static_cast<unsigned long>(offset / 1024),
                      static_cast<unsigned long>(part->size / 1024));
      }
      delay(1);  // let the idle task run between blocks
    }
    return true;
  }

  bool clearNvsEntries() {
    haveAccountIdMark = bootStateGetAccountIdHighWater(accountIdMark);
    const bool cleared = clearOtherNamespaces();
    restoreAccountIdMark();
    return cleared;
  }

  bool eraseNvs() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NVS_DEFAULT_PART_NAME);
    if (!part) {
      Serial.println("[WIPE] NVS partition not found");
      return false;
    }
    (void)nvs_flash_deinit();
    const esp_err_t erased = otaSafetyEraseNvsForFactoryWipe(part);
    const esp_err_t ready = nvs_flash_init();
    if (erased != ESP_OK || ready != ESP_OK) {
      Serial.printf("[WIPE] NVS erase 0x%x, init 0x%x\n",
                    static_cast<unsigned>(erased), static_cast<unsigned>(ready));
    }
    if (ready == ESP_OK) restoreAccountIdMark();
    return erased == ESP_OK;
  }

  void restoreAccountIdMark() {
    if (haveAccountIdMark && accountIdMark) (void)bootStateRaiseAccountIdHighWater(accountIdMark);
  }
};

}  // namespace

bool factoryWipeArm() {
  bootStateInit();  // NVS up; idempotent
  return writeRequest(1);
}

void factoryWipeDisarm() {
  bootStateInit();
  eraseRequest();
}

void factoryWipeRunIfArmed() {
  bootStateInit();
  FlashOps ops;
  const FactoryWipeResult result = factoryWipeRun(ops);
  sBootReport = factoryWipeResultText(result);
  if (sBootReport) Serial.printf("[WIPE] %s\n", sBootReport);
}

const char* factoryWipeBootReport() { return sBootReport; }
