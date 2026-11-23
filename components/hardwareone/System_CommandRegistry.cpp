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
#include "System_Camera_DVP.h"
#if ENABLE_MICROPHONE
#include "HAL_Audio.h"
#endif

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
    "(the device's local time, the source that set the clock, and the uptime), temperature and voltage (ESP32 internal "
    "die temp and supply rail), taskstats/perftop (FreeRTOS task and live loop/CPU "
    "profiling), fsusage, events (recent system events from the in-memory register "
    "that drives automation event triggers), and the memory tools memsample (snapshot, with memsample "
    "track on|off|reset|status for allocation tracking) and memreport. Control and "
    "power: reboot, ramflush, cpufreq [MHz] to read or set a supported CPU clock, lightsleep "
    "[seconds] for ESP32 light sleep, deepsleep for power-off (reset to wake), and "
    "wait <ms> to pause command-script execution. "
    "timeset sets the clock by hand: a date and time is read as device-local time unless "
    "the word utc follows it, a Unix timestamp is UTC, and the device's own NTP replaces the "
    "time at its next reply. broadcast <message> pushes a line of text to all "
    "connected output interfaces, and factoryreset deletes the user-accounts file so "
    "the first-boot setup wizard re-runs on next reboot while deliberately preserving "
    "WiFi credentials and other settings. Most mutating commands (timeset, cpufreq, "
    "reboot, ramflush, factoryreset, broadcast, lightsleep, deepsleep) require admin.", commands,             &commandsCount, 0, nullptr },
  { "versions",   "Firmware and compatibility versions", "versions lists, one line each, the firmware version and the "
    "versions a separately built part must agree with: the recovery updater, OTA layout, manifest format and data schema "
    "(OTA layouts); the ESP-NOW format, the radio's ESP-NOW stack and the ESP-NOW identity files; the automations "
    "file and backup bundle formats; the BLE console "
    "channel and the phone link, pipe channel and console records (phone-pipe builds); the UART link protocol and the CM5 control "
    "protocol; the "
    "C6 companion's ESP-Hosted and bridge versions with the ones this image expects (P4); the local STT model; and the "
    "ESP-IDF and Arduino core it was built with. An item is shown only where the build includes that part. versions "
    "json returns the same as one object with stable keys, also carried as the versions object of status json.",
    versionsCommands, &versionsCommandsCount, 0, nullptr },
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
#if ENABLE_ESPNOW_SEND
  { "catchup",    "File catch-up: logs this device offers its named masters",
    "A worker that names a master (espnowmasterfingerprint, and the backup) offers its "
    "whitelisted files to them over ESP-NOW: sensor captures, events.log, battery.csv, "
    "health files sealed at rest, errors and crash history, and the system log and saved "
    "transcripts sealed on send with the capture key. Login logs, the command audit, "
    "QuickNotes and keys are never offered. The masters pull on their own when the device "
    "is back in range; a file counts as delivered once one of them has it. espnowcatchup "
    "lists the offered files and what was taken; espnowcatchupserve turns serving off.",
    catchupCommands,      &catchupCommandsCount, 0, nullptr },
#endif
#if ENABLE_ESPNOW_COLLECT
  { "collect",    "File catch-up: pull the files of the devices that name this hub",
    "A mesh master or backup master built as a collector pulls the files each device "
    "that names it offers (file catch-up) onto its SD card under /sd/collect/<mac>/files/, "
    "when the device's session comes back, every 15 minutes and on collectnow. It needs a "
    "writable card and a set clock. collectstatus shows each device and its files, "
    "collectnow starts a pull, collect on|off stops or starts pulling, collectrate and "
    "collectquota set the frame rate and the card space per device.",
    collectCommands,      &collectCommandsCount, 0, nullptr },
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
    "entirely configuration: broker host/port (mqtthost, mqttport), credentials "
    "(mqttuser, mqttpassword), TLS mode and CA path, base/discovery topics, publish "
    "interval, and a long list of per-source publish toggles (mqttpublishthermal, "
    "mqttpublishimu, and so on). These are persisted settings and most config commands "
    "are admin-only; after changing them, reconnect with closemqtt/openmqtt to apply to "
    "a live session. openmqtt and closemqtt start and stop the client, mqttstatus shows "
    "connection state, and mqttautostart controls whether it connects at boot. For "
    "inbound data, enable mqttsubscribeexternal with mqttsubscribetopics; values "
    "received from those topics are cached and read back with mqttexternalsensors.", mqttCommands,         &mqttCommandsCount, 0, nullptr },
