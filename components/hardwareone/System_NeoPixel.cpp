/**
 * NeoPixel LED Module - QT Py ESP32 built-in RGB LED control
 */

#include "System_NeoPixel.h"
#include "System_Utils.h"
#include "System_Command.h"
#include "System_Settings.h"        // For SettingsModule (merged from System_LED.cpp)
#include "System_Debug.h"    // For debug macros (merged from System_LED.cpp)
#include "System_BuildConfig.h"  // For NEOPIXEL_PIN_DEFAULT

// Use NEOPIXEL_PIN_DEFAULT from System_BuildConfig.h (board-specific)
// Boards without NeoPixel set this to -1
#if !defined(NEOPIXEL_PIN_DEFAULT)
  #define NEOPIXEL_PIN_DEFAULT -1
#endif

// Determine if NeoPixel hardware is available at compile time
#define NEOPIXEL_AVAILABLE (NEOPIXEL_PIN_DEFAULT >= 0)

// Global NeoPixel instance - only instantiate if hardware is available
#define NUMPIXELS 1
#if NEOPIXEL_AVAILABLE
  Adafruit_NeoPixel pixels(NUMPIXELS, NEOPIXEL_PIN_DEFAULT, NEO_GRB + NEO_KHZ800);
#else
  // Dummy instance that won't touch any GPIO pins
  Adafruit_NeoPixel pixels(0, -1, NEO_GRB + NEO_KHZ800);
#endif

// ============================================================================
// 64-Color Palette (stored in PROGMEM to save RAM)
// ============================================================================
const ColorEntry colorTable[] PROGMEM = {
  // Primary colors
  { "red", { 255, 0, 0 } },
  { "green", { 0, 255, 0 } },
  { "blue", { 0, 0, 255 } },
  { "yellow", { 255, 255, 0 } },
  { "cyan", { 0, 255, 255 } },
  { "magenta", { 255, 0, 255 } },
  { "white", { 255, 255, 255 } },
  { "black", { 0, 0, 0 } },

  // Orange family
  { "orange", { 255, 165, 0 } },
  { "darkorange", { 255, 140, 0 } },
  { "orangered", { 255, 69, 0 } },
  { "coral", { 255, 127, 80 } },
  { "tomato", { 255, 99, 71 } },
  { "peach", { 255, 218, 185 } },

  // Red family
  { "darkred", { 139, 0, 0 } },
  { "crimson", { 220, 20, 60 } },
  { "firebrick", { 178, 34, 34 } },
  { "indianred", { 205, 92, 92 } },
  { "lightcoral", { 240, 128, 128 } },
  { "salmon", { 250, 128, 114 } },

  // Pink family
  { "pink", { 255, 192, 203 } },
  { "lightpink", { 255, 182, 193 } },
  { "hotpink", { 255, 105, 180 } },
  { "deeppink", { 255, 20, 147 } },
  { "palevioletred", { 219, 112, 147 } },
  { "mediumvioletred", { 199, 21, 133 } },

  // Purple family
  { "purple", { 128, 0, 128 } },
  { "darkviolet", { 148, 0, 211 } },
  { "blueviolet", { 138, 43, 226 } },
  { "mediumpurple", { 147, 112, 219 } },
  { "plum", { 221, 160, 221 } },
  { "orchid", { 218, 112, 214 } },

  // Blue family
  { "darkblue", { 0, 0, 139 } },
  { "navy", { 0, 0, 128 } },
  { "mediumblue", { 0, 0, 205 } },
  { "royalblue", { 65, 105, 225 } },
  { "steelblue", { 70, 130, 180 } },
  { "lightblue", { 173, 216, 230 } },
  { "skyblue", { 135, 206, 235 } },
  { "lightskyblue", { 135, 206, 250 } },
  { "deepskyblue", { 0, 191, 255 } },
  { "dodgerblue", { 30, 144, 255 } },
  { "cornflowerblue", { 100, 149, 237 } },
  { "cadetblue", { 95, 158, 160 } },

  // Green family
  { "darkgreen", { 0, 100, 0 } },
  { "forestgreen", { 34, 139, 34 } },
  { "seagreen", { 46, 139, 87 } },
  { "mediumseagreen", { 60, 179, 113 } },
  { "springgreen", { 0, 255, 127 } },
  { "limegreen", { 50, 205, 50 } },
  { "lime", { 0, 255, 0 } },
  { "lightgreen", { 144, 238, 144 } },
  { "palegreen", { 152, 251, 152 } },
  { "aquamarine", { 127, 255, 212 } },
  { "mediumaquamarine", { 102, 205, 170 } },

  // Yellow/Gold family
  { "gold", { 255, 215, 0 } },
  { "lightyellow", { 255, 255, 224 } },
  { "lemonchiffon", { 255, 250, 205 } },
  { "lightgoldenrodyellow", { 250, 250, 210 } },
  { "khaki", { 240, 230, 140 } },
  { "darkkhaki", { 189, 183, 107 } },

  // Brown family
  { "brown", { 165, 42, 42 } },
  { "saddlebrown", { 139, 69, 19 } },
  { "sienna", { 160, 82, 45 } },
  { "chocolate", { 210, 105, 30 } },
  { "peru", { 205, 133, 63 } },
  { "tan", { 210, 180, 140 } },
  { "burlywood", { 222, 184, 135 } },
  { "wheat", { 245, 222, 179 } },

  // Gray family
  { "gray", { 128, 128, 128 } },
  { "darkgray", { 169, 169, 169 } },
  { "lightgray", { 211, 211, 211 } },
  { "silver", { 192, 192, 192 } },
  { "dimgray", { 105, 105, 105 } },
  { "gainsboro", { 220, 220, 220 } }
};
const int numColors = sizeof(colorTable) / sizeof(colorTable[0]);

