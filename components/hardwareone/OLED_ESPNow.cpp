#include "OLED_ESPNow.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sessions.h"   // sendStatusGet / SendStatusState — post-send delivery indicator
#include "System_MeshPeers.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif

// Mesh health data for online/offline status display
// gMeshPeers, gMeshPeerMeta, gMeshPeerSlots, isMeshPeerAlive, getMeshPeerHealth,
// getMeshPeerMeta are all declared in System_ESPNow.h (already included above).
// Compound queries (MeshPeers::isHealthy / displayName / ...) live in
// System_MeshPeers.h.

// =============================================================================
// OLED ESP-NOW Interface Implementation
// =============================================================================

// Main menu items (Bluetooth-style)
static const char* espnowMenuItems[] = {
  "Status",      // 0 - Network status overview
  "Devices",     // 1 - Device list with filter/sort
  "Rooms",       // 2 - Room-based device grouping
  "Settings",    // 3 - Local device settings
  "Start/Stop",  // 4 - Toggle ESP-NOW on/off
  "Pairing"      // 5 - Enter pairing mode
};
static const int ESPNOW_MENU_ITEM_COUNT = 6;

// Global state
OLEDEspNowState gOledEspNowState;

// Sent text is now recorded in the SHARED per-peer history (sent[] ring) at the
// cmd_espnow_send chokepoint, so the OLED no longer keeps a private sent ring —
// it reads the unified conversation via espnowGetConversation(). This is what
// lets the OLED see messages sent from the web/BLE too, not just ones typed here.

// Per-row direction/status metadata, kept in lockstep with messageList.items by
// oledEspNowRefreshMessages and read by the device-detail renderer (left=received,
// right=sent + live delivery status).
// PSRAM .bss: CPU-only row metadata, written/read solely on the OLED task.
EXT_RAM_BSS_ATTR static struct { bool isSent; uint32_t msgId; uint8_t sendState; } gOledRowMeta[OLED_SCROLL_MAX_ITEMS];

// =============================================================================
// Pairing view — WPS-style toggle (see espnowpairmode / espnowPairMode* core)
// =============================================================================
// Open the timed pairing window on BOTH same-mesh devices; they broadcast a
// discovery beacon and auto secure-pair each other. This screen is just the
// on/off toggle + countdown + a live list of what has paired in. All discovery
// and pairing logic lives in System_ESPNow.cpp; the OLED only drives the window.

// --- Conversation (device-detail) message-list geometry ---
// The conversation view relies on the GLOBAL header bar (title "ESPNOW: Text"),
// so message rows start at OLED_CONTENT_START_Y like the sibling ESP-NOW views
// and stack down to the GLOBAL footer band. The footer top is
// OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT (the exact value drawOLEDFooter() uses),
// so deriving the visible-row count and the render/scrollbar bounds from it here
// keeps the shared scroll window (oledScrollUp/Down + the follow-tail restore) in
// lockstep with what actually fits. Hardcoding it (the old visibleLines=3 / yPos<56)
// let a scrolled-to row spill into and below the footer, which also made the list
// look stuck because the cursor lived in a slot that wasn't physically on screen.
static constexpr int kMsgListTopY  = OLED_CONTENT_START_Y;  // first row Y, below the global header
static constexpr int kMsgRowHeight = 16;  // two 8px lines per message row
static inline int oledEspNowMsgFooterTopY() { return OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT; }
static inline int oledEspNowMsgVisibleRows() {
  return max(1, (oledEspNowMsgFooterTopY() - kMsgListTopY) / kMsgRowHeight);
}

// Title shown in the GLOBAL header bar (oledRenderHeader) for OLED_ESPNOW. Only
// the conversation (device-detail) view customizes it — "ESPNOW: Text/Remote/
// File" — so the interaction mode lives in the header instead of a clipped
// in-body "Mode:" line. All other ESP-NOW sub-views return nullptr so the caller
// falls back to the plain "ESP-NOW" mode name.
const char* oledEspNowHeaderTitle() {
  if (gOledEspNowState.currentView != ESPNOW_VIEW_DEVICE_DETAIL) return nullptr;
  switch (gOledEspNowState.interactionMode) {
    case ESPNOW_MODE_TEXT:   return "ESPNOW: Text";
    case ESPNOW_MODE_REMOTE: return "ESPNOW: Remote";
    case ESPNOW_MODE_FILE:   return "ESPNOW: File";
  }
  return nullptr;
}

void oledEspNowInit() {
  gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
  gOledEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  gOledEspNowState.modeSelectorIndex = 0;
  gOledEspNowState.modeSelectorActive = false;
  gOledEspNowState.fileChooserIndex = 0;
  gOledEspNowState.lastUpdate = 0;
  gOledEspNowState.needsRefresh = true;
  memset(gOledEspNowState.selectedDeviceMac, 0, 6);
  gOledEspNowState.selectedDeviceName = "";
  
  // Text mode state
  gOledEspNowState.textMessageBuffer = "";
  
  // Remote mode state
  gOledEspNowState.remoteFormField = 0;
  gOledEspNowState.remoteUsername = "";
  gOledEspNowState.remotePassword = "";
  gOledEspNowState.remoteCommand = "";
  
  // Initialize scrolling lists
  oledScrollInit(&gOledEspNowState.deviceList, "ESP-NOW Devices", 3);
  oledScrollInit(&gOledEspNowState.messageList, nullptr, 3);
  // Truthful visible-row count (footer-aware) so the shared scroll helpers and the
  // follow-tail restore never push the selection into/below the footer.
  gOledEspNowState.messageList.visibleLines = oledEspNowMsgVisibleRows();
  
  // Settings menu state (local)
  gOledEspNowState.settingsMenuIndex = 0;
  gOledEspNowState.settingsEditField = -1;
  
  // Device config menu state (remote)
  gOledEspNowState.deviceConfigMenuIndex = 0;
  gOledEspNowState.deviceConfigEditField = -1;
  
  // Device list filtering and sorting
  gOledEspNowState.filterMode = 0;  // All devices
  gOledEspNowState.sortMode = 0;    // Sort by name
  memset(gOledEspNowState.filterValue, 0, sizeof(gOledEspNowState.filterValue));
  
  // Main menu state (Bluetooth-style)
  gOledEspNowState.mainMenuSelection = 0;
  gOledEspNowState.mainMenuScrollOffset = 0;
  gOledEspNowState.showingStatusDetail = false;
  
  // Rooms view state
  gOledEspNowState.roomsMenuSelection = 0;
  gOledEspNowState.roomsDeviceSelection = 0;
  gOledEspNowState.inRoomDeviceList = false;
  
  // Start at main menu if ESP-NOW is initialized
  if (gEspNow && gEspNow->initialized) {
    gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
  }
}

void oledEspNowShowInitPrompt() {
  gOledEspNowState.currentView = ESPNOW_VIEW_INIT_PROMPT;
}

void oledEspNowShowNameKeyboard() {
  gOledEspNowState.currentView = ESPNOW_VIEW_NAME_KEYBOARD;
  const char* initialText = "";
  if (gSettings.espnowDeviceName.length() > 0) {
    initialText = gSettings.espnowDeviceName.c_str();
  }
  oledKeyboardInit("Device Name:", initialText, 20);
}

void oledEspNowDisplay(Adafruit_SSD1306* display) {
  if (!display) return;

  if (gOledEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT && gEspNow && gEspNow->initialized) {
    oledEspNowInit();
  }
  
  // Handle views that don't require ESP-NOW to be initialized
  if (gOledEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT ||
      gOledEspNowState.currentView == ESPNOW_VIEW_NAME_KEYBOARD) {
    // These views are shown before ESP-NOW init
    switch (gOledEspNowState.currentView) {
      case ESPNOW_VIEW_INIT_PROMPT:
        {
          // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
          display->setTextSize(1);
          display->setTextColor(DISPLAY_COLOR_WHITE);
          display->setCursor(0, OLED_CONTENT_START_Y);
          display->println("ESP-NOW not");
          display->println("initialized");
          display->println();
          display->println("Press Y to set");
          display->println("device name and");
          display->println("initialize");
          
          // Note: Footer is drawn by global render loop
        }
        break;
      case ESPNOW_VIEW_NAME_KEYBOARD:
        oledKeyboardDisplay(display);
        break;
      default:
        break;
    }
    return;
  }
  
  // All other views require ESP-NOW to be initialized
  if (!gEspNow || !gEspNow->initialized) return;
  
  // Refresh data periodically
  unsigned long now = millis();
  if (now - gOledEspNowState.lastUpdate > 1000 || gOledEspNowState.needsRefresh) {
    if (gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST) {
      oledEspNowRefreshDeviceList();
    } else if (gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_DETAIL) {
      oledEspNowRefreshMessages();
    }
    gOledEspNowState.lastUpdate = now;
    gOledEspNowState.needsRefresh = false;
  }
  
  // Display current view
  switch (gOledEspNowState.currentView) {
    case ESPNOW_VIEW_MAIN_MENU:
      oledEspNowDisplayMainMenu(display);
      break;
    case ESPNOW_VIEW_STATUS:
      oledEspNowDisplayStatus(display);
      break;
    case ESPNOW_VIEW_DEVICE_LIST:
      oledEspNowDisplayDeviceList(display);
      break;
    case ESPNOW_VIEW_DEVICE_DETAIL:
      oledEspNowDisplayDeviceDetail(display);
      break;
    case ESPNOW_VIEW_MODE_SELECT:
      oledEspNowDisplayModeSelect(display);
      break;
    case ESPNOW_VIEW_DEVICE_CONFIG:
      oledEspNowDisplayDeviceConfig(display);
      break;
    case ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD:
      oledKeyboardDisplay(display);
      break;
    case ESPNOW_VIEW_TEXT_KEYBOARD:
      oledKeyboardDisplay(display);
      break;
    case ESPNOW_VIEW_REMOTE_FORM:
      oledEspNowDisplayRemoteForm(display);
      break;
    case ESPNOW_VIEW_ROOMS:
      oledEspNowDisplayRooms(display);
      break;
    case ESPNOW_VIEW_SETTINGS:
      oledEspNowDisplaySettings(display);
      break;
    case ESPNOW_VIEW_SETTINGS_KEYBOARD:
      oledKeyboardDisplay(display);
      break;
    case ESPNOW_VIEW_PAIRING:
      oledEspNowDisplayPairing(display);
      break;
    default:
      break;
  }
}

// =============================================================================
// Main Menu Display (Bluetooth-style)
// =============================================================================

