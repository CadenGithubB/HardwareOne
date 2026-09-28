#ifndef SYSTEM_BATTERY_H
#define SYSTEM_BATTERY_H

#include <Arduino.h>
#include "System_BuildConfig.h"

#include "BatteryPolicy.h"

// Reference constants, status enum and validity-bearing state live in the
// platform-independent policy header. Voltage bands are estimates; charger
// state and battery presence remain unknown without independent evidence.

// Compatibility symbol. Consumers should use the coherent snapshot accessor.
extern BatteryState gBatteryState;

// Lifecycle.
void initBattery();
void updateBattery();

// Accessors used by OLED, G2, notifications, etc.
BatteryState getBatterySnapshot();
// Numeric accessors return NAN when unavailable; boolean accessors require known state.
float getBatteryPercentage();
float getBatteryVoltage();
bool isBatteryCharging();
bool isUsbPresent();
const char* batteryStatusString(BatteryStatus status);
const char* batteryStatusString(const BatteryState& state);
const char* getBatteryStatusString();
char getBatteryIcon();

// CLI commands.
const char* cmd_battery_status(const String& args);
const char* cmd_battery_calibrate(const String& args);
const char* cmd_batterylog(const String& args);

// Time-series logging. Appends one clean comma-CSV row to /battery.csv per
// sample so a discharge curve can be pulled off-device and graphed with
// standard tools. Columns:
//   boot,uptime_ms,epoch_s,datetime,pct,voltage,crate,status,charging,usb,event
// Unknown samples/charging/USB fields are empty; status marks estimates.
// Visual/aligned presentation is the web UI's job, not this file's format.
// Called every main-loop battery tick; self-gates on gSettings.batteryLogEnabled
// + gSettings.batteryLogIntervalMs and rotates the file by size. Independent of
// the sensor log.
void batteryLogTick();

// Append a discrete power-state annotation (sleep/wake, CPU freq change, power
// mode change, power-save enter/wake) — same schema, with the token in the
// 'event' column and the live battery snapshot alongside it. Always recorded
// (not interval-gated); respects gSettings.batteryLogEnabled. Safe from any
// task. Tokens must be comma/newline-free.
void batteryLogEvent(const char* event);

#endif // SYSTEM_BATTERY_H
