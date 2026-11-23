/**
 * HAL_Input.h - Input Hardware Abstraction Layer
 * 
 * Provides compile-time and runtime input controller selection.
 * This allows the same UI code to work with different input hardware by
 * changing INPUT_TYPE in System_BuildConfig.h or at runtime.
 * 
 * Currently supported:
 *   - INPUT_TYPE_SEESAW_GAMEPAD: Adafruit Seesaw gamepad (I2C)
 *   - INPUT_TYPE_CLICK_WHEEL: Rotary encoder with buttons
 *   - INPUT_TYPE_CUSTOM: User-defined button mappings
 *   - INPUT_TYPE_ANO_ENCODER, INPUT_TYPE_GPIO_ENCODER: rotary encoders
 *   - INPUT_TYPE_GT911_TOUCH: single-contact touchscreen
 *   - INPUT_TYPE_IMU: the motion sensor (taps are buttons, tilt is the joystick)
 */

#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "Input_TouchCore.h"
#include "Input_GT911.h"

// Gamepad input cache structure (always available for type-safe references)
struct InputCache {
  uint32_t buttons = 0;
  int joyX = 0, joyY = 0;
  unsigned long lastUpdate = 0;
  bool dataValid = false;
  uint32_t seq = 0;
  uint32_t buttonPressedAccum = 0;  // Latched press edges — OR'd in by task, read+cleared by UI
  // Held buttons the OLED navigation treats as released (active high, a
  // subset of the pressed bits of `buttons`). `buttons` stays the physical
  // state for every other reader (sensor log, G2 page); the navigation applies
  // this mask itself (inputNavButtons). The ANO sets RIGHT (its role is the
  // SELECT edge at release) and the IN of a RIGHT+IN chord while it is held
  // (that IN is START, never A). Every other backend leaves it 0.
  uint32_t navHidden = 0;
};

// The buttons as the OLED navigation sees them (active low like `buttons`):
// the physical state with the navHidden buttons released.
inline uint32_t inputNavButtons(const InputCache& c) { return c.buttons | c.navHidden; }

// =============================================================================
// Input Type Selection (compile-time default)
// =============================================================================

#define INPUT_TYPE_NONE            0
#define INPUT_TYPE_SEESAW_GAMEPAD  1
#define INPUT_TYPE_CLICK_WHEEL     2
#define INPUT_TYPE_CUSTOM          3
#define INPUT_TYPE_ANO_ENCODER     4
#define INPUT_TYPE_GPIO_ENCODER    5
#define INPUT_TYPE_GT911_TOUCH     6
#define INPUT_TYPE_IMU             7

// Default input type. BuildConfig owns the user-visible INPUT_DEVICE_TYPE
// flag; this internal INPUT_TYPE is derived from it so HAL code has a single
// constant to switch on regardless of which input device is built.
#ifndef INPUT_TYPE
  #if ENABLE_GT911_TOUCH
    #define INPUT_TYPE INPUT_TYPE_GT911_TOUCH
  #elif ENABLE_IMU_INPUT
    #define INPUT_TYPE INPUT_TYPE_IMU
  #elif defined(INPUT_DEVICE_TYPE) && INPUT_DEVICE_TYPE == 3  /* INPUT_DEVICE_TYPE_GPIO_ENCODER */
    #define INPUT_TYPE  INPUT_TYPE_GPIO_ENCODER
  #elif defined(INPUT_DEVICE_TYPE) && INPUT_DEVICE_TYPE == 2  /* INPUT_DEVICE_TYPE_ANO_ENCODER */
    #define INPUT_TYPE  INPUT_TYPE_ANO_ENCODER
  #else
    #define INPUT_TYPE  INPUT_TYPE_SEESAW_GAMEPAD
  #endif
#endif

