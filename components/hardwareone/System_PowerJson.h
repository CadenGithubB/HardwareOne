#ifndef SYSTEM_POWER_JSON_H
#define SYSTEM_POWER_JSON_H

// JSON snapshots of the power and battery modules. The declarations live apart
// from System_Power.h and System_Battery.h so those two headers do not pull in
// ArduinoJson.

#include <ArduinoJson.h>

// Power snapshot (System_Power.cpp): mode, clocks, the workload holding a
// boost, idle power-save state and the preset table. `power json` and
// /api/power/status both call it.
void buildPowerJson(JsonDocument& doc);

// Battery snapshot (System_Battery.cpp). `battery json` and
// /api/battery/status both call it.
void buildBatteryJson(JsonDocument& doc);

#endif // SYSTEM_POWER_JSON_H
