// Exercise the production transport with failed SDK calls and no hardware.
// c++ -std=c++17 -Isdmmc_stubs test_sdmmc_lifecycle.cpp -o /tmp/sdmmc-test
#define SYSTEM_BUILDCONFIG_H
#define CONFIG_IDF_TARGET_ESP32P4 1
#define HW_BOARD_P4X_EYE 1
#define ENABLE_SDMMC_CARD 1
#define ENABLE_SD_CARD 1
#include "../../System_Board_P4X_EYE.h"
#include "../../HAL_SDCard.cpp"
#include <cassert>
#include <vector>

namespace {
bool inserted = true;
int powerLevel = 1, ldoCount = 0, mounts = 0, formats = 0;
esp_err_t mountResult = ESP_OK, ldoResult = ESP_OK, formatResult = ESP_OK;
esp_err_t unmountResult = ESP_OK;
FRESULT fatRemountResult = FR_OK;
FATFS volume;
sdmmc_card_t card;
sdmmc_host_t capturedHost;
bool capturedFormat = false;
std::vector<std::string> calls;
}

void delay(unsigned) {}
const char* esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "OK" : "error"; }
esp_err_t gpio_set_level(gpio_num_t pin, int level) {
  assert(pin == SD_MMC_POWER_PIN);
  powerLevel = level;
  calls.push_back(level ? "power-off" : "power-on");
  return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 45); return inserted ? 0 : 1; }
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t* c, sd_pwr_ctrl_handle_t* out) {
  assert(c->ldo_chan_id == 4);
  if (ldoResult != ESP_OK) return ldoResult;
  ++ldoCount;
  *out = &card;
  return ESP_OK;
}
esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t p) {
  assert(p == &card && powerLevel == 1);
  --ldoCount;
  calls.push_back("release-ldo");
  return ESP_OK;
}
esp_err_t sdmmc_host_deinit_slot(int slot) {
  assert(slot == 0);  // slot 1 belongs to the radio, including mount failures
  calls.push_back("release-slot");
  return ESP_OK;
}
esp_err_t esp_vfs_fat_sdmmc_mount(const char* path, const sdmmc_host_t* h,
    const sdmmc_slot_config_t* s, const esp_vfs_fat_sdmmc_mount_config_t* c,
    sdmmc_card_t** out) {
  ++mounts;
  assert(std::string(path) == "/sd" && powerLevel == 0 && ldoCount == 1);
  assert(h->slot == 0 && h->max_freq_khz == 40000);
  assert(h->flags & SDMMC_HOST_FLAG_DEINIT_ARG);
  assert(h->deinit_p == sdmmc_host_deinit_slot && h->pwr_ctrl_handle == &card);
  assert(s->width == 4 && s->clk == 43 && s->cmd == 44);
  assert(s->d0 == 39 && s->d1 == 40 && s->d2 == 41 && s->d3 == 42);
  assert(s->cd == 45 && s->d4 == -1 && s->d5 == -1 && s->d6 == -1 && s->d7 == -1);
  capturedHost = *h;
  capturedFormat = c->format_if_mount_failed;
  if (mountResult == ESP_OK) *out = &card;
  else h->deinit_p(h->slot); // FatFs failure unwind must leave radio intact
  return mountResult;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t* c) {
  assert(c == &card && ldoCount == 1 && powerLevel == 0);
  calls.push_back("unmount");
  capturedHost.deinit_p(capturedHost.slot);
  return unmountResult;
}
esp_err_t esp_vfs_fat_sdcard_format(const char*, sdmmc_card_t*) {
  assert(false); // the IDF helper has ambiguous ownership on failure
  return ESP_FAIL;
}
BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t*) { return 1; }
FRESULT f_getfree(const char* path, DWORD* free, FATFS** out) {
  assert(std::string(path) == "1:"); // no hardcoded drive 0 assumption
  *free = 1; *out = &volume;
  return FR_OK;
}
FRESULT f_mount(FATFS* fs, const char*, int) {
  if (!fs) return FR_OK;
  assert(fs == &volume);
  return fatRemountResult;
}
FRESULT f_mkfs(const char*, const MKFS_PARM* options, void*, unsigned len) {
  assert(options->fmt == FM_FAT32 && len >= 512);
  ++formats;
  return formatResult;
}
esp_err_t esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free) {
  assert(std::string(path) == "/sd");
  *total = 1000; *free = 300;
  return ESP_OK;
}

int main() {
  // Empty slot is optional: no FAT/host mount, no retained LDO or card power.
  inserted = false;
  assert(!gSdMmcCard.begin() && mounts == 0 && ldoCount == 0 && powerLevel == 1);
  inserted = true;
  ldoResult = ESP_FAIL;
  assert(!gSdMmcCard.begin() && mounts == 0 && ldoCount == 0 && powerLevel == 1);
  ldoResult = ESP_OK;

  // A corrupt/unformatted card is preserved; repeated failures release power.
  mountResult = ESP_FAIL;
  for (int i = 0; i != 3; ++i) {
    assert(!gSdMmcCard.begin() && !capturedFormat && formats == 0);
    assert(ldoCount == 0 && powerLevel == 1);
  }
  mountResult = ESP_OK;
  assert(gSdMmcCard.begin() && gSdMmcCard.available());
  assert(!capturedFormat && formats == 0);
  assert(gSdMmcCard.totalBytes() == 1000 && gSdMmcCard.usedBytes() == 700);
  const int mountedCount = mounts;
  assert(gSdMmcCard.begin() && mounts == mountedCount); // idempotent

  // An observed removal stays unavailable after reinsertion until remount.
  inserted = false;
  assert(!gSdMmcCard.available());
  inserted = true;
  assert(!gSdMmcCard.available());
  calls.clear();
  assert(gSdMmcCard.end());
  assert((calls == std::vector<std::string>{"unmount", "release-slot", "power-off", "release-ldo"}));
  assert(ldoCount == 0 && gSdMmcCard.begin() && gSdMmcCard.available());

  // Only the explicit format entry permits format-on-mount and erases a valid volume.
  assert(gSdMmcCard.format() && capturedFormat && formats == 1);
  formatResult = ESP_FAIL;
  assert(!gSdMmcCard.format());
  assert(!gSdMmcCard.available() && ldoCount == 0 && powerLevel == 1);
  assert(gSdMmcCard.end() && ldoCount == 0); // idempotent failure cleanup
  formatResult = ESP_OK;
  fatRemountResult = ESP_FAIL;
  assert(!gSdMmcCard.format()); // successful erase but failed FAT remount
  assert(!gSdMmcCard.available() && ldoCount == 0 && powerLevel == 1);
  fatRemountResult = FR_OK;
  assert(gSdMmcCard.begin());
  unmountResult = ESP_FAIL; // SDK has consumed card before unregister error
  assert(!gSdMmcCard.end() && ldoCount == 0 && !gSdMmcCard.available());
  assert(gSdMmcCard.end()); // no second SDK unmount of the consumed pointer
}
