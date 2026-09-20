# Conversate on the shared audio pipeline

## Scope and status

ESP32 implementation, September 20, 2026. Not yet qualified on hardware with
a CM5 PCM receiver. The companion coprocessor repository now has a local
`codex/g2-conversate` implementation extending its existing UART service with
long-session reception, streaming STT and optional audio recording. Neither
side of this audio integration has been deployed. Returned captions are still
pending. Do not interpret native “conversation saved” UI as proof that an
audio file exists.

This extends `HAL_Audio` and `System_LiveAudio`, not the bounded WAV recorder.
Conversate, EvenAI and manual capture should share capture ownership, format,
transport and downstream services while retaining their different native
control handshakes and policies. EvenAI's existing recorder-shadow API and
manual recording limits remain unchanged.

Conversate uses the existing G2 LC3 decoder, 32,768-sample PSRAM PCM ring and
`live_audio_tx` worker. That worker is the sole PCM reader; there is no second
recorder, filesystem write, shadow queue or newly spawned task. The native HAL
claim forces the left G2 source with an exact connection generation, never
falls back to PDM, and restores the previous selected source when released.
It does not send E0 AudioCtrl, acquire the legacy recording FAST hold or create
a display container. G2's native 0x0B owner retains start/keepalive/close/FAST.

## CM5 control contract

The CM5's existing UART service must be the only owner of the serial port.
Integrate the receiver there; do not open a second competing serial reader.
These commands require a real authenticated UART session, not web/USB or
AuthBypass, at a minimum 921,600 baud. The connected XIAO reported 2,000,000
baud and login epoch 1 during inspection, with no live-audio lease.

1. Inspect `liveaudio capabilities`; require `conversate=1` and support flag 2.
2. Acquire `liveaudio ready 1 <controller_hex16>` and renew it every 1,000 ms.
   The lease expires after 3,000 ms. IDs are exactly 16 hex digits with both
   32-bit halves nonzero. Keep the same controller ID for the lease lifetime.
3. Send `liveaudio conversate 1 <controller_hex16> on`.
4. Enable the G2 native handler with `g2conversate on` if not already enabled.
   The wearer launches Conversate on the glasses. Neither `ready` nor `on`
   starts the mic; arming during a session applies to its next fresh launch.
5. Demultiplex existing binary UART frames and text replies on the same link.
   Associate every received stream with BEGIN's controller and exchange IDs.
6. `liveaudio conversate 1 <controller_hex16> off`, exact-ID `abort`, or
   `release` cancels an active native stream. The G2 owner then closes its
   native session on the next control-worker tick. A renewed/replaced lease
   does not restart it: a new wearer launch is required.

Without opt-in, existing packet-only qualification still works. If opted in
but admission fails (expired lease, occupied transport, capture failure), the
native launch closes with `host_audio_failed`; it must not silently claim to
be streaming. Firmware reboot starts with Conversate and streaming off.

## Wire format (existing live-pcm-v1)

All integers below are little-endian. The outer transport remains zero-prefixed
COBS with type u8, sequence u16, payload length u16, payload, CRC16-CCITT-FALSE.
Payload ceiling is 1,024 bytes. Frame sequence starts at 0 for BEGIN and wraps
modulo 65,536; validate it modulo that width, not as a never-wrapping integer.
Frame payloads:

| Frame | Payload fields, in order |
| --- | --- |
| BEGIN `0x10`, 28 bytes | version u8=1; flags u8=2; source u8=2 (G2); format u8=1; rate u32=16000; exchange u64; controller u64; logical chunk u16=2048; reserved u16=0 |
| PCM `0x11`, 24 + 2N bytes | version u8=1; flags u8=2; exchange u64; controller u64; first sample offset u32; N u16 (1..500); N signed 16-bit mono samples |
| END `0x12` / ABORT `0x13`, 30 bytes | version u8=1; reason u8; exchange u64; controller u64; samples sent u32; PCM CRC32 u32; known dropped samples u32 |

Flag bit 0 remains synthetic, bit 1 identifies native Conversate. The CRC32 is
the existing IEEE PCM-byte CRC (golden `123456789` = `CBF43926`), not a CRC of
frame headers. The counter limit is the v1 u32 sample offset: before overflow
(about 74.6 hours at 16 kHz), the stream explicitly aborts with reason 9.
There is no 30/60-second recording deadline in native mode. Packet sizing can
vary; never assume an exact number of PCM frames per BLE notification.

