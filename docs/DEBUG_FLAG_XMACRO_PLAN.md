# Debug-Flag / Tag Vocabulary Unification — Implementation Plan

**Date:** 2026-07-20 · **Status:** plan only, nothing implemented
**Companion:** [LOGGING_TAG_AUDIT.md](LOGGING_TAG_AUDIT.md) (the findings this plan fixes)
**Method:** 6 parallel consumer-layer mappings → 1 design pass → 4 adversarial reviews
(compilation / missed-consumers / behavioural-equivalence / staging). All four returned
**sound-with-fixes**; 7 blockers were found and are folded in below. Every blocker was
re-verified by hand against current code before being accepted.

---

## 1. The problem, stated precisely

One fact — *"subsystem X is bit N, tags as `X`, persists as `debugX`, is toggled by command
`debugx`, shows label L, colours C"* — is currently hand-written in **nine** places:

| # | Layer | Location | Size |
|---|---|---|---|
| L1 | Flag constants | System_Debug.h:128-329 | 116 `#define`s |
| L2 | Macro-embedded tag text | System_Debug.h:600-891 | ~114 macros |
| L3 | Writer-side names | System_Debug.cpp:2112-2232 | ~106 if-entries |
| L4 | Settings fields | System_Settings.h:475+ | 157 bools |
| L5 | Settings→flag map | System_Settings.cpp:712-826 | 106 `DBG_MAP` rows |
| L6 | Parent aggregation | System_Settings.cpp:838-947 | 11 `syncXParent` helpers |
| L7 | Settings registry | System_Settings.cpp:1710-1926 | 160 rows |
| L8 | CLI handlers + table | System_Debug.cpp:1083-1978, 3007-3184 | 156 handlers, 170 rows |
| L9 | Web checkboxes + colours | WebPage_Logging.h:276-461, 1500-1548 | 45 boxes, ~60 colours |

`157 = 116 flags + 40 bitless subs + 1 duplicate (debugEspNow)`.

Nothing enforces agreement, so they have drifted — that drift *is* the audit's findings list.

**Goal:** one X-macro table generating L1/L3/L5/L6/L7/L8/L9, with the invariants held by
`static_assert` so drift becomes a build error. Copy the in-repo SYSEVT idiom
(System_Events.h:286-293, System_Events.cpp:127-144) rather than inventing a pattern.

---

## 2. The one fact the whole design rests on

**An X-macro cannot emit a `#define`.** So L1's 116 `#define DEBUG_X` must become
`inline constexpr DebugFlagMask DEBUG_X`. That is only safe if no flag name is ever used in a
preprocessor context. Verified: `grep -E '^\s*#\s*(if|ifdef|elif).*DEBUG_[A-Z]'` returns only
`DEBUG_MEM_SUMMARY` (an unrelated build toggle); zero `defined(DEBUG_`; zero `case DEBUG_`
(impossible anyway — `DebugFlagMask` is a 256-bit struct, not integral).

Two consequences that must be honoured:

- **`inline constexpr`, not bare `constexpr`.** At namespace scope in a header, bare `constexpr`
  has internal linkage → one 32-byte `.rodata` copy per TU per used flag, across ~143 TUs. A
  reviewer measured this with the project's own compiler/flags: ~−160 B in a 5-flag synthetic TU,
  roughly 10-19 KB of flash tree-wide. Applies to the mask constants *and* every `kDbg*` column
  array. **The app partition is already at 5% free — this is not optional.**
- **Indexing.** `DebugFlagMask` has no `operator<` and no integral conversion
  (System_Debug.h:51-102), so it can never subscript an array. Every row gets a dense `uint8_t`
  index (`DBG_##name`) alongside its mask (`DEBUG_##name`); **all parallel tables are indexed by
  the index enum, never the mask.**

Toolchain: 143 of 148 hardwareone `.cpp` files compile at `-std=gnu++2b` (the other 5 are
CMake-feature-gated out). Relaxed `constexpr` is available everywhere the table lands.
*(An earlier draft claimed "203/203" — wrong count, conclusion unaffected.)*

---

## 3. Table shape

