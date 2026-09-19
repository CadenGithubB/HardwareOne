# Headless Deployments

A **Headless Node** is a Hardware One device with no local display, input
controller, or optional attached sensors. It is still a fully managed network
node; "headless" does not mean "HTTP-disabled."

The checked-in profile deliberately keeps a small, stable service set:

- Wi-Fi and the core HTTP web interface
- ESP-NOW mesh networking
- Bluetooth LE server
- event-, time-, and schedule-driven automations
- serial CLI, authentication, LittleFS, and remote management
- board-native battery monitoring (ADC on Feather V2; MAX17048 over the
  primary I2C bus on FeatherS3[D])
- signed OTA through the golden factory recovery updater

It excludes optional I2C sensors, displays, local input, MQTT, G2/R1 support,
camera, microphone, speech recognition, LLM, maps, games, and the CM5 link.
The S3 profile enables only the I2C core needed by its on-board fuel gauge;
that exception does not enable a general sensor set. These remain separate
deployment policies rather than accidental additions to the headless image.

## Source layout

```text
deployments/headless/
├── features.h
└── boards/
    ├── feather_esp32_v2/
    │   ├── contract.conf
    │   ├── MIGRATION.md
    │   └── partitions.csv
    └── feathers3/
        ├── contract.conf
        ├── features.h
        ├── MIGRATION.md
        └── partitions.csv
```

Each contract selects one complete feature profile. Feather V2 selects the
deployment-wide `features.h`; FeatherS3[D] selects its board-local `features.h`
because its headless policy must retain the I2C core for the MAX17048. Board
directories also own target, OTA identity, release limits, and partition
geometry. Normal `boards/*.defaults` files remain the source of truth for
Arduino variants, PSRAM, pins, and other physical board configuration.

Generated build products do not belong here. `tools/build_deployment.sh` writes
them below `build/deployments/headless/<board>/`.

## Supported boards

### Feather ESP32 V2

The first supported Headless Node is `feather_esp32_v2`. Its battery reading is
the VBAT/2 divider on GPIO35, so battery monitoring remains available with I2C
completely absent.

Its 8 MB flash is divided into a golden 1,144 KiB factory updater, one 4.5 MiB
main slot, and 2,368 KiB of LittleFS. The signed main release gate is 4 MiB,
leaving at least 512 KiB of emergency growth space inside the app slot.

Build the paired updater, main image, manifest, and offline bundle with:

```bash
source "$IDF_PATH/export.sh"
HW1_OTA_SIGNING_KEY=/absolute/path/to/ota-signing-key.pem \
  tools/build_deployment.sh headless feather_esp32_v2
```

The private key is never copied into the release directory.
The published release includes the signed OTA bundle plus the complete cable
migration image set; see the board's `MIGRATION.md` for guarded installation
and exact offsets.

### Existing devices

This layout moves the beginning of LittleFS from `0x630000` to `0x5B0000`.
It cannot be installed as an ordinary OTA update and an old filesystem cannot
grow backward in place. Back up files, install the layout over a cable, and
restore them to the same physical board. NVS stays at the same address and is
preserved, so recovery credentials and device-bound secrets remain usable.

### Unexpected Maker FeatherS3 / FeatherS3[D]

Use the plain `feathers3` target for both the original FeatherS3 and the newer
FeatherS3[D]. It selects the `um_feathers3` Arduino variant, ESP32-S3 target,
16 MB QSPI flash, and Quad PSRAM. It does **not** enable irreversible flash
encryption; `feathers3_fe` is a separate board target and is not interchangeable.

The FeatherS3[D] MAX17048 battery gauge is at address `0x36` on the primary
I2C bus (SDA GPIO8, SCL GPIO9). The S3 headless profile enables the I2C core,
MAX17048 battery monitoring, and the battery web page solely for that on-board
gauge. Every optional I2C sensor, display, and input remains compiled out. The
secondary connector (SDA GPIO16, SCL GPIO15) is present in the board pin map,
but I2C2 remains disabled by default in this deployment.

That battery policy specifically targets the Series[D] hardware. The original
FeatherS3 used an ADC battery input instead of the MAX17048; under this profile
it will report the gauge absent and needs a separate ADC-oriented deployment
profile for battery telemetry.

Feather ESP32 V2 policy is unchanged: its GPIO35 ADC divider provides battery
monitoring while I2C remains completely disabled. The network, web, BLE,
ESP-NOW, Automations, serial, storage, and signed recovery-OTA surfaces are the
same on both headless boards.

Its 16 MB flash is divided into a golden 1,336 KiB factory updater, one 4.5 MiB
main slot, and 10,368 KiB of LittleFS. The signed main release gate is 4 MiB,
leaving at least 512 KiB of emergency growth space inside the app slot.

Build the complete paired release with:

```bash
source "$IDF_PATH/export.sh"
HW1_OTA_SIGNING_KEY=/absolute/path/to/ota-signing-key.pem \
  tools/build_deployment.sh headless feathers3
```

The deployment layout is distinct from the generic `hw1-f3-ota-v1` layout and
must be installed once over USB. See the board-local `MIGRATION.md` for a full
erase option, guarded provisioning command, exact offsets, and first recovery
credential setup.
