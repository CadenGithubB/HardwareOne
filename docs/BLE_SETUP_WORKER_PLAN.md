# One BLE setup worker: plan

Status (2026-10-18): S1 to S5 are implemented, uncommitted and not yet on
hardware. S1 ran on the bench inside images 62a49d7d, 5300a5ae and their
successors. S2 to S5 went in together, as the end state, with no halfway gate
code. See **Implementation** below.

**How it was made.** A nine-agent research run:
- three readers mapped the code;
- three designers each took a different approach, and a judge scored them and merged one plan;
- two skeptics tried to break that plan.

The plan is then re-based on the bench as it stands now: the C6 was reflashed to four BLE links that morning, after the research ran. Line numbers drift in this tree, so anchor on function names.

## The decision

The user decided on 2026-10-18 that **all BLE connection setup runs on one worker task, with the client as the job argument**:
- **The phone is just another queued job.** It waits its turn instead of cancelling itself.
- **No setup is interrupted by another client's setup** once it is running.

This changes the approved 2026-10-15 rule only where it said the phone yields. The waiting order may stay "glasses, then ring, then phone" (see Decisions).

## Where it stands today

**Glasses and ring.**
- They share one FIFO worker, `g2_ble_connect`, in `G2_Glasses.cpp`: 5120 B internal stack, priority 5, APP_CORE, created on the first submit.
- Jobs are `BleConnectJob`s with a `BleConnectKind` (`G2_EYE`, `G2_SAVED`, `G2_REPAIR`, `RING_SCAN`, `RING_SAVED`, `RING_MAC`, `RING_SCAN_ONLY`, `BARRIER`).
- Each job runs a blocking function:
  - a missed glasses re-seek holds the worker about 13 s;
  - a ring connect can wait up to 90 s for G2 audio to go idle before it even scans.

**Phone.**
- The phone pipe has its own task, `phone_pipe` (8192 B, priority 1).
- Its setup is a step machine advanced every 10 ms; the same task carries the data after `ready`.

**Where they meet.**
- Both meet at the setup gate in `BLE_CentralCoordinator.cpp`, a recursive mutex plus waiter and holder counts.
- The phone may not start while a glasses or ring job waits.
- If one starts waiting during the phone's setup, the phone cancels itself (`tickLocked`, `cause=14(yielded)`) and retries on its 5/15/30 s ladder. A cancelled setup typically costs 20 s or more.

**Since the C6 reflash (four links):**
- The ring is no longer shut out while the pipe is on, so glasses, ring and phone can all want a turn on the bench.
- The ring-yield paths (`bleCentralRingShouldYield`, the uncommitted ring fix) are now reachable only on a three-link C6.

## End state

**One task, `ble_setup`, in new `BLE_SetupWorker.h/.cpp`.**
- The slot, pick, withdraw and idle logic live in a pure `BLE_SetupWorkerCore.h`, host-compiled like `BLE_PhonePipeCore.h`.
- It is built whenever Bluetooth and either G2 or the phone pipe is on, so `p4-phone-only` gets it too.
- The phone pipe code and its strings are wrapped in `#if ENABLE_BLE_PHONE_PIPE` (the former phone-client mode is gone: the pipe is a device on the shared client stack, BLE_CentralStack).

**Three clients, each with one fixed slot.**
- The clients are G2, Ring and Phone. A slot is Empty, Filling, Queued, Running or Finishing, with a token.
- Each owner registers a constant ops table in its own file: `ready(token)` returning Run, Wait or Drop; `run(token)`; `finish(token, end)`.
- The worker knows no job kinds.
- The FreeRTOS pointer queue, the per-job `new`, `BARRIER` and the setup gate are all gone.

**Submit and withdraw never block or allocate.**
- **Submit:** claim the slot (Empty to Filling), write the payload, publish Queued with a new token, wake the worker. The wake uses an indexed task notification (index 1) or a static semaphore, because library code on the worker may use index 0.
- **Withdraw:** runs only on the owner's task and calls `finish` exactly once.

**Picking runs in two phases.**
1. Snapshot the slots under the spinlock.
2. Call `ready()` with no lock held. The ring's `ready()` takes the audio mutex and may log.
3. Back under the spinlock, change Queued(token) to Running, or to Empty on Drop; this fails if the token changed meanwhile.
4. Call `finish()` after the lock is released.

While anything is queued, the worker re-checks every 100 ms so stale jobs finish promptly.

**The phone's turn.**
- The phone's `run()` grants the turn to its token and raises `phone_pipe` to priority 5. This replaces the mutex priority inheritance it gets today.
- It then waits until the phone ends that token, and restores priority 1.
- `phone_pipe` keeps its step machine and data path; `BLE_PhoneGatt.*` is untouched.
- Every exit calls `leaveSetup()`, which runs `routeSet()` first, so the next client's scan events are never swallowed by the phone's interceptor.
- The phone's `finish` is lock-free: it stores a refused or ended token for the Waiting step to consume. Waiting also gets a backstop, so a job finished without running can never leave the phone stuck answering `BUSY`.

**Overrun.** A turn has a 75 s ceiling. On overrun:
1. The worker revokes the grant, by compare-and-swap so a late tick cannot use it.
2. It asks the phone to stop locally (the existing 4 s cleanup bound) and carries on with the other clients.
3. Only if the phone has not ended its turn by then does it set the recovery block and then `recoveryNeeded`, in that order, so they are never left half set.

The host-recovery path that drops the glasses is a last resort, not the first response.

**Kept:** link reservations, the recovery block, the TX gate (`BLE_CentralTx`), and the `BLE_Peers` ladders and admission results.

**Removed:** the yield cancel and `CauseDetail::Yielded`.

## Implementation (2026-10-18)

**What runs.**
- **One task, `ble_setup`** (BLE_SetupWorker.h/.cpp):
  - priority 5 on APP_CORE, started on first use;
  - the stack is 5120 bytes of internal RAM on boards with glasses and 4096 without (p4-phone-only).
- **Clients, in turn order:** glasses, ring retirement (pipe builds only), ring, phone.
- **Slots.** Each client has one slot with a token. Submit and withdraw never block or allocate.
- **Ordering.** A ready job that was passed over goes next. Several passed-over jobs go in the order they were passed over, so a job waits for at most one turn of each other client.
- **Pure scheduler.** The scheduler is BLE_SetupWorkerCore.h and has no OS dependencies.
- **Removed:**
  - the old `g2_ble_connect` queue worker and its BARRIER;
  - the shared setup gate with its waiter, holder and queued counts;
  - the phone's yield.

**Clients.**
- **Glasses and ring** (G2_Glasses.cpp). Their `ready()` checks, in order:
  1. Drop on their own cancel fence (G2 epoch; ring generation and peer request);
  2. Drop on a lifecycle fault or retirement;
  3. Drop on the recovery block;
  4. Wait during a role transition.
- **Ring's G2-audio wait.** The ring's wait for G2 audio is now a Wait verdict, so it no longer holds the worker. Its 90 s budget counts only audio time: other waits and other clients' runs pause it but never reset it.
- **Manual `ringscan`.** It polls every 100 ms and ends when another client's job is queued.
- **Who stops a ring scan.** Only the scanning job stops it. A stop request from another task only sets a flag, and the job keeps that flag from its start, so the request still works when it lands just before `start()`.
- **Ring retirement on a three-link C6.** It is a worker job:
  - it is queued from the main loop, only when a ring client exists and the G2 client is up;
  - it drops, rather than waits, on a role change.
- **Phone** (BLE_PhonePipe.cpp). It keeps its own task and step machine and runs its setup there. The worker:
  1. grants the turn, after raising phone_pipe to priority 5;
  2. waits for the phone to give it back;
  3. restores priority 1 on every exit.
- **Phone turn limits.**
  - A turn has a 75 s ceiling. An unused grant is revoked quietly. A used one is revoked; the phone stops locally within 10 s, or the worker sets the recovery block and then `recoveryNeeded`.
  - Enrollment lasts 60 s from the grant, and at most 60 s waiting for it.
  - At READY the turn goes back only once no scan flag is left.
  - On a three-link C6 the phone waits up to 10 s while the ring still holds its link.

**Teardown.**
- `g2Disconnect` and `g2RingDisconnect` withdraw a queued job of their own family.
- `deinitG2Client` cancels both families, withdraws ring retirement, then waits up to 6 s for the accessory slots to empty.
- `bleStackRecycleIfWedged` needs every slot idle, the phone's turn included.
- `bleRecoverStuckPhone` waits only for the phone's slot (quiescing ends its turn). Deinit takes care of the glasses and ring.
- Every host teardown that can follow a glasses session drops the ring's kept client first: `closeble`, `openconsole`, the stack recoveries and the companion reset, all through BLE_CentralStack's hostTeardown (the pipe no longer owns a host). This was found in review and is older than this change.

**Records.**
- `wait_ms` on glasses and ring connect records is the job's wait for other clients' setups: from submission to its turn, minus the time it was not ready itself.
- `BLE_COEX: setup wait <job> ms=` now also covers the phone and ring retirement.
- The phone's `wait_ms` is from admission to grant.
- The ring's in-flight watchdog is restamped when the job gets its turn.

**Verification (host only).**
- **New suite `ble_setup_worker`:** about 2,760 checks under ASan/UBSan and TSan, including a 20,000-iteration two-thread race and a measured pass-over bound.
- **Rewritten suites:** `ring_phone_yield` runs the real worker, ops and ring code on a virtual clock; `phone_runtime` runs the phone's turn.
- **Updated suites:** `ble_central_coordinator`, `g2_client_lifecycle`, `phone_recovery`, `ble_recycle_phone` and `ble_coex_hooks`.
- **Mutations:** 29 on the core, 25 on the ring and glasses clients, 21 on the phone and 21 on the review fixes. All are caught.
- **Adversarial review:** five reviewers with refute-first verifiers. All 11 upheld findings are fixed.
- **Not yet done:**
  - the manual bench checks under S1 and S4 below;
  - the phone checks, which need the phone advertising;
  - stack high-water marks on the S3 and ESP32 boards.

**First hardware boot (2026-10-18).** Image 06effab5 carries this change plus the OLED and status work. 120 s capture, 18h:
- **Health:** a clean boot, with no panic, watchdog or crash record.
- **`ble_setup`:** present, using 3,224 of 5,120 bytes of stack on the P4. `phone_pipe` uses 2,084 of 8,192.
- **Glasses:** `setup wait g2 saved ms=2021` (behind the phone's boot turn), then both temples connected in 65 and 45 ms.
- **Ring:** connected in 69 ms with `wait_ms=0`; setup verified as 2.3.2.0008.
- **Phone:** absent, so it missed on its 5/15/30 s schedule.
- **Worker warnings:** no `BLE_SETUP:` warnings.

**Cost.**
- The refactor adds about 1.0 KB of flash on xiao-default (4,768 bytes left after this and the R1 health work), plus 214 bytes of `.bss` and 72 bytes of `.data`.
- That misses the target above of no net growth.
- p4-phone-only gains the worker: a 4096-byte stack plus its control block.

## Stages

### S0. Land the ring fix and take a baseline (no new code)

1. **Fold in the remaining ring-fix review finding.** A ring job dropped before `commitAdmission` runs loses a one-shot reseek, because the worker preempts the main loop. Make `blePeerRestoreDroppedReseek` record the drop when it comes early, and have the commit keep the nudge.
2. **Ask whether three-link C6s are still supported.** If not, the ring-yield machinery can go entirely.
3. **Record the baseline** on the configuration S1 will be tested on, with continuous console capture of `BLE_COEX` lines and the `phonepipe stats` totals; `blerecords` keeps only 16 records.
   - This is Android's agreed A2 run: 20 or more toggles, glasses powered off, not `closeg2`. That answers their open question before the yield disappears.
4. **Measure the glasses' latency too:**
   - boot time to `[G2] Connected` (10 or more samples);
   - a temple power-cycled during a phone setup.

### S1. The phone waits its turn on today's gate (small, ships first)

**Status, 2026-10-18: implemented and green, uncommitted, not yet on hardware.**
- **Review fixes:** an adversarial review led to five more changes, all in the
  same change:
  - a glasses job queued while a ring job waits now goes first, and the ring
    job is put back in the queue;
  - the ring's own cancel ends its wait;
  - queued accessory jobs keep a Waiting phone out, and the phone re-checks
    after its take;
  - the C6's reported link limit holds for the transport session;
  - glasses and ring waits are measured from submission, and logged as
    `BLE_COEX: setup wait <job> ms=…`.
- **Tests:** the host suites pass and 29 of 29 mutations are caught.
- **Builds:** seven lanes build, including `feathers3` and `feather_esp32_v2`.
- **Candidate:** P4 `p4-phone-pipe-camera-off-test` image `2c919721…38a0`
  (ELF `58602c159`).
- **On the bench (2026-10-18, about 12:00):**
  - **Image:** S1 went onto the P4 inside image `62a49d7d`. That image was
    built from the shared tree, so it also has the R1 2.3.2 health changes.
  - **First evidence:** `setup wait ring saved ms=8424` followed by a ring
    connect 71 ms later.
  - **Earlier ring failures:** the open-stage timeouts (30 s, conn-cancelled)
    happened while the Even app held the ring.
  - **Ignore one log:** a `blerecords` block that shows `cause=14(yielded)`
    came from the earlier f724253f boot. The flashed image has no "yielded"
    label.
  - **Still needed:** a fresh `blerecords` on `62a49d7d`, and the bench
    checks below.

**Changes:**
- Delete the yield branch in `tickLocked`. Stop, intent change and enrollment expiry stay.
- In the Retiring to Idle branch, call `routeSet()` before giving the gate or lifting the recovery block.
- Add an accessory take that waits in 100 ms slices. Between slices it checks an abandon predicate (G2 cancel epoch, lifecycle fault, retirement, recovery block, ring-should-yield), keeping the waiter count raised for the whole wait.
- Handle `BARRIER` before any gate wait. Without that, `closeg2 full` fails behind an uninterruptible phone setup.
- Record the glasses' wait for the gate in their own records (`waitMs`; today it is phone-only), and widen it past 65,534 ms or mark saturation.
- Delete `CauseDetail::Yielded`. Fix the comments and docs that describe the cancel: the design review §2.6, the coexistence study and the phone pipe handoff.

**Pass criteria:**
- Every phone `cancelled` record has cause stopped, replaced or expired.
- The cancelled total does not rise with glasses or ring activity.
- The glasses' boot latency stays within an agreed budget (proposed: median up to 8 s slower).

**Glasses latency.** The phone usually wins at boot today: its boot reconnect starts before the glasses', 2 s apart. After S1 the glasses wait for the whole phone setup on every boot. If that misses the budget, defer the phone's boot admission until the glasses' boot job is submitted.

**Starvation.** On four links, glasses and ring re-seeks can alternate and keep the phone waiting. Either accept that until S4, or bring the pass-over bound (see Decisions) forward into S1.

**Bench checks:**
- A phone toggle during an `openg2 saved`, and the reverse.
- `closeg2` with a glasses job queued.
- `closephone` and `closeg2` during a phone setup.
- Three-client cases with the ring paired.
- The 30-minute run again.

### S2. The generic worker; glasses and ring move onto it (no intended behaviour change)

**Changes:**
- Create `ble_setup` and the core header.
- Split today's worker switch into the G2 and ring ops in their own files.
- Payloads are owned by their slots.
- Withdraw from `g2Disconnect`, `g2RingDisconnect` and the stack-teardown path. Teardown waits with `bleSetupWaitIdle`.

**Delete:** the queue, the barrier and the active flag.

**Worker creation.** Keep it lazy, or accept and record an always-on 5120 B on each board; this matters on `feather_esp32_v2`.

**Tests:** a new `ble_setup_worker` suite covering:
- `finish` runs exactly once for every outcome;
- a withdraw racing the pick on two threads;
- a re-submit between `ready()` and the pick;
- zero allocations per submit;
- glasses before ring.

The existing queue harness keeps its numbers.

**Bench:** connect, repair, `closeg2`/`openg2` five times, ring with the pipe stopped. `taskstats` should show `ble_setup` within about 200 B of today's 3208 B.

### S3. Ring work stops holding the worker (decide separately)

**(a) The ring's 90 s G2-audio wait becomes `ready()=Wait`.** It no longer occupies the worker, and it drops once at 90 s. Now that the ring is admitted on the bench, this can move early as an independent fix.

**(b) Ring retirement moves onto the worker.** When the pipe starts on a three-link C6, the main-loop ring retirement becomes a worker job with its own flag (not the ring connect payload). This takes up to 2 s of TX waiting off the main loop.

**(c) Manual `ringscan` becomes interruptible.** Convert `g2RingScan` to a non-blocking start with a 100 ms poll and an explicit stop (the glasses' pattern). Then the worker can end a manual `ringscan` when another client's job is queued, and a lost scan-complete event cannot wedge the worker. Add a per-job ceiling for accessory jobs.

**(d) Whether a glasses job may still interrupt a ring connect's scan** (`bleConnectPreemptForG2Repair`) is the user's call.
- Removing it extends "never interrupted" to accessories.
- It changes glasses-plus-ring behaviour on boards without the pipe (XIAO, FeatherS3, Feather V2, Pocket Assistant).
- So it needs a XIAO bench check: power-cycle a temple during a `ringconnect` self-scan, and run dictation with a ring reconnect pending. It also needs its own commit.

### S4. The phone becomes a worker client

**Changes:**
- `admit()` and the enroll branch submit the Phone slot. The Waiting step consumes a grant instead of taking the gate.
- `releaseSetup()` becomes `leaveSetup()` at every exit.
- The overrun handling described above.
- The enrollment window: proposed 60 s from the grant, with a 60 s cap while queued.
- `wait_ms` becomes admission to grant.
- In `bleRecoverStuckPhone`, add a bounded `bleSetupWaitIdle` right after the phone is quiesced and before the glasses are torn down. A timeout takes the existing lifecycle-fault path.

This is the highest-risk stage: the first change to the validated pipe's admission and setup start, and it gives up mutex priority inheritance.

**Bench:**
- S1's checks again, now turn-based.
- `PIPE_SETUP` totals within about 1 s of S1 with the camera and STT running.
- Stop, disconnect and forget, each while queued and mid-turn.
- `phonepipe enroll` while a glasses job holds the worker.
- `p4-phone-only`: 10 drop and reconnect cycles.

### S5. Delete the gate

**Delete:** `bleCentralSetupTake`/`Give`, `BleCentralSetupGuard`, the S1 take, the mutex and the waiter and holder counts.

**Re-source:** `bleCentralAccessoryBusy` becomes `bleSetupAccessoryBusy()`, true while a glasses or ring slot is queued or running.

**Stack:** set the final size from a measured high-water mark and the static call-graph bound (`-fcallgraph-info` plus `objdump`), per architecture. Never shrink below 5120 B from P4 numbers alone, and never move it to PSRAM.

**Bench:** the full S4 matrix, and an LLM HTTP request in flight during `openg2 saved`.

## Cost

**RAM:**
- **Boards with the glasses:** about neutral. The same 5120 B stack; three static slots and a semaphore replace the queue, the heap copies and the gate mutex.
- **`p4-phone-only`:** gains the worker, 5120 B of internal stack plus a task control block, unless a measured mark justifies less.
- **Watch:** the phone's secure-channel handshake stays on `phone_pipe`, so the worker's stack does not grow for it.

**Flash:**
- Hard ceiling: S2 to S5 net 0 B or less on `xiao-default` (16.6 KB free).
- Keep a per-stage table for `xiao-default` and `feathers3`, measured on the linked image.

**Latency:**
- **Phone:** stops losing 20 s or more per collision.
- **Glasses:** may wait one phone setup, 7 to 8.5 s typically and about 62 s at the phase bounds.
- **Main loop:** stops blocking up to 2 s in ring retirement.
- **Ring:** stops holding the worker for 90 s during G2 audio.

**Build lanes for every stage:** `p4-phone-pipe-camera-off-test`, `p4-phone-only`, `p4-glasses-test`, `xiao-default`, `xiao-no-bluetooth`, `feathers3`, `feather_esp32_v2`.

## Decisions needed

1. **Waiting order.** Recommended: glasses, then ring, then phone, where a job already passed over once goes next, bounding any wait to the running job plus one per other client. The alternative is plain FIFO.
2. **Three-link C6s.** Still supported? If not, delete the ring-yield paths rather than keep testing them.
3. **Manual `ringscan`.** May another client's queued job end it early (recommended), and may a glasses job still interrupt a ring connect's scan?
4. **Phone priority.** Raise `phone_pipe` to 5 for its whole turn (recommended), or only while another client waits?
5. **Enrollment window.** 60 s from the grant with a 60 s cap while queued (recommended), or 60 s from the command?
6. **Commits.** The tree has about 226 uncommitted files, including the ring fix and the coexistence records. Checkpoint it after your hardware test so S1 is a clean diff? Then S1 alone, and S2 to S5 as one commit after its hardware test.
7. **Android.**
   - Run the agreed A2 run first, as S0's baseline.
   - Send a firmware-009 note with S1 ("yielded" is gone, the glasses now wait, `wait_ms` can be large).
   - Route any phone-ladder change through the exchange as an amendment to A-3.
8. **A native ESP32-S3 test configuration** with glasses, ring and the pipe? Optional; the bench now covers three clients.
