<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-black.svg">
  <img alt="Hardware One logo" src="assets/logo-black.svg" width="140">
</picture>

# Hardware One v0.99.99.1

**Hardware One is a modular ESP32 firmware that works like a distributed operating system for cheap microcontrollers.**

</div>

You compile and flash each ESP32 chip to fit a specific job: a smart-home sensor, a Smart Glasses companion gadget made to live in your pocket or backpack, a headless mesh node, a camera node that captures photos and beams them across the mesh, and many more. No matter what feature set a device is built for, every one of them speaks the same custom ESP-NOW protocol, letting them form a private, router-free mesh. And because each device brings its own capabilities to that mesh, they can pool their data and work together - turning a scattered collection of chips into one system you can monitor from a single dashboard.

On any single device, control works the same way no matter how you reach it: one command system - the CLI - issued over USB serial, a browser, the on-device screen + gamepad, Bluetooth, voice, or another node on the mesh. Same commands, same permission checks, every way in.

> Built on **ESP-IDF** (not Arduino IDE). Runs on the **Seeed XIAO ESP32-S3**, **Unexpected Maker FeatherS3**, **Adafruit Feather ESP32**, and several other ESP32 / ESP32-S3 boards.

> **ESP32-P4X-EYE is newly supported** (`tools/build_board.sh p4x_eye`). Its Wi-Fi, Bluetooth and ESP-NOW run on the onboard ESP32-C6, whose companion firmware is built and flashed from [`tools/p4/companion`](tools/p4/companion/README.md). See [P4X-EYE notes](docs/P4X_EYE_PERIPHERALS.md) for setup and what is not yet qualified. Every board builds on **ESP-IDF 5.5.5**.

---

## Configurations

Hardware One can be used in several different ways depending on the hardware you attach and the role you want the device to play:

These are starting feature selections. A board can use any combination supported
by its attached hardware, implemented backends and available resources. Use
[saved configurations](configurations/README.md) to customize those choices.

### 1) Headless Node
- Just the microcontroller board - no display, no sensors, no gamepad.
- WiFi, web, ESP-NOW, Bluetooth, Automations, CLI, signed recovery OTA, and remote management remain available in the checked-in lean deployment.
- Good for relay nodes and remote endpoints.
- Reproducible per-board profiles live under [`deployments/headless/`](deployments/headless/README.md); this is a deployment family, not a new hardware board.

### 2) Sensor Appliance
- Build a dedicated single-purpose device around one or more sensors, such as IMU, gamepad, thermal sensor, GPS, RTC, ToF, or presence.
- Useful for fixed installs where you want one job done well without having all features compiled in.
- Can still expose data over web, CLI, automations, MQTT, and ESP-NOW like a Headless Node when those optional modules are included.

### 3) Hardware One (Standard Handheld)
- The intended full build: board + SSD1306 OLED + Seesaw gamepad (or ANO rotary encoder) + a selection of I2C sensors.
- Can be used from USB power or as a battery-powered handheld.
- Best fit when you want both the local OLED/gamepad UI and the web UI.
- The ESP32-P4X-EYE is this configuration with its own LCD, wheel, camera and microphone; its reproducible profile lives under [`deployments/handheld/`](deployments/handheld/README.md) as a factory-only (cable-flashed) deployment.

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

