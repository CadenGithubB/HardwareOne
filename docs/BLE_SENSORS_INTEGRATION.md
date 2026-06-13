# HardwareOne — Sensor viewing & control over BLE (firmware contract)

Authoritative firmware-side contract for adding a **Sensors page** (view live
readings + control sensors) to the Android companion app, over the existing
BLE command channel. **No web/HTTP dependency** — same principle as the Device
Status page (`BLE_STATUS_PAGE_INTEGRATION.md`).

This doc is what the app should build against. Where firmware isn't there yet,
it's marked **[firmware: pending]** — those will be implemented to *exactly*
this contract.

---

## 0. TL;DR

- **Control is already live.** The app controls sensors by sending existing CLI
  commands over BLE — no new firmware. **Live power toggle = `open<id>`/`close<id>`**
  (input id `input` → `openinput`/`closeinput`); `features <id>`/`<id>AutoStart` is the
  separate *boot-autostart* setting. Run sensor actions with per-sensor commands.
- **Viewing** is one read command: **`sensors json`** — a single JSON document
  with every compiled sensor's state + live readings (**LIVE**).
- Readings are passed through in each sensor's **native** shape under `data`;
  the app renders them (generic for scalars, light structure-awareness for
  vectors). `kind` tells you what to expect.
- Image/stream sensors (thermal grid, camera, mic) are **never** sent over BLE —
  state only.

---

## 1. Transport (same as the Status page)

You already have a working, authenticated, Secure-Channel command channel.
Reuse it verbatim:

- Send a command string; **reassemble** the chunked reply; `JSON.parse` once.
- Requires the normal authenticated session (`login`) + Secure Channel.
- All reads return one verbatim JSON document via the command result (no log
  noise interleaved). Top-level `"v"` is the schema version.

If anything here is unclear, the mechanics are identical to
`BLE_STATUS_PAGE_INTEGRATION.md` — read that first.

---

## 2. CONTROLLING sensors  — available now (no new firmware)

### 2.1 Live power toggle — `open<id>` / `close<id>`

The card's on/off toggle starts/stops the sensor **live**. Bind it to `sensors
json` `enabled` (which reflects the live running state) and send:

```
openimu      # start IMU now
closeimu     # stop IMU now
```

- `open<id>` starts the sensor task immediately; `close<id>` stops it. `enabled`
  flips on the next `sensors json` poll. Every sensor's `id` matches its verbs —
  e.g. the input device's id is **`input`** → `openinput`/`closeinput`. No
  per-sensor overrides.
- ⚠️ `features <id> on|off` and `<id>AutoStart on|off` set the PERSISTED
  **boot-autostart** flag — they do **not** start the sensor now. Surface those
  in the Settings panel (`controls json`), not the live toggle.

### 2.2 Discover what's present — `features json` / `sensors json`

`features json` gives per feature `id`/`name`/`compiled`/`enabled`/`toggleable`;
`sensors json` gives live `enabled`/`connected` per sensor. Use them to decide
which cards/toggles to show.

### 2.3 Sensor-specific actions — per-sensor commands (exact tokens)

Command tokens are **single concatenated words** (`fmradiotune`, not `fmradio
tune`). Full surface in `BLE_SENSOR_CONTROLS_CONTRACT.md §3`. Examples:

| Sensor | Action commands |
|---|---|
| `fmradio` | `fmradiotune 101.5`, `fmradiovolume 8`, `fmradiomute`/`fmradiounmute`, `fmradioseek up\|down` |
| `imu` | `openimu`/`closeimu`, `imuread` — **no `calibrate`**; zero via `imupitchoffset`/`imurolloffset`/`imuyawoffset` |
| `rtc` | `rtcread`, `rtcsync to\|from` |
| `apds` | `apdsmode color\|proximity\|gesture on\|off`, `apdscolor`/`apdsproximity`/`apdsgesture` |
| `gps` / `presence` / `tof` / `thermal` | `open<id>`/`close<id>`, `<id>read` |

These are free-form CLI — special-case the high-value ones; the rest are reads.

> **After any control command, re-poll `sensors json`** (or wait for a `seq`
> change) to render the new state/readings. Don't assume from the text reply.

---

## 3. VIEWING sensors — `sensors json`

**Status: LIVE (Phase 1 + 2)** — `sensors json` returns the envelope + per-sensor
state (`id`/`name`/`kind`/`enabled`/`connected`) **and** the live `data` readings
for active, non-stream sensors (presence, tof, imu, gps, fmradio, rtc, gamepad).
Stream sensors (thermal) and APDS carry no `data` (see notes).

One command returns every **compiled** sensor with its current state and (for
small, active sensors) its live readings.

```
sensors json
```

- **Poll** while the Sensors page is visible (~2–3 s, `RESUMED` only — same
  lifecycle gating as the status poll). Stop when backgrounded.
- Compact: stream sensors carry no data, so the payload stays a few hundred
  bytes to ~2 KB — well within the command buffer.
- The plain `sensors` (no `json`) command is a **static parts catalog**, not
  this — always use `sensors json` for live data.

### 3.1 Envelope

```json
{
  "v": 1,
  "seq": 1234,
  "sensors": [ <sensor entry>, <sensor entry>, ... ]
}
```

Only sensors **compiled into this build** appear in the array. (Use
`features json` if you also want the not-compiled matrix.)

### 3.2 Sensor entry

```json
{
  "id": "presence",
  "name": "STHS34PF80 presence",
  "kind": "scalar",
  "enabled": true,
  "connected": true,
  "data": { ...native readings, see §5... }
}
```

| Field | Meaning |
|---|---|
| `id` | Stable sensor id. **Same id** used by `features json` and the control commands (§6) — correlate on this. |
| `name` | Human label. |
| `kind` | `"scalar"` \| `"vector"` \| `"stream"` — how to render (see §3.3). |
| `enabled` | Sensor is turned on (config). Authoritative state — use this, not any `enabled` that may also appear inside `data`. |
| `connected` | Chip is physically present on the bus right now. |
| `data` | The live readings, **native per-sensor shape** (§5). **Present only when** `connected && enabled && kind != "stream"`. **Omitted otherwise** — render the card from state alone. |

### 3.3 `kind` — what to render

- **`scalar`** — `data` is a flat object of `key → number/bool/string`. Render
  each as a labeled row. (presence, rtc, fmradio)
- **`vector`** — `data` has nested objects/arrays (e.g. IMU `accel/gyro/ori`,
  GPS lat/lon, ToF `objects[]`). Render with light structure-awareness or
  special-case (compass, map pin). (imu, gps, tof)
- **`stream`** — image/bulk data that is **never sent over BLE**. No `data`.
  Show state + "view on web/OLED". (thermal, camera, mic)

### 3.4 `seq` — change detection

`seq` is the firmware's sensor-status change counter. It bumps when the sensor
**set or a sensor's enabled/connected state changes** (debounced) — **not** on
every reading update. Use it to know when to rebuild the card list/structure;
keep polling for live reading values regardless.

---

## 4. App rendering model

1. One card per `sensors[]` entry: `name` + a `connected`/`enabled` chip + a
   power toggle that sends `open<id>`/`close<id>` (input id `input` → `openinput`/`closeinput`).
2. If `data` present and `kind == "scalar"`: list its keys as `label: value`
   rows (humanize the key; add units from §5).
3. If `kind == "vector"`: render the nested structure (or special-case the
   sensor for a richer widget).
4. If `kind == "stream"` (or no `data`): show state only + a "not available over
   Bluetooth — view on web/OLED" note.
5. Most `data` objects include a **`valid`** bool — if `false`, the reading is
   stale/unavailable; show "—" rather than zeros.

A fully generic renderer (chip + key/value rows, recursing into nested objects)
covers everything in v1 with **no per-sensor app code**. Special-case
individual sensors later for nicer UI — that needs no firmware change.

---

## 5. Native `data` schemas (reference)

`data` is the sensor's own JSON, embedded verbatim. Below are the current
shapes. Render generically; these are for adding units/labels and for
special-casing. Fields can be absent when not applicable; always tolerate
missing keys. Units marked *(raw)* are unitless device values.

### presence — `kind: scalar`
```json
{ "valid": true, "ambient": 24.5, "presence": 312, "presenceDetected": false,
  "motion": 0, "motionDetected": false, "tempShock": 0,
  "tempShockDetected": false, "ts": 1234567 }
```
`ambient` °C · `presence`/`motion`/`tempShock` *(raw)* · `*Detected` booleans · `ts` ms.

### rtc — `kind: scalar`
```json
{ "valid": true, "year": 2026, "month": 6, "day": 11, "hour": 14, "minute": 3,
  "second": 22, "temp": 27.5, "ts": 1234567 }
```
`temp` °C (RTC die temp) · date/time fields are ints.

### fmradio — `kind: scalar`
```json
{ "connected": true, "enabled": true, "frequency": 101.5, "volume": 8,
  "muted": false, "stereo": true, "rssi": 42, "headphones": true,
  "station": "", "radioText": "" }
```
`frequency` MHz · `volume` 0–15 · `rssi` *(raw)* · `station`/`radioText` strings (RDS).

### imu — `kind: vector`
```json
{ "valid": true, "ageMs": 40, "accel": {"x":0.1,"y":0.0,"z":9.8},
  "gyro": {"x":0,"y":0,"z":0}, "ori": {"yaw":120.5,"pitch":-2.0,"roll":1.1},
  "temp": 31.0, "timestamp": 1234567 }
```
`accel` m/s² · `gyro` deg/s · `ori` degrees (yaw/pitch/roll) · `temp` °C.
(May also carry `enabled`/`connected`/`init*` — use the entry-level state.)

### gps — `kind: vector`
```json
{ "valid": true, "fix": true, "quality": 1, "sats": 7, "lat": 37.7749,
  "lon": -122.4194, "alt": 12.0, "speed": 0.0 }
```
`lat`/`lon` degrees · `alt` meters · `sats` count · `speed` device units *(raw — confirm)*.

### tof — `kind: vector`
```json
{ "total_objects": 1, "seq": 88, "timestamp": 1234567,
  "objects": [ { "id": 0, "detected": true, "distance_mm": 412,
                 "distance_cm": 41.2, "status": 0 } ] }
```
Multi-object distance sensor; iterate `objects[]`. On error: `{ "error": "..." }`.

### input (id `input`) — `kind: scalar`
Button/axis state (small). Render generically. (Exact keys per input device —
seesaw gamepad vs ANO encoder; tolerate either.)

### thermal, camera, microphone — `kind: stream`
No `data`. State only. Direct users to the web/OLED for the live image/grid/audio.

### battery — separate command
Battery is **not** in `sensors json`; it has its own already-implemented command
**`batterystatus json`** (see `BLE_STATUS_PAGE_INTEGRATION.md`). If you want it in the
Sensors page, render a card from `batterystatus json` alongside the `sensors json`
cards.

---

## 6. Sensor id reference

`id` is identical across `sensors json`, `features json`, and the control
commands — correlate on it.

| id | name | kind | enable/disable | actions |
|---|---|---|---|---|
| `presence` | STHS34PF80 IR presence | scalar | `features presence on/off` | `presence on/off` |
| `tof` | VL53L4CX distance | vector | `features tof on/off` | `tof start/stop` |
| `imu` | BNO055 orientation | vector | `features imu on/off` | `imu calibrate/start/stop` |
| `gps` | PA1010D GPS | vector | `features gps on/off` | `gps start/stop` |
| `fmradio` | RDA5807 FM radio | scalar | `features fmradio on/off` | `fmradio tune/volume/mute` |
| `rtc` | DS3231 RTC | scalar | `features rtc on/off` | — |
| `apds` | APDS9960 gesture/color | scalar | `features apds on/off` | per-module cmds |
| `input` | Seesaw gamepad (or ANO encoder) | scalar | `openinput`/`closeinput` (live); `sensorautostart input on/off` (boot) — NOT `features` | `input` module cmds |
| `thermal` | MLX90640 thermal | stream | `features thermal on/off` | `thermal start/stop` (data on web/OLED only) |
| `camera` | Camera | stream | — | web/OLED only |
| `microphone` | Mic | stream | — | web/OLED only |

(Only ids compiled into the running build appear in `sensors json` / `features json`.)

---

## 7. Phasing & implementation status

| Capability | Status |
|---|---|
| Live toggle: `open<id>`/`close<id>` (input id `input` → `openinput`/`closeinput`) | **Live now** |
| Control: per-sensor action commands | **Live now** |
| Discovery: `features json` | **Live now** |
| `batterystatus json` | **Live now** |
| `sensors json` — state only (`enabled`/`connected`/`kind`, no `data`) | **Live now** (Phase 1) |
| `sensors json` — + scalar/vector `data` | **[pending]** Phase 2 |
| Per-sensor on-demand `sensor <id> json` | future, only if payload pressure appears |

> Phase 1 lists these compiled sensors: presence, tof, imu, gps, fmradio, rtc,
> apds, gamepad, thermal. `camera`/`microphone` are deferred (stream-only,
> low value over BLE) — the app renders the array dynamically, so they simply
> won't appear until added.

App can build the **control UI and the card scaffolding today** (against
`features json` + the control commands), and light up readings when `sensors
json` lands. Firmware will implement `sensors json` to this exact schema.

---

## 8. Compliance checklist for the app

- [ ] Use `sensors json` (not `sensors`) for live data; reassemble + parse once.
- [ ] Check top-level `v`; tolerate unknown future versions gracefully.
- [ ] Render from **entry-level** `enabled`/`connected` (not any duplicates inside `data`).
- [ ] Treat missing `data` as "no readings" (stream or inactive) — render state only.
- [ ] Honor `kind`: scalar = flat rows, vector = nested, stream = state + "view on web/OLED".
- [ ] Respect each `data.valid` (false → show "—").
- [ ] Power toggle via `open<id>`/`close<id>` (input id `input` → `openinput`/`closeinput`),
      bound to `enabled`; **re-poll** after control commands. (`features`/`*AutoStart`
      = boot autostart, a Settings knob — not the live toggle.)
- [ ] Lifecycle-gate the poll (RESUMED only); use `seq` to skip structural rebuilds.
- [ ] Never expect image/grid/audio over BLE.
