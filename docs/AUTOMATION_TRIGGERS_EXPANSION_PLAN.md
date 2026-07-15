# Automation Triggers Expansion Plan

Status: IMPLEMENTED (built green on feathers3, 2026-07-12) - awaiting HW flash/test.
Goal: expand the set of internal system capabilities that automations can
check/poll to gate and branch their behavior.

## Implemented (2026-07-12)
17 new condition variables added to the ONE resolver evaluateCondition()
(System_Automation.cpp), so each works in BOTH the "Fire when" gate and the
Add-Logic IF/THEN with no other backend change:
- Numeric: BATTERY, HEAP, PSRAM, FSFREE, UPTIME, CHIPTEMP, HOUR, RSSI, SPEED, SATS, PEERS
- Enum:    WIFI, BLE, NTP, DAY, GPS, LLM
Each is a cheap synchronous read, `#if ENABLE_*`-guarded where the subsystem is
optional (WiFi/GPS/ESP-NOW/LLM), returns false when absent; CHIPTEMP is
NAN-guarded, PEERS is self-MAC-filtered + liveness-checked (30s heartbeat).
Web UI: both dropdowns (a_cond_var + Add-Logic varSelect) reorganized into
<optgroup>s + placeholder hints (WebPage_Automations.h). Help text
(System_Utils.cpp) + docs/USERGUIDE.md condition table expanded; also fixed two
stale doc points (>=/<= were undocumented; removed the unsupported AND example).
SKIPPED: MQTT (compiled out by default + latent isMqttConnected link bug).
Two pre-existing issues flagged as separate task chips: the MQTT link bug, and
the dead a_trigger_mode "once" control (unbacked in firmware). NOT committed -
user flashes + validates on HW first.

## 1. How the system works today (verified by direct code reading)

An automation has THREE independent layers. Do not conflate them:

1. **Triggers (`triggers[]` array, max 4)** - WHEN the automation wakes up.
   Types: `time` (at-time, daily/weekly), `interval`, `manual` (afterDelay),
   `boot`. Built by the primary trigger controls + the "Additional Triggers"
   section (`secondary_triggers_section`, WebPage_Automations.h:128-181).
   Backend supports per-trigger scheduling via
   `updateAutomationTriggerNextAt(id, triggerIdx, nextAt)`
   (System_Automation.cpp:563). **KEEP this section - user decision, it is
   NOT redundant** (it adds scheduling sources; Add Logic adds conditional
   actions - different layers).

2. **"Fire when" condition gate (optional, one per automation)** - checked at
   trigger-fire time; gates whether the action list actually runs.
   UI: `a_cond_var` / `a_cond_op` / `a_cond_val` / `a_trigger_mode`
   (WebPage_Automations.h:187-191). Mode: "repeat while true" vs "once on
   false->true crossing" (edge-tracked, re-arms when false).
   Poll cadence = whatever trigger it is paired with (usually an interval).

3. **Add Logic (IF / ELSE IF / ELSE ... THEN action)** - branching INSIDE the
   action list, serialized as `IF <var> <op> <val> THEN <action>` command
   strings, validated by `validateConditionalChain()`
   (System_Automation.cpp:2938).

**KEY ARCHITECTURAL FACT:** layers 2 and 3 are backed by ONE resolver:
`evaluateCondition(const char*)` at **System_Automation.cpp:2645**. Call sites
wrap the stored gate fields into `IF <var> <op> <val> THEN x` strings (lines
~801, ~1694, ~2146, ~3673) and Add Logic chains flow through
`evaluateConditionalChain()` -> `evaluateCondition()`. So **adding one
`else if` branch to the resolver lights the new variable up in BOTH the
fire-when gate AND Add Logic** - only the two UI dropdowns need matching
options. No JSON schema change needed (var name is stored as a string).

### The resolver seam (System_Automation.cpp:2740-2896)

Single `else if (strcmp(sensor, "X") == 0)` chain. Parser (lines 2689-2733):
splits `<sensor> <op> <value>`, operators searched longest-first:
`CONTAINS >= <= != > < =`. Everything uppercased before compare.

Two value paths:
- **Numeric**: set `currentValue` (float); ops `> < = >= <= !=`
  (`=`/`!=` use 0.1 epsilon). Value parsed with `atof` (negative values OK).
- **String/enum**: set `isNumeric=false` + `currentStringValue` (32 bytes,
  UPPERCASE); ops `= != CONTAINS`.

Existing variables:
| Var | Type | Source |
|---|---|---|
| TEMP | numeric | `gThermalCache.thermalAvgTemp` under `SensorCacheGuard` (ENABLE_THERMAL_SENSOR) |
| HUMIDITY | stub | always returns false ("not available") - pattern for graceful absence |
| DISTANCE | numeric special | matches if ANY of up to 4 `gTofCache.tofObjects[]` meets the condition |
| LIGHT | numeric | `gApdsCache.apdsClear` |
| MOTION | enum | `apdsProximity > 50` -> DETECTED / NONE |
| TIME | enum | hour -> MORNING / AFTERNOON / EVENING / NIGHT |
| ROOM / ZONE | string | `gSettings.espnowRoom` / `espnowZone` (NONE if empty) |
| TAGS | string+CONTAINS | `gSettings.espnowTags` |
| (unknown) | - | returns false |

Conventions the new branches MUST follow:
- Read from caches/globals only; take `SensorCacheGuard` with a 50 ms timeout
  where a mutex exists; NEVER block or do I/O in the resolver (it runs from
  the scheduler tick in the main loop).
- If the subsystem is disabled/absent -> `return false` (like TEMP's
  `#if` guard and the HUMIDITY stub).
- Uppercase string values before compare.

### UI touchpoints for a new variable (WebPage_Automations.h)
1. `a_cond_var` select, line ~187 (fire-when gate dropdown; lowercase values).
2. `addLogicField()` varSelect innerHTML, line ~602 (Add Logic dropdown).
3. `updateValuePlaceholder()`, line ~638 (placeholder hints per var).
4. The EDIT/populate path around lines ~1274 and ~1381 reuses `a_cond_var` -
   verify it needs no per-variable code (it shouldn't; it sets `.value`).

### Scheduler model (for cadence expectations)
Poll-based only - no event-driven triggers exist today. Main loop calls
`automationsAnyDue(now)` (fast in-RAM check against `gAutoMemoNextAt[]`,
cap 128) and then `schedulerTickMinute()` streams/evaluates due automations.
Conditions are therefore polled, at the granularity of the paired trigger.

## 2. Proposed new condition variables (the actual deliverable)

Rule: only ship variables whose accessor is verified to exist and is cheap to
read synchronously. Phase 0 verifies each accessor. Naming: single UPPERCASE
word in the resolver, lowercase value in the UI dropdowns (existing style).

### Tier 1 - system state, no extra hardware, near-certain accessors
| Var | Type | Meaning / default units | Accessor (verify exact symbol in Phase 0) | Example |
|---|---|---|---|---|
| BATTERY | numeric | percent (or voltage if no fuel gauge) | board battery monitor (FeatherS3; grep vbat/maxlipo/lc709/MAX17048/battery) | `battery < 20 -> ledcolor red` |
| HEAP | numeric | free internal DRAM, KB | `esp_get_free_heap_size()` / `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` | `heap < 40 -> reboot guard warning` |
| PSRAM | numeric | free PSRAM, KB | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` | `psram < 512 -> broadcast alert` |
| WIFI | enum | CONNECTED / NONE | `WiFi.status() == WL_CONNECTED` (or firmware's own wifi state global) | `wifi != CONNECTED -> ledeffect blink` |
| RSSI | numeric | dBm (negative) | `WiFi.RSSI()` | `rssi < -75 -> print weak wifi` |
| UPTIME | numeric | minutes since boot | `millis()/60000` or `esp_timer_get_time()` | `uptime > 1440 -> status` |
| CHIPTEMP | numeric | SoC temperature C | `temperatureRead()` (arduino-esp32) | `chiptemp > 70 -> broadcast hot` |
| HOUR | numeric | 0-23 local hour | `localtime()` (same as TIME branch) | `hour >= 22 -> oleddim` |
| DAY | enum | MON..SUN | `localtime()->tm_wday` | `day = SAT -> weekend routine` |
| FSFREE | numeric | filesystem free, KB | LittleFS total/used (System_Filesystem.cpp; verify helper) | `fsfree < 100 -> autolog off` |

### Tier 2 - connectivity/presence (accessors exist, wiring to verify)
| Var | Type | Meaning | Accessor lead |
|---|---|---|---|
| PEERS | numeric | ESP-NOW peers seen recently | peer table in System_ESPNow.cpp (lastSeen/heartbeat tracking from mesh work) |
| MQTT | enum | CONNECTED / NONE | System_MQTT.cpp connected flag/client state |
| BLE | enum | CONNECTED / NONE | BLE server connection flag (G2/app link) |
| NTP | enum | SYNCED / NONE | time-validity check the scheduler already relies on (verify how "time is sane" is detected) |

### Tier 3 - feature state (nice-to-have, judge during execution)
| Var | Type | Meaning | Accessor lead |
|---|---|---|---|
| GPS | enum | FIX / NOFIX | GPS module state (GPS tracks feature) |
| SPEED / SATS | numeric | GPS speed / satellite count | same module |
| LLM | enum | READY / BUSY / NONE | `llmIsGenerationDone()` / model-loaded flag (gLLM) |

Deliberately EXCLUDED for now: gamepad/button states (better served by an
event-driven trigger type later), camera state, FM radio state (low value).

## 3. New trigger TYPES (deferred - Phase 5, separate effort)
Everything today is schedule-poll. Genuinely event-driven trigger types would
need hooks in the source subsystems + `notifyAutomationScheduler()` calls:
- on ESP-NOW message/peer-join, on WiFi connect/disconnect, on BLE
  connect/disconnect, on GPS geofence enter/exit, on button/gesture.
Medium-high effort; NOT part of this pass. The condition variables above give
most of the practical value via `interval + fire-when` pairing.

## 4. Execution phases (for Opus)

**Phase 0 - Verify (read-only).**
- Confirm each Tier 1/2 accessor symbol + header (greps listed above).
- Read `schedulerTickMinute()` + the gate call site (~System_Automation.cpp:2146)
  to confirm the wrap format and edge-tracking (once-mode) storage.
- CHECK FOR ALLOWLISTS: grep the add/save paths (`cmd_automation_add`,
  the web save handler, `sanitizeAutomationsJson`, `cmd_validate_conditions`)
  for any validation of the condition VARIABLE NAME that would reject new
  names. If one exists, extend it in Phase 1.
- Check OLED_Mode_Automations.cpp - if it only lists/toggles/runs automations
  (likely), no OLED changes needed.
- Optional: prior workflow run `wf_55dc7ea1-0d0` gathered a capability
  inventory; its journal (if still present) is at
  `~/.claude/projects/-Users-morgan-esp-hardwareone-idf/fa55dfde-3a15-404b-8461-9293aee674e0/subagents/workflows/wf_55dc7ea1-0d0/journal.jsonl`
  - reuse instead of re-sweeping if convenient.

**Phase 1 - Resolver.** Add one `else if` branch per approved variable at the
seam (System_Automation.cpp, after TAGS, before the unknown-sensor fallback),
following the TEMP/MOTION patterns (guarded, cheap, `return false` when
absent). Consider replacing the HUMIDITY stub if a real humidity source
exists; otherwise leave it.

**Phase 2 - Web UI.** Add matching `<option>`s to BOTH dropdowns
(gate line ~187, Add Logic line ~602) + placeholders in
`updateValuePlaceholder()` (battery->'20', rssi->'-75', day->'SAT',
wifi->'CONNECTED', etc.). Verify the edit-form repopulate path needs nothing.

**Phase 3 - Help/docs.** Update automation command help strings
(System_Automation.cpp command table ~line 3802 area / System_Utils.cpp
automation help ~2864) and docs/USERGUIDE.md automation section with the new
variable list. ASCII only in docs.

**Phase 4 - Build + HW test.** Build with the standard env
(`. ~/esp/esp-idf/export.sh && idf.py build`; HW_BOARD as configured).
NO incremental commits - user flashes and validates on hardware first, then
ONE `vX.Y.Z:` commit (likely v0.99.0 - it's a feature). Per standing rules:
no backwards-compat shims needed.

**Phase 5 (deferred).** Event-driven trigger types (section 3) - only if the
user asks after using the new variables.

## 5. Defaults chosen (change only if user objects)
- Units: HEAP/PSRAM/FSFREE in KB; UPTIME in minutes; BATTERY percent;
  RSSI raw dBm.
- Connectivity vars are enums (CONNECTED/NONE) matching the MOTION pattern,
  not numerics.
- HOUR added as numeric alongside the existing TIME enum (finer control
  without breaking existing automations).
- Poll-only semantics retained; no scheduler changes in this pass.
