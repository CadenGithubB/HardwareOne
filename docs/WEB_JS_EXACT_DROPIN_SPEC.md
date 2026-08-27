> **CORRECTIONS — READ FIRST (2026-08-24).** This spec was re-verified by a
> three-layer pass (mechanical checker → evidence-mandatory report → adversarial
> reproduction; 78 rulings, 0 overturned, 22 modified). The authoritative correction
> layer is **docs/WEB_JS_CLAIMS_VERIFICATION.md**. Known-wrong items in THIS file:
> Games.h has SEVEN `var hw` shadows (not 5) and belongs in the dry-run exclusion list;
> B6 must read FOUR 3-arg addEventListener sites (adds WebPage_CLI.h:187, window,
> {capture:true}) and R4 "minus 3"; the LLM exclusion is 21 prefix sites (not 50) and
> its test guards fail LOUD (the page can be rewritten in a dedicated wave with a small
> tooling patch); the gate-blindness list omits WebServer_Utils.h's 5 dialog sites
> (:1520-1524); §naming (hw.$-as-alias, :285-330) is superseded — ruling: pure rename,
> user chose hw.$ (2026-08-24); wave 1 APPLIED; scope of record: 27 files / 1,015 sites / 17,315 B.

# window.hw Exact Drop-In Adoption Spec

Status: specification only. No code has been changed.
Scope: browser JS embedded in C++ string literals under `components/hardwareone/`.
Corpus: 1,616 classified sites across 33 files.
Basis: one mechanical classification pass, attacked by 10 independent probes, merged and then spot-verified against source for this document.

---

## 1. What this is

The firmware streams a shared client runtime, `window.hw`, into every page from
`WebServer_Utils.cpp:773`. It is almost entirely unadopted: 1,005 raw
`document.getElementById` calls against 40 uses of `hw._ge`.

This document specifies **only the swaps that are exact behavioural matches**.
That constraint is deliberate and comes from the user: the motive is
*compatibility*, not cleanup. A near-match — anything that changes what happens
on an error path, a missing element, or a CSS fallback — is a separate decision
and is explicitly deferred, not silently folded in.

The distinction matters more here than in a typical codebase, because of one
property that governs everything below:

> **Every one of the 1,616 sites lives inside a C++ string literal.**
> `hw._eg(` instead of `hw._ge(` is a perfectly valid C++ string on every board.
> The build stays green. The compiler can catch exactly two things: a broken
> quote and a broken raw-string delimiter — and only in files that compile at all.

So the safety of this pass rests entirely on the rules in §6, not on the toolchain.

### The one thing the classifier could not see

The mechanical pass answered "is this swap semantically identical?" It could not
answer "does this code run?", "is this file compiled?", or "does a test read this
source text?". Those three questions produced most of the corrections below.

---

## 2. The tiers

| Tier | Count | Meaning | Risk |
|---|---:|---|---|
| **STRICT** | 1,058 | Identical in every case, including error paths. | None, if §6 rules are encoded. |
| **SUPERSET** | 276 | Identical whenever the element exists; `hw.*` silently no-ops where raw code throws. | Low semantically, **high procedurally** — see §4. |
| **NEAR** | 180 | Needs an API change or a per-site judgement call. | Deferred. Not in this pass. |
| **EXCLUDE** | 102 | No `window.hw`, unreachable, or the site *is* the runtime. | Do not touch. See §7. |
| | **1,616** | | |

These are the reconciled totals after 15 deduplicated corrections to the original
`1152 / 258 / 183 / 23`. Every correction moved in the cautious direction.
Arithmetic:

```
STRICT   1152 - 94  = 1058     (48 scoped-qs, 2 Settings, 2 ESPNow, 2 Maps.h,
                                1 Bluetooth alias, 10 Maps.cpp, 21 LLM,
                                7 sths34pf80, 1 LoginRequired)
SUPERSET  258 + 50 - 32 =  276
NEAR      183 + 10 - 13 =  180
EXCLUDE    23 + 79      =  102
                          -----
                           1616  ✓
```

### Honest note on per-file reconciliation

Summing the probes' per-file *confirmed drop-in* figures gives **1,048**, not
1,058 — a gap of 10. The gap is a reporting artifact: three probes reported
"confirmed after promotions and downgrades" while the merged arithmetic worked
from the original class totals. **1,048 is the conservative floor**; treat the
per-file table in §3 as the number of sites a probe actually put eyes on and
signed off, and 1,058 as the class total. Do not chase the difference by
promoting sites to close it.

### What this actually saves

Measured against the real replacement strings:

```
document.getElementById(   -> hw._ge(           17 B x ~999  = 16,983 B
document.querySelector(    -> hw.qs(            17 B x  ~60  =  1,020 B
document.querySelectorAll( -> hw.qsa(           19 B x  ~73  =  1,387 B
document.addEventListener( -> hw.on(document,   11 B x   19  =    209 B
display collapses (SUPERSET tier, ~188 sites)   ~14 B each   =  2,632 B
                                                              --------
                                                              ~21.7 KB
```

**Call it ~20 KB of flash.** Be clear-eyed about that number: it is small. The
same corpus would give up roughly **~195 KB** to a build-time pass that simply
stripped comments and indentation from the embedded JS — an order of magnitude
more, for no semantic risk at all, because it never touches an identifier.

**The real value of this pass is consistency, not bytes.** One idiom instead of
two, a single place to fix a DOM-access bug, and a runtime that is actually used
rather than dead weight streamed to every page. If flash headroom is the goal,
do the whitespace strip instead — or first. If you want both, they compose: the
strip does not care which identifier it is stripping around.

---

## 3. Tier 1 — STRICT (1,058 sites)

**This tier changes no observable behaviour.** Every rule below preserves the
throw, the return value, the `null`, and the ordering of the original. If a rule
would not, it is not in this tier.

### 3.1 The mechanical rules

Verified against the actual runtime at `WebServer_Utils.cpp:773`.

**R1 — getElementById, string-literal argument** (854 sites)
```js
// before
document.getElementById('mesh-warning')
// after
hw._ge('mesh-warning')
```
`hw._ge = function(x){ return typeof x==='string' ? document.getElementById(x) : x }`.
For a string argument this is `document.getElementById` verbatim, including the
`null` return. Exact.

**R2 — getElementById, expression argument** (145 sites: 87 concatenations, 58 bare identifiers)
```js
// before
document.getElementById('msg-' + mac)
document.getElementById(btnId)
// after
hw._ge('msg-' + mac)
hw._ge(btnId)
```
Exact **only where the argument is provably a string.** `hw._ge` passes a
non-string straight through instead of coercing it. Probes traced every one of
these to its call sites and found exactly one site where the pass-through is
load-bearing (`WebPage_ESPNow.h:2586`, which feeds an Element in deliberately —
see §6 C9). Any *new* expression-argument site must be traced before it qualifies.

**R3 — querySelector / querySelectorAll, no context argument** (the safe subset of 133)
```js
// before
document.querySelector('.mesh-row-actions')
document.querySelectorAll('input[type="checkbox"]')
// after
hw.qs('.mesh-row-actions')
hw.qsa('input[type="checkbox"]')
```
`hw.qs = function(s,c){ return (c||document).querySelector(s) }`. With `c`
omitted the receiver is `document`. Exact.

> **Context-passing calls are NOT in this tier.** See blocker B1 — they were the
> single largest correction to the classification.

**R4 — addEventListener on `document` / `window`, exactly two arguments** (19 sites, minus 2)
```js
// before
document.addEventListener('DOMContentLoaded', init)
// after
hw.on(document, 'DOMContentLoaded', init)
```
`hw.on = function(e,v,f){ if(e) e.addEventListener(v,f) }`. `document` and
`window` are never null, so the guard is inert. Exact — **but only at arity 2.**
See blocker B6: two sites in this bucket pass a third argument and must leave it.

**R5 — local alias helper body** (1 site)
```js
// WebPage_Bluetooth.h:477 — swap the BODY only
function el(id){ return document.getElementById(id); }   // before
function el(id){ return hw._ge(id); }                     // after
```
Do **not** rewrite the declaration to `var el = hw._ge;` — that changes hoisting.
See blocker B3 / §6 C9.

