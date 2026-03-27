# Board Switching Guide

This document explains how to switch between different ESP32 board configurations.

## Supported Boards

| Board | Chip | Arduino Variant | Notes |
|-------|------|-----------------|-------|
| Adafruit QT Py ESP32 Pico | ESP32 | `adafruit_qtpy_esp32` | Built-in NeoPixel, Stemma QT |
| Adafruit Feather ESP32 V2 | ESP32 | `adafruit_feather_esp32_v2` | Battery monitoring, built-in NeoPixel |
| Seeed XIAO ESP32S3 | ESP32-S3 | `XIAO_ESP32S3` | Base board |
| Seeed XIAO ESP32S3 Sense | ESP32-S3 | `XIAO_ESP32S3` + `XIAO_ESP32S3_SENSE_ENABLED` | Camera, mic, SD slot |
| Seeed XIAO ESP32S3 Plus | ESP32-S3 | `XIAO_ESP32S3_Plus` | 16MB flash, battery ADC, extra UART/SPI |
| Generic ESP32 | ESP32 | `esp32` | Fallback — verify pins manually |

---

## Switching Between Different Chip Families (ESP32 ↔ ESP32-S3)

When switching between **ESP32** and **ESP32-S3** (different architectures), you **must** do a full clean first. The build cache is not compatible between chip families.

```bash
# 1. Full clean (required when changing chip type)
idf.py fullclean

# 2. Set the target chip
idf.py set-target esp32      # For ESP32-based boards (QT Py, Feather)
idf.py set-target esp32s3    # For ESP32-S3 boards (XIAO S3, XIAO S3 Plus)

# 3. If your board variant differs from the default, update it
idf.py menuconfig
# → Component config → Arduino → Board
```

---

## Switching Between XIAO ESP32S3 Variants (no fullclean needed)

The base XIAO, Sense, and Plus are all ESP32-S3 — same chip, same `set-target`. You only need to change the Arduino variant. There are two ways:

### Option A — Edit `sdkconfig.defaults.esp32s3` (recommended for persistent config)

Open `sdkconfig.defaults.esp32s3` and change the `CONFIG_ARDUINO_VARIANT` line:

```ini
# Base XIAO ESP32S3
CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"

# --- or ---

# XIAO ESP32S3 Plus (16MB flash, battery ADC, extra UART/SPI)
CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3_Plus"

# --- or ---

# XIAO ESP32S3 Sense (camera, mic, SD) — also define XIAO_ESP32S3_SENSE_ENABLED
CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"
```

After editing, run `idf.py build`. The new variant will be picked up automatically.

> For the Sense board you also need `XIAO_ESP32S3_SENSE_ENABLED` defined. See the Sense-specific section below.

### Option B — Use menuconfig

```bash
idf.py menuconfig
# → Component config → Arduino → Board
# Select your variant, save and exit, then build
```

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

---

## sdkconfig.defaults Files

```
sdkconfig.defaults            # Common config (IRAM, stack sizes, log level)
sdkconfig.defaults.esp32      # ESP32-specific (Quad PSRAM, DIO flash, Classic BT)
sdkconfig.defaults.esp32s3    # ESP32-S3-specific (Octal PSRAM, QIO flash, BLE only)
```

ESP-IDF loads them in order — the target-specific file overrides the base. The variant set in `sdkconfig.defaults.esp32s3` is `XIAO_ESP32S3` by default. Change it to `XIAO_ESP32S3_Plus` if you are using the Plus board.

---

## Troubleshooting

### "Wrong chip type" error
Run `idf.py fullclean && idf.py set-target <target>` — required any time you switch between ESP32 and ESP32-S3.

### Board not detected / wrong pin mapping
Verify `CONFIG_ARDUINO_VARIANT` in `sdkconfig` matches your board exactly (case-sensitive). The valid values are: `adafruit_qtpy_esp32`, `adafruit_feather_esp32_v2`, `XIAO_ESP32S3`, `XIAO_ESP32S3_Plus`.

### SD card not appearing on Sense
Ensure `XIAO_ESP32S3_SENSE_ENABLED` is defined so the SD pins are configured correctly.

### Battery monitor shows no data on base XIAO
Expected — the base XIAO has no battery ADC. Use the Plus if you need battery monitoring.

### Flash size mismatch warning on Plus
Set `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` in sdkconfig or via menuconfig (Serial flasher config → Flash size → 16 MB).

---

## Quick Reference

```bash
# Switch from base XIAO to XIAO Plus (same chip family — no fullclean needed)
# Edit sdkconfig.defaults.esp32s3, change:
#   CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"
# to:
#   CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3_Plus"
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor

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
