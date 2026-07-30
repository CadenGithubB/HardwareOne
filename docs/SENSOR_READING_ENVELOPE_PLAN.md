# Sensor reading envelope - standardization plan

**Goal:** give every genuine sensor *reading* a uniform JSON shape so the one device - driven over Bluetooth, web, OLED, serial, MQTT, and ESP-NOW - can be developed against **one parser instead of N-per-sensor**. This is the natural completion of the already-standard *discovery* layer (`sensors json`) and is in scope for [project_web_api_modernization].

Status: **EXECUTED - committed and shipped (by v0.99.3).** All 10 builders open with `sensorEnvelopeBegin` (System_I2C.cpp:1718; contract System_I2C.h:528). The body cleanup followed in [SENSOR_ENVELOPE_CLEANUP_PLAN.md](SENSOR_ENVELOPE_CLEANUP_PLAN.md). One sec. 4 item never landed: **`cmd_anoencoder` still hand-rolls a second shape** (`{"schema":1,"connected","position",...}`, no envelope, mutex-less cache read) - converge it through `anoEncoderBuildDataJSON`. Current audited state incl. consumer-adoption reality: [SENSOR_ENVELOPE_GROUND_TRUTH_2026-07-28.md](SENSOR_ENVELOPE_GROUND_TRUTH_2026-07-28.md). *(Original method notes, kept: staged, HW-validated, no incremental commits mid-refactor, fix BY HAND - no python/sed sweeps.)*

> **No backwards-compat needed** (per [feedback_no_backwards_compat]): the user owns the only devices and erases before every flash, so there is nothing in the field to stay compatible with. Do the clean key renames directly - **no dual-key shims, no version gates, no "additive-first-then-rename" staging for compatibility.** The off-device FlutterApp just gets its parse updated in the same flash cycle.

---

## 1. Why (the NxM -> N+M argument)
Every interface consumes sensor data. If each sensor emits its own shape, each interface must know each sensor -> (interfaces x sensors) integrations, and a *new* sensor means touching BLE + web + OLED + serial. A shared envelope collapses this: each sensor emits one shape, each interface parses one shape, and a new sensor shows up everywhere for free. The payoff is extensibility, not just tidiness.

## 2. Current state (what's already banked vs missing)
- **Discovery layer - standardized.** `sensors json` -> `{schema:1, seq, brief, sensors:[{id,name,kind,enabled,connected,data?}]}` (`addSensorEntry`, System_I2C.cpp:1648-1719). Uniform `enabled` (live `g<X>Enabled`, toggled by `open<id>`/`close<id>`) vs `connected` (`isSensorConnected()` bus presence). Fixed `kind` vocab: scalar|vector|stream.
- **The reading SEAM - standardized (the expensive part is done).** Every read path funnels through ONE primitive per sensor: `int <x>BuildDataJSON(char* buf, size_t)` (`SensorDataFn`, System_I2C.cpp:1620) - used by `<x>read json`, the `sensors json` embedded `data`, MQTT, ESP-NOW streaming, and web endpoints.
- **The reading PAYLOAD - NOT standardized (this plan).** The ~9 builders hand-roll divergent inner keys: validity is `valid` vs `val` vs an error-string vs none; timestamp is `ts` vs `timestamp` vs `ageMs` vs absent; no `connected` in most; units baked into key names. Consequence: a client can't ask "is this reading real / how old is it" one way. ToF has no top-level `valid`; thermal has no timestamp - latent consumer bugs.

## 3. Target envelope (thin, opt-in, JSON-only)
A shared **inner** contract that standardizes only the 4 things consumers trip on, leaving each sensor's value keys alone:

```json
{ "valid": true, "connected": true, "ts": 123456, "age": 240, <existing value keys...> }
```
- `valid` (bool) - is this reading real/fresh (one key, everywhere)
- `connected` (bool) - is the sensor physically present now
- `ts` (uint ms) - cache `lastUpdate` (millis)
- `age` (uint ms, optional) - `millis() - lastUpdate`; omit when `ts==0`

**Deliberately NOT in the payload:** `id`/`name`/`kind` (already live one level up in the registry entry - do not duplicate) and a machine `units` field (deferred: low value, real byte cost on 256 B frames).

### Shared helper
```c
// Writes the leading envelope {"valid":..,"connected":..,"ts":..,"age":.. , returns bytes written (0 on overflow).
// Omits "age" when lastUpdateMs==0. Does NOT close the object - builder appends its value keys then closes.
int sensorEnvelopeBegin(char* buf, size_t bufSize, bool valid, bool connected, unsigned long lastUpdateMs);
```
Each `BuildDataJSON` opens with this, keeps its value-`snprintf` in the middle, and closes - preserving the single-builder seam that is already the win. No new layer.

## 4. Per-sensor migration table
Ordered easiest -> most involved. **Only `fmradio` reaches upstream.**

| Sensor (file) | valid today | ts today | edit |
|---|---|---|---|
| **imu** (bno055.cpp:586) | `valid` yes | `ageMs`+`timestamp` (dup) | rename `timestamp`->`ts`, `ageMs`->`age`. Lowest effort. |
| **rtc** (ds3231.cpp:385) | `valid` yes | `ts` yes | add `connected` (`gRtcConnected`), optional `age`. |
| **presence** (sths34pf80.cpp:479) | `valid` yes | `ts` yes | add `connected`, optional `age`. |
| **tof** (vl53l4cx.cpp:522) | none top-level (per-object only) | `timestamp` | replace the two `{"error":...}` early-returns with envelope; add top-level `valid`; `timestamp`->`ts`. |
| **apds** (apds9960.cpp:75) | `valid` (means *enabled*, not fresh) | none | add `connected` + `ts` (`APDSCache.apdsLastUpdate`); fix `valid` semantics to freshness. |
| **gps** (pa1010d.cpp:317) | `val:1` (hardcoded, fake) | none | `val`->`valid` (real: `gGpsConnected && hasFix`); add `connected`, `ts` (`GPSCache.lastUpdate`). |
| **gamepad** (seesaw.cpp:414) | `val:1` (hardcoded) | none | `val`->`valid` (`InputCache.dataValid`); add `connected`, `ts` (`InputCache.lastUpdate`). |
| **ano** (ano_encoder.cpp:288) | `val:1` (hardcoded) | none | `val`->`valid`; add `connected`, `ts`. **Also fix the TWO divergent json shapes** - the builder emits `val` while `cmd_anoencoder` (line 305) emits `connected`+`schema`. Converge them. |
| **fmradio** (rda5807.cpp:732) | none (`connected`/`enabled` only) | none | add `valid`. **UPSTREAM: `FMRadioCache` has no timestamp field and a dead `dataValid`** - must add timestamp capture in the poll/update path to source `ts`/`valid`. |
| **thermal** (mlx90640.cpp, built inline) | `val` (int 1/0) | none (`seq` only) | **out of scope for the frame body.** Optional: extract a real `thermalBuildDataJSON` and normalize its *summary* envelope (min/max/avg), but NEVER the 768-px frame. |

Not shared-builder participants (leave / template): **battery** (`buildBatteryJson`, ArduinoJson idiom) already models the target - `schema` + `present` gate + relative `lastReadMsAgo` + single source. **Copy its pattern; do not force it into the generic shape** (its `present`/`usbPresent`/`backend` keys are legitimately domain-specific). **mic**: `level` is computed on-demand (`getAudioLevel()`), no cache struct -> decide separately.

## 5. Downstream effects (who parses this - the break-set)
**Verified: NO on-device consumer breaks on the rename.** Traced every parser:
- **Web HTTP handlers** (WebPage_Sensors.cpp) - forward builder JSON **verbatim** (tof/imu/ano/fmradio); gps/gamepad/rtc/presence endpoints are **cache-direct** with a *different* key set and don't touch the builders at all. Pass-through -> safe.
- **Web JS cards** (`i2csensor_*_web.h`) - IMU reads `valid` (kept); ToF reads per-*object* keys (not the top-level envelope); GPS card reads the cache-direct endpoint (`fix`/`satellites`/`latitude`, not the builder's `val`); gamepad **already probes `j.v || j.valid || j.ok`** defensively; ANO reads `pos`/`buttons`/`axis` (ignores `val`); FM reads value keys (envelope adds are additive). All safe.
- **MQTT** (System_MQTT.cpp) - nests the builder blob **verbatim**; HA templates read `.tof.distance`/`.imu.accel_x`/`.gps.lat`, never `val`/`valid`/`ts`. Safe (even off-device HA).
- **BLE streaming** (Bluetooth.cpp `buildSensorDataJSON`) - reads the cache structs **directly** with its own compact schema. Fully decoupled.
- **ESP-NOW RX** (System_ESPNow_Sensors.cpp) - stores the JSON as an **opaque blob**, wraps verbatim; freshness from the cache's own `.valid`/`.lastUpdate`, not a JSON key.
- **OLED / G2 generic remote render** (`formatRemoteSensorReadable`) - its skip-list **already contains both `val` AND `valid`/`ts`** -> forward-compatible; the `timestamp`->`ts` rename actually *improves* it (currently `timestamp` leaks onto the screen).
- **OLED bespoke remote parsers** (OLED_Mode_Network.cpp) - read **value** keys (`lat`/`ori`/`x`/`objects`/`frequency`...), not envelope keys. Safe.

### The one downstream consumer to update - the off-device FlutterApp
The **FlutterApp** consumes `sensors json` `data` and `<x>read json` **verbatim** off-device; if it parses `.val` (gps/gamepad/ano) or `.timestamp` (tof/imu), the rename changes what it reads. **No shim/compat needed** (see the no-backwards-compat note) - the user owns all devices and erases before flashing, so simply **update the app's sensor parse in the same flash cycle.** No dual-key emission, no version gate, no additive-first-for-compat. It's a coordinated app change, not a compatibility problem. (Memory: FlutterApp is community RE; firmware wins on conflicts.)

## 6. Upstream effects (who feeds this)
**Almost entirely a pure output-layer change.** For 11 of 12 genuine-reading sensors, `valid`/`ts`/`age` are already in cache fields the poll task populates (`<X>Cache.lastUpdate` + `<X>Cache.dataValid`) - **no poll-task edit**. Exceptions:
- **fmradio - the lone upstream change.** `FMRadioCache` (rda5807.h:23) has **no timestamp** and its `dataValid` is present-but-dead. Sourcing `ts`/`valid` requires stamping a timestamp + setting validity in the FM update path.
- **mic-level** - `level` is computed on-demand (`getAudioLevel()`), not cached; decide whether to cache a `lastUpdate` or leave mic out.

## 7. Buffer constraint (CORRECTED 2026-07-28 - the real ceiling is the 200 B TX gate)
The binding limit is the **200-byte TX gate** in `v4_send_sensor_envelope` (System_ESPNow.cpp:2621; readings travel as session-encrypted unicast since the D2 secure fetcher - the plaintext broadcast this section predated is deleted), plus a separate **210 B bond-path cap** (:13572). The `char[256]` RX buffer (`REMOTE_SENSOR_BUFFER_SIZE`) never binds - the TX gate rejects first and RX truncates safely. The envelope header measures **53-56 B** as printed (not ~40-50 as estimated here). **Keep keys short**, and the at-risk sensors are **imu** (worst-case motion ~219 B), **presence** (~198-201 at the edge), **fmradio** (full RDS ~258) and **ToF >=3 objects** - gps (~148 B) and input (~93 B) are the *safest*, the opposite of the original guess. If a reading doesn't fit, drop `age` (derivable from `ts`) on the mesh path, or emit it json-only on roomy transports (CLI/web).

## 8. Staging (deliberate, HW-validated - no compat staging)
Since no backwards-compat is required, the additions and renames land together in ONE clean convergence - staging is only for HW validation, not to preserve an old format.
1. **Add the shared helper** `sensorEnvelopeBegin` (System_I2C or System_Debug). No behavior change yet.
2. **Converge every builder in one pass** - add `connected`/`ts`/`age`/`valid` where missing (gps, gamepad, apds, fmradio[+upstream ts], ano dedup) AND do the renames directly (`val`->`valid`, `timestamp`->`ts`, `ageMs`->`age`). Build + HW test. Update the FlutterApp's sensor parse in the same flash cycle.
3. **Optional:** extract thermal's `BuildDataJSON` for a normalized summary envelope (frame body untouched).
Verify with `sensors json`, each `<x>read json`, and a mesh remote read; confirm `formatRemoteSensorReadable` output and the web cards still render.

## 9. Stop-line (do NOT wrap - genuinely different shapes)
- **servo/PCA9685** - actuator, no read-back. Nothing to envelope.
- **camera** - media device; config + capture acks, not a reading.
- **thermal frame** (768 px) - summary can carry the envelope; the frame cannot.
- **mic config blob** / **battery display panel** - device/presentation state, not scalar readings.
- **Human text formatters** - leave (UX; some read live-vs-cache by design).
- **sensorlog CSV** - a deliberate fixed-column offline-analysis contract; do NOT route it through the JSON builders.
- **Do not duplicate `id`/`name`/`kind`/`units` into each payload** - that envelope already exists one level up.

## 10. Risks & rollback
- **FlutterApp parse** (sec. 5) - update the app's keys in the same flash cycle (you control it; no shim). A coordinated app change, not a compat problem.
- **200 B ESP-NOW TX gate** (sec. 7, corrected) - the at-risk shapes are imu/presence/fmradio/ToF>=3obj; gps and input are the safest.
- **tof/gps/rtc text-vs-cache divergence** is behavioral, not formatting - do not "fix" blind while here.
- **Rollback:** staged; the additive pass is safely revertible; the rename pass is one atomic commit to revert.

## 11. Checklist (reconciled 2026-07-28)
- [x] `sensorEnvelopeBegin` helper added *(System_I2C.cpp:1718)*
- [x] Additive pass: gps / gamepad / apds / fmradio (+ upstream ts) carry `valid`+`connected`+`ts` *(done; **ano-dedup still open** - `cmd_anoencoder`'s second shape, see Status)*
- [x] fmradio upstream: FMRadioCache timestamp + real dataValid *(rda5807.h:25-26; stamped :478-479, cleared :224)*
- [x] Decide mic-level *(decided: leave out - the mic is an audio pipeline, not a scalar reading)*
- [ ] FlutterApp sensor parse updated for the new keys *(handoff regen still open; APP_JSON_CONTRACT.md still pins `anoencoderread`'s old shape)*
- [x] ~~256 B RX headroom verified on gps + input~~ *(moot - the 200 B TX gate binds first; see sec. 7 correction)*
- [x] Build green + HW-validated *(committed and shipped by v0.99.3)*
- [x] Thermal summary-envelope extraction (optional) *(thermalBuildSummaryJSON; registered in `sensors json`)*

Same legibility goal as [project_uniform_return_contract]; this is the *data*-layer analogue of the OK:/Error: *status*-layer work.
