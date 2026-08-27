# CM5 presence + keep-alive as a framed transport (not a command)

> **STATUS: SUPERSEDED — DO NOT IMPLEMENT. Written and refuted 2026-08-14.**
>
> The premise is false. Both messages are ALREADY off the command plane via
> control-plane intrinsics this plan failed to find:
> `liveAudioHandleReadyIntrinsic` (`System_UartLink.cpp:681`) and
> `cm5PresenceHandleHeartbeatIntrinsic` (`:816`). Both `return` before
> `appendCommandToFeed` (`:838`) and `submitAndExecuteSync` (`:878`). §2's goal
> is already met in the working tree.
>
> §1's proposed "one-line fix" (adding `ready` to
> `liveAudioIsHousekeepingCommand`) is a REGRESSION, not a fix.
> `System_UartLink.cpp:829-833` states the design directly: *"A healthy ready
> renewal already returned through the intrinsic above; an initial/repair ready
> intentionally reaches this ordinary, busy-accounted path."* Exempting `ready`
> would also exempt lease acquisition and repair — which spin up the TX task and
> allocate the 16 KB PSRAM shadow — and delete them from the audit trail.
>
> **The open bug this leaves.** The 2026-08-14 XIAO log shows
> `[CMD] cm5@uart: liveaudio ready ... -> OK` for EVERY renewal at the 2 s
> cadence. `[CMD]` is emitted only by `logCommandExecution`
> (`System_Utils.cpp:896`), unreachable from the intrinsic's early return.
> So the intrinsic was not intercepting. Either the flashed image predates it,
> or every renewal is failing the healthy-renewal preconditions
> (`parseCanonicalReadyLine`, `namedSessionMayControl`, `namedSessionEpoch != 0`)
> and taking the acquisition path 30x/minute. **That is the bug worth chasing.**
>
> Retained below only for the three design constraints that would bind if a
> host->device frame direction is ever wanted for other reasons — see the
> ADVERSARIAL FINDINGS section appended at the end.

## 1. Why — and what this does NOT fix

The trigger was wake rejections (`host_service_busy`) on the G2 native path.
**That cause has been found and is not what this plan addresses.**

`System_UartLink.cpp:837` already exempts host housekeeping from the busy bit:

```cpp
const bool cm5ProtocolCommand = cm5PresenceIsProtocolCommand(cmd.c_str());
const bool liveAudioHousekeeping = liveAudioIsHousekeepingCommand(cmd.c_str());
const bool hostHousekeeping = cm5ProtocolCommand || liveAudioHousekeeping;
if (!hostHousekeeping) cm5PresenceCommandStarted(commandSessionEpoch);
```

`cm5 heartbeat` is already covered. `liveAudioIsHousekeepingCommand` covers only
`status` and `capabilities` — **not `ready`**, the 2-second lease renewal. That
omission alone produced the rejected wakes. A one-line fix (add `ready`, and
audit `shadow` too) resolves the bug with no protocol change.

What this plan DOES address, which the one-liner does not:

- Every command — housekeeping or not — runs `appendCommandToFeed()`
  (`System_UartLink.cpp:838`) and `submitAndExecuteSync()` on `cmd_exec_task`.
  Measured 2026-08-14: `cmd_exec_task` at **70-75% CPU** on an idle device,
  `[LOOPHEALTH] stall: lap took 700-830 ms | loop-bound: INPUT` continuously.
- The RX drain handles **at most one line per `loop()` lap**
  (`System_UartLink.cpp:983` `break;`). Keep-alive traffic competes with every
  other line for that single slot, and lap time is what stalls.
- Housekeeping pollutes the command audit feed, which is the thing doing a
  LittleFS scan per command (see `project_cmd_audit_input_stall`).

So the goal is **cost and separation of planes**, not the wake bug.

## 2. Goal

Move `cm5 heartbeat` (5 s presence) and `liveaudio ready` (2 s lease renewal)
off the command plane onto the existing COBS frame plane, so they never touch
`cmd_exec_task`, the command feed, or the audit — while remaining visible in
device logs the way G2 heartbeats are.

