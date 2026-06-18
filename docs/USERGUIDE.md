# Hardware One v0.95.7 - User Guide

This is the full reference for Hardware One. It covers every subsystem, all CLI commands, configuration options, and how the major features work. For initial setup, see the [Quick Start Guide](QUICKSTART.md).

## Table of Contents

- [Build Configuration](#build-configuration)
- [Board Reference](#board-reference)
- [Web UI](#web-ui)
- [OLED Interface](#oled-interface)
- [ESP-NOW Mesh](#esp-now-mesh)
- [Automations](#automations)
- [MQTT](#mqtt)
- [On-Device LLM](#on-device-llm)
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
| `ENABLE_G2_GLASSES` | `0` | Even Realities G2 BLE client (requires `ENABLE_BLUETOOTH=1`) - **planned, not yet working** |
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

- **Sensors** - live sensor data, start/stop individual sensors, logging controls
- **ESP-NOW** - peer list, pairing, bonding, metadata sync, file transfer, mesh status
- **Pair** - guided pairing/bonding wizard for connecting two devices
- **Maps** - offline map viewer, waypoint management, GPS track logging (requires `ENABLE_MAPS`)
- **Bluetooth** - BLE connection status and controls (requires `ENABLE_BLUETOOTH`)
- **MQTT** - broker configuration, topic preview, Home Assistant status (requires `ENABLE_MQTT`)
- **Settings** - all device settings, debug flags, user management
- **LLM** - on-device language model chat interface: load/unload model, ask questions, adjust temperature and sampling settings (requires `ENABLE_ONDEVICE_LLM`)
- **CLI** - full command interface in the browser, with history

Authentication is required. Default credentials are set on first boot via the setup wizard or the `users` CLI commands.

---

## OLED Interface

The OLED displays a menu system navigated with the Seesaw gamepad (joystick + buttons). On first boot a setup wizard runs to configure WiFi, device name, room, and zone. After that it goes to the main menu.

Main menu sections:
- **Network** - WiFi status, ESP-NOW peer list, connect/disconnect
- **Sensors** - per-sensor live readout and start/stop
- **System** - memory, uptime, IP address, reboot
- **Settings** - brightness, display timeout, output routing
- **Logging** - view recent log entries
- **Power** - battery level (if enabled), sleep controls

---

## ESP-NOW Mesh

ESP-NOW V3 is Hardware One's inter-device wireless protocol. Devices pair with a shared passphrase and form an encrypted mesh.

### Pairing
1. On both devices, go to the **Pair** tab in the web UI (or use `espnow pair` CLI).
2. Set the same passphrase on both devices.
3. One device initiates - the other accepts.
4. Once paired, devices appear in each other's peer list.

### Bonding (Master/Worker)
With `ENABLE_BONDED_MODE=1`, two devices can bond into a master/worker pair. The master gains a **Remote** tab in its web UI showing the worker's features, even if those features aren't compiled into the master.

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
| `debugespnow` | ESP-NOW general |
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

Type `help` on the device to enter the interactive help system. Type a module name to see its commands. Type `help all` to include disconnected sensors.

<details>
<summary><strong>core - System commands</strong></summary>

```
status                          - Show system status (WiFi, FS, memory)
uptime                          - Show device uptime
time                            - Show current time (uptime + NTP if synced)
timeset <YYYY-MM-DD HH:MM:SS>   - Set time manually (or unix timestamp)
reboot                          - Restart the device
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
taskstats                       - Detailed FreeRTOS task statistics
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

```
userlist                        - List all users
useradd <user> <pass> [0|1]     - Create user (1 = admin)
userdelete <user>               - Delete user
userchangepassword <cur> <new> <confirm>  - Change own password
userresetpassword <user> <pass> [0|1]     - Reset another user's password (admin)
userpromote <user>              - Grant admin role
userdemote <user>               - Remove admin role
userrequest <user> <pass>       - Request a new account (self-registration)
userapprove <user>              - Approve a pending account request (admin)
userdeny <user>                 - Deny a pending account request (admin)
pendinglist                     - List pending account requests
usersync <user> <target>        - Sync user credentials to an ESP-NOW peer (admin)
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

```
sensorlog start <sensor>        - Start logging sensor data to CSV on LittleFS
sensorlog stop <sensor>         - Stop logging
sensorlog status                - Show active log files
sensorlog format <sensor>       - Set log format
sensorlog maxsize <sensor>      - Set max log file size
sensorlog rotations <sensor>    - Set number of rotation files kept
sensorlog sensors               - List loggable sensors
```
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
battery status                  - Show voltage, estimated charge level, and status
battery calibrate               - Recalibrate ADC voltage readings
```
</details>

<details>
<summary><strong>images - Image capture and management</strong></summary>

```
capture [littlefs|sd|both]      - Capture and save an image
images [littlefs|sd]            - List saved images
imagedelete "<path>"            - Delete an image
imagesend <device> ["<path>"]   - Send image to a peer via ESP-NOW
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
camerarotate <on|off>           - Rotate 180°
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
<summary><strong>speech - ESP-SR voice recognition (requires ENABLE_ESPSR)</strong></summary>

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
<summary><strong>g2 - Even Realities G2 glasses (requires ENABLE_G2_GLASSES) - ⚠️ planned, not yet working</strong></summary>

> **Status: not functional yet.** These commands compile and the scaffolding is
> in place, but the G2 BLE client is a work-in-progress goal - connection,
> display, and gesture handling are not reliable. Documented here as the
> intended command surface once the feature lands.

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
```
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
thermalrotation <0-3>           - Rotate thermal image (0=0°, 1=90°, 2=180°, 3=270°)
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
<summary><strong>gamepad - Seesaw gamepad</strong></summary>

```
opengamepad                     - Start gamepad
closegamepad                    - Stop gamepad
gamepadread                     - Read joystick axes and button states
gamepadautostart [on|off]       - Auto-start on boot
```
</details>

---

## Per-Module Notes

### VL53L4CX (ToF)
Supports up to 4 simultaneous distance measurements. Range up to 6m. Polling rate configurable via settings (`tofPollingMs`, default 220ms).

### MLX90640 (Thermal)
32×24 IR thermal camera. Web UI displays interpolated heatmap with HSL color mapping. Configurable palette, interpolation steps, and frame rate. High memory footprint - uses PSRAM.

### BNO055 (IMU)
9-DoF orientation (accel + gyro + magnetometer fusion). Orientation correction configurable via settings (`imuOrientationMode`, `imuPitchOffset`, etc.).

### APDS9960
Three independent modes: color/RGB (`apdscolor`), proximity (`apdsproximity`), gesture up/down/left/right (`apdsgesture`). Each can be run independently.

### GPS (PA1010D)
NMEA output parsed for lat/lon/speed/heading. Track logging to LittleFS. Offline map viewer in web UI.

### FM Radio (RDA5807)
Tune, seek up/down, set volume, mute. `fmradio tune <MHz>` - e.g., `fmradio tune 101.5`.

### Even Realities G2 Glasses *(planned - not yet working)*
> **Status: work in progress.** This is a goal feature, not a working one yet.
> The intention is a BLE client that connects to G2 glasses, sends display text
> via their teleprompter protocol, and maps glasses gestures to OLED menu
> navigation - mutually exclusive with phone BLE server mode at runtime. The
> CLI commands exist as placeholders but the underlying protocol is still being
> developed. Do not expect this to work on a current build.

---

## License

MIT License

---

> ## Quick start: [QUICKSTART.md](QUICKSTART.md)
> ## Overview: [README](../README.md)
