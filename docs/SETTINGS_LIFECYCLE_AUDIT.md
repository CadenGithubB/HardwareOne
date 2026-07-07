# Settings.json Lifecycle Audit — Report & Plan

**Date:** 2026-07-02 (plan-only; no code changes made)
**Scope:** `/system/settings.json` — load, save, boot lifecycle, every writer/deleter in the tree, and the system-logging gap that made the incident hard to diagnose.
**Method:** full-code audit; every risk claim below was independently re-traced and adversarially challenged before being accepted. Claims that didn't survive are listed in §6.

---

## 0. TL;DR — the three questions answered

1. **"It said it was deleting settings.json but the file is still there."** Solved, and it's harmless-but-embarrassing legacy code: the boot-time corrupt-file check at [System_Filesystem.cpp:156](../components/hardwareone/System_Filesystem.cpp) still lists the **pre-reorg root path `/settings.json`**, while the real file has lived at `/system/settings.json` since the IDF migration ([HardwareOne.cpp:236](../components/hardwareone/HardwareOne.cpp)). The message `"%s appears corrupt (not valid JSON), removing"` (line 163) deleted a stale fossil file at the root — the real file was never touched. (The other boot deleter, the orphaned-`.tmp` sweep at line 141, can also print a settings-flavored deletion: an interrupted save leaves `/settings.tmp`, which the next boot removes while the previous good settings.json survives untouched.)

2. **"Will it overwrite itself for any invalid reason?"** Yes — two confirmed high-severity paths, both at boot:
   - a **corrupt/unparseable settings.json is silently replaced with pure defaults** on the next boot (§4.1), and
   - a **single failed LittleFS mount formats the entire data partition** (§4.2).
   Neither has a backup, quarantine, or confirmation step. Several medium/low issues follow in §4.

3. **"Is a true system log useful? Is it partially built?"** Useful, cheap, and ~80% of the plumbing already exists. The always-on `[ERROR]` → `errors.log` tee inside the debug output task is exactly the right pattern; a system-event log is that tee widened to an `[EVENT]` class plus ~10 call sites (§7). Notably: **every message in this incident (`removing`, format, defaults-written) currently goes only to serial and is lost forever** — which is why we're reconstructing it from code instead of reading a log.

---

## 1. How the lifecycle actually works today

**Boot order** ([HardwareOne.cpp](../components/hardwareone/HardwareOne.cpp)):
1. `initFilesystem()` (:1192) — mount LittleFS (format-on-fail, §4.2), create dirs, sweep orphaned `*.tmp` files ([System_Filesystem.cpp:119-151](../components/hardwareone/System_Filesystem.cpp)), delete "corrupt" critical JSON files (:153-168, §4.3).
2. `settingsDefaults()` (:1218) — registry defaults applied to the in-RAM `gSettings` struct.
3. `readSettingsJson()` (:1227) — only if the file exists; **parse failure returns false with no recovery branch** — device silently runs on defaults.
4. If the file doesn't exist: `"[Settings] No file found, writing defaults"` + `writeSettingsJson()` (:1231-1232) — the only legitimate boot-time settings write.
5. `setSetting(crashCount/lastResetReason)` (:1242-1243) — persists RTC crash counters; change-gated, but this is the trigger that turns a failed load into a defaults-overwrite (§4.1).

**Save chokepoint** — `writeSettingsJson()` ([System_Settings.cpp:1011-1112](../components/hardwareone/System_Settings.cpp)):
- Merge-reads the existing on-disk file first to preserve "orphaned" sections (keys the current build's registry doesn't own) — a backwards-compat mechanism (§5).
- Rebuilds every registered key from `gSettings` (`buildSettingsJsonDoc`), stamps `firmwareVersion` (:911).
- Writes to `/settings.tmp` **at the root** (:1056), then renames onto `/system/settings.json` (:1084) — atomic on LittleFS.
- On rename failure: falls back to a **truncating in-place rewrite of the live file** (:1087-1101).
- `fsLock` is taken per-phase (merge-read / tmp-write / rename), not across the whole operation; serialization of `gSettings` happens **outside** any lock.

