/**
 * NeoPixel LED Module - QT Py ESP32 built-in RGB LED control
 * 
 * Controls the single built-in NeoPixel LED on the QT Py board
 */

#ifndef NEOPIXEL_LED_H
#define NEOPIXEL_LED_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

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
  EFFECT_BREATHE = 4
};

// 64-color palette (defined in neopixel_led.cpp)
extern const ColorEntry colorTable[];
extern const int numColors;

// Global NeoPixel instance
extern Adafruit_NeoPixel pixels;

// LED control functions
void initNeoPixelLED();
void setLEDColor(RGB color);
void runLEDEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration);
bool getRGBFromName(const String& colorName, RGB& color);

// Color utility functions
RGB blendColors(RGB a, RGB b, float ratio);
RGB adjustBrightness(RGB color, float brightness);
RGB rainbowColor(int step, int maxSteps);
String getClosestColorName(uint16_t r, uint16_t g, uint16_t b, RGB& closestRGB);

// NeoPixel command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry neopixelCommands[];
extern const size_t neopixelCommandsCount;

#endif // NEOPIXEL_LED_H
