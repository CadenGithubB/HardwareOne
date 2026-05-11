# Task-Local Auth Identity Refactor — Plan

> **Status: Stage 1 LANDED.** This document was written as a forward-looking
> plan and remains accurate as a reference. The "Stage 1 — what shipped"
> addendum at the very bottom records the actual landing, deviations from
> the plan, and follow-on stages still pending. Read that addendum first if
> you're a future session looking for current state.

## Executive summary

The codebase stores per-task identity (`gExecAuthContext`, `gExecUser`,
`gExecIsAdmin`) in **shared globals**. Any task that writes to one is silently
mutating the others' view of "the current actor." Recent symptoms:

- Q25 SD-pack worker reading empty/zero auth context and getting PERM DENY for
  files it should be allowed to read.
- `gExecIsAdmin` left at `true` after an admin command finishes — subsequent
  non-admin automation triggers can pass admin-bypass checks in
  `System_Automation.cpp` they should fail. **Real privilege escalation.**

The session that wrote this plan applied save/restore "patches" at every known
writer (`setCurrentCommandContext`, `executeCommand`, hijack tap dispatcher,
live-page/live-text workers, image-probe worker). Those work but they're
discipline-dependent — one new contributor who forgets the dance silently
reopens the bug class.

**The natural fix:** move the three globals into FreeRTOS task-local storage
(TLS). Each task sees only its own identity. Cross-task interference becomes
structurally impossible. All save/restore patches can be deleted.

This document is the full plan. Read it cold; everything you need is here.

---

## What's currently broken (and why patches don't fix it)

### The three siblings

```cpp
// HardwareOne.cpp:381-383
String        gExecUser       = "";    // current actor's username
bool          gExecIsAdmin    = false; // is current actor an admin?
AuthContext   gExecAuthContext;        // full context: user + transport + path + ip + ...
```

All three are read across every task. Every one of these globals is meant to
mean "the actor whose command/operation is being processed right now."

### Why they leak across tasks

ESP32 FreeRTOS schedules multiple tasks across two cores. Tasks that touch
these globals include:
- `cmd_exec_task` (HardwareOne.cpp) — processes the command queue
- `g2_tap_disp` (G2_Glasses.cpp) — handles hijack menu taps
- `g2_page_swap_w` — page swap worker for hijack UI
- `g2_img_probe` — image probe workers (Q25 etc.)
- `g2_ble_connect` — connect worker
- `httpd` — HTTP request handlers
- `wifi`, `BTU_TASK`, `BTC_TASK` — IDF system tasks (may pump callbacks)
- `arduino_events` — Arduino loop
- Live-page/live-text refresh workers
- `cam_pwr`, `cam_stream` — camera lifecycle/streaming workers

When task A writes `gExecAuthContext = X`, task B reading it sees X — even
though X is meant to identify A's current operation, not B's.

### Symptom matrix

| Scenario | Stale value seen by reader | Result |
|---|---|---|
| Admin command finishes, leaves `gExecIsAdmin=true` | Non-admin automation reads `gExecIsAdmin=true` | Bypasses admin check, can override/delete others' automations |
| Internal background command runs with `transport=0 user=""` | G2 hijack worker reads default-zero | VFS PERM DENY (Q25 slime case) |
| User A's command sets identity, before clear, User B's request comes in | Wrong user attribution in audit logs | Log misattribution; possible auth confusion |
| BLE notify callback running on BLE task touches VFS | Identity is whatever the last `cmd_exec` command left | Random pass/deny |

### Why save/restore patches are insufficient

Every patch is one more place a contributor has to remember to:
1. Save the prior value before writing.
2. Restore on EVERY exit path (early returns, exceptions, mid-function bail).
3. Nest correctly with other guards.
4. Get the order right (set-before-use, restore-after-use).

One forgotten guard → silent reintroduction of the bug. The save/restore
patches added in the prior session are listed below in the "Patches to remove"
section. They all become dead code after this refactor.

---

## The target architecture: task-local storage

### One-paragraph summary

Each FreeRTOS task gets its own copy of the three identity values, stored in
its TCB (task control block) via `pvTaskGetThreadLocalStoragePointer` /
`vTaskSetThreadLocalStoragePointer`. Reading "the current actor" returns the
calling task's own slot. Writing only affects the calling task. The OS
guarantees isolation.

### What replaces the globals

A new accessor pattern:

```cpp
// In a new file: System_AuthIdentity.h
const AuthContext& currentAuthContext();   // reads calling task's TLS slot
const String&      currentExecUser();
bool               currentExecIsAdmin();

// Scoped setter — RAII, restores on destruction
class ExecIdentityGuard {
  AuthContext savedCtx;
  String      savedUser;
  bool        savedIsAdmin;
public:
  explicit ExecIdentityGuard(const AuthContext& install);
  ~ExecIdentityGuard();
};
```

Read sites change `gExecAuthContext.user` → `currentAuthContext().user`.
Write sites change raw assignment to constructing an `ExecIdentityGuard` for
the scope they want the identity active.

### Why this is structurally secure

- A task that never installs an identity reads its default (see below) — it
  can't accidentally see another task's identity.
- A task that DOES install via guard restores on scope exit — no leak.
- Concurrent tasks can have completely different identities simultaneously
  without interference.
- New contributors using the type-safe accessor naturally don't trip the bug
  pattern. There's no global to write to.

---

## Security model: default identity per task

This is the core security decision. **Do not default to SYSTEM.**

### The chosen default: ANON, explicit SYSTEM where needed

Each TLS slot is zero-initialized at task creation (`user=""`, `transport=0`),
which `resolveRole` already maps to `FsRole::ANON`. ANON is denied for all
guarded operations (existing behavior; see
`System_Filesystem.cpp:resolveRole`).

**This is fail-closed.** A task that needs to do firmware-internal work
(read/write system files, manage settings, write debug logs to flash, etc.)
**must explicitly install a SYSTEM identity** via a scoped guard at the start
of its work region:

```cpp
void someInternalBackgroundTask(void*) {
  // Identity defaults to ANON — guarded ops would fail here.
  for (;;) {
    {
      ExecIdentityGuard identity(systemAuth("background_poll"));
      // ... do guarded work, identity scoped to this block ...
    }
    vTaskDelay(...);
  }
}
```

### Why NOT default-SYSTEM

The user asked: "Is this hackable in a way that gives people the ability to
run as system?"

If the default were SYSTEM:
- Any task that forgets to install a non-system identity would silently run
  with full filesystem access.
- Attacker-facing paths (BLE notify handlers, HTTP request paths, serial input)
  that miss installing the caller's identity would default to SYSTEM, granting
  the attacker's input full privilege.
- The failure mode is **invisible** — code works fine in dev, exposes data in
  prod.

With ANON default:
- Forgotten identity installs cause **immediate, visible failures** (PERM DENY
  logs).
- Attacker-facing paths that miss auth fail closed.
- The failure mode is **loud** — you see it during testing and fix it.

ANON default is the fail-closed choice that the prior session was advocating;
SYSTEM default would be fail-open. We pick ANON.

### Where SYSTEM gets installed explicitly

Audit during migration: find every task that needs to do firmware work and
install `ExecIdentityGuard(systemAuth("..."))` at the start of its work region.
Candidates (from current grep):
- Boot/init path (`main_task` → `app_main`) before any other task spawns
- Settings persistence path (writeSettingsJson and friends, if not already
  using `VFS::systemAuth("settings.write")` which they do)
- Debug output queue task (`debug_out`) if it writes to flash
- Any task that calls `VFS::*Guarded(SETTINGS_JSON_FILE, ...)` etc.

Note: many sites already use `VFS::systemAuth("...")` as a passed-in context
parameter — those don't read the global at all and need no changes. Only sites
that rely on the GLOBAL being right need explicit installation.

---

## File inventory

### Files that reference the three identity globals (28 files)

These are the files that need conversion to the new accessor API:

```
BLE_Peers.cpp
Bluetooth.cpp
G2_Glasses.cpp
G2_HijackCmd.cpp
G2_HijackCmd.h
G2_Page_Files.cpp
G2_Page_TestSuite.cpp
HardwareOne.cpp                    ← global definitions live here
OLED_Mode_Map.cpp
OLED_Mode_UnifiedMenu.cpp
OLED_RemoteSettings.cpp
OLED_Utils.cpp
System_Automation.cpp              ← reads gExecIsAdmin + gExecUser for auth checks (security-critical)
System_Automation.h                ← extern declarations
System_ESPNow.cpp
System_ESPNow.h
System_ESPSR.cpp
System_EdgeImpulse.cpp
System_FileManager.cpp
System_Filesystem.cpp              ← role-resolution reads
System_MQTT.cpp
System_Maps.cpp
System_Microphone.cpp
System_User.cpp
System_Utils.cpp                   ← executeCommand + ExecAuthContextGuard
System_VFS.h
WebServer_Server.cpp               ← HTTP handler entry points
```

