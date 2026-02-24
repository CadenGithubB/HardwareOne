/**
 * OLED Setup Wizard Implementation
 * 
 * OLED-specific rendering for setup wizard.
 * Core logic is in System_SetupWizard.cpp
 */

#include "OLED_SetupWizard.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_FeatureRegistry.h"
#include "System_Settings.h"
#include "System_SetupWizard.h"
#include "HAL_Input.h"
#include "i2csensor-seesaw.h"
#include "System_I2C.h"

// I2C address for OLED (must match OLED_Display.cpp)
#ifndef OLED_I2C_ADDRESS
#define OLED_I2C_ADDRESS 0x3D
#endif

// Helper macro to wrap OLED operations in I2C transaction
#define OLED_TRANSACTION(code) \
  i2cDeviceTransactionVoid(OLED_I2C_ADDRESS, 400000, 500, [&]() { code; })

// External references
extern bool oledConnected;
extern Settings gSettings;
extern ControlCache gControlCache;

// OLED text input
extern String getOLEDTextInput(const char* prompt, bool masked, const char* defaultValue, int maxLen, bool* wasCancelled = nullptr);
extern bool getOLEDWiFiSelection(String& selectedSSID);

// Joystick navigation state
static bool sJoyUpHeld = false;
static bool sJoyDownHeld = false;
static bool sJoyLeftHeld = false;
static bool sJoyRightHeld = false;

// ============================================================================
// Drawing Functions
// ============================================================================

void drawWizardHeader(int pageNum, int totalPages, const char* title) {
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, 0);
  
  // Line 1: Full page title
  char header[22];
  snprintf(header, sizeof(header), "SETUP %d/%d: %s", pageNum, totalPages, title);
  oledDisplay->print(header);
  
  // Line 2: Heap bar using shared utility
  uint32_t enabledKB = 0, maxKB = 1;
  int percentage = 0;
  getHeapBarData(&enabledKB, &maxKB, &percentage);
  char heapLabel[12];
  snprintf(heapLabel, sizeof(heapLabel), "%lu/%luKB", (unsigned long)enabledKB, (unsigned long)maxKB);
  oledDrawBar(oledDisplay, 0, 10, 70, 6, percentage, 100, heapLabel);
  
  // Separator below heap bar
  oledDisplay->drawFastHLine(0, 18, 128, SSD1306_WHITE);
}


void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction) {
  // Use correct footer position: header + content area
  const int footerStartY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
  
  // Draw separator line
  oledDisplay->drawFastHLine(0, footerStartY, 128, SSD1306_WHITE);
  
  // Draw footer text
  oledDisplay->setCursor(0, footerStartY + 2);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  
  // Compact format to fit 21 chars: "A:Tog >:Nxt B:Bk"
  char footer[22];
  if (backAction) {
    snprintf(footer, sizeof(footer), "A:%.3s >:%.3s B:%.2s", leftAction, rightAction, backAction);
  } else {
    snprintf(footer, sizeof(footer), "A:%.4s >:%.4s", leftAction, rightAction);
  }
  oledDisplay->print(footer);
}


// ============================================================================
// Page Renderers
// ============================================================================

