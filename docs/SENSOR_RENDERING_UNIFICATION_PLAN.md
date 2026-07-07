# Sensor Rendering Unification Plan

Status: **LEVEL 1 EXECUTED 2026-07-01** (built green, web-side, UNTESTED on HW); Levels 2–3 plan-only.
NOTE: L1's original premise (reuse the local ToF/thermal renderers for remote) did NOT survive the
code — ToF has per-card smoothing state and thermal's remote data is only the summary, not the frame.
So L1 shipped as: a shared **generic labeled-field card** (`hwRenderGenericSensor`) replacing the raw
`JSON.stringify` remote fallback, **plus** the input dedup (parameterized `hwRenderAnoState` +
`hwBuildAnoInner`, deleted `hwRenderRemoteInput`). Bespoke local views (smoothed bars, heatmap) stay
local-only. §3 below describes the original idealized plan; the shipped version is the honest subset.

The view-layer sequel to the sensor-envelope work
([SENSOR_READING_ENVELOPE_PLAN.md](SENSOR_READING_ENVELOPE_PLAN.md),
[SENSOR_ENVELOPE_CLEANUP_PLAN.md](SENSOR_ENVELOPE_CLEANUP_PLAN.md)). No backwards-compat
needed. Fix BY HAND — no python/sed sweeps.

Goal: **render each sensor once, reuse everywhere it makes sense** — local ↔ remote, and
across web / OLED / G2 / serial — without collapsing the things that genuinely differ
(pixel layouts per display). The envelope unified the *data*; this unifies the *view*.

---

## 1. The layers of sharing (what "where it makes sense" means)

You can't run web-canvas code on a 128×64 OLED — the **drawing** is per-display. But
almost everything *above* the drawing is shareable. Four layers, innermost already done:

| layer | what it is | shared today? |
|---|---|---|
| **Data** | the reading shape (`{valid,connected,ts,age,…}`) | ✅ **done** (envelope) |
| **Spec** | per-field meaning: id, label, unit, precision, scale, source | ❌ duplicated 3–5× per sensor |
| **Renderer** | field→output logic *within one interface* (JS card, text lines) | ⚠️ only the gamepad |
| **Drawing** | pixel/glyph placement for a specific screen | ❌ inherently per-interface (the ceiling) |

The plan fills in **Spec** and **Renderer**. **Drawing** stays per-interface — that's correct.

---

## 2. Current state (the duplication)

**Each sensor's field list is written 3–5 times.** IMU is representative:
- cache struct `ImuCache` (i2csensor_bno055.h:10-19)
- CLI text `cmd_imu` — hardcoded printf: `"Orientation - Yaw: %.1f° Pitch: %.1f° Roll: %.1f°"`… (i2csensor_bno055.cpp:95-101)
- JSON builder `imuBuildDataJSON` — hardcoded snprintf schema (i2csensor_bno055.cpp:605-613)
- G2 compact `imuG2FormatValue` → `"Y91 P-3 R12"` (G2_Page_Sensors.cpp:102-114)
- (OLED local shows only on/off)

The field labels, units (`°`,`m/s²`,`rad/s`,`°C`), and precisions (`%.1f/%.2f/%.3f`) are
scattered across all of these. Change a unit → touch 3–5 places.

**Per interface, local vs remote:**

| interface | local | remote | shared? |
|---|---|---|---|
| **Web** | per-sensor renderers, **hardcoded DOM ids** (i2csensor_*_web.h) | generic `JSON.stringify` (WebPage_Sensors.h:635) + gamepad widget | only **gamepad** (`hwRenderGamepadState(j, ids)`) |
| **OLED** | on/off only (OLED_Mode_Sensors.cpp:190-303) | per-sensor cases + `formatRemoteSensorReadable` fallback (OLED_Mode_Network.cpp:1058-1245) | `formatRemoteSensorReadable` (remote) |
| **G2** | per-sensor `*G2FormatValue` (G2_Page_Sensors.cpp:102-256) | `formatRemoteSensorReadable` (G2_Page_ESPNow.cpp:629) | `formatRemoteSensorReadable` (remote) |
| **Serial/CLI** | per-sensor `cmd_<x>read` hardcoded printf | (n/a — CLI is local) | — |

