# Sensor Envelope — Minimal-Essential Cleanup & Mesh Transport Plan

Status: **TIER 2 EXECUTED 2026-07-01** (built green, all sensors, no new warnings; UNTESTED on HW).
Tier 1 (disable thermal broadcast) done earlier; Tier 3 (chunking) not started.
Follow-up to [SENSOR_READING_ENVELOPE_PLAN.md](SENSOR_READING_ENVELOPE_PLAN.md).
The envelope rollout standardized the **header** (`valid`/`connected`/`ts`/`age`) across
all 9 sensor readings; this doc captures the **body** cleanup + the mesh-transport
question surfaced while investigating ESP-NOW. No backwards-compat needed (single-operator,
erase-before-flash). Fix BY HAND — no python/sed sweeps.

---

## 1. The principle

> **A reading envelope should carry only the essential *measurement* — no
> diagnostics, no device-state, no bookkeeping, no redundant/derived fields.**

The envelope pass was deliberately header-only and non-breaking, so each sensor's
pre-envelope body fields were *preserved*, not audited. The cost: several readings
still carry cruft. This principle, applied as ONE audit pass, keeps every sensor
lean, is a rule (not a per-sensor hack), and — as a side effect — gets the bloated
sensors under the ESP-NOW 200-byte limit (see §4).

Three categories of "extra":
- **Bookkeeping** — counters that exist only to version a reading (`seq`, `total_objects`). → **drop**
- **Device-state** — flags describing what the device is *doing*, not what it *measured*
  (IMU `enabled`/init flags, APDS mode flags, FM `enabled`). The coarse `enabled` is a
  **literal duplicate** of the discovery layer's per-sensor `enabled` (§3b) → **drop**; finer
  state (APDS sub-modes, IMU init) → **drop if unconsumed** (verified for APDS), else relocate
  to a `<id>status` reading.
- **Redundant/derived** — a field computable from another (`distance_cm` = `distance_mm`/10). → **drop**

