#include "OLED_SettingsEditor.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include <cstring>

#include "i2csensor-seesaw.h"
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_Utils.h"

// External sensor connection flags
#if ENABLE_THERMAL_SENSOR
extern bool thermalConnected;
#endif
#if ENABLE_TOF_SENSOR
extern bool tofConnected;
#endif

// Check if a setting entry should be visible (used for conditional I2C clock settings)
static bool isSettingVisible(const SettingEntry* entry) {
  if (!entry || !entry->jsonKey) return true;
  
  // Hide Thermal I2C clock if thermal sensor not compiled or not connected
  if (strcmp(entry->jsonKey, "i2cClockThermalHz") == 0) {
#if ENABLE_THERMAL_SENSOR
    return thermalConnected;
#else
    return false;
#endif
  }
  
  // Hide ToF I2C clock if ToF sensor not compiled or not connected
  if (strcmp(entry->jsonKey, "i2cClockToFHz") == 0) {
#if ENABLE_TOF_SENSOR
    return tofConnected;
#else
    return false;
#endif
  }
  
  return true;
}

// Global settings editor context
SettingsEditorContext gSettingsEditor;

// ============================================================================
// Initialization
// ============================================================================

void initSettingsEditor() {
  DEBUG_SYSTEMF("[SettingsEditor] initSettingsEditor called");
  gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
  gSettingsEditor.categoryIndex = 0;
  gSettingsEditor.itemIndex = 0;
  gSettingsEditor.editValue = 0;
  gSettingsEditor.hasChanges = false;
  gSettingsEditor.currentModule = nullptr;
  gSettingsEditor.currentEntry = nullptr;
  gSettingsEditor.errorMessage = "";
  gSettingsEditor.errorDisplayUntil = 0;
  
  // Verify settings modules are registered
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  DEBUG_SYSTEMF("[SettingsEditor] Init complete: %zu modules available", moduleCount);
  if (moduleCount > 0 && modules && modules[0]) {
    DEBUG_SYSTEMF("[SettingsEditor] First module: %s", modules[0]->name);
  }
}

void resetSettingsEditor() {
  initSettingsEditor();
}

// ============================================================================
// Helper Functions
// ============================================================================

// Draw a horizontal slider bar with value indicator
// For bool: shows 0|1 with indicator at current position
// For int: shows min|max with proportional indicator
void drawSettingsSlider(Adafruit_SSD1306* display, int y, int minVal, int maxVal, int currentVal, bool isBool) {
  const int barX = 10;
  const int barY = y;
  const int barWidth = 108;  // Leave room for value text
  const int barHeight = 8;
  
  // Draw slider track
  display->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  
  // Calculate indicator position
  int range = maxVal - minVal;
  if (range == 0) range = 1;  // Avoid division by zero
  int indicatorX = barX + ((currentVal - minVal) * (barWidth - 4)) / range;
  
  // Draw indicator (filled rectangle)
  display->fillRect(indicatorX, barY + 1, 4, barHeight - 2, DISPLAY_COLOR_WHITE);
  
  // Draw min/max labels
  display->setTextSize(1);
  display->setCursor(barX, barY + barHeight + 2);
  if (isBool) {
    display->print("0");
  } else {
    display->print(minVal);
  }
  
  display->setCursor(barX + barWidth - 12, barY + barHeight + 2);
  if (isBool) {
    display->print("1");
  } else {
    // Right-align max value
    String maxStr = String(maxVal);
    display->setCursor(barX + barWidth - (maxStr.length() * 6), barY + barHeight + 2);
    display->print(maxVal);
  }
  
  // Draw current value (centered above bar)
  String valStr = String(currentVal);
  int valX = barX + (barWidth / 2) - (valStr.length() * 3);
  display->setCursor(valX, barY - 10);
  display->setTextSize(1);
  display->print(currentVal);
}

// Get current value from setting entry
int getSettingCurrentValue(const SettingEntry* entry) {
  if (!entry || !entry->valuePtr) return 0;
  
  switch (entry->type) {
    case SETTING_INT:
      return *((int*)entry->valuePtr);
    case SETTING_BOOL:
      return *((bool*)entry->valuePtr) ? 1 : 0;
    default:
      return 0;
  }
}

// Set value to setting entry
void setSettingValue(const SettingEntry* entry, int value) {
  if (!entry || !entry->valuePtr) return;

  switch (entry->type) {
    case SETTING_INT:
      setSetting(*((int*)entry->valuePtr), value);
      break;
    case SETTING_BOOL:
      setSetting(*((bool*)entry->valuePtr), (bool)(value != 0));
      break;
    default:
      break;
  }
}

// Validate value against min/max
bool validateSettingValue(const SettingEntry* entry, int value, String& errorMsg) {
  if (!entry) return false;
  
  // Check range
  if (entry->minVal != 0 || entry->maxVal != 0) {
    if (value < entry->minVal || value > entry->maxVal) {
      errorMsg = "Value must be " + String(entry->minVal) + ".." + String(entry->maxVal);
      return false;
    }
  }
  
  return true;
}

// ============================================================================
// Display Functions
// ============================================================================

void displaySettingsEditor() {
  
  if (!oledDisplay) {
    DEBUG_SYSTEMF("[SettingsEditor] oledDisplay is NULL, returning");
    return;
  }
  
  // Don't call clearDisplay() - the main update loop already cleared the content area
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get module list
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  
  
  // Display error message if active
  if (millis() < gSettingsEditor.errorDisplayUntil) {
    DEBUG_SYSTEMF("[SettingsEditor] Showing error: %s", gSettingsEditor.errorMessage.c_str());
    oledDisplay->setTextSize(1);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("ERROR:");
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 10);
    oledDisplay->println(gSettingsEditor.errorMessage);
    // Note: Don't call display() here - main render loop handles it via displayUpdate()
    return;
  }
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT: {
      
      // Show category list (header shows "Config", no need for title here)
      oledDisplay->setTextSize(1);
      
      if (moduleCount == 0) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("No modules found!");
      } else {
        // Calculate scrolling window (43px content / 10px per item = 4 visible items)
        const int lineHeight = 10;
        const int maxVisibleItems = OLED_CONTENT_HEIGHT / lineHeight;  // 43px / 10px = 4 items
        
        // Calculate scroll offset to keep selection visible
        static int scrollOffset = 0;
        if (gSettingsEditor.categoryIndex < scrollOffset) {
          scrollOffset = gSettingsEditor.categoryIndex;
        } else if (gSettingsEditor.categoryIndex >= scrollOffset + maxVisibleItems) {
          scrollOffset = gSettingsEditor.categoryIndex - maxVisibleItems + 1;
        }
        
        // Draw visible categories
        int y = OLED_CONTENT_START_Y;
        for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < (int)moduleCount; i++) {
          int itemIdx = scrollOffset + i;
          if (itemIdx == gSettingsEditor.categoryIndex) {
            oledDisplay->fillRect(0, y, 128, 10, DISPLAY_COLOR_WHITE);
            oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
          } else {
            oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          }
          
          oledDisplay->setCursor(2, y + 1);
          oledDisplay->println(modules[itemIdx]->name);
          y += lineHeight;
        }
        
        // Draw scroll indicators
        if (scrollOffset > 0) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
          oledDisplay->print("\x18");  // Up arrow
        }
        if (scrollOffset + maxVisibleItems < (int)moduleCount) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(120, OLED_CONTENT_START_Y + (maxVisibleItems - 1) * lineHeight);
          oledDisplay->print("\x19");  // Down arrow
        }
      }
      
      break;
    }
    
    case SETTINGS_ITEM_SELECT: {
      // Show settings list for current category
      // Header breadcrumb already shows "Set>moduleName"
      if (!gSettingsEditor.currentModule) break;
      
      oledDisplay->setTextSize(1);
      
      // Filter to only INT and BOOL settings that are visible
      int visibleCount = 0;
      int visibleIndex = -1;
      
      // Count visible items and find current visible index
      for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if ((entry->type == SETTING_INT || entry->type == SETTING_BOOL) && isSettingVisible(entry)) {
          if (i == (size_t)gSettingsEditor.itemIndex) {
            visibleIndex = visibleCount;
          }
          visibleCount++;
        }
      }
      
      // Calculate scrolling window - content starts below header
      const int itemStartY = OLED_CONTENT_START_Y;
      const int lineHeight = 10;
      const int maxVisibleItems = OLED_CONTENT_HEIGHT / lineHeight;  // 4 items
      
      // Calculate scroll offset to keep selected item visible
      static int itemScrollOffset = 0;
      if (visibleIndex < itemScrollOffset) {
        itemScrollOffset = visibleIndex;
      } else if (visibleIndex >= itemScrollOffset + maxVisibleItems) {
        itemScrollOffset = visibleIndex - maxVisibleItems + 1;
      }
      
      // Draw visible settings items
      int displayedCount = 0;
      int currentVisibleIdx = 0;
      
      for (size_t i = 0; i < gSettingsEditor.currentModule->count && displayedCount < maxVisibleItems; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        
        // Skip non-int/bool settings and hidden settings
        if (entry->type != SETTING_INT && entry->type != SETTING_BOOL) continue;
        if (!isSettingVisible(entry)) continue;
        
        // Skip items before scroll offset
        if (currentVisibleIdx < itemScrollOffset) {
          currentVisibleIdx++;
          continue;
        }
        
        int y = itemStartY + displayedCount * lineHeight;
        
        if ((int)i == gSettingsEditor.itemIndex) {
          oledDisplay->fillRect(0, y - 1, 128, 10, DISPLAY_COLOR_WHITE);
          oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
        } else {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        }
        
        oledDisplay->setCursor(2, y);
        // Show label and current value - truncate to prevent wrapping
        String label = entry->label ? entry->label : entry->jsonKey;
        int currentVal = getSettingCurrentValue(entry);
        
        // Truncate label if too long (max ~15 chars to leave room for value)
        if (label.length() > 15) {
          label = label.substring(0, 14) + "~";
        }
        
        // Use print instead of println to prevent wrapping
        oledDisplay->print(label);
        oledDisplay->print(":");
        oledDisplay->print(currentVal);
        
        displayedCount++;
        currentVisibleIdx++;
      }
      
      // Draw scroll indicators
      if (itemScrollOffset > 0) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, itemStartY);
        oledDisplay->print("\x18");  // Up arrow
      }
      if (itemScrollOffset + maxVisibleItems < visibleCount) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, itemStartY + (maxVisibleItems - 1) * lineHeight);
        oledDisplay->print("\x19");  // Down arrow
      }
      
      break;
    }
    
    case SETTINGS_VALUE_EDIT: {
      // Show value editor with slider
      // Header breadcrumb already shows "Set>moduleName"
      if (!gSettingsEditor.currentEntry) break;
      
      oledDisplay->setTextSize(1);
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      String label = gSettingsEditor.currentEntry->label ? gSettingsEditor.currentEntry->label : gSettingsEditor.currentEntry->jsonKey;
      oledDisplay->println(label);
      
      // Draw slider
      bool isBool = (gSettingsEditor.currentEntry->type == SETTING_BOOL);
      int minVal = gSettingsEditor.currentEntry->minVal;
      int maxVal = gSettingsEditor.currentEntry->maxVal;
      
      drawSettingsSlider(oledDisplay, OLED_CONTENT_START_Y + 14, minVal, maxVal, gSettingsEditor.editValue, isBool);
      
      // Show change indicator
      if (gSettingsEditor.hasChanges) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 10);
        oledDisplay->print("* Modified");
      }
      
      break;
    }
  }
  
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// ============================================================================
// Input Handling
// ============================================================================

bool handleSettingsEditorInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Registered mode input handlers receive RAW deltaX/deltaY values
  // We must apply deadzone check ourselves, just like file browser and other modes
  
  // Auto-repeat timing for navigation
  static unsigned long lastMoveTimeX = 0;
  static unsigned long lastMoveTimeY = 0;
  static bool wasDeflectedX = false;
  static bool wasDeflectedY = false;
  const unsigned long INITIAL_DELAY_MS = 200;
  const unsigned long REPEAT_DELAY_MS = 100;
  unsigned long now = millis();
  
  bool handled = false;
  
  // Y-axis navigation with auto-repeat
  bool deflectedY = abs(deltaY) > JOYSTICK_DEADZONE;
  if (!deflectedY) {
    wasDeflectedY = false;
    lastMoveTimeY = 0;
  } else {
    bool shouldMove = false;
    if (!wasDeflectedY) {
      shouldMove = true;
      wasDeflectedY = true;
      lastMoveTimeY = now;
    } else {
      unsigned long elapsed = now - lastMoveTimeY;
      unsigned long threshold = (elapsed > INITIAL_DELAY_MS) ? REPEAT_DELAY_MS : INITIAL_DELAY_MS;
      if (elapsed >= threshold) {
        shouldMove = true;
        lastMoveTimeY = now;
      }
    }
    
    if (shouldMove) {
      if (deltaY < 0) {
        settingsEditorUp();
      } else {
        settingsEditorDown();
      }
      handled = true;
    }
  }
  
  // X-axis for value adjustment in edit mode with auto-repeat
  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT) {
    bool deflectedX = abs(deltaX) > JOYSTICK_DEADZONE;
    if (!deflectedX) {
      wasDeflectedX = false;
      lastMoveTimeX = 0;
    } else {
      bool shouldMove = false;
      if (!wasDeflectedX) {
        shouldMove = true;
        wasDeflectedX = true;
        lastMoveTimeX = now;
      } else {
        unsigned long elapsed = now - lastMoveTimeX;
        unsigned long threshold = (elapsed > INITIAL_DELAY_MS) ? REPEAT_DELAY_MS : INITIAL_DELAY_MS;
        if (elapsed >= threshold) {
          shouldMove = true;
          lastMoveTimeX = now;
        }
      }
      
      if (shouldMove) {
        if (deltaX < 0) {
          // Decrease value
          if (gSettingsEditor.editValue > gSettingsEditor.currentEntry->minVal) {
            gSettingsEditor.editValue--;
            gSettingsEditor.hasChanges = true;
            handled = true;
          }
        } else {
          // Increase value
          if (gSettingsEditor.editValue < gSettingsEditor.currentEntry->maxVal) {
            gSettingsEditor.editValue++;
            gSettingsEditor.hasChanges = true;
            handled = true;
          }
        }
      }
    }
  }
  
  // Button actions
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    settingsEditorSelect();
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Handle back navigation
    if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT) {
      // At top level - let caller handle exit to menu
      return false;
    } else {
      // Navigate back within settings
      settingsEditorBack();
      handled = true;
    }
  }
  
  return handled;
}

// ============================================================================
// Navigation Functions
// ============================================================================

void settingsEditorUp() {
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      if (gSettingsEditor.categoryIndex > 0) {
        gSettingsEditor.categoryIndex--;
      } else {
        gSettingsEditor.categoryIndex = moduleCount - 1;  // Wrap to bottom
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      if (!gSettingsEditor.currentModule) break;
      
      // Find previous INT/BOOL visible setting
      for (int i = gSettingsEditor.itemIndex - 1; i >= 0; i--) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if ((entry->type == SETTING_INT || entry->type == SETTING_BOOL) && isSettingVisible(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      
      // Wrap to last INT/BOOL visible setting
      for (int i = gSettingsEditor.currentModule->count - 1; i >= 0; i--) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if ((entry->type == SETTING_INT || entry->type == SETTING_BOOL) && isSettingVisible(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      break;
      
    case SETTINGS_VALUE_EDIT:
      // No up/down in edit mode (use left/right for value)
      break;
  }
}

void settingsEditorDown() {
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      if (gSettingsEditor.categoryIndex < (int)moduleCount - 1) {
        gSettingsEditor.categoryIndex++;
      } else {
        gSettingsEditor.categoryIndex = 0;  // Wrap to top
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      if (!gSettingsEditor.currentModule) break;
      
      // Find next INT/BOOL visible setting
      for (size_t i = gSettingsEditor.itemIndex + 1; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if ((entry->type == SETTING_INT || entry->type == SETTING_BOOL) && isSettingVisible(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      
      // Wrap to first INT/BOOL visible setting
      for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if ((entry->type == SETTING_INT || entry->type == SETTING_BOOL) && isSettingVisible(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      break;
      
    case SETTINGS_VALUE_EDIT:
      // No up/down in edit mode (use left/right for value)
      break;
  }
}

void settingsEditorSelect() {
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      // Enter selected category
      if (gSettingsEditor.categoryIndex < (int)moduleCount) {
        gSettingsEditor.currentModule = modules[gSettingsEditor.categoryIndex];
        gSettingsEditor.itemIndex = 0;
        
        // Find first INT/BOOL setting
        for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
          const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
          if (entry->type == SETTING_INT || entry->type == SETTING_BOOL) {
            gSettingsEditor.itemIndex = i;
            break;
          }
        }
        
        gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      // Enter value editor for selected setting
      if (!gSettingsEditor.currentModule) break;
      if (gSettingsEditor.itemIndex >= (int)gSettingsEditor.currentModule->count) break;
      
      gSettingsEditor.currentEntry = &gSettingsEditor.currentModule->entries[gSettingsEditor.itemIndex];
      
      // Only allow INT and BOOL editing
      if (gSettingsEditor.currentEntry->type != SETTING_INT && 
          gSettingsEditor.currentEntry->type != SETTING_BOOL) {
        gSettingsEditor.errorMessage = "Only int/bool editable";
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
        break;
      }
      
      gSettingsEditor.editValue = getSettingCurrentValue(gSettingsEditor.currentEntry);
      gSettingsEditor.hasChanges = false;
      gSettingsEditor.state = SETTINGS_VALUE_EDIT;
      break;
      
    case SETTINGS_VALUE_EDIT:
      // Save value
      if (!gSettingsEditor.currentEntry) break;
      
      // Validate
      String errorMsg;
      if (!validateSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue, errorMsg)) {
        gSettingsEditor.errorMessage = errorMsg;
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
        break;
      }
      
      // Apply and persist — setSetting() handles the flash write automatically
      setSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue);

      DEBUG_SYSTEMF("[SettingsEditor] Saved %s = %d",
                    gSettingsEditor.currentEntry->jsonKey, gSettingsEditor.editValue);
      
      // Return to item select
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.hasChanges = false;
      break;
  }
}

// Open settings editor directly to a specific module by name
// Returns true if module was found and editor was opened
bool openSettingsEditorForModule(const char* moduleName) {
  if (!moduleName) return false;
  
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  for (size_t i = 0; i < moduleCount; i++) {
    if (modules[i] && strcmp(modules[i]->name, moduleName) == 0) {
      // Found the module - set up editor to start there
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.categoryIndex = i;
      gSettingsEditor.currentModule = modules[i];
      gSettingsEditor.itemIndex = 0;
      gSettingsEditor.editValue = 0;
      gSettingsEditor.hasChanges = false;
      gSettingsEditor.currentEntry = nullptr;
      gSettingsEditor.errorMessage = "";
      gSettingsEditor.errorDisplayUntil = 0;
      
      DEBUG_SYSTEMF("[SettingsEditor] Opened module: %s", moduleName);
      return true;
    }
  }
  
  DEBUG_SYSTEMF("[SettingsEditor] Module not found: %s", moduleName);
  return false;
}

void settingsEditorBack() {
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      // Exit settings editor (handled by caller)
      break;
      
    case SETTINGS_ITEM_SELECT:
      // Return to category select
      gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
      gSettingsEditor.currentModule = nullptr;
      break;
      
    case SETTINGS_VALUE_EDIT:
      // Cancel edit and return to item select
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.hasChanges = false;
      break;
  }
}

// ============================================================================
// Settings Mode Registration (merged from oled_settings_mode.cpp)
// ============================================================================

// Forward declaration for registerOLEDModes
void registerOLEDModes(const OLEDModeEntry* modes, size_t count);

// Force linker to include this file - must be called from oled_display.cpp
void forceSettingsModeLink() {
  DEBUG_SYSTEMF("[SettingsMode] forceSettingsModeLink() called - file is linked");
}

// Display handler for settings mode
void displaySettingsMode() {
  DEBUG_SYSTEMF("[SettingsMode] displaySettingsMode called!!!");
  
  // Initialize on first entry if needed
  static bool initialized = false;
  if (!initialized) {
    DEBUG_SYSTEMF("[SettingsMode] Initializing settings editor");
    initSettingsEditor();
    initialized = true;
  }
  
  DEBUG_SYSTEMF("[SettingsMode] Calling displaySettingsEditor");
  displaySettingsEditor();
  DEBUG_SYSTEMF("[SettingsMode] displaySettingsEditor returned");
}

// Input handler for settings mode
bool handleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Check if we should exit back to menu
  if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT && 
      INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Let default handler take us back to menu
    return false;
  }
  
  // Otherwise, let settings editor handle input
  return handleSettingsEditorInput(deltaX, deltaY, newlyPressed);
}

// Availability check - always available
bool isSettingsAvailable(String* outReason) {
  return true;
}

// Settings mode entry
static const OLEDModeEntry settingsModeEntry = {
  OLED_SETTINGS,
  "Settings",
  "settings",  // Icon name
  displaySettingsMode,
  isSettingsAvailable,
  handleSettingsInput,
  true,   // Show in menu
  100     // Menu order (near end)
};

// Settings modes array
static const OLEDModeEntry settingsOLEDModes[] = {
  settingsModeEntry
};

// Auto-register settings mode
REGISTER_OLED_MODE_MODULE(settingsOLEDModes, sizeof(settingsOLEDModes) / sizeof(settingsOLEDModes[0]), "Settings");

// ============================================================================
// Quick Settings Mode (merged from oled_quick_settings.cpp)
// ============================================================================

#if ENABLE_WIFI
#include <WiFi.h>
#endif
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
extern httpd_handle_t server;
#endif

// Forward declaration for command execution
extern void runUnifiedSystemCommand(const String& cmd);

// ============================================================================
// Quick Settings - Dynamic item registry based on compile flags
// ============================================================================

typedef bool (*QuickGetStateFunc)();
typedef void (*QuickToggleFunc)();

struct QuickSettingsItem {
  const char* name;
  QuickGetStateFunc getState;
  QuickToggleFunc toggle;
};

#define MAX_QUICK_ITEMS 8
static QuickSettingsItem quickItems[MAX_QUICK_ITEMS];
static int quickItemCount = 0;
static bool quickItemsInitialized = false;

static int quickSelectedItem = 0;

static char quickStatusMsg[32] = "";
static unsigned long quickStatusExpireMs = 0;

static void setQuickStatus(const char* msg, unsigned long durationMs = 2000) {
  strncpy(quickStatusMsg, msg, 31);
  quickStatusMsg[31] = '\0';
  quickStatusExpireMs = millis() + durationMs;
}

static void addQuickItem(const char* name, QuickGetStateFunc getState, QuickToggleFunc toggle) {
  if (quickItemCount < MAX_QUICK_ITEMS) {
    quickItems[quickItemCount] = { name, getState, toggle };
    quickItemCount++;
  }
}

// --- WiFi ---
#if ENABLE_WIFI
static bool getQuickWiFiState() {
  return (WiFi.getMode() != WIFI_MODE_NULL);
}
static void toggleQuickWiFi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    setQuickStatus("WiFi OFF");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  } else {
    setQuickStatus("WiFi ON");
    WiFi.mode(WIFI_STA);
  }
}
#endif

// --- Bluetooth ---
#if ENABLE_BLUETOOTH
static bool getQuickBluetoothState() {
  extern bool isBLERunning();
  return isBLERunning();
}
static void bluetoothToggleConfirmedQuick(void* userData) {
  (void)userData;
  extern bool isBLERunning();
  if (isBLERunning()) {
    setQuickStatus("Bluetooth OFF");
    runUnifiedSystemCommand("closeble");
  } else {
    setQuickStatus("Bluetooth ON");
    runUnifiedSystemCommand("openble");
  }
}
static void toggleQuickBluetooth() {
  extern bool isBLERunning();
  if (isBLERunning()) {
    oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr, false);
  } else {
    oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr);
  }
}
#endif

// --- HTTP Server ---
#if ENABLE_HTTP_SERVER
static bool getQuickHTTPState() {
  return (server != nullptr);
}
static void httpToggleConfirmedQuick(void* userData) {
  (void)userData;
  if (server != nullptr) {
    setQuickStatus("HTTP OFF");
    runUnifiedSystemCommand("closehttp");
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    setQuickStatus("HTTP ON");
    runUnifiedSystemCommand("openhttp");
  }
}
static void toggleQuickHTTP() {
  if (server != nullptr) {
    oledConfirmRequest("Stop HTTP?", nullptr, httpToggleConfirmedQuick, nullptr, false);
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    oledConfirmRequest("Start HTTP?", nullptr, httpToggleConfirmedQuick, nullptr);
  }
}
#endif

static void initQuickItems() {
  if (quickItemsInitialized) return;
  quickItemsInitialized = true;
  quickItemCount = 0;
#if ENABLE_WIFI
  addQuickItem("WiFi", getQuickWiFiState, toggleQuickWiFi);
#endif
#if ENABLE_BLUETOOTH
  addQuickItem("Bluetooth", getQuickBluetoothState, toggleQuickBluetooth);
#endif
#if ENABLE_HTTP_SERVER
  addQuickItem("HTTP Server", getQuickHTTPState, toggleQuickHTTP);
#endif
}

// Display function
void displayQuickSettings() {
  if (!oledDisplay || !oledConnected) return;
  initQuickItems();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Title is shown in header, draw separator line only
  oledDisplay->drawLine(0, OLED_CONTENT_START_Y, 128, OLED_CONTENT_START_Y, DISPLAY_COLOR_WHITE);
  
  if (quickItemCount == 0) {
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 14);
    oledDisplay->print("No toggles available");
  } else {
    int yPos = OLED_CONTENT_START_Y + 6;
    for (int i = 0; i < quickItemCount; i++) {
      bool isSelected = (i == quickSelectedItem);
      bool isEnabled = quickItems[i].getState ? quickItems[i].getState() : false;
      
      if (isSelected) {
        oledDisplay->fillRect(0, yPos - 2, 128, 12, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      oledDisplay->setCursor(4, yPos);
      oledDisplay->print(quickItems[i].name);
      
      oledDisplay->setCursor(90, yPos);
      oledDisplay->print(isEnabled ? "[ON]" : "[OFF]");
      
      yPos += 14;
    }
  }
  
  if (quickStatusMsg[0] != '\0' && millis() < quickStatusExpireMs) {
    // Draw status toast at bottom of content area (above footer)
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 10);
    oledDisplay->print(quickStatusMsg);
  } else if (quickStatusMsg[0] != '\0') {
    quickStatusMsg[0] = '\0';
  }
}

// Input handler
bool quickSettingsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  initQuickItems();
  if (quickItemCount == 0) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      extern OLEDMode popOLEDModeStack();
      setOLEDMode(popOLEDModeStack());
      return true;
    }
    return false;
  }
  
  bool handled = false;
  
  if (gNavEvents.up) {
    quickSelectedItem = (quickSelectedItem - 1 + quickItemCount) % quickItemCount;
    handled = true;
  } else if (gNavEvents.down) {
    quickSelectedItem = (quickSelectedItem + 1) % quickItemCount;
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (quickSelectedItem >= 0 && quickSelectedItem < quickItemCount && quickItems[quickSelectedItem].toggle) {
      quickItems[quickSelectedItem].toggle();
    }
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    extern OLEDMode popOLEDModeStack();
    setOLEDMode(popOLEDModeStack());
    handled = true;
  }
  
  return handled;
}

// Note: Quick settings mode is registered directly in oled_display.cpp
// to ensure it's always linked and available (accessed via SELECT button)

#endif // ENABLE_OLED_DISPLAY
