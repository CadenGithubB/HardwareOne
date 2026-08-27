# G2 Interactive Settings Editor — Plan

Status: **IMPLEMENTED 2026-07-22 (uncommitted, pending hardware build + test).** Original
plan produced 2026-07-21 from a 7-agent sweep; implemented and adversarially reviewed on
2026-07-22.

## What landed (2026-07-22)

- **NEW `components/hardwareone/System_SettingsEditorCore.{h,cpp}`** — display-independent
  helpers (visibility, editability with a per-surface type-mask, width-correct value read,
  the enum-options parser, command resolution/existence) lifted out of the OLED editor so
  the G2 lens (present on boards with no OLED) can share them. Added to the **unconditional**
  CMake source list. OLED calls them via thin wrappers (`SETTINGS_EDIT_MASK_OLED` =
  INT/BOOL/STRING — behaviour unchanged from the working tree).
- **`G2_Page_Settings.cpp`** — "Pretty" → "INTERACTIVE" editor. Four levels
  MODULES/GROUPS/ENTRIES/PICK. Group drill-down for modules with ≥2 buckets (debug's
  authentication/http/… families); entry rows render `label` (debug rows share jsonKey
  "enabled"). Tap → bool flip (`cmd 0/1`) / enum pick-list / keyboard (numeric + string).
  Commit via `g2SubmitHijackCommand`; success = positive "OK" prefix; readOnly / isSecret /
  bitmask / no-command refused with a banner; string >32 chars or containing `"` refused;
  empty commit refused; keyboard re-asserts the hijack page before submit.
- **Pagination fix**: `SET_VISIBLE_MODULE_ROWS` 10→9 (stays inside the proven 13-row
  single-fragment budget; middle module pages now render both prev+next → all reachable).
