# `ENABLE_G2_GLASSES` bare-gate audit — decision report

*Repo: `/Users/morgan/esp/hardwareone-idf`, component `components/hardwareone/`. Four parallel auditors classified 31 sites; I re-read every load-bearing claim below and ran an independent preprocessor-nesting walk. Where I did not verify something myself, I say so.*

---

## 1. HEADLINE

**4 sites are worth changing. 27 are noise.** And there is a **one-line systemic fix** that closes 3 of the 4 plus the entire class.

The audit's framing was right about the hazard but wrong about its prevalence. I ran a complete census of bare gates (`grep` for `#if`/`#elif` on `ENABLE_G2_GLASSES` without `ENABLE_BLUETOOTH` on the same line) — **29 sites, exactly matching the audited set**, so coverage is complete. Then I walked the preprocessor nesting of each. Result:

| Effective gate on a BT-off board | Count |
|---|---|
| Already inside a BT-gated region (file-level or nested) → **equivalent to the compound gate** | 27 |
| Genuinely unguarded → bare gate really is TRUE with BT off | **2** |

Plus two sites that a `#if` grep does not catch:
- **`WebServer_Server.cpp:3032`** — the macro used as a *runtime value*, not a gate. This one ships wrong data today.
- **`WebPage_Bluetooth.h:20`** — an `#if !ENABLE_BLUETOOTH` fallback that cannot be selected in any config.

**The most important correction to the task premise:** the brief states that a `#if !ENABLE_G2_GLASSES` fallback "NEVER compiles, on any board." That is **not true**, and acting on it would delete live shipping code. `Bluetooth.cpp:2612` is the sole handler for `blemode client` in the known-green **BT=1 / G2=0** build. I verified: `System_BuildConfig.h:232` is a plain literal that the user edits to `0` for that config, `CMakeLists.txt:112-115` greps that literal, and there is no `#undef` anywhere. So `!ENABLE_G2_GLASSES` is FALSE on BT-off but TRUE on G2-off. Treat inverted gates as *suspicious*, not as *automatically dead*. (`WebPage_Bluetooth.h:20` is dead for a different, verified reason — see §2.)

---

## 2. REAL PROBLEMS

### 2.1 `WebServer_Server.cpp:3032` — RUNTIME_WRONG, live today ⚠️ highest priority

```c
ENABLE_G2_GLASSES       ? "true" : "false",   // "g2glasses" field
```

`handleBuildConfig()` (starts :3015) serves the compile-time feature manifest at `GET /api/buildconfig`. The neighbouring `"bluetooth"` field at :3031 correctly uses `ENABLE_BLUETOOTH`. This one uses the raw macro, which is hardcoded `1` and never undefined — so a BT-off board returns:

```json
{ ..., "bluetooth": false, "g2glasses": true, ... }
```

It advertises a BLE-client glasses capability on a device with no BLE radio, where CMake dropped `System_G2_Protocol.cpp` / `System_R1_Protocol.cpp` and every G2 call resolves to a no-op stub. The route is registered unconditionally (:5754) and is on the guest-readable allowlist (`WebServer_Utils.cpp:369`).

**In-tree blast radius is small** — the only consumer is `WebPage_Settings.h:2566-2569`, which caches the response into `window.__buildConfig` and never reads the `g2glasses` key back. The real exposure is external: the companion Android app and any script polling this endpoint.

**Fix:**
```c
(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES) ? "true" : "false",
```
**Risk: near zero.** Value is unchanged on three of four green builds; on BT=0 it flips `true`→`false`, which is the correction. The `char json[512]` buffer at :3022 is untouched (still emits `"true"`/`"false"`). *Note for later:* `ENABLE_R1_HEALTH` has the same never-undef'd literal shape, so if an `r1` field is ever added here it needs the same treatment.

### 2.2 `System_Filesystem.cpp:73` — RUNTIME_WRONG, live today

```c
#if ENABLE_G2_GLASSES
  if (VFS::isSDAvailable()) {
    (void)VFS::mkdirGuarded(String(G2_ICON_ANIMATIONS_VFS_PATH), VFS::systemAuth("filesystem.g2_icon_anim_init"));
  }
#endif
```

I confirmed with the nesting walk that this site has **no enclosing gate at all** (`enclosing = []`). Every symbol resolves without G2: `G2_ICON_ANIMATIONS_VFS_PATH` is an unconditional `#define "/sd/g2_icon_animations"` at `System_BuildConfig.h:242`, and `VFS::isSDAvailable`/`mkdirGuarded`/`systemAuth` come from a never-gated header. So **a BT-off board with an SD card creates an empty G2 icon-pack directory on every boot**, and it can never be populated or read — the only two consumers (`G2_Glasses.cpp:24254`, `G2_Page_TestSuite.cpp:876`) live in compound-gated TUs that compile to nothing.

