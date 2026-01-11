// ============================================================================
// OLED Logging Mode
// ============================================================================
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Command.h"
#include "System_User.h"
#include "System_Logging.h"
#include "System_Utils.h"
#include "System_SensorLogging.h"
#include "System_Debug.h"

// External OLED display pointer
extern Adafruit_SSD1306* oledDisplay;
extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;

// Logging mode state enum (must match OLED_Display.h if defined there)
enum LoggingMenuState {
  LOG_MENU_MAIN,
  LOG_MENU_SENSOR,
  LOG_MENU_SYSTEM,
  LOG_MENU_SENSOR_CONFIG,
  LOG_MENU_VIEWER
};

static LoggingMenuState loggingCurrentState = LOG_MENU_MAIN;
static int loggingMenuSelection = 0;
static int loggingSensorConfigSelection = 0;

// Menu items
static const char* loggingMainMenuItems[] = {
  "Sensor Logging",
  "System Logging",
  "Back"
};
static const int loggingMainMenuCount = 3;

static const char* loggingSensorMenuItems[] = {
  "Start Logging",
  "Stop Logging",
  "Configure"
};
static const int loggingSensorMenuCount = 3;

static const char* loggingSystemMenuItems[] = {
  "Start Logging",
  "Stop Logging"
};
static const int loggingSystemMenuCount = 2;

static const char* loggingSensorConfigItems[] = {
  "Thermal",
  "ToF",
  "IMU",
  "Gamepad",
  "APDS",
  "Interval",
  "Format",
  "Back"
};
static const int loggingSensorConfigCount = 8;

// Helper to draw menu items
static void drawLoggingMenuItem(int y, const char* text, bool selected, bool enabled = true) {
  if (selected) {
    oledDisplay->fillRect(0, y, 128, 10, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  } else {
    oledDisplay->setTextColor(enabled ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_WHITE);
  }
  
  oledDisplay->setCursor(selected ? 4 : 2, y + 1);
  
  if (!enabled && !selected) {
    char dimmed[32];
    snprintf(dimmed, sizeof(dimmed), "%.20s", text);
    oledDisplay->print(dimmed);
  } else {
    oledDisplay->print(text);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
}

// Execute logging command with proper authentication context
static void executeLoggingCommand(const String& cmd) {
  if (!ensureDebugBuffer()) return;
  
  // Create authentication context for OLED
  extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);
  
  AuthContext ctx;
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user = gLocalDisplayAuthed ? gLocalDisplayUser : "";
  ctx.ip = "oled";
  ctx.path = "/oled/logging";
  ctx.sid = "";
  
  char out[512];
  bool success = executeCommand(ctx, cmd.c_str(), out, sizeof(out));
  
  if (!success && strlen(out) > 0) {
    Serial.printf("[LOGGING_CMD] Command failed: %s\n", out);
  }
}

// Logging display function
static void displayLoggingMode() {
  if (!oledDisplay) {
    return;
  }
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  switch (loggingCurrentState) {
    case LOG_MENU_MAIN: {
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Logging Control");
      
      int startY = 12;
      for (int i = 0; i < loggingMainMenuCount; i++) {
        drawLoggingMenuItem(startY + (i * 10), loggingMainMenuItems[i], i == loggingMenuSelection);
      }
      break;
    }
    
    case LOG_MENU_SENSOR: {
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Sensor Logging");
      
      oledDisplay->setCursor(0, 10);
      if (gSensorLoggingEnabled) {
        oledDisplay->print("Status: ACTIVE");
      } else {
        oledDisplay->print("Status: STOPPED");
      }
      
      int startY = 22;
      for (int i = 0; i < loggingSensorMenuCount; i++) {
        bool enabled = true;
        if (i == 0 && gSensorLoggingEnabled) enabled = false;
        if (i == 1 && !gSensorLoggingEnabled) enabled = false;
        
        drawLoggingMenuItem(startY + (i * 10), loggingSensorMenuItems[i], i == loggingMenuSelection, enabled);
      }
      break;
    }
    
    case LOG_MENU_SYSTEM: {
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("System Logging");
      
      oledDisplay->setCursor(0, 10);
      if (gSystemLogEnabled) {
        oledDisplay->print("Status: ACTIVE");
      } else {
        oledDisplay->print("Status: STOPPED");
      }
      
      int startY = 22;
      for (int i = 0; i < loggingSystemMenuCount; i++) {
        bool enabled = true;
        if (i == 0 && gSystemLogEnabled) enabled = false;
        if (i == 1 && !gSystemLogEnabled) enabled = false;
        
        drawLoggingMenuItem(startY + (i * 10), loggingSystemMenuItems[i], i == loggingMenuSelection, enabled);
      }
      break;
    }
    
    case LOG_MENU_SENSOR_CONFIG: {
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Sensor Config");
      
      int visibleStart = max(0, loggingSensorConfigSelection - 1);
      int visibleEnd = min(loggingSensorConfigCount, visibleStart + 4);
      
      int y = 10;
      for (int i = visibleStart; i < visibleEnd; i++) {
        bool selected = (i == loggingSensorConfigSelection);
        
        char itemText[32];
        if (i < 5) {
          bool enabled = false;
          if (i == 0) enabled = (gSensorLogMask & LOG_THERMAL);
          else if (i == 1) enabled = (gSensorLogMask & LOG_TOF);
          else if (i == 2) enabled = (gSensorLogMask & LOG_IMU);
          else if (i == 3) enabled = (gSensorLogMask & LOG_GAMEPAD);
          else if (i == 4) enabled = (gSensorLogMask & LOG_APDS);
          
          snprintf(itemText, sizeof(itemText), "%s: %s", 
                   loggingSensorConfigItems[i], enabled ? "ON" : "OFF");
        } else if (i == 5) {
          snprintf(itemText, sizeof(itemText), "Int: %lums", gSensorLogIntervalMs);
        } else if (i == 6) {
          snprintf(itemText, sizeof(itemText), "Fmt: %s", gSensorLogFormatCSV ? "CSV" : "TXT");
        } else {
          snprintf(itemText, sizeof(itemText), "%s", loggingSensorConfigItems[i]);
        }
        
        drawLoggingMenuItem(y, itemText, selected);
        y += 10;
      }
      
      if (visibleStart > 0) {
        oledDisplay->setCursor(120, 10);
        oledDisplay->print("^");
      }
      if (visibleEnd < loggingSensorConfigCount) {
        oledDisplay->setCursor(120, 50);
        oledDisplay->print("v");
      }
      break;
    }
    
    case LOG_MENU_VIEWER: {
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Log Viewer");
      oledDisplay->setCursor(0, 12);
      oledDisplay->print("Not implemented");
      oledDisplay->setCursor(0, 24);
      oledDisplay->print("Use CLI viewer");
      oledDisplay->setCursor(0, 36);
      oledDisplay->print("or web interface");
      break;
    }
  }
}

// Logging input handler
static bool handleLoggingModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  bool handled = false;
  
  // Use centralized navigation events for proper debounce/auto-repeat
  if (gNavEvents.up) {
    if (loggingCurrentState == LOG_MENU_MAIN) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingMainMenuCount) % loggingMainMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingSensorMenuCount) % loggingSensorMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingSystemMenuCount) % loggingSystemMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      loggingSensorConfigSelection = (loggingSensorConfigSelection - 1 + loggingSensorConfigCount) % loggingSensorConfigCount;
    }
    handled = true;
  } else if (gNavEvents.down) {
    if (loggingCurrentState == LOG_MENU_MAIN) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingMainMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingSensorMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingSystemMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      loggingSensorConfigSelection = (loggingSensorConfigSelection + 1) % loggingSensorConfigCount;
    }
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (loggingCurrentState == LOG_MENU_MAIN) {
      if (loggingMenuSelection == 0) {
        loggingCurrentState = LOG_MENU_SENSOR;
        loggingMenuSelection = 0;
      } else if (loggingMenuSelection == 1) {
        loggingCurrentState = LOG_MENU_SYSTEM;
        loggingMenuSelection = 0;
      } else if (loggingMenuSelection == 2) {
        return false;
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      if (loggingMenuSelection == 0 && !gSensorLoggingEnabled) {
        executeLoggingCommand("sensorlog start");
      } else if (loggingMenuSelection == 1 && gSensorLoggingEnabled) {
        executeLoggingCommand("sensorlog stop");
      } else if (loggingMenuSelection == 2) {
        loggingCurrentState = LOG_MENU_SENSOR_CONFIG;
        loggingSensorConfigSelection = 0;
      }
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      if (loggingMenuSelection == 0 && !gSystemLogEnabled) {
        executeLoggingCommand("log start");
      } else if (loggingMenuSelection == 1 && gSystemLogEnabled) {
        executeLoggingCommand("log stop");
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      if (loggingSensorConfigSelection < 5) {
        uint8_t mask = 0;
        if (loggingSensorConfigSelection == 0) mask = LOG_THERMAL;
        else if (loggingSensorConfigSelection == 1) mask = LOG_TOF;
        else if (loggingSensorConfigSelection == 2) mask = LOG_IMU;
        else if (loggingSensorConfigSelection == 3) mask = LOG_GAMEPAD;
        else if (loggingSensorConfigSelection == 4) mask = LOG_APDS;
        
        gSensorLogMask ^= mask;
      } else if (loggingSensorConfigSelection == 5) {
        if (gSensorLogIntervalMs == 1000) gSensorLogIntervalMs = 5000;
        else if (gSensorLogIntervalMs == 5000) gSensorLogIntervalMs = 10000;
        else if (gSensorLogIntervalMs == 10000) gSensorLogIntervalMs = 30000;
        else if (gSensorLogIntervalMs == 30000) gSensorLogIntervalMs = 60000;
        else gSensorLogIntervalMs = 1000;
      } else if (loggingSensorConfigSelection == 6) {
        gSensorLogFormatCSV = !gSensorLogFormatCSV;
      } else if (loggingSensorConfigSelection == 7) {
        loggingCurrentState = LOG_MENU_SENSOR;
        loggingMenuSelection = 2;
      }
    }
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (loggingCurrentState == LOG_MENU_SENSOR) {
      loggingCurrentState = LOG_MENU_MAIN;
      loggingMenuSelection = 0;
      handled = true;
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      loggingCurrentState = LOG_MENU_MAIN;
      loggingMenuSelection = 1;
      handled = true;
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      loggingCurrentState = LOG_MENU_SENSOR;
      loggingMenuSelection = 2;
      handled = true;
    } else if (loggingCurrentState == LOG_MENU_MAIN) {
      // At main menu - let global handler pop mode stack
      return false;
    }
  }
  
  return handled;
}

// Logging availability check
static bool isLoggingModeAvailable(String* outReason) {
  return true;
}

// Logging mode registration
static const OLEDModeEntry loggingModeEntry = {
  OLED_LOGGING,
  "Logging",
  "file_text",
  displayLoggingMode,
  isLoggingModeAvailable,
  handleLoggingModeInput,
  true,
  93
};

static const OLEDModeEntry loggingModes[] = { loggingModeEntry };

// Register with unique variable name
static OLEDModeRegistrar _oled_mode_registrar_logging(loggingModes, sizeof(loggingModes) / sizeof(loggingModes[0]), "Logging");

// Force linker to include this file - called from OLED_Utils.cpp
void oledLoggingModeInit() {
  // Static registrar already handles registration during global init
  // This function exists solely to force the linker to include this translation unit
}

#endif // ENABLE_OLED_DISPLAY
