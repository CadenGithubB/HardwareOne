# P4 STT session cache — latency qualification

2026-09-28, `codex/jpeg-portable`, following continuous-STT commit `512f201`.

Continuous dictation now retains verified model weights in PSRAM for the life
of one session. In the full application without the camera, model loading fell
from **3.38 seconds for the first phrase to a median 0.173 seconds
for subsequent phrases**. This measures loading, not total recognition latency.
The same-input standalone probe reduced total processing from **4.497 to 2.003
seconds**, with byte-identical input and output tensors. Accuracy and endpoint
timing were not changed.

## Implementation and compatibility

`System_STTLocal` owns an optional non-copyable session. Its P4 backend lazily
retains 7,172,016 bytes of aligned raw weights after validating the pinned
container, decompression and SHA-256. Every subsequent segment checks the
container header and hashes the resident weights again before the vendor parser.
Only the verified weights are retained: every segment constructs and destroys
its own shape-specific model and activation arena, avoiding the vendor model
resize path. No weights are loaded during indefinite silence.

The continuous worker explicitly resets this session after inference returns
and capture joins, before announcing completion or deleting its FreeRTOS task.
Cancellation, logout and error paths use that same lifetime boundary. Ordinary
one-shot transcription still frees its temporary weights after each call.
`weightsReused` in status describes the last segment; it is not a live allocation
indicator. The retained weights add no second weight copy to the existing peak
allocation, but remain unavailable to other features between phrases.

The shared HAL, Dictation mailbox and authenticated chunk protocol are retained.
Pi UART v1, S3 firmware and speech settings were not changed. This is a backend
optimization and does not add a separate UI path. No new feature exclusions,
compiler tuning, model, frontend, decoder, gain or segmentation changes were
introduced in this milestone.

## Installed image

- `private/images/cache-app-v1/hardwareone-idf.bin`: **6,367,776 bytes**, with
  **9,696 bytes** left in the unchanged app partition; 496 bytes larger than
  continuous-v4. SHA-256:
  `56c0dba75ed40c32959085f2134b4c5f8c3283c1660cc1e22f1cc325ed26d945`.
- App-only flash verified on the connected P4 and boot completed. IDF 5.5.5,
  ESP-DL 3.3.12 and ESP-SR 2.5.5 retained; **7,956 source checks passed**.
- Existing partitions and stored STT/ESP-SR models unchanged. The previously
  selected experimental omission of the optional R1 health web page/navigation
  and its two HTTP APIs remains; no further feature was removed.
- P4 rev 3.2, 400 MHz, 32 MB PSRAM, onboard PDM via HAL_Audio, OV2710 attached.
  Mac speakers played saved synthetic sentences; all inference ran on the P4.

## Physical results

| Check | Result |
| --- | --- |
| Quiet room | 30.9 s; no segments, no model allocation. |
| Speech without camera | 90.6 s; 7 ordered chunks, all acknowledged; first cold load and all later cache hits. |
| Camera coexistence | 301.6 s; 29 ordered chunks; five JPEG captures, 1 requested while inference was active. |
| Concurrent capture | Observed in both speech runs; zero reported HAL overruns. |
| Stop and delivery | All admitted segments drained; both queues empty at Done; repeated peek and duplicate ACK checks passed. |
| Inference cancellation | Actual Inference phase reached; capture and worker joined before Cancelled. |
| Logout | Prior run denied after re-login; following run started successfully. |
| Unresponsive receiver | Explicit failure at eight queued results after 110.4 s; no silent overwrite. |
| One-shot regression | Eight seconds / 128,000 samples transcribed on this image, with a fresh weight load. |
| ESP-SR | Started, competing STT rejected, then stopped/disarmed; zero wake/command detections. |
| Saved settings | All 20 checks passed after reboot; mic, camera and SR stopped, voice disarmed. |

With the camera running, warm loading had a median of **191 ms**
and a range of **171–595 ms**.
Warm segment processing ranged from
**1.17–5.93 seconds**
for the varying segment lengths in this run; this is not a same-input speedup
comparison. The camera run captured **4,826,112 samples**.
Minimum observed free stack was **7,356 bytes**
for the worker and **2,688 bytes**
for capture. No allocation failures were reported.

