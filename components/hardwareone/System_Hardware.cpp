#include <Arduino.h>

#include "System_Debug.h"
#include "System_Settings.h"
#include "System_Utils.h"

extern bool ensureDebugBuffer();

// Hardware LED settings (migrated from .ino)
const char* cmd_hardwareled_brightness(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledbrightness <0..100>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 100) return "Error: LED brightness must be 0..100";
  gSettings.ledBrightness = v;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED brightness set to %d%%", v);
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupenabled(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledstartupenabled <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  gSettings.ledStartupEnabled = enabled;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED startup effect %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupeffect(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledstartupeffect <rainbow|pulse|solid>";
  while (*p == ' ') p++;  // Skip whitespace
  // Store effect name (case-insensitive match, store lowercase)
  if (strncasecmp(p, "rainbow", 7) == 0) {
    gSettings.ledStartupEffect = "rainbow";
  } else if (strncasecmp(p, "pulse", 5) == 0) {
    gSettings.ledStartupEffect = "pulse";
  } else if (strncasecmp(p, "solid", 5) == 0) {
    gSettings.ledStartupEffect = "solid";
  } else {
    gSettings.ledStartupEffect = p;  // Store as-is for other values
  }
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED startup effect set to %s", gSettings.ledStartupEffect.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupcolor(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledstartupcolor <red|green|blue|cyan|magenta|yellow|white|orange|purple>";
  while (*p == ' ') p++;  // Skip whitespace
  gSettings.ledStartupColor = p;  // Store as-is
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED startup color set to %s", gSettings.ledStartupColor.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupcolor2(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledstartupcolor2 <red|green|blue|cyan|magenta|yellow|white|orange|purple>";
  while (*p == ' ') p++;  // Skip whitespace
  gSettings.ledStartupColor2 = p;  // Store as-is
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED startup color 2 set to %s", gSettings.ledStartupColor2.c_str());
  return getDebugBuffer();
}

const char* cmd_hardwareled_startupduration(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: hardwareledstartupduration <100..10000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 100 || v > 10000) return "Error: LED duration must be 100..10000 ms";
  gSettings.ledStartupDuration = v;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "LED startup duration set to %dms", v);
  return getDebugBuffer();
}
