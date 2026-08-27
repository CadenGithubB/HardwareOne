# G2 BLE reconnect wedge remediation report

**Date:** 2026-08-14

**Scope:** G2 left/right temple reconnect, shared R1 Ring central operations, BLE role lifecycle, and Bluedroid host recovery

**Source basis:** current workspace at Git `d3ad5354` plus the supplied field log

**Status:** prevention and safe-recovery changes implemented in the current
workspace; XIAO S3 integration build passes; hardware fault-injection/replay is
still required before release

## Implementation outcome

The delivered change keeps the existing `g2_ble_connect` task and makes it the
single owner of G2/R1 scan and create-connection work. It does **not** add a new
task or task stack. Missing-temple recovery and `g2recover` now enqueue a
topology-aware job instead of blocking `g2_ctrl_owner`; scheduler, manual, and
Ring producers coalesce through synchronized admission and immutable request
generations; saved reconnect completion is committed only if the exact user,
owner, target, and policy incarnation is still current. Manual name/explicit-MAC
connects that may learn a target carry a target-free intent/identity I/X fence,
so target publication and any complete-topology logical completion share one conditioned transaction
rather than a late write after the user or owner has moved on.

The singleton scan now uses persistent callbacks plus owner/epoch and
callback-in-flight fences. Manual Ring scans use the same worker, and G2 repair
can stop a lower-priority Ring scan at a safe scan boundary. The Ring owner has
an explicit teardown barrier, while the boot-lifetime lens/tap workers use
in-band queue-generation barriers so host teardown cannot race a job that was
dequeued just before its `active` bit became visible.
For targeted G2 LEFT/RIGHT scans, the callback also refuses an advert classified
for the opposite side before it can publish into that temple object.

BLE role decisions now distinguish controller liveness, Bluedroid-host state,
and phone-server ownership. Role changes use an exclusive transition token.
The vendored Arduino BLE layer reports exact connect/unregister outcomes,
bounds characteristic writes, waits for GATTC unregister acknowledgement, and
provides a checked, phase-ordered host/controller teardown. An incomplete
teardown fails closed into a reboot-required lifecycle fault instead of
blindly reinitializing or deleting a callback target.

Reconnect configuration and owner authority are synchronized separately:
runtime targets are fixed-size snapshots, rejected/coalesced work no longer
consumes a launched attempt, explicit disconnect invalidates old work, and a
late success cannot clear that suppression. Manual learning validates its I/X
fence while atomically updating the guarded settings mirror, fixed runtime
target, identity generation, and topology completion; persistence follows
outside the family completion mutex. Automatic reconnect never creates pairing
ownership as a side effect of saving a MAC.

Terminal completion is now serialized per peer family. G2 has a recursive
completion/event mutex, a published-temple mask, and a cancel epoch advanced
and snapshotted atomically under its admission mux; Ring has the equivalent
completion mutex, UP-event marker, and mux-protected cancel generation. Logical
disconnect advances cancellation and publishes DOWN
before slower physical cleanup. A worker may publish UP only while holding the
same family mutex and after rechecking its peer request and cancel generation.
Manual wrappers also hold that family mutex across the family claim/cancel
snapshot and peer manual-request/I-X generation, so a disconnect cannot splice
those two admission domains.
Those mutexes are deliberately not held across scan, OPEN, discovery,
subscription, descriptor writes, or disconnect calls.

Manual saved reconnect no longer performs a separate “clear suppression, then
read target” sequence. `blePeerBeginManualConnectRequest()` advances manual
intent and snapshots the exact target, owner, and identity in one scheduler
transaction while preserving any independent automatic retry episode. Finite
G2 missing-temple repair uses an atomic conditioned commit that can publish a
newly learned missing MAC and link-up together. Enabling auto-reconnect binds
owner validation to configuration publication with the expected owner
generation, closing the revoke/replacement window. Name-based G2/Ring connect
and direct Ring-MAC connect use `blePeerBeginManualLearn()` instead: the returned
intent/identity pair does not require an existing target, and
`blePeerCommitLearnedTargetIfCurrent()` applies only the authorized address
slots if that exact fence and owner remain current. For a partial first-time G2
pair, persistent retry remains armed rather than consuming completion or
combining a new temple with an unauthorized stale counterpart. A manual one-eye
learn that keeps the other eye connected pins that survivor's connection
generation and admitted MAC/address type; completion revalidates all three under
the G2 completion mutex before the learned target may commit.

GATT setup is now positive-evidence based. G2 command/audio and Ring
notification setup inspect registration API/event status and the CCCD `0x2902`
descriptor-write API/event status; timeout, disconnect, late/superseded
generation, missing CCCD, or any negative status rejects the new link. LEFT mic
availability is not published until audio subscription succeeds. Descriptor
writes have their own bounded structured result rather than being assumed
successful because the API call returned.

The vendored client layer now tracks every factory-created `BLEClient` in an
all-client registry and synchronizes the route map, aliases, deletion claims,
and callback references. `deleteClient()` refuses routed or callback-referenced
objects instead of freeing a live callback target. HardwareOne provides exactly
three fixed retirement lanes—G2 left, G2 right, and R1. Unsafe deletion transfers
the pointer into its empty lane or restores it to the application owner on a
lane collision; both outcomes block replacement. Checked host teardown blocks
creation, drains callbacks, and deletes the complete registry only after
Bluedroid reaches a terminal state.
An APP_REGISTER timeout keeps routing alive: late success is cleanup-only and
requests UNREG; late failure removes the route. The route remains until the
corresponding unregister acknowledgement. If that acknowledgement is lost and
`deleteClient()` cannot prove retirement, the fixed lane and all-client registry
retain ownership, the affected peer bit is recorded, and every normal central
admission fails closed. Recovery retries deletion and gives asynchronous UNREG
3 seconds to finish before recycling the whole central role, even if a sibling
G2/Ring link survived—the one narrow exception to ordinary live-link
preservation. Terminal host teardown is then the lifetime acknowledgement. The
survivor may be dropped. Before teardown, recovery snapshots the union of the
affected retirement peers and every G2/Ring peer physically live at that
boundary. After verified restoration it requests a one-shot reseek for that
union even when persistent auto-reconnect is off; explicit user-down, current
owner, and current saved-target gates remain authoritative. Recovery restores
the actual G2-client ownership it quiesced rather than persisted `bleMode`,
because boot can coerce client ownership without rewriting that preference. A
retirement-only normalization stays down when neither a G2 runtime nor a live
central peer existed before teardown; master-disable also wins. The phone-server
role is never recycled by this path.

RAM impact is limited to small fixed mutex/control blocks, generation counters,
event markers, three retired-client pointer lanes with timestamps/flags, bounded
result records, callback references, and expanded queue-job metadata. The
all-client set has one dynamic node per already-created client and is reclaimed
on verified teardown. Existing queues/tasks remain in place, so
there is no additional task-stack reservation. The remaining release gate is
hardware validation of the out-of-range/cancel fault sequence and forced
teardown failures. Ring audio deferral can still occupy the shared worker for
its existing bounded window; it no longer creates concurrent BLE operations,
but moving that policy fully outside the worker remains a latency follow-up.

### Adversarial review outcome

