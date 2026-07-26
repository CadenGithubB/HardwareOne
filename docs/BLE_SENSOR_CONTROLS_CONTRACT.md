# HardwareOne — Sensor control-surface contract (firmware)

Authoritative control + settings surface for the Android app's per-sensor
controls, over the BLE command channel. No web dependency. Pairs with
`BLE_SENSORS_INTEGRATION.md` (viewing) and `BLE_STATUS_PAGE_INTEGRATION.md`.

---

## 0. The shape of the answer (read this first)

There are **two control layers**, and most of the work is already done in
firmware:

1. **Settings / config knobs** (poll intervals, thresholds, sensitivity, modes,
   offsets, autostart) — these live in a **self-describing registry** the
   firmware already serializes via `buildSettingsSchemaJson()`. It emits, per
   setting: `key`, `label`, `type`, the **exact set-command** (`cmdKey`), `min`,
   `max`, `options` (enum), `default`, `readOnly`. **Recommendation:** expose
   that builder over the CLI as **`controls json`** (it's web-only today) and
   the app renders every knob generically and **never drifts**. Low lift — the
   builder exists; it just needs a command wrapper. (I can wire this up.)

2. **Actions** (one-shot verbs: start/stop, read, tune, seek, volume, mute,
   mode, set, sync, log, capture) — per-sensor commands, enumerated in §3.

### ⚠️ Command tokens are CONCATENATED, not space-separated
The earlier sensors-contract draft showed `fmradio tune 101.5` / `imu calibrate`.
**That is wrong.** Real commands are single tokens: **`fmradiotune 101.5`**,
**`openimu`**, **`fmradiovolume 8`**. And **IMU has no `calibrate`** — orientation
is corrected via offset settings (§3). Use the exact tokens below.

### Two-source "current value" model (for two-way binding)
- **Live device controls** (tune/volume/mute/seek/mode) → current value is in
  **`sensors json` → that sensor's `data`** (e.g. `data.frequency`,
  `data.volume`, `data.muted`).
- **Config settings** (intervals/thresholds/offsets/autostart) → current value
  via the setting's **read command** (`<cmdKey>` with no argument returns it),
  or the settings-values doc. These are *not* in `sensors json data`.

---

## 1. Setting a config knob — generic, uniform

- **Read current:** from **`controls json <module>`** — each entry's `value`
  field is the live value (§2). (Note: sending a setting command with *no*
  argument returns its usage text, **not** the value — so always read current
  state from `controls json`, never by running the bare command.)
- **Set:** **`<cmd> <value>`** — use the entry's **`cmd`** field, which is the
  exact set-command. Validated against `min`/`max`; out-of-range is rejected.
  An entry with **no `cmd` field is not settable** — render it read-only.
  **Never derive a command from `key`.** See the warning under §4.

So the app needs no hand-maintained per-setting list — it reads `controls json`
for each module and drives every knob from `key` + `value` + `min`/`max`/
`options`: `imuPollingMs`, `tofMaxDistanceMm`, every `*AutoStart`, etc.

---

## 2. `controls json <module>` — the settings descriptor  **[LIVE]**

Per-module control descriptor with **current values baked in** — so the app
binds each control to its real position in one call. `<module>` is the same
`id` as in `sensors json` (`imu`, `tof`, `fmradio`, …). Fetch it when the user
opens that sensor's control panel.

- `controls json` (no module) → discovery: `{ "v":1, "modules":[ "imu","tof",… ] }`
- `controls json imu` → that module's controls + live values:

```json
{
  "v": 1,
  "module": "imu",
  "name": "BNO055 9-axis IMU",
  "entries": [
    { "key": "imuPollingMs", "label": "Polling (ms)", "type": "int",
      "min": 50, "max": 2000, "value": 200, "group": "timing" },
    { "key": "imuOrientationMode", "label": "Orientation mode", "type": "int",
      "min": 0, "max": 8, "options": "0,1,2,3,4,5,6,7,8", "value": 0 },
    { "key": "imuEwmaFactor", "label": "EWMA smoothing", "type": "float",
      "min": 0, "max": 1, "value": 0.2 },
    { "key": "imuAutoStart", "label": "Auto-start", "type": "bool", "value": false }
  ]
}
```

- **`value`** = the live current value — bind the control to it directly (no
  second call, no parsing the human reply).