void oledEspNowDisplayMainMenu(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Status line in content area (header shows "ESP-NOW")
  display->setCursor(0, OLED_CONTENT_START_Y);
  if (gEspNow && gEspNow->initialized) {
    display->print("Online: ");
    display->println(MeshPeers::countHealthy());
  } else {
    display->println("Status: OFF");
  }
  
  // Calculate visible menu area (44px content - 10px status line = 34px for menu)
  const int kStatusHeight = 10;
  const int kLineHeight = 8;
  const int kMaxVisibleItems = (OLED_CONTENT_HEIGHT - kStatusHeight) / kLineHeight;  // 4 lines
  const int kTotalItems = ESPNOW_MENU_ITEM_COUNT;
  
  // Clamp selection
  if (gOledEspNowState.mainMenuSelection >= kTotalItems) {
    gOledEspNowState.mainMenuSelection = kTotalItems - 1;
  }
  if (gOledEspNowState.mainMenuSelection < 0) {
    gOledEspNowState.mainMenuSelection = 0;
  }
  
  // Adjust scroll offset to keep selection visible
  int& scrollOffset = gOledEspNowState.mainMenuScrollOffset;
  if (gOledEspNowState.mainMenuSelection < scrollOffset) {
    scrollOffset = gOledEspNowState.mainMenuSelection;
  }
  if (gOledEspNowState.mainMenuSelection >= scrollOffset + kMaxVisibleItems) {
    scrollOffset = gOledEspNowState.mainMenuSelection - kMaxVisibleItems + 1;
  }
  // Clamp scroll offset
  if (scrollOffset > kTotalItems - kMaxVisibleItems) {
    scrollOffset = kTotalItems - kMaxVisibleItems;
  }
  if (scrollOffset < 0) {
    scrollOffset = 0;
  }
  
  // Draw visible menu items (starting after status line)
  int menuStartY = OLED_CONTENT_START_Y + kStatusHeight;
  for (int i = 0; i < kMaxVisibleItems && (scrollOffset + i) < kTotalItems; i++) {
    int itemIndex = scrollOffset + i;
    display->setCursor(0, menuStartY + i * kLineHeight);
    if (itemIndex == gOledEspNowState.mainMenuSelection) {
      display->print("> ");
    } else {
      display->print("  ");
    }
    display->print(espnowMenuItems[itemIndex]);
    
    // Show state indicators inline
    if (itemIndex == 4) {  // Start/Stop
      display->print(gEspNow && gEspNow->initialized ? " *" : "");
    }
  }
  
  // Show scroll indicators in right margin if needed
  if (scrollOffset > 0) {
    display->setCursor(120, menuStartY);
    display->print("\x18");  // Up arrow
  }
  if (scrollOffset + kMaxVisibleItems < kTotalItems) {
    display->setCursor(120, menuStartY + (kMaxVisibleItems - 1) * kLineHeight);
    display->print("\x19");  // Down arrow
  }
  
  // Note: Footer is drawn by global render loop, don't draw it here
}

void oledEspNowDisplayStatus(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header shows "ESP-NOW", start content below it
  display->setCursor(0, OLED_CONTENT_START_Y);
  
  // Role
  const char* roleStr = "Worker";
  if (gSettings.meshRole == MESH_ROLE_MASTER) roleStr = "Master";
  else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) roleStr = "Backup";
  display->print("Role: ");
  display->println(roleStr);
  
  // Device count: online / total. Both pulled from the SAME gMeshPeers
  // health array so the ratio is always sensible (numerator <= denominator).
  // Previously the denominator counted gMeshPeerMeta[i].isActive — a
  // different array populated by a different code path. When they got out
  // of sync (e.g. peer sent heartbeat before meta was registered) the
  // display rendered "1/0", which is nonsense as a ratio. countActive()
  // and countHealthy() walk the same slots so the math always works.
  display->print("Devices: ");
  display->print(MeshPeers::countHealthy());
  display->print("/");
  display->println(MeshPeers::countActive());
  
  // Encryption status
  display->print("Encrypt: ");
  display->println(gEspNow && gEspNow->encryptionEnabled ? "Yes" : "No");
  
  // Channel
  display->print("Channel: ");
  display->println(gEspNow ? gEspNow->channel : 0);
  
  // Device name (truncate if too long)
  display->print("Name: ");
  String name = gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName : "(none)";
  if (name.length() > 15) { name = name.substring(0, 14); name += '~'; }
  display->println(name);
  
  // Note: Footer is drawn by global render loop
}

// Max rooms we can track on the OLED
#define ROOMS_MAX 16
#define ROOMS_DEVICES_MAX 16

// Cached room list (rebuilt on entry)
struct RoomListEntry {
  char name[32];
  int deviceCount;
};
EXT_RAM_BSS_ATTR static RoomListEntry sRoomList[ROOMS_MAX];
static int sRoomCount = 0;

// Cached device list for selected room
struct RoomDeviceEntry {
  char name[24];
  uint8_t mac[6];
  bool alive;
};
EXT_RAM_BSS_ATTR static RoomDeviceEntry sRoomDevices[ROOMS_DEVICES_MAX];
static int sRoomDeviceCount = 0;

// Rebuild the room list from mesh peer metadata + local device
static void rebuildRoomList() {
  sRoomCount = 0;
  
  auto addRoom = [&](const char* room) {
    if (!room || room[0] == '\0') return;
    // Check if already in list
    for (int r = 0; r < sRoomCount; r++) {
      if (strcasecmp(sRoomList[r].name, room) == 0) {
        sRoomList[r].deviceCount++;
        return;
      }
    }
    // New room
    if (sRoomCount < ROOMS_MAX) {
      strncpy(sRoomList[sRoomCount].name, room, 31);
      sRoomList[sRoomCount].name[31] = '\0';
      sRoomList[sRoomCount].deviceCount = 1;
      sRoomCount++;
    }
  };
  
  // Add local device's room
  if (gSettings.espnowRoom.length() > 0) {
    addRoom(gSettings.espnowRoom.c_str());
  }
  
  // Add rooms from mesh peers
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive && gMeshPeerMeta[i].room[0]) {
      addRoom(gMeshPeerMeta[i].room);
    }
  }
}

// Rebuild the device list for a specific room
static void rebuildRoomDeviceList(const char* room) {
  sRoomDeviceCount = 0;
  if (!room || room[0] == '\0') return;
  
  // Check if local device is in this room
  if (gSettings.espnowRoom.length() > 0 && strcasecmp(gSettings.espnowRoom.c_str(), room) == 0) {
    if (sRoomDeviceCount < ROOMS_DEVICES_MAX) {
      const char* name = gSettings.espnowFriendlyName.length() > 0 ? gSettings.espnowFriendlyName.c_str() :
                         gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName.c_str() : "(this device)";
      strncpy(sRoomDevices[sRoomDeviceCount].name, name, 23);
      sRoomDevices[sRoomDeviceCount].name[23] = '\0';
      memset(sRoomDevices[sRoomDeviceCount].mac, 0, 6);
      sRoomDevices[sRoomDeviceCount].alive = true;  // Local device is always alive
      sRoomDeviceCount++;
    }
  }
  
  // Add mesh peers in this room
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    if (strcasecmp(gMeshPeerMeta[i].room, room) != 0) continue;
    if (sRoomDeviceCount >= ROOMS_DEVICES_MAX) break;
    
    String displayName = MeshPeers::displayName(gMeshPeerMeta[i].mac);
    strncpy(sRoomDevices[sRoomDeviceCount].name, displayName.c_str(), 23);
    sRoomDevices[sRoomDeviceCount].name[23] = '\0';
    memcpy(sRoomDevices[sRoomDeviceCount].mac, gMeshPeerMeta[i].mac, 6);
    sRoomDevices[sRoomDeviceCount].alive = MeshPeers::isHealthy(gMeshPeerMeta[i].mac);
    sRoomDeviceCount++;
  }
}