The final implementation was challenged against the failure orders most likely
to invalidate a happy-path fix: user disconnect between successful OPEN and
completion; owner/target replacement during admission; manual learned-target
commit racing disconnect or a replacement target; partial G2 learning that
could splice a new side with an old side; an opposite-side advert arriving in a
targeted scan; replacement or reconnect of the preserved eye before one-eye
completion; manual connect racing an automatic episode; cancel during scan,
discovery, notify registration, and CCCD write;
API success followed by negative/missing events; late APP_REGISTER success and
failure; unregister reordering or loss; client deletion while routed or inside
a callback; a lost route with a surviving central peer; and host teardown with
a retained non-alias client; and a second replacement attempt while an older
client still occupied the same retirement lane. These checks drove the
per-family completion locks, mux-protected cancel generations, learned-target
I/X commit, checked subscription results, callback references, three fixed
retirement lanes with collision backpressure, fail-closed central admission,
affected-plus-previously-live restore masking, actual-runtime-role restoration,
and the all-client registry. Static reasoning and
builds cover code shape; physical controller timing and fault injection remain
required before release.

## Pre-remediation diagnosis and executive decision

The out-of-range disconnect was normal. The failure to recover was not.

The firmware did schedule reconnects. The pre-remediation source had two independent G2 recovery paths with no common admission boundary:

1. the generic `BLE_Peers` saved-MAC reconnect, running on the shared BLE-connect worker; and
2. the G2 half-connected recovery, running synchronously on the G2 control/heartbeat task.

Both paths mutate the same singleton `BLEScan`, scan callback, target/filter globals, temple advert pointers, and process-global Bluedroid create-connection admission state. The log timing and different-address cancellation strongly support this race being exercised, but the capture has no job IDs and cannot uniquely attribute each request to one caller. The direct wedge mechanism is narrower and certain: after a timed-out request, Bluedroid remained in `BLE_CONN_CANCEL`, so later LE create requests could not start. The intended self-heal then failed to run because its “phone server owns BLE” guard calls `isBLERunning()`, which deliberately returns true whenever the controller is enabled—including normal G2 client mode.

The remediation decision separated prevention from destructive recovery:

1. **Prevent the collision:** make one coordinator the sole owner of central scans, LE create requests, and connect-attempt cleanup; turn missing-arm and manual recovery into asynchronous, topology-aware jobs on that coordinator. The final patch serializes Ring readiness in the same worker with its existing bound; moving that wait outside the worker remains a latency follow-up, not a safety dependency.
2. **Make recovery safe:** add explicit BLE owner predicates and a role-lifecycle transaction, then perform a verified full host recycle only when the G2 client owns the stack, the worker/owners are quiescent, the phone-server role is absent, and all admissions are closed. Ordinary cancel-wedge recovery still waits for all central links to be down. A lost APP_REGISTER/UNREG route is the implemented lifetime-safety exception: it occupies one of three fixed retirement lanes, closes admission, retries safe deletion for 3 seconds, and then forces checked whole-central-role recycle even with a survivor. The survivor can be dropped, so verified restoration requests one-shot reseek for both the route-affected peer and every peer live before reset, subject to current user-down/owner/target gates. An independent admin force-recovery remains a possible follow-up rather than part of this patch.

Changing only the dead recycle predicate is unsafe. It would expose an R1 client-pointer invalidation bug and a role-transition race. Adding only a mutex is also insufficient because it would leave the G2 heartbeat owner blocked for up to 35 seconds and would not give teardown a reliable cancellation boundary.

## Pre-remediation findings and confidence

### Field evidence

The capture begins after the first right-temple loss, so the original right-side HCI reason is not present. The later sequence is unambiguous:

| Time | Evidence | Meaning |
|---|---|---|
| 18:27:09 | Right advert found at about -68 dBm while another recovery attempt was still in progress | The temple had returned to range and was advertising. |
| 18:27:18 | `rsn=0x100` after a 35-second attempt | Local GATT/L2CAP connection cancellation, not pairing/authentication failure. |
| 18:27:20 | `cannot start new connection at conn st: 3` | ESP-IDF state 3 is `BLE_CONN_CANCEL`. |
| 18:30:07 | `BLE-Peers ... attempt=5` | Automatic reconnect was enabled and still running. |
| 18:30:11 | Left link drops with `rsn=0x08`; both temples are immediately seen at usable RSSI | `0x08` is a real connection/supervision timeout, consistent with range loss; visibility was no longer the problem. |
| 18:30:46 | `different BDA Connecting: <right> Cancel: <left>` | The host still believed the right create request was pending while left-side cleanup attempted to cancel it. |
| 18:32:17 | Ring gets a 1 ms “stale link record” refusal; wedge streak reaches 2 | Admission of new LE creates was failing process-wide, not just against one RF peer; the left link had continued working earlier in the episode. |

The absence of a subsequent `Host stack wedged ... recycling` message matches the impossible recycle predicate in the pre-remediation source. `0x100` by itself is not wedge proof—it is expected when a normal local timeout is cancelled. The wedge evidence is the subsequent persistent state 3, different-address cancellation, and immediate refusal against another peer.

### Primary causes

#### 1. Two G2 connection owners

An unexpected temple disconnect calls `blePeerNoteLinkLost()` whenever both sides are not up (`G2_Glasses.cpp:1799-1860`). The generic main-loop scheduler later calls `g2ConnectSaved()`, which enqueues a `G2_SAVED` job on the shared worker (`BLE_Peers.cpp:607-657`, `G2_Glasses.cpp:12520-12577`).

Independently, `recoveryHeartbeatTick()` executes `attemptMissingArmRecovery()` directly (`G2_Glasses.cpp:9482-9678`). That function performs its own scan and calls `connectTemple()` without going through the worker or setting `gConnectTaskActive`. The manual `g2recover` command calls the same synchronous function from command context (`G2_Glasses.cpp:20234-20251`).

The result is a confirmed concurrency exposure: multiple tasks can believe they exclusively own one controller. The field timing strongly supports overlap, but instrumentation is required to prove the exact caller of each request in a future capture.

#### 2. Shared singleton scan state

Arduino `BLEDevice::getScan()` returns a singleton, and `BLEScan::setAdvertisedDeviceCallbacks()` simply replaces its callback pointer. Full G2 reconnect, half recovery, Ring connect, and manual `ringscan` can all replace that callback or mutate shared scan globals.

Pre-remediation G2 scans also allocated `new G2ScanCallbacks()` on every attempt. The scan object did not delete the prior callback, so retries leaked callback objects. A late result could also be interpreted using the next request's target/filter globals.

#### 3. Timeout cleanup is asynchronous at the host/controller boundary

The local Arduino `BLEClient::connect()` patch bounds the OPEN wait. On timeout it unregisters the GATT application and returns, but it does not wait for LE create-connection cancellation to complete. Bluedroid permits one outstanding LE create request globally. `L2CA_CancelBleConnectReq()` changes the state to `BLE_CONN_CANCEL`. A successful HCI cancel-complete is the normal cancellation route back to idle; failed LE connection-complete paths can also clear the state. In this capture neither clearing path arrived or took effect before later attempts.

This makes a prompt second request—especially for another address—dangerous. A fixed delay may reduce exposure but cannot prove the state recovered. Directly forcing Bluedroid's private state to idle would be worse because it could allow two controller requests to exist at once.

#### 4. Self-heal is unreachable in G2 client mode

