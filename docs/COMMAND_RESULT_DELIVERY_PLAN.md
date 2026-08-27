# Command Result Delivery Plan

**Status:** Phase 1 implemented (2026-07-26, HW test pending) — see §11 for as-built notes; §4.4 is superseded  
**Date:** 2026-07-25  
**Goal:** Stop routing the command **return value** through the debug/broadcast line queue, so large/machine-readable results (especially JSON over serial) stay byte-exact and copy-pasteable, while keeping human line streaming on the existing small debug path.

---

## 1. Why this exists

The firmware already documents two output channels (`System_Utils.cpp` OUTPUT CONTRACT):

| Channel | Shape | Cap | Purpose |
|---|---|---|---|
| `broadcastOutput()` / debug queue | Human lines | `DEBUG_MSG_SIZE` = 256 B | Live console, progress, tables |
| Command return value (`out`) | One verbatim blob | `CMD_RESULT_MAX` = 4096 B | JSON / addressed reply |

Debug enqueue is **intentionally small and non-splitting**: callers prove a line fits; oversize is `[CUT]`. That is correct for logs.

The bug is architectural leakage at **transport completion**: several origins take the return blob and call `broadcastOutput(out, ctx)`, which re-enters the 256 B queue. Serial is the worst case (no other delivery path). Web/BLE already have a clean reply channel and only use the queue for console mirroring.

Recent serial chunking (`broadcastCommandResultQueued`) is a bandage on that leak. It preserves content but keeps results in the wrong pipe (prefixes, timestamps, interleaving, prompt races).

---

## 2. Verdict: is it worth it?

**Yes — if scoped as “finish the existing two-channel design,” not “build a second debug system.”**

| Concern | Reality |
|---|---|
| Is debug meant for 4 KB JSON? | **No.** Verified in `System_Debug.h` / `System_Utils.cpp`. |
| Must handlers change? | **No** for Phase 1–2. Handlers already return the blob correctly. |
| Is this a rewrite of `debug_out`? | **No.** Debug stays line-sized. |
| Where is the real work? | Transport completion sites that fan out `out` via `broadcastOutput(..., ctx)`. |

It is **medium**, not enormous: one new delivery helper + a handful of call-site swaps + serial prompt ownership. It becomes large only if we also redesign web CLI history, BLE stream console, and OLED mirroring in one shot.

---

## 3. Target architecture

```
                    ┌─────────────────────────────┐
  cmd_* handler ──► │ return value (≤4 KB blob)   │
                    └──────────────┬──────────────┘
                                   │
                    deliverCommandResult(out, ctx)
                                   │
         ┌─────────────┬───────────┼───────────┬────────────┐
         ▼             ▼           ▼           ▼            ▼
      Serial        HTTP body   BLE notify   MQTT/ESPNOW   optional
      (direct,      (already)   (already)    (already)     mirror:
       undeco-                                              short
       rated)                                               summary
                                                            or file

  During execution, human lines still go:
      broadcastOutput(...) ──► debug queue (256 B) ──► sinks
```

**Rules after this work:**

1. **Never** put the full command return blob into `gDebugOutputQueue`.
2. Human streaming during a command still uses `broadcastOutput` / debug queue.
3. JSON mode still must not mix broadcast lines with the return blob (existing rule).
4. Serial prompt `$ ` prints **after** the result is fully written.
5. Optional: a **short** audit/mirror line may still go through the debug queue (e.g. `[serial red@local] memreport json → 1842 B`) for console history — not the payload itself.

---

## 4. What needs to change

### 4.1 New API (core of the work)

Introduce one chokepoint, e.g. in `HardwareOne.cpp` / `System_Debug.h` (or a small `System_CommandResult.cpp`):

```cpp
// Deliver the command return blob to origin-appropriate sinks.
// MUST NOT enqueue the full blob into gDebugOutputQueue.
void deliverCommandResult(const String& result, const CommandContext& ctx);
```

Behavior by origin / mask:

| Origin / need | Delivery |
|---|---|
| `ORIGIN_SERIAL` + `MSG_ROUTE_SERIAL` | Write blob to UART via a **serial result writer** (still owned by firmware I/O policy; may briefly pause or coordinate with `debug_out` so two tasks do not interleave mid-byte). Then print `$ `. |
| Web HTTP | Unchanged: body already sent from `out`. Mirror to web history / file is the open design choice (see §5). |
| BLE command reply | Unchanged: `sendBLEResponseToConn` already uses the blob. **Remove** any secondary full-blob queue fan-out if present on that path. |
| MQTT / ESP-NOW / voice | Unchanged: they already treat return value as the reply. |
| File / OLED / G2 | Do **not** dump 4 KB JSON into OLED/G2 by default. Either skip, or emit a one-line summary through the normal broadcast path. |