void oledEspNowDisplayRooms(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  if (!gOledEspNowState.inRoomDeviceList) {
    // === Room list view ===
    if (sRoomCount == 0) {
      display->setCursor(0, OLED_CONTENT_START_Y);
      display->println("No rooms defined.");
      display->println();
      display->println("Set room in");
      display->println("Settings menu.");
      return;
    }
    
    // Scrollable room list
    const int lineHeight = 10;
    const int maxVisible = OLED_CONTENT_HEIGHT / lineHeight;  // ~4 items
    
    // Clamp selection
    if (gOledEspNowState.roomsMenuSelection >= sRoomCount) {
      gOledEspNowState.roomsMenuSelection = sRoomCount - 1;
    }
    if (gOledEspNowState.roomsMenuSelection < 0) {
      gOledEspNowState.roomsMenuSelection = 0;
    }
    
    // Scroll offset
    static int roomsScrollOffset = 0;
    if (gOledEspNowState.roomsMenuSelection < roomsScrollOffset) {
      roomsScrollOffset = gOledEspNowState.roomsMenuSelection;
    } else if (gOledEspNowState.roomsMenuSelection >= roomsScrollOffset + maxVisible) {
      roomsScrollOffset = gOledEspNowState.roomsMenuSelection - maxVisible + 1;
    }
    
    for (int v = 0; v < maxVisible && (roomsScrollOffset + v) < sRoomCount; v++) {
      int idx = roomsScrollOffset + v;
      int y = OLED_CONTENT_START_Y + v * lineHeight;
      
      if (idx == gOledEspNowState.roomsMenuSelection) {
        display->fillRect(0, y, 128, lineHeight, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(2, y + 1);
      // Room name + device count
      char buf[28];
      snprintf(buf, sizeof(buf), "%s (%d)", sRoomList[idx].name, sRoomList[idx].deviceCount);
      display->print(buf);
    }
    
    // Scroll indicators
    display->setTextColor(DISPLAY_COLOR_WHITE);
    if (roomsScrollOffset > 0) {
      display->setCursor(120, OLED_CONTENT_START_Y);
      display->print("\x18");
    }
    if (roomsScrollOffset + maxVisible < sRoomCount) {
      display->setCursor(120, OLED_CONTENT_START_Y + (maxVisible - 1) * lineHeight);
      display->print("\x19");
    }
  } else {
    // === Device list within a room ===
    // Title: room name
    display->setCursor(0, OLED_CONTENT_START_Y);
    display->print(sRoomList[gOledEspNowState.roomsMenuSelection].name);
    display->drawFastHLine(0, OLED_CONTENT_START_Y + 9, 128, DISPLAY_COLOR_WHITE);
    
    if (sRoomDeviceCount == 0) {
      display->setCursor(0, OLED_CONTENT_START_Y + 12);
      display->println("No devices");
      return;
    }
    
    // Clamp selection
    if (gOledEspNowState.roomsDeviceSelection >= sRoomDeviceCount) {
      gOledEspNowState.roomsDeviceSelection = sRoomDeviceCount - 1;
    }
    if (gOledEspNowState.roomsDeviceSelection < 0) {
      gOledEspNowState.roomsDeviceSelection = 0;
    }
    
    const int lineHeight = 10;
    const int listStartY = OLED_CONTENT_START_Y + 11;
    const int maxVisible = (OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - listStartY) / lineHeight;  // ~3 items
    
    static int devScrollOffset = 0;
    if (gOledEspNowState.roomsDeviceSelection < devScrollOffset) {
      devScrollOffset = gOledEspNowState.roomsDeviceSelection;
    } else if (gOledEspNowState.roomsDeviceSelection >= devScrollOffset + maxVisible) {
      devScrollOffset = gOledEspNowState.roomsDeviceSelection - maxVisible + 1;
    }
    
    for (int v = 0; v < maxVisible && (devScrollOffset + v) < sRoomDeviceCount; v++) {
      int idx = devScrollOffset + v;
      int y = listStartY + v * lineHeight;
      
      if (idx == gOledEspNowState.roomsDeviceSelection) {
        display->fillRect(0, y, 128, lineHeight, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(2, y + 1);
      display->print(sRoomDevices[idx].alive ? "+" : "-");
      display->print(" ");
      display->print(sRoomDevices[idx].name);
    }
    
    // Scroll indicators
    display->setTextColor(DISPLAY_COLOR_WHITE);
    if (devScrollOffset > 0) {
      display->setCursor(120, listStartY);
      display->print("\x18");
    }
    if (devScrollOffset + maxVisible < sRoomDeviceCount) {
      display->setCursor(120, listStartY + (maxVisible - 1) * lineHeight);
      display->print("\x19");
    }
  }
  
  // Note: Footer is drawn by global render loop
}


// Selected candidate row on the pairing/discovery screen (shared render+input).
static int sPairingSel = 0;

// Incoming pair request → the shared confirm dialog (Accept/Reject). Poll-driven
// from oledUpdate() so it pops no matter which screen is showing. The dialog
// holds line2 by pointer, so the name buffer is static (stable while it's up).
static char sPairPromptName[26];
static void onPairReqAccept(void* /*ud*/) { executeOLEDCommand("espnowaccept"); }
static void onPairReqReject(void* /*ud*/) { executeOLEDCommand("espnowreject"); }

void oledEspNowPollPairRequest() {
  if (oledConfirmIsActive()) return;                       // a dialog is already up
  char reqName[24];
  if (!espnowGetIncomingPairRequest(reqName, sizeof(reqName))) return;
  snprintf(sPairPromptName, sizeof(sPairPromptName), "'%s'", reqName);
  // Default to No so an unattended device never auto-accepts.
  oledConfirmRequest("Pair request from", sPairPromptName, onPairReqAccept, nullptr,
                     /*defaultYes=*/false, onPairReqReject);
}

void oledEspNowDisplayPairing(Adafruit_SSD1306* display) {
  if (!display) return;

  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  int y = OLED_CONTENT_START_Y;

  // (An incoming pair request pops the shared confirm dialog globally via
  // oledEspNowPollPairRequest() — not drawn here.)

  // Discovery window open → live, selectable candidate list.
  if (espnowPairModeActive()) {
    oledMarkDirtyUntil(millis() + 1200);   // live countdown/list

    uint32_t remS = espnowPairModeRemainingMs() / 1000;
    char line[22];
    snprintf(line, sizeof(line), "Discovery  %lu:%02lu",
             (unsigned long)(remS / 60), (unsigned long)(remS % 60));
    display->setCursor(0, y); display->println(line); y += 11;

    int n = espnowGetPairCandidateCount();
    if (n <= 0) {
      sPairingSel = 0;
      display->setCursor(0, y); display->println("Searching...");
      return;
    }
    if (sPairingSel >= n) sPairingSel = n - 1;
    if (sPairingSel < 0)  sPairingSel = 0;

    display->setCursor(0, y); display->println("Select to pair:"); y += 11;
    const int maxRows = 3;
    int start = (sPairingSel >= maxRows) ? sPairingSel - maxRows + 1 : 0;
    for (int i = start; i < n && i < start + maxRows; i++) {
      char nm[24]; uint8_t mac[6]; int rssi = 0;
      if (!espnowGetPairCandidate(i, nm, sizeof(nm), mac, &rssi)) continue;
      display->setCursor(0, y);
      display->print(i == sPairingSel ? "> " : "  ");
      display->println(nm);
      y += 10;
    }
    return;
  }

  // 3. Window closed.
  display->setCursor(0, y); display->println("Discovery OFF"); y += 12;
  if (gEspNow && !gEspNow->encryptionEnabled) {
    display->setCursor(0, y); display->println("Set a mesh"); y += 10;
    display->setCursor(0, y); display->println("passphrase first."); y += 10;
    display->setCursor(0, y); display->println("(Settings)");
  } else {
    display->setCursor(0, y); display->println("Press A to start,"); y += 10;
    display->setCursor(0, y); display->println("then same on the"); y += 10;
    display->setCursor(0, y); display->println("other device.");
  }
}

// =============================================================================
// Main Menu Navigation
// =============================================================================

int oledEspNowGetMainMenuItemCount() {
  return ESPNOW_MENU_ITEM_COUNT;
}

void oledEspNowMainMenuUp() {
  if (gOledEspNowState.mainMenuSelection > 0) {
    gOledEspNowState.mainMenuSelection--;
  }
}

void oledEspNowMainMenuDown() {
  if (gOledEspNowState.mainMenuSelection < ESPNOW_MENU_ITEM_COUNT - 1) {
    gOledEspNowState.mainMenuSelection++;
  }
}

void oledEspNowMainMenuSelect() {
  switch (gOledEspNowState.mainMenuSelection) {
    case 0:  // Status
      gOledEspNowState.currentView = ESPNOW_VIEW_STATUS;
      break;
    case 1:  // Devices
      gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
      oledEspNowRefreshDeviceList();
      break;
    case 2:  // Rooms
      rebuildRoomList();
      gOledEspNowState.roomsMenuSelection = 0;
      gOledEspNowState.inRoomDeviceList = false;
      gOledEspNowState.currentView = ESPNOW_VIEW_ROOMS;
      break;
    case 3:  // Settings
      gOledEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
      break;
    case 4:  // Start/Stop
      {
        if (gEspNow && gEspNow->initialized) {
          executeOLEDCommand("closeespnow");
        } else {
          executeOLEDCommand("openespnow");
        }
      }
      break;
    case 5:  // Pairing
      gOledEspNowState.currentView = ESPNOW_VIEW_PAIRING;
      break;
  }
}

void oledEspNowDisplayDeviceList(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Build dynamic title with role, filter, and sort indicators
  static char titleBuf[24];
  const char* roleStr = "[W]";
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    roleStr = "[M]";
  } else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) {
    roleStr = "[B]";
  }
  
  // Filter indicator: All, Room, Zone
  const char* filterStr = "";
  if (gOledEspNowState.filterMode == 1) {
    filterStr = "R";
  } else if (gOledEspNowState.filterMode == 2) {
    filterStr = "Z";
  }
  
  // Sort indicator: Name, Room, Status
  const char* sortStr = "N";
  if (gOledEspNowState.sortMode == 1) {
    sortStr = "Rm";
  } else if (gOledEspNowState.sortMode == 2) {
    sortStr = "St";
  }
  
  // Build title: "ESP-NOW [M] E R:Rm" (role, encrypted, filter, sort)
  if (gEspNow && gEspNow->encryptionEnabled) {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s E %s:%s", roleStr, filterStr[0] ? filterStr : "A", sortStr);
  } else {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s %s:%s", roleStr, filterStr[0] ? filterStr : "A", sortStr);
  }
  gOledEspNowState.deviceList.title = titleBuf;
  
  // Render device list using scrolling system
  oledScrollRender(display, &gOledEspNowState.deviceList, true, true);
}

void oledEspNowDisplayDeviceDetail(Adafruit_SSD1306* display) {
  if (!display) return;

  // The interaction mode + device context live in the GLOBAL header bar
  // ("ESPNOW: Text/Remote/File", via oledEspNowHeaderTitle()), so content starts
  // at OLED_CONTENT_START_Y like the sibling ESP-NOW views. (The old in-body
  // device-name + "Mode:" lines were drawn at y=0/8 and got clobbered/clipped by
  // the global header — removed.)
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);

  // If in File mode, show file browser prompt instead of message list
  if (gOledEspNowState.interactionMode == ESPNOW_MODE_FILE) {
    // Send / Receive options shown inline (A=select, Up/Down=move, X=mode, B=back).
    display->setTextSize(1);
    const char* opts[2] = { "Send Files", "Receive Files" };
    int y = OLED_CONTENT_START_Y + 4;
    for (int i = 0; i < 2; i++) {
      bool sel = (gOledEspNowState.fileChooserIndex == i);
      if (sel) {
        display->fillRect(0, y - 1, SCREEN_WIDTH, 11, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      display->setCursor(6, y + 1);
      display->print(sel ? "> " : "  ");
      display->print(opts[i]);
      y += 14;
    }
    display->setTextColor(DISPLAY_COLOR_WHITE);
    return;
  }
  
  // Render message list below the global header. The visible window, the per-row
  // clip and the scrollbar all derive from the SAME footer geometry as
  // messageList.visibleLines (see kMsgList* above), so a scrolled-to row can never
  // land in or below the global footer.
  const int yOffset    = kMsgListTopY;
  const int footerTopY = oledEspNowMsgFooterTopY();
  const int lineHeight = 8;
  int visibleStart = gOledEspNowState.messageList.scrollOffset;
  int visibleEnd = min(gOledEspNowState.messageList.itemCount,
                       visibleStart + gOledEspNowState.messageList.visibleLines);

  int yPos = yOffset;

  const int rightEdge = SCREEN_WIDTH - 4;  // leave room for the scrollbar
  for (int i = visibleStart; i < visibleEnd && (yPos + kMsgRowHeight) <= footerTopY; i++) {
    OLEDScrollItem* item = &gOledEspNowState.messageList.items[i];
    bool isSelected = (i == gOledEspNowState.messageList.selectedIndex);
    bool isSent = gOledRowMeta[i].isSent;  // left = received, right = sent (web-style)

    // Line 1: message text (truncated). Sent right-aligned, received left-aligned.
    String msg = item->line1 ? item->line1 : "";
    if (msg.length() > 20) { msg = msg.substring(0, 19); msg += '~'; }
    int textW = (int)msg.length() * 6;  // ~6 px/char at text size 1
    int x = isSent ? max(4, rightEdge - textW) : (isSelected ? 4 : 0);

    // Selection bar on the message's own side.
    if (isSelected) {
      int barX = isSent ? (SCREEN_WIDTH - 2) : 0;
      display->fillRect(barX, yPos, 2, lineHeight * 2, DISPLAY_COLOR_WHITE);
    }
    display->setCursor(x, yPos);
    display->print(msg);
    yPos += lineHeight;

    // Line 2: delivery status (sent, right) or sender (received, left).
    if (isSent) {
      // Durable delivery state from the message record (stamped on ACK by
      // espnowUpdateSentDeliveryState) — NOT the ephemeral sendStatus ring, which
      // is swept after 30s and used to make delivered rows revert to "Sent".
      const char* label; bool icon = true, dbl = false;
      switch (gOledRowMeta[i].sendState) {
        case SEND_STATUS_DELIVERED: label = "Delivered"; dbl = true; break;
        case SEND_STATUS_TIMEOUT:   label = "No ACK"; icon = false; break;
        case SEND_STATUS_FAILED:    label = "Failed"; icon = false; break;
        default:                    label = "Sent"; break;  // PENDING: awaiting ACK
      }
      int iconW = icon ? (dbl ? 8 : 6) : 0;
      int lblW = (int)strlen(label) * 6 + iconW;
      int lx = max(4, rightEdge - lblW);
      if (icon) { oledEspNowDrawStatusIcon(display, lx, yPos, dbl); lx += iconW; }
      display->setCursor(lx, yPos);
      display->print(label);
    } else {
      display->setCursor(isSelected ? 4 : 0, yPos);
      if (item->line2) display->print(item->line2);
    }
    yPos += lineHeight;
  }
  
  // Show scrollbar if needed
  if (gOledEspNowState.messageList.itemCount > gOledEspNowState.messageList.visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    int scrollbarY = yOffset;
    int scrollbarHeight = footerTopY - yOffset;  // content band, above the global footer
    
    display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
    
    int thumbHeight = max(4, (scrollbarHeight * gOledEspNowState.messageList.visibleLines) / 
                            gOledEspNowState.messageList.itemCount);
    int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * 
                 gOledEspNowState.messageList.scrollOffset / 
                 max(1, gOledEspNowState.messageList.itemCount - gOledEspNowState.messageList.visibleLines);
    
    display->fillRect(scrollbarX - 1, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
  }
}

void oledEspNowDisplayModeSelect(Adafruit_SSD1306* display) {
  if (!display) return;

  // Popup box within the content region: boxY = OLED_CONTENT_START_Y sits 1px
  // below the header boundary line (drawn at OLED_HEADER_HEIGHT-1), and
  // height-2 leaves the matching 1px gap above the footer boundary line (at
  // OLED_HEADER_HEIGHT+OLED_CONTENT_HEIGHT). Text is anchored off boxY.
  const int boxY = OLED_CONTENT_START_Y;
  const int boxH = OLED_CONTENT_HEIGHT - 2;
  display->fillRect(20, boxY, 88, boxH, DISPLAY_COLOR_BLACK);
  display->drawRect(20, boxY, 88, boxH, DISPLAY_COLOR_WHITE);

  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(24, boxY + 3);
  display->println("Select Mode:");

  display->setCursor(24, boxY + 13);
  if (gOledEspNowState.modeSelectorIndex == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Text     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Text     ");
  }

  display->setCursor(24, boxY + 21);
  if (gOledEspNowState.modeSelectorIndex == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Remote   ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Remote   ");
  }

  display->setCursor(24, boxY + 29);
  if (gOledEspNowState.modeSelectorIndex == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> File     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  File     ");
  }
}

bool oledEspNowHandleInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Handle input based on current view
  switch (gOledEspNowState.currentView) {
    case ESPNOW_VIEW_INIT_PROMPT:
      // Init prompt is handled in oled_display.cpp
      return false;
      
    case ESPNOW_VIEW_NAME_KEYBOARD:
      // Let keyboard handle input
      return oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    case ESPNOW_VIEW_MAIN_MENU:
      // Navigate main menu using centralized navigation events
      if (gNavEvents.up) {
        oledEspNowMainMenuUp();
        return true;
      }
      if (gNavEvents.down) {
        oledEspNowMainMenuDown();
        return true;
      }
      
      // A button: Select menu item
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowMainMenuSelect();
        return true;
      }
      
      // B button: Exit to main OLED menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        return false;  // Let default handler take us back to OLED menu
      }
      return false;
    
    case ESPNOW_VIEW_STATUS:
      // B button: Back to main menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
        return true;
      }
      return false;

    case ESPNOW_VIEW_PAIRING: {
      // An incoming request is handled by the global confirm dialog (see
      // oledEspNowPollPairRequest); its input is intercepted before we get here.
      // Discovery open → navigate candidates; A requests a pair; B stops + exits.
      if (espnowPairModeActive()) {
        int n = espnowGetPairCandidateCount();
        if (gNavEvents.up && sPairingSel > 0)       { sPairingSel--; return true; }
        if (gNavEvents.down && sPairingSel < n - 1) { sPairingSel++; return true; }
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          uint8_t mac[6]; char nm[24]; int rssi = 0;
          if (n > 0 && espnowGetPairCandidate(sPairingSel, nm, sizeof(nm), mac, &rssi)) {
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "espnowpairrequest %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            executeOLEDCommand(cmd);   // waits for the other device to Accept
          }
          oledMarkDirty();
          return true;
        }
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          espnowPairModeClose();       // leaving the screen stops discovery
          gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
          return true;
        }
        return false;
      }
      // 3. Window closed → A starts discovery, B back to menu.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        if (!(gEspNow && !gEspNow->encryptionEnabled)) {   // need a mesh key first (screen says so)
          sPairingSel = 0;
          espnowPairModeOpen(120);
        }
        oledMarkDirty();
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
        return true;
      }
      return false;
    }
      
    case ESPNOW_VIEW_ROOMS:
      if (!gOledEspNowState.inRoomDeviceList) {
        // Room list navigation
        if (gNavEvents.up && gOledEspNowState.roomsMenuSelection > 0) {
          gOledEspNowState.roomsMenuSelection--;
          return true;
        }
        if (gNavEvents.down && gOledEspNowState.roomsMenuSelection < sRoomCount - 1) {
          gOledEspNowState.roomsMenuSelection++;
          return true;
        }
        // A button: drill into room
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) && sRoomCount > 0) {
          rebuildRoomDeviceList(sRoomList[gOledEspNowState.roomsMenuSelection].name);
          gOledEspNowState.roomsDeviceSelection = 0;
          gOledEspNowState.inRoomDeviceList = true;
          return true;
        }
        // B button: back to main menu
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
          return true;
        }
      } else {
        // Device list within room navigation
        if (gNavEvents.up && gOledEspNowState.roomsDeviceSelection > 0) {
          gOledEspNowState.roomsDeviceSelection--;
          return true;
        }
        if (gNavEvents.down && gOledEspNowState.roomsDeviceSelection < sRoomDeviceCount - 1) {
          gOledEspNowState.roomsDeviceSelection++;
          return true;
        }
        // A button: select device -> go to device detail (if not local device)
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) && sRoomDeviceCount > 0) {
          int sel = gOledEspNowState.roomsDeviceSelection;
          // Check if this is a remote device (non-zero MAC)
          uint8_t zeroMac[6] = {0};
          if (memcmp(sRoomDevices[sel].mac, zeroMac, 6) != 0) {
            memcpy(gOledEspNowState.selectedDeviceMac, sRoomDevices[sel].mac, 6);
            gOledEspNowState.selectedDeviceName = sRoomDevices[sel].name;
            gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
            gOledEspNowState.needsRefresh = true;
            oledEspNowRefreshMessages();
          }
          return true;
        }
        // B button: back to room list
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          gOledEspNowState.inRoomDeviceList = false;
          return true;
        }
      }
      return false;
      
    case ESPNOW_VIEW_DEVICE_LIST:
      // Navigate device list using centralized navigation events
      if (gNavEvents.up) {
        oledScrollUp(&gOledEspNowState.deviceList);
        return true;
      }
      if (gNavEvents.down) {
        oledScrollDown(&gOledEspNowState.deviceList);
        return true;
      }
      
      // A button: Select device
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectDevice();
        return true;
      }
      
      // X button: Cycle filter mode (All -> Room -> Zone -> All)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        gOledEspNowState.filterMode = (gOledEspNowState.filterMode + 1) % 3;
        
        // If switching to room/zone filter, pick first available value
        if (gOledEspNowState.filterMode > 0 && gMeshPeerMeta) {
          memset(gOledEspNowState.filterValue, 0, sizeof(gOledEspNowState.filterValue));
          
          // Find first device with room or zone set
          for (int i = 0; i < gMeshPeerSlots; i++) {
            if (!gMeshPeerMeta[i].isActive) continue;
            
            if (gOledEspNowState.filterMode == 1 && gMeshPeerMeta[i].room[0]) {
              strncpy(gOledEspNowState.filterValue, gMeshPeerMeta[i].room, sizeof(gOledEspNowState.filterValue) - 1);
              break;
            } else if (gOledEspNowState.filterMode == 2 && gMeshPeerMeta[i].zone[0]) {
              strncpy(gOledEspNowState.filterValue, gMeshPeerMeta[i].zone, sizeof(gOledEspNowState.filterValue) - 1);
              break;
            }
          }
        }
        
        gOledEspNowState.needsRefresh = true;
        return true;
      }
      
      // Y button: Cycle sort mode (Name -> Room -> Status -> Name)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        gOledEspNowState.sortMode = (gOledEspNowState.sortMode + 1) % 3;
        gOledEspNowState.needsRefresh = true;
        return true;
      }
      
      // B button: Back to main menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_SETTINGS:
      return oledEspNowHandleSettingsInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_SETTINGS_KEYBOARD: {
      // Let the keyboard process this frame's input, then act on the resulting
      // state. Checking IsCancelled() here (not only IsCompleted()) is what makes
      // a single B press return to the settings list: the keyboard's own B
      // handler flips it to cancelled+inactive, so without this branch the view
      // would stay on a now-inactive keyboard. The global header then paints over
      // its title, leaving a title-less "ghost" keyboard that looks exactly like
      // the device-name entry screen — and it took a SECOND B press to escape.
      bool kbHandled = oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      if (oledKeyboardIsCompleted()) {
        String value = oledKeyboardGetText();
        oledEspNowApplySettingsEdit(value);
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        return true;
      }
      if (oledKeyboardIsCancelled()) {
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        return true;
      }
      return kbHandled;
    }

    case ESPNOW_VIEW_DEVICE_CONFIG:
      return oledEspNowHandleDeviceConfigInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD: {
      // Same single-press cancel fix as ESPNOW_VIEW_SETTINGS_KEYBOARD above.
      bool kbHandled = oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      if (oledKeyboardIsCompleted()) {
        String value = oledKeyboardGetText();
        oledEspNowApplyDeviceConfigEdit(value);
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
        return true;
      }
      if (oledKeyboardIsCancelled()) {
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
        return true;
      }
      return kbHandled;
    }

    case ESPNOW_VIEW_DEVICE_DETAIL:
      // File mode shows Send/Receive inline: Up/Down move the cursor, A selects.
      // X (mode) and B (back) fall through to the shared all-modes handlers below.
      if (gOledEspNowState.interactionMode == ESPNOW_MODE_FILE) {
        if (gNavEvents.up && gOledEspNowState.fileChooserIndex > 0) {
          gOledEspNowState.fileChooserIndex--;
          return true;
        }
        if (gNavEvents.down && gOledEspNowState.fileChooserIndex < 1) {
          gOledEspNowState.fileChooserIndex++;
          return true;
        }
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          if (gOledEspNowState.fileChooserIndex == 1) {
            oledFileBrowserStartEspnowReceive(gOledEspNowState.selectedDeviceMac);
            requestOLEDMode(OLED_FILE_BROWSER, "espnow.receive");  // B returns here
          } else {
            oledFileBrowserStartEspnowSend(gOledEspNowState.selectedDeviceMac);
            requestOLEDMode(OLED_FILE_BROWSER, "espnow.send");
          }
          return true;
        }
      } else if (gOledEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
        // A button: Open text keyboard
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOledEspNowState.currentView = ESPNOW_VIEW_TEXT_KEYBOARD;
          gOledEspNowState.textMessageBuffer = "";
          oledKeyboardInit("Send Message:", "", 128);
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOledEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOledEspNowState.messageList);
          return true;
        }
        
        // Y button: Open device config
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          oledEspNowOpenDeviceConfig();
          return true;
        }
      } else if (gOledEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
        // A button: Open remote form
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOledEspNowState.currentView = ESPNOW_VIEW_REMOTE_FORM;
          gOledEspNowState.remoteFormField = 0;
          gOledEspNowState.remoteUsername = "";
          gOledEspNowState.remotePassword = "";
          gOledEspNowState.remoteCommand = "";
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOledEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOledEspNowState.messageList);
          return true;
        }
        
        // Y button: Open device config
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          oledEspNowOpenDeviceConfig();
          return true;
        }
      }
      
      // X button: Open mode selector (all modes)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        oledEspNowOpenModeSelector();
        return true;
      }
      
      // B button: Back to device list (all modes)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledEspNowBackToList();
        return true;
      }
      return false;  // No input handled

    case ESPNOW_VIEW_MODE_SELECT:
      // Navigate mode selector using centralized navigation events
      if (gNavEvents.up && gOledEspNowState.modeSelectorIndex > 0) {
        gOledEspNowState.modeSelectorIndex--;
        return true;
      }
      if (gNavEvents.down && gOledEspNowState.modeSelectorIndex < 2) {
        gOledEspNowState.modeSelectorIndex++;
        return true;
      }
      
      // A button: Select mode
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectMode();
        return true;
      }
      
      // B button: Cancel
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_TEXT_KEYBOARD:
      // Handle keyboard input
      if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
        return true;
      }
      
      // Check if keyboard completed or cancelled
      if (oledKeyboardIsCompleted()) {
        gOledEspNowState.textMessageBuffer = String(oledKeyboardGetText());
        oledEspNowSendTextMessage();
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      if (oledKeyboardIsCancelled()) {
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_REMOTE_FORM:
      return oledEspNowHandleRemoteFormInput(deltaX, deltaY, newlyPressed);
  }
  
  return false;  // Default: no input handled
}