Three tables in a new `System_DebugFlags.h`, included from `System_Debug.h` **after**
`DEBUG_BIT` is defined. It is an include-order-dependent fragment, not a standalone header —
open it with `#ifndef DEBUG_BIT / #error "include System_Debug.h" / #endif` so a stray direct
include fails loudly instead of emitting 116 "does not name a type" errors.

```c
// F(sym, base, width, "Label", "#rrggbb")   — banks mirror the byte-per-family map
#define DBG_BANK_LIST(F) \
  F(CORE, 0, 24, "Core", "#569cd6") \
  F(MEMORY, 24, 8, "Memory", "#b5cea8") \
  ... \
  F(CONTROL, 248, 8, "Control", "#808080")   /* bit 255 = DEBUG_ALWAYS */

// X(SUFFIX, bit, bank, parent, "TAG", settingsField, cmdIdent,
//   "group", "jsonKey", "UI Label", agg, dflt, "help", "#colorOverride")
//   parent : bare suffix, or NONE
//   agg    : DBG_AGG_MASTER (subs do NOT light parent) | DBG_AGG_ROLLUP | DBG_AGG_NONE
#define DBG_FLAG_LIST(X) \
  X(GPS, 128, DBG_BANK_GPS, NONE, "GPS", debugGps, debuggps, \
    "gps", "enabled", "All GPS", DBG_AGG_MASTER, 0, "Debug GPS (parent).", "") \
  X(GPS_LIFECYCLE, 129, DBG_BANK_GPS, GPS, "GPS_LIFE", debugGpsLifecycle, debuggpslifecycle, \
    "gps", "lifecycle", "Lifecycle", DBG_AGG_NONE, 0, "GPS init/connect/recovery.", "") \
  /* ESP-NOW's parent is *_CORE, not *_ESPNOW — a generator that strips the last
     _SUFFIX to find the parent mis-parents this family, DEBUG_MIC_* and DEBUG_AUTO_*. */ \
  X(ESPNOW_CORE, 32, DBG_BANK_ESPNOW, NONE, "ESP-NOW", debugEspNowCore, debugespnowcore, \
    "esp-now", "core", "Core", DBG_AGG_MASTER, 1, "ESP-NOW core.", "") \
  /* 113 more */
```

**The group/jsonKey/label/cmdIdent columns are verbatim transcriptions of existing registry
rows — never computed.** They do not follow from the symbol (`NTP` ↔ `debugDateTime` ↔
`debugdatetime` ↔ group `datetime`), and computing them silently renames persisted JSON keys.

