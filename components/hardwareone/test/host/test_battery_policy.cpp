// Exercises the production battery policy: no Arduino/IDF replacements here.
#include "BatteryPolicy.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

static bool closeTo(float actual, float expected, float tolerance = 0.02f) {
  return std::fabs(actual - expected) <= tolerance;
}

static void estimatesAreBoundedAndMonotonic() {
  assert(std::isnan(BatteryPolicy::estimatePercentage(-1.0f)));
  assert(std::isnan(BatteryPolicy::estimatePercentage(0.0f)));
  assert(std::isnan(BatteryPolicy::estimatePercentage(2.49f)));
  assert(std::isnan(BatteryPolicy::estimatePercentage(4.51f)));
  assert(std::isnan(BatteryPolicy::estimatePercentage(std::numeric_limits<float>::infinity())));
  assert(std::isnan(BatteryPolicy::estimatePercentage(std::numeric_limits<float>::quiet_NaN())));
  assert(closeTo(BatteryPolicy::estimatePercentage(4.2f), 100.0f));
  assert(closeTo(BatteryPolicy::estimatePercentage(2.5f), 0.0f));
  float previous = -1.0f;
  for (int mv = 2500; mv <= 4500; ++mv) {
    const float value = BatteryPolicy::estimatePercentage(static_cast<float>(mv) / 1000.0f);
    assert(std::isfinite(value));
    assert(value >= 0.0f && value <= 100.0f);
    assert(value >= previous);
    previous = value;
  }
}

static void adcDoesNotInventPresenceOrUsb() {
  BatteryState state{};
  assert(BatteryPolicy::classify(state) == BATTERY_UNKNOWN);
  BatteryPolicy::applyAdcVoltage(state, 4.2f, 2100, 100);
  assert(state.voltageValid && state.percentageValid && state.percentageEstimated);
  assert(closeTo(state.voltage, 4.2f) && closeTo(state.percentage, 100.0f));
  assert(!state.usbKnown && !state.usbPresent);
  assert(!state.chargingKnown && !state.isCharging);
  assert(!state.presenceKnown);
  assert(!state.rateValid);
  assert(state.lastReadMs == 100 && state.rawADC == 2100);
  assert(BatteryPolicy::classify(state) == BATTERY_FULL);

  // A fast voltage increase is not a measurement of charging or external power.
  BatteryPolicy::applyAdcVoltage(state, 3.2f, 1600, 200);
  BatteryPolicy::applyAdcVoltage(state, 4.15f, 2075, 300);
  assert(!state.usbKnown && !state.usbPresent);
  assert(!state.chargingKnown && !state.isCharging);

  // An autonomous charger can drive an unloaded BAT terminal. Even a plausible
  // voltage is not evidence that a cell is attached; zero is not authoritative
  // absence either.
  BatteryPolicy::applyAdcVoltage(state, 0.0f, 0, 400);
  assert(!state.percentageValid && !state.presenceKnown);
  assert(BatteryPolicy::classify(state) == BATTERY_UNKNOWN);
}

static void failureKeepsLastSuccessfulTimestamp() {
  BatteryState state{};
  BatteryPolicy::applyAdcVoltage(state, 3.85f, 1925, 1234);
  const float previousVoltage = state.voltage;
  const float previousPercentage = state.percentage;
  BatteryPolicy::invalidateSample(state, 257, 8000);
  assert(state.lastReadMs == 1234);
  assert(state.lastAttemptMs == 8000);
  assert(state.lastError == 257);
  assert(state.voltage == previousVoltage && state.percentage == previousPercentage);
  assert(!state.voltageValid && !state.percentageValid && !state.rateValid);
  assert(!state.chargingKnown);
  assert(BatteryPolicy::classify(state) == BATTERY_UNKNOWN);
  assert(state.status == BATTERY_UNKNOWN);
  BatteryPolicy::invalidateSample(state, 258, 20000);
  assert(state.lastReadMs == 1234 && state.lastAttemptMs == 20000);
  BatteryPolicy::applyAdcVoltage(state, 3.90f, 1950, 21000);
  assert(state.voltageValid && state.percentageValid && state.lastError == 0);
  assert(state.lastReadMs == 21000);
}

static void staleSnapshotsIncludeWraparound() {
  BatteryState state{};
  BatteryPolicy::applyAdcVoltage(state, 3.9f, 1950, 100);
  BatteryPolicy::expireSample(state, 30100);
  assert(state.voltageValid && state.percentageValid);
  BatteryPolicy::expireSample(state, 30101);
  assert(!state.voltageValid && !state.percentageValid);
  assert(state.lastReadMs == 100);
  assert(BatteryPolicy::classify(state) == BATTERY_UNKNOWN);

  // A valid sample taken at boot millisecond0 must not be mistaken for no read.
  state = {};
  BatteryPolicy::applyAdcVoltage(state, 3.9f, 1950, 0);
  BatteryPolicy::expireSample(state, 1);
  assert(state.voltageValid && state.percentageValid);

  state = {};
  const uint32_t then = UINT32_MAX - 15000u;
  BatteryPolicy::applyAdcVoltage(state, 3.9f, 1950, then);
  BatteryPolicy::expireSample(state, then + 30000u);
  assert(state.voltageValid);
  BatteryPolicy::expireSample(state, then + 30001u);
  assert(!state.voltageValid && state.lastReadMs == then);
}

static void measuredSignalsClassifyOnlyWhenKnown() {
  BatteryState state{};
  BatteryPolicy::applyAdcVoltage(state, 3.85f, 1925, 1);
  state.usbPresent = true;
  assert(BatteryPolicy::classify(state) != BATTERY_CHARGING);
  state.usbKnown = true;
  assert(BatteryPolicy::classify(state) != BATTERY_CHARGING);
  state.isCharging = true;
  assert(BatteryPolicy::classify(state) != BATTERY_CHARGING);
  state.chargingKnown = true;
  assert(BatteryPolicy::classify(state) == BATTERY_CHARGING);
  state.presenceKnown = true;
  state.present = false;
  assert(BatteryPolicy::classify(state) == BATTERY_NOT_PRESENT);
}

static void invalidAdcReadingsAndIndependentUsbExpiry() {
  BatteryState state{};
  assert(BatteryPolicy::applyAdcVoltage(state, 3.9f, 1950, 0));
  const float good = state.voltage;
  for (float invalid : {-0.01f, 6.01f, INFINITY, NAN}) {
    assert(!BatteryPolicy::applyAdcVoltage(state, invalid, 65535, 1000));
    assert(!state.voltageValid && !state.percentageValid);
    assert(state.lastReadMs == 0 && state.voltage == good);
    assert(state.lastError != 0);
  }
  // A fresh GPIO observation survives an ADC failure, but expires separately
  // if the sampling task stalls. Presence of VBUS does not certify charging.
  state.usbKnown = state.usbPresent = true;
  BatteryPolicy::invalidateSample(state, 257, 5000);
  assert(state.usbKnown && state.usbPresent);
  BatteryPolicy::expireSample(state, 35000);
  assert(state.usbKnown);
  BatteryPolicy::expireSample(state, 35001);
  assert(!state.usbKnown);
}

int main() {
  estimatesAreBoundedAndMonotonic();
  adcDoesNotInventPresenceOrUsb();
  failureKeepsLastSuccessfulTimestamp();
  staleSnapshotsIncludeWraparound();
  measuredSignalsClassifyOnlyWhenKnown();
  invalidAdcReadingsAndIndependentUsbExpiry();
  std::cout << "Battery policy: estimates, unknown signals, failures, freshness and wraparound passed\n";
}