// ============================================================================
// LED Control Functions
// ============================================================================

void initNeoPixelLED() {
#if NEOPIXEL_AVAILABLE
  // Enable NeoPixel power pin (required on QT Py ESP32 GPIO 8, Feather V2 GPIO 2)
  // Guard against boards that define NEOPIXEL_POWER_PIN as -1 (no power pin)
  #if defined(NEOPIXEL_POWER_PIN) && NEOPIXEL_POWER_PIN >= 0
    pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
    digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
    delay(10);  // Allow power to stabilize
  #endif
  
  // Legacy: some boards use NEOPIXEL_I2C_POWER instead
  #if defined(NEOPIXEL_I2C_POWER) && !defined(NEOPIXEL_POWER_PIN)
    pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
    delay(10);
  #endif
  
  pixels.begin();
  // Apply saved brightness (0-100% -> 0-255); fall back to 50% if settings not yet loaded
  int bPct = (gSettings.ledBrightness > 0) ? gSettings.ledBrightness : 50;
  if (bPct > 100) bPct = 100;
  pixels.setBrightness((uint8_t)(bPct * 255 / 100));
  pixels.show();  // Initialize all pixels to 'off'
#endif
  // No-op on boards without NeoPixel hardware
}

void setLEDColor(RGB color) {
#if NEOPIXEL_AVAILABLE
  pixels.setPixelColor(0, pixels.Color(color.r, color.g, color.b));
  pixels.show();
#endif
  // No-op on boards without NeoPixel hardware
}

bool getRGBFromName(const String& colorName, RGB& color) {
  String name = colorName;
  name.toLowerCase();
  
  // Handle special "off" alias
  if (name == "off") { color = {0, 0, 0}; return true; }
  
  // Search color table (PROGMEM)
  for (int i = 0; i < numColors; i++) {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[i], sizeof(ColorEntry));
    if (name.equalsIgnoreCase(entry.name)) {
      color = entry.rgb;
      return true;
    }
  }
  
  return false;  // Unknown color
}

