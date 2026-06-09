// Background-poll pause primitive — see System_PollPause.h for the rationale.
//
// Single owner of the pause-depth counters and the gSensorPollingPaused mirror.
// Two independent, nesting depth counters: one blanket counter (pauses every
// bus) and one per-bus counter each. A short portMUX spinlock makes the inc/dec
// + mirror update atomic across both cores. The depth ints are volatile so the
// lock-free reads in pollPaused() (called from the hot poll loops) always see
// fresh values; writes are serialized under the mux.

#include "System_PollPause.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Coarse mirror of (any pause active). Lives here so the whole primitive —
// state + ops — is one translation unit. Zero-init, no constructor → no
// static-initialization-order concerns.
volatile bool gSensorPollingPaused = false;

static portMUX_TYPE sPauseMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int sAllDepth = 0;                      // blanket pause depth
static volatile int sBusDepth[POLL_NUM_BUSES] = { 0 };  // per-bus pause depth

// Recompute the coarse mirror. Caller must hold sPauseMux.
static inline void refreshMirror() {
  bool any = (sAllDepth > 0);
  for (uint8_t b = 0; !any && b < POLL_NUM_BUSES; b++) {
    if (sBusDepth[b] > 0) any = true;
  }
  gSensorPollingPaused = any;
}

void pollPause(uint8_t bus) {
  portENTER_CRITICAL(&sPauseMux);
  if (bus < POLL_NUM_BUSES) sBusDepth[bus]++;
  else                      sAllDepth++;   // POLL_BUS_ALL / out-of-range → blanket
  refreshMirror();
  portEXIT_CRITICAL(&sPauseMux);
}

void pollResume(uint8_t bus) {
  portENTER_CRITICAL(&sPauseMux);
  if (bus < POLL_NUM_BUSES) { if (sBusDepth[bus] > 0) sBusDepth[bus]--; }
  else                      { if (sAllDepth > 0)      sAllDepth--; }
  refreshMirror();
  portEXIT_CRITICAL(&sPauseMux);
}

bool pollPaused(uint8_t bus) {
  // Lock-free: each read is a single aligned volatile int (atomic on this MCU).
  // A blanket pause masks every bus; otherwise consult the specific bus.
  if (sAllDepth > 0) return true;
  if (bus < POLL_NUM_BUSES) return sBusDepth[bus] > 0;
  return gSensorPollingPaused;   // unknown bus → coarse "anything paused"
}
