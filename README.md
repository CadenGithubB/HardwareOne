<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-black.svg">
  <img alt="Hardware One logo" src="assets/logo-black.svg" width="140">
</picture>

# Hardware One v0.99.7

**Hardware One is a modular ESP32 firmware that works like a distributed operating system for cheap microcontrollers.**

</div>

You compile and flash each ESP32 chip to fit a specific job: a smart-home sensor, a Smart Glasses companion gadget made to live in your pocket or backpack, a headless mesh node, a camera node that captures photos and beams them across the mesh, and many more. No matter what feature set a device is built for, every one of them speaks the same custom ESP-NOW protocol, letting them form a private, router-free mesh. And because each device brings its own capabilities to that mesh, they can pool their data and work together - turning a scattered collection of chips into one system you can monitor from a single dashboard.

On any single device, control works the same way no matter how you reach it: one command system - the CLI - issued over USB serial, a browser, the on-device screen + gamepad, Bluetooth, voice, or another node on the mesh. Same commands, same permission checks, every way in.

> Built on **ESP-IDF** (not Arduino IDE). Runs on the **Seeed XIAO ESP32-S3**, **Unexpected Maker FeatherS3**, **Adafruit Feather ESP32**, and several other ESP32 / ESP32-S3 boards.

---

## Configurations

Hardware One can be used in several different ways depending on the hardware you attach and the role you want the device to play:

### 1) Barebones / Headless Node
- Just the microcontroller board - no display, no sensors, no gamepad.
- Full web UI, ESP-NOW, WiFi, CLI, MQTT, automation, and remote management features still available.
- Good for relay nodes and remote endpoints.

### 2) Sensor Appliance
- Build a dedicated single-purpose device around one or more sensors, such as IMU, gamepad, thermal sensor, GPS, RTC, ToF, or presence.
- Useful for fixed installs where you want one job done well without having all features compiled in.
- Can still expose data over web, CLI, automations, MQTT, and ESP-NOW like the barebones / headless node.

### 3) Hardware One (Standard Handheld)
- The intended full build: board + SSD1306 OLED + Seesaw gamepad (or ANO rotary encoder) + a selection of I2C sensors.
- Can be used from USB power or as a battery-powered handheld.
- Best fit when you want both the local OLED/gamepad UI and the web UI.

### 5) Wearable Companion
- Board + Bluetooth, paired with Even Realities G2 smart glasses and/or an R1 smart ring.
- The firmware drives the G2 lens directly as a full six-category UI - not a notification mirror - so the device in your pocket is operable without taking out a phone.
- With an R1 ring, adds health vitals (heart rate, HRV, SpO2, temperature) with on-lens graphs and background Health Track logging.
- Composes with any of the above: the glasses are another interface onto the same command system, not a separate build.

### 4) Bonded Microcontrollers
- Control features unique to one device you flash while another device is flashed with other features - effectively removing the limit of software features that is faced due to iram constrictions on the ESP32.
- This was intended to be used in a way where one unit is the device which deals with the Display/Input Devices and bluetooth connectivity, while the other device exposes other hardware / sensors, or other software features.
- The devices create an auth token during the bond sync / handshake process. This is used to execute commands with implicit trust between the devices to reduce the need to enter in username + password for every remote command.
- Command registries are shared between bonded peers, so when a command is queued for execution there is a check to see if the command trying to be executed is able to be found on the local command registry, or if its found on the bonded device's command registry. From there it will either execute the command locally, or reroute the command to the bonded device which will enqueue the command (so it is the same code path as a standard command), and then send the output back via ESP-NOW streaming.


---

## Software Features

<ins>Key</ins>: ✅ Intended for this deployment &nbsp; ❌ Not Available &nbsp; ⚙️ Configurable

> All features can be enabled or disabled via `System_BuildConfig.h` to match your hardware and use case.

> The **Wearable Companion** configuration is not a column here - the G2 glasses and R1 ring are interfaces that compose with any of the four builds below.

| Feature | Barebones | Sensor Appliance | Standard Handheld | Bonded |
| ------- | :-------: | :--------------: | :---------------: | :----: |
| Serial CLI with full command system | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| LittleFS file system | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| Data logging (CSV export to LittleFS) | ⚙️ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;✅ |
| WiFi (connect, auto-reconnect, AP scan) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Web UI (browser-based control & monitoring) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Authentication (4 role tiers: guest / user / admin / super admin) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| HTTPS (TLS web server, self-signed or uploaded certs) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Notifications (OLED banners, web toasts, G2 cards, notification center) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| Backup & restore (`.hwbackup` migration between devices) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| ESP-NOW V3 (peer discovery, pairing, bonding) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| ESP-NOW metadata sync & file transfer | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| MQTT (Home Assistant integration) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Automations (scheduled & conditional commands) | ✅ | ✅ | ✅ | ⚙️&nbsp;+&nbsp;✅ |
| Seesaw gamepad input | ❌ | ❌ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| ANO rotary encoder input (alternative to the gamepad) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| OLED display for onboard visuals | ❌ | ⚙️ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| BNO055 IMU (9-DoF orientation) | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| VL53L4CX Time-of-Flight distance sensor | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| MLX90640 / AMG8833 thermal camera | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| APDS9960 gesture / proximity / RGB sensor | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| PA1010D GPS + offline maps | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| DS3231 RTC (hardware clock) | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| STHS34PF80 IR presence / motion | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| DVP camera (OV2640 / OV5640) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| PDM microphone (I2S audio capture) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| TEA5767 FM Radio receiver | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| ESP-SR voice commands (wake word + command recognition) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| BLE server + Even Realities G2 glasses client | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| R1 smart ring - health vitals, graphs, Health Track logging | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Offline maps + waypoints | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Browser games (Tilt Maze or A Dark Room - one per build) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| On-device LLM inference (ESP32-S3 + PSRAM only) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Edge Impulse ML inference | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Battery monitoring (LiPo voltage via ADC) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| PCA9685 servo controller | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |

> If a module is enabled in the build config but not physically connected, its commands will gracefully fail - nothing breaks.

---

## Supported Hardware

### Boards - pick one

Each device in your setup runs one board. Multiple boards can coexist on the same ESP-NOW mesh network simultaneously.

| Board | Flash / PSRAM | Camera | PDM Mic | Battery monitor | Notes |
| ----- | :-----------: | :----: | :-----: | :-------------: | ----- |
| Seeed XIAO ESP32-S3 (+ Sense) | 8 / 8 MB | ✅ | ✅ | ❌ | Camera, mic & microSD come on the Sense expansion. Primary dev target |
| Unexpected Maker FeatherS3 | 16 / 8 MB | ❌ | ❌ | ✅ | Quad PSRAM; dual STEMMA QT (second I2C bus); onboard RGB LED; MAX17048 fuel gauge |
| Adafruit Feather ESP32 V2 | 8 / 2 MB | ❌ | ❌ | ✅ (GPIO35) | Onboard NeoPixel + battery monitoring |
| Adafruit QT Py ESP32 | 8 / 2 MB | ❌ | ❌ | ❌ | ESP32-PICO; STEMMA QT onboard; compact |

> A **generic ESP32** fallback build also exists for unlisted boards - it compiles, but verify the I2C pins match your hardware.

### Peripherals - Stemma QT / I2C

These connect via Stemma QT (or standard I2C) and work the same on any supported board. Mix and match as needed.

| Peripheral | Link |
| ---------- | ---- |
| SSD1306 OLED display | [ID: 326](https://www.adafruit.com/product/326) |
| Adafruit Seesaw Gamepad | [ID: 5743](https://www.adafruit.com/product/5743) |
| Adafruit ANO Rotary Encoder breakout (seesaw-based, I2C `0x49`) - alternative to the gamepad, pick one | — |
| BNO055 9-DoF IMU | [ID: 4646](https://www.adafruit.com/product/4646) |
| VL53L4CX Time-of-Flight sensor | [ID: 5425](https://www.adafruit.com/product/5425) |
| MLX90640 32×24 Thermal Camera | [ID: 4407](https://www.adafruit.com/product/4407) |
| Adafruit AMG8833 8×8 Thermal Camera | [ID: 3538](https://www.adafruit.com/product/3538) |
| APDS9960 Gesture / Light sensor | [ID: 3595](https://www.adafruit.com/product/3595) |
| PA1010D GPS module | [ID: 4415](https://www.adafruit.com/product/4415) |
| DS3231 RTC | [ID: 5188](https://www.adafruit.com/product/5188) |
| STHS34PF80 IR presence sensor | [ID: 6426](https://www.adafruit.com/product/6426) |
| TEA5767 FM Radio module | [ID: 1712](https://www.adafruit.com/product/1712) |
| PCA9685 servo driver | [ID: 815](https://www.adafruit.com/product/815) |
| Stemma QT hub (for chaining) | [ID: 5625](https://www.adafruit.com/product/5625) |

### Peripherals - board-specific

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

If your build enables the G2 glasses / R1 ring Bluetooth support, first apply the small local patches to the managed Arduino BLE component: see [docs/arduino-local-patches/](docs/arduino-local-patches/) (patch file, verify script, and instructions). Building those features against a stock copy of the library fails to link.

---

> ## Get up and running quickly: [Quick Start Guide](docs/QUICKSTART.md)

> ## Full reference, commands, and configuration: [User Guide](docs/USERGUIDE.md)

> ## Every command, generated from source: [Command Reference](docs/COMMAND_REFERENCE.md)
