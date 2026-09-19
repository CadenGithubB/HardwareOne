# Pocket Assistant - XIAO ESP32-S3 cable migration

This layout **cannot be installed as an ordinary OTA update.** The stock XIAO
image has no factory recovery slot at all: it is a single 5,844 KiB `factory`
partition with LittleFS at `0x5C5000`. The Pocket Assistant layout introduces a
golden recovery updater, an `otadata` journal and an `ota_0` application slot,
and moves LittleFS to `0x6D0000`.

Install it once over USB. After that, every firmware update is a signed OTA
into `ota_0` and this procedure is never needed again.

## Before you start

- **LittleFS is destroyed.** Its start address moves and a filesystem cannot
  grow or slide in place. Back up device files first and verify the backup.
- **NVS is preserved.** It stays at `0xA000`, so recovery credentials, Wi-Fi
  credentials and device-bound secrets survive. Do **not** add `erase-flash`
  to this procedure - that would destroy them.
- The microSD card is untouched; it is not part of the flash layout.

## Layout

| Partition  | Offset      | Size        |
|------------|-------------|-------------|
| `nvs`      | `0xA000`    | 16 KiB      |
| `nvs_key`  | `0xE000`    | 4 KiB       |
| `phy_init` | `0xF000`    | 4 KiB       |
| `factory`  | `0x10000`   | 1,144 KiB   |
| `otadata`  | `0x12E000`  | 8 KiB       |
| `ota_0`    | `0x130000`  | 5,760 KiB   |
| `littlefs` | `0x6D0000`  | 1,216 KiB   |

The partition table itself lives at `0x9000`, not the XIAO's usual `0x8000` - 
`config/sdkconfig.ota.defaults` moves it for every recovery-OTA build. Flash
the table to `0x9000` or the device will boot the old map.

## Install

```bash
source "$IDF_PATH/export.sh"
HW1_OTA_SIGNING_KEY=/absolute/path/to/ota-signing-key.pem \
  tools/build_deployment.sh pocket_assistant xiao_s3
```

Then, from `build/deployments/pocket_assistant/xiao_s3/release/`, with the
board in bootloader mode and `ESPPORT` naming the correct device:

```bash
python -m esptool --chip esp32s3 -p "$ESPPORT" -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size keep --flash_freq 80m 0x0 bootloader.bin 0x9000 partition-table.bin 0x10000 factory-updater.bin 0x12E000 ota-data-initial.bin 0x130000 firmware.bin 0x6D0000 littlefs.bin
```

`littlefs.bin` is a blank filesystem image and writing it is **required**, not
optional. `initFilesystem()` calls `LittleFS.begin(formatOnFail=false)` on
purpose, so a freshly relocated partition has nothing mountable and the
firmware halts at `LittleFS mount FAILED - data partition preserved`. No
firmware in this project can format its own storage; only the host toolchain
can seed it.

Never use `idf.py flash` on an OTA-layout board - it writes the application to
`factory`, which here holds the recovery updater.

## After the first boot

1. Restore your backed-up files.
2. Confirm the recovery updater is reachable before relying on OTA.
3. `ota status` should report layout `hw1-pa-xiao-ota-v1` and a version ending
   in `+xiaopa1`. Anything else means the wrong artifacts were flashed.
