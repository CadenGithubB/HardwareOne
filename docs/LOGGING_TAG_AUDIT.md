# Logging Tag & Logging-Page Audit

**Date:** 2026-07-19/20 · **Scope:** how log lines acquire subsystem tags, how the /logging web
page parses/filters them, and where the taxonomy or the page needs new tags/features.
**Method:** direct code verification (every claim below cites the line it was read from).
Investigation only — no code changed.

---

## 1. How the tag system actually works (verified pipeline)

A log line can acquire a subsystem tag in **three independent places**, and the web viewer
reconciles them heuristically:

1. **Macro-embedded text tags** — `ERROR_*F` / `WARN_*F` / `INFO_*F` macros bake a literal
   `[LEVEL][TAG]` prefix into the message text (System_Debug.h:756-891). Plain `DEBUG_*F`
   macros embed **no** tag; whatever the call site writes is the text.
2. **Writer-side category tag** — when `log start … tags=1`, the debug-output task prefixes
   `[<name>]` from `getDebugCategoryName(msg->category)` (System_Debug.cpp:238-240). Serial,
   web mirror, BLE, and OLED sinks **never** get this tag — file only (System_Debug.cpp:215-224).
3. **Ad-hoc inline tags** — `BROADCAST_PRINTF_CAT` and raw `[TAG]`-prefixed strings
   (`[NOTIF]`, `[EVLOG]`, `[HEAP_MONITOR]`, `[AutoStart]`, …), category = 0 (no flag).

The viewer (WebPage_Logging.h:1225-1358) tries, in order: Format 1 `[digits] [TAG] msg`
(then rewrites TAG from an inner `[LEVEL][TAG2]`), Format 2 `[digits] user@source cmd -> result`,
Format 3 `[digits] msg` with inline-tag salvage, else an **UNKNOWN** raw gray row.
Tag regex is `^\[([A-Z][A-Z0-9_-]*)\]` — **first char must be uppercase A-Z** — and hyphens
normalize to `_` for coloring. Filters: single-select category, exact-match level, substring
search. No sorting (lines stay in file order), no time filter, no live tail.

Key routing facts:

- `log start flags=0x…` **overwrites the global `gDebugFlags`** (System_Debug.cpp:2446), and
  `log start` *without* `flags=` silently restores the last-used mask over whatever the user
  configured via per-flag settings (System_Debug.cpp:2456-2461). The "log file filter" is
  really the device-wide emission filter for serial/web/BLE too.
- ERROR/WARN macros pass **`0xFFFFFFFF` as the category mask** (bits 0-31 all set) to mean
  "always". Consequences in §2.3.
- `[ERROR]`-prefixed text is always teed to `errors.log`, `[EVENT]` to `system-events.log`,
  `[EVLOG]` to the event-stream file — matched by **text prefix**, independent of routing
  (System_Debug.cpp:266-306).

Timestamp bases differ per stream (relevant to any sort feature):

| Stream | Prefix | Viewer parse |
|---|---|---|
| system log (`log start`) | `[millis]` | ✅ Formats 1/3 |
| command-audit.log | `[seconds-since-boot]` (System_Utils.cpp:904,934) | ✅ Format 2 (unit unlabeled, 1000× off from the above) |
| errors.log / system-events.log / event-stream / i2c_errors.log | `[YYYY-MM-DD HH:MM:SS.mmm] \| ` or `[ms=N]` fallback (System_Utils.cpp:755-777, System_Debug.cpp:3316-3325) | ❌ **no format matches** |

---

## 2. Verified defects (each read directly from current code)

### 2.1 HIGH — The viewer cannot parse the four always-on admin logs
`buildTimestampPrefix()` emits a wall-clock prefix; every viewer regex requires `^\[(\d+)\]`.
So **errors.log, system-events.log, the [EVLOG] event stream, and i2c_errors.log render
entirely as UNKNOWN gray raw rows** — no category filter, no level filter, no colors — despite
being the most valuable admin logs and directly selectable in the page's file explorer
(`/system/sys_logs`). The `[ms=N]` no-NTP fallback fails the regex too.
*Fix:* add a "Format 0" that strips `[wall-clock] | ` / `[ms=N]` and then reuses the existing
`[LEVEL][TAG]` salvage. Small JS-only change; makes four log families first-class.

### 2.2 HIGH — The SR debug bank is inert; SR logs are gated on the MICROPHONE flag
`DEBUG_SR`/`DEBUG_SR_*` (bits 88-93) have **zero emitters**. System_ESPSR.cpp:46-47 defines
`DEBUG_SRF` as `DEBUG_MICF("[ESP_SR] " …)` — SR lines are gated on **DEBUG_MICROPHONE**, tagged
`[MIC]` by the writer with `[ESP_SR]` inline, and the legacy `gSrDebugLevel` still gates via
`SR_DBG_L` (System_ESPSR.cpp:283). Toggling any SR flag does nothing; toggling MIC drags in SR
noise. *Fix:* point `DEBUG_SRF` at `DEBUG_SR` (+sub-flag variants), add SR names to
`getDebugCategoryName`, or delete the bank.