**Fix:** `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`.
**Risk: very low.** `mkdirGuarded` is idempotent, so boards that already have the directory keep it — the change only stops creating it on new BT-off flashes; it deletes nothing.

**Related, same edit if you want the surface fully gone:** the filesystem permission table still carries an **ungated** row at `System_Filesystem.cpp:1381`:
```c
{"/sd/g2_icon_animations/", 0, PERM_READ|PERM_DELETE|PERM_IMPORT|PERM_CREATE, PERM_ALL, false, false},
```
That is data, not a `#if` site, and it is harmless on its own (it permits a path that will not exist).

### 2.3 `WebPage_Bluetooth.cpp:44` and `:110` — LATENT_COMPILE_BREAK (real, but not reachable today)

I read both blocks. `:44-87` defines four static helpers whose *signatures* take G2 types: `g2DesiredJsonName(G2ControlDesired)`, `g2ObservedJsonName(G2ControlObserved)`, `g2PhaseJsonName(G2ControlPhase)`, `addG2FeatureJson(JsonObject, const G2ControlFeatureStatus&)`. `:110-145` declares `G2ControlStatus status{}` and calls `g2ControlStatusSnapshot(&status)`.

**These five names are NOT stub-covered.** `G2ControlDesired` (`G2_Glasses.h:296`), `G2ControlObserved` (:302), `G2ControlPhase` (:308), `G2ControlFeatureStatus` (:320), `G2ControlStatus` (:329) and `g2ControlStatusSnapshot` (:341) are all declared *inside* the `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` region. The `#else` stub arm supplies `getG2Status()` at :1495 but **none of these**. A BT=0/G2=1 compile of this TU is an immediate hard error on incomplete types plus an undeclared function.

**Be honest about severity: this cannot happen today.** I verified the protection chain end to end — `System_BuildConfig.h:670-674` unconditionally forces `ENABLE_WEB_BLUETOOTH = 0` when `!ENABLE_BLUETOOTH`, and the file's only two includers (`WebPage_Bluetooth.cpp:7`, `WebServer_Server.cpp:81`) are both behind `#if ENABLE_WEB_BLUETOOTH`. So this is **defence-in-depth, not a live bug**. Its cost is that the file's safety depends on a rule ~630 lines away in another file.

**Fix:** AND-in `ENABLE_BLUETOOTH` at **16, 44, and 110 together**.
**Risk: strict no-op on all four green builds — but the lockstep requirement is absolute.** Converting `:16` alone (the include) while leaving 44/110 bare would drop `G2_Glasses.h` while its consumers still compile — **that is the already-fixed `System_Utils.cpp:59` bug re-introduced verbatim.** Converting 44 without 110 leaves `handleBleStatus()` calling helpers that no longer exist. Converting 44+110 without 16 is safe (unused include). See §6.

### 2.4 `WebPage_Bluetooth.h:20` — INVERTED_LOGIC, ~1.4 KB of provably-unselectable source

`#if !ENABLE_BLUETOOTH` guards a friendly "Bluetooth Disabled / recompile with ENABLE_BLUETOOTH=1" HTML page followed by `return;` at :40, which short-circuits the rest of `streamBluetoothInner()`. By the same implication chain as §2.3 (`ENABLE_WEB_BLUETOOTH=1` strictly implies `ENABLE_BLUETOOTH=1`, and those are the only two includers), **this branch is preprocessed away in every reachable config**. It has never shipped.

What a BT-off user actually gets: no nav link (`WebServer_Utils.cpp:516-518` gates the `/bluetooth` link on `ENABLE_WEB_BLUETOOTH`) and no route (`WebServer_Server.cpp:5852-5854` gates `registerBluetoothHandlers` on the same flag) — so a hand-typed `/bluetooth` returns a bare **404**, not this page.

**Two honest options, and this is a product call, not a code call:**
- **(a) Delete lines 20-41.** Zero risk — the branch cannot be selected by any config, so removal cannot change any built binary. It also removes the only thing making the rest of the function conditionally dead. But you keep the bare 404.
- **(b) Keep the friendly page** by moving it into a `#if ENABLE_HTTP_SERVER && !ENABLE_BLUETOOTH` stub handler in `WebServer_Server.cpp` that registers `/bluetooth` plus a nav link, mirroring the existing `handleSensorsStatusStub` pattern at `WebServer_Server.cpp:5848-5850`. This is new code needing its own review; the one hazard is that the stub must **not** include `WebPage_Bluetooth.h`.

Do **not** AND-in a G2 term here — it is not a G2 problem.

---

## 3. DEAD CODE

Grouped, with an honest reclaim estimate. **None of this is worth a build cycle on its own** — fold it into the §5 sweep or skip it.