RGB blendColors(RGB a, RGB b, float ratio) {
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  return {
    (uint8_t)(a.r + (b.r - a.r) * ratio),
    (uint8_t)(a.g + (b.g - a.g) * ratio),
    (uint8_t)(a.b + (b.b - a.b) * ratio)
  };
}

RGB adjustBrightness(RGB color, float brightness) {
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  return {
    (uint8_t)(color.r * brightness),
    (uint8_t)(color.g * brightness),
    (uint8_t)(color.b * brightness)
  };
}

RGB rainbowColor(int step, int maxSteps) {
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

String getClosestColorName(uint16_t r, uint16_t g, uint16_t b, RGB& closestRGB) {
  // Find closest color in table using Euclidean distance
  int minDist = INT_MAX;
  String closestName = "unknown";
  
  for (int i = 0; i < numColors; i++) {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[i], sizeof(ColorEntry));
    
    int dr = (int)r - entry.rgb.r;
    int dg = (int)g - entry.rgb.g;
    int db = (int)b - entry.rgb.b;
    int dist = dr*dr + dg*dg + db*db;
    
    if (dist < minDist) {
      minDist = dist;
      closestName = entry.name;
      closestRGB = entry.rgb;
    }
  }
  
  return closestName;
}

// ============================================================================
// Shared effect-name table + brightness ladder (see System_NeoPixel.h)
// ============================================================================
// Order matches the EFFECT_* enum starting at EFFECT_FADE=1, so
// code = table index + 1. Keep in sync with the EffectType enum.
const char* const ledEffectNames[] = { "fade", "pulse", "rainbow", "blink", "strobe" };
const int ledEffectNameCount = (int)(sizeof(ledEffectNames) / sizeof(ledEffectNames[0]));

int ledEffectCodeForName(const String& name) {
  for (int i = 0; i < ledEffectNameCount; i++) {
    if (name.equalsIgnoreCase(ledEffectNames[i])) return i + 1;  // EFFECT_FADE=1
  }
  return EFFECT_NONE;
}

int ledBrightnessNextPreset(int cur) {
  static const int presets[] = { 10, 25, 50, 75, 100 };
  const int n = (int)(sizeof(presets) / sizeof(presets[0]));
  for (int i = 0; i < n; i++) {
    if (cur < presets[i]) return presets[i];
  }
  return presets[0];  // at/above top -> wrap
}

// ============================================================================
// Non-blocking effect engine
// ============================================================================
// Every effect is a pure function of elapsed time, so instead of the old
// delay()-loop (which parked cmd_exec — and therefore the OLED UI, which waits
// on it synchronously — for the whole duration), the engine keeps a tiny state
// struct and advances one frame per ledEffectTick() call from the main loop.
// cmd_ledeffect now starts an effect and returns immediately; `ledeffect off`,
// ledcolor, and ledclear genuinely cancel a running effect (previously
// impossible — cmd_exec was busy running it).

static struct {
  bool          active;
  int           type;
  RGB           c1, c2;
  unsigned long startMs;
  unsigned long durationMs;
  unsigned long lastFrameMs;
} gLedFx = {};

