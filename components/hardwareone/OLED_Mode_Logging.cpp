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
#include "System_Settings.h"
#include "System_Debug.h"

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
static OLEDScrollState loggingConfigScroll;

// Menu items
static const char* loggingMainMenuItems[] = {
  "Sensor Logging",
  "System Logging",
  "Auto-Start",
  "Back"
};
static const int loggingMainMenuCount = 4;

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
  "GPS",
  "Interval",
  "Format",
  "Back"
};
static const int loggingSensorConfigCount = 9;

// Helper to draw menu items
static void drawLoggingMenuItem(int y, const char* text, bool selected, bool enabled = true) {
  if (selected) {
    oledDisplay->fillRect(0, y, 128, 10, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  } else {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
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

// (logging commands use executeOLEDCommand from OLED_Utils.h)

// Logging display function
static void displayLoggingMode() {
  if (!oledDisplay) {
    return;
  }
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  switch (loggingCurrentState) {
    case LOG_MENU_MAIN: {
      int startY = OLED_CONTENT_START_Y;
      for (int i = 0; i < loggingMainMenuCount; i++) {
        char itemText[32];
        if (i == 2) {
          snprintf(itemText, sizeof(itemText), "Auto-Start: %s", gSettings.sensorLogAutoStart ? "ON" : "OFF");
          drawLoggingMenuItem(startY + (i * 10), itemText, i == loggingMenuSelection);
        } else {
          drawLoggingMenuItem(startY + (i * 10), loggingMainMenuItems[i], i == loggingMenuSelection);
        }
      }
      break;
    }
    
    case LOG_MENU_SENSOR: {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
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
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
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
      static char sCfgBuf[9][24];
      const uint8_t masks[6] = {LOG_THERMAL, LOG_TOF, LOG_IMU, LOG_GAMEPAD, LOG_APDS, LOG_GPS};
      for (int i = 0; i < 6; i++) {
        snprintf(sCfgBuf[i], 24, "%s: %s", loggingSensorConfigItems[i], (gSensorLogMask & masks[i]) ? "ON" : "OFF");
      }
      snprintf(sCfgBuf[6], 24, "Int: %lums", gSensorLogIntervalMs);
      const char* fmtName = (gSensorLogFormat == SENSOR_LOG_CSV) ? "CSV" :
                            (gSensorLogFormat == SENSOR_LOG_TRACK) ? "TRK" : "TXT";
      snprintf(sCfgBuf[7], 24, "Fmt: %s", fmtName);
      snprintf(sCfgBuf[8], 24, "%s", loggingSensorConfigItems[8]);
      
      int savedSel = loggingSensorConfigSelection;
      int savedOff = loggingConfigScroll.scrollOffset;
      oledScrollClear(&loggingConfigScroll);
      for (int i = 0; i < loggingSensorConfigCount; i++) {
        oledScrollAddItem(&loggingConfigScroll, sCfgBuf[i], nullptr);
      }
      loggingConfigScroll.selectedIndex = savedSel;
      loggingConfigScroll.scrollOffset = savedOff;
      
      oledScrollRender(oledDisplay, &loggingConfigScroll, true, true);
      break;
    }
    
    case LOG_MENU_VIEWER: {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->print("Not implemented");
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 12);
      oledDisplay->print("Use CLI viewer");
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 24);
      oledDisplay->print("or web interface");
      break;
    }
  }
}

// Logging input handler
static bool handleLoggingModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  bool handled = false;
  
  // Use centralized navigation events for proper debounce/auto-repeat
  if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
    if (oledScrollHandleNav(&loggingConfigScroll)) {
      loggingSensorConfigSelection = loggingConfigScroll.selectedIndex;
      handled = true;
    }
  } else if (gNavEvents.up) {
    if (loggingCurrentState == LOG_MENU_MAIN) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingMainMenuCount) % loggingMainMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingSensorMenuCount) % loggingSensorMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      loggingMenuSelection = (loggingMenuSelection - 1 + loggingSystemMenuCount) % loggingSystemMenuCount;
    }
    handled = true;
  } else if (gNavEvents.down) {
    if (loggingCurrentState == LOG_MENU_MAIN) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingMainMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingSensorMenuCount;
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      loggingMenuSelection = (loggingMenuSelection + 1) % loggingSystemMenuCount;
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
        executeOLEDCommand("sensorlog autostart");
      } else if (loggingMenuSelection == 3) {
        return false;
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      if (loggingMenuSelection == 0 && !gSensorLoggingEnabled) {
        executeOLEDCommand("sensorlog start");
      } else if (loggingMenuSelection == 1 && gSensorLoggingEnabled) {
        executeOLEDCommand("sensorlog stop");
      } else if (loggingMenuSelection == 2) {
        loggingCurrentState = LOG_MENU_SENSOR_CONFIG;
        loggingSensorConfigSelection = 0;
        oledScrollInit(&loggingConfigScroll, nullptr, 4);
        oledScrollSetSplitPane(&loggingConfigScroll, 128, 0, 0);
      }
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      if (loggingMenuSelection == 0 && !gSystemLogEnabled) {
        executeOLEDCommand("log start");
      } else if (loggingMenuSelection == 1 && gSystemLogEnabled) {
        executeOLEDCommand("log stop");
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      if (loggingSensorConfigSelection < 6) {
        uint8_t mask = 0;
        if (loggingSensorConfigSelection == 0) mask = LOG_THERMAL;
        else if (loggingSensorConfigSelection == 1) mask = LOG_TOF;
        else if (loggingSensorConfigSelection == 2) mask = LOG_IMU;
        else if (loggingSensorConfigSelection == 3) mask = LOG_GAMEPAD;
        else if (loggingSensorConfigSelection == 4) mask = LOG_APDS;
        else if (loggingSensorConfigSelection == 5) mask = LOG_GPS;
        
        uint8_t newMask = gSensorLogMask ^ mask;
        String sensorList = "";
        if (newMask & LOG_THERMAL)  sensorList += "thermal,";
        if (newMask & LOG_TOF)      sensorList += "tof,";
        if (newMask & LOG_IMU)      sensorList += "imu,";
        if (newMask & LOG_GAMEPAD)  sensorList += "gamepad,";
        if (newMask & LOG_APDS)     sensorList += "apds,";
        if (newMask & LOG_GPS)      sensorList += "gps,";
        if (sensorList.endsWith(",")) sensorList.remove(sensorList.length() - 1);
        if (sensorList.isEmpty()) sensorList = "none";
        executeOLEDCommand("sensorlog sensors " + sensorList);
      } else if (loggingSensorConfigSelection == 6) {
        uint32_t newInterval;
        if (gSensorLogIntervalMs == 1000) newInterval = 5000;
        else if (gSensorLogIntervalMs == 5000) newInterval = 10000;
        else if (gSensorLogIntervalMs == 10000) newInterval = 30000;
        else if (gSensorLogIntervalMs == 30000) newInterval = 60000;
        else newInterval = 1000;
        executeOLEDCommand("sensorlog interval " + String(newInterval));
      } else if (loggingSensorConfigSelection == 7) {
        const char* newFmt;
        if (gSensorLogFormat == SENSOR_LOG_TEXT) newFmt = "csv";
        else if (gSensorLogFormat == SENSOR_LOG_CSV) newFmt = "track";
        else newFmt = "text";
        executeOLEDCommand(String("sensorlog format ") + newFmt);
      } else if (loggingSensorConfigSelection == 8) {
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
      loggingMenuSelection = 2;  // Configure
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
  93,
  "A:Select B:Back"
};

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry loggingModes[] = { loggingModeEntry };

REGISTER_OLED_MODE_MODULE(loggingModes, sizeof(loggingModes) / sizeof(loggingModes[0]), "Logging");

// Force linker to include this file - called from OLED_Utils.cpp
void oledLoggingModeInit() {
  // Static registrar already handles registration during global init
  // This function exists solely to force the linker to include this translation unit
}

#endif // ENABLE_OLED_DISPLAY
