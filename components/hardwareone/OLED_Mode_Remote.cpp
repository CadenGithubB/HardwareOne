// OLED_Mode_Remote.cpp - Remote device UI for bond mode
// Menu-driven interface with Status, Sensors, and Swap Roles
// Modeled after OLED_Mode_Network.cpp menu pattern

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_ESPNow.h"
#include "HAL_Input.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

extern DisplayDriver* oledDisplay;
extern bool oledConnected;
extern Settings gSettings;
extern EspNowState* gEspNow;
extern NavEvents gNavEvents;

extern String getEspNowDeviceName(const uint8_t* mac);
extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);
extern bool isBondSynced();
extern bool isBondSessionTokenValid();

// =============================================================================
// Menu State
// =============================================================================

static int bondMenuSelection = 0;
static bool bondShowingStatus = false;
static bool bondShowingSensors = false;
static int bondSensorSelection = 0;  // Which sensor row is selected
static int bondSensorColumn = 0;     // 0=Enable, 1=Stream

// Main menu items
static const char* bondMenuItems[] = {
  "Status",        // 0 - Show bond status details
  "Sensors",       // 1 - Sensor enable/stream submenu (master only)
  "Swap Roles",    // 2 - Swap master/worker
};
static const int BOND_MENU_ITEMS = 3;

// Sensor definitions matching capability masks in System_ESPNow.h
struct BondSensorDef {
  const char* name;
  const char* id;       // CLI command suffix (e.g. "thermal" for open/close)
  uint16_t mask;         // CAP_SENSOR_* bit mask
  bool* streamSetting;   // Pointer to gSettings.bondStream* field
};

static BondSensorDef bondSensorDefs[] = {
  { "Thermal",  "thermal",  0x01, &gSettings.bondStreamThermal },
  { "ToF",      "tof",      0x02, &gSettings.bondStreamTof },
  { "IMU",      "imu",      0x04, &gSettings.bondStreamImu },
  { "Gamepad",  "gamepad",  0x08, &gSettings.bondStreamGamepad },
  { "GPS",      "gps",      0x20, &gSettings.bondStreamGps },
  { "RTC",      "rtc",      0x40, &gSettings.bondStreamRtc },
  { "Presence", "presence", 0x80, &gSettings.bondStreamPresence },
};
static const int BOND_SENSOR_DEF_COUNT = sizeof(bondSensorDefs) / sizeof(bondSensorDefs[0]);

// Get list of visible sensors (compiled on remote device)
static int getVisibleSensors(int* indices, int maxCount) {
  int count = 0;
  if (!gEspNow || !gEspNow->lastRemoteCapValid) return 0;
  uint16_t remoteMask = gEspNow->lastRemoteCap.sensorMask;
  for (int i = 0; i < BOND_SENSOR_DEF_COUNT && count < maxCount; i++) {
    if (remoteMask & bondSensorDefs[i].mask) {
      indices[count++] = i;
    }
  }
  return count;
}

// Check if remote sensor is enabled (running) from live peer status
static bool isRemoteSensorEnabled(uint16_t mask) {
  if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
  return (gEspNow->bondPeerStatus.sensorEnabledMask & mask) != 0;
}

// Check if remote sensor is connected (hardware detected) from live peer status
static bool isRemoteSensorConnected(uint16_t mask) {
  if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
  return (gEspNow->bondPeerStatus.sensorConnectedMask & mask) != 0;
}

// =============================================================================
// Status Display (detailed view)
// =============================================================================