Full-app PSRAM deltas (after minus before, bytes) were: silence
+0, short speech +528,
cancel -528, logout +0,
queue-full +528, camera run -118,456.
Camera was enabled for both camera-run readings. Its resize buffer is allocated
lazily on first capture (115,200 raw bytes), so that delta is not a pure STT leak
measurement; small remaining differences are not attributed. The standalone
probe supplies the direct cache-release measurement below.

`srstart` briefly auto-armed voice recognition during the regression. Both wake
and command counts remained zero; `closesr` confirmed it was disarmed afterward.
The test used an unrecognized `voice disarm` spelling before that stop; the
standalone disarm command is `voicedisarm`. No voice-command execution occurred.

## Numerical parity and host checks

The actual ESP-DL probe ran fixtures A, B, A with one cache, explicitly reset it,
then loaded A again after reset. All four input/output SHA-256 values matched
the historical host fixtures exactly. The resident weight hash remained correct
after every model destructor. Cold calls read 5,490,696 bytes; warm calls read
only the 96-byte container header and verified the resident weights.

PSRAM free was exactly **33,551,716 bytes** before the probe, after explicit reset,
and after final RAII destruction. Retaining the weights used 7,172,020 heap bytes
including allocation overhead. A cold A took 2,627/492/1,378/0 ms for load/frontend/
inference/decode; warm A took 141/492/1,370/0 ms. The 55% reduction is for that
same 6.625-second input on the standalone probe, not a universal full-app claim.

Actual-source cache, one-shot and continuous broker tests passed ASan/UBSan;
the real-thread continuous harness passed ThreadSanitizer. Coverage includes
changing tensor shapes, per-call hash verification, pre/post-publication faults,
cancellation, corrupt cache rejection, memory budgets, session isolation and
reset before FreeRTOS self-delete. Tests explicitly verify model-before-weights
destruction and require explicit task cleanup so host C++ unwinding cannot hide
a device leak. These controlled tests complement the vendor probe and physical
application tests, rather than replacing them.

## Console interruption and remaining limits

The first camera run stopped after 16 acknowledged chunks and three captures
because a direct ESP-IDF Wi-Fi log split a status JSON object mid-byte. The board
remained responsive; the harness cancelled/joined STT and closed the camera.
That run is not counted as a completed long test. The final run uses up to three
retries for read-only status/peek/memory replies and 1.2-second polling. It needed
**one such retry**. Firmware serial serialization remains
unchanged and needs a separate fix; consuming JSON on this console must tolerate
background logs.

This remains phrase-level transcription. The eight-second minimum span, 600 ms
quiet endpoint, twenty-second hard cut and zero overlap are unchanged. Removing
load overhead does not remove endpoint waiting or make recognition word-by-word.
The five-minute cache run and historical ten-minute continuous run do not prove
infinite endurance. HAL counters do not detect every possible driver-level loss.
Heap checks are snapshots, not allocation reservations; concurrent heap pressure
and vendor OOM handling remain limits.

Wider CTC search did not improve the eight errors in fifty held-out synthetic
words, and gain/padding experiments gave no consistent benefit. No speculative
accuracy change was applied; see [DECODER_LATENCY_NOTES.md](DECODER_LATENCY_NOTES.md).
Human speech, G2 microphone, physical UI and Pi continuous transport still need
qualification. No S3, Pi, G2 or display was physically exercised this milestone.

[cache-validation.json](cache-validation.json) records timings, counts, memory,
identities and private-evidence hashes without transcripts or credentials.
[CONTINUOUS_RESULTS.md](CONTINUOUS_RESULTS.md) and [RESULTS.md](RESULTS.md) remain
historical evidence. The previous app remains available at
`private/images/continuous-v4/`, SHA-256
`08a961f324a7f54ce31f6d3b96e42d87fd670e8986d2ff5c501744befce637e7`.
