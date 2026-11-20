# Handheld / ESP32-P4X-EYE: installing a release

This deployment is **factory-only**: one application image in the `factory`
partition, no recovery updater, no signed OTA. Every release is installed over
the USB cable with esptool. Settings, accounts and files live in the LittleFS
partition and survive an application reflash; they are erased only by a full
`erase-flash` or by writing `littlefs.bin`.

## Prerequisites

- ESP-IDF 5.5.5 exported, `tools/p4/prepare_sdk.sh` run once (patched `bt`
  and `esp_driver_jpeg` components and the patched ESP-DL 3.3.12).
- The ESP-IDF PIE context-switch fix
  (`tools/p4/idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch`,
  see the README beside it). Since 2026-11-20 this deployment builds local
  speech-to-text with speaker labels, and the build stops without the fix.
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
never formats storage on its own. Then upload the ESP-SR model files through
the web file manager (see `docs/P4X_EYE_PERIPHERALS.md`).

## microSD card and speech-to-text model (since 2026-11-20)

This deployment now contains the microSD card and local speech-to-text. Fit a
card and copy the acoustic model to `/sd/STT Models/citrinet384-2000h.p4.stt`
(and, if you have them, `meeting.lm` and `speaker.spk` into the same folder);
the
[Local speech-to-text model](../../../../docs/README_DETAILS.md#local-speech-to-text-model-esp32-p4)
section lists the file's size and checksum. Nothing on the card is touched by
a firmware flash. A device updated from a release without the card needs no
other step: without a card or a model file it runs as before and
speech-to-text reports that its model cannot be opened.

The image is larger than the releases before it by the speech-to-text code.
It still has to fit the 7 MB factory partition (the release gate is
`MAIN_RELEASE_MAX` in `contract.conf`); `tools/build_deployment.sh` fails the
build if it does not.

## Moving to a recovery-OTA layout later

When a P4 recovery layout exists, a new contract with `OTA_LAYOUT=1` replaces
this one. Its partition table will move the LittleFS offset, so that migration
reformats user data; it must carry its own migration guide and a backup step.
