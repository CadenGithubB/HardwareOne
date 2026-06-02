#ifndef SYSTEM_BATTERY_H
#define SYSTEM_BATTERY_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// Adafruit Feather ESP32 battery monitoring (ADC backend default pin).
// Boards override via BATTERY_ADC_PIN; this fallback matches the original
// Feather ESP32 / Feather ESP32 V2 wiring (GPIO 35, ADC1 channel 7) for any
// board that didn't redefine BATTERY_ADC_PIN.
#define BATTERY_PIN 35
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_7

// Voltage divider: 2x (100K + 100K resistors) on Feather. ADC reads 0-3.3V,
// actual battery voltage is 0-4.2V (LiPo). Fuel-gauge backend ignores this
// — the MAX17048 reports cell voltage directly with no scaling.
#define VBAT_DIVIDER 2.0f

// LiPo voltage levels (in volts) — used by both backends for status thresholds.
#define VBAT_FULL 4.2f
#define VBAT_NOMINAL 3.7f
#define VBAT_LOW 3.4f
#define VBAT_CRITICAL 3.2f

// Off-charger status bands (LiPo discharge curve). These are capacity-
// independent voltage levels, so they work with ANY battery. classifyAndNotify
// walks them top-down once USB is absent. On the charger the status is always
// CHARGING — during the CV charge phase the cell sits near 4.2V regardless of
// true SOC, so voltage can't tell "topped off" from "still filling". Tune here:
// the OLED icon, G2 widget, web page, and notifications all read the status.
#define VBAT_BAND_FULL      4.15f   // off-charger, >= → Full
#define VBAT_BAND_HIGH      4.00f   // >= → High
#define VBAT_BAND_GOOD      3.80f   // >= → Good
#define VBAT_BAND_MEDIUM    3.65f   // >= → Medium
#define VBAT_BAND_LOW       3.45f   // >= → Low
#define VBAT_BAND_CRITICAL  3.30f   // >= → Critical, else → Empty

// Status enum — public API surface, shared across all backends.
enum BatteryStatus {
  BATTERY_UNKNOWN = 0,
  BATTERY_CHARGING,
  BATTERY_FULL,
  BATTERY_DISCHARGING,   // legacy generic off-charger state; superseded by the finer bands below
  BATTERY_LOW,
  BATTERY_CRITICAL,
  BATTERY_NOT_PRESENT,
  // Finer off-charger voltage bands. Appended so the existing ordinals stay
  // stable. classifyAndNotify() now walks the full ladder when on battery.
  BATTERY_HIGH,
  BATTERY_GOOD,
  BATTERY_MEDIUM,
  BATTERY_EMPTY
};

// Snapshot of current battery state. Updated by updateBattery() on the 10s
// main-loop tick; consumed by OLED widgets, G2 corner widget, notifications,
// and the `batterystatus` CLI command.
//
// Fields populated by ALL backends: voltage, percentage, status, isCharging,
//   lastReadMs.
// Backend-specific fields:
//   rawADC          — ADC backend only (0..4095). Always 0 for fuel-gauge.
//   cratePctPerHr   — Fuel-gauge backend only (signed %/hr). Always 0 for ADC.
struct BatteryState {
  float voltage;           // Current battery voltage
  float percentage;        // Estimated charge percentage (0-100)
  BatteryStatus status;    // Current status
  bool isCharging;         // True if cell is taking charge (not just "USB connected")
  bool usbPresent;         // True if a USB power source is supplying the device.
                           // Distinct from isCharging: the cell can be at float
                           // (100%, CRATE≈0) while USB is plugged in — isCharging
                           // is false but usbPresent is true. Inferred from
                           // isCharging OR voltage > 4.15V (LiPo can't hold that
                           // high without an external charge source). Always
                           // true in the USB-only stub build.
  uint32_t lastReadMs;     // Last reading timestamp
  uint16_t rawADC;         // [ADC backend] Raw ADC value, for diagnostics
  float cratePctPerHr;     // [Fuel-gauge backend] Signed rate of change (%/hr)
};

// Global battery state — single source of truth across the codebase.
extern BatteryState gBatteryState;

// Lifecycle.
void initBattery();
void updateBattery();

// Accessors used by OLED, G2, notifications, etc.
float getBatteryPercentage();
float getBatteryVoltage();
bool isBatteryCharging();
bool isUsbPresent();
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
