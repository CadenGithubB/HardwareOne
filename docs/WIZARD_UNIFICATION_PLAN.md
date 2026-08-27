# Setup-Wizard Unification Plan

Working tree `main` @ `d3ad5354` (dirty). All line refs are `components/hardwareone/…` unless noted.

---

## 1. VERDICT & SHAPE

**One core, two drivers — yes. But the core is a step machine, not a rendering framework.**

The three investigations converge on one-core-two-drivers; they diverge on how much to abstract. I am rejecting the widest version.

**What I am rejecting:** the `WizardStepInfo` / `WizardRow` / `wizardCoreDescribeStep()` / `wizardCoreRowAt()` description layer. It requires rewriting `printSerialPageStatus()` (`System_SetupWizard.cpp:990-1090`) *and* all four OLED page renderers (`OLED_SetupWizard.cpp:133-411`) — 400+ lines of rewrite on a code path whose failure mode is an unbootable device, in exchange for zero behavioural gain. The defects it claims to make "unrepresentable" (B6/B7 — the OLED System page carrying its own index ladder at `OLED_SetupWizard.cpp:328-392` that disagrees with `getSystemItemAt` at `System_SetupWizard.cpp:284-300`) are fixable pointwise in ~25 lines by making `renderSystemPage` call `getSystemItemAt()`. Do that instead. Row painting is *already* shared where it matters; the OLED renderers are a display concern and should stay one.

**What I am accepting:** the CLIMode engine's step machine — `WizardSubMode`, `subModeForPage`, `paintAfterTransition`, the five `dispatch*` functions (`System_SetupWizardMode.cpp:53-750`) — *is* the general wizard. It already unrolls every page into linear, one-input-at-a-time steps, which is precisely what a blocking pump also needs. The legacy engine's `runSetupWizard()` (`:1512-1782`) plus its four serial sub-page handlers are a second, worse implementation of the same machine. Move the CLIMode machine into a driver-neutral TU, delete the legacy one, and give the blocking path a ~60-line pump.

### Target architecture

```
                    System_SetupWizardCore.cpp   (NEW TU — driver-neutral)
                    ┌───────────────────────────────────────────────┐
                    │ step machine: WizardSubMode, subModeForPage,   │
                    │   dispatchTopLevel/Mode/ESPNow/MQTT/WiFi       │
                    │ page model   : (stays in System_SetupWizard.cpp,│
                    │   already shared: initSetupWizard, nav,        │
                    │   wizardToggle*, wizardModeApply, heap bar)    │
                    │ semantics    : field apply, commit, abort      │
                    │ sink         : WizardEmitFn (indirection only) │
                    │ field query  : wizardEngineField() -> {kind,   │
                    │                 prompt, isSecret, maxLen}      │
                    └───────┬───────────────────────────────┬────────┘
                            │                               │
        ┌───────────────────▼──────────┐      ┌─────────────▼────────────────┐
        │ BLOCKING DRIVER              │      │ CLIMODE DRIVER               │
        │ System_SetupWizard.cpp       │      │ System_SetupWizardMode.cpp   │
        │ • runAndApplyFeatureWizard() │      │ • kWizardMode table          │
        │ • gWizardOwnsSerial          │      │ • onEnter/onInput/onExit/    │
        │ • waitForSerialInputBlocking │      │   onTick  (~150 lines)       │
        │   OR getOLEDTextInput()      │      │ • idle timeout 10 min        │
        │ • sink = DIRECT Serial       │      │ • sink = broadcastSetupOutput│
        │ • timeout = INFINITE         │      │ • session ownership          │
        │ • no cancel word             │      │ • 'cancel' word              │
        │ • returns SetupWizardResult  │      │ • ignores the result         │
        └──────────────────────────────┘      └──────────────────────────────┘
                 called by FTS                    called by cmd_featuresetup
              System_FirstTimeSetup.cpp:824       System_FeatureRegistry.cpp:664
```

The core never knows which driver it is under. It never blocks, never touches `Serial` directly, never reads a clock, and never decides when it is allowed to exit. Those four things *are* the driver boundary.

**Load-bearing constraint confirming the shape:** `cliEnterModePrepared()` bails at `System_CLIMode.cpp:166-172` when `currentOwner(owner, requireInteractive=true)` fails. FTS can never enter a CLIMode. So the shared thing *cannot* be the CLIMode — it must sit underneath it. The rejection recorded in `docs/FTS_WIZARD_OUTPUT_PLAN.md` is correct and this plan does not relitigate it.

---

## 2. THE SHARED CORE

New TU `System_SetupWizardCore.{h,cpp}`. Add to `CMakeLists.txt` beside `System_SetupWizardMode.cpp`.

`System_SetupWizard.cpp` keeps the **page model** (already genuinely shared, both engines call it today): `initSetupWizard` `:364`, `rebuildNetworkSettingsPage` `:473`, `wizardIsPageVisible`/`AdvanceFrom`/`RetreatFrom` `:659-714`, `wizardToggleCurrentItem`/`MoveUp`/`MoveDown`/`CycleOption` `:720-808`, `wizardModeApply` `:615`, `getHeapBarData` `:341`, the WiFi scan cache `:1325-1380`, and the static tables `:95-183`. None of that moves; it is already correct and driver-neutral.

### 2a. Output sink

```cpp
// One line, no trailing newline. The core never calls Serial or broadcastOutput.
typedef void (*WizardEmitFn)(const char* line);
void wizardEngineSetSink(WizardEmitFn sink);   // NULL restores direct-Serial default
void wizardEmit(const char* line);             // core-internal + used by page painters
void wizardEmitf(const char* fmt, ...);
```

`printSerialPageStatus()`'s 22 raw `Serial.*` calls (`:990-1090`) convert to `wizardEmit`/`wizardEmitf`. **The two drivers choose different sinks — deliberately (see §7).**

### 2b. Field descriptor — how the blocking driver knows *how* to read

This is the piece that lets one core serve a blocking pump and an async dispatcher without an `onNeedsText` callback or a re-entrant state machine.

```cpp
enum WizardFieldKind : uint8_t {
  WIZ_FIELD_NONE = 0,   // top-level page: n / b / <number>
  WIZ_FIELD_MENU,       // c / n / b intro card
  WIZ_FIELD_TEXT,       // free text
  WIZ_FIELD_SECRET,     // free text, masked, never logged
  WIZ_FIELD_SCANLIST,   // WiFi SSID: number | raw | rescan | skip | back
};
struct WizardFieldInfo {
  WizardFieldKind kind;
  const char* prompt;      // never NULL
  const char* initialText; // "" if none — feeds getOLEDTextInput's initialText
  int         maxLength;   // 32 default; 21 for device name
};
void wizardEngineField(WizardFieldInfo* out);   // describes the CURRENT step
```

The blocking driver switches on `kind` to pick `waitForSerialInputBlocking()` (`System_Utils.h:342`) versus `getOLEDTextInput(prompt, isPassword, initialText, maxLength, &cancelled, canSkip)` (`OLED_FirstTimeSetup.h:33`) versus `getOLEDWiFiSelection()` (`:55`). The CLIMode driver ignores it entirely — its input already arrives as a line.

### 2c. The engine

```cpp
void wizardEngineEnter(void);                  // initSetupWizard + tz sync + subMode reset
bool wizardEngineDone(void);                   // true once subMode == DONE
CLIModeInputResult wizardEngineInput(const String& line, char* out, size_t outSize);
void wizardEngineCancel(void);                 // mark aborted; does NOT commit
bool wizardEngineCommit(void);                 // THE apply path; returns false if not completed
const SetupWizardResult& wizardEngineResult(void);
```

`wizardEngineInput` returns the existing `CLIModeInputResult` — reusing the enum avoids inventing a parallel status type and keeps the CLIMode driver a pass-through. `CLI_MODE_HANDLED_AND_EXIT` means "the machine reached DONE"; the driver decides what to do about it.

`wizardEngineCommit()` is the **single** apply path, replacing six scattered sites (`wizardFinalize` `:868`, `runAndApplyFeatureWizard` `:1829-1862`, `wizardMode_onExit` `:809-855`, plus per-page tails at `:1181`, `:1272`, `System_SetupWizardMode.cpp:504-517`, `:611-618`). Its fixed order — **this order is a preserved invariant**:

1. `wizardFinalize()` (tz + `Clock::applyTimezone()` + log level + NTP + LED + device name + `cmd_certgen` under `ENABLE_HTTPS`)
2. `upsertWiFiNetwork` → `sortWiFiByPriority` → `saveWiFiNetworks` → `setSetting(gSettings.wifiAutoStart, true)`
3. `writeSettingsJson()` → `applySettings()`
4. `if (WiFi.isConnected()) setupNTP()`
5. completion banner + timezone line + **heap summary** (fixes B4)

Note `wizardFinalize` at `:871` writes `gSettings.tzOffsetMinutes` **without** `Clock::applyTimezone()` — that call lives only in `wizardNextPage:823`. Fold the `applyTimezone()` into `wizardFinalize` as part of this move (fixes N7).

### 2d. Staging policy — stated, because it has a cost

**Strings and scalars stage in `SetupWizardResult`; boolean feature toggles stay live in `gSettings`.**

Toggles must stay live: `getHeapBarData` (`:341-358`) → `getEnabledFeaturesHeapEstimate()` reads them back through the registry to draw the heap bar. Staging them requires giving the registry a shadow-state parameter — wider blast radius than this project earns.

Consequence, stated honestly: after an abort, feature toggles remain dirty in RAM (unpersisted, exactly as today), but tz / NTP / LED / device names / MQTT / ESP-NOW are not (strictly better than today, where legacy `:1779` finalizes unconditionally and *then* prints "No changes saved" — B8).

### 2e. What each driver implements

| | Blocking driver | CLIMode driver |
|---|---|---|
| Sink | direct `Serial` (unchanged from today) | `broadcastSetupOutput` |
| Input | `Serial.available()` poll + `waitForSerialInputBlocking()` / OLED keyboard, selected by `wizardEngineField()` | `onInput(line)` from `cliModeDispatchInput` |
| OLED joystick | inline poll (`:1732-1776`) | `onTick` (`System_SetupWizardMode.cpp:868-891`) |
| Timeout | `setSerialWaitTimeout(idleTimeoutMs)`; **FTS passes 0 = infinite** | `kWizardMode.idleTimeoutMs = 10 min` (`:963`) |
| Cancel | `isWizardCancelRequested()` polling only; **no cancel word** | `'cancel'` keyword + owner-loss exit |
| Serial ownership | `gWizardOwnsSerial` (`:1793`) | not needed |
| Result | returns `SetupWizardResult` by value | discards |
| Commit trigger | explicit, after the pump exits with `wizardEngineDone()` | `onExit`, gated on an explicit completion latch (see D9) |

