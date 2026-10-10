# Saved build configurations

Build different feature selections for the same board without editing shared headers:

```sh
python3 tools/build_config.py build xiao-default
python3 tools/build_config.py build xiao-mqtt
python3 tools/build_config.py build xiao-display
python3 tools/build_config.py build xiao-no-bluetooth
python3 tools/build_config.py build qtpy-sensors
python3 tools/build_config.py build qtpy-oled
python3 tools/build_config.py build qtpy-espnow
python3 tools/build_config.py build qtpy-logging-test
python3 tools/build_config.py build qtpy-maps-test
```

Source the ESP-IDF environment first, as for `tools/build_board.sh`. These
commands use the ordinary board build path. Outputs, SDK configuration, and
generated feature inputs live separately in `build-config-<name>/`.
Each successful build writes `BUILD_INFO.md` with
compiler-resolved feature flags, image size, and remaining app-partition space.
These commands build only; they do not flash a device.

`xiao-default` preserves the ordinary XIAO selection with MQTT off. `xiao-mqtt`
enables MQTT, Wi-Fi, and the MQTT settings page, and disables offline maps to
fit the existing app partition. Both use the existing Sense assembly selection
and board defaults. This trade-off is an explicit saved choice, not a dependency
rule or an automatic size adjustment.

`xiao-display` enables external I2C and an SSD1306 display. It explicitly disables
the camera because the current SDK camera bus and external I2C both use
controller 1, and disables maps and the onboard LLM to leave image space.
External I2C uses SDA GPIO5 and SCL GPIO6. No external display has been physically
qualified in this change. Supported external input selections can be saved
independently; their bus and driver dependencies are checked at compile time.

`xiao-no-bluetooth` disables application Bluetooth and its SDK stack. Bluetooth
SDK policy now lives in `presets/`, separate from board hardware geometry.

`qtpy-sensors` targets the **Adafruit QT Py ESP32 Pico** (classic ESP32). It
enables VL53L4CX ToF, the Seesaw gamepad, STHS34PF80 presence and PA1010D GPS,
with Wi-Fi and the sensor web page for testing. No local display is selected.
Bluetooth, MQTT, ESP-NOW, maps, LLM and host-link features are off in this saved
selection. The existing board definition supplies the STEMMA QT bus: bus 0
(`Wire1`), SDA GPIO22 and SCL GPIO19. Other boards' defaults are unchanged.

After flashing and completing any first-time setup, `i2cscan` should find the
attached modules at GPS `0x10`, ToF `0x29`, gamepad `0x50` (or its configured
alternate address) and presence `0x5A`. On fresh settings, start polling with
`opentof`, `openinput`, `openpresence` and `opengps`, or use the sensor web page.
These are queued starts; use `sensors json` to inspect subsequent state/readings.
An indoor GPS with no satellite fix is expected; check that it remains responsive
and the other sensors keep updating. A firmware build cannot qualify that live
behavior. Existing device settings, including bus assignments and autostart,
remain in effect until explicitly changed.

`qtpy-oled` uses the same QT Py bus and ToF/presence/GPS selection, with an
SSD1306 OLED in place of the gamepad and an APDS9960 at `0x39`. Both display
addresses (`0x3D`, `0x3C`) are supported. Physical input is disabled; serial
commands control the bench test. Use `oledstart`, `oledstop`, `oledbrightness <0..255>` and `oledstatus`.
For startup on reboot, enable `oledenabled 1` and `oledautostart 1`; these are
saved device settings, independent of compiling the display into the image.
Start APDS with `openapds` (color mode by default); `apdscolor` and
`apdsread json` show cached samples. `apdsmode proximity on` enables proximity
sampling, read with `apdsproximity`. These runtime mode commands do not change
the saved build selection.

`qtpy-espnow` keeps the `qtpy-oled` sensor/display selection and raises
`NETWORK_FEATURE_LEVEL` to 3 to include ESP-NOW. Use it for camera-image transfers
and remote filesystem tests with an XIAO peer. Pairing, channel and peer identity
remain runtime settings; the build profile does not contain credentials or
automatically pair devices.

`qtpy-logging-test` adds the Seesaw gamepad and OLED input to that selection.
`qtpy-maps-test` additionally enables Maps, including its web page. It exercises
the shared `gpstrack live start|stop|toggle` operation with local GPS and OLED
controls. Live recording leaves GPS and logger boot auto-start preferences
unchanged; a GPS fix is required to collect coordinates. The Maps image has
passed build and host checks; command/lifecycle hardware qualification is
recorded in [the Map test results](../docs/LOGGER_TEST.md#map-live-recording--2026-10-10).

## Guided setup

Start the terminal wizard from the repository root:

```sh
python3 tools/build_config.py configure
```

Choose a saved configuration to copy, board defaults, or an existing
configuration to edit. You can also choose that starting point directly:

```sh
python3 tools/build_config.py configure my-xiao --from xiao-mqtt
python3 tools/build_config.py configure my-board --board feathers3
python3 tools/build_config.py configure my-xiao --edit
```

The menu offers feature choices, SDK choices, and review. Select a choice by
number or name; `/mqtt` filters the list, and `/` shows all choices again.
Use `=name` for a literal name that could be a menu command, such as `=q` or `=2`.
Help comes from the existing source comments. Use `on`/`off` for switches, the
documented number for level/display/input choices, or `inherit` to remove an
override. Enter keeps the current value or returns from a choice list.

At review, `s` saves, `b` saves and builds, and `e` returns to editing.
`q`, Ctrl-C, or EOF while choosing/reviewing cancels the draft without changing
configuration or build files. New names cannot overwrite existing files.
Editing preserves the board and checks the original file again before saving;
if it detects an intervening edit, it stops and asks you to retry.

Saving alone needs Python. To choose **Save and build**, source the ESP-IDF
environment first. This runs the same board wrapper, compiler checks and image
size checks as `build`; it does not flash. If the build fails, the configuration
stays saved so you can revise it and retry. Dependencies and resource limits
are checked by the build, not inferred from the choice list.

## Discover and inspect

These commands need Python, but do not need an active ESP-IDF environment or
create build files:

```sh
python3 tools/build_config.py list boards
python3 tools/build_config.py list configurations
python3 tools/build_config.py list features
python3 tools/build_config.py list sdk
python3 tools/build_config.py show xiao-mqtt
```

Add `--json` to any `list` or `show` command for script-readable output.
`show` reports the saved choices. Unlisted values inherit defaults; compiler
checks during a build determine which features actually enter the firmware.
The choice lists and input validation use the same discovered names and ranges.
An accepted value can still require hardware, a backend or other features;
those requirements remain checked by the build.

## Create and edit

Create a configuration, or copy an existing one and change its choices:

```sh
python3 tools/build_config.py create my-xiao --from xiao-mqtt --set ENABLE_HTTPS=0
python3 tools/build_config.py create my-feather --board feathers3 --set ENABLE_MQTT=0
python3 tools/build_config.py create my-no-bt --from xiao-default \
  --set ENABLE_BLUETOOTH=0 --sdk CONFIG_BT_ENABLED=n
python3 tools/build_config.py build my-xiao
```

Preview an edit, apply it, or remove an override to restore inheritance:

```sh
python3 tools/build_config.py edit my-xiao --set ENABLE_HTTPS=1 --dry-run
python3 tools/build_config.py edit my-xiao --set ENABLE_HTTPS=1
python3 tools/build_config.py edit my-xiao --unset ENABLE_HTTPS
python3 tools/build_config.py edit my-no-bt --unset-sdk CONFIG_BT_ENABLED
```

`--dry-run` prints the candidate JSON and leaves configuration/build files
untouched. It validates names and value types/ranges; dependency and size
checks happen during the build. A normal edit replaces the file atomically,
preserves unmentioned choices and refuses conflicting set/unset operations.
An unchanged edit leaves the existing file and timestamp alone.

Explicit `0` (or SDK `false`) selects off. `--unset` / `--unset-sdk` removes the
choice and restores the inherited default, which may be on. Build again after
an edit to apply it. The named configuration keeps its board; choose a new name
when switching boards. You can also edit the JSON directly.

`features` accepts editable numeric
feature inputs. Optional `sdkconfig` accepts boolean choices listed in
`presets/sdkconfig.defaults` (currently `CONFIG_BT_ENABLED`). Unknown names,
derived flags, pin definitions, hardware geometry and security settings are
rejected. For example:

```json
{
  "board": "xiao_s3",
  "features": {"ENABLE_BLUETOOTH": 0},
  "sdkconfig": {"CONFIG_BT_ENABLED": false}
}
```

SDK defaults layer shared preset, board preset, then saved choices. Changing or
removing an SDK override reconciles the existing build-local sdkconfig before
Kconfig runs; removing it restores the inherited preset. The build checks the
actual Kconfig result and records explicit SDK choices in its manifest.

Unlisted values inherit the shared and board defaults **at the source
revision being built**. These are feature overlays, not complete frozen release
configurations. Keep the source revision and build manifest with an image.

The script generates literal definitions for the existing CMake source filters
and `System_BuildConfig.h`. It contains no alternate dependency resolver. After
configuration, the real compiler verifies the explicit input values. MQTT
without Wi-Fi is rejected with the dependency's explanation. Other input values
that existing rules change are rejected with the selected and resolved values.
`CUSTOM_ENABLE_*` values are input selectors; their corresponding effective
`ENABLE_*` flags in the manifest also depend on the group level and dependencies.

XIAO MQTT and peripheral selections are now defaults instead of board-wide
vetoes. Actual hardware and implemented-backend requirements remain enforced.
A successful compile does not establish hardware qualification or available
runtime memory.

Other ordinary IDF actions can be passed after the name, for example
`python3 tools/build_config.py build my-xiao size`. Use `clean` or `reconfigure`
if needed. `fullclean`, `set-target`, CMake `-D` overrides, and alternate project/
build paths are rejected because they would remove or bypass the saved inputs.
Choose a new name when changing boards. Deployment release contracts remain
separate; saved feature configurations cannot override `HW_DEPLOYMENT`.

## Verified build comparison

Measured 2026-10-07 on `78f4b73a` plus these changes, using ESP-IDF 5.5.5,
`esp-14.2.0_20260121`, and Arduino 3.3.5 with the documented local patches.
All four final profiles passed compilation, linking, image creation, compiler
choice validation and partition size checks. Their partition binaries are
identical; the app partition remains 5,984,256 bytes.

| Configuration | Image bytes | App headroom |
|---|---:|---:|
| `xiao-default` | 5,883,136 | 101,120 |
| `xiao-mqtt` | 5,784,272 | 199,984 |
| `xiao-display` | 5,730,816 | 253,440 |
| `xiao-no-bluetooth` | 4,350,144 | 1,634,112 |

An initial MQTT + settings page build retaining maps exceeded the partition
by 11,296 bytes. This explains the explicit maps trade-off in `xiao-mqtt`.
These are measured combinations, not additive estimates for other feature sets
or future revisions. MQTT and maps can be selected together, but need another
size trade-off to fit. Each build directory contains its input snapshot and
`BUILD_INFO.md` with the resolved flags and size report.

## Peripheral, SDK, and command-boundary migration

The command registry metadata now lives in `System_CommandRegistry.h`.
Command-only headers can use it without pulling in the broad utility header,
sensor declarations, transport context or SDK headers. `System_Utils.h`
re-exports it for existing callers; this is an incremental dependency cleanup.
Command metadata and authorization flags retain their existing layout and
behavior.

Host checks cover saved configuration validation (17 tests), XIAO peripherals
(9 tests / 40 compiler cases), optional peripherals (9 tests), MQTT selection
(5 tests), SDK policy layering (10 tests), HTTP gates (100 profiles), the
command-header dependency boundary, disabled G2 public headers (3 cases), and
display navigation/source state without a physical input controller.
Existing command lookup/limits/help/I2C tests also passed with sanitizers.

Real ESP-IDF 5.5.5 configuration checks switched XIAO Bluetooth off, on, off,
and back to the inherited on default in one build directory. QT Py switched
on and back to its inherited off default. Bluedroid followed each selection;
board variant, flash geometry, PSRAM, partition offset and encryption settings
remained unchanged.

The full builds also exercise previously uncommon feature combinations. Fixes
include the disabled G2 stub's type declarations, explicit sensor/input includes,
and bounded, terminated copies in Bluetooth and ESP-NOW display code. Display
state and input-independent setup helpers now remain available when no physical
input controller is selected; controller polling stays conditional.

MQTT broker testing is explicitly skipped at the owner's request. Compilation
and device boot do not establish broker connectivity or MQTT runtime behavior.


The connected XIAO was fully erased and flashed with `xiao-mqtt`; esptool verified
the hashes of the bootloader, app and partition table. First-time setup completed,
and the requested account authenticated both before and after a software reboot.
The device reported 8 MB flash, 8 MB PSRAM and zero crashes. No Wi-Fi or broker
was configured. The owner confirmed no SD card was attached; SD operation and
attached peripherals were not qualified. The console was closed after verification.

The discovery/editing and guided workflows have separate host tests for
read-only commands, inheritance, atomic-write failures, no-op timestamps, stale
build inputs, cancellation, conflicting edits, and save/build ordering.
These tooling checks do not require rebuilding or reflashing the firmware.
Additional setup interfaces should reuse these functions as described in the
[development guidelines](../CONTRIBUTING.md).