### 2.3 MEDIUM — ERROR/WARN lines get a spurious `[AUTH]` writer tag in the system log
Category `0xFFFFFFFF` sets bits 0-31, and `getDebugCategoryName` returns the **first** match —
bit 0 = `DEBUG_AUTH` (System_Debug.cpp:2114). With `tags=1`, every ERROR/WARN line in the
system log file reads `[ts] [AUTH] [ERROR][THERMAL] …`. The viewer recovers via the inner-tag
rewrite, but raw reads/grep/serial-side copies of the file mislead.
*Fix:* treat the always-mask specially (skip the writer tag, or derive it from the inline tag).

### 2.4 MEDIUM — ~40 writer-side sub-category names are unreachable
Sub-flag macros enqueue `PARENT | SUB` (System_Debug.h:634-701), and `getDebugCategoryName`
checks the **parent first** for every family except G2 (which deliberately checks subs first,
System_Debug.cpp:2222-2230). So `CAMERA_LIFECYCLE`, `MQTT_CONN`, `MEMORY_HEAP`, `I2C_BUS`,
`THERMAL_POLL`, `GPS_LIFE`, … (System_Debug.cpp:2121-2203) can never be returned: DEBUG-level
sub lines all collapse to the parent tag in the file. (INFO-level sub lines survive only
because their macros embed `[INFO][GPS_LIFE]`-style inline tags.)
*Fix:* order sub-checks before parents, G2-style — the names are already written.

### 2.5 MEDIUM — The Bluetooth debug bank has zero emitters
`DEBUG_BLUETOOTH`/`_CORE`/`_GATT`/`_DATA` (bits 64-67) appear only in settings plumbing;
no `DEBUGF_QUEUE` gates on them, and `getDebugCategoryName` has no entry (falls to
`"UNKNOWN"`). The only BT tag is always-on `[WARN][BT]` (System_Debug.h:809). For a subsystem
the size of Bluetooth.cpp/BLE_Peers.cpp this is the biggest **missing tag family** in the
system. *Fix:* wire `DEBUG_BT*F` macros + writer names, or drop the bank.

### 2.6 MEDIUM — Split tag vocabulary: the same subsystem filters under 2-3 names
Writer-side names vs macro-embedded names diverge: `USERS`/`USER`, `SYSTEM`/`SYS`,
`MEMORY`/`MEM`, `LOGGER`/`LOG`, `HTTP`/`WEB`, `CLI`/`CMD`/`CMD_SYS`/`CMD_FLOW`,
`ESP-NOW`/`ESPNOW_*`. The color map aliases them to shared colors, but the **filter dropdown
treats each as a separate category** — filtering `WEB` misses `HTTP` lines and vice versa.
*Fix:* one canonical name per subsystem (see §4.1).

### 2.7 LOW — Mixed-case inline tags are invisible to the parser
`[AutoStart]` ×30, `[resolve]` ×15, `[Discovery]` ×3 fail the uppercase-first regex and land
as GENERAL. They even have proper flags (`DEBUG_I2C_AUTOSTART`, `DEBUG_NTP`,
`DEBUG_I2C_DISCOVERY`). *Fix:* retag as `[I2C_AUTOSTART]`, `[NTP_RESOLVE]`, `[I2C_DISCOVERY]`.

### 2.8 LOW — Ghost and stale entries
- `[SECURITY]` is special-cased in the help-suppression gate (System_Debug.cpp:176,797) and has
  a color key, but **nothing emits it**.
- Color-map keys with no remaining emitters: `SENSORS`, `SENSORS_FRAME`, `SENSORS_DATA`
  (umbrella removed), `ESPNOW_ENCRYPTION` (flag removed), `GAMEPAD` (renamed INPUT — and
  `INPUT` itself has **no** color key).
- Flags-pane checkbox `flag-espnow-enc` sets freed bit 37 → dead control
  (WebPage_Logging.h:390 vs System_Debug.h:168).
- `getDebugCategoryName` carries pre-renumber comments ("Bits 32-39", "Bit 63", "Bits 64-69")
  that no longer match the 256-bit map.

### 2.9 `[EVENT][BOOT]` category collapse
Format-3 salvage takes the **first** bracket group, so every lifecycle event files under
category `EVENT` and the actual subsystem (25 distinct categories: SENSOR, SETTINGS, ESPNOW,
USERS, FS, WIFI, SETUP, LOG, HTTP, CRYPTO, SR, MESH, I2C, CAM, BOOT, SD, MQTT, G2, DISPLAY,
AUTO, TIME, TASK, REBOOT, LLM, BOND) is lost to filtering — and `EVENT` has no color key.