---

## 3. INCREMENTAL STEPS

Each step is one commit, build-green on every board, with an explicit rollback. **No incremental commits mid-refactor within a step** — finish the step, hardware-test, then commit (per the project's standing rule).

### Step 0 — Baseline oracle (no code change)

Build every board via `tools/build_board.sh`; record `.bin` sizes. Capture full serial transcripts of (a) a real FTS Advanced run, (b) a complete `featuresetup` walkthrough. Set up the fast FTS loop (§8). These transcripts are the only regression oracle for Steps 3-5.

*Rollback: n/a.*

### Step 1 — Independent fixes + dead-code removal

- Delete `printSerialHeapBar` `:905`, `printSerialFeaturePage` `:920`, `printSerialNetworkPage` `:939`, `printSerialSystemPage` `:957`, `runOLEDSetupWizard` (`OLED_SetupWizard.cpp:1309` + decl `OLED_SetupWizard.h:61`).
- Delete `SetupWizardResult::deviceName` and `::wifiEnabled` (both written, zero reads).
- Fix the four Engine-B parity bugs: B1 MQTT skip, B2 empty device name, B3 blank-port clobber, B4 missing heap summary (§5).
- Fix B6/B7 pointwise: `OLED_SetupWizard.cpp:384` change `#ifndef ENABLE_ESPNOW` → `#if !ENABLE_ESPNOW`; make `renderSystemPage` (`:328-392`) drive its rows from `getSystemItemAt()`/`getWizardSystemPageCount()` instead of its private ladder.
- Fix B9/N6: clamp in `setWizardCurrentSelection`.

FTS untouched. Testable with no erase (except the MQTT items — see §8).

*Rollback: `git revert`. Zero coupling to later steps.*

### Step 2 — Sink indirection, sink unchanged

Add `wizardEmit`/`wizardEmitf`/`wizardEngineSetSink` with the default pointing at direct `Serial`. Convert `printSerialPageStatus()`'s 22 `Serial.*` calls. **Output must be byte-identical** — diff against the Step-0 transcript; a non-empty diff is a failed refactor, not a discovered improvement.

*Rollback: `git revert`; pure indirection.*

### Step 3 — CLIMode driver switches sink; fixes web invisibility (N3)

Add `broadcastSetupOutput(const char*)` built on `broadcastOutputCore_Routed(text, len, MSG_ROUTE_ALL | MSG_ROUTE_ALLOW_IN_HELP)` (`System_Debug.h:374`). **`MSG_ROUTE_ALLOW_IN_HELP` is mandatory, not optional:** a serial-owned CLIMode sets `sSuppressAmbientSerial` (`System_CLIMode.cpp:201`), and `System_Debug.cpp:275-281` swallows any `MSG_ROUTE_SERIAL` line lacking that flag. Without it this step makes the CLIMode wizard invisible *on serial* while fixing it on web.

`wizardMode_onEnter` calls `wizardEngineSetSink(broadcastSetupOutput)`; `onExit` restores the default.

Two known cosmetic deltas, accept them and update the oracle: serial lines gain the `[%lu] ` timestamp prefix (`System_Debug.cpp:287`), and the trailing bare `"> "` prompt gains a newline.

*Rollback: one-line sink reassignment.*

### Step 4 — Relocate the step machine (no behaviour change)

Move `WizardSubMode` (`System_SetupWizardMode.cpp:53-88`), `sWizard` (`:89-108`), `subModeForPage`, `paintAfterTransition` (`:134-293`), and `dispatchTopLevel`/`dispatchModePage`/`dispatchESPNow`/`dispatchMQTT`/`dispatchWiFi` (`:299-750`) into `System_SetupWizardCore.cpp` behind the §2 API. Same functions, same order, new TU.

Fix **N1/D10 here** — it is a prerequisite for the blocking driver, which would otherwise never terminate. `goNextPage()` (`:498-503`, `:605-610`) re-derives the sub-mode from `getWizardCurrentPage()`, but `wizardNextPage` returns `false` *without* changing `currentPage` at completion (`System_SetupWizard.cpp:842-848`). So on a terminal ESP-NOW or MQTT page the machine loops back to its own intro forever and the `DONE` guards at `:583`/`:663` are dead conditions. Make `goNextPage()` set `subMode = DONE` explicitly on the `wizardNextPage()==false` branch. N2 (`'n'` returning `CLI_MODE_HANDLED` after completion, `:556/:566/:573/:638/:645/:653`) falls out of the same fix.

Also fix **N4/D13** (MQTT intro unrecognized input: adopt the ESP-NOW convention — treat it as the host value, per the rationale comment at `:1135-1139`), **N5/D14** (skip `MQTT_PASS` when the username is blank), **D15** (only write `espnowStationary` when the stationary step was actually reached), and **D18** (`onTick:935` re-deriving sub-mode on any nav input — only re-derive when `getWizardCurrentPage()` actually changed).

`System_SetupWizardMode.cpp` shrinks to ~150 lines of CLIMode glue.

*Rollback: `git revert`. The TU split is mechanical; the N1/N4/N5 fixes are individually small enough to back out alone.*

### Step 5 — Blocking driver, behind a compile gate ← **the risky one**

Add to the core TU:

```cpp
SetupWizardResult runSetupWizardBlocking(void) {
  wizardEngineSetSink(nullptr);            // direct Serial — see §7
  wizardEngineEnter();
  char out[256];
  while (!wizardEngineDone()) {
    if (isWizardCancelRequested()) { wizardEngineCancel(); break; }
    WizardFieldInfo f; wizardEngineField(&f);
    String line = readWizardLine(f);       // ONLY function that knows Serial/OLED
    if (isWizardCancelRequested()) { wizardEngineCancel(); break; }
    if (wizardEngineInput(line, out, sizeof out) == CLI_MODE_HANDLED_AND_EXIT) break;
    if (out[0]) wizardEmit(out);
  }
  return wizardEngineResult();
}
```

`runAndApplyFeatureWizard()` keeps its signature, keeps `gWizardOwnsSerial` (`:1807/:1811`) and `setSerialWaitTimeout`, and changes exactly one internal call: `runSetupWizard()` → `runSetupWizardBlocking()`, then `wizardEngineCommit()` in place of its inline apply block.

**Gate on `#define WIZARD_USE_SHARED_ENGINE` in `System_BuildConfig.h`**, legacy call in the `#else`. Compile-time exclusive, so the parked branch costs zero flash. **Delete nothing in this step.**

*Rollback: flip the macro to 0 and rebuild.*

### Step 6 — OLED text entry in the blocking pump

`readWizardLine()` gains: when `oledDisplay && oledConnected` and `f.kind` is `WIZ_FIELD_TEXT`/`WIZ_FIELD_SECRET`, source from `getOLEDTextInput(f.prompt, f.kind==WIZ_FIELD_SECRET, f.initialText, f.maxLength, &cancelled, true)`; when `WIZ_FIELD_SCANLIST`, from `getOLEDWiFiSelection()`. This restores every FTS OLED sub-flow with one branch instead of `handleOLEDESPNowPage`/`handleOLEDMQTTPage`/`renderWiFiPage`'s three private input loops.

*Rollback: same macro as Step 5.*

### Step 7 — Delete the legacy loop

**Only after a green erase-free FTS run on hardware, on both an OLED and a non-OLED board.** Remove `runSetupWizard` (`:1512-1782`), `handleSerialESPNowPage` (`:1093`), `handleSerialMQTTPage` (`:1198`), `handleSerialWiFiPage` (`:1384`), `handleSerialModePage`, `handleOLEDESPNowPage` (`OLED_SetupWizard.cpp:694`), `handleOLEDMQTTPage` (`:810`), `renderWiFiPage`'s input loop (`:413-505`), and the `WIZARD_USE_SHARED_ENGINE` macro.

*Rollback: `git revert` — but by this point the shared path is the only tested one, so treat Step 7 as irreversible in practice and gate it on hardware validation, not on a build.*

### Step 8 — DEFERRED, separate proposal: OLED text entry in the CLIMode driver

Closing the four-flow gap in `wizardMode_onTick`'s `default:` branch (`System_SetupWizardMode.cpp:919-932`) means calling a blocking OLED keyboard from a tick that `System_CLIMode.h:124-133` explicitly restricts to "sub-millisecond work only". `handleSystemInput` → `getOLEDTextInput` (`OLED_SetupWizard.cpp:559`) already violates this today, but making it load-bearing needs a decision, not a patch. **Do not bundle this into the unification.** Until then, the CLIMode driver stays text-only on WEBMODE/BTMODE/ESPNOW/MQTT/WIFI and should *say so on the display* rather than silently accepting only `nav.left`.

---

## 4. BEHAVIOUR THAT MUST BE PRESERVED

### The timeout asymmetry — stated first because it is the one that bricks

**Boot driver: `idleTimeoutMs == 0` means wait forever. No cancel word. No owner concept. The wizard cannot be exited except by completing it.**

This is not a default the core may supply — it is a *driver-supplied policy*, and the core must have no timeout of its own. Three different values exist today and none is the right one to inherit: legacy `0` = infinite (`setSerialWaitTimeout`, `System_Utils.cpp:723-727`); the CLIMode framework default `kDefaultIdleTimeoutMs` = 5 min (`System_CLIMode.cpp:46`); the wizard's override = 10 min (`System_SetupWizardMode.cpp:963`). If the shared core ever picks up the framework default, a user who walks away during first-time setup returns to a half-configured device with no users.json and no CLI. There is no recovery but a cable reflash.

Corollary: `'cancel'` must NOT abort FTS. If the boot driver accepts it at all it must mean "skip the remaining wizard pages" — FTS already tolerates `wifiConfigured=false` (`System_FirstTimeSetup.cpp:866-870`) — never "abandon first-time setup."

Second corollary: legacy's idle timer only advances *inside* `waitForSerialInputBlocking`; the top-level page loop uses `Serial.available()` + `delay(50)` (`:1658-1664`), so top-level pages never time out. That asymmetry is moot today (the only caller passes 0) and the unified pump should time out uniformly — **but only when the driver asked for a timeout.**

### Divergence table with chosen resolution

| # | Behaviour | Legacy | CLIMode | Chosen | Where fixed |
|---|---|---|---|---|---|
| D1 | MQTT skip → `mqttAutoStart=false` | yes `:1218` | **no** `Mode:630` | Legacy | Step 1 |
| D2 | ESP-NOW name `'n'`/empty | keeps current `:1132` | **writes `""`** `Mode:506` | Legacy | Step 1 |
| D3 | Blank MQTT port | leaves 0, gated `>0` `:1243/:1274` | writes 1883 `Mode:646` | Legacy | Step 1 |
| D4 | Heap summary at completion | printed `:1831-1839` | missing | Legacy | Step 1 |
| D5 | `wizardFinalize` on cancel | **runs** (gSettings + ~1s certgen) `:1779` | skipped `Mode:812` | **CLIMode** | Step 4 (commit gated on completion latch) |
| D6 | `idleTimeout == 0` | wait forever | 5-min framework default | **Per driver** — boot infinite, non-negotiable | Step 5 |
| D7 | Idle timer only ticks in blocking reads | yes | no (monotonic) | CLIMode, **only when a timeout was requested** | Step 5 |
| D8 | `'cancel'` word | absent | present | **Per driver** — CLI keeps; boot must not abort FTS | Step 5 |
| D9 | Exit on owner loss commits if `completed` | n/a | **yes** | Neither | Step 4: commit requires an explicit `sCompletionLatched` set only by the DONE transition, not by the `completed` flag surviving an involuntary exit |
| D10 | Terminal ESPNOW/MQTT page exits | yes | **infinite loop (N1)** | Legacy | Step 4 |
| D11 | `'n'` mid-walk exits when complete | yes | no (N2) | Legacy | Step 4 |
| D12 | Paint reaches all transports | n/a (serial by design) | **claims to, doesn't (N3)** | CLIMode's intent, **CLI driver only** | Step 3 |
| D13 | MQTT intro unrecognized input | skip (serial) / configure (OLED) | skip | **configure** (ESP-NOW convention) | Step 4 |
| D14 | MQTT password prompt | only if username entered `:1254` | always | Legacy | Step 4 |
| D15 | `espnowStationary` unconditional write | yes `:1187` | yes `Mode:514` | **Neither** — write only if the step was reached | Step 4 |
| D16 | OLED WEBMODE/BTMODE/ESPNOW/MQTT/WiFi | full handlers | **absent** | Blocking driver keeps them (Step 6); CLI driver stays text-only and says so | Steps 6 / 8 |
| D17 | OLED text entry from `onTick` | n/a | violates tick contract | Blocking driver structurally; CLI deferred | Step 8 |
| D18 | `onTick:935` resets sub-mode on any nav | n/a | yes | Neither | Step 4 |
| D19 | Serial + OLED simultaneously | supported (`:1664` + `:1733`) | impossible (`SOURCE_LOCAL_DISPLAY` gate, `System_CLIMode.cpp:284-286`) | **Per driver** — boot keeps dual input; CLI keeps exact-session ownership (a security property) | Step 5 |
| D20 | `deviceName` / `wifiEnabled` fields | written | written/not | Dead — delete | Step 1 |
| D21 | Result reaches the caller | by value | none | Core exposes `wizardEngineResult()`; CLI ignores it | Step 4 |
| D22 | Numeric selection unbounded | yes `:1702` | yes `Mode:424` | Neither — clamp | Step 1 |

### Boot-driver invariants (the must-NOT-change list)

1. `runAndApplyFeatureWizard()` runs **synchronously inside `setup()`** and must not return until done — phase-8 autostarts read what it wrote.
2. Infinite idle wait; no cancel that aborts FTS; no owner/session concept.
3. Serial **and** OLED joystick both live at once, with no authenticated session.
4. All six conditional pages retain a working OLED path *and* a working serial path, chosen by `oledDisplay && oledConnected` (`:1579/:1592/:1605/:1618`).
5. Commit order stays: finalize → WiFi upsert/sort/save + `wifiAutoStart=true` → `writeSettingsJson()` → `applySettings()` → `setupNTP()` if connected.
6. `result.wifiConfigured` + `result.wifiSSID` must reach `System_FirstTimeSetup.cpp:825`, and `gSettings.i2cEnabled` must be observable at `:875`. **FTS reads `gSettings` directly, not only the result struct** — the wizard's in-RAM mutations are part of the contract.
7. `gWizardOwnsSerial` stays set for the whole boot run. It has **two out-of-file consumers**: `HardwareOne.cpp:2716` and `System_UartLink.cpp:1070` (which parks the CM5 co-processor link on it). Do not let it leak into the core; do not change its lifetime.
8. `SETUP_IN_PROGRESS` is not widened; the wizard does not become async at boot.

---

## 5. PARITY BUGS FIXED ALONG THE WAY

**The four known (Step 1):**

- **B1** `System_SetupWizardMode.cpp:630-634` — MQTT skip must set `gSettings.mqttAutoStart = false` (legacy `:1218`). ESP-NOW already does the equivalent (`Mode:534`).
- **B2** `Mode:506-507` via `:556` — `ESPNOW_NAME` + `'n'` calls `applyAndAdvance()`, which unconditionally assigns `espnowFriendlyName` (still `""`) to both `bleDeviceName` and `espnowDeviceName`. **This is the only device-naming path on the primary build** — `systemPageHasDeviceName()` (`:266-272`) is live only when `ENABLE_ESPNOW == 0`. Resolve the effective name once before writing; never write an empty name.
- **B3** `Mode:646` — blank MQTT port writes 1883, passing the `>0` guard and clobbering a configured port. Keep the sentinel 0 = "unchanged"; apply 1883 at connect time.
- **B4** `Mode:815-817` — no heap summary. Moves into `wizardEngineCommit()`.

**Found during this analysis (Steps 1 and 4):**

- **N1/D10** CLIMode never reaches `DONE` from a terminal ESP-NOW or MQTT page; it repaints the intro until the 10-minute timeout, then commits anyway. **This is a blocker for the blocking driver, not a nicety.**
- **N2** `'n'` on `ESPNOW_NAME/ROOM/ZONE` and `MQTT_HOST/PORT/USER` returns `CLI_MODE_HANDLED` even after completion — requires a second Enter.
- **N3** The CLIMode wizard is invisible to non-serial transports: `paintCurrentPage()` (`Mode:134-142`) claims it "fans out to every connected transport", but `printSerialPageStatus()` is 22 raw `Serial.*` calls and `paintAfterTransition` returns `""` for pages 1-4 (`Mode:179`). A web-CLI user sees the header and nothing else.
- **N4/N5** MQTT intro unrecognized-input is three-way inconsistent; password prompt gating differs.
- **B6** `OLED_SetupWizard.cpp:384` uses `#ifndef ENABLE_ESPNOW`, but `ENABLE_ESPNOW` is always `#define`d (0 or 1) at `System_BuildConfig.h:515-527` — the guard is always false. On a non-ESP-NOW OLED build, `getWizardSystemPageCount()` counts a Device Name row that is never drawn → a selectable blank row.
- **B7** `OLED_SetupWizard.cpp:356` gates the NTP row on `isFeatureEnabled(wifi)`; the core's `systemPageHasNTP()` (`:254-257`) gates on `isFeatureCompiled`. WiFi compiled-but-disabled → the OLED System page mislabels every row at index ≥ 2 while `getSystemItemAt` acts on the core's numbering.
- **B8/D5** `System_SetupWizard.cpp:1779` calls `wizardFinalize(result)` unconditionally, outside the loop — a cancelled or timed-out FTS wizard still mutates `gSettings` and runs `cmd_certgen` (~1s ECDSA) and *then* prints "No changes saved." The message is false.
- **B9/N6** `setWizardCurrentSelection(num - 1)` is unbounded (`:1702`, `Mode:424`); `getSystemItemAt` falls through to `SYS_ITEM_TIMEZONE` for any out-of-range index (`:299`), so typing `99` on the System page silently cycles the timezone.
- **N7** `wizardFinalize` writes `tzOffsetMinutes` without `Clock::applyTimezone()` — the split-clock hazard its own comment at `:818-823` warns about.
- **D18** `Mode:935` clobbers sub-mode on any joystick nudge, discarding an in-progress field walk.
- **D9** `wizardMode_onExit` applies whenever `result.completed` is true, regardless of *why* it exited — combined with N1, a timeout commits settings.

---

## 6. DELETIONS — verified dead

| Item | Evidence |
|---|---|
| `printSerialHeapBar` `System_SetupWizard.cpp:905` | Only callers are the three below |
| `printSerialFeaturePage` `:920` | Zero references in `components/` or `main/` |
| `printSerialNetworkPage` `:939` | ditto |
| `printSerialSystemPage` `:957` | ditto |
| `runOLEDSetupWizard` `OLED_SetupWizard.cpp:1309` (+ decl `OLED_SetupWizard.h:61`) | Declared, defined, never called |
| `SetupWizardResult::deviceName` | Written `System_SetupWizard.cpp:1519`, `System_SetupWizardMode.cpp:775`; zero reads repo-wide |
| `SetupWizardResult::wifiEnabled` | Written `:845`, `:850`, `:1515`; zero reads |
| Legacy serial + OLED sub-page handlers | Step 7 only, after hardware validation |

**All five dead functions are already absent from the linked binary** — file-static unreferenced functions are discarded by the compiler and `--gc-sections` drops `runOLEDSetupWizard`. Deleting them **saves zero flash**. Do it for readability; do not count it toward any size budget.

**`sDummyResult` (`OLED_SetupWizard.cpp:516,529,540`) is latent, not live.** `wizardNextPage` writes `result` only on the SYSTEM page, on `next == WIZARD_PAGE_WIFI`, and at end-of-wizard; SYSTEM is unconditionally visible and follows NETWORK, so Features/Sensors/Network can never be the last page, and the only field the dummy eats is `wifiEnabled` — itself dead. Step 4 removes it by making the engine own its result. **Do not describe this as a bug fix.**

---

## 7. RISKS & WHAT COULD SILENTLY BREAK

### Flash size — the premise in the brief is stale, and this project is not a size play

The brief says "only 2% partition headroom." **That is no longer true.** `partitions.csv:26` reads `factory, app, factory, 0x10000, 0x5B5000` — the factory partition was grown +512K (the header comment is dated 2026-08-16, "was ~2% free"). Against the last built binary (`build-feathers3/hardwareone-idf.bin`, 4,961,600 B, **dated Aug 8 — stale, re-measure**), headroom is now **≈1,022,656 B / 17.1%**, not 2%.

Measured savings at Step 7 (from Aug-8 objects, ±10%):

| Symbol | text | rodata | literal | total |
|---|---:|---:|---:|---:|
| `handleSerialESPNowPage` | 1775 | 597 | 624 | 2,996 |
| `handleSerialMQTTPage` | 1379 | 308 | 472 | 2,159 |
| `handleSerialWiFiPage` | 1092 | 189 | 312 | 1,593 |
| `runSetupWizard` (incl. inlined `handleSerialModePage`) | 1373 | 164 | 440 | 1,977 |
| | | | | **8,725 B** |

Blocking pump adds ~300-500 B. **Net ≈ −8.2 KB on non-OLED builds — 0.8% of current headroom.** OLED builds would gain another ~3-5 KB at Step 7 (`handleOLEDESPNowPage`, `handleOLEDMQTTPage`, `renderWiFiPage`'s loop), but **that is unmeasured**: no build directory in the tree contains `OLED_SetupWizard.cpp.obj` — every current board config has OLED off. Build one OLED board before promising it.

**Honest conclusion: do this project for correctness and maintainability. Size is neither a justification nor a constraint.** Do not let a size argument push Step 7 ahead of hardware validation.

### Step 5 is the riskiest thing here, by a wide margin

1. **No on-device rollback.** FTS runs inside `hardwareone_setup()` before `hardwareone_loop()` exists (`HardwareOne.cpp:1842`, `System_OTASafety.cpp:198`). A hang, or a machine that never reaches `DONE`, leaves the device at a boot prompt with no CLI, no web, no OTA. Recovery is a cable reflash. **This is exactly why N1 must be fixed in Step 4, before the pump exists.**
2. **Double- or zero-apply.** `wizardEngineCommit()` has real side effects including a ~1s ECDSA `cmd_certgen` (`:889-897`). The pump must call it exactly once, on exactly the completed path. Two calls = two cert generations; zero = a silently unconfigured device.
3. **Ordering hazard with phase-8 autostarts.** `wizardNextPage` writes `tzOffsetMinutes` and calls `Clock::applyTimezone()` *mid-flow* (`:813-823`), not at finalize. Any change to *when* the SYSTEM page is left changes when the timezone lands relative to `applySettings()`/`setupNTP()`.
4. **Cancel-polling asymmetry.** Legacy polls `isWizardCancelRequested()` at the loop top (`:1560`) and inside each sub-handler (`:1174`, `:1268`, `:1400`, `:1441`). The pump must reinstate all of it, because `runAndApplyFeatureWizard(idleTimeoutMs)`'s signature still admits a non-zero timeout.

### I am deliberately NOT converting the boot driver's output to `broadcastOutput`

`docs/FTS_WIZARD_OUTPUT_PLAN.md` Step 1.1 proposes pointing *both* drivers at a broadcast sink. **Reject that for the boot driver.** `broadcastOutput` enqueues onto `gDebugOutputQueue`, drained asynchronously by `debugOutputTask` (`System_Debug.cpp:237`, `:691-706`). Three specific consequences at boot:

- **`enqueueChunk` drops silently on a full queue** (`System_Debug.cpp:809-814` — returns false, bumps a counter). Queue depth is 64 without PSRAM, 192 with (`System_Debug.h:196-197`). A page paint is ~25-30 lines; a dropped line is an invisibly missing menu row on a device with no other UI.
- **There is no flush primitive.** I found no `debugFlush`/`waitDebugDrain` anywhere in the component. A blocking pump that emits a prompt and immediately blocks on `Serial` has no way to guarantee the prompt reached the wire first.
- **Mixing enqueued and direct writes reorders output** — which is the underlying ordering bug the plan doc is chasing. Converting *all* the boot driver's output to one direct-Serial sink fixes ordering just as well, with none of the queue risk.

The CLIMode driver has none of these problems (it never blocks, and it is already 100% broadcast), so Step 3 gives it the broadcast sink and fixes N3 where it actually matters. The emitter indirection makes this a one-line-per-driver choice rather than an architectural fork.

### The OLED text-entry gap

Step 6 closes it for the blocking driver cheaply. Step 8 (CLIMode) is **deferred, not solved**, and the deferral has a cost: on an OLED build, a user running `featuresetup` from the display today hits WEBMODE/BTMODE/ESPNOW/MQTT/WIFI and can only press left. Leaving that unlabelled after unification is worse than before, because the surrounding code will look finished. If Step 8 is not taken, the `default:` branch (`Mode:919-932`) must render an explicit "configure this page from the serial or web console" message.

### What could break without anyone noticing

- **Board-gated code hides compile breaks.** `ENABLE_OLED_DISPLAY` is 0 on every current board config; `ENABLE_MQTT` is 0 at `System_BuildConfig.h:105`. Half the code this plan touches is not compiled by the default build. "Built green" proves only the current board.
- **`gWizardOwnsSerial`'s second consumer** (`System_UartLink.cpp:1070`, the CM5 link) is easy to miss when refactoring the driver.
- **`wizardShouldShowMQTT()`** (`:558-565`) returns false when `ENABLE_MQTT==0`, so **two of the four parity bugs cannot be exercised on the default build.** Either produce an `ENABLE_MQTT=1` bench build or accept B1/B3 as code-review-only fixes and say so.
- **Archetype seeding is not part of either wizard.** `applyArchetypeSeed()` (`:77-86`) is called only from `System_FirstTimeSetup.cpp:741`, does its own `writeSettingsJson()` at `:84`, and runs only in Basic mode where the wizard never runs. **Do not pull it into the core.**

---

## 8. TESTING

### FTS does NOT require `erase_flash` — set this up before Step 5

`detectFirstTimeSetupState()` keys purely on the existence of `/system/users/users.json` (`System_FirstTimeSetup.cpp:86-87`; path from `System_User.h:316`). So the loop is:

1. `filecopy /system/users/users.json /users.bak`
2. `filedelete /system/users/users.json confirm`
3. reboot → full Advanced FTS runs
4. restore the backup to skip FTS next boot

~30 seconds per cycle instead of erase-and-reflash. **This is the single highest-leverage prerequisite for the whole project.**

### Bench scaffold — also before Step 5

Add a **bench-only, never-committed** admin command `ftswizard` that calls `runAndApplyFeatureWizard(0)` from serial on a provisioned device, gated on `!cliInModeActive()`. It exercises the blocking pump, `gWizardOwnsSerial`, `waitForSerialInputBlocking`, the OLED keyboard path, and `wizardEngineCommit` — everything except `SETUP_IN_PROGRESS` and phase-8 ordering. That converts ~90% of Step 5's surface into a no-erase test.

### Per step

- **Step 1 — no erase.** Run `featuresetup` over serial on a provisioned device:
  - MQTT → skip → verify `mqttAutoStart == false` *(requires `ENABLE_MQTT=1` build)*
  - ESP-NOW → `c` → `n` at the name prompt → verify `bleDeviceName` unchanged (blanked today)
  - MQTT → `c` → host → blank at port → verify `mqttPort` retained *(requires `ENABLE_MQTT=1`)*
  - Complete → expect the `Heap estimate: ~N KB` line
  - Type `99` on the System page → must be rejected, must not cycle the timezone
  - OLED build: System page with WiFi compiled-but-disabled → row labels must match the actions taken (B7)
- **Step 2 — transcript diff, must be byte-identical** on serial.
- **Step 3 — serial AND web CLI.** Web must now show all pages (N3). Serial must still show all pages (the `ALLOW_IN_HELP` trap). Expect the timestamp-prefix delta; re-baseline the oracle.
- **Step 4 — no erase, structural diff must be empty** except the deliberate N1/N4/N5/D15 changes. Specifically test: WiFi disabled so ESP-NOW or MQTT is the last visible page → the wizard must **exit immediately**, not loop (N1). Enter a field walk, nudge the joystick → the walk must survive (D18).
- **Steps 5-6 — bench scaffold first**, then the real thing via the users.json loop, across: (a) headless serial, (b) OLED connected, (c) OLED build with the display physically absent, (d) `ENABLE_WIFI=0`, (e) `b`/back at every page boundary, (f) power-cut mid-wizard → next boot re-enters FTS cleanly, (g) **walk away for 20 minutes mid-wizard → the boot wizard must still be sitting at its prompt** (the timeout invariant).
- **Step 7 — full build-gate matrix.** Every board in `boards/`, plus one explicit `ENABLE_OLED_DISPLAY=1` build and one `ENABLE_MQTT=1` build. The OLED handler removals are otherwise invisible.

### Stated uncertainty

Nothing here was run on hardware. All byte counts derive from Aug-8 objects that no longer match the source (`wifiScanPrintNamed` has a different signature in the object than in the tree) — re-measure before quoting. OLED-side flash savings are unmeasured; no OLED build artifact exists in the tree. The `cliModeSuppressesAmbientSerial` swallow (`System_CLIMode.cpp:201` → `System_Debug.cpp:275-281`) reads correctly as described but has not been observed on a device.

**Key paths:** `/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_SetupWizard.{h,cpp}`, `System_SetupWizardMode.{h,cpp}`, `OLED_SetupWizard.{h,cpp}`, `OLED_FirstTimeSetup.{h,cpp}`, `System_FirstTimeSetup.cpp`, `System_FeatureRegistry.cpp`, `System_CLIMode.{h,cpp}`, `System_Debug.{h,cpp}`, `System_BuildConfig.h`, `/Users/morgan/esp/hardwareone-idf/partitions.csv`, `/Users/morgan/esp/hardwareone-idf/docs/FTS_WIZARD_OUTPUT_PLAN.md`.