# G2 subsystem refactor proposal — task consolidation, queue unification, race-safe lens UI

**Companion to:** [G2_TASKS_REPORT.md](G2_TASKS_REPORT.md) (raw inventory)
**Date:** 2026-05-04
**Status:** Proposal. No code changes yet.
**Audience:** Whoever picks this up next — likely a different model or future me. Self-contained: cites line numbers, defines new contracts, lists open questions.

---

## 0. Why this document exists

The G2 stack works. It is also slapped on top of the rest of the firmware: most G2 features were added with a `xTaskCreate` per feature instead of going through the existing `cmd_exec_task` + `submitCommandAsync` pipeline that Web, Serial, and Phone-BLE already share.

This document does three things:

1. **Inventories** what is actually running today (all G2-attributable tasks, queues, semaphores). The raw data lives in [G2_TASKS_REPORT.md](G2_TASKS_REPORT.md); §2 here is the consolidated rollup with stack budgets.
2. **Identifies** the structural problems — not "too many tasks" as a vibe, but specifically *which* tasks duplicate infrastructure that already exists, *which* are load-bearing for real reasons (BLE-callback reentrancy, lens UX latency), and *which* are accidental complexity.
3. **Proposes** a target architecture (a hybrid: dedicated lens worker for navigation + everything else through `cmd_exec_task` via `submitCommandAsync` with race-safety contracts) and a phased migration plan.

The proposal is **opinionated**. Disagree with specific moves and tell me; do not fork it silently.

---

## 1. Executive summary

**Current cost (G2-attributable persistent tasks):**

| Task | Stack | Priority | Lifecycle |
|---|---|---|---|
| `tapDispatcherWorkerLoop` | **20 KB** | 2 | Persistent |
| `pageSwapWorkerLoop` | 4 KB | 2 | Persistent |
| `fsmWorkerTask` (hijack FSM) | 3 KB | 5 | Persistent |
| `heartbeatWorkerTask` | ~8 KB (dynamic) | 5 | Persistent |
| **Subtotal** | **~35 KB** | | |

Plus **17 distinct one-shot worker patterns** (`xTaskCreate` per user gesture) totaling 4–8 KB each, frequently with explicit "if internal DRAM allows, else PSRAM-static fallback" branches — a strong signal that the design has already outgrown internal DRAM.

**System-wide cost (for context, not G2-attributable):**

| Task | Stack | Priority |
|---|---|---|
| `cmd_exec_task` | 24 KB (6144 words) | 1 (LOW) |

`cmd_exec_task` is the existing async command executor. It is **already** how Web, Serial, Phone-BLE Server, ESP-NOW, and Automation get their work done. G2 hijack UI and most G2-spawned features bypass it.

**Headline diagnosis:**

- The 20 KB `g2_tap_disp` stack exists because hijack tap handlers do deep work inline (`writeSettingsJson`, WiFi state mutation, sensor toggles). That work belongs on `cmd_exec_task`, not on a UI dispatcher.
- The 17 one-shot patterns largely exist because each new G2 feature was added as "spawn a task" instead of "submit a command." Most of them allocate the same heap-owned arg struct, run a single function, and self-delete — a worker-pool antipattern.
- The two-queue model (gPageSwapQueue depth 2, gTapQueue depth 8) plus the FSM queue is **fine in shape** but does not connect cleanly to the rest of the system: there is no contract for "command finished — refresh lens UI" because lens UI is currently driven by direct calls inside the tap handlers.
- There is **no generation counter for command/UI synchronization**. `gNotifyGen` (G2_Glasses.cpp:9078) handles timed notification clears, but no equivalent exists for hijack page state, so a slow command completing after a navigation will redraw a stale screen.

**Headline target:**

| | Today | Target |
|---|---|---|
| Persistent G2 tasks | 4 (~35 KB) | 3 (~12 KB) |
| One-shot patterns | 17 | ~3 (typed worker pool) |
| Tap dispatcher stack | 20 KB | ~6 KB (UI only) |
| System mutations from taps | inline on tap_disp | `submitCommandAsync` → `cmd_exec_task` |
| Lens UI updates | scattered direct calls | single `LensUiJob` queue |
| Race safety | none (no menuGen / cmdSeq) | menuGen + cmdSeq + ViewId contract |

---

## 2. Consolidated inventory

Everything below is sourced from the inventory in [G2_TASKS_REPORT.md](G2_TASKS_REPORT.md). Line numbers are absolute paths into the working tree.

### 2.1 Persistent G2 tasks

