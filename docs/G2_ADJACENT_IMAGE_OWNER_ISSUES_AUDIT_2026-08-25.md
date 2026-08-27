# G2 adjacent image and control-owner issues audit

Date: 2026-08-25

Status: source audit complete; report only; no firmware behavior changed

Scope: Map image retries, full-screen JPG, and `g2_ctrl_owner` latency

## Executive verdict

All three concerns are real, but one part of the earlier diagnosis is no
longer true.

| Issue | Verdict | Priority | Battery relevance | Recommended decision |
|---|---|---:|---|---|
| Map retries | Confirmed unbounded autonomous retry loop | P1 for the ECO project | Direct: eligible failed transport attempts repeatedly reacquire FAST and can prevent a 5–10 s ECO tail from ever expiring | Bound retries, classify failures, then suspend until explicit Retry or verified recovery/surface recreation |
| Full-screen JPG | Deterministically broken | P1 feature correctness | Secondary but real: the failed path still decodes, tiles, compresses, and briefly acquires FAST four times while transmitting no image | Replace the four solo transactions with one shared four-child full-screen transport used by BMP and JPG |
| `g2_ctrl_owner` latency | No finite wake-to-service bound; not yet measured as a field regression | P0 prerequisite for making it the sole radio-policy owner; P1 latent reliability risk today | Directly relevant: a late ECO extends FAST, and delayed image ACK parsing can trigger Map retries | Do not put a hard-SLA FAST/BALANCED/ECO controller on this task yet; use a small dedicated policy task or first remove every blocking path and prove a bound |

The earlier claim that half-connected recovery can block `g2_ctrl_owner` for
about 3.5 seconds is stale. Recovery admission occurs on the owner, but the
scan, connect, and long OPEN wait now run on `g2_ble_connect`. The owner audit
nevertheless found more serious long-tail paths: GATT writes that can occupy a
single call for over 60 seconds in the worst source-permitted case, raw SD
writes and UART waits without a hard bound, and an R1 telemetry mutex that can
be held across a G2 GATT send.

For the battery project, Map is the immediate behavioral blocker. For the
radio-policy architecture, the owner latency is the foundation blocker. The
JPG defect should be repaired as a separately attributed correctness patch.

## Evidence and limits

`python3 docs2/docsctl.py status` reports 234 of 428 documents fresh, with 185
stale, four missing, and five modified. `INDEX.md` and
`files/G2_Glasses.cpp.md` are stale. They were used for discovery only; every
material conclusion in this report was checked against the current working
tree.

The working tree already contains user changes in the relevant G2 files. Line
numbers below describe that current tree. No source file was edited for this
audit.

This is a static source audit. It proves deterministic code paths and source
bounds, but it does not claim that every worst-case stall has occurred on the
current hardware. In particular, owner wake-to-service latency is not currently
instrumented, so healthy-path latency and percentile claims require a hardware
run.

## 1. Map's unlimited image retries

### Finding

The Map worker retries every image failure without a ceiling while the session
remains active. It starts with
`dirty=true`, renders a 288x144 BMP, makes one strict no-CREATE push, and on any
false result leaves `dirty=true`. Its backoff is 50, 100, 200, 400, 800, then
1,000 ms for every later attempt, with no ceiling and no suspended state.

Primary source anchors:

- Map worker and initial state:
  `components/hardwareone/G2_Glasses.cpp:31498-31677`.
- Render, generation coalescing, and push:
  `components/hardwareone/G2_Glasses.cpp:31808-31858`.
- Unbounded retry:
  `components/hardwareone/G2_Glasses.cpp:31861-31893`.
- Status processing that is skipped on every failure:
  `components/hardwareone/G2_Glasses.cpp:31896-31914`.
- No-CREATE image transport:
  `components/hardwareone/G2_Glasses.cpp:28288-28489`.

The generation checks before transport are good: if a tap arrives while a map
is being rendered, the stale frame is dropped and the newest state is rendered.
Connection generation and lens lifecycle fences also block stale-surface
writes. Those protections do not bound autonomous work after a persistent
failure.

### Why the one-second cap is misleading

The cap applies only to the delay between outer attempts. A live-fence attempt
can hold `G2FastLinkGuard` through compression, fragment transmission,
sliding-window throttle, and final ACK completion.

- Strict in-flight throttling can wait 14 seconds before abandoning a stuck
  window (`2 * kImgPushAckTimeoutMs`).
- A fully submitted burst can wait seven seconds for final ACK completion.
- Each Cmd=3 fragment already has three lower-level TX attempts.
- If a fragment fails before every fragment is submitted, `aborted` remains
  false and the helper still performs the seven-second final wait even though
  the registered ACK target has become impossible to reach.
- Every lower-level GATT write can itself have a long completion tail, as
  described in the owner section.

The relevant constants and paths are at
`components/hardwareone/G2_Glasses.cpp:26430-26509`,
`:26835-26870`, and `:28366-28489`.

Consequently, the current behavior is not "one cheap retry per second." It can
be a nearly continuous sequence of expensive render, PSRAM/LZ4, GATT, and ACK
episodes. Each eligible attempt acquires the global FAST lease; when BALANCED
is not masking it, the current policy requests FAST on both connected temples.
With the current release behavior, the glasses may then retain an active radio
tail for minutes; with delayed ECO, every new attempt can cancel and re-arm
that tail.