- **`key`** identifies the setting. It is **NOT** the set command.
- **`cmd`** is the exact set command: **`<cmd> <value>`** (e.g. `imupollingms 500`,
  `imuautostart on`). Matching is case-insensitive. Validated against `min`/`max`;
  out-of-range is rejected. **An entry with no `cmd` is not settable** — render it
  read-only rather than guessing.

  > **Do not fall back to `key`.** An earlier version of this contract said `key`
  > doubled as the set command. It does not, and building on that assumption is
  > what broke the app's write path: an audit of all 407 controls
  > (`docs/CONTROLS_WRITE_INTEGRITY.md`) found **243 DEAD** (key resolves to no
  > command, write silently discarded) and **6 MISFIRE** (key resolves to a
  > *different, real* command). The worst misfire: the debug module's `capture`
  > key resolves to the camera's `capture` verb, so toggling a log flag **took a
  > real photo and wrote it to storage**. A misfire is worse than a dead write
  > because nothing looks wrong. Only ~82 of 407 keys ever worked by coincidence.
- Secrets are never emitted. Unknown module → `{ "error":"unknown module" }`.

**Control mapping (app):**

| schema | UI control | command to set |
|---|---|---|
| `type:int`/`float` + `min`/`max` | **slider** (or **stepper** if small range) | `<key> <value>` |
| `type:bool` | **toggle** | `<key> on` / `<key> off` |
| `options` present | **select** (split `options` on `,`) | `<key> <value>` |
| `type:string` | **text** | `<key> <value>` |
| `readOnly:true` | **display-only** | — |

`group` → optional sub-section. Module `name` (the arg) matches the sensor `id`
from `sensors json`.

> This single descriptor covers **all** poll-interval / threshold / sensitivity
> / mode / offset / autostart knobs for every sensor — so the bulk of the
> "settings/config" question is answered by one generic mechanism.

---

## 3. Actions — per sensor (exact tokens)

`auth`: all require an authenticated session; some writes may require **admin**
(handle an "admin required" reply). `applies`: **live** unless noted.
`re-poll`: after any action, re-poll `sensors json` (or watch `seq`) to confirm;
returns are short human strings, not structured.

Lifecycle is uniform for every sensor:
- **`open<id>`** — start the sensor (live). **`close<id>`** — stop it (live).
- **`<id>read`** — read once (human text; for structured use `sensors json`).
- **`<id>autostart on|off`** — persisted auto-start-on-boot (also a setting in §2).

> **`open<id>`/`close<id>` is the live power toggle** (bind to `sensors json`
> `enabled`). `features <id> on|off` / `<id>autostart` set PERSISTED boot
> autostart only — a Settings knob, NOT the live toggle. (Ids match their
> verbs — the input device's id is `input` → `openinput`/`closeinput`; no
> overrides.)

### fmradio  (`kind: scalar`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `openfmradio` / `closefmradio` | — | `data.enabled` |
| Tune | slider | `fmradiotune <MHz>` | decimal, ~87.0–108.0 MHz | `data.frequency` |
| Seek | action ×2 | `fmradioseek up` / `fmradioseek down` | enum up\|down | `data.frequency` |
| Volume | slider | `fmradiovolume <0-15>` | int 0–15 | `data.volume` |
| Mute | toggle | `fmradiomute` / `fmradiounmute` | — | `data.muted` |
| Auto-start | toggle | `fmradioautostart on\|off` | persisted | (settings) |

Notes: `volume 0` is quiet but `mute` is separate. `station`/`radioText`
(`data.*`) are RDS, read-only.

### imu  (`kind: vector`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `openimu` / `closeimu` | — | `data` presence |
| Pitch offset | slider | `imupitchoffset <deg>` | int −180..180 | (read via cmdKey) |
| Roll offset | slider | `imurolloffset <deg>` | int −180..180 | (read via cmdKey) |
| Yaw offset | slider | `imuyawoffset <deg>` | int −180..180 | (read via cmdKey) |
| Orientation mode | select | `imuorientationmode <0-8>` | int 0..8 | (read via cmdKey) |
| Orient. correction | toggle | `imuorientationcorrection <0\|1>` | 0/1 | (read via cmdKey) |

Plus settings (§2): `imupollingms` 50–2000, `imuewmafactor` 0.0–1.0,
`imutransitionms` 0–1000, `imuwebmaxfps` 1–30, `imudevicepollms` 50–1000,
`imuautostart`. Live orientation is `data.ori {yaw,pitch,roll}`.
**No calibrate command** — use the offset sliders to zero the orientation.

### tof  (`kind: vector`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `opentof` / `closetof` | — | `data` presence |

Settings (§2): `tofpollingms` 50–5000, `tofstabilitythreshold` 0–50,
`toftransitionms` 0–5000, `tofmaxdistancemm` 100–10000, `tofdevicepollms`
100–2000, `tofautostart`. Live distance: `data.objects[].distance_mm`.

### gps  (`kind: vector`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `opengps` / `closegps` | — | `data.fix` |
| Track logging | action | `gpslog [interval_ms]` | optional int ms | — (persists; starts a track log) |
| Auto-start | toggle | `gpsautostart on\|off` | persisted | (settings) |