Genuine measurement-context (GPS `fix`/`quality`/`sats`, presence detection booleans,
a radio's tuned state) is NOT cruft — keep it.

---

## 2. Per-sensor audit (current body, past the envelope)

| sensor | body fields | keep (measurement) | DROP / relocate |
|---|---|---|---|
| **tof** | objects[{id,detected,distance_mm,distance_cm,status,valid}], total_objects, seq | objects[…] (id/detected/distance_mm/status/valid) | **`seq`** (bookkeeping), **`total_objects`** (=len), **`distance_cm`** (=mm/10), **empty padding objects** (emit only detected) |
| **imu** | seq, enabled, initRequested, initDone, initResult, accel, gyro, ori, temp | accel, gyro, ori, temp | **`seq`** (bookkeeping); **`enabled`** (duplicate of discovery `enabled` → drop); **`initRequested`/`initDone`/`initResult`** (init-state → drop if unconsumed, else relocate) |
| **thermal** (summary) | min, avg, max, seq | min, avg, max | **`seq`** (bookkeeping) |
| **apds** | colorEnabled, proximityEnabled, gestureEnabled, r,g,b,c,proximity | r,g,b,c,proximity | **`colorEnabled`/`proximityEnabled`/`gestureEnabled`** (mode-state, UNCONSUMED on-device → drop) |
| **fmradio** | enabled, frequency, volume, muted, stereo, rssi, headphones, station, radioText | frequency, volume, muted, stereo, rssi, headphones, station, radioText (a radio's tuned state IS its reading) | **`enabled`** (literal duplicate of discovery `enabled` → drop) |
| **gps** | fix, quality, sats, lat, lon, alt, speed | all (fix/quality/sats qualify the reading) | — clean |
| **presence** | ambient, presence, presenceDetected, motion, motionDetected, tempShock, tempShockDetected | all | — clean |
| **rtc** | year…second, temp | all | — clean |
| **gamepad / ano** | x,y,buttons / pos,axis,buttons | all | — clean |

---

## 3. `seq` + `total_objects` are provably unconsumed (drop first — lowest risk)

Verified 2026-07-01 by grep across every consumer:
- **Bluetooth.cpp** re-declares the sensor cache structs but NEVER reads `tofSeq`/`thermalSeq`/`imuSeq`.
- **Web sensor-page JS** never parses a sensor's `seq`.
- **OLED / G2** explicitly SKIP `seq` in `formatRemoteSensorReadable` (skip-list).
- **ESP-NOW** dedup uses the mesh-layer `msgId`, not the sensor `seq`.
- **`total_objects`** is emitted by the ToF builder and read by nobody (= `objects.length`).

The `seq` *values* are used internally (OLED render-skip via `oledLastRenderedSensorSeq`,
power-save wake via `gInputCache.seq`) but those read the **cache field directly**, not the
JSON — so nothing needs `seq` *in the envelope*.

**Why it's obsolete:** `seq`'s whole job was change-detection ("is this a new reading?").
The envelope's **`ts` now answers that** (changed `ts` = new reading). `seq` is a fossil
from before readings had a timestamp — redundant with `ts`. (Its only theoretical edge —
counting *dropped* readings — is realized nowhere: nothing does missed-reading detection,
and over ESP-NOW `seq` isn't used and the reading is size-dropped anyway.)

Only unknown: the off-device **FlutterApp** *might* read `seq` — but `ts` covers the same
need, and per no-backwards-compat the app updates alongside the flash. → **Drop `seq`
(tof, imu, thermal) + `total_objects` (tof).** Provably unconsumed on-device; the clearest,
lowest-risk item in this plan.

---

## 3b. Device-state: `enabled` duplicates the discovery layer; APDS flags unconsumed

Verified 2026-07-01:
- The `sensors json` **discovery layer already emits `enabled` + `connected` per sensor** at
  the entry level (`addSensorEntry` → `o["enabled"]`, `o["connected"]`, System_I2C.cpp:~1651).
  So **FM radio's and IMU's body `enabled` is the SAME value** (`gFmRadioEnabled`/`gImuEnabled`)
  **printed twice** — a literal duplicate one layer down. → **drop** (the entry already carries it).
- **APDS `colorEnabled`/`proximityEnabled`/`gestureEnabled`**: grep for JSON consumers (web JS /
  OLED / G2) came back **EMPTY** — nothing reads them. Not redundant with the single discovery
  `enabled` (they're finer-grained: which sub-mode is on), but they're state, not measurement,
  and unconsumed. → **drop** (or relocate to `apdsstatus` only if a future UI needs per-mode state).

So device-state moves from "relocate if consumed" to **mostly drop — verified**: the coarse
`enabled` is a duplicate; the finer APDS flags have no reader. The one sub-case still worth a
consumer check before removal is IMU's init flags (`initRequested`/`initDone`/`initResult`) —
they reflect the BNO055 init handshake and a debug UI might surface them.

Layer model this enforces:
- **Discovery** (`sensors json` entry): identity + on/off/present (`enabled`/`connected`). *Already there.*
- **Envelope** (`data`): reading validity + timing (`valid`/`ts`/`age`).
- **Body**: the measurement, nothing else.

## 4. The mesh-transport reality (why the body size matters)

ESP-NOW sensor broadcast is **single-packet, fire-and-forget, no chunking** (see the
transport research). A hard **200-byte JSON gate** (`v4_broadcast_sensor_data`,
System_ESPNow.cpp:2418) silently DROPS any reading larger than that. Consequences today:
- **Reliably transmit** (≤200 B): gamepad, rtc, presence, apds, gps, fmradio(short RDS).
- **Silently dropped** (>200 B): **IMU** (~273 B), **ToF** with 2+ objects (~450 B), and the
  **thermal frame** (always — now disabled, §5).

The §2/§3 trim directly helps: dropping IMU's `seq` + 4 device-state flags (~90 B) puts
IMU (~193 B) **under the gate**. ToF's trim (only-detected objects + drop `distance_cm`)
fits the common 1–2 object case; a crowded 4-object scene is fundamentally >200 B and
needs Tier 3.

---

## 5. Three tiers (cheapest first)

1. **DONE — disable the broken thermal broadcast.** `gSensorSpecs[REMOTE_SENSOR_THERMAL]`
   set to `{nullptr,0,0}` (System_ESPNow_Sensors.cpp) — it was building a 4 KB frame every
   second that always failed the 200-byte gate. Also shrank the broadcaster buffer 4096→1024 B.
   Re-enable over mesh anytime by pointing the spec at `thermalBuildSummaryJSON` (~90 B).

2. **Minimal-essential trim (this doc).** One audit pass, by hand, HW-validated:
   - **a. Drop `seq` (tof/imu/thermal) + `total_objects` (tof)** — provably unconsumed (§3). Do first.
   - **b. ToF: emit only *detected* objects; drop `distance_cm`** (derive from mm). Shrinks + fits common case.
   - **c. Drop device-state duplicates + unconsumed flags** (§3b): drop FM/IMU body `enabled`
     (duplicate of the discovery-layer `enabled`) and APDS `colorEnabled`/`proximityEnabled`/
     `gestureEnabled` (verified unconsumed). Only IMU init flags need a consumer check before
     removal; relocate to `<id>status` if a UI wants them.
   - Rebuild + `sensors json` / `<x>read json` check per sensor; confirm web cards + OLED remote view still render.

3. **Chunked sensor transport (bigger project, the only *complete* fix).** Add multi-packet
   fragmentation + RX reassembly for sensor broadcasts (the encrypted file path already has
   chunking to model). This is the one investment that makes an **arbitrary-size** reading work
   over the mesh — fixes crowded-ToF, IMU, *and* re-enables the full thermal frame in one stroke.
   Would also need the 256-byte `RemoteSensorData.jsonData` RX buffer to grow.

---

## 6. Downstream / risk

- On-device: dropping `seq`/`total_objects` is zero-risk (no consumers). Relocating device-state
  needs a per-field consumer check (web IMU card, APDS mode toggles UI) before removal.
- Off-device FlutterApp: update its parse alongside the flash (no shim — no-compat). The app
  handoff prompt already documents the envelope; regenerate it after this trim.
- Method: BY HAND, no incremental commits mid-refactor, HW-test then commit
  (see [feedback_no_incremental_commits_during_refactor], [feedback_no_backwards_compat]).

## 7. Checklist (when executed)

- [x] Drop `seq` from tof/imu/thermal builders; drop `total_objects` from tof. **(done)**
- [x] ToF: loop only over detected objects; remove `distance_cm`. **(done — variable-length objects)**
- [x] Drop IMU init flags / APDS mode flags / FM `enabled` (verified unconsumed/duplicate). **(done)**
- [x] Update the 3 UI consumers (tof web id-remap, imu web not-ready, OLED remote-tof). **(done)**
- [x] Re-measure IMU vs the 200 B mesh gate. **(IMU ~187 B — now UNDER the gate ✓; ToF fits for 1–2 objects)**
- [ ] HW-test on FeatherS3, then revert scaffolding (board + `CUSTOM_ENABLE_*`).
- [ ] Regenerate the app handoff prompt for the trimmed shapes.
- [ ] (Optional) Tier 3 chunking — separate effort.
