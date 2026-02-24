# Hardware One

> Hardware One is an ESP32-based firmware platform that turns a small microcontroller board into a capable, networked device. Attach sensors, a display, and input hardware, flash the firmware, and you have a standalone device with a web UI, an OLED interface, an ESP-NOW mesh network, and a full CLI — all configurable without recompiling.

Built on **ESP-IDF** (not Arduino IDE). Runs on the **Seeed XIAO ESP32-S3**, **Adafruit QT PY ESP32**, and **Adafruit Feather ESP32** boards.

---

## Configurations

There are three ways to use Hardware One, depending on how much hardware you want to attach:

### 1) Barebones
- Just the microcontroller board — no display, no sensors, no gamepad.
- Full web UI, ESP-NOW, WiFi, CLI, and all network features still available.
- Good starting point for headless or custom builds.

### 2) Hardware One (Standard)
- The intended full build: board + SSD1306 OLED + Seesaw gamepad + a selection of I2C sensors.
- Wired (USB power) or wireless (LiPo battery) variants.
- Everything works out of the box.

### 3) DIY
- Fork the repo, enable or disable any feature in `System_BuildConfig.h`, and build whatever you need.
- All subsystems are individually toggleable — add new hardware by wiring it up and enabling the relevant flag.

---

## Software Features

<ins>Key</ins>: ✅ Available, but able to be disabled &nbsp; ❌ Not available

> DIY can enable or disable any feature individually via `System_BuildConfig.h`.

| Feature | Barebones | Standard |
| ------- | :-------: | :------: |
| WiFi (connect, auto-reconnect, AP scan) | ✅ | ✅ |
| Web UI (browser-based control & monitoring) | ✅ | ✅ |
| Web authentication (user accounts, sessions) | ✅ | ✅ |
| Serial / web CLI with full command system | ✅ | ✅ |
| ESP-NOW V3 mesh (peer discovery, pairing, bonding) | ✅ | ✅ |
| ESP-NOW metadata sync & file transfer | ✅ | ✅ |
| MQTT (Home Assistant integration) | ✅ | ✅ |
| Automations (scheduled & conditional commands) | ✅ | ✅ |
| OLED display with full menu system | ❌ | ✅ |
| Seesaw gamepad input | ❌ | ✅ |
| BNO055 IMU (9-DoF orientation) | ❌ | ✅ |
| VL53L4CX Time-of-Flight distance sensor | ❌ | ✅ |
| MLX90640 / AMG8833 thermal camera | ❌ | ✅ |
| APDS9960 gesture / proximity / RGB sensor | ❌ | ✅ |
| PA1010D GPS + offline maps | ❌ | ✅ |
| DS3231 RTC (hardware clock) | ❌ | ✅ |
| STHS34PF80 IR presence / motion | ❌ | ✅ |
| DVP camera (OV2640 / OV5640) | ❌ | ✅ |
| BLE server + Even Realities G2 glasses client | ❌ | ✅ |
| Edge Impulse ML inference | ❌ | ✅ |
| Battery monitoring (LiPo voltage via ADC) | ❌ | ✅ |
| PCA9685 servo controller | ❌ | ✅ |

> If a module is enabled in the build config but not physically connected, its commands will gracefully fail — nothing breaks.

---

## Supported Hardware

### Boards — pick one

Each device in your setup runs one board. Multiple boards can coexist on the same ESP-NOW mesh network simultaneously.

| Board | Camera | PDM Mic | Battery monitor | Notes |
| ----- | :----: | :-----: | :-------------: | ----- |
| Seeed XIAO ESP32-S3 | ✅ | ✅ | ❌ | Primary dev target |
| Adafruit QT PY ESP32-S3 | ✅ | ✅ | ❌ | Stemma QT onboard |
| Adafruit Feather ESP32 | ❌ | ❌ | ✅ (GPIO35) | Good for battery builds |

### Peripherals — Stemma QT / I2C

These connect via Stemma QT (or standard I2C) and work the same on any supported board. Mix and match as needed — a hub lets you chain multiple sensors.

| Peripheral | Link |
| ---------- | ---- |
| SSD1306 OLED display | Any 128×64 I2C OLED |
| Adafruit Seesaw Gamepad | [ID: 5743](https://www.adafruit.com/product/5743) |
| BNO055 9-DoF IMU | — |
| VL53L4CX Time-of-Flight sensor | [ID: 5425](https://www.adafruit.com/product/5425) |
| MLX90640 32×24 Thermal Camera | — |
| Adafruit AMG8833 8×8 Thermal Camera | [ID: 3538](https://www.adafruit.com/product/3538) |
| APDS9960 Gesture / Light sensor | [ID: 3595](https://www.adafruit.com/product/3595) |
| PA1010D GPS module | — |
| DS3231 RTC | — |
| STHS34PF80 IR presence sensor | — |
| PCA9685 servo driver | — |
| Stemma QT hub (for chaining) | [ID: 5625](https://www.adafruit.com/product/5625) |

### Peripherals — board-specific

| Peripheral | Compatible boards |
| ---------- | ----------------- |
| DVP camera (OV2640 / OV5640) | XIAO ESP32-S3, QT PY ESP32-S3 |
| PDM microphone (I2S) | XIAO ESP32-S3, QT PY ESP32-S3 |
| LiPo battery + BMS | Any board with a JST connector |

---

## Build System

Hardware One uses **ESP-IDF** (not Arduino IDE). The quickest way to get going:

```bash
# Clone
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne

# Build and flash (replace PORT with your device's serial port)
idf.py -p PORT flash monitor
```

All user-configurable options (which sensors, which web modules, which network features) live in one file: `components/hardwareone/System_BuildConfig.h`. Flip the flags, rebuild, done.

---

> ## Get up and running quickly: [Quick Start Guide](docs/QUICKSTART.md)

> ## Full reference, commands, and configuration: [User Guide](docs/USERGUIDE.md)
