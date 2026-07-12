# LLM group_size=64 re-quant — change report

**Goal:** move every weight matmul onto the *aligned fast path* in
`matmul_q8` by re-quantizing models at `group_size=64` instead of `128`.

> **REVISION (2026-07-12, after the INT16-activation path landed + was HW-tested):**
> The scalar speed case for gs=64 has **largely evaporated.** It was originally
> sized against the *old FP32-activation fallback*, which did 2 float-mults per
> weight. The `LLM_Q8_INT16_ACT` kernel rewrote that fallback to accumulate in
> int32 per constant-scale run (scale applied once per group, not per element),
> so the INT16 fallback is now within <1% of the INT16 aligned path — the gap
> gs=64 was meant to close is already closed. gs=64 also adds ~3% scale-byte
> traffic, which roughly cancels the tiny bookkeeping win (could even be a hair
> slower). **Do not reconvert for scalar speed.** gs=64's remaining value is:
> (1) it is the clean-alignment **enabler for the future SIMD backend** (64
> divides both dims *and* the vector width → aligned SIMD on 100% of matmuls,
> no ragged remainder), and (2) a marginal quantization-quality bump. **Bundle
> the reconvert with the SIMD step, not before it.** The mechanics below still
> hold; only the speed justification changed.

**One-line verdict:** this is a **converter + redeploy change, not a firmware
change.** The device already reads `group_size` from the model header and
computes every derived quantity dynamically. No `.cpp` edits are required to
*load and run* a gs=64 model. The work is (1) re-convert the models, (2)
redeploy the `.bin`s, (3) measure.

---

## 1. Why this helps (the actual finding)

`matmul_q8` in `System_LLM_Kernels.cpp` has two branches:

- **Fast path**, taken only when `n % group_size == 0`: the per-group scale is
  pulled out of the inner loop (≈1 float multiply per weight).
- **General fallback**, taken otherwise: a per-element scale lookup and an extra
  float multiply per weight (≈2 multiplies per weight).

The shipping model is `dim=192`, `hidden=512`, `group_size=128`. Because
`192 % 128 = 64 ≠ 0`, the matmuls whose **input dim is 192** fall into the slow
fallback:

| Matmul | input dim `n` | `n % 128` | `n % 64` | Path today (gs=128) | Path at gs=64 |
|--------|--------------|-----------|----------|---------------------|---------------|
| wq, wk, wv | 192 | 64 | 0 | **fallback** | fast |
| wo (attn out) | 192 | 64 | 0 | **fallback** | fast |
| w3 (FFN up) | 192 | 64 | 0 | **fallback** | fast |
| w2 (FFN down) | 512 | 0 | 0 | fast | fast |
| classifier (tied) | 192 | 64 | 0 | **fallback** | fast |

By weight volume that is **~71% of the per-layer matmul weight on the slow
branch today.** `gcd(192, 512) = 64`, so re-quantizing at **64** puts *every*
matmul (including the tied classifier and the embedding dequant) on the fast
branch.

### Correctness note (why the fast branch is safe here)

The fast branch assumes row `i`, group `g` scale lives at `scales[i*n_groups + g]`.
That equals the true flat index `scales[(i*n + j)/gs]` **iff `n` is a multiple of
`gs`** (then `i*n` is always a multiple of `gs`, so groups never straddle a row
boundary). gs=64 divides 192, 512, and `kv_dim=192`, so the fast branch is not
just faster — it is *exactly* the same arithmetic the fallback computes today.
This is the same reason the code correctly *avoids* the fast branch at gs=128.

---

## 2. What actually changes

### 2.1 Converter (separate repo: `HardwareOne_LLM_Tool`, `index.html`)

- The browser converter already exposes group size as a parameter (the README
  step selects "group size 128"). **Change the selection to 64** and re-run.
- **Confirmed:** the group-size dropdown (`index.html`, `#group-select`) already
  offers `64` (options are 32 / 64 / 128-default / 256). So gs=64 needs **zero
  tool changes** — pick 64, click Convert. No algorithm change: the converter
  emits `ceil(N/gs)` FP32 scales in flat row-major order; the device reads them
  the same way.