**(a) Unreachable C++ branch — `Bluetooth.cpp:2574-2576`.** I read `bleSubsystemStateString()` at :2564-2578 and the case analysis holds. Both predicates are pure flag reads with no yield between them (`isG2ClientInitialized` = `gG2State && gG2State->initialized`, `G2_Glasses.cpp:13067`; `isBleServerInitialized` = `gBLEState && gBLEState->initialized`, `Bluetooth.cpp:1944`). `(client=1,server=0)` returns at :2566-2568; `(server=1, any client)` returns at :2573; `(0,0)` falls to :2577. **No input reaches :2575.** It is the residue of the "both roles published" diagnostic path that the :2573 server line already absorbs — exactly as the comment at :2568-2570 says it should. This is *not* a gate problem; the gate is fine. **Delete the three lines.** Do not "fix" it by AND-ing in Bluetooth; that leaves the dead line in place. Reordering it above :2573 instead would flip a deliberate documented policy — prefer deletion unless you want that policy changed.

**(b) Redundant gates inside an always-true region — `G2_Glasses.cpp:5397` and `:5444`.** The file is wrapped end-to-end in `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` (:13 → :30314), so both inner gates are unconditionally true. `:5397-5414` wraps **comment text only** — zero statements. `:5444-5449` wraps two `snprintf` lines with an `#else` writing an empty string; that `#else` **cannot be selected in any config**, which is precisely the untestable-fallback pattern this audit exists to find. **Recommended: delete the `#if`/`#else`/`#endif` scaffolding, keep the code.** Do not AND-in Bluetooth — that enshrines a conditional that can never be false and tells the next reader the surrounding function is reachable with G2 off. Flash reclaimed: **zero** (comments and an unreachable `#else` emit nothing). This is readability only.

**(c) `WebPage_Bluetooth.h:20-41`** — see §2.4. ~1.4 KB of source, 0 bytes of flash.

**Net reclaimable flash across all dead code: essentially nothing.** Do this for clarity, not for space.

---

## 4. CORRECT_AS_IS — the 27, and why

I confirmed the enclosing-gate claims myself with a preprocessor depth walk rather than trusting the auditors' line spans. Every one checked out:

| Site(s) | Effective enclosing gate (verified) |
|---|---|
| `OLED_Mode_Bluetooth.cpp` 27, 79, 201, 251, 280, 470 | `#if ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH` |
| `OLED_SettingsEditor.cpp:1070` | `ENABLE_OLED_DISPLAY` → `ENABLE_BLUETOOTH` |
| `System_Settings.cpp` 2311, 2399 | `#if ENABLE_BLUETOOTH` |
| `System_Utils.cpp:1791` | `#if ENABLE_BLUETOOTH` |
| `HardwareOne.cpp` 2011, 2037 | `#if ENABLE_BLUETOOTH` |
| `Bluetooth.cpp` 1214, 2558, 2565, 2598, 2611 | file-wide `#if ENABLE_BLUETOOTH` (:21 → :3013) |
| `WebServer_Server.cpp:69` | lexically nested inside `#if ENABLE_BLUETOOTH` at :67 |
| `WebPage_Bluetooth.h` 105, 226, 424 | header unreachable unless `ENABLE_WEB_BLUETOOTH`, which implies BT |
| `G2_Glasses.cpp` 5397, 5444 | file-wide compound gate (:13 → :30314) |

Three worth calling out specifically, because they are the ones that *would* have been genuine `RUNTIME_WRONG` if bare:

- **`OLED_Mode_Bluetooth.cpp:79`** adds the `G2 Glasses >>` / `R1 Ring >>` menu rows. A truly bare gate here means a G2 submenu on a board with no BLE. It is not bare — the file gate at :19 covers it. Its dispatch site (:251) and its mode-registry rows (:470) share one gate expression, so they cannot drift into "dispatched but never registered."
- **`System_Settings.cpp:2399`** registers `g2DeviceSettingsModule`. Bare here would be both a link error (the `extern` at :2311 is BT-nested) *and* a consumed registry slot against your cap of 20. Correctly nested; extern and use are in exact lockstep.
- **`WebServer_Server.cpp:69`** is a false positive of any flat `grep -n '#if ENABLE_G2_GLASSES'` — the nesting under :67 is only visible with two lines of context. If you keep a sweep script, teach it to treat gates nested under `#if ENABLE_BLUETOOTH` as compound, or this site re-surfaces on every audit.

**One auditor self-contradiction to resolve: `OLED_SettingsEditor.cpp:21`.** It was returned as verdict `CORRECT_AS_IS` but action `AND_IN_BLUETOOTH`. My read: the auditor is right on the facts and the verdict is the correct one. This is the only bare `#if` outside a BT region in a non-web `.cpp` besides `System_Filesystem.cpp:73` — but it guards *an include only*, and nothing in the file uses it (all four G2 references at :1071/:1073/:1076/:1078 sit inside the `#if ENABLE_BLUETOOTH` at :1059). The include gate is **broader** than its sole use, and a broader include can never under-include. This is the *opposite* shape from the `System_Utils.cpp:59` bug you already fixed, which was a *narrower* include gate. Treat it as cosmetic (§5, item 5).