Existing abort codes 1..7 remain lease expired, auth lost, link lost, released,
host request, TX backpressure and internal error. New codes: 8 source lost or
audio integrity failure; 9 sample-offset limit. A missing terminal frame after
link/auth loss is an incomplete stream, not an implicit successful END.

## Stop, pause and quality

Normal wearer exit, explicit session stop or a qualification deadline freezes
decoder input, drains the already-buffered tail and sends END. The HAL claim
remains exclusive until drain/abort finishes. Exact-ID hooks cannot pause or
terminate a successor conversation. A boot nonce and monotonic counter form
each native exchange ID. Session identity is separate from 8-bit BLE magic
tokens and 16-bit UART sequence counters.

Explicit native PAUSE freezes decoder input while keeping the session/lease
alive; RESUME unfreezes it. No silence is synthesized during a pause, so v1
sample offsets describe captured audio, not wall-clock time. Display-off and
transcription/AI-cue visibility switches do not pause capture. For a future
wall-clock-aligned archive, add pause/timestamp metadata before promising
timestamp-accurate transcripts across pauses.

`liveaudio status` now exposes `paused=0|1` for the active stream. A host may
extend its idle timeout only after verifying paused=1 together with the exact
exchange/controller, active Conversate mode, current lease/session epoch,
remaining lease and no pending abort. It must keep that proof fresh; a pause
does not turn off link-loss detection or waive integrity checks.

Lease expiry, login replacement, link failure, 100-ms frame admission timeout,
source disconnect, ring overrun, decoder failure/concealment, BLE loss or
duplicates cause ABORT, not a misleading clean recording. Dropped-sample
counts cover known unsent decoded samples, not an exact reconstruction of
missing BLE audio. A source error can therefore have dropped=0. The G2 owner
closes the native session when its admitted stream fails; it does not restart
automatically. Decode and notify callbacks never wait for UART or do disk I/O.

## Remaining qualification and CM5 work

- Flash this audio integration and first verify opt-out keepalive behavior.
- Deploy the companion implementation inside its existing UART owner, after
  inspecting/backing up the running service and local configuration. It now
  supports the Conversate flag, shared readiness renewal, stream identities,
  sequence wrap, offsets, CRC and terminal/error handling. No authentication
  bypass or second UART process.
- Bench-test live audio quality and sustained delivery, close/tail/reopen,
  receiver stall, both-arm reconnects, display-off and native pause/resume.
  Existing packet-only tests do not prove decoded PCM quality or UART timing.
- Qualify the implemented fan-out to streaming STT and an optional recording
  writer, with bounded queues and explicit disk/STT failure reporting. PCM
  costs 115.2 MB/hour before headers/compression. Saving and transcription can
  consume the same audio without a second microphone stream.
- Add session-fenced partial/final text return to native TRANSCRIBE command 6
  after verifying append/replace semantics; no “STT working” claim until this
  loop is tested end-to-end. Translation and AI cues remain separate consumers
  of the same session, not additional capture implementations.

Host checks compile the actual HAL and full live-audio module with mocked
hardware/RTOS boundaries. They cover auth/opt-in, exclusivity, exact IDs,
startup cancellation, native-vs-E0 separation, tail drain including an END
race, pause, 120 seconds of PCM, sequence and millis wrap, source/lease/auth/
backpressure faults and offset overflow. Existing G2 owner tests exercise
native integration and two-hour keepalive simulation. These are deterministic
software tests, not proof of real multi-core timing or hardware audio quality.

Final software checkpoint: all 47 host tests passed with ASan/UBSan. The XIAO
build passed at 5,811,584 bytes, leaving 172,672 bytes (3%) in the app partition.
Image SHA-256:
`ea9ec2bcfcd8d9aab85a2cebbe20440292bf4a4e278a8652a5cc8d3af084af47`.
This image has **not** been flashed. The earlier qualified keepalive firmware
remains on the board. Rapid reopen is queued until the previous audio stream
has fully drained and completed its terminal frame, including the interval
after HAL release but before UART finalization.