void oledEspNowSelectDevice() {
  OLEDScrollItem* selected = oledScrollGetSelected(&gOledEspNowState.deviceList);
  if (!selected || !selected->userData) return;
  
  // Store selected device MAC
  EspNowDevice* device = (EspNowDevice*)selected->userData;
  memcpy(gOledEspNowState.selectedDeviceMac, device->mac, 6);
  gOledEspNowState.selectedDeviceName = String(device->name);
  
  // Switch to device detail view
  gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOledEspNowState.needsRefresh = true;
  
  // Refresh messages for this device
  oledEspNowRefreshMessages();
}

void oledEspNowBackToList() {
  gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
  gOledEspNowState.needsRefresh = true;
}

void oledEspNowOpenModeSelector() {
  gOledEspNowState.currentView = ESPNOW_VIEW_MODE_SELECT;
  // Map current mode to selector index: Text=0, Remote=1, File=2
  if (gOledEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
    gOledEspNowState.modeSelectorIndex = 0;
  } else if (gOledEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
    gOledEspNowState.modeSelectorIndex = 1;
  } else {
    gOledEspNowState.modeSelectorIndex = 2;  // File
  }
  gOledEspNowState.modeSelectorActive = true;
}

void oledEspNowSelectMode() {
  // Map selector index to mode: 0=Text, 1=Remote, 2=File
  if (gOledEspNowState.modeSelectorIndex == 0) {
    gOledEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  } else if (gOledEspNowState.modeSelectorIndex == 1) {
    gOledEspNowState.interactionMode = ESPNOW_MODE_REMOTE;
  } else {
    gOledEspNowState.interactionMode = ESPNOW_MODE_FILE;
  }
  gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOledEspNowState.modeSelectorActive = false;
}

