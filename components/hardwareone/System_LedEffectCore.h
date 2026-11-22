#pragma once

// Colour and effect-frame math shared by every RGB light the firmware drives:
// the board's NeoPixel (System_NeoPixel) and the pixels of the encoder board
// (i2csensor_encoder). Header-only and free of Arduino types so the
// host tests compile the same code the firmware runs.

#include <math.h>
#include <stdint.h>

// RGB color structure
struct RGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

inline bool operator==(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(RGB a, RGB b) { return !(a == b); }

// LED Effect types
enum EffectType {
  EFFECT_NONE = 0,
  EFFECT_FADE = 1,
  EFFECT_PULSE = 2,
  EFFECT_RAINBOW = 3,
  EFFECT_BLINK = 4,
  EFFECT_STROBE = 5
};

inline RGB rainbowColor(int step, int maxSteps) {
  // Generate rainbow colors using HSV-like approach
  float hue = (float)step / maxSteps * 360.0f;
  float s = 1.0f, v = 1.0f;

  int hi = (int)(hue / 60.0f) % 6;
  float f = hue / 60.0f - hi;
  float p = v * (1 - s);
  float q = v * (1 - f * s);
  float t = v * (1 - (1 - f) * s);

  RGB result;
  switch (hi) {
    case 0: result = {(uint8_t)(v*255), (uint8_t)(t*255), (uint8_t)(p*255)}; break;
    case 1: result = {(uint8_t)(q*255), (uint8_t)(v*255), (uint8_t)(p*255)}; break;
    case 2: result = {(uint8_t)(p*255), (uint8_t)(v*255), (uint8_t)(t*255)}; break;
    case 3: result = {(uint8_t)(p*255), (uint8_t)(q*255), (uint8_t)(v*255)}; break;
    case 4: result = {(uint8_t)(t*255), (uint8_t)(p*255), (uint8_t)(v*255)}; break;
    default: result = {(uint8_t)(v*255), (uint8_t)(p*255), (uint8_t)(q*255)}; break;
  }
  return result;
}

// Frame color at `elapsed` ms of an effect lasting `duration` ms. Every effect
// is a pure function of elapsed time, so a caller advances it from a tick.
inline RGB ledEffectFrame(int effectType, RGB c1, RGB c2,
                          unsigned long elapsed, unsigned long duration) {
  switch (effectType) {
    case EFFECT_FADE: {
      float progress = (duration > 0) ? (float)elapsed / duration : 1.0f;
      return { (uint8_t)(c1.r + (c2.r - c1.r) * progress),
               (uint8_t)(c1.g + (c2.g - c1.g) * progress),
               (uint8_t)(c1.b + (c2.b - c1.b) * progress) };
    }
    case EFFECT_PULSE: {
      float progress = (float)elapsed / 1000.0f;
      int brightness = (int)(127.5f + 127.5f * sinf(progress * 3.14159f * 2));
      return { (uint8_t)(c1.r * brightness / 255),
               (uint8_t)(c1.g * brightness / 255),
               (uint8_t)(c1.b * brightness / 255) };
    }
    case EFFECT_RAINBOW:
      return rainbowColor((int)((elapsed / 20) % 256), 256);
    case EFFECT_BLINK:
      return ((elapsed / 250) % 2 == 0) ? c1 : RGB{0, 0, 0};
    case EFFECT_STROBE:
      return ((elapsed / 50) % 2 == 0) ? c1 : RGB{0, 0, 0};
    default:
      return {0, 0, 0};
  }
}
