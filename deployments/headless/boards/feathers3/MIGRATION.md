# Headless FeatherS3[D] cable provisioning

The release directory contains a complete first-provisioning set for the plain
`feathers3` target. It covers Unexpected Maker FeatherS3 and FeatherS3[D]. Do
not use the `feathers3_fe` target unless flash encryption was intentionally
enabled for that physical board.

## Optional complete erase

Skip this step when preserving NVS is desired. It irreversibly removes every
old app, setting, credential, and filesystem block from the selected board:

```sh
esptool.py --chip esp32s3 --port /dev/cu.YOUR_PORT erase_flash
```

Resolve and verify the exact USB port before running it. A complete erase also
removes the recovery credential, so `setpin` must be run in the factory updater
after provisioning.

## Install the complete layout

The guarded repository target rechecks updater/main identity and requires the
deployment-bound confirmation phrase immediately before writing:

```sh
HW_DEPLOYMENT=headless/feathers3 \
  HW_BOARD=feathers3 HW_OTA_LAYOUT=1 \
  HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  HW1_UPDATER_BIN="$PWD/build/deployments/headless/feathers3/updater/hw1-updater.bin" \
  HW1_OTA_MIGRATION_CONFIRM="MIGRATE headless/feathers3 hw1-hl-f3-ota-v1" \
  ESPPORT=/dev/cu.YOUR_PORT \
  idf.py -B build/deployments/headless/feathers3/main migration-flash
```

For an archived release used without the source tree, these are the exact raw
flash offsets:

| Offset | Release artifact | Purpose |
| ---: | --- | --- |
| `0x0` | `bootloader.bin` | ESP32-S3 bootloader |
| `0x9000` | `partition-table.bin` | Headless partition map |
| `0x10000` | `factory-updater.bin` | Golden recovery updater |
| `0x15E000` | `ota-data-initial.bin` | Start in factory recovery |
| `0x160000` | `firmware.bin` | Main image in `ota_0` |
| `0x5E0000` | `littlefs.bin` | Blank 10,368 KiB filesystem |

The complete layout write replaces LittleFS. Without the optional erase it
deliberately leaves NVS and the NVS-key partition untouched, preserving
per-device settings and recovery credentials.

Blank OTA data boots the factory updater first. On its serial `hw1up>` prompt,
run `setpin <12-to-63-character-passphrase>` and then `cancel` to select and
boot the already-flashed main image.

Do not use the main build's generic `idf.py flash` command: ESP-IDF selects the
small factory app slot for that command. Recovery-layout builds block the stock
flash targets and provide `migration-flash`, `ota0-flash`, and `factory-flash`
instead.
