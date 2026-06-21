# HardwareOne — App JSON Command Contract

Audience: the Android app AI. This documents the JSON command surface added across
this work session, plus the conventions for consuming it. Everything below is on
the device after the pending flash (it will ride the next version bump on commit;
the peer-metadata items shipped earlier in v0.95.10).

---

## 1. Conventions (read first)

- **Opt-in flag.** Append a standalone `json` token to a supported command to get a
  JSON reply. Without it, you get the unchanged human-readable text. Example:
  `wifistatus` → text, `wifistatus json` → `{"schema":1,...}`.
- **These are flags, not new commands.** No command names were added/renamed; each
  existing handler gained an `if (json) {...}` branch. You append ` json` to commands
  you already call.
- **Shape.** Most replies are `{"schema":1, ...}`. Some sensor replies use `"valid"`
  instead (see §6).
- **Failure detection (unchanged).** A command failed if its result string starts
  with `Error` / `ERROR` (the firmware sets `ok=false` on that prefix). On success,
  status/data commands return their object; action commands return `OK`.
- **No console spam.** JSON replies are returned only to the caller — they are *not*
  broadcast to other consoles. Plain (non-json) calls behave exactly as before.
- **Transport.** BLE responses are already chunked by the BLE secure-channel layer
  (frames ≤195 B payload); reassemble frames as you do today.
- **⚠️ 2 KB result ceiling (important).** Every command that runs through the
  cmd_exec path — **both BLE and the web `/api/cli`** — copies the handler's result
  into a fixed **2048-byte** buffer (`ExecReq.out`; `strncpy` cap). A reply longer
  than ~2047 bytes is **silently truncated → invalid JSON**. This is upstream of the
  BLE framing, so chunking does not save it. Almost every command's JSON is well
  under 2 KB; the one that can exceed it is the **aggregate `sensors json`** when
  several sensors are active (ToF's data alone is ~1 KB). See §6 — fetch sensors
  per-sensor, not via the aggregate, over BLE.
- **Feature compiled out → never a crash.** A command whose feature is not in the
  build resolves to ONE of:
  - **Not registered** → `"Unknown command"` reply (most features), or
  - **JSON stub** → `{"schema":1,"available":false,"reason":"..."}` (the web/HTTP-gated
    ones: `banlist`, `httpstatus`, `sessionlist`).
  Treat any unknown-command / non-JSON / `available:false` reply as "feature
  unavailable." A web server that is enabled-but-not-started still answers fine
  (data is file-backed).

---

## 2. Change log (this session, by area)

1. **Automation** — descriptive errors on `automation run`; `json` on `automation system`.
2. **Network/system status** — `blestatus`, `wifistatus`, `mqttstatus`, `httpstatus`.
3. **Status/list batch** — `fsusage`, `sdinfo`, `wifilist`, `blepeers`, `images`,
   `maplist`, `oledstatus`, and ESP-NOW `espnowrooms`/`espnowsessions`/
   `espnowrouterstats`/`espnowsubs`.
4. **Web-off stub hardening** — `banlist`/`httpstatus`/`sessionlist` stubs now emit
   `available:false` JSON instead of text.
5. **Deferred set** — `memreport`, `banlist`, `miclist`, `cameravideolist`, `ringstatus`.
6. **Second-ring diagnostics** — `espnowidentity`, `espnowmeshmetrics`, `espnowworker`,
   `i2chealth`, `i2cmetrics`, `servolist`, `mqttExternalSensors`, `logtier`, `map`,
   `whereami`.
7. **Sensors** — every live sensor read now has JSON, and `sensors json` is the
   preferred aggregate (see §6).

---

## 3. Automation

| Command | Reply |
|---|---|
| `automation system status json` | `{"schema":1,"enabled":<bool>}` |
| `automation system enable json` / `disable json` | sets, then returns `{"schema":1,"enabled":<bool>}` (the resulting state — toggle + read in one call) |
| `automation run id=<id>` | success `OK`; failure is now a descriptive `Error: ...` string (e.g. `Error: automation id 6073722 not found (it may have been deleted)`), `ok=false` |
| `automationlist json` | (pre-existing) raw `{"version":1,"automations":[...]}` document |

Note: read the global on/off via `automation system status json`, not from
`automationlist`. The list is the automation documents; the flag is a system setting.

---

## 4. System / network / storage

| Command (+`json`) | Reply shape |
|---|---|
| `blestatus` (alias `bleread`) | `{"schema":1,"initialized","state","activeConnections","maxConnections","totalConnections","commandsReceived","responsesSent","connections":[{"index","user","mac","connectedSec","commands"}]}` |
| `wifistatus` (alias `wifiread`) | `{"schema":1,"connected","ssid","ip","rssi","mac"}` or `{"schema":1,"connected":false,"savedSsid","mac"}` |
| `wifilist` | `{"schema":1,"networks":[{"index","priority","ssid","hidden","primary"}],"count"}` |
| `mqttstatus` | `{"schema":1,"enabled","connected","host","port","user","baseTopic","publishIntervalMs","lastError"?}` |
| `httpstatus` | `{"schema":1,"running","https","port"}` (stub: `{"schema":1,"running":false,"compiled":false}`) |
| `fsusage` | `{"schema":1,"ready","totalBytes","usedBytes","freeBytes","usagePercent"}` |
| `sdinfo` | `{"schema":1,"supported","mounted","type","totalMB","usedMB","freeMB","mount"}` (or `supported:false` / `mounted:false`) |
| `memreport` | `{"schema":1,"dram":{"total","used","free","minFree","peakUsed"},"psram":{"available","total","used","free"},"heapCaps":{"internalFree","internalLargest","dmaFree","dmaLargest"}}` |
| `logtier` | `{"schema":1,"tier","overflow","littlefs":{"free","total","used"},"sd":{"available","total","used","free"}}` |
| `oledstatus` | `{"schema":1,"connected","address","width","height","enabled","mode"}` |
| `temperature` | `{"schema":1,"tempC","tempF"}` |
| `voltage` | `{"schema":1,"measured":false,"estimatedCurrentMa","estimatedPowerW","note"}` — estimate only; use `batterystatus json` for measured power |
| `i2chealth` | `{"schema":1,"deviceCount","devices":[{"address","name","consecutiveErrors","totalErrors","degraded","nack","timeout","busError","adaptiveTimeoutMs"}]}` |
| `i2cmetrics` | `{"schema":1,"uptimeSec","totalTransactions","mutexTimeouts","busContentions","totalBytes"}` |
| `blepeers` | `{"schema":1,"peers":[{"name","displayName","connectable","connected","autoConnect","mac1","mac2"?,"pairedBy"}],"count"}` |
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
| `espnowsubs` | `{"schema":1,"peers":[{"mac","meshId","subs"}]}` — `subs` is a 32-bit bitmask int |
| `espnowidentity` | `{"schema":1,"valid","mac","pub","createdAtSec","regenCount"}` |
| `espnowmeshmetrics` | `{"schema":1,"mode","activePeers","ttl","adaptiveTtl"}` |
| `espnowworker` (or `espnowworker show`) | `{"schema":1,"enabled","intervalMs","fields":{"heap","rssi","thermal","imu"}}` |
| `ringstatus` | `{"schema":1,"connected","name","addr","mtu","rx","upMs","scanFound"}` |
| `espnowmeshstatus` / `espnowlist` / `espnowstats` | already JSON (pre-existing) |

### ESP-NOW peer metadata is pull-only
- **Self** = `espnowdeviceinfo json`; **peers** = `espnowdevices json`.
- The peer cache is refreshed on demand. To get a peer's *current* metadata: send
  `espnowrequestmeta <mac>`, wait ~1–2 s (or poll `espnowdevices json` until the
  fields populate), then read. This mirrors the web's "Sync Metadata" button.
- Do **not** look up the gateway's own MAC in the peer list — it's never there.

---

## 6. Sensors — fetch per-sensor over BLE (do NOT use the aggregate)

**Over BLE, fetch each sensor individually and load them one at a time.** The
aggregate `sensors json` embeds every active sensor's `data` in one reply, which can
exceed the 2 KB result ceiling (§1) and truncate to invalid JSON. Each per-sensor
`<x>read json` returns a single sensor (≤ ~1 KB), always safely under the ceiling,
and self-describes `valid` / `enabled` / `connected`.

**Discovery / loading pattern:**
1. Call **`sensors json brief`** once → a small, bounded enumeration (state only, **no
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
or for low-sensor-count devices — but treat it as unreliable over BLE with multiple
sensors active.

```json
{"schema":1,"seq":<n>,"sensors":[
  {"id":"imu","name":"BNO055 orientation","kind":"vector",
   "enabled":true,"connected":true,"data":{ ...live reading... }}, ...
]}
```

`thermal` is a stream sensor, so even in the aggregate it has no embedded `data` —
always read it via `thermalread json`.

### Per-command sensor reads (1:1 parity)
Each also has its own `json`. **Important shape note:** the per-sensor reads that
delegate to the shared builder return the *same object* that appears in the `data`
field of `sensors json` — that object uses a `valid` flag (and often
`enabled`/`connected`), and may **not** carry a top-level `schema`. Parse defensively
(check `valid`). The directly-written ones use `{"schema":1,...}`.

| Command (+`json`) | Returns |
|---|---|
| `imuread`, `tofread`, `gpsread`, `presenceread`, `presencestatus`, `fmradioread`, `rtcread`, `gamepadread`, `apdsread` | the shared builder's `data` object (has `valid`/`enabled`/`connected` + readings) |
| `thermalread` | `{"valid","min","max","avg","seq"}` (or `{"valid":false}`) |
| `micread` | `{"schema":1,"enabled","connected","recording","sampleRate","bitDepth","channels","level"}` |
| `miclevel` | `{"schema":1,"enabled","level"}` |
| `anoencoderread` | `{"schema":1,"connected","position","axis","buttons"}` |
| `cameraread` | camera status JSON (already JSON; returns unconditionally) |
| `temperature`, `voltage` | see §4 |

Example builder `data` shapes (for reference; fields vary per sensor):
- imu: `{"valid","seq","enabled","connected","ageMs","yaw","pitch","roll","accel...","gyro...","temp"}`
- gps: `{"val","fix","quality","sats","lat","lon","alt","speed"}`
- apds: `{"valid","colorEnabled","proximityEnabled","gestureEnabled","r","g","b","c","proximity"}`

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

## 8. Remote sensors, streaming state & automation cadence (NEW — breaking shape change)

This session reworked how a MASTER exposes its bonded/mesh **peers'** sensors. If the app
consumes any of the remote-device endpoints below, it must update. (The LOCAL sensor
contract in §6 is **unchanged**.)

### 8.1 Remote-device list — `sensors` changed from strings to objects (breaking)
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

### 8.2 Per-sensor remote read — now wrapped (breaking)
Web `GET /api/sensors/remote?device=<MAC>&sensor=<type>` previously returned the raw sensor
JSON (or `{"error":...}`). Now:
```json
{ "connected": true, "enabled": true, "fresh": true, "data": { ...sensor json... } }
```
- `data` is the reading object, or **`null`** when not fresh (disabled / stale) — there is no
  more `{"error":...}` for "no data".
- Drive the status dot from `enabled`; render `data` for the live value.

### 8.3 What `enabled` MEANS for a remote sensor
`enabled` = **"is it streaming its data to the mesh"** — the axis the Sensor Streaming UI
toggles — NOT whether the sensor hardware is powered. (An always-on sensor like the gamepad
is "on" forever; what matters across the link is whether it's broadcasting.) So:
streaming → `enabled:true, fresh:true`; stopped → `enabled:false, fresh:false`. A present-but-
not-streaming sensor still appears in the list (render it red/off) and ages out only after the
device goes silent (~60 s).

### 8.4 Automation cadence — already in the data (no firmware change)
A peer's automations arrive as its `automations.json`. Each automation's schedule is in
`triggers[]`; for a `time` trigger the cadence is `triggers[0].recurrence` =
`daily` | `weekly` | `monthly` | `yearly` (the "Repeat" dropdown). **A time trigger with no
`recurrence` means daily.** Render the cadence next to the time (e.g. `14:02 daily`) — showing
bare `14:02` is ambiguous. The field was always present; just surface it.

## 9. Status

All of the above builds clean and is pending on-device validation; on confirmation it
will be committed with a version bump. If a shape here doesn't match what the device
emits, the device is authoritative — report the mismatch.
