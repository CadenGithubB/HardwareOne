#pragma once

#include "System_BuildConfig.h"

#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
#include <FS.h>
#include <sd_defines.h>
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl.h"

// The SDMMC transport belongs to VFS. Callers use VFS::*Guarded, just as they
// do for the SPI card. The methods below require the VFS filesystem lock,
// except cardPresent(), removalLatched() and noteRemoved(): those read the
// detect switch or one flag and are what the card watch (VFS::sdWatchTick,
// UP-06) calls from the main loop without the lock.
// Keeping this separate from Arduino SD_MMC lets slot 0 coexist with the
// ESP-Hosted C6 on slot 1 without relying on Arduino variant-global settings.
class SdMmcCardFs final : public fs::FS {
 public:
  SdMmcCardFs();
  bool begin();                     // mount existing FAT; never auto-format
  bool end();                       // release slot, then card power / LDO
  bool format();                    // destructive, explicit VFS command only
  bool available();                // latches removal until the next mount
  bool cardPresent() const;        // the detect switch (lock-free)
  // UP-06, lock-free. A mount exists and its card was seen to leave: the
  // mount is stale until end() and a new begin().
  bool removalLatched() const { return card_ != nullptr && removed_; }
  // The card watch saw the switch open while this mount existed: latch the
  // removal now, as available() does when a caller next asks.
  void noteRemoved() { if (card_ != nullptr) removed_ = true; }
  uint8_t cardType() const;
  uint64_t totalBytes() const;
  uint64_t usedBytes() const;
  const char* lastError() const;

 private:
  bool mount(bool allowFormat);
  bool configurePower();
  void releasePower();
  esp_err_t armDetectPin();        // detect switch as input + pull-up; again after each slot release
  sdmmc_card_t* card_ = nullptr;
  sd_pwr_ctrl_handle_t power_ = nullptr;
  esp_err_t error_ = ESP_OK;
  bool pinsConfigured_ = false;
  bool removed_ = false;
};

extern SdMmcCardFs gSdMmcCard;
#endif
