# Amended Setup-Wizard Unification Plan

Working tree `main` @ `d3ad5354` (dirty). All paths under `/Users/morgan/esp/hardwareone-idf/components/hardwareone/` unless noted. Every line reference below was re-opened and read; none is inherited from the census or the attack reports.

---

## 1. VERDICT

**The shape survives. The API does not.**

One driver-neutral step machine plus two drivers (blocking-at-boot, async CLIMode) is still the right decomposition, and the load-bearing argument for it is confirmed: `cliEnterModePrepared()` bails at `System_CLIMode.cpp:167-172` when `currentOwner(requireInteractive=true)` fails, and FTS at boot has no `CommandContext` at all — so the shared thing cannot be the CLIMode, it must sit underneath. Nothing in the attack touches that.

What the attack breaks is the plan's **I/O contract**, in four specific places, each independently a blocker:

1. **Input is not always a line.** `wizardEngineInput(const String&)` is the core's only mutation entry point in §2c. The OLED joystick — the primary FTS input device on a display board — produces `(uint32_t buttons, JoystickNav)`, mutates the page model directly, and signals *wizard completion* by `handleSystemInput()` returning `false` (`OLED_SetupWizard.cpp:590-594`). None of that can be expressed as a `String`. The Step-5 pump blocks in `readWizardLine()` and the joystick is dead while it blocks (`System_Utils.cpp:735-750` polls only `Serial.available()`).
2. **`wizardEngineDone()` as specified is permanently false.** `subModeForPage()`'s only `DONE` producer is its `default:` (`System_SetupWizardMode.cpp:165`), reachable only if `getWizardCurrentPage()` leaves the 9 listed pages — and `wizardNextPage()` returns `false` at completion *without* assigning `currentPage` (`System_SetupWizard.cpp:842-848`; the assignment is at `:852`, on the other branch). `while (!wizardEngineDone())` is decorative, and D9's "latch set only by the DONE transition" would **zero-apply on the two most common completion paths** (`Mode:404-409` `n` at the last top-level page; `Mode:693-697` skip-WiFi).
3. **Reading is not describing.** `WizardFieldInfo.prompt` has exactly one possible source — `paintAfterTransition()` (`Mode:172-293`) — which paints ~25 lines, runs a **blocking WiFi scan** (`Mode:268-279`), and returns pointers into rotating `static char prompt[80]` buffers (`Mode:182`, `:225`).
4. **`WizardFieldKind` cannot represent four of the steps it exists to serve.** ESP-NOW stationary is a two-item picker (`OLED_SetupWizard.cpp:726-792`), the two intro cards are Configure/Skip pickers (`:603-688`), and WEBMODE/BTMODE is a numbered list whose labels live in `WizardModeMenu` (`System_SetupWizard.h`), unreachable from `{kind, prompt, initialText, maxLength}`.

None of these forces a different architecture. All four are fixed by widening the core's API (a nav entry point, an explicit DONE latch, a pure `promptFor()`, a `WIZ_FIELD_CHOICE` kind with an options vector) and by adding a driver-policy struct. **Proceed — with the amended API, the amended step order, and Step 5 split in two.**

The seeded error is confirmed and is not the only one: `debugWaitOutputDrained(uint32_t)` is defined at `System_Debug.cpp:545`, declared `System_Debug.h:285`, and called from `HardwareOne.cpp:955`. §7's "There is no flush primitive" is false. The §7 *decision* (direct-Serial sink for the boot driver) survives on its other two arguments, but the premise must be struck and a second §7 claim — "converting *all* the boot driver's output to one direct-Serial sink" — is also false (see D-06).

---

## 2. CONFIRMED DEFECTS IN THE PLAN

Ordered by severity. **[WRONG]** = the plan asserts something false. **[INCOMPLETE]** = the plan is silent on something load-bearing.

### BLOCKERS

---

**D-01 — [INCOMPLETE] The pump has no input channel for the OLED joystick; Step 7 then deletes it.**

*Evidence.* The legacy top-level loop never blocks: `delay(50)` at `System_SetupWizard.cpp:1658`, non-blocking `Serial.available()` at `:1662`, joystick block at `:1730-1776`, all in the same lap. That interleave *is* boot invariant 3. `waitForSerialInputBlocking()` (`System_Utils.cpp:735-750`) polls only Serial. Step 6 adds OLED sourcing only for `WIZ_FIELD_TEXT`/`SECRET`/`SCANLIST` — the four top-level pages are `WIZ_FIELD_NONE`. Step 7 removes `runSetupWizard` (`:1512-1782`) including `:1730-1776`.

A nuance all four attackers missed and which makes this worse: **on `ENABLE_OLED_DISPLAY` builds the legacy loop routes even *serial* `n`/`b` through the joystick handlers** with a synthetic `JoystickNav` (`:1670-1697`), so on an OLED build the completion signal for the serial path is *also* `handleSystemInput() == false`, not `wizardNextPage()`. A pump that only feeds lines to `wizardEngineInput` changes completion semantics on OLED builds even for a headless serial user.

*Amendment.* Add a second, non-line entry point to §2c:

```cpp
CLIModeInputResult wizardEngineNav(uint32_t newButtons, const JoystickNav& nav);
```

implemented by the same `handleFeaturesInput` / `handleSensorsInput` / `handleNetworkInput` / `handleSystemInput` calls both drivers already share, with `handleSystemInput()==false` mapped onto the same `wizardGoDone(true)` latch as the text path (see D-02). Change `readWizardLine()` from a blocking read to a **poll lap that owns the 50 ms quantum**:

```cpp
// returns kind=NONE when nothing arrived this lap; the pump loops
WizardInputEvent readWizardEvent(const WizardFieldInfo& f);
```
servicing, in order: Serial → joystick (`readWizardJoystickNav()` + the `SensorCacheGuard` edge-detect from `:1734-1753`) → OLED repaint → `delay(50)`. Reserve the *blocking* read (`waitForSerialInputBlocking` / `getOLEDTextInput`) for field steps only (`ESPNOW_*`, `MQTT_*`, `WIFI_*`), which is exactly what the legacy handlers already do.

Add invariant: **the boot driver must never block on a single input source; every wait must service serial, joystick and OLED repaint in the same lap.**

---

**D-02 — [WRONG] `wizardEngineDone()` is unreachable, and the Step-4 N1 fix reaches only 2 of 9 exit sites.**

*Evidence.* Nine sites terminate the machine. Only two are the `goNextPage()` lambdas the plan patches:

| site | what it does | touches `subMode`? |
|---|---|---|
| `Mode:334-338` | cancel word | no |
| `Mode:404-409` | `n` at last top-level page — **the normal completion** | no |
| `Mode:461-464` | `n` at last mode page | no |
| `Mode:498-503` | ESP-NOW `goNextPage` | re-derives → intro |
| `Mode:605-610` | MQTT `goNextPage` | re-derives → intro |
| `Mode:693-697` | WiFi skip / blank — **the other normal completion** | no |
| `Mode:725-727` | `#else` WiFi-not-compiled | no |
| `Mode:740-743` | WiFi password captured | no |
| `Mode:912-916` | `onTick` `handleSystemInput()==false` | no |

Plus `Mode:384-388`, the `DONE` arm, which sets `completed = true` on *any* input — dead today only because DONE is unreachable, and made live by the plan's own N1 fix.

The plan's N1 diagnosis is correct (see §3, R-01) but its prescription is not: after "make `goNextPage()` set DONE", `wizardEngineDone()` is true on two rare ESP-NOW/MQTT-terminal paths and false on the four common ones. D9's "commit requires `sCompletionLatched` set only by the DONE transition" then **refuses to commit a normal completed FTS run** — the plan's own §7 risk #2, "zero = a silently unconfigured device", on a device whose only recovery is a cable reflash.

*Amendment.* Make DONE the single termination fact, set explicitly:

```cpp
static void wizardGoDone(bool ok) {
  sWizard.subMode = WizardSubMode::DONE;
  sWizard.result.completed = ok;
  sCompletionLatched = ok;          // D9's latch, set HERE and only here
}
```
Call it from **all nine sites**. Delete `completed = true` from `Mode:387` (replace with a bare `HANDLED_AND_EXIT` that neither commits nor cancels). Define `wizardEngineDone()` as `subMode == DONE`; make `wizardEngineCommit()` assert `wizardEngineDone() && sCompletionLatched` and be idempotent (`sCommitted` guard) so the pump-after-loop call and any `onExit` call cannot run `cmd_certgen` twice.

**D18 is now a prerequisite, not a nicety.** `onTick:935` re-derives `subMode = subModeForPage(getWizardCurrentPage())` unconditionally after any joystick input, which would clobber DONE back to `ESPNOW_INTRO`. Because `wizardMode_onExit` is executor-affine (`System_CLIMode.cpp:383-387`) and drained from `cmd_exec_task` with a 1000 ms queue wait (`HardwareOne.cpp:729`), there is a real ≤1 s window between the DONE transition and the commit during which one joystick nudge discards the completion. Fix D18 in the same commit as D-02, and state the dependency.

*Rejected alternative, recorded so it is not re-proposed:* setting `currentPage = WIZARD_PAGE_COUNT` on `wizardNextPage`'s completion branch (`System_SetupWizard.cpp:843-848`) would make every re-derivation converge on DONE for free. It is elegant and it is also a wider blast radius — `wizardRetreatFrom` (`:687-695`) returns `current` for COUNT, `printSerialPageStatus`'s `default:` silently paints nothing, and the legacy OLED render `default: running = false` at `:1642` changes meaning. Explicit latch wins.

---

**D-03 — [WRONG] The core's `'cancel'` keyword contradicts the plan's own non-negotiable FTS invariant, and §2c has no hook to disable it.**

*Evidence.* `Mode:334-339` tests `cancel` **before** the sub-mode switch and unconditionally. §4 states "`'cancel'` must NOT abort FTS" and lists D8 as "per driver" — but no mechanism exists to make it per-driver: not `wizardEngineInput`'s signature, not `wizardEngineEnter()`, not `WizardFieldInfo`. Moving the function into the shared TU *gives FTS an abort word*. Because the test precedes the switch it also makes the literal string `cancel` unusable as a WiFi SSID, MQTT host, MQTT username, ESP-NOW room/zone, or device name — true today on the CLI path, and it would spread to FTS. `onEnter` even advertises it (`Mode:794`), a banner the boot driver must not print.

*Amendment.* Add an explicit driver-policy struct, supplied at entry:

```cpp
struct WizardPolicy {
  bool allowCancelWord;     // CLIMode true, boot false
  bool cancelWordAtFieldsToo; // false: 'cancel' is only legal at NONE/MENU/CHOICE steps
  bool allowAbort;          // boot false — see D-04
};
void wizardEngineEnter(const WizardPolicy&);
```
Gate `Mode:334` on it, and gate the `onEnter` banner too. Recommend `cancelWordAtFieldsToo = false` for **both** drivers so free-text fields become opaque.

---

**D-04 — [INCOMPLETE] §2d's abort semantics are false at boot: FTS persists the wizard's live `gSettings` mutations unconditionally, and Step 6 makes abort user-reachable for the first time.**

*Evidence, two independent facts the plan never states.*

(a) `System_FirstTimeSetup.cpp` calls `writeSettingsJson(); applySettings();` on the completion path **regardless of `wizardResult.completed`** (the block after `broadcastOutput("FIRST-TIME SETUP COMPLETE!")`), and reads `bool i2cDisabledByUser = !gSettings.i2cEnabled;` live from the global, not from the result struct. The wizard's feature toggles are raw aliases into `gSettings` — `featuresPage[n].setting = f->enabledSetting` (`System_SetupWizard.cpp:396`), `*item->setting = !*item->setting` (`:725-726`) — and `"i2c"` is bound to `&gSettings.i2cEnabled` (`System_FeatureRegistry.cpp:352-355`), on wizard page 1. Skipping `wizardEngineCommit()` on abort un-writes **nothing**.

(b) Abort is unreachable at boot today: the sole caller passes the default `idleTimeoutMs = 0` (`System_FirstTimeSetup.cpp:824`, default at `System_SetupWizard.h:206`), so `sWizardCancelRequested` is never set (`System_Utils.cpp:741-745`), and there is no cancel word on the legacy path. **Step 6's `getOLEDTextInput(..., &cancelled, /*canSkip=*/true)` introduces the first user-triggered abort at boot.**

Compose them and the failure is: user toggles I2C off on page 1, aborts on an OLED keyboard, commit is skipped, FTS continues, users.json is written (setup is now permanently "complete" — `detectFirstTimeSetupState()` keys purely on that file), `writeSettingsJson()` persists `i2cEnabled=false`, reboot. Next boot `isFirstTimeSetup()` is false so `initI2CBuses()` no longer force-enables (`System_I2C.cpp:386-392`), and an OLED-only unit has no display, no gamepad, and — if WiFi was also skipped — no network.

*Amendment.* Two parts.

1. **The boot driver has no abort path.** Delete both `isWizardCancelRequested()` branches from the Step-5 pump for the boot driver (they are provably dead there), pass `canSkip=false` to `getOLEDTextInput`, and map the OLED keyboard's `cancelled` to **BACK**, never to "leave the wizard" (see D-05).
2. **Rewrite §2d honestly.** Only two fields are finalize-only: `gSettings.ntpServer` (`System_SetupWizard.cpp:875`) and `gSettings.ledStartupEffect` (`:880`). Everything else is a live mid-flow write: `tzOffsetMinutes` + `Clock::applyTimezone()` + `logLevel` + both device-name fields on leaving SYSTEM (`:813-834`); `httpsEnabled`/`bleMode` on every mode keystroke (`wizardModeApply :615`); six ESP-NOW fields and four MQTT fields at page-apply (`Mode:506-514`, `:612-615`; legacy `:1182-1187`, `:1273-1276`). So D5's chosen resolution buys "abort skips certgen and skips persistence" — a real improvement — and **not** "tz / NTP / LED / device names / MQTT / ESP-NOW are not dirty". Say that.

Add **boot invariant 9**: *the wizard's feature toggles are applied to `gSettings` live and are persisted by `System_FirstTimeSetup.cpp`'s post-wizard `writeSettingsJson()` whether or not the wizard completed. Abort is not a rollback at boot.*

---

### HIGH

---

**D-05 — [INCOMPLETE] Routing OLED text entry through `getOLEDTextInput` collapses three intents into one empty string, and the core reads empty as three different destructive commands.**

*Evidence.* `getOLEDTextInput` returns `""` for a blank field, for `'n'` when `canSkip` (`OLED_FirstTimeSetup.cpp:110`), and for `'b'`/B-press (`:112-118`, `:190-194`) — distinguished only by the `wasCancelled` out-param, which the Step-5 pump never reads. The core's meaning of empty varies per step: `WIFI_SSID` empty → **complete and exit the whole wizard** (`Mode:693-697`); `ESPNOW_INTRO` empty → **disable ESP-NOW** (`Mode:531-534`); `MQTT_PORT` empty → write 1883 (`Mode:646`, i.e. B3 through the front door); `ESPNOW_NAME` empty → keep current (`Mode:559`); `ESPNOW_ROOM` empty → write empty (`Mode:567`). Pressing B to back out of the WiFi SSID field on an OLED FTS board would end setup as "completed".

*Amendment.* Change the pump's input contract from `String` to a small result type and give the core matching entry points, so the driver never synthesizes magic words:

```cpp
struct WizardInputEvent {
  enum Kind : uint8_t { NONE, TEXT, BACK, SKIP, NAV, CHOICE, TIMEOUT } kind;
  String   text;      // TEXT
  uint8_t  choice;    // CHOICE
  uint32_t buttons; JoystickNav nav;  // NAV
};
CLIModeInputResult wizardEngineBack(void);
CLIModeInputResult wizardEngineSkip(void);
CLIModeInputResult wizardEngineChoice(uint8_t idx);
```
Call `getOLEDTextInput` with `canSkip=false` so it stops eating `'n'`. Add to Step 6 an explicit 11-row table: for each field sub-mode, what BACK, SKIP and empty-TEXT mean — and assert `WIFI_SSID` BACK ≠ SKIP.

---

**D-06 — [WRONG] Step 3's `MSG_ROUTE_ALL` discards owner scoping and starts writing wizard repaints to flash. These are not "two known cosmetic deltas".**

*Evidence.* `broadcastOutputCore` computes `route = (mask & (SERIAL|WEB|FILE|BLE)) | MSG_ROUTE_OLED | MSG_ROUTE_G2` from the caller's `CommandContext` (`System_Debug.cpp:983-987`), and `routeOverride` short-circuits that entirely (`:977-979`). `MSG_ROUTE_ALL` is `0x3F` (`System_Debug.h:32`) — six sinks, including `MSG_ROUTE_FILE` `0x04` and `MSG_ROUTE_BLE` `0x10`. Consequences the plan does not list: a web-owned `featuresetup` would paint the physically-attached serial console and the BLE lane; and every ~25-30-line page repaint would reach the system-log file writer (`System_Debug.cpp:299`), which today it never does.

The `MSG_ROUTE_ALLOW_IN_HELP` half of the recipe **is** correct and mandatory — confirmed at `System_Debug.cpp:274-282`, where a `MSG_ROUTE_SERIAL` line without that bit is swallowed into the suppressed ring while a serial-owned mode is active, and `sSuppressAmbientSerial` is cleared only *after* `onExit` returns (`System_CLIMode.cpp:410`), so even the exit banner is affected.

