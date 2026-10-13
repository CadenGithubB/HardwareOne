// Compiled command-module catalog. Module implementations own their command
// tables; this file owns their grouping, help order, and availability callbacks.
// Command lookup, execution, and authorization keep their existing owners.
#include "System_CommandRegistry.h"
#include "System_BuildConfig.h"
#include "System_RadioCompanion.h"  // HW1_RADIO_COMPANION and radioCompanionOnline
#include "System_I2C.h"             // isSensorConnected
#if ENABLE_SD_CARD
#include "System_VFS.h"
#endif
#if ENABLE_OLED_INPUT || ENABLE_GAMEPAD_SENSOR
#include "HAL_Input.h"
#endif
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"
#endif
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"
#endif
#if ENABLE_MICROPHONE
#include "HAL_Audio.h"
#endif

// Only command-table exports are needed from the remaining modules. Keeping
// these declarations local avoids importing their drivers and runtime state.
// Disabled modules need no definition: only compiled catalog rows refer to them.
extern const CommandEntry cliCommands[];
extern const size_t cliCommandsCount;
extern const CommandEntry commands[];
extern const size_t commandsCount;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;
extern const CommandEntry espNowCommands[];
extern const size_t espNowCommandsCount;
extern const CommandEntry companionCommands[];
extern const size_t companionCommandsCount;
extern const CommandEntry mqttCommands[];
extern const size_t mqttCommandsCount;
extern const CommandEntry bluetoothCommands[];
extern const size_t bluetoothCommandsCount;
extern const CommandEntry filesystemCommands[];
extern const size_t filesystemCommandsCount;
extern const CommandEntry sdCommands[];
extern const size_t sdCommandsCount;
extern const CommandEntry oledCommands[];
extern const size_t oledCommandsCount;
extern const CommandEntry neopixelCommands[];
extern const size_t neopixelCommandsCount;
extern const CommandEntry ledCommands[];
extern const size_t ledCommandsCount;
extern const CommandEntry matrixCommands[];
extern const size_t matrixCommandsCount;
extern const CommandEntry servoCommands[];
extern const size_t servoCommandsCount;
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;
extern const CommandEntry imuCommands[];
extern const size_t imuCommandsCount;
extern const CommandEntry inputCommands[];
extern const size_t inputCommandsCount;
extern const CommandEntry gamepadCommands[];
extern const size_t gamepadCommandsCount;
extern const CommandEntry anoEncoderCommands[];
extern const size_t anoEncoderCommandsCount;
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;
extern const CommandEntry rtcCommands[];
extern const size_t rtcCommandsCount;
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;
extern const CommandEntry cameraCommands[];
extern const size_t cameraCommandsCount;
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;
extern const CommandEntry edgeImpulseCommands[];
extern const size_t edgeImpulseCommandsCount;
extern const CommandEntry transcriptionUICommands[];
extern const size_t transcriptionUICommandsCount;
extern const CommandEntry noteCommands[];
extern const size_t noteCommandsCount;
extern const CommandEntry sttCommands[];
extern const size_t sttCommandsCount;
extern const CommandEntry espsrCommands[];
extern const size_t espsrCommandsCount;
extern const CommandEntry i2cCommands[];
extern const size_t i2cCommandsCount;
extern const CommandEntry automationCommands[];
extern const size_t automationCommandsCount;
extern const CommandEntry batteryCommands[];
extern const size_t batteryCommandsCount;
extern const CommandEntry debugCommands[];
extern const size_t debugCommandsCount;
extern const CommandEntry settingsCommands[];
extern const size_t settingsCommandsCount;
extern const CommandEntry sensorLoggingCommands[];
extern const size_t sensorLoggingCommandsCount;
extern const CommandEntry healthCommands[];
extern const size_t healthCommandsCount;
extern const CommandEntry userSystemCommands[];
extern const size_t userSystemCommandsCount;
extern const CommandEntry featureCommands[];
extern const size_t featureCommandsCount;
extern const CommandEntry imageCommands[];
extern const size_t imageCommandsCount;
extern const CommandEntry mapCommands[];
extern const size_t mapCommandsCount;
extern const CommandEntry mapsSettingCommands[];
extern const size_t mapsSettingCommandsCount;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;
extern const CommandEntry liveAudioCommands[];
extern const size_t liveAudioCommandsCount;
extern const CommandEntry cm5PresenceCommands[];
extern const size_t cm5PresenceCommandsCount;
extern const CommandEntry otaCommands[];
extern const size_t otaCommandsCount;
extern const CommandEntry setPatternCommands[];
extern const size_t setPatternCommandsCount;
extern const CommandEntry g2Commands[];
extern const size_t g2CommandsCount;
extern const CommandEntry g2RingCommands[];
extern const size_t g2RingCommandsCount;
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;
extern const CommandEntry settingEditorCommands[];
extern const size_t settingEditorCommandsCount;