### Failure types are incorrectly collapsed

The boolean result combines conditions that need different policies:

| Failure class | Examples | Correct action |
|---|---|---|
| Superseded/cancelled | New Map generation, Back, terminal lifecycle | Do not consume retry budget; render newest state or exit |
| Surface/link no longer valid | Generation/lifecycle mismatch, disconnect | Exit or deliberately recreate; never retry an obsolete fence |
| Recoverable plugin silence | `pluginDead` on the same generation | Suspend without rendering or FAST; poll eligibility cheaply |
| Local resource/preparation | Scratch/push-buffer allocation, BMP/protobuf build | At most a very small retry allowance, then visible suspension |
| Transient transport | Central/write busy, incomplete Cmd3 TX, ACK timeout | Finite retry batch with absolute deadlines |
| Peer rejection | Explicit `ImageRawResp` failure/invalid container | Usually terminal for this surface; at most one controlled recreate |

Today all of these become the same outer retry. A stale surface can therefore
cause pointless rerendering even though the transport helper rejects before
FAST, while a real transport failure can repeatedly keep FAST active.

### Additional correctness and lifecycle problems

1. Every outer attempt reuses a magic prefix beginning at 243. A raw canonical
   Map frame uses 243–248, while auto-LZ4 can shorten the range. The response
   matcher uses
   MapSessionId when the glasses include it, but supported responses may omit
   that field and explicitly fall back to side + connection generation + magic.
   A late ACK from failed attempt A can therefore be counted after attempt B
   resets ACK state and reuses the same window. This is a structural ambiguity;
   routine late delivery beyond seven seconds has not been proven on hardware.

2. A task notification ends the backoff early. This is good for Back/input
   responsiveness, but the current code immediately attempts the same dirty
   frame again. Repeated taps can bypass the intended retry delay.

3. A Back tap received during an active push does not cancel that push. It can
   wait behind the throttle/ACK/GATT tail before the worker drains the Back
   action.

4. Persistent failures `continue` before the one-second status block, freezing
   GPS/status text for the entire episode.

5. A LEFT-only Map session has a concrete bad edge: a LEFT disconnect can make
   its fence permanently stale without necessarily clearing the global hijack
   flag, leaving the outer loop free to rerender and fence-fail forever.

6. Cleanup and `onDone` are unconditional. A terminal or replacement session
   can therefore be followed by navigation from an obsolete Map worker. Health
   already has stronger current-session checks that Map can follow.

### Recommended Map state machine

Replace `dirty + consecutiveFrameFailures` with explicit state:

```text
state = Idle | RenderNeeded | PushReady | Backoff | Suspended
desiredGeneration
preparedGeneration
preparedBmpValid
attemptsForRequest
attemptsForFailureEpisode
nextAttemptAtMs
lastFailureKind
```

Required behavior:

1. Drain controls first on every wake. The Back ingress path must also publish
   an exact per-transfer cancellation token. Transport must poll it during
   throttle/ACK waits and between ATT envelopes, so exit latency is bounded by
   at most the one physical write already in progress rather than the entire
   logical image burst.
2. Render once for the newest generation. A generation change during render
   consumes no transport retry.
3. Keep the packed BMP in session scratch for transport-only retries while the
   generation is unchanged; do not rerender identical content.
4. Use an absolute `nextAttemptAtMs`. Notifications may wake input handling but
   cannot accelerate the same request's radio retry.
5. Return a typed transport outcome such as `Success`, `Cancelled`,
   `StaleFence`, `PluginSilent`, `LocalResource`, `TxIncomplete`, `AckTimeout`,
   and `PeerFailure`.
6. Start with an experimental budget of one attempt plus two full-frame
   retries. Each Cmd3 already has three internal TX attempts. Keep this runtime
   tunable until three-link hardware data exists.
7. Add a failure-episode circuit breaker that survives new pan/zoom generations.
   A per-request budget alone can be defeated by repeated taps.
8. When the budget expires, set `Suspended`, clear autonomous image work, keep
   status/input processing alive, and show a one-shot message such as "Map
   update paused — tap Retry." The error update itself must not retry forever.
9. Ordinary pan/zoom/search input may update the desired generation while an
   episode is suspended, but it must not reset the circuit breaker. Only an
   explicit Retry action, verified same-surface renderer recovery, or a
   deliberate successful surface recreation may open a new bounded episode.
   Back always exits.
10. Use one centralized allocator to prevent overlapping live magic bands
    across image owners. Within a bounded Map failure episode, do not reuse a
    band from an ambiguous or aborted attempt; suspend if the episode cannot
    obtain a safe band rather than wrapping immediately. Permanent global
    quarantine is not viable in a u8 namespace and would eventually halt
    continuous pages. Before bands are reused indefinitely, establish and test
    an ACK-drain/reuse horizon, add stronger peer correlation or a proven
    protocol-session reset, or explicitly accept and test the residual
    late-ACK ambiguity. Completed and ambiguous bands should not automatically
    receive the same reuse policy.

Reusing a prepared BMP assumes the request generation covers every map-state
mutation. Some map globals are shared with non-G2 callers. Either snapshot all
render inputs into the request, add a shared map-state revision, or explicitly
define a transport retry as "retry these exact previously prepared bytes."