// Canonical task name + log tag for the input poller. The single inputTask()
// symbol is provided by either i2csensor_seesaw.cpp or i2csensor_ano_encoder.cpp
// depending on INPUT_TYPE — these macros keep xTaskCreate, the known-task
// table in System_TaskUtils (collectKnownTaskRows), and any
// stack/HWM reports showing the actual driver in use instead of a misleading
// "input_gamepad" on ANO builds. FreeRTOS task name cap is
// CONFIG_FREERTOS_MAX_TASK_NAME_LEN (16) — both names fit.
#if INPUT_TYPE == INPUT_TYPE_GT911_TOUCH
  #define INPUT_TASK_NAME "input_touch"
  #define INPUT_TASK_TAG "touch"
#elif INPUT_TYPE == INPUT_TYPE_IMU
  // No task: the IMU input runs in the IMU task's per-sample hook. The name
  // is only the (empty) row of the known-task table.
  #define INPUT_TASK_NAME "input_imu"
  #define INPUT_TASK_TAG  "imuinput"
#elif INPUT_TYPE == INPUT_TYPE_GPIO_ENCODER
  #define INPUT_TASK_NAME "input_encoder"
  #define INPUT_TASK_TAG  "encoder"
#elif INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
  #define INPUT_TASK_NAME "input_ano"
  #define INPUT_TASK_TAG  "ano"
#else
  #define INPUT_TASK_NAME "input_gamepad"
  #define INPUT_TASK_TAG  "gamepad"
#endif

// =============================================================================
// Gamepad Button Definitions
// =============================================================================
// These are defined in the hardware-specific sensor header (i2csensor_seesaw.h).
// Forward-declare them here for files that need HAL_Input but don't include seesaw.
// The actual values come from i2csensor_seesaw.h when it's included.

#include "i2csensor_seesaw.h"
#include "i2csensor_ano_encoder.h"
#include "Input_GPIOEncoder.h"
#include "Input_IMU.h"

// =============================================================================
// Logical Button Identifiers (hardware-agnostic)
// =============================================================================

enum InputButton {
  INPUT_BUTTON_A,      // Primary action (select/confirm)
  INPUT_BUTTON_B,      // Secondary action (back/cancel)
  INPUT_BUTTON_X,      // Tertiary action (mode/options/toggle)
  INPUT_BUTTON_Y,      // Quaternary action (delete/special)
  INPUT_BUTTON_START,  // Menu/start
  INPUT_BUTTON_SELECT, // Select (if available)
  INPUT_BUTTON_COUNT   // Number of buttons (for array sizing)
};

// =============================================================================
// Controller Type Selection (runtime-switchable)
// =============================================================================

enum InputControllerType {
  INPUT_CONTROLLER_GAMEPAD_SEESAW,  // Adafruit Seesaw gamepad
  INPUT_CONTROLLER_CLICK_WHEEL,     // Generic click wheel / rotary encoder
  INPUT_CONTROLLER_CUSTOM,          // Custom controller mapping
  INPUT_CONTROLLER_ANO_ENCODER,     // Adafruit ANO rotary encoder breakout
  INPUT_CONTROLLER_GPIO_ENCODER,    // Direct GPIO quadrature + push switch
  INPUT_CONTROLLER_GT911_TOUCH,     // Single-contact touchscreen
  INPUT_CONTROLLER_IMU              // Motion sensor: taps and tilt
};

// =============================================================================
// Input Abstraction Functions
// =============================================================================

// Initialize input abstraction layer with default controller type
void inputAbstractionInit();

// Get/set current controller type (runtime switchable)
InputControllerType inputGetControllerType();
void inputSetControllerType(InputControllerType type);

// Get physical button mask for a logical button
uint32_t inputGetButtonMask(InputButton button);

// Check if a logical button is pressed in the given button state
bool inputIsButtonPressed(uint32_t buttonState, InputButton button);

// Custom mapping configuration
void inputSetCustomButtonMapping(InputButton button, uint32_t mask);
uint32_t inputGetCustomButtonMapping(InputButton button);

