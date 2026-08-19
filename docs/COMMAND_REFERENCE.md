# Command Reference

> **Generated file — do not edit by hand.**
> Regenerate with `python3 tools/command_registry.py reference`.
> Source of truth is the `CommandEntry` tables in `components/hardwareone/*.cpp`.

946 commands across 49 modules (958 registry entries).

Commands are matched case-insensitively, and lookup uses longest-prefix matching, so `automation list` resolves to the `automation` dispatcher with `list` as its argument.

Legend: **A** = requires admin &nbsp; **S** = requires super admin

## Modules

- [`cli`](#cli) — 4 commands
- [`system`](#system) — 26 commands
- [`wifi`](#wifi) — 20 commands
- [`espnow`](#espnow) — 120 commands
- [`mqtt`](#mqtt) — 27 commands
- [`bluetooth`](#bluetooth) — 19 commands
- [`filesystem`](#filesystem) — 10 commands
- [`sd`](#sd) — 5 commands
- [`oled`](#oled) — 20 commands
- [`neopixel`](#neopixel) — 3 commands
- [`led`](#led) — 6 commands
- [`servo`](#servo) — 5 commands
- [`thermal`](#thermal) — 22 commands
- [`tof`](#tof) — 9 commands
- [`imu`](#imu) — 15 commands
- [`input`](#input) — 4 commands
- [`gamepad`](#gamepad) — 1 commands
- [`anoencoder`](#anoencoder) — 5 commands
- [`apds`](#apds) — 8 commands
- [`gps`](#gps) — 5 commands
- [`fmradio`](#fmradio) — 9 commands
- [`rtc`](#rtc) — 6 commands
- [`presence`](#presence) — 5 commands
- [`camera`](#camera) — 49 commands
- [`microphone`](#microphone) — 15 commands
- [`dictation`](#dictation) — 1 commands
- [`edgeimpulse`](#edgeimpulse) — 16 commands
- [`espsr`](#espsr) — 45 commands
- [`i2c`](#i2c) — 33 commands
- [`automation`](#automation) — 8 commands
- [`battery`](#battery) — 3 commands
- [`debug`](#debug) — 182 commands
- [`settings`](#settings) — 18 commands
- [`sensorlog`](#sensorlog) — 2 commands
- [`health`](#health) — 3 commands
- [`users`](#users) — 23 commands
- [`features`](#features) — 2 commands
- [`image`](#image) — 4 commands
- [`map`](#map) — 12 commands
- [`mapsettings`](#mapsettings) — 3 commands
- [`power`](#power) — 3 commands
- [`liveaudio`](#liveaudio) — 1 commands
- [`cm5`](#cm5) — 14 commands
- [`ota`](#ota) — 18 commands
- [`setpattern`](#setpattern) — 1 commands
- [`even_g2`](#even-g2) — 56 commands
- [`even_r1`](#even-r1) — 5 commands
- [`llm`](#llm) — 30 commands
- [`settingsedit`](#settingsedit) — 57 commands

---

## cli

> The cli module is the on-device help and CLI navigation layer, not a feature subsystem. help opens a paged help browser: bare help shows the main menu listing every registered module, help <module> drills into one module command page (and prints that module subsystem overview at the top), and the special topics help sensors (aggregate view across all sensor modules), help all (show every command including hidden ones), and help tail (dump suppressed output) cover the rest. While the browser is open the CLI is in a help state, so back steps from a module page up to the main menu, exit leaves help mode entirely and returns to the normal prompt, and clear wipes the CLI scrollback/history.

| Command | | Description |
| ------- | :-: | ----------- |
| `back` |  | Return to main help menu |
| `clear` |  | Clear CLI history |
| `exit` |  | Exit help mode |
| `help` |  | Display help menu (help [topic])<br/>`Usage: help [<module>\|sensors\|all\|tail]` |

## system

> The system module holds core device commands that do not belong to any peripheral. Status and inspection: status (WiFi, filesystem, memory summary), uptime, time (uptime plus NTP wall-clock if synced), temperature and voltage (ESP32 internal die temp and supply rail), taskstats/perftop (FreeRTOS task and live loop/CPU profiling), fsusage, events (recent system events from the in-memory register that drives automation event triggers), and the memory tools memsample (snapshot, with memsample track on|off|reset|status for allocation tracking) and memreport. Control and power: reboot, ramflush, cpufreq [80|160|240] to read or set CPU clock, lightsleep [seconds] for ESP32 light sleep, deepsleep for power-off (reset to wake), and wait <ms>/sleep <ms> to pause command-script execution. timeset sets the clock manually. broadcast <message> pushes a line of text to all connected output interfaces, and factoryreset deletes the user-accounts file so the first-boot setup wizard re-runs on next reboot while deliberately preserving WiFi credentials and other settings. Most mutating commands (timeset, cpufreq, reboot, ramflush, factoryreset, broadcast, lightsleep, deepsleep) require admin.

| Command | | Description |
| ------- | :-: | ----------- |
| `bootcount` |  | Show boot count (NVS), crash count, last reset reason. 'bootcount reset' zeroes it (admin). (add 'json')<br/>`Usage: bootcount [reset\|json]` |
| `broadcast` | A | Send a message to all connected output interfaces.<br/>`Usage: broadcast <message>` |
| `cpufreq` | A | Get/set CPU frequency (admin).<br/>`Usage: cpufreq [80\|160\|240]` |
| `crashlog` |  | Show the last recorded crash (panic text, core/PC, boot phase, repeat count). (add 'json')<br/>`Usage: crashlog [json]` |
| `deepsleep` | A | Power off via deep sleep (no wake source — reset button to wake).<br/>`Usage: deepsleep (admin; wakes only via physical reset)` |
| `events` |  | Show recent system events (the in-memory register that drives automation event triggers).<br/>`Usage: events [kinds [json]] (bare): show the recent-event ring kinds: list every valid event-kind name (json = machine form)` |
| `factoryreset` | S | Wipe user accounts and reboot to re-run setup wizard.<br/>`Usage: factoryreset (no args, confirmation required) Deletes /system/users/users.json so the first-time setup wizard runs on next boot. WiFi credentials and other settings are preserved.` |
| `fsusage` |  | Show filesystem usage. (add 'json' for JSON output) |
| `lightsleep` | A | Enter ESP32 light sleep: lightsleep [seconds] (default 20s).<br/>`Usage: lightsleep [seconds] (1..3600, default 20)` |
| `memreport` |  | Comprehensive memory report (Task Manager style). (add 'json' for JSON output) |
| `memsample` |  | Memory snapshot with component requirements. Use 'memsample track [on\|off\|reset\|status]' for allocation tracking.<br/>`Usage: memsample [track <on\|off\|reset\|status>]` |
| `pendinglist` | A | List pending user requests. |
| `perftop` |  | Live performance snapshot: loop laps/s, period, per-section timing, worst stalls + live task CPU%. (add 'json' for loop + per-task CPU% JSON) |
| `ramflush` | A | Reboot to reclaim RAM, restoring the features running right now.<br/>`Usage: ramflush [status] (bare): capture which features are running, reboot, and restore them status: show what the last boot restored (no reboot) Restores for that one boot only — a normal reboot returns to configured autostart. Never changes your autostart settings.` |
| `reboot` | A | Reboot the system. |
| `sleep` |  | Alias for wait: sleep <ms>.<br/>`Usage: sleep <ms> (1..60000)` |
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

## wifi

> The WiFi subsystem manages station-mode network connections plus the network services that ride on top of them: NTP time sync and the on-device HTTP/HTTPS server. Saved networks are stored as a prioritized list (wifilist, wifiadd, wifirm, wifipromote) and persist to flash; openwifi connects by best-priority (default) or by --index <N>, and a failed indexed attempt auto-rolls back to the previously connected network. Note two distinct disconnects: closewifi tears down the link AND stops the HTTP server and web output to free heap, while wifidisconnect (drop) leaves the radio and web server up so you can move to another network. wifiscan lists nearby APs, ntpsync/ntpstatus handle clock sync, and openhttp/closehttp/httpstatus run the web server (compiled in only when the HTTP server is enabled). certinfo and certgen (admin-only) manage the self-signed HTTPS certificate.

| Command | | Description |
| ------- | :-: | ----------- |
| `certgen` | S | Generate self-signed HTTPS certificate: [rsa] (default: ECDSA P-256)<br/>`Usage: certgen [rsa] Default: ECDSA P-256 (~1s). Use 'certgen rsa' for RSA-2048 (~30-60s).` |
| `certinfo` |  | Show HTTPS certificate details. |
| `closehttp` | A | Stop HTTP server. |
| `closewifi` | A | Disconnect from WiFi (also stops HTTP server + web output to free heap). |
| `httpread` |  | Read HTTP server status. (add 'json' for JSON output) |
| `httpstatus` |  | Show HTTP server status. (add 'json' for JSON output) |
| `ntpstatus` |  | Show NTP configuration and sync state. |
| `ntpsync` |  | Sync time with NTP server. |
| `openhttp` | A | Start HTTP server. |
| `openwifi` | A | Connect to WiFi: [--best \| --index <N>] (default: best)<br/>`Usage: openwifi [--best \| --index <1..N>]` |
| `radiopower` | A | Power the whole 2.4GHz radio on/off (airplane mode; also stops/restores ESP-NOW; runtime only, not persisted): [on\|off\|toggle]<br/>`Usage: radiopower [on\|off\|toggle]` |
| `wifiadd` | A | Add WiFi network: <ssid> <pass> [priority] [hidden]<br/>`Usage: wifiadd <ssid> <pass> [priority] [hidden0\|1]` |
| `wifidisconnect` | A | Disconnect from the current network but keep the radio on (HTTP/web stay up). |
| `wifigettxpower` | A | Report the current WiFi TX power in dBm (read-only; set it with 'wifitxpower').<br/>`Usage: wifigettxpower (no arguments)` |
| `wifilist` |  | List saved WiFi networks. (add 'json' for JSON output) |
| `wifipromote` | A | Promote WiFi to top priority: <ssid> [newPriority]<br/>`Usage: wifipromote <ssid> [newPriority]` |
| `wifiread` |  | Read current WiFi connection info. (add 'json' for JSON output) |
| `wifirm` | A | Remove WiFi network: <ssid><br/>`Usage: wifirm <ssid>` |
| `wifiscan` |  | Scan for available WiFi networks. (add 'json' for JSON output) |
| `wifistatus` |  | Show current WiFi connection info. (add 'json' for JSON output) |

## espnow

> ESP-NOW links HardwareOne devices directly over the WiFi radio with no router or access point, as named peers that can also form a multi-hop mesh. Pair with espnowpair, then message (espnowsend/espnowbroadcast), push a file (espnowsendfile), pull a file (espnowfetch), browse a peer's files (espnowbrowse), or run a command on a peer (espnowremote). espnowremote, espnowfetch, espnowbrowse, espnowroomcmd and espnowtagcmd are ASYNCHRONOUS: they return OK on delivery; the real result arrives later in the message buffer, read with 'espnowmessages json [mac]'. Mesh mode (espnowmode mesh) adds routing with a TTL and master/worker/backup roles; each device carries identity metadata (name, friendly name, room, zone, tags) queried with espnowdeviceinfo locally or espnowrequestmeta for a peer.

| Command | | Description |
| ------- | :-: | ----------- |
| `bondconnect` | A | Connect to bonded peer device: 'bondconnect <mac_or_name>'. (async - bond establishes when peer is seen; watch bondstatus)<br/>`Usage: bondconnect <mac_or_name> Returns immediately; the bond completes when the peer appears via heartbeat - watch 'bondstatus'.` |
| `bonddisconnect` | A | Disconnect from bonded peer device. |
| `bondrequestcap` | A | Request capability summary from bonded peer. (async - remote cap via GET /api/bond/status; note bondshowcap shows LOCAL cap) |
| `bondrequestmanifest` | A | Request full manifest from bonded peer. (async - view with bondshowremotemanifest) |
| `bondrequestschema` | A | Request settings schema from bonded peer. (async - cached; read via GET /api/bond/settings/schema) |
| `bondrequestsettings` | A | Request settings file from bonded peer. (async - cached; read via GET /api/bond/settings) |
| `bondresync` | A | Force re-sync of bond state (cap+manifest+settings+schema). Use when UI is stuck on 'Establishing Bond' or peer state looks stale. (async - results populate as they arrive)<br/>`Usage: bondresync [--cap\|--manifest\|--settings\|--schema\|--all] Returns OK on dispatch; results arrive over time - view via 'bondshowremotemanifest' and GET /api/bond/status, /api/bond/settings, /api/bond/settings/schema.` |
| `bondrole` | A | Get/set bond mode role: 'bondrole [master\|worker]' (no arg shows current role).<br/>`Usage: bondrole [master\|worker] (no arg shows current role)` |
| `bondshowcap` |  | Show local device capability summary. |
| `bondshowmanifest` |  | Show local device manifest (UI apps + CLI commands). |
| `bondshowremotemanifest` |  | Show cached remote manifest(s): 'bondshowremotemanifest [fwHash]'.<br/>`Usage: bondshowremotemanifest [<fwHash>]` |
| `bondstatus` |  | Show bond mode status and configuration. |
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
| `espnowbackupfingerprint` | A | Worker: set the authorized BACKUP-master Ed25519 fingerprint (64-hex; empty=deny).<br/>`Usage: espnowbackupfingerprint <64-hex-pubkey>` |
| `espnowbondmodeenabled` | A | Enable/disable bond mode: <0\|1><br/>`Usage: espnowbondmodeenabled <0\|1>` |
| `espnowbondpeermac` | A | Set bond peer MAC address<br/>`Usage: espnowbondpeermac <AA:BB:CC:DD:EE:FF>` |
| `espnowbroadcast` |  | Broadcast message: 'espnowbroadcast <message>'. (async send; delivery only, no reply)<br/>`Usage: espnowbroadcast <message> (single frame, <= 218 bytes; longer text is NOT fragmented and fails silently) Returns whether the single broadcast frame was transmitted to all peers, NOT a per-device delivery count; no per-device reply.` |
| `espnowbroadcaststats` |  | Show broadcast ACK tracking statistics. |
| `espnowbrowse` | A | Browse a peer's files; user/pass are an account ON THE TARGET: 'espnowbrowse <target> <target-user> <target-pass> ["path"]'. (async - result via espnowmessages json)<br/>`Usage: espnowbrowse <target> <target-user> <target-pass> ["path"] Credentials are verified ON THE TARGET device, not this one. Returns OK on delivery; the remote listing arrives later - read with 'espnowmessages json' (match the reqId).` |
| `espnowchannel` | A | Get/set preferred ESP-NOW channel: 'espnowchannel [1-13\|auto\|resync]'. No arg shows a checker (actual vs expected). Set the SAME value on both devices to pair off-grid.<br/>`Usage: espnowchannel [<1..13>\|auto\|resync] (no arg) = show preference, actual radio channel, expected, and an OK/MISMATCH check. auto (0) = follow WiFi, pin fallback when offline. 1-13 = force this channel when not joined to WiFi. resync = force the radio back onto the correct channel now (no setting change). Two devices only hear each other on the same channel — set both the same for field use.` |
| `espnowdeviceinfo` |  | Show all local device metadata. |
| `espnowdevices` |  | List all mesh devices with room/zone/tags/status: espnowdevices [json]. |
| `espnowdiscovered` | A | List devices found during discovery: 'espnowdiscovered [clear]'. Pair one with 'espnowpairrequest <mac_or_name>'.<br/>`Usage: espnowdiscovered [clear] Lists same-mesh devices heard while your discovery window ('espnowpairmode') is open.` |
| `espnowenabled` | A | Enable/disable ESP-NOW (0\|1, takes effect after reboot).<br/>`Usage: espnowenabled <0\|1>` |
| `espnowencstatus` | A | Show ESP-NOW encryption status and key fingerprint. |
| `espnowfailovertimeout` | A | Set failover timeout: <5000-120000 ms><br/>`Usage: espnowfailovertimeout <5000..120000>` |
| `espnowfetch` | A | Fetch a file from a peer; user/pass are an account ON THE TARGET: 'espnowfetch <target> <target-user> <target-pass> "<path>"'. (async - status via espnowmessages json; file saved on this device)<br/>`Usage: espnowfetch <target> <target-user> <target-pass> "<path>" Credentials are verified ON THE TARGET device, not this one. Returns OK on delivery; status lands in 'espnowmessages json'; the fetched file is written to this device's filesystem.` |
| `espnowfind` |  | Find devices by name, room, or tag: 'espnowfind <query>'.<br/>`Usage: espnowfind <query>` |
| `espnowfirsttimesetup` | A | Set first time setup flag: <0\|1><br/>`Usage: espnowfirsttimesetup <0\|1>` |
| `espnowforget` | A | Forget a peer's crypto identity + close its session: 'espnowforget <name_or_mac>'.<br/>`Usage: espnowforget <name_or_mac>` |
| `espnowfriendlyname` |  | Get/set friendly display name: 'espnowfriendlyname [name]'.<br/>`Usage: espnowfriendlyname [<name>] (<=47 chars) espnowfriendlyname clear` |
| `espnowhbmode` |  | Get/set heartbeat mode: 'espnowhbmode [public\|private]'.<br/>`Usage: espnowhbmode [public\|private]` |
| `espnowheartbeatbroadcast` | A | Set heartbeat broadcast: <0\|1><br/>`Usage: espnowheartbeatbroadcast <0\|1>` |
| `espnowheartbeatinterval` | A | Set master heartbeat interval: <1000-60000 ms><br/>`Usage: espnowheartbeatinterval <1000..60000>` |
| `espnowidentity` |  | Show long-term Ed25519 identity (MAC, pub key, createdAtSec, regenCount). |
| `espnowkeyex` | A | Initiate KEY_EX handshake with a peer (runs alongside legacy pairing). (async - handshake completes later; check espnowsessions)<br/>`Usage: espnowkeyex <name_or_mac> [<mesh>] Returns OK when KEY_EX_HELLO is sent; the handshake completes asynchronously - inspect with 'espnowsessions' / 'espnowencstatus'.` |
| `espnowlist` |  | List all paired ESP-NOW devices. |
| `espnowmasterfingerprint` | A | Worker: set the authorized PRIMARY-master Ed25519 fingerprint (64-hex; empty=deny).<br/>`Usage: espnowmasterfingerprint <64-hex-pubkey>` |
| `espnowmeshbackup` | A | Get/set backup MAC: 'espnowmeshbackup [MAC]'.<br/>`Usage: espnowmeshbackup [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmeshes` | A | Manage multi-mesh slots: 'espnowmeshes [list\|add\|remove\|enable\|setdefault\|rename\|setpassphrase] ...'.<br/>`Usage: espnowmeshes list espnowmeshes add <label> (then set passphrase via 'espnowsetpassphrase <label> <pw>') espnowmeshes remove <label> (alias: disable) espnowmeshes enable <label> espnowmeshes setdefault <label> espnowmeshes setpassphrase <label> <passphrase> espnowmeshes rename <oldLabel> <newLabel>` |
| `espnowmeshmaster` | A | Get/set master MAC: 'espnowmeshmaster [MAC]'.<br/>`Usage: espnowmeshmaster [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmeshmetrics` |  | Show multi-hop routing metrics (flood forwards, routed hops, drops, route churn). |
| `espnowmeshpeermax` | A | Set max peer slots: <1-16> (reboot required)<br/>`Usage: espnowmeshpeermax <1..16>` |
| `espnowmeshrelay` | A | Carry other nodes' mesh traffic: <0\|1><br/>`Usage: espnowmeshrelay <0\|1> 1 (default) = act as a relay so out-of-range peers can reach each other through this node. 0 = stop forwarding for others. This node still uses multi-hop for its OWN traffic (set espnowmeshttl 1 for that), and stops advertising routes so nobody sends via it.` |
| `espnowmeshrole` | A | Get/set mesh role: 'espnowmeshrole [worker\|master\|backup]'.<br/>`Usage: espnowmeshrole [worker\|master\|backup]` |
| `espnowmeshroutes` |  | Show the mesh route table: who this node can reach and via which neighbour.<br/>`Usage: espnowmeshroutes [clear] 'via (direct)' = in radio range. Anything else is reached over one or more relay hops. Multi-hop routes are learned from neighbours every 30s — allow a minute after boot.` |
| `espnowmeshsave` |  | Manually save mesh peer topology to filesystem. |
| `espnowmeshstatus` |  | Show mesh peer health (heartbeats & ACKs). |
| `espnowmeshtopo` |  | Discover mesh topology (run on the master; role not enforced). (async - read results with espnowtoporesults) |
| `espnowmeshttl` |  | Get/set the multi-hop budget: 'espnowmeshttl [1-10]'.<br/>`Usage: espnowmeshttl [<1..10>] How many hops this node's relay-eligible frames may travel. 1 = single hop (no multi-hop). Applies to broadcast text/time sync and to routed unicast; heartbeats and pairing are always single-hop.` |
| `espnowmessages` |  | Buffered message history as JSON: 'espnowmessages json [sinceSeq] [mac]' — async results of espnowremote/browse/fetch.<br/>`Usage: espnowmessages [json] [<sinceSeq>] [<AA:BB:CC:DD:EE:FF>]` |
| `espnowmode` | A | Get/set ESP-NOW mode: 'espnowmode [direct\|mesh]'.<br/>`Usage: espnowmode [direct\|mesh]` |
| `espnowpair` | A | Pair ESP-NOW device: 'espnowpair <mac> <name> [mesh]'. (synchronous; local registry add, no remote handshake)<br/>`Usage: espnowpair <mac> <name> [mesh]` |
| `espnowpairmode` | A | Open the discovery window: 'espnowpairmode [seconds\|off\|status]'. Open on BOTH same-mesh devices, then 'espnowdiscovered' to see + pair. No auto-pair.<br/>`Usage: espnowpairmode [<seconds>\|off\|status] Default 120s, max 600. Requires a mesh passphrase set on both devices (same mesh). While open, the device broadcasts a discovery beacon and RECORDS other same-mesh devices it hears (see 'espnowdiscovered') — it no longer auto-pairs. Pair from the list with 'espnowpairsecure <mac> <name>'.` |
| `espnowpairrequest` | A | Request a pair with a discovered device: 'espnowpairrequest <mac_or_name>'. The other device must Accept.<br/>`Usage: espnowpairrequest <mac_or_name> (name/MAC from 'espnowdiscovered') Sends a request; the target shows an Accept prompt (or run 'espnowaccept' there).` |
| `espnowpairsecure` | A | Pair device with encryption: 'espnowpairsecure <mac> <name> [mesh]'. (local pair is synchronous; secure channel completes async - see espnowsessions)<br/>`Usage: espnowpairsecure <mac_address> <device_name> [mesh] Requires a mesh passphrase first - run 'espnowsetpassphrase <mesh> <passphrase>'. The device is added synchronously; KEY_EX then runs asynchronously (~100ms) so the encrypted channel becomes usable shortly after - inspect with 'espnowsessions' / 'espnowencstatus'.` |
| `espnowprobe` | A | Reachability probe via KEY_EX. Synchronous, bounded timeout. Reports alive+mesh+firmware in one shot (no plaintext on the wire).<br/>`Usage: espnowprobe <name_or_mac> [<timeoutMs (50-5000, default 500)>] [<mesh>]` |
| `espnowread` |  | Read ESP-NOW status and configuration. |
| `espnowregenidentity` | S | Regenerate Ed25519 identity. Requires '--confirm-wipe-all-bonds'.<br/>`Usage: espnowregenidentity --confirm-wipe-all-bonds` |
| `espnowreject` | A | Reject a pending incoming pair request: 'espnowreject [<mac_or_name>]'.<br/>`Usage: espnowreject [<mac_or_name>]` |
| `espnowrekey` | A | Force immediate SESSION_REKEY for a peer (manual trigger). (async - completes later; check espnowsessions)<br/>`Usage: espnowrekey <name_or_mac> Returns OK when REKEY is sent; new keys derive when the peer's REKEY arrives - verify with 'espnowsessions'.` |
| `espnowrelayblock` | A | DIAGNOSTIC: drop ALL inbound ESP-NOW frames from one MAC (bench stand-in for an out-of-range peer; relay HW testing). RAM-only - reboot clears it.<br/>`Usage: espnowrelayblock <mac\|clear\|status> Drops frames at the RX drain before any parsing - heartbeats too, so the peer goes offline within ~30s, like real RF loss.` |
| `espnowremote` | A | Execute a command on a peer: 'espnowremote <target> <target-user> <target-pass> <cmd>'. user/pass are an account ON THE TARGET (verified there), not this device. (async - result via espnowmessages json)<br/>`Usage: espnowremote <target> <target-user> <target-pass> <command> <target-user>/<target-pass> are credentials ON THE TARGET device, not this one. Async: returns a reqId on delivery; read the output later with 'espnowmessages json 0 <target-mac>' (match the reqId).` |
| `espnowrequestevents` | A | Ask a peer to send US only events in <bitmask>. Updates state ON THE PEER. (async - changes peer state, no reply; verify with espnowsubs on the peer)<br/>`Usage: espnowrequestevents <name_or_mac> <bitmask> Returns OK on delivery; this updates the PEER's subscription (no confirmation returns) - run 'espnowsubs' on that peer to verify.` |
| `espnowrequestmeta` |  | Request metadata from peer: 'espnowrequestmeta <name_or_mac>'. (async - updates cache; view with espnowdevices)<br/>`Usage: espnowrequestmeta <name_or_mac> Returns OK on delivery; the peer's name/room/zone/tags arrive later and update the local peer cache shown by 'espnowdevices' / 'espnowrooms' / 'espnowfind'.` |
| `espnowresetstats` | A | Reset ESP-NOW statistics counters. |
| `espnowroom` |  | Get/set device room: 'espnowroom [name]'.<br/>`Usage: espnowroom [Kitchen\|Bedroom\|...] espnowroom clear` |
| `espnowroomcmd` | A | Run command on all devices in a room; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)<br/>`Usage: espnowroomcmd <room> <target-user> <target-pass> <command> Credentials are checked ON EACH target device, not this one. Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'.` |
| `espnowrooms` |  | List rooms and their devices (aggregated from this node's cached peer metadata). |
| `espnowrouterstats` |  | Show message router statistics and metrics. |
| `espnowsaturation` |  | Show ESP-NOW link saturation: frames/sec, stream-queue depth, drops, ACK RTT (rolling 30s). |
| `espnowsaturationreset` |  | Clear the saturation rolling window (use before a stress test). |
| `espnowsend` |  | Send message to one peer, relayed over multiple hops if it is out of radio range: 'espnowsend [json] <name_or_mac> <message>'. Requires ESP-NOW encryption enabled. (async send; delivery only, no reply)<br/>`Usage: espnowsend [json] <name_or_mac> <message> Requires ESP-NOW encryption (set a mesh passphrase first); plaintext send was removed. Leading 'json' flag returns {schema,ok,msgId} for delivery-status polling. Returns OK on delivery; one-way message, no result comes back. Out-of-range peers are reached via relays when a route exists ('espnowmeshroutes'); a relayed message is split at ~130 chars per piece instead of ~200.` |
| `espnowsendfile` | A | Send file: 'espnowsendfile <name_or_mac> "<filepath>"'. (synchronous send; fails if the receiver rejects/cancels mid-transfer)<br/>`Usage: espnowsendfile <name_or_mac> "<filepath>" Blocks until the file is sent. 'success' means every chunk was transmitted and the receiver did not cancel; final storage is confirmed by the receiver's CRC check, which is not reported back here.` |
| `espnowsensorbroadcast` |  | Enable/disable all sensor ESP-NOW communication: 'espnowsensorbroadcast <on\|off>'.<br/>`Usage: espnowsensorbroadcast [on\|off]` |
| `espnowsensorbroadcastinterval` | A | Set sensor broadcast interval: <100-10000 ms><br/>`Usage: espnowsensorbroadcastinterval <100..10000>` |
| `espnowsensorreq` | A | TEST (Phase 1b): hand-send a SENSOR_REQ to a paired worker.<br/>`Usage: espnowsensorreq <MAC\|name> <mask> <mode 0=sub\|1=unsub\|2=oneshot> <intervalMs> <leaseMs>` |
| `espnowsensorstatus` |  | Show remote sensor cache (master) or worker streaming status (worker). |
| `espnowsensorstream` |  | Enable/disable sensor data streaming to master (worker only): 'espnowsensorstream <sensor> <on\|off>'. (local toggle; streamed data lands on the master's espnowsensorstatus)<br/>`Usage: espnowsensorstream <thermal\|tof\|imu\|gps\|input\|fmradio\|camera\|microphone\|rtc\|presence\|apds> <on\|off> Local on/off toggle; the worker then streams to the master, viewable there via 'espnowsensorstatus' / GET /api/sensors/remote.` |
| `espnowsessionopen` | A | Initiate SESSION handshake (requires prior espnowkeyex). (async - session goes ACTIVE later; check espnowsessions)<br/>`Usage: espnowsessionopen <name_or_mac> [<mesh>] Returns OK when SESSION_OPEN is sent; the session becomes ACTIVE when CONFIRM arrives - run 'espnowsessions'.` |
| `espnowsessions` |  | Show in-RAM session state (peer, sessionId, dir, age, counters). |
| `espnowsessionsend` | A | DIAGNOSTIC: send an AEAD-encrypted CHAT message over an active session (exercises the session-crypto path). NOT executed on the peer and returns no reply - to RUN a command use 'espnowremote'.<br/>`Usage: espnowsessionsend <name_or_mac> <message> Delivers an encrypted CHAT message (lands in the peer's espnowmessages). It is NOT command execution and no reply comes back. To run a command on the peer: espnowremote <target> <target-user> <target-pass> <command>.` |
| `espnowsetname` | A | Get/set device name: 'espnowsetname [name]'.<br/>`Usage: espnowsetname [<name>] (<=20 chars; letters, numbers, - and _ only)` |
| `espnowsetpassphrase` | S | Set encryption passphrase on a mesh: 'espnowsetpassphrase <mesh> <phrase>'.<br/>`Usage: espnowsetpassphrase <mesh> <passphrase> espnowsetpassphrase <mesh> clear` |
| `espnowstationary` |  | Get/set stationary flag: 'espnowstationary [0\|1]'.<br/>`Usage: espnowstationary [on\|off\|0\|1]` |
| `espnowstats` |  | Show ESP-NOW statistics (messages, errors, etc.). |
| `espnowstatus` |  | Show ESP-NOW status and configuration. |
| `espnowsubs` |  | List peers + their event-subscription bitmaps (what they want from us). |
| `espnowtagcmd` | A | Run command on all devices with a tag; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)<br/>`Usage: espnowtagcmd <tag> <target-user> <target-pass> <command> Credentials are checked ON EACH target device, not this one. Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'.` |
| `espnowtags` |  | Get/set device tags: 'espnowtags [tag1,tag2,...]'.<br/>`Usage: espnowtags stationary,thermal espnowtags clear` |
| `espnowtimestatus` |  | Show time synchronization status. |
| `espnowtimesync` |  | Broadcast NTP time to mesh (intended for the master; role not enforced). (async broadcast; delivery only, no reply) |
| `espnowtopoautorefresh` | A | Set auto refresh topology: <0\|1><br/>`Usage: espnowtopoautorefresh <0\|1>` |
| `espnowtopodiscoveryinterval` | A | Set topology discovery interval: <0-300000 ms><br/>`Usage: espnowtopodiscoveryinterval <0..300000>` |
| `espnowtoporesults` |  | Get topology discovery results. |
| `espnowunpair` | A | Unpair ESP-NOW device (also clears its crypto identity): 'espnowunpair <name_or_mac>'.<br/>`Usage: espnowunpair <name_or_mac>` |
| `espnowusersync` | S | Enable/disable user credential sync: 'espnowusersync [on\|off]'.<br/>`Usage: espnowusersync [on\|off]` |
| `espnowworker` |  | Configure worker status reporting: 'espnowworker [show\|on\|off\|interval <ms>\|fields <list>]'.<br/>`Usage: espnowworker [show\|on\|off\|interval <ms>\|fields <heap,rssi,thermal,imu>]` |
| `espnowworkerstatusinterval` | A | Set worker status interval: <5000-120000 ms><br/>`Usage: espnowworkerstatusinterval <5000..120000>` |
| `espnowzone` |  | Get/set device zone: 'espnowzone [name]'.<br/>`Usage: espnowzone [Counter\|Door\|Ceiling\|...] espnowzone clear` |
| `openespnow` | A | Initialize ESP-NOW communication. |
| `openstream` | A | Start streaming all output to ESP-NOW caller (admin, remote only). |
| `testcleanup` |  | Test cleanup of stale topology streams. |
| `testconcurrent` |  | Test concurrent topology streams (simulated). |
| `testfilelock` |  | Test file transfer lock acquire/release. |
| `teststreams` |  | Test topology stream management functions. |

## mqtt

> The MQTT subsystem connects the device to a broker, primarily to publish its sensor and system telemetry to Home Assistant via HA discovery. It is almost entirely configuration: broker host/port (mqttHost, mqttPort), credentials (mqttUser, mqttPassword), TLS mode and CA path, base/discovery topics, publish interval, and a long list of per-source publish toggles (mqttPublishThermal, mqttPublishIMU, and so on). These are persisted settings and most config commands are admin-only; after changing them, reconnect with closemqtt/openmqtt to apply to a live session. openmqtt and closemqtt start and stop the client, mqttstatus shows connection state, and mqttautostart controls whether it connects at boot. For inbound data, enable mqttSubscribeExternal with mqttSubscribeTopics; values received from those topics are cached and read back with mqttExternalSensors.

| Command | | Description |
| ------- | :-: | ----------- |
| `closemqtt` | A | Stop MQTT client |
| `mqttautostart` | A | MQTT auto-start [0\|1]<br/>`Usage: mqttautostart [0\|1]` |
| `mqttBaseTopic` | A | Base topic [topic\|auto]<br/>`Usage: mqttBaseTopic [topic\|auto]` |
| `mqttCACertPath` | A | CA cert path [path\|clear]<br/>`Usage: mqttCACertPath [path\|clear]` |
| `mqttclientenabled` | A | Enable/disable MQTT [0\|1]<br/>`Usage: mqttclientenabled [0\|1]` |
| `mqttDiscoveryPrefix` | A | HA discovery prefix [prefix]<br/>`Usage: mqttDiscoveryPrefix [prefix]` |
| `mqttExternalSensors` |  | List external sensor data (add 'json' for JSON output) |
| `mqttHost` | A | MQTT broker host [hostname]<br/>`Usage: mqttHost [hostname]` |
| `mqttPassword` | A | MQTT password [pass\|clear]<br/>`Usage: mqttPassword [password\|clear]` |
| `mqttPort` | A | MQTT broker port [port]<br/>`Usage: mqttPort [port]` |
| `mqttPublishAPDS` | A | Publish APDS [0\|1]<br/>`Usage: mqttPublishAPDS [0\|1]` |
| `mqttPublishGPS` | A | Publish GPS [0\|1]<br/>`Usage: mqttPublishGPS [0\|1]` |
| `mqttPublishIMU` | A | Publish IMU [0\|1]<br/>`Usage: mqttPublishIMU [0\|1]` |
| `mqttPublishInput` | A | Publish input device data [0\|1]<br/>`Usage: mqttPublishInput [0\|1]` |
| `mqttPublishIntervalMs` | A | Publish interval [ms]<br/>`Usage: mqttPublishIntervalMs [1000-300000]` |
| `mqttPublishPresence` | A | Publish presence [0\|1]<br/>`Usage: mqttPublishPresence [0\|1]` |
| `mqttPublishRTC` | A | Publish RTC [0\|1]<br/>`Usage: mqttPublishRTC [0\|1]` |
| `mqttPublishSystem` | A | Publish system [0\|1]<br/>`Usage: mqttPublishSystem [0\|1]` |
| `mqttPublishThermal` | A | Publish thermal [0\|1]<br/>`Usage: mqttPublishThermal [0\|1]` |
| `mqttPublishToF` | A | Publish ToF [0\|1]<br/>`Usage: mqttPublishToF [0\|1]` |
| `mqttPublishWiFi` | A | Publish WiFi [0\|1]<br/>`Usage: mqttPublishWiFi [0\|1]` |
| `mqttstatus` |  | Show MQTT status (add 'json' for JSON output) |
| `mqttSubscribeExternal` | A | External subscriptions [0\|1]<br/>`Usage: mqttSubscribeExternal [0\|1]` |
| `mqttSubscribeTopics` | A | Subscribe topics [topics]<br/>`Usage: mqttSubscribeTopics [topic1,topic2,...]` |
| `mqttTLSMode` | A | TLS mode [0\|1\|2]<br/>`Usage: mqttTLSMode [0\|1\|2\|none\|tls\|verify]` |
| `mqttUser` | A | MQTT username [user\|clear]<br/>`Usage: mqttUser [username\|clear]` |
| `openmqtt` | A | Start MQTT client |

## bluetooth

> The Bluetooth subsystem runs the device BLE stack in one of two mutually exclusive roles selected by blemode: server mode (the device advertises and a phone/app connects to it) or client mode (the device acts as a BLE central for Even G2 glasses; the even_g2 commands then apply). Switching modes tears down the other role automatically. In server mode, openble/closeble start and stop advertising, blesend pushes a one-off message and bleevent an event to the connected client, and blestream toggles periodic pushes as a bitmask of sensors/system/events (blestream on/off/sensors/system/events, plus interval) -- all of which require an active connection. An app-layer Secure Channel (X25519 + passphrase + ChaCha20-Poly1305, independent of BLE bonding) is configured with blesecret and required with blesecure; both are admin-only, as is blerequireauth. Boot reconnection to saved-MAC peers is per-peer via bleautoreconnect <name> [on|off] (see blepeers for names).

| Command | | Description |
| ------- | :-: | ----------- |
| `bleadv` | A | Start/stop/toggle BLE advertising [start\|stop\|toggle].<br/>`Usage: bleadv [start\|stop\|toggle]` |
| `bleautoreconnect` | A | Per-peer auto-reconnect (boot + mid-session drop): bleautoreconnect <name> [on\|off]. `blepeers` lists names.<br/>`Usage: bleautoreconnect <peer-name> [on\|off] on: reconnect at boot and reseek after unexpected drops (not after ringdisconnect/closeg2)` |
| `bleautostart` | A | Enable/disable BLE auto-start after boot [on\|off].<br/>`Usage: bleautostart [on\|off]` |
| `bledisconnect` | A | Disconnect current BLE client. |
| `bleevent` |  | Send event to BLE client: <event>.<br/>`Usage: bleevent <message>` |
| `bleinfo` |  | Show BLE configuration and settings. (add 'json' for JSON output) |
| `blemode` | A | Get/set BLE mode [server\|client].<br/>`Usage: blemode [server\|client]` |
| `blename` |  | Get/set BLE device name [name].<br/>`Usage: blename [name]` |
| `blepeers` |  | List all registered BLE peers and their state. |
| `bleread` |  | Read Bluetooth connection status. (add 'json' for JSON output) |
| `blerequireauth` | S | Enable/disable BLE authentication requirement [on\|off].<br/>`Usage: blerequireauth [on\|off]` |
| `blesecret` | S | Set/clear the BLE Secure Channel passphrase: blesecret <phrase\|clear>.<br/>`Usage: blesecret <passphrase\|clear>` |
| `blesecure` | A | Require app-layer BLE encryption [on\|off].<br/>`Usage: blesecure [on\|off]` |
| `blesend` |  | Send message to BLE client: <message>.<br/>`Usage: blesend <message>` |
| `blestatus` |  | Show Bluetooth connection status. (add 'json' for JSON output) |
| `blestream` |  | Control streaming: <on\|off\|sensors\|system\|events\|interval>.<br/>`Usage: blestream [on\|off\|sensors\|system\|events\|interval] \| interval <sensor_ms> <system_ms>` |
| `bletxpower` |  | Get/set BLE TX power [0-7].<br/>`Usage: bletxpower [0..7]` |
| `closeble` | A | Stop Bluetooth LE and deinitialize. |
| `openble` | A | Start Bluetooth LE and begin advertising. |

## filesystem

> Manages files and directories on the device internal LittleFS flash. Browse with files ["/path"] (add json for app/BLE, or files stats json for storage usage); create and remove with mkdir, rmdir, filecreate, and filedelete; view and rename with fileview and filerename. Critically, every path argument MUST be wrapped in double quotes, e.g. fileview "/system/notes" -- an unquoted or unmatched-quote path is rejected, and a leading slash is added automatically. For programmatic transfer, fileread and filewrite move data in chunks: fileread returns {success,size,offset,len,eof,enc,data} and you loop offset until eof, while filewrite is strictly sequential -- offset 0 truncates/creates the file, each later offset must equal the current file size, and passing final runs the post-save hooks. Access is permission-gated per path: system trees like /system are read-only (or browse-only) for admins, while user data is fully writable; logtier reports whether logs are writing to LittleFS or have spilled into SD overflow.

| Command | | Description |
| ------- | :-: | ----------- |
| `filecreate` | A | Create file: "<path>"<br/>`Usage: filecreate "<path>"` |
| `filedelete` | A | Delete file: "<path>" [confirm]<br/>`Usage: filedelete "<path>" [confirm]` |
| `fileread` | A | Read file chunk as JSON: "<path>" [offset] [len] [b64\|bin] |
| `filerename` | A | Rename file: "<oldpath>" "<newname>"<br/>`Usage: filerename "<oldpath>" "<newname>"` |
| `files` | A | List files ["path"] \| files json ["path"] \| files stats json ["path"] |
| `fileview` | A | View file (paged): "<path>" [page]<br/>`Usage: fileview "<path>" [page] (pages are ~3.5KB, split at line boundaries)` |
| `filewrite` | A | Write file chunk: "<path>" <offset> <b64chunk> [final] |
| `logtier` |  | Show current log storage tier (LittleFS vs SD overflow). |
| `mkdir` | A | Create directory: "<path>"<br/>`Usage: mkdir "<path>"` |
| `rmdir` | A | Remove directory: "<path>"<br/>`Usage: rmdir "<path>"` |

## sd

> Controls the optional microSD card, which mounts at /sd and serves as overflow/bulk storage (and is only compiled in on boards that wire a card-detect/CS pin). sdmount attempts to mount the card and sdunmount safely unmounts it; sdinfo shows the card type, size, and used/free space, and sddiag runs a raw-SPI hardware diagnostic to troubleshoot a card that will not mount. sdformat erases the entire card and reformats it as FAT32 and therefore requires sdformat confirm to proceed. Once mounted, file commands address the card through its /sd/... path prefix.

| Command | | Description |
| ------- | :-: | ----------- |
| `sddiag` |  | SD card hardware diagnostics |
| `sdformat` | S | Format SD card as FAT32 |
| `sdinfo` |  | Show SD card information |
| `sdmount` |  | Mount SD card |
| `sdunmount` | A | Unmount SD card |

## oled

> Drives the small SSD1306 OLED display: its lifecycle, the live screen contents, and persistent appearance settings. oledstart/oledstop (aliases openoled/closeoled) power the display task on and off, and oledstatus (alias oledread) reports its state. oledmode <mode> switches the live screen among the built-in views (menu, status, sensordata, thermal, network, mesh, gps, espnow, memory, off, and more); oledtext <message> shows custom text and oledanim <name>|fps <n> picks the animation -- both require the display to be running (run oledstart first) and neither persists across reboot. Separately, the oled* config commands write settings to flash immediately: oledbootmode and oleddefaultmode set the screen shown at boot and as the idle default, while oledbrightness <0-255>, oledflip, oledbootduration, oledupdateinterval, oledthermalscale, oledthermalcolormode, and oledenabled tune appearance and timing. oledrequireauth <0|1> (admin-only) controls whether a user must log in at the display before interacting with it.

| Command | | Description |
| ------- | :-: | ----------- |
| `closeoled` |  | Stop OLED display. |
| `oledanim` |  | Select animation: <name> or fps <1-60><br/>`Usage: oledanim <name> oledanim fps <1-60>` |
| `oledbootduration` |  | Boot animation duration (ms): <500-10000><br/>`Usage: oledbootduration <500..10000>` |
| `oledbootmode` |  | OLED boot mode: <logo\|status\|sensors\|thermal\|network\|mesh\|off><br/>`Usage: oledbootmode <logo\|status\|sensors\|thermal\|network\|mesh\|off>` |
| `oledbrightness` |  | Display brightness: <0-255><br/>`Usage: oledbrightness <0..255>` |
| `oledclear` |  | Clear OLED display. |
| `oleddefaultmode` |  | OLED default mode: <logo\|status\|sensors\|thermal\|network\|mesh\|off><br/>`Usage: oleddefaultmode <logo\|status\|sensors\|thermal\|network\|mesh\|off>` |
| `oledenabled` |  | Enable/disable OLED: <0\|1><br/>`Usage: oledenabled <0\|1>` |
| `oledflip` |  | Flip display 180°: [on\|off\|toggle]<br/>`Usage: oledflip [on\|off\|toggle]` |
| `oledmode` |  | Set display mode: <mode><br/>`Usage: oledmode <menu\|status\|sensordata\|sensorlist\|thermal\|network\|mesh\|gps\|text\|logo\|anim\|imuactions\|fmradio\|files\|automations\|espnow\|memory\|off> Example: oledmode memory Example: oledmode off` |
| `oledread` |  | Read OLED display status. (add 'json' for JSON output) |
| `oledrequireauth` | A | OLED auth requirement: <0\|1><br/>`Usage: oledrequireauth <0\|1>` |
| `oledstart` |  | Start OLED display. |
| `oledstatus` |  | Show OLED status. (add 'json' for JSON output) |
| `oledstop` |  | Stop OLED display. |
| `oledtext` |  | Set custom text: <message><br/>`Usage: oledtext <message>` |
| `oledthermalcolormode` |  | Thermal color mode: <3level\|grayscale><br/>`Usage: oledthermalcolormode <3level\|grayscale>` |
| `oledthermalscale` |  | Thermal image scale: <0.1-10.0><br/>`Usage: oledthermalscale <0.1..10.0>` |
| `oledupdateinterval` |  | Display update interval (ms): <10-1000><br/>`Usage: oledupdateinterval <10..1000>` |
| `openoled` |  | Start OLED display. |

## neopixel

> Controls the addressable RGB status LED (WS2812/NeoPixel). ledcolor <name> lights it a solid color from a fixed palette (red, green, blue, yellow, magenta, cyan, white, orange, purple, pink), and ledclear turns it off. ledeffect <fade|blink|pulse|strobe> [color] [color2] [duration 100-60000ms] runs an animated effect (defaults: red/blue, 3000 ms; ledeffect off clears it). These commands change the LED immediately and are not saved -- the persistent power-on brightness and startup animation live in the led settings module, not here. Note the effect call runs synchronously for its full duration before returning.

| Command | | Description |
| ------- | :-: | ----------- |
| `ledclear` |  | Turn off LED. |
| `ledcolor` |  | Set LED color: <color><br/>`Usage: ledcolor <red\|green\|blue\|yellow\|magenta\|cyan\|white\|orange\|purple\|pink>` |
| `ledeffect` |  | Run LED effect: <effect><br/>`Usage: ledeffect <fade\|pulse\|blink\|rainbow\|strobe\|off> [color] [color2] [duration 100..60000]` |

## led

> Configures the board onboard single LED -- its brightness and the one-shot effect played at startup. These are persistent settings written to flash, not live controls: ledbrightness <0-100> sets the global brightness, ledstartupenabled <0|1> toggles the boot effect, and ledstartupeffect <none|rainbow|pulse|fade|blink|strobe> with ledstartupcolor, ledstartupcolor2, and ledstartupduration <100-10000ms> define what plays on power-up. (The live, immediate RGB controls are the separate ledcolor/ledeffect commands in the neopixel module.)

| Command | | Description |
| ------- | :-: | ----------- |
| `ledbrightness` |  | Set LED brightness 0-100.<br/>`Usage: ledbrightness <0..100>` |
| `ledstartupcolor` |  | Set LED startup primary color (any of ~80 named colors or 'off'; unknown defaults to cyan).<br/>`Usage: ledstartupcolor <color name\|off> (any of ~80 named colors; unknown names default to cyan)` |
| `ledstartupcolor2` |  | Set LED startup secondary color (any of ~80 named colors or 'off'; unknown defaults to magenta).<br/>`Usage: ledstartupcolor2 <color name\|off> (any of ~80 named colors; unknown names default to magenta)` |
| `ledstartupduration` |  | Set LED startup effect duration in ms.<br/>`Usage: ledstartupduration <100..10000>` |
| `ledstartupeffect` |  | Set LED startup effect [none\|rainbow\|pulse\|fade\|blink\|strobe].<br/>`Usage: ledstartupeffect <none\|rainbow\|pulse\|fade\|blink\|strobe>` |
| `ledstartupenabled` |  | Enable/disable LED startup effect [0\|1].<br/>`Usage: ledstartupenabled <0\|1>` |

## servo

> The PCA9685 is a 16-channel I2C PWM driver used to control hobby servos (and generic PWM outputs) without tying up the ESP32 own timers. servo <channel> <angle> moves the servo on a channel to an angle, while pwm <channel> <value> [freq] writes a raw PWM duty (and optional frequency) for non-servo loads like LEDs or motor drivers. Because different servos expect different pulse ranges, servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name> stores a per-channel calibration that maps angles to the correct pulse widths (servolist shows the saved profiles), and servocalibrate <channel> opens an interactive mode to find those pulse limits by hand.

| Command | | Description |
| ------- | :-: | ----------- |
| `pwm` |  | Set PWM output: pwm <channel> <value> [freq].<br/>`Usage: pwm <channel> <value> [freq]` |
| `servo` |  | Control servo motor: servo <channel> <angle>.<br/>`Usage: servo <channel> <angle>` |
| `servocalibrate` |  | Enter calibration mode: servocalibrate <channel>.<br/>`Usage: servocalibrate <channel>` |
| `servolist` |  | List configured servo profiles. (add 'json' for JSON output) |
| `servoprofile` |  | Configure servo profile: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>.<br/>`Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>` |

## thermal

> The MLX90640 is a 32x24 (768-pixel) infrared thermal camera. openthermal starts it, thermalread reports the current frame min/max/avg temperature in Celsius, and closethermal stops it; thermalautostart [on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus (Wire1). The sensor runs in chess-pattern mode at 16-bit ADC resolution, with thermaltargetfps <1..8> selecting the device refresh rate. Display tuning is extensive: thermalpalettedefault picks the color map (grayscale, iron, rainbow, hot, or coolwarm), thermalrotation <0..3> rotates the image 0/90/180/270 degrees, and thermalupscalefactor plus the thermalinterpolation* commands smooth and enlarge the 32x24 grid for the web/OLED view. Frame readings are stabilized by temporal/EWMA smoothing, per-pixel outlier rejection, and an optional rolling min/max auto-scale that keeps the color scale from flickering; thermaldiag prints a hardware self-check.

| Command | | Description |
| ------- | :-: | ----------- |
| `closethermal` |  | Stop MLX90640 thermal sensor. |
| `openthermal` |  | Start MLX90640 thermal sensor. |
| `thermalautostart` |  | Enable/disable thermal auto-start after boot [on\|off]<br/>`Usage: thermalautostart [on\|off]` |
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

> The VL53L4CX is a laser time-of-flight ranging sensor that measures distance to nearby objects. opentof starts it, tofread reports the closest valid distance in centimeters (or full object data as JSON), and closetof stops it; tofautostart [on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus (Wire1). It is configured for LONG distance mode with a 200 ms timing budget, and is multi-target: each measurement can return up to four objects, which are signal-rate-gated and exponentially smoothed before the nearest valid one is reported. Most tunables are client-side visualization knobs rather than sensor settings: tofpollingms, toftransitionms, and tofmaxdistancemm shape the UI, tofstabilitythreshold sets how steady a reading must be, and tofdevicepollms controls how often the firmware reads the hardware.

| Command | | Description |
| ------- | :-: | ----------- |
| `closetof` |  | Stop VL53L4CX ToF sensor. |
| `opentof` |  | Start VL53L4CX ToF sensor. |
| `tofautostart` |  | Enable/disable ToF auto-start after boot [on\|off]<br/>`Usage: tofautostart [on\|off]` |
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
| `closeimu` |  | Stop BNO055 IMU sensor. |
| `imuactions` |  | Show IMU action detection state. |
| `imuautostart` |  | Enable/disable IMU auto-start after boot [on\|off]<br/>`Usage: imuautostart [on\|off]` |
| `imudevicepollms` | A | IMU device poll interval: <50..1000><br/>`Usage: imuDevicePollMs <50..1000>` |
| `imuewmafactor` | A | IMU EWMA smoothing: <0.0..1.0><br/>`Usage: imuewmafactor <0.0..1.0>` |
| `imuorientationcorrection` | A | IMU orientation correction: <0\|1><br/>`Usage: imuorientationcorrection <0\|1>` |
| `imuorientationmode` | A | IMU orientation mode: <0..8><br/>`Usage: imuorientationmode <0..8>` |
| `imupitchoffset` | A | IMU pitch offset in degrees (recommended -180..180)<br/>`Usage: imupitchoffset <degrees> (recommended -180..180)` |
| `imupollingms` | A | IMU UI polling interval: <50..2000><br/>`Usage: imupollingms <50..2000>` |
| `imuread` |  | Read IMU sensor data. (add 'json' for JSON output) |
| `imurolloffset` | A | IMU roll offset in degrees (recommended -180..180)<br/>`Usage: imurolloffset <degrees> (recommended -180..180)` |
| `imutransitionms` | A | IMU transition time: <0..1000><br/>`Usage: imutransitionms <0..1000>` |
| `imuwebmaxfps` | A | IMU web max FPS: <1..30><br/>`Usage: imuwebmaxfps <1..30>` |
| `imuyawoffset` | A | IMU yaw offset in degrees (recommended -180..180)<br/>`Usage: imuyawoffset <degrees> (recommended -180..180)` |
| `openimu` |  | Start BNO055 IMU sensor. |

## input

> Device-agnostic abstraction for the OLED input controller, which is either the Seesaw gamepad or the ANO rotary encoder -- chosen at compile time via INPUT_DEVICE_TYPE and mutually exclusive, so exactly one driver is present per firmware. These commands operate on whichever driver was built in: openinput starts it, closeinput stops it, inputautostart [on|off] persists boot auto-start, and inputdevicepollms <10-1000> sets the polling interval in milliseconds (default 90). Driver-specific debugging and tuning live in the gamepad and anoencoder modules; this module holds only the shared settings (poll interval and auto-start).

| Command | | Description |
| ------- | :-: | ----------- |
| `closeinput` |  | Stop the input device. |
| `inputautostart` |  | Enable/disable input device auto-start [on\|off]<br/>`Usage: inputautostart [on\|off]` |
| `inputdevicepollms` | A | Set input device poll interval ms [10-1000]<br/>`Usage: inputdevicepollms <10-1000>` |
| `openinput` |  | Start the input device (gamepad or ANO encoder). |

## gamepad

> Adafruit Seesaw I2C gamepad (analog joystick plus buttons), exposed here as a low-level debug interface for the raw device. The driver-agnostic open/close/autostart/poll commands live under the input module; the only gamepad-specific command is gamepadread, which polls the Seesaw once and dumps raw state -- joystick X/Y and the button bitmask -- attempting an on-demand connect with backoff if the device is not yet initialized. A background task polls the gamepad at roughly 50 ms and caches the latest reading for the OLED UI and sensor JSON. This module is mutually exclusive at build time with anoencoder; only one input device is compiled in per firmware (see input).

| Command | | Description |
| ------- | :-: | ----------- |
| `gamepadread` |  | Read Seesaw gamepad state (x/y/buttons). (add 'json' for JSON output) |

## anoencoder

> Adafruit ANO directional navigation rotary encoder on Seesaw I2C: a click wheel with a center IN press and UP/DOWN/LEFT/RIGHT buttons, used as the OLED navigation input. This module provides debug and remap commands; the actual open/close/autostart/poll lifecycle lives under the input module. anoencoderread dumps raw state -- encoder position, the currently selected rotary axis, and the button bitmask. Remap commands persist to settings: anoencoderi2caddr <1-127> changes the device address (reboot required), anoencoderinvert [on|off] reverses rotation direction, and anoencoderswapud / anoencoderswaplr [on|off|toggle] swap the UP/DOWN and LEFT/RIGHT button pairs. A polling task accumulates encoder detents so fast spins do not drop clicks. Mutually exclusive at build time with the Seesaw gamepad.

| Command | | Description |
| ------- | :-: | ----------- |
| `anoencoderi2caddr` | A | Set ANO I2C address [1-127]<br/>`Usage: anoencoderi2caddr <1-127>` |
| `anoencoderinvert` |  | Invert rotation direction [on\|off]<br/>`Usage: anoencoderinvert [on\|off]` |
| `anoencoderread` |  | Read ANO encoder state. (add 'json' for JSON output) |
| `anoencoderswaplr` |  | Swap LEFT/RIGHT buttons [on\|off\|toggle]<br/>`Usage: anoencoderswaplr [on\|off\|toggle]` |
| `anoencoderswapud` |  | Swap UP/DOWN buttons [on\|off\|toggle]<br/>`Usage: anoencoderswapud [on\|off\|toggle]` |

## apds

> The APDS9960 is a combined RGB color, proximity, and gesture sensor. openapds starts it (color sensing enabled by default), apdsread shows which modes are active plus the latest RGBC and proximity values, and closeapds stops it; apdsautostart [on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus (Wire1). Its three functions are toggled independently at runtime with apdsmode <color|proximity|gesture> [on|off] -- note that enabling gesture also turns proximity on, since the gesture engine needs it. Dedicated reads apdscolor, apdsproximity, and apdsgesture print a single sample on demand (gesture returns UP/DOWN/LEFT/RIGHT), and apdsdevicepollms sets how often the background task samples the hardware.

| Command | | Description |
| ------- | :-: | ----------- |
| `apdsautostart` |  | Enable/disable APDS auto-start after boot [on\|off]<br/>`Usage: apdsautostart [on\|off]` |
| `apdscolor` |  | Read APDS9960 color values. |
| `apdsgesture` |  | Read APDS9960 gesture. |
| `apdsmode` |  | Control APDS modes: apdsmode <color\|proximity\|gesture> [on\|off].<br/>`Usage: apdsmode <color\|proximity\|gesture> [<on\|off>]` |
| `apdsproximity` |  | Read APDS9960 proximity value. |
| `apdsread` |  | Read APDS9960 sensor status and data. (add 'json' for JSON output) |
| `closeapds` |  | Stop APDS9960 sensor. |
| `openapds` |  | Start APDS9960 sensor. |

## gps

> PA1010D I2C GPS receiver. Lifecycle: opengps starts the parser task, gpsread prints the current fix, and closegps stops it; gpsautostart [on|off] persists boot auto-start, and the module appears in help only when the chip is detected. gpsread reports fix yes/no, fix quality, satellite count, and (only when a fix is held) latitude/longitude in degrees, altitude in meters, speed in knots, heading angle, plus GPS UTC time and date; with no fix it shows just quality and satellites. The distinctive gpslog [interval_ms] command is a one-shot setup that turns on gpsAutoStart, configures sensorlog to format=track with sensors=gps, then immediately starts both the GPS sensor and the logger to record a track (default 1000 ms, minimum 100 ms) that persists across reboots.

| Command | | Description |
| ------- | :-: | ----------- |
| `closegps` |  | Stop PA1010D GPS module. |
| `gpsautostart` |  | Enable/disable GPS auto-start after boot [on\|off]<br/>`Usage: gpsautostart [on\|off]` |
| `gpslog` |  | Set up and start GPS track logging now (persists across boots). Usage: gpslog [interval_ms]<br/>`Usage: gpslog [interval_ms] Sets gpsAutoStart, sensorlog format=track, sensors=gps, and autostart, then starts both the GPS sensor and sensor logging immediately. interval_ms: log interval in ms (default 1000, min 100) Example: gpslog (1-second logging) gpslog 500 (500ms logging)` |
| `gpsread` |  | Read GPS location and time data. (add 'json' for JSON output) |
| `opengps` |  | Start PA1010D GPS module. |

## fmradio

> RDA5807M I2C FM radio receiver. Lifecycle: openfmradio starts it, fmradioread reports status, and closefmradio stops it; fmradioautostart [on|off] persists boot auto-start and the module shows in help only when detected. fmradiotune <freq> accepts either MHz (e.g. 103.9) or 10 kHz integer units (e.g. 10390) -- values under 200 are read as MHz, otherwise as raw units -- and rejects anything outside 76.0-108.0 MHz; tuning clears any decoded RDS station name and text. fmradioseek [up|down] STARTS a hunt for the next station (no band wrap) and returns immediately -- the radio task finalizes the result within a few seconds; watch 'fmradioread' (JSON field "seeking") or the OLED FM screen. fmradiovolume <0-15> sets output level, and fmradiomute / fmradiounmute toggle audio. The OLED FM screen drives all of this from the gamepad: L/R tune, Up/Down seek, A mute, Y volume, X power.

| Command | | Description |
| ------- | :-: | ----------- |
| `closefmradio` |  | Stop FM Radio sensor. |
| `fmradioautostart` |  | Enable/disable FM Radio auto-start after boot [on\|off]<br/>`Usage: fmradioautostart [on\|off]` |
| `fmradiomute` |  | Mute audio |
| `fmradioread` |  | Read FM Radio status. (add 'json' for JSON output) |
| `fmradioseek` |  | Start seeking the next station [up\|down] (async; 'fmradioread' shows the result)<br/>`Usage: fmradioseek [up\|down]` |
| `fmradiotune` |  | Tune to frequency: <freq><br/>`Usage: fmradiotune <frequency> (e.g., 103.9 or 10390)` |
| `fmradiounmute` |  | Unmute audio |
| `fmradiovolume` |  | Set volume: <0-15><br/>`Usage: fmradiovolume <0-15>` |
| `openfmradio` |  | Start FM Radio sensor. |

## rtc

> DS3231 precision I2C real-time clock with battery backup and an on-chip temperature sensor. Lifecycle: openrtc starts the RTC task, rtcread [status|temp] reads the clock (or die temperature), and closertc stops it; rtcautostart [on|off] persists boot auto-start and the module appears in help only when detected. rtcset accepts either "YYYY-MM-DD HH:MM:SS" or a bare Unix timestamp and writes it to the chip, computing day-of-week automatically. rtcsync [to|from] moves time between the RTC and the system clock: to (the default) copies RTC -> system, from copies system -> RTC (use this after an NTP sync to persist accurate time into the battery-backed chip). Setting the time via rtcset or rtcsync from also marks the RTC as calibrated so later boots trust it as a time source.

| Command | | Description |
| ------- | :-: | ----------- |
| `closertc` |  | Stop DS3231 RTC sensor. |
| `openrtc` |  | Start DS3231 RTC sensor. |
| `rtcautostart` |  | Enable/disable RTC auto-start after boot [on\|off]<br/>`Usage: rtcautostart [on\|off]` |
| `rtcread` |  | Read RTC status [status\|temp]<br/>`Usage: rtcread [status\|temp] [json]` |
| `rtcset` | A | Set RTC time: <datetime\|timestamp><br/>`Usage: rtcset YYYY-MM-DD HH:MM:SS or rtcset <unix_timestamp>` |
| `rtcsync` | A | Sync time: [to\|from]<br/>`Usage: rtcsync [to\|from] (to=RTC->system, from=system->RTC)` |

## presence

> The STHS34PF80 is an infrared presence and motion sensor that detects warm bodies without contact. openpresence starts it, presenceread reports ambient temperature plus presence, motion, and temperature-shock values (each with a DETECTED flag), and closepresence stops it; presenceautostart [on|off] persists launching it at boot. The sensor is initialized at an 8 Hz output data rate with block-data-update enabled, and its on-chip presence/motion/ambient-shock detection engines provide the detection flags directly; presencestatus prints connection and data-validity diagnostics, and presencedevicepollms controls the hardware read interval.

| Command | | Description |
| ------- | :-: | ----------- |
| `closepresence` |  | Stop STHS34PF80 sensor. |
| `openpresence` |  | Start STHS34PF80 IR presence/motion sensor. |
| `presenceautostart` |  | Enable/disable presence auto-start after boot [on\|off]<br/>`Usage: presenceautostart [on\|off]` |
| `presenceread` |  | Read STHS34PF80 presence/motion/temperature data. (add 'json' for JSON output) |
| `presencestatus` |  | Show STHS34PF80 sensor status. (add 'json' for JSON output) |

## camera

> Driver and CLI for the attached DVP camera sensor (OV2640/OV3660 class). The sensor must be powered up first with opencamera before any capture or tuning command works (closecamera stops it); cameraread and cameradump report status and all current sensor register values. Three distinct capture paths exist: cameracapture grabs one JPEG frame into RAM and reports its size only, camerasave captures and writes a frame to storage (LittleFS, SD, or both, per camerastoragelocation, into cameracapturefolder), and cameratiny produces a 160x120 frame small enough for a single ESP-NOW packet; camerarecord start|stop records MJPEG-AVI video and requires an SD card. Resolution and image controls (camerares/cameraframesize, cameraquality, camerafps) and a large set of sensor-tuning commands (brightness/contrast/saturation, white balance, exposure/AEC, gain/AGC, special effects, mirror/flip/rotate, plus raw camerareg register writes) adjust the live image. Automation settings (cameraautostart, cameraautocapture/cameraautocaptureinterval, camerasendaftercapture/cameratargetdevice) drive timed capture and optional ESP-NOW delivery to a named peer.

| Command | | Description |
| ------- | :-: | ----------- |
| `cameraaec` | A | Auto exposure: <on\|off><br/>`Usage: cameraaec <on\|off\|1\|0\|true\|auto>` |
| `cameraaec2` | A | Alt AEC algorithm: <on\|off><br/>`Usage: cameraaec2 <on\|off>` |
| `cameraaecvalue` | A | Exposure value: <0-1200><br/>`Usage: cameraaecvalue <0..1200>` |
| `cameraagc` | A | Auto gain: <on\|off><br/>`Usage: cameraagc <on\|off\|1\|0\|true\|auto>` |
| `cameraagcgain` | A | Gain value: <0-30><br/>`Usage: cameraagcgain <0..30>` |
| `cameraautocapture` | A | Auto-capture: <on\|off><br/>`Usage: cameraautocapture <on\|off\|1\|0\|true>` |
| `cameraautocaptureinterval` | A | Auto-capture: <sec><br/>`Usage: cameraautocaptureinterval <10..3600>` |
| `cameraautostart` | A | Auto-start: <on\|off><br/>`Usage: cameraautostart <on\|off\|1\|0\|true\|false>` |
| `cameraawbgain` | A | AWB gain: <on\|off><br/>`Usage: cameraawbgain <on\|off>` |
| `camerabpc` | A | Black pixel correction: <on\|off><br/>`Usage: camerabpc <on\|off>` |
| `camerabrightness` |  | Set brightness: <-2..2><br/>`Usage: camerabrightness <-2..2>` |
| `cameracapture` |  | Capture a single frame |
| `cameracapturefolder` | A | Photo folder: <path><br/>`Usage: cameracapturefolder <path>` |
| `cameracolorbar` | A | Color bar test pattern: <on\|off><br/>`Usage: cameracolorbar <on\|off>` |
| `cameracontrast` |  | Set contrast: <-2..2><br/>`Usage: cameracontrast <-2..2>` |
| `cameradcw` | A | Downsize crop window: <on\|off><br/>`Usage: cameradcw <on\|off>` |
| `cameradenoise` | A | Set denoise level: <0-8><br/>`Usage: cameradenoise <0..8>` |
| `cameradump` |  | Print all current sensor values |
| `cameraeffect` | A | Special effect: <0-6><br/>`Usage: cameraeffect <0..6> (0=None,1=Negative,2=Grayscale,3=Red,4=Green,5=Blue,6=Sepia)` |
| `cameraexposure` | A | Set AE level: <-2..2><br/>`Usage: cameraexposure <-2..2> (negative=darker)` |
| `camerafps` | A | Camera FPS: <1-20><br/>`Usage: camerafps <1..20>` |
| `cameraframesize` | A | Set resolution by index: <0-10><br/>`Usage: cameraframesize <0..10> (0-5: QVGA..UXGA, 6-10: 96x96/QQVGA/QCIF/HQVGA/240x240)` |
| `camerafx` |  | Set bri/con/sat together: <bri> <con> <sat> (-2..+2 each)<br/>`Usage: camerafx <bri> <con> <sat> (-2..+2 each)` |
| `cameragainceiling` | A | Gainceiling: <0-6> (2X..128X)<br/>`Usage: cameragainceiling <0..6> (2X..128X)` |
| `cameragamma` | A | Raw gamma: <on\|off><br/>`Usage: cameragamma <on\|off>` |
| `camerahmirror` |  | Horizontal mirror: <on\|off><br/>`Usage: camerahmirror <on\|off\|1\|0\|true>` |
| `cameralenc` | A | Lens shading correction: <on\|off><br/>`Usage: cameralenc <on\|off>` |
| `cameramaxstoredimages` | A | Max stored: <0-1000><br/>`Usage: cameramaxstoredimages <0..1000> (0=unlimited)` |
| `cameraquality` |  | Set JPEG quality: <0-63><br/>`Usage: cameraquality <0..63> (lower = better quality, larger file)` |
| `cameraread` |  | Read camera status |
| `camerarecord` |  | Start/stop MJPEG-AVI recording (SD only): <start\|stop><br/>`Usage: camerarecord <start\|stop\|1\|0>` |
| `camerareg` | A | Direct register write: <addr_hex> <mask_hex> <value_hex><br/>`Usage: camerareg <addr_hex> <mask_hex> <value_hex> (example: camerareg 0x3824 0x1f 0x04)` |
| `camerares` |  | Set camera resolution: <res><br/>`Usage: camerares <96x96\|qqvga\|qcif\|hqvga\|240x240\|qvga\|cif\|vga\|svga\|xga\|sxga\|uxga>` |
| `camerarotate` |  | Rotate 180°: <on\|off><br/>`Usage: camerarotate <on\|off\|1\|0\|true\|180>` |
| `camerasaturation` |  | Set saturation: <-2..2><br/>`Usage: camerasaturation <-2..2>` |
| `camerasave` |  | Save current frame to storage |
| `camerasendaftercapture` | A | Send after capture: <on\|off><br/>`Usage: camerasendaftercapture <on\|off\|1\|0\|true>` |
| `camerasharpness` | A | Set sharpness: <-2..2><br/>`Usage: camerasharpness <-2..2> (OV3660 only)` |
| `camerastoragelocation` | A | Storage location: <0-2><br/>`Usage: camerastoragelocation <0..2> (0=LittleFS,1=SD,2=Both)` |
| `cameratargetdevice` | A | Target device: <name><br/>`Usage: cameratargetdevice <name>` |
| `cameratiny` |  | Capture tiny frame for ESP-NOW |
| `cameravflip` |  | Vertical flip: <on\|off><br/>`Usage: cameravflip <on\|off\|1\|0\|true>` |
| `cameravideodelete` | A | Delete recording: "<filename>"<br/>`Usage: cameravideodelete "<filename>"` |
| `cameravideolist` |  | List AVI recordings on SD (add 'json' for JSON output) |
| `camerawb` | A | White balance mode: <0-4><br/>`Usage: camerawb <0..4> (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)` |
| `camerawhitebal` | A | AWB master: <on\|off><br/>`Usage: camerawhitebal <on\|off>` |
| `camerawpc` | A | White pixel correction: <on\|off><br/>`Usage: camerawpc <on\|off>` |
| `closecamera` |  | Stop camera sensor. |
| `opencamera` |  | Start camera sensor. |

## microphone

> Driver and CLI for the microphone — the on-board PDM mic and/or the G2 glasses mic, selected with micsource. The mic must be started with openmic before reads or recording (closemic stops it); commands that need the running mic return a use-openmic-first error otherwise. miclevel returns the current audio level (percent; add json for structured output) and micviz shows a live level meter until a key is pressed. micrecord start [vad <ms>] [trim]|stop records audio to a WAV file; owner-correlated capture uses micrecord startid|statusid|stopid with a strict 16-hex ID and optional discard. miclist lists saved recordings, while micdelete and micdeleteid remove manual or exact owner-scoped results. Audio format is configured with micsamplerate (8000-48000), micgain (0-100), and micbitdepth (16 or 32), each usable as a getter with no argument; micautostart on|off persists whether the mic powers up automatically at boot.

| Command | | Description |
| ------- | :-: | ----------- |
| `closemic` |  | Stop microphone sensor. |
| `micautostart` |  | Enable/disable microphone auto-start after boot [on\|off]<br/>`Usage: micautostart [on\|off]` |
| `micbitdepth` |  | Get/set bit depth.<br/>`Usage: micbitdepth [16\|32]` |
| `micdelete` | A | Delete recording(s).<br/>`Usage: micdelete "<filename>" \| micdelete all` |
| `micdeleteid` | A | Delete one owner-correlated recording.<br/>`Usage: micdeleteid <16hex> "<filename>"` |
| `micgain` |  | Get/set microphone gain.<br/>`Usage: micgain [0-100]` |
| `miclevel` |  | Get current audio level.<br/>`Usage: miclevel [json]` |
| `miclist` |  | List saved recordings.<br/>`Usage: miclist [json]` |
| `micread` |  | Read microphone sensor status.<br/>`Usage: micread [json]` |
| `micrecord` |  | Start/stop recording to WAV file (bare = show recording status).<br/>`Usage: micrecord [start [vad <200..10000>] [trim] \| stop \| 1 \| 0] micrecord startid <16hex> [vad <200..10000>] [trim] micrecord statusid <16hex> micrecord stopid <16hex> [discard]` |
| `micsamplerate` |  | Get/set sample rate.<br/>`Usage: micsamplerate [8000-48000]` |
| `micsource` |  | Get/set mic source: onboard PDM or G2 glasses.<br/>`Usage: micsource [auto\|pdm\|g2]` |
| `micviz` |  | Real-time audio level visualizer.<br/>`Usage: micviz (press any key to stop)` |
| `openmic` |  | Start microphone sensor. |
| `voicefetch` |  | Stream a recording to the UART host as binary frames (CM5 bulk pull).<br/>`Usage: voicefetch "<path>" - path must be under /recordings or /sd/recordings. Sends META+AUDIO frames on the UART link, then replies with byte/frame totals and crc16.` |

## dictation

> Host half of the OLED keyboard's mic page. The wearer arms a capture from the keyboard (whichever mic the source layer has resolved — on-board PDM or the G2 glasses), the firmware records it with VAD auto-stop and pushes a dictate_request <id> <path> event on the authenticated UART link, and the CM5 host voicefetches the WAV, transcribes it, and returns the words with dictate result <id> <text> — the text runs to end of line, so it needs no quoting or escaping. dictate fail <id> [reason] reports a transcription that could not be produced, and dictate status reports the current state, active mic source and elapsed time. The id is single-use and bound to the display session that armed it, so a transcript can never land in a field belonging to a different user; the recording is deleted once its words are delivered. This device cannot transcribe speech on its own, so the whole command family is UART-only and inert without a logged-in host.

| Command | | Description |
| ------- | :-: | ----------- |
| `dictate` |  | Deliver a host transcript into the on-device text field.<br/>`Usage: dictate status dictate result <16hex> <text> - text runs to end of line dictate fail <16hex> [reason]` |

## edgeimpulse

> On-device machine-learning image inference using TensorFlow Lite Micro models exported from Edge Impulse. Two things must be in place before inference: a model must be loaded with eimodelload "<file>" (models live in the models directory on LittleFS; eimodellist/eimodelinfo/eimodelunload manage them) and inference must be enabled with eienable 1 (which also initializes the inference buffers). eidetect runs a single detection on a live camera frame and so additionally requires the camera to be opened (see opencamera), returning detected objects with confidence and bounding boxes as JSON; eifile "<path>" runs the same inference against a stored JPEG instead of the camera. eicontinuous 1 runs detection repeatedly in the background, eiconfidence <0.0-1.0> sets the minimum confidence to report, and eistatus shows current state. The eitrack family (eitrackenable, eitrackstatus, eitrackclear) adds cross-frame state tracking of detected objects on top of raw detections.

| Command | | Description |
| ------- | :-: | ----------- |
| `ei` |  | Edge Impulse ML inference commands.<br/>`Usage: ei <subcommand>` |
| `eiconfidence` |  | Set minimum detection confidence.<br/>`Usage: eiconfidence <0.0-1.0>` |
| `eicontinuous` |  | Start/stop continuous inference mode.<br/>`Usage: eicontinuous <0\|1>` |
| `eidetect` |  | Run single object detection inference.<br/>`Usage: eidetect` |
| `eienable` |  | Enable/disable Edge Impulse inference.<br/>`Usage: eienable <0\|1>` |
| `eifile` |  | Run inference on stored JPEG image.<br/>`Usage: eifile "<path>"` |
| `eimodel` |  | Model management commands.<br/>`Usage: eimodel <subcommand>` |
| `eimodelinfo` |  | Show loaded model information.<br/>`Usage: eimodelinfo` |
| `eimodellist` |  | List available .tflite models.<br/>`Usage: eimodellist` |
| `eimodelload` |  | Load a TFLite model from LittleFS.<br/>`Usage: eimodelload "<filename>"` |
| `eimodelunload` |  | Unload the current model.<br/>`Usage: eimodelunload` |
| `eistatus` |  | Show Edge Impulse status.<br/>`Usage: eistatus` |
| `eitrack` |  | State tracking commands.<br/>`Usage: eitrack <subcommand>` |
| `eitrackclear` |  | Clear all tracked objects.<br/>`Usage: eitrackclear` |
| `eitrackenable` |  | Enable/disable state tracking.<br/>`Usage: eitrackenable <0\|1>` |
| `eitrackstatus` |  | Show currently tracked objects.<br/>`Usage: eitrackstatus` |

## espsr

> Offline voice control built on Espressif ESP-SR: a WakeNet wake-word stage gates a MultiNet command-phrase recognizer, so the device waits for the wake word and then listens for a known command phrase. Note that srenable/sr enable only reports the compile-time build flag and cannot toggle the feature at runtime; the real lifecycle commands are opensr/srstart to start the recognition pipeline and closesr/srstop to stop it. Starting the pipeline also arms voice command execution as the current authenticated user (and stopping it disarms); arming can be managed directly with voicearm/voicedisarm/voicestatus, and recognized phrases only execute commands while armed. The command vocabulary is managed with the srcmds family (list/add/del/clear plus save/reload to an SD file and srcmdssync to import phrases from the CLI registry). Recognition is tuned through srconfidence, srtimeout, the srtuning* audio controls (gain, AGC, VAD, filters), and srdebug* telemetry; the mic feed follows the device-wide source (set with `micsource auto|pdm|g2` — onboard PDM or the G2 glasses left-temple mic), and the srsnip* commands capture audio snippets (by default on the wake word) for debugging.

| Command | | Description |
| ------- | :-: | ----------- |
| `closesr` |  | Stop ESP-SR pipeline.<br/>`Usage: closesr` |
| `opensr` |  | Start ESP-SR pipeline and arm voice as the current user.<br/>`Usage: opensr` |
| `sr` |  | ESP-SR speech recognition commands.<br/>`Usage: sr <enable\|start\|stop\|status\|stack\|cmds\|debug\|confidence\|timeout\|tuning\|accept\|dyngain\|raw\|autotune\|snip>` |
| `sraccept` |  | Configure target acceptance policy (gap acceptance).<br/>`Usage: sraccept [on\|off\|floor <0.0-1.0>\|gap <0.0-1.0>\|speech <0\|1>]` |
| `srautotune` |  | Auto-cycle through gain configurations to find best settings.<br/>`Usage: srautotune [start\|stop\|status]` |
| `srcmds` | A | Manage MultiNet command phrases.<br/>`Usage: srcmds <list\|add\|del\|clear\|save\|reload\|sync>` |
| `srcmdsadd` | A | Add or update a MultiNet command.<br/>`Usage: srcmdsadd <id> <phrase>` |
| `srcmdsclear` | A | Clear all MultiNet commands.<br/>`Usage: srcmdsclear confirm` |
| `srcmdsdel` | A | Delete a MultiNet command (by phrase or id).<br/>`Usage: srcmdsdel <phrase\|id>` |
| `srcmdslist` | A | List current MultiNet commands.<br/>`Usage: srcmdslist` |
| `srcmdsreload` | A | Reload commands from SD file.<br/>`Usage: srcmdsreload` |
| `srcmdssave` | A | Save current commands to SD file.<br/>`Usage: srcmdssave` |
| `srcmdssync` | A | Sync voice commands from CLI registry.<br/>`Usage: srcmdssync` |
| `srconfidence` |  | Get/set command confidence threshold.<br/>`Usage: srconfidence [<0.0-1.0> \| category <0.0-1.0> \| target <0.0-1.0>]` |
| `srdebug` |  | SR debug/telemetry commands.<br/>`Usage: srdebug <level\|telem\|stats\|reset>` |
| `srdebuglevel` |  | Set debug verbosity (0-4).<br/>`Usage: srdebuglevel [0-4]` |
| `srdebugreset` |  | Reset SR debug counters.<br/>`Usage: srdebugreset` |
| `srdebugstats` |  | Print current SR statistics.<br/>`Usage: srdebugstats` |
| `srdebugtelem` |  | Set periodic telemetry interval (ms, 0=off).<br/>`Usage: srdebugtelem [ms]` |
| `srdyngain` |  | Configure dynamic gain normalization (MultiNet input only).<br/>`Usage: srdyngain [on\|off\|min <0.1-10>\|max <0.1-10>\|target <1000-30000>\|alpha <0.0-1.0>\|reset]` |
| `srenable` | A | ESP-SR enable is a compile-time flag (cannot be toggled at runtime).<br/>`Usage: srenable (informational; ESP-SR is set at compile time, any 0\|1 argument is ignored)` |
| `srraw` |  | Toggle raw output mode (shows all MultiNet hypotheses).<br/>`Usage: srraw [on\|off]` |
| `srsnip` |  | Voice snippet capture commands.<br/>`Usage: srsnip <on\|off\|start\|stop\|status\|config>` |
| `srsnipconfig` |  | Configure snippet capture params.<br/>`Usage: srsnipconfig [pre_ms\|max_ms\|dest] [value]` |
| `srsnipoff` |  | Disable auto-capture.<br/>`Usage: srsnipoff` |
| `srsnipon` |  | Enable auto-capture on wake word.<br/>`Usage: srsnipon` |
| `srsnipstart` |  | Start manual snippet capture now.<br/>`Usage: srsnipstart` |
| `srsnipstatus` |  | Show snippet capture status.<br/>`Usage: srsnipstatus` |
| `srsnipstop` |  | Stop manual snippet capture and save.<br/>`Usage: srsnipstop` |
| `srstack` |  | Show sr_task stack high-water mark (run after voice stress test).<br/>`Usage: srstack` |
| `srstart` |  | Start ESP-SR pipeline and arm voice as the current user.<br/>`Usage: srstart` |
| `srstatus` |  | Show ESP-SR status.<br/>`Usage: srstatus` |
| `srstop` |  | Stop ESP-SR pipeline.<br/>`Usage: srstop` |
| `srtimeout` |  | Get/set command listening timeout.<br/>`Usage: srtimeout [1000-30000]` |
| `srtuning` |  | Show/set audio tuning parameters.<br/>`Usage: srtuning [<gain\|agc\|vad\|swgain\|filters> <value>] (bare = show status)` |
| `srtuningagc` |  | Set AGC mode (0=off, 1-3=levels).<br/>`Usage: srtuningagc <0-3>` |
| `srtuningfilters` |  | Toggle audio filters (high-pass + pre-emphasis).<br/>`Usage: srtuningfilters <on\|off>` |
| `srtuninggain` |  | Set AFE linear gain (0.1-10.0).<br/>`Usage: srtuninggain <0.1-10.0>` |
| `srtuningswgain` |  | Set software gain (1.0-50.0) by updating shared micgain.<br/>`Usage: srtuningswgain <1.0-50.0>` |
| `srtuningvad` |  | Set VAD sensitivity (0-4).<br/>`Usage: srtuningvad <0-4>` |
| `voicearm` |  | Arm voice command execution as the current authenticated user.<br/>`Usage: voicearm` |
| `voicecancel` |  | Cancel current voice command sequence. |
| `voicedisarm` |  | Disarm voice command execution.<br/>`Usage: voicedisarm` |
| `voicehelp` |  | Show available voice options for current state. |
| `voicestatus` |  | Show voice arming status.<br/>`Usage: voicestatus` |

## i2c

> The i2c module configures and diagnoses up to two I2C buses and the sensor device registry. There are two buses with a deliberate naming convention: bus 0 is I2C1 (Arduino Wire1, the primary STEMMA QT / sensor bus) and bus 1 is I2C2 (Wire, the optional secondary bus); each has its own enable flag and SDA/SCL pin settings, and bus/pin changes require a reboot. Each sensor can be routed to either bus with a per-device command (oledBus, gpsBus, rtcBus, imuBus, thermalBus, tofBus, etc.), all taking 0 or 1 and needing a reboot. Discovery and diagnostics: i2cscan dumps raw addresses found on each active bus; detect reports configured-vs-present hardware and detect apply (admin) auto-enables newly detected cheap devices; i2cmetrics/i2cstats/i2chealth show bus performance, error counters, and per-device health. Bus recovery: i2cpause/i2cresume stop and restart sensor polling, i2creset does a pause-recover-resume cycle, and i2crecover <address> clears a single device degraded state. The device registry is exposed via sensors [filter|json], sensorinfo <name>, devices, discover, and devicefile; sensorautostart [sensor] [on|off] controls which sensors start polling automatically at boot.

| Command | | Description |
| ------- | :-: | ----------- |
| `apdsbus` | A | Route APDS9960 gesture to bus: <0\|1> (reboot required)<br/>`Usage: apdsBus <0\|1>` |
| `detect` |  | Detect hardware: scan I2C buses, diff vs. configured features.<br/>`Usage: detect [apply] detect - read-only report (present/enabled/missing) detect apply - auto-enable cheap detected devices (admin; reboot for some)` |
| `devicefile` |  | Show device registry JSON file. |
| `devices` |  | Show discovered I2C device registry. (add 'json' for JSON output) |
| `discover` |  | Re-scan and register I2C devices. |
| `fmradiobus` | A | Route RDA5807 FM radio to bus: <0\|1> (reboot required)<br/>`Usage: fmRadioBus <0\|1>` |
| `fuelgaugebus` | A | Route MAX17048 fuel gauge to bus: <0\|1> (reboot required)<br/>`Usage: fuelGaugeBus <0\|1>` |
| `gpsbus` | A | Route PA1010D GPS to bus: <0\|1> (reboot required)<br/>`Usage: gpsBus <0\|1>` |
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
| `imubus` | A | Route BNO055 IMU to bus: <0\|1> (reboot required)<br/>`Usage: imuBus <0\|1>` |
| `inputbus` | A | Route input device to bus: <0\|1> (reboot required)<br/>`Usage: inputBus <0\|1>` |
| `oledbus` | A | Route OLED to bus: <0\|1> (reboot required)<br/>`Usage: oledBus <0\|1>` |
| `presencebus` | A | Route STHS34PF80 presence to bus: <0\|1> (reboot required)<br/>`Usage: presenceBus <0\|1>` |
| `rtcbus` | A | Route DS3231 RTC to bus: <0\|1> (reboot required)<br/>`Usage: rtcBus <0\|1>` |
| `sensorautostart` | A | Sensor auto-start: [sensor] [on\|off]<br/>`Usage: sensorautostart [sensor] [on\|off] sensorautostart all [on\|off] Sensors: thermal, tof, imu, gps, fmradio, apds, input` |
| `sensorinfo` |  | Sensor details: <name><br/>`Usage: sensorinfo <sensor_name> Example: sensorinfo BNO055` |
| `sensors` |  | List I2C sensors [filter]<br/>`Usage: sensors [filter] - filter by name, description, or manufacturer sensors json [brief] - live state (+readings; 'brief' = state only, no data) Example: sensors temperature, sensors json brief` |
| `servobus` | A | Route PCA9685 servo to bus: <0\|1> (reboot required)<br/>`Usage: servoBus <0\|1>` |
| `thermalbus` | A | Route MLX90640 thermal to bus: <0\|1> (reboot required)<br/>`Usage: thermalBus <0\|1>` |
| `tofbus` | A | Route VL53L4CX ToF to bus: <0\|1> (reboot required)<br/>`Usage: tofBus <0\|1>` |

## automation

> The automation module runs saved jobs (stored in automations.json) that execute one or more CLI commands on a schedule, condition, or system event. Every automation has one of four trigger types: atTime (fires daily at time=HH:MM, optionally limited to days=Mon,Tue,...), afterDelay (fires once after delayms milliseconds), interval (fires repeatedly every intervalms milliseconds), or event (fires when a system event occurs: on=<kind> with an optional match=<text> filter against the event's subject/detail — run 'events' to see recent kinds like peer_online, text_rx, battery_low, login_fail); jobs can also carry runatboot=1 to fire at startup. The primary entry point is automation <subcommand> (list, add, enable, disable, delete, run, trigger, sanitize, recompute) with single-word aliases automationlist, automationadd, automationrun, and automationtrigger. Note the important distinction: automationrun id=<id> executes a job commands immediately, whereas automationtrigger id=<id> only arms an afterDelay/manual timer so it fires after its delay; and automation system enable|disable|status is the global master switch that gates whether the scheduler runs at all, independent of each job own enabled flag. Jobs may also include an optional condition expression, and conditional commands use an IF <expr> THEN <command> [ELSE <command>] form (e.g. IF temp>75 THEN ledcolor red). Conditions can check sensors (temp, distance, light, motion), time (time, hour, day, ntp), system state (battery, heap, psram, fsfree, uptime, chiptemp), connectivity (wifi, rssi, peers, ble), ESP-NOW/bond (espnow, bond_mode, bond_paired, bond_online, bond_synced, bond_role, bond_rssi, bond_peer_heap, bond_peer_uptime, pairmode, pairmode_secs, peersknown, stalestpeerage), location (gps, speed, sats, wp_dist:<name>), the on-device model (llm), and ESP-NOW metadata (room, zone, tags); numeric variables use the > < = >= <= != operators and string/enum variables use = != CONTAINS. A true condition fires every time its trigger is due. Supporting commands: validate-conditions checks conditional syntax without running it, autolog records automation activity to a file, and print <message> broadcasts text to all outputs.

| Command | | Description |
| ------- | :-: | ----------- |
| `autolog` |  | Automation logging: autolog start <file> \| stop \| status.<br/>`Usage: autolog start <filename> \| autolog stop \| autolog status` |
| `automation` | A | Automation system: automation <subcommand> [args].<br/>`Usage: automation <system enable\|disable\|status \| list \| add \| enable \| disable \| delete \| run \| trigger \| sanitize \| recompute>` |
| `automationadd` | A | Add automation (same as 'automation add'). |
| `automationlist` |  | List all automations. |
| `automationrun` | A | Run automation by ID: automationrun id=<id>. |
| `automationtrigger` | A | Arm afterDelay automation timer: automationtrigger id=<id>. |
| `print` |  | Broadcast a message to all outputs: print <message>. |
| `validate-conditions` | A | Validate conditional automation syntax: validate-conditions IF temp>75 THEN ledcolor red. |

## battery

> The battery module reports cell state and keeps a time-series log; it is only present when battery monitoring is compiled in. The backend is a MAX17048 fuel gauge over I2C (with an ADC or USB-only fallback on other boards), and charging detection cross-references the gauge CRATE register with a VBUS-present signal so the reported state distinguishes truly charging from merely USB-powered. batterystatus prints voltage, charge percentage, charging/USB state, and a coarse status label, or returns the same data as JSON. batterylog manages a CSV discharge/charge log written to the device for later graphing: with no args it shows status, and subcommands are on/off (enable/disable), interval <5..3600> seconds (sampling period), tail (show the most recent rows), and clear (erase the log); significant events such as sleep/wake are always recorded regardless of the interval. batterycalibrate (admin) re-calibrates the ADC-based readings.

| Command | | Description |
| ------- | :-: | ----------- |
| `batterycalibrate` | A | Recalibrate/re-probe the battery sensor (ADC characterize or fuel-gauge re-probe) |
| `batterylog` |  | Battery time-series CSV log (on/off/interval/tail/clear)<br/>`Usage: batterylog [on\|off\|interval <s>\|tail\|clear]` |
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
| `debugbluetoothcore` | A | Debug Bluetooth core lifecycle.<br/>`Usage: debugbluetoothcore <0\|1> [temp\|runtime]` |
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
| `debugespnow` | A | Debug ESP-NOW core messages (alias of debugespnowcore).<br/>`Usage: debugespnow <0\|1> [temp\|runtime]` |
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
| `debugringbridge` | A | Debug R1→G2 spoof bridge push.<br/>`Usage: debugringbridge <0\|1> [temp\|runtime]` |
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
| `debugwifidriver` | A | Debug WiFi driver.<br/>`Usage: debugwifidriver <0\|1> [temp\|runtime]` |
| `debugwifiscanning` | A | Debug WiFi scanning.<br/>`Usage: debugwifiscanning <0\|1> [temp\|runtime]` |
| `log` | A | System-wide logging to file.<br/>`Usage: log <start\|stop\|status\|autostart> start ["filepath"] [flags=0x...] [tags=0\|1]: Begin system logging filepath: Log file path, quoted (auto-generated if omitted) flags: Debug flag mask, up to 64 hex digits (bit map in System_Debug.h) tags: Prefix lines with category tags (0\|1, default 1) stop / status: Stop logging / show logging status autostart [on\|off]: Toggle logging auto-start on boot (bare = toggle)` |
| `loglevel` | A | Set log level (error\|warn\|info\|debug).<br/>`Usage: loglevel <error\|warn\|info\|debug>` |
| `loglink` | A | Route ESP-IDF logs through the unified output queue (stops UART interleave).<br/>`Usage: loglink [<0\|1\|on\|off>] (bare = show status)` |
| `memorysampleintervalsec` | A | Set memory sampling interval in seconds (0=disabled).<br/>`Usage: memorysampleintervalsec <0-300>` |
| `notifstats` | A | Notification pipeline counters: loss, suppression, ring lag, SSE drops.<br/>`Usage: notifstats [reset] (bare): print pipeline counters reset: zero them` |
| `outble` |  | Enable/disable BLE broadcast output.<br/>`Usage: outble <0\|1> - streams broadcast output to authenticated BLE clients` |
| `outg2` |  | Enable/disable G2 glasses output.<br/>`Usage: outg2 <0\|1> - streams CLI output to G2 glasses` |
| `settingsmodulesummary` | A | Show settings module summary. |
| `webconsole` | A | Enable/disable browser-side debug console output in the web UI.<br/>`Usage: webconsole <0\|1>` |

## settings

> The settings subsystem holds the device persisted configuration and the commands that change it. Each setting command (for example outserial, serialrequireauth, displayrequireauth, tzoffsetminutes, ntpserver, wifitxpower, webclihistorysize) sets one value; writes normally go to RAM and are flushed to the settings JSON on flash. Because flash writes are costly, you can batch them: beginwrite defers all subsequent writes, then savesettings flushes everything in a single write and ends the batch (savesettings is also the explicit flush-now command after individual changes). Most commands here are admin-gated. Some changes only take effect after a reboot (for example espnowenabled and httpsEnabled are marked reboot required). The controls command emits a machine-readable JSON descriptor of a module settable controls for UI use. Note that most subsystem settings (wifi, i2c, sensors, power, oled, bluetooth, espnow) are owned and registered by their own modules; this module hosts the cross-cutting CLI/output/auth/time settings plus the batch-write machinery.

| Command | | Description |
| ------- | :-: | ----------- |
| `beginwrite` | A | Start a batch settings update — defers flash write until savesettings. |
| `controls` |  | Per-module control descriptor (JSON): controls json [module]<br/>`Usage: controls json <module> (e.g. 'controls json imu'); 'controls json' lists modules` |
| `displayrequireauth` | S | Require auth for display: <0\|1><br/>`Usage: displayrequireauth <0\|1>` |
| `espnowenabled` | A | Enable/disable ESP-NOW: <0\|1> (reboot required)<br/>`Usage: espnowenabled <0\|1>` |
| `httpAutoStart` | A | Auto-start HTTP server at boot: <0\|1><br/>`Usage: httpAutoStart <0\|1>` |
| `httpsEnabled` | A | Enable/disable HTTPS: <0\|1> (reboot required)<br/>`Usage: httpsEnabled <0\|1>` |
| `ntpserver` | A | Set NTP server: <hostname><br/>`Usage: ntpserver <host>` |
| `oledclihistorysize` | A | Set OLED CLI history size: <10..100><br/>`Usage: oledclihistorysize <10..100>` |
| `outserial` | A | Set serial output: <0\|1> [persist\|temp]<br/>`Usage: outserial <0\|1> [persist\|temp]` |
| `savesettings` | A | Flush deferred settings to flash (single write). |
| `serialrequireauth` | S | Require auth for serial: <0\|1><br/>`Usage: serialrequireauth <0\|1>` |
| `tzoffsetminutes` | A | Set timezone offset: <-720..840><br/>`Usage: tzoffsetminutes <-720..840>` |
| `uartlink` | A | UART host link: status \| on \| off<br/>`Usage: uartlink [status\|on\|off]` |
| `uartlinkbaud` | A | Set UART link baud (0=board default)<br/>`Usage: uartlinkbaud <0\|9600-max>` |
| `uartrequireauth` | S | Require auth for UART link: <0\|1><br/>`Usage: uartrequireauth <0\|1>` |
| `webclihistorysize` | A | Set web CLI history size: <1..100><br/>`Usage: webclihistorysize <1..100>` |
| `wifiautoreconnect` | A | Keep hunting for the AP after an unexpected drop: <0\|1><br/>`Usage: wifiautoreconnect <0\|1> Separate from wifiautostart: this is about recovering a dropped link, not connecting at boot.` |
| `wifitxpower` | A | Set WiFi TX power: <dBm><br/>`Usage: wifitxpower <dBm>` |

## sensorlog

> Periodically samples the onboard sensors and appends readings to a file, driven by the single multiplexed sensorlog <subcommand> command. sensorlog start <filepath> [interval_ms] begins logging (default 5000 ms; the filepath must start with / and parent directories are created automatically) and sensorlog stop ends it; only one log can run at a time, so start refuses if logging is already active. sensorlog status reports the active file, interval, format, rotation settings, selected sensors, and last-write age. Configure behavior with format <text|csv|track> (track is a compact GPS-only format with signal-loss dedup), maxsize and rotations for log rotation, and sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none> to choose which sensors are recorded. sensorlog interval <ms> sets the poll period (100-3600000, default 5000). NOTE: format track additionally REPLACES the sensor mask with GPS-only and persists it, so a prior selection is lost — and rotations 0 deletes the active file at the size cap rather than pruning older generations. sensorlog autostart [on|off] makes logging resume on the next boot using the last-used parameters; the format/maxsize/rotations/sensors/autostart choices are persisted.

| Command | | Description |
| ------- | :-: | ----------- |
| `capturecrypt` | A | Capture at-rest encryption: status, mode (off/health/all), plaintext export<br/>`Usage: capturecrypt [status\|off\|health\|all\|export "<in>" "<out>"] Sealed sessions write '#HW1ENC' on line 1 + per-row ciphertext; filenames don't change. health: seal sessions that include the R1 ring (default). all: every capture session. Mode changes apply at the next session or day rollover — a single file is never mixed-mode. Viewers (fileview, web view, G2/OLED) decrypt for authorized users; raw downloads, fileread and ESP-NOW transfers ship sealed bytes. export: write a decrypted copy (inside /logging_captures) for sharing.` |
| `sensorlog` |  | Sensor data logging: start, stop, status, format, maxsize, rotations, sensors<br/>`Usage: sensorlog <start\|stop\|status\|format\|maxsize\|rotations\|sensors\|interval\|autostart> [args...] start <filepath> [interval_ms]: Begin logging (default 5000ms) stop: Stop logging status: Show current logging status format <text\|csv\|track>: Set log format (default: text) track = GPS-only compact format with signal loss dedup; NOTE: also OVERWRITES the sensor mask to GPS-only and persists it maxsize <bytes>: Set max file size before rotation (default: 256000) rotations <count>: Old generations to keep (1-9). 0 = DELETE the active file at the size cap, losing the current day (default: 3) sensors <thermal\|tof\|imu\|gamepad\|apds\|gps\|presence\|r1\|all\|none>: Select sensors to log interval <ms>: Set poll interval 100-3600000 (default 5000) autostart [on\|off]: Auto-start logging on boot (bare = toggle)` |

## health

> Live vitals from a paired R1 ring (HR, HRV, SpO2, temperature, battery) plus the on-device health logger. healthstatus reads live values, ring control state, logging state and typed-history status; healthstatus json is what the app and web page consume. healthlogging drives the local CSV logger independently of the ring's own health-collection privacy setting, and healthlogmerge stitches captures together. The ring rides the G2 BLE transport, so this whole family requires Bluetooth + G2; connect via ringconnect.

| Command | | Description |
| ------- | :-: | ----------- |
| `healthlogging` |  | Start/stop local R1 health logging (independent of ring collection)<br/>`Usage: healthlogging <on\|off\|toggle\|status\|interval [sec]> on: enable LOG_R1, force format=CSV, start under /logging_captures/sensors/, persist for boot (one dated per-day file when the clock is set, boot-<N>/ until sync then roll) off: remove LOG_R1; stop logging if no other sensors remain interval <sec>: how often local logging polls/mines the ring (default 900 = 15 min) R1-only sessions write ONLY on that mine (and Poll Now) — no 5s empty heartbeats This does not change the ring's health-collection privacy setting.` |
| `healthlogmerge` | A | Byte-concatenate sensor logs in the given order: "<out>" "<in1>" "<in2>" ...<br/>`Usage: healthlogmerge "<output>" "<in1>" "<in2>" [...] OUTPUT FIRST — arg 0 is TRUNCATED. Inputs follow, in the order you want them. Bare output name → /logging_captures/sensors/; INPUTS need a full path. Extensionless output gets .csv appended even for TEXT inputs. Concatenation is byte-exact: CSV inputs keep their header lines mid-file, rows are NOT time-ordered, and mixing formats/sensor masks yields an unparseable result. Max 8 inputs (arg limit).` |
| `healthstatus` |  | R1 live vitals, ring controls, local logging, and typed history status<br/>`Usage: healthstatus [json\|poll\|history\|force-history\|refresh-controls] bare/json: live values, desired/observed controls, local logging, history/store poll: kick HR→HRV→SpO2→battery point queries (replies via notify) history: normal typed history refresh; force-history: admin freshness bypass refresh-controls: read low-power state; health collection has no proven GET and stays Unknown BLE App / Web use healthstatus json; connect via ringconnect / Bluetooth page` |

## users

> The users subsystem provides admin-gated account management, authentication, sessions, and bans. Accounts have two roles, admin and standard; the first account is the owner-admin, and userpromote/userdemote change roles while useradd creates an account directly (optionally forcing a password change on first login). New accounts can also come through an approval flow: userrequest files a pending request that an admin clears with userapprove or rejects with userdeny (pendinglist shows the queue). login and logout authenticate per transport (serial, display, bluetooth, g2), userlist enumerates accounts, and the password commands cover both self-service (userchangepassword) and admin reset (userresetpassword). Sessions are tracked per transport: sessionlist shows active sessions and sessionrevoke force-logs-out a session by SID or by username. Two independent ban mechanisms exist: ban/unban/banlist block an IP address, while banuser/unbanuser suspend a user account so it cannot log in until unbanned; the primary admin account cannot be banned. usersync pushes a user credentials to another device over ESP-NOW, authenticated by an admin account on the receiving device.

| Command | | Description |
| ------- | :-: | ----------- |
| `ban` | A | Permanently ban an IP: <ip> [reason]<br/>`Usage: ban <ip> [reason] Blocks all access from the IP until manually unbanned.` |
| `banlist` | A | List all banned IPs. (add 'json' for JSON output) |
| `banuser` | A | Permanently ban a user account: <username> [reason]<br/>`Usage: banuser <username> [reason] Prevents the account from logging in until manually unbanned.` |
| `login` |  | Login this interface, or target another session after signing in: <user> <pass> [serial\|uart\|display]<br/>`Usage: login <username> <password> [serial\|uart\|display] Bare login always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session.` |
| `logout` |  | Logout this interface, or target another session after signing in: [serial\|uart\|display]<br/>`Usage: logout [serial\|uart\|display] Bare logout always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session.` |
| `pendinglist` | A | List pending user requests. (add 'json' for JSON output) |
| `serialrequireauth` | S | Enable/disable serial auth requirement [on\|off].<br/>`Usage: serialrequireauth [on\|off]` |
| `sessionlist` | A | List active sessions. (add 'json' for JSON output) |
| `sessionrevoke` | A | Revoke session: <sid\|user> [reason]<br/>`Usage: sessionrevoke sid <sid> [reason] sessionrevoke user <username> [reason]` |
| `unban` | A | Remove an IP ban: <ip><br/>`Usage: unban <ip>` |
| `unbanuser` | A | Remove a user account ban: <username><br/>`Usage: unbanuser <username>` |
| `useradd` | A | Create user: <username> <password> [0\|1] [role]<br/>`Usage: useradd <username> <password> [0\|1] [guest\|user\|admin\|superadmin] 0\|1: 1 = require a new password on next login (default 0) role: defaults to user; you cannot grant a role above your own The two optional tokens may appear in either order` |
| `userapprove` | A | Approve pending request: <username><br/>`Usage: userapprove <username>` |
| `userchangepassword` |  | Change own password: <currentPass> <newPass> <confirmPass><br/>`Usage: userchangepassword <currentPassword> <newPassword> <confirmPassword>` |
| `userdelete` | A | Delete user: <username><br/>`Usage: userdelete <username>` |
| `userdemote` | A | Lower a user's role: <username> [admin\|user\|guest]<br/>`Usage: userdemote <username> [admin\|user\|guest] (default user; demoting a super-admin requires a super-admin caller)` |
| `userdeny` | A | Deny pending request: <username><br/>`Usage: userdeny <username>` |
| `userlist` | A | List all users. (add 'json' for JSON output) |
| `userpromote` | A | Promote a user: <username> [user\|admin\|superadmin]<br/>`Usage: userpromote <username> [user\|admin\|superadmin] (default admin; granting superadmin requires a super-admin caller)` |
| `userrequest` |  | Request account: <user> <pass> [confirm]<br/>`Usage: userrequest <username> <password> [confirmPassword]` |
| `userresetpassword` | A | Reset user password: <username> <newPassword> [0\|1]<br/>`Usage: userresetpassword <username> <newPassword> [0\|1] Optional: 1 = require password change on next login` |
| `usersync` | A | Sync a user to another device over ESP-NOW. (async; result only on the target device - check its userlist)<br/>`Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass> Returns OK on delivery; the user is created on the TARGET device (no confirmation returns here) - verify on that device's userlist. targetAdminUser/targetAdminPass = an admin account on the RECEIVING device (validated there). yourAdminPass = your admin password on THIS device; userPass = the synced user's password.` |
| `whoami` |  | Show the identity of the submitting interface.<br/>`Usage: whoami` |

## features

> The features subsystem enables or disables compiled-in capabilities at runtime and reports their memory cost. features with no argument lists every feature grouped by category (Network, Display, Sensors, System) with an approximate heap estimate and a status of ON, OFF, or N/C (not compiled in this build); features <id> shows one feature details and features <id> <on|off> toggles it, persisting the change immediately. Only features that are compiled and marked runtime-toggleable can be changed; a few are compile-time only, and some (wifi, oled, i2c, https) are flagged reboot required so the toggle persists but the capability does not actually start or stop until the next restart. featuresetup launches an interactive, admin-only wizard that walks through the same toggles and works from any CLI transport.

| Command | | Description |
| ------- | :-: | ----------- |
| `features` | A | Show/toggle system features with heap estimates. |
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
| `gpstrack` |  | Manage GPS tracks: <status\|load\|clear><br/>`Usage: gpstrack [status\|load <filepath>\|clear]` |
| `gpstrackmerge` | A | Stitch GPS logs in order: "<out>" "<in1>" "<in2>" ...<br/>`Usage: gpstrackmerge "<output>" "<in1>" "<in2>" [...] (output first, then inputs in stitch order; max 9 inputs)` |
| `map` |  | Show current map info (add 'json' for JSON output) |
| `maplist` |  | List available maps (add 'json' for JSON output) |
| `mapload` |  | Load map file: "<path>"<br/>`Usage: mapload "<path>"` |
| `maporganize` |  | Organize map files in /maps into subdirectories |
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

> The power subsystem manages CPU frequency and battery-oriented power saving. The main command is power: power alone prints the current mode, CPU clock, display brightness, and auto-mode state; power mode <perf|balanced|saver|ultra|locked|0-4> selects one of five preset modes (Performance 240/80 MHz, Balanced 160/80 MHz, PowerSaver 80 MHz, UltraSaver 80 MHz interactive / 40 MHz idle, Locked 240 MHz always) which sets both the CPU frequency and the display brightness; the chosen mode is persisted. Locked alone holds 240 MHz through idle power-save (OLED blanks but the core does not downclock). UltraSaver's headline 40 MHz is idle-only — it is applied solely when idle power-save blanks the screen (40 MHz is too laggy for the live UI) and any input or command restores >=80 MHz; so UltraSaver only reaches 40 MHz if powersave is enabled. power auto <on|off> enables an automatic low-battery downshift gated by power threshold <0-100>. Two related idle controls are separate commands: powersave <0..1440> sets an idle timeout (minutes; 0 disables) after which the OLED blanks and the CPU may downclock (mode-dependent) while the radio stays up so the device remains reachable, and powercooldown <0..60000> sets an anti-flap cooldown (milliseconds) that prevents rapid back-to-back sleep transitions. All of these values persist.

| Command | | Description |
| ------- | :-: | ----------- |
| `power` | A | Power management [mode] [auto] [threshold]<br/>`Usage: power - show current power status power mode <perf\|balanced\|saver\|ultra\|locked\|0-4> power auto <on\|off> power threshold <0-100>` |
| `powercooldown` | A | Sleep transition cooldown (ms; 0 disables)<br/>`Usage: powercooldown <0..60000>` |
| `powersave` | A | Idle power-save: OLED off + optional downclock (0 disables)<br/>`Usage: powersave <0..1440>` |

## liveaudio

> Exercises the live-pcm-v1 UART framing and bounded receiver path. A real authenticated UART host first acquires a renewable 3-second controller lease with liveaudio ready. liveaudio synth schedules deterministic 16 kHz signed-16-bit mono PCM; an explicit, exact-ID liveaudio shadow arm can instead tee an owned 16 kHz PDM or G2 recording through a fixed 16 KiB PSRAM queue. Shadow transport is disabled by default, never delays or replaces the WAV writer, and a transport fault aborts only the live copy while the finalized WAV remains authoritative. This diagnostic transport does not enable production streaming STT, LLM, or lens delivery.

| Command | | Description |
| ------- | :-: | ----------- |
| `liveaudio` |  | Opt-in live PCM transport, lease, and shadow diagnostics<br/>`Usage: liveaudio <capabilities\|status\|ready 1 <controller_hex16>\|shadow 1 <controller_hex16> on <exchange_hex16\|native>\|shadow 1 <controller_hex16> off\|release 1 <controller_hex16>\|synth 1 <controller_hex16> <exchange_hex16> <duration_ms>\|abort 1 <controller_hex16> <exchange_hex16>>` |

## cm5

> Everything that talks to the CM5 Linux host over the authenticated UART link. cm5 status exposes the current named-UART epoch binding, freshness, state, command bridge, monitor transitions, and task stack watermark; cm5 capabilities reports the heartbeat protocol constants. The five-second heartbeat itself is authenticated UART control-plane traffic, not a user command, so it remains responsive even while the shared command executor is occupied. cm5 power and cm5 fan drive the host through two independent finite request/ACK/report state machines that share one ID space: cm5 power status requests fresh CM5 state, cm5 power profile <eco|balanced|performance|auto> requests a power profile, and cm5 fan <quiet|max> pins a fan mode while cm5 fan auto returns control to the Linux temperature curve. Bare cm5 power / cm5 fan (or their show form) display local delivery state and the last CM5 report. Initiation requires an admin; reboot, halt, suspend, and sleep_for <1..1440 minutes> additionally require superadmin plus a literal same-command confirm token, and recover confirm clears only an inspected fail-closed transition. ACK/report callbacks are accepted from a real authenticated UART session only, which lets the CM5 service account stay user-tier. One request per protocol may be pending at a time and delivery retries are finite; destructive execution additionally requires confirmed accepted and committed ACK phases, while a normalized Linux boot ID distinguishes a daemon restart from a completed host boot. A max fan request may supersede one pending non-max request.

| Command | | Description |
| ------- | :-: | ----------- |
| `cm5` |  | Inspect CM5 service presence, and host power/fan control.<br/>`Usage: cm5 <status\|capabilities> (heartbeat is UART control-plane only)` |
| `cm5 capabilities` |  | Show the CM5 presence protocol capabilities.<br/>`Usage: cm5 capabilities` |
| `cm5 fan` | A | Inspect or request CM5 fan mode/readback.<br/>`Usage: cm5 fan [show\|status\|quiet\|auto\|max]` |
| `cm5 fan ack` |  | Accept a CM5 fan ACK (authenticated UART session only).<br/>`Usage: cm5 fan ack 1 <16-hex-id> <accepted\|applied\|failed>` |
| `cm5 fan report` |  | Accept bounded CM5 fan readback (authenticated UART session only).<br/>`Usage: cm5 fan report 1 <id> <requested-mode> <effective-mode> <temp-mc\|-1> <target-pwm> <pwm> <rpm\|-1> <health>` |
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
| `otaack` | A | Unavailable without the opt-in OTA layout. |
| `otacancel` | A | Cancel a staged request before recovery boot is armed.<br/>`Usage: otacancel confirm` |
| `otacancel` | A | Unavailable without the opt-in OTA layout. |
| `otapin` | A | Set the persistent recovery WPA2/HTTP credential (serial console only).<br/>`Usage: otapin <12..63 printable characters> \| otapin clear confirm` |
| `otapin` | A | Unavailable without the opt-in OTA layout. |
| `otarecovery` | A | Reboot into authenticated recovery for direct upload.<br/>`Usage: otarecovery confirm [allow-downgrade]` |
| `otarecovery` | A | Unavailable without the opt-in OTA layout. |
| `otaresetjournal` | A | Serial-only repair of the two OTA transaction keys.<br/>`Usage: otaresetjournal confirm` |
| `otaresetjournal` | A | Unavailable without the opt-in OTA layout. |
| `otastage` | A | Validate and journal uploaded candidate.part + manifest.part.<br/>`Usage: otastage confirm [allow-downgrade]` |
| `otastage` | A | Unavailable without the opt-in OTA layout. |
| `otastatus` |  | Show signed recovery OTA state. (add 'json')<br/>`Usage: otastatus [json]` |
| `otastatus` |  | Explain OTA availability for this build.<br/>`Usage: otastatus` |
| `otaupdate` | A | Revalidate staged firmware and reboot into recovery apply.<br/>`Usage: otaupdate confirm [force-power]` |
| `otaupdate` | A | Unavailable without the opt-in OTA layout. |
| `otawrite` | A | Stage exact OTA members over encrypted Bluetooth.<br/>`Usage: otawrite begin <candidate\|manifest> <size> <sha256> \| status \| finish \| abort` |
| `otawrite` | A | Unavailable without the opt-in OTA layout. |

## setpattern

> Provides the single admin-only command setgamepadpassword, which opens the gamepad-pattern password setup flow on the OLED screen. A pattern is a sequence of joystick directions that is hashed and stored as the logged-in user password, usable for on-device login. You must already be logged in at the OLED display first (the command errors otherwise); the guided on-screen flow then re-authenticates you, prompts you to enter the new pattern and confirm it, and saves it to your account. This command only launches the OLED mode -- the actual entry and confirmation happen on the device screen.

| Command | | Description |
| ------- | :-: | ----------- |
| `setgamepadpassword` | A | Set gamepad joystick password (requires an active OLED-display login). |

## even_g2

> This subsystem drives Even Reality G2 smart glasses while Bluetooth is in client mode (blemode client); the two temples are addressed as left/right/auto. openg2 starts scan-and-connect IN THE BACKGROUND and returns immediately -- it does not block, so poll g2status (or g2info for firmware/MAC/battery) to see when the link is up, and nearly every other command here requires that connection first. Display commands render to the lens: g2show prints text, g2ai/g2ai-noask/g2ai-direct push a front-pane AI answer card through the EvenAI pipeline, g2bmp shows a BMP file, and g2sensors/g2network/g2files/g2settingspage show built-in info pages; g2nav [on|off] enables menu-navigation mode and g2clear blanks the display. For audio, g2mic only sends the enable/disable control frame (LC3 decode is not yet wired, so no audio arrives); the working capture path is g2micrec (raw LC3 packets to SD) and g2micwav (decodes to a 16 kHz mono WAV on SD), each an SD-backed start/stop/status lifecycle that needs an SD card. closeg2 disconnects but keeps the GATT cache for fast reconnect; closeg2 full also frees the cache to recover about 30 KB. The remaining g2* commands are low-level protocol probes and diagnostics (g2probe, g2protostats, g2devcfg, g2dumpframes).

| Command | | Description |
| ------- | :-: | ----------- |
| `closeg2` | A | Disconnect G2 glasses [full=also free ~30KB GATT cache].<br/>`Usage: closeg2 [full]` |
| `g2ai` |  | Front-pane AI card (full pipeline): g2ai <text><br/>`Usage: g2ai <text>` |
| `g2ai-direct` |  | Variant: CTRL+REPLY only: g2ai-direct <text><br/>`Usage: g2ai-direct <text>` |
| `g2ai-noask` |  | Variant: skip ASK step: g2ai-noask <text><br/>`Usage: g2ai-noask <text>` |
| `g2aiconfig` |  | Send EvenAI CONFIG (cmd=10): g2aiconfig [voiceSwitch] [streamSpeed] [duplexMode]<br/>`Usage: g2aiconfig [voiceSwitch] [streamSpeed] [duplexMode] (defaults: 0 80 0; use - to omit a field)` |
| `g2aih` |  | Front-pane card with custom heading: g2aih <heading>\|<body><br/>`Usage: g2aih <heading>\|<body> (no \| = whole text as body)` |
| `g2battery` |  | Query G2 battery % on connected temples |
| `g2bmp` |  | Display BMP: g2bmp </path.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]<br/>`Usage: g2bmp </path/to/file.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]` |
| `g2clear` |  | Clear G2 display |
| `g2connpri` | A | Probe the BLE conn-interval admission boundary: g2connpri [<min> <max>\|default] (bare = report)<br/>`Usage: g2connpri [<min> <max>\|default] (ticks of 1.25 ms; runtime only; bare = report)` |
| `g2control` | A | Persist/reconcile G2 device policy without overwriting official-app state by default<br/>`Usage: g2control [<headup\|notifications> <preserve\|off\|on>] (bare = status; work is queued to g2 control owner)` |
| `g2deinit` |  | Deinitialize G2 client mode |
| `g2devcfg` |  | Typed sid=0x80 sender: g2devcfg <heartbeat\|auth\|role\|time\|ring> [args]<br/>`Usage: g2devcfg <heartbeat\|auth\|role <both\|right\|left>\|time [tzQuarterHours]\|ring <mac> <name>>` |
| `g2dumpframes` |  | Print the recent G2 envelope ring buffer |
| `g2envgap` | A | Image inter-envelope gap override: g2envgap [<5..500>\|auto] (bare = report)<br/>`Usage: g2envgap [<5..500>\|auto] (runtime only; bare = report)` |
| `g2evenai` |  | Push into the matching live Hey-Even exchange (strict 16-hex ID required)<br/>`Usage: g2evenai <askid\|replyid\|replypartid\|replyendid\|exitid> <16hex-id> [text] status: show active exchange ID/arm/generation capabilities: show guarded command contract replypartid: stream one delta (leading glue preserved) legacy ask/reply/replypart/replyend/exit fail closed; use g2ai for deliberate bench cards` |
| `g2files` |  | Show Files browser page on the G2 lens |
| `g2glasses` | A | Change glasses-device settings (including capture-proven HeadUp on fw 2.2.6.10)<br/>`Usage: g2glasses [show\|refresh] \| g2glasses <brightness\|autobright\|weardetect\|x\|y\|headup\|headangle\|silent\|unitformat\|distunit\|timeformat\|dateformat\|tempunit> [value]` |
| `g2health` |  | Open the Health (R1 vitals + graphs) app on the G2 lens<br/>`Usage: g2health (Overview/HR/HRV/SpO2/Battery on the lens; Back to exit)` |
| `g2hijacktest` |  | Simulate a Blocks tap (status-page hijack) |
| `g2imgprobe` |  | Probe Cmd=3 multi-frag wire path: g2imgprobe [size_bytes]<br/>`Usage: g2imgprobe [size_bytes] (1..4096, default 1024)` |
| `g2info` |  | Dump device info (firmware, MAC, battery, etc.) |
| `g2init` |  | Initialize G2 client mode |
| `g2listrebuild` |  | REBUILD-list on swap when pure list + same row count [on\|off] (default ON)<br/>`Usage: g2listrebuild [<on\|off>] (bare = report state)` |
| `g2liveloop` |  | Q13/Q14 lens-idle keep-alive: g2liveloop keep [on\|off] (default off → break on lens timeout)<br/>`Usage: g2liveloop keep [<on\|off>] (bare = report state)` |
| `g2liverate` |  | Get/set live-update probe cadence (ms), default 600: g2liverate [N]<br/>`Usage: g2liverate [ms>=100] (bare = report)` |
| `g2map` |  | Render the offline map on the G2 lens (288x144)<br/>`Usage: g2map (renders the current map view; double-tap the lens to dismiss)` |
| `g2micoff` |  | G2 mic probe: AudioCtrCmd{en=0} (stop stream) |
| `g2micon` |  | G2 mic probe: AudioCtrCmd{en=1} on LEFT (or 'r' for RIGHT)<br/>`Usage: g2micon [r] (default LEFT; arg starting r = RIGHT)` |
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
| `g2pet` |  | Open the Pet (virtual creature) app on the G2 lens<br/>`Usage: g2pet (Feed/Play/Clean/Sleep on the lens; Back to exit)` |
| `g2probe` |  | Fire arbitrary pb cmd on non-mutation sids: g2probe <sid_hex> <cmd_dec> [body_hex]<br/>`Usage: g2probe <sid_hex> <cmd_dec> [body_hex] (sids 01/03/04/09/80 blocked)` |
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
| `g2verbose` |  | Scan-verbose logging: g2verbose [on\|off\|toggle] (bare = report state)<br/>`Usage: g2verbose [on\|off\|toggle] (bare = report state)` |
| `openg2` | A | Connect to G2 glasses: openg2 [left\|right\|auto\|saved]<br/>`Usage: openg2 [left\|right\|auto\|saved] (default auto; saved = peer MACs)` |

## even_r1

> This subsystem talks to the Even R1 smart ring over BLE through a serialized, profile-gated transaction owner. ringscan [seconds] discovers the ring and ringconnect [mac] connects (auto-scanning when no MAC is given, or connecting directly when one is), with ringstatus and ringdisconnect for state and teardown. ringquery is the main data command, requesting wear/heart-rate/HRV/SpO2/temperature/activity/sleep readings. Its raw diagnostic form is admin-only, requires confirmation for SETs, and prohibits user-profile writes. Ring health collection and low-power desired state are managed through the authenticated Health surfaces; health SETs remain ACKed-unverified while low power has capture-proven readback. debugringdump toggles a redacted byte dump for debugging. Bridging ring data onto the G2 glasses is deliberately unavailable -- the commands exist in the code but are intentionally left unregistered because both approaches proved to be dead ends.

| Command | | Description |
| ------- | :-: | ----------- |
| `ringconnect` | A | Connect to the R1 ring: ringconnect [mac\|reconnect]<br/>`Usage: ringconnect [mac\|reconnect] (no arg = scan-then-connect; mac = direct; reconnect = drop+settle+connect)` |
| `ringdisconnect` | A | Disconnect from the R1 ring |
| `ringquery` |  | Queue an R1 query: ringquery <wear\|hr\|hrv\|spo2\|temp\|activity\|sleep\|raw> [type]<br/>`Usage: ringquery <wear\|hr\|hrv\|spo2\|temp\|activity\|sleep> [daily\|point\|measure] \| raw <module> <cmd> <subCmd> [hex_payload] [status=NN] (raw is admin-only; SET requires confirmation)` |
| `ringscan` |  | Scan for the R1 ring: ringscan [seconds] (default 30, max 300)<br/>`Usage: ringscan [seconds] (1..300, default 30)` |
| `ringstatus` |  | Show R1 ring connection status |

## llm

> On-device large language model that runs a quantized model file entirely on the device (model weights held in PSRAM). A model must be loaded before generation: llmload [file.bin] loads one (bare filenames are looked up on the SD card under /sd/llm then internal /system/llm), llmmodels lists available files, llmunload frees the PSRAM, llmstatus shows engine state, and llmautostart 0|1 / llmdefaultmodel control boot-time loading. Two generate forms: bare 'llmgenerate <prompt>' BLOCKS and prints the whole reply, while 'llmgenerate json ...' starts async and returns a session id immediately — then poll llmresult json <offset> repeatedly (each call returns new text, the running total length, and a done flag) until done flips true; llmstop aborts an in-progress generation. The engine keeps a multi-turn conversation: llmclear resets it, llmretry regenerates the last reply (async), and llmturns json <index> reads back one turn at a time. The llm* setters (temperature, topp, minp, maxtokens, sentencelimit, hardcap, reppenalty/repwindow, maxcontext, kvprec, norepeatngram, confthreshold, contentboost) are admin-only sampler and KV-cache defaults that persist to flash; kvprec and maxcontext only take effect on the next model load.

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
| `llmkvprec` | A | KV cache precision (0=FP32,1=FP16,2=INT8)<br/>`Usage: llmkvprec <0..2> (0=FP32,1=FP16,2=INT8; reload model to apply)` |
| `llmload` | A | Load model [model.bin]<br/>`Usage: llmload [filename.bin]` |
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
| `llmturns` |  | Read a conversation turn (JSON)<br/>`Usage: llmturns json <index>` |
| `llmunload` | A | Unload model and free PSRAM |

## settingsedit

> Static CLI commands the web/OLED settings screen uses to persist individual settings fields that have no dedicated module command. Each writes one setting via handleSettingCommand; the value is read live or applied on next start. Fields that need a live apply action are routed to their module command instead of getting one of these.

| Command | | Description |
| ------- | :-: | ----------- |
| `apdsdevicepollms` | A | Set APDS poll interval (ms)<br/>`Usage: apdsdevicepollms <value>` |
| `apdsenabled` | A | Enable/disable the APDS gesture sensor subsystem<br/>`Usage: apdsenabled <0\|1>` |
| `automationautostart` | A | Start the automation scheduler at boot<br/>`Usage: automationautostart <0\|1>` |
| `bleenabled` | A | Enable/disable the Bluetooth subsystem<br/>`Usage: bleenabled <0\|1>` |
| `cameraenabled` | A | Enable/disable the camera subsystem<br/>`Usage: cameraenabled <0\|1>` |
| `eiautostart` | A | Start Edge Impulse inference at boot<br/>`Usage: eiautostart <0\|1>` |
| `eiinputsize` | A | Set Edge Impulse input size<br/>`Usage: eiinputsize <value>` |
| `eiinterval` | A | Set Edge Impulse inference interval (ms)<br/>`Usage: eiinterval <100-10000>` |
| `eimaxdetections` | A | Set Edge Impulse max detections<br/>`Usage: eimaxdetections <value>` |
| `eirequirelabels` | A | Set Edge Impulse require-labels flag<br/>`Usage: eirequirelabels <0\|1>` |
| `espnowautostart` | A | Start ESP-NOW at boot<br/>`Usage: espnowautostart <0\|1>` |
| `espnowcaptureskipheartbeats` | A | Omit heartbeat frames from the ESP-NOW capture<br/>`Usage: espnowcaptureskipheartbeats <0\|1>` |
| `espnowcapturetosd` | A | Capture ESP-NOW frames to the SD card<br/>`Usage: espnowcapturetosd <0\|1> Needs an SD card mounted; frames are appended as they arrive.` |
| `eventlog` | A | Enable/disable the structured event-history log (events.log)<br/>`Usage: eventlog <0\|1> One line per system event, durable across reboots. Display/behavior unaffected.` |
| `fmradiodevicepollms` | A | Set FM radio poll interval (ms)<br/>`Usage: fmradiodevicepollms <value>` |
| `fmradioenabled` | A | Enable/disable the FM radio subsystem<br/>`Usage: fmradioenabled <0\|1>` |
| `gpsdevicepollms` | A | Set GPS poll interval (ms)<br/>`Usage: gpsdevicepollms <value>` |
| `gpsenabled` | A | Enable/disable the GPS subsystem<br/>`Usage: gpsenabled <0\|1>` |
| `httpenabled` | A | Enable/disable the web server subsystem<br/>`Usage: httpenabled <0\|1>` |
| `imuenabled` | A | Enable/disable the IMU subsystem<br/>`Usage: imuenabled <0\|1>` |
| `inputenabled` | A | Enable/disable the input device subsystem<br/>`Usage: inputenabled <0\|1>` |
| `llmenabled` | A | Enable/disable the on-device LLM subsystem<br/>`Usage: llmenabled <0\|1>` |
| `logcategorytags` | A | Set log category-tags flag (persist only)<br/>`Usage: logcategorytags <0\|1>` |
| `micenabled` | A | Enable/disable the microphone subsystem<br/>`Usage: micenabled <0\|1>` |
| `notifydeviceapp` | A | Enable/disable Android app notification cards<br/>`Usage: notifydeviceapp <0\|1> Cards go only to BLE sessions that are logged in; an unauthenticated app sees none.` |
| `notifydevicebanners` | A | Enable/disable OLED notification banners<br/>`Usage: notifydevicebanners <0\|1>` |
| `notifydeviceg2` | A | Enable/disable G2 lens notification cards<br/>`Usage: notifydeviceg2 <0\|1>` |
| `notifydevicekind` | A | Set per-event notification visibility (device-wide)<br/>`Usage: notifydevicekind [list [json]] \| <kind> [all\|admin\|off] Bare: show non-default kinds; list: show every kind (json = machine form) <kind> alone shows its level; with a level, sets and persists it admin: only admin viewers see it; off: hidden for everyone Levels affect banners/toasts/queue only - events and automations still fire` |
| `notifydevicequeue` | A | Enable/disable the notification-center queue<br/>`Usage: notifydevicequeue <0\|1>` |
| `notifydevicetoasts` | A | Enable/disable web notification toasts<br/>`Usage: notifydevicetoasts <0\|1>` |
| `notifylevel` |  | Set YOUR notification importance floor<br/>`Usage: notifylevel [verbose\|standard\|alert] Bare: show your current floor (default: standard) verbose: everything; standard: skip routine chatter; alert: security/safety only Nothing is lost - filtered kinds still reach the notification center and automations` |
| `notifyusermute` |  | Mute event kinds from notifications for YOUR user<br/>`Usage: notifyusermute [<kind,kind,...>\|none] Bare: show your muted kinds; none: clear Applies only to the logged-in user (stored with your dashboard preferences) List valid kinds with 'events kinds'` |
| `notifyusershow` |  | Force event kinds through YOUR importance floor<br/>`Usage: notifyusershow [<kind,kind,...>\|none] Bare: show your forced kinds; none: clear Opposite of notifyusermute: these interrupt even below your notifylevel List valid kinds with 'events kinds'` |
| `oledautostart` | A | Start the OLED display at boot<br/>`Usage: oledautostart <0\|1>` |
| `powerdim` | A | Set display dim level (%)<br/>`Usage: powerdim <0-100>` |
| `presencedevicepollms` | A | Set presence sensor poll interval (ms)<br/>`Usage: presencedevicepollms <50-5000>` |
| `presenceenabled` | A | Enable/disable the presence sensor subsystem<br/>`Usage: presenceenabled <0\|1>` |
| `rtcenabled` | A | Enable/disable the RTC subsystem<br/>`Usage: rtcenabled <0\|1>` |
| `sensorlogenabled` | A | Enable/disable sensor logging entirely: <0\|1><br/>`Usage: sensorlogenabled <0\|1>` |
| `sensorlogformat` | A | Set sensor-log format (0=text,1=csv,2=track)<br/>`Usage: sensorlogformat <0\|1\|2>` |
| `sensorlogmask` | A | Set sensor-log sensor bitmask<br/>`Usage: sensorlogmask <0-255>` |
| `sensorlogpath` | A | Set default sensor-log file path<br/>`Usage: sensorlogpath <"/path">` |
| `sessionidleble` | A | Set BLE session idle-logout (min)<br/>`Usage: sessionidleble <0-1440>` |
| `sessionidledisplay` | A | Set OLED session idle-logout (min)<br/>`Usage: sessionidledisplay <0-1440>` |
| `sessionidleserial` | A | Set serial session idle-logout (min)<br/>`Usage: sessionidleserial <0-1440>` |
| `sessionidleuart` | A | Set UART link session idle-logout (min)<br/>`Usage: sessionidleuart <0-1440>` |
| `sessionidleweb` | A | Set web CLI session idle-logout (min)<br/>`Usage: sessionidleweb <0-1440>` |
| `srautostart` | A | Set ESP-SR auto-start flag<br/>`Usage: srautostart <0\|1>` |
| `srenabled` | A | Enable/disable the speech-recognition subsystem<br/>`Usage: srenabled <0\|1>` |
| `srmodelsource` | A | Set ESP-SR model source<br/>`Usage: srmodelsource <value>` |
| `systemlogenabled` | A | Enable/disable system logging entirely: <0\|1><br/>`Usage: systemlogenabled <0\|1>` |
| `systemlogflags` | A | Set system-log debug category mask (hex)<br/>`Usage: systemlogflags <0x...>` |
| `thermalenabled` | A | Enable/disable the thermal camera subsystem<br/>`Usage: thermalenabled <0\|1>` |
| `tofenabled` | A | Enable/disable the ToF distance sensor subsystem<br/>`Usage: tofenabled <0\|1>` |
| `tofi2cclockhz` | A | Set ToF I2C clock (Hz)<br/>`Usage: tofi2cclockhz <50000-400000>` |
| `wifiautostart` | A | Connect to a saved WiFi network at boot: <0\|1><br/>`Usage: wifiautostart <0\|1>` |
| `wifienabled` | A | Enable/disable WiFi entirely (ESP-NOW unaffected): <0\|1><br/>`Usage: wifienabled <0\|1>` |