Transport hardening should accompany the state machine:

- If not all fragments were submitted, disarm ACK tracking immediately instead
  of waiting seven seconds for impossible completion.
- Check abort before acquiring FAST or compressing.
- Preflight local inputs before acquiring FAST. Once a FAST connection-profile
  request is submitted, mark `restoreNeeded` and schedule the quiet-tail restore
  on final release even if no Cmd3 payload is ultimately written; otherwise a
  later local failure can strand an asserted FAST profile.
- Carry the exact lifecycle fence between ATT envelopes, not only between
  logical Cmd3 fragments.
- Make cleanup and `onDone` conditional on exact session/surface ownership.

### Map acceptance tests

- Success on attempt one; fail then succeed; finite failures then `Suspended`.
- Same-request wakeups do not shorten the absolute backoff.
- Repeated pan generations cannot bypass the failure-episode breaker.
- A new generation during render consumes no retry and no FAST lease.
- Transport retries reuse the prepared BMP; a new generation invalidates it.
- Scratch/protobuf-buffer allocation, Cmd3 TX, explicit peer failure, throttle
  stall, and missing-ACK injection all choose the intended typed policy; failed
  or non-useful LZ4 compression follows the current successful RAW fallback.
- Incomplete TX skips the impossible seven-second completion wait.
- Inject late attempt-A ACKs without MapSessionId after attempt B is armed;
  verify they cannot satisfy B.
- Back, replacement, RIGHT disconnect, LEFT-only disconnect, terminal event,
  and plugin silence at every transfer stage.
- On hardware with G2 alone and G2+R1: finite Map Cmd3 count, Map's FAST depth
  returns to zero, delayed ECO fires after the last failed batch, and no further
  Map Cmd3 or Map-caused connection update occurs until explicit Retry or
  verified recovery.

## 2. Broken full-screen JPG

### Deterministic proof

Files routes JPG/JPEG "View Full" to `g2ShowJpgFileFullScreen()` at
`components/hardwareone/G2_Page_Files.cpp:1142-1151`. The worker decodes the
file into RGB888, converts it to a canonical 288x144 4-bpp BMP, tears down the
Files page, builds four quadrants, and calls `sendImageBmpMultiFragment()` four
times (`components/hardwareone/G2_Glasses.cpp:34114-34229` and
`:34340-34412`).

`G2_MAGIC_IMAGE_BASE` is 210
(`components/hardwareone/System_G2_Protocol.h:170`). The four push bases are:

| Quadrant | Expression | Actual base | Valid u8? |
|---:|---|---:|---|
| 0 | `210 + 0x31` | 259 | No |
| 1 | `210 + 0x41` | 275 | No |
| 2 | `210 + 0x51` | 291 | No |
| 3 | `210 + 0x61` | 307 | No |

The helper rejects when `pushMagicBase + fragmentCount - 1 > 255` before it
constructs or sends CREATE (`components/hardwareone/G2_Glasses.cpp:27799-27816`).
All four calls therefore return false. No image CREATE, Cmd3 fragment, or image
ACK window is produced after the initial page teardown.

The caller ignores every false result, logs "image up," and enters the 60-second
hold without creating a replacement image surface
(`components/hardwareone/G2_Glasses.cpp:34392-34412`). The exact visible
post-teardown pixels are firmware/state dependent, so source alone does not
prove that the lens is blank. Because the helper takes `G2FastLinkGuard` and
prepares/compresses the payload before validating the magic, this deterministic
local rejection can still create four FAST lease cycles despite transmitting
no image. Today that can unnecessarily assert/retain the active profile; under
delayed ECO it would cancel and re-arm the quiet tail four times.

The stale docs2 note also blames CREATE magic. Current source now uses
`allocCreateMagic()`, so CREATE magic is no longer the guaranteed blocker. The
push bands are.

### Why changing four constants is not a fix

`sendImageBmpMultiFragment()` hardcodes one child: CID 2, name `imgQ4`, at
`(0,0,288,144)` (`components/hardwareone/G2_Glasses.cpp:27793-27816`). Calling
it four times attempts four independent one-child CREATE transactions. There
is no inter-iteration Shutdown despite the comment saying "Shutdown+CREATE."

Even with valid magics, this cannot construct a 2x2 canvas. Depending on
firmware behavior, later CREATEs would be rejected, replace the earlier child,
or leave only one quadrant at the top-left.

The correct wire shape already exists in the BMP full-screen worker:

- four children with CIDs 2–5 and positions `(0,0)`, `(288,0)`, `(0,144)`, and
  `(288,144)` at `components/hardwareone/G2_Glasses.cpp:33871-33887`;
- one `g2BuildCreateImageMulti()` at `:33904-33910`;
- one correlated CreateResp wait at `:33916-33942`; and
- four `sendImageBmpFragmentsNoCreate()` pushes into the exact children at
  `:33954-33982`.

Q12 and Q21 independently exercise the same multi-child shape. A canonical
20,854-byte tile needs six raw 3,800-byte Cmd3 chunks. Existing seven-wide
magic reservations are conservative headroom, not the current raw count.

### Do not copy the BMP worker verbatim