`bleStackRecycleIfWedged()` uses `if (isBLERunning()) return false` as its “server mode off” guard (`G2_Glasses.cpp:12141-12175`). `isBLERunning()` is intentionally controller-wide and returns true whenever the BT controller is enabled (`Bluetooth.cpp:1783-1797`). A functioning G2 client necessarily has the controller enabled, so the recycle path cannot run in the state it was designed to recover.

### Important contributing defects

- The half-recovery comment budgets about three seconds, but `connectTemple()` can block the control/heartbeat owner for 35 seconds plus discovery. That can make the surviving temple unhealthy and prevents timely control-worker shutdown.
- `bleAutoReconnectTick()` increments attempts and advances backoff even when `connectSaved()` rejects the request as busy or queue-full.
- `RING_SAVED` waits up to 20 seconds for both glasses, then settles for 3 seconds, **while occupying the only connect worker**. A Ring job ahead of a G2 job can block the operation that would satisfy its own wait condition.
- The Ring watchdog clears its in-flight flag after 240 seconds without cancelling or retiring the old job. That permits duplicate work to queue behind a stuck job.
- Cross-task admission uses several `volatile` check-then-set booleans. `volatile` does not make those claims atomic.
- The recycle cooldown is stamped before `deinitG2Client()`. A harmless preflight/teardown deferral can therefore suppress another recovery attempt for ten minutes.
- Fixing the recycle gate alone would call `BLEDevice::deinit(false)` after `g2RingDisconnect()`, which deliberately retains the Ring client pointer. Ring pointers must be invalidated before host object deletion, but the current raw `g2RingInvalidateLink()` is not by itself a teardown barrier against the persistent Ring owner; it must be strengthened as described below.
- Runtime status conflates controller liveness with phone-server ownership. This affects the recycle guard, G2 init, JSON `server` field, the network page, and the OLED quick toggle.
- The current timing-only wedge detector can miss the G2-only failure (35-second timeouts do not raise its streak) and can misclassify unrelated fast failures from app registration or `esp_ble_gattc_open()`.

## Required invariants used for implementation review

The implementation was reviewed against these invariants; hardware acceptance still requires the tests later in this report:

1. At most one LE create-connection operation is outstanding process-wide.
2. Every G2 and Ring scan, LE create request, and connect-attempt cleanup enters one central-operation coordinator. Established-link disconnect remains an out-of-band lifecycle operation, but it must be generation-fenced and excluded from a concurrent create.
3. The G2 control/heartbeat task never waits for scanning, connecting, discovery, or cancellation.
4. A queued job re-evaluates current link topology before acting; enqueue-time topology is not trusted.
5. Late scan and GATT callbacks cannot mutate a newer job or a reinitialized client generation.
6. Server, G2-client, and host-recovery role transitions are mutually exclusive from start through rollback/commit.
7. Automatic host recycle never destroys a live phone-server link. It preserves live temple/Ring links for an ordinary admission wedge; a positively identified unretired native client route is the narrow exception and may reset the whole central role, after which the affected peer and every previously-live central peer receive gated one-shot reseek.
8. A rejected/coalesced request does not consume a reconnect attempt or one-shot reseek.
9. Explicit user disconnect remains authoritative and is not undone by a callback or recycle.
10. A failed recycle leaves one truthful terminal state and cannot thrash init every 180 seconds.

### Recovery policy decision

Automatic recovery is **non-destructive by default**. For the ordinary cancel/admission-wedge signature, a live temple, Ring, or phone link is preserved and recovery reports/defer rather than recycling underneath it. If such a central link persists indefinitely, the terminal automatic state is `degraded: recycle blocked by live link`.

The implemented exception is an APP_REGISTER/UNREG route that cannot be retired. The matching fixed retirement lane blocks all central admission; the recovery tick first retries deletion and allows a 3-second asynchronous-UNREG grace period. If the lane remains occupied, checked recovery resets the whole client role even with a temple or Ring linked. This can drop that survivor. It still never tears down the phone-server role. Recovery captures the route-affected mask plus the physically live G2/Ring mask before teardown and requests one-shot reseek for their union after restoration; it does not requeue peers that were neither affected nor live, and the current user-down/owner/target gates can reject any captured peer.

Provide a separate authenticated/admin force-recovery action that explicitly states it will drop every BLE link. That action still uses the same role transaction and teardown barriers; it only relaxes the live-link policy after informed user intent.

## Implemented design basis and remaining follow-ups

The following section records the design used to drive the patch. The live code
implements the single shared worker, topology-aware repair, conditioned peer
transactions, mux-protected cancel generations, scan/callback fences, checked client teardown, and verified role
recovery. Two items remain deliberately bounded follow-ups rather than claims of
completion here: Ring audio eligibility can still hold the shared worker for its
existing timeout, and richer public job-ID/event telemetry is not a prerequisite
for the safety fix.

### 1. Extend the existing worker into the sole central-operation coordinator

Do not create a second task. The existing `g2_ble_connect` worker is the right serialization point, but its admission and job model need to become authoritative.

Add immutable job metadata similar to:

```cpp
enum class BleCentralJobKind : uint8_t {
  G2_EYE_CONNECT,       // openg2 left/right/auto, including authorized pairing
  G2_REPAIR_SAVED,
  G2_FULL_SAVED,
  RING_SCAN_CONNECT,
  RING_SAVED_CONNECT,
  RING_MAC_CONNECT,
  RING_SCAN_ONLY,
};

enum class BleJobSource : uint8_t {
  BOOT, LINK_LOSS, HALF_HEAL, MANUAL, HEALTH_RESEEK, RING_RESEEK
};

struct BleCentralJob {
  uint32_t id;
  uint32_t requestEpoch;
  uint32_t userIntentGeneration;
  uint32_t peerIdentityGeneration;
  BleCentralJobKind kind;
  BleJobSource source;
  uint8_t permittedTopology;       // missing-only or full-allowed
  uint8_t eye;                     // left/right/auto for G2_EYE_CONNECT
  uint16_t scanDurationSec;        // includes ringscan's current 1..300 s API
  uint32_t expectedLiveGeneration; // validation, not authority
  char mac1[18];
  char mac2[18];
  uint8_t addressType1;
  uint8_t addressType2;
  uint8_t pairingAuthority;        // reconnect-only or authenticated pairing
};
```

Protect admission with a mutex or critical section and track `queued`, `running`, `cancelling`, and `completed` by job ID. Replace per-family check-then-set booleans as the source of truth. Duplicate G2 repair requests should coalesce; a full reconnect intent may supersede a queued missing-only intent. Completion must clear state only if the job ID/epoch still matches.

Resolve saved targets at dispatch under `peerIdentityGeneration`, or abort/re-plan if the queued identity changed. Validate `userIntentGeneration` before scan, before create, and before completion so unpair, MAC replacement, ownership change, explicit disconnect, master-disable, or role change invalidates old work. Public/random address type is part of peer identity; a MAC string alone is insufficient.

Worker shutdown needs a reserved out-of-band control notification, not an ordinary queue item that can be blocked by a full queue. A cancellation epoch must be checked at scan, OPEN, discovery, subscribe/setup, and post-connect boundaries. Bound every stage, including currently unbounded remote-characteristic event waits. Teardown waits for a positive worker-idle acknowledgement; if the worker cannot quiesce, enter `FAULT`/reboot-required and never force-delete it or deinitialize the host beneath it.

### 2. Make G2 repair topology-aware at dispatch time

Both `recoveryHeartbeatTick()` and `g2recover` should only submit intent. They must return immediately and never call scan/connect code.

Publish all `gL/gR` link transitions through a small `G2LinkSnapshot` API. BLE callbacks and the worker must update connected flags, generation, and pointer-ownership state under the same short writer-side lock; job readers copy a snapshot and release the lock before any BLE call or wait.

When a G2 repair job starts, take that consistent snapshot and choose:

| Current topology | Missing-only request | Full-allowed request |
|---|---|---|
| L up, R up | Complete as no-op | Complete as no-op |
| L up, R down | Scan/connect R only | Scan/connect R only |
| L down, R up | Scan/connect L only | Scan/connect L only |
| L down, R down | Terminate the finite half-heal intent; do not reconnect implicitly | Scan/connect both saved sides |

Before scan, before connect, and after connect, validate the request epoch, user-intent/peer-identity generations, and the generation of the side that was expected to remain live. If topology changed, stop and re-plan; do not continue with stale advert pointers. Track links created by the job and close only those links if cancellation becomes stale after a connect succeeds. If both sides fall, only a still-valid persistent auto-reconnect intent may request a full reconnect.

Replace `g2ConnectSavedSync()`'s topology-unaware behavior with this selection. It already selects one eye when only one MAC is saved, but when both MACs exist it chooses AUTO even if one temple is already linked. A connected temple normally is not advertising, so scanning for it only lengthens the operation and increases scan/cancel exposure.

### 3. Centralize successful-topology finalization

Create one `g2FinalizeConnectedTopology()` path used by initial connect, saved reconnect, and missing-arm repair. Make transition semantics explicit:

- 0→1 starts aggregate session time and emits the legacy connected event;
- 1→2 preserves session start, emits a versioned topology-repaired detail, and marks the peer fully linked;
- 2→1 preserves the surviving session while reporting the dropped side;
- 1→0 ends the session and stops heartbeat ownership;
- start/maintain heartbeat ownership;
- publish existing status and typed events only at their defined transition;
- persist a newly learned MAC only when the job carries authenticated pairing authority or a still-valid owner/session identity—not merely because a name matched;
- call `blePeerNoteLinkUp()` only when both sides are actually up;
- leave per-temple prelude, MTU, connection generation, plugin/container, notification, microphone, and connection-priority setup consistent with `connectTemple()`.

Today the half-repair success path omits some common bookkeeping, while a generic partial reconnect can reset aggregate session time. The common function must also reject stale completion after user disconnect rather than letting `blePeerNoteLinkUp()` clear that intent.

Split pairing from reconnect authority. An authenticated `g2Pair`/`openg2` path may stamp owner and learn a name-resolved MAC; boot/scheduler `g2ReconnectSaved` must never create or replace ownership. A half-repair without a saved MAC may learn it only by inheriting the still-valid authorized session that created the partial connection.

### 4. Give the singleton scan an owner and epoch

Use one persistent callback object instead of allocating one per scan. Store an immutable, fixed-size scan request containing owner, epoch, target side, explicit and fallback saved MACs, peer-identity generation, address types, and whether opportunistic Ring discovery is permitted. Avoid cross-core `String` mutation in the callback.

The callback must reject a result unless owner, epoch, and peer generation match the active scan. Stopping a scan should invalidate the epoch before a later request installs its context. Epoch-protect the current side channel that stashes Ring advert/name/address during a G2 scan. Track callbacks in flight so role teardown does not free scan/temple state while a callback can still publish.

Route manual `ringscan` through the coordinator as `RING_SCAN_ONLY`, or place every scan entry point behind the same global scan admission object. Preserve its 1–300 second duration. A 300-second scan cannot monopolize the only worker: run long scans in short non-blocking slices and allow a higher-priority link-loss request or `g2StopScan()` to invalidate the scan epoch and stop it. Priority should be lifecycle cancel/teardown, G2 missing-side repair, G2 full reconnect, Ring maintenance, then manual discovery. An active create/discovery operation is not preempted unsafely; latency guarantees apply only once the coordinator is idle or at a cooperative boundary.

### 5. Remaining latency follow-up: remove Ring head-of-line waiting from the worker

The Ring's “wait up to 20 seconds for both glasses, then settle 3 seconds” policy can remain, but the wait must not hold the central worker.

Implement it as scheduler eligibility:

- if G2 is not ready and the Ring readiness deadline has not expired, retain the Ring intent outside the worker without polling/requeue storms;
- when G2 becomes ready, start the 3-second settle deadline outside the worker;
- once ready or the 20-second bound expires, enqueue the Ring connect;
- prioritize an admitted G2 repair over Ring reconnect, while preserving bounded Ring progress.

Replace the 240-second Ring flag reset with an explicit overdue job state. An overdue running job should raise host suspicion or request coordinated recovery; it must not clear ownership and enqueue a duplicate.

`bleBootReconnect()` must also honor the admission result. It currently clears scheduler state, ignores `connectSaved()`'s return, and marks the peer kicked. A Ring boot intent deferred for G2 readiness must remain owned by the coordinator or be preserved for retry; it cannot disappear merely because it was not immediately eligible.

### 6. Make reconnect scheduling truthful and synchronized

Use an explicit result in the first coordinator patch; boolean is not expressive enough:

```cpp
switch (p->ops->connectSaved()) {
  case STARTED:
    incrementLaunchedAttemptAndApplyBackoff();
    break;
  case COALESCED:
    attachIntentToExistingJobCompletion();
    break;
  case BUSY:
    preserveIntentAndRetryAdmissionAtBoundedDelay();
    break;
  case ALREADY_UP:
    completeIntentWithoutCountingAttempt();
    break;
  case NO_TARGET:
    completeWithConfigurationError();
    break;
  case ROLE_BLOCKED:
    preserveOrCancelAccordingToCurrentUserIntentGeneration();
    break;
}
```

Do not consume a non-auto one-shot reseek until a job is `STARTED` or its intent is attached to a `COALESCED` job. Do not retry a coalesced request on a short timer; its completion owns the next decision.

Protect the `BLE_Peers` reconnect arrays. Link callbacks, command handlers, and the main loop currently read/write them across tasks. Use a small mux and an intent generation: snapshot a due action under the lock, call the peer outside the lock, then commit the result only if the generation is unchanged.

Preserve current policy in the first patch:

- an existing partial session receives the finite seven-step half-heal behavior even when persistent auto-reconnect is off;
- persistent auto-reconnect intent survives indefinitely when enabled, but new create attempts pause after a qualifying host-admission fault until recovery becomes eligible or the user acts;
- both-down recovery with auto-reconnect off remains a user action;
- explicit disconnect suppresses both policies.

The two policy producers may initially remain, provided their intents coalesce into one coordinator. A later cleanup can make `BLE_Peers` the sole retry FSM with explicit `DOWN/PARTIAL/UP` peer state.

## Safe host-stack recovery

### 1. Replace ambiguous state predicates

Do not change `isBLERunning()` in place; callers already depend on its controller-wide meaning. Add explicit APIs:

```cpp
bool isBleControllerEnabled();
bool isBluedroidHostEnabled();
bool isBleServerInitialized();
BleStackOwner bleStackOwner();  // NONE, SERVER_*, G2_*, RECOVERING, FAULT
```

Use `isBleServerInitialized()`—not controller liveness—in `initG2Client()` and the recycle permission check. Define aggregate subsystem status as server initialized or G2 client initialized, rather than bare-controller state.

Audit and correct the current consumers:

- `G2_Glasses.cpp`: server teardown before client init; recycle permission;
- `System_Utils.cpp`: JSON `server` field;
- `G2_Page_Network.cpp`: `serverUp`;
- `OLED_SettingsEditor.cpp`: make the quick toggle mode-aware instead of issuing server-only `closeble` in client mode;
- `Bluetooth.cpp`: aggregate state routing and settings status callbacks;
- `System_RamFlush.cpp`: use the new explicit server predicate in place of its current hand-coded equivalent.

Use actual runtime owner state for transition permission. Persisted `gSettings.bleMode` is preference, not proof of ownership: boot can currently coerce an effective client role without updating that setting.

### 2. Add one role-lifecycle transaction

Server init/deinit, G2 init/deinit, and host recycle must share an exclusive task-level **transition token** and published owner state. The existing server session-table lifecycle mutex is not suitable to hold across Bluedroid teardown because callbacks can need it. Publish `RECOVERING` and the lifecycle epoch under a short state lock; BLE/GAP/GATT callbacks atomically observe that state and drop/defer rather than blocking on the transition token while teardown waits for callback/host completion.

During `RECOVERING`, reject or defer new work, but immediately record changes to desired state:

- new G2/Ring jobs and scans;
- G2 initialization;
- phone-server initialization;
- role toggles, master-disable, unpair, and user-disconnect intent;
- reconnect tick admission.

Keep the exclusive transition token held from preflight through rollback or committed reinitialization. Public entry points try-lock/reject or record desired intent; internal `_Locked`/token-taking helpers perform init/deinit without recursively acquiring the token or reopening admission. If the user disables Bluetooth or changes role during recovery, advance the intent generation and finish in the newly requested powered-down/other-role state rather than silently re-enabling G2.

Document and assert the lock hierarchy. A suitable task-lock order is role transition → coordinator admission → session/control teardown → `bleCentralTx` → temple/Ring write mutex. Spinlocks are used only to snapshot/publish state, are never held across waits or BLE calls, and are never acquired after a blocking task lock in the reverse order. Callbacks must not acquire the role transition token.

### 3. Recycle sequence

The automatic recovery transaction should be:

1. Try to acquire the role transaction without blocking the main loop indefinitely.
2. Publish `RECOVERING`, close every central/init/session admission, and pause reconnect dispatch while preserving its intent.
3. Revalidate non-worker predicates: qualifying suspicion or an occupied fixed retirement lane is present; server not initialized; Bluetooth master enabled; destructive cooldown clear. Retry `deleteClient()` for occupied lanes and allow 3 seconds from retirement transfer for normal DISCONNECT→UNREG completion. For ordinary suspicion, require an initialized G2 client and no phone, L, R, or Ring link. For a still-occupied lane, keep the phone-server prohibition but permit a surviving G2/Ring link—or an already-deinitialized G2 runtime with residual host state—so terminal host teardown cannot be blocked forever. Snapshot current scan/job/callback activity without assuming it is already idle.
4. Invalidate the scan/request epoch, signal the out-of-band cancellation path, and wait for positive scan-stopped and coordinator-idle acknowledgements. If any bounded OPEN/discovery/setup stage or unbounded library wait cannot quiesce, publish `FAULT: worker-not-quiescent` and require reboot; never continue to host deinit.
5. Revalidate that the coordinator queue and worker are idle, no scan owns the singleton, and measured callback-in-flight counts are zero.
6. Close G2 control/session/UI producer admission. Drain their queues and wait for active-job acknowledgement, or enforce the new lifecycle epoch on **every** persistent lens/tap job kind before freeing runtime. Advancing only presentation/page-swap state is insufficient for currently unguarded custom/native-notification work.
7. Quiesce the boot-lifetime Ring owner through a new teardown barrier: mark the transport offline and close producer admission, wake and acknowledge the owner/RX paths, then acquire `bleCentralTx` followed by `gRing.writeMutex` before invalidating GATT pointers. A false `g2RingIsConnected()` boolean is not an ownership barrier.
8. Retire every temple/Ring BLE client with explicit GATTC UNREG acknowledgement, routing-map removal, callback-refcount drain, static `BLEDevice::m_pClient` alias retirement, and fixed application-slot ownership. The final wrapper keeps `gattc_if` and the route until an actual UNREG event; if that event was lost, leave the object both in the all-client registry and its exact fixed retirement lane for terminal host teardown rather than freeing or reusing it. A collision retains the newer pointer in its application owner and blocks replacement instead of creating a fourth lane.
9. Disconnect and retire remaining app state using internal G2/Ring deinit helpers that keep admission closed. Invoke the strengthened Ring invalidation under the exclusion above; plain `g2RingDisconnect()` deliberately retains a client pointer.
10. Clear custom GAP/GATTC/GATTS handlers, then stamp the destructive-attempt timestamp immediately before the first host mutation.
11. Deinitialize the host/controller in an ordering that keeps client objects alive until host callbacks are drained. Patch or wrap `BLEDevice::deinit(false)` as needed; its current implementation deletes the singleton scan/server/client before disabling Bluedroid and ignores all return codes. Do not substitute `btStop()/btStart()` because controller-only cycling does not clear the host TCB/cancel state.
12. Normalize and verify actual state: disable Bluedroid if enabled, deinitialize it if initialized, then disable/deinitialize the controller as its actual state requires. Proceed only when the host is uninitialized and controller idle. The Arduino initialized flag is not proof of this state.
13. Initialize a clean G2 client under the same transition token. Use internal transition helpers so recovery does not reject its own init. Require the Arduino initialized flag, Bluedroid enabled state, controller enabled state, callback handlers rebound, and a valid scan object before publishing `G2_READY`.
14. Re-check user power intent. Only after verified success, acknowledge/clear fixed retirement lanes, clear wedge suspicion, reopen the appropriate admissions, release the transition token, and allow preserved reconnect intent to run. Restore the actual G2-client ownership that admitted recovery, not persisted `bleMode`, because boot may have selected the client role without rewriting that setting. For lost-route recovery, one-shot reseek the union of route-affected peers and peers physically live before teardown, including live peers with persistent auto-reconnect off; user-down, owner, and target gates still apply. Leave retirement-only normalization down only when no G2 application runtime and no central peer existed before teardown (or when the user disabled BLE).

If preflight or G2 quiesce fails before host mutation, reopen admission, report `deferred`, and do not consume the ten-minute destructive cooldown. Once host mutation begins, rate-limit subsequent destructive attempts even if reinit fails. Track `lastAttempt`, `lastSuccess`, and the failed phase separately.

On failure after mutation, invalidate all BLE pointers, roll back partial client state, publish the exact phase and actual host/controller states, and suppress ordinary 180-second reconnect attempts from repeatedly calling initialization. Attempt phase-aware normalization to the clean host-uninitialized/controller-idle pair. If normalization fails, publish `FAULT: reboot-required`; an admin retry must not call normal init against an inconsistent real state. Never report “recycle complete” unless real host/controller state and G2 runtime initialization are verified.

Initially permit at most one automatic destructive recycle per boot, in addition to the ten-minute minimum gap. The current firmware already warns of DRAM loss on BLE stop/start cycles; a one-per-boot limit bounds damage until the heap soak proves repeated recycling safe.

### 4. Improve wedge classification

Duration alone should be a fallback signal, not the diagnosis.

Add an optional detailed connect result to the vendored `BLEClient` while retaining the existing bool API for unaffected callers. Capture:

- failure stage: app register, OPEN API call, OPEN event, OPEN timeout, discovery, subscribe/setup;
- numeric `esp_err_t` and `esp_gatt_status_t`;
- GATT interface, target, elapsed time, and whether the target was seen in the current scan epoch;
- unregister request/result and UNREG event latency.

Install a central GATTC event tap in client mode to observe numeric OPEN/UNREG outcomes. Replace the misleading `Unknown ESP_ERR` formatting of a GATT status with the numeric status and the correct GATT reason name. This tap cannot observe HCI create-cancel completion or Bluedroid's private `BLE_CONN_CANCEL` variable; diagnostic builds need an explicit vendored IDF/controller hook or btsnoop for those facts.

Raise host suspicion for a bounded combination such as:

- a timeout/cancel followed, after the bounded quiesce window, by a different peer's immediate local OPEN refusal;
- a different-address cancellation or an instrumented create state that remains non-idle past its deadline;
- repeated timeout-after-current-epoch-advertisement plus an explicit failure to reach the coordinator's cancellation acknowledgement boundary.

Do not classify `0x100` alone: it is expected after an ordinary local timeout cancellation. Exclude app-register failure, allocation failure, queue rejection, not-seen scan, and role rejection from the **ordinary create-admission wedge score**. APP_REGISTER/UNREG loss has its own stronger lifetime path: quarantine the exact client, close central admission, and record the affected peer for checked terminal teardown. A completed ordinary cancellation must be a negative test, and a successful link clears ordinary suspicion only when no unretired route remains. Add a short post-timeout quiesce window and explicit cancellation/UNREG acknowledgement before admitting another create request. If that acknowledgement cannot be obtained, stop admitting work; full recycle is allowed only after the worker and callbacks independently acknowledge quiescence.

## Downstream impact assessment

| Area | Intended effect | Risk introduced | Required mitigation/test |
|---|---|---|---|
| Surviving G2 temple | Heartbeats continue during repair; missing side is targeted only | Stale queued repair could act after topology changes | Link/request generations; dispatch-time topology; cancel-created-link cleanup |
| G2 mic/audio | Left loss still releases capture; reconnect restores availability | Common finalization could omit audio notify/prelude | Reuse `connectTemple()` setup and test left drop during active capture |
| Lens/hijack/EvenAI | Right loss still invalidates presentation and active exchange | Old queued page work could replay after recycle | Advance lifecycle/presentation epochs and drain/drop stale work |
| R1 Ring | No G2/Ring connect overlap; the already-admitted Ring job survives its bounded audio gate | The current 90-second audio wait still occupies the shared worker and delays later jobs | Verify timeout/pending state and priority behavior; moving eligibility outside the worker remains a latency follow-up |
| Ring client/owner | Host recycle cannot leave a dangling pointer or active writer | Raw invalidation can race the persistent owner and late callbacks | Acknowledged owner/RX teardown barrier; `bleCentralTx`→Ring-write lock order; explicit UNREG/routing-map drain |
| Manual learned targets | Name/explicit-MAC success updates identity and logical completion together | Late completion could overwrite a replacement target or splice new-L/old-R | Target-free I/X fence, explicit replace mask, side-filtered targeted scan publication, atomic target/topology commit, preserved-eye generation/MAC/type revalidation, partial-pair and cancel/owner-replacement tests |
| Lost native client route | Prevent allocation behind a live APP_REGISTER/UNREG route | Whole central-role recycle can drop an otherwise healthy G2/Ring survivor | Three fixed owner lanes with collision backpressure, 3-second safe-reap grace, checked terminal teardown, phone-server exclusion, affected-plus-previously-live gated reseek, collateral-link hardware test |
| Phone BLE server | Never destroyed by automatic client recovery, including lost-route recovery | Incorrect owner publication could authorize recycle | Server-active/connected negative tests; role transaction |
| CLI/Web/OLED | Honest queued/running/recycling state | `g2recover`/`ringscan` become asynchronous | Return “queued” plus job ID; expose completion/status instead of claiming success |
| Auto-reconnect settings | Existing finite/persistent/user-disconnect semantics remain | Busy jobs could skew backoff or boot role | Count only admitted jobs; test auto on/off, forced boot client, user disconnect |
| Security/ownership | Paired-owner identity and user disconnect remain authoritative | An automatic saved reconnect could create ownership or use a replaced MAC | Split authenticated pairing from saved reconnect; validate peer/intent generations; never stamp owner in scheduler/boot reconnect |
| Events/automations | Better forensic state | Reusing an old event with new semantics could break consumers | Add versioned started/succeeded/deferred/failed details; keep legacy link events |
| Memory | Callback leak removed; fewer redundant scans/clients | Job/result/fault metadata plus three retirement lanes consumes small fixed RAM; the all-client registry has one dynamic node per created client until verified retirement | Fixed-size job/callback context; lane/registry counters; heap/largest-block soak |
| Power/radio | Fewer full 12-second scans and less contention | Infinite auto-reconnect still consumes power | Preserve capped backoff and add per-attempt reason/scan duration metrics |
| Build variants | Same behavior when features enabled | New cross-module symbols can break disabled builds | Add `!ENABLE_BLUETOOTH` and `!ENABLE_G2_GLASSES` stubs; build flag matrix |

Two additional lifecycle issues should be corrected while touching this code:

1. `initG2Client()` needs one rollback path. It currently does not verify `BLEDevice::getInitialized()` and some early failures leave a partially initialized host/handler behind.
2. Define separate runtime-only and full-stack shutdown APIs. `g2deinit` frees G2 runtime but leaves the host/controller up, while server `closeble` can no-op when server state is absent. `bleenabled 0` should be able to prove the host/controller are actually down.

The broad UI/status cleanup, richer event schema, and full-shutdown API can land as separately gated follow-ups to the prevention patch. They become prerequisites only before automatic destructive recycle is enabled; scan/create serialization should not wait for unrelated presentation work.

## Telemetry and user-visible behavior

Every connect job should log or expose:

- job ID, kind, peer/side, source, request epoch;
- enqueue/start/end timestamps, queue depth, and result;
- scan owner/epoch/start/stop/late-result count;
- current topology at enqueue and dispatch;
- failure stage, numeric status, target address type, and elapsed time;
- cancellation requested, observable UNREG/routing-map acknowledgement timing, and—only in diagnostic builds—instrumented HCI cancel-complete timing;
- next retry due and whether a request was admitted, coalesced, or rejected.

Every recycle evaluation should expose its blocker or trigger (`server`, `live-link`, `unretired-route`, `retirement-grace`, `scan`, `job`, `callback`, `role`, `cooldown`) and record distinct `started`, `deferred`, `succeeded`, and `failed:<phase>` outcomes. Lost-route telemetry should include occupied fixed lanes, their age, the affected-peer mask, the previously-live mask, the resulting restore mask, and whether a central survivor will be dropped. The current event is posted before success is known; that should be corrected.

Suggested user behavior:

- `g2recover` → `G2 recovery queued (job 42, target will be re-evaluated)`;
- `g2status`/SSE → `partial L=up R=down repair=queued next=4.2s`, then `repair=running`, `recycling`, or a terminal reason;
- for an ordinary admission wedge, if a live Ring link blocks recycle, preserve it and report `host recovery deferred: ring still linked`;
- for an unretired APP_REGISTER/UNREG route, report `central recovery required: surviving G2/Ring links will reset; affected=<mask> restore=<mask>` because this implemented lifetime path is allowed to drop a survivor. An independent admin “force full BLE recovery” may still be offered for other cases with a clear warning that it drops all BLE links.

## Verification plan

There is no current automated G2/Ring reconnect harness. Extract the admission, topology, retry, and recycle-predicate logic into host-testable units, following the existing OTA protocol host-suite pattern.

### Host unit tests

1. **Admission/coalescing:** simultaneous link-loss callback, half-heal tick, manual `g2recover`, and saved reconnect produce one G2 job and one scan.
2. **Topology matrix:** all four L/R states produce the table above; queued topology changes are re-evaluated.
3. **Identity/authority:** unpair, MAC/address-type replacement, owner change, master-disable, role change, and user disconnect invalidate a queued job at every stage; automatic reconnect never stamps a new owner.
4. **Retry accounting:** busy, queue-full, coalesced, and role-blocked results do not consume attempts or one-shot intent; coalesced work waits for completion; `millis()` wrap is covered.
5. **Reconnect policy:** auto on/off, finite partial heal terminating on both-down, indefinite persistent retry, user disconnect, role change, boot-effective-role mismatch, and deferred Ring boot intent.
6. **Scan epoch/preemption:** late callbacks after stop, cancel, next request, and deinit are ignored; a LEFT/RIGHT targeted scan cannot publish the opposite side; callback allocation count remains constant; G2 repair preempts sliced manual Ring scan without callback-owner change.
7. **Cancellation:** cancel during scan, 35-second OPEN wait, discovery, characteristic write, prelude, and success-before-completion cannot publish stale success or leak a newly created link. Queue-full shutdown still reaches the out-of-band control path.
8. **Recycle predicate:** client controller-on/server-off permits once safe; server initialized, live phone, scan, queued/running job, callback, disabled master, wrong owner, and cooldown all block. Live L/R/Ring blocks the ordinary wedge signature but does **not** block a recorded unretired-route recovery; that exception still requires application-owner quiescence and must never authorize server teardown.
9. **Recycle failure:** pre-host deferral does not consume destructive cooldown; every host/controller state combination follows its normalization table; inconsistent normalization ends reboot-only without retry thrash.
10. **Detailed classifier:** an ordinary completed `0x100` cancellation does not trip; a timeout followed by post-quiesce cross-peer refusal or instrumented stuck create does; app-register/open API errors, allocation, queue rejection, and not-seen do not pollute that score. A missing APP_REGISTER/UNREG acknowledgement independently trips the lost-route quarantine/fault path.
11. **Client/Ring retirement:** reordered or missing APP_REGISTER/OPEN/DISCONNECT/UNREG/cancel events, persistent Ring-owner writes, RX callbacks, and static client alias cannot produce reuse, use-after-free, or double delete. Each of G2-L/G2-R/R1 has exactly one fixed retirement lane; a same-lane collision retains the newer application pointer and blocks replacement, and no fourth/untracked lane is created.
12. **Recovery intent:** master-disable, user-disconnect, unpair, and role change injected at every recovery phase are recorded immediately and determine the committed terminal role.
13. **Recovery restore set and role:** a G2 lost route with a live Ring and a Ring lost route with one live temple both reset the central role after the 3-second grace. The affected peer plus the collateral peer that was live before reset each get a one-shot reseek even when persistent auto-reconnect is off; current suppression/owner/target checks may still reject either. A boot-coerced client is restored even when persisted `bleMode` says server. Retirement-only normalization after runtime deinit stays down only when no central peer was previously live; master-disable stays down.
14. **One-eye manual learning:** preserve one admitted eye while learning the other, then replace/disconnect/reconnect the survivor or change its address type before completion; the job must close only its new link and must not commit a mixed pair.

### Integration tests with fakes

- Fake `BLEClient`/`BLEScan` with delayed, missing, and reordered APP_REGISTER, OPEN, UNREG, DISCONNECT, cancel acknowledgement, and scan callbacks, covering public and random address types and a survivor in the other central family.
- Assert heartbeats/control work continue while a fake connect burns the full timeout.
- Inject a G2 job and server start at every recycle boundary; one must defer, never overlap.
- Exercise queue full, allocation failure, worker stall, held TX/write mutex, persistent Ring owner, callback-handler clear/rebind, and every init/deinit rollback phase.
- Assert connection-priority nesting returns to zero and every persistent UI/Ring queue either drains or rejects its stale lifecycle epoch.
- Run extracted host code under ASan/UBSan and TSan where practical.

### Hardware and controller fault injection

1. Walk both temples out and back; independently shield/power off left and right; repeat at least 100 transitions.
2. Use a deterministic diagnostic IDF/controller fault hook to omit or delay cancel-complete and reproduce the stuck-create state. Walking out of range is not a reliable injector. Once all safety predicates are eligible, verify at most one bounded recycle per boot, then saved reconnect without reboot.
3. Repeat with Ring auto-reconnect both enabled and disabled, Ring already connected, active G2 microphone capture, live lens pages, and connection-priority changes. Inject a lost G2 route while Ring survives and a lost Ring route while one temple survives; prove checked recovery may reset the survivor, never the phone server, and requests gated one-shot reseek for both the affected peer and the peer live before reset—without requeueing an unrelated down peer.
4. Keep an authenticated phone-server session active and prove automatic client recovery never recycles it.
5. Drop/late-deliver OPEN, UNREG, cancel-complete, DISCONNECT, and scan result events with the diagnostic hook; verify handler clear/rebind and public/random address types.
6. Switch client → server → client repeatedly, including during queued reconnect and during recovery preflight.
7. Boot with an auto-reconnect peer plus persisted server preference; boot with Bluetooth master-disabled; verify explicit user-disconnect suppression.
8. Long soak: worker/task count, stack high-water marks, callback count, GATT client quarantine count, free heap, largest block, connection-priority depth, and subsequent manual connect. Build and boot the disabled-Bluetooth and disabled-G2 variants too.

### Acceptance criteria

