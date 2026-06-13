# HardwareOne — On-Device LLM Subsystem Report

**Purpose:** complete map of the on-device LLM system for a focused engineering session.
**Scope:** firmware only (ESP-IDF, `components/hardwareone/`). Android app is a separate concern.
**As of:** 2026-06-12. Build target `esp32s3` + PSRAM. Gated by `ENABLE_ONDEVICE_LLM`.

> **Headline open problem:** a single `forward()` pass (one token) on the current model
> exceeds the 5 s task-watchdog window, so generation trips `task_wdt` on `IDLE1`/`llm_gen`
> on core 1. Per-token yielding is already in place and is **not** sufficient — the fix must
> yield *inside* `forward()`. See §10. This is the reason this report exists.

---

## 1. Build gating & target

- Master switch: `System_BuildConfig.h` → `#define ENABLE_ONDEVICE_LLM 1` (line ~225).
  Requires ESP32-S3 + PSRAM. CMake excludes LLM sources on non-S3 targets.
- Context cap: `LLM_MAX_CONTEXT_LEN` (default 1024) — only compiled when the feature is on.
- WDT config (sdkconfig): `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, `CHECK_IDLE_TASK_CPU0/1=y`,
  `CONFIG_FREERTOS_HZ=1000` (so `vTaskDelay(1)` ≈ 1 ms).
- **App-partition headroom is tight:** with LLM on, the app binary is ~95% of the 0x4d5000
  partition (~5% / ~240 KB free). Adding large features will overflow; bump the partition
  table if needed (16 MB flash has room). Model files live on LittleFS/SD, not in the binary.

## 2. File map

| File | Lines | Responsibility |
|---|---|---|
| `System_LLM.h` | ~216 | Public API, `LLMConfig`/`LLMStatus`/`LLMGenParams` structs, constants, LLM1 format magic |
| `System_LLM.cpp` | ~4293 | **The engine.** Model load, forward pass, sampling, tokenizer (BPE), sync+async generation, CLI commands, settings module |
| `System_LLMChat.h` | ~158 | Conversation-layer API (turns, streaming cursor, param resolution) |
| `System_LLMChat.cpp` | ~504 | Turn ring buffer, drain-on-read streaming, param resolution/clamping. Shared by web + OLED + BLE |
| `WebPage_LLM.h` | — | Embedded Chat web page (HTML/JS) |
| `WebPage_LLM.cpp` | ~438 | HTTP `/api/llm/*` handlers (the reference protocol the app mirrors) |
| `OLED_Mode_LLM.cpp` | ~491 | On-device OLED chat UI (model picker, keyboard, render) |

## 3. Model format — "LLM1" (`esp32-llm-converter`)

- Magic `0x4C4C4D31` ("LLM1"), `file_version` 2 or 3. Tokenizer embedded in the model file.
- Forward pass derived from Karpathy's `llama2.c`. Supports **Llama** (`arch_type=0`) and
  **GPT-2** (`arch_type=1`).
- Quantization (`quant_type`): `0=FP32`, `1=INT8`, `2=INT4_MIXED` (INT8 layers at front/back
  via `n_q8_start`/`n_q8_end`, INT4 in the middle). `group_size` sets the quant group.
- `LLMConfig` (header): `dim, hidden_dim, n_layers, n_heads, n_kv_heads (GQA), vocab_size,
  seq_len, quant_type, group_size, arch_type, n_q8_start, n_q8_end, file_version`.
- Models load from `/system/llm/` (LittleFS) or `/sd/llm/` (SD). Default `/system/llm/model.bin`.

## 4. Core data structures (`System_LLM.cpp`)

- `gLLM` (line ~288, the singleton runtime): holds `runState` (`LLMState`), `config`
  (`LLMConfig`), `weights` (`TransformerWeights`), `state` (`RunState`), `tokenizer`
  (`TokenizerState`), `seq_ctx` (runtime KV context, auto-reduced to fit PSRAM),
  `requestedMaxCtx`, `stopRequested`, `errorMsg`, `lastTokPerSec`, etc.
- `TransformerWeights` (~132): all weight tensors (FP32 ptrs + INT8 `*_i8`/`*_sc` scale
  variants + `layer_quant[]` per-layer quant map; `wcls`/`wcls_i8`/`wcls_sc` classifier).
- `RunState` (~194): activation scratch — `x, xb, xb2, hb, hb2, q, key_cache, value_cache,
  att, logits`. KV caches are the big PSRAM consumers (`2 * n_layers * seq_ctx * kv_dim * 4`).
- `TokenizerState` (~230): merge-based BPE — `vocab[]` string pool, `MergeLookup` hash map,
  `PreSplitToken[]` (special/added tokens matched whole before BPE).
- `LLMGenParams` (header ~165): one generation's knobs incl. `suppressTokens[128]` (retry steering).
- `LLMAsyncContext gLLMAsyncCtx` (~302): prompt+params handed to the background task.
- Streaming globals (~296–300): `gLLMResultBuf` (8 KB PSRAM, `LLM_RESULT_BUF_SIZE`),
  `volatile gLLMResultLen`, `volatile gLLMResultDone`, `volatile gLLMSessionId`, `gLLMTask`.

## 5. Engine internals — function inventory (`System_LLM.cpp`)

**Math / forward:**
- `rmsnorm` (356), `layernorm` (366), `softmax` (381), `matmul` (394), `wmatmul` (481, the
  quant-aware matmul dispatch), `scaleCount` (405), `vecstats` (496, debug).
- `forward(token, pos)` (**537**) — the hot path. Embedding lookup/dequant → **per-layer loop
  `for (int l=0; l<n_layers; l++)`** (line **629**: rmsnorm/layernorm → QKV `wmatmul`s →
  RoPE/attention → output proj → FFN `wmatmul`s → residuals) → final **classifier `wmatmul`
  into logits** (line **962**, `dim × vocab_size`). **No `vTaskDelay`/yield anywhere inside.**

**Sampling:** `sample_argmax` (1008), `sample_topp` (1020, nucleus), `sample` (1097, applies
temperature, dynamic-temp, Mirostat-2, top-p, repetition penalty, content-token boosting).

**Tokenizer:** `loadTokenizerFromFile` (1265), `encode` (1470, BPE), `decode` (1596),
`mergeLookup`/`mergeMapInsert`, `freeTokenizer`.

**Model load:** `parseModelHeader` (1802), `validateLlmConfig` (1621), `prescanTiedWeights`
(1857, tied wcls=embedding), `computeMemoryLayout` (1931, auto-fits `seq_ctx` to PSRAM
budget — `LLM_PSRAM_RESERVE_BYTES`), `allocateRunState` (2192), `readTensor`/`readChunked`/
`tensorFileSize`, `spotCheckWeights` (2121), `loadWeights` (2281), `llmLoadModel` (2801).

**Lifecycle / status:** `llmInit` (2795), `llmUnload` (2842), `llmIsReady` (2885),
`llmGetStatus` (2889), `llmStop` (3765), `llmTokenize` (3769), `llmListModels` (3775).

**Generation:**
- `llmGenerate(...)` (**2906**) — synchronous. Sets `runState=GENERATING` (2910), prompt
  normalization + tokenize, then the **autoregressive loop `while (pos < steps)`** (~3287):
  `forward()` (3290) → `sample()` → stop checks (EOS/Q:/sentence-limit/hard-cap/Do-mode) →
  `tokenCb(piece)` → repetition ring update → **per-token yield** `if (generated %
  YIELD_INTERVAL==0) vTaskDelay(1)` (~3723, `YIELD_INTERVAL=1`) → restore `READY`.
- Async: `llmStartAsync` (3832) spawns `llmAsyncTask` (308) — `xTaskCreatePinnedToCore(...,
  LLM_TASK_STACK_SIZE=16KB, LLM_TASK_PRIORITY=3, core 1)`. The task calls `llmGenerate` with
  a callback that appends tokens to `gLLMResultBuf` and publishes `gLLMResultLen`.
- Poll API: `llmGetResultChunk(offset,buf,len)` (3878), `llmGetResultLen` (3888),
  `llmIsGenerationDone` (3889), `llmGetSessionId` (3890). The result buffer **persists after
  done** until the next `llmStartAsync` — important for last-chunk reads.

## 6. Conversation layer (`System_LLMChat.*`)

Centralizes "a conversation" (was duplicated in web JS + OLED). One turn ring
(`LLM_CHAT_MAX_TURNS=16`, `LLM_CHAT_TURN_MAX_BYTES=2048`/turn, bodies in PSRAM), one streaming
turn, one param resolver. Mutex-guarded; all readers safe from any task.

- `chatBeginTurn(prompt, opt)` — append USER + empty ASSISTANT turn, `chatResolveParams` from
  `gSettings.llm*` + overrides + clamps, then `llmStartAsync`. Returns engine session id (>0)
  or 0 (busy/not-ready). **This is the entry the web and BLE both use.**
- `chatRetryLast`, `chatStop`, `chatClear` (refused mid-gen).
- Readers: `chatGetTurnCount`, `chatGetTurnInfo`, `chatReadTurn(idx,off,buf,len)`,
  `chatGetStreamLen`, `chatReadStream`, `chatIsGenerating`, `chatGetSessionId`.
- **Drain-on-read:** readers call `drainEngineLocked()` first, copying new bytes from the
  engine buffer into the live turn — no separate poll task. **Gotcha:** the streaming cursor
  (`chatGetStreamLen`/`chatReadStream`) goes to 0 the instant the turn finalizes, so the BLE
  `llmresult` command reads the **engine** buffer directly instead (persists after done).

## 7. Surfaces

### 7a. CLI / BLE commands (`llmCommands[]`, registered ~4259)
All now return **web-identical JSON** for app parity (recently aligned):
- `llmstatus json` → `{v,state,model,tokPerSec,error,psramKB,contextUsed,contextMax,tokens}`
- `llmmodels json` → `{v,models:[names]}`
- `llmgenerate json <prompt>` → `{v,ok:true,session:N}` (async start; leading `json` is the
  mode flag, rest is the prompt) / `{v,ok:false,error}`
- `llmresult json <offset>` → `{v,text,done,len}` (poll loop; 512-byte window; reads engine buf)
- `llmstop` / `llmclear` / `llmload <name>` / `llmunload` → `{v,ok:true}` / `{v,ok:false,error}`
- `llmretry` → `{v,ok:true,session:N}` ; `llmturns json <index>` → one turn per call (BLE 4 KB cap)
- Plus 13 persisted setting commands: `llmtemperature, llmtopp, llmmaxtokens, llmsentencelimit,
  llmhardcap, llmreppenalty, llmrepwindow, llmmaxcontext, llmusemirostat2, llmmirostattau,
  llmmirostateta, llmdyntemp, llmdefaultmodel`.

### 7b. Web `/api/llm/*` (`WebPage_LLM.cpp`) — the protocol of record
`status, models, load, unload, generate, stop, result, chat/turns, chat/retry, chat/clear`
(handlers `handleLLM*`). The async flow: `POST generate → {ok,session}`, then poll
`GET result?offset=N → {text,done,len}`. Chat page served by `handleLLMPage`.

### 7c. OLED (`OLED_Mode_LLM.cpp`)
On-device chat UI: model picker (`openModelPicker`/`onLLMModelPicked`→`llmLoadModel`),
on-screen keyboard, `syncStateFromEngine`, render states (no-model/loading/chat),
`isLLMAvailable`. Polls the same chat layer.

## 8. Settings (`gSettings.llm*`, persisted; `llmSettingsModule`)
`llmTemperature(0.5) llmTopP(0.8) llmMaxTokens(256) llmSentenceLimit(2) llmHardCap(80)
llmRepPenalty(1.3) llmRepWindow(32) llmMaxContext(0=auto) llmUseMirostat2(false)
llmMirostatTau(5.0) llmMirostatEta(0.1) llmDynTemp(false) llmDefaultModel("model.bin")`.

## 9. Threading & memory model
- **`llm_gen`**: priority 3, **pinned core 1**, 16 KB stack. Arduino `loopTask` (prio 1) and
  `cmd_exec_task` (prio 1, unpinned) also run on core 1 → `llm_gen` outranks them.
- Web HTTP server is effectively on the other core, which is why the **web survives** heavy
  generation while **BLE/serial command latency suffers** (cmd_exec starved on core 1).
- Memory: weights + KV cache in **PSRAM** (current "HardwareOneHelpAgent" model ≈ **6.7 MB**,
  PSRAM ~95% used). DRAM is fine when WiFi is off (~90 KB free). `seq_ctx` auto-reduces to fit
  `LLM_PSRAM_RESERVE_BYTES`. `gLLMResultBuf` = 8 KB PSRAM.

## 10. Known issues & current state

### (A) ★ Watchdog: a single `forward()` exceeds 5 s — **OPEN, the priority item**
- Symptom: `task_wdt got triggered … CPU 1: llm_gen`, repeating every 5 s during generation;
  multi-second/100 s+ `LOOPHEALTH` stalls.
- **Already done (insufficient):** generation-loop yield changed from every-4-tokens to
  **every token** (`YIELD_INTERVAL=1`, ~3723). The WDT still fires → conclusion: **one token's
  `forward()` pass alone is > 5 s** (≈6.7 MB of PSRAM-resident weights streamed per token).
  Yielding *between* tokens cannot help.
- **Required fix (designed, NOT yet applied):** yield *inside* `forward()` so `IDLE1` runs
  within a single pass. Concretely:
  - Add `vTaskDelay(1)` at the **top of the per-layer loop** (line **629**) — bounds the
    uninterrupted span to ~one layer's matmuls.
  - Add `vTaskDelay(1)` **before the classifier `wmatmul`** (line **962**) — the `dim ×
    vocab_size` op can be the single largest, especially with a big vocab.
  - Cost ≈ `(n_layers+1)` ms/token at 1 kHz tick — negligible vs multi-second tokens.
  - If a *single layer* or the classifier still exceeds 5 s, escalate: yield **inside
    `wmatmul`** (chunk the output-row loop and `vTaskDelay`/`esp_task_wdt_reset` every K rows),
    and/or raise `CONFIG_ESP_TASK_WDT_TIMEOUT_S`. Note: resetting the WDT only helps if the task
    is subscribed; the real lever is letting `IDLE1` run (i.e., actually yielding).
- **Secondary lever:** consider lowering `LLM_TASK_PRIORITY` 3→1 so core-1 time-slices fairly
  with loop/cmd_exec (improves OLED/BLE responsiveness during gen). Does *not* fix the WDT by
  itself (IDLE is prio 0) — must combine with in-`forward` yields. Validate on HW.
- **Model angle:** 6.7 MB is very large for S3. A smaller/more-quantized model would cut
  per-token time dramatically. Firmware should still be robust to slow models regardless.

**Symptom variant — "llmgenerate blocks ~20 s / no session reply" (same root cause as A):**
An app-side BLE trace showed `llmgenerate json` returning `{ok:true,session:N}` only ~20 s
after sending, after generation already finished. **This is NOT a sync regression** —
`cmd_llm_generate` (System_LLM.cpp:4045) still calls the async `chatBeginTurn`; the `{session}`
is produced in ms. What's delayed is **transmitting** it: `chatBeginTurn` spawns `llm_gen`
(prio 3, core 1) which preempts `cmd_exec_task` (prio 1, core 1) *before* it pushes the reply
out BLE, and `llm_gen` won't yield for >5 s (one forward). So the reply can't be sent until
generation ends. Same fix (in-`forward()` yields). Lowering `LLM_TASK_PRIORITY` would also let
cmd_exec send the reply promptly. (An earlier build "worked" only because forward was ~0.9 s/
token then, so the reply made the app's 5 s window. `ok:true` is a printf-only change and is
unrelated to the timing.)

### (E) Console/audit broadcast leaks into the BLE response channel — OPEN, secondary
Every command's audit line `[CMD] user@src: cmd -> OK result` is broadcast with
**`MSG_ROUTE_ALL`** at **`System_Utils.cpp:964`** (`broadcastOutputCore_Routed`), which includes
the BLE notify characteristic. So BLE replies arrive interleaved with `[CMD] …` lines (observed:
`{"v":1,"ok":true,"session":3}[CMD] dev@bluetooth: llmstatus json -> O…`). Pre-existing, not
LLM-specific; the app tolerates it (parser stops at `}`). Fix direction: don't route the
audit/console broadcast onto the BLE *response* characteristic (separate response vs console
streams, or suppress the echo back to the originating BLE session). Decide in the focused session.

### (B) App streaming UX — app-side, not firmware
The Android app stops polling `llmresult` on empty `text:""` and falls back to `llmstatus`,
so the bubble never fills. Firmware is correct (proven: tokens stream when polled). App must
loop `llmresult json <offset>` until `done:true`, ignore empty `text`, and not gate on
`llmstatus`. (Documented separately for the app session.)

### (C) JSON parity — DONE
All `llm*` commands now return the same JSON shapes as `/api/llm/*` (incl. `ok:true`). This
removed the "streaming isn't supported" false trigger (which ultimately came from the non-JSON
`"Unknown command…"` fallback at `System_Command.cpp:262`).

### (D) Heap/partition — watch, not blocking
App partition ~5% free with LLM on; DRAM tight only with WiFi on. Not the cause of (A).

## 11. Suggested focus for the dedicated session
1. Implement the in-`forward()` yields (§10A) and measure: does `task_wdt` stop? tok/s impact?
2. If single ops still >5 s, chunk-yield inside `wmatmul` (the row loop) — most robust.
3. Re-evaluate `LLM_TASK_PRIORITY` and core pinning for responsiveness during generation.
4. Validate end-to-end over BLE with the corrected app polling loop.
5. Consider a smaller default model and/or surfacing per-token latency in `llmstatus`.

## 12. Build / flash quick-reference
- Build (avoid the cwd trap — always use `-C`): `idf.py -C /Users/morgan/esp/hardwareone-idf build`
- Flash (no erase needed; partition table unchanged): `idf.py -p <PORT> flash`
- Erase only for a clean factory state (wipes NVS/users): `idf.py -p <PORT> erase-flash`
