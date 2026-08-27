# FTS → CLIMode Migration Plan

**Repo:** `/Users/morgan/esp/hardwareone-idf` · component `components/hardwareone/`
**Scope:** boot-time first-time setup (`firstTimeSetupIfNeeded` → `runAndApplyFeatureWizard` → `runSetupWizard`)
**Date:** 2026-08-16

---

## 1. VERDICT

**Do not migrate boot-time FTS onto the CLIMode framework. The deferral note in `System_SetupWizardMode.h:40-48` reached the right conclusion — for the wrong reason — and the premise that motivated re-opening it does not survive contact with the code.**

Three findings drive this.

**(a) The documented rationale is factually wrong, but the conclusion holds.** `System_SetupWizardMode.h:43-45` and `System_FeatureRegistry.cpp:661-663` say FTS "has no cmd_exec hostage problem because cmd_exec doesn't exist at boot yet." It does exist: `gCmdExecQ = xQueueCreate(8, ...)` at `HardwareOne.cpp:1672` and `cmd_exec_task` at `HardwareOne.cpp:1684`, both **172 lines before** `firstTimeSetupIfNeeded()` at `HardwareOne.cpp:1856`. The real blockers are different and worse: no input pump exists inside `setup()`, and `cliEnterModePrepared` hard-requires a live authenticated transport session (`System_CLIMode.cpp:165-172` → `currentOwner`, `:72-86`) that by definition cannot exist when `users.json` is the thing being created. **Correct the comments regardless of what else you do.**

**(b) Half the stated motivation does not apply.** The periodic emitters cannot splatter into boot-time FTS. `periodicMemorySample()` (`[MEMSAMPLE]`) is called only from `hardwareone_loop()` at `HardwareOne.cpp:2527`; `reportAllTaskStacks()` (`[STACK]`) only at `:2555`; `sensorLogAutoStart()` only at `:2315`. None of them run until `setup()` returns, and `setup()` cannot return while FTS blocks. Going async would **create** that exposure and then require CLIMode suppression to fix it. Net zero.

**(c) The "existing correct implementation" is not correct.** `paintCurrentPage()` (`System_SetupWizardMode.cpp:134-142`) claims `printSerialPageStatus()` writes through `broadcastOutput`. It does not — that function is 22 raw `Serial.println/printf` calls beginning at `System_SetupWizard.cpp:990`, part of 106 `Serial.print*` calls in the file against only 14 `broadcastOutput`. So the CLIMode wizard paints every top-level page **directly to the UART**, exactly like the legacy one. Worse: because it is serial-owned, `cliModeSuppressesAmbientSerial()` makes the drain **swallow** its own `broadcastOutput` lines into a 32-line ring (`System_Debug.cpp:275-284`, `kHelpTailLines = 32` at `:145`) that is never auto-replayed. Today, a serial `featuresetup` loses the WiFi scan list (`System_SetupWizard.cpp:1351-1375`), "ESP-NOW identity configured.", and "Feature configuration complete." **Migrating onto this target would import a live bug, not fix one.** *(Uncertainty: this is a static read of an unambiguous code path; I have not run `featuresetup` over serial on hardware. Confirm before acting — it takes two minutes.)*

**Honest size if you did it anyway:** a new pre-auth boot-session concept in the transport/auth layer, a contract change to CLIMode idle timeouts, non-blocking OLED text entry for four flows (ESP-NOW / MQTT / WiFi / device name), a completion channel back to FTS, a boot-sequence restructure, plus ~14 downstream fixes. Realistically **8–12 files, two of them security-sensitive (`System_User.cpp`, `System_CLIMode.cpp`), and a security re-review**. Against a benefit that is entirely achievable in one file.

**What to do instead:** Track 1 in §3 — put both output halves on one ordered stream and give the wizard a suppression primitive decoupled from CLIMode ownership. One file's output calls plus one small debug-layer addition. Same user-visible fix, none of the blast radius. Details in §6.

---

## 2. THE CORE PROBLEM

### Stated precisely

