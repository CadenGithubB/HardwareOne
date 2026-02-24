#ifndef I2CSENSOR_SEESAW_H
#define I2CSENSOR_SEESAW_H

#include "System_BuildConfig.h"

#if ENABLE_GAMEPAD_SENSOR

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Forward declarations
class String;
class Adafruit_seesaw;

// Seesaw gamepad object (defined in gamepad_sensor.cpp)
extern Adafruit_seesaw gGamepadSeesaw;

// Control input cache (gamepad)
struct ControlCache {
  SemaphoreHandle_t mutex = nullptr;
  uint32_t gamepadButtons = 0;
  int gamepadX = 0, gamepadY = 0;
  unsigned long gamepadLastUpdate = 0;
  bool gamepadDataValid = false;
  uint32_t gamepadSeq = 0;
};

// Global control cache (defined in gamepad_sensor.cpp)
extern ControlCache gControlCache;

// Seesaw gamepad button bit masks (active-low, so invert before checking)
#define GAMEPAD_BUTTON_SELECT (1 << 0)   // Select button
#define GAMEPAD_BUTTON_B      (1 << 1)   // Button B
#define GAMEPAD_BUTTON_Y      (1 << 2)   // Button Y
#define GAMEPAD_BUTTON_A      (1 << 5)   // Button A (Select/Confirm)
#define GAMEPAD_BUTTON_X      (1 << 6)   // Button X
#define GAMEPAD_BUTTON_START  (1 << 16)  // Start button

// Combined mask for all buttons (use this in digitalReadBulk)
#define GAMEPAD_BUTTON_MASK   (GAMEPAD_BUTTON_SELECT | GAMEPAD_BUTTON_B | GAMEPAD_BUTTON_Y | \
                               GAMEPAD_BUTTON_A | GAMEPAD_BUTTON_X | GAMEPAD_BUTTON_START)

// Joystick calibration constants (global for all input handling)
#define JOYSTICK_CENTER       512        // Joystick center position
#define JOYSTICK_DEADZONE     200        // Deadzone ±200 to prevent drift

// Gamepad watermark tracking
extern volatile UBaseType_t gGamepadWatermarkMin;
extern volatile UBaseType_t gGamepadWatermarkNow;

// Gamepad timing
extern unsigned long gLastGamepadInitMs;
extern const unsigned long kGamepadInitMinIntervalMs;

// Gamepad command handlers
const char* cmd_gamepad(const String& cmd);
const char* cmd_gamepadstart_queued(const String& cmd);
const char* cmd_gamepadstop(const String& cmd);
const char* cmd_gamepadpoll(const String& cmd);

// Gamepad state and control
extern bool gamepadEnabled;
extern bool gamepadConnected;
extern unsigned long gamepadLastStopTime;
extern TaskHandle_t gamepadTaskHandle;

// Gamepad internal start (called by queue processor)
const char* startGamepadInternal();

// Gamepad control functions
void pollGamepad();

// Gamepad initialization functions
bool initGamepad();
bool initGamepadConnection();
void readGamepad();

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry gamepadCommands[];
extern const size_t gamepadCommandsCount;

#endif // ENABLE_GAMEPAD_SENSOR
#endif // GAMEPAD_SENSOR_H
