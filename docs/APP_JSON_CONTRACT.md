# HardwareOne - App JSON Command Contract

Audience: the Android app AI. This documents the JSON command surface added across
this work session, plus the conventions for consuming it. Everything below is on
the device after the pending flash (it will ride the next version bump on commit;
the peer-metadata items shipped earlier in v0.95.10).

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
- **Failure detection (unchanged).** A command failed if its result string starts
  with `Error` / `ERROR` (the firmware sets `ok=false` on that prefix). On success,
  status/data commands return their object; action commands return `OK`.
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
   `espnowrouterstats`/`espnowsubs`.
4. **Web-off stub hardening** - `banlist`/`httpstatus`/`sessionlist` stubs now emit
   `available:false` JSON instead of text.
5. **Deferred set** - `memreport`, `banlist`, `miclist`, `cameravideolist`, `ring status`.
6. **Second-ring diagnostics** - `espnowidentity`, `espnowmeshmetrics`, `espnowworker`,
   `i2chealth`, `i2cmetrics`, `servolist`, `mqttExternalSensors`, `logtier`, `map`,
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

---

## 3. Automation

| Command | Reply |
|---|---|
| `automation system status json` | `{"schema":1,"enabled":<bool>}` |
| `automation system enable json` / `disable json` | sets, then returns `{"schema":1,"enabled":<bool>}` (the resulting state - toggle + read in one call) |
| `automation run id=<id>` | success `OK`; failure is now a descriptive `Error: ...` string (e.g. `Error: automation id 6073722 not found (it may have been deleted)`), `ok=false` |
| `automationlist json` | (pre-existing) raw `{"version":1,"automations":[...]}` document |

Note: read the global on/off via `automation system status json`, not from
`automationlist`. The list is the automation documents; the flag is a system setting.

---

## 4. System / network / storage

| Command (+`json`) | Reply shape |
|---|---|
| `blestatus` (alias `bleread`) | `{"schema":1,"initialized","state","activeConnections","maxConnections","totalConnections","commandsReceived","responsesSent","connections":[{"index","connId","authed","user","mac","connectedSec","commands"}],"link":{...},"phoneLink"?:{...},"hint"?}`; the console keys from `state` to `connections` are present only while `initialized` (the app console server runs; the console on the phone link shows only in `link` and `phoneLink`). `link` and `hint`: sec. 11.3; `phoneLink` (pipe builds): sec. 11.9 |
| `bleinfo` | `buildBLEInfoJson`'s 13 keys (`schema`, `deviceName`, `txPower`, `autoStart`, `requireAuth`, `secureChannelRequired`, `initialized`, `state`, `connections`, `maxConnections`, `totalConnections`, `commandsReceived`, `clients`), plus `phoneLink` (full form, sec. 11.9) in pipe builds. Errors: `{"error":"oom"}`, `"unavailable"`, `"status too large"` |
| `wifistatus` (alias `wifiread`) | `{"schema":1,"connected","ssid","ip","rssi","mac"}` or `{"schema":1,"connected":false,"savedSsid"?,"mac"}` (`savedSsid` is omitted when the saved list cannot be read) |
| `wifilist` | `{"schema":1,"networks":[{"index","priority","ssid","hidden","primary"}],"count"}` |
| `mqttstatus` | `{"schema":1,"enabled","connected","host","port","user","baseTopic","publishIntervalMs","lastError"?}` |
| `httpstatus` | `{"schema":1,"running","https","port"}` (stub: `{"schema":1,"running":false,"compiled":false}`) |
| `fsusage` | `{"schema":1,"ready","totalBytes","usedBytes","freeBytes","usagePercent"}` |
| `sdinfo` | `{"schema":1,"supported","mounted","type","totalMB","usedMB","freeMB","mount"}` (or `supported:false` / `mounted:false`) |
| `memreport` | `{"schema":1,"dram":{"total","used","free","minFree","peakUsed"},"psram":{"available","total","used","free"},"heapCaps":{"internalFree","internalLargest","dmaFree","dmaLargest"}}` |
| `logtier` | `{"schema":1,"tier","overflow","littlefs":{"free","total","used"},"sd":{"available","total","used","free"}}` |
| `oledstatus` | `{"schema":1,"connected","address","width","height","enabled","mode"}` |
| `temperature` | `{"schema":1,"tempC","tempF"}` |
| `voltage` | `{"schema":1,"measured":false,"estimatedCurrentMa","estimatedPowerW","note"}` - estimate only; use `batterystatus json` for measured power |
| `i2chealth` | `{"schema":1,"deviceCount","devices":[{"address","name","consecutiveErrors","totalErrors","degraded","nack","timeout","busError","adaptiveTimeoutMs"}]}` |
| `i2cmetrics` | `{"schema":1,"uptimeSec","totalTransactions","mutexTimeouts","busContentions","totalBytes"}` |
| `blepeers` | `{"schema":1,"peers":[{"name","displayName","connectable","connected","autoReconnect","mac1","mac2"?,"pairedBy"}],"count"}` |
| `banlist` | `{"schema":1,"bans":[{"ip","reason"}],"count"}` (stub: `available:false`). IP bans are web-only. |

---

## 5. Lists / media / ESP-NOW

| Command (+`json`) | Reply shape |
|---|---|
| `images` (or `images sd`) | `{"schema":1,"location","available","usedBytes","totalBytes","imageCount","images":[{"filename","size"}]}` |
| `maplist` | `{"schema":1,"maps":["/maps/..."],"count"}` |
| `miclist` | `{"schema":1,"count","recordings":[{"filename","size"}]}` |
| `cameravideolist` | `{"schema":1,"count","recordings":[{"filename","size"}]}` |
| `servolist` | `{"schema":1,"servos":[{"channel","name","minPulse","maxPulse","centerPulse"}],"count"}` |
| `map` | `{"schema":1,"valid","filename","region","featureCount","fileSize","bounds":{"minLat","minLon","maxLat","maxLon"}}` |
| `whereami` | `{"schema":1,"valid","road"?,"roadDistanceM"?,"area"?,"areaDistanceM"?}` |
| `mqttExternalSensors` | `{"schema":1,"sensors":[{"name","value","ageSec"}],"count"}` |
| **ESP-NOW** | |
| `espnowdevices` *(v0.95.10)* | all peers: `{"schema":1,"devices":[{"mac","deviceName","friendlyName","room","zone","tags","stationary","online","lastSeenSec","source"}],"count"}` |
| `espnowdeviceinfo` *(v0.95.10)* | this device (self): `{"schema":1,"name","friendlyName","room","zone","tags","stationary","meshRole","mac"}` |
| `espnowrooms` | `{"schema":1,"rooms":[{"room","devices":[{"name","tags","online"}]}],"count"}` |
| `espnowsessions` | `{"schema":1,"sessions":[{"slot","mac","meshId","sessionId","dir","state","ageMs","txSeq","rxHwm"}]}` |
| `espnowrouterstats` | `{"schema":1,"messagesSent","messagesReceived","messagesFailed","v4FragTx","v4FragRx","reassembled","reassemblyGc","reassemblyTimeouts","nextMessageId"}` |
| `espnowsubs` | `{"schema":1,"peers":[{"mac","meshId","subs"}]}` - `subs` is a 32-bit bitmask int |
| `espnowidentity` | `{"schema":1,"valid","mac","pub","createdAtSec","regenCount"}` |
| `espnowmeshmetrics` | `{"schema":1,"mode","activePeers","ttl","adaptiveTtl"}` |
| `espnowworker` (or `espnowworker show`) | `{"schema":1,"enabled","intervalMs","fields":{"heap","rssi","thermal","imu"}}` |
| `ring status` | `{"schema":1,"connected","name","mtu","rx","upMs","scanFound"}` (no address) |
| `espnowmeshstatus` / `espnowlist` / `espnowstats` | already JSON (pre-existing) |

### ESP-NOW peer metadata is pull-only
- **Self** = `espnowdeviceinfo json`; **peers** = `espnowdevices json`.
- The peer cache is refreshed on demand. To get a peer's *current* metadata: send
  `espnowrequestmeta <mac>`, wait ~1-2 s (or poll `espnowdevices json` until the
  fields populate), then read. This mirrors the web's "Sync Metadata" button.
- Do **not** look up the gateway's own MAC in the peer list - it's never there.

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
| `imuread`, `tofread`, `gpsread`, `presenceread`, `presencestatus`, `fmradioread`, `rtcread`, `gamepadread`, `apdsread`, `anoencoderread` | the shared builder's `data` object: envelope `{"valid","connected","ts"}` + value keys |
| `thermalread` | envelope + `{"min","avg","max"}` (summary; the 768-px frame is a separate shape) |
| `micread` | `{"schema":1,"enabled","connected","recording","sampleRate","bitDepth","channels","level"}` |
| `miclevel` | `{"schema":1,"enabled","level"}` |
| `cameraread` | camera status JSON (already JSON; returns unconditionally) |
| `temperature`, `voltage` | see sec. 4 |

**(Corrected 2026-07-28 - the examples below previously showed pre-envelope shapes.
Same day, later: the optional `age` key was removed entirely; derive staleness from `ts`.)**
Every builder object opens with the envelope: `{"valid":bool,"connected":bool,"ts":ms}` -
`ts` is 0 when never sampled. `valid:false` variants keep the envelope. Then per-sensor value keys:
- imu: `+ {"accel":{x,y,z},"gyro":{x,y,z},"ori":{yaw,pitch,roll},"temp"}`
- gps: `+ {"fix","quality","sats","lat","lon","alt","speed"}`
- apds: `+ {"r","g","b","c","proximity"}`
- anoencoder: `+ {"pos","axis","buttons"}` *(was `{"schema":1,"connected","position",...}` - `schema` dropped, `position`->`pos`, envelope added)*
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
  detection), `sensorinfo <name>` (static chip catalog), `sensorlog` (logging status).
- **Actions / setters / help:** `banuser`, `espnowforget`, `blestream`, `bondstream`,
  `oledbootmode`, `oleddefaultmode`, `voicehelp`, `g2` toggles, module summaries.

---

## 8. Remote sensors, streaming state & automation cadence (NEW - breaking shape change)

This session reworked how a MASTER exposes its bonded/mesh **peers'** sensors. If the app
consumes any of the remote-device endpoints below, it must update. (The LOCAL sensor
contract in sec. 6 is **unchanged**.)

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
host (`bleenabled 0` is `closeble` plus the persisted switch). The app console
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

`phone status json` errors: `{"error":"oom"}`, `{"error":"unavailable"}`,
`{"error":"status too large"}`. Plain `phone status` prints
`pipe=<text> role=<role> paired=yes|no auto=on|off console=<console> state='<state>'`.

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

## 12. Status

These contracts describe the current source. The Phase 2 catalog path passes
the complete artifact matrix, while physical transport/fault acceptance remains
pending as stated above. If a shape here does not match what a device emits,
the flashed device revision is authoritative - report the mismatch instead of
silently adapting the client.
