# Board Switching Guide

This document explains how to switch between different ESP32 board configurations.

## Preferred: per-board isolated builds (no fullclean, ever)

```bash
tools/build_board.sh feathers3               # → build-feathers3/  (own sdkconfig)
tools/build_board.sh qtpy_esp32              # → build-qtpy_esp32/ (own toolchain cache, classic ESP32)
tools/build_board.sh xiao_s3 flash monitor   # actions pass through to idf.py
```

Every board builds in its own `build-<board>/` directory with its own
`sdkconfig`, generated fresh from `sdkconfig.defaults` + the chip-family
defaults + `boards/<board>.defaults`. Consequences:

- **Switching boards never needs `idf.py fullclean`** — the PSRAM-mode/
  bootloader mismatch that forced it cannot happen, since each board keeps its
  own bootloader in its own cache. Rebuilding a board you built before is
  incremental.
- The tracked `./sdkconfig` and the default `./build/` directory (your daily-
  driver board's state) are never touched by another board's build.
- The wrapper derives the chip target from the board file's `# HW_TARGET:`
  marker and always sets `HW_BOARD` — closing the footgun where a bare
  `idf.py build` reconfigure on esp32s3 defaults to `flash=16mb` and
  regenerates `partitions.csv` with a layout an 8 MB sdkconfig can't fit.
- **Caveat — do not run two different boards' builds concurrently.** The root
  `partitions.csv` is generated at configure time and read during the build;
  it is the one file the per-board dirs still share. Serialize builds; the
  file is gitignored/generated, so the next configure simply rewrites it.
- Local sdkconfig experiments (`menuconfig`) apply per board dir, since each
  dir owns its sdkconfig.
- Each successful build writes **`build-<board>/BUILD_INFO.md`** — a manifest
  of what that image actually contains (every feature flag resolved by the
  compiler, chip/PSRAM/flash settings, image size and partition headroom, and
  the git commit it came from). Ship or archive an image with that file and it
  explains itself.

### Known gap — feature flags are still one shared file

Per-board dirs isolate **sdkconfig** (chip, PSRAM mode, flash size, BT stack),
but **`components/hardwareone/System_BuildConfig.h` is global**: one set of
feature flags for every board. Boards genuinely disagree about them, so
switching boards can still require editing the header:

| Board | Requires | Why |
|---|---|---|
| XIAO Sense (carrier) | `I2C_FEATURE_LEVEL 0`, `ENABLE_BLUETOOTH 1` | no breakout sensors/OLED; BT for G2 |
| FeatherS3[D] | `I2C_FEATURE_LEVEL > 0` | MAX17048 fuel gauge is on I2C — a `#error` fires otherwise |
| QT Py ESP32 | `ENABLE_BLUETOOTH 0` | `boards/qtpy_esp32.defaults` sets `CONFIG_BT_ENABLED=n`, so the Bluedroid headers do not exist |

Building a board with the wrong header state fails loudly (a `#error`, or a
missing `esp_gap_ble_api.h`-style include) rather than producing a bad image —
but it is still a manual step the build dirs do not solve. A future
`boards/<board>.features.h` overlay layered by `System_BuildConfig.h` would
close this; until then, treat the header's user-config block as part of the
board profile and expect to set it before switching.

The classic single-`build/` flow below still works and remains what
`./build/` + bare `idf.py` uses; the sections are kept for reference and for
one-off menuconfig work.

## Supported Boards

| Board | Chip | Arduino Variant | Notes |
|-------|------|-----------------|-------|
| Adafruit QT Py ESP32 Pico | ESP32 | `adafruit_qtpy_esp32` | Built-in NeoPixel, Stemma QT |
| Adafruit Feather ESP32 V2 | ESP32 | `adafruit_feather_esp32_v2` | Battery monitoring, built-in NeoPixel |
| Seeed XIAO ESP32S3 | ESP32-S3 | `XIAO_ESP32S3` | Base board |
| Seeed XIAO ESP32S3 Sense | ESP32-S3 | `XIAO_ESP32S3` + `XIAO_ESP32S3_SENSE_ENABLED` | Camera, mic, SD slot |
| Unexpected Maker FeatherS3[D] | ESP32-S3 | `um_feathers3` | 2× STEMMA QT, MAX17048G fuel gauge, **Quad** PSRAM |
| Generic ESP32 | ESP32 | `esp32` | Fallback — verify pins manually |

---

## Switching Between Different Chip Families (ESP32 ↔ ESP32-S3)

When switching between **ESP32** and **ESP32-S3** (different architectures), you **must** do a full clean first. The build cache is not compatible between chip families.

```bash
# 1. Full clean (required when changing chip type)
idf.py fullclean

# 2. Set the target chip
idf.py set-target esp32      # For ESP32-based boards (QT Py, Feather V2)
idf.py set-target esp32s3    # For ESP32-S3 boards (XIAO S3, FeatherS3)

# 3. Pick the board with HW_BOARD (defaults: esp32→qtpy_esp32, esp32s3→feathers3)
HW_BOARD=feather_esp32_v2 idf.py build
```

The `HW_BOARD` env var (see the section below) is the canonical way to select
a board on **both** chip families — it layers the right variant, PSRAM mode/CS,
and flash size for you. `menuconfig → Component config → Arduino → Board` still
works for one-off overrides but is reverted by the next `fullclean`.

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
```

The XIAO profile also pins the camera component to the three sensors this
firmware supports (`OV2640`, `OV3660`, and `OV5640`) and compiles the other
sensor drivers out. Keep that allowlist synchronized with the camera hardware
contract before adding a new XIAO/Sense sensor.

`CMakeLists.txt` reads the `HW_BOARD` env var at configure time and layers the
matching board file on top of the chip-family defaults (`sdkconfig.defaults.<target>`,
which holds only the settings common to all boards of that chip). Each board
file carries a `# HW_TARGET: <chip>` marker; the build **validates** it against
the active target and fails with a `set-target` hint on mismatch — so you can't
accidentally bake an S3 PSRAM mode onto an ESP32 image.

When `HW_BOARD` is unset, it defaults per target: `feathers3` for esp32s3,
`qtpy_esp32` for esp32 — so existing builds keep working unchanged. `HW_BOARD`
only *layers* board settings; it does not switch chips, so set the target with
`idf.py set-target` first.

### Switching boards

```bash
# Build for FeatherS3[D]
HW_BOARD=feathers3 idf.py fullclean
HW_BOARD=feathers3 idf.py build
HW_BOARD=feathers3 idf.py -p /dev/cu.usbmodem* flash monitor

# Back to XIAO ESP32-S3 (HW_BOARD can be unset since xiao_s3 is the default)
idf.py fullclean && idf.py build

# Confirm which board the build picked up
HW_BOARD=feathers3 idf.py reconfigure 2>&1 | grep HW_BOARD
# → -- HW_BOARD=feathers3 → layering .../boards/feathers3.defaults
```

`fullclean` is required between boards because PSRAM mode (Octal vs Quad) lives
in the bootloader, not just the app — without it you get a board that boots but
reports 0 KB PSRAM.

### Adding a new board

1. Add a new `#elif defined(ARDUINO_<NAME>_DEV)` block to
   `components/hardwareone/System_BuildConfig.h` (pin map + features).
2. Drop a `boards/<short_name>.defaults` containing **only** the lines that
   differ from the chip-family default (`sdkconfig.defaults.<target>`) —
   typically `CONFIG_ARDUINO_VARIANT`, `CONFIG_SPIRAM_MODE_*`, the flash size,
   and (on ESP32 PICO parts) `CONFIG_PICO_PSRAM_CS_IO`. Start the file with a
   `# HW_TARGET: <chip>` marker so the build can validate it against the target.
3. Build with `HW_BOARD=<short_name>` (after `idf.py set-target <chip>`).

> **Original Adafruit Feather ESP32 (HUZZAH32) is not supported.** It has 4 MB
> flash and no PSRAM; this firmware's factory app partition alone is ~4.83 MB
> (and there is no `partitions_*_4mb.csv`), so it cannot be flashed at the
> current feature set. Its pin map still exists in `System_BuildConfig.h` for
> reference, but there is intentionally no `boards/feather_esp32.defaults`.

### Alternative: menuconfig (one-off changes)

```bash
idf.py menuconfig
# → Component config → Arduino → Board
# Select your variant, save and exit, then build
```

This overrides the board-file setting for the current `sdkconfig` only — useful
for one-off experiments, but the next `fullclean` reverts to whatever
`HW_BOARD` picks.

---

## Complete Menuconfig Settings

### Adafruit QT Py ESP32 Pico (ESP32)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32` |
| **Arduino** | Arduino board | `adafruit_qtpy_esp32` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Quad` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `40 MHz` |
| **PSRAM CS Pin** | PSRAM clock and cs IO for ESP32-PICO-D4 → CS IO | `10` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `DIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (Classic + BLE 4.2) |

### Seeed XIAO ESP32S3 — Base (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32s3` |
| **Arduino** | Arduino board | `XIAO_ESP32S3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Octal` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |

### Seeed XIAO ESP32S3 Plus (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32s3` |
| **Arduino** | Arduino board | `XIAO_ESP32S3_Plus` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Octal` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `16 MB` ← differs from base |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |

> **Flash size:** The Plus has 16 MB. If you flash it with 8 MB configured the upper flash will be unused and logs may show a mismatch warning.

### Seeed XIAO ESP32S3 Sense (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32s3` |
| **Arduino** | Arduino board | `XIAO_ESP32S3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Octal` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |
| **Sense Define** | Compiler options → Extra C/C++ flags | `-DXIAO_ESP32S3_SENSE_ENABLED` |

### Unexpected Maker FeatherS3[D] (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32s3` |
| **Arduino** | Arduino board | `um_feathers3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | **`Quad`** ← differs from XIAO |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` |
| **Bluetooth** | Component config → Bluetooth | `Bluedroid` (BLE 5.0 only) |
| **USB Mode** | Component config → USB-OTG | Enable for native USB |

> **PSRAM mode is the gotcha:** FeatherS3 uses Quad PSRAM, all the XIAO S3 variants use Octal. After switching `CONFIG_ARDUINO_VARIANT` to `um_feathers3`, flip `CONFIG_SPIRAM_MODE_OCT=y` to `CONFIG_SPIRAM_MODE_QUAD=y` in `sdkconfig.defaults.esp32s3`, then `idf.py fullclean && idf.py build`. Wrong mode = board boots but reports 0 KB PSRAM.

---

## Key Hardware Differences

| Feature | QT Py ESP32 | XIAO ESP32S3 | XIAO ESP32S3 Plus | XIAO ESP32S3 Sense |
|---------|-------------|--------------|-------------------|--------------------|
| **Chip** | ESP32 LX6 | ESP32-S3 LX7 | ESP32-S3 LX7 | ESP32-S3 LX7 |
| **Flash** | 8 MB | 8 MB | **16 MB** | 8 MB |
| **PSRAM** | Quad 40 MHz | Octal 80 MHz | Octal 80 MHz | Octal 80 MHz |
| **Bluetooth** | Classic + BLE 4.2 | BLE 5.0 only | BLE 5.0 only | BLE 5.0 only |
| **USB** | CP2102 UART | Native USB | Native USB | Native USB |
| **Battery ADC** | None | None | **GPIO10** | None |
| **NeoPixel** | GPIO5 | None | None | None |
| **User LED** | None | GPIO21 | GPIO21 | None (SD conflict) |
| **Camera** | None | None | None | OV2640 |
| **Microphone** | None | None | None | PDM digital |
| **SD Card** | None | None | None | MicroSD |
| **Extra UART** | — | — | TX=GPIO42, RX=GPIO41 | — |
| **Extra SPI** | — | — | MOSI=11, MISO=12, SCK=13 | — |
| **I2C (SDA/SCL)** | GPIO22/19 | GPIO5/6 | GPIO5/6 | GPIO5/6 |

---

## Critical Differences When Switching Chip Families

### Bluetooth Stack
- **ESP32**: Supports both Bluetooth Classic and BLE 4.2
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

### XIAO ESP32S3 Plus
When `CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3_Plus"` is set:
- **Flash**: 16 MB — set `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` in sdkconfig
- **Battery monitoring**: ADC on GPIO10 (`BATTERY_ADC_PIN=10`), enabled automatically
- **Extra UART**: TX=GPIO42, RX=GPIO41
- **Extra SPI**: MOSI=GPIO11, MISO=GPIO12, SCK=GPIO13

### XIAO ESP32S3 Sense
When `XIAO_ESP32S3_SENSE_ENABLED` is defined:
- **Camera**: OV2640 on expansion board (I2C on GPIO39/40)
- **PDM Microphone**: CLK=GPIO42, DATA=GPIO41
- **SD Card**: CS=GPIO21, SCK=GPIO7, MISO=GPIO8, MOSI=GPIO9
- **User LED disabled** — GPIO21 is taken by SD_CS on the expansion board

### Adafruit QT Py ESP32 Pico
- **Built-in NeoPixel**: GPIO5, power on GPIO8
- **Stemma QT I2C**: SDA=GPIO22, SCL=GPIO19

### Unexpected Maker FeatherS3[D]
- **STEMMA QT I2C (primary)**: SDA=GPIO8, SCL=GPIO9 (I2C1, always-on LDO; shared with MAX17048G fuel gauge @ 0x36)
- **STEMMA QT I2C (secondary)**: SDA=GPIO15, SCL=GPIO16 (I2C2, LDO2 — powers off in deep sleep). Not currently used by the codebase; would require a second `Wire1` bus instance.
- **Built-in RGB LED**: data on GPIO40, powered via LDO2 (enable on GPIO39)
- **Battery monitoring**: MAX17048G fuel gauge on I2C1 @ 0x36 — *no ADC fallback on the [D]*. Currently disabled in the codebase (ADC-based `battery_monitor` can't talk to the fuel gauge). Adding a small `i2csensor_max17048` driver would re-enable it with better accuracy than the old ADC method.
- **No on-board camera, microphone, or display** — pair with an OLED via STEMMA QT if a display is needed.

---

## sdkconfig.defaults Files

```
sdkconfig.defaults              # Common config (IRAM, stack sizes, log level)
sdkconfig.defaults.esp32        # ESP32-family (Quad PSRAM, DIO flash, Classic BT, variant=qtpy_esp32)
sdkconfig.defaults.esp32s3      # ESP32-S3-family commons (PSRAM presence/speed, QIO flash, BLE only)
boards/xiao_s3.defaults         # XIAO ESP32-S3: variant + Octal PSRAM mode  (default if HW_BOARD unset)
boards/feathers3.defaults       # FeatherS3[D]: variant + Quad PSRAM mode    (activate with HW_BOARD=feathers3)
```

ESP-IDF loads them in order — later files override earlier. For S3 builds the
chain is: `sdkconfig.defaults` → `sdkconfig.defaults.esp32s3` → `boards/<HW_BOARD>.defaults`
(layered by the `HW_BOARD` hook in `CMakeLists.txt`). For ESP32 builds the
chain stops at `sdkconfig.defaults.esp32` and the board file is skipped.

---

## Troubleshooting

### "Wrong chip type" error
Run `idf.py fullclean && idf.py set-target <target>` — required any time you switch between ESP32 and ESP32-S3.

### Board not detected / wrong pin mapping
Verify `CONFIG_ARDUINO_VARIANT` in `sdkconfig` matches your board exactly (case-sensitive). The valid values are: `adafruit_qtpy_esp32`, `adafruit_feather_esp32_v2`, `XIAO_ESP32S3`, `XIAO_ESP32S3_Plus`, `um_feathers3`.

### FeatherS3 reports 0 KB PSRAM
You forgot to flip PSRAM mode after switching from XIAO. Edit `sdkconfig.defaults.esp32s3`, change `CONFIG_SPIRAM_MODE_OCT=y` to `CONFIG_SPIRAM_MODE_QUAD=y`, then `idf.py fullclean && idf.py build`.

### SD card not appearing on Sense
Ensure `XIAO_ESP32S3_SENSE_ENABLED` is defined so the SD pins are configured correctly.

### Battery monitor shows no data on base XIAO
Expected — the base XIAO has no battery ADC. Use the Plus if you need battery monitoring.

### Flash size mismatch warning on Plus
Set `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` in sdkconfig or via menuconfig (Serial flasher config → Flash size → 16 MB).

---

## Quick Reference

```bash
# Switch between ESP32-S3 boards via HW_BOARD (fullclean required — PSRAM mode
# changes affect the bootloader, which has to be rebuilt).
HW_BOARD=feathers3 idf.py fullclean
HW_BOARD=feathers3 idf.py build
HW_BOARD=feathers3 idf.py -p /dev/cu.usbmodem* flash monitor

# Back to XIAO (HW_BOARD optional — xiao_s3 is the default)
idf.py fullclean && idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor

# Within the XIAO family (base ↔ Plus ↔ Sense): edit boards/xiao_s3.defaults to
# change CONFIG_ARDUINO_VARIANT, then `idf.py build`. PSRAM mode doesn't change
# so fullclean isn't strictly required, but flash size differs on the Plus
# (16 MB) — see Plus section below.

# Switch from ESP32 to ESP32-S3 (different chip family — fullclean required)
idf.py fullclean
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor

# Switch from ESP32-S3 to ESP32 (different chip family — fullclean required)
idf.py fullclean
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial* flash monitor
```
