# Combined Audio+Video Recording (muxed AVI) — Design Plan

Status: **PLAN ONLY — not implemented.** Revision 4, 2026-08-25.

Built from a seven-reader subsystem investigation, then hardened by three
adversarial rounds: the muxing design (10 corrections), the Phase-0 fix designs
(5 corrections), and a meta-audit that re-checked those corrections and swept for
scope creep (13 more). §9 is the full ledger. Revision 4 is **smaller** than
revision 3 — the audit removed more than it added.

Goal: one recording produces one file with interleaved video **and** audio,
instead of today's separate `VID_*.AVI` (silent MJPEG) and `rec_*.wav`.

---

## 1. Recommendation

Add a second stream — 16-bit mono PCM as `auds` / `01wb` chunks — to the existing
MJPEG-in-AVI writer in `System_Camera_Video.cpp`. Feed it by running the existing
mic recorder in a new **sink=AV mode**: the capture lifecycle it already owns
(ownership tokens, source arbitration, G2 keepalive, source-loss handling) runs
unchanged, but instead of writing a WAV it publishes PCM into a persistent PSRAM
ring that `cam_record` drains into the same AVI. Surface it as
`camerarecord start audio`. No new task, no new command, no new registry row.

Sync is kept honest by **absolute-t0 slot accounting**: latch a wall-clock origin
once, compute each written frame's slot from it, and emit zero-length `00dc`
placeholder chunks for slots that passed without a frame. Combined with declaring
the *period* rather than the nominal rate, the video timeline becomes correct by
construction and audio stays the sample-counted reference.

## 2. Reuse inventory

**Video — `System_Camera_Video.cpp`.** Hand-rolled RIFF writer: 224-byte
single-stream skeleton (`buildHeaderSkeleton`, :137-208), `00dc` chunks with even
padding (:253-287), PSRAM `idx1` (:473-477), finalize patches four offsets
(:84-87, :313-327). Task `cam_record` (6144 B, prio 5, core 1) paces at
`gSettings.cameraStreamFps` (1-20, default 5). SD-only, every chunk write under
the global `FsLockGuard` because concurrent mic/sensorlog SD writes used to
truncate AVIs mid-frame (:261-264).

**Audio — `System_Microphone.cpp`.** `mic_record` (4096 B, prio 5, core 1) pulls
`audioReadPcm(buffer, 2048, 100)` — 4096-byte chunks = 128 ms @16 kHz. Per-source
DSP (PDM only), VAD scoring, then the proven bounded non-blocking tee
`liveAudioRecorderOffer` (:1177-1180), then `writePcm` (:903-913). The lifecycle
machinery — owner tokens, claim/busy/result ring, per-capture source re-resolve,
G2 FAST container and stream keepalive, first-wins terminal cause, boundary trim —
must be reused, not reimplemented.

**HAL_Audio.** Exclusive single-owner **destructive** pull; `audioReadPcm` takes
no owner argument. The muxer must never claim a lease or pull directly — it tees
off the mic task. Every source is mono int16; PDM at `micSampleRate` (8000-48000),
G2 fixed 16 kHz. PDM DMA slack is 4×1024 frames = **256 ms at 16 kHz, 85 ms at
48 kHz**. G2 has a 2 s PSRAM jitter ring whose overrun policy is **drop-oldest**.

**Camera.** `captureFrame()` is the single choke point, zero-timeout mutex, losers
skip a frame; sensor emits JPEG directly. **A failed grab runs a full camera
re-init inline in the recorder loop** (:774-801) — over a second. `cameraWidth` /
`cameraHeight` are already exported globals (System_Camera_DVP.h:26-27), set at
init and on every resolution change, and already trusted by the status JSON.

**Storage.** SD over SPI, XIAO Sense only. One measured figure: ~410 KB/s at
4 MHz. One global reentrant FS mutex over both filesystems, with a measured
216-776 ms convoy from the command-audit `LittleFS.usedBytes()` scan.