The BMP worker has the right topology but not a complete transaction contract:

- It ignores the boolean result of all four no-CREATE pushes. `anyTileFailed`
  is set only for tile-construction failure, so partial/zero transport can
  still log success and hold for 60 seconds.
- Several CREATE-preparation/fence failure branches leave probe cleanup
  unbalanced.
- It stamps `noteOurShutdownSent()` immediately before CREATE even though that
  stamp represents an intentional Cmd9 Shutdown echo lease.
- The shared hold checks only the tap flag. It ignores abort, lifecycle,
  connection generation, and surface ownership. An ordinary replacement sets
  abort, waits three seconds, then refuses the new session while the viewer may
  remain in its 60-second hold.
- Viewer cleanup and `onDone` are unconditional and can shut down or redraw
  over a newer same-lifecycle presentation.

The repair should improve the shared full-screen path, not duplicate these
gaps into JPG.

### Recommended full-screen transport

Keep file loading separate, but extract one full-screen transport core used by
both BMP and JPG:

1. Each loader returns an owned, validated canonical 288x144 4-bpp BMP.
2. Allocate tile scratch and preflight the four child specs/buffer capacity
   before tearing down the current page. Do not allocate the CREATE sequence or
   build its wire envelope yet: an intervening Cmd9 must receive the earlier
   sequence.
3. Use a scoped probe/lens transaction whose destructor balances every exit.
4. Tear down once, then allocate magic/sequence and send exactly one rotating-u8
   four-child CREATE.
5. On successful CreateResp, commit an image-specific publication transaction
   that advances presentation identity and returns an exact surface token
   containing connection generation, lifecycle, presentation, and owner
   serial. The current BMP path does not make this publication, so adding only
   fields to `G2ImageCreateFence` is insufficient.
6. Push four tiles into their exact CID/name with four valid, disjoint u8
   windows. Treat any false result, incomplete fragment count, explicit Cmd4
   failure, timeout, abort, or stale fence as terminal for this display.
7. Acquire one outer FAST lease only for CREATE plus all four pushes. Keep inner
   transport guards as a safety net. Decode/preflight failures must never tune
   the radio.
8. Enter the hold only after all four pushes fully complete.
9. Make the hold observe abort, link/lifecycle/surface ownership, tap, and
   timeout.
10. Keep cleanup authority separate from navigation authority. A confirmed
    surface may receive final Cmd9 only under its exact surface token. A sent
    CREATE with an ambiguous timeout needs the existing quarantined compensating
    Shutdown even though no surface was confirmed. Independently, after a
    destructive teardown the Files callback may restore the menu only if the
    originating navigation/session token remains current, whether the image
    succeeded or failed.

The first repair should not attempt an unverified single 576x288 ImageObject,
nor broadly merge the single-tile and full-screen workers.

### Secondary safety finding from this trace

JPG conversion produces a canonical BMP, so its current intermediate is safe
for the tiler. The BMP full-screen loader is not equally strict:

- `readBmpFromVfs()` does not prove that pixel offset plus the complete pixel
  storage lies inside the file.
- `buildTileBmpFromQuadrant()` checks only `bfOffBits <= srcLen` before reading
  all quadrant pixels.
- Extended DIB headers are accepted while the tiler assumes a 40-byte DIB and
  immediately following palette.
- `INT32_MIN` width or height can reach unsafe signed negation.

A truncated user BMP can therefore produce an out-of-bounds read in the path
that the new shared transport would consume. Canonical validation must reject
truncated base or extended DIB headers, prove palette bounds, and use checked
arithmetic for `bfOffBits + rowStride * absHeight`. It must be part of
extraction, not deferred.

The JPEG resource gate also needs tightening. A 128 KiB compressed-file cap
does not bound decoded memory: the accepted 1600x1600 limit permits a 7,680,000
byte RGB888 allocation plus the compressed file and BMP. `PreferPSRAM` may fall
back to internal memory, and the 16 KiB free-heap check does not prove PSRAM
capacity. Use an overflow-checked decoded-byte limit, largest-PSRAM-block
reserve, PSRAM-required allocation for bulk buffers, and cancellation checks
before destructive lens teardown.

### JPG/full-screen acceptance tests

- Pure test of a four-labelled-quadrant JPEG through decode and tiling,
  including top-down/bottom-up orientation and 2x expansion.
- Parse CREATE and assert exactly four unique children, CIDs 2–5, correct
  positions, and no magic above 255.
- Script CREATE reject/timeout and a failure on each tile; no partial failure
  may enter hold or log success.
- Late/duplicate/out-of-order ACKs with and without MapSessionId.
- Malformed/truncated BMP tests under ASan, including offset overflow, missing
  pixels, extended DIB, invalid dimensions, and `INT32_MIN`.
- Cancellation during file read, decode completion, teardown, CREATE, each tile,
  inter-tile gap, and hold.
- Hardware capture must show one four-child CREATE, one matching success, four
  child-specific Cmd3 streams with complete ACK sets, and no CREATE/Shutdown
  between tiles.
- Test G2 alone and G2+R1; double-tap, timeout, disconnect, and same-lifecycle
  page replacement; repeat 50–100 cycles while tracking heap, largest PSRAM
  block, callback count, and probe state.
