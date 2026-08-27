# G2 LZ4 image compression — bench test

`even_hub_sdk` 0.0.12 added LZ4 to the image raw-data path and stamps
`CompressMode = 2` on `ImageRawDataUpdate`. Production auto path uses LZ4
**bare block** (Even App `dart_lz4` / Q16d). Solo FRAME_CSIZE also paints
but blanks on mixed children — do not use it for production.

Panel paths (menus pruned 2026-07-31):

- Solo CREATE-image LZ4 A/B: **Test Suite → Image Tests → Compression (LZ4)**
- Multi-container canaries: **Test Suite → Image Tests → Mixed / multi**

```
Compression (LZ4)          Mixed / multi
<- Image                   <- Image
Q32: A/B (bare block)      Q16 raw / Q16d LZ4 block
                           Q17 / Q18 / Q28 / Q28L / Q30 / Q30c
```

Retired from menus (results kept in `G2_LZ4_MIXED_CONTAINER_INVESTIGATION.md`):
Q32f framing sweep, Q32 A/B under frame/size-prefix, Q16a–c FRAME chase,
Q30b wire-order, Q29 2-bpp, Q31 REBUILD-move.

## Q32 — A/B benchmark (~30 s)

Solo CREATE once, then ART/NOISE × raw/LZ4 (bare block), 5 reps each.
Watch the lens: bars marching = LZ4 painted. Compressibility, not pixel
count, sets cost — NOISE often stays raw (`compLen >= bmpLen`).

Magic must be ≤255 (`MagicRandom` is uint8). Q32 create=248, push=249–254.

## Production auto path

Both image chunkers (`sendImageBmpMultiFragment` /
`sendImageBmpFragmentsNoCreate`) run `g2ImgPrepareWirePayload`: on RAW they
try `lz4CompressBlock` and stamp mode 2 only when the payload shrinks; on
LZ4 they passthrough. Kill switch: `gG2ImgLz4Auto`. Photos/noise that do
not shrink stay raw. Health / Maps / Pet / camera stream / viewers use auto.
Q16 keeps force-raw as the mixed uncompressed control. Q32 clears auto for
its raw A/B legs.

`MapTotalSize` is the **compressed** length under mode 2. See
`docs/G2_PROTOCOL.md`.

## Outcome (2026-07-31, verified on-device)

| Path | Framing | Result |
|---|---|---|
| Solo CREATE-image | FRAME_CSIZE or bare block | paints (Q32) |
| Mixed image child | FRAME_CSIZE | blank (Q16a–c) |
| Mixed image child | bare block | **paints** (Q16d) |

| Leg | Size | Frags | Push time | Notes |
|---|---:|---:|---:|---|
| ART raw | 20854 B | 6 | ~2455 ms | Bars paint |
| ART LZ4 (frame+csize, solo) | ~317 B | 1 | ~68 ms | Solo paints |
| Q16d LZ4 (bare block, mixed) | — | 1 | — | list+image paints |
