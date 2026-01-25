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

extern Adafruit_SSD1306* oledDisplay;
extern bool writeSettingsJson();

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
      *((int*)entry->valuePtr) = value;
      break;
    case SETTING_BOOL:
      *((bool*)entry->valuePtr) = (value != 0);
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
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("ERROR:");
    oledDisplay->setCursor(0, 10);
    oledDisplay->println(gSettingsEditor.errorMessage);
    // Note: Don't call display() here - main render loop handles it via displayUpdate()
    return;
  }
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT: {
      
      // Show category list
      oledDisplay->setTextSize(1);
      oledDisplay->setCursor(0, 0);
      oledDisplay->println("Settings Categories:");
      
      if (moduleCount == 0) {
        oledDisplay->setCursor(0, 12);
        oledDisplay->println("No modules found!");
      } else {
        // Show up to 4 categories
        int startIdx = max(0, gSettingsEditor.categoryIndex - 3);
        int endIdx = min((int)moduleCount, startIdx + 4);
        
        
        int y = 12;
        for (int i = startIdx; i < endIdx; i++) {
          if (i == gSettingsEditor.categoryIndex) {
            oledDisplay->fillRect(0, y - 1, 128, 10, DISPLAY_COLOR_WHITE);
            oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
          } else {
            oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          }
          
          oledDisplay->setCursor(2, y);
          oledDisplay->println(modules[i]->name);
          y += 10;
        }
      }
      
      break;
    }
    
    case SETTINGS_ITEM_SELECT: {
      // Show settings list for current category
      if (!gSettingsEditor.currentModule) break;
      
      oledDisplay->setTextSize(1);
      oledDisplay->setCursor(0, 0);
      oledDisplay->print(gSettingsEditor.currentModule->name);
      oledDisplay->println(" Settings:");
      
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
      
      // Calculate scroll offset to keep selected item visible
      // Content area: y=12 to y=53 (42px available)
      // Each line: 10px, so max 4 lines, but we show 3 to prevent wrapping into footer
      const int maxVisibleItems = 3;
      int scrollOffset = 0;
      if (visibleIndex >= maxVisibleItems) {
        scrollOffset = visibleIndex - maxVisibleItems + 1;
      }
      
      // Show up to 3 settings with scrolling
      int displayedCount = 0;
      int y = 12;
      int currentVisibleIdx = 0;
      
      for (size_t i = 0; i < gSettingsEditor.currentModule->count && displayedCount < maxVisibleItems; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        
        // Skip non-int/bool settings and hidden settings
        if (entry->type != SETTING_INT && entry->type != SETTING_BOOL) continue;
        if (!isSettingVisible(entry)) continue;
        
        // Skip items before scroll offset
        if (currentVisibleIdx < scrollOffset) {
          currentVisibleIdx++;
          continue;
        }
        
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
        
        y += 10;
        displayedCount++;
        currentVisibleIdx++;
      }
      
      // Show scroll indicators if needed
      if (scrollOffset > 0) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, 12);
        oledDisplay->print("^");
      }
      if (scrollOffset + maxVisibleItems < visibleCount) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, 32);
        oledDisplay->print("v");
      }
      
      break;
    }
    
    case SETTINGS_VALUE_EDIT: {
      // Show value editor with slider
      if (!gSettingsEditor.currentEntry) break;
      
      oledDisplay->setTextSize(1);
      oledDisplay->setCursor(0, 0);
      String label = gSettingsEditor.currentEntry->label ? gSettingsEditor.currentEntry->label : gSettingsEditor.currentEntry->jsonKey;
      oledDisplay->println(label);
      
      // Draw slider
      bool isBool = (gSettingsEditor.currentEntry->type == SETTING_BOOL);
      int minVal = gSettingsEditor.currentEntry->minVal;
      int maxVal = gSettingsEditor.currentEntry->maxVal;
      
      drawSettingsSlider(oledDisplay, 25, minVal, maxVal, gSettingsEditor.editValue, isBool);
      
      // Show change indicator
      if (gSettingsEditor.hasChanges) {
        oledDisplay->setCursor(0, 40);
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
      
      // Apply value
      setSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue);
      
      // Persist to JSON
      if (!writeSettingsJson()) {
        gSettingsEditor.errorMessage = "Failed to save";
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
        break;
      }
      
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

#include <WiFi.h>
#include <esp_http_server.h>

// Forward declaration for command execution
extern void runUnifiedSystemCommand(const String& cmd);

#if ENABLE_WIFI
extern bool ensureWiFiInitialized();

// Access to WiFi initialization state (defined in System_WiFi.cpp as static)
// We'll use a helper function instead to avoid exposing static variable
static bool isWiFiInitialized() {
  // Try to access WiFi state - if WiFi is not initialized, this will be safe
  // because we check the mode first
  return (WiFi.getMode() != WIFI_MODE_NULL);
}
#endif

static const char* TAG_QUICK = "OLED_QUICK_SETTINGS";

#if ENABLE_HTTP_SERVER
extern httpd_handle_t server;
#endif

// Quick settings state
static int quickSelectedItem = 0;
static const int QUICK_ITEM_COUNT = 3;  // WiFi, Bluetooth, HTTP

// Status message for quick settings feedback
static char quickStatusMsg[32] = "";
static unsigned long quickStatusExpireMs = 0;

static void setQuickStatus(const char* msg, unsigned long durationMs = 2000) {
  strncpy(quickStatusMsg, msg, 31);
  quickStatusMsg[31] = '\0';
  quickStatusExpireMs = millis() + durationMs;
}

// Item names
static const char* quickItemNames[] = {
  "WiFi",
  "Bluetooth",
  "HTTP Server"
};

// Get current state of each toggle
static bool getQuickWiFiState() {
#if ENABLE_WIFI
  // Check if WiFi radio is enabled (not just connected)
  return (WiFi.getMode() != WIFI_MODE_NULL);
#else
  return false;
#endif
}

static bool getQuickBluetoothState() {
#if ENABLE_BLUETOOTH
  extern bool isBLERunning();
  return isBLERunning();
#else
  return false;
#endif
}

static bool getQuickHTTPState() {
#if ENABLE_HTTP_SERVER
  return (server != nullptr);
#else
  return false;
#endif
}

// Toggle functions
static void toggleQuickWiFi() {
#if ENABLE_WIFI
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    // WiFi is ON - turn it OFF
    setQuickStatus("WiFi OFF");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  } else {
    // WiFi is OFF - turn it ON
    setQuickStatus("WiFi ON");
    WiFi.mode(WIFI_STA);
  }
#else
  setQuickStatus("WiFi disabled");
#endif
}

// Confirmation callbacks for Bluetooth and HTTP toggles
static void bluetoothToggleConfirmedQuick(void* userData) {
  (void)userData;
#if ENABLE_BLUETOOTH
  extern bool isBLERunning();
  if (isBLERunning()) {
    setQuickStatus("Bluetooth OFF");
    runUnifiedSystemCommand("blestop");
  } else {
    setQuickStatus("Bluetooth ON");
    runUnifiedSystemCommand("blestart");
  }
#endif
}

static void httpToggleConfirmedQuick(void* userData) {
  (void)userData;
#if ENABLE_HTTP_SERVER
  if (server != nullptr) {
    setQuickStatus("HTTP OFF");
    runUnifiedSystemCommand("httpstop");
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    setQuickStatus("HTTP ON");
    runUnifiedSystemCommand("httpstart");
  }
#endif
}

static void toggleQuickBluetooth() {
#if ENABLE_BLUETOOTH
  extern bool isBLERunning();
  if (isBLERunning()) {
    oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr, false);
  } else {
    oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr);
  }
#else
  setQuickStatus("BT disabled");
#endif
}

static void toggleQuickHTTP() {
#if ENABLE_HTTP_SERVER
  if (server != nullptr) {
    oledConfirmRequest("Stop HTTP?", nullptr, httpToggleConfirmedQuick, nullptr, false);
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    oledConfirmRequest("Start HTTP?", nullptr, httpToggleConfirmedQuick, nullptr);
  }
#else
  setQuickStatus("HTTP disabled");
#endif
}

// Display function
void displayQuickSettings() {
  if (!oledDisplay || !oledConnected) {
    return;
  }
  
  // Note: Main loop already cleared the display, don't clear again
  
  // Title
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("Quick Settings");
  oledDisplay->drawLine(0, 10, 128, 10, DISPLAY_COLOR_WHITE);
  
  // Menu items
  int yPos = 16;
  for (int i = 0; i < QUICK_ITEM_COUNT; i++) {
    bool isSelected = (i == quickSelectedItem);
    bool isEnabled = false;
    
    // Get state
    switch (i) {
      case 0: isEnabled = getQuickWiFiState(); break;
      case 1: isEnabled = getQuickBluetoothState(); break;
      case 2: isEnabled = getQuickHTTPState(); break;
    }
    
    // Draw selection indicator
    if (isSelected) {
      oledDisplay->fillRect(0, yPos - 2, 128, 12, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Draw item name
    oledDisplay->setCursor(4, yPos);
    oledDisplay->print(quickItemNames[i]);
    
    // Draw state indicator
    oledDisplay->setCursor(90, yPos);
    oledDisplay->print(isEnabled ? "[ON]" : "[OFF]");
    
    yPos += 14;
  }
  
  // Show status message if active
  if (quickStatusMsg[0] != '\0' && millis() < quickStatusExpireMs) {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 56);
    oledDisplay->print(quickStatusMsg);
  } else if (quickStatusMsg[0] != '\0') {
    // Clear expired message
    quickStatusMsg[0] = '\0';
  }
}

// Input handler
bool quickSettingsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  bool handled = false;
  
  // Use centralized navigation events (computed with proper debounce/auto-repeat)
  if (gNavEvents.up) {
    quickSelectedItem = (quickSelectedItem - 1 + QUICK_ITEM_COUNT) % QUICK_ITEM_COUNT;
    handled = true;
  } else if (gNavEvents.down) {
    quickSelectedItem = (quickSelectedItem + 1) % QUICK_ITEM_COUNT;
    handled = true;
  }
  
  // A button - toggle selected item
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    switch (quickSelectedItem) {
      case 0: toggleQuickWiFi(); break;
      case 1: toggleQuickBluetooth(); break;
      case 2: toggleQuickHTTP(); break;
    }
    handled = true;
  }
  
  // B button - back to previous mode
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    extern OLEDMode popOLEDModeStack();
    currentOLEDMode = popOLEDModeStack();
    handled = true;
  }
  
  return handled;
}

// Note: Quick settings mode is registered directly in oled_display.cpp
// to ensure it's always linked and available (accessed via SELECT button)

#endif // ENABLE_OLED_DISPLAY