- Nested safety guards may still call acquire/release, but telemetry must show
  exactly one global FAST-depth `0 -> 1` transition and one `1 -> 0` transition
  for the transfer, with no zero-depth gap between tiles. Delayed ECO starts
  only after the final transition.

## 3. Potentially excessive latency in `g2_ctrl_owner`

### Correction to the earlier report

Half-connected recovery no longer performs its scan/connect on the owner.
`recoveryHeartbeatTick()` only admits `BleConnectKind::G2_REPAIR` at
`components/hardwareone/G2_Glasses.cpp:12626-12685`. The central worker calls
`attemptMissingArmRecoverySync()` at `:16197-16206`. The stale shutdown comment
at `:14406-14407` still cites a 3.5-second owner scan and should be corrected,
but it is not current behavior.

### The six-second sleep is also not the problem

The task sleeps on a binary semaphore. Its nominal wait is 250 ms during a
control transaction, one second during mic/EvenAI/raw-recorder activity, and
otherwise six seconds (`components/hardwareone/G2_Glasses.cpp:14051-14087`).
An explicit `g2ControlWake()` makes a sleeping task runnable immediately, so a
normal signaled event does not pay six seconds.

The actual problem is residual current-lap time. A wake cannot preempt a task
already blocked inside the current lap. Multiple wakes coalesce into one
binary token, and there is no enqueue timestamp, reason bitmask, lap timer, or
phase timer. Therefore the current source has no finite wake-to-service bound
and no measurement of its percentiles.

The current post-wake order is:

1. mic GATT reassert, connection-profile reapply, and mic-rate watchdog;
2. raw mic SD drain;
3. priority native WAKE/EXIT RX drain;
4. EvenAI cancel/session work, including GATT and UART;
5. ordinary RX drain;
6. SyncInfo lifecycle reduction;
7. HeadUp/notification control reconcile, including GATT; and
8. up to four heartbeat sends plus safety/recovery maintenance.

See `components/hardwareone/G2_Glasses.cpp:14102-14239`. A proposed radio
policy placed near the current reconcile would be behind every earlier phase.
Even the priority native queue cannot preempt an in-progress mic, SD, or GATT
operation.

### Confirmed long-tail paths

| Path | Source-permitted delay | Why it matters |
|---|---|---|
| Owner-originated G2 GATT write | 1.5 s central gate + 1.5 s temple mutex + up to 30 s prior-write gate + up to 30 s completion + 100/200 ms cooldown; about 63.2 s for one one-chunk normal send in the worst composition | Owner holds outer TX locks; later RX, heartbeats, native EXIT, and policy cannot run |
| Heartbeat GATT write | 50 ms central gate, but still 1.5 s temple mutex and the two 30 s characteristic waits | The short central timeout does not make the physical write short |
| R1 forwarded telemetry | `RingTelemetryGuard(portMAX_DELAY)`; another path holds that mutex across a G2 send | Owner can inherit the full GATT tail transitively |
| Health series update | Forwarded telemetry calls Health while holding the R1 lock; Health series lock is also `portMAX_DELAY` and rendering holds it through composition | Adds another cross-subsystem blocking dependency |
| Raw mic recorder | Up to four batches/32 `File::write()` calls; FS-lock waits are bounded, physical write/close and tail mutex are not | A storage stall delays the next owner lap and mic reassert |
| UART event publication | Lifecycle and TX mutex waits plus a HAL UART mutex/buffer wait without a hard source bound | EvenAI owner work can delay unrelated G2 control |
| RX drains | `while (dequeue)` on queues that may refill concurrently | Depth 4/8 bounds queued storage, not total drain-loop duration under continuous refill |

The G2 send path takes outer gates at
`components/hardwareone/G2_Glasses.cpp:11318-11354`, then calls the legacy
three-argument `writeValue(..., false)` at `:11242-11246`. The local patched
Arduino BLE library maps that overload to 30,000 ms at each of its two
semaphore waits:
`components/arduino/libraries/BLE/src/BLERemoteCharacteristic.h:118` and
`components/arduino/libraries/BLE/src/BLERemoteCharacteristic.cpp:804-868`.
Fragmented envelopes can invoke the characteristic path more than once.

The library's authentication wait is a no-op in the current tree:
`BLESecurity::m_authReq` defaults to zero, the wait returns when bonding is not
enabled, and no first-party `setAuthenticationMode` call exists. If bonding is
added later, its default ten-second wait becomes another owner consideration.

The R1 transitive block is concrete. Owner RX dispatch calls
`g2RingNoteForwardedTelemetry()` for sid 0x90/0x91 at
`components/hardwareone/G2_Glasses.cpp:11115-11125`. That takes the telemetry
mutex indefinitely at `components/hardwareone/G2_Ring.cpp:2820-2825`.
`ringSpoofSendOnce()` holds the same mutex while calling
`g2SendToRightTemple()` at `components/hardwareone/G2_Ring.cpp:5973-6023`.
It is not necessarily a lock cycle; the supported conclusion is
cross-subsystem lock-held blocking and cross-device latency coupling.

Raw recording similarly bounds acquisition of the FS lock but not the
operation done under it. See
`components/hardwareone/G2_Glasses.cpp:3758-3859`. Its post-write ring-tail
advance uses `portMAX_DELAY`.

