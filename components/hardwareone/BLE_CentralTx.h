/**
 * BLE Central TX gate — controller-level write serialization.
 *
 * G2 temples (L/R) and the R1 ring each have their own per-peer write mutex,
 * but they share one BT controller. Concurrent writeValue() calls from
 * different links saturate esp_ble_gattc_write_char (rc=-1) mid image-burst.
 *
 * Hold for one envelope / short write only — release between fragments so
 * heartbeats and queued ring TX can run in the paced gaps. Ring TX that
 * cannot take the gate is enqueued (see g2RingTryDrainPendingTx).
 *
 * Lock order (hard rule — never reverse):
 *   1. bleCentralTx
 *   2. peer-local mutex (G2Temple::writeMutex or ring writeMutex)
 */

#pragma once

#include <stdint.h>

// Create the mutex once. Idempotent; safe to call from G2 and ring init.
void bleCentralTxInit();

// Take / give the controller TX gate. timeoutMs=0 → try once (no wait).
bool bleCentralTxTake(uint32_t timeoutMs);
void bleCentralTxGive();

// True if some task currently holds the gate (including the caller).
bool bleCentralTxIsHeld();

// True if a *different* task holds the gate (heartbeat skip-when-busy).
bool bleCentralTxIsHeldByOther();

// RAII helper — takes on construct, gives on destruct if take succeeded.
struct BleCentralTxGuard {
  bool held;
  explicit BleCentralTxGuard(uint32_t timeoutMs);
  ~BleCentralTxGuard();
  BleCentralTxGuard(const BleCentralTxGuard&) = delete;
  BleCentralTxGuard& operator=(const BleCentralTxGuard&) = delete;
  explicit operator bool() const { return held; }
};
