/**
 * HAL_Input.cpp - Input Hardware Abstraction Layer Implementation
 */

#include "HAL_Input.h"
#include "System_Debug.h"
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"
#endif
#if ENABLE_OLED_INPUT
#include "System_Command.h"
#include "System_Settings.h"
#if ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER
#include "System_I2C.h"
#endif
#include "System_Utils.h"
#endif

// =============================================================================
// Static State
// =============================================================================

// Current controller type. Derive the initial value from the compile-time
// INPUT_TYPE macro (set by BuildConfig's INPUT_DEVICE_TYPE) so the button
// mapping table is correct from the very first INPUT_CHECK — even before
// inputAbstractionInit() runs. The previous hardcoded GAMEPAD_SEESAW
// default meant that under an ANO build (where ENABLE_GAMEPAD_SENSOR == 0
// makes gGamepadSeesawMapping all zeros) every INPUT_CHECK returned false
// until cmd_oledstart's init path happened to also call inputAbstractionInit.
// On a normal boot path that never fires, so IN/A/B/X/Y were dead from
// boot through every menu mode until the user manually restarted the OLED.
// Runtime inputSetControllerType() can still change this later.
#if INPUT_TYPE == INPUT_TYPE_GPIO_ENCODER
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_GPIO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_ANO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_CLICK_WHEEL
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_CLICK_WHEEL;
#elif INPUT_TYPE == INPUT_TYPE_CUSTOM
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_CUSTOM;
#else
static InputControllerType gCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif

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

// Direct GPIO wheel gestures use HAL logical bit positions. Double-click is the
// SELECT/function action; auxiliary board keys supply X, Y and START.
static const uint32_t gGpioEncoderMapping[] = {
  1u, 2u, 4u, 8u, ENABLE_GPIO_ENCODER_BUTTONS ? 16u : 0u, 32u
};

// ANO rotary encoder button mappings. The 4 directional buttons are PURE
// action buttons — they never navigate the keyboard cursor (the wheel does
// that). Layout matches typical text-entry intuition:
//   IN     → A (primary / type char)
//   LEFT   → B (back / cancel)
//   UP     → Y (keyboard "backspace" — top button erases)
//   DOWN   → X (keyboard "submit" — bottom button enters)
//   RIGHT  → SELECT (the "function" key — currently bound to keyboard
//                    autocomplete / mode-toggle by oledKeyboardHandleInput;
//                    other modes may consume it for quick-settings open)
//   RIGHT+IN chord → START (synthesized by the driver as ANO_VIRT_START)
#if ENABLE_ANO_ENCODER
static const uint32_t gAnoEncoderMapping[] = {
  ANO_BTN_IN,      // INPUT_BUTTON_A      (center = type / select)
  ANO_BTN_LEFT,    // INPUT_BUTTON_B      (back / cancel)
  ANO_BTN_DOWN,    // INPUT_BUTTON_X      (submit / complete)
  ANO_BTN_UP,      // INPUT_BUTTON_Y      (backspace)
  ANO_VIRT_START,  // INPUT_BUTTON_START  (synthesized by RIGHT+IN chord)
  ANO_BTN_RIGHT    // INPUT_BUTTON_SELECT (the "function" button)
};
#else
static const uint32_t gAnoEncoderMapping[] = { 0, 0, 0, 0, 0, 0 };
#endif

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
#elif INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
  gCurrentControllerType = INPUT_CONTROLLER_ANO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_GPIO_ENCODER
  gCurrentControllerType = INPUT_CONTROLLER_GPIO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_CUSTOM
  gCurrentControllerType = INPUT_CONTROLLER_CUSTOM;
#else
  gCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif
  
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Initialized with controller type: %d", gCurrentControllerType);
#if ENABLE_GAMEPAD_SENSOR
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Button mappings: A=0x%08lX B=0x%08lX X=0x%08lX Y=0x%08lX START=0x%08lX",
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
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Controller type changed to: %d", type);
}

uint32_t inputGetButtonMask(InputButton button) {
  // Validate button index
  if (button < INPUT_BUTTON_A || button > INPUT_BUTTON_SELECT) {
    ERROR_INPUTF("[HAL_INPUT] Invalid button: %d", button);
    return 0;
  }
  
  // Get mask from appropriate mapping table
  switch (gCurrentControllerType) {
    case INPUT_CONTROLLER_GAMEPAD_SEESAW:
      return gGamepadSeesawMapping[button];

    case INPUT_CONTROLLER_CLICK_WHEEL:
      return gClickWheelMapping[button];

    case INPUT_CONTROLLER_ANO_ENCODER:
      return gAnoEncoderMapping[button];

    case INPUT_CONTROLLER_GPIO_ENCODER:
      return gGpioEncoderMapping[button];

    case INPUT_CONTROLLER_CUSTOM:
      return gCustomMapping[button];

    default:
      ERROR_INPUTF("[HAL_INPUT] Unknown controller type: %d", gCurrentControllerType);
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
    DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Custom mapping set: button %d = 0x%08lX",
                  button, (unsigned long)mask);
  }
}

