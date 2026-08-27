# G2 native "Hey Even" session and dismissal contract

**Status (2026-08-09):** the hands-free native-window path was hardware-
validated before this revision. The exchange-ID dismissal revision is
implemented in the working tree, passes local host tests, and builds for the
XIAO; it still needs a coordinated CM5 deploy/XIAO flash and physical
dismissal testing. The current STT path is finalized-WAV/batch inference. Live
PCM transport, streaming production STT, and partial-question display are not
implemented.

Related records:

- [`G2_PROTOCOL.md`](G2_PROTOCOL.md) — capture-backed sid `0x07` wire format.
- [`../cm5/G2_EVENAI_RENDER_TEST_RECORD.md`](../cm5/G2_EVENAI_RENDER_TEST_RECORD.md)
  — optical-render proxy measurements and stable trial IDs.
- [`../cm5/LIVE_STT_G2_EXECUTION_PLAN.md`](../cm5/LIVE_STT_G2_EXECUTION_PLAN.md)
  — future live-STT gates; it is not a report of features already used.
- [`../cm5/CM5_DEPLOYMENT_PATHS.md`](../cm5/CM5_DEPLOYMENT_PATHS.md) — the one
  canonical source/sync/deployment runbook.

## 1. What this path does now

The glasses initiate their native EvenAI UI when they hear "Hey Even." The
XIAO plays the phone role: it accepts the native WAKE, sends ENTER and
heartbeats, records one VAD-ended utterance, and tells the CM5 about that exact
exchange. The CM5 fetches the closed WAV, runs batch STT, starts the LLM, and
uses the G2's native listening and response windows for the question and
answer.

This is not the `g2ai` fabricated-card path and not the HardwareOne hijack UI.
Production mutations require the active exchange ID:

```text
G2 -> XIAO       CTRL{WAKE_UP}
XIAO             allocate exchange ID; bind initiating arm + BLE generation
XIAO -> G2       CTRL{ENTER}; HEARTBEAT about every 3 s
XIAO recorder    micrecord startid <id> vad 1800 trim
XIAO -> CM5      evenai_wake <id>          (only after ID-owned CAPTURING)
CM5 -> XIAO      micrecord statusid/stopid <id>
CM5              voicefetch closed WAV -> batch STT
CM5 -> XIAO      g2evenai askid <id> <transcript>
CM5              LLM generation
CM5 -> XIAO      g2evenai replyid ...
                  or replypartid ... + replyendid ...
G2 -> XIAO       CTRL{EXIT} on wearer dismissal/session end
XIAO             terminalize <id>; stopid <id> discard when capture remains
XIAO -> CM5      evenai_cancel <id> <reason> (advisory, repeated)
```

The current pipeline does not send mutable Moonshine hypotheses to the lens.
The recognized question appears only after the WAV is closed, fetched, and
batch-transcribed. Do not describe this as live STT.

Before publishing the wake, the wrapper verifies that the CAPTURING recorder
is owned by the same exchange ID. A manual or delayed foreign start therefore
cannot impersonate the native capture: the exchange terminates as
`capture_owner_mismatch`, while exact-ID cleanup leaves the foreign recording
untouched.

## 2. Exchange ID and recorder ownership

Every native WAKE gets a 64-bit ID rendered as exactly 16 hexadecimal digits:

```text
<nonzero 32-bit random boot nonce><nonzero 32-bit per-boot counter>
```

The same value names all of these authorities:

- the native G2 session;
- the exact initiating temple and connection generation;
- the recorder owner and deterministic `rec_<id>.wav` filename;
- wake, autostop, cancel, status, stop/discard, fetch cleanup, ASK, reply parts,
  reply finalization, and conditional job completion.

Zero and malformed IDs fail closed. The host parser additionally requires both
32-bit halves to be nonzero. The recorder keeps four completed owner results in
RAM so delayed status/stop/delete can resolve an old exchange while a new one
captures. This is bounded correlation, not persistence: a reboot clears it,
and an ID older than four newer completions returns `NOT_FOUND`.

There is no "latest recording" fallback in the machine path. A command for
exchange A must never consume the result or path belonging to exchange B.

## 3. Production command and event grammar

### Firmware-to-CM5 EVT payloads

