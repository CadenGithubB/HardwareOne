# Handheld / ESP32-P4X-EYE: installing a release

This deployment is **factory-only**: one application image in the `factory`
partition, no recovery updater, no signed OTA. Every release is installed over
the USB cable with esptool. Settings, accounts and files live in the LittleFS
partition and survive an application reflash; they are erased only by a full
`erase-flash` or by writing `littlefs.bin`.

## Prerequisites

- ESP-IDF 5.5.5 exported, `tools/p4/prepare_sdk.sh` run once (patched `bt`
  and `esp_driver_jpeg` components).
- The board's ESP32-C6 must run the HardwareOne companion firmware (ESP-Hosted
  with the ESP-NOW bridge). Build and flash it with
  [`tools/p4/companion`](../../../../tools/p4/companion/README.md). The P4
  image reports the companion's state at boot; stock Hosted firmware leaves
  Wi-Fi and BLE working but ESP-NOW unable to start.

## Build

```sh
tools/build_deployment.sh handheld p4x_eye
```

No signing key is needed. The release lands in
`build/deployments/handheld/p4x_eye/release/` with `firmware.bin`,
`bootloader.bin`, `partition-table.bin`, `littlefs.bin`, `flasher_args.json`,
`BUILD_INFO.md` and this contract.

## Flash a new release (keeps user data)

Identify the P4's USB port first; macOS may rename it after reconnecting.

```sh
esptool.py --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/deployments/handheld/p4x_eye/release/firmware.bin
```

`flasher_args.json` in the release directory is the authority for every
offset; `0x10000` is the factory partition of this contract's table.

**Devices flashed before 2026-10-01 carry the old shared 16 MB table** (factory
`0x615000`, LittleFS at `0x625000`). This contract's table grows the factory
partition to `0x715000` and moves LittleFS to `0x725000`, so such a device
needs the full install below once, and that install reformats its data
partition. Compare the device's partition table (or the deploy tool's CSV
check) before any app-only flash: a 7 MB image does not fit the old slot.

## First install or full reset (erases user data)

```sh
esptool.py --chip esp32p4 --port "$P4_PORT" --no-stub erase-flash
esptool.py --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000  build/deployments/handheld/p4x_eye/release/bootloader.bin \
  0x8000  build/deployments/handheld/p4x_eye/release/partition-table.bin \
  0x10000 build/deployments/handheld/p4x_eye/release/firmware.bin \
  0x725000 build/deployments/handheld/p4x_eye/release/littlefs.bin
```

The blank `littlefs.bin` makes the data partition mountable; the firmware
never formats storage on its own. Then upload the ESP-SR and STT model files
through the web file manager (see `docs/P4X_EYE_PERIPHERALS.md`).

## Moving to a recovery-OTA layout later

When a P4 recovery layout exists, a new contract with `OTA_LAYOUT=1` replaces
this one. Its partition table will move the LittleFS offset, so that migration
reformats user data; it must carry its own migration guide and a backup step.