`firstTimeSetupIfNeeded()` is a **blocking synchronous flow on the ESP-IDF main task inside `setup()`**. Its input primitive is `waitForSerialInputBlocking()` (`System_Utils.cpp:735-750`), which spins on `delay(10)` until a newline arrives. At boot the timeout is 0 — `runAndApplyFeatureWizard` is called with the default `idleTimeoutMs = 0` (`System_FirstTimeSetup.cpp:813`), which calls `setSerialWaitTimeout(0)` at `System_SetupWizard.cpp:1801` — so **every boot-time wait is infinite by design**. There are 12 blocking waits in `System_FirstTimeSetup.cpp` and 11 more in `System_SetupWizard.cpp`, plus ~10 unbounded poll loops across `OLED_FirstTimeSetup.cpp` and `OLED_SetupWizard.cpp`.

A `CLIMode` is an **async state machine with no self-drive**. `onInput` runs only on `cmd_exec_task`, reached only via `executeCommand` → `cliModeDispatchInput` (`System_Utils.cpp:5098` → `System_CLIMode.cpp:290`). `onTick` runs only from `cliModeTick()` at `HardwareOne.cpp:2697`. Serial bytes reach the dispatcher only via the drain at `HardwareOne.cpp:2716`. **All three live in `hardwareone_loop()`, which has never executed when FTS runs.**

So the tension is not "sync vs async" in the abstract. It is: **the CLIMode framework's three input paths are all owned by a loop that FTS structurally precedes.**

Layered on top are two independent gates that make entry impossible even if you solved the pump:

- **Auth.** `transportSessionEpochIsLive` for `SOURCE_SERIAL` requires `shadow.authed || !gSettings.serialRequireAuth` (`System_User.cpp:294-306`); `serialRequireAuth` defaults **true** (`System_Settings.h:377`). Plus the loop's own gate at `HardwareOne.cpp:2838-2846`, plus `featuresetup` being `requiresAdmin=true` (`System_FeatureRegistry.cpp:682`) against an `isAdminUser()` that returns false with no `users.json` (`System_User.cpp:818-834`). Three locks, all closed.
- **Owner exclusivity.** `sSuppressAmbientSerial` is set **only** for `owner.source == SOURCE_SERIAL` (`System_CLIMode.cpp:200-201`); `cliModeTick` dispatches **only** for `owner.source == SOURCE_LOCAL_DISPLAY` (`System_CLIMode.cpp:286`). FTS is the one flow that drives serial *and* the OLED simultaneously (see the deliberate `inputStartInternal()` at `HardwareOne.cpp:1836`). **No single-owner CLIMode as written can serve it.**

### Strategies

**Strategy A — truly defer boot.** Return from `setup()` immediately, let the loop drive an FTS mode. **Reject.** It removes the barrier that phases 7-9 depend on: `wifiAutoStart` (read `HardwareOne.cpp:1900`), `bleAutoStart` (`:2029`), `httpAutoStart` (`:2120`), `espnowEnabled` (`:2282`), `ledStartupEffect` (`:2169`), `processAutoStartSensors()` (`:2082`) all read `gSettings` fields the wizard writes. The user picks an archetype and nothing happens until reboot. It also turns `SETUP_IN_PROGRESS` — a security predicate at `WebServer_MigrationTool.cpp:637` — into a long-lived runtime state. Full ledger in §4.

**Strategy B — boot-local pump, stay blocking.** Keep the blocking call site. Inside it, run a local loop that calls `cliModeTick()` and feeds serial lines to the mode, until the mode exits. This is the *only* viable CLIMode migration: boot ordering is preserved, phases 7-9 still read final settings, no `SETUP_IN_PROGRESS` widening. But it still requires (1) a synthetic pre-auth boot session, because `cliEnterMode` will reject you; (2) a never-timeout sentinel, because `modeTimedOutLocked` (`System_CLIMode.cpp:98-106`) treats `idleTimeoutMs == 0` as *use the 5-minute default*, not *never* — getting this wrong reintroduces exactly the bug `bf5bee81` shipped to fix; (3) decoupling the `SOURCE_SERIAL`/`SOURCE_LOCAL_DISPLAY` exclusivity; (4) building the four missing OLED text-entry flows. And after all that it delivers **no behavioural change the user can see** beyond what Track 1 delivers — because Strategy B doesn't run the loop either, so there is still nothing ambient to suppress.