**The one proven pattern:** the gamepad renderer is parameterized by a target-ids object,
so the *same function* serves local (`hwRenderGamepadState(j, {data:'gamepad-data',…})`,
i2csensor_seesaw_web.h:125) and remote (`hwRenderGamepadState(payload, {data:id,…})`,
WebPage_Sensors.h:632). Every other sensor hardcodes local ids → remote falls back to raw JSON.

---

## 3. Level 1 — Web: one renderer per sensor, shared local ↔ remote (extend the gamepad pattern)

**What:** refactor each per-sensor web renderer to take a target-ids object (defaulting to
the local ids), exactly like `hwRenderGamepadState`. Then `updateRemoteSensor` calls the
real renderer with `remote-<mac>-<sensor>` ids instead of `JSON.stringify`.

Per sensor (from the map):
- **ANO** — `hwRenderAnoState(j, baseId)`; **also deletes** the duplicate `hwRenderRemoteInput` (WebPage_Sensors.h:556-597 re-implements the same dial/buttons).
- **ToF** — `updateToFObjects(ids)`: parameterize `distance-bar-N`/`object-info-N`/`tof-*`. Remote gets the distance bars.
- **Thermal** — `updateThermalVisualization(ids)`: parameterize the canvas/min/max/avg ids. Remote gets the heatmap.
- **IMU** — parameterize `gyro-data`/`device-rotation-canvas`, OR accept `JSON.stringify` for remote (the 3D cube adds little remotely). Low priority.
- **GPS/RTC/Presence** — currently `JSON.stringify` even locally; give them a small shared field-list renderer (feeds naturally from Level 3).

**Effort:** medium, JS-only, in the `_web.h` files + `updateRemoteSensor`. **Payoff:** remote
sensors get real cards (not raw JSON); each sensor's card has ONE definition. This is the
direct answer to "why two implementations" — there don't need to be.

Local vs remote stay separate where it matters: different cards, "my sensors" vs "peer's
sensors," different layout containers. Only the field→pixel logic is shared.

---

## 4. Level 2 — OLED/G2: one text formatter per sensor, shared local ↔ remote

**What:** the text displays (OLED list, G2 list/detail) all want "sensor reading → short
human lines." Today G2 has ~15 per-sensor formatters (`imuG2FormatValue`… G2_Page_Sensors.cpp:102-256),
OLED local shows only on/off, and OLED/G2 remote share `formatRemoteSensorReadable`.

Consolidate into a shared per-sensor readout module:
```c
struct SensorReadoutRow {
  const char* id;                                   // "imu","tof",…
  int (*formatLine)(char* out, size_t cap);         // 1-line summary (cache-direct, LOCAL)
  int (*formatDetail)(char* out, size_t cap);       // multi-line detail (cache-direct, LOCAL)
};
```
- OLED local switches from on/off to `formatLine` (gets real data on the list).
- G2 local reuses `formatLine`/`formatDetail` instead of its private formatters.
- OLED/G2 **remote** keep `formatRemoteSensorReadable` (JSON-generic) — already shared.
- New sensor → add one registry row; every text surface picks it up.

**Effort:** medium, C. Move the G2 formatters into a shared `System_SensorFormatters.*`
consumed by both `G2_Page_Sensors.cpp` and `OLED_Mode_Sensors.cpp`.

---

## 5. Level 3 — the field descriptor: one spec, every interface (kills the 3–5× duplication)

