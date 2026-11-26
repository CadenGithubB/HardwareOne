<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-black.svg">
  <img alt="Hardware One logo" src="assets/logo-black.svg" width="140">
</picture>

# Hardware One v0.99.99.1

**Hardware One is a modular ESP32 firmware that works like a distributed operating system for cheap microcontrollers.**

</div>

You build and flash each board for one job: a smart-home sensor, a pocket companion for smart glasses, a headless mesh node, a camera node. Every device speaks the same custom ESP-NOW v1 protocol, so they form a private, router-free mesh, pool their data, and can be monitored from a single dashboard.

Every device is driven by one command system, the CLI, over USB serial, a browser, the on-device screen, Bluetooth, voice, or another node on the mesh. Same commands, same permission checks, every way in.

Built on **ESP-IDF v5.5.5** (not the Arduino IDE).

## Boards

| Board | Name to type |
| ----- | ------------ |
| Seeed XIAO ESP32-S3 (Sense) | `xiao_s3` |
| Unexpected Maker FeatherS3 | `feathers3` |
| Adafruit Feather ESP32 V2 | `feather_esp32_v2` |
| Adafruit QT Py ESP32 | `qtpy_esp32` |
| Espressif ESP32-P4X-EYE | `p4x_eye` |
| Waveshare ESP32-P4 Touch LCD 5 (experimental) | `waveshare_p4_touch_lcd_5` |
| FeatherS3 with flash encryption (experimental; first boot burns eFuses) | `feathers3_fe` |

`python3 tools/build_config.py list boards` prints the same list with what is on each board. Details and the support matrix: [Supported Hardware](docs/README_DETAILS.md#supported-hardware).

## Getting started

1. **Install ESP-IDF v5.5.5.** One version for every board; see [Software Setup](docs/QUICKSTART.md#software-setup).

2. **Clone, and fetch the patched Arduino component** (once per clone):

   ```sh
   git clone https://github.com/CadenGithubB/HardwareOne.git
   cd HardwareOne
   tools/vendor_arduino.sh
   ```

3. **Load the ESP-IDF environment and run the guided setup.** It asks for your board, then what the device should do (a saved configuration of that board, or the board's defaults), lets you switch features on and off by plain name, and saves your choices as `configurations/<name>.json` ([how it works](configurations/README.md#guided-setup)):

   ```sh
   . "$IDF_PATH/export.sh"
   python3 tools/build_config.py new
   ```

   If `IDF_PATH` is not set in this shell, give the path of your ESP-IDF folder instead, for example `. ~/esp/esp-idf/export.sh`.

4. **Build and flash.** Replace `<name>` with the name you saved, `<board>` with the name from the table above and `PORT` with the board's serial port.

   Build only:

   ```sh
   python3 tools/build_config.py build <name>
   ```

   Flash, then open the serial monitor:

   ```sh
   tools/build_board.sh <board> --config <name> -p PORT flash monitor
   ```

5. **First boot.** The setup wizard starts on the serial monitor (115200 baud) and on the local display, if the board has one: [First-Time Use](docs/QUICKSTART.md#first-time-use).

To skip the questions, build a saved configuration by name: `python3 tools/build_config.py list configurations` shows them, and `python3 tools/build_config.py build xiao-default` builds the standard XIAO ESP32-S3 selection (Bluetooth and MQTT off).

- **ESP32-P4 boards** need extra one-time steps (`tools/p4/prepare_sdk.sh`, the firmware for the onboard ESP32-C6 radio and, for speech builds, one ESP-IDF patch from `tools/p4/patches/`): [P4X-EYE building](docs/P4X_EYE_PERIPHERALS.md#building), [radio companion](docs/P4X_EYE_PERIPHERALS.md#radio-companion-esp32-c6), [Waveshare notes](docs/WAVESHARE_P4_SUPPORT.md).
- **A board that ran other firmware** should be erased before the flash in step 4: `tools/build_board.sh <board> --config <name> -p PORT erase-flash`.
- **Logs on the SD card are sealed.** Logs on the card can be read only by the device that wrote them; to read a pulled card, put it back in that device ([Logs on the SD card](docs/USERGUIDE.md#logs-on-the-sd-card)).

## More

- [Device roles, feature table, supported hardware, support matrix and build system](docs/README_DETAILS.md)
- <a name="local-speech-to-text-model-esp32-p4"></a>[Local speech-to-text model (ESP32-P4)](docs/README_DETAILS.md#local-speech-to-text-model-esp32-p4)
- [Saved build configurations and the guided setup](configurations/README.md)
- [Quick Start Guide](docs/QUICKSTART.md)
- [User Guide](docs/USERGUIDE.md): full reference, commands and configuration
- [Command Reference](docs/COMMAND_REFERENCE.md): every command, generated from source
- [Compatibility](docs/COMPATIBILITY.md): what a version expects from its peers and what an upgrade keeps
- [Known Issues](docs/KNOWN_ISSUES.md): open problems and their workarounds
- [Troubleshooting](docs/TROUBLESHOOTING.md): what to check after a crash, a restart or a hang, and what to put in a report
- [Security Policy](SECURITY.md): reporting a vulnerability privately, and the known security limits
- [Contributing](CONTRIBUTING.md): development guidelines

## License

PolyForm Noncommercial 1.0.0: free for any noncommercial use. See [LICENSE.md](LICENSE.md); bundled third-party code is listed in [NOTICE.md](NOTICE.md).