void oledEspNowUnpairDevice() {
  if (!gEspNow || !gEspNow->initialized) return;
  if (!espnowDeletePeerRuntime(gOledEspNowState.selectedDeviceMac)) return;
  removeEspNowDevice(gOledEspNowState.selectedDeviceMac);
  oledEspNowBackToList();
}

// Helper struct for sorting devices
struct DeviceEntry {
  EspNowDevice* device;
  MeshPeerMeta* meta;
  MeshPeerHealth* health;
  bool alive;
  const char* displayName;
};

// Comparison function for sorting by name
static int compareByName(const void* a, const void* b) {
  const DeviceEntry* da = (const DeviceEntry*)a;
  const DeviceEntry* db = (const DeviceEntry*)b;
  return strcasecmp(da->displayName, db->displayName);
}

// Comparison function for sorting by room
static int compareByRoom(const void* a, const void* b) {
  const DeviceEntry* da = (const DeviceEntry*)a;
  const DeviceEntry* db = (const DeviceEntry*)b;
  
  const char* roomA = (da->meta && da->meta->room[0]) ? da->meta->room : "~";
  const char* roomB = (db->meta && db->meta->room[0]) ? db->meta->room : "~";
  
  int roomCmp = strcasecmp(roomA, roomB);
  if (roomCmp != 0) return roomCmp;
  
  // Same room, sort by name
  return strcasecmp(da->displayName, db->displayName);
}

// Comparison function for sorting by status (online first)
static int compareByStatus(const void* a, const void* b) {
  const DeviceEntry* da = (const DeviceEntry*)a;
  const DeviceEntry* db = (const DeviceEntry*)b;
  
  // Online devices first
  if (da->alive != db->alive) {
    return db->alive - da->alive;  // true (1) before false (0)
  }
  
  // Same status, sort by name
  return strcasecmp(da->displayName, db->displayName);
}

void oledEspNowRefreshDeviceList() {
  if (!gEspNow) return;

  // Keep the cursor across this rebuild — this runs on a 1s timer while the
  // device list is open, so a plain oledScrollClear() (which resets selectedIndex
  // to 0) would snap the selection back to the top every second.
  oledScrollClearKeepSelection(&gOledEspNowState.deviceList);
  
  // Get own MAC to skip self
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  // Static buffers for scroll item text (pointers stored in scroll items, so
  // they must outlive this call). PSRAM .bss: display text and sort rows,
  // written and read solely on the OLED task, never DMA'd — no reason to hold
  // ~1.2 KB of internal DRAM for a menu most devices never open.
  static EXT_RAM_BSS_ATTR char line1Bufs[16][28];
  static EXT_RAM_BSS_ATTR char line2Bufs[16][28];

  // Build array of device entries for filtering and sorting
  static EXT_RAM_BSS_ATTR DeviceEntry entries[16];
  int entryCount = 0;
  
  for (int i = 0; i < gEspNow->deviceCount && entryCount < 16; i++) {
    EspNowDevice* device = &gEspNow->devices[i];
    
    // Skip own device
    if (memcmp(device->mac, myMac, 6) == 0) {
      continue;
    }
    
    // Look up mesh metadata and health for this device. Health pointer is
    // stored in entries[] for the sort + render passes below.
    MeshPeerMeta* meta = getMeshPeerMeta(device->mac);
    MeshPeerHealth* health = getMeshPeerHealth(device->mac, false);
    bool alive = MeshPeers::isHealthy(device->mac);
    
    // Apply filter
    if (gOledEspNowState.filterMode == 1) {  // Filter by room
      if (!meta || !meta->room[0] || strcasecmp(meta->room, gOledEspNowState.filterValue) != 0) {
        continue;  // Skip devices not in selected room
      }
    } else if (gOledEspNowState.filterMode == 2) {  // Filter by zone
      if (!meta || !meta->zone[0] || strcasecmp(meta->zone, gOledEspNowState.filterValue) != 0) {
        continue;  // Skip devices not in selected zone
      }
    }
    
    // Determine display name (prefer friendlyName > meta name > device name)
    const char* displayName = device->name.c_str();
    if (meta) {
      if (meta->friendlyName[0]) displayName = meta->friendlyName;
      else if (meta->name[0]) displayName = meta->name;
    }
    if (!displayName || displayName[0] == '\0') {
      displayName = "Unknown";
    }
    
    // Add to entries array
    entries[entryCount].device = device;
    entries[entryCount].meta = meta;
    entries[entryCount].health = health;
    entries[entryCount].alive = alive;
    entries[entryCount].displayName = displayName;
    entryCount++;
  }
  
  // Sort entries based on sort mode
  if (entryCount > 1) {
    if (gOledEspNowState.sortMode == 1) {  // Sort by room
      qsort(entries, entryCount, sizeof(DeviceEntry), compareByRoom);
    } else if (gOledEspNowState.sortMode == 2) {  // Sort by status
      qsort(entries, entryCount, sizeof(DeviceEntry), compareByStatus);
    } else {  // Sort by name (default)
      qsort(entries, entryCount, sizeof(DeviceEntry), compareByName);
    }
  }
  
  // Add sorted/filtered entries to scroll list
  for (int i = 0; i < entryCount; i++) {
    DeviceEntry* entry = &entries[i];
    
    // Line 1: status indicator + display name
    snprintf(line1Bufs[i], sizeof(line1Bufs[0]), "%s %s",
             entry->alive ? "+" : "-", entry->displayName);
    
    // Line 2: room + encrypted flag, or MAC if no room
    if (entry->meta && entry->meta->room[0]) {
      snprintf(line2Bufs[i], sizeof(line2Bufs[0]), " %s%s",
               entry->meta->room, entry->device->encrypted ? " E" : "");
    } else {
      snprintf(line2Bufs[i], sizeof(line2Bufs[0]), " %02X%02X%02X%s",
               entry->device->mac[3], entry->device->mac[4], entry->device->mac[5],
               entry->device->encrypted ? " E" : "");
    }
    
    oledScrollAddItem(&gOledEspNowState.deviceList, line1Bufs[i], line2Bufs[i], true, entry->device);
  }
  
  // If no visible devices (excluding self), show message
  if (entryCount == 0) {
    static const char* noDevLine1 = "No devices";
    static const char* noDevLine2;
    if (gOledEspNowState.filterMode > 0) {
      noDevLine2 = "(filtered out)";
    } else {
      noDevLine2 = "Pair via web UI";
    }
    oledScrollAddItem(&gOledEspNowState.deviceList, noDevLine1, noDevLine2, false, nullptr);
  }

  // Clamp the preserved cursor back into range (device count may have shrunk).
  oledScrollClampSelection(&gOledEspNowState.deviceList);
}