// Module registry - collects all command tables from modules
// Now includes metadata: description, flags, isConnected callback
// Order matters for help display; longest-match search handles conflicts
// Columns: name, description, long description, commands, count pointer,
// flags, isConnected. Count addresses are link-time constants, so this whole
// build-specific registry remains in flash instead of writable DRAM.
static constexpr CommandModule gCommandModules[] = {
  { "cli",        "Help and CLI navigation", "The cli module is the on-device help and CLI navigation layer, not a feature "
    "subsystem. help opens a paged help browser: bare help shows the main menu listing "
    "every registered module, help <module> drills into page 1 of that module (and "
    "prints its subsystem overview at the top), and help <module> p<N> opens an "
    "explicit later page. The special topics help "
    "sensors (aggregate view across all sensor modules), help all (show every command "
    "including hidden ones), and help tail (dump suppressed output) cover the rest. "
    "While the browser is open the CLI is in a help state, so p<N>, next and prev move "
    "within a module, back steps from a module page up to the main menu, and exit leaves "
    "help mode entirely and returns to the normal "
    "prompt, and clear wipes the CLI scrollback/history.", cliCommands,          &cliCommandsCount, CMD_MODULE_CORE, nullptr },
  { "system",     "Core system commands", "The system module holds core device commands that do not belong to any peripheral. "
    "Status and inspection: status (WiFi, filesystem, memory summary), uptime, time "
    "(uptime plus NTP wall-clock if synced), temperature and voltage (ESP32 internal "
    "die temp and supply rail), taskstats/perftop (FreeRTOS task and live loop/CPU "
    "profiling), fsusage, events (recent system events from the in-memory register "
    "that drives automation event triggers), and the memory tools memsample (snapshot, with memsample "
    "track on|off|reset|status for allocation tracking) and memreport. Control and "
    "power: reboot, ramflush, cpufreq [MHz] to read or set a supported CPU clock, lightsleep "
    "[seconds] for ESP32 light sleep, deepsleep for power-off (reset to wake), and "
    "wait <ms>/sleep <ms> to pause command-script execution. "
    "timeset sets the clock manually. broadcast <message> pushes a line of text to all "
    "connected output interfaces, and factoryreset deletes the user-accounts file so "
    "the first-boot setup wizard re-runs on next reboot while deliberately preserving "
    "WiFi credentials and other settings. Most mutating commands (timeset, cpufreq, "
    "reboot, ramflush, factoryreset, broadcast, lightsleep, deepsleep) require admin.", commands,             &commandsCount, 0, nullptr },
#if ENABLE_WIFI
  { "wifi",       "Network management (connect, scan, add/remove networks)", "The WiFi subsystem manages station-mode network connections plus the network "
    "services that ride on top of them: NTP time sync and the on-device HTTP/HTTPS "
    "server. Saved networks are stored as a prioritized list (wifilist, wifiadd, "
    "wifirm, wifipromote) and persist to flash; openwifi connects by best-priority "
    "(default) or by --index <N>, and a failed indexed attempt auto-rolls back to the "
    "previously connected network. Note two distinct disconnects: closewifi tears down "
    "the link AND stops the HTTP server and web output to free heap, while "
    "wifidisconnect (drop) leaves the radio and web server up so you can move to "
    "another network. closewifi powers the radio down only when ESP-NOW is not "
    "using it; 'radiopower off' is the unconditional airplane switch. wifiscan lists nearby APs, ntpsync/ntpstatus handle clock sync, "
    "and openhttp/closehttp/httpstatus run the web server (compiled in only when the "
    "HTTP server is enabled). certinfo and certgen (admin-only) manage the self-signed "
    "HTTPS certificate.", wifiCommands,         &wifiCommandsCount, 0, nullptr },
#endif
#if ENABLE_ESPNOW
  { "espnow",     "ESP-NOW wireless communication (peer-to-peer, mesh)",
    "ESP-NOW links HardwareOne devices directly over the WiFi radio with no router or "
    "access point, as named peers that can also form a multi-hop mesh. Pair with "
    "espnowpair, then message (espnowsend/espnowbroadcast), push a file (espnowsendfile), "
    "pull a file (espnowfetch), browse a peer's files (espnowbrowse), or run a command on "
    "a peer (espnowremote). espnowremote, espnowfetch, espnowbrowse, espnowroomcmd and "
    "espnowtagcmd are ASYNCHRONOUS: they return OK on delivery; the real result arrives "
    "later in the message buffer, read with 'espnowmessages json [mac]'. Mesh mode "
    "(espnowmode mesh) adds routing with a TTL and master/worker/backup roles; each "
    "device carries identity metadata (name, friendly name, room, zone, tags) queried "
    "with espnowdeviceinfo locally or espnowrequestmeta for a peer.",
    espNowCommands,       &espNowCommandsCount, 0, nullptr },
#endif
#if HW1_RADIO_COMPANION
  { "c6",         "ESP32-C6 radio companion (ESP-Hosted)",
    "On the ESP32-P4X-EYE the Wi-Fi, Bluetooth and ESP-NOW radio is a separate ESP32-C6 "
    "that the P4 drives over SDIO (ESP-Hosted). c6status reports its firmware, the ESP-NOW "
    "bridge, heartbeat, memory and recovery counters; c6restart resets it in place; c6hold "
    "turns the radio fully off by holding the C6 in reset and brings it back; c6update "
    "flashes a new companion image over SDIO with rollback protection and c6confirm accepts "
    "the running image; c6autorecover, c6autohold and c6heartbeat tune the background "
    "monitor. Radio features never start while the companion is offline, and a companion "
    "that stops answering is recovered without rebooting the P4.",
    companionCommands,    &companionCommandsCount, 0, radioCompanionOnline },
#endif
#if ENABLE_MQTT
  { "mqtt",       "MQTT broker connection for Home Assistant", "The MQTT subsystem connects the device to a broker, primarily to publish its "
    "sensor and system telemetry to Home Assistant via HA discovery. It is almost "
    "entirely configuration: broker host/port (mqttHost, mqttPort), credentials "
    "(mqttUser, mqttPassword), TLS mode and CA path, base/discovery topics, publish "
    "interval, and a long list of per-source publish toggles (mqttPublishThermal, "
    "mqttPublishIMU, and so on). These are persisted settings and most config commands "
    "are admin-only; after changing them, reconnect with closemqtt/openmqtt to apply to "
    "a live session. openmqtt and closemqtt start and stop the client, mqttstatus shows "
    "connection state, and mqttautostart controls whether it connects at boot. For "
    "inbound data, enable mqttSubscribeExternal with mqttSubscribeTopics; values "
    "received from those topics are cached and read back with mqttExternalSensors.", mqttCommands,         &mqttCommandsCount, 0, nullptr },
#endif
  #if ENABLE_BLUETOOTH
  { "bluetooth",  "Bluetooth LE control and status", "The Bluetooth subsystem runs the device BLE stack in one of two mutually exclusive "
    "roles selected by blemode: server mode (the device advertises and a phone/app "
    "connects to it) or client mode (the device acts as a BLE central for Even G2 "
    "glasses; the even_g2 commands then apply). Switching modes tears down the other "
    "role automatically. In server mode, openble/closeble start and stop advertising, "
    "blesend pushes a one-off message and bleevent an event to the connected client, "
    "and blestream toggles periodic pushes as a bitmask of sensors/system/events "
    "(blestream on/off/sensors/system/events, plus interval) -- all of which require an "
    "active connection. An app-layer Secure Channel (X25519 + passphrase + "
    "ChaCha20-Poly1305, independent of BLE bonding) is configured with blesecret and "
    "required with blesecure; both require super admin, as does blerequireauth. Boot "
    "reconnection to saved-MAC peers is per-peer via bleautoreconnect <name> [on|off] "
    "(see blepeers for names).", bluetoothCommands, &bluetoothCommandsCount, 0, nullptr },
  #endif
  { "filesystem", "File operations and storage management", "Manages files and directories on the device internal LittleFS flash. Browse with "
    "files [\"/path\"] (add json for app/BLE, or files stats json for storage usage); "
    "create and remove with mkdir, rmdir, filecreate, and filedelete; view and rename "
    "with fileview and filerename. Critically, every path argument MUST be wrapped in "
    "double quotes, e.g. fileview \"/system/notes\" -- an unquoted or unmatched-quote "
    "path is rejected, and a leading slash is added automatically. For programmatic "
    "transfer, fileread and filewrite move data in chunks: fileread returns "
    "{success,size,offset,len,eof,enc,data} and you loop offset until eof, while "
    "filewrite is strictly sequential -- offset 0 truncates/creates the file, each "
    "later offset must equal the current file size, and passing final runs the "
    "post-save hooks. Access is permission-gated per path: system trees like /system "
    "are read-only (or browse-only) for admins, while user data is fully writable; "
    "logtier reports whether logs are writing to LittleFS or have spilled into SD "
    "overflow.", filesystemCommands,   &filesystemCommandsCount, 0, nullptr },
#if ENABLE_SD_CARD
  { "sd",         "SD card mount, format, and info", "Controls the optional microSD card, which mounts at /sd and serves as "
    "overflow/bulk storage (and is only compiled in on boards that wire a "
    "supported SPI or SDMMC slot). sdmount attempts to mount the card and sdunmount safely "
    "unmounts it; sdinfo shows the card type, size, and used/free space, and sddiag "
    "reports backend-specific diagnostics to troubleshoot a card that will not mount. "
    "sdformat erases the entire card and reformats it as FAT32 and therefore requires "
    "sdformat confirm to proceed. Once mounted, file commands address the card through "
    "its /sd/... path prefix.", sdCommands,           &sdCommandsCount, 0, []() { return VFS::isSDAvailable(); } },
#endif
  { "oled",       "Local display control and graphics", "Drives the selected local display backend: its lifecycle, the live screen contents, "
    "and persistent appearance settings. oledstart/oledstop (aliases "
    "openoled/closeoled) power the display task on and off, and oledstatus (alias "
    "oledread) reports its state. oledmode <mode> switches the live screen among the "
    "built-in views (menu, status, sensordata, thermal, network, mesh, gps, espnow, "
    "memory, off, and more); oledtext <message> shows custom text and oledanim "
    "<name>|fps <n> picks the animation -- both require the display to be running (run "
    "oledstart first) and neither persists across reboot. Separately, the oled* config "
    "commands write settings to flash immediately: oledbootmode and oleddefaultmode set "
    "the screen shown at boot and as the idle default, while oledbrightness <0-255>, "
    "oledflip, oledbootduration, oledupdateinterval, oledthermalscale, "
    "oledthermalcolormode, and oledenabled tune appearance and timing. Whether a user "
    "must log in at the display before interacting with it is set with "
    "displayrequireauth <0|1> (super admin, Settings module).", oledCommands,        &oledCommandsCount, 0, nullptr },
#if ENABLE_NEOPIXEL
  { "neopixel",   "RGB LED strip and effects", "Controls the addressable RGB status LED (WS2812/NeoPixel). ledcolor <name> lights "
    "it a solid color from a fixed palette (red, green, blue, yellow, magenta, cyan, "
    "white, orange, purple, pink), and ledclear turns it off. ledeffect "
    "<fade|blink|pulse|strobe> [color] [color2] [duration 100-60000ms] runs an animated "
    "effect (defaults: red/blue, 3000 ms; ledeffect off clears it). These commands "
    "change the LED immediately and are not saved -- the persistent power-on brightness "
    "and startup animation live in the led settings module, not here. Note the effect "
    "call runs synchronously for its full duration before returning.", neopixelCommands,     &neopixelCommandsCount, 0, nullptr },
  { "led",        "LED brightness and startup effects", "Configures the board onboard single LED -- its brightness and the one-shot effect "
    "played at startup. These are persistent settings written to flash, not live "
    "controls: ledbrightness <0-100> sets the global brightness, ledstartupenabled "
    "<0|1> toggles the boot effect, and ledstartupeffect "
    "<none|rainbow|pulse|fade|blink|strobe> with ledstartupcolor, ledstartupcolor2, and "
    "ledstartupduration <100-10000ms> define what plays on power-up. (The live, "
    "immediate RGB controls are the separate ledcolor/ledeffect commands in the "
    "neopixel module.)", ledCommands,           &ledCommandsCount, 0, nullptr },
#endif  // ENABLE_NEOPIXEL (neopixel + led modules)
#if ENABLE_LED_MATRIX
  { "matrix", "HT16K33 monochrome LED matrix", "matrix test draws an orientation pattern. matrix text HI, matrix pixel 3 2 on, "
    "matrix clear/fill and matrix on/off control the display. matrix brightness <0-15> and matrix rotation <0-3> persist; "
    "matrix blink <0-3> selects off/2Hz/1Hz/0.5Hz. matrixbus <0|1> and matrixaddress <0x70-0x77> require reboot. "
    "matrix size 8x8 uses one square; matrix panel <0|1> selects the physical square. matrix size 16x8 restores both. "
    "Use an unused non-0x70 address when sharing a bus with a PCA9685. Static text clips at the display edge.",
    matrixCommands, &matrixCommandsCount, 0, nullptr },
#endif
#if ENABLE_SERVO
  { "servo",      "PCA9685 servo motor control", "The PCA9685 is a 16-channel I2C PWM driver used to control hobby servos (and "
    "generic PWM outputs) without tying up the ESP32 own timers. servo <channel> "
    "<angle> moves the servo on a channel to an angle, while pwm <channel> <value> "
    "[freq] writes a raw PWM duty (and optional frequency) for non-servo loads like "
    "LEDs or motor drivers. Because different servos expect different pulse ranges, "
    "servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name> stores a per-channel "
    "calibration that maps angles to the correct pulse widths (servolist shows the "
    "saved profiles), and servocalibrate <channel> opens an interactive mode to find "
    "those pulse limits by hand.", servoCommands,        &servoCommandsCount, 0, nullptr },
#endif
#if ENABLE_THERMAL_SENSOR
  { "thermal",    "MLX90640 thermal camera (32x24)", "The MLX90640 is a 32x24 (768-pixel) infrared thermal camera. openthermal starts "
    "it, thermalread reports the current frame min/max/avg temperature in Celsius, and "
    "closethermal stops it; thermalautostart [on|off] persists launching it at boot, "
    "and it runs on the fixed secondary I2C bus (Wire1). The sensor runs in "
    "chess-pattern mode at 16-bit ADC resolution, with thermaltargetfps <1..8> "
    "selecting the device refresh rate. Display tuning is extensive: "
    "thermalpalettedefault picks the color map (grayscale, iron, rainbow, hot, or "
    "coolwarm), thermalrotation <0..3> rotates the image 0/90/180/270 degrees, and "
    "thermalupscalefactor plus the thermalinterpolation* commands smooth and enlarge "
    "the 32x24 grid for the web/OLED view. Frame readings are stabilized by "
    "temporal/EWMA smoothing, per-pixel outlier rejection, and an optional rolling "
    "min/max auto-scale that keeps the color scale from flickering; thermaldiag prints "
    "a hardware self-check.", thermalCommands,      &thermalCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("thermal"); } },
