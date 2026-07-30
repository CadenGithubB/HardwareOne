# Hardware One v0.99.5 - User Guide

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
- [On-Device LLM](#on-device-llm)
- [Debug Flags](#debug-flags)
- [Command Reference](#command-reference) - common commands; full generated list in [COMMAND_REFERENCE.md](COMMAND_REFERENCE.md)
- [Per-Module Notes](#per-module-notes)
- [License](#license)

---

## Build Configuration

All feature flags live in one file: `components/hardwareone/System_BuildConfig.h`. Edit it before building to enable or disable any subsystem. No other files need to change.

| Flag | Default | Description |
| ---- | :-----: | ----------- |
| `I2C_FEATURE_LEVEL` | `4` (Custom) | `0`=disabled, `1`=OLED only, `2`=OLED+gamepad, `3`=all sensors, `4`=custom selection |
| `NETWORK_FEATURE_LEVEL` | `4` (Custom) | `0`=disabled, `1`=WiFi only, `2`=WiFi+HTTP, `3`=WiFi+HTTP+ESP-NOW, `4`=custom |
| `WEB_FEATURE_LEVEL` | `4` (Custom) | `0`=disabled, `1`=core UI, `2`=standard modules, `3`=all modules, `4`=custom |
| `DISPLAY_TYPE` | `1` (SSD1306) | `0`=none, `1`=SSD1306 OLED, `2`=ST7789 TFT, `3`=ILI9341 TFT. SSD1306 is the tested path; the TFT branches exist in `HAL_Display.cpp` but ILI9341 is a placeholder and the `OLED_Mode_*` UI is SSD1306-shaped |
| `INPUT_DEVICE_TYPE` | `1` (gamepad) | Physical input controller, mutually exclusive: `0`=none, `1`=Seesaw gamepad, `2`=ANO rotary encoder |
| `ENABLE_HTTPS` | `1` | TLS for the web server; certs in `/system/certs/`. Runtime toggle: `httpsEnabled` |
| `ENABLE_MAPS` | `1` | Offline maps and waypoints |
| `ENABLE_GAMES` | `0` | Browser games page. Pick exactly one game: `ENABLE_WEB_GAME_MAZE` or `ENABLE_WEB_GAME_DARKROOM` - both at once overflows the app partition and is rejected at build time |
| `ENABLE_ESP_SR` | `0` | ESP-SR voice: WakeNet wake word + MultiNet command grammar |
| `ENABLE_BLUETOOTH` | `0` | BLE server with GATT services |
| `ENABLE_G2_GLASSES` | `0` | Even Realities G2 BLE client (requires `ENABLE_BLUETOOTH=1`) |
| `ENABLE_R1_HEALTH` | `1` | R1 Health vitals UI (G2 Apps->Health, OLED, Web `/r1-health`) + Health Track. Requires Bluetooth + G2; auto-off if either is off. Ring connect stays under `ENABLE_G2_GLASSES`. |
| `ENABLE_MQTT` | `1` | Home Assistant MQTT integration |
| `ENABLE_AUTOMATION` | `1` | Scheduled tasks and conditional commands |
| `ENABLE_CAMERA_SENSOR` | `0` | ESP32-S3 DVP camera (OV2640/OV5640) |
| `ENABLE_MICROPHONE_SENSOR` | `0` | PDM microphone via I2S |
| `ENABLE_BATTERY_MONITOR` | `0` | LiPo voltage monitoring via ADC |
| `ENABLE_EDGE_IMPULSE` | `0` | Edge Impulse ML inference |
| `ENABLE_BONDED_MODE` | `0` | Bonded Microcontrollers - two devices share command registries and the controller shows a Remote tab with the paired device's features |
| `ENABLE_ONDEVICE_LLM` | `1` | On-device LLM inference - tiny transformer runs locally on ESP32-S3 with PSRAM. Requires a model file on LittleFS or SD card. |

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

Set by `sdkconfig.defaults.<target>`, selected automatically by `set-target`:

| | ESP32 (QT Py, Feather V2) | ESP32-S3 (XIAO, FeatherS3) |
| - | ------------------------- | -------------------------- |
| PSRAM speed | 40 MHz | 80 MHz |
| Flash mode | `DIO` | `QIO` |
| Bluetooth | Classic BT + BLE 4.2 | BLE 5.0 only |
| Camera/Mic | No | Yes (S3 only) |
| USB | UART bridge (`usbserial`) | Native USB (`usbmodem`) |
| `set-target` | `esp32` | `esp32s3` |

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

> **Do not infer PSRAM mode from the chip family.** The two ESP32-S3 boards above
> disagree - the XIAO is octal, the FeatherS3 is quad. A wrong mode is a *silent*
> failure: the device boots and reports 0 KB PSRAM, and the first thing you notice
> is the LLM, large web responses, or ESP-NOW buffers OOMing at runtime.

When switching between chip families: `idf.py fullclean`, then
`idf.py set-target <chip>`.

---

## Web UI

Navigate to the device's IP address in a browser. The web server must be running - start it for this session with `openhttp`, or set `httpAutoStart 1` to bring it up on every boot.

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
| **R1 Health** | `/r1-health` | `WEB_R1_HEALTH` | Ring vitals, graphs, Health Track (requires `ENABLE_R1_HEALTH`) |
| **Battery** | `/battery` | `WEB_BATTERY` | Voltage, charge level, battery log |
| **MQTT** | `/mqtt` | `WEB_MQTT` | Broker config, topic preview, Home Assistant status (requires `ENABLE_MQTT`) |
| **Speech** | `/speech` | `WEB_SPEECH` | ESP-SR status and tuning (requires `ENABLE_ESP_SR`) |
| **LLM** | `/llm` | `ENABLE_ONDEVICE_LLM` | On-device model chat - load/unload, ask, temperature and sampling |
| **Games** | `/games` or `/darkroom` | `WEB_GAMES` | Browser game (requires `ENABLE_GAMES`; exactly one game per build) |
| **Settings** | `/settings` | core | All device settings, debug flags, user management |
| **CLI** | `/cli` | core | Full command interface in the browser, with history |

Authentication is required; `/login` and `/register` are the unauthenticated entry points. Credentials are created on first boot via the setup wizard, or later with the `users` CLI commands.

> There is no **Pair** page. Pairing and bonding are run from the ESP-NOW page.

---

## OLED Interface

The OLED displays a menu system navigated with the input device - Seesaw gamepad or ANO rotary encoder, whichever `INPUT_DEVICE_TYPE` selects. On first boot the setup wizard runs here as well as on serial (see the [Quick Start](QUICKSTART.md#first-time-use)). After that it lands on the main menu.

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
- **Health** (R1 ring vitals, Poll Now, Health Track) requires `ENABLE_R1_HEALTH`. Ring *pairing* is under Connect -> Bluetooth.
- **Pet** is G2-only - it has no OLED mode, so the lens Apps launcher has one row this menu does not.

---

## G2 Lens Interface

Requires `ENABLE_BLUETOOTH=1` and `ENABLE_G2_GLASSES=1`. The ESP32 acts as BLE
central and drives the Even Realities G2 temples directly - the firmware
"hijacks" the lens to render its own pages, so this is a full on-glasses UI, not
a notification mirror. Connect with `openg2`; navigate by tapping the temple.

The main menu mirrors the OLED's six categories:

| Category | Rows |
| -------- | ---- |
| **System** | Status, System Events, Logging, Tests |
| **Config** | Settings, Users (admin only), OLED Login |
| **Connect** | WiFi >>, Bluetooth >>, ESP-NOW >> |
| **Hardware** | Sensor list (one row per compiled sensor), LED, FM tuner |
| **Apps** | ESP-NOW, Files, Maps, LLM, Automations, Health, Pet |
| **Power** | CPU presets, restart, power off |

Sub-pages are registered but hidden from the top level - they are reached
through their category, and every page has a `<- Back` row. A few sit one level
deeper still: **Camera settings** is reached from Hardware -> sensor list -> `CAM`
-> `Settings >`, and **Mic detail** from the `MIC` row the same way.

### Apps

| App | Notes |
| --- | ----- |
| **ESP-NOW** | Peer messaging. Distinct from Connect -> ESP-NOW, which owns the settings |
| **Files** | Browse, view, rename, delete. Folders carry an item-count badge |
| **Maps** | Pan/zoom offline map viewer. Row reads `Maps (none)` when no map files are present (requires `ENABLE_MAPS`) |
| **LLM** | Read-only streaming viewer plus a guided-input picker (requires `ENABLE_ONDEVICE_LLM`) |
| **Automations** | List, view, and run automations |
| **Health** | R1 ring vitals + sparkline graphs (requires `ENABLE_R1_HEALTH`) - see below |
| **Pet** | Virtual-creature app with an animated tile renderer |

### Apps -> Health

Left column is a metric list, right side a 288x144 graph: Overview, Trends,
Heart Rate, HRV, SpO2, Temperature, Battery, Poll Now, Toggle Track.

- **Overview** - native-text vitals with wear state and one shared recentness figure on the status line.
- **Trends** - submenu graphing the ring's daily-history payload (HR / HRV / SpO2 today + Refresh), kept separate from the live sparklines.
- **Toggle Track** - starts Health Track logging (`healthtrack on`). While on, the ring is mined every `healthTrackPollIntervalSec` (default 15 min). Opening the page or **Poll Now** also logs a sample when R1 logging is active.

Pair the ring under Connect -> Bluetooth. The same vitals appear on the OLED
(**Apps -> Health**) and the web (**`/r1-health`**).

### Text entry

Pages that need input (WiFi join, file rename, login) open an on-lens keyboard.
Two things to know: it deliberately has **no double-quote key**, and a
pre-filled value longer than 32 characters is silently truncated.

### CLI

Most lens pages have a CLI equivalent - `g2status`, `g2show`, `g2clear`,
`g2nav`, `g2health`, `g2pet`, `g2map`, `g2files`, `g2sensors`, `g2settings`,
`g2network`, `g2battery`, and the `g2ai*` / `g2mic*` / `g2notify*` families.
Run `help g2` on the device for the full list.

> `openg2` and `ringconnect` return OK when the connection is *kicked off*, not
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
| `guest` | 0 | Authenticated but view-only. `login` / `logout` only for commands; filesystem access is masked to read |
| `user` | 1 | Ordinary commands; not admin-gated ones |
| `admin` | 2 | Admin commands - user management, most settings, device control |
| `superadmin` | 3 | Additionally the identity / crypto / destructive / auth-posture command set |

Rules that hold everywhere:

- **You cannot grant a role above your own.** Granting or removing `superadmin` requires a super-admin caller.
- A command marked super-admin-only is refused for an ordinary admin - `blerequireauth`, `serialrequireauth`, and `displayrequireauth` are examples, since they change whether the device demands a login at all.
- An unrecognised role name collapses to `user`.

Set roles with `useradd ... [role]`, `userpromote`, and `userdemote` - see the
[users command block](#command-reference).

### Per-transport authentication

Each way in has its own switch for whether a login is required, and its own
idle-logout timer. All the `*requireauth` switches are super-admin-only.

```
serialrequireauth <0|1>         - Require login on the serial console
displayrequireauth <0|1>        - Require login on the display
oledrequireauth <0|1>           - Require login for the OLED menu
blerequireauth [on|off]         - Require login for BLE access

sessionidleserial <0-1440>      - Serial idle-logout, minutes (0 = never)
sessionidleweb <0-1440>         - Web CLI idle-logout
sessionidledisplay <0-1440>     - OLED idle-logout
sessionidleble <0-1440>         - BLE idle-logout
```

Sessions and bans are managed with `sessionlist`, `sessionrevoke`, `ban` /
`unban` / `banlist` (by IP) and `banuser` / `unbanuser` (by account).

### Account recovery

`factoryreset` wipes user accounts and reboots into the setup wizard. It is the
intended way back in if you lose admin credentials - device settings and files
are not the target of the wipe, accounts are.

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

This is the fastest way to stand up a replacement board with an existing
device's settings, and the reason a factory reset is not a catastrophe if you
have a recent export.

---

## ESP-NOW Mesh

ESP-NOW V3 is Hardware One's inter-device wireless protocol. Devices pair with a shared passphrase and form an encrypted mesh.

### Pairing
1. On both devices, open the **ESP-NOW** page in the web UI (pairing lives there - there is no separate Pair page) or use the `espnowpair` CLI.
2. Set the same passphrase on both devices.
3. One device initiates - the other accepts.
4. Once paired, devices appear in each other's peer list.

### Bonding (Master/Worker)
With `ENABLE_BONDED_MODE=1`, two devices can bond into a master/worker pair. The master gains a **Remote** tab in its web UI showing the worker's features, even if those features aren't compiled into the master.

### Metadata Sync
Each device has a name, room, zone, and tags set in settings. The **Metadata** tab lets you pull this information from any peer. Set your own device metadata with (note: single words, no space after `espnow`):
```
espnowsetname [name]            - Device name (<=20 chars; letters, numbers, - and _)
espnowroom [name]               - Room, e.g. Kitchen. 'espnowroom clear' to unset
espnowzone [name]               - Zone, e.g. Counter. 'espnowzone clear' to unset
espnowtags [tag1,tag2,...]      - Tags. 'espnowtags clear' to unset
espnowfriendlyname [name]       - Display name (<=47 chars); 'clear' to unset
espnowstationary [on|off|0|1]   - Mark the device fixed or mobile
espnowdeviceinfo                - Show all local metadata
```
Called with no argument, each of these prints the current value.

### File Transfer
Files can be transferred between paired devices via the web UI or CLI (`espnow sendfile`). Used for syncing automations, settings, and manifests.

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
| `peer_paired` | pairing-mode auto-pair completes | peer name / MAC |
| `text_rx` | ESP-NOW text message received | sender / text preview |
| `file_rx` | ESP-NOW file received | sender / filename |
| `bond_online` / `bond_offline` | bonded peer session up / heartbeat timeout | peer name / MAC |
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
| `g2_worn` / `g2_not_worn` | glasses picked up / set down (plugin heartbeat proxy) | side |
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
| `fs_low_space` | flash below the log reserve (once per boot) | free bytes |
| `setting_changed` | a setting was saved | key / value |
| `settings_save_failed` | settings.json write failed | stage / file |
| `sensor_started` / `sensor_stopped` | sensor came up / stopped (includes Camera) | sensor name |
| `sensor_start_failed` | sensor failed to start | sensor name |
| `sensor_fault` | sensor auto-disabled after repeated I2C errors | sensor name / errors |
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
| System | `BATTERY` | numeric | charge percent (reads 100 on boards with no battery) |
| System | `HEAP` `PSRAM` `FSFREE` | numeric | free internal DRAM / PSRAM / storage, in KB |
| System | `UPTIME` | numeric | minutes since boot |
| System | `CHIPTEMP` | numeric | SoC temperature, C |
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
| Mesh/Bond | `BOND_PEER_HEAP` `BOND_PEER_UPTIME` | numeric | bonded peer free heap (KB) / uptime (min) |
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

Configure via the **MQTT** tab in the web UI, or via CLI:
```
mqtt broker <host>           - Set broker host/IP
mqtt port <port>             - Set broker port (default 1883)
mqtt user <username>         - Set MQTT username
mqtt pass <password>         - Set MQTT password
mqtt topic <prefix>          - Set topic prefix
mqtt connect                 - Connect to broker
mqtt disconnect              - Disconnect
mqtt status                  - Show connection status
```

---

## On-Device LLM

Requires `ENABLE_ONDEVICE_LLM=1` and an ESP32-S3 board with PSRAM. Runs a tiny transformer model entirely on-device - no internet connection required.

### Model files

Place LLM1-format model files (produced by `esp32-llm-converter`) in one of two locations:

- **LittleFS (internal flash):** `/system/llm/model.bin` - default path, loaded automatically
- **SD card:** `/sd/llm/<filename>.bin` - useful for swapping models without reflashing

The default path (`/system/llm/model.bin`) is loaded when you call `llmload` with no arguments. Use `llmmodels` to list all available files across both storage locations.

### Memory

Models and KV cache are allocated in PSRAM. The firmware automatically reduces the context window to fit available PSRAM (auto-fit). A 400 KB reserve is kept free for the rest of the system. Run `llmstatus` to see current PSRAM usage after loading.

### Web UI

The **LLM** tab provides a chat interface. Select a model from the dropdown, click **Load**, then type a prompt and click **Ask**. Adjustable settings: temperature, sentence limit, and repetition penalty.

### Generation modes

The model supports two generation modes triggered by how the prompt ends:

- **Normal mode** - generates a natural-language response, stopping after the configured sentence limit.
- **Do: mode** - when the prompt ends with the `Do:` token, the model outputs a CLI command instead of prose. Used internally by the automation and web UI to translate natural-language requests into executable commands.

### CLI commands

```
llmstatus               - Show LLM engine state, model config, and PSRAM usage
llmload [model.bin]     - Load model (default: /system/llm/model.bin)
llmunload               - Unload model and free all PSRAM buffers
llmmodels               - List available model files (LittleFS + SD)
llmgenerate <prompt>    - Generate text from a prompt (synchronous)
llmstop                 - Stop in-progress generation
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
| `debugespnow` | Alias of `debugespnowcore` - ESP-NOW core messages |
| `debugespnowcore` | ESP-NOW V3 frame layer |
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
| `debuggamepad` | Gamepad button/axis events |
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

---

## Command Reference

> **This section is a curated tour of the commands you reach for most - it is
> not the complete list.** The device registers **893 commands across 44
> modules**. For every command, generated directly from the `CommandEntry`
> tables, see **[COMMAND_REFERENCE.md](COMMAND_REFERENCE.md)**.

Type `help` on the device to enter the interactive help system. Type a module name to see its commands. Type `help all` to include disconnected sensors.

Two things about lookup that explain most surprises:

- Matching is **case-insensitive**, so `mqttHost` and `mqtthost` are the same command. A few commands are registered in camelCase (the `mqtt*` family and `httpAutoStart` / `httpsEnabled`); the rest are lowercase.
- Lookup is **longest-prefix**, so `automation list` resolves to the `automation` dispatcher with `list` as an argument rather than needing its own entry. This is why some features appear as one command with subcommands and others as a family of single words.

<details>
<summary><strong>core - System commands</strong></summary>

```
status                          - Show system status (WiFi, FS, memory)
uptime                          - Show device uptime
time                            - Show current time (uptime + NTP if synced)
timeset <YYYY-MM-DD HH:MM:SS>   - Set time manually (or unix timestamp)
reboot                          - Restart the device
ramflush                        - Reboot to reclaim RAM, restoring the features running right now
factoryreset                    - Wipe user accounts and reboot into the setup wizard (admin)
deepsleep                       - Power off via deep sleep (no wake source - reset button to wake)
bootcount [reset|json]          - Boot count (NVS), crash count, last reset reason
clear                           - Clear CLI history
broadcast <message>             - Send message to all users (admin)
wait <ms>                       - Delay execution for N milliseconds
sleep <ms>                      - Alias for wait
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
</details>

<details>
<summary><strong>espnow - ESP-NOW mesh</strong></summary>

```
openespnow                      - Initialize ESP-NOW
closeespnow                     - Deinitialize ESP-NOW and free resources
espnowstatus                    - Show ESP-NOW status and configuration
espnowstats                     - Show message/error counters
espnowlist                      - List all paired peers
espnowpair <mac> <name>         - Pair with a device
espnowunpair <name_or_mac>      - Remove a paired peer
espnowsend <name_or_mac> <msg>  - Send a text message (auto-routes via mesh)
espnowbroadcast <message>       - Broadcast to all peers
espnowsendfile <name_or_mac> "<path>"      - Send a file to a peer
espnowbrowse <name_or_mac> <user> <pass> ["path"]  - Browse remote filesystem
espnowfetch <name_or_mac> <user> <pass> "<path>"   - Fetch a file from a peer
espnowremote <name_or_mac> <user> <pass> <cmd>   - Execute command on peer

--- Mesh ---
espnowmode [direct|mesh]        - Get/set routing mode
espnowmeshstatus                - Show mesh peer health (heartbeats, ACKs)
espnowmeshmetrics               - Show routing metrics (forwards, drops, path stats)
espnowmeshttl [1-10|adaptive]   - Get/set mesh TTL
espnowmeshtopo                  - Discover mesh topology (master only)
espnowtimesync                  - Broadcast NTP time to mesh (master only)
espnowtimestatus                - Show time sync status

--- Device identity ---
espnowsetname [name]            - Get/set device name
espnowroom [name]               - Get/set room
espnowzone [name]               - Get/set zone
espnowtags [tag1,tag2,...]      - Get/set tags
espnowfriendlyname [name]       - Get/set friendly display name
espnowstationary [0|1]          - Get/set stationary flag
espnowdeviceinfo                - Show all local device metadata
espnowdevices                   - List all mesh devices with metadata (master)
espnowrooms                     - List rooms and their devices (master)
espnowfind <query>              - Find devices by name, room, or tag
espnowroomcmd <room> <cmd>      - Run command on all devices in a room
espnowtagcmd <tag> <cmd>        - Run command on all devices with a tag

--- Sensor streaming ---
espnowworker [show|on|off|interval <ms>|fields <list>]  - Worker status reporting
espnowsensorstream <sensor> <on|off>   - Stream sensor data to master (worker)
espnowsensorstatus              - Show remote sensor cache (master) or worker streaming status

--- Security ---
espnowsetpassphrase "phrase"    - Set encryption passphrase
espnowencstatus                 - Show encryption status and key fingerprint
espnowpairsecure <mac> <name>   - Pair with encryption

--- Metadata sync ---
espnowrequestmeta <name_or_mac> - Pull metadata from a peer
espnowusersync [on|off]         - Enable/disable credential sync across mesh
```
</details>

<details>
<summary><strong>bond - Bonded mode (requires ENABLE_BONDED_MODE)</strong></summary>

```
bondconnect <mac_or_name>       - Connect to bonded peer
bonddisconnect                  - Disconnect bonded peer
bondstatus                      - Show bond mode status
bondrole <master|worker>        - Set bond mode role
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
mqttHost [hostname]             - Set broker host/IP
mqttPort [port]                 - Set broker port (default 1883)
mqttUser [user|clear]           - Set username
mqttPassword [pass|clear]       - Set password
mqttBaseTopic [topic|auto]      - Set base topic prefix
mqttTLSMode [0|1|2]             - TLS mode (0=off, 1=verify, 2=no-verify)
mqttCACertPath [path|clear]     - CA certificate path
mqttPublishIntervalMs [ms]      - Sensor publish interval
mqttDiscoveryPrefix [prefix]    - Home Assistant discovery prefix
mqttPublishWiFi [0|1]           - Publish WiFi state
mqttPublishSystem [0|1]         - Publish system state
mqttPublishThermal [0|1]        - Publish thermal data
mqttPublishToF [0|1]            - Publish ToF distance
mqttPublishIMU [0|1]            - Publish IMU orientation
mqttPublishPresence [0|1]       - Publish presence/motion
mqttPublishGPS [0|1]            - Publish GPS location
mqttPublishAPDS [0|1]           - Publish APDS color/proximity
mqttPublishRTC [0|1]            - Publish RTC time
mqttPublishGamepad [0|1]        - Publish gamepad state
mqttSubscribeTopics [topics]    - Set external subscription topics
mqttExternalSensors             - List external sensor data received via MQTT
```
</details>

<details>
<summary><strong>bluetooth - BLE (requires ENABLE_BLUETOOTH)</strong></summary>

```
openble                         - Start BLE and begin advertising
closeble                        - Stop BLE and deinitialize
blestatus                       - Show connection status
bleinfo                         - Show BLE configuration and settings
blename [name]                  - Get/set BLE device name
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
sdformat                        - Format SD card as FAT32
sdinfo                          - Show SD card information
sddiag                          - SD card hardware diagnostics
```
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
oledbootmode <logo|status|thermal|off>   - Mode shown at boot
oleddefaultmode <status|thermal|off>     - Mode after boot animation
oledbootduration <ms>           - Boot animation duration (500-10000ms)
oledrequireauth <0|1>           - Require login to use OLED menu
oledenabled <0|1>               - Enable/disable OLED
oledthermalscale <1.0-10.0>     - Thermal image scale on OLED
oledthermalcolormode <3level|grayscale|binary>  - Thermal color mode on OLED
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
ledstartupcolor <color>         - Set startup primary color
ledstartupcolor2 <color>        - Set startup secondary color
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
</details>

<details>
<summary><strong>automation - Scheduled & conditional commands</strong></summary>

```
automation                      - Show automation system status
automationlist                  - List all automations
automationadd                   - Add an automation (JSON)
automationrun id=<id>           - Run automation immediately by ID
automation add type=event on=<kind> [match=<text>] - Event-triggered automation
autolog start <file>            - Start automation execution log
autolog stop                    - Stop automation log
autolog status                  - Show log status
validate-conditions <expr>      - Validate conditional syntax (e.g. IF temp>75 THEN ledcolor red)
print <message>                 - Broadcast a message to all output channels
```
</details>

<details>
<summary><strong>settings - Device configuration</strong></summary>

```
wifiautoreconnect <0|1>         - WiFi auto-reconnect on disconnect
ntpserver <hostname>            - Set NTP server
tzoffsetminutes <-720..720>     - Set timezone offset in minutes
httpAutoStart <0|1>             - Auto-start web server on boot
httpsEnabled <0|1>              - Enable HTTPS (reboot required)
webclihistorysize <1-100>       - Web CLI history buffer size
oledclihistorysize <10-100>     - OLED CLI history buffer size
beginwrite                      - Start a batch settings update (defers flash write)
savesettings                    - Flush deferred settings to flash
features                        - Show/toggle system features with heap estimates
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
userdelete <user>               - Delete user
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
sensorlog format <text|csv|track>         - text/csv for multi-sensor; track = GPS-only
sensorlog maxsize <bytes>                 - Max file size before rotation
sensorlog rotations <count>               - Old logs to keep (0-9)
sensorlog sensors <list|all|none>         - Replace mask: thermal,tof,imu,gamepad,apds,gps,presence,r1
sensorlog interval <ms>                   - Poll interval (100-3600000)
sensorlog autostart [on|off]              - Auto-start on boot with last-used path/mask
```

**R1 Health Track (preferred):** `healthtrack on` (or Apps -> Health / OLED R1 Health /
Web R1 Health -> Track) enables R1 in the sensorlog mask, starts capture under
`/logging_captures/sensors/`, and persists so boot resumes. The exact path is
shaped per session: a dated per-day file (`YYYY-MM-DD/health-YYYY-MM-DD.csv`)
once the clock is set, or a `boot-NNNNNN/` subfolder while it is still dark -
those are retro-dated automatically when real time arrives.
While Track is on, the ring is polled/mined on a timer (default **900 s / 15 min**)
and **only those mines write rows** (plus Poll Now / opening Health). There are no
5 s empty timestamp heartbeats in R1-only Track sessions. Adjust with
`healthtrack interval <sec>` or setting `healthTrackPollIntervalSec` (60-86400).
`healthtrack off` removes R1; stops logging if nothing else is selected.
Settings: `logging.sensorlog` -> **R1 Health Track** / **R1 Health poll interval (sec)**.

**Surfaces** (requires `ENABLE_R1_HEALTH`): G2 Apps -> Health (graphs), OLED **R1 Health**,
Web **`/r1-health`**. Ring connect remains under Bluetooth (OLED / Web). Snapshot CLI
for Web and a future Bluetooth App: `healthstatus` / `healthstatus json` / `healthstatus poll`.
Stitch split captures: `healthlogmerge "<out>" "<in1>" "<in2>" ...` (same idea as `gpstrackmerge`).
**Output goes first and is truncated** - passing it last overwrites a real capture. The merge is a
byte concatenation: it does not reorder rows by time, does not drop the header lines of later CSV
inputs, and does not check that the inputs share a format or sensor mask. Per-day append means
same-day sessions already land in one file, so stitching is only for spanning days.

**Keep the ring up:** `bleautoreconnect r1-ring on` reconnects at boot **and** reseeks after
unexpected drops (backoff). While Health Track is on, each due mine (default 15 min)
also nudges a ring reseek if the link is down (one non-blocking connect attempt;
not a continuous scan). `ringdisconnect` / `closeg2` do not reseek.

You can still use raw `sensorlog sensors r1` + `sensorlog start ...` if you want
manual control without the Health Track product switch (that path still uses the
normal sensorlog interval / heartbeats).
</details>

<details>
<summary><strong>llm - On-device LLM inference (requires ENABLE_ONDEVICE_LLM)</strong></summary>

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
waypoint list                   - List all waypoints
waypoint add <name>             - Add waypoint at current GPS location
waypoint del <name>             - Delete waypoint
waypoint goto <name>            - Navigate to waypoint
waypoint clear                  - Clear all waypoints
waypointfile <file> <wpName>    - Link a file to a waypoint
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
power                           - Show power mode status
power mode <mode>               - Set power mode
power auto                      - Enable automatic power management
power threshold <value>         - Set power threshold
```
</details>

<details>
<summary><strong>battery - Battery monitoring (requires ENABLE_BATTERY_MONITOR)</strong></summary>

```
batterystatus                   - Show voltage, charge level, and status
batterycalibrate                - Recalibrate/re-probe the sensor (ADC characterize or fuel-gauge re-probe)
batterylog [on|off|interval <s>|tail|clear]  - Battery time-series CSV log
```
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
<summary><strong>camera - DVP camera (ESP32-S3 only, requires ENABLE_CAMERA_SENSOR)</strong></summary>

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
cameraautocapture <on|off>      - Auto-capture on schedule
cameraautocaptureinterval <sec> - Auto-capture interval
camerasendaftercapture <on|off> - Send image via ESP-NOW after capture
cameratargetdevice <name>       - Target device for post-capture send
camerastoragelocation <0-2>     - Storage destination (LittleFS/SD/both)
cameracapturefolder <path>      - Folder for captured images
cameramaxstoredimages <0-1000>  - Max images to keep
cameratiny                      - Capture a small frame (for ESP-NOW transfer)
```
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
voicecancel                     - Cancel current voice command sequence
voicehelp                       - Show available voice options for current state

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
srtuninggain <0.1-10.0>         - Set AFE linear gain
srtuningagc <0-3>               - Set AGC mode (0=off)
srtuningvad <0-4>               - Set VAD sensitivity
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
closeg2                         - Disconnect from G2 glasses
g2status                        - Show connection status
g2scan                          - Scan for G2 glasses
g2show <text>                   - Display text on G2 glasses
g2clear                         - Clear G2 display
g2init                          - Initialize G2 client mode (disables BLE server)
g2deinit                        - Deinitialize G2 client mode
g2nav [on|off]                  - Map G2 gestures to OLED menu navigation
g2verbose [on|off]              - Toggle verbose packet logging
g2health                        - Open Apps -> Health (R1 vitals + graphs) on the lens (ENABLE_R1_HEALTH)
g2pet                           - Open Apps -> Pet on the lens
```

**Apps -> Health** (lens, `ENABLE_R1_HEALTH`): left list (Overview / Trends / Heart Rate / HRV /
SpO2 / Temperature / Battery / Poll Now / Toggle Track). **Overview** shows native-text
vitals (wear + one shared recentness on the status line); live metric rows show
title/value/age above a line graph. **Trends** opens a submenu (HR/HRV/SpO2 today +
Refresh) that graphs the ring's daily-history payload separately from the live
sparklines (weekly aggregation later).
**Toggle Track** starts Health Track logging (`healthtrack on`); while on, the ring is
mined every `healthTrackPollIntervalSec` (default 15 min). Opening the page or **Poll Now**
also logs a sample when R1 logging is active. Pair the R1 under Bluetooth -> R1 Ring;
vitals also on OLED **R1 Health** and Web **`/r1-health`**.
Ring/Health CLI: `ringconnect`, `ringstatus`, `ringquery hr|temp|wear`,
`healthstatus [json|poll]`, `healthtrack status|interval`. A Bluetooth App can call the
same commands over GATT (no dedicated phone UI yet).
3-pane list+text+image experiments: Tests -> Image -> Streaming -> Compound -> Q30*.
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
rtcset <datetime|timestamp>     - Set RTC time
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
(`0`=none, `1`=Seesaw gamepad, `2`=ANO rotary encoder). The two are mutually
exclusive - both hang off STEMMA QT and would collide. Start/stop and
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
Tune, seek up/down, set volume, mute. `fmradio tune <MHz>` - e.g., `fmradio tune 101.5`.

### Even Realities G2 Glasses
BLE client for Even G2 temples (`openg2` / hijack menus / Apps -> Health). Requires
`ENABLE_G2_GLASSES`. See the `g2` / `g2health` / `ring*` command sections above.
R1 ring connect and vitals share the same Bluetooth central stack.

---

## License

MIT License

---

> ## Quick start: [QUICKSTART.md](QUICKSTART.md)
> ## Overview: [README](../README.md)