**Rule: zero `#if` inside any table row.** Feature gating stays at the emit site. This is what
makes one green build meaningful. Accept the honest cost: on a build with
`ENABLE_ONDEVICE_LLM=0` or `ENABLE_G2_GLASSES=0`, the 13 currently-gated registry rows
(System_Settings.cpp:1869-1876, 1891-1904) become unconditional → **+13 debug.json keys, +13
schema entries, +13 checkboxes, +13 inert commands.** Budget the serialized debug.json size
against its write buffer before Stage C2. *(The earlier draft's "key-neutral" claim was wrong.)*

### The 40 bitless subs — and why `DebugSubFlags` must NOT be deleted

40 settings (`http*`, `wifi*`, `auth*`, `storage*`, `system*`, `users*`, `cli*`, `perf*`,
`sse*`, `cmdflow*`, `ntp*`) have **no bit**; they live in `DebugSubFlags` (System_Debug.h:336-405)
and only OR up into a parent. They get their own 8-column table.

The original design deleted `DebugSubFlags` as "a redundant copy of gSettings". **Verified
wrong** — System_Debug.cpp:2570:

```c
gDebugSubFlags.httpRequests = (v!=0);                              // unconditional
if (!modeTemp) setDebugSetting(gSettings.debugHttpRequests, ...);  // persistent only
```

`DebugSubFlags` is the **runtime layer**; `gSettings` is the **persistent layer**. They diverge
the moment anyone types `temp` — which is the entire purpose of that argument, advertised in all
156 command usage strings. Deleting it turns `temp`/`runtime` into a silent no-op for 40 commands.

**Corrected approach: generate it, don't delete it.** Emit
`inline bool gDbgSubRuntime[DBG_SUBBOOL_COUNT];` from the table, replacing the 45 hand-written
members with a table-indexed array (same line-count win, semantics preserved).

**Do not promote the 40 to real bits in this project.** Bits are available (57 free), but no
emitter gates on them — every one of the ~200 `DEBUG_HTTPF`-style sites tests only the parent.
Promoting mints 40 bits nothing reads and 40 checkboxes that do nothing.

### Colour: bank default + per-flag override

Bank-keyed colour alone is a **regression**: the CORE bank holds 16 unrelated subsystems (AUTH,
HTTP, SSE, CLI, WIFI, NTP, DISPLAY, NOTIFICATIONS…) that today have distinct colours and would
collapse to one. Hence the 14th column: per-flag override, defaulting to the bank. Sensor and
ESP-NOW banks inherit correctly (the real win — 76 tags currently hash to arbitrary hues); the
16 CORE subsystems keep their existing colours, transcribed verbatim.

### Compile-time invariants (the payoff)

`dbgBitsUnique` · `dbgBitsInBank` · `dbgBanksDisjoint` · `dbgParentsWellFormed` ·
`dbgTagsUnique` · `dbgAlwaysReserved` · `DBG_FLAG_COUNT == 116` · row-count asserts on every
hand-written block. Verified compilable: a `constexpr const char* kDbgTag[116]` with an O(n²)
uniqueness check inside a `static_assert` builds fine at gnu++2b, far under the step limit.

Two corrections to the guards as originally specified:
- `dbgGroupKeyUnique()` must skip hand-appended rows or null-guard `dbgStrEq` — the `logLevel`
  row has a **null** group pointer (System_Settings.cpp:1925); a constexpr `strcmp` over it is a
  compile error, not a passing check.
- The standing CI grep `^\s*#\s*(if|ifdef|elif).*DEBUG_[A-Z]` is **red on a clean tree today** —
  no word boundary, so it matches `#ifndef DEBUG_SYSTEM_H` (System_Debug.h:33). Fix the regex and
  verify each standing grep returns its claimed result *before* writing it into the plan.

---

## 4. Staging — restructured: value first, table second

The original plan sequenced every user-visible fix *behind* ~1.5 days of 116-row transcription.
The staging reviewer's objection is correct and reshapes the plan: **the three highest-value
fixes have zero dependency on the table.** Phase A ships all of them and can be evaluated on
hardware before committing to any transcription.

### Phase A — table-free fixes (~2.5 days, ships every user-visible win)

| Stage | What | Files | Proof |
|---|---|---|---|
| **A0** | **Prerequisites.** Delete duplicate `debugEspNow` field/row (keep `debugespnow` as a hand-written alias). Fix SR: point `DEBUG_SRF` at `DEBUG_SR`. **Move `srSyncDebugLevel()`'s declaration out of `#if ENABLE_ESP_SR`** (or add an `#else` stub) — see below. | System_Settings.cpp, System_ESPSR.h/.cpp | CLI per-change |
| **A1** | **Add the `debugflags` command** (~10 lines): print the 4-word mask + set flags by name. | System_Debug.cpp | It is the instrument every later stage is graded on |
| **A2** | **Precedence fix** — reorder `getDebugCategoryName` sub-before-parent, G2-style. Un-hides ~47 structurally-dead sub entries. | System_Debug.cpp (~47 lines reordered) | `debugi2c 1; debugi2cautostart 1` → lines file under `I2C_AUTOSTART` |
| **A3** | **`DEBUG_ALWAYS`** — replace `(flag)==0xFFFFFFFF` with a real bit-255 test; repoint 46 ERROR/WARN macros at `DEBUG_ALWAYS \| <real flag>`. Stops ~700 sites filing under `AUTH`. | System_Debug.h:611,757-809; System_ESPSR.cpp:47-50; **System_LLM.cpp:2366-2375**; Bluetooth.cpp:64 | Trigger `ERROR_I2CF`; category must read `I2C` |
| **A4** | **12 orphan `DBG_MAP` rows** — the 8 sensor parents + ANO family are wiped by `setDebugFlags(0)`; UI shows ON, bit is clear. | System_Settings.cpp | `debugthermal 1` → reboot → still set |

**A3 hazard (verified):** System_LLM.cpp:2366/2370/2375 pass literal `0xFFFFFFFF` into
`DEBUGF_QUEUE`. `dbgHasBit(0xFFFFFFFF, 255)` is **false**, so these three unconditional lines
silently become conditional. Repoint or delete them. The standing grep must be unscoped —
a filename filter is what let these hide.

**A0 hazard (verified — and the comment lies):** System_ESPSR.h declares `srSyncDebugLevel()`
*inside* `#if ENABLE_ESP_SR`, with a comment claiming "Safe to call when SR is disabled — the
inline `#else` above keeps it out of the build." The `#else` branch contains **only** a
`registerESPSRHandlers` stub. Any unconditional caller fails to compile on an SR-off board.

**Decision point.** Flash Phase A, live with it. Every audit-visible bug is now fixed. Phase B
buys *permanence*, not new behaviour — decide with hardware evidence in hand.

### Phase B — the table (~1.5 days, compile-only)

**B1.** Create `System_DebugFlags.h`; generate the 116 `inline constexpr` masks + index enums +
column arrays; delete System_Debug.h:107-331. **Zero behaviour change** — the `static_assert`s
*are* the proof, plus `debugflags` output byte-identical against the same saved debug.json.

Generate the rows **with a script** parsing System_Debug.h and System_Settings.cpp, then
hand-review. Do not hand-type 116 rows.

**B2.** Repoint `getDebugCategoryName` at the table (~8 lines replacing the if-chain). Deletes
the `#if ENABLE_ONDEVICE_LLM` at :2211-2219.

> **Acceptance criterion, corrected:** "byte-identical capture, zero exceptions." The original
> criterion ("identical except BT/SR lines changing UNKNOWN→tags") is **unsatisfiable**: BT lines
> go through `BLE_DEBUGF` → `broadcastOutput`, bypassing the queue entirely (Bluetooth.cpp:64-71),
> and `getDebugCategoryName` is only reached with `msg->category != 0` (System_Debug.cpp:239). A
> tester following it would conclude the stage FAILED. Also note G2 is **hand-hoisted out of bit
> order** today (System_Debug.cpp:2229-2230), so emitting in bit order is not byte-identical for
> `G2_DUMP` — either emit in current chain order, or name the delta up front.

### Phase C — generate the consumers (~6 days; **C1-C4 are one commit-or-revert unit**)

**C1** L5+L6: generated `DBG_MAP` + `dbgRecomputeParents()`; `DebugSubFlags` **generated, not
deleted**. Recompute must be **scoped to the touched row** — a global recompute on every toggle
clobbers temp-set parent bits across unrelated families (today each `syncXParent` is called only
from its own family, so cross-family clobber is structurally impossible; don't introduce it).
Keep the full sweep for the `applySettings` path only.

**C2** L7 registry. **Mandatory scripted diff** of the full sorted tuple
`(group, jsonKey, label, cmdKey, intDefault)` — not just `(group, jsonKey)`. A transposed label
compiles, passes every `static_assert`, and is silent at runtime. Also enumerate the **three**
non-flag rows and the **fourth** default-value source: `kBootDefaultDebugFlags`
(System_Debug.cpp:37-46), a 29-flag static-init union the design never counted — and which is a
live consumer of three flags the design elsewhere calls deletion candidates.

**C3** L8 CLI. Hand-written rows are **14**, not 11 (`memorysampleintervalsec`, `loglevel`,
`webconsole` are settings-backed but not flags). Assert against an explicit
`kDbgHandCmdRows = 14` so adding a row forces editing the constant. Preserve `dbgApplyHook` —
`debughttps` must still drive ESP-IDF TLS verbosity, the case a uniform generator silently drops.

**C4** L9 web. `GET /api/debug/flags`; render the pane and colours from it.
**Do not delete `kSystemLogFlagsBitmaskOptions`** — verified live at System_Debug.cpp:3491 as the
`options` of the `systemLogFlags` `SettingEntry`, rendered as a schema-driven BitmaskField grid on
the **Settings** page (a second working UI the design missed). Either regenerate it from the table
(preferred — it gains the 71 missing flags) or explicitly convert the field and say so.
Measure the `/api/settings/schema` response against its 65536-byte buffer: 45→116 flags adds
~4-6 KB atop ~330 entries. That endpoint has already returned HTTP 500 on overflow once.

### Phase D — optional

**D1** (~0.5 day) `static_assert(dbgStrEq(kDbgTag[DBG_X], "TAG"))` ×114 → macro-vs-table drift
becomes a build error. **Cheap, high value — recommended even if D2 never happens.**

**D2** (~2 days + broad HW pass) Delete embedded `[LEVEL][TAG]` from 114 macros; render in the
sink layer. Resolves all 8 name disagreements permanently. **Blast radius is larger than
originally stated** — two in-firmware consumers parse the level out of `msg->text`:
help-mode gating (System_Debug.cpp:177) and the errors.log tee (System_Debug.cpp:266). Both must
move to a level field on `DebugMessage`, which is scope D2 does not currently carry.

---

## 5. Verification matrix

Feature gates are **hand-edited literals in CMakeLists.txt**, not board-derived, so board
coverage alone does not exercise them. Required cells:

| # | Config | Catches |
|---|---|---|
| 1 | feathers3 default | baseline (builds green as of 2026-07-20) |
| 2 | xiao_s3 | second S3 board |
| 3 | feather_esp32_v2 | non-S3 target |
| 4 | `ENABLE_ONDEVICE_LLM=0` | LLM rows/gates |
| 5 | `ENABLE_ESP_SR=1` | the A0 `srSyncDebugLevel` fix |
| 6 | `ENABLE_BLUETOOTH=0` / `ENABLE_G2_GLASSES=0` | BT+G2 rows |
| 7 | `HW_CFG_ENABLE_MQTT=1` | **MQTT is compiled OUT by default** (CMakeLists.txt:15) |
| 8 | `HW_CFG_INPUT_DEVICE_TYPE=2` | ANO encoder family |

Cells 7-8 were missing from the original matrix and cover families the plan's own acceptance
tests name. **Add a pre-flight step to every stage:** diff `ls *.cpp` against the compiled set in
`build/compile_commands.json` and list which flag families are dark — 143 of 148 files compile in
the default config, and a "green build" says nothing about the other 5.

**Acceptance-procedure hazard, all phases:** `log start` **without** a `flags=` token overwrites
`gDebugFlags` from the persisted mask (System_Debug.cpp:2455-2461), wiping the flags a test just
set. Every capture-based proof must set the mask through `log start … flags=<mask> tags=1`
itself, or set per-flag commands *after* `log start`. Quote all paths (unquoted paths are
rejected at :2429).

---

## 6. Risk register

| Risk | Mitigation |
|---|---|
| 116-row transcription error | Script-generate rows, hand-review; 8 `static_assert`s make structural errors build errors |
| Silent registry-key rename → settings lost | C2's 5-column scripted diff; keys are transcribed, never computed |
| Flash bloat (5% partition headroom) | `inline constexpr` everywhere; `idf.py size` .rodata delta between B1 and A4 builds |
| Point of no return | **Stages A0-B2 are individually revertible. C1-C4 are one unit. Tag the B2 build as the rollback target.** |
| Feature-gated code hides breaks | 8-cell matrix + compiled-file-set pre-flight |
| Behaviour change mistaken for regression | A2/A3 change ~40 categories and ~700 lines' tags *deliberately* — changelog entries required |

## 7. Bottom line

Phase A is **~2.5 days, ships every user-visible fix in the audit, and carries zero transcription
risk.** Phase B+C is a further ~7.5 days that buys permanence: drift becomes a build error, and
the ~62 missing checkboxes appear. D1 is a cheap 0.5-day lock worth taking regardless.

Recommendation: **execute Phase A, flash, evaluate — then decide on B/C with evidence.**