// Frame color at `elapsed` ms — the old loop bodies, minus the loops.
static RGB ledEffectFrame(int effectType, RGB c1, RGB c2,
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

void ledEffectStart(int effectType, RGB startColor, RGB endColor, unsigned long duration) {
  gLedFx.type        = effectType;
  gLedFx.c1          = startColor;
  gLedFx.c2          = endColor;
  gLedFx.durationMs  = duration;
  gLedFx.startMs     = millis();
  gLedFx.lastFrameMs = 0;
  gLedFx.active      = true;
}

void ledEffectStop(bool clearLed) {
  if (!gLedFx.active) return;
  gLedFx.active = false;
  if (clearLed) setLEDColor({0, 0, 0});  // matches the old end-of-effect clear
}

bool ledEffectActive() { return gLedFx.active; }

// Main-loop tick. Cheap no-op while idle; ~20 ms frame pacing while active
// (the fastest cadence the old loops used — strobe/rainbow).
void ledEffectTick() {
  if (!gLedFx.active) return;
  const unsigned long now = millis();
  const unsigned long elapsed = now - gLedFx.startMs;
  if (elapsed >= gLedFx.durationMs) {
    ledEffectStop(true);
    return;
  }
  if (now - gLedFx.lastFrameMs < 20) return;
  gLedFx.lastFrameMs = now;
  setLEDColor(ledEffectFrame(gLedFx.type, gLedFx.c1, gLedFx.c2, elapsed, gLedFx.durationMs));
}

// Blocking wrapper over the same engine — kept for the boot-time startup
// effect (HardwareOne.cpp), which deliberately runs before the main loop is
// ticking and where blocking a few seconds of setup() is the intended UX.
// Runtime paths (cmd_ledeffect) use ledEffectStart instead.
void runLEDEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration) {
  ledEffectStart(effectType, startColor, endColor, duration);
  while (gLedFx.active) {
    ledEffectTick();
    delay(10);
  }
}

// ============================================================================
// NeoPixel Command Handlers
// ============================================================================

const char* cmd_ledcolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String colorName = argsInput;
  colorName.trim();
  colorName.toLowerCase();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (colorName.length() == 0) {
    return "Error: invalid arguments — Usage: ledcolor <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>";
  }
  
  RGB color;
  if (!getRGBFromName(colorName, color)) {
    snprintf(getDebugBuffer(), 1024, "Error: Unknown color: %s", colorName.c_str());
    return getDebugBuffer();
  }

  // Cancel any running effect first, or the next tick would repaint over the
  // user's chosen color within 20 ms.
  ledEffectStop(false);
  setLEDColor(color);
  snprintf(getDebugBuffer(), 1024, "LED set to %s", colorName.c_str());
  return getDebugBuffer();
}

const char* cmd_ledclear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  ledEffectStop(false);  // cancel a running effect too, not just the color
  setLEDColor({0, 0, 0});
  return "LED cleared (turned off)";
}

