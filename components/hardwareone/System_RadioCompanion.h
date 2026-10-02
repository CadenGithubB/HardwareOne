/**
 * System_RadioCompanion - the radio co-processor as a managed subsystem.
 *
 * On the ESP32-P4X-EYE the Wi-Fi/BLE/ESP-NOW radio is an ESP32-C6 behind
 * ESP-Hosted (see HAL_Radio.h). This module makes it behave like every other
 * subsystem: it has status (`c6status`), System Events the notification
 * system can route, settings, a restart command, a firmware update command,
 * a hold-in-reset command, and a background health monitor that retries a
 * companion that never came up, recovers one that stops answering, and
 * escalates to a reboot only when soft recovery keeps failing.
 *
 * Boards with an on-chip radio compile every entry point to a no-op.
 */
#ifndef SYSTEM_RADIO_COMPANION_H
#define SYSTEM_RADIO_COMPANION_H

#include <stddef.h>
#include <stdint.h>
#include "HAL_Radio.h"

#if HW1_RADIO_COMPANION

#include <ArduinoJson.h>
class String;
struct CommandEntry;
struct SettingsModule;

// Boot (from setup(), before any radio feature starts): subscribe to the
// HAL's events, apply the persisted monitoring settings, record how the
// transport came up and post the boot summary events.
void radioCompanionBootInit();
// One line per fact after setup(), for the boot log.
void radioCompanionLogBootSummary();
// Main-loop tick: drains events, runs the health monitor, dispatches work.
void radioCompanionTick();

// True when the companion is online and usable; the command module's
// "connected" predicate.
bool radioCompanionOnline();
// Whether a radio feature may start right now. Returns true with a reason when
// the companion is offline, starting, recovering or updating. A held companion
// is released and started first (blocking for a few seconds) and then allowed.
bool radioCompanionBlocksRadio(const char** reason);

// Power hooks, called by the sleep commands.
void radioCompanionBeforeLightSleep();
void radioCompanionAfterLightSleep();
void radioCompanionBeforeDeepSleep();

// Status for /api/system and the dashboard.
void radioCompanionJson(JsonObject out);

extern const CommandEntry companionCommands[];
extern const size_t companionCommandsCount;
extern const SettingsModule companionSettingsModule;

#else  // native radio

inline void radioCompanionBootInit() {}
inline void radioCompanionLogBootSummary() {}
inline void radioCompanionTick() {}
inline bool radioCompanionOnline() { return true; }
inline bool radioCompanionBlocksRadio(const char** reason) {
  if (reason) *reason = nullptr;
  return false;
}
inline void radioCompanionBeforeLightSleep() {}
inline void radioCompanionAfterLightSleep() {}
inline void radioCompanionBeforeDeepSleep() {}

#endif  // HW1_RADIO_COMPANION
#endif  // SYSTEM_RADIO_COMPANION_H