### apds  (`kind: scalar`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `openapds` / `closeapds` | — | — |
| Color mode | toggle | `apdsmode color on\|off` | enum + on/off | (status flags) |
| Proximity mode | toggle | `apdsmode proximity on\|off` | enum + on/off | (status flags) |
| Gesture mode | toggle | `apdsmode gesture on\|off` | enum + on/off | (status flags) |
| Read color | action | `apdscolor` | — | — |
| Read proximity | action | `apdsproximity` | — | — |
| Read gesture | action | `apdsgesture` | — | — |

Notes: APDS has 3 independently-toggled sub-modes. **APDS is not in the
`sensors json` readings registry** — its live values come from the
`apdscolor`/`apdsproximity`/`apdsgesture` read commands (text), not `data`.

### presence  (`kind: scalar`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `openpresence` / `closepresence` | — | `data` presence |
| Status | action | `presencestatus` | — | — |

Settings (§2): any presence thresholds appear in the schema. Live values:
`data.ambient`, `data.presence`, `data.motion`, `data.*Detected`.

### rtc  (`kind: scalar`)
| label | type | command | arg spec | currentField |
|---|---|---|---|---|
| Power | toggle | `openrtc` / `closertc` | — | `data` presence |
| Read | action | `rtcread [status\|temp]` | enum | `data.*` |
| Set time | text/action | `rtcset <datetime\|timestamp>` | ISO datetime or epoch | `data.year..second` |
| Sync | action ×2 | `rtcsync to` / `rtcsync from` | enum to\|from | — |

`rtcsync to` writes system→RTC; `rtcsync from` writes RTC→system.

---

## 4. Stream sensors (thermal / camera / microphone)

Data is **never** sent over BLE — but control actions are (all via `controls
json thermal` etc. for settings; actions below):

- **thermal:** `openthermal` / `closethermal` (start/stop), `thermalread`
  (min/max/avg text), `thermaldiag`. Rich config in the settings schema
  (`controls json thermal`): `thermalpalettedefault`
  (`grayscale|iron|rainbow|hot|…` → select), `thermalrotation` 0–3,
  `thermaltargetfps` 1–8, `thermalupscalefactor` 1–4, `thermalinterpolation*`,
  `thermalpollingms`, etc. The grid itself is web/OLED only.
- **microphone:** `micrecord` (start/stop → WAV), `miclevel` (audio level),
  `micviz`, `micread`, `miclist`, `micdelete`; settings: `micsamplerate`,
  `micgain`, `micbitdepth`, `micautostart`. No audio stream over BLE.
- **camera:** capture via `capture [littlefs|sd|both]` (saves an image). Image
  data is not sent over BLE. Camera config in its settings module.

(Camera/microphone are not I²C sensors and are **not** in `sensors json` — drive
them purely via these commands + `controls json <module>`.)

---

## 5. Multi-step / latency notes

- `open<id>` may take a moment to initialize hardware; re-poll `sensors json`
  until `connected/enabled` reflect the change rather than assuming from the
  text reply.
- `fmradioseek` scans the band — can take a second or two; `data.frequency`
  updates when done.
- `gpslog` starts persistent logging; `gps` fix itself can take tens of seconds
  outdoors (`data.fix` / `data.sats`).
- There is **no** long-running calibration flow with progress today (IMU uses
  static offsets). If one is added later it'll surface state in `sensors json`.

---

## 6. Auth, persistence, re-poll (summary)

- **Auth:** authenticated session required (login + Secure Channel). Some
  writes may require admin — handle the rejection gracefully.
- **Persistence:** `*autostart` and the §2 settings persist to flash. `open/close`
  and live device controls (tune/volume/mute/mode) are runtime state.
- **Confirm:** treat command success = accepted; **re-poll `sensors json`** (or
  watch `seq`) to render the new state. For live controls, the new value shows
  up in `data.*` on the next poll.

---

## 7. Status & what's left for "complete + functional"

| Piece | Status |
|---|---|
| `controls json <module>` — settings descriptor + **current values** | **DONE (live)** |
| Generic set via `<key> <value>` (case-insensitive) | **DONE (live)** |
| `sensors json` state (id/name/kind/enabled/connected) | **DONE** |
| `sensors json` **`data`** readings (live-control current values) | **DONE (Phase 2)** |
| Action verbs per sensor (§3) | **DONE** (tokens verified, I²C sensors) |
| Thermal/microphone/camera action tokens (§4) | **DONE** |
| `fmradiotune` range | **DONE** — accepts MHz (`103.9`) or ×100 (`10390`); FM band 87–108 MHz |
| APDS live readings (not in the `sensors json` registry) | **open** — via `apdscolor`/`apdsproximity`/`apdsgesture` commands (text), not `data` |

Everything needed for sensor view + control is live. The one residual is APDS
live values not appearing in `sensors json data` (it has no registry builder) —
the app reads those via the `apds*` commands for now.
