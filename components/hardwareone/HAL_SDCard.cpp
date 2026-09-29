#include "HAL_SDCard.h"

#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
#if !HW_BOARD_P4X_EYE || !CONFIG_IDF_TARGET_ESP32P4
#error "The SDMMC backend requires the ESP32-P4X-EYE board profile"
#endif

#include <Arduino.h>
#include "vfs_api.h"
#include "driver/gpio.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "ff.h"
#include "diskio_sdmmc.h"

namespace {
constexpr char kMountPoint[] = "/sd";
constexpr char kTag[] = "SDMMC";
static_assert(SD_MMC_SLOT == 0, "Slot 1 is reserved for ESP-Hosted on P4X-EYE");
}

SdMmcCardFS gSdMmcCard;

SdMmcCardFS::SdMmcCardFS() : FS(fs::FSImplPtr(new VFSImpl())) {}

bool SdMmcCardFS::configurePower() {
  gpio_config_t pins = {};
  pins.pin_bit_mask = 1ULL << SD_MMC_POWER_PIN;
  pins.mode = GPIO_MODE_OUTPUT;
  // Preload the off level before enabling the output to avoid a power glitch.
  error_ = gpio_set_level(static_cast<gpio_num_t>(SD_MMC_POWER_PIN),
                         !SD_MMC_POWER_ACTIVE_LEVEL);
  if (error_ != ESP_OK) return false;
  error_ = gpio_config(&pins);
  if (error_ != ESP_OK) return false;
  pinsConfigured_ = true;

  pins = {};
  pins.pin_bit_mask = 1ULL << SD_MMC_DETECT_PIN;
  pins.mode = GPIO_MODE_INPUT;
  pins.pull_up_en = GPIO_PULLUP_ENABLE;
  error_ = gpio_config(&pins);
  if (error_ != ESP_OK) return false;

  sd_pwr_ctrl_ldo_config_t ldo = {};
  ldo.ldo_chan_id = SD_MMC_LDO_CHANNEL;
  error_ = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &power_);
  if (error_ != ESP_OK) return false;
  delay(20);
  error_ = gpio_set_level(static_cast<gpio_num_t>(SD_MMC_POWER_PIN),
                         SD_MMC_POWER_ACTIVE_LEVEL);
  if (error_ != ESP_OK) return false;
  delay(20);
  return true;
}

void SdMmcCardFS::releasePower() {
  if (pinsConfigured_) {
    gpio_set_level(static_cast<gpio_num_t>(SD_MMC_POWER_PIN),
                   !SD_MMC_POWER_ACTIVE_LEVEL);
  }
  if (power_) {
    sd_pwr_ctrl_del_on_chip_ldo(power_);
    power_ = nullptr;
  }
}

bool SdMmcCardFS::cardPresent() const {
  return pinsConfigured_ &&
      gpio_get_level(static_cast<gpio_num_t>(SD_MMC_DETECT_PIN)) == 0;
}

bool SdMmcCardFS::available() {
  if (card_ && !cardPresent()) removed_ = true;
  return card_ && !removed_;
}

bool SdMmcCardFS::begin() {
  return mount(false);
}

