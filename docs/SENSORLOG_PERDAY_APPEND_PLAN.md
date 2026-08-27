# Per-Day Append for Sensor/Health Capture — Implementation Plan (v2, critique-corrected)

Goal: one calendar day of CSV health capture = ONE continuous, graphable file, built on the
already-implemented `shapeSessionPath()` dated subfolders + `System_TimeAnchors` retro-dating.
Produced by a 4-area investigation + adversarial review (verdict: ready-with-fixes; all four
GAP fixes are folded in below). User decisions taken: keep `sensorLogMaxSize` 256000 default;
midnight rollover leaves ONE `logSystemEvent` breadcrumb (no notification/automation event).

## Core decisions

| Decision | Choice | Grounding |
|---|---|---|
| Row timestamp | Same `timestamp_ms` column, dual-range: **epoch-ms when `Clock::isSynced()`** (13 digits), boot-ms otherwise (≤10) — widened to int64 | Nothing in-repo parses the value: `parseGPSLine` skips field 1, web log viewer treats CSV rows as raw lines, `healthlogmerge` is byte-blind, all health UIs graph from RAM/BLE. `%lld` safe: newlib NANO_FORMAT unset in sdkconfig. |
| Day-file naming | CSV shaped sessions → `sensors/YYYY-MM-DD/<base>-YYYY-MM-DD.csv`; TEXT/TRACK keep per-session timestamped names | Goal is CSV graphs; TEXT rows already wall-clock-prefixed + headerless (append-safe by construction); a GPS TRACK crossing midnight is one journey. |
| Session-path modes | Tri-state derived FROM the accepted path at start: **DAY** (`/sensors/YYYY-MM-DD/…`), **BOOT_SHAPED** (`/sensors/boot-N/…`), **MANUAL** (anything else) | GAP 3 fix: a single day-mode bool can't express boot→synced transitions; manual literal paths must never roll. |
| Midnight rollover | Quiet in-tick roll for DAY sessions (same task as writes — no race); one events.log line | Health sessions are unbounded (autostart), so "next natural boundary" never comes; a full stop/start fires SYSEVT_SENSOR_STARTED into notifications/automations nightly and can silently fail after flipping the run flag. |
| Sync-flip roll (GAP 3) | A BOOT_SHAPED CSV session rolls onto the day file **the moment `Clock::isSynced()` flips**, in the tick, before the first synced row is written. Pre-sync rows stay in the boot file → anchor sweep retro-dates it (it's no longer the active file). Day files stay pure epoch-ms. | Without this, an always-on device that boots dark writes one boot file forever and never produces a day file — the feature would be inert on its main target. |
| Mask change mid-day | Header-compat guard: existing file's first line vs `buildCsvHeader(mask)`; mismatch → `-2..-9` variants; exhaustion → timestamped per-session fallback (capture never refuses to start) | Different masks are different column sets; merging is semantically impossible. |
| Rotation / SD overflow | Mechanism + 256000 default unchanged. Header-on-create at the APPEND site fixes: post-rotation headerless base, mid-session SD-overflow flip, headerless `/sd` mirror. LittleFS→SD split day = two headered files that stitch like rotated siblings. | SD auto-mkdirs subfolders (vfs_api). Overflow latch is one-way per boot. |
| Promote sweep | Keep-separate (dark-boot files promote as their own files, never merged into the day file) | Merge needs per-row timestamp rewriting, breaks rename atomicity, risks out-of-order rows, can't reconcile mask mismatches. `healthlogmerge` stitches when wanted. |
| Heartbeats | Suppress the no-data heartbeat rows **only when format == CSV** (GAP 1 fix — a "TEXT-only" gate would silence TRACK's `SIGNAL_LOST` marker, which is emitted via the heartbeat path when GPS disconnects) | Bare-timestamp CSV rows are grapher junk at 5s cadence; epoch stamps make gaps self-describing. |

## Edit list (dependency order; System_SensorLogging.cpp unless noted)

- **E1** `sensorLogTick` append section: manual `fsLock`/`fsUnlock` (644/727) → RAII `FsLockGuard`. MUST land first — later edits add early exits that would leak the mutex and deadlock all FS access. Helpers inside never call raw `fsUnlock()`.
- **E2** Extract `static String buildCsvHeader(uint8_t mask)` (exact 893-918 logic, no trailing `\n`), hoist TRACK static header to file scope, add `static bool writeHeaderChecked(File&)` that dispatches on `gSensorLogFormat` (CSV→mask header, TRACK→static, TEXT→no-op) and checks the `write()` return (unchecked today).
- **E3** `buildCSVFromSnap`: `int64_t ts = Clock::isSynced() ? Clock::epochMillis() : millis();` printed `%lld`. New `Clock::epochMillis()` (gettimeofday-based) beside the other clock accessors in System_Clock — no duplicated epoch math. The `unsigned long`→int64 widening is mandatory (epoch-ms truncates silently in 32-bit).
- **E4** `shapeSessionPath`: when synced AND `gSensorLogFormat == SENSOR_LOG_CSV` → `<base>-YYYY-MM-DD<ext>` day name (TEXT/TRACK keep full timestamps). No new basename stripper: the existing `-YYYY-` strip already swallows `-N` variant suffixes.
- **E5** New `resolveSessionTarget(String shaped) -> String`: resolve via `VFS::resolveOverflowPath` FIRST (probing the raw path while appends go to `/sd` validates the wrong file); missing-or-`size()==0` = fresh (create + `writeHeaderChecked`; empty file adopted, never burns a variant); else CSV-only first-line compare (1024-byte buffer — worst-case header is ~642 B; rstrip `\r\n `), mismatch → `-2..-9` → timestamped fallback. Used by `cmd_sensorlog start` (replacing the create-if-needed block, keeping the parent-mkdir walk) and by rollover.
- **E6** Append site: after `openGuarded(resolved, "a", …, create)`, if `f.size()==0 && format != TEXT` → `writeHeaderChecked(f)` before the row.
- **E7** Promote `approxSizeBytes` (function-static, 134) to file scope; seed from the resolved file's on-disk size at start and rollover (fixes reboot-append under-count and same-boot restart carry-over). Cross-task write (start=cmd_exec, tick=loop) is benign — worst case one early rotation.
- **E8** Tick rollover, inside the E1 guard, before the append: DAY mode + synced + date≠cached → re-shape from the persisted BASE path, `resolveSessionTarget`, reseed size, update cached day; BOOT_SHAPED + synced + CSV → same roll (sync-flip). One `logSystemEvent("LOG", …)` breadcrumb; NO SYSEVT, no broadcast, no `setSetting`.
- **E9** (GAP 4 form) New `stripSessionShaping(const String&)` — the shaper's dir-component + basename strip steps extracted; `cmd_sensorlog start` persists `stripSessionShaping(filepath)` into `gSettings.sensorLogPath` (no-op for manual literal paths, base-recovery for shaped ones; variants never persist).
- **E10** `sensorlog sensors` while a CSV session runs → `Error:` + cliHint("stop first / use healthtrack"). PLUS (GAP 2): `healthTrackSet(true)` captures `hadR1` BEFORE OR-ing `LOG_R1` into the mask; the "already logging" shortcut only when `hadR1` — otherwise fall through to the restart so the header matches the new columns.
- **E11** Heartbeat block gated: skip when `format == SENSOR_LOG_CSV` (TEXT and TRACK keep heartbeats; TRACK's SIGNAL_LOST depends on them).
- **E12** WebPage_Maps.cpp (~245) + OLED_Mode_Map.cpp (~602-616): track pickers recurse one subfolder level under `/logging_captures/sensors` — they currently skip subdirectories entirely, a shipped regression from dated subfolders (autostart-resumed TRACK logs are invisible today).

## Accepted limits (stated, not hidden)

- millis()-wrap: boot-ms rows + anchor lift math break past 49.7 unsynced days — pre-existing, dark-boot-only.
- A LittleFS→SD overflow day is two files; a >256KB day rotates into `.1/.N` siblings — both stitch by epoch-ms sort.
- Variant probe does ≤9 opens at session start, under lock — bounded, start-only.
- The variant-then-fail ordering (probe before heap/space checks) is pre-existing shape.

## HW-test checklist

1. Synced boot → `sensors/YYYY-MM-DD/health-YYYY-MM-DD.csv`, header once, 13-digit epoch rows.
2. Stop/start + reboot same day → appends, no duplicate header.
3. Mask change mid-day (`healthtrack` off/on with other sensors) → `-2` variant, original intact; `healthtrack on` during a running non-R1 session → restart (not silent live-mask mutation).
4. `sensorlogmaxsize 10240`, run past → `.csv.1` + recreated base HAS a header; rotation fires near 10KB right after reboot (seeding).
5. Fill LittleFS below reserve → `/sd/...` mirror file has a header.
6. Clock 23:59 across midnight → new day file, one events.log line, NO notification/automation.
7. Dark boot → boot file; sync mid-session → session ROLLS to day file at sync; boot file promotes via sweep as separate file.
8. Manual `sensorlog start /some/literal.csv` → untouched literal path, no rollover, path persists as typed.
9. `sensorlog sensors` during CSV run → refused with hint.
10. CSV all-sensors-down → no heartbeat rows; TEXT still writes them; TRACK GPS unplug → SIGNAL_LOST still appears.
11. Power-cut during header write → empty file adopted next start (no `-2` burned).