#endif
#if ENABLE_TOF_SENSOR
  { "tof",        "VL53L4CX time-of-flight distance sensor", "The VL53L4CX is a laser time-of-flight ranging sensor that measures distance to "
    "nearby objects. opentof starts it, tofread reports the closest valid cached distance in "
    "centimeters (or full cached object data as JSON), and closetof stops it; tofautostart "
    "[on|off] persists launching it at boot, and tofbus selects its I2C bus after reboot. "
    "It is configured for LONG distance mode with a 200 ms timing budget, and "
    "is multi-target: each measurement can return up to four objects, which are "
    "signal-rate-gated and exponentially smoothed before the nearest valid one is "
    "reported. Most tunables are client-side visualization knobs rather than sensor "
    "settings: tofpollingms, toftransitionms, and tofmaxdistancemm shape the UI, "
    "tofstabilitythreshold sets how steady a reading must be, and tofdevicepollms "
    "controls how often the firmware reads the hardware.", tofCommands,          &tofCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("tof"); } },
#endif
#if ENABLE_IMU_SENSOR
  { "imu",        "BNO055 9-DOF orientation sensor", "The BNO055 is a 9-DOF inertial measurement unit providing fused absolute "
    "orientation. openimu starts it, imuread reports yaw/pitch/roll (degrees) plus "
    "acceleration, gyroscope, and chip temperature, and closeimu stops it; imuautostart "
    "[on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus "
    "(Wire1) using the board external crystal. Beyond raw orientation, imuactions runs "
    "gesture/event detection derived from the motion data: shake, tilt (with "
    "direction), tap/knock, rotation (with axis), freefall, a step counter with "
    "cadence, and screen-style orientation. Because the chip can be mounted in any "
    "pose, imuorientationmode <0..8> applies a fixed remap (flip pitch/roll/yaw, "
    "90-degree rotations, upside-down fixes), imuorientationcorrection <0|1> toggles "
    "that correction, and imupitchoffset/imurolloffset/imuyawoffset trim each axis in "
    "degrees.", imuCommands,          &imuCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("imu"); } },
#endif
#if ENABLE_OLED_INPUT
  { "input",      "Input device (gamepad or rotary encoder)", "Device-agnostic abstraction for the OLED input controller, which is either the "
    "Seesaw gamepad, ANO encoder or GPIO rotary encoder -- chosen at compile time via "
    "INPUT_DEVICE_TYPE and mutually exclusive, so exactly one driver is present per "
    "firmware. These commands operate on whichever driver was built in: openinput "
    "starts it, closeinput stops it, inputautostart [on|off] persists boot auto-start, "
    "and inputdevicepollms <10-1000> sets the polling interval in milliseconds (default "
    "90 for I2C devices; GPIO input uses a fixed 5 ms button sample). Driver-specific debugging and tuning live in the gamepad and anoencoder "
    "modules; this module holds only the shared settings (poll interval and "
    "auto-start).", inputCommands,       &inputCommandsCount,       CMD_MODULE_SENSOR, []() { return gInputConnected; } },
#endif
#if ENABLE_GAMEPAD_SENSOR
  { "gamepad",    "Seesaw gamepad — raw debug commands", "Adafruit Seesaw I2C gamepad (analog joystick plus buttons), exposed here as a "
    "low-level debug interface for the raw device. The driver-agnostic "
    "open/close/autostart/poll commands live under the input module; the only "
    "gamepad-specific command is gamepadread, which polls the Seesaw once and dumps raw "
    "state -- joystick X/Y and the button bitmask -- attempting an on-demand connect "
    "with backoff if the device is not yet initialized. A background task polls the "
    "gamepad at roughly 50 ms and caches the latest reading for the OLED UI and sensor "
    "JSON. This module is mutually exclusive at build time with anoencoder; only one "
    "input device is compiled in per firmware (see input).", gamepadCommands,      &gamepadCommandsCount, CMD_MODULE_SENSOR, []() { return gInputConnected; } },
#endif
#if ENABLE_ANO_ENCODER
  { "anoencoder", "ANO rotary encoder — debug + driver-specific config", "Adafruit ANO directional navigation rotary encoder on Seesaw I2C: a click wheel "
    "with a center IN press and UP/DOWN/LEFT/RIGHT buttons, used as the OLED navigation "
    "input. This module provides debug and remap commands; the actual "
    "open/close/autostart/poll lifecycle lives under the input module. anoencoderread "
    "dumps raw state -- encoder position, the currently selected rotary axis, and the "
    "button bitmask. Remap commands persist to settings: anoencoderi2caddr <1-127> "
    "changes the device address (reboot required), anoencoderinvert [on|off] reverses "
    "rotation direction, and anoencoderswapud / anoencoderswaplr [on|off|toggle] swap "
    "the UP/DOWN and LEFT/RIGHT button pairs. A polling task accumulates encoder "
    "detents so fast spins do not drop clicks. Mutually exclusive at build time with "
    "the Seesaw gamepad.", anoEncoderCommands, &anoEncoderCommandsCount, CMD_MODULE_SENSOR, []() { return gAnoEncoderConnected; } },
#endif
#if ENABLE_APDS_SENSOR
  { "apds",       "APDS9960 color, proximity, gesture sensor", "The APDS9960 is a combined RGB color, proximity, and gesture sensor. openapds "
    "starts it (color sensing enabled by default), apdsread shows which modes are "
    "active plus the latest RGBC and proximity values, and closeapds stops it; "
    "apdsautostart [on|off] persists launching it at boot, and apdsbus selects its I2C "
    "bus after reboot. Its three functions are toggled at "
    "runtime with apdsmode <color|proximity|gesture> [on|off] -- note that enabling "
    "gesture also turns proximity hardware on. Dedicated reads apdscolor, "
    "apdsproximity, and apdsgesture show the latest cached sample; they report "
    "unavailable when that mode has no reading yet. Gesture reports the last "
    "swipe (UP/DOWN/LEFT/RIGHT) and its age, or no gesture since start, without "
    "consuming events. apdsdevicepollms sets how often the background task samples "
    "color and proximity; gesture mode reads the gesture FIFO at least every 100 ms.", apdsCommands,         &apdsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("apds"); } },
#endif
#if ENABLE_GPS_SENSOR
  { "gps",        "PA1010D GPS module", "PA1010D I2C GPS receiver. Lifecycle: opengps starts the parser task, gpsread "
    "prints the current fix, and closegps stops it; gpsautostart [on|off] persists boot "
    "auto-start, and the module appears in help only when the chip is detected. gpsread "
    "reports fix yes/no, fix quality, satellite count, and (only when a fix is held) "
    "latitude/longitude in degrees, altitude in meters, speed in knots, heading angle, "
    "plus GPS UTC time and date; with no fix it shows just quality and satellites. The "
    "gpslog [interval_ms] command requests GPS startup and starts GPS-only track "
    "logging (100..3600000 ms, default 1000). It refuses while a log is running or "
    "health logging is enabled. GPS initializes in the background; a queued request "
    "does not establish a satellite fix. GPS/logger boot auto-start is enabled "
    "after the logger starts successfully.", gpsCommands,          &gpsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("gps"); } },
#endif
#if ENABLE_FM_RADIO
  { "fmradio",    "RDA5807 FM radio receiver", "RDA5807M I2C FM radio receiver. Lifecycle: openfmradio starts it, fmradioread "
    "reports status, and closefmradio stops it; fmradioautostart [on|off] persists boot "
    "auto-start and the module shows in help only when detected. fmradiotune <freq> "
    "accepts either MHz (e.g. 103.9) or 10 kHz integer units (e.g. 10390) -- values "
    "under 200 are read as MHz, otherwise as raw units -- and rejects anything outside "
    "76.0-108.0 MHz; tuning clears any decoded RDS station name and text. fmradioseek "
    "[up|down] STARTS a hunt for the next station (no band wrap) and returns "
    "immediately -- the radio task finalizes the result within a few seconds; watch "
    "'fmradioread' (JSON field \"seeking\") or the OLED FM screen. fmradiovolume <0-15> "
    "sets output level, and fmradiomute / fmradiounmute toggle audio. The OLED FM "
    "screen drives all of this from the gamepad: L/R tune, Up/Down seek, A mute, "
    "Y volume, X power.", fmRadioCommands,      &fmRadioCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("fmradio"); } },
#endif
#if ENABLE_RTC_SENSOR
  { "rtc",        "DS3231 precision RTC", "DS3231 precision I2C real-time clock with battery backup and an on-chip "
    "temperature sensor. Lifecycle: openrtc starts the RTC task, rtcread [status|temp] "
    "reads the clock (or die temperature), and closertc stops it; rtcautostart [on|off] "
    "persists boot auto-start and the module appears in help only when detected. rtcset "
    "accepts either \"YYYY-MM-DD HH:MM:SS\" or a bare Unix timestamp and writes it to "
    "the chip, computing day-of-week automatically. rtcsync [to|from] moves time "
    "between the RTC and the system clock: to (the default) copies RTC -> system, from "
    "copies system -> RTC (use this after an NTP sync to persist accurate time into the "
    "battery-backed chip). Setting the time via rtcset or rtcsync from also marks the "
    "RTC as calibrated so later boots trust it as a time source.", rtcCommands,          &rtcCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("rtc"); } },
#endif
#if ENABLE_PRESENCE_SENSOR
  { "presence",   "STHS34PF80 IR presence/motion sensor", "The STHS34PF80 is an infrared presence and motion sensor that detects warm bodies "
    "without contact. openpresence starts it, presenceread reports ambient temperature "
    "plus presence, motion, and temperature-shock values (each with a DETECTED flag), "
    "and closepresence stops it; presenceautostart [on|off] persists launching it at "
    "boot. The sensor is initialized at an 8 Hz output data rate with block-data-update "
    "enabled, and its on-chip presence/motion/ambient-shock detection engines provide "
    "the detection flags directly; presencestatus prints connection and data-validity "
    "diagnostics, and presencedevicepollms controls the hardware read interval.", presenceCommands,     &presenceCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("presence"); } },
#endif
#if ENABLE_CAMERA_SENSOR
  { "camera",     "ESP32-S3 DVP camera sensor", "Driver and CLI for the attached DVP camera sensor (OV2640/OV3660 class). The "
    "sensor must be powered up first with opencamera before any capture or tuning "
    "command works (closecamera stops it); cameraread and cameradump report status and "
    "all current sensor register values. Three distinct capture paths exist: "
    "cameracapture grabs one JPEG frame into RAM and reports its size only, camerasave "
    "captures and writes a frame to storage (LittleFS, SD, or both, per "
    "camerastoragelocation, into cameracapturefolder), and cameratiny produces a "
    "160x120 frame small enough for a single ESP-NOW packet; camerarecord start|stop "
    "records MJPEG-AVI video and requires an SD card. Resolution and image controls "
    "(camerares/cameraframesize, cameraquality, camerafps) and a large set of "
    "sensor-tuning commands (brightness/contrast/saturation, white balance, "
    "exposure/AEC, gain/AGC, special effects, mirror/flip/rotate, plus raw camerareg "
    "register writes) adjust the live image. cameraautostart powers the sensor up at "
    "boot, and cameramaxstoredimages caps how many saved photos are kept before the "
    "oldest are rotated out. There is no timed auto-capture: every capture is "
    "requested by a command or a page.", cameraCommands,       &cameraCommandsCount, CMD_MODULE_SENSOR, []() { return cameraConnected; } },
#endif
#if ENABLE_MICROPHONE
  { "microphone", "Microphone (PDM or G2 glasses)", "Driver and CLI for the microphone — the on-board PDM mic and/or the G2 glasses mic, selected with micsource. The mic must be started with "
    "openmic before reads or recording (closemic stops it); commands that need the "
    "running mic return a use-openmic-first error otherwise. miclevel returns the "
    "current audio level (percent; add json for structured output) and micviz shows a "
    "live level meter until a key is pressed. micrecord start [vad <ms>] [trim]|stop "
    "records audio to a WAV file; owner-correlated capture uses micrecord "
    "startid|statusid|stopid with a strict 16-hex ID and optional discard. miclist "
    "lists saved recordings, while micdelete and micdeleteid remove manual or exact "
    "owner-scoped results. Audio format is configured with micsamplerate (8000-48000), micgain (0-100), "
    "and micbitdepth (16 or 32), each usable as a getter with no argument; micautostart "
    "on|off persists whether the mic powers up automatically at boot.", micCommands,          &micCommandsCount, CMD_MODULE_SENSOR, []() { return audioAnySourceAvailable(); } },
