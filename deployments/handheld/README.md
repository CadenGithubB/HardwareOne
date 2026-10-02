# Handheld Deployments

A **Handheld** is the Standard Handheld product of the README: a board with
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