- **Action-table row→item maps** at every level (incl. modules) — no fragile index math.
- **Async-completion staleness** (`gNavGen` display generation, passed as the commit
  callback's userData): a redraw is dropped if the settings display changed between submit
  and completion (cross-level navigation, or pick re-tap), so a completion never repaints
  the wrong sub-level and a pick-window tap can't misroute to the entry editor.

Type-scope decision: G2 edits **all** scalar types via the keyboard (`SETTINGS_EDIT_MASK_ALL`
exists); OLED stays INT/BOOL/STRING. The stale-keyboard-session-after-safety-timeout hazard
was **not** touched (deferred). Could not compile in-session (no IDF toolchain) — hardware
build + test is the remaining step; per the board-gate rule the XIAO no-OLED path needs the
real build.

---

Original plan below (file:line references verified 2026-07-21; some line numbers have since
shifted — re-verify before relying on them).

## Goal

Rename the G2 Settings page's "Pretty" view to "Interactive" and make it an editor:
drill into a settings group, tap an individual setting row, and edit it the way the OLED
settings editor does — flip booleans, pick from enum lists, type strings/numbers on the
G2 keyboard — committing through the real per-setting CLI commands.

## Current state (verified)

- The drill-down **already exists**: Level 1 module list → Level 2 per-module `key=value`
  rows in a LIST widget, paginated (`G2_Page_Settings.cpp`). Tapping a setting row is an
  explicit read-only no-op at `G2_Page_Settings.cpp:649` — that no-op is the entire
  insertion point for this feature.
- The shared settings registry carries the full edit model per entry
  (`System_Settings.h:1182` `SettingEntry`): type (7 kinds), `minVal`/`maxVal`,
  `options` pick-list metadata, `isSecret`, `readOnly`, `label`, and `cmdKey`
  (real CLI command; `nullptr` = jsonKey is the command). One registry serves web, OLED,
  and G2 — no schema work needed.
- Commit chokepoint exists: `g2SubmitHijackCommand` (`G2_HijackCmd.cpp:85`) → cmd_exec →
  `authorizeCommand` as the paired user, completion callback with the result string.
  Command line = `(cmdKey ?: jsonKey) + " " + value`, same rule as
  `OLED_SettingsEditor.cpp:258`.
- Feedback plumbing exists and works today: gen-guarded `LensUiJob` **Redraw** jobs are
  staleness-dropped (`G2_Glasses.cpp:10216`); only PageSwap is log-only.
- Compiled-in interaction precedents on FeatherS3: Power CPU picker (pick-list),
  Network toggles + keyboard→command commits, Files rename/delete (keyboard guards,
  positive-OK classifier, action-table dispatch), LED color picker, Users menuGen hygiene.
  G2_Page_CameraSettings is the closest architectural precedent (table-driven cycle/pick/
  commit editor) but is **compiled out on FeatherS3** (camera gate) — design reference only.

## Increments

1. **Fix the live pagination bug.** 23 modules register on FeatherS3 but
   `SET_TOTAL_MODULES_ROWS=13` can't hold back + toggle + 10 modules + Prev + Next, so
   `g2PaginatorWriteChrome` (`G2_Page_Common.h:70`) drops "Next" on middle pages —
   **module-list page 3 (notif, llm, maps) is unreachable from the lens today.**
   Fix (13→14 rows, or restructure) and re-verify the 253 B single-fragment ceiling.
2. **Shared-core extraction (~150–200 lines) — required, not optional.** The helpers G2
   needs (`isEditableEntry`, `isSettingVisible`, the enum-options parser trio,
   `enumOptionIndexForCurrent`, `getSettingCurrentValue`) are file-static inside
   `OLED_SettingsEditor.cpp`, whose whole TU is gated `#if ENABLE_OLED_DISPLAY` and does
   not exist on the XIAO build where G2 still runs. Move them to a display-independent TU
   (e.g. `System_SettingsEditorCore.h/.cpp`); rewire OLED to call the shared copies.
   Per the board-gate rule: a green FeatherS3 build does not prove the XIAO path.
3. **Entry resolver + bool flip.** `entryIdx = gEntryPage * SET_VISIBLE_ENTRY_ROWS + (idx - 1)`
   (mirror `moduleIdxFromTap`; do **not** reuse `gPageStartIdx` — stale at the entries
   level). Replace the no-op with a type dispatch; BOOL = submit `<cmd> <1-current>` and
   redraw via gen-guarded Redraw.
4. **Pick-list level** (`SET_LEVEL_EDIT`) for entries with non-bitmask `options`:
   `[X]`-marked rows, existing paginator, commit the option's VALUE token.
5. **Keyboard flows** for STRING/INT (and FLOAT/U8/U16/U32 if scope allows) via
   `g2BeginTextEntry` with pre-fill, plus all pre-flight guards (below).
6. **Result surfacing:** completion callback classifies success **only** by
   `strncmp(result, "OK", 2) == 0`; error banners via the Network-style gen-guarded
   Redraw pattern; `findCommand()` pre-check per entry renders command-less rows inert.
7. **Optional polish:** use the currently-ignored `SettingEntry.group` field as a
   sub-level inside large modules (debug's 156+ flags have X-macro family groups) —
   group list → entries. No surface uses `group` today; metadata is already populated.
8. **Rename + comment hygiene:** toggle row `PRETTY` → `INTERACTIVE`; rewrite the stale
   read-only contract comment (`G2_Page_Settings.cpp:37`), the phantom "+N more (web UI)"
   trailer comments (lines 14, 79–83), and the wrong "both nav rows fit" claim (179–182).

## Hard rules distilled from the study

- **False-success hole:** `"Unknown command: …"` returns ok=true with no `Error` prefix
  and no `OK:` stamp (`System_Utils.cpp:4619–4643`). Negative "Error"-prefix classifiers
  report fake success. Positive-OK test + `findCommand` pre-check are both mandatory.
- **Debug rows must commit via `cmdKey`.** A direct `handleSettingCommand` write persists
  via `writeSettingsJson` only — the debug module persists to `debug.json`, so the edit
  would survive in RAM and vanish on reboot. The `cmdKey ?: jsonKey` rule routes debug
  rows to the generated `debug*` commands automatically; never shortcut it.
- **Bools:** the write core accepts only `1`/`true` as true — `"on"` silently writes
  false. Always emit literal `0`/`1`.
- **Strings:** empty args = "show current value", so strings cannot be cleared via this
  path; refuse empty keyboard commits (OLED does). Refuse values >32 chars (keyboard
  pre-fill silently truncates; commit would corrupt) and values containing `"` — banner
  "edit on web", per the Files-rename guards (`G2_Page_Files.cpp:859–869`).
- **Secrets stay non-editable** (`isSecret` exclusion). Extra reason:
  `g2SubmitHijackCommand` debug-logs the first 40 chars of every command line — a secret
  set would leak into logs.
- **readOnly is editor-side only** — `handleSettingCommand` has no readOnly guard; the
  shared editability predicate is the sole protection.
- **Re-assert `g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS)`** after every keyboard
  commit/cancel — keyboard teardown leaves hijackPage at TEXT_VIEW.
- **Never touch `gDeferWrites` from G2** — it is a global shared with every surface;
  accept one flash write per commit.
- **`bitmask:` options are not enums** (sensorLogMask; systemLogFlags has a ~5 KB options
  string that must never go near a 253 B list fragment). Exclude in v1.
- **No inline mutation on the tap path** — commits go through `g2SubmitHijackCommand`
  only; treat a false return as a hard no-op with a "Busy" banner.
- Auth rides the paired user: admin-gated setting commands deny non-admin pairers with a
  displayable `Error:` string; optionally gray rows via `commandRequiresAdmin` pre-check.

## Open decisions

- **Type scope:** OLED edits only INT/BOOL/STRING; `handleSettingCommand` parses all 7
  types, so G2 could also edit FLOAT/U8/U16/U32 (most mesh/power/thermal knobs). Decide
  whether to parametrize the shared predicate or keep parity with OLED.
- **Group sub-level (increment 7):** ship in v1 or defer.
- **Stale keyboard session hazard:** the 60 s hijack safety timeout never clears
  `gTE.active`, so a stale keyboard can hijack taps in the next session — pre-existing
  bug worth fixing alongside (or explicitly deferring).
- Related but independent: the scoped SETTING_CHANGED subject work (`moduleDisplayName()`
  et al.) is **not in the repo** — zero hits in tree and all git history. If wanted,
  it must be re-implemented; do not plan around those helpers existing.

---

## Note: multi-agent fan-out execution strategy

*(Requested as a note only — for the session that executes this plan, resource cost not a
consideration. Scale numbers are deliberately maximalist; shrink at will.)*

The task has one **serialization spine** — `G2_Page_Settings.cpp` is a single hot file,
and hardware validation is human-gated — so raw parallelism goes into everything *around*
that spine: verification, audit, design, and review. Suggested shape:

**Phase A — Ground-truth re-verification (fan-out, ~10 agents).** The executing session
starts fresh; do not trust this document's line numbers or even its claims. One agent per
load-bearing finding, each prompted to *refute* it: the pagination bug (count registered
modules against current BuildConfig, walk the paginator math), the tap no-op insertion
point, the `ENABLE_OLED_DISPLAY` TU gate, the Unknown-command false-success path, the
debug-persistence split, the Redraw gen-guard being enforced, the keyboard truncation
behavior, bool parse, empty-args semantics, secret logging. Anything refuted reshapes the
plan before code is written.

**Phase B — Design judge panel (3 designs × 3 judges).** Three agents independently
design the editor state machine (levels, dispatch, row-index mapping — flat extra level
vs Files-style action table vs CameraSettings-style declarative table) and the shared-core
API surface. Three judge agents score for index-drift resistance (the dominant historical
bug source per precedent pages), stack discipline, and minimal diff to the OLED editor.
Synthesize the winner; graft runner-up ideas.

**Phase C — Implementation lanes (worktree isolation, 3 lanes).** Freeze the shared-core
header contract first so lanes proceed concurrently against the agreed interface:
- Lane 1: `System_SettingsEditorCore` extraction + OLED rewire (touches
  `OLED_SettingsEditor.cpp` only).
- Lane 2: pagination fix + stale-comment sweep (`G2_Page_Settings.cpp` chrome +
  `G2_Page_Common.h` — small, merges first).
- Lane 3: the editor itself in `G2_Page_Settings.cpp` (the serialized spine — exactly one
  agent at a time; increments 3→4→5→6 sequential within the lane).
Merge order 2 → 1 → 3. Do not parallelize edits inside the spine file.

**Phase D — Entry-coverage audit fleet (embarrassingly parallel, ~23 agents).** One agent
per registered settings module, auditing **every entry**: does `cmdKey ?: jsonKey` resolve
to a registered command (closes the false-success hole per-row); type + options string
well-formed; string entries whose plausible values exceed 32 chars or contain untypeable
characters; secret/readOnly/bitmask flags. Output: a per-module include/exclude/refuse
table the editor dispatch consumes, plus a list of registry rows with missing commands
(real firmware bugs regardless of this feature). This is the highest-value use of
unlimited agents — ~380 entries no single context should eyeball.

**Phase E — Adversarial verification panels (~9 risks × 3–5 skeptics).** After the spine
lands, each hard rule above gets its own refute-by-default panel: agents attempt to
construct a concrete input/state that violates it (e.g. "find a tap sequence that commits
through a negative-prefix classifier", "find a debug row whose edit skips cmdKey"). A
separate OLED-equivalence panel proves the extraction changed no OLED behavior
(pre/post predicate outputs over the full registry, enum parser golden cases). A build
matrix agent force-verifies both board paths (FeatherS3 build + XIAO compile of the new
shared TU) per the board-gate rule, remembering the cert-bundle race
(`ninja -j1 x509_crt_bundle.S`) on clean builds.

**Phase F — Review loop-until-dry.** Multi-lens review fleet over the final diff
(correctness, stack/heap on the tap path, BLE fragment budgets, auth/identity, index
drift), findings adversarially verified, repeat with fresh finders until two consecutive
rounds surface nothing new. Then stop: **hardware validation and commit remain
human-gated** — per project rules, no commits until the user HW-tests, and the version
bump is patch-level when they approve.