```text
evenai_wake <16hex-id>
evenai_cancel <16hex-id> <reason>
mic_autostop <16hex-id> <absolute-path>
```

Untagged native wake/cancel events are rejected. The legacy two-token
`mic_autostop <path>` event remains only for a manual `ask` recording; it is
not accepted as ownership evidence for native EvenAI.

### CM5-to-firmware native-window commands

```text
g2evenai askid <16hex-id> <text>
g2evenai replyid <16hex-id> <text>
g2evenai replypartid <16hex-id> <delta-text>
g2evenai replyendid <16hex-id> [trailing-text]
g2evenai exitid <16hex-id>
g2evenai status
g2evenai capabilities
```

`replypartid` text is a delta and preserves leading inter-chunk whitespace.
The first part sends ANALYSE before opening the response stream; `replyendid`
sends the final `fTextEnd=1` marker. A one-shot reply uses `replyid` when it
fits; longer answers are split into parts.

Legacy `g2evenai ask|reply|replypart|replyend|exit` deliberately return an
error and never apply their requested text. If one reaches firmware while an
exchange is active—most plausibly from a stale daemon during a mismatched
upgrade—the firmware also best-effort sends native EXIT and terminalizes that
active ID with reason `legacy_command`. This avoids heartbeating an unowned card
until the 60-second timeout. Explicit bench-card commands remain `g2ai`,
`g2ai-noask`, and `g2ai-direct`.

`g2evenai status` reports the active ID, arm, connection generation, bound UART
session epoch, UART runtime state, active and last-issued UART epochs, and the
last UART session lifecycle event. `uartlink status` reports the same UART
epoch/event diagnostics without requiring an active EvenAI exchange.
`g2evenai capabilities` must include `exchange-id-v1`; the maintained CM5 probe
refuses production-style trials without that capability.

### Recorder commands

```text
micrecord startid <16hex-id> [vad <200..10000>] [trim]
micrecord statusid <16hex-id>
micrecord stopid <16hex-id> [discard]
micdeleteid <16hex-id> "<basename.wav>"
```

`discard` is monotonic for the matching owner. The recorder still enters
FINALIZING, rewrites/closes the WAV, removes only its retained exact path, and
publishes IDLE last. It emits neither a saved event nor `mic_autostop` for a
discarded capture. `micdeleteid` checks that the quoted basename belongs to the
same keyed result before deleting it.

Manual `micrecord start|stop` and `micdelete` remain available for human tools;
they are intentionally not used as ownership fallbacks in this native path.

## 4. Where dismissal linearizes

The local XIAO terminal transition is the safety authority. On a matching
native EXIT, bound-arm disconnect, timeout, superseding WAKE, capture/consumer
failure, UART-link stop/authentication loss, or host `exitid`, firmware clears
the active ID/arm/stream state under its session lock before attempting a host
event. UART-link stop/authentication loss best-effort sends native EXIT and
drives the exact owned WAV through discard finalization. Its terminal reason
identifies the host-link state observed at that boundary:

| Reason | Firmware observation | Meaning |
| --- | --- | --- |
| `host_link_lost_runtime` | UART runtime is stopped. | The command/event transport is unavailable. This classification takes precedence over the epoch diagnostics. |
| `host_link_lost_never` | UART runtime is running, active epoch is zero, and last-issued epoch is zero. | No UART login has succeeded during this firmware boot. |
| `host_link_lost_cleared` | UART runtime is running, active epoch is zero, and last-issued epoch is nonzero. | A login succeeded earlier in this boot, but that authenticated session was cleared. `last_event` distinguishes such diagnostics as link stop, idle timeout, explicit/transport logout, or account revocation when known. |
| `host_link_lost_epoch` | The exchange is bound to a nonzero epoch and the current active epoch is a different nonzero value. | A logout/re-login or session replacement occurred. The new login cannot inherit authority over the old exchange. |

The last-issued epoch and `last_event` are diagnostics only; neither grants
authorization. The active epoch is the authority token, and a new exchange is
not bound to it until ENTER admission.