### 3.2 Per-file confirmed drop-ins

Sites a probe individually reviewed and signed off as exact:

| File(s) | Total sites | STRICT confirmed |
|---|---:|---:|
| `WebPage_ESPNow.h` | 368 | 257 |
| `WebPage_Automations.h` | 274 | 167 |
| `WebPage_Settings.h` | 142 | 105 |
| `WebPage_Maps.h` + `WebPage_Maps.cpp` | 141 | 101 (`.h` only) |
| `WebPage_Logging.h` | 116 | 97 |
| `System_Camera_DVP_Web.h` (79), `System_EdgeImpulse_Web.h` (39), `System_Microphone_Web.h` (26) | 144 | 94 |
| `WebPage_LLM.h` (50), `WebPage_Files.h` (47), `WebPage_Speech.h` (45), `WebPage_CLI.h` (12), `WebPage_AviPlayer.h` (18) | 172 | 100 (LLM's 21 excluded) |
| `WebPage_Dashboard.h`, `WebPage_Sensors.h` | 83 | 52 |
| `WebServer_Utils.h`, `WebServer_Utils.cpp`, `WebServer_Server.cpp` | 67 | 24 |
| `WebPage_MQTT.cpp`, `WebPage_Bond.cpp`, `WebPage_Battery.cpp`, `WebPage_Bluetooth.h`, `WebPage_Login.h`, `WebPage_LoginRequired.h`, 9 × `i2csensor_*_web.h` | 109 | 51 |
| **Total** | **1,616** | **1,048** |

### 3.3 What "no observable behaviour change" does not cover

Two caveats that are true of the tier as a whole:

- **Performance.** `WebPage_Maps.h:1024` (`isOn` inside `layerEnabled`) is called
  once per map *feature* from the draw loops at `:1132, :1150, :1190, :1233,
  :1249, :1370`, and repaints fire on every mousemove drag and wheel tick.
  `hw._ge` adds a call frame plus a `typeof` test to each. Skip it on cost grounds.
- **Shared failure point.** The helper IIFE at `WebServer_Utils.cpp:773` has no
  `try/catch` around its assignments — only the trailing `console.log` is
  wrapped. Today a page full of raw `document.getElementById` survives a failure
  in that block. After adoption it would not. Given the stated motive is
  compatibility, this is worth stating out loud.

---

## 4. Tier 2 — SUPERSET (276 sites)

### 4.1 Lead with the null-guard question

The tier definition is: *identical whenever the element exists; `hw.*` silently
no-ops where the raw code throws.*

Probes found that definition **understates the divergence**. In four files the
throw is not merely reported — it is **routed**, into a `.catch` that performs a
fallback, repaints the UI, or re-invokes a callback with sentinel state.
Swapping there does not suppress a crash; it produces a *different, plausible-looking
UI state*.

Answering trap 6 directly: **load-bearing throws are real.**

### 4.2 Every site where a probe found the throw may be load-bearing

Do not treat any of these as a drop-in.

| Site | What the throw currently does |
|---|---|
| `WebPage_Settings.h:3812` | 6th of 9 statements in a `.then()` with a real `.catch` at `:3818`. Verified in source. `hw.show()` lets `setDirty(false)`, `startDirtyPoll()` and `renderBondedModules()` run — rendering bonded module panels into a still-hidden host while status reads "Synced". **Highest-consequence SUPERSET site.** |
| `WebPage_Settings.h:2294` | `.catch` rewrites both cert-status labels to "Unknown". Swallowing leaves the optimistic "Present"/"Missing" text. |
| `WebPage_Settings.h:2385` | `.catch` overwrites "Generated!" with a red error. Swallowing leaves "Generated!" on a failure. |
| `WebPage_Settings.h:2323` | The throw is what **stops the `httpsEnabled` write from being dispatched**. Swallowing flips a security-relevant setting from never-written to written. |
| `WebPage_ESPNow.h:501-515` | `.catch` at `:551-554` is the **fallback trigger** (`refreshStatus()` down the non-batch path), not a logger. |
| `WebPage_ESPNow.h:391-414` | `.catch` at `:465-467` **paints the error into the visible status pane**. |
| `WebPage_ESPNow.h:2758, 2779, 2792, 2793, 2817, 2837, 2857, 2863, 2868, 2873` | Ten bare `addEventListener` in `setupButtonHandlers`. The `try/catch` at `:3584` wraps only the DOMContentLoaded *registration*, so a throw also kills `refreshStatusBatch()` at `:3589`. The function **deliberately mixes idioms** — it defines a null-tolerant `_on` at `:2757` for optional controls and uses bare `addEventListener` for mandatory ones. Rewriting the ten erases an intentional must-exist/may-not-exist distinction. |
| `WebPage_Automations.h:297, 299, 309, 311` | `.catch` at `:315-318` writes "Error checking status: " into `#auto-status-text`. `hw.hide()` leaves a self-contradicting UI: status reads "enabled and running" while the Enable button and "Disabled" banner are both still on screen. |
| `WebPage_LLM.h:321, 377, 383` (catch `:427`); `:1032, 1040` (catch `:1023`) | **Measured, not argued.** The display-only rewrite produced `FAIL ready: pill shows Ready [Offline]` and `FAIL steady: model-loaded said exactly once [0 times]`. `menuFetchAll`'s catch re-invokes its own `done` callback a second time with sentinel `gen=-1, acc=[]`. (Moot — LLM is EXCLUDE, see §7 — but it is the empirical proof the mechanism is real.) |
| `WebPage_Logging.h:521, 522, 535, 562, 844, 845, 857, 871` (catch `:441-447`); `:415` | `.catch` is a **fallback trigger**, switching page init from the batched `/api/logging/status` path to the legacy per-command `/api/cli` path. |

### 4.3 The counter-finding, which matters equally

In **5 of 10 probes' files the SUPERSET bucket is empty in practice** — every
site is already explicitly `if (el)`-guarded, or operates on a `createElement`
result or a NodeList member, so no throw exists to be load-bearing.

Checked and clear (do not re-derive): all 28 Camera/EdgeImpulse/Microphone
SUPERSET sites; all 20 `WebPage_Maps.h` SUPERSET sites; all 15
Dashboard/Sensors sites; all 15 `WebServer_Utils.h` sites; all 27 across the 15
small files.

That inverts where the danger lies:

> **The risk in this tier is not the silent no-op. It is the guard-deletion step
> that adoption invites.** "`hw.hide` already null-checks, so the `if` is
> redundant" is the natural next thought, and it is wrong wherever the guard
> carries a second conjunct. See blocker B3.

### 4.4 Recommendation

**Split the tier and take only the provably-guarded half, with the guard preserved verbatim.**

1. **Adopt now (~70 sites, catalogued in §6 C7):** every SUPERSET site that is
   already `if (el)`-guarded, or operates on a `createElement` result or a
   NodeList member. These are exact. Keep the existing guard byte-for-byte:
   ```js
   if (cwn) { cwn.style.display = 'none'; }   // before
   if (cwn) { hw.hide(cwn); }                 // after — guard KEPT
   ```
2. **Do not promote them to STRICT.** The class label is what licenses the guard
   deletion. Leaving them labelled SUPERSET is the mechanism that keeps the
   guard in the source.
3. **Hold every site in §4.2** — every SUPERSET display or `addEventListener`
   site inside a `.then(...).catch(...)` chain — until someone makes an explicit
   fail-loud vs fail-silent decision. That is a product decision, not a
   refactoring one.

**Why:** the null-elision is harmless where a guard already exists and the
divergence is unreachable; it is a behaviour change where the throw is routed.
Splitting on "is there already a guard?" is a test a tool can apply, and it
leaves the ~20 genuinely divergent sites in human hands.

---

## 5. Decision required: the API name

**This is a decision for the user, not a recommendation to apply silently.**

`hw._ge` is underscore-prefixed — private by convention. This pass would create
roughly **999 external callers of a private helper**, which is the opposite of
what the underscore is for. The other five helpers this pass uses
(`hw.qs`, `hw.qsa`, `hw.on`, `hw.show`, `hw.hide`) are already public names;
`_ge` is the only private one in the set.

Measured byte deltas over ~999 sites:

| Option | Call form | Total saved | Δ vs `hw._ge` | Runtime cost |
|---|---|---:|---:|---|
| **A** — use `hw._ge` as-is | `hw._ge('x')` | 16,983 B | — | 0 B |
| **B** — add public alias `hw.$` | `hw.$('x')` | 18,981 B | **+1,998 B** | +12 B (`hw.$=hw._ge;`) |
| **C** — add public alias `hw.el` | `hw.el('x')` | 17,982 B | +999 B | +13 B |

**Option A** ships the smallest diff and adds no API, but enshrines a private
name as the most-called function in the web UI. The underscore then means
nothing, and a future maintainer who reads it as "internal, safe to change"
would break 999 sites.

**Option B** is ~2 KB cheaper across the pass and reads as a deliberate public
API. Cost: `$` is conventionally jQuery. That is a real concern here and not a
stylistic one — `WebPage_DarkRoom.h` embeds actual minified jQuery
(`:44, :46, :48`), so `$` is genuinely taken elsewhere in this repo, and
`WebPage_Settings.h:2431` already defines its own `window.$ = function(id){...}`
page-local shim. Two different `$` semantics in one codebase is a trap.

**Option C** is unambiguous, public, and costs 1 KB more than B.

**Recommended: Option B, `hw.$`, with a caveat — or Option C if the jQuery
collision bothers you.**

Reasoning: B is the cheapest *and* fixes the naming problem, and the collision is
containable because `WebPage_DarkRoom.h` is out of scope entirely (blocker B9)
and `WebPage_Settings.h:2431` is a page-local `window.$` that this pass already
touches. If that page-local shim is retargeted to `hw.$` in the same change, the
collision disappears and Settings gets 79 call sites for free. If you would
rather not spend the review effort on that interaction, take **C (`hw.el`)** —
it costs 1 KB, has no collision, and needs no coordination.

Either way, **keep `hw._ge` defined** as the implementation and make the new name
a pure alias. That preserves the 40 existing `hw._ge` callers with a zero-line diff.

---

## 6. Blockers and care items

This section is what makes the rewrite safe. Encode every blocker in the tool
before generating a single edit.

### BLOCKERS — must be encoded or the rewrite is wrong

---

**B1 — Context-passing `querySelector`/`querySelectorAll` is never STRICT.**

Verified at `WebServer_Utils.cpp:773`: `hw.qs = function(s,c){ return (c||document).querySelector(s) }`.
It is **not** `c.querySelector(s)`.

A falsy receiver does not throw and does not no-op — it **silently re-roots the
query at `document` and returns a real, wrong element.** That is strictly worse
than either STRICT or SUPERSET semantics. Reported independently by 7 of 10 probes.

The class definition is what is wrong, and it applies to **all 133
`querySelector` sites**, not only those below.

- `WebPage_Automations.h` — 48 sites: `577, 593, 740, 796, 807-811, 1113, 1115,
  1126, 1133, 1134, 1136, 1142, 1149, 1152, 1155, 1159, 1164, 1166, 1170, 1172,
  1176, 1180, 1183, 1191, 1194-1196, 1199, 1207, 1208, 1214, 1215, 1217, 1219,
  1220, 1305, 1315-1320, 1583`.
  Worst case is `getSecondaryTriggerData` / `populateSecondaryTrigger`
  (`1149-1220`): a falsy `row` would serialize **row 1's values for every
  secondary trigger row**, writing corrupt automation config to the device with
  no error anywhere.
- `WebPage_ESPNow.h:3495, 3517` — verified in source:
  `var passInp = panel ? panel.querySelector('.mesh-pass-input') : null;`
  The guard is an **inline ternary fused to the call site**, so `hw.qs` looks
  null-safe and dropping the ternary is the natural simplification. Consequence:
  `espnowsetpassphrase <this label> "<other row's passphrase>"` — a wrong mesh
  key written to flash. Same shape at `:3517` for `.mesh-rename-input`.
- `WebPage_Settings.h:932, 952` — verified: `container` at `:931` is the **only
  unguarded read** of `network-dynamic-container` in the file (`:838` and `:926`
  both guard with `if (container)`). `hw.qsa` would feed every `[id^="net-"]` in
  the page to the network save loop.
- `WebPage_Maps.h:2352, 2395` — receiver proven non-null only by an
  `if (!el) return;` 12 and 53 lines earlier.
- `System_Microphone_Web.h:178`, `WebPage_LLM.h:811`, `WebPage_Dashboard.h:601`.

**Rule:** reclassify all context-passing calls as SUPERSET, never drop the
receiver guard, and never emit `hw.qs(sel)` where the source had a receiver.

---

**B2 — Read which branch holds `'none'` before emitting `hw.toggle`.**

`X.style.display = C ? 'none' : ''` requires `hw.toggle(X, !C)`.

A repo-wide scan found **14 inverted-polarity display writes**, of which 4 are
live `hw.toggle` candidates. The classifier's emitted hint string is
**byte-identical for both polarities** — it carries no polarity information at all.

- `WebServer_Utils.h:1532` — verified in source:
  `ca.style.display=al?'none':'';` two lines below
  `inp.style.display=ip?'':'none';`. Following the hint mechanically **removes
  Cancel from every `hwConfirm()`/`hwPrompt()` and adds one to every
  `hwAlert()`**, on the shared dialog used by every authenticated page.
  **Highest-consequence single site in the corpus.**
- `WebPage_Bluetooth.h:574` — verified:
  `if(openBtn) openBtn.style.display = isOn ? 'none' : '';`
  with *normal* polarity on the adjacent `:575`. With BLE running you would see
  "Enable Bluetooth" and no "Disable".
- `WebPage_ESPNow.h:3313` — adjacent to `:3312` which has opposite polarity;
  reusing `hasFreeSlot` inverts the "All 4 mesh slots configured" banner. Same
  adjacency at `:3270/:3271`.
- `WebPage_Settings.h:1628` — verified: `body.style.display = open ? 'none' : '';`
  inverts the notification-family accordion so a header click can never open a group.

Inverted **but currently NEAR** (non-`''` false branch) — do not promote these
when an `hw.show(x, value)` API lands: `WebPage_Speech.h:290`,
`WebPage_Automations.h:298, 300, 308`, `WebPage_Sensors.h:278, 486`,
`System_EdgeImpulse_Web.h:245, 251`, `WebPage_Bluetooth.h:568`,
`WebPage_Settings.h:2850`.

---

**B3 — Never delete an existing guard because `hw.*` "already null-checks".**

`hw.on` / `hw.hide` / `hw.show` guard **only their first argument**. Compound
guards carry extra conjuncts `hw.*` cannot express. This is the real danger in
the SUPERSET tier.

- `WebPage_Bluetooth.h:528, 530` — verified in source:
  `if(advBtn && bleMode === 'client') advBtn.style.display = 'none';`
  Deleting the guard hides Advertise and Disconnect in **server** mode, the only
  mode where they work. Correct form: `if(bleMode === 'client') hw.hide(advBtn);`
  This is in the one page-file of probe 10's fifteen that **actually compiles today**.
- `System_Camera_DVP_Web.h:531` — `if (streamBtn && streamStopBtn && img)` guards
  the `hw.on` conversions at `:532`/`:537`; `hw.on` cannot express the third conjunct.
- `WebPage_ESPNow.h:3495, 3517` — ternary guard fused to the call (see B1).
- All ~70 already-guarded SUPERSET sites listed in C7.

---

**B4 — Never rewrite a page whose tests supply their own `hw`, or whose tests
regex the C++ source for the raw idiom.**

Demonstrated empirically by a probe, not argued.

- `WebPage_LLM.h:234-1121` (all 50 sites) —
  `tools/webui/harness/llm_page_harness.js:422` stubs `hw` with **only**
  `fetchJSON` / `postJSON` / `postFormText`, and executes page JS via
  `new Function("document","window","hw",...)` at `:512`. Measured: the
  `getElementById -> hw._ge` rewrite gives `rc=1`, zero `PASS` lines, no
  `HARNESS_RESULT` sentinel, and `Uncaught TypeError: hw._ge is not a function`
  at the top of the IIFE. The display-only rewrite fails **20 of 61** checks with
  `hw.hide is not a function`.
- `tools/webui/tests/test_llm_page.py:165` and
  `tools/webui/harness/llm_page_harness.js:546` — **two independent
  id-agreement guards** regex the page *source* for
  `getElementById(['"]id['"])`. Verified both present. Rewriting silences both
  (21 ids reported unrequested; harness emits `found no getElementById calls at all`).
  These guards exist to catch a rename landing on only one side — silencing them
  is a real loss of coverage.

**Unblock order:** extend the stub → rework both guards → re-run → *then* rewrite.

---

**B5 — Never rewrite code that cannot be served.**

No registered route, no defined shell emitter, or no caller. STRICT's premise
("`window.hw` exists here") is not merely unverified but **unprovable**, and no
hardware test can ever exercise the edit.

- `WebPage_Maps.cpp:349, 350, 391, 392, 396-398, 407-409` — `handleWaypointsPage`
  is never registered (`registerMapsHandlers` at `:509-524` has no `/waypoints`
  entry) and calls `streamPageHeader(:315)` / `streamPageFooter(:480)`, declared
  at `:48-49` and **defined nowhere in the repo**. Verified: repo-wide grep finds
  only the declaration at `WebPage_Maps.h:2952` and the definition of
  `handleWaypointsPage` itself at `WebPage_Maps.cpp:312`. Corroborated by
  `docs/PRE_1_0_CODE_HEALTH_AUDIT.md:513` (nm shows both undefined; the TU links
  only because `--gc-sections` discards the function).
- `WebPage_LoginRequired.h:36` — `streamAuthRequiredInner` has **zero callers**
  repo-wide (verified: only the definition at `:11`). Already listed in
  `firmware-dead-functions.csv:71` and `docs/PRE_1_0_CODE_HEALTH_AUDIT.md:1115`.
- `i2csensor_sths34pf80_web.h:74, 75, 83, 87, 91, 95, 109` —
  `getPresenceWebScript` has **zero callers** (verified: only the definition at
  `:71`). The live path is `streamSTHS34PF80PresenceSensorJs` at `:154-156`.
  7 of this file's 10 sites are dead even with `ENABLE_PRESENCE_SENSOR=1`.
  **Not previously recorded in either audit doc.**

Prefer *deleting* this code over excluding it — the health audit already
prescribes the first two.

---

**B6 — Never drop `addEventListener`'s third argument.**

Verified at `WebServer_Utils.cpp:773`: `hw.on = function(e,v,f){ if(e) e.addEventListener(v,f) }`
— arity 3. Any `options` / `useCapture` argument is **silently discarded**.

A scan of every non-bundled source found exactly **three** three-arg sites, and
all are on `document` — precisely the shape the "19 addEventListener on
document/window → STRICT" bucket assumes is always safe. **That bucket is
unsound as stated.**

- `WebPage_Maps.h:2433, 2442` — verified in source:
  `document.addEventListener('pointerdown', _searchOutside, { once: true, capture: true });`
  Losing `capture:true` breaks click-outside-to-close ordering; losing
  `once:true` **leaks a live pointerdown listener every time the search box is
  opened**.
  **Cross-probe conflict:** the Maps probe classified all 12 of the file's
  `addEventListener` sites without flagging these; the Utils probe found them.
  Resolved by direct verification — stricter verdict taken → NEAR.
- `System_EdgeImpulse_Web.h:421` — `btn.addEventListener('click', function(e){...}, true)`
  with a source comment "use capturing to intercept before generic handler" and
  `e.stopImmediatePropagation()` at `:422`. Converting would double-fire with the
  bubble-phase listener bound at `:81` and issue a **duplicate CLI command**.
  Already correctly NEAR.
- `WebPage_DarkRoom.h:1557` — outside the corpus (see B9).

---

**B7 — Balanced-paren, string-aware parsing is mandatory.**

Never extract an argument by scanning to the first `)`, or a receiver by scanning
left to the first `(`. **Two tool bugs are already proven in the classifier's own
emitted output.**

- **Receiver mis-capture** — `WebPage_Automations.h:1113, 1115, 1136, 1164, 1170, 1176`.
  Verified in source: `const v=parseInt(row.querySelector('.st-interval-value').value,10);`
  A left-scan grabs the *enclosing* call and emits
  `hw.qs('.st-interval-value', parseInt(row))` — not valid JS. Same for
  `fillEventKindSelect(node...)`, `stTypeChanged(node...)`, `stRecurChanged(row...)`.
  Will recur wherever a scoped `querySelector` nests inside another call.
- **Argument truncation** — `WebPage_Settings.h:2043, 2051`. The classifier's own
  suggested replacements `hw._ge('dbg-'+g.replace(/[^a-z0-9]/g,)` and
  `hw._ge('dbg-'+gn.replace(/[^a-z0-9]/g)` are invalid JavaScript, truncated at
  the first inner `)` and **inconsistently between the two**. Verified the real
  source is `.replace(/[^a-z0-9]/g,'')` in both. Applying verbatim breaks the
  debug panel group toggle and `dbgToggleAll`.
- **Parens or `)` inside the argument** — `WebPage_Settings.h:932, 952, 1476, 1504, 1686`
  (`'[id^="net-"]:not([disabled])'`, `.replace(/\./g, '-')` which also contains a
  top-level-looking comma).
- **Argument order inversion** — raw is receiver-first `ctx.querySelectorAll(sel)`;
  `hw.*` is selector-first `hw.qsa(sel, ctx)`. 16 of 23 `WebPage_Settings.h`
  querySelector sites carry a context; a rewriter preserving textual order emits
  `hw.qsa(ctx, sel)`, which still runs and either throws in the wrong place or
  silently matches nothing.
- **Member-expression receivers** — `WebPage_LLM.h:534, 772, 796, 802, 873` use
  `ctx.retryBtn` / `currentCtx.retryBtn`. A `(\w+)\.style\.display` capture
  produced `ctx.hw.hide(retryBtn)` and crashed the harness with
  `Cannot read properties of undefined (reading 'hide')`.

---

**B8 — Never treat a `.style.display` comparison as a write.**

The classifier confuses `===` with `=` and captures the comparison's right-hand
side as an assignment RHS. **These 11 sites are not candidates in any class.**

There is no `hw.*` read accessor **and none should be added** — several of these
reads are load-bearing state, so inventing `hw.isHidden` / `hw.getDisplay` would
create a new coupling rather than remove one.

- `WebPage_Settings.h:13` (`var isHidden = (p.style.display === 'none' || !p.style.display)`),
  `:2849` (`var isVisible = dropdown.style.display === 'block'`),
  `:2843` (emitted **twice** — one real write plus the read inside its own
  ternary condition, inflating the NEAR total).
- `WebPage_Logging.h:775, 1610` (the reason string literally shows the regex
  swallowing `){ togglePane(...` as the RHS).
- `System_Camera_DVP_Web.h:153, 271, 275` and `System_Microphone_Web.h:211, 218`
  (each line emitted with one extra write-candidate).
- `WebPage_Dashboard.h:726` — **load-bearing:** `openLayoutEditor` rebuilds
  `_editHidden` by reading exactly `=== 'none'`, the value `hw.hide` writes. The
  round-trip is only safe while both halves agree.

**Related reads the classifier got right — do not let a looser pass catch them:**
`WebServer_Utils.h:1538` (`closeD(inp.style.display!=='none' ? inp.value : true)`
is how `hwPrompt` returns text and `hwConfirm` returns `true`);
`WebPage_Maps.h:2406, 2415` (treat `''` as CLOSED, so `hw.show` would make both
overlay panels **impossible to close**); `WebPage_Settings.h:1627`;
`WebPage_Automations.h:798`; `WebPage_ESPNow.h:307, 818, 844, 2011, 3395`
(`pairMeshArg` reads display to decide whether to append a mesh name to the pair
CLI command).

---

**B9 — Never run the rewrite repo-wide. Scope it to the explicit 33-file corpus.**

Pages that build their own `<!DOCTYPE` shell have no `window.hw`, and some
contain bundled minified vendor JS. A repo-wide regex would corrupt them
silently and **the build would stay green**.

- `WebPage_Games.h` — already named in the original EXCLUDE justification; 109
  idiom-bearing lines; the one place `hw` is shadowed (verified: `var hw = ...`
  as a half-width local at `:2891, :4588, :4683, :9157, :10063`).
- `WebPage_DarkRoom.h` — **not** named in the original EXCLUDE justification and
  not assigned to any probe. Verified `<!DOCTYPE html>` at `:25` inside
  `R"ADR(`, with no `streamBeginHtml` anywhere in the file. Idiom hits at
  `:44, :46, :48` are inside **minified jQuery**; `:1557` is a 3-arg
  `addEventListener`. (Also excluded by name from the syntax gate via
  `extract_js.WALK_EXCLUDE_NAMES`.)
- `WebServer_Server.cpp:4025` — a second self-built shell beside the icon-test
  page at `:5036`: `"<!DOCTYPE html><html><head><title>Login Success</title></head><body>"`
  plus its own `<script>`, no `streamBeginHtml`, no `hw.*`. Zero idioms today, so
  the EXCLUDE set for that file is **two** pages, not one.
- **`WebServer_Server.cpp:4426` — NEW, found while verifying this spec and named
  in no probe report.** `streamViewerHead()` is a **third** self-built shell
  (`<!DOCTYPE html><html data-theme="...">`, its own `<head>`/`<style>`, no
  `streamBeginHtml`), and unlike the other two it is **live**, with two callers
  at `:4580` and `:4902` (the file-viewer pages). It contains **zero idiom sites
  today** — verified by scanning `:4424-4600` — so it never surfaced in the
  corpus. Record it: the EXCLUDE set for `WebServer_Server.cpp` is **three**
  shells, and any future idiom added under `streamViewerHead` must not be
  classified STRICT.

---

### CARE ITEMS — true, and the rewrite must respect them

---

**C1 — Ordering is proven safe corpus-wide. One genuine violation exists and is already excluded.**

All 10 probes agree, and the decisive line was verified for this document:
`WebServer_Utils.cpp:772` is the closing `}` of the `if (!isPublic) {` opened at
`:725`, and `:773` (the hw core: `qs`/`qsa`/`on`/`_ge`/`setText`/`setHTML`/
`show`/`hide`/`toggle` + fetch helpers) sits **outside** it — despite a
misleading 4-space indent versus `:774`'s 2-space. So the core reaches public
pages too.

Every page streams via `streamBeginHtml` first. No file has a `<head>` script,
`defer`, `async`, `type=module`, or `document.write`. Classic inline scripts
execute in document order and HTTP chunking cannot reorder bytes.

The strongest corroboration is positive rather than static: **30+ pages already
call `hw.fetchJSON` / `hw.postFormText` / `hw._ge` at parse time and ship working.**

- `WebServer_Utils.cpp:762` — the **one real violation**. The theme/guest block
  is streamed **before** the hw core at `:773` and calls `hw.initTheme()`
  synchronously on its last statement. The source itself concedes the gap:
  verified in place, `hw.loadThemePref` / `saveThemePref` are written
  `(hw.fetchJSON ? hw.fetchJSON(u) : fetch(u,{...}))` and **the fallback branch
  is always taken**. Correctly EXCLUDE — but the recorded reason ("no `hw.*` on
  this page") is **false**: `hw` is on that page; the block literally creates it
  (`var hw=w.hw||(w.hw={})`).
  **The general rule: order is determined by position within `streamBeginHtml`,
  not by whether `hw` reaches the page.**
- `WebServer_Utils.cpp:813, 837, 842` — the toast block is streamed at `:808`,
  **after** the core, so `hw._ge`/`hw.on` are available and these are technically
  drop-ins. Recorded as EXCLUDE for the same false reason. Keep them excluded on
  the **correct** ground: they are part of the hw runtime, and defining the
  runtime in terms of itself buys nothing.

---

**C2 — `hw.show` writes `style.display=''`. It removes the inline declaration and falls back to the cascade.**

It is **not** "restore the previous value" and is **not** interchangeable with
`'block'` / `'flex'` / `'inline-block'` / `'grid'`. Corollary: **NEAR is not a
uniform bucket**, and any future `hw.show(x, value)` API must be applied
per-site, never as a blanket sweep.

Proven non-equivalences (checked against real CSS, not assumed):

- `.btn{display:inline-flex}` and `button.btn,a.btn{display:inline-flex}` at
  `WebServer_Utils.h:1489, 1493` override nothing when display is cleared, so
  `hw.show` **re-enables centering the raw `'inline-block'` suppressed**:
  `WebPage_Logging.h:536, 561, 858, 870`, `WebPage_Files.h:147, 148`,
  `WebPage_Speech.h:290, 291`.
- Class-level `display:none` means a bare `hw.show` leaves the element
  **hidden**: `WebPage_ESPNow.h:396, 506` (`.en-header-status`), `#setup-error`
  (`.setup-modal-error`, `:98`), `#mesh-warning` (`.mesh-warning`, `:87`).
- **No CSS rule exists at all**, so the display value lives only in the JS:
  `WebPage_Dashboard.h:447` and `WebPage_Sensors.h:224, 672` (`display:'grid'`;
  grep for `sensor-status-grid` returns exactly one hit, the inline style at
  `WebPage_Dashboard.h:212`) — `hw.show` stacks every sensor card full-width.
- Flex containers that lose centering: `WebServer_Utils.h:1533` (`#hw-dlg`,
  `align-items`/`justify-content` go inert), `WebPage_CLI.h:246`,
  `WebPage_Files.h:185`, `WebPage_AviPlayer.h:153` (`#avi-modal`),
  `WebPage_Settings.h:2952, 2992, 3693`.
- Wrong element type restored: `WebPage_Automations.h:1135` (a `<span>` falls
  back to `inline`, collapsing the trigger-field row), `WebPage_ESPNow.h:1294,
  1298` (`.message-log` is `display:flex` column but the JS writes `'block'`).

**NEAR-but-actually-safe today — do not conflate with the above:**
`WebPage_Settings.h:2320, 2329` (only display source is the inline style at
`:2199`/`:2203`, so `''` resolves to `block`); `WebPage_Logging.h:1115, 1116`
(unclassed divs, contingently safe); `WebPage_ESPNow.h` `#en-not-init` /
`#backup-mac-group` (explicit `'flex'` is redundant).

---

**C3 — The driver must be occurrence-based, not line-based.**

Claim one span per site; skip any site whose span overlaps an already-rewritten
one. Composition is order-dependent: `_ge`-then-hide gives
`hw.hide(hw._ge('x'))` (correct) and hide-then-`_ge` gives `hw.hide('x')`
(correct), but a display rule matching `document.getElementById('X').style.display`
**after** the `_ge` pass has rewritten the prefix silently fails to match and
leaves the line half-converted.

- Three sites on one line: `WebPage_Files.h:149, 169, 170`; `WebPage_Automations.h:1249`.
- Two sites on one line: `WebPage_Automations.h:1257, 1268, 1278, 1341, 1399,
  1408, 1415, 1421, 1437, 1445, 1471`; `WebPage_Logging.h:415, 521, 522, 844,
  845, 1115, 1116, 1546-1547`; `WebPage_Dashboard.h:372 (×2), 447, 448`;
  `WebPage_Sensors.h:406, 415`; `WebPage_Automations.h:1133` (qsa site + display
  site, different idioms).
- One expression spanning two lines: `WebPage_ESPNow.h:1800-1801` and `1856-1857`
  (`getElementById('metadata-content-'+mac) || getElementById('metadata-'+mac)`);
  `WebPage_Logging.h:1510-1511`.
- Two proposals for one line: `WebPage_Bluetooth.h:477` (body swap plus
  alias-declaration swap).

---

**C4 — Escaping: the classic hazard is absent; the real ones are different.**

The corpus is overwhelmingly C++ **raw** literals, so "naive sed breaks the `\"`
literal" largely does not apply — **zero classified sites sit on a line
containing an escaped C++ double quote.** Byte-wise, escape-preserving edits
only; never normalise.

- **JS-level `\'` inside a raw literal** (not C++ escapes — the enclosing literal
  is `R"JS(`): `WebPage_ESPNow.h:885`, where a JS string builds an
  `oninput="..."` attribute. A sed that assumes C++ escaping or normalises `\'`
  to `'` silently corrupts the emitted attribute.
- **Attribute quote shape is load-bearing and inverted between files:**
  `i2csensor_rda5807_web.h:27, 28` (attribute **double**-quoted, JS
  **single**-quoted — emitting `hw._ge("...")` terminates the attribute
  mid-expression); `WebPage_Maps.h:75` (attribute **single**-quoted, JS
  **double**-quoted — flipping the inner quotes breaks the Upload button).
  Neither file compiles in the saved config, so nothing catches it.
- **JS and C++ on one physical line** (single-line `httpd_resp_send_chunk` of a
  whole function): `i2csensor_vl53l4cx_web.h:108, 109` and
  `i2csensor_rda5807_web.h:48`. This produced the garbage NEAR verdict at
  `vl53l4cx:109` whose quoted RHS is literally `HTTPD_RESP_USE_STRLEN`.
  **Re-read every NEAR verdict whose RHS contains that string — it is proof the
  regex overran the literal.** Manual-only.
- **Raw UTF-8 em dashes** (`E2 80 94`) inside JS string literals on 13 site lines:
  `WebPage_Logging.h:545-551, 862-864, 872-874`. Rewrite byte-wise; `perl`
  without `-CSD` or a locale-sensitive `sed` can corrupt them.
- **Double quotes inside the selector argument:** `WebPage_Logging.h:903, 962,
  986, 990` (`input[type="checkbox"]`) — the sibling sites at `573, 701, 757,
  763` use the unquoted form, so these four are easy to miss when spot-checking.
- **Pre-existing already-broken attributes — preserve bytes exactly, fix as a
  SEPARATE change:** `WebPage_Settings.h:672, 678` (both WiFi "Connect" buttons)
  and `:2801` (SSID "Select") emit a literal backslash-quote inside a
  double-quoted `onclick`; HTML has no backslash escape, so the attribute
  terminates early and the handlers are truncated mid-string. Also
  `WebPage_Maps.h:2484` escapes only single quotes when interpolating an
  attacker-influenceable map feature name into an `onclick`.

---

**C5 — Compilation cannot validate this change at all.**

Every site is inside a C++ string literal. The only damage a compiler can catch
is a broken quote or a broken raw-string delimiter — and even that only where the
file compiles.

Roughly **240 of the 1,058 STRICT sites have no compile coverage** in the saved
config (`CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"`).

- **Compiles on NO board:** `System_EdgeImpulse_Web.h` (39 sites) —
  `System_BuildConfig.h:322` is a bare `#define ENABLE_EDGE_IMPULSE 0`, not
  `#ifndef`-guarded, so it cannot even be overridden. Confirmed by `strings(1)`:
  the marker id `ei-tracked-list` is absent from `build/`, `build-feathers3` and
  `build-xiao_s3`.
- **Not compiled in the saved config:** `WebPage_Maps.h` + `WebPage_Maps.cpp`
  (141 sites; `ENABLE_MAPS 0` at `System_BuildConfig.h:411`, `ENABLE_WEB_MAPS`
  force-0 at `:774-775`, and `CMakeLists.txt:481-483` drops the `.cpp` entirely —
  `grep -c WebPage_Maps build.ninja` returns 0 in both `build-xiao_s3` and
  `build-feather_esp32_v2`; the `.obj` files under `build/` are **stale** and will
  mislead anyone who checks artifacts instead of ninja rules).
  `WebPage_Speech.h` (45 sites; `ENABLE_ESP_SR 0` at `:319` forces
  `ENABLE_WEB_SPEECH 0` at `:736-738`, and the `/speech` route is never
  registered so it cannot be HW-tested either).
- **The build log lies about coverage:** `WebPage_MQTT.cpp` (26),
  `WebPage_Bond.cpp` (16) are listed in `CMakeLists.txt:470/473` so they compile
  as **empty translation units** and appear in the build log.
  `WebPage_Battery.cpp` (4): `ENABLE_WEB_BATTERY` is force-zeroed at `:1199` by
  `!ENABLE_BATTERY_MONITOR` (a board property), so it is covered only by
  `build-feathers3`. Plus 8 of 9 i2c sensor headers.
- **Not on the primary board:** `WebPage_Settings.h:78-115` (9 sites under
  `#if ENABLE_NEOPIXEL`; proven by `strings(1)` — `led-live-brightness` absent
  from `build/` and `build-xiao_s3`, present in `build-feathers3`,
  `build-feather_esp32_v2`, `build-qtpy_esp32`). `System_Camera_DVP_Web.h`
  (79 sites) is the inverse: present in `build/` and `build-xiao_s3`, **absent**
  from `build-feathers3`.
- **Whole-corpus gate:** `ENABLE_HTTP_SERVER` (derived at
  `System_BuildConfig.h:588-603` from `ENABLE_WIFI`, `NETWORK_FEATURE_LEVEL` and
  `WEB_FEATURE_LEVEL`). A `WEB_LEVEL_DISABLED` build takes every site dark at once.

> **Method warning for anyone re-probing flags:** a bare `cc -E` over
> `System_BuildConfig.h` defines no `ARDUINO_*_DEV` variant macro, falls through
> to the unsupported branch, and reports `BOARD_NAME "Unknown/Unsupported"` with
> wrong flag values. Re-run with `-DARDUINO_XIAO_ESP32S3_DEV`. Any board-gating
> answer produced without a variant define is wrong the same way.

**Plan browser verification per page, not a build.**

---

**C6 — (See §4.2.)** Trap 6 is answered there in full, with the counter-finding in §4.3.

---

**C7 — Classifier over-conservatism: ~70 SUPERSET sites are provably exact.**

Deliberately **not** applied to the final counts. Promoting them to STRICT is
exactly what invites the guard-deletion blocker (B3). **Adopt them freely with
the existing guard preserved verbatim** and they are identical in every case.

- Already `if (el)`-guarded, `createElement` results, or NodeList `forEach` params:
  `WebPage_Automations.h:312, 341, 343, 344, 374, 376, 398, 401, 404, 814, 1133,
  1459, 1572, 1809, 1829, 1902` (16);
  `WebServer_Utils.h:257, 258, 1538, 1539, 1540, 1541` (6);
  `WebPage_Logging.h:1547`;
  `WebPage_Dashboard.h:372 (×2), 447, 448, 452 (×2), 641, 748` and
  `WebPage_Sensors.h:224, 278, 399, 415, 643, 647` (15);
  `WebPage_LLM.h:287, 321, 377, 383, 531, 534, 742, 745, 772, 796, 802, 821, 873`,
  `WebPage_Files.h:200, 201, 209, 210`, `WebPage_Speech.h:372`,
  `WebPage_CLI.h:138, 139, 188`, `WebPage_AviPlayer.h:177, 180` (23);
  all 28 Camera/EI/Mic SUPERSET sites; all 27 of probe 10's;
  `WebPage_Maps.h:576, 582, 624, 2396, 2624`.
- **Arg-miscount false positives** — the classifier's comma counter walks into
  multi-line handler bodies: `WebPage_Settings.h:489`, `WebPage_Dashboard.h:812`
  and `WebPage_Bluetooth.h:861` were all tagged NEAR "3 args (options/capture)"
  but have exactly **two** arguments (Bluetooth `:861` verified byte-for-byte with
  `od -c`; Dashboard `:812` is the same shape the classifier called STRICT at
  `WebPage_Sensors.h:686`). The same broken heuristic could **mis-count a genuine
  3-arg call as 2** — which is precisely what happened at `WebPage_Maps.h:2433/2442`.
- `i2csensor_vl53l4cx_web.h:109` — tagged NEAR with RHS
  `''}}\", HTTPD_RESP_USE_STRLEN)`; the real statement is
  `if(ph){ph.textContent='...';ph.style.display=''}`, a guarded `hw.show`.

---

**C8 — Dead-but-reachable code: classification is trivially correct, value is zero, typo risk is highest.**

`window.hw` **is** present on these pages, so they stay in their current classes —
but the driver should **skip** them. Roughly 8% of `WebPage_ESPNow.h`'s "STRICT"
surface is unreachable or no-op code.

- `WebPage_ESPNow.h:2172, 2922, 2923, 2935, 2944, 2952, 2957, 2958, 2960, 2963,
  2966, 2972, 2976, 2979, 2982, 2988, 3065, 3068, 3094-3097` — 22 sites touching
  12 ids that exist **nowhere in the repo** (`send-mac`, `send-message`,
  `message-log`, `file-target-mac`, `file-path`, `file-transfer-status`,
  `remote-results-log`, `remote-device`, `remote-username`, `remote-password`,
  `remote-command`, `device-metadata-status`). The handler bodies are registered
  via `_on('btn-send-message'...)` etc. and **none of those button ids exist
  either**, so `_on`'s `if(el)` means nothing is ever wired.
- `WebPage_ESPNow.h:2890, 2895, 2900, 2905, 2910, 3129, 2180` — 7 live-but-inert
  sites: `#device-metadata-status` does not exist (Set Name/Room/Zone/Tags/
  Stationary give no feedback) and `window.addMessageToLog` is a permanent no-op
  called from 8 live call sites. **Pre-existing UI defects, not caused by the
  rewrite — flagged so nobody "verifies" a rewrite by clicking those buttons.**
- `System_EdgeImpulse_Web.h` — `streamEdgeImpulseSensorCard` is dead
  (`WebPage_Sensors.h:171` notes ML moved into the camera card), so 8 STRICT
  sites resolve to `null` in the shipped DOM: `btn-ei-settings-toggle`,
  `ei-settings`, `eiModelPath`, `ei-status-indicator`, `ei-tracked-list`,
  `ei-state-log`, `camera-stream-img`.

---

**C9 — Local alias helpers and adjacent idioms: swap the body only.**

Never change the declaration form, and **never collapse an adjacent
`.textContent=` / `.innerHTML=` into `hw.setText` / `hw.setHTML`.** That collapse
is the single most tempting wrong "improvement" in the corpus: it converts a
throw into a silent no-op at exactly the sites that are the page's only error
surface.

- `WebPage_Bluetooth.h:477` — replacing `function el(id){return document.getElementById(id)}`
  with `var el=hw._ge;` changes hoisting (function declaration vs var
  assignment). Safe **today** only because lines 433-476 of the IIFE contain
  nothing executable — a fragile invariant a future edit silently breaks. The
  body swap is exact (all 37 direct + 3 indirect call sites verified string).
  Related: the local `setText(id,v){...v||''}` at `:842` coerces
  `undefined`/`0`/`false` to `''` where `hw.setText` writes them through, and its
  parameter is **named `statusEl`** — if anyone ever passes a real element, raw
  silently no-ops while `hw._ge` passes it through and the write lands.
- `WebPage_Settings.h:2431` — `window.$ = function(id){return document.getElementById(id)}`;
  79 call sites all verified string. The one edge is `$(map[cmd])` at `:2614`
  where an unmapped `cmd` gives `undefined` (`getElementById` returns `null`,
  `hw._ge` returns `undefined`; both falsy, consumer is inert).
- **Do NOT collapse to `setText`/`setHTML`:** `WebPage_Dashboard.h:560, 571`
  (the `.catch` error reporter — a silent no-op leaves the modal stuck on
  "Loading..." forever), `:559, 573`; `WebPage_MQTT.cpp:377` (the page's **only**
  error surface, no `console.error` fallback) and `:336-343`;
  `WebPage_Settings.h:1682, 1699` (the `.catch` at `:1697` writes the auth-aware
  failure message).
- **`hw._ge`'s non-string pass-through is load-bearing in exactly one place:**
  `WebPage_ESPNow.h:2586` — `document.getElementById('master-mac')?.parentElement`
  feeds an Element into the `hw.hide`/`hw.show` sites at `:2589/2596/2603`.
  Everything else in the corpus is provably string. The one cross-file invariant
  worth watching: `i2csensor_seesaw_web.h:60, 79` and
  `i2csensor_ano_encoder_web.h:77, 82, 84, 102` take caller-supplied ids and are
  the shared ESP-NOW remote-peer path, so a future remote caller is what would
  break it.

---

**C10 — Attribute-embedded sites: 13 corpus-wide.**

Far fewer than a mechanical estimate suggests. All are inside C++ **raw**
literals, so there is **no C++ escaping to corrupt** — the risk is entirely at the
HTML/JS layer (see C4 for the quote-shape hazard). All fire on user interaction,
never during parse, so they add no ordering risk. `hw` resolves through the
inline-handler scope chain element → document → window, and no page has a
`<form>` or an element named/id'd `hw` that could shadow it.

- `WebPage_Settings.h:672, 678` — 4 sites, and both handlers are **already
  malformed** (see C4).
- `WebPage_ESPNow.h:142` — 2 `getElementById` sites plus 2 NEAR display sites
  (one read, one write) fused into **one attribute value on one source line**; a
  rewrite must edit all four together. `:885` — 1 site, the JS-escaped `oninput` builder.
- `i2csensor_rda5807_web.h:27, 28` — 2 sites; double-quoted attribute,
  single-quoted JS.
- `WebPage_Maps.h:75` — 1 site; single-quoted attribute, double-quoted JS.
- `WebServer_Utils.h:250, 272` — 4 display sites inside `onerror="..."`,
  correctly NEAR. `:272` is one ~300-char line mixing unescaped HTML-delimiter
  quotes, JS-level `\'` escapes, and a CSS `style="display:none"` that a sed keyed
  on `style.display` would also hit; and `this.nextSibling.style.display='inline-block'`
  has **no `hw.*` expression at all** (DOM-relative receiver, non-`''` value).
  **Leave both lines completely alone.**

**Zero attribute-embedded sites in:** `WebPage_Automations.h`,
`WebPage_Logging.h` (all 31 handlers are bare function calls),
`WebPage_Dashboard.h` (10 handlers, all bare `Dash.*` calls),
`WebPage_Sensors.h`, `WebPage_LLM.h`/`Files`/`Speech`/`CLI`/`AviPlayer` (14
handlers), `System_Camera_DVP_Web.h`/`EdgeImpulse`/`Microphone`.
Note several files contain `X.onclick =` **JS property assignments** that grep for
`onclick=` — these are not attributes.

---

**C11 — Sites to leave raw deliberately.**

Adoption here is a net loss even though the swap is exact. Decide these
consciously rather than by a sweep.

- `WebPage_CLI.h:135` — `window.addEventListener('error', ...)` is the page's
  last-resort JS-error reporter that paints `[JS Error] ...` into the terminal.
  Converting makes the diagnostic itself depend on the hw block having arrived
  intact; on a device with 5 HTTP sockets and chunked streaming, a truncated hw
  chunk would take out the error reporter first.
- `WebPage_AviPlayer.h` (18 sites) — currently a **zero-dependency shared
  component** injected by both `WebPage_Files.h:33/56` and
  `System_Camera_DVP_Web.h:141/199`. Adopting `hw.*` permanently couples it to
  `streamBeginHtml` so it could never be reused on a self-shelled page. It also
  has the weakest safety net in the corpus: concatenated C++ literals, so not
  parsed by the compiler, and **verified not covered** by the syntax gate
  (`blocks=1 regions=0` — the gate sees only the modal markup, never the player
  JS). The gate's own docstring confirms the class of gap: *"covers 0% of
  JavaScript assembled from ordinary (non-raw) C++ string literals."*
- `WebPage_Login.h:66` — public page (`isPublic=true`), one of only two files
  with zero `hw.*` usage today. `window.addEventListener('load', fn)` gains
  nothing from `hw.on` (window is never null) and stakes the pre-auth `revokeMsg`
  alert on the `isPublic` guard boundary at `WebServer_Utils.cpp:725-772`, which
  the 4-space indent at `:773` shows has demonstrably moved before.
- `WebPage_Maps.h:1862` — `document.removeEventListener('click', closeOnClickOutside)`
  has **no `hw.*` counterpart** (there is no `hw.off`). Its partner at `:1866` is
  a clean STRICT swap, so adopting one leaves the pair half-converted. Decide
  deliberately.
- `WebPage_Automations.h:219` — leave the inline handler text alone:
  `document.querySelector('button[onclick="createAutomation()"]')` at `:1465` and
  `:1593` selects the button by the **literal text of that attribute**. Any pass
  that reformats inline handlers silently breaks the Add / "Save Changes" relabel
  with no error.

---

## 7. Do-not-touch list (102 EXCLUDE sites)

Recorded with reasons so nobody re-files them.

| Sites | File:line | Why |
|---:|---|---|
| 50 | `WebPage_LLM.h:234-1121` | Test harness supplies its own partial `hw`; two source-regex guards go blind. **Conditional** — unblocked by fixing the harness first. See B4. |
| 23 | `WebServer_Utils.cpp:762-842`, `WebServer_Server.cpp:5081-5086` | The hw runtime itself + the icon-test page's own `<!DOCTYPE` shell at `:5036`. Note the **corrected reason** for `:762` in C1. |
| 11 | `WebPage_Settings.h:13, 2843 (dup), 2849`; `WebPage_Logging.h:775, 1610`; `System_Camera_DVP_Web.h:153, 271, 275`; `System_Microphone_Web.h:211, 218`; `WebPage_Dashboard.h:726` | `===` read misparsed as a write. Not candidates in any class; **no read accessor should be added.** See B8. |
| 10 | `WebPage_Maps.cpp:349, 350, 391, 392, 396-398, 407-409` | `handleWaypointsPage` unreachable: no route, and `streamPageHeader`/`streamPageFooter` are defined nowhere. **Prefer deleting.** See B5. |
| 7 | `i2csensor_sths34pf80_web.h:74, 75, 83, 87, 91, 95, 109` | `getPresenceWebScript` has zero callers. **New finding** — in neither audit doc. **Prefer deleting.** |
| 1 | `WebPage_LoginRequired.h:36` | `streamAuthRequiredInner` has zero callers; already in `firmware-dead-functions.csv:71`. **Prefer deleting.** |

**Out of corpus entirely — never let a repo-wide pass reach these** (B9):
`WebPage_Games.h` (shadows `hw`), `WebPage_DarkRoom.h` (own shell + minified
jQuery), `WebServer_Server.cpp:4025` (Login Success shell),
**`WebServer_Server.cpp:4426` (`streamViewerHead`, live file-viewer shell — new)**.

---

## 8. Suggested execution order

### Stage 0 — Fix the classifier before generating any edit

Three of these are already **proven** to emit invalid JavaScript.

1. Balanced-paren, string-aware argument **and receiver** extraction (B7).
2. Exclude `===` from the display-write rule (B8).
3. Arity counting that does not walk into handler bodies (B7 / C7).
4. Selector-first argument ordering for `hw.qs`/`hw.qsa` (B7).
5. Reclassify all context-passing `querySelector` as SUPERSET (B1).
6. Emit polarity with every `hw.toggle` proposal (B2).
7. Scope the file list to the 33-file corpus; hard-deny `WebPage_Games.h`,
   `WebPage_DarkRoom.h`, and the three `WebServer_Server.cpp` shells (B9).

### Stage 1 — Unscoped STRICT swaps only

`hw._ge`, receiver-less `hw.qs`/`hw.qsa`, arity-2 `hw.on` on `document`/`window`.
**These preserve every throw.** Start with the files that are compiled, live, and
covered by the syntax gate:

1. `WebPage_ESPNow.h` (257) — largest win, raw literals throughout, no C++
   escaping, page already hard-depends on `hw.*`. Skip the 29 dead sites in C8.
2. `WebPage_Automations.h` (167) — 100% raw literals, zero attribute-embedded
   sites, compiles on every HTTP-server build.
3. `WebPage_Logging.h` (97) — clean; watch the em dashes (C4) and the
   double-quoted selectors at `903, 962, 986, 990`.
4. `WebPage_Settings.h` (105) — **only after** Stage 0 item 1; sites `2043`/`2051`
   are the proven truncation case.
5. `WebPage_Dashboard.h` / `WebPage_Sensors.h` (52) — note Dashboard is
   concatenated literals, so the syntax gate does **not** cover it.
6. `WebServer_Utils.h` (24), then the small live files.

### Stage 2 — Guarded SUPERSET only

Only the ~70 sites in C7, **guard preserved verbatim**. Do not promote to STRICT.

### Stage 3 — Hand-write, one at a time, never by tool

`WebServer_Utils.h:1532`; `WebPage_Bluetooth.h:528, 530, 574`;
`WebPage_ESPNow.h:3313, 3495, 3517`; `WebPage_Settings.h:1628`.

### Stage 4 — Deferred

Everything in §4.2 (load-bearing throws) pending a fail-loud vs fail-silent
decision. All 180 NEAR sites pending an `hw.show(x, value)` API — and when it
lands, apply **per site**, never as a sweep (C2).

---

### What is verifiable on the host

`tools/webui/` holds a **JS syntax gate that parses every raw-string JS region in
the firmware** — `tools/webui/tests/test_embedded_js_syntax.py`, backed by
`tools/webui/extract_js.py` and `tools/webui/js_engine.py`.

Run from the repository root:

```sh
python3 -m unittest discover -s tools/webui/tests -t .
```

Run it **before and after** every stage; the diff in output is the signal.

Its own docstring is explicit about what a green run does **not** mean, and both
caveats bite this pass directly:

- **It is a syntax gate, not a semantic one.** It cannot tell that `hw._eg` is a
  typo. It catches a broken brace, a broken quote, a broken raw-string
  delimiter — nothing about whether the identifier exists.
- **It covers 0% of JS assembled from ordinary (non-raw) C++ literals**, and
  names the exact files: `WebPage_Dashboard.h`, the `window.hw` library in
  `WebServer_Utils.cpp`, and the i2c sensor web headers. Add
  `WebPage_AviPlayer.h` to that list — verified `blocks=1 regions=0`.
- It reads the **source**, not the build, so it covers board-gated files that no
  compiler on this machine has ever seen — which is the only automated coverage
  the ~240 uncompiled STRICT sites will ever get.

Also host-verifiable: `python3 -m unittest tools.webui.tests.test_llm_page` — the
gate that currently **blocks** `WebPage_LLM.h` (B4).

### What needs the board

Everything else. Because a typo inside a string literal is invisible to the
toolchain, **each rewritten page must be loaded in a browser and exercised.**

- **Primary board (FeatherS3):** covers Settings' `ENABLE_NEOPIXEL` block and
  `WebPage_Battery.cpp`, but **not** `System_Camera_DVP_Web.h`.
- **XIAO ESP32S3** (the saved config): covers Camera, but **not** the NEOPIXEL
  block or Battery.
- **No board covers** `System_EdgeImpulse_Web.h` (39), `WebPage_Maps.*` (141), or
  `WebPage_Speech.h` (45) — and Speech's `/speech` route is never registered, so
  it cannot be HW-tested at all. Treat all three as **unverifiable**: rewrite
  them last, or not at all, and never as part of a batch whose green result you
  intend to trust.
- **Do not verify by clicking** the ESP-NOW buttons listed in C8 — they are
  pre-existing no-ops and will look broken whether or not the rewrite is correct.