---

## 5. RECOMMENDED SWEEP

### Option A — the systemic fix (my recommendation)

Add a derived rule to `System_BuildConfig.h`, next to the existing derived block around :715-730:

```c
// G2 needs the BLE radio. ENABLE_G2_GLASSES is a plain literal above so CMake
// can grep it; this is the compile truth for TUs.
#if !ENABLE_BLUETOOTH
  #undef ENABLE_G2_GLASSES
  #define ENABLE_G2_GLASSES 0
#endif
```

This is **not a novel pattern** — it is exactly what `ENABLE_R1_HEALTH` already does at `System_BuildConfig.h:725-728`:
```c
#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)
  #undef ENABLE_R1_HEALTH
  #define ENABLE_R1_HEALTH 0
#endif
```

One edit fixes **§2.1, §2.2, and §2.3** and neutralizes the whole latent class permanently.

**What I verified about the risk:**
- **CMake is unaffected.** `CMakeLists.txt:112-115` uses `string(REGEX MATCH ...)`, which returns the *first* match — line 232's `#define ENABLE_G2_GLASSES 1`. `HW_CFG_BUILD_G2` at :162-165 already ANDs `HW_CFG_ENABLE_BLUETOOTH` itself. `ENABLE_R1_HEALTH` proves this arrangement works today.
- **`ENABLE_MICROPHONE` is unaffected.** `System_BuildConfig.h:259` defines it as an *unexpanded expression* `(ENABLE_MICROPHONE_SENSOR || (ENABLE_BLUETOOTH && ENABLE_G2_GLASSES))`, evaluated at each use site, not at the definition line — so there is no ordering trap, and the compound is already correct either way.
- **No `#else`/negated arm changes truth value anywhere.** I enumerated every one repo-wide: `Bluetooth.cpp:2611` (inside `#if ENABLE_BLUETOOTH`), `G2_HijackFsm.h:108` and `G2_Page_TextEntry.h:80` (both `#else` of *compound* gates, already firing on BT-off), `System_Utils.cpp:1794`, `HardwareOne.cpp:2013`, `G2_Glasses.cpp:5447` (all inside BT-gated or file-gated regions). **Zero behaviour change on any fallback arm.**
- **No other component uses the macro.** Repo-wide grep outside `components/hardwareone/` hits only `docs/`, `CHANGELOG.md`, and a comment in `sdkconfig.defaults.esp32s3`.

**The one mandatory companion edit:** the comment at `System_BuildConfig.h:252-255` explicitly states the macro "is never `#undef`'d." Adding this rule makes that comment **false**. It must be rewritten in the same commit, or the next auditor is misled by a load-bearing lie. This is the single biggest cost of Option A.

**Residual uncertainty:** Option A changes the preprocessor truth table for the BT-off config across 29 sites at once. My analysis says nothing moves except the two real bugs, but **this needs a BT=0 build to settle** — it is not a change I would land on analysis alone. It should also be re-run against BT=1/G2=0 to confirm no interaction with the R1 derived rule.

### Option B — per-site edits (if you want minimum blast radius)

Ordered by value, with risk:

| # | Edit | Risk |
|---|---|---|
| 1 | `WebServer_Server.cpp:3032` → `(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES) ? ...` | **Near zero.** Nothing in-tree branches on the field. External clients keying off `g2glasses==true` now correctly see `false` on BT-off — intended, not a regression. |
| 2 | `System_Filesystem.cpp:73` → compound gate | **Very low.** Costs one boot-time mkdir on BT-off; no consumer exists there. Optionally gate the `:1381` permission row in the same edit. |
| 3 | `WebPage_Bluetooth.cpp` **16 + 44 + 110 together** → compound gate | **No-op on all four builds, but lockstep-critical.** See §6. |
| 4 | `WebPage_Bluetooth.h:20-41` → delete (or §2.4 option b) | **Zero for delete.** Branch is unselectable, so removal cannot change any binary. Option (b) is new code needing review. |
| 5 | `OLED_SettingsEditor.cpp:21` → compound gate | **Very low, and optional.** Purely to make a bare-gate grep return zero. Only residual risk: some other header silently relying on this TU pulling `G2_Glasses.h` first — unlikely given include guards; a BT-off build settles it in one pass. |
| 6 | Dead code: `Bluetooth.cpp:2574-2576` delete; `G2_Glasses.cpp:5397`/`:5444` gate-scaffolding delete | **Zero.** Unreachable / always-true regions. |