### Files that create FreeRTOS tasks (22 files)

These create tasks; some need explicit SYSTEM identity install at task start
if they perform firmware-internal work that touches guarded resources:

```
G2_Glasses.cpp                     ← g2_tap_disp, g2_page_swap_w, g2_ble_connect, heartbeat
G2_HijackFsm.cpp                   ← FSM worker (may need SYSTEM for state file reads)
G2_Page_Network.cpp                ← network sub-page tasks
G2_Page_TestSuite.cpp              ← g2_img_probe (already captures from caller; switch to TLS)
G2_Ring.cpp                        ← ring connect/poll
HardwareOne.cpp                    ← cmd_exec_task (per-request install)
OLED_Mode_LLM.cpp                  ← LLM mode worker
OLED_Mode_Map.cpp                  ← map loader
System_Camera_DVP.cpp              ← cam_pwr lifecycle worker
System_Camera_Video.cpp            ← video recording worker (writes to /sd, needs SYSTEM)
System_Debug.cpp                   ← debug_out worker (writes log files, needs SYSTEM)
System_ESPNow.cpp                  ← ESPNow tx/rx tasks
System_ESPNow_Sensors.cpp          ← sensor broadcast task
System_ESPSR.cpp                   ← speech recognition worker
System_EdgeImpulse.cpp             ← Edge Impulse inference worker
System_I2C.cpp                     ← I2C bus polling
System_LLM.cpp                     ← LLM streaming
System_Microphone.cpp              ← mic polling/capture
System_TaskUtils.cpp               ← task helpers (audit for default install)
```

---

## New code: System_AuthIdentity.{h,cpp}

### Header (`System_AuthIdentity.h`)

```cpp
// Per-task identity for guarded operations. Each FreeRTOS task gets its own
// copy of (AuthContext, user, isAdmin) stored in its TCB's thread-local
// storage slot. Default is ANON (zero-initialized); tasks that need SYSTEM
// access must install it explicitly via ExecIdentityGuard.
//
// Replaces the old globals gExecAuthContext, gExecUser, gExecIsAdmin.
//
// THREAD SAFETY: each task sees only its own slot. No locks needed.
// Concurrent tasks can have completely different identities simultaneously
// with zero interference.

#pragma once

#include "System_User.h"  // AuthContext, isAdminUser
#include <Arduino.h>      // String

// Read accessors — return the calling task's current identity.
const AuthContext& currentAuthContext();
const String&      currentExecUser();
bool               currentExecIsAdmin();

// Build a SYSTEM identity AuthContext (transport=SOURCE_INTERNAL, user="system").
// Use for firmware-internal work that needs full FS access. Always pair with
// ExecIdentityGuard — never assign directly.
AuthContext systemIdentity(const char* purpose);

// Scoped identity install. Constructor saves prior identity, installs new.
// Destructor restores. Use a stack-scoped instance to bracket the region
// where you want a specific identity active.
class ExecIdentityGuard {
 public:
  explicit ExecIdentityGuard(const AuthContext& install);
  ~ExecIdentityGuard();
  // Non-copyable, non-movable — guard's lifetime equals its scope.
  ExecIdentityGuard(const ExecIdentityGuard&) = delete;
  ExecIdentityGuard& operator=(const ExecIdentityGuard&) = delete;

 private:
  AuthContext savedCtx_;
  String      savedUser_;
  bool        savedIsAdmin_;
};

// Convenience: install SYSTEM for the rest of the current scope.
// Equivalent to ExecIdentityGuard guard(systemIdentity(purpose));
#define SYSTEM_IDENTITY_SCOPE(purpose) \
  ExecIdentityGuard _sysIdentityGuard_##__LINE__(systemIdentity(purpose))

// Initialize TLS slot for the calling task. Idempotent — safe to call
// multiple times. Tasks that don't call this start with ANON default
// (which is fail-closed, the intended behavior).
//
// Typically called only at boot from app_main() so the main task has a
// fallback slot allocated. Worker tasks usually don't need to call this
// because their first ExecIdentityGuard construction allocates the slot
// lazily.
void initAuthIdentityForCurrentTask();
```

### Implementation (`System_AuthIdentity.cpp`)

```cpp
#include "System_AuthIdentity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "System_Debug.h"

// FreeRTOS gives us configNUM_THREAD_LOCAL_STORAGE_POINTERS slots per task.
// ESP-IDF default is 1; sdkconfig.defaults likely needs this raised to >=1.
// Verify: `idf.py menuconfig` → Component config → FreeRTOS → Number of
// thread local storage pointers (target: at least 4 for safety; we use 1).
//
// We use slot 0 by convention. If another subsystem also wants TLS, they
// claim a different index and we coordinate via this comment.
static constexpr BaseType_t kAuthTlsSlot = 0;

struct TaskIdentity {
  AuthContext ctx;        // default-constructed: all-zero, user="", transport=0
  String      user;       // explicit so accessors can return by const ref
  bool        isAdmin = false;
  bool        initialized = false;  // true once we've touched the slot
};

// Per-task identity is allocated on first write. Reads of unallocated slots
// return a static ANON sentinel — fail-closed by design.
static const TaskIdentity& anonSentinel() {
  static const TaskIdentity kAnon{};
  return kAnon;
}

static void deleteIdentity(int /*index*/, void* p) {
  delete static_cast<TaskIdentity*>(p);
}

static TaskIdentity* getOrCreateSlot() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  if (!slot) {
    slot = new TaskIdentity{};
    slot->initialized = true;
    // Register the deleter so the slot is freed when the task is deleted.
    vTaskSetThreadLocalStoragePointerAndDelCallback(
        self, kAuthTlsSlot, slot, deleteIdentity);
  }
  return slot;
}

static const TaskIdentity* getSlotReadOnly() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return &anonSentinel();  // pre-scheduler context
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  return slot ? slot : &anonSentinel();
}

const AuthContext& currentAuthContext() { return getSlotReadOnly()->ctx; }
const String&      currentExecUser()    { return getSlotReadOnly()->user; }
bool               currentExecIsAdmin() { return getSlotReadOnly()->isAdmin; }

AuthContext systemIdentity(const char* purpose) {
  AuthContext ctx;
  ctx.transport = SOURCE_INTERNAL;
  ctx.user      = "system";
  ctx.path      = String("/system/") + (purpose ? purpose : "?");
  ctx.ip        = "internal";
  ctx.sid       = "";
  ctx.opaque    = nullptr;
  return ctx;
}

ExecIdentityGuard::ExecIdentityGuard(const AuthContext& install) {
  TaskIdentity* slot = getOrCreateSlot();
  savedCtx_     = slot->ctx;
  savedUser_    = slot->user;
  savedIsAdmin_ = slot->isAdmin;
  slot->ctx     = install;
  slot->user    = install.user;
  slot->isAdmin = isAdminUser(install.user);
}

ExecIdentityGuard::~ExecIdentityGuard() {
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(),
                                         kAuthTlsSlot));
  if (!slot) return;  // shouldn't happen — ctor created it
  slot->ctx     = savedCtx_;
  slot->user    = savedUser_;
  slot->isAdmin = savedIsAdmin_;
}

void initAuthIdentityForCurrentTask() { (void)getOrCreateSlot(); }
```

### sdkconfig check

Before this builds: verify `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 1`
(default is 1 in ESP-IDF; should be fine but verify). If another component
already uses slot 0, bump our `kAuthTlsSlot` to an unused index and add to the
coordinator comment.

---

## Migration steps (execute in this order)

### Phase 0 — Prep (no code changes affecting build)

1. Create `components/hardwareone/System_AuthIdentity.h` and `.cpp` with the
   code above. Make sure it compiles (add to `CMakeLists.txt` if there's an
   explicit source list — most ESP-IDF components glob).
2. Verify `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 1` in `sdkconfig`.
3. Run a build. The new file should compile but be unreferenced.

### Phase 1 — Make the new API the source of truth

In `HardwareOne.cpp`, change the three globals to redirect to TLS:

```cpp
// OLD:
// String        gExecUser = "";
// bool          gExecIsAdmin = false;
// AuthContext   gExecAuthContext;

// NEW: keep extern-visible names as inline forwarders so existing extern
// declarations across the codebase still link. These are READ-ONLY shims —
// any code that assigned to them previously will fail to compile, which is
// exactly the signal we want to find every writer and convert it.

const AuthContext& gExecAuthContext_get() { return currentAuthContext(); }
const String&      gExecUser_get()        { return currentExecUser();    }
bool               gExecIsAdmin_get()     { return currentExecIsAdmin(); }

// Provide the old names as macros for read-only access during migration.
// After migration completes, remove the macros and update all readers to
// call the accessor functions directly.
#define gExecAuthContext (gExecAuthContext_get())
#define gExecUser        (gExecUser_get())
#define gExecIsAdmin     (gExecIsAdmin_get())
```

**This deliberately breaks all writers at compile time.** Every assignment
like `gExecAuthContext = X` now fails because the macro expands to a function
call. The compile errors are your migration TODO list.

### Phase 2 — Convert writers (compile-error-driven)

Build. Each compile error is a writer. For each:

1. Replace the assignment with `ExecIdentityGuard` scoped to the right region.
2. Delete the matching save/restore code (the "patches" the prior session
   added). Specific patches to remove:

   **HardwareOne.cpp:**
   - `gCmdExecPriorAuthContext`, `gCmdExecPriorUser`, `gCmdExecPriorIsAdmin`
     static variables and their save/restore in
     `setCurrentCommandContext`/`clearCurrentCommandContext`. Replace the
     whole pair with a single function that returns an `ExecIdentityGuard`
     instance the caller scope-binds, or refactor cmd_exec_task to declare a
     scoped guard around the per-request work block.
   - The `clearCurrentCommandContext()` call added at line ~865 — keep or
     remove depending on which pattern you choose. (Recommended: refactor to
     scoped guard, remove the function pair entirely.)

   **System_Utils.cpp:**
   - `ExecAuthContextGuard` struct (lines ~2750-2779). Delete entirely.
   - Inside `executeCommand`, replace the manual `gExecUser = ctx.user;` /
     `gExecIsAdmin = isAdminUser(...)` writes and the
     `ExecAuthContextGuard authGuard(ctx);` construction with a single
     `ExecIdentityGuard identity(ctx);`.

   **G2_HijackCmd.cpp:**
   - `G2HijackCtxGuard` ctor/dtor (lines ~167-174). The class can stay as a
     convenience wrapper — change its implementation to use
     `ExecIdentityGuard` internally instead of writing the global directly.
     The API stays the same so callers in `handleHijackMenuTap` etc. don't
     need to change.

   **G2_Glasses.cpp:**
   - Live-page worker (line ~9178): the
     `savedCtx`/`gLivePageOwnerCtx`/restore bracket. Convert to:
     `ExecIdentityGuard identity(gLivePageOwnerCtx); gLivePageBuildFn(...);`
   - Same for live-text workers at lines ~9756, ~9850.
   - The `gLivePageOwnerCtx` / `gLiveTextOwnerCtx` globals stay (they're how
     the captured-at-spawn identity is passed from the spawning task to the
     worker task), but the application-to-`gExecAuthContext` step uses the
     guard.

   **G2_Page_TestSuite.cpp:**
   - `gImgProbeOwnerCtx` capture (line ~1040 in `spawnImgProbeWorker`): keep.
     This is the spawning-task identity captured for the worker.
   - `imgProbeWorkerImpl` save/restore (lines ~962-963, 990, 1012): replace
     with `ExecIdentityGuard identity(gImgProbeOwnerCtx);` scoped to the
     worker body.

### Phase 3 — Convert readers (code clarity)

After all writers are converted, the macros (`#define gExecAuthContext ...`)
still work. But for clarity, do a final pass replacing reads with explicit
accessor calls:

```cpp
// Before:
if (gExecUser == createdBy) { ... }
// After:
if (currentExecUser() == createdBy) { ... }
```

This is a mechanical rename (~28 files). Do it module by module. After all
readers are converted, the macros can be deleted.

### Phase 4 — Audit task-spawning sites for explicit SYSTEM identity

For each task created in the 22 files that spawn tasks, decide:

| Task does | Action |
|---|---|
| Touches guarded VFS (reads/writes settings, logs, photos, etc.) | Install `SYSTEM_IDENTITY_SCOPE("task_name")` at start of task body |
| Only does compute/BLE/networking, no FS | Leave default ANON (will refuse guarded ops if attempted — fail-closed) |
| Handles external input (HTTP, BLE notify, serial command) | Install identity per-request based on the input's auth — should already be doing this via cmd_exec |

Specific tasks confirmed needing SYSTEM:
- `debug_out` worker — writes log files to LittleFS/SD
- `cam_pwr` and `cam_stream` — write photos/videos to SD
- Settings autosave / writeSettingsJson callers
- `g2_page_swap_w` — may render Files page which reads SD

Specific tasks that should NOT install SYSTEM (stay ANON, install per-request
identity for actual work):
- `cmd_exec_task` — installs per-request via the ExecIdentityGuard in
  `executeCommand`
- `httpd` request handlers — install via session lookup at request entry
- `g2_tap_disp` — installs hijack identity via `G2HijackCtxGuard` at the top
  of `handleHijackMenuTap`

### Phase 5 — Verification

After all conversion is done:

1. `grep -rE "\bgExecAuthContext\s*=|\bgExecUser\s*=|\bgExecIsAdmin\s*="
   components/hardwareone/` — should return zero matches outside of
   `System_AuthIdentity.cpp`.