const char* cmd_ledeffect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);

  if (a.count() == 0 || a.arg(0) == "off" || a.arg(0) == "none") {
    // Genuinely cancels now — with the old blocking engine this branch could
    // never run mid-effect because cmd_exec was busy inside the delay loop.
    ledEffectStop(false);
    setLEDColor({0, 0, 0});
    return "LED effect: off";
  }

  // Parse effect type
  String effectType = a.arg(0);
  effectType.toLowerCase();

  String remaining = a.remaining(0);

  // Default values
  RGB color1 = {255, 0, 0};     // Red
  RGB color2 = {0, 0, 255};     // Blue
  unsigned long duration = 3000;  // 3 seconds

  // Parse optional color and duration arguments
  if (remaining.length() > 0) {
    int secondSpace = remaining.indexOf(' ');
    String firstArg = (secondSpace >= 0) ? remaining.substring(0, secondSpace) : remaining;
    firstArg.trim();

    if (firstArg.length() > 0 && isdigit(firstArg.charAt(0))) {
      duration = firstArg.toInt();
      if (duration < 100) duration = 100;
      if (duration > 60000) duration = 60000;
    } else {
      if (!getRGBFromName(firstArg, color1)) {
        if (!ensureDebugBuffer()) return "Error: Unknown color";
        snprintf(getDebugBuffer(), 1024, "Error: Unknown color '%s'", firstArg.c_str());
        return getDebugBuffer();
      }

      // Parse second color or duration
      if (secondSpace >= 0) {
        String rest = remaining.substring(secondSpace + 1);
        rest.trim();
        int thirdSpace = rest.indexOf(' ');
        String secondArg = (thirdSpace >= 0) ? rest.substring(0, thirdSpace) : rest;

        if (secondArg.length() > 0 && !isdigit(secondArg.charAt(0))) {
          if (!getRGBFromName(secondArg, color2)) {
            if (!ensureDebugBuffer()) return "Error: Unknown color";
            snprintf(getDebugBuffer(), 1024, "Error: Unknown color '%s'", secondArg.c_str());
            return getDebugBuffer();
          }

          if (thirdSpace >= 0) {
            String durationStr = rest.substring(thirdSpace + 1);
            duration = durationStr.toInt();
            if (duration < 100) duration = 100;
            if (duration > 60000) duration = 60000;
          }
        } else if (secondArg.length() > 0) {
          duration = secondArg.toInt();
          if (duration < 100) duration = 100;
          if (duration > 60000) duration = 60000;
        }
      }
    }
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // Execute effect — parse via the shared name table (same list the OLED and
  // G2 pickers render, so the surfaces can't drift from the parser).
  const int effectCode = ledEffectCodeForName(effectType);
  if (effectCode == EFFECT_NONE) {
    snprintf(getDebugBuffer(), 1024, "Error: Unknown effect: %s. Options: fade, pulse, blink, rainbow, strobe", effectType.c_str());
    return getDebugBuffer();
  }

  // Non-blocking: the main-loop ledEffectTick() drives the frames. The command
  // (and any UI waiting on it) returns immediately instead of parking cmd_exec
  // for the whole duration. Starting a new effect replaces a running one.
  ledEffectStart(effectCode, color1, color2, duration);
  snprintf(getDebugBuffer(), 1024, "%s effect started (%lums)", effectType.c_str(), duration);
  return getDebugBuffer();
}

// ============================================================================
// NeoPixel Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry neopixelCommands[] = {
  { "ledcolor", "Set LED color: <color>", false, cmd_ledcolor, "Usage: ledcolor <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>", "led", "change color" },
  { "ledclear", "Turn off LED.", false, cmd_ledclear, nullptr, "led", "turn off" },
  { "ledeffect", "Run LED effect: <effect>", false, cmd_ledeffect, "Usage: ledeffect <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000]" },
};

const size_t neopixelCommandsCount = sizeof(neopixelCommands) / sizeof(neopixelCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// LED Settings Module (merged from System_LED.cpp)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry ledSettingEntries[] = {
  { "ledBrightness", SETTING_INT, &gSettings.ledBrightness, 100, 0, nullptr, 0, 100, "Brightness", nullptr, false, nullptr, "ledbrightness" },
  { "ledStartupEnabled", SETTING_BOOL, &gSettings.ledStartupEnabled, true, 0, nullptr, 0, 1, "Startup Enabled", nullptr, false, nullptr, "ledstartupenabled" },
  { "ledStartupEffect", SETTING_STRING, &gSettings.ledStartupEffect, 0, 0, "rainbow", 0, 0, "Startup Effect", "none,rainbow,pulse,fade,blink,strobe", false, nullptr, "ledstartupeffect" },
  { "ledStartupColor", SETTING_STRING, &gSettings.ledStartupColor, 0, 0, "cyan", 0, 0, "Startup Color", nullptr, false, nullptr, "ledstartupcolor" },
  { "ledStartupColor2", SETTING_STRING, &gSettings.ledStartupColor2, 0, 0, "magenta", 0, 0, "Startup Color 2", nullptr, false, nullptr, "ledstartupcolor2" },
  { "ledStartupDuration", SETTING_INT, &gSettings.ledStartupDuration, 1000, 0, nullptr, 100, 10000, "Startup Duration (ms)", nullptr, false, nullptr, "ledstartupduration" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule ledSettingsModule = {
  "led", "hardware.led", ledSettingEntries,
  sizeof(ledSettingEntries) / sizeof(ledSettingEntries[0]),
  nullptr,
  "LED brightness and startup effects"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp
