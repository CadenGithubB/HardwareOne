#pragma once

#include "System_BuildConfig.h"

#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
#include <FS.h>
#include <sd_defines.h>
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl.h"

// The SDMMC transport belongs to VFS. Callers use VFS::*Guarded, just as they
// do for the SPI card. All methods below require the VFS filesystem lock.
// Keeping this separate from Arduino SD_MMC lets slot 0 coexist with the
// ESP-Hosted C6 on slot 1 without relying on Arduino variant-global settings.
class SdMmcCardFS final : public fs::FS {
 public:
  SdMmcCardFS();
  bool begin();                     // mount existing FAT; never auto-format
  bool end();                       // release slot, then card power / LDO
  bool format();                    // destructive, explicit VFS command only
  bool available();                // latches removal until an explicit remount
  bool cardPresent() const;
  uint8_t cardType() const;
  uint64_t totalBytes() const;
  uint64_t usedBytes() const;
  const char* lastError() const;

 private:
  bool mount(bool allowFormat);
  bool configurePower();
  void releasePower();
  sdmmc_card_t* card_ = nullptr;
  sd_pwr_ctrl_handle_t power_ = nullptr;
  esp_err_t error_ = ESP_OK;
  bool pinsConfigured_ = false;
  bool removed_ = false;
};

extern SdMmcCardFS gSdMmcCard;
#endif