*Amendment.* Build `broadcastSetupOutput` on the owner-scoped mask:

```
route = (ownerMask & (SERIAL|WEB|FILE|BLE)) | MSG_ROUTE_OLED | MSG_ROUTE_G2 | MSG_ROUTE_ALLOW_IN_HELP
```
`getCurrentCommandOutputMask()` reads the calling task's slot and is valid only on `cmd_exec_task` (where `onInput` runs). `onTick` runs on the main loop, so **capture the owner mask into a static in `wizardMode_onEnter` and use the captured value from every paint site.** Restate the Step-3 deltas: (i) serial lines gain `[%lu] ` (`:288`), which will visibly disturb the aligned `%-5s` / `%2dKB` columns at `:1006`/`:1054`; (ii) the `"> "` prompt gains a terminator — but that delta belongs to Step 2, not Step 3 (see D-07).

---

**D-07 — [WRONG] Step 2 cannot be byte-identical: the 22 calls emit three different terminators.**

*Evidence.* Seven are `Serial.println(...)` → **CRLF** (`Print::println(void)` is `print("\r\n")`, `components/arduino/cores/esp32/Print.cpp:169`; the string form is `print(c) + println()`, `:179`): `System_SetupWizard.cpp:996, 997, 1072, 1077, 1084, 1085, 1087`. Fourteen are `Serial.printf("...\n", ...)` → **LF**: `:1001, 1006, 1016, 1021, 1031, 1037, 1046, 1052, 1054, 1057, 1061, 1065, 1071, 1076`. One is `Serial.print("> ")` → **no terminator** (`:1089`) — the live input prompt. A `wizardEmit(const char* line)` whose sink appends one terminator cannot reproduce all three, and putting the `\n` in the payload breaks Step 3's queue sink (`broadcastOutputCore` enqueues one call as one verbatim message, `System_Debug.cpp:1000-1003`, and the drain prints one `[%lu] %s\n` per message, `:288`).

*Amendment.* Add a second primitive to §2a: `void wizardEmitRaw(const char* text); // no terminator appended`, used only at `:1089`. Then declare the **expected** Step-2 diff up front: the 14 `printf` lines change LF → CRLF. Harmless on every consumer in the tree, but it must be written down as the expected result, not treated as a failed refactor. Move the `"> "` delta from Step 3 to Step 2 in the plan text. Also specify the capture method: raw bytes off the port (`cat /dev/tty… > baseline.bin`) compared with `cmp`/`xxd | diff` — a terminal-log `diff` will not surface a CRLF change, which is precisely the change Step 2 is most likely to introduce.

---

**D-08 — [WRONG] `paintAfterTransition()` is a side-effecting painter, not a describer, so `wizardEngineField()` cannot be built on it; and the pump would double-prompt.**

*Evidence.* `Mode:268-279` performs a blocking WiFi scan and mutates engine state (`wifiNamedCount`, `wifiScanValid`) inside the prompt-returning function. `Mode:178` calls `paintCurrentPage()` (~25 lines). `Mode:192-205`, `:215-221`, `:245-251` emit headers and option rows. Prompts are returned as pointers into rotating `static char prompt[80]` buffers (`:182`, `:225`) — a second call rewrites the buffer under any held `WizardFieldInfo.prompt`. Separately, `appendPromptTo()` (`:314-326`) already delivers the next prompt in `out`, and `getOLEDTextInput` prints its prompt itself (`OLED_FirstTimeSetup.cpp:94`), so passing `f.prompt` duplicates it.

*Amendment.* Split before the move (Step 4, not Step 5):
- `static const char* promptFor(WizardSubMode)` — pure, no emission, no scan, single source for `WizardFieldInfo.prompt`; prompts that interpolate go into an engine-owned `sCurrentPrompt[80]` latched at transition, never a returned static.
- `static void paintFor(WizardSubMode)` — emission and the WiFi scan only, called on transition.

`appendPromptTo` = `paintFor` + `promptFor`. `wizardEngineField` = `promptFor` only. Decide the prompt channel once and write it into Step 5: **the core keeps appending the prompt to `out`; the blocking driver passes `nullptr` for the OLED keyboard's prompt.** Populate `initialText` at the same three sites (`Mode:180`, `:225-228`, the device-name path).

---

**D-09 — [WRONG] `WizardFieldInfo` cannot express the four choice-picker steps, so Step 6 is a UX regression and Step 7 orphans two functions.**

*Evidence.* (i) ESP-NOW stationary is a bespoke two-item joystick picker with a serial branch (`OLED_SetupWizard.cpp:726-792`); the core models it as free text matched against `s`/`stationary` (`Mode:580`). (ii)(iii) Both intro cards are Configure/Skip pickers via `showWizardOptionalPageIntro` returning 1/0/-1 (`:603-688`, called at `:696` and `:812`); `WIZ_FIELD_MENU` names the case but carries no labels and no return mapping. (iv) WEBMODE/BTMODE labels live in `WizardModeMenu::modes[i].label`, reachable only from inside `paintAfterTransition` (`Mode:190-202`) — no kind fits. Step 7's removal list omits `handleModePage` (`OLED_SetupWizard.cpp:869`, sole caller `System_SetupWizard.cpp:1580`) and `showWizardOptionalPageIntro`, both of which lose their only callers when the Step-7 deletions land, so D16's promise that "the blocking driver keeps them" is unfulfilled as written.

*Amendment.* Add `WIZ_FIELD_CHOICE` and widen the descriptor:

```cpp
struct WizardFieldInfo {
  WizardFieldKind kind;
  const char*     prompt;        // never NULL
  const char*     initialText;   // "" if none
  int             maxLength;     // per-field, see D-14
  const char* const* options;    // CHOICE/MENU only
  uint8_t         optionCount;
  uint8_t         optionCurrent;
};
```
Populate `options` from the Mobile/Stationary pair, from `{Configure, Skip}` for the two intros, and from `WizardModeMenu::modes` for WEBMODE/BTMODE. Driver returns `wizardEngineChoice(idx)`. **Until `WIZ_FIELD_CHOICE` exists, Step 6 keeps `handleOLEDESPNowPage` / `handleOLEDMQTTPage` / `handleModePage` / `showWizardOptionalPageIntro` and Step 7 does not delete them.**

---

**D-10 — [WRONG] The Step-5 pump discards every terminal message the engine produces.**

*Evidence.* The skeleton at plan `:206-207` does `if (wizardEngineInput(...) == CLI_MODE_HANDLED_AND_EXIT) break;` **before** `if (out[0]) wizardEmit(out);`. Every exit path writes a user-facing string into `out` first: `Mode:336` "Wizard cancelled…", `:408` and `:463` "Feature configuration complete.", `:696` "WiFi configuration skipped.", `:725` "WiFi not compiled in this build.", `:741` "WiFi credentials captured for '%s'." On an FTS device with no other UI the user's final Enter would produce no acknowledgement at all.

*Amendment.* Reorder: `r = wizardEngineInput(...); if (out[0]) wizardEmit(out); if (r == CLI_MODE_HANDLED_AND_EXIT) break;`. Add a Step-5 test asserting the final line appears for complete-at-System, complete-at-WiFi-password, and skip-at-WiFi.

---

**D-11 — [INCOMPLETE] The §8 `ftswizard` bench scaffold can roll back and ABORT an unverified OTA image.**

*Evidence.* `runAndApplyFeatureWizard(0)` blocks forever by construction and, invoked as a CLI command, parks `cmd_exec_task`. `sCmdExecHeartbeatMs` is refreshed only at the top of the receive loop (`HardwareOne.cpp:709-713`, with an in-tree comment saying exactly this). `hardwareone_loop()` passes `cmdHeartbeatAge <= 5000U` into `otaSafetyLoopHeartbeat` (`:2958-2961`), so `coreHealthy` goes false within 5 s; `otaSafetyLoopHeartbeat` then resets the healthy window every lap (`System_OTASafety.cpp:305-310`) and `kProbationHardLimitTicks` (5 min, `:52`) fires at `:135-139` → `rebootPendingImage(OTA_PROBATION_HEALTH_TIMEOUT)`. Per the in-tree rationale at `HardwareOne.cpp:1842-1849`, an ABORTED image "needs a full re-provision to clear". The existing rescue does not apply: `otaSafetyAcceptProvisioningBoot()` is called only under `filesystemReady && isFirstTimeSetup()` (`:1853`), false on the provisioned bench device the scaffold targets. Secondary: the scaffold sets `gWizardOwnsSerial` (`System_SetupWizard.cpp:1807`), which parks the CM5 host link for the whole run (`System_UartLink.cpp:1070`).

