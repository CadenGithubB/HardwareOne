# Sensor Envelope — Ground Truth (2026-07-28)

Status: **REFERENCE.** Produced by a 9-agent mapping workflow + 4 independent adversarial
verifiers + hand spot-checks of every single-sourced claim. Every statement below is
code-verified on this date; items verified by only one source are marked *(single-source)*.
Supersedes the status lines in SENSOR_READING_ENVELOPE_PLAN.md, SENSOR_ENVELOPE_CLEANUP_PLAN.md,
and SENSOR_RENDERING_UNIFICATION_PLAN.md (see §8 — all three carry stale statuses).

---

## 1. What exists

The standardization is real, shipped, and committed (by v0.99.3; all sensor files git-clean).
Three layers:

1. **Discovery** — `sensors json` → `{"schema":1,"seq":N,"brief":b,"sensors":[{id,name,kind,enabled,connected[,data]}]}`.
   `buildSensorsJson` System_I2C.cpp:1777-1814, `addSensorEntry` :1740-1766, cmd :1816.
   Per-sensor `data` = the builder's JSON deserialized + deep-copied (key-agnostic; a builder
   emitting invalid JSON silently loses its `data`).
2. **The reading seam** — one `int <x>BuildDataJSON(char*,size_t)` per sensor
   (`SensorDataFn` typedef System_I2C.cpp:1681). Both machine registries dispatch through it:
   `buildSensorsJson` and ESP-NOW `gSensorSpecs[]` (System_ESPNow_Sensors.cpp:128-185).
3. **The envelope** — `sensorEnvelopeBegin` (decl+contract System_I2C.h:528-539, impl
   System_I2C.cpp:1718). Writes, WITHOUT closing brace:
   `{"valid":<b>,"connected":<b>,"ts":<ms>` —
   returns bytes written, or 0 (with `buf[0]='\0'`) on overflow → caller must not append.
   **10 builders, 20 call sites, all honoring the 0-return rule** (each followed by
   `if (pos==0) return 0;`).
   *(2026-07-28, later than the audit below: the optional `age` key was REMOVED — it cost
   8-12 B against the 200 B mesh TX gate and had exactly one consumer, the local IMU web
   page's not-ready diagnostic, since repointed at `ts`. Derive staleness from `ts`.
   References to `age` in the findings below describe the pre-removal state.)*

Envelope callers: ds3231 :392,:400 · bno055 :593,:604 · vl53l4cx :556,:564,:591 ·
seesaw :421,:428 · apds9960 :86,:93 · ano_encoder :294,:301 · rda5807 :807 ·
pa1010d :364,:372 · sths34pf80 :523,:532 · mlx90640 :303,:326.

Implementation nuances (comment vs code):
- `age` is wraparound-clamped: future `ts` → `age:0` (System_I2C.cpp:1724). A ~49.7-day
  millis rollover makes a stale reading report fresh. Header comment doesn't mention the clamp.
- "leaves room for the caller's suffix" (System_I2C.h:536) actually guarantees only the NUL slot;
  harmless — every caller bounds its own suffix snprintf.
- Header says "~45 B"; measured **53-56 B** (7- to 10-digit `ts`). Size budgets derived from
  the comment inherit the error.
- Nine builders deliberately pass `lastUpdateMs=0` on lock-timeout → `ts:0`, **no `age` key**.
  A parser assuming `age` always exists breaks on any lock timeout.
- ToF's stale-data path (vl53l4cx :556) passes a real `ts` with `valid:false` — invalid
  readings are not always ts-less (deliberate, commented).

## 2. Coverage

| reading | builder | enveloped | note |
|---|---|---|---|
| presence | sths34pf80.cpp:517 | YES | |
| tof | vl53l4cx.cpp:544 | YES | detected-only objects `{id,distance_mm,status,valid}` |
| imu | bno055.cpp:587 | YES | body = accel/gyro/ori/temp only |
| gps | pa1010d.cpp:358 | YES | `valid` = real `dataValid` (any parsed NMEA — NOT gated on fix; `fix` is a body key) |
| fmradio | rda5807.cpp:788 | YES | cache gained `lastUpdate`+live `dataValid` (rda5807.h:25-26, stamped :478-479) |
| rtc | ds3231.cpp:386 | YES | |
| input/seesaw | seesaw.cpp:415 | YES | |
| input/ANO | ano_encoder.cpp:289 | YES | but see §6: `cmd_anoencoder` second shape; missing from `sensors json` on ANO builds |
| apds | apds9960.cpp:77 | YES | |
| thermal SUMMARY | mlx90640.cpp:298 | YES | min/avg/max |
| thermal FRAME | mlx90640.cpp:1085 | NO — deliberate | old shape `{"val","seq","mn","mx","w","h","data"}`; the builder wired to web (:116) & MQTT (:1153) |
| microphone | System_Microphone.cpp:795 | NO | status-shaped; internal 150 ms level cache exists but no SensorCache struct. Two shapes: builder has no `schema`, cmd_mic/cmd_miclevel emit `schema:1` |
| battery | System_Battery.cpp:492 | NO — deliberate template | `schema`+`present`+`lastReadMsAgo` idiom |
| camera | System_Camera_DVP.cpp:888 | NO — status only | |
| R1 ring health | System_SensorLogging.cpp:1491 | NO — competing idiom | `schema:2`, per-field `<x>Valid`/`<x>AgeSec` instead of document-level |

**Body cleanup (Tier 2) LANDED** — `seq`/`total_objects`/`distance_cm`/imu-state-flags/apds-mode-flags/fm-`enabled` all
gone from builders, with exactly one survivor: the ToF lock-timeout branch still emits
`"total_objects":0` (vl53l4cx.cpp:593). FM added `seeking` beyond the plan's keep-list.

## 3. Consumers — where "one parser" holds vs leaks

**Holds:** CLI `<x>read json` (all route through builders) · `sensors json` · ESP-NOW TX
(one dispatch table) · ESP-NOW RX (opaque memcpy, freshness from cache fields, System_ESPNow.cpp:3116/:3809) ·
G2 remote detail (generic formatter only) · web tof/imu/ano/fmradio endpoints (verbatim) ·
IMU web card (the ONE fully envelope-aware UI, bno055_web.h:46,70-71) · web games IMU tilt
(WebPage_Games.h:9751 gates on `j.valid && j.ori`).

**Leaks (own shape / cache-direct):**
- Web endpoints gps/rtc/presence/input hand-roll different key names (gps: `satellites/latitude/longitude` vs builder `sats/lat/lon`; rtc re-implements local-tz + Zeller inline :443-529; presence drops `tempShock` entirely). APDS has no endpoint.
- Web JS cards: bespoke value keys everywhere except IMU; seesaw card is a 3-way shape-sniffer whose named probes match nothing local (works via fallback).
- MQTT: 3 shapes in one document — builder-verbatim (thermal/tof/imu) vs hand-rolled (presence/gps/apds/rtc/input); hand-rolled blocks read caches WITHOUT mutexes; gamepad publishes LOGICAL button bits (different semantics from builder's raw bits).
- BLE stream (`buildSensorDataJSON` Bluetooth.cpp:2268-2323): cache-direct, 3 sensors only (thermal/tof/imu), hardcoded `"valid":true`, top-level `ts`=millis-at-serialization (INVERTED meaning vs envelope `ts`), frozen keys (`heading`, `dist_mm`). The old re-declared-structs hazard is FIXED (includes real headers, comment :34-40). CLI bridge carries `sensors json` to BLE clients as a parallel envelope-true path.
- OLED remote: bespoke parsers for input/imu/gps/tof/fm (OLED_Mode_Network.cpp:1088-1235); generic formatter is the default case only.
- All local rendering (OLED `i2csensor_*_oled.h`, G2_Page_Sensors.cpp:106-232), sensor logging (private snapshot → TEXT/CSV/TRACK), automations — cache-direct by design; envelope irrelevant.

The two "generic" renderers (`formatRemoteSensorReadable` System_ESPNow_Sensors.cpp:843, skip-list
:864-866; `hwRenderGenericSensor` WebPage_Sensors.h:557) achieve one-parser by SKIPPING envelope
keys, not consuming them. The generic formatter also skips arrays/objects (:880) → **on G2**
(generic-only) a remote ToF renders "(no fields)" and remote IMU shows only `temp`; **OLED is
unaffected** (bespoke parsers handle both).

**Blunt verdict: the envelope is real and uniformly produced, but today it is mostly
write-only metadata — consumed end-to-end by ~1.5 surfaces (IMU web card + games IMU gate).**
The durable win is producer-side: builders, `sensors json`, ESP-NOW TX/RX, and any FUTURE
consumer get one shape for free.

## 4. Blast radius (verified change-impact)

| change | firmware effect | external effect |
|---|---|---|
| envelope key rename | **NO compile error anywhere.** One reader: the skip-list → renamed key leaks onto G2/OLED remote readouts as a clutter line. IMU card + games IMU gate silently die. | web JS / Android / BLE-app see it silently |
| value key rename | **GPS only:** `getRemoteGPSData` (System_ESPNow_Sensors.cpp:1185-1236, keys :1221-1227) parses `fix/quality/sats/lat/lon/alt/speed` with `\|default` fallbacks → OLED map remote overlay silently zeros (OLED_Mode_Map.cpp:1088). Others: label-text changes only. | that sensor's web card + HA entities break silently |
| cache struct rename | **LOUD** — compile errors across automations resolver (System_Automation.cpp:2666-2752), logging fill (:607-763), builders, stubs, BLE, G2/OLED | none |
| cache SEMANTIC change (units, valid meaning) | silent drift into automation thresholds + CSV history | silent |

Automations, sensor logging, and LLM/voice are 100% envelope-immune (cache-direct / untouched).

## 5. ESP-NOW transport truth (docs are stale here)

- `v4_broadcast_sensor_data` **no longer exists** (tombstones System_ESPNow.cpp:2610-2613).
  Readings travel ONLY as session-encrypted unicast `SENSOR_ENVELOPE=154` via
  `v4_send_sensor_envelope` (:2619) under the secure-fetcher lease system (SUBSCRIBE/UNSUBSCRIBE/
  ONESHOT, System_ESPNow_Sensors.h:97-99; controller side = Phase-1b test cmd `espnowsensorreq`).
- TX gate unchanged: `jsonLen > 200` reject (System_ESPNow.cpp:2621, now DEBUGF-logged).
  Bond path has a SECOND cap: 210 B (`sendBondedSensorData` System_ESPNow.cpp:13560; its
  "max 226/218" comment is stale V3 numbers).
- Measured payloads vs the gate (computed from the actual format strings): thermal-summary ~90 ·
  gamepad ~93 · ano ~102 · apds ~110 · ToF 1-obj ~118 / 3-obj ~222 **DROP** · rtc ~132 ·
  gps ~148 · **imu floor 186 / resting 194-197 / worst-motion ~219 DROP** · presence ~198,
  crosses at long uptime · fmradio full-RDS ~258 **DROP**.
  **IMU drops exactly during vigorous motion** (negative signs widen every float field).
  Cheapest fix per the original plan: drop `age` when tight (saves 9-12 B).
- The "256 B RX overflow" concern is a phantom: TX gate rejects first; both RX handlers truncate
  safely. Failure mode is silent drop, not corruption.
- Unwired TX slots despite working builders: APDS unconditionally `nullptr`
  (System_ESPNow_Sensors.cpp:184); thermal `nullptr` both arms (:133,:135 — re-enable =
  point at `thermalBuildSummaryJSON`).
- Tier 3 chunking: not started; would now target the unicast path.

## 6. Defect register

Multi-source confirmed (≥2 independent verifications or hand-checked):
1. **GPS + RTC report `valid:true` forever after `close<x>`** — stop paths clear only
   `g<X>Connected` (pa1010d.cpp:428-431 task-exit; ds3231.cpp:572 rtcStop); `dataValid=false`
   exists only in the NEXT start's stale-cache wipe (:107 / :593). Envelope emits
   `valid:true,connected:false` + stale values in the stop→restart window. The other 8 clear on stop.
2. **Thermal web card dead** — reads `d.v` (mlx90640_web.h:61); both producers emit `"val"`.
   Predates the envelope.
3. **Games gamepad polling double-dead** — polls `sensor=gamepad` but handler matches `"input"`
   only (WebPage_Games.h:9781 vs WebPage_Sensors.cpp:213) → invalid-sensor error; AND its gate
   reads `j.v` while the handler emits `"val"`. Two independent fixes required.
4. **`isSensorConnected("gamepad")` always false** — module table passes "gamepad"
   (System_Utils.cpp:2923); every i2cSensors row uses "input" → CLI help-gating + settings
   schema show gamepad "(Not Connected)" forever.
5. **MQTT thermal publish arithmetically dead** — 768-px frame (~3.8-4.6 KB) can never fit the
   2048 B buffer (System_MQTT.cpp:1152); builder returns 0; `len>0` guard silently skips. Has
   never published data.
6. **HA discovery templates match nothing** for exactly the 3 sensors that DO nest builders:
   `thermal.min_temp/max_temp/avg_temp` vs `mn/mx`; `tof.distance` vs `objects[]`;
   `imu.accel_x` vs `accel.x` (System_MQTT.cpp:591-610).
7. **`cmd_anoencoder` second shape** — `{"schema":1,"connected","position",...}`
   (ano_encoder.cpp:316-323): no envelope, `position` vs builder's `pos`, reads cache with
   NO mutex. The only unlanded §4 migration item.
8. **`sensors json` loses "input" on ANO builds** — entry gated `#if ENABLE_GAMEPAD_SENSOR`
   only (System_I2C.cpp:1805-1810); ANO forces that 0. ESP-NOW's table already fixed this
   exact bug with `#elif ENABLE_ANO_ENCODER` (System_ESPNow_Sensors.cpp:152-163).
9. **ToF unchecked appends** — only builder violating the checked-append pattern
   (vl53l4cx.cpp:558,566,576,586,593); on truncation returns a would-be length consumed raw by
   web :193 / MQTT :1163. Latent (unreachable at current 500-1024 B buffers vs ~310 B worst).
10. **BLE latent OOB** — unchecked `pos += snprintf` chain ending `buf[pos]='\0'`
    (Bluetooth.cpp:2322) on 512 B stack buf; safe today (~230 B worst) but structurally fragile.
11. **`buildThermalDataJSONInteger` is dead code** — zero call sites; stale comment at
    mlx90640.cpp:1552 claims the broadcaster uses it. (Re-verify before deleting, per convention.)
12. MQTT hand-rolled sensor blocks read caches **without mutexes** (System_MQTT.cpp:1181-1230).

Single-source (code-cited, not independently re-verified):
13. FM RDS strings enter JSON unescaped (rda5807.cpp:101-123 strncpy → bare `%s` :809-820) —
    a station broadcasting `"` or `\` corrupts the reading JSON.
14. SSE truncation — 2048 B status doc snprintf'd into `char[1200]`
    (WebServer_Events.cpp:208) → malformed JSON to every SSE client above ~1170 B.
15. `ConnectedDevice.isConnected` never set false; `scanBusForDevices` has zero callers;
    `detect` and `sensors json` can disagree on presence (System_I2C.cpp:1033/:1262/:1061).
16. Ten CLI `<x>read json` fallbacks emit truncated envelopes (`{"valid":false}` /
    `{"valid":false,"error":"buffer"}` — no `connected`/`ts`); mlx90640.cpp:340 adds a bare
    `enabled` key outside the envelope.
17. Web tof/imu handlers pass unchecked `jsonLen` to `httpd_resp_send` (WebPage_Sensors.cpp:193,:210)
    → 0-length `application/json` body on builder failure (fmradio/thermal DO guard).
18. `sensors json` vs `/api/sensors/status`: key `sensors` is an array in one, an id-keyed map
    in the other (System_I2C.cpp:1781 vs :2591); NonI2C registry (camera/mic) appears only in
    the latter. `kind` vocabulary (scalar/vector/stream) has zero firmware consumers.
19. Outer `connected` (boot-scan registry, refreshed only at boot/`discover`) vs embedded
    `data.connected` (live flag) can disagree within one `sensors json` document; `input` alone
    uses the live flag for the outer field (System_I2C.cpp:1809).

## 7. Outstanding work, ranked

1. `cmd_anoencoder` → route through `anoEncoderBuildDataJSON` (fixes shape + mutex; defect #7).
2. One-liner: drop `total_objects` from vl53l4cx.cpp:593.
3. `#elif ENABLE_ANO_ENCODER` in buildSensorsJson (defect #8).
4. Clear `dataValid` in GPS task-exit + rtcStop (defect #1).
5. Convert web gps/rtc/presence/input endpoints to their builders (kills the second shapes,
   restores `tempShock`, deletes inline tz/Zeller math; FlutterApp parse updates in same cycle).
6. MQTT: adopt builders for the 5 hand-rolled sensors (fixes mutex-less reads too) + fix or
   drop the 3 broken HA templates + decide thermal (summary vs delete; the frame path is dead).
7. Fix thermal web card (`d.v`→`d.val`) and games gamepad (name + key).
8. Wire ESP-NOW APDS + thermal-summary slots; add the drop-`age`-when-tight rule for imu/presence/fm.
9. Regenerate the FlutterApp handoff for current shapes (cleanup plan :159 — still open).
10. Tier 3 chunking against the unicast transport (only if >200 B readings matter).
11. Doc status-line sweep (§8) + this file's own eventual retirement into the plans.

## 8. Stale doc lines (found 2026-07-28; size/transport items + status lines corrected same day)

- SENSOR_READING_ENVELOPE_PLAN.md:5 "PLAN ONLY. Not started." → executed + committed; open
  items = §7. Checklist :101-109 unticked but mostly done; :107 names the wrong at-risk sensors
  (says gps/input; actually imu/presence/fmradio). Line refs drifted ~90 lines throughout §4.
- SENSOR_ENVELOPE_CLEANUP_PLAN.md: :104-106 names deleted `v4_broadcast_sensor_data`; :157/:111
  "IMU under the gate ✓" is wrong for motion (worst ~219 DROPS); :42 keep-list still shows
  dropped `detected`; :46 omits added `seeking`; :153 `total_objects` tick premature; "9 sensor
  readings" undercounts (10 enveloped builders); :158-159 boxes (HW-test/scaffolding-revert,
  app handoff) never closed.
- SENSOR_RENDERING_UNIFICATION_PLAN.md: header vs :173 checklist self-contradictory on ToF/thermal
  parameterization; :54 stringify-fallback line superseded by `hwRenderGenericSensor`; L2/L3
  genuinely unstarted (no System_SensorFormatters.*, zero descriptor hits).
- Memory index entries for envelope/rendering/fetcher said "plan-only"/"design-only" — corrected 2026-07-28.
