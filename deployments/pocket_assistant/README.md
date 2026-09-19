# Pocket Assistant Deployments

A **Pocket Assistant** is a wearable Hardware One node: a screenless, sensorless
device that pairs with Even Realities G2 glasses and an R1 ring over BLE, and
talks to a Raspberry Pi CM5 / Pi 5 co-processor over an authenticated UART link.
The glasses are the display and the microphone; the CM5 is where the assistant
actually thinks.

It is the Headless Node service set plus the wearable link:

- Wi-Fi and the core HTTP web interface
- ESP-NOW mesh networking
- Bluetooth LE, **G2 glasses** and **R1 ring health**
- the **authenticated CM5/Pi UART host link** - `cm5 power`, `cm5 fan`, and the
  CM5 as the LLM answer source
- event-, time-, and schedule-driven automations
- serial CLI, authentication, LittleFS, and remote management
- signed OTA through the golden factory recovery updater

It excludes I2C entirely, displays, local input, MQTT, HTTPS, the on-board
camera and PDM microphone, speech recognition, the on-device LLM engine, maps,
games, bonded mode, and battery monitoring.

Two of those exclusions are worth spelling out, because the obvious reading is
wrong:

- **No microphone hardware, but the microphone feature is on.**
  `ENABLE_MICROPHONE` is `(ENABLE_MICROPHONE_SENSOR || (ENABLE_BLUETOOTH &&
  ENABLE_G2_GLASSES))`. The on-board PDM silicon is not driven, but the
  source-agnostic audio layer, live audio and dictation all compile in, with the
  G2 glasses microphone as the source. That is the intended arrangement for a
  device you wear rather than hold.
- **No on-device LLM, but the LLM feature is on.**
  `ENABLE_LLM_SOURCE_CM5` is the only source. The on-device engine is the
  single largest contributor to image size and this profile already spends its
  8 MB on Bluedroid plus the G2/R1 stack. Adding it needs a bigger board, not a
  bigger partition.

## Source layout

```text
deployments/pocket_assistant/
└── boards/
    └── xiao_s3/
        ├── contract.conf
        ├── features.h
        ├── sdkconfig.defaults
        ├── MIGRATION.md
        └── partitions.csv
```

There is no deployment-wide `features.h`. The single board owns a complete
profile, for the same reason `headless/boards/feathers3` does: component CMake
reads the selected file as plain text before the preprocessor runs, so each
board must present one unambiguous literal value for every source-list gate.

Board directories also own target, OTA identity, release limits, and partition
geometry. Normal `boards/*.defaults` files remain the source of truth for
Arduino variants, PSRAM, pins, and other physical board configuration.

Generated build products do not belong here. `tools/build_deployment.sh` writes
them below `build/deployments/pocket_assistant/<board>/`.

## The sdkconfig overlay, and why it exists

`contract.conf` names an optional `SDKCONFIG_DEFAULTS` fragment. This is the
first deployment to use it, and it is load-bearing.

`boards/xiao_s3.defaults` sets `CONFIG_BT_ENABLED=n`. That was the right call
for the image it was written for - a standalone XIAO with the on-device LLM,
which overflowed its factory partition by `0x3ff0` with Bluedroid linked in.
It is a per-image trade, not a property of the silicon.

Bluetooth cannot be turned back on from `features.h`. `ENABLE_BLUETOOTH 1` is
only a *request*: the derived rules in `System_BuildConfig.h`, and their mirror
in `components/hardwareone/CMakeLists.txt`, force it to `0` when the active
sdkconfig carries no Bluedroid, and `ENABLE_G2_GLASSES` and `ENABLE_R1_HEALTH`
follow it down. **The failure is silent** - you get a green build of a Pocket
Assistant with no glasses, no ring and no BLE.

So the overlay turns the stack back on, and it is layered last, after
`config/sdkconfig.defaults`, `boards/xiao_s3.defaults`,
`config/sdkconfig.ota.defaults` and `boards/<board>.ota.defaults`, so it wins.
It applies to the main application only; the recovery updater is a separate
project configured by `updater/boards/xiao_s3.defaults`, and has no use for a
radio.

