#pragma once
#include <cstdint>
#include <memory>
#include <string>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NOT_FOUND=2;
using gpio_num_t = int;
constexpr int GPIO_NUM_NC=-1, GPIO_MODE_OUTPUT=1, GPIO_MODE_INPUT=2, GPIO_PULLUP_ENABLE=1;
struct gpio_config_t { uint64_t pin_bit_mask=0; int mode=0, pull_up_en=0; };
esp_err_t gpio_set_level(gpio_num_t, int);
esp_err_t gpio_config(const gpio_config_t*);
int gpio_get_level(gpio_num_t);
void delay(unsigned);
const char* esp_err_to_name(esp_err_t);
using sd_pwr_ctrl_handle_t = void*;
struct sd_pwr_ctrl_ldo_config_t { int ldo_chan_id; };
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t*, sd_pwr_ctrl_handle_t*);
esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t);
constexpr int SDMMC_HOST_FLAG_4BIT=1, SDMMC_HOST_FLAG_1BIT=2, SDMMC_HOST_FLAG_DEINIT_ARG=4;
constexpr int SDMMC_SLOT_NO_WP=-1, SD_OCR_SDHC_CAP=8;
constexpr int CARD_NONE=0, CARD_SD=2, CARD_SDHC=3;
struct sdmmc_host_t { int slot=1, max_freq_khz=0, flags=0; esp_err_t (*deinit_p)(int)=nullptr; sd_pwr_ctrl_handle_t pwr_ctrl_handle=nullptr; };
struct sdmmc_slot_config_t { int clk=-1,cmd=-1,d0=-1,d1=-1,d2=-1,d3=-1,d4=-1,d5=-1,d6=-1,d7=-1,cd=-1,wp=-1,width=0,flags=0; };
struct sdmmc_card_t { int ocr=SD_OCR_SDHC_CAP; };
#define SDMMC_HOST_DEFAULT() {}
#define SDMMC_SLOT_CONFIG_DEFAULT() {}
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed=false; int max_files=0, allocation_unit_size=0; bool disk_status_check_enable=false; };
esp_err_t sdmmc_host_deinit_slot(int);
esp_err_t esp_vfs_fat_sdmmc_mount(const char*, const sdmmc_host_t*, const sdmmc_slot_config_t*, const esp_vfs_fat_sdmmc_mount_config_t*, sdmmc_card_t**);
esp_err_t esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t*);
esp_err_t esp_vfs_fat_sdcard_format(const char*, sdmmc_card_t*);
esp_err_t esp_vfs_fat_info(const char*, uint64_t*, uint64_t*);
using BYTE = uint8_t;
using DWORD = uint32_t;
using FRESULT = int;
constexpr int FR_OK = 0, FM_FAT32 = 2;
struct FATFS {};
struct MKFS_PARM { BYTE fmt, n_fat; unsigned align, n_root, au_size; };
BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t*);
FRESULT f_getfree(const char*, DWORD*, FATFS**);
FRESULT f_mount(FATFS*, const char*, int);
FRESULT f_mkfs(const char*, const MKFS_PARM*, void*, unsigned);
namespace fs {
struct FSImpl { virtual ~FSImpl()=default; std::string path; void mountpoint(const char* p) { path=p?p:""; } };
using FSImplPtr = std::shared_ptr<FSImpl>;
class FS { protected: FSImplPtr _impl; public: explicit FS(FSImplPtr impl):_impl(impl){} };
}
class VFSImpl: public fs::FSImpl {};
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