### ⚠️ Sites where AND-ing in `ENABLE_BLUETOOTH` would CHANGE BEHAVIOUR rather than tighten a gate

I looked for these specifically. **The good news: there is no site in the 31 where AND-ing in BT changes behaviour in a currently-buildable config** — at every one of the 27 nested sites `ENABLE_BLUETOOTH` is provably 1 throughout the region, so the term is a tautology. The hazards are all of a different kind:

- **`WebPage_Bluetooth.cpp:16` converted alone** — not a behaviour change but a **hard compile break** at BT=0/G2=1: drops the include while :44/:110 still compile. This is the `System_Utils.cpp:59` bug re-introduced.
- **`WebPage_Bluetooth.h:226` and `:424` converted independently of each other** — a real *runtime* behaviour change. `:424` injects `window.__bluetoothG2Enabled`, which the page JS reads at `:489` and `:1081` to decide whether to reveal `#bt-g2-panels` (the markup from `:226`). If they disagree, the page either advertises G2 with no panels present or ships panels the JS refuses to show. **Treat 226 and 424 as one unit.**
- **`HardwareOne.cpp:2037`** — no behaviour change from the gate term, but the block is structurally fragile: the `} else` at :2055 sits *inside* the guarded region and binds to the `if (ramFlushResolve(...))` at :2057 *outside* it. Any edit must preserve that dangling-else shape or the server-mode boot path silently becomes unconditional. Since the change buys nothing here, **don't touch it.**

---

## 6. WHAT NOT TO TOUCH

1. **`Bluetooth.cpp:2612`** — the `#if !ENABLE_G2_GLASSES` arm returning `"Error: [BLE] G2 client not compiled (ENABLE_G2_GLASSES=0)"`. **This is live shipping code in the BT=1/G2=0 build.** Deleting it as "inverted logic" would make `blemode client` fall through to the generic `"Error: invalid arguments — Usage: blemode [server|client]"` at :2623, replacing an accurate diagnostic with a misleading one. The task premise does not apply here.

2. **`WebPage_Bluetooth.cpp` 16 / 44 / 110 individually.** Convert all three or none. Converting the include alone is the exact bug you fixed this session.

3. **`WebPage_Bluetooth.h` 226 / 424 individually.** Markup and its JS feature flag must move together.

4. **`HardwareOne.cpp:2011` `#else`** (`wantClientForAutoReconnect = false`). Load-bearing in BT=1/G2=0: `bleAnyPeerWantsAutoReconnect()` lives in `BLE_Peers`, which is BT-gated but G2-agnostic, and the `false` substitution is what stops server-only boards coercing themselves into client mode. Do not delete it as a dead fallback.

5. **`HardwareOne.cpp:2037`'s dangling-`else` structure** — see §5.

6. **`System_Settings.cpp` 2311 / 2399 as a pair.** They match exactly today. A mismatch between the `extern` nesting and the registration nesting is a genuine link error. Change both or neither — and neither is correct.

7. **`WebServer_Server.cpp:69`** — already compound via nesting. Flattening it would drop the nesting that makes the `Bluetooth.h` → `G2_Glasses.h` dependency ordering obvious. No functional gain.

8. **The 27 nested `CORRECT_AS_IS` sites generally.** AND-ing BT into them is churn that *reduces* clarity: it implies each site is independently BT-gated, which would be misleading if the enclosing file gate were ever narrowed. If your goal is a clean grep, **Option A gets you there without touching a single one of them.**

---

## Uncertainty, stated plainly

- **What I verified myself:** the complete bare-gate census (29, exact match to the audited set); the preprocessor nesting of every `CORRECT_AS_IS` claim; the full contents of `WebPage_Bluetooth.cpp` 16/44/110, `WebPage_Bluetooth.h` 20/105/424, `Bluetooth.cpp` 2550-2624, `System_Filesystem.cpp` 73 + 1381, `WebServer_Server.cpp` 3032; the `ENABLE_WEB_BLUETOOTH` derivation and its only two includers; the `ENABLE_R1_HEALTH` precedent; CMake's grep behaviour; the absence of any `#undef ENABLE_G2_GLASSES`.
- **What I did not verify:** the exact contents of the large `OLED_Mode_Bluetooth.cpp:280-456` block and the `G2_Glasses.cpp` comment-only claims. Their *verdicts* don't depend on those contents — the enclosing gates settle them — so I accepted the auditors' descriptions.
- **What needs a build, not analysis:** Option A. My claim that no fallback arm changes truth value is derived from a repo-wide enumeration and I believe it, but a 29-site preprocessor change on the BT-off config deserves an actual BT=0 build plus a BT=1/G2=0 build before it lands.
- **The auditors were internally consistent except at `OLED_SettingsEditor.cpp:21`** (verdict vs. action contradiction), resolved in §4.
---

