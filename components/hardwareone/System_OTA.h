#pragma once

#include <Arduino.h>

#include "System_Utils.h"

// Native ESP-IDF recovery OTA integration for the main HardwareOne image.
// The command surface is always registered so ordinary builds can explain why
// OTA is unavailable, but all mutating behavior is compiled out unless the
// opt-in recovery partition layout was selected with HW_OTA_LAYOUT=1.

// Called after crashRecordBootConsume(), while boot is still pre-Serial.  A
// repeatedly crashing accepted trial can be redirected to the immutable
// factory recovery image.  This is a no-op outside the OTA layout.
void otaSystemCrashLoopEscapeEarly();

// Emergency path used when a committed main image cannot mount LittleFS. It
// selects factory recovery and records a direct-upload request without ever
// formatting retained data. Returns false when recovery cannot be armed.
bool otaSystemRecoverFromStorageFailure();

// Called after LittleFS and the command system are available.  Reconciles a
// trial boot with the transaction journal and reports any unacknowledged result.
void otaSystemInitAfterStorage();

// Fail-closed journal check performed immediately before the ESP-IDF image is
// made permanent. A pending image without a coherent trial transaction must
// roll back rather than becoming valid with an unauditable OTA history.
bool otaSystemCanMarkImageValid();

// Called only after esp_ota_mark_app_valid_cancel_rollback() succeeds.  Commits
// the terminal transaction result and removes the staged image/manifest.
void otaSystemOnImageMarkedValid();

// Encrypted BLE bulk-staging sink. The secure-channel layer recognizes the
// binary envelope and calls this on cmd_exec_task. The implementation checks
// the live BLE identity on every frame and accepts data only after an
// authorized `otawrite begin` command established an exact transfer contract.
bool otaBleHandleEncryptedFrame(uint16_t connId, const uint8_t* frame, size_t size);

// Cheap main-loop cleanup for abandoned/disconnected BLE upload sessions.
void otaBleUploadHousekeeping();

extern const CommandEntry otaCommands[];
extern const size_t otaCommandsCount;