// Shared renderer for feature/sensor toggle lists
static void renderToggleList(int pageNum, const char* title,
                             WizardFeatureItem* items, size_t itemCount,
                             const char* footerLeft, const char* footerRight,
                             const char* footerBack) {
  oledDisplay->clearDisplay();
  drawWizardHeader(pageNum, 5, title);
  
  // Content area: y=20 to y=52 (below 2-line header)
  const int startY = 20;
  const int lineHeight = 10;
  const int maxVisible = 3;
  
  int scrollOffset = getWizardScrollOffset();
  int currentSelection = getWizardCurrentSelection();
  
  // Adjust scroll to keep selection visible
  if (currentSelection < scrollOffset) {
    setWizardScrollOffset(currentSelection);
    scrollOffset = currentSelection;
  } else if (currentSelection >= scrollOffset + maxVisible) {
    setWizardScrollOffset(currentSelection - maxVisible + 1);
    scrollOffset = currentSelection - maxVisible + 1;
  }
  
  for (int i = 0; i < maxVisible && (scrollOffset + i) < (int)itemCount; i++) {
    int idx = scrollOffset + i;
    WizardFeatureItem* item = &items[idx];
    
    int y = startY + i * lineHeight;
    bool isSelected = (idx == currentSelection);
    
    // Highlight bar for selected item (like main menu)
    if (isSelected) {
      oledDisplay->fillRect(0, y - 1, 126, lineHeight, SSD1306_WHITE);
      oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      oledDisplay->setTextColor(SSD1306_WHITE);
    }
    
    oledDisplay->setCursor(1, y);
    
    // Checkbox + label + heap cost
    bool enabled = item->setting ? *item->setting : false;
    char line[22];
    const char* essential = item->essential ? "*" : "";
    snprintf(line, sizeof(line), "%s %.10s%s %dK",
             enabled ? "[X]" : "[ ]", item->label, essential, item->heapKB);
    oledDisplay->print(line);
    
    oledDisplay->setTextColor(SSD1306_WHITE);
  }
  
  // Scroll indicators
  if (itemCount > (size_t)maxVisible) {
    if (scrollOffset > 0) {
      oledDisplay->setCursor(121, startY);
      oledDisplay->print("^");
    }
    if (scrollOffset + maxVisible < (int)itemCount) {
      oledDisplay->setCursor(121, startY + (maxVisible - 1) * lineHeight);
      oledDisplay->print("v");
    }
  }
  
  drawWizardFooter(footerLeft, footerRight, footerBack);
  OLED_TRANSACTION(oledDisplay->display());
}

static void renderFeaturesPage() {
  renderToggleList(1, "Features",
                   getWizardFeaturesPage(), getWizardFeaturesPageCount(),
                   "Toggle", "Next", nullptr);
}

static void renderSensorsPage() {
  renderToggleList(2, "Sensors",
                   getWizardSensorsPage(), getWizardSensorsPageCount(),
                   "Toggle", "Next", "Back");
}

static void renderNetworkPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(3, 5, "Network");
  
  const int startY = 20;
  const int lineHeight = 10;
  const int maxVisible = 3;
  
  int currentSelection = getWizardCurrentSelection();
  size_t networkPageCount = getWizardNetworkPageCount();
  WizardNetworkItem* networkPage = getWizardNetworkPage();
  
  for (size_t i = 0; i < networkPageCount && i < (size_t)maxVisible; i++) {
    int y = startY + i * lineHeight;
    bool isSelected = ((int)i == currentSelection);
    
    // Highlight bar for selected item
    if (isSelected) {
      oledDisplay->fillRect(0, y - 1, 126, lineHeight, SSD1306_WHITE);
      oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      oledDisplay->setTextColor(SSD1306_WHITE);
    }
    
    oledDisplay->setCursor(1, y);
    
    // Label + value
    if (networkPage[i].isBool) {
      bool enabled = *networkPage[i].boolSetting;
      char line[22];
      snprintf(line, sizeof(line), "%s %s", enabled ? "[ON] " : "[OFF]", networkPage[i].label);
      oledDisplay->print(line);
    } else {
      oledDisplay->print(networkPage[i].label);
    }
    
    oledDisplay->setTextColor(SSD1306_WHITE);
  }
  
  drawWizardFooter("Toggle", "Next", "Back");
  OLED_TRANSACTION(oledDisplay->display());
}

static void renderSystemPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(4, 5, "System");
  
  const int startY = 20;
  const int lineHeight = 10;
  
  int currentSelection = getWizardCurrentSelection();
  int timezoneSelection = getWizardTimezoneSelection();
  int logLevelSelection = getWizardLogLevelSelection();
  const TimezoneEntry* timezones = getTimezones();
  const char** logLevelNames = getLogLevelNames();
  
  // Time zone
  int y0 = startY;
  if (currentSelection == 0) {
    oledDisplay->fillRect(0, y0 - 1, 126, lineHeight, SSD1306_WHITE);
    oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    oledDisplay->setTextColor(SSD1306_WHITE);
  }
  oledDisplay->setCursor(1, y0);
  oledDisplay->print("Timezone: ");
  oledDisplay->print(timezones[timezoneSelection].abbrev);
  oledDisplay->setTextColor(SSD1306_WHITE);
  
  // Log level
  int y1 = startY + lineHeight;
  if (currentSelection == 1) {
    oledDisplay->fillRect(0, y1 - 1, 126, lineHeight, SSD1306_WHITE);
    oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    oledDisplay->setTextColor(SSD1306_WHITE);
  }
  oledDisplay->setCursor(1, y1);
  oledDisplay->print("Log level: ");
  oledDisplay->print(logLevelNames[logLevelSelection]);
  oledDisplay->setTextColor(SSD1306_WHITE);
  
  drawWizardFooter("Change", "Next", "Back");
  OLED_TRANSACTION(oledDisplay->display());
}

static bool renderWiFiPage(SetupWizardResult& result) {
  // Loop to allow going back from password entry to network selection
  // Returns true if WiFi setup completed (or skipped), false if user pressed B to go back
  while (true) {
    // This page uses existing WiFi selection UI
    oledDisplay->clearDisplay();
    drawWizardHeader(5, 5, "WiFi");
    
    oledDisplay->setCursor(0, 14);
    oledDisplay->println("Select network or");
    oledDisplay->println("press B to go back");
    
    drawWizardFooter("Select", "Done", "Back");
    OLED_TRANSACTION(oledDisplay->display());
    
    // Wait a moment then launch WiFi selector
    delay(500);
    
    String selectedSSID;
    if (getOLEDWiFiSelection(selectedSSID)) {
      result.wifiSSID = selectedSSID;
      
      // Get password - check if user cancels to go back
      bool passwordCancelled = false;
      result.wifiPassword = getOLEDTextInput("WiFi Password:", true, "", 64, &passwordCancelled);
      
      if (passwordCancelled) {
        // User pressed B during password entry - loop back to network selection
        continue;
      }
      
      // Password entered (may be empty for open networks)
      result.wifiConfigured = true;
      return true;
    } else {
      // getOLEDWiFiSelection returned false = user pressed B/cancelled at network list
      // Go back to system settings page
      wizardPrevPage();
      return false;
    }
  }
}

// ============================================================================
// Joystick Input Helper
// ============================================================================

// Read joystick and return navigation events
struct JoystickNav {
  bool up;
  bool down;
  bool left;
  bool right;
};

static JoystickNav readJoystickNav() {
  JoystickNav nav = {false, false, false, false};
  
  if (xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (gControlCache.gamepadDataValid) {
      int joyX = gControlCache.gamepadX;
      int joyY = gControlCache.gamepadY;
      
      int deltaX = joyX - JOYSTICK_CENTER;
      int deltaY = JOYSTICK_CENTER - joyY;  // Invert so up is positive
      
      // Check for new deflections (edge detection)
      bool deflectedUp = deltaY > JOYSTICK_DEADZONE;
      bool deflectedDown = deltaY < -JOYSTICK_DEADZONE;
      bool deflectedLeft = deltaX < -JOYSTICK_DEADZONE;
      bool deflectedRight = deltaX > JOYSTICK_DEADZONE;
      
      // Only trigger on new deflection
      if (deflectedUp && !sJoyUpHeld) nav.up = true;
      if (deflectedDown && !sJoyDownHeld) nav.down = true;
      if (deflectedLeft && !sJoyLeftHeld) nav.left = true;
      if (deflectedRight && !sJoyRightHeld) nav.right = true;
      
      sJoyUpHeld = deflectedUp;
      sJoyDownHeld = deflectedDown;
      sJoyLeftHeld = deflectedLeft;
      sJoyRightHeld = deflectedRight;
    }
    xSemaphoreGive(gControlCache.mutex);
  }
  
  return nav;
}

// ============================================================================
// Input Handling
// ============================================================================