- HCI/host trace never shows two outstanding create requests or a different-BDA cancellation caused by firmware overlap.
- Once a retry is due and the coordinator is idle, admission completes within 250 ms; a lower-priority sliced manual scan yields within 1 second.
- Recovery causes no control-owner heartbeat gap above 7 seconds on the normal 5-second cadence, including a full 35-second connect timeout.
- Schedule the first partial-repair due time directly on disconnect/partial-connect completion. Once due and idle, a temple seen in the first 3-second scan reconnects with p95 ≤15 seconds and p99 ≤25 seconds without scanning the already-live side.
- After a qualifying compound signature and once recycle safety is eligible, enter `RECOVERING` within 1 second and reach verified `G2_READY` or truthful `FAULT` within 20 seconds. For an ordinary wedge a live central link may defer this indefinitely; a recorded unretired native route overrides only that central-link preservation gate.
- No automatic recycle occurs with a live phone-server role or phone link. A live temple/Ring blocks ordinary wedge recovery; it may be reset only by the explicit unretired-route exception, with the affected peer mask and collateral drop surfaced.
- Lost-route recovery restores actual client ownership rather than persisted `bleMode`; its restore mask is exactly route-affected peers OR peers live before reset. A captured peer can reconnect with persistent auto-reconnect off, while user-down, missing owner, or missing target still rejects it. Retirement-only normalization with no prior G2 runtime or live peer remains down.
- Rejected/coalesced job counts, launched attempts, and next retry times remain truthful.
- Late callbacks cannot mutate a newer generation or the opposite eye during a targeted scan; a one-eye learned target cannot commit after the preserved survivor's generation, MAC, or address type changes; ASan/TSan-compatible extracted tests are clean.
- Across 100 clean reconnect cycles after warm-up: callback-object and task-count deltas are zero, routed/unreaped client count returns to zero after each acknowledged quiesce, and net DRAM loss is ≤8 KiB with no positive linear slope. Forced failures occupy at most the three named fixed retirement lanes; a same-lane collision creates no extra lane/allocation, and successful terminal teardown clears all three only after the library registry is gone.
- One automatic recycle is allowed per boot initially; its DRAM delta must remain ≤12 KiB and connection-priority depth must return to zero within 1 second of job completion.
- A pre-mutation deferral is visible within 1 second; every post-mutation failure reaches a verified ready state or reboot-only `FAULT` within 20 seconds—never silent retry forever.

## Rollout sequence

1. Land numeric connect-result telemetry, scan/job IDs, and host tests first so baseline and changed behavior are comparable.
2. Land the coordinator conversion: async G2 repair/manual scan, topology-aware saved reconnect, scan epoch/preemption, explicit admission results, truthful attempt accounting, and bounded in-worker Ring audio eligibility. Moving that eligibility outside the worker remains a latency follow-up.
3. Bench-gate prevention on at least three physical devices for 100 loss/recovery cycles each plus a 72-hour Ring+G2 soak. Required: zero crashes/UAFs/different-BDA overlap, zero callback/task growth, and the heap/latency bounds above. Keep automatic host recycle disabled; reboot remains the fallback.
4. Land the role-lifecycle transaction, client/Ring teardown barriers, normalization matrix, and safe recycle behind an independent local/persisted or compile-time kill switch that remains usable when BLE itself is down.
5. Require deterministic cancel-complete and lost APP_REGISTER/UNREG fault injection plus all server/ordinary-live-link/intent negative gates before enabling one automatic recycle per boot on a canary: at least five devices (or 5% of a larger fleet) for seven days.
6. Canary thresholds: zero crashes/UAFs, zero server-session recycle, zero live-link drops outside the documented unretired-route central reset, exact affected-plus-previously-live restore masking in that exception, no threshold breach above, and non-synthetic automatic recycle rate below 0.1 per device-day. Any one safety violation, >12 KiB recycle DRAM loss, worker/callback growth, or >1 recycle/device-day disables the recycle gate while retaining coordinator prevention.
7. Expand only after the canary passes; retain structured telemetry, the non-BLE kill switch, and explicit manual force-recovery. Raise the one-recycle-per-boot cap only after a separate repeated-recycle heap soak passes.

Keeping prevention and destructive recovery independently switchable makes rollback safe: if recycle exposes an unmodeled library lifecycle issue, disable only automatic recycle while retaining the serialization fix that prevents the original collision.

## Alternatives considered

| Alternative | Why not use it as the fix |
|---|---|
| Add a delay after every timeout | Reduces pressure but cannot prove cancel-complete arrived. |
| Change only `isBLERunning()` in the recycle guard | Exposes Ring dangling-pointer and role-transition races; can kill phone-server state if implemented broadly. |
| Put a semaphore around current scan functions | Contains some overlap but still blocks the heartbeat/control owner and leaves cancellation/lifecycle ambiguous. |
| Treat “one G2 side up” as peer connected | Suppresses the generic race by hiding a degraded pair; finite half recovery can then give up permanently even with auto-reconnect requested. |
| Force private Bluedroid state from CANCEL to IDLE | Can violate the controller's one-create invariant and is dependent on private IDF internals. |
| Reboot immediately after two failures | Reliable but destructive, loses unrelated runtime state, and does not remove the triggering race. |
| Rewrite all of `BLE_Peers` into a tri-state peer FSM now | Architecturally attractive, but expands boot/Ring/settings regression surface. Do it after the coordinator repair is proven. |

## Files changed for this remediation

| File | Change |
|---|---|
| `components/hardwareone/G2_Glasses.cpp/.h` | Existing-worker coordinator, async topology repair, family cancel epoch, atomic manual request/I-X handoff, targeted-scan side filtering, preserved-eye completion revalidation, completion/event linearization, checked command/audio subscription, three fixed retired-client lanes with reap grace/collision backpressure, scan/UI fences, actual-client-role restoration, and affected-plus-previously-live reseek |
| `components/hardwareone/G2_Ring.cpp/.h` | Coordinated scan/connect jobs, Ring cancel generation and atomic manual request/learned-target commit, completion/event linearization, checked notify/CCCD setup, fixed R1 retirement lane, and owner/RX teardown barrier |
| `components/hardwareone/BLE_Peers.cpp/.h` | Atomic manual saved-request creation, target-free manual-learn begin plus conditioned target/topology commit, synchronized identity/intent generations, conditioned full/repair completion, owner-conditioned config, and truthful retry accounting |
| `components/hardwareone/Bluetooth.cpp/.h` | Explicit controller/host/server predicates, role transition ownership, checked host lifecycle, and fail-closed fault publication |
| `components/arduino/libraries/BLE/src/BLEClient.cpp/.h` | Structured connect/unregister results, bounded APP_REGISTER wait, late registration cleanup, and route/interface retention until a real UNREG acknowledgement |
| `components/arduino/libraries/BLE/src/BLEDevice.cpp/.h` | Synchronized all-client registry, route/callback references, fail-closed `deleteClient()`, and complete post-host teardown retirement |
| `components/arduino/libraries/BLE/src/BLERemoteCharacteristic.cpp/.h` | Structured registration plus mandatory CCCD outcome with timeout/disconnect/late/generation reporting |
| `components/arduino/libraries/BLE/src/BLERemoteDescriptor.cpp/.h` | Checked descriptor-write API/event status and bounded generation-aware completion |

The affected private `docs2` G2/Bluetooth/Ring per-file and subsystem documents
are refreshed with the final implementation contract. Load-bearing concurrency,
lifecycle, protocol, memory, and status claims were checked against live source;
hardware-only timing claims remain explicitly unverified.

## Bottom line

The implemented fix is not “retry harder.” Central BLE create work now has one
owner; jobs are topology-, identity-, intent-, cancel-, and generation-aware;
manual target learning commits atomically against its I/X fence; subscription
success includes the CCCD descriptor result; and destructive recovery has an
acknowledged role/worker/client teardown transaction with complete tracked-client
retirement. Ordinary wedges preserve live central links. A lost native route
instead enters one of three fixed owner lanes, fails central admission closed,
gets a bounded safe-reap grace, and may reset a surviving central peer so the
route can be retired, while excluding the phone server and explicitly requeueing
the affected peer plus every central peer live before reset, each still gated by
current user-down/owner/target state. This removes the confirmed firmware race
consistent with the incident. Real-device cancel/re-entry, collateral-link, and
teardown fault injection remain the release gate.