### 4.2 Call sites that currently abuse the queue for results

These are the mechanical swaps (`broadcastOutput(out, ctx)` → `deliverCommandResult(out, ctx)` or drop if already delivered):

| Site | File | Notes |
|---|---|---|
| Serial CLI completion | `HardwareOne.cpp` (~2480) | **Must-fix.** Also move `$ ` after delivery. |
| Web `/api/cli` | `WebServer_Server.cpp` (~3265) | HTTP body already correct; today also mirrors full result into queue. Decide mirror policy (§5). |
| Web `/api/cli/batch` | `WebServer_Server.cpp` (~5094) | Same as web CLI. |
| `executeUnifiedWebCommand` | `System_Utils.cpp` (~4990) | Same. |
| Automation / boot helper | `System_Utils.cpp` (~4974) | Often short; still should not assume 256 B. |
| OLED remote / unified menu | `OLED_Utils.cpp`, `OLED_Mode_UnifiedMenu.cpp` | `broadcastOutput(out)` without ctx — treat as human mirror or route through deliver with OLED origin. |

**Already correct (do not regress):**

- BLE `bleCommandResultCallback` → `sendBLEResponseToConn` (blob, not queue).
- MQTT / voice / ESP-NOW remote reply paths that publish `out` on their own pipes.

### 4.3 Remove / shrink the bandage

Once serial uses `deliverCommandResult`:

- Delete or gut `broadcastCommandResultQueued` in `HardwareOne.cpp`.
- `broadcastOutput(const String&, const CommandContext&)` either:
  - becomes a thin wrapper that only emits a **short decorated status/audit line**, or
  - is deprecated for result fan-out and only used for human lines that happen to carry ctx metadata.

Do **not** raise `DEBUG_MSG_SIZE` to 4096. That would balloon the message pool (~96–192 slots × size) and fight the whole lazy/RAM direction.

### 4.4 Serial writer vs `debug_out` (the one real concurrency issue)

Today serial writes from `debugOutputTask` (`Serial.printf("[%lu] %s\n", ...)`). If `deliverCommandResult` also writes Serial from `loop` / `cmd_exec`, two writers can interleave.

**Pick one (recommended order):**

1. **Preferred:** result delivery posts a single “result job” to `debug_out` (or a tiny sibling queue) that writes the **whole blob** in one drain turn, without packing it into `DebugMessage::text[256]`. The blob pointer/length lives outside the line pool (stack/`String`/`ExecReq::out` lifetime until drained).
2. **Acceptable:** mutex around UART writes shared by debug drain and result delivery.
3. **Avoid:** raw `Serial.print` from arbitrary tasks with no coordination (violates the project’s “no unsynchronized UART” rule that motivated the IDF log bridge).

Option 1 keeps “one UART owner” while still **not** using the 256 B line pool for the payload. That is the key distinction from today’s chunker.

### 4.5 Files expected to change

| Phase | Files |
|---|---|
| 1 (serial fix) | `HardwareOne.cpp`, maybe `System_Debug.cpp`/`.h` (UART ownership helper), this doc |
| 2 (API + call sites) | `System_Utils.cpp`, `WebServer_Server.cpp`, OLED callers above |
| 3 (cleanup) | remove chunker; tighten comments in `System_CommandTypes.h` / OUTPUT CONTRACT; CHANGELOG |
| Optional polish | web CLI JS if history should show “result omitted, see response body”; CLI help text |

**Out of scope unless explicitly expanded:** command handlers, `CMD_RESULT_MAX`, debug pool sizing, BLE secure framing, ESP-NOW reassembly.

---

## 5. Design choices to decide up front

### A. Web mirror of the return blob

Today web CLI sends HTTP body **and** `broadcastOutput(redactedOut, ctx)` into the web mirror.

Options:

1. **Stop mirroring the full blob** into the queue; HTTP response is authoritative. Web CLI UI already has the body. (Cleanest.)
2. Mirror a **truncated one-liner** for history (`[web] memreport json (1842 B)`).
3. Keep full mirror via a web-only path that is **not** the debug line pool (e.g. append to `gWebMirror` directly in chunks). More work; only if history must show full JSON.

**Recommendation:** (1) or (2) for Phase 2.

### B. Human vs machine on serial

- Machine JSON (`{` / `[` or explicit `json` mode): raw blob, no `[serial user@ip]`, no timestamp.
- Human short returns (`OK: ...`): either raw or lightly decorated — pick one and stick to it.
- Human multi-line reports that already stream via `broadcastOutput` and return `"OK"`: unchanged (debug queue).