static bool handleFeaturesInput(uint32_t buttons, JoystickNav& nav) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    // Toggle current item
    wizardToggleCurrentItem();
    return true;
  }
  
  // Joystick navigation (inverted for this wizard - physical up = visual up)
  if (nav.down) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.up) {
    wizardMoveDown();
    return true;
  }
  
  // Next page (joystick right or START)
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    setWizardCurrentPage(WIZARD_PAGE_SENSORS);
    setWizardCurrentSelection(0);
    setWizardScrollOffset(0);
    return true;
  }
  
  return false;
}

static bool handleSensorsInput(uint32_t buttons, JoystickNav& nav) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    wizardToggleCurrentItem();
    return true;
  }
  
  if (nav.down) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.up) {
    wizardMoveDown();
    return true;
  }
  
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    rebuildNetworkSettingsPage();  // Build network items before showing page
    setWizardCurrentPage(WIZARD_PAGE_NETWORK);
    setWizardCurrentSelection(0);
    setWizardScrollOffset(0);
    return true;
  }
  
  if ((buttons & INPUT_MASK(INPUT_BUTTON_B)) || nav.left) {
    setWizardCurrentPage(WIZARD_PAGE_FEATURES);
    setWizardCurrentSelection(0);
    setWizardScrollOffset(0);
    return true;
  }
  
  return false;
}

static bool handleNetworkInput(uint32_t buttons, JoystickNav& nav) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    wizardToggleCurrentItem();
    return true;
  }
  
  if (nav.down) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.up) {
    wizardMoveDown();
    return true;
  }
  
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
    setWizardCurrentSelection(0);
    return true;
  }
  
  if ((buttons & INPUT_MASK(INPUT_BUTTON_B)) || nav.left) {
    setWizardCurrentPage(WIZARD_PAGE_SENSORS);
    setWizardCurrentSelection(0);
    setWizardScrollOffset(0);
    return true;
  }
  
  return false;
}

static bool handleSystemInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    // Cycle through options
    wizardCycleOption();
    return true;
  }
  
  if (nav.down) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.up) {
    wizardMoveDown();
    return true;
  }
  
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    // Save timezone and log level
    const TimezoneEntry* timezones = getTimezones();
    int tzSel = getWizardTimezoneSelection();
    int logSel = getWizardLogLevelSelection();
    
    result.timezoneOffset = timezones[tzSel].offsetMinutes;
    result.timezoneAbbrev = timezones[tzSel].abbrev;
    gSettings.tzOffsetMinutes = result.timezoneOffset;
    gSettings.logLevel = logSel;
    
    // Check if WiFi is enabled - if so, go to WiFi page
#if ENABLE_WIFI
    // Find WiFi feature and check if enabled
    const FeatureEntry* wifiFeature = getFeatureById("wifi");
    if (wifiFeature && isFeatureEnabled(wifiFeature)) {
      setWizardCurrentPage(WIZARD_PAGE_WIFI);
      result.wifiEnabled = true;
    } else {
      result.wifiEnabled = false;
      result.completed = true;
      return false;  // Signal completion
    }
#else
    result.wifiEnabled = false;
    result.completed = true;
    return false;  // Signal completion
#endif
    return true;
  }
  
  if ((buttons & INPUT_MASK(INPUT_BUTTON_B)) || nav.left) {
    wizardPrevPage();
    setWizardCurrentSelection(0);
    return true;
  }
  
  return false;
}

// ============================================================================
// Main Wizard Function
// ============================================================================

