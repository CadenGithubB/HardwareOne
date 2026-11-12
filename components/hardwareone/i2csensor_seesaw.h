#ifndef I2CSENSOR_SEESAW_H
#define I2CSENSOR_SEESAW_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// OLED-facing input state. Defined by exactly one input driver — either
// i2csensor_seesaw.cpp (when ENABLE_GAMEPAD_SENSOR) or i2csensor_ano_encoder.cpp
// (when ENABLE_ANO_ENCODER). The ANO driver populates the shared input reading with
// synthesized values (buttons in active-low layout matching the gamepad's
// semantics) so the OLED input pipeline doesn't care which driver is live.
#if ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GPIO_ENCODER || ENABLE_GT911_TOUCH
extern bool gInputRunning;
extern bool gInputConnected;
#else
// No input driver is in this build: the state reads as stopped and a start
// request fails.
inline bool gInputRunning = false;
inline bool gInputConnected = false;
inline bool inputStartInternal() { return false; }
#endif

// Joystick calibration constants. Shared by both input drivers since the
// ANO encoder driver synthesizes joyX/Y at CENTER (encoder-to-nav-event goes
// through a separate path) — but JOYSTICK_DEADZONE is still referenced by
// other OLED code paths regardless of which input device is active.
#define JOYSTICK_CENTER       512
#define JOYSTICK_DEADZONE     200

#if ENABLE_GAMEPAD_SENSOR

// Forward declarations
class String;
class Adafruit_seesaw;

// Heap-allocated at sensor init with the right TwoWire* for the gamepad's
// configured bus (gSettings.inputBus). nullptr until init runs (or after a
// failed init). Was a stack global `Adafruit_seesaw gGamepadSeesaw(&Wire1)`
// pre-dual-bus, but the library binds the TwoWire pointer at construction
// time so we need to defer construction until we know which bus to use.

// Build gamepad JSON snapshot from the shared input reading. Safe to call from any task —
// acquires the HAL cache mutex. Returns bytes written or 0 on failure.
int gamepadBuildDataJson(char* buf, size_t bufSize);

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

// Gamepad watermark tracking
extern volatile UBaseType_t gGamepadWatermarkMin;
extern volatile UBaseType_t gGamepadWatermarkNow;

// Gamepad timing
extern unsigned long gLastGamepadInitMs;
extern const unsigned long kGamepadInitMinIntervalMs;

// Gamepad command handlers
const char* cmd_gamepad(const String& argsInput);
const char* cmd_gamepadstart_queued(const String& argsInput);
const char* cmd_gamepadstop(const String& argsInput);
const char* cmd_gamepadpoll(const String& argsInput);

// Gamepad state and control
extern unsigned long gGamepadLastStopTime;
// gInputTaskHandle (shared by every input driver) is declared in System_TaskUtils.h.

// Gamepad internal start (called by queue processor)
bool inputStartInternal();

// Gamepad control functions
void gamepadPoll();

// Gamepad initialization functions
bool gamepadInit();
bool gamepadInitConnection();
void gamepadPoll();

// Accessor functions (for MQTT and other modules)
int gamepadGetX();
int gamepadGetY();
uint32_t gamepadGetButtons();

#else
// The gamepad is not in this build. Another input driver may still be.
inline unsigned long gGamepadLastStopTime = 0;
#endif // ENABLE_GAMEPAD_SENSOR
#endif // GAMEPAD_SENSOR_H
