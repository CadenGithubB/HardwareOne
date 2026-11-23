# Saved build configurations

Build different feature selections for the same board without editing shared headers:

```sh
# name, board, and what each is for
python3 tools/build_config.py list configurations
python3 tools/build_config.py build xiao-default
# make your own: board first, then what it does
python3 tools/build_config.py new
```

Fifteen configurations are kept, each a board and a job:

| Board | Configuration | What it is for |
|---|---|---|
| ESP32-P4X-EYE | `p4-glasses-test` | Glasses captions: speech-to-text on the board with the G2 glasses and R1 ring |
| | `p4-openclaw` | Captions plus an assistant that answers over HTTP (OpenClaw) |
| | `p4-phone-pipe` | Captions and the HTTP assistant plus the link to the Android app |
| | `p4-portable` | The carry-around device: speech-to-text, glasses, ring, phone link, the HTTP assistant and microSD; ESP-NOW worker in the home mesh. No camera |
| | `p4-home-node` | Rack-mounted mesh master: ESP-NOW, MQTT, Wi-Fi, the web page and microSD. No speech, glasses, ring, camera or microphone |
| XIAO ESP32-S3 Sense | `xiao-default` | The standard selection; Bluetooth and MQTT off to fit the app partition |
| | `xiao-base` | The standard selection for a XIAO without the Sense board |
| | `xiao-mqtt` | Home Assistant node: MQTT and its settings page; offline maps off to fit |
| | `xiao-openclaw` | Assistant over HTTP or from a Raspberry Pi; Bluetooth and the on-board model off |
| | `xiao-display` | An SSD1306 display on external I2C; camera, maps and the on-board model off |
| | `xiao-status-sign` | Meeting-room status sign: two Adafruit 4991 encoder boards lit by the LED commands; Wi-Fi, web interface and automations only |
| FeatherS3[D] | `feathers3-status-sign` | Meeting-room status sign: two Adafruit 4991 encoder boards, an Adafruit 4517 motion sensor that is also the OLED's input (tap for buttons, tilt to move), the OLED, a DS3231 clock, battery gauge and u.FL antenna; Wi-Fi, web interface with the Sensors page, and automations |
| QT Py ESP32 | `qtpy-sensors` | Sensor node: GPS, distance and presence sensors with a gamepad |
| | `qtpy-oled` | Sensor node with an SSD1306 display and a gesture sensor |
| | `qtpy-espnow` | The OLED sensor node plus the ESP-NOW mesh |

Each file carries that line as its `title` and `summary`; the lists and the guided setup show them.
The other boards (Feather ESP32 V2, Waveshare ESP32-P4 Touch LCD 5) have no saved
configuration: the guided setup starts them from the board's defaults.

**Off unless chosen (since 2026-11-20).** HTTPS for the web interface, the bonded pair and the
assistant (with its three answer sources) are off in the shared defaults of
`System_BuildConfig.h`. A build from a board's defaults alone has none of them; the guided setup
offers each under its plain name (groups Network and Assistant), starting at off. The saved
configurations above were not changed by this: each one that had HTTPS, the bonded pair or the
assistant through the old defaults now writes the switch itself (`ENABLE_HTTPS`,
`ENABLE_BONDED_MODE`, `ENABLE_LLM_BACKEND`, `ENABLE_LLM_SOURCE_ONBOARD`, `ENABLE_LLM_SOURCE_CM5`
in the three QT Py and five XIAO files), and all thirteen resolve to the same features as before
(`xiao-status-sign`, added on 2026-11-22, writes HTTPS on and the others off).
The five P4X-EYE configurations already had them off or written, in the file or in
`boards/p4x_eye.features.h`.

A configuration of your own that was saved before 2026-11-20 is not covered: a saved file holds
only what differs from the defaults, so one that had these features through the old defaults
builds without them now, and nothing warns. To keep them, add the switches to its `features`
(`"ENABLE_HTTPS": 1`, `"ENABLE_BONDED_MODE": 1`, `"ENABLE_LLM_BACKEND": 1` with at least one
`ENABLE_LLM_SOURCE_*`), for example with
`python3 tools/build_config.py edit <name> --set ENABLE_HTTPS=1`. A device rebuilt without HTTPS
serves plain HTTP even with `httpsenabled` saved, so an `https://` bookmark stops working.

