# Continuous local P4 STT — physical qualification

2026-09-28, `codex/jpeg-portable`, building on bounded STT commit `b2ade97`.

The connected P4 completed **600.9 seconds of continuous
capture**, producing **48 ordered, acknowledged chunks** while inference ran
concurrently. Normal stop drained all admitted audio and text. Three forced 20-second
boundaries occurred, and observed segment processing took about 7.0–13.1 seconds. There is no
whole-session duration timer; this ten-minute check does not establish infinite
endurance. Results arrive as phrases, not word-by-word live captions.

## Installed firmware and scope

- App: `private/images/continuous-v4/hardwareone-idf.bin`, **6,367,280 bytes**;
  **10,192 bytes** remain in the unchanged app partition. SHA-256:
  `08a961f324a7f54ce31f6d3b96e42d87fd670e8986d2ff5c501744befce637e7`.
- App-only flash was verified on the intended P4 and booted successfully.
  IDF 5.5.5, ESP-DL 3.3.12 and ESP-SR 2.5.5 are retained; all 7,956 sealed
  build-source checks passed. Existing model/frontend identities are unchanged.
- Only the optional **R1 health web page/navigation and its two HTTP APIs** were
  disabled to fit this experimental profile. Ring controls, Bluetooth, core
  health and its other interfaces remain. No compiler tuning was retained.
- Onboard PDM through the existing HAL; P4 rev 3.2 at 400 MHz with 32 MB PSRAM.
  OV2710 camera attached; no display/G2/Pi available. Mac speakers played the
  held-out test sentences. All model inference happened on the P4.
- The primary checkout, S3 firmware, C6 firmware, partitions and stored models
  were not changed during this continuous milestone. No captured session WAVs
  or whole-session transcript accumulates on the board.

## Device results

| Check | Result |
| --- | --- |
| Quiet room | 60.5 s, zero speech segments and reported overruns. |
| Repeated speech | 120.9 s, 11 ordered chunks, concurrent capture/inference; stop drained all 11. |
| Long run with camera | 600.9 s capture, 9,615,360 samples, 48 chunks; all acknowledged, both queues empty at Done. |
| Camera coexistence | 10 JPEG captures succeeded, 7 requested while inference was active. Camera closed afterward. |
| Result retries | Each ordinary next result was re-read unchanged before ACK; duplicate ACK accepted. Sequence order and final counts checked. |
| Cancel during inference | Actual Inference phase reached; Cancelled after worker/capture joined, with no pending text. |
| Logout during processing | Old result token denied after re-login; microphone/resources became available for the next run. |
| Receiver stops ACKing | Explicit text-queue-full failure at 8 retained results, after 96.8 s; no silent overwrite. Cleanup and restart passed. |
| Bounded mode regression | 8 s / 128,000 samples transcribed successfully on this final image. |
| ESP-SR | Started successfully, rejected STT competing for its microphone, then stopped/disarmed; no wake/command detections. |
| Saved settings | All 20 postchecks passed after reboot; camera, mic and SR stopped, voice commands disarmed. |

The long run reported **zero audio-overrun events** and ended with capture-stack
headroom **2,616 bytes** and inference-worker headroom **7,352 bytes**. These
are measured stack minima, not stack sizes. PSRAM free before/after that run
(with camera enabled for both readings) was **27,309,200 /
27,191,432 bytes**, a delta of **-117,768 bytes**. The first run
retained 684 bytes of PSRAM; later cancel/logout/queue tests recovered to within
four bytes of their baselines. The camera allocates its 240×240 RGB565 resize
buffer lazily on first capture (115,200 raw bytes), retaining it until camera
shutdown. Therefore the long-run delta is not a pure STT leak measurement; the
remaining small difference was not attributed. No allocation failures were
reported, but these observations do not prove indefinite leak-free operation.

The existing `opensr` command briefly auto-armed voice recognition during this
regression; both wake and command counts stayed zero, and `closesr` disarmed it.

Detailed timings, chunk extents, memory and evidence hashes are in
[continuous-validation.json](continuous-validation.json). Private transcripts,
audio fixtures, credentials and raw device logs remain ignored. Historical
bounded-model accuracy evidence remains in [RESULTS.md](RESULTS.md).

## Shared code and verification

`System_Dictation` retains common input ownership and chunk delivery for local
and Pi providers. OLED/G2 use the same peek/commit mailbox. `HAL_Audio` supplies
microphones; `Audio_VadPolicy` shares the recorder's amplitude decisions. The
Pi recorder's original processing, thresholds and timing pass parity tests;
UART v1 remains unchanged. Continuous Pi transport still needs the adaptation
in [CONTINUOUS_PI.md](CONTINUOUS_PI.md).

Actual-source host tests passed under ASan/UBSan, and the real-thread continuous
broker passed ThreadSanitizer. Coverage includes queue pressure, allocation/task/
model/HAL failures, stop/cancel joins, session revocation, late final publication,
finite input fields, sample boundaries, 64-bit counters and recorder parity.
P4/XIAO HAL variants and G2 live-audio regressions passed host checks; they do not
replace physical S3/G2/Pi testing. Full P4 compilation and source-seal checks passed.

## Limits and observed interruption

An initial run on this same image lost the middle of a USB status reply and its
completion marker after seven results. Model-loading and mesh logs continued,
and no broker lock inversion was found. That run is **not** counted as a complete
speech pass. The final suite used less log mirroring and slower polling; the
firmware's underlying USB-console behavior was not changed.

The detector is an adaptive energy gate, not neural VAD. Its 500 ms calibration,
45/16 raw-RMS minima, bounded ambient history and eight-second minimum segment
span worked for this room/board fixture. Sudden noise or quiet speech can still
be misclassified. Forced 20-second cuts have zero overlap and can split words.
Accuracy remains limited and requires human-speech evaluation.

Zero HAL events do not prove every driver-level sample was retained. In
particular, some G2 BLE packet-gap/stall diagnostics are outside the existing
HAL aggregate counter. This milestone qualifies P4 onboard-PDM behavior only.
UI text fields, result chunks and queues remain finite. A receiver must consume
results continuously; no claim of an unlimited single input field is made.

The bounded-STT rollback app remains `private/images/app-v3/`, SHA-256
`75c6e184932a2df4cbda16f0848fd7e65f40d65fb36aa6c4646598b051bc21e5`.
Use the [README](README.md) for the continuous CLI and reproduction steps.
