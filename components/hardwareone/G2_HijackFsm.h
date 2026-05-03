// =============================================================================
// G2 Hijack lifecycle finite state machine
// =============================================================================
// The G2 hijack/page-swap state used to live across half a dozen flags
// and timestamps (gPageSwapActive, gOurShutdownAtMs, gHijackActive,
// per-temple containerReady/IsList, pluginDead, gLens.* mirror fields).
// Each was added to fix a specific bug, and the cross-product of "which
// flag is set when" became hard to reason about.
//
// Phases 1-6 of the FSM refactor consolidated that into this state
// machine. The FSM is now the single source of truth for "is the hijack
// live?" / "are we mid-swap?" / "is a probe running?". Reads go through
// hijackFsmState() (or the convenience inlines g2FsmHijackActive() /
// g2FsmPageSwapping() defined in G2_Glasses.cpp). Writes go through
// hijackFsmDispatch(), which posts to a single FSM worker task that
// applies transitions and the lens-mirror gLens.* writes.
//
// Phase status:
//   0. Extract pageSwapBegin/End, noteOurShutdownSent helpers (DONE).
//   1. Add this FSM as a shadow + verify (DONE).
//   2. Replace the 2-second wall-clock echo guard with FSM state checks
//      (DONE — gOurShutdownAtMs / ourShutdownEchoActive() remain as a
//      defensive fallback only).
//   3. Promote ImageProbing to a first-class state (DONE).
//   4. Move dispatches off the caller's task via a mailbox / FSM worker
//      task (DONE).
//   5. Make FSM authoritative for containerReady + lens-mirror writes
//      (DONE — the worker calls g2LensApply* via the apply path).
//   6. Delete gPageSwapActive / gHijackActive globals (DONE — predicates
//      replace them).
//   7. Promote PluginDead to a state.
//
// Single-writer concurrency model: only the FSM worker task mutates
// gFsmState. Producers post events to the queue; readers do a volatile
// load on a uint8_t (atomic on ESP32 / Xtensa). No mutex needed.
#pragma once

#include "System_BuildConfig.h"
#include <stdint.h>   // outside the gate — used by both #if and #else branches

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// ─── States ──────────────────────────────────────────────────────────────
// Phase 1 collapsed the eventual ShuttingDown / Creating distinction into
// a single PageSwapping state because the existing event sites couldn't
// disambiguate the boundary without adding new dispatch points. Phase 3
// promoted ImageProbing. PluginDead is declared here but not entered
// until Phase 7 wires its events.
enum class HijackState : uint8_t {
  Idle = 0,
  Hijacked,
  PageSwapping,
  ImageProbing,
  PluginDead,
};

// ─── Events ──────────────────────────────────────────────────────────────
// One event per chokepoint we already have. Each existing helper
// (pageSwapBegin/End, noteOurShutdownSent, g2LensSetHijackActive,
// g2LensSetContainer/Clear, imageProbeBegin/End) gets exactly one
// dispatch call.
enum class HijackEvent : uint8_t {
  HijackEnter,        // g2LensSetHijackActive(true)
  HijackExit,         // g2LensSetHijackActive(false)
  PageSwapBegin,      // pageSwapBegin()
  PageSwapEnd,        // pageSwapEnd()
  ShutdownSent,       // noteOurShutdownSent()
  ContainerCreated,   // g2LensSetContainer(ready=true, ...)
  ContainerCleared,   // g2LensClearContainer()
  ImageProbeBegin,    // imageProbeBegin() — pre-burst SHUTDOWN site
  ImageProbeEnd,      // imageProbeEnd() — post-probe SHUTDOWN site
};

// Payload for events that need to communicate state to the FSM worker.
// Phase 5 made the worker the single writer of `gLens.container*`; the
// CREATE site supplies the new container shape via this payload so the
// worker can do the write itself instead of the caller racing it.
struct HijackEventPayload {
  bool     isList;     // ContainerCreated: was the new widget CREATEd as a list?
  uint32_t widgetId;   // ContainerCreated: firmware widgetId we tagged the CREATE with.
};

const char* hijackStateName(HijackState s);
const char* hijackEventName(HijackEvent e);

// Lazily create the event queue + worker task. Call once during G2 init
// (idempotent). Dispatches that occur before the queue is up apply
// in-line — the device is still single-threaded at that point so the
// behaviour matches the post-init worker.
void hijackFsmInit();

// Read current state. Single-writer model: only the FSM worker task
// mutates gFsmState; readers do a plain volatile load. Safe from any
// task context.
HijackState hijackFsmState();

// Enqueue an event for the FSM worker task to apply. Non-blocking — if
// the queue is full the event is dropped and counted (logged at the next
// successful drain). Tag identifies the call site in the log
// (e.g. "g2ShowListPage", "pageSwapWorker.cleanup") and is copied into
// the queue entry, so the caller may pass a non-static pointer.
void hijackFsmDispatch(HijackEvent ev, const char* tag);

// Payload-carrying overload — used by ContainerCreated callers that need
// to communicate the new container's shape so the FSM worker can apply
// the lens-mirror write itself (Phase 5).
void hijackFsmDispatch(HijackEvent ev, const char* tag,
                       const HijackEventPayload& payload);

#else  // !ENABLE_BLUETOOTH || !ENABLE_G2_GLASSES

enum class HijackState : uint8_t { Idle = 0 };
enum class HijackEvent : uint8_t { HijackEnter = 0 };
struct HijackEventPayload {
  bool     isList;
  uint32_t widgetId;
};

inline const char* hijackStateName(HijackState) { return "Idle"; }
inline const char* hijackEventName(HijackEvent) { return ""; }
inline void hijackFsmInit() {}
inline HijackState hijackFsmState() { return HijackState::Idle; }
inline void hijackFsmDispatch(HijackEvent, const char*) {}
inline void hijackFsmDispatch(HijackEvent, const char*,
                              const HijackEventPayload&) {}

#endif
