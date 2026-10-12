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
#include "System_SetupWizard.h"
#include "System_FirstTimeSetup.h"
#include "HAL_Input.h"
#include "i2csensor_seesaw.h"
#include "System_I2C.h"
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

// OLED_I2C_ADDRESS, OLED_TRANSACTION, and oledConnected from OLED_Display.h

// OLED text input
extern String getOLEDTextInput(const char* prompt, bool masked, const char* defaultValue, int maxLen, bool* wasCancelled = nullptr, bool canSkip = true);
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
  oledDisplay->setTextWrap(false);

  // Keep the group title readable, with its position aligned on the right.
  char position[12];
  snprintf(position, sizeof(position), "%d/%d", pageNum, totalPages);
  const int positionX = 128 - (int)strlen(position) * 6;
  char heading[22];
  snprintf(heading, sizeof(heading), "%.*s", positionX / 6, title);
  oledDisplay->setCursor(0, 0);
  oledDisplay->print(heading);
  oledDisplay->setCursor(positionX, 0);
  oledDisplay->print(position);
  oledDisplay->setTextWrap(true);

  uint32_t enabledKB = 0, maxKB = 1;
  int percentage = 0;
  getHeapBarData(&enabledKB, &maxKB, &percentage);
  char heapLabel[12];
  snprintf(heapLabel, sizeof(heapLabel), "%lu/%luKB", (unsigned long)enabledKB, (unsigned long)maxKB);
  oledDrawBar(oledDisplay, 0, 10, 70, 6, percentage, 100, heapLabel);
  oledDisplay->drawFastHLine(0, 18, 128, SSD1306_WHITE);
}

void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction) {
  const int footerStartY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
  const int footerY = footerStartY + 2;

  oledDisplay->drawFastHLine(0, footerStartY, 128, SSD1306_WHITE);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, footerY);

  if (leftAction && leftAction[0]) {
    oledDisplay->print("A:");
    oledDisplay->print(leftAction);
  }
  if (rightAction && rightAction[0]) {
#if ENABLE_GPIO_ENCODER
    oledDisplay->print(" X:");
#else
    oledDisplay->print(" >:");
#endif
    oledDisplay->print(rightAction);
  }
  if (backAction && backAction[0]) {
    oledDisplay->print(" ");
    oledDrawBackArrowIcon(oledDisplay, footerY);
  }
}


// ============================================================================
// Shared info-page renderer — title + header rule, wrapped body, footer rule
// ============================================================================
void drawSetupInfoPage(const char* title, const char* body, const char* footer,
                       int pageNum, int pageCount) {
  if (!oledDisplay || !oledConnected) return;
  OLED_TRANSACTION(
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(SSD1306_WHITE);
    // Header: optional "n/N" indicator top-right, then the title clipped to the
    // space left of it (so a long title never overlaps the indicator).
    int titleMax = 21;  // chars per 128px line at size 1
    if (pageCount > 0) {
      char ind[12];
      snprintf(ind, sizeof(ind), "%d/%d", pageNum, pageCount);
      int indChars = strlen(ind);
      oledDisplay->setTextWrap(false);
      oledDisplay->setCursor(128 - indChars * 6, 0);
      oledDisplay->print(ind);
      titleMax = 21 - indChars - 1;  // leave a 1-char gap before the indicator
    }
    String tt = String(title);
    if ((int)tt.length() > titleMax) tt = tt.substring(0, titleMax - 1) + ".";
    oledDisplay->setTextWrap(false);
    oledDisplay->setCursor(0, 0);
    oledDisplay->print(tt);
    oledDisplay->setTextWrap(true);
    oledDisplay->drawFastHLine(0, 10, 128, SSD1306_WHITE);
    // Body (word-wraps at the display width)
    oledDisplay->setCursor(0, 14);
    oledDisplay->print(body);
    // Footer: rule + free-form hint
    const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
    oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
    oledDisplay->setCursor(0, footerY + 2);
    oledDisplay->print(footer);
    oledDisplay->display();
  );
}


// ============================================================================
// Page Renderers
// ============================================================================