2. `grep -rE "\bgExecAuthContext\b|\bgExecUser\b|\bgExecIsAdmin\b"
   components/hardwareone/` — all remaining references should be the macro
   sites (or after Phase 3, zero references — they're all `currentXxx()` now).
3. Build clean.
4. Run all the test scenarios in the checklist below.

### Phase 6 — Final cleanup

1. Delete the `#define gExecAuthContext`, `#define gExecUser`,
   `#define gExecIsAdmin` macros if you completed Phase 3.
2. Delete the forwarder functions `gExecAuthContext_get` etc. — readers now
   call `currentAuthContext()` directly.
3. Delete the original `extern AuthContext gExecAuthContext;` declarations
   from headers (System_Automation.h, System_ESPNow.h, System_VFS.h,
   G2_HijackCmd.h).
4. Run the grep checks again — zero references to the old names.

---

## Testing checklist

Run these after migration:

### Functional (must still work)

- [ ] Web login as `asd`, run `openwifi` from web UI — succeeds.
- [ ] Pair G2 glasses fresh — `pairedByUser` stamps correctly and persists.
- [ ] Reboot — `pairedByUser` loads from JSON.
- [ ] Tap a hijack menu item (e.g., Network → Disable WiFi) — command runs,
      auth visible in audit log as `asd@display`.
- [ ] Q25 SD pack (slime) — taps slime, frames load, animation plays.
- [ ] Files page on lens — browse `/sd/` and `/system/`, files listed correctly.
- [ ] Camera capture saves to `/sd/photos/` — works.
- [ ] Settings JSON writes survive reboot — write a setting from web, reboot,
      verify it loaded.

### Security (must enforce correctly)

- [ ] Create automation as user A (non-admin). Log in as user B (non-admin),
      try to delete A's automation — REJECTED with ownership error.
- [ ] User A runs `wifiautoreconnect` (admin command) and is rejected (not
      admin). State left should NOT make B's subsequent automation triggers
      see `isAdmin=true`.
- [ ] Tap a lens menu item that triggers a queued command, then immediately
      run an unrelated HTTP request — both audit-log with correct distinct
      identities.
- [ ] Task that doesn't install identity attempts a guarded VFS operation —
      PERM DENY logged (fail-closed).

### Stress (must not race)

- [ ] Wake-word + camera streaming + G2 hijack tap all active concurrently —
      no PERM DENY or wrong-user audit lines.
- [ ] Soak: leave running 1+ hours with periodic taps and heartbeats — no
      identity drift, no auth confusion.

---

## Known gotchas

1. **String copying in TLS struct.** `TaskIdentity` stores `String user`
   by value, not by reference. Each guard construction does a String copy on
   save and another on install. That's intentional — we can't hold a
   reference to a String that lives in the caller's stack frame. Cost: a few
   heap allocations per command. Acceptable.

2. **Pre-scheduler context.** Code that runs before `vTaskStartScheduler()`
   (very early boot) has no task handle. `currentAuthContext()` returns the
   ANON sentinel — anything that needs SYSTEM during early boot must do its
   work via the explicit `VFS::*Guarded(path, systemIdentity("boot"))` form
   that takes the context as a parameter, NOT via the global.

3. **Deferred task local cleanup.** `vTaskSetThreadLocalStoragePointerAndDelCallback`
   handles cleanup when a task is deleted. But many of our tasks are
   persistent and never deleted, so this rarely runs. Verify no `delete`
   leak by spawning + deleting an image-probe worker repeatedly and watching
   heap.

4. **The existing `G2HijackCtxGuard` and the new `ExecIdentityGuard` can
   nest.** That's fine — they both save and restore. Just don't have them
   live in the same scope unless you actually want a double-bracket.

5. **Other globals that share this pattern.** Out of scope for this refactor
   but worth noting:
   - `gCurrentCommandContext` (void*) — pointer not identity; leave alone.
   - `gCLIValidateOnly` — also a per-command flag set by cmd_exec_task; uses
     a local saved-prev pattern that works fine.
   - `gAutoLogActive`, `gInAutomationContext` — different lifecycle (per
     automation run, not per task). Not currently broken; revisit if they
     ever leak across tasks.

---

## Patches the prior session added (to be removed during this refactor)

Reference for the new session — these are the bandaid patches that should
become dead code after TLS lands. Delete during Phase 2.

1. **HardwareOne.cpp** (~line 760-790):
   - `static AuthContext gCmdExecPriorAuthContext;`
   - `static String      gCmdExecPriorUser;`
   - `static bool        gCmdExecPriorIsAdmin = false;`
   - The save block in `setCurrentCommandContext`.
   - The restore block in `clearCurrentCommandContext`.
   - The `clearCurrentCommandContext()` call added in cmd_exec_task loop
     (~line 865) — keep or replace with scoped guard depending on chosen
     refactor pattern for that function.

2. **System_Utils.cpp** (~line 2750-2779):
   - The entire `struct ExecAuthContextGuard { ... };` definition.
   - Inside `executeCommand`: the `ExecAuthContextGuard authGuard(ctx);` line.
     Replace the whole `gExecUser = ctx.user; ... gExecIsAdmin = ...;
     ExecAuthContextGuard ...` block with `ExecIdentityGuard identity(ctx);`.

3. **G2_Page_TestSuite.cpp** (~line 962-1012):
   - The `AuthContext savedCtx = gExecAuthContext; gExecAuthContext =
     gImgProbeOwnerCtx;` save/install at the top of `imgProbeWorkerImpl`.
   - The matching restores at the early-return and end of function.
   - Replace with a single `ExecIdentityGuard identity(gImgProbeOwnerCtx);`
     at the top of the function. RAII handles all return paths.

4. **G2_Glasses.cpp** — three save/restore blocks for live workers
   (lines ~9178, ~9756, ~9850). Each converts to a scoped
   `ExecIdentityGuard identity(gLivePage/TextOwnerCtx);` around the buildFn
   call.

5. **G2_HijackCmd.cpp** (~line 167-174):
   - `G2HijackCtxGuard::G2HijackCtxGuard()` and `~G2HijackCtxGuard()`. Keep
     the class but reimplement to wrap `ExecIdentityGuard` internally:
     ```cpp
     class G2HijackCtxGuard {
       ExecIdentityGuard guard_;
      public:
       G2HijackCtxGuard() : guard_(g2HijackAuthContext()) {}
     };
     ```
     Same external API, new internals. Callers (handleHijackMenuTap etc.)
     don't change.

---

## Estimated effort

- Phase 0 (new file): 30 min
- Phase 1 (forwarder + macros): 15 min — but immediately breaks build
- Phase 2 (writer conversion, compile-driven): 2–3 hours; ~10 distinct write
  sites across 5 files; each converts to a guard scope.
- Phase 3 (reader rename): 1 hour mechanical work across ~22 files.
- Phase 4 (audit task SYSTEM installs): 1–2 hours; need to look at each task
  and decide.
- Phase 5 (verify): 30 min grep + smoke build.
- Phase 6 (cleanup): 30 min.

**Total: ~half a day.** Worth it.

---

## Out of scope for this refactor (separate work)

- **Command auth tightening.** The prior session noted that
  `authorizeCommand` lets non-admin commands run with empty `user`. After
  this refactor, empty `user` is structurally rarer (only happens for tasks
  that didn't install identity), but the `authorizeCommand` logic is still
  the same. If you want to require all commands to have a non-empty user,
  modify `authorizeCommand` to reject `ctx.user.length() == 0` regardless of
  `requiresAdmin`. Do that AFTER this refactor proves stable so you can A/B
  the changes.

- **Pair-time identity enforcement.** `bleStampPairedByIfBlank` silently
  no-ops if there's no current user. After this refactor, that helper will
  read `currentAuthContext().user` which is ANON in the boot-reconnect case.
  If you want pairing to error rather than silently no-op, change the helper
  to return false and have the caller surface a "no current user — log in
  first to claim this device" message.

- **Make `gExecPathPaging`, `gAutoLogActive`, `gInAutomationContext`
  task-local too.** They have the same pattern but are less security-
  critical. Worth a follow-up audit.

---

---

# Audit addendum — exhaustive cross-check

The first draft of this plan was based on a 28-file grep. A deeper second-pass
audit found:

- **222 total references** to the three globals across 27 source files
- **52 distinct read sites** after filtering out comments and extern declarations
- **A second call site for `setCurrentCommandContext`** that the prior session
  missed entirely
- **A second class of related globals** (`gNotificationContext`,
  `gCurrentAutomationUser`, `gCLIValidateOnly`, `gAutoLogActive`,
  `gInAutomationContext`, etc.) that suffer the same task-leak vulnerability
  pattern but for non-auth state
- **Existing code comments that already acknowledge "gExecAuthContext may
  have stale identity"** — workarounds were already being threaded through
  call signatures in `WebServer_Server.cpp` to avoid the global

The audit doesn't invalidate the plan; it expands the scope and adjusts the
effort estimate.

---

## A. Hidden caller missed in the first draft

### `submitAndExecuteSync` early-boot fallback path

**Location:** `System_Utils.cpp:3025-3046`

There is a SECOND caller of `setCurrentCommandContext` outside the
cmd_exec_task loop. When the executor queue isn't ready (very early boot
before `gCmdExecQ` is initialized), `submitAndExecuteSync` does a synchronous
direct call:

```cpp
if (gCmdExecQ == nullptr) {
  char* outBuf = (char*)ps_alloc(2048, ...);
  setCurrentCommandContext(cmd.ctx);          // ← installs identity
  extern void* gCurrentCommandContext;
  gCurrentCommandContext = (void*)&cmd.ctx;
  bool ok = executeCommand((AuthContext&)cmd.ctx.auth, cmd.line.c_str(), outBuf, 2048);
  gCurrentCommandContext = nullptr;            // ← only nulls the void*, NOT the identity
  out = outBuf;
  free(outBuf);
  return ok;
}
```

This path **never calls `clearCurrentCommandContext()`**, so the prior session's
save/restore patches in `setCurrentCommandContext`/`clearCurrentCommandContext`
don't run at all here. The early-boot path silently leaks the command's identity
to whatever runs next.

**Action:** during Phase 2, this site MUST be converted to use
`ExecIdentityGuard` scoped around the `executeCommand` call. Eliminates the
leak structurally.

---

## B. Related task-leak globals (out of original scope, in same bug class)

These are NOT auth identity, but they have the **same shape of bug**: shared
mutable global representing "current operation context," written by one task
and observable by another. If the TLS pattern is right for auth, it's right
for these too.

The new session should decide whether to do these in the same refactor or as
a follow-up. My recommendation: **do them all together** to land the
architecture change cleanly. Adding TLS-aware patterns for some globals while
leaving others as shared globals creates a confusing mixed model.

### B.1 — `gNotificationContext` (System_Notifications.cpp:36)

```cpp
struct {
  uint8_t source;
  char    subsource[24];
} gNotificationContext = { NOTIF_SOURCE_UNKNOWN, "" };
```

- Set by `setNotificationContext(source, subsource)` (line ~38)
- Cleared by `clearNotificationContext()` — **resets to UNKNOWN, doesn't
  restore prior**
- Read by `oledNotificationAdd(...)` and other notification dispatchers
  (lines ~74-75)
- Wrapped by `NotificationContextGuard` RAII (System_Notifications.h:49)

**Two bugs:**
1. Not task-local — cross-task interference identical to the auth case
2. Destructor clears instead of saves/restores — nested guards within the
   same task break (inner guard's destructor clears the outer guard's state)

This is the likely upstream cause of the **ArduinoJson::TextFormatter
LoadProhibited crashes** documented in the current todo list. The notification
struct is being serialized while one task's notification context is stale data
from another task's just-finished operation. `subsource` is a char buffer; if
it gets clobbered mid-serialize, you get exactly the
`writeString` null-deref / freed-pointer crash signature observed.

**Fix:** identical to auth globals — make `gNotificationContext` a per-task
TLS slot, make `NotificationContextGuard` save-and-restore not clear.

### B.2 — `gCurrentAutomationUser` (System_Automation.cpp:68)

```cpp
static String gCurrentAutomationUser;
```

Set during automation execution (line ~800), cleared at end (line ~821). Read
across tasks at line 1717 by ownership-check code:

```cpp
if (!gExecIsAdmin && gCurrentAutomationUser != gExecUser) { deny; }
```

This compares two task-leak-prone globals against each other. If either is
stale, the check is wrong. Concurrent automations on different cores cause
race conditions in ownership enforcement.

**Action:** make per-task. Use the same TLS slot mechanism, different field.

### B.3 — `gCLIValidateOnly` (HardwareOne.cpp:187)

```cpp
bool gCLIValidateOnly = false;
```

Set in cmd_exec_task per-command (the prior session noted "uses local
saved-prev pattern that works fine" — but on further inspection it's only
saved for the single command's duration, not across tasks). Read by every
command handler via `RETURN_VALID_IF_VALIDATE_CSTR()`.

**Audit:** cmd_exec_task is single-threaded, and validate-only mode is set
per-command via local save/restore. The risk is if any OTHER task reads
`gCLIValidateOnly` — they'd see the cmd_exec task's transient state.

```bash
grep -rn 'gCLIValidateOnly' components/hardwareone/ | wc -l
# 30 references across multiple files; mostly the RETURN_VALID_IF_VALIDATE_CSTR
# macro expansion. Some explicit reads in System_Automation.cpp.
```

**Action:** make per-task. Lower urgency than auth (single-threaded usage in
practice) but include in the refactor for uniform pattern.

### B.4 — Automation state cluster

- `gAutoLogActive` (System_Automation.cpp:107)
- `gAutoLogFile` (referenced in System_Automation.h:100)
- `gAutoLogAutomationName` (System_Automation.h:101)
- `gInAutomationContext` (System_Automation.cpp:166)
- `gAutosDirty` (System_Automation.h:96)

All set during a single automation's execution, read across tasks. If two
automations run concurrently (which can happen via parallel triggers), they
race.

**Action:** evaluate whether automations can actually run concurrently. If
no (single scheduler thread), they're safe-by-construction even as globals.
If yes, they need TLS or per-automation structs.

### B.5 — Output capture buffers

- `gCmdCaptureBuf` (referenced in HardwareOne.cpp:835)
- `gCmdCaptureLen`
- `gCmdCaptureCap`

Set in cmd_exec_task when a command requests output capture. Used by
`broadcastOutput` to redirect output to a buffer instead of the wire.

**Audit:** these are written and read only on the cmd_exec task. The hazard
is if `broadcastOutput` is called from another task while capture is active —
output would be captured into the wrong buffer or skipped entirely.

`broadcastOutput` is called from many tasks (status updates, debug logs,
etc.). If a non-cmd_exec task happens to call it while cmd_exec has a
capture active, the buffer is wrong.

**Action:** make per-task. Capture is inherently per-request; the global is
wrong by design.

### B.6 — Per-transport auth-state globals (HardwareOne.cpp:314-318)

- `gSerialAuthed`, `gSerialUser` — serial CLI authentication state
- `gLocalDisplayAuthed`, `gLocalDisplayUser` — OLED display auth state

These are arguably correct as globals because they represent "is the SERIAL
PORT logged in" and "is the OLED LOGGED IN" — transport-level, not task-level
state. A single serial port has one auth state.

**Audit:** OK as-is, but verify only the serial command handler reads/writes
`gSerialAuthed`, and only the OLED handler reads/writes
`gLocalDisplayAuthed`. If cross-transport reads exist, they're suspicious.

**Action:** likely no change. Document the invariant.

### B.7 — `gCurrentCommandContext` (HardwareOne.cpp:748)

```cpp
void* gCurrentCommandContext = nullptr;
```

A void pointer to the current `CommandContext` struct. Set by
`setCurrentCommandContext`, nulled by `clearCurrentCommandContext` and at
inline sites.

**Audit:** the previous plan said "leave alone — it's transient pointer and
nullptr is safe." On second look: it's read by `getCurrentCommandOutputMask()`
(HardwareOne.cpp:750), which is called by `broadcastOutput()`. Cross-task
reads of this pointer mean a broadcast from task A can read the
output-routing of task B's command. The result is wrong-route broadcasts
(messages going to the wrong WebSocket session or wrong serial line).

**Action:** make per-task. Same TLS slot, different field.

---

## C. Auth-checking helper functions — confirmed correct, no changes needed

These take `AuthContext` (or `String user`) as an explicit parameter. They do
NOT read the global. They're fine as-is.

| Function | Signature | Source |
|---|---|---|
| `isAdminUser(const String& who)` | takes user | System_User.cpp:155 |
| `commandRequiresAdmin(const String& line)` | takes command line | System_Utils.cpp:1901 |
| `hasAdminPrivilege(const AuthContext& ctx)` | takes ctx | (call sites in authorizeCommand) |
| `authorizeCommand(const AuthContext& ctx, ...)` | takes ctx | System_Utils.cpp:2718 |
| `resolveRole(const AuthContext& ctx)` | takes ctx | System_Filesystem.cpp:884 |
| `canRead/Write/Create/Delete/Rename(path, ctx)` | take ctx | System_Filesystem.cpp |
| `VFS::existsGuarded(path, ctx)` | takes ctx | System_VFS.cpp:794 |
| `VFS::openGuarded(path, mode, ctx)` | takes ctx | System_VFS.cpp |
| `VFS::*Guarded` family | takes ctx | System_VFS.cpp |
| `canImport(path, ctx)` | takes ctx | System_Filesystem.cpp |
| `getDirPerms(path, ctx)` | takes ctx | System_Filesystem.cpp |
| `logFsAccessDeny(path, ctx, ...)` | takes ctx | System_Filesystem.cpp:971 |

**Architectural observation:** the auth/permission layer was designed
correctly — every check takes context as a parameter. The bug is purely at
**callers** that read the global instead of plumbing context through.

This makes the migration mechanical. Every read of `gExecAuthContext` is a
caller bug to fix; the callee already does the right thing.

---

## D. Comments in existing code that already flag this bug

Found during the second-pass audit. These are not exhaustive but show that
this problem was known and worked around piecemeal:

**WebServer_Server.cpp:2053** —
> `canImport(path) using gExecAuthContext leak, and a separate isAdminOnlyPath gate — three branches that could disagree. Now one decision point.`

Translation: the developer noticed `gExecAuthContext` was leaking, refactored
this specific function to pass ctx through. Did NOT do the codebase-wide fix.

**WebServer_Server.cpp:3812** —
> `path-only call used gExecAuthContext which may have stale identity.`

Same pattern — local workaround, didn't do the systemic fix.

**G2_Glasses.cpp:9079, 9175** —
> `bracket because gExecAuthContext is a single global shared across tasks — leaving our value installed would corrupt other readers.`

The live-page worker author understood the problem. Added save/restore.
Didn't do the systemic fix.

**G2_Page_Files.cpp:592** —
> `gExecAuthContext is whatever was last set (often empty on a fresh boot...)`

The Files page author also knew.

**System_MQTT.cpp:1316** —
> `CLI handler: dispatch sets gExecAuthContext to the caller's identity. The caller (web admin or serial) needs read perm on the path they're pointing MQTT at.`

This one ASSUMES the global is correct at the read site. With my findings,
it's correct only when no other task has stomped it since the dispatch.
Subtle bug that won't fire often but exists.

**The pattern:** every contributor who got close to this bug noticed it,
fixed their immediate site, and moved on. Nobody did the systemic fix. This
refactor IS that systemic fix.

---

## E. Read sites that go through `VFS::*Guarded(path, gExecAuthContext)`

These are mechanical — replace `gExecAuthContext` with `currentAuthContext()`.
After Phase 1 macros are removed (Phase 6), the explicit accessor call makes
the dependency obvious.

Files with VFS guarded calls that read the global:

| File | Read site count |
|---|---|
| System_Maps.cpp | 14 |
| System_ESPNow.cpp | 5 |
| System_Microphone.cpp | 7 |
| System_EdgeImpulse.cpp | 2 |
| G2_Glasses.cpp | 3 |
| G2_Page_Files.cpp | 1 |
| G2_Page_TestSuite.cpp | 2 |
| G2_HijackCmd.cpp | 1 (the guard ctor) |
| System_MQTT.cpp | 1 |
| System_Maps.cpp | (see above) |
| OLED_Mode_UnifiedMenu.cpp | 1 |
| OLED_Utils.cpp | 1 |
| OLED_RemoteSettings.cpp | (audit needed) |
| OLED_Mode_Map.cpp | (audit needed) |
| Bluetooth.cpp | 1 (the extern, may not actually read) |

Total: ~40+ read sites. Each is a one-line replacement.

---

## F. Read sites that look at non-`gExecAuthContext` fields of the global

These need slightly different treatment because they read specific fields:

- **System_ESPNow.cpp:9312** — `if (!gExecAuthContext.ip.startsWith("espnow:"))`
  — checks the ip field for "is this a remote ESP-NOW request." Need to
  ensure ESP-NOW request handlers install ctx via `ExecIdentityGuard` with
  `ip = "espnow:<mac>"` so this check still works after the refactor.

- **System_User.cpp:1649, 3005** — read `gExecAuthContext.user` for
  "createdBy" stamps and admin verification. These convert mechanically to
  `currentAuthContext().user` or `currentExecUser()`.

- **System_Utils.cpp:2768-2770** — the prior session's save block. Delete
  during Phase 2.

- **System_Automation.cpp:1090, 1376, 1455, 1717, 1726** — read `gExecUser`
  + `gExecIsAdmin` for ownership/admin checks. These are the
  **security-critical** reads. Conversion to `currentExecUser()` /
  `currentExecIsAdmin()` is mechanical, but verify the call paths above
  these install identity correctly before reaching the check.

---

## G. Revised effort estimate

The first-draft estimate of "half a day" assumed only the three identity
globals. With the related globals included:

| Phase | Original est. | Revised est. |
|---|---|---|
| 0 — new System_AuthIdentity.{h,cpp} | 30 min | 1 hr (more careful TLS slot mgmt) |
| 1 — forwarder + macros | 15 min | 30 min |
| 2 — convert writers (compile-driven) | 2-3 hr | 3-4 hr (include early-boot fallback) |
| 3 — convert readers (rename to accessor) | 1 hr | 2 hr (more files, more sites) |
| 4 — audit task SYSTEM installs | 1-2 hr | 2-3 hr (include capture buffer tasks) |
| **Related globals B.1-B.7** | — | 4-6 hr (notification ctx, automation, capture buffers) |
| 5 — verification | 30 min | 1 hr (more test cases) |
| 6 — cleanup | 30 min | 30 min |
| **Total** | ~half day | **~1.5-2 days** of focused work |

Breaking by scope:
- **Auth-only refactor** (the three identity globals, sections in the
  original plan): ~1 day
- **Related globals included** (B.1-B.7 in this addendum): +0.5-1 day
- **Total architecturally consistent landing**: 1.5-2 days

---

## H. Suggested sequencing options

### Option 1: All-at-once landing (user's original ask)

Single refactor lands TLS pattern for auth + notification + automation +
capture state. 1.5-2 days. Clean architecture, one PR.

**Risk:** large blast radius. If something breaks, hard to bisect.

### Option 2: Three-stage landing

Stage 1 — auth identity only (the three primary globals). Half-day.
Stage 2 — notification context (likely fixes ArduinoJson crashes too).
  Half-day.
Stage 3 — automation state + capture buffers. Half-day.

Each stage independently testable. Slower overall, safer.

**Recommendation:** Option 2. The notification context fix in Stage 2 may
unblock the existing JSON crash todo (the addendum item B.1 hypothesizes
this is the upstream cause). That alone is worth doing as a discrete change
so the impact is measurable.

---

## I. Migration safety considerations

### I.1 — Phase 1 macro trick breaks the build globally

Phase 1 in the original plan recommends `#define gExecAuthContext
(gExecAuthContext_get())` to force compile errors at every writer. This
**makes the build unbuildable until Phase 2 completes**. No bisection
possible during that window.

**Alternative:** keep the original global definitions during Phases 0-3 but
add the TLS-backed accessors. Convert writers to ExecIdentityGuard one file
at a time, building between each. Only after all writers are converted,
flip the global to read-only / remove it.

This is more incremental but safer. Recommended for a refactor of this size.

### I.2 — Auth identity during static init / pre-scheduler

Code that runs before `vTaskStartScheduler()` (very early `app_main`, static
constructors) has no FreeRTOS task and therefore no TLS slot.
`currentAuthContext()` must return a safe default (the ANON sentinel).

Any code in this window that wants to do FS access MUST use the explicit
`VFS::*Guarded(path, systemAuth("boot"))` form that takes context as
parameter. None of this code should rely on the global. Audit the boot path
for any global reads — likely zero, but verify.

### I.3 — Idle and Timer tasks

ESP-IDF spawns IDLE0, IDLE1, Tmr Svc, ipc0, ipc1 etc. These tasks don't
install identity. Anything they call into that reads `currentAuthContext()`
gets ANON.

**Action:** verify nothing in idle/timer callback paths reads guarded VFS
or auth state without explicit ctx parameter. Most of the codebase already
threads ctx through; this is just due diligence.

### I.4 — TLS slot management

The plan uses slot 0. Need to verify no other component already claims slot
0. Check `pvTaskGetThreadLocalStoragePointer(NULL, 0)` returns NULL early
in boot before our code runs — if not, something else owns it.

Also: `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS` in sdkconfig must be
>= 1. ESP-IDF default is 1. If we ever want a second TLS user (e.g., the
related globals each in their own slot for cleaner ownership), we'd need to
raise this. Recommend raising to 4 preemptively to leave headroom.

### I.5 — Heap pressure

Each task that constructs an `ExecIdentityGuard` does at minimum:
- One small heap allocation for the `TaskIdentity` struct (~64 bytes
  including the `String user` field's internal buffer)

This happens once per task lifetime (first guard construction allocates;
subsequent reuse the slot). So total heap impact is ~64 bytes × number of
tasks that ever construct a guard. Bounded and small.

### I.6 — Pre-scheduler init order

`initAuthIdentityForCurrentTask()` doesn't need to be called from
`app_main` because the lazy-init in `getOrCreateSlot()` handles it. But
calling it explicitly at the top of `app_main` is good documentation: it
makes the main task's slot allocation explicit instead of "happens on
first guard construction sometime later."

---

## J. Files to add this refactor to (final exhaustive list)

### Files needing global → accessor conversion (read sites only)

After all writers are converted to `ExecIdentityGuard`, these files contain
read-only references to the globals that need to be converted to
`currentXxx()` accessor calls. The macros (Phase 1) keep them working
through Phases 2-3; Phase 6 removes the macros and these become explicit
accessor calls.

```
BLE_Peers.cpp           (1 ref — comment only)
Bluetooth.cpp           (1 ref — extern only, not actually used? audit)
G2_Glasses.cpp          (~15 refs — comments + 3 reads + 6 writes for live workers)
G2_HijackCmd.cpp        (2 refs — guard ctor/dtor)
G2_HijackCmd.h          (1 ref — extern + class definition)
G2_Page_Files.cpp       (2 refs — canRead + comment)
G2_Page_TestSuite.cpp   (7 refs — picker enum + worker + 3 patches to remove)
HardwareOne.cpp         (definitions + cmd_exec_task — major writer)
OLED_Mode_Map.cpp       (audit needed)
OLED_Mode_UnifiedMenu.cpp (1 — manifest read)
OLED_RemoteSettings.cpp (audit needed)
OLED_Utils.cpp          (1 — existsGuarded read)
System_Automation.cpp   (8+ refs — ownership/admin checks — SECURITY CRITICAL)
System_Automation.h     (1 — extern declaration)
System_ESPNow.cpp       (~7 refs — VFS reads + IP check + extern)
System_ESPNow.h         (1 — extern)
System_ESPSR.cpp        (1 — extern + executeCommand call)
System_EdgeImpulse.cpp  (2 — VFS reads)
System_FileManager.cpp  (1 — canDelete read)
System_Filesystem.cpp   (1 — resolveRole, takes ctx as param)
System_MQTT.cpp         (2 — existsGuarded + extern + executeCommand caller)
System_Maps.cpp         (14 — VFS reads, all takes ctx)
System_Microphone.cpp   (7 — VFS reads)
System_User.cpp         (2 — admin user creation, sync admin verify — SECURITY CRITICAL)
System_Utils.cpp        (major — executeCommand, ExecAuthContextGuard, etc.)
System_VFS.h            (1 — extern only)
WebServer_Server.cpp    (multiple — explicit ctx already threaded; just extern decls and comments)
```

### Files where SYSTEM identity needs explicit install (Phase 4)

These spawn tasks that do firmware-internal work and need `SYSTEM_IDENTITY_SCOPE`
at the top of their task body. Audit each in Phase 4:

```
System_Debug.cpp                    debug_out — writes log files (NEEDS SYSTEM)
System_Camera_DVP.cpp               cam_pwr — camera lifecycle, may write photos (LIKELY)
System_Camera_Video.cpp             video recording — writes /sd (NEEDS SYSTEM)
System_Microphone.cpp               mic polling — writes recordings (LIKELY)
HardwareOne.cpp                     cmd_exec_task — per-request install (NOT system default)
G2_Glasses.cpp                      g2 worker tasks — install hijack identity per-tap (NOT system default)
G2_Page_TestSuite.cpp               img_probe — captures from spawner (NOT system default)
G2_HijackFsm.cpp                    fsm worker — audit needed
System_TaskUtils.cpp                helpers — audit needed
System_ESPNow.cpp                   esp-now tasks — install per-request (NOT system default)
System_I2C.cpp                      i2c polling — likely no VFS, leave ANON
System_LLM.cpp                      LLM streaming — may write logs (LIKELY)
System_EdgeImpulse.cpp              EI inference — model file reads (LIKELY)
System_ESPSR.cpp                    SR worker — model file reads (LIKELY)
OLED_Mode_LLM.cpp                   LLM mode — may write logs (LIKELY)
OLED_Mode_Map.cpp                   map loader — reads /maps (NEEDS SYSTEM)
G2_Ring.cpp                         ring connect/poll — audit needed
G2_Page_Network.cpp                 network sub-page — audit needed
System_ESPNow_Sensors.cpp           sensor broadcast — audit needed
```

### Files in scope ONLY for related globals (B.1-B.7)

If doing the all-at-once landing:

```
System_Notifications.cpp / .h       gNotificationContext refactor
System_Automation.cpp / .h          gCurrentAutomationUser, gAutoLog*, gInAutomationContext
```

---

## K. Concrete checklist for the new session

Pre-flight:
- [ ] Confirm `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 4` in
      sdkconfig (recommend raising to 4 for headroom even though we use 1).
- [ ] Confirm slot 0 is not in use by any other component
      (grep `pvTaskGetThreadLocalStoragePointer\|vTaskSetThreadLocal` in
      managed_components/).
- [ ] Confirm there is no pthread/C++11 thread_local usage in the codebase
      that conflicts (`grep -rn 'thread_local' components/hardwareone/`).

Phase 0:
- [ ] Create `System_AuthIdentity.h` + `.cpp` from the templates in this doc.
- [ ] Add to CMakeLists.txt if explicit source list (verify component uses
      `idf_component_register(SRCS ...)` vs glob).
- [ ] Build clean — file should compile but be unreferenced.

Phase 1 — choose ONE migration approach:
- [ ] **Aggressive (compile-driven)**: macro `#define gExecAuthContext
      (currentAuthContext())` etc. Build breaks at every writer. Work
      through errors one by one.
- [ ] **Conservative (incremental)**: leave globals as-is. Add accessor
      functions alongside. Convert writers file-by-file, building between
      each. Globals removed at the end.

Phase 2 — convert writers (in this order — least-dependent first):

- [ ] G2_Page_TestSuite.cpp:962-1012 — replace save/restore with
      `ExecIdentityGuard identity(gImgProbeOwnerCtx)`. Remove patches.
- [ ] G2_HijackCmd.cpp:167-174 — reimplement G2HijackCtxGuard using
      ExecIdentityGuard internally. External API unchanged.
- [ ] G2_Glasses.cpp lines 9178, 9756, 9850 — replace save/restore with
      ExecIdentityGuard scoped around buildFn calls.
- [ ] System_Utils.cpp:2750-2779 — delete ExecAuthContextGuard. Replace
      `gExecUser = ...; gExecIsAdmin = ...; ExecAuthContextGuard ...` with
      single `ExecIdentityGuard identity(ctx)`.
- [ ] HardwareOne.cpp:776-792 — delete gCmdExecPriorAuthContext/User/IsAdmin
      statics. Refactor setCurrentCommandContext/clearCurrentCommandContext
      OR replace their usage in cmd_exec_task with a scoped guard around
      the per-request work block.
- [ ] HardwareOne.cpp:870 — verify clearCurrentCommandContext() call is
      replaced or removed depending on refactor pattern.
- [ ] System_Utils.cpp:3025-3046 — the `submitAndExecuteSync` early-boot
      direct-call path. Add ExecIdentityGuard around the executeCommand
      call.

Phase 3 — convert readers (mechanical rename):
- [ ] All 40+ read sites changed from `gExecAuthContext` → `currentAuthContext()`.
- [ ] All `gExecUser` → `currentExecUser()`.
- [ ] All `gExecIsAdmin` → `currentExecIsAdmin()`.
- [ ] All extern declarations removed (the `extern AuthContext
      gExecAuthContext;` lines scattered across ~20 files).

Phase 4 — Phase 4: SYSTEM_IDENTITY_SCOPE installs in worker tasks (see list above).

Phase 5 — verification:
- [ ] Functional checklist from original plan (slime works, hijack commands
      work, automations work, files browser works).
- [ ] Security checklist (non-admin can't override admin's automation;
      stale admin context doesn't leak).
- [ ] Stress: concurrent hijack tap + camera stream + automation trigger
      — no PERM DENY, no wrong-user audit lines.
- [ ] grep verification: zero `gExecAuthContext`, `gExecUser`,
      `gExecIsAdmin` references remain (except inside the new
      System_AuthIdentity.{h,cpp}).

Phase 6 — related globals (if doing all-at-once):
- [ ] System_Notifications: gNotificationContext → TLS.
  NotificationContextGuard reimplements to save-and-restore not clear.
- [ ] System_Automation: gCurrentAutomationUser, gAutoLog*, gInAutomationContext
  → TLS or per-automation struct.
- [ ] Output capture: gCmdCaptureBuf/Len/Cap → TLS or per-request struct.
- [ ] gCurrentCommandContext (void*) → TLS field on same slot.

Phase 7 — cleanup:
- [ ] Delete forwarder macros from HardwareOne.cpp.
- [ ] Delete original global definitions.
- [ ] Final grep verification.
- [ ] Doc update: the comments in WebServer_Server.cpp, G2_Glasses.cpp etc.
      that reference "stale gExecAuthContext" can be deleted or rewritten
      since the bug class no longer exists.

---

## L. What this refactor does NOT cover (separate work items)

Listed here so the new session doesn't accidentally scope-creep:

- **Tightening `authorizeCommand` to reject empty user.** Separate change
  per architectural discussion. Do AFTER TLS lands and proves stable.
- **Pair-time identity enforcement.** `bleStampPairedByIfBlank` silently
  no-ops. Separate change.
- **The ArduinoJson::TextFormatter LoadProhibited crash investigation.**
  Stage 2 (notification context refactor) may incidentally fix this. The
  existing todo to investigate `serializeJson` call sites for null/freed
  field reads remains valid — do it after Stage 2 lands so you can A/B
  whether the crash is gone.
- **The `static bool gInAutomationContext = false;` in System_Automation.h:122
  is INSIDE the `#else !ENABLE_AUTOMATION` block** — it's a stub for when
  automation is compiled out, NOT a header-level definition bug. Don't
  confuse with the canonical definitions in System_Automation.cpp.

---

## When this is done, the system has these properties

- Each task's identity is **structurally isolated** from every other task's.
- No save/restore discipline required from contributors. Writing the wrong
  thing produces a compile error (Phase 1's deliberate breakage), not a
  silent leak.
- Default identity is **ANON** (fail-closed). Tasks that need elevated
  access install it **explicitly** via scoped guards.
- The Q25 PERM DENY bug is structurally impossible.
- The "stale `gExecIsAdmin=true` lets non-admin override admin" privilege
  escalation is structurally impossible.
- All current save/restore patches become dead code and are deleted.
- Future async workers, page renders, or background tasks naturally get the
  right behavior — no new bug class to invent.

The auth identity model becomes a property of the FreeRTOS task, not a
shared mutable global. Same as how every other modern system handles it.

---

# Stage 1 — what shipped

## Summary

Stage 1 (the three auth-identity globals — `gExecAuthContext`, `gExecUser`,
`gExecIsAdmin`) is complete and tested on hardware. The verification grep
returns zero matches outside `System_AuthIdentity.{h,cpp}`:

```
grep -rE "\bgExecAuthContext\s*=|\bgExecUser\s*=|\bgExecIsAdmin\s*=" components/hardwareone/
# (zero matches)
```

The legacy globals are deleted. `ExecIdentityGuard` is the only writer.
`currentAuthContext()` / `currentExecUser()` / `currentExecIsAdmin()` are
the only readers.

Validated paths on hardware:
- Web login + camera commands (FPS, exposure, brightness/contrast).
- G2 pairing + auto-reconnect; `pairedByUser` persists across reboot.
- Hijack menu navigation (Status → live text, Tests → Image probes).
- Q25 SD-pack animation: 80 frames of `/sd/g2_icon_animations/slime/`
  shipped end-to-end. This was the original reported PERM DENY symptom;
  it now works structurally because the worker task installs its
  captured identity via `ExecIdentityGuard(gImgProbeOwnerCtx)`.
- Files → /system/users → users.json → Pretty View → multi-page scroll
  → DOUBLE_CLICK exit. (Required the BTC stack/dispatcher fix below.)

## Deviations from the plan

### Slot index: 1, not 0

The plan picked TLS slot 0 "by convention." Wrong. ESP-IDF's pthread
library claims slot 0 unconditionally (`components/pthread/pthread_local_storage.c:23`
`#define PTHREAD_TLS_INDEX 0`). On first boot post-flash, any call to a
libc/ArduinoJson path that internally hits `pthread_getspecific` reads
our `TaskIdentity*` as a `values_list_t*` and crashes with
`LoadProhibited EXCVADDR=0x00000002` (null pointer + 2 offset, matching
the internal struct layout).

**Fix**: `kAuthTlsSlot = 1` with a coordinator comment in
`System_AuthIdentity.cpp` documenting the pthread claim. The pre-flight
check in the plan (Section K, "grep `pvTaskGetThreadLocalStoragePointer\|
vTaskSetThreadLocalStorage` in managed_components/") didn't catch this
because pthread is in IDF core, not `managed_components/`. The correct
check is `grep -rn PTHREAD_TLS_INDEX /path/to/esp-idf/`.

### BTC_TASK stack pressure

The refactor added ~16 bytes per `ExecIdentityGuard` scope plus 2-3
nested call frames (`getOrCreateSlot`, `xTaskGetCurrentTaskHandle`,
`pvTaskGetThreadLocalStoragePointer`, `isAdminUser`). Across deep call
chains this is hundreds of bytes. The 3 KB `CONFIG_BT_BTC_TASK_STACK_SIZE`
budget (and the synchronous-heavy-work-on-BTC pattern that was already
fragile pre-refactor) tipped over on the Files JSON Pretty View exit
handler: BLE notify → SysEvent DOUBLE_CLICK → inline `fn()` that
redraws the file chooser. Stack canary trips at next context switch.

**Two fixes landed together** (one commit):
1. `CONFIG_BT_BTC_TASK_STACK_SIZE`: 3072 → 4096 (defense in depth, +1 KB
   DRAM at boot).
2. Route the TEXT-view exit handler through the existing `g2_tap_disp`
   worker. `TapDispatchEntry` extended with a `kind` discriminator
   (`TAP_DISPATCH_IDX` / `TAP_DISPATCH_EXIT_FN`); new producer
   `tapDispatcherEnqueueExit(void(*fn)())`. Three call sites in
   `G2_Glasses.cpp` (CLICK exit, DOUBLE_CLICK exit, user-activity exit)
   now snapshot `fn`, clear `gTextViewActive`/`gTextViewExitFn`/
   `gTextViewTapFn` synchronously on BTC, and enqueue. Heavy work runs
   on the dispatcher (25 KB stack).

The two fixes are belt-and-braces: either alone might be enough, but
both together gives both extra stack headroom AND eliminates the worst
deep call chain from BTC entirely.

### `gTextViewTapFn` page-next handler stays inline

Section "Other navigation candidates" of this addendum: the page-next
handler (`gTextViewTapFn(kind)` at three sites in `handleDevEvent`)
still runs inline on BTC. Tested working through three pages of pretty
JSON. The work is light (compute next page, enqueue to
`g2_page_swap_w`) — different shape from the exit handler which
synchronously redraws. Left inline; can be moved to the dispatcher
using the same `TAP_DISPATCH_TAP_FN` variant pattern if it ever
overflows in practice.

## Out-of-scope from Stage 1 (not landed)

These items are listed in Section L of the plan and remain valid future
work. None block Stage 2 or 3.

- **`authorizeCommand` empty-user tightening.** Plan recommended doing
  this after Stage 1 lands and proves stable, in its own change.
- **Pair-time identity enforcement.** `bleStampPairedByIfBlank` still
  silently no-ops if there's no current user. Boot-reconnect scenario
  now hits this predictably.
- **Audit remaining task-leak-pattern globals** (`gExecPathPaging`,
  etc.). Lower urgency; revisit after Stages 2-3.

## Stage 2 — pending (next)

`gNotificationContext` (System_Notifications.cpp:36) — same task-leak
pattern, plus a destructor bug (`NotificationContextGuard::~` clears
instead of save/restore, so nested guards break even within a single
task).

**Hypothesis worth measuring**: this refactor may incidentally fix the
existing `ArduinoJson::TextFormatter` LoadProhibited crash todos. The
crash signature matches: not-task-local + destructor-clears-instead-of-
restoring + serialized struct fields = the `writeString` null-deref /
freed-pointer crash mode. If the crash stops recurring after Stage 2
lands, those todos close as a side effect.

Same shape as Stage 1: move into a TLS slot (different field on the
same `TaskIdentity` struct, or a new slot index — slot 2 is unused).
Fix the destructor. Convert ~5-10 read sites. Build + soak test.
Effort: ~half a day.

## Stage 3 — pending (after Stage 2)

Per Sections B.2, B.4, B.5, B.7:
- `gCurrentAutomationUser` + automation state cluster
  (`gAutoLogActive`, `gAutoLogFile`, `gAutoLogAutomationName`,
  `gInAutomationContext`, `gAutosDirty`)
- Output capture buffers (`gCmdCaptureBuf`, `gCmdCaptureLen`,
  `gCmdCaptureCap`)
- `gCurrentCommandContext` (void*) — broadcast-output routing

Effort: ~half a day to a day. Stage 3 only matters if automations can
actually run concurrently (single-scheduler-thread assumption needs
verification first).

## Lessons logged for future TLS work

1. **TLS slot 0 is taken by ESP-IDF pthread.** Use slot 1 (or 2/3 if
   another subsystem ever claims another slot — currently free). The
   coordinator comment lives in `System_AuthIdentity.cpp`.
2. **`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=4`** in sdkconfig
   (raised from default 1). Leaves headroom for Stages 2/3 + future
   subsystems that want their own slot. Cost: 24 bytes × tasks ≈ 700 B.
3. **Pre-flight check**: search ALL of esp-idf for
   `PTHREAD_TLS_INDEX|pvTaskGetThreadLocalStoragePointer`, not just
   `managed_components/`.
4. **BTC_TASK runs BLE notify callbacks on a tiny stack** (3 KB default,
   4 KB after our bump). Never call heavy application work
   synchronously from `handleDevEvent` or `handleNotify`. Defer to
   `tapDispatcherEnqueue*` (worker has 25 KB).
5. **The dual-write migration strategy** (ExecIdentityGuard writes
   both TLS and legacy globals) was unused — we did all reader
   conversions before deleting globals, so they never had to coexist
   long. Worth remembering for Stage 2/3: pick one or the other, not
   both half-measures.