*Amendment.* Gate the scaffold on a validated image — add a trivial `otaSafetyProbationActive()` accessor and refuse with `"Error: running an unverified OTA image; wait for probation to end (see otastatus)"`. Document the CM5-link park in §8. Do not "fix" this by refreshing the heartbeat from inside the pump; that weakens a real safety signal.

---

### MEDIUM

---

**D-12 — [INCOMPLETE] Field-level vs page-level back is a real divergence against *both* legacy drivers and is absent from D1-D22.**

Core: `'b'` inside a field walk retreats one field — `Mode:565` ROOM→NAME, `:572` ZONE→ROOM, `:579` STATIONARY→ZONE, `:644` PORT→HOST, `:652` USER→PORT, `:659` PASS→USER. Legacy serial: `'b'` at any of those calls `wizardPrevPage(); return;` — the whole page (`System_SetupWizard.cpp:1148-1150, 1157-1159, 1166-1168, 1239-1242, 1248-1251, 1258-1261`). Legacy OLED: same, on `cancelled` (`OLED_SetupWizard.cpp:718, 723, 826, 831, 836, 842`). Adopting the core silently changes FTS back-navigation on six steps in a step whose heading says "no behaviour change".

*Amendment.* Add row **D23** — *back inside ESPNOW/MQTT field walk: legacy page-level, CLIMode field-level, chosen **field-level** (it is what every prompt string already advertises, e.g. `Mode:233`); fixed in Step 4.* Remove "no behaviour change" from Step 4's heading. Test: `'b'` at MQTT_USER must land on MQTT_PORT, not on SYSTEM.

---

**D-13 — [INCOMPLETE] Three more divergences absent from the table.**

(a) **OLED ESP-NOW commits the device name before the page is finished.** `OLED_SetupWizard.cpp:713-714` writes `gSettings.bleDeviceName` and `gSettings.espnowDeviceName` immediately after the Name field; Room/Zone/Stationary can still abort at `:718`, `:723`, `:788-790`, and the page's final apply block (`:796-799`) never rewrites the names. Serial legacy (`:1182-1183`) and CLIMode (`Mode:506-507`) write at page-apply. **Chosen: page-apply.** Note it in Step 6 as a fix, so nobody re-adds the early write while porting.

(b) **D3 is three-way, not two.** Serial legacy leaves `mqttPort` at 0 (`:1243`, gated `>0` at `:1274`); CLIMode writes 1883 (`Mode:646`); **OLED legacy prefills the keyboard with the literal `"1883"`** (`OLED_SetupWizard.cpp:830`), so an OLED user accepting the visible default *does* write 1883. §2b's single `const char* initialText` cannot express "prefill on the keyboard, sentinel on the line". **Chosen:** make `initialText` the *current* value (`gSettings.mqttPort` when non-zero, else `"1883"`) on every driver and treat blank as "unchanged" uniformly — which fixes B3 without lying to either user. This makes `initialText` per-invocation, not a compile-time literal; say so in §2b.

(c) **Per-field keyword vocabulary is inconsistent four ways.** Legacy serial MQTT implements no skip word: typing `n` at Host stores the literal `"n"` into `result.mqttHost` (`:1234`), which passes the `length() > 0` guard at `:1273` and writes `gSettings.mqttHost = "n"` — same at Username (`:1252`→`:1275`) and Password (`:1262`→`:1276`), while the page's own instructions advertise only "Press Enter to use defaults, 'b' to go back" (`:1224`). CLIMode honours `'n'` at MQTT_HOST/PORT/USER (`Mode:638, 645, 653`) but **not** at MQTT_PASS (`:658-664`). Legacy ESP-NOW honours `'n'` at Room/Zone (`:1151, 1160`) but not at Stationary (`:1163-1169`). **Chosen: uniform in the core** — `'b'`=back, `'n'`=skip-and-finish-page, blank=keep-current, everything else=value, at every text field including MQTT_PASS and ESPNOW_STATIONARY; update every prompt string to advertise exactly that set.

---

**D-14 — [WRONG] `maxLength` inventory.**

Real caps: 20 for ESP-NOW name/room/zone (`OLED_SetupWizard.cpp:709, 717, 722`), 40 MQTT host (`:824`), 5 MQTT port (`:830`), 32 MQTT user/pass (`:835, 841`), 64 WiFi password (`:437`), 20 device name (`:559`). §2b's "32 default; 21 for device name" would silently truncate a working 40-char MQTT host and would let the keyboard accept a 21st device-name character that both writers then drop (`wizardDeviceName[21]` at `System_SetupWizard.cpp:196`; `strncpy(buf, …, 20); buf[20]='\0'` at `Mode:357-358` and `OLED_SetupWizard.cpp:562-563`). Also: `getOLEDTextInput` honours `maxLength` only on the keyboard path — its serial branch caps at `OLED_KEYBOARD_MAX_LENGTH` (`OLED_FirstTimeSetup.cpp:118-120`) and `waitForSerialInputBlocking` has no cap at all. Populate per-field, derive the device name from `sizeof(wizardDeviceName)-1`, and delete the stray `isSecret` member from §1's architecture box (§2b carries secrecy in `kind`).

---

**D-15 — [WRONG] D1's fix makes the MQTT page unreachable by `'b'`; ESP-NOW already is.**

`wizardShouldShowMQTT()` gates on `isFeatureEnabled(mqtt) && gSettings.mqttAutoStart` (`System_SetupWizard.cpp:562-565`) and `wizardRetreatFrom` filters on `wizardIsPageVisible` (`:687-695`). The instant the skip handler sets `mqttAutoStart = false`, the page vanishes — `'b'` from the next page cannot return, and `getWizardTotalPages()` (`:697-703`) shrinks under the running "SETUP n/N" counter. The identical trap already exists for ESP-NOW in all three engines: the feature's `enabledSetting` is `&gSettings.espnowEnabled` (`System_FeatureRegistry.cpp:239-242`) and every skip path clears it (legacy `:1115`, `Mode:534`, OLED `:700`). *Amendment:* record the skip in the result (`result.mqttSkipped` / `result.espnowSkipped`) and apply the disable inside `wizardEngineCommit()`. Page visibility and the page counter then stay stable for the whole run. Add a Step-1 test: skip MQTT, press `'b'` on the next page, land back on MQTT.

---

**D-16 — [WRONG] §7's "converting *all* the boot driver's output to one direct-Serial sink" is false, and its "no flush primitive" bullet is the seeded error.**

`wifiScanPrintNamed()` — the numbered network list the user must read to answer the WiFi prompt — writes exclusively through `broadcastOutput` (`System_SetupWizard.cpp:1352, 1358, 1364, 1367, 1372, 1375`) and stays in the page model by the plan's own TU boundary; the core reaches it at `Mode:276`, so §2a's "the core never calls broadcastOutput" is false transitively. `wizardFinalize`'s certgen lines do the same (`:895-896`). Under the blocking driver the WiFi list would be enqueued while the surrounding header and prompt go straight to the UART — exactly the mixed ordering §7 says it is avoiding, on the one page where a dropped or reordered line makes the prompt unanswerable. And `debugWaitOutputDrained` exists (`System_Debug.cpp:545`, decl `System_Debug.h:285`, caller `HardwareOne.cpp:955`), so the remedy §7 rules out is available.

*Amendment.* Route `wifiScanPrintNamed`'s six calls and `wizardFinalize`'s two through `wizardEmit` — this makes §2a true and lets the sink decide — **and** have the boot driver call `debugWaitOutputDrained(120)` immediately before every blocking read as a belt-and-braces against anything else that reaches the queue. Strike the "no flush primitive" bullet; keep the other two (`enqueueChunk` silent drops at `System_Debug.cpp:809-814`, queue depth 64/192 at `System_Debug.h:196-197`; enqueued/direct interleaving), which independently justify the direct-Serial choice.

---

**D-17 — [WRONG] Step 5's compile gate does not give a working rollback, and as written it cannot be flipped to 0.**

The plan literally writes `#define WIZARD_USE_SHARED_ENGINE` with no value; valueless + `#if` is a preprocessor error, and with `#ifdef` "flip it to 0" leaves it defined. This is the same bug class the plan itself files as B6 (`OLED_SetupWizard.cpp:384` `#ifndef ENABLE_ESPNOW`, always false because the macro is always `#define`d 0 or 1). Separately, `#if`/`#else` compiles exactly one arm, so from Step 5 to Step 7 the parked legacy arm never sees a compiler; and flipping the macro requires a full CMake reconfigure (`CMakeLists.txt:2` registers `System_BuildConfig.h` as `CMAKE_CONFIGURE_DEPENDS`), rebuild, and cable reflash — the same operations as `git revert`.