void renderWizardPage() {
  const SetupWizardPage page = getWizardCurrentPage();
  const int itemCount = (int)getWizardMenuItemCount(page);
  const int selection = getWizardCurrentSelection();
  const int maxVisible = 4;
  int scrollOffset = getWizardScrollOffset();
  if (selection < scrollOffset) scrollOffset = selection;
  if (selection >= scrollOffset + maxVisible) scrollOffset = selection - maxVisible + 1;
  if (scrollOffset < 0) scrollOffset = 0;
  setWizardScrollOffset(scrollOffset);

  oledDisplay->clearDisplay();
  drawWizardHeader(getWizardPageNumber(page), getWizardTotalPages(), getWizardPageTitle(page));
  const int startY = 21;
  const int lineHeight = 8;
  for (int row = 0; row < maxVisible && scrollOffset + row < itemCount; ++row) {
    const int index = scrollOffset + row;
    const int y = startY + row * lineHeight;
    char label[64], value[64];
    if (!wizardFormatMenuItem(page, index, label, sizeof(label), value, sizeof(value))) continue;
    if (index == selection) {
      oledDisplay->fillRect(0, y - 1, 126, lineHeight, SSD1306_WHITE);
      oledDisplay->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      oledDisplay->setTextColor(SSD1306_WHITE);
    }
    // Keep the current value visible beside its label; long names remain editable.
    const int valueWidth = min((int)strlen(value), 12);
    char line[22];
    snprintf(line, sizeof(line), "%.*s %.*s", 19 - valueWidth, label, valueWidth, value);
    oledDisplay->setCursor(1, y);
    oledDisplay->print(line);
  }
  oledDisplay->setTextColor(SSD1306_WHITE);
  if (scrollOffset > 0) {
    oledDisplay->setCursor(121, startY);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisible < itemCount) {
    oledDisplay->setCursor(121, startY + (maxVisible - 1) * lineHeight);
    oledDisplay->print("v");
  }
  drawWizardFooter("Edit", wizardAdvanceFrom(page) == WIZARD_PAGE_COUNT ? "Finish" : "Next",
                   wizardIsFirstPage() ? "Cancel" : "Back");
  OLED_TRANSACTION(oledDisplay->display());
}

static int showWizardOptionalPageIntro(SetupWizardPage page, const char* title,
                                       const char* description);

