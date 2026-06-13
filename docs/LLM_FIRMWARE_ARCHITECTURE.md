# On-Device LLM — Firmware Architecture (Ground Truth)

**Scope:** how the HardwareOne ESP32-S3 firmware loads and runs the tiny on-device
LLM. Written to refresh tools/AIs whose mental model predates the 2026 engine
refactor and the sampler/recovery work. **Firmware-side ground truth** — where a
secondhand report and this doc disagree, this doc (and the code it cites) wins.

Code lives in `components/hardwareone/`. Reference everything by **file + symbol**;
line numbers drift. Last updated: 2026-06-13.

---

## 0. The one thing to get right: the device eats ONE self-contained file

The device loads a single `model.bin` (default `/system/llm/model.bin` on LittleFS,
or `/sd/llm/…` on SD). It is the **LLM1** container format and it has **two parts
baked in**:

1. **Weights** (quantized transformer tensors).
2. **The tokenizer** (BPE vocab + merges + special tokens), embedded as a section
   inside the same file.

**Training data (`.txt`) never goes to the device, and there is no second file.**
The vocabulary/special-tokens that *derive* from the corpus do travel to the device
— but only as the *embedded tokenizer inside `model.bin`*, not as raw data. The
device needs the tokenizer to encode prompts and decode outputs, and it reads it
straight out of `model.bin` at load.

```
training .txt + special-tokens ─► [Python training] ─► safetensors + tokenizer.json
        (stays on PC)                                          │
                                                               ▼
                                       [converter] ─► model.bin  ─► device
                                                      (weights + tokenizer, ONE file)
```

So any plan that proposes shipping a second "training/tokenizer" file *to the
device* is solving a non-problem. Keeping the corpus paired with the model for
PC-side reproducibility is fine — but that's a host concern, off-device.

> Note: a **fact/retrieval table** (Phase 4 of the improvement plan) *would* be a
> separate on-device asset (e.g. `/system/llm/facts.json`), read from flash on
> demand. That is **not built yet** and is unrelated to the tokenizer.

---

## 1. Module layout (post-refactor)

The old ~4300-line monolith was split into focused translation units. Find symbols
within these:

| Area | File | Key symbols |
|------|------|-------------|
| Engine core | `System_LLM.cpp` | `forward()`, `llmGenerate()`, `llmAsyncTask`, `cmd_llm_generate`, settings table |
| Public API / config | `System_LLM.h` | `LLMConfig`, `LLMStatus`, `LLMGenParams`, `llmGenerate()` sig, `LLM_DEFAULT_*` |
| Private structs | `System_LLM_Internal.h` | `LLMRuntime gLLM`, `RunState`, `TransformerWeights`, `llmBindRunState()`, `setLlmError()` |
| Compute kernels | `System_LLM_Kernels.{h,cpp}` | `rmsnorm/layernorm/softmax`, `matmul_q8/q4`, `linear()`, `vecAddInPlace()` |
| Sampling | `System_LLM_Sampler.{h,cpp}` | `sample()` (temp/top-p/min-p), `sample_mirostat2()` |
| Model loader | `System_LLM_Model.{h,cpp}` | `loadWeights`, `buildLayerTensors`, `allocateRunState`, `llmBindRunState()` |
| Tokenizer | `System_LLM_Tokenizer.{h,cpp}` | `loadTokenizerFromFile`, `encode`, `decode`, BPE |
| Unified chat caller | `System_LLMChat.cpp` | `chatBeginTurn`, `chatResolveParams` (web + BLE + OLED all enter here) |

---

## 2. The model (current shipping config: `HardwareOneHelpAgent.bin`)

- **Arch:** GPT-2 (`arch_type == 1`). **Absolute learned positional embeddings**
  (added in `forward()` from `pos_embedding_table`). The loader has RoPE scratch
  for the Llama path, but **RoPE is inert for this GPT-2 model** — don't "optimize"
  it expecting a speedup.
- **Quant:** INT8 (`quant_type==1`). Engine also supports FP32 (0) and
  INT4_MIXED (2 — INT8 front/back layers, INT4 middle).
- **Dims:** `dim=192`, `hidden=512`, `n_layers=16`, `n_heads=6`, `n_kv_heads=6`
  (no GQA), `vocab=3282`, `group_size=128`. Tied weights (lm_head == embedding).
- **Special tokens:** `Q:`=3, `A:`=4, `Do:`=5. EOS for GPT-2 = **0**
  (`EOS_TOKEN_GPT2`). Generation also stops if the model emits `Q:`(3).
- **Presplit command tokens:** many device command names are **single vocab
  tokens** trained in (e.g. `openwifi`=19, `ntpsync`=105, `oledmode`=57). The
  tokenizer has a presplit table (~34 entries) that emits these directly. This
  matters for any constrained "Do:" grammar — most commands need no multi-token
  trie.

### Context is auto-fit — runtime ≈ 41, NOT seq_len 128

`seq_len=128` is the model's trained max, but the **runtime KV-cache context is
PSRAM-capped and auto-shrinks** at load. On the FeatherS3 it lands around
**`ctx≈41`** (`gLLM.seq_ctx`). `allocateRunState()` even has a fragmentation
fallback that shrinks ctx further if the largest free PSRAM block can't hold the
cold state. **Any design that assumes a 128-token window (long injected context,
multi-fact prompts) is wrong** — prompt + answer + anything injected all compete
inside ~41 tokens. Generation steps are capped: `steps = min(maxTokens + prompt, seq_ctx)`.

---

## 3. Memory & tasks (why it's tight)

- **PSRAM (~8 MB):** holds the weights (~6.3 MB Q8) + the "cold" run state
  (KV cache + attention + logits). `LLM_PSRAM_RESERVE_BYTES` keeps a margin free.
- **Internal DRAM (~tight):** the "hot" run state (activation scratch: `x`, `xb`,
  `xb2`, `q`, `hb`, `hb2`, RoPE scratch) is allocated in **internal RAM** for speed,
  falling back to PSRAM only if internal alloc fails. Free DRAM runs low
  (a `HEAP_PRESSURE` warning fires under ~40 KB; it routinely sits in the 20s of KB
  during generation). This tightness is the backdrop for the corruption-recovery
  path in §5.
- **Bandwidth-bound:** generation streams ~7.3 MB of INT8 weights per token →
  ~1.6–1.7 tok/s. It's PSRAM-bus-bound; the 64B data-cache line gave ~+13%.
  Software prefetch is impossible on S3 (no `dpf`). INT4-everywhere was rejected
  on quality. The real 2× is octal PSRAM (XIAO), not the primary FeatherS3.
- **Tasks:** generation runs on **`llm_gen`**, `xTaskCreatePinnedToCore(..., core 1)`,
  16 KB stack, priority `LLM_TASK_PRIORITY`. Command execution (`cmd_exec_task`) is
  pinned to **core 0** so BLE/web replies don't starve behind `llm_gen` on core 1.

---

## 4. Load flow (`llmLoadModel` → `System_LLM_Model.cpp`)

1. Parse the LLM1 header → fill `gLLM.config`.
2. Compute `seq_ctx` (auto-fit to PSRAM budget).
3. `loadWeights()` — stream tensors into PSRAM (per-layer, Q8/Q4 as configured).
4. `buildLayerTensors()` — build `QuantTensor` views per layer + classifier for
   `linear()` dispatch.
5. `allocateRunState()` — allocate the two base blocks (`stateHotData` internal-RAM,
   `stateData` PSRAM) then call **`llmBindRunState()`** to carve all `RunState`
   pointers (`s->x … s->logits`) out of them.
6. Load the embedded tokenizer.

`llmBindRunState()` is the **single source of truth** for state-pointer binding —
used at load and again as a recovery step (§5).

---

## 5. Generation pipeline (`llmGenerate` → `System_LLM.cpp`)

Per turn:

1. **Prompt normalization** (in `forward()`'s caller path):
   - **Framing restore:** BLE strips `\r\n`, so `"Q: … A:"` / `"Q: … Do:"` get the
     trailing newline restored to `"\nA:"` / `"\nDo:"` so the right special token
     is the last prompt token.
   - **Casing fix (Phase 0):** title-cases the first letter of the question body
     (Q&A path only — `\nDo:` mode is skipped). Lowercase question words tokenize
     differently (`who`=1558 vs `Who`=2387) and answer worse; this matches the
     title-cased training format. Fires only on lowercase input → well-cased web
     prompts stay byte-identical.
   - Filler detection is **log-only** (no stripping — stripping starved the KV cache).
2. **Encode** prompt → token ids (embedded BPE).
3. **Forward loop** (`while pos < steps`): `forward(token,pos)` → logit
   post-processing → sample → stop checks. Logit stage includes: repetition
   penalty (content-exempt ring buffer), suppress penalty (retry), prompt-content
   logit boost, sentence-aware temp/top-p taper, optional dynamic temperature.
4. **Sampling** (`sample()`): temperature → softmax → **min-p OR top-p** →
   categorical. Mirostat v2 is a separate path. Defaults: `temp=0.5`, `topp=0.8`,
   **`minp=0` (off → top-p)**. Note: on this model's peaked factual distributions,
   `top_prob` is often 1.000, so min-p ≈ top-p there.
5. **Stop conditions:** EOS(0), `Q:`(3), sentence limit (2), hard cap (80),
   `Do:`-mode early stop on explanation tokens, user stop.
6. **Confidence signal (Phase 2):** `sample()` reports the chosen token's
   post-softmax probability; the loop accumulates mean log-prob and exposes it on
   `LLMStatus.lastMeanLogprob` / the status JSON (`meanLogprob`) and the run summary
   log (`conf=…`). Pure observability — it does **not** change output, and greedy
   (`temp==0`) yields no signal (no softmax). It catches open-domain *flailing*,
   **not** confident-wrong answers.

### Corruption guard + soft recovery (new)

Under DRAM pressure a stray overrun can zero a `RunState` pointer (observed:
`s->x → null` mid-generation → `LoadProhibited` panic/reboot in `vecAddInPlace`).
Now:
- `forward()` **null-checks the critical `RunState` pointers on entry** and returns
  `nullptr` instead of dereferencing.
- The generation loop, on `nullptr`, calls `llmBindRunState()` to **re-derive the
  pointers from the surviving base blocks** and **retries the same position** (KV
  contents for prior positions remain valid), up to **2 times**.
- If recovery fails, it sets a clean error + `LLMState::ERROR` and returns partial
  output — **no device reboot.**

This is a **safety net, not a cure** — it does not fix the underlying overrun.

---

## 6. Settings & entry points

- Runtime knobs persist in **NVS** via a `SettingEntry` table in `System_LLM.cpp`;
  `chatResolveParams()` (in `System_LLMChat.cpp`) resolves them into `LLMGenParams`
  (temp, topp, **minP**, rep penalty, mirostat, etc.). Add a tunable in three
  places, mirroring `topp`: a `#define LLM_DEFAULT_*`, the `llmGenerate` signature,
  and the `LLMGenParams` struct.
- **All callers converge on `chatBeginTurn` / `chatResolveParams`** — web, BLE, and
  OLED. Any per-turn feature (e.g. a future retrieval dispatcher) belongs there so
  every transport gets it.
- Generation is async: `llmStartAsync()` spawns `llm_gen`; results stream via a
  token callback and a PSRAM result buffer the chat/status path polls.

---

## 7. Quick "don't get burned" list

- One file to the device: **`model.bin`**, tokenizer baked in. No training data,
  no second tokenizer file, on-device.
- **Context ≈ 41**, not 128. Budget prompt + answer + anything injected accordingly.
- GPT-2 absolute pos-emb → **RoPE does nothing here**.
- Many commands are **single tokens** (presplit table).
- The model is **confidently wrong** on facts (peaked distribution at a wrong
  value) — confidence/entropy can't detect that; only forcing values at decode
  (logit masking) or answering from a fact table is reliable. (Retrieval/fact-table
  = planned, not built.)
- Default-off features are **byte-identical** to prior behavior (min-p, casing only
  fires on lowercase, confidence is read-only).
