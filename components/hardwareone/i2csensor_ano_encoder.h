#ifndef I2CSENSOR_ANO_ENCODER_H
#define I2CSENSOR_ANO_ENCODER_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The button bits, pins, address rule and poll state machine
// (i2csensor_ano_encoder_core.h, host-tested).
#include "i2csensor_ano_encoder_core.h"

// ANO Rotary Encoder Breakout (Adafruit 5740, seesaw-based, I2C 0x49..0x50).
// The I2C input device of INPUT_DEVICE_TYPE 2, in place of the Mini Gamepad.
//
// Physical inputs: 5 buttons (IN/UP/DOWN/LEFT/RIGHT) and a wheel with
// detents. The task polls every inputdevicepollms (90 ms by default); short
// taps between two polls are caught from the seesaw's interrupt flags. The
// wheel's detents accumulate and the OLED drains them each frame, so fast
// spins are not lost.
//
// Button roles (logical buttons in HAL_Input.cpp, made by anoPoll()):
//   IN    -> A (select/confirm)
//   LEFT  -> B (back/cancel)
//   DOWN  -> X (submit/complete)
//   UP    -> Y (backspace)
//   RIGHT -> SELECT, sent when RIGHT is released (a tap)
//   RIGHT held + IN -> START only (no SELECT, no A)
// The wheel is always the vertical (up/down) navigation axis.

struct AnoEncoderCache {
  uint32_t buttons = 0;              // Buttons held now (ANO_BTN_*, active high)
  int32_t  encoderPosition = 0;      // Wheel count since the device was opened (the chip's direction)
  int32_t  encoderDelta = 0;         // Unconsumed detents — the OLED drains them each frame
  unsigned long lastUpdate = 0;
  bool     dataValid = false;
  uint32_t seq = 0;
};

#if ENABLE_ANO_ENCODER

class String;

bool anoEncoderCacheSnapshot(AnoEncoderCache& out, uint32_t timeoutMs = 10);

extern bool gAnoEncoderEnabled;
extern bool gAnoEncoderConnected;
extern TaskHandle_t gAnoEncoderTaskHandle;

// The address the driver opens: the anoEncoderI2cAddr setting (0x49..0x50).
uint8_t anoEncoderAddress();

// Lifecycle (the sensor queue's worker calls inputStartInternal).
bool anoEncoderInit();
bool inputStartInternal();

// One-step consumer used by the OLED input dispatch: returns the sign of one
// detent (+1 / -1 / 0) and moves the cached delta one step toward zero.
int  anoEncoderConsumeOneDetent();

// JSON snapshot for ESP-NOW / web. Acquires the cache mutex.
int  anoEncoderBuildDataJson(char* buf, size_t bufSize);

// Command handlers — only ANO-specific ones. Unified open/close/autostart/poll
// live in HAL_Input.cpp (inputCommands table).
const char* cmd_anoencoder(const String& argsInput);
const char* cmd_anoencoderstatus(const String& argsInput);
const char* cmd_anoencoderi2caddr(const String& argsInput);
const char* cmd_anoencoderinvert(const String& argsInput);

#else
inline bool anoEncoderCacheSnapshot(AnoEncoderCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
#endif // ENABLE_ANO_ENCODER
#endif // I2CSENSOR_ANO_ENCODER_H
