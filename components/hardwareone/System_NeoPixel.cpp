/**
 * NeoPixel LED Module - QT Py ESP32 built-in RGB LED control
 */

#include "System_NeoPixel.h"
#include "System_Utils.h"
#include "System_Command.h"  // For CommandModuleRegistrar
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

// External dependencies
extern bool ensureDebugBuffer();
extern char* getDebugBuffer();
extern void broadcastOutput(const String& msg);

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
  // Enable power to STEMMA QT connector on Feather V2
  // This pin powers the 3.3V regulator for I2C devices and NeoPixel
  #ifdef NEOPIXEL_I2C_POWER
    pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
    delay(10);  // Allow power to stabilize
  #endif
  
  pixels.begin();
  pixels.setBrightness(50);  // Set moderate brightness
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

void runLEDEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration) {
  unsigned long startTime = millis();
  
  switch (effectType) {
    case 1:  // Fade
      while (millis() - startTime < duration) {
        float progress = (float)(millis() - startTime) / duration;
        RGB currentColor = {
          (uint8_t)(startColor.r + (endColor.r - startColor.r) * progress),
          (uint8_t)(startColor.g + (endColor.g - startColor.g) * progress),
          (uint8_t)(startColor.b + (endColor.b - startColor.b) * progress)
        };
        setLEDColor(currentColor);
        delay(50);
      }
      break;
      
    case 2:  // Blink
      while (millis() - startTime < duration) {
        setLEDColor(startColor);
        delay(250);
        setLEDColor({0, 0, 0});
        delay(250);
      }
      break;
      
    case 3:  // Pulse
      while (millis() - startTime < duration) {
        float progress = (float)(millis() - startTime) / 1000.0;
        int brightness = (int)(127.5 + 127.5 * sin(progress * 3.14159 * 2));
        RGB currentColor = {
          (uint8_t)(startColor.r * brightness / 255),
          (uint8_t)(startColor.g * brightness / 255),
          (uint8_t)(startColor.b * brightness / 255)
        };
        setLEDColor(currentColor);
        delay(50);
      }
      break;
      
    case 4:  // Strobe
      while (millis() - startTime < duration) {
        setLEDColor(startColor);
        delay(50);
        setLEDColor({0, 0, 0});
        delay(50);
      }
      break;
  }
  
  setLEDColor({0, 0, 0});  // Turn off when done
}

// ============================================================================
// NeoPixel Command Handlers
// ============================================================================

const char* cmd_ledcolor(const String& command) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String colorName = command;
  colorName.trim();
  colorName.toLowerCase();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (colorName.length() == 0) {
    return "Usage: ledcolor <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>";
  }
  
  RGB color;
  if (!getRGBFromName(colorName, color)) {
    snprintf(getDebugBuffer(), 1024, "Unknown color: %s", colorName.c_str());
    return getDebugBuffer();
  }
  
  setLEDColor(color);
  snprintf(getDebugBuffer(), 1024, "LED set to %s", colorName.c_str());
  return getDebugBuffer();
}

const char* cmd_ledclear(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  setLEDColor({0, 0, 0});
  return "LED cleared (turned off)";
}

const char* cmd_ledeffect(const String& command) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = command;
  args.trim();

  if (args == "off" || args == "none" || args.length() == 0) {
    setLEDColor({0, 0, 0});
    return "LED effect: off";
  }

  // Parse effect type
  int firstSpace = args.indexOf(' ');
  String effectType = (firstSpace >= 0) ? args.substring(0, firstSpace) : args;
  effectType.toLowerCase();

  String remaining = (firstSpace >= 0) ? args.substring(firstSpace + 1) : "";
  remaining.trim();

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

  // Execute effect
  int effectCode = 0;
  if (effectType == "fade") effectCode = 1;
  else if (effectType == "blink") effectCode = 2;
  else if (effectType == "pulse") effectCode = 3;
  else if (effectType == "strobe") effectCode = 4;
  else {
    snprintf(getDebugBuffer(), 1024, "Unknown effect: %s. Options: fade, blink, pulse, strobe", effectType.c_str());
    return getDebugBuffer();
  }

  runLEDEffect(effectCode, color1, color2, duration);
  snprintf(getDebugBuffer(), 1024, "%s effect completed (%lums)", effectType.c_str(), duration);
  return getDebugBuffer();
}

// ============================================================================
// NeoPixel Command Registry
// ============================================================================

const CommandEntry neopixelCommands[] = {
  { "ledcolor", "Set LED color by name.", false, cmd_ledcolor, "Usage: ledcolor <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>", "led", "change color" },
  { "ledclear", "Turn off LED.", false, cmd_ledclear, nullptr, "led", "turn off" },
  { "ledeffect", "Run a predefined LED effect.", false, cmd_ledeffect },
};

const size_t neopixelCommandsCount = sizeof(neopixelCommands) / sizeof(neopixelCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _neopixel_cmd_registrar(neopixelCommands, neopixelCommandsCount, "neopixel");

// ============================================================================
// LED Settings Module (merged from System_LED.cpp)
// ============================================================================

static const SettingEntry ledSettingEntries[] = {
  { "ledBrightness",      SETTING_INT,    &gSettings.ledBrightness,      100, 0, nullptr, 0, 255, "Brightness", nullptr },
  { "ledStartupEnabled",  SETTING_BOOL,   &gSettings.ledStartupEnabled,  true, 0, nullptr, 0, 1, "Startup Enabled", nullptr },
  { "ledStartupEffect",   SETTING_STRING, &gSettings.ledStartupEffect,   0, 0, "rainbow", 0, 0, "Startup Effect", "rainbow,pulse,solid,chase,breathe" },
  { "ledStartupColor",    SETTING_STRING, &gSettings.ledStartupColor,    0, 0, "cyan", 0, 0, "Startup Color", nullptr },
  { "ledStartupColor2",   SETTING_STRING, &gSettings.ledStartupColor2,   0, 0, "magenta", 0, 0, "Startup Color 2", nullptr },
  { "ledStartupDuration", SETTING_INT,    &gSettings.ledStartupDuration, 1000, 0, nullptr, 100, 10000, "Startup Duration (ms)", nullptr }
};

extern const SettingsModule ledSettingsModule = {
  "led", "led", ledSettingEntries,
  sizeof(ledSettingEntries) / sizeof(ledSettingEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp
