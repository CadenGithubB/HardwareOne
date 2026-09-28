# Continuous STT: shared session and Pi adaptation

Architecture audit and adaptation notes, 2026-09-28. Firmware baseline: `b2ade97`.
P4 implementation and qualification come first. The local `continuous-v4` build
and host tests pass; a 60.5-second physical quiet-room check produced zero
segments and zero overruns. A ten-minute continuous speech run with camera
captures also passed; see [CONTINUOUS_RESULTS.md](CONTINUOUS_RESULTS.md). The exact firmware identity
and size are recorded in [README.md](README.md). The Pi is unavailable for a
physical test; nothing in this note changes its software or the S3 speech setup.

## Shared application boundary implemented in this change

`System_Dictation` remains the single keyboard API. The P4 local backend and
existing Pi backend both publish through its common delivery mailbox and use
`dictationPeekTextFor` / `dictationCommitTextFor`. OLED/G2 have no local-provider
compile branch. Begin, stop, cancel and snapshot retain the existing public
entry points; `continuous`, capture and preparation metadata describe behavior,
not a particular chip or provider.

Pi UART v1 is unchanged. Its accepted `dictate result` becomes one final segment
(sequence 1), with `deliveryExchange` retaining the accepted ID after host
ownership is cleared. The same peek/commit functions can deliver it in pieces
without losing its identity when asynchronous WAV cleanup finishes. The old
`dictationTakeTextFor` is a compatibility wrapper over this mailbox, rather than
a second Pi-only delivery implementation.

Local jobs stage successive broker chunks into that mailbox. The common state
owns text offsets, field identity, stop intent and UI state. Local bookkeeping
retains only the asynchronous backend token and immutable identity needed to
cancel/join a late start safely. It does not hold a parallel UI result mailbox.
A backend chunk is acknowledged only after all of its pieces have been consumed;
a valid empty chunk is acknowledged without displaying an error. Partial pieces
preserve the tail. Fields stop visibly when full. G2's established character
filter remains a consumer policy, and its consumed count distinguishes filtered
bytes from an unconsumed capacity-limited tail.

Focused ASan/UBSan checks pass for both `ENABLE_LOCAL_STT=0` and `1`, compiling
actual shared mailbox/Pi-result functions. They cover 512-byte local results,
empty chunks, partial/replayed/wrong-owner commits, cancellation and revocation,
late starts, field-full cleanup and compatibility drains. Separate tests compile
the actual OLED/G2 consumer functions, G2 countdown service and guarded field
append/capacity functions. This is host coverage; no Pi hardware or OLED/G2
accessory qualification is implied.

## Shared voice-activity decision policy

`components/hardwareone/Audio_VadPolicy.h` now holds the common amplitude
threshold decision used by the existing recorder and the P4 continuous
segmenter. Its ambient-relative gate is `max(silenceFloor, 2 * ambientFloor)`;
its endpoint gate additionally includes `peak / 8`. Speech onset requires a
previously seeded ambient floor and the configured speech minimum. The helper
only returns decisions: each caller retains its measurement, history, timing
and PCM ownership.

The Pi recorder's behavior is unchanged. It still measures mean absolute
amplitude after its existing source processing (PDM DSP; decoded G2 PCM passes
through), uses speech/silence minima **120/45**, and updates the same **40-chunk**
minimum window. Peak updates, first-frame latch exclusion, silence accumulation,
trim decisions and recording deadlines remain in `System_Microphone.cpp`.
`test_audio_vad_policy.py --sanitize` compiles the actual recorder decision block
and compares it with the original expressions. ASan/UBSan parity checks cover
threshold boundaries, window wrap, long and changing level sequences, source
rates, disabled VAD, trim modes, latch state and stop timing.

P4 continuous capture uses the same decision helper with **DC-removed RMS for
analysis only**. The raw PCM copied to the model remains unchanged. Its own
initial **500 ms** ambient calibration is excluded from both model input and
pre-roll; its bounded **500-frame** history represents five seconds of 10 ms
analysis frames. During an utterance, the effective ambient floor may decrease
but cannot drift upward into sustained speech. Natural endpoints resume ambient
adaptation and reset the utterance peak; idle peaks cannot suppress later quiet
speech. These are segmenter state rules, not changes to the Pi recorder.

The current segmenter raw-RMS minima are **45/16** (speech/silence), selected from
P4 room noise of roughly 3–31 RMS and speech peaks of 124–339 RMS. They are
configurable input-level parameters, not a claim that every microphone or room
uses the same levels. The older fixed-energy mode remains available. Host
ASan/UBSan regressions verify 120 seconds of fluctuating 3–31 RMS noise without
segments, subsequent 60-RMS speech detection, exact calibration exclusion,
DC/chunk-size invariance, peak resets, and sustained voice across hard limits.
Explicit 12/6 settings separately verify configurable low-level speech detection.

These host checks do not establish human-speech accuracy or hardware throughput.
The separate ten-minute P4 result is in [CONTINUOUS_RESULTS.md](CONTINUOUS_RESULTS.md).
Sharing this policy and the Dictation delivery mailbox does **not** implement
continuous Pi capture or change UART v1; the Pi adaptation below is still future
work.

## Source availability

No Pi service checkout was found by a targeted filename search under
`/Users/cadbecaimacmini/Documents/Codex/Projects`. The firmware README points to
[HardwareOne_RaspPi_CoProcessor](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor).
That repository is public. Its relevant source was inspected at immutable commit
[`9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9`](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor/tree/9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9),
without cloning or modifying it. Host filenames below refer to that revision.

## Existing contract and limits

| Area | Current behavior and evidence |
| --- | --- |
| Pi keyboard exchange | `System_Dictation.h:12` describes one owned recording, `dictate_request <16hex> <path>`, host WAV fetch, and one `dictate result <16hex> <text>` response. Local STT is a separate provider selected at build time (`System_Dictation.cpp:808`). |
| Recording | Pi dictation starts the shared recorder with 1200 ms silence stop and trimming (`System_Dictation.cpp:26,949`). Its UI supervisor stops it at 30 seconds (`System_Dictation.cpp:1146`; `System_Microphone.h:36`). The generic recorder also has an independent 60-second sample/wall cap (`System_Microphone.cpp:998`). PDM recorder audio passes its DSP; local QuartzNet deliberately consumes raw HAL PCM. Do not accidentally change either model's input convention while sharing session code. |
| Storage | The recorder chooses writable SD, otherwise LittleFS (`System_Microphone.cpp:55`), and writes a token-derived WAV (`:1648`). At 16 kHz mono int16, 30 seconds is 960,000 PCM bytes plus a WAV header. The eventual continuous path should use bounded RAM segments, not an indefinitely growing recording or repeated flash writes. |
| Bulk transport | `cmd_voicefetch` rejects any active/finalizing recording and atomically excludes live PCM (`System_UartLink.cpp:1458`). It loads the complete file into PSRAM-preferred RAM, allows at most 2 MiB, and rejects transfers estimated to exceed 45 seconds at the configured baud (`:1472`). It then sends META/AUDIO COBS frames plus an end reply with whole-file CRC16 (`:1512`). The maximum frame payload is 1024 bytes (`System_UartLink.h:104`). |
| Deadlines | Firmware waits up to 90 seconds for a Pi result (`System_DictationPolicy.h:13`). The host's framed fetch uses its ordinary command timeout (`audio/fetch.py:433`); the existing firmware budget explicitly leaves room under its 60-second synchronous command window. Raising these is not a continuous architecture. |
| Host admission | Dictation requires an exact logged-in UART epoch, `dictate hostready v1`, and a fresh Ready/Busy CM5 heartbeat (`System_Dictation.cpp:187`). Heartbeat leases are 15 seconds normally and 75 seconds when Busy (`System_Cm5Presence.h:35`). |
| Result authority | The request latches the authorized host epoch when published; results must match that epoch, the live original input-surface epoch, and the exact single-use exchange ID (`System_Dictation.cpp:1282`). The direct UART intrinsic is isolated from command execution and transcript-bearing audit logs (`System_UartLink.cpp:926`). The owning OLED/G2 surface drains text once and checks its original epoch again (`System_Dictation.cpp:1184`). Preserve these boundaries. |
| Cancellation/cleanup | `dictate_cancel <id>` goes only to the host epoch which saw the request. Cancellation that races publication retains cleanup debt until the exact owner/path is finalized and deleted. Successful host delivery also queues exact-owner WAV removal (`System_Dictation.cpp:1343`). |
| Text | Firmware accepts nonempty printable ASCII up to 256 bytes (`System_Dictation.cpp:1427`). Host `sanitize_transcript` folds Unicode and silently clips to that limit (`dictation.py:105`). Existing keyboard fields have their own smaller limits. Continuous text needs chunk delivery and field-full behavior, not an ever-growing String. |

The host [DictationController](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor/blob/9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9/ai-service/hw1_ai_service/dictation.py#L125)
is deliberately a single-exchange actor: its queue has capacity one, a newly
queued request supersedes an old one, and its latest-ID/generation fences drop
stale work. It fetches one WAV and calls `pipeline.transcribe_dictation`, then
sends one result with mutation replay disabled. Reusing that actor unchanged for
segments would lose earlier speech when a newer segment arrives.

## Useful existing Pi building blocks

- The host's [STTEngine interface](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor/blob/9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9/ai-service/hw1_ai_service/stt/base.py#L18)
  already accepts complete mono int16 PCM plus sample rate and returns text.
  Bounded utterance segments can use Moonshine or Zipformer through this adapter.
- [LiveMoonshineWorker](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor/blob/9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9/ai-service/hw1_ai_service/stt/live.py#L328)
  already has bounded audio/text queues, worker ownership, partial hypotheses,
  and terminal text reconciliation. It is useful later; continuous sessions do
  not require both backends to become token-streaming engines.
- `System_LiveAudio` already carries 16 kHz mono int16 PCM with controller/exchange
  IDs, sample offsets, per-frame CRC, terminal CRC32 and explicit drops/abort.
  It has a renewable 3-second lease, 100 ms bounded TX admission and a 16-KiB
  shadow queue (`System_LiveAudio.cpp:27,44,420,444`). Its current admission is
  tied to recorder/EvenAI provenance or G2 Conversate; expose a separate narrow
  STT producer rather than impersonating either owner (`System_LiveAudio.h:65`,
  `System_LiveAudio.cpp:1021`).
- The host [live PCM receiver](https://github.com/CadenGithubB/HardwareOne_RaspPi_CoProcessor/blob/9b8aa2b585eee00e39990f7ac7745ad0e1fee0b9/ai-service/hw1_ai_service/audio/live.py#L20)
  checks sequence/sample offsets and CRCs, with a 48-KiB inbox, 96-frame ceiling,
  0.5-second first-PCM, 3-second interframe and **65-second absolute timeout**.
  Continuous sessions must rotate bounded exchanges or explicitly negotiate a
  renewed session protocol; removing the timeout without replacement is unsafe.
- The firmware live transport requires at least 921,600 baud. Classic ESP32's
  documented 230,400-baud configuration cannot sustain 16-kHz int16 audio:
  32,000 PCM bytes/s exceeds 23,040 raw UART bytes/s before framing. A future
  classic-ESP32 adapter needs an explicitly negotiated lower-bandwidth format
  or a faster validated link; a larger queue cannot solve the sustained deficit.

## Minimum shared contract

Keep one shared session/capture/consumer layer. Providers transcribe segments;
they do not own UI identities, HAL selection, history retention, or commands.

1. **Session admission:** live owner `(source, epoch)`, boot-unique session token,
   fixed source, fixed PCM format, selected backend and its generation. Pi also
   latches the authenticated UART epoch/capability at admission. A reconnect
   cancels old work; it never adopts a successor host or silently changes backend.
2. **Segment:** `(session, sequence, startSample, endSample, forcedBoundary)` plus
   a bounded immutable PCM span. Sample positions are 64-bit session-relative.
   Silence/maximum-utterance segmentation is shared. Any overlap policy is
   explicit; final text must not duplicate the overlapped words. Each engine
   retains its established input preprocessing inside its adapter.
3. **Backend operations:** `available/capabilities`, `submit(segment)` returning
   Accepted/Busy/Failed, completion carrying the same identifiers plus final
   text or error, and cooperative cancel/join. Submission cannot block capture.
   The broker retains PCM until the backend releases it. Local synchronous
   inference runs on its worker; a Pi adapter waits asynchronously for exact-ID
   transport completion. No callback may free a buffer still in use.
4. **Bounded results:** ordered immutable text chunks, non-destructive oldest
   peek and explicit exact-sequence acknowledgment. Already-acknowledged IDs are
   harmless, future/out-of-order IDs fail. Slow consumers or audio overflow fail
   visibly; never overwrite unconsumed audio or text. A session can continue for
   arbitrary elapsed time while retained RAM stays bounded.
5. **Finish versus cancel:** finish freezes input, seals the final segment and
   drains queued work/results. Cancel invalidates pending delivery, discards
   private data and waits for actual worker/HAL release before reuse. Source,
   identity or backend loss cannot produce successful partial completion.
6. **Consumers:** CLI returns private chunks to the originating session; shared
   mirrors remain redacted. OLED/G2 append pieces only to the original field,
   acknowledge after the complete chunk has been accepted, and stop explicitly
   when the finite field is full. No entire-session transcript must accumulate
   in broker RAM, and recognized words never execute as commands.

The local provider uses the P4 broker's `sttBeginContinuous`, `sttReadChunk`,
`sttAcknowledgeChunk`, session counters and finish/cancel lifecycle internally.
Its engine/worker also serves headless CLI; keyboard ownership and text delivery
remain in the shared `System_Dictation` layer described above. Pi v1 already uses
that same application delivery layer, while continuous Pi audio transport still
needs the separate adaptation below. Those wire details are a design only, not
an implemented or compatible protocol claim.

## Pi changes needed later

Negotiate a new explicit continuous-STT capability, separate from `dictate
hostready v1`. Use bounded exchange IDs mapped to shared session/segment IDs and
sample ranges. Prefer reusing the existing COBS live frame encoder/receiver and
its epoch/CRC defenses, with an authorized STT producer and ordered result
messages. Keep the broker's internal 64-bit session positions even if individual
wire exchanges retain 32-bit offsets. Rotate before wire limits and renew leases.

The daemon needs a continuous actor with bounded ordered jobs, no latest-request
superseding, and a bounded replay/acknowledgment ledger. It can reuse its batch
engine per segment first. Do not reuse the current 256-byte clipping sanitizer
as a whole-session result function; deliver bounded pieces with explicit limits.
Give each segment a bounded completion deadline, independently of the ongoing
session lease. Preserve mutation ambiguity handling, cancellation tombstones,
no transcript logging, and generation checks before every result transmission.

Do **not** remove voicefetch's recording guard to make this fit. Besides the
recorder/global-state coupling, synchronous full-file transfer blocks the command
lane and allocates another utterance-sized buffer. If a file-based transitional
adapter is later chosen, it needs a separate immutable completed-segment handle,
bounded spool/quota, asynchronous transfer and explicit cleanup ownership. That
is additional work and is unnecessary for the P4-first RAM path.

## Validation before claiming Pi support

Host tests should cover exact session/segment identity, duplicate/out-of-order
results and ACKs, callback cancellation races, reconnect, queue backpressure,
CRC/offset gaps, bounded cleanup and a finite field becoming full. Long replay
must show bounded buffers/counters without latest-ID data loss. Then qualify
real UART bandwidth, lease renewal under command traffic, continuous capture,
model contention, and explicit loss behavior on an actual Pi/ESP pairing.
Existing one-shot Pi dictation and G2 native voice flows remain regression
controls; no Pi continuous qualification has been performed in this task.
