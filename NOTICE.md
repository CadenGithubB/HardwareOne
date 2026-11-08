# Notices

HardwareOne is licensed under the PolyForm Noncommercial License 1.0.0
([LICENSE.md](LICENSE.md)). The firmware also contains code written by others.
That code keeps its own licence, listed below, and the licence text sits in each
component's folder unless a link is given. Binary release downloads ship this
file together with those licence texts.

## Built into the firmware

### Board support and SDK

| Component | Licence | Copyright / source |
|---|---|---|
| ESP-IDF 5.5.5 | Apache-2.0 | Espressif Systems. ESP-IDF bundles further components (FreeRTOS, lwIP, mbedTLS, NimBLE and others) under their own licences, listed in ESP-IDF's `COPYRIGHT.rst`. |
| arduino-esp32 3.3.5 (`11bc7ac1a`, with the patches in `docs/arduino-local-patches/`) | LGPL-2.1 | Espressif Systems and contributors, <https://github.com/espressif/arduino-esp32> |

### Espressif managed components

These come from the ESP Component Registry at the versions pinned in `dependencies.lock*`.

| Component | Licence |
|---|---|
| esp-dsp, esp-nn, esp-tflite-micro, esp_jpeg, mdns, esp_hosted, esp_cam_sensor, esp_sccb_intf, esp_serial_slave_link, esp_wifi_remote, eppp_link, wifi_remote_over_eppp, usb_host_uvc, cmake_utilities | Apache-2.0 |
| esp-dl, dl_fft | MIT |
| esp-sr, esp_new_jpeg, esp_video, esp_ipa | Espressif MIT (use limited to Espressif chips) |
| cJSON | MIT (Dave Gamble and contributors) |
| libsodium | ISC (Frank Denis) |
| esp_littlefs (joltwallet) with littlefs | MIT / BSD-3-Clause |

### Libraries compiled from `components/hardwareone_libs/`

| Library | Version | Licence | Copyright |
|---|---|---|---|
| Adafruit BNO055 | 1.6.4 | MIT | Adafruit Industries |
| Adafruit Unified Sensor | 1.1.15 | Apache-2.0 | Adafruit Industries |
| Adafruit BusIO | 1.17.4 | MIT | Adafruit Industries |
| Adafruit GFX | 1.12.3 | BSD | Adafruit Industries |
| Adafruit LED Backpack | 1.5.1 | MIT | Adafruit Industries |
| Adafruit SSD1306 | 2.5.15 | BSD | Adafruit Industries |
| Adafruit NeoPixel | 1.15.1 | LGPL-3.0 | Adafruit Industries and contributors |
| Adafruit APDS9960 | 1.3.0 | BSD | Adafruit Industries |
| STM32duino VL53L4CX | 1.1.0 | BSD-3-Clause | STMicroelectronics |
| Adafruit MLX90640 | 1.1.1 | Apache-2.0 | Adafruit Industries |
| Adafruit PWM Servo Driver | 3.0.2 | BSD | Adafruit Industries |
| Adafruit GPS | 1.7.5 | BSD | Adafruit Industries |
| Adafruit seesaw | 1.7.9 | BSD (licence text in the source headers) | Adafruit Industries |
| Radio | 3.0.1 | BSD | Matthias Hertel |
| liblc3 | upstream snapshot | Apache-2.0 | Google LLC |
| ArduinoJson (headers) | 7.4.2 | MIT | Benoit Blanchon |

### Other bundled code

| Component | Licence | Notes |
|---|---|---|
| es8311 codec driver (`components/hardwareone/third_party/es8311/`) | Apache-2.0 | Espressif Systems |
| ESP-NOW hosted component (`components/esp_now_hosted/`) | Apache-2.0 | See its LICENSE |
| P4 companion bridge (`tools/p4/companion/bridge/`) | Apache-2.0 | See its LICENSE |
| A Dark Room (`components/hardwareone/WebPage_DarkRoom.h`) | MPL-2.0 | Doublespeak Games, <https://github.com/doublespeakgames/adarkroom>. The header contains the game's source, flattened by `tools/build_darkroom_header.py`, and is covered by the Mozilla Public License 2.0: <https://mozilla.org/MPL/2.0/>. |

## Release downloads that are not in the firmware image

| Item | Licence | Source |
|---|---|---|
| Speech-recognition model files | CC-BY-4.0 | Derived from NVIDIA QuartzNet (NeMo) checkpoints; fine-tuned and quantized for HardwareOne. See `experiments/stt_p4/frontend/README.md`. |
| Speaker-label model (`speaker.spk`) | MIT | clova Fast ResNet-34, NAVER Corp., <https://github.com/clovaai/voxceleb_trainer> |

## LGPL libraries

arduino-esp32 (LGPL-2.1) and Adafruit NeoPixel (LGPL-3.0) are linked into the
firmware image. HardwareOne's complete source and build instructions are public,
so you can rebuild the firmware with your own modified copy of either library
(see the build steps in [README.md](README.md)). Nothing in HardwareOne's licence
restricts you from modifying these libraries, or from reverse-engineering the
firmware to debug such modifications.

## Source tree only

`components/hardwareone_libs/` also holds libraries that are not compiled into
any firmware image, some of them under the GPL (for example `SD` and the
`WebServer_ESP32_*` Ethernet libraries). They are not part of the shipped
binaries; each keeps the licence in its own folder.