static void displayBondStatus() {
  if (!gEspNow) return;
  
  // Get peer name
  uint8_t peerMac[6];
  String peerName = gSettings.bondPeerMac;
  if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    String name = getEspNowDeviceName(peerMac);
    if (name.length() > 0) peerName = name;
  }
  
  char buf[32];
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  // Line 1: Peer name + online/offline
  bool online = gEspNow->bondPeerOnline;
  oledDisplay->print(peerName.substring(0, 14));
  oledDisplay->println(online ? " [ON]" : " [OFF]");
  
  // Line 2: Role + sync status
  const char* role = (gSettings.bondRole == 1) ? "Master" : "Worker";
  bool synced = isBondSynced();
  snprintf(buf, sizeof(buf), "%s %s", role, synced ? "Synced" : "Syncing...");
  oledDisplay->println(buf);
  
  // Line 3: RSSI + heartbeats
  snprintf(buf, sizeof(buf), "RSSI:%ddBm HB:%lu/%lu",
           (int)gEspNow->bondRssiAvg,
           (unsigned long)gEspNow->bondHeartbeatsReceived,
           (unsigned long)gEspNow->bondHeartbeatsSent);
  oledDisplay->println(buf);
  
  // Line 4: Last seen + peer uptime
  if (online && gEspNow->lastBondHeartbeatReceivedMs > 0) {
    unsigned long ageSec = (millis() - gEspNow->lastBondHeartbeatReceivedMs) / 1000;
    uint32_t up = gEspNow->bondPeerUptime;
    if (up >= 3600) {
      snprintf(buf, sizeof(buf), "Seen:%lus Up:%luh%lum", ageSec, (unsigned long)(up/3600), (unsigned long)((up%3600)/60));
    } else if (up >= 60) {
      snprintf(buf, sizeof(buf), "Seen:%lus Up:%lum%lus", ageSec, (unsigned long)(up/60), (unsigned long)(up%60));
    } else {
      snprintf(buf, sizeof(buf), "Seen:%lus Up:%lus", ageSec, (unsigned long)up);
    }
    oledDisplay->println(buf);
  } else {
    oledDisplay->println("Last seen: never");
  }
  
  // Line 5-6: Remote capabilities summary
  if (gEspNow->lastRemoteCapValid) {
    CapabilitySummary& cap = gEspNow->lastRemoteCap;
    String sensors = getCapabilityListShort(cap.sensorMask, SENSOR_NAMES);
    oledDisplay->print("S:");
    oledDisplay->println(sensors.substring(0, 20));
    snprintf(buf, sizeof(buf), "%luMB/%luMB Ch%d",
             (unsigned long)cap.flashSizeMB, (unsigned long)cap.psramSizeMB, cap.wifiChannel);
    oledDisplay->println(buf);
  } else {
    oledDisplay->println();
    oledDisplay->println("Awaiting capabilities...");
  }
}

// =============================================================================
// Sensors Submenu Display
// =============================================================================

static void displayBondSensors() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  bool isMaster = (gSettings.bondRole == 1);
  if (!isMaster) {
    oledDisplay->println("Sensor control is");
    oledDisplay->println("managed by master.");
    return;
  }
  
  if (!isBondSynced()) {
    oledDisplay->println("Waiting for sync...");
    return;
  }
  
  int indices[BOND_SENSOR_DEF_COUNT];
  int visCount = getVisibleSensors(indices, BOND_SENSOR_DEF_COUNT);
  
  if (visCount == 0) {
    oledDisplay->println("No remote sensors");
    oledDisplay->println("available.");
    return;
  }
  
  // Clamp selection
  if (bondSensorSelection >= visCount) bondSensorSelection = visCount - 1;
  if (bondSensorSelection < 0) bondSensorSelection = 0;
  
  // Header row
  //          "Sensor     Enbl Strm"
  oledDisplay->println("Sensor     Enbl Strm");
  
  // Calculate visible window (up to 5 rows fit below header)
  const int maxVisible = 5;
  int scrollOffset = 0;
  if (visCount > maxVisible) {
    scrollOffset = bondSensorSelection - maxVisible / 2;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > visCount - maxVisible) scrollOffset = visCount - maxVisible;
  }
  
  for (int v = scrollOffset; v < visCount && v < scrollOffset + maxVisible; v++) {
    int si = indices[v];
    BondSensorDef& sd = bondSensorDefs[si];
    bool connected = isRemoteSensorConnected(sd.mask);
    bool enabled = isRemoteSensorEnabled(sd.mask);
    bool streaming = *(sd.streamSetting);
    
    bool isSelected = (v == bondSensorSelection);
    char line[22];
    
    // Build: "  Name      [x] [x]" or "> Name      [x] [x]"
    // Column highlight: underline or bracket the active column
    const char* enblMark = enabled ? "ON " : "OFF";
    const char* strmMark = streaming ? "ON " : "OFF";
    
    if (!connected) {
      // Disconnected sensor: show dashes
      snprintf(line, sizeof(line), "%s%-10s --  --",
               isSelected ? ">" : " ", sd.name);
    } else if (isSelected) {
      // Selected row: show cursor and highlight active column
      if (bondSensorColumn == 0) {
        snprintf(line, sizeof(line), ">%-10s[%s] %s",
                 sd.name, enblMark, strmMark);
      } else {
        snprintf(line, sizeof(line), ">%-10s %s [%s]",
                 sd.name, enblMark, strmMark);
      }
    } else {
      snprintf(line, sizeof(line), " %-10s %s  %s",
               sd.name, enblMark, strmMark);
    }
    oledDisplay->println(line);
  }
}

