# HardwareOne - App JSON Command Contract

Audience: the Android app AI. This documents the device's JSON command surface and
the conventions for consuming it, as of firmware v0.99.98.5 (last revised with that
release, 2026-10-25). Dated notes in the text mark when a field was added; the
peer-metadata commands date from v0.95.10. Changes after v0.99.98.5 are recorded in
`CHANGELOG.md`, not here, with one exception: the rows that the command reply change
of 2026-11-17 touched (failure detection in sec. 1, automation in sec. 3, `blepeers`
and `versions` in sec. 4, `images`, `maplist`, `cameravideolist` and `espnowstatus`
in sec. 5, `cameraread` in sec. 6, and the action list in sec. 7) are corrected in place, and the `CHANGELOG.md`
entry "Command replies" under Unreleased states what changed.

---

## 1. Conventions (read first)

- **Opt-in flag.** Append a standalone `json` token to a supported command to get a
  JSON reply. Without it, you get the unchanged human-readable text. Example:
  `wifistatus` -> text, `wifistatus json` -> `{"schema":1,...}`.
- **These are flags, not new commands.** No command names were added/renamed; each
  existing handler gained an `if (json) {...}` branch. You append ` json` to commands
  you already call.
- **Shape.** Most replies are `{"schema":1, ...}`. Some sensor replies use `"valid"`
  instead (see sec. 6).
- **Failure detection.** A command failed if its result string starts with
  `Error` / `ERROR`, `Usage:` or `Unknown command:`, or if it is a JSON object
  whose top level says `"success":false` or `"ok":false` or whose first key is
  `"error"` (`{"error":"oom"}`) (`commandReplyIsFailure()` in
  `System_CommandReply.h`; the firmware sets `ok=false` on these, and the
  web `/api/cli` answers HTTP 400, or 403 for an admin refusal). An `"error"`
  field further into a status object is that service's last error, not a failed
  command. Over BLE you get only the text, so apply the same rule there. On success, status/data
  commands return their object; action commands return `OK`, `OK: <text>` or, where
  the handler confirms the action itself (sensor logging, health logging, live
  tracking), `SUCCESS: <text>`. Judge success by the absence of the three failure
  leads, not by an `OK` lead.
- **No console spam.** JSON replies are returned only to the caller - they are *not*
  broadcast to other consoles. Plain (non-json) calls behave exactly as before.
- **Transport.** Over the BLE secure channel, responses are chunked by the
  secure-channel layer (frames <=195 B payload); reassemble frames as you do
  today. Plaintext BLE notifications are not chunked: one notification of at
  most MTU-3 bytes (514 with the 517 MTU), so long results need the secure
  channel.
- **WARNING: 4 KB result ceiling (important).** Every command that runs through the
  cmd_exec path - **both BLE and the web `/api/cli`** - copies the handler's result
  into a fixed **4096-byte** buffer (`ExecReq.out` = `CMD_RESULT_MAX`), including
  the trailing NUL. The central registry checks `resultLen >= outSize` and returns
  `Error: result too large...` instead of copying a torn/truncated handler result;
  transports may supply an output buffer smaller than the 4,096-byte maximum. The
  System Event catalog adapter additionally preflights its exact size against the
  fixed command-result buffer. (This section said 2048 until the buffer was raised
  alongside `GLOBAL_DEBUG_BUFFER_SIZE`; the inbound `ExecReq.line` array is the one
  that is still 2048 bytes including its trailing NUL.) This ceiling is upstream of
  BLE framing, so chunking does not rescue an oversized command result. Almost every
  command's JSON is well under 2 KB; the one that can exceed the result budget is the
  **aggregate `sensors json`** when several sensors are active (ToF's data alone is
  ~1 KB). See sec. 6 - fetch sensors per-sensor, not via the aggregate, over BLE.
- **Feature compiled out -> never a crash.** A command whose feature is not in the
  build resolves to ONE of:
  - **Not registered** -> `"Unknown command"` reply (most features), or
  - **JSON stub** -> `{"schema":1,"available":false,"reason":"..."}` (the web/HTTP-gated
    ones: `banlist`, `httpstatus`, `sessionlist`).
  Treat any unknown-command / non-JSON / `available:false` reply as "feature
  unavailable." A web server that is enabled-but-not-started still answers fine
  (data is file-backed).

---

## 2. Change log (this session, by area)

1. **Automation** - descriptive errors on `automation run`; `json` on `automation system`.
2. **Network/system status** - `blestatus`, `wifistatus`, `mqttstatus`, `httpstatus`.
3. **Status/list batch** - `fsusage`, `sdinfo`, `wifilist`, `blepeers`, `images`,
   `maplist`, `oledstatus`, and ESP-NOW `espnowrooms`/`espnowsessions`/
   `espnowrouterstats`/`espnowsubs` (`espnowsubs` was removed in ESP-NOW 1.0).
4. **Web-off stub hardening** - `banlist`/`httpstatus`/`sessionlist` stubs now emit
   `available:false` JSON instead of text.
5. **Deferred set** - `memreport`, `banlist`, `miclist`, `cameravideolist`, `ring status`.
6. **Second-ring diagnostics** - `espnowidentity`, `espnowmeshmetrics`,
   `i2chealth`, `i2cmetrics`, `servolist`, `mqttexternalsensors`, `logtier`, `map`,
   `whereami`.
7. **Sensors** - every live sensor read now has JSON, and `sensors json` is the
   preferred aggregate (see sec. 6).
8. **System Event vocabulary** - `events kinds json` and
   `GET /api/events/kinds` now use one shared, allocation-free serializer over
   the native typed catalog (see sec. 10).
9. **Bluetooth link words** - `blestatus json` gains `link` and `hint`;
   `GET /api/ble/status` gains `link` and the phone object (schema 2);
   `status json` `connectivity.bluetooth` gains the link keys; the web SSE stream
   gains `phone-status` (see sec. 11).
10. **Phone link block (phone link v2)** - `bleinfo json`, `blestatus json` and
   `GET /api/ble/status` gain `phoneLink` (full form), `status json`
   `connectivity.bluetooth` gains it in compact form; the `console` word puts the
   pipe's console stream first; `blemode` accepts `glasses`
   (see sec. 11.9).
11. **Versions** - `versions json` is new, and `status json` gains the same keys
   as its `versions` object (see sec. 4).
12. **Phone path and LLM reasons (2026-11-19, additive)** - `phone status json` and
   the `/api/ble/status` phone object gain the optional `pathState`, `pathText`,
   `retryInS` and `attempt` (sec. 11.6); `llmgenerate json`, `llmretry`, `llmload`
   and `llmask json` gain an optional `reason`, and `llmstatus json` can be `READY`
   with a non-empty `error` (sec. 11.10). The `phone-status` SSE event is unchanged.
13. **`features` only lists (2026-11-20)** - `features json` is unchanged (schema 1,
   same keys and values; now listed in sec. 4). `features <id> on|off` is refused
   with an `Error:` naming that feature's own commands and writes nothing; switch a
   feature with `open<x>`/`close<x>`, `<x>autostart` and `<x>enabled` instead. The
   `feature_toggled` event is no longer posted (sec. 10).
14. **Device name default (2026-11-20, additive)** - `bleinfo json` and the
   `GET /api/ble/status` `ble` object gain `deviceNameSet` (14 keys); `status json`,
   `GET /api/system` and the SSE `system` event gain top-level `deviceName` and
   `deviceNameSet`, and `connectivity.espnow` gains `deviceNameSet`; `espnowdeviceinfo json`
   gains `deviceNameSet` (sec. 4). The fallback name of an unnamed device is
   `HardwareOne-` and the last two address bytes (`HardwareOne-B2C3`, was three), and an
   unnamed sender in the pairing list is `peer-B2C3`. Tell an unnamed device by
   `deviceNameSet`, never by comparing names.
15. **LED targets and the encoder board (2026-11-21, additive)** - the LED commands
   take an optional first word naming the light: `onboard`, `encoder` or
   `encoder:<n>` (sec. 4). Left out, it is `onboard` (the encoder on a board without
   an onboard LED), so every command the app sends today is unchanged. New:
   `ledstatus json` and, in builds with the encoder board, `encoderstatus json`.
16. **Encoder boards 2-4 (2026-11-22, additive)** - up to four encoder boards. Board 1 is
   `encoder` as before; board n (2-4) is the LED target `encoder<n>` / `encoder<n>:<p>`
   (sec. 4), `encoderstatus <n> json` adds `"instance":n`, and `ledstatus json` lists each
   board 2-4 that has an address as one more `lights` object with `"target":"encoder<n>"`
   and `"instance":n`. Board 1's objects are byte-identical, with no `instance` key.

---

## 3. Automation

| Command | Reply |
|---|---|
| `automation system status json` | `{"schema":1,"enabled":<bool>}`; `enabled` is whether the scheduler runs now (since 2026-11-21; it was the saved allowed switch, now removed: `automationautostart` says whether it starts at boot) |
| `automation system enable json` / `disable json` | starts or stops the scheduler now (nothing is saved), then returns `{"schema":1,"enabled":<bool>}` (the resulting state - toggle + read in one call) |
| `automation run id=<id>` | success `OK`; failure is a descriptive `Error: ...` string (e.g. `Error: automation id not found`), `ok=false`. Two cases that are failures too: a run held back by its condition (`Error: Automation not run, its condition is not met (<condition>). Run it again when the condition holds.`) and a run with steps that did not run (`Error: <k> of <n> commands did not run. First reason: <reason of the first such step>`). A queued step `automation run id=<id>` that its target's condition holds back is not logged as a failed step and posts no dropped-action event. A missing `id=` answers `Usage: automation run id=<id>`, also a failure (sec. 1) |
| `automation trigger id=<id>` | arms the automation's manual delay timer; success `OK: Armed automation id=<id> (fires in <n> ms)`; failure `Error: ...`, `ok=false`, also when the scheduler is not running (`Error: The automation scheduler is not running. Run 'automation system enable' first.`) or the automation is disabled (`Error: Automation <id> is disabled. Run 'automation enable id=<id>' first.`). A missing `id=` answers `Usage: automation trigger id=<id>`, also a failure (sec. 1) |
| `automationlist json` | (pre-existing) raw `{"version":1,"automations":[...]}` document; the document is the whole reply. A list over the reply limit answers `Error: The automation list is <n> B, over the <m> B reply limit. Read it on the web Automations page.` |

Note: read the global on/off via `automation system status json`, not from
`automationlist`. The list is the automation documents; the flag is a system setting.

---

## 4. System / network / storage