| Feature | Headless Node | Sensor Appliance | Standard Handheld | Bonded |
| ------- | :-------: | :--------------: | :---------------: | :----: |
| Serial CLI with full command system | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| LittleFS file system | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| Data logging (CSV export to LittleFS) | ⚙️ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;✅ |
| WiFi (connect, auto-reconnect, AP scan) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Web UI (browser-based control & monitoring) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Authentication (4 role tiers: guest / user / admin / super admin) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| HTTPS (TLS web server, self-signed or uploaded certs) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Notifications (OLED banners, web toasts, G2 cards, notification center) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| Backup & restore (`.hwbackup` migration between devices) | ⚙️ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Signed OTA firmware updates - opt-in `HW_OTA_LAYOUT=1` build-time layout, not a `System_BuildConfig.h` flag (staged to the filesystem, over Bluetooth on builds that include it, or uploaded to the recovery SoftAP; applied by a factory recovery image) | ✅ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| ESP-NOW V3 (peer discovery, pairing, bonding) | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| ESP-NOW metadata sync & file transfer | ✅ | ✅ | ✅ | ✅&nbsp;+&nbsp;✅ |
| MQTT (Home Assistant integration) | ❌ | ✅ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| Automations (scheduled & conditional commands) | ✅ | ✅ | ⚙️ (board builds; compiled out of the checked-in P4 handheld deployment) | ⚙️&nbsp;+&nbsp;✅ |
| Seesaw gamepad input | ❌ | ❌ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| ANO rotary encoder input (alternative to the gamepad) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| OLED display for onboard visuals | ❌ | ⚙️ | ✅ | ✅&nbsp;+&nbsp;⚙️ |
| BNO055 IMU (9-DoF orientation) | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| VL53L4CX Time-of-Flight distance sensor | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| MLX90640 thermal camera | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| APDS9960 gesture / proximity / RGB sensor | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| PA1010D GPS + offline maps | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| DS3231 RTC (hardware clock) | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| STHS34PF80 IR presence / motion | ❌ | ⚙️ | ⚙️ | ✅&nbsp;+&nbsp;⚙️ |
| DVP camera (OV2640 / OV5640) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| PDM microphone (I2S audio capture) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| RDA5807M FM radio receiver | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| ESP-SR voice commands (wake word + command recognition) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;✅ |
| BLE server + Even Realities G2 glasses client | ✅ server / ❌ G2 | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| R1 smart ring - health vitals, graphs, health logging | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Offline maps + waypoints | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| A Dark Room browser game | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| LLM assistant (tiny on-device model, needs PSRAM, and/or answered by the Pi co-processor) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Raspberry Pi co-processor over UART (LLM, speech-to-text, dictation, power/fan control, clock) | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Edge Impulse ML inference | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| Battery monitoring (LiPo voltage via ADC) | ✅ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |
| PCA9685 servo controller | ❌ | ⚙️ | ⚙️ | ⚙️&nbsp;+&nbsp;⚙️ |

> If a module is enabled in the build config but not physically connected, its commands will gracefully fail - nothing breaks.

---

## Supported Hardware

### Boards - pick one

Each device in your setup runs one board. Multiple boards can coexist on the same ESP-NOW mesh network simultaneously.