void oledEspNowRefreshMessages() {
  if (!gEspNow) return;

  // Snapshot scroll position BEFORE clear so we can restore it after rebuild.
  // This function is called on a 1-second timer from the render loop — without
  // preserving position the user would get snapped to the top of the message
  // list every tick, making it impossible to scroll up to read older messages
  // while a peer is actively chatting.
  int savedSelectedIndex = gOledEspNowState.messageList.selectedIndex;
  int savedScrollOffset  = gOledEspNowState.messageList.scrollOffset;
  int prevItemCount      = gOledEspNowState.messageList.itemCount;

  oledScrollClear(&gOledEspNowState.messageList);

  // Build the conversation from the SHARED store: the core merges this peer's
  // received + sent rings into one time-ordered list (espnowGetConversation), so
  // the OLED shows the same outgoing history as every other interface — including
  // messages sent from the web or BLE, not just ones typed here. Sent rows carry
  // msgId (head->reqId) for the live delivery-status indicator; received rows show
  // the sender name + part count. (Zero-copy refs into the live rings.)
  uint8_t* mac = gOledEspNowState.selectedDeviceMac;

  static EXT_RAM_BSS_ATTR CollapsedMsgRef refs[2 * MESSAGES_PER_DEVICE];
  int rc = espnowGetConversation(mac, refs, 2 * MESSAGES_PER_DEVICE);

  static EXT_RAM_BSS_ATTR char metaBuf[OLED_SCROLL_MAX_ITEMS][24];  // persistent "name (k/n)" labels
  const int CAP = OLED_SCROLL_MAX_ITEMS;

  // Show the most recent CAP messages (conversation is oldest→newest).
  int start = (rc > CAP) ? (rc - CAP) : 0;
  int m = 0;
  for (int i = start; i < rc && m < CAP; i++) {
    const ReceivedTextMessage* msg = refs[i].head;
    if (!msg || !oledEspNowValidateMessagePtr(msg, mac)) continue;  // ref still in the live ring

    gOledRowMeta[m].isSent    = refs[i].isSent;
    gOledRowMeta[m].msgId     = refs[i].isSent ? msg->reqId : 0;
    gOledRowMeta[m].sendState = msg->sendState;  // durable; survives the 30s sendStatus sweep

    const char* meta;
    if (refs[i].isSent) {
      meta = nullptr;  // sent rows: delivery status drawn live by the renderer
    } else if (refs[i].partsTotal > 1) {
      const char* who = (msg->senderName[0]) ? msg->senderName : "Unknown";
      snprintf(metaBuf[m], sizeof(metaBuf[m]), "%s (%u/%u)", who,
               refs[i].partsPresent, refs[i].partsTotal);
      meta = metaBuf[m];
    } else {
      meta = (msg->senderName[0]) ? msg->senderName : "Unknown";
    }

    oledScrollAddItem(&gOledEspNowState.messageList, msg->message, meta, true, nullptr);
    m++;
  }

  if (m == 0) {
    static const char* noMsgLine1 = "No messages yet";
    static const char* noMsgLine2 = "Start chatting!";
    gOledRowMeta[0].isSent = false; gOledRowMeta[0].msgId = 0;
    oledScrollAddItem(&gOledEspNowState.messageList, noMsgLine1, noMsgLine2, false, nullptr);
    return;
  }

  // Restore scroll position with auto-follow-tail behavior:
  //
  //   1. If the user WAS at the bottom (most recent message visible as the
  //      last row) AND a new message just arrived (count grew), snap to the
  //      new bottom — standard chat UX where reading the latest messages
  //      keeps following live updates.
  //
  //   2. Otherwise (user scrolled up reading history, or count unchanged,
  //      or count shrank from ring-buffer aging), restore the saved
  //      position clamped to the new range. Reading older messages stays
  //      sticky — incoming traffic won't yank you away.
  //
  // "At bottom" is defined as: the saved scroll window's bottom edge
  // (scrollOffset + visibleLines) had reached or passed the previous last
  // index. This intentionally matches the moment the most-recent message
  // is on screen, not just a strict equality, so it works whether the list
  // fits entirely on screen or only the tail does.
  int newCount = gOledEspNowState.messageList.itemCount;
  if (newCount > 0) {
    int vis = gOledEspNowState.messageList.visibleLines;
    bool wasAtBottom = (prevItemCount > 0 &&
                        (savedScrollOffset + vis) >= prevItemCount);
    bool messageArrived = (newCount > prevItemCount);

    int idx, off;
    if (wasAtBottom && messageArrived) {
      // Follow the tail: jump to the newest message.
      idx = newCount - 1;
      off = max(0, newCount - vis);
    } else {
      // Sticky restore + clamp.
      idx = savedSelectedIndex;
      off = savedScrollOffset;
      if (idx >= newCount) idx = newCount - 1;
      if (idx < 0)         idx = 0;
      int maxOff = max(0, newCount - vis);
      if (off > maxOff) off = maxOff;
      if (off < 0)      off = 0;
    }
    gOledEspNowState.messageList.selectedIndex = idx;
    gOledEspNowState.messageList.scrollOffset  = off;
  }
}

String oledEspNowFormatMac(const uint8_t* mac) {
  if (!mac) return "00:00:00:00:00:00";
  return macToDisplayStr(mac);  // canonical DISPLAY form (System_Utils.h)
}

void oledEspNowDrawStatusIcon(Adafruit_SSD1306* display, int x, int y, bool delivered) {
  if (!display) return;
  
  // Draw checkmark(s)
  if (delivered) {
    // Double checkmark for delivered
    display->drawLine(x, y + 2, x + 1, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 1, y + 3, x + 3, y + 1, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 2, y + 2, x + 3, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 3, y + 3, x + 5, y + 1, DISPLAY_COLOR_WHITE);
  } else {
    // Single checkmark for sent
    display->drawLine(x, y + 2, x + 1, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 1, y + 3, x + 3, y + 1, DISPLAY_COLOR_WHITE);
  }
}

// =============================================================================
// Buffer Safety Validation
// =============================================================================

bool oledEspNowValidateMessagePtr(const void* msgPtr, const uint8_t* peerMac) {
  if (!msgPtr || !peerMac || !gEspNow || !gEspNow->peerMessageHistories) return false;
  
  // Find the peer history for this MAC.
  // Bound by peerHistoryCapacity (the dynamically-grown size of the
  // peerMessageHistories array), NOT gMeshPeerSlots — that sizes the separate
  // gMeshPeers/gMeshPeerMeta arrays and is normally larger, which would overscan
  // this allocation. Matches findOrCreatePeerHistory/getAllMessages.
  PeerMessageHistory* history = nullptr;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    if (gEspNow->peerMessageHistories[i].active && 
        memcmp(gEspNow->peerMessageHistories[i].peerMac, peerMac, 6) == 0) {
      history = &gEspNow->peerMessageHistories[i];
      break;
    }
  }
  
  if (!history) return false;

  // Pointer must land inside EITHER the received ring or the sent ring (the
  // conversation view zero-copies refs from both).
  const ReceivedTextMessage* msg = (const ReceivedTextMessage*)msgPtr;
  bool inRecv = (msg >= &history->messages[0] && msg < &history->messages[MESSAGES_PER_DEVICE]);
  bool inSent = (msg >= &history->sent[0]     && msg < &history->sent[MESSAGES_PER_DEVICE]);
  if (!inRecv && !inSent) return false;

  // Check if message is still active
  return msg->active;
}

bool oledEspNowValidateDevicePtr(const void* devicePtr) {
  if (!devicePtr || !gEspNow) return false;
  
  // Check if pointer is within the device array bounds
  const EspNowDevice* device = (const EspNowDevice*)devicePtr;
  const EspNowDevice* arrayStart = &gEspNow->devices[0];
  const EspNowDevice* arrayEnd = &gEspNow->devices[16];  // devices[16] in EspNowSystem
  
  return (device >= arrayStart && device < arrayEnd);
}

// =============================================================================
// ESP-NOW Remote Form and Text Message Functions (merged from oled_espnow_remote.cpp)
// =============================================================================

void oledEspNowDisplayRemoteForm(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // If keyboard is active, show it instead of the form
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(display);
    return;
  }
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, OLED_CONTENT_START_Y);
  
  // Display form fields with selection indicator
  // Field 0: Username
  if (gOledEspNowState.remoteFormField == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> User: ");
  display->println(gOledEspNowState.remoteUsername.length() > 0 ? 
                   gOledEspNowState.remoteUsername.c_str() : "_____");
  
  // Field 1: Password
  if (gOledEspNowState.remoteFormField == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Pass: ");
  // Show asterisks for password
  if (gOledEspNowState.remotePassword.length() > 0) {
    for (size_t i = 0; i < gOledEspNowState.remotePassword.length(); i++) {
      display->print("*");
    }
    display->println();
  } else {
    display->println("_____");
  }
  
  // Field 2: Command
  if (gOledEspNowState.remoteFormField == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Cmd: ");
  display->println(gOledEspNowState.remoteCommand.length() > 0 ? 
                   gOledEspNowState.remoteCommand.c_str() : "_____");
}

bool oledEspNowHandleRemoteFormInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Check if keyboard is active (inline editing) - handle this first
  if (oledKeyboardIsActive()) {
    // Let keyboard handle input
    oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    // Check if keyboard completed
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      switch (gOledEspNowState.remoteFormField) {
        case 0:
          gOledEspNowState.remoteUsername = String(text);
          break;
        case 1:
          gOledEspNowState.remotePassword = String(text);
          break;
        case 2:
          gOledEspNowState.remoteCommand = String(text);
          break;
      }
      oledKeyboardReset();
      return true;
    }
    
    // Check if keyboard cancelled
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      return true;
    }
    
    return true;  // Keyboard is active, consume all input
  }
  
  // Keyboard not active - handle form navigation
  // Navigate between fields using centralized navigation events
  if (gNavEvents.up && gOledEspNowState.remoteFormField > 0) {
    gOledEspNowState.remoteFormField--;
    return true;
  }
  if (gNavEvents.down && gOledEspNowState.remoteFormField < 2) {
    gOledEspNowState.remoteFormField++;
    return true;
  }
  
  // A button: Edit current field with keyboard
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    const char* title = "";
    const char* initialText = "";
    
    switch (gOledEspNowState.remoteFormField) {
      case 0:
        title = "Username:";
        initialText = gOledEspNowState.remoteUsername.c_str();
        break;
      case 1:
        title = "Password:";
        initialText = gOledEspNowState.remotePassword.c_str();
        break;
      case 2:
        title = "Command:";
        initialText = gOledEspNowState.remoteCommand.c_str();
        break;
    }
    
    oledKeyboardInit(title, initialText, 64);
    return true;
  }
  
  // Y button: Send remote command
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    oledEspNowSendRemoteCommand();
    gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  // B button: Cancel form
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  return false;
}

void oledEspNowSendTextMessage() {
  if (!gEspNow || gOledEspNowState.textMessageBuffer.length() == 0) return;
  
  // Send text message to selected device
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOledEspNowState.selectedDeviceMac[0],
           gOledEspNowState.selectedDeviceMac[1],
           gOledEspNowState.selectedDeviceMac[2],
           gOledEspNowState.selectedDeviceMac[3],
           gOledEspNowState.selectedDeviceMac[4],
           gOledEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnowsend json <mac> <message>. The "json" flag makes the
  // handler return {"ok":true,"msgId":N}. cmd_espnow_send records the sent text
  // into the shared peer history itself, so the conversation row appears on the
  // next refresh (and on every other interface too); we only use the ack here to
  // confirm success and keep the view re-rendering so the row can animate
  // Sent → Delivered as the ACK arrives asynchronously.
  char cmdBuf[256];
  snprintf(cmdBuf, sizeof(cmdBuf), "espnowsend json %s %s", macStr, gOledEspNowState.textMessageBuffer.c_str());
  char resp[96];
  if (executeOLEDCommandWithResult(cmdBuf, resp, sizeof(resp))) {
    const char* p = strstr(resp, "\"msgId\":");  // cheap strstr — avoids a JSON parse
    if (p && (uint32_t)strtoul(p + 8, nullptr, 10) != 0) {
      oledMarkDirtyUntil(millis() + 13000);
    }
  }

  // Clear buffer
  gOledEspNowState.textMessageBuffer = "";

  // Refresh message list
  gOledEspNowState.needsRefresh = true;
}

void oledEspNowSendRemoteCommand() {
  if (!gEspNow) return;
  
  // Validate that all fields are filled
  if (gOledEspNowState.remoteUsername.length() == 0 ||
      gOledEspNowState.remotePassword.length() == 0 ||
      gOledEspNowState.remoteCommand.length() == 0) {
    return;  // Don't send if any field is empty
  }
  
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOledEspNowState.selectedDeviceMac[0],
           gOledEspNowState.selectedDeviceMac[1],
           gOledEspNowState.selectedDeviceMac[2],
           gOledEspNowState.selectedDeviceMac[3],
           gOledEspNowState.selectedDeviceMac[4],
           gOledEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnow remote <mac> <username> <password> <command>
  char cmdBuf[384];
  snprintf(cmdBuf, sizeof(cmdBuf), "espnowremote %s %s %s %s", macStr,
           gOledEspNowState.remoteUsername.c_str(),
           gOledEspNowState.remotePassword.c_str(),
           gOledEspNowState.remoteCommand.c_str());
  executeOLEDCommand(cmdBuf);
  
  // Clear form
  gOledEspNowState.remoteUsername = "";
  gOledEspNowState.remotePassword = "";
  gOledEspNowState.remoteCommand = "";
  
  // Refresh message list
  gOledEspNowState.needsRefresh = true;
}

// =============================================================================
// ESP-NOW Settings Menu
// =============================================================================

// Settings menu items: 0=Name, 1=Room, 2=Zone, 3=Friendly Name, 4=Tags, 5=Stationary, 6=Passphrase, 7=Role, 8=MasterMAC, 9=BackupMAC, 10=Channel
#define ESPNOW_SETTINGS_COUNT 11

static const char* espnowSettingsLabels[ESPNOW_SETTINGS_COUNT] = {
  "Device Name",
  "Room",
  "Zone",
  "Friendly Name",
  "Tags",
  "Stationary",
  "Passphrase",
  "Role",
  "Master MAC",
  "Backup MAC",
  "Channel"
};

void oledEspNowOpenSettings() {
  gOledEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
  gOledEspNowState.settingsMenuIndex = 0;
  gOledEspNowState.settingsEditField = -1;
}

void oledEspNowDisplaySettings(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header shows "ESP-NOW", start content below it
  // Scrollable list: 4 visible items in content area
  int startY = OLED_CONTENT_START_Y;
  int lineHeight = 9;
  const int maxVisible = (OLED_CONTENT_HEIGHT) / lineHeight;  // ~4 items
  
  // Calculate scroll offset to keep selection visible
  static int settingsScrollOffset = 0;
  if (gOledEspNowState.settingsMenuIndex < settingsScrollOffset) {
    settingsScrollOffset = gOledEspNowState.settingsMenuIndex;
  } else if (gOledEspNowState.settingsMenuIndex >= settingsScrollOffset + maxVisible) {
    settingsScrollOffset = gOledEspNowState.settingsMenuIndex - maxVisible + 1;
  }
  
  for (int v = 0; v < maxVisible && (settingsScrollOffset + v) < ESPNOW_SETTINGS_COUNT; v++) {
    int i = settingsScrollOffset + v;
    int y = startY + v * lineHeight;
    
    // Selection indicator
    if (i == gOledEspNowState.settingsMenuIndex) {
      display->fillRect(0, y, 2, lineHeight - 1, DISPLAY_COLOR_WHITE);
    }
    
    display->setCursor(4, y);
    display->print(espnowSettingsLabels[i]);
    display->print(": ");
    
    // Show current value (truncated)
    String value;
    switch (i) {
      case 0: // Device Name
        value = gSettings.espnowDeviceName;
        if (value.length() == 0) value = "(not set)";
        break;
      case 1: // Room
        value = gSettings.espnowRoom;
        if (value.length() == 0) value = "(not set)";
        break;
      case 2: // Zone
        value = gSettings.espnowZone;
        if (value.length() == 0) value = "(not set)";
        break;
      case 3: // Friendly Name
        value = gSettings.espnowFriendlyName;
        if (value.length() == 0) value = "(not set)";
        break;
      case 4: // Tags
        value = gSettings.espnowTags;
        if (value.length() == 0) value = "(not set)";
        break;
      case 5: // Stationary
        value = gSettings.espnowStationary ? "Yes" : "No";
        break;
      case 6: // Passphrase
        value = gSettings.meshes[0].passphrase.length() > 0 ? "****" : "(not set)";
        break;
      case 7: // Role
        if (gSettings.meshRole == MESH_ROLE_MASTER) value = "Master";
        else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) value = "Backup";
        else value = "Worker";
        break;
      case 8: // Master MAC
        value = gSettings.meshMasterMAC;
        if (value.length() == 0) value = "(auto)";
        break;
      case 9: // Backup MAC
        value = gSettings.meshBackupMAC;
        if (value.length() == 0) value = "(none)";
        break;
      case 10: // Channel
        value = gSettings.espnowChannel == 0 ? "Auto" : String(gSettings.espnowChannel);
        break;
    }
    
    // Truncate value if needed
    int labelLen = strlen(espnowSettingsLabels[i]) + 2;  // label + ": "
    int maxValueLen = (128 - 4 - labelLen * 6) / 6;
    if (value.length() > maxValueLen && maxValueLen > 3) {
      value = value.substring(0, maxValueLen - 1); value += '~';
    }
    display->print(value);
  }
  
  // Scroll indicators
  if (settingsScrollOffset > 0) {
    display->setCursor(120, startY);
    display->print("\x18");
  }
  if (settingsScrollOffset + maxVisible < ESPNOW_SETTINGS_COUNT) {
    display->setCursor(120, startY + (maxVisible - 1) * lineHeight);
    display->print("\x19");
  }
  
  // Note: Footer is drawn by global render loop
}

bool oledEspNowHandleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Navigation
  if (gNavEvents.up && gOledEspNowState.settingsMenuIndex > 0) {
    gOledEspNowState.settingsMenuIndex--;
    return true;
  }
  if (gNavEvents.down && gOledEspNowState.settingsMenuIndex < ESPNOW_SETTINGS_COUNT - 1) {
    gOledEspNowState.settingsMenuIndex++;
    return true;
  }
  
  // A button: Edit selected item
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    gOledEspNowState.settingsEditField = gOledEspNowState.settingsMenuIndex;
    
    // Stationary: toggle boolean via command
    if (gOledEspNowState.settingsEditField == 5) {
      executeOLEDCommand(gSettings.espnowStationary ? "espnowstationary 0" : "espnowstationary 1");
      gOledEspNowState.settingsEditField = -1;
      return true;
    }
    
    // Role: cycle through options via command
    if (gOledEspNowState.settingsEditField == 7) {
      if (gSettings.meshRole == MESH_ROLE_WORKER) {
        executeOLEDCommand("espnowmeshrole master");
      } else if (gSettings.meshRole == MESH_ROLE_MASTER) {
        executeOLEDCommand("espnowmeshrole backup");
      } else {
        executeOLEDCommand("espnowmeshrole worker");
      }
      gOledEspNowState.settingsEditField = -1;
      return true;
    }

    // Channel: cycle the useful ladder Auto -> 1 -> 6 -> 11 via command. These
    // are the non-overlapping 2.4GHz channels plus auto; arbitrary 1-13 values
    // are settable from the web UI / CLI (espnowchannel <n>). Off-ladder values
    // set elsewhere snap forward into the ladder on the next press.
    if (gOledEspNowState.settingsEditField == 10) {
      uint8_t c = gSettings.espnowChannel;
      const char* next = (c == 0) ? "1" : (c < 6) ? "6" : (c < 11) ? "11" : "auto";
      executeOLEDCommand(String("espnowchannel ") + next);
      gOledEspNowState.settingsEditField = -1;
      return true;
    }

    // For other fields, open keyboard
    const char* prompt = espnowSettingsLabels[gOledEspNowState.settingsEditField];
    String initialValue = "";
    int maxLen = 32;
    
    switch (gOledEspNowState.settingsEditField) {
      case 0: // Device Name
        initialValue = gSettings.espnowDeviceName;
        maxLen = 16;
        break;
      case 1: // Room
        initialValue = gSettings.espnowRoom;
        maxLen = 30;
        break;
      case 2: // Zone
        initialValue = gSettings.espnowZone;
        maxLen = 30;
        break;
      case 3: // Friendly Name
        initialValue = gSettings.espnowFriendlyName;
        maxLen = 46;
        break;
      case 4: // Tags
        initialValue = gSettings.espnowTags;
        maxLen = 62;
        break;
      case 6: // Passphrase
        initialValue = "";  // Don't show existing passphrase
        maxLen = 32;
        break;
      case 8: // Master MAC
        initialValue = gSettings.meshMasterMAC;
        maxLen = 17;  // XX:XX:XX:XX:XX:XX
        break;
      case 9: // Backup MAC
        initialValue = gSettings.meshBackupMAC;
        maxLen = 17;
        break;
    }
    
    oledKeyboardInit(prompt, initialValue.c_str(), maxLen);
    gOledEspNowState.currentView = ESPNOW_VIEW_SETTINGS_KEYBOARD;
    return true;
  }
  
  // B button: Back to main menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
    return true;
  }
  
  return false;
}

void oledEspNowApplySettingsEdit(const String& value) {
  String cmd;
  switch (gOledEspNowState.settingsEditField) {
    case 0: // Device Name
      cmd = "espnowsetname " + value;
      break;
    case 1: // Room
      cmd = "espnowroom " + value;
      break;
    case 2: // Zone
      cmd = "espnowzone " + value;
      break;
    case 3: // Friendly Name
      cmd = "espnowfriendlyname " + value;
      break;
    case 4: // Tags
      cmd = "espnowtags " + value;
      break;
    case 6: // Passphrase
      if (value.length() > 0) {
        // OLED first-time setup configures the primary mesh's passphrase.
        // Multi-mesh setup is done from the CLI / web UI later.
        cmd = "espnowsetpassphrase primary \"" + value + "\"";
      }
      break;
    case 8: // Master MAC
      cmd = "espnowmeshmaster " + value;
      break;
    case 9: // Backup MAC
      cmd = "espnowmeshbackup " + value;
      break;
  }
  if (cmd.length() > 0) {
    executeOLEDCommand(cmd);
  }
  gOledEspNowState.settingsEditField = -1;
}

// ============================================================================
// Device Configuration Menu (Remote Device)
// ============================================================================

// Device config menu items: 0=Restart, 1=Role, 2=Name, 3=Room, 4=Zone, 5=PrettyName, 6=Unpair
#define DEVICE_CONFIG_COUNT 7

static const char* deviceConfigLabels[DEVICE_CONFIG_COUNT] = {
  "Restart Device",
  "Set Role",
  "Set Name",
  "Set Room",
  "Set Zone",
  "Set Pretty Name",
  "Unpair Device"
};

void oledEspNowOpenDeviceConfig() {
  gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
  gOledEspNowState.deviceConfigMenuIndex = 0;
  gOledEspNowState.deviceConfigEditField = -1;
}

void oledEspNowDisplayDeviceConfig(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header with device name
  display->setCursor(0, 0);
  display->print("Config: ");
  String name = gOledEspNowState.selectedDeviceName;
  if (name.length() > 14) { name = name.substring(0, 13); name += '~'; }
  display->println(name);
  
  display->drawFastHLine(0, 9, 128, DISPLAY_COLOR_WHITE);
  
  // Menu items
  int startY = 12;
  int lineHeight = 10;
  
  for (int i = 0; i < DEVICE_CONFIG_COUNT; i++) {
    int y = startY + i * lineHeight;
    if (y > 48) break;
    
    // Selection indicator
    if (i == gOledEspNowState.deviceConfigMenuIndex) {
      display->fillRect(0, y, 2, lineHeight - 1, DISPLAY_COLOR_WHITE);
    }
    
    display->setCursor(4, y);
    display->print(deviceConfigLabels[i]);
  }
  
  // Note: Footer is drawn by global render loop
}

bool oledEspNowHandleDeviceConfigInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Navigation
  if (gNavEvents.up && gOledEspNowState.deviceConfigMenuIndex > 0) {
    gOledEspNowState.deviceConfigMenuIndex--;
    return true;
  }
  if (gNavEvents.down && gOledEspNowState.deviceConfigMenuIndex < DEVICE_CONFIG_COUNT - 1) {
    gOledEspNowState.deviceConfigMenuIndex++;
    return true;
  }
  
  // A button: Execute selected action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             gOledEspNowState.selectedDeviceMac[0],
             gOledEspNowState.selectedDeviceMac[1],
             gOledEspNowState.selectedDeviceMac[2],
             gOledEspNowState.selectedDeviceMac[3],
             gOledEspNowState.selectedDeviceMac[4],
             gOledEspNowState.selectedDeviceMac[5]);
    
    switch (gOledEspNowState.deviceConfigMenuIndex) {
      case 0: // Restart Device
        {
          char cmdBuf[64];
          snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s restart", macStr);
          executeOLEDCommand(cmdBuf);
          broadcastOutput("[ESP-NOW] Sent restart command");
          gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        }
        break;
        
      case 1: // Set Role
        gOledEspNowState.deviceConfigEditField = 1;
        oledKeyboardInit("Role (master/backup/worker):", "", 16);
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 2: // Set Name
        gOledEspNowState.deviceConfigEditField = 2;
        oledKeyboardInit("Device Name:", gOledEspNowState.selectedDeviceName.c_str(), 16);
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 3: // Set Room
        gOledEspNowState.deviceConfigEditField = 3;
        oledKeyboardInit("Room:", "", 16);
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 4: // Set Zone
        gOledEspNowState.deviceConfigEditField = 4;
        oledKeyboardInit("Zone:", "", 16);
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 5: // Set Pretty Name
        gOledEspNowState.deviceConfigEditField = 5;
        oledKeyboardInit("Pretty Name:", "", 24);
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 6: // Unpair Device
        oledEspNowUnpairDevice();
        break;
    }
    return true;
  }
  
  // B button: Back to device detail
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  return false;
}

void oledEspNowApplyDeviceConfigEdit(const String& value) {
  if (value.length() == 0) {
    gOledEspNowState.deviceConfigEditField = -1;
    return;
  }
  
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOledEspNowState.selectedDeviceMac[0],
           gOledEspNowState.selectedDeviceMac[1],
           gOledEspNowState.selectedDeviceMac[2],
           gOledEspNowState.selectedDeviceMac[3],
           gOledEspNowState.selectedDeviceMac[4],
           gOledEspNowState.selectedDeviceMac[5]);
  
  switch (gOledEspNowState.deviceConfigEditField) {
    case 1: // Set Role
      {
        char cmdBuf[96];
        snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s meshrole %s", macStr, value.c_str());
        executeOLEDCommand(cmdBuf);
        broadcastOutput("[ESP-NOW] Sent role change command");
      }
      break;
      
    case 2: // Set Name
      {
        char cmdBuf[96];
        snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s espnowname %s", macStr, value.c_str());
        executeOLEDCommand(cmdBuf);
        gOledEspNowState.selectedDeviceName = value;
        broadcastOutput("[ESP-NOW] Sent name change command");
      }
      break;
      
    case 3: // Set Room
      {
        char cmdBuf[96];
        snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s room %s", macStr, value.c_str());
        executeOLEDCommand(cmdBuf);
        broadcastOutput("[ESP-NOW] Sent room change command");
      }
      break;
      
    case 4: // Set Zone
      {
        char cmdBuf[96];
        snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s zone %s", macStr, value.c_str());
        executeOLEDCommand(cmdBuf);
        broadcastOutput("[ESP-NOW] Sent zone change command");
      }
      break;
      
    case 5: // Set Pretty Name
      {
        char cmdBuf[96];
        snprintf(cmdBuf, sizeof(cmdBuf), "espnow cmd %s prettyname %s", macStr, value.c_str());
        executeOLEDCommand(cmdBuf);
        broadcastOutput("[ESP-NOW] Sent pretty name change command");
      }
      break;
  }
  
  gOledEspNowState.deviceConfigEditField = -1;
}

// ============================================================================
// Remote File Browsing State and Functions
// ============================================================================

// Remote file browser state
struct RemoteFileBrowseState {
  bool active;                    // Remote file browse mode active
  bool pending;                   // Waiting for response
  bool hasData;                   // Have data to display
  uint8_t targetMac[6];           // Target device MAC
  char currentPath[128];          // Current browse path
  char items[10][64];             // File/folder names (max 10 items displayed)
  bool isFolder[10];              // Is item a folder
  int itemCount;                  // Number of items
  int selectedIndex;              // Currently selected item
  int scrollOffset;               // Scroll offset for display
};
EXT_RAM_BSS_ATTR static RemoteFileBrowseState gRemoteFileBrowse;

void oledEspNowSendBrowseRequest(const char* path) {
  if (!gEspNow || !gEspNow->initialized || !gEspNow->encryptionEnabled) {
    return;
  }
  
  // Need credentials - for now use a stored admin credential or prompt
  // This is a simplified version - in production you'd want stored credentials
  gRemoteFileBrowse.pending = true;
  strncpy(gRemoteFileBrowse.currentPath, path, sizeof(gRemoteFileBrowse.currentPath) - 1);
  
  // Build and send FILE_BROWSE message
  // Note: This requires stored credentials - placeholder for now
  broadcastOutput("[ESP-NOW] Remote file browse requires stored credentials (not yet implemented)");
}

void oledEspNowDisplayRemoteFiles(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  
  if (gRemoteFileBrowse.pending) {
    display->println("Remote Files");
    display->println();
    display->println("Loading...");
    return;
  }
  
  if (!gRemoteFileBrowse.hasData) {
    display->println("Remote Files");
    display->println();
    display->println("No data");
    display->println();
    display->println("Press A to browse");
    return;
  }
  
  // Display path
  display->print("Path: ");
  String pathStr = gRemoteFileBrowse.currentPath;
  if (pathStr.length() > 15) pathStr = "..." + pathStr.substring(pathStr.length() - 12);
  display->println(pathStr);
  
  display->drawFastHLine(0, 9, 128, DISPLAY_COLOR_WHITE);
  
  // Display files
  int startY = 12;
  int visibleItems = 5;
  for (int i = 0; i < visibleItems && (i + gRemoteFileBrowse.scrollOffset) < gRemoteFileBrowse.itemCount; i++) {
    int idx = i + gRemoteFileBrowse.scrollOffset;
    int y = startY + (i * 9);
    
    if (idx == gRemoteFileBrowse.selectedIndex) {
      display->fillRect(0, y, 128, 9, DISPLAY_COLOR_WHITE);
      display->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      display->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    display->setCursor(2, y + 1);
    if (gRemoteFileBrowse.isFolder[idx]) {
      display->print("[D] ");
    } else {
      display->print("    ");
    }
    display->print(gRemoteFileBrowse.items[idx]);
  }
  
  display->setTextColor(DISPLAY_COLOR_WHITE);
}

bool oledEspNowHandleRemoteFilesInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Handle navigation
  if (deltaY < 0 && gRemoteFileBrowse.selectedIndex > 0) {
    gRemoteFileBrowse.selectedIndex--;
    if (gRemoteFileBrowse.selectedIndex < gRemoteFileBrowse.scrollOffset) {
      gRemoteFileBrowse.scrollOffset = gRemoteFileBrowse.selectedIndex;
    }
    return true;
  }
  if (deltaY > 0 && gRemoteFileBrowse.selectedIndex < gRemoteFileBrowse.itemCount - 1) {
    gRemoteFileBrowse.selectedIndex++;
    if (gRemoteFileBrowse.selectedIndex >= gRemoteFileBrowse.scrollOffset + 5) {
      gRemoteFileBrowse.scrollOffset = gRemoteFileBrowse.selectedIndex - 4;
    }
    return true;
  }
  
  return false;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

// ============================================================================
// Stub for storing remote file browse results
// This is outside the OLED guard so it can be called from ESP-NOW handler
// ============================================================================
#if ENABLE_ESPNOW
#include <ArduinoJson.h>

void storeRemoteFileBrowseResult(const uint8_t* mac, const char* path, JsonArray& files) {
#if ENABLE_OLED_DISPLAY
  // Store results in remote file browse state
  extern RemoteFileBrowseState gRemoteFileBrowse;
  
  gRemoteFileBrowse.pending = false;
  gRemoteFileBrowse.hasData = true;
  memcpy(gRemoteFileBrowse.targetMac, mac, 6);
  strncpy(gRemoteFileBrowse.currentPath, path, sizeof(gRemoteFileBrowse.currentPath) - 1);
  
  gRemoteFileBrowse.itemCount = 0;
  gRemoteFileBrowse.selectedIndex = 0;
  gRemoteFileBrowse.scrollOffset = 0;
  
  for (JsonVariant file : files) {
    if (gRemoteFileBrowse.itemCount >= 10) break;
    
    const char* name = file["name"] | "";
    const char* type = file["type"] | "file";
    
    strncpy(gRemoteFileBrowse.items[gRemoteFileBrowse.itemCount], name, 63);
    gRemoteFileBrowse.items[gRemoteFileBrowse.itemCount][63] = '\0';
    gRemoteFileBrowse.isFolder[gRemoteFileBrowse.itemCount] = (strcmp(type, "folder") == 0);
    gRemoteFileBrowse.itemCount++;
  }
  
  INFO_ESPNOWF("[FILE_BROWSE] Stored %d items from path '%s'", gRemoteFileBrowse.itemCount, path);
#else
  // OLED not enabled - just log
  (void)mac;
  (void)path;
  (void)files;
#endif
}

#endif // ENABLE_ESPNOW
