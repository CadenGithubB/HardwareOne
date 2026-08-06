#pragma once

// Main-application OTA rollback probation.
//
// These hooks deliberately carry no updater command or NVS request schema. They
// only protect the first boot of an ESP-IDF OTA image while its state is
// ESP_OTA_IMG_PENDING_VERIFY (or the short-lived NEW state).

#include <stddef.h>
#include <stdint.h>

// Why a trial image was rejected. All five of these previously reached the
// recovery updater as one generic `rollback_detected`, so an operator could see
// THAT an image was rolled back but never WHICH check rejected it - and the
// image itself is destroyed by the rollback, so there was nothing left to
// inspect. Each one implies a different next action:
//   SETUP_TIMEOUT      - the new firmware hung before finishing setup
//   LOOP_STALL         - the main loop stopped completing passes
//   HEALTH_TIMEOUT     - it ran, but never accumulated a healthy window
//   SUPERVISOR_ALLOC   - the supervisor task itself could not be created
//   JOURNAL_REFUSED    - the image was fine; the OTA journal was inconsistent
enum OtaProbationCause : uint8_t {
  OTA_PROBATION_NONE = 0,
  OTA_PROBATION_SETUP_TIMEOUT,
  OTA_PROBATION_LOOP_STALL,
  OTA_PROBATION_HEALTH_TIMEOUT,
  OTA_PROBATION_SUPERVISOR_ALLOC,
  OTA_PROBATION_JOURNAL_REFUSED,
};

const char* otaSafetyProbationCauseName(OtaProbationCause cause);

// Reads the breadcrumb left by the last probation abort, if any. Survives the
// reboot AND the trip through the recovery updater, so the main app can explain
// a rollback that happened on a previous boot. Returns false when no abort is
// recorded. Pass consume=true to clear it after reporting.
bool otaSafetyTakeProbationAbort(OtaProbationCause* cause, uint32_t* uptimeMs,
                                 char* detail, size_t detailSize, bool consume);

// Call at the very start of app_main(), before initArduino(). Detects whether
// the running image is unverified and, only in that case, starts the independent
// probation supervisor that can recover setup/loop hangs by rebooting.
void otaSafetyInitEarly();

// Accept the running image because it reached interactive first-time setup.
//
// firstTimeSetupIfNeeded() blocks inside hardwareone_setup() waiting for a
// human, so neither otaSafetySetupReachedRunning() nor the loop heartbeat can
// run until setup completes - and the 5-minute setup deadline was rolling back
// perfectly good images because an operator read the menu slowly.
//
// Reaching this point proves more than the loop probation would: image
// verified, storage mounted, settings loaded, serial interactive. And it can
// only happen on a cable-provisioned device, because an OTA-delivered image
// arrives on a filesystem that still holds users.json.
//
// CALLER MUST confirm the filesystem genuinely mounted. A broken image that
// lost its storage also reports "first-time setup required", and that case
// must keep its probation. Returns false if the image could not be marked.
bool otaSafetyAcceptProvisioningBoot();

// True only while the running OTA image is still unverified. This remains true
// while esp_ota_mark_app_valid_cancel_rollback() is in progress and becomes
// false only after that call succeeds.
bool otaSafetyIsPendingVerification();

// Destructive automatic storage recovery is always forbidden. Kept as a named
// policy hook so storage callers cannot silently reintroduce erase-on-error.
bool otaSafetyAllowsDestructiveStorageRecovery();

// Call once, immediately after HardwareOne advances its crash phase to RUNNING.
// The 60-second probation interval starts here, never from power-on uptime.
void otaSafetySetupReachedRunning();

// Call after a complete HardwareOne loop pass. The heartbeat keeps the pending-
// image supervisor alive. coreHealthy must cover the minimum services required
// to consider the application usable; false keeps the heartbeat alive but
// restarts the continuous healthy interval.
void otaSafetyLoopHeartbeat(bool coreHealthy);