- The converter's built-in quality hint recommends group size **≤ dim** and will
  still treat 128 as fine for dim=192 — it has no knowledge of the firmware
  fast-path alignment (our reason for 64), so it won't flag 64 as better. No
  warning fires at 64 either (64 < 192).
- Re-convert from the **existing FP32 training outputs** — no retraining.
  `group_size` is a quantization-time parameter only; training never sees it.
- Converter repo state (checked 2026-07-12): `main`, in sync with upstream,
  conversion path clean; only unrelated training-data generators uncommitted.

### 2.2 Firmware (this repo)

- **No code change to load/run.** `group_size` is parsed from the header
  (`System_LLM_Model.cpp` header parse) and threaded through `n_groups`,
  `tensorFileSize`, `readTensorQ8`, `buildLayerTensors`, and `matmul_q8`
  dynamically. The only validation is `group_size != 0`.
- Optional (not required): a debug log line confirming which matmul branch ran,
  to verify the fast path is actually taken after redeploy.

### 2.3 Models to re-convert (in the tool repo)

Every deployed `.bin` must be re-converted to *get* the benefit (old gs=128
files still load fine, just stay on the slow path):

- `HardwareOneHelpAgent.bin` (shipping)
- Kanto Pokemon, Periodic Table, and any custom user models
- Any INT4_MIXED builds — gs=64 also works for the INT4 pack path
  (`matmul_q4` handles arbitrary group size; 64 is an even boundary so nibble
  packing stays clean).

---

## 3. Downstream impacts

| Impact | Direction | Magnitude | Notes |
|--------|-----------|-----------|-------|
| Scale bytes in the file | ↑ | 2× the scales: ~3.1% → ~6.25% of weight bytes | +~200–230 KB on the 7.3 MB model |
| Model `.bin` size (SD/LittleFS) | ↑ | ~+3% | trivial for storage |
| PSRAM weight footprint | ↑ | ~+230 KB | headroom was ~733 KB → still fits (~500 KB left) |
| Runtime KV context (`seq_ctx` auto-fit) | ↓ | possibly −1 token (e.g. 41→40) | `allocateRunState` budget = freePSRAM − weights; tighter on **quad FeatherS3**, fine on **octal XIAO** |
| Per-token bandwidth | ↑ | ~+3% (extra scale stream) | the honest cost that partially offsets the compute win |
| Compute per matmul (the win) | ↓ | ~1 fewer float-mul/weight on ~71% of matmul weight | **magnitude is measure-gated** — real only to the extent those matmuls are compute-limited, not bus-limited |
| Quantization quality | ↑ (slightly) | finer 64-element groups track local weight magnitude better | free accuracy bump |
| Backwards compat | none needed | — | user erases before flashing, owns all devices; old gs=128 files still load |

**Net:** guaranteed ~+3% bandwidth cost vs. a compute saving on ~71% of the
matmul work. It nets positive only if those matmuls are compute-co-limited
(the working hypothesis) — so this change is worth landing **paired with a
before/after tok/s measurement**, not on faith.

---

## 4. Risks & how to verify

- **Risk: converter emits scales in a different order than the device expects.**
  Mitigation: the device uses flat row-major `(i*n + j)/gs` grouping; confirm the
  converter is unchanged except for the gs value. Spot-check with the existing
  `spotCheckQ8` debug output at load (it prints dequantized samples vs scales).
- **Risk: tighter PSRAM drops `seq_ctx` more than expected.** Mitigation: watch
  the `[LLM] Context: model seq_len=… -> runtime ctx=…` log after load; if ctx
  falls too far on the quad board, that board can stay on gs=128.
- **Verify the win:** log tok/s before/after on the **same board** with the same
  prompt. If tok/s is flat, those matmuls were bus-limited, not compute-limited —
  keep gs=128 to save the 3% bandwidth, and move the effort to the INT8-activation
  SIMD kernel (0a) or fewer-bytes (INT4-mixed boundary) instead.

## 5. Rollback

Trivial and instant: redeploy the old gs=128 `.bin`. No firmware revert, no
migration, no data change. The two file generations are interchangeable at load
time.
