#ifndef HARDWAREONE_BATTERY_POLICY_H
#define HARDWAREONE_BATTERY_POLICY_H

#include <cmath>
#include <cstdint>

// Shared single-cell LiPo reference and voltage-band thresholds.
#define VBAT_FULL 4.2f
#define VBAT_NOMINAL 3.7f
#define VBAT_LOW 3.4f
#define VBAT_CRITICAL 3.2f
#define VBAT_BAND_FULL 4.15f
#define VBAT_BAND_HIGH 4.00f
#define VBAT_BAND_GOOD 3.80f
#define VBAT_BAND_MEDIUM 3.65f
#define VBAT_BAND_LOW 3.45f
#define VBAT_BAND_CRITICAL 3.30f

// Keep existing ordinals stable for event and display consumers.
enum BatteryStatus {
  BATTERY_UNKNOWN = 0, BATTERY_CHARGING, BATTERY_FULL, BATTERY_DISCHARGING,
  BATTERY_LOW, BATTERY_CRITICAL, BATTERY_NOT_PRESENT, BATTERY_HIGH,
  BATTERY_GOOD, BATTERY_MEDIUM, BATTERY_EMPTY
};

struct BatteryState {
  float voltage = NAN;
  float percentage = NAN;
  BatteryStatus status = BATTERY_UNKNOWN;
  bool isCharging = false;
  bool usbPresent = false;
  uint32_t lastReadMs = 0;       // Last successful voltage read, never an attempt.
  uint16_t rawADC = 0;
  float cratePctPerHr = NAN;
  bool voltageAvailable = false; // Configured measurement hardware, not cell presence.
  bool voltageValid = false;
  bool voltageCalibrated = false;
  bool percentageValid = false;
  bool percentageEstimated = false;
  bool statusEstimated = false;
  bool presenceKnown = false;
  bool present = false;
  bool chargingKnown = false;
  bool chargingEstimated = false;
  bool usbKnown = false;
  bool rateValid = false;
  bool hasSample = false;
  bool stale = false;
  uint32_t lastAttemptMs = 0;
  int32_t lastError = 0;
};

namespace BatteryPolicy {
constexpr uint32_t kStaleAfterMs = 30000;

// Indicative single-cell LiPo curve, not a capacity measurement. Load, cell
// chemistry, temperature and charging all move the voltage/SOC relationship.
// In particular a charger can drive this voltage with NO cell attached.
inline float estimatePercentage(float volts) {
  if (!std::isfinite(volts) || volts < 2.5f || volts > 4.5f) return NAN;
  constexpr float v[] = {3.20f, 3.30f, 3.45f, 3.65f, 3.80f, 4.00f, 4.20f};
  constexpr float p[] = {0.0f, 5.0f, 10.0f, 30.0f, 60.0f, 85.0f, 100.0f};
  if (volts <= v[0]) return 0.0f;
  for (unsigned i = 1; i < sizeof(v) / sizeof(v[0]); ++i) {
    if (volts <= v[i]) return p[i - 1] + (volts - v[i - 1]) *
      (p[i] - p[i - 1]) / (v[i] - v[i - 1]);
  }
  return 100.0f;
}

inline BatteryStatus classify(const BatteryState& s) {
  if (s.presenceKnown && !s.present) return BATTERY_NOT_PRESENT;
  if (!s.voltageValid || !s.percentageValid) return BATTERY_UNKNOWN;
  if (s.chargingKnown && s.isCharging) return BATTERY_CHARGING;
  if (s.voltage >= VBAT_BAND_FULL) return BATTERY_FULL;
  if (s.voltage >= VBAT_BAND_HIGH) return BATTERY_HIGH;
  if (s.voltage >= VBAT_BAND_GOOD) return BATTERY_GOOD;
  if (s.voltage >= VBAT_BAND_MEDIUM) return BATTERY_MEDIUM;
  if (s.voltage >= VBAT_BAND_LOW) return BATTERY_LOW;
  if (s.voltage >= VBAT_BAND_CRITICAL) return BATTERY_CRITICAL;
  return BATTERY_EMPTY;
}

inline void invalidateSample(BatteryState& s, int32_t error, uint32_t attemptMs) {
  s.voltageValid = s.percentageValid = s.rateValid = false;
  s.chargingKnown = false;
  s.presenceKnown = false;
  s.status = BATTERY_UNKNOWN;
  s.statusEstimated = false;
  s.lastAttemptMs = attemptMs;
  s.lastError = error;
  s.stale = false;
  // Retain last values for diagnostics; readers MUST check validity.
  // An independent VBUS GPIO remains meaningful after an ADC/I2C failure.
}

inline bool applyAdcVoltage(BatteryState& s, float volts, uint16_t raw, uint32_t now) {
  if (!std::isfinite(volts) || volts < 0.0f || volts > 6.0f) {
    invalidateSample(s, -1, now);
    return false;
  }
  s.voltage = volts;
  s.rawADC = raw;
  s.voltageValid = s.hasSample = true;
  s.percentage = estimatePercentage(volts);
  s.percentageValid = std::isfinite(s.percentage);
  s.percentageEstimated = true;
  s.presenceKnown = false; // A BAT-node measurement is not a cell-presence test.
  s.chargingKnown = s.chargingEstimated = s.rateValid = false;
  s.isCharging = false;
  s.lastReadMs = s.lastAttemptMs = now;
  s.lastError = 0;
  s.stale = false;
  s.status = classify(s);
  s.statusEstimated = s.status != BATTERY_UNKNOWN;
  return true;
}

inline void expireSample(BatteryState& s, uint32_t now) {
  if (s.hasSample && static_cast<uint32_t>(now - s.lastReadMs) > kStaleAfterMs) {
    s.voltageValid = s.percentageValid = s.rateValid = s.chargingKnown = false;
    s.presenceKnown = false;
    s.status = BATTERY_UNKNOWN;
    s.statusEstimated = false;
    s.stale = true;
  }
  // VBUS itself is cached at the same cadence. It must not outlive a stalled
  // monitor merely because the last ADC read succeeded or failed differently.
  if (s.usbKnown && static_cast<uint32_t>(now - s.lastAttemptMs) > kStaleAfterMs)
    s.usbKnown = false;
}
} // namespace BatteryPolicy
#endif
