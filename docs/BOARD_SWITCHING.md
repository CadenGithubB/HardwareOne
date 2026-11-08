# Board Switching Guide

This document explains how to switch between different ESP32 board configurations.

## Saved feature configurations

For different feature selections on the same board, use a saved configuration:

```bash
python3 tools/build_config.py build xiao-default
python3 tools/build_config.py build xiao-mqtt
```

These build independently in `build-config-<name>/`, including separate
`sdkconfig` files. They use the same board build and compiler dependency rules,
without editing shared headers. XIAO keeps MQTT off by default; the MQTT
configuration enables it and its settings page, with offline maps disabled to
fit the existing partition. See
[saved configurations](../configurations/README.md) for creation, inherited
defaults, validation, and build reports.

## Preferred: per-board isolated builds (no fullclean, ever)

```bash
tools/build_board.sh feathers3               # → build-feathers3/  (own sdkconfig)
tools/build_board.sh qtpy_esp32              # → build-qtpy_esp32/ (own toolchain cache, classic ESP32)
tools/build_board.sh xiao_s3 flash monitor   # actions pass through to idf.py
tools/build_board.sh p4x_eye                 # ESP32-P4X-EYE (run tools/p4/prepare_sdk.sh once first)
```

Every board builds in its own `build-<board>/` directory with its own
`sdkconfig`, generated fresh from `sdkconfig.defaults` + the chip-family
defaults + `boards/<board>.defaults`, followed by the optional SDK policy in
`presets/` and any saved SDK choices. Consequences:

- **Switching boards never needs `idf.py fullclean`** — the PSRAM-mode/
  bootloader mismatch that forced it cannot happen, since each board keeps its
  own bootloader in its own cache. Rebuilding a board you built before is
  incremental.
