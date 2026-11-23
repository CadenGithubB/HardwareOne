# Command Reference

> **Generated file — do not edit by hand.**
> Regenerate with `python3 tools/command_registry.py reference`.
> Source of truth is the `CommandEntry` tables in `components/hardwareone/*.cpp`.

1031 commands across 60 modules (1031 registry entries).

Commands are matched case-insensitively, and lookup uses longest-prefix matching, so `automation list` resolves to the `automation` dispatcher with `list` as its argument.

Legend: **A** = requires admin &nbsp; **S** = requires super admin

## Modules

- [`cli`](#cli) — 4 commands
- [`system`](#system) — 25 commands
- [`versions`](#versions) — 1 commands
- [`wifi`](#wifi) — 19 commands
- [`espnow`](#espnow) — 126 commands
- [`catchup`](#catchup) — 2 commands
- [`collect`](#collect) — 5 commands
- [`c6`](#c6) — 9 commands
- [`mqtt`](#mqtt) — 26 commands
- [`bluetooth`](#bluetooth) — 21 commands
- [`phone`](#phone) — 10 commands
- [`relay`](#relay) — 2 commands
- [`filesystem`](#filesystem) — 10 commands
- [`sd`](#sd) — 5 commands
- [`oled`](#oled) — 15 commands
- [`speaker`](#speaker) — 1 commands
- [`neopixel`](#neopixel) — 5 commands
- [`led`](#led) — 5 commands
- [`matrix`](#matrix) — 7 commands
- [`encoder`](#encoder) — 6 commands
- [`servo`](#servo) — 6 commands
- [`thermal`](#thermal) — 23 commands
- [`tof`](#tof) — 10 commands
- [`imu`](#imu) — 16 commands
- [`input`](#input) — 6 commands
- [`gamepad`](#gamepad) — 1 commands
- [`anoencoder`](#anoencoder) — 6 commands
- [`apds`](#apds) — 9 commands
- [`gps`](#gps) — 6 commands
- [`fmradio`](#fmradio) — 9 commands
- [`rtc`](#rtc) — 7 commands
- [`presence`](#presence) — 6 commands
- [`camera`](#camera) — 45 commands
- [`microphone`](#microphone) — 17 commands
- [`edgeimpulse`](#edgeimpulse) — 15 commands
- [`transcription`](#transcription) — 2 commands
- [`note`](#note) — 1 commands
- [`stt`](#stt) — 5 commands
- [`espsr`](#espsr) — 43 commands
- [`i2c`](#i2c) — 24 commands
- [`automation`](#automation) — 5 commands
- [`battery`](#battery) — 3 commands
- [`debug`](#debug) — 181 commands
- [`settings`](#settings) — 19 commands
- [`sensorlog`](#sensorlog) — 3 commands
- [`health`](#health) — 2 commands
- [`users`](#users) — 22 commands
- [`features`](#features) — 2 commands
- [`image`](#image) — 4 commands
- [`map`](#map) — 12 commands
- [`mapsettings`](#mapsettings) — 3 commands
- [`power`](#power) — 4 commands
- [`liveaudio`](#liveaudio) — 1 commands
- [`cm5`](#cm5) — 15 commands
- [`ota`](#ota) — 9 commands
- [`setpattern`](#setpattern) — 1 commands
- [`even_g2`](#even-g2) — 68 commands
- [`ring`](#ring) — 22 commands
- [`llm`](#llm) — 36 commands
- [`settingsedit`](#settingsedit) — 58 commands

---

## cli

> The cli module is the on-device help and CLI navigation layer, not a feature subsystem. help opens a paged help browser: bare help shows the main menu listing every registered module, help <module> drills into page 1 of that module (and prints its subsystem overview at the top), and help <module> p<N> opens an explicit later page. The special topics help sensors (aggregate view across all sensor modules), help all (show every command including hidden ones), and help tail (dump suppressed output) cover the rest. While the browser is open the CLI is in a help state, so p<N>, next and prev move within a module, back steps from a module page up to the main menu, and exit leaves help mode entirely and returns to the normal prompt, and clear wipes the CLI scrollback/history.

| Command | | Description |
| ------- | :-: | ----------- |
| `back` |  | Return to main help menu |
| `clear` |  | Clear CLI history |
| `exit` |  | Exit help mode |
| `help` |  | Display help menu (module results use p<N> pages)<br/>`Usage: help [<module> [p<N>]\|sensors\|all\|tail] Example: help espnow p2 In help mode: p<N>, next, prev, back, exit` |

## system

> The system module holds core device commands that do not belong to any peripheral. Status and inspection: status (WiFi, filesystem, memory summary), uptime, time (uptime plus NTP wall-clock if synced), temperature and voltage (ESP32 internal die temp and supply rail), taskstats/perftop (FreeRTOS task and live loop/CPU profiling), fsusage, events (recent system events from the in-memory register that drives automation event triggers), and the memory tools memsample (snapshot, with memsample track on|off|reset|status for allocation tracking) and memreport. Control and power: reboot, ramflush, cpufreq [MHz] to read or set a supported CPU clock, lightsleep [seconds] for ESP32 light sleep, deepsleep for power-off (reset to wake), and wait <ms> to pause command-script execution. timeset sets the clock manually. broadcast <message> pushes a line of text to all connected output interfaces, and factoryreset deletes the user-accounts file so the first-boot setup wizard re-runs on next reboot while deliberately preserving WiFi credentials and other settings. Most mutating commands (timeset, cpufreq, reboot, ramflush, factoryreset, broadcast, lightsleep, deepsleep) require admin.

| Command | | Description |
| ------- | :-: | ----------- |
| `bootcount` |  | Show boot count (NVS), crash count, last reset reason. 'bootcount reset' zeroes it (admin). (add 'json')<br/>`Usage: bootcount [reset\|json]` |
| `broadcast` | A | Send a message to all connected output interfaces.<br/>`Usage: broadcast <message>` |
| `cpufreq` | A | Get/set CPU frequency (admin).<br/>`Usage: cpufreq [MHz] (run without arguments to list supported values)` |
| `crashlog` |  | Show the last recorded crash (panic text, core/PC, boot phase, repeat count). (add 'json')<br/>`Usage: crashlog [json]` |
| `deepsleep` | S | Power off via deep sleep (no wake source — reset button to wake).<br/>`Usage: deepsleep (super admin; wakes only via physical reset)` |
| `events` |  | Show recent system events (the in-memory register that drives automation event triggers).<br/>`Usage: events [kinds [json]] (bare): show the recent-event ring kinds: list every valid event-kind name (json = machine form)` |
| `factoryreset` | S | Wipe user accounts and their data (all: every stored setting and file) and reboot to setup wizard.<br/>`Usage: factoryreset [all] (confirmation required) Deletes /system/users/users.json, pending account requests, every user's settings file and every account's transcripts and notes (/stt/u*, /sd/stt/u*), so the first-time setup wizard runs on next boot. WiFi credentials and other settings are preserved. factoryreset all (serial console only): also erases the whole internal filesystem and NVS during the restart: saved networks, credentials, keys, Bluetooth bonds, the recovery credential and logs. For handing a device over.` |
| `fsusage` |  | Show filesystem usage. (add 'json' for JSON output) |
| `lightsleep` | A | Enter ESP32 light sleep: lightsleep [seconds] (default 20s).<br/>`Usage: lightsleep [seconds] (1..3600, default 20)` |
| `memdiag` | A | Bench memory diagnostics: [status\|check\|arm\|disarm\|dump\|wptest\|psramtest [seconds]\|noabort]<br/>`Usage: memdiag [status\|check\|arm\|disarm\|dump\|wptest\|psramtest [seconds]\|noabort]` |
| `memreport` |  | Comprehensive memory report (Task Manager style). (add 'json' for JSON output) |
| `memsample` |  | Memory snapshot with component requirements. Use 'memsample track [on\|off\|reset\|status]' for allocation tracking.<br/>`Usage: memsample [track <on\|off\|reset\|status>]` |
| `perftop` |  | Live performance snapshot: loop laps/s, period, per-section timing, worst stalls + live task CPU%. (add 'json' for loop + per-task CPU% JSON) |
| `ramflush` | A | Reboot to reclaim RAM, restoring the features running right now.<br/>`Usage: ramflush [status] (bare): capture which features are running, reboot, and restore them status: show what the last boot restored (no reboot) Restores for that one boot only — a normal reboot returns to configured autostart. Never changes your autostart settings.` |
| `reboot` | A | Reboot the system. |
| `status` |  | Show system status (WiFi, FS, memory). (add 'json' for JSON output) |
| `taskstats` |  | Detailed task statistics (state/prio/stack min-free). (add 'json' for JSON output) |
| `temperature` |  | Read ESP32 internal temperature. (add 'json' for JSON output) |
| `testencryption` | A | Test WiFi password encryption (admin only). |
| `testpassword` | A | Test user password hashing (admin only). |
| `time` |  | Show device time (uptime + NTP if synced). (add 'json' for JSON output) |
| `timeset` | A | Set time manually: timeset YYYY-MM-DD HH:MM:SS or <unix_timestamp>.<br/>`Usage: timeset <YYYY-MM-DD HH:MM:SS>\|<unix_timestamp>` |
| `uptime` |  | Show device uptime. (add 'json' for JSON output) |
| `voltage` |  | Estimate power draw from active subsystems (not a real voltage measurement; use batterystatus for measured volts). (add 'json' for JSON output) |
| `wait` |  | Delay execution for N milliseconds: wait <ms>.<br/>`Usage: wait <ms> (1..60000)` |

## versions

> versions lists, one line each, the firmware version and the versions a separately built part must agree with: the recovery updater, OTA layout, manifest format and data schema (OTA layouts); the ESP-NOW format, the radio's ESP-NOW stack and the ESP-NOW identity files; the automations file and backup bundle formats; the BLE console channel and the phone link, pipe channel and console records (phone-pipe builds); the UART link protocol and the CM5 control protocol; the C6 companion's ESP-Hosted and bridge versions with the ones this image expects (P4); the local STT model; and the ESP-IDF and Arduino core it was built with. An item is shown only where the build includes that part. versions json returns the same as one object with stable keys, also carried as the versions object of status json.

| Command | | Description |
| ------- | :-: | ----------- |
| `versions` |  | Show the firmware version and every version a separately built part must match. (add 'json')<br/>`Usage: versions [json]` |

## wifi

> The WiFi subsystem manages station-mode network connections plus the network services that ride on top of them: NTP time sync and the on-device HTTP/HTTPS server. Saved networks are stored as a prioritized list (wifilist, wifiadd, wifirm, wifipromote) and persist to flash; openwifi connects by best-priority (default) or by --index <N>, and a failed indexed attempt auto-rolls back to the previously connected network. Note two distinct disconnects: closewifi tears down the link AND stops the HTTP server and web output to free heap, while wifidisconnect (drop) leaves the radio and web server up so you can move to another network. closewifi powers the radio down only when ESP-NOW is not using it; 'radiopower off' is the unconditional airplane switch. wifiscan lists nearby APs, ntpsync/ntpstatus handle clock sync, and openhttp/closehttp/httpstatus run the web server (compiled in only when the HTTP server is enabled). certinfo and certgen (admin-only) manage the self-signed HTTPS certificate.

| Command | | Description |
| ------- | :-: | ----------- |
| `certgen` | S | Generate self-signed HTTPS certificate: [rsa] (default: ECDSA P-256)<br/>`Usage: certgen [rsa] Default: ECDSA P-256 (~1s). Use 'certgen rsa' for RSA-2048 (~30-60s).` |
| `certinfo` |  | Show HTTPS certificate details. |
| `closehttp` | A | Stop HTTP server; it stays off across later WiFi joins until openhttp. |
| `closewifi` | A | Disconnect from WiFi (also stops HTTP server + web output to free heap). |
| `httpstatus` |  | Show HTTP server status. (add 'json' for JSON output) |
| `ntpstatus` |  | Show NTP configuration and sync state. |
| `ntpsync` |  | Sync time with NTP server. |
| `openhttp` | A | Start HTTP server. |
| `openwifi` | A | Connect to WiFi: [--best \| --index <N> \| --saved-id <id>] (default: best; ids come from a screen's saved list and are valid for this boot only)<br/>`Usage: openwifi [--best \| --index <1..N> \| --saved-id <id>]` |
| `radiopower` | A | Power the whole 2.4GHz radio on/off (airplane mode; also stops/restores ESP-NOW; runtime only, not persisted): [on\|off\|toggle]<br/>`Usage: radiopower [on\|off\|toggle]` |
| `wifiadd` | A | Add WiFi network: <ssid> <pass> [priority] [hidden]<br/>`Usage: wifiadd <ssid> <pass> [priority] [hidden0\|1]` |
| `wifidisconnect` | A | Disconnect from the current network but keep the radio on (HTTP/web stay up). |
| `wifigettxpower` | A | Report the current WiFi TX power in dBm (read-only; set it with 'wifitxpower').<br/>`Usage: wifigettxpower (no arguments)` |
| `wifilist` |  | List saved WiFi networks. (add 'json' for JSON output) |
| `wifipromote` | A | Promote WiFi to top priority: <ssid> [newPriority]<br/>`Usage: wifipromote <ssid> [newPriority]` |
| `wifirm` | A | Remove WiFi network: <ssid><br/>`Usage: wifirm <ssid>` |
| `wifirmid` | A | Remove a saved WiFi entry by its saved id (ids come from a screen's saved list and are valid for this boot only). Ctrl-C or a lone Esc cancels; arrow keys do not.<br/>`Usage: wifirmid <saved-id>` |
| `wifiscan` |  | Scan for available WiFi networks. (add 'json' for JSON output) |
| `wifistatus` |  | Show current WiFi connection info. (add 'json' for JSON output) |

## espnow

> ESP-NOW links HardwareOne devices directly over the WiFi radio with no router or access point, as named peers that can also form a multi-hop mesh. Pair with espnowpair, then message (espnowsend/espnowbroadcast), push a file (espnowsendfile), pull a file (espnowfetch), browse a peer's files (espnowbrowse), or run a command on a peer (espnowremote). espnowremote, espnowfetch, espnowbrowse, espnowroomcmd and espnowtagcmd are ASYNCHRONOUS: they return OK on delivery; the real result arrives later in the message buffer, read with 'espnowmessages json [mac]'. Mesh mode (espnowmode mesh) adds routing with a TTL and master/worker/backup roles; each device carries identity metadata (name, friendly name, room, zone, tags) queried with espnowdeviceinfo locally or espnowrequestmeta for a peer.

| Command | | Description |
| ------- | :-: | ----------- |
| `bondconnect` | A | EXPERIMENTAL in 1.0. Connect to bonded peer device: 'bondconnect <mac_or_name>'. Run it on both devices. (async - bond establishes when peer is seen; watch bondstatus)<br/>`Usage: bondconnect <mac_or_name> Returns immediately; the bond completes when the peer appears via heartbeat - watch 'bondstatus'. The peer is asked to confirm; until 'bondconnect' is run there too it answers that it is not bonded, and 'bondstatus' says so. A role chosen with 'bondrole' is kept; else the other than the one the peer's user chose; else the higher address is master. Refused when the peer's session shows its build has no bond mode (espnowsessions caps).` |
| `bonddisconnect` | A | Disconnect from bonded peer device. The peer is told that the bond ended. |
| `bondforward` | A | Bond master: forward the bond partner's readings, battery state and events to the mesh master named on this device: 'bondforward [on\|off\|sensors <all\|default\|name,name,...>]'.<br/>`Usage: bondforward [on \| off \| sensors <all \| default \| name,name,...>] No argument shows whether the partner's data is forwarded, and why not. Off by default. Only to the master named here, when it is paired in this device's primary mesh. Input is not forwarded unless named. That master may ask for the partner's readings (espnowsensorreq <partner> via <this device> there).` |
| `bondforwardsensors` | A | Set the sensors a bond master forwards, as a mask: <0..2047><br/>`Usage: bondforwardsensors <0..2047> (bit i: sensor i; 'bondforward sensors <names>' sets it by name)` |
| `bondrequestcap` | A | Request capability summary from bonded peer. (async - remote cap via GET /api/bond/status; note bondshowcap shows LOCAL cap) |
| `bondrequestmanifest` | A | Request full manifest from bonded peer. (async - view with bondshowremotemanifest) |
| `bondrequestschema` | A | Request settings schema from bonded peer. (async - cached; read via GET /api/bond/settings/schema) |
| `bondrequestsettings` | A | Request settings file from bonded peer. (async - cached; read via GET /api/bond/settings) |
| `bondresync` | A | Force re-sync of bond state (cap+manifest+settings+schema). Use when UI is stuck on 'Establishing Bond' or peer state looks stale. (async - results populate as they arrive)<br/>`Usage: bondresync [--cap\|--manifest\|--settings\|--schema\|--all] Returns OK on dispatch; results arrive over time - view via 'bondshowremotemanifest' and GET /api/bond/status, /api/bond/settings, /api/bond/settings/schema.` |
| `bondrole` | A | Get/choose bond mode role: 'bondrole [master\|worker\|auto]' (no arg shows the role and whether it was chosen).<br/>`Usage: bondrole [master\|worker\|auto] (no arg shows the role and how it was decided) master or worker: your choice, kept by bondconnect; a partner whose role is automatic takes the other one. auto: give the choice up (the role stays until the partner chooses or the next bondconnect).` |
| `bondshowcap` |  | Show local device capability summary. |
| `bondshowmanifest` |  | Show local device manifest (UI apps + CLI commands). |
| `bondshowremotemanifest` |  | Show cached remote manifest(s): 'bondshowremotemanifest [fwHash]'.<br/>`Usage: bondshowremotemanifest [<fwHash>]` |
| `bondstatus` |  | EXPERIMENTAL in 1.0. Show bond mode status and configuration, with what the peer answered about the bond (Agreement) and whether its data is forwarded to the mesh. |
| `bondstream` | A | Toggle bond sensor streaming (works on both roles): 'bondstream <sensor> <on\|off>'. WORKER streams its sensor to the bonded master; MASTER commands the bonded worker to start/stop. (local toggle; data lands on the master's espnowsensorstatus)<br/>`Usage: bondstream <sensor> <on\|off> bondstream (show status) On a WORKER: streams this device's sensor to the bonded master. On a MASTER: tells the bonded worker to start/stop that sensor. Streamed data is viewable on the master via 'espnowsensorstatus' / GET /api/sensors/remote.` |
| `bondstreamfmradio` | A | Set auto-stream FM radio: <0\|1><br/>`Usage: bondstreamfmradio <0\|1>` |
| `bondstreamgps` | A | Set auto-stream GPS: <0\|1><br/>`Usage: bondstreamgps <0\|1>` |
| `bondstreamimu` | A | Set auto-stream IMU: <0\|1><br/>`Usage: bondstreamimu <0\|1>` |
| `bondstreaminput` | A | Set auto-stream input device: <0\|1><br/>`Usage: bondstreaminput <0\|1>` |
| `bondstreampresence` | A | Set auto-stream presence: <0\|1><br/>`Usage: bondstreampresence <0\|1>` |
| `bondstreamrtc` | A | Set auto-stream RTC: <0\|1><br/>`Usage: bondstreamrtc <0\|1>` |
| `bondstreamthermal` | A | Set auto-stream thermal: <0\|1><br/>`Usage: bondstreamthermal <0\|1>` |
| `bondstreamtof` | A | Set auto-stream ToF: <0\|1><br/>`Usage: bondstreamtof <0\|1>` |
| `bondtestsensor` | A | Test v3 sensor data transmission (worker only - a master cannot send sensor data): 'bondtestsensor [sensor_type]'. (async - frame appears on the master via espnowsensorstatus)<br/>`Usage: bondtestsensor [thermal\|tof\|imu\|gps\|input\|fmradio\|rtc\|presence] (worker only) Returns OK on send; the test frame appears on the bonded master's remote-sensor cache ('espnowsensorstatus' / GET /api/sensors/remote).` |
| `closeespnow` | A | Deinitialize ESP-NOW and free resources. |
| `closestream` | A | Stop streaming output to ESP-NOW device (admin). |
| `espnowaccept` | A | Accept a pending incoming pair request: 'espnowaccept [<mac_or_name>]'.<br/>`Usage: espnowaccept [<mac_or_name>] (no arg = the sole pending request)` |
| `espnowacceptsensorcontrol` | A | Worker: opt in to honor SENSOR_REQ from the configured master/backup: 'espnowacceptsensorcontrol <on\|off>'.<br/>`Usage: espnowacceptsensorcontrol <on\|off>` |
| `espnowbackupenable` | A | Enable/disable backup master feature: 'espnowbackupenable [on\|off]'.<br/>`Usage: espnowbackupenable [on\|off]` |
| `espnowbackupfingerprint` | S | Name the backup master on this device: its key as 'espnowidentity' shows it (64 hex), or 'clear' to name none.<br/>`Usage: espnowbackupfingerprint [<64-hex-pubkey>\|clear] No argument shows it. The backup named here gets this device's battery state and events, may ask it for readings, and gives it mesh time while it acts as master. clear: no backup master is named.` |
| `espnowbattery` |  | Mesh master: show the battery state other devices sent: 'espnowbattery [json] [<name_or_mac>] [from <n>]'.<br/>`Usage: espnowbattery [json] [<name_or_mac>] [from <n>] A device sends its battery state to the master named on it (espnowpowerinterval). json: 'next' is the number to pass as 'from' when not every device fitted one reply.` |
| `espnowbondmodeenabled` | A | EXPERIMENTAL in 1.0. Enable/disable bond mode: <0\|1><br/>`Usage: espnowbondmodeenabled <0\|1>` |
| `espnowbondpeermac` | A | Set bond peer MAC address<br/>`Usage: espnowbondpeermac <AA:BB:CC:DD:EE:FF>` |
| `espnowbroadcast` |  | Broadcast message: 'espnowbroadcast <message>'. (async send; delivery only, no reply)<br/>`Usage: espnowbroadcast <message> (single frame, <= 202 bytes while any enabled mesh has a passphrase, 218 otherwise; longer text is refused) Fails when the frame reached no peer. There is no per-device delivery count and no per-device reply.` |
| `espnowbroadcaststats` |  | Show broadcast ACK tracking statistics. |
| `espnowbrowse` | A | Browse a peer's files; user/pass are an account ON THE TARGET: 'espnowbrowse <target> <target-user> <target-pass> ["path"]'. (async - result via espnowmessages json)<br/>`Usage: espnowbrowse <target> <target-user> <target-pass> ["path"] [json] Credentials are verified ON THE TARGET device, not this one. Returns OK on delivery; the remote listing arrives later - read with 'espnowmessages json' (match the reqId).` |
| `espnowchannel` | A | Get/set preferred ESP-NOW channel: 'espnowchannel [1-13\|auto\|resync]'. No arg shows a checker (actual vs expected). Set the SAME value on both devices to pair off-grid.<br/>`Usage: espnowchannel [<1..13>\|auto\|resync] (no arg) = show preference, actual radio channel, expected, and an OK/MISMATCH check. auto (0) = a worker follows its master's channel (hunting for it when silent); a master follows WiFi; offline fallback otherwise. 1-13 = force this channel when not joined to WiFi. resync = force the radio back onto the correct channel now (no setting change). Two devices only hear each other on the same channel — set both the same for field use.` |
| `espnowdeviceinfo` |  | Show all local device metadata. |
| `espnowdevices` |  | List all mesh devices with the room/zone/tags they announce and their status; [pending] marks a room or tags not accepted here: espnowdevices [json].<br/>`Usage: espnowdevices [json] json: pendingRoom / pendingTags per device say what of its room and tags is not accepted here; devices that do not fit one reply are counted in notShown.` |
| `espnowdiscovered` | A | List devices found during discovery: 'espnowdiscovered [clear]'. Pair one with 'espnowpairrequest <mac_or_name>'.<br/>`Usage: espnowdiscovered [clear] Lists same-mesh devices heard while your discovery window ('espnowpairmode') is open.` |
| `espnowencstatus` | A | Show ESP-NOW encryption status and key fingerprint. |
| `espnoweventpush` | A | Which of this device's system events go to the mesh master: 'espnoweventpush [off\|alert\|standard\|verbose]'.<br/>`Usage: espnoweventpush [off\|alert\|standard\|verbose] alert: security, safety and faults only (the default). standard: also the everyday ones. verbose: all.` |
| `espnowevents` | A | Mesh master: show the system events other devices sent: 'espnowevents [json] [since <rx>]'.<br/>`Usage: espnowevents [json] [since <rx>] A device sends its events to the master named on it (espnoweventpush). The master keeps the newest 24 per mesh. since <rx>: only events after that receive number. json: 'more' means ask again with since = the last rx.` |
| `espnowfailovertimeout` | A | Set failover timeout: <5000-120000 ms><br/>`Usage: espnowfailovertimeout <5000..120000>` |
| `espnowfetch` | A | Fetch a file from a peer; user/pass are an account ON THE TARGET: 'espnowfetch <target> <target-user> <target-pass> "<path>"'. (async - status via espnowmessages json; file saved on this device)<br/>`Usage: espnowfetch <target> <target-user> <target-pass> "<path>" Credentials are verified ON THE TARGET device, not this one. Returns OK on delivery; status lands in 'espnowmessages json'; the fetched file is written to this device's filesystem.` |
| `espnowfind` |  | Find devices by name, room, or tag: 'espnowfind <query>'.<br/>`Usage: espnowfind <query>` |
| `espnowfirsttimesetup` | A | Set first time setup flag: <0\|1><br/>`Usage: espnowfirsttimesetup <0\|1>` |
| `espnowforget` | S | Forget a peer's crypto identity + close its session: 'espnowforget <name_or_mac>'.<br/>`Usage: espnowforget <name_or_mac>` |
| `espnowforwardlinks` |  | Mesh master: show the devices that forward their bond partner's data to this device: 'espnowforwardlinks [json] [from <n>]'.<br/>`Usage: espnowforwardlinks [json] [from <n>] json: 'next' is the number to pass as 'from' when not every link fitted one reply.` |
| `espnowfriendlyname` |  | Get/set friendly display name: 'espnowfriendlyname [name]'.<br/>`Usage: espnowfriendlyname [<name>] (<=47 chars) espnowfriendlyname clear` |
| `espnowgroupaccept` | A | Accept the room and tags a paired peer announces right now, for room and tag commands: 'espnowgroupaccept [json] <name_or_mac\|all>'.<br/>`Usage: espnowgroupaccept [json] <name_or_mac\|all> Records exactly what the peer announces at this moment; the reply states what was accepted. A name that more than one paired device has is refused: use the address espnowgroups shows. From then on espnowroomcmd and espnowtagcmd send that room's and those tags' commands, with the account password they carry, to this peer.` |
| `espnowgroupdrop` | S | Take back the room and tags accepted for a paired peer: 'espnowgroupdrop [json] <name_or_mac\|all>'.<br/>`Usage: espnowgroupdrop [json] <name_or_mac\|all> The peer is no longer a target of room or tag commands; what it announces is pending again.` |
| `espnowgroups` | A | List paired peers with the room and tags each announces, what was accepted here and what is pending: 'espnowgroups [json] [name_or_mac]'.<br/>`Usage: espnowgroups [json] [<name_or_mac>] announced = what the peer says about itself; accepted = what room and tag commands go by; pending = announced and not accepted, so not used. A peer that stops announcing a room or tag leaves it at once; a new one waits for espnowgroupaccept.` |
| `espnowhbmode` | A | Get/set heartbeat mode: 'espnowhbmode [public\|private]'.<br/>`Usage: espnowhbmode [public\|private]` |
| `espnowheartbeatbroadcast` | A | Set heartbeat broadcast: <0\|1><br/>`Usage: espnowheartbeatbroadcast <0\|1>` |
| `espnowheartbeatinterval` | A | Set master heartbeat interval: <1000-60000 ms><br/>`Usage: espnowheartbeatinterval <1000..60000>` |
| `espnowhistorykb` | A | Message history per peer and direction in KB: [<0-32>] (0 = board default; no argument shows saved and active; reboot required)<br/>`Usage: espnowhistorykb [<0..32>]` |
| `espnowidentity` |  | Show long-term Ed25519 identity (MAC, pub key, createdAtSec, regenCount). |
| `espnowkeyex` | A | Initiate KEY_EX handshake with a peer (runs alongside legacy pairing). (async - handshake completes later; check espnowsessions)<br/>`Usage: espnowkeyex <name_or_mac> [<mesh>] Returns OK when KEY_EX_HELLO is sent; the handshake completes asynchronously - inspect with 'espnowsessions' / 'espnowencstatus'.` |
| `espnowlist` |  | List all paired ESP-NOW devices. |
| `espnowmasterfingerprint` | S | Name the mesh master on this device: its key as 'espnowidentity' shows it (64 hex), or 'clear' to name none.<br/>`Usage: espnowmasterfingerprint [<64-hex-pubkey>\|clear] No argument shows it. The master named here gives this device mesh time, gets its readings, battery state and events, and may ask it for readings. clear: no master is named (the reply says what that stops).` |
| `espnowmeshbackup` | A | Get/set backup MAC: 'espnowmeshbackup [MAC]'.<br/>`Usage: espnowmeshbackup [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmeshes` | A | Manage multi-mesh slots: 'espnowmeshes [list\|add\|remove\|delete\|enable\|setdefault\|rename\|setpassphrase] ...'.<br/>`Usage: espnowmeshes list espnowmeshes add <label> (then set passphrase via 'espnowsetpassphrase <label> <pw>') espnowmeshes remove <label> (alias: disable) espnowmeshes delete <label> (frees the slot; unpair its devices first) espnowmeshes enable <label> espnowmeshes setdefault <label> espnowmeshes setpassphrase <label> <passphrase> espnowmeshes rename <oldLabel> <newLabel>` |
| `espnowmeshmaster` | A | Get/set master MAC: 'espnowmeshmaster [MAC]'.<br/>`Usage: espnowmeshmaster [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmeshmetrics` |  | Show multi-hop routing metrics (flood forwards, routed hops, drops, route churn). |
| `espnowmeshpeermax` | A | Set max peer slots: <1-16> (reboot required)<br/>`Usage: espnowmeshpeermax <1..16>` |
| `espnowmeshrelay` | A | Carry other nodes' mesh traffic: <0\|1><br/>`Usage: espnowmeshrelay <0\|1> 1 (default) = act as a relay so out-of-range peers can reach each other through this node. 0 = stop forwarding for others. This node still uses multi-hop for its OWN traffic (set espnowmeshttl 1 for that), and stops advertising routes so nobody sends via it.` |
| `espnowmeshrole` | A | Get/set mesh role: 'espnowmeshrole [worker\|master\|backup]'.<br/>`Usage: espnowmeshrole [worker\|master\|backup]` |
| `espnowmeshroutes` |  | Show the mesh route table: who this node can reach and via which neighbour.<br/>`Usage: espnowmeshroutes [clear] 'via (direct)' = in radio range. Anything else is reached over one or more relay hops. Multi-hop routes are learned from neighbours every 30s — allow a minute after boot.` |
| `espnowmeshstatus` |  | Show mesh peer health (heartbeats & ACKs). |
| `espnowmeshtime` | A | Mesh time on/off: the master sends its clock, devices that name it as their master take it when they have no better source: 'espnowmeshtime [on\|off]'.<br/>`Usage: espnowmeshtime [on\|off]` |
| `espnowmeshtopo` |  | Discover mesh topology (run on the master; role not enforced). (async - read results with espnowtoporesults) |
| `espnowmeshttl` |  | Get/set the multi-hop budget: 'espnowmeshttl [1-10]'.<br/>`Usage: espnowmeshttl [<1..10>] How many hops this node's relay-eligible frames may travel. 1 = single hop (no multi-hop). Applies to broadcast text/time sync and to routed unicast; heartbeats and pairing are always single-hop.` |
| `espnowmessages` |  | Buffered message history as JSON: 'espnowmessages json [sinceSeq] [mac]' — async results of espnowremote/browse/fetch.<br/>`Usage: espnowmessages [json] [<sinceSeq>] [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmode` | A | Get/set ESP-NOW mode: 'espnowmode [direct\|mesh]'.<br/>`Usage: espnowmode [direct\|mesh]` |
| `espnowpair` | A | Pair ESP-NOW device: 'espnowpair <mac> <name> [mesh]'. (synchronous; local registry add, no remote handshake)<br/>`Usage: espnowpair <mac> <name> [mesh]` |
| `espnowpairmode` | A | Open the discovery window: 'espnowpairmode [seconds\|off\|status]'. Open on BOTH same-mesh devices, then 'espnowdiscovered' to see + pair. Nothing pairs by itself.<br/>`Usage: espnowpairmode [<seconds>\|off\|status] Default 120s, max 600. Requires a mesh passphrase set on both devices (same mesh). While open, the device broadcasts a discovery beacon and RECORDS other same-mesh devices it hears (see 'espnowdiscovered'); it does not pair by itself. Pair from the list with 'espnowpairsecure <mac> <name>'.` |
| `espnowpairrequest` | A | Request a pair with a discovered device: 'espnowpairrequest <mac_or_name>'. The other device must Accept.<br/>`Usage: espnowpairrequest <mac_or_name> (name/MAC from 'espnowdiscovered') Sends a request; the target shows an Accept prompt (or run 'espnowaccept' there).` |
| `espnowpairsecure` | A | Pair device with encryption: 'espnowpairsecure <mac> <name> [mesh]'. (local pair is synchronous; secure channel completes async - see espnowsessions)<br/>`Usage: espnowpairsecure <mac_address> <device_name> [mesh] Requires a mesh passphrase first - run 'espnowsetpassphrase <mesh> <passphrase>'. The device is added synchronously; KEY_EX then runs asynchronously (~100ms) so the encrypted channel becomes usable shortly after - inspect with 'espnowsessions' / 'espnowencstatus'.` |
| `espnowpowerinterval` | A | How often this device sends its battery state to the mesh master, in seconds: 'espnowpowerinterval [0\|10..3600]'. 0 = not sent.<br/>`Usage: espnowpowerinterval [0\|10..3600]` |
| `espnowprobe` | A | Reachability probe via KEY_EX. Synchronous, bounded timeout. Reports alive+mesh+firmware in one shot (no plaintext on the wire).<br/>`Usage: espnowprobe <name_or_mac> [<timeoutMs (50-5000, default 500)>] [<mesh>]` |
| `espnowregenidentity` | S | Regenerate Ed25519 identity. Requires '--confirm-wipe-all-bonds'.<br/>`Usage: espnowregenidentity --confirm-wipe-all-bonds` |
| `espnowreject` | A | Reject a pending incoming pair request: 'espnowreject [<mac_or_name>]'.<br/>`Usage: espnowreject [<mac_or_name>]` |
| `espnowrekey` | A | Force immediate SESSION_REKEY for a peer (manual trigger). (async - completes later; check espnowsessions)<br/>`Usage: espnowrekey <name_or_mac> Returns OK when REKEY is sent; new keys derive when the peer's REKEY arrives - verify with 'espnowsessions'.` |
| `espnowrelayblock` | A | DIAGNOSTIC: drop ALL inbound ESP-NOW frames from one MAC (bench stand-in for an out-of-range peer; relay HW testing). RAM-only - reboot clears it.<br/>`Usage: espnowrelayblock <mac\|clear\|status> Drops frames at the RX drain before any parsing - heartbeats too, so the peer goes offline within ~30s, like real RF loss.` |
| `espnowremote` | A | Execute a command on a peer: 'espnowremote <target> <target-user> <target-pass> <cmd>'. user/pass are an account ON THE TARGET (verified there), not this device. (async - result via espnowmessages json)<br/>`Usage: espnowremote <target> <target-user> <target-pass> <command> <target-user>/<target-pass> are credentials ON THE TARGET device, not this one. Async: returns a reqId on delivery; read the output later with 'espnowmessages json 0 <target-mac>' (match the reqId).` |
| `espnowrequestevents` | A | Ask a peer to send US only events in <bitmask>. Updates state ON THE PEER. (async - changes peer state, no reply; verify with espnowsubs on the peer)<br/>`Usage: espnowrequestevents <name_or_mac> <bitmask> Returns OK on delivery; this updates the PEER's subscription (no confirmation returns) - run 'espnowsubs' on that peer to verify.` |
| `espnowrequestmeta` |  | Request metadata from peer: 'espnowrequestmeta <name_or_mac>'. (async - updates cache; view with espnowdevices)<br/>`Usage: espnowrequestmeta <name_or_mac> Returns OK on delivery; the peer's name/room/zone/tags arrive later and update the local peer cache shown by 'espnowdevices' / 'espnowrooms' / 'espnowfind'.` |
| `espnowresetstats` | A | Reset ESP-NOW statistics counters. |
| `espnowroom` |  | Get/set device room: 'espnowroom [name]'.<br/>`Usage: espnowroom [Kitchen\|Bedroom\|...] espnowroom clear` |
| `espnowroomcmd` | A | Run command on all devices accepted here as members of a room; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)<br/>`Usage: espnowroomcmd <room> <target-user> <target-pass> <command> Credentials are checked ON EACH target device, not this one. Goes only to peers whose room was accepted on this device (espnowgroups, espnowgroupaccept); a peer that only announces the room is skipped and counted in the reply. Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'.` |
| `espnowrooms` |  | List announced rooms and their devices (from this node's cached peer metadata); [pending] marks a membership not accepted here. |
| `espnowrouterstats` |  | Show message router statistics and metrics. |
| `espnowsaturation` |  | Show ESP-NOW link saturation: frames/sec, stream-queue depth, drops, ACK RTT (rolling 30s). |
| `espnowsaturationreset` |  | Clear the saturation rolling window (use before a stress test). |
| `espnowsend` |  | Send message to one peer, relayed over multiple hops if it is out of radio range: 'espnowsend [json] <name_or_mac> <message>'. Requires ESP-NOW encryption enabled. (async send; delivery only, no reply)<br/>`Usage: espnowsend [json] <name_or_mac> <message> Requires ESP-NOW encryption (set a mesh passphrase first); plaintext send was removed. Leading 'json' flag returns {schema,ok,msgId} for delivery-status polling ("queued":true while it waits). Returns OK when sent, or queued until the session with the peer opens (sent then, or failed after 5 s; up to 4 wait per peer); one-way message, no result comes back. Out-of-range peers are reached via relays when a route exists ('espnowmeshroutes'); a relayed message is split at ~146 chars per piece instead of ~200.` |
| `espnowsendfile` | A | Send file: 'espnowsendfile <name_or_mac> "<filepath>"'. (synchronous send; fails if the receiver rejects/cancels mid-transfer)<br/>`Usage: espnowsendfile <name_or_mac> "<filepath>" Blocks until the file is sent. 'success' means every chunk was transmitted and the receiver did not cancel; final storage is confirmed by the receiver's CRC check, which is not reported back here.` |
| `espnowsensorbroadcast` |  | Enable/disable all sensor ESP-NOW communication: 'espnowsensorbroadcast <on\|off>'.<br/>`Usage: espnowsensorbroadcast [on\|off]` |
| `espnowsensorbroadcastinterval` | A | Set sensor broadcast interval: <100-10000 ms><br/>`Usage: espnowsensorbroadcastinterval <100..10000>` |
| `espnowsensorreq` | A | Hand-send a SENSOR_REQ to a paired worker, or to a bond partner through the bond master that forwards for it ('<partner> via <bond master>').<br/>`Usage: espnowsensorreq <MAC\|name> [via <MAC\|name>] <sensors> <mode> <intervalMs> <leaseMs> sensors: names, comma separated (tof,imu), 'all', or a mask (0x6). mode: subscribe\|sub\|0, unsubscribe\|unsub\|1, oneshot\|once\|2. A forwarded partner: its name and 'via' the bond master (espnowforwardlinks lists them); its name alone when it is not paired here. The bond master needs 'bondforward on' and forwards only the sensors it names (bondforward sensors). Refused when the session shows the device has no sensors in its build (or, 'via', no bond mode).` |
| `espnowsensorstatus` |  | Show what this device sends to the mesh master (readings, battery state, events) and why not, and what it received, lost and refused: 'espnowsensorstatus [json [<name_or_mac> [via <name_or_mac>]]]'.<br/>`Usage: espnowsensorstatus [json [<name_or_mac> [via <name_or_mac>]]] json <name_or_mac>: everything this device keeps of one sending device. via <name_or_mac>: the bond partner that device forwards for (also found by the name its link shows).` |
| `espnowsensorstream` |  | Send one sensor's readings to the master named on this device: 'espnowsensorstream <sensor> <on\|off>'. (on this device only; the readings show on the master's espnowsensorstatus)<br/>`Usage: espnowsensorstream <thermal\|tof\|imu\|gps\|input\|fmradio\|camera\|microphone\|rtc\|presence\|apds> <on\|off> The readings go to the master named on this device (espnowmasterfingerprint), which must be paired with it. The reply says where they go, or why nothing is sent yet. On a bond master it asks the bond partner.` |
| `espnowsessionopen` | A | Initiate SESSION handshake (requires prior espnowkeyex). (async - session goes ACTIVE later; check espnowsessions)<br/>`Usage: espnowsessionopen <name_or_mac> [<mesh>] Returns OK when SESSION_OPEN is sent; the session becomes ACTIVE when CONFIRM arrives - run 'espnowsessions'.` |
| `espnowsessions` |  | Show in-RAM session state (peer, mesh slot, sessionId, dir, age, counters) and each peer's capabilities from its handshake (caps=bond,remote_fs,...); the first line shows this device's. |
| `espnowsessionsend` | A | DIAGNOSTIC: send an AEAD-encrypted CHAT message over an active session (exercises the session-crypto path). NOT executed on the peer and returns no reply - to RUN a command use 'espnowremote'.<br/>`Usage: espnowsessionsend <name_or_mac> <message> Delivers an encrypted CHAT message (lands in the peer's espnowmessages). It is NOT command execution and no reply comes back. To run a command on the peer: espnowremote <target> <target-user> <target-pass> <command>.` |
| `espnowsetpassphrase` | S | Set encryption passphrase on a mesh: 'espnowsetpassphrase <mesh> <phrase>'.<br/>`Usage: espnowsetpassphrase <mesh> <passphrase> espnowsetpassphrase <mesh> clear` |
| `espnowstationary` |  | Get/set stationary flag: 'espnowstationary [0\|1]'.<br/>`Usage: espnowstationary [on\|off\|0\|1]` |
| `espnowstats` |  | Show ESP-NOW statistics (messages, errors, and received frames dropped by cause: too short for their type, an unknown must-understand flag, an unknown mesh id). |
| `espnowstatus` |  | Show ESP-NOW status and configuration. |
| `espnowsubs` |  | List peers + their event-subscription bitmaps (what they want from us). |
| `espnowtagcmd` | A | Run command on all devices accepted here as holders of a tag; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)<br/>`Usage: espnowtagcmd <tag> <target-user> <target-pass> <command> Credentials are checked ON EACH target device, not this one. One tag per command: a tag list (a comma) is refused. Goes only to peers whose tag was accepted on this device (espnowgroups, espnowgroupaccept); a peer that only announces the tag is skipped and counted in the reply. Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'.` |
| `espnowtags` |  | Get/set device tags: 'espnowtags [tag1,tag2,...]'.<br/>`Usage: espnowtags stationary,thermal espnowtags clear` |
| `espnowtimestatus` |  | Show mesh time: where this device's clock comes from, what it sends or last received: 'espnowtimestatus [json]'.<br/>`Usage: espnowtimestatus [json]` |
| `espnowtimesync` |  | Mesh master: send this device's clock to the paired devices now (it is also sent when a session starts and every 10 minutes).<br/>`Usage: espnowtimesync Only the mesh master sends time, and only when its own clock comes from rtc, ntp, timeset or cm5. A paired device takes it when it names this device as its master (espnowmasterfingerprint) and has no such source itself. See 'espnowtimestatus'.` |
| `espnowtopoautorefresh` | A | Set auto refresh topology: <0\|1><br/>`Usage: espnowtopoautorefresh <0\|1>` |
| `espnowtopodiscoveryinterval` | A | Set topology discovery interval: <0-300000 ms><br/>`Usage: espnowtopodiscoveryinterval <0..300000>` |
| `espnowtoporesults` |  | Get topology results: 'espnowtoporesults [page] [request-id]'. |
| `espnowunpair` | S | Unpair ESP-NOW device and remove what this device stored about it (crypto identity, bond caches, its cached manifest); unpairing the bond partner ends the bond too: 'espnowunpair <name_or_mac>'.<br/>`Usage: espnowunpair <name_or_mac>` |
| `espnowusersync` | S | Enable/disable user credential sync: 'espnowusersync [on\|off]'.<br/>`Usage: espnowusersync [on\|off]` |
| `espnowzone` |  | Get/set device zone: 'espnowzone [name]'.<br/>`Usage: espnowzone [Counter\|Door\|Ceiling\|...] espnowzone clear` |
| `openespnow` | A | Initialize ESP-NOW communication. |
| `openstream` | A | Start streaming all output to ESP-NOW caller (admin, remote only). |
| `testcleanup` | A | Test cleanup of stale topology streams. |
| `testconcurrent` | A | Test concurrent topology streams (simulated). |
| `testfilelock` |  | Test file transfer lock acquire/release. |
| `teststreams` | A | Test topology stream management functions. |

## catchup

> A worker that names a master (espnowmasterfingerprint, and the backup) offers its whitelisted files to them over ESP-NOW: sensor captures, events.log, battery.csv, health files sealed at rest, errors and crash history, and the system log and saved transcripts sealed on send with the capture key. Login logs, the command audit, QuickNotes and keys are never offered. The masters pull on their own when the device is back in range; a file counts as delivered once one of them has it. espnowcatchup lists the offered files and what was taken; espnowcatchupserve turns serving off.

| Command | | Description |
| ------- | :-: | ----------- |
| `espnowcatchup` | A | Files this device offers its named masters, and what they took [json] [from <n>]<br/>`Usage: espnowcatchup [json] [from <n>]` |
| `espnowcatchupserve` | A | Serve file catch-up to the named masters [on\|off]<br/>`Usage: espnowcatchupserve [on\|off]` |

## collect

> A mesh master or backup master built as a collector pulls the files each device that names it offers (file catch-up) onto its SD card under /sd/collect/<mac>/files/, when the device's session comes back, every 15 minutes and on collectnow. It needs a writable card and a set clock. collectstatus shows each device and its files, collectnow starts a pull, collect on|off stops or starts pulling, collectrate and collectquota set the frame rate and the card space per device.

| Command | | Description |
| ------- | :-: | ----------- |
| `collect` | A | Pull files from the devices that name this hub (shows why not with no argument). |
| `collectnow` | A | Pull a device's files now (full: also what the other master has). |
| `collectquota` | S | Card quota per device in MB (64-65535). |
| `collectrate` | A | Frames per second the collector asks for (5-100). |
| `collectstatus` | A | File catch-up: what this hub pulled from each device that names it. |

## c6

> On the ESP32-P4X-EYE the Wi-Fi, Bluetooth and ESP-NOW radio is a separate ESP32-C6 that the P4 drives over SDIO (ESP-Hosted). c6status reports its firmware, the ESP-NOW bridge, heartbeat, memory and recovery counters; c6restart resets it in place; c6hold turns the radio fully off by holding the C6 in reset and brings it back; c6update flashes a new companion image over SDIO with rollback protection and c6confirm accepts the running image; c6autorecover, c6autohold and c6heartbeat tune the background monitor. Radio features never start while the companion is offline, and a companion that stops answering is recovered without rebooting the P4.

| Command | | Description |
| ------- | :-: | ----------- |
| `c6autohold` | A | Hold the companion in reset while no radio feature runs: c6autohold [on\|off] (persists). |
| `c6autorecover` | A | Auto-recover the companion after an outage: c6autorecover [on\|off] (persists). |
| `c6confirm` | S | Confirm the companion's running image so it cannot roll back. |
| `c6console` | A | Keep the C6's own console mirrored into the P4 log: c6console [on\|off] (persists). Off: only during boot and for 10 minutes after a companion fault.<br/>`Usage: c6console [on\|off] on - mirror the companion's console as C6> lines all the time (uses about 6 KB of internal memory) off - mirror it only during boot and after a companion fault, until the companion has been healthy for 10 minutes (the default)` |
| `c6heartbeat` | A | Companion heartbeat interval in seconds, 0 disables the watchdog: c6heartbeat <0..60> (persists). |
| `c6hold` | S | Hold the C6 in reset (radio fully off) or release it: c6hold [on\|off].<br/>`Usage: c6hold [on\|off] on - stop Wi-Fi, Bluetooth and ESP-NOW and hold the companion in reset off - release it and restore what was running` |
| `c6restart` | A | Restart the C6 companion in place (radio features stop and come back). |
| `c6status` |  | ESP32-C6 companion status: firmware, bridge, heartbeat, memory, recovery (add 'json'). |
| `c6update` | A | Flash companion firmware from a file over SDIO: c6update "<path>".<br/>`Usage: c6update "/sd/firmware/network_adapter.bin" The file must be an ESP32-C6 image built by tools/p4/companion (it is checked for the ESP-NOW bridge marker before anything is sent).` |

## mqtt

> The MQTT subsystem connects the device to a broker, primarily to publish its sensor and system telemetry to Home Assistant via HA discovery. It is almost entirely configuration: broker host/port (mqtthost, mqttport), credentials (mqttuser, mqttpassword), TLS mode and CA path, base/discovery topics, publish interval, and a long list of per-source publish toggles (mqttpublishthermal, mqttpublishimu, and so on). These are persisted settings and most config commands are admin-only; after changing them, reconnect with closemqtt/openmqtt to apply to a live session. openmqtt and closemqtt start and stop the client, mqttstatus shows connection state, and mqttautostart controls whether it connects at boot. For inbound data, enable mqttsubscribeexternal with mqttsubscribetopics; values received from those topics are cached and read back with mqttexternalsensors.

| Command | | Description |
| ------- | :-: | ----------- |
| `closemqtt` | A | Stop MQTT client |
| `mqttautostart` | A | MQTT auto-start [0\|1]<br/>`Usage: mqttautostart [0\|1]` |
| `mqttbasetopic` | A | Base topic [topic\|auto]<br/>`Usage: mqttbasetopic [topic\|auto]` |
| `mqttcacertpath` | A | CA cert path [path\|clear]<br/>`Usage: mqttcacertpath [path\|clear]` |
| `mqttdiscoveryprefix` | A | HA discovery prefix [prefix]<br/>`Usage: mqttdiscoveryprefix [prefix]` |
| `mqttexternalsensors` |  | List external sensor data (add 'json' for JSON output) |
| `mqtthost` | A | MQTT broker host [hostname]<br/>`Usage: mqtthost [hostname]` |
| `mqttpassword` | A | MQTT password [pass\|clear]<br/>`Usage: mqttpassword [password\|clear]` |
| `mqttport` | A | MQTT broker port [port]<br/>`Usage: mqttport [port]` |
| `mqttpublishapds` | A | Publish APDS [0\|1]<br/>`Usage: mqttpublishapds [0\|1]` |
| `mqttpublishgps` | A | Publish GPS [0\|1]<br/>`Usage: mqttpublishgps [0\|1]` |
| `mqttpublishimu` | A | Publish IMU [0\|1]<br/>`Usage: mqttpublishimu [0\|1]` |
| `mqttpublishinput` | A | Publish input device data [0\|1]<br/>`Usage: mqttpublishinput [0\|1]` |
| `mqttpublishintervalms` | A | Publish interval [ms]<br/>`Usage: mqttpublishintervalms [1000-300000]` |
| `mqttpublishpresence` | A | Publish presence [0\|1]<br/>`Usage: mqttpublishpresence [0\|1]` |
| `mqttpublishrtc` | A | Publish RTC [0\|1]<br/>`Usage: mqttpublishrtc [0\|1]` |
| `mqttpublishsystem` | A | Publish system [0\|1]<br/>`Usage: mqttpublishsystem [0\|1]` |
| `mqttpublishthermal` | A | Publish thermal [0\|1]<br/>`Usage: mqttpublishthermal [0\|1]` |
| `mqttpublishtof` | A | Publish ToF [0\|1]<br/>`Usage: mqttpublishtof [0\|1]` |
| `mqttpublishwifi` | A | Publish WiFi [0\|1]<br/>`Usage: mqttpublishwifi [0\|1]` |
| `mqttstatus` |  | Show MQTT status (add 'json' for JSON output) |
| `mqttsubscribeexternal` | A | External subscriptions [0\|1]<br/>`Usage: mqttsubscribeexternal [0\|1]` |
| `mqttsubscribetopics` | A | Subscribe topics [topics]<br/>`Usage: mqttsubscribetopics [topic1,topic2,...]` |
| `mqtttlsmode` | A | TLS mode [0\|1\|2]<br/>`Usage: mqtttlsmode [0\|1\|2\|none\|tls\|verify]` |
| `mqttuser` | A | MQTT username [user\|clear]<br/>`Usage: mqttuser [username\|clear]` |
| `openmqtt` | A | Start MQTT client |

## bluetooth

> The Bluetooth radio runs one role at a time. openble brings up the shared client stack (the host, its controller link and the setup worker; like openwifi) and closeble takes the whole radio down again; the stack stays up until closeble, never because the last device closed. On the stack each device opens and closes on its own: openg2/closeg2 for the Even G2 glasses (the even_g2 commands then apply), openring/closering for the R1 ring, openphone/closephone for the phone link; closing one leaves the others and the stack alone, and a device out of range stays open and reconnects. openconsole starts the app console server instead (legacy: the device advertises the App console and the HardwareOne app connects to it; it closes the three devices first and never coexists with the client stack) and closeconsole stops it. blemode stores the boot role only (client, the default, or server). The phone link carries the relay (the LLM fallback and the Terminal, while the app forwards) and the app console as a stream inside it, which the device offers when its phone secret is a device code from phone pair new and bleconsole is on (bleconsole on|off|status); every console session there signs in. Beside the glasses the console can also ride the link's own command service (Secure Channel only, never advertised). blestatus and bleinfo show the role running now, the host state, the app console and the Phone link block (state, console, relay, streams and session end reasons; phone status has the pipe's detail). blesend pushes a one-off message to the connected app (on the phone link too); in server mode bleevent sends an event and blestream toggles periodic pushes as a bitmask of sensors/system/events (blestream on/off/sensors/system/events, plus interval) -- all of which require an active connection. An app-layer Secure Channel (X25519 + passphrase + ChaCha20-Poly1305, independent of BLE bonding) is configured with blesecret and required with blesecure; both require super admin, as does blerequireauth. Boot reconnection to saved-MAC peers is per-peer via bleautoreconnect <name> [on|off] (see blepeers for names).

| Command | | Description |
| ------- | :-: | ----------- |
| `bleadv` | A | Start/stop/toggle BLE advertising [start\|stop\|toggle].<br/>`Usage: bleadv [start\|stop\|toggle]` |
| `bleautoreconnect` | A | Per-peer auto-reconnect (boot + mid-session drop): bleautoreconnect <name> [on\|off]. `blepeers` lists names.<br/>`Usage: bleautoreconnect <peer-name> [on\|off] on: reconnect at boot and reseek after unexpected drops (not after closering / closeg2 / closephone)` |
| `bleautostart` | A | Enable/disable BLE auto-start after boot [on\|off].<br/>`Usage: bleautostart [on\|off]` |
| `bleconsole` | A | App console on the phone link: its console stream (a device code from 'phone pair new'), and beside the glasses the Secure Channel attach [on\|off\|status].<br/>`Usage: bleconsole [on\|off\|status]` |
| `bledisconnect` | A | Disconnect BLE app clients: all, or one by connection id [connId].<br/>`Usage: bledisconnect [connId]` |
| `bleevent` |  | Send event to BLE client: <event>.<br/>`Usage: bleevent <message>` |
| `bleinfo` |  | Show BLE configuration and settings. (add 'json' for JSON output) |
| `blemode` | A | Get/set the saved Bluetooth boot role: client = the shared stack for glasses, ring and phone (default); server = the app console server (legacy).<br/>`Usage: blemode [client\|server] client = the shared client stack: glasses, ring and phone open on it (openg2 / openring / openphone; auto-reconnect at boot) server = the app console server, advertising at boot (openconsole starts it now; it never coexists with the client stack)` |
| `blepeers` |  | List all registered BLE peers and their state. |
| `blerecords` | A | BLE scan/connect records with the Wi-Fi state (no addresses).<br/>`Usage: blerecords [scans\|connects\|clear]` |
| `blerequireauth` | S | Enable/disable BLE authentication requirement [on\|off].<br/>`Usage: blerequireauth [on\|off]` |
| `blesecret` | S | Set/clear the BLE Secure Channel passphrase: blesecret <phrase\|clear>.<br/>`Usage: blesecret <passphrase\|clear>` |
| `blesecure` | S | Require app-layer BLE encryption [on\|off].<br/>`Usage: blesecure [on\|off]` |
| `blesend` |  | Send message to BLE client: <message>.<br/>`Usage: blesend <message>` |
| `blestatus` |  | Show the Bluetooth role, app console and relay pipe. (add 'json' for JSON output) |
| `blestream` |  | Control streaming: <on\|off\|sensors\|system\|events\|interval>.<br/>`Usage: blestream [on\|off\|sensors\|system\|events\|interval] \| interval <sensor_ms> <system_ms>` |
| `bletxpower` |  | Get/set BLE TX power [0-7].<br/>`Usage: bletxpower [0..7]` |
| `closeble` | A | Bluetooth off: close the glasses, ring, phone and the app console, then stop the host. |
| `closeconsole` | A | Stop the app console server (saved boot role back to client; 'openble' brings the client stack back). |
| `openble` | A | Bluetooth on: bring the shared client stack up; devices with auto-reconnect come back (openg2 / openring / openphone open one). |
| `openconsole` | A | Start the app console server for a direct app connection (closes the glasses, ring and phone; saved boot role = server). |

## phone

> The Android phone as a BLE device, reached through the authenticated phone pipe. The pipe is a device on the shared client stack: openphone opens the phone link (bringing the stack up if it is down; it opens by itself at boot and after openble when its auto-reconnect is on) and closephone closes it, with the app console on its link, leaving the glasses, the ring and the stack as they were; the app console can ride its link (bleconsole). phone status [json] shows the link. First-time pairing: sign in as a super admin on serial or the OLED and run phone pair new (or Bluetooth > Phone > New code and pair): the device replaces the phone secret with a 16-symbol code, shows it once (printed on serial, or on its own OLED screen, never in a log) and keeps pairing open for up to 3 minutes, until the first success; type the code in the app. Only such a device code offers the app console on the phone link. A passphrase of your own set with phone secret <passphrase> (super admin, not over Bluetooth) carries the relay only; then phone pair from serial or the OLED opens a 60 s window. openphone, closephone and phone forget act on the paired phone; openphone, phone pair, phone pair new and phone forget refuse while the app console server runs (closeconsole first). bleautoreconnect phone [on|off] reconnects it at boot. phone scan shows or sets the reconnect scan and phone stats the link counters.

| Command | | Description |
| ------- | :-: | ----------- |
| `closephone` | A | Close the phone link (glasses, ring and Bluetooth stay up). |
| `openphone` | A | Open the phone link on the Bluetooth client stack and connect to the paired phone. |
| `phone` |  | Android phone link. |
| `phone forget` | S | Forget the paired phone (super admin; serial or OLED). |
| `phone pair` | A | Open a 60 s pairing window (serial or OLED admin). |
| `phone pair new` | S | Make a new pairing code (the phone secret), show it once on serial or the OLED and pair for 3 min.<br/>`Usage: phone pair new` |
| `phone scan` | A | Show or set the phone reconnect scan.<br/>`Usage: phone scan [default\|legacy\|<interval_ms> <window_ms> dup\|nodup]` |
| `phone secret` | S | Set/clear the phone pairing passphrase.<br/>`Usage: phone secret <passphrase\|clear>` |
| `phone stats` | A | Phone link sessions, counters and setup timings. |
| `phone status` |  | Show the phone link. (add 'json' for JSON output) |

_`phoneCommands` has `#if` alternatives in `BLE_PhonePipe.cpp`; this table is the first branch. Other build configurations register a different version of these rows (for example the disabled branch of the feature)._

## relay

> The phone pipe relays TCP for HardwareOne's network users: the LLM HTTP source and the G2 Terminal. relay status shows how their requests reached the network (Wi-Fi or the phone), with outcomes and the last eight requests.

| Command | | Description |
| ------- | :-: | ----------- |
| `relay` |  | What the phone link carries. |
| `relay status` | A | How relay users' requests reached the network (Wi-Fi or phone). |

_`relayCommands` has `#if` alternatives in `BLE_PhonePipe.cpp`; this table is the first branch. Other build configurations register a different version of these rows (for example the disabled branch of the feature)._

## filesystem

> Manages files and directories on the device internal LittleFS flash. Browse with files ["/path"] (add json for app/BLE, or files stats json for storage usage); create and remove with mkdir, rmdir, filecreate, and filedelete; view and rename with fileview and filerename. Critically, every path argument MUST be wrapped in double quotes, e.g. fileview "/system/notes" -- an unquoted or unmatched-quote path is rejected, and a leading slash is added automatically. For programmatic transfer, fileread and filewrite move data in chunks: fileread returns {success,size,offset,len,eof,enc,data} and you loop offset until eof, while filewrite is strictly sequential -- offset 0 truncates/creates the file, each later offset must equal the current file size, and passing final runs the post-save hooks. Access is permission-gated per path: system trees like /system are read-only (or browse-only) for admins, while user data is fully writable; logtier reports whether logs are writing to LittleFS or have spilled into SD overflow.

| Command | | Description |
| ------- | :-: | ----------- |
| `filecreate` |  | Create file: "<path>"<br/>`Usage: filecreate "<path>"` |
| `filedelete` |  | Delete file: "<path>" [confirm]<br/>`Usage: filedelete "<path>" [confirm]` |
| `fileread` |  | Read file chunk as JSON: "<path>" [offset] [len] [b64\|bin] |
| `filerename` |  | Rename file: "<oldpath>" "<newname>"<br/>`Usage: filerename "<oldpath>" "<newname>"` |
| `files` |  | List files ["path"] \| files json ["path"] \| files stats json ["path"] |
| `fileview` |  | View file (paged): "<path>" [page] [raw]<br/>`Usage: fileview "<path>" [page] [raw] (pages are ~3.5KB, split at line boundaries; JSON is re-indented unless raw)` |
| `filewrite` |  | Write file chunk: "<path>" <offset> <b64chunk> [final] |
| `logtier` |  | Show current log storage tier (LittleFS vs SD overflow). |
| `mkdir` |  | Create directory: "<path>"<br/>`Usage: mkdir "<path>"` |
| `rmdir` |  | Remove directory: "<path>"<br/>`Usage: rmdir "<path>"` |

## sd

> Controls the optional microSD card, which mounts at /sd and serves as overflow/bulk storage (and is only compiled in on boards that wire a supported SPI or SDMMC slot). sdmount attempts to mount the card and sdunmount safely unmounts it; on a board with a card-detect switch a card is also mounted when it is put in; sdinfo shows the card type, size, and used/free space, and sddiag reports backend-specific diagnostics to troubleshoot a card that will not mount. sdformat erases the entire card and reformats it as FAT32 and therefore requires sdformat confirm to proceed. Once mounted, file commands address the card through its /sd/... path prefix.

| Command | | Description |
| ------- | :-: | ----------- |
| `sddiag` |  | SD card hardware diagnostics |
| `sdformat` | S | Format SD card as FAT32 |
| `sdinfo` |  | Show SD card information |
| `sdmount` |  | Mount SD card |
| `sdunmount` | A | Unmount SD card |

## oled

> Drives the selected local display backend: its lifecycle, the live screen contents, and persistent appearance settings. openoled/closeoled power the display task on and off, and oledstatus reports its state. oledmode <mode> switches the live screen among the built-in views (menu, status, sensordata, thermal, network, mesh, gps, espnow, memory, off, and more); oledtext <message> shows custom text and oledanim <name>|fps <n> picks the animation -- both require the display to be running (run openoled first) and neither persists across reboot. Separately, the oled* config commands write settings to flash immediately: oleddefaultmode sets the idle default screen, while oledbrightness <0-255>, oledflip, oledbootduration, oledupdateinterval and oledthermalscale tune appearance and timing, and oledautostart says whether the display comes up at boot. Whether a user must log in at the display before interacting with it is set with displayrequireauth <0|1> (super admin, Settings module).

| Command | | Description |
| ------- | :-: | ----------- |
| `closeoled` | A | Stop OLED display. |
| `oledanim` | A | Select animation: <name> or fps <1-60><br/>`Usage: oledanim <name> oledanim fps <1-60>` |
| `oledbootduration` | A | Boot animation duration (ms): <500-10000><br/>`Usage: oledbootduration <500..10000>` |
| `oledbrightness` | A | Display brightness: <0-255><br/>`Usage: oledbrightness <0..255>` |
| `oledcameraread` |  | Read the local camera preview state (JSON) |
| `oledclear` | A | Clear OLED display. |
| `oleddefaultmode` | A | OLED default mode: <logo\|status\|sensors\|thermal\|network\|mesh\|off><br/>`Usage: oleddefaultmode <logo\|status\|sensors\|thermal\|network\|mesh\|off>` |
| `oledflip` | A | Flip display 180°: [on\|off\|toggle]<br/>`Usage: oledflip [on\|off\|toggle]` |
| `oledlandscape` | A | Landscape UI with right-side controls: [0\|1]<br/>`Usage: oledlandscape [0\|1]` |
| `oledmode` | A | Set display mode: <mode><br/>`Usage: oledmode <menu\|status\|sensordata\|sensorlist\|thermal\|camera\|network\|mesh\|gps\|text\|logo\|anim\|imuactions\|fmradio\|files\|logging\|automations\|espnow\|memory\|off> Example: oledmode memory Example: oledmode off` |
| `oledstatus` |  | Show OLED status. (add 'json' for JSON output) |
| `oledtext` | A | Set custom text: <message><br/>`Usage: oledtext <message>` |
| `oledthermalscale` | A | Thermal image scale: <1.0-10.0><br/>`Usage: oledthermalscale <1.0..10.0>` |
| `oledupdateinterval` | A | Display update interval (ms): <10-1000><br/>`Usage: oledupdateinterval <10..1000>` |
| `openoled` | A | Start OLED display. |

## speaker

> speakertone plays a short test tone; frequency, duration and volume are optional.

| Command | | Description |
| ------- | :-: | ----------- |
| `speakertone` |  | Play a short speaker test tone.<br/>`Usage: speakertone [100..4000 Hz] [50..2000 ms] [0..60 volume]` |

## neopixel

> One set of commands for every RGB LED this build has, the LED named by an optional first word: onboard (the board's own NeoPixel), encoder (every pixel of the encoder board) or encoder:<p> (one pixel, 1-based); encoder board n (2-4) is encoder<n> and encoder<n>:<p>. Left out, it is onboard, or the encoder on a board without an onboard LED. ledcolor [target] <colour> lights it a named colour (red, green, blue, yellow, magenta, cyan, white, orange, purple, pink and ~70 more, or off); ledclear [target] turns it off. ledeffect [target] <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100-60000ms] runs an effect (defaults: red/blue, 3000 ms) and returns at once; the onboard LED goes dark when it ends, the encoder's pixels show their colour again. ledbrightness [onboard|encoder[<n>]] <0-100> is saved per LED and applies at once (admin). ledstatus [json] lists every LED of the build, connected or not, and what it shows. Colours and effects are live and not saved; the onboard startup effect lives in the led module.

| Command | | Description |
| ------- | :-: | ----------- |
| `ledbrightness` | A | Save an LED brightness 0-100: [onboard\|encoder[<n>]] <0..100>.<br/>`Usage: ledbrightness [onboard\|encoder[<n>]] <0..100>` |
| `ledclear` |  | Turn an LED off: [onboard\|encoder[<n>][:<p>]].<br/>`Usage: ledclear [onboard\|encoder[<n>][:<p>]]` |
| `ledcolor` |  | Set an LED colour: [onboard\|encoder[<n>][:<p>]] <colour>.<br/>`Usage: ledcolor [onboard\|encoder[<n>][:<p>]] <red\|green\|blue\|yellow\|magenta\|cyan\|white\|orange\|purple\|pink>` |
| `ledeffect` |  | Run an LED effect: [onboard\|encoder[<n>][:<p>]] <effect>.<br/>`Usage: ledeffect [onboard\|encoder[<n>][:<p>]] <fade\|pulse\|blink\|rainbow\|strobe\|off> [color] [color2] [duration 100..60000]` |
| `ledstatus` |  | Show every LED this build has and what it shows. (add 'json' for JSON output)<br/>`Usage: ledstatus [json]` |

## led

> Configures the effect the board's onboard LED plays at startup. These are persistent settings written to flash: ledstartupenabled <0|1> toggles the boot effect, and ledstartupeffect <none|rainbow|pulse|fade|blink|strobe> with ledstartupcolor, ledstartupcolor2, and ledstartupduration <100-10000ms> define what plays on power-up. (The live colour, effect and brightness commands are in the neopixel module.)

| Command | | Description |
| ------- | :-: | ----------- |
| `ledstartupcolor` | A | Set LED startup primary color (any of ~80 named colors or 'off'; an unknown name is refused).<br/>`Usage: ledstartupcolor <color name\|off> (any of ~80 named colors)` |
| `ledstartupcolor2` | A | Set LED startup secondary color (any of ~80 named colors or 'off'; an unknown name is refused).<br/>`Usage: ledstartupcolor2 <color name\|off> (any of ~80 named colors)` |
| `ledstartupduration` | A | Set LED startup effect duration in ms.<br/>`Usage: ledstartupduration <100..10000>` |
| `ledstartupeffect` | A | Set LED startup effect [none\|rainbow\|pulse\|fade\|blink\|strobe].<br/>`Usage: ledstartupeffect <none\|rainbow\|pulse\|fade\|blink\|strobe>` |
| `ledstartupenabled` | A | Enable/disable LED startup effect [0\|1].<br/>`Usage: ledstartupenabled <0\|1>` |

## matrix

> matrix test draws an orientation pattern. matrix text HI, matrix pixel 3 2 on, matrix clear/fill and matrix on/off control the display. matrix brightness <0-15> and matrix rotation <0-3> persist; matrix blink <0-3> selects off/2Hz/1Hz/0.5Hz. matrixbus <0|1> and matrixaddress <0x70-0x77> require reboot. matrix size 8x8 uses one square; matrix panel <0|1> selects the physical square. matrix size 16x8 restores both. matrixbrightness/matrixrotation/matrixsquare/matrixpanel save without applying (used by the settings editors). Use an unused non-0x70 address when sharing a bus with a PCA9685. Static text clips at the display edge.

| Command | | Description |
| ------- | :-: | ----------- |
| `matrix` |  | HT16K33 LED matrix: status, clear, fill, on/off, pixel, text, brightness, rotation, blink, test |
| `matrixaddress` | A | Select matrix address (reboot required)<br/>`Usage: matrixaddress <0x70..0x77>` |
| `matrixbrightness` | A | Save matrix brightness (applied on next matrix command)<br/>`Usage: matrixbrightness <0-15>` |
| `matrixbus` | A | Select matrix I2C bus (reboot required)<br/>`Usage: matrixbus <0\|1>` |
| `matrixpanel` | A | Save which 8x8 square is used (applied on next matrix command)<br/>`Usage: matrixpanel <0\|1>` |
| `matrixrotation` | A | Save matrix rotation (applied on next matrix command)<br/>`Usage: matrixrotation <0-3>` |
| `matrixsquare` | A | Save single 8x8 square mode (applied on next matrix command)<br/>`Usage: matrixsquare <0\|1>` |

## encoder

> An Adafruit seesaw encoder board on I2C, used for its RGB pixels: the 5752 Quad Rotary Encoder (4 pixels, address 0x49, jumpers to 0x50) or the 4991 Rotary Encoder with NeoPixel (1 pixel, address 0x36, jumpers to 0x3D), told apart by the product id the board reports. The knobs and switches are not used. openencoder and closeencoder open and close it (a closed board goes dark); encoderautostart [on|off] opens it after boot. Its pixels take the LED commands with the encoder target: ledcolor encoder <colour>, ledcolor encoder:2 red, ledeffect encoder pulse red, ledbrightness encoder <0-100>, ledclear encoder; board n (2-4) is ledcolor encoder<n> red. encoderstatus [json] shows the connection, product, pixel count, address, bus and brightness. encoderaddress applies at the next open, encoderbus <0|1> after a reboot. A board that stops answering closes itself and restarts on the sensor retry schedule; an LED command to it reconnects a board that is meant to be open.

| Command | | Description |
| ------- | :-: | ----------- |
| `closeencoder` | A | Close an encoder board (its pixels go dark); n is instance 2..4.<br/>`Usage: closeencoder [n]` |
| `encoderaddress` | A | Save an encoder board's I2C address (next open); 0 unsets instance 2..4.<br/>`Usage: encoderaddress [n] <0x49..0x50\|0x36..0x3D>` |
| `encoderautostart` | A | Open an encoder board after boot [n] [on\|off].<br/>`Usage: encoderautostart [n] [on\|off]` |
| `encoderbus` | A | Route an encoder board to bus <0\|1> (reboot required).<br/>`Usage: encoderbus [n] <0\|1>` |
| `encoderstatus` |  | Show an encoder board: connection, product, pixels, address, brightness. (add 'json' for JSON output)<br/>`Usage: encoderstatus [n] [json]` |
| `openencoder` | A | Open an encoder board (its pixels light with 'ledcolor encoder'); n is instance 2..4.<br/>`Usage: openencoder [n]` |

## servo

> The PCA9685 is a 16-channel I2C PWM driver used to control hobby servos (and generic PWM outputs) without tying up the ESP32 own timers. servo <channel> <angle> moves the servo on a channel to an angle, while pwm <channel> <value> [freq] writes a raw PWM duty (and optional frequency) for non-servo loads like LEDs or motor drivers. Because different servos expect different pulse ranges, servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name> stores a per-channel calibration that maps angles to the correct pulse widths (servolist shows the saved profiles), and servocalibrate <channel> opens an interactive mode to find those pulse limits by hand.

| Command | | Description |
| ------- | :-: | ----------- |
| `pwm` |  | Set PWM output: pwm <channel> <value> [freq].<br/>`Usage: pwm <channel> <value> [freq]` |
| `servo` |  | Control servo motor: servo <channel> <angle>.<br/>`Usage: servo <channel> <angle>` |
| `servobus` | A | Route PCA9685 servo to bus: <0\|1> (reboot required)<br/>`Usage: servoBus <0\|1>` |
| `servocalibrate` | A | Enter calibration mode: servocalibrate <channel>.<br/>`Usage: servocalibrate <channel>` |
| `servolist` |  | List configured servo profiles. (add 'json' for JSON output) |
| `servoprofile` | A | Configure servo profile: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>.<br/>`Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>` |

## thermal

> The MLX90640 is a 32x24 (768-pixel) infrared thermal camera. openthermal starts it, thermalread reports the current frame min/max/avg temperature in Celsius, and closethermal stops it; thermalautostart [on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus (Wire1). The sensor runs in chess-pattern mode at 16-bit ADC resolution, with thermaltargetfps <1..8> selecting the device refresh rate. Display tuning is extensive: thermalpalettedefault picks the color map (grayscale, iron, rainbow, hot, or coolwarm), thermalrotation <0..3> rotates the image 0/90/180/270 degrees, and thermalupscalefactor plus the thermalinterpolation* commands smooth and enlarge the 32x24 grid for the web/OLED view. Frame readings are stabilized by temporal/EWMA smoothing, per-pixel outlier rejection, and an optional rolling min/max auto-scale that keeps the color scale from flickering; thermaldiag prints a hardware self-check.

| Command | | Description |
| ------- | :-: | ----------- |
| `closethermal` | A | Stop MLX90640 thermal sensor. |
| `openthermal` | A | Start MLX90640 thermal sensor. |
| `thermalautostart` | A | Enable/disable thermal auto-start after boot [on\|off]<br/>`Usage: thermalautostart [on\|off]` |
| `thermalbus` | A | Route MLX90640 thermal to bus: <0\|1> (reboot required)<br/>`Usage: thermalBus <0\|1>` |
| `thermaldevicepollms` | A | Thermal device poll: <100..2000><br/>`Usage: thermalDevicePollMs <100..2000>` |
| `thermaldiag` |  | Run thermal sensor diagnostics. |
| `thermalewmafactor` | A | Thermal EWMA factor: <0.0..1.0><br/>`Usage: thermalewmafactor <0.0..1.0>` |
| `thermali2cclockhz` | A | Thermal I2C clock: <100000..1000000><br/>`Usage: thermalI2cClockHz <100000..1000000>` |
| `thermalinterpolationbuffersize` | A | Thermal interp buffer: <1..10><br/>`Usage: thermalinterpolationbuffersize <1..10>` |
| `thermalinterpolationenabled` | A | Thermal interpolation: <0\|1><br/>`Usage: thermalinterpolationenabled <0\|1>` |
| `thermalinterpolationsteps` | A | Thermal interp steps: <1..8><br/>`Usage: thermalinterpolationsteps <1..8>` |
| `thermalpalettedefault` | A | Thermal palette: <grayscale\|iron\|rainbow\|hot\|coolwarm><br/>`Usage: thermalpalettedefault <grayscale\|iron\|rainbow\|hot\|coolwarm>` |
| `thermalpollingms` | A | Thermal UI polling: <50..5000><br/>`Usage: thermalpollingms <50..5000>` |
| `thermalread` |  | Read thermal frame; min/max/avg broadcast to output. (add 'json' for JSON output) |
| `thermalrollingminmaxalpha` | A | Thermal rolling alpha: <0.0..1.0><br/>`Usage: thermalrollingminmaxalpha <0.0..1.0>` |
| `thermalrollingminmaxenabled` | A | Thermal rolling min/max: <0\|1><br/>`Usage: thermalrollingminmaxenabled <0\|1>` |
| `thermalrollingminmaxguardc` | A | Thermal rolling guard: <0.0..10.0><br/>`Usage: thermalrollingminmaxguardc <0.0..10.0>` |
| `thermalrotation` | A | Thermal rotation: <0\|1\|2\|3><br/>`Usage: thermalrotation <0\|1\|2\|3> (0=0°, 1=90°, 2=180°, 3=270°)` |
| `thermaltargetfps` | A | Thermal target FPS: <1..8><br/>`Usage: thermalTargetFps <1..8>` |
| `thermaltemporalalpha` | A | Thermal temporal alpha: <0.0..1.0><br/>`Usage: thermaltemporalalpha <0.0..1.0>` |
| `thermaltransitionms` | A | Thermal transition time: <0..5000><br/>`Usage: thermaltransitionms <0..5000>` |
| `thermalupscalefactor` | A | Thermal upscale factor: <1..4><br/>`Usage: thermalupscalefactor <1..4>` |
| `thermalwebmaxfps` | A | Thermal web max FPS: <1..30><br/>`Usage: thermalWebMaxFps <1..30>` |

## tof

> The VL53L4CX is a laser time-of-flight ranging sensor that measures distance to nearby objects. opentof starts it, tofread reports the closest valid cached distance in centimeters (or full cached object data as JSON), and closetof stops it; tofautostart [on|off] persists launching it at boot, and tofbus selects its I2C bus after reboot. It is configured for LONG distance mode with a 200 ms timing budget, and is multi-target: each measurement can return up to four objects, which are signal-rate-gated and exponentially smoothed before the nearest valid one is reported. Most tunables are client-side visualization knobs rather than sensor settings: tofpollingms, toftransitionms, and tofmaxdistancemm shape the UI, tofstabilitythreshold sets how steady a reading must be, and tofdevicepollms controls how often the firmware reads the hardware.

| Command | | Description |
| ------- | :-: | ----------- |
| `closetof` | A | Stop VL53L4CX ToF sensor. |
| `opentof` | A | Start VL53L4CX ToF sensor. |
| `tofautostart` | A | Enable/disable ToF auto-start after boot [on\|off]<br/>`Usage: tofautostart [on\|off]` |
| `tofbus` | A | Route VL53L4CX ToF to bus: <0\|1> (reboot required)<br/>`Usage: tofBus <0\|1>` |
| `tofdevicepollms` | A | ToF device poll: <100..2000><br/>`Usage: tofDevicePollMs <100..2000>` |
| `tofmaxdistancemm` | A | ToF max distance: <100..10000><br/>`Usage: tofmaxdistancemm <100..10000>` |
| `tofpollingms` | A | ToF UI polling: <50..5000><br/>`Usage: tofpollingms <50..5000>` |
| `tofread` |  | Read ToF distance sensor. (add 'json' for JSON output) |
| `tofstabilitythreshold` | A | ToF stability threshold: <0..50><br/>`Usage: tofstabilitythreshold <0..50>` |
| `toftransitionms` | A | ToF transition time: <0..5000><br/>`Usage: toftransitionms <0..5000>` |

## imu

> The BNO055 is a 9-DOF inertial measurement unit providing fused absolute orientation. openimu starts it, imuread reports yaw/pitch/roll (degrees) plus acceleration, gyroscope, and chip temperature, and closeimu stops it; imuautostart [on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus (Wire1) using the board external crystal. Beyond raw orientation, imuactions runs gesture/event detection derived from the motion data: shake, tilt (with direction), tap/knock, rotation (with axis), freefall, a step counter with cadence, and screen-style orientation. Because the chip can be mounted in any pose, imuorientationmode <0..8> applies a fixed remap (flip pitch/roll/yaw, 90-degree rotations, upside-down fixes), imuorientationcorrection <0|1> toggles that correction, and imupitchoffset/imurolloffset/imuyawoffset trim each axis in degrees.

| Command | | Description |
| ------- | :-: | ----------- |
| `closeimu` | A | Stop BNO055 IMU sensor. |
| `imuactions` |  | Show IMU action detection state. |
| `imuautostart` | A | Enable/disable IMU auto-start after boot [on\|off]<br/>`Usage: imuautostart [on\|off]` |
| `imubus` | A | Route BNO055 IMU to bus: <0\|1> (reboot required)<br/>`Usage: imuBus <0\|1>` |
| `imudevicepollms` | A | IMU device poll interval: <50..1000><br/>`Usage: imuDevicePollMs <50..1000>` |
| `imuewmafactor` | A | IMU EWMA smoothing: <0.0..1.0><br/>`Usage: imuewmafactor <0.0..1.0>` |
| `imuorientationcorrection` | A | IMU orientation correction: <0\|1><br/>`Usage: imuorientationcorrection <0\|1>` |
| `imuorientationmode` | A | IMU orientation mode: <0..8><br/>`Usage: imuorientationmode <0..8>` |
| `imupitchoffset` | A | IMU pitch offset in degrees: <-180..180><br/>`Usage: imupitchoffset <-180..180>` |
| `imupollingms` | A | IMU UI polling interval: <50..2000><br/>`Usage: imupollingms <50..2000>` |
| `imuread` |  | Read IMU sensor data. (add 'json' for JSON output) |
| `imurolloffset` | A | IMU roll offset in degrees: <-180..180><br/>`Usage: imurolloffset <-180..180>` |
| `imutransitionms` | A | IMU transition time: <0..1000><br/>`Usage: imutransitionms <0..1000>` |
| `imuwebmaxfps` | A | IMU web max FPS: <1..30><br/>`Usage: imuwebmaxfps <1..30>` |
| `imuyawoffset` | A | IMU yaw offset in degrees: <-180..180><br/>`Usage: imuyawoffset <-180..180>` |
| `openimu` | A | Start BNO055 IMU sensor. |

## input

> Device-agnostic abstraction for the OLED input controller, which is either the Seesaw gamepad, ANO encoder or GPIO rotary encoder -- chosen at compile time via INPUT_DEVICE_TYPE and mutually exclusive, so exactly one driver is present per firmware. These commands operate on whichever driver was built in: openinput starts it, closeinput stops it, inputautostart [on|off] persists boot auto-start, and inputdevicepollms <10-1000> sets the polling interval in milliseconds (default 90 for I2C devices; GPIO input uses a fixed 5 ms button sample). Driver-specific debugging and tuning live in the gamepad and anoencoder modules; this module holds only the shared settings (poll interval and auto-start).

| Command | | Description |
| ------- | :-: | ----------- |
| `closeinput` | A | Stop the input device. |
| `gpioencoderread` |  | Read GPIO wheel position, switch and pending input as JSON. |
| `inputautostart` | A | Enable/disable input device auto-start [on\|off]<br/>`Usage: inputautostart [on\|off]` |
| `inputdevicepollms` | A | Set input device poll interval ms [10-1000]<br/>`Usage: inputdevicepollms <10-1000>` |
| `openinput` | A | Start the configured input device. |
| `touchread` |  | Read the single-touch snapshot without consuming events. |

## gamepad

> Adafruit Seesaw I2C gamepad (analog joystick plus buttons), exposed here as a low-level debug interface for the raw device. The driver-agnostic open/close/autostart/poll commands live under the input module; the only gamepad-specific command is gamepadread, which polls the Seesaw once and dumps raw state -- joystick X/Y and the button bitmask -- attempting an on-demand connect with backoff if the device is not yet initialized. A background task polls the gamepad at roughly 50 ms and caches the latest reading for the OLED UI and sensor JSON. This module is mutually exclusive at build time with anoencoder; only one input device is compiled in per firmware (see input).

| Command | | Description |
| ------- | :-: | ----------- |
| `gamepadread` |  | Read Seesaw gamepad state (x/y/buttons). (add 'json' for JSON output) |

## anoencoder

> Adafruit ANO directional navigation rotary encoder (5740) on seesaw I2C: a click wheel with a center IN press and UP/DOWN/LEFT/RIGHT buttons, used as the OLED navigation input. The wheel scrolls up and down; IN is A, LEFT is B, DOWN is X, UP is Y, a RIGHT tap is SELECT (sent when RIGHT is released) and RIGHT held with IN is START. The open/close/autostart/poll lifecycle lives under the input module and the bus is set with inputbus. anoencoderstatus [json] shows the bus, address, settings and whether it is open; anoencoderread [json] gives the wheel position and buttons. anoencoderi2caddr <0x49..0x50> sets the address (A0-A2 jumpers; refused when another device of this build uses it on the input bus; applies at the next openinput), anoencoderinvert [on|off] reverses rotation, and anoencoderswapud / anoencoderswaplr [on|off|toggle] swap the UP/DOWN and LEFT/RIGHT button pairs. The polling task reads the buttons, the seesaw's change flags (so taps shorter than one poll are kept) and the wheel's detents in one checked transaction. Mutually exclusive at build time with the Seesaw gamepad.

| Command | | Description |
| ------- | :-: | ----------- |
| `anoencoderi2caddr` | A | Set the ANO I2C address [0x49-0x50] (next openinput)<br/>`Usage: anoencoderi2caddr <0x49..0x50>` |
| `anoencoderinvert` | A | Invert rotation direction [on\|off]<br/>`Usage: anoencoderinvert [on\|off]` |
| `anoencoderread` |  | Read the ANO encoder: wheel position and buttons. (add 'json' for JSON output) |
| `anoencoderstatus` |  | ANO encoder status: bus, address, settings. (add 'json' for JSON output)<br/>`Usage: anoencoderstatus [json]` |
| `anoencoderswaplr` | A | Swap LEFT/RIGHT buttons [on\|off\|toggle]<br/>`Usage: anoencoderswaplr [on\|off\|toggle]` |
| `anoencoderswapud` | A | Swap UP/DOWN buttons [on\|off\|toggle]<br/>`Usage: anoencoderswapud [on\|off\|toggle]` |

## apds

> The APDS9960 is a combined RGB color, proximity, and gesture sensor. openapds starts it (color sensing enabled by default), apdsread shows which modes are active plus the latest RGBC and proximity values, and closeapds stops it; apdsautostart [on|off] persists launching it at boot, and apdsbus selects its I2C bus after reboot. Its three functions are toggled at runtime with apdsmode <color|proximity|gesture> [on|off] -- note that enabling gesture also turns proximity hardware on. Dedicated reads apdscolor, apdsproximity, and apdsgesture show the latest cached sample; they report unavailable when that mode has no reading yet. Gesture reports the last swipe (UP/DOWN/LEFT/RIGHT) and its age, or no gesture since start, without consuming events. apdsdevicepollms sets how often the background task samples color and proximity; gesture mode reads the gesture FIFO at least every 100 ms.

| Command | | Description |
| ------- | :-: | ----------- |
| `apdsautostart` | A | Enable/disable APDS auto-start after boot [on\|off]<br/>`Usage: apdsautostart [on\|off]` |
| `apdsbus` | A | Route APDS9960 gesture to bus: <0\|1> (reboot required)<br/>`Usage: apdsBus <0\|1>` |
| `apdscolor` |  | Show latest cached APDS9960 color values. |
| `apdsgesture` |  | Show latest sampled APDS9960 gesture (does not consume events). |
| `apdsmode` | A | Control APDS modes: apdsmode <color\|proximity\|gesture> [on\|off].<br/>`Usage: apdsmode <color\|proximity\|gesture> [<on\|off>]` |
| `apdsproximity` |  | Show latest cached APDS9960 proximity value. |
| `apdsread` |  | Read APDS9960 sensor status and data. (add 'json' for JSON output) |
| `closeapds` | A | Stop APDS9960 sensor. |
| `openapds` | A | Start APDS9960 sensor. |

## gps

> PA1010D I2C GPS receiver. Lifecycle: opengps starts the parser task, gpsread prints the current fix, and closegps stops it; gpsautostart [on|off] persists boot auto-start, and the module appears in help only when the chip is detected. gpsread reports fix yes/no, fix quality, satellite count, and (only when a fix is held) latitude/longitude in degrees, altitude in meters, speed in knots, heading angle, plus GPS UTC time and date; with no fix it shows just quality and satellites. The gpslog [interval_ms] command requests GPS startup and starts GPS-only track logging (100..3600000 ms, default 1000). It refuses while a log is running or health logging is enabled. GPS initializes in the background; a queued request does not establish a satellite fix. GPS/logger boot auto-start is enabled after the logger starts successfully.

| Command | | Description |
| ------- | :-: | ----------- |
| `closegps` | A | Stop PA1010D GPS module. |
| `gpsautostart` | A | Enable/disable GPS auto-start after boot [on\|off]<br/>`Usage: gpsautostart [on\|off]` |
| `gpsbus` | A | Route PA1010D GPS to bus: <0\|1> (reboot required)<br/>`Usage: gpsBus <0\|1>` |
| `gpslog` | A | Set up and start GPS track logging now (persists across boots). Usage: gpslog [interval_ms]<br/>`Usage: gpslog [interval_ms] Requests GPS startup, selects GPS-only Track format, and starts the logger. GPS initializes in the background. Refuses while another log is running or health logging is enabled. Boot auto-start is enabled after the logger starts successfully. interval_ms: log interval in ms (default 1000, range 100..3600000) Example: gpslog (1-second logging) gpslog 500 (500ms logging)` |
| `gpsread` |  | Read GPS location and time data. (add 'json' for JSON output) |
| `opengps` | A | Start PA1010D GPS module. |

## fmradio

> RDA5807M I2C FM radio receiver. Lifecycle: openfmradio starts it, fmradioread reports status, and closefmradio stops it; fmradioautostart [on|off] persists boot auto-start and the module shows in help only when detected. fmradiotune <freq> accepts either MHz (e.g. 103.9) or 10 kHz integer units (e.g. 10390) -- values under 200 are read as MHz, otherwise as raw units -- and rejects anything outside 76.0-108.0 MHz; tuning clears any decoded RDS station name and text. fmradioseek [up|down] STARTS a hunt for the next station (no band wrap) and returns immediately -- the radio task finalizes the result within a few seconds; watch 'fmradioread' (JSON field "seeking") or the OLED FM screen. fmradiovolume <0-15> sets output level, and fmradiomute on|off mutes and unmutes audio. The OLED FM screen drives all of this from the gamepad: L/R tune, Up/Down seek, A mute, Y volume, X power.

| Command | | Description |
| ------- | :-: | ----------- |
| `closefmradio` | A | Stop FM Radio sensor. |
| `fmradioautostart` | A | Enable/disable FM Radio auto-start after boot [on\|off]<br/>`Usage: fmradioautostart [on\|off]` |
| `fmradiobus` | A | Route RDA5807 FM radio to bus: <0\|1> (reboot required)<br/>`Usage: fmRadioBus <0\|1>` |
| `fmradiomute` | A | Mute or unmute audio: [on\|off] (bare = mute)<br/>`Usage: fmradiomute [on\|off]` |
| `fmradioread` |  | Read FM Radio status. (add 'json' for JSON output) |
| `fmradioseek` | A | Start seeking the next station [up\|down] (async; 'fmradioread' shows the result)<br/>`Usage: fmradioseek [up\|down]` |
| `fmradiotune` | A | Tune to frequency: <freq><br/>`Usage: fmradiotune <frequency> (e.g., 103.9 or 10390)` |
| `fmradiovolume` | A | Set volume: <0-15><br/>`Usage: fmradiovolume <0-15>` |
| `openfmradio` | A | Start FM Radio sensor. |

## rtc

> DS3231 precision I2C real-time clock with battery backup and an on-chip temperature sensor. Lifecycle: openrtc starts the RTC task, rtcread [status|temp] reads the clock (or die temperature), and closertc stops it; rtcautostart [on|off] persists boot auto-start and the module appears in help only when detected. rtcset accepts either "YYYY-MM-DD HH:MM:SS" (UTC) or a bare Unix timestamp, years 2020 to 2099, and writes it to the chip, computing day-of-week automatically. rtcsync [to|from] moves time between the RTC and the system clock: to (the default) copies RTC -> system, from copies system -> RTC (use this after an NTP sync to persist accurate time into the battery-backed chip). Setting the time via rtcset or rtcsync from also marks the RTC as calibrated so later boots trust it as a time source.

| Command | | Description |
| ------- | :-: | ----------- |
| `closertc` | A | Stop DS3231 RTC sensor. |
| `openrtc` | A | Start DS3231 RTC sensor. |
| `rtcautostart` | A | Enable/disable RTC auto-start after boot [on\|off]<br/>`Usage: rtcautostart [on\|off]` |
| `rtcbus` | A | Route DS3231 RTC to bus: <0\|1> (reboot required)<br/>`Usage: rtcBus <0\|1>` |
| `rtcread` |  | Read RTC status [status\|temp]<br/>`Usage: rtcread [status\|temp] [json]` |
| `rtcset` | A | Set RTC time: <datetime\|timestamp><br/>`Usage: rtcset YYYY-MM-DD HH:MM:SS (UTC) or rtcset <unix_timestamp>` |
| `rtcsync` | A | Sync time: [to\|from]<br/>`Usage: rtcsync [to\|from] (to=RTC->system, from=system->RTC)` |

## presence

> The STHS34PF80 is an infrared presence and motion sensor that detects warm bodies without contact. openpresence starts it, presenceread reports ambient temperature plus presence, motion, and temperature-shock values (each with a DETECTED flag), and closepresence stops it; presenceautostart [on|off] persists launching it at boot. The sensor is initialized at an 8 Hz output data rate with block-data-update enabled, and its on-chip presence/motion/ambient-shock detection engines provide the detection flags directly; presencestatus prints connection and data-validity diagnostics, and presencedevicepollms controls the hardware read interval.

| Command | | Description |
| ------- | :-: | ----------- |
| `closepresence` | A | Stop STHS34PF80 sensor. |
| `openpresence` | A | Start STHS34PF80 IR presence/motion sensor. |
| `presenceautostart` | A | Enable/disable presence auto-start after boot [on\|off]<br/>`Usage: presenceautostart [on\|off]` |
| `presencebus` | A | Route STHS34PF80 presence to bus: <0\|1> (reboot required)<br/>`Usage: presenceBus <0\|1>` |
| `presenceread` |  | Read STHS34PF80 presence/motion/temperature data. (add 'json' for JSON output) |
| `presencestatus` |  | Show STHS34PF80 sensor status. (add 'json' for JSON output) |

## camera

> Driver and CLI for the attached DVP camera sensor (OV2640/OV3660 class). The sensor must be powered up first with opencamera before any capture or tuning command works (closecamera stops it); cameraread and cameradump report status and all current sensor register values. Three distinct capture paths exist: cameracapture grabs one JPEG frame into RAM and reports its size only, camerasave captures and writes a frame to storage (LittleFS, SD, or both, per camerastoragelocation, into cameracapturefolder), and cameratiny produces a 160x120 frame small enough for a single ESP-NOW packet; camerarecord start|stop records MJPEG-AVI video and requires an SD card. Resolution and image controls (camerares/cameraframesize, cameraquality, camerafps) and a large set of sensor-tuning commands (brightness/contrast/saturation, white balance, exposure/AEC, gain/AGC, special effects, mirror/flip/rotate, plus raw camerareg register writes) adjust the live image. cameraautostart powers the sensor up at boot, and cameramaxstoredimages caps how many saved photos are kept before the oldest are rotated out. There is no timed auto-capture: every capture is requested by a command or a page.

| Command | | Description |
| ------- | :-: | ----------- |
| `cameraaec` | A | Auto exposure: <on\|off><br/>`Usage: cameraaec <on\|off\|1\|0\|true\|auto>` |
| `cameraaec2` | A | Night mode (slower fps, brighter in low light): <on\|off><br/>`Usage: cameraaec2 <on\|off>` |
| `cameraaecvalue` | A | Exposure value: <0-1200><br/>`Usage: cameraaecvalue <0..1200>` |
| `cameraagc` | A | Auto gain: <on\|off><br/>`Usage: cameraagc <on\|off\|1\|0\|true\|auto>` |
| `cameraagcgain` | A | Gain value: <0-30><br/>`Usage: cameraagcgain <0..30>` |
| `cameraautostart` | A | Auto-start: <on\|off><br/>`Usage: cameraautostart <on\|off\|1\|0\|true\|false>` |
| `cameraawbgain` | A | AWB gain: <on\|off><br/>`Usage: cameraawbgain <on\|off>` |
| `camerabpc` | A | Black pixel correction: <on\|off><br/>`Usage: camerabpc <on\|off>` |
| `camerabrightness` | A | Set brightness: <-2..2><br/>`Usage: camerabrightness <-2..2>` |
| `cameracapture` |  | Capture a single frame |
| `cameracapturefolder` | A | Photo folder: <path><br/>`Usage: cameracapturefolder <path>` |
| `cameracolorbar` | A | Color bar test pattern: <on\|off><br/>`Usage: cameracolorbar <on\|off>` |
| `cameracontrast` | A | Set contrast: <-2..2><br/>`Usage: cameracontrast <-2..2>` |
| `cameradcw` | A | Downsize crop window: <on\|off><br/>`Usage: cameradcw <on\|off>` |
| `cameradenoise` | A | Set denoise level: <0-8><br/>`Usage: cameradenoise <0..8>` |
| `cameradump` |  | Print all current sensor values |
| `cameraeffect` | A | Special effect: <0-6><br/>`Usage: cameraeffect <0..6> (0=None,1=Negative,2=Grayscale,3=Red,4=Green,5=Blue,6=Sepia)` |
| `cameraexposure` | A | Set AE level: <-2..2><br/>`Usage: cameraexposure <-2..2> (negative=darker)` |
| `camerafps` | A | Camera FPS: <1-20><br/>`Usage: camerafps <1..20>` |
| `cameraframesize` | A | Set resolution by index: <0-12><br/>`Usage: cameraframesize <0..12> (0-5: QVGA..UXGA, 6-10: small, 11: HD, 12: CIF; availability depends on backend)` |
| `camerafx` | A | Set bri/con/sat together: <bri> <con> <sat> (-2..+2 each)<br/>`Usage: camerafx <bri> <con> <sat> (-2..+2 each)` |
| `cameragainceiling` | A | Gainceiling: <0-6> (2X..128X)<br/>`Usage: cameragainceiling <0..6> (2X..128X)` |
| `cameragamma` | A | Raw gamma: <on\|off><br/>`Usage: cameragamma <on\|off>` |
| `camerahmirror` | A | Horizontal mirror: <on\|off><br/>`Usage: camerahmirror <on\|off\|1\|0\|true>` |
| `cameralenc` | A | Lens shading correction: <on\|off><br/>`Usage: cameralenc <on\|off>` |
| `cameramaxstoredimages` | S | Max stored: <0-1000><br/>`Usage: cameramaxstoredimages <0..1000> (0=unlimited)` |
| `cameraquality` | A | Set JPEG quality: <0-63><br/>`Usage: cameraquality <0..63> (lower = better quality, larger file)` |
| `cameraread` |  | Read camera status |
| `camerarecord` |  | Start/stop MJPEG-AVI recording (SD only): <start\|stop><br/>`Usage: camerarecord <start\|stop\|1\|0>` |
| `camerareg` | A | Direct register write: <addr_hex> <mask_hex> <value_hex><br/>`Usage: camerareg <addr_hex> <mask_hex> <value_hex> (example: camerareg 0x3824 0x1f 0x04)` |
| `camerares` | A | Set camera resolution: <res><br/>`Usage: camerares <96x96\|qqvga\|qcif\|hqvga\|240x240\|qvga\|cif\|vga\|svga\|xga\|sxga\|uxga\|hd>` |
| `camerarotate` | A | Rotate 180°: <on\|off><br/>`Usage: camerarotate <on\|off\|1\|0\|true\|180>` |
| `camerasaturation` | A | Set saturation: <-2..2><br/>`Usage: camerasaturation <-2..2>` |
| `camerasave` |  | Save current frame to storage |
| `camerasharpness` | A | Set sharpness: <-2..2><br/>`Usage: camerasharpness <-2..2> (OV3660 only)` |
| `camerastoragelocation` | A | Storage location: <0-2><br/>`Usage: camerastoragelocation <0..2> (0=LittleFS,1=SD,2=Both)` |
| `cameratiny` |  | Capture tiny frame for ESP-NOW |
| `cameravflip` | A | Vertical flip: <on\|off><br/>`Usage: cameravflip <on\|off\|1\|0\|true>` |
| `cameravideodelete` | A | Delete recording: "<filename>"<br/>`Usage: cameravideodelete "<filename>"` |
| `cameravideolist` |  | List AVI recordings on SD (add 'json' for JSON output) |
| `camerawb` | A | White balance mode: <0-4><br/>`Usage: camerawb <0..4> (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)` |
| `camerawhitebal` | A | AWB master: <on\|off><br/>`Usage: camerawhitebal <on\|off>` |
| `camerawpc` | A | White pixel correction: <on\|off><br/>`Usage: camerawpc <on\|off>` |
| `closecamera` | A | Stop camera sensor. [async: queue it and return at once] |
| `opencamera` | A | Start camera sensor. [async: queue it and return at once] |

## microphone

> Driver and CLI for the microphone — the on-board PDM mic and/or the G2 glasses mic, selected with micsource. The mic must be started with openmic before reads or recording (closemic stops it); commands that need the running mic return a use-openmic-first error otherwise. miclevel returns the current audio level (percent; add json for structured output) and micviz shows a live level meter until a key is pressed. micrecord start [vad [<ms>] [trim]]|stop records audio to a WAV file; owner-correlated capture uses micrecord startid|statusid|stopid with a strict 16-hex ID and optional discard. miclist lists saved recordings, while micdelete and micdeleteid remove manual or exact owner-scoped results. Audio format is configured with micsamplerate (8000-48000), micgain (0-100), and micbitdepth (16 or 32), each usable as a getter with no argument; micautostart on|off persists whether the mic powers up automatically at boot.

| Command | | Description |
| ------- | :-: | ----------- |
| `closemic` | A | Stop microphone sensor. |
| `micautostart` | A | Enable/disable microphone auto-start after boot [on\|off]<br/>`Usage: micautostart [on\|off]` |
| `micbitdepth` | A | Get/set bit depth.<br/>`Usage: micbitdepth [16\|32]` |
| `micdelete` | S | Delete recording(s).<br/>`Usage: micdelete "<filename>" \| micdelete all` |
| `micdeleteid` | A | Delete one owner-correlated recording.<br/>`Usage: micdeleteid <16hex> "<filename>"` |
| `micgain` | A | Get/set microphone gain.<br/>`Usage: micgain [0-100]` |
| `miclevel` |  | Get current audio level.<br/>`Usage: miclevel [json]` |
| `miclist` |  | List saved recordings.<br/>`Usage: miclist [json]` |
| `micread` |  | Read microphone sensor status.<br/>`Usage: micread [json]` |
| `micrecord` |  | Start/stop recording to WAV file (bare = show recording status).<br/>`Usage: micrecord [start [vad [<200..10000>] [trim]] \| stop \| 1 \| 0] micrecord startid <16hex> [vad [<200..10000>] [trim]] micrecord statusid <16hex> micrecord stopid <16hex> [discard]` |
| `micsamplerate` | A | Get/set sample rate.<br/>`Usage: micsamplerate [8000-48000]` |
| `micsource` | A | Get/set mic source: onboard microphone or G2 glasses.<br/>`Usage: micsource [auto\|onboard\|pdm\|g2]` |
| `micviz` |  | Real-time audio level visualizer.<br/>`Usage: micviz (press any key to stop)` |
| `openmic` | A | Start microphone sensor. |
| `pdmamplify` | A | Get/set the onboard PDM hardware gain multiplier (applies at the next capture start).<br/>`Usage: pdmamplify [1-15]` |
| `pdmhpf` | A | Get/set the onboard PDM high-pass cut-off in Hz, 0 = off (applies at the next capture start).<br/>`Usage: pdmhpf [0\|23-185]` |
| `voicefetch` |  | Stream a recording to the UART host as binary frames (CM5 bulk pull).<br/>`Usage: voicefetch "<path>" - path must be under /recordings or /sd/recordings. Sends META+AUDIO frames on the UART link, then replies with byte/frame totals and crc16.` |

## edgeimpulse

> On-device machine-learning image inference using TensorFlow Lite Micro models exported from Edge Impulse. Two things must be in place before inference: a model must be loaded with eimodelload "<file>" (models live in the models directory on LittleFS; eimodellist/eimodelinfo/eimodelunload manage them), or by the Edge Impulse start-at-boot setting, which loads the default model at boot. eidetect runs a single detection on a live camera frame and so additionally requires the camera to be opened (see opencamera), returning detected objects with confidence and bounding boxes as JSON; eifile "<path>" runs the same inference against a stored JPEG instead of the camera. eicontinuous 1 runs detection repeatedly in the background, eiconfidence <0.0-1.0> sets the minimum confidence to report, and eistatus shows current state. The eitrack family (eitrackenable, eitrackstatus, eitrackclear) adds cross-frame state tracking of detected objects on top of raw detections.

| Command | | Description |
| ------- | :-: | ----------- |
| `ei` |  | List the Edge Impulse commands.<br/>`Usage: ei` |
| `eiconfidence` |  | Set minimum detection confidence.<br/>`Usage: eiconfidence <0.0-1.0>` |
| `eicontinuous` |  | Start/stop continuous inference mode.<br/>`Usage: eicontinuous <0\|1>` |
| `eidetect` |  | Run single object detection inference.<br/>`Usage: eidetect` |
| `eifile` |  | Run inference on stored JPEG image.<br/>`Usage: eifile "<path>"` |
| `eimodel` |  | List the model management commands.<br/>`Usage: eimodel` |
| `eimodelinfo` |  | Show loaded model information.<br/>`Usage: eimodelinfo` |
| `eimodellist` |  | List available .tflite models.<br/>`Usage: eimodellist` |
| `eimodelload` |  | Load a TFLite model from LittleFS.<br/>`Usage: eimodelload "<filename>"` |
| `eimodelunload` |  | Unload the current model.<br/>`Usage: eimodelunload` |
| `eistatus` |  | Show Edge Impulse status.<br/>`Usage: eistatus` |
| `eitrack` |  | List the state tracking commands.<br/>`Usage: eitrack` |
| `eitrackclear` |  | Clear all tracked objects.<br/>`Usage: eitrackclear` |
| `eitrackenable` |  | Enable/disable state tracking.<br/>`Usage: eitrackenable <0\|1>` |
| `eitrackstatus` |  | Show currently tracked objects.<br/>`Usage: eitrackstatus` |

## transcription

> Session controls and private transcript pages shared by display and web interfaces.

| Command | | Description |
| ------- | :-: | ----------- |
| `transcription` |  | Transcription interface controls |
| `transcripts` |  | Browse your saved transcripts |

## note

> Private spoken notes saved under your own /stt/u<id>/notes/. note start works from the lens, web or local display; other consoles use note start glasses (paired wearer only). Status never shows the text; list/read/delete browse your notes.

| Command | | Description |
| ------- | :-: | ----------- |
| `note` |  | Private spoken notes (QuickNote) |

## stt

> Buffered local dictation through the shared audio HAL. Stop SR and the microphone sensor first, then use stt record [seconds]. Poll stt status [id], finish early with stt stop <id>, discard with stt cancel <id>, and read stt result <id> from the same authenticated session. Audio and results stay local; no host link is required. This backend produces text for review, not voice command execution.

| Command | | Description |
| ------- | :-: | ----------- |
| `stt` |  | Local continuous or bounded speech-to-text with private session-owned results.<br/>`Usage: stt start \| record [1..20 seconds] \| status [id] \| stop\|cancel\|result\|next\|draft <id> \| ack <id> <sequence>` |
| `sttdetector` |  | Show the mic-path (PDM gain, high-pass) and speech-detector (floors, percentile) tuning in effect.<br/>`Usage: sttdetector (onboard: sttautogain, pdmamplify, pdmhpf, sttspeechfloor, sttsilencefloor, sttnoisepercentile, sttdecisionpre, sttdecisionmod, sttdecisionhold; glasses: sttg2autogain, sttg2speechfloor, sttg2silencefloor, sttg2noisepercentile, sttg2decisionpre, sttg2decisionmod, sttg2decisionhold)` |
| `sttperf` |  | STT pacing/profiling per segment (timings and CPU only, never text).<br/>`Usage: sttperf [show\|clear\|log on\|off\|stages [all\|clear]]` |
| `sttraw` | A | Save the raw microphone input of continuous STT sessions to /sd/STT Raw for bench replay.<br/>`Usage: sttraw [status\|on\|off] (on arms the next continuous session; not kept across reboot)` |
| `sttvocab` |  | Speech vocabulary: names and terms with their display spelling (STT Models/user_vocab.tsv).<br/>`Usage: sttvocab [list] \| add DISPLAY[\|spoken] \| remove <spoken> \| reload (edits need an administrator)` |

## espsr

> Offline voice control built on Espressif ESP-SR: a WakeNet wake-word stage gates a MultiNet command-phrase recognizer, so the device waits for the wake word and then listens for a known command phrase. Note that srenable only reports the compile-time build flag and cannot toggle the feature at runtime; the real lifecycle commands are opensr to start the recognition pipeline and closesr to stop it. Starting the pipeline also arms voice command execution as the current authenticated user (and stopping it disarms); arming can be managed directly with voicearm/voicedisarm/voicestatus, and recognized phrases only execute commands while armed. The command vocabulary is managed with the srcmds family (list/add/del/clear plus save/reload to an SD file and srcmdssync to import phrases from the CLI registry). Recognition is tuned through srconfidence, srtimeout, the srtuning* audio controls (gain, AGC, VAD, filters), and srdebug* telemetry; the mic feed follows the device-wide source (set with `micsource auto|pdm|g2` — onboard PDM or the G2 glasses left-temple mic), and the srsnip* commands capture audio snippets (by default on the wake word) for debugging.

| Command | | Description |
| ------- | :-: | ----------- |
| `closesr` | A | Stop ESP-SR pipeline.<br/>`Usage: closesr` |
| `opensr` | A | Start ESP-SR pipeline and arm voice as the current user.<br/>`Usage: opensr` |
| `sr` |  | ESP-SR speech recognition commands.<br/>`Usage: opensr \| closesr \| srstatus \| srstack \| srcmdslist \| srdebuglevel \| srconfidence \| srtimeout \| srtuning \| sraccept \| srdyngain \| srraw \| srautotune \| srsnipstatus` |
| `sraccept` | A | Configure target acceptance policy (gap acceptance).<br/>`Usage: sraccept [on\|off\|floor <0.0-1.0>\|gap <0.0-1.0>\|speech <0\|1>]` |
| `srautotune` | A | Auto-cycle through gain configurations to find best settings.<br/>`Usage: srautotune [start\|stop\|status]` |
| `srcmds` | A | Manage MultiNet command phrases.<br/>`Usage: srcmdslist \| srcmdsadd <id> <phrase> \| srcmdsdel <phrase\|id> \| srcmdsclear confirm \| srcmdssave \| srcmdsreload \| srcmdssync` |
| `srcmdsadd` | A | Add or update a MultiNet command.<br/>`Usage: srcmdsadd <id> <phrase>` |
| `srcmdsclear` | S | Clear all MultiNet commands.<br/>`Usage: srcmdsclear confirm` |
| `srcmdsdel` | A | Delete a MultiNet command (by phrase or id).<br/>`Usage: srcmdsdel <phrase\|id>` |
| `srcmdslist` | A | List current MultiNet commands.<br/>`Usage: srcmdslist` |
| `srcmdsreload` | A | Reload commands from SD file.<br/>`Usage: srcmdsreload` |
| `srcmdssave` | S | Save current commands to SD file.<br/>`Usage: srcmdssave` |
| `srcmdssync` | A | Sync voice commands from CLI registry.<br/>`Usage: srcmdssync` |
| `srconfidence` | A | Get/set command confidence threshold.<br/>`Usage: srconfidence [<0.0-1.0> \| category <0.0-1.0> \| target <0.0-1.0>]` |
| `srdebug` |  | SR debug/telemetry commands.<br/>`Usage: srdebuglevel [0-4] \| srdebugtelem [ms] \| srdebugstats \| srdebugreset` |
| `srdebuglevel` | A | Set debug verbosity (0-4).<br/>`Usage: srdebuglevel [0-4]` |
| `srdebugreset` | A | Reset SR debug counters.<br/>`Usage: srdebugreset` |
| `srdebugstats` |  | Print current SR statistics.<br/>`Usage: srdebugstats` |
| `srdebugtelem` | A | Set periodic telemetry interval (ms, 0=off).<br/>`Usage: srdebugtelem [ms]` |
| `srdyngain` | A | Configure dynamic gain normalization (MultiNet input only).<br/>`Usage: srdyngain [on\|off\|min <0.1-10>\|max <0.1-10>\|target <1000-30000>\|alpha <0.0-1.0>\|reset]` |
| `srenable` | A | ESP-SR enable is a compile-time flag (cannot be toggled at runtime).<br/>`Usage: srenable (informational; opensr / closesr start and stop recognition)` |
| `srraw` | A | Toggle raw output mode (shows all MultiNet hypotheses).<br/>`Usage: srraw [on\|off]` |
| `srsnip` |  | Voice snippet capture commands.<br/>`Usage: srsnipon \| srsnipoff \| srsnipstart \| srsnipstop \| srsnipstatus \| srsnipconfig` |
| `srsnipconfig` | A | Configure snippet capture params.<br/>`Usage: srsnipconfig [pre_ms <100-5000> \| max_ms <1000-30000> \| dest <auto\|sd\|internal>]` |
| `srsnipoff` | A | Disable auto-capture.<br/>`Usage: srsnipoff` |
| `srsnipon` | A | Enable auto-capture on wake word.<br/>`Usage: srsnipon` |
| `srsnipstart` |  | Start manual snippet capture now.<br/>`Usage: srsnipstart` |
| `srsnipstatus` |  | Show snippet capture status.<br/>`Usage: srsnipstatus` |
| `srsnipstop` |  | Stop manual snippet capture and save.<br/>`Usage: srsnipstop` |
| `srstack` |  | Show sr_main stack high-water mark (run after voice stress test).<br/>`Usage: srstack` |
| `srstatus` |  | Show ESP-SR status.<br/>`Usage: srstatus` |
| `srtimeout` | A | Get/set command listening timeout.<br/>`Usage: srtimeout [1000-30000]` |
| `srtuning` |  | Show the audio tuning values.<br/>`Usage: srtuning` |
| `srtuningagc` | A | Set AGC mode (0=off, 1-3=levels).<br/>`Usage: srtuningagc <0-3>` |
| `srtuningfilters` | A | Toggle audio filters (high-pass + pre-emphasis).<br/>`Usage: srtuningfilters <on\|off>` |
| `srtuninggain` | A | Set AFE linear gain (0.1-10.0).<br/>`Usage: srtuninggain <0.1-10.0>` |
| `srtuningswgain` | A | Set software gain (1.0-50.0) by updating shared micgain.<br/>`Usage: srtuningswgain <1.0-50.0>` |
| `srtuningvad` | A | Set VAD sensitivity (0-4).<br/>`Usage: srtuningvad <0-4>` |
| `voicearm` |  | Arm voice command execution as the current authenticated user.<br/>`Usage: voicearm` |
| `voicecancel` |  | Spoken only: say cancel (or nevermind) after the wake word. Typed, it answers an error. |
| `voicedisarm` |  | Disarm voice command execution.<br/>`Usage: voicedisarm` |
| `voicehelp` |  | Spoken only: say help after the wake word. Typed, it answers an error. |
| `voicestatus` |  | Show voice arming status.<br/>`Usage: voicestatus` |

## i2c

> The i2c module configures and diagnoses up to two I2C buses and the sensor device registry. There are two buses with a deliberate naming convention: bus 0 is I2C1 (Arduino Wire1, the primary STEMMA QT / sensor bus) and bus 1 is I2C2 (Wire, the optional secondary bus); each has its own enable flag and SDA/SCL pin settings, and bus/pin changes require a reboot. Each sensor can be routed to either bus with a per-device command. Each sensor's routing verb now lives in that sensor's own module (run 'help gps' for gpsBus, 'help rtc' for rtcBus, and so on); oledBus, inputBus and fuelGaugeBus stay here. The persisted setting names are unchanged. all taking 0 or 1 and needing a reboot. Discovery and diagnostics: i2cscan dumps raw addresses found on each active bus; detect reports configured-vs-present hardware and detect apply (admin) auto-enables newly detected cheap devices; i2cmetrics/i2cstats/i2chealth show bus performance, error counters, and per-device health. Bus recovery: i2cpause/i2cresume stop and restart sensor polling, i2creset does a pause-recover-resume cycle, and i2crecover <address> clears a single device degraded state. The device registry is exposed via sensors [filter|json], sensorinfo <name>, devices, discover, and devicefile; sensorautostart [sensor] [on|off] controls which sensors start polling automatically at boot.

| Command | | Description |
| ------- | :-: | ----------- |
| `detect` |  | Detect hardware: scan I2C buses, diff vs. configured features.<br/>`Usage: detect [apply] detect - read-only report (present/enabled/missing) detect apply - auto-enable cheap detected devices (admin; reboot for some)` |
| `devicefile` |  | Show device registry JSON file. |
| `devices` |  | Show discovered I2C device registry. (add 'json' for JSON output) |
| `discover` |  | Re-scan and register I2C devices. |
| `fuelgaugebus` | A | Route MAX17048 fuel gauge to bus: <0\|1> (reboot required)<br/>`Usage: fuelGaugeBus <0\|1>` |
| `i2c2busenabled` | A | Enable/disable I2C2 bus: <0\|1> (reboot required)<br/>`Usage: i2c2BusEnabled <0\|1>` |
| `i2c2sclpin` | A | Set I2C2 SCL pin: <-1..> (-1=unavailable)<br/>`Usage: i2c2SclPin <-1..> (-1=unavailable)` |
| `i2c2sdapin` | A | Set I2C2 SDA pin: <-1..> (-1=unavailable)<br/>`Usage: i2c2SdaPin <-1..> (-1=unavailable)` |
| `i2cbusenabled` | A | Enable/disable I2C1 bus: <0\|1> (reboot required)<br/>`Usage: i2cBusEnabled <0\|1>` |
| `i2chealth` |  | Show per-device I2C health status. (add 'json' for JSON output) |
| `i2cmetrics` |  | Show I2C bus performance metrics. (add 'json' for JSON output) |
| `i2cpause` | A | Pause all I2C sensor polling. |
| `i2crecover` | A | Clear degraded state for device: <address><br/>`Usage: i2crecover <address> (hex 0x01-0x7F or decimal 1-127)` |
| `i2creset` | A | Reset I2C bus: pause polling, recover bus, resume. |
| `i2cresume` | A | Resume I2C sensor polling. |
| `i2cscan` |  | Scan I2C bus for devices. |
| `i2csclpin` | A | Set I2C1 SCL pin: <0..> (max GPIO for this board)<br/>`Usage: i2cSclPin <0..> (max GPIO for this board)` |
| `i2csdapin` | A | Set I2C1 SDA pin: <0..> (max GPIO for this board)<br/>`Usage: i2cSdaPin <0..> (max GPIO for this board)` |
| `i2cstats` |  | I2C bus statistics and errors. |
| `inputbus` | A | Route input device to bus: <0\|1> (reboot required; the ANO: next openinput)<br/>`Usage: inputBus <0\|1>` |
| `oledbus` | A | Route OLED to bus: <0\|1> (reboot required)<br/>`Usage: oledBus <0\|1>` |
| `sensorautostart` | A | Sensor auto-start: [sensor] [n] [on\|off]<br/>`Usage: sensorautostart [sensor] [n] [on\|off] sensorautostart all [on\|off] Sensors: thermal, tof, imu, gps, fmradio, apds, input, rtc, presence, encoder (n: encoder 1..4)` |
| `sensorinfo` |  | Sensor details: <name><br/>`Usage: sensorinfo <sensor_name> Example: sensorinfo BNO055` |
| `sensors` |  | List I2C sensors [filter]<br/>`Usage: sensors [filter] - filter by name, description, or manufacturer sensors json [brief] - live state (+readings; 'brief' = state only, no data) Example: sensors temperature, sensors json brief` |

## automation

> The automation module runs saved jobs (stored in automations.json) that execute one or more CLI commands on a schedule, condition, or system event. Every automation has one of four trigger types: atTime (fires daily at time=HH:MM, optionally limited to days=Mon,Tue,...), afterDelay (fires once after delayms milliseconds), interval (fires repeatedly every intervalms milliseconds), or event (fires when a system event occurs: on=<kind> with an optional match=<text> filter against the event's subject/detail — run 'events' to see recent kinds like peer_online, text_rx, battery_low, login_fail); jobs can also carry runatboot=1 to fire at startup. The primary entry point is automation <subcommand> (list, add, enable, disable, delete, run, trigger, sanitize, recompute); automationlist lists the jobs for any signed-in user. Note the important distinction: automation run id=<id> executes a job commands immediately, whereas automation trigger id=<id> only arms an afterDelay/manual timer so it fires after its delay; and automation system enable|disable|status is the global master switch that gates whether the scheduler runs at all, independent of each job own enabled flag. Jobs may also include an optional condition expression, and conditional commands use an IF <expr> THEN <command> [ELSE <command>] form (e.g. IF temp>75 THEN ledcolor red). Conditions can check sensors (temp, distance, light, motion), time (time, hour, day, ntp), system state (battery, heap, psram, fsfree, uptime, chiptemp), connectivity (wifi, rssi, peers, ble), ESP-NOW/bond (espnow, bond_mode, bond_paired, bond_online, bond_synced, bond_role, bond_rssi, bond_peer_heap, bond_peer_uptime, pairmode, pairmode_secs, peersknown, stalestpeerage), location (gps, speed, sats, wp_dist:<name>), the on-device model (llm), and ESP-NOW metadata (room, zone, tags); numeric variables use the > < = >= <= != operators and string/enum variables use = != CONTAINS. A true condition fires every time its trigger is due. Supporting commands: validateconditions checks conditional syntax without running it, autolog records automation activity to a file, and print <message> broadcasts text to all outputs.

| Command | | Description |
| ------- | :-: | ----------- |
| `autolog` | A | Automation logging: autolog start <file> \| stop \| status.<br/>`Usage: autolog start <filename> \| autolog stop \| autolog status` |
| `automation` | A | Automation system: automation <subcommand> [args].<br/>`Usage: automation <system enable\|disable\|status \| list \| add \| enable \| disable \| delete \| run \| trigger \| sanitize \| recompute>` |
| `automationlist` |  | List all automations. |
| `print` |  | Broadcast a message to all outputs: print <message>. |
| `validateconditions` | A | Validate conditional automation syntax: validateconditions IF temp>75 THEN ledcolor red. |

## battery

> The battery module reports cell state and keeps a time-series log; it is only present when battery monitoring is compiled in. The backend is a MAX17048 fuel gauge over I2C or a calibrated ADC voltage divider selected by the board. ADC charge percentage is approximate; the charger can drive the measured BAT node even without a cell. USB, charging and cell presence are reported only when known. Missing readings are null in JSON, not zero/full estimates. batterystatus prints voltage, charge percentage, charging/USB state, and a coarse status label, or returns the same data as JSON. batterylog manages a CSV discharge/charge log written to the device for later graphing: with no args it shows status, and subcommands are on/off (enable/disable), interval <5..3600> seconds (sampling period), tail (show the most recent rows), and clear (erase the log); significant events such as sleep/wake are always recorded regardless of the interval. batterycalibrate (admin) reinitializes ADC calibration or re-probes the fuel gauge.

| Command | | Description |
| ------- | :-: | ----------- |
| `batterycalibrate` | A | Reinitialize ADC calibration or re-probe the fuel gauge |
| `batterylog` |  | Battery time-series CSV log (status/on/off/interval/tail/clear)<br/>`Usage: batterylog [status\|on\|off\|interval <s>\|tail\|clear]` |
| `batterystatus` |  | Show battery voltage, charge level, and status |

## debug

> The debug subsystem controls diagnostic logging verbosity across every part of the firmware. Its core is a large set of per-subsystem debug-flag toggles (for example debugwifi, debughttprequests, debugespnowcore, debugcamera, debugimuvalues) that each follow a <0|1> [temp|runtime] model: with no mode the new state is persisted to flash, while temp or runtime flips only the live runtime flag and is NOT saved (it reverts on reboot). Many subsystems have a parent flag plus finer sub-flags (lifecycle/polling/values, or core/router/mesh/topo for ESP-NOW); the parent acts as a master switch and any sub-flag also lights its parent. Separate from the on/off flags, loglevel sets a severity threshold (error|warn|info|debug, persisted) and debugverbose is a global override. Related commands manage where output goes: outserial gates the UART lane (persisted), outg2/outble open runtime CLI streams to G2 glasses / BLE clients (session-only, reset on reboot), log starts/stops system-wide logging to a file, loglink routes ESP-IDF framework logs through the unified output queue, and debugstack/debugbuffer expose low-level trace and queue diagnostics.

| Command | | Description |
| ------- | :-: | ----------- |
| `commandmodulesummary` | A | Show command module summary. |
| `debuganoencoder` | A | Debug ANO rotary encoder driver internals.<br/>`Usage: debuganoencoder <0\|1> [temp\|runtime]` |
| `debuganoencoderlifecycle` | A | Debug ANO encoder init/connect/recovery.<br/>`Usage: debuganoencoderlifecycle <0\|1> [temp\|runtime]` |
| `debuganoencoderpolling` | A | Debug ANO encoder poll/encoder reads.<br/>`Usage: debuganoencoderpolling <0\|1> [temp\|runtime]` |
| `debuganoencodervalues` | A | Debug ANO encoder rotation/button events.<br/>`Usage: debuganoencodervalues <0\|1> [temp\|runtime]` |
| `debugapds` | A | Debug APDS sensor (APDS9960).<br/>`Usage: debugapds <0\|1> [temp\|runtime]` |
| `debugapdslifecycle` | A | Debug APDS init/connect/recovery.<br/>`Usage: debugapdslifecycle <0\|1> [temp\|runtime]` |
| `debugapdspolling` | A | Debug APDS poll cadence.<br/>`Usage: debugapdspolling <0\|1> [temp\|runtime]` |
| `debugapdsvalues` | A | Debug APDS color/proximity/gesture values.<br/>`Usage: debugapdsvalues <0\|1> [temp\|runtime]` |
| `debugauth` | A | Debug authentication (parent flag).<br/>`Usage: debugauth <0\|1> [temp\|runtime]` |
| `debugauthbootid` | A | Debug auth boot ID.<br/>`Usage: debugauthbootid <0\|1> [temp\|runtime]` |
| `debugauthcookies` | A | Debug auth cookies.<br/>`Usage: debugauthcookies <0\|1> [temp\|runtime]` |
| `debugauthlogin` | A | Debug auth login.<br/>`Usage: debugauthlogin <0\|1> [temp\|runtime]` |
| `debugauthsessions` | A | Debug auth sessions.<br/>`Usage: debugauthsessions <0\|1> [temp\|runtime]` |
| `debugautocondition` | A | Debug automations conditions.<br/>`Usage: debugautocondition <0\|1> [temp\|runtime]` |
| `debugautoexec` | A | Debug automations execution.<br/>`Usage: debugautoexec <0\|1> [temp\|runtime]` |
| `debugautomations` | A | Debug automations scheduler and actions.<br/>`Usage: debugautomations <0\|1> [temp\|runtime]` |
| `debugautoscheduler` | A | Debug automations scheduler.<br/>`Usage: debugautoscheduler <0\|1> [temp\|runtime]` |
| `debugautotiming` | A | Debug automations timing.<br/>`Usage: debugautotiming <0\|1> [temp\|runtime]` |
| `debugbluetooth` | A | Debug Bluetooth (parent flag).<br/>`Usage: debugbluetooth <0\|1> [temp\|runtime]` |
| `debugbluetoothcore` | A | Debug Bluetooth core lifecycle; also shows the Bluetooth stack's own routine log lines (Bluedroid BT_*, BLE_COEX records, the P4X-EYE's C6 console), which print only errors while it is off.<br/>`Usage: debugbluetoothcore <0\|1> [temp\|runtime] On: BT_APPL, BT_BTM, BT_HCI, BT_L2CAP, BT_GATT, BT_SMP, BT_BTC, BLE_INIT, BTDM_INIT, BLE_COEX and C6> at their stock level (C6> while the console mirror runs, see c6console). Off (default): errors only. debugbluetooth alone does not change them; a saved esplog level wins.` |
| `debugbluetoothdata` | A | Debug Bluetooth command/data path.<br/>`Usage: debugbluetoothdata <0\|1> [temp\|runtime]` |
| `debugbluetoothgatt` | A | Debug Bluetooth GATT operations.<br/>`Usage: debugbluetoothgatt <0\|1> [temp\|runtime]` |
| `debugbuffer` | A | Show debug ring buffer status. |
| `debugcamera` | A | Debug camera (parent flag).<br/>`Usage: debugcamera <0\|1> [temp\|runtime]` |
| `debugcameracapture` | A | Debug captureFrame, JPEG validation, fb buffer, recovery.<br/>`Usage: debugcameracapture <0\|1> [temp\|runtime]` |
| `debugcameralifecycle` | A | Debug camera init/stop/PWDN-RESET/GPIO state.<br/>`Usage: debugcameralifecycle <0\|1> [temp\|runtime]` |
| `debugcamerasettings` | A | Debug runtime camera resolution/quality changes.<br/>`Usage: debugcamerasettings <0\|1> [temp\|runtime]` |
| `debugcameravideo` | A | Debug video recording start/finalize, frame writing.<br/>`Usage: debugcameravideo <0\|1> [temp\|runtime]` |
| `debugcli` | A | Debug CLI processing.<br/>`Usage: debugcli <0\|1> [temp\|runtime]` |
| `debugcliexecution` | A | Debug CLI execution.<br/>`Usage: debugcliexecution <0\|1> [temp\|runtime]` |
| `debugcliqueue` | A | Debug CLI queue.<br/>`Usage: debugcliqueue <0\|1> [temp\|runtime]` |
| `debugclivalidation` | A | Debug CLI validation.<br/>`Usage: debugclivalidation <0\|1> [temp\|runtime]` |
| `debugcmdflowcontext` | A | Debug command flow context.<br/>`Usage: debugcmdflowcontext <0\|1> [temp\|runtime]` |
| `debugcmdflowqueue` | A | Debug command flow queue.<br/>`Usage: debugcmdflowqueue <0\|1> [temp\|runtime]` |
| `debugcmdflowrouting` | A | Debug command flow routing.<br/>`Usage: debugcmdflowrouting <0\|1> [temp\|runtime]` |
| `debugcommandflow` | A | Debug command flow.<br/>`Usage: debugcommandflow <0\|1> [temp\|runtime]` |
| `debugcommandsystem` | A | Debug modular command registry operations.<br/>`Usage: debugcommandsystem <0\|1> [temp\|runtime]` |
| `debugdatetime` | A | Debug NTP/date-time (parent flag).<br/>`Usage: debugdatetime <0\|1> [temp\|runtime]` |
| `debugdatetimeanchor` | A | Debug NTP boot anchor write/read.<br/>`Usage: debugdatetimeanchor <0\|1> [temp\|runtime]` |
| `debugdatetimeresolve` | A | Debug NTP timestamp resolution for users.<br/>`Usage: debugdatetimeresolve <0\|1> [temp\|runtime]` |
| `debugdatetimesetup` | A | Debug NTP setup / configTime calls.<br/>`Usage: debugdatetimesetup <0\|1> [temp\|runtime]` |
| `debugdatetimesync` | A | Debug NTP sync loop (DNS, wait, result).<br/>`Usage: debugdatetimesync <0\|1> [temp\|runtime]` |
| `debugdisplay` | A | Debug OLED init/probe/boot-animation/mode-transitions.<br/>`Usage: debugdisplay <0\|1> [temp\|runtime]` |
| `debugespnowcore` | A | Debug ESP-NOW core operations.<br/>`Usage: debugespnowcore <0\|1> [temp\|runtime]` |
| `debugespnowmesh` | A | Debug ESP-NOW mesh operations.<br/>`Usage: debugespnowmesh <0\|1> [temp\|runtime]` |
| `debugespnowmetadata` | A | Debug ESP-NOW metadata exchange (REQ/RESP/PUSH).<br/>`Usage: debugespnowmetadata <0\|1> [temp\|runtime]` |
| `debugespnowrouter` | A | Debug ESP-NOW router operations.<br/>`Usage: debugespnowrouter <0\|1> [temp\|runtime]` |
| `debugespnowstream` | A | Debug ESP-NOW streaming output.<br/>`Usage: debugespnowstream <0\|1> [temp\|runtime]` |
| `debugespnowtopo` | A | Debug ESP-NOW topology discovery.<br/>`Usage: debugespnowtopo <0\|1> [temp\|runtime]` |
| `debugflags` | A | Show the live debug mask (4 hex words) and the flags it has set.<br/>`Usage: debugflags` |
| `debugfmradio` | A | Debug FM Radio operations.<br/>`Usage: debugfmradio <0\|1> [temp\|runtime]` |
| `debugfmradiolifecycle` | A | Debug FM radio init/tune/recovery.<br/>`Usage: debugfmradiolifecycle <0\|1> [temp\|runtime]` |
| `debugfmradiopolling` | A | Debug FM radio poll cadence.<br/>`Usage: debugfmradiopolling <0\|1> [temp\|runtime]` |
| `debugfmradiovalues` | A | Debug FM radio RDS/RSSI/state values.<br/>`Usage: debugfmradiovalues <0\|1> [temp\|runtime]` |
| `debugg2` | A | Debug G2 smart glasses BLE operations.<br/>`Usage: debugg2 <0\|1> [temp\|runtime]` |
| `debugg2dump` | A | Debug G2 ring-buffer dumps on errors.<br/>`Usage: debugg2dump <0\|1> [temp\|runtime]` |
| `debugg2events` | A | Debug G2 DevEvents/SysEvents/gestures.<br/>`Usage: debugg2events <0\|1> [temp\|runtime]` |
| `debugg2heartbeat` | A | Debug G2 heartbeat TX + acks (loud).<br/>`Usage: debugg2heartbeat <0\|1> [temp\|runtime]` |
| `debugg2lifecycle` | A | Debug G2 BLE lifecycle (scan/connect/MTU).<br/>`Usage: debugg2lifecycle <0\|1> [temp\|runtime]` |
| `debugg2pages` | A | Debug G2 page-swap worker / hijack / lens state.<br/>`Usage: debugg2pages <0\|1> [temp\|runtime]` |
| `debugg2protocol` | A | Debug G2 envelope TX/RX, CRC, fragmentation.<br/>`Usage: debugg2protocol <0\|1> [temp\|runtime]` |
| `debuggps` | A | Debug GPS sensor (PA1010D).<br/>`Usage: debuggps <0\|1> [temp\|runtime]` |
| `debuggpslifecycle` | A | Debug GPS init/connect/recovery.<br/>`Usage: debuggpslifecycle <0\|1> [temp\|runtime]` |
| `debuggpspolling` | A | Debug GPS poll cadence.<br/>`Usage: debuggpspolling <0\|1> [temp\|runtime]` |
| `debuggpsvalues` | A | Debug GPS NMEA/fix/coordinate values.<br/>`Usage: debuggpsvalues <0\|1> [temp\|runtime]` |
| `debughttp` | A | Debug HTTP requests.<br/>`Usage: debughttp <0\|1> [temp\|runtime]` |
| `debughttphandlers` | A | Debug HTTP handlers.<br/>`Usage: debughttphandlers <0\|1> [temp\|runtime]` |
| `debughttprequests` | A | Debug HTTP requests.<br/>`Usage: debughttprequests <0\|1> [temp\|runtime]` |
| `debughttpresponses` | A | Debug HTTP responses.<br/>`Usage: debughttpresponses <0\|1> [temp\|runtime]` |
| `debughttps` | A | Debug HTTPS/TLS handshake + connection errors (ESP-IDF logs).<br/>`Usage: debughttps <0\|1> [temp\|runtime]` |
| `debughttpstreaming` | A | Debug HTTP streaming.<br/>`Usage: debughttpstreaming <0\|1> [temp\|runtime]` |
| `debugi2c` | A | Debug I2C bus (parent flag).<br/>`Usage: debugi2c <0\|1> [temp\|runtime]` |
| `debugi2cautostart` | A | Debug I2C sensor auto-start orchestration + init results.<br/>`Usage: debugi2cautostart <0\|1> [temp\|runtime]` |
| `debugi2cbus` | A | Debug I2C bus lifecycle, polling pause/resume, status bumps.<br/>`Usage: debugi2cbus <0\|1> [temp\|runtime]` |
| `debugi2cdiscovery` | A | Debug I2C device probing, registry, scan results.<br/>`Usage: debugi2cdiscovery <0\|1> [temp\|runtime]` |
| `debugimu` | A | Debug IMU sensor (BNO055).<br/>`Usage: debugimu <0\|1> [temp\|runtime]` |
| `debugimulifecycle` | A | Debug IMU init/connect/recovery.<br/>`Usage: debugimulifecycle <0\|1> [temp\|runtime]` |
| `debugimupolling` | A | Debug IMU poll cadence.<br/>`Usage: debugimupolling <0\|1> [temp\|runtime]` |
| `debugimuvalues` | A | Debug IMU orientation/acceleration values.<br/>`Usage: debugimuvalues <0\|1> [temp\|runtime]` |
| `debuginput` | A | Debug input abstraction layer (HAL_Input + OLED dispatch).<br/>`Usage: debuginput <0\|1> [temp\|runtime]` |
| `debuginputlifecycle` | A | Debug input abstraction layer lifecycle.<br/>`Usage: debuginputlifecycle <0\|1> [temp\|runtime]` |
| `debuginputpolling` | A | Debug input abstraction layer poll/dispatch.<br/>`Usage: debuginputpolling <0\|1> [temp\|runtime]` |
| `debuginputvalues` | A | Debug input abstraction layer event values.<br/>`Usage: debuginputvalues <0\|1> [temp\|runtime]` |
| `debugllm` | A | Debug on-device LLM (parent flag).<br/>`Usage: debugllm <0\|1> [temp\|runtime]` |
| `debugllmforward` | A | Debug LLM transformer forward (verbose).<br/>`Usage: debugllmforward <0\|1> [temp\|runtime]` |
| `debugllmgenerate` | A | Debug LLM generation loop and sampling.<br/>`Usage: debugllmgenerate <0\|1> [temp\|runtime]` |
| `debugllmload` | A | Debug LLM checkpoint load and validation.<br/>`Usage: debugllmload <0\|1> [temp\|runtime]` |
| `debugllmmemory` | A | Debug LLM PSRAM budget and context cap.<br/>`Usage: debugllmmemory <0\|1> [temp\|runtime]` |
| `debugllmtokenizer` | A | Debug LLM tokenizer / BPE.<br/>`Usage: debugllmtokenizer <0\|1> [temp\|runtime]` |
| `debuglogger` | A | Debug sensor logger internals.<br/>`Usage: debuglogger <0\|1> [temp\|runtime]` |
| `debugmaps` | A | Debug maps (parent flag).<br/>`Usage: debugmaps <0\|1> [temp\|runtime]` |
| `debugmapsloading` | A | Debug map file loading and tile directory.<br/>`Usage: debugmapsloading <0\|1> [temp\|runtime]` |
| `debugmapsperf` | A | Debug map performance timing (render ms, tile I/O, cache, FPS).<br/>`Usage: debugmapsperf <0\|1> [temp\|runtime]` |
| `debugmapsrendering` | A | Debug map render pipeline and feature drawing.<br/>`Usage: debugmapsrendering <0\|1> [temp\|runtime]` |
| `debugmemory` | A | Debug memory (parent flag).<br/>`Usage: debugmemory <0\|1> [temp\|runtime]` |
| `debugmemorybuffers` | A | Debug response/cookie buffer sizing diagnostics.<br/>`Usage: debugmemorybuffers <0\|1> [temp\|runtime]` |
| `debugmemoryheap` | A | Debug per-task heap (free/min/largest), DRAM low watermark.<br/>`Usage: debugmemoryheap <0\|1> [temp\|runtime]` |
| `debugmemorystack` | A | Debug per-task stack watermarks + peak reports.<br/>`Usage: debugmemorystack <0\|1> [temp\|runtime]` |
| `debugmiclifecycle` | A | Debug microphone init/start/stop.<br/>`Usage: debugmiclifecycle <0\|1> [temp\|runtime]` |
| `debugmicpolling` | A | Debug microphone capture cadence.<br/>`Usage: debugmicpolling <0\|1> [temp\|runtime]` |
| `debugmicrophone` | A | Debug microphone operations.<br/>`Usage: debugmicrophone <0\|1> [temp\|runtime]` |
| `debugmicvalues` | A | Debug microphone level/sample values.<br/>`Usage: debugmicvalues <0\|1> [temp\|runtime]` |
| `debugmqtt` | A | Debug MQTT (parent flag).<br/>`Usage: debugmqtt <0\|1> [temp\|runtime]` |
| `debugmqttcommands` | A | Debug MQTT inbound commands + auth.<br/>`Usage: debugmqttcommands <0\|1> [temp\|runtime]` |
| `debugmqttconnection` | A | Debug MQTT connect/disconnect/TLS/init.<br/>`Usage: debugmqttconnection <0\|1> [temp\|runtime]` |
| `debugmqttdiscovery` | A | Debug MQTT Home Assistant auto-discovery.<br/>`Usage: debugmqttdiscovery <0\|1> [temp\|runtime]` |
| `debugmqttpubsub` | A | Debug MQTT publish/subscribe + received messages.<br/>`Usage: debugmqttpubsub <0\|1> [temp\|runtime]` |
| `debugnotifications` | A | Debug notification pipeline: ring lag/skips, stale/cooldown drops, SSE saturation.<br/>`Usage: debugnotifications <0\|1> [temp\|runtime]` |
| `debugperfheap` | A | Debug performance heap.<br/>`Usage: debugperfheap <0\|1> [temp\|runtime]` |
| `debugperformance` | A | Debug performance metrics.<br/>`Usage: debugperformance <0\|1> [temp\|runtime]` |
| `debugperfstack` | A | Debug performance stack.<br/>`Usage: debugperfstack <0\|1> [temp\|runtime]` |
| `debugperftiming` | A | Debug performance timing.<br/>`Usage: debugperftiming <0\|1> [temp\|runtime]` |
| `debugpresence` | A | Debug presence sensor (STHS34PF80).<br/>`Usage: debugpresence <0\|1> [temp\|runtime]` |
| `debugpresencelifecycle` | A | Debug presence sensor init/connect/recovery.<br/>`Usage: debugpresencelifecycle <0\|1> [temp\|runtime]` |
| `debugpresencepolling` | A | Debug presence sensor poll cadence.<br/>`Usage: debugpresencepolling <0\|1> [temp\|runtime]` |
| `debugpresencevalues` | A | Debug presence detection values.<br/>`Usage: debugpresencevalues <0\|1> [temp\|runtime]` |
| `debugring` | A | Debug R1 health ring (parent flag).<br/>`Usage: debugring <0\|1> [temp\|runtime]` |
| `debugringdump` | A | Debug R1 raw hex frame/payload dumps (loud).<br/>`Usage: debugringdump <0\|1> [temp\|runtime]` |
| `debugringhealth` | A | Debug R1 telemetry cache + history sweep.<br/>`Usage: debugringhealth <0\|1> [temp\|runtime]` |
| `debugringlifecycle` | A | Debug R1 scan/connect/GATT/disconnect.<br/>`Usage: debugringlifecycle <0\|1> [temp\|runtime]` |
| `debugringprotocol` | A | Debug R1 per-frame decode/reassembly (loud).<br/>`Usage: debugringprotocol <0\|1> [temp\|runtime]` |
| `debugringsetup` | A | Debug R1 setup ritual + clock custody.<br/>`Usage: debugringsetup <0\|1> [temp\|runtime]` |
| `debugringtxn` | A | Debug R1 transactions + packetAck (loud).<br/>`Usage: debugringtxn <0\|1> [temp\|runtime]` |
| `debugrtc` | A | Debug RTC sensor (DS3231).<br/>`Usage: debugrtc <0\|1> [temp\|runtime]` |
| `debugrtclifecycle` | A | Debug RTC init/connect/recovery.<br/>`Usage: debugrtclifecycle <0\|1> [temp\|runtime]` |
| `debugrtcpolling` | A | Debug RTC poll cadence.<br/>`Usage: debugrtcpolling <0\|1> [temp\|runtime]` |
| `debugrtcvalues` | A | Debug RTC time-read values.<br/>`Usage: debugrtcvalues <0\|1> [temp\|runtime]` |
| `debugsr` | A | Debug ESP-SR speech recognition (parent flag).<br/>`Usage: debugsr <0\|1> [temp\|runtime]` |
| `debugsrafe` | A | Debug SR AFE chain (VAD/noise/gain).<br/>`Usage: debugsrafe <0\|1> [temp\|runtime]` |
| `debugsrcommand` | A | Debug SR MultiNet command recognition.<br/>`Usage: debugsrcommand <0\|1> [temp\|runtime]` |
| `debugsrlifecycle` | A | Debug SR init/start/stop verbose.<br/>`Usage: debugsrlifecycle <0\|1> [temp\|runtime]` |
| `debugsrtuning` | A | Debug SR auto-tune sweeps + threshold.<br/>`Usage: debugsrtuning <0\|1> [temp\|runtime]` |
| `debugsrwake` | A | Debug SR wake word detection events.<br/>`Usage: debugsrwake <0\|1> [temp\|runtime]` |
| `debugsse` | A | Debug Server-Sent Events.<br/>`Usage: debugsse <0\|1> [temp\|runtime]` |
| `debugssebroadcast` | A | Debug SSE broadcast.<br/>`Usage: debugssebroadcast <0\|1> [temp\|runtime]` |
| `debugsseconnection` | A | Debug SSE connection.<br/>`Usage: debugsseconnection <0\|1> [temp\|runtime]` |
| `debugsseevents` | A | Debug SSE events.<br/>`Usage: debugsseevents <0\|1> [temp\|runtime]` |
| `debugstack` | A | Low-level stack/heap trace to Serial: <on\|off>.<br/>`Usage: debugstack <0\|1\|on\|off>` |
| `debugstorage` | A | Debug storage operations.<br/>`Usage: debugstorage <0\|1> [temp\|runtime]` |
| `debugstoragefiles` | A | Debug storage files.<br/>`Usage: debugstoragefiles <0\|1> [temp\|runtime]` |
| `debugstoragejson` | A | Debug storage JSON.<br/>`Usage: debugstoragejson <0\|1> [temp\|runtime]` |
| `debugstoragemigration` | A | Debug storage migration.<br/>`Usage: debugstoragemigration <0\|1> [temp\|runtime]` |
| `debugstoragepermissions` | A | Debug storage [PERM] DENY audit.<br/>`Usage: debugstoragepermissions <0\|1> [temp\|runtime]` |
| `debugstoragesettings` | A | Debug storage settings.<br/>`Usage: debugstoragesettings <0\|1> [temp\|runtime]` |
| `debugsystem` | A | Debug system/boot operations.<br/>`Usage: debugsystem <0\|1> [temp\|runtime]` |
| `debugsystemboot` | A | Debug system boot.<br/>`Usage: debugsystemboot <0\|1> [temp\|runtime]` |
| `debugsystemconfig` | A | Debug system config.<br/>`Usage: debugsystemconfig <0\|1> [temp\|runtime]` |
| `debugsystemhardware` | A | Debug system hardware.<br/>`Usage: debugsystemhardware <0\|1> [temp\|runtime]` |
| `debugsystemtasks` | A | Debug system tasks.<br/>`Usage: debugsystemtasks <0\|1> [temp\|runtime]` |
| `debugthermal` | A | Debug thermal sensor (MLX90640).<br/>`Usage: debugthermal <0\|1> [temp\|runtime]` |
| `debugthermallifecycle` | A | Debug thermal init/connect/recovery.<br/>`Usage: debugthermallifecycle <0\|1> [temp\|runtime]` |
| `debugthermalpolling` | A | Debug thermal poll cadence/FPS/capture.<br/>`Usage: debugthermalpolling <0\|1> [temp\|runtime]` |
| `debugthermalvalues` | A | Debug thermal value updates/interpolation.<br/>`Usage: debugthermalvalues <0\|1> [temp\|runtime]` |
| `debugtof` | A | Debug ToF sensor (VL53L4CX).<br/>`Usage: debugtof <0\|1> [temp\|runtime]` |
| `debugtoflifecycle` | A | Debug ToF init/connect/recovery.<br/>`Usage: debugtoflifecycle <0\|1> [temp\|runtime]` |
| `debugtofpolling` | A | Debug ToF poll cadence/capture.<br/>`Usage: debugtofpolling <0\|1> [temp\|runtime]` |
| `debugtofvalues` | A | Debug ToF range/object detection values.<br/>`Usage: debugtofvalues <0\|1> [temp\|runtime]` |
| `debuguart` | A | Debug UART host link (parent flag).<br/>`Usage: debuguart <0\|1> [temp\|runtime]` |
| `debuguartcontrol` | A | Debug UART CM5/liveaudio control intrinsics.<br/>`Usage: debuguartcontrol <0\|1> [temp\|runtime]` |
| `debuguartlifecycle` | A | Debug UART link/session lifecycle.<br/>`Usage: debuguartlifecycle <0\|1> [temp\|runtime]` |
| `debugusers` | A | Debug user management.<br/>`Usage: debugusers <0\|1> [temp\|runtime]` |
| `debugusersmgmt` | A | Debug users management.<br/>`Usage: debugusersmgmt <0\|1> [temp\|runtime]` |
| `debugusersquery` | A | Debug users query.<br/>`Usage: debugusersquery <0\|1> [temp\|runtime]` |
| `debugusersregister` | A | Debug users registration.<br/>`Usage: debugusersregister <0\|1> [temp\|runtime]` |
| `debugverbose` | A | Global debug verbosity override (forces all debug + loglevel=DEBUG).<br/>`Usage: debugverbose <0\|1>` |
| `debugwifi` | A | Debug WiFi operations.<br/>`Usage: debugwifi <0\|1> [temp\|runtime]` |
| `debugwificonfig` | A | Debug WiFi config.<br/>`Usage: debugwificonfig <0\|1> [temp\|runtime]` |
| `debugwificonnection` | A | Debug WiFi connection.<br/>`Usage: debugwificonnection <0\|1> [temp\|runtime]` |
| `debugwifidriver` | A | Debug WiFi driver; on the P4X-EYE also shows ESP-Hosted's and the C6 console's routine log lines, which print only errors while it is off.<br/>`Usage: debugwifidriver <0\|1> [temp\|runtime] On: transport, H_SDIO_DRV, sdio_wrapper, H_API, RPC_WRAP, rpc_*, vhci_drv, os_wrapper_esp, esp_cli, esp_wifi_remote and C6> at their stock level (C6> while the console mirror runs, see c6console). Off (default): errors only. debugwifi alone does not change them; a saved esplog level wins.` |
| `debugwifiscanning` | A | Debug WiFi scanning.<br/>`Usage: debugwifiscanning <0\|1> [temp\|runtime]` |
| `esplog` | A | ESP-IDF log levels by tag, saved across reboots (loglevel covers only HardwareOne's own log).<br/>`Usage: esplog [<tag\|*> <none\|error\|warn\|info\|debug\|verbose\|default>] \| esplog clear (bare): list the saved tag levels <tag> <level>: set and save, e.g. esplog BT_BTM error, esplog rpc_utils warn <tag> default: remove the saved level (a radio stack tag follows its debug flag again at once, any other tag gets the firmware's level back at the next reboot) * <level>: every tag, the firmware's quieted ones too; tag entries go on top clear: remove every saved level` |
| `log` | A | System-wide logging to file.<br/>`Usage: log <start\|stop\|status\|autostart> start ["filepath"] [flags=0x...] [tags=0\|1]: Begin system logging filepath: Log file path, quoted (auto-generated if omitted) flags: Debug flag mask, up to 64 hex digits (bit map in System_Debug.h) tags: Prefix lines with category tags (0\|1, default 1) stop / status [json]: Stop logging / show logging status (json: one JSON object) maxsize <bytes> / rotations <0-9>: Rotate the file at this size, keeping that many old files (.1, .2, ...) autostart [on\|off]: Toggle logging auto-start on boot (bare = toggle)` |
| `loglevel` | A | Set log level (error\|warn\|info\|debug).<br/>`Usage: loglevel <error\|warn\|info\|debug>` |
| `loglink` | A | Route ESP-IDF logs through the unified output queue (stops UART interleave).<br/>`Usage: loglink [<0\|1\|on\|off>] (bare = show status)` |
| `memorysampleintervalsec` | A | Set memory sampling interval in seconds (0=disabled).<br/>`Usage: memorysampleintervalsec <0-300>` |
| `notifstats` | A | Notification pipeline counters: loss, suppression, ring lag, SSE drops.<br/>`Usage: notifstats [reset] (bare): print pipeline counters reset: zero them` |
| `outble` |  | Enable/disable BLE broadcast output.<br/>`Usage: outble <0\|1> - streams broadcast output to authenticated BLE clients` |
| `outg2` |  | Enable/disable G2 glasses output.<br/>`Usage: outg2 <0\|1> - streams CLI output to G2 glasses` |
| `settingsmodulesummary` | A | Show settings module summary. |
| `webconsole` | A | Enable/disable browser-side debug console output in the web UI.<br/>`Usage: webconsole <0\|1>` |

## settings

> The settings subsystem holds the device persisted configuration and the commands that change it. Each setting command (for example outserial, serialrequireauth, displayrequireauth, tzoffsetminutes, ntpserver, wifitxpower, webclihistorysize) sets one value and normally requests an immediate settings-file write. Because flash writes are costly, beginwrite and savesettings can coalesce changes made by the same request or transport session: the final save writes each changed settings file at most once. This is write coalescing, not a RAM transaction; unrelated sources continue to persist immediately, and their full snapshot can include live values changed while a batch is open. Idle batches expire after two minutes. savesettings remains an explicit flush-now command when no batch is open. Most commands here are admin-gated. Some changes only take effect after a reboot (for example httpsenabled is marked reboot required). The controls command emits a machine-readable JSON descriptor of a module's settable controls for UI use. Note that most subsystem settings (wifi, i2c, sensors, power, oled, bluetooth, espnow) are owned and registered by their own modules; this module hosts the cross-cutting CLI/output/auth/time settings plus the batch-write machinery.

| Command | | Description |
| ------- | :-: | ----------- |
| `beginwrite` | A | Start owner-scoped settings write coalescing until savesettings (not a RAM transaction). |
| `controls` |  | Per-module control descriptor (JSON): controls json [module]<br/>`Usage: controls json <module> (e.g. 'controls json imu'); 'controls json' lists modules` |
| `devicename` | A | Get/set the device name (ESP-NOW name, Bluetooth name, Wi-Fi hostname): 'devicename [name\|clear]'.<br/>`Usage: devicename [<name>\|clear] (1-31 letters, digits and '-'; not starting or ending with '-'; not clear/all/json/none or a MAC address; 'clear' while ESP-NOW is stopped)` |
| `displayrequireauth` | S | Require auth for display: <0\|1><br/>`Usage: displayrequireauth <0\|1>` |
| `httpautostart` | A | Auto-start HTTP server at boot and on any later WiFi join (unless closehttp was run since the last openhttp): <0\|1><br/>`Usage: httpautostart <0\|1>` |
| `httpsenabled` | A | Enable/disable HTTPS: <0\|1> (reboot required)<br/>`Usage: httpsenabled <0\|1>` |
| `ntpserver` | A | Set NTP server: <hostname><br/>`Usage: ntpserver <host>` |
| `oledclihistorysize` | A | Set OLED CLI history size: <10..100><br/>`Usage: oledclihistorysize <10..100>` |
| `outserial` | A | Set serial output: <0\|1> [persist\|temp]<br/>`Usage: outserial <0\|1> [persist\|temp]` |
| `savesettings` | A | Flush this source's changed settings files and end its coalescing scope. |
| `serialrequireauth` | S | Require auth for serial: <0\|1><br/>`Usage: serialrequireauth <0\|1>` |
| `tzoffsetminutes` | A | Set timezone offset: <-720..840><br/>`Usage: tzoffsetminutes <-720..840>` |
| `uartlink` | A | UART host link: status \| on \| off<br/>`Usage: uartlink [status\|on\|off]` |
| `uartlinkbaud` | A | Set UART link baud (0=board default)<br/>`Usage: uartlinkbaud <0\|9600-max>` |
| `uartrequireauth` | S | Require auth for UART link: <0\|1><br/>`Usage: uartrequireauth <0\|1>` |
| `webclihistorysize` | A | Set web CLI history size: <1..100><br/>`Usage: webclihistorysize <1..100>` |
| `wifiautoreconnect` | A | Keep hunting for the AP after an unexpected drop, and retry a failed boot connect (first hunt 60 s later, then 60 s, 120 s, every 300 s): <0\|1><br/>`Usage: wifiautoreconnect <0\|1> Separate from wifiautostart: this is about recovering a dropped link, not connecting at boot.` |
| `wifiexternalantenna` | A | Use the external u.FL antenna instead of the on-board one (boards with an RF switch, the FeatherS3[D]; reboot required): [0\|1]<br/>`Usage: wifiexternalantenna [0\|1] No argument shows the saved choice. Takes effect at the next boot.` |
| `wifitxpower` | A | Set WiFi TX power: <dBm><br/>`Usage: wifitxpower <2..21>` |

## sensorlog

> Periodically samples the onboard sensors and appends readings to a file, driven by the single multiplexed sensorlog <subcommand> command. sensorlog start <filepath> [interval_ms] begins logging (default 5000 ms; the filepath must start with / and parent directories are created automatically) and sensorlog stop ends it; only one log can run at a time, so start refuses if logging is already active. sensorlog status reports the active file, interval, format, rotation settings, selected sensors, and last-write age. Configure behavior with format <text|csv|track> (track is a compact GPS-only format with signal-loss dedup), maxsize and rotations for log rotation, and sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none> to choose which sensors are recorded. sensorlog interval <ms> sets the poll period (100-3600000, default 5000). NOTE: format track additionally REPLACES the sensor mask with GPS-only and persists it, so a prior selection is lost — and rotations 0 deletes the active file at the size cap rather than pruning older generations. sensorlog autostart [on|off] makes logging resume on the next boot using the last-used parameters; the format/maxsize/rotations/sensors/autostart choices are persisted.

| Command | | Description |
| ------- | :-: | ----------- |
| `capturecrypt` | A | Capture at-rest encryption: status, mode (off/health/all), plaintext export<br/>`Usage: capturecrypt [status\|off\|health\|all\|export "<in>" "<out>"] Sealed sessions write '#HW1ENC' on line 1 + per-row ciphertext; filenames don't change. health: seal sessions that include the R1 ring (default). all: every capture session. Mode changes apply at the next session or day rollover — a single file is never mixed-mode. Viewers (fileview, web view, G2/OLED) decrypt for authorized users; raw downloads, fileread and ESP-NOW transfers ship sealed bytes. export: write a decrypted copy (inside /logging_captures) for sharing.` |
| `sensorlog` |  | Sensor data logging: start, stop, status, format, maxsize, rotations, sensors<br/>`Usage: sensorlog <start\|stop\|status\|format\|maxsize\|rotations\|sensors\|interval\|autostart> [args...] start <filepath> [interval_ms]: Begin logging (default 5000ms) stop: Stop logging status [json]: Show current logging status (json: one JSON object) format <text\|csv\|track>: Set log format (default: text) track = GPS-only compact format with signal loss dedup; NOTE: also OVERWRITES the sensor mask to GPS-only and persists it maxsize <bytes>: Set max file size before rotation (default: 256000) rotations <count>: Old generations to keep (1-9). 0 = DELETE the active file at the size cap, losing the current day (default: 3) sensors <thermal\|tof\|imu\|gamepad\|apds\|gps\|presence\|r1\|all\|none>: Select sensors to log sensors toggle <sensor>: Toggle one sensor in the current selection interval <ms>: Set poll interval 100-3600000 (default 5000) autostart [on\|off]: Auto-start logging on boot (bare = toggle; admin)` |
| `sensorlog autostart` | A | Auto-start sensor logging on boot with the last-used parameters (saved).<br/>`Usage: sensorlog autostart [on\|off] (bare = toggle)` |

## health

> HardwareOne's own CSV logger for R1 ring vitals. healthlogging on|off|toggle drives it independently of the ring's own health-collection privacy setting, healthlogging status shows it, and healthlogmerge stitches captures together. The ring's live vitals, refresh and history are the ring module (ring health).

| Command | | Description |
| ------- | :-: | ----------- |
| `healthlogging` |  | Start/stop local R1 health logging (independent of ring collection)<br/>`Usage: healthlogging <on\|off\|toggle\|status\|interval [sec]> on: enable LOG_R1, force format=CSV, start under /logging_captures/sensors/, persist for boot (one dated per-day file when the clock is set, boot-<N>/ until sync then roll) off: remove LOG_R1; stop logging if no other sensors remain interval <sec>: legacy POINT mining interval (default 900 = 15 min) On 2.2.9, logging is passive/on-demand and records Poll Now refreshes; it sends no timed POINT query R1-only sessions write only on an admitted mine/refresh — no 5s empty heartbeats This does not change the ring's health-collection privacy setting.` |
| `healthlogmerge` | A | Byte-concatenate sensor logs in the given order: "<out>" "<in1>" "<in2>" ...<br/>`Usage: healthlogmerge "<output>" "<in1>" "<in2>" [...] OUTPUT FIRST — arg 0 is TRUNCATED. Inputs follow, in the order you want them. Bare output name → /logging_captures/sensors/; INPUTS need a full path. Extensionless output gets .csv appended even for TEXT inputs. Concatenation is byte-exact: CSV inputs keep their header lines mid-file, rows are NOT time-ordered, and mixing formats/sensor masks yields an unparseable result. Max 8 inputs (arg limit).` |

## users

> The users subsystem provides admin-gated account management, authentication, sessions, and bans. Accounts have two roles, admin and standard; the first account is created as the owner with the superadmin role, and userpromote/userdemote change roles while useradd creates an account directly (optionally forcing a password change on first login). New accounts can also come through an approval flow: userrequest files a pending request that an admin clears with userapprove or rejects with userdeny (pendinglist shows the queue). login and logout authenticate per transport (serial, display, bluetooth, g2), userlist enumerates accounts, and the password commands cover both self-service (userchangepassword) and admin reset (userresetpassword). Sessions are tracked per transport: sessionlist shows active sessions and sessionrevoke force-logs-out a session by SID or by username. Two independent ban mechanisms exist: ban/unban/banlist block an IP address, while banuser/unbanuser suspend a user account so it cannot log in until unbanned; the primary admin account cannot be banned. usersync pushes a user credentials to another device over ESP-NOW, authenticated by an admin account on the receiving device.

| Command | | Description |
| ------- | :-: | ----------- |
| `ban` | A | Permanently ban an IP: <ip> [reason]<br/>`Usage: ban <ip> [reason] Blocks all access from the IP until manually unbanned.` |
| `banlist` | A | List all banned IPs. (add 'json' for JSON output) |
| `banuser` | S | Permanently ban a user account: <username> [reason]<br/>`Usage: banuser <username> [reason] Prevents the account from logging in until manually unbanned.` |
| `login` |  | Login this interface, or target another session after signing in: <user> <pass> [serial\|uart\|display]<br/>`Usage: login <username> <password> [serial\|uart\|display] Bare login always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session.` |
| `logout` |  | Logout this interface, or target another session after signing in: [serial\|uart\|display]<br/>`Usage: logout [serial\|uart\|display] Bare logout always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session.` |
| `pendinglist` | A | List pending user requests. (add 'json' for JSON output) |
| `sessionlist` | A | List active sessions. (add 'json' for JSON output) |
| `sessionrevoke` | A | Revoke session: <sid\|user> [reason]<br/>`Usage: sessionrevoke sid <sid> [reason] sessionrevoke user <username> [reason]` |
| `unban` | A | Remove an IP ban: <ip><br/>`Usage: unban <ip>` |
| `unbanuser` | A | Remove a user account ban: <username><br/>`Usage: unbanuser <username>` |
| `useradd` | A | Create user: <username> <password> [0\|1] [role]<br/>`Usage: useradd <username> <password> [0\|1] [guest\|user\|admin\|superadmin] 0\|1: 1 = require a new password on next login (default 0) role: defaults to user; you cannot grant a role above your own The two optional tokens may appear in either order` |
| `userapprove` | A | Approve pending request: <username><br/>`Usage: userapprove <username>` |
| `userchangepassword` |  | Change own password: <currentPass> <newPass> <confirmPass><br/>`Usage: userchangepassword <currentPassword> <newPassword> <confirmPassword>` |
| `userdelete` | S | Delete user: <username><br/>`Usage: userdelete <username>` |
| `userdemote` | S | Lower a user's role: <username> [admin\|user\|guest]<br/>`Usage: userdemote <username> [admin\|user\|guest] (super admin; default user)` |
| `userdeny` | A | Deny pending request: <username><br/>`Usage: userdeny <username>` |
| `userlist` | A | List all users. (add 'json' for JSON output) |
| `userpromote` | A | Promote a user: <username> [user\|admin\|superadmin]<br/>`Usage: userpromote <username> [user\|admin\|superadmin] (default admin; granting superadmin requires a super-admin caller)` |
| `userrequest` |  | Request account: <user> <pass> [confirm]<br/>`Usage: userrequest <username> <password> [confirmPassword]` |
| `userresetpassword` | S | Reset user password: <username> <newPassword> [0\|1]<br/>`Usage: userresetpassword <username> <newPassword> [0\|1] Optional: 1 = require password change on next login` |
| `usersync` | A | Sync a user to another device over ESP-NOW. (async; result only on the target device - check its userlist)<br/>`Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass> Returns OK on delivery; the user is created on the TARGET device (no confirmation returns here) - verify on that device's userlist. targetAdminUser/targetAdminPass = an admin account on the RECEIVING device (validated there). yourAdminPass = your admin password on THIS device; userPass = the synced user's password.` |
| `whoami` |  | Show the identity of the submitting interface.<br/>`Usage: whoami` |

## features

> The features subsystem lists the compiled-in capabilities and their memory cost; it changes nothing. features with no argument lists every feature grouped by category (Network, Display, Sensors, System) with an approximate heap estimate and four states: built in, running now, start at boot and allowed (- when the feature has no such switch or state; only the I2C bus has an allowed switch, i2cbusenabled, and takes it at the next restart). features <id> shows one feature and names the commands that switch it: open<x>/close<x> start or stop it now and <x>autostart sets start at boot (a few use other names, such as eicontinuous, llmload and automation system enable|disable). A few rows are fixed when the firmware is built. features <id> <on|off> is refused with those command names. features json is the capability list for the app. featuresetup launches an interactive, admin-only wizard that sets which features start at boot, and works from any CLI transport.

| Command | | Description |
| ------- | :-: | ----------- |
| `features` | A | List system features: built in, running now, start at boot, allowed. |
| `featuresetup` | A | Run the interactive feature configuration wizard. |

## image

> Captures stills from the camera and manages the saved photo library. capture grabs a frame and saves it as a JPG (target storage chosen by argument: littlefs/lfs, sd, or both; default follows the cameraStorageLocation setting), and requires a camera sensor that is both compiled in and enabled -- on boards with no camera the capture simply fails. images lists saved photos with sizes and storage stats (add sd to list the card, json for app/BLE output); imagedelete "<path>" removes one (path must be quoted). imagesend transmits a photo to another device over ESP-NOW: imagesend <device> "<path>" resolves the device by name or MAC and sends the named file (path required).

| Command | | Description |
| ------- | :-: | ----------- |
| `capture` |  | Capture and save image: capture [littlefs\|sd\|both]<br/>`Usage: capture [littlefs\|lfs\|sd\|both]` |
| `imagedelete` | A | Delete image: imagedelete "<path>"<br/>`Usage: imagedelete "<path>"` |
| `images` |  | List saved images: images [littlefs\|sd]<br/>`Usage: images [sd] [json]` |
| `imagesend` | A | Send image via ESP-NOW: imagesend <device> "<path>" (synchronous send; fails if the receiver rejects/cancels)<br/>`Usage: imagesend <device> "<path>" Blocks until the image is sent; it is written to the peer's /espnow/received/ inbox. An error result means the send aborted or the receiver cancelled.` |

## map

> On-device offline map subsystem backed by region map files stored under /maps/ (custom HWMap tile format). A map must be loaded before any lookup works: mapload "<path>" loads a file into PSRAM, maplist shows what is available, map prints the current map region/feature-count/bounds (add json for structured output), and mapunload frees the PSRAM and tile cache. search <name> finds named features in the loaded map, while whereami reports the nearest road and area for the current GPS position and therefore needs both a loaded map and a live GPS fix. Waypoints are persistent user markers managed through waypoint (list/add/del/goto/clear/clearall/rename/notes) and can have files attached via waypointfile/waypointfiles; gpstrack loads, inspects, or clears a recorded GPS breadcrumb track (and rejects tracks that fall outside the loaded map bounds), and maporganize sorts loose files in /maps into subdirectories.

| Command | | Description |
| ------- | :-: | ----------- |
| `gpstrack` |  | Manage GPS tracks: status, load, clear, live recording<br/>`Usage: gpstrack [status\|load <filepath>\|clear\|live <start\|stop\|toggle>]` |
| `gpstrackmerge` | A | Stitch GPS logs in order: "<out>" "<in1>" "<in2>" ...<br/>`Usage: gpstrackmerge "<output>" "<in1>" "<in2>" [...] (output first, then inputs in stitch order; max 9 inputs)` |
| `map` |  | Show current map info (add 'json' for JSON output) |
| `maplist` |  | List available maps (add 'json' for JSON output) |
| `mapload` |  | Load map file: "<path>"<br/>`Usage: mapload "<path>"` |
| `maporganize` | A | Organize map files in /maps into subdirectories |
| `mapunload` |  | Unload current map (free PSRAM on device) |
| `search` |  | Search map features: <name><br/>`Usage: search <name>` |
| `waypoint` |  | Manage waypoints: <list\|add\|del\|goto\|clear\|clearall\|rename\|notes><br/>`Usage: waypoint [list\|add <lat> <lon> [name]\|del <index>\|goto <index>\|clear\|clearall\|rename <index> <name>\|notes <index> <notes>]` |
| `waypointfile` |  | Link file to waypoint: "<file>" <wpName><br/>`Usage: waypointfile "<file>" <wpName> \| waypointfile "<file>" <lat> <lon> [wpName]` |
| `waypointfiles` |  | Waypoint files: <name> [del <idx>]<br/>`Usage: waypointfiles <wpName> [del <index>]` |
| `whereami` |  | Show current location context (add 'json' for JSON output) |

## mapsettings

> Persisted rendering defaults for the maps app, stored under apps.maps and applied to the live map at boot. mapzoom <0.5..20.0> sets the initial zoom, maplayers <0..1023> sets a bitmask controlling which feature layers are drawn, and mapcachekb <256..4096> sizes the tile LRU cache pool. The zoom and layers setters also mirror immediately into the running renderer so changes take effect without a reboot, but the cache size only re-applies on the next map load (or reboot). All three are admin-only and, run from the CLI, write to flash immediately so they survive a reboot.

| Command | | Description |
| ------- | :-: | ----------- |
| `mapcachekb` | A | Set tile cache size in KB (effective on next map load)<br/>`Usage: mapcachekb <256..4096>` |
| `maplayers` | A | Set visible layer bitmask: <0..1023><br/>`Usage: maplayers <bitmask 0..1023>` |
| `mapzoom` | A | Set default map zoom: <0.5..20.0><br/>`Usage: mapzoom <0.5..20.0>` |

## power

> The power subsystem manages CPU frequency and battery-oriented power saving. The main command is power: power alone prints the current mode, CPU clock and display brightness; power mode <perf|balanced|adaptive|ultra|locked|0-4> selects one of five target-specific presets: Performance, Balanced, Adaptive (the default), UltraSaver, or Locked. Run power or power json to see their active and idle MHz. A mode sets the clock while the device is in use and while idle, plus the display brightness; the chosen mode is persisted. Every mode runs transcription, camera and recording at the maximum clock while they are busy and drops back when they finish; Adaptive stays at the floor clock otherwise. Locked keeps the maximum clock through idle. UltraSaver uses its lower idle-only clock once the local display blanks; input or a command restores the target's interactive floor. A command from the glasses, phone, web or serial keeps the clock up but does not light the local display; only local input, the display's own commands and on-device voice do. Nothing switches the mode on battery level in this firmware: power auto and power threshold report that automatic mode is not available. Two related idle controls are separate commands: powersleep enters display idle immediately without changing the timeout; powersave <0..1440> sets an idle timeout (minutes; 0 disables) after which the OLED blanks and the CPU may downclock (mode-dependent) while the radio stays up so the device remains reachable, and powercooldown <0..60000> sets an anti-flap cooldown (milliseconds) that prevents rapid back-to-back sleep transitions. All of these values persist.

| Command | | Description |
| ------- | :-: | ----------- |
| `power` | A | Power management [mode]<br/>`Usage: power - show current power status power json - status as one JSON blob (same schema as /api/power/status) power mode <perf\|balanced\|adaptive\|ultra\|locked\|0-4>` |
| `powercooldown` | A | Sleep transition cooldown (ms; 0 disables)<br/>`Usage: powercooldown <0..60000>` |
| `powersave` | A | Idle power-save: OLED off + optional downclock (0 disables)<br/>`Usage: powersave <0..1440>` |
| `powersleep` | A | Enter display idle now; local input wakes it<br/>`Usage: powersleep` |

## liveaudio

> Exercises the live-pcm-v1 UART framing and bounded receiver path. A real authenticated UART host first acquires a renewable 3-second controller lease with liveaudio ready. liveaudio synth schedules deterministic 16 kHz signed-16-bit mono PCM; an explicit, exact-ID liveaudio shadow arm can instead tee an owned 16 kHz PDM or G2 recording through a fixed 16 KiB PSRAM queue. Shadow transport is disabled by default, never delays or replaces the WAV writer, and a transport fault aborts only the live copy while the finalized WAV remains authoritative. This diagnostic transport does not enable production streaming STT, LLM, or lens delivery.

| Command | | Description |
| ------- | :-: | ----------- |
| `liveaudio` |  | Opt-in live PCM transport, lease, and shadow diagnostics<br/>`Usage: liveaudio <capabilities\|status\|ready 1 <controller_hex16>\|conversate 1 <controller_hex16> on\|off\|shadow 1 <controller_hex16> on <exchange_hex16\|native>\|shadow 1 <controller_hex16> off\|release 1 <controller_hex16>\|synth 1 <controller_hex16> <exchange_hex16> <duration_ms>\|abort 1 <controller_hex16> <exchange_hex16>>` |

_`liveAudioCommands` has `#if` alternatives in `System_LiveAudio.cpp`; this table is the first branch. Other build configurations register a different version of these rows (for example the disabled branch of the feature)._

## cm5

> Everything that talks to the CM5 Linux host over the authenticated UART link. cm5 status exposes the current named-UART epoch binding, freshness, state, command bridge, monitor transitions, and task stack watermark; cm5 capabilities reports the heartbeat protocol constants. The five-second heartbeat itself is authenticated UART control-plane traffic, not a user command, so it remains responsive even while the shared command executor is occupied. cm5 power and cm5 fan drive the host through two independent finite request/ACK/report state machines that share one ID space: cm5 power status requests fresh CM5 state, cm5 power profile <eco|balanced|performance|auto> requests a power profile, and cm5 fan <quiet|max> pins a fan mode while cm5 fan auto returns control to the Linux temperature curve. Bare cm5 power / cm5 fan (or their show form) display local delivery state and the last CM5 report. Initiation requires an admin; reboot, halt, suspend, and sleep_for <1..1440 minutes> additionally require superadmin plus a literal same-command confirm token, and recover confirm clears only an inspected fail-closed transition. ACK/report callbacks are accepted from a real authenticated UART session only, which lets the CM5 service account stay user-tier. One request per protocol may be pending at a time and delivery retries are finite; destructive execution additionally requires confirmed accepted and committed ACK phases, while a normalized Linux boot ID distinguishes a daemon restart from a completed host boot. A max fan request may supersede one pending non-max request.

| Command | | Description |
| ------- | :-: | ----------- |
| `cm5` |  | Inspect CM5 service presence, link health, and host power/fan control.<br/>`Usage: cm5 <status\|capabilities\|linkhealth> (heartbeat is UART control-plane only)` |
| `cm5 capabilities` |  | Show the CM5 presence protocol capabilities.<br/>`Usage: cm5 capabilities` |
| `cm5 fan` | A | Inspect or request CM5 fan mode/readback.<br/>`Usage: cm5 fan [show\|status\|quiet\|auto\|max]` |
| `cm5 fan ack` |  | Accept a CM5 fan ACK (authenticated UART session only).<br/>`Usage: cm5 fan ack 1 <16-hex-id> <accepted\|applied\|failed>` |
| `cm5 fan report` |  | Accept bounded CM5 fan readback (authenticated UART session only).<br/>`Usage: cm5 fan report 1 <id> <requested-mode> <effective-mode> <temp-mc\|-1> <target-pwm> <pwm> <rpm\|-1> <health>` |
| `cm5 linkhealth` |  | Show the CM5 host's UART link fault tally.<br/>`Usage: cm5 linkhealth [json]` |
| `cm5 power` | A | Inspect or request CM5 host power/profile state.<br/>`Usage: cm5 power [show\|status\|profile <eco\|balanced\|performance\|auto>]` |
| `cm5 power ack` |  | Accept a CM5 delivery/application ACK (UART session only).<br/>`Usage: cm5 power ack 1 <16-hex-id> <accepted\|committed\|applied\|failed>` |
| `cm5 power halt` | S | Request a confirmed CM5 halt.<br/>`Usage: cm5 power halt confirm` |
| `cm5 power reboot` | S | Request a confirmed CM5 reboot.<br/>`Usage: cm5 power reboot confirm` |
| `cm5 power recover` | S | Clear a fail-closed uncertain CM5 transition after inspection.<br/>`Usage: cm5 power recover confirm` |
| `cm5 power report` |  | Accept finite CM5 power state/profile readback (UART session only).<br/>`Usage: cm5 power report 1 <16-hex-id\|0> <state> <profile> <32-hex-linux-boot-id>` |
| `cm5 power sleep_for` | S | Request confirmed CM5 timed sleep in bounded minutes.<br/>`Usage: cm5 power sleep_for <1..1440 minutes> confirm` |
| `cm5 power suspend` | S | Request confirmed CM5 system suspend (host may reject it).<br/>`Usage: cm5 power suspend confirm` |
| `cm5 status` |  | Inspect the setup-agnostic CM5 service-presence lease.<br/>`Usage: cm5 status` |

## ota

> Native ESP-IDF signed OTA support. otastatus reports the journal, partition identity, staged pair, and last result. A superadmin sets a persistent recovery credential with otapin - on the physical serial console only, because it decides whether recovery can be reached at all - then uploads /system/ota/candidate.part and manifest.part, validates and journals them with otastage confirm, then uses otaupdate confirm to reboot into the immutable factory recovery updater. otarecovery confirm enters the same authenticated recovery image for a direct upload when the main filesystem cannot stage an image; its explicit allow-downgrade option is required for older signed releases. otacancel replaces a staged request safely, and otaack <result-sequence> confirm acknowledges exactly the durable result that was reviewed. Mutating OTA commands are forbidden from automations and require the opt-in 16 MB OTA partition layout.

| Command | | Description |
| ------- | :-: | ----------- |
| `otaack` | A | Acknowledge the durable OTA result after reviewing it.<br/>`Usage: otaack <result-sequence> confirm` |
| `otacancel` | A | Cancel a staged request before recovery boot is armed.<br/>`Usage: otacancel confirm` |
| `otapin` | A | Set the persistent recovery WPA2/HTTP credential (serial console only).<br/>`Usage: otapin <12..63 printable characters> \| otapin clear confirm` |
| `otarecovery` | A | Reboot into authenticated recovery for direct upload.<br/>`Usage: otarecovery confirm [allow-downgrade]` |
| `otaresetjournal` | A | Serial-only repair of the two OTA transaction keys.<br/>`Usage: otaresetjournal confirm` |
| `otastage` | A | Validate and journal uploaded candidate.part + manifest.part.<br/>`Usage: otastage confirm [allow-downgrade]` |
| `otastatus` |  | Show signed recovery OTA state. (add 'json')<br/>`Usage: otastatus [json]` |
| `otaupdate` | A | Revalidate staged firmware and reboot into recovery apply.<br/>`Usage: otaupdate confirm [force-power]` |
| `otawrite` | A | Stage exact OTA members over encrypted Bluetooth.<br/>`Usage: otawrite begin <candidate\|manifest> <size> <sha256> \| status \| finish \| abort` |

_`otaCommands` has `#if` alternatives in `System_OTA.cpp`; this table is the first branch. Other build configurations register a different version of these rows (for example the disabled branch of the feature)._

## setpattern

> Provides the single admin-only command setgamepadpassword, which opens the gamepad-pattern password setup flow on the OLED screen. A pattern is a sequence of joystick directions that is hashed and stored as the logged-in user password, usable for on-device login. You must already be logged in at the OLED display first (the command errors otherwise); the guided on-screen flow then re-authenticates you, prompts you to enter the new pattern and confirm it, and saves it to your account. This command only launches the OLED mode -- the actual entry and confirmation happen on the device screen.

| Command | | Description |
| ------- | :-: | ----------- |
| `setgamepadpassword` | A | Set gamepad joystick password (requires an active OLED-display login). |

## even_g2

> This subsystem drives Even Reality G2 smart glasses in the Glasses mode (blemode client or glasses: the glasses, plus the phone link whenever a phone secret is set, with the app console on that link); the two temples are addressed as left/right/auto. openg2 starts scan-and-connect IN THE BACKGROUND and returns immediately -- it does not block, so poll g2status (or g2info for firmware/MAC/battery) to see when the link is up, and nearly every other command here requires that connection first. Display commands render to the lens: g2show prints text, g2ai/g2ainoask/g2aidirect push a front-pane AI answer card through the EvenAI pipeline, g2bmp shows a BMP file, and g2sensors/g2network/g2files/g2settingspage show built-in info pages; g2nav [on|off] enables menu-navigation mode and g2clear blanks the display. For audio, use the shared Microphone controls: micsource selects auto, pdm or g2; openmic/closemic start and stop the selected source; micrecord controls WAV recording. g2micon/g2micoff are low-level stream probes; g2micrec and g2micwav remain SD-backed raw-LC3 and decoded-WAV diagnostics. closeg2 disconnects but keeps the GATT cache for fast reconnect; closeg2 full also frees the cache to recover about 30 KB. The glasses' own stock widget can show system metrics instead of share prices: g2 stats status and g2 stats preview show the rows and bytes; g2 stats send (admin) sends one batch; g2 stats feed [on|off] (admin, saved, off by default) feeds it on change, g2 stats metrics <labels> and g2 stats interval <seconds> (admin, saved) choose what and how often; the chart is the last 185 points (kept while the glasses are away), one every g2 stats step [10|20|60] seconds (admin, saved; default 20, about an hour; a change starts the chart over), and g2 stats pad [gap|flat] (admin, saved) fills its left part until the window is full. The glasses' QuickList can carry Wear Detection, Silent Mode, Auto Brightness, Head-Up Display and Brightness rows (firmware 2.3.2.14): g2 list status shows them; g2 list feed [on|off] (admin, saved, off by default) sends them after each connect, g2 list push (admin) sends them now, each time replacing the glasses' whole list (items added in the Even app are removed), g2 list preview (admin) shows their bytes; ticking a row and leaving the list switches that setting (Brightness turns auto brightness off and steps through 25, 50, 75 and 100 percent). The remaining g2* commands are low-level protocol probes and diagnostics (g2probe, g2protostats, g2devcfg, g2dumpframes).

| Command | | Description |
| ------- | :-: | ----------- |
| `closeg2` | A | Close the G2 glasses: drop both links and release the glasses runtime (ring, phone and Bluetooth stay up). |
| `g2 list feed` | S | Replace the glasses' QuickList with Wear Detection, Silent Mode, Auto Brightness, Head-Up Display and Brightness rows after each connect (saved; off by default).<br/>`Usage: g2 list feed [on\|off]` |
| `g2 list preview` | A | Show the next QuickList rows' bytes without sending.<br/>`Usage: g2 list preview` |
| `g2 list push` | S | Read the glasses' settings and replace their QuickList with the five control rows now (items added in the Even app are removed).<br/>`Usage: g2 list push (both temples, glasses firmware 2.3.2.14)` |
| `g2 list status` |  | QuickList rows (wear detection, silent mode, auto brightness, head-up display, brightness): feed, row text, ticks and writes. |
| `g2 stats feed` | A | Feed the stock widget with system metrics (saved; off by default).<br/>`Usage: g2 stats feed [on\|off] (off clears the widget)` |
| `g2 stats interval` | A | How often the stock widget feed may send changed values (saved).<br/>`Usage: g2 stats interval [seconds 30-3600] (default 60; 10 s while the stock page is open)` |
| `g2 stats metrics` | A | Which metrics the stock widget shows, in order (saved).<br/>`Usage: g2 stats metrics [label,label,...\|default] (up to 9 of CPU CPU0 CPU1 HEAP HBLK PSRM SD LFS TEMP LINK MESH RING CLK; default CPU,HEAP,PSRM,SD,LFS,CLK,LINK,MESH,RING)` |
| `g2 stats pad` | A | How the stock chart's left part is filled before its 185 points exist (saved).<br/>`Usage: g2 stats pad [gap\|flat] (default gap: empty; flat: the oldest point repeated)` |
| `g2 stats preview` |  | Show the stock rows and the first message's bytes without sending.<br/>`Usage: g2 stats preview` |
| `g2 stats send` | A | Send one batch of the saved metrics to the stock widget now.<br/>`Usage: g2 stats send (right temple, glasses firmware 2.3.2.14)` |
| `g2 stats status` |  | Stock widget feed: state, metrics shown now, sends and acks. |
| `g2 stats step` | A | Seconds between stock chart points; the chart is 185 points (saved; a change starts it over).<br/>`Usage: g2 stats step [10\|20\|60] (default 20: about 1 h; 10: 31 min; 60: 3 h 5 min)` |
| `g2ai` |  | Front-pane AI card (full pipeline): g2ai <text><br/>`Usage: g2ai <text>` |
| `g2aiconfig` |  | Send EvenAI CONFIG (cmd=10): g2aiconfig [voiceSwitch] [streamSpeed] [duplexMode]<br/>`Usage: g2aiconfig [voiceSwitch] [streamSpeed] [duplexMode] (defaults: 0 80 0; use - to omit a field)` |
| `g2aidirect` |  | Variant: CTRL+REPLY only: g2aidirect <text><br/>`Usage: g2aidirect <text>` |
| `g2aih` |  | Front-pane card with custom heading: g2aih <heading>\|<body><br/>`Usage: g2aih <heading>\|<body> (no \| = whole text as body)` |
| `g2ainoask` |  | Variant: skip ASK step: g2ainoask <text><br/>`Usage: g2ainoask <text>` |
| `g2battery` |  | Query G2 battery % on connected temples |
| `g2bmp` |  | Display BMP: g2bmp </path.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]<br/>`Usage: g2bmp </path/to/file.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]` |
| `g2clear` |  | Clear G2 display |
| `g2connpri` | A | Probe the BLE conn-interval admission boundary: g2connpri [<min> <max>\|default] (bare = report)<br/>`Usage: g2connpri [<min> <max>\|default] (ticks of 1.25 ms; runtime only; bare = report)` |
| `g2control` | A | Persist/reconcile G2 device policy without overwriting official-app state by default<br/>`Usage: g2control [<headup\|notifications> <preserve\|off\|on>] (bare = status; work is queued to g2 control owner)` |
| `g2conversate` |  | Native Conversate mic keepalive (no STT or file)<br/>`Usage: g2conversate on \| test [seconds 10..3600, default 300] \| status \| stop \| off` |
| `g2devcfg` |  | Typed sid=0x80 sender: g2devcfg <heartbeat\|auth\|role\|time\|ring> [args]<br/>`Usage: g2devcfg <heartbeat\|auth\|role <both\|right\|left>\|time [tzQuarterHours]\|ring <mac> <name>>` |
| `g2dumpframes` |  | Print the recent G2 envelope ring buffer |
| `g2envgap` | A | Image inter-envelope gap override: g2envgap [<5..500>\|auto] (bare = report)<br/>`Usage: g2envgap [<5..500>\|auto] (runtime only; bare = report)` |
| `g2evenai` |  | Push into the matching live Hey-Even exchange (strict 16-hex ID required)<br/>`Usage: g2evenai <askid\|replyid\|replypartid\|replyendid\|exitid> <16hex-id> [text] status: show active exchange ID/arm/generation capabilities: show guarded command contract replypartid: stream one delta (leading glue preserved) legacy ask/reply/replypart/replyend/exit fail closed; use g2ai for deliberate bench cards` |
| `g2files` |  | Show Files browser page on the G2 lens |
| `g2glasses` | A | Change glasses-device settings (including capture-proven HeadUp on fw 2.2.6.10 and 2.3.2.14)<br/>`Usage: g2glasses [show\|refresh\|help] \| g2glasses <brightness\|autobright\|weardetect\|x\|y\|headup\|headangle\|silent\|unitformat\|distunit\|timeformat\|dateformat\|tempunit> [value]` |
| `g2health` |  | Open the Health (R1 vitals + graphs) app on the G2 lens<br/>`Usage: g2health (Overview/HR/HRV/SpO2/Battery on the lens; Back to exit)` |
| `g2hijacktest` |  | Simulate a Blocks tap (status-page hijack) |
| `g2hold` |  | Long-press (hold) input: firmware profile, pairing, routes and last 16 hold records<br/>`Usage: g2hold [status\|clear\|sim <press [ring\|right\|left\|unknown]\|release\|abort>] bare = status; clear empties the record ring and counters sim (admin; press only by the paired wearer): synthetic right-arm hold through the real pairing and dispatcher (default source ring)` |
| `g2imgprobe` |  | Probe Cmd=3 multi-frag wire path: g2imgprobe [size_bytes]<br/>`Usage: g2imgprobe [size_bytes] (1..4096, default 1024)` |
| `g2info` |  | Dump device info (firmware, MAC, battery, etc.) |
| `g2listrebuild` |  | REBUILD-list on swap when pure list + same row count [on\|off] (default ON)<br/>`Usage: g2listrebuild [<on\|off>] (bare = report state)` |
| `g2liveloop` |  | Q13/Q14 lens-idle keep-alive: g2liveloop keep [on\|off] (default off → break on lens timeout)<br/>`Usage: g2liveloop keep [<on\|off>] (bare = report state)` |
| `g2liverate` |  | Get/set live-update probe cadence (ms), default 600: g2liverate [N]<br/>`Usage: g2liverate [ms>=100] (bare = report)` |
| `g2map` |  | Render the offline map on the G2 lens (288x144)<br/>`Usage: g2map (renders the current map view; double-tap the lens to dismiss)` |
| `g2micoff` |  | G2 mic probe: AudioCtrCmd{en=0} (stop stream) |
| `g2micon` |  | G2 mic probe: AudioCtrCmd{en=1} on LEFT (or 'r' for RIGHT)<br/>`Usage: g2micon [l\|r] (default LEFT)` |
| `g2micrec` |  | G2 mic dump: g2micrec start ["path"] \| stop \| status — writes raw 205B LC3 packets to SD<br/>`Usage: g2micrec start ["path"] \| stop \| status (bare = status)` |
| `g2micreset` |  | G2 mic probe: zero per-arm counters |
| `g2micstats` |  | G2 mic probe: dump per-arm frame counters |
| `g2micverbose` |  | G2 mic probe: per-frame log [on\|off]<br/>`Usage: g2micverbose [<on\|off>] (bare = toggle)` |
| `g2micwav` |  | G2 mic decode: g2micwav start ["path"] \| stop \| status — decodes LC3 → 16k mono WAV on SD<br/>`Usage: g2micwav start ["path"] \| stop \| status (bare = status)` |
| `g2nativeconfig` | A | Captured G2 config: selftest, HeadUp-adjacent dashboard/menu/notification replay<br/>`Usage: g2nativeconfig <selftest\|dashboard july31\|notification july31\|menu <id,id,...>> (writes require exact fw 2.2.6.10)` |
| `g2nativenotify` | A | Native EFS notification card (real overlay, admin): g2nativenotify selftest \| <title>\|<body><br/>`Usage: g2nativenotify selftest \| <title>\|<body> \| <name>\|<title>\|<body>` |
| `g2nav` |  | Menu navigation mode: g2nav [on\|off\|toggle] (bare = report state)<br/>`Usage: g2nav [on\|off\|toggle] (bare = report state)` |
| `g2network` |  | Show Network info page on the G2 lens |
| `g2notifenable` | A | Prime native notifications on sid 0x04 (enable + whitelist-disable) before g2nativenotify<br/>`Usage: g2notifenable (sends NOTIF_CTRL enable + WHITELIST_CTRL disable to the right arm)` |
| `g2notify` |  | Transient text (placeholder): g2notify [secs] <text><br/>`Usage: g2notify [<seconds>] <text> (seconds 1..599, default 5)` |
| `g2packrate` |  | SD-pack animation cadence: g2packrate [<ms>] (range 20..2000, default 80)<br/>`Usage: g2packrate [<ms>] (20..2000; bare = report)` |
| `g2probe` |  | Fire arbitrary pb cmd on non-mutation sids: g2probe <sid_hex> <cmd_dec> [body_hex] [R\|L]<br/>`Usage: g2probe <sid_hex> <cmd_dec> [body_hex] [R\|L] (R/L forces a temple; sids 01/03/04/09/80 blocked)` |
| `g2protostats` |  | Show G2 protocol stats per sid: g2protostats [verbose]<br/>`Usage: g2protostats [verbose]` |
| `g2recover` |  | Try to reconnect a missing G2 temple without tearing down the connected one |
| `g2reopen` |  | Re-open the hijacked Blocks app after an abnormal exit |
| `g2scan` |  | Scan for G2 glasses |
| `g2sensors` |  | Show device's sensor list on the G2 lens |
| `g2settings` |  | Settings debug: g2settings verbose [on\|off]<br/>`Usage: g2settings verbose [<on\|off>] (bare verbose = toggle)` |
| `g2settingspage` |  | Show Settings inspector page on the G2 lens |
| `g2show` |  | Display text: g2show <text><br/>`Usage: g2show <text>` |
| `g2status` |  | Show G2 connection status |
| `g2streamres` |  | Lens stream resolution: g2streamres [<W>x<H>] (bare = report; e.g., 96x96, 160x120, 288x144)<br/>`Usage: g2streamres [<W>x<H>] (W 16..288, H 16..144; bare = report)` |
| `g2streamtonemap` |  | Lens stream 4-bpp tone: 0=Linear 1=Balanced 2=Shadows 3=Legacy (bare = report)<br/>`Usage: g2streamtonemap [<0\|1\|2\|3\|linear\|balanced\|shadows\|legacy\|off\|on>] (bare = report state)` |
| `g2terminal` |  | Native Terminal voice/chat (firmware 2.3.2.14)<br/>`Usage: g2terminal open \| status \| close. Browse Mingus host sessions and recent history; no automatic submission.` |
| `g2verbose` |  | Scan-verbose logging: g2verbose [on\|off\|toggle] (bare = report state)<br/>`Usage: g2verbose [on\|off\|toggle] (bare = report state)` |
| `openg2` | A | Connect to G2 glasses: openg2 [left\|right\|auto\|saved]<br/>`Usage: openg2 [left\|right\|auto\|saved] (default auto; saved = peer MACs)` |

## ring

> The Even R1 smart ring over BLE, through a serialized, profile-gated transaction owner. ring scan [seconds] finds it and openring [saved|reconnect|<mac>] connects (scanning first when no argument is given); ring status [json] shows the link and closering ends it. ring health [json] reads vitals, ring controls and typed history; ring poll refreshes the vitals and ring history the history (ring history force, admin, skips the freshness window). ring query requests one reading; its raw form is admin-only, confirms every SET and refuses user-profile writes. ring control <collection|lowpower> <preserve|on|off> (admin) saves a desired state applied asynchronously, which the OLED and web Health controls also use; ring control refresh reads low power back. The G2's own Health panel is fed the way the Even app feeds it: ring panel status and ring panel preview [test] show the snapshot and its bytes; ring panel send [test] (admin) sends one; ring panel feed [on|off] (admin) feeds it automatically, saved and on by default, refreshing ring history every ring panel history [minutes] (admin, default 30). Its insights page gets up to three lines from ring history (heart rate and HRV, steps, sleep or SpO2): ring insights status and ring insights preview show them and their bytes; ring insights send (admin) sends them; ring insights feed [on|off] (admin, saved, on by default, runs with the panel feed) and ring insights days [1-7] (admin, default 3) set the feed and its window.

| Command | | Description |
| ------- | :-: | ----------- |
| `closering` | A | Close the R1 ring: drop its link and release its owner task (glasses, phone and Bluetooth stay up). |
| `openring` | A | Open the R1 ring on the Bluetooth client stack and connect (scan, the saved address, reconnect, or a MAC).<br/>`Usage: openring [saved\|reconnect\|<mac>] (none = scan then connect; saved = the saved address; reconnect = drop, settle, connect)` |
| `ring` |  | Even R1 ring.<br/>`Usage: ring <status\|scan\|health\|poll\|history\|query\|control\|panel\|insights> (openring / closering connect and disconnect)` |
| `ring control` | A | Set the ring's health collection or low power.<br/>`Usage: ring control <collection\|lowpower> <preserve\|on\|off> (saved, applied asynchronously; ring health shows desired/observed)` |
| `ring control refresh` |  | Read the ring's low-power state back. |
| `ring health` |  | R1 vitals, ring controls and typed history. (add 'json' for JSON output) |
| `ring history` |  | Refresh R1 typed history. |
| `ring history force` | A | Refresh R1 typed history past the freshness window. |
| `ring insights days` | A | Days of ring history the insights cover (saved).<br/>`Usage: ring insights days [1-7] (default 3; the most recent days with data)` |
| `ring insights feed` | A | Fill the Health panel's insights page from ring history (saved; on by default, runs with ring panel feed).<br/>`Usage: ring insights feed [on\|off]` |
| `ring insights preview` |  | Show the insight lines and their bytes without sending.<br/>`Usage: ring insights preview` |
| `ring insights send` | A | Send the insight lines to the glasses' Health panel now.<br/>`Usage: ring insights send` |
| `ring insights status` |  | Glasses Health insights: state, window, current lines, last send and echo. |
| `ring panel feed` | A | Feed the glasses' Health panel from the ring (saved; on by default).<br/>`Usage: ring panel feed [on\|off]` |
| `ring panel history` | A | How often the feed refreshes ring history (steps, kcal, sleep).<br/>`Usage: ring panel history [minutes 10-1440] (default 30)` |
| `ring panel preview` |  | Show the snapshot and its bytes without sending.<br/>`Usage: ring panel preview [test]` |
| `ring panel send` | A | Send one snapshot to the glasses' Health panel now.<br/>`Usage: ring panel send [test] (test = fixed values 1234 steps, 210 kcal, sleep score 80 / 7h30m, 72 bpm, 98%)` |
| `ring panel status` |  | Glasses Health panel feed: state, current values, last send and echo. |
| `ring poll` |  | Refresh R1 vitals now. |
| `ring query` |  | Queue one R1 query.<br/>`Usage: ring query <wear\|hr\|hrv\|spo2\|temp\|activity\|sleep> [daily\|point\|measure] ring query raw <module> <cmd> <subCmd> [hex_payload] [status=NN] (admin; a SET asks to confirm) status: bit0 type (0 notify, 1 ack), bit1 method (0 get, 1 set), bits2-3 ack (0 ok, 1 err, 2 refuse, 3 notSup)` |
| `ring scan` |  | Scan for the R1 ring.<br/>`Usage: ring scan [seconds] (1..300, default 30)` |
| `ring status` |  | R1 link status. (add 'json' for JSON output) |

## llm

> On-device large language model that runs a quantized model file entirely on the device (model weights held in PSRAM). A model must be loaded before generation: llmload [file.bin] loads one (bare filenames are looked up in internal /system/llm first, then on the SD card under /sd/llm), llmmodels lists available files, llmunload frees the PSRAM, llmstatus shows engine state, and llmautostart 0|1 / llmdefaultmodel control boot-time loading. Two generate forms: bare 'llmgenerate <prompt>' BLOCKS and prints the whole reply, while 'llmgenerate json ...' starts async and returns a session id immediately — then poll llmresult json <offset> repeatedly (each call returns new text, the running total length, and a done flag) until done flips true; llmstop aborts an in-progress generation. The engine keeps a multi-turn conversation: llmclear resets it, llmretry regenerates the last reply (async), and llmturns json <index> reads back one turn at a time. The llm* setters (temperature, topp, minp, maxtokens, sentencelimit, hardcap, reppenalty/repwindow, maxcontext, kvprec, norepeatngram, confthreshold, contentboost) are admin-only sampler and KV-cache defaults that persist to flash; kvprec and maxcontext only take effect on the next model load.

| Command | | Description |
| ------- | :-: | ----------- |
| `llmask` |  | Ask a guided question by index<br/>`Usage: llmask <group> <template> [entity] \| llmask json <gen> <g> <t> [e] \| llmask repeat Composes the question on-device from 'llmmenu' indices and streams the answer (read it with 'llmresult json 0').` |
| `llmautostart` | A | Auto-load default model at boot (0\|1)<br/>`Usage: llmautostart <0\|1>` |
| `llmclear` |  | Reset the LLM conversation |
| `llmconfthreshold` | A | Low-confidence hedge threshold (0=off)<br/>`Usage: llmconfthreshold <-8.0-0> (mean logprob; default -1.0, 0 disables)` |
| `llmcontentboost` | A | On-topic logit bonus (0=off)<br/>`Usage: llmcontentboost <0.0-4.0> (default 1.5; higher = stickier to prompt words)` |
| `llmcorrupttest` | A | Debug: force corruption-recovery test |
| `llmdefaultmodel` | A | Set default model filename<br/>`Usage: llmdefaultmodel <filename.bin>` |
| `llmdomaingate` | A | Refuse prompts outside the model's domain (0\|1)<br/>`Usage: llmdomaingate <0\|1> (only enforced when the model .bin carries a domain vocab)` |
| `llmgenerate` |  | Ask the loaded model a question<br/>`Usage: llmgenerate <question> \| llmgenerate do: <intent> The question is wrapped in the model's Q:/A: format for you, same as the web chat. Prefix 'do:' to ask for a command instead of an answer. A prompt that already starts with 'Q:' is sent as-is.` |
| `llmhardcap` | A | Set default hard token cap<br/>`Usage: llmhardcap <0-512>` |
| `llmhttpca` | A | Set a private CA PEM for HTTPS<br/>`Usage: llmhttpca <PEM with escaped newlines> (or - to use system CA bundle)` |
| `llmhttpenabled` | A | Enable the optional HTTP agent source<br/>`Usage: llmhttpenabled <0\|1>` |
| `llmhttpmodel` | A | Set the OpenClaw agent target<br/>`Usage: llmhttpmodel openclaw/default` |
| `llmhttpplaintext` | A | Allow plaintext HTTP for a local bench<br/>`Usage: llmhttpplaintext <0\|1>` |
| `llmhttptoken` | A | Set the gateway token (stored encrypted)<br/>`Usage: llmhttptoken <token> (or - to clear)` |
| `llmhttpurl` | A | Set the agent gateway base URL<br/>`Usage: llmhttpurl https://host:port/v1 (or - to clear)` |
| `llmkvprec` | A | KV cache precision (0=FP32,1=FP16,2=INT8)<br/>`Usage: llmkvprec <0..2> (0=FP32,1=FP16,2=INT8; reload model to apply)` |
| `llmload` | A | Load model [model.bin]<br/>`Usage: llmload [model id] [maxctx]` |
| `llmmaxcontext` | A | Set KV cache context window (0=auto)<br/>`Usage: llmmaxcontext <0-4096>` |
| `llmmaxtokens` | A | Set default max tokens per reply<br/>`Usage: llmmaxtokens <1-512>` |
| `llmmenu` |  | Guided-input menu status/listing (add 'json')<br/>`Usage: llmmenu \| llmmenu json \| llmmenu json tpl <g> <off> \| llmmenu json ent <g> <off> Lists this model's guided question templates + entity rosters (empty if the model ships none).` |
| `llmminp` | A | Set min-p sampling floor (0=off)<br/>`Usage: llmminp <0.0-1.0>` |
| `llmmodels` |  | List available model files (add 'json' for JSON output) |
| `llmnorepeatngram` | A | Ban repeating generated n-grams (0=off)<br/>`Usage: llmnorepeatngram <0-8> (default 0=off; 3 breaks verbatim phrase loops)` |
| `llmprofile` | A | Per-section forward-pass timing breakdown (0\|1)<br/>`Usage: llmprofile <0\|1> (diagnostic; splits qkv/attn/ffn/cls after each generation. Turn OFF other debugllm* flags for clean numbers)` |
| `llmreppenalty` | A | Set default repetition penalty<br/>`Usage: llmreppenalty <1.0-3.0>` |
| `llmrepwindow` | A | Set default rep-penalty look-back<br/>`Usage: llmrepwindow <1-32>` |
| `llmresult` |  | Poll streamed generation (JSON)<br/>`Usage: llmresult json <offset>` |
| `llmretry` |  | Regenerate the last reply (JSON) |
| `llmsentencelimit` | A | Set default sentence stop limit<br/>`Usage: llmsentencelimit <0-20>` |
| `llmstatus` |  | Show LLM engine status (add 'json' for JSON output) |
| `llmstop` |  | Stop in-progress generation |
| `llmtemperature` | A | Set default sampling temperature<br/>`Usage: llmtemperature <0.0-2.0>` |
| `llmtopp` | A | Set default Top-P threshold<br/>`Usage: llmtopp <0.0-1.0>` |
| `llmturns` |  | Read a conversation turn (JSON)<br/>`Usage: llmturns json <index> [offset] (follow next while more=true)` |
| `llmunload` | A | Unload model and free PSRAM |

## settingsedit

> Static CLI commands the web/OLED settings screen uses to persist individual settings fields that have no dedicated module command. Each writes one setting via handleSettingCommand; the value is read live or applied on next start. Fields that need a live apply action are routed to their module command instead of getting one of these.

| Command | | Description |
| ------- | :-: | ----------- |
| `apdsdevicepollms` | A | Set APDS poll interval (ms)<br/>`Usage: apdsdevicepollms <value>` |
| `automationautostart` | A | Start the automation scheduler at boot<br/>`Usage: automationautostart <0\|1>` |
| `eiautostart` | A | Load the default Edge Impulse model at boot<br/>`Usage: eiautostart <0\|1>` |
| `eiinterval` | A | Set Edge Impulse inference interval (ms)<br/>`Usage: eiinterval <100-10000>` |
| `eimaxdetections` | A | Set Edge Impulse max detections<br/>`Usage: eimaxdetections <value>` |
| `eirequirelabels` | A | Set Edge Impulse require-labels flag<br/>`Usage: eirequirelabels <0\|1>` |
| `espnowautostart` | A | Start ESP-NOW at boot<br/>`Usage: espnowautostart <0\|1>` |
| `espnowcaptureskipheartbeats` | A | Omit heartbeat frames from the ESP-NOW capture<br/>`Usage: espnowcaptureskipheartbeats <0\|1>` |
| `espnowcapturetosd` | A | Capture ESP-NOW frames to the SD card<br/>`Usage: espnowcapturetosd <0\|1> Needs an SD card mounted; frames are appended as they arrive.` |
| `eventlog` | A | Enable/disable the structured event-history log (events.log)<br/>`Usage: eventlog <0\|1> One line per system event, durable across reboots. Display/behavior unaffected.` |
| `fmradiodevicepollms` | A | Set FM radio poll interval (ms)<br/>`Usage: fmradiodevicepollms <value>` |
| `g2holdaction` | A | Set what a ring/temple hold on a HW1 lens page does<br/>`Usage: g2holdaction [0\|1] 0 = Off, 1 = QuickNote; a page with its own hold handler always gets the hold first` |
| `g2holdsource` | A | Set which hold starts the hold action<br/>`Usage: g2holdsource [0\|1] 0 = ring only, 1 = ring + temples; glasses firmware that reports no hold source accepts any` |
| `g2holdunprofiled` | A | Allow hold actions on unprofiled glasses firmware (testing)<br/>`Usage: g2holdunprofiled [0\|1] Profiled: both arms 2.2.9.22 or 2.3.2.14 (see g2hold status)` |
| `gpsdevicepollms` | A | Set GPS poll interval (ms)<br/>`Usage: gpsdevicepollms <value>` |
| `logcategorytags` | A | Set log category-tags flag (persist only)<br/>`Usage: logcategorytags <0\|1>` |
| `notemaxsec` | A | Set the longest QuickNote, in seconds<br/>`Usage: notemaxsec [10-120] Default 60 with on-device transcription, 30 with the Pi (which caps every note at 30)` |
| `notemic` | A | Set the microphone for CLI and web QuickNotes<br/>`Usage: notemic [0\|1\|2] 0 = Auto (the paired wearer's glasses if connected, else the board mic), 1 = Glasses, 2 = Board mic Notes started from the glasses always use the glasses mic` |
| `notifydeviceapp` | A | Enable/disable Android app notification cards<br/>`Usage: notifydeviceapp <0\|1> Cards go only to BLE sessions that are logged in; an unauthenticated app sees none.` |
| `notifydevicebanners` | A | Enable/disable OLED notification banners<br/>`Usage: notifydevicebanners <0\|1>` |
| `notifydeviceg2` | A | Enable/disable G2 lens notification cards<br/>`Usage: notifydeviceg2 <0\|1>` |
| `notifydevicekind` | A | Set per-event notification visibility (device-wide)<br/>`Usage: notifydevicekind [list [json]] \| <kind> [all\|admin\|off] Bare: show non-default kinds; list: show every kind (json = machine form) <kind> alone shows its level; with a level, sets and persists it admin: only admin viewers see it; off: hidden for everyone Levels affect banners/toasts/queue only - events and automations still fire` |
| `notifydevicequeue` | A | Enable/disable the notification-center queue<br/>`Usage: notifydevicequeue <0\|1>` |
| `notifydevicetoasts` | A | Enable/disable web notification toasts<br/>`Usage: notifydevicetoasts <0\|1>` |
| `notifylevel` |  | Set YOUR notification importance floor<br/>`Usage: notifylevel [verbose\|standard\|alert] Bare: show your current floor (default: standard) verbose: everything; standard: skip routine chatter; alert: security/safety only Nothing is lost - filtered kinds still reach the notification center and automations` |
| `notifyusermute` |  | Mute event kinds from notifications for YOUR user<br/>`Usage: notifyusermute [<kind,kind,...>\|set <kind> <on\|off>\|patch <+kind,-kind>\|all\|none] Bare: show; legacy list: replace; set: change one; patch: change several all/none: mute every current kind/clear; applies only to your logged-in user List valid kinds with 'events kinds'` |
| `notifyusershow` |  | Force event kinds through YOUR importance floor<br/>`Usage: notifyusershow [<kind,kind,...>\|set <kind> <on\|off>\|patch <+kind,-kind>\|all\|none] Bare: show; legacy list: replace; set: change one; patch: change several all/none: force every current kind/clear; overrides your notifylevel List valid kinds with 'events kinds'` |
| `oledautostart` | A | Start the OLED display at boot<br/>`Usage: oledautostart <0\|1>` |
| `presencedevicepollms` | A | Set presence sensor poll interval (ms)<br/>`Usage: presencedevicepollms <50-5000>` |
| `sensorlogformat` | A | Set sensor-log format (0=text,1=csv,2=track)<br/>`Usage: sensorlogformat <0\|1\|2> Track (2) replaces the saved sensor selection with GPS-only.` |
| `sensorlogmask` | A | Set sensor-log sensor bitmask<br/>`Usage: sensorlogmask <0-255>` |
| `sensorlogpath` | A | Set default sensor-log file path<br/>`Usage: sensorlogpath <"/path">` |
| `sessionidleble` | A | Set BLE session idle-logout (min)<br/>`Usage: sessionidleble <0-1440>` |
| `sessionidledisplay` | A | Set OLED session idle-logout (min)<br/>`Usage: sessionidledisplay <0-1440>` |
| `sessionidleserial` | A | Set serial session idle-logout (min)<br/>`Usage: sessionidleserial <0-1440>` |
| `sessionidleuart` | A | Set UART link session idle-logout (min)<br/>`Usage: sessionidleuart <0-1440>` |
| `sessionidleweb` | A | Set web CLI session idle-logout (min)<br/>`Usage: sessionidleweb <0-1440>` |
| `srautostart` | A | Set ESP-SR auto-start flag<br/>`Usage: srautostart <0\|1>` |
| `srmodelsource` | A | Set ESP-SR model source<br/>`Usage: srmodelsource <value>` |
| `sttautogain` | A | STT picks its own onboard-mic gain (changed only when several sessions agree) and a 120 Hz high-pass; 0 = use pdmamplify/pdmhpf. Applies to the next session.<br/>`Usage: sttautogain [0\|1]` |
| `sttdecisionhold` | A | Continuous-STT onset noise control: a phrase only the pre-emphasised level opened needs the raw level to reach its onset rule before raw votes hold it open (0 = off, the default); applies to the next session.<br/>`Usage: sttdecisionhold [0\|1]` |
| `sttdecisionmod` | A | Continuous-STT onset noise control: the pre-emphasised level votes only while its last 500 ms is modulated (ratio x0.01; 0 = off, the default). A noisy-room mode that gives back far speech; applies to the next session.<br/>`Usage: sttdecisionmod [0\|101-1000]` |
| `sttdecisionpre` | A | Continuous-STT level-robust onset: a phrase may also open on the pre-emphasised level, matched to the room (coefficient x0.01; 0 = raw level only, 97 = default); applies to the next session.<br/>`Usage: sttdecisionpre [0-100]` |
| `sttg2autogain` | A | G2 glasses-mic profile: STT multiplies the glasses audio (1..8). Starts at x1 and changes only when several sessions agree. Applies to the next session.<br/>`Usage: sttg2autogain [0\|1]` |
| `sttg2decisionhold` | A | G2 glasses-mic profile: onset raw-latch hold (0 = off, the default); applies to the next session.<br/>`Usage: sttg2decisionhold [0\|1]` |
| `sttg2decisionmod` | A | G2 glasses-mic profile: onset modulation gate (ratio x0.01; 0 = off, the default); applies to the next session.<br/>`Usage: sttg2decisionmod [0\|101-1000]` |
| `sttg2decisionpre` | A | G2 glasses-mic profile: level-robust onset (x0.01; 0 = raw level only, 97 = default); applies to the next session.<br/>`Usage: sttg2decisionpre [0-100]` |
| `sttg2noisepercentile` | A | G2 glasses-mic profile: noise-window percentile used as the room level; applies to the next session.<br/>`Usage: sttg2noisepercentile [0-50]` |
| `sttg2silencefloor` | A | G2 glasses-mic profile: silence floor in frame-RMS counts; applies to the next session.<br/>`Usage: sttg2silencefloor [1-32767]` |
| `sttg2speechfloor` | A | G2 glasses-mic profile: speech onset floor in frame-RMS counts; applies to the next session.<br/>`Usage: sttg2speechfloor [1-32767]` |
| `sttnoisepercentile` | A | Continuous-STT noise-window percentile used as the room level; applies to the next session.<br/>`Usage: sttnoisepercentile [0-50]` |
| `sttphraselevel` | A | Within-phrase leveler: STT steps a session's audio x1..x8 every 0.1 s, for the mic whose automatic gain is on (0 = off, the default). Applies to the next session.<br/>`Usage: sttphraselevel [0\|1]` |
| `sttsavetranscripts` | A | Save recognized text; changes apply to the next STT session.<br/>`Usage: sttsavetranscripts [0\|1]` |
| `sttsilencefloor` | A | Continuous-STT silence floor in frame-RMS counts; applies to the next session.<br/>`Usage: sttsilencefloor [1-32767]` |
| `sttspeechfloor` | A | Continuous-STT speech onset floor in frame-RMS counts; applies to the next session.<br/>`Usage: sttspeechfloor [1-32767]` |
| `systemlogflags` | A | Set system-log debug category mask (hex)<br/>`Usage: systemlogflags <0x...>` |
| `tofi2cclockhz` | A | Set ToF I2C clock (Hz)<br/>`Usage: tofi2cclockhz <50000-400000>` |
| `wifiautostart` | A | Connect to a saved WiFi network at boot: <0\|1><br/>`Usage: wifiautostart <0\|1>` |

