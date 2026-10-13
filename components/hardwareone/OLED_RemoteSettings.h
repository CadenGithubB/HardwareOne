#ifndef OLED_REMOTESETTINGS_H
#define OLED_REMOTESETTINGS_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include <Arduino.h>
#include "System_Settings.h"

// Remote settings mode - edits the bonded peer's settings on the OLED.
// The module set is built from the peer's cached schema.json (only rows that
// carry a setter command, are neither read-only nor secret, and pass
// BondedPeer::remoteEditAllowed), with current values from the cached
// settings.json. The shared settings editor renders it; a commit sends the
// row's schema setter to the peer and the outcome is the peer's CMD_RESP.

// Non-blocking load: call once per frame until READY or FAILED. IDLE builds
// from the cached schema+settings, or (no schema cached) starts one schema
// sync -> SYNCING until the file lands or REMOTE_SCHEMA_SYNC_MS elapses.
// READY/FAILED stay until freeRemoteSettingsModules().
enum RemoteSettingsLoadState {
  REMOTE_LOAD_IDLE,
  REMOTE_LOAD_SYNCING,
  REMOTE_LOAD_READY,
  REMOTE_LOAD_FAILED
};
RemoteSettingsLoadState remoteSettingsLoadStep();

// Why the last load failed ("" if none).
const char* remoteSettingsLastError();

// Synchronous build from the caches (used by remoteSettingsLoadStep).
bool loadRemoteSettingsModules();

// Free the module set; also drops the pending commit and resets the load state.
void freeRemoteSettingsModules();

// The peer's module set (same shape as getSettingsModules).
const SettingsModule** getRemoteSettingsModules(size_t& count);

// Send the entry's schema setter to the bonded peer through the authorized OLED
// command path. True = accepted for delivery and now pending; false = not sent
// (errOut: "No command", "Empty value", "Busy" or the executor's error text).
// The peer's answer arrives through remoteSettingsPollOutcome().
bool applyRemoteSettingChange(const SettingEntry* entry, const String& value, String& errOut);

// Outcome of the pending commit, consumed once. APPLIED also writes the value
// into the entry's cached slot so the list shows it. textOut: the peer's text
// (APPLIED/FAILED) or "" (TIMEOUT after REMOTE_SET_REPLY_TIMEOUT_MS).
enum RemoteSettingsOutcome {
  REMOTE_OUTCOME_NONE,
  REMOTE_OUTCOME_PENDING,
  REMOTE_OUTCOME_APPLIED,
  REMOTE_OUTCOME_FAILED,
  REMOTE_OUTCOME_TIMEOUT
};
RemoteSettingsOutcome remoteSettingsPollOutcome(String& textOut);

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#endif // OLED_REMOTESETTINGS_H