#endif
#if ENABLE_EDGE_IMPULSE
  { "edgeimpulse", "Edge Impulse ML inference", "On-device machine-learning image inference using TensorFlow Lite Micro models "
    "exported from Edge Impulse. Two things must be in place before inference: a model "
    "must be loaded with eimodelload \"<file>\" (models live in the models directory on "
    "LittleFS; eimodellist/eimodelinfo/eimodelunload manage them) and inference must be "
    "enabled with eienable 1 (which also initializes the inference buffers). eidetect "
    "runs a single detection on a live camera frame and so additionally requires the "
    "camera to be opened (see opencamera), returning detected objects with confidence "
    "and bounding boxes as JSON; eifile \"<path>\" runs the same inference against a "
    "stored JPEG instead of the camera. eicontinuous 1 runs detection repeatedly in the "
    "background, eiconfidence <0.0-1.0> sets the minimum confidence to report, and "
    "eistatus shows current state. The eitrack family (eitrackenable, eitrackstatus, "
    "eitrackclear) adds cross-frame state tracking of detected objects on top of raw "
    "detections.", edgeImpulseCommands,  &edgeImpulseCommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif

#if ENABLE_DICTATION
  { "transcription", "Transcription interfaces", "Session controls and private transcript pages shared by display and web interfaces.", transcriptionUICommands, &transcriptionUICommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif
#if ENABLE_QUICKNOTE
  { "note", "QuickNote private notes", "Private spoken notes saved under your own /stt/u<id>/notes/. "
    "note start works from the lens, web or local display; other consoles use note start glasses (paired "
    "wearer only). Status never shows the text; list/read/delete browse your notes.", noteCommands, &noteCommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif

#if ENABLE_LOCAL_STT
  { "stt", "Local speech-to-text", "Buffered local dictation through the shared audio HAL. "
    "Stop SR and the microphone sensor first, then use stt record [seconds]. "
    "Poll stt status [id], finish early with stt stop <id>, discard with stt cancel <id>, "
    "and read stt result <id> from the same authenticated session. Audio and results "
    "stay local; no host link is required. This backend produces text for review, "
    "not voice command execution.", sttCommands, &sttCommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif

 #if ENABLE_ESP_SR
   { "espsr", "ESP-SR speech recognition", "Offline voice control built on Espressif ESP-SR: a WakeNet wake-word stage gates a "
    "MultiNet command-phrase recognizer, so the device waits for the wake word and then "
    "listens for a known command phrase. Note that srenable/sr enable only reports the "
    "compile-time build flag and cannot toggle the feature at runtime; the real "
    "lifecycle commands are opensr/srstart to start the recognition pipeline and "
    "closesr/srstop to stop it. Starting the pipeline also arms voice command execution "
    "as the current authenticated user (and stopping it disarms); arming can be managed "
    "directly with voicearm/voicedisarm/voicestatus, and recognized phrases only "
    "execute commands while armed. The command vocabulary is managed with the srcmds "
    "family (list/add/del/clear plus save/reload to an SD file and srcmdssync to import "
    "phrases from the CLI registry). Recognition is tuned through srconfidence, "
    "srtimeout, the srtuning* audio controls (gain, AGC, VAD, filters), and srdebug* "
    "telemetry; the mic feed follows the device-wide source (set with `micsource "
    "auto|pdm|g2` — onboard PDM or the G2 glasses left-temple mic), and the srsnip* commands capture audio "
    "snippets (by default on the wake word) for debugging.", espsrCommands,  &espsrCommandsCount, CMD_MODULE_SENSOR, nullptr },
 #endif
#if ENABLE_I2C_SYSTEM
  { "i2c",        "I2C bus diagnostics and scanning", "The i2c module configures and diagnoses up to two I2C buses and the sensor device "
    "registry. There are two buses with a deliberate naming convention: bus 0 is I2C1 "
    "(Arduino Wire1, the primary STEMMA QT / sensor bus) and bus 1 is I2C2 (Wire, the "
    "optional secondary bus); each has its own enable flag and SDA/SCL pin settings, "
    "and bus/pin changes require a reboot. Each sensor can be routed to either bus with "
    "a per-device command. Each sensor's routing verb now lives in that sensor's own "
    "module (run 'help gps' for gpsBus, 'help rtc' for rtcBus, and so on); oledBus, "
    "inputBus and fuelGaugeBus stay here. The persisted setting names are unchanged. "
    "all taking 0 or 1 and needing a reboot. Discovery and diagnostics: i2cscan dumps "
    "raw addresses found on each active bus; detect reports configured-vs-present "
    "hardware and detect apply (admin) auto-enables newly detected cheap devices; "
    "i2cmetrics/i2cstats/i2chealth show bus performance, error counters, and per-device "
    "health. Bus recovery: i2cpause/i2cresume stop and restart sensor polling, i2creset "
    "does a pause-recover-resume cycle, and i2crecover <address> clears a single device "
    "degraded state. The device registry is exposed via sensors [filter|json], "
    "sensorinfo <name>, devices, discover, and devicefile; sensorautostart [sensor] "
    "[on|off] controls which sensors start polling automatically at boot.", i2cCommands,          &i2cCommandsCount, 0, nullptr },
#endif
#if ENABLE_AUTOMATION
  { "automation", "Scheduled tasks and conditional commands", "The automation module runs saved jobs (stored in automations.json) that execute "
    "one or more CLI commands on a schedule, condition, or system event. Every automation has one of "
    "four trigger types: atTime (fires daily at time=HH:MM, optionally limited to "
    "days=Mon,Tue,...), afterDelay (fires once after delayms milliseconds), interval "
    "(fires repeatedly every intervalms milliseconds), or event (fires when a system "
    "event occurs: on=<kind> with an optional match=<text> filter against the event's "
    "subject/detail — run 'events' to see recent kinds like peer_online, text_rx, "
    "battery_low, login_fail); jobs can also carry runatboot=1 "
    "to fire at startup. The primary entry point is automation <subcommand> (list, add, "
    "enable, disable, delete, run, trigger, sanitize, recompute) with single-word "
    "aliases automationlist, automationadd, automationrun, and automationtrigger. Note "
    "the important distinction: automationrun id=<id> executes a job commands "
    "immediately, whereas automationtrigger id=<id> only arms an afterDelay/manual "
    "timer so it fires after its delay; and automation system enable|disable|status is "
    "the global master switch that gates whether the scheduler runs at all, independent "
    "of each job own enabled flag. Jobs may also include an optional condition "
    "expression, and conditional commands use an IF <expr> THEN <command> [ELSE "
    "<command>] form (e.g. IF temp>75 THEN ledcolor red). Conditions can check sensors "
    "(temp, distance, light, motion), time (time, hour, day, ntp), system state "
    "(battery, heap, psram, fsfree, uptime, chiptemp), connectivity (wifi, rssi, peers, "
    "ble), ESP-NOW/bond (espnow, bond_mode, bond_paired, bond_online, bond_synced, "
    "bond_role, bond_rssi, bond_peer_heap, bond_peer_uptime, pairmode, pairmode_secs, "
    "peersknown, stalestpeerage), location (gps, speed, sats, wp_dist:<name>), the on-device model (llm), "
    "and ESP-NOW metadata "
    "(room, zone, tags); numeric variables use the > < = >= <= != operators and "
    "string/enum variables use = != CONTAINS. A true condition fires every time its "
    "trigger is due. Supporting commands: validate-conditions checks conditional syntax without "
    "running it, autolog records automation activity to a file, and print <message> "
    "broadcasts text to all outputs.", automationCommands,   &automationCommandsCount, 0, nullptr },
#endif
#if ENABLE_BATTERY_MONITOR
  { "battery",    "Battery voltage and charge monitoring", "The battery module reports cell state and keeps a time-series log; it is only "
    "present when battery monitoring is compiled in. The backend is a MAX17048 fuel "
    "gauge over I2C or a calibrated ADC voltage divider selected by the board. "
    "ADC charge percentage is approximate; the charger can drive the measured BAT "
    "node even without a cell. USB, charging and cell presence are reported only "
    "when known. Missing readings are null in JSON, not zero/full estimates. "
    "batterystatus prints voltage, charge percentage, charging/USB state, and a coarse "
    "status label, or returns the same data as JSON. batterylog manages a CSV "
    "discharge/charge log written to the device for later graphing: with no args it "
    "shows status, and subcommands are on/off (enable/disable), interval <5..3600> "
    "seconds (sampling period), tail (show the most recent rows), and clear (erase the "
    "log); significant events such as sleep/wake are always recorded regardless of the "
    "interval. batterycalibrate (admin) reinitializes ADC calibration or re-probes the fuel gauge.", batteryCommands,      &batteryCommandsCount, 0, nullptr },
#endif
  { "debug",      "System debugging and diagnostics", "The debug subsystem controls diagnostic logging verbosity across every part of the "
    "firmware. Its core is a large set of per-subsystem debug-flag toggles (for example "
    "debugwifi, debughttprequests, debugespnowcore, debugcamera, debugimuvalues) that "
    "each follow a <0|1> [temp|runtime] model: with no mode the new state is persisted "
    "to flash, while temp or runtime flips only the live runtime flag and is NOT saved "
    "(it reverts on reboot). Many subsystems have a parent flag plus finer sub-flags "
    "(lifecycle/polling/values, or core/router/mesh/topo for ESP-NOW); the parent acts "
    "as a master switch and any sub-flag also lights its parent. Separate from the "
    "on/off flags, loglevel sets a severity threshold (error|warn|info|debug, "
    "persisted) and debugverbose is a global override. Related commands manage where "
    "output goes: outserial gates the UART lane (persisted), outg2/outble open "
    "runtime CLI streams to G2 glasses / BLE clients (session-only, reset on "
    "reboot), log starts/stops system-wide logging to a file, loglink routes ESP-IDF "
    "framework logs through the unified output queue, and debugstack/debugbuffer expose "
    "low-level trace and queue diagnostics.", debugCommands,        &debugCommandsCount, 0, nullptr },
  { "settings",   "Device configuration and preferences", "The settings subsystem holds the device persisted configuration and the commands "
    "that change it. Each setting command (for example outserial, "
    "serialrequireauth, displayrequireauth, tzoffsetminutes, ntpserver, wifitxpower, "
    "webclihistorysize) sets one value and normally requests an immediate settings-file "
    "write. Because flash writes are costly, beginwrite and savesettings can coalesce "
    "changes made by the same request or transport session: the final save writes each "
    "changed settings file at most once. This is write coalescing, not a RAM transaction; "
    "unrelated sources continue to persist immediately, and their full snapshot can "
    "include live values changed while a batch is open. Idle batches expire after two "
    "minutes. savesettings remains an explicit flush-now command when no batch is open. "
    "Most commands here are admin-gated. Some "
    "changes only take effect after a reboot (for example espnowenabled and "
    "httpsEnabled are marked reboot required). The controls command emits a "
    "machine-readable JSON descriptor of a module's settable controls for UI use. Note "
    "that most subsystem settings (wifi, i2c, sensors, power, oled, bluetooth, espnow) "
    "are owned and registered by their own modules; this module hosts the cross-cutting "
    "CLI/output/auth/time settings plus the batch-write machinery.", settingsCommands,     &settingsCommandsCount, 0, nullptr },
  { "sensorlog", "Sensor data logging to files", "Periodically samples the onboard sensors and appends readings to a file, driven by "
    "the single multiplexed sensorlog <subcommand> command. sensorlog start <filepath> "
    "[interval_ms] begins logging (default 5000 ms; the filepath must start with / and "
    "parent directories are created automatically) and sensorlog stop ends it; only one "
    "log can run at a time, so start refuses if logging is already active. sensorlog "
    "status reports the active file, interval, format, rotation settings, selected "
    "sensors, and last-write age. Configure behavior with format <text|csv|track> "
    "(track is a compact GPS-only format with signal-loss dedup), maxsize and rotations "
    "for log rotation, and sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none> "
    "to choose which sensors are recorded. sensorlog interval <ms> sets the poll period "
    "(100-3600000, default 5000). NOTE: format track additionally REPLACES the sensor "
    "mask with GPS-only and persists it, so a prior selection is lost — and rotations 0 "
    "deletes the active file at the size cap rather than pruning older generations. "
    "sensorlog autostart [on|off] makes logging "
    "resume on the next boot using the last-used parameters; the "
    "format/maxsize/rotations/sensors/autostart choices are persisted.", sensorLoggingCommands, &sensorLoggingCommandsCount, 0, nullptr },
#if ENABLE_R1_HEALTH
  { "health",     "R1 ring health vitals and local health logging", "Live vitals from a paired R1 ring (HR, HRV, SpO2, temperature, battery) "
    "plus the on-device health logger. healthstatus reads live values, ring control state, "
    "logging state and typed-history status; healthstatus json is what the app and web page consume. "
    "healthlogging drives the local CSV logger independently of the ring's own health-collection "
    "privacy setting, and healthlogmerge stitches captures together. The ring rides the G2 BLE "
    "transport, so this whole family requires Bluetooth + G2; connect via ringconnect.",
    healthCommands, &healthCommandsCount, 0, nullptr },
#endif
  { "users",      "User authentication and management", "The users subsystem provides admin-gated account management, authentication, "
    "sessions, and bans. Accounts have two roles, admin and standard; the first account "
    "is the owner-admin, and userpromote/userdemote change roles while useradd creates "
    "an account directly (optionally forcing a password change on first login). New "
    "accounts can also come through an approval flow: userrequest files a pending "
    "request that an admin clears with userapprove or rejects with userdeny "
    "(pendinglist shows the queue). login and logout authenticate per transport "
    "(serial, display, bluetooth, g2), userlist enumerates accounts, and the password "
    "commands cover both self-service (userchangepassword) and admin reset "
    "(userresetpassword). Sessions are tracked per transport: sessionlist shows active "
    "sessions and sessionrevoke force-logs-out a session by SID or by username. Two "
    "independent ban mechanisms exist: ban/unban/banlist block an IP address, while "
    "banuser/unbanuser suspend a user account so it cannot log in until unbanned; the "
    "primary admin account cannot be banned. usersync pushes a user credentials to "
    "another device over ESP-NOW, authenticated by an admin account on the receiving "
    "device.", userSystemCommands,         &userSystemCommandsCount, 0, nullptr },
  { "features",   "System feature management", "The features subsystem enables or disables compiled-in capabilities at runtime and "
    "reports their memory cost. features with no argument lists every feature grouped "
    "by category (Network, Display, Sensors, System) with an approximate heap estimate "
    "and a status of ON, OFF, or N/C (not compiled in this build); features <id> shows "
    "one feature details and features <id> <on|off> toggles it, persisting the change "
    "immediately. Only features that are compiled and marked runtime-toggleable can be "
    "changed; a few are compile-time only, and some (wifi, oled, i2c, https) are "
    "flagged reboot required so the toggle persists but the capability does not "
    "actually start or stop until the next restart. featuresetup launches an "
    "interactive, admin-only wizard that walks through the same toggles and works from "
    "any CLI transport.", featureCommands,      &featureCommandsCount, 0, nullptr },
#if ENABLE_CAMERA_SENSOR
  { "image",      "Image capture and management", "Captures stills from the camera and manages the saved photo library. capture grabs "
    "a frame and saves it as a JPG (target storage chosen by argument: littlefs/lfs, "
    "sd, or both; default follows the cameraStorageLocation setting), and requires a "
    "camera sensor that is both compiled in and enabled -- on boards with no camera the "
    "capture simply fails. images lists saved photos with sizes and storage stats (add "
    "sd to list the card, json for app/BLE output); imagedelete \"<path>\" removes one "
    "(path must be quoted). imagesend transmits a photo to another device over ESP-NOW: "
    "imagesend <device> \"<path>\" resolves the device by name or MAC and sends the "
    "named file (path required).", imageCommands,        &imageCommandsCount, 0, nullptr },
#endif
#if ENABLE_MAPS
  { "map",        "Map navigation and waypoints", "On-device offline map subsystem backed by region map files stored under /maps/ "
    "(custom HWMap tile format). A map must be loaded before any lookup works: mapload "
    "\"<path>\" loads a file into PSRAM, maplist shows what is available, map prints "
    "the current map region/feature-count/bounds (add json for structured output), and "
    "mapunload frees the PSRAM and tile cache. search <name> finds named features in "
    "the loaded map, while whereami reports the nearest road and area for the current "
    "GPS position and therefore needs both a loaded map and a live GPS fix. Waypoints "
    "are persistent user markers managed through waypoint "
    "(list/add/del/goto/clear/clearall/rename/notes) and can have files attached via "
    "waypointfile/waypointfiles; gpstrack loads, inspects, or clears a recorded GPS "
    "breadcrumb track (and rejects tracks that fall outside the loaded map bounds), and "
    "maporganize sorts loose files in /maps into subdirectories.", mapCommands,          &mapCommandsCount, 0, nullptr },
  { "mapsettings","Maps app settings (zoom, layers, cache)", "Persisted rendering defaults for the maps app, stored under apps.maps and applied "
    "to the live map at boot. mapzoom <0.5..20.0> sets the initial zoom, maplayers "
    "<0..1023> sets a bitmask controlling which feature layers are drawn, and "
    "mapcachekb <256..4096> sizes the tile LRU cache pool. The zoom and layers setters "
    "also mirror immediately into the running renderer so changes take effect without a "
    "reboot, but the cache size only re-applies on the next map load (or reboot). All "
    "three are admin-only and, run from the CLI, write to flash immediately so they "
    "survive a reboot.", mapsSettingCommands, &mapsSettingCommandsCount, 0, nullptr },
#endif
  { "power",      "Power management", "The power subsystem manages CPU frequency and battery-oriented power saving. The "
    "main command is power: power alone prints the current mode, CPU clock and display "
    "brightness; power mode <perf|balanced|saver|ultra|locked|0-4> "
    "selects one of five target-specific presets: Performance, Balanced, PowerSaver, "
    "UltraSaver, or Locked. Run power or power json to see their active and idle MHz. "
    "Modes set the CPU clock and display brightness; the chosen mode is persisted. "
    "Locked keeps the maximum active clock when the OLED blanks. UltraSaver uses its "
    "lower idle-only clock when powersave blanks the display; input or a command "
    "restores the target's interactive floor. Nothing switches the mode on battery "
    "level in this firmware: power auto and power threshold report that automatic "
    "mode is not available. Two related idle "
    "controls are separate commands: powersave <0..1440> sets an idle timeout (minutes; "
    "0 disables) after which the OLED blanks and the CPU may downclock (mode-dependent) "
    "while the radio stays up so the device remains reachable, and powercooldown "
    "<0..60000> sets an anti-flap cooldown (milliseconds) that prevents rapid "
    "back-to-back sleep transitions. All of these values persist.", powerCommands,        &powerCommandsCount, 0, nullptr },
  { "liveaudio",  "Opt-in live PCM transport and recorder shadow", "Exercises the live-pcm-v1 UART framing and bounded receiver path. "
    "A real authenticated UART host first acquires a renewable 3-second controller lease with liveaudio ready. liveaudio synth schedules "
    "deterministic 16 kHz signed-16-bit mono PCM; an explicit, exact-ID liveaudio shadow arm can instead tee an owned 16 kHz PDM or G2 "
    "recording through a fixed 16 KiB PSRAM queue. Shadow transport is disabled by default, never delays or replaces the WAV writer, and a "
    "transport fault aborts only the live copy while the finalized WAV remains authoritative. This diagnostic transport does not enable "
    "production streaming STT, LLM, or lens delivery.", liveAudioCommands, &liveAudioCommandsCount,
    0, nullptr },
  { "cm5", "CM5 service presence and host power/fan control", "Everything that talks to the CM5 Linux host over the authenticated UART link. "
    "cm5 status exposes the current named-UART epoch binding, freshness, state, command bridge, monitor transitions, and task stack watermark; "
    "cm5 capabilities reports the heartbeat protocol constants. The five-second heartbeat itself is authenticated UART control-plane traffic, "
    "not a user command, so it remains responsive even while the shared command executor is occupied. "
    "cm5 power and cm5 fan drive the host through two independent finite request/ACK/report state machines that share one ID space: "
    "cm5 power status requests fresh CM5 state, cm5 power profile <eco|balanced|performance|auto> requests a power profile, and cm5 fan "
    "<quiet|max> pins a fan mode while cm5 fan auto returns control to the Linux temperature curve. Bare cm5 power / cm5 fan (or their show "
    "form) display local delivery state and the last CM5 report. Initiation requires an admin; reboot, halt, suspend, and sleep_for "
    "<1..1440 minutes> additionally require superadmin plus a literal same-command confirm token, and recover confirm clears only an "
    "inspected fail-closed transition. ACK/report callbacks are accepted from a real authenticated UART session only, which lets the CM5 "
    "service account stay user-tier. One request per protocol may be pending at a time and delivery retries are finite; destructive "
    "execution additionally requires confirmed accepted and committed ACK phases, while a normalized Linux boot ID distinguishes a daemon "
    "restart from a completed host boot. A max fan request may supersede one pending non-max request.",
    cm5PresenceCommands, &cm5PresenceCommandsCount,
    0, nullptr },
  { "ota",        "Signed firmware recovery and update", "Native ESP-IDF signed OTA support. otastatus reports the journal, "
    "partition identity, staged pair, and last result. A superadmin sets a persistent recovery credential with otapin - "
    "on the physical serial console only, because it decides whether recovery can be reached at all - then "
    "uploads /system/ota/candidate.part and manifest.part, validates and journals them with otastage confirm, then uses "
    "otaupdate confirm to reboot into the immutable factory recovery updater. otarecovery confirm enters the same "
    "authenticated recovery image for a direct upload when the main filesystem cannot stage an image; its explicit "
    "allow-downgrade option is required for older signed releases. otacancel replaces a staged request safely, and "
    "otaack <result-sequence> confirm acknowledges exactly the durable result that was reviewed. Mutating OTA "
    "commands are forbidden from automations and require the opt-in 16 MB OTA partition layout.",
    otaCommands, &otaCommandsCount, CMD_MODULE_CORE, nullptr },
#if ENABLE_OLED_DISPLAY
  { "setpattern", "OLED gamepad password entry", "Provides the single admin-only command setgamepadpassword, which opens the "
    "gamepad-pattern password setup flow on the OLED screen. A pattern is a sequence of "
    "joystick directions that is hashed and stored as the logged-in user password, "
    "usable for on-device login. You must already be logged in at the OLED display "
    "first (the command errors otherwise); the guided on-screen flow then "
    "re-authenticates you, prompts you to enter the new pattern and confirm it, and "
    "saves it to your account. This command only launches the OLED mode -- the actual "
    "entry and confirmation happen on the device screen.", setPatternCommands,   &setPatternCommandsCount, 0, nullptr },
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "even_g2",    "Even G2 smart glasses control", "This subsystem drives Even Reality G2 smart glasses while Bluetooth is in client "
    "mode (blemode client); the two temples are addressed as left/right/auto. openg2 "
    "starts scan-and-connect IN THE BACKGROUND and returns immediately -- it does not "
    "block, so poll g2status (or g2info for firmware/MAC/battery) to see when the link "
    "is up, and nearly every other command here requires that connection first. Display "
    "commands render to the lens: g2show prints text, g2ai/g2ai-noask/g2ai-direct push "
    "a front-pane AI answer card through the EvenAI pipeline, g2bmp shows a BMP file, "
    "and g2sensors/g2network/g2files/g2settingspage show built-in info pages; g2nav "
    "[on|off] enables menu-navigation mode and g2clear blanks the display. For audio, "
    "use the shared Microphone controls: micsource selects auto, pdm or g2; "
    "openmic/closemic start and stop the selected source; micrecord controls WAV "
    "recording. g2micon/g2micoff are low-level stream probes; g2micrec and g2micwav "
    "remain SD-backed raw-LC3 and decoded-WAV diagnostics. closeg2 disconnects but keeps "
    "the GATT cache for fast reconnect; closeg2 full also frees the cache to recover "
    "about 30 KB. The remaining g2* commands are low-level protocol probes and "
    "diagnostics (g2probe, g2protostats, g2devcfg, g2dumpframes).", g2Commands,           &g2CommandsCount, 0, nullptr },
  { "even_r1",    "Even R1 ring link, health, and controls", "This subsystem talks to the Even R1 smart ring over BLE through a serialized, "
    "profile-gated transaction owner. ringscan [seconds] discovers the ring and "
    "ringconnect [mac] connects "
    "(auto-scanning when no MAC is given, or connecting directly when one is), with "
    "ringstatus and ringdisconnect for state and teardown. ringquery is the main data "
    "command, requesting wear/heart-rate/HRV/SpO2/temperature/activity/sleep readings. "
    "Its raw diagnostic form is admin-only, requires confirmation for SETs, and "
    "prohibits user-profile writes. Ring health collection and low power are set with "
    "ringcontrol <collection|lowpower> <preserve|on|off> (admin): a persisted desired "
    "state applied to the ring asynchronously, which healthstatus reports as desired and "
    "observed (the OLED and web Health controls run the same command). Health SETs remain "
    "ACKed-unverified while low power has capture-proven readback. debugringdump toggles "
    "a redacted byte dump for debugging. Bridging ring data onto the G2 glasses is not "
    "supported: both approaches proved to be dead ends and were removed.", g2RingCommands,   &g2RingCommandsCount, 0, nullptr },
#endif
#if ENABLE_LLM_BACKEND
  { "llm",        "On-device LLM text generation", "On-device large language model that runs a quantized model file entirely on the "
    "device (model weights held in PSRAM). A model must be loaded before generation: "
    "llmload [file.bin] loads one (bare filenames are looked up on the SD card under "
    "/sd/llm then internal /system/llm), llmmodels lists available files, llmunload "
    "frees the PSRAM, llmstatus shows engine state, and llmautostart 0|1 / "
    "llmdefaultmodel control boot-time loading. Two generate forms: bare 'llmgenerate "
    "<prompt>' BLOCKS and prints the whole reply, while 'llmgenerate json ...' starts "
    "async and returns a session id immediately — then poll llmresult json <offset> "
    "repeatedly (each call returns new text, the running total length, and a done flag) "
    "until done flips true; llmstop aborts an in-progress generation. The engine keeps a "
    "multi-turn conversation: llmclear resets it, llmretry regenerates the last reply "
    "(async), and llmturns json <index> reads back one turn at a time. The llm* setters "
    "(temperature, topp, minp, maxtokens, sentencelimit, hardcap, reppenalty/repwindow, "
    "maxcontext, kvprec, norepeatngram, confthreshold, contentboost) are admin-only "
    "sampler and KV-cache defaults that persist to flash; kvprec and maxcontext only take "
    "effect on the next model load.", llmCommands,          &llmCommandsCount, 0, nullptr },
#endif
  { "settingsedit", "Per-field settings save commands",
    "Static CLI commands the web/OLED settings screen uses to persist individual settings fields that have no dedicated module command. Each writes one setting via handleSettingCommand; the value is read live or applied on next start. Fields that need a live apply action are routed to their module command instead of getting one of these.",
    settingEditorCommands, &settingEditorCommandsCount, 0, nullptr },
 };
static constexpr size_t gCommandModulesCount = sizeof(gCommandModules) / sizeof(gCommandModules[0]);

const CommandModule* getCommandModules(size_t& count) {
  count = gCommandModulesCount;
  return gCommandModules;
}
