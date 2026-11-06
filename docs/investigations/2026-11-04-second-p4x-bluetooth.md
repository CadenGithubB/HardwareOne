# Second P4X-EYE Bluetooth and crash investigation

Investigated November 4, 2026, America/New_York. Target: the second P4X-EYE
(its identity is recorded in the private archive), firmware `0.99.99`, ELF
identifier `c1c6260a4`. The other P4X console (the STT board) was not opened.

## Findings

The board did crash. Its retained history contains two instances of the same
**Stack protection fault**, on core 0 at PC `0x40367536`, with signature
`0x6e7d99a7`, in build `c1c6260a4`. Separately, it has substantial ring
reconnection/setup failures and glasses supervision timeouts. The evidence does
not establish that one Bluetooth defect caused both crashes.

The reported C6 capacity is **four connections**, not three. The current boot
has a matching Hosted 2.12.13 companion, no missed companion heartbeats, no
transport errors/timeouts, and no companion recoveries. Wi-Fi scan/connect/
disconnect counters are zero in the recorded current-boot attempts. Neither an
outdated three-link companion nor active Wi-Fi contention explains these
captured failures.

The user reports no Even app competing for connections. Devices were mostly
nearby, but the user often went downstairs away from the P4X. That is consistent
with actual range loss. Recovery after returning remains a separate concern.

## Crash evidence

These are **device-recorded clock values**, not verified local wall times:

| Boot that died → boot recording it | Recorded timestamp | Fault |
| --- | --- | --- |
| 16 → 17 | No valid wall time | Brownout; no panic record |
| 21 → 22 | 2026-10-29 16:08:39 | Load access fault, core 0, PC `0x4ff374d6`, build `b1e9fe08a` |
| 53 → 54 | 2026-11-03 17:48:08 | Stack protection fault, core 0, PC `0x40367536`, build `c1c6260a4` |
| 56 → 57 | 2026-11-04 16:19:09 | Same stack protection fault, PC, build and signature |

Latest saved backtrace:

```text
0x4ff951f0 0x4ff95678 0x4ff9520c 0x4ff95838
0x4ff901b4 0x4ff95a28 0x403a5bd2 0x403e3a9c
```

The November 3 trace has different intermediate addresses but the same fault PC,
signature and final two frames. Both occurred in phase `running`. Neither
record names the task. Before both crashes, the event stream contains a glasses
session and a ring long-press refused with “Checking speech model. Please wait.”
The gaps are minutes, and other work could have occurred. This is a possible
reproduction context, **not proof that speech initialization caused the fault**.

Follow-up source review during repairs identified an important limit: on RISC-V,
Arduino starts this array with SP and then scans stack words; it does not unwind
a call chain. Also, ESP-IDF's hardware guard has a separate offending PC that
the old recorder omitted. The repeated old PC/signature therefore does not
prove both crashes had the same offending instruction. See
[fix progress](2026-11-04-p4x-fix-progress.md) for the corrected capture work.

The inspected local ELFs and compressed candidates did not match `c1c6260a4`.
Firmware/debug files were searched in the project, other local Codex worktrees,
durable workspace and inspected external hardware archives. No incorrect ELF was
used to assign source lines. Exact symbolication remains unresolved.

System file logging is inactive and autostart is off; IDF log routing (`loglink`)
is off, so IDF messages go directly to serial. The two remaining manual captures,
`vfscheck.log` and `vfscheck2.log`, contain no panic/overflow evidence. There was
no full panic transcript among the retrieved logs. The local SDK configuration
disables full core dumps, but the running build's exact configuration was not
recovered, so that setting is not asserted as a verified fact about this image.

The board clock lagged the host by approximately 22 minutes during collection.
Older logs also show time adoption from the ring. Relative uptime, boot numbers
and event order are more reliable than interpreting these timestamps as local
clock time.

## Ring failures

The internal error log contains 643 ring errors across its retained history:

| Error | Count |
| --- | ---: |
| Setup `identity-unknown` | 355 |
| BLE connection failure | 263 |
| Ring service not found | 18 |
| Setup disconnected | 5 |
| Setup timeout | 2 |

The SD error log adds 117 connection failures and 9 service-not-found errors
from an earlier period. These are retained-history counts, not all from today
or from one boot.

### Missing glasses identities before the latest crash

All 355 `identity-unknown` errors occurred in boots 54–56; 349 were in boot 54.
The associated event stream shows prolonged operation with only one glasses arm
connected. Ring setup therefore repeatedly reached a software prerequisite
failure after a Bluetooth link could be established.

The current source explains this error at `ringServiceSetup()` in
`G2_Ring.cpp`: final `advStart` setup needs both glasses addresses. It takes
live addresses and falls back to a complete saved pair only when it matches
every live arm. Missing, invalid or mismatched saved identities cause setup to
fail closed with `G2_RING_ERR_IDENTITY_UNKNOWN`, after which the connection
path disconnects the ring. This is not an RF timeout or an exhausted link slot.

The repository already contains the saved-address fallback and tests for it;
this investigation did not add a duplicate fix. Historical pairing contents
were not preserved in the logs, and the running ELF was unavailable, so the
precise reason its identities could not be resolved then is unknown. The current
peer listing has both saved glasses addresses. There are **no identity-unknown
errors in the retrieved current-boot error-log snapshot**.

### Current boot: link instability and discovery failures

At the first diagnostic snapshot, boot 57 had 83 low-level ring attempts:
51 link opens succeeded and 32 failed. At the later snapshot it had 84 attempts,
52 successful opens and the same 32 failures. “Connected” in the low-level
counter does **not** mean full ring setup completed.

The current-boot error-log snapshot has 32 connection failures, 8 service-not-found
errors and 1 setup timeout. The event stream records 41 fully announced ring
connections and 40 disconnects. Of the 40 completed connected periods, 11 lasted
under 30 seconds; the median was about 75 seconds. Others lasted over an hour.
These figures apply to the preserved snapshot and exclude subsequent activity.

Live evidence includes:

- Opens failing after 27–30 seconds at `open-event`, with generic GATT error
  `0x85`, followed by successful client unregister acknowledgement.
- HCI reason `0x3e`, named by the SDK as connection-establishment failure, and
  `0x100`, connection cancelled. Generic GATT `0x85` alone is not a root cause.
- An open reported successful after 11.1 seconds, then service discovery
  unavailable and the ring service not found.
- Later automatic recovery without a reset; the final ring snapshot was up.

This is a mixture of failed radio establishment and failure after establishment.
Range, intermittent reachability and limited airtime while reconnecting are
plausible contributors. The available durable ring-disconnect events do not
retain enough HCI detail to classify all 40 disconnects as range loss, peer
closure or local teardown.

## Glasses and phone

Both glasses arms were responding to heartbeat ACKs when first attached. During
the investigation, the right arm dropped and the left remained connected. Saved
events record supervision timeouts (`0x08`) for both left and right arms during
the current boot, with repair scans afterward. The left later recovered, as
confirmed by heartbeat ACKs and the final device-info snapshot.

Some successful repair/reconnect scans had RSSI around `-83` to `-84 dBm`.
Combined with the user's downstairs trips, this supports intermittent weak
coverage as a contributor. It does not explain the identity prerequisite
failure or establish the cause of the stack faults.

Phone diagnostics show four ready sessions, three ended sessions, five
connection attempts (four successful, one failed), zero authentication failures,
zero version mismatches, zero peer resets and zero service-change events. End
reasons are two `connect` and one `deadline`; these application reasons are not
equivalent to a complete HCI disconnect diagnosis. The phone was ready in both
snapshots. Its last successful setup took 5.588 seconds, with a 10 ms setup-worker
wait, and its link used a 120 ms idle interval.

Ring/glasses/phone setup shares one worker. A roughly 30-second ring attempt can
delay another queued setup. However, the retained attempts show ring waits of
0–1 ms and the latest phone wait of 10 ms. These measurements do not establish
worker starvation as the primary failure. A simultaneous return-to-range
reproduction is needed to measure that case.

## Memory, stack and storage

The snapshot had about 223 KB free internal memory, a largest internal block of
147,456 bytes, about 32.4 MB free PSRAM, and zero failures in the instrumented
allocation counters. Those counters do not cover every allocation or the prior
crashed boot. They do not support general heap exhaustion in the observed boot.

Task low-water marks, in **bytes**, were:

| Task | Minimum free stack observed |
| --- | ---: |
| `g2_owner` | 312 |
| `log_out` | 524 |
| `cmd_exec` | 700 |
| `ble_setup` | 1,124 |
| `r1_owner` | 2,456 |
| `phone_pipe` | 3,572 |

These are high-water usage measurements from the surviving boot, not the task
identified at the crash. Current source pins `cmd_exec` and `log_out` to core 0,
and `g2_owner` to core 1. Therefore the especially low `g2_owner` figure must not
be presented as identifying the saved core-0 fault. Stack depth/corruption in a
core-0 path deserves targeted examination once matching symbols are recovered.

Older records show LittleFS dropping below its 100 KB reserve and routing logs
to SD, plus settings-save failures. This explains why both storage tiers needed
inspection. Current internal storage had roughly 7.6 MB free, and SD roughly
119 GB free; the old low-space condition is not current. Authentication and I2C
error files added no specific explanation for today's Bluetooth failures.
October 30 records include C6 unexpected resets with completed soft recoveries;
these must not be confused with today's clean C6 uptime.

## Recommended next investigation

1. Recover the exact `c1c6260a4` ELF/map and decode both stack-fault traces. Check
   the core-0 call path and stack use before choosing a stack increase or code
   correction. Without symbols or a new full panic transcript, naming the
   crashing function would be speculation.
2. Reproduce leaving and returning to range with all four peers enabled. Capture
   continuous serial output, per-peer HCI disconnect reasons, setup-worker waits,
   saved/live glasses identities and full ring setup outcomes. This separates
   expected range losses from failed recovery.
3. If `identity-unknown` recurs, inspect the live/saved address match at that
   moment. The source already has a guarded fallback; do not replace it with
   unconditional identities or bypass the pairing check.

No firmware, settings, pairing or radio state was changed to test a theory.
No reset, flash, restart, log clearing or deliberate connection disruption was
performed. The authenticated serial port was released after collection.

## Evidence and collection limits

Private evidence is retained in the ignored project directory:

```text
work/second-p4x-investigation-20261104/
  capture-*.log
  authenticated-20261105T015740Z/
  logs-20261105T015835Z/
  authenticated-20261105T020407Z/
  extra-logs-20261105T020521Z/
  elf-search.json
```

Seventeen saved log files, **1,482,684 bytes**, were copied and verified against
their transfer manifests using byte counts and SHA-256. Each file preserves the
prefix of the size recorded at its directory listing; live appends after that
boundary are not claimed as part of the snapshot. Serial reads checked returned
offsets and decoded chunk lengths. Internal command audit and system-events
grew during collection without an observed size drop; diagnostics themselves
append audit records. The manifest records observed sizes and local hashes;
these are not device-side whole-file hash attestations or atomic filesystem
snapshots.

Two nonexistent-directory probes generated new storage errors after the main
log snapshot; those are investigation artifacts, not pre-existing faults.
Credentials were supplied interactively and the login command was redacted in
the serial evidence. Raw logs may contain personal data and remain private.
No large build or dependency tree was created for this investigation.
