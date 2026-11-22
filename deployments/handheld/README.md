# Handheld Deployments

A **Handheld** is the
[Standard Handheld](../../docs/README_DETAILS.md#3-hardware-one-standard-handheld)
configuration: a board with
its own display and local input, usually a camera and microphone, the web UI,
Bluetooth for the G2 glasses and R1 ring, and the ESP-NOW mesh. It is the
full interactive device, as opposed to the screenless Headless Node and the
wearable-only Pocket Assistant.

| Board | Contract | Release kind |
| --- | --- | --- |
| Espressif ESP32-P4X-EYE | [`boards/p4x_eye`](boards/p4x_eye/contract.conf) | factory-only (`OTA_LAYOUT=0`): flashed over USB, no recovery updater yet |

## What a factory-only contract is

Every deployment contract fixes the feature header, the partition table and
the release size gate for one physical board, so a release is reproducible
from a clean checkout. A recovery-OTA contract (`OTA_LAYOUT=1`, the headless
and pocket-assistant families) additionally pairs the image with a signed
factory updater and a layout id. A factory-only contract (`OTA_LAYOUT=0`)
omits those three OTA keys, needs no signing key, and
`tools/build_deployment.sh` produces an unsigned release directory with the
images, `flasher_args.json` and `BUILD_INFO.md`. The OTA host tools refuse
such a contract with a clear message instead of guessing.

## Build

```sh
tools/p4/prepare_sdk.sh                      # once per SDK install
tools/build_deployment.sh handheld p4x_eye   # -> build/deployments/handheld/p4x_eye/release/
```

Installation and the data-preserving reflash procedure are in
[`boards/p4x_eye/MIGRATION.md`](boards/p4x_eye/MIGRATION.md). The ESP32-C6
companion firmware this image expects is built and flashed with
[`tools/p4/companion`](../../tools/p4/companion/README.md).

## What the P4X-EYE deployment contains

`boards/p4x_eye/features.h` is the complete list. In short: the display, wheel
and buttons, camera, microphone, battery, the microSD card, Wi-Fi and the web
UI, Bluetooth for the G2 glasses and the R1 ring, the ESP-NOW mesh, the ESP-SR
wake word, and local speech-to-text with speaker labels and QuickNote. It
leaves out HTTPS, MQTT, bonded mode, automations, the assistant (LLM), maps,
games and the phone link.

microSD and local speech-to-text were switched on on 2026-11-20
(`ENABLE_SDMMC_CARD`, `ENABLE_SD_CARD` and `ENABLE_LOCAL_STT` are 1). Local
speech-to-text on the P4 reads its acoustic model from the card: this
deployment builds Citrinet-384 and loads
`/sd/STT Models/citrinet384-2000h-c90.p4.stt`; the
[Local speech-to-text model](../../docs/README_DETAILS.md#local-speech-to-text-model-esp32-p4)
section says where the file comes from. Without the card or the file the
rest of the device works and speech-to-text reports that its model cannot be
opened.
`boards/p4x_eye/sdkconfig.defaults` adds the one SDK option local
speech-to-text measures its memory with.

The card and local speech-to-text run daily on this board in the saved
configuration `p4-portable`, which has no camera and no wake word. This
deployment's selection, with both next to local speech-to-text, has not been
built or run on hardware as one image: the image size against the release
gate (`MAIN_RELEASE_MAX` in the contract) and a wake-word plus dictation
session are checked before a release is made from it.
