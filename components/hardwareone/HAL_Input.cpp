/**
 * HAL_Input.cpp - Input Hardware Abstraction Layer Implementation
 */

#include "HAL_Input.h"
#include "HAL_InputInternal.h"
#include "HAL_Display.h"         // displayRotation: navigation turns with the screen
#include "Input_RotationCore.h"
#include "System_Mutex.h"
#include "System_Debug.h"
#include "i2csensor_seesaw.h"
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"
#endif
#if ENABLE_IMU_INPUT
#include "HAL_Motion.h"  // gImuRunning: the openinput reply says whether the IMU is open yet
#endif
#if ENABLE_OLED_INPUT
#include "System_Command.h"
#include "System_Settings.h"
#if ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH
#include "System_I2C.h"
#endif
#include "System_Utils.h"
#include "System_CommandRegistry.h"
// The open/close argument check is pure (no System_I2C.h): the GPIO encoder
// build has no I2C input and may have no I2C system at all.
#include "System_I2CInstanceCore.h"
#endif

// Shared input readings and edge ownership, independent of the selected driver.
#if ENABLE_OLED_INPUT
static InputCache sInputCache;
#if ENABLE_GT911_TOUCH
static TouchQueue sTouch;
#endif
static StaticSemaphore_t sInputCacheMutexStorage;
static SemaphoreHandle_t sInputCacheMutex = xSemaphoreCreateMutexStatic(&sInputCacheMutexStorage);
#endif

bool inputCacheInit() {
#if ENABLE_OLED_INPUT
  return sInputCacheMutex != nullptr;
#else
  return false;
#endif
}

size_t inputCacheStorageBytes() {
#if ENABLE_OLED_INPUT
  return sizeof(sInputCache) + sizeof(sInputCacheMutex) + sizeof(sInputCacheMutexStorage)
#if ENABLE_GT911_TOUCH
      + sizeof(sTouch)
#endif
      ;
#else
  return 0;
#endif
}

#if ENABLE_IMU_INPUT
// The IMU input publishes on every motion sample (20 ms) and has no task of
// its own to notice when the samples stop (a paused bus, the IMU closed). Its
// last deflection must not go on scrolling a menu, so a reading older than
// this is handed out centred and released. Latched presses are kept.
static constexpr uint32_t kImuInputStaleMs = 250;
static void imuInputExpire(InputCache& reading) {
  if (static_cast<uint32_t>(millis() - reading.lastUpdate) <= kImuInputStaleMs) return;
  reading.joyX = reading.joyY = JOYSTICK_CENTER;
  reading.buttons = UINT32_MAX;
}
#endif

bool inputCacheSnapshot(InputCache& out, uint32_t timeoutMs) {
  out = {};
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(timeoutMs), "input.snapshot");
  if (!guard.held) return false;
  out = sInputCache;
#if ENABLE_IMU_INPUT
  imuInputExpire(out);
#endif
  return true;
#else
  (void)timeoutMs;
  return false;
#endif
}

bool inputConsumeSnapshot(InputCache& out, uint32_t timeoutMs) {
  out = {};
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(timeoutMs), "input.consume");
  if (!guard.held || !gInputRunning || !sInputCache.dataValid) return false;
  out = sInputCache;
  sInputCache.buttonPressedAccum = 0;
#if ENABLE_IMU_INPUT
  imuInputExpire(out);
#endif
  return true;
#else
  (void)timeoutMs;
  return false;
#endif
}

bool inputActivitySequence(uint32_t& out, uint32_t timeoutMs) {
  InputCache reading;
  const bool copied = inputCacheSnapshot(reading, timeoutMs);
  if (copied) out = reading.seq;
  return copied;
}

bool inputCacheInvalidate() {
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(100), "input.invalidate");
  if (!guard.held) return false;
  sInputCache.dataValid = false;
  return true;
#else
  return false;
#endif
}

bool inputCacheReset(uint32_t buttons, int joyX, int joyY, bool clearPresses) {
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(100), "input.reset");
  if (!guard.held) return false;
  sInputCache.buttons = buttons;
  sInputCache.navHidden = 0;
  sInputCache.joyX = joyX;
  sInputCache.joyY = joyY;
  sInputCache.dataValid = false;
  if (clearPresses) sInputCache.buttonPressedAccum = 0;
  return true;
#else
  (void)buttons; (void)joyX; (void)joyY; (void)clearPresses;
  return false;
#endif
}

#if ENABLE_OLED_INPUT
static void publishInputLocked(uint32_t buttons, int joyX, int joyY,
                               uint32_t pressedEdges, bool activity, unsigned long stamp,
                               uint32_t navHidden) {
  sInputCache.buttons = buttons;
  sInputCache.navHidden = navHidden;
  sInputCache.joyX = joyX;
  sInputCache.joyY = joyY;
  sInputCache.buttonPressedAccum |= pressedEdges;
  sInputCache.lastUpdate = stamp;
  sInputCache.dataValid = true;
  if (activity) ++sInputCache.seq;
}
#endif

bool inputPublishState(uint32_t buttons, int joyX, int joyY,
                       uint32_t pressedEdges, bool activity, unsigned long stamp,
                       uint32_t navHidden, uint32_t waitMs) {
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(waitMs), "input.publish");
  if (!guard.held) return false;
  publishInputLocked(buttons, joyX, joyY, pressedEdges, activity, stamp, navHidden);
  return true;
#else
  (void)buttons; (void)joyX; (void)joyY; (void)pressedEdges; (void)activity; (void)stamp;
  (void)navHidden; (void)waitMs;
  return false;
#endif
}

bool inputPublishGamepadState(uint32_t buttons, int joyX, int joyY,
                              uint32_t interruptPresses, unsigned long stamp) {
#if ENABLE_OLED_INPUT
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(50), "input.gamepadPublish");
  if (!guard.held) return false;
  const bool activity = sInputCache.buttons != buttons ||
                        abs(sInputCache.joyX - joyX) > 1 || abs(sInputCache.joyY - joyY) > 1;
  // Active-low transitions plus the hardware latch preserve short completed taps.
  const uint32_t presses = (sInputCache.buttons & ~buttons) | interruptPresses;
  publishInputLocked(buttons, joyX, joyY, presses, activity, stamp, 0);
  return true;
#else
  (void)buttons; (void)joyX; (void)joyY; (void)interruptPresses; (void)stamp;
  return false;
#endif
}

// =============================================================================
// Static State
// =============================================================================

// Current controller type. Derive the initial value from the compile-time
// INPUT_TYPE macro (set by BuildConfig's INPUT_DEVICE_TYPE) so the button
// mapping table is correct from the very first INPUT_CHECK — even before
// inputAbstractionInit() runs. The previous hardcoded GAMEPAD_SEESAW
// default meant that under an ANO build (where ENABLE_GAMEPAD_SENSOR == 0
// makes kGamepadSeesawMapping all zeros) every INPUT_CHECK returned false
// until cmd_oledstart's init path happened to also call inputAbstractionInit.
// On a normal boot path that never fires, so IN/A/B/X/Y were dead from
// boot through every menu mode until the user manually restarted the OLED.
// Runtime inputSetControllerType() can still change this later.
#if INPUT_TYPE == INPUT_TYPE_GT911_TOUCH
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_GT911_TOUCH;
#elif INPUT_TYPE == INPUT_TYPE_IMU
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_IMU;
#elif INPUT_TYPE == INPUT_TYPE_GPIO_ENCODER
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_GPIO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_ANO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_CLICK_WHEEL
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_CLICK_WHEEL;
#elif INPUT_TYPE == INPUT_TYPE_CUSTOM
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_CUSTOM;
#else
static InputControllerType sCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif

// =============================================================================
// Button Mapping Tables
// =============================================================================

// Gamepad Seesaw button mappings (native - no fallbacks)
#if ENABLE_GAMEPAD_SENSOR
static const uint32_t kGamepadSeesawMapping[] = {
  GAMEPAD_BUTTON_A,      // INPUT_BUTTON_A
  GAMEPAD_BUTTON_B,      // INPUT_BUTTON_B
  GAMEPAD_BUTTON_X,      // INPUT_BUTTON_X
  GAMEPAD_BUTTON_Y,      // INPUT_BUTTON_Y
  GAMEPAD_BUTTON_START,  // INPUT_BUTTON_START
  GAMEPAD_BUTTON_SELECT  // INPUT_BUTTON_SELECT (bit 0)
};
#else
static const uint32_t kGamepadSeesawMapping[] = {
  0, 0, 0, 0, 0, 0
};
#endif

// Click wheel button mappings (example - adjust for your hardware)
static const uint32_t kClickWheelMapping[] = {
  (1 << 0),   // INPUT_BUTTON_A - Center click
  (1 << 1),   // INPUT_BUTTON_B - Back button
  (1 << 2),   // INPUT_BUTTON_X - Menu button
  (1 << 3),   // INPUT_BUTTON_Y - Special button
  (1 << 4),   // INPUT_BUTTON_START - Start button
  (1 << 5)    // INPUT_BUTTON_SELECT - Select button
};

// Direct GPIO wheel gestures use HAL logical bit positions: click=A,
// double-click=B, tap-then-hold=SELECT/function. Auxiliary keys supply X, Y and START.
static const uint32_t kGpioEncoderMapping[] = {
  1u, 2u, 4u, 8u, ENABLE_GPIO_ENCODER_BUTTONS ? 16u : 0u, 32u
};

// IMU input (Input_IMUCore.h) publishes HAL logical bit positions: one tap
// = A, two = B, three = Y, four = X, a shake = START. SELECT has no gesture.
static const uint32_t kImuInputMapping[] = {
  1u, 2u, 4u, 8u, 16u, 0u
};

// ANO rotary encoder button mappings. The 4 directional buttons are action
// buttons; the wheel does the up/down navigation. Layout:
//   IN     → A (primary / type char)
//   LEFT   → B (back / cancel)
//   UP     → Y (keyboard "backspace" — top button erases)
//   DOWN   → X (keyboard "submit" — bottom button enters)
//   RIGHT  → SELECT at its release (ANO_VIRT_SELECT, an edge the driver makes
//            for a RIGHT tap: keyboard autocomplete / mode toggle, Quick
//            Settings outside the menu). SELECT only: no right step.
//            The cache keeps RIGHT held (the physical state); the navigation
//            ignores it through InputCache::navHidden.
//   RIGHT held + IN → START only (ANO_VIRT_START; no SELECT, no A)
// The rules are anoPoll()'s (i2csensor_ano_encoder_core.h, host-tested).
#if ENABLE_ANO_ENCODER
static const uint32_t kAnoEncoderMapping[] = {
  ANO_BTN_IN,       // INPUT_BUTTON_A      (center = type / select)
  ANO_BTN_LEFT,     // INPUT_BUTTON_B      (back / cancel)
  ANO_BTN_DOWN,     // INPUT_BUTTON_X      (submit / complete)
  ANO_BTN_UP,       // INPUT_BUTTON_Y      (backspace)
  ANO_VIRT_START,   // INPUT_BUTTON_START  (RIGHT held + IN)
  ANO_VIRT_SELECT   // INPUT_BUTTON_SELECT (RIGHT tap, at its release)
};
#else
static const uint32_t kAnoEncoderMapping[] = { 0, 0, 0, 0, 0, 0 };
#endif

// Custom controller mappings (user-configurable)
static uint32_t sCustomMapping[] = {
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
#if INPUT_TYPE == INPUT_TYPE_GT911_TOUCH
  sCurrentControllerType = INPUT_CONTROLLER_GT911_TOUCH;
#elif INPUT_TYPE == INPUT_TYPE_IMU
  sCurrentControllerType = INPUT_CONTROLLER_IMU;
#elif INPUT_TYPE == INPUT_TYPE_SEESAW_GAMEPAD
  sCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#elif INPUT_TYPE == INPUT_TYPE_CLICK_WHEEL
  sCurrentControllerType = INPUT_CONTROLLER_CLICK_WHEEL;
#elif INPUT_TYPE == INPUT_TYPE_ANO_ENCODER
  sCurrentControllerType = INPUT_CONTROLLER_ANO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_GPIO_ENCODER
  sCurrentControllerType = INPUT_CONTROLLER_GPIO_ENCODER;
#elif INPUT_TYPE == INPUT_TYPE_CUSTOM
  sCurrentControllerType = INPUT_CONTROLLER_CUSTOM;
#else
  sCurrentControllerType = INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif
  
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Initialized with controller type: %d", sCurrentControllerType);
#if ENABLE_GAMEPAD_SENSOR
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Button mappings: A=0x%08lX B=0x%08lX X=0x%08lX Y=0x%08lX START=0x%08lX",
                (unsigned long)GAMEPAD_BUTTON_A, (unsigned long)GAMEPAD_BUTTON_B,
                (unsigned long)GAMEPAD_BUTTON_X, (unsigned long)GAMEPAD_BUTTON_Y,
                (unsigned long)GAMEPAD_BUTTON_START);
#endif
}

InputControllerType inputGetControllerType() {
  return sCurrentControllerType;
}

void inputSetControllerType(InputControllerType type) {
  sCurrentControllerType = type;
  DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Controller type changed to: %d", type);
}

uint32_t inputGetButtonMask(InputButton button) {
  // Validate button index
  if (button < INPUT_BUTTON_A || button > INPUT_BUTTON_SELECT) {
    ERROR_INPUTF("[HAL_INPUT] Invalid button: %d", button);
    return 0;
  }
  
  // Get mask from appropriate mapping table
  switch (sCurrentControllerType) {
    case INPUT_CONTROLLER_GAMEPAD_SEESAW:
      return kGamepadSeesawMapping[button];

    case INPUT_CONTROLLER_GT911_TOUCH:
    case INPUT_CONTROLLER_CLICK_WHEEL:
      return kClickWheelMapping[button];

    case INPUT_CONTROLLER_ANO_ENCODER:
      return kAnoEncoderMapping[button];

    case INPUT_CONTROLLER_GPIO_ENCODER:
      return kGpioEncoderMapping[button];

    case INPUT_CONTROLLER_IMU:
      return kImuInputMapping[button];

    case INPUT_CONTROLLER_CUSTOM:
      return sCustomMapping[button];

    default:
      ERROR_INPUTF("[HAL_INPUT] Unknown controller type: %d", sCurrentControllerType);
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
    sCustomMapping[button] = mask;
    DEBUG_INPUT_LIFECYCLEF("[HAL_INPUT] Custom mapping set: button %d = 0x%08lX",
                  button, (unsigned long)mask);
  }
}

uint32_t inputGetCustomButtonMapping(InputButton button) {
  if (button >= INPUT_BUTTON_A && button <= INPUT_BUTTON_SELECT) {
    return sCustomMapping[button];
  }
  return 0;
}

// ============================================================================
// Device-agnostic data accessors
// ============================================================================
// Both gamepad and ANO drivers populate the shared input reading (the gamepad-shaped proxy
// — joyX/Y + buttons), so these accessors work under either build. The
// previous gamepadGetX/Y/Buttons functions in i2csensor_seesaw.cpp are
// gamepad-only by virtue of where they live; this generalisation lives in
// the HAL so it's available regardless of which driver is compiled.

// Navigation turns with the screen (HAL_Input.h).
int inputScreenQuarterTurns() {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  return displayRotation() & 3;
#else
  return 0;
#endif
}

static void inputTurnToScreen(InputCache& reading) {
  input_rotation::rotateStick(inputScreenQuarterTurns(), JOYSTICK_CENTER, reading.joyX, reading.joyY);
}

bool inputNavSnapshot(InputCache& out, uint32_t timeoutMs) {
  const bool ok = inputCacheSnapshot(out, timeoutMs);
  if (ok) inputTurnToScreen(out);
  return ok;
}

bool inputNavConsume(InputCache& out, uint32_t timeoutMs) {
  const bool ok = inputConsumeSnapshot(out, timeoutMs);
  if (ok) inputTurnToScreen(out);
  return ok;
}

int inputGetX() {
  InputCache reading;
  if (!gInputConnected || !inputCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.joyX;
}

int inputGetY() {
  InputCache reading;
  if (!gInputConnected || !inputCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.joyY;
}

uint32_t inputGetButtonsRaw() {
  InputCache reading;
  if (!gInputConnected || !inputCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.buttons;
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
  InputCache reading;
  if (!gInputConnected || !inputCacheSnapshot(reading) || !reading.dataValid) return 0;
  return inputButtonsToLogical(inputNavButtons(reading));
}

bool inputHasPendingDetents() {
#if ENABLE_GPIO_ENCODER
  return gpioEncoderHasPendingDetents();
#elif ENABLE_ANO_ENCODER
  AnoEncoderCache reading;
  return anoEncoderCacheSnapshot(reading, 0) && reading.encoderDelta != 0;
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
  InputCache reading;
  if (!inputConsumeSnapshot(reading)) return 0;
  // The navigation's view: the ANO's RIGHT and chord IN are not held here.
  const uint32_t buttons = inputNavButtons(reading);
  uint32_t pressed = reading.buttonPressedAccum;
  if (initialized) pressed |= ~buttons & previousActiveLow;
  previousActiveLow = buttons;
  initialized = true;
  return pressed;
}

#if ENABLE_OLED_INPUT
// ============================================================================
// Unified Input Device CLI + Settings (driver-agnostic abstraction surface)
// ============================================================================
// These commands work under either driver (Seesaw gamepad or ANO encoder).
// Driver-internal CLIs that dump raw device state stay in their own driver
// files (gamepadread, anoencoderread).

#if !ENABLE_INPUT_DIRECT_START
static const char* inputDeviceLabel() {
#if ENABLE_GT911_TOUCH
  return "GT911 Touch";
#elif ENABLE_ANO_ENCODER
  return "ANO Encoder";
#else
  return "Gamepad";
#endif
}

#endif

// "openinput 2" and the like: the input has one instance on every backend.
static const char* inputOpenCloseArgError(const String& argsInput, const char* usage) {
  char* buf = ensureDebugBuffer() ? getDebugBuffer() : nullptr;
  return i2cOpenCloseArgErrorText(I2C_DEVICE_INPUT, argsInput.c_str(), usage, buf, buf ? 1024 : 0);
}

static const char* cmd_openinput(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = inputOpenCloseArgError(argsInput, "openinput")) return error;
#if ENABLE_GPIO_ENCODER
  // gpioEncoderStart() reports a running encoder as Started; the command
  // answers it as a refusal.
  if (gInputRunning) {
    return "Error: GPIO encoder is already running. Stop it first with 'closeinput' to restart it.";
  }
  switch (gpioEncoderStart()) {
    case GpioEncoderStart::Started:
      break;
    case GpioEncoderStart::PinsInvalid:
      return "Error: GPIO encoder pins are not valid on this board. "
             "Correct the encoder pins in the board profile and rebuild.";
    case GpioEncoderStart::CacheMissing:
      return "Error: Input cache is not available. Reboot the device and retry openinput.";
    case GpioEncoderStart::OutOfMemory:
      return kCommandReplyOutOfMemory;
    case GpioEncoderStart::Busy:
      return "Error: GPIO encoder is busy. Retry openinput in a few seconds.";
    case GpioEncoderStart::TaskNotCreated:
      return "Error: GPIO encoder task could not be created. Free memory and retry openinput.";
  }
  return "[Input] GPIO encoder started";
#elif ENABLE_IMU_INPUT
  if (gInputRunning && gInputConnected) {
    return "Error: IMU input is already running. Stop it first with 'closeinput' to restart it.";
  }
  if (const char* why = imuInputStartReply()) return why;
  return gImuRunning && gImuConnected
             ? "[Input] IMU input started: tap for buttons, tilt for the joystick; to read it, run 'imuinputread'"
             : "[Input] IMU input started; the IMU is opening and taps and tilt work from its first sample "
               "(to read it, run 'imuinputread')";
#else
  // The device starts asynchronously and this command returns no button state —
  // point at the driver read command so the next step is to read live input.
#if ENABLE_GT911_TOUCH
  static const char* const kReadHint = "to inspect touch, run touchread";
#elif ENABLE_ANO_ENCODER
  static const char* const kReadHint = "to read live input, run 'anoencoderread' once the device is up";
#else
  static const char* const kReadHint = "to read live input, run 'gamepadread' once the device is up";
#endif
  return cmd_sensorstart_queued(I2C_DEVICE_INPUT, inputDeviceLabel(), "openinput@enqueue", kReadHint);
#endif
}

static const char* cmd_closeinput(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = inputOpenCloseArgError(argsInput, "closeinput")) return error;
  INFO_INPUT_LIFECYCLEF("cmd_closeinput: Stop requested");
#if ENABLE_GPIO_ENCODER
  if (!gInputRunning) {
    return "Error: GPIO encoder is not running. Start it with 'openinput'.";
  }
  gpioEncoderStop();
  return gInputRunning ? "Error: GPIO encoder is busy; retry closeinput" : "[Input] GPIO encoder stopped";
#elif ENABLE_IMU_INPUT
  if (!gInputRunning) {
    return "Error: IMU input is not running. Start it with 'openinput'.";
  }
  imuInputStop();
  return gInputRunning ? "Error: IMU input is busy; retry closeinput"
                       : "[Input] IMU input stopped. The IMU stays open; close it with 'closeimu'.";
#else
  // Same replies as every sensor stop: an Error when the input is not running
  // (bus disabled or nothing open), an Error when the request queue is missing.
  return cmd_sensorstop_queued(I2C_DEVICE_INPUT, "Input");
#endif
}

static const char* cmd_inputautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.inputAutoStart ? "[Input] Auto-start: enabled" : "[Input] Auto-start: disabled";
  }
  const int parsed = parseBoolArg(arg);
  if (parsed < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.inputAutoStart, parsed == 1);
  return parsed == 1 ? "[Input] Auto-start enabled" : "[Input] Auto-start disabled";
}

static const char* cmd_inputdevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
#if ENABLE_GT911_TOUCH
  if (CommandArgs(argsInput).count() > 0) {
    return "Error: The poll interval is fixed at 20 ms on the GT911 touch input. Nothing to set.";
  }
  return "[Input] GT911 uses a fixed 20 ms sample interval";
#elif ENABLE_GPIO_ENCODER
  if (CommandArgs(argsInput).count() > 0) {
    return "Error: The GPIO encoder has no poll interval. Nothing to set.";
  }
  return "[Input] GPIO encoder uses interrupts and a fixed 5 ms switch sample; poll interval applies to I2C input only";
#elif ENABLE_IMU_INPUT
  if (CommandArgs(argsInput).count() > 0) {
    return "Error: IMU input has no poll interval of its own: it reads every sample of the IMU. Nothing to set.";
  }
  return "[Input] IMU input reads every sample of the IMU (20 ms on the Adafruit 4517)";
#else
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    EXT_RAM_BSS_ATTR static char buf[48];  // PSRAM: the reply is copied by the executor
    snprintf(buf, sizeof(buf), "[Input] Poll interval: %d ms", gSettings.inputDevicePollMs);
    return buf;
  }
  int ms = 0;
  if (!parseIntArg(a.arg(0), ms)) return kCommandReplyNotANumber;
  if (ms < 10 || ms > 1000) return "Error: The poll interval must be 10..1000 ms.";
  setSetting(gSettings.inputDevicePollMs, ms);
  return "[Input] Poll interval updated";
#endif
}

#if ENABLE_GPIO_ENCODER
static const char* cmd_gpioencoderread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[256];  // PSRAM: the reply is copied by the executor
  return gpioEncoderBuildDataJson(buf, sizeof(buf)) > 0 ? buf : "Error: GPIO encoder snapshot unavailable";
}
#endif

#if ENABLE_GT911_TOUCH
static const char* cmd_touchread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  TouchSnapshot touch;
  if (!inputTouchSnapshot(touch)) return "Error: touch snapshot unavailable";
  static char buf[192];
  snprintf(buf, sizeof(buf), "{\"connected\":%s,\"valid\":%s,\"down\":%s,\"x\":%u,\"y\":%u,\"sequence\":%lu}",
           gInputConnected ? "true" : "false", touch.valid ? "true" : "false", touch.down ? "true" : "false",
           touch.contact.x, touch.contact.y, static_cast<unsigned long>(touch.contact.sequence));
  return buf;
}
#endif

// Command registry — registered unconditionally as the "input" module since
// it works under either driver.
const CommandEntry inputCommands[] = {
#if ENABLE_GT911_TOUCH
  { "touchread", "Read the single-touch snapshot without consuming events.", false, cmd_touchread },
#endif
  { "openinput",         "Start the configured input device.",              true, cmd_openinput },
  { "closeinput",        "Stop the input device.",                            true, cmd_closeinput },
  { "inputautostart",    "Enable/disable input device auto-start [on|off]",   true, cmd_inputautostart,    "Usage: inputautostart [on|off]" },
  { "inputdevicepollms", "Set input device poll interval ms [10-1000]",       true,  cmd_inputdevicepollms, "Usage: inputdevicepollms <10-1000>" },
#if ENABLE_GPIO_ENCODER
  { "gpioencoderread",   "Read GPIO wheel position, switch and pending input as JSON.", false, cmd_gpioencoderread },
#endif
};
const size_t inputCommandsCount = sizeof(inputCommands) / sizeof(inputCommands[0]);

// Settings module — JSON section `hardware.sensors.input` holds the
// device-agnostic settings. Driver-specific entries (the ANO's address,
// rotation and swaps) live in anoEncoderSettingsModule.
static const SettingEntry kInputSettingEntries[] = {
  { "inputAutoStart",    SETTING_BOOL, &gSettings.inputAutoStart,     0, 0, nullptr, 0,  1,    "Auto-start after boot", nullptr, false, nullptr, "inputautostart" },
#if !ENABLE_INPUT_DIRECT_START && !ENABLE_GT911_TOUCH
  { "inputDevicePollMs", SETTING_INT,  &gSettings.inputDevicePollMs, 90, 0, nullptr, 10, 1000, "Poll Interval (ms)",   nullptr, false, nullptr, "inputdevicepollms" },
#endif
};

static bool isInputConnected() { return gInputConnected; }

extern const SettingsModule inputSettingsModule = {
  "input",
  // Live under hardware.sensors.* so the web Settings page groups it with
  // the other sensors (the page splits modules by jsonSection prefix).
  "hardware.sensors.input",
  kInputSettingEntries,
  sizeof(kInputSettingEntries) / sizeof(kInputSettingEntries[0]),
  isInputConnected,
  "Local display input device (gamepad, encoder, touch or IMU)"
};

#endif // ENABLE_OLED_INPUT

bool inputTouchSnapshot(TouchSnapshot& out) {
  out = {};
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(10), "touch.snapshot");
  if (!guard.held) return false;
  out = sTouch.snapshot;
  return true;
#else
  return false;
#endif
}
bool inputTouchPending() {
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(10), "touch.pending");
  return guard.held && sTouch.pending();
#else
  return false;
#endif
}
bool inputTouchConsume(TouchEvent& out) {
  out = {};
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, pdMS_TO_TICKS(10), "touch.consume");
  return guard.held && sTouch.pop(out);
#else
  return false;
#endif
}
void inputTouchDiscard() {
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, portMAX_DELAY, "touch.discard");
  if (guard.held) sTouch.discard(millis());
#endif
}
void inputTouchCancel() {
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, portMAX_DELAY, "touch.cancel");
  if (guard.held) sTouch.cancel(millis());
#endif
}
void inputTouchPublish(uint8_t count, uint8_t id, uint16_t x, uint16_t y, uint32_t stamp) {
#if ENABLE_GT911_TOUCH
  SensorCacheGuard guard(sInputCacheMutex, portMAX_DELAY, "touch.publish");
  if (!guard.held) return;
  const uint32_t before = sTouch.snapshot.contact.sequence;
  sTouch.sample(count, id, x, y, stamp);
  if (before != sTouch.snapshot.contact.sequence) ++sInputCache.seq;
  sInputCache.lastUpdate = stamp;
#else
  (void)count; (void)id; (void)x; (void)y; (void)stamp;
#endif
}