- The tracked `./sdkconfig` and the default `./build/` directory (your daily-
  driver board's state) are never touched by another board's build.
- The wrapper derives the chip target from the board file's `# HW_TARGET:`
  marker and always sets `HW_BOARD` — closing the footgun where a bare
  `idf.py build` reconfigure on esp32s3 could combine `flash=16mb` with an
  8 MB board's saved configuration.
- Build directories and partition inputs are independent. Dependency installation
  still shares `managed_components/`, synced to the configuring target's lock;
  `cmake/HW1ManagedComponents.cmake` makes the next build of another target
  reconfigure and resync it on its own, and serializes the syncs. Builds of one
  target may run concurrently; the wrappers refuse a build of another target
  while one runs (`tools/checkout_build_lock.sh`).
- Local sdkconfig experiments (`menuconfig`) apply per board dir, since each
  dir owns its sdkconfig.
- Each successful build writes **`build-<board>/BUILD_INFO.md`** — a manifest
  of what that image actually contains (every feature flag resolved by the
  compiler, chip/PSRAM/flash settings, image size and partition headroom, and
  the git commit it came from). Ship or archive an image with that file and it
  explains itself.

### Board and deployment are independent axes

The [configuration ownership and portability report](CONFIGURATION_OWNERSHIP.md)
classifies current hardware facts, resource constraints, and application policy,
and describes the migration toward reusable feature selections. Saved configurations
now cover XIAO MQTT, external I2C/display/input choices, and Bluetooth SDK support.

Per-board directories isolate **sdkconfig**, including hardware settings and
optional SDK features. Shared application defaults live in
`components/hardwareone/System_BuildConfig.h`; board feature defaults live in
`boards/<board>.features.h`. Saved configurations override those defaults
without editing shared headers.

| Selection | Default or constraint |
|---|---|
| XIAO Sense | MQTT, external I2C, display and input default off; saved configurations can enable them. |
| XIAO external I2C | GPIO5/6 on controller 1; the current camera SDK also uses controller 1, so the supplied display configuration explicitly disables the camera. |
| FeatherS3[D] | Its onboard MAX17048 requires I2C; the build diagnoses a disabled bus. |
| Bluetooth SDK | Shared default on; QT Py defaults off through `presets/qtpy_esp32.sdkconfig.defaults`. Saved SDK choices can override either default. |

`xiao-no-bluetooth` disables both the application feature and SDK Bluetooth
support. `xiao-display` selects the supported SSD1306 backend. Missing bus,
backend and assembly-pin requirements produce build errors. Physical flash,
PSRAM, radio wiring and security settings remain outside the saved feature API.

Checked-in deployment profiles solve that problem for reproducible release
images. A deployment selects a feature overlay and an exact OTA/partition
contract while continuing to use `boards/<board>.defaults` for the physical
hardware. Build them with both names:

```bash
HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  tools/build_deployment.sh headless feather_esp32_v2

# Unexpected Maker FeatherS3 / FeatherS3[D]
HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  tools/build_deployment.sh headless feathers3
```

This writes the updater, main image, and release artifacts below
`build/deployments/headless/<board>/`; it does not change the shared header or
reuse an ordinary board build directory. The canonical Headless Node contracts
are documented in [`deployments/headless/README.md`](../deployments/headless/README.md).

A contract may also declare `OTA_LAYOUT=0`: a **factory-only** deployment for a
board without a recovery layout. It keeps the feature header, partition table
and size gate, but builds no updater, needs no signing key, and produces an
unsigned release directory for cable flashing. The ESP32-P4X-EYE ships this
way as [`deployments/handheld/boards/p4x_eye`](../deployments/handheld/README.md):

```bash
tools/build_deployment.sh handheld p4x_eye
```

The coverage tools (`tools/build_coverage.sh`, `tools/build_memory_coverage.sh`,
`tools/feature_size_sweep.sh`) take the board as their first argument and edit
the board's `boards/<board>.features.h` for any flag it defines, since that
profile outranks the shared header.

A bare `idf.py` in the repository root (no `HW_BOARD`, shared `./build/`) is
not a supported build. It would use the tracked `config/sdkconfig` (a XIAO,
8 MB configuration) with the per-target default board (FeatherS3, 16 MB table)
and rewrite that tracked file, so configure stops with a message pointing at
`tools/build_board.sh`. The board files, `HW_BOARD` rules and menuconfig tables
below are reference for what `tools/build_board.sh` selects; every command in
them goes through the wrapper.

## Supported Boards

| Board | Chip | Arduino Variant | Notes |
|-------|------|-----------------|-------|
| Adafruit QT Py ESP32 Pico | ESP32 | `adafruit_qtpy_esp32` | Built-in NeoPixel, Stemma QT |
| Adafruit Feather ESP32 V2 | ESP32 | `adafruit_feather_esp32_v2` | Battery monitoring, built-in NeoPixel |
| Seeed XIAO ESP32S3 (base, no Sense) | ESP32-S3 | `XIAO_ESP32S3` | **Not supported**: every ESP32-S3 build is a Sense build - see [KNOWN_ISSUES.md](../KNOWN_ISSUES.md) |
| Seeed XIAO ESP32S3 Sense | ESP32-S3 | `XIAO_ESP32S3` + `XIAO_ESP32S3_SENSE_ENABLED` | Camera, mic, SD slot |
| Unexpected Maker FeatherS3[D] | ESP32-S3 | `um_feathers3` | 2× STEMMA QT, MAX17048G fuel gauge, **Quad** PSRAM |
| Espressif ESP32-P4X-EYE | ESP32-P4 | `esp32p4` + `boards/p4x_eye.features.h` | LCD, wheel + 3 buttons, MIPI camera, PDM mic, battery; Wi-Fi/BLE via onboard C6 - see [P4X_EYE_PERIPHERALS.md](P4X_EYE_PERIPHERALS.md) |
| Generic ESP32 | ESP32 | `esp32` | Fallback — verify pins manually |

---

## Switching Between Different Chip Families (ESP32 ↔ ESP32-S3)

The build cache is not compatible between chip families, so each board builds
in its own directory. `tools/build_board.sh` reads the chip from the board
file's `# HW_TARGET:` marker; no `fullclean` or `set-target` is needed:

```bash
tools/build_board.sh feather_esp32_v2   # ESP32    → build-feather_esp32_v2/
tools/build_board.sh xiao_s3            # ESP32-S3 → build-xiao_s3/
```

The wrapper always sets the `HW_BOARD` env var (see the section below), which
layers the right variant, PSRAM mode/CS and flash size on **both** chip
families. `tools/build_board.sh <board> menuconfig` still works for one-off
overrides; they apply to `build-<board>/sdkconfig` only.

---

## Switching Between ESP32-S3 Boards — the `HW_BOARD` env var

The XIAO ESP32-S3 family and the Unexpected Maker FeatherS3[D] are all ESP32-S3,
but they differ in pin map AND **PSRAM mode** (XIAO=Octal, FeatherS3=Quad), so
swapping just the `CONFIG_ARDUINO_VARIANT` isn't enough — the bootloader gets
baked with the PSRAM mode and has to be rebuilt.

To make this painless, the project has one file per board under `boards/`. The
`HW_BOARD` mechanism is **not** S3-only — every supported board (both chip
families) has a file here:

```
# The MB value below is FLASH size (drives the partition table). PSRAM size is
# auto-detected (CONFIG_SPIRAM_TYPE_AUTO) — FeatherS3 = 8 MB Quad, XIAO = 8 MB Octal.
boards/feathers3.defaults         # esp32s3  — um_feathers3       + Quad PSRAM,  16 MB flash
boards/xiao_s3.defaults           # esp32s3  — XIAO_ESP32S3       + Octal PSRAM,  8 MB flash
boards/qtpy_esp32.defaults        # esp32    — adafruit_qtpy_esp32      + Quad PSRAM, 8 MB flash
boards/feather_esp32_v2.defaults  # esp32    — adafruit_feather_esp32_v2 + Quad PSRAM, 8 MB flash
boards/p4x_eye.defaults           # esp32p4  - esp32p4 + C6 Hosted radio wiring + camera sensor, 16 MB flash
```

The XIAO profile also pins the camera component to the three sensors this
firmware supports (`OV2640`, `OV3660`, and `OV5640`) and compiles the other
sensor drivers out. Keep that allowlist synchronized with the camera hardware
contract before adding a new XIAO/Sense sensor.

`CMakeLists.txt` reads the `HW_BOARD` env var at configure time and layers the
matching board file on top of the chip-family defaults (`sdkconfig.defaults.<target>`,
which holds only the settings common to all boards of that chip). Each board
file carries a `# HW_TARGET: <chip>` marker; the build **validates** it against
the active target and fails on mismatch — so you can't accidentally bake an S3
PSRAM mode onto an ESP32 image. `tools/build_board.sh` sets the target from
that marker, so the two always agree.

When `HW_BOARD` is unset, the board defaults per target: `feathers3` for
esp32s3, `qtpy_esp32` for esp32. Configure then stops (naming
`tools/build_board.sh`) if the sdkconfig in use is the tracked
`config/sdkconfig`, or was saved for a different Arduino variant or flash size
than that default board — the combination that used to produce a XIAO
configuration with the FeatherS3 16 MB partition table.

### Switching boards

```bash
tools/build_board.sh feathers3                      # → build-feathers3/
tools/build_board.sh feathers3 -p PORT flash monitor
tools/build_board.sh xiao_s3                        # → build-xiao_s3/, untouched by the FeatherS3 build

# Confirm which board a build directory uses
tools/build_board.sh feathers3 reconfigure 2>&1 | grep HW_BOARD
# → -- HW_BOARD=feathers3 (target=esp32s3, flash=16mb) → layering .../boards/feathers3.defaults
```

PSRAM mode (Octal vs Quad) lives in the bootloader, not just the app; because
each board keeps its own build directory and bootloader, no `fullclean` is
needed between boards.

### Adding a new board

1. Add a new `#elif defined(ARDUINO_<NAME>_DEV)` block to
   `components/hardwareone/System_BuildConfig.h` (pin map + features).
2. Drop a `boards/<short_name>.defaults` containing **only** the lines that
   differ from the chip-family default (`sdkconfig.defaults.<target>`) —
   typically `CONFIG_ARDUINO_VARIANT`, `CONFIG_SPIRAM_MODE_*`, the flash size,
   and (on ESP32 PICO parts) `CONFIG_PICO_PSRAM_CS_IO`. Start the file with a
   `# HW_TARGET: <chip>` marker so the build can validate it against the target.
3. Build with `tools/build_board.sh <short_name>`.
4. Optional: if the board's application features differ from the shared
   editable defaults (a board with no I2C display, a different radio, ...), add
   `boards/<short_name>.features.h` with `#undef`/`#define` pairs, one literal
   `#define NAME value` per setting. `System_BuildConfig.h` includes it after its
   defaults and the CMake source gates read it too, so it only applies when that
   board is selected. A deployment profile, if also selected, is applied after
   it. `boards/p4x_eye.features.h` is the example.

> **Original Adafruit Feather ESP32 (HUZZAH32) is not supported.** It has 4 MB
> flash and no PSRAM; this firmware's factory app partition alone is ~4.83 MB
> (and there is no `partitions_*_4mb.csv`), so it cannot be flashed at the
> current feature set. Its pin map still exists in `System_BuildConfig.h` for
> reference, but there is intentionally no `boards/feather_esp32.defaults`.

### Alternative: menuconfig (one-off changes)

```bash
tools/build_board.sh <board> menuconfig
```

Changes apply to `build-<board>/sdkconfig` only. Settings the board file
defines (Arduino variant, PSRAM mode, flash size) are restored from it on the
next configure; other menuconfig tuning stays.

---

## Complete Menuconfig Settings

Reference: what each board file and the chip-family defaults select. View or
change them with `tools/build_board.sh <board> menuconfig`.

### Adafruit QT Py ESP32 Pico (ESP32)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `# HW_TARGET:` in the board file | `esp32` |
| **Arduino** | Arduino board | `adafruit_qtpy_esp32` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Quad` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `40 MHz` |
| **PSRAM CS Pin** | PSRAM clock and cs IO for ESP32-PICO-D4 → CS IO | `10` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `DIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid`, controller BLE-only (`CONFIG_BTDM_CTRL_MODE_BLE_ONLY`) |

### Seeed XIAO ESP32S3 - Base (not supported)

There is no base-XIAO build: every ESP32-S3 build defines
`XIAO_ESP32S3_SENSE_ENABLED`, so a base XIAO gets the Sense build, whose
camera, microphone and SD card expect the Sense expansion board. See
[KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

### Seeed XIAO ESP32S3 Sense (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `# HW_TARGET:` in the board file | `esp32s3` |
| **Arduino** | Arduino board | `XIAO_ESP32S3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Octal` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |
| **Sense Define** | `CMakeLists.txt` (every ESP32-S3 target) and `System_BuildConfig.h` | `XIAO_ESP32S3_SENSE_ENABLED 1`, always set |

### Unexpected Maker FeatherS3[D] (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `# HW_TARGET:` in the board file | `esp32s3` |
| **Arduino** | Arduino board | `um_feathers3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | **`Quad`** ← differs from XIAO |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `16 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |

> **PSRAM mode is the gotcha:** FeatherS3 uses Quad PSRAM, the XIAO S3 boards use Octal. `boards/feathers3.defaults` and `boards/xiao_s3.defaults` set the mode, and each board builds in its own directory with its own bootloader, so `tools/build_board.sh` keeps them apart. Wrong mode = board boots but reports 0 KB PSRAM.

---

## Key Hardware Differences

| Feature | QT Py ESP32 | XIAO ESP32S3 (base, not supported) | XIAO ESP32S3 Sense |
|---------|-------------|--------------|--------------------|
| **Chip** | ESP32 LX6 | ESP32-S3 LX7 | ESP32-S3 LX7 |
| **Flash** | 8 MB | 8 MB | 8 MB |
| **PSRAM** | Quad 40 MHz | Octal 80 MHz | Octal 80 MHz |
| **Bluetooth** | BLE 4.2 (controller BLE-only) | BLE 5.0 only | BLE 5.0 only |
| **USB** | CP2102 UART | Native USB | Native USB |
| **Battery ADC** | None | None | None |
| **NeoPixel** | GPIO5 | None | None |
| **User LED** | None | GPIO21 | None (SD conflict) |
| **Camera** | None | None | OV2640 |
| **Microphone** | None | None | PDM digital |
| **SD Card** | None | None | MicroSD |
| **I2C (SDA/SCL)** | GPIO22/19 | GPIO5/6 | GPIO5/6 |

---

## Critical Differences When Switching Chip Families

### Bluetooth Stack
- **ESP32**: the chip supports Bluetooth Classic and BLE 4.2; this firmware configures the controller BLE-only
- **ESP32-S3**: **BLE 5.0 only** — no Bluetooth Classic support

### PSRAM Configuration
- **ESP32**: Quad SPI PSRAM at 40 MHz (`CONFIG_SPIRAM_MODE_QUAD`)
- **ESP32-S3**: Octal SPI PSRAM at 80 MHz (`CONFIG_SPIRAM_MODE_OCT`)
- Wrong PSRAM mode causes a boot failure or crash

### USB Mode
- **ESP32**: External UART chip (CP2102) — port appears as `/dev/cu.SLAB*` or `/dev/cu.usbserial*`
- **ESP32-S3**: Native USB — port appears as `/dev/cu.usbmodem*`
- May need to hold BOOT button when flashing S3 for the first time

### Flash Mode
- **ESP32**: `DIO`
- **ESP32-S3**: `QIO` (faster; check your specific module if unsure)

---

## Board-Specific Features

### XIAO ESP32S3 Sense
`XIAO_ESP32S3_SENSE_ENABLED` is 1 on every ESP32-S3 build, so the XIAO build configures:
- **Camera**: OV2640 on expansion board (I2C on GPIO39/40)
- **PDM Microphone**: CLK=GPIO42, DATA=GPIO41
- **SD Card**: CS=GPIO21, SCK=GPIO7, MISO=GPIO8, MOSI=GPIO9
- **User LED disabled** — GPIO21 is taken by SD_CS on the expansion board

### Adafruit QT Py ESP32 Pico
- **Built-in NeoPixel**: GPIO5, power on GPIO8
- **Stemma QT I2C**: SDA=GPIO22, SCL=GPIO19

### Unexpected Maker FeatherS3[D]
- **STEMMA QT I2C (primary)**: SDA=GPIO8, SCL=GPIO9 (I2C1, always-on LDO; shared with MAX17048G fuel gauge @ 0x36)
- **STEMMA QT I2C (secondary)**: SDA=GPIO16, SCL=GPIO15 (I2C2, LDO2 - powers off in deep sleep)
- **Built-in RGB LED**: data on GPIO40, powered via LDO2 (enable on GPIO39)
- **Battery monitoring**: the existing `i2csensor_max17048` driver reads the MAX17048G on I2C1 @ 0x36; there is no ADC fallback on the [D]
- **No on-board camera, microphone, or display** — pair with an OLED via STEMMA QT if a display is needed.

The `headless/feathers3` deployment is intentionally gauge-only: it enables
the core primary I2C path and MAX17048 battery surfaces, while every optional
sensor, display, and input is compiled out. I2C2 remains disabled by default,
so the secondary connector does not silently add another active bus. This is a
deployment policy, not a limitation of the board or I2C implementation.

The `headless/feather_esp32_v2` deployment is unchanged: it uses the GPIO35
ADC battery divider and keeps I2C entirely disabled.

---

## sdkconfig.defaults Files

```
sdkconfig.defaults              # Common config (IRAM, stack sizes, log level)
sdkconfig.defaults.esp32        # ESP32-family commons (PSRAM presence/speed, DIO flash, BLE-only controller)
sdkconfig.defaults.esp32s3      # ESP32-S3-family commons (PSRAM presence/speed, QIO flash, BLE only)
boards/xiao_s3.defaults         # XIAO ESP32-S3: variant + Octal PSRAM mode, 8 MB flash
boards/feathers3.defaults       # FeatherS3[D]: variant + Quad PSRAM mode, 16 MB flash (default board on esp32s3 if HW_BOARD unset)
boards/qtpy_esp32.defaults      # QT Py ESP32: variant + PSRAM CS, 8 MB flash  (default board on esp32 if HW_BOARD unset)
sdkconfig.defaults.esp32p4      # ESP32-P4-family commons (400 MHz, hex PSRAM, Hosted Wi-Fi/BLE, camera pipeline)
boards/p4x_eye.defaults         # P4X-EYE: C6 SDIO wiring, 16 MB flash, OV2710/SC2336 camera
boards/p4x_eye.features.h       # P4X-EYE application feature profile (not an sdkconfig file)
```

The `sdkconfig.defaults*` files live in `config/`. ESP-IDF loads them in
order — later files override earlier. The chain is: `sdkconfig.defaults` →
`sdkconfig.defaults.<target>` → `boards/<HW_BOARD>.defaults` (layered by the
`HW_BOARD` hook in `CMakeLists.txt` on every target that has a board file),
then the partition selection and the optional SDK policy in `presets/`.

---

## Troubleshooting

### Configure stops with "A bare idf.py configure (no HW_BOARD) ..."
You ran `idf.py` in the repository root without naming a board. Build with
`tools/build_board.sh <board>` instead.

### "HW_BOARD=... targets '...', but the active chip is '...'"
The board file's `# HW_TARGET:` marker disagrees with the target being
configured. `tools/build_board.sh` sets the target from that marker; use it
rather than `idf.py set-target`.

### Board not detected / wrong pin mapping
Verify `CONFIG_ARDUINO_VARIANT` in `build-<board>/sdkconfig` matches your board exactly (case-sensitive). The valid values are: `adafruit_qtpy_esp32`, `adafruit_feather_esp32_v2`, `XIAO_ESP32S3`, `um_feathers3`, `esp32p4`.

### FeatherS3 reports 0 KB PSRAM
The image was built with Octal PSRAM. Build it with `tools/build_board.sh feathers3`, which layers `CONFIG_SPIRAM_MODE_QUAD=y` from `boards/feathers3.defaults` and restores it on every configure.

### SD card not appearing on Sense
Check that the image was built with `tools/build_board.sh xiao_s3`; ESP32-S3 builds always set `XIAO_ESP32S3_SENSE_ENABLED`, which configures the SD pins.

### Battery monitor shows no data on the XIAO
Expected — the XIAO ESP32-S3 boards have no battery ADC in this firmware.

---

## Quick Reference

```bash
# Each board builds in build-<board>/ with its own sdkconfig and bootloader:
# no fullclean or set-target when switching boards or chip families.
tools/build_board.sh feathers3                         # ESP32-S3, 16 MB
tools/build_board.sh feathers3 -p /dev/cu.usbmodem* flash monitor
tools/build_board.sh xiao_s3 -p /dev/cu.usbmodem* flash monitor
tools/build_board.sh qtpy_esp32 -p /dev/cu.usbserial* flash monitor   # ESP32
tools/build_board.sh p4x_eye                           # run tools/p4/prepare_sdk.sh once first

# One-off SDK changes for one board
tools/build_board.sh xiao_s3 menuconfig
```