// Helper: Print current page to Serial for dual-interface support
static void printSerialPageStatus() {
  SetupWizardPage page = getWizardCurrentPage();
  int sel = getWizardCurrentSelection();
  
  Serial.println();
  Serial.println("========================================");
  
  switch (page) {
    case WIZARD_PAGE_FEATURES: {
      Serial.println("  SETUP 1/5: Features");
      WizardFeatureItem* items = getWizardFeaturesPage();
      size_t count = getWizardFeaturesPageCount();
      for (size_t i = 0; i < count; i++) {
        bool enabled = items[i].setting ? *items[i].setting : false;
        Serial.printf(" %s%zu. [%s] %s%s ~%dKB\n",
          (int)i == sel ? ">" : " ", i + 1,
          enabled ? "X" : " ",
          items[i].label,
          items[i].essential ? "*" : "",
          items[i].heapKB);
      }
      break;
    }
    case WIZARD_PAGE_SENSORS: {
      Serial.println("  SETUP 2/5: Sensors & Display");
      WizardFeatureItem* items = getWizardSensorsPage();
      size_t count = getWizardSensorsPageCount();
      for (size_t i = 0; i < count; i++) {
        bool enabled = items[i].setting ? *items[i].setting : false;
        Serial.printf(" %s%zu. [%s] %s%s ~%dKB\n",
          (int)i == sel ? ">" : " ", i + 1,
          enabled ? "X" : " ",
          items[i].label,
          items[i].essential ? "*" : "",
          items[i].heapKB);
      }
      break;
    }
    case WIZARD_PAGE_NETWORK: {
      Serial.println("  SETUP 3/5: Network Settings");
      WizardNetworkItem* items = getWizardNetworkPage();
      size_t count = getWizardNetworkPageCount();
      for (size_t i = 0; i < count; i++) {
        if (items[i].isBool) {
          bool enabled = *items[i].boolSetting;
          Serial.printf(" %s%zu. [%s] %s\n",
            (int)i == sel ? ">" : " ", i + 1,
            enabled ? "X" : " ",
            items[i].label);
        }
      }
      break;
    }
    case WIZARD_PAGE_SYSTEM: {
      Serial.println("  SETUP 4/5: System Settings");
      const TimezoneEntry* tz = getTimezones();
      int tzSel = getWizardTimezoneSelection();
      int logSel = getWizardLogLevelSelection();
      const char** logNames = getLogLevelNames();
      Serial.printf(" %s1. Timezone: %s\n", sel == 0 ? ">" : " ", tz[tzSel].abbrev);
      Serial.printf(" %s2. Log Level: %s\n", sel == 1 ? ">" : " ", logNames[logSel]);
      break;
    }
    default:
      break;
  }
  
  Serial.println("----------------------------------------");
  Serial.println("Serial: # to toggle, 'n' next, 'b' back");
  Serial.println("OLED: Joystick + A=Toggle, Right=Next");
  Serial.print("> ");
}

// ============================================================================
// Setup Mode Selection (Basic vs Advanced)
// ============================================================================