// =============================================================================
// Main Display Function
// =============================================================================

void displayRemoteMode() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Check if bond mode is enabled
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Not bonded.");
    oledDisplay->println();
    oledDisplay->println("Use CLI:");
    oledDisplay->println("  bond connect <device>");
    return;
  }
  
  if (!gEspNow) return;
  
  // Sub-views
  if (bondShowingStatus) {
    displayBondStatus();
    return;
  }
  
  if (bondShowingSensors) {
    displayBondSensors();
    return;
  }
  
  // Main menu
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  // Show peer name and status on first line for context
  bool online = gEspNow->bondPeerOnline;
  uint8_t peerMac[6];
  String peerName = gSettings.bondPeerMac;
  if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    String name = getEspNowDeviceName(peerMac);
    if (name.length() > 0) peerName = name;
  }
  oledDisplay->print(peerName.substring(0, 14));
  oledDisplay->println(online ? " [ON]" : " [OFF]");
  
  // Menu options
  bool isMaster = (gSettings.bondRole == 1);
  for (int i = 0; i < BOND_MENU_ITEMS; i++) {
    // Skip Sensors for workers (they can't control)
    if (i == 1 && !isMaster) continue;
    
    if (i == bondMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->println(bondMenuItems[i]);
  }
}

// =============================================================================
// Menu Navigation
// =============================================================================

static void bondMenuUp() {
  if (bondShowingStatus) return;
  
  if (bondShowingSensors) {
    bondSensorSelection--;
    int indices[BOND_SENSOR_DEF_COUNT];
    int visCount = getVisibleSensors(indices, BOND_SENSOR_DEF_COUNT);
    if (bondSensorSelection < 0) bondSensorSelection = visCount - 1;
    return;
  }
  
  bool isMaster = (gSettings.bondRole == 1);
  int startIdx = bondMenuSelection;
  do {
    bondMenuSelection--;
    if (bondMenuSelection < 0) bondMenuSelection = BOND_MENU_ITEMS - 1;
  } while (bondMenuSelection != startIdx && (bondMenuSelection == 1 && !isMaster));
}

static void bondMenuDown() {
  if (bondShowingStatus) return;
  
  if (bondShowingSensors) {
    bondSensorSelection++;
    int indices[BOND_SENSOR_DEF_COUNT];
    int visCount = getVisibleSensors(indices, BOND_SENSOR_DEF_COUNT);
    if (bondSensorSelection >= visCount) bondSensorSelection = 0;
    return;
  }
  
  bool isMaster = (gSettings.bondRole == 1);
  int startIdx = bondMenuSelection;
  do {
    bondMenuSelection++;
    if (bondMenuSelection >= BOND_MENU_ITEMS) bondMenuSelection = 0;
  } while (bondMenuSelection != startIdx && (bondMenuSelection == 1 && !isMaster));
}

static void bondSensorLeft() {
  if (bondSensorColumn > 0) bondSensorColumn--;
}

static void bondSensorRight() {
  if (bondSensorColumn < 1) bondSensorColumn++;
}

// Confirm callback for swap roles — must change remote first, then local
static void swapRolesConfirmed(void* userData) {
  (void)userData;
  // If we're master, peer becomes master and we become worker (and vice versa)
  bool wasMaster = (gSettings.bondRole == 1);
  char cmd[48];
  // Remote first (so peer is ready before local handshake restarts)
  snprintf(cmd, sizeof(cmd), "remote:bond role %s", wasMaster ? "master" : "worker");
  executeOLEDCommand(cmd);
  // Then local
  snprintf(cmd, sizeof(cmd), "bond role %s", wasMaster ? "worker" : "master");
  executeOLEDCommand(cmd);
}

static void executeBondAction() {
  if (bondShowingStatus) {
    bondShowingStatus = false;
    return;
  }
  
  if (bondShowingSensors) {
    // Toggle the selected sensor's enable or stream setting
    int indices[BOND_SENSOR_DEF_COUNT];
    int visCount = getVisibleSensors(indices, BOND_SENSOR_DEF_COUNT);
    if (bondSensorSelection < 0 || bondSensorSelection >= visCount) return;
    
    int si = indices[bondSensorSelection];
    BondSensorDef& sd = bondSensorDefs[si];
    
    if (!isRemoteSensorConnected(sd.mask)) return;  // Can't toggle disconnected
    
    if (bondSensorColumn == 0) {
      // Toggle enable: send open/close command to bonded device
      bool currentlyOn = isRemoteSensorEnabled(sd.mask);
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "remote:%s%s", currentlyOn ? "close" : "open", sd.id);
      executeOLEDCommand(cmd);
    } else {
      // Toggle stream: flip the local bondStream* setting
      bool currentlyStreaming = *(sd.streamSetting);
      char cmd[40];
      snprintf(cmd, sizeof(cmd), "bondstream%s %d", sd.id, currentlyStreaming ? 0 : 1);
      executeOLEDCommand(cmd);
    }
    return;
  }
  
  // Main menu actions
  switch (bondMenuSelection) {
    case 0: // Status
      bondShowingStatus = true;
      break;
    case 1: // Sensors
      bondShowingSensors = true;
      bondSensorSelection = 0;
      bondSensorColumn = 0;
      break;
    case 2: // Swap Roles
      oledConfirmRequest("Swap roles?", "Both devices", swapRolesConfirmed, nullptr);
      break;
  }
}

static void bondMenuBack() {
  if (bondShowingStatus) {
    bondShowingStatus = false;
  } else if (bondShowingSensors) {
    bondShowingSensors = false;
  }
}

// =============================================================================
// Input Handler (registered via OLEDModeEntry or called from switch-case)
// =============================================================================

bool bondModeInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Sensors submenu: left/right moves between Enable/Stream columns
  if (bondShowingSensors) {
    if (gNavEvents.left) { bondSensorLeft(); return true; }
    if (gNavEvents.right) { bondSensorRight(); return true; }
    if (gNavEvents.up) { bondMenuUp(); return true; }
    if (gNavEvents.down) { bondMenuDown(); return true; }
  } else if (!bondShowingStatus) {
    // Main menu: up/down navigation
    if (gNavEvents.up || gNavEvents.left) { bondMenuUp(); return true; }
    if (gNavEvents.down || gNavEvents.right) { bondMenuDown(); return true; }
  }
  
  // A/X button: Execute action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executeBondAction();
    return true;
  }
  
  // B button: Back (sub-view -> menu, or let parent handle menu -> previous mode)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (bondShowingStatus || bondShowingSensors) {
      bondMenuBack();
      return true;
    }
    return false;  // Let main handler pop mode
  }
  
  return false;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
