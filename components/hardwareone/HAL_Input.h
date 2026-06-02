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
 */

#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// =============================================================================
// Input Type Selection (compile-time default)
// =============================================================================

#define INPUT_TYPE_NONE            0
#define INPUT_TYPE_SEESAW_GAMEPAD  1
#define INPUT_TYPE_CLICK_WHEEL     2
#define INPUT_TYPE_CUSTOM          3
#define INPUT_TYPE_ANO_ENCODER     4

// Default input type. BuildConfig owns the user-visible INPUT_DEVICE_TYPE
// flag; this internal INPUT_TYPE is derived from it so HAL code has a single
// constant to switch on regardless of which input device is built.
#ifndef INPUT_TYPE
  #if defined(INPUT_DEVICE_TYPE) && INPUT_DEVICE_TYPE == 2  /* INPUT_DEVICE_TYPE_ANO_ENCODER */
    #define INPUT_TYPE  INPUT_TYPE_ANO_ENCODER
  #else
    #define INPUT_TYPE  INPUT_TYPE_SEESAW_GAMEPAD
  #endif
#endif

// Canonical task name + log tag for the input poller. The single inputTask()
// symbol is provided by either i2csensor_seesaw.cpp or i2csensor_ano_encoder.cpp
// depending on INPUT_TYPE — these macros keep xTaskCreate, the known-task
// tables in System_TaskUtils / System_MemoryMonitor / System_Utils, and any
// stack/HWM reports showing the actual driver in use instead of a misleading
// "gamepad_task" on ANO builds. FreeRTOS task name cap is
// CONFIG_FREERTOS_MAX_TASK_NAME_LEN (16) — both names fit.
#if INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
  #define INPUT_TASK_NAME "ano_task"
  #define INPUT_TASK_TAG  "ano"
#else
  #define INPUT_TASK_NAME "gamepad_task"
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
  INPUT_CONTROLLER_ANO_ENCODER      // Adafruit ANO rotary encoder breakout
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
// gInputCache, so these work under either build. For ANO, joyX/Y are always
// JOYSTICK_CENTER (no analog deflection); read gNavEvents.wheelDelta for
// rotary state.
int       inputGetX();
int       inputGetY();

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
// Unified Input Device CLI + Settings (works under either driver)
// =============================================================================
struct CommandEntry;
struct SettingsModule;
extern const CommandEntry inputCommands[];
extern const size_t inputCommandsCount;
extern const SettingsModule inputSettingsModule;

// =============================================================================
// Joystick Configuration
// =============================================================================
// JOYSTICK_CENTER and JOYSTICK_DEADZONE are defined in i2csensor_seesaw.h

#endif // HAL_INPUT_H