**The deepest unification.** Define each sensor's fields ONCE as data:
```c
struct SensorFieldDescriptor {
  const char* id;        // "oriYaw"  (also the JSON key)
  const char* label;     // "Yaw"
  const char* unit;      // "°" ("" if dimensionless)
  const char* group;     // "orientation" (for CLI/section grouping)
  float       scale;     // 1.0, or 180/π for rad→deg, etc.
  uint8_t     precision;  // decimals
  const float* value;    // &gImuCache.oriYaw   (or a getter)
};
// i2csensor_bno055.cpp: static const SensorFieldDescriptor imuFields[] = { … };
```
Then **shared, sensor-agnostic renderers** consume the descriptor list:
- `sensorBuildJSON(fields, n, valid, connected, ts, buf, cap)` — envelope + loop. *Replaces the hand-written per-sensor `*BuildDataJSON` snprintf.*
- `sensorBuildCliText(fields, n, …)` — grouped `Label: value unit` lines for `cmd_<x>read`.
- `sensorBuildLine(fields, n, …)` / `sensorBuildG2Value(...)` — compact one-liners for OLED/G2.

Now a field's label/unit/precision lives in **one place**, and CLI + JSON + OLED + G2 all
derive from it. Adding a field = one array entry. Models already in the codebase to follow:
`FeatureEntry` (System_FeatureRegistry.h:32-42), `SettingEntry` (System_Settings.h),
`I2CSensorEntry` (System_I2C.h:422-435).

**Effort:** large (touches every sensor + the builders). **Payoff:** single source of truth;
the envelope's `sensorEnvelopeBegin` already proves the "one shared header, per-sensor tail"
shape — this generalizes the tail too. **Caveat:** sensors with nested/array data (ToF
`objects[]`, IMU `accel:{x,y,z}`) need the descriptor to express nesting, or keep a custom
builder for those and use the descriptor for the flat sensors first.

---

## 6. The ceiling — what stays per-interface (and why that's correct)

Genuinely NOT shareable (different rendering primitives):
- Gamepad ASCII art / ANO rotary dial (G2 lens 5×5 grid) — G2-only.
- Thermal heatmap (Adafruit GFX pixels) — OLED/web-only.
- Joystick/button pixel layouts (128×64 vs 576×288 vs web canvas vs 40-col serial).

These are the thin per-interface *drawing* functions. They consume the shared Data + Spec +
(within-interface) Renderer, but the final pixel/glyph placement is theirs. That's the right
boundary — don't force a lowest-common-denominator card that's ugly everywhere.

---

## 7. Staging (value/effort, cheapest first)

1. **Level 1 — web local↔remote renderers** (medium, JS). Directly answers "why two
   implementations"; remote gets real cards. Start with ANO (also deletes duplicate code),
   ToF, thermal. **Do first.**
2. **Level 2 — shared OLED/G2 text formatter** (medium, C). Kills the ~15 G2 formatters +
   gives OLED local real data.
3. **Level 3 — field descriptor** (large, C). Single source of truth; generate JSON/CLI/
   OLED/G2 from one spec. Do flat sensors first; keep custom builders for nested (ToF/IMU).

Each level is independently valuable and independently shippable. HW-test after each; no
incremental commits mid-level ([feedback_no_incremental_commits_during_refactor]).

## 8. Checklist

- [ ] L1: parameterize `hwRenderAnoState`/`updateToFObjects`/`updateThermalVisualization` by ids; wire `updateRemoteSensor` to call them; delete `hwRenderRemoteInput`.
- [ ] L1: give GPS/RTC/Presence a shared small field renderer (or defer to L3).
- [ ] L2: extract G2 per-sensor formatters → `System_SensorFormatters.*`; OLED local uses `formatLine`.
- [ ] L3: define `SensorFieldDescriptor` + shared `sensorBuild{JSON,CliText,Line}`; migrate flat sensors; keep custom builders for ToF `objects[]` / IMU nested.
- [ ] Verify each interface (web local+remote card, OLED list+remote, G2 list+remote, `<x>read` + `<x>read json`) after each level; regenerate the app handoff prompt if JSON keys move.
