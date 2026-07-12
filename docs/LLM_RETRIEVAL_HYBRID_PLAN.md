# LLM Retrieval-Hybrid Plan — exact-answer lookup before generation

**Status: PLAN ONLY — nothing implemented.**
Companion to the 2026-07-10 LLM engine gap analysis. All sizes measured against
the real corpus; all hook points verified against the current code.

## 1. The idea

The on-device model is a memorization model: it was trained on a fixed set of
Q&A pairs and its best-case output *is* the trained answer, delivered at
~1.5 tok/s through a sampler that can still mangle it. But the device can ship
the canonical Q→A table itself. So:

1. User asks a question (web / OLED / BLE / serial — all funnel through the
   chat layer).
2. Canonicalize the raw question text, hash it, look it up in a table built
   from the training corpus.
3. **Hit** → return the canonical answer instantly: verbatim-correct, properly
   terminated, zero generation cost, zero hallucination risk.
4. **Miss** → fall through to the LLM exactly as today. Nothing changes.

The LLM stops being the retrieval mechanism for known questions and becomes
the fallback for novel phrasings — which is the only job a 6M-param model
should have.

## 2. Measured reality (kanto-pokemon-master corpus, 2026-07-10)

| Quantity | Value |
|---|---|
| Raw corpus file | 529,380 B (517 KB) |
| Q&A pairs | 5,825 (+9 free-text lore paragraphs, not indexable) |
| Unique answers | 1,525 (avg 3.8 phrasings per answer) |
| Deduped answer blob (NUL-terminated) | 84,928 B |
| Answer length min/median/max | 20 / 43 / 124 B |
| 32-bit FNV-1a collisions over 5,825 normalized questions | **0** |
| **Design (a) hash-only lookup file** | **131,528 B (~128 KB)** |
| **Design (b) + question text for fuzzy match** | **296,094 B (~289 KB)** |
| Model .bin for comparison | 7.3 MB |
| LittleFS partition, FeatherS3 16MB layout (partitions.csv) | 10,604 KB (~3.5 MB free with model installed) |

Answer to "would this increase the file size tremendously": **no** —
1.8–4.0% of the model, 3.7–8.3% of post-model free space.

Partition caveats: `partitions_sr_8mb.csv` LittleFS is 128 KB (neither design
fits); `partitions_no_sr_8mb.csv` is 2,796 KB (fits only without the model on
LittleFS). Primary board (FeatherS3 16MB) is comfortable. SD card (`/sd`) is a
valid escape hatch — full path plumbing already exists.

## 3. File format: `<model>.lut`

One binary sidecar per model, built by the trainer tooling, shipped next to
the `.bin`:

```
[u32 magic 'HLUT'] [u16 version] [u16 flags]        flags bit0 = has question blob
[u32 corpusHash]                                     FNV-1a of the source corpus text
[u32 count]
[count x entry, sorted by qhash ascending]
    u32 qhash          FNV-1a-32 of canonicalized question
    u32 ansOffset      into answer blob
    (u32 qOffset       into question blob — only when flags bit0)
[answer blob: NUL-terminated strings]
[question blob: NUL-terminated canonicalized questions — only when flags bit0]
```

- `corpusHash` lets the firmware log a warning when the `.lut` doesn't match
  the model generation it shipped with (stale-sidecar detection).
- Build tool must **fail** on any qhash collision (measured 0 today; the check
  is what makes 32-bit safe).

## 4. Canonicalization (the one correctness-critical rule)

Hash input = raw user text, lowercased, whitespace collapsed to single
spaces, trailing `?`/`!`/`.` stripped. Applied **identically** in the host
builder and the firmware.

**Never hash the post-casing-pass string.** The vocab-aware casing rewrite
(System_LLM.cpp Phase 0/0b, ~:889–1050) picks whichever casing tokenizes to
fewer tokens — a model/vocab-dependent choice that changes across retrains.
Hook the lookup **before** prompt mutation, on the raw text. Measured: the
lowercase mapping is lossless on the current corpus (still 5,825 distinct
normalized questions).

## 5. Device side

**Hook point:** `chatBeginTurn()` in System_LLMChat.cpp, before
`llmStartAsync`. One hook serves every sink (web, OLED, G2, BLE, serial) —
the chat layer is the single conversation owner (matches the
share-logic-don't-duplicate rule).

**Hit path:** append the user turn as usual, then append the canonical answer
via `appendTurnLocked()` with the turn finalized immediately. All sinks are
pull-based pollers (`chatReadStream`/`chatReadTurn`), so an instantly-complete
turn renders naturally — no streaming simulation needed. Tag the turn source
(`lookup` vs `model`) so UIs *can* badge it later.

**Miss path:** fall through to `llmStartAsync` unchanged.

**Lookup cost:** binary search over the sorted entry array with 8-byte
`pread()`s from LittleFS — ~13 reads for 5,825 entries, no resident table.
Optional: cache the 46.6 KB entry array in PSRAM at model load (never DRAM).
Either way, microseconds-to-milliseconds vs ~10 s of generation.

**Lifecycle:** load/validate the `.lut` in `llmLoadModel` (same base name as
the `.bin`), free with the model. Missing file → feature silently off.

**Interactions (all resolved by design):**
- **Do: mode** — bypass the lookup entirely (command mode, not Q&A).
- **`llmretry`** — a retry after a lookup hit should **skip the lookup** and
  go to the model: natural escape hatch when the canonical answer wasn't what
  the user wanted, and it composes with the existing suppress mechanism.
- **Confidence gate** — irrelevant on hits (no generation, no hedge). Miss
  path keeps it.
- **History** — lookup answers are normal turns; multi-turn context unchanged.

## 6. Host side (llm-converter repo)

`Training/training_scripts/build_answer_lut.py`:
parse the corpus (blank-line-separated `Q:`/`A:` blocks) → canonicalize
questions → dedupe answers → emit `.lut` (design a or b via flag) → **fail on
collision** → print stats (pairs, unique answers, bytes, coverage of corpus).
Wire into both trainers' post-training packaging step so every model ships
with a fresh, corpus-hash-stamped sidecar. (Same pattern as
`audit_token_coverage.py`, which already runs in-trainer.)

## 7. Settings / commands

Per the settings conventions (append-only table, real per-setting command, no
auto-registration):
- `llmlookup <0|1>` — enable/disable (default 1; inert when no `.lut`).
  Append as the next `llmSettingEntries` index with matching
  `LLM_SETTING_CMD` + `CommandEntry`.
- `llmstatus` gains: lut loaded? entry count, hit/miss counters for the
  session (cheap ints; proves value on HW immediately).

## 8. Phases

**Phase 1 — exact match (the MVP, design a, 128 KB):**
host builder + `.lut` load + `chatBeginTurn` hook + `llmlookup` +
status counters. Known questions answered instantly; everything else exactly
as today. This alone covers benchmark-style usage and every trained phrasing.

**Phase 2 — fuzzy fallback (design b, 289 KB):**
retain question text; on exact miss, score candidate questions by token
overlap (word-level Jaccard against the canonicalized input; only candidates
sharing ≥1 rare word — cheap prefilter via a small word→entry index or a
linear scan of 165 KB, still trivial vs generation). Above threshold → treat
as hit (optionally prefix nothing; it's still a canonical answer). Below →
LLM. This is the version that changes real-user experience, since novel
wordings ("tell me about pikachu") map onto trained questions.

**Phase 3 — polish:**
converter/web-UI awareness (upload `.lut` beside `.bin` in the migration
tool), UI badge for lookup-vs-model answers, per-question opt-out list if
some answers should always generate.

## 9. Risks / limits

1. **Phase-1 coverage is exact-phrasing only** — real users phrase freely;
   phase 2 is where the UX win lives. Ship 1 for plumbing, aim for 2.
2. **9 free-text lore paragraphs** have no `Q:` line — stay model-only.
3. **Stale sidecar** after retrain — mitigated by `corpusHash` + load warning.
4. **8MB SR partition layout** can't host either design — FeatherS3 primary
   target is fine; don't silently assume on other layouts.
5. **Multi-turn context**: lookup answers ignore conversation history (each
   question matched standalone). Today's model barely uses history either
   (KV reset per turn), so no regression — but a future context-aware model
   would need the lookup to only fire on history-free questions.
6. **"It's cheating"** — it is, productively: the model was already trained to
   memorize these exact strings; the table just removes the lossy 1.5 tok/s
   middleman for the cases where memorization already won.
