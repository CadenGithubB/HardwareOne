// OLED_Mode_Bluetooth.cpp - Bluetooth / G2-glasses OLED modes
//
// Umbrella-compliant (matches OLED_Mode_Power / OLED_Mode_Network): each screen
// is its own OLED mode — no bluetoothInG2Menu / gBluetoothShowingStatus flag-soup.
// Menus are OLEDScrollState models; status screens and the G2 submenu are pushed
// sub-modes entered via requestOLEDMode and popped by the global B handler.
//   OLED_BLUETOOTH            main menu
//   OLED_BLUETOOTH_STATUS     BT status detail (pushed)
//   OLED_BLUETOOTH_G2         G2 glasses submenu (pushed, ENABLE_G2_GLASSES)
//   OLED_BLUETOOTH_G2_STATUS  G2 status detail (pushed, ENABLE_G2_GLASSES)
//
// (Moved out of Bluetooth.cpp so every OLED mode lives in its own OLED_Mode_*.cpp.
//  BLE internals are reached through Bluetooth.h; the recent-message buffer stays
//  file-local in Bluetooth.cpp behind bleGetRecentMessages().)

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Settings.h"
#include "Bluetooth.h"
#include "OLED_SettingsEditor.h"  // openSettingsEditorForModule
#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"
#endif

// ---- Main menu model -------------------------------------------------------
enum BtAction {
  BT_ACT_STATUS,
  BT_ACT_SETTINGS,
  BT_ACT_TOGGLE,
  BT_ACT_G2,
  BT_ACT_ADVERTISING,
  BT_ACT_DISCONNECT,
};
static OLEDScrollState sBtMenuScroll;
static bool sBtMenuInit = false;
static BtAction sBtActions[OLED_SCROLL_MAX_ITEMS];

// Confirmation callback for Bluetooth Start/Stop
static void bluetoothToggleConfirmedMenu(void* userData) {
  (void)userData;
  if (gBLEState && gBLEState->initialized) {
    deinitBluetooth();
  } else {
    // Pause sensor polling during BLE init to avoid interrupt contention
    extern volatile bool gSensorPollingPaused;
    bool wasPaused = gSensorPollingPaused;
    gSensorPollingPaused = true;
    vTaskDelay(pdMS_TO_TICKS(50));
    initBluetooth();
    startBLEAdvertising();
    gSensorPollingPaused = wasPaused;
  }
}

static void addBtItem(const char* label, BtAction act) {
  if (oledScrollAddItem(&sBtMenuScroll, label)) {
    int idx = sBtMenuScroll.itemCount - 1;
    if (idx >= 0 && idx < (int)(sizeof(sBtActions) / sizeof(sBtActions[0]))) {
      sBtActions[idx] = act;
    }
  }
}