bool SdMmcCardFS::mount(bool allowFormat) {
  if (card_) return available();
  error_ = ESP_OK;
  removed_ = false;
  if (!configurePower()) {
    ESP_LOGW(kTag, "Card power setup failed: %s", lastError());
    releasePower();
    return false;
  }
  if (!cardPresent()) {
    error_ = ESP_ERR_NOT_FOUND;
    ESP_LOGI(kTag, "No card inserted");
    releasePower();
    return false;
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SD_MMC_SLOT;
  host.max_freq_khz = SD_MMC_MAX_FREQ_KHZ;
  host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_1BIT |
               SDMMC_HOST_FLAG_DEINIT_ARG;
  // Never call sdmmc_host_deinit(): that would tear down the C6's slot too.
  // FatFs also invokes this callback on a failed card/FAT mount.
  host.deinit_p = sdmmc_host_deinit_slot;
  host.pwr_ctrl_handle = power_;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = static_cast<gpio_num_t>(SD_MMC_CLK_PIN);
  slot.cmd = static_cast<gpio_num_t>(SD_MMC_CMD_PIN);
  slot.d0 = static_cast<gpio_num_t>(SD_MMC_D0_PIN);
  slot.d1 = static_cast<gpio_num_t>(SD_MMC_D1_PIN);
  slot.d2 = static_cast<gpio_num_t>(SD_MMC_D2_PIN);
  slot.d3 = static_cast<gpio_num_t>(SD_MMC_D3_PIN);
  slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
  slot.cd = static_cast<gpio_num_t>(SD_MMC_DETECT_PIN);
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.width = 4;
  slot.flags = 0;  // board has external bus pull-ups; stay at 3.3 V SDR

  esp_vfs_fat_sdmmc_mount_config_t config = {};
  config.format_if_mount_failed = allowFormat;
  config.max_files = 10;
  config.allocation_unit_size = 16 * 1024;
  config.disk_status_check_enable = true;
  error_ = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &config, &card_);
  if (error_ != ESP_OK) {
    card_ = nullptr;
    ESP_LOGW(kTag, "Mount failed: %s (automatic format %s)",
             lastError(), allowFormat ? "explicitly requested" : "disabled");
    releasePower();
    return false;
  }
  _impl->mountpoint(kMountPoint);
  ESP_LOGI(kTag, "Mounted SDMMC slot %d, 4-bit, max %d kHz",
           SD_MMC_SLOT, SD_MMC_MAX_FREQ_KHZ);
  return true;
}

bool SdMmcCardFS::end() {
  bool ok = true;
  if (card_) {
    error_ = esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    ok = error_ == ESP_OK;
    // IDF consumes the card before unregistering the VFS path. Even if that
    // final unregister reports an error, this pointer must never be reused.
    card_ = nullptr;
    _impl->mountpoint(nullptr);
  }
  removed_ = false;
  releasePower();
  return ok;
}

bool SdMmcCardFS::format() {
  if (!end() || !mount(true)) return false;
  // mount(true) repairs an unformatted card; this explicit operation must
  // also erase an already valid volume. Normal begin() never reaches here.
  // Own the FatFs transition ourselves: IDF 5.5's sdcard_format helper can
  // free its card on remount failure without clearing its saved VFS context.
  // It also chooses FM_ANY, whereas this command promises FAT32.
  const BYTE drive = ff_diskio_get_pdrv_card(card_);
  char driveName[] = {static_cast<char>('0' + drive), ':', '\0'};
  FATFS* volume = nullptr;
  DWORD freeClusters = 0;
  if (drive == 0xff || f_getfree(driveName, &freeClusters, &volume) != FR_OK ||
      f_mount(nullptr, driveName, 0) != FR_OK) {
    end();
    error_ = ESP_FAIL;
    return false;
  }
  // SD cards expose 512-byte logical sectors. One sector is the documented
  // minimum f_mkfs workspace; keep the command-task stack bounded.
  BYTE work[512];
  const MKFS_PARM options = {FM_FAT32, 2, 0, 0, 16 * 1024};
  const FRESULT formatted = f_mkfs(driveName, &options, work, sizeof(work));
  const FRESULT remounted = f_mount(volume, driveName, 1);
  if (formatted != FR_OK || remounted != FR_OK) {
    end();
    error_ = ESP_FAIL;
    return false;
  }
  error_ = ESP_OK;
  return true;
}

uint8_t SdMmcCardFS::cardType() const {
  if (!card_) return CARD_NONE;
  return (card_->ocr & SD_OCR_SDHC_CAP) ? CARD_SDHC : CARD_SD;
}

uint64_t SdMmcCardFS::totalBytes() const {
  uint64_t total = 0, free = 0;
  if (!card_ || esp_vfs_fat_info(kMountPoint, &total, &free) != ESP_OK) return 0;
  return total;
}

uint64_t SdMmcCardFS::usedBytes() const {
  uint64_t total = 0, free = 0;
  if (!card_ || esp_vfs_fat_info(kMountPoint, &total, &free) != ESP_OK) return 0;
  return total >= free ? total - free : 0;
}

const char* SdMmcCardFS::lastError() const {
  return esp_err_to_name(error_);
}
#endif
