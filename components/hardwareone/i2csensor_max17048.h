// i2csensor_max17048.h — MAX17048G LiPo fuel gauge driver
// I2C Address: 0x36 (fixed per datasheet)
// Wired on FeatherS3[D]'s I2C1 (horizontal STEMMA QT, always-on LDO).
//
// Lifecycle: this is NOT a user-toggleable sensor — it's the battery backend
// for boards that select BATTERY_BACKEND_FUEL_GAUGE. System_Battery.cpp owns
// the polling cadence (10s tick from the main loop) and writes results into
// gBatteryState. No CLI start/stop, no I2CSensorEntry, no task.
//
// Why no task: a single read pass (~5ms of I2C) at 0.1 Hz can ride the
// existing main-loop tick without spinning up a dedicated task. Saves ~2 KB
// of DRAM that's better spent on the worker queues per project conventions.

#ifndef I2CSENSOR_MAX17048_H
#define I2CSENSOR_MAX17048_H

#include "System_BuildConfig.h"

#if BATTERY_BACKEND_FUEL_GAUGE

#include <stdint.h>

// Snapshot of one MAX17048 read pass. All fields are valid only when
// fuelGaugeRead() returned true. Owned by the caller (System_Battery
// allocates on the stack and copies relevant bits into gBatteryState).
struct FuelGaugeReading {
  float voltage;          // Cell voltage in volts (typ. 3.0 - 4.2 for LiPo)
  float socPct;           // State of charge in percent (0.0 - 100.0)
  float cratePctPerHr;    // Signed rate of change (%/hr). Positive = charging.
  uint16_t version;       // Chip VERSION register — useful in logs to confirm part
};

// Probe for the MAX17048 at I2C_ADDR_FUEL_GAUGE on the configured bus
// (gSettings.fuelGaugeBus). Returns true if the chip ACKs AND its VERSION
// register reads a plausible value (high nibble == 0x0). Side-effect:
// registers the device for I2C health tracking on first success, so the
// i2cmetrics/i2cscan commands see it. Idempotent.
bool fuelGaugeProbe();

// Read VCELL/SOC/CRATE/VERSION into `out`. Returns false on I2C error.
// Does NOT update gBatteryState — that's System_Battery's job. Safe to call
// before fuelGaugeProbe() (will fail gracefully if the chip isn't there).
bool fuelGaugeRead(FuelGaugeReading* out);

#endif  // BATTERY_BACKEND_FUEL_GAUGE

#endif  // I2CSENSOR_MAX17048_H
