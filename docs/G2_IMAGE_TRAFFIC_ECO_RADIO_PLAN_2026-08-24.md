# G2 image-traffic classification and delayed-ECO radio plan

Date: 2026-08-24

Status: reviewed revision; adversarial corrections incorporated

Implementation status: design only; no firmware source has been changed

## Executive decision

The battery-life idea is sound, but the safe implementation is not simply
"send `72-84` five seconds after `g2ConnPriReleaseFast()`." The current
connection-parameter sender can issue conflicting requests from several tasks,
tracks only part of a requested profile, and dereferences a temple client that
may be torn down before a delayed request runs. Adding ECO directly to that
path would make existing races more likely.

The recommended design has two linked parts:

1. Classify each **image producer**, rather than each menu page, as `None`,
   `OneShot`, `OnDemand`, or `Continuous`. Each producer owns an explicit,
   scoped throughput lease whose lifetime matches actual pixel production.
2. Move all temple connection-parameter requests to one serialized radio-policy
   execution context. Prefer the existing `g2_ctrl_owner` only after its
   wake-to-submit latency is bounded; it can currently spend about 3.5 seconds
   in recovery work. If that work cannot be moved or bounded, use a small
   dedicated policy task. The sole issuer serializes complete FAST, BALANCED,
   and experimental ECO profiles and sends a one-shot ECO request after a
   quiet tail only when no throughput or R1-admission lease remains.

The distinction between a page and a producer is load-bearing. Files is one
registered page but launches several one-shot viewers; Sensors offers both a
one-shot Capture and a continuous Stream; Health changes dynamically between a
text-only overview and on-demand graphs; and the production keyboard is neither
a `G2PageModule` nor a `G2SessionJob`. A mode stored only in `G2PageModule`
would be incomplete and, for mixed pages, wrong.

The experiment must initially default off on every boot. Before it can be
enabled, a fresh trace must establish the complete peer-requested idle tuple
for each G2 temple on the target glasses firmware and topology, including
supervision timeout. Current source records G2 idle as `72-84` ticks with slave
latency 4, but not its timeout. An unpublished local bench capture from
2026-08-22 (`.scratch/btsnoop/g2-r1-229-coldconnect-gestures-20260822-060655/`)
contains `72-84 / 4 / 600` for the R1 and a different low-power G2 request, so
the R1 tuple must not be copied to the glasses by assumption. Phase 0 must add
the exact glasses firmware and address-to-device classification to a sanitized
capture note before treating those tuples as reproducible evidence.

## Scope and evidence quality

This report works backward from the radio operation to every current image
producer, then proposes the smallest ownership model that represents the real
traffic.

`python3 docs2/docsctl.py status` reports 142 of 428 documents fresh, with 275
stale. The G2 source and the G2 subsystem/file documents are among the changed
or stale inputs. The private knowledge base was therefore used only for
discovery; every material claim below was checked against the current working
tree. The working tree already contains extensive user changes, all of which
were left untouched.

Primary source anchors:

- Connection profiles and controller-admission observations:
  `components/hardwareone/G2_Glasses.cpp:3001`.
- Mic FAST ownership: `components/hardwareone/G2_Glasses.cpp:4197`.
- Existing control owner: `components/hardwareone/G2_Glasses.cpp:14051`.
- GAP result decoder and cache invalidation:
  `components/hardwareone/G2_Glasses.cpp:16734`.
- Current fixed-latency sender: `components/hardwareone/G2_Glasses.cpp:16867`.
- Current priority arbiter: `components/hardwareone/G2_Glasses.cpp:16944`.
- Session worker: `components/hardwareone/G2_Glasses.cpp:22318`.
- Rendered-image transport chokepoints:
  `components/hardwareone/G2_Glasses.cpp:27759` and
  `components/hardwareone/G2_Glasses.cpp:28288`.
- R1 BALANCED guard: `components/hardwareone/G2_Ring.cpp:5125`.

This is a design report, not proof of a battery improvement. Connection-event
frequency is a useful mechanism measurement; only a controlled discharge test
can establish the end-to-end battery gain.

## Working backward from the radio

### 1. The link state we want after activity

The glasses have been observed requesting an active state with latency 0 and
an idle state with a substantially longer interval and slave latency 4. Current
source records these observations:

| State | Observed/requested interval | Latency | Purpose |
|---|---:|---:|---|
| FAST | Current host request `12-12` ticks | 0 | Image and mic throughput |
| Native active | Peer request `12-24`, landed 24 | 0 | Glasses' own active state |
| Native idle candidate | Peer request `72-84`, landed 84 | 4 | Glasses' own low-power state recorded in source |
| BALANCED | Current host request `32-48` | 0 | Free controller admission capacity during R1 connect |

At 84 ticks the connection interval is 105 ms. Latency 4 permits a peripheral
with nothing to send to skip up to four connection events. Compared with a
15 ms, latency-0 link, this is roughly 7 times the event spacing and up to 5
times the skip allowance. The existing source describes this as an
order-of-magnitude, roughly 35-fold reduction in **mandatory wake
opportunities**. It is not a claim of 35-fold battery life.

The complete ECO tuple remains unknown. In particular, the target G2 idle
supervision timeout has not been preserved in source. A sanitized reading of a
recent multi-device capture found:

- one G2 low-power request at `36-72 / latency 4 / timeout 600`, landing 72;
- the R1 at `72-84 / latency 4 / timeout 600`, landing 84; and
- later G2 active requests at `12-24 / latency 0 / timeout 600`.

That may reflect firmware version, connection phase, or more than one G2 power
state. The first prerequisite is therefore to observe both temple BDAs and log
the full tuple when the exact target glasses enter idle naturally.

### 2. What the sender can express today

`setTempleConnParams()` accepts only `min_int` and `max_int`. It always sends
latency 0 and timeout 500 (5 seconds). Its request cache stores only min/max,
while the GAP callback separately stores the applied interval, latency, and
timeout.

That representation cannot safely add ECO:

- it cannot request latency 4;
- it cannot distinguish two requests with the same interval range but
  different latency or timeout;
- a newer request can overwrite the min/max used to classify an older GAP
  completion;
