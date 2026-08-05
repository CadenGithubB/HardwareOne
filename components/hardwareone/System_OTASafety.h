#pragma once

// Main-application OTA rollback probation.
//
// These hooks deliberately carry no updater command or NVS request schema. They
// only protect the first boot of an ESP-IDF OTA image while its state is
// ESP_OTA_IMG_PENDING_VERIFY (or the short-lived NEW state).

// Call at the very start of app_main(), before initArduino(). Detects whether
// the running image is unverified and, only in that case, starts the independent
// probation supervisor that can recover setup/loop hangs by rebooting.
void otaSafetyInitEarly();

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
