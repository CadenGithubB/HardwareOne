# Decouple migration tool from `ENABLE_HTTP_SERVER`

## Goal

Allow `ENABLE_HTTP_SERVER=0` builds to still expose the FTS restore-from-backup
recovery path. Today, turning off the regular web UI also drops the migration
tool entirely — there is no path to recover a bricked-config device via
backup file.

## Current state

`components/hardwareone/WebServer_MigrationTool.cpp` is a single file holding
two **logically distinct** features:

| Sub-feature | Endpoint(s) | Auth | Lifetime | Depends on `WebServer_Server.cpp`? |
|---|---|---|---|---|
| **Backup export** | `POST /api/backup`, `OPTIONS /api/ping` | Authenticated (`tgRequireAuth`) | Registered alongside main web UI when device is up | **Yes** — uses `makeWebAuthCtx`, `tgRequireAuth` |
| **Restore-only server** | `GET /`, `OPTIONS+POST /api/ping`, `POST /api/restore` | No auth (gated by FTS state + `gAcceptingRestore`) | Spun up only during FTS "Import from Backup", torn down on completion/abort | **No** — fully self-contained |

The whole file is gated at the top by `#if ENABLE_HTTP_SERVER` ([line 15](../components/hardwareone/WebServer_MigrationTool.cpp)),
which conflates these two features. Restore-only mode is held hostage to the
regular web UI's compile flag despite needing none of its code.

### What restore-only actually depends on

Verified by reading every external call in the restore path:

- `httpd_handle_t server` — global defined in [`HardwareOne.cpp:370`](../components/hardwareone/HardwareOne.cpp), **not** in `WebServer_Server.cpp`
- `getDeviceFingerprint()` — lives in `System_Settings.cpp`
- `gFirstTimeSetupState`, `gAcceptingRestore`, `gRestoreComplete` — in `System_FirstTimeSetup.cpp`
- `broadcastOutput()` — in `System_Debug.cpp` / `System_Utils.cpp`
- `LittleFS`, `VFS` — filesystem layer
- `httpd_start()` / `httpd_ssl_start()` — ESP-IDF native API
- `setCorsHeaders()` — defined locally inside `WebServer_MigrationTool.cpp`

**Zero calls into `WebServer_Server.cpp`.** The restore-only path could already
compile without it — it's only the file-level gate that's blocking.

### What backup-export depends on (for contrast)

- All of the above
- **Plus**: `makeWebAuthCtx()`, `tgRequireAuth()` — declared in `WebServer_Server.h`
- **Plus**: `buildSystemInfoJson()` — defined in `WebServer_Server.cpp`

So the backup endpoint genuinely cannot survive `ENABLE_HTTP_SERVER=0`. That's fine.

## Proposed design

Introduce a new compile-time flag `ENABLE_MIGRATION_TOOL`, default-on whenever
`ENABLE_HTTP_SERVER` is on. This preserves existing behavior bit-for-bit.

Inside `WebServer_MigrationTool.cpp`, the outer guard becomes `ENABLE_MIGRATION_TOOL`,
and the backup-export sub-feature is further wrapped in an inner `#if ENABLE_HTTP_SERVER`
because it depends on auth helpers from `WebServer_Server.h`.

Resulting build matrix:

| `ENABLE_HTTP_SERVER` | `ENABLE_MIGRATION_TOOL` | Build outcome |
|:---:|:---:|---|
| 1 | 1 | **Current behavior.** Full web UI + `/api/backup` + restore-only server. |
| 1 | 0 | Full web UI, no migration endpoints. (Niche, but lets paranoid deployments strip the unauthenticated `/api/restore`.) |
| **0** | **1** | **New target.** No regular web UI. Restore-only server still available during FTS for recovery. |
| 0 | 0 | Headless, no HTTP at all. Current "HTTP off" build. |

## File-by-file changes

### 1. `System_BuildConfig.h`

Add the flag with sensible default that preserves current behavior:

```c
// Migration tool: backup/restore endpoints for the HardwareOne Migration Tool.
// Defaults to whatever ENABLE_HTTP_SERVER is set to — turning HTTP on/off
// flips this in lockstep, matching the historical single-gate behavior. Set
// explicitly to 1 with ENABLE_HTTP_SERVER=0 to ship a headless build that
// still offers FTS restore-from-backup as a recovery path.
#ifndef ENABLE_MIGRATION_TOOL
#define ENABLE_MIGRATION_TOOL ENABLE_HTTP_SERVER
#endif
```

### 2. `WebServer_MigrationTool.cpp`

Change outer guard from `#if ENABLE_HTTP_SERVER` (line 15) to
`#if ENABLE_MIGRATION_TOOL`.

Wrap backup-related code with an inner `#if ENABLE_HTTP_SERVER`:
- `static esp_err_t handleBackup(httpd_req_t*)` (line 162)
- `void registerMigrationBackupHandler(httpd_handle_t)` (line 512)
- `void registerPingOptionsHandler(httpd_handle_t)` (line 529)
- The `handlePingOptionsGet` handler if it depends on backup's auth path

Restore-only path stays unconditional within the outer `ENABLE_MIGRATION_TOOL`:
- `handleRestoreSplash`, `handlePingRestore`, `handleRestore`
- `registerMigrationRestoreHandler`, `unregisterMigrationRestoreHandler`
- `startRestoreOnlyHttpServer`, `stopRestoreOnlyHttpServer`

Estimated diff: ~6 added `#if`/`#endif` pairs, no logic changes.

### 3. `WebServer_MigrationTool.h`

Mirror the same gating on the public declarations. Functions only used by
the regular web server (`registerMigrationBackupHandler`, `registerPingOptionsHandler`)
go behind `#if ENABLE_HTTP_SERVER`. Restore-only entry points
(`startRestoreOnlyHttpServer`, `stopRestoreOnlyHttpServer`) go behind
`#if ENABLE_MIGRATION_TOOL`.

### 4. `System_FirstTimeSetup.cpp`

Change three `#if ENABLE_HTTP_SERVER && ENABLE_WIFI` blocks to
`#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI`:
- Line 204-207: `#include "WebServer_MigrationTool.h"` + `extern httpd_handle_t server`
- Line 243-250: serial menu listing "Import from Backup" as option 3
- Line 256-259: setup-mode dispatch for `setupMode=2`

Plus the same change at line 274 and wherever else the restore mode arm is gated.

### 5. `WebServer_Server.cpp:5246-5247`

These calls already live inside `WebServer_Server.cpp` which is itself gated
by `ENABLE_HTTP_SERVER`, so no change strictly required. **But** for clarity
and to handle the `(1, 0)` config, wrap them in `#if ENABLE_MIGRATION_TOOL`:

```cpp
#if ENABLE_MIGRATION_TOOL
  registerMigrationBackupHandler(server);
  registerPingOptionsHandler(server);
#endif
```

### 6. CMakeLists.txt

Verify `WebServer_MigrationTool.cpp` is in the source list unconditionally.
The build system already adds it; the `#if` inside the file handles
exclusion. No CMake change expected — confirm during implementation.

## Risks and edge cases

### Header pollution

`WebServer_MigrationTool.cpp` includes `"WebServer_Server.h"` at line 35. The
header is gated by `#if ENABLE_HTTP_SERVER` internally ([WebServer_Server.h:7](../components/hardwareone/WebServer_Server.h)),
meaning most declarations vanish when HTTP is off. Two questions to resolve:

1. Does the migration tool actually need anything from `WebServer_Server.h`
   in the restore-only path? Verified above: **no, only the auth helpers
   needed by the backup path.**
2. Can we just drop the `#include "WebServer_Server.h"` when only the
   restore path compiles? Yes — restore-only doesn't reference any symbol
   declared in that header.

Implementation choice: leave the include but make it conditional on
`ENABLE_HTTP_SERVER`. Cleaner than juggling forward declarations.

### Global `httpd_handle_t server` definition

The global `server` is defined in two places today:
- `HardwareOne.cpp:370` — `httpd_handle_t server = NULL;` (always compiled)
- `System_SensorStubs.cpp:201` — `httpd_handle_t server = nullptr;` (stub build)

If both compile, the linker errors with multiple definition. There must
already be a guard preventing both — likely the stubs file is only included
when `ENABLE_HTTP_SERVER=0`. Need to confirm during implementation; may need
to move the canonical definition outside any feature gate.

### "Import from Backup" menu text

With `(ENABLE_HTTP_SERVER=0, ENABLE_MIGRATION_TOOL=1)`, the FTS menu still
offers "Import from Backup" as option 3 and tells the user to use the
Migration Tool browser application. Accurate — the device IS running a
minimal restore-only HTTP server during that flow. No copy changes needed.

### Existing comment in code

Line 4-9 of `WebServer_MigrationTool.cpp` describes the file's purpose as a
single unit. The header comment should be updated to describe the two
sub-features and their independent gating.

### HTTPS

