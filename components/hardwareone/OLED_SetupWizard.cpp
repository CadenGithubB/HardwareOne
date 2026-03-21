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
static void renderToggleList(const char* title, SetupWizardPage page,
                             WizardFeatureItem* items, size_t itemCount,
                             const char* footerLeft, const char* footerRight,
                             const char* footerBack) {
  oledDisplay->clearDisplay();
  drawWizardHeader(getWizardPageNumber(page), getWizardTotalPages(), title);
  
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
    snprintf(line, sizeof(line), "%s %.11s%s %dK",
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

void renderFeaturesPage() {
  renderToggleList("Features", WIZARD_PAGE_FEATURES,
                   getWizardFeaturesPage(), getWizardFeaturesPageCount(),
                   "Toggle", "Next", nullptr);
}

void renderSensorsPage() {
  renderToggleList("Sensors", WIZARD_PAGE_SENSORS,
                   getWizardSensorsPage(), getWizardSensorsPageCount(),
                   "Toggle", "Next", "Back");
}

void renderNetworkPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(getWizardPageNumber(WIZARD_PAGE_NETWORK), getWizardTotalPages(), "Network");

  const int startY = 20;
  const int lineHeight = 10;
  const int maxVisible = 3;

  int currentSelection = getWizardCurrentSelection();
  size_t networkPageCount = getWizardNetworkPageCount();
  WizardNetworkItem* networkPage = getWizardNetworkPage();
  int scrollOffset = getWizardScrollOffset();

  // Adjust scroll to keep selection visible
  if (currentSelection < scrollOffset) {
    setWizardScrollOffset(currentSelection);
    scrollOffset = currentSelection;
  } else if (currentSelection >= scrollOffset + maxVisible) {
    setWizardScrollOffset(currentSelection - maxVisible + 1);
    scrollOffset = currentSelection - maxVisible + 1;
  }

  for (int i = 0; i < maxVisible && (scrollOffset + i) < (int)networkPageCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * lineHeight;
    bool isSelected = (idx == currentSelection);

    // Highlight bar for selected item
    if (isSelected) {
      oledDisplay->fillRect(0, y - 1, 126, lineHeight, SSD1306_WHITE);
      oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      oledDisplay->setTextColor(SSD1306_WHITE);
    }

    oledDisplay->setCursor(1, y);

    // Label + value
    if (networkPage[idx].isBool) {
      bool enabled = *networkPage[idx].boolSetting;
      char line[22];
      snprintf(line, sizeof(line), "%s %s", enabled ? "[ON] " : "[OFF]", networkPage[idx].label);
      oledDisplay->print(line);
    } else {
      oledDisplay->print(networkPage[idx].label);
    }

    oledDisplay->setTextColor(SSD1306_WHITE);
  }

  // Scroll indicators
  if (networkPageCount > (size_t)maxVisible) {
    if (scrollOffset > 0) {
      oledDisplay->setCursor(121, startY);
      oledDisplay->print("^");
    }
    if (scrollOffset + maxVisible < (int)networkPageCount) {
      oledDisplay->setCursor(121, startY + (maxVisible - 1) * lineHeight);
      oledDisplay->print("v");
    }
  }

  drawWizardFooter("Toggle", "Next", "Back");
  OLED_TRANSACTION(oledDisplay->display());
}

void renderSystemPage() {
  oledDisplay->clearDisplay();
  drawWizardHeader(getWizardPageNumber(WIZARD_PAGE_SYSTEM), getWizardTotalPages(), "System");

  const int startY = 20;
  const int lineHeight = 10;
  const int maxVisible = 3;

  int currentSelection = getWizardCurrentSelection();
  int scrollOffset = getWizardScrollOffset();
  size_t itemCount = getWizardSystemPageCount();

  // Adjust scroll to keep selection visible
  if (currentSelection < scrollOffset) {
    setWizardScrollOffset(currentSelection);
    scrollOffset = currentSelection;
  } else if (currentSelection >= scrollOffset + maxVisible) {
    setWizardScrollOffset(currentSelection - maxVisible + 1);
    scrollOffset = currentSelection - maxVisible + 1;
  }

  // Build display strings for each system item
  int timezoneSelection = getWizardTimezoneSelection();
  int logLevelSelection = getWizardLogLevelSelection();
  int ntpSel = getWizardNTPSelection();
  int ledSel = getWizardLEDEffectSelection();
  const TimezoneEntry* timezones = getTimezones();
  const char** logLevelNames = getLogLevelNames();
  const char* const* ntpPresets = getNTPPresets();
  const char* const* ledEffects = getLEDEffects();

  for (int i = 0; i < maxVisible && (scrollOffset + i) < (int)itemCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * lineHeight;
    bool isSelected = (idx == currentSelection);

    if (isSelected) {
      oledDisplay->fillRect(0, y - 1, 126, lineHeight, SSD1306_WHITE);
      oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      oledDisplay->setTextColor(SSD1306_WHITE);
    }

    oledDisplay->setCursor(1, y);
    char line[22];

    // Map idx to logical item (log=0, timezone=1, ntp=2?, led=3?)
    // Use same mapping as getSystemItemAt() in System_SetupWizard.cpp
    int logicalIdx = 0;
    if (idx == logicalIdx) {
      // Log level (first)
      snprintf(line, sizeof(line), "Log: %s", logLevelNames[logLevelSelection]);
      oledDisplay->print(line);
    }
    logicalIdx++;

    if (idx == logicalIdx) {
      // Timezone
      int offMin = timezones[timezoneSelection].offsetMinutes;
      int offH = offMin / 60;
      int offMrem = abs(offMin % 60);
      if (offMrem != 0)
        snprintf(line, sizeof(line), "Timezone:%-4s %+d:%02d", timezones[timezoneSelection].abbrev, offH, offMrem);
      else
        snprintf(line, sizeof(line), "Timezone:%-4s %+dh", timezones[timezoneSelection].abbrev, offH);
      oledDisplay->print(line);
    }
    logicalIdx++;

    // NTP (conditional)
    bool hasNTP = (getNTPPresetCount() > 0);
#if ENABLE_WIFI
    {
      extern const FeatureEntry* getFeatureById(const char*);
      extern bool isFeatureEnabled(const FeatureEntry*);
      const FeatureEntry* wf = getFeatureById("wifi");
      hasNTP = wf && isFeatureEnabled(wf);
    }
#else
    hasNTP = false;
#endif
    if (hasNTP) {
      if (idx == logicalIdx) {
        snprintf(line, sizeof(line), "NTP:%.16s", ntpPresets[ntpSel]);
        oledDisplay->print(line);
      }
      logicalIdx++;
    }

    // LED (conditional)
    {
      extern const FeatureEntry* getFeatureById(const char*);
      extern bool isFeatureCompiled(const FeatureEntry*);
      const FeatureEntry* lf = getFeatureById("led");
      if (lf && isFeatureCompiled(lf)) {
        if (idx == logicalIdx) {
          snprintf(line, sizeof(line), "LED: %s", ledEffects[ledSel]);
          oledDisplay->print(line);
        }
        logicalIdx++;
      }
    }

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

  drawWizardFooter("Change", "Next", "Back");
  OLED_TRANSACTION(oledDisplay->display());
}

bool renderWiFiPage(SetupWizardResult& result) {
  // Loop to allow going back from password entry to network selection
  // Returns true if WiFi setup completed (or skipped), false if user pressed B to go back
  while (true) {
    // This page uses existing WiFi selection UI
    oledDisplay->clearDisplay();
    drawWizardHeader(getWizardPageNumber(WIZARD_PAGE_WIFI), getWizardTotalPages(), "WiFi");
    
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

void resetWizardJoystickState() {
  sJoyUpHeld = false;
  sJoyDownHeld = false;
  sJoyLeftHeld = false;
  sJoyRightHeld = false;
}

JoystickNav readWizardJoystickNav() {
  JoystickNav nav = {false, false, false, false};
  
  if (xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (gControlCache.gamepadDataValid) {
      int joyX = gControlCache.gamepadX;
      int joyY = gControlCache.gamepadY;
      
      int deltaX = joyX - JOYSTICK_CENTER;
      int deltaY = JOYSTICK_CENTER - joyY;  // Physical DOWN = positive (matches main menu)
      
      // Check for new deflections (edge detection)
      bool deflectedUp = deltaY < -JOYSTICK_DEADZONE;
      bool deflectedDown = deltaY > JOYSTICK_DEADZONE;
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

// Shared toggle-page input handler: A=toggle, up/down=move, right=next, left/B=back
static bool handleTogglePageInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    wizardToggleCurrentItem();
    return true;
  }

  if (nav.up) { wizardMoveUp(); return true; }
  if (nav.down) { wizardMoveDown(); return true; }

  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    wizardNextPage(result);
    return true;
  }

  if ((buttons & INPUT_MASK(INPUT_BUTTON_B)) || nav.left) {
    wizardPrevPage();
    return true;
  }

  return false;
}

// We need a dummy result for pages that don't need it
static SetupWizardResult sDummyResult;

bool handleFeaturesInput(uint32_t buttons, JoystickNav& nav) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    wizardToggleCurrentItem();
    return true;
  }
  if (nav.up) { wizardMoveUp(); return true; }
  if (nav.down) { wizardMoveDown(); return true; }

  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) {
    wizardNextPage(sDummyResult);
    return true;
  }
  // No back from first page
  return false;
}

bool handleSensorsInput(uint32_t buttons, JoystickNav& nav) {
  return handleTogglePageInput(buttons, nav, sDummyResult);
}

bool handleNetworkInput(uint32_t buttons, JoystickNav& nav) {
  return handleTogglePageInput(buttons, nav, sDummyResult);
}

bool handleSystemInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result) {
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
    if (!wizardNextPage(result)) {
      return false;  // Signal completion
    }
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
// Optional page intro: shows a "Configure / Skip" selection screen.
// Returns: 1=Configure, 0=Skip, -1=Back (wizardPrevPage already called)
// ============================================================================

static int showWizardOptionalPageIntro(SetupWizardPage page, const char* title,
                                       const char* description) {
  int selection = 0; // 0 = Configure, 1 = Skip
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  sJoyUpHeld = false;
  sJoyDownHeld = false;

  // Print prompt on serial so terminal users can also navigate
  Serial.println();
  Serial.printf("=== %s (SETUP %d/%d) ===\n",
                title, getWizardPageNumber(page), getWizardTotalPages());
  Serial.println(description);
  Serial.println("----------------------------------------");
  Serial.println(" c = Configure   s = Skip   b = Back");
  Serial.print("Choice [s]: ");

  while (true) {
    // Check serial input (non-blocking) — 'c'/'s'/'n'/'b' all work
    if (Serial.available()) {
      String in = Serial.readStringUntil('\n');
      in.trim(); in.toLowerCase();
      Serial.println(in);
      if (in == "b" || in == "back") { wizardPrevPage(); return -1; }
      if (in == "c" || in == "configure") return 1;
      return 0; // 's', 'n', enter, or anything else = skip
    }

    oledDisplay->clearDisplay();
    drawWizardHeader(getWizardPageNumber(page), getWizardTotalPages(), title);

    oledDisplay->setCursor(0, 20);
    oledDisplay->print(description);

    // Options at y=34 and y=44
    oledDisplay->setCursor(1, 34);
    oledDisplay->print(selection == 0 ? ">" : " ");
    oledDisplay->println(" Configure");

    oledDisplay->setCursor(1, 44);
    oledDisplay->print(selection == 1 ? ">" : " ");
    oledDisplay->println(" Skip");

    drawWizardFooter("Select", "", "Back");
    OLED_TRANSACTION(oledDisplay->display());

    delay(50);

    JoystickNav nav = readWizardJoystickNav();
    if (nav.up || nav.down) {
      selection = selection == 0 ? 1 : 0;
      delay(150);
      continue;
    }

    uint32_t buttons = lastButtons;
    bool haveButtons = false;
    if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gControlCache.gamepadDataValid) { buttons = gControlCache.gamepadButtons; haveButtons = true; }
      xSemaphoreGive(gControlCache.mutex);
    }
    if (haveButtons && !lastButtonsInitialized) { lastButtons = buttons; lastButtonsInitialized = true; continue; }
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtons;
    uint32_t newButtons = pressedNow & ~pressedLast;
    lastButtons = buttons;

    if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) {
      return (selection == 0) ? 1 : 0; // 1=configure, 0=skip
    }
    if (newButtons & INPUT_MASK(INPUT_BUTTON_B)) {
      wizardPrevPage();
      return -1; // back
    }
  }
  return 0;
}