## 3. The enabling fact, and the missing half

Frame body on the wire today (`uartLinkWriteFrameWithWait`,
`System_UartLink.cpp:1077`):

```
type(1) | seq_lo | seq_hi | len_lo | len_hi | payload[len] | crc16_lo | crc16_hi
```

COBS-encoded, `0x00` delimiter. COBS never emits `0x00` inside a frame and a
command line never contains one, so **frames and text can share the wire
unambiguously in both directions**. That is what makes this possible at all.

**The missing half:** host->device is text-only today. `transport.py` exposes
only `write_line()`; the firmware RX drain (`System_UartLink.cpp:945-999`) is a
byte loop that only ever accumulates into `sUartCLI` until `\n`. There is no
COBS decoder in firmware and no frame encoder on the host. This plan adds a
direction to the protocol, not a message to an existing one.

## 4. Wire additions

Two new host->device frame types (device->host `FRAME_EVT 0x03` is unchanged):

| type | name | payload |
|---|---|---|
| `0x20` | `FRAME_HOST_PRESENCE` | `ver(1) seq(4) mode(1)` |
| `0x21` | `FRAME_HOST_LEASE` | `ver(1) seq(4) controller(8)` |

Device->host ack reuses `FRAME_EVT` with a reserved ASCII prefix, or adds
`0x22 FRAME_HOST_ACK` carrying `ver(1) seq(4) status(1) session_epoch(4)`.
**Decision required** — see §9.

`seq` widens from the 16-bit frame header `seq` to a 4-byte payload field
because the host presence actor's sequence is a monotonic session counter, not
a frame counter, and reboot detection depends on it.

## 5. Firmware changes

1. **`System_UartLink.cpp` RX drain** — add a COBS frame accumulator beside the
   line accumulator. On `0x00`: if the pending buffer is non-empty, COBS-decode,
   verify CRC16, dispatch by type; else it is a delimiter run, ignore. Bound the
   buffer at `UARTLINK_FRAME_MAX_PAYLOAD + 7` and drop overlong frames the way
   `sDiscardingLine` drops overlong lines.
2. **Do not consume the one-line-per-lap budget.** Frames must be drained in the
   same `while (available())` loop but must NOT `break` — otherwise keep-alive
   frames inherit exactly the lap-rate limit this plan exists to escape. Cap
   frames-per-lap separately (e.g. 8) to bound worst-case lap time.
3. **`System_Cm5Presence.cpp`** — factor the existing `cm5 heartbeat` handler
   into a transport-neutral `cm5PresenceApply(epoch, seq, mode)` that both the
   command handler and the frame handler call. The command form stays for
   backward compatibility (§8).
4. **`System_LiveAudio.cpp`** — same split for `liveaudio ready`.
5. **Idle clock.** `sLastInteractionMs` is stamped only on a completed *line*
   (`System_UartLink.cpp:981`). Presence frames MUST stamp it too, or the UART
   session idles out and the host gets "Signed out due to inactivity" — a
   regression that would appear only after the idle timeout, i.e. not in a short
   bench test. This is the single most dangerous item in this plan.
6. **Event visibility.** Emit a debug line per accepted frame under a link debug
   flag, mirroring `[G2-L] Heartbeat #1196 seq=0x0E (20 bytes)`. Proposed:
   `[UART-HB] presence seq=%lu mode=%s epoch=%lu` and
   `[UART-HB] lease seq=%lu controller=%016llx`. Rate-limit or gate behind the
   flag: at 5 s + 2 s cadence this is ~17k lines/day if left on.

## 6. Host changes

1. **`link/protocol.py`** — frame *encoder* (COBS + CRC16, mirroring the
   decoder already there) and the new type constants.
2. **`link/transport.py`** — `write_frame(type, seq, payload)` alongside
   `write_line()`, sharing the same write lock so a frame cannot interleave
   mid-line.
3. **`cm5_presence.py`** — replace `session.command(...)` with
   `transport.write_frame(...)` plus an ack await. The actor keeps its
   single-owner discipline, sequence validation, and `_reboot_fences` logic;
   only the carrier changes.
4. **`stt/live_gate.py`** — same for `ensure_armed` / `_renew_loop`. Note the
   arm path also sends `liveaudio status` and `liveaudio shadow`, which stay
   commands: only the 2 s renewal moves.
5. **`link/session.py`** — frame acks must not be mistaken for command replies.
   The reply collector keys on lines; acks arrive as frames and route to the
   existing `FrameSink`, so this is mostly a routing addition.

## 7. Downstream impacts

- **Idle logout** (§5.5) — highest risk, delayed failure mode.
- **Auth.** `cm5 heartbeat` today refuses unless there is a named authenticated
  UART session (`System_Cm5Presence.cpp:486`). A frame carries no session. The
  handler must stamp `uartLinkSessionEpoch()` and reject when it is 0 — meaning
  anyone able to write bytes on that UART can assert host readiness without
  logging in. Today that is already true for *reading* live PCM frames, so this
  is consistent, but it is a real widening. **Decision required.**
- **`fake_firmware.py`** must learn to decode host frames, or every presence and
  live-gate test breaks. Affected: `test_cm5_presence.py`, `test_live_gate.py`,
  `test_renew_loop.py`, `test_session.py`, `test_daemon_startup.py`,
  `test_pipeline.py`, `test_g2_evenai_probe.py`, `test_p2_frames.py`.
- **Rolling upgrade.** A new daemon against old firmware sends frames the
  firmware silently drops -> presence goes stale -> every native wake is refused
  with `ServiceStale`. The reverse (old daemon, new firmware) is fine because the
  command form is retained. **The daemon must negotiate before switching.**
- **`liveaudio status` parse** — `_parse_epoch` reads `session_epoch` from the
  ready *reply*. A frame ack must carry it or arming loses its epoch check.
- **Command feed / audit.** Removing these from `appendCommandToFeed` is the
  point, but it also removes them from the audit trail. Confirm no operator
  workflow greps the feed for heartbeats.
- **`cm5PresenceCommandStarted` grace.** The post-command grace window
  (`System_UartLink.cpp:904`) exists partly so the heartbeat actor can acquire
  the Session lock after a reply lands. If the heartbeat no longer takes that
  lock, revisit whether the grace is still calibrated correctly.

## 8. Compatibility and rollout

1. Land the **one-line `liveaudio ready` housekeeping fix first**, independently.
   It resolves the actual bug and is trivially revertible.
2. Add firmware frame RX + handlers, keeping both command forms working.
3. Add a capability probe: the daemon asks the firmware whether host frames are
   supported (extend `liveaudio capabilities` or add `uartlink capabilities`)
   and only then switches carrier. Fall back to commands otherwise.
4. Ship daemon with frames **off by default**; enable by config for one bake
   period on the bench rig.
5. Only after a clean multi-hour soak (idle logout is the thing to watch) make
   frames the default.

## 9. Open decisions

1. **Ack transport** — reuse `FRAME_EVT` with a prefix, or add `0x22`?
2. **Auth model** — is stamping the active epoch acceptable, or should the frame
   carry a token bound to the login?
3. **Debug flag** — which family owns `[UART-HB]`, and default off?
4. **Does `liveaudio ready` even need to move** once it is classified as
   housekeeping? Its remaining cost is the audit stall, which fixing
   `project_cmd_audit_input_stall` would address for every command at once.
   That fix may dominate this entire plan on cost/benefit.

---

## ADVERSARIAL FINDINGS (2026-08-14)

Constraints that would bind if host->device framing is ever built for other
reasons. All three must be designed in from the start, not retrofitted.