# Appendix A — Root cause: why this happened

*Archaeology run 2026-08-16. The question asked was "was this a lapse in ensuring this was taken
care of?" The answer is no — not a single lapse. It is a systematic pattern with a structural cause.*

## A.1 Base rate

| Form | Count |
|---|---|
| `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` (correct) | 56 |
| `#if ENABLE_G2_GLASSES` (bare) | 29 |

**34% of G2 gates use the bare form.** That is a habit, not an oversight.

## A.2 Timeline — the rule was violated while it was being written

The warning comment at `System_BuildConfig.h:252-255` ("we MUST AND-in ENABLE_BLUETOOTH") was added
**2026-07-24** in `889f1b83` (v0.99.1). `git blame` on the bare sites:

| Site | Commit | Date |
|---|---|---|
| Bluetooth.cpp:1214, :2611, WebPage_Bluetooth.h:105, HardwareOne.cpp:2011 | `861cd948b` | 2026-04-28 |
| Bluetooth.cpp:2558, WebServer_Server.cpp:69 | `6ea00b659` | 2026-04-29 |
| G2_Glasses.cpp:5397 | `2a11c6422` | 2026-05-03 |
| System_Filesystem.cpp:73 | `0b3c218bc` | 2026-05-09 |
| OLED_Mode_Bluetooth.cpp:27 | `1126bcc27` | 2026-06-01 |
| System_Utils.cpp:1789 | `88e4ed454` | 2026-06-11 |
| **WebPage_Bluetooth.cpp:16** | **`889f1b835`** | **2026-07-24 — the same commit that wrote the warning** |
| **System_Settings.cpp:2311** | **`2879f02b3`** | **2026-08-05 — after the warning (v0.99.8)** |
| **OLED_SettingsEditor.cpp:21** | **uncommitted** | **2026-08-16 — in the working tree today** |

So the pattern predates the rule, continued in the very commit that authored the rule, recurred
eleven days later, and is present in work in progress. "Write it down once" demonstrably did not hold.

## A.3 The four structural causes

1. **The flag opts out of the normalization the rest of the codebase relies on.** The derived
   section of `System_BuildConfig.h` `#undef`s and redefines flags so a bare `#if` is genuinely
   safe — `ENABLE_OLED_DISPLAY` (:477), `ENABLE_HTTP_SERVER` (:531), and the whole `ENABLE_WEB_*`
   family (:645-654) including `ENABLE_WEB_R1_HEALTH`. `ENABLE_G2_GLASSES` is the exception, and
   the comment says why: *"Keep this a plain literal expression — CMakeLists greps
   `#define NAME <int>` and cannot evaluate `(A || B)`."* Developer habits are calibrated on the
   majority behaviour; this flag silently violates it.

2. **The name does not mean what it says.** `ENABLE_G2_GLASSES` reads as "is G2 enabled" — so
   `#if ENABLE_G2_GLASSES` looks self-evidently correct. It actually means "G2 was *requested*";
   availability requires the second term. A macro that is never 0 and whose real meaning needs
   another operand is a trap by construction.

3. **Nothing enforces it.** Contrast the debug-flag system in the same repo: `DBG_FLAG_COUNT == 128`,
   `dbgBitsUnique()`, `dbgBanksDisjoint()`, `dbgParentsWellFormed()`, `dbgRegRowsPickedOnce()` —
   there, a mistake is a *build error*. The build-gate idiom has no static assert, no lint, no CI
   check. Its only guard is one prose comment attached to an unrelated `#define`.

4. **The failure was invisible.** `ENABLE_BLUETOOTH` has been `1` in every shipped configuration;
   the BT-off path had never been built until 2026-08-16. A rule with no failing test decays.

## A.4 What would actually prevent recurrence

Ranked by leverage, not by effort:

1. **Normalize the flag (Option A in §5).** Removes the trap entirely — a bare gate becomes
   *correct*, so the habit stops mattering. Verified feasible: CMake takes the first regex match, and
   `ENABLE_R1_HEALTH` already uses this exact pattern. Requires rewriting the now-false
   "never `#undef`'d" comment in the same commit.
2. **A grep-based check** (CI or pre-commit) for `#if` on `ENABLE_G2_GLASSES` without
   `ENABLE_BLUETOOTH`. Cheap, but noisy: it must understand enclosing nesting or it re-reports the
   27 already-safe sites forever (see `WebServer_Server.cpp:69`, a false positive of any flat grep).
   Option A makes this check unnecessary.
3. **Build the off configurations.** BT=0 and G2=0 were never built until today, and building them
   immediately surfaced two real defects. Whatever else changes, these configs need to be built
   periodically or they will rot again.

---

# Appendix B — Adversarial check (run before any edits, 2026-08-16)