static void populateBtMenu() {
  if (!sBtMenuInit) {
    oledScrollInit(&sBtMenuScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sBtMenuInit = true;
  }
  oledScrollClearKeepSelection(&sBtMenuScroll);
  bool inited = (gBLEState && gBLEState->initialized);
  bool connected = inited && (gBLEState->connectionState == BLE_STATE_CONNECTED);

  addBtItem("Status", BT_ACT_STATUS);
  addBtItem("Settings", BT_ACT_SETTINGS);
  addBtItem("Start/Stop", BT_ACT_TOGGLE);
#if ENABLE_G2_GLASSES
  addBtItem("G2 Glasses >>", BT_ACT_G2);
#endif
  if (inited)    addBtItem("Advertising", BT_ACT_ADVERTISING);  // only when BT is up
  if (connected) addBtItem("Disconnect", BT_ACT_DISCONNECT);    // only when a client is connected

  oledScrollClampSelection(&sBtMenuScroll);
}

// ---- OLED_BLUETOOTH_STATUS: BT status detail (read-only, pushed) -----------
static void displayBluetoothStatusDetail() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  if (!gBLEState || !gBLEState->initialized) {
    oledDisplay->println("BLE: Disabled");
    oledDisplay->println();
    oledDisplay->println("Select Start/Stop");
    oledDisplay->println("to enable");
    return;
  }

  // Device name (truncate if needed)
  oledDisplay->print("Name: ");
  const char* name = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName.c_str() : "HardwareOne";
  size_t nameLen = strlen(name);
  if (nameLen > 12) {
    char truncated[13];
    strncpy(truncated, name, 11);
    truncated[11] = '~';
    truncated[12] = '\0';
    oledDisplay->println(truncated);
  } else {
    oledDisplay->println(name);
  }

  oledDisplay->print("State: ");
  if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
    oledDisplay->println("Advertising");
  } else {
    oledDisplay->println(getBLEStateString());
  }

  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    oledDisplay->print("Clients: ");
    oledDisplay->print(gBLEState->activeConnectionCount);
    oledDisplay->print("/");
    oledDisplay->println(BLE_MAX_CONNECTIONS);

    oledDisplay->print("Rx:");
    oledDisplay->print(gBLEState->commandsReceived);
    oledDisplay->print(" Tx:");
    oledDisplay->println(gBLEState->responsesSent);
    oledDisplay->println("Peers:");
    int shown = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS && shown < 3; i++) {
      if (!gBLEState->connections[i].active) continue;
      uint32_t duration = (millis() - gBLEState->connections[i].connectedSince) / 1000;
      oledDisplay->print("#");
      oledDisplay->print(i);
      oledDisplay->print(" ");
      oledDisplay->print(gBLEState->connections[i].deviceName.c_str());
      oledDisplay->print(" ");
      oledDisplay->print(duration);
      oledDisplay->println("s");
      shown++;
    }
  } else {
    oledDisplay->print("TX Power: ");
    oledDisplay->println(gSettings.bleTxPower);
    oledDisplay->print("Total: ");
    oledDisplay->println(gBLEState->totalConnections);
  }

  oledDisplay->print("Streams: ");
  bool sensorsOn = (gBLEState->streamFlags & BLE_STREAM_SENSORS);
  bool systemOn = (gBLEState->streamFlags & BLE_STREAM_SYSTEM);
  bool eventsOn = (gBLEState->streamFlags & BLE_STREAM_EVENTS);
  oledDisplay->print(sensorsOn ? "S" : "s");
  oledDisplay->print(systemOn ? " Sys" : " sys");
  oledDisplay->print(eventsOn ? " Ev" : " ev");
  oledDisplay->println();
  oledDisplay->print("Int:");
  oledDisplay->print(gBLEState->sensorStreamInterval);
  oledDisplay->print("/");
  oledDisplay->println(gBLEState->systemStreamInterval);

  // Recent messages (newest first) via the Bluetooth.cpp accessor.
  const char* recent[2];
  int nRecent = bleGetRecentMessages(recent, 2);
  if (nRecent > 0) {
    oledDisplay->println();
    oledDisplay->println("Last:");
    for (int i = 0; i < nRecent; i++) oledDisplay->println(recent[i]);
  }
}

