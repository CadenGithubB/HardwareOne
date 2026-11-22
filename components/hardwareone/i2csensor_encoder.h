#pragma once

// The Adafruit seesaw encoder boards as an ordinary optional I2C device named
// `encoder` (owner decision 2026-11-21): the 5752 I2C Quad Rotary Encoder
// (four NeoPixels) and the 4991 Rotary Encoder with NeoPixel (one), told apart
// by the product id the board reports. Only their pixels are used: the knobs,
// push switches and INT pin are not. openencoder / closeencoder through the
// sensor queue, encoderautostart opens it after boot, and the main loop
// advances its frames. The LED commands (System_NeoPixel) reach its pixels
// through the `encoder[<n>]` and `encoder[<n>]:<p>` targets and the functions below.
//
// Up to kEncoderMaxInstances boards at once (multi-instance design, phase
// 1c): instance n (1-based) has its own saved settings, connection, colours
// and reconnect. Instance 1 is `encoder` everywhere, as before; instance n >= 2
// is named by a trailing number (`openencoder 2`, `encoderaddress 2 0x37`).
// Every function below takes the instance last, 1 when it is left out.

#include "System_BuildConfig.h"
#include <stdint.h>

#include "System_LedEffectCore.h"  // RGB
#include "i2csensor_encoder_light.h"  // EncoderInstanceSettings, kEncoderMaxInstances

// What `ledstatus` shows of the encoder light.
struct EncoderLightPixelStatus {
  RGB rgb{0, 0, 0};       // the steady colour (before brightness)
  char colour[24] = "";   // its name as the command gave it
  char effect[12] = "";   // the running effect, or "off"
};
struct EncoderLightStatus {
  bool connected = false;
  uint16_t product = 0;             // the board last detected; 0 before the first open
  const char* boardName = nullptr;  // its product name; nullptr before the first open
  uint8_t pixelCount = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  uint8_t brightness = 0;           // percent, saved
  EncoderLightPixelStatus pixels[4];
};

#if ENABLE_ENCODER

// The saved settings of every instance (index n - 1), in PSRAM; the settings
// rows point into it.
extern EncoderInstanceSettings gEncoderInst[kEncoderMaxInstances];

// Instance n is open and answered (set by the sensor queue worker).
bool encoderRunning(uint8_t n = 1);
// Any instance is open (the RAM flush keeps one switch for the device).
bool encoderAnyRunning();
// Instance n's start-at-boot setting; false for an n the device does not have.
bool encoderAutoStartOf(uint8_t n);
// The field behind it (gEncoderInst), for the switch tables of System_I2C.cpp
// (a constant address, so those tables stay in flash) and `sensorautostart
// encoder <n>`; nullptr for an n the device does not have.
constexpr bool* encoderAutoStartSetting(uint8_t n) {
  return (n >= 1 && n <= kEncoderMaxInstances) ? &gEncoderInst[n - 1].autoStart : nullptr;
}
// Instance n's address for the next open (0 when instance n >= 2 has none
// set) and bus (the bus its pixels were bound to by its first open, else the
// setting), for the device table's scan, identify and pre-registration and
// the sensor queue's presence probe (System_I2C.cpp).
uint8_t encoderInstanceAddress(uint8_t n);
uint8_t encoderInstanceBus(uint8_t n);

// Sensor queue worker only.
bool encoderStartInternal(uint8_t n = 1);
void encoderStop(uint8_t n = 1);

// Main loop: advance each open instance's colours/effects one frame when one
// is due. Writes only a changed frame, at most about 30 times a second per
// board, and skips a frame when the bus is busy.
void encoderTick();

// The LED commands' `encoder` target. `pixel` is 0 for every pixel, else
// 1-based. Each returns nullptr when done, else the reply to give: the board
// is not connected (a board meant to be open starts a reconnect, at most every
// 2 s), the pixel is past the board's count (the reply names the count), or
// the light's state lock was busy.
const char* encoderLightSetColour(uint8_t pixel, RGB colour, const char* name, uint8_t n = 1);
const char* encoderLightStartEffect(uint8_t pixel, int effect, RGB colour1, RGB colour2,
                                    uint32_t durationMs, const char* name, uint8_t n = 1);
// Saves the brightness (0..100); the next frame applies it, connected or not.
void encoderLightSetBrightness(int percent, uint8_t n = 1);
// False when the state lock was busy.
bool encoderLightReadStatus(EncoderLightStatus& out, uint8_t n = 1);

#else
// The encoder board is not in this build: it never runs, a start fails, and
// the light refuses every request.
constexpr const char* kEncoderReplyNotBuilt =
    "Error: The encoder board is not in this build. Build it in with CUSTOM_ENABLE_ENCODER.";
inline bool encoderRunning(uint8_t = 1) { return false; }
inline bool encoderAnyRunning() { return false; }
inline bool encoderAutoStartOf(uint8_t) { return false; }
// One switch that stays off: nothing in this build reads it as on.
inline bool gEncoderNotBuiltSwitch = false;
constexpr bool* encoderAutoStartSetting(uint8_t) { return &gEncoderNotBuiltSwitch; }
inline uint8_t encoderInstanceAddress(uint8_t) { return 0; }
inline uint8_t encoderInstanceBus(uint8_t) { return 0; }
inline bool encoderStartInternal(uint8_t = 1) { return false; }
inline void encoderStop(uint8_t = 1) {}
inline void encoderTick() {}
inline const char* encoderLightSetColour(uint8_t, RGB, const char*, uint8_t = 1) { return kEncoderReplyNotBuilt; }
inline const char* encoderLightStartEffect(uint8_t, int, RGB, RGB, uint32_t, const char*, uint8_t = 1) {
  return kEncoderReplyNotBuilt;
}
inline void encoderLightSetBrightness(int, uint8_t = 1) {}
#endif  // ENABLE_ENCODER
