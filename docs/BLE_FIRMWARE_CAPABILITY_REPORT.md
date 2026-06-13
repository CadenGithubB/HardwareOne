# HardwareOne firmware — BLE sensor view & control capability report

**Audience:** the Android companion-app AI. **Purpose:** the exact, *current*
firmware surface for viewing and controlling sensors over the BLE command
channel (no web dependency), what changed since your last sync, what's still
open, and a checklist to bring the app into lockstep.

All of this rides the existing authenticated, Secure-Channel command channel —
the same one `login` and the status page already use. Read commands return one
verbatim JSON document via the command result; reassemble chunks and `JSON.parse`
once. `"v"` is each document's schema version.

---

## 1. The three contracts (source of truth — read these)

| Doc | Covers |
|---|---|
| `BLE_STATUS_PAGE_INTEGRATION.md` | Device status & I²C device list: `status json`, `devices json`, `features json`, `bleinfo json`, `uptime json`, `time json`, `batterystatus json` |
| `BLE_SENSORS_INTEGRATION.md` | Sensor **viewing**: `sensors json` (state + live readings) + control via existing commands |
| `BLE_SENSOR_CONTROLS_CONTRACT.md` | Sensor **control surface**: `controls json <module>` + per-sensor action verbs |

This report is the index/changelog over those three.

---

## 2. Live firmware surface (all implemented & building green)

### Read / view — JSON commands (append `json`)
| Command | Returns |
|---|---|
| `status json` | device summary (fw, board, net, mem, storage, connectivity{espnow,bt,mqtt,i2c,…}) |
| `devices json` | I²C device list `{name, addr, bus}` |
| `features json` | every feature `{id,name,category,compiled,enabled,toggleable}` |
| `bleinfo json` | BLE config + `secureChannelRequired` |
| `uptime json` / `time json` / `batterystatus json` | as named |
| **`sensors json`** | **per-sensor `{id,name,kind,enabled,connected,data?}` — state + LIVE readings** |
| **`controls json [module]`** | **per-module control descriptor: `{key,label,type,min,max,options,value,readOnly}` (with current values); no arg → module list** |

### Control — existing commands (send the string; short text reply)
| Pattern | Effect |
|---|---|
| **`open<id>` / `close<id>`** | **the live power toggle** — start/stop the sensor NOW; flips the `enabled` field in `sensors json`. **This is what a power switch should send.** |
| `<id>AutoStart on\|off` (a.k.a. `features <id> on\|off`) | sets PERSISTED **boot autostart** only — does NOT start the sensor now. A Settings knob, surfaced in `controls json`. |
| `<id>read` | read once (human text; prefer `sensors json` for structured) |
| **`<key> <value>`** | **set any setting from `controls json` (case-insensitive; validated to min/max)** |
| per-sensor actions | `fmradiotune <MHz>`, `fmradiovolume <0-15>`, `fmradiomute`/`unmute`, `fmradioseek up\|down`, `apdsmode <color\|proximity\|gesture> on\|off`, `rtcsync to\|from`, `gpslog [ms]`, `micrecord`, `capture [littlefs\|sd\|both]`, … (full list in the controls contract §3–§4) |

> **No token overrides.** Every sensor's `id` matches its command verbs, so the
> toggle is always `open<id>`/`close<id>`. The input device's id is **`input`**
> (name "Seesaw gamepad") → `openinput`/`closeinput`, `controls json input`,
> `sensorautostart input`.

---

## 3. What changed since your last sync (changelog)

0. **Input sensor id is now `input`, not `gamepad`** (consistency fix). The
   Seesaw gamepad's `sensors json` `id` is **`input`** (display `name` stays
   "Seesaw gamepad"), matching its command verbs (`openinput`/`closeinput`),
   `controls json input`, `sensorautostart input`, and the I²C module name. **No
   more `gamepad→openinput` override** — `open<id>` just works. (`sensorautostart
   gamepad` still accepted as a legacy alias.) → in the app, correlate the input
   card on `id == "input"` and drop the gamepad token-override.
1. **`sensors json` now includes live `data`** (Phase 2). For active, non-stream
   sensors the entry carries the sensor's **native** readings object
   (presence/tof/imu/gps/fmradio/rtc/input). Stream sensors (thermal) and APDS
   carry no `data`. → bind live device controls (frequency/volume/…) to
   `data.*`.
2. **`controls json <module>` added** — per-module settings descriptor **with
   current values baked in**. This is the control-panel surface: render
   sliders/steppers/selects/toggles generically from `type`+`min`/`max`+
   `options`, position them at `value`, and set with `<key> <value>`.
3. **Live-toggle semantics fixed** — `sensors json` `enabled` now reports the
   **live running state** (`g<X>Enabled`), not the boot-autostart flag. The power
   toggle should send **`open<id>`/`close<id>`** (live), not `features <id>`
   (which is autostart only). See the corrections table (§4).
