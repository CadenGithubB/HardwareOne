// System_Versions.h — `versions`: the firmware version and the versions at
// every boundary where something built separately must agree with it (the
// recovery updater, ESP-NOW peers, the phone app, the CM5 daemon, the C6
// companion, the STT model file, retained files, the SDK it was built with).
//
// Reports only; it owns none of these values. Each comes from its owner (the
// OTA, ESP-NOW, Bluetooth, CM5 and companion modules, the SDK) and an item is
// present only in builds that compile that boundary. Output contract:
// docs/APP_JSON_CONTRACT.md sec. 4 (`versions`).
#ifndef SYSTEM_VERSIONS_H
#define SYSTEM_VERSIONS_H

#include <ArduinoJson.h>

#include "System_CommandRegistry.h"
#include "System_VersionsCore.h"

// Fill `out` from the owners. Cheap: no I/O beyond one app-descriptor read of
// the factory slot on OTA layouts.
void versionsCollect(hw1versions::List& out);

// `status json` carries the same keys as its "versions" object.
void versionsAddJson(JsonObject out);

extern const CommandEntry versionsCommands[];
extern const size_t versionsCommandsCount;

#endif  // SYSTEM_VERSIONS_H