// =============================================================================
// Device-agnostic data accessors
// =============================================================================
// Replaces gamepadGetX/Y/Buttons() — both gamepad and ANO drivers populate
// the shared input reading, so these work under either build. For ANO, joyX/Y are always
// JOYSTICK_CENTER (no analog deflection); read gNavEvents.wheelDelta for
// rotary state.
// Copies are data-only. Failed reads clear output and leave pending input intact.
bool inputCacheSnapshot(InputCache& out, uint32_t timeoutMs = 10);
// Atomically copies a running, valid reading and consumes its latched presses.
bool inputConsumeSnapshot(InputCache& out, uint32_t timeoutMs = 10);
// On failure, preserve the caller's last sequence (no synthetic activity).
bool inputActivitySequence(uint32_t& out, uint32_t timeoutMs = 10);

// Navigation turns with the screen (owner, 2026-11-23): with the SSD1306
// turned by `oledrotation`, up on the joystick, the IMU tilt and the ANO ring
// is up on the screen as it is seen. The quarter turns the screen is turned
// by (0 off the SSD1306; the P4 canvases keep today's input).
int inputScreenQuarterTurns();
// The OLED navigation's reads: inputCacheSnapshot / inputConsumeSnapshot with
// the joystick turned to the screen. `buttons` stays the physical state for
// every other reader (the ANO's direction buttons are turned where its pins
// become buttons, i2csensor_ano_encoder_core.h). The wheel is never turned:
// clockwise stays clockwise at any rotation.
bool inputNavSnapshot(InputCache& out, uint32_t timeoutMs = 10);
bool inputNavConsume(InputCache& out, uint32_t timeoutMs = 10);
size_t inputCacheStorageBytes();

// Raw panel coordinates, independent of button/joystick data. One UI consumer.
bool inputTouchSnapshot(TouchSnapshot& out);
bool inputTouchPending();
bool inputTouchConsume(TouchEvent& out);
// Discard queued/held contacts across UI ownership changes or display wake.
void inputTouchDiscard();

int       inputGetX();
int       inputGetY();

// Shared rotary channel for any wheel backend. Joysticks return false/zero.
bool      inputHasPendingDetents();
int       inputConsumeOneDetent();

// Blocking wizard readers use the same latched press edges as the main UI.
// Caller owns its previous raw state. First read ignores a held raw switch but
// preserves a completed click that arrived before the first wizard frame.
uint32_t inputConsumeButtonPresses(uint32_t& previousActiveLow, bool& initialized);

// Raw cache buttons (active-LOW at the underlying device's native bit
// positions — gamepad chip bits or ANO_BTN_* bits depending on build).
// Useful when the caller already knows the device. NOT safe to send across
// wire boundaries without translation.
uint32_t  inputGetButtonsRaw();

// Same data normalized to a canonical logical-bit layout (active-HIGH, bit
// position == InputButton enum value):
//    bit 0 = INPUT_BUTTON_A pressed
//    bit 1 = INPUT_BUTTON_B
//    bit 2 = INPUT_BUTTON_X
//    bit 3 = INPUT_BUTTON_Y
//    bit 4 = INPUT_BUTTON_START
//    bit 5 = INPUT_BUTTON_SELECT
// Use this at every wire boundary (MQTT, ESP-NOW, G2, sensor log, JSON API)
// so peers and consumers see consistent bits regardless of which physical
// input device is on the other end.
uint32_t  inputGetButtonsLogical();

// Lower-level: translate an arbitrary device-native cache value to the
// canonical logical-bit layout. Exposed so callers that hold a cache snapshot
// in a struct (e.g., sensor-log replay) can do the translation explicitly.
uint32_t  inputButtonsToLogical(uint32_t deviceCacheButtonsActiveLow);

// =============================================================================
// Convenience Macros
// =============================================================================

#define INPUT_CHECK(state, btn) inputIsButtonPressed(state, btn)
#define INPUT_MASK(btn) inputGetButtonMask(btn)

// =============================================================================
// Joystick Configuration
// =============================================================================
// JOYSTICK_CENTER and JOYSTICK_DEADZONE are defined in i2csensor_seesaw.h

#endif // HAL_INPUT_H