| Task fn | File:line | Stack | Prio | Queue/source |
|---|---|---|---|---|
| `pageSwapWorkerLoop` | [G2_Glasses.cpp:7370](../components/hardwareone/G2_Glasses.cpp:7370) | 4 KB | 2 | `gPageSwapQueue` (depth 2) |
| `tapDispatcherWorkerLoop` | [G2_Glasses.cpp:7496](../components/hardwareone/G2_Glasses.cpp:7496) | **20 KB** | 2 | `gTapQueue` (depth 8) |
| `fsmWorkerTask` | [G2_HijackFsm.cpp:310](../components/hardwareone/G2_HijackFsm.cpp:310) | 3 KB | 5 | `gFsmQueue` (depth 32) |
| `heartbeatWorkerTask` | [G2_Glasses.cpp:5330](../components/hardwareone/G2_Glasses.cpp:5330) | ~8 KB | 5 | `gBeatSem` |

### 2.2 One-shot G2 worker patterns

Each row is a distinct `xTaskCreate` site that allocates an args struct, runs a function, and self-deletes.

| Worker | File:line | Stack | Prio | Purpose |
|---|---|---|---|---|
| `hijackWorkerTask` | [G2_Glasses.cpp:3883](../components/hardwareone/G2_Glasses.cpp:3883) | 4 KB | 2 | Hijack bootstrap on menu start |
| `g2ConnectTaskBody` | [G2_Glasses.cpp:5849](../components/hardwareone/G2_Glasses.cpp:5849) | 6 KB | 5 | Connect single eye |
| `g2ConnectSavedTaskBody` | [G2_Glasses.cpp:5915](../components/hardwareone/G2_Glasses.cpp:5915) | 6 KB | 5 | Reconnect from NVS |
| `livePageWorker` | [G2_Glasses.cpp:8312](../components/hardwareone/G2_Glasses.cpp:8312) | 4 KB | 5 | Live list refresh |
| `liveTextWorker` | [G2_Glasses.cpp:8893](../components/hardwareone/G2_Glasses.cpp:8893) | 4 KB | 5 | Live text refresh |
| `notifyClearTaskBody` | [G2_Glasses.cpp:9127](../components/hardwareone/G2_Glasses.cpp:9127) | 3 KB | 1 | Delayed notification dismiss |
| `g2BmpViewerWorker` | [G2_Glasses.cpp:12376](../components/hardwareone/G2_Glasses.cpp:12376) | 6 KB | 2 | Single BMP push |
| `g2CameraViewerWorker` | [G2_Glasses.cpp:12679](../components/hardwareone/G2_Glasses.cpp:12679) | 6 KB | 2 | Camera one-shot frame |
| `g2CameraStreamWorker` | [G2_Glasses.cpp:12961](../components/hardwareone/G2_Glasses.cpp:12961) | 6 KB | 2 | Camera streaming loop |
| `g2BmpFullViewerWorker` | [G2_Glasses.cpp:13274](../components/hardwareone/G2_Glasses.cpp:13274) | 8 KB | 2 | Full-screen BMP |
| `networkScanWorker` | [G2_Page_Network.cpp:756](../components/hardwareone/G2_Page_Network.cpp:756) | 4 KB | 5 | WiFi scan flow |
| `wifiPendingWatchdogTask` | [G2_Page_Network.cpp:886](../components/hardwareone/G2_Page_Network.cpp:886) | 4 KB | 5 | Connect-pending poll |
| `aiWorker` | [G2_Page_TestSuite.cpp:517](../components/hardwareone/G2_Page_TestSuite.cpp:517) | 4 KB | 5 | AI test command |
| `imgProbeWorker` (×2 paths) | [G2_Page_TestSuite.cpp:993](../components/hardwareone/G2_Page_TestSuite.cpp:993) | variable | 5 | Image probe (internal+PSRAM static fallback) |
| `ringConnectTaskBody` | [G2_Ring.cpp:947](../components/hardwareone/G2_Ring.cpp:947) | 5 KB | 5 | Ring connect |
| `ringConnectSavedTaskBody` | [G2_Ring.cpp:970](../components/hardwareone/G2_Ring.cpp:970) | 5 KB | 5 | Ring reconnect from NVS |
| `ringConnectMacTaskBody` | [G2_Ring.cpp:1000](../components/hardwareone/G2_Ring.cpp:1000) | 5 KB | 5 | Ring connect by MAC |
| `ringSpoofTaskBody` | [G2_Ring.cpp:1184](../components/hardwareone/G2_Ring.cpp:1184) | 4 KB | 4 | Spoof loop |
| `ringBridgeHeartbeatBody` | [G2_Ring.cpp:1564](../components/hardwareone/G2_Ring.cpp:1564) | 3 KB | 3 | Ring HB loop |