// ============================================================================
// OLED ESP-NOW Identity Page (text input for each field)
// ============================================================================

void handleOLEDESPNowPage(SetupWizardResult& result, bool& running) {
  // Show intro screen: Configure or Skip?
  int choice = showWizardOptionalPageIntro(WIZARD_PAGE_ESPNOW, "ESP-NOW",
      "Optional: ESP-NOW ID");
  if (choice == -1) return; // went back
  if (choice == 0) {        // skip
    if (!wizardNextPage(result)) running = false;
    return;
  }

  bool cancelled = false;

  // Room
  String room = getOLEDTextInput("Room:", false, gSettings.espnowRoom.c_str(), 20, &cancelled);
  if (cancelled) { wizardPrevPage(); return; }
  result.espnowRoom = room;

  // Zone
  String zone = getOLEDTextInput("Zone:", false, gSettings.espnowZone.c_str(), 20, &cancelled);
  if (cancelled) { wizardPrevPage(); return; }
  result.espnowZone = zone;

  // Friendly name mirrors the device name set at the start of setup
  result.espnowFriendlyName = gSettings.espnowDeviceName;

  // Stationary - simple toggle selection
  {
    int selection = gSettings.espnowStationary ? 1 : 0;
    uint32_t lastButtons = 0;
    bool lastButtonsInitialized = false;
    sJoyUpHeld = false;
    sJoyDownHeld = false;

    while (true) {
      oledDisplay->clearDisplay();
      drawWizardHeader(getWizardPageNumber(WIZARD_PAGE_ESPNOW), getWizardTotalPages(), "ESP-NOW");
      oledDisplay->setCursor(0, 20);
      oledDisplay->println("The device will be:");
      oledDisplay->setCursor(1, 34);
      oledDisplay->print(selection == 0 ? ">" : " ");
      oledDisplay->println(" Mobile");
      oledDisplay->setCursor(1, 44);
      oledDisplay->print(selection == 1 ? ">" : " ");
      oledDisplay->println(" Stationary");
      drawWizardFooter("Select", "", "Back");
      OLED_TRANSACTION(oledDisplay->display());

      delay(50);

      JoystickNav nav = readWizardJoystickNav();
      if (nav.up || nav.down) { selection = selection == 0 ? 1 : 0; delay(150); continue; }

      uint32_t buttons = lastButtons;
      bool haveButtons = false;
      if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (gControlCache.gamepadDataValid) { buttons = gControlCache.gamepadButtons; haveButtons = true; }
        xSemaphoreGive(gControlCache.mutex);
      }
      if (haveButtons && !lastButtonsInitialized) { lastButtons = buttons; lastButtonsInitialized = true; continue; }
      uint32_t pressedNow = ~buttons;
      uint32_t pressedLast = ~lastButtons;
      uint32_t newButtons = pressedNow & ~pressedLast;
      lastButtons = buttons;

      if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) {
        result.espnowStationary = (selection == 1);
        break;
      }
      if (newButtons & INPUT_MASK(INPUT_BUTTON_B)) {
        wizardPrevPage();
        return;
      }
    }
  }

  // Apply to settings
  if (result.espnowRoom.length() > 0) gSettings.espnowRoom = result.espnowRoom;
  if (result.espnowZone.length() > 0) gSettings.espnowZone = result.espnowZone;
  if (result.espnowFriendlyName.length() > 0) gSettings.espnowFriendlyName = result.espnowFriendlyName;
  gSettings.espnowStationary = result.espnowStationary;

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

