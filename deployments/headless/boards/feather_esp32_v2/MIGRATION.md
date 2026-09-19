# Headless Feather ESP32 V2 cable migration

The release directory contains a complete first-provisioning set. The guarded
repository target is preferred because it rechecks updater/main identity and
requires the deployment-bound confirmation phrase immediately before writing:

```sh
HW_DEPLOYMENT=headless/feather_esp32_v2 \
  HW_BOARD=feather_esp32_v2 HW_OTA_LAYOUT=1 \
  HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  HW1_UPDATER_BIN="$PWD/build/deployments/headless/feather_esp32_v2/updater/hw1-updater.bin" \
  ESPPORT=/dev/cu.YOUR_PORT \
  idf.py -B build/deployments/headless/feather_esp32_v2/main migration-flash
```

It requires the exact phrase:

```text
MIGRATE headless/feather_esp32_v2 hw1-hl-fv2-ota-v1
```

For an archived release used without the source tree, these are the exact raw
flash offsets:

| Offset | Release artifact | Purpose |
| ---: | --- | --- |
| `0x1000` | `bootloader.bin` | ESP32 bootloader |
| `0x9000` | `partition-table.bin` | Headless partition map |
| `0x10000` | `factory-updater.bin` | Golden recovery updater |
| `0x12E000` | `ota-data-initial.bin` | Start in factory recovery |
| `0x130000` | `firmware.bin` | Main image in `ota_0` |
| `0x5B0000` | `littlefs.bin` | Blank 2,368 KiB filesystem |

Writing the complete set destroys LittleFS. It deliberately does not write the
NVS or NVS-key partitions, so per-device settings and recovery credentials are
preserved when migrating an existing board. Back up and verify LittleFS first.

Do not use the main build's generic `idf.py flash` command: ESP-IDF selects the
small factory app slot for that command. Recovery-layout builds block the stock
flash targets and provide `migration-flash`, `ota0-flash`, and `factory-flash`
instead.
