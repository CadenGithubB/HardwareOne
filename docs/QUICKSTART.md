# Quick Start Guide

This guide will help you get up and running with Hardware One.

## Hardware Setup

### Barebones
Just your board and a USB-C cable. Connect it to your computer and continue to Software Setup.

### Hardware One (Standard — wired or wireless)

> **NOTE:** This assumes you have already soldered headers to any modules that require them.

1. Connect your I2C sensors and peripherals via Stemma QT cables. If you have more than one, use a Stemma QT hub to chain them.
2. Connect the SSD1306 OLED display via I2C.
3. Connect the Seesaw Gamepad via Stemma QT.
4. If using a **wireless** build, connect your LiPo battery to the board's JST connector. Make sure the power switch is in the **Off** position before continuing.
5. Connect the board to your computer via USB-C.
6. Continue to Software Setup.

> **Before powering on:** double-check that all power and ground connections are correct. Reversing polarity can damage components or the battery.

---

## Software Setup

Hardware One uses **ESP-IDF**, not Arduino IDE. If you don't have it installed:

- [ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
- Install the version matching your target chip (ESP32 or ESP32-S3).

### 1. Clone the repo

```bash
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne
```

### 2. Configure your build (optional)

Open `components/hardwareone/System_BuildConfig.h` and enable or disable any features you want — sensors, web modules, ESP-NOW, MQTT, etc. The defaults are set for the standard full build. If you're happy with defaults, skip this step.

### 3. Set your target board and flash

```bash
# Set the chip target (do this once, or any time you switch chip families)
idf.py set-target esp32s3    # XIAO ESP32-S3 or QT PY ESP32-S3
idf.py set-target esp32      # Adafruit QT PY ESP32 or Feather ESP32

# Build
idf.py build

# Flash and open serial monitor (replace PORT with your device's port)
idf.py -p PORT flash monitor
```

> You can also run `idf.py -p PORT flash monitor` directly — it will build automatically if anything has changed. The separate `build` step is useful if you want to confirm the build succeeds before connecting the device.

- **XIAO / QT PY ESP32-S3:** port is usually `/dev/cu.usbmodem*` (native USB). You may need to hold the BOOT button when initiating the flash.
- **QT PY ESP32 / Feather ESP32:** port is usually `/dev/cu.usbserial*` or `/dev/cu.SLAB*` (UART bridge).

That's it. The build can take a few minutes the first time.

---

## Switching Between Board Families

If you are switching between an **ESP32** board (QT PY, Feather) and an **ESP32-S3** board (XIAO, QT PY S3), a full clean is required — the two chip families have different architectures and the build cache is not compatible.

```bash
idf.py fullclean
idf.py set-target esp32s3    # or esp32
idf.py -p PORT flash monitor
```

The project includes target-specific defaults (`sdkconfig.defaults.esp32` and `sdkconfig.defaults.esp32s3`) that handle the most important differences automatically, including:

- **PSRAM mode:** ESP32 uses Quad SPI PSRAM at 40 MHz; ESP32-S3 uses Octal SPI PSRAM at 80 MHz. Using the wrong mode will cause a boot failure or crash.
- **Flash mode:** ESP32 uses `DIO`; ESP32-S3 can use `QIO`.
- **Bluetooth:** ESP32 supports Classic BT + BLE 4.2; ESP32-S3 is **BLE 5.0 only** — no Classic Bluetooth.

If you need to deviate from the defaults, run `idf.py menuconfig` after `set-target`. See [BOARD_SWITCHING.md](BOARD_SWITCHING.md) for the full per-board menuconfig reference.

---

## First-Time Use

Once the device is flashed and powered on, you'll see the boot sequence in the serial monitor. From here, setup can happen two ways — use whichever is more convenient.

### Option A: OLED Setup Wizard (if display is connected)

On first boot, the device will launch a setup wizard on the OLED display. Navigate with the gamepad:

1. **WiFi** — enter your SSID and password.
2. **Device name / room / zone** — set the identity used for ESP-NOW mesh.
3. **Confirm** — settings are saved to flash. The wizard won't run again unless you reset settings.

### Option B: Serial Console

Open the serial monitor (115200 baud) and type commands directly:

```
setssid YourWiFiName
setpass YourWiFiPassword
wifi
```

To start the web server:
```
webstart
```

The device will print its IP address. Navigate to it in a browser to access the full web UI.

To have the web server start automatically on every boot:
```
webauto on
```

Type `help` at any time to see all available commands. Use `scan` to detect connected I2C sensors.

### Web UI

Once connected, the web interface gives you:
- Real-time sensor data
- ESP-NOW mesh management (pair, bond, sync metadata, file transfer)
- Remote command execution
- MQTT configuration
- Automations editor
- Settings

---

> ## Back to the overview: [README](../README.md)

> ## Full reference, commands, and configuration: [User Guide](USERGUIDE.md)

