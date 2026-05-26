#ifndef I2CSENSOR_ANO_ENCODER_H
#define I2CSENSOR_ANO_ENCODER_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ANO Rotary Encoder Breakout (Adafruit, seesaw-based, default I2C 0x49).
// Drop-in replacement for the Mini Gamepad on the same STEMMA QT bus.
//
// Physical inputs: 5 D-pad-style buttons (IN/UP/DOWN/LEFT/RIGHT) + a rotary
// encoder with detents. The encoder produces signed deltas; the driver
// accumulates them and the OLED reader consumes one detent per frame so
// fast spins scroll smoothly across frames instead of being lost.
//
// Button → logical mapping (resolved by HAL_Input):
//   IN    → A (select/confirm)
//   LEFT  → B (back/cancel)
//   UP    → X (mode/options)
//   DOWN  → Y (delete/special)
//   RIGHT → axis-toggle modifier (driver-internal; not a logical button)
//   RIGHT + IN (chord) → START (menu)
//
// Rotary routes to either deltaY (vertical, default) or deltaX (horizontal)
// based on `currentAxis`. RIGHT-tap (press+release without any other button
// in between) flips `currentAxis`. State is per-mode — reset by the OLED
// mode-change path via anoEncoderResetAxisForMode().

// Bit layout for the `buttons` field and the buttonPressedAccum edge latch.
// These bits are arbitrary but must match the HAL_Input mapping in HAL_Input.cpp.
#define ANO_BTN_IN     (1u << 0)
#define ANO_BTN_UP     (1u << 1)
#define ANO_BTN_DOWN   (1u << 2)
#define ANO_BTN_LEFT   (1u << 3)
#define ANO_BTN_RIGHT  (1u << 4)
// Virtual bit synthesized by the driver when RIGHT+IN chord fires. Uses bit
// 16 to match the gamepad's GAMEPAD_BUTTON_START so any UI code that reads
// the bit directly (not via HAL) keeps working.
#define ANO_VIRT_START (1u << 16)

// Per-axis state values.
#define ANO_AXIS_VERTICAL   0
#define ANO_AXIS_HORIZONTAL 1

struct AnoEncoderCache {
  SemaphoreHandle_t mutex = nullptr;
  uint32_t buttons = 0;              // Current button state (active-high after invert)
  uint32_t buttonPressedAccum = 0;   // Latched press edges — OR'd in by task, read+cleared by UI
  int32_t  encoderPosition = 0;      // Monotonic absolute count
  int32_t  encoderDelta = 0;         // Unconsumed detents — UI consumes one per frame
  uint8_t  currentAxis = ANO_AXIS_VERTICAL;  // ANO_AXIS_* — flipped by RIGHT-tap, reset on mode change
  unsigned long lastUpdate = 0;
  bool     dataValid = false;
  uint32_t seq = 0;
};

#if ENABLE_ANO_ENCODER

class String;
class Adafruit_seesaw;

extern Adafruit_seesaw* gAnoSeesaw;
extern AnoEncoderCache  gAnoEncoderCache;

extern bool gAnoEncoderEnabled;
extern bool gAnoEncoderConnected;
extern TaskHandle_t gAnoEncoderTaskHandle;

// Lifecycle
bool anoEncoderInit();
bool anoEncoderInitConnection();
bool inputStartInternal();

// Reset currentAxis to the per-mode default. Called by the OLED mode-switch
// path so the axis state from a horizontal mode doesn't leak into a vertical
// one. `defaultAxis` is ANO_AXIS_VERTICAL or ANO_AXIS_HORIZONTAL.
void anoEncoderResetAxisForMode(uint8_t defaultAxis);

// One-step consumer used by the OLED input dispatch: returns the sign of one
// detent (+1 / -1 / 0) and decrements the cached delta toward zero. Lets
// fast spins scroll smoothly across multiple OLED frames.
int  anoEncoderConsumeOneDetent();

// JSON snapshot for ESP-NOW / web. Acquires the cache mutex.
int  anoEncoderBuildDataJSON(char* buf, size_t bufSize);

// Command handlers — only ANO-specific ones. Unified open/close/autostart/poll
// live in HAL_Input.cpp (inputCommands table).
const char* cmd_anoencoder(const String& argsInput);
const char* cmd_anoencoderi2caddr(const String& argsInput);
const char* cmd_anoencoderinvert(const String& argsInput);

struct CommandEntry;
extern const CommandEntry anoEncoderCommands[];
extern const size_t anoEncoderCommandsCount;

#endif // ENABLE_ANO_ENCODER
#endif // I2CSENSOR_ANO_ENCODER_H
