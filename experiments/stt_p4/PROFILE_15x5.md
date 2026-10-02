# QuartzNet15x5 on the P4: per-stage inference profile (2026-09-29)

Measured on the P4X-EYE (`02:48:57:31:04:68`, 400 MHz, Performance mode) with
the `sttperf stages` counters: each of the 201 ESP-DL stages is timed around
`model->run(...)` and summed over 8 speaker-test segments (~9 s audio each,
~450 output frames). Raw per-stage averages: `/Volumes/USB2/stt/work/deploy15/stage_us.json`.

**Network total: ~2.6 s per ~9 s segment** (plus ~0.2 s of scheduler yields, now
reduced by yielding every 5 stages instead of every stage).

## Where the time goes

| Layer type (count) | ms / segment | Share | Throughput* |
|---|---:|---:|---:|
| Pointwise 1x1 conv 512->512 (53) | 1355 | 51.9% | ~4.6 GMAC/s |
| Pointwise 1x1 conv 256->256 (36) | 179 | 6.8% | ~5.9 GMAC/s |
| Depthwise k=75, 512 ch (15) | 353 | 13.5% | ~0.7 GMAC/s |
| Depthwise k=63, 512 ch (15) | 204 | 7.8% | ~1.1 GMAC/s |
| Depthwise k=51, 512 ch (15) | 145 | 5.6% | ~1.2 GMAC/s |
| Depthwise k=39 / k=33, 256 ch (31) | 91 | 3.5% | ~1.4 GMAC/s |
| **Final depthwise k=87, dilation 2 (1, stage #197)** | **105** | **4.0%** | **~0.2 GMAC/s** |
| Pointwise 512->1024 (1, #199) | 59 | 2.3% | ~4.0 GMAC/s |
| Residual Add (15) + separate ReLU (15) | 89 | 3.4% | - |
| Everything else | ~30 | ~1% | - |

\*MACs at ~450 output frames per segment. The P4's int8 SIMD peak is roughly
12 GMAC/s across both cores, so pointwise runs at ~40-50% of peak.

## Findings

1. **Pointwise (1x1) convolutions are ~60% of the time and already efficient**
   (~40-50% of peak). ESP-DL's P4 assembly kernels are doing their job here;
   only modest gains are plausible (activation placement, internal RAM).
2. **Depthwise convolutions are ~33% of the time at 5-8x lower efficiency.**
   ESP-DL's P4 depthwise kernel (`dl_esp32p4_s8_depthwise_conv2d.S`, driven by
   `dl_base_dwconv_loop.cpp`) vectorises across channels but reloads the weights
   for every output frame, so QuartzNet's long 1-D kernels (33-87 taps) are
   load-bound. A kernel that keeps weights in registers across several output
   frames (register blocking along time) could plausibly run these 2-3x faster,
   worth ~15-20% of total inference. Requires custom P4 assembly.
3. **The single dilated k=87 layer (#197) is the worst: 4% of all time in one
   layer at ~0.2 GMAC/s.** Its padding is 86 frames per side, so with ~450
   frames about 38% of outputs fall in ESP-DL's per-position edge path, which
   recomputes loop bounds in scalar code. Pre-padding the input (so the conv runs
   only its "body" path) or de-interleaving even/odd frames (turning the
   dilation-2 conv into two plain convs) should cut most of this. The same
   edge-path cost applies, less severely, to every long-kernel depthwise layer
   (k=75 pads 37 per side).
4. **Residual Add + ReLU (3.4%)** run as two separate requantising passes; fusing
   the ReLU into the Add would save ~1%.
5. **Variance:** stage #197 swung 85-320 ms between segments in earlier runs,
   consistent with the edge path's cost depending on segment length.

## Suggested order

| Step | Effort | Expected gain (inference) |
|---|---|---:|
| Pre-pad / de-interleave the dilated #197 (export-time graph change) | small-medium | ~3% |
| Pre-pad long depthwise convs to avoid edge paths | medium | a few % |
| Register-blocked long-kernel depthwise kernel (P4 assembly) | large | ~15-20% |
| Fuse ReLU into residual Add | small | ~1% |
| Distil to 10x5 / 5x5 student | large (training) | ~35% / ~65% |

Engineering changes above keep the model's maths identical (bit-exact apart from
the export-time rewrites, which are mathematically equivalent); only distillation
changes the model.