// ---- OLED_BLUETOOTH: main menu (display + input) ---------------------------
static void displayBluetoothStatus() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);

  populateBtMenu();

  // Decorated single-line render from the scroll state (honors the scroll
  // window), with the original inline state indicators.
  const int lineHeight = 8;
  int visEnd = sBtMenuScroll.scrollOffset + sBtMenuScroll.visibleLines;
  if (visEnd > sBtMenuScroll.itemCount) visEnd = sBtMenuScroll.itemCount;
  int row = 0;
  for (int i = sBtMenuScroll.scrollOffset; i < visEnd; i++, row++) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + row * lineHeight);
    oledDisplay->print(i == sBtMenuScroll.selectedIndex ? "> " : "  ");
    if (sBtMenuScroll.items[i].line1) oledDisplay->print(sBtMenuScroll.items[i].line1);
    switch (sBtActions[i]) {
      case BT_ACT_TOGGLE:
        if (gBLEState && gBLEState->initialized) oledDisplay->print(" *");
        break;
#if ENABLE_G2_GLASSES
      case BT_ACT_G2:
        if (isG2Connected()) oledDisplay->print(" *");
        break;
#endif
      case BT_ACT_ADVERTISING:
        if (gBLEState && gBLEState->connectionState == BLE_STATE_ADVERTISING) oledDisplay->print(" *");
        break;
      default:
        break;
    }
  }

  // Scroll indicators
  if (sBtMenuScroll.scrollOffset > 0) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
    oledDisplay->print("\x18");
  }
  if (visEnd < sBtMenuScroll.itemCount) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y + (sBtMenuScroll.visibleLines - 1) * lineHeight);
    oledDisplay->print("\x19");
  }
}

static bool bluetoothInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledScrollHandleNav(&sBtMenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int idx = sBtMenuScroll.selectedIndex;
    if (idx < 0 || idx >= sBtMenuScroll.itemCount) return true;
    switch (sBtActions[idx]) {
      case BT_ACT_STATUS:
        requestOLEDMode(OLED_BLUETOOTH_STATUS, "bluetooth.status");
        break;
      case BT_ACT_SETTINGS:
        if (openSettingsEditorForModule("bluetooth")) {
          requestOLEDMode(OLED_SETTINGS, "bluetooth.settings");
        }
        break;
      case BT_ACT_TOGGLE:
        if (gBLEState && gBLEState->initialized) {
          oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr, false);
        } else {
          oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr);
        }
        break;
#if ENABLE_G2_GLASSES
      case BT_ACT_G2:
        requestOLEDMode(OLED_BLUETOOTH_G2, "bluetooth.g2");
        break;
#endif
      case BT_ACT_ADVERTISING:
        if (gBLEState && gBLEState->initialized) {
          if (gBLEState->connectionState == BLE_STATE_ADVERTISING) stopBLEAdvertising();
          else startBLEAdvertising();
        }
        break;
      case BT_ACT_DISCONNECT:
        if (gBLEState && gBLEState->initialized &&
            gBLEState->connectionState == BLE_STATE_CONNECTED) {
          disconnectBLE();
        }
        break;
      default:
        break;
    }
    return true;
  }

  // B: return false so the global handler pops the mode stack.
  return false;
}

#if ENABLE_G2_GLASSES
// ---- OLED_BLUETOOTH_G2: G2 glasses submenu --------------------------------
static char g2TextInputBuffer[64] = "Hello from ESP32!";  // fixed "Show Text" payload

static OLEDScrollState sG2MenuScroll;
static bool sG2MenuInit = false;
// Index → action: 0 Connect, 1 Disconnect, 2 Status, 3 Show Text, 4 Nav Mode
static const char* kG2MenuItems[] = { "Connect", "Disconnect", "Status", "Show Text", "Nav Mode" };

static void populateG2Menu() {
  if (!sG2MenuInit) {
    oledScrollInit(&sG2MenuScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sG2MenuInit = true;
  }
  oledScrollClearKeepSelection(&sG2MenuScroll);
  for (unsigned i = 0; i < sizeof(kG2MenuItems) / sizeof(kG2MenuItems[0]); i++) {
    oledScrollAddItem(&sG2MenuScroll, kG2MenuItems[i]);
  }
  oledScrollClampSelection(&sG2MenuScroll);
}

static void displayG2Menu() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  // Header with connection status
  oledDisplay->print("G2 GLASSES ");
  if (isG2Connected()) {
    oledDisplay->println("[OK]");
  } else if (isG2ClientInitialized()) {
    G2State state = getG2State();
    if (state == G2_STATE_SCANNING) {
      oledDisplay->println("[SCAN]");
    } else if (state == G2_STATE_CONNECTING || state == G2_STATE_AUTHENTICATING) {
      oledDisplay->println("[...]");
    } else {
      oledDisplay->println("[--]");
    }
  } else {
    oledDisplay->println("[OFF]");
  }

  populateG2Menu();

  extern bool gG2MenuNavEnabled;
  for (int i = 0; i < sG2MenuScroll.itemCount; i++) {
    oledDisplay->print(i == sG2MenuScroll.selectedIndex ? "> " : "  ");
    if (sG2MenuScroll.items[i].line1) oledDisplay->print(sG2MenuScroll.items[i].line1);
    if (i == 0 && isG2Connected()) oledDisplay->print(" *");        // Connect
    else if (i == 4 && gG2MenuNavEnabled) oledDisplay->print(" *"); // Nav Mode
    oledDisplay->println();
  }
}

static bool g2InputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledScrollHandleNav(&sG2MenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sG2MenuScroll.selectedIndex) {
      case 0:  // Connect
        if (!isG2Connected()) {
          if (!isG2ClientInitialized()) initG2Client();
          g2Connect(G2_EYE_AUTO);
        }
        break;
      case 1:  // Disconnect
        if (isG2Connected()) g2Disconnect();
        break;
      case 2:  // Status
        requestOLEDMode(OLED_BLUETOOTH_G2_STATUS, "bluetooth.g2.status");
        break;
      case 3:  // Show Text (fixed payload)
        if (isG2Connected()) g2ShowText(g2TextInputBuffer);
        break;
      case 4:  // Nav Mode toggle
        { extern bool gG2MenuNavEnabled; gG2MenuNavEnabled = !gG2MenuNavEnabled; }
        break;
    }
    return true;
  }
  return false;  // B: global pop
}

// ---- OLED_BLUETOOTH_G2_STATUS: G2 status detail (read-only, pushed) --------
static void displayG2StatusDetail() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  oledDisplay->println("== G2 GLASSES ==");
  oledDisplay->println();
  oledDisplay->print("State: ");
  oledDisplay->println(getG2StateString());

  if (isG2Connected()) {
    char statusBuf[64];
    getG2Status(statusBuf, sizeof(statusBuf));
    oledDisplay->println(statusBuf);
    extern bool gG2MenuNavEnabled;
    oledDisplay->print("Nav Mode: ");
    oledDisplay->println(gG2MenuNavEnabled ? "ON" : "OFF");
  } else {
    oledDisplay->println();
    oledDisplay->println("Not connected.");
    oledDisplay->println("Use Connect to");
    oledDisplay->println("pair glasses.");
  }
}
#endif // ENABLE_G2_GLASSES

// Availability check for Bluetooth OLED mode
static bool bluetoothOLEDModeAvailable(String* /*outReason*/) {
  return true;  // Always show in menu
}

// ---- Registration ----------------------------------------------------------
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry bluetoothOLEDModes[] = {
  { OLED_BLUETOOTH,        "Bluetooth", "bt_idle", displayBluetoothStatus,
    bluetoothOLEDModeAvailable, bluetoothInputHandler, true,  45, "A:Select B:Back" },
  { OLED_BLUETOOTH_STATUS, "BT Status", "bt_idle", displayBluetoothStatusDetail,
    nullptr, nullptr,                                  false, -1, "B:Back" },
#if ENABLE_G2_GLASSES
  { OLED_BLUETOOTH_G2,        "G2",        "bt_idle", displayG2Menu,
    nullptr, g2InputHandler,                           false, -1, "A:Select B:Back" },
  { OLED_BLUETOOTH_G2_STATUS, "G2 Status", "bt_idle", displayG2StatusDetail,
    nullptr, nullptr,                                  false, -1, "B:Back" },
#endif
};

REGISTER_OLED_MODE_MODULE(bluetoothOLEDModes, sizeof(bluetoothOLEDModes) / sizeof(bluetoothOLEDModes[0]), "Bluetooth");

#endif // ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH
