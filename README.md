<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-black.svg">
  <img alt="Hardware One logo" src="assets/logo-black.svg" width="140">
</picture>

# Hardware One v0.99.92

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

### 4) Bonded Microcontrollers
- Control features unique to one device you flash while another device is flashed with other features - effectively removing the limit of software features that is faced due to iram constrictions on the ESP32.
- This was intended to be used in a way where one unit is the device which deals with the Display/Input Devices and bluetooth connectivity, while the other device exposes other hardware / sensors, or other software features.
- The devices create an auth token during the bond sync / handshake process. This is used to execute commands with implicit trust between the devices to reduce the need to enter in username + password for every remote command.
- Command registries are shared between bonded peers, so when a command is queued for execution there is a check to see if the command trying to be executed is able to be found on the local command registry, or if its found on the bonded device's command registry. From there it will either execute the command locally, or reroute the command to the bonded device which will enqueue the command (so it is the same code path as a standard command), and then send the output back via ESP-NOW streaming.

### 5) Wearable Companion
- Board + Bluetooth, paired with Even Realities G2 smart glasses and/or an R1 smart ring.
- The firmware drives the G2 lens directly as a full six-category UI - not a notification mirror - so the device in your pocket is operable without taking out a phone.
- On-lens text entry is an arrow-pad QWERTY keyboard: taps move a cursor around a three-page key grid, a ring double-tap types the highlighted key, and a Mic row hands the field to the Pi co-processor's speech-to-text when one is attached.
- With an R1 ring, adds health vitals (heart rate, HRV, SpO2, temperature) with on-lens graphs and background health logging; which of those a ring can be polled for on demand depends on its firmware profile.
- Composes with any of the above: the glasses are another interface onto the same command system, not a separate build.

### 6) Pi Co-Processor Pair
- Board + a Raspberry Pi (Compute Module 5 or Pi 5) wired over a dedicated UART. The ESP32 stays the device - same CLI, same auth, same mesh - and the Pi is a co-processor it can hand work to.
- The Pi runs one Linux daemon that speaks the link, does speech-to-text and LLM generation, and bridges power/fan control. The firmware treats it as a selectable LLM source (`cm5:<model>`), a dictation engine for the OLED and on-lens keyboards, the answering side of a native "Hey Even" voice session on the G2 glasses, and a time source.
- The Pi logs into the device like any other user, sends a heartbeat so the firmware knows when it is actually ready, and its power state is driven by confirmed `cm5 power` / `cm5 fan` requests.
- The Pi-side software lives in its own repository: **[HardwareOne_RaspPi_CoProcessor](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor)**. Wiring, setup and commands are in the [User Guide](docs/USERGUIDE.md#raspberry-pi-co-processor-cm5).

---

## Software Features

<ins>Key</ins>: ✅ Intended for this deployment &nbsp; ❌ Not Available &nbsp; ⚙️ Configurable

> Features are turned on and off in `System_BuildConfig.h` to match your hardware and use case. That file is yours to edit, so whatever values it happens to carry in a fresh clone are a starting point, not a fixed set of defaults. A few options are chosen at build time instead of in that file; where that is the case, the row below names the switch.

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
| Signed OTA firmware updates - opt-in `HW_OTA_LAYOUT=1` build-time layout, not a `System_BuildConfig.h` flag (staged to the filesystem, over Bluetooth on builds that include it, or uploaded to the recovery SoftAP; applied by a factory recovery image) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
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
| R1 smart ring - health vitals, graphs, health logging | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Offline maps + waypoints | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Browser games (Tilt Maze or A Dark Room - one per build) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| LLM assistant (tiny on-device model on ESP32-S3 + PSRAM, and/or answered by the Pi co-processor) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Raspberry Pi co-processor over UART (LLM, speech-to-text, dictation, power/fan control, clock) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
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

> The opt-in **recovery OTA layout** (`HW_OTA_LAYOUT=1`) is available for the FeatherS3, the Feather ESP32 V2 and the QT Py ESP32. The XIAO ESP32-S3 has no OTA layout yet, so it updates over the cable.

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
| Raspberry Pi CM5 / Pi 5 co-processor (UART) | Any board - each board block defines a link UART; live audio streaming needs the ESP32-S3 boards (≥ 921,600 baud) |

---

## Raspberry Pi Co-Processor

A Raspberry Pi (CM5 or Pi 5) can sit beside the ESP32 on a dedicated UART and take the work the microcontroller cannot do well: speech-to-text, a real LLM, and its own power and thermal management. The firmware half is in this repository; the Pi half - a single daemon (`hw1-ai-service`) that speaks the link, runs the STT/LLM engines and bridges power/fan control - is here:

> **https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor**

What you get with the pair: ask the Pi questions from the web LLM page, the OLED, or the glasses; dictate into any OLED or on-lens text field; answer "Hey Even" on the G2 glasses with no phone involved; `cm5 power` / `cm5 fan` control with confirmed request/ACK; and a clock source for dark boots. Wiring, setup steps and the command list are in the [User Guide](docs/USERGUIDE.md#raspberry-pi-co-processor-cm5).

---

## Build System

Hardware One uses **ESP-IDF** (not Arduino IDE). The quickest way to get going:

```bash
# Clone
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne

# Build and flash (replace PORT with your device's serial port)
idf.py -p PORT flash monitor

# Or build one specific board (see boards/ for the list)
tools/build_board.sh xiao_s3 -p PORT flash monitor
```

`tools/build_board.sh <board> [idf.py args...]` gives each board its own `build-<board>/` directory and its own sdkconfig, so switching boards never needs an `idf.py fullclean`.

The feature flags (which sensors, which web modules, which network features) live in one file: `components/hardwareone/System_BuildConfig.h`. Set the flags for the device you are building, rebuild, done - the values already in that file are one example configuration, not a contract. The recovery OTA layout is the exception: it is selected at build time with `HW_OTA_LAYOUT=1`, not by a flag in that file.

If your build enables the G2 glasses / R1 ring Bluetooth support, first apply the small local patches to the managed Arduino BLE component: see [docs/arduino-local-patches/](docs/arduino-local-patches/) (patch file, verify script, and instructions). Building those features against a stock copy of the library fails to link.

---

> ## Get up and running quickly: [Quick Start Guide](docs/QUICKSTART.md)

> ## Full reference, commands, and configuration: [User Guide](docs/USERGUIDE.md)

> ## Every command, generated from source: [Command Reference](docs/COMMAND_REFERENCE.md)
