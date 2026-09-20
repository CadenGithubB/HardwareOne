# Conversate lifecycle investigation — Even Android 2.3.0

Investigation date: 2026-09-20. This records the interoperability analysis,
implementation, and subsequent application-only XIAO deployment at 08:07 EDT.
Pi software is out of scope. The new packet-only lifecycle firmware is deployed;
sustained phone-free microphone and UI qualification remain pending.

## Why the previous diagnostic said “Conversation saved” and exited

The previous Hardware One diagnostic deliberately ended a session after its
configured duration. `G2_ConversateSession::check` returned `Deadline`, and
`g2ConversateStop` sent native CLOSE. Its last enable command was
`g2conversate on 30`; `on` then aliased the old timed test behavior.
Making that handler reusable fixed re-entry, not that per-session deadline.

The post-update serial capture now shows two complete runs without another
enable command or XIAO reboot:

- Uptime 723655 ms: deadline, 588 packets, 5/5 heartbeat ACKs, maximum gap 120 ms.
- Uptime 765361 ms: deadline, 588 packets, 5/5 heartbeat ACKs, maximum gap 105 ms.

This confirms re-entry works on this image. It also confirms that the timed
exits were host policy, not evidence of a glasses microphone duration cap.
The native saved message is **not proof of a saved file**: Hardware One's
diagnostic does not save audio or transcripts.

A third run ended differently. Following SyncInfo `bg=11 fg=34` and then
`bg=11` without a foreground app, the next heartbeat received COMM_RSP 162,
magic 100, error 1 at uptime 882357 ms. Hardware One immediately sent CLOSE:
492 packets, 5 heartbeats, 4 successful ACKs, maximum gap 102 ms. This is a
separate error-handling defect/qualification gap, not a deadline. No preceding
Conversate STATUS/CLOSE or END_SESSION request was observed in that interval;
the reason the glasses rejected that heartbeat remains unproven.

## Evidence and reproducibility

The connected Pixel Fold reported `com.even.sg`, versionName **2.3.0**,
versionCode **236**, lastUpdateTime `2026-09-20 03:22:01`. Only installed APK
files were pulled; no account database, credentials or private app data were
needed. Bluetooth was off at APK inspection. No new phone/glasses capture or
firmware flash occurred during that APK analysis. A subsequent stock-app
capture is documented below; it also required no firmware changes.

Local, ignored artifacts:

- `.scratch/even-phone-2.3.0-build236/base.apk`
  SHA-256 `9a44633545da693b0cf28b3172fe71a2297a5c6f8bb579e53ca4a48e0fdb97e4`.
- `.scratch/even-phone-2.3.0-build236/arm64.apk`
  SHA-256 `d661cf54cd94df002e60fdb121420ff130739d3a41f8691ed25851fad7e85d46`.
- `.scratch/even-phone-2.3.0-build236/blutter-out/asm/`, `objs.txt`, `pp.txt`.
- `.scratch/btsnoop/conversate-test1-20260920-031411/` contains the earlier
  phone Bluetooth capture; its app-version equivalence to 2.3.0 is not assumed.
- `.scratch/g2-conversate-console.log` contains the XIAO bench events above.

The application logic is compiled Dart ARM64 in `libapp.so`. It was inspected
locally using [Blutter](https://github.com/worawit/blutter), commit
`4a60ac648bf448c5a7596437243bcd0b9376fdf0`, with the matching Dart 3.11.5 runtime
and snapshot hash `78da37fed6bf1489361a312568249f3f`. Python dependencies and
analyzer builds are isolated under `.scratch`; no APK was uploaded. The
analyzer emitted one instruction-analysis warning in HERE SDK's token cache,
outside the Conversate methods discussed here, and completed its dumps.

These are annotated native disassemblies and recovered constant objects, **not
the original Dart source**. Function names, object constants and inspected
branches are evidence; unexecuted recovery paths still require device tests.
The earlier 2.2.9 build 230 dump was investigated first, then the key findings
were checked against the actual installed 2.3.0 build.

## What the installed application actually does

### Keeps the native session alive independently of captions

`conversate/conversate_service.dart`:

- `_startCustomHeartbeat`, address `0x17c9f6c`, creates a repeating timer using
  Duration object `0x2a5bbc1`: 5,000,000 microseconds, or **5 seconds**.
- `_sendCustomHeartbeat` keeps this path active when `_isInLiveSession` is
  true. `_isInLiveSession` accepts **preparing, active and paused**. Outside
  those states it cancels the timer. Paused is not the same as ended.
- `_sendHeatBeat`, `0x17ca10c`, resets its failure counter after success.
  Failure increments it; the close-attempt branch starts at **12 consecutive
  failures** (`cmp x2, #0xc`). That is a count, not an exact wall-clock timeout.
- `ConversateActionCallbackImpl::tryCloseByLossHeartBeat`, `0x17ca1bc`, checks
  the glasses-required configuration before attempting session close.

`even/pages/conversate/callback/conversate_proto_callback_impl.dart`:

- `sendHearBeat`, `0x17ca2b8`, creates an empty ConversateHeartBeat and sends
  command **255** through the G2 transport. It checks COMM_RSP error status;
  an explicit non-success response is a failure, not a successful heartbeat.
- This agrees with the earlier phone wire capture: nine service-0x0B
  heartbeats roughly five seconds apart, separate from 35 caption updates.

Our diagnostic has the right basic heartbeat transaction, but its policy of
closing on the **first** matching nonzero ACK is much more aggressive. A
bounded consecutive-failure policy is justified by the app evidence; simply
ignoring failures or fabricating ACKs is not.

### Has real pause/resume and a separate end state

Recovered LiveSessionStatus names are init, connecting, preparing, active,
paused, error, ending and ended. These application states must not be confused
with wire control numbers.

Recovered native control values in `objs.txt`:

- CONTROL command 1: START=1, CLOSE=2, PAUSE=3, RESUME=4, CONFIG=5.
- STATUS_NOTIFY command 161 carries the corresponding native status.
- COMM_RESP command 162: success=0, generic failure=1, network failure=2,
  fallback failure=3.

`ConversateModuleService::_handleConverseNotify` handles native start, close,
pause and resume separately. `ConversateService::pauseConversate` only accepts
preparing/active states; resume requires paused and guards concurrent resumes.
The audio layer serializes capture operations and rejects obsolete start
completions. Recovery explicitly preserves a user's pause intent instead of
silently resuming recording.

The service also has `_startPauseHeartbeat`, but it calls an **HTTP** pause
heartbeat using the conversation ID, initially and every five minutes. This
is cloud-session maintenance, not a replacement for the five-second glasses
heartbeat. Hardware One should not depend on Even's cloud API to hold its
local native mic session.

### Answers the glasses' menus and end requests

The installed app's recovered command IDs include:

- 164: OS_REQUEST_INTERFACE_SWITCH; 165: APP_RESPONSE_INTERFACE_SWITCH.
- 166: OS_REQUEST_LANG_SWITCH; 167: APP_RESPONSE_LANG_SWITCH.
- 168: OS_REQUEST_END_SESSION.

The interface-switch handler validates the requested transcription/AI-cue
flags, applies the supported configuration, and sends an explicit response.
The language-switch path also has a response, bounded waits and recovery.
The end-session handler feeds the real close flow. These are not arbitrary
menu taps that an implementation can safely ignore forever.

Our driver currently recognizes only PREP, SELECT, STATUS and ACK, and treats
non-close STATUS as unsupported. The earlier Interface/Transcription menu
interaction therefore exposed a real implementation gap. The APK does not
prove that this gap caused the third run's heartbeat rejection; no matching
menu request was captured in that run's final interval.

### Serializes close and subsequent start

`ConversateService::startConversate` waits for an in-flight glasses close.
The protocol callback implements bounded START/CLOSE retries and timeout
handling. The close path also handles application/history bookkeeping; that
is separate from sending native CLOSE and separate from keeping the mic alive.

I did not find a 30-second session-expiry policy in the inspected native
lifecycle paths. This is not a claim that every app, cloud or glasses failure
can keep Conversate open indefinitely.

## Proposed Hardware One implementation sequence

This was the investigation plan. The source implementation is now summarized
under **Implementation status** below. It is now flashed and boot-checked,
but the new lifecycle has not yet been qualified on the glasses.

1. **Separate ordinary use from qualification.** Make `on` enable a
   user-ended session with no arbitrary elapsed-duration cutoff. Keep
   `test <seconds>` explicitly finite; no longer alias the two. Keep `stop`
   for ending one session and `off` for disabling future launches. Do not
   silently persist recording enablement across boots.
2. **Implement a session state machine.** Distinguish available, preparing,
   running, paused, recovering and closing. Ending frees the mic but preserves
   availability. Native pause frees/suspends audio delivery while preserving
   the native session and heartbeat. Resume must be explicit and current.
3. **Retain health bounds, not a recording-duration bound.** Keep five-second
   heartbeat scheduling, exact connection/token checks and audio telemetry.
   Track consecutive heartbeat failures and bounded retries. Continue the
   independent no-audio watchdog only while audio is expected; a deliberate
   pause must not trigger the eight-second audio-stall shutdown.
4. **Handle native control requests explicitly.** Add STATUS PAUSE/RESUME and
   END_SESSION, then menu request/response handlers. Advertise only supported
   capabilities; decline unsupported translation/AI operations explicitly.
   Protect terminal controls from RX-queue loss and fence late ACKs/callbacks.
5. **Complete CLOSE before accepting a new lease.** Add an acknowledged or
   bounded-time close phase without replaying old audio or old queued START.
   Preserve the user's pause/exit intent through interrupts and reconnects.
6. **Keep the first implementation G2-only.** Qualify native microphone
   continuity and UI lifecycle first. Then connect one decoded-PCM consumer to
   the CM5 transport and route returned text through TRANSCRIBE_DATA command 6.
   Pi STT/recording fan-out is separate work; no Pi changes are proposed here.

## Targeted stock-app capture before implementing uncertain wire behavior

Use the connected Pixel with Bluetooth HCI logging confirmed, and make the
phone the only glasses controller for this test. Do not erase bonds or logs.
Capture synthetic speech only; avoid personal conversation.

1. Start native Conversate; speak a short counting sequence, then leave it
   active beyond the former test cutoff, including a quiet interval.
2. Open the exit dialog and choose **No**; leave the session running afterward.
3. Pause and resume once, if available. Identify audio stop/restart, native
   controls and whether five-second heartbeats continue during the pause.
4. Change Interface/Transcription once and back. Capture the exact request,
   response, error fields and any effect on mic delivery.
5. Exit with **Yes**, then reopen without restarting either device. Capture
   CLOSE/END_SESSION ordering, acknowledgements and fresh PREP/SELECT.

Only then mirror the observed behavior in firmware and run the same cases
phone-free. Add a long packet/decoded-audio run crossing token rotation and
interruption tests. A successful ACK stream alone is not an audio test.

## Follow-up stock-app capture — 2026-09-20, 06:53–06:56 EDT

The user completed a second capture with the Pixel as controller. They counted,
waited in silence, canceled an exit confirmation, toggled the glasses display
off (not a microphone pause), toggled transcription and AI cues off/on, changed
translation selections, closed from the tap-and-hold menu, reopened, and closed
through the confirmation dialog. There was no pause button. Neither exit
method invalidated the test.

Before the test, Android's Bluetooth diagnostics explicitly reported
`btsnoop log is enabled`. The earlier empty settings/property query was not
evidence that logging was disabled. ADB bugreport retrieval completed and the
HCI logs were extracted locally under the ignored directory:

`.scratch/btsnoop/conversate-lifecycle-test2-20260920-065904/`

- Current log SHA-256:
  `49b29dcc17240adfa314a23f93b7b5fa909c473d61411e281ccf49f38b422eed`.
- Combined current/previous log SHA-256:
  `5eb18040220666606d6a73048e9bc1168af5a03076725c7f058a7ec3f30d3003`.
- Reproduce the redacted analysis with
  `.scratch/conversate_lifecycle_capture.py <combined-log> --after 2026-09-20T06:51:00`.
  It uses the existing connection-aware HCI/ACL/ATT reassembler; it does not
  display transcript/cue content, audio bytes or device addresses.
- The focused interval contains 389 reconstructed G2 messages, all passing
  CRC, with no unterminated ACL fragments. No BLE reconnect occurred between
  the two Conversate sessions.

### Observed actions and microphone continuity

All timestamps below are local EDT, derived from HCI timestamps.

- First session: START at **06:53:40.852**, followed by **2,756** 205-byte
  microphone packets from **06:53:41.264 to 06:55:59.758** (138.494 seconds).
  Maximum inter-arrival gap was **121.077 ms**, with no gap over 0.5 seconds.
  All **27** native heartbeat requests in this session received success ACKs.
- Exit-dialog overlays (`SyncInfo bg=11, fg=34`) appeared and cleared at
  **06:54:22–25** and **06:54:32–34**, without CLOSE or audio interruption.
  These are compatible with the user's canceled-exit action; HCI alone is not
  a screen recording or a label for every individual local gesture.
- The entire reported display-off interval is within that uninterrupted
  stream. No native PAUSE/RESUME control or status was observed. This supports
  **display visibility is separate from microphone/session lifetime**; do not
  use a dark screen or foreground-overlay change as an implicit mic stop.
- Transcription off/on requests occurred at **06:55:07.603 / 16.638**;
  AI-cue off/on requests at **06:55:21.703 / 27.645**. Each received a successful
  interface response after approximately **219–243 ms**. Audio continued.
- Translation requests were **AUTO, ES, FR, OFF** at **06:55:38.355, 42.346,
  44.685, 49.996**. Each received a successful language response; audio did
  not stop during any of these switches. The final selection was OFF.
- First close: RX **STATUS 161, CLOSE=2** at **06:55:59.776**; the phone sent
  native CONTROL CLOSE at **06:55:59.842**. The fresh PREP arrived at
  **06:56:10.072**, with another START at **06:56:11.399**. The second session
  delivered **215** microphone packets over **10.739 seconds** before closing.
- Second close: RX **STATUS 161, CLOSE=2** at **06:56:22.637**; phone CLOSE at
  **06:56:22.705**, successful ACK at **06:56:22.755**. Both reported exit methods
  therefore used STATUS/CLOSE in this capture. **Command 168 was not observed**;
  its existence in the APK is not evidence that this firmware used it here.

Packet continuity does not prove decoded-audio intelligibility, and this
phone-backed run is not qualification of the unfinished ESP32 implementation.

### A rejected heartbeat also occurs with the stock app

During the second session's final exit sequence, the foreground exit dialog
cleared at **06:56:21.286**. The phone sent heartbeat magic 159 at
**06:56:21.346**, and the glasses replied **error 1** at **06:56:21.406**.
The app did **not** immediately send CLOSE. Microphone packets continued until
**06:56:22.589**, and the explicit native CLOSE status arrived at **22.637**.

Thus 28 of the capture's 29 heartbeats succeeded; one was rejected near a
normal exit. This is direct evidence against treating every single rejected
heartbeat as an immediate unsupported-state terminal failure. It does not
prove why the prior XIAO heartbeat was rejected or validate ignoring sustained
failures. Preserve explicit user exit handling and bounded failure recovery.

### Capture-backed wire details for the next implementation

- Interface request **164** uses wrapper field **14**, nested fields
  **1=transcribe, 2=aiCue**. Zero-valued request fields can be omitted: the
  transcription-off request contains only `f2=1`; AI-cue-off only `f1=1`.
  Do not incorrectly interpret an absent proto3 scalar as unchanged/unknown.
- Response **165** uses wrapper field **15**, nested fields
  **1=error, 2=transcribe, 3=aiCue**. All four observed replies used error 0 and
  the requested resulting flags, and echoed the request magic.
- Language request **166** uses wrapper field **16**, nested bytes field
  **1=language key**. Response **167** uses wrapper field **17**, nested
  **1=error**. All four responses reported success. AUTO/ES echoed magic;
  FR changed **148 → 149**, and OFF **151 → 153**. Do not generalize mandatory
  request-magic echo to all stock language replies; investigate that path's
  allocator/retry behavior before declaring a rule.
- These captured UI toggles did not restart the native microphone and should
  not be modeled as pause/stop operations. Actual pause/resume remains
  APK-derived behavior, not a tested interaction from this capture.

## Implementation status

Implemented on `codex/g2-audio`, G2/ESP32 only:

- Separate `on` (user-ended, no elapsed cutoff) from `test [seconds]` (finite).
  Runtime opt-in remains off at boot. `stop` ends a session; `off` also disables
  new launches. Neither command changes persistent settings.
- Preparing/running/paused/closing states, five-second heartbeat scheduling,
  12-consecutive-failure policy, and an independent running-only audio watchdog.
  Recovery is represented by the consecutive failure counter while retaining
  the current phase; it never silently reopens a stopped microphone.
- Three-second bounded CLOSE completion with ACK correlation, audio fencing,
  safe deferral of a fresh PREP, and cancellation on newer dismissal/off/link
  changes. The mic remains reserved through closing; FAST link priority is
  released when closing begins.
- Captured interface request/reply fields and proto3-zero semantics. Flags are
  UI visibility preferences only; no STT/AI generation is implemented. Language
  replies support OFF and explicitly decline unavailable translation. The
  advertised list is now OFF-only instead of copying unsupported app languages.
- APK-derived native pause/resume and END_SESSION handling, idempotent pause/
  resume replies, and no pause inference from SyncInfo/display visibility.
- Bounded RX admission for CLOSE/PAUSE and a local cancellation epoch carried
  across queued notifications and both levels of packet reassembly. Earlier
  queued launch packets cannot undo a later local stop/off.
- Extended the existing host target to compile actual owner/CLI/queue/parser
  functions, not a separate simulated state machine. Coverage includes a
  two-hour session, clock/token wrap, captured menu fixtures, heartbeat errors,
  pause/resume, deferred reopen, queue overflow, stale and split RX, competing
  owners, close failures/timeouts, and default-off/shutdown behavior.

The source still counts microphone packets only: no CM5, UART audio consumer,
STT return stream, AI output, cloud API or audio/transcript file was added.
Phone-free radio/UI qualification remains necessary, especially the APK-only
pause/resume/END_SESSION paths and OFF-only menu advertisement. The phone
capture proves the reference behavior, not the new ESP32 firmware's behavior.

Final software validation (2026-09-20): all 46 host tests passed with
ASan/UBSan enabled, and the XIAO ESP32-S3 build succeeded. The application is
5,807,376 bytes with 176,880 bytes of partition headroom (about 3%). Firmware
SHA-256: `89a860a6a5d0ba6c4288f2c5843417c5c7a3600be59853056c427ca9d05be71c`.
This image was subsequently flashed application-only at `0x10000` on the
confirmed XIAO at approximately 08:07 EDT. Esptool verified the write; boot
logs matched the built ELF and showed successful filesystem/Wi-Fi/web startup.
The web interface responded after reboot. Existing bootloader, partition table,
NVS and LittleFS were not flashed. The previous application was backed up;
deployment evidence and its hash are recorded in `G2_CONVERSATE_MIC.md`.
Sustained Conversate hardware qualification is the next step, not a completed
result; no microphone session was started during this deployment check.