`ENABLE_HTTPS` is referenced inside `WebServer_MigrationTool.cpp` (lines 25,
601, 605, 608, 649, 667, 711). HTTPS support in restore-only mode is
desirable for a recovery flow. Whether HTTPS-without-regular-web-UI is
sensible is a separate question — left for follow-up. Current refactor
keeps HTTPS conditional independently as it already is.

## Upsides

- **Headless recovery path.** Devices shipped with `ENABLE_HTTP_SERVER=0`
  (no web UI in normal operation) can still be rescued from a bad config
  by holding "Import from Backup" during FTS.
- **Smaller flash for migration-only builds.** Removing the regular web
  UI saves an estimated 50-100 KB of flash (dashboard pages, sensor JSON
  endpoints, sessions, auth machinery, automation pages). The restore
  server alone is a few KB.
- **Independent disabling.** Security-paranoid deployments can ship
  `ENABLE_HTTP_SERVER=1, ENABLE_MIGRATION_TOOL=0` to strip the
  unauthenticated `/api/restore` endpoint entirely — currently that's not
  possible without forking the source.
- **Cleaner intent in the source.** The two sub-features are conceptually
  different (long-lived authenticated backup vs. transient unauthenticated
  recovery server). Today they look identical because of the shared gate.

## Downsides

- **Build matrix grows.** From 2 (`ENABLE_HTTP_SERVER ∈ {0,1}`) to 4
  combinations. In practice the (0,1) build is the new addition we'd
  test; (1,0) is niche. Risk: silent breakage in (0,1) until someone
  actually builds it.
- **Maintenance discipline.** Every future addition to
  `WebServer_MigrationTool.cpp` has to think about which sub-feature it
  belongs to and which flag gates it. The current single-gate pattern is
  brain-cheap.
- **Subtle linker risks.** The `httpd_handle_t server` double-definition
  needs verification (see above). If the stub-build guard relies on
  `ENABLE_HTTP_SERVER` to switch between definitions, we now need it to
  consider `ENABLE_MIGRATION_TOOL` too.
- **User confusion.** "Why does my headless build have an `/api/restore`
  endpoint?" is a fair question. The FTS setup menu should make it
  obvious this is only active during first-time setup. Already does, but
  worth re-checking the wording.

## Effort and verification

**Code changes:** ~30-50 lines across 5 files. Low complexity — all guard
shuffling, no logic changes.

**Build verification matrix:**

| Build | Expected | What to check |
|---|---|---|
| `HTTP=1, MIG=1` | Regression-free | Boot to dashboard, run backup via Migration Tool, restore on a wiped device |
| `HTTP=1, MIG=0` | Backup endpoint should 404 | Regular web UI works; `POST /api/backup` returns 404 |
| `HTTP=0, MIG=1` | New configuration | Boot to FTS, select "Import from Backup", confirm restore-only server reachable, send `.hwbackup`, confirm reboot with restored settings |
| `HTTP=0, MIG=0` | Same as today's HTTP-off build | No HTTP server at any point |

**Runtime tests for the new (0,1) build specifically:**

1. Boot a fresh device → FTS menu appears → option 3 "Import from Backup" is visible
2. Select option 3 → WiFi connect → restore-only server starts → ping returns fingerprint
3. POST a known-good `.hwbackup` to `/api/restore` → returns success → device reboots
4. After reboot: `ENABLE_HTTP_SERVER=0` still in effect, no web UI reachable, but device has the restored settings

## Recommended implementation order

1. Add `ENABLE_MIGRATION_TOOL` to `System_BuildConfig.h` defaulting to `ENABLE_HTTP_SERVER`.
2. Update `WebServer_MigrationTool.cpp` outer + inner guards.
3. Update `WebServer_MigrationTool.h` declarations.
4. Update `System_FirstTimeSetup.cpp` guards (4 sites).
5. Confirm `WebServer_Server.cpp:5246-5247` still compiles (already inside HTTP_SERVER guard).
6. Verify global `server` symbol resolves cleanly under all 4 builds.
7. Build all 4 permutations.
8. Run the runtime test sequence above on the (0,1) config to prove the recovery path works without the regular web UI.

## Out of scope (deliberately)

- Streaming `/api/settings/schema` (separate `SCHEMA_STREAMING_TODO.md`)
- HTTPS-only restore mode without HTTP-fallback
- An entirely separate "recovery firmware image" — that's a different
  product decision; this refactor just makes it possible.
- Migration Tool client-side changes — unaffected since the wire protocol
  doesn't change.
