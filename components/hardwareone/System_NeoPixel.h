/**
 * NeoPixel LED Module - QT Py ESP32 built-in RGB LED control
 * 
 * Controls the single built-in NeoPixel LED on the QT Py board
 */

#ifndef SYSTEM_NEOPIXEL_H
#define SYSTEM_NEOPIXEL_H

// BuildConfig FIRST: this header branches on ENABLE_NEOPIXEL below, and the
// .cpp includes this header before anything else — without this include the
// stub branch would be chosen there and collide with the real definitions.
#include "System_BuildConfig.h"
#include <Arduino.h>
#if ENABLE_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#endif

// RGB color structure
struct RGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// Color entry for lookup table
struct ColorEntry {
  const char* name;
  RGB rgb;
};

// LED Effect types
enum EffectType {
  EFFECT_NONE = 0,
  EFFECT_FADE = 1,
  EFFECT_PULSE = 2,
  EFFECT_RAINBOW = 3,
  EFFECT_BLINK = 4,
  EFFECT_STROBE = 5
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

// Global NeoPixel instance
extern Adafruit_NeoPixel pixels;

// LED control functions
void initNeoPixelLED();
void setLEDColor(RGB color);
// Blocking effect runner — boot-time startup effect only (spins the async
// engine below until done). Runtime paths use the non-blocking API.
void runLEDEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration);
// Non-blocking effect engine: start returns immediately; frames are advanced
// by ledEffectTick() from the main loop. Starting a new effect replaces the
// running one; ledcolor/ledclear/`ledeffect off` cancel via ledEffectStop.
void ledEffectStart(int effectType, RGB startColor, RGB endColor, unsigned long duration);
void ledEffectStop(bool clearLed);
bool ledEffectActive();
void ledEffectTick();

#else  // !ENABLE_NEOPIXEL — inline no-op stubs so shared callers need no guards

inline void initNeoPixelLED() {}
inline void setLEDColor(RGB) {}
inline void runLEDEffect(int, RGB, RGB, unsigned long) {}
inline void ledEffectStart(int, RGB, RGB, unsigned long) {}
inline void ledEffectStop(bool) {}
inline bool ledEffectActive() { return false; }
inline void ledEffectTick() {}

#endif  // ENABLE_NEOPIXEL

bool getRGBFromName(const String& colorName, RGB& color);

// Color utility functions
RGB blendColors(RGB a, RGB b, float ratio);
RGB adjustBrightness(RGB color, float brightness);
RGB rainbowColor(int step, int maxSteps);
String getClosestColorName(uint16_t r, uint16_t g, uint16_t b, RGB& closestRGB);

#if ENABLE_NEOPIXEL
// NeoPixel command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry neopixelCommands[];
extern const size_t neopixelCommandsCount;
#endif

#endif // SYSTEM_NEOPIXEL_H