**Strategy C — hybrid: keep the architecture, extract the two mechanisms. ← RECOMMEND.** The migration's real payload is two things: *one ordered output stream*, and *ambient-serial suppression*. Neither is intrinsic to CLIMode. Ordering is fixed by routing all wizard output through `broadcastOutput`. Suppression is a `std::atomic<bool>` in `System_Debug.cpp` that CLIMode happens to own; give it a second, explicit owner. Both are additive, testable, and reversible.

**Why C.** The observed bug has exactly one cause: two writers to one UART with no shared ordering. Strategy C addresses the cause directly. Strategies A and B address it by moving the whole flow into a framework that — per §1(c) — has the same bug plus a suppression path that eats its own output. You would be paying a security review to inherit a defect.

---

## 3. MIGRATION STEPS

### Track 1 — Recommended. Ship this.

**Step 1.1 — Add a routed wizard-output helper.**
`System_Debug.h` / `System_Debug.cpp`. `broadcastOutputCore_Routed(text, len, route)` already exists (`System_Debug.cpp:1073`). Add:

```cpp
// Wizard/setup output: ordered through the queue like everything else, but
// carries MSG_ROUTE_ALLOW_IN_HELP so a serial-owned CLIMode's ambient
// suppression (System_Debug.cpp:275-281) never swallows the page the user
// is looking at.
void broadcastSetupOutput(const char* s);
void broadcastSetupOutput(const String& s);
```
Implementation: `broadcastOutputCore_Routed(s, len, MSG_ROUTE_ALL | MSG_ROUTE_ALLOW_IN_HELP)`. Bit defined at `System_Debug.h:31`.

**Step 1.2 — Convert the legacy wizard's direct writes.**
`System_SetupWizard.cpp`: replace all 106 `Serial.print/println/printf` with `broadcastSetupOutput`. `Serial.println()` with no argument → `broadcastSetupOutput("")`. `printf` sites → build into a `char buf[]` then one call (the 255-byte `DEBUG_MSG_SIZE` clamp marks overflow `[CUT]`, `System_Debug.cpp:958-963` — check the two widest format strings by hand). The concentration is `printSerialPageStatus()` at `:990-1090` (22 calls) and the main loop banner at `:1543-1548`.

**Step 1.3 — Same conversion in the OLED setup files.**
`OLED_FirstTimeSetup.cpp` and `OLED_SetupWizard.cpp` — audit for `Serial.print*` and convert. These share the console with the wizard.

**Step 1.4 — Fix the CLIMode wizard's identical bug.**
`System_SetupWizardMode.cpp`: it calls the same `printSerialPageStatus()`, so 1.2 fixes its page painting for free. Also correct the false comment at `:134-142`. Then verify the WiFi scan list survives — `wifiScanPrintNamed` (`System_SetupWizard.cpp:1339-1378`) must move to `broadcastSetupOutput`, or the serial `featuresetup` keeps losing it.

**Step 1.5 — Drain before the handoff.**
`System_SetupWizard.cpp:1795`, top of `runAndApplyFeatureWizard`, before `gWizardOwnsSerial = true`: call `debugWaitOutputDrained(150)`. This primitive already exists (`System_Debug.cpp:545`, declared `System_Debug.h:285`, precedent at `HardwareOne.cpp:955`). It guarantees FTS's pre-wizard `broadcastOutput` lines — including the "Feature Configuration..." line at `System_FirstTimeSetup.cpp:811` that you saw land ten lines late — are on the wire before the wizard's first frame. Note: after 1.2 this is belt-and-braces, since everything is on one queue.

**Step 1.6 — Add explicit suppression, decoupled from CLIMode ownership.** *(Optional; take it only if hardware shows ambient noise during FTS. Per §1(b) I do not expect any.)*
`System_Debug.cpp`: add `sSuppressAmbientSerialExplicit` alongside `sSuppressAmbientSerial`, with `debugSuppressAmbientSerialBegin()/End()`, and OR it into the `suppressAmbientSerial` computation at `:275-281`. Drive it from `runAndApplyFeatureWizard` in the same RAII shape as `gWizardOwnsSerial` (`System_SetupWizard.cpp:1793-1812`). Do **not** reuse `cliModeSuppressesAmbientSerial()` — that flag is cleared only by `cliModeExecutorDrainPending` on `cmd_exec_task` (`System_CLIMode.cpp:411`), which the main task can never run; latching it from FTS would silence serial for the rest of the boot.

