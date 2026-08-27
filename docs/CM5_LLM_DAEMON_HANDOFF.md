# Handoff: CM5 daemon side of the LLM backend

> **STATUS 2026-08-17 — CARRIED OUT. This is now a protocol reference, not a
> work order.** The daemon side is implemented in
> `~/esp/HardwareOne_RaspPi_CoProcessor`: `ai-service/hw1_ai_service/cm5_llm.py`
> plus 46 tests (600 pass suite-wide). All three gotchas below are implemented
> and pinned by tests. Neither side is hardware-tested; both are uncommitted.
>
> **Two firmware bugs were found by the daemon author reading this spec against
> the source, and are now fixed:**
> 1. The prompt-length check compared the escaped length against
>    `CM5_LLM_MAX_PROMPT * 2`, allowing a 1400-byte payload into a 1024-byte
>    frame. The budget is now `CM5_LLM_MAX_PROMPT_ESCAPED` (960) checked against
>    the **escaped** length, with a raw pre-check.
> 2. `llmBackendActiveModel()` returned the model the user *asked* for, not the
>    one the host adopted via `ready`. It now re-derives from the source.
>
> **Host-depended behaviours — do not "clean up" without telling the daemon side:**
> - An **empty push** (`cm5 llm push <s> <seq>` with no tail) is a **keepalive**,
>   not a no-op. It stamps `sLastEventMs` and consumes a seq. The host sends one
>   every 22.5 s during a cold model's prefill; removing the empty-tail branch
>   makes cold-start switches abandon mid-turn on `CM5_LLM_STALL_MS`.
> - **Catalog names are single escape-free tokens ≤31 chars.** `models`/`ready`
>   store the name with `tokCopy` (no unescape) while `cm5LlmSelectByName`
>   escapes it again on the way out, so the round trip is only an identity while
>   the name contains no whitespace. This is a **contract**, not an accident.
> - The host always sends a terminal `end`, including after `llm_cancel`. Because
>   `cm5LlmStop` clears `sGenEpoch` before pushing the cancel, that `end` is
>   rejected with "session epoch mismatch". **Expected traffic, not a bug.** Same
>   for pushes arriving after the stall timer fires.
>
> ---
>
> ## UPDATE 2026-08-17 (second pass) — read this before the sections below
>
> **THE SELECT-FAILURE GAP. This is the important one.** There is no verb by
> which you can tell the firmware that a select failed. `models | ready | push |
> end` is the entire inbound vocabulary, and `end` is fenced on a generation
> session that a select never mints, so an `end` sent to report a failed select
> is rejected as a stale session. Firmware now bounds the damage instead of
> hanging forever:
>
> - **`CM5_LLM_SELECT_TIMEOUT_MS` = 120000.** A select you accept and never
>   confirm with `ready` is abandoned after two minutes, or **immediately** if
>   the presence lease lapses. Before this, the device sat in LOADING until a
>   human ran `llmunload`. Deliberately far above `CM5_LLM_STALL_MS` — it bounds
>   "never", not "slow".
> - **`cm5LlmStatus()` now consults the presence lease.** Previously it reported
>   READY on a model whose host had been gone for minutes while
>   `cm5LlmStartAsync` was already refusing to generate. It now reports ERROR
>   "the CM5 is not reachable". Side effect worth knowing: `LLMState::ERROR` was
>   previously **unreachable** for the CM5 source, so every surface's error
>   branch was dead code for remote models. It is live now.
>
> **What you should do today, needing no firmware change:** on ANY select
> failure, immediately send `cm5 llm ready <gen> <the-model-you-are-actually-
> serving>`. That is the only line that clears LOADING, it is unfenced, and the
> name is adopted verbatim — so it produces exactly the snap-back a user
> expects. If the failure left you serving nothing, there is no line that says
> so; keep or restart a known-good fallback and `ready` on that.
>
> **Also re-send `ready` after ANY llama-server restart or config reload, even
> when the model did not change.** `push` and `end` carry no model identity, so
> the firmware cannot otherwise notice that what it displays is no longer what
> answers.
>
> **OPEN PROPOSAL, needs your call:** extend `ready` to
> `cm5 llm ready <gen> <name> [ok|fail] [escaped-reason]`, 4th and 5th tokens
> optional so the current daemon stays compatible. That would let a failure
> carry a reason to the user instead of a flat timeout. Say the word and it goes
> in.
>
> **`[tokens]` in `end` MUST be model completion tokens** — `n_decoded` /
> `tokens_predicted` in llama.cpp terms. Not the number of `push` lines, not
> characters, not prompt+completion. It lands in the same `LLMStatus` field the
> on-device engine fills from its own sampler loop, with no backend tag attached,
> so a surface cannot tell which engine produced the integer. Two corrections to
> assumptions: `[tokens]` and `[tokPerSecX10]` are **independent** — firmware
> never derives one from the other, so sending pushes for `[tokens]` corrupts
> only the "N tok" figure — and an omitted `[tokens]` shows **0**, never a stale
> value from the previous generation (both are zeroed at the top of every
> generation).
>
> **THE PROMPT CEILING IS RESOLVED IN FIRMWARE, AND THE STALE 1400 IS NOW YOURS.**
> The frame-overflow bug is genuinely fixed with 33 bytes of margin. The figure
> that remains wrong is `MAX_PROMPT_CHARS` in your `cm5_llm.py` — **set it to
> 960**. After unescape the firmware can never emit more than 960 raw characters
> (whitespace-free extreme); the all-whitespace extreme caps the raw prompt at
> 480. 960 is the correct single bound. Note the firmware's rejection is
> currently SILENT and model-dependent (a prompt accepted for a local model can
> be refused for a remote one), which is a firmware wart, not yours.
>
> **PARAMS: 8 resolved and clamped, 3 transmitted.** `chatResolveParams` clamps
> BEFORE marshalling on every path that can produce a frame, so the values you
> receive are already final — do **not** re-clamp and do **not** reject them.
> On the wire: `maxTokens` [1,512], `temperature` [0.0,2.0] as `tempX100`,
> `topp` [0.01,1.0] as `toppX100`. Dropped on the way to you: `minP`,
> `repPenalty`, `repWindow`, `sentenceLimit`, `hardCap`, and the suppress list
> (that last one deliberately — those are ids from the device's own tokenizer).
> **The consequence is a real divergence you should know about:** `hardCap` (80)
> and `sentenceLimit` (2) are resolved, clamped, then thrown away for the CM5
> while `maxTokens` (256) is sent — so on stock settings the onboard model
> answers in two sentences and you answer with up to 256 tokens for the identical
> prompt.
>
> **`cm5 heartbeat` now takes an optional trailing reason token.** See the
> presence section; capabilities advertises `heartbeat_reason=1 reason_max=96
> reason_states=busy,degraded`.
>
> ---
>
> **`CM5_LLM_STALL_MS` is now 60000 (was 45000).** It is sized against the host's
> 22.5 s keepalive, not against inference time — it is really "how many
> keepalives may be lost before the turn is abandoned". At 45 s that was two, and
> since the firmware admits at most one line per loop lap, losing one keepalive
> to a busy moment put the second exactly on the boundary. 60 s buys a third.
> **If the host changes its keepalive interval, this must move with it.**

---

## What already exists on the firmware side (do not re-derive)

The XIAO now has an LLM **model registry** where the on-device engine and the
CM5 are two symmetric *sources*. Selecting `cm5:<model>` in any surface (web,
OLED, G2, CLI, BLE app) routes the whole conversation to the CM5.

Firmware files (read-only for you, but they are the contract):
`components/hardwareone/System_LLMCm5.{h,cpp}` — protocol + escaping, fully
documented in the header. `System_LLMBackend.{h,cpp}` — the registry.

The firmware is the UART **server**; you are the client. Firmware cannot call
you and block — it pushes an EVT frame and waits for you to come back with
commands. That is the same shape as the shipped power/fan machines.

---

## Wire protocol

### Firmware → CM5 (EVT frames, via `route_link_event`)

```
llm_select <escaped-model-name>
llm_ask <session> <maxTokens> <tempX100> <toppX100> <escaped-prompt>
llm_cancel <session>
```

`<session>` is a positive int minted per generation by the firmware. Echo it
back on every push/end. `tempX100`/`toppX100` are integers (0.7 → `70`).

### CM5 → firmware (authenticated commands, via `session.command`)

```
cm5 llm models <gen> <idx> <count> <sizeMB> <name>
cm5 llm ready  <gen> <name>
cm5 llm push   <session> <seq> <escaped-text>
cm5 llm end    <session> <ok|error|stopped> [tokens] [tokPerSecX10]
```

- `<gen>` — a catalog generation counter you own. Bump it whenever the model
  set changes. Firmware resets its table when `gen` changes, so a shrinking
  catalog cannot leave stale rows.
- `models` — send one line per model, `idx` 0-based, `count` = total. Firmware
  caps at **8**; extra rows are dropped with a log line, not silently.
- `ready` — send after a `llm_select` completes AND `/health` is green. The
  firmware treats you as authoritative and adopts the name you report, so if you
  resolved to a different file, say so here. **Until this arrives the firmware
  shows the model as LOADING and refuses to generate.**
- `push` — one per flushed delta group. `seq` starts at **0** per session and
  increments by one. Firmware is idempotent by seq: a replay of an already-
  applied seq is accepted and ignored; a *gap* aborts the generation.
- `end` — always send one, including on error/cancel. **A lost `end` is the
  worst failure mode**: the firmware has a 45 s stall timeout that will
  eventually abandon the turn, but every surface shows a hung answer until then.

Firmware replies `OK …` or `ERROR <reason>` to each. The line is consumed by an
intrinsic *before* `cmd_exec`, so pushes never take the command lock and never
enter the durable command audit.

---

## Escaping — get this exactly right, both directions

The firmware's inbound line handler **trims the line** before dispatch, and
streamed deltas carry their inter-word space at exactly the chunk boundary. So
rather than protect only the edges, **all whitespace is escaped** and nothing
can damage a chunk:

| raw | wire |
|---|---|
| `\` | `\\` |
| LF | `\n` |
| CR | `\r` | 
| TAB | `\t` |
| space | `\s` |

Unknown escapes decode to the character itself (so adding one later degrades to
readable text, not a hole). Apply to `<name>`, `<escaped-text>` — and decode the
same way on `llm_select` / `llm_ask` payloads coming *from* the firmware.

Reference implementation to mirror exactly: `cm5LlmUnescape` / `cm5LlmEscape` in
`System_LLMCm5.cpp`.

---

## Three gotchas that will bite you

**1. `route_link_event` decodes strict ASCII and drops the payload.**
`jobs.py:250` does `payload.decode("ascii")` and returns on `UnicodeDecodeError`
with only a `log.warning`. One curly apostrophe from a phone keyboard silently
loses the whole prompt with no error and no timeout on either side. Give
`llm_ask` its own branch that decodes **UTF-8 with `errors="replace"`**, and
split with `maxsplit` so the prompt remainder is taken raw rather than
whitespace-tokenized.

**2. Use `replay=False, auth_replay=False` on every push.**
`Session.command` defaults to re-login-and-replay on timeout
(`link/session.py:181`). Pushes are **append deltas** — a replay duplicates
answer text in the user's chat. `auth_replay=False` also raises `LinkClosed`
instead of re-logging-in, which is correct here because the session epoch is
bound into the firmware's fence.

**3. Per-token push is not viable.** The firmware admits at most one line per
loop lap and you serialize behind one `asyncio.Lock`. Use the shipped EvenAI
cadence as precedent: `_STREAM_FLUSH_CHARS = 140`, `_STREAM_PART_BYTES = 200`
(`pipeline.py:55-57`), flushing on sentence ends or at the char limit on a word
boundary. Use `expect="status"` so you do not pay the 150 ms `QUIET_GAP_S`.

---

## Implementation shape

Follow the **fan/power module pattern**, which already solves "EVT arrives on
the loop thread, work is async":

- `submit_event(payload: bytes) -> bool` — synchronous parse + enqueue, never
  does I/O, returns `True` if it consumed the payload. Wire it into
  `route_link_event` alongside `fan.submit_event` / `power.submit_event`
  (`jobs.py:246-250`), **before** the ASCII decode.
- A worker task drains the queue and does the async work.

The pieces you need already exist:
- `llm/client.py` → `LlmClient.ask_stream(prompt, commit_history=...)` yields
  deltas. Use it directly.
- `llm/server.py` → `LlamaServerSupervisor`. Model switching = change
  `cfg.model`, `stop()`, `start()`, wait `/health`, then send `cm5 llm ready`.
  Note `_args()` currently hardcodes a single `--model` from config — that is
  what `llm_select` has to change.
- Enumerate models from the configured model directory (glob `*.gguf`); push the
  catalog on link-up and after any change.

**Honour the params** from `llm_ask` (`maxTokens`, `temperature`, `top_p`) by
passing them into the request body — the firmware clamps them centrally, and
silently ignoring them voids the shared override contract that web/BLE rely on.
`llm_ask` deliberately carries **no** suppress-token list; those are ids from the
device's own tokenizer and mean nothing to you.

---

## Tests to add (`ai-service/tests/`)

There is an existing `fake_firmware.py` harness — extend it rather than mocking
the transport. Minimum bar:

1. **Round-trip escaping** — property-style: `unescape(escape(s)) == s` for
   strings containing newlines, tabs, runs of spaces, backslashes, and non-ASCII.
2. **A prompt with a curly apostrophe survives** `llm_ask` end-to-end (this is
   gotcha #1 and it is currently silent).
3. **Deltas reassemble byte-exactly**, including the leading space on a
   mid-sentence chunk — the whole point of the escaping.
4. **`end` is sent on the error path** (llama-server 500, cancel, supervisor
   restart mid-answer).
5. **`seq` is contiguous from 0** and a forced retry does not duplicate text.
6. **`llm_select` on a nonexistent model** does not leave the firmware in
   LOADING forever. NOTE: as originally written this test asked for something
   the protocol cannot express — there is no failure verb (see the 2026-08-17
   update at the top). Test instead that your failure path sends
   `cm5 llm ready <gen> <fallback-model>`, and rely on the firmware's 120 s
   select timeout as the backstop rather than the reporting mechanism.

---

## What NOT to do

- Don't send a `ready` before `/health` is green — the firmware will start a
  generation and it will fail.
- Don't reuse `<session>`; the firmware fences on it and will reject stale lines.
- Don't send the local `Q:/A:` framing — the firmware deliberately does **not**
  frame remote prompts, because you apply your own chat template and system
  prompt. The prompt arrives raw and should stay that way.
- Don't add history on the firmware side of the wire; history lives in
  `LlmClient` (see its docstring) and that stays true here.
