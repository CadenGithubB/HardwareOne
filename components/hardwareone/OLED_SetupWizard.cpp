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

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern Settings gSettings;
extern bool writeSettingsJson();
extern ControlCache gControlCache;

// OLED text input
extern String getOLEDTextInput(const char* prompt, bool masked, const char* defaultValue, int maxLen);
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
  
  // Title with page number
  char header[32];
  snprintf(header, sizeof(header), "SETUP %d/%d: %s", pageNum, totalPages, title);
  oledDisplay->println(header);
}

void drawHeapBar(int y) {
  uint32_t enabledKB = getEnabledFeaturesHeapEstimate();
  uint32_t maxKB = getTotalPossibleHeapCost();
  
  // Ensure we don't divide by zero
  if (maxKB == 0) maxKB = 1;
  
  // Draw bar background
  const int barX = 0;
  const int barWidth = 90;
  const int barHeight = 6;
  
  oledDisplay->drawRect(barX, y, barWidth, barHeight, SSD1306_WHITE);
  
  // Fill bar proportionally
  int fillWidth = (barWidth - 2) * enabledKB / maxKB;
  if (fillWidth > barWidth - 2) fillWidth = barWidth - 2;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, y + 1, fillWidth, barHeight - 2, SSD1306_WHITE);
  }
  
  // Draw text: XX/XXX KB
  char heapText[16];
  snprintf(heapText, sizeof(heapText), "%lu/%luKB", (unsigned long)enabledKB, (unsigned long)maxKB);
  oledDisplay->setCursor(barX + barWidth + 2, y);
  oledDisplay->print(heapText);
}

void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction) {
  oledDisplay->setCursor(0, 56);
  oledDisplay->setTextSize(1);
  
  // Format: "A:Action >:Next B:Back"
  char footer[48];
  if (backAction) {
    snprintf(footer, sizeof(footer), "A:%s >:%s B:%s", leftAction, rightAction, backAction);
  } else {
    snprintf(footer, sizeof(footer), "A:%s >:%s", leftAction, rightAction);
  }
  oledDisplay->print(footer);
}

static void drawSeparator(int y) {
  oledDisplay->drawFastHLine(0, y, 128, SSD1306_WHITE);
}

// ============================================================================
// Page Renderers
// ============================================================================

static void renderFeaturesPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(1, 5, "Features");
  drawHeapBar(10);
  drawSeparator(18);
  
  // Draw feature items (max 4 visible at a time)
  const int startY = 20;
  const int lineHeight = 9;
  const int maxVisible = 4;
  
  int scrollOffset = getWizardScrollOffset();
  int currentSelection = getWizardCurrentSelection();
  size_t featuresPageCount = getWizardFeaturesPageCount();
  WizardFeatureItem* featuresPage = getWizardFeaturesPage();
  
  for (int i = 0; i < maxVisible && (scrollOffset + i) < (int)featuresPageCount; i++) {
    int idx = scrollOffset + i;
    WizardFeatureItem* item = &featuresPage[idx];
    
    int y = startY + i * lineHeight;
    oledDisplay->setCursor(0, y);
    
    // Selection indicator
    if (idx == currentSelection) {
      oledDisplay->print(">");
    } else {
      oledDisplay->print(" ");
    }
    
    // Checkbox
    bool enabled = item->setting ? *item->setting : false;
    oledDisplay->print(enabled ? "[X]" : "[ ]");
    
    // Label with heap cost
    char line[24];
    const char* essential = item->essential ? "*" : "";
    snprintf(line, sizeof(line), "%s%s ~%dKB", item->label, essential, item->heapKB);
    oledDisplay->print(line);
  }
  
  // Scroll indicator if needed
  if (featuresPageCount > (size_t)maxVisible) {
    if (scrollOffset > 0) {
      oledDisplay->setCursor(120, startY);
      oledDisplay->print("^");
    }
    if (scrollOffset + maxVisible < (int)featuresPageCount) {
      oledDisplay->setCursor(120, startY + (maxVisible - 1) * lineHeight);
      oledDisplay->print("v");
    }
  }
  
  drawSeparator(54);
  drawWizardFooter("Toggle", "Next", nullptr);
  oledDisplay->display();
}

static void renderSensorsPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(2, 5, "Sensors");
  drawHeapBar(10);
  drawSeparator(18);
  
  const int startY = 20;
  const int lineHeight = 9;
  const int maxVisible = 4;
  
  int scrollOffset = getWizardScrollOffset();
  int currentSelection = getWizardCurrentSelection();
  size_t sensorsPageCount = getWizardSensorsPageCount();
  WizardFeatureItem* sensorsPage = getWizardSensorsPage();
  
  for (int i = 0; i < maxVisible && (scrollOffset + i) < (int)sensorsPageCount; i++) {
    int idx = scrollOffset + i;
    WizardFeatureItem* item = &sensorsPage[idx];
    
    int y = startY + i * lineHeight;
    oledDisplay->setCursor(0, y);
    
    if (idx == currentSelection) {
      oledDisplay->print(">");
    } else {
      oledDisplay->print(" ");
    }
    
    bool enabled = item->setting ? *item->setting : false;
    oledDisplay->print(enabled ? "[X]" : "[ ]");
    
    char line[24];
    const char* essential = item->essential ? "*" : "";
    snprintf(line, sizeof(line), "%s%s ~%dKB", item->label, essential, item->heapKB);
    oledDisplay->print(line);
  }
  
  if (sensorsPageCount > (size_t)maxVisible) {
    if (scrollOffset > 0) {
      oledDisplay->setCursor(120, startY);
      oledDisplay->print("^");
    }
    if (scrollOffset + maxVisible < (int)sensorsPageCount) {
      oledDisplay->setCursor(120, startY + (maxVisible - 1) * lineHeight);
      oledDisplay->print("v");
    }
  }
  
  drawSeparator(54);
  drawWizardFooter("Toggle", "Next", "Back");
  oledDisplay->display();
}

static void renderNetworkPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(3, 5, "Network");
  drawHeapBar(10);
  drawSeparator(18);
  
  const int startY = 20;
  const int lineHeight = 10;
  
  int currentSelection = getWizardCurrentSelection();
  size_t networkPageCount = getWizardNetworkPageCount();
  WizardNetworkItem* networkPage = getWizardNetworkPage();
  
  for (size_t i = 0; i < networkPageCount && i < 4; i++) {
    int y = startY + i * lineHeight;
    oledDisplay->setCursor(0, y);
    
    if ((int)i == currentSelection) {
      oledDisplay->print(">");
    } else {
      oledDisplay->print(" ");
    }
    
    oledDisplay->print(networkPage[i].label);
    
    // Show current value
    oledDisplay->setCursor(90, y);
    if (networkPage[i].isBool) {
      oledDisplay->print(*networkPage[i].boolSetting ? "[ON]" : "[OFF]");
    }
  }
  
  drawSeparator(54);
  drawWizardFooter("Toggle", "Next", "Back");
  oledDisplay->display();
}

static void renderSystemPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(4, 5, "System");
  drawHeapBar(10);
  drawSeparator(18);
  
  const int startY = 22;
  
  int currentSelection = getWizardCurrentSelection();
  int timezoneSelection = getWizardTimezoneSelection();
  int logLevelSelection = getWizardLogLevelSelection();
  const TimezoneEntry* timezones = getTimezones();
  const char** logLevelNames = getLogLevelNames();
  
  // Time zone
  oledDisplay->setCursor(0, startY);
  oledDisplay->print(currentSelection == 0 ? ">" : " ");
  oledDisplay->print("Timezone: ");
  oledDisplay->print(timezones[timezoneSelection].abbrev);
  
  // Log level
  oledDisplay->setCursor(0, startY + 12);
  oledDisplay->print(currentSelection == 1 ? ">" : " ");
  oledDisplay->print("Log level: ");
  oledDisplay->print(logLevelNames[logLevelSelection]);
  
  drawSeparator(54);
  drawWizardFooter("Change", "Next", "Back");
  oledDisplay->display();
}

static void renderWiFiPage(SetupWizardResult& result) {
  // This page uses existing WiFi selection UI
  oledDisplay->clearDisplay();
  drawWizardHeader(5, 5, "WiFi");
  drawHeapBar(10);
  drawSeparator(18);
  
  oledDisplay->setCursor(0, 24);
  oledDisplay->println("Select network or");
  oledDisplay->println("press B to skip...");
  
  drawSeparator(54);
  drawWizardFooter("Select", "Done", "Skip");
  oledDisplay->display();
  
  // Wait a moment then launch WiFi selector
  delay(500);
  
  String selectedSSID;
  if (getOLEDWiFiSelection(selectedSSID)) {
    result.wifiSSID = selectedSSID;
    // Get password
    result.wifiPassword = getOLEDTextInput("WiFi Password:", true, "", 64);
    result.wifiConfigured = true;
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
  
  // Joystick up
  if (nav.up) {
    wizardMoveUp();
    return true;
  }
  
  // Joystick down
  if (nav.down) {
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
  
  if (nav.up) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.down) {
    wizardMoveDown();
    return true;
  }
  
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
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
  
  if (nav.up) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.down) {
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
  
  if (nav.up) {
    wizardMoveUp();
    return true;
  }
  
  if (nav.down) {
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
    setWizardCurrentPage(WIZARD_PAGE_NETWORK);
    setWizardCurrentSelection(0);
    return true;
  }
  
  return false;
}

// ============================================================================
// Main Wizard Function
// ============================================================================

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
  
  bool running = true;
  uint32_t lastButtons = 0;
  
  while (running) {
    SetupWizardPage currentPage = getWizardCurrentPage();
    
    // Render current page
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
        renderWiFiPage(result);
        result.completed = true;
        running = false;
        break;
      default:
        running = false;
        break;
    }
    
    if (!running) break;
    
    // Read input with debounce
    delay(50);
    
    // Read buttons
    uint32_t buttons = 0xFFFFFFFF;
    if (xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gControlCache.gamepadDataValid) {
        buttons = gControlCache.gamepadButtons;
      }
      xSemaphoreGive(gControlCache.mutex);
    }
    
    // Only act on new button presses
    uint32_t newButtons = buttons & ~lastButtons;
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
      delay(150);
    }
  }
  
  return result;
}

#endif // ENABLE_OLED_DISPLAY
