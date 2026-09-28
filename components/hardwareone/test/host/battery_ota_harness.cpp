// Generated runner inserts only the production OTA admission function below.
// State schema and freshness policy come from the production header.
#include "BatteryPolicy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

[[maybe_unused]] static constexpr uint32_t kFreshPowerMs = 30000;
[[maybe_unused]] static constexpr float kMinimumBatteryPercent = 30.0f;
#define VBAT_BAND_MEDIUM 3.65f
static uint32_t nowMs = 100000;
static BatteryState rawSnapshot;
uint32_t millis() { return nowMs; }
BatteryState getBatterySnapshot() {
  BatteryState result = rawSnapshot;
  BatteryPolicy::expireSample(result, millis());
  return result;
}

// INSERT_PRODUCTION_OTA_POWER_GATE

static bool allowed(bool force = false) {
  char reason[320]{};
  const bool result = powerIsSafe(force, reason, sizeof(reason));
  if (!result) {
    assert(strstr(reason, "power") != nullptr);
    assert(strstr(reason, "force-power") != nullptr);
  }
  return result;
}

int main() {
#if !ENABLE_BATTERY_MONITOR
  assert(allowed());
  rawSnapshot.voltage = -1;
  rawSnapshot.percentage = -1;
  nowMs = UINT32_MAX;
  assert(allowed()); // Explicit unmonitored-build policy; not proof of USB.
  assert(allowed(true));
#else
  // Default/no reading must not pass merely because uptime is still fresh.
  nowMs = 1;
  assert(!allowed());
  assert(allowed(true));
  nowMs = 100000;
  rawSnapshot = {};
  rawSnapshot.lastReadMs = nowMs;
  rawSnapshot.hasSample = true;
  rawSnapshot.voltage = 4.2f;
  rawSnapshot.percentage = 100.0f;
  rawSnapshot.usbPresent = true;
  rawSnapshot.status = BATTERY_FULL;
  assert(!allowed()); // Values with no validity flags cannot authorize OTA.
  rawSnapshot.status = BATTERY_NOT_PRESENT;
  assert(!allowed()); // Apparent absence is not proof of external supply.
  rawSnapshot.usbKnown = true;
  rawSnapshot.lastAttemptMs = nowMs;
  assert(allowed());
  rawSnapshot.lastReadMs = 1;
  rawSnapshot.hasSample = false;
  assert(allowed()); // Independently fresh VBUS can survive unavailable ADC.
  rawSnapshot.lastReadMs = nowMs;
  rawSnapshot.hasSample = true;
  rawSnapshot.lastAttemptMs = nowMs - 30001;
  assert(!allowed()); // USB is stale even if voltage timing is fresh.
  rawSnapshot.lastAttemptMs = nowMs;
  rawSnapshot.usbPresent = false;
  assert(!allowed());
  rawSnapshot.usbKnown = false;
#if BATTERY_BACKEND_ADC
  rawSnapshot.voltageValid = true;
  rawSnapshot.voltage = 3.649f;
  assert(!allowed());
  rawSnapshot.voltage = 3.65f;
  assert(allowed());
  rawSnapshot.percentage = 0.0f;
  rawSnapshot.percentageValid = true;
  assert(allowed()); // ADC admission uses volts, not approximate SOC.
#else
  rawSnapshot.percentageValid = true;
  rawSnapshot.percentage = 29.99f;
  assert(!allowed());
  rawSnapshot.percentage = 30.0f;
  assert(allowed());
  rawSnapshot.voltage = 0.0f;
  assert(allowed()); // Gauge policy uses its valid SOC, not the ADC rule.
#endif
  nowMs += 30000;
  assert(allowed());
  nowMs += 1;
  assert(!allowed());
  assert(allowed(true));

  // Unsigned age computation remains correct across millis wraparound.
  nowMs = 100;
  rawSnapshot.lastReadMs = UINT32_MAX - 200;
  rawSnapshot.lastAttemptMs = rawSnapshot.lastReadMs;
  assert(allowed());
  nowMs = 30000;
  assert(!allowed());
#endif
  std::cout << "Battery OTA admission: backend=" << BATTERY_BACKEND_ADC
            << " enabled=" << ENABLE_BATTERY_MONITOR << " passed\n";
}