#endif
  #if ENABLE_BLUETOOTH
  { "bluetooth",  "Bluetooth LE control and status", "The Bluetooth radio runs one role at a time. "
    "openble brings up the shared client stack (the host, its controller link and the setup "
    "worker; like openwifi) and closeble takes the whole radio down again; the stack stays up "
    "until closeble, never because the last device closed. On the stack each "
    "device opens and closes on its own: openg2/closeg2 for the Even G2 glasses (the even_g2 "
    "commands then apply), openring/closering for the R1 ring, openphone/closephone for the "
    "phone link; closing one leaves the others and the stack alone, and a device out of range "
    "stays open and reconnects. openconsole starts the app console server instead (legacy: the "
    "device advertises the App console and the HardwareOne app connects to it; it closes the "
    "three devices first and never coexists with the client stack) and closeconsole stops it. "
    "blemode stores the boot role only (client, the default, or server). The phone link carries "
    "the relay (the LLM fallback and the Terminal, while the app forwards) and the app console "
    "as a stream inside it, which the device offers when its phone secret is a device code from "
    "phone pair new and bleconsole is on (bleconsole on|off|status); every console session there "
    "signs in. Beside the glasses the console can also ride the link's own command service "
    "(Secure Channel only, never advertised). blestatus and bleinfo show the role running now, "
    "the host state, the app console and the Phone link block (state, console, relay, streams "
    "and session end reasons; phone status has the pipe's detail). blesend "
    "pushes a one-off message to the connected app (on the phone link too); in server mode "
    "bleevent sends an event "
    "and blestream toggles periodic pushes as a bitmask of sensors/system/events "
    "(blestream on/off/sensors/system/events, plus interval) -- all of which require an "
    "active connection. An app-layer Secure Channel (X25519 + passphrase + "
    "ChaCha20-Poly1305, independent of BLE bonding) is configured with blesecret and "
    "required with blesecure; both require super admin, as does blerequireauth. Boot "
    "reconnection to saved-MAC peers is per-peer via bleautoreconnect <name> [on|off] "
    "(see blepeers for names).", bluetoothCommands, &bluetoothCommandsCount, 0, nullptr },
#if ENABLE_BLE_PHONE_PIPE
  { "phone",      "Android phone link", "The Android phone as a BLE device, reached through the "
    "authenticated phone pipe. The pipe is a device on the shared client stack: openphone opens "
    "the phone link (bringing the stack up if it is down; it opens by itself at boot and after "
    "openble when its auto-reconnect is on) and closephone closes it, with the app console on "
    "its link, leaving the glasses, the ring and the stack as they were; the app console can "
    "ride its link (bleconsole). phone status [json] shows the link. First-time pairing: sign in as a super admin on serial "
    "or the OLED and run phone pair new (or Bluetooth > Phone > New code and pair): the device "
    "replaces the phone secret with a 16-symbol code, shows it once (printed on serial, or on its "
    "own OLED screen, never in a log) and keeps pairing open for up to 3 minutes, until the first "
    "success; type the code in the app. Only such a device code offers the app console on the "
    "phone link. A passphrase of your own set with phone secret <passphrase> (super admin, not "
    "over Bluetooth) carries the relay only; then phone pair from serial or the OLED opens a 60 s "
    "window. openphone, closephone and phone forget act on the paired phone; openphone, phone "
    "pair, phone pair new and phone forget refuse while the app console server runs "
    "(closeconsole first). bleautoreconnect phone [on|off] reconnects it at boot. phone "
    "scan shows or sets the reconnect scan and phone stats the link counters.",
    phoneCommands, &phoneCommandsCount, 0, nullptr },
  { "relay",      "What the phone link carries", "The phone pipe relays TCP for HardwareOne's network "
    "users: the LLM HTTP source and the G2 Terminal. relay status shows how their requests reached "
    "the network (Wi-Fi or the phone), with outcomes and the last eight requests.",
    relayCommands, &relayCommandsCount, 0, nullptr },
#else
  { "phone",      "Android phone link (not in this build)", nullptr, phoneCommands, &phoneCommandsCount, 0, nullptr },
  { "relay",      "Phone relay (not in this build)", nullptr, relayCommands, &relayCommandsCount, 0, nullptr },
#endif
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
    "logtier reports where log lines go: to the card first, sealed with the device "
    "key, or to flash when no card takes them.", filesystemCommands,   &filesystemCommandsCount, 0, nullptr },
#if ENABLE_SD_CARD
  { "sd",         "SD card mount, format, and info", "Controls the optional microSD card, which mounts at /sd and serves as "
    "overflow/bulk storage (and is only compiled in on boards that wire a "
    "supported SPI or SDMMC slot). sdmount attempts to mount the card and sdunmount safely "
    "unmounts it; on a board with a card-detect switch a card is also mounted when it is "
    "put in; sdinfo shows the card type, size, and used/free space, and sddiag "
    "reports backend-specific diagnostics to troubleshoot a card that will not mount. "
    "sdformat erases the entire card and reformats it as FAT32 and therefore requires "
    "sdformat confirm to proceed. Once mounted, file commands address the card through "
    "its /sd/... path prefix.", sdCommands,           &sdCommandsCount, 0, []() { return VFS::isSdAvailable(); } },
#endif
  { "oled",       "Local display control and graphics", "Drives the selected local display backend: its lifecycle, the live screen contents, "
    "and persistent appearance settings. openoled/closeoled "
    "power the display task on and off, and oledstatus "
    "reports its state. oledmode <mode> switches the live screen among the "
    "built-in views (menu, status, sensordata, thermal, network, mesh, gps, espnow, "
    "memory, off, and more); oledtext <message> shows custom text and oledanim "
    "<name>|fps <n> picks the animation -- both require the display to be running (run "
    "openoled first) and neither persists across reboot. Separately, the oled* config "
    "commands write settings to flash immediately: oleddefaultmode sets the idle "
    "default screen, while oledbrightness <0-255>, oledflip, oledbootduration, "
    "oledupdateinterval and oledthermalscale tune appearance and timing, and "
    "oledautostart says whether the display comes up at boot. Whether a user "
    "must log in at the display before interacting with it is set with "
    "displayrequireauth <0|1> (super admin, Settings module).", oledCommands,        &oledCommandsCount, 0, nullptr },
#if ENABLE_SPEAKER
  { "speaker", "Onboard ES8311 speaker", "speakertone plays a short test tone; frequency, duration and volume are optional.", speakerCommands, &speakerCommandsCount, 0, nullptr },
#endif
#if ENABLE_LED_LIGHTS
  { "neopixel",   "RGB LEDs: colours, effects, brightness", "One set of commands for every RGB LED this "
    "build has, the LED named by an optional first word: onboard (the board's own NeoPixel), encoder "
    "(every pixel of the encoder board) or encoder:<p> (one pixel, 1-based); encoder board n (2-4) is "
    "encoder<n> and encoder<n>:<p>. Left out, it is onboard, or "
    "the encoder on a board without an onboard LED. ledcolor [target] <colour> lights it a named colour "
    "(red, green, blue, yellow, magenta, cyan, white, orange, purple, pink and ~70 more, or off); ledclear "
    "[target] turns it off. ledeffect [target] <fade|pulse|blink|rainbow|strobe|off> [color] [color2] "
    "[duration 100-60000ms] runs an effect (defaults: red/blue, 3000 ms) and returns at once; the onboard "
    "LED goes dark when it ends, the encoder's pixels show their colour again. ledbrightness [onboard|encoder[<n>]] "
    "<0-100> is saved per LED and applies at once (admin). ledstatus [json] lists every LED of the build, "
    "connected or not, and what it shows. Colours and effects are live and not saved; the onboard "
    "startup effect lives in the led module.", neopixelCommands,     &neopixelCommandsCount, 0, nullptr },