That is 19 distinct workers, of which **17 follow the same shape**: heap-alloc args, single function, self-delete. The two exceptions (`ringSpoofTaskBody`, `ringBridgeHeartbeatBody`) are loops that exit on a flag — those are legitimate persistent-but-toggleable workers.

### 2.3 G2 queues / semaphores

| Primitive | File:line | Depth/type | Producer → Consumer |
|---|---|---|---|
| `gPageSwapQueue` | [G2_Glasses.cpp:7361](../components/hardwareone/G2_Glasses.cpp:7361) | 2 × `PageSwapArgs*` | Tap/page handlers → `pageSwapWorkerLoop` |
| `gTapQueue` | [G2_Glasses.cpp:7490](../components/hardwareone/G2_Glasses.cpp:7490) | 8 × `TapDispatchEntry` | `handleDevEvent` (BLE notify) → `tapDispatcherWorkerLoop` |
| `gFsmQueue` | [G2_HijackFsm.cpp:302](../components/hardwareone/G2_HijackFsm.cpp:302) | 32 × `FsmEvent` | `hijackFsmDispatch` → `fsmWorkerTask` |
| `gCmdExecQ` | [HardwareOne.cpp:1154](../components/hardwareone/HardwareOne.cpp:1154) | 6 × `ExecReq*` | All command origins → `cmd_exec_task` |

Plus per-temple `writeMutex`, `gMicRecMutex`, `gMicWavMutex`, `gMicAfeMutex`, `gMicAfeReadySem`, `gCreateAckSem`, `gRebuildAckSem`, `gImgPushAckSem`, `gBeatSem`, `gLivePageRefreshSem`, `gLiveTextRefreshSem`, `gRing.writeMutex`. These are mostly fine; mic mutexes belong to the audio subsystem, ack sems are protocol-correct, ring write mutex is necessary.

### 2.4 The infrastructure G2 is currently bypassing

```cpp
// System_CommandTypes.h:36-46
struct CommandContext {
  CommandOrigin origin;       // SERIAL / WEB / AUTOMATION / SYSTEM / BLUETOOTH
  AuthContext   auth;
  uint32_t      id;
  uint32_t      timestampMs;
  uint32_t      outputMask;   // CMD_OUT_LOG | CMD_OUT_BLE | ...
  bool          validateOnly;
  bool          captureOutput;
  void*         replyHandle;
  httpd_req_t*  httpReq;
};

// System_CommandTypes.h:54-69
struct ExecReq {
  char line[2048];
  CommandContext ctx;
  char out[4096];
  SemaphoreHandle_t done;            // sync mode
  bool ok;
  ExecAsyncCallback asyncCallback;   // async mode
  void* asyncUserData;
};

// System_Utils.cpp:3058
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback cb, void* userData);
```

Phone-BLE already does it right — see [Bluetooth.cpp:661](../components/hardwareone/Bluetooth.cpp:661):

```cpp
submitCommandAsync(ucmd, bleCommandResultCallback, (void*)(uintptr_t)connId);
```

The G2 hijack path does not. **That is the central architectural mismatch.**

---

## 3. Where the bloat actually lives

Three causes, in order of how much they cost:

### 3.1 Hijack handlers do system mutation inline on `tap_disp`

The 20 KB stack on `tapDispatcherWorkerLoop` is sized for the deepest thing any tap handler does. The deep paths are not lens protocol — they are `setSetting → writeSettingsJson` (NVS + JSON serialize + flash write) and similar. These are exactly what `cmd_exec_task` exists for, and `cmd_exec_task` already has a 24 KB stack for that purpose.

System-mutation leaves identified in [G2_Page_Network.cpp:834](../components/hardwareone/G2_Page_Network.cpp:834), [G2_Page_Network.cpp:971](../components/hardwareone/G2_Page_Network.cpp:971), Settings/Files/Sensors/Power/CameraSettings page handlers:

- `WiFi.disconnect`, `connectToBestWiFiNetwork`
- `cmd_httpstop`, `cmd_httpstart`
- `cmd_wifirm`
- `setSetting` + `writeSettingsJson` (every settings toggle)
- File ops, sensor toggles, BLE mode changes

If those move to `cmd_exec_task` via `submitCommandAsync`, `tap_disp` only needs to do: decode the gesture, decide "navigation or mutation", and either enqueue a lens job or submit a command. That fits in a few KB of stack.

### 3.2 17 one-shot patterns are a worker pool that wasn't built

Almost every "feature that needs to do something async without blocking the caller" got its own `xTaskCreate`. The shape is identical:

```cpp
struct FeatureArgs { ...heap-owned... };
static void featureWorker(void* pv) {
  auto* a = (FeatureArgs*)pv;
  doTheThing(*a);
  delete a;
  vTaskDelete(nullptr);
}
xTaskCreate(featureWorker, "feature", STACK, args, PRIO, nullptr);
```