**Step 1.7 — Correct the stale rationale.**
`System_SetupWizardMode.h:43-45` and `System_FeatureRegistry.cpp:661-663`: "cmd_exec doesn't exist at boot yet" → the accurate reason ("`hardwareone_loop()` has not started, so no CLIMode input pump or tick exists; and `cliEnterMode` requires an authenticated transport session that cannot exist before `users.json`").

**Step 1.8 — Fix the four `featuresetup` parity bugs found in passing** (they affect the shipped CLI wizard today, independent of FTS):
- `System_SetupWizardMode.cpp:630-634` — MQTT skip must set `gSettings.mqttAutoStart = false`, matching `System_SetupWizard.cpp:1218`. The ESP-NOW equivalent already does this at `:534`.
- `System_SetupWizardMode.cpp:506-507` / `:556` — `ESPNOW_NAME` + `n` writes an **empty** device name over `gSettings.bleDeviceName`. Legacy preserves the current name (`System_SetupWizard.cpp:1132`).
- `System_SetupWizardMode.cpp:646` — blank MQTT port writes `1883`, clobbering a configured port. Legacy leaves 0 and applies only `if (>0)` (`:1243`, `:1274`).
- `System_SetupWizardMode.cpp:809-855` — no heap summary at completion (legacy `:1831-1839`).

**Step 1.9 — Two latent defects worth fixing now, unrelated to output.**
- **Permanent-lockout window.** `System_FirstTimeSetup.cpp:889` writes `users.json`, then `:899` writes the per-user settings (password hash). A reboot between them yields `detectFirstTimeSetupState() == SETUP_NOT_NEEDED` (`:83-84`) with `isValidUser()` failing at `loadUserSettings` (`System_User.cpp:1941`) — **bricked, no FTS to recover it**. Swap the order: write user-1 settings first, `users.json` last as the atomic commit token.
- **Shared httpd handle.** `startRestoreOnlyHttpServer()` (`WebServer_MigrationTool.cpp:894-993`) starts into the single global `server` (`HardwareOne.cpp:390`, `WebServer_Handle.h:11`) with no `if (server) return;` guard, and `stopRestoreOnlyHttpServer()` (`:997-1010`) unconditionally stops and NULLs whatever is there. Safe only because FTS blocks. Give the restore server its own handle.

**Validation:** flash a device with `users.json` erased. Watch serial through the full 6-page wizard. Expect strictly ordered, timestamped output with no line landing inside a menu. Then run `featuresetup` over serial on a provisioned device and confirm the WiFi scan list now appears (it currently does not).

---

### Track 2 — Only if the owner overrides the verdict. Strategy B, ordered.

Prerequisite: Track 1 steps 1.1–1.4 and 1.9 land first. Track 2 without 1.2 does not fix the bug.