*Amendment.* Write `#define WIZARD_USE_SHARED_ENGINE 1` and gate with `#if`. Add an explicit `WIZARD_USE_SHARED_ENGINE=0` build to the Step-5/5b/6 gates so the parked arm is proven to compile. Restate the rollback honestly: "edit the header, full reconfigure, rebuild, cable reflash."

---

**D-18 — [WRONG] Step 1 is not decoupled from Step 7, and D5's fix does not land in Step 4.**

(a) `runOLEDSetupWizard()`'s entire body is `return runSetupWizard();` (`OLED_SetupWizard.cpp:1309-1311`). Step 1 deletes it; Step 7 deletes `runSetupWizard`. So Step 1 is a **prerequisite** of Step 7, and a post-Step-7 `git revert` of Step 1 restores a call to a deleted symbol. Both breakages are invisible because the whole TU sits inside `#if ENABLE_OLED_DISPLAY` (`:11`) and `DISPLAY_TYPE 0` (`System_BuildConfig.h:153`) forces it off on every board. *Amendment:* move the `runOLEDSetupWizard` deletion out of Step 1 into Step 7; the other five Step-1 deletions genuinely are independent.

(b) D5's "Where fixed: Step 4" is wrong for the boot driver. `runSetupWizard` calls `wizardFinalize(result)` **unconditionally, outside the loop** (`System_SetupWizard.cpp:1779`), and that stays live until Step 5 swaps the call. Change D5's cell to "Step 4 (CLI driver) / Step 5 (boot driver)". Note also that Step 5's "changes exactly one internal call … then `wizardEngineCommit()` in place of its inline apply block" erases two distinct messages: `:1818-1822` "Feature setup timed out. No changes saved." and `:1824-1827` "Feature setup cancelled. No changes saved." `wizardEngineCommit()`'s single bool collapses them; the driver must keep the distinction.

---

**D-19 — [INCOMPLETE] `handleSerialModePage` has no cancel poll and busy-loops on timeout; §7's cancel-site inventory is short.**

`System_SetupWizard.cpp:1468-1504` is a blocking sub-handler with its own `while (true)` and zero `isWizardCancelRequested()` checks. Once the flag is set, `waitForSerialInputBlocking` returns `String()` immediately on every call without a `delay` (`System_Utils.cpp:741-746`; `sSerialWaitLastActivityMs` is never refreshed), empty is not `b`/`n`/a valid number, and `sel == lastPrinted` suppresses the reprint — a tight CPU-bound loop, forever, on `cmd_exec_task`. Latent today (sole caller passes 0) and tripped by the §8 bench scaffold or any future non-zero timeout. `handleModePage` (`OLED_SetupWizard.cpp:869+`) has the same gap. *Amendment:* correct the §7 inventory, and make the unified pump the single cancel-polling point — one check per lap, with `readWizardEvent` returning a `TIMEOUT` kind — so no sub-handler needs its own poll. Record that this *fixes* the spin rather than reproducing it.

---

**D-20 — [WRONG] Step 8's deferral rationale cites a style contract; the real cost is an OTA rollback, and it is live today.**

`cliModeTick()` runs from `hardwareone_loop()` (`HardwareOne.cpp:2697`); `otaSafetyLoopHeartbeat()` is the loop's last statement (`:2960`). A blocking tick stops the heartbeat, and `probationSupervisor` reboots for rollback after `kLoopHangTimeoutTicks` = 30 s (`System_OTASafety.cpp:51`, `:127-132`) in both `Running` and `MarkingValid`. `wizardMode_onTick:912` calls `handleSystemInput`, which on an A-press over the Device Name row calls `getOLEDTextInput` (`OLED_SetupWizard.cpp:557-559`) — an unbounded `while (oledKeyboardIsActive())` loop (`OLED_FirstTimeSetup.cpp:101+`). A user who takes >30 s to type a device name into an OLED-owned `featuresetup`, on a board still in probation, loses the image.

*Amendment.* Restate the cost in §7. **Fix the live instance now instead of deferring it:** the core already has the non-blocking representation (`WizardSubMode::PAGE_SYSTEM_DEVICENAME`, entered at `Mode:427-431`), so the tick should set that sub-mode and let the field be typed from the owning text transport. ~10 lines, in Step 4. Note the `default:` branch is not the one that blocks — `PAGE_SYSTEM` is.

---

**D-21 — [INCOMPLETE] Step 4 is labelled "no behaviour change" while landing six, and §8's allowed-diff list names only four.**

Step 4 lands N1/D10, N2, N4/D13, N5/D14, D15 and D18. §8 lists N1/N4/N5/D15 — **N2 and D18 are missing**, so a diff caused by either reads as an unexplained regression or gets waved through. `ENABLE_MQTT 0` (`System_BuildConfig.h:105`) makes N4/N5/B1/B3 untestable on every current board, so an `ENABLE_MQTT=1` bench build is a **hard gate** on Step 4, not a parenthetical. Also correct N2's mechanism: it does **not** "fall out of the same fix" — the five `applyAndAdvance()` sites (`Mode:556, 566, 573, 638, 653`) hard-return `CLI_MODE_HANDLED` with no DONE test, unlike the four that do (`:537, 583, 633, 663`). Change them explicitly.

---

### LOW

---

**D-22 — [INCOMPLETE] Dead state and a stale error string.**

- `sWizard.espnowConfiguring` (`Mode:107`) is written at `:523`, `:545`, reset at `:780`, and **read nowhere** — `grep -rn espnowConfiguring components/hardwareone` returns exactly those four lines. Its comment claims it gates the skip path, which `Mode:531-534` does unconditionally. Add it to the §6 deletions table; delete in Step 1.
- `cmd_featuresetup` returns `"Error: another interactive mode is active. Type 'exit' or 'cancel' first."` for *every* `setupWizardMode_start()` failure (`System_FeatureRegistry.cpp:664-666`), including the "no live interactive session" bail at `System_CLIMode.cpp:167-172`. An admin running `featuresetup` over MQTT / ESP-NOW / BLE / UART / G2 / an automation is told a mode is active when none is. This is the single most user-visible artifact of the transport matrix and it is absent from the plan. Fix in Step 1: have `setupWizardMode_start()` return a reason enum and report it.
- The comment block above `cmd_featuresetup` (`System_FeatureRegistry.cpp:645-664`) asserts FTS keeps the legacy path "because … the OLED + joystick integration there is already correct" — must be rewritten at Step 5, not left as a fossil.
- Three stale comments claim `cmd_featuresetup` installs a 60 s timeout: `System_SetupWizard.cpp:1398`, `System_SetupWizard.h:204-205`, `System_Utils.cpp:713-714`. It does not; the whole timeout/cancel path is unreachable. Fix in Step 1.
- `wizardMode_onExit` prints `"Timezone: " + result.timezoneAbbrev` (`Mode:817`) before `wizardFinalize` refreshes it (`:823`). Cosmetic ordering only — see R-04.

---

**D-23 — [INCOMPLETE] Missing invariant: `isFirstTimeSetup()` must stay true for the whole wizard.**

`initI2CBuses()` force-enables the bus on `bool forceForSetup = isFirstTimeSetup();` (`System_I2C.cpp:386-392`), overriding a persisted `i2cEnabled == false`. That override is the only reason a power-cut retry still has a display, and the commit writes `settings.json` (`System_SetupWizard.cpp:1854`) *before* FTS writes `users.json` — so a cut in that window leaves `i2cEnabled: false` durable with setup still incomplete. Add **boot invariant 10**: *the shared core must never write `users.json`, never call `setFirstTimeSetupState()`, and never tear down I2C/OLED in place, at any point before it returns to `System_FirstTimeSetup.cpp:825`.* Make §8 test (f) specific: cut power after the completion banner but before "Saved /system/users/users.json", with I2C toggled OFF, and verify the next boot re-enters FTS **with a working OLED**.

---

## 3. FINDINGS I REJECT

**R-01 — "N1 is wrong / the DONE guards are live."** Not asserted by any attacker, but worth recording: **the plan's N1/D10 diagnosis is correct and is its strongest finding.** `wizardNextPage` returns `false` at `System_SetupWizard.cpp:843-848` before the `currentPage = next` at `:852`, `subModeForPage(WIZARD_PAGE_ESPNOW)` is `ESPNOW_INTRO` (`Mode:162`), and the ternaries at `Mode:583`/`:663` can never select `HANDLED_AND_EXIT`. It is reachable on the shipping profile: `ENABLE_MQTT 0` hides MQTT, and `wizardShouldShowWiFi()` (`:802-809`) returns false the moment the user unchecks WiFi on page 1, leaving ESP-NOW terminal in `kPageOrder`. Keep the finding; broaden the fix per D-02.

**R-02 — "`sDummyResult` means joystick transitions never reach the engine's result" (Attacker 3).** Rejected as a live defect; the plan's assessment is right. `wizardNextPage` writes to `result` only when `currentPage == WIZARD_PAGE_SYSTEM` (`:813-834`), when `next == WIZARD_PAGE_WIFI` (`:850`), and at end-of-wizard (`:845-846`). `handleFeaturesInput`/`handleSensorsInput`/`handleNetworkInput` (`OLED_SetupWizard.cpp:530, 546, 550`) are only ever on FEATURES/SENSORS/NETWORK, and `WIZARD_PAGE_SYSTEM` is unconditionally visible (`wizardIsPageVisible` `default: return true`) and follows all three in `kPageOrder` — so none of them can be terminal, and the only field the dummy eats is `wifiEnabled`, itself dead. It is a **latent trap, not a bug**. Do pass the engine's real result once the core owns the nav path (D-01) — it costs three lines — but do not describe it as a fix, and do not let it justify keeping the joystick out of the core.

**R-03 — "Abort at boot bricks the device today" (Attacker 3, as framed).** The mechanism is real (D-04) but the reachability claim is not: `sWizardCancelRequested` is only set when `sSerialWaitTimeoutMs > 0` (`System_Utils.cpp:741-745`), the sole FTS caller passes the default 0 (`System_FirstTimeSetup.cpp:824`, `System_SetupWizard.h:206`), and the legacy path has no cancel word. Nothing is broken today. **Step 6 is what makes it reachable** — which is why the amendment lives in Step 6, not in a hotfix.

**R-04 — "The completion timezone line is stale" (Attacker 4, part c).** Rejected as a defect. `result.timezoneAbbrev` is written by `wizardNextPage` on leaving SYSTEM (`:815-816`), *before* the advance check at `:842`, and SYSTEM is unconditionally visible and cannot be skipped — so by the time any completion path runs, the field is already correct. `wizardFinalize`'s re-write at `:869-870` is idempotent. Ordering should still be tidied for readability, but it is not a behaviour change and must not be listed as one in the Step-4 oracle.

**R-05 — "Step 1's four `printSerial*` deletions could cascade" (implicitly cleared by Attacker 2; independently re-checked).** They are safe. All four are file-static with no external references; the only edges are `printSerialFeaturePage`/`NetworkPage`/`SystemPage` → `printSerialHeapBar` (`:923, 942, 960`). Every file-static they touch has a surviving user: `logLevelNames` `:133` via `getLogLevelNames()` `:235`; `timezoneSelection`/`logLevelSelection` `:192-193` via the accessors at `:231-234`; `networkPage`/`networkPageCount` throughout `rebuildNetworkSettingsPage`; `ntpPresets`/`ledEffects`/`wizardDeviceName` via `printSerialPageStatus`. `SetupWizardResult::deviceName`/`::wifiEnabled` are confirmed write-only (`:845, 850, 1515, 1519`; `Mode:775`). No amendment needed for these six items.