uint32_t inputGetCustomButtonMapping(InputButton button) {
  if (button >= INPUT_BUTTON_A && button <= INPUT_BUTTON_SELECT) {
    return gCustomMapping[button];
  }
  return 0;
}

// ============================================================================
// Device-agnostic data accessors
// ============================================================================
// Both gamepad and ANO drivers populate gInputCache (the gamepad-shaped proxy
// — joyX/Y + buttons), so these accessors work under either build. The
// previous gamepadGetX/Y/Buttons functions in i2csensor_seesaw.cpp are
// gamepad-only by virtue of where they live; this generalisation lives in
// the HAL so it's available regardless of which driver is compiled.

int inputGetX() {
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.joyX;
}

int inputGetY() {
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.joyY;
}

uint32_t inputGetButtonsRaw() {
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.buttons;
}

uint32_t inputButtonsToLogical(uint32_t deviceCacheButtonsActiveLow) {
  // Step 1: invert from active-low cache convention to active-high "pressed"
  // mask at the underlying device's native bit positions. For gamepad: chip
  // pin bits. For ANO: ANO_BTN_* positions.
  uint32_t pressed = ~deviceCacheButtonsActiveLow;
  // Step 2: walk every logical button, look up its device-native mask via
  // the active mapping table, and set the corresponding logical bit (== enum
  // value) if the device bits show that logical button as pressed.
  uint32_t logical = 0;
  for (int b = INPUT_BUTTON_A; b < INPUT_BUTTON_COUNT; b++) {
    uint32_t mask = inputGetButtonMask((InputButton)b);
    if (mask != 0 && (pressed & mask) == mask) {
      logical |= (1u << b);
    }
  }
  return logical;
}

uint32_t inputGetButtonsLogical() {
  // A stopped/missing device has no pressed buttons. Its raw accessor's zero
  // sentinel must not be inverted into every logical button being pressed.
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return inputButtonsToLogical(inputGetButtonsRaw());
}

bool inputHasPendingDetents() {
#if ENABLE_GPIO_ENCODER
  return gpioEncoderHasPendingDetents();
#elif ENABLE_ANO_ENCODER
  return gAnoEncoderCache.encoderDelta != 0;
#else
  return false;
#endif
}

int inputConsumeOneDetent() {
#if ENABLE_GPIO_ENCODER
  return gpioEncoderConsumeOneDetent();
#elif ENABLE_ANO_ENCODER
  return anoEncoderConsumeOneDetent();
#else
  return 0;
#endif
}

uint32_t inputConsumeButtonPresses(uint32_t& previousActiveLow, bool& initialized) {
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
  if (!gInputRunning || !gInputCache.dataValid) {
    xSemaphoreGive(gInputCache.mutex);
    return 0;
  }
  const uint32_t raw = gInputCache.buttons;
  uint32_t pressed = gInputCache.buttonPressedAccum;
  gInputCache.buttonPressedAccum = 0;
  if (initialized) pressed |= ~raw & previousActiveLow;
  previousActiveLow = raw;
  initialized = true;
  xSemaphoreGive(gInputCache.mutex);
  return pressed;
}

#if ENABLE_OLED_INPUT
// ============================================================================
// Unified Input Device CLI + Settings (driver-agnostic abstraction surface)
// ============================================================================
// These commands work under either driver (Seesaw gamepad or ANO encoder).
// Driver-internal CLIs that dump raw device state stay in their own driver
// files (gamepadread, anoencoderread).

#if !ENABLE_GPIO_ENCODER
static const char* inputDeviceLabel() {
#if ENABLE_ANO_ENCODER
  return "ANO Encoder";
#else
  return "Gamepad";
#endif
}

// Defined in System_I2C.cpp — generic queued-start dispatcher.
extern const char* cmd_sensorstart_queued(I2CDeviceType sensor, const char* displayName,
                                          const bool& enabledFlag, const char* eventTag);
#endif

static const char* cmd_openinput(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
#if ENABLE_GPIO_ENCODER
  return inputStartInternal() ? "[Input] GPIO encoder started" : "Error: GPIO encoder start failed";
#else
  const char* result = cmd_sensorstart_queued(I2C_DEVICE_INPUT, inputDeviceLabel(),
                                              gInputRunning, "openinput@enqueue");
  // The device starts asynchronously and this command returns no button state —
  // point at the driver read command so the next step is to read live input.
#if ENABLE_ANO_ENCODER
  cliHint("to read live input, run 'anoencoderread' once the device is up");
#else
  cliHint("to read live input, run 'gamepadread' once the device is up");
#endif
  return result;
#endif
}

static const char* cmd_closeinput(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  INFO_INPUT_LIFECYCLEF("cmd_closeinput: Stop requested");
#if ENABLE_GPIO_ENCODER
  gpioEncoderStop();
  return gInputRunning ? "Error: GPIO encoder is busy; retry closeinput" : "[Input] GPIO encoder stopped";
#else
  handleDeviceStopped(I2C_DEVICE_INPUT);
  return "[Input] Stop requested; cleanup will complete asynchronously";
#endif
}

static const char* cmd_inputautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.inputAutoStart ? "[Input] Auto-start: enabled" : "[Input] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.inputAutoStart, true);
    return "[Input] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.inputAutoStart, false);
    return "[Input] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: inputautostart [on|off]";
}

static const char* cmd_inputdevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
#if ENABLE_GPIO_ENCODER
  return "[Input] GPIO encoder uses interrupts and a fixed 5 ms switch sample; poll interval applies to I2C input only";
#else
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "[Input] Poll interval: %d ms", gSettings.inputDevicePollMs);
    return buf;
  }
  int ms = a.argInt(0, 0);
  if (ms < 10 || ms > 1000) return "Error: invalid arguments — Usage: inputdevicepollms <10-1000>";
  setSetting(gSettings.inputDevicePollMs, ms);
  return "[Input] Poll interval updated";
#endif
}

#if ENABLE_GPIO_ENCODER
static const char* cmd_gpioencoderread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[256];
  return gpioEncoderBuildDataJSON(buf, sizeof(buf)) > 0 ? buf : "Error: GPIO encoder snapshot unavailable";
}
#endif

// Command registry — registered unconditionally as the "input" module since
// it works under either driver.
const CommandEntry inputCommands[] = {
  { "openinput",         "Start the configured input device.",              false, cmd_openinput },
  { "closeinput",        "Stop the input device.",                            false, cmd_closeinput },
  { "inputautostart",    "Enable/disable input device auto-start [on|off]",   false, cmd_inputautostart,    "Usage: inputautostart [on|off]" },
  { "inputdevicepollms", "Set input device poll interval ms [10-1000]",       true,  cmd_inputdevicepollms, "Usage: inputdevicepollms <10-1000>" },
#if ENABLE_GPIO_ENCODER
  { "gpioencoderread",   "Read GPIO wheel position, switch and pending input as JSON.", false, cmd_gpioencoderread },
#endif
};
const size_t inputCommandsCount = sizeof(inputCommands) / sizeof(inputCommands[0]);

// Settings module — JSON section `hardware.input` holds the device-agnostic
// settings. Driver-specific entries (anoEncoderI2cAddr, anoEncoderInvert)
// live in anoEncoderSettingsModule.
static const SettingEntry inputSettingEntries[] = {
  { "inputEnabled", SETTING_BOOL, &gSettings.inputEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "inputenabled" },
  { "inputAutoStart",    SETTING_BOOL, &gSettings.inputAutoStart,     0, 0, nullptr, 0,  1,    "Auto-start after boot", nullptr, false, nullptr, "inputautostart" },
#if !ENABLE_GPIO_ENCODER
  { "inputDevicePollMs", SETTING_INT,  &gSettings.inputDevicePollMs, 90, 0, nullptr, 10, 1000, "Poll Interval (ms)",   nullptr, false, nullptr, "inputdevicepollms" },
#endif
};

static bool isInputConnected() { return gInputConnected; }

extern const SettingsModule inputSettingsModule = {
  "input",
  // Live under hardware.sensors.* so the web Settings page groups it with
  // the other sensors (the page splits modules by jsonSection prefix).
  "hardware.sensors.input",
  inputSettingEntries,
  sizeof(inputSettingEntries) / sizeof(inputSettingEntries[0]),
  isInputConnected,
  "Local display input device (Seesaw, ANO or GPIO encoder)"
};

#endif // ENABLE_OLED_INPUT
