/**
 * PCA9685 Servo Controller Module
 * 
 * I2C-based 16-channel PWM/Servo controller
 * Supports servo motor control with calibration and profiles
 */

#ifndef I2CSENSOR_PCA9685_H
#define I2CSENSOR_PCA9685_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>

// Servo profile structure
struct ServoProfile {
  char name[32];
  uint16_t minPulse;
  uint16_t maxPulse;
  uint16_t centerPulse;
  bool configured;
};

// PCA9685 constants
#define PCA9685_I2C_ADDRESS 0x40
#define MAX_SERVO_CHANNELS 16

// Global PCA9685 driver instance
extern Adafruit_PWMServoDriver* pwmDriver;
extern ServoProfile servoProfiles[MAX_SERVO_CHANNELS];

// PCA9685 initialization
bool initPCA9685();

// Servo control functions
void setServoAngle(uint8_t channel, int angle);
void setPWMValue(uint8_t channel, uint16_t value);

// PCA9685/Servo command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry servoCommands[];
extern const size_t servoCommandsCount;

#endif // I2CSENSOR_PCA9685_H