# VFS raw-API removal: migration plan

Status (2026-10-28, later the same day): Phase 5 item 1 (capture-tree
confinement of `sensorlog start`, `log start` and both autostarts) and Phases
1-3 (helpers take an identity, the 65 callers and the 13 direct raw calls
converted) are APPLIED and uncommitted. Builds: p4x_eye full-debug green,
feathers3 green, xiao_s3 default lane overflows its factory partition by
3,888 B (headroom was already near zero on this tree). Host suite 321/324;
the three failures (tof_poll_outcomes, tool_pins, oled_mode_availability)
read none of the files this work touched. Phase 4 (make the raw functions
file-static) and the rest of Phase 5 remain.
Hardware check 2026-10-28 on the P4X-EYE (app-only reflash of the full-debug
image, serial capture from the ROM banner, `p4_seriallog_vfs_hwcheck_2026-10-28.txt`):
zero `[PERM]` lines across boot, a logged-in command session and two idle
minutes; command-audit.log grew 1145→1221→2188 B; `log start "/system/x.log"`
and `sensorlog start /system/users/users.json` refused before any open;
`log start` / `sensorlog start` inside /logging_captures created, ran and
stopped; useradd/userlist/userdelete, notifydevicekind (notifications.json),
systemlogenabled/sensorlogenabled (settings.json) and fileview of errors.log
all clean. Not covered on this board: automations (ENABLE_AUTOMATION=0 on
p4x_eye), backup/restore (HTTP needs Wi-Fi), a forced capped-log rotation.
Caveat: the full-debug console drops some boot lines when the debug queue
saturates, so "zero [PERM] on the console" has that small blind spot.
Phase 4 APPLIED the same day: the six raw declarations are gone from
`System_VFS.h`, the dispatchers are `static` in `System_VFS.cpp`, `fake_vfs.h`
lost its raw forwarders (its self-test drives `fake_vfs::fs()` directly), and
six runners that sliced the header up to the old `bool exists(...)` line now
anchor on `bool getStats(`. The remaining harness-local raw fakes are the
backends of their own guarded fakes, not bypasses. Builds after Phase 4:
p4x_eye full-debug green (18% free, now with ENABLE_AUTOMATION on for this
board at the user's request), feathers3 green (1% free, 592 B smaller),
xiao_s3 overflow shrank to 3,200 B. P4 hardware re-check with automations:
boot creates the empty store, `automation add`/`list`/`delete` rewrite
automations.json (250 → 42 B), zero `[PERM]` lines. Host suite: the six
affected suites pass individually; full run 321/324, the same three
pre-existing failures as before Phase 4.
Phase 5 APPLIED the same day: every system context is scoped except four
that are unconfined on purpose and say why (boot tmp sweep, boot mkdirs,
factory reset, the root-level battery log). Restore has a static allow-list
(the six state files, user_settings, certs, maps) and per-tree scopes
(/maps or /system); Edge Impulse classifies a file AS THE CALLER and its
model folder is scoped; mic cleanup, GPS tracks, ESP-NOW state/captures,
ESP-SR, STT, LLM, images, the lens snapshot, MQTT CA cert, users, settings,
time anchors and IP bans all have scopes, with tier pickers for folders that
live on either storage. New scopes: MAPS, EI_MODELS, RECORDINGS(_SD),
ESPNOW_STATE, ESPNOW_CAPTURES, LLM_MODELS(_SD), ESPSR(_SD), PICTURES_SD.
Two things the P4 hardware check caught: the settings/debug writers staged
through a fixed temporary at the filesystem ROOT (now /system/settings.tmp
and /system/debug.tmp), and a pre-existing `log stop` race that reopened an
empty path from the debug task (stop now retires the file and path in one
lock hold; the task re-checks under the lock). After those: zero [PERM]
lines across boot, two log start/stop cycles, sensorlog, user add/delete,
automation delete, notification and settings saves, and two idle minutes.
Builds: P4 green, feathers3 green (1% free), xiao_s3 overflow 5,264 B.
Host suite 321/324 (same three pre-existing failures); 12 harness fakes
gained the scope constants and tier pickers; the restore harness gained a
foreign-path refusal case and a map-file case.
Line numbers below are from the tree before these edits and have drifted.

## Goal

Every file access in the firmware goes through the guarded VFS pipeline
(normalize, role lookup, scope confinement, dispatch). The raw entry points
`VFS::exists / open / mkdir / remove / rename / rmdir` stop being callable
from outside `System_VFS.cpp`, so the compiler, not a scan, proves there is
no second path. System code keeps working through `systemAuth(scope, reason)`
and, as part of the same sweep, every system context that can be confined to
a subtree is.

## Why the raw API exists today

- Raw `VFS::open` and the `readText` family date from the initial ESP-IDF
  migration (a8014ca4, 2026-01-11). The guarded layer and the rule table
  arrived with the "Phase 2 perm refactor" checkpoint (0b3c218b, 2026-05-09).
  The refactor converted the user-facing surfaces and left the helpers and a
  few internals on the old calls. Nothing enforces the header's "use
  systemAuth for internal code" note.
- The raw functions are the dispatch substrate: `openGuarded` ends in
  `return open(norm, mode, create)`. They must keep existing inside
  `System_VFS.cpp`; they only need to stop being public.
- For the SYSTEM role the rule table grants `PERM_ALL` everywhere, so there is
  no functional reason for internal code to skip the check. What the guarded
  path adds for system code is scope confinement, traversal rejection, and a
  `[PERM]` denial line when something goes wrong.

## Current state in numbers

| What | Count | Where |
|---|---|---|
| Raw `VFS::*` calls outside `System_VFS.cpp` | 32 | 19 inside the text helpers, 13 direct (5 files) |
| Callers of the text helpers | 65 | 13 files |
| Direct backend bypasses (`LittleFS.`, `SD.`, POSIX) | 1 | `::stat` in `loadUserSettings` (deliberate, see below) |
| `systemAuth` contexts, unscoped | ~175 | 26 files |
| `systemAuth` contexts, scoped | 44 | 14 files |
| Host harnesses that declare their own `namespace VFS` | 44 | `test/host/*_harness.cpp` |
| Host harnesses that fake a text helper | 8 | see Phase 1 |
| Host runners that extract the real helper bodies | 2 | `test_checked_text_writes.py`, `test_capped_log_writes.py` |

No `using namespace VFS` anywhere. The only backend calls outside the VFS are
the two `LittleFS.begin` mounts in `initFilesystem` and the SD mount/format
code, which stay.

## The one security finding from the scan

`sensorlog start <filepath>` is registered with `requiresAdmin = false`
(`System_SensorLogging.cpp:2619`). The only validation is a leading `/`
(`sensorLogStart`, ~1360). Directory creation runs as
`systemAuth("senlog.setup_mkdir")`, file creation as
`systemAuth("senlog.setup_create")`, and every later row is appended by
`openSensorLogForAppend` under `systemAuth("senlog.append")` (line 291) with
mode `"a"` on any existing file. A signed-in non-admin user can therefore
append sensor rows to `/system/users/users.json` or `/system/settings.json`.
Verified by reading the code, not exercised on hardware. `log start` has the
same shape but is admin-only. Both get the same fix in Phase 5, item 1, and
that item can be pulled forward on its own.

## Design decisions (recommendations marked)

1. **Helper identity parameter.** `readText`, `writeText`, `writeTextAtomic`
   and `readTextLimited` gain a trailing `const AuthContext& ctx` with no
   default. A default would recreate the unguarded path. (Recommended.)
2. **Capped-log writer is sys_logs-only by construction.** `appendLineWithCap`
   keeps its signature. Internally it resolves the overflow tier first, then
   builds `systemAuth(sysLogScopeFor(dest), "log.append")` and uses the
   guarded calls. Every one of its 17 callers writes under
   `/system/sys_logs/`; the one generic wrapper, `logToFile(path)`, needs its
   callers checked during implementation. `recoverLogOrphanAt` /
   `cleanupLogOrphan` use the same confined identity. The result: the only
   writer that can rotate a log cannot be pointed outside the log tree.
   (Recommended; the alternative is a ctx parameter on all 17 sites for no
   gain.)
3. **Scope granularity for /system state files.** `pathWithinScope` is
   boundary-aware, so a scope naming an exact file rejects the `.tmp` sibling
   every atomic writer needs. Options: (a) directory scopes now, `"/system"`
   for settings/debug/automations/boot-anchors/notifications and
   `"/system/users"` for the roster family; (b) teach `pathWithinScope` a
   file-stem rule (`scope` or `scope + ".tmp"`), giving exact-file scopes.
   (Recommend (a) now; (b) is a small later refinement that tightens 20 sites
   without touching callers.)
4. **Capture commands confine to the capture tree.** `log start`,
   `sensorlog start` and the system-log autostart reuse the existing
   `pathInCaptureTree` + `captureScopeFor` pair the three stitch commands
   already use. This refuses paths outside `/logging_captures` (either tier).
   Behaviour change for anyone logging elsewhere; the project's no-compat rule
   applies. (Recommended.)
5. **Restore gets an allow-list, not a user identity.** Admins are READ-only
   on `settings.json` and `users.json` in the rule table, so the restore has
   to stay a system identity. Confine it two ways: a static allow-list of
   restorable paths (mirror of what `addFileToBackup` emits) checked before
   staging, and `systemAuth("/system", "migration.restore")` for the writes.
   (Recommended.)

## New scopes needed

Add to `VFS::Scopes` in `System_VFS.h`:

| Constant | Prefix | Used by |
|---|---|---|
| `SYS_LOGS` / `SYS_LOGS_SD` | `/system/sys_logs`, `/sd/system/sys_logs` | capped-log writer, orphan recovery, crash history, command audit |
| `SYSTEM_STATE` | `/system` | settings, debug.json, automations, boot anchors, notifications, pending-credential marker, restore |
| `USERS` | `/system/users` | users.json, pending_users.json, ip_bans.json, user_settings |
| `ESPNOW_STATE` | `/system/espnow` | devices.json, identity, peers, schema/manifest caches |
| `LLM_MODELS` | `/system/llm` | on-device LLM model loading |

Plus `sysLogScopeFor(dest)` next to `captureScopeFor`. ESP-SR, Edge Impulse,
STT, images and mic recordings use folders from settings or module constants;
those get computed scopes at the call site (the header already anticipates
this) and are listed in Phase 5.

## Phase 1: the helpers (the sites the compiler will point at first)

Files: `System_Utils.cpp` (readText 701, writeText 719, writeTextAtomic 743),
`System_Filesystem.cpp` (readTextLimited 2215, appendLineWithCap 2276,
recoverLogOrphanAt 2429, cleanupLogOrphan 2452), declarations in
`System_Utils.h:76-86` and `System_Filesystem.h:231,246`. Stray forward
declarations of `appendLineWithCap` in `HardwareOne.cpp:255` and
`System_TaskUtils.cpp:37` go away (include the header).

| Line | Raw call | Becomes |
|---|---|---|
| System_Utils.cpp:709 | `VFS::open(path, "r")` | `openGuarded(path, "r", ctx)` |
| System_Utils.cpp:727 | `VFS::open(dest, "w", true)` | `openGuarded(dest, "w", ctx, true)` |
| System_Utils.cpp:756 | `VFS::rename(tmp, dest)` | `renameGuarded(tmp, dest, ctx)` |
| System_Utils.cpp:762 | `VFS::remove(tmp)` | `removeGuarded(tmp, ctx)` |
| System_Filesystem.cpp:2222 | `VFS::open(path, "r")` | `openGuarded(path, "r", ctx)` |
| System_Filesystem.cpp:2301, 2326, 2373 | three opens | `openGuarded` with the sys_logs identity |
| System_Filesystem.cpp:2364, 2393, 2403, 2404, 2410 | `VFS::remove` | `removeGuarded` |
| System_Filesystem.cpp:2409 | `VFS::rename` | `renameGuarded` |
| System_Filesystem.cpp:2433, 2435 | `VFS::exists` | `existsGuarded` |
| System_Filesystem.cpp:2437, 2441, 2445 | remove / rename / remove | guarded equivalents |

Lock contract is unchanged: the helpers already take `FsLockGuard`; raw
`open()` takes it reentrantly; `openGuarded` does not take it and dispatches to
raw `open()`. `PollPauseGuard` ordering in readText/writeText stays.

Host-test fallout for this phase:

- `test_checked_text_writes.py` and `test_capped_log_writes.py` extract the
  real bodies and compile them against `fake_vfs.h` with
  `FAKE_VFS_PROVIDE_VFS_NAMESPACE`, which already forwards the guarded
  signatures. They need the new scope constants visible and, for the
  checked-text suite, a ctx argument in its calls.
- Eight harnesses define their own fake of a helper and must grow the ctx
  parameter: `automation_scheduler_harness.cpp` (readText 85,
  writeTextAtomic 95), `fileread_psram_harness.cpp` (readTextLimited 161),
  `g2_automations_page_harness.cpp` (readText 36),
  `g2_files_publish_harness.cpp` (readTextLimited 81),
  `initial_admin_setup_harness.cpp` (writeTextAtomic 76),
  `settings_file_writes_harness.cpp` (readText 281),
  `web_json_replies_harness.cpp` (readText 104).

## Phase 2: the 65 helper callers

Proposed identity per site. "state" = `systemAuth(Scopes::SYSTEM_STATE, …)`,
"users" = `systemAuth(Scopes::USERS, …)`, "caller" = the already-available
`currentAuthContext()` or the request's context.

| File | Lines | Function(s) | Path | Identity |
|---|---|---|---|---|
| System_Automation.cpp | 769, 898, 929, 995, 2688, 2704, 2868, 2894, 2916, 4233, 4587 | cache rebuild, atomic write, scheduler, reschedule | automations.json | state |
| System_Automation.cpp | 1271, 1450, 1751, 1828, 1939, 2190, 2326 | `cmd_automation_*` handlers | automations.json | state (admins are READ-only in the table; the command layer already gates these as admin) |
| System_CrashRecord.cpp | 461 | crashRecordPersistToFile | crash-history.log | none needed (appendLineWithCap, decision 2) |
| System_Debug.cpp | 377, 391, 401, 2381, 2440, 2491-2498, 2536, 2558-2561, 2574 | debugOutputTask, logToFile, logAuthAttempt, markers, I2C | LOG_* files | none needed (decision 2); check `logToFile` callers |
| System_Filesystem.cpp | 236 | initFilesystem critical-file validate | debug/automations/users.json | the `sys` context already built on line 225, re-scoped to state |
| System_Filesystem.cpp | 279, 586 | initFilesystem, runFileWritePostSaveHooks | automations.json | state |
| System_Filesystem.cpp | 831 | cmd_fileview | user path | caller (existsGuarded on 818 already uses it; this removes the split check) |
| G2_Page_Files.cpp | 959 | showFileInfo | user path | caller (canRead on 958 already; same split) |
| System_FirstTimeSetup.cpp | 340 | createInitialAdminUser | users.json | users |
| System_Notifications.cpp | 208, 251 | notifPolicyLoad/Save | notifications.json | state |
| System_Settings.cpp | 1883 | settingsRestampFirmwareVersion | settings.json | state |
| System_TimeAnchors.cpp | 139 | registryRewrite | kAnchorsPath | state (confirm the path lives under /system) |
| System_User.cpp | 712, 2310, 2348, 3097, 4409, 4521, 4564, 4677, 4701, 4717 | roster, pending, boot anchors | users.json, pending_users.json, boot_anchors.json | users for the first two, state for boot anchors (file is `/system/boot_anchors.json`) |
| System_Utils.cpp | 878 | logCommandExecutionPrepared | command-audit.log | none needed (decision 2) |
| WebServer_MigrationTool.cpp | 226 | addFileToBackup | each backup path | `systemAuth(SYSTEM_STATE, "migration.backup")`; the path list is static |
| WebServer_MigrationTool.cpp | 552, 642, 653 | writeRestoreFilesFromDoc | settings.json, staged restore paths | `systemAuth(SYSTEM_STATE, "migration.restore")` after the allow-list check (decision 5) |
| WebServer_Server.cpp | 1199 | saveIpBans | ip_bans.json | users |
| WebServer_Server.cpp | 3580, 5023, 5084 | handleAutomationsGet/Export | automations.json | state (handlers already gate admin) |

## Phase 3: the 13 direct raw calls

| File:line | Function | Today | Becomes |
|---|---|---|---|
| G2_Page_Files.cpp:836 | filesReadTextBuffer | `VFS::open(path, "r")` after a separate canRead | `openGuarded(path, "r", currentAuthContext())`; drop the separate check |
| QuickNote_Store.cpp:309 | create | `VFS::exists(root)` then scoped mkdirGuarded | `existsGuarded(root, systemAuth(root, "note root"))` |
| QuickNote_Store.cpp:314 | create | `VFS::exists(path)` | `existsGuarded(path, ctx)` (owner ctx on 302) |
| QuickNote_Store.cpp:375 | restamp | `VFS::exists(copy)` | `existsGuarded(copy, ctx)` |
| QuickNote_Store.cpp:540, 544 | sweep | `VFS::exists(nativeFolder/root)` | `existsGuarded` with the scoped sweep identity already built on 541/547 |
| System_Transcript.cpp:135 | TranscriptSession::write | `VFS::exists(root)` | `existsGuarded(root, systemAuth(root, "transcript root"))` |
| System_Transcript.cpp:139 | TranscriptSession::write | `VFS::exists(status_.path)` | `existsGuarded(status_.path, ctx)` |
| System_Utils.cpp:2724 | cmd_crashlog | `VFS::exists(crash-history.log)` for a hint | `existsGuarded(..., currentAuthContext())`; non-admins lose the hint, which matches their READ denial on sys_logs |
| WebServer_MigrationTool.cpp:668 | writeRestoreFilesFromDoc | `VFS::rename(staging, dest)` | `renameGuarded` with the restore identity |
| WebServer_MigrationTool.cpp:680, 682 | writeRestoreFilesFromDoc | `VFS::exists` / `VFS::remove` on staging | `existsGuarded` / `removeGuarded`, same identity |

## Phase 4: close the header

- `System_VFS.h:184-189`: delete the six raw declarations. Add a comment at
  the guarded block saying the raw dispatchers are private to
  `System_VFS.cpp`.
- `System_VFS.cpp`: mark `exists/open/mkdir/remove/rename/rmdir` `static`
  (or put them in an anonymous namespace). `openGuarded` and friends,
  `StagedWrite`, `replaceFileGuarded`, the SD probe and the format path are
  all in the same file and keep calling them.
- `fake_vfs.h`: the `FAKE_VFS_PROVIDE_VFS_NAMESPACE` block forwards the raw
  names too; drop them so a harness cannot compile a bypass the firmware no
  longer has. `fake_vfs_harness.cpp` / `test_fake_vfs.py` exercise the fake's
  own "guarded-versus-unguarded lock seam"; that test adjusts.
- The 44 harness-local `namespace VFS` blocks: only those that declare a raw
  signature need editing. Find them with
  `grep -l -E 'File open\(|bool exists\(' test/host/*_harness.cpp` once the
  header change makes the real callers stop compiling.
- Build gate: every board lane, not just the P4. Board-gated code hides
  breaks; the memory note on that is the reason.

## Phase 5: scope the unscoped system contexts

Ranked by exposure. Items 1-3 take a path from outside the firmware.

1. **`sensorlog start` / `log start` / system-log autostart.**
   `System_SensorLogging.cpp` 1369-1407 (`senlog.setup_*`), 291
   (`senlog.append`), 355, 393, 1094-1144 (size/rotate); `System_Debug.cpp`
   1987-2009 (`debug.log_setup_*`), 327 (`debug.system_log_append`), 2603
   (`debug.log_autostart`). Add `if (!VFS::pathInCaptureTree(filepath))
   return "Error: ..."` before any open, then `systemAuth(captureScopeFor(p),
   …)` everywhere. The sensor-log rotation family (1122-1144) derives its
   names from the running path, so it takes the same scope. Also make
   `sensorlog` `requiresAdmin = true` or gate `start` on admin; the plan does
   not pick, both are one-line.
2. **Restore** (`WebServer_MigrationTool.cpp` 606 mkdir, 1024-1028 cert
   restore): allow-list plus `SYSTEM_STATE`; certs use `Scopes::CERTS`, which
   the cert commands already do.
3. **Edge Impulse explicit paths** (`System_EdgeImpulse.cpp` 161/165
   `edge.labels_load`, 1290 `edge.image_classify`): the path comes from a
   command argument. Scope labels to the model directory and classification
   to the configured capture folder (computed scope, same pattern as
   `System_STTVocabulary.cpp:60`).
4. **Mic recording removal** (`System_Microphone.cpp:486` via four callers):
   scope to the recordings folder.
5. **GPS track save** (`System_Maps.cpp:3103`): `Scopes::CAPTURES`; the table
   comment already says this impersonation only exists because admins lacked
   CREATE there, which has since been granted, so the caller's own context may
   suffice.
6. **Fixed-path families**, mechanical:
   `System_ESPNow.cpp` (33: devices/identity/caches → `ESPNOW_STATE`;
   capture → `CAPTURES`; bond JSON via `writeBondJsonFileLocked` →
   `ESPNOW_STATE`), `System_ESPNow_Identity.cpp` (15 → `ESPNOW_STATE`),
   `System_User.cpp` (27 → `USERS`, boot-seq/anchors → `SYSTEM_STATE`),
   `System_Settings.cpp` (10 → `SYSTEM_STATE`, user_settings → `USERS`),
   `System_ESPSR.cpp` (12 → its model/commands folder), `System_EdgeImpulse.cpp`
   (remaining 8 → model dir), `System_ImageManager.cpp` (6 → computed from
   `gSettings.cameraCaptureFolder`), `System_STTLocal.cpp` (4 →
   `STT_MODELS`/`_SD`), `System_TimeAnchors.cpp` (4), `System_LLMBackend.cpp`
   + `System_LLM_Model.cpp` (3 → `LLM_MODELS` or the SD model folder),
   `System_BondedPeer.cpp` (2 → `ESPNOW_STATE`), `System_Automation.cpp:909`,
   `System_Notifications`, `WebServer_Server.cpp` 1099/1104 (`USERS`),
   `HardwareOne.cpp` 1726/1755 (`SYSTEM_STATE`), `G2_Glasses.cpp:37996`
   (capture folder), `System_Battery.cpp:521` (`/battery.csv` plus its `.1`,
   `.2` and staging siblings live at the root, so no directory scope is
   tighter than "everything"; leave unscoped with the reason string saying
   why).
7. **Legitimately unconfined**, keep with a reason that says so:
   `System_Filesystem.cpp` 90 (boot tmp sweep across the tree), 168, 191
   (boot mkdirs), 277; `System_Utils.cpp:2483` factory reset;
   `System_FirstTimeSetup.cpp` 120/442/445/932 (setup touches users, settings
   and the marker; `SYSTEM_STATE` fits if preferred).

After this phase the count of unscoped contexts should be in the single
digits, each with a reason string that names why.

## Phase 6: verification

Host suites to run (all green today): user_roster_authority,
user_store_mutations, display_auth_policy, credential_attempts,
checked_text_writes, capped_log_writes, boot_temp_cleanup,
capture_stitching, factory_reset_wipe, espnow_fs_transcript_privacy,
filesystem_listing_permissions_contract, migration_restore, ip_ban_store,
time_anchor_persistence, automation_scheduler, fileread_psram,
g2_files_publish, initial_admin_setup, settings_file_writes,
web_json_replies, fake_vfs, quicknote_store, transcript_path_permissions.

Builds: p4x_eye (the bench image, `p4-full-debug`), feathers3, xiao_s3.

Hardware, on the P4 with the full-debug image, reading the `[PERM]` lines
after each step:

1. Erase, flash, first-time setup; boot must show no `[PERM] DENY`.
2. Login, a few commands: command-audit.log still grows (the writer is now
   confined to sys_logs).
3. `log start "/logging_captures/system/t.log"` succeeds; `log start
   "/system/x.log"` is refused before any open.
4. `sensorlog start /logging_captures/sensors/s.txt` succeeds; a path under
   `/system` is refused; as a non-admin user `start` is refused outright if
   the admin gate was chosen.
5. Backup, then restore the same bundle; a bundle with an extra path outside
   the allow-list is refused with the path named.
6. Force a capped-log rotation (lower `LOG_ERROR_CAP` on the bench or flood
   errors) and confirm the `.tmp` promote still works on LittleFS, then with
   the SD overflow latch tripped.
7. Settings save, user add/approve/delete, automation add/delete, notification
   policy save: no denials.
8. Leave the device idle for an hour with everything on; zero `[PERM]` lines.

## Risks

- A scope chosen too tightly turns a working write into a logged denial. The
  SD overflow mirror is the usual trap: anything that can land on `/sd/...`
  needs the `_SD` twin picked at runtime, as `captureScopeFor` does.
- Cost: one rule-table scan and a few string compares per helper call,
  microseconds against a 40 ms flash open. The command path already pays
  this in `existsGuarded`.
- Named-account contexts resolve their role through the roster, which is
  itself read under a system identity, so there is no recursion; the
  authority cache keeps that at 57 µs.
- Harness churn is the largest line count: the helper signature change touches
  eight fakes and two runners before the header change touches up to 44.
- `loadUserSettings` keeps its `::stat` on the physical path
  (`System_Settings.cpp:4049`); it exists to tell ENOENT from a transient
  failure, runs under the FS lock on a path derived from a numeric id, and is
  the one deliberate hole in "no backend calls outside the VFS". A
  `VFS::statGuarded` could absorb it later.

## Order of work

Phase 1 → 2 → 3 compile together (the helpers cannot change without their
callers). Phase 4 is a separate compile gate. Phase 5 item 1 can land before
or after any of them and is the only part with a security clock on it. Phase
6 runs once at the end, with host suites after each phase.
