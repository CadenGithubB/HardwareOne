// OLED_Mode_Remote.cpp - Bond mode UI
// Menu-driven interface with device picker, Status, Sensors, and Swap Roles
// Modeled after OLED_Mode_Network.cpp menu pattern

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_TextField.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_ESPNow.h"
#include "System_BondedPeer.h"
#include "HAL_Input.h"
#include "System_MemUtil.h"   // ps_alloc — bond-picker scroll/labels live in PSRAM
#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

// =============================================================================
// Menu State
// =============================================================================

static int sBondMenuSelection = 0;
static bool sBondShowingStatus = false;
static bool sBondShowingSensors = false;
// Flat list selection: each visible sensor contributes 2 rows (Enable + Stream).
// Row index N → sensor visible-index (N/2), field (N%2: 0=Enable, 1=Stream).
// This replaces the old 2D grid with bondSensorColumn — see comment near the
// display function for the UX rationale.
static int sBondSensorSelection = 0;

// Device picker (shown when not yet bonded)
static int sBondPickerSelection = 0;
static OledScrollState* sBondPickerScroll = nullptr;
static char (*pickerLabels)[24] = nullptr;  // Heap-allocated on first use to avoid DRAM BSS overflow
static int sPickerDeviceIndices[16] = {};    // Maps scroll index -> gEspNow->devices[] index (self filtered out)
static int sPickerFilteredCount = 0;         // Number of devices after filtering self

// Main menu items
static const char* sBondMenuItems[] = {
  "Status",        // 0 - Show bond status details
  "Sensors",       // 1 - Sensor enable/stream submenu (master only)
  "Swap Roles",    // 2 - Swap master/worker
};
static const int kBondMenuItems = 3;

// Sensor definitions. The masks are the CAP_SENSOR_* bits of System_ESPNow.h
// (the one sensor numbering), never a number typed here.
struct BondSensorDef {
  const char* name;
  const char* id;       // CLI command suffix (e.g. "thermal" for open/close)
  uint32_t mask;         // CAP_SENSOR_* bit mask (32 bits, as on the wire)
  bool* streamSetting;   // Pointer to gSettings.bondStream* field
};

static const BondSensorDef kBondSensorDefs[] = {
  { "Thermal",  "thermal",  CAP_SENSOR_THERMAL,  &gSettings.bondStreamThermal },
  { "ToF",      "tof",      CAP_SENSOR_TOF,      &gSettings.bondStreamTof },
  { "IMU",      "imu",      CAP_SENSOR_IMU,      &gSettings.bondStreamImu },
#if ENABLE_ANO_ENCODER
  { "ANO Enc",  "input",    CAP_SENSOR_INPUT,    &gSettings.bondStreamInput },
#else
  { "Gamepad",  "input",    CAP_SENSOR_INPUT,    &gSettings.bondStreamInput },
#endif
  { "GPS",      "gps",      CAP_SENSOR_GPS,      &gSettings.bondStreamGps },
  { "RTC",      "rtc",      CAP_SENSOR_RTC,      &gSettings.bondStreamRtc },
  { "Presence", "presence", CAP_SENSOR_PRESENCE, &gSettings.bondStreamPresence },
};
static const int kBondSensorDefCount = sizeof(kBondSensorDefs) / sizeof(kBondSensorDefs[0]);

// Get list of visible sensors (compiled on remote device)
static int getVisibleSensors(int* indices, int maxCount) {
  int count = 0;
  if (!gEspNow || !gEspNow->lastRemoteCapValid) return 0;
  uint32_t remoteMask = gEspNow->lastRemoteCap.sensorMask;
  for (int i = 0; i < kBondSensorDefCount && count < maxCount; i++) {
    if (remoteMask & kBondSensorDefs[i].mask) {
      indices[count++] = i;
    }
  }
  return count;
}

// Check if remote sensor is enabled (running) from live peer status
static bool isRemoteSensorEnabled(uint32_t mask) {
  if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
  return (gEspNow->bondPeerStatus.sensorEnabledMask & mask) != 0;
}

// Check if remote sensor is connected (hardware detected) from live peer status
static bool isRemoteSensorConnected(uint32_t mask) {
  if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
  return (gEspNow->bondPeerStatus.sensorConnectedMask & mask) != 0;
}

// =============================================================================
// Status Display (detailed view)
// =============================================================================

// "<peer name>  [ON]" on the current text row: the name in a bounded field,
// the state right-aligned in its own column. Advances to the next row.
static void remoteDrawPeerNameRow(const char* peerName, bool online) {
  const char* state = online ? "[ON]" : "[OFF]";
  const int stateX = SCREEN_WIDTH - (int)strlen(state) * 6;
  const int ry = oledDisplay->getCursorY();
  oledTextFieldDraw(oledDisplay, 0, ry, stateX - 6, peerName, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                    false, &sBondMenuSelection);
  oledDisplay->setCursor(stateX, ry);
  oledDisplay->println(state);
}

static void displayBondStatus() {
  if (!gEspNow) return;
  
  // Get peer name
  char peerName[32];  // a device name (31 bytes) + NUL
  // BondedPeer::peerName() does the parse + name resolution and falls back
  // to the raw MAC string when no friendly name is known. Truncate to fit.
  String displayName = BondedPeer::peerName();
  strlcpy(peerName, displayName.c_str(), sizeof(peerName));
  
  char buf[32];
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  // Line 1: Peer name + online/offline
  bool online = gEspNow->bondPeerOnline;
  remoteDrawPeerNameRow(peerName, online);
  
  // Line 2: Role + sync status. A partner that said it has no bond with this
  // device is not "Syncing": nothing will come until it is bonded there too.
  const char* role = (isBondMaster()) ? "Master" : "Worker";
  bool synced = isBondSynced();
  const BondAgreementView agreement = bondAgreementView();
  const char* syncText = synced ? "Synced" : "Syncing...";
  if (agreement.state == BOND_AGREEMENT_DECLINED) syncText = "No bond there";
  else if (agreement.roleConflict) syncText = "Same role!";
  snprintf(buf, sizeof(buf), "%s %s", role, syncText);
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
    char sensorBuf[24];
    {
      String sensors = sensorMaskNames(cap.sensorMask, /*longForm=*/false);
      strncpy(sensorBuf, sensors.c_str(), 20);
      sensorBuf[20] = '\0';
    }
    oledDisplay->print("S:");
    oledDisplay->println(sensorBuf);
    snprintf(buf, sizeof(buf), "%luMB/%luMB Ch%d",
             (unsigned long)cap.flashSizeMb, (unsigned long)cap.psramSizeMb, cap.wifiChannel);
    oledDisplay->println(buf);
  } else {
    oledDisplay->println();
    oledDisplay->println("Awaiting capabilities...");
  }
}

// =============================================================================
// Sensors Submenu Display
// =============================================================================

// Flat list, one row per (sensor, field) tuple. Was a 2D grid where LEFT/
// RIGHT switched between Enable and Stream columns — but that horizontal
// nav conflicted with the ANO encoder's LEFT-as-back convention used
// everywhere else in the firmware, and it was the only page that broke
// the "scroll = up/down, action = A, back = LEFT" pattern. Flattening
// halves screen density (5 sensors fit becomes 2.5 sensors fit) but the
// rotary wheel handles long lists fine and the UI now obeys the same
// rules as every other settings list.
static void displayBondSensors() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  bool isMaster = (isBondMaster());
  if (!isMaster) {
    oledDisplay->println("Sensor control is");
    oledDisplay->println("managed by master.");
    return;
  }

  if (!isBondSynced()) {
    oledDisplay->println("Waiting for sync...");
    return;
  }

  int indices[kBondSensorDefCount];
  int visCount = getVisibleSensors(indices, kBondSensorDefCount);

  if (visCount == 0) {
    oledDisplay->println("No remote sensors");
    oledDisplay->println("available.");
    return;
  }

  // Each visible sensor contributes 2 rows: row 2k = Enable, 2k+1 = Stream.
  int rowCount = visCount * 2;

  // Clamp selection to the flattened range.
  if (sBondSensorSelection >= rowCount) sBondSensorSelection = rowCount - 1;
  if (sBondSensorSelection < 0) sBondSensorSelection = 0;

  // Visible window: one 8 px row each, as many as fit above the footer (5 on
  // the 64 px OLED, 13 on the P4X-EYE canvas; no column header, the field
  // name is on every row).
  const int maxVisible = oledVisibleRows(8);
  int scrollOffset = 0;
  if (rowCount > maxVisible) {
    scrollOffset = sBondSensorSelection - maxVisible / 2;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > rowCount - maxVisible) scrollOffset = rowCount - maxVisible;
  }

  for (int r = scrollOffset; r < rowCount && r < scrollOffset + maxVisible; r++) {
    int sensorIdx = indices[r / 2];
    bool isStreamField = (r & 1) != 0;
    const BondSensorDef& sd = kBondSensorDefs[sensorIdx];
    bool connected = isRemoteSensorConnected(sd.mask);
    bool enabled = isRemoteSensorEnabled(sd.mask);
    bool streaming = *(sd.streamSetting);

    bool isSelected = (r == sBondSensorSelection);
    const char* cursor = isSelected ? ">" : " ";
    const char* fieldLabel = isStreamField ? "Strm" : "Enbl";
    bool value = isStreamField ? streaming : enabled;

    // Fixed columns: cursor, a nine-glyph name field (the former %-9s), then
    // the field label and value, which never move under a long name.
    const int ry = oledDisplay->getCursorY();
    oledDisplay->setCursor(0, ry);
    oledDisplay->print(cursor);
    oledTextFieldDraw(oledDisplay, 6, ry, 54, sd.name, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                      isSelected, kBondSensorDefs);
    char tail[12];
    if (!connected) {
      // Disconnected: both rows show "--" instead of a value.
      snprintf(tail, sizeof(tail), "%s --", fieldLabel);
    } else {
      snprintf(tail, sizeof(tail), "%s [%s]", fieldLabel, value ? "ON " : "OFF");
    }
    oledDisplay->setCursor(60, ry);
    oledDisplay->println(tail);
  }
}

// =============================================================================
// Main Display Function
// =============================================================================

// Build device name string for display (name or MAC fallback)
static void formatDeviceLabel(char* buf, int bufLen, const EspNowDevice& dev) {
  if (dev.name.length() > 0) {
    snprintf(buf, bufLen, "%s", dev.name.c_str());
  } else {
    macToDisplay(dev.mac, buf, bufLen);
  }
}

void displayRemoteMode() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Check if bond mode is enabled
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    // Not bonded — show device picker or hint
    if (!gEspNow) {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->println("No paired devices.");
      oledDisplay->println();
      oledDisplay->println("Pair a device first:");
      oledDisplay->println(" espnow pair");
      return;
    }

    // Init scroll state and label buffer on first use (heap to save ~800 bytes DRAM BSS)
    if (!sBondPickerScroll) {
      sBondPickerScroll = (OledScrollState*)ps_alloc(sizeof(OledScrollState), AllocPref::PreferPsram, "oled.bondpicker");
      if (!sBondPickerScroll) return;
      oledScrollInit(sBondPickerScroll, nullptr, oledVisibleRows(10, 1, 4));
      oledScrollSetSplitPane(sBondPickerScroll, 128, 0, 0);
    }
    if (!pickerLabels) {
      pickerLabels = (char (*)[24])ps_alloc(16 * 24, AllocPref::PreferPsram, "oled.pickerlabels");
    }
    if (!pickerLabels) return;

    // Rebuild item list each frame (device list may change).
    // Filter out this device's own MAC — it cannot bond with itself.
    int savedSel = sBondPickerScroll->selectedIndex;
    int savedOff = sBondPickerScroll->scrollOffset;
    oledScrollClear(sBondPickerScroll);
    // ESP-NOW's cached address while it runs: this list rebuilds every frame,
    // and the driver query is a round trip to the radio co-processor on a
    // Hosted board (espnowSelfStaMac only falls back to it while ESP-NOW is
    // stopped). One read for the whole loop; zeroed so a failed read matches
    // no device.
    uint8_t selfMac[6] = {};
    (void)espnowSelfStaMac(selfMac);
    int total = min(gEspNow->deviceCount, 16);
    sPickerFilteredCount = 0;
    for (int i = 0; i < total; i++) {
      // Skip this device's own entry — it should never bond with itself
      if (memcmp(gEspNow->devices[i].mac, selfMac, 6) == 0) continue;
      sPickerDeviceIndices[sPickerFilteredCount] = i;
      formatDeviceLabel(pickerLabels[sPickerFilteredCount], sizeof(pickerLabels[0]), gEspNow->devices[i]);
      oledScrollAddItem(sBondPickerScroll, pickerLabels[sPickerFilteredCount], nullptr);
      sPickerFilteredCount++;
    }

    if (sPickerFilteredCount == 0) {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->println("No paired devices.");
      oledDisplay->println();
      oledDisplay->println("Pair a device first:");
      oledDisplay->println(" espnow pair");
      return;
    }

    sBondPickerScroll->selectedIndex = min(savedSel, max(0, sPickerFilteredCount - 1));
    sBondPickerScroll->scrollOffset = savedOff;
    sBondPickerSelection = sBondPickerScroll->selectedIndex;

    oledScrollRender(oledDisplay, sBondPickerScroll, true, true);
    return;
  }
  
  if (!gEspNow) return;
  
  // Sub-views
  if (sBondShowingStatus) {
    displayBondStatus();
    return;
  }
  
  if (sBondShowingSensors) {
    displayBondSensors();
    return;
  }
  
  // Main menu
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  // Show peer name and status on first line for context
  bool online = gEspNow->bondPeerOnline;
  char peerName[32];  // a device name (31 bytes) + NUL
  // BondedPeer::peerName() does the parse + name resolution and falls back
  // to the raw MAC string when no friendly name is known. Truncate to fit.
  String displayName = BondedPeer::peerName();
  strlcpy(peerName, displayName.c_str(), sizeof(peerName));
  // The peer name in a bounded field; the link state keeps a fixed right column.
  remoteDrawPeerNameRow(peerName, online);
  
  // Menu options
  bool isMaster = (isBondMaster());
  for (int i = 0; i < kBondMenuItems; i++) {
    // Skip Sensors for workers (they can't control)
    if (i == 1 && !isMaster) continue;
    
    const int ry = oledDisplay->getCursorY();
    const bool selected = i == sBondMenuSelection;
    oledDisplay->setCursor(0, ry);
    oledDisplay->print(selected ? "> " : "  ");
    oledTextFieldDraw(oledDisplay, 12, ry, SCREEN_WIDTH - 12, sBondMenuItems[i],
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, selected, sBondMenuItems);
    oledDisplay->println();
  }
}

// =============================================================================
// Menu Navigation
// =============================================================================

// =============================================================================
// Picker Navigation (when not bonded)
// =============================================================================

static void handlePickerInput(uint32_t newlyPressed) {
  if (!gEspNow || sPickerFilteredCount == 0) return;

  if (!sBondPickerScroll) return;
  if (oledScrollHandleNav(sBondPickerScroll)) {
    sBondPickerSelection = sBondPickerScroll->selectedIndex;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int scrollIdx = sBondPickerScroll->selectedIndex;
    if (scrollIdx >= 0 && scrollIdx < sPickerFilteredCount) {
      int devIdx = sPickerDeviceIndices[scrollIdx];
      char cmd[64];
      const EspNowDevice& dev = gEspNow->devices[devIdx];
      if (dev.name.length() > 0) {
        snprintf(cmd, sizeof(cmd), "bondconnect %s", dev.name.c_str());
      } else {
        snprintf(cmd, sizeof(cmd), "bondconnect " MAC_FMT,
                 MAC_ARGS(dev.mac));
      }
      executeOledCommandShowResult(cmd);  // the bond comes up later: toast what it did
    }
  }
}

static void bondMenuUp() {
  if (sBondShowingStatus) return;

  if (sBondShowingSensors) {
    sBondSensorSelection--;
    int indices[kBondSensorDefCount];
    int visCount = getVisibleSensors(indices, kBondSensorDefCount);
    int rowCount = visCount * 2;  // 2 rows per visible sensor (Enable, Stream)
    if (sBondSensorSelection < 0) sBondSensorSelection = rowCount - 1;
    return;
  }

  bool isMaster = (isBondMaster());
  int startIdx = sBondMenuSelection;
  do {
    sBondMenuSelection--;
    if (sBondMenuSelection < 0) sBondMenuSelection = kBondMenuItems - 1;
  } while (sBondMenuSelection != startIdx && (sBondMenuSelection == 1 && !isMaster));
}

static void bondMenuDown() {
  if (sBondShowingStatus) return;

  if (sBondShowingSensors) {
    sBondSensorSelection++;
    int indices[kBondSensorDefCount];
    int visCount = getVisibleSensors(indices, kBondSensorDefCount);
    int rowCount = visCount * 2;
    if (sBondSensorSelection >= rowCount) sBondSensorSelection = 0;
    return;
  }

  bool isMaster = (isBondMaster());
  int startIdx = sBondMenuSelection;
  do {
    sBondMenuSelection++;
    if (sBondMenuSelection >= kBondMenuItems) sBondMenuSelection = 0;
  } while (sBondMenuSelection != startIdx && (sBondMenuSelection == 1 && !isMaster));
}

// Confirm callback for swap roles — must change remote first, then local
static void swapRolesConfirmed(void* userData) {
  (void)userData;
  // If we're master, peer becomes master and we become worker (and vice versa)
  bool wasMaster = (isBondMaster());
  char cmd[48];
  // Remote first (so peer is ready before local handshake restarts). A refused
  // or unsent remote step ends the swap with its reason on the toast: this
  // device keeps its role.
  snprintf(cmd, sizeof(cmd), "remote:bondrole %s", wasMaster ? "master" : "worker");
  if (!oledRunCommand(cmd)) return;
  // Then local
  snprintf(cmd, sizeof(cmd), "bondrole %s", wasMaster ? "worker" : "master");
  executeOledCommandShowResult(cmd);  // the last step: its result is the toast
}

static void executeBondAction() {
  if (sBondShowingStatus) {
    sBondShowingStatus = false;
    return;
  }
  
  if (sBondShowingSensors) {
    // Flat list: row N/2 = sensor visible-index, N%2 = field (0 Enable, 1 Stream).
    int indices[kBondSensorDefCount];
    int visCount = getVisibleSensors(indices, kBondSensorDefCount);
    int rowCount = visCount * 2;
    if (sBondSensorSelection < 0 || sBondSensorSelection >= rowCount) return;

    int sensorIdx = indices[sBondSensorSelection / 2];
    bool isStreamField = (sBondSensorSelection & 1) != 0;
    const BondSensorDef& sd = kBondSensorDefs[sensorIdx];

    if (!isRemoteSensorConnected(sd.mask)) return;  // Can't toggle disconnected

    if (!isStreamField) {
      // Toggle enable: send open/close command to bonded device
      bool currentlyOn = isRemoteSensorEnabled(sd.mask);
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "remote:%s%s", currentlyOn ? "close" : "open", sd.id);
      // The row changes only when the bonded device reports back: toast its reply.
      executeOledCommandShowResult(cmd);
    } else {
      // Toggle stream: flip the local bondStream* setting
      bool currentlyStreaming = *(sd.streamSetting);
      char cmd[40];
      snprintf(cmd, sizeof(cmd), "bondstream%s %d", sd.id, currentlyStreaming ? 0 : 1);
      executeOledCommand(cmd);
    }
    return;
  }
  
  // Main menu actions
  switch (sBondMenuSelection) {
    case 0: // Status
      sBondShowingStatus = true;
      break;
    case 1: // Sensors
      sBondShowingSensors = true;
      sBondSensorSelection = 0;
      break;
    case 2: // Swap Roles
      oledConfirmRequest("Swap roles?", "Both devices", swapRolesConfirmed, nullptr);
      break;
  }
}

static void bondMenuBack() {
  if (sBondShowingStatus) {
    sBondShowingStatus = false;
  } else if (sBondShowingSensors) {
    sBondShowingSensors = false;
  }
}

// =============================================================================
// Input Handler (registered via OledModeEntry or called from switch-case)
// =============================================================================

bool bondModeInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Device picker (not yet bonded)
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    handlePickerInput(newlyPressed);
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) return false;  // Let parent pop mode
    return (gEspNow && sPickerFilteredCount > 0);
  }
  
  // Sensors submenu: flat list, UP/DOWN scrolls across (sensor,field) rows.
  // LEFT/RIGHT used to switch between Enable and Stream columns in the old
  // 2D grid — removed when the page was flattened so LEFT stays consistent
  // with B-back everywhere else in the firmware. A toggles the current row.
  if (sBondShowingSensors) {
    if (gNavEvents.up) { bondMenuUp(); return true; }
    if (gNavEvents.down) { bondMenuDown(); return true; }
  } else if (!sBondShowingStatus) {
    // Main menu: up/down only. LEFT/RIGHT are reserved for B-back on ANO
    // encoder — conflating them here swallowed the back press and got the
    // user stuck. Canonical pattern matching other migrated handlers.
    if (gNavEvents.up) { bondMenuUp(); return true; }
    if (gNavEvents.down) { bondMenuDown(); return true; }
  }
  
  // A/X button: Execute action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executeBondAction();
    return true;
  }
  
  // B button: Back (sub-view -> menu, or let parent handle menu -> previous mode)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (sBondShowingStatus || sBondShowingSensors) {
      bondMenuBack();
      return true;
    }
    return false;  // Let main handler pop mode
  }
  
  return false;
}

// ============================================================================
// Remote Mode Registration
// ============================================================================

static const OledModeEntry kRemoteModes[] = {
  { OLED_REMOTE, "Bond", "notify_espnow", displayRemoteMode, nullptr, bondModeInputHandler, false, -1, "A:Select  B:Back" },
};

REGISTER_OLED_MODE_MODULE(kRemoteModes, sizeof(kRemoteModes) / sizeof(kRemoteModes[0]), "Bond");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