### C. Should automation results hit serial?

Automation today uses a broad `outputMask`. After the split, automation should keep streaming human lines on the debug path; the return blob should go to file/autolog / whatever already consumes `out`, not dump JSON onto the user’s interactive serial unless that was intentional.

### D. Prompt ownership

Serial `$ ` must not print until result delivery completes (including async drain if using option 1 in §4.4). Today it prints immediately after `broadcastOutput`, racing the queue.

---

## 6. Phased rollout

### Phase 0 — Spec lock (½ day)

- Agree: debug queue stays 256 B; results never enter it.
- Agree: serial UART ownership strategy (§4.4 option 1 or mutex).
- Agree: web mirror policy (§5 A).

### Phase 1 — Serial path only (1–2 days) — **highest value**

1. Add `deliverCommandResult` with serial + “wait/drain then `$ `”.
2. Serial CLI: replace `broadcastOutput(out, uc.ctx)` with `deliverCommandResult`.
3. Keep `broadcastCommandResultQueued` temporarily for other origins, or leave them as-is.
4. Test: `memreport json`, `taskstats json`, `status json`, long human `taskstats`, login/logout, noisy BT/G2 background.

**Exit criteria:** paste serial JSON into `jq` with no stripping; `$` appears after the blob; debug lines may appear before/after but not mid-blob if UART is owned for the write.

### Phase 2 — Unify completion sites (1–2 days)

1. Swap remaining `broadcastOutput(out, ctx)` result fan-outs to `deliverCommandResult`.
2. Apply web mirror decision.
3. OLED callers: summary line or deliver with OLED-safe policy.

**Exit criteria:** no call site feeds a full `CMD_RESULT_MAX` blob into `enqueueChunk`.

### Phase 3 — Cleanup (½–1 day)

1. Remove `broadcastCommandResultQueued`.
2. Narrow `broadcastOutput(..., ctx)` docs: not for return blobs.
3. Add a short contract note near `CMD_RESULT_MAX` pointing at `deliverCommandResult`.
4. CHANGELOG + quickstart note if serial UX changes.

### Phase 4 — Optional hardening

- Assert/debug counter: if anything enqueues a line that looks like a full JSON result > 256 B path, log once.
- Host-side serial collector can drop its JSON reassembly hacks.

---

## 7. Size estimate (honest)

| Scope | Effort | Risk |
|---|---|---|
| Phase 1 only (serial) | **Small–medium** | Low — biggest user win |
| Phase 1–3 | **Medium** | Medium — web history UX needs a conscious choice |
| Plus redesigning BLE stream console / dual queues for all sinks | **Large** | High — **not recommended** |

This is **not** a rewrite of the debug system. It is undoing an accidental coupling at ~5–8 completion sites.

If it feels large, that is usually Phase 2 web-history ambiguity — not the serial fix itself. Ship Phase 1 even if Phase 2 waits.

---

## 8. Test plan

### Must pass

- [ ] `memreport json` on serial → single contiguous JSON, `jq` parses, no `[CUT]`, no `[serial …]` prefix on the blob
- [ ] `taskstats json`, `perftop json`, `status json` same
- [ ] Human `taskstats` / `memreport` (text) still stream line-by-line via debug queue
- [ ] `$ ` appears only after result completes
- [ ] Web `/api/cli` still returns full body in HTTP response
- [ ] BLE command reply still delivers full blob on notify characteristic
- [ ] With BT/G2 connected and debug noise on, JSON blob is not spliced mid-object (UART ownership)
- [ ] `login` / auth failures still readable on serial
- [ ] Commands that stream then return `OK` unchanged

### Regression watch

- [ ] Web CLI page history still usable under chosen mirror policy
- [ ] File logging still gets audit lines (not necessarily full JSON)
- [ ] OLED/G2 not flooded with multi-KB JSON
- [ ] Help mode / suppressed output behavior unchanged for human lines
- [ ] Capture-output web path (`capture=1`) still folds broadcast stream into HTTP body

---

## 9. Non-goals

- Raising `DEBUG_MSG_SIZE` / growing the debug pool for results
- Per-handler `Serial.print` of results
- Replacing `broadcastOutput` for human streaming
- Changing `CMD_RESULT_MAX` or handler JSON builders
- Host-only reassembly as the long-term serial contract

---

## 10. Recommended decision

Proceed with **Phase 1 immediately**, Phase 2–3 as a short follow-up.

That gives copy-pasteable serial JSON and removes the wrong-pipe design without a large undertaking. Treat anything beyond “deliver the return blob outside the line pool, keep debug small” as scope creep.

---

## 11. Phase 1 as built (2026-07-26)

Phase 1 shipped with a **simpler mechanism than §4.4 proposed**. Code review established that on this build `Serial` is USB-CDC (`HWCDC`) and every `Serial.write()` call already holds the driver's `tx_lock` for the whole call (classic-UART boards are equally mutex-locked per call). A single write of the whole blob is therefore already atomic against `debug_out`'s per-line printf — the §4.4 result-job/drain-barrier/mutex apparatus was solving a solved problem, and its stated blob-lifetime anchor (`ExecReq::out` until drained) was wrong anyway (`submitAndExecuteSync` frees the ExecReq before completion fan-out runs). **§4.4 is superseded.**

### What was implemented

- `deliverCommandResult(result, ctx)` (`HardwareOne.cpp`, declared in `System_Debug.h`): serial-origin results are written to the console **directly** — blob + trailing `\n` in one `Serial.write()` call, byte-exact, no `[origin]` decoration, no chunking — then mirrored to the remaining sinks (file/OLED/G2, web when masked) through the existing queued chunker with `MSG_ROUTE_SERIAL` cleared. Non-serial origins pass through to `broadcastOutput(s, ctx)` unchanged.
- Gates replicated from the queue pipeline (a direct write bypasses where they live): per-command `ctx.validateOnly` (race-free equivalent of the `gCLIValidateOnly` global the core checks), help-mode suppression (`gCLIState`/`gInHelpRender` — mirror path keeps the suppression bookkeeping), and the `outSerial` kill-switch (`gOutputFlags & MSG_ROUTE_SERIAL`).
- `debugWaitOutputDrained(120)` (`System_Debug.cpp`) before the write: bounded wait for the queue to drain so the result doesn't overtake lines the command streamed. USB-CDC short writes are detected and surfaced.
- Serial CLI completion swaps `broadcastOutput(out, uc.ctx)` → `deliverCommandResult(out, uc.ctx)`. The `$ ` prompt is unchanged code but now correctly ordered: the blob write is synchronous on the same task.
- The chunker (`broadcastCommandResultQueued`) gained pacing: `debugQueueBackpressure(8, 50)` per frame under a **250 ms total budget** — fixes the silent tail-drop of large results under queue saturation without letting a wedged drain hold the calling task for ~850 ms.

### Accepted deltas / known limitations (all verified minor)

1. **Drain-wait timeout reorder:** if the queue still holds >120 ms of backlog at completion, the result prints before the backlog tail (old path was FIFO; its failure mode under the same load was dropping frames instead).
2. **Empty results** no longer print a stray `[serial user@local] ` prefix-only line on serial (improvement; mirror sinks unchanged).
3. **Mirror timestamps** for serial results are stamped after the wait + write (≤ ~220 ms later); file/OLED interleave order can shift accordingly. Content is byte-identical.
4. **Classic-UART boards** (QT Py / Feather V2): the single 4 KB write holds the UART lock ~350 ms, pausing the drain for that window; the short-write check is CDC-only there. Accepted on secondary targets.
5. **LOOPHEALTH**: large results under heavy debug traffic can add bounded INPUT-section lap time (≤120 ms wait + write + ≤250 ms mirror pacing); `[LOOPHEALTH]` warns on that path are expected under load, not a defect.

### Still open for Phase 2 (revalidate before starting — see review notes)

- Web mirror policy (§5A): **option 1 as written does not work** — the web CLI poller repaints the pane from the mirror every 500 ms (`WebPage_CLI.h`), so dropping the mirror requires the JS change; it is mandatory, not optional polish.
- §5C's premise is wrong: `System_Utils.cpp:4974` is the **boot-autostart** helper (`runUnifiedSystemCommand`), not automation, and its queue broadcast is the only consumer of `out` — dropping `MSG_ROUTE_FILE` there needs a replacement. Automations already discard results (`submitCommandAsync(uc, nullptr, nullptr)`).
- §4.2's OLED rows are mis-scoped (no-ctx overload, 256-capped; `completeRemoteCommandInput` is unreachable) and the BLE “secondary fan-out” in the ctx overload is dead code (no caller sets `MSG_ROUTE_BLE`) — remove, don't budget for it.
- Redaction is inconsistent across completion sites (serial logs raw `out` to `debug.log`; `/api/cli/batch` returns unredacted bodies while mirroring redacted) — unify deliberately when the sites merge.
- Independent pre-existing bug found in review: ESP-NOW remote command results are truncated to 256 B at the *originator's* console via `BROADCAST_PRINTF` (`System_ESPNow.cpp:~8840`) — same bug class this plan fixes for serial.