Symptoms of pressure already visible in the code:

- `imgProbeWorker` has two `xTaskCreate` paths — one internal-stack, one PSRAM-static — chosen by runtime DRAM check ([G2_Page_TestSuite.cpp:993,1013](../components/hardwareone/G2_Page_TestSuite.cpp:993)).
- `aiWorker` has a fallback to inline if `xTaskCreate` fails ([G2_Page_TestSuite.cpp:517](../components/hardwareone/G2_Page_TestSuite.cpp:517)).
- `networkScanWorker` same fallback ([G2_Page_Network.cpp:756](../components/hardwareone/G2_Page_Network.cpp:756)).
- `heartbeatWorkerTask` has a DRAM-headroom check before spawning ([G2_Glasses.cpp:5330](../components/hardwareone/G2_Glasses.cpp:5330)).

These fallbacks each say the same thing: *we want to spawn a task here, but we know we are running close to the edge.* The fix is not "more clever fallbacks" — it is "stop spawning tasks per gesture."

### 3.3 No unified lens applier — UI mutations are direct function calls

`gPageSwapQueue` exists, but it is one of *several* paths that write to the lens. Tap handlers also call:

- `g2ShowListPage`, `g2ShowTextAsList`, `g2ShowMultiText` directly
- `g2RedrawHijackMainMenu`
- `g2LensStartOverlay`
- per-temple `writeMutex`-guarded BLE writes

This is fine while everything happens on `tap_disp`. It is **not** fine once `cmd_exec_task` is sending lens updates back, because cmd_exec callbacks would race tap_disp's lens writes and there is no single ordering point.

---

## 4. Architectural problems beyond stack count

These matter more than the stack arithmetic, because stacks are easy to shrink once these are right.

1. **No request identity.** Tap handlers spawn workers and forget about them. There is no way to ask "is the WiFi-connect that this tap kicked off still relevant, or did the user navigate away?" There is no `cmdSeq`, no `menuGen` (only `gNotifyGen` for overlays).

2. **Two writers, one wire.** Today only `tap_disp` and the page-swap worker write to the lens. The moment `cmd_exec_task` posts a result back, you have a third writer with no shared ordering primitive. You cannot have three writers sharing a per-temple `writeMutex` and expect the lens to update in user-meaningful order.

3. **Origin/auth is lost.** When a tap toggles a setting, the resulting `setSetting` runs without origin/auth context, so the audit log loses information that Web/Serial/BLE-server already record correctly. This will bite when something on the lens silently changes a security-relevant setting.

4. **Output mask doesn't reach the lens.** `outputMask` controls where command output goes (log, BLE text, etc.). Lens is not in that list. If we route taps through `cmd_exec`, we need either a new mask bit (`CMD_OUT_LENS`) or an explicit "post a lens job from this callback" rule. Mixing those is how "the lens shows last week's WiFi error" bugs are born.

5. **No cancellation story.** If the user taps "Connect to FooNet" and immediately navigates away, the connect attempt continues, and its eventual success/failure has nowhere meaningful to render. Today this is hidden because the connect happens inline. With async, it becomes visible.

---

## 5. Target architecture (the hybrid)

This is the architecture you articulated in the conversation, formalized.

### 5.1 Three layers, three queues

```
                +--------------------+
   BLE notify   |  tap_disp (UI)     |
   (BTC_TASK)   |  ~6 KB stack       |
   --------->   |  prio 2            |
   gTapQueue    |  Decides:          |
                |  navigation vs cmd |
                +--------+-----------+
                         |
              +----------+-----------+
              |                      |
        navigation              system mutation
              |                      |
              v                      v
     +-----------------+    +-------------------+
     | gLensJobQueue   |    | gCmdExecQ         |
     | (replaces       |    | (existing,        |
     |  gPageSwapQueue,|    |  submitCommandAsync)
     |  unified)       |    |                   |
     +--------+--------+    +---------+---------+
              |                       |
              v                       v
     +-----------------+    +---------------------+
     | lens_applier    |    | cmd_exec_task       |
     | ~4 KB, prio 2   |    | 24 KB, prio 1       |
     | persistent      |    | persistent (exists) |
     +--------+--------+    +----------+----------+
              |                        |
              | <----- LensUiJob ------+
              |        (gen-guarded callback enqueues here)
              v
        BLE temple write (per-temple writeMutex)
```

**Three persistent G2 tasks. One existing system task. One queue per concern. No per-gesture `xTaskCreate`.**

### 5.2 Role of each task