#endif  // ENABLE_LED_LIGHTS
#if ENABLE_NEOPIXEL
  { "led",        "Onboard LED startup effect", "Configures the effect the board's onboard LED plays at "
    "startup. These are persistent settings written to flash: ledstartupenabled <0|1> toggles the boot "
    "effect, and ledstartupeffect <none|rainbow|pulse|fade|blink|strobe> with ledstartupcolor, "
    "ledstartupcolor2, and ledstartupduration <100-10000ms> define what plays on power-up. (The live "
    "colour, effect and brightness commands are in the neopixel module.)", ledCommands,           &ledCommandsCount, 0, nullptr },
#endif  // ENABLE_NEOPIXEL (led module)
#if ENABLE_LED_MATRIX
  { "matrix", "HT16K33 monochrome LED matrix", "matrix test draws an orientation pattern. matrix text HI, matrix pixel 3 2 on, "
    "matrix clear/fill and matrix on/off control the display. matrix brightness <0-15> and matrix rotation <0-3> persist; "
    "matrix blink <0-3> selects off/2Hz/1Hz/0.5Hz. matrixbus <0|1> and matrixaddress <0x70-0x77> require reboot. "
    "matrix size 8x8 uses one square; matrix panel <0|1> selects the physical square. matrix size 16x8 restores both. "
    "matrixbrightness/matrixrotation/matrixsquare/matrixpanel save without applying (used by the settings editors). "
    "Use an unused non-0x70 address when sharing a bus with a PCA9685. Static text clips at the display edge.",
    matrixCommands, &matrixCommandsCount, 0, nullptr },
#endif
#if ENABLE_ENCODER
  { "encoder", "Encoder board (Adafruit 5752 or 4991)", "An Adafruit seesaw encoder board on I2C, used for "
    "its RGB pixels: the 5752 Quad Rotary Encoder (4 pixels, address 0x49, jumpers to 0x50) or the 4991 "
    "Rotary Encoder with NeoPixel (1 pixel, address 0x36, jumpers to 0x3D), told apart by the product id "
    "the board reports. The knobs and switches are not used. openencoder and closeencoder open and close "
    "it (a closed board goes dark); encoderautostart [on|off] opens it after boot. Its pixels take the LED "
    "commands with the encoder target: ledcolor encoder <colour>, ledcolor encoder:2 red, ledeffect encoder "
    "pulse red, ledbrightness encoder <0-100>, ledclear encoder; board n (2-4) is ledcolor encoder<n> red. "
    "encoderstatus [json] shows the connection, "
    "product, pixel count, address, bus and brightness. encoderaddress applies at the next open, encoderbus "
    "<0|1> after a reboot. A board that stops answering closes itself and restarts on the sensor retry "
    "schedule; an LED command to it reconnects a board that is meant to be open.",
    encoderCommands, &encoderCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("encoder"); } },
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
  { "imu",        "IMU (motion sensor)", "The IMU is the motion sensor on I2C; the build picks its chip (a BNO055 "
    "or an Adafruit 4517 LSM6DSOX + LIS3MDL). openimu starts it, imuread reports "
    "yaw/pitch/roll (degrees) plus acceleration, gyroscope, magnetometer and chip "
    "temperature, and closeimu stops it; imuautostart [on|off] persists launching it at "
    "boot, imubus and imuaddress say where it is wired, and imustatus shows the chip, its bus and "
    "addresses and whether yaw is a magnetic heading. Beyond raw orientation, imuactions runs "
    "gesture/event detection derived from the motion data: shake, tilt (with "
    "direction), tap/knock, rotation (with axis), freefall, a step counter with "
    "cadence, and screen-style orientation. Because the chip can be mounted in any "
    "pose, imuorientationmode <0..8> applies a fixed remap (flip pitch/roll/yaw, "
    "90-degree rotations, upside-down fixes), imuorientationcorrection <0|1> toggles "
    "that correction, and imupitchoffset/imurolloffset/imuyawoffset trim each axis in "
    "degrees.", imuCommands,          &imuCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("imu"); } },
#endif
#if ENABLE_OLED_INPUT
  { "input",      "Input device (gamepad, encoder, touch or IMU)", "Device-agnostic abstraction for the OLED input controller, which is the "
    "Seesaw gamepad, ANO encoder, GPIO rotary encoder, GT911 touchscreen or the IMU (taps and tilt) -- chosen at compile time via "
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
  { "anoencoder", "ANO rotary encoder — status, reading and driver settings", "Adafruit ANO directional navigation rotary encoder (5740) on seesaw I2C: a click wheel "
    "with a center IN press and UP/DOWN/LEFT/RIGHT buttons, used as the OLED navigation "
    "input. The wheel scrolls up and down; IN is A, LEFT is B, DOWN is X, UP is Y, a RIGHT "
    "tap is SELECT (sent when RIGHT is released) and RIGHT held with IN is START. The "
    "open/close/autostart/poll lifecycle lives under the input module and the bus is set "
    "with inputbus. anoencoderstatus [json] shows the bus, address, settings and whether it "
    "is open; anoencoderread [json] gives the wheel position and buttons. "
    "anoencoderi2caddr <0x49..0x50> sets the address (A0-A2 jumpers; refused when another "
    "device of this build uses it on the input bus; applies at the next openinput), "
    "anoencoderinvert [on|off] reverses rotation, and anoencoderswapud / anoencoderswaplr "
    "[on|off|toggle] swap the UP/DOWN and LEFT/RIGHT button pairs. The polling task reads "
    "the buttons, the seesaw's change flags (so taps shorter than one poll are kept) and the "
    "wheel's detents in one checked transaction. Mutually exclusive at build time with "
    "the Seesaw gamepad.", anoEncoderCommands, &anoEncoderCommandsCount, CMD_MODULE_SENSOR, []() { return gAnoEncoderConnected; } },
#endif
#if ENABLE_IMU_INPUT
  { "imuinput",   "IMU as the input device — taps are buttons, tilt is the joystick", "The motion sensor used as the OLED navigation "
    "input (INPUT_DEVICE_TYPE 5; it needs the Adafruit 4517, whose chip detects taps). Tap the device: one tap is A, two are B, "
    "three are Y, four are X; the button arrives once no further tap has followed for the tap window (imuinputtapms "
    "<200..800>, default 350 ms). Five taps take the device's present pose as the joystick's centre, as does "
    "imuinputzero; six or more taps do nothing. Tilting the device away from that centre pose by more than the tilt angle "
    "(imuinputtilt <8..40> degrees, default 15) deflects the joystick left, right, up or down, one direction at a time, "
    "and returning releases it; a lean of more than 60 degrees is taken as a new pose and centres the joystick until the "
    "next re-centre. imuinputswapxy, imuinputinvertx and imuinputinverty [on|off|toggle] fit the directions to how the "
    "sensor sits in its enclosure. A sustained shake is START (imuinputshake [on|off]); SELECT has no gesture. "
    "imuinputread [json] shows the state, the lean in degrees, the joystick and the taps being counted. The "
    "open/close/autostart lifecycle lives under the input module: openinput also opens the IMU and holds it open "
    "(closeimu refuses while the input runs), closeinput leaves the IMU open.", imuInputCommands, &imuInputCommandsCount, CMD_MODULE_SENSOR, []() { return gInputConnected; } },
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
    "sets output level, and fmradiomute on|off mutes and unmutes audio. The OLED FM "
    "screen drives all of this from the gamepad: L/R tune, Up/Down seek, A mute, "
    "Y volume, X power.", fmRadioCommands,      &fmRadioCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("fmradio"); } },
#endif
#if ENABLE_RTC_SENSOR
  { "rtc",        "DS3231 precision RTC", "DS3231 precision I2C real-time clock with battery backup and an on-chip "
    "temperature sensor. Lifecycle: openrtc starts the RTC task, rtcread [status|temp] "
    "reads the clock (or die temperature), and closertc stops it; rtcautostart [on|off] "
    "persists boot auto-start and the module appears in help only when detected. rtcset "
    "reads its argument as timeset does (\"YYYY-MM-DD HH:MM:SS\" is device-local time unless "
    "the word utc follows it; a bare Unix timestamp is UTC; years 2020 to 2099), writes it to "
    "the chip in UTC, computing day-of-week automatically, and sets the system clock to the "
    "same time. rtcsync [to|from] moves time "
    "between the RTC and the system clock: to (the default) copies RTC -> system, and is "
    "refused while the clock is set from a source that outranks the chip (the device's own "
    "NTP, a manual time, an NTP-backed mesh master, the CM5); from "
    "copies system -> RTC (use this after an NTP sync to persist accurate time into the "
    "battery-backed chip). Setting the time via rtcset, rtcsync from or rtcsync to marks the "
    "RTC as set, so later boots take its time on their own; a chip that this device has not "
    "set, or that lost power since (its oscillator-stopped flag), is not adopted at boot.", rtcCommands,          &rtcCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("rtc"); } },
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
    "live level meter until a key is pressed. micrecord start [vad [<ms>] [trim]]|stop "
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
    "LittleFS; eimodellist/eimodelinfo/eimodelunload manage them), or by the Edge Impulse "
    "start-at-boot setting, which loads the default model at boot. eidetect "
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
  { "transcription", "Transcription interfaces", "Session controls and private transcript pages shared by display and web interfaces.", transcriptionUiCommands, &transcriptionUiCommandsCount, CMD_MODULE_SENSOR, nullptr },
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
    "listens for a known command phrase. Note that srenable only reports the "
    "compile-time build flag and cannot toggle the "
    "feature at runtime; the real "
    "lifecycle commands are opensr to start the recognition pipeline and "
    "closesr to stop it. Starting the pipeline also arms voice command execution "
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
    "enable, disable, delete, run, trigger, sanitize, recompute); automationlist lists "
    "the jobs for any signed-in user. Note "
    "the important distinction: automation run id=<id> executes a job commands "
    "immediately, whereas automation trigger id=<id> only arms an afterDelay/manual "
    "timer so it fires after its delay; and automation system enable|disable|status is "
    "the global master switch that gates whether the scheduler runs at all, independent "
    "of each job own enabled flag. Jobs may also include an optional condition "
    "expression, and conditional commands use an IF <expr> THEN <command> [ELSE "
    "<command>] form (e.g. IF temp>75 THEN ledcolor red). Conditions can check sensors "
    "(temp, distance, light, motion, and the IMU: imu_pitch, imu_roll, imu_tilt, imu_pose, "
    "imu_tap_age), time (time, hour, day, ntp), system state "
    "(battery, heap, psram, fsfree, uptime, chiptemp), connectivity (wifi, rssi, peers, "
    "ble), ESP-NOW/bond (espnow, bond_mode, bond_paired, bond_online, bond_synced, "
    "bond_role, bond_rssi, bond_peer_heap, bond_peer_uptime, pairmode, pairmode_secs, "
    "peersknown, stalestpeerage), location (gps, speed, sats, wp_dist:<name>), the on-device model (llm), "
    "and ESP-NOW metadata "
    "(room, zone, tags); numeric variables use the > < = >= <= != operators and "
    "string/enum variables use = != CONTAINS. A true condition fires every time its "
    "trigger is due. Supporting commands: validateconditions checks conditional syntax without "
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
    "changes only take effect after a reboot (for example httpsenabled is marked "
    "reboot required). The controls command emits a "
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
  { "health",     "Local logging of R1 ring vitals", "HardwareOne's own CSV logger for R1 ring vitals. "
    "healthlogging on|off|toggle drives it independently of the ring's own health-collection "
    "privacy setting, healthlogging status shows it, and healthlogmerge stitches captures together. "
    "The ring's live vitals, refresh and history are the ring module (ring health).",
    healthCommands, &healthCommandsCount, 0, nullptr },
#endif
  { "users",      "User authentication and management", "The users subsystem provides admin-gated account management, authentication, "
    "sessions, and bans. Accounts have two roles, admin and standard; the first account "
    "is created as the owner with the superadmin role, and userpromote/userdemote change roles while useradd creates "
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
  { "features",   "System feature management", "The features subsystem lists the compiled-in capabilities and their memory cost; "
    "it changes nothing. features with no argument lists every feature grouped by "
    "category (Network, Display, Sensors, System) with an approximate heap estimate "
    "and four states: built in, running now, start at boot and allowed (- when the "
    "feature has no such switch or state; only the I2C bus has an allowed switch, "
    "i2cbusenabled, and takes it at the next restart). features <id> shows one feature "
    "and names the commands that switch it: open<x>/close<x> start or stop it now and "
    "<x>autostart sets start at boot (a few use other names, such as eicontinuous, "
    "llmload and automation system enable|disable). A few rows are fixed when the "
    "firmware is built. features "
    "<id> <on|off> is refused with those command names. features json is the "
    "capability list for the app. featuresetup launches an interactive, admin-only "
    "wizard that sets which features start at boot, and works from "
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
    "brightness; power mode <perf|balanced|adaptive|ultra|locked|0-4> "
    "selects one of five target-specific presets: Performance, Balanced, Adaptive "
    "(the default), UltraSaver, or Locked. Run power or power json to see their active "
    "and idle MHz. A mode sets the clock while the device is in use and while idle, plus "
    "the display brightness; the chosen mode is persisted. Every mode runs "
    "transcription, camera and recording at the maximum clock while they are busy and "
    "drops back when they finish; Adaptive stays at the floor clock otherwise. Locked "
    "keeps the maximum clock through idle. UltraSaver uses its lower idle-only clock "
    "once the local display blanks; input or a command restores the target's "
    "interactive floor. A command from the glasses, phone, web or serial keeps the clock "
    "up but does not light the local display; only local input, the display's own "
    "commands and on-device voice do. Nothing switches the mode on battery "
    "level in this firmware: power auto and power threshold report that automatic "
    "mode is not available. Two related idle "
    "controls are separate commands: powersleep enters display idle immediately without changing the timeout; powersave <0..1440> sets an idle timeout (minutes; "
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
  { "even_g2",    "Even G2 smart glasses control", "This subsystem drives Even Reality G2 smart glasses in the Glasses mode "
    "(blemode client or glasses: the glasses, plus the phone link whenever a phone secret is "
    "set, with the app console on that link); the two temples are addressed as left/right/auto. openg2 "
    "starts scan-and-connect IN THE BACKGROUND and returns immediately -- it does not "
    "block, so poll g2status (or g2info for firmware/MAC/battery) to see when the link "
    "is up, and nearly every other command here requires that connection first. Display "
    "commands render to the lens: g2show prints text, g2ai/g2ainoask/g2aidirect push "
    "a front-pane AI answer card through the EvenAI pipeline, g2bmp shows a BMP file, "
    "and g2sensors/g2network/g2files/g2settingspage show built-in info pages; g2nav "
    "[on|off] enables menu-navigation mode and g2clear blanks the display. For audio, "
    "use the shared Microphone controls: micsource selects auto, pdm or g2; "
    "openmic/closemic start and stop the selected source; micrecord controls WAV "
    "recording. g2micon/g2micoff are low-level stream probes; g2micrec and g2micwav "
    "remain SD-backed raw-LC3 and decoded-WAV diagnostics. closeg2 disconnects but keeps "
    "the GATT cache for fast reconnect; closeg2 full also frees the cache to recover "
    "about 30 KB. The glasses' own stock widget can show system metrics instead of share prices: "
    "g2 stats status and g2 stats preview show the rows and bytes; g2 stats send (admin) sends one "
    "batch; g2 stats feed [on|off] (admin, saved, off by default) feeds it on change, g2 stats "
    "metrics <labels> and g2 stats interval <seconds> (admin, saved) choose what and how often; "
    "the chart is the last 185 points (kept while the glasses are away), one every g2 stats step "
    "[10|20|60] seconds (admin, saved; default 20, about an hour; a change starts the chart over), "
    "and g2 stats pad [gap|flat] (admin, saved) fills its left part until the window is full. "
    "The glasses' QuickList can carry Wear Detection, Silent Mode, Auto Brightness, Head-Up Display "
    "and Brightness rows (firmware 2.3.2.14): g2 list "
    "status shows them; g2 list feed [on|off] (admin, saved, off by default) sends them after each "
    "connect, g2 list push (admin) sends them now, each time replacing the glasses' whole list (items "
    "added in the Even app are removed), g2 list preview (admin) shows their bytes; ticking a "
    "row and leaving the list switches that setting (Brightness turns auto brightness off and steps "
    "through 25, 50, 75 and 100 percent). "
    "The remaining g2* commands are low-level protocol probes and "
    "diagnostics (g2probe, g2protostats, g2devcfg, g2dumpframes).", g2Commands,           &g2CommandsCount, 0, nullptr },
  { "ring",       "Even R1 ring link, health and controls", "The Even R1 smart ring over BLE, through a serialized, "
    "profile-gated transaction owner. ring scan [seconds] finds it and openring [saved|reconnect|<mac>] "
    "connects (scanning first when no argument is given); ring status [json] shows the link and "
    "closering ends it. ring health [json] reads vitals, ring controls and typed history; ring "
    "poll refreshes the "
    "vitals and ring history the history (ring history force, admin, skips the freshness window). "
    "ring query requests one reading; its raw form is admin-only, confirms every SET and refuses "
    "user-profile writes. ring control <collection|lowpower> <preserve|on|off> (admin) saves a desired "
    "state applied asynchronously, which the OLED and web Health controls also use; ring control "
    "refresh reads low power back. The G2's own Health panel is fed the way the Even app feeds it: "
    "ring panel status and ring panel preview [test] show the snapshot and its bytes; ring panel send "
    "[test] (admin) sends one; ring panel feed [on|off] (admin) feeds it automatically, saved and on by default, refreshing ring history every "
    "ring panel history [minutes] (admin, default 30). Its insights page gets up to three lines from "
    "ring history (heart rate and HRV, steps, sleep or SpO2): ring insights status and ring insights "
    "preview show them and their bytes; ring insights send (admin) sends them; ring insights feed "
    "[on|off] (admin, saved, on by default, runs with the panel feed) and ring insights days [1-7] "
    "(admin, default 3) set the feed and its window.", g2RingCommands, &g2RingCommandsCount, 0, nullptr },
#endif
#if ENABLE_LLM_BACKEND
  { "llm",        "On-device LLM text generation", "On-device large language model that runs a quantized model file entirely on the "
    "device (model weights held in PSRAM). A model must be loaded before generation: "
    "llmload [file.bin] loads one (bare filenames are looked up in internal /system/llm "
    "first, then on the SD card under /sd/llm), llmmodels lists available files, llmunload "
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