Add `--wdt-soak` to `tools/build_board.sh` (for example
`tools/build_board.sh xiao_s3 --config xiao-default --wdt-soak`, or
`python3 tools/build_config.py build p4-openclaw --wdt-soak`) for the task-watchdog
soak image: the watchdog panics after 20 s and the main loop is subscribed
(`config/sdkconfig.wdt_soak.defaults`). It builds in `build-config-<name>-wdt-soak/`
(or `build-<board>-wdt-soak/`), so ordinary builds never pick it up. Bench use only.
For the same reason, configuration names may not end in `-wdt-soak`.

Source the ESP-IDF environment first, as for `tools/build_board.sh`. These
commands use the ordinary board build path. Outputs, SDK configuration, and
generated feature inputs live separately in `build-config-<name>/`.
Each successful build writes `BUILD_INFO.md` with
compiler-resolved feature flags, image size, and remaining app-partition space.
These commands build only; they do not flash a device.

`xiao-default` uses the ordinary XIAO selection with MQTT and Bluetooth off.
Bluetooth is disabled in both the application and SDK to leave app-partition
space. This is a saved-profile choice; board and shared
defaults remain available to other profiles. `xiao-mqtt`
enables MQTT, Wi-Fi, and the MQTT settings page, and disables offline maps to
fit the existing app partition. Both use the existing Sense assembly selection
and board defaults. This trade-off is an explicit saved choice, not a dependency
rule or an automatic size adjustment.

`p4-openclaw` starts from the qualified `p4-glasses-test` speech profile and
adds the LLM feature and HTTP source. It keeps local STT (the default Citrinet
model), speaker labels and SDMMC enabled, with ESP-SR and onboard/CM5 LLM sources
off. Like every P4 speech profile, both opt into `CONFIG_HEAP_USE_HOOKS` for the
speech pipeline's internal-memory accounting (added to `p4-glasses-test` on
2026-10-18; 0.99.98.5 built that profile and flashed it to the bench P4X-EYE,
which booted to a complete setup, but its glasses qualification is still
pending; see CHANGELOG.md). Runtime relay credentials and the public CA are configured on the device.
Use the existing Stop → wait for transcription → review → Send interaction.
The combined voice-to-agent path still needs hardware qualification.

`xiao-openclaw` enables the HTTP agent source and keeps CM5 available. This test
profile explicitly disables Bluetooth (application and SDK) and the onboard LLM
to fit the existing app partition;
the user selected that trade-off for remote-agent testing. It also turns off
the BLE coexistence records (`ENABLE_BLE_COEX_RECORDS`): with them on, the image
overflowed the partition by 5,696 bytes (2026-10-17; 2,480 bytes remain without
them). The assistant and its Raspberry Pi source are written in the file (the
shared defaults have the assistant off); the BLE records stay a shared default and
are derived off in profiles that disable Bluetooth. TLS
trust/verification and all other existing choices are unchanged. Endpoint credentials remain runtime device
settings; the saved configuration contains no secrets.

`xiao-display` enables external I2C and an SSD1306 display. It explicitly disables
the camera because the current SDK camera bus and external I2C both use
controller 1, and disables maps and the onboard LLM to leave image space.
External I2C uses SDA GPIO5 and SCL GPIO6. No external display has been physically
qualified in this change. Supported external input selections can be saved
independently; their bus and driver dependencies are checked at compile time.

Bluetooth SDK policy lives in `presets/`, separate from board hardware geometry.

`xiao-base` is `xiao-default` for a XIAO ESP32S3 without the Sense expansion
board: `XIAO_ESP32S3_SENSE_ENABLED`, `ENABLE_CAMERA_SENSOR` and
`ENABLE_MICROPHONE_SENSOR` are 0, so the image has no camera, microphone or SD
card and the user LED on GPIO21 is available. Every other XIAO configuration is
for the Sense assembly. The image builds; it has not been run on a base board.