bool renderWiFiPage(SetupWizardResult& result) {
  while (true) {
    if (isWizardCancelRequested()) { result.completed = false; return false; }
    const int choice = showWizardOptionalPageIntro(WIZARD_PAGE_WIFI, "WiFi", "Optional: network");
    if (choice < 0) return false; // Back already navigated; Cancel is handled by the owner.
    if (choice == 0) {
      result.wifiConfigured = false;
      result.wifiSSID = "";
      result.wifiPassword = "";
      wizardNextPage(result);
      return true;
    }

    // Back from password entry returns to the picker. Back from the picker
    // returns to Configure/Skip; only the intro's Back leaves this page.
    while (true) {
      String selectedSSID;
      if (!getOLEDWiFiSelection(selectedSSID)) break;
      bool passwordCancelled = false;
      String password = getOLEDTextInput("WiFi Password:", true, "", 64, &passwordCancelled);
      if (isWizardCancelRequested()) { result.completed = false; return false; }
      if (passwordCancelled) continue;
      result.wifiSSID = selectedSSID;
      result.wifiPassword = password;
      result.wifiConfigured = true;
      wizardNextPage(result);
      return true;
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
  // Consume outside the cache lock: GPIO wheels share that mutex with buttons.
  const int detent = inputConsumeOneDetent();
  nav.up = detent < 0;
  nav.down = detent > 0;

  InputCache input{};
  if (inputCacheSnapshot(input)) {
    if (input.dataValid) {
      int joyX = input.joyX;
      int joyY = input.joyY;
      
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
  }

  if (gWizardOwnsSerial && (nav.up || nav.down || nav.left || nav.right)) noteWizardInputActivity();
  return nav;
}

// ============================================================================
// Input Handling
// ============================================================================

// Shared toggle-page input handler: A=toggle, up/down=move, right=next, left/B=back
static bool wizardNextRequested(uint32_t buttons, const JoystickNav& nav) {
  if (nav.right || (buttons & INPUT_MASK(INPUT_BUTTON_START))) return true;
#if ENABLE_GPIO_ENCODER
  // The one-switch wheel exposes X via press-and-turn; use it for Next while
  // double-click remains the keyboard mode/function key.
  if (buttons & INPUT_MASK(INPUT_BUTTON_X)) return true;
#endif
  return false;
}

bool handleWizardPageInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) {
    const WizardMenuItem* item = getWizardMenuItem(getWizardCurrentPage(), getWizardCurrentSelection());
    if (item && item->type == WIZARD_ITEM_DEVICE_NAME) {
      bool cancelled = false;
      String newName = getOLEDTextInput("Device Name:", false, getWizardDeviceName(), 19, &cancelled);
      if (!cancelled && !isWizardCancelRequested() && newName.length() > 0) {
        char* buf = getWizardDeviceNameBuf();
        strncpy(buf, newName.c_str(), 19);
        buf[19] = '\0';
      }
    } else {
      wizardActivateCurrentItem();
    }
    return true;
  }
  if (nav.up) { wizardMoveUp(); return true; }
  if (nav.down) { wizardMoveDown(); return true; }
  if (wizardNextRequested(buttons, nav)) {
    wizardNextPage(result);
    return true;
  }
  if ((buttons & INPUT_MASK(INPUT_BUTTON_B)) || nav.left) {
    if (wizardIsFirstPage()) requestWizardCancel();
    else wizardPrevPage();
    return true;
  }
  return false;
}

// ============================================================================
// Optional page intro: shows a "Configure / Skip" selection screen.
// Returns: 1=Configure, 0=Skip, -1=Back (already navigated), -2=Cancel
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
  Serial.println(" c = Configure   n = Next (skip)   b = Back");
  Serial.print("Choice: ");

  while (true) {
    if (isWizardCancelRequested()) return -2;
    // Check serial input (non-blocking) — 'c'/'n'/'b'/'cancel' all work
    if (Serial.available()) {
      String in = Serial.readStringUntil('\n');
      if (wizardHandleCancelInput(in)) return -2;
      in.trim(); in.toLowerCase();
      Serial.println(in);
      if (in == "b" || in == "back") { wizardPrevPage(); return -1; }
      if (in == "c" || in == "configure") return 1;
      if (in == "n" || in == "next" || in.length() == 0) return 0; // explicit skip (or blank/Enter)
      // Anything else looks like the user typed their answer directly instead
      // of picking from the c/n/b menu (e.g. a device name or MQTT host) —
      // treat it as "configure" rather than silently skipping, which is
      // surprising for what's almost always a menu mix-up, not an intentional
      // skip. The typed text itself is re-prompted for by the next field.
      return 1;
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

    const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
    if (newButtons) noteWizardInputActivity();

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
  if (choice == -2) { result.completed = false; running = false; return; }
  if (choice == -1) return; // went back
  if (choice == 0) {        // skip — disable ESP-NOW since it's unconfigured
    wizardSetFeatureEnabled("espnow", false);
    if (!wizardNextPage(result)) running = false;
    return;
  }

  bool cancelled = false;
  result.espnowFriendlyName = "";
  result.espnowRoom = "";
  result.espnowZone = "";
  result.espnowStationary = getWizardDraftValues().espnowStationary;

  // Device Name (used for Bluetooth + ESP-NOW identity)
  const String& draftName = getWizardDraftValues().deviceName;
  String currentName = draftName.length() > 0 ? draftName : "HardwareOne";
  String deviceName = getOLEDTextInput("Device Name:", false, currentName.c_str(), 19, &cancelled);
  if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
  if (cancelled) { wizardPrevPage(); return; }
  if (deviceName.length() == 0) deviceName = currentName;
  result.espnowFriendlyName = deviceName;

  // Room
  String room = getOLEDTextInput("Room:", false, getWizardDraftValues().espnowRoom.c_str(), 20, &cancelled);
  if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
  if (cancelled) { wizardPrevPage(); return; }
  result.espnowRoom = room;

  // Zone
  String zone = getOLEDTextInput("Zone:", false, getWizardDraftValues().espnowZone.c_str(), 20, &cancelled);
  if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
  if (cancelled) { wizardPrevPage(); return; }
  result.espnowZone = zone;

  // Stationary - simple toggle selection (OLED + serial)
  {
    int selection = getWizardDraftValues().espnowStationary ? 1 : 0;
    uint32_t lastButtons = 0;
    bool lastButtonsInitialized = false;
    sJoyUpHeld = false;
    sJoyDownHeld = false;

    // Echo prompt to serial so users monitoring via terminal can respond
    Serial.println(selection ? "The device will be — (m)obile or (s)tationary [s]:"
                             : "The device will be — (m)obile or (s)tationary [m]:");

    while (true) {
      if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
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

      // Check for serial input
      if (Serial.available()) {
        String serialInput = Serial.readStringUntil('\n');
        if (wizardHandleCancelInput(serialInput)) { result.completed = false; running = false; return; }
        serialInput.trim();
        if (serialInput.equalsIgnoreCase("b") || serialInput.equalsIgnoreCase("back")) {
          wizardPrevPage();
          return;
        }
        result.espnowStationary = serialInput.length() == 0 ? (selection == 1)
          : (serialInput.equalsIgnoreCase("s") || serialInput.equalsIgnoreCase("stationary"));
        break;
      }

      JoystickNav nav = readWizardJoystickNav();
      if (nav.up || nav.down) { selection = selection == 0 ? 1 : 0; delay(150); continue; }

      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      if (newButtons) noteWizardInputActivity();

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

  if (!wizardStageESPNowSettings(result)) {
    broadcastOutput("Error: ESP-NOW choices could not be staged. Try again or cancel.");
    return;
  }

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
  if (choice == -2) { result.completed = false; running = false; return; }
  if (choice == -1) return; // went back
  if (choice == 0) {        // skip: a configured broker stays as it is (owner rule)
    wizardSkipMQTTPage();
    if (!wizardNextPage(result)) running = false;
    return;
  }

  bool cancelled = false;
  result.mqttHost = "";
  result.mqttPort = 0;
  result.mqttUser = "";
  result.mqttPassword = "";

  // Host
  String host = getOLEDTextInput("MQTT Host:", false,
    getWizardDraftValues().mqttHost.c_str(), 40, &cancelled);
  if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
  if (cancelled) { wizardPrevPage(); return; }
  result.mqttHost = host;

  // Port
  const String currentPort(getWizardDraftValues().mqttPort);
  while (true) {
    String portStr = getOLEDTextInput("MQTT Port:", false, currentPort.c_str(), 5, &cancelled);
    if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
    if (cancelled) { wizardPrevPage(); return; }
    if (wizardParseMQTTPort(portStr, result.mqttPort)) break;
    broadcastOutput("Error: MQTT port must be 1..65535, or blank to keep it.");
  }

  // Username
  String user = getOLEDTextInput("MQTT User:", false, getWizardDraftValues().mqttUser.c_str(), 32, &cancelled);
  if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
  if (cancelled) { wizardPrevPage(); return; }
  result.mqttUser = user;

  // Password (only if username was entered)
  if (user.length() > 0) {
    String pass = getOLEDTextInput("MQTT Password:", true, "", 32, &cancelled);
    if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
    if (cancelled) { wizardPrevPage(); return; }
    result.mqttPassword = pass;
  }

  if (!wizardStageMQTTSettings(result)) {
    broadcastOutput("Error: MQTT choices could not be staged. Try again or cancel.");
    return;
  }

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

// ============================================================================
// WEBMODE / BTMODE conditional page (FTS / blocking engine)
// ============================================================================
// Pick the feature's mode (HTTP/HTTPS or Server/G2). Self-contained blocking
// sub-flow handling BOTH OLED buttons and serial, navigated via the wizard's
// page model — A / number / 'n' selects and advances; B / left / 'b' returns to
// the previous page. Mirrors the MQTT/WiFi conditional-page handlers. The mode
// table + accessors are shared with the CLI-mode engine (see System_SetupWizard
// .cpp: wizardModeMenuForPage / wizardModeCurrentIndex / wizardModeApply), so
// both wizards behave identically. The page only appears when its feature is
// enabled (wizardShouldShowWebMode/BtMode), so it's never skipped silently.
void handleModePage(SetupWizardPage page, SetupWizardResult& result, bool& running) {
  const WizardModeMenu* sub = wizardModeMenuForPage(page);
  if (!sub) { if (!wizardNextPage(result)) running = false; return; }   // nothing to pick — skip

  int sel = wizardModeCurrentIndex(sub);
  int lastPrinted = -1;
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  sJoyUpHeld = false;
  sJoyDownHeld = false;

  while (true) {
    if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
    if (sel != lastPrinted) {
      Serial.println();
      Serial.printf("=== SETUP %d/%d: %s ===\n", getWizardPageNumber(page), getWizardTotalPages(), sub->title);
      for (int i = 0; i < sub->modeCount; i++) {
        Serial.printf(" %s%d. [%s] %s\n", sel == i ? ">" : " ", i + 1,
                      sel == i ? "X" : " ", sub->modes[i].label);
      }
      Serial.println("----------------------------------------");
      Serial.println("Serial: # to select, 'n' next, 'b' back");
      Serial.println("OLED:   Joystick + A=Select, B=Back");
      Serial.print("> ");
      lastPrinted = sel;
    }

    if (oledDisplay && oledConnected) {
      oledDisplay->clearDisplay();
      drawWizardHeader(getWizardPageNumber(page), getWizardTotalPages(), sub->title);
      for (int i = 0; i < sub->modeCount && i < 4; i++) {
        oledDisplay->setCursor(0, 20 + i * 11);
        oledDisplay->setTextColor(SSD1306_WHITE);
        oledDisplay->print(sel == i ? ">" : " ");
        oledDisplay->print(sel == i ? " [X] " : " [ ] ");
        oledDisplay->println(sub->modes[i].label);
      }
      const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
      oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
      oledDisplay->setCursor(0, footerY + 2);
      oledDisplay->print("A/>:Next  B/<:Back");
      OLED_TRANSACTION(oledDisplay->display());
    }

    delay(50);

    if (oledDisplay && oledConnected) {
      JoystickNav nav = readWizardJoystickNav();
      // Apply on scroll (like the feature-toggle pages) so the header heap bar
      // updates live to reflect the highlighted mode (e.g. HTTPS over HTTP).
      if (nav.up)   { sel = (sel > 0) ? sel - 1 : sub->modeCount - 1; wizardModeApply(sub, sel); delay(150); continue; }
      if (nav.down) { sel = (sel < sub->modeCount - 1) ? sel + 1 : 0; wizardModeApply(sub, sel); delay(150); continue; }
      if (nav.left) { wizardPrevPage(); return; }
      // Joystick right advances (apply current pick + next), matching A and the
      // other wizard pages — without this, right did nothing on the mode pages.
      if (nav.right) { wizardModeApply(sub, sel); if (!wizardNextPage(result)) running = false; return; }

      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      if (newButtons) noteWizardInputActivity();
      if (newButtons & (INPUT_MASK(INPUT_BUTTON_A) | INPUT_MASK(INPUT_BUTTON_START))) {
        wizardModeApply(sub, sel);
        if (!wizardNextPage(result)) running = false;
        return;
      }
      if (newButtons & INPUT_MASK(INPUT_BUTTON_B)) { wizardPrevPage(); return; }
    }

    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      if (wizardHandleCancelInput(input)) { result.completed = false; running = false; return; }
      input.trim();
      if (input.equalsIgnoreCase("b") || input.equalsIgnoreCase("back")) { wizardPrevPage(); return; }
      if (input.equalsIgnoreCase("n") || input.equalsIgnoreCase("next")) {
        wizardModeApply(sub, sel);
        if (!wizardNextPage(result)) running = false;
        return;
      }
      int n = input.toInt();
      if (n >= 1 && n <= sub->modeCount) {
        // Select that mode and STAY on the page (re-render shows the moved [X]),
        // exactly like the feature/sensor pages. Advancing is 'n' only — matches
        // the on-screen "# to select, 'n' next" hint. (Was: select-and-advance.)
        sel = n - 1;
        wizardModeApply(sub, sel);
        lastPrinted = -1;   // force re-render of the page with the new selection
        continue;
      }
    }
  }
}


// ============================================================================
// Setup Mode Selection (Basic vs Advanced vs Import from Backup)
// ============================================================================

bool getOLEDSetupModeSelection(int& setupMode) {
  const int optionCount = (int)getSetupModeCount();
  if (optionCount == 0) return false;
  int selection = 0;
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  int lastPrintedSelection = -1;
  sJoyUpHeld = false;
  sJoyDownHeld = false;

  while (true) {
    if (selection != lastPrintedSelection) {
      Serial.println();
      Serial.println("========================================");
      Serial.println("       SELECT SETUP MODE");
      Serial.println("========================================");
      for (int i = 0; i < optionCount; ++i) {
        const SetupModeOption* option = getSetupModeOption(i);
        Serial.printf(" %s%d. %s - %s\n", selection == i ? ">" : " ", i + 1,
                      option->label, option->description);
      }
      Serial.println("----------------------------------------");
      Serial.printf("Serial: Enter 1..%d\n", optionCount);
      Serial.println("OLED: Joystick to move, A to select");
      Serial.print("> ");
      lastPrintedSelection = selection;
    }

    if (oledDisplay && oledConnected) {
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(SSD1306_WHITE);
      oledDisplay->setCursor(20, 0);
      oledDisplay->println("FIRST-TIME SETUP");
      oledDisplay->drawFastHLine(0, 10, 128, SSD1306_WHITE);
      oledDisplay->setCursor(0, 13);
      oledDisplay->println("Select setup mode:");
      for (int i = 0; i < optionCount; ++i) {
        const SetupModeOption* option = getSetupModeOption(i);
        oledDisplay->setCursor(0, 24 + i * 10);
        oledDisplay->print(selection == i ? "> " : "  ");
        oledDisplay->println(option->label);
      }
      const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
      oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
      oledDisplay->setCursor(0, footerY + 2);
      oledDisplay->print("A:Select  Joy:Move");
      OLED_TRANSACTION(oledDisplay->display());
    }
    delay(50);

    if (oledDisplay && oledConnected) {
      JoystickNav nav = readWizardJoystickNav();
      if (nav.up) {
        selection = selection > 0 ? selection - 1 : optionCount - 1;
        delay(150);
        continue;
      }
      if (nav.down) {
        selection = selection < optionCount - 1 ? selection + 1 : 0;
        delay(150);
        continue;
      }
      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) {
        setupMode = (int)getSetupModeOption(selection)->mode;
        return true;
      }
    }
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.length() == 0) continue;  // a bare Enter is not a choice while the joystick drives the menu
      SetupMode selected;
      if (parseSetupModeInput(input, selected)) {
        setupMode = (int)selected;
        return true;
      }
    }
  }
}

// ============================================================================
// Archetype Selection (Level 2 — shown after "Basic")
// ============================================================================
// Card-per-item on OLED (name + wrapped blurb + nav), full list + blurbs on
// serial. Returns the chosen archetype index, or -1 if the user backed out
// (B button / 'b') to return to the Basic/Advanced/Import menu.
int getOLEDArchetypeSelection() {
  // Only offer archetypes whose defining feature is compiled (hides e.g.
  // "G2 Glasses Companion" without Bluetooth, "Meshed Node" without ESP-NOW).
  int avail[16];
  int NUM = 0;
  for (size_t i = 0; i < setupArchetypesCount && NUM < 16; i++) {
    if (setupArchetypeAvailable(&setupArchetypes[i])) avail[NUM++] = (int)i;
  }
  if (NUM == 0) return 0;  // unreachable: handheld/headless are always available
  int selection = 0;
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
  int lastPrintedSelection = -1;

  sJoyUpHeld = false;
  sJoyDownHeld = false;

  while (true) {
    // Serial: full list + per-option blurb, cursor on current
    if (selection != lastPrintedSelection) {
      Serial.println();
      Serial.println("========================================");
      Serial.println("    WHAT WILL YOU USE THIS DEVICE FOR?");
      Serial.println("========================================");
      for (int i = 0; i < NUM; i++) {
        const SetupArchetype& opt = setupArchetypes[avail[i]];
        Serial.printf(" %s%d. %s\n", i == selection ? ">" : " ", i + 1, opt.name);
        Serial.printf("      %s\n", opt.blurb);
      }
      Serial.println("----------------------------------------");
      Serial.printf("Serial: 1-%d, or 'b' back   OLED: Joy=move A=pick B=back\n", NUM);
      Serial.print("> ");
      lastPrintedSelection = selection;
    }

    // OLED: one card for the highlighted archetype
    if (oledDisplay && oledConnected) {
      const SetupArchetype& a = setupArchetypes[avail[selection]];
      drawSetupInfoPage(a.name, a.blurb, "Joy A:Pick B:Back", selection + 1, NUM);
    }

    delay(50);

    if (oledDisplay && oledConnected) {
      JoystickNav nav = readWizardJoystickNav();
      if (nav.up)   { selection = (selection > 0) ? selection - 1 : NUM - 1; delay(150); continue; }
      if (nav.down) { selection = (selection < NUM - 1) ? selection + 1 : 0; delay(150); continue; }

      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      if (newButtons & INPUT_MASK(INPUT_BUTTON_A)) return avail[selection];
      if (newButtons & INPUT_MASK(INPUT_BUTTON_B)) return -1;
    }

    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.equalsIgnoreCase("b") || input.equalsIgnoreCase("back")) return -1;
      int num = input.toInt();
      if (num >= 1 && num <= NUM) return avail[num - 1];
      for (int i = 0; i < NUM; i++) {
        if (input.equalsIgnoreCase(setupArchetypes[avail[i]].id)) return avail[i];
      }
    }
  }
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
      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      
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
