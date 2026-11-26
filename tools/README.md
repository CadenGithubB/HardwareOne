# Host tools

Everything here runs on the development machine, not on a device. Run the
tools from the repository root; most need only Python 3 and its standard
library, and the ones that need more say so in their own README.

## Building

| Tool | What it does |
|---|---|
| `build_board.sh <board>` | Builds one board in its own `build-<board>/` directory; `--config <name>` builds a saved configuration. Actions after the board go straight to `idf.py` (`flash`, `monitor`, ...). See [board switching](../docs/BOARD_SWITCHING.md). |
| `build_config.py` | Saved build configurations: `new` runs the guided setup, `prepare`/`check` are used by `build_board.sh`. See [configurations](../configurations/README.md). |
| `build_config_catalog.json` | Plain names, groups and descriptions the guided setup shows for each board and switch. |
| `build_config_catalog.py`, `build_config_resolve.py`, `build_config_wizard.py` | The guided setup's parts: names, the dependency answers read from `System_BuildConfig.h`, and the menus. Used by `build_config.py`, not run directly. |
| `build_deployment.sh <deployment> <board>` | Builds a checked-in deployment as a paired factory-updater and main image. See [releasing](../RELEASING.md). |
| `checkout_build_lock.sh` | Sourced by the two build scripts: one build target per checkout at a time. |
| `gen_build_info.py` | Writes `BUILD_INFO.md` next to an image: features, board, sizes and source tree. Run by the build scripts. |
| `vendor_arduino.sh` | Vendors arduino-esp32 into `components/arduino` and applies the local patches; `--check` verifies an existing copy. |

## Registries and generated references

| Tool | What it does |
|---|---|
| `command_registry.py` | Reads the command tables from the C++ source. `reference` writes [the command reference](../docs/COMMAND_REFERENCE.md); `audit` checks that every command the firmware runs is registered. Both are release gates. |
| `settings_registry.py` | The same for settings: `matrix` writes a settings matrix to `docs/SETTINGS_MATRIX.md` (not checked in). |

## Folders

| Folder | What it holds |
|---|---|
| [`checks/`](#checks) | Build-coverage, flash-size and source checks. |
| [`p4/`](#p4) | ESP32-P4 setup: patched SDK components, the C6 radio companion, serial and RAM tools. |
| [`ota/`](ota/README.md) | Build, sign, bundle, back up and qualify recovery OTA images. |
| [`webui/`](webui/README.md) | Host tests for the web UI's embedded JavaScript, and the Dark Room page generator. |
| [`model/`](model/caps.py) | Reads and stamps the capability section of an on-device LLM model file. |
| [`ble_secure/`](#ble_secure) | Reference implementation of the BLE Secure Channel v1, its test vectors, and an event-catalog client. |
| [`ble_phone_pipe/`](ble_phone_pipe/README.md) | The Arduino BLE patch the phone pipe needs, and its check. |
| [`phone_link_peer/`](phone_link_peer/README.md) | A Mac that stands in for the Android phone on the phone link, for bench tests. |
| [`g2/`](#g2) | Even Realities G2 helpers: mic capture decoding, case frames, image preparation. |
| [`btsnoop/`](btsnoop/README.md) | Offline tools for Android Bluetooth HCI captures. |
| [`stt_raw/`](stt_raw/README.md) | Replays `sttraw` recordings through the speech segmenter on the host. |
| [`showcase/`](showcase/README.md) | A narrated live view of a running device, for demos. Reads the device; changes nothing. |

### checks

| Tool | What it does |
|---|---|
| `build_coverage.sh [board]` | Builds with features the shipping profile leaves off, so hidden code still compiles. |
| `build_memory_coverage.sh [board]` | The same for allocator-sensitive code. |
| `build_event_catalog_coverage.sh` | Builds the event catalog provider with its consumers on and off. |
| `feature_size_sweep.sh [board]` | Turns each feature family off in turn and records the image size. Run it from a separate clone: it edits the configuration while it runs. |
| `map_feature_cost.py` | Groups a linker map by feature to show what each one costs in flash. |
| `check_raw_allocations.py` | Lists direct `malloc`/`calloc`/`realloc` calls outside `System_MemUtil`; with `--expect` it fails when the count changes (a host test pins it). |

The build scripts restore every file they edit when they exit.

### p4

| Path | What it holds |
|---|---|
| `prepare_sdk.sh` | One-time setup for P4 builds: stages the patched ESP-IDF Bluetooth, JPEG, ESP-DL and ESP-Hosted components in `.sdk-overrides/` (ignored by git); `--check` verifies them. |
| `prepare_*.py`, `fetch_p4_packages.py`, `check_idf_pie.py` | The steps `prepare_sdk.sh` runs. |
| [`patches/`](p4/patches/README.md) | The pinned patches those steps apply, and the one ESP-IDF patch applied by hand. |
| [`companion/`](p4/companion/README.md) | Firmware and programmer for the board's ESP32-C6 radio. |
| `serial_capture.py`, `serial_first_time_setup.py` | USB console capture and first-time setup without resetting the board. |
| `ram_monitor.py`, `ram_monitor.html` | A live memory graph from `memreport json`. |
| `tests/` | Host tests for the patches and setup scripts (run by the host suite). |

### ble_secure

`secure_channel_v1.py` is the host side of the BLE Secure Channel v1, checked
against the vectors in `fixtures/`. `event_catalog_client.py` uses it to fetch
and validate the device's event catalog: offline by default, over BLE with
`--physical`. `test_event_catalog_client.sh` runs both in a pinned virtual
environment (`requirements.txt`, needs PyNaCl).

### g2

| Tool | What it does |
|---|---|
| `decode_g2_mic.py` | Turns a `g2micrec` capture into a WAV file. |
| `g2_case_decoder.py` | Decodes glasses-case and settings frames. |
| `g2_presence_policy_sim.py` | Simulates the accessory-presence policy. |
| `gif_to_g2_bmps.py`, `g2_image_prepare.sh` | Convert images and GIF frames to the glasses' 4-bit BMP format (`requirements-gif.txt`). |

## Tests

The host suite in [`components/hardwareone/test/host`](../components/hardwareone/test/host/README.md)
runs the tests of `ota/`, `webui/`, `model/`, `p4/tests/`, `ble_phone_pipe/`
and `checks/check_raw_allocations.py`. `phone_link_peer/` and `ble_secure/`
are bench tools with their own test commands.