Five independent refuters attacked the load-bearing claims above. **Three were refuted.**
Nothing in this document should be actioned without reading this appendix.

## B.1 REFUTED (high) — the census was wrong

Claim: "27 of the 29 bare sites sit inside a BT-gated region, so only 2 matter."

**Counterexample: `OLED_SettingsEditor.cpp:21`.** Its enclosing stack is only
`#if ENABLE_OLED_DISPLAY` (:4). The `#if ENABLE_BLUETOOTH` at :18 wraps **only** the
`#include "Bluetooth.h"` and **closes at :20** — line 21 is its *sibling*, not its child. On the
green BLUETOOTH=0 build this bare gate is TRUE and pulls in the ~1500-line `G2_Glasses.h` while
`Bluetooth.h` is excluded. This is precisely the shape that fools a visual audit. (Benign in effect —
the file's only G2 use at :1076 is under a real BT gate at :1059 — but the census claim is false.)

Also downgraded: `WebPage_Bluetooth.h:105/226/424` have **no** enclosing preprocessor BT gate at all;
their safety comes from TU-inclusion (`ENABLE_WEB_BLUETOOTH`), a second-order dependency. And
`WebPage_Bluetooth.cpp:16/44/110` are enclosed by the *derived* `ENABLE_WEB_BLUETOOTH`, not by
`ENABLE_BLUETOOTH` directly.

Corrected tally: **~21 genuinely nested · 4 safe only by TU-inclusion · 1 (OLED_SettingsEditor.cpp:21)
not equivalent at all · 2 real bugs.** The audit also conflated a directive count with a use count:
`WebServer_Server.cpp:3032` is a C ternary, not a `#if`, so "29 bare #if sites, one of which is 3032"
cannot both be true.

## B.2 REFUTED (medium) — `Bluetooth.cpp:2574-2576` is NOT safe to delete

Claim: the last branch of `bleSubsystemStateString()` is unreachable, delete it.

The sequential proof holds **only under sequential consistency**, which does not apply here:

- `gG2State` (`G2_Glasses.cpp:647`) and `gBLEState` (`Bluetooth.cpp:81`) are plain, **non-volatile,
  non-atomic** globals.
- Neither accessor takes a lock, though a lifecycle mutex for exactly this data exists and is used by
  the *writers* (`BleLifecycleGuard`, `Bluetooth.cpp:150-167`).
- Writers run on a different task from readers.

**Concrete reaching interleaving, requiring no invariant violation** — the normal automatic
stack-recycle path: main loop → `bleStackRecycleIfWedged()` (`G2_Glasses.cpp:13106`) is admitted only
when the server is down, then does `deinitG2Client()` → `initG2Client()`, so server stays 0 while
client goes 1 → 0 → 1. A concurrent reader on the httpd task (`buildSystemInfoJson()` →
`bleSubsystemStateString()`, `System_Utils.cpp:1788`, reachable from SSE and `/api`) can observe
client=0 at :2566 and client=1 at :2574. **Line 2575 executes.**

**Action: DO NOT DELETE.** (The deeper issue — unsynchronised reads of lifecycle state — is a
separate, larger finding.)

## B.3 REFUTED (high, but in the desired direction) — Option A *does* change behaviour

Claim: the derived-rule normalization changes no truth value anywhere.

Sub-claims **(a) CMake unaffected** and **(b) `ENABLE_MICROPHONE` unaffected** both **SURVIVED**, and
were verified *empirically* (header copied to scratch, BT set to 0, preprocessed with and without the
proposed block: only `ENABLE_G2_GLASSES` flips; `ENABLE_MICROPHONE`/`ENABLE_R1_HEALTH`/
`ENABLE_WEB_BLUETOOTH` are 0 either way; a simulation of CMake's first-match regex still returns `1`).
(b) holds structurally: the rule only fires when `ENABLE_BLUETOOTH==0`, and the G2 term in
`ENABLE_MICROPHONE` is `(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)`, already 0 at that moment.

Sub-claim **(c) survived as literally worded** (23 `#else` arms enumerated; none newly activates)
**but is refuted in substance**: Option A changes exactly two observable behaviours on a BT-off board —
`/api/buildconfig` reports `"g2glasses":false` instead of `true`, and `System_Filesystem.cpp:73` stops
creating `/sd/g2_icon_animations`. **Those are the two bugs.** The refutation confirms the fix works
and hits nothing else.

## B.4 NOT REFUTED (high) — `Bluetooth.cpp:2612` is live; do not delete

Six routes attempted, all failed. `Bluetooth.cpp` is compiled unconditionally
(`CMakeLists.txt:169`); `#if ENABLE_BLUETOOTH` (:21→:3013) is TRUE at BT=1/G2=0; `ENABLE_G2_GLASSES`
is the literal `0`; the `blemode` registry row (:2651) carries no G2 guard. **This arm is the sole
handler for `blemode client` in that config.** Confirms the earlier "inverted gates are always dead"
claim was wrong.

## B.5 NOT REFUTED (high) — `WebPage_Bluetooth.h:20-41` is genuinely unselectable

Six independent refutation attempts failed: exactly two includers, both behind `ENABLE_WEB_BLUETOOTH`;
that flag is force-zeroed at `System_BuildConfig.h:674-677` (unconditional, depth 2, last write in the
file); no `#undef ENABLE_BLUETOOTH` exists anywhere. Deletion is zero-risk — but zero-value too
(0 bytes of flash).

## B.6 Net recommendation after adversarial review

| Action | Verdict |
|---|---|
| **Option A — derived-rule normalization** | **DO** — fixes both live bugs + neutralizes the class; (a)/(b) empirically verified |
| Rewrite the now-false "never `#undef`'d" comment at :252-255 | **MANDATORY, same commit** |
| Delete `Bluetooth.cpp:2574-2576` | **DO NOT** — reachable via a real race (B.2) |
| Delete `Bluetooth.cpp:2612` | **DO NOT** — live shipping code (B.4) |
| Per-site sweep of the 29 gates | **SKIP** — Option A subsumes it without touching one call site |
| Delete `WebPage_Bluetooth.h:20-41` | Optional, zero-risk, zero-value |

**Must be settled by building, not analysis:** BT=0 and BT=1/G2=0 after Option A lands.

---

# Appendix C — Applied (2026-08-16). UNCOMMITTED.

**Option A landed**, plus the mandatory comment rewrite. No call site was touched.

`System_BuildConfig.h`, derived section (immediately above the `ENABLE_R1_HEALTH` rule):
```c
#if !ENABLE_BLUETOOTH
  #undef ENABLE_G2_GLASSES
  #define ENABLE_G2_GLASSES 0
#endif
```
The `:252-255` comment claiming the macro "is never #undef'd" was rewritten (it is now false); the
`ENABLE_MICROPHONE` definition deliberately keeps its explicit `ENABLE_BLUETOOTH &&` term, because it
is an unexpanded expression evaluated at use sites and the literal is what CMake greps — so it stays
true independently of rule ordering.

**Verified by preprocessing, BT=0:**
```
ENABLE_G2_GLASSES = 0      (was 1 — the whole bare-gate class is now correct)
ENABLE_MICROPHONE = (0 || (0 && 0))   unchanged
ENABLE_R1_HEALTH  = 0                 unchanged
#if ENABLE_G2_GLASSES  -> no longer fires
```

**Both live bugs fixed as a consequence, with zero call-site edits:**
- `WebServer_Server.cpp:3032` — `/api/buildconfig` now reports `"g2glasses":false` on a BT-off board.
- `System_Filesystem.cpp:73` — no longer mkdirs `/sd/g2_icon_animations` on a BT-off board.

**Build matrix, all four GREEN** (`xiao_s3`, config restored byte-identical after each):
shipping · R1_HEALTH=0 · G2=0 · BT=0.

**Deliberately NOT done** (per Appendix B): no deletion of `Bluetooth.cpp:2574-2576` (reachable via a
real race) or `Bluetooth.cpp:2612` (live shipping code at G2=0); no per-site sweep of the 29 gates —
Option A subsumes it.

## C.1 Scope check — are other flags affected?

Asked: should the Phone App and R1 Ring get the same treatment? Checked every `ENABLE_*` literal:

- **R1 Ring — already had it.** `ENABLE_R1_HEALTH` has carried this exact derived rule all along
  (`#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)` → `#undef` → 0). Nothing to add. (Note: §2.1's
  remark that `ENABLE_R1_HEALTH` "has the same never-undef'd literal shape" was **wrong** — it is
  undef'd.)
- **Phone App — no such build flag.** The phone/BLE-server role is a *runtime* mode
  (`bleMode == BLE_MODE_SERVER`, set by `blemode server|phone`), compiled under `ENABLE_BLUETOOTH`
  itself. There is no `ENABLE_PHONE_*` to normalize.
- **G2 was the last one.** The trap needs: value `1` + a real dependency + no normalization. Every
  other dependent flag is already normalized (`ENABLE_BONDED_MODE`, `ENABLE_HTTPS`, `ENABLE_MQTT`,
  `ENABLE_R1_HEALTH`, `ENABLE_WEB_GAME_*`). The remaining value-1 unnormalized flags have no
  unenforced dependency: `ENABLE_BLUETOOTH` is a root flag, and
  `ENABLE_RASPBERRY_PI_HOST_POWER`/`_FAN` are documented as deliberately independent of the UART link
  ("does not enable the UART link at runtime by itself"). Flags sitting at `0` cannot exhibit the trap.
