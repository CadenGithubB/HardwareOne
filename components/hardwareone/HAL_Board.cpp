// HAL_Board.cpp - shared rails and the antenna switch (HAL_Board.h).
#include "HAL_Board.h"

#include <Arduino.h>

#include "System_BuildConfig.h"

void boardPowerRailInit() {
#if defined(NEOPIXEL_POWER_PIN) && NEOPIXEL_POWER_PIN >= 0
  pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
  delay(10);  // Allow power to stabilize
#endif
}

bool boardHasI2c2Rail() { return I2C2_POWER_PIN >= 0; }

int boardI2c2RailOn() {
#if I2C2_POWER_PIN >= 0
  pinMode(I2C2_POWER_PIN, OUTPUT);
  digitalWrite(I2C2_POWER_PIN, HIGH);
#endif
  return I2C2_POWER_PIN;
}

void boardI2c2RailOff() {
#if I2C2_POWER_PIN >= 0
  digitalWrite(I2C2_POWER_PIN, LOW);
#endif
}

bool boardHasAntennaSwitch() { return WIFI_ANTENNA_SWITCH_PIN >= 0; }

int boardAntennaSelectExternal() {
#if WIFI_ANTENNA_SWITCH_PIN >= 0
  pinMode(WIFI_ANTENNA_SWITCH_PIN, OUTPUT);
  digitalWrite(WIFI_ANTENNA_SWITCH_PIN, HIGH);
#endif
  return WIFI_ANTENNA_SWITCH_PIN;
}
