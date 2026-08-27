# Web JS Rewrite — Claims Verification (final)

**Date:** 2026-08-24. **Method:** three-layer verification ordered after earlier analysis
errors: (1) a mechanical checker (`scratchpad/checks.py`) that turns every checkable claim
into executable PASS/FAIL, (2) a 7-agent report workflow re-deriving every semantic claim
with mandatory command+output evidence, (3) a 7-agent adversarial workflow that re-ran
every piece of evidence with default verdict OVERTURNED and hunted for what both passes
missed. **Outcome: 78 rulings — 56 UPHELD, 22 MODIFIED, 0 OVERTURNED, 0 UNPROVABLE**,
plus 19 new findings. The dry run was implemented **three times independently**
(checks.py, W1, W2's fresh lexer) and agrees byte-for-byte.

Nothing in `components/` was modified. This document is the authoritative correction
layer over `docs/WEB_JS_EXACT_DROPIN_SPEC.md` (see the banner there).

---

## 1. The error ledger — what was actually wrong, and the failure modes

Errors found in earlier analysis (all now corrected):

| # | Error | Failure mode to never repeat |
|---|---|---|
| 1 | "3-arg listener sites: exactly 3, all on document" — there are **4**; `WebPage_CLI.h:187` is on `window`, `System_EdgeImpulse_Web.h:421` closes `}, true);` at :431 | **Single-line reads of multi-line calls lie.** Read to the closing paren. |
| 2 | Naive `grep '#define'` flag readings (ANO=1, WEB_SENSORS=0, HTTP_SERVER=0 — all wrong) | Derived flags need **preprocessor resolution** (`cc -E` with labeled `#if` probes; macro names in probe output get expanded — label with undefined tokens). |
| 3 | Flag probe treated as global truth — flags are **per-board** (`-DARDUINO_<VARIANT>_DEV`); camera XIAO-only, NeoPixel non-XIAO-only | Per-board truth comes from `compile_commands.json` / `ninja -t deps`, never from one preprocessor pass. |
| 4 | First audit draft shipped "CONFIRMED" labels while its verify agents had crashed on a schema error | **A verdict that no verifier produced is not a verdict.** Check agent exit states before trusting labels. |
| 5 | "hw.$ collides with window.$" — different names; no collision | Check the actual namespace, don't pattern-match. |
| 6 | "rewriting LLM.h silently blinds its tests" — it fails **loud** (python-side guard lists all 21 ids; only the harness-side guard is masked) | Run the experiment (a replica was built and all 4 experiments reproduced) instead of predicting. |
| 7 | "ENABLE_MAPS=0 everywhere so Maps never builds" — the user flips flags live; `build-g2-map-hwtest` (Aug 22) is a complete maps-enabled image | Deadness arguments from current flag values are invalid in this repo. Use link maps / route registration. |
| 8 | Build-dir staleness judged by `git log` date — the operative header changes were **uncommitted**; only 2 of 7 build dirs are actually fresh | Judge staleness by **working-tree mtime/content hash**, never last-commit date. |
| 9 | Spec says Games.h has 5 `var hw` shadows — it has **7** (adds :16187, :18360); spec's exclusion list also omitted Games.h from the dry-run set | Counts belong to scripts, not prose. |
| 10 | Rename cost claim "no extra HW surface" — false: R1_Health.cpp and Battery.cpp are rename-only files (2 extra page smoke-tests); byte saving is **1,008 B**, not "~1.06–1.09 KB" | Every supporting sub-claim gets checked, not just the headline. |

## 2. The verified plan of record

**The pass:** prefix-only textual rewrite inside C++ string literals:
`document.getElementById(` → `hw._ge(` (968 sites), `document.querySelector(` → `hw.qs(`
(17), `document.querySelectorAll(` → `hw.qsa(` (30).

**Scope (triple-reproduced, byte-exact): 27 files, 1,015 sites, 17,315 B saved.**
Exclusion set — all of: `WebPage_LoginSuccess.h`, `WebServer_Utils.cpp`,
`WebPage_Maps.cpp` (dead waypoints page), `WebPage_LLM.h` (wave 2),
`WebServer_Server.cpp` self-shelled spans, **`WebPage_Games.h`**, `WebPage_DarkRoom.h`.

**Why it is safe (proven, not asserted):**
- `hw._ge/qs/qsa` are verbatim aliases for the document-rooted forms; null returns and
  subsequent throws are preserved bit-for-bit. Every "load-bearing throw" blocker binds
  only the deferred SUPERSET tier.
- Zero occurrences of the pattern in user-visible JS strings, JS comments, template
  literals, regex literals, or split across adjacent C++ literals (scanned; counts match
  raw-text counts exactly). Zero receiver-prefixed `X.document.getElementById`.
- Zero `hw` shadowing in scope; served-JS enumeration is closed (no JS-serving file
  exists outside targets+exclusions, verified recursively across all components).
- End-to-end: the rewrite applied to the 5 biggest files parses green through the repo's
  own gate, with working negative controls (region-level sabotage caught; C++ escaping
  intact; line/quote invariants hold per file).
- All handler chains emit the hw core (unguarded at `WebServer_Utils.cpp:773`, public
  pages included) before any target JS. `WebPage_R1_Health.cpp` (13× `hw._ge`, zero raw
  sites) is the existence proof of the end state.

**HARD-DENY (self-recursion landmine):** `WebServer_Utils.cpp:762-842` contains
`document.getElementById` inside the hw definitions themselves. Rewriting :773 turns
`hw._ge` into infinite recursion — **every page dead on first helper call, build stays
green, gate can't see it.** The driver must hard-deny this span by name, not rely on
file-level exclusion lists staying in sync.

**Waves (corrected):**
1. **Wave 1** — 13 gate-covered files, 835 sites. 820 are gate-verifiable; **15 are not**
   (attribute-embedded `onclick` JS + non-`<script>` raw strings, incl. the 5
   `hwConfirm/hwPrompt` dialog sites at `WebServer_Utils.h:1520-1524` — the highest
   blast-radius JS in the corpus). The driver must emit the authoritative per-site
   overhang roster at run time and require a **named manual diff** of those lines.
   Delete dead `getPresenceWebScript`+`getPresenceWebCard` (sths34pf80) first — 7 of
   that file's 10 sites are dead JS; its 3 live sites move to wave 3.
2. **Wave 2** — `WebPage_LLM.h` (21 sites) + the ~10-line tooling patch (extend the two
   id-regexes to accept the new name; add `_ge` to the harness stub; hoist the harness's
   id-agreement guard above the `new Function` call — its "precondition" comment is
   currently false). Patch verified in a replica: 15/15 green both ways, negative
   control still bites.
3. **Wave 3** — extend `tools/webui/extract_js.py` to concatenated literals FIRST, then
   the 14 gate-invisible files (~180 sites).
4. **Builds:** two lanes are mandatory, not optional — feathers3 (NeoPixel=1 covers the
   8 Settings LED sites) **and** xiao_s3 (camera=1 covers the 42 camera sites). Then one
   HW session; per standing rule, no commit until the user HW-tests.

**Verification tiers (final):** 653 gate+compile / 167 gate-only / 136 compile-only /
59 dark = 1,015. Sites with no JS parse anywhere: 195.

## 3. The one open decision — the final name

The adversarial ruling on naming: **rename `hw._ge` → public name, as a pure rename
(never an alias), folded into the same finish→HW-test→commit cycle — conditional on the
rewrite proceeding** (if the rewrite doesn't happen, keep `_ge` and touch nothing).
Deciding fact (objective, survived attack): zero consumers of the literal outside 4
component files — verified repo-wide including untracked dirs, the Android app, and CM5 —
and a grep-zero postcondition proves sed completeness. Cost: R1_Health + Battery pages
join the HW smoke list.

What facts **cannot** decide: `el` vs `$`. `hw.$(...)` saves 2,016 B, `hw.el(...)` saves
1,008 B; `$` reads as jQuery, `el` reads as "element". Both collide with nothing. This is
the user's call; the spec's old §naming (hw.$-as-alias) is superseded either way.

## 4. Dead code found in passing (report-only; nothing deleted)

`handleWaypointsPage` (+ would-be link error: `streamPageHeader/Footer` defined nowhere;
two link maps show it discarded), `handleSensorsStatus` (superseded),
`getPresenceWebScript`/`getPresenceWebCard`, `hw.cycleTheme` (dead — and therefore the
theme toggle can never reach the 'system' pref: a real UI bug),
`streamEdgeImpulseSensorCard` (dead duplicate; live card ships from
`System_Camera_DVP_Web.h:98-100`), `streamLoginSuccessContent`, plus 12 zero-caller
`httpd_req_t*` functions (list in the W2 output). Spec landmine for the deferred listener
tier: B6/R4 must say **4** three-arg sites, minus **3** on document/window.