| Board | Flash / PSRAM | Camera | PDM Mic | Battery monitor | Notes |
| ----- | :-----------: | :----: | :-----: | :-------------: | ----- |
| Seeed XIAO ESP32-S3 (+ Sense) | 8 / 8 MB | ✅ | ✅ | ❌ | Camera, mic & microSD come on the Sense expansion. Primary dev target |
| Unexpected Maker FeatherS3 | 16 / 8 MB | ❌ | ❌ | ✅ | Quad PSRAM; dual STEMMA QT (second I2C bus); onboard RGB LED; MAX17048 fuel gauge. Also built as `feathers3_fe` (flash encryption; experimental, first boot burns eFuses) |
| Adafruit Feather ESP32 V2 | 8 / 2 MB | ❌ | ❌ | ✅ (GPIO35) | Onboard NeoPixel + battery monitoring |
| Adafruit QT Py ESP32 | 8 / 2 MB | ❌ | ❌ | ❌ | ESP32-PICO; STEMMA QT onboard; compact |
| Espressif ESP32-P4X-EYE | 16 / 32 MB | ✅ (MIPI) | ✅ | ✅ (voltage) | 240×240 LCD, wheel + 3 buttons; radio via onboard ESP32-C6; local speech-to-text (needs a separately installed model file: the saved P4 configurations read the published model from microSD; the plain board build keeps microSD off and expects an unpublished model, see [Local speech-to-text model](#local-speech-to-text-model-esp32-p4)). [Notes](docs/P4X_EYE_PERIPHERALS.md), [enclosure](physical_enclosures/p4x_eye/README.md) |
| Waveshare ESP32-P4 Touch LCD 5 | 32 / 32 MB | ✅ (MIPI, saved profile) | ✅ (ES7210) | ✅ (ADC) | Experimental. 5-inch 720×1280 HX8394 DSI panel with GT911 touch; radio via onboard ESP32-C6; ES8311 speaker output (driver-tested only). [Notes](docs/WAVESHARE_P4_SUPPORT.md) |

> A **generic ESP32** fallback build also exists for unlisted boards - it compiles, but verify the I2C pins match your hardware.

### Support matrix

What each row promises, how it updates, and the last dated hardware record behind it. "Stable" rows
build from a clean clone at every release and are checked on hardware before a tag (`RELEASING.md`,
gate); "experimental" rows build and have been run, but are not held to the release checks.

| Board | Build | Tier | Update path | Last dated hardware record |
|---|---|---|---|---|
| Unexpected Maker FeatherS3 | `tools/build_board.sh feathers3`; release: `tools/build_deployment.sh headless feathers3` | stable | recovery OTA layout `hw1-f3-ota-v1` (signed, BLE or SoftAP) and cable | OTA end to end 2026-08-05 (v0.99.8); BLE/G2/R1 2026-07-30; needs re-qualification on the release image |
| Adafruit Feather ESP32 V2 | `feather_esp32_v2`; release: `tools/build_deployment.sh headless feather_esp32_v2` | stable | recovery OTA layout `hw1-fv2-ota-v1` and cable | OTA recovery path, setup, Wi-Fi, web 2026-08-25; BLE/G2 2026-08-19 |
| Seeed XIAO ESP32-S3 (+ Sense) | `xiao_s3 --config xiao-no-bluetooth` (Bluetooth off; the bare board build with Bluetooth does not fit its factory partition); release: `tools/build_deployment.sh pocket_assistant xiao_s3` (Bluetooth on, its own partition table) | stable (cable); OTA layout experimental until a dated record exists | cable; `pocket_assistant` OTA layout without a hardware record yet | 0.99.97.4 boot + serial check 2026-10-12 |
| Adafruit QT Py ESP32 | `qtpy_esp32` | stable (cable) | cable; OTA layout experimental (no record) | 0.99.97.4 boot + serial check 2026-10-12 |
| Espressif ESP32-P4X-EYE | `p4x_eye` (needs `tools/p4/prepare_sdk.sh`); release: `tools/build_deployment.sh handheld p4x_eye` | stable, cable-only | cable (app-only write; no OTA layout) | multi-hour STT bench runs and BLE investigations 2026-11-07/08 (`docs/P4X_PANIC_HUNT_20261107.md`) |
| FeatherS3 flash-encrypted (`feathers3_fe`) | `feathers3_fe` | experimental | `hw1-f3fe-ota-v1` | flash-encryption trial 2026-07-30 |
| Waveshare ESP32-P4 Touch LCD 5 | `waveshare_p4_touch_lcd_5` (needs `prepare_sdk.sh`) | experimental | cable | bring-up acceptance records 2026-10/11 (`docs/WAVESHARE_P4_*.md`) |
| generic ESP32 | default target | experimental | cable | none |

Automations are compiled out of the P4 handheld deployment contract (`deployments/handheld`); the
`p4x_eye` board build keeps them. Peers and what an update keeps: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md);
open problems: [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

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
| APDS9960 Gesture / Light sensor | [ID: 3595](https://www.adafruit.com/product/3595) |
| PA1010D GPS module | [ID: 4415](https://www.adafruit.com/product/4415) |
| DS3231 RTC | [ID: 5188](https://www.adafruit.com/product/5188) |
| STHS34PF80 IR presence sensor | [ID: 6426](https://www.adafruit.com/product/6426) |
| RDA5807M FM radio board (e.g. ScoutMakes FM Radio Board, I2C `0x11`) | — |
| PCA9685 servo driver | [ID: 815](https://www.adafruit.com/product/815) |
| Stemma QT hub (for chaining) | [ID: 5625](https://www.adafruit.com/product/5625) |

### Peripherals - board-specific

| Peripheral | Compatible boards |
| ---------- | ----------------- |
| DVP camera (OV2640 / OV5640) | XIAO ESP32-S3 Sense |
| MIPI-CSI camera | ESP32-P4X-EYE (onboard); Waveshare ESP32-P4 Touch LCD 5 (OV5647, enabled by the `waveshare-p4-bringup` saved profile) |
| PDM microphone | XIAO ESP32-S3 Sense, ESP32-P4X-EYE |
| ES7210 codec microphone | Waveshare ESP32-P4 Touch LCD 5 (onboard) |
| LiPo battery + BMS | Any board with a JST connector |
| Raspberry Pi CM5 / Pi 5 co-processor (UART) | Any board - each board block defines a link UART; live audio streaming needs the ESP32-S3 boards (≥ 921,600 baud) |


### Local speech-to-text model (ESP32-P4)

Local speech-to-text on the P4 boards reads its acoustic model from a file at
run time; the file is not part of the firmware image or of this repository.

- **Saved P4 configurations** (`p4-glasses-test`, `p4-phone-pipe`, `p4-openclaw`
  and the other `p4-*` profiles in `configurations/`, plus `waveshare-p4-bringup`)
  set `ENABLE_STT_MODEL_15X5_GEN2` and load
  `/sd/STT Models/quartznet15x5-gen2-25397.p4.stt` from the microSD card. That
  file is published as a release asset of the public speech-to-text repository:
  [`model-gen2-25397`](https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text/releases/tag/model-gen2-25397)
  → `quartznet15x5-gen2-25397.p4.stt`, 20,112,224 bytes, SHA-256
  `dccef3a8cead069d97d780e0d08ddfc65d9f0870c0af4673b29327f00158f048`
  (`SHA256SUMS.txt` in the same release lists it).
- **To install it**, download the file, check it with
  `shasum -a 256 quartznet15x5-gen2-25397.p4.stt`, and copy it onto the FAT32
  microSD card into a top-level folder named exactly `STT Models`, keeping the
  file name. The firmware refuses a file of the wrong size and checks the
  model's embedded checksum before using it. The release's `meeting.lm` is
  optional: copied into the same folder it is used as the language model;
  without it decoding is greedy.
- **The plain `p4x_eye` board build and the `handheld` deployment** keep
  microSD off, so they build without local transcription. Local STT on an
  ESP32-P4 always runs this 15×5 model from the card: enable it with
  `ENABLE_SDMMC_CARD=1` and `ENABLE_LOCAL_STT=1` (in the handheld deployment
  also `ENABLE_SD_CARD=1`); the model choice follows, and the build refuses
  local STT without the card.
- **Speaker labels** (on by default on `p4x_eye`) also read
  `/sd/STT Models/speaker.spk`, which is not published either. Without it,
  transcription still works and transcripts carry no speaker labels.

---

## Raspberry Pi Co-Processor

A Raspberry Pi (CM5 or Pi 5) can sit beside the ESP32 on a dedicated UART and take the work the microcontroller cannot do well: speech-to-text, a real LLM, and its own power and thermal management. The firmware half is in this repository; the Pi half - a single daemon (`hw1-ai-service`) that speaks the link, runs the STT/LLM engines and bridges power/fan control - is here:

> **https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor**

What you get with the pair: ask the Pi questions from the web LLM page, the OLED, or the glasses; dictate into any OLED or on-lens text field; answer "Hey Even" on the G2 glasses with no phone involved; `cm5 power` / `cm5 fan` control with confirmed request/ACK; and a clock source for dark boots. Wiring, setup steps and the command list are in the [User Guide](docs/USERGUIDE.md#raspberry-pi-co-processor-cm5).

---

## Build System

Use [saved build configurations](configurations/README.md) to choose features
without editing shared headers. For code changes, follow the
[development guidelines](CONTRIBUTING.md): clear ownership, small module
interfaces, shared validation, and independently selectable features.

Start guided setup to choose a board or saved configuration, select features,
and review before saving or building:

```sh
python3 tools/build_config.py configure
```

Hardware One uses **ESP-IDF** (not Arduino IDE). The quickest way to get going:

```bash
# Clone
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne

# Vendor the patched Arduino component (once per clone; needs ESP-IDF 5.5.5 exported)
tools/vendor_arduino.sh

# Build and flash one board (replace PORT with your device's serial port;
# run tools/build_board.sh with no arguments to list the boards)
tools/build_board.sh feathers3 -p PORT flash monitor

# A saved feature configuration (see configurations/): the XIAO ESP32-S3 without Bluetooth
tools/build_board.sh xiao_s3 --config xiao-no-bluetooth -p PORT flash monitor
```

`tools/build_board.sh <board> [idf.py args...]` gives each board its own `build-<board>/` directory and its own sdkconfig, so switching boards never needs an `idf.py fullclean`.

Always build through `tools/build_board.sh` (or `tools/build_config.py build` /
`tools/build_deployment.sh`, which also pass a board and a per-build sdkconfig).
A bare `idf.py` in the repository root is not a supported build: it reads the
tracked `config/sdkconfig`, which is set up for the XIAO ESP32-S3 (8 MB flash,
octal PSRAM), while with no `HW_BOARD` the root `CMakeLists.txt` picks the
default board for the chip (`feathers3` for esp32s3, `qtpy_esp32` for esp32) and
that board's partition table. The two do not fit together (for esp32s3, a 16 MB
table in an 8 MB flash setting), so configure stops before touching the tracked
`config/sdkconfig`, with a message naming `tools/build_board.sh`.

Checked-in deployment profiles add a second, independent axis on top of the
physical board. For example, paired Headless Node updater/main releases are:

```bash
HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  tools/build_deployment.sh headless feather_esp32_v2

HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \
  tools/build_deployment.sh headless feathers3
```

Ordinary builds take their feature flags (sensors, web modules, and network
features) from `components/hardwareone/System_BuildConfig.h`. Checked-in
deployment builds apply their own feature overlay and OTA/partition contract,
so the same release can be reproduced without editing that shared header.

`components/arduino` is not in the repository: `tools/vendor_arduino.sh` clones arduino-esp32 3.3.5 and applies HardwareOne's local patches (BLE, ESP-Hosted, panic path, OTA removal); every build script verifies them first. Details: [docs/arduino-local-patches/README.md](docs/arduino-local-patches/README.md).

---

> ## Get up and running quickly: [Quick Start Guide](docs/QUICKSTART.md)

> ## Full reference, commands, and configuration: [User Guide](docs/USERGUIDE.md)

> ## Every command, generated from source: [Command Reference](docs/COMMAND_REFERENCE.md)

> ## What a version expects from its peers and what an upgrade keeps: [Compatibility](docs/COMPATIBILITY.md)

> ## Open problems and their workarounds: [Known Issues](KNOWN_ISSUES.md)

> ## Reporting a vulnerability privately, and the known security limits: [Security Policy](SECURITY.md)

## License

PolyForm Noncommercial 1.0.0: free for any noncommercial use. See [LICENSE.md](LICENSE.md); bundled third-party code is listed in [NOTICE.md](NOTICE.md).