### Existing and battery-project impact

- Image ACKs are parsed by this owner. A stall can fill/drop the eight-entry
  ordinary RX queue, cause a sender's seven-second ACK wait to expire, and turn
  one healthy on-air image into an apparent failure.
- Map then amplifies that failure into its unbounded retry loop.
- Native EXIT, SyncInfo's 400 ms lifecycle decision, control ACKs, and page
  response processing are delayed.
- Headless mic reassert targets about one second because the glasses can stop
  delivery after roughly two seconds. An SD/GATT/R1 stall can miss that window.
- Heartbeat/watchdog work is delayed and can create false failure/reconnect
  behavior.
- An epoch-current delayed-ECO request that is merely late is functionally safe
  but prolongs FAST and erodes the battery saving. A stale ECO submitted after
  a newer FAST/BALANCED lease is not safe: it can slow an image or halve mic
  delivery. Desired-policy and generation epochs must be revalidated
  immediately before submission.
- A late FAST request is not performance-safe: image pacing can begin on the
  slow interval and mic delivery has been observed to halve.
- R1 BALANCED is an admission prerequisite, not best-effort timing. Ring
  scan/connect must defer or abort unless the observed BALANCED tuple is
  attributable to the current link lifecycle. After an ambiguous same-BDA
  reconnect, neither a local terminal result nor the tuple alone proves that
  attribution.

### Recommendation for the radio policy

For the first delayed-ECO experiment, use a small dedicated radio-policy
submitter rather than making the current owner the sole issuer. Its contract
should be deliberately narrow:

- no GATT, filesystem, UART, rendering, or protobuf work;
- fixed-capacity state and nonblocking wake/event bits;
- the raw GAP sender is private and every FAST, BALANCED, ECO, reconnect
  reapply, and diagnostic update goes through this sole issuer;
- full-profile desired/in-flight/observed state per arm;
- BDA + connection-generation + local policy-serial fencing, while recognizing
  that the GAP event echoes no generation or serial and cannot authenticate a
  late same-BDA event by itself;
- at most one global connection update in flight as a conservative first-
  experiment policy, not as a claimed Bluedroid requirement; this adds two-arm
  latency and may be relaxed per link only after evidence;
- preserve the current effective precedence `R1 BALANCED > FAST > ECO` whenever
  a Ring BALANCED lease is held, and `FAST > ECO` otherwise;
- publish request state before `esp_ble_gap_update_conn_params()` and hold no
  callback-shared mutex across it. The public API queues BTC work rather than
  nesting the application callback, but the callback can race the caller's
  next state publication almost immediately;
- callback copies a bounded event into a non-droppable completion latch and
  wakes the task without blocking. Ambiguous peer/same-range or late same-BDA
  events remain observations, not authenticated self completions; and
- typed submission/admission result so a binary wake is never mistaken for an
  acknowledgment.

An application timeout must not blindly reopen the update slot. If completion
is ambiguous, quarantine the request. The observed tuple alone is also
insufficient after same-BDA reuse because a late prior-link callback can write
that tuple. Ring admission may use the required BALANCED tuple only after a
proven link-lifecycle/event-drain barrier excludes prior-link events; otherwise
it must fail or defer throughout the ambiguous reconnect window. A locally
claimed self-ACK identity that the peer never echoes is not a substitute.

This is not free: another task consumes internal stack and adds a lifecycle to
start, stop, and deinitialize. Create it only while G2 runtime is active, size
it from measured stack watermark, and include allocation/start failure in the
typed fallback. That cost is preferable to silently inheriting unrelated
multi-second I/O, but it must be measured rather than assumed negligible.

If avoiding another task is important, `g2_ctrl_owner` can be reused only after
the blockers below are removed and fault injection proves a maximum
wake-to-submit bound. Merely moving policy reconciliation to the top of the lap
improves normal ordering but does not solve a wake that arrives during a
60-second GATT call.

### Owner hardening independent of the radio decision

1. Add an atomic earliest-pending timestamp and wake-reason bits. Record
   callback/enqueue, task wake, phase start/end, policy submit, callback-applied,
   lap duration, queue age/depth/drop, and stuck phase. Report p50/p95/p99/max.
2. Move raw SD draining and its tail commit to one dedicated low-priority
   writer. At minimum, run them after urgent RX/policy work. Do not simply add
   a timeout to post-write tail advancement: failing to commit a successful
   write makes the next lap duplicate packets.
3. Snapshot R1 telemetry and generation under its mutex, then release the lock
   before G2 TX. Hand forwarded parsing to a ring-owned queue or use bounded
   admission. The snapshot must be immutable and revalidated against Ring
   identity/generation before send so old cache data is not relabelled after a
   reconnect. Preserve the established telemetry-to-series lock order.
4. Budget RX drains by the initial count or a time/count quota, then resignal if
   data remains. Do not use while-until-empty against a concurrently refilled
   queue.
5. Give owner-originated GATT sends an explicit short timeout or enqueue them to
   a TX worker. Shortening the library timeout requires late-WRITE_CHAR_EVT and
   gate-recovery tests because a completion timeout intentionally leaves the
   inner gate closed until a late event or disconnect.
6. Make UART publication try/queue rather than waiting on lifecycle/TX/HAL
   locks from the owner.
7. Make heartbeat pending/reason state atomic; the current volatile read-then-
   clear can lose a timer set between those operations.
8. Remove the stale recovery comment and make shutdown diagnostics report the
   exact stuck owner phase. The current six-second shutdown safely retains
   resources if the owner has not exited, but its rationale no longer matches
   the real hazards.
9. Move connection-policy reconciliation to after a new temple's connected and
   generation state is published. Current `g2ConnPriReapply()` runs at
   `components/hardwareone/G2_Glasses.cpp:15050`, before
   `g2SetTempleConnected(t, true)` at `:15147`, so an active FAST/BALANCED
   policy can skip the newly connected arm.

### Owner/radio acceptance tests

- Wake before take, while sleeping, during each phase, after state snapshot,
  and multiple coalesced wakes. Level state must survive; exact submit ACKs may
  not be inferred from semaphore tokens.
- Hold central mutex, temple mutex, characteristic gate, R1 telemetry mutex,
  Health series lock, FS lock, SD write/close, UART locks, and continuously
  refill RX. The radio controller must meet its declared submit SLA or return a
  typed failure/fallback.
- Drop and delay WRITE_CHAR_EVT; verify no stale later completion satisfies a
  new operation.
- FAST acquire cancels ECO; final FAST release is epoch-fenced; a timed-out old
  FAST cannot apply after release.
- No-op GAP callback racing the caller's post-call path, immediate rejection,
  ambiguous peer event, late callback, reused BDA after reconnect, and one-arm/
  two-arm topology changes.
- R1 BALANCED wins over both FAST and ECO while its lease is held. Exercise a
  concurrent mic/image FAST request, record delivery impact, and verify Ring
  connect aborts/defers unless current-link-authenticated BALANCED admission is
  available. A late prior-link same-BDA callback must not admit it.
- Dual G2 + R1 hardware stress: image push, mic capture, raw recorder with SD
  contention, Ring bridge/spoof, half-arm recovery, continuous settings/RX, and
  debug logging. Record wake-to-submit/apply, lap/phase time, queue drops,
  heartbeat gaps, mic rate, and disconnects.
- Shutdown during every injected long phase; no UAF, and retained resources
  identify the exact blocking phase.

## Cross-issue failure chain

These findings should not be treated as three unrelated cleanup items. One
plausible chain is:

```text
R1 spoof holds telemetry mutex across G2 GATT send
  -> g2_ctrl_owner blocks parsing forwarded/ordinary RX
  -> image Cmd4 ACK queue fills or ACK service misses its deadline
  -> Map classifies the frame as failed
  -> Map retries without a ceiling while the session remains active
  -> each eligible attempt reacquires the global FAST lease when BALANCED is absent
  -> delayed ECO is continually cancelled/rearmed
```

This is why the fixes need independent attribution but a combined stress test.
Fixing Map bounds the amplifier. Hardening/decoupling the owner removes a
source of false image failures. Fixing JPG prevents a separate no-data path
from creating FAST tails and 60-second holds with no replacement image surface.

## Recommended implementation order

Use separate patches and measurements so a result remains attributable. The
critical path to a controlled battery experiment is:

1. **Instrumentation only.** Add owner wake/phase measurements, image typed
   failure counters, retry-episode counters, and FAST acquisition/release tags.
   Do not enable ECO yet.
2. **Map circuit breaker.** Add typed image outcomes, impossible-ACK early
   disarm, exact cancellation, absolute backoff, finite per-request and
   per-episode budgets, suspension, exact cleanup, a central live-band magic
   allocator, and bounded-episode quarantine for ambiguous attempts.
3. **Serialized radio policy.** Use the dedicated narrow policy task for the
   first experiment. Preserve current FAST/BALANCED behavior with ECO absent,
   prove conservative one-global-in-flight reconciliation and post-connect arm
   reapply, then enable runtime-only ECO.
4. **Hardware A/B.** Test G2 only and G2+R1 with injected failures and normal
   workloads, then run controlled current/discharge measurement. Connection
   timing alone is mechanism evidence, not battery-life proof.

Two independently attributable hardening tracks can proceed alongside that
path; neither needs to be silently bundled into the ECO result when the
dedicated policy task is used:

- **Full-screen transport:** add canonical BMP validation and the shared
  four-child transaction; migrate BMP full under tests, then JPG full; correct
  hold, compensation, navigation callback, and surface publication at the same
  boundary.
- **Owner hardening:** remove I/O under the R1 telemetry lock, move raw storage
  and blocking UART/GATT work off owner, budget RX drains, and make wake state
  atomic. This remains important for existing image ACK, mic, native EXIT, and
  heartbeat reliability even though it is not a prerequisite for a separate
  policy task.

Map must be bounded before delayed ECO is enabled. The policy controller must
not depend on the current owner until its bound is proven. Run a final combined
G2+R1 stress regression after all tracks converge.

## Point-by-point challenge pass

The recommendations above already incorporate these corrections:

1. **"One-second retries are bounded enough."** False: that is only the outer
   sleep; a failed attempt can hold FAST through 14-second throttle or
   seven-second ACK waits and long GATT calls. **Correction:** bound attempts
   and eliminate known-impossible waits.
2. **"A per-request retry count solves Map."** Incomplete: every tap creates a
   request. **Correction:** add a failure-episode circuit breaker that ordinary
   pan, zoom, search, or metric changes cannot reset; reset it only for an
   explicit Retry action, verified renderer recovery, or successful surface
   recreation.
3. **"Back can wait for the current push and then drain queued requests."** That
   can leave the user inside throttle and ACK waits. **Correction:** give each
   transfer an exact cancellation token, poll it during throttle/ACK waits and
   between ATT envelopes, and bound Back only by an already-started physical
   write.
4. **"Rotate MapSessionId to reject old ACKs."** Incomplete: supported replies
   may omit it. **Correction:** prevent overlapping live bands globally and do
   not reuse ambiguous bands within the bounded Map failure episode. Permanent
   global quarantine would exhaust the u8 namespace and break continuous image
   pages; indefinite reuse needs a proven drain horizon, stronger correlation,
   a proven session reset, or an explicitly accepted residual ambiguity.
5. **"Only a sent Cmd3 fragment creates a FAST tail."** False once a FAST
   connection-profile request has been submitted. **Correction:** finish local
   preflight before FAST, then set `restoreNeeded` at FAST submission and
   schedule the ECO tail on every subsequent exit, even if no Cmd3 payload is
   written.
6. **"Fix the JPG magic bases."** False: four fixed one-child CREATEs still
   cannot make a 2x2 page. **Correction:** one multi-child CREATE followed by
   four no-CREATE pushes.
7. **"Copy the working BMP full-screen code."** Unsafe: it ignores transport
   booleans, has unbalanced lifetime exits, and conflates cleanup authority with
   navigation authority. **Correction:** extract a shared transaction core;
   build the sequence-bearing CREATE only after intervening Shutdown; publish
   a surface token only after image-specific success; retain a quarantined
   compensating-Shutdown path for ambiguous CREATE; and restore the Files
   callback from the caller/navigation token after destructive teardown.
8. **"The JPG file-size cap bounds memory."** False: decoded dimensions govern
   RGB memory, and the BMP loader can read beyond truncated or malformed input.
   **Correction:** enforce decoded-byte/PSRAM-reserve limits and validate signed
   dimensions, DIB/palette bounds, offsets, stride arithmetic, and total pixel
   extent before tiling either format.
9. **"The owner's six-second sleep is the latency bug."** False for signaled
   work. **Correction:** measure and bound residual current-lap latency.
10. **"Recovery blocks the owner for 3.5 seconds."** Stale in current source.
   **Correction:** recovery is off-owner; remove the stale comment and focus on
   GATT, R1, SD, UART, and refillable drains.
11. **"The 1.5-second send mutexes bound GATT."** False: the characteristic
   overload has two additional 30-second waits. **Correction:** explicit
   owner-safe timeout/worker isolation plus late-event tests.
12. **"Put policy first in the owner loop and the problem is solved."** Only
    normal ordering improves; a wake during blocking work still waits.
    **Correction:** use a dedicated narrow task or remove every blocker and
    prove a maximum.
13. **"A dedicated task makes connection updates simple."** Not by itself. The
    GAP request is queued asynchronously; its completion can race the caller's
    next state publication, and the event does not echo an application
    generation or serial. **Correction:** publish request state before the API
    call without holding a callback-shared mutex; use a non-droppable completion
    latch; treat a single global in-flight update as a conservative experiment
    policy, not a Bluedroid invariant; and treat an ambiguous same-BDA event as
    observation, not authenticated completion of the current request.
14. **"An application timeout safely reopens the connection-update slot."**
    False: a late same-peer completion can then be credited to a newer request.
    **Correction:** quarantine the timed-out slot. After same-BDA reuse, trust
    neither a later callback nor its observed tuple without a proven
    link-lifecycle/event-drain barrier; otherwise defer until teardown or
    another event that unambiguously ends the window.
15. **"Shortening `writeValue` timeout is harmless."** False: timeout leaves
    the characteristic gate closed until a late event/disconnect to prevent
    misattribution. **Correction:** test late completion and recovery before
    changing the bound.
16. **"Moving owner blockers means timing them out in place."** Unsafe for
    transactional work. **Correction:** move the raw-SD data write and its tail
    commit together so retry cannot duplicate a record; for R1 forwarding,
    snapshot immutable data plus generation, release the telemetry lock, send,
    then revalidate without changing the established cache identity.
17. **"Reconnect automatically reapplies the desired radio arm."** Not in the
    current ordering: `g2ConnPriReapply()` runs before the new temple is marked
    connected. **Correction:** reconcile after connected/generation state is
    published, with a test proving the newly connected arm receives the current
    effective profile.

## Final decision

- Treat Map retry bounding as required before delayed ECO can be considered
  effective.
- Repair full-screen JPG through a shared, validated four-child transaction;
  do not ship a magic-only patch.
- Retract the old recovery-latency explanation.
- Treat `g2_ctrl_owner` as currently unsuitable for a hard-SLA sole radio
  controller. Instrument it and harden it, while using a narrow dedicated
  policy task for the first ECO experiment.
- Keep all three changes as separate patches with a combined G2+R1 stress test
  before battery A/B measurement.