The CM5 handles all four reasons through the normal cancellation path. It
validates the event grammar, cancels and tombstones only the named exchange ID,
cooperatively stops that exchange's remaining work, and admits no later lens
mutation for it. The reason is retained for logs but does not select a special
retry or recovery branch. UART reconnect/login recovery belongs to the generic
link/session supervisor, and a later native wake must use its newly issued ID.

Every tagged native-window envelope rechecks the exact ID, arm, and connection
generation. The physical ATT loop repeats the check beside each
`writeValue()` call. Once terminal state commits, no later fragment is newly
admitted. One fragment whose check completed immediately before the commit is
already inside the blocking BLE stack and cannot be recalled; the design does
not claim otherwise.

A tagged ASK, one-shot REPLY, reply part, or reply finalizer that fails to send
is also terminal. Firmware best-effort sends native EXIT, clears the local
exchange with reason `send_failed`, and returns an error. It does not continue
heartbeats around a partially delivered listening/answer card.

WAKE/EXIT notifications have a separate four-entry admitted queue, drained
before session work. If it fills, a new EXIT evicts the oldest transition; a
new WAKE may be dropped. Missing a new invocation is safer than processing a
dismissal late and resurrecting old work.

The host `evenai_cancel` is a latency hint, not the firmware's safety fence.
EVT frames are CRC-protected but unacknowledged, so firmware retains up to four
cancel tombstones for eight seconds and tries to deliver three copies at
100 ms intervals. Duplicate host cancellation is idempotent. If every copy is
lost, the matching discarded recorder result still stops the host's native
fetch wait, and any later display command fails the firmware ID fence.

This is not a host-process liveness detector. If the daemon crashes while the
UART remains open and its authenticated state remains latched, firmware cannot
distinguish that from a slow healthy host; the 60-second session cap remains
the last-resort card bound. A future renewable host lease/heartbeat is still
required before live PCM can claim crash-fast teardown.

## 5. CM5 cancellation semantics by stage

The CM5 keeps active exchanges and a bounded terminal-tombstone cache keyed by
ID. Cancel-before-wake, duplicate wake, duplicate cancel, late completion, and
an old exchange finishing after a new WAKE are all idempotent and cannot clear
the newer exchange. Pending native work has priority over queued manual jobs,
but does not preempt a job already executing.

Cancellation is cooperative around the single UART command channel:

- Before a command write, the guard is checked under the command lock after
  stale EVT drain. A canceled mutation is never written.
- If cancellation arrives after a normal command was written, its one status
  reply is drained before cancellation is raised, so it cannot poison the next
  request.
- If it arrives during `voicefetch`, remaining frames and the terminal status
  are drained while audio frames are discarded.
- Native Moonshine batch STT cannot be interrupted safely on its sole worker.
  It is allowed to finish, but its transcript is discarded and no next stage
  starts.
- LLM streaming is closed when cancellation wins. No later part or
  `replyendid` is sent.
- ASK render holds are cancellation-aware.
- Conversation history, last-utterance persistence, and failed-STT WAV
  archives commit only after still-active successful delivery.
- Exact-owner WAV cleanup may overlap STT, but remains job-owned and is drained
  before the next exchange. Its command has a five-second ceiling; failure
  leaves a logged stray file instead of blocking the next wake for the generic
  command timeout.
- Any host job that exits without marking its matching answer fully delivered
  makes a five-second, non-replayed best-effort `g2evenai exitid <id>` attempt
  after recording its existing cancellation reason, or `host_incomplete` when
  no earlier reason exists. The exact firmware ID fence makes a wearer-
  dismissed or superseded rejection harmless and prevents this cleanup from
  closing a newer exchange.

Native display mutations use `replay=False`. In particular, replaying a reply
delta after an executed command lost its status would duplicate visible text.

## 6. Timeouts that still matter

These values have different jobs and must not be collapsed into one setting:

| Value | Owner | Purpose after this revision |
| --- | --- | --- |
| 1,800 ms | XIAO native capture | Trailing-silence endpoint for a Hey-Even recording. Hard-coded in the current firmware path. |
| `audio.vad_silence_ms` (default 1,200 ms) | CM5 manual `ask` | Silence window requested by host-started manual recording; it does not override the native 1,800 ms value. |
| `audio.vad_max_seconds` (default 15 s) | CM5 | Host safety deadline while waiting for auto-stop. It still bounds manual ask and is a lost-event/never-ended backstop for an active native capture. A matching dismissal/discard exits early instead of waiting 15 s. |
| 5 s | XIAO session wrapper | Native recorder-start deadline. |
| 60 s | XIAO recorder/session | Absolute recording cap and native idle safety EXIT; these are separate mechanisms even though both currently use 60 s. The session cap also bounds an undetected daemon crash on a still-open/authenticated UART. |
| 5 s | CM5 owner cleanup | `micdeleteid` ceiling before leaving a logged stray. |
| 65 s | CM5 session | Generic command deadline chosen to outlast the firmware's worst command path. Not reused for owner cleanup. |

Live STT may eventually make the 15-second wait uncommon, but it does not make
the fallback watchdog irrelevant. The live design still needs an ID-bearing
BEGIN/PCM/END/ABORT frame protocol and a safe fallback to the finalized WAV.

## 7. What remains for live STT

The exchange ID, recorder ownership, tagged final ASK/reply path, and dismissal
fence are now prerequisites that live transport can reuse. They do not mean
live transport exists. Still required:

- a bounded nonblocking PCM fork and dedicated UART TX worker;
- ID-bearing BEGIN/PCM/END/ABORT frames with offsets, format, end reason, and
  rolling integrity evidence;
- a renewable host-ready lease established before capture starts;
- one dedicated Moonshine stream worker and fallback to batch decode;
- shadow-mode continuity, load, WER, and END-to-final measurements;
- optional revisioned cumulative ASK only after the G2 renderer proves it can
  update without restart/flicker/queued repaint.

The finalized transcript remains the only LLM prompt. A mutable partial must
never enter conversation history.

## 8. Validation still owed on hardware

After coordinated deployment, exercise one fresh wake for each boundary:

1. dismiss while CAPTURING;
2. dismiss during WAV stop/fetch;
3. dismiss while batch STT is running;
4. dismiss during the ASK render hold;
5. dismiss after the first streamed reply part;
6. supersede an old exchange with a new WAKE;
7. disconnect the bound temple while active;
8. stop the UART runtime or revoke its authentication while active.

For every case, the native card must stay dismissed, no later part/end may
appear, a canceled WAV must not be retained as conversation or failed-STT
evidence, and the next wake must work. The XIAO log should show one terminal
ID/reason, repeated cancel delivery attempts, and stop+discard when a recorder
still belonged to that ID. The CM5 log should show the same ID canceled and no
subsequent native lens mutation for it.

Also force one tagged send failure and one stale-daemon legacy command. The
first must terminalize as `send_failed`; the second must not apply its text and
must terminalize as `legacy_command`. A host-side failure before complete
delivery should attempt exact-ID `exitid` and must not affect the next wake.
The explicit link/auth teardown must log the matching
`host_link_lost_runtime`, `host_link_lost_cleared`, or
`host_link_lost_epoch` reason and discard the matching owned WAV. A wake while
the runtime is up but no login has ever succeeded this boot must report
`host_link_lost_never`. Do not misreport a daemon crash on a still-open,
authenticated UART as passing that test; verify separately that the known
fallback remains the 60-second session cap.

## 9. Source map

- Firmware native session, guard, priority controls, and capture wrapper:
  `components/hardwareone/G2_Glasses.cpp`.
- Recorder FSM and owner-scoped commands:
  `components/hardwareone/System_Microphone.{h,cpp}`.
- UART event/frame transport: `components/hardwareone/System_UartLink.{h,cpp}`.
- Strict host grammar: `cm5/ai-service/hw1_ai_service/evenai_protocol.py`.
- ID registry/tombstones/priority: `cm5/ai-service/hw1_ai_service/jobs.py`.
- command/frame-drain guards: `cm5/ai-service/hw1_ai_service/link/session.py`.
- owner-scoped wait/fetch/cleanup:
  `cm5/ai-service/hw1_ai_service/audio/fetch.py`.
- per-stage cancellation and transactional delivery:
  `cm5/ai-service/hw1_ai_service/pipeline.py` and `llm/client.py`.
- maintained hardware probe: `cm5/ai-service/tools/g2_evenai_probe.py`.
