/**
 * HAL_Input.cpp - Input Hardware Abstraction Layer Implementation
 */

#include "HAL_Input.h"
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif

// =============================================================================
// Static State
// =============================================================================

// Current controller type (can be changed at runtime)
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;

// =============================================================================
// Button Mapping Tables
// =============================================================================

// Gamepad Seesaw button mappings (native - no fallbacks)
#if ENABLE_GAMEPAD_SENSOR
static const uint32_t gGamepadSeesawMapping[] = {
  GAMEPAD_BUTTON_A,      // INPUT_BUTTON_A
  GAMEPAD_BUTTON_B,      // INPUT_BUTTON_B
  GAMEPAD_BUTTON_X,      // INPUT_BUTTON_X
  GAMEPAD_BUTTON_Y,      // INPUT_BUTTON_Y
  GAMEPAD_BUTTON_START,  // INPUT_BUTTON_START
  GAMEPAD_BUTTON_SELECT  // INPUT_BUTTON_SELECT (bit 0)
};
#else
static const uint32_t gGamepadSeesawMapping[] = {
  0, 0, 0, 0, 0, 0
};
#endif

// Click wheel button mappings (example - adjust for your hardware)
static const uint32_t gClickWheelMapping[] = {
  (1 << 0),   // INPUT_BUTTON_A - Center click
  (1 << 1),   // INPUT_BUTTON_B - Back button
  (1 << 2),   // INPUT_BUTTON_X - Menu button
  (1 << 3),   // INPUT_BUTTON_Y - Special button
  (1 << 4),   // INPUT_BUTTON_START - Start button
  (1 << 5)    // INPUT_BUTTON_SELECT - Select button
};

// Custom controller mappings (user-configurable)
static uint32_t gCustomMapping[] = {
  (1 << 5),   // INPUT_BUTTON_A
  (1 << 1),   // INPUT_BUTTON_B
  (1 << 6),   // INPUT_BUTTON_X
  (1 << 4),   // INPUT_BUTTON_Y
  (1 << 16),  // INPUT_BUTTON_START
  (1 << 0)    // INPUT_BUTTON_SELECT
};

// =============================================================================
// Implementation
// =============================================================================

void inputAbstractionInit() {
  // Set default controller type based on compile-time INPUT_TYPE
#if INPUT_TYPE == INPUT_TYPE_SEESAW_GAMEPAD
  gCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#elif INPUT_TYPE == INPUT_TYPE_CLICK_WHEEL
  gCurrentControllerType = INPUT_CONTROLLER_CLICK_WHEEL;
#elif INPUT_TYPE == INPUT_TYPE_CUSTOM
  gCurrentControllerType = INPUT_CONTROLLER_CUSTOM;
#else
  gCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif
  
  Serial.printf("[HAL_INPUT] Initialized with controller type: %d\n", gCurrentControllerType);
#if ENABLE_GAMEPAD_SENSOR
  Serial.printf("[HAL_INPUT] Button mappings: A=0x%08lX B=0x%08lX X=0x%08lX Y=0x%08lX START=0x%08lX\n",
                (unsigned long)GAMEPAD_BUTTON_A, (unsigned long)GAMEPAD_BUTTON_B,
                (unsigned long)GAMEPAD_BUTTON_X, (unsigned long)GAMEPAD_BUTTON_Y,
                (unsigned long)GAMEPAD_BUTTON_START);
#endif
}

InputControllerType inputGetControllerType() {
  return gCurrentControllerType;
}

void inputSetControllerType(InputControllerType type) {
  gCurrentControllerType = type;
  Serial.printf("[HAL_INPUT] Controller type changed to: %d\n", type);
}

uint32_t inputGetButtonMask(InputButton button) {
  // Validate button index
  if (button < INPUT_BUTTON_A || button > INPUT_BUTTON_SELECT) {
    Serial.printf("[HAL_INPUT] ERROR: Invalid button: %d\n", button);
    return 0;
  }
  
  // Get mask from appropriate mapping table
  switch (gCurrentControllerType) {
    case INPUT_CONTROLLER_GAMEPAD_SEESAW:
      return gGamepadSeesawMapping[button];
      
    case INPUT_CONTROLLER_CLICK_WHEEL:
      return gClickWheelMapping[button];
      
    case INPUT_CONTROLLER_CUSTOM:
      return gCustomMapping[button];
      
    default:
      Serial.printf("[HAL_INPUT] ERROR: Unknown controller type: %d\n", gCurrentControllerType);
      return 0;
  }
}

bool inputIsButtonPressed(uint32_t buttonState, InputButton button) {
  uint32_t mask = inputGetButtonMask(button);
  if (mask == 0) {
    return false;
  }
  return (buttonState & mask) != 0;
}

void inputSetCustomButtonMapping(InputButton button, uint32_t mask) {
  if (button >= INPUT_BUTTON_A && button <= INPUT_BUTTON_SELECT) {
    gCustomMapping[button] = mask;
    Serial.printf("[HAL_INPUT] Custom mapping set: button %d = 0x%08lX\n", 
                  button, (unsigned long)mask);
  }
}

uint32_t inputGetCustomButtonMapping(InputButton button) {
  if (button >= INPUT_BUTTON_A && button <= INPUT_BUTTON_SELECT) {
    return gCustomMapping[button];
  }
  return 0;
}