**A. COBS desync is a real, already-encountered failure mode.** §3's argument
("a command line never contains 0x00") is insufficient in the host->device
direction. The HOST already hit and fixed this exact bug — `transport.py:198-209`:
*"a ROM boot burst has arbitrary 0x00s and can leave odd delimiter parity.
WITHOUT this, the reader stays in_frame forever, swallowing every subsequent
NUL-free text reply."* A CM5 reset emits exactly that into the firmware's RX.
With no idle-based resync, one stray 0x00 at wrong parity wedges the drain in
frame mode permanently: every subsequent login and command line is swallowed and
the device is unreachable over UART until a link restart. Port the host's
idle-timeout abort verbatim. Two more reliable paths into the same state:
`gWizardOwnsSerial` returns early from the whole drain (`:943`), and
`UL_PENDING_RESTART` applies a baud change without draining (`:928`).

**B. The host cannot receive frame acks as designed.** There is one rx queue and
one `frame_sink`, immutable for the transport's lifetime by design
(`transport.py:89-92`) and already owned by `LivePcmInbox` (`__main__.py:137`).
Non-EVT frames are dropped by both drainers: `_collect` -> `_route_frame` returns
False (`session.py:500-501`), and `_drain_stale` -> `_note_stray` logs and drops
(`:486-488`) — the latter running before every command write (`:371`) and every
login (`:410`). An ack landing while any coroutine holds `Session._lock` is
destroyed. Needs a FrameSink multiplexer with typed registration: a transport
refactor, not "mostly a routing addition".

**C. Silence is not an acceptable ack contract.** Today reboot detection comes
from a LINE: `_command_once` sees `is_auth_required` (`session.py:377`) ->
`_mark_reboot_suspected()` -> re-login + replay. A rebooted firmware drops an
unauthenticated frame silently, so the daemon sees only a timeout,
`_reboot_fences` (`cm5_presence.py:224-226`) never fires, and the STARTING fence
that stops a READY heartbeat replaying into a new epoch is bypassed. A NACK frame
is mandatory, and ack payloads must carry everything consumers actually validate:
`_validate_reply` checks seq AND mode AND session lease
(`cm5_presence.py:228-243`); the ready reply carries `lease_ttl_ms`, `renew_ms`,
`baud` (`System_LiveAudio.cpp:806-812`).

**Lesser but real.** Idle-expiry is *evaluated* on a completed line
(`uartProcessLine:651`), so a missed stamp surfaces on the next real command as
"Signed out due to inactivity" — meaning any validation soak must be QUIESCENT
longer than `sessionIdleUart` or another command masks it. The nag limiter
`sLastNagMs` (`:492`) is one global 2 s window shared with the auth nag, so a
frame flood suppresses a real operator's auth nag. The transport-epoch splice
guard (`:949-956`) has no frame equivalent, and since a frame carries no session,
a partial lease frame from before a logout can complete after re-login and apply
to the new session — the lease resurrection `System_LiveAudio.cpp:781-800` exists
to prevent. Any accumulator must be static DRAM, not stack: the TX side already
learned this (`:1040-1042`, "~2.2KB, too heavy"), and the loop task shares 8192
bytes with `char controlReply[256]` plus CommandArgs/String/Command frames.

**Corrections to the plan's own citations.** §7 cites
`System_Cm5Presence.cpp:486` for the authenticated-session refusal; that is the
registry stub's message, the real check is `:438-442`. §2's cost model is also
wrong: the measured 700-830 ms INPUT-bound lap is far more plausibly
`submitAndExecuteSync` blocking the loop task at `:878` plus the 5000 ms TX wait
at `:889`, neither of which this protocol change touches.

**Verified sound.** The frame body layout (`:1076-1087`); type values 0x20-0x22
not colliding (`System_UartLink.h:97-104`); host->device being text-only today
(`transport.py:141-150`); `sLastInteractionMs` as a delayed-failure risk;
the auth-widening asymmetry (outbound frames ARE session-gated at `:1060-1063`);
`fake_firmware.py` blast radius; and the new-daemon/old-firmware staleness
direction.
