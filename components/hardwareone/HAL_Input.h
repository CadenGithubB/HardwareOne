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

// Default input type if not specified in System_BuildConfig.h
#ifndef INPUT_TYPE
  #define INPUT_TYPE  INPUT_TYPE_SEESAW_GAMEPAD
#endif

// =============================================================================
// Gamepad Button Definitions
// =============================================================================
// These are defined in the hardware-specific sensor header (i2csensor-seesaw.h).
// Forward-declare them here for files that need HAL_Input but don't include seesaw.
// The actual values come from i2csensor-seesaw.h when it's included.

#include "i2csensor-seesaw.h"

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
  INPUT_CONTROLLER_GAMEPAD_SEESAW,  // Adafruit Seesaw gamepad (current default)
  INPUT_CONTROLLER_CLICK_WHEEL,     // Click wheel / rotary encoder
  INPUT_CONTROLLER_CUSTOM           // Custom controller mapping
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
// Convenience Macros
// =============================================================================

#define INPUT_CHECK(state, btn) inputIsButtonPressed(state, btn)
#define INPUT_MASK(btn) inputGetButtonMask(btn)

// =============================================================================
// Joystick Configuration
// =============================================================================
// JOYSTICK_CENTER and JOYSTICK_DEADZONE are defined in i2csensor-seesaw.h

#endif // HAL_INPUT_H