4. **Gamepad `connected` bug fixed** — it was reporting `connected:false` for an
   attached Seesaw (unified under module "input"); now correct via the input
   driver flags.
4. **I²C counts made honest** — `i2c.activeDevices` can no longer exceed the
   detected device list (it's derived from the same physical scan).
5. **ESP-NOW web "Initialize"** now acks-then-inits async (separate fix; affects
   the web ESP-NOW page, not sensors).

---

## 4. Corrections to earlier drafts (things the app may have assumed wrong)

These were wrong in the first sensor-control draft — use the right column:

| Wrong assumption | Correct |
|---|---|
| **`features <id> on\|off` is the live enable/disable toggle** *(this report previously said so — wrong)* | **`features <id> on\|off` / `<id>AutoStart` set PERSISTED boot-autostart only; they do NOT start the sensor now.** The live power toggle is **`open<id>` / `close<id>`** (starts/stops the task; flips `sensors json` `enabled`). No token overrides — ids match their verbs (the input sensor's id is `input` → `openinput`). |
| `fmradio tune 101.5` (space-separated) | **`fmradiotune 101.5`** — command tokens are **single concatenated words** (`openimu`, `fmradiovolume 8`) |
| `imu calibrate` exists | **No calibrate.** Orientation is zeroed via offset settings `imupitchoffset`/`imurolloffset`/`imuyawoffset` |
| `sensors json` `enabled` = autostart | **`enabled` now = live running state** (firmware fixed) — bind the power toggle to it. |
| read a setting's current value by running the bare command | bare command returns **usage text**, not the value → read current from **`controls json`'s `value`** |
| controls descriptor is metadata-only | `controls json` **includes the live `value`** per setting |

---

## 5. Quick schemas (full detail in the contracts)

**`sensors json`**
```json
{ "v":1, "seq":1234, "sensors":[
  { "id":"fmradio", "name":"RDA5807 FM radio", "kind":"scalar",
    "enabled":true, "connected":true,
    "data":{ "frequency":101.5, "volume":8, "muted":false, "stereo":true, … } },
  { "id":"thermal", "name":"MLX90640 thermal", "kind":"stream",
    "enabled":false, "connected":true }      // stream → no data
] }
```
`kind ∈ {scalar, vector, stream}`. `data` present only when `connected && enabled
&& kind!="stream"`. Native per-sensor shapes documented in
`BLE_SENSORS_INTEGRATION.md §5`.

**`controls json imu`**
```json
{ "v":1, "module":"imu", "name":"BNO055 9-axis IMU", "entries":[
  { "key":"imuPollingMs", "label":"Polling (ms)", "type":"int",
    "min":50, "max":2000, "value":200, "group":"timing" },
  { "key":"imuAutoStart", "label":"Auto-start", "type":"bool", "value":false }
] }
```
Set with `imuPollingMs 500` / `imuAutoStart on`. `module` == the sensor `id`.

---

## 6. Still open (do NOT build against as if complete)

- **APDS live values** are not in `sensors json data` (no registry builder).
  Read them via `apdscolor` / `apdsproximity` / `apdsgesture` commands (text).
- **camera / microphone** are not in `sensors json` (stream subsystems). Control
  via their commands (`capture …`, `micrecord`, etc.) + `controls json <module>`.
- **Per-sensor on-demand `sensor <id> json`** is not implemented — `sensors json`
  returns all compiled sensors in one call (small enough; stream sensors carry no
  data).

---

## 7. Suggested app plan to match firmware

1. **Sensors page:** poll `sensors json` (lifecycle-gated). Render one card per
   entry: name + `enabled`/`connected` chips. If `data` present, render readings
   (scalar → flat rows; vector → nested/special-case). `kind:"stream"` → state +
   "view on web/OLED".
2. **Per-sensor controls:** on opening a sensor, fetch `controls json <id>` once;
   render each entry by `type` (slider/stepper/select/toggle), position at
   `value`, set with `<key> <value>`. Re-fetch (or re-poll `sensors json`) after
   a change.
3. **Action buttons:** drive from the contract §3/§4 verbs (power, tune, seek,
   volume, mute, mode, set, sync, record, capture). Use exact concatenated
   tokens.
4. **Power toggle:** send **`open<id>`/`close<id>`** (live start/stop), bound to
   `sensors json` `enabled`. input id `input` → `openinput`/`closeinput`.
   Boot-autostart (`<id>AutoStart` / `features <id> on`) is a *separate* Settings
   knob in `controls json` — not the power toggle.
5. **Two-way binding sources:** live device values ← `sensors json data.*`;
   config values ← `controls json … value`.
6. **Correlate everything on `id`** — identical across `sensors json`,
   `features json`, `controls json` module name, and the command tokens.

---

## 8. Build/commit status

All firmware in this report **builds green** and is staged but **not yet
committed** (pending a hardware validation pass). Once validated on device it'll
be committed; the schemas above are stable and safe to build the app against now.
