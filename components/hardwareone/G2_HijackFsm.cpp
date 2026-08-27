// =============================================================================
// G2 Hijack FSM — implementation
// =============================================================================
// See header for the full plan and rationale. This file implements:
//   * The transition table (small switch — nine events, five states).
//   * A FreeRTOS queue + worker task (Phase 4): producers enqueue and
//     return immediately, the worker drains and applies. Single-writer
//     discipline means readers need no lock.
//   * Lens-mirror side effects (Phase 5): the worker calls
//     g2LensApplyHijackActive / g2LensApplyContainer to keep the gLens
//     state struct in sync with FSM transitions.
//   * Single line per transition + illegal-event in the debug log so
//     device traces can be diffed against expected operation.
//
// The FSM is intentionally permissive: an "illegal" event in the current
// state is logged but never crashes or drops state. Phase 6 retired the
// verifyFlags / HijackFsmFlags machinery — the legacy globals it
// compared against (gPageSwapActive / gHijackActive) no longer exist.
#include "G2_HijackFsm.h"
#include "System_TaskUtils.h"  // APP_CORE / PRO_CORE task-placement constants

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "System_Debug.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Phase 5 hooks. Provided by G2_Glasses.cpp. The FSM worker is the
// only caller — these functions perform the raw gLens.* writes that
// used to happen inline at the dispatch site. Forward-declared here
// rather than including G2_Glasses.h to keep the FSM TU's include
// surface minimal.
extern void g2LensApplyHijackActive(bool active);
extern void g2LensApplyContainer(bool ready, bool isList, uint32_t widgetId);