| Command (+`json`) | Reply shape |
|---|---|
| `blestatus` | `{"schema":1,"initialized","state","activeConnections","maxConnections","totalConnections","commandsReceived","responsesSent","connections":[{"index","connId","authed","user","mac","connectedSec","commands"}],"link":{...},"phoneLink"?:{...},"hint"?}`; the console keys from `state` to `connections` are present only while `initialized` (the app console server runs; the console on the phone link shows only in `link` and `phoneLink`). `link` and `hint`: sec. 11.3; `phoneLink` (pipe builds): sec. 11.9 |
| `bleinfo` | `buildBleInfoJson`'s 14 keys (`schema`, `deviceName` (the one device name, `devicename`; `HardwareOne-B2C3`, the last two address bytes, when none is set), `deviceNameSet` (false when `deviceName` is that fallback: show it with "(default)" and never save it as the name), `txPower`, `autoStart`, `requireAuth`, `secureChannelRequired`, `initialized`, `state`, `connections`, `maxConnections`, `totalConnections`, `commandsReceived`, `clients`), plus `phoneLink` (full form, sec. 11.9) in pipe builds. Errors: `{"error":"oom"}`, `"unavailable"`, `"status too large"` |
| `wifistatus` | `{"schema":1,"connected","ssid","ip","rssi","mac"}` or `{"schema":1,"connected":false,"savedSsid"?,"mac"}` (`savedSsid` is omitted when the saved list cannot be read) |
| `wifilist` | `{"schema":1,"networks":[{"index","priority","ssid","hidden","primary"}],"count"}` |
| `mqttstatus` | `{"schema":1,"enabled","connected","host","port","user","baseTopic","publishIntervalMs","lastError"?}` |
| `httpstatus` | `{"schema":1,"running","https","port"}` (stub: `{"schema":1,"running":false,"compiled":false}`) |
| `fsusage` | `{"schema":1,"ready","totalBytes","usedBytes","freeBytes","usagePercent"}` |
| `sdinfo` | `{"schema":1,"supported","mounted","type","totalMB","usedMB","freeMB","mount"}` (or `supported:false` / `mounted:false`) |
| `memreport` | `{"schema":1,"dram":{"total","used","free","minFree","peakUsed","allocatorMeta"},"psram":{"available","total"?,"used"?,"free"?},"sections":{"bssInternal","bssPsram","noinitInternal","noinitPsram"},"heapCaps":{"internalFree","internalLargest","internalFragPct","dmaFree","dmaLargest","spiramFree"?,"spiramLargest"?},"allocationTraffic"?:{"enabled","generation","tags","capacity","attempts","successes","failures","fallbacks","dramBytes","psramBytes","contentionDrops","invalidTagEvents","overflowEvents","semantics"}}` (bytes; `internalFragPct` is a percent; the `psram` sizes and the two `spiram` keys only when `available` is true; `allocationTraffic` is cumulative requests since the tracker's reset, not live memory) |
| `logtier` | `{"schema":2,"tier","sealed","cardFull","keyUnavailable","noKeyLines","movedAside","reserveDrops","overflow","littlefs":{"free","total","used"},"sd":{"available","total","used","free"}}` (`tier`: `"sd"` while log lines go to the card, where they are sealed, else `"littlefs"`; `sealed`, `cardFull`, `keyUnavailable`, `overflow` booleans; the rest numbers. Schema 1, `{tier, overflow, littlefs, sd}`, is not sent any more) |
| `oledstatus` | `{"schema":1,"connected","address","width","height","rotation","enabled","mode"}`; `width` / `height` are the logical canvas (64 x 128 on an SSD1306 turned to portrait), `rotation` the applied `oledrotation` in degrees (0, 90, 180, 270; added 2026-11-23) |
| `ledstatus` | `{"schema":1,"default","lights":[...]}`: `default` is the target an LED command without one reaches (`onboard`, or `encoder` on a board without an onboard LED). One object per light the build has, connected or not: `{"target":"onboard","present":true,"connected":true,"color","rgb":[r,g,b],"brightness","effect"}`, or `{"target":"onboard","present":false}` on a board without one; with the encoder board built in, `{"target":"encoder","present":true,"connected","product","board","bus","address","brightness","pixels":[{"pixel","color","rgb":[r,g,b],"effect"}]}` for board 1, then the same object for each encoder board 2-4 that has an address set (or is connected), with `"target":"encoder<n>","instance":n` first (`{"target":"encoder2","instance":2,"present":true,...}`); board 1's object has no `instance` key. `color` is the colour word the last command gave (`off` after `ledclear`, and on the onboard light after an effect), `rgb` that colour before brightness, `effect` the running effect or `off`, `brightness` the saved percent. `product` (5752 or 4991) and `board` are `0` and `""`, and `pixels` is empty, until a board has been detected; `bus` is 0 for I2C1. Errors (text): `Error: The encoder light state is busy. Retry.`, `Error: The LED status outgrew the reply buffer.` |
| `encoderstatus` | Builds with the encoder board. `{"schema":1,"connected","product","board","pixels","bus","address","brightness","autoStart"}`; `product` 5752 (4 pixels) or 4991 (1 pixel), `0` with `board` `""` and `pixels` 0 until a board has been detected. `encoderstatus <n> json` (n 2-4) answers for board n with the same keys plus `"instance":n` after `schema`; its `address` is 0 while none is set. Board 1's object has no `instance` key |
| `anoencoderstatus` | Builds with the ANO encoder (`INPUT_DEVICE_TYPE` 2). `{"schema":1,"connected","product","bus","address","pollMs","mount","mirror","autoStart"}`; `mount` (0, 90, 180, 270) and `mirror` are `anomount` (2026-11-23; they replace `invert`, `swapUpDown` and `swapLeftRight`: both swaps were mount 180, one swap a mirror); `connected` is true while the encoder is open, `product` 5740 then and 0 otherwise; `bus` and `address` are where it was opened while connected, else the settings (`inputbus`, `anoencoderi2caddr`, 0x49..0x50); `pollMs` is `inputdevicepollms`, `autoStart` is `inputautostart` |
| `imustatus` | Builds with the IMU. `{"schema":1,"connected","backend","bus","address"[,"magPart","magAddress"],"hwTap","hwFreefall","hasMag","hasHeading","gyroOffset","sampleMs","pollMs","autoStart","heldBy","sampleHook"}`; `connected` is true while the IMU is open; `backend` is the build's chip (`"BNO055"`, or `"LSM6DSOX"` for the Adafruit 4517, which adds `magPart` `"LIS3MDL"` and its `magAddress`); `bus` (0 or 1) and the addresses are where it was opened while connected, else the settings (`imubus`, `imuaddress`, `imumagaddress`); `hwTap` / `hwFreefall`: the chip detects taps and free falls itself; `hasMag`: a magnetometer is read; `hasHeading`: yaw is a heading from magnetic north (false: it counts from the IMU's start); `gyroOffset` is `"chip"` (the chip keeps its own), `"closed"` (measured at each start, the IMU is not open), `"measuring"`, `"still"` (measured on a still board) or `"moving"` (the board never held still: no offset, yaw drifts faster until the IMU is opened again); `sampleMs` is how often the chip is read, `pollMs` (`imudevicepollms`) how often the reading is refreshed, `autoStart` is `imuautostart`; `heldBy` names what keeps the IMU open (`""` when nothing does; `"input"` for the IMU input device; several are separated by `, `): while it is not empty `closeimu` answers `Error: The IMU is in use by ...`; `sampleHook` is true while a consumer takes every sample |
| `imuinputread` | Builds with the IMU as the input device (`INPUT_DEVICE_TYPE` 5; added 2026-11-23). `{"schema":1,"state","running","connected","x","y","buttons","leanX","leanY","taps","shaking","presses","recentres","lastPress","lastTaps","drops","lastDrop":{"taps","hit","why"},"tapWindowMs","tiltDeg","shakeCalibrate","mount","mirror","learning"}`; `state` is `"closed"` (run `openinput`), `"starting"` (the IMU is opening), `"calibrating"` (a shake started a tilt calibration: set the device down and hold it still 3 s), `"centring"` (the centre pose is being taken: hold still), `"ready"`, `"imu-closed"` (the IMU closed under the input) or `"unsupported"` (the build's IMU chip detects no taps); `running` is true while the input is open and `connected` while the IMU's samples arrive; `x` / `y` are the joystick the menus see (0..1023, centre 512; x above centre is right, y above centre is up); `buttons` are the buttons shown as held, active high: A bit 0, B 1, X 2, Y 3, START 4, SELECT 5 (each for about 100 ms after its taps); `leanX` / `leanY` are degrees from the centre pose (right and up positive); `taps` counts the run of taps in progress; `presses` and `recentres` count since the input opened; `lastPress` is the buttons of the most recent press (same bits as `buttons`) and `lastTaps` the taps in the run that made it, both kept until the next press; `drops` counts runs of taps that made no button since the input opened, and `lastDrop` is the last one: its taps, `hit` `"side"` or `"face"`, and `why` (`"never-calm"`: the device did not settle within about a second of the last tap, `"gap"`: the samples stopped, `"shake"`: a shake or calibration began, `"no-button"`: the count has no button); `tapWindowMs`, `tiltDeg`, `shakeCalibrate`, `mount` (0, 90, 180, 270) and `mirror` are its settings (`imuinputtapms`, `imuinputtilt`, `imuinputshake`, `imuinputmount`; `mount`/`mirror` replace `swapXY`/`invertX`/`invertY`, 2026-11-23), and `learning` is true while the teach step (`imuinputlearn`, or the one offered after a calibration) waits for a tilt |
| `imutaptrace json` | Builds with the Adafruit 4517 backend (`IMU_DEVICE_TYPE` 2; added 2026-11-23). `{"schema":1,"detector","thresholdMg","maxMs","ringMs","total","counts":{"tap","soft","long","ring"},"feed":{"reads","failed","empty","samples","overruns","unread","pattern","lastMg":[x,y,z],"jumpMg","inKnock"},"knocks":[{"n","peakMg","durMs","gapMs","v","vec":[x,y,z],"dir","share","hit"}]}`; `detector` is false when the chip uses its own tap engine (the LSM6DSOX; then `knocks` is empty); each knock (the last 24, oldest first, `n` numbering them since the last `imutaptrace clear`) has its peak jump in mg, its length, the gap since the previous knock's start (-1: none), its verdict (`"tap"`, `"soft"` under the threshold, `"long"` over `maxMs`, `"ring"` within `ringMs` of a hard knock), the jump per axis at its peak, the main axis (`"+X"`..`"-Z"`) and its share in %, and `hit` (`"side"` or `"face"`, from `imutapsideaxis` / `imutapsideratio`); `feed` is the FIFO stream behind it |
| `temperature` | `{"schema":1,"tempC","tempF"}` (both `null` when the chip has no believable reading) |
| `time` | `{"schema":1,"synced","source","time","timeEpochMs","tzOffsetMinutes","uptime_ms","rtc_temp_c"?}`; `time` is the system clock in device-local time as `YYYY-MM-DDTHH:MM:SS`, `""` while the clock is not set (`synced` is then `false`; on a board with a DS3231 the chip's own reading is `rtcread`, never this field); `timeEpochMs` is the same reading as UTC epoch milliseconds, `null` while the clock is not set; `tzOffsetMinutes` is the device's fixed offset (minutes east of UTC, no automatic daylight saving): local time = `timeEpochMs` + `tzOffsetMinutes` minutes. `source` is what set the clock this boot: `none`, `rtc`, `ntp`, `manual`, `ring`, `cm5`, `mesh` (the mesh master, whose clock is not from NTP) or `mesh-ntp` (the mesh master, whose clock is from NTP); `none` with `synced:true` is a clock carried over a restart. `rtc_temp_c` only with an RTC reading |
| `sessionlist` (admin) | `{"schema":1,"sessions":[...]}`. A web session: `{"sid","user","transport":"web","createdAtEpochMs","createdAtUptimeMs","lastSeenEpochMs","lastSeenUptimeMs","expiresAtEpochMs","expiresAtUptimeMs","ip","current"}`; `sid` is the first 8 characters and `...`. Each `...EpochMs` is UTC epoch milliseconds, `null` while the device's clock is not set; each `...UptimeMs` is the device's `millis()` stamp (32 bit, wraps after 49.7 days). A key never changes its meaning with the clock (until 2026-11-22 `createdAt`, `lastSeen` and `expiresAt` held epoch ms or uptime ms depending on it; those three keys are gone). The other transports: `{"user","transport"}` with `serial`, `uart`, `oled`, `g2` or `bluetooth`. `/api/sessions` returns the same rows under `{"success":true,"sessions":[...]}` |
| `pendinglist` (admin) | `{"schema":1,"pending":[{"username","requestedAtEpochMs"}]}`; `requestedAtEpochMs` is when the account was requested, UTC epoch milliseconds, `null` when the clock was not set then (it replaces `timestamp`, an uptime). `/api/admin/pending` returns the same rows under `{"success":true,"pending":[...]}` |
| `sensorlog status json` | `{"schema":1,"running","file"?,"intervalMs","format","maxSizeBytes","rotations","sensors":[names],"autoStart","lastWriteAgoSec"?,"approxSizeBytes"?}`; `format` is `text`, `csv` or `track`; `file`, `lastWriteAgoSec` and `approxSizeBytes` (the logger's running estimate) appear only while running; a reply that does not fit is `{"schema":1,"ok":false,"error":"response too large"}`. Also the second entry of `GET /api/logging/status` |
| `log status json` (admin) | `{"schema":1,"state","running","file"?,"lastWriteAgoSec"?,"outputFlags"?,"autoStart","maxSizeKb","keep","rotations","reserveSkips","reserveKb"}`; `state` is `active`, `configured` (started but not routed to the file) or `inactive`; `file` appears while running, `lastWriteAgoSec` and `outputFlags` while active; oversize gives the same `response too large` object. Also the third entry of `GET /api/logging/status` |
| `voltage` | `{"schema":1,"measured":false,"estimatedCurrentMa","estimatedPowerW","note"}` - estimate only; use `batterystatus json` for measured power |
| `i2chealth` | `{"schema":1,"deviceCount","devices":[{"address","name","consecutiveErrors","totalErrors","degraded","nack","timeout","busError","adaptiveTimeoutMs"}]}` |
| `i2cmetrics` | `{"schema":1,"uptimeSec","totalTransactions","mutexTimeouts","busContentions","totalBytes"}` |
| `blepeers` | `{"schema":1,"peers":[{"name","displayName","connectable","connected","autoReconnect","mac1","mac2"?,"pairedBy"}],"count"}`. Errors (text, not JSON): `Error: Out of memory. Free memory and retry the command.`, `Error: Peer list too large for the reply. Run blepeers without json.` |
| `banlist` | `{"schema":1,"bans":[{"ip","reason"}],"count"}` (stub: `available:false`). IP bans are web-only. |
| `versions` | `{"schema":1,"firmware",...,"idf","arduino"}`: every value a string, keys in this order and present only where the build compiles that part. `firmware`; OTA layouts: `ota_layout`, `updater` (`unavailable` without a factory updater), `ota_manifest_format`, `ota_data_schema`; ESP-NOW: `espnow` (`custom ESP-NOW v1`), `espnow_wire` (the frame version byte, JSON only), `espnow_stack` (`v1`/`v2`, `unknown` until ESP-NOW has started), `espnow_identity_file`, `espnow_peer_file`; automations: `automations_file`; migration tool: `backup_format`; Bluetooth: `ble_console_channel`; phone pipe: `phone_link`, `phone_pipe_channel`, `phone_console_records`; CM5 host control: `cm5_control`; C6 companion: `c6_hosted`, `c6_hosted_qualified`, `c6_bridge_wire`, `c6_bridge_wire_expected` (reported values `unknown` until the companion answers); local STT: `stt_model`; then `idf`, `arduino`. `status json` carries the same keys, without `schema`, as `versions`. Errors (text, not JSON): `Error: Out of memory. Free memory and retry the command.`, `Error: The version list does not fit one reply. Run versions without json.` |
| `features json` (admin) | `{"schema":1,"features":[{"id","name","category","heapKB","compiled","enabled","toggleable"}]}`, one object per feature, every feature in one reply (a missing id means old firmware: assume present; the one exception is `encoder`, listed only in builds that have the encoder board, so a missing `encoder` means not built). `heapKB` is 0 when `compiled` is false. `enabled` is the feature's saved switch: start at boot on every feature (since 2026-11-21 also wifi, espnow, mqtt, oled, edgeimpulse and automation, whose "allowed" switches were removed), the start-up effect on led, the bus switch on i2c, and `true` for a built-in feature with no switch; it is not "running now". `toggleable` is true when the feature is built in and has a saved switch. Unchanged when `features` became list-only (2026-11-20): `features <id> on\|off` is refused, so `toggleable` says the feature has a switch, not that `features` can set it; the text `features <id>` names the commands that do. Error (text): `Error: feature list outgrew the response buffer` |
| `c6status` | P4X-EYE builds. `{"schema":1,"backend","state","transport_up","held","operation","outage","auto_recover","auto_hold","reboot_escalation","console_mirror","firmware":{"hosted"?,"match"?,"expected","project"?,"version"?,"idf"?,"built"?},"bridge":{"state","wire"?,"image_state"?,"slot"?,"build"?,"ble_max_connections"?},"ble_links","heartbeat":{"interval_sec","seen","age_ms","missed"},"boot_count","last_reset_reason","chip"?:{"id","name"},"companion"?:{"uptime_ms","free_heap","min_free_heap","cpu_mhz","reset_reason","native_espnow"},"memory"?:{"total","free","min_free","internal_free","internal_largest","age_ms"},"bridge_counters":{"timeouts","transport_errors","events_dropped","malformed","stale","hci_tx_down_drops"},"recovery":{"start_attempts","start_failures","soft","failed","hard_requested","outages","lifetime_outages","lifetime_reboots","last_fault_epoch_ms","outage_age_ms","next_retry_ms","online_age_ms","events_dropped"},"console_mirror_reason","last_result"}`. `recovery.outages` counts this boot's incidents; `lifetime_outages` and `lifetime_reboots` survive restarts (saved after a fault, 0 after a factory reset); `last_fault_epoch_ms` is the UTC milliseconds of the last fault, or `null` (none yet, or the clock was not set at the last fault). `console_mirror_reason` is `off`, `saved` (`c6console on`), `fault` (on for 10 minutes after a companion fault), `boot` (on while the device boots; stopped when boot finishes) or `unsupported`. The `companion` object of `status json` is this reply up to `last_reset_reason` and is unchanged. Errors (text, not JSON): `Error: Out of memory. Free memory and retry the command.`, `Error: Companion status too large for the reply. Run c6status without json.` |

The LED commands (`ledcolor`, `ledclear`, `ledeffect`, `ledbrightness`) take an optional first
word naming the light: `onboard`, `encoder` (every pixel of encoder board 1) or `encoder:<p>`
(one pixel, 1-based); encoder board n (2-4) is `encoder<n>` and `encoder<n>:<p>`, the `target`
that `ledstatus json` gives it (`ledbrightness` takes `onboard`, `encoder` or `encoder<n>`, not a
pixel). A first word that is not one of these is the colour or effect, and the default light
(`ledstatus`'s `default`) gets the command. Replies are text: a light the build or board does not
have, a board that is not connected, a pixel past the board's count, `encoder1`
(`Error: 'encoder1' is not a target; instance 1 is 'encoder'.`) and a board past the maximum
(`Error: encoder has instances 1-4. Usage: ...`) answer `Error: ...`.

`status json` / `GET /api/system`, `storage` object: `total_kb`, `used_kb` and `free_kb` are absent when the
internal storage reading is unavailable (they were 0); `storage` itself is always present, and `storage.sd` is
absent without a readable card.

`status json` / `GET /api/system` / the SSE `system` event, device name: top-level `deviceName` (the name in use:
the stored name, else the fallback `HardwareOne-B2C3`; never empty, never with "(default)") and `deviceNameSet`
(false when `deviceName` is the fallback), in every build. In ESP-NOW builds `connectivity.espnow` keeps
`deviceName` as the stored name (`""` when none) and gains `deviceNameSet` beside it. `connectivity.espnow.enabled`
is `espnowautostart` (start at boot, as `connectivity.mqtt.enabled` is `mqttautostart`); until 2026-11-21 it was the
removed `espnowenabled` switch. `running` says whether ESP-NOW runs now.

---

## 5. Lists / media / ESP-NOW

| Command (+`json`) | Reply shape |
|---|---|
| `images` (or `images sd`) | `{"schema":1,"location","available","usedBytes","totalBytes","imageCount","images":[{"filename","size"}]}`. `usedBytes` and `totalBytes` are 64-bit values: an SD card above 4 GB reports its real size, so do not read them as 32-bit integers |
| `maplist` | `{"schema":1,"maps":["/maps/..."],"count","truncated"?}`; `"truncated":true` is present only when the device holds more than the 8 maps listed |
| `miclist` | `{"schema":1,"count","recordings":[{"filename","size"}]}` |
| `cameravideolist` | `{"schema":1,"count","recordings":[{"filename","size"}]}`; without an SD card the reply is the text `Error: SD card not available. Insert a card or run sdmount, then retry.` (non-JSON, see sec. 1) |
| `servolist` | `{"schema":1,"servos":[{"channel","name","minPulse","maxPulse","centerPulse"}],"count"}` |
| `map` | `{"schema":1,"valid","filename","region","featureCount","fileSize","bounds":{"minLat","minLon","maxLat","maxLon"}}` |
| `whereami` | `{"schema":1,"valid","road"?,"roadDistanceM"?,"area"?,"areaDistanceM"?}` |
| `mqttexternalsensors` | `{"schema":1,"sensors":[{"name","value","ageSec"}],"count"}` |
| **ESP-NOW** | |
| `espnowstatus` | `{"schema":1,"initialized","channel"?,"channelPref"?,"masterChannel"?,"follow"?,"masterSilentMs"?,"sweepFailures"?,"nextSweepMs"?,"mac"?,"pairedDevices"?,"lastInitError"?}`. Before the first start of a boot only `initialized:false` (plus `lastInitError` after a failed start). `channel` and `channelPref` (0 = auto, 1-13 = pinned when offline) appear once a start has set up the ESP-NOW state and stay after `closeespnow`; `mac` and `pairedDevices` are present only while `initialized`; `masterChannel` (0 = unknown), `follow` (the channel-follow phase), `masterSilentMs`, `sweepFailures` and `nextSweepMs` are present only while `initialized`; `lastInitError` is the reason the last start failed, present only while ESP-NOW is down. A reply that does not fit is `{"schema":1,"ok":false,"error":"response too large"}` |
| `espnowdevices` *(v0.95.10)* | all peers: `{"schema":1,"devices":[{"mac","deviceName","friendlyName","room","zone","tags","pendingRoom"?,"pendingTags"?,"stationary","online","lastSeenSec","source"}],"count","notShown"?}` |
| `espnowdeviceinfo` *(v0.95.10)* | this device (self): `{"schema":1,"name","nameValid","deviceNameSet","friendlyName","room","zone","tags","stationary","meshRole","mac","hint"}`; `nameValid` is false when no usable device name is saved, and `deviceNameSet` has the same value (the key the other replies use); `name` is `""` then (ESP-NOW never uses the fallback name) |
| `espnowmode` (admin) | `{"schema":1,"enabled","mode"}`; `mode` is `direct` or `mesh`; `enabled` is `espnowautostart` (start at boot; until 2026-11-21 it was the removed `espnowenabled` switch) |
| `espnowmeshrole` (admin) | `{"schema":1,"role","masterMac","backupEnabled","backupMac"}`; `role` is `worker`, `master` or `backup`; the MACs are `""` when unset |
| `espnowrooms` | `{"schema":1,"rooms":[{"room","devices":[{"name","tags","online","accepted","pendingTags"?}]}],"count","notShown"?}` |
| `espnowgroups` (or `espnowgroups json <name_or_mac>`; admin) | `{"schema":1,"peers":[{"mac","name","room","tags","acceptedRoom","acceptedTags","pendingRoom","pendingTags"}],"count","pendingCount","notShown"?}` |
| `espnowgroupaccept json <name_or_mac\|all>` / `espnowgroupdrop json <name_or_mac\|all>` (admin; `json` leads) | `{"schema":1,"peers":[{"mac","name","room","tags","acceptedRoom","acceptedTags","pendingRoom","pendingTags","changed"}],"ok","count","changed","saved","error"?,"notShown"?}` |
| `espnowsessions` | `{"schema":1,"selfCaps":[...],"sessions":[{"slot","mac","meshId","sessionId","dir","state","ageMs","txSeq","rxHwm","caps"?,"capsWord"?}],"more"?}` - `meshId` is the mesh slot index. `selfCaps` / `caps`: the names of the capability bits set in this device's / the peer's session word (`bond`, `remote_fs`, `file_receive`, `sensor_send`, `collect`, `relay_forward`, `time_master`, `session_unknown`, and the 1.x names `backlog_serve`, `files_serve`, which a 1.0 build never sets but shows when a peer does; a bit with no name is left out of the list and kept in `capsWord`); `caps` and `capsWord` (the whole 32-bit word, reserved bits included) only for an ACTIVE or REKEY session. `more`?: present only when the list did not fit the reply (about 13 sessions with every capability set); the number of sessions left out at the end |
| `espnowrevoked json` (admin) | `{"schema":1,"locked","max","revoked":[{"name","mac","keyId","meshSlot","revokedEpochMs"}],"identities":[{"mac","name","keyId","meshSlot","inDeviceList"}]}` - `locked`: revoked.json is there but cannot be read (new keys and pairing are refused). `keyId`: the first 16 hex digits of the key, `null` for a device revoked by address with no stored key. `revokedEpochMs`: UTC epoch ms, `null` when the clock was not set. `identities`: every peer key stored on this device; `name` is `""` and `inDeviceList` false for one whose device is not in the device list. A reply that does not fit is `{"schema":1,"ok":false,"error":"too large"}` |
| `espnowrouterstats` | `{"schema":1,"messagesSent","messagesReceived","messagesFailed","v4FragTx","v4FragRx","reassembled","reassemblyGc","reassemblyTimeouts","nextMessageId"}` |
| `espnowidentity` | `{"schema":1,"valid","mac","pub","createdAtSec","regenCount"}` |
| `espnowmeshmetrics` | `{"schema":1,"mode","activePeers","ttl","adaptiveTtl"}` |
| `ring status` | `{"schema":1,"connected","name","mtu","rx","upMs","scanFound"}` (no address) |
| `espnowmeshstatus` / `espnowlist` / `espnowstats` | already JSON (pre-existing). `espnowstats json` adds `rxLengthDrops`, `rxMustUnderstandDrops`, `rxRevokedDrops` (frames from a revoked radio source or relayed from a revoked origin), `rxUnknownMeshIdDrops` and, once it is nonzero, `rxUnknownMeshIdLast` (the last mesh id dropped) and `rxUnknownMeshIdLastMac` |

### ESP-NOW peer metadata is pull-only
- **Self** = `espnowdeviceinfo json`; **peers** = `espnowdevices json`.
- The peer cache is refreshed on demand. To get a peer's *current* metadata: send
  `espnowrequestmeta <mac>`, wait ~1-2 s (or poll `espnowdevices json` until the
  fields populate), then read. This mirrors the web's "Sync Metadata" button.
- The device also keeps it current by itself: a peer sends its metadata when it
  changes, and the device asks each paired peer once when a session with it has come
  up (after a restart of either side, and after a key change), so a change made while
  the two could not hear each other arrives without a request from the app.
- Do **not** look up the gateway's own MAC in the peer list - it's never there.

### ESP-NOW room and tag membership
- `room` and `tags` are what a peer **announces** about itself. `espnowroomcmd` and
  `espnowtagcmd` go only by what was **accepted** on this device. `pendingRoom` and
  `pendingTags` are the announced values that are not accepted. Rooms and tags compare
  without regard to letter case.
- `espnowdevices` and the web `/api/espnow/metadata?mac=` reply carry `pendingRoom` and
  `pendingTags` only when there is something pending; a device without them has all of its
  room and tags accepted (or announces none). They carry no copy of the accepted values:
  `espnowgroups json` has `acceptedRoom` and `acceptedTags` (comma-separated) for every
  paired peer, there with `pendingRoom` and `pendingTags` always present (empty when
  nothing waits).
- In `espnowrooms` the rooms are the announced ones; `accepted` says whether a room
  command reaches the device, and `pendingTags` is present when some of its tags wait.
- `espnowgroupaccept json <name_or_mac|all>` accepts what is announced at that moment
  and returns the result per peer, so read `acceptedRoom`/`acceptedTags` from the reply
  instead of assuming the values listed earlier. `ok` is false, with `error`, when the
  change could not be saved; it then holds until a restart.
- `name` is the name the peer was paired under on this device, or its MAC address when
  that name cannot be used as a command argument or is not the name of this peer alone.
  A name that more than one paired device has is refused by the three membership
  commands; use the address.
- `espnowtagcmd` takes one tag. A target with a comma is refused before anything is sent.
- `count` covers every entry. When a reply would exceed the result budget (sec. 1), the
  last entries are left out and `notShown` gives their number: in `espnowgroups` and the
  accept and drop replies the peers left out of `peers` (name one peer to get it), in
  `espnowdevices` the devices left out of `devices`, in `espnowrooms` the devices left out
  of the rooms (a room none of whose devices fit is left out too, while `count` still
  counts it). A reply is never cut in the middle.
- `/api/bond/paired-devices` has `roomAccepted` per device.

---

## 6. Sensors - fetch per-sensor over BLE (do NOT use the aggregate)

**Over BLE, fetch each sensor individually and load them one at a time.** The
aggregate `sensors json` embeds every active sensor's `data` in one reply, which can
exceed the command transport's result budget (sec. 1) and fail with an explicit
`Error:` reply. Each per-sensor `<x>read json` returns a single sensor (<= ~1 KB),
stays safely under the ceiling, and self-describes `valid` / `enabled` /
`connected`.

**Discovery / loading pattern:**
1. Call **`sensors json brief`** once -> a small, bounded enumeration (state only, **no
   embedded `data`**), safe over BLE regardless of sensor count:
   ```json
   {"schema":1,"brief":true,"seq":<n>,
    "sensors":[{"id":"imu","name":"BNO055 orientation","kind":"vector",
                "enabled":true,"connected":true}, ...]}
   ```
   The IMU's `name` follows the build's chip: `"BNO055 orientation"`, or
   `"LSM6DSOX orientation"` on an Adafruit 4517 build. Match on `id`.
2. For each entry that's present (`connected:true`, and `enabled:true` if you only
   want live ones), fetch its `<x>read json` and render as it arrives
   ("one at a time").
3. Skip any `<x>read json` that returns `valid:false` or `"Unknown command"`.

`id` / read-command mapping:

| id | per-sensor command |
|---|---|
| imu | `imuread json` |
| tof | `tofread json` |
| gps | `gpsread json` |
| presence | `presenceread json` |
| fmradio | `fmradioread json` |
| rtc | `rtcread json` |
| input (gamepad) | `gamepadread json` |
| input (IMU taps and tilt, `INPUT_DEVICE_TYPE` 5) | none of its own: its reading is the `input` entry of `sensors json`, in the gamepad's shape `{"valid","connected","ts","x","y","buttons"}` with the gamepad's button bits, active low (A bit 5, B 1, X 6, Y 2, START 16); `imuinputread json` is its status |
| apds | `apdsread json` |
| thermal | `thermalread json` |
| mic | `micread json` / `miclevel json` |
| anoencoder | `anoencoderread json` |
| (internal) | `temperature json`, `voltage json` |

**The aggregate `sensors json` still exists** (shape below) and is fine for the web
sensors page (its own endpoint serializes directly, bypassing the 2 KB cmd ceiling)
or for low-sensor-count devices - but treat it as unreliable over BLE with multiple
sensors active.

```json
{"schema":1,"seq":<n>,"sensors":[
  {"id":"imu","name":"BNO055 orientation","kind":"vector",
   "enabled":true,"connected":true,"data":{ ...live reading... }}, ...
]}
```

`thermal` is a stream sensor, so even in the aggregate it has no embedded `data` -
always read it via `thermalread json`.

### Per-command sensor reads (1:1 parity)
Each also has its own `json`. **Important shape note:** the per-sensor reads that
delegate to the shared builder return the *same object* that appears in the `data`
field of `sensors json` - that object uses a `valid` flag (and often
`enabled`/`connected`), and may **not** carry a top-level `schema`. Parse defensively
(check `valid`). The directly-written ones use `{"schema":1,...}`.

| Command (+`json`) | Returns |
|---|---|
| `imuread`, `tofread`, `gpsread`, `presenceread`, `presencestatus`, `fmradioread`, `rtcread`, `gamepadread`, `apdsread`, `anoencoderread` | the shared builder's `data` object: envelope `{"valid","connected","ts"}` + value keys. `rtcread`: `{"valid","connected","ts","utc":true,"year","month","day","hour","minute","second","temp"}`; the date and time fields are the chip's, in UTC, and `"utc":true` says so (`time json` is device-local time and carries `tzOffsetMinutes`) |
| `thermalread` | envelope + `{"min","avg","max"}` (summary; the 768-px frame is a separate shape) |
| `micread` | `{"schema":1,"enabled","connected","recording","sampleRate","bitDepth","channels","level"}` |
| `miclevel` | `{"schema":1,"enabled","level"}` |
| `cameraread` | camera status JSON (already JSON, no `json` token needed); when the camera is busy, out of memory or the status does not fit, an `Error: ...` text instead (treat as unavailable, sec. 1) |
| `temperature`, `voltage` | see sec. 4 |

**(Corrected 2026-07-28 - the examples below previously showed pre-envelope shapes.
Same day, later: the optional `age` key was removed entirely; derive staleness from `ts`.)**
Every builder object opens with the envelope: `{"valid":bool,"connected":bool,"ts":ms}` -
`ts` is 0 when never sampled. `valid:false` variants keep the envelope. Then per-sensor value keys:
- imu: `+ {"accel":{x,y,z},"gyro":{x,y,z},"ori":{yaw,pitch,roll},"temp"[,"mag":{x,y,z}][,"cal"]}`. Units: `accel` m/s², `gyro` rad/s, `ori` degrees (yaw 0..360, pitch ±90, roll ±180, as the chip reports them unless `imuorientationcorrection` is on; never clamped), `temp` °C. Added 2026-11-22, present only when the build's IMU chip has them: `mag` (magnetometer, µT; the BNO055 has it) and `cal` (the chip's calibration byte; BNO055 CALIB_STAT: system, gyro, accel and magnetometer, two bits each from the top, 3 = calibrated). The Adafruit 4517 (`IMU_DEVICE_TYPE 2`, LSM6DSOX + LIS3MDL) sends the same keys with `mag` and without `cal`; its `ori.yaw` is relative to the IMU's start (no magnetometer calibration yet, so no magnetic heading) and drifts slowly, while pitch and roll are absolute. Ignore keys you do not know. Added 2026-11-22 to `imuread json`, the `imu` entry of `sensors json` and the web page's `/api/sensors?sensor=imu` only (the reading sent over ESP-NOW and MQTT does not carry them): `"hdg"` (bool: `true` when `ori.yaw` is a heading from magnetic north, `false` when it counts from the IMU's start, as on the 4517 today; show a compass only with `hdg:true`) and `"backend"` (the chip that made the reading: `"BNO055"` or `"LSM6DSOX"`). They are the last two keys; a reply that could not get them (the cache was busy) is the object without them. The BLE sensor stream (`blestream`) sends its own short object, `"imu":{"heading","pitch","roll","valid"}`: its keys are unchanged, and since 2026-11-22 `heading` is `null` when the yaw is not a heading (the 4517 today), a number as before otherwise
- gps: `+ {"fix","quality","sats","lat","lon","alt","speed"}`
- apds: `+ {"r","g","b","c","proximity"}`
- anoencoder: `+ {"pos","axis","buttons"}` *(was `{"schema":1,"connected","position",...}` - `schema` dropped, `position`->`pos`, envelope added)*. `axis` is always 0 (the wheel is the vertical axis); `buttons` is the device-native layout, active high: IN bit 0, UP 1, DOWN 2, LEFT 3, RIGHT 4 (START and SELECT are edges and never appear in it). `pos` counts the wheel's detents since the encoder was opened. `/api/sensors?sensor=anoencoder` answers this object in every case (closed: `valid` and `connected` false)
- tof: `+ {"objects":[{"id","distance_mm","status","valid"}]}` (detected objects only; variable length)

---

## 7. Still text-only (do NOT request `json`)

These have no JSON path; requesting it returns text (or "unknown command" if the
feature is compiled out). Treat accordingly.

- **Poll/viz/debug actions:** `apdscolor`, `apdsgesture`, `apdsproximity` (values are
  in `apdsread json` / `sensors json`), `micviz`, `cameradump`.
- **Niche subsystems** (no app panels yet): `ei*` (Edge Impulse), `sr*` (ESP-SR),
  `g2info` / `g2protostats` / `g2sensors`.
- **Edge data not yet covered** (ask if you need them): `imuactions` (IMU action
  detection), `sensorinfo <name>` (static chip catalog).
- **Actions / setters / help:** `banuser`, `espnowrevoke`, `espnowunrevoke`, `blestream`, `bondstream`,
  `oleddefaultmode`, `g2` toggles, module summaries.

---

## 8. Remote sensors, streaming state & automation cadence (NEW - breaking shape change)

This session reworked how a MASTER exposes its bonded/mesh **peers'** sensors. If the app
consumes any of the remote-device endpoints below, it must update. (The LOCAL sensor
contract in sec. 6 is **unchanged**.)

### 8.0 Additions with the 1.0 reading format (2026-11-17, additive)
- `GET /api/sensors/remote?device=<MAC>&sensor=<type>` keeps `connected`, `enabled`, `fresh` and
  `data` and, once a reading has arrived, adds the receiver's own arrival stamp and counts:
  `arrivalEpochMs` (UTC milliseconds, `null` while the receiving device's clock was not set),
  `arrivalUptimeMs`, `agoMs`, `clock` (how the receiving device's clock was set: `rtc`, `ntp`,
  `manual`, `ring`, `cm5`, `mesh`, `mesh-ntp` (a mesh master whose own clock is from NTP; since
  2026-11-22), `carried` for a clock kept over a restart, `none`), `bootTag` and
  `counter` (the sender's: with its MAC and the sensor they name one reading), `received`, `lost`, `late`, `repeated`, `restarts`,
  `resumes` and `incomplete`. `lost - late` is what never arrived; `incomplete` counts readings of
  that sender (any sensor) of which a frame was missing, and is part of `lost`. `data` is at most
  536 bytes.
- `espnowsensorstatus json`: `role` is `worker`, `backup` or `master`. On every device: `targets`
  (the masters named on this device that are paired: `kind` `primary` or `backup`, `mac`, `name`,
  `session`), `masterNamed`, `battery` (`sends`, `whyNot`, `intervalS`, `sent`, `lastSentAgoS`),
  `events` (`sends`, `whyNot`, `floor`, `sent`, `leftOut`) and `table` (`senders`, `sendersMax`,
  `sensors`, `received`, `lost`, `late`, `incomplete`, `malformed`, `refusedFull`,
  `refusedNoMemory`, `badType`, `notReceiver`). `whyNot` is `null` or one of `not_in_this_build`,
  `off`, `mesh_off`, `is_master`, `no_master`, `master_not_paired`, `no_session`. On a device that
  sends (worker, backup): `streaming` (all eleven sensor types) and `sent` (per sensor with
  activity: `sent`, `notSent`, and a count per reason: `too_long`, `no_receiver`, `no_session`,
  `queue_full`, `is_master`, `bond_offline`, `broadcast_off`, `mesh_off`, `not_in_this_build`,
  `master_not_paired`; and `requestsLeftOut` when a master's sensor request named it while it was not
  running here: it is left out of the lease). On a device that receives (master, backup, or any device that holds
  readings): `devices` as in 8.1 (a device's sensors are listed from its sensor status frame before
  its first reading: `connected` true, `enabled` as the device says, `fresh` false, `data` null),
  each with `received`, `lost` and `late` added, and
  `devicesMore`: `false`, or `true` when the reply (4 KB) could not hold every device and the rest
  were left out. A reply that does not fit even without devices is
  `{"schema":1,"error":"the reply does not fit its buffer"}`.
- `espnowsensorstatus json <name_or_mac>`: everything kept of one sending device: `mac`, `name`,
  `paired`, `incomplete`, `malformed`, `sensors` (per sensor name the keys of
  `/api/sensors/remote` without `data`), `battery` (one row of `espnowbattery json`, or `null`)
  and `events` (`received`, `stored`, `lost`, `late`, `restarts`, `resumes`, `senderSkipped`,
  `refusedRate`, or `null`). `events` is there for an admin only: for any other user the key is
  left out and `eventCounts` is `"admin only"` (it is absent for an admin).
- `espnowbattery json [<name_or_mac>] [from <n>]` (the battery state other devices sent; kept by
  a mesh master or backup master): `schema`, `role`, `sends` (what this device sends itself, as in
  `espnowsensorstatus json`), `refused` (`notReceiver`, `full`, `noMemory`, `malformed`), `from`,
  `devices` and `next` (`null`, or the number to pass as `from` for the devices that did not fit
  this reply). Each device: `mac`, `name`, `arrivalEpochMs` (the receiving device's UTC clock in
  milliseconds, `null` while it was not set), `arrivalUptimeMs`, `agoMs`, `clock`, `bootTag`,
  `counter`, `status` (`unknown`, `charging`, `full`, `discharging`, `low`, `critical`,
  `not_present`, `high`, `good`, `medium`, `empty`), `backend` (`none`, `adc`, `fuelgauge`,
  `unknown`), `voltage`, `percent`, `present`, `charging`, `usb`, `ratePctPerHr`, `sampleAgeMs`
  (each `null` when the sending device did not know it), `percentEstimated`, `chargingEstimated`,
  `statusEstimated`, `stale`, and the counts `received`, `lost`, `late`, `restarts`, `resumes`.
- `espnowevents json [since <rx>]` (the system events other devices sent; admin only, a user
  without admin gets the command layer's refusal): `schema`, `role`,
  `boot` (this device's boot counter: `rx` numbers start again at 1 after a restart), `latest`
  (the newest `rx`), `since` (as used; a number above `latest` is read as 0), `overwritten` (events
  after `since` that are no longer held), `sends`, `refused`, `senders` (per sending device `mac`,
  `name`, `received`, `stored`, `lost`, `late`, `restarts`, `resumes`, `senderSkipped`,
  `refusedRate`), `events` (oldest first) and `more` (`true`: ask again with `since` = the last
  `rx` of this reply). Each event: `rx`, `mesh`, `mac`, `name`, `arrivalEpochMs` (`null` while the
  receiving device's clock was not set), `arrivalUptimeMs`, `clock`, `ageMs` (how old the event was
  when it was sent), `eventSeq` (its number on the sending device), `bootTag`, `counter`, `tier`
  (`verbose`, `standard`, `alert`, `unknown`), `source` (`unknown`, `cli`, `oled`, `web`, `voice`,
  `remote`, `system`, `g2`), `kind` (the kind's name), `subject`, `detail`, `who`, and `cut` (the
  names of the fields that were cut to fit the frame). An event is identified by (`mac`,
  `bootTag`, `counter`), also at a second master.
- `GET /api/bond/status` adds `sensorBits` (sensor name to mask bit). The bits of
  `sensorMask`, `sensorConnectedMask` and `sensorEnabledMask` changed to the one sensor numbering
  (thermal 1, tof 2, imu 4, gps 8, input 16, fmradio 32, rtc 256, presence 512, apds 1024): read
  the masks through `sensorBits`, not through numbers of your own.
- The ESP-NOW capability manifest's `sensorStatus` object (`thermal`, `tof`, `imu`, `input`,
  `apds`, `gps`, `rtc`, `presence`; keys unchanged) is stricter since 2026-11-22: a value is `true`
  only when this build has that sensor and the last bus scan found it at the bus and address the
  build uses for it (a configured address counts; `presence` looks at the presence sensor's own
  address, not 0x61). Before, any device answering at a matching name counted, whatever the build.
- `espnowtimestatus json`: `schema`, `meshTime`, `role`, `masterNamed` (`false`: no master is named
  on this device, so it takes no mesh time), `clockSet`, `clockSource`, `clockSetAgoS`,
  `epoch`, `sends`, `sendsWhyNot`, `lastSentAgoS`, `framesSent`, `peersSent`, `peersReady`,
  `peersPaired`, `lastFrame` (`mac`, `name`, `agoS`, `source`, `sinceSetMin`, `result`,
  `differenceS`; `null` before any frame) and `counts`. `result` is one of `applied`, `agreed`,
  `ignored_off`, `ignored_mesh_off`, `ignored_master`, `ignored_no_master`, `ignored_not_named`,
  `ignored_better_source`,
  `ignored_weak_source`, `ignored_other_master`, `ignored_too_soon`, `refused_bad_time`,
  `held_large_change`; `differenceS` is 0 for a frame whose time is not a real date. `counts.ignored`
  also counts frames that found no room to wait for the main loop (four senders' frames wait at
  most). `clockSource` is `none`, `carried`, `rtc`, `ntp`, `manual`, `ring`, `cm5`, `mesh` or, since
  2026-11-22, `mesh-ntp`: the clock was set from a named master whose own clock is from NTP
  (`lastFrame.source` `ntp`); `mesh` is a master's time from any other source. `ignored_better_source`
  follows the clock's rank table (ntp > manual > mesh-ntp > cm5 > rtc > ring > mesh): an NTP-backed
  master's time is taken by a device whose clock is from the CM5, the RTC chip or the ring; any
  other master's time only by a clock that mesh time set or that was carried over a restart.

### 8.0.1 Forwarding for a bond partner (2026-11-18, additive)
A bond master may forward its bond partner's readings, battery state, events and sensor list to the
mesh master named on it, when that master is paired in the bond master's primary mesh (`bondforward
on` on the bond master; the mesh master accepts any paired device that announces). A sender is then
identified by its address **and** the device that forwarded it:
- Every device row gets `link` (`mesh`: the device sent it itself; `bond`: it came over the bond link
  to this device, its bond master; `forwarded`: a bond master forwarded it) and `via` (`null`, or
  `{"mac":"..","name":".."}`, the bond master). This holds for `devices` of
  `espnowsensorstatus json` and `GET /api/sensors/remote` (one row per sender: the same `mac` can be
  listed twice, once with `via: null` and once via a bond master), the `devices` of
  `espnowbattery json`, `senders` and `events` of `espnowevents json`, and the reply of
  `GET /api/sensors/remote?device=<MAC>&sensor=<type>&via=<MAC>` (without `via`: the device's own
  record). `name` of a forwarded row is the partner's name (the mesh master's own name for a partner
  paired with it, else the name the bond master announced). Treat (`mac`, `via.mac`) as the key.
- `espnowsensorstatus json` adds `forwardLinks` (the number of active links on this device) and
  `table.forwardRefused` (forwarded frames no link named). `espnowsensorstatus json <name_or_mac>
  [via <name_or_mac>]` adds `link` and `via`; a forwarded partner is also found by its link's name.
  A forwarded partner's sensors are listed (present, streaming) before its first reading, from the
  sensor status the bond master forwards.
- `refused` of `espnowbattery json` and `espnowevents json` adds `notBondMaster` and
  `forwardRefused`. `whyNot` adds `bond_offline` (a bond worker whose partner is not connected).
- `espnowforwardlinks json [from <n>]`: `schema`, `role`, `refused` (`malformed`, `unannounced`, `staleLink`,
  `streamNotAnnounced`, `ignored`), `from`, `links` and `next` (`null`, or the number to pass as
  `from`). Each link: `via`, `viaName`, `partner`, `name`, `namePaired`, `linkGen`, `viaBootTag`,
  `state` (`active`, `ended`, `queried`), `confirmed` (`false` after 30 minutes without an
  announcement), `streams` (`readings`, `battery`, `events`), `sensors` (names), `acceptedAgoS`,
  `lastDataAgoS` (`null`: no data yet), `frames`, `refused` (`unannounced`, `staleLink`,
  `streamNotAnnounced`).
- `bondstatus json` and `GET /api/bond/status` add `forward`: `setting`, `sensors` (names),
  `forwards`, `whyNot` (`null` or `off`, `not_bond_master`, `bond_not_agreed`, `mesh_off`,
  `is_master`, `no_master`, `master_not_paired`, `not_primary_mesh`), `linkGen` (`null` without a
  link), `partner` (`{"mac","name"}` or `null`), `targets` (per named master: `kind`
  `primary`/`backup`, `mac`, `name`, `isPartner`, `inPrimaryMesh` (`false`: paired in another mesh
  than this device's primary one, so it gets nothing), `state` `idle`/`announcing`/`accepted`/`refused`,
  `result` (`null` or the master's last answer: `accepted`, `withdrawn`, `not_receiver`,
  `bad_partner`, `key_mismatch`, `full`, `stale`, `name_taken`, `refused`), `streams`, `session`),
  `forwarded` (`readings`, `battery`, `events`), `notForwarded` (per stream a count per reason: the
  `whyNot` keys and `target_is_partner`, `no_session`, `not_accepted`, `refused`, `queue_full`,
  `too_long`, `partner_changed`, `sensor_off`, `not_kept_here`), `answersIgnored` and
  `sensorRequests` (the mesh master's sensor requests for the partner: `applied`, `refused`,
  `lastRefused` (`null` or `off`, `not_bond_master`, `no_link`, `old_link`, `not_primary_master`,
  `bad_mode`, `not_forwarded`, `partner_offline`, `partner_status_unknown`: the partner has not sent
  its sensor list yet, `partner_lacks_sensor`: the partner's list names none of the forwarded sensors
  asked for), `refusedBy` (a count per refusal reason, the same keys, only those above 0),
  `partnerLacks` (`count`: requests that asked for a sensor the partner does not have running, refused
  or with it left out; `last`: those sensors of the last such request), `leased` (the sensors a lease
  runs for now)). The object is at most 3072 bytes; the whole `bondstatus json` reply fits the 4 KB
  reply buffer.
- `bondstatus json` adds `sensorRequestsRefused`: the sensor requests a mesh master sent while this
  device is a bond worker (refused: its bond master decides what it streams).
- `bondstatus json` adds `roleChosen` (`true`: the user chose `role` with `bondrole`, and
  `bondconnect` keeps it) and `roleSource`: `chosen`, `partner` (not chosen; the partner's user chose
  the other role), `address` (not chosen; the role is the one `bondconnect` gives by the two
  addresses, the higher one master) or `kept` (not chosen, and not the one the addresses give: it was
  set earlier, by a partner's choice since given up or before `bondrole auto`, and stays until the next
  `bondconnect`).

### 8.0.2 File catch-up (2026-11-19, additive; admin only)
Two new commands, one per side (docs/BACKCAPTURE_FILES.md); neither exists on a build without its
switch (`Error: unknown command`).
- Device (`ENABLE_ESPNOW_SEND`): `espnowcatchup json [from <n>]`: `schema` (1), `serve` (the
  setting), `built`, `named` (`{"primary","backup"}`: the masters named on this device, or `null`),
  `keyReady`, `keyId` (16 hex, a pseudonym of the capture key, never the key; `null` without a key),
  `ledgerId` (8 hex), `ledgerGen`, `from`, `files` (each: `fileId`, `kind` (`sensor`, `events`,
  `battery`, `health`, `errors`, `system_log`, `transcript`), `name` (relative to its kind's
  folder), `onSd`, `seal` (`none`, `at_rest`, `on_send`), `start`, `end`, `delivered` (how far some
  named master said it stored the file), `growing`), `withheld` (`unsealedHealth`, `noKey`),
  `lastRequest` (`null` or `peer`, `utc`, `op`, `fileId`, `frames`, `bytes`), `reconcileMs`,
  `refused`, `refusedSilent`, `busy`, `next` (`null`, or the number to pass as `from`).
- Hub (`ENABLE_ESPNOW_COLLECT`): `collectstatus json [from <n>]`: `schema` (1), `collect` (the
  setting), `built`, `whyNot` (`null`, `off`, `not_master`, `no_card`, `clock_not_set`), `card`
  (`mounted`, `writable`, `freeBytes`), `clock` (`set`, `source`), `role`, `rateFps`, `quotaMb`,
  `devices` (each: `mac`, `name`, `state` (`idle`, `listing`, `fetching`, `verifying`, `current`,
  `suspended`, `waiting`, `refused`, `unsupported`), `whyNot` (`null`, `no_session`,
  `session_settling`, `no_file_catchup`, `no_answer`, `busy`, `not_ready`, `refused`, `unsupported`,
  `card_error`, `yielded`, `ledger_reset`), `files`, `filesLeft`, `bytesLeft`, `pct`, `etaS`,
  `lastCaughtUpUtc`, `bytesOnCard`, `lastHeardAgoMs`, `gaps` (bytes per reason: `beforeFirstFetch`,
  `trimmed`, `lostOnDevice`, `noLongerOffered`), `elsewhereBytes` (bytes at the other master)),
  `next`. `collectstatus json <device> [from <n>]`: `device` and `files` (each with the index.json
  fields: `fileId`, `kind`, `name`, `onSd`, `seal`, `deviceStart`, `lastSeenEnd`,
  `lastSeenDelivered`, `held`, `pos`, `segments`, `gaps` (`from`, `to`, `why`), `elsewhere`,
  `digest`, `firstSeenUtc`, `createdBoot`, `evicted`), `next`. Replies are paged under 4 KB.

### 8.1 Remote-device list - `sensors` changed from strings to objects (breaking)
Returned by web `GET /api/sensors/remote` (no params) **and** nested verbatim under
`devices` in the BLE/CLI `espnowsensorstatus json` (master role):
- **Before:** `"sensors": ["thermal","input"]`
- **Now:** `"sensors": [ { "type": "input", "enabled": true, "fresh": true } ]`

Full shape:
```json
{ "devices": [ { "mac": "<HEX>", "name": "dev",
  "sensors": [ { "type": "input", "enabled": true, "fresh": true } ] } ] }
```
Action: read `sensor.type` (was the bare string). You gain `enabled`/`fresh` for a per-sensor
on/off indicator.

### 8.2 Per-sensor remote read - now wrapped (breaking)
Web `GET /api/sensors/remote?device=<MAC>&sensor=<type>` previously returned the raw sensor
JSON (or `{"error":...}`). Now:
```json
{ "connected": true, "enabled": true, "fresh": true, "data": { ...sensor json... } }
```
- `data` is the reading object, or **`null`** when not fresh (disabled / stale) - there is no
  more `{"error":...}` for "no data".
- Drive the status dot from `enabled`; render `data` for the live value.

### 8.3 What `enabled` MEANS for a remote sensor
`enabled` = **"is it streaming its data to the mesh"** - the axis the Sensor Streaming UI
toggles - NOT whether the sensor hardware is powered. (An always-on sensor like the gamepad
is "on" forever; what matters across the link is whether it's broadcasting.) So:
streaming -> `enabled:true, fresh:true`; stopped -> `enabled:false, fresh:false`. A present-but-
not-streaming sensor still appears in the list (render it red/off) and ages out only after the
device goes silent (~60 s).

### 8.4 Automation cadence - already in the data (no firmware change)
A peer's automations arrive as its `automations.json`. Each automation's schedule is in
`triggers[]`; for a `time` trigger the cadence is `triggers[0].recurrence` =
`daily` | `weekly` | `monthly` | `yearly` (the "Repeat" dropdown). **A time trigger with no
`recurrence` means daily.** Render the cadence next to the time (e.g. `14:02 daily`) - showing
bare `14:02` is ambiguous. The field was always present; just surface it.

## 9. Unsolicited notification cards (`#NOTIF`) - the app as a notification sink

Everything above is request/reply. This section is the one thing the device sends
**without being asked**, so it needs different handling on the app side.

The device's notification system fans one event out to several surfaces (OLED banner,
web toast, G2 lens card). The companion app is now one of those surfaces - sink
`NSINK_APP`, peer of `NSINK_G2`. When an event passes the filters, the device writes a
single line to the ordinary `CMD_RESPONSE` reply lane:

```
#NOTIF {"kind":"peer_online","level":"info","title":"Mesh","msg":"Peer online: kitchen","ms":2000}\n
```

| Field | Meaning |
|---|---|
| `kind` | Event-kind **name** - the stable contract (kind numbers are internal). Feed it straight back to `notifyusermute` / `notifyusershow`. |
| `level` | `info` \| `success` \| `warning` \| `error`. Drives icon/colour. |
| `title` | Event **family** name, for grouping. Same title the G2 lens card uses. |
| `msg` | The one-liner body, JSON-escaped. Source text is <=63 chars, but it is truncated to fit the envelope (see "Sizing" below) — treat it as short display text, not a fixed width. |
| `ms` | Suggested display duration for a transient banner. |

**Delivery rules the firmware guarantees:**

- **Directed, not broadcast.** Cards go to one session at a time, and only to sessions
  that are **logged in**. An unauthenticated app receives nothing at all - there is no
  "anonymous" card.
- **Per-user filtering already happened.** The card you receive already passed that
  session user's device policy, admin gating, importance floor (`notifylevel`), and
  personal mutes (`notifyusermute` / `notifyusershow`). Do not re-filter; just render.
- **Best-effort.** If the link is mid-transfer on a large paced reply, the card is
  dropped rather than queued (visible as `app_drop` in `notifstats`). Cards are not
  reliable delivery and must never be the only record of anything.
- **Encryption.** Cards ride the same Secure Channel framing as any other reply when
  one is established; nothing special to do.
- **Sizing.** A whole card line is capped at 195 bytes — exactly the Secure Channel's
  per-frame payload budget, so a card is always one frame on a secure link and always
  inside the MTU any working peer has already negotiated. `msg` is escaped into
  whatever space the other fields leave. Escaping can expand one byte to six (`\u00XX`
  for a control character) and event text can carry remote-supplied names, so a long or
  escape-heavy body **is truncated**. Truncation stops on a clean character boundary:
  you will see a shortened `msg`, never malformed JSON. Do not assume round-tripping.

**Two things the app MUST do (the `#` prefix is why it is there):**

1. **Reject `#NOTIF` lines from the in-flight capture buffer.** Reply capture claims a
   fragment only if it starts with `{`, so a card can never *start* a bogus capture -
   but once a capture buffer is non-empty it appends whatever arrives. Check the
   `#NOTIF ` prefix **before** that append.

   Be precise about why, so nobody later "optimises away" the check: a card cannot
   interleave *within* a single framed message. The secure sender holds its tx mutex
   for a whole message, and cards are sent non-blocking, so a card arriving mid-message
   is dropped rather than spliced in. The real exposure is **between** messages - a
   large reply is emitted as several separate sends, and your capture buffer spans all
   of them. A card landing in that gap is what corrupts the reply.
2. **Handle `#NOTIF` before the auth-state sniffer and before the console.** The
   line-level auth check does substring matches for `Login successful` /
   `Authentication required` / `Logged out`; a card body carrying such text (a device
   or file named that way) would flip the app's auth state. No current formatter emits
   those exact phrases, so this is hardening, not a live bug - but the interception
   point is the same one, so do both at once.

Device-side controls: `notifydeviceapp <0|1>` (admin, master switch for this sink) and
`notifstats`, whose `app-cards` / `app` / `app-drop` counters are the ground truth for
"did the device actually send it".

## 10. System Event kind catalog

The stable command is:

```text
events kinds json
```

It returns the compact v1 vocabulary shape below. This response intentionally
has no numeric ids, aliases, flat duplicate array, `schema`, or revision field:

```json
{"families":[{"n":"Connectivity","k":["wifi_connected","wifi_disconnected"]}]}
```

The complete current response is exactly **3,051 UTF-8 bytes** and contains
**13 families / 159 unique canonical kinds**. The command buffer therefore
needs 3,052 bytes including its trailing NUL and has 1,044 usable payload bytes
of headroom beneath the 4,095-byte command-result ceiling. Family order,
within-family order, labels, and canonical snake_case names match the reviewed
v1 fixture. The exact tokens `boot`, `none`, `set`, `patch`, `all`, and `list`
are reserved because they already mean an alias, sentinel, or notification
command operation; none can be a canonical row or appear in catalog JSON.
Process-local numeric ids are also never serialized. Persist and compare
canonical names, never enum values.

Both command JSON and HTTP use the same serializer over the immutable typed
provider:

- The guest-readable `GET /api/events/kinds` endpoint (subject to the normal web
  authentication policy) streams the response in chunks and is not limited by
  `CMD_RESULT_MAX`. The handler preflights the exact size; after streaming
  starts, a sink failure aborts/closes the response instead of appending a JSON
  error tail or a success terminator.
- UART and authenticated command clients receive the same bytes through the
  ordinary command result lane. The command adapter checks the 4,096-byte
  buffer and fails explicitly if future catalog growth no longer fits.
- A phone companion may fetch the full v1 response only after establishing the
  application Secure Channel and must reassemble its paced reply fragments.
  The request is small; the multi-frame response is what needs reassembly.
- **Do not request the full catalog over plaintext BLE.** That reply path sends
  one unfragmented notification, whose ATT payload is far smaller than 3,051
  bytes. Full-catalog plaintext delivery is unsupported; firmware may attempt
  the oversized notification without propagating its asynchronous send failure
  back into command success. A future plaintext client needs a separately
  versioned paged protocol.

The serializer/text migration and the restoring five-profile Phase 2 artifact
matrix are complete. Host verification passes all 18 registered tests,
including exact typed↔JSON fixture parity, escaping, sizing, sink failure,
checked human-text packing, and ownership boundaries. The matrix passes all
five board/gate rows, rebuilds ordinary FeatherS3 and XIAO-S3 artifacts, and
restores the source config exactly. The physical Secure-BLE client run and
deterministic live HTTP sink-failure test remain pending and are not implied by
those automated results.

`feature_toggled` (System family) stays in the v1 catalog so the frozen order
holds, but nothing posts it since 2026-11-20: `features` only lists. A change to
a feature's switch arrives as `setting_changed` from the command that made it
(`<x>enabled`, `<x>autostart`, `automation system enable|disable`). An app
automation or mute rule on `feature_toggled` never fires.

## 11. Bluetooth link status: role, relay pipe, app console

The phone has two roles. In `blemode client` both ride **one link**: the
device is central to the app's pipe server, and the app is a GATT client of the
device's command service on that same link (no scan, no advertising, no second
link).

| | Relay pipe | App console |
|---|---|---|
| Central | the device | the phone app |
| GATT server | the Android app | the device (the command service the app's BLE commands use) |
| Used for | relay, LLM HTTP fallback, Terminal | CLI commands, `#NOTIF` cards (sec. 9) |
| Runs in | the shared client stack (`openphone`: beside the glasses, or alone) | `openconsole` (saved `blemode server`: advertised, a direct app connection), and in `blemode client` on the relay pipe's link (Secure Channel only) |

Phone link v2: wherever the pipe runs (`glasses-pipe` and `phone-pipe`), the app
console is also stream slot 2 inside the pipe itself, offered when the phone
secret is a device code from `phone pair new` and `bleconsole` is on (sec.
11.9). Design A, the command service on the pipe's link in `blemode client`, is
kept beside it for now.

How the app reads `console`:

- `role` `console`: the console server. Scan for the command service when
  `waiting`.
- `role` `glasses-pipe` or `phone-pipe`, the console stream first: `signed-out` /
  `signed-in` while the stream is open on a `ready` pipe; `waiting-phone` while
  it is offered and not open (the app opens it when the link is ready).
- `role` `glasses-pipe` (or `glasses` in a pipe build) with no stream offered:
  design A's words. The console rides the pipe's link. It reads `waiting-phone`
  until the pipe is `ready` and the app has joined that link (its first write,
  normally the Secure Channel HELLO), then `signed-out` / `signed-in` (a joined
  design-A slot shows so beside an offered stream too). Attach on the pipe's
  "ready" instead of scanning; `no-secret` means `blesecret` must be set first
  (the phone link refuses plaintext); `off` means `bleconsole off`. The session
  rules on that link are in sec. 11.8.
- `role` `phone-pipe` with no stream offered (no device code, or
  `bleconsole off`), or the glasses in a build without the pipe (or with a
  failed registration): `not-here`.
  Show "not available in this mode" and stop scanning until the role changes;
  `phoneLink.console` says why (`needs-code`, `off`).

The words come from one place (`BLE_LinkStatusCore.h`), so the OLED, lens, web
page, CLI and JSON agree.

### 11.1 The `link` object

`blestatus json` (`link`), `GET /api/ble/status` (`link`) and `status json`
(`connectivity.bluetooth`, flattened in, sec. 11.4) carry the same keys from one
reading. Nothing in it is an address, IP or passphrase.

| Key | Type | Values |
|---|---|---|
| `role` | string | What runs now: `console` \| `glasses` \| `glasses-pipe` \| `phone-pipe` \| `off` |
| `roleText` | string | The same as shown: `App console` \| `Glasses` \| `Glasses+pipe` \| `Phone pipe` \| `Off` |
| `saved` | string | The role the saved `blemode` starts, as `role`: `server` -> `console`; `client` -> `glasses-pipe` (`glasses` in a build without the pipe) |
| `savedText` | string | `saved` as shown, as `roleText` |
| `savedDiffers` | bool | `saved` is not what runs (the glasses with or without the pipe count as the same mode). Always `true` while `role` is `off` |
| `pipe` | string | Pipe builds only. `off` \| `no-secret` \| `not-paired` \| `pairing` \| `waiting` \| `searching` \| `linking` \| `ready` \| `closing` \| `idle` \| `fail` \| `fault` |
| `pipeText` | string | Pipe builds only. `pipe` as shown: `off`, `no secret`, `not paired`, `pairing`, `waiting`, `searching`, `linking`, `ready`, `closing`, `idle`, `fail`, `fault` |
| `pipeReason` | string | Pipe builds only, and only when there is one (a `fail`, a `closing` or a `fault`): `not found` \| `timed out` \| `link lost` \| `scan fail` \| `auth fail` \| `no slot` \| `busy` \| `expired` \| `cancelled` \| `cleanup` \| `forgotten` \| `no secret` \| `error`. `error` is a failure the table does not name yet; never read it as success |
| `pipePaired` | bool | Pipe builds only. A pipe enrollment is saved |
| `console` | string | `not-here` (the console is not on this host: the phone pipe role with no console stream offered, the glasses without the pipe, or a failed registration) \| `off` \| `waiting` (server advertising, no app) \| `hidden` (server up, advertising off) \| `waiting-phone` (on the phone link, not open yet: the console stream offered and not open, or design A's slot the app has not joined: the pipe is not ready, or the app has not written yet) \| `no-secret` (design A on the pipe's link, `blesecret` not set, no stream offered) \| `signed-out` (an app attached, nobody signed in) \| `signed-in` (the console stream open on a `ready` pipe, its session signed in or not; or design A's joined slot) |
| `consoleText` | string | The long form: `not available in this mode`, `off`, `waiting for the app`, `not visible (advertising off)`, `waiting for the phone link`, `set blesecret first`, `app connected, signed out`, `app signed in`. The OLED and lens show the short words (`n/a here`, `waiting`, `no secret`, `signed out`, `signed in`, ...) |
| `consoleClients` | int | App centrals connected (the glasses and ring excluded); `0` unless the console server is up, or on the phone link while `console` is `signed-out` or `signed-in`: its console sessions (the stream's, and design A's joined slot), at least `1` |
| `advertising` | bool | `false` unless the console server is up; never on the pipe's link |

### 11.2 Saved mode vs running role

`blemode` with no argument prints both, for example
`Saved mode: client (Glasses+pipe); running: Off`. The saved mode is the boot
role only; what runs is whatever was opened last. Drive the UI from `role`;
offer the saved mode only as what boot will start.

`blemode <mode>` takes `server`, `client` (alias `glasses`), or the dropdown's
`0`/`1` (`g2` also means `client`); it changes the saved boot role and starts
nothing. Anything else is refused with `Error: invalid arguments — Usage:
blemode [client|server]`. The stored values, the saved keyword in replies and
`mode` stay `server` \| `client`. The former `phone-client` mode is gone: the
phone pipe is a device on the shared client stack.

**Verbs (2026-10-21).** Every Bluetooth device is opened and closed with
`openX`/`closeX`, and closing one never touches another: `openg2`/`closeg2`
(the glasses), `openring`/`closering` (the R1 ring), `openphone`/`closephone`
(the phone pipe). `openble`/`closeble` are the whole radio: `openble` brings the
shared client stack up and the peers with auto-reconnect on reconnect by
themselves; `closeble` closes every device and the app console and stops the
host (whether Bluetooth comes up at boot is `bleautostart`). The app console
server is `openconsole`/`closeconsole`; it never coexists with the client stack
(`openphone` while it runs is refused: `closeconsole` first). The former
`phone start`, `phone stop`, `phone connect`, `phone disconnect`,
`ring connect`, `ring disconnect`, `g2init`, `g2deinit` and the `closeg2 full`
argument no longer exist; there are no aliases.

### 11.3 `blestatus json`: `link` and `hint`

- `link` is always present (sec. 11.1), whether or not the console runs.
- `host` (string, 2026-10-21): what the Bluetooth host is doing, beside `link.role`:
  `on` (a role runs), `idle` (the client stack is up with no device open),
  `off` (verified stopped), `fault` (a teardown did not complete; reboot
  required), `parked` (a host generation is still up with no owner). The text
  form prints the same as a `Bluetooth:` line when no role runs.
- `hint` is present only while `initialized` is false (the console server is
  not up) and the console is not open on the phone link. It says how to reach
  the console: with the pipe running what opens it (the console stream needs a
  device code), with only the glasses what `openble` (server mode) would stop:

| `link.role` | `link.console` | `hint` |
|---|---|---|
| `glasses-pipe`, `phone-pipe` | `waiting-phone`, stream offered, pipe not `ready` | `the app console opens on the phone link once it is ready; see 'phone status'` |
| | `waiting-phone`, stream offered, pipe `ready` | `the phone link is ready; the app console opens when the app asks for it` |
| `glasses`, `glasses-pipe` | `waiting-phone` (design A), pipe not `ready` | `the app console attaches to the phone link once the relay pipe is ready; see 'phone status'` |
| | `waiting-phone` (design A), pipe `ready` | `the phone link is ready; the app console attaches when the app opens it` |
| | `no-secret` | `run 'phone pair new' for the app console on the phone link (or set 'blesecret' for its Secure Channel attach)` |
| | `off` | `'bleconsole on' attaches the app console to the phone link` |
| `glasses` | `not-here` | `'openble' switches to server mode for a direct app connection (stops the glasses)` |
| `glasses-pipe`, `phone-pipe` | `not-here`, `bleconsole` on | `the app console on the phone link needs a device pairing code: run 'phone pair new' on serial or the OLED` |
| | `not-here`, `bleconsole` off | `'bleconsole on' offers the app console on the phone link` |
| any but `off` | `signed-out`, `signed-in` | none |
| `off` | | `to start the app console, run 'openble'` |

- Outside the console server the reply is `{"schema":1,"initialized":false,"link":{...},"phoneLink"?:{...},"hint"?:"..."}`
  (`phoneLink` in pipe builds, in every role: sec. 11.9).
- Errors: `{"schema":1,"error":"unavailable"}` (Bluetooth busy; retry), `"buffer"`,
  `"oom"`, `"status too large"`.
- Plain `blestatus` outside the console server answers
  `Role: ...` / `App console: ...` and the phone link block (pipe builds only,
  sec. 11.9; no `Clients:` line on the phone link) and prints the hint, when
  there is one, as a separate `Hint: ...` console line. In server mode the
  block sits between the `Clients:` line and the per-client lines.

### 11.4 `status json`: `connectivity.bluetooth`

The existing keys stay: `running`, `state`, `mode`, `server`, `client`,
`g2Connected`. Changes:

- `state` is the pipe's `pipeText` while only the pipe runs.
- Added in pipe builds: `phoneConnected` (bool: `pipe` is `ready`) and
  `phoneState` (the pipe line: `ready`, `fail: not found`, ...).
- Added: every `link` key of sec. 11.1 (`role`, `roleText`, `saved`, `savedText`,
  `savedDiffers`, `pipe`, `pipeText`, `pipeReason`, `pipePaired`, `console`,
  `consoleText`, `consoleClients`, `advertising`), at the same level. None
  collides with an existing key.
- Added in pipe builds: `phoneLink`, the compact form of sec. 11.9 (no
  `stateText`, `secretText`, `consoleText` or `endReasons`).

### 11.5 `GET /api/ble/status`

Top level: `schema` (1), `mode` (the saved keyword: `server` \| `client`),
`ble` (as `bleinfo json`, without its `phoneLink`), then in G2
builds `g2`, `g2Text` and `ring`, then:

- `link`: sec. 11.1.
- `phone` (pipe builds): the phone object below.
- `phoneLink` (pipe builds): the full form of sec. 11.9, from the same reading as
  `link`.

Errors: `503 {"error":"BLE status unavailable"}` (also `G2 status unavailable`,
`Ring status unavailable`), `500 {"error":"Out of memory"}`.

### 11.6 The phone object (schema 2)

`GET /api/ble/status` `phone`, and `phone status json` (the same object). It is
the relay pipe, from the device's side.

| Key | Type | Meaning |
|---|---|---|
| `schema` | int | `2` |
| `active` | bool | The pipe runs (beside the glasses, or alone) |
| `standalone` | bool | The pipe runs with no glasses runtime beside it (the host belongs to the shared stack either way) |
| `connected` | bool | `pipe` is `ready` |
| `state` | string | The pipe's own text, for diagnostics only (e.g. `stopped`, `scanning`, `phone not found`); not a contract, use `pipe` |
| `text` | string | The pipe line as shown: `ready`, `searching`, `fail: not found`, ... |
| `role` | string | The pipe's place: `off` \| `with-glasses` \| `phone-only`. Not the device role (that is `link.role`) |
| `enrolled` | bool | A pipe enrollment is saved |
| `autoReconnect` | bool | Reconnect the paired phone at boot |
| `pipe`, `pipeText`, `pipeReason`? | string | As in sec. 11.1 |
| `console`, `consoleText` | string | As in sec. 11.1 |
| `pathState`? | string | The phone path as the LLM source sees it: `up` \| `relay-off` \| `connecting` \| `retrying` \| `held` \| `not-paired` \| `off`. Optional (2026-11-19) |
| `pathText`? | string | The same, as one readable line (e.g. `phone not heard (out of range, or phone Bluetooth/app off?), retrying in 12 s`). For display; not a contract |
| `retryInS`? | int | Seconds to the next reconnect attempt; only while `pathState` is `retrying` |
| `attempt`? | int | Reconnect attempts in this outage; only when nonzero |

`phone status json` errors: `{"error":"oom"}`, `{"error":"unavailable"}`,
`{"error":"status too large"}`. Plain `phone status` prints
`pipe=<text> role=<role> paired=yes|no auto=on|off console=<console> state='<state>'
next=<s>s|- try=<k> heard=<n>|- phone=<n>|- path='<path text>'` (`heard` and `phone`:
what the pipe's last scan heard, every report and the phone's own adverts).

### 11.7 Web SSE: `phone-status`

On the web event stream (`GET /api/events`), event `phone-status` is pushed when
the pipe's flags, setup step or text change (checked every 250 ms; no event at
boot, so read `/api/ble/status` first). Compact keys, since an SSE payload is
capped at 128 characters:

| Key | Type | Meaning |
|---|---|---|
| `u` | bool | Pipe link up (`connected`) |
| `on` | bool | Pipe running (`active`) |
| `own` | bool | Phone-only (`standalone`) |
| `en` | bool | Enrolled (`enrolled`) |
| `ar` | bool | `autoReconnect` |
| `p` | string | The pipe id, as `pipe` (`ready`, `not-paired`, ...) |
| `r` | string | Its reason, as `pipeReason`; only when there is one |

The pipe's own text (`state`) is not in the event. The event carries no console
or role keys: re-read `/api/ble/status` for those.

### 11.8 The console session on the pipe's link

In `blemode client` the device keeps one console session on the pipe's link
while the pipe is `ready`, `blesecret` is set and `bleconsole` is on. The
command service and Secure Channel v1 are unchanged; these rules are the
difference from server mode:

- **Joining.** The session waits for the app: it is not a connected app (no
  `signed-out` console word, no BLE connect event, `consoleClients` 0) until the
  device admits the app's first write on `…de01`.
- **Status read.** `…de03` answers the pipe's link with
  `{"state":"connected","uptime":...,"rx":...,"tx":...}` while the session
  exists (before the first write too), and every other link, or the phone
  outside a session, with `{"state":"unavailable"}`.
- **Renewed under a live link.** `bledisconnect` (the OLED "End console", the
  web Disconnect), `bleconsole off` then `on`, and `blesecret` cleared and set
  again end the session without dropping the link, and the device starts a
  fresh one there with no channel. The app's next DATA frame then gets `SC_REJECT` reason
  `0x03` ("no channel"), once per session: run the Secure Channel handshake
  again, then `login`. Frames sent before that reject arrived get nothing;
  resend the command that got no reply. Ignore a reason `0x03` that arrives
  while a new handshake runs.
- **Console off.** With `bleconsole off` the link's writes get no reply at all;
  `…de03` reads `unavailable`.
- **Pipe gone.** When the pipe leaves `ready` the session ends at once: a write
  after that is not run. The next `ready` starts a fresh session.

### 11.9 The phone link block (`phoneLink`, phone link v2)

The phone link as one block: the link (the pipe words), its console stream,
relay forwarding, live streams and how READY sessions ended. Counts and words
only: no address, secret or content. Present in pipe builds only (every role,
also while the pipe is stopped), in `bleinfo json`, `blestatus json` and
`GET /api/ble/status` (full form) and in `status json`
`connectivity.bluetooth` (compact form).

| Key | Type | Values |
|---|---|---|
| `state` | string | The link, as `link.pipe`: `off` \| `no-secret` \| `not-paired` \| `pairing` \| `waiting` \| `searching` \| `linking` \| `ready` \| `closing` \| `idle` \| `fail` \| `fault` |
| `stateText` | string | Full form. As `link.pipeText` |
| `reason` | string | Only when there is one, as `link.pipeReason` (adds `version`: a phone app with the wrong pipe version, and `app old`: three of those in a row; reconnects are held until `openphone`, `phone pair` or `phone pair new`) |
| `paired` | bool | As `link.pipePaired` |
| `secret` | string | The phone secret's kind: `code` (a device code from `phone pair new`; offers the console) \| `passphrase` (set with `phone secret`; relay only) \| `none` |
| `secretText` | string | Full form. `device code` \| `passphrase` \| `no secret` |
| `console` | string | The console stream: `off` (`bleconsole off`) \| `needs-code` (no device code) \| `waiting` (offered; the link is not READY) \| `not-wanted` (READY; the app has the console off, or closed it) \| `opening` \| `open` (signed out) \| `signed-in` \| `closing` \| `backoff` (reopening after an error) \| `stopped` (refused by the app; not reopened this session) |
| `consoleText` | string | Full form. `off`, `needs a device code`, `waiting for the link`, `not wanted by the app`, `opening`, `open, signed out`, `signed in`, `closing`, `backing off`, `refused by the app` |
| `relay` | string | Relay forwarding as the phone reports it: `on` \| `off` \| `unknown` (the link is not READY). Chat and Terminal use the phone only while `on`; the link and the console do not depend on it |
| `streams` | object | Live streams by class: `{"relay": 0..2, "console": 0..1}` |
| `ended` | int | READY sessions ended since boot |
| `lastEnd` | string | Only when `ended` > 0: the last one's end reason (names below) |
| `endReasons` | object | Full form. `{name: count}` of the end reasons since boot; names with no count are left out |

End reason names, by the pipe error code: 0 `other` (also any code the pipe
does not know), 1 `protocol`, 2 `version`, 3 `unauthorized`, 4 `target`, 5
`vpn`, 6 `dns`, 7 `connect`, 8 `connect-timeout`, 9 `busy`, 10 `resources`, 11
`tcp`, 12 `flow`, 13 `deadline`, 14 `cancelled`, 15 `display`, 16 `service-off`,
17 `unsupported`, 18 `paused`.

Plain `bleinfo` (after its own lines) and `blestatus` (after `App console:`)
print the same block in pipe builds, for example:

```
Phone link: ready
  Paired: yes (device code)
  Console: signed in
  Relay: off
  Streams: relay 0/2, console 1/1
  Ended: 3 (last: paused)
  End reasons: cancelled=2 paused=1
```

The `End reasons:` line is left out while nothing has ended.

### 11.10 LLM not-ready reasons (2026-11-19, additive)

`llmgenerate json` and `llmretry` keep `{"ok":false,"error":"model not ready"}` and
may add `"reason":"<text>"` (for example `relay off in the app`, `HTTP agent disabled
(llmhttpenabled)`, `no model selected (llmload http:openclaw)`); a refused start keeps
`"error":"busy or failed to start"` and may add `reason`. `llmload` answering
`{"ok":true,"ready":false}` and `llmask json` may add `reason` too. Keep matching on
`error`; `reason` is for display.

`llmstatus json` (and `/api/llm/status`) for the HTTP source: `"state":"READY"` can now
come with a non-empty `"error"` - the phone path while the link reconnects (the request
waits up to 30 s for it), or `last request: <error>` after a failed request. Treat
`READY` as usable and show `error` as a note. `ERROR` means a configuration problem or a
path that does not come back by itself (relay off in the app, link closed, no phone
paired).

## 12. Status

These contracts describe the current source. The Phase 2 catalog path passes
the complete artifact matrix, while physical transport/fault acceptance remains
pending as stated above. If a shape here does not match what a device emits,
the flashed device revision is authoritative - report the mismatch instead of
silently adapting the client.