// ============================================================================
// OLED MQTT Broker Page (text input for each field)
// ============================================================================

void handleOLEDMQTTPage(SetupWizardResult& result, bool& running) {
  // Show intro screen: Configure or Skip?
  int choice = showWizardOptionalPageIntro(WIZARD_PAGE_MQTT, "MQTT",
      "Optional: MQTT broker");
  if (choice == -1) return; // went back
  if (choice == 0) {        // skip
    if (!wizardNextPage(result)) running = false;
    return;
  }

  bool cancelled = false;

  // Host
  String host = getOLEDTextInput("MQTT Host:", false,
    gSettings.mqttHost.length() > 0 ? gSettings.mqttHost.c_str() : "", 40, &cancelled);
  if (cancelled) { wizardPrevPage(); return; }
  result.mqttHost = host;

  // Port
  String portStr = getOLEDTextInput("MQTT Port:", false, "1883", 5, &cancelled);
  if (cancelled) { wizardPrevPage(); return; }
  result.mqttPort = portStr.length() > 0 ? portStr.toInt() : 0;

  // Username
  String user = getOLEDTextInput("MQTT User:", false, "", 32, &cancelled);
  if (cancelled) { wizardPrevPage(); return; }
  result.mqttUser = user;

  // Password (only if username was entered)
  if (user.length() > 0) {
    String pass = getOLEDTextInput("MQTT Password:", true, "", 32, &cancelled);
    if (cancelled) { wizardPrevPage(); return; }
    result.mqttPassword = pass;
  }

  // Apply to settings
  if (result.mqttHost.length() > 0) gSettings.mqttHost = result.mqttHost;
  if (result.mqttPort > 0) gSettings.mqttPort = result.mqttPort;
  if (result.mqttUser.length() > 0) gSettings.mqttUser = result.mqttUser;
  if (result.mqttPassword.length() > 0) gSettings.mqttPassword = result.mqttPassword;

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

// ============================================================================
// Setup Mode Selection (Basic vs Advanced vs Import from Backup)
// ============================================================================

bool getOLEDSetupModeSelection(int& setupMode) {
  int selection = 0;  // 0 = Basic, 1 = Advanced, 2 = Import from Backup
#if ENABLE_HTTP_SERVER && ENABLE_WIFI
  const int NUM_OPTIONS = 3;
  const bool importAvailable = true;
#else
  const int NUM_OPTIONS = 2;
  const bool importAvailable = false;
#endif
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
      if (importAvailable) {
        Serial.printf(" %s3. Import from Backup\n", selection == 2 ? ">" : " ");
      }
      Serial.println("----------------------------------------");
      Serial.printf("Serial: Enter 1%s\n", importAvailable ? ", 2, or 3" : " or 2");
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
      oledDisplay->setCursor(0, 24);
      oledDisplay->print(selection == 0 ? ">" : " ");
      oledDisplay->println(" Basic Setup");

      // Option 2: Advanced
      oledDisplay->setCursor(0, 34);
      oledDisplay->print(selection == 1 ? ">" : " ");
      oledDisplay->println(" Advanced Setup");

      // Option 3: Import from Backup (only if WiFi+HTTP compiled)
      if (importAvailable) {
        oledDisplay->setCursor(0, 44);
        oledDisplay->print(selection == 2 ? ">" : " ");
        oledDisplay->println(" Import from Backup");
      }

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
      JoystickNav nav = readWizardJoystickNav();

      if (nav.up) {
        selection = (selection > 0) ? selection - 1 : NUM_OPTIONS - 1;
        delay(150);
        continue;
      }
      if (nav.down) {
        selection = (selection < NUM_OPTIONS - 1) ? selection + 1 : 0;
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
        setupMode = selection;
        return true;
      }
    }

    // Also check serial input
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input == "1" || input.equalsIgnoreCase("basic")) {
        setupMode = 0;
        return true;
      } else if (input == "2" || input.equalsIgnoreCase("advanced")) {
        setupMode = 1;
        return true;
      } else if (importAvailable && (input == "3" || input.equalsIgnoreCase("restore") || input.equalsIgnoreCase("import"))) {
        setupMode = 2;
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
      JoystickNav nav = readWizardJoystickNav();
      
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
  return runSetupWizard();
}


#endif // ENABLE_OLED_DISPLAY