**R-06 — "Step 5's gate buys nothing, remove it" (Attacker 2's stronger form).** Partially rejected. The gate's *rollback* value is oversold (D-17), but it has real value the attacker misses: it keeps the legacy and shared paths simultaneously present in the tree across the hardware-validation window, so a bench operator can A/B two flashes from one checkout. Keep the gate; fix the macro form; add the `=0` build to the gate matrix; rewrite the rollback sentence.

**R-07 — "`enqueueChunk` silent drops make broadcast unusable at boot" (plan §7, re-checked).** Stands. `System_Debug.cpp:809-814` returns false and bumps a counter; depth is 64 without PSRAM (`System_Debug.h:196-197`). The flush primitive existing does not repair a *dropped* line. Direct-Serial for the boot driver remains correct.

---

## 4. THE AMENDED STEP SEQUENCE

Each step is one commit, build-green on every board **plus** the two off-profile builds named in its gate. No incremental commits mid-step — finish, hardware-test, then commit.

### Prerequisites (do these FIRST, before Step 1)

**P0 — Fast FTS loop.** `detectFirstTimeSetupState()` keys purely on `/system/users/users.json` existing, so: `filecopy` it aside → `filedelete … confirm` → reboot → full Advanced FTS → restore. ~30 s per cycle, no erase. *This is the single highest-leverage item in the whole project and the plan is right to say so — but it must precede Step 1, not Step 5.*

**P1 — Bench scaffold, moved from Step 5 to here.** Bench-only, never committed: admin `ftswizard` calling `runAndApplyFeatureWizard(0)`, gated on `!cliInModeActive()` **and** `!otaSafetyProbationActive()` (D-11). Document that it parks the CM5 link via `gWizardOwnsSerial` (`System_UartLink.cpp:1070`). Having it *before* Steps 1-4 means every step can be exercised against the blocking driver, not only the CLI one.

**P2 — Off-profile bench builds.** One `ENABLE_MQTT=1` and one `DISPLAY_TYPE`≠0 / `ENABLE_OLED_DISPLAY=1` board config. Without these, `dispatchMQTT`, every OLED handler and every joystick path are uncompiled (`System_BuildConfig.h:105`, `:153`→`:481`; `CMakeLists.txt:378` gates `OLED_SetupWizard.cpp` on `HW_CFG_BUILD_OLED_MODES`) and "built green" proves nothing about half the code this plan touches.

**Step 0 — Baseline oracle.** Build every board via `tools/build_board.sh`; record `.bin` sizes. Capture **raw byte** transcripts (`cat /dev/tty… > baseline.bin`, no line-discipline translation) of: (a) FTS Advanced on a headless board, (b) FTS Advanced on the OLED bench board driven by joystick only, (c) a full `featuresetup` walkthrough on serial, (d) the same on web CLI. Compare later with `cmp` / `xxd | diff`, never `diff` on a terminal log (D-07).

---

### SAFE TO DO NOW (no FTS path change; validated with P0/P1 on the bench)

**Step 1 — Independent fixes + dead-code removal.**
- Delete `printSerialHeapBar` `:905`, `printSerialFeaturePage` `:920`, `printSerialNetworkPage` `:939`, `printSerialSystemPage` `:957`. **Do NOT delete `runOLEDSetupWizard` here** — moved to Step 7 (D-18a).
- Delete `SetupWizardResult::deviceName`, `::wifiEnabled`, and `sWizard.espnowConfiguring` (`Mode:107`) with its false comment (D-22).
- Parity bugs B1/B2/B3/B4 — with D-15's amendment: B1 records `result.mqttSkipped` and applies `mqttAutoStart=false` at commit, not at keystroke; same for ESP-NOW's existing equivalent. B3 per D-13b: `initialText` = current value, blank = unchanged, everywhere.
- B6 `#ifndef ENABLE_ESPNOW` → `#if !ENABLE_ESPNOW` (`OLED_SetupWizard.cpp:384`); B7 `renderSystemPage` (`:328-392`) drives its rows from `getSystemItemAt()`/`getWizardSystemPageCount()`.
- B9/N6 clamp in `setWizardCurrentSelection`.
- `setupWizardMode_start()` returns a reason; `cmd_featuresetup` reports the real cause (D-22).
- Delete the three stale timeout comments (`System_SetupWizard.cpp:1398`, `System_SetupWizard.h:204-205`, `System_Utils.cpp:713-714`).

*Gate:* default board + `ENABLE_MQTT=1` + OLED build. *Rollback:* `git revert`; genuinely independent of later steps once `runOLEDSetupWizard` is out of it.

**Step 2 — Sink indirection.** Add `wizardEmit` / `wizardEmitf` / **`wizardEmitRaw`** / `wizardEngineSetSink`, default = direct `Serial`. Convert `printSerialPageStatus()`'s 22 calls, using `wizardEmitRaw` at `:1089`. Also route `wifiScanPrintNamed`'s six `broadcastOutput` calls (`:1352-1375`) and `wizardFinalize`'s two (`:895-896`) through `wizardEmit` (D-16), which is what makes §2a true.
*Expected diff, declared in advance:* the 14 `printf` lines change LF → CRLF. Everything else byte-identical. *Gate:* raw-byte `cmp` against Step-0 (a) and (c).

**Step 3 — CLIMode driver switches sink; fixes N3.** `broadcastSetupOutput` built on the **owner-scoped** mask + `MSG_ROUTE_ALLOW_IN_HELP`, with the owner mask captured in `onEnter` for use from `onTick` (D-06). *Gate:* serial **and** web CLI must both show all pages; system-log file must not grow during a wizard walkthrough.

**Step 4 — Relocate the step machine + terminal-state and sub-page fixes** *(retitled — this step lands eight behaviour changes)*.
- Move `WizardSubMode`, `sWizard`, `subModeForPage`, `dispatch*` into `System_SetupWizardCore.cpp`.
- **Split `paintAfterTransition` into pure `promptFor()` + emitting `paintFor()`** with an engine-owned `sCurrentPrompt[80]` latch (D-08). Do this *before* the move, as its own hunk.
- **`wizardGoDone(bool)` called from all nine exit sites**; `sCompletionLatched`; delete `completed=true` at `Mode:387`; `wizardEngineDone()` = `subMode==DONE`; `wizardEngineCommit()` asserts latch and is idempotent (D-02).
- **D18 as prerequisite**: `onTick:935` re-derives only when `getWizardCurrentPage()` actually changed (D-02).
- N2 at all five `applyAndAdvance` sites explicitly (D-21).
- N4/D13, N5/D14, D15, plus the uniform keyword vocabulary (D-13c) and the field-level-back decision recorded as **D23** (D-12).
- **Device-name A-press on `PAGE_SYSTEM` sets `PAGE_SYSTEM_DEVICENAME` instead of calling `getOLEDTextInput` from the tick** (D-20).
- Fold `Clock::applyTimezone()` into `wizardFinalize` (N7).

*Allowed-diff list for §8, exhaustive:* N1, N2, N4, N5, D15, D18, D23, and the device-name tick change. *Gate:* `ENABLE_MQTT=1` build is **hard**; OLED build required for D18/D-20. Specific tests: WiFi disabled so ESP-NOW is terminal → must exit immediately; reach terminal ESP-NOW, complete, nudge the joystick before the exit drains → commit exactly once; `'b'` at MQTT_USER → MQTT_PORT.

---

### REQUIRES HARDWARE VALIDATION FIRST (the FTS path)

**Step 5a — Blocking driver, top-level pages only** *(new — Step 5 is split)*.
Point `runSetupWizardBlocking()` at the shared core for FEATURES / SENSORS / NETWORK / SYSTEM only. Leave `handleSerialESPNowPage` / `handleSerialMQTTPage` / `handleSerialWiFiPage` / `handleSerialModePage` and their OLED twins in place, called exactly as today. The pump:

```cpp
SetupWizardResult runSetupWizardBlocking(void) {
  wizardEngineSetSink(nullptr);                 // direct Serial
  wizardEngineEnter({/*allowCancelWord=*/false,
                     /*cancelWordAtFieldsToo=*/false,
                     /*allowAbort=*/false});    // D-03, D-04
  // wizardEngineEnter paints header + first page and latches the first prompt
  char out[256];
  while (!wizardEngineDone()) {
    WizardFieldInfo f; wizardEngineField(&f);
    WizardInputEvent ev = readWizardEvent(f);   // 50 ms lap: serial + joystick + repaint
    CLIModeInputResult r;
    switch (ev.kind) {
      case WizardInputEvent::NONE:   continue;
      case WizardInputEvent::NAV:    r = wizardEngineNav(ev.buttons, ev.nav); break;
      case WizardInputEvent::BACK:   r = wizardEngineBack();   break;
      case WizardInputEvent::SKIP:   r = wizardEngineSkip();   break;
      case WizardInputEvent::CHOICE: r = wizardEngineChoice(ev.choice); break;
      default:                       r = wizardEngineInput(ev.text, out, sizeof out); break;
    }
    if (out[0]) wizardEmit(out);                // D-10: emit BEFORE break
    if (r == CLI_MODE_HANDLED_AND_EXIT) break;
  }
  return wizardEngineResult();
}
```
`wizardEngineEnter()` **paints** (header + first page + first prompt) — the plan's skeleton went straight from enter to read, which would leave an FTS user at a blank console on a device with no other UI.
`runAndApplyFeatureWizard()` keeps its signature, `gWizardOwnsSerial` (`:1807/:1811`) and `setSerialWaitTimeout`, swaps `runSetupWizard()` → `runSetupWizardBlocking()`, and calls `wizardEngineCommit()` while keeping the two distinct early-exit messages at `:1818-1827` (D-18b).
Gate: `#define WIZARD_USE_SHARED_ENGINE 1` + `#if` (D-17), with a `=0` build in the matrix. **Delete nothing.**

**Step 5b — Sub-pages onto the core.** Migrate ESPNOW / MQTT / WiFi / WEBMODE / BTMODE to the core's field walk, with `WIZ_FIELD_CHOICE` implemented (D-09) and `readWizardEvent` covering `WIZ_FIELD_MENU` and `WIZ_FIELD_NONE` on OLED, not only TEXT/SECRET/SCANLIST. Preserve `getOLEDTextInput`'s **dual serial+OLED** behaviour explicitly — it falls back to `waitForSerialInputBlocking()` when `isOLEDAvailable()` is false (`OLED_FirstTimeSetup.cpp:84-88`) and still polls `Serial.available()` inside the keyboard loop (`:104-121`); an FTS run on an OLED board can be completed entirely from a serial terminal today, and that must not be lost to a later "simplification". Note the three different availability predicates in the tree (`oledDisplay && oledConnected` at `System_SetupWizard.cpp:1579/1592/1605/1618`; `gOledRunning && oledConnected` at `System_FirstTimeSetup.cpp:707/752`; all three at `OLED_FirstTimeSetup.cpp:56-58`) and **unify on `isOLEDAvailable()`** — the mismatch is a live boot-hang: with `gOledRunning` false, `runSetupWizard` enters the OLED WiFi page, `getOLEDWiFiSelection` falls back to the serial prompt, Enter returns false → `wizardPrevPage()` → SYSTEM → `'n'` → WiFi, forever, with no timeout.

**Step 6 — OLED text/choice sourcing polish.** `canSkip=false` everywhere; `cancelled` → BACK; per-field `maxLength` (D-14); WiFi SSID skip/back disambiguated — add `getOLEDWiFiSelectionEx(String&, WizardScanOutcome&)` so the OLED selector can report SKIP and BACK separately (today `false` means BACK in `renderWiFiPage:448-450` but `skip` at `OLED_FirstTimeSetup.cpp:584-587` also returns `false`, while serial and CLIMode both treat `skip` as *finish the wizard* — `System_SetupWizard.cpp:1415-1420`, `Mode:693-697`). **Chosen: serial's meaning** (`skip` = finish, `b` = previous page).

*Gate for 5a / 5b / 6:* bench scaffold first, then the P0 users.json loop, across (a) headless serial, (b) OLED connected, (c) OLED build with the display physically absent, (d) **OLED board with the serial cable physically unplugged, joystick only — full Advanced FTS must complete** (new; the only test that catches D-01), (e) `ENABLE_WIFI=0`, (f) `b`/back at every page boundary *and inside every OLED text field*, (g) power-cut after the completion banner but before "Saved users.json" with I2C toggled OFF → next boot re-enters FTS **with a working OLED** (D-23), (h) walk away 20 minutes mid-wizard → still at its prompt, (i) toggle I2C off then abort → `settings.json` must still say `i2cEnabled: true` (D-04).

**Step 7 — Delete the legacy loop.** Only after green erase-free FTS runs on both an OLED and a non-OLED board. Remove `runSetupWizard` (`:1512-1782`), `handleSerialESPNowPage` (`:1093`), `handleSerialMQTTPage` (`:1198`), `handleSerialWiFiPage` (`:1384`), `handleSerialModePage` (`:1468`), `handleOLEDESPNowPage` (`OLED_SetupWizard.cpp:694`), `handleOLEDMQTTPage` (`:810`), **`handleModePage` (`:869`)**, **`showWizardOptionalPageIntro` (`:603`)**, `renderWiFiPage`'s input loop (`:413-505`), `runOLEDSetupWizard` (`:1309` + decl `.h:61`), and the gate macro. *Gate:* every board in `boards/` plus the OLED and MQTT bench builds. Treat as irreversible in practice.

**Step 8 — DEFERRED: OLED text entry in the CLIMode driver.** Unchanged in scope, corrected in rationale (D-20). Until taken, the `default:` branch (`Mode:919-932`) must render "configure this page from the serial or web console" rather than silently accepting only `nav.left`. Note this is currently **uncompiled and untestable** on every board, and the only route to an OLED-owned wizard is typing `featuresetup` into the OLED CLI-Input keyboard (`OLED_Mode_CLIInput.cpp:241`) — **no OLED menu entry for the wizard exists anywhere**.

---

## 5. INTERFACE MATRIX

Legend: ✔ works · ✖ blocked · ◐ partial · **⚠ would lose capability under the plan as written**

### Engine A — BLOCKING (FTS at boot)

| Interface | Today: start / input / output | Merged design MUST preserve |
|---|---|---|
| **Boot step** (only starter) | ✔ / — / — ; no auth of any kind (`HardwareOne.cpp:1856` → `System_FirstTimeSetup.cpp:369`) | Synchronous inside `setup()`; returns before phase-8 autostarts; `SETUP_IN_PROGRESS` not widened |
| **Serial** | ✖ / ✔ every page / ✔ raw Serial | Every page reachable from a terminal with **no display present**; infinite wait; no cancel word; no abort (D-03, D-04) |
| **OLED + joystick** | ✖ / ✔ every page / ✔ panel | **⚠ LOSES the four top-level pages under Step 5+6 as written** (D-01). Restored only by `wizardEngineNav` + polling `readWizardEvent`. Also: `handleSystemInput()==false` must remain a completion signal (D-02) |
| **Dual serial+OLED at once** | ✔ (`:1662` + `:1730` same lap) | **⚠ LOST by any blocking single-source read.** New invariant: never block on one source |
| **Web / BLE / ESP-NOW / MQTT / UART / G2 / voice / automations** | ✖ / ✖ / ✖ — none exist before `hardwareone_loop()` | No change; do not invent one |

### Engine B — CLIMODE (`featuresetup`)

| Interface | Today | Merged design MUST preserve / fix |
|---|---|---|
| **Serial** | ✔ start, ✔ text, ◐ **broken output** — `printSerialPageStatus` visible raw, every `broadcastOutput` swallowed by `sSuppressAmbientSerial` (`System_CLIMode.cpp:201-202` → `System_Debug.cpp:274-282`), including the whole `onExit` commit banner | Step 3 fixes it — `MSG_ROUTE_ALLOW_IN_HELP` is **mandatory**, and the `onExit` sink needs it too (the flag clears only after `onExit` returns, `System_CLIMode.cpp:410`) |
| **Web CLI** (`/api/cli`, `interactive=1`) | ✔ start, ✔ text, ◐ **never sees a page** — `paintAfterTransition` returns `""` for all four top-level pages (`Mode:174-179`) and they paint via raw Serial | N3 fixed in Step 3 with the **owner-scoped** mask, not `MSG_ROUTE_ALL` (D-06) |
| **OLED + joystick** | ◐ start (only by typing `featuresetup` on the CLI-Input keyboard — no menu entry exists), ◐ input (joystick on pages 1/4/5/6; `default:` handles only `nav.left`, `Mode:919-932`), ✔ output | Gap stays (Step 8), but must be **labelled**. Fix the device-name tick blocker now (D-20) |
| **G2 glasses** | ✖ start (`MODE_INDEPENDENT`, `G2_HijackCmd.cpp:179`) / ✖ input / ✔ **can see it** — G2 bit force-added to every command route (`System_Debug.cpp:987`) | Owner-scoped mask must keep force-adding OLED|G2 (it does); do not regress to `MSG_ROUTE_ALL` |
| **BLE** | ✖ / ✖ / ◐ observe only | unchanged |
| **UART / CM5** | ✖ (`System_UartLink.cpp:992`) / ✖ / ✖ | `gWizardOwnsSerial` must **not** leak into the CLI driver — Engine B does not set it today, and extending it would park the CM5 link for the wizard's duration (`System_UartLink.cpp:1070`) |
| **ESP-NOW / MQTT / voice / automations** | ✖ — `transportSessionEpoch` never assigned → 0, blocked at `System_CLIMode.cpp:75` | unchanged; **but the error message must stop lying** (D-22) |
| **Exact-session ownership** | ✔ `sameOwner` (`:313`), owner-loss exit (`:121-128`) | A security property. Boot keeps dual input; CLI keeps ownership. Per-driver, never unified |

**Interfaces that would lose capability under the plan as written: OLED joystick on the boot driver (all four top-level pages), and dual serial+joystick simultaneity at boot.** Both are D-01. Everything else either holds or improves.

---

## 6. RESIDUAL RISK — what cannot be settled without hardware

1. **Nothing here was run.** No board was built. Every claim above is a static read of the tree at `d3ad5354` (dirty).
2. **Half of what this plan touches is uncompiled today.** `ENABLE_MQTT 0` (`System_BuildConfig.h:105`) and `DISPLAY_TYPE 0` (`:153` → `ENABLE_OLED_DISPLAY 0` at `:481`) mean `dispatchMQTT`, every OLED handler, `showWizardOptionalPageIntro`, `handleModePage` and every joystick path are absent from the current binary. P2's off-profile builds are the only thing that converts "built green" into evidence. Until they exist, D-01, D-09, D-12, D-13a and D-20 are unfalsifiable on the bench.
3. **OLED-owned Engine B framebuffer contention is unresolved.** `oledUpdate()` (`HardwareOne.cpp:2680`) and `cliModeTick()` (`:2697`) run in the same lap, and `updateOLEDDisplay` is dirty-gated. Whether the user sees stable wizard pages or a fight between the wizard and the CLI-Input result screen cannot be determined from source. Treat "OLED can drive Engine B" as *architecturally permitted, empirically unverified*.
4. **The `sSuppressAmbientSerial` swallow** (`System_CLIMode.cpp:201` → `System_Debug.cpp:274-282`) reads exactly as described and is the basis of D-06's `ALLOW_IN_HELP` requirement, but has not been observed on a device.
5. **Flash figures are stale.** The plan's Step-7 savings table derives from Aug-8 objects that no longer match the source (`wifiScanPrintNamed` has a different signature in the object than in the tree). Re-measure before quoting. OLED-side savings are entirely unmeasured — no build directory contains `OLED_SetupWizard.cpp.obj`. **Size is neither a justification nor a constraint for this project**; that part of §7 is correct and should not be softened.
6. **The `~106 Serial.print*` figure was not re-counted.** I verified the 22 in `printSerialPageStatus` (`:990-1090`) and the concentration in the four serial sub-page handlers; the total is inherited.
7. **Queue-drop behaviour at boot is untested.** Whether `debugOutputTask` is even running at the point FTS executes — which decides whether §7's remaining two arguments bite hard or not at all — I did not trace. It affects the *severity* of D-16, not its existence.
8. **`MSG_ROUTE_G2` delivery** additionally requires `isG2Connected()` (`System_Debug.cpp:422`); when that is true during a wizard session is untraced.
9. **Timing of the `onExit` drain window** (D-02's ≤1 s clobber window) is inferred from `pdMS_TO_TICKS(1000)` at `HardwareOne.cpp:729`; the actual worst case under load is unmeasured.
---

# Appendix — Prerequisites BUILT and VERIFIED (2026-08-16)

Both gates from §6 (Residual Risk) are now satisfied. No wizard code has been changed.

## P1 — Off-profile compile coverage: `tools/build_coverage.sh`

**Baseline result: GREEN, zero compile errors.** The OLED handlers, gamepad/joystick paths
and every MQTT branch compile clean today — so there are **no pre-existing breaks** in the half
of the wizard the shipping profile hides. This is the baseline every later step is measured against.

The script flips `I2C_FEATURE_LEVEL 0→4`, `DISPLAY_TYPE 0→1`, `INPUT_DEVICE_TYPE 0→1`,
`ENABLE_MQTT 0→1`, builds, then restores `System_BuildConfig.h` **byte-identically (md5-verified)**
via an `EXIT` trap, so a Ctrl-C or a failed build cannot leave the user's live flags altered.

```bash
tools/build_coverage.sh            # default board xiao_s3
tools/build_coverage.sh feathers3
```

It also prints a **coverage proof**, because "built green" does not prove the files were compiled —
a gated-out TU still produces an object:

```
    OLED_SetupWizard            426760 bytes      <- real content
    OLED_FirstTimeSetup         388312 bytes
    System_MQTT                2263960 bytes
    (contrast: a genuinely empty TU, e.g. G2_Page_LED, is ~2272 bytes)
```

Run this at **every** step, not just at the end. Half of what this project touches is invisible to
`tools/build_board.sh`.

## P2 — No-erase FTS loop — **the plan's version does not work**

§8 of the v1 plan prescribes `filecopy /system/users/users.json /users.bak`. **There is no
`filecopy` command.** The registered file commands are `filecreate`, `filedelete`, `fileread`,
`filerename`, `files`, `fileview`, `filewrite` (`System_Filesystem.cpp:1344` and neighbours).

The mechanism still holds — `detectFirstTimeSetupState()` keys purely on
`USERS_JSON_FILE` = `/system/users/users.json` (`System_User.h:316`) — but use `filerename`:

```
filerename "/system/users/users.json" "users.bak"     # FTS will run on next boot
reboot                                                 # full Advanced FTS
```

To get back to the pre-test admin afterwards, delete the freshly written `users.json` and rename
`users.bak` back. For repeat wizard testing the backup is optional — re-running setup is the point:

```
filedelete "/system/users/users.json" confirm
reboot
```

~30 s per cycle, versus an erase-and-reprovision. **Note `filerename`'s second argument is a new
NAME, not a path** — it renames within the same directory.