namespace {

volatile HijackState gFsmState = HijackState::Idle;

const char* stateName(HijackState s) {
  switch (s) {
    case HijackState::Idle:         return "Idle";
    case HijackState::Hijacked:     return "Hijacked";
    case HijackState::PageSwapping: return "PageSwapping";
    case HijackState::ImageProbing: return "ImageProbing";
    case HijackState::PluginDead:   return "PluginDead";
  }
  return "?";
}

const char* eventName(HijackEvent e) {
  switch (e) {
    case HijackEvent::HijackEnter:      return "HijackEnter";
    case HijackEvent::HijackExit:       return "HijackExit";
    case HijackEvent::PageSwapBegin:    return "PageSwapBegin";
    case HijackEvent::PageSwapEnd:      return "PageSwapEnd";
    case HijackEvent::ShutdownSent:     return "ShutdownSent";
    case HijackEvent::ContainerCreated: return "ContainerCreated";
    case HijackEvent::ContainerCleared: return "ContainerCleared";
    case HijackEvent::ImageProbeBegin:  return "ImageProbeBegin";
    case HijackEvent::ImageProbeEnd:    return "ImageProbeEnd";
  }
  return "?";
}

// Compute the next state for (current, event). Returns the same state
// when the event is a no-op in the current state. Sets *legal=false when
// the event is unexpected (logged separately by the caller).
HijackState nextState(HijackState s, HijackEvent e, bool& legal) {
  legal = true;
  switch (s) {
    case HijackState::Idle:
      switch (e) {
        case HijackEvent::HijackEnter:      return HijackState::Hijacked;
        case HijackEvent::PageSwapBegin:    return HijackState::PageSwapping;
        // ShutdownSent in Idle: image probes use the page-swap shutdown
        // path even when no hijack is in progress (e.g. a CLI-fired probe
        // tearing down a stale container). Tolerate without state change.
        case HijackEvent::ShutdownSent:     return s;
        // ContainerCleared in Idle: idempotent (DISPLAY_OFF echo, etc.).
        case HijackEvent::ContainerCleared: return s;
        // ContainerCreated in Idle is suspicious — it would mean a CREATE
        // outside any tracked lifecycle. Log but stay.
        case HijackEvent::ContainerCreated: legal = false; return s;
        case HijackEvent::HijackExit:       legal = false; return s;
        case HijackEvent::PageSwapEnd:      legal = false; return s;
        // Probes from cold-Idle (e.g. CLI-fired without hijack) — allow,
        // but log so we notice. Most probe entries come from Hijacked.
        case HijackEvent::ImageProbeBegin:  return HijackState::ImageProbing;
        // ImageProbeEnd in Idle is the "probe worker finished after we
        // already forced-tore-down" path — e.g. BLE wedge disconnect drove
        // ImageProbing → Idle via HijackExit while the probe worker was
        // mid-fragment. The deferred ImageProbeEnd then lands here. Benign.
        case HijackEvent::ImageProbeEnd:    return s;
      }
      break;

    case HijackState::Hijacked:
      switch (e) {
        case HijackEvent::PageSwapBegin:    return HijackState::PageSwapping;
        case HijackEvent::HijackExit:       return HijackState::Idle;
        // ContainerCleared while Hijacked is NOT an exit signal. The
        // probe pre-burst tear-down path (probeTearDownActiveContainer)
        // calls g2LensClearContainer() to drop the cached container
        // shape before the probe re-CREATEs an image-shaped one — the
        // hijack itself stays active throughout. Real hijack exits
        // always pair this with g2LensSetHijackActive(false), which
        // fires HijackExit and drops us to Idle.
        case HijackEvent::ContainerCleared: return s;
        // ContainerCreated while Hijacked — happens on subsequent
        // g2ShowText/CREATE that doesn't go through page-swap (e.g.
        // direct Status snapshot push). Tolerate.
        case HijackEvent::ContainerCreated: return s;
        case HijackEvent::ShutdownSent:     return s;
        case HijackEvent::HijackEnter:      legal = false; return s;
        case HijackEvent::PageSwapEnd:      legal = false; return s;
        // Phase 3: probe lifecycle is now its own state. Entry from
        // Hijacked (the common path — user taps a probe in the test
        // menu).
        case HijackEvent::ImageProbeBegin:  return HijackState::ImageProbing;
        // Duplicate / deferred: worker may emit ImageProbeEnd after we are
        // already Hijacked (BLE wedge + disconnect, or shutdown race).
        case HijackEvent::ImageProbeEnd:    return s;
      }
      break;

    case HijackState::PageSwapping:
      switch (e) {
        // PageSwapEnd is the "worker is done" signal. Default assumption:
        // success → Hijacked. If the swap actually failed, the next
        // HijackExit / ContainerCleared event corrects us.
        case HijackEvent::PageSwapEnd:      return HijackState::Hijacked;
        case HijackEvent::HijackExit:       return HijackState::Idle;
        // Everything else is expected mid-swap and doesn't change state.
        case HijackEvent::ShutdownSent:     return s;
        case HijackEvent::ContainerCreated: return s;
        case HijackEvent::ContainerCleared: return s;
        case HijackEvent::PageSwapBegin:    legal = false; return s;
        case HijackEvent::HijackEnter:      legal = false; return s;
        // A probe starting mid-page-swap would be a bug — page-swap
        // worker is mutating the lens; probe would race it.
        case HijackEvent::ImageProbeBegin:  legal = false; return s;
        case HijackEvent::ImageProbeEnd:    legal = false; return s;
      }
      break;

    case HijackState::ImageProbing:
      switch (e) {
        // Probe finished — return to Hijacked (probe was launched from
        // there and the post-probe rebuild restores the menu) or Idle
        // (probe was launched from cold; nothing to restore).
        case HijackEvent::ImageProbeEnd:    return HijackState::Hijacked;
        // Any of these are expected mid-probe: the probe itself sends
        // intentional Cmd=9 SHUTDOWNs (ShutdownSent), reshapes the
        // container (ContainerCleared / ContainerCreated), etc.
        case HijackEvent::ShutdownSent:     return s;
        case HijackEvent::ContainerCreated: return s;
        case HijackEvent::ContainerCleared: return s;
        // BLE drop / explicit hijack exit cancels the probe.
        case HijackEvent::HijackExit:       return HijackState::Idle;
        // Probes don't go through page-swap, and shouldn't nest.
        case HijackEvent::PageSwapBegin:    legal = false; return s;
        case HijackEvent::PageSwapEnd:      legal = false; return s;
        case HijackEvent::HijackEnter:      legal = false; return s;
        case HijackEvent::ImageProbeBegin:  legal = false; return s;
      }
      break;

    case HijackState::PluginDead:
      // Not entered yet (Phase 7 wires plugin-dead detection). Leave the
      // branch in place so adding the events later is a single edit.
      legal = false;
      return s;
  }
  legal = false;
  return s;
}

// ─── Mailbox / worker (Phase 4) ──────────────────────────────────────────
// Sites posting events from any task context — including the BLE notify
// task on SYSTEM_EXIT / DISPLAY_OFF echoes — push to gFsmQueue and return
// immediately. The worker task drains and applies. Producers never take
// a mutex, so a notify-task post can never block on a slower task that
// happens to be running through the transition table.
//
// Tag strings are copied into the queue entry so callers may pass a
// transient pointer (in practice all current sites pass string literals;
// the copy is cheap insurance for future callers).

struct FsmEvent {
  HijackEvent        ev;
  HijackEventPayload payload;
  bool               hasPayload;
  char               tag[24];
};

constexpr UBaseType_t kFsmQueueDepth = 32;

QueueHandle_t gFsmQueue = nullptr;
TaskHandle_t  gFsmTaskHandle = nullptr;
volatile uint32_t gFsmDropped = 0;

void copyTag(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  // The tag's only consumers are DEBUG_G2F lines in applyEvent — skip the
  // copy under the same gate (applyEvent prints "?" for an empty tag).
  if (!(getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_G2))) {
    dst[0] = '\0';
    return;
  }
  if (!src) { dst[0] = '\0'; return; }
  size_t n = 0;
  while (src[n] && n + 1 < dstSize) { dst[n] = src[n]; ++n; }
  dst[n] = '\0';
}

void applyEvent(const FsmEvent& e) {
  const HijackState before = gFsmState;
  bool legal = true;
  const HijackState after = nextState(before, e.ev, legal);
  const char* tag = e.tag[0] ? e.tag : "?";

  if (after != before) {
    gFsmState = after;
    DEBUG_G2F("[FSM] %s --%s--> %s (@%s)",
              stateName(before), eventName(e.ev), stateName(after), tag);
  } else if (!legal) {
    DEBUG_G2F("[FSM] illegal: %s in %s (@%s)",
              eventName(e.ev), stateName(before), tag);
  }
  // Else: legal no-op; don't spam the log.

  // Phase 5: apply lens-mirror side effects. Done unconditionally on
  // every dispatch (not just transitions) — a duplicate ContainerCreated
  // that arrives while we're already Hijacked needs to update the lens
  // mirror so the new widgetId/isList replace the old. Same for hijack
  // toggles. Worker-only writes mean no race with other tasks; this is
  // the single point where gLens.container* and gLens.hijack* change.
  switch (e.ev) {
    case HijackEvent::HijackEnter:
      g2LensApplyHijackActive(true);
      break;
    case HijackEvent::HijackExit:
      g2LensApplyHijackActive(false);
      // Hijack ended → no live container. Mirror that here so the next
      // status snapshot doesn't claim a stale ready=true.
      g2LensApplyContainer(false, false, 0);
      break;
    case HijackEvent::ContainerCreated:
      if (e.hasPayload) {
        g2LensApplyContainer(true, e.payload.isList, e.payload.widgetId);
      } else {
        // Defensive: a payload-less ContainerCreated shouldn't happen
        // (only g2LensSetContainer dispatches this event, always with
        // payload). Log so we notice if a future caller forgets.
        DEBUG_G2F("[FSM] ContainerCreated without payload @%s — lens not updated",
                  tag);
      }
      break;
    case HijackEvent::ContainerCleared:
      g2LensApplyContainer(false, false, 0);
      break;
    case HijackEvent::PageSwapEnd:
    case HijackEvent::ImageProbeEnd:
      // Safety-timeout / DISPLAY_OFF exit via HijackExit, which clears
      // gLens.hijackActive. The Health (etc.) onDone path then rebuilds
      // Apps with PageSwapBegin from Idle → PageSwapEnd → Hijacked.
      // Without restoring the lens mirror here, page workers that gate
      // on gLens.hijackActive CREATE successfully then abort on their
      // first loop tick ("hijack ended") — flash of Health then bounce
      // to Apps. ImageProbeEnd → Hijacked gets the same restore for
      // symmetry (idempotent when the mirror is already true).
      if (after == HijackState::Hijacked) {
        g2LensApplyHijackActive(true);
      }
      break;
    default:
      // PageSwapBegin, ShutdownSent, ImageProbeBegin: no lens-mirror
      // side effects.
      break;
  }
}

void fsmWorkerTask(void*) {
  FsmEvent e;
  uint32_t lastDroppedSeen = 0;
  for (;;) {
    if (xQueueReceive(gFsmQueue, &e, portMAX_DELAY) == pdTRUE) {
      applyEvent(e);
      const uint32_t dropped = __atomic_load_n(&gFsmDropped, __ATOMIC_RELAXED);
      if (dropped != lastDroppedSeen) {
        DEBUG_G2F("[FSM] queue full — %u event(s) dropped (cumulative)",
                  (unsigned)dropped);
        lastDroppedSeen = dropped;
      }
    }
  }
}

void postEvent(const FsmEvent& e) {
  if (!gFsmQueue) {
    // Pre-init dispatch — apply inline. The device is single-threaded at
    // this point (BLE / page-swap workers haven't started), so this
    // matches the post-init worker semantics.
    applyEvent(e);
    return;
  }
  if (xQueueSend(gFsmQueue, &e, 0) != pdPASS) {
    __atomic_add_fetch(&gFsmDropped, 1, __ATOMIC_RELAXED);
  }
}

}  // namespace

const char* hijackStateName(HijackState s) { return stateName(s); }
const char* hijackEventName(HijackEvent e) { return eventName(e); }
HijackState hijackFsmState() { return gFsmState; }

void hijackFsmInit() {
  if (gFsmQueue) return;
  gFsmQueue = xQueueCreate(kFsmQueueDepth, sizeof(FsmEvent));
  if (!gFsmQueue) {
    DEBUG_G2F("[FSM] queue alloc failed — dispatches will apply inline");
    return;
  }
  // Priority 5 matches the G2 control/connect workers and preempts the
  // priority-2 page/tap workers so lifecycle transitions drain promptly.
  // Stack is 4096 BYTES (4 KB). ESP-IDF's xTaskCreate takes usStackDepth in
  // BYTES, not words — see System_TaskUtils.h.
  //
  // Hardware validation on XIAO ESP32-S3 (2026-08-12) measured a 2280-byte
  // peak on the former 2560-byte allocation, leaving only 280 bytes (10.9%).
  // 4096 leaves 1816 bytes (44.3%) at that observed peak and costs 1536 bytes
  // of additional internal DRAM. Keep measuring on both ESP32-S3 and ESP32.
  const BaseType_t ok = xTaskCreateLogged(fsmWorkerTask, "g2-fsm",
                                    /*stack bytes*/ 4096,
                                    nullptr, 5, &gFsmTaskHandle,
                                    "g2.fsm", APP_CORE);
  if (ok != pdPASS) {
    DEBUG_G2F("[FSM] worker task create failed — dispatches will apply inline");
    vQueueDelete(gFsmQueue);
    gFsmQueue = nullptr;
    gFsmTaskHandle = nullptr;
  }
}

void hijackFsmDispatch(HijackEvent ev, const char* tag) {
  if (!gFsmQueue) hijackFsmInit();
  FsmEvent e = {};
  e.ev = ev;
  e.hasPayload = false;
  copyTag(e.tag, sizeof(e.tag), tag);
  postEvent(e);
}

void hijackFsmDispatch(HijackEvent ev, const char* tag,
                       const HijackEventPayload& payload) {
  if (!gFsmQueue) hijackFsmInit();
  FsmEvent e = {};
  e.ev = ev;
  e.payload = payload;
  e.hasPayload = true;
  copyTag(e.tag, sizeof(e.tag), tag);
  postEvent(e);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
