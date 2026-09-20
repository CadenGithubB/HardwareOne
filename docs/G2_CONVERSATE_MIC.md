# Native Conversate microphone keepalive

## Scope and evidence

The integration is an **opt-in, packet-only native session service**, not
a replacement STT implementation. Normal micrecord / dictation / EvenAI behavior
and their 30/60-second recording limits are unchanged. No Pi code, transcripts,
audio files, persistent settings, or HTML-game files are changed by this test.

The current source separates normal user-ended sessions from timed tests:
`g2conversate on` has **no elapsed-duration cutoff**; `g2conversate test 300`
is explicitly finite. The lifecycle implementation described here has host
test coverage but still requires a new phone-free hardware qualification run.
See [the APK and capture investigation](G2_CONVERSATE_LIFECYCLE.md) for evidence.

The stock Even app capture on 2026-09-20 showed:

- Native control on the right temple, service `0x0B`; mic audio on left `6402`.
- PREP request (2), empty note list reply (3), SELECT (4, `isSkip=1`), START (1).
- Heartbeat command 255 every approximately 5 seconds, wrapper field 11 empty.
  Unlike EvenAI, there is **no nested heartbeat counter**. ACK 162 echoes magic.
- START settings and prep reply include translation-language label extensions
  absent from the older public protobuf schema. The current builders retain
  the captured `OFF` label and omit unavailable translation choices. START
  echoes SELECT magic; CLOSE echoes STATUS.
- 982 205-byte audio packets over 49.32 seconds, with no arrival gap over 0.5 s.
  No E0 AudioCtrl writes occurred during that interval. This supports testing a
  native session lease; it does **not yet prove** multi-minute phone-free capture
  or that heartbeat alone is sufficient without transcript traffic.
- Native STATUS 161 / CLOSE=2 ended the session. Pause/resume was not captured.

The sanitized wire fixtures live in `test/host/g2_conversate_harness.cpp`.
Private HCI data and transcript text stay in ignored `.scratch/btsnoop/`.
Capture SHA-256 (btsnoop_hci.log):
`a40be501c1503996216c66b3c35b6e42c67c5ae161cf25986b89b69c30bf9c1e`.

## How it fits Hardware One

`System_G2_Protocol` builds native prep/start/close/heartbeat and menu reply
envelopes and strictly parses supported RX events. `G2_ConversateSession.h` contains the
dependency-free lifetime/watchdog policy. `G2_Glasses.cpp` attaches it to the
existing G2 control owner and audio subscription; no additional task is created.

The existing E0 microphone path reasserts AudioCtrl while a finite recorder is
busy. The native path instead retains its own session, independent of the WAV
recorder and E0 lens container. It never paints a "Listening..." page, re-arms E0,
or writes audio to storage. It holds FAST link priority from START through
running/paused states and releases FAST when closing begins.

Only the control owner sends native packets. Both temple connection generations
are bound when a fresh native PREP arms a session; TX is fenced to the right generation again under the BLE
TX locks. Only valid left 205-byte packets count as audio. Received heartbeat
ACKs must match the one outstanding magic token. Disconnect/reconnect or an
unsolicited SELECT cannot revive a finished session. Locally generated tokens
rotate through 1–250 (the driver's native byte-token convention), excluding the
current session's PREP/START tokens, eight recent control/menu response tokens,
and the last outstanding heartbeat token. A peer menu/START token colliding
with a pending heartbeat retires that ambiguous heartbeat as one failure; its
ACK cannot be counted as heartbeat liveness.
Unsolicited/stale ACKs never move the allocator. The byte protocol cannot tell
an ACK delayed through an entire token rotation (~20 minutes) from a current
ACK; continuing audio is therefore an independent requirement, not optional.

Tests refuse an existing HAL capture, raw/WAV dump, EvenAI session or custom lens
page. Legacy mic-on/off and recorder auto-page creation are blocked during the
native lease. While **preparing, running, paused or closing**, an EvenAI wake is declined with an EvenAI EXIT
on its exact originating temple generation, before any EvenAI exchange or host
capture is started. Idle availability still permits normal EvenAI use. This
does not distinguish deliberate from accidental wakes: exit Conversate first
to use EvenAI. Declining a wake does not restart Conversate or mask lost audio;
the normal audio/ACK watchdogs still apply. The policy needs hardware validation.
A competing capture/page, native exit, link loss, TX failure or test expiry ends
the session. Stop releases link priority and sends native CLOSE
without writing to a replacement connection. The session stays **closing**,
rejecting audio and holding the mic reservation until its success ACK or a
three-second deadline. Disconnect/shutdown or a failed CLOSE write finish
locally without waiting. Ambiguous close tokens wait for the deadline instead of trusting an
old ACK. A fresh PREP received during closing can be deferred until completion;
new dismissal, disable or connection change cancels it. No auto-resume. Runtime service
availability survives session completion: a fresh native PREP/SELECT can start
another session without a CLI re-arm or XIAO reboot. Idle availability holds no
microphone or FAST lease, sends no heartbeats, and preserves the last counters.
Every new PREP rechecks readiness and competing owners before binding current
temple generations. A SELECT, ACK, audio packet or reconnect alone cannot start
capture. Availability is off at boot and is not written to persistent settings.

Session and diagnostic bounds (not claims about firmware limits):

- No expiry while enabled and waiting for a native launch. Once PREP arrives,
  the handshake has 60 seconds to reach START.
- `on`: no elapsed-time cutoff. `test [seconds]`: 300 seconds by default,
  configurable 10–3600 seconds per session (including any intentional pauses).
- 8 seconds without a first/new audio packet **while running** stops with
  `audio_stall`. Paused time is excluded and resume gets a new first-audio grace
  period. Silence in speech is not silence in BLE packets.
- Native heartbeats are due every five seconds while preparing/running/paused.
  At most one is outstanding. Its 12-second ACK timeout counts as one failure,
  then permits a fresh token; there is no retry flood. Rejection also counts as
  one failure. Success clears the consecutive count. **12 consecutive failures**
  stop with `heartbeat_lost`; this is a count, not a fixed 60-second deadline.
- Native STATUS PAUSE/RESUME preserves the session; only an explicit RESUME
  resumes audio expectation. These paths are APK-derived and not yet physically
  qualified. Display-off and foreground overlays do not imply a pause or exit.
- Interface requests receive error/status plus both resulting visibility flags;
  missing proto3 flag fields mean zero. These are UI preferences, not an STT/AI
  implementation. Only translation OFF is advertised; other language requests
  receive explicit failure without restarting/stopping the mic.
- Both captured close methods use STATUS CLOSE. END_SESSION command 168 is
  also handled based on the APK, though not observed in the capture.
- CLOSE/PAUSE are admitted despite a full ordinary RX queue; PAUSE cannot evict
  a queued CLOSE. Local stop/off/failure advances a receive epoch so already
  queued or partly reassembled PREP/SELECT cannot resurrect the old session.

## Bench procedure

1. Disconnect the phone from G2. Connect both temples to Hardware One.
2. Stop other recordings, close the HAL microphone (`closemic`), and leave any
   Hardware One custom lens page. Do not run mic-on, raw dump or WAV commands.
3. Send `g2conversate on`; verify status says `enabled=on idle limit_s=0`
   (fields may have other telemetry between them). For a finite diagnostic use
   `g2conversate test 300`. Old `on 300` syntax is rejected, not silently timed.
4. Open **native Conversate** on the glasses. The empty note
   list may skip directly into capture; choose **Skip & Start** if offered.
   No transcription text will appear: this first test sends
   no STT results. Verify status says `running` and packets increase.
5. Check at 15 s, beyond 60 s, and beyond 300 s. Expect approximately 20 packets/s,
   increasing heartbeat/ACK counts, small `silent_ms` and no sustained gaps.
   `lost` uses the 8-bit trailer only for gaps under 1 s; longer gaps are reported
   as `max_gap_ms`, not a fabricated exact loss count.
6. For interruption qualification, deliberately invoke EvenAI once while the
   session is running. Expect `evenai_declines` to increase, no EvenAI capture,
   and continued Conversate audio. If the display or audio stops, report it:
   the policy is not yet hardware-qualified and must not silently re-arm.
7. In normal mode, use native exit / `g2conversate stop`; only `test` expires.
   Confirm `closing` then `idle`, the expected reason, and `close_ack` or
   `close_timeout`. Retained packet counters make the
   final status inspectable. Reopen native Conversate without another CLI
   command: PREP/SELECT should start a fresh run with reset counters. Also test
   early exit and disconnect. `stop` ends just the current session; `off` stops
   it and disables future launches. Use `g2conversate off` when finished.

Do not claim keepalive is validated merely because ACKs increase: **audio must
continue**. Packet continuity also does not prove intelligibility; later qualify
decoded PCM with a spoken timing marker and a recording-capable sink.

Host check:

```sh
python3 components/hardwareone/test/host/test_g2_conversate.py --sanitize
```

This compiles the full production protocol, session policy, owner, CLI and
receive-queue/reassembly function bodies against mocked platform/transport
boundaries. It tests captured menu/close/heartbeat bytes, OFF-only capability
advertising, malformed RX, and a simulated **two-hour** user-ended stream
(144,000 packets, 1,439 ACKs, multiple token and millis wraps). It covers
bounded heartbeat recovery, paused heartbeat/audio expectations, close ACK and
timeout, deferred fresh PREP, reserved/ambiguous tokens, stop/off precedence,
generation mismatch, queue overflow, split-notification cancellation, EvenAI
declines and TX/audio failures. It is not a radio, FreeRTOS scheduling or
decoded-audio intelligibility test. Historical bench entries below describe
earlier firmware and retain their original command syntax/results.

### Current lifecycle implementation validation (2026-09-20)

All 46 host tests passed with ASan/UBSan enabled after the final source change.
The XIAO ESP32-S3 build passed: `build-xiao_s3/hardwareone-idf.bin` is
5,807,376 bytes, leaving 176,880 bytes (about 3%) in the application partition.
Image SHA-256:
`89a860a6a5d0ba6c4288f2c5843417c5c7a3600be59853056c427ca9d05be71c`.
This image was flashed on 2026-09-20 at approximately 08:07 EDT, as recorded
below. Sustained phone-free microphone operation, menu behavior, and exit/re-entry
still need on-device qualification. No Pi,
game, STT or recording-file implementation was changed in this lifecycle step.

### Current lifecycle deployment (2026-09-20 08:07 EDT)

The reconnected XIAO was verified over its USB serial port: ESP32-S3,
8 MB flash and 8 MB PSRAM. Its on-device partition table matched the build
byte-for-byte before flashing. The complete previous factory application
partition was backed up to ignored
`.scratch/g2-lifecycle-flash-LffZ96/application-before.bin` (5,984,256 bytes;
SHA-256 `117d348cb6a205599a0e0bc21ba52e2078bd142b902806fc18083c2c26ffb903`).

Only the application at **0x10000** was written; esptool verified its hash.
Bootloader, partition table, NVS and LittleFS were not flashed or erased.
The captured reboot reported ELF SHA prefix `481c5f935`, matching the built
ELF, successful PSRAM self-test and LittleFS mount, the existing Wi-Fi connection,
and completed setup. HTTP returned 302 after reboot.
The 30-second startup capture contained no panic, assertion or backtrace.
Evidence: `.scratch/g2-lifecycle-flash-LffZ96/boot-after.log`.
Conversate was not enabled or tested during this deployment check.

### Historical diagnostic builds and bench results

Initial software validation (2026-09-20): 46/46 host tests passed with ASan/UBSan
enabled; XIAO ESP32-S3 firmware built successfully at 5,801,440 bytes (182,816
bytes app-partition headroom). Physical duration/exit testing remains separate
from those software checks.

Bench deployment: the confirmed XIAO's previous application partition was backed
up locally to ignored `.scratch/g2-before-conversate-app.bin` (5,984,256 bytes;
SHA-256 `e79c22451b86fa341c679786fee773b58cd9d76bdcb0b447acc6400760e04aa9`).
The new app (`e689754c792ce4983239718b3c49864b20b212e0acfa3f173d1409c88033f56e`)
was flashed at **0x10000 only**, and esptool verified its hash. Bootloader,
partition table, NVS and LittleFS were not flashed or erased. Web service returned
after reboot; existing web sessions require a fresh login as expected.

First on-device attempt: both temples connected via `openg2 saved`. At
04:14:45 local, `g2conversate test 300` armed successfully. No native `0x0B`
request/start was received within the 60-second entry window. At 04:15:46 it
stopped with `arm_timeout`, `packets=0 hb=0 ack=0 close_sent=0`. This validates
the idle arming timeout only; **no sustained-microphone result yet**. The next
attempt should start with the wearer already at the native Conversate entry.

Second on-device attempt: armed at 04:19:18; native PREP/SELECT/START completed
at 04:19:32, and left audio began at 04:19:33. At 04:20:09 the glasses sent an
EvenAI WAKE (0x07), followed by SyncInfo `bg=11 fg=7`. The initial mutual-
exclusion guard deliberately closed Conversate (`reason=busy`), after 725
packets and 7/7 heartbeat ACKs; largest inter-packet gap was 105 ms and the mic
diagnostic reported no loss/stalls. EvenAI then exited because no authenticated
Pi host was available. This explains the user's dark display; it was **not** a
Conversate heartbeat or audio-stall timeout. At this point sustained >60-second
capture remained unqualified. The user was unsure whether the wake had been
triggered inadvertently and requested a repeat before changing the interruption
policy; no firmware change was made between these runs.

### Five-minute hardware result: PASS (same firmware)

The repeat was armed at 04:22:50; native START completed at 04:23:49. At 04:28:49
the control owner closed it at the configured 300-second deadline:

```text
stopped reason=deadline packets=5961 hb=59 ack=59 max_gap_ms=165 close_sent=1
```

Final `g2conversate status` confirmed idle/deadline, `lost=0`, `duplicates=0`,
and 59/59 matching ACKs. Rolling delivery stayed approximately 20 packets/s.
The legacy mic diagnostic's one stall was the gap BETWEEN the two runs, not a
stall during this run. Audio notifications ceased shortly after native CLOSE;
no new EvenAI wake occurred during the five-minute run. The phone remained
disconnected and the CM5 was not connected. Temporary G2 logging was disabled
again afterward. This qualifies native microphone packet continuity past the
existing 30/60-second recorder caps, **not** PCM intelligibility, STT, pause/
resume, indefinite operation, or unexpected foreground interruption recovery.

### Follow-up hardening

The follow-up changes the running-session EvenAI policy to decline the popup,
adds byte-bounded token rotation, reserves PREP/START tokens across rotations,
and ignores duplicate PREP requests without allocating additional tokens.
Status/stop logs expose `magic_wraps` and `evenai_declines`. The existing
30/60-second recorder limits and all Pi software remain unchanged. All 46 host
tests pass after these changes, including a one-hour production-owner simulation
with ASan/UBSan. Hardware results for this image follow below.

The follow-up XIAO build passed at 5,802,512 bytes, with 181,744 bytes of
app-partition space left. Application-only deployment at `0x10000` completed
and esptool verified the written hash; no settings/filesystem partitions were
flashed. Image SHA-256:
`e0428e0a014e1621d110b73ab03768bd2add692424b631703b31b9d744dabb6f`.

First follow-up hardware run: the right temple initially did not advertise;
after the wearer rebooted that arm, recovery promoted it and both links were
ready. A 120-second test was armed at device uptime 907634 ms and native START
was sent at 923896 ms. Audio arrived at approximately 20 packets/s. The wearer
explored the tap-and-hold Interface / Transcription options during this run,
so it is **not a controlled EvenAI interruption test**. No EvenAI WAKE/decline
was observed. Native CLOSE ended the test before its deadline, at 1038241 ms:

```text
stopped reason=native_exit packets=2265 hb=22 ack=22 max_gap_ms=182 magic_wraps=0 evenai_declines=0 close_sent=1
```

This is useful native-exit and packet-continuity evidence on the new image,
but does not qualify the EvenAI guard or token rotation. Later unsolicited
Conversate requests did not restart capture. The XIAO then rebooted following
an explicit web-user reboot command; boot diagnostics reported software reset
and crashCount=0. No keepalive test was active at that reboot. Logs remain in
the ignored local console capture; no firmware changes were made for the retry.

### Controlled EvenAI interruption: PASS (180-second hardware run)

After reconnect/reboot, a 180-second retry was armed at uptime 1468354 ms;
native START completed at 1483461 ms. At 1504354 ms the right temple sent a
real EvenAI WAKE. The owner declined it at 1504356 ms; the glasses acknowledged
EXIT and SyncInfo returned from `bg=11 fg=7` to `bg=11` without foreground AI.
The wearer confirmed a brief popup followed by its dismissal, with Conversate
continuing visibly throughout. No EvenAI host capture/exchange was started.

At 1663462 ms the configured duration ended:

```text
stopped reason=deadline packets=3574 hb=35 ack=35 max_gap_ms=536 magic_wraps=0 evenai_declines=1 close_sent=1
```

Rolling delivery stayed near 20 packets/s. One 536 ms delivery gap occurred
during later peer-initiated BLE connection-parameter renegotiation; the FAST
request recovered the link automatically. The packet diagnostic reported one
stall and no sequence loss. This is **not gap-free audio**, and packet-only
testing still cannot qualify intelligibility. Native CLOSE was acknowledged;
a short audio tail followed, and SyncInfo cleared Conversate approximately
1.9 seconds after CLOSE. The phone remained disconnected; the user had connected
the CM5, but this packet-only test does not depend on or exercise its software.

Remaining bench checks: a 25-minute run crossing a token rotation and a live
disconnect/reconnect with no automatic capture restart. Track BLE delivery gaps
and qualify decoded PCM separately. Continue checking packets, not ACKs alone.

### Restart diagnosis and reusable-session fix

On the same unrebooted XIAO, opening Conversate after the prior deadline sent
PREP request 2 (magic 66) at uptime 3268221 ms, but the one-shot handler was idle
and sent no reply. The glasses eventually showed a connection error. This was
not a CM5 dependency or a broken BLE link. After leaving the native app and
re-arming the existing firmware, another launch recovered without rebooting or
reconnecting either device. Its 30-second run ended at uptime 3377303 ms:

```text
stopped reason=deadline packets=588 hb=5 ack=5 max_gap_ms=105 magic_wraps=0 evenai_declines=0 close_sent=1
```

The fix separates runtime availability from each session's microphone lease.
Session completion leaves the handler available for another native launch;
only explicit `off`, control-owner shutdown or a reboot disables it. The
60-second timer now applies to an actual PREP handshake, not idle availability.
The per-session duration, audio/ACK watchdogs and exclusive-owner checks remain.
This addresses the ignored-PREP restart defect; it does not remove duration
limits or implement pause/resume, STT or recording. On-device exit/re-entry
qualification of this new fix is still required.

Software validation for the restart fix: all 46 host tests passed with
ASan/UBSan; the XIAO build passed at 5,803,360 bytes, leaving 180,896 bytes (3%)
in the application partition. Application-only flash at `0x10000` completed
and esptool verified its hash; bootloader, NVS and LittleFS were not flashed.
Image SHA-256:
`1130a1498c96543cc94bf320672408ef274a8065493a5eb633d0f8940fc72e86`.
Boot completed and web login recovered. `g2conversate on 30` was accepted at
uptime 101037 ms. At 201722 ms, status still reported `enabled=on idle`, reason
none, zero packets and a 30-second limit: idle availability survives more than
60 seconds and link loss without capturing audio. Before any native PREP/START,
the right temple disconnected, briefly reconnected (advertised RSSI -93 dBm),
then both links dropped. Exit/re-entry qualification is pending stable links;
these pre-session disconnects are not evidence for or against the restart fix.

## September 20 lifecycle qualification checkpoint

The later lifecycle firmware (`89a860a6a5d0ba6c4288f2c5843417c5c7a3600be59853056c427ca9d05be71c`)
was flashed and tested phone-free. A baseline session ran 14m56s with 17,825
packets, 179/179 heartbeat ACKs and no reported loss/duplicates. A controlled
session ran about 28 minutes, including display off/on and transcription/AI
cue menu toggles. It delivered 33,380 packets before a deliberate right-arm
reboot ended it with `disconnected`. Both arms subsequently reconnected and a
fresh wearer launch resumed normally without rebooting/re-enabling the XIAO.
Immediate close/reopen also worked. One short exit while the wearer was not
watching remains inconclusive, not an established firmware failure.

These results qualify native session persistence/re-entry and the tested
reconnect path, not decoded audio, transcription or saving. They supersede
the earlier pending re-entry qualification notes above. The new shared PCM
transport is documented in [G2_CONVERSATE_AUDIO.md](G2_CONVERSATE_AUDIO.md);
its CM5/audio hardware qualification is still pending.

## Next stages, after on-device qualification

1. Give a production Conversate capture an explicit audio owner and a single
   PCM consumer. Reuse LC3 decode and the existing framed UART live-audio
   transport; do not add two readers to the destructive HAL ring.
2. Replace the recorder-shadow lifetime with the native session lifetime, with
   bounded queues, host readiness/backpressure, session fencing and clean stop.
   Audit sequence wrap and UART bandwidth for hours rather than short clips.
3. Pi side (separate work): fan out each decoded chunk to a streaming STT engine
   and optional local recording. 16 kHz mono s16 PCM is about 115 MB/hour.
4. Add session-fenced partial/final transcript commands back to native 0x0B
   TRANSCRIBE (6). Capture suggests text updates replace the current segment;
   verify the exact semantics before implementing append/replace behavior.
5. Capture and qualify native pause/resume, interruptions, long runs, recovery,
   and battery impact before making this an everyday phone-free feature.