**Where it can run.** `ENABLE_CAMERA_SENSOR` defaults on only for the XIAO
ESP32S3 Sense — the same board with the only PDM mic and the only SD slot. No
classic-ESP32 camera exists in this tree, so the classic I2S0 DVP-vs-PDM conflict
never arises. The XIAO is also the flash-tightest board (~3% free).

## 3. Design

### 3.1 Container
Insert an audio `strl` LIST after the video `strl` (at old offset 212). Every
existing video field offset survives; only the movi header moves. Audio-build
header = **324 bytes**; video-only keeps 224.

| Offset | Content |
|---|---|
| 0 | `RIFF` size(patched) `AVI ` |
| 12 | `LIST` **292** `hdrl` |
| 24 | `avih` cb=56: **dwStreams=2**, **dwFlags=0x110** (HASINDEX\|ISINTERLEAVED), `dwMicroSecPerFrame = usPerFrame` |
| 88 | `LIST` 116 `strl` — video |
| 212 | `LIST` **92** `strl` — audio (4 + 64 strh block + 24 strf block) |
| 224 | `strh` cb=56: `auds`, fccHandler=0, dwScale=1, dwRate=latched rate, dwSampleSize=2; `dwLength` patched **@264** |
| 288 | `strf` cb=16 PCMWAVEFORMAT: mono, 16-bit, nBlockAlign=2 |
| 312 | `LIST` `movi`, size patched **@316** |

- **`AviLayout`** — `{hasAudio, headerSize, moviSizeOff, audsLengthOff, usPerFrame,
  sampleRate, idxCap}` computed once at start, replacing the four `OFF_*` constants.
