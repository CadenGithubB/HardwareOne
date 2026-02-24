# Hardware One — User Guide

This is the full reference for Hardware One. It covers every subsystem, all CLI commands, configuration options, and how the major features work. For initial setup, see the [Quick Start Guide](QUICKSTART.md).

## Table of Contents

- [Build Configuration](#build-configuration)
- [Board Reference](#board-reference)
- [Web UI](#web-ui)
- [OLED Interface](#oled-interface)
- [ESP-NOW Mesh](#esp-now-mesh)
- [Automations](#automations)
- [MQTT](#mqtt)
- [Debug Flags](#debug-flags)
- [Command Reference](#command-reference)
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
| `DISPLAY_TYPE` | `1` (SSD1306) | `0`=none, `1`=SSD1306 OLED, `2`=ST7789 TFT, `3`=ILI9341 TFT |
| `ENABLE_BLUETOOTH` | `0` | BLE server with GATT services |
| `ENABLE_G2_GLASSES` | `0` | Even Realities G2 BLE client (requires `ENABLE_BLUETOOTH=1`) |
| `ENABLE_MQTT` | `1` | Home Assistant MQTT integration |
| `ENABLE_AUTOMATION` | `1` | Scheduled tasks and conditional commands |
| `ENABLE_CAMERA_SENSOR` | `0` | ESP32-S3 DVP camera (OV2640/OV5640) |
| `ENABLE_MICROPHONE_SENSOR` | `0` | PDM microphone via I2S |
| `ENABLE_BATTERY_MONITOR` | `0` | LiPo voltage monitoring via ADC |
| `ENABLE_EDGE_IMPULSE` | `0` | Edge Impulse ML inference |
| `ENABLE_PAIRED_MODE` | `0` | Two-device bonded pair (master/worker) |

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

> If a module is enabled in the config but not physically connected, its commands will fail gracefully — the rest of the system is unaffected.

---

## Board Reference

See [BOARD_SWITCHING.md](BOARD_SWITCHING.md) for full menuconfig tables. Key differences:

| | ESP32 (QT PY, Feather) | ESP32-S3 (XIAO, QT PY S3) |
| - | ---------------------- | ------------------------- |
| PSRAM | Quad SPI, 40 MHz | Octal SPI, 80 MHz |
| Bluetooth | Classic BT + BLE 4.2 | BLE 5.0 only |
| Camera/Mic | No | Yes (S3 only) |
| USB | UART bridge (`usbserial`) | Native USB (`usbmodem`) |
| `set-target` | `esp32` | `esp32s3` |

When switching between chip families: `idf.py fullclean` then `idf.py set-target <chip>`. Wrong PSRAM mode = boot failure.

---

## Web UI

Navigate to the device's IP address in a browser. The web server must be running (`webstart` or `webauto on`).

- **Sensors** — live sensor data, start/stop individual sensors, logging controls
- **ESP-NOW** — peer list, pairing, bonding, metadata sync, file transfer, mesh status
- **Pair** — guided pairing/bonding wizard for connecting two devices
- **Maps** — offline map viewer, waypoint management, GPS track logging (requires `ENABLE_MAPS`)
- **Bluetooth** — BLE connection status and controls (requires `ENABLE_BLUETOOTH`)
- **MQTT** — broker configuration, topic preview, Home Assistant status (requires `ENABLE_MQTT`)
- **Settings** — all device settings, debug flags, user management
- **CLI** — full command interface in the browser, with history

Authentication is required. Default credentials are set on first boot via the setup wizard or the `users` CLI commands.

---

## OLED Interface

The OLED displays a menu system navigated with the Seesaw gamepad (joystick + buttons). On first boot a setup wizard runs to configure WiFi, device name, room, and zone. After that it goes to the main menu.

Main menu sections:
- **Network** — WiFi status, ESP-NOW peer list, connect/disconnect
- **Sensors** — per-sensor live readout and start/stop
- **System** — memory, uptime, IP address, reboot
- **Settings** — brightness, display timeout, output routing
- **Logging** — view recent log entries
- **Power** — battery level (if enabled), sleep controls

---

## ESP-NOW Mesh

ESP-NOW V3 is Hardware One's inter-device wireless protocol. Devices pair with a shared passphrase and form an encrypted mesh.

### Pairing
1. On both devices, go to the **Pair** tab in the web UI (or use `espnow pair` CLI).
2. Set the same passphrase on both devices.
3. One device initiates — the other accepts.
4. Once paired, devices appear in each other's peer list.

### Bonding (Master/Worker)
With `ENABLE_PAIRED_MODE=1`, two devices can bond into a master/worker pair. The master gains a **Remote** tab in its web UI showing the worker's features, even if those features aren't compiled into the master.

### Metadata Sync
Each device has a name, room, zone, and tags set in settings. The **Metadata** tab lets you pull this information from any peer. Set your own device metadata with:
```
espnow setname <name>
espnow setroom <room>
espnow setzone <zone>
espnow settags <tags>
```

### File Transfer
Files can be transferred between paired devices via the web UI or CLI (`espnow sendfile`). Used for syncing automations, settings, and manifests.

---

## Automations

Automations are scheduled or conditional command sequences stored on the device. They run locally — no internet required.

### Syntax
```
NAME: <name>
SCHEDULE: <time_or_interval>
IF <condition> THEN <command>; <command>
```

### Schedule formats
- `TIME=HH:MM` — run at a specific time daily
- `INTERVAL=Xs` / `Xm` / `Xh` — repeat every N seconds/minutes/hours
- `BOOT` — run once on startup

### Conditions
```
IF TEMP>75 THEN ledcolor red
IF TIME=EVENING THEN ledbrightness 30
IF ROOM=Kitchen THEN PRINT Kitchen automation triggered
IF ZONE=Upstairs AND TIME=NIGHT THEN ledbrightness 10
IF TAGS CONTAINS outdoor THEN PRINT Outdoor device
```

Supported operators: `>`, `<`, `=`, `!=`, `CONTAINS` (for TAGS field only).  
Metadata values (`ROOM`, `ZONE`, `TAGS`) come from `gSettings.espnowRoom/Zone/Tags`. If not set, value is `"NONE"`.

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
| `debugespnow` | ESP-NOW general |
| `debugespnowcore` | ESP-NOW V3 frame layer |
| `debugespnowmesh` | Mesh peer management |
| `debugespnowrouter` | Message routing |
| `debugespnowstream` | Stream output |
| `debugespnowmetadata` | Metadata REQ/RESP/PUSH pipeline |
| `debugmqtt` | MQTT publish/subscribe |
| `debugautomations` | Automation scheduling and execution |
| `debugsensors` | Sensor polling and data |
| `debugstorage` | Filesystem read/write |
| `debugcli` | Command execution flow |
| `debugauth` | Authentication events |
| `debugperformance` | Timing and performance metrics |
| `debugsystem` | System events |
| `debugusers` | User management |

---

## Command Reference

Type `help` on the device to enter the interactive help system. Type a module name to see its commands. Type `help all` to include disconnected sensors.

<details>
<summary><strong>core — System commands</strong></summary>

```
status              - Show system status
uptime              - Show system uptime
memory              - Show heap/PSRAM usage
memsum              - One-line memory summary
memreport           - Comprehensive memory report
memtrack <on|off|reset|status>
psram               - Show PSRAM usage details
reboot              - Restart the device
clear               - Clear CLI history
broadcast <message> - Send message to all users (admin)
```
</details>

<details>
<summary><strong>wifi — Network management</strong></summary>

```
wifi                - Show WiFi status
wifi connect        - Connect to saved network
wifi disconnect     - Disconnect
wifi scan           - Scan for nearby access points
wifi on             - Enable WiFi
wifi off            - Disable WiFi
setssid <ssid>      - Save WiFi SSID
setpass <pass>      - Save WiFi password
webauto <on|off>    - Auto-start web server on boot
webstart            - Start web server
webstop             - Stop web server
webstatus           - Show web server status and IP
synctime            - Sync time from NTP server
time                - Show current time
```
</details>

<details>
<summary><strong>espnow — ESP-NOW mesh</strong></summary>

```
espnow status       - Show ESP-NOW status and peer list
espnow pair         - Start pairing with a peer
espnow unpair <mac> - Remove a paired peer
espnow send <mac> <msg>  - Send text message to peer
espnow sendfile <mac> <path>  - Send file to peer
espnow requestmeta <mac>  - Request metadata from peer
espnow setname <name>     - Set this device's name
espnow setroom <room>     - Set room identifier
espnow setzone <zone>     - Set zone identifier
espnow settags <tags>     - Set comma-separated tags
espnow peers        - List all known peers
espnow mesh         - Show mesh topology
```
</details>

<details>
<summary><strong>mqtt — MQTT broker</strong></summary>

```
mqtt status         - Show connection status
mqtt connect        - Connect to broker
mqtt disconnect     - Disconnect from broker
mqtt broker <host>  - Set broker hostname/IP
mqtt port <port>    - Set broker port
mqtt user <user>    - Set username
mqtt pass <pass>    - Set password
mqtt topic <prefix> - Set topic prefix
```
</details>

<details>
<summary><strong>bluetooth — BLE</strong></summary>

```
bluetooth status    - Show BLE status
bluetooth on        - Enable BLE
bluetooth off       - Disable BLE
bluetooth scan      - Scan for BLE devices
bluetooth connect <addr>  - Connect to device
bluetooth disconnect      - Disconnect
```
</details>

<details>
<summary><strong>filesystem — File operations</strong></summary>

```
fsusage             - Show filesystem usage
files [path]        - List files (default '/')
mkdir <path>        - Create directory
rmdir <path>        - Remove empty directory
filecreate <path>   - Create empty file
fileview <path>     - View file contents
filedelete <path>   - Delete file
```
</details>

<details>
<summary><strong>oled — Display control</strong></summary>

```
oled on             - Turn OLED on
oled off            - Turn OLED off
oled brightness <0-255>  - Set brightness
oled menu           - Open main menu
oled page <name>    - Go to named page
```
</details>

<details>
<summary><strong>neopixel — RGB LED</strong></summary>

```
ledcolor <color>    - Set onboard NeoPixel color
ledcolor off        - Turn off NeoPixel
```
</details>

<details>
<summary><strong>i2c — I2C bus</strong></summary>

```
i2c scan            - Scan I2C bus and detect devices
i2c speed <hz>      - Set I2C bus speed
i2c reset           - Reset I2C bus
i2c status          - Show I2C bus status
```
</details>

<details>
<summary><strong>automation — Automations</strong></summary>

```
automation list     - List all automations
automation add <json>     - Add automation
automation delete <name>  - Delete automation
automation run <name>     - Run immediately
automation enable <name>  - Enable
automation disable <name> - Disable
automation export   - Export all as JSON
```
</details>

<details>
<summary><strong>settings — Device configuration</strong></summary>

```
settings list       - Show all settings
settings get <key>  - Get a setting value
settings set <key> <value>  - Set a value
settings reset      - Reset all to defaults
settings export     - Export settings as JSON
```
</details>

<details>
<summary><strong>users — User management (admin)</strong></summary>

```
users list          - List all users
users add <user> <pass>   - Add user
users delete <user>       - Delete user
users passwd <user> <pass> - Change password
users promote <user>      - Grant admin
users demote <user>       - Remove admin
```
</details>

<details>
<summary><strong>debug — Debug flags</strong></summary>

```
debug list          - Show all debug flags and current state
debug<flagname> 1   - Enable flag (persistent)
debug<flagname> 1 temp  - Enable flag (runtime only)
debug<flagname> 0   - Disable flag
```
See [Debug Flags](#debug-flags) section for the full flag list.
</details>

<details>
<summary><strong>sensorlog — Sensor data logging</strong></summary>

```
sensorlog start <sensor>   - Start logging sensor to file
sensorlog stop <sensor>    - Stop logging
sensorlog list             - List active log files
sensorlog view <sensor>    - View recent log entries
sensorlog clear <sensor>   - Clear log file
```
</details>

<details>
<summary><strong>battery — Battery monitoring</strong></summary>

```
battery status      - Show voltage, charge level, status
battery calibrate   - Recalibrate ADC readings
```
</details>

<details>
<summary><strong>Sensors — all use open/close/read pattern</strong></summary>

All sensor modules follow a consistent command pattern:
- `open<sensor>` — start the sensor (e.g., `openthermal`, `opentof`, `openimu`)
- `close<sensor>` — stop the sensor
- `<sensor>read` — take a single reading (e.g., `tofread`, `imuread`, `gpsread`)
- `<sensor>autostart <on|off>` — auto-start on boot

| Module | Sensor | Key commands |
| ------ | ------ | ------------ |
| `thermal` | MLX90640 32×24 thermal camera | `openthermal`, `closethermal`, `thermalautostart` |
| `tof` | VL53L4CX Time-of-Flight (up to 6m, multi-object) | `opentof`, `closetof`, `tofread` |
| `imu` | BNO055 9-DoF orientation | `openimu`, `closeimu`, `imuread` |
| `gamepad` | Seesaw gamepad | `opengamepad`, `closegamepad` |
| `apds` | APDS9960 gesture/proximity/RGB | `apdscolor`, `apdsproximity`, `apdsgesture` |
| `gps` | PA1010D GPS | `opengps`, `closegps`, `gpsread` |
| `rtc` | DS3231 precision RTC | `openrtc`, `closertc`, `rtcread` |
| `presence` | STHS34PF80 IR presence/motion | `openpresence`, `closepresence`, `presenceread` |
| `fmradio` | RDA5807 FM radio | `openfmradio`, `closefmradio`, `fmradio tune <MHz>` |
| `camera` | DVP camera (ESP32-S3 only) | `opencamera`, `closecamera`, `cameraread` |
| `microphone` | PDM microphone (ESP32-S3 only) | `openmicrophone`, `closemicrophone`, `micread` |
| `edgeimpulse` | Edge Impulse ML inference | `edgeimpulse run`, `edgeimpulse status` |
| `servo` | PCA9685 servo controller | `servo <channel> <angle>` |
</details>

---

## Per-Module Notes

### VL53L4CX (ToF)
Supports up to 4 simultaneous distance measurements. Range up to 6m. Polling rate configurable via settings (`tofPollingMs`, default 220ms).

### MLX90640 (Thermal)
32×24 IR thermal camera. Web UI displays interpolated heatmap with HSL color mapping. Configurable palette, interpolation steps, and frame rate. High memory footprint — uses PSRAM.

### BNO055 (IMU)
9-DoF orientation (accel + gyro + magnetometer fusion). Orientation correction configurable via settings (`imuOrientationMode`, `imuPitchOffset`, etc.).

### APDS9960
Three independent modes: color/RGB (`apdscolor`), proximity (`apdsproximity`), gesture up/down/left/right (`apdsgesture`). Each can be run independently.

### GPS (PA1010D)
NMEA output parsed for lat/lon/speed/heading. Track logging to LittleFS. Offline map viewer in web UI.

### FM Radio (RDA5807)
Tune, seek up/down, set volume, mute. `fmradio tune <MHz>` — e.g., `fmradio tune 101.5`.

### Even Realities G2 Glasses
BLE client that connects to G2 glasses. Sends display text via teleprompter protocol. Gesture input from glasses maps to OLED menu navigation. Mutually exclusive with phone BLE server mode at runtime.

---

## License

MIT License

---

> ## Quick start: [QUICKSTART.md](QUICKSTART.md)
> ## Overview: [README](../README.md)
