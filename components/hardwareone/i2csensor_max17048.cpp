// i2csensor_max17048.cpp — MAX17048G LiPo fuel gauge driver

#include "i2csensor_max17048.h"

#if BATTERY_BACKEND_FUEL_GAUGE

#include <Arduino.h>
#include <Wire.h>

#include "System_Debug.h"
#include "System_I2C.h"
#include "System_I2C_Manager.h"
#include "System_Settings.h"

// MAX17048 registers (big-endian on the wire, 16-bit values).
//   VCELL  0x02 — 78.125 µV per LSB, unsigned
//   SOC    0x04 — 1/256 % per LSB, unsigned
//   MODE   0x06 — QuickStart / enable bits
//   VERSION 0x08 — production parts return 0x001? per datasheet
//   CRATE  0x16 — 0.208 %/hr per LSB, SIGNED (+ = charging)
//   CONFIG 0x0C — alert thresholds, sleep enable
//   STATUS 0x1A — alert flags
//   CMD    0xFE — write 0x5400 for POR
#define MAX17048_REG_VCELL    0x02
#define MAX17048_REG_SOC      0x04
#define MAX17048_REG_VERSION  0x08
#define MAX17048_REG_CRATE    0x16

#define MAX17048_VCELL_LSB_UV   78.125f     // microvolts per LSB
#define MAX17048_SOC_LSB        (1.0f/256.0f)
#define MAX17048_CRATE_LSB      0.208f      // %/hr per signed LSB

// Resolve which bus + Wire* the chip is on from settings. Returns false if
// the bus isn't initialized (e.g. user set fuelGaugeBus=1 but I2C2 is off) —
// each read function bails on failure, so a misconfigured bus surfaces as
// "fuel gauge not connected" rather than a silent fall-through to bus 0.
static bool fuelGaugeResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.fuelGaugeBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

// Read a 16-bit big-endian register starting at `reg`. Used for VCELL / SOC /
// VERSION / CRATE — all the registers we care about are 16-bit. The chip
// auto-increments the internal pointer, so a 2-byte read after a 1-byte
// register address write gives MSB then LSB.
static bool fuelGaugeReadReg16(uint8_t reg, uint16_t* out) {
  if (!out) return false;
  uint8_t bus; TwoWire* w;
  if (!fuelGaugeResolveBus(&bus, &w)) return false;

  return i2cDeviceTransaction(bus, I2C_ADDR_FUEL_GAUGE, 100000, 100, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_FUEL_GAUGE);
    w->write(reg);
    if (w->endTransmission(false) != 0) return false;  // repeated start

    if (w->requestFrom((uint8_t)I2C_ADDR_FUEL_GAUGE, (uint8_t)2) != 2) return false;
    const uint8_t hi = w->read();
    const uint8_t lo = w->read();
    *out = ((uint16_t)hi << 8) | lo;
    return true;
  });
}

bool fuelGaugeProbe() {
  if (!i2cPingAddress(I2C_ADDR_FUEL_GAUGE, 100000, 100, (uint8_t)gSettings.fuelGaugeBus)) {
    return false;
  }

  // Confirm it's really a MAX17048 (or compatible) and not some other chip
  // happening to ACK at 0x36. Datasheet VERSION register encodes the part
  // in the high nibble; production MAX17048 parts return 0x001?, with the
  // low nibble being the silicon revision. Treat any value whose high
  // nibble is 0x0 as "looks like a MAX17048-family device" — defensive
  // against revision drift while still rejecting outright wrong silicon.
  uint16_t version = 0;
  if (!fuelGaugeReadReg16(MAX17048_REG_VERSION, &version)) {
    DEBUG_SYSTEMF("[FUEL] Probe: VERSION read failed");
    return false;
  }
  if ((version & 0xF000) != 0x0000) {
    DEBUG_SYSTEMF("[FUEL] Probe: unexpected VERSION=0x%04X (not a MAX17048?)", version);
    return false;
  }

  // Register for I2C health tracking so i2cmetrics/i2cscan see the device.
  // Idempotent — the manager dedupes by (bus, address).
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) mgr->registerDevice(I2C_ADDR_FUEL_GAUGE, "MAX17048", 100000, 100,
                               (uint8_t)gSettings.fuelGaugeBus);

  INFO_SYSTEMF("[FUEL] MAX17048 detected on bus %u, VERSION=0x%04X",
               (unsigned)gSettings.fuelGaugeBus, version);
  return true;
}

bool fuelGaugeRead(FuelGaugeReading* out) {
  if (!out) return false;

  uint16_t rawVcell = 0, rawSoc = 0, rawCrate = 0, rawVer = 0;
  if (!fuelGaugeReadReg16(MAX17048_REG_VCELL,   &rawVcell)) return false;
  if (!fuelGaugeReadReg16(MAX17048_REG_SOC,     &rawSoc))   return false;
  if (!fuelGaugeReadReg16(MAX17048_REG_CRATE,   &rawCrate)) return false;
  if (!fuelGaugeReadReg16(MAX17048_REG_VERSION, &rawVer))   return false;

  out->voltage       = (float)rawVcell * (MAX17048_VCELL_LSB_UV * 1e-6f);
  out->socPct        = (float)rawSoc * MAX17048_SOC_LSB;
  // CRATE is a signed 16-bit value — preserve sign before scaling.
  out->cratePctPerHr = (float)((int16_t)rawCrate) * MAX17048_CRATE_LSB;
  out->version       = rawVer;
  return true;
}

#endif  // BATTERY_BACKEND_FUEL_GAUGE