bool getOLEDSetupModeSelection(bool& advancedMode) {
  int selection = 0;  // 0 = Basic, 1 = Advanced
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  int lastPrintedSelection = -1;
  
  // Reset joystick state
  sJoyUpHeld = false;
  sJoyDownHeld = false;
  
  while (true) {
    // Print to Serial if selection changed (dual-interface support)
    if (selection != lastPrintedSelection) {
      Serial.println();
      Serial.println("========================================");
      Serial.println("       SELECT SETUP MODE");
      Serial.println("========================================");
      Serial.printf(" %s1. Basic Setup\n", selection == 0 ? ">" : " ");
      Serial.printf(" %s2. Advanced Setup\n", selection == 1 ? ">" : " ");
      Serial.println("----------------------------------------");
      Serial.println("Serial: Enter 1 or 2");
      Serial.println("OLED: Joystick to move, A to select");
      Serial.print("> ");
      lastPrintedSelection = selection;
    }
    
    // Draw selection screen on OLED (if available)
    if (oledDisplay && oledConnected) {
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(SSD1306_WHITE);
      
      // Title
      oledDisplay->setCursor(20, 0);
      oledDisplay->println("FIRST-TIME SETUP");
      oledDisplay->drawFastHLine(0, 10, 128, SSD1306_WHITE);
      
      // Subtitle
      oledDisplay->setCursor(0, 13);
      oledDisplay->println("Select setup mode:");
      
      // Option 1: Basic
      oledDisplay->setCursor(0, 25);
      oledDisplay->print(selection == 0 ? ">" : " ");
      oledDisplay->println(" Basic Setup");
      oledDisplay->setCursor(12, 33);
    
      
      // Option 2: Advanced
      oledDisplay->setCursor(0, 43);
      oledDisplay->print(selection == 1 ? ">" : " ");
      oledDisplay->println(" Advanced Setup");
      
      // Footer at standard position (matching drawOLEDFooter)
      const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
      oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
      oledDisplay->setCursor(0, footerY + 2);
      oledDisplay->print("A:Select  Joy:Move");
      
      OLED_TRANSACTION(oledDisplay->display());
    }
    
    delay(50);
    
    // Read joystick (if OLED/gamepad available)
    if (oledDisplay && oledConnected) {
      JoystickNav nav = readJoystickNav();
      
      if (nav.up || nav.down) {
        selection = (selection == 0) ? 1 : 0;
        delay(150);
        continue;
      }
      
      // Read buttons
      uint32_t buttons = lastButtons;
      bool haveButtons = false;
      if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (gControlCache.gamepadDataValid) {
          buttons = gControlCache.gamepadButtons;
          haveButtons = true;
        }
        xSemaphoreGive(gControlCache.mutex);
      }
      
      if (haveButtons && !lastButtonsInitialized) {
        lastButtons = buttons;
        lastButtonsInitialized = true;
        continue;
      }
      
      uint32_t pressedNow = ~buttons;
      uint32_t pressedLast = ~lastButtons;
      uint32_t newButtons = pressedNow & ~pressedLast;
      lastButtons = buttons;
      
      // A button = select
      if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) {
        advancedMode = (selection == 1);
        return true;
      }
    }
    
    // Also check serial input
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input == "1" || input.equalsIgnoreCase("basic")) {
        advancedMode = false;
        return true;
      } else if (input == "2" || input.equalsIgnoreCase("advanced")) {
        advancedMode = true;
        return true;
      }
    }
  }
  
  return false;
}

// ============================================================================
// Theme Selection (Light vs Dark)
// ============================================================================

bool getOLEDThemeSelection(bool& darkMode) {
  int selection = 0;  // 0 = Light, 1 = Dark
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  int lastPrintedSelection = -1;
  
  // Reset joystick state
  sJoyUpHeld = false;
  sJoyDownHeld = false;
  
  while (true) {
    // Print to Serial if selection changed (dual-interface support)
    if (selection != lastPrintedSelection) {
      Serial.println();
      Serial.printf(" %s1. Light (default)\n", selection == 0 ? ">" : " ");
      Serial.printf(" %s2. Dark\n", selection == 1 ? ">" : " ");
      Serial.print("> ");
      lastPrintedSelection = selection;
    }
    
    // Draw selection screen on OLED (if available)
    if (oledDisplay && oledConnected) {
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(SSD1306_WHITE);
      
      // Title
      oledDisplay->setCursor(24, 0);
      oledDisplay->println("WEB UI THEME");
      oledDisplay->drawFastHLine(0, 10, 128, SSD1306_WHITE);
      
      // Option 1: Light
      oledDisplay->setCursor(0, 24);
      oledDisplay->print(selection == 0 ? ">" : " ");
      oledDisplay->println(" Light (default)");
      
      // Option 2: Dark
      oledDisplay->setCursor(0, 36);
      oledDisplay->print(selection == 1 ? ">" : " ");
      oledDisplay->println(" Dark");
      
      // Footer at standard position
      const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
      oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
      oledDisplay->setCursor(0, footerY + 2);
      oledDisplay->print("A:Select  Joy:Move");
      
      OLED_TRANSACTION(oledDisplay->display());
    }
    
    delay(50);
    
    // Read joystick (if OLED/gamepad available)
    if (oledDisplay && oledConnected) {
      JoystickNav nav = readJoystickNav();
      
      if (nav.up || nav.down) {
        selection = (selection == 0) ? 1 : 0;
        delay(150);
        continue;
      }
      
      // Read buttons
      uint32_t buttons = lastButtons;
      bool haveButtons = false;
      if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (gControlCache.gamepadDataValid) {
          buttons = gControlCache.gamepadButtons;
          haveButtons = true;
        }
        xSemaphoreGive(gControlCache.mutex);
      }
      
      if (haveButtons && !lastButtonsInitialized) {
        lastButtons = buttons;
        lastButtonsInitialized = true;
        continue;
      }
      
      uint32_t pressedNow = ~buttons;
      uint32_t pressedLast = ~lastButtons;
      uint32_t newButtons = pressedNow & ~pressedLast;
      lastButtons = buttons;
      
      // A button = select
      if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) {
        darkMode = (selection == 1);
        return true;
      }
    }
    
    // Also check serial input
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input == "1" || input.equalsIgnoreCase("light")) {
        darkMode = false;
        return true;
      } else if (input == "2" || input.equalsIgnoreCase("dark")) {
        darkMode = true;
        return true;
      }
    }
  }
  
  return false;
}

