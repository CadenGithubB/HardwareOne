// ============================================================================
// OLED Logging Mode
// ============================================================================
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <algorithm>
#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Command.h"
#include "System_User.h"
#include "System_Logging.h"
#include "System_Utils.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "System_Debug.h"

// LoggingMenuState enum and extern declarations in OLED_Utils.h
LoggingMenuState loggingCurrentState = LOG_MENU_MAIN;
int loggingMenuSelection = 0;
static int loggingSensorConfigSelection = 0;
EXT_RAM_BSS_ATTR static OLEDScrollState loggingConfigScroll;

// Menu items — the Auto-Start toggles moved to Config > Settings (modules
// "sensorlog" / "systemlog", entries sensorLogAutoStart / systemLogAutoStart,
// saved via the real `sensorlog autostart` / `log autostart` commands).
// These menus start/stop logging; each Configure screen edits the saved
// options the next start uses, through the same commands the CLI uses.
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
  "Stop Logging",
  "Configure"
};
static const int loggingSystemMenuCount = 3;

static constexpr int loggingIntervalRow = kSensorLogSensorCount;
static constexpr int loggingFormatRow = loggingIntervalRow + 1;
static constexpr int loggingBackRow = loggingFormatRow + 1;
static constexpr int loggingSensorConfigCount = loggingBackRow + 1;

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

// (logging commands run through oledRunCommand from OLED_Utils.h, which
// toasts a refusal; the rows show the new state on success)

// ============================================================================
// System Logging > Configure
// ============================================================================
// Tags, Categories and the read-only Path. Categories opens the debug-flag
// families (DBG_BANK_LIST), then the flags in one family. Every row shows the
// saved option the next `log start` uses, and changes go through the same
// commands as the CLI: `logcategorytags` and `systemlogflags` (which saves the
// whole mask and also applies it live).

static constexpr int sysCfgTagsRow = 0;
static constexpr int sysCfgCategoriesRow = 1;
static constexpr int sysCfgPathRow = 2;
static constexpr int sysCfgBackRow = 3;
static constexpr int sysCfgRowCount = 4;

// A category is a debug flag with a tag; the tagless Control row is not one.
static constexpr bool isLogCategory(int flag) { return kDbgTag[flag][0] != '\0'; }

static constexpr int logCategoryCount(int bank) {
  int n = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; i++) {
    if (kDbgBank[i] == bank && isLogCategory(i)) n++;
  }
  return n;
}

static constexpr int maxLogCategoriesPerBank() {
  int most = 0;
  for (int b = 0; b < DBG_BANK_COUNT; b++) most = std::max(most, logCategoryCount(b));
  return most;
}

// Row pool for whichever of these lists is on screen. The family list holds
// at most every bank but Control, plus Back. Rows are clipped to what fits
// beside the "> " cursor on one line.
static constexpr int sysRowCount =
    std::max({sysCfgRowCount, (int)DBG_BANK_COUNT, maxLogCategoriesPerBank() + 1});
static_assert(sysRowCount <= OLED_SCROLL_MAX_ITEMS, "system-log list outgrew OLEDScrollState");
static constexpr int sysRowChars = std::min(SCREEN_WIDTH / 6 - 2, 23);
EXT_RAM_BSS_ATTR static char sysRowBuf[sysRowCount][sysRowChars + 1];

// The flag list keeps one heading line (family name and count) above it.
static constexpr int sysListHeadingPx = 8;

static int sysBank = 0;      // family shown in LOG_MENU_SYSTEM_FLAGS
static int sysBankRow = 0;   // its row in the family list, for Back

// Family on row `row` of the family list, or -1 for its Back row.
static int sysBankAtRow(int row) {
  for (int b = 0; b < DBG_BANK_COUNT; b++) {
    if (logCategoryCount(b) == 0) continue;
    if (row-- == 0) return b;
  }
  return -1;
}

// Flag on row `row` of `bank`'s list, or -1 for its Back row.
static int sysFlagAtRow(int bank, int row) {
  for (int i = 0; i < DBG_FLAG_COUNT; i++) {
    if (kDbgBank[i] != bank || !isLogCategory(i)) continue;
    if (row-- == 0) return i;
  }
  return -1;
}

// Categories of `bank` (-1 = every family) that `mask` turns on.
static int logCategoriesOn(int bank, DebugFlagMask mask) {
  int n = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; i++) {
    if (!isLogCategory(i) || (bank >= 0 && kDbgBank[i] != bank)) continue;
    if (mask & kDbgMask[i]) n++;
  }
  return n;
}

static bool loggingStateIsSystemConfig(LoggingMenuState state) {
  return state == LOG_MENU_SYSTEM_CONFIG || state == LOG_MENU_SYSTEM_BANKS ||
         state == LOG_MENU_SYSTEM_FLAGS || state == LOG_MENU_SYSTEM_PATH;
}

// Show one of the list screens with row `selected` highlighted; reservedTopPx
// keeps a heading line above the list.
static void sysOpenList(LoggingMenuState state, int selected, int reservedTopPx = 0) {
  loggingCurrentState = state;
  oledScrollInit(&loggingConfigScroll, nullptr, (OLED_CONTENT_HEIGHT - reservedTopPx) / 8);
  loggingConfigScroll.selectedIndex = selected;
}

// System Logging > Configure, from the System Logging menu.
static void openSystemConfig() {
  sysOpenList(LOG_MENU_SYSTEM_CONFIG, sysCfgTagsRow);
}

// One level up (B, or A on Back).
static void sysConfigBack() {
  switch (loggingCurrentState) {
    case LOG_MENU_SYSTEM_CONFIG:
      loggingCurrentState = LOG_MENU_SYSTEM;
      loggingMenuSelection = 2;  // Configure
      break;
    case LOG_MENU_SYSTEM_BANKS:
      sysOpenList(LOG_MENU_SYSTEM_CONFIG, sysCfgCategoriesRow);
      break;
    case LOG_MENU_SYSTEM_FLAGS:
      sysOpenList(LOG_MENU_SYSTEM_BANKS, sysBankRow);
      break;
    case LOG_MENU_SYSTEM_PATH:
      sysOpenList(LOG_MENU_SYSTEM_CONFIG, sysCfgPathRow);
      break;
    default:
      break;
  }
}

// Rebuild the shared scroll list from the first `rows` pool rows and draw it.
static void sysRenderRows(int rows, int startY = OLED_CONTENT_START_Y) {
  oledScrollClearKeepSelection(&loggingConfigScroll);
  for (int i = 0; i < rows; i++) {
    oledScrollAddItem(&loggingConfigScroll, sysRowBuf[i], nullptr);
  }
  oledScrollRenderSimple(oledDisplay, &loggingConfigScroll, true, startY);
}

static void renderSystemConfig() {
  const DebugFlagMask mask = systemLogConfiguredFlags();
  switch (loggingCurrentState) {
    case LOG_MENU_SYSTEM_CONFIG: {
      snprintf(sysRowBuf[sysCfgTagsRow], sizeof(sysRowBuf[0]), "Tags: %s",
               gSettings.systemLogCategoryTags ? "ON" : "OFF");
      snprintf(sysRowBuf[sysCfgCategoriesRow], sizeof(sysRowBuf[0]), "Categories: %d >",
               logCategoriesOn(-1, mask));
      const char* path = gSettings.systemLogPath.c_str();
      const char* slash = strrchr(path, '/');
      snprintf(sysRowBuf[sysCfgPathRow], sizeof(sysRowBuf[0]), "Path: %s",
               path[0] ? (slash ? slash + 1 : path) : "auto");
      snprintf(sysRowBuf[sysCfgBackRow], sizeof(sysRowBuf[0]), "Back");
      sysRenderRows(sysCfgRowCount);
      break;
    }

    case LOG_MENU_SYSTEM_BANKS: {
      int rows = 0;
      for (int b = 0; b < DBG_BANK_COUNT; b++) {
        const int total = logCategoryCount(b);
        if (total == 0) continue;
        snprintf(sysRowBuf[rows++], sizeof(sysRowBuf[0]), "%s %d/%d",
                 kDbgBankLabel[b], logCategoriesOn(b, mask), total);
      }
      snprintf(sysRowBuf[rows++], sizeof(sysRowBuf[0]), "Back");
      sysRenderRows(rows);
      break;
    }

    case LOG_MENU_SYSTEM_FLAGS: {
      char heading[sysRowChars + 3];
      snprintf(heading, sizeof(heading), "%s %d/%d", kDbgBankLabel[sysBank],
               logCategoriesOn(sysBank, mask), logCategoryCount(sysBank));
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->print(heading);
      int rows = 0;
      for (int i = 0; i < DBG_FLAG_COUNT; i++) {
        if (kDbgBank[i] != sysBank || !isLogCategory(i)) continue;
        snprintf(sysRowBuf[rows++], sizeof(sysRowBuf[0]), "%s %s",
                 (mask & kDbgMask[i]) ? "[+]" : "[ ]", kDbgLabel[i]);
      }
      snprintf(sysRowBuf[rows++], sizeof(sysRowBuf[0]), "Back");
      sysRenderRows(rows, OLED_CONTENT_START_Y + sysListHeadingPx);
      break;
    }

    // The full path, wrapped by character, since a list row clips it.
    case LOG_MENU_SYSTEM_PATH: {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->print("Log file path:");
      const char* path = gSettings.systemLogPath.c_str();
      const char* text = path[0] ? path : "Auto-named when logging starts";
      char line[sysRowChars + 3];
      const int perLine = (int)sizeof(line) - 1;  // a full line: no "> " cursor here
      const int lastY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8;
      int y = OLED_CONTENT_START_Y + 10;
      for (size_t off = 0, len = strlen(text); off < len && y <= lastY; off += perLine, y += 8) {
        snprintf(line, sizeof(line), "%.*s", perLine, text + off);
        oledDisplay->setCursor(0, y);
        oledDisplay->print(line);
      }
      break;
    }

    default:
      break;
  }
}

static bool handleSystemConfigInput(uint32_t newlyPressed) {
  // The path screen has no list; its nav events are simply consumed.
  bool handled = loggingCurrentState == LOG_MENU_SYSTEM_PATH ||
                 oledScrollHandleNav(&loggingConfigScroll);
  const int row = loggingConfigScroll.selectedIndex;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    handled = true;
    if (loggingCurrentState == LOG_MENU_SYSTEM_CONFIG) {
      if (row == sysCfgTagsRow) {
        oledRunCommand(gSettings.systemLogCategoryTags ? "logcategorytags 0" : "logcategorytags 1");
      } else if (row == sysCfgCategoriesRow) {
        sysOpenList(LOG_MENU_SYSTEM_BANKS, 0);
      } else if (row == sysCfgPathRow) {
        loggingCurrentState = LOG_MENU_SYSTEM_PATH;
      } else {
        sysConfigBack();
      }
    } else if (loggingCurrentState == LOG_MENU_SYSTEM_BANKS) {
      const int bank = sysBankAtRow(row);
      if (bank >= 0) {
        sysBank = bank;
        sysBankRow = row;
        sysOpenList(LOG_MENU_SYSTEM_FLAGS, 0, sysListHeadingPx);
      } else {
        sysConfigBack();
      }
    } else if (loggingCurrentState == LOG_MENU_SYSTEM_FLAGS) {
      const int flag = sysFlagAtRow(sysBank, row);
      if (flag >= 0) {
        char mask[80];
        formatSystemLogFlags(systemLogConfiguredFlags() ^ kDbgMask[flag], mask, sizeof(mask));
        oledRunCommand(String("systemlogflags ") + mask);
      } else {
        sysConfigBack();
      }
    } else {
      sysConfigBack();  // the path screen: A closes it like B
    }
  } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    sysConfigBack();
    handled = true;
  }
  return handled;
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
      int startY = OLED_CONTENT_START_Y;
      for (int i = 0; i < loggingMainMenuCount; i++) {
        drawLoggingMenuItem(startY + (i * 10), loggingMainMenuItems[i], i == loggingMenuSelection);
      }
      break;
    }
    
    case LOG_MENU_SENSOR: {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      if (gSensorLoggingRunning) {
        oledDisplay->print("Status: ACTIVE");
      } else {
        oledDisplay->print("Status: STOPPED");
      }
      
      int startY = 22;
      for (int i = 0; i < loggingSensorMenuCount; i++) {
        bool enabled = true;
        if (i == 0 && gSensorLoggingRunning) enabled = false;
        if (i == 1 && !gSensorLoggingRunning) enabled = false;
        drawLoggingMenuItem(startY + (i * 10), loggingSensorMenuItems[i], i == loggingMenuSelection, enabled);
      }
      break;
    }
    
    case LOG_MENU_SYSTEM: {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      if (gSystemLogRunning) {
        oledDisplay->print("Status: ACTIVE");
      } else {
        oledDisplay->print("Status: STOPPED");
      }
      
      int startY = 22;
      for (int i = 0; i < loggingSystemMenuCount; i++) {
        bool enabled = true;
        if (i == 0 && gSystemLogRunning) enabled = false;
        if (i == 1 && !gSystemLogRunning) enabled = false;
        drawLoggingMenuItem(startY + (i * 10), loggingSystemMenuItems[i], i == loggingMenuSelection, enabled);
      }
      break;
    }
    
    case LOG_MENU_SENSOR_CONFIG: {
      EXT_RAM_BSS_ATTR static char sCfgBuf[loggingSensorConfigCount][24];
      const uint8_t selectedMask = sensorLogSelectedMask();
      for (size_t i = 0; i < kSensorLogSensorCount; i++) {
        snprintf(sCfgBuf[i], 24, "%s: %s", kSensorLogSensors[i].label,
                 (selectedMask & kSensorLogSensors[i].bit) ? "ON" : "OFF");
      }
      snprintf(sCfgBuf[loggingIntervalRow], 24, "Int: %lums", (unsigned long)sensorLogIntervalMs());
      const SensorLogFormat format = sensorLogCurrentFormat();
      const char* fmtName = (format == SENSOR_LOG_CSV) ? "CSV" :
                            (format == SENSOR_LOG_TRACK) ? "TRK" : "TXT";
      snprintf(sCfgBuf[loggingFormatRow], 24, "Fmt: %s", fmtName);
      snprintf(sCfgBuf[loggingBackRow], 24, "Back");
      
      int savedSel = loggingSensorConfigSelection;
      int savedOff = loggingConfigScroll.scrollOffset;
      oledScrollClear(&loggingConfigScroll);
      for (int i = 0; i < loggingSensorConfigCount; i++) {
        oledScrollAddItem(&loggingConfigScroll, sCfgBuf[i], nullptr);
      }
      loggingConfigScroll.selectedIndex = savedSel;
      loggingConfigScroll.scrollOffset = savedOff;

      // Shared single-line list: "> " cursor, scrollbar on the right edge
      // while rows are hidden.
      oledScrollRenderSimple(oledDisplay, &loggingConfigScroll);
      break;
    }

    case LOG_MENU_SYSTEM_CONFIG:
    case LOG_MENU_SYSTEM_BANKS:
    case LOG_MENU_SYSTEM_FLAGS:
    case LOG_MENU_SYSTEM_PATH:
      renderSystemConfig();
      break;
  }
}

// Logging input handler
static bool handleLoggingModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (loggingStateIsSystemConfig(loggingCurrentState)) return handleSystemConfigInput(newlyPressed);
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
        return false;
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR) {
      if (loggingMenuSelection == 0 && !gSensorLoggingRunning) {
        String startCmd = String("sensorlog start \"") + gSettings.sensorLogPath + "\"";
        oledRunCommand(startCmd);
      } else if (loggingMenuSelection == 1 && gSensorLoggingRunning) {
        oledRunCommand("sensorlog stop");
      } else if (loggingMenuSelection == 2) {
        loggingCurrentState = LOG_MENU_SENSOR_CONFIG;
        loggingSensorConfigSelection = 0;
        // One 8 px row per entry, as many as the content area holds.
        oledScrollInit(&loggingConfigScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
      }
    } else if (loggingCurrentState == LOG_MENU_SYSTEM) {
      if (loggingMenuSelection == 0 && !gSystemLogRunning) {
        oledRunCommand("log start");
      } else if (loggingMenuSelection == 1 && gSystemLogRunning) {
        oledRunCommand("log stop");
      } else if (loggingMenuSelection == 2) {
        openSystemConfig();
      }
    } else if (loggingCurrentState == LOG_MENU_SENSOR_CONFIG) {
      if (loggingSensorConfigSelection >= 0 &&
          loggingSensorConfigSelection < loggingIntervalRow) {
        oledRunCommand(String("sensorlog sensors toggle ") +
                       kSensorLogSensors[loggingSensorConfigSelection].name);
      } else if (loggingSensorConfigSelection == loggingIntervalRow) {
        uint32_t newInterval;
        const uint32_t interval = sensorLogIntervalMs();
        if (interval == 1000) newInterval = 5000;
        else if (interval == 5000) newInterval = 10000;
        else if (interval == 10000) newInterval = 30000;
        else if (interval == 30000) newInterval = 60000;
        else newInterval = 1000;
        oledRunCommand("sensorlog interval " + String(newInterval));
      } else if (loggingSensorConfigSelection == loggingFormatRow) {
        const char* newFmt;
        const SensorLogFormat format = sensorLogCurrentFormat();
        if (format == SENSOR_LOG_TEXT) newFmt = "csv";
        else if (format == SENSOR_LOG_CSV) newFmt = "track";
        else newFmt = "text";
        oledRunCommand(String("sensorlog format ") + newFmt);
      } else if (loggingSensorConfigSelection == loggingBackRow) {
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
// Entry hook (was menu-select-only at 5348, so a CLI `oledmode logging` could
// land on a stale sub-view): reset to the top logging menu on a fresh visit.
// Back-navigation preserves the sub-view you were in.
static void loggingOnEnter(bool isForward) {
  if (isForward) {
    loggingCurrentState = LOG_MENU_MAIN;
    loggingMenuSelection = 0;
  }
}

static const OLEDModeEntry loggingModeEntry = {
  OLED_LOGGING,
  "Logging",
  "file_text",
  displayLoggingMode,
  isLoggingModeAvailable,
  handleLoggingModeInput,
  true,
  93,
  "A:Select B:Back",
  loggingOnEnter
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
