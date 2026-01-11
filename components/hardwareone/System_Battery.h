#ifndef SYSTEM_BATTERY_H
#define SYSTEM_BATTERY_H

#include <Arduino.h>

// Adafruit Feather ESP32 battery monitoring
// Battery voltage is on A13 (GPIO 35) via voltage divider (2x)
#define BATTERY_PIN 35
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_7

// Voltage divider: 2x (100K + 100K resistors)
// ADC reads 0-3.3V, actual battery voltage is 0-4.2V (LiPo)
#define VBAT_DIVIDER 2.0f

// LiPo voltage levels (in volts)
#define VBAT_FULL 4.2f
#define VBAT_NOMINAL 3.7f
#define VBAT_LOW 3.4f
#define VBAT_CRITICAL 3.2f

// Battery status
enum BatteryStatus {
  BATTERY_UNKNOWN = 0,
  BATTERY_CHARGING,
  BATTERY_FULL,
  BATTERY_DISCHARGING,
  BATTERY_LOW,
  BATTERY_CRITICAL,
  BATTERY_NOT_PRESENT
};

struct BatteryState {
  float voltage;           // Current battery voltage
  float percentage;        // Estimated charge percentage (0-100)
  BatteryStatus status;    // Current status
  bool isCharging;         // True if USB connected and charging
  uint32_t lastReadMs;     // Last reading timestamp
  uint16_t rawADC;         // Raw ADC value for debugging
};

// Global battery state
extern BatteryState gBatteryState;

// Initialize battery monitoring
void initBattery();

// Read current battery voltage and update state
void updateBattery();

// Get battery percentage (0-100)
float getBatteryPercentage();

// Get battery voltage
float getBatteryVoltage();

// Check if battery is charging (USB connected)
bool isBatteryCharging();

// Get battery status string
const char* getBatteryStatusString();

// Get battery icon character for OLED
char getBatteryIcon();

// Commands
const char* cmd_battery_status(const String& args);
const char* cmd_battery_calibrate(const String& args);

#endif // SYSTEM_BATTERY_H