1. **Unify first-admin creation.** Two divergent copies exist — `createInitialAdminUser()` (`System_FirstTimeSetup.cpp:238-283`) and the inline block at `:868-914`, acknowledged at `:234-237`. Collapse to one. Separate commit.
2. **Never-timeout sentinel.** `System_CLIMode.h` + `modeTimedOutLocked` (`System_CLIMode.cpp:98-106`). Add `kIdleTimeoutNever = UINT32_MAX` handled explicitly. **`0` is not an escape hatch** — it selects `kDefaultIdleTimeoutMs` (`:46`, 5 min).
3. **Boot-only synthetic session.** `System_User.cpp` — mint a `TransportSessionEpoch` valid **only** while `gFirstTimeSetupState == SETUP_IN_PROGRESS && !VFS::exists(USERS_JSON_FILE)`, invalidated the instant `users.json` is written. Do **not** relax `serialRequireAuth`, `localDisplayRequireAuth`, or `isAdminUser()`.
4. **Dedicated FTS mode.** New `System_FTSMode.cpp`. Its `onInput` must **never** return `CLI_MODE_PASSTHROUGH` — that non-fallthrough is the entire containment argument for the pre-auth surface.
5. **Decouple the owner gates.** `System_CLIMode.cpp:200-201` (suppression) and `:286` (tick) — allow a mode to declare it wants both regardless of owner source.
6. **Boot-local pump.** In `firstTimeSetupIfNeeded`: `while (ftsModeActive()) { cliModeTick(); drainSerialToMode(); delay(10); }`. Feeding lines needs a path to `cliModeDispatchInput` that doesn't go through the loop's auth gate at `HardwareOne.cpp:2838-2846`.
7. **`onExit` affinity trap.** `cliModeExecutorDrainPending` hard-returns off `cmd_exec_task` (`System_CLIMode.cpp:384-387`). If the pump ever enters a mode via the direct-call fallback (`System_Utils.cpp:5212`), `onExit` can never run and `sSuppressAmbientSerial` latches true for the rest of the boot. Assert the invariant.
8. **Four OLED text-entry flows.** ESP-NOW, MQTT, WiFi, device name. `wizardMode_onTick`'s `default:` branch (`System_SetupWizardMode.cpp:919-932`) handles only `nav.left`. **Without this, an OLED+gamepad device with no serial attached cannot enter WiFi credentials during FTS** — a hard functional regression, not cosmetic. Note also that `handleSystemInput` already calls a *blocking* OLED keyboard from inside `onTick` (`OLED_SetupWizard.cpp:559`), violating the "sub-millisecond work only" contract in `System_CLIMode.h`.
9. **Completion channel.** `sWizard.result` is file-static with no accessor (`System_SetupWizardMode.cpp:90-108`) and `wizardMode_onExit` returns `void`. FTS reads `wizardResult.wifiConfigured && wizardResult.wifiSSID.length()` (`System_FirstTimeSetup.cpp:814`). Add a getter or completion callback.
10. **Restructure the FTS tail** (`System_FirstTimeSetup.cpp:797-1005`) around async wizard completion.
11. Then work §4.

---

## 4. DOWNSTREAM EFFECTS

Severity is for **Strategy A (fully async)** unless noted. Rows marked **[B]** also apply to Strategy B. Rows marked **[fix now]** are latent today and worth fixing regardless.

| # | Effect | Sev | Fix |
|---|---|---|---|
| 1 | **CLIMode entry is impossible.** Triple lock: epoch liveness (`System_User.cpp:294-306`, `serialRequireAuth` default true `System_Settings.h:377`), loop auth gate (`HardwareOne.cpp:2838-2846`), admin gate (`System_FeatureRegistry.cpp:682` → `System_User.cpp:818-834`). Any bypass creates a **pre-auth, admin-equivalent command surface** on serial and — post-restore — UART/BLE/ESP-NOW. **[B]** | CRITICAL | Boot-only one-shot epoch scoped to `SETUP_IN_PROGRESS && !exists(users.json)`; FTS mode never passes through to `findCommand()`. New security-review item. |
| 2 | **5-minute idle timeout aborts provisioning.** `kDefaultIdleTimeoutMs` (`System_CLIMode.cpp:46`) → `requestExitLocked("idle_timeout")` (`:126`) → `wizardMode_onExit` prints "No changes saved." (`System_SetupWizardMode.cpp:851-853`). Device left with no admin. Same *shape* as the bug `bf5bee81` fixed. Boot FTS deliberately has no timeout (`System_Utils.cpp:712-714`). **[B]** | CRITICAL | Explicit never-timeout sentinel. `0` means *default*, not *never*. |
| 3 | **Phase-8 autostarts read pre-wizard defaults.** WiFi `HardwareOne.cpp:1900`, BLE `:2029`, sensors `:2082`, camera/mic `:2087-2112`, HTTP `:2120`, LLM `:2137`, MQTT `:2148`, LED `:2169`, ESP-NOW `:2282`. User picks an archetype; nothing happens. | HIGH | Unconditional reboot at FTS completion (three of four existing exits already reboot: `:353`, `:681`, `:973`). |
| 4 | **OLED boot sequence completes → login screen on a userless device.** `processOLEDBootSequence()` (`OLED_Utils.cpp:4931`, driven from `HardwareOne.cpp:2677`) sets `oledBootModeActive = false` and requests `OLED_LOGIN` (`:4959-4976`) since `localDisplayRequireAuth` defaults true (`System_Settings.h:270`). Masked only by the `SETUP_IN_PROGRESS` early-return at `OLED_Utils.cpp:3793-3796` — the mode state is still corrupted. | HIGH | Gate the phase advance on `gFirstTimeSetupState == SETUP_NOT_NEEDED`. |
| 5 | **`/api/restore` gate loses temporal isolation.** `WebServer_MigrationTool.cpp:636-643` keys on `SETUP_IN_PROGRESS`, meaningful today only because it coincides with a blocked boot serving 5 URIs. | HIGH | Add `!VFS::exists(USERS_JSON_FILE)` as a third gate. |
| 6 | **Shared `httpd_handle_t server`.** Unguarded `httpd_start(&server,...)` (`WebServer_MigrationTool.cpp:894-993`) vs `cmd_openhttp` (`HardwareOne.cpp:2118`). Worst case: Import's `goBack` (`System_FirstTimeSetup.cpp:661`) stops and NULLs the **user's real web server**. **[fix now]** | HIGH | Separate `sRestoreServer` handle; guard start. |
| 7 | **`users.json`-before-password crash window → permanent lockout.** `System_FirstTimeSetup.cpp:889` vs `:899`. Sub-millisecond today; a live-system window when async. **[fix now]** | MED→HIGH | Write user-1 settings first; `users.json` last as commit token. |
| 8 | **I2C/OLED force-enable never unwound.** `System_I2C.cpp:389-393` and `OLED_Utils.cpp:4801` force hardware on for FTS; `tryAutoStartInputForMenu` (`:6646`) bypasses on the same flag. State flips mid-runtime with no re-init. | MED | Unconditional reboot. Do **not** tear down in place — see the heap-fragmentation note at `System_FirstTimeSetup.cpp:976-979`. |
| 9 | **`uartLinkInitFromSettings()` loses its invariant.** `HardwareOne.cpp:1858-1861` states it explicitly. Harmless on a virgin device (`uartLinkEnabled` false), real on the post-restore path where `settings.json` is restored but `users.json` deliberately is not (`System_FirstTimeSetup.cpp:365-376`). | MED | Replace ordering with `if (!isFirstTimeSetup())`; re-drive after completion. |
| 10 | **32-line suppression ring swallows boot diagnostics.** `kHelpTailLines = 32` (`System_Debug.cpp:145`), never auto-replayed. Phases 7-9 emit far more. WiFi result, HTTP URL, `printMemoryReport()` all lost. **[B, partially]** | MED | Enlarge the ring, or route boot lines with `MSG_ROUTE_ALLOW_IN_HELP` (Step 1.1's helper). |
| 11 | **`volatile` FTS globals become cross-task.** `gFirstTimeSetupState` / `gSetupProgressStage` (`System_FirstTimeSetup.h:36-41`) documented as "single writer (setup) + OLED-animation readers." Async, the writer moves to `cmd_exec_task` while the httpd task reads it as a **security predicate** (`WebServer_MigrationTool.cpp:637`). | MED | `std::atomic` with acquire/release; single acquire snapshot at the gate. |
| 12 | **Radios up before an admin exists.** Small on virgin devices (`espnowEnabled` false `System_Settings.h:206`, `bleAutoStart` false `:386`, no saved WiFi). Real post-restore: donor `settings.json` + absent `users.json`. Authority still fails closed (`isAdminUser` false; ESP-NOW validates via `isValidUser`, `System_ESPNow.cpp:4075`) — this is surface, not authority. | MED | Gate phase 8 on `!isFirstTimeSetup()`, or reboot. |
| 13 | **`oledBootModeActive` becomes a long-lived display-auth bypass.** The #4 mitigation extends `shouldBlockForDisplayAuth()`'s bypass (`OLED_Utils.cpp:48`, `:2956`; also `OLED_UI.cpp:545,735,749`) from seconds to however long a human takes. | MED | Separate `oledFirstTimeSetupActive` flag so the two conditions audit independently. |
| 14 | **Two divergent first-admin creation sites.** `System_FirstTimeSetup.cpp:238-283` vs `:868-914`, acknowledged at `:234-237`. **[B, fix now]** | MED | Unify on `createInitialAdminUser()` before any restructure. |
| 15 | **`SETUP_IN_PROGRESS` re-entry log churn.** `setFirstTimeSetupState` (`:114-126`) dedupes, but back-navigation over minutes with the event-history file sink live (not live during blocking FTS) changes the profile. | LOW | Already deduped; monitor. |
| 16 | **Progress bar runs backwards.** `OLED_Mode_Animations.cpp:422-441` computes `((gSetupProgressStage + 1) * 100) / 5`; async back-navigation re-enters stages. | LOW | Monotonic high-water mark. |
| 17 | **`otaSafetyAcceptProvisioningBoot()` becomes dead code** (`HardwareOne.cpp:1853`, rationale `System_OTASafety.cpp:196-201`). The one genuine benefit of async — **already obtained** by `bf5bee81`. | LOW (+) | Remove the call if async lands. |
| 18 | **`resolvePendingUserCreationTimes()` resolves immediately** instead of staying `"pending"` (`System_FirstTimeSetup.cpp:911-913`; NTP is phase 7). Genuine small win. | LOW (+) | — |

**Track 1 downstream effects: rows 6, 7, 14 only** — and those are pre-existing defects, not consequences of the change. Track 1 alters no boot ordering, no auth surface, no global lifetime.

---

## 5. WHAT COULD SILENTLY BREAK

Things that compile clean, pass any test you have, and fail on a user's desk.

1. **Suppression latched forever.** If any path sets `sSuppressAmbientSerial` (`System_CLIMode.cpp:201`) from a task that is not `cmd_exec_task`, `cliModeExecutorDrainPending` hard-returns (`:384-387`), the mode sticks in `ExitPending`, and **every ambient serial line for the rest of the boot vanishes** into a 32-line ring. No error, no log — the console just goes quiet. This is why Step 1.6 uses a separate flag.

2. **255-byte truncation on converted `printf` sites.** `enqueueChunk` clamps at `DEBUG_MSG_SIZE` and marks `[CUT]` (`System_Debug.cpp:958-963`). A direct `Serial.printf` has no such limit. Any wizard line built from a device name plus a long SSID plus a status suffix silently loses its tail. **Hand-audit the widest format strings in `System_SetupWizard.cpp` during Step 1.2.**

3. **Queue drops under pressure.** `broadcastOutput` returns void; `enqueueChunk` returns false on a full queue and only bumps `incrementDebugDropped()`. Converting 106 direct writes to enqueues raises queue pressure during page paints. A dropped line is an invisibly missing menu item. Watch the drop counter (`System_MemoryMonitor.cpp:244`) on the first hardware run.

4. **`Serial.readStringUntil('\n')` inherits Arduino's 1000 ms `Stream::_timeout`.** Nothing in the component calls `Serial.setTimeout` (grep returns nothing). Any guarded read that sees a byte without a newline blocks the main task for a full second. Already true today; any change to the input path can shift how often it fires.

5. **`sDummyResult`.** `handleFeaturesInput` / `handleTogglePageInput` call `wizardNextPage(sDummyResult)` (`OLED_SetupWizard.cpp:516, 529, 540`), so a joystick-driven "next" off the last page sets `completed` on a **throwaway** result. Only `handleSystemInput` gets the real one (`:583`). Both engines inherit this. Any refactor that changes which handler owns the last page changes whether `completed` reaches the caller.

6. **`SetupWizardResult::deviceName` is dead.** Written at `System_SetupWizard.cpp:1519` and `System_SetupWizardMode.cpp:775`, read nowhere. Someone will "fix" this by wiring it up and change device naming behaviour. Delete it.

7. **Three dead print functions.** `printSerialFeaturePage` (`:920`), `printSerialNetworkPage` (`:939`), `printSerialSystemPage` (`:957`), plus `printSerialHeapBar` (`:905`) — defined, never called. Converting them in Step 1.2 is wasted work; leaving them creates two conventions in one file. **Verify dead, then delete** (per the audit lesson: re-verify before deleting).

8. **`gWizardOwnsSerial` has a second consumer.** `System_UartLink.cpp:1070` parks the CM5 link on it. Any change to its lifetime (`System_SetupWizard.cpp:1807/1811`) silently affects the UART host link.

9. **Partial state is already normal and already safe — don't "fix" it.** `settings.json` is written with defaults *before* FTS (`HardwareOne.cpp:1474-1477`); `applyArchetypeSeed` writes it mid-flow (`System_SetupWizard.cpp:84`); the Import path saves WiFi networks before any user exists (`System_FirstTimeSetup.cpp:508-512`). This works because `detectFirstTimeSetupState()` keys **strictly** on `users.json` (`:83-84`, comment: settings can exist without users). Anyone who "hardens" that predicate to also check `settings.json` breaks re-entrant setup.

10. **Untested claim:** that serial `featuresetup` currently loses its `broadcastOutput` lines (§1(c)). The code path is unambiguous but unverified on hardware. If it turns out the suppression does not fire — e.g. the epoch is not live so the mode never enters — then Step 1.4's urgency drops, though Step 1.2 is unaffected.

11. **Untested claim:** that `cliModeTick`'s zero-wait `ModeLock(0)` (`System_CLIMode.cpp:279`) drops ticks rarely enough for joystick edge detection. `wizardMode_onTick` edge-detects against its own `sLastButtons` (`System_SetupWizardMode.cpp:872-887`), so a dropped tick loses the sampling instant, not the edge. Only matters for Track 2.

---

## 6. CHEAPER ALTERNATIVE (= Track 1, expanded)

**The change:** route every wizard line through the same queue, in the same order, as the FTS lines around it. Optionally add an explicit ambient-suppression switch not coupled to CLIMode ownership.

**Why it works.** Your observed symptom — `"Feature Configuration..."` timestamped `[137250]`, created before the wizard printed anything, delivered ten lines into the menu — is fully explained without invoking periodic emitters. `broadcastOutput` at `System_FirstTimeSetup.cpp:811` enqueues on the main task. The main task then enters `runSetupWizard()` and writes directly via `Serial.println` at `System_SetupWizard.cpp:1543-1548` and `printSerialPageStatus()` at `:1653`. The `debug_out` task is `TASK_PRIORITY_LOW`, pinned `PRO_CORE` (`System_Debug.cpp:704-720`) — **the same priority and the same core as the main task**. It cannot preempt; it runs only when the main task yields. The wizard's first yield is `delay(50)` at `System_SetupWizard.cpp:1658` — after the banner and the whole first menu page are already on the wire. That is your ten lines, exactly.

Put both writers on one queue and the ordering is correct **by construction**, not by timing luck.

**What it fixes:**
- The observed interleave, completely and permanently.
- The *in-wizard* interleave you have not reported yet but is present: the WiFi scan list (`broadcastOutput`, `System_SetupWizard.cpp:1351-1375`) races the WiFi page banner (direct `Serial`) on every FTS run that reaches page 9.
- The shipped serial `featuresetup` bug where those same scan lines are swallowed (Step 1.4).
- Ordering for `System_SetupWizardMode.cpp` too, since it shares `printSerialPageStatus()`.
- With Step 1.6: periodic splatter, if hardware ever shows any.

**What it does not fix:**
- Nothing about the blocking architecture. An unattended device with no `users.json` still hangs in `hardwareone_setup()` forever — `sSerialWaitTimeoutMs` is 0, `isWizardCancelRequested()` is permanently false at boot, `CONFIG_ESP_TASK_WDT_PANIC` is unset (`sdkconfig:1791`) and all wait loops yield via `delay()` so idle tasks stay fed. *(This is deliberate: `System_Utils.cpp:712-714` documents "wait forever — fresh-device owner may need time to read instructions." If you want it bounded, that is a separate one-line decision about `setSerialWaitTimeout`, not a reason to migrate.)*
- No behavioural unification between the two wizards. Two engines remain.
- The four `featuresetup` parity bugs — fixed separately in Step 1.8, cheaply.

**When to prefer the migration instead.** Only if a requirement appears that the blocking design genuinely cannot serve: FTS over BLE/web/ESP-NOW concurrently with serial; or an FTS that must not block boot because something time-critical has to run first. Neither is a current requirement, and the one benefit that *was* real — OTA probation expiring during a slow read — was already solved by `otaSafetyAcceptProvisioningBoot()` (`HardwareOne.cpp:1853`, `System_OTASafety.cpp:194-235`, shipped in `bf5bee81`).

**Effort.** Track 1 core (1.1–1.5, 1.7): one new debug helper plus mechanical conversion in three files, no behaviour change beyond ordering. Add 1.8 and 1.9 as separate commits — they are independent bug fixes that happen to be in the neighbourhood.

**Recommendation: do Track 1. Correct the stale comments in the same commit so the next person to open this question starts from the real constraints — the missing input pump and the auth gate — not the incorrect one about `cmd_exec`.**