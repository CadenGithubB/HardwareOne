#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "System_Battery.h"
#include "System_Command.h"
#include "System_Debug.h"

// Global battery state
BatteryState gBatteryState = {
  .voltage = 0.0f,
  .percentage = 0.0f,
  .status = BATTERY_UNKNOWN,
  .isCharging = false,
  .lastReadMs = 0,
  .rawADC = 0
};

// ADC calibration characteristics
static esp_adc_cal_characteristics_t* adc_chars = nullptr;

// Moving average filter for stable readings
#define BATTERY_SAMPLES 10
static float voltageHistory[BATTERY_SAMPLES] = {0};
static uint8_t voltageIndex = 0;
static bool historyFilled = false;

void initBattery() {
  // Configure ADC for battery monitoring
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);
  
  // Characterize ADC for accurate voltage readings
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
    ADC_UNIT_1, 
    ADC_ATTEN_DB_11, 
    ADC_WIDTH_BIT_12, 
    1100,  // Default Vref
    adc_chars
  );
  
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
    INFO_SYSTEMF("Battery ADC calibrated using Two Point Value");
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    INFO_SYSTEMF("Battery ADC calibrated using eFuse Vref");
  } else {
    INFO_SYSTEMF("Battery ADC calibrated using Default Vref");
  }
  
  // Take initial reading
  updateBattery();
  
  INFO_SYSTEMF("Battery monitoring initialized (pin=%d)", BATTERY_PIN);
}

void updateBattery() {
  // Read ADC value
  uint32_t adcSum = 0;
  const int samples = 16;
  
  for (int i = 0; i < samples; i++) {
    adcSum += adc1_get_raw(BATTERY_ADC_CHANNEL);
    delayMicroseconds(100);
  }
  
  uint16_t adcValue = adcSum / samples;
  gBatteryState.rawADC = adcValue;
  
  // Convert to voltage using calibration
  uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adcValue, adc_chars);
  
  // Apply voltage divider correction (2x divider on Feather)
  float batteryVoltage = (voltage_mv / 1000.0f) * VBAT_DIVIDER;
  
  // Add to moving average filter
  voltageHistory[voltageIndex] = batteryVoltage;
  voltageIndex = (voltageIndex + 1) % BATTERY_SAMPLES;
  if (voltageIndex == 0) historyFilled = true;
  
  // Calculate average
  float sum = 0;
  int count = historyFilled ? BATTERY_SAMPLES : voltageIndex;
  for (int i = 0; i < count; i++) {
    sum += voltageHistory[i];
  }
  gBatteryState.voltage = sum / count;
  
  // Calculate percentage (simple linear approximation)
  // LiPo discharge curve is non-linear, but this is good enough
  if (gBatteryState.voltage >= VBAT_FULL) {
    gBatteryState.percentage = 100.0f;
  } else if (gBatteryState.voltage <= VBAT_CRITICAL) {
    gBatteryState.percentage = 0.0f;
  } else {
    gBatteryState.percentage = ((gBatteryState.voltage - VBAT_CRITICAL) / 
                                 (VBAT_FULL - VBAT_CRITICAL)) * 100.0f;
  }
  
  // Determine charging status
  // On Feather, if voltage is rising or > 4.1V, likely charging
  // This is a simple heuristic - proper detection needs a charge status pin
  static float lastVoltage = 0;
  if (gBatteryState.voltage > 4.1f) {
    gBatteryState.isCharging = true;
  } else if (gBatteryState.voltage > lastVoltage + 0.05f) {
    gBatteryState.isCharging = true;
  } else {
    gBatteryState.isCharging = false;
  }
  lastVoltage = gBatteryState.voltage;
  
  // Determine status
  if (gBatteryState.voltage < 2.0f) {
    gBatteryState.status = BATTERY_NOT_PRESENT;
  } else if (gBatteryState.isCharging && gBatteryState.voltage >= VBAT_FULL - 0.05f) {
    gBatteryState.status = BATTERY_FULL;
  } else if (gBatteryState.isCharging) {
    gBatteryState.status = BATTERY_CHARGING;
  } else if (gBatteryState.voltage <= VBAT_CRITICAL) {
    gBatteryState.status = BATTERY_CRITICAL;
  } else if (gBatteryState.voltage <= VBAT_LOW) {
    gBatteryState.status = BATTERY_LOW;
  } else {
    gBatteryState.status = BATTERY_DISCHARGING;
  }
  
  gBatteryState.lastReadMs = millis();
}

float getBatteryPercentage() {
  return gBatteryState.percentage;
}

float getBatteryVoltage() {
  return gBatteryState.voltage;
}

bool isBatteryCharging() {
  return gBatteryState.isCharging;
}

const char* getBatteryStatusString() {
  switch (gBatteryState.status) {
    case BATTERY_CHARGING: return "Charging";
    case BATTERY_FULL: return "Full";
    case BATTERY_DISCHARGING: return "Discharging";
    case BATTERY_LOW: return "Low";
    case BATTERY_CRITICAL: return "Critical";
    case BATTERY_NOT_PRESENT: return "Not Present";
    default: return "Unknown";
  }
}

char getBatteryIcon() {
  if (gBatteryState.status == BATTERY_NOT_PRESENT) return '?';
  if (gBatteryState.isCharging) return '+';
  
  if (gBatteryState.percentage >= 75) return 'F';  // Full
  if (gBatteryState.percentage >= 50) return 'H';  // High
  if (gBatteryState.percentage >= 25) return 'M';  // Medium
  if (gBatteryState.percentage >= 10) return 'L';  // Low
  return 'E';  // Empty/Critical
}

const char* cmd_battery_status(const String& args) {
  updateBattery();
  
  extern void broadcastOutput(const char* s);
  
  broadcastOutput("");
  broadcastOutput("╔════════════════════════════════════════╗");
  broadcastOutput("║         BATTERY STATUS                 ║");
  broadcastOutput("╠════════════════════════════════════════╣");
  
  char line[64];
  snprintf(line, sizeof(line), "║ Voltage:     %.2fV                    ║", gBatteryState.voltage);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║ Percentage:  %.0f%%                    ║", gBatteryState.percentage);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║ Status:      %-20s ║", getBatteryStatusString());
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║ Charging:    %-20s ║", gBatteryState.isCharging ? "Yes" : "No");
  broadcastOutput(line);
  
  broadcastOutput("║                                        ║");
  
  snprintf(line, sizeof(line), "║ Raw ADC:     %4d / 4095               ║", gBatteryState.rawADC);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║ Last Read:   %lu ms ago               ║", (unsigned long)(millis() - gBatteryState.lastReadMs));
  broadcastOutput(line);
  
  broadcastOutput("╠════════════════════════════════════════╣");
  broadcastOutput("║ LiPo Voltage Reference:                ║");
  
  snprintf(line, sizeof(line), "║   Full:      %.2fV                    ║", VBAT_FULL);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║   Nominal:   %.2fV                    ║", VBAT_NOMINAL);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║   Low:       %.2fV                    ║", VBAT_LOW);
  broadcastOutput(line);
  
  snprintf(line, sizeof(line), "║   Critical:  %.2fV                    ║", VBAT_CRITICAL);
  broadcastOutput(line);
  
  broadcastOutput("╚════════════════════════════════════════╝");
  
  return "Battery status displayed above";
}

const char* cmd_battery_calibrate(const String& args) {
  // Re-read ADC characteristics
  if (adc_chars) {
    free(adc_chars);
  }
  
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);
  
  // Clear history
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    voltageHistory[i] = 0;
  }
  voltageIndex = 0;
  historyFilled = false;
  
  // Take fresh readings
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    updateBattery();
    delay(100);
  }
  
  return "Battery calibration complete. Check 'battery status' for new readings.";
}

// Command registration moved to system_utils.cpp to ensure linker inclusion