Deployments that need no such override simply omit the key, which is why the
two headless contracts are unchanged.

## Supported boards

### Seeed XIAO ESP32-S3

`xiao_s3` selects the `XIAO_ESP32S3` Arduino variant, ESP32-S3 target, 8 MB
flash, and **octal** PSRAM. It does not enable flash encryption.

`XIAO_ESP32S3_SENSE_ENABLED` is deliberately left at `1`, unlike the headless
profiles. On this board that flag does not mean "compile the camera and mic in"
 - it selects which XIAO pin block the board chain in `System_BuildConfig.h`
matches. The Sense block is the one that defines the microSD pins and the
`UART_LINK_*` pins this deployment depends on; the base XIAO block defines no SD
card. The camera and PDM mic are compiled out by their own flags instead.

The CM5 link is UART0 on the XIAO's D6/D7 pads (GPIO43/44), which is what the
carrier wires to the Pi's `uart2`. The IDF console therefore lives on
USB-Serial/JTAG, for the application and the recovery updater alike.

There is no battery hardware on this board - no connector, no gauge, no VBAT
divider - so the battery subsystem and its web page are both out. This is the
one headless service the Pocket Assistant cannot offer.

#### Flash budget

Its 8 MB flash is divided into a golden 1,144 KiB factory updater, one 5,760 KiB
main slot, and 1,216 KiB of LittleFS.

That split is the opposite of the headless one, on purpose. A headless image is
small and its board has flash to spare, so LittleFS gets the surplus. This
profile carries Bluedroid plus the whole G2/R1 stack on a board with half the
flash of a FeatherS3, so the **application slot is the binding constraint** and
LittleFS is sized to what it actually needs: `users.json`, settings, certs,
automations and the system event log. Bulk capture and the G2 icon packs belong
on the Sense microSD card - `G2_ICON_ANIMATIONS_VFS_PATH` is `/sd/...`, not
LittleFS.

The signed main release gate is 5,376 KiB. Note this is **not** the 512 KiB of
emergency space below the slot that the headless boards leave. At 4,996 KiB
built, an 8 MB Pocket Assistant cannot have both generous growth room and that
much emergency space, so the gate splits the difference: 380 KiB of room to grow
into before the gate trips, and 384 KiB of emergency space between the gate and
the slot ceiling.

Build the paired updater, main image, manifest, and offline bundle with:

```bash
source "$IDF_PATH/export.sh"
HW1_OTA_SIGNING_KEY=/absolute/path/to/ota-signing-key.pem \
  tools/build_deployment.sh pocket_assistant xiao_s3
```

The private key is never copied into the release directory.

#### A deployment-only board

`xiao_s3` is the first board with **no board-only recovery layout**. It reaches
the OTA system solely as `pocket_assistant/xiao_s3`, so it deliberately has no
row in the recovery-OTA registry in the root `CMakeLists.txt`, nor in
`BOARD_CONTRACT` / `BOARD_LAYOUTS` / `BOARD_SUFFIXES` under `tools/ota`. A
deployment contract supplies the layout id, version suffix, partition table and
release limits itself and overrides that registry; a row there would assert a
board-only layout that does not exist.

What it does need, and has: `updater/boards/xiao_s3.defaults`, a row in
`updater/CMakeLists.txt` - which **refuses** a bare `HW_BOARD=xiao_s3` updater
build rather than letting it fall through to the shared 8 MB table and stamp a
layout id it does not implement - and the id constants in
`components/hw1_ota_protocol/include/hw1_ota_protocol.h`.

#### Existing devices

The stock XIAO layout has no factory recovery slot and puts LittleFS at
`0x5C5000`. This layout cannot be installed as an ordinary OTA update and an old
filesystem cannot slide in place. Back up files, install over a cable, and
restore them to the same physical board. NVS stays at `0xA000` and is preserved,
so recovery credentials and device-bound secrets remain usable. See the board's
`MIGRATION.md` for exact offsets and the guarded procedure.