- `ESP_OK` means queued, not accepted; the final result arrives later; and
- Bluedroid can complete its internal no-op path quickly enough that the queued
  GAP result may be delivered before the caller's following state publication,
  so state must be published before the API call without holding a lock that
  the callback also needs.

The replacement must operate on a complete profile:

```text
G2ConnProfile = { name, minInterval, maxInterval, latency, timeout }
```

Requested, in-flight, observed, and desired profiles must all contain the full
tuple.

### 3. Who currently controls the sender

FAST and BALANCED depth changes are protected by `sConnPriMux`, but the lock is
released before `connPriApply()` calls the BLE API. Callers run on multiple
tasks and cores. A FAST request, an R1 BALANCED request, and a reconnect reapply
can consequently overlap after their counters were individually updated.
Status 13 is already decoded as "two requests in flight."

The current path is:

```text
image, mic, R1, or reconnect task
  -> mutate a raw depth under sConnPriMux
  -> directly apply the current policy on that caller
  -> update both connected temples immediately
  -> receive the final result later on the Bluedroid callback task
```

The proposed path is:

```text
producer or R1 task
  -> acquire/release an exact logical lease
  -> mutate bounded policy state and wake the serialized policy owner
  -> policy owner computes the newest per-arm policy
  -> issue at most one full-profile update globally
  -> GAP callback copies the raw result into bounded storage and wakes the owner
  -> owner validates, records, and reconciles the newest policy
```

No timer callback and no producer task should call
`esp_ble_gap_update_conn_params()`.

### 4. Where image throughput is requested today

All production rendered BMP payloads pass through two helpers:

- `sendImageBmpMultiFragment()` performs CREATE plus the initial image push and
  holds a per-call `G2FastLinkGuard`.
- `sendImageBmpFragmentsNoCreate()` updates an existing image child and also
  holds a per-call `G2FastLinkGuard`.

This is a strong safety net: a caller cannot currently send a rendered pixel
burst without FAST. It is not enough to express producer intent. A continuous
camera loop releases FAST between frames; a four-tile logical image appears as
four separate leases; and the generic Test Suite wrapper holds an outer FAST
lease even through 60-second static inspection periods.

The transport helpers should retain a nested guard as a last line of defense.
Nesting is logically free while an outer lease is active. The semantic outer
scope belongs at the producer, where the code knows whether the next update is
automatic, input-driven, or never coming.

## Image producer contract

### Modes

| Mode | Meaning | FAST ownership | ECO-tail boundary |
|---|---|---|---|
| `None` | No rendered pixel payload is permitted in this state | None; a pixel helper rejects this mode in every build | None |
| `OneShot` | One trigger produces exactly one logical static display, possibly made of several tiles | From the first on-air transfer attempt through the final ACK/terminal failure | Final transfer attempt, never the static viewer hold |
| `OnDemand` | Every logical image burst has a distinct user/state/refresh action identity | Only for the bounded transfer attempt associated with that action | Successful final push or terminal failure for that attempt |
| `Continuous` | The producer autonomously expects another content frame, whether the sequence is finite or open-ended | One outer lease for the bounded producer sequence, including capture stalls and inter-frame gaps | Producer-sequence termination |

`OneShot` and `OnDemand` deliberately have the same radio action while a burst
is active. They remain distinct because their behavioral enforcement catches
an accidental second static batch or an automatic loop on an on-demand page.
A four-tile image representing one static frame is still `OneShot`; "batch" is
a scope boundary, not a fifth public mode. A two-frame scripted animation is
`Continuous` during that finite script because the second content frame arrives
without another action. Transport retries are not new content frames and do not
automatically turn an on-demand producer into a page-long continuous lease.

### Proposed producer-facing shape

The exact names can change during implementation, but the ownership should look
like this:

```text
G2ImageProducerPolicy { stableId, stableTag, updateMode, targetArmPolicy }
G2ImageProducerContext(policy, lifecycle, connectionGenerations)  // no FAST by itself
G2ImageActivityGuard(context, actionId, exactArmMask)              // exact token
```

Every rendered-image helper call receives an explicit producer context. The
context makes the page's declared cadence visible even while an on-demand page
has zero radio leases; the activity guard is the separate object that actually
owns FAST. `None` is rejected in every build. `OneShot` permits one logical
batch per context, `OnDemand` requires a new action id, and `Continuous` is the
only mode allowed to repeat content autonomously under one outer lease.

Token and producer records are fixed-capacity, allocation-free, and use stable
ids/tags—never borrowed stack tag pointers. Pool exhaustion latches a policy
fault that inhibits ECO and retains/pins FAST for safety; it must never let an
untracked image or mic transfer make the radio appear idle. The exact token is
released idempotently. Raw anonymous increment/decrement pairs are not safe for
a continuous lease because a destructor from an old connection generation must
not decrement a replacement generation's holder.

The callsite selects the activity scope:

- A native-size file viewer wraps its single helper call.
- A full-screen viewer wraps the entire four-tile transfer, but releases before
  the tap/60-second observation hold.
- Map wraps each bounded transfer attempt. The current infinite retry loop must
  gain a finite retry budget and suspend until new input; it must not own one
  lease forever.
- Health wraps only a graph render/retry, not the page loop or 1 Hz text tick.
- Camera Stream wraps its automatic frame loop, including skipped camera
  captures, but releases before post-stream navigation and cleanup.
- The keyboard wraps the coalesced set of dirty bands for one logical update;
  its autonomous mic countdown temporarily enters a bounded `Continuous`
  sub-context.

Transport-level inner guards remain in place and share the same exact target.
This prevents a future direct helper caller from bypassing throughput policy,
while the outer producer scope prevents false zero-depth gaps.

An actual on-air transfer attempt marks `activityOccurred`; merely constructing
a guard does not. This matters because the current helper acquires FAST before
validating its BMP. Invalid local input must not schedule an otherwise needless
ECO transaction.

### Why this does not belong only in `G2PageModule` or `G2SessionJob`