**What triggers a save** (writer inventory):
- ~394 `setSetting()` call sites — change-gated, one full-file write per changed value unless `gDeferWrites` batching is armed (`beginwrite`/`savesettings`).
- Direct `writeSettingsJson()` callers that **bypass** `gDeferWrites`: boot defaults, WiFi save, I2C, camera, feature registry, wizards.
- **Every successful WiFi connect** — unconditional `lastConnected = millis()` + full rewrite ([System_WiFi.cpp:822-823](../components/hardwareone/System_WiFi.cpp)). This is why a connected device's version stamp tracks its running firmware: the `lastConnected: 3384` in your file means the last save landed ~3.4 s after boot, at WiFi connect.
- Cross-task writers besides `cmd_exec`: G2 tap worker, OLED input path, boot/main task, MQTT and voice-command tasks (§4.5).
- ESP-NOW remote CLI (funnels through cmd_exec — serialized, but it's a settings write source, including remote `beginwrite`).
- FTS migration-restore (raw `writeText`, only during the physically-selected first-time-setup restore window).

**Version handling — there is none (by design, and that's fine):** `firmwareVersion` is stamped only on save and only ever *logged* on load (:1168-1175). **No version-mismatch reset/delete/migration logic exists anywhere.** Your file saying `0.96.2` simply means the last save ran under 0.96.2 firmware — expected, benign, and per the no-backwards-compat policy the right amount of version logic.

Also: `network.espnow.firstTimeSetup: true` means **setup COMPLETED** — the field's name is inverted relative to its meaning ("True if ESP-NOW setup has been completed", [System_Settings.h:663](../components/hardwareone/System_Settings.h)), and nothing ever reads it. Pure confusion generator (§5).

---

## 2. The "deleting settings.json" incident — full reconstruction

What the boot log printed was one of these two, both from `initFilesystem()`:

| Message | Source | What it actually deleted |
|---|---|---|
| `W FS: /settings.json appears corrupt (not valid JSON), removing` | System_Filesystem.cpp:163 | A stale **root-level** `/settings.json` fossil (legacy path). Real file untouched. |
| `I FS: Cleaned orphaned temp file: /settings.tmp` | System_Filesystem.cpp:141 | The staging temp from an interrupted save. Previous good settings.json intact. |

Corroborating evidence that the real file was never deleted or rewritten: the surviving `firmwareVersion: 0.96.2` stamp (only updated on save), all user data intact, and boot never rewrites an *existing* file (parse failure just returns false).

Two genuine problems this exposes, though:
- The corrupt-check **deletes instead of quarantining**, on a naive first-character test (no JSON parse). For its two *real* entries (`/system/automations.json`, `/system/users/users.json`) that's a delete-your-data-on-suspicion policy. Deleting users.json re-arms the setup wizard. (The doomsday chain via truncated writes was **refuted** — LittleFS copy-on-write means torn writes never commit — but flash corruption or a stray write can still trip it, and delete-with-no-backup is the wrong response.)
- These messages exist **only on serial**. Nothing durable records that boot deleted a file — the direct motivation for §7.

---

## 3. Severity summary — confirmed invalid-overwrite paths

| # | Finding | Severity | Where |
|---|---|---|---|
| 4.1 | Corrupt settings.json → silently replaced with defaults next boot | **HIGH** | System_Settings.cpp:1152-1159 + HardwareOne.cpp:1242-1243 + System_Settings.cpp:1032-1037 |
| 4.2 | One failed LittleFS mount → format entire data partition | **HIGH** | System_Filesystem.cpp:54-68 |
| 4.3 | Boot corrupt-check deletes (not quarantines); settings entry is a dead legacy path | MED | System_Filesystem.cpp:153-168 |
| 4.4 | Save failures invisible: no caller checks `writeSettingsJson()`; success reported on failure | MED | ~20 call sites; System_Settings.cpp:2205-2243, 2512-2517 |
| 4.5 | No RAM-layer lock: `gSettings` Strings mutated cross-task while another task serializes them | MED | System_Settings.h:1011-1033; System_Settings.cpp:1043 |
| 4.6 | `gDeferWrites` can be stranded true → all changes RAM-only until next `savesettings`/reboot | MED | System_Settings.cpp:104, 2506-2517; WebServer_Server.cpp:4779; WebPage_Bond.cpp:1507-1523 |
| 4.7 | Rename-fail fallback truncates the live file in place; returns true even on failed serialize | LOW-MED | System_Settings.cpp:1087-1101 |
| 4.8 | Every WiFi connect rewrites the whole file to store a boot-relative `millis()` | LOW | System_WiFi.cpp:822-823 |
| 4.9 | Emptied WiFi network list resurrects from disk (deleted SSID+password come back) | LOW | System_Settings.cpp:988 + merge-read :1024-1040 |
| 4.10 | Runtime-only values leak to disk on the next unrelated save (`setMeshRole`, power-mode brightness) | LOW | System_ESPNow.cpp:7347-7352; System_Power.cpp:87-90 |
| 4.11 | Merge-read parse-fail/OOM silently discards all orphaned keys | LOW | System_Settings.cpp:1030-1037 |

Environmental findings (not overwrite bugs, but part of the risk picture): settings share the single 2796K LittleFS partition with all log churn (a full FS makes settings permanently unsaveable until space is freed); IDF persists the last-used WiFi credentials **in cleartext in NVS** because nothing calls `esp_wifi_set_storage(WIFI_STORAGE_RAM)` — outside settings.json's AES-at-rest scheme and surviving both `factoryreset` and a LittleFS format; `factoryreset` deletes only users.json (no command exists to reset settings.json to defaults — the only "resets" are the accidental ones above); users.json is truncate-rewritten (`"w"`, non-atomic) every boot to bump `bootCounter`.

---

## 4. Confirmed findings in detail

### 4.1 HIGH — Corrupt file → deterministic defaults-overwrite at next boot
If `/system/settings.json` is unparseable, `readSettingsJson()` returns false (System_Settings.cpp:1152-1158) and boot has **no failure branch** — `gSettings` silently stays all-defaults. Then HardwareOne.cpp:1243 runs `setSetting(gSettings.lastResetReason, rtcLastResetReason)`: the default is 0 and `esp_reset_reason()` is ≥1 on every real boot, so the guard "only write when changed" **always fires after a failed load**. Inside `writeSettingsJson()`, the merge-read of the same corrupt file also fails → `doc.clear()` (:1034) → pure defaults are serialized and atomically renamed over the user's file. WiFi networks, mesh passphrases, identity, BLE peers: gone. No backup, no quarantine, no log entry that survives reboot. One corrupt byte = full config loss, and the mechanism that does it is the crash counter, not the user.

Sharpening from a second verification pass: the corrupt-file case is only the worst variant. Even a **transient** read failure on an *intact* file (PSRAM allocation failure, flash read hiccup) ends the same way for registered settings — the write that follows serializes RAM defaults over every registered section, because the merge-read only preserves *unregistered* keys and the skip-if-empty arrays (WiFi networks survive; module configs don't). The failed load sets no flag anywhere, so nothing downstream can tell "loaded" from "defaulted".

### 4.2 HIGH — Format-on-mount-fail wipes the partition on any single failure
`LittleFS.begin()` returns a bare bool; the code cannot distinguish genuine corruption from `ESP_ERR_NO_MEM` (transient boot-time heap pressure) or config errors — all fall into the same branch that immediately calls `LittleFS.format()` (System_Filesystem.cpp:55-61). No retry-before-format, no marker requiring N consecutive failures, no user confirmation. Blast radius is the entire data partition (settings, users, automations, logs, photos, maps, games), followed by a fresh defaults write. The only evidence is one serial line printed before any logging exists.

### 4.3 MED — Boot corrupt-check: wrong path, delete-not-quarantine, one-byte heuristic
See §2. The `/settings.json` entry is dead legacy code (can only ever delete a pre-reorg fossil — which is exactly what you watched it do). The two real entries delete a whole config/user database because of one suspicious leading byte, with no `.corrupt`/`.bad` rename for recovery.

### 4.4 MED — Save failures are invisible (violates the OK:/Error: contract)
`writeSettingsJson()` has five distinct return-false paths; **every** call site ignores the result. `handleSettingCommand` returns "[Settings] Configuration updated" and `cmd_savesettings` returns "Settings saved" unconditionally — web/CLI/BLE/OLED/G2 all report success on a failed save; the RAM value works until reboot, then the change evaporates. Reachable on real hardware via a full filesystem (this device stores 343KB ESP-NOW transfers, photos, games on the same partition). Bonus: the non-atomic fallback ignores `serializeJson`'s result and returns **true** on a failed truncating rewrite of the live file.

### 4.5 MED — No `gSettings` mutex; serialization happens outside `fsLock`
`fsLock` guards the three file phases separately; `buildSettingsJsonDoc` walks heap-backed `String` fields with no lock at all. Confirmed concurrent writers besides `cmd_exec` (core 0): the **unpinned** `g2_tap_disp` worker (G2 hijack handlers call `setSetting` directly), the OLED input path on loopTask, boot-time writes, MQTT/voice-command tasks. A `String` reassignment on one core while another core's `serializeJson` reads the same String is a free-while-reading heap race → corrupt file *contents* (with a perfectly atomic rename) or a crash. The shared `/settings.tmp` path race between per-phase lock windows is the mild sibling of this bug.

### 4.6 MED — `gDeferWrites` can be stranded
A single global armed by `beginwrite`, disarmed only by `savesettings`, no timeout, no scope guard. Confirmed strand paths: `handleCliBatch` breaks out mid-batch if a command stops the HTTP server (WebServer_Server.cpp:4779) — the trailing `savesettings` never runs; the bonded-settings page sends `beginwrite`/`savesettings` to the **remote worker** as fire-and-forget ESP-NOW commands — one lost frame leaves the worker deferring every write while the master UI reports success. Self-heals on the next successful `savesettings` and many paths bypass the flag, so severity is medium — but while stranded, every change is RAM-only and lost at reboot with zero indication.

### 4.7 LOW — Non-atomic fallback rewrite (truncation refuted; two real residues)
The scary version of this — power loss during the fallback's truncating rewrite leaves an empty/partial file — is **refuted** on this stack: the vendored littlefs stages `open("w")` truncation in RAM only, commits the directory entry atomically at sync/close, and refuses to commit files whose writes errored (`LFS_F_ERRED`), so the old complete file survives any interruption. What's actually wrong here is smaller but real: (a) the primary path's success check is `bytesWritten == 0` (:1073) — a *partial* serialize (short write under FS pressure) that produces ≥1 byte would be committed and atomically renamed over the good file; checking `bytesWritten == measureJson(doc)` closes it; (b) the fallback ignores `serializeJson`'s result and returns **true**, reporting "saved" while the old file remains on disk (§4.4). Fail loudly instead of falling back.

### 4.8-4.11 LOW — hygiene
- **WiFi connect churn:** `lastConnected = millis()` is boot-relative (meaningless after reboot), write-only, and costs a full-file rewrite on every (re)connect — pure wear plus a wider window for 4.5/4.7.
- **Empty-list resurrect:** `buildSettingsJsonDoc` skips the networks array when count==0 (:988), and the merge-read faithfully copies the old array back. `wifirm` of the last network brings the SSID + encrypted password back on the next save/boot. Same skip-if-empty pattern guards meshes/BLE peers.
- **Runtime leak-through:** `setMeshRole()` documents "does not persist" but writes the registered `gSettings.meshRole` field — the next unrelated save persists a failover-promoted role. `applyPowerMode()` persists the mode's display brightness via `setSetting(oledBrightness)`, clobbering the user's configured value.
- **Merge-read failure:** any parse/alloc failure during the save-time merge silently drops every unregistered key (`doc.clear()`), while the save still reports success.

---

## 5. Modernization inventory (it is indeed old code)

Ranked by value; items marked **[policy]** are backwards-compat code that the no-backcompat rule says to delete outright.

1. **[policy] Legacy `/settings.json` entry in `criticalFiles`** (System_Filesystem.cpp:156) — the incident's source. Delete the entry (or replace the whole block's delete with quarantine for the two real files).
2. **[policy] The save-time merge-read "orphaned sections" mechanism** (System_Settings.cpp:1024-1043) — exists so files written by builds with different `ENABLE_*` flags survive. Costs a full read+parse before every write, resurrects deleted data (4.9), and lets stale sections live forever. Delete → save becomes pure `gSettings` serialization. *Prerequisite:* the skip-if-empty guards for wifi networks / meshes / BLE peers must become always-write (empty array allowed), or those sections would vanish.
3. **[policy] Legacy single-mesh bond scalars** `bondModeEnabled`/`bondRole`/`bondPeerMac` beside the per-mesh arrays (System_Settings.h:713-724, "still read by most callers as of Phase 2.1") — finish the migration and delete.
4. **[policy] Deprecated flat `wifiSSID`/`wifiPassword`** struct fields (System_Settings.h:401-402); **[policy]** WebPage_Settings.h:758 flat-key compat map ("older flat settings.json that lingers").
5. **`espnowFirstTimeSetup`** — inverted name, write-only, read by nothing. Delete (or rename `espnowSetupComplete` if kept).
6. **ArduinoJson 6 fossils** — "5120 bytes" capacity comments and the `NoMemory` "document too small" message (System_Settings.cpp:1021, 1050, 1146-1147, 1155); `PSRAM_JSON_DOC` has been an elastic AJ7 document for a long time.
7. **Crash counters out of settings.json** — persisting `crashCount`/`lastResetReason` in NVS (or a tiny separate file) removes the boot-time write entirely and **defuses 4.1's trigger**.
8. **Tmp-file convention** — `/settings.tmp` at root → `/system/settings.json.tmp` (every other atomic writer uses `<dest>.tmp`; also shrinks the boot sweep's reason to scan `/`).
9. **Split JSON homes** — espnow scalars under `network.espnow`, meshes/bonds under root `espnow`, BLE peers special-cased under `bluetooth.peers` outside the registry. One convention would delete the special-case serializers.
10. **`wifiPrimarySSID` injected by the persistence builder then stripped by the writer** (System_Settings.cpp:917-925 vs :1046) — inverted responsibility; the web layer should add it.
11. **Dead comments** — "saveUnifiedSettings() removed … from .ino instead" (System_Utils.cpp:845, System_Utils.h:174 — no .ino exists), "TEMP DEBUG (2026-04-03)" (HardwareOne.cpp:1235).
12. **Manual `pollPause()/pollResume()` on every exit path** in read/write (8 hand-maintained resume calls) while `PollPauseGuard` exists and is used in System_Utils.cpp:795.
13. **`readSettingsJson` bare-false for absent/open-fail/corrupt** — forcing the caller to pre-check `exists()`; a tri-state result enables the quarantine fix.
14. **users.json bootCounter** — truncate-rewrite every boot; per-user settings saves already use the correct per-file tmp + whole-operation `FsLockGuard` pattern (System_Settings.cpp:2556-2591) — the main writer should copy its own sibling.
15. **Migration restore uses non-atomic `writeText`** (WebServer_MigrationTool.cpp:550) while `writeTextAtomic` sits unused beside it. (Note: the migration tool is device-to-device backup/restore, *not* a version-migration shim — it stays; just fix the write.)

---

## 6. Claims that did NOT survive verification (for honesty)

- **"users.json corrupt-check delete → unauthenticated device on the network"** — refuted. LittleFS copy-on-write means torn writes never commit a garbage first byte; empty/short reads are skipped by the guards; and the re-armed wizard blocks on *physical* serial/OLED input before WiFi or the web server ever start. The remaining nit is delete-vs-quarantine (§4.3).
- **"FTS restore accepts unauthenticated settings replacement"** — real code, but triple-gated: the restore server only exists after the owner physically selects Import-from-Backup during first-time setup, re-checks state per request, and disarms after one restore. `registerMigrationRestoreHandler` (the main-webserver variant) has **zero callers** — dead code. Residual: a LAN attacker could race the owner during that brief window (`/api/ping` discloses the fingerprint that passes the compatibility gate). Low, consistent with the single-owner threat model; the dead registration function should be deleted.
- **"Fallback truncate = corruption on power loss"** — refuted outright on re-verification: littlefs's copy-on-write commit plus its erred-file guard (`LFS_F_ERRED` blocks sync of failed writes) mean there is no window where settings.json is empty or partial on disk. The residual issues are the weak `bytesWritten == 0` success check and success-on-failure reporting (§4.7).
- **"Boot usually rewrites settings.json"** — wrong premise from an early pass; `setSetting` is change-gated, so a healthy clean-booting device rewrites nothing. (The unconditional wifi-connect save is the churn source, not boot.)

---

## 7. System log: what exists, what's missing, and the plan

### 7.1 Already built (more than expected)

| Logger | State | Notes |
|---|---|---|
| **Command audit** → `/system/sys_logs/command-audit.log` | Complete, always-on | One redacted line per command, 500KB cap. User actions only — the WHO-did-WHAT domain. |
| **`log` command** (the `logging.systemlog.*` settings) | Complete but wrong altitude | Tees the entire debug firehose to a file. Off by default, **no size cap**, autostart **truncates** the previous boot's log, millis-only timestamps, starts too late for early boot. It's a serial-capture tool, not a system log. |
| **`[ERROR]` tee → `errors.log`** (System_Debug.cpp:250-279) | Complete, always-on | **The foundation.** Classifies queue lines by prefix, appends durably with 256KB cap + 2s dedupe, independent of debug flags. |
| Login/security logs, i2c_errors.log | Complete | Auth + I2C domains covered. |
| **battery.csv** (System_Battery.cpp:581-676) | Complete | The best *content* template: always-on, wall-clock + boot# columns, an event column for discrete state changes, generation rotation. |
| sensorlog, autolog, loglink (IDF bridge) | Complete | Opt-in domains. |
| `appendLineWithCap` (System_Filesystem.cpp:1610+) | Complete | Battle-tested capped-append + 85% trim + orphan recovery. The storage layer for any new log already exists. |
| Crash counters (RTC → settings.json) | Partial | Two bare numbers; no timeline; core dumps compiled out. |

### 7.2 The gap
No always-on durable sink for **INFO-level firmware decisions**: settings writes/failures/resets, boot lifecycle events (file deleted, defaults written, FS formatted), WiFi lifecycle, crash/boot timeline, firmware-version transitions, mesh role changes. Early-boot events are raw `Serial.println` emitted before the debug system exists — which is precisely why the "deleting settings.json" message can only be reconstructed from source today.

### 7.3 Plan: `/system/sys_logs/system-events.log`
Deliberately **not** a new subsystem — it's the errors.log pattern, one prefix wider:

1. **`logSystemEvent(category, fmt, ...)`** → emits `[EVENT][CAT] text` through the existing debug queue; a second prefix match beside the `[ERROR]` tee in `debugOutputTask` appends it via `appendLineWithCap` (256KB) with wall-clock timestamps. Always-on, independent of debug flags. ~30 lines plus call sites.
2. **Early-boot handling:** before the debug queue exists, `logSystemEvent` falls back to a small static ring; flushed into the queue at `initDebugSystem()`. Events before the FS mounts (mount-fail/format) are writable immediately after the remount succeeds.
3. **First-pass event sites** (low volume — events, not polling):
   - Boot summary: `boot #N | reset=<reason> | crashCount=N | fw vX.Y.Z` (write site already exists at HardwareOne.cpp:1240-1244) — include `firmware changed vA → vB` by comparing the settings stamp at load.
   - FS: mount failed / **formatted**, corrupt-file quarantine/delete, orphan-tmp sweep summary.
   - Settings: defaults written, load failed (quarantined), **save failed** (pairs with fix 4.4), external restore applied.
   - WiFi: connected (SSID, IP), disconnected, AP-fallback entered.
   - FTS/wizard entered & completed; mesh role change; time synced (marker already exists — fold it in).
4. **Surface:** the web logging page already browses `/system/sys_logs/` — zero UI work. Optionally a `syslog [n]` CLI to tail it.
5. **What NOT to do:** don't absorb the command audit (complementary domains, cross-referenced by the same timestamps); don't build it on the `log` module (wrong altitude). Separately, the `log` module needs a cap/rotation and to stop truncating on autostart — or gets renamed to what it is (`debugcapture`).

### 7.4 v2 candidate events (subsystem sweep, 2026-07-02)

v1 shipped the settings/boot/FS core. A full-subsystem sweep found the following uninstrumented lifecycle decisions. All are on-failure-only, per-boot-bounded, or transition-gated — the log stays quiet on a healthy device. The dominant theme is **silent divergence**: the firmware ends up in a state different from what settings or the user asked, with no durable trace.

**Connectivity**
- `[WIFI] connection lost` — **no disconnect handler exists at all** (no `WiFi.onEvent` anywhere); a runtime AP loss today produces zero output and no reconnect attempt (`wifiAutoReconnect` is only consulted once at boot, HardwareOne.cpp:1521). Register a handler at System_WiFi.cpp:1347-1360, edge-gated. This is also a latent *feature* gap, not just a logging one.
- `[WIFI] boot connect failed — continuing offline` (System_WiFi.cpp:1408-1410); fold the driver-dead diagnosis (WL_STOPPED/WL_NO_SHIELD, :839-843) into the reason string.
- `[HTTP] HTTPS requested but fell back to plain HTTP` (WebServer_Server.cpp:4894, 4899) — a silent security downgrade the user configured against.
- `[HTTP] web server FAILED to start` (WebServer_Server.cpp:4927-4929) — the one failure that makes a headless device unreachable is currently not even an `[ERROR]`-prefixed line.
- `[MQTT] connected` / `broker connection lost` (System_MQTT.cpp:783-787, 805-808) — DISCONNECTED **must** be gated on the was-connected flag (esp-mqtt retries every ~10s; ungated = thousands of lines per outage).
- `[G2] temple TX wedged → forced disconnect` (G2_Glasses.cpp:5597-5615); `half-connected recovery gave up after 7 attempts` (:5822-5823).
- `[ESPNOW] init FAILED at boot — mesh unavailable` (System_ESPNow.cpp:8655-8686).
- `[MESH] peer '<name>' offline (heartbeat timeout)` (System_ESPNow.cpp:7795-7802 — the stale-peer sweep is fully silent today); `[BOND] bond peer offline — sync reset` (:7773-7792, edge-gated by bondPeerOnline).
- `[ESPNOW] paired: new peer identity persisted` (System_ESPNow_Handlers_Crypto.cpp:286-291, 351ish); `SECURITY: peer presented DIFFERENT pubkey — refused` (:277-282, :351-357) — a possible impersonation attempt, currently a volatile WARN.
- `[ESPNOW] user '<name>' created via mesh user-sync from '<peer>'` (System_ESPNow.cpp:3470-3476) — account creation the local command audit never sees.
- `[ESPNOW] file received / receive FAILED` (System_ESPNow.cpp:3901-3912).

**Storage / power / system core**
- `[FS] LittleFS free below reserve — log writes latched to SD overflow` (System_VFS.cpp:577-584; latch flag already exists) — early warning for the "full FS makes settings unsaveable" failure mode in §4.4.
- `[SD] write failure — card marked not writable` (System_VFS.cpp:292-298, edge-triggered); `[SD] card formatted (ALL data erased)` (System_VFS.cpp:960-963).
- `[USERS] user approved` (System_User.cpp:1284 — **the web approval path bypasses the command audit entirely**); `[USERS] users.json REWRITE FAILED during approve/ban/delete` (System_User.cpp:1259-1262, 895-896) — auth DB inconsistency, currently volatile.
- `[REBOOT]` at every restart site: `cmd_reboot` (System_Utils.cpp:2001-2005 — **a commanded reboot is recorded nowhere durable today**), G2 power-menu restart (G2_Page_Power.cpp:104-111), setup/restore `rebootWithMessage` (System_FirstTimeSetup.cpp:155-161). Every reboot should be attributable: commanded, setup, or crash (absence of a REBOOT event before a BOOT event = power cut or panic).
- `[TIME] clock STEPPED by ±Ns while already synced` (System_Utils.cpp:2270-2276) — large NTP steps mean RTC drift/dead battery, and they corrupt the meaning of every other timestamp.
- `[AUTO] scheduled sub-command DROPPED (exec queue full)` (System_Automation.cpp:98-99) — a scheduled run silently did not fully happen.

**Peripherals / apps**
- `[I2C] bus init FAILED — every device on this bus unavailable` (System_I2C_Manager.cpp:290) — i2c_errors.log is device-level only, bus-level death is unrecorded.
- `[SENSOR] autostart skipped: enabled in settings but not detected` (System_I2C.cpp:2986 etc., ≤9 lines worst case); `start FAILED (detected but init failed)` (System_I2C.cpp:2751+); `auto-DISABLED after N consecutive I2C failures` (i2csensor_seesaw.cpp:645-660 and siblings) — the canonical "it just stopped working" class.
- `[DISPLAY] OLED enabled but init failed at boot` (OLED_Utils.cpp:4258).
- `[SR] model source fallback (SD/config load failed → partition models)` (System_ESPSR.cpp:1633+).
- `[LLM] state corruption — ERROR state until reload` (System_LLM.cpp:1342-1344).
- `[LOG] sensor-log / system-log autostart FAILED` (System_SensorLogging.cpp:1143-1145, System_Debug.cpp:3329-3332) — a logger that fails to start is invisible precisely because it's the logger.

**Borderline (maybe, decide at implementation):** BLE boot auto-reconnect outcome per peer; BLE secure-channel handshake rejection (wrong passphrase / MITM); SD mounted-but-unwritable probe result; log-rotation failure; battery critical transition (battery.csv mostly covers it); camera init failure (autostart path only); LLM load-fail reason + context auto-reduction; NTP-failed/RTC-fallback boot line; HTTPS cert expired (log expired only, not the 30-day countdown).

**Deliberately excluded (noise or already covered):** phone BLE connect/disconnect (login logs + several/day), G2 wear/unwear temple events (routine), mesh failover promote/demote (already logged via setMeshRole), session rekeys (routine), all CLI-driven user/automation/espnow commands (command audit), sleep/wake/powersave (battery.csv event column), SD absent at boot (fires every boot on card-less boards), login lockouts (failed_login.log), sensor-log rotation (routine).

### 7.5 v2 SHIPPED (2026-07-03) — status and post-implementation review

The v2 tier above is **implemented** (uncommitted, builds green esp32s3, ~90 `logSystemEvent` sites across 34 files) plus a design extension: log lifecycle transitions **both directions** (online *and* offline), via central funnels (`notifySensorStarted`, `logFileTransferEvent`, `xTaskCreateLogged` failure hook). A per-boot divider `logBootAnchorToLogs()` also writes `Device Powered On | boot #N | reset=…` to the 4 login/i2c/error logs (not system-events.log). See [docs/AUTH_LOG_FORMAT.md](AUTH_LOG_FORMAT.md).

An adversarial review of the v2 additions found and **fixed** the following (all confirmed real; two were serious):

- **HIGH — mesh peer-offline flood.** The offline sweep gated on `isActive`, which *any* RX frame re-arms and the 5 s bootstrap re-creates — so a powered-off or heartbeat-silent-but-ACKing peer logged `peer … offline` on every 100 Hz sweep, churning the whole 256 KB log in minutes. Fixed with an aliveness-edge latch (`MeshPeerHealth.onlineLogged`) keyed on `isMeshPeerAlive` (heartbeat-based), which also yields the `peer … online` recovery counterpart for free ([System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) §3c).
- **HIGH — file-receive FAILED per-chunk flood.** `v4h_file_data` logged a durable `RECV_FAILED` on every `STREAM_APPEND_FAIL`; after the first failure every remaining in-flight chunk (≈1700 on a 343 KB transfer) returned FAIL again → dozens-to-hundreds of identical lines per aborted transfer. Fixed with a new `STREAM_APPEND_ALREADY_FAILED` code so only the first failing chunk logs.
- **MED — automation drop** now rate-limited (one line / 5 s + suppressed count); **camera "online"** no longer fires from the per-frame capture-recovery path and reports `DEGRADED` when the sensor handle is NULL; **ESP-NOW init FAILED** now covers all three exits (was 1 of 3); **file-transfer timeout** now emits a durable RECV_FAILED (was the one uncovered receive-failure path).
- **LOW —** `notifySensorStarted` no longer hardcodes the I2C-specific "detected but init failed" (wrong for the non-I2C "Logging" caller); OLED init-failure no longer writes a false hardware-failure record during first-time setup on a no-OLED board; I2C bus-1 invalid-pins now logs symmetrically with bus 0; `AUTH_LOG_FORMAT.md` corrected (the time-sync anchor *is* written to system-events.log).
- **Refuted / not changed:** the bond-peer-offline min-interval concern (already edge-gated); a claimed pre-init-ring overflow of `[EVENT][BOOT]`; the "I2C bus online overstates" nuance (kept "online" for vocabulary consistency with the other subsystem online events — bus-level up vs device-reachability is `i2c_errors.log`'s domain).

**Still not done:** hardware flash test (v1+v2), commit (awaiting HW validation), and settings-audit **Phase 1** data-safety fixes (corrupt-load quarantine, NVS crash counters, format-on-fail guard, checked save returns) — see §8.

---

## 8. Remediation plan (staged; user tests on HW between phases; no commits until validated)

**Track A — system event log first (§7.3).** Independent of everything else and makes every later phase observable/testable. Small.

**Phase 1 — stop the two invalid-overwrite paths (data safety):**
1. Corrupt-load quarantine: tri-state `readSettingsJson`; on corrupt → rename to `settings.json.bad`, EVENT log, run on defaults, **no auto-write** (an explicit user save is the only way defaults reach disk).
2. Move `crashCount`/`lastResetReason` to NVS — removes the boot write that pulls the trigger in 4.1, and stops crash counters churning the settings file.
3. Format-on-fail: require a persisted consecutive-mount-failure marker (NVS counter ≥ 2) before formatting; EVENT-log any format.
4. Check `writeSettingsJson()`'s return at `handleSettingCommand`/`cmd_savesettings` → `Error:` status (uniform contract); delete the non-atomic fallback (rename fails ⇒ delete tmp, return false loudly); verify `bytesWritten == measureJson(doc)` before renaming so a partial serialize can never replace a good file.

**Phase 2 — write hygiene:**
5. One `FsLockGuard` across the whole save + a `gSettings` write mutex (or route all saves through cmd_exec); tmp file to `/system/settings.json.tmp`.
6. `gDeferWrites`: scope guard + max-age auto-flush; clear on batch-abort paths.
7. Kill the wifi-connect rewrite (drop `lastConnected` or keep RAM-only).
8. Always write the networks/meshes/peers arrays (fixes empty-list resurrect; prerequisite for Phase 3).
9. Fix runtime leak-through: `setMeshRole` uses a runtime shadow; `applyPowerMode` stops persisting brightness.

**Phase 3 — modernization sweep [policy deletions]:**
10. Delete the merge-read orphan mechanism (after #8) — saves become pure serialization.
11. Delete: legacy `criticalFiles` settings entry (quarantine semantics for the rest), bond scalars, flat wifi fields, flat-key web map, `espnowFirstTimeSetup`, AJ6 fossil comments, dead breadcrumbs, `wifiPrimarySSID` injection; converge on `PollPauseGuard`; atomic users.json bootCounter write; `writeTextAtomic` in migration restore; delete dead `registerMigrationRestoreHandler`.

**Optional hardening (noted, not scheduled):** `esp_wifi_set_storage(WIFI_STORAGE_RAM)` so cleartext WiFi creds stop persisting in NVS (settings.json already stores them AES-at-rest and re-applies at boot); a deliberate `settingsreset` command so the only paths to defaults stop being accidents.