- **Audio chunk alignment.** `01wb` sizes are even **by construction** — the ring
  is sample-indexed, `avMuxOffer` takes a sample count, the drain emits
  `2 × samples`, and an assert catches violations. **Keep the generic even-pad
  path for all chunk kinds.** Removing it for `01wb` (revision 1's prescription)
  would mean any odd size that escaped the invariant puts the next chunk header at
  an odd offset and desyncs the entire movi walk, video included — far worse than
  the one lost sample that padding costs.
- **Separate index cursor.** Five sites conflate frames with index entries
  (:276, :277, :298, :301, :308). Add `s_idxCount` as the sole append cursor;
  `s_frameCount` stays the video slot count feeding `dwTotalFrames`/`dwLength`.
  Audio entries carry `AVIIF_KEYFRAME` (every PCM chunk is a random-access point);
  placeholders carry flags 0.
- **Index sizing must come from duration, not a constant multiple.** Audio entries
  accrue at ~3.9/s regardless of fps, so `2 × MAX_FRAMES` is exactly break-even at
  fps=5 (zero headroom) and 60% short at fps=1. Size as
  `videoSlots + ceil(durationMs / kMaxAudioChunkMs) + margin`, derived from the
  Q4 seconds cap. **Also add the missing clamp at :301** — finalize writes
  `s_frameCount * 16` bytes from the buffer with no `s_idxCap` check, which is
  safe today only because the two are equal.
- **Allocation.** Use the tree's tagged allocator —
  `ps_alloc(bytes, AllocPolicy::RequirePSRAM, "avmux.ring")` — not raw
  `heap_caps_malloc`. `RequirePSRAM` is already strict-SPIRAM, and the tagged form
  keeps ~500+ KB of permanently-pinned AV allocation out of the memory report's
  unattributed bucket. `PreferPSRAM` silently falls back to internal DRAM and must
  not be used here.
- **Real dimensions with no capture and no backward seek.** Build the header from
  the already-exported `cameraWidth`/`cameraHeight` globals. This removes the
  `seek(64)/seek(176)/seek(AVI_HEADER_SIZE)` dance at zero cost, and fixes the
  geometry fields that ship stale at 640×480 today. Keep `probeJpegDims` on the
  first written frame purely as verification; if it disagrees, patch at finalize.
  (Revision 3 proposed capturing frame 0 up front instead — dropped, see §9.16.)
- Finalize patches: RIFF size @4, `dwTotalFrames` @48, video `dwLength` @140,
  `dwSuggestedBufferSize` @60/@144 from `s_maxFrameBytes` (tracked at :284, read
  nowhere today), audio `dwLength` @264, movi size @316.

### 3.2 Sync
**(a) Declare the period, from one number.** Pacing is `intervalMs = 1000 / fps`
in **integer** ms with `CONFIG_FREERTOS_HZ=1000`, while the header declares
`dwRate = fps`. Thirteen of the twenty legal rates are affected — worst is 17 fps
at 1.42% (4.3 s over five minutes), and 15 fps (a glasses-page stop, 1.01%) is two
taps from the default. Fix: define **`usPerFrame = intervalMs × 1000`** as the
single primitive and write `dwMicroSecPerFrame = usPerFrame`, `dwScale =
usPerFrame`, `dwRate = 1000000`. Deriving both fields from one value is what stops
avih and strh from contradicting each other. **The existing `usPerFrame` at :140
is `1000000 / fps` — it must be deleted, not reused**, or the quantization error
survives under a new name.

**(b) Latch fps once.** It is read twice — :481-483 on the caller's task for the
header (with a dead `>60` clamp; every writer already bounds 1-20), :351-353 on
`cam_record` for pacing — with task creation between and the setting live-editable
from the glasses page. Latch into `AviLayout`. Fix the fallback from 10 to 5;
every other reader in the tree uses 5.

**(c) Absolute-t0 slot accounting.** Latch `s_t0` once. For each written frame
compute `slot = (millis() - s_t0) / intervalMs`, and emit zero-length `00dc`
placeholders for `lastSlot+1 .. slot-1` as **one batched write under one
`FsLockGuard`** (a 20 fps recording can owe 15 at once, immediately after the
convoy that caused them). The video timeline then equals slots × interval by
construction, independent of how the loop paces.

*Do not hoist the pacing anchor.* `TickType_t loopStart` is declared inside the
loop body, so `vTaskDelayUntil` never accumulates and the real period is
`max(work, intervalMs)`. Revision 3 called hoisting it "the largest single win";
the audit showed the opposite. Because the un-hoisted loop can never run *ahead*
of the grid, two frames can never land in the same slot — so absolute-t0 slot
accounting gets the same guarantee while the at-most-one-per-slot rule,
drop-late-frames, and the post-stall free-run burst (which would re-run
`captureFrame`, and with it a possible >1 s camera re-init, purely to discard the
result) all become unnecessary. Three fewer moving parts and no new failure mode
to hardware-test.

**(d) Cross-clock drift — only in one direction.** PDM shares a crystal with the
CPU tick and cannot drift. G2 can, but its dominant failure is the jitter ring's
**drop-oldest** policy, which makes audio *shorter* than wall time. Revision 3's
rule ("re-anchor video onto audio when they differ by 200 ms") would therefore
speed up a correct video timeline to match a lossy audio track. Re-anchor **only
when audio is longer** than the video timeline — a genuine clock-rate mismatch.
When audio is short, leave the video timebase alone; §3.4 already declares a short
audio stream valid. Defer this entirely to Phase 2 — it is G2-only, and PDM is
the v1 path.

### 3.3 Dataflow
```
mic_record (exists)                        cam_record (exists)
 audioReadPcm 128 ms chunks                  per slot:
 → DSP (PDM) / raw (G2)                        captureFrame() → 00dc + placeholders
 → [sink=AV] avMuxOffer(buf, samples) ─►ring─► drain → 01wb chunks (≤256 ms framing)
    bounded copy, never blocks, no FS         index entries for both streams
```
- **Ring: persistent, allocated once at max size, never freed.** Both tasks
  self-delete asynchronously, so a `Begin/End` alloc/free ring is unsafe by
  construction. A monotonic **generation counter** re-checked after each bounded
  copy makes a late offer from a retired capture a no-op — the LiveAudio shadow
  precedent (System_LiveAudio.cpp:721-740, 983-995).
- **Depth ≥ 4 s.** A failed frame grab runs a full camera re-init inside the
  recorder loop (>1 s), and FS convoys stack on top. 2 s is exceeded by a routine
  camera glitch.
- **First overflow is not fatal.** Emit silence of exactly the lost sample count —
  which preserves the timebase where a silent drop would not — and flag it. Only
  repeated overflow stops the recording.
- **One `FsLockGuard` for the whole per-slot drain**, not one per chunk. The
  ≤256 ms bound governs chunk *framing*, not lock granularity; a 4 s backlog would
  otherwise take 16 sequential acquisitions in one slot — the same anti-pattern
  §3.2(c) batches placeholders to avoid. The guard is per-task reentrant, so an
  outer guard costs nothing.
- **The drain writes straight from the ring** (at most two writes, handling
  wraparound). Nothing staged on `cam_record`'s 6144 B stack — a 48 kHz drain
  would be 19 KB. (The comment at :93-94 claiming large JPEGs are memcpy'd onto
  this stack is false; `captureFrame` copies into a PSRAM heap buffer. Fix it
  while in the file.)
- **Ring arming: the orchestrator does it, between the mic claim and the header
  write.** Revision 3 had the mic task arm the ring with `cam_record` polling for
  "armed" — impossible, because the audio `strh` sits at a fixed offset inside the
  header that `startVideoRecording` writes before `cam_record` exists. Add a
  `micRecordingLatchedSampleRate()` accessor beside `micRecordingBusy()`. The rate
  *is* knowable synchronously: `gRecSampleRate` is latched at :1570 inside
  `startRecordingInternal`, on the caller's task, before it returns — only the
  accessor is missing. This also restores a clean-fail path when PSRAM cannot
  supply the ring.

### 3.4 Lifecycle
**Start — mic first, transactional.** Mic-first is required because the `auds`
header needs the latched rate. `startVideoRecording()` has **nine** `return false`
exits after that point (:407, :411, :422, :429, :441, :446, :470, :497, :519) —
every one must unwind the audio claim, or the mic FSM sits in CAPTURING forever
holding the HAL lease and the G2 FAST container, locking out dictation and voice
capture until reboot.

**Stop — orchestrator-owned, unconditional.** `stopVideoRecording` gives up after
3 s and returns anyway, and `cam_record` can be further than that from finalize
during a camera re-init. Releasing the claim inside finalize is unreachable in
exactly the case that needs it.

**Never wait on the audio side while holding `FsLockGuard`.** Finalize holds it
across index, patches, flush and close. Stop is: (a) request audio stop, (b) wait
for the FSM to leave CAPTURING **outside** any guard, (c) take the guard once for
residue-drain + index + patches.

**Source loss / teardown mid-recording → video-only.** The mic FSM ends the claim
independently; wall-clock slots never depended on the audio clock. A capture
yielding zero samples finalizes with `dwStreams=2, dwLength=0` — valid and
playable. **`dwStreams` is never patched down**, because the audio `strl` is still
physically present and demuxers iterating up to `dwStreams` would mis-map indices.

**Start alignment.** Discarding the ring at first-frame time drops only what has
been *published*; audio still unread in the PDM DMA (256 ms) or the G2 ring is
offered after the discard, so audio **leads** video by roughly 250-400 ms. Measure
this with the clap test before building millis-stamping machinery for it.

### 3.5 Modularization
1. **`AviWriter`** (in place): header build/patch, chunk append, index — pure
   format logic parameterized by `AviLayout`.
2. **`avMux` ring** (new, ~100 lines): persistent allocation, generation guard,
   sample-indexed offer/drain.
3. **Mic sink** (`MicSink {WAV, AV_MUX}` latched at claim). Three things revision 3
   under-scoped:
   - **`writePcm` must dispatch on sink kind**, not merely re-gate. Applying
     "replace every `recordingFile` test with `sinkReady()`" literally to
     `writePcm` (:903-913) makes it call `.write()` on an invalid `File`, get 0
     back, and fire `micRecordingMarkFailed("short PCM write")` on the first chunk.
     Only the *gating* predicates (:1024, :1272-1274, :1310, :1349, :1367) become
     `sinkReady()`.
   - **The start path does filesystem work too** — folder mkdir, path build, file
     open and placeholder-header write, all under `FsLockGuard` (:1639-1686), with
     two `failStart` exits keyed on file failure. All must be skipped in AV mode,
     and the empty `currentRecordingPath` then feeds `micRecordingRemoveExactPath`
     and `micRecordingPublishIdle`.
   - **The `FsLockGuard` at :1201 is unconditional**, inside the successful-read
     branch. It must move into the WAV arm of the sink dispatch, or the mic task
     blocks behind video frame writes and the FS convoys exactly as the rejected
     direct-write design would — which would hollow out §3.6's whole argument.
   Also: `MAX_RECORDING_SEC` is welded into **two** loop bounds (:1002 samples,
   :1015 wall clock) — convert to a per-capture `gRecMaxSec` and change both, or
   AV recordings silently go video-only at 60 s. Suppress
   `SYSEVT_MIC_RECORD_STARTED`/`SYSEVT_MIC_SAVED` for AV captures (they fire with
   an empty path). AV captures run with `trim=false, silenceStopMs=0` — the
   existing defaults, so a documented requirement rather than new code.
4. **Glue**: tokenized arg parse, owner mint (six local lines — see §9.19), ring
   arming, transactional start, unconditional stop.

### 3.6 Considered alternative: direct-write (no ring)
Having the mic task write `01wb` chunks straight into the shared file would remove
the ring, its ownership problem, and the start-alignment machinery.

**Rejected on one reason, not two.** The sound reason is *failure-domain
coupling*: the mic task would block on the FS lock behind video frame writes and
the 216-776 ms audit convoy, and exceeding the PDM DMA depth (256 ms at 16 kHz,
**85 ms at 48 kHz**) drops audio at the hardware. The ring keeps the capture side
off the filesystem entirely.

Revision 3's second reason — shared `File` handle corruption — **does not hold**.
`FsLockGuard` gives real cross-task mutual exclusion, every piece of the shared
state is already mutated only under it, finalize holds one guard across index +
patches + close, and §3.4's stop ordering already eliminates the late write.
A write after close returns 0, which is a no-op, not corruption. Recorded here so
a future revisit is not misled by a reason that was never load-bearing.

## 4. Surfaces

| Surface | Change |
|---|---|
| CLI | `camerarecord start audio`. **Tokenize the parse** — `arg == "1" \|\| arg.equalsIgnoreCase("start")` compares the whole trimmed string, so `start audio` falls to the error. **Update both registry columns** — the dispatcher auto-appends the usage string to any `Error:` reply, so a stale `<start\|stop\|1\|0>` would tell the user the argument does not exist. Stop reply prints the full path unless a name accessor is added. |
| Camera status JSON | Add `cameraRecordingAudio` to **both arms** — the `#else` arm hardcodes `cameraRecording=false`, so a non-camera build would report `undefined` where the applier expects a bool. |
| Web camera card | Phase 2. The show/hide rule lives in the single `apply` chunk (WebPage_Sensors.h:278) while the click binding lives separately (System_Camera_DVP_Web.h:145-156) — both change. |
| G2 lens "Rec" row | v1: video-only. |
| OLED | No camera mode exists. |
| Automations | Free — add-time validation checks registry membership only, never arguments. |
| Events | Reuse the existing start/saved pair. `char det[24]` at :342 is a self-imposed local; `SYSEVT_DETAIL_LEN` is 80, so widen it. |
| Settings | Honors `cameraStreamFps` and the mic trio. No new settings in v1. |

**Build gating.** The feature gate is `ENABLE_CAMERA_SENSOR && ENABLE_MICROPHONE`,
but that must **not** become the file gate: the recorder's exported symbols are
referenced from two TUs gated on the camera flag alone, so a compound guard at
`System_Camera_Video.cpp:42` breaks the link on any camera-without-mic build.
Line 42 stays as it is; audio blocks carry inner `#if ENABLE_MICROPHONE`. Nearly
free — `micRecordingBusy()` is declared outside the mic gate and no-op stubs
already exist.

## 5. Downstream

Byte-transparent and unaffected: `/api/videos` and its file endpoint, the OLED and
G2 file browsers, the WAV recordings list, ESP-NOW (already past its 4 MB ceiling
with silent AVIs).

**The web AVI player breaks three ways** and must be fixed first: the movi scan
hard-breaks on the first unrecognized chunk id (which is the first `01wb`, landing
right after frame 0 — a one-frame file); `parseHdrl` reads rate/scale from every
`strh` last-wins, so the audio stream sets the rate to 16000; and the scan also
breaks on any zero-length chunk, which is what a placeholder is. Fix detail lives
in Unit A (§7) — §5 deliberately does not restate it.

Recordings are not distinguishable as audio-bearing from the listing. Keep the
`VID_*.AVI` prefix (nothing branches on it) and label from the parsed stream count
if wanted — Phase 3.

## 6. Pitfalls

- **P1 — the fps lie.** Three causes: integer-ms quantization against a
  non-quantized declared rate, the fps double-read, and frames written < slots
  elapsed. Fixed by §3.2(a)(b)(c).
- **P2 — HAL exclusivity.** `audioReadPcm` takes no owner and drains
  destructively; a second puller garbles both consumers. Tee inside the mic task.
- **P3 — micviz steals samples today.** `micVisualizerTaskFunc` (:2970) pulls with
  no busy check and no mutex, and mutates shared DSP state via
  `micProcessForSource` (:2976). This corrupts WAV recordings now. *Fix:* Unit E.
- **P4 — FS-lock convoys.** 216-776 ms audit scans plus O(size) log rotations on
  one global lock. Mitigated by ring depth, batched placeholders, and never pacing
  inside the lock.
- **P5 — SD throughput floor.** VGA@10fps ≈ 250-400 KB/s against ~410 KB/s at the
  4 MHz rung. Audio adds 32 KB/s at 16 kHz but **96 KB/s at 48 kHz**, where DMA
  slack also falls to 85 ms. Resolved by capping v1 audio at 16 kHz (Q5).
- **P6 — G2 clock drift**, one direction only. See §3.2(d).
- **P7 — mic settings commands destroy an AV capture; they do not refuse it.**
  `micsource`/`micsamplerate`/`micbitdepth` all force-stop a busy recorder. *No
  fix needed:* §3.4 already survives this as video-only, and adding a
  `micRecordingBusy()` refusal would regress dictation and voice capture too.
  Document it and surface it in the stop reply.
- **P8 — header and index churn.** The `OFF_*` constants and the five
  frame-count-as-index-cursor sites. Video-only output **is not byte-identical**
  after this work — the timebase, geometry, buffer-size fields and movi slot
  content all change deliberately. Re-baseline any golden-file hash once, at the
  end of Unit C.
- **P9 — allocation strictness.** `RequirePSRAM`, tagged, with a clean-fail path.
- **P10 — XIAO flash budget** (~3% free). Everything inside the gates; measure the
  delta. Note Unit E is flash-*positive*.
- **P11 — manual `sleep` during capture** freezes I2S/camera DMA. Pre-existing for
  both recorders; out of scope.
- **P12 — no free-space checks.** Pre-existing. AV files grow ≈1.1× faster at
  10 fps, ≈1.25× at the default 5.

## 7. Five independently testable units

**Unit A — player tolerance** (~15 lines of JS, ≈0.7 KB). Gate the rate/scale read
on `readFourCC(dv, sp+8) === 'vids'`; replace the `break` on an unknown chunk id
with a skip-by-size, keeping the `fp+8+fsize > buf.byteLength` bound; drop only
the `fsize === 0` half of the guard. **Use a plain id-inequality test, not a
regex** — this JS ships as ordinary C string literals, so a `\d` pattern is eaten
by the compiler and would match nothing, playing zero frames from every file. No
`vidsIdx` generalization and no `rec `/`ix##` descent: this device only ever
produces stream-0 video and never emits those chunk types. Testable today against
existing recordings — it must be a no-op.

**Unit B — timebase + slot accounting** (~35 lines, ≈0.4 KB). Latch fps and
`intervalMs` once; `usPerFrame = intervalMs × 1000`; write
`dwMicroSecPerFrame = usPerFrame, dwScale = usPerFrame, dwRate = 1000000`; latch
absolute `s_t0`; compute slots; emit batched zero-length placeholders. Do **not**
hoist the pacing anchor. Testable video-only: `ffprobe` duration must match wall
clock at fps=17.

**Unit C — container refactor** (~110 lines, ≈1.2 KB). `AviLayout`; `s_idxCount`;
duration-derived index cap with the finalize clamp; `AVIF_ISINTERLEAVED`;
two-stream header; `01wb` writer and index entries; `dwLength` @264. Header built
from `cameraWidth`/`cameraHeight`. Verify the video-only path matches Unit B's
output byte-for-byte.

**Unit D — audio path** (~250 lines, ≈3 KB). `avMux` ring; `MicSink::AV_MUX` with
`sinkReady()` gating, `writePcm` sink dispatch, the start-path FS bypass, the
`:1201` guard move, per-capture `gRecMaxSec` on both bounds, event suppression;
`micRecordingLatchedSampleRate()`; tokenized CLI plus both registry columns;
transactional start with unwind on all nine exits; orchestrator-owned stop.

**Unit E — micviz** (drive-by, flash-positive, gates nothing). Route micviz
through `getAudioLevel()` instead of its own pull, and delete
`captureAudioSamples()` at all three sites (definition, header declaration,
stub). Honest framing: this does **not** make micviz a pure published-scalar
reader — `getAudioLevel` performs its own destructive pull when idle. What it does
is swap an *unguarded* pull for the guarded, throttled one, which removes the
theft during recordings, which is the actual bug. Expect displayed levels to
roughly double (micviz's own `/32767` scale is replaced by the recorder's
`/16384`); that is the correction, not a regression. The 150 ms cache against a
50 ms loop means ~2 of 3 rendered frames repeat — raise the delay to 100 ms.

**Sequence:** A → B → C → D, with E anywhere. Roughly 410 lines and 5-6 KB of
flash. Deferred: the reboot/factory-reset `stopVideoRecording` addition (a real
drive-by fix for video-only recording, but it stacks up to 8 s on the OTA path and
needs its own shortened budget); the idx1 offset-base change (land it after Unit D
validates, so the refactor keeps a byte-identical baseline); §3.2(d) drift
re-anchor; web card and status labeling; start-alignment stamping.

## 8. Open questions

- **Q1** Should `camerarecord start` include audio whenever the mic is open, with
  an opt-out, rather than requiring the explicit argument?
- **Q2** Glasses record row with audio — needs a BLE load test first.
- **Q3** Should finished recordings feed the CM5 transcription pipeline?
- **Q4** Cap policy, in **seconds** rather than frames — this also sets the index
  cap (§3.1), so the two must be resolved together.
- **Q5 — answered: yes, refuse audio above 16 kHz in v1.** One comparison in the
  arg parse; it bounds the ring at 128 KB instead of 384 KB, keeps DMA slack at
  256 ms instead of 85 ms, and removes 96 KB/s against a 410 KB/s floor.

## 9. Corrections ledger

**Round 1 — muxing design.**
1. *"`writePcm` is the single choke point."* `recordingFile` gates the whole read
   branch; a file-less capture dies in ~128 ms.
2. *"The index grows to 2×."* Allocated once, never resized.
3. *"Audio chunks even-padded like video."* Right conclusion, wrong mechanism —
   and see §9.17.
4. *"A two-line drift patch."* Leaves avih contradicting strh.
5. *"PDM timelines cannot drift."* Integer-ms quantization is systematic.
6. *"Mic settings changes are refused."* Backwards — they force-stop.
7. *"2 s ring."* One camera re-init exceeds it.
8. *"Start alignment ≤128 ms."* ~250-400 ms; audio leads.
9. *"Release the audio claim in finalize."* Unreachable past the 3 s budget.
10. *"Files grow 1.3×."* ≈1.1× at 10 fps, ≈1.25× at 5.

**Round 2 — Phase-0 fix designs.**
11. *"Declaring the period is the fps fix."* Smallest piece only.
12. *"Use `s_startMs` for a measured-rate correction."* Wrong anchor; would patch
    playback slow.
13. *"Guard the micviz pull."* Half-measure — but see §9.18.
14. *"Guard `captureAudioSamples`."* Zero call sites; delete it.
15. *"Whitelist chunk ids with `/^\d\d[a-z][a-z]$/`."* Ships as
    `/^dd[a-z][a-z]$/` and matches nothing — verified by compiling it. Superseded
    by §9.20: use no regex at all.

**Round 3 — meta-audit (corrections to the corrections, and scope).**
16. *"Capture frame 0 up front to kill the dims patch."* **Dropped.** The hazard
    was invented — the dims patch already runs before the frame write in the same
    iteration. And it would move a possible >1 s inline camera re-init onto
    `cmd_exec_task` while holding the recorder control mutex, stalling every other
    command and freezing the glasses viewfinder. `cameraWidth`/`cameraHeight`
    already exist and cost nothing.
17. *"`01wb` chunks are never pad-padded."* **Dangerous as written.** Removing the
    pad path means an odd size desyncs the whole movi walk. Keep the path; make it
    never fire by construction.
18. *"Hoisting the pacing anchor is the largest single win."* **Dropped
    entirely.** Absolute-t0 slot accounting gives the same guarantee, and *not*
    hoisting means two frames can never share a slot — retiring the
    at-most-one-per-slot rule, drop-late-frames, and a new post-stall burst hazard
    that would re-run `captureFrame` only to discard the result.
19. *"Factor out `micRecordingMintOwner()`."* **Dropped.** There are three existing
    copies, not two, and each is welded inside its own module's admission
    spinlock — dictation's mint sits between the state check and the transition.
    A shared helper would be adopted by none of them. Write six local lines.
20. *"Structural regex plus `rec `/`ix##` descent in the player."* **Dropped.**
    This device produces none of those chunk types, and a plain id-inequality test
    needs no pattern at all — which retires §9.15's trap rather than working
    around it.
21. *"The drift re-anchor pulls video onto audio."* **Backwards for G2.**
    Drop-oldest makes audio *short*; re-anchoring would speed up a correct video
    timeline to match a lossy track.
22. *"Replace every `recordingFile` test with `sinkReady()`."* Applied literally to
    `writePcm` this fails the capture on chunk one. It must dispatch on sink kind.
23. *"The mic task does zero FS work in AV mode."* Only true of the *task* — the
    start path does three FS operations, and the `:1201` guard is unconditional.
24. *"Direct-write risks a corrupt file via the shared handle."* **Not
    load-bearing** — the global lock and §3.4's ordering already prevent it. Only
    the failure-domain argument stands.
25. *"micviz becomes a pure published-scalar reader."* `getAudioLevel` performs its
    own pull when idle. The fix is still right; the rationale was not.
26. *"The two level scales must be unified in the same change."* No-op —
    `getAudioLevel` already uses the recorder's scale. (And the ratio is 1.99994,
    not exactly 2.)
27. *"A `ps_alloc`/`free` mismatch in `captureAudioSamples`."* Not a defect;
    `ps_free` is `free`.
28. *"`2 × MAX_FRAMES` index cap."* Break-even at fps=5, 60% short at fps=1.
29. Drops for churn: `dwMaxBytesPerSec` patching, `biSizeImage`, the player's
    `vidsIdx` generalization, and the idx1 offset-base change (which would have
    destroyed the regression baseline for the largest refactor in the plan).
