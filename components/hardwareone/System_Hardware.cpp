#include <Arduino.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_NeoPixel.h"
#include "System_Settings.h"
#include "System_Utils.h"

// Hardware LED settings (migrated from .ino)
const char* cmd_hardwareled_brightness(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Usage: hardwareledbrightness <0..100>";
  int v = arg.toInt();
  if (v < 0 || v > 100) return "Error: LED brightness must be 0..100";
  setSetting(gSettings.ledBrightness, v);
  // Apply immediately to hardware (0-100% -> 0-255)
  extern Adafruit_NeoPixel pixels;
  pixels.setBrightness((uint8_t)(v * 255 / 100));
  pixels.show();
  snprintf(getDebugBuffer(), 1024, "LED brightness set to %d%%", v);
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Usage: hardwareledstartupenabled <0|1>";
  bool enabled = (arg == "1" || arg.equalsIgnoreCase("true"));
  setSetting(gSettings.ledStartupEnabled, enabled);
  snprintf(getDebugBuffer(), 1024, "LED startup effect %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupeffect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) return "Usage: hardwareledstartupeffect <none|rainbow|pulse|fade|blink|strobe>";
  if (arg == "none"   || arg == "rainbow" || arg == "pulse" ||
      arg == "fade"   || arg == "blink"   || arg == "strobe") {
    setSetting(gSettings.ledStartupEffect, arg.c_str());
  } else {
    return "Error: Unknown effect. Valid: none, rainbow, pulse, fade, blink, strobe";
  }
  snprintf(getDebugBuffer(), 1024, "LED startup effect set to %s", gSettings.ledStartupEffect.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupcolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Usage: hardwareledstartupcolor <red|green|blue|cyan|magenta|yellow|white|orange|purple>";
  setSetting(gSettings.ledStartupColor, arg.c_str());
  snprintf(getDebugBuffer(), 1024, "LED startup color set to %s", gSettings.ledStartupColor.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupcolor2(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Usage: hardwareledstartupcolor2 <red|green|blue|cyan|magenta|yellow|white|orange|purple>";
  setSetting(gSettings.ledStartupColor2, arg.c_str());
  snprintf(getDebugBuffer(), 1024, "LED startup color 2 set to %s", gSettings.ledStartupColor2.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupduration(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Usage: hardwareledstartupduration <100..10000>";
  int v = arg.toInt();
  if (v < 100 || v > 10000) return "Error: LED duration must be 100..10000 ms";
  setSetting(gSettings.ledStartupDuration, v);
  snprintf(getDebugBuffer(), 1024, "LED startup duration set to %dms", v);
  return getDebugBuffer();
}

// ============================================================================
// Command Registration
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
extern const CommandEntry hardwareCommands[] = {
  { "hardwareledbrightness",    "Set LED brightness 0-100.",                      false, cmd_hardwareled_brightness,    "Usage: hardwareledbrightness <0..100>" },
  { "hardwareledstartupenabled","Enable/disable LED startup effect [0|1].",        false, cmd_hardwareled_startupenabled,"Usage: hardwareledstartupenabled <0|1>" },
  { "hardwareledstartupeffect", "Set LED startup effect [none|rainbow|pulse|fade|blink|strobe].",   false, cmd_hardwareled_startupeffect, "Usage: hardwareledstartupeffect <none|rainbow|pulse|fade|blink|strobe>" },
  { "hardwareledstartupcolor",  "Set LED startup primary color.",                  false, cmd_hardwareled_startupcolor,  "Usage: hardwareledstartupcolor <red|green|blue|cyan|magenta|yellow|white|orange|purple>" },
  { "hardwareledstartupcolor2", "Set LED startup secondary color.",                false, cmd_hardwareled_startupcolor2, "Usage: hardwareledstartupcolor2 <red|green|blue|cyan|magenta|yellow|white|orange|purple>" },
  { "hardwareledstartupduration","Set LED startup effect duration in ms.",         false, cmd_hardwareled_startupduration,"Usage: hardwareledstartupduration <100..10000>" },
};

extern const size_t hardwareCommandsCount = sizeof(hardwareCommands) / sizeof(hardwareCommands[0]);