`G2PageModule::liveIntervalMs` is text/list rebuild timing. Several image
producers are not modules at all. A session-wide automatic FAST guard is also
wrong: static BMP/JPG/camera viewers keep their worker alive for up to 60
seconds after the final ACK, and Map/Health wait indefinitely for input while
their images remain unchanged.

Producer descriptors can still be attached to session payloads or exposed in
status telemetry, but the lease must be owned by the exact transfer or producer
loop. Session lifetime is not the authority.

## Production classification and wiring plan

| Surface/producer | Mode | Where the activity scope belongs | Notes |
|---|---|---|---|
| Ordinary menu, list, and text pages | `None` | No image scope | Text/list updates must never hold FAST |
| Files: native BMP | `OneShot` | Around `g2BmpViewerWorker`'s single image transfer | Release before 60 s hold |
| Files: native JPG | `OneShot` | Around decode result transfer | Release before 60 s hold |
| Files: full-screen BMP | `OneShot` | One outer scope around all four tile pushes | Do not depend on the ECO delay bridging 50 ms gaps |
| Files: full-screen JPG | Intended `OneShot`, currently excluded | No ECO acceptance coverage until repaired separately | Every current push magic is greater than 255 and the helper rejects all four quadrants |
| Sensors: Camera Capture | `OneShot` | Around the one captured frame's transfer | Release before static display hold |
| Sensors: Camera Stream | `Continuous` | Around the automatic frame-production loop | Hold across capture mutex misses and long frames; release on every exit path |
| Apps: Maps | `OnDemand` | Initial render and each accepted pan/zoom/recenter/search transfer attempt | Status-only text changes do not acquire it; add a finite retry budget, then suspend until new input |
| Apps: Health Overview | `None` | No image scope | 1 Hz updates are text only |
| Apps: Health metric/trends graph | `OnDemand` | Metric enter, Poll completion, or Trends/history graph push and retry | This is a dynamic state inside one page |
| Text entry/QWERTY image pad | `OnDemand`, temporarily `Continuous` | Initial paint and each coalesced dirty-band action; one bounded continuous scope for the autonomous mic countdown | Idle typing never owns FAST; release the countdown scope when it arms/cancels/exits |
| `g2bmp` CLI | `OneShot` | Around the transfer | Configurable observation hold is outside the lease |
| G2 microphone capture | Not an image mode; throughput lease | Pair-wide in Phases 1-3, matching current behavior | LEFT-only is a separately measured Phase 4 A/B |

Source locations for this inventory are listed in the appendix.

### Diagnostics and animated icons

The Test Suite needs policy-aware behavior before it can validate ECO:

- Static image probes are `OneShot`; their current common outer FAST guard must
  end after the final transfer, not after the tap/60-second hold and picker
  redraw.
- Automatically generated bars, streaming tests, Q25 icon packs, Q28/Q28L,
  and periodic Q30 images are `Continuous` for the bounded animation loop.
- Finite scripted multi-frame probes are `Continuous` for their bounded
  autonomous sequence. Only multiple tiles forming one static frame are a
  `OneShot` batch.
- Q33 keyboard A/B is `OnDemand`.
- Q4, the image-doc summary, and Q14 produce no pixel traffic and are `None`.
- The direct non-rendering `g2imgprobe` command sends one raw Cmd=3 diagnostic
  payload and should declare an explicit diagnostic `OneShot` policy instead
  of bypassing the producer contract.

Q25 is not yet a clean validation vehicle: its initial helper creates the
hardcoded child name `imgQ4`, while subsequent frames target `imgAnim`. Resolve
that container contract before treating it as the canonical continuous-icon
test.

For future icon pages, the rule is behavioral: an icon that advances itself is
`Continuous`; an icon that changes only because a user or external state event
arrived is `OnDemand`; a static icon sent once is `OneShot`.

## Serialized radio policy

### Complete profiles

The controller should use named, complete profiles. Values shown as
experimental are not enabled until measured.

| Profile | Min/max ticks | Latency | Timeout | Persistence |
|---|---:|---:|---:|---|
| FAST | Runtime-tested default `12/12` | 0 | 500 today | Reassert while an exact throughput lease exists |
| BALANCED | `32/48` | 0 | 500 today | Admission constraint while the R1 guard exists |
| ECO | Candidate `72/84` | Candidate 4 | **Unknown until target G2 capture** | One-shot nudge after a quiet tail; never continuously fought |
| NEUTRAL | No request | n/a | n/a | Default when no managed intent exists |

Three predicates must remain distinct:

- application request identity/de-dup compares min, max, latency, and timeout;
- predicted Bluedroid no-op compares the live interval to requested **max**,
  plus latency and timeout—the stack deliberately ignores min; and
- accepted profile requires a plausible matching self event with status OK,
  applied interval within min/max, and exact required latency/timeout.

Return a typed outcome such as `Deduplicated`, `PredictedNoOp`, `Queued`, or
`ImmediateError`, carrying the real `esp_err_t` separately. Do not reuse
`ESP_ERR_INVALID_STATE` as a private dedup sentinel. One shared validator must
enforce the BLE min/max/latency/timeout ranges and supervision-timeout
inequality before any compiled, runtime, or diagnostic profile is submitted.

### Inputs and precedence

The initial implementation can preserve pair-wide behavior, but its internal
model should be per arm so targeted FAST can be tested later.

Inputs:

```text
pair-wide exact BALANCED lease tokens
per-arm exact FAST lease tokens
per-arm last-throughput-release timestamp
per-arm activityOccurred and restoreNeeded latches
per-arm ECO armed/deadline and policy epoch
per-arm connection generation, parameter-eligible state, and copied BDA
runtime ECO experiment enable and volatile tail delay
```

Effective policy per arm:

```text
if R1 BALANCED is held:
    request the exact proven BALANCED profile
else if that arm has a FAST lease:
    request FAST
else if an eligible ECO deadline matured:
    request ECO once
else:
    assert nothing
```

BALANCED remains above FAST because it is an admission constraint for adding
the R1 link. The first implementation preserves the exact measured
`32-48 / latency 0` behavior. Whether an already-ECO link is admission-safe is
plausible but unproved; skipping BALANCED belongs in a later hardware A/B. If
tested later, BALANCED+FAST must still request exact BALANCED so concurrent
throughput remains bounded, and the decision must use a live connection-params
query rather than stale telemetry.

### State transitions

#### FAST acquire

1. Validate the requested arm mask and capture the current connection
   generations.
2. Allocate an exact lease token.
3. Cancel that arm's pending ECO deadline before any pixel/audio transfer.
4. Publish FAST as the newest desired policy unless BALANCED is held.
5. Wake the control owner.

Moving submission to an owner removes today's ordering in which the API is at
least queued before the guard constructor returns. Acquisition therefore gets
a bounded owner acknowledgement: `submitted`, `intentionally skipped/no-op`,
or `owner unavailable/late`. It does not wait indefinitely for the applied
GAP result. On a submission-ack timeout, image traffic proceeds only with the
existing measured-slow conservative pacing and records the miss; it never
assumes FAST. The page dispatcher and BLE callback task are never blocked.
Wake-to-submit latency is an acceptance metric. If the existing owner cannot
meet it because of its legacy recovery work, that work moves out or a dedicated
radio-policy task is used.

#### FAST release

1. Retire the exact token idempotently and record the last actual activity
   time only if an on-air transfer was attempted.
2. Arm a candidate only when actual activity occurred and the link was observed
   or later confirmed in a non-ECO active profile. Invalid input, no connected
   target, immediate failure, or a request that never established active state
   does not create a restoration transaction.
3. If FAST applies after activity already ended, start the quiet tail no earlier
   than both the final activity/release and the terminal FAST result. This
   prevents an immediate FAST-then-ECO pair.
4. Use wrap-safe elapsed-time arithmetic, not an absolute `millis()+delay`
   comparison, and wake the owner.

The deadline starts after the final logical activity scope, not merely after a
single fragment helper if a batch/continuous outer scope remains.

#### BALANCED acquire/release

Acquire cancels or defers both ECO deadlines and asks the owner to establish
exact BALANCED sequentially. Before scan/OPEN, the R1 worker waits for each
ready temple to reach a terminal admission-safe result. If a fast arm cannot be
made safe, the connection attempt defers/aborts rather than pretending the
guard worked. This behavior and its time budget must be hardware-tested before
merging because it makes an existing implicit assumption explicit.

On final BALANCED release, reconcile a waiting FAST lease first. A BALANCED-only
R1 window does **not** arm ECO in the first experiment. If real image/mic FAST
activity occurred wholly beneath BALANCED, preserve its `restoreNeeded`
candidate and begin a fresh quiet tail now; never execute an old, already
mature deadline beside R1 setup traffic. Restoring after a BALANCED-only window
is a separate later A/B so it cannot confound the image/mic result.

#### ECO deadline

At deadline, the owner revalidates all conditions. ECO is skipped/cancelled if:

- the runtime experiment is off;
- FAST or BALANCED is active for that arm;
- the topology mask, temple generation, BDA, or parameter-eligible state changed;
- teardown/deinit is active;
- no actual current-generation throughput activity established
  `restoreNeeded`; or
- the full observed tuple is already ECO-like.

Eligible temples are updated one at a time. ECO is a one-shot nudge. Peer
refusal, controller refusal, or timeout records telemetry and returns the arm
to NEUTRAL; it does not create a periodic retry loop.

#### A higher-priority lease during an ECO request

An HCI request already submitted cannot be recalled. ECO eligibility validation
and reservation of the single in-flight serial happen atomically under the
connection-policy mux before the BLE call. FAST/BALANCED acquisition that sees
the reservation marks it superseded. Retain the newer desired state, but do not
submit it until a terminal GAP event, an immediate API error, or link-generation
teardown proves the Bluedroid slot free.

An application watchdog never reopens the slot merely because time elapsed.
It may query `esp_ble_get_current_conn_params()`: if the live tuple proves the
desired result, record a callback-loss/no-op observation; otherwise latch the
generation's update channel uncertain and inhibit further submissions until a
terminal stack event or disconnect. The stack's own pending timer can be much
longer than the UI tail, so a local timeout followed by a retry would recreate
status 13.

#### GAP result

The BLE callback must do bounded work only. Because losing the one self
completion would permanently block the state machine, use one protected,
non-droppable self-completion latch (only one self request exists globally)
plus coalesced per-arm peer-observation slots/counters. At callback time, under
a dedicated connection-policy mux, stamp the candidate with the current local
arm generation and in-flight serial (or zero), then wake the owner. Overflow or
an occupied terminal latch is a fail-safe fault that inhibits new requests; it
is never silently overwritten. The owner then:

1. attributes the raw result by a synchronized copied BDA;
2. requires the callback-stamped serial and generation to match the current
   in-flight request;
3. validates the request's captured policy epoch;
4. records the full observed tuple and decoded status;
5. clears the in-flight slot; and
6. reconciles the newest desired policy.

The GAP event has no connection handle, application generation, or request id,
so generation stamping fences local state but cannot authenticate the event.
A very late event from the same BDA after rapid reconnect, or a peer request
using the same range, remains ambiguous. Avoid a new same-BDA request until the
old state is terminal/quarantined, and treat ambiguous events as telemetry—not
proof. This limitation must remain documented and tested.

#### Link up/down and deinit

On link down, invalidate ECO deadlines, the copied address, in-flight identity,
and old-generation FAST tokens before freeing runtime state. A late release
from an old RAII guard becomes an idempotent no-op.

On link up, reset the full per-arm request state and reconcile only after the
temple is logically connected and parameter-eligible for its new generation.
FAST data itself still obeys normal session readiness. The current
`g2ConnPriReapply()` is called before `t.connected` becomes true, while
`g2SetAllTemplesConnPriority()` skips disconnected temples, so it cannot
actually apply the active policy to the newly connecting arm. The new owner
hook must be placed after connected/generation publication. All BDA
publication, clearing, matching, and policy snapshots use one documented lock
order; no topology and connection spinlocks are nested.

### Concurrency and lifetime invariants

1. Only the selected serialized radio-policy owner calls
   `esp_ble_gap_update_conn_params()`; the raw pair sender is no longer public.
2. No BLE API is called while a spinlock is held.
3. At most one host-initiated temple connection update is in flight globally
   during the experiment.
4. The sender uses a copied, generation-validated BDA; it never dereferences
   `G2Temple::client` from delayed work.
5. Requested, desired, in-flight, observed, and application de-duplicated state
   uses the complete profile tuple; predicted Bluedroid no-op uses its actual
   max/latency/timeout rule.
6. FAST acquisition cancels ECO before transfer work can start.
7. ECO cannot start during R1 admission, teardown, a changed topology, or a
   current-generation throughput lease.
8. A rejected or stack-terminal timeout ECO is fail-open and never changes
   connection readiness or disconnect policy. An application watchdog never
   frees the controller slot on its own.
9. Peer-initiated renegotiation is not fought while NEUTRAL. FAST and exact
   BALANCED may be reasserted with bounded backoff only while their respective
   lease still exists.
10. Every delayed action is fenced by policy epoch and temple generation.
11. Every long-lived producer releases an exact lease token on every normal,
    abort, disconnect, and error exit.
12. Policy/token state is fixed-capacity and boot-lifetime; exhaustion or
    completion-latch overflow inhibits ECO and fails toward retained FAST.
13. All deadline checks are safe across `millis()` wrap.
14. No borrowed producer tag pointer outlives its storage.

### Existing control owner integration

`g2_ctrl_owner` already sleeps on a binary semaphore and caps its wait for
other deadlines, but it can currently be occupied for roughly 3.5 seconds in
legacy recovery. Reuse it only with the following requirements:

- lease mutation, link state changes, and GAP results call `g2ControlWake()`;
- its `ownerWaitMs` is capped by the next ECO deadline or watchdog observation;
- it drains connection policy immediately after every wake, before RX,
  heartbeat, mic-drain, or recovery work;
- FAST acquisition receives a bounded submit acknowledgement and R1 BALANCED
  receives a terminal admission result;
- maximum wake-to-submit latency is measured; if blocking recovery prevents the
  bound, move that work or use a dedicated lightweight policy task; and
- very-fast GAP no-op callbacks remain safe because the owner publishes the
  in-flight identity under a lock, releases the lock, and only then calls the
  BLE API.

If the API returns an immediate error and no callback will arrive, the owner
clears that exact in-flight serial under the lock and reconciles again. If a
very-fast queued callback already cleared it, the serial check prevents the
error path from corrupting newer state. The public API queues work to BTC; the
stack's internal no-op callback is synchronous within Bluedroid processing, not
a promised same-stack application callback.

`g2connpri` must also go through this owner. It becomes an explicit diagnostic
operation, serialized LEFT then RIGHT, with an operation id or bounded
owner-owned result. It may not mutate the runtime FAST tuple while a policy
request is in flight, and its local wait timing out never frees the controller
slot. `g2SetAllTemplesConnPriority()` becomes an internal policy implementation
detail so no caller can bypass the invariant.

## Additional battery opportunity: target only the arm carrying traffic

Current image helpers know their exact `G2Temple&`, and the G2 microphone is
LEFT-only, but `G2FastLinkGuard` tunes both temples. This means a one-arm image
push or recording can wake the otherwise idle temple at FAST parameters.

The internal token model should therefore carry an arm mask from the start.
For the first ECO experiment, keep the existing pair-wide target to isolate the
behavioral change. After the serialized controller is proven stable, A/B test:

- image FAST on only the actual destination arm;
- mic FAST on LEFT only; and
- pair-wide fallback if the glasses show synchronization, ACK, or display
  regressions.

This may reduce active energy more directly than shortening the tail, but it
must be measured independently so a failure is attributable.

## Runtime experiment and observability

Suggested volatile CLI surface:

```text
g2ecorestore status
g2ecorestore on 7500
g2ecorestore off
```

Properties:

- off at boot and after reboot;
- delay accepted only in a conservative 5000-10000 ms range for the first
  experiment;
- ECO tuple selected only for a detected, separately verified target-G2
  firmware/topology; store per-arm tuples if LEFT and RIGHT differ, never an
  arbitrary command value or an R1-derived universal tuple;
- disabling cancels armed deadlines but does not try to reverse an on-air
  request; and
- status is safe to call without changing the radio.

Status/telemetry should expose:

- experiment enabled flag and delay;
- active producer tags, modes, arm masks, and exact lease ids/counts;
- BALANCED/FAST effective policy per arm;
- observed full tuple per temple;
- desired and in-flight profile, generation, age, and superseded flag;
- owner wake-to-submit latency, submission acknowledgement, typed submission
  outcome, callback-loss query result, and update-channel-uncertain latch;
- ECO arm, cancel reason, deadline, fire, already-idle skip, acceptance,
  refusal, controller refusal, timeout, and stale-generation counters;
- time from final FAST release to applied ECO;
- peer-versus-self GAP counters;
- completion-latch/policy-mailbox overflow and token-pool fault counters; and
- whether R1 was disconnected, connected, scanning, or connecting.

No log line may call an `ESP_OK` queue result "applied." Applied is reserved for
the matching successful GAP result.

## Implementation sequence after this report is approved

### Phase 0: establish the hardware fact

1. Add no firmware behavior yet.
2. Capture both temple addresses and the complete peer-initiated idle tuple on
   the exact target G2 firmware, with R1 both disconnected and connected. Add a
   sanitized capture note with date, firmware, topology, and device
   classification method.
3. Determine whether `72-84 / 4` and `36-72 / 4` are firmware-, phase-, or
   topology-specific states.
4. Record the per-arm native supervision timeout and the time from last
   activity to the peer's own idle request.
5. Establish baseline variance, repetition count, and frozen thresholds for
   tap/first-image latency, mic rate, R1 connect success/time, disconnects, and
   energy before seeing the ECO-on results.

Exit criterion: a complete, repeatable G2 ECO candidate exists and is not
inferred from an R1 packet.

### Phase 1: harden profile plumbing without enabling ECO

1. Introduce the full profile type, typed submission outcomes, three distinct
   predicates, and the shared BLE profile validator.
2. Add deterministic reducer tests before changing the live sender.
3. Move all connection-update issuance—including `g2connpri`—to one serialized
   owner; make the raw sender private.
4. Reconcile policy first on each owner lap, add bounded submission/admission
   acknowledgements, and instrument wake-to-submit latency. Move blocking
   recovery or use a dedicated small owner if the bound cannot be met.
5. Add one-global-in-flight serialization, atomic reserve/supersede,
   non-droppable self completion, coalesced peer observations, and
   generation/policy fencing.
6. Replace client dereference with synchronized copied-BDA use and define lock
   order.
7. Move fresh-link reconciliation after the link is parameter-eligible.
8. Preserve exact current FAST/BALANCED profiles, require terminal BALANCED
   admission before R1 scan/OPEN, and keep ECO absent/off.

Exit criterion: existing image, mic, reconnect, and R1 tests behave the same,
with zero host-busy status-13 collisions in the exercised matrix.

### Phase 2: add explicit producer modes and exact leases

1. Add the four-value behavioral producer context and fixed-capacity exact
   token type, including overflow fail-safe behavior.
2. Make rendered-image helper callsites declare a producer; retain nested
   transport safety guards.
3. Place scopes according to the production table.
4. Give Map a finite transfer retry budget followed by a visible suspended
   state that waits for new input.
5. Use a focused ECO harness for static, on-demand, and continuous lifetime
   tests. The viewer-hold abort bug, Test Suite wrapper cleanup, full-screen JPG
   repair, and Q25 child-name fix are separately attributed patches; exclude
   broken paths until each is repaired.

Exit criterion: telemetry shows no FAST lease during a static viewer hold,
Health Overview, an idle Map/Health page, or text-only page; Camera Stream never
drops to zero FAST depth during its automatic loop.

### Phase 3: enable a runtime-only ECO experiment

1. Add the disabled-by-default gate, 7.5-second default tail, status command,
   counters, `activityOccurred`/`restoreNeeded` logic, and verified per-arm ECO
   tuple(s).
2. Send sequentially to one temple at a time.
3. Run the interaction and topology matrix below.
4. Do not retry rejected ECO periodically, and never reopen the submission
   slot from an application timeout.

Exit criterion: matching successful applied tuples, no added disconnects or
host-busy collisions, and no first-interaction/image/mic regressions.

### Phase 4: isolate further savings

1. A/B targeted-arm FAST versus current pair-wide FAST.
2. A/B tail delays within 5-10 seconds.
3. Separately test whether a live ECO-like link is R1-admission-safe and whether
   a BALANCED-only window should create a restore candidate.
4. Measure battery discharge/current under matched randomized workloads.
5. Only then consider a boot default, persistent setting, or adaptive delay.

### Phase 5: documentation acceptance

After first-party source changes, run `python3 docs2/docsctl.py update --changed`,
review and explicitly accept every affected per-file and subsystem document
according to `docs2/README.md`, then run `python3 docs2/docsctl.py check`.
Queueing the review is not acceptance.

## Test matrix

### Deterministic host/reducer tests

Before hardware, drive the policy reducer with an injected clock, API result,
link topology, leases, and GAP events. Cover:

- nested, out-of-order, double, stale-generation, and pool-exhausted lease
  release;
- `millis()` wrap;
- FAST acquisition exactly between ECO eligibility and API submission;
- immediate API failure racing a very fast callback;
- ECO supersession without a terminal callback (watchdog must not resubmit);
- peer events on the other/same arm, peer same-range ambiguity, peer flood, and
  non-droppable self-completion overflow behavior;
- disconnect/reconnect to the same BDA and callback-drain/quarantine;
- ECO disable while armed and while on-air;
- BALANCED/FAST acquire-release races and peer renegotiation while either is
  held;
- owner unavailable/shutdown or submission-ack timeout;
- `g2connpri` while a policy operation is active; and
- G2/R1/mic/image feature-disabled build variants.

### Hardware scenarios

Each row must be run with ECO off and on, using the same firmware build and
workload. Capture requested and applied full tuples, request origin, status,
disconnect reason, producer leases, and R1 state.

| Scenario | Required checks |
|---|---|
| Both temples, R1 disconnected, no page activity | No unsolicited host ECO before a managed burst; native idle tuple observed |
| Native BMP/JPG viewer | FAST only through final ACK; ECO after tail while image remains visible; tap/back remains responsive |
| Full-screen four-tile viewer | One logical FAST scope; no ECO between tiles; release before hold |
| Camera Capture | One-shot behavior; no FAST during static hold |
| Camera Stream | FAST continuously held through inter-frame and camera-mutex stalls; one tail after stop |
| Map pan/zoom/search burst and forced failures | One on-demand scope per transfer attempt; retry budget suspends instead of looping forever; idle page can return ECO |
| Health Overview | No image FAST lease |
| Health HR -> HRV -> SpO2 and Poll/Trends refresh | Each graph change cancels/rearms the tail; 1 Hz text updates do not |
| QWERTY cursor/page and mic countdown | Coalesced on-demand bands; bounded Continuous scope only during autonomous countdown; idle typing does not pin FAST |
| G2 mic capture | FAST for the exact recording; ECO only after delivery stops; nominal notification rate retained |
| R1 connect during pending ECO | ECO cancelled/superseded; exact BALANCED terminal result serialized before connect proceeds or fails closed |
| FAST requested during in-flight ECO | No second concurrent request; newest FAST waits for terminal event/immediate error/disconnect, not an app timeout; slow pacing remains safe |
| Temple disconnect/reconnect during tail | Old deadline and token invalid; no client dereference; fresh link reconciles only after ready |
| One temple only, then second connects | No stale topology action; new arm joins active policy after readiness |
| Peer self-renegotiates while FAST held | Owner reasserts FAST with bounded backoff; NEUTRAL never fights peer |
| Peer self-renegotiates while BALANCED held | Owner reasserts exact BALANCED with bounded backoff before R1 admission continues |
| ECO rejection/status 13/14/16/19 injection or observation | Correct classification, fail-open behavior, no retry storm |
| Test Suite static and continuous probes | Policy-aware lease lifetime; ECO test path not masked by generic outer guard |
| Mic starts during R1 BALANCED/connect | Measure the known admission race; either validate retained limitation or design a separate cross-subsystem fence |

Battery measurement should use repeatable scenarios such as a fixed number of
one-shot views per hour, periodic Health metric switches, a fixed camera-stream
window, and an otherwise connected idle baseline. Randomize/alternate
experiment order to reduce battery-age and temperature bias. Prefer external
current integration where practical; glasses battery percentage alone may be
too coarse for a marginal improvement.

## Acceptance criteria

The change is not ready to default on until all of the following hold:

1. The exact native G2 idle tuple is repeatable per arm/firmware/topology and
   includes timeout.
2. Every production rendered-image path has an explicit mode and producer tag.
3. Only the selected serialized policy owner issues connection updates,
   including diagnostics, and its wake-to-submit bound is met.
4. No host-busy/two-in-flight results occur in the test matrix.
5. Static viewers and idle on-demand pages show zero FAST leases.
6. Continuous camera/icon loops never allow ECO mid-loop, including stalls.
7. A new FAST/BALANCED request always cancels or atomically supersedes ECO
   safely; no application timeout reopens the stack slot.
8. Disconnect/reconnect/deinit produces no stale request, stale release, UAF,
   or new-generation cache inheritance.
9. Mic delivered rate stays above the existing 17 notifications/s degraded
   threshold, and first-image/tap P50/P95 remain inside thresholds frozen from
   the pre-experiment baseline.
10. R1 connection success/time-to-connect, disconnect count, and owner
    admission wait remain inside their predeclared bounds.
11. Applied GAP data confirms ECO rather than relying on an API return code.
12. Broken full-screen JPG and ambiguous Q25 are excluded until separately
    repaired and requalified.
13. A controlled energy test exceeds the predeclared noise/usefulness threshold;
    otherwise the added state machinery remains diagnostic/disabled.

## Known issues discovered while designing this plan

These are adjacent defects or ambiguities exposed by the backward trace. They
should not be silently bundled into the ECO experiment without separate test
attribution.

1. `probeHoldUntilTapOrTimeout()` checks only the tap flag. It does not honor
   `gImgProbeAbort`, lens lifecycle, hijack state, or connection generation.
   A replacement session can wait three seconds, find the old static viewer
   still busy, and refuse the new page. Radio scopes must end at transfer
   completion regardless; Phase 2 should also fix this lifecycle bug.
2. Full-screen JPG is currently guaranteed to reject all four pushes:
   `G2_MAGIC_IMAGE_BASE` is 210, its bases start at `+0x31` (259), and the
   helper rejects magic values above 255. It also uses four solo CREATEs rather
   than the verified BMP multi-child layout. Fix it separately; it is excluded
   from ECO acceptance.
3. Q25 creates `imgQ4` and later pushes `imgAnim`. Do not use it as the primary
   continuous validation until the child identity is corrected/verified.
4. Current fresh-link reapply occurs before the new temple is marked connected
   and therefore skips that arm.
5. Current targetless FAST unnecessarily tunes both temples for a one-arm image
   or LEFT-only mic capture; targeted FAST is a separate, promising A/B.
6. Current BALANCED acquisition does not wait for applied results before R1
   connect proceeds and can race other profile changes.
7. Map retries image failures forever at a one-second capped backoff. Without a
   retry budget it can continuously rearm the tail and defeat idle recovery.
8. A G2 mic capture can start immediately after the R1 worker's audio-idle
   check. Serialization does not close that cross-subsystem admission race;
   retain it explicitly or design a separately reviewed fence.
9. `g2_ctrl_owner` can block in legacy recovery for about 3.5 seconds. It is
   only suitable as sole policy owner after bounded-ack instrumentation and
   recovery refactoring prove prompt submission.
10. The sanitized 2026-08-22 capture provenance and exact G2 firmware are not
    yet documented, so its tuples remain unpublished bench observations.

## Point-by-point adversarial review and corrections

This section records the challenge pass requested before any firmware change.
The main plan above has already been rewritten to contain the corrected
decisions; this is the audit trail from the initial draft to the reviewed
revision.

1. **Owner handoff ordering.** The first draft assumed waking
   `g2_ctrl_owner` was equivalent to today's immediate API submission. It is
   not, and the owner can be busy for about 3.5 seconds. **Correction:** add a
   bounded submit acknowledgement for FAST, a terminal admission result for
   R1 BALANCED, policy-first owner laps, latency telemetry, and a dedicated
   policy-task fallback.
2. **Unsafe application timeout.** The first draft allowed a superseding
   profile after a bounded local timeout. Bluedroid may still own its pending
   slot. **Correction:** only a terminal GAP event, immediate API error, or
   link teardown frees submission; a watchdog queries/logs and otherwise
   inhibits further updates.
3. **Conflated predicates.** The first draft said all no-op/de-dup checks used
   the full tuple. Bluedroid ignores min for its live no-op check.
   **Correction:** separate full request identity, max/latency/timeout predicted
   no-op, and applied-profile acceptance.
4. **Conflated return values.** The current private dedup sentinel reuses
   `ESP_ERR_INVALID_STATE`. **Correction:** use typed `Deduplicated`,
   `PredictedNoOp`, `Queued`, and `ImmediateError` outcomes with the real error
   carried separately.
5. **Unproved "already slow enough" R1 shortcut.** The initial proposal treated
   ECO as automatically admission-safe. **Correction:** preserve exact
   BALANCED in Phases 1-3; test the shortcut only as a later isolated A/B.
6. **Unconditional restore timer.** A helper can acquire FAST before even
   validating its BMP. **Correction:** require actual on-air activity plus a
   confirmed/observed managed active link before setting `restoreNeeded`; if
   FAST finishes late, the tail starts after that terminal result.
7. **Completion generation overclaim.** A GAP event has no application
   generation, handle, or request id. **Correction:** stamp local serial/gen in
   the callback for fencing, quarantine across reconnect, but keep same-BDA and
   peer-same-range results explicitly ambiguous.
8. **Droppable completion mailbox.** A generic fixed mailbox could lose the
   only event that unlocks global single-flight. **Correction:** reserve a
   non-droppable self-completion latch, coalesce peer observations separately,
   and fail safe on overflow.
9. **Public bypass.** `g2SetAllTemplesConnPriority()` and `g2connpri` could
   violate owner-only issuance. **Correction:** make the raw sender private and
   route the diagnostic sequentially through the owner.
10. **Profile validation.** The first draft omitted a single formal BLE
    validator. **Correction:** validate all field ranges and the supervision
    timeout inequality before submission.
11. **Finite autonomous sequences misclassified.** The first draft called some
    scripted multi-frame work OneShot/OnDemand. **Correction:** any autonomous
    next content frame is `Continuous` for that bounded sequence; multi-tile
    construction of one static frame remains `OneShot`.
12. **Enum as documentation only.** A label would not stop an `OnDemand` loop
    from behaving continuously. **Correction:** enforce one batch for
    `OneShot`, action identity for `OnDemand`, autonomous repetition only for
    `Continuous`, and unconditional rejection for `None`.
13. **Map retry pin.** Map retries forever, contradicting a bounded on-demand
    burst. **Correction:** scope individual attempts and add a finite retry
    budget followed by a visible suspended state awaiting new input.
14. **Token storage unspecified.** Exact tokens could allocate, exhaust, or
    retain stack tag pointers. **Correction:** fixed boot-lifetime records,
    stable ids/tags, idempotent release, and overflow that inhibits ECO/retains
    FAST.
15. **Mic target contradiction.** The initial table said LEFT-only while the
    experiment promised pair-wide compatibility. **Correction:** pair-wide in
    Phases 1-3; LEFT-only is Phase 4 A/B.
16. **Undefined GATT-busy fence.** No safe nonblocking predicate existed and a
    lock check would be TOCTOU. **Correction:** remove the vague gate; the real
    fence is an activity token acquired before transfer.
17. **Full-screen JPG understated.** The initial report called it merely
    mismatched. Its push magics are all above 255 and are guaranteed to be
    rejected. **Correction:** call it broken, exclude it, and repair in a
    separately attributed change.
18. **Adjacent fixes bundled inconsistently.** The first phases mixed ECO with
    viewer/Test Suite defects. **Correction:** use a focused ECO harness and
    keep viewer abort, JPG, Q25, and generic probe-wrapper repairs separate.
19. **Hardware-only state-machine testing.** The first draft lacked
    deterministic concurrency tests. **Correction:** add an injected reducer
    suite for clocks, leases, API/callback races, peer interleaving, overflow,
    reconnect, shutdown, and build variants.
20. **R1/mic admission race.** A mic can start after the R1 audio-idle check.
    **Correction:** retain it as an explicit limitation and test it; any
    cross-subsystem fence receives separate design review.
21. **Movable acceptance targets.** "No regression" and "useful" were not
    falsifiable. **Correction:** measure baseline variance and freeze P50/P95,
    success, disconnect, mic-rate, and energy thresholds before seeing ECO-on
    results.
22. **Capture provenance.** The tuple discrepancy was not reproducible from
    the report alone. **Correction:** label it unpublished bench evidence and
    require a sanitized dated firmware/topology/classification note in Phase 0.
23. **Documentation workflow missing.** The first draft ended at hardware
    validation. **Correction:** add `docsctl update --changed`, explicit review
    and acceptance, then `docsctl check` after implementation.
24. **ECO/BALANCED source ambiguity.** The initial transition could arm ECO
    after any BALANCED-only R1 window. **Correction:** Phase 3 candidates come
    only from real image/mic throughput; BALANCED-only restoration is a later
    isolated A/B.
25. **Atomic ECO reservation.** The first draft did not explicitly close the
    check-to-submit window. **Correction:** eligibility and in-flight
    reservation linearize under the same policy mux; a concurrent FAST marks
    the already-reserved ECO superseded before the BLE call returns.

After these corrections, the core conclusion survived review: explicit
producer behavior plus a serialized, full-profile, one-shot ECO experiment is
the right direction. The review mainly changed how much concurrency hardening
must precede the experiment and which adjacent paths are safe validation
vehicles.

## Appendix: current producer source inventory

| Producer | Entry/worker | Pixel transfer |
|---|---|---|
| Native BMP file | `G2_Page_Files.cpp:1129`; `G2_Glasses.cpp:29005` | `G2_Glasses.cpp:29055` |
| Native JPG file | `G2_Page_Files.cpp:1129`; `G2_Glasses.cpp:34241` | `G2_Glasses.cpp:34280` |
| Full BMP | `G2_Page_Files.cpp:1149`; `G2_Glasses.cpp:33847` | `G2_Glasses.cpp:33966` |
| Full JPG | `G2_Page_Files.cpp:1149`; `G2_Glasses.cpp:34340` | `G2_Glasses.cpp:34393` |
| Camera Capture | `G2_Page_Sensors.cpp:1604`; `G2_Glasses.cpp:33096` | `G2_Glasses.cpp:33171` |
| Camera Stream | `G2_Page_Sensors.cpp:1618`; `G2_Glasses.cpp:33259` | `G2_Glasses.cpp:33619` |
| Map | `G2_Glasses.cpp:31498` | `G2_Glasses.cpp:31848` |
| Health | `G2_Glasses.cpp:31990` | `G2_Glasses.cpp:32361` |
| Production keyboard | `G2_Page_TextEntry.cpp:482`; `G2_Glasses.cpp:30202` | `G2_Glasses.cpp:29849` |
| `g2bmp` CLI | `G2_Glasses.cpp:28918` | `G2_Glasses.cpp:28972` |
| Generic Test Suite wrapper | `G2_Page_TestSuite.cpp:984` | Outer guard spans complete probe today |

The exhaustive diagnostic classification used for this plan also covers Q4,
Q6/Q6b, Q9-Q33, the image-doc summary, and direct `g2imgprobe`; it should be
converted into a policy table beside the Test Suite dispatch when implemented,
so new probes cannot silently inherit the wrong lifetime.
