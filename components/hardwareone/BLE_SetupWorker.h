#pragma once
// The one BLE setup worker (`ble_setup`). Every central setup runs on its
// turn: glasses, then ring retirement, then ring, then phone, and a job that
// was passed over once goes next. A setup in progress is never cancelled for
// another client's. The scheduler is BLE_SetupWorkerCore.h; each client
// registers an ops table in its own file (docs/BLE_SETUP_WORKER_PLAN.md).
#include "System_BuildConfig.h"
#include "BLE_SetupWorkerCore.h"
#include <cstdint>

#define HW1_BLE_SETUP_WORKER (ENABLE_BLUETOOTH && (ENABLE_G2_GLASSES || ENABLE_BLE_PHONE_PIPE))

using BleSetupClient = hw1setup::Client;
enum class BleSetupWithdraw : uint8_t { Withdrawn, Running, NotLive };

// A client's callbacks. ready() runs on the worker without any lock and must
// not log, allocate, call BLE or touch what teardown frees; waiting() is told
// each time the job's wait reason changes (to log it); run() runs on the
// worker; finish() runs exactly once per token, on the worker or on the task
// that withdrew it, and releases the client's own in-flight claim as its last
// step.
struct BleSetupOps {
  const char* (*label)(uint32_t token);
  hw1setup::Verdict (*ready)(uint32_t token, const hw1setup::WaitInfo& info);
  void (*waiting)(uint32_t token, uint8_t reason);
  hw1setup::End (*run)(uint32_t token, const hw1setup::RunInfo& info);
  void (*finish)(uint32_t token, const hw1setup::FinishInfo& info);
  bool recordWait;  // put the wait on the job's connect records (glasses, ring)
};

#if HW1_BLE_SETUP_WORKER
// Starts the task (task context; allocates its stack). True while it runs,
// false while a release is in progress; a released worker starts again here.
bool bleSetupWorkerStart();
// Ends the task cooperatively and frees its stack: no new claim is taken, a
// job in progress finishes (up to timeoutMs, never forced), then the worker
// nulls its handle and deletes itself. True once no worker exists (also with
// none started); false, with the worker kept and claims open again, on a
// timeout, while it is not ready, or from the worker itself. Task context,
// never the worker's own.
bool bleSetupWorkerRelease(uint32_t timeoutMs);
// Submit: claim the slot, write the client's own payload, publish. None of
// these blocks or allocates. Claim fails while the slot is in use or the
// worker is not running.
bool bleSetupClaim(BleSetupClient client, uint32_t* token);
bool bleSetupPublish(BleSetupClient client, uint32_t token);
void bleSetupCancelClaim(BleSetupClient client, uint32_t token);
// Takes a queued job back (token 0: whatever is queued) and runs its finish
// on the calling task. A running job is not touched. Never from BTC_TASK.
BleSetupWithdraw bleSetupWithdraw(BleSetupClient client, uint32_t token);
bool bleSetupLive(BleSetupClient client, uint32_t token);
// No job of the masked clients is claimed, queued, running or finishing.
bool bleSetupIdle(uint8_t mask);
// Polls bleSetupIdle every 5 ms up to timeoutMs. On the worker itself it only
// checks. Never forced and never refused.
bool bleSetupWaitIdle(uint8_t mask, uint32_t timeoutMs);
// A glasses or ring job (retirement included) is queued or running.
bool bleSetupAccessoryBusy();
bool bleSetupOtherQueued(BleSetupClient self);
void bleSetupWake();
// The worker's own sleep, for a phone turn (worker task only).
void bleSetupWaitWake(uint32_t ms);
#else
inline bool bleSetupWorkerStart() { return false; }
inline bool bleSetupWorkerRelease(uint32_t) { return true; }
inline bool bleSetupClaim(BleSetupClient, uint32_t*) { return false; }
inline bool bleSetupPublish(BleSetupClient, uint32_t) { return false; }
inline void bleSetupCancelClaim(BleSetupClient, uint32_t) {}
inline BleSetupWithdraw bleSetupWithdraw(BleSetupClient, uint32_t) { return BleSetupWithdraw::NotLive; }
inline bool bleSetupLive(BleSetupClient, uint32_t) { return false; }
inline bool bleSetupIdle(uint8_t) { return true; }
inline bool bleSetupWaitIdle(uint8_t, uint32_t) { return true; }
inline bool bleSetupAccessoryBusy() { return false; }
inline bool bleSetupOtherQueued(BleSetupClient) { return false; }
inline void bleSetupWake() {}
inline void bleSetupWaitWake(uint32_t) {}
#endif
