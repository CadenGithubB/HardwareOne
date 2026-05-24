# Sensors Umbrella Removal — Migration Plan

## Goal

Eliminate the `DEBUG_SENSORS` umbrella flag. Every per-sensor and sensor-adjacent log call should be gated by its own dedicated flag instead of the catch-all `DEBUG_SENSORS` bit. Once migrated, the "Sensors" UI group can be deleted entirely.

## Current state (audit)

337 SENSORS-tagged macro calls across 20 files:

| Macro | Count | Gate |
|---|---|---|
| `DEBUG_SENSORSF` | 166 | `DEBUG_SENSORS` (suppressed when umbrella off) |
| `INFO_SENSORSF`  | 94  | `DEBUG_SENSORS` + `LOG_LEVEL_INFO` |
| `WARN_SENSORSF`  | 9   | `0xFFFFFFFF` (always on) — tag still says `[SENSORS]` |
| `ERROR_SENSORSF` | 68  | `0xFFFFFFFF` (always on) — tag still says `[SENSORS]` |

Even the always-on WARN/ERROR variants need migration so the log tag matches the actual subsystem (`[ERROR][CAMERA]` instead of `[ERROR][SENSORS] [CAM_PWR] ...`).

## Per-file → target flag mapping

### Tier 1: clean 1:1 mappings (driver files; mechanical migration)

| File | Calls | Target flag | Notes |
|---|---|---|---|
| `i2csensor-mlx90640.cpp` | 37 | `DEBUG_THERMAL` | |
| `i2csensor-seesaw.cpp` | 31 | `DEBUG_GAMEPAD` | 5 WARN |
| `i2csensor-pa1010d.cpp` | 27 | `DEBUG_GPS` | flag is currently dead — fix in passing |
| `i2csensor-ds3231.cpp` | 21 | `DEBUG_RTC` | flag is currently dead |
| `System_Microphone.cpp` | 19 | `DEBUG_MICROPHONE` | |
| `i2csensor-sths34pf80.cpp` | 15 | `DEBUG_PRESENCE` | flag is currently dead |
| `i2csensor-bno055.cpp` | 14 | `DEBUG_IMU` | 1 WARN |
| `i2csensor-rda5807.cpp` | 12 | `DEBUG_FMRADIO` | |
| `i2csensor-apds9960.cpp` | 10 | `DEBUG_APDS` | flag is currently dead |
| `i2csensor-vl53l4cx.cpp` | 7  | `DEBUG_TOF` | |
| `HAL_Input.cpp` | 6 | `DEBUG_GAMEPAD` | input HAL = gamepad |
| `System_Camera_DVP.cpp` | 3 | `DEBUG_CAMERA*` | leftover from camera-breakout cleanup; line 808 missed |
| `System_ImageManager.cpp` | 3 | `DEBUG_CAMERA` | uses camera path |
| `System_Camera_Video.cpp` | 1 | `DEBUG_CAMERA_VIDEO` | one ERROR I left |
| **Subtotal** | **206** | | |

### Tier 2: existing flag, but cross-cutting (per-call review needed)

| File | Calls | Target flag | Notes |
|---|---|---|---|
| `System_Maps.cpp` | 28 | `DEBUG_MAPS` | 2 WARN; existing flag has sub-flags Loading/Rendering/Perf — most calls go to parent |
| `OLED_Mode_Map.cpp` | 4 | `DEBUG_MAPS` | |
| `System_I2C.cpp` | 16 | `DEBUG_I2C` | 1 WARN; `INFO_I2CF`/`WARN_I2CF`/`ERROR_I2CF` already exist |
| **Subtotal** | **48** | | |

### Tier 3: needs categorization (cross-cutting orchestration)

| File | Calls | Plan |
|---|---|---|
| `System_ESPNow_Sensors.cpp` | 61 | Per-tag mapping. Tags `[SENSOR_STREAM]`/`[SENSOR_STREAM_CMD]`/`[SENSOR_DATA_TX]`/`[SENSOR_TX]`/`[BCAST_*]` → `DEBUG_ESPNOW_STREAM`. Tags `[SENSOR_STATUS_TX]`/`[CACHE_UPDATE]`/`[GET_REMOTE_JSON]` → `DEBUG_ESPNOW_METADATA`. ~10 untagged calls need per-call review. |
| `OLED_Utils.cpp` | 15 | Display init/probe/boot-animation. Two options: (a) fold into `DEBUG_SYSTEM` + `DEBUG_I2C` per call, or (b) introduce a new `DEBUG_DISPLAY` flag. **Recommend (b)** — display is its own subsystem and 15 calls is enough to justify it. New flag at bit 80, with parent + maybe init/probe sub-flags later. |
| `HardwareOne.cpp` | 7 | Per-tag: `[STATUS_BUMP]` (3) → `DEBUG_ESPNOW_METADATA` (sensor status broadcast aggregation). `[SSE_BROADCAST]` (3) → `DEBUG_SSE` (already has `INFO_SSEF`/etc patterns? no — check). `[Boot] Gamepad init result` (1) → `DEBUG_SYSTEM` + systemBoot sub-flag. |
| **Subtotal** | **83** | |

**Grand total: 337 calls.** Tier 1 + Tier 2 = 254 mechanical; Tier 3 = 83 needing per-call eyes.

## New macros to add

### Always-on severity macros (no flag gating, just tag)

For every per-sensor flag missing them. Existing pattern lives in `System_Debug.h` lines 519-560.

```cpp
#define ERROR_THERMALF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][THERMAL] " fmt, ##__VA_ARGS__)
#define WARN_THERMALF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][THERMAL] "  fmt, ##__VA_ARGS__); } while (0)
```

Need to add (severity × subsystem):

| Subsystem | Need INFO_*F | Need WARN_*F | Need ERROR_*F |
|---|---|---|---|
| THERMAL | yes | no usage | yes |
| TOF | yes | no usage | yes |
| IMU | yes | yes (1 call) | yes |
| GAMEPAD | yes | yes (5 calls) | yes |
| APDS | yes | no usage | yes |
| PRESENCE | yes | no usage | yes |
| GPS | yes | no usage | yes |
| RTC | yes | no usage | yes |
| FMRADIO | yes | no usage | yes |
| MAPS | yes | yes (2 calls) | yes |
| MIC | yes | no usage | yes |
| CAMERA | already added | no usage | **yes** (need to add) |
| DISPLAY (NEW) | yes | no usage | yes |
| SSE | yes (if HardwareOne goes there) | no | no |

### New flag (Tier 3 only)

- `DEBUG_DISPLAY` — bit 80 (next free hi-half bit after camera 76-79). Covers OLED init / probe / boot animation / mode transitions. Pure cross-cutting display subsystem flag.

No other new flag bits needed — every other migration target has an existing flag.

## Order of execution

Each tier should land as its own commit so reverts stay clean.

### Step 0 — macro infrastructure (single commit)

1. Add the ~25 new INFO_*F / ERROR_*F / WARN_*F macros to `System_Debug.h`.
2. Add `DEBUG_DISPLAY` flag bit + setting field + DBG_MAP + UI registry row + cmd_* setter.
3. Build to verify no syntax errors.

### Step 1 — Tier 1 mechanical migrations (one commit per file group)

Order from smallest to largest so the pattern is established before tackling the long files:

1. Camera leftovers (4 calls across 3 files — 5 minute fix)
2. `i2csensor-vl53l4cx.cpp` (TOF, 7)
3. `i2csensor-apds9960.cpp` (APDS, 10) — also "fixes" dead APDS flag
4. `i2csensor-rda5807.cpp` (FMRADIO, 12)
5. `i2csensor-bno055.cpp` (IMU, 14)
6. `i2csensor-sths34pf80.cpp` (PRESENCE, 15) — fixes dead PRESENCE flag
7. `System_Microphone.cpp` (MICROPHONE, 19)
8. `i2csensor-ds3231.cpp` (RTC, 21) — fixes dead RTC flag
9. `i2csensor-pa1010d.cpp` (GPS, 27) — fixes dead GPS flag
10. `i2csensor-seesaw.cpp` (GAMEPAD, 31)
11. `i2csensor-mlx90640.cpp` (THERMAL, 37)
12. `HAL_Input.cpp` (GAMEPAD, 6)
13. `System_ImageManager.cpp` (CAMERA, 3)

### Step 2 — Tier 2 (existing flag, easy)

14. `OLED_Mode_Map.cpp` (MAPS, 4)
15. `System_I2C.cpp` (I2C, 16)
16. `System_Maps.cpp` (MAPS, 28)

### Step 3 — Tier 3 (per-call review)

17. `OLED_Utils.cpp` (DISPLAY, 15) — also introduces DEBUG_DISPLAY usage
18. `HardwareOne.cpp` (mixed, 7)
19. `System_ESPNow_Sensors.cpp` (ESPNOW_STREAM/METADATA, 61) — biggest, hardest

### Step 4 — kill the umbrella

Once `grep -rE '(DEBUG|INFO|WARN|ERROR)_SENSORSF\(' components/hardwareone/` returns zero hits:

1. Delete `DEBUG_SENSORS` flag from `System_Debug.h` (frees bit 6)
2. Delete `DEBUG_SENSORSF` / `INFO_SENSORSF` / `WARN_SENSORSF` / `ERROR_SENSORSF` macros
3. Delete `debugSensors` and `debugSensorsGeneral` fields from Settings struct + ctor
4. Delete the two `DBG_MAP` entries
5. Delete the two UI registry rows
6. Delete `cmd_debugsensors` + `cmd_debugsensorsgeneral` setters
7. Delete the command-table entries for those two cmds
8. Delete the `DEBUG_SENSORS` case in the flag-name mapping (`if (flag & DEBUG_SENSORS) return "SENSORS"`)
9. Verify [WebPage_Games.h:10951](components/hardwareone/WebPage_Games.h#L10951) `'[DEBUG_SENSORS'` text-grep is dead pattern (looks orthogonal — checks log lines, not macro)
10. Build and verify Sensors UI group is gone

## Risks

- **Behavioral change**: any user who has `debugSensors=true` saved in `settings.json` was getting all the per-sensor info. After migration, they'll need to enable each per-sensor flag they care about. Mitigation: settings JSON ignores unknown keys, so old `debugSensors` becomes a no-op — no migration script needed, but document the change in release notes.
- **Cross-cutting calls in `System_ESPNow_Sensors.cpp`**: a few `DEBUG_SENSORSF` calls at message dispatch points might cover multiple sensor types per call. Per-tag mapping handles it; the 10 untagged calls need eyes.
- **Tag rename in always-on logs**: `ERROR_SENSORSF("[CAM_PWR] queue create failed")` becomes `ERROR_CAMERAF("[CAM_PWR] queue create failed")`. Anyone grepping logs for `[ERROR][SENSORS]` will need to update.

## Estimated scope

| Step | Files | Calls | Effort |
|---|---|---|---|
| 0 — macros | 1 | n/a | ~30 min |
| 1 — Tier 1 | 13 | 206 | ~2 hr (mechanical perl passes) |
| 2 — Tier 2 | 3 | 48 | ~30 min |
| 3 — Tier 3 | 3 | 83 | ~1 hr (per-call review) |
| 4 — kill umbrella | ~5 files | -337 (deletions) | ~30 min |
| **Total** | | | **~4-5 hr work** |

## Verification gates

After each tier:
1. `grep -rE 'INFO_SENSORSF|DEBUG_SENSORSF' components/hardwareone/<file>` → 0 hits in the migrated file
2. Full project build (`idf.py build`)
3. Spot-check the UI: navigate to debug settings, confirm the per-sensor toggle now actually controls the migrated output (smoke test by enabling/disabling and watching log)
