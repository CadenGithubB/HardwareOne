# Board Switching Guide

This document explains how to switch between different ESP32 board configurations.

## Supported Boards

| Board | Chip | Arduino Variant | Notes |
|-------|------|-----------------|-------|
| Adafruit QT Py ESP32 Pico | ESP32 | `adafruit_qtpy_esp32` | Built-in NeoPixel |
| Seeed XIAO ESP32S3 | ESP32-S3 | `XIAO_ESP32S3` | Base board |
| Seeed XIAO ESP32S3 Sense | ESP32-S3 | `XIAO_ESP32S3` + `XIAO_ESP32S3_SENSE_ENABLED` | Camera, mic, SD slot |
| Generic ESP32 | ESP32 | `esp32` | Fallback |

## Switching Between Different Chip Families

When switching between ESP32 and ESP32-S3 (different architectures), you **must** do a full clean:

```bash
# 1. Full clean (required when changing chip type)
idf.py fullclean

# 2. Set the target chip
idf.py set-target esp32      # For ESP32-based boards (QT Py, etc.)
idf.py set-target esp32s3    # For ESP32-S3 boards (XIAO S3, etc.)

# 3. Configure
idf.py menuconfig
```

## Complete Menuconfig Settings

### For Adafruit QT Py ESP32 Pico (ESP32)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32` |
| **Arduino** | Arduino board | `adafruit_qtpy_esp32` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Quad` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `40 MHz` |
| **PSRAM CS Pin** | PSRAM clock and cs IO for ESP32-PICO-D4 → CS IO | `10` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `DIO` |
| **Bluetooth** | Component config → Bluetooth → Bluetooth controller | `Bluedroid` (dual-mode) |
| **BT Controller** | Bluetooth → Controller Options | ESP32 controller (built-in) |

### For Seeed XIAO ESP32S3 Sense (ESP32-S3)

| Category | Setting | Value |
|----------|---------|-------|
| **Target** | `idf.py set-target` | `esp32s3` |
| **Arduino** | Arduino board | `XIAO_ESP32S3` |
| **PSRAM Mode** | ESP PSRAM → SPI RAM config → Mode | `Octal` |
| **PSRAM Speed** | ESP PSRAM → SPI RAM config → Speed | `80 MHz` |
| **Flash Size** | Serial flasher config → Flash size | `8 MB` |
| **Flash Mode** | Serial flasher config → Flash mode | `QIO` or `DIO` |
| **Bluetooth** | Component config → Bluetooth → Bluetooth controller | `Bluedroid` (BLE only on S3) |
| **BT Controller** | Bluetooth → Controller Options | ESP32-S3 controller |
| **USB Mode** | Component config → USB-OTG | Enable if using native USB |
| **Sense Define** | Compiler options → Preprocessor definitions | `XIAO_ESP32S3_SENSE_ENABLED` |

## Key Hardware Differences

| Feature | QT Py ESP32 Pico | XIAO ESP32S3 Sense |
|---------|------------------|---------------------|
| **Chip** | ESP32 (Xtensa LX6 dual-core) | ESP32-S3 (Xtensa LX7 dual-core) |
| **PSRAM Type** | Quad SPI (40 MHz) | Octal SPI (80 MHz) |
| **Flash** | 8MB | 8MB |
| **Bluetooth** | BT Classic + BLE 4.2 | **BLE 5.0 only** (no Classic) |
| **USB** | CP2102 UART bridge | Native USB-OTG |
| **Camera** | None | OV2640 |
| **Microphone** | None | PDM digital mic |
| **SD Card** | None | MicroSD slot |
| **AI Acceleration** | None | Vector instructions for ML |

## Critical Differences When Switching

### 1. Bluetooth Stack
- **ESP32**: Supports both Bluetooth Classic and BLE 4.2
- **ESP32-S3**: **BLE 5.0 only** - no Bluetooth Classic support
- If your code uses `BT_CLASSIC_ENABLED`, it will fail on ESP32-S3

### 2. PSRAM Configuration
- **ESP32**: Quad SPI PSRAM at 40 MHz (`CONFIG_SPIRAM_MODE_QUAD`)
- **ESP32-S3**: Octal SPI PSRAM at 80 MHz (`CONFIG_SPIRAM_MODE_OCT`)
- Wrong PSRAM mode = boot failure or crashes

### 3. USB Mode
- **ESP32**: Uses external UART chip (CP2102), always `/dev/cu.SLAB*` or `/dev/cu.usbserial*`
- **ESP32-S3**: Native USB, shows as `/dev/cu.usbmodem*`
- May need to hold BOOT button when flashing S3

### 4. Flash Mode
- **ESP32**: Usually `DIO` mode
- **ESP32-S3**: Can use `QIO` for faster speeds (check your module)

## Using sdkconfig.defaults for Shared Configuration

This project uses target-specific sdkconfig.defaults files to maintain common settings across boards:

```
sdkconfig.defaults           # Common config (IRAM savings, stack sizes, etc.)
sdkconfig.defaults.esp32     # ESP32-specific (PSRAM Quad, Arduino variant)
sdkconfig.defaults.esp32s3   # ESP32-S3-specific (PSRAM Octal, Arduino variant)
```

ESP-IDF loads them in order, so target-specific files override the base defaults.

The `XIAO_ESP32S3_SENSE_ENABLED` define is automatically added via `CMakeLists.txt` when building for ESP32-S3.

## Quick Reference Commands

```bash
# Clean build
idf.py fullclean

# Set target
idf.py set-target esp32s3

# Configure (optional - defaults are auto-applied)
idf.py menuconfig

# Build
idf.py build

# Flash and monitor (adjust port as needed)
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Board-Specific Features

### XIAO ESP32S3 Sense
When `XIAO_ESP32S3_SENSE_ENABLED` is defined:
- **Camera**: OV2640 on expansion board (GPIO39/40 for I2C)
- **PDM Microphone**: CLK=GPIO42, DATA=GPIO41
- **SD Card Slot**: CS=GPIO3, SCK=GPIO7, MISO=GPIO8, MOSI=GPIO10
- **No built-in NeoPixel** (external only)

### Adafruit QT Py ESP32 Pico
- **Built-in NeoPixel**: GPIO5, power on GPIO8
- **No camera/mic/SD** (external only)
- **STEMMA QT I2C**: SDA=GPIO22, SCL=GPIO19

## Troubleshooting

### "Wrong chip type" error
You need to `fullclean` and `set-target` when switching between ESP32 and ESP32-S3.

### Board not detected in BuildConfig
Check that `CONFIG_ARDUINO_VARIANT` in `sdkconfig` matches a supported board in `System_BuildConfig.h`.

### SD card not appearing
For XIAO Sense, ensure `XIAO_ESP32S3_SENSE_ENABLED` is defined so the correct SD pins are used.
