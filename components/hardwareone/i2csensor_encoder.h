#pragma once

// The Adafruit seesaw encoder boards as an ordinary optional I2C device named
// `encoder` (owner decision 2026-11-21): the 5752 I2C Quad Rotary Encoder
// (four NeoPixels) and the 4991 Rotary Encoder with NeoPixel (one), told apart
// by the product id the board reports. Only their pixels are used: the knobs,
// push switches and INT pin are not. openencoder / closeencoder through the
// sensor queue, encoderautostart opens it after boot, and the main loop
// advances its frames. The LED commands (System_NeoPixel) reach its pixels
// through the `encoder` and `encoder:<n>` targets and the functions below.

#include "System_BuildConfig.h"
#include <stdint.h>

#include "System_LedEffectCore.h"  // RGB

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

// The board is open and answered (set by the sensor queue worker).
bool encoderRunning();
// Address and bus the next open uses (the bus is latched by the first open).
uint8_t encoderDeviceAddress();
uint8_t encoderDeviceBus();

// Sensor queue worker only.
bool encoderStartInternal();
void encoderStop();

// Main loop: advance the colours/effects one frame when one is due. Writes
// only a changed frame, at most about 30 times a second, and skips a frame
// when the bus is busy.
void encoderTick();

// The LED commands' `encoder` target. `pixel` is 0 for every pixel, else
// 1-based. Each returns nullptr when done, else the reply to give: the board
// is not connected (a board meant to be open starts a reconnect, at most every
// 2 s), the pixel is past the board's count (the reply names the count), or
// the light's state lock was busy.
const char* encoderLightSetColour(uint8_t pixel, RGB colour, const char* name);
const char* encoderLightStartEffect(uint8_t pixel, int effect, RGB colour1, RGB colour2,
                                    uint32_t durationMs, const char* name);
// Saves the brightness (0..100); the next frame applies it, connected or not.
void encoderLightSetBrightness(int percent);
// False when the state lock was busy.
bool encoderLightReadStatus(EncoderLightStatus& out);

#else
// The encoder board is not in this build: it never runs, a start fails, and
// the light refuses every request.
constexpr const char* kEncoderReplyNotBuilt =
    "Error: The encoder board is not in this build. Build it in with CUSTOM_ENABLE_ENCODER.";
inline bool encoderRunning() { return false; }
inline uint8_t encoderDeviceAddress() { return 0x49; }
inline uint8_t encoderDeviceBus() { return 0; }
inline bool encoderStartInternal() { return false; }
inline void encoderStop() {}
inline void encoderTick() {}
inline const char* encoderLightSetColour(uint8_t, RGB, const char*) { return kEncoderReplyNotBuilt; }
inline const char* encoderLightStartEffect(uint8_t, int, RGB, RGB, uint32_t, const char*) {
  return kEncoderReplyNotBuilt;
}
inline void encoderLightSetBrightness(int) {}
#endif  // ENABLE_ENCODER
