# Hardware One v0.99.99.1 - User Guide

This is the full reference for Hardware One. It covers every subsystem, all CLI commands, configuration options, and how the major features work. For initial setup, see the [Quick Start Guide](QUICKSTART.md).

## Table of Contents

- [Build Configuration](#build-configuration)
- [Board Reference](#board-reference)
- [Web UI](#web-ui)
- [OLED Interface](#oled-interface)
- [G2 Lens Interface](#g2-lens-interface)
- [Users, Roles & Access](#users-roles--access)
- [Notifications](#notifications)
- [Backup & Restore](#backup--restore)
- [ESP-NOW Mesh](#esp-now-mesh)
- [Automations](#automations)
- [MQTT](#mqtt)
- [Raspberry Pi Co-Processor (CM5)](#raspberry-pi-co-processor-cm5)
- [LLM](#llm)
- [Debug Flags](#debug-flags)
- [Command Reference](#command-reference) - common commands; full generated list in [COMMAND_REFERENCE.md](COMMAND_REFERENCE.md)
- [Per-Module Notes](#per-module-notes)
- [License](#license)

---

## Build Configuration

Feature flags are declared, with their defaults, in `components/hardwareone/System_BuildConfig.h`. That header is *your* configuration surface for an ordinary board build - edit it before building to enable or disable any subsystem. Three other layers can override it: a board's own `boards/<board>.features.h` (the XIAO and the P4 boards have one), a deployment's `deployments/<name>/.../features.h`, and a saved configuration in `configurations/*.json` built with `tools/build_config.py build <name>`, which changes flags without editing the header (see [configurations/README.md](../configurations/README.md) and the ownership table in [CONTRIBUTING.md](../CONTRIBUTING.md)).

> **The values committed in that header are incidental, not a contract.** They are
> whatever the last working tree happened to be built with, and they change from
> commit to commit. This guide therefore does not state what any flag "is" - open
> the header to read your own tree's values. The table below describes what each
> flag *does*; you choose the value.

| Flag | What it controls |
| ---- | ---------------- |
| `I2C_FEATURE_LEVEL` | How much of the I2C stack is compiled: `0`=disabled, `1`=OLED only, `2`=OLED+gamepad, `3`=all sensors, `4`=custom selection via the `CUSTOM_ENABLE_*` flags below |
| `NETWORK_FEATURE_LEVEL` | How much of the network stack is compiled: `0`=disabled, `1`=WiFi only, `2`=WiFi+HTTP, `3`=WiFi+HTTP+ESP-NOW, `4`=custom |
| `WEB_FEATURE_LEVEL` | How much of the web UI is compiled: `0`=disabled, `1`=core UI, `2`=standard modules, `3`=all modules, `4`=custom |
| `DISPLAY_TYPE` | Which panel driver is compiled: `0`=none, `1`=SSD1306 OLED, `2`=generic ST7789 TFT, `3`=generic ILI9341 TFT, `4`=ST7789 240×240 LCD of the ESP32-P4X-EYE, `5`=HX8394 720×1280 DSI panel of the Waveshare ESP32-P4 Touch LCD 5. `4` and `5` are set by those boards' feature files. SSD1306 is the tested path for the other boards; `2` and `3` are example branches without board wiring, and a saved configuration that selects them is refused at compile time |
| `INPUT_DEVICE_TYPE` | Which physical input controller is compiled, mutually exclusive: `0`=none, `1`=Seesaw gamepad, `2`=ANO rotary encoder, `3`=GPIO wheel and buttons (ESP32-P4X-EYE), `4`=GT911 touch panel (Waveshare ESP32-P4 Touch LCD 5). `3` and `4` are set by those boards' feature files |
| `ENABLE_HTTPS` | Set to `1` for TLS on the web server; certs in `/system/certs/`. Runtime toggle: `httpsenabled` |
| `ENABLE_MAPS` | Set to `1` to build offline maps and waypoints |
| `ENABLE_GAMES` | Set to `1` with `ENABLE_WEB_GAME_DARKROOM=1` to build A Dark Room and its browser launcher |
| `ENABLE_ESP_SR` | Set to `1` to build ESP-SR voice: WakeNet wake word + MultiNet command grammar |
| `ENABLE_BLUETOOTH` | Set to `1` to build the BLE server with GATT services. It gates *our* code only - to actually reclaim the Bluedroid stack's flash/RAM you also need `CONFIG_BT_ENABLED=n` in sdkconfig, and on a board whose sdkconfig drops the stack the header's derived rules force this flag back to `0` |
| `ENABLE_G2_GLASSES` | Set to `1` to build the Even Realities G2 BLE client (requires `ENABLE_BLUETOOTH=1`) |
| `ENABLE_R1_HEALTH` | Set to `1` to build the R1 Health vitals UI (G2 Apps->Health, OLED, Web `/r1-health`) + health logging. Requires Bluetooth + G2; forced off if either is off. Ring connect stays under `ENABLE_G2_GLASSES`. |
| `ENABLE_MQTT` | Set to `1` to build the Home Assistant MQTT integration |
| `ENABLE_AUTOMATION` | Set to `1` to build scheduled tasks and conditional commands |
| `ENABLE_CAMERA_SENSOR` | Set to `1` to build the shared camera system and the board's DVP or CSI backend |
| `ENABLE_MICROPHONE_SENSOR` | Set to `1` to build the board's microphone: PDM over I2S on the XIAO ESP32-S3 Sense and the ESP32-P4X-EYE, the ES7210 codec on the Waveshare ESP32-P4 Touch LCD 5 |
| `ENABLE_BATTERY_MONITOR` | Enable the board's ADC or fuel-gauge battery backend |
| `ENABLE_EDGE_IMPULSE` | Set to `1` to build Edge Impulse ML inference |
| `ENABLE_BONDED_MODE` | Set to `1` to build Bonded Microcontrollers - two devices share command registries and the controller shows a Remote tab with the paired device's features |
| `ENABLE_LLM_BACKEND` | Master switch for the LLM feature (web LLM page, OLED LLM mode, `llm*` commands, lens viewer). Set to `1` *and* turn on at least one source below; a build error tells you if you forget. |
| `ENABLE_LLM_SOURCE_ONBOARD` | Set to `1` to build the tiny transformer that runs on this chip (needs PSRAM). Requires a model file on LittleFS or SD card. |
| `ENABLE_LLM_SOURCE_CM5` | Set to `1` to answer from the Raspberry Pi co-processor over the UART link. Requires a board with a `UART_LINK_PORT` (all supported boards). |
| `ENABLE_LLM_SOURCE_HTTP` | Set to `1` to answer from an OpenClaw server at a configured HTTP(S) endpoint. Requires WiFi; no local model. |
| `ENABLE_LOCAL_STT` | On-device speech-to-text on the ESP32-P4 boards (the engine is compiled only for that target). Needs a model file on the microSD card at run time: Citrinet-384 by default (`ENABLE_STT_MODEL_CITRINET`), or the published QuartzNet 15×5 with `ENABLE_STT_MODEL_QUARTZNET=1` and `ENABLE_STT_MODEL_CITRINET=0` (one model per build). Where the file comes from and how to install it: [Local speech-to-text model](README_DETAILS.md#local-speech-to-text-model-esp32-p4) |
| `ENABLE_RASPBERRY_PI_HOST_POWER` | Set to `1` to build `cm5 power` - profile, reboot, halt, suspend, timed sleep of the Pi with confirmed request/ACK |
| `ENABLE_RASPBERRY_PI_HOST_FAN` | Set to `1` to build `cm5 fan` - quiet/auto/max with temperature, PWM, RPM and health readback |

When `I2C_FEATURE_LEVEL = 4`, individual sensors are controlled by `CUSTOM_ENABLE_*` flags:

| Flag | Sensor |
| ---- | ------ |
| `CUSTOM_ENABLE_OLED` | SSD1306 OLED display |
| `CUSTOM_ENABLE_GAMEPAD` | Seesaw gamepad |
| `CUSTOM_ENABLE_IMU` | BNO055 9-DoF IMU |
| `CUSTOM_ENABLE_TOF` | VL53L4CX Time-of-Flight |
| `CUSTOM_ENABLE_THERMAL` | MLX90640 thermal camera |
| `CUSTOM_ENABLE_APDS` | APDS9960 gesture/proximity/light |
| `CUSTOM_ENABLE_GPS` | PA1010D GPS |
| `CUSTOM_ENABLE_RTC` | DS3231 RTC |
| `CUSTOM_ENABLE_PRESENCE` | STHS34PF80 IR presence sensor |
| `CUSTOM_ENABLE_FM_RADIO` | RDA5807 FM radio |
| `CUSTOM_ENABLE_SERVO` | PCA9685 servo controller |

> If a module is enabled in the config but not physically connected, its commands will fail gracefully - the rest of the system is unaffected.

---

## Board Reference

See [BOARD_SWITCHING.md](BOARD_SWITCHING.md) for full menuconfig tables.

### Per chip family

Set by `config/sdkconfig.defaults.<target>`; `tools/build_board.sh` picks the
target from the board file's `# HW_TARGET:` line:

| | ESP32 (QT Py, Feather V2) | ESP32-S3 (XIAO, FeatherS3) | ESP32-P4 (P4X-EYE, Waveshare) |
| - | ------------------------- | -------------------------- | ----------------------------- |
| PSRAM | 40 MHz | 80 MHz | hex mode |
| Flash mode | `DIO` | `QIO` | `DIO` |
| Radio / Bluetooth | Classic BT + BLE 4.2 | BLE 5.0 only | none on chip: Wi-Fi, BLE and ESP-NOW through the onboard ESP32-C6 (ESP-Hosted) |
| Camera/Mic | No | XIAO Sense | Yes (board-dependent, see below) |
| USB console | UART bridge (`usbserial`) | Native USB (`usbmodem`) | P4X-EYE native USB; Waveshare UART bridge |
| Target | `esp32` | `esp32s3` | `esp32p4` (needs `tools/p4/prepare_sdk.sh` once) |

### Per board

Set by `boards/<name>.defaults`, layered on top of the family file. PSRAM
**mode**, the Arduino pin variant, flash size, and the partition table are all
board properties, not chip-family properties:

| Board | Board file | Arduino variant | PSRAM mode | Flash |
| ----- | ---------- | --------------- | :--------: | :---: |
| Unexpected Maker FeatherS3 | `feathers3` | `um_feathers3` | **Quad** | 16 MB |
| Seeed XIAO ESP32-S3 | `xiao_s3` | `XIAO_ESP32S3` | **Octal** | 8 MB |
| Adafruit QT Py ESP32 | `qtpy_esp32` | `adafruit_qtpy_esp32` | **Quad** | 8 MB |
| Adafruit Feather ESP32 V2 | `feather_esp32_v2` | `adafruit_feather_esp32_v2` | **Quad** | 8 MB |
| FeatherS3, flash-encrypted (experimental; first boot burns eFuses) | `feathers3_fe` | `um_feathers3` | **Quad** | 16 MB |
| Espressif ESP32-P4X-EYE | `p4x_eye` | `esp32p4` | **Hex** (family file) | 16 MB |
| Waveshare ESP32-P4 Touch LCD 5 (experimental) | `waveshare_p4_touch_lcd_5` | `esp32p4` | **Hex** (family file) | 32 MB |

> **Do not infer PSRAM mode from the chip family.** The ESP32-S3 boards above
> disagree - the XIAO is octal, the FeatherS3 is quad. A wrong mode is a *silent*
> failure: the device boots and reports 0 KB PSRAM, and the first thing you notice
> is the LLM, large web responses, or ESP-NOW buffers OOMing at runtime.

Build every board with `tools/build_board.sh <board>`: each board gets its own
`build-<board>/` directory and sdkconfig, so switching boards or chip families
needs no `fullclean` or `set-target`. A bare `idf.py` in the repository root is
not a supported build path (see the [Quick Start](QUICKSTART.md#3-set-your-target-board-and-flash)).

---

## Web UI

Navigate to the device's IP address in a browser. The web server must be running - start it for this session with `openhttp`, or set `httpautostart 1` to bring it up on every boot.

Which pages exist depends on the per-page compile gates (`CUSTOM_ENABLE_WEB_*` when `WEB_FEATURE_LEVEL = 4`); a page whose gate is off is not in the binary and cannot be enabled at runtime.

| Page | Route | Gate | What it does |
| ---- | ----- | ---- | ------------ |
| **Dashboard** | `/dashboard` | core | Landing page - device status, quick links |
| **Sensors** | `/sensors` | `WEB_SENSORS` | Live sensor data, start/stop individual sensors, camera stream |
| **ESP-NOW** | `/espnow` | `WEB_ESPNOW` | Peer list, pairing, metadata sync, file transfer, mesh status |
| **Bond** | `/bond` | `WEB_BOND` | Remote tab for the bonded peer's features (requires `ENABLE_BONDED_MODE`) |
| **Automations** | `/automations` | `ENABLE_AUTOMATION` | Create, edit, enable, and run automations |
| **Files** | `/files` | core | File manager - browse, view, upload, create, delete on LittleFS and SD |
| **Logging** | `/logging` | core | Browse the capture tree (`/logging_captures`), sensor logs, GPS tracks |
| **Maps** | `/maps` | `WEB_MAPS` | Offline map viewer, waypoints, GPS track logging (requires `ENABLE_MAPS`) |
| **Bluetooth** | `/bluetooth` | `WEB_BLUETOOTH` | BLE status and controls, G2 and R1 ring connect (requires `ENABLE_BLUETOOTH`) |
| **R1 Health** | `/r1-health` | `WEB_R1_HEALTH` | Ring vitals, graphs, health logging (requires `ENABLE_R1_HEALTH`) |
| **Battery** | `/battery` | `WEB_BATTERY` | Voltage, charge level, battery log |
| **MQTT** | `/mqtt` | `WEB_MQTT` | Broker config, topic preview, Home Assistant status (requires `ENABLE_MQTT`) |
| **Speech** | `/speech` | `WEB_SPEECH` | ESP-SR status and tuning (requires `ENABLE_ESP_SR`) |
| **LLM** | `/llm` | `ENABLE_LLM_BACKEND` | Model chat - load/unload, ask, temperature and sampling. Answers come from whichever source the build enables (on-chip model and/or the CM5 co-processor) |
| **Games** | `/games` or `/darkroom` | `WEB_GAMES` | A Dark Room (requires `ENABLE_GAMES` and `ENABLE_WEB_GAME_DARKROOM`) |
| **Settings** | `/settings` | core | All device settings, debug flags, user management |
| **CLI** | `/cli` | core | Full command interface in the browser, with history |

Authentication is required; `/login` and `/register` are the unauthenticated entry points. Credentials are created on first boot via the setup wizard, or later with the `users` CLI commands.

> There is no **Pair** page. Pairing and bonding are run from the ESP-NOW page.

---

## OLED Interface

The OLED (or the P4 board's LCD) displays a menu system navigated with the input device that `INPUT_DEVICE_TYPE` selects - Seesaw gamepad or ANO rotary encoder, or the P4X-EYE wheel or Waveshare touch panel. On first boot the setup wizard runs here as well as on serial (see the [Quick Start](QUICKSTART.md#first-time-use)). After that it lands on the main menu.

The main menu is six categories. Rows inside each are compile-gated, so a build without (say) ESP-NOW or an LLM simply shows fewer entries. The G2 lens mirrors this layout - see [G2 Lens Interface](#g2-lens-interface).

| Category | Rows |
| -------- | ---- |
| **System** | Status, Memory, Perf, Notifications, CLI Output, CLI Input, Logging |
| **Config** | Settings, Login, Logout, Change PW, Users (admin), Gamepad PW |
| **Connect** | Network, Bluetooth, Bond, Web |
| **Hardware** | Sensors, Microphone, Speech, I2C Scan, LED |
| **Apps** | ESP-NOW, Files, Maps, LLM Chat, Automations, Health |
| **Power** | Power |

Notes on placement, which changed in the menu reorg:

- **ESP-NOW** lives under **Apps**, not Connect - it is treated as a messaging program, and unlike the G2 the OLED bundles the ESP-NOW *settings* into the same mode.
- **Maps** lives under **Apps** and needs both `ENABLE_GPS_SENSOR` and `ENABLE_MAPS`.
- **Sensors** is a submenu: Data, List, then one row per compiled sensor (Thermal, ToF, IMU, APDS, GPS, Gamepad, FM Radio, RTC, Presence, Camera).
- **Users** is hidden for non-admins, and the mode refuses to open without an admin session.
- **Health** (R1 ring vitals, Poll Now, Health Logging) requires `ENABLE_R1_HEALTH`. Ring *pairing* is under Connect -> Bluetooth.

---

## G2 Lens Interface

Requires `ENABLE_BLUETOOTH=1` and `ENABLE_G2_GLASSES=1`. The ESP32 acts as BLE
central and drives the Even Realities G2 temples directly - the firmware
"hijacks" the lens to render its own pages, so this is a full on-glasses UI, not
a notification mirror. Connect with `openg2`; navigate by tapping the temple.
Holding the ring on any of these pages records a private note - see
[Hold (long-press) and QuickNote](#hold-long-press-and-quicknote).

The main menu mirrors the OLED's six categories:

| Category | Rows |
| -------- | ---- |
| **System** | Status, System Events, Logging, Tests (only on an `ENABLE_G2_TESTSUITE` build) |
| **Config** | Settings, Users (admin only) |
| **Connect** | WiFi >>, Bluetooth >>, ESP-NOW >> |
| **Hardware** | Sensor list (one row per compiled sensor; the RTC row and page show local time), LED, FM tuner |
| **Apps** | ESP-NOW, Files, Notes, Maps, LLM, Automations, Health |
| **Power** | CPU presets, restart, power off |

Sub-pages are registered but hidden from the top level - they are reached
through their category, and every page has a `<- Back` row. A few sit one level
deeper still: **Camera settings** is reached from Hardware -> sensor list -> `CAM`
-> `Settings >`, and **Mic detail** from the `MIC` row the same way.

### Apps

| App | Notes |
| --- | ----- |
| **ESP-NOW** | Peer messaging. Distinct from Connect -> ESP-NOW, which owns the settings. `Peers (2/3 up)` counts the paired devices other than this one and how many hold an encrypted session, as the status line's `ESPNow: 2/3 up` |
| **Files** | Browse, view, rename, delete. Folders carry an item-count badge |
| **Notes** | Private spoken notes (QuickNote): `● New note`, your newest notes, read and delete (requires `ENABLE_QUICKNOTE`) - see [QUICKNOTE.md](QUICKNOTE.md) |
| **Maps** | Pan/zoom offline map viewer. Row reads `Maps (none)` when no map files are present (requires `ENABLE_MAPS`) |
| **LLM** | Submenu: Open chat / Ask (Mic / Keys) / Ask (guided) / Re-run last / Select Model (requires `ENABLE_LLM_BACKEND`) - see [LLM](#llm) |
| **Automations** | List, view, and run automations (requires `ENABLE_AUTOMATION`) |
| **Health** | R1 ring vitals + sparkline graphs (requires `ENABLE_R1_HEALTH`) - see below |

### Apps -> Health

Left column is a metric list, right side a 288x144 graph: Overview, Activity,
Trends, Heart Rate, HRV, SpO2, Temperature, Battery, Poll Now, Health Logging.

- **Overview** - native-text vitals with wear state and one shared recentness figure on the status line.
- **Trends** - submenu graphing the ring's daily-history payload (HR / HRV / SpO2 today + Refresh), kept separate from the live sparklines.
- **Poll Now** - one composite refresh: a full daily sweep plus a correlated device-status read, under a 75-second ceiling. The status line walks `Refreshing...` -> `Refreshed`, or reports `Refresh incomplete` / `Refresh failed` / `Refresh unsupported`, so "Refreshed" means the sweep finished rather than "the requests were sent". Simply *opening* the page no longer fires a poll burst - it reads the freshness-throttled history lane instead.
- **Health Logging** - toggles local R1 logging (`healthlogging on` / `healthlogging off`). While on, the ring is mined every `healthLoggingPollIntervalSec` seconds (default 900 = 15 min). Opening the page or **Poll Now** also logs a sample when R1 logging is active.

On-demand refresh is a per-firmware capability. HardwareOne recognises four R1
firmware profiles (**2.2.7.0005**, **2.2.9.0003**, **2.3.2.0007** and
**2.3.2.0008**); on-demand refresh is admitted on 2.2.9.0003, 2.3.2.0007 and
2.3.2.0008. On 2.2.7.0005 or an unrecognised firmware the row reads `Poll unsupported` (or
`Passive only` on the metric readouts) and no command is sent, instead of arming
a request whose only outcome is a timeout. The same gate applies on the web page
(**Poll Now** renders disabled as `Poll unavailable` / `Poll unsupported`) and on
the OLED R1 Health page. Live sparklines are right-anchored: the newest sample
sits at the right edge and any slack shows on the left, where it reads as "no
data that far back". Trends keeps its own left-anchored day window.

Pair the ring under Connect -> Bluetooth. The same vitals appear on the OLED
(**Apps -> Health**) and the web (**`/r1-health`**).

### Hold (long-press) and QuickNote

A plain hold of the ring (or a temple) on a HardwareOne page reaches the
firmware as a press when the glasses recognise it and a release when the finger
lifts. On the Notes page a hold records a note; on any other HardwareOne page
the global hold action (`g2holdaction`, default QuickNote where it is built)
switches to Notes and records. Letting go saves the note. Full guide:
[QUICKNOTE.md](QUICKNOTE.md).

- Holds reach the firmware only while a HardwareOne page is on the lens - open
  Blocks first. There is no trigger from the idle lens, the dashboard or a
  native app.
- Tap then hold (the glasses menu), holding both temples (silent mode) and
  double tap (back) stay with the glasses.
- Holds are ignored while the firmware menu or a notification covers the page,
  while the keyboard is open, and on pages that cannot be left (Maps, Health,
  the LLM, image and camera viewers).
- By default only the ring starts a note (`g2holdsource 0`); `g2holdsource 1`
  adds the temples. That needs glasses firmware that reports the source
  (2.3.2.14); on 2.2.9.22 every hold is accepted.
- Holds act only on glasses firmware with a hold profile (both arms 2.3.2.14
  or 2.2.9.22). On anything else they are recorded but do nothing;
  `g2hold status` shows the profile and the last 16 holds.
- Nothing is written to the glasses: the Even app's own Press & hold binding,
  the gesture table and the menu are left as they are.

### Config -> Users

Admin only. The list carries a `+ Add User` row; the add flow is username ->
role -> password -> confirm.

Opening an account always shows two rows first: **`Role: <role>`** and a
**`Status: Active/Banned`** line. The role row is only *editable* - it gains a
`>` and opens the role picker - when the target is none of: the founder, the
identity that currently owns the lens, or an account of higher rank than you.
Otherwise it is a read-only info row.

What comes after those two rows is **one of four mutually exclusive branches**,
not a single menu - you never see the whole set on one account:

| The account is | Rows you get |
| -------------- | ------------ |
| Higher rank than you | `Higher role - protected` and nothing else |
| The founder | `Founder - protected`; plus **Change Password** only if the founder is *you* |
| The lens owner's identity | `Current G2 owner` if it is you, else `G2 owner name collision`; plus **Change Password** only if it is you |
| Anything else | **Reset Password**, **Kick Sessions**, **Ban/Unban User**, **Delete User** |

So a normal account offers the four admin actions and **no Change Password row**;
Change Password appears *only* on the founder-self and lens-owner-self branches,
where it is the single available action. Reset Password (admin) offers a
`Save Password` or `Save + Require Change` choice, and every destructive row
goes through an explicit on-lens confirm row. Kick and ban appear only on builds
with `ENABLE_HTTP_SERVER`, which provides their handlers.

The name-collision case is worth knowing about: if a username differs from the
lens owner's only by letter case it lands in the owner-identity branch as
`G2 owner name collision` and offers nothing at all, because account lookup is
case-sensitive but session revocation is not - the destructive rows would act on
the wrong account.

### Text entry

Pages that need input - WiFi join, file rename, ESP-NOW name and messages, a
settings value, a user password, the Maps search box, the LLM's
`Ask (Mic / Keys)` row - open the shared on-lens keyboard. Since v0.99.92 that
is an **arrow-pad QWERTY grid** as the default surface; the old grouped cycling
keyboard is still in the firmware as the last-resort fallback (see below).

The layout is a 288x144 key grid on the right and a 7-row nav list on the left,
with a live text pane showing the prompt, the buffer and a character count:

| Nav row | Does |
| ------- | ---- |
| **X Cancel** | Abandon the field |
| **Done** | Commit what is in the buffer |
| **Up** / **Down** | Move the cursor one grid row (wraps) |
| **Left** / **Right** | Move the cursor one grid cell (wraps) |
| **Mic / Keys** | Toggle between the key grid and the speech page - see below |

**Selecting the highlighted key is an R1 ring double-tap**, not a list row.
The captured firmware reports that gesture with no row index, so the pad takes
the rowless double-click directly - no Select row is needed, and the nav list
never has to be rebuilt while you type. The gesture only reaches the firmware
when the ring is BLE-paired to the glasses; temple taps drive the nav rows.

The grid is 5 rows x 10 columns across **three pages** - lowercase, uppercase,
and symbols. Shift, the page toggle (`#!` on the letter pages, `ab` on the
symbol page), backspace and the space bar are grid *cells*, so the whole
character set is reachable without any list rebuild. The cursor starts on `q`.
Two things to know: the grid deliberately has **no double-quote key** (text
entry has to stay safe to wrap in a quoted CLI argument), and a pre-filled value
longer than the field's own limit is silently truncated - most fields cap at 32
characters, and the surface's ceiling is 256.

If the lens refuses the four-band grid, the firmware falls back to a single
keyboard image, and then to the legacy grouped character list, without telling
you - the field still works.

**Mic row (dictation).** Tapping **Mic / Keys** from a key page opens a speech
page: a short `GET READY` countdown, then `SPEAK NOW` (`AUTO-STOPS AFTER
SILENCE`), then `TRANSCRIBING` while the Pi works. The transcript is appended to
the field you were typing in. Tapping the same row again stops an in-progress
recording, cancels a transcription, or returns you to the keys. The row reads
**`Mic disabled`** when the field is a secret (passwords and PSKs never
dictate) or when no paired-user session owns the lens. Dictation needs a
microphone build *and* a logged-in Raspberry Pi co-processor that has advertised
its capability - see [Raspberry Pi Co-Processor](#raspberry-pi-co-processor-cm5).
Captures are capped at 30 seconds.

### CLI

Most lens pages have a CLI equivalent - `g2status`, `g2show`, `g2clear`,
`g2nav`, `g2health`, `g2map`, `g2files`, `g2sensors`, `g2settings`,
`g2network`, `g2battery`, and the `g2ai*` / `g2mic*` / `g2notify*` families.
Run `help g2` on the device for the full list. `g2hold status` reports the hold
input, and `note` starts, stops and browses QuickNotes (`note start glasses`
from serial, the BLE app or UART).

> `openg2` and `openring` return OK when the connection is *kicked off*, not
> when it completes. A menu that reads the link state immediately after may
> still show "disconnected".

---

## Users, Roles & Access

One account database (`users.json`) governs every interface. The same command
run over serial, the web CLI, the OLED, BLE, voice, or an ESP-NOW peer goes
through the same permission check.

### Role tiers

Four ranks, lowest to highest. Accounts store the role *name*; the ranks below
are what comparisons use.

| Role | Rank | Can do |
| ---- | :--: | ------ |
| `guest` | 0 | A real, named account with view-only authorization. It may use caller-local `login`, `logout`, and `whoami`; filesystem access is masked to read |
| `user` | 1 | Ordinary commands; not admin-gated ones |
| `admin` | 2 | Admin commands - user management, most settings, device control |
| `superadmin` | 3 | Additionally the identity / crypto / destructive / auth-posture command set |

Rules that hold everywhere:

- **You cannot grant a role above your own.** Granting or removing `superadmin` requires a super-admin caller.
- A command marked super-admin-only is refused for an ordinary admin - `blerequireauth`, `serialrequireauth`, and `displayrequireauth` are examples, since they change whether the device demands a login at all.
- An unrecognised role name collapses to `user`.
- A role is what `users.json` says for that account; an account stored without a role is a `user`,
  the first account included. A device without a `superadmin` entry has no super admin, and the
  super-admin commands (`factoryreset`, `certgen`, pairing secrets, `sdformat`) are unavailable;
  `userlist` and the boot log say so. A device first set up before v0.99.0 may be in that state:
  before updating, check `/system/users/users.json` for `"role": "superadmin"` and, if none is
  there, add one on the old firmware (`useradd <name> <password> 0 superadmin`).
- The stored `guest` role is not a username and is not the internal `AuthBypass` sentinel. `AuthBypass` appears only when a transport's require-auth policy is disabled; it is not an account and never grants cross-transport session control.

Set roles with `useradd ... [role]`, `userpromote`, and `userdemote` - see the
[users command block](#command-reference).

### Per-transport authentication

Each way in has its own switch for whether a login is required, and its own
idle-logout timer. All the `*requireauth` switches are super-admin-only.

`login`, `logout`, and `whoami` are caller-relative by default: a bare command
always acts on the Serial, UART, BLE, or OLED interface that submitted it. A
named, non-Guest account already signed in on Serial, UART, or OLED may manage
one of the other local command interfaces explicitly:

```
login <user> <pass> <serial|uart|display>
logout <serial|uart|display>
```

Explicit same-interface forms are rejected; use the bare form instead. BLE,
G2, web, anonymous, `AuthBypass`, and Guest-role callers cannot administer a
different interface. The old G2 -> OLED Login menu has been removed; sign in
on an eligible command interface before performing any cross-interface login.

Queued commands and replies are fenced by boot-local session counters. These
epochs are sequence numbers, not login timestamps: NTP may be absent at boot or
may become available between commands without changing their meaning.

```
serialrequireauth <0|1>         - Require login on the serial console
displayrequireauth <0|1>        - Require login on the local display, OLED menu included (super admin)
blerequireauth [on|off]         - Require login for BLE access

sessionidleserial <0-1440>      - Serial idle-logout, minutes (0 = never)
sessionidleweb <0-1440>         - Web CLI idle-logout
sessionidledisplay <0-1440>     - OLED idle-logout
sessionidleble <0-1440>         - BLE idle-logout
```

Sessions and bans are managed with `sessionlist`, `sessionrevoke`, `ban` /
`unban` / `banlist` (by IP) and `banuser` / `unbanuser` (by account).

### Account recovery

`factoryreset` deletes every account, the pending account requests and each
user's own settings, then reboots into the setup wizard; device settings and
files stay. It is a super-admin command that must be run from a logged-in
super-admin session, so it is **not** a way back in once the admin credentials are
lost:

- With a second administrator account: that admin signs in and runs
  `userresetpassword <user> <pass>` (the user chooses a new password at the next
  login), or, if that account is a super admin, `factoryreset` to start over.
- With a single admin account and no other way in: erase the device over the
  cable (`tools/build_board.sh <board> -p PORT erase-flash`, or erase just the LittleFS region) and
  flash again. This removes settings and captured data as well; keep backups
  (`backup` / Backup & Restore below) if they matter. On a recovery OTA layout
  erase only the filesystem (`littlefs-flash`, `encrypted-littlefs-flash` on
  `feathers3_fe`); a full erase also removes the recovery updater and is
  refused from a recovery-OTA build unless `HW1_ALLOW_FULL_ERASE=1` is set (see
  Update and recovery below).

**Handing a device over:** what `factoryreset` keeps includes the saved WiFi
networks, the MQTT and LLM endpoint credentials, Bluetooth bonds other than the
phone pipe, and the ESP-NOW identity and keys, so it does not prepare a device
for a new owner. `factoryreset all` does: run it on the serial console as a
super admin. It does the ordinary reset, restarts, erases the whole internal
filesystem and NVS before anything is mounted (up to a few minutes; it resumes
if power is lost) and then runs first-time setup. Firmware and the speech model
stay. It does not wipe the SD card apart from `/sd/stt/u*`, so remove or wipe
the card yourself, and on boards with a radio companion chip (P4) that chip
keeps the WiFi network it last joined. The `otapin` recovery credential is
erased with the rest; set it again on a recovery-OTA device you keep. The
command is refused over the web, Bluetooth and MQTT and while a new firmware
is in OTA health probation. It has not been run on hardware yet.

The other way is to erase the whole flash over the cable
(`tools/build_board.sh <board> -p PORT erase-flash`) and flash it again. On a
recovery-OTA layout this also removes the recovery updater, so the command is
refused there until `HW1_ALLOW_FULL_ERASE=1` is put in front of it; provision
the device again with [OTA_DEVICE_SETUP.md](OTA_DEVICE_SETUP.md).

First-time setup refuses reserved owner names (`system`, `AuthBypass`, the
bond admin) and passwords shorter than six characters, so a new device cannot
be set up into a lockout. Compatibility and upgrade promises:
[COMPATIBILITY.md](COMPATIBILITY.md).

### Update and recovery

- **Which versions a board runs**: `versions` lists the firmware version and
  everything it must match - the recovery updater, the ESP-NOW format, the phone
  link, the C6 companion, the STT model and the SDK it was built with
  ([COMPATIBILITY.md](COMPATIBILITY.md)); `versions json` gives the same for tools.
- **Cable update** works on every board: build (or download) the image for your
  board and flash the app over USB. On ESP32-P4 boards the app is written alone
  (`esptool write_flash 0x10000 <app>.bin`); the filesystem and your data stay.
- **Recovery OTA layout** (`HW_OTA_LAYOUT=1`, FeatherS3, Feather ESP32 V2, QT Py
  ESP32; the XIAO ESP32-S3 through the `pocket_assistant` deployment): a signed
  update is staged over Bluetooth or the recovery access point, applied on
  probation and confirmed with `otaack`; `otastatus` shows the state. Setting it
  up is a one-time, destructive migration described in
  [OTA_DEVICE_SETUP.md](OTA_DEVICE_SETUP.md). A layout counts as supported only
  when the [support matrix](README_DETAILS.md#support-matrix) lists a dated PASS for it on a current
  image; until then it is experimental and the cable path is the supported one.
- **Moving from 0.99.x to 1.0** is a full-erase install (`erase-flash`, then
  flash, then the setup wizard). Within 1.x, settings, accounts, networks and
  automations are kept across updates ([COMPATIBILITY.md](COMPATIBILITY.md)).
  On a recovery OTA layout the erase also removes the recovery updater and the
  recovery credential, so it needs `HW1_ALLOW_FULL_ERASE=1` in front, and the
  plain `flash` is refused there: install with
  `migration-flash` (`encrypted-migration-flash` on `feathers3_fe`) and then
  `setpin`, steps 3-4 of [OTA_DEVICE_SETUP.md](OTA_DEVICE_SETUP.md).

---

## Notifications

System events feed a single notification pipeline. Whether a given event
becomes a pop-up depends on three layers that are evaluated in order.

### Sinks

Where a notification can render:

| Sink | Surface |
| ---- | ------- |
| `BANNER` | Transient OLED banner/ribbon |
| `TOAST` | Web toast, pushed over SSE |
| `G2` | Native notification card on the G2 lens |
| `QUEUE` | Persistent notification-center list (silent history) |

`BANNER`, `TOAST`, and `G2` are the **interrupt** surfaces - they grab
attention, and the per-user importance floor gates all three identically.
`QUEUE` is history and is never floor-gated, so the notification center keeps a
record even of events that never popped up.

### Importance tiers

Every event kind carries a tier, orthogonal to which subsystem it came from:

| Tier | Meaning | Examples |
| ---- | ------- | -------- |
| `VERBOSE` | Chatty/info, opt-in | setting changes, sensor start/stop, gestures |
| `STANDARD` | Genuinely useful - **the default floor** | presence, inbound messages, WiFi, battery low |
| `ALERT` | Must-know | security, safety, faults |

### The three layers

1. **Compiled default** - which sinks a kind can render to at all.
2. **Device policy** - per-kind visibility for the whole device, from `/system/notifications.json`. Admin-set.
3. **Per-user preference** - each viewer's importance floor, plus per-kind force-on and force-off masks.

So a kind must survive its device policy *and* clear the viewer's floor (or be
explicitly forced on) before an interrupt surface fires.

### Commands

```
--- Device-wide (admin) ---
notifydevicebanners <0|1>       - Enable/disable OLED banners
notifydevicetoasts <0|1>        - Enable/disable web toasts
notifydeviceg2 <0|1>            - Enable/disable G2 lens cards
notifydevicequeue <0|1>         - Enable/disable the notification-center queue
notifydevicekind <kind> <all|admin|off>  - Per-event visibility, device-wide

--- Per-user ---
notifylevel <tier>              - Your importance floor
notifyusermute <kinds>          - Kinds you never want, on any sink
notifyusershow <kinds>          - Kinds that punch through your floor

--- Diagnostics ---
notifstats [reset]              - Pipeline counters: loss, suppression, ring lag, SSE drops
```

The event kinds are the same ones listed in the
[Automations event-trigger table](#event-triggers) - `events` in the CLI shows
them live, which is the quickest way to find the exact kind name to tune.

---

## Backup & Restore

The migration tool exports the device's configuration to a `.hwbackup` file and
imports it onto another device. Requires WiFi and the HTTP server
(`ENABLE_MIGRATION_TOOL` follows `ENABLE_HTTP_SERVER`).

- **Export** - `/api/backup` from the web UI.
- **Import** - `/api/restore`, or choose **Import from Backup** at first boot. In the first-boot flow the device brings up WiFi, prints its IP, and waits for you to send the `.hwbackup` from another device's migration tool; you then confirm on the OLED or serial.
  Before the Yes/No prompt (default No) the device shows the bundle's claimed hostname and board, the bytes it received, the first 8 hex of their SHA-256, and whether the bundle's fingerprint matched this board ("this device"), did not ("other device", credentials skipped), or was missing ("no fingerprint (unverified)", credentials still restored). Compare with your file: `wc -c <file>.hwbackup` and the first 8 hex of `shasum -a 256 <file>.hwbackup`. They match only if the file was sent unchanged; a forced cross-device upload has a `force` flag added, so the device says they will not match and you check the source and board instead.
  A bundle exported by newer firmware (`formatVersion` above 1) is refused before anything is staged; update this device's firmware first. A bundle without `formatVersion` is read as format 1. `versions` shows the format this firmware writes (`Backup bundle format`).

This is the fastest way to stand up a replacement board with an existing
device's settings, and the reason a factory reset is not a catastrophe if you
have a recent export.

---

## ESP-NOW Mesh

Hardware One's custom ESP-NOW v1 mesh is its inter-device wireless protocol. Devices pair with a shared passphrase and form an encrypted mesh.

### Pairing
1. On both devices, open the **ESP-NOW** page in the web UI (pairing lives there - there is no separate Pair page) or use the `espnowpair` CLI.
2. Set the same passphrase on both devices.
3. One device initiates - the other accepts.
4. Once paired, devices appear in each other's peer list.

### Radio channel
Devices only hear each other on the same radio channel. `espnowchannel` shows the preference, the radio's actual channel, the channel it should be on and a check; `espnowchannel <1-13>` pins a channel for when the device is not joined to Wi-Fi, `espnowchannel auto` (the default) lets the device choose, and `espnowchannel resync` puts the radio back on the right channel now.

In `auto`:
- **Joined to Wi-Fi**, the access point decides the channel (a radio cannot be on two at once).
- **A mesh master** (or a backup acting as master) not joined to Wi-Fi uses its last Wi-Fi channel, or channel 1.
- **A mesh worker** (with a master named by `espnowmasterfingerprint`, or by `espnowmeshmaster` when no fingerprint is named, and paired) follows its master's channel. Every heartbeat states its sender's channel; the worker remembers its master's last channel across reboots (setting `masterChannel`, read-only) and starts on it.
- **Hunting:** a worker not joined to Wi-Fi that hears nothing from its master for 30 s looks for it: the saved channel first, then channels 1-11, sending one probe per channel and staying on the channel the master answers on. A failed hunt is repeated after 30 s, 60 s, 120 s, then every 5 minutes; between hunts the worker waits on the saved channel. There is no hunt while the worker is joined to Wi-Fi (or connecting to it), while a file transfer, session handshake, key exchange or pairing runs (at most 60 s), on a pinned channel, or on a master. Channels 12 and 13 are not used without Wi-Fi.
- **Wi-Fi wins:** a worker joined to Wi-Fi on another channel than its master pauses the mesh; `espnowstatus` says so ("master on 6, me on 11 (joined to <ssid>)") and the worker follows its master again by itself when the Wi-Fi link ends. While a worker has a live, encrypted session with its master, the automatic Wi-Fi retry waits; `openwifi` still scans.

`espnowstatus` shows a worker's follow state on its `Master channel:` line (`espnowstatus json`: `masterChannel`, `follow`, `masterSilentMs`, `sweepFailures`, `nextSweepMs`). A master on older firmware does not state its channel; a worker still finds it (by its acknowledgement and its next heartbeat), only more slowly, and with several candidates it picks the strongest signal. For a fleet that mixes firmware, pinning the same channel on every device stays the simplest choice.

### Multi-hop (reaching devices out of radio range)
Devices relay for each other, so the mesh reaches further than any single radio.
If A can hear B and B can hear C, then A and C can talk even though they cannot
hear each other. Nothing to configure: each device learns the routes
automatically from its neighbours (allow about a minute after boot), and traffic
goes back to the direct link the moment two devices can hear each other again.

```
espnowmeshroutes                - Who this device can reach, and via whom
espnowmeshttl [1-10]            - How many hops a message may travel (default 3)
espnowmeshrelay [0|1]           - Carry other devices' traffic (default on)
espnowmeshmetrics               - Forwarding counters, if you want to see it working
```

In `espnowmeshroutes`, a peer listed as `(direct)` is in radio range; anything
else is being reached through the device named in the **VIA** column. Turn
`espnowmeshrelay` off on a device you don't want carrying traffic for others
(a battery-tight node, say) — it will still send and receive over multi-hop
itself. Set `espnowmeshttl 1` to opt a device out of multi-hop entirely.

Two things stay deliberately single-hop: **discovery and pairing requests** (the
discover, request and accept exchange in the steps above), so discovery only finds
devices in radio range, and **file transfers**, which are too large to relay. Key
exchange and encrypted session setup do travel through relays, so a paired peer
that moves out of range stays reachable, and pairing by MAC
(`espnowpairsecure <mac> <name>`) with a device that shares the mesh passphrase
completes through relays too. The passphrase, not distance, controls who can join. Messages and remote commands relay fine, but a relayed message is
capped at ~146 characters per piece.

### What a peer's firmware can do (capabilities)
When two paired devices open an encrypted session, each says what its firmware build can do: bond mode,
remote file access, receiving files, sending sensor readings, collecting, relaying, sending mesh time,
and understanding "I restarted" answers. This describes the build, not your settings (turning the relay
off on a device does not change it). `espnowsessions` shows it per peer, and this device's own on the
first line:

```
espnowsessions
  ESP-NOW Sessions:
    This device: caps=bond,remote_fs,file_receive,sensor_send,relay_forward,time_master,session_unknown
    [0] peer=AA:BB:CC:DD:EE:FF meshSlot=0 sessionId=4711 dir=A state=ACTIVE ... caps=remote_fs,file_receive,...
```

A device uses it to avoid asking for what the other cannot do: it never asks a device built without
bond mode to bond (see Bonding), and `espnowsensorreq` refuses a device built without sensors. A bit
shown as `+0x...` comes from newer firmware. `espnowstats` counts frames dropped because they carry a
header flag this firmware must understand and does not (`RX Must-Understand Drops`; newer firmware on
that device).

### Bonding (Master/Worker)
With `ENABLE_BONDED_MODE=1`, two devices can bond into a master/worker pair. The master gains a **Remote** tab in its web UI showing the worker's features, even if those features aren't compiled into the master.

A bond is a setting on each device, so `bondconnect <mac_or_name>` is run on **both**. The two devices then tell each other how the bond stands, and `bondstatus` shows it on the `Agreement:` line:

| Agreement | Meaning | What the device does |
|-----------|---------|----------------------|
| *is bonded to this device too* | the partner has this device as its partner | the bond runs: heartbeat, sync, Remote tab |
| *no answer yet* | the partner has not answered (out of range, or firmware from before the agreement) | as before the agreement existed: it keeps trying, and a partner that sends bond heartbeats counts as agreed |
| *has bond mode off* / *is bonded to another device* / *ended the bond* | the partner answered that it has no bond with this device | keeps its own bond setting, shows the partner as `NOT BONDED TO THIS DEVICE`, sends no heartbeat and asks again once a minute |
| *... but both are master/worker* | both ends have the same role | the bond runs as it is; run `bondrole` on one device |
| *was built without bond mode, so it cannot be a bond partner* | the partner's firmware has no bond mode (its session says so; the P4X-EYE glasses build, for one) | never asks it to bond and starts no handshake for it; run `bonddisconnect`, or install a build with bond mode on the partner: its next session asks again at once. `bondconnect` to such a device is refused |

What you see when something changes (console and event log, tagged `[EVENT][BOND]`):

- on the device that was asked: `<name> wants to bond with this device; run 'bondconnect <name>' here to accept`. Nothing is bonded until you run it; a request never changes the bond settings of the device that receives it.
- on the device that asked: `<name> accepted the bond`, or `<name> has bond mode off` / `is bonded to another device` with the command to run on the other device.
- when the partner runs `bonddisconnect`, bonds to another device, turns bond mode off or unpairs this device: `<name> ended the bond`. This device still names it as partner and asks it again every minute, so the bond returns by itself if the partner runs `bondconnect` again; `bonddisconnect` here ends it on this side too.

**Which device is master.** `bondrole master` or `bondrole worker` chooses the role, and `bondconnect` keeps a chosen role. A device whose role was not chosen (the default) takes the other role than a partner whose user chose one: at `bondconnect`, when the partner already asked to bond with this device, or as soon as the partner's request or answer arrives (the console says `<partner> chose the master role: this device is worker now`). When neither device chose, the one with the higher address is master. When both chose the same role, `bondstatus` shows *both are master* (or *worker*). `bondrole auto` gives the choice up; the role stays until the partner chooses or the next `bondconnect`. `bondrole` with no argument, `bondconnect` and `bondstatus` say whether the role was chosen or how it was decided (`kept as it was set earlier` when it is neither chosen nor the one the addresses give). A partner on older firmware never takes the other role: with a role chosen here that it has too, `bondstatus` shows the conflict until `bondrole` changes one of them.

Running `bondconnect <mac_or_name>` again for the partner already set does not change anything on this device. While the partner has not agreed it asks the partner again at once (at most every 5 s) instead of at the next minute, and the reply says what the partner last answered; once the partner has agreed the reply is `Error: The bond with this device is already running. Refresh it with 'bondresync'.` (or, while there is no session with it, that its handshake started).

A device never says which other device it is bonded to. The web Remote page and the OLED bond screen show the same state (`Bond Not Confirmed`, `No bond there`).

**Your bond partner on the mesh master.** A bonded pair is one device that shows as two on the mesh master, for logging. The bond worker never sends its readings, battery state, events or sensor list to a mesh master itself: they go to its bond master only, whatever master is named on it, and it refuses sensor requests from a mesh master. The bond master can pass them on to the master named on the bond master:

```
bondforward on                     - on the bond master (off by default; the partner cannot change it)
bondforward                        - where the partner's data goes now, or why not
bondforward sensors tof,imu,gps    - which sensors (default: all but input; 'all', 'default')
espnowforwardlinks                 - on the mesh master: who forwards for whom
espnowsensorreq porch via glasses tof subscribe 1000 90000
                                   - on the mesh master: ask for porch's ToF through glasses (a 90 s lease)
```

`bondforward on` is the one switch: the mesh master accepts the link from any paired device that names it as master, as it accepts that device's own readings. Only a master paired in the bond master's primary mesh (the default mesh of `espnowmeshes`) gets the partner's data; a bond master in several meshes never sends it to the others, and `bondforward` says so. The bond master announces the link to that master and forwards once it accepted: readings to the master, battery state and events to the master and the backup master. The mesh master lists the partner's sensors (present, streaming) before its first reading.

The mesh master asks for the partner's readings as it asks any sender: `espnowsensorreq <partner> via <bond master> <sensors> <mode> <intervalMs> <leaseMs>`, where the sensors are names (`tof,imu`), `all` or a mask (`0x6`) and the mode is `subscribe` (`sub`, 0), `unsubscribe` (`unsub`, 1) or `oneshot` (`once`, 2), or `espnowsensorreq <partner> ...` when the partner is not paired with the mesh master (the name `espnowforwardlinks` shows). The bond master switches the partner's sensors on over the bond link, switches them off when the lease ends, and the readings come back forwarded. Only the master that gets the readings may ask, only for forwarded sensors the partner has running (its own sensor list decides: a sensor it lacks is refused `partner_lacks_sensor`, or left out of a request for several; a request that comes before the partner sent its list is refused `partner_status_unknown`, so ask again a few seconds later), and a sensor the partner (or the bond master's user, `bondstream`) streams already stays on after the lease. A lease ends early when `bondforward sensors` no longer forwards its sensor or the mesh master leaves the bond master's primary mesh; when the partner is out of range as a lease ends, the bond master switches the sensor off as soon as the partner is back. The bond master's `bondstatus` and `bondforward` say how many requests it applied and refused (and why last), which sensors were asked for that the partner lacks, and which sensors are leased now; `bondstatus json` counts each refusal reason. A device asked directly (not through a bond master) follows the same rule: it leases only the sensors it has running and counts the rest in `espnowsensorstatus`.

On the mesh master the partner shows as its own device, `porch via glasses`, with its own counts: a reading lost between the partner and the bond master, or between the bond master and the mesh master, counts as lost. What the bond master forwards is kept apart from the partner's own record (if the partner is also in the mesh) and from the bond master's; a partner that is paired with the mesh master is shown under the mesh master's own name for it, and an announced name that another paired device already has is refused. `bondstatus` has "Forwarding to the mesh" lines (on a bond worker also the sensor requests from a mesh master it refused). When the bond master is itself the mesh master, it keeps its partner's data and sends nothing over the air. Data is not stored for later: what could not be forwarded (no session, not accepted yet, the send queue full) is counted on the bond master and shows as lost on the mesh master.

### Metadata Sync
Each device has a name, room, zone, and tags set in settings. The **Metadata** tab lets you pull this information from any peer. Set your own device metadata with (note: single words, no space after `espnow`):
```
devicename [name|clear]         - The device name (see "Device name" below); 'clear' while ESP-NOW is stopped
espnowroom [name]               - Room, e.g. Kitchen. 'espnowroom clear' to unset
espnowzone [name]               - Zone, e.g. Counter. 'espnowzone clear' to unset
espnowtags [tag1,tag2,...]      - Tags. 'espnowtags clear' to unset
espnowfriendlyname [name]       - Display name (<=47 chars); 'clear' to unset
espnowstationary [on|off|0|1]   - Mark the device fixed or mobile
espnowdeviceinfo                - Show all local metadata
```
Called with no argument, each of these prints the current value.

### Device name
A device has one name. It is its ESP-NOW name (in every frame that carries a name), its Bluetooth
name and its Wi-Fi hostname, and it is set with `devicename <name>` (also in the setup wizard, the
web ESP-NOW and Bluetooth pages and the display's ESP-NOW menu). The rule, checked in one place:

- 1 to 31 bytes of ASCII letters, digits and `-`; it does not start or end with `-` (a hostname label);
- not one of the command words `clear`, `all`, `json` or `none`;
- not something that reads as a MAC address (twelve hex digits, with or without separators).

Names are kept as typed and compared ignoring case. When another device this one knows (a paired
device's name here, or a name a peer announces) already has the name, `devicename` sets it and warns
(so does the setup wizard when it saves a new name): both are then shown by address, and commands need
the MAC to reach either. The warning knows only the devices loaded while ESP-NOW has run since the
device started. A peer's name given to
`espnowpairsecure` / `espnow pair` follows the same rule, with the same warning.

Bluetooth takes a new name at its next start and Wi-Fi at its next connection (a name chosen in the
setup wizard is already used for the connection the wizard makes). Without a stored name, Bluetooth and
Wi-Fi use `HardwareOne-B2C3` (the last two bytes of the chip's address, so the name does not carry the
whole address; on a P4 board that is the P4's own address, not the radio address of its C6 that ESP-NOW
shows). Screens and text replies show it as `HardwareOne-B2C3 (default)` (the OLED setup wizard's row
shows `(default)` alone, and Edit opens on the name), JSON replies give `deviceNameSet` false beside it,
and the Bluetooth name and the hostname carry it without the mark. Two unnamed devices can share this
name: give each its own. ESP-NOW does not start until a name is set. A Bluetooth advertisement carries
up to 29 bytes of the name (a longer name is advertised shortened); a connected phone reads the whole
name. A received name that breaks the rule is not used or cleaned: the sender is shown by its address
(an unpaired sender in the pairing list as `peer-B2C3`; when two discovered devices show the same name,
`espnowpairrequest` refuses it and asks for the MAC). The free-text display label is separate:
`espnowfriendlyname` (up to 47 bytes).

A device updated from a build before the one device name (which had `espnowsetname` and a separate
Bluetooth name) keeps its name: the first start after the update takes its old ESP-NOW name, or, when
it had none, its old Bluetooth name unless that was the default `HardwareOne`. A name that breaks the
rule is converted: spaces and `_` become `-`, other characters are dropped, runs of `-` are joined, the
name is cut to 31 bytes and `-` is trimmed at the ends (`Living_Room` becomes `Living-Room`). The system
log has one line naming the name taken and where it came from; when nothing valid is left the old name
is dropped and the line says to set one with `devicename <name>`. This happens once and never replaces a
device name that is already stored; the old settings are removed from `settings.json`.

### Mesh id
Every ESP-NOW frame carries a 16-bit mesh id, which a receiver matches against its own meshes. The id
comes from the mesh key (the label and the passphrase), so a mesh has none until its passphrase is set,
and a mesh without an id sends nothing. `espnowmeshes` shows it. Changing a mesh's passphrase or label
gives it a new id, and its frames and the other devices' are dropped at each other's mesh gate until
they have the same passphrase and label: `espnowstats` counts those frames (`RX Unknown Mesh Id Drops`,
with the last id and sender), and a paired device or a key exchange that sends another id gets one
warning a minute. Clearing a passphrase keeps the id, so paired devices keep exchanging frames. Two
meshes on one device never share an id: `espnowsetpassphrase`, `espnowmeshes rename` and
`espnowmeshes enable` refuse a change that would give one the id of another and change nothing (pick
another passphrase or label). If two meshes have one id anyway (an edited or restored settings file), the
one in the higher slot sends and receives nothing until its passphrase or label changes, with one warning
at each start of the device.

### Rooms, tags and group commands
`espnowroomcmd <room> <user> <pass> <command>` and `espnowtagcmd <tag> <user> <pass> <command>` run a command on every device of a room or with a tag. The user and password are an account on the target devices and travel with the command, encrypted for each device.

Room and tags are what each device says about itself, so they do not decide alone who gets that password. This device keeps, for every paired peer, the room and tags **accepted** here, next to what the peer **announces**:

- A room or tag a peer newly announces is **pending**. It is shown and not used. The device prints one line when that happens: `[EVENT][ESPNOW] <name> says it is in room 'Kitchen'; not used for room commands until accepted here: espnowgroupaccept <name>`. A peer that keeps changing its room or tags is reported once a minute: the first change at once, then one line with the number of further changes and what is pending at the end of the minute.
- A room or tag a peer stops announcing is dropped as soon as this device hears of it, without a confirm. A peer that moves from Garage to Kitchen no longer gets Garage commands from that moment and gets Kitchen commands once it is accepted. A peer tells the devices it has a session with when it changes its room or tags; a device that could not hear it then (restarting, out of range) asks for the peer's room and tags by itself when the session with it is up again.
- This device's own room and tags need no acceptance.

```
espnowgroups [json] [name_or_mac]         - Per peer: announced, accepted and pending room and tags
espnowgroupaccept [json] <name_or_mac|all> - Accept what the peer announces right now; the reply says what
espnowgroupdrop [json] <name_or_mac|all>   - Take back what was accepted for the peer
```
All three need admin. A peer is named by the name it was paired under on this device, or by its MAC address. When two paired devices have the same name, the lists and notices call them by their MAC address and the three commands refuse the name. Example (replies indented):
```
espnowgroups
    Fridge (AA:BB:CC:DD:EE:02)
      announced: room 'Kitchen', tag 'cold'
      accepted:  nothing
      pending:   room 'Kitchen', tag 'cold'
espnowgroupaccept Fridge
    Fridge (AA:BB:CC:DD:EE:02): accepted room 'Kitchen', tag 'cold'
espnowroomcmd Kitchen admin <pass> status
      -> Fridge (AA:BB:CC:DD:EE:02)
    Sent 'status' to 1 device(s) in Kitchen
```
A group command says how many devices it left out: `Skipped 1 device(s) whose room is pending (announced, not accepted here): Lamp. Accept with 'espnowgroupaccept <name_or_mac|all>'`. `espnowtagcmd` takes one tag; a list such as `light,door` is refused, run the command once per tag. Commands sent over MQTT to `room:<name>` or `tag:<name>` follow the same rules.

What was accepted is stored on this device and survives a restart. It is cleared for a peer when the peer is unpaired, when `espnowforget` is run for it, and when a different identity key appears at its address. The lists that show a peer's room (`espnowdevices`, `espnowrooms`, `espnowfind`, the web Metadata tab, the OLED and glasses device lists) show the announced room and mark it (`[pending: ...]`, `(not accepted)` or `?`) while it is not accepted; the web Metadata tab has an **Accept** button.

### File Transfer
Files can be transferred between paired devices via the web UI or CLI (`espnowsendfile <name_or_mac> "<filepath>"`). Used for syncing automations, settings, and manifests.

### File catch-up (device)
A build with `ENABLE_ESPNOW_SEND` (the portable) offers its logs to the masters named on it
(`espnowmasterfingerprint`, `espnowbackupfingerprint`), which pull them over ESP-NOW when the device
is back in range: sensor captures, `events.log`, `battery.csv`, health files sealed at rest,
`errors.log` and `crash-history.log`, the system log and saved transcripts, at most 32 files. The
system log, transcripts, `events.log` and any sensor capture that is not a plain sensor CSV (GPS
columns, TEXT or TRACK captures) are sealed on send with the capture key; health files go only when
sealed at rest. Nothing else is offered (no login logs, command audit, QuickNotes, keys). Only a
named master whose firmware collects may read, by its key; no password travels. A byte counts as
delivered once one named master stored it.
```
espnowcatchup [json] [from <n>]   - The files offered, how far a master took each, the last request (admin)
espnowcatchupserve [on|off]       - Serve file catch-up to the named masters (default on) (admin)
```
Details: `docs/BACKCAPTURE_FILES.md`.

### Collector
A mesh master or backup master built with `ENABLE_ESPNOW_COLLECT` (the home node) pulls the files of
each device that names it onto its SD card under `/sd/collect/<mac12>/files/` (an `index.json` in
UTC and segment files named from ids), when the device's session comes back, every 15 minutes,
after it starts or a card is put in, and on `collectnow`. It pulls only with a writable card and a
set clock, one device at a time, and keeps live traffic first. What the other master already has
is skipped and shown as "at the other master"; bytes the device dropped before they were fetched are
shown as gaps with their reason.
```
collectstatus [json] [<device>] [from <n>]  - Each device: fetching, up to date, waiting and why; one device: its files (admin)
collectnow <device> [full]        - Pull a device now; full also fetches what the other master has (admin)
collect [on|off]                  - Pull on its own (default on); no argument shows why it is not pulling (admin)
collectrate [<5..100>]            - Frames per second the hub asks for (default 40) (admin)
collectquota [<64..65535>]        - Card space per device in MB (default 2048) (admin)
```

---

## Automations

Automations are scheduled or conditional command sequences stored on the device. They run locally - no internet required.

### Syntax
```
NAME: <name>
SCHEDULE: <time_or_interval>
IF <condition> THEN <command>; <command>
```

### Schedule formats
- `TIME=HH:MM` - run at a specific time daily
- `INTERVAL=Xs` / `Xm` / `Xh` - repeat every N seconds/minutes/hours
- `BOOT` - run once on startup
- `EVENT` - run when a system event occurs (see Event triggers below)

### Event triggers
Automations can fire the moment something happens on the device instead of
polling on a clock. An event trigger names an event kind (`on=`) and an
optional `match=` filter; the automation fires within about one main-loop pass
of the event (milliseconds, not the next interval tick).

```
automation add name=greet type=event on=peer_online match=BACKDOOR commands="espnowsend BACKDOOR hello" enabled=1
automation add name=lowbatt type=event on=battery_low commands="ledcolor red" enabled=1
```

On the web Automations page pick trigger type **On Event**, choose the kind,
and optionally set a match. Event triggers work as any of the up-to-4 triggers
on an automation, alongside time/interval/boot triggers, and the optional
"fire when" condition still gates the run.

`match` is a case-insensitive substring test against the event's subject
(who/what: peer name, sensor name, username, setting key), its detail
(MAC address, text preview, filename, value), OR its by-who identity (the
user/device that caused it). Empty or `*` fires on every event of that kind.
So `on=setting_changed match=hub` fires only when user `hub` changes a
setting, regardless of which setting it was.

| Kind | Fires when | subject / detail |
|---|---|---|
| `peer_online` / `peer_offline` | mesh peer heartbeat appears / times out (30s) | peer name / MAC |
| `peer_paired` | a peer is saved as paired (`espnowpair`, `espnowpairsecure`, or an accepted `espnowpairrequest`); pairing mode only discovers, it no longer auto-pairs | peer name / MAC |
| `text_rx` | ESP-NOW text message received | sender / text preview |
| `file_rx` | ESP-NOW file received (not the bond's own sync files: the partner's settings, manifest, schema and automation list) | sender / filename |
| `bond_online` / `bond_offline` | bonded peer session up / heartbeat timeout, or the peer answered that it has no bond with this device | peer name / MAC |
| `bond_reject` | unpaired device probed the bond channel (30s cooldown) | MAC / count |
| `espnow_on` / `espnow_off` | ESP-NOW radio started / stopped | - |
| `pair_window_open` / `pair_window_closed` | pairing mode opened / closed | seconds / - |
| `mesh_promoted` / `mesh_demoted` | backup-master failover / master returned | device name / reason |
| `remote_cmd_rx` | an authenticated remote command ran on THIS device (mesh or MQTT) | sender / command |
| `wifi_connected` / `wifi_disconnected` | WiFi got IP / dropped | IP / - |
| `wifi_connect_failed` | all connect attempts to a network failed | SSID / attempts |
| `wifi_net_added` / `wifi_net_removed` | saved WiFi network added / removed | SSID |
| `mqtt_connected` / `mqtt_disconnected` | broker link up / lost (once per transition) | broker / seconds up |
| `ble_connected` / `ble_disconnected` | companion BLE device connected / dropped | device type / MAC |
| `g2_connected` / `g2_disconnected` | glasses link up / temple dropped | sides / side |
| `g2_plugin_alive` / `g2_plugin_silent` | the temple's app plugin answering / not answering renderer heartbeats (app idle or set down; not a wear sensor). RIGHT temple only: on stock firmware LEFT is silent by design (it sends only mic audio and acks) | side |
| `ring_connected` / `ring_disconnected` | R1 ring GATT up (after setup) / link lost | name / MAC |
| `ring_worn` / `ring_not_worn` | R1 on / off finger (wearStatus edge) | name |
| `time_synced` | clock first became valid (NTP or RTC) | ntp\|rtc / time |
| `login_ok` / `login_fail` | login on any transport (web, serial, OLED, BLE, MQTT, ESP-NOW) | username / transport |
| `usb_on` / `usb_off` | USB power plugged / unplugged (30s debounce) | - |
| `battery_low` / `battery_critical` | battery crossed threshold (30s debounce) | percent |
| `charging_started` / `charging_stopped` | actually charging vs merely USB-powered | percent |
| `power_save_enter` / `power_save_exit` | display power-save engaged / woke | idle minutes / - |
| `sd_mounted` / `sd_unmounted` | SD card mounted / unmounted | free MB / - |
| `sd_write_failed` | SD went unwritable (once per episode) | hint |
| `fs_low_space` | flash below the log reserve (once per boot); shown in the notification centre and as a toast | free bytes |
| `heap_low` | internal RAM free under 25 KB or its largest block under 16 KB (once per low spell, at most once a minute); shown in the notification centre, and as a toast at most once in 10 minutes | free bytes / `largest <bytes>` |
| `setting_changed` | a setting was saved | key / value |
| `settings_save_failed` | settings.json write failed | stage / file |
| `sensor_started` / `sensor_stopped` | sensor came up / stopped (includes Camera) | sensor name |
| `sensor_start_failed` | sensor failed to start | sensor name |
| `sensor_fault` | sensor auto-disabled after repeated I2C errors | sensor name / errors |
| `sensor_unresponsive` / `sensor_recovered` | a sensor that answered since boot stopped responding / answered again (once per outage) | sensor name / address, bus, nack\|timeout |
| `presence_detected` / `presence_cleared` | IR presence trip / cleared (held ~2s) | value |
| `gesture` | APDS swipe detected | UP\|DOWN\|LEFT\|RIGHT |
| `imu_shake` / `imu_tap` / `imu_freefall` | device shaken / knocked / dropped | intensity |
| `imu_orientation` | stable orientation change (3-frame debounce) | new / previous |
| `gps_fix` / `gps_lost` | fix acquired / lost (lost held 10s) | sats / lat,lon |
| `button` | gamepad or encoder button PRESS (releases not posted) | button name |
| `fm_rds_station` | RDS station name identified | name / frequency |
| `ei_detected` / `ei_lost` | camera-AI object confirmed (3 frames) / gone (2s) | label / confidence |
| `photo_saved` / `video_saved` / `mic_saved` | media file finished writing | filename |
| `llm_gen_done` / `llm_model_loaded` | generation finished / model ready | reason\|model / stats |
| `file_deleted` | a file was deleted | filename / path |
| `voice_wake` / `voice_command` | wake word / successful voice command | - / command |
| `user_request` / `user_added` / `user_approved` / `user_deleted` | account lifecycle | username |
| `password_changed` | password rotated | username / self\|admin-reset |
| `g2_long_press` / `g2_long_release` | ring/temple hold pressed / released on a HardwareOne lens page | ring\|left\|right\|unknown / press: consumer, `ignored:<reason>` or `refused:<reason>`; release: `<ms>ms` |
| `note_started` / `note_saved` / `note_failed` | a QuickNote began / was saved / was not saved (never the note text) | hold\|lens\|cli\|web (failed: reason) / started: local\|pi; saved: `<words>w <ms>ms` |

Run `events` in the CLI to watch the register live (last 48 events, newest
first, each tagged with who caused it, e.g. `by web:hub`) - handy for finding
the exact subject text to match on. Events are also the notification
pipeline: kinds like WiFi, battery, login, and mesh changes render as OLED
banners, web toasts, and entries in the OLED notification center
automatically.

Security note: mesh peer names come from peer metadata, which is not
authenticated - a device in radio range that knows your mesh setup can
influence names and presence. If that matters for a rule, match on the MAC
address (the event detail) instead of the name, and prefer `bond_*` events
(authenticated + encrypted channel) for anything sensitive.

### Conditions
An automation can carry an optional "fire when" condition that gates whether it runs, and command lists can branch with `IF <expr> THEN <command> [ELSE <command>]`. Each condition is a single `<variable> <operator> <value>` test.
```
IF TEMP>75 THEN ledcolor red
IF BATTERY<20 THEN ledcolor red
IF HEAP<40 THEN PRINT low memory
IF WIFI=CONNECTED THEN PRINT online
IF DAY=SAT THEN ledbrightness 30
IF HOUR>=22 THEN oleddim
IF TAGS CONTAINS outdoor THEN PRINT Outdoor device
```

Numeric variables compare with `>`, `<`, `=`, `>=`, `<=`, `!=`. String/enum variables compare with `=`, `!=`, `CONTAINS`. Values are case-insensitive.

| Category | Variables | Type | Notes |
|---|---|---|---|
| Sensors | `TEMP` `DISTANCE` `LIGHT` | numeric | thermal C / ToF cm / ambient light |
| Sensors | `MOTION` | enum | `DETECTED` / `NONE` |
| Time | `TIME` | enum | `MORNING` `AFTERNOON` `EVENING` `NIGHT` |
| Time | `HOUR` | numeric | local hour, 0-23 |
| Time | `DAY` | enum | `SUN` `MON` `TUE` `WED` `THU` `FRI` `SAT` |
| Time | `NTP` | enum | `SYNCED` / `NONE` (clock holds a real date) |
| System | `BATTERY` | numeric | charge estimate; ADC values are approximate, unavailable/stale readings do not satisfy the condition |
| System | `HEAP` `PSRAM` `FSFREE` | numeric | free internal DRAM / PSRAM / storage, in KB |
| System | `UPTIME` | numeric | minutes since boot |
| System | `CHIPTEMP` | numeric | SoC temperature, C; the condition is false when the chip has no reading |
| Network | `WIFI` `BLE` | enum | `CONNECTED` / `NONE` |
| Network | `RSSI` | numeric | WiFi signal, dBm (negative; only while connected) |
| Network | `PEERS` | numeric | live ESP-NOW mesh peers (heartbeat within 30s) |
| Location | `GPS` | enum | `FIX` / `NOFIX` |
| Location | `SPEED` `SATS` | numeric | GPS ground speed (knots) / satellites in view |
| Location | `WP_DIST:<name>` | numeric | meters from current GPS fix to a named map waypoint (case-insensitive name; fail-closed if no fix, maps off, or waypoint missing) |
| AI | `LLM` | enum | `READY` / `BUSY` / `NONE` |
| Mesh/Bond | `ESPNOW` | enum | `ACTIVE` / `NONE` (radio stack up) |
| Mesh/Bond | `BOND_MODE` `BOND_PAIRED` `BOND_ONLINE` `BOND_SYNCED` | enum | bond config + link state |
| Mesh/Bond | `BOND_ROLE` | enum | `MASTER` / `WORKER` |
| Mesh/Bond | `PAIRMODE` | enum | `ACTIVE` / `NONE` (WPS pairing window open) |
| Mesh/Bond | `BOND_RSSI` | numeric | bond link RSSI, dBm (only while online) |
| Mesh/Bond | `BOND_PEER_HEAP` `BOND_PEER_UPTIME` | numeric | bonded peer free internal (byte-addressable) RAM (KB) / uptime (min) |
| Mesh/Bond | `PAIRMODE_SECS` `PEERSKNOWN` `STALESTPEERAGE` | numeric | pairing seconds left / known peers / oldest peer heartbeat age (s) |
| ESP-NOW | `ROOM` `ZONE` `TAGS` | string | THIS device's metadata (`gSettings.espnowRoom/Zone/Tags`); `NONE` if unset |

A variable whose subsystem is disabled or absent evaluates to false, so the condition simply never matches.

Example - leave-home radio power save (create a map waypoint named `Home` first; keep GPS on and that map loaded):

```
IF WP_DIST:Home > 200 THEN closewifi
```

Pair with an Interval trigger (e.g. every 60s) as the Fire when gate, and a companion automation (or ELSE IF) with a lower threshold such as `WP_DIST:Home < 150` that runs `openwifi` / `openespnow` so the radios come back near home without chattering at the boundary.

### PRINT command
```
PRINT <message>
```
Sends a message to all output channels (serial, web, OLED).

### CLI commands
```
automation list              - List all automations
automation add <json>        - Add a new automation
automation delete <name>     - Delete automation by name
automation run <name>        - Run automation immediately
automation enable <name>     - Enable automation
automation disable <name>    - Disable automation
events                       - Show recent system events (for event triggers)
```

---

## MQTT

Requires `ENABLE_MQTT=1`. Connects to a broker (e.g., Home Assistant Mosquitto) and publishes sensor data and device state.

Configure via the **MQTT** tab in the web UI, or via CLI (single words; there is
no `mqtt` command with subcommands - the full list is in the
[mqtt command section](#command-reference) below):
```
mqtthost [hostname]          - Set broker host/IP
mqttport [port]              - Set broker port (default 1883)
mqttuser [user|clear]        - Set MQTT username
mqttpassword [pass|clear]    - Set MQTT password
mqttbasetopic [topic|auto]   - Set topic prefix
openmqtt                     - Connect to broker
closemqtt                    - Disconnect
mqttstatus                   - Show connection status
```

### What is published

The device publishes one JSON state document to `<base>/state` every `mqttpublishintervalms`, and
Home Assistant discovery configs under the discovery prefix when it connects. The state document has
`timestamp` (uptime in ms), `time` (UTC, `YYYY-MM-DDTHH:MM:SSZ`, only while the clock is set),
`deviceName` (the name in use; the fallback `HardwareOne-B2C3` when none is stored) and
`deviceNameSet`, then one section per reading switched on with its `mqttpublish*` setting: `system`
(`uptime`, `heap_free`, `heap_min`, and `psram_free`/`psram_total` on boards with PSRAM), `wifi`,
and the sensor sections `thermal` (min/avg/max), `tof`, `imu`, `presence`, `gps`, `apds`, `rtc` and
`input`, each written by that sensor's own reading builder. `rtc.datetime` is the RTC's reading in UTC with `Z`.
A section that cannot be read as JSON is left out of that publish (with a warning when that
changes); a document that does not fit is not sent.

Home Assistant shows a sensor as unknown while its section is missing or not valid, and GPS
position, altitude and speed also while there is no fix. Units: GPS speed in knots (`kn`, which
Home Assistant converts for display), gyro in rad/s, acceleration in m/s², ToF distance in mm (the
nearest target). The device names its board as model and its firmware as software version. A mesh
peer bridged by the master has neither, shows Last Seen, and appears as `peer-B2C3` when it has no
name.
Entities an earlier build declared and this one does not (Presence Object Temp, a peer's Uptime and
thermal entities, and the gamepad entities on encoder and touch builds) are removed from Home
Assistant on connect.

---

## Raspberry Pi Co-Processor (CM5)

A Raspberry Pi (Compute Module 5 or Pi 5) can be wired to the ESP32 over a dedicated UART and act as a co-processor. The ESP32 remains the device - same command system, same users and roles, same mesh - and hands the Pi the jobs a microcontroller does badly: speech-to-text, a real LLM, and its own power and thermal management.

The firmware half is this repository. The Pi half is a single Linux daemon, `hw1-ai-service`, that speaks the link, runs the STT and LLM engines, and bridges power/fan control. It lives in its own repository, with its own install guide, architecture notes and deployment paths:

> **https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor**

### What the pair can do

- **Ask the Pi** - the Pi's models appear as an LLM source with a `cm5:` prefix (`llmmodels`, `llmload cm5:<name>`) on the web LLM page, the OLED LLM mode and the lens; answers stream back to whichever surface asked. See [LLM](#llm).
- **"Hey Even" without a phone** - with G2 glasses paired, the wake word opens a native voice session: the device records, streams the audio to the Pi, the Pi transcribes and answers, and the reply lands on the lens. The firmware only accepts a wake while the Pi is logged in over the link *and* its heartbeat says `ready`; otherwise the glasses are told to fall back to their own timeout instead of showing a listening card nobody will answer. `g2evenai status` shows the current exchange and why a wake was declined.
- **Dictation** - speak into any text field. On the OLED, cycle keyboard modes with SELECT until `MIC` (A records, Y deletes, X accepts). On the G2 lens, tap the keyboard's **Mic / Keys** row (see [Text entry](#text-entry)). Speak, the Pi transcribes, and the text lands in the field you were typing in. Each surface drains only its own transcript, so an OLED login or logout can no longer cancel a glasses-owned dictation. Only offered while the microphone, the link and a logged-in Pi are all present - and the daemon must additionally announce `dictate hostready v1` after each login, so a Pi that cannot transcribe is refused up front (`host not present` / `host stale` / `host not ready`) instead of arming a capture nobody will answer. Captures are capped at 30 seconds; the firmware waits up to 90 seconds for the transcript, which covers a cold model load on the Pi. Dictation is compiled in whenever the build has a microphone *and* at least one keyboard surface - an OLED panel **or** G2 glasses - so a glasses-only build (no panel) still gets it.
- **Power and fan** - `cm5 power` (profile, reboot, halt, suspend, timed sleep) and `cm5 fan` (quiet / auto / max with temperature, PWM, RPM and health readback). Every request is a confirmed request/ACK exchange reconciled against the Pi's boot-id, so a lost ACK is completed, never re-executed; an ambiguous outcome fails closed until `cm5 power recover confirm`. Destructive verbs are super-admin and need `confirm` on the same line.
- **Presence** - the daemon sends `cm5 heartbeat` every 5 s (`starting` / `ready` / `busy` / `degraded`); the firmware holds a 15 s lease (75 s while the Pi is busy). `cm5 status` shows the lease, `cm5 capabilities` the protocol version. A stale lease means voice sessions and remote LLM calls are declined rather than left hanging.
- **Clock** - a Pi that knows the time can set the device clock over the link (source `cm5` in clock status). A dark boot adopts it; an already-synced clock is only corrected when the Pi is NTP-synced, the current source is neither manual nor NTP, and the drift is over two minutes.
- **Bulk audio** - `voicefetch "<path>"` streams a finished recording to the Pi as framed binary with a whole-file CRC; `liveaudio` is an opt-in real-time PCM transport for the S3 boards.
- **Link diagnostics** - `cm5 linkhealth [json]` prints the *Pi daemon's own* UART fault tally (garbage, corrupt and stray frames, timeouts, logins, resets, tx/rx line counts, host uptime). The daemon pushes it over the authenticated link, so the numbers that name a link fault are readable from this device's CLI even when the Pi has no network. Counters are parsed by key, so a counter the daemon adds later reaches an existing build without a reflash; a malformed report is rejected whole, because damage is exactly what these counters measure. Diagnostics only - nothing in the firmware reads the stored values back.

### Wiring

The link is a plain 3.3 V UART, separate from the USB console. Pi side: GPIO4 (TXD2) → ESP32 RX, GPIO5 (RXD2) ← ESP32 TX, common ground. Enable the port with `dtoverlay=uart2-pi5` in `/boot/firmware/config.txt` (that is the Pi 5-family overlay name; plain `uart2` is the Pi 4 overlay and does nothing on a CM5); it appears as `/dev/ttyAMA2` and never carries the Linux console or Bluetooth.

| Board | Port | ESP32 TX | ESP32 RX | Default baud | Max baud |
| ----- | ---- | :------: | :------: | -----------: | -------: |
| Seeed XIAO ESP32-S3 (Sense or plain) | `Serial0` | GPIO43 (D6) | GPIO44 (D7) | 2,000,000 | 3,000,000 |
| Unexpected Maker FeatherS3 | `Serial0` | GPIO43 | GPIO44 | 921,600 | 3,000,000 |
| Adafruit Feather ESP32 V2 | `Serial1` | GPIO8 | GPIO7 | 230,400 | 460,800 |
| Adafruit QT Py ESP32 | `Serial1` | GPIO32 | GPIO7 | 230,400 | 230,400 |
| Generic ESP32 | `Serial2` | GPIO32 | GPIO33 | 230,400 | 230,400 |

The pins and ceilings live in each board's block in `System_BuildConfig.h` (`UART_LINK_PORT`, `UART_LINK_TX_PIN`, `UART_LINK_RX_PIN`, `UART_LINK_BAUD_DEFAULT`, `UART_LINK_BAUD_MAX`). Live PCM streaming needs at least 921,600 baud, so it is an ESP32-S3 feature; everything else (commands, LLM answers, dictation text, power/fan) works at 230,400. On the classic ESP32 the UltraSaver power profile keeps the CPU clock up while the link runs above 250 kbaud so the UART does not lose bytes.

### Setting it up

On the device:

1. Create an account for the daemon - it logs in over the link like any other user. The co-processor README currently documents `useradd cm5svc <password> 0 admin`; use the role its release asks for (it is the account the Pi will hold, so keep the password in the daemon's credentials file only).
2. Turn the link on: `uartlink on` (persisted as `uartLinkEnabled`; it is off by default). Optionally `uartlinkbaud <n>` (0 = the board default above). `uartrequireauth` is on by default - leave it on.
3. Check it: `uartlink` prints the link state, the session epoch, the last session event and the CM5 lease (`cm5=… cm5_fresh=… cm5_age_ms=…`).

On the Pi: follow the install steps in the co-processor repository (overlay, Python venv, `~/.config/hw1-ai-service/credentials`, `config.yaml`, llama.cpp), then run its `probe` command - it connects, logs in, and prints status. Once the daemon is up, `cm5 status` on the device should show a fresh `ready` lease.

Notes:

- The Pi's session is an ordinary named session with its own login epoch; a re-login, `uartrequireauth` flip, or idle timeout fences any in-flight transfer at the next frame boundary. Sign in with the bare `login <user> <password>` (quote a password that contains spaces - unquoted spaces are refused); the targeted `login … serial|display` forms only work once the link session itself is signed in.
- Heartbeats, time pushes, `liveaudio ready` renewals and the power/fan ACK callbacks are handled on the link's control plane - they never enter the command queue, never appear in command history, and never extend a CLI session.
- The daemon cannot open or answer interactive prompts (help, confirm, the setup wizard); those belong to a human session.

### Commands

```
uartlink [status|on|off]            - UART host link on/off; status shows session + CM5 lease
uartlinkbaud <0|9600-max>           - baud (0 = board default); persisted
uartrequireauth <0|1>               - require login on the link (super admin; default 1)
cm5 [status|capabilities]           - presence lease / protocol (cm5-presence-v1)
cm5 linkhealth [json]               - the Pi daemon's UART fault tally (diagnostics only)
cm5 power [show|status|profile <eco|balanced|performance|auto>]
cm5 power reboot|halt|suspend confirm
cm5 power sleep_for <1..1440> confirm
cm5 power recover confirm           - clear a fail-closed uncertain transition
cm5 fan [show|status|quiet|auto|max]
llmmodels / llmload cm5:<name>      - the Pi's models carry a cm5: prefix (see LLM)
dictate status|result|fail          - UART-only; not a CLI command. See below
voicefetch "<path>"                 - stream a recording to the Pi (paths under /recordings)
liveaudio status|capabilities       - live PCM transport (S3 boards, >= 921600 baud)
g2evenai status|capabilities        - native "Hey Even" exchange and host-gate state
```

> `dictate` is **not** in the command registry - it does not appear in `help`,
> and typing it at a serial or web CLI gets you nothing. The verb is claimed
> directly on the authenticated UART control plane, so only a logged-in Pi
> session can use it. Daemons keep sending the same `dictate result` / `dictate
> fail` lines, but must now send `dictate hostready v1` after each login before
> the firmware will arm a capture.

---

## LLM

Requires `ENABLE_LLM_BACKEND=1` plus at least one answer source: `ENABLE_LLM_SOURCE_ONBOARD` (a tiny transformer running on this chip; needs PSRAM) and/or `ENABLE_LLM_SOURCE_CM5` (answers from the [Raspberry Pi co-processor](#raspberry-pi-co-processor-cm5) over the UART link). Every surface - web page, OLED mode, lens viewer, `llm*` commands - talks to whichever source holds the current model; models are addressed by `<source>:<name>` ids such as `onboard:model.bin` or `cm5:Qwen3-1.7B-Q4_0.gguf` (a bare filename still resolves).

### Model files (on-board source)

Place LLM1-format model files (produced by `esp32-llm-converter`) in one of two locations:

- **LittleFS (internal flash):** `/system/llm/model.bin` - default path, loaded automatically
- **SD card:** `/sd/llm/<filename>.bin` - useful for swapping models without reflashing

`llmload` with no arguments loads the configured default model (`llmDefaultModel`, initially `/system/llm/model.bin`). Use `llmmodels` to list every available model across LittleFS, SD and the Pi; `llmmodels json` returns them as objects with `id`, `backend`, `storage` and `available`.

### Memory (on-board source)

Models and KV cache are allocated in PSRAM. The firmware automatically reduces the context window to fit available PSRAM (auto-fit). A 400 KB reserve is kept free for the rest of the system. Run `llmstatus` to see current PSRAM usage after loading.

### Web UI

The **LLM** tab provides a chat interface. Select a model from the dropdown (Pi models are tagged `[cm5]`, SD-card models `[SD]`; unavailable ones are listed but disabled), click **Load**, then type a prompt and click **Ask**. A **Do:** button appears only when the loaded model declares command-mode support. Adjustable settings: temperature, sentence limit, and repetition penalty. The page shows a measured load bar while a Pi model loads and reports `[connection lost]` if the device stops answering.

### On the lens

**Apps -> LLM** opens a submenu rather than dropping straight into the viewer:

| Row | Does |
| --- | ---- |
| **Open chat** | The read-only streaming answer viewer |
| **Ask (Mic / Keys)** | New in v0.99.92 - a free-text or spoken turn, typed on the shared [arrow-pad keyboard](#text-entry) or dictated from its Mic row |
| **Ask (guided)** | The drill-down prompt picker. Hidden when the loaded model publishes no menu |
| **Re-run last** | Retry the previous turn |
| **Select Model** | Unload, or pick a model; the row label shows the loaded model's filename |

**Re-run last** follows the surface the question came from: a keyboard or mic
turn is retried through the chat path, and only a guided turn is re-asked as a
plain guided turn.

### Generation modes

The model supports two generation modes triggered by how the prompt ends:

- **Normal mode** - generates a natural-language response, stopping after the configured sentence limit.
- **Do: mode** - when the prompt ends with the `Do:` token, the model outputs a CLI command instead of prose. Used internally by the automation and web UI to translate natural-language requests into executable commands. Only an on-board model whose header declares command-mode capability gets this mode; other models (and the Pi source) refuse `Do:` rather than guess.

### CLI commands

```
llmstatus                 - Show LLM state, active source/model, and (on-board) PSRAM usage
llmload [id|name|path]    - Load a model: onboard:model.bin, cm5:<name>, a bare filename, or a path
llmunload                 - Unload the model (frees PSRAM for the on-board source)
llmmodels [json]          - List models across LittleFS, SD and the Pi
llmgenerate <prompt>      - Generate text (synchronous; on a CM5-only build use the json form)
llmresult json            - Poll a streaming answer; returns a `next` cursor
llmstop                   - Stop in-progress generation
```

---

## Debug Flags

Debug output is controlled by named flags. Each flag can be enabled persistently (saved to flash) or temporarily (runtime only, cleared on reboot).

```
debug<flagname> 1            - Enable (persistent)
debug<flagname> 1 temp       - Enable (runtime only)
debug<flagname> 0            - Disable
```

Available debug modules (type `help debug` on device for full list):

| Command | Controls |
| ------- | -------- |
| `debughttp` | HTTP request/response logging |
| `debugwifi` | WiFi connection events |
| `debugespnowcore` | custom ESP-NOW v1 frame layer |
| `debugespnowmesh` | Mesh peer management |
| `debugespnowrouter` | Message routing |
| `debugespnowstream` | Stream output |
| `debugespnowmetadata` | Metadata REQ/RESP/PUSH pipeline |
| `debugmqtt` | MQTT publish/subscribe |
| `debugautomations` | Automation scheduling and execution |
| `debuggps` | GPS NMEA parsing and fix events |
| `debugrtc` | RTC reads/writes and drift |
| `debugimu` | IMU polling, calibration, motion events |
| `debugthermal` | Thermal sensor frames and events |
| `debugtof` | Time-of-flight ranging |
| `debugapds` | APDS gesture/proximity/colour |
| `debuginput` | Input device events (gamepad, encoder, wheel, touch) |
| `debugpresence` | Presence sensor activity |
| `debugstorage` | Filesystem read/write |
| `debugcli` | Command execution flow |
| `debugauth` | Authentication events |
| `debugperformance` | Timing and performance metrics |
| `debugsystem` | System events |
| `debugusers` | User management |
| `debugllm` | LLM general (parent flag) |
| `debugllmload` | Model load, header validation, weight mapping |
| `debugllmtokenizer` | Tokenizer BPE encode/decode |
| `debugllmforward` | Transformer forward pass (verbose - use sparingly) |
| `debugllmgenerate` | Generation loop, sampling, throughput |
| `debugllmmemory` | PSRAM estimates, context cap, allocations |
| `debugg2events` | G2 list/text/system events and ring/temple holds (`debugg2` includes them) |

---

## Command Reference

> **This section is a curated tour of the commands you reach for most - it is
> not the complete list.** For every command your build actually registers, see
> **[COMMAND_REFERENCE.md](COMMAND_REFERENCE.md)** - it is generated directly
> from the `CommandEntry` tables, so it reflects the build flags that were set
> when it was produced. Regenerate it for your own configuration with
> `python3 tools/command_registry.py reference`; the command and module counts
> move with your flags, which is why they are not repeated here.

Type `help` on the device to enter the interactive help system. Type a module
name to see its commands, or use `help <module>` directly. Longer module lists
are paged before they enter any serial, web, or other display buffer:
`help <module> p<N>` opens page *N* (`help espnow p2`, for example). While an
interactive module page is open, type `next`, `prev`, or `p<N>` to move between
its pages. `back` returns from a module page to the main help menu, and `exit`
leaves help. Type `help all` to include disconnected sensors.

Two things about lookup that explain most surprises:

- Matching is **case-insensitive**, so `MQTTHOST` and `mqtthost` are the same command. Every command is registered in lower case without separators (`mqtthost`, `httpautostart`); a family of sub-commands puts a space between its words (`phone pair new`, `ring panel feed`).
- Lookup is **longest-prefix**, so `automation list` resolves to the `automation` dispatcher with `list` as an argument rather than needing its own entry. This is why some features appear as one command with subcommands and others as a family of single words.

A command that cannot do what it was asked answers `Error: <reason>. <next step>.`, and the web
API, automations and the displays count it as failed. That includes stopping something that is
not running and starting something that is already running: `closehttp` with no server answers
`Error: HTTP server is not running. Start it with 'openhttp'.`, and `openhttp` while the server
runs answers `Error: HTTP server is already running. Stop it first with 'closehttp' to restart it.`

<details>
<summary><strong>core - System commands</strong></summary>

```
status                          - Show system status (WiFi, FS, memory)
versions                        - Firmware version and every version a separate part must match
uptime                          - Show device uptime
time                            - Show current time (uptime + NTP if synced)
timeset <YYYY-MM-DD HH:MM:SS>   - Set time manually (or unix timestamp)
reboot                          - Restart the device
ramflush                        - Reboot to reclaim RAM, restoring the features running right now
factoryreset [all]              - Wipe user accounts and reboot into the setup wizard (super admin);
                                  all: also erase every stored setting, credential and file (serial only)
deepsleep                       - Power off via deep sleep (no wake source - reset button to wake)
bootcount [reset|json]          - Boot count (NVS), crash count, last reset reason
clear                           - Clear CLI history
broadcast <message>             - Send message to all users (admin)
wait <ms>                       - Delay execution for N milliseconds
lightsleep [seconds]            - Enter ESP32 light sleep (default 20s)
temperature                     - Read ESP32 internal temperature
voltage                         - Read supply voltage
cpufreq                         - Get/set CPU frequency
memsample                       - Memory snapshot with component breakdown
memreport                       - Comprehensive memory report (Task Manager style)
perftop                         - Live perf snapshot: loop laps/s, per-section timing, worst stalls, per-task CPU%
taskstats                       - Detailed FreeRTOS task statistics
events                          - Recent system events (drives automation event triggers)
pendinglist                     - List pending user account requests
```
</details>

<details>
<summary><strong>wifi - Network management</strong></summary>

```
openwifi [ssid]                 - Connect to WiFi (optional SSID override)
closewifi                       - Disconnect from WiFi
wifistatus                      - Show current connection info
wifiscan                        - Scan for nearby access points
wifilist                        - List saved networks
wifiadd <ssid> <pass> [priority] [hidden]  - Add/save a WiFi network
wifirm <ssid>                   - Remove a saved network
wifipromote <ssid>              - Promote network to top priority
ntpsync                         - Sync time from NTP server
openhttp                        - Start HTTP/HTTPS web server
closehttp                       - Stop web server
httpstatus                      - Show web server status and IP
certinfo                        - Show HTTPS certificate details
certgen [rsa]                   - Generate self-signed certificate (default: ECDSA P-256)
```

With auto-reconnect on (`wifiautoreconnect`, the default), a lost connection is
first retried on the same network. If the device is still offline 60 seconds
later and two or more networks are saved, it tries all saved networks, as
`openwifi` does, and repeats that after 60 s, 120 s and then every 5 minutes
until one answers. So a device carried to another saved network comes back online by
itself. `wifidisconnect`, `closewifi` and `wifiautoreconnect 0` stop it. A device that
found no network at boot is not retried: run `openwifi`.
</details>

<details>
<summary><strong>espnow - ESP-NOW mesh</strong></summary>

```
openespnow                      - Initialize ESP-NOW
closeespnow                     - Deinitialize ESP-NOW and free resources
espnowstatus                    - Show ESP-NOW status and configuration
espnowstats                     - Show message/error counters (RX Length Drops: frames shorter than their type allows)
espnowlist                      - List all paired peers
espnowpair <mac> <name>         - Pair with a device
espnowunpair <name_or_mac>      - Remove a paired peer
espnowsend <name_or_mac> <msg>  - Send a text message (relayed if the peer is out of range)
espnowbroadcast <message>       - Broadcast to the whole mesh (flooded, up to the TTL)
espnowsendfile <name_or_mac> "<path>"      - Send a file to a peer
espnowbrowse <name_or_mac> <user> <pass> ["path"]  - Browse remote filesystem
espnowfetch <name_or_mac> <user> <pass> "<path>"   - Fetch a file from a peer
espnowremote <name_or_mac> <user> <pass> <cmd>   - Execute command on peer

--- Mesh ---
espnowmode [direct|mesh]        - Get/set routing mode
espnowmeshstatus                - Show mesh peer health (heartbeats, ACKs)
espnowmeshmetrics               - Show routing metrics (forwards, drops, route churn)
espnowmeshroutes                - Show the route table (who's reachable, and via whom)
espnowmeshttl [1-10]            - Hops a message may travel (1 = direct only)
espnowmeshrelay [0|1]           - Carry other nodes' traffic through this one
espnowmeshtopo                  - Discover mesh topology (master only)
espnowtimesync                  - Master: send this device's clock to the paired devices now
espnowtimestatus [json]         - Where this device's clock comes from; what was sent or last received
espnowmeshtime [on|off]         - Mesh time on/off (default on)

--- Device identity ---
devicename [name|clear]         - Get/set the device name (ESP-NOW, Bluetooth, hostname; 'clear' while ESP-NOW is stopped)
espnowroom [name]               - Get/set room
espnowzone [name]               - Get/set zone
espnowtags [tag1,tag2,...]      - Get/set tags
espnowfriendlyname [name]       - Get/set friendly display name
espnowstationary [0|1]          - Get/set stationary flag
espnowdeviceinfo                - Show all local device metadata
espnowdevices                   - List all mesh devices with metadata (master)
espnowrooms                     - List rooms and their devices (master)
espnowfind <query>              - Find devices by name, room, or tag
espnowroomcmd <room> <user> <pass> <cmd>  - Run command on the devices accepted into a room
espnowtagcmd <tag> <user> <pass> <cmd>    - Run command on the devices accepted for a tag
espnowgroups [json] [name_or_mac]         - Per peer: announced, accepted and pending room and tags
espnowgroupaccept [json] <name_or_mac|all> - Accept the room and tags a peer announces now
espnowgroupdrop [json] <name_or_mac|all>   - Take back what was accepted for a peer

--- What a master collects: readings, battery, events ---
espnowsensorstream <sensor> <on|off>   - Send one sensor's readings to the master named on this device
espnowsensorstatus [json [<name_or_mac> [via <name_or_mac>]]]  - What this device sends and why not; what it received, lost and refused
espnowbattery [json] [<name_or_mac>] [from <n>]  - Master: the battery state other devices sent
espnowevents [json] [since <rx>]       - Master: the system events other devices sent (admin)
espnowpowerinterval [0|10..3600]       - Seconds between this device's battery reports (0 = none, default 60)
espnoweventpush [off|alert|standard|verbose]  - Which of this device's events go to the master (default alert)
espnowmasterfingerprint [64 hex|clear] - Name the mesh master on this device (its key fingerprint) (admin)
espnowbackupfingerprint [64 hex|clear] - Name the backup master on this device (admin)
espnowforwardlinks [json] [from <n>]   - Master: who forwards for which bond partner

--- Security ---
espnowsetpassphrase <mesh> "<passphrase>"  - Set a mesh's encryption passphrase (double quotes when it has spaces)
espnowencstatus                 - Show encryption status and key fingerprint
espnowpairsecure <mac> <name>   - Pair with encryption

--- Metadata sync ---
espnowrequestmeta <name_or_mac> - Pull metadata from a peer
espnowusersync [on|off]         - Enable/disable credential sync across mesh
```

**Mesh time.** The mesh master sends its clock to each paired device when their encrypted session
starts, every 10 minutes and when its own clock is set. A device takes it only from the master
named on it (`espnowmasterfingerprint`, or the backup master named with `espnowbackupfingerprint`
while it acts as master), and only when it has no better source of its own (RTC chip, NTP,
`timeset`, CM5); `time` then shows the source "mesh". A device that names no master takes no mesh
time; `espnowtimestatus` says so. A master named in place of another (or a fingerprint cleared)
counts at once: the device no longer follows the master it followed and takes the newly named
one's next time. The master only sends a clock that comes from one of those four
sources. A time more than an hour away from
a clock that is already set is applied when the master says it again: within about 15 seconds when
the master's own clock was just set, else at its next 10-minute round. A device with mesh mode off
takes no mesh time. The clock is good to a few seconds. Times are kept in UTC; the timezone only
changes how they are shown.

**What a master collects.** The mesh master (and a backup master) is the receiving end: it keeps the
sensor readings, the battery state and the system events the other devices send it. A device sends
to the master that is named on it, so name the master on each sending device
(`espnowmasterfingerprint <fingerprint>`, the 64-character public key `espnowidentity` shows on the master;
`espnowbackupfingerprint` for a backup master) and pair the two devices. Anything but 64
hexadecimal characters is refused. `espnowmasterfingerprint clear` (and `espnowbackupfingerprint
clear`) names none again, and the reply says what that stops; with neither named the device takes
no mesh time, sends its readings, battery state and events to no device and refuses sensor
requests. On the web settings page and in the OLED settings editor, emptying the field does the
same. Naming the master is the
consent to send; nothing here needs a bond, and all of it works through a relay. The receiving
device must have the master or backup master role (`espnowmeshrole`): a worker keeps nothing that
is sent to it, counts what it refused and says so on its console. `espnowsensorstatus` shows on every device who it sends to, what was
sent and why not, and what it received; the event counts of the sending devices are shown to an
admin only (anyone else sees "Event counts: admin only.").

**Sensor readings.** `espnowsensorstream <sensor> on` sends that sensor's readings to the named
master, and the reply says where they go or why nothing is sent yet. The master lists a device's
sensors (present, streaming or not) as soon as that device's sensor status arrives, before the
first reading. A reading can be up to 536
bytes and travels in up to four frames. The receiving device stamps each reading with its own clock
when it arrives and counts lost readings per device and sensor
(`GET /api/sensors/remote?device=<MAC>&sensor=<type>`). A reading that cannot be sent is counted
with its reason, and the console says so once a minute. A master keeps the readings of up to
sixteen sending devices; a seventeenth is refused and shown as refused. A bond partner that a bond
master forwards for (`bondforward`, see Bonding) counts as one of them and is listed as
`porch via glasses`; `GET /api/sensors/remote?device=<MAC>&via=<MAC>` reads its readings.

**Battery state.** A device with a battery monitor reports its battery to the named master every
60 seconds and when its state changes (`espnowpowerinterval`). On the master, `espnowbattery` lists
the devices with percentage, voltage, status, USB power and how long ago the report arrived.

**Events.** A device sends its system events to the named master: by default only alerts (failed
logins, lockouts, crashes, a factory reset, critical battery); `espnoweventpush standard` adds the
everyday ones, `verbose` all, `off` none. On the master, `espnowevents` (admin only) lists the
newest 24 per mesh, each with the master's own arrival time. Long texts are cut to fit one frame; the full text
is in the sending device's event log. A burst is limited (eight at once, then one every two
seconds), and the next event says how many were left out.
</details>

<details>
<summary><strong>bond - Bonded mode (requires ENABLE_BONDED_MODE)</strong></summary>

```
bondconnect <mac_or_name>       - Connect to bonded peer
bonddisconnect                  - Disconnect bonded peer
bondstatus                      - Show bond mode status
bondforward [on|off|sensors <list>] - Forward the partner's data to the mesh master named here
bondrole [master|worker|auto]   - Show, choose (kept by bondconnect) or give up the bond role
bondshowcap                     - Show local capability summary
bondrequestcap                  - Request capability summary from peer
bondshowmanifest                - Show local manifest (UI apps + CLI commands)
bondrequestmanifest             - Request full manifest from peer
bondshowremotemanifest [fwHash] - Show cached remote manifest
bondstream <sensor> <on|off>    - Stream sensor data to bonded master (worker)
openstream                      - Start streaming all output to ESP-NOW caller (admin, remote only)
closestream                     - Stop streaming to ESP-NOW device (admin)
```
</details>

<details>
<summary><strong>mqtt - MQTT broker</strong></summary>

```
openmqtt                        - Start MQTT client
closemqtt                       - Stop MQTT client
mqttstatus                      - Show connection status
mqttautostart [0|1]             - Auto-connect on boot
mqtthost [hostname]             - Set broker host/IP
mqttport [port]                 - Set broker port (default 1883)
mqttuser [user|clear]           - Set username
mqttpassword [pass|clear]       - Set password
mqttbasetopic [topic|auto]      - Set base topic prefix
mqtttlsmode [0|1|2]             - TLS mode (0=off, 1=verify, 2=no-verify)
mqttcacertpath [path|clear]     - CA certificate path
mqttpublishintervalms [ms]      - Sensor publish interval
mqttdiscoveryprefix [prefix]    - Home Assistant discovery prefix
mqttpublishwifi [0|1]           - Publish WiFi state
mqttpublishsystem [0|1]         - Publish system state
mqttpublishthermal [0|1]        - Publish thermal data
mqttpublishtof [0|1]            - Publish ToF distance
mqttpublishimu [0|1]            - Publish IMU orientation
mqttpublishpresence [0|1]       - Publish presence/motion
mqttpublishgps [0|1]            - Publish GPS location
mqttpublishapds [0|1]           - Publish APDS color/proximity
mqttpublishrtc [0|1]            - Publish RTC time
mqttpublishinput [0|1]          - Publish input device state
mqttsubscribetopics [topics]    - Set external subscription topics
mqttexternalsensors             - List external sensor data received via MQTT
```
</details>

<details>
<summary><strong>bluetooth - BLE (requires ENABLE_BLUETOOTH)</strong></summary>

```
openble                         - Bluetooth on: bring the shared client stack up (devices with auto-reconnect come back)
closeble                        - Bluetooth off: close the glasses, ring, phone and the app console, then stop the host
openconsole                     - Start the app console server for a direct app connection (closes the devices)
closeconsole                    - Stop the app console server
blestatus                       - Show connection status
bleinfo                         - Show BLE configuration and settings
bletxpower [0-7]                - Get/set TX power
bleadv                          - Start advertising
bledisconnect                   - Disconnect current BLE client
blesend <message>               - Send message to BLE client
blestream <on|off|sensors|system>  - Control sensor/system data streaming
bleautostart [on|off]           - Auto-start BLE on boot
blerequireauth [on|off]         - Require authentication for BLE access
```
</details>

<details>
<summary><strong>filesystem - LittleFS file operations</strong></summary>

```
fsusage                         - Show filesystem usage
files ["path"]                  - List files (default '/')
mkdir "<path>"                  - Create directory
rmdir "<path>"                  - Remove directory
filecreate "<path>"             - Create empty file
fileview "<path>"               - View file contents
filedelete "<path>"             - Delete file
filerename "<oldpath>" "<newname>"  - Rename file

(Paths are always double-quoted, e.g. fileview "/logs/boot.txt".)
```
</details>

<details>
<summary><strong>sd - SD card</strong></summary>

```
sdmount                         - Mount SD card
sdunmount                       - Unmount SD card
sdformat confirm                - Format SD card as FAT32 (erases the card; without confirm it answers an error and formats nothing)
sdinfo                          - Show SD card information
sddiag                          - SD card hardware diagnostics
```

On the P4X-EYE a card is mounted on its own 1 to 2 s after it is put in; `sdunmount` keeps it
unmounted until `sdmount` or until it is taken out. `sdmount` right after putting a card in may
answer that the card is already mounted: the board mounted it.
</details>

<details>
<summary><strong>oled - Display control (requires OLED)</strong></summary>

```
openoled                        - Start OLED display
closeoled                       - Stop OLED display
oledstatus                      - Show OLED status
oledmode <mode>                 - Set display mode
oledtext <message>              - Set custom text overlay
oledanim <name>                 - Select animation, or: oledanim fps <1-60>
oledclear                       - Clear display
oledbrightness <0-255>          - Set brightness
oledupdateinterval <ms>         - Display update interval (10-1000ms)
oleddefaultmode <status|thermal|off>     - Mode after boot animation
oledbootduration <ms>           - Boot animation duration (500-10000ms)
oledenabled <0|1>               - Enable/disable OLED
oledthermalscale <1.0-10.0>     - Thermal image scale on OLED
setgamepadpassword              - Set gamepad joystick unlock pattern
```
</details>

<details>
<summary><strong>led - NeoPixel & startup effects</strong></summary>

```
ledcolor <color>                - Set NeoPixel color (name or hex)
ledcolor off                    - Turn off NeoPixel
ledclear                        - Turn off NeoPixel
ledeffect <effect>              - Run a NeoPixel effect
ledbrightness <0-100>           - Set LED brightness
ledstartupenabled [0|1]         - Enable/disable startup effect
ledstartupeffect <none|rainbow|pulse|fade|blink|strobe>  - Set startup effect
ledstartupcolor <color>         - Set startup primary color (an unknown name is refused)
ledstartupcolor2 <color>        - Set startup secondary color (an unknown name is refused)
ledstartupduration <ms>         - Startup effect duration
```
</details>

<details>
<summary><strong>i2c - I2C bus management</strong></summary>

```
i2cscan                         - Scan bus and list detected device addresses
i2creset                        - Reset I2C bus (pause polling, recover, resume)
i2cpause                        - Pause all I2C sensor polling
i2cresume                       - Resume I2C sensor polling
i2crecover <address>            - Clear degraded state for a specific device
i2cmetrics                      - Show I2C bus performance metrics
i2cstats                        - Show bus error statistics
i2chealth                       - Show per-device health status
i2cbusenabled <0|1>             - Enable/disable I2C bus (reboot required)
i2csdapin <pin>                 - Set SDA pin (0-39)
i2csclpin <pin>                 - Set SCL pin (0-39)
sensors [filter]                - List I2C sensors
sensorinfo <name>               - Show sensor details
sensorautostart [sensor] [on|off]  - Configure auto-start for a sensor
devices                         - Show discovered I2C device registry
discover                        - Re-scan and register I2C devices
```

A sensor whose `<x>enabled` is off cannot be opened: `open<x>` answers
`Error: <name> is disabled - run '<x>enabled 1' first`. A sensor that switches itself off after
repeated I2C errors is started again by itself while its `<x>enabled` and `<x>autostart` are on
(after 30 s, 2 min, 10 min, then every 30 min). `close<x>` ends that. The commands that save an
autostart or enabled setting (`<x>autostart`, `<x>enabled`, `micautostart`, `oledenabled`,
`ledstartupenabled`, `eienable`, `sensorlog autostart`) are admin-only.
</details>

<details>
<summary><strong>automation - Scheduled & conditional commands</strong></summary>

```
automation                      - Show automation system status
automationlist                  - List all automations
automation add <args>           - Add an automation
automation run id=<id>          - Run automation immediately by ID
automation add type=event on=<kind> [match=<text>] - Event-triggered automation
autolog start <file>            - Start automation execution log
autolog stop                    - Stop automation log
autolog status                  - Show log status
validateconditions <expr>       - Validate conditional syntax (e.g. IF temp>75 THEN ledcolor red)
print <message>                 - Broadcast a message to all output channels
```
</details>

<details>
<summary><strong>settings - Device configuration</strong></summary>

```
wifiautoreconnect <0|1>         - WiFi auto-reconnect on disconnect
ntpserver <hostname>            - Set NTP server
tzoffsetminutes <-720..720>     - Set timezone offset in minutes
httpautostart <0|1>             - Auto-start web server on boot
httpsenabled <0|1>              - Enable HTTPS (reboot required)
webclihistorysize <1-100>       - Web CLI history buffer size
oledclihistorysize <10-100>     - OLED CLI history buffer size
beginwrite                      - Coalesce this source's settings-file writes
savesettings                    - Flush changed files and end write coalescing
features                        - Show system features: built in, running, start at boot, allowed
featuresetup                    - Run interactive feature configuration wizard
```
</details>

<details>
<summary><strong>users - User management (admin)</strong></summary>

Roles form four tiers, lowest to highest: **guest** (authenticated but
view-only) -> **user** -> **admin** -> **superadmin**. A handful of commands are
marked super-admin-only and an ordinary admin cannot run them; granting or
removing `superadmin` requires a super-admin caller.

> **The `[0|1]` argument is not the admin flag.** On `useradd` and
> `userresetpassword` it means *require a new password on the next login*
> (default `0`). Role is a separate, later argument on `useradd`, and is set
> afterwards with `userpromote` / `userdemote`.

You cannot grant a role above your own. On `useradd` the two optional tokens
(`[0|1]` and `[role]`) may be given in either order.

```
userlist                        - List all users
useradd <user> <pass> [0|1] [role]  - Create user
                                  [0|1]: 1 = force password change at next login (default 0)
                                  [role]: guest|user|admin|superadmin (default user)
userdelete <user>               - Delete user (also disables the automations that user created)
userchangepassword <cur> <new> <confirm>  - Change own password
userresetpassword <user> <pass> [0|1]     - Reset another user's password (admin)
                                            1 = force password change at next login
userpromote <user> [role]       - Raise role: user|admin|superadmin (default admin)
userdemote <user> [role]        - Lower role: admin|user|guest (default user)
userrequest <user> <pass>       - Request a new account (self-registration)
userapprove <user>              - Approve a pending account request (admin)
userdeny <user>                 - Deny a pending account request (admin)
pendinglist                     - List pending account requests
usersync <user> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>
                                - Sync a user to an ESP-NOW peer (admin). Async: OK means
                                  delivered, not created - verify on the target's userlist
sessionlist                     - List active sessions
sessionrevoke <sid|user> [reason]  - Revoke a session
serialrequireauth [on|off]      - Require login on serial interface
ban <ip> [reason]               - Permanently ban an IP address (admin)
unban <ip>                      - Remove an IP ban (admin)
banlist                         - List all banned IPs
banuser <user> [reason]         - Ban a user account (admin)
unbanuser <user>                - Remove a user account ban (admin)
login <user> <pass>             - Log in
logout                          - Log out
whoami                          - Show the user on the submitting interface
login <user> <pass> <serial|uart|display>
                                - Log another local interface in (named non-Guest
                                  Serial/UART/OLED session required)
logout <serial|uart|display>    - Log another local interface out (same requirement)
```
</details>

<details>
<summary><strong>debug - Debug flags</strong></summary>

```
debug<flagname> 1               - Enable flag (persistent, saved to flash)
debug<flagname> 1 temp          - Enable flag (runtime only, cleared on reboot)
debug<flagname> 0               - Disable flag
```
See [Debug Flags](#debug-flags) section for the full flag list.
</details>

<details>
<summary><strong>sensorlog - Sensor data logging</strong></summary>

One device-wide logger writes a single file at a chosen interval. Sensors are
selected with a bitmask (`sensorlog sensors`), not started one-at-a-time.

```
sensorlog start <filepath> [interval_ms]  - Begin logging (path required; default interval 5000 ms)
sensorlog stop                            - Stop logging
sensorlog status                          - Show path, interval, format, selected sensors
sensorlog status json                     - The same status as one JSON object
sensorlog format <text|csv|track>         - text/csv for multi-sensor; track = GPS-only
sensorlog maxsize <bytes>                 - Max file size before rotation
sensorlog rotations <count>               - Old logs to keep (0-9)
sensorlog sensors <list|all|none>         - Replace mask: thermal,tof,imu,gamepad,apds,gps,presence,r1
sensorlog interval <ms>                   - Poll interval (100-3600000)
sensorlog autostart [on|off]              - Auto-start on boot with last-used path/mask
```

CSV rows always carry every selected column: a selected sensor without a reading leaves its
fields empty, so the later columns stay under their header names.

**Old capture data is removed before a volume fills.** Below a low-water mark (20 % of the volume,
at most 64 MB, at least twice the 100 KB log reserve) the oldest day's sensor-log files go first,
one day an hour, then old transcripts, and last health files older than the 14 newest stored days,
until 5 % (at most 16 MB) above the mark is free. Today's folder, the open session, the three newest
days, the eight newest transcripts, system logs, GPS tracks, notes and everything under `/system`
are never removed. The internal flash is left alone while a writable SD card takes the overflow.
Each pass that removed something writes one line to the system log. `sensorlog status` shows how
many rows had to wait for the filesystem.

**System logs** (`/system/sys_logs`): most logs are two files, `<name>.log.1` (older) and
`<name>.log`; the web Logging viewer shows them as one. Together they use at most a quarter of the
internal flash data partition. Error, event, I2C and command-audit lines are written in batches
(within 2 s); a power loss or crash can lose up to 2 s of them.

**R1 health logging (preferred):** `healthlogging on` (or the **Health Logging** row on
Apps -> Health, the OLED R1 Health page, or the **Health logging** button on Web
R1 Health) enables R1 in the sensorlog mask, starts capture under
`/logging_captures/sensors/`, and persists so boot resumes. The exact path is
shaped per session: a dated per-day file (`YYYY-MM-DD/health-YYYY-MM-DD.csv`)
once the clock is set, or a `boot-NNNNNN/` subfolder while it is still dark -
those are retro-dated automatically when real time arrives.
While health logging is on, the ring is polled/mined on a timer (default **900 s /
15 min**) and **only those mines write rows** (plus Poll Now / opening Health). There
are no 5 s empty timestamp heartbeats in R1-only sessions. Adjust with
`healthlogging interval <sec>` or setting `healthLoggingPollIntervalSec` (60-86400).
`healthlogging off` removes R1; stops logging if nothing else is selected.
Settings: `logging.sensorlog` -> **Health logging** / **Health logging poll interval (sec)**.

**Surfaces** (requires `ENABLE_R1_HEALTH`): G2 Apps -> Health (graphs), OLED **R1 Health**,
Web **`/r1-health`**. Ring connect remains under Bluetooth (OLED / Web). Snapshot CLI
for Web and a future Bluetooth App: `ring health` / `ring health json` / `ring poll`.
Stitch split captures: `healthlogmerge "<out>" "<in1>" "<in2>" ...` (same idea as `gpstrackmerge`).
**Output goes first and is truncated** - passing it last overwrites a real capture. The merge is a
byte concatenation: it does not reorder rows by time, does not drop the header lines of later CSV
inputs, and does not check that the inputs share a format or sensor mask. Per-day append means
same-day sessions already land in one file, so stitching is only for spanning days.

**Keep the ring up:** `bleautoreconnect r1-ring on` reconnects at boot **and** reseeks after
unexpected drops (backoff). While health logging is on, each due mine (default 15 min)
also nudges a ring reseek if the link is down (one non-blocking connect attempt;
not a continuous scan). `closering` / `closeg2` do not reseek.

You can still use raw `sensorlog sensors r1` + `sensorlog start ...` if you want
manual control without the `healthlogging` product switch (that path still uses the
normal sensorlog interval / heartbeats).
</details>

<details>
<summary><strong>llm - LLM inference, on-chip or via the CM5 co-processor (requires ENABLE_LLM_BACKEND)</strong></summary>

```
llmstatus                       - Show LLM state, model config, PSRAM usage, last generation stats
llmload [model.bin]             - Load model from LittleFS or SD (default: /system/llm/model.bin)
llmunload                       - Unload model and free all PSRAM buffers
llmmodels                       - List available model files on LittleFS and SD
llmgenerate <prompt>            - Generate text from prompt (runs synchronously)
llmstop                         - Interrupt in-progress generation
```
</details>

<details>
<summary><strong>maps - Offline maps & waypoints (requires ENABLE_MAPS)</strong></summary>

```
maplist                         - List available map files
mapload "<path>"                - Load a map file into memory
mapunload                       - Unload current map and free PSRAM
map                             - Show current map info and GPS position
whereami                        - Show current location context (map, room, zone)
search <name>                   - Search map for a named feature
waypoint list                   - List all waypoints with their index
waypoint add <lat> <lon> [name] - Add a waypoint at the given coordinates (decimal degrees)
waypoint del <index>            - Delete a waypoint (index from waypoint list)
waypoint goto <index>           - Navigate to a waypoint
waypoint clear                  - Clear the navigation target
waypoint clearall               - Delete all waypoints
waypoint rename <index> <name>  - Rename a waypoint
waypoint notes <index> <notes>  - Set a waypoint's notes
waypointfile "<file>" <wpName>  - Link a file to a waypoint (quoted path; or "<file>" <lat> <lon> [wpName])
waypointfiles <name> [del <idx>] - List or remove waypoint file attachments
gpstrack status                 - Show GPS track log status
gpstrack load                   - Load a saved GPS track
gpstrack clear                  - Clear the current track
gpslog [interval_ms]            - Start GPS track logging (persists across boots)
maporganize                     - Organize map files in /maps into subdirectories
```
</details>

<details>
<summary><strong>power - Power management</strong></summary>

```
power                           - Show the mode, live and target clock, and any busy workload
power json                      - The same as one JSON blob (the schema of /api/power/status)
power mode <mode>               - perf, balanced, adaptive (the default), ultra, locked, or 0-4
power auto / power threshold    - Not available in this firmware
```

A mode sets the clock while the device is in use and after the idle timeout
(`powersave`). Every mode runs transcription, the camera and a recording at the
maximum clock while they are busy and drops back when they finish; Adaptive
stays at the lowest clock otherwise. The idle downclock runs on every board. A
board with a local display also blanks it after the timeout, and only local
input, the display's own commands or on-device voice light it again: a command
from the glasses, phone, web or serial keeps the clock up but leaves the screen
dark. `cpufreq` sets the clock directly and lasts until the policy next changes
its target.
</details>

<details>
<summary><strong>battery - Battery monitoring (requires ENABLE_BATTERY_MONITOR)</strong></summary>

```
batterystatus                   - Show voltage, charge level, and status
batterycalibrate                - Reinitialize ADC calibration or re-probe the fuel gauge
batterylog [status|on|off|interval <s>|tail|clear]  - Battery time-series CSV log
```

Use `batterystatus json` for the measured voltage, validity flags and estimate
source, or open `/battery` for the same readings and history. The P4X-EYE uses
its calibrated GPIO49 divider through the shared ADC backend. A `~` marks an
estimated percentage in the web page and display widgets. Its charger can
raise the measured BAT-node voltage with no cell fitted, so voltage alone
cannot establish cell presence, USB presence, or charging. Those facts remain
unknown unless the board has the corresponding telemetry. Missing or stale
numeric readings are `null` in JSON and `--` in the UI; they are not 0% or 100%.

Monitored boards allow OTA with a fresh confirmed USB signal, a fresh ADC
voltage of at least 3.65 V, or a fresh fuel-gauge reading of at least 30%, as
appropriate for the backend. Low/unknown ADC voltage no longer proves external
power. The existing explicit `force-power` override remains available. Builds
without battery monitoring retain their existing unmonitored OTA policy.
</details>

<details>
<summary><strong>images - Image capture and management</strong></summary>

```
capture [littlefs|sd|both]      - Capture and save an image
images [littlefs|sd]            - List saved images
imagedelete "<path>"            - Delete an image
imagesend <device> "<path>"     - Send image to a peer via ESP-NOW (blocks; fails if the receiver cancels)
```
</details>

<details>
<summary><strong>camera - shared DVP/CSI camera (requires ENABLE_CAMERA_SENSOR)</strong></summary>

```
opencamera                      - Start camera sensor
closecamera                     - Stop camera sensor
cameraread                      - Show camera status
cameracapture                   - Capture a single frame
camerasave                      - Save current frame to storage
camerares <res>                 - Set resolution preset
cameraquality <0-63>            - Set JPEG quality (lower = higher quality)
camerabrightness <-2..2>        - Set brightness
cameracontrast <-2..2>          - Set contrast
camerasaturation <-2..2>        - Set saturation
cameraexposure <-2..2>          - Set AE level
cameraaec <on|off>              - Auto exposure
cameraaecvalue <0-1200>         - Manual exposure value
cameraagc <on|off>              - Auto gain
cameraagcgain <0-30>            - Manual gain
camerahmirror <on|off>          - Horizontal mirror
cameravflip <on|off>            - Vertical flip
camerarotate <on|off>           - Rotate 180
camerawb <0-4>                  - White balance mode
cameraeffect <0-6>              - Special effect
camerasharpness <-2..2>         - Sharpness
cameradenoise <0-8>             - Denoise level
cameraautostart <on|off>        - Auto-start on boot
camerastoragelocation <0-2>     - Storage destination (LittleFS/SD/both)
cameracapturefolder <path>      - Folder for captured images
cameramaxstoredimages <0-1000>  - Max images to keep
cameratiny                      - Capture a small frame (for ESP-NOW transfer)
```

The same photo, preview and stream commands are used on S3 and P4. Run
`cameraread` to see this camera's supported resolutions and sensor controls.
The web page disables unavailable controls; the G2 settings page omits them.
A CLI request for an unsupported size or control returns an error. JPEG quality
uses the existing 0..63 scale on both chips, with lower values giving better
quality. Capture fails cleanly if its JPEG exceeds the shared output budget;
reduce resolution or increase the quality number and retry.

Saved resolution IDs 0..10 are unchanged; 11 is HD (1280x720), and 12 is CIF
(400x296). Only sizes reported by the active backend can be selected. P4 CSI
capture may scale a fixed sensor source to produce smaller JPEGs; the status
reports source dimensions separately from output dimensions. SD recording
still requires a card, and G2 viewing still requires connected glasses.
</details>

<details>
<summary><strong>microphone - PDM microphone (ESP32-S3 only, requires ENABLE_MICROPHONE_SENSOR)</strong></summary>

```
openmic                         - Start microphone
closemic                        - Stop microphone
micread                         - Show microphone status
miclevel                        - Get current audio level
micviz                          - Real-time audio level visualizer
micrecord                       - Start/stop recording to WAV file
miclist                         - List saved recordings
micdelete                       - Delete recording(s)
micsamplerate                   - Get/set sample rate
micgain                         - Get/set microphone gain
micbitdepth                     - Get/set bit depth
micautostart [on|off]           - Auto-start on boot
```
</details>

<details>
<summary><strong>speech - ESP-SR voice recognition (requires ENABLE_ESP_SR)</strong></summary>

```
opensr                          - Start ESP-SR pipeline
closesr                         - Stop ESP-SR pipeline
srstatus                        - Show pipeline status
srconfidence                    - Get/set command confidence threshold
srtimeout                       - Get/set command listening timeout
srraw                           - Toggle raw output mode (all MultiNet hypotheses)
srautotune                      - Auto-cycle gain configs to find best settings

--- Voice arming ---
voicearm                        - Arm voice command execution as current user
voicedisarm                     - Disarm voice command execution
voicestatus                     - Show arming status
voicecancel                     - Spoken only: say cancel after the wake word
voicehelp                       - Spoken only: say help after the wake word

--- Command phrases ---
srcmdslist                      - List current MultiNet command phrases
srcmdsadd                       - Add or update a command phrase
srcmdsdel                       - Delete a command phrase (by phrase or ID)
srcmdsclear                     - Clear all command phrases
srcmdsreload                    - Reload phrases from SD file
srcmdssave                      - Save current phrases to SD file
srcmdssync                      - Sync voice commands from CLI registry

--- Audio tuning ---
srtuningswgain <1.0-50.0>       - Set software gain
srtuninggain <0.1-10.0>         - Set AFE linear gain (saved; restart SR to apply)
srtuningagc <0-3>               - Set AGC mode (0=off) (saved; restart SR to apply)
srtuningvad <0-4>               - Set VAD sensitivity (saved; restart SR to apply)
srtuningfilters                 - Toggle high-pass + pre-emphasis filters
srdyngain                       - Configure dynamic gain normalization

--- Snippet capture ---
srsnipon                        - Enable auto-capture on wake word
srsnipoff                       - Disable auto-capture
srsnipstart                     - Start manual snippet capture now
srsnipstop                      - Stop and save manual snippet
srsnipstatus                    - Show snippet capture status
srsnipconfig                    - Configure snippet capture parameters

--- Debug ---
srdebuglevel <0-4>              - Set debug verbosity
srdebugtelem <ms>               - Set periodic telemetry interval (0=off)
srdebugstats                    - Print current SR statistics
srdebugreset                    - Reset SR counters
```
</details>

<details>
<summary><strong>g2 - Even Realities G2 glasses (requires ENABLE_G2_GLASSES)</strong></summary>

```
openg2 [left|right|auto]        - Connect to G2 glasses
closeg2                         - Close the glasses: drop both links and release the glasses runtime (ring, phone and Bluetooth stay up)
g2status                        - Show connection status
g2scan                          - Scan for G2 glasses
g2show <text>                   - Display text on G2 glasses
g2clear                         - Clear G2 display
g2nav [on|off]                  - Map G2 gestures to OLED menu navigation
g2verbose [on|off]              - Toggle verbose packet logging
g2health                        - Open Apps -> Health (R1 vitals + graphs) on the lens (ENABLE_R1_HEALTH)
g2hold [status|clear]           - Hold (long-press) input: firmware profile, settings, last 16 holds
g2hold sim <press|release|abort> - Admin: synthetic hold through the real dispatcher (press: paired wearer only)
g2holdaction [0|1]              - What a hold does on a HW1 page: 0 = Off, 1 = QuickNote
g2holdsource [0|1]              - 0 = ring only, 1 = ring + temples
g2holdunprofiled [0|1]          - Testing: act on holds from unprofiled glasses firmware
g2 stats status                 - Stock widget feed: state, metrics shown now, sends and acks
g2 stats preview                - The stock rows and the first message's bytes, not sent
g2 stats send                   - Admin: send one batch of the saved metrics now
g2 stats feed [on|off]          - Admin: feed the stock widget with system metrics (saved, off by default)
g2 stats metrics [list|default] - Admin: up to 9 metrics, in order (saved)
g2 stats interval [seconds]     - Admin: least time between sends of changed values, 30-3600 (default 60)
g2 stats pad [gap|flat]         - Admin: how the chart's left part is filled before its window is full (saved, default gap)
g2 stats step [10|20|60]        - Admin: seconds between chart points (saved, default 20); a change starts the chart over
```

**Stock widget as a system-stats display.** The glasses' own stock widget (dashboard tile and
expanded stock page, stock firmware 2.3.2.14 only) can show HardwareOne's system metrics in place of
share prices: each metric is one "stock" with its label as the ticker (row 1), its long name as the
company (row 2), the value as the price, the change over the last refresh interval (60 s on the tile,
10 s with the page open) as the change with an up/down/flat arrow, and its history as the chart (percentages on a fixed 0-100 scale).
`g2 stats feed on` (admin, saved, off by default) sends a full set to the right temple after every
connect, then whenever a shown value changed or a new chart point was taken (so the chart scrolls
even when every value is steady), at most once per `g2 stats interval` (default 60 s)
and every 10 s while the glasses report the expanded stock page open. It pauses while Even AI,
Conversate, the native terminal, a native note or QuickNote use the glasses, and waits for the
glasses' ack to each stock before sending the next (one retry after 3 s). The 10 s cadence ends when
the glasses report the page closed, or 90 s after their last page update (a display that times out
does not report it).

The chart is always the glasses' full 185 points, one point per chart step, taken on the portable's
own clock whatever the send cadence, newest point at the right edge, so the line scrolls left as time
passes; the rightmost point is the value shown. The step is chosen with `g2 stats step` (admin,
saved; also a choice in the settings editors as `g2StockStepSec`), and the window is 185 steps:

| `g2 stats step` | Chart window |
|---|---|
| `10` | about 31 min |
| `20` (default) | about 1 h 2 min |
| `60` | 3 h 5 min |

Any other value is refused with the list of allowed ones. Changing the step drops the chart history
at once and starts it over at the new step (one chart never mixes two spacings), and the feed sends
a full set. The tile's refresh (60 s, 10 s with the page open) and the change over it stay as they
are at every step; with the 60 s step a point and a refresh often fall on the same sample, and
only the refresh moves the change. `g2 stats status` shows the step and the window it covers. While
the feed is on the portable keeps sampling when the glasses are off or out of range, so a reconnect
shows the whole window. `g2 stats feed off` and a reboot of the portable drop the history (it is
not saved). Until the window is full the left part is padded, chosen with `g2 stats pad` (admin,
saved): `gap` (default) leaves it empty (sent as NaN points; how the lens draws them is not yet
confirmed) and `flat` repeats the oldest point. A sample a metric cannot read adds no point, so its
time axis then runs slightly short of the window. A full set is 9 stocks of about 840 bytes each (4 BLE
fragments), sent one at a time, which is why the page-open cadence is 10 s rather than 5 s. A set that was cut short (a lost ack or a failed write) goes again after a
clear, so the tile can be blank for a second or two. `g2 stats feed off` clears the widget. The default metrics, in order, are `CPU,HEAP,PSRM,SD,LFS,TEMP,LINK,MESH,RING` (CPU load,
internal RAM used, PSRAM used, SD card used, flash storage used, chip temperature, BLE links in use,
ESP-NOW peers up, R1 ring battery); `CPU0`, `CPU1` (per core) and `HBLK` (largest internal RAM block,
KB) can be chosen too, e.g. `g2 stats metrics cpu,heap,psrm`. A metric this device cannot read is
left out rather than shown as 0; SD and flash use come from the storage readings the firmware already
keeps, so right after boot or a card change they are left out until one exists. The stock tile has to be in the glasses' dashboard layout (set in
the Even app); HardwareOne does not change the layout, and the Even app replaces the rows with real
stocks when it next sends its own. Real share prices are added in the Even app's stock watchlist;
`g2 stats metrics` only chooses system metrics.

**Apps -> Health** (lens, `ENABLE_R1_HEALTH`): left list (Overview / Activity / Trends / Heart Rate /
HRV / SpO2 / Temperature / Battery / Poll Now / Health Logging). **Overview** shows native-text
vitals (wear + one shared recentness on the status line); live metric rows show
title/value/age above a line graph. **Trends** opens a submenu (HR/HRV/SpO2 today +
Refresh) that graphs the ring's daily-history payload separately from the live
sparklines (weekly aggregation later).
**Health Logging** toggles local R1 logging (`healthlogging on`); while on, the ring is
mined every `healthLoggingPollIntervalSec` seconds (default 900 = 15 min). Opening the page or **Poll Now**
also logs a sample when R1 logging is active. Pair the R1 under Bluetooth -> R1 Ring;
vitals also on OLED **R1 Health** and Web **`/r1-health`**.
Ring/Health CLI: `openring`, `closering`, `ring status`, `ring query hr|temp|wear`,
`ring health [json]`, `ring poll`, `healthlogging status|interval`. A Bluetooth App can call the
same commands over GATT (no dedicated phone UI yet).
The glasses' own **Health** tile (stock firmware) can show the same R1 data without the Even app:
`ring panel feed on` (admin, saved, on by default) pushes steps, active kcal, sleep score and
duration, heart rate and SpO2 to the right temple on connect and when values change (at most once a
minute), and keeps the ring data fresh while it runs (`ring panel history <min>`, default 30).
`ring panel status` shows what was sent. Details: [G2_HEALTH_PANEL.md](G2_HEALTH_PANEL.md).
3-pane list+text+image experiments: Tests -> Image -> Com2 (LZ4 multi) -> Q30*.
</details>

<details>
<summary><strong>note - QuickNote private spoken notes (requires ENABLE_QUICKNOTE)</strong></summary>

```
note start [hold|toggle] [mic=auto|g2|pdm]   - Start a note (lens, web CLI or local display)
note start glasses [hold|toggle]             - Start on the paired wearer's glasses (serial, BLE app, UART)
note stop|cancel <id>                        - Save / discard the live note
note status [id]                             - Live note or how it ended (never the text)
note list [internal|sd] [offset] [u<id>]     - Your notes, eight per page, newest first
note read "<path>" [offset]                  - 512-byte window of a note
note delete "<path>"                         - Delete a note
notemaxsec <10-120>                          - Longest note (CM5 builds cap it at 30)
notemic <0|1|2>                              - Mic for web/display notes: auto, glasses, board
```

Notes are saved under `/sd/stt/u<id>/notes/` (or `/stt/...`), open to their
owner and superadmins only. See [QUICKNOTE.md](QUICKNOTE.md).
</details>

<details>
<summary><strong>edgeimpulse - Edge Impulse ML inference (requires ENABLE_EDGE_IMPULSE)</strong></summary>

```
eienable                        - Enable/disable Edge Impulse inference
eidetect                        - Run single object detection inference
eifile                          - Run inference on a stored JPEG image
eicontinuous                    - Start/stop continuous inference mode
eiconfidence                    - Set minimum detection confidence threshold
eistatus                        - Show Edge Impulse status

--- Model management ---
eimodellist                     - List available .tflite models on LittleFS
eimodelload                     - Load a TFLite model
eimodelinfo                     - Show loaded model information
eimodelunload                   - Unload the current model

--- State tracking ---
eitrackstatus                   - Show currently tracked objects
eitrackenable                   - Enable/disable state tracking
eitrackclear                    - Clear all tracked objects
```
</details>

<details>
<summary><strong>thermal - MLX90640 thermal camera</strong></summary>

```
openthermal                     - Start thermal sensor
closethermal                    - Stop thermal sensor
thermalread                     - Read min/max/avg temperature
thermalautostart [on|off]       - Auto-start on boot
thermaldiag                     - Run sensor diagnostics
thermalpollingms <50-5000>      - UI polling interval
thermalpalettedefault <name>    - Color palette (grayscale|iron|rainbow|hot|coolwarm)
thermalrotation <0-3>           - Rotate thermal image (0=0, 1=90, 2=180, 3=270)
thermalinterpolationenabled <0|1>       - Enable interpolated upscaling
thermalinterpolationsteps <1-8>         - Interpolation steps
thermalupscalefactor <1-4>              - Display upscale factor
thermaltargetfps <1-8>                  - Target frame rate
thermaldevicepollms <100-2000>          - Sensor hardware poll interval
thermaltemporalalpha <0.0-1.0>          - Temporal smoothing factor
thermalewmafactor <0.0-1.0>             - EWMA smoothing factor
thermaltransitionms <0-5000>            - Color transition time
thermalrollingminmaxenabled <0|1>       - Rolling min/max normalization
thermalrollingminmaxalpha <0.0-1.0>     - Rolling normalization alpha
thermalrollingminmaxguardc <0.0-10.0>   - Rolling normalization guard band
```
</details>

<details>
<summary><strong>tof - VL53L4CX Time-of-Flight sensor</strong></summary>

```
opentof                         - Start ToF sensor
closetof                        - Stop ToF sensor
tofread                         - Read distance measurement(s)
tofautostart [on|off]           - Auto-start on boot
tofpollingms <50-5000>          - UI polling interval
tofdevicepollms <100-2000>      - Sensor hardware poll interval
tofmaxdistancemm <100-10000>    - Maximum valid distance
tofstabilitythreshold <0-50>    - Stability threshold for readings
toftransitionms <0-5000>        - Reading transition time
```
</details>

<details>
<summary><strong>imu - BNO055 9-DoF IMU</strong></summary>

```
openimu                         - Start IMU sensor
closeimu                        - Stop IMU sensor
imuread                         - Read orientation data
imuautostart [on|off]           - Auto-start on boot
imuactions                      - Show action detection state (tap, shake, etc.)
imupollingms <50-2000>          - UI polling interval
imudevicepollms <50-1000>       - Sensor hardware poll interval
imuorientationmode <0-8>        - Orientation mode
imuorientationcorrection <0|1>  - Apply orientation correction
imupitchoffset <-180..180>      - Pitch offset correction
imurolloffset <-180..180>       - Roll offset correction
imuyawoffset <-180..180>        - Yaw offset correction
imuewmafactor <0.0-1.0>         - EWMA smoothing factor
imutransitionms <0-1000>        - Transition time
imuwebmaxfps <1-30>             - Web UI max frame rate
```
</details>

<details>
<summary><strong>apds - APDS9960 gesture/proximity/color sensor</strong></summary>

```
openapds                        - Start APDS9960 sensor
closeapds                       - Stop APDS9960 sensor
apdsread                        - Read sensor status and data
apdsmode <color|proximity|gesture> [on|off]  - Enable/disable a mode
apdscolor                       - Read color/RGB values
apdsproximity                   - Read proximity value
apdsgesture                     - Read gesture (up/down/left/right)
apdsautostart [on|off]          - Auto-start on boot
```
</details>

<details>
<summary><strong>gps - PA1010D GPS module</strong></summary>

```
opengps                         - Start GPS module
closegps                        - Stop GPS module
gpsread                         - Read current location, speed, and heading
gpsautostart [on|off]           - Auto-start on boot
gpslog [interval_ms]            - Start GPS track logging (persists across boots)
```
</details>

<details>
<summary><strong>rtc - DS3231 precision RTC</strong></summary>

```
openrtc                         - Start RTC
closertc                        - Stop RTC
rtcread [status|temp]           - Read time or temperature compensation
rtcset <datetime|timestamp>     - Set RTC time (UTC, years 2020-2099)
rtcsync [to|from]               - Sync time to/from system clock
rtcautostart [on|off]           - Auto-start on boot
```
</details>

<details>
<summary><strong>presence - STHS34PF80 IR presence/motion sensor</strong></summary>

```
openpresence                    - Start presence sensor
closepresence                   - Stop presence sensor
presenceread                    - Read presence, motion, and temperature data
presencestatus                  - Show sensor status
presenceautostart [on|off]      - Auto-start on boot
```
</details>

<details>
<summary><strong>fmradio - RDA5807 FM radio</strong></summary>

```
openfmradio                     - Start FM radio
closefmradio                    - Stop FM radio
fmradioread                     - Read tuner status
fmradiotune <MHz>               - Tune to frequency (e.g. fmradiotune 101.5)
fmradioseek [up|down]           - Seek next station
fmradiovolume <0-15>            - Set volume
fmradiomute                     - Mute audio
fmradiounmute                   - Unmute audio
fmradioautostart [on|off]       - Auto-start on boot
```
</details>

<details>
<summary><strong>servo - PCA9685 servo controller</strong></summary>

```
servo <channel> <angle>         - Move servo to angle (degrees)
pwm <channel> <value> [freq]    - Set raw PWM output
servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>  - Configure servo profile
servolist                       - List configured servo profiles
servocalibrate <channel>        - Enter calibration mode for a channel
```
</details>

<details>
<summary><strong>input - Gamepad / ANO encoder</strong></summary>

The physical input controller is chosen at build time with `INPUT_DEVICE_TYPE`
(`0`=none, `1`=Seesaw gamepad, `2`=ANO rotary encoder; the P4 boards set `3`,
the P4X-EYE wheel and buttons, and `4`, the Waveshare GT911 touch panel, in
their own feature files). The gamepad and the encoder are mutually exclusive -
both hang off STEMMA QT and would collide. Start/stop and
auto-start are device-agnostic; only the read and tuning commands are
driver-specific.

```
openinput                       - Start the input device (gamepad or ANO encoder)
closeinput                      - Stop the input device
inputautostart [on|off]         - Auto-start on boot
inputdevicepollms <10-1000>     - Poll interval

--- Seesaw gamepad (INPUT_DEVICE_TYPE=1) ---
gamepadread                     - Read joystick axes and button states

--- ANO rotary encoder (INPUT_DEVICE_TYPE=2) ---
anoencoderread                  - Read encoder position and button state
anoencoderi2caddr <1-127>       - Set I2C address
anoencoderinvert [on|off]       - Invert rotation direction
anoencoderswapud [on|off|toggle]  - Swap UP/DOWN buttons
anoencoderswaplr [on|off|toggle]  - Swap LEFT/RIGHT buttons
```

> `opengamepad`, `closegamepad`, and `gamepadautostart` no longer exist - they
> were replaced by the device-agnostic `openinput` / `closeinput` /
> `inputautostart` when the ANO encoder was added.
</details>

---

## Per-Module Notes

### VL53L4CX (ToF)
Supports up to 4 simultaneous distance measurements. Range up to 6m. Polling rate configurable via settings (`tofPollingMs`, default 220ms).

### MLX90640 (Thermal)
32x24 IR thermal camera. Web UI displays interpolated heatmap with HSL color mapping. Configurable palette, interpolation steps, and frame rate. High memory footprint - uses PSRAM.

### BNO055 (IMU)
9-DoF orientation (accel + gyro + magnetometer fusion). Orientation correction configurable via settings (`imuOrientationMode`, `imuPitchOffset`, etc.).

### APDS9960
Three independent modes: color/RGB (`apdscolor`), proximity (`apdsproximity`), gesture up/down/left/right (`apdsgesture`). Each can be run independently.

### GPS (PA1010D)
NMEA output parsed for lat/lon/speed/heading. Track logging to LittleFS. Offline map viewer in web UI.

### FM Radio (RDA5807)
Tune, seek up/down, set volume, mute. `fmradiotune <frequency>` - e.g., `fmradiotune 101.5`.

### Even Realities G2 Glasses
BLE client for Even G2 temples (`openg2` / hijack menus / Apps -> Health). Requires
`ENABLE_G2_GLASSES`. See the `g2` / `g2health` / `ring*` command sections above.
R1 ring connect and vitals share the same Bluetooth central stack. A ring or
temple hold on a HardwareOne page is an input of its own; QuickNote uses it
([QUICKNOTE.md](QUICKNOTE.md)).

---

## License

HardwareOne is licensed under the PolyForm Noncommercial License 1.0.0: you may use,
change and share it for any noncommercial purpose. See [LICENSE.md](../LICENSE.md).
Bundled third-party code keeps its own licence; see [NOTICE.md](../NOTICE.md).

---

> ## Quick start: [QUICKSTART.md](QUICKSTART.md)
> ## Overview: [README](../README.md)
