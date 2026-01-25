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
  i2cDeviceTransactionVoid(OLED_I2C_ADDRESS, 100000, 500, [&]() { code; })

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern Settings gSettings;
extern bool writeSettingsJson();
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
  
  // Title with page number
  char header[32];
  snprintf(header, sizeof(header), "SETUP %d/%d: %s", pageNum, totalPages, title);
  oledDisplay->println(header);
}

void drawHeapBar(int y) {
  uint32_t enabledKB = 0;
  uint32_t maxKB = 1;
  int percentage = 0;
  getHeapBarData(&enabledKB, &maxKB, &percentage);
  if (maxKB == 0) maxKB = 1;
  
  // Draw bar background - shorter bar to fit text
  const int barX = 0;
  const int barWidth = 70;
  const int barHeight = 6;
  
  oledDisplay->drawRect(barX, y, barWidth, barHeight, SSD1306_WHITE);
  
  // Fill bar proportionally
  int fillWidth = (barWidth - 2) * percentage / 100;
  if (fillWidth > barWidth - 2) fillWidth = barWidth - 2;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, y + 1, fillWidth, barHeight - 2, SSD1306_WHITE);
  }
  
  // Draw text: XX/XXXKB (compact format to fit)
  char heapText[24];
  snprintf(heapText, sizeof(heapText), "%lu/%luKB", (unsigned long)enabledKB, (unsigned long)maxKB);
  oledDisplay->setCursor(barX + barWidth + 2, y);
  oledDisplay->print(heapText);
}

void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction) {
  oledDisplay->setCursor(0, 56);
  oledDisplay->setTextSize(1);
  
  // Compact format to fit 21 chars: "A:Tog >:Nxt B:Bk"
  char footer[22];
  if (backAction) {
    snprintf(footer, sizeof(footer), "A:%.3s >:%.3s B:%.2s", leftAction, rightAction, backAction);
  } else {
    snprintf(footer, sizeof(footer), "A:%.4s >:%.4s", leftAction, rightAction);
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
    
    // Label with heap cost - truncate label to fit (max 21 chars total, ~10 for label)
    char line[18];
    char truncLabel[11];
    strncpy(truncLabel, item->label, 10);
    truncLabel[10] = '\0';
    const char* essential = item->essential ? "*" : "";
    snprintf(line, sizeof(line), "%.9s%s %dK", truncLabel, essential, item->heapKB);
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
  OLED_TRANSACTION(oledDisplay->display());
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
    
    // Label with heap cost - truncate label to fit
    char line[18];
    char truncLabel[11];
    strncpy(truncLabel, item->label, 10);
    truncLabel[10] = '\0';
    const char* essential = item->essential ? "*" : "";
    snprintf(line, sizeof(line), "%.9s%s %dK", truncLabel, essential, item->heapKB);
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
  OLED_TRANSACTION(oledDisplay->display());
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
    
    // Truncate label to fit with value
    char truncLabel[13];
    strncpy(truncLabel, networkPage[i].label, 12);
    truncLabel[12] = '\0';
    oledDisplay->print(truncLabel);
    
    // Show current value
    oledDisplay->setCursor(84, y);
    if (networkPage[i].isBool) {
      oledDisplay->print(*networkPage[i].boolSetting ? "[ON]" : "[OFF]");
    }
  }
  
  drawSeparator(54);
  drawWizardFooter("Toggle", "Next", "Back");
  OLED_TRANSACTION(oledDisplay->display());
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
  OLED_TRANSACTION(oledDisplay->display());
}

static void renderWiFiPage(SetupWizardResult& result) {
  // Loop to allow going back from password entry to network selection
  bool wifiSetupComplete = false;
  
  while (!wifiSetupComplete) {
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
      wifiSetupComplete = true;
    } else {
      // WiFi selection skipped/cancelled
      wifiSetupComplete = true;
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
    setWizardCurrentPage(WIZARD_PAGE_NETWORK);
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
      Serial.println("  SETUP 1/5: Features (Network)");
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
          renderWiFiPage(result);
          result.completed = true;
          running = false;
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