`xiao-status-sign` is the 3D-printed meeting-room status sign: a XIAO ESP32S3 with two
Adafruit 4991 Rotary Encoder with NeoPixel boards on the STEMMA QT bus (I2C1, SDA GPIO5,
SCL GPIO6), used only for their pixels (green cell and red cell). It starts from `xiao-default`
and keeps Wi-Fi, the web interface with HTTPS and automations; it adds the encoder board device
(`CUSTOM_ENABLE_ENCODER`) and turns off the camera (it would hold I2C controller 1), the
microphone, the assistant, ESP-NOW and the bonded pair, the Raspberry Pi link, offline maps and
the Sensors, Maps, ESP-NOW and Power pages. Bluetooth and MQTT stay off as in `xiao-default`. No
OLED and no fuel gauge are compiled in, so the address-collision check leaves 0x36..0x3D to the
encoder boards. The Sense selection is kept (microSD stays available).

A saved configuration holds build switches only, not settings, so the boards' addresses are set
once on the device after the first setup (Settings page, or the console):

```text
encoderaddress 0x36
encoderaddress 2 0x37
encoderautostart on
encoderautostart 2 on
```

Then `ledcolor encoder green` and `ledcolor encoder2 red` (or `ledeffect`, `ledbrightness`,
`ledclear` with the same targets) light the cells, by hand or from an automation. The image
builds; it has not been run on the sign.

`feathers3-status-sign` is the same sign on an Unexpected Maker FeatherS3[D] (16 MB, no flash
encryption: board `feathers3`, not `feathers3_fe`), with the handheld's SSD1306 OLED, a LiPo and
an external antenna on the u.FL. It starts from `xiao-status-sign` with the board changed and the
OLED and the Power page left on, and adds:

- a DS3231 clock (`CUSTOM_ENABLE_RTC`), which keeps the time through power loss and before
  Wi-Fi/NTP is up;
- an Adafruit 4517 motion sensor (`CUSTOM_ENABLE_IMU`, `IMU_DEVICE_TYPE 2`: LSM6DSOX + LIS3MDL)
  for tilt, flip and tap. Its yaw counts from the IMU's start: there is no compass heading until
  the magnetometer calibration lands;
- that motion sensor as the OLED's input device (`INPUT_DEVICE_TYPE 5`), the sign's only one:
  one tap on the sign is A, two are B, three are Y, four are X, a shake is START, and tilting
  the sign left, right, up or down moves through the menus and the on-screen keyboard like a
  joystick. Five taps (or `imuinputzero`) take the sign's present pose as the centre, for when
  it is stood or hung another way. `imuinputread` shows what it makes of the motion; the
  directions are fitted to how the sensor sits in the enclosure with `imuinputswapxy`,
  `imuinputinvertx` and `imuinputinverty`. (Until 2026-11-23 an Adafruit 5740 ANO wheel was the
  input device, `INPUT_DEVICE_TYPE 2`; this configuration no longer builds its driver);
- the web Sensors page (`CUSTOM_ENABLE_WEB_SENSORS`), which shows the input and the motion sensor.

The wiring, as found on the board:

| Bus | Port | Devices |
|---|---|---|
| 0 (I2C1) | horizontal STEMMA QT | MAX17048 fuel gauge `0x36` (on the board), DS3231 clock `0x68`, the second 4991 at `0x37` once its A0 pad is bridged |
| 1 (I2C2) | vertical STEMMA QT | OLED `0x3D`, the first 4991 `0x36`, the 4517: LSM6DSOX `0x6A` and LIS3MDL `0x1C` |

The [D]'s fuel gauge has the first 4991's address (`0x36`), so the two cannot share bus 0; the
second 4991 goes there at `0x37`. The clock starts at boot with its defaults (`rtcbus 0`,
`rtcautostart on`). Once after the first setup, in this order:

```text
encoderbus 1
encoderaddress 0x36
encoderautostart on
encoderbus 2 0
encoderaddress 2 0x37
encoderautostart 2 on
imubus 1
imuautostart on
inputautostart on
oledautostart on
wifiexternalantenna on
powersave 0
reboot
```

The order matters, because every bus and address command refuses an address that another device
of the build already has on that bus. On fresh settings `0x36` on bus 0 is the fuel gauge's: so
the first 4991 moves to bus 1 before it gets `0x36`. `encoderbus 2 0` is the default and is listed
so the block says where each board is. A host test replays the block against the collision check
(`test_i2c_instance_core`, `testStatusSignSetup`).

`inputautostart on` starts the taps-and-tilt input at boot; it opens the motion sensor itself and
keeps it open (`closeimu` refuses while the input runs, `closeinput` first). The pose the sign is
in when the input starts is the joystick's centre.

After the reboot, `i2cscan` should list `0x36`, `0x37` and `0x68` on bus 0 and `0x1C`, `0x36`,
`0x3D` and `0x6A` on bus 1 (an ANO wheel still plugged in shows at `0x49` as a device this build
does not use); `imustatus`, `imuinputread`, `encoderstatus 1` and `encoderstatus 2` show each
device.

`powersave 0` matters on this board: the OLED is on bus 1 (the vertical STEMMA QT, the [D]'s
default OLED bus), and a power-save blank cuts LDO2 (GPIO39), which also powers off everything else on
bus 1 (the first 4991's pixel and the motion sensor, which is not started again when the rail
returns: with it the sign's input stops until `openinput` is run again). With power saving off the panel never blanks on its own; a deliberate sleep
(light/deep sleep) still cuts the rail.

`wifiexternalantenna` (FeatherS3[D] only, reboot required) drives GPIO41 high at boot, which turns
the board's RF switch to the u.FL connector; off, the pin is left alone and the switch's pull-down
keeps the on-board antenna.

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
commands control the bench test. Use `openoled`, `closeoled`, `oledbrightness <0..255>` and `oledstatus`.
For startup on reboot, run `oledautostart 1`; it is a
saved device setting, independent of compiling the display into the image.
Start APDS with `openapds` (color mode by default); `apdscolor` and
`apdsread json` show cached samples. `apdsmode proximity on` enables proximity
sampling, read with `apdsproximity`. These runtime mode commands do not change
the saved build selection.

`qtpy-espnow` keeps the `qtpy-oled` sensor/display selection and raises
`NETWORK_FEATURE_LEVEL` to 3 to include ESP-NOW. Use it for camera-image transfers
and remote filesystem tests with an XIAO peer. Pairing, channel and peer identity
remain runtime settings; the build profile does not contain credentials or
automatically pair devices.

## Guided setup

Start it from the repository root:

```sh
python3 tools/build_config.py new
```

1. **Board.** The seven boards, each with its name and what is on it.
2. **What should it do?** The saved configurations of that board, by title and summary, and "Board
   defaults only". The one you pick is copied as the starting point; the saved file is not changed.
3. **Add or remove.** Everything the board can have, grouped (Voice, Assistant, Network, Web pages,
   Bluetooth and wearables, Sensors and add-ons, Display and input, Board hardware, Raspberry Pi
   link, Apps and automation, Advanced). Open a group by number or name and switch an item by
   number or name: `x` is on, blank is off, `-` cannot be turned on as things stand (choose it to
   read why). Display, input control and speech model are lists of values instead of switches.
4. **Review and save.** `review` asks for a name (Enter takes the suggestion, such as `xiao-custom`)
   and one optional line on what it is for, shows what is on by group and the file to be written,
   then `s` saves, `b` saves and builds, `e` returns to editing.

What the setup does for you while you switch things:

- **True states.** On and off are what the firmware will really have for this board and selection.
  The setup runs `System_BuildConfig.h` through this computer's C++ preprocessor with the board's
  feature header, the layered SDK defaults and your choices (`tools/build_config_resolve.py`), so
  board defaults, feature levels and derived rules are all applied. It keeps no second copy of the
  rules.
- **Prerequisites.** Turning on an item also turns on what it needs and says so (the MQTT settings
  page brings MQTT; speech-to-text on the board brings the microSD card). Turning Bluetooth on or
  off moves the SDK's Bluetooth stack with it.
- **Refusals with the reason.** A change the header rejects is not applied, and the header's own
  message is shown with the setup's names added, for example
  `External I2C uses controller 1, reserved by camera SCCB; ... disable ENABLE_CAMERA_SENSOR ("Camera")`.
- **Side effects.** Items that go on or off as a consequence are listed ("now off as well: ...").
- **Feature levels.** Network, web pages and I2C devices are governed by a level on some boards
  (the P4X-EYE states network level 3). Switching one item sets that level to custom and writes the
  other items of the group as they were, so nothing else changes.
- **A starting point the rules reject.** The bare Waveshare selection is one: the shared defaults
  leave the Raspberry Pi features on and the board has no UART link. The overview says what is
  wrong ("Not buildable yet: ..."), and the selection can be repaired item by item.

The other words on the overview: `names` lists every switch by its own name (`/mqtt` filters,
`inherit` removes an override, help comes from the source comments), `sdk` lists the SDK choices,
and `q`, Ctrl-C or EOF cancels without changing any file. Use `=name` for a literal name that
could be read as a command, such as `=q` or `=2`.

You can also start further in:

```sh
python3 tools/build_config.py new my-xiao --from xiao-mqtt     # copy a saved configuration
python3 tools/build_config.py new my-board --board feathers3   # the board's defaults
python3 tools/build_config.py new my-xiao --edit               # change a saved configuration
```

`configure` is the same command under its earlier name. New names cannot overwrite existing
files. Editing preserves the board and checks the original file again before saving; if it
detects an intervening edit, it stops and asks you to retry.

Saving alone needs Python and, for the checklist, a host C++ compiler (`c++`); without one the
groups show `?` and `names` still works. To choose **Save and build**, source the ESP-IDF
environment first. This runs the same board wrapper, compiler checks and image size checks as
`build`; it does not flash. If the build fails, the configuration stays saved so you can revise
it and retry. The setup's evaluation uses the SDK defaults files, not a resolved sdkconfig, and
does not know image size: the build remains the final check.

The plain names live in `tools/build_config_catalog.json`: a title, a group and one line for each
switch, a name and hardware line for each board, and the prerequisite shortcuts. A switch added to
`System_BuildConfig.h` is offered by `names` at once, and the host test `build_config_catalog`
fails until it has a catalogue entry.

## Discover and inspect

These commands need Python, but do not need an active ESP-IDF environment or
create build files:

```sh
python3 tools/build_config.py list boards
python3 tools/build_config.py list configurations
python3 tools/build_config.py list configurations --board xiao_s3
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
choice and restores the inherited default: off for HTTPS, the bonded pair and the
assistant, and for other switches whatever the board and the shared defaults say,
which may be on. Build again after
an edit to apply it. The named configuration keeps its board; choose a new name
when switching boards. You can also edit the JSON directly.

`features` accepts editable numeric
feature inputs. Optional `sdkconfig` accepts the boolean choices listed in
`presets/sdkconfig.defaults`; every `CONFIG_*=y|n` line in that file is one
(`tools/build_config.py` and `cmake/HW1Sdkconfig.cmake` read the list from it). Unknown names,
derived flags, pin definitions, hardware geometry and security settings are
rejected. For example:

```json
{
  "board": "xiao_s3",
  "features": {"ENABLE_BLUETOOTH": 0},
  "sdkconfig": {"CONFIG_BT_ENABLED": false},
  "title": "No Bluetooth",
  "summary": "The standard selection without Bluetooth."
}
```

`title` (at most 60 characters) and `summary` (at most 240) are optional single lines. They are
not build inputs: changing them does not touch a build directory. A copy made with `--from` does
not take them.

SDK defaults layer shared preset, board preset, then saved choices. Changing or
removing an SDK override reconciles the existing build-local sdkconfig before
Kconfig runs; removing it restores the inherited preset. The build checks the
actual Kconfig result and records explicit SDK choices in its manifest.

Every key listed in `presets/sdkconfig.defaults` or a board preset is reconciled
on every `tools/build_board.sh` build, plain board builds included. Changing
such a key with `menuconfig` in a build's sdkconfig therefore does not stick:
the next build resets it to the preset. Save the value as an SDK choice in a
named configuration instead, as `xiao-default` does for
`CONFIG_BT_ENABLED`.

Unlisted values inherit the shared and board defaults **at the source
revision being built**. These are feature overlays, not complete frozen release
configurations. Keep the source revision and build manifest with an image.

The script generates literal definitions for the existing CMake source filters
and `System_BuildConfig.h`. It contains no alternate dependency resolver: the guided
setup's checks evaluate that same header. After configuration, the real compiler verifies
the explicit input values. MQTT
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

`xiao-no-bluetooth` was removed on 2026-11-16; `xiao-default` has carried the same choices since
it turned Bluetooth off.

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

## Coverage builds

No saved configuration reaches the branches with networking off, the web server off or every I2C
sensor on. After a change to a disabled branch (CONTRIBUTING.md, "Keep interfaces small"), create
these three and build them; they are throwaway, so delete the files afterwards.

```sh
python3 tools/build_config.py create cover-no-network --from xiao-base --set NETWORK_FEATURE_LEVEL=0
python3 tools/build_config.py create cover-no-web --from xiao-base --set WEB_FEATURE_LEVEL=0
python3 tools/build_config.py create cover-all-sensors --from qtpy-sensors \
  --set CUSTOM_ENABLE_THERMAL=1 --set CUSTOM_ENABLE_IMU=1 --set CUSTOM_ENABLE_APDS=1 \
  --set CUSTOM_ENABLE_FM_RADIO=1 --set CUSTOM_ENABLE_RTC=1 --set CUSTOM_ENABLE_SERVO=1
for name in cover-no-network cover-no-web cover-all-sensors; do python3 tools/build_config.py build $name; done
```

## Removed on 2026-11-16

Fourteen bench, debugging and duplicate configurations were removed. Older documents and
changelog entries still name them; each is a few choices on top of a kept one:

| Removed | Recreate from |
|---|---|
| `xiao-no-bluetooth` | identical to `xiao-default` |
| `xiao-no-network`, `xiao-no-web`, `qtpy-all-sensors` | "Coverage builds" above |
| `p4-glasses-test-quartznet` | `p4-glasses-test` with `ENABLE_STT_MODEL_QUARTZNET=1`, `ENABLE_STT_MODEL_CITRINET=0` (guided setup: Advanced, Speech-to-text model) |
| `p4-glasses-test-prev-onset` | `p4-glasses-test` with `ENABLE_STT_LEVEL_ROBUST_ONSET=0` |
| `p4-full-debug` | `p4-phone-pipe` with `ENABLE_DEBUG_ALL_BY_DEFAULT=1` |
| `p4-phone-pipe-camera-off-test` | `p4-phone-pipe` with `ENABLE_CAMERA_SENSOR=0` |
| `p4-phone-only` | `p4-phone-pipe` with `ENABLE_G2_GLASSES=0` (it also left the SDK choice `CONFIG_HEAP_USE_HOOKS` out) |
| `p4-phone-no-camera-test` | `p4-phone-only` with `ENABLE_CAMERA_SENSOR=0` |
| `p4-phone-pipe-heapdiag` | `p4-phone-pipe` with the SDK choice `CONFIG_HEAP_POISONING_COMPREHENSIVE` on |
| `qtpy-logging-test` | `qtpy-espnow` with the Seesaw gamepad as input control (`CUSTOM_ENABLE_GAMEPAD=1`, `INPUT_DEVICE_TYPE=1`) |
| `qtpy-maps-test` | `qtpy-logging-test` with `ENABLE_MAPS=1` |
| `waveshare-p4-bringup` | the `create` command in [Waveshare support](../docs/WAVESHARE_P4_SUPPORT.md) |
