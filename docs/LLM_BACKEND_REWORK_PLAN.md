# LLM Backend Rework — Plan v2 (post-adversarial)

Goal: make "which model answers my question" a first-class, pluggable choice
across web / OLED / G2 / BLE app, with the on-device PSRAM engine and the CM5
co-processor as two backends behind one registry — and make a third backend
(someone else's HTTP endpoint, a second co-processor) a self-contained file
rather than a shotgun edit across four surfaces.

Status: **plan only, no code written.** v1 written 2026-08-17, attacked by six
independent skeptics + refuters the same day. 15 high-severity findings; 6 were
adversarially verified and **none were refuted**. v2 below is the corrected plan.

---

## 0. Corrections to v1 (things v1 asserted that are false)

**C1 — the ANO button map was wrong.** v1 said IN→A, LEFT→B, UP→X, DOWN→Y.
Actual mapping is **IN→A, LEFT→B, DOWN→X, UP→Y**, plus RIGHT→SELECT and a
RIGHT+IN chord→START ([HAL_Input.cpp:86-93](../components/hardwareone/HAL_Input.cpp:86)).
v1 copied a stale comment block instead of reading the mapping array.

**C2 — the "RIGHT-tap flips the wheel axis" bug does not exist.** It was
deliberately removed from the driver; the code comment records why
([i2csensor_ano_encoder.cpp:560-570](../components/hardwareone/i2csensor_ano_encoder.cpp:560)).
`currentAxis` is now written **only** by `anoEncoderResetAxisForMode()`, whose
sole caller always passes `ANO_AXIS_VERTICAL`
([OLED_Utils.cpp:3338](../components/hardwareone/OLED_Utils.cpp:3338)). The
wheel therefore never emits `gNavEvents.left/right` on any ANO build. v1's
claim that three shipped pickers carry a live bug came from the **stale header
comment** at [i2csensor_ano_encoder.h:26-27](../components/hardwareone/i2csensor_ano_encoder.h:26).

**C3 — v1's proposed fix for the non-existent bug would have caused a real
regression.** On ANO, LEFT *is* the B/back button, and one LEFT press raises
`gNavEvents.left` **and** `INPUT_BUTTON_B` in the same frame
([OLED_Utils.cpp:6371-6376](../components/hardwareone/OLED_Utils.cpp:6371),
[:6566](../components/hardwareone/OLED_Utils.cpp:6566)). Every picker calls
`oledScrollHandleNav()` first and returns early on true
([OLED_Mode_LLM.cpp:769](../components/hardwareone/OLED_Mode_LLM.cpp:769),
[:783](../components/hardwareone/OLED_Mode_LLM.cpp:783),
[:807](../components/hardwareone/OLED_Mode_LLM.cpp:807)), so passing
`leftRightNav=true` would consume LEFT as a scroll and the B test below would
never run. The pickers are modal (`return true` unconditionally), so the global
back handler is blocked too — **the user would be trapped in the picker.**

**Net:** the ANO axis work is struck from the plan entirely. The only ANO item
that remains is fixing the stale header comment so the next reader isn't misled.

---

## 1. S0 — a live bug that blocked everything ✅ FIXED 2026-08-17

**All 17 LLM setting commands write the wrong setting field, off by exactly two.**

`LLM_SETTING_CMD(funcName, idx)` expands to
`handleSettingCommand(&llmSettingEntries[idx], a)` — a raw **index** into the
table ([System_LLM.cpp:3017-3020](../components/hardwareone/System_LLM.cpp:3017)).
Two rows (`llmEnabled`, `autoStart`) were later inserted at the **top** of the
table ([System_LLM.cpp:2983-2984](../components/hardwareone/System_LLM.cpp:2983))
without renumbering the macros. Verified mechanically:

```
cmd_llm_temperature   idx=0  -> llmEnabled      cmd_llm_defaultmodel  idx=8  -> repWindow
cmd_llm_topp          idx=1  -> autoStart       cmd_llm_minp          idx=9  -> maxContext
cmd_llm_maxtokens     idx=2  -> temperature     cmd_llm_kvprec        idx=10 -> defaultModel
cmd_llm_sentencelimit idx=3  -> topP            cmd_llm_autostart     idx=11 -> minP
cmd_llm_hardcap       idx=4  -> maxTokens       cmd_llm_norepeatngram idx=12 -> kvPrecision
cmd_llm_reppenalty    idx=5  -> sentenceLimit   … 17 of 17 mismatched
cmd_llm_repwindow     idx=6  -> hardCap         index 17 (profile) unreachable
```

`llmdefaultmodel my.bin` writes a **string into an INT field**. The table even
carries an "APPEND-ONLY below this line" warning
([System_LLM.cpp:2994-2999](../components/hardwareone/System_LLM.cpp:2994))
explaining this exact failure mode — the insert went in above it.

**Currently harmless only because `ENABLE_ONDEVICE_LLM 0` compiles it all out.**
It goes live the instant this rework re-enables the surfaces layer, and it would
land on `llmDefaultModel` — the very setting §5/S4 depends on.

**S0 fix — APPLIED.** `LLM_SETTING_CMD` now takes a cmdKey literal and resolves
via a new file-local `llmSettingByCmdKey()`, mirroring `SETTING_EDITOR_CMD`
([System_Settings.cpp:3079](../components/hardwareone/System_Settings.cpp:3079)).
Searches this module's own 18-row table rather than `findSettingByCmdKey()`
(which walks every registered module) — smaller, and no dependency on
settings-module registration order. Renumbering was rejected: it resets the
clock but leaves the same trap for the next insert. Row order is now free.

Verified: all 17 handlers resolve to their own field, and every `CommandEntry`
name matches its handler's cmdKey. `llmenabled` is the one table row with no
`LLM_SETTING_CMD` — not a gap; it is served by the generic
`SETTING_EDITOR_CMD(cmd_set_llmenabled, "llmenabled")` at
[System_Settings.cpp:3137](../components/hardwareone/System_Settings.cpp:3137).

Compile-checked by temporarily setting `ENABLE_ONDEVICE_LLM 1` and running
`-fsyntax-only` on the TU with the xiao_s3 compile flags plus the esp-dsp
include path (which CMake only adds when the flag is on): **exit 0**, only three
pre-existing unused-variable warnings at :488/:2198/:2199. Flag restored to `0`.
**Not run on hardware** — the subsystem is still compiled out.

---

## 2. Build flags — one feature, N sources

v1 proposed three derived flags (`ENABLE_LLM_CHAT` / `_WEB` / `_OLED`). That was
wrong twice: unimplementable as written, and — more importantly — it invented a
new concept for something this codebase already has a documented convention for.

**The convention.** `System_BuildConfig.h` §5 is titled *"USER-FACING APPS —
Composed features shipped as products"*, and its stated rule is that each app
flag *"gates its own web page and (where applicable) OLED mode."* `ENABLE_GAMES`
([System_BuildConfig.h:347-356](../components/hardwareone/System_BuildConfig.h:347))
is exactly the shape wanted here: **one master switch, then flat literal
sub-flags choosing what goes in it, with a build-time guard rejecting illegal
combinations.** The LLM is a composed user-facing app and belongs in §5 — not,
as today, as a bare subsystem flag in §4.

**The model: the LLM is one feature; the onboard engine and the CM5 are two
_sources_ of answers, symmetric with each other.**

```c
// LLM assistant: model registry, conversation layer, CLI commands, web page,
// OLED mode, G2 lens page.
//   ENABLE_LLM_BACKEND is the master switch. Then enable one or more SOURCES —
//   a source is a place answers can come from. Master off ⇒ sources ignored.
//   Master on with no source ⇒ every picker would be empty, so a guard rejects it.
//   Keep all three a plain literal — CMakeLists regex-greps these exact lines.
#define ENABLE_LLM_BACKEND          0
#define ENABLE_LLM_SOURCE_ONBOARD   0   // on-device PSRAM engine (S3 + PSRAM, 1-4 MB)
#define ENABLE_LLM_SOURCE_CM5       0   // CM5 / Pi 5 co-processor over the UART link
```

**Why flat literals rather than nesting the sources inside
`#if ENABLE_LLM_BACKEND`:** CMake, not the preprocessor, decides which files
compile. [CMakeLists.txt:72-75](../components/hardwareone/CMakeLists.txt:72)
regex-greps this header for a plain integer and takes the **first match**;
[:465-481](../components/hardwareone/CMakeLists.txt:465) is the only thing
putting the LLM `.cpp` files into `hardwareone_srcs`, and a parse failure is
fail-safe to **0** — it silently drops every LLM source with a `message(STATUS)`
and no error. A source flag nested inside an `#if` would still be grepped and
read as its literal, compiling the engine with the master off. The nesting is
therefore expressed as **guards, not `#if`-wrapping** — the same way the games
flags do it:

```c
#if ENABLE_LLM_BACKEND && !ENABLE_LLM_SOURCE_ONBOARD && !ENABLE_LLM_SOURCE_CM5
  #error "ENABLE_LLM_BACKEND needs at least one source (ONBOARD and/or CM5)."
#endif
#if !ENABLE_LLM_BACKEND && (ENABLE_LLM_SOURCE_ONBOARD || ENABLE_LLM_SOURCE_CM5)
  #error "An LLM source is enabled but ENABLE_LLM_BACKEND is 0."
#endif
#if ENABLE_LLM_SOURCE_CM5 && !defined(UART_LINK_PORT)
  #error "ENABLE_LLM_SOURCE_CM5 requires the UART host link."
#endif
```

**Surfaces need no flags of their own.** Per §5's own convention they follow the
master plus the capability flag that already exists — `ENABLE_LLM_BACKEND &&
ENABLE_OLED_DISPLAY` for the OLED mode, `ENABLE_LLM_BACKEND &&
ENABLE_HTTP_SERVER` for the web page, `ENABLE_LLM_BACKEND && ENABLE_G2_GLASSES`
for the lens page. On the carrier XIAO the OLED mode drops for free because
`ENABLE_OLED_DISPLAY` is already 0 there. That is v1's `_OLED` flag deleted at
no cost. The `_WEB` flag is also deleted: `CUSTOM_ENABLE_WEB_*` is an existing
10-entry convention ([:86-95](../components/hardwareone/System_BuildConfig.h:86)),
so if the ~60 KB page ever needs dropping independently it is one row in a table
that already exists, not a new concept.

**Naming.** `ENABLE_ONDEVICE_LLM` → `ENABLE_LLM_SOURCE_ONBOARD` so the three
read as one family and "onboard" matches how the feature is actually described.
Mechanical: 32 files by `sed`, plus the CMake grep line at
[CMakeLists.txt:72](../components/hardwareone/CMakeLists.txt:72). Worth doing —
the incoherent naming is part of what made the old flag mean two things at once.

**Consequence: `System_LLM.cpp` must be split.** It currently holds *both* the
engine and the 30-command surface, so no single flag can gate it correctly.
Engine (kernels/sampler/tokenizer/model loader + the esp-dsp `REQUIRES`) →
`SOURCE_ONBOARD`. Registry, commands, `llmListModels` → `BACKEND`.

**Runtime layer, unchanged in concept.** `gSettings.llmEnabled` already exists
as a master runtime enable but is read in only two places
([System_LLM.cpp:2667](../components/hardwareone/System_LLM.cpp:2667),
[HardwareOne.cpp:2137](../components/hardwareone/HardwareOne.cpp:2137)). It now
also filters the registry, so turning it off empties the pickers rather than
only refusing a load. A source appears in a picker iff **compiled in AND
runtime-enabled AND available** (CM5 additionally needs a fresh presence lease).

---

## 3. Header/gate surgery — materially larger than v1 said

v1 said the split "requires moving `LLMStatus` / `LLMGenParams` / `LLMState`".
That is far short. `#if ENABLE_ONDEVICE_LLM` wraps **entire file bodies**:
[System_LLM.h:17-342](../components/hardwareone/System_LLM.h:17) (the whole
header), [System_LLMChat.h:34](../components/hardwareone/System_LLMChat.h:34),
and [OLED_Mode_LLM.cpp:29-895](../components/hardwareone/OLED_Mode_LLM.cpp:29).

Roughly **79 engine-API call sites across ~20 distinct symbols** (chat 15,
web 16, OLED 25, G2 23) must be re-pointed — not three struct definitions.
Several are engine *concepts* with no remote meaning and need ungated no-op
bodies, not relocation: `llmContextDegraded` (PSRAM ctx auto-fit),
`llmModelDescription` / `llmModelIcon` (read the loaded `.bin` info block), and
the whole `llmMenu*` family (reads the model's embedded MENU blob via
`System_LLM_Internal.h::gLLM`, so it cannot simply move).

Also under the gate and unmentioned in v1: **15 `gSettings.llm*` fields** plus
their initializers and module registration, and the feature registry's
`isLLMCompiled()` predicate — which would report `compiled:false` on a working
CM5-backed build.

**Consequence for staging:** v1's S1 gate ("build the never-built
`BACKEND=1, SOURCE_ONBOARD=0` combo") is **unreachable in S1's scope** — and
worse, it would pass *misleadingly*, because the OLED mode simply vanishes
rather than failing to build. S1 and S2 are merged below, and the gate is
restated as a behavioural one.

This is also why the §2 flag model is not just cosmetics: with one master flag
and two source flags, "does the LLM feature exist" and "is the onboard engine
one of its sources" become independent questions the build can actually answer.
The old single `ENABLE_ONDEVICE_LLM` conflated them, which is what forced every
surface to be engine-gated in the first place.

---

## 4. Registry — corrected

```c
enum class LlmBackendKind : uint8_t { Local = 0, Cm5 = 1 };

struct LlmModelDesc {           // 156 B/row as specced in v1 — NOT the ~40 B implied
  char           id[48];
  char           name[32];
  LlmBackendKind backend;
  char           path[64];      // drop for remote-only rows
  uint32_t       sizeKB;
  uint8_t        storage;
  bool           available;
};

size_t llmEnumerateModels(LlmModelDesc* out, size_t cap);
bool   llmResolveModelId(const char* id, LlmModelDesc* out);   // NEW — see below
```

**Every `LlmModelDesc` array must be an `EXT_RAM_BSS_ATTR static` owned by the
surface, never a local.** At 156 B/row an 8-row local is 1.2 KB on tasks with
7,680 B stacks. Precedent: [G2_Glasses.cpp:7338-7339](../components/hardwareone/G2_Glasses.cpp:7338).

**`llmResolveModelId()` is not optional.** v1 enumerated the four model
*listing* paths but never the *selection* paths, each of which resolves a bare
filename with its own hard-coded rules. An id like `local:model.bin` breaks all
of them. `llmResolveModelId` must be the single resolver, and S2 repoints every
call site — including `OLED_Mode_LLM.cpp:233`, the G2 `llmload %s` emission at
`G2_Glasses.cpp:7717`, `/api/llm/load`, and the boot autostart at
`HardwareOne.cpp:2139`. v1's risk 4 framed this as an autostart-only problem;
it is a four-surface problem.

*Pre-existing, neither created nor worsened by this plan:* a model filename
containing a space already breaks `llmload` — `CommandArgs` splits on
whitespace and both the G2 and autostart call sites emit unquoted.

---

## 5. Chat layer — v1's central design claim was wrong

v1 claimed the CM5 backend's own result buffer lets `chatReadTurn`'s pull-on-read
"drain it exactly as it drains the engine's result globals — one data path".
**False.** Only the byte copy is backend-neutral. `drainEngineLocked()`
([System_LLMChat.cpp:175-223](../components/hardwareone/System_LLMChat.cpp:175))
also reads three engine-**global control signals**:

| Line | Call | Resolves to |
|---|---|---|
| :180 | `llmGetSessionId()` | `gLLMSessionId`, bumped only inside `llmStartAsync` |
| :191/:195 | `llmGetResultLen/Chunk()` | `gLLMResultBuf` |
| :211 | `llmIsGenerationDone()` | `gLLMResultDone`, **initialised `true`** ([System_LLM.cpp:158](../components/hardwareone/System_LLM.cpp:158)) |

Every reader calls the drain (`chatGetTurnCount` :463, `chatGetTurnInfo` :471,
`chatReadTurn` :489, `chatBeginTurn` :317). And **above** all of it,
`chatBeginTurn:309` gates on `llmIsReady()` — which is
`gLLM.runState == READY`, true only when a **local** model is loaded.

So on the target configuration (carrier board, CM5 backend, no local model)
`chatBeginTurn` returns 0 and no CM5 turn is ever created. Patch only that gate
and the first poll runs the drain: either the session ids differ and the turn is
**silently dropped** at :180 (empty bubble, no error anywhere), or they collide
and the drain copies stale local bytes and instantly finalises at :211 because
`gLLMResultDone` is permanently true.

**Corrected design:** the drain must be backend-**dispatched**, not just the
byte read. Store the backend kind on the streaming turn; add
`llmBackendSessionId()`, `llmBackendIsDone()`, `llmBackendIsReady()` and
per-turn metrics to the registry; rewrite :180 / :191 / :195 / :211 / :212 and
the readiness gates at :309 and :354 through it. v1's "two bypass points" (§2.2)
was accurate *for prompt framing* — the plan simply never enumerated the read
path at all.

**Plus a wedge:** `sStreamingTurnSlot` is cleared in exactly one place. Locally
termination is unconditional; remotely it depends on a `cm5 llm end` line
surviving the UART. v1 added `llm_cancel` on the wire but **no firmware-side
deadline**, and `chatStop()` cannot cancel a remote turn. One lost line wedges
the whole chat layer for every surface. Fix: record last-push millis on the
streaming turn and finalise as `stopped` when it goes stale or the CM5 presence
lease drops.

**Also unaddressed by v1:** `llm_ask` carries no generation params, silently
voiding the shared clamped override contract that is chat's whole reason for
owning parameter resolution. And `chatRetryLast`'s suppress-token list is
meaningless remotely — retry needs a different remote semantic.

---

## 6. CM5 wire protocol — corrected

**What held up under attack:** the firmware→CM5 direction really is
length-delimited and newline-safe end to end (firmware writes `len_le(2)`
[System_UartLink.cpp:1204-1223](../components/hardwareone/System_UartLink.cpp:1204);
the daemon's reader puts the `in_frame` branch *before* the `0x0A` line branch,
`transport.py:210-233`; `parse_frame_body` slices by declared length). A 990 B
payload encodes to ~1002 B, under `_MAX_FRAME_WIRE = 1100`. Intrinsic
placement also held: `appendCommandToFeed`
([System_UartLink.cpp:965](../components/hardwareone/System_UartLink.cpp:965))
and `submitAndExecuteSync` (:1005) both sit *after* the callback branch, so a
push genuinely skips the command lock and the durable audit.

**What broke:**

1. **The push text cannot go through `CommandArgs`.** `uartProcessLine` trims
   the line and `CommandArgs` re-tokenizes it, destroying the inter-chunk glue
   that streamed deltas depend on (a delta's leading space is significant).
   Match the prefix on the raw `const char* line` and take the tail as a raw
   pointer offset — the `restRaw` pattern.
2. **CM5→firmware is line-framed, and that is the direction carrying prose.**
   v1 solved framing only in the direction that didn't need it. Define an
   escape applied by the daemon and undone by the intrinsic:
   `\n`→`\\n`, `\r`→`\\r`, `\\`→`\\\\`.
3. **The daemon's EVT entry point decodes strict ASCII** and drops the payload
   on failure with only a log line. One curly apostrophe from a phone keyboard
   silently loses the prompt with no error and no timeout. Give `llm_ask` its
   own branch decoding UTF-8 with `errors="replace"`, split with `maxsplit` so
   the prompt tail is taken raw — and add a firmware-side negative ack/timeout.
4. **`<session>` is never defined.** Worse, the daemon's `Session._command_once`
   auto-re-logins and **replays** a command on an auth-required reply — and
   pushes are append deltas, so a replay **duplicates answer text**. Define
   `<session>` as the UART named-session epoch captured at `llm_ask` time,
   reject mismatches exactly as
   [System_Cm5HostControl.cpp:1306](../components/hardwareone/System_Cm5HostControl.cpp:1306)
   does, and make pushes idempotent by seq.

Per-token push is not viable (firmware admits one line per loop lap
[System_UartLink.cpp:1110](../components/hardwareone/System_UartLink.cpp:1110);
daemon serializes behind one `asyncio.Lock`). Use the shipped EvenAI cadence as
precedent: `_STREAM_FLUSH_CHARS=140`, `_STREAM_PART_BYTES=200`.

---

## 7. OLED — corrected

v1's "fourth instance of a pattern already in that file" was **wrong about the
pattern**. The pattern includes a hard state-ownership structure that erases any
sub-state not enumerated in it.

`displayLLM()` calls `syncStateFromEngine()` at the top of every frame
([OLED_Mode_LLM.cpp:653](../components/hardwareone/OLED_Mode_LLM.cpp:653)).
That function is a **fully-enumerated dispatcher with no leave-alone default**:
`sKeyboardActive` returns at :147, the PICK_GROUP/TEMPLATE/ENTITY trio returns
at :168, GENERATING at :177/:183, LOADING at :193 — and only NO_MODEL and READY
reach the reflect-engine block at :196-203, which assigns `sUIState`
**unconditionally** from engine state. A new `MENU` entered from READY is
stomped back to READY on the next tick; `PICK_MODEL` entered from NO_MODEL is
stomped back to NO_MODEL. **The picker would render for zero frames**, and the
enum, switch case, populate function and input case would all look correct in
isolation — it only shows on hardware.

Nor can the trio's guard simply be extended: it requires
`st.state == READY && llmMenuGroupCount() != 0 && llmMenuGeneration() == sPickGen`
(:160-162), and `PICK_MODEL` is entered precisely when **no model is loaded**.

**Corrected design:** replace the three-name branch with a `stateOwnsUI()`
predicate covering MENU / PICK_MODEL / PICK_GROUP / PICK_TEMPLATE / PICK_ENTITY,
each with its **own** exit condition — guided pickers keep the engine-READY +
menu-generation test; `PICK_MODEL` has no engine precondition and exits only on
select/back; `MENU` exits when the engine leaves READY.

**What held up:** `OLEDScrollState` does store `const char*` and not copies, so
static PSRAM row backing is required and sufficient; index-based selection into
`sModelDescs` is lifetime-safe; `EXT_RAM_BSS_ATTR` is valid and degrades to DRAM
on non-PSRAM boards; and re-binding X in READY to MENU does **not** break the
retry/`sLastTurnGuided` logic (set only in `submitGuided` :381-398, cleared on
keyboard submit :681).

**Row budget:** `sModelRows[LLM_CHARS+1]` is 21 glyphs and
`oledScrollRenderSimple` prints a 2-char `"> "` cursor first
([OLED_Utils.cpp:1481](../components/hardwareone/OLED_Utils.cpp:1481)), leaving
**19 columns**. `LFM2-8B-A1B-UD-Q3_K_XL.gguf` is 27 chars; add `" [pi]"` or
`"(offline)"` and ~10-14 characters of model name survive — not enough to
distinguish two quant variants of one family. Design the row format (or shorten
`name`) before S3.

---

## 8. Footprint — the number v1 cited is stale

**MEASURED 2026-08-17** on `xiao_s3` (the carrier config), replacing the review's
regression estimate. Three real builds, `wc -c` on `hardwareone-idf.bin`:

| `BACKEND` / `ONBOARD` / `CM5` | binary | delta |
|---|---|---|
| 0 / 0 / 0 (baseline) | 5,348,992 | — |
| 1 / 1 / 0 (feature + onboard engine) | 5,537,024 | **+188,032 (+183.6 KB)** |
| 1 / 0 / 1 (feature + CM5 only) | 5,348,992 | **+0** |

The review's ~94 KB figure was for the *surfaces* only and excluded the engine
TUs; the engine is roughly the other ~90 KB. Nothing contradicts, but §8 v2's
phrase "the full layer" was ambiguous — **the real all-in number is 183.6 KB.**

The `+0` row is expected, not a win: at S1a the surfaces' internal `#if`s still
say `ENABLE_LLM_SOURCE_ONBOARD`, so with the engine off they compile to empty
TUs. It does prove the flag model links in the combination the review flagged as
never-built. Making that row carry real code is exactly S1b's job, and its true
cost lands there.

v1's "~246 KB (5%) free" matches neither the committed tree nor the working
tree. Committed HEAD gives **108 KB free (2.0%)**, where ~94 KB is genuinely
marginal. The working tree gives **620 KB free (10.6%)**, where it fits several
times over — but that depends on an **uncommitted `partitions_no_sr_8mb.csv`
factory growth that also forces a one-time LittleFS reformat.**

**This is the decision that gates the whole plan and it is yours, not mine:**
either commit that partition change (accepting the reformat), or the carrier
build ships `ENABLE_LLM_BACKEND 1` + `SOURCE_CM5 1` + `SOURCE_ONBOARD 0` and
takes `CUSTOM_ENABLE_WEB_LLM 0` as well.

The §2 model makes that fallback cheap rather than a redesign: dropping the
onboard source removes the engine, and the OLED mode is already gone on that
board via `ENABLE_OLED_DISPLAY 0`. What remains is registry + chat + commands
(~10 KB), which fits the committed 108 KB with room to spare. The full ~94 KB
layer is what needs the partition growth.

---

## 9. Corrected staging

| Stage | Content | Gate |
|---|---|---|
| ~~**S0**~~ | ~~`LLM_SETTING_CMD` index→cmdKey lookup (§1)~~ | ✅ **DONE** — 17/17 verified, syntax-clean |
| ~~**S1a**~~ | ~~Flag model (§2): LLM moved to BuildConfig §5, `ENABLE_LLM_BACKEND` + two `SOURCE_*` flags + 3 guards, rename across 30 files, CMake mirror~~ | ✅ **DONE** — 8/8 flag combos behave correctly; 3 board builds green |
| ~~**S1b**~~ | ~~backend registry, CM5 source, chat drain, command-surface split, settings re-gate~~ | ✅ **DONE** — all 3 legal configs build; see below |
| ~~**S2**~~ | ~~re-gate web + OLED surfaces, OLED `stateOwnsUI()` + MENU + PICK_MODEL, re-gate registration sites~~ | ✅ **DONE** — CM5-only build is wired, +100.8 KB |
| **S3** | G2 lens page: 11 `ENABLE_LLM_SOURCE_ONBOARD` guards + `llmMenuScanModels()` → registry | G2 model picker lists CM5 models |
| **S3** | OLED `stateOwnsUI()` restructure, MENU + PICK_MODEL, row-format budget, stale ANO header comment | HW test on joystick **and** ANO |
| **S4** | CM5 backend: catalog table, EVT ask w/ params, raw-tail push intrinsic + escaping, session epoch fencing, staleness deadline | HW test with daemon |
| **S5** | Daemon: enumerate `/opt/models/*.gguf`, switch = respawn + health wait, push catalog, UTF-8 EVT branch | HW test |

S1b+S2 is the large stage. S4/S5 remain the coordinated pair.

**S1a as shipped (2026-08-17, uncommitted, not hardware-tested):**
`System_BuildConfig.h` §4 LLM block deleted and re-declared in §5 as a composed
app; `ENABLE_ONDEVICE_LLM` → `ENABLE_LLM_SOURCE_ONBOARD` across 30 files;
three guards added after the board blocks (late, because the CM5 guard needs
`UART_LINK_PORT`); `CMakeLists.txt` now greps three flags and splits sources —
engine TUs + the `esp-dsp` REQUIRES on `SOURCE_ONBOARD`, chat/web/OLED on
`BACKEND`. `OLED_Mode_LLM.cpp` deliberately keeps no CMake-side display gate, per
the existing convention that OLED modes are always compiled and self-gate.

Verified: all 8 flag combinations produce the intended result (3 legal build, 5
illegal `#error`, none misfire); `xiao_s3` builds green at 0/0/0, 1/1/0 and
1/0/1; restored config returns to a byte-identical `0x519e80`; zero stale
references to the old flag name in code.

**The `System_LLM.cpp` split was deliberately deferred** out of S1a. It requires
moving the command surface away from file-static engine globals (`gLLM`,
`gLLMResultBuf`), which is the same coupling S1b breaks with the backend vtable
— doing it twice would mean doing it wrong once.

---

## S1b as shipped (2026-08-17, uncommitted, NOT hardware-tested)

New files: `System_LLMTypes.h` (vocabulary that must exist without an engine),
`System_LLMBackend.{h,cpp}` (registry + dispatch), `System_LLMCm5.{h,cpp}` (the
CM5 source), `System_LLMCommands.cpp` (the command surface, moved out of
`System_LLM.cpp` so it compiles under the feature flag).

Landed:
- **Registry.** `llmEnumerateModels()` / `llmResolveModelId()` — the single
  id→source resolver. Ids are `onboard:<file>` / `cm5:<name>`; a bare filename
  or absolute path still resolves, for hand-typed CLI and pre-id settings.
- **Chat drain is backend-dispatched.** Every engine global named in §5
  (`llmGetSessionId` :180, `llmGetResultLen/Chunk` :191/:195,
  `llmIsGenerationDone` :211, `llmGetStatus` :212) plus the readiness gates at
  :309/:354 now route through the vtable. Framing goes through
  `llmBackendFramePrompt`, so the local `Q:/A:` template can no longer leak into
  a remote prompt; `llmBackendTokenize` returns 0 remotely, which leaves retry's
  suppress list naturally empty.
- **Three duplicate model scans collapsed into one.** `llmListModels()` is now a
  projection of the registry, so the web picker sees remote models for free.
- **Two duplicate path resolvers deleted** — the SD-then-internal probe in
  `cmd_llm_load` and the directory allowlist in `handleLLMLoad` both now call
  `llmBackendSelect`.
- **`llmmodels` is SCHEMA 2**: rows are objects with an `id`. A name is no
  longer a handle. Apps reading `models[]` as strings must be updated.
- **CM5 protocol** per §6, with the review's fixes built in: raw-tail parsing,
  whitespace-escaped both directions (so the pre-dispatch `cmd.trim()` cannot
  eat a chunk's leading space), seq-idempotent pushes, epoch fencing, and a
  45 s stall timeout plus presence-loss abort wired to `llmBackendTick()` — the
  wedge §5 warned about.
- **Settings re-gated** to `ENABLE_LLM_BACKEND`.

Measured on `xiao_s3`:

| config | binary | delta |
|---|---|---|
| 0/0/0 baseline | 5,348,992 | — |
| 1/0/1 CM5-only | 5,354,784 | **+5.7 KB** |
| 1/1/0 onboard-only | 5,539,792 | +186.3 KB |

**Superseded by S2 — see below.** That +5.7 KB was measured before the surfaces
were re-gated, and was mostly a dead-strip artifact.

---

## S2 as shipped (2026-08-17, uncommitted, NOT hardware-tested)

- **Web + OLED re-gated** to `ENABLE_LLM_BACKEND` (`WebPage_LLM.{h,cpp}`,
  `OLED_Mode_LLM.cpp`).
- **The local-model API got ungated no-op stubs** (`System_LLMBackend.cpp`,
  declared in `System_LLMTypes.h`): `llmContextDegraded/Warning`,
  `llmModelDescription/Icon`, and the whole `llmMenu*` family. A remote model
  reports "no guided menu, no icon, no degraded context", which is exactly what
  the surfaces already handle for a local `.bin` that ships none — so they need
  no new branching. The alternative was ~30 `#if`s scattered through the UI.
- **OLED `syncStateFromEngine()` restructured** around a `stateOwnsUI()`
  predicate with a per-state exit condition, then **`MENU` + `PICK_MODEL`
  added**. X in READY now opens an action list (Ask / Guided questions / Switch
  model / Unload) instead of being inert on models with no guided menu; the
  model picker is a real `OLEDScrollState` list over the registry, replacing the
  `FilePickerRequest` into the file browser. Row text elides the MIDDLE of long
  names so two quant variants of one family stay distinguishable in 19 columns.
- **Registration sites re-gated** — `System_Utils.cpp` (commands + help),
  `WebServer_Server.cpp` (routes), `WebPage_Dashboard.h` (tile),
  `OLED_Utils.cpp` (menu entry + mode init), `System_FeatureRegistry.cpp`
  (`isLLMCompiled`), `System_Automation.cpp`, `System_RamFlush.cpp`.

**Two traps worth recording.**

*Dead-strip.* Re-gating the surface files was not enough: the sites that
**register** them were still engine-gated, so the whole feature compiled and was
then discarded by the linker. `System_LLMCommands.cpp.obj` measured 2,579 bytes
in that state and 34,875 once actually referenced — a 13× difference that looks
exactly like "it works and is cheap". **Object size is not evidence of linkage.**

*Linkage.* `const CommandEntry llmCommands[]` at namespace scope has INTERNAL
linkage in C++ unless a prior `extern` declaration is in scope. That declaration
lived in `System_LLM.h`, which a CM5-only build does not include, so the table
silently became file-static. Moved to `System_LLMTypes.h`.

**Measured back-to-back in one tree state** (see the caveat below):

| config | binary | delta |
|---|---|---|
| 0/0/0 baseline | 5,701,648 | — |
| 1/0/1 CM5-only, wired | 5,804,912 | **+100.8 KB** |
| 1/1/0 onboard-only | 5,894,080 | +187.9 KB (**2% partition free**) |

⚠️ **These are not comparable to the S1a/S1b figures above.** Another agent was
editing this working tree concurrently (microphone keyboard input / dictation),
and the 0/0/0 baseline moved by ~352 KB between measurement sessions. Every
number in this section was taken back-to-back within one tree state; the older
tables were taken in a different one. Re-measure before making a decision on
partition headroom.

---

## 10. Review coverage — honest limits

Six lenses, 15 high-severity findings. My verification budget covered the **top
6 by severity**; the other 9 highs in §§3-6 above are reported **as found, with
citations, but not independently refuted.** The two v1 claims I checked myself
after the fact (ANO map, axis flip) both turned out **wrong in v1**, which is
reason to treat the unverified nine as leads to confirm during implementation
rather than as settled fact.

Not covered by any lens: the Android/BLE app side of the `llmmodels json`
contract change, and any actual build measurement.