SetupWizardResult runOLEDSetupWizard() {
  SetupWizardResult result;
  result.completed = false;
  result.wifiEnabled = false;
  result.wifiConfigured = false;
  result.wifiSSID = "";
  result.wifiPassword = "";
  result.deviceName = "HardwareOne";
  result.timezoneOffset = -300;  // EST default
  result.timezoneAbbrev = "EST";
  
  // Initialize wizard state
  initSetupWizard();
  
  // Find current timezone in list
  const TimezoneEntry* timezones = getTimezones();
  size_t tzCount = getTimezoneCount();
  for (size_t i = 0; i < tzCount; i++) {
    if (timezones[i].offsetMinutes == gSettings.tzOffsetMinutes) {
      setWizardTimezoneSelection(i);
      break;
    }
  }
  
  // Reset joystick state
  sJoyUpHeld = false;
  sJoyDownHeld = false;
  sJoyLeftHeld = false;
  sJoyRightHeld = false;
  
  // Track last page to know when to reprint serial status
  SetupWizardPage lastPrintedPage = WIZARD_PAGE_COUNT;
  int lastPrintedSel = -1;
  
  bool running = true;
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  
  while (running) {
    SetupWizardPage currentPage = getWizardCurrentPage();
    int currentSel = getWizardCurrentSelection();
    
    // Render current page to OLED (if available)
    if (oledDisplay && oledConnected) {
      switch (currentPage) {
        case WIZARD_PAGE_FEATURES:
          renderFeaturesPage();
          break;
        case WIZARD_PAGE_SENSORS:
          renderSensorsPage();
          break;
        case WIZARD_PAGE_NETWORK:
          renderNetworkPage();
          break;
        case WIZARD_PAGE_SYSTEM:
          renderSystemPage();
          break;
        case WIZARD_PAGE_WIFI:
          if (renderWiFiPage(result)) {
            result.completed = true;
            running = false;
          }
          break;
        default:
          running = false;
          break;
      }
    } else {
      // No OLED - WiFi page is handled differently
      if (currentPage == WIZARD_PAGE_WIFI) {
        // Serial-only WiFi setup
        Serial.println();
        Serial.println("=== WiFi Setup ===");
        Serial.println("Enter WiFi SSID (or press Enter to skip):");
        Serial.print("> ");
        while (!Serial.available()) { delay(10); }
        String ssid = Serial.readStringUntil('\n');
        ssid.trim();
        if (ssid.length() > 0) {
          Serial.println("Enter WiFi password:");
          Serial.print("> ");
          while (!Serial.available()) { delay(10); }
          String pass = Serial.readStringUntil('\n');
          pass.trim();
          result.wifiSSID = ssid;
          result.wifiPassword = pass;
          result.wifiConfigured = true;
        }
        result.completed = true;
        running = false;
        break;
      }
    }
    
    if (!running) break;
    
    // Print to Serial if page or selection changed
    if (currentPage != lastPrintedPage || currentSel != lastPrintedSel) {
      printSerialPageStatus();
      lastPrintedPage = currentPage;
      lastPrintedSel = currentSel;
    }
    
    // Read input with debounce
    delay(50);
    
    // Check for serial input first (non-blocking)
    bool serialHandled = false;
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      input.toLowerCase();
      
      if (input == "n" || input == "next") {
        // Navigate right (next page)
        JoystickNav fakeNav = {false, false, false, true};
        switch (currentPage) {
          case WIZARD_PAGE_FEATURES:
            handleFeaturesInput(0, fakeNav);
            break;
          case WIZARD_PAGE_SENSORS:
            handleSensorsInput(0, fakeNav);
            break;
          case WIZARD_PAGE_NETWORK:
            handleNetworkInput(0, fakeNav);
            break;
          case WIZARD_PAGE_SYSTEM:
            if (!handleSystemInput(0, fakeNav, result)) {
              running = false;
            }
            break;
          default:
            break;
        }
        serialHandled = true;
      } else if (input == "b" || input == "back") {
        // Navigate left (back)
        JoystickNav fakeNav = {false, false, true, false};
        switch (currentPage) {
          case WIZARD_PAGE_FEATURES:
            handleFeaturesInput(0, fakeNav);
            break;
          case WIZARD_PAGE_SENSORS:
            handleSensorsInput(0, fakeNav);
            break;
          case WIZARD_PAGE_NETWORK:
            handleNetworkInput(0, fakeNav);
            break;
          case WIZARD_PAGE_SYSTEM:
            handleSystemInput(0, fakeNav, result);
            break;
          default:
            break;
        }
        serialHandled = true;
      } else if (input.length() > 0) {
        int num = input.toInt();
        if (num > 0) {
          // Select item and toggle
          setWizardCurrentSelection(num - 1);
          if (currentPage == WIZARD_PAGE_SYSTEM) {
            wizardCycleOption();
          } else {
            wizardToggleCurrentItem();
          }
          serialHandled = true;
        }
      }
      
      if (serialHandled) {
        lastPrintedSel = -1;  // Force reprint
        continue;
      }
    }
    
    // Read gamepad buttons (seesaw is active-low: 0=pressed, 1=unpressed)
    uint32_t buttons = lastButtons;
    bool haveButtons = false;
    if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gControlCache.gamepadDataValid) {
        buttons = gControlCache.gamepadButtons;
        haveButtons = true;
      }
      xSemaphoreGive(gControlCache.mutex);
    }

    // Initialize baseline button state on first valid read to avoid false toggles
    // caused by starting lastButtons at 0 (which would look like a press for most bits).
    if (haveButtons && !lastButtonsInitialized) {
      lastButtons = buttons;
      lastButtonsInitialized = true;
      continue;
    }

    // Only act on new button presses (edge detect on active-high pressed bits)
    // For active-low hardware, a press is a 1->0 transition in the raw bit.
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtons;
    uint32_t newButtons = pressedNow & ~pressedLast;
    lastButtons = buttons;
    
    // Read joystick navigation
    JoystickNav nav = readJoystickNav();
    
    // Skip if no input
    bool hasInput = (newButtons != 0) || nav.up || nav.down || nav.left || nav.right;
    if (!hasInput) continue;
    
    // Handle input for current page
    bool handled = false;
    switch (currentPage) {
      case WIZARD_PAGE_FEATURES:
        handled = handleFeaturesInput(newButtons, nav);
        break;
      case WIZARD_PAGE_SENSORS:
        handled = handleSensorsInput(newButtons, nav);
        break;
      case WIZARD_PAGE_NETWORK:
        handled = handleNetworkInput(newButtons, nav);
        break;
      case WIZARD_PAGE_SYSTEM:
        if (!handleSystemInput(newButtons, nav, result)) {
          running = false;  // Wizard completed
        }
        break;
      default:
        break;
    }
    
    // Small delay for button debounce
    if (handled) {
      lastPrintedSel = -1;  // Force reprint after change
      delay(150);
    }
  }
  
  return result;
}

#endif // ENABLE_OLED_DISPLAY