**`tap_disp` (renamed `g2_ui` ideally) — UI dispatcher.**
- Receives `TapDispatchEntry` from BLE notify path (unchanged producer side; that part is correct and load-bearing for BTC reentrancy reasons documented at [G2_Glasses.cpp:7410-7437](../components/hardwareone/G2_Glasses.cpp:7410)).
- Decides per-tap whether the work is *navigation* (stay here) or *mutation* (delegate).
- For navigation: directly enqueues a `LensUiJob` to `gLensJobQueue`. No lens writes from this task — only enqueues.
- For mutation: builds a `Command` with proper `CommandContext` (origin = `ORIGIN_G2_HIJACK`), captures a `cmdSeq` and `viewId` snapshot, calls `submitCommandAsync` with a callback that will enqueue a `LensUiJob` on completion. Returns immediately.
- Stack drops from 20 KB to ~6 KB (no more `writeSettingsJson` on this stack).

**`lens_applier` (replaces `pageSwapWorkerLoop`) — the only writer to the lens.**
- Receives `LensUiJob` from `gLensJobQueue` (replaces `gPageSwapQueue`, larger depth, e.g. 8).
- Holds the per-temple `writeMutex` discipline (unchanged).
- Guards every job against staleness using `menuGen` and `cmdSeq` (see §6).
- Stack ~4 KB (same as today).
- This is the **single point of serialization for lens output**, which gives us the ordering property §4.2 requires.

**`fsmWorkerTask` (unchanged).**
- Already a clean queue worker; FSM events are not touched by this refactor.

**`cmd_exec_task` (unchanged).**
- All system mutations from G2 go here, alongside Web/Serial/BLE/Automation traffic.
- Async callbacks from G2-originated commands enqueue `LensUiJob`s — they **do not** touch the lens directly.

### 5.3 The two new types

```cpp
// New origin enum value (extend CommandOrigin in System_CommandTypes.h:19)
enum CommandOrigin {
  ORIGIN_SERIAL,
  ORIGIN_WEB,
  ORIGIN_AUTOMATION,
  ORIGIN_SYSTEM,
  ORIGIN_BLUETOOTH,
  ORIGIN_G2_HIJACK,   // new
};

// New job type for the unified lens applier queue
enum class LensJobKind : uint8_t {
  PageSwap,           // subsumes PSK_LIST/TEXT/MULTITEXT/LIST_TEXT
  Redraw,             // re-render current page from canonical state (e.g. after WiFi toggle)
  Toast,              // transient overlay (success/error from a command)
  Notify,             // existing notification path (subsumes notifyClearTaskBody)
  Custom,             // typed payload for one-off renders (BMP, camera frame, etc.)
};

struct LensUiJob {
  LensJobKind kind;
  uint32_t    submitMenuGen;   // captured at enqueue time
  uint64_t    cmdSeq;          // 0 if navigation-origin, non-zero if cmd-completion-origin
  G2HijackPage targetPage;     // viewId snapshot at submit
  NetworkSubMode targetNetSub; // viewId snapshot at submit (-1 if N/A)
  union {
    PageSwapArgs* swap;        // existing struct; lens_applier frees on completion
    RedrawSpec*   redraw;
    ToastSpec*    toast;
    NotifySpec*   notify;
    CustomSpec*   custom;
  } payload;
};
```

### 5.4 The race-safety contract

Two counters owned by the UI thread:

- `gMenuGen : uint32_t` — bumped whenever the user navigates (back, into a submenu, into text entry, etc.). Anything that captured an old `gMenuGen` is now stale.
- `gCmdSeq : uint64_t` — monotonic, incremented per `submitCommandAsync` from G2.

When `tap_disp` submits a command:

```cpp
G2CmdCookie cookie {
  .seq          = ++gCmdSeq,
  .menuGen      = gMenuGen,
  .targetPage   = g2GetHijackPage(),
  .targetNetSub = gNetSub,
};
submitCommandAsync(cmd, g2HijackCmdComplete, heap_copy_of(cookie));
```

When the callback fires on `cmd_exec_task`:

```cpp
void g2HijackCmdComplete(bool ok, const char* result, void* userData) {
  auto* c = static_cast<G2CmdCookie*>(userData);
  // Build a LensUiJob with c->seq and c->menuGen captured.
  // Enqueue to gLensJobQueue. Do NOT touch the lens here.
  enqueueLensJob(buildJobFromResult(*c, ok, result));
  delete c;
}
```

When `lens_applier` dequeues a job:

```cpp
if (job.submitMenuGen != gMenuGen) {
  // User navigated away. Drop silently. Log at TRACE only.
  return;
}
// Optional: per-subsystem latest-wins. If the cmd was a WiFi op and a
// newer WiFi cmdSeq has already completed, drop this one.
if (isStaleForSubsystem(job)) return;
applyJob(job);
```

This is the same shape as `gNotifyGen`, just generalized. It is the single most important new contract; everything else follows from it.

### 5.5 What about latency?

The earlier worry was: "if taps go through `cmd_exec_task` (priority 1), they'll wait behind a long automation job." The hybrid neutralizes this:

- **Navigation taps** never touch `cmd_exec_task`. They go straight to `gLensJobQueue` from `tap_disp`. No starvation possible.
- **Mutation taps** *do* go through `cmd_exec_task`. If automation is running long, the mutation waits, but:
  - The user sees an immediate "Pending…" overlay (an optimistic `LensUiJob` enqueued by `tap_disp` before submitting the command).
  - When the command completes, the callback enqueues the real result.
  - This matches how Web/Serial users already experience the system; G2 users now have the same fairness.

If lens mutation latency proves unacceptable in practice, the mitigation is **two queues in `cmd_exec_task` consumed by the same worker** (interactive vs. bulk), not a separate worker. The earlier conversation noted this; restating for the record.

---

## 6. Concrete consolidation moves

In rough order of value-to-effort:

### 6.1 (M1) Replace `gPageSwapQueue` with `gLensJobQueue`

- New struct `LensUiJob` (§5.3). `PageSwapArgs` becomes one variant.
- Rename `pageSwapWorkerLoop` → `lensApplierLoop`. Same task, larger queue (depth 8).
- `notifyClearTaskBody` (the only timed task remaining) becomes a `Notify` job posted by a one-shot esp_timer; the timer fires and enqueues a clear-job. **Eliminates the `xTaskCreate` per notification.**
- `livePageWorker` and `liveTextWorker` are replaced by jobs of kind `Redraw` posted from a single `liveRefreshTimer`. **Eliminates two more `xTaskCreate` patterns.**

**Net effect:** `gPageSwapQueue` is gone, three one-shot patterns are gone, one new queue exists. Lens output is now serialized through one task.

### 6.2 (M2) Add `menuGen` / `cmdSeq` / cookie discipline

- Add the two counters next to `gNotifyGen`.
- Bump `menuGen` in `g2SetHijackPage`, `g2EnterTextEntry`/`g2ExitTextEntry`, `gNetSub` mutations, and any "back" path.
- Add `G2CmdCookie` and the helper `g2SubmitHijackCommand(line, cookie)` that wraps `submitCommandAsync` with the right `CommandContext` (auth = system, origin = `ORIGIN_G2_HIJACK`, `outputMask = CMD_OUT_LOG`).
- This change is small and lands **before** any tap handler is rewritten — it is infrastructure that everyone after it depends on.

### 6.3 (M3) Migrate hijack tap handlers to commands, page by page

For each page handler that mutates state:

1. Identify the mutation leaves (already done in [G2_TASKS_REPORT.md](G2_TASKS_REPORT.md) §5).
2. Confirm a CLI command exists for that mutation. If not, add one — these are useful regardless of G2.
3. Replace the inline call with `g2SubmitHijackCommand("wifi disconnect", cookie)` etc.
4. Make sure the optimistic UI update (immediate "Pending…" overlay) goes through a `LensUiJob`.
5. Make sure the callback enqueues a `Redraw` job for the right `targetPage`/`targetNetSub`.

Recommended order (least-risk first): Files → Sensors → CameraSettings → Settings → Network → Power. Network is last because WiFi state changes are the most user-visible and the most prone to races.

After M3, `tap_disp` no longer does deep work. **Drop its stack to 6 KB.**

### 6.4 (M4) Collapse the connect/reconnect variants

`g2ConnectTaskBody`, `g2ConnectSavedTaskBody`, `ringConnectTaskBody`, `ringConnectSavedTaskBody`, `ringConnectMacTaskBody` are five tasks doing the same thing five ways. They are also the right kind of work to live behind commands:

- Define `g2connect [eye|saved]` and `ringconnect [saved|<mac>]` as commands (if they don't already exist as such).
- Delete the five worker entry points; their callers now `submitCommandAsync`.
- The "active" flags (`gConnectTaskActive`, `gRingConnectTaskActive`) become per-subsystem "in-flight cmdSeq" markers in the cookie discipline.

**Eliminates 5 one-shot patterns. Saves ~28 KB of stack at peak (worst-case overlap).**

### 6.5 (M5) Collapse the media workers

`g2BmpViewerWorker`, `g2CameraViewerWorker`, `g2CameraStreamWorker`, `g2BmpFullViewerWorker` are four tasks doing similar things (push pixels to the lens). Two options:

- **Option A (smaller change):** Make them all `Custom` `LensUiJob`s. The applier's stack stays small, but the job payload carries a function pointer. This works only if the operations are bounded — camera *streaming* isn't, so it has to stay separate or run as a timer-driven job-emitter.
- **Option B (cleaner):** A single persistent `g2_media` worker (~6 KB stack) consuming a small media queue. Streaming becomes a job that loops; one-shot pushes are a single iteration.

I'd recommend B. **Eliminates 4 one-shot patterns.**

### 6.6 (M6) Collapse the test/AI/scan workers

`aiWorker`, `imgProbeWorker` (×2 paths), `networkScanWorker`, `wifiPendingWatchdogTask`, `hijackWorkerTask` are all "do a slow thing, post a result." All five of these are commands in disguise:

- `ai test`, `imgprobe`, `wifiscan` already exist as CLI commands (or should).
- The "watchdog" pattern (`wifiPendingWatchdogTask`) is a deadline poll; it should be a one-shot esp_timer that enqueues a `Toast` job on expiry.
- `hijackWorkerTask` is the hijack bootstrap; its body should be moved into the FSM (which already has a queue) or expressed as a sequence of `LensUiJob`s.

**Eliminates 5–6 one-shot patterns.**

### 6.7 (M7) Keep ring spoof and ring bridge HB as-is

`ringSpoofTaskBody` and `ringBridgeHeartbeatBody` are loop-with-flag workers, not "do-once-and-die." They are legitimately persistent-but-toggleable. Leave them. Optional polish: convert to event-group-driven loops or shared "g2_ring_loop" task that handles both, but that's <5 KB savings and not the bottleneck.

---

## 7. Stack budget: before/after

### Persistent G2 stacks

| | Before | After (target) |
|---|---|---|
| `tap_disp` (or `g2_ui`) | 20 KB | 6 KB |
| `pageSwapWorkerLoop` / `lens_applier` | 4 KB | 4 KB |
| `fsmWorkerTask` | 3 KB | 3 KB |
| `heartbeatWorkerTask` | ~8 KB | ~8 KB (out of scope) |
| **Total persistent** | **~35 KB** | **~21 KB** |

### Peak overlap (one-shot tasks alive simultaneously, worst case)

Today: at the worst moment, you can plausibly have hijack worker (4) + g2 connect (6) + ring connect (5) + bmp full viewer (8) + live page (4) + notify clear (3) = **~30 KB transient** on top of the persistent 35 KB.

After: those same operations are either commands on `cmd_exec_task` (already running) or jobs on `lens_applier` / `g2_media` (already running). **Transient overhead → near zero.**

### Total internal-DRAM relief estimate

Roughly **~14 KB persistent + ~25 KB peak transient**, i.e. ~40 KB of internal-DRAM headroom that was previously allocated to G2 task stacks is freed for BLE, WiFi, crypto, and PSRAM-incapable buffers.

This is conservative — the real win is that the "if internal DRAM allows" fallbacks at [G2_Page_TestSuite.cpp:1013](../components/hardwareone/G2_Page_TestSuite.cpp:1013), [G2_Page_Network.cpp:756](../components/hardwareone/G2_Page_Network.cpp:756), [G2_Page_TestSuite.cpp:517](../components/hardwareone/G2_Page_TestSuite.cpp:517), [G2_Glasses.cpp:5330](../components/hardwareone/G2_Glasses.cpp:5330) all go away, removing a class of "feature works on a fresh boot, fails after running for an hour" bugs.

---

## 8. Migration plan (phased, risk-ordered)

Each milestone is independently shippable. Do not bundle.

| # | Milestone | Scope | Risk | Visible to user? |
|---|---|---|---|---|
| M1 | Unified lens applier | New `LensUiJob`, rename worker, replace `gPageSwapQueue`, fold `notifyClearTaskBody` + live workers into jobs | Low — same producers, same consumer pattern | No |
| M2 | menuGen / cmdSeq infra | Add counters, cookie helper, `g2SubmitHijackCommand`, new `ORIGIN_G2_HIJACK` enum value | Low — additive only | No |
| M3 | Tap handler migration | Per-page, mutation leaves → commands. Page order: Files, Sensors, CameraSettings, Settings, Network, Power | Medium — every settings toggle, every WiFi op | Yes (UX must match) |
| M3.5 | Drop `tap_disp` stack to 6 KB | After M3 lands and bakes | Low | No |
| M4 | Connect/reconnect collapse | g2/ring connect variants → commands | Medium — connect flow is fragile | Yes |
| M5 | Media worker pool | bmp/camera workers → `g2_media` | Medium — streaming has timing concerns | Yes (streaming smoothness) |
| M6 | Test/AI/scan workers | ai/imgprobe/wifiscan/watchdog/hijack-bootstrap | Low–Medium | Mostly no |

Acceptance per milestone: existing G2 hijack flows pass on physical hardware (no regression), `idf.py size` shows expected stack reduction, no new "stack overflow in task X" panics in stress runs.

---

## 9. What stays as-is and why

- **`handleDevEvent` → `tapDispatcherEnqueue` non-blocking path.** This is load-bearing: BTC has a small stack and Bluedroid spinlocks make the temple notify path fundamentally unsafe for any allocation. The comment block at [G2_Glasses.cpp:7410-7437](../components/hardwareone/G2_Glasses.cpp:7410) is a primary source; do not "simplify" this path.
- **`fsmWorkerTask` and `gFsmQueue`.** Already a clean queue/worker. Hijack FSM transitions are not commands; they are state-machine internal work.
- **Per-temple `writeMutex`.** BLE write serialization per device is correct; do not centralize it.
- **`heartbeatWorkerTask`.** Outside this refactor's scope. If we later move heartbeat to a timer-driven job emitter, that's a separate proposal.
- **Ring spoof and ring-bridge heartbeat.** Loops with flags, legitimately persistent, low cost.

---

## 10. Open questions / decisions needed

These need a human (you) to decide before the refactor proceeds.

1. **Auth model for `ORIGIN_G2_HIJACK`.** Hijack actions have no logged-in user. Options:
   - (a) Reuse `ORIGIN_SYSTEM` semantics (full-trust, no audit user). Simplest.
   - (b) Define an `AuthContext` with user = `"g2_hijack"`, transport = `"g2"`, path = `/g2/hijack/<page>/<idx>`. Cleaner audit trail, requires one struct field.
   - **Recommendation:** (b). Three lines of code, much better forensics.

2. **Optimistic UI vs. pessimistic UI per subsystem.** When a user taps "Connect to FooNet":
   - **Optimistic:** immediately render "Connecting to FooNet…" then correct on completion. Snappy but can lie.
   - **Pessimistic:** render "Pending…" until completion, then real status. Honest but feels slow.
   - **Recommendation:** optimistic for fast ops (settings toggles ~50 ms), pessimistic for slow ops (WiFi connect, BLE pair). Codified per command.

3. **Cancellation semantics.** When the user navigates away from a page that has a command in flight:
   - (a) Drop the result (cooperative). Easy. Command still completes server-side.
   - (b) Hard-cancel (signal the executor). Hard, requires per-command cancel tokens.
   - **Recommendation:** (a) for now. Add (b) only when a specific user-visible bug demands it.

4. **Lens output mask.** Add `CMD_OUT_LENS` bit, or keep "lens is callback-only, not a mask"?
   - **Recommendation:** callback-only. The mask is for *human-readable text*; lens output is structured (page swap, redraw, toast). Conflating them encourages bad shortcuts.

5. **Subsystem latest-wins or strict-serial?**
   - Latest-wins (my preference): drop older callback if a newer cmdSeq for the same subsystem has already completed.
   - Strict-serial: queue duplicate taps, reject only if the queue is full.
   - **Recommendation:** latest-wins for WiFi/BLE-pair (state-converging), strict-serial for file ops (each one matters). Per-subsystem policy.

6. **Do we need a second queue inside `cmd_exec_task` for interactivity?**
   - Only if M3 lands and we observe lens lag during automation runs. **Recommendation:** ship M3 first, measure, then decide.

---

## 11. Non-goals

So that the next person doesn't expand this into a year-long project:

- **Not** rewriting the G2 protocol (`System_G2_Protocol.h`).
- **Not** changing the `cmd_exec_task` worker itself (only adding a new `CommandOrigin`).
- **Not** touching the hijack FSM.
- **Not** changing how `handleDevEvent` decodes BLE notifications.
- **Not** redesigning settings persistence; we use whatever CLI command already does it.

---

## 12. TL;DR for whoever is reading this cold

The G2 stack accumulated 19 distinct task-spawn patterns because each feature was added with `xTaskCreate`. The firmware already has `cmd_exec_task` + `submitCommandAsync` for exactly this purpose, used by Web, Serial, and Phone-BLE. Move every G2 system-mutation through that pipe; keep a small dedicated worker for lens UI navigation and a single `lens_applier` task that owns all output to the glasses; gate every async update with a `menuGen`/`cmdSeq` cookie so a slow command can't redraw a stale screen. Net: ~14 KB persistent and ~25 KB peak transient stack reclaimed, ~17 one-shot patterns collapsed into 3 typed workers, one queue per concern, and the glasses become first-class citizens of the same command pipeline as everything else.