### 2.10 Flags pane coverage: ~44 valid checkboxes vs ~106 emitting flags
Present: word-0 singles, ESP-NOW 5, automations 5, memory/camera/mic/fmradio/i2c **parents
only**, 8 sensor parents, 8 sensor detail subs (labels still say "Frame/Data" though the map
renamed to POLLING/VALUES). **Missing entirely:** HTTPS(3), NTP(14), DISPLAY(15), memory subs
(25-27), ESPNOW_METADATA(38), the whole MQTT bank (48-52), the whole G2 bank (72-78), the whole
LLM bank (96-101), the whole Maps bank (104-107), camera subs (113-116), I2C subs (121-123),
ANO bank (208-211), and most per-sensor LIFECYCLE/VALUES subs — **~62 emitting flags cannot be
selected from the page.** (BT/SR banks excluded — orphaned, see §2.2/2.5.)
Note the mask JS is **correct** (BigInt, WebPage_Logging.h:1078-1082) — coverage, not math,
is the problem.

### 2.11 Missing color keys for real, frequent tags
No color (render gray): `I2C`, `I2C_BUS/DISCOVERY/AUTOSTART`, `MQTT` + subs, `NTP`, `DISPLAY`,
`NOTIF`, `MAPS*`, `LLM*`, `INPUT`, `ANO*`, `EVENT`, `EVLOG`, `ESP_SR`, `VIDEO`, all G2 sub-tags
(`G2_LIFE`…), all sensor sub-tags (`GPS_LIFE`…), and the command-audit source categories
(`SERIAL`, `ESPNOW`, `INTERNAL`, `DISPLAY`, `BLUETOOTH`, `MQTT`, `VOICE`) — only `WEB` and `G2`
of the audit sources have colors.

---

## 3. What already works well (don't break)

- The G2 family is the reference implementation end-to-end: parent-OR-sub gating, sub-first
  writer names, parent as master switch (System_Debug.h:646-657, System_Debug.cpp:2222-2230).
- BigInt mask assembly; 256-bit `flags=` parsing with colon-word syntax (System_Debug.cpp:2263+).
- Error-file dedupe window; `[EVENT]`/`[EVLOG]` always-on tees; audit-line redaction +
  quiet-poll suppression; bonded-peer log viewer reusing the same parser/filters.
- The viewer's Format-1 inner-tag rewrite means most *system-log* content classifies correctly
  today — the gaps are the *other* files (§2.1) and granularity (§2.4).

## 4. Recommendations (ranked)

### Taxonomy
1. **Single source of truth for the tag vocabulary.** One X-macro table (the SYSEVT fusion
   pattern already proven in System_Events) generating: flag constant, canonical tag string,
   `getDebugCategoryName` entries (sub-before-parent), settings name, web checkbox list, and
   the color map. Kills §2.3/2.4/2.6/2.8/2.10-class drift permanently.
2. **Wire the two dead banks** (BT first — biggest untagged subsystem; SR second) or delete them.
3. **New tag families worth adding** (subsystems with real log traffic but no lane):
   `BOND` (bond/pairing verticals — only a logSystemEvent category today), `POWER`
   (power-save/CPU-freq transitions — the recent crash-loop work debugged blind), and a
   proper `BT` family per above. G2/LLM/Maps/NOTIF already have flags — they need *exposure*
   (checkboxes/colors), not new tags.
4. Uppercase the mixed-case inline tags (§2.7); retire ghosts (§2.8).

### Viewer/parser (small JS, high payoff)
5. **Format 0 for wall-clock prefixes** (§2.1) — unlocks errors/events/evlog/i2c logs.
6. Treat `[EVENT][X]` / `[EVLOG]` specially: level-style badge + real subsystem category (§2.9).
7. **Deterministic hash-color fallback** so unknown tags are never gray; keep the curated map
   for the big families (fixes §2.11 for free, future-proof).
8. Normalize timestamps for display: viewer knows millis vs seconds vs wall-clock per format;
   boot-anchor lines + NTP anchors make millis→wall-clock conversion possible. Label units.

### Page features
9. **Flags pane regeneration**: family accordions matching the byte-bank map, parent/sub
   indentation, per-family select-all — covering all emitting flags (§2.10); remove bit-37 box.
10. **Multi-select category filter with per-category counts** (chips), alias-aware grouping.
11. **Level threshold filter** (≥WARN) instead of exact match.
12. **Live tail** of the active system log (poll `/api/files/view` with offset or SSE lane).
13. Sort controls (timestamp/category/level) — currently file-order only; plus export-filtered
    and filter-state persistence.
14. Surface `log start`'s global side effect in the UI (§1): either label it honestly
    ("sets device-wide debug flags") or add a file-only filter lane.

## 5. Caveats
- Emitter counts (~106 emitting flags, ~62 missing checkboxes) are from static grep sweeps;
  a handful of flags may emit via indirect call sites not matched by the patterns used.
- The multi-agent verification pass for this audit died on a session usage limit; every
  finding above was instead verified by direct file reads (file:line cited inline).
