#ifndef I2CSENSOR_SEESAW_H
#define I2CSENSOR_SEESAW_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Gamepad input cache structure (always available for type-safe references)
struct InputCache {
  SemaphoreHandle_t mutex = nullptr;
  uint32_t buttons = 0;
  int joyX = 0, joyY = 0;
  unsigned long lastUpdate = 0;
  bool dataValid = false;
  uint32_t seq = 0;
  uint32_t buttonPressedAccum = 0;  // Latched press edges — OR'd in by task, read+cleared by UI
};

// OLED-facing input state. Defined by exactly one input driver — either
// i2csensor_seesaw.cpp (when ENABLE_GAMEPAD_SENSOR) or i2csensor_ano_encoder.cpp
// (when ENABLE_ANO_ENCODER). The ANO driver populates gInputCache with
// synthesized values (buttons in active-low layout matching the gamepad's
// semantics) so the OLED input pipeline doesn't care which driver is live.
extern bool gInputRunning;
extern bool gInputConnected;
extern InputCache gInputCache;

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
extern Adafruit_seesaw* gGamepadSeesaw;

// Build gamepad JSON snapshot from gInputCache. Safe to call from any task —
// acquires gInputCache.mutex. Returns bytes written or 0 on failure.
int gamepadBuildDataJSON(char* buf, size_t bufSize);

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
extern TaskHandle_t gInputTaskHandle;

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

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry gamepadCommands[];
extern const size_t gamepadCommandsCount;

#endif // ENABLE_GAMEPAD_SENSOR
#endif // GAMEPAD_SENSOR_H
