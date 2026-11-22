/**
 * NeoPixel LED control for boards with an addressable RGB LED, and the LED
 * commands every light shares (the onboard pixel, the encoder board's pixels:
 * System_LedTarget.h).
 */

#ifndef SYSTEM_NEOPIXEL_H
#define SYSTEM_NEOPIXEL_H

// BuildConfig FIRST: this header branches on ENABLE_NEOPIXEL below, and the
// .cpp includes this header before anything else — without this include the
// stub branch would be chosen there and collide with the real definitions.
#include "System_BuildConfig.h"
#include <Arduino.h>

// RGB, EffectType, rainbowColor and the effect frame math are shared with the
// encoder board's pixels; they live in one header.
#include "System_LedEffectCore.h"

// Color entry for lookup table
struct ColorEntry {
  const char* name;
  RGB rgb;
};

// 75-color palette (PROGMEM; enumerate via memcpy_P per entry — the name
// pointers reference flash string literals and are program-lifetime).
extern const ColorEntry colorTable[];
extern const int numColors;

// Effect-name table — the single source of truth shared by the cmd_ledeffect
// parser and every UI that offers an effect picker (OLED LED screen, G2 LED
// page). "off" is NOT in the table (it's a cancel, not an effect) — pickers
// append their own Off row.
extern const char* const ledEffectNames[];
extern const int ledEffectNameCount;
// Name -> EFFECT_* code (case-insensitive); EFFECT_NONE (0) if unknown.
int ledEffectCodeForName(const String& name);

// The ledeffect grammar after its optional target (System_LedTarget.h), the
// same for every light:
// <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000].
// Defaults: red, blue, 3000 ms.
enum class LedEffectArgsError : uint8_t {
  None, Missing, TooMany, UnknownColor, NotANumber, DurationRange, UnknownEffect
};
struct LedEffectArgs {
  bool off = false;                 // "off" or "none": cancel
  String effectName;                // lower case, as typed
  int effect = EFFECT_NONE;         // EFFECT_* code
  RGB color1 = {255, 0, 0};         // Red
  RGB color2 = {0, 0, 255};         // Blue
  unsigned long durationMs = 3000;  // 3 seconds
  String badToken;                  // the unknown color or effect name
};
LedEffectArgsError parseLedEffectArgs(const String& argsInput, LedEffectArgs& out);

// Brightness preset ladder shared by the OLED/G2 "tap to cycle" rows:
// 10 -> 25 -> 50 -> 75 -> 100 -> 10; an off-ladder value lands on the next
// step up.
int ledBrightnessNextPreset(int cur);

// Board power rail: some boards route peripheral power through the NeoPixel
// power pin (Feather V2 GPIO2 also powers the STEMMA QT connector), so the
// rail is asserted INDEPENDENTLY of ENABLE_NEOPIXEL — a slim build with the
// LED compiled out must not lose I2C. No-op on boards without a power pin.
void boardPowerRailInit();

#if ENABLE_NEOPIXEL

// LED control functions
void initNeoPixelLed();
void setLedColor(RGB color);
// Blocking effect runner — boot-time startup effect only (spins the async
// engine below until done). Runtime paths use the non-blocking API.
void runLedEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration);
// Non-blocking effect engine: start returns immediately; frames are advanced
// by ledEffectTick() from the main loop. Starting a new effect replaces the
// running one; ledcolor/ledclear/`ledeffect off` cancel via ledEffectStop.
void ledEffectStart(int effectType, RGB startColor, RGB endColor, unsigned long duration);
void ledEffectStop(bool clearLed);
bool ledEffectActive();
void ledEffectTick();

#else  // !ENABLE_NEOPIXEL — inline no-op stubs so shared callers need no guards

inline void initNeoPixelLed() {}
inline void setLedColor(RGB) {}
inline void runLedEffect(int, RGB, RGB, unsigned long) {}
inline void ledEffectStart(int, RGB, RGB, unsigned long) {}
inline void ledEffectStop(bool) {}
inline bool ledEffectActive() { return false; }
inline void ledEffectTick() {}

#endif  // ENABLE_NEOPIXEL

bool getRgbFromName(const String& colorName, RGB& color);

// Color utility functions
RGB blendColors(RGB a, RGB b, float ratio);
RGB adjustBrightness(RGB color, float brightness);
String getClosestColorName(uint16_t r, uint16_t g, uint16_t b, RGB& closestRgb);

#endif // SYSTEM_NEOPIXEL_H
