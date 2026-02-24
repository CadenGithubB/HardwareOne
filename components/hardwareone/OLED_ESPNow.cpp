#include "OLED_ESPNow.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_ESPNow.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif

// Mesh health data for online/offline status display
// gMeshPeers, gMeshPeerMeta, gMeshPeerSlots, isMeshPeerAlive, getMeshPeerHealth
// are all declared in System_ESPNow.h (already included above)

// Lookup MeshPeerMeta by MAC (returns nullptr if not found)
static MeshPeerMeta* findPeerMeta(const uint8_t mac[6]) {
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive && memcmp(gMeshPeerMeta[i].mac, mac, 6) == 0) {
      return &gMeshPeerMeta[i];
    }
  }
  return nullptr;
}

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
OLEDEspNowState gOLEDEspNowState;

void oledEspNowInit() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
  gOLEDEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  gOLEDEspNowState.modeSelectorIndex = 0;
  gOLEDEspNowState.modeSelectorActive = false;
  gOLEDEspNowState.lastUpdate = 0;
  gOLEDEspNowState.needsRefresh = true;
  memset(gOLEDEspNowState.selectedDeviceMac, 0, 6);
  gOLEDEspNowState.selectedDeviceName = "";
  
  // Text mode state
  gOLEDEspNowState.textMessageBuffer = "";
  
  // Remote mode state
  gOLEDEspNowState.remoteFormField = 0;
  gOLEDEspNowState.remoteUsername = "";
  gOLEDEspNowState.remotePassword = "";
  gOLEDEspNowState.remoteCommand = "";
  
  // Initialize scrolling lists
  oledScrollInit(&gOLEDEspNowState.deviceList, "ESP-NOW Devices", 3);
  oledScrollInit(&gOLEDEspNowState.messageList, nullptr, 3);
  
  // Settings menu state (local)
  gOLEDEspNowState.settingsMenuIndex = 0;
  gOLEDEspNowState.settingsEditField = -1;
  
  // Device config menu state (remote)
  gOLEDEspNowState.deviceConfigMenuIndex = 0;
  gOLEDEspNowState.deviceConfigEditField = -1;
  
  // Device list filtering and sorting
  gOLEDEspNowState.filterMode = 0;  // All devices
  gOLEDEspNowState.sortMode = 0;    // Sort by name
  memset(gOLEDEspNowState.filterValue, 0, sizeof(gOLEDEspNowState.filterValue));
  
  // Main menu state (Bluetooth-style)
  gOLEDEspNowState.mainMenuSelection = 0;
  gOLEDEspNowState.mainMenuScrollOffset = 0;
  gOLEDEspNowState.showingStatusDetail = false;
  
  // Rooms view state
  gOLEDEspNowState.roomsMenuSelection = 0;
  gOLEDEspNowState.roomsDeviceSelection = 0;
  gOLEDEspNowState.inRoomDeviceList = false;
  
  // Start at main menu if ESP-NOW is initialized
  if (gEspNow && gEspNow->initialized) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
  }
}

void oledEspNowShowInitPrompt() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_INIT_PROMPT;
}

void oledEspNowShowNameKeyboard() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_NAME_KEYBOARD;
  const char* initialText = "";
  if (gSettings.espnowDeviceName.length() > 0) {
    initialText = gSettings.espnowDeviceName.c_str();
  }
  oledKeyboardInit("Device Name:", initialText, 20);
}

void oledEspNowDisplay(Adafruit_SSD1306* display) {
  if (!display) return;

  if (gOLEDEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT && gEspNow && gEspNow->initialized) {
    oledEspNowInit();
  }
  
  // Handle views that don't require ESP-NOW to be initialized
  if (gOLEDEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT ||
      gOLEDEspNowState.currentView == ESPNOW_VIEW_NAME_KEYBOARD) {
    // These views are shown before ESP-NOW init
    switch (gOLEDEspNowState.currentView) {
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
  if (now - gOLEDEspNowState.lastUpdate > 1000 || gOLEDEspNowState.needsRefresh) {
    if (gOLEDEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST) {
      oledEspNowRefreshDeviceList();
    } else if (gOLEDEspNowState.currentView == ESPNOW_VIEW_DEVICE_DETAIL) {
      oledEspNowRefreshMessages();
    }
    gOLEDEspNowState.lastUpdate = now;
    gOLEDEspNowState.needsRefresh = false;
  }
  
  // Display current view
  switch (gOLEDEspNowState.currentView) {
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
    // Count online devices
    int onlineCount = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeerMeta[i].isActive) {
        MeshPeerHealth* health = getMeshPeerHealth(gMeshPeerMeta[i].mac, false);
        if (health && isMeshPeerAlive(health)) {
          onlineCount++;
        }
      }
    }
    display->print("Online: ");
    display->println(onlineCount);
  } else {
    display->println("Status: OFF");
  }
  
  // Calculate visible menu area (44px content - 10px status line = 34px for menu)
  const int kStatusHeight = 10;
  const int kLineHeight = 8;
  const int kMaxVisibleItems = (OLED_CONTENT_HEIGHT - kStatusHeight) / kLineHeight;  // 4 lines
  const int kTotalItems = ESPNOW_MENU_ITEM_COUNT;
  
  // Clamp selection
  if (gOLEDEspNowState.mainMenuSelection >= kTotalItems) {
    gOLEDEspNowState.mainMenuSelection = kTotalItems - 1;
  }
  if (gOLEDEspNowState.mainMenuSelection < 0) {
    gOLEDEspNowState.mainMenuSelection = 0;
  }
  
  // Adjust scroll offset to keep selection visible
  int& scrollOffset = gOLEDEspNowState.mainMenuScrollOffset;
  if (gOLEDEspNowState.mainMenuSelection < scrollOffset) {
    scrollOffset = gOLEDEspNowState.mainMenuSelection;
  }
  if (gOLEDEspNowState.mainMenuSelection >= scrollOffset + kMaxVisibleItems) {
    scrollOffset = gOLEDEspNowState.mainMenuSelection - kMaxVisibleItems + 1;
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
    if (itemIndex == gOLEDEspNowState.mainMenuSelection) {
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
  
  // Device count
  int totalDevices = 0, onlineDevices = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive) {
      totalDevices++;
      MeshPeerHealth* health = getMeshPeerHealth(gMeshPeerMeta[i].mac, false);
      if (health && isMeshPeerAlive(health)) onlineDevices++;
    }
  }
  display->print("Devices: ");
  display->print(onlineDevices);
  display->print("/");
  display->println(totalDevices);
  
  // Encryption status
  display->print("Encrypt: ");
  display->println(gEspNow && gEspNow->encryptionEnabled ? "Yes" : "No");
  
  // Channel
  display->print("Channel: ");
  display->println(gEspNow ? gEspNow->channel : 0);
  
  // Device name (truncate if too long)
  display->print("Name: ");
  String name = gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName : "(none)";
  if (name.length() > 15) name = name.substring(0, 14) + "~";
  display->println(name);
  
  // Note: Footer is drawn by global render loop
}

// Max rooms we can track on the OLED
#define ROOMS_MAX 16
#define ROOMS_DEVICES_MAX 16

// Cached room list (rebuilt on entry)
static struct {
  char name[32];
  int deviceCount;
} sRoomList[ROOMS_MAX];
static int sRoomCount = 0;

// Cached device list for selected room
static struct {
  char name[24];
  uint8_t mac[6];
  bool alive;
} sRoomDevices[ROOMS_DEVICES_MAX];
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
    
    const char* name = gMeshPeerMeta[i].friendlyName[0] ? gMeshPeerMeta[i].friendlyName :
                       gMeshPeerMeta[i].name[0] ? gMeshPeerMeta[i].name : "Unknown";
    strncpy(sRoomDevices[sRoomDeviceCount].name, name, 23);
    sRoomDevices[sRoomDeviceCount].name[23] = '\0';
    memcpy(sRoomDevices[sRoomDeviceCount].mac, gMeshPeerMeta[i].mac, 6);
    MeshPeerHealth* health = getMeshPeerHealth(gMeshPeerMeta[i].mac, false);
    sRoomDevices[sRoomDeviceCount].alive = health ? isMeshPeerAlive(health) : false;
    sRoomDeviceCount++;
  }
}

void oledEspNowDisplayRooms(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  if (!gOLEDEspNowState.inRoomDeviceList) {
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
    if (gOLEDEspNowState.roomsMenuSelection >= sRoomCount) {
      gOLEDEspNowState.roomsMenuSelection = sRoomCount - 1;
    }
    if (gOLEDEspNowState.roomsMenuSelection < 0) {
      gOLEDEspNowState.roomsMenuSelection = 0;
    }
    
    // Scroll offset
    static int roomsScrollOffset = 0;
    if (gOLEDEspNowState.roomsMenuSelection < roomsScrollOffset) {
      roomsScrollOffset = gOLEDEspNowState.roomsMenuSelection;
    } else if (gOLEDEspNowState.roomsMenuSelection >= roomsScrollOffset + maxVisible) {
      roomsScrollOffset = gOLEDEspNowState.roomsMenuSelection - maxVisible + 1;
    }
    
    for (int v = 0; v < maxVisible && (roomsScrollOffset + v) < sRoomCount; v++) {
      int idx = roomsScrollOffset + v;
      int y = OLED_CONTENT_START_Y + v * lineHeight;
      
      if (idx == gOLEDEspNowState.roomsMenuSelection) {
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
    display->print(sRoomList[gOLEDEspNowState.roomsMenuSelection].name);
    display->drawFastHLine(0, OLED_CONTENT_START_Y + 9, 128, DISPLAY_COLOR_WHITE);
    
    if (sRoomDeviceCount == 0) {
      display->setCursor(0, OLED_CONTENT_START_Y + 12);
      display->println("No devices");
      return;
    }
    
    // Clamp selection
    if (gOLEDEspNowState.roomsDeviceSelection >= sRoomDeviceCount) {
      gOLEDEspNowState.roomsDeviceSelection = sRoomDeviceCount - 1;
    }
    if (gOLEDEspNowState.roomsDeviceSelection < 0) {
      gOLEDEspNowState.roomsDeviceSelection = 0;
    }
    
    const int lineHeight = 10;
    const int listStartY = OLED_CONTENT_START_Y + 11;
    const int maxVisible = (OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - listStartY) / lineHeight;  // ~3 items
    
    static int devScrollOffset = 0;
    if (gOLEDEspNowState.roomsDeviceSelection < devScrollOffset) {
      devScrollOffset = gOLEDEspNowState.roomsDeviceSelection;
    } else if (gOLEDEspNowState.roomsDeviceSelection >= devScrollOffset + maxVisible) {
      devScrollOffset = gOLEDEspNowState.roomsDeviceSelection - maxVisible + 1;
    }
    
    for (int v = 0; v < maxVisible && (devScrollOffset + v) < sRoomDeviceCount; v++) {
      int idx = devScrollOffset + v;
      int y = listStartY + v * lineHeight;
      
      if (idx == gOLEDEspNowState.roomsDeviceSelection) {
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


void oledEspNowDisplayPairing(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  display->println("== PAIRING MODE ==");
  display->println();
  display->println("Listening for new");
  display->println("devices...");
  display->println();
  display->println("New devices will");
  display->println("appear in list.");
  
  // Note: Footer is drawn by global render loop
}

// =============================================================================
// Main Menu Navigation
// =============================================================================

int oledEspNowGetMainMenuItemCount() {
  return ESPNOW_MENU_ITEM_COUNT;
}

void oledEspNowMainMenuUp() {
  if (gOLEDEspNowState.mainMenuSelection > 0) {
    gOLEDEspNowState.mainMenuSelection--;
  }
}

void oledEspNowMainMenuDown() {
  if (gOLEDEspNowState.mainMenuSelection < ESPNOW_MENU_ITEM_COUNT - 1) {
    gOLEDEspNowState.mainMenuSelection++;
  }
}

void oledEspNowMainMenuSelect() {
  switch (gOLEDEspNowState.mainMenuSelection) {
    case 0:  // Status
      gOLEDEspNowState.currentView = ESPNOW_VIEW_STATUS;
      break;
    case 1:  // Devices
      gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
      oledEspNowRefreshDeviceList();
      break;
    case 2:  // Rooms
      rebuildRoomList();
      gOLEDEspNowState.roomsMenuSelection = 0;
      gOLEDEspNowState.inRoomDeviceList = false;
      gOLEDEspNowState.currentView = ESPNOW_VIEW_ROOMS;
      break;
    case 3:  // Settings
      gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
      break;
    case 4:  // Start/Stop
      if (gEspNow && gEspNow->initialized) {
        extern const char* cmd_espnow_deinit(const String& cmd);
        cmd_espnow_deinit("");
      } else {
        extern const char* cmd_espnow_init(const String& cmd);
        cmd_espnow_init("");
      }
      break;
    case 5:  // Pairing
      gOLEDEspNowState.currentView = ESPNOW_VIEW_PAIRING;
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
  if (gOLEDEspNowState.filterMode == 1) {
    filterStr = "R";
  } else if (gOLEDEspNowState.filterMode == 2) {
    filterStr = "Z";
  }
  
  // Sort indicator: Name, Room, Status
  const char* sortStr = "N";
  if (gOLEDEspNowState.sortMode == 1) {
    sortStr = "Rm";
  } else if (gOLEDEspNowState.sortMode == 2) {
    sortStr = "St";
  }
  
  // Build title: "ESP-NOW [M] E R:Rm" (role, encrypted, filter, sort)
  if (gEspNow && gEspNow->encryptionEnabled) {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s E %s:%s", roleStr, filterStr[0] ? filterStr : "A", sortStr);
  } else {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s %s:%s", roleStr, filterStr[0] ? filterStr : "A", sortStr);
  }
  gOLEDEspNowState.deviceList.title = titleBuf;
  
  // Render device list using scrolling system
  oledScrollRender(display, &gOLEDEspNowState.deviceList, true, true);
}

void oledEspNowDisplayDeviceDetail(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Look up mesh metadata and health for this device
  MeshPeerMeta* meta = findPeerMeta(gOLEDEspNowState.selectedDeviceMac);
  MeshPeerHealth* health = getMeshPeerHealth(gOLEDEspNowState.selectedDeviceMac, false);
  bool alive = health ? isMeshPeerAlive(health) : false;
  
  // Draw header with device name + online indicator
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  
  // Prefer friendly name from metadata
  String header = "";
  if (meta && meta->friendlyName[0]) header = meta->friendlyName;
  else if (gOLEDEspNowState.selectedDeviceName.length() > 0) header = gOLEDEspNowState.selectedDeviceName;
  else header = oledEspNowFormatMac(gOLEDEspNowState.selectedDeviceMac);
  
  // Append online/offline indicator
  int maxNameLen = health ? 18 : 21;  // Reserve space for status if health data exists
  if ((int)header.length() > maxNameLen) header = header.substring(0, maxNameLen - 1) + "~";
  display->print(header);
  if (health) {
    display->print(alive ? " [+]" : " [-]");
  }
  display->println();
  
  // Line 2: room/zone or mode indicator
  display->setCursor(0, 8);
  if (meta && meta->room[0]) {
    display->print(meta->room);
    if (meta->zone[0]) {
      display->print("/");
      display->print(meta->zone);
    }
    // Show mode indicator compactly on the right
    const char* modeChar = gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT ? "T" :
                           gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE ? "R" : "F";
    int modeX = 128 - 6;  // Right-align single char
    display->setCursor(modeX, 8);
    display->print(modeChar);
  } else {
    display->print("Mode: ");
    if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
      display->println("Text");
    } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
      display->println("Remote");
    } else {
      display->println("File");
    }
  }
  
  // Draw separator
  display->drawFastHLine(0, 17, 128, DISPLAY_COLOR_WHITE);
  
  // If in File mode, show file browser prompt instead of message list
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_FILE) {
    display->setCursor(0, 20);
    display->setTextSize(1);
    display->println("File Transfer Mode");
    display->println();
    display->println("Press A to browse");
    display->println("files to send");
    display->println();
    return;
  }
  
  // Render message list (offset by header height)
  int yOffset = 18;
  int visibleStart = gOLEDEspNowState.messageList.scrollOffset;
  int visibleEnd = min(gOLEDEspNowState.messageList.itemCount, 
                       visibleStart + gOLEDEspNowState.messageList.visibleLines);
  
  int yPos = yOffset;
  int lineHeight = 8;
  
  for (int i = visibleStart; i < visibleEnd && yPos < 56; i++) {
    OLEDScrollItem* item = &gOLEDEspNowState.messageList.items[i];
    bool isSelected = (i == gOLEDEspNowState.messageList.selectedIndex);
    
    // Draw selection indicator
    if (isSelected) {
      display->fillRect(0, yPos, 2, lineHeight * 2, DISPLAY_COLOR_WHITE);
      display->setCursor(4, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    
    // Draw message text (truncated)
    String msg = item->line1;
    if (msg.length() > 20) msg = msg.substring(0, 19) + "~";
    display->println(msg);
    
    yPos += lineHeight;
    
    // Draw status/time on second line
    if (isSelected) {
      display->setCursor(4, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    display->println(item->line2);
    
    yPos += lineHeight;
  }
  
  // Show scrollbar if needed
  if (gOLEDEspNowState.messageList.itemCount > gOLEDEspNowState.messageList.visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    int scrollbarHeight = 38;  // 56 - 18
    int scrollbarY = yOffset;
    
    display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
    
    int thumbHeight = max(4, (scrollbarHeight * gOLEDEspNowState.messageList.visibleLines) / 
                            gOLEDEspNowState.messageList.itemCount);
    int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * 
                 gOLEDEspNowState.messageList.scrollOffset / 
                 max(1, gOLEDEspNowState.messageList.itemCount - gOLEDEspNowState.messageList.visibleLines);
    
    display->fillRect(scrollbarX - 1, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
  }
}

void oledEspNowDisplayModeSelect(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Draw semi-transparent background (drop-up menu effect)
  display->fillRect(20, 16, 88, 38, DISPLAY_COLOR_BLACK);
  display->drawRect(20, 16, 88, 38, DISPLAY_COLOR_WHITE);
  
  // Draw title
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(24, 18);
  display->println("Select Mode:");
  
  // Draw options
  display->setCursor(24, 28);
  if (gOLEDEspNowState.modeSelectorIndex == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Text     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Text     ");
  }
  
  display->setCursor(24, 36);
  if (gOLEDEspNowState.modeSelectorIndex == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Remote   ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Remote   ");
  }
  
  display->setCursor(24, 44);
  if (gOLEDEspNowState.modeSelectorIndex == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> File     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  File     ");
  }
}


bool oledEspNowHandleInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Handle input based on current view
  switch (gOLEDEspNowState.currentView) {
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
    case ESPNOW_VIEW_PAIRING:
      // B button: Back to main menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_ROOMS:
      if (!gOLEDEspNowState.inRoomDeviceList) {
        // Room list navigation
        if (gNavEvents.up && gOLEDEspNowState.roomsMenuSelection > 0) {
          gOLEDEspNowState.roomsMenuSelection--;
          return true;
        }
        if (gNavEvents.down && gOLEDEspNowState.roomsMenuSelection < sRoomCount - 1) {
          gOLEDEspNowState.roomsMenuSelection++;
          return true;
        }
        // A button: drill into room
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) && sRoomCount > 0) {
          rebuildRoomDeviceList(sRoomList[gOLEDEspNowState.roomsMenuSelection].name);
          gOLEDEspNowState.roomsDeviceSelection = 0;
          gOLEDEspNowState.inRoomDeviceList = true;
          return true;
        }
        // B button: back to main menu
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
          return true;
        }
      } else {
        // Device list within room navigation
        if (gNavEvents.up && gOLEDEspNowState.roomsDeviceSelection > 0) {
          gOLEDEspNowState.roomsDeviceSelection--;
          return true;
        }
        if (gNavEvents.down && gOLEDEspNowState.roomsDeviceSelection < sRoomDeviceCount - 1) {
          gOLEDEspNowState.roomsDeviceSelection++;
          return true;
        }
        // A button: select device -> go to device detail (if not local device)
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) && sRoomDeviceCount > 0) {
          int sel = gOLEDEspNowState.roomsDeviceSelection;
          // Check if this is a remote device (non-zero MAC)
          uint8_t zeroMac[6] = {0};
          if (memcmp(sRoomDevices[sel].mac, zeroMac, 6) != 0) {
            memcpy(gOLEDEspNowState.selectedDeviceMac, sRoomDevices[sel].mac, 6);
            gOLEDEspNowState.selectedDeviceName = sRoomDevices[sel].name;
            gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
            gOLEDEspNowState.needsRefresh = true;
            oledEspNowRefreshMessages();
          }
          return true;
        }
        // B button: back to room list
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          gOLEDEspNowState.inRoomDeviceList = false;
          return true;
        }
      }
      return false;
      
    case ESPNOW_VIEW_DEVICE_LIST:
      // Navigate device list using centralized navigation events
      if (gNavEvents.up) {
        oledScrollUp(&gOLEDEspNowState.deviceList);
        return true;
      }
      if (gNavEvents.down) {
        oledScrollDown(&gOLEDEspNowState.deviceList);
        return true;
      }
      
      // A button: Select device
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectDevice();
        return true;
      }
      
      // X button: Cycle filter mode (All -> Room -> Zone -> All)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        gOLEDEspNowState.filterMode = (gOLEDEspNowState.filterMode + 1) % 3;
        
        // If switching to room/zone filter, pick first available value
        if (gOLEDEspNowState.filterMode > 0 && gMeshPeerMeta) {
          memset(gOLEDEspNowState.filterValue, 0, sizeof(gOLEDEspNowState.filterValue));
          
          // Find first device with room or zone set
          for (int i = 0; i < gMeshPeerSlots; i++) {
            if (!gMeshPeerMeta[i].isActive) continue;
            
            if (gOLEDEspNowState.filterMode == 1 && gMeshPeerMeta[i].room[0]) {
              strncpy(gOLEDEspNowState.filterValue, gMeshPeerMeta[i].room, sizeof(gOLEDEspNowState.filterValue) - 1);
              break;
            } else if (gOLEDEspNowState.filterMode == 2 && gMeshPeerMeta[i].zone[0]) {
              strncpy(gOLEDEspNowState.filterValue, gMeshPeerMeta[i].zone, sizeof(gOLEDEspNowState.filterValue) - 1);
              break;
            }
          }
        }
        
        gOLEDEspNowState.needsRefresh = true;
        return true;
      }
      
      // Y button: Cycle sort mode (Name -> Room -> Status -> Name)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        gOLEDEspNowState.sortMode = (gOLEDEspNowState.sortMode + 1) % 3;
        gOLEDEspNowState.needsRefresh = true;
        return true;
      }
      
      // B button: Back to main menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_SETTINGS:
      return oledEspNowHandleSettingsInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_SETTINGS_KEYBOARD:
      if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
        if (oledKeyboardIsCompleted()) {
          String value = oledKeyboardGetText();
          oledEspNowApplySettingsEdit(value);
          oledKeyboardReset();
          gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        }
        return true;
      }
      // B button cancels keyboard
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_DEVICE_CONFIG:
      return oledEspNowHandleDeviceConfigInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD:
      if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
        if (oledKeyboardIsCompleted()) {
          String value = oledKeyboardGetText();
          oledEspNowApplyDeviceConfigEdit(value);
          oledKeyboardReset();
          gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
        }
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_DEVICE_DETAIL:
      // If in File mode, A button opens file browser
      if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_FILE) {
        // A button: Open file browser
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          extern OLEDMode currentOLEDMode;
          extern void resetOLEDFileBrowser();
          extern void pushOLEDMode(OLEDMode mode);
          pushOLEDMode(currentOLEDMode);  // Push so B returns here
          setOLEDMode(OLED_FILE_BROWSER);
          resetOLEDFileBrowser();
          return true;
        }
      } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
        // A button: Open text keyboard
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOLEDEspNowState.currentView = ESPNOW_VIEW_TEXT_KEYBOARD;
          gOLEDEspNowState.textMessageBuffer = "";
          oledKeyboardInit("Send Message:", "", 128);
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOLEDEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOLEDEspNowState.messageList);
          return true;
        }
        
        // Y button: Open device config
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          oledEspNowOpenDeviceConfig();
          return true;
        }
      } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
        // A button: Open remote form
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOLEDEspNowState.currentView = ESPNOW_VIEW_REMOTE_FORM;
          gOLEDEspNowState.remoteFormField = 0;
          gOLEDEspNowState.remoteUsername = "";
          gOLEDEspNowState.remotePassword = "";
          gOLEDEspNowState.remoteCommand = "";
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOLEDEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOLEDEspNowState.messageList);
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
      if (gNavEvents.up && gOLEDEspNowState.modeSelectorIndex > 0) {
        gOLEDEspNowState.modeSelectorIndex--;
        return true;
      }
      if (gNavEvents.down && gOLEDEspNowState.modeSelectorIndex < 2) {
        gOLEDEspNowState.modeSelectorIndex++;
        return true;
      }
      
      // A button: Select mode
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectMode();
        return true;
      }
      
      // B button: Cancel
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
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
        gOLEDEspNowState.textMessageBuffer = String(oledKeyboardGetText());
        oledEspNowSendTextMessage();
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      if (oledKeyboardIsCancelled()) {
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_REMOTE_FORM:
      return oledEspNowHandleRemoteFormInput(deltaX, deltaY, newlyPressed);
  }
  
  return false;  // Default: no input handled
}

void oledEspNowSelectDevice() {
  OLEDScrollItem* selected = oledScrollGetSelected(&gOLEDEspNowState.deviceList);
  if (!selected || !selected->userData) return;
  
  // Store selected device MAC
  EspNowDevice* device = (EspNowDevice*)selected->userData;
  memcpy(gOLEDEspNowState.selectedDeviceMac, device->mac, 6);
  gOLEDEspNowState.selectedDeviceName = String(device->name);
  
  // Switch to device detail view
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOLEDEspNowState.needsRefresh = true;
  
  // Refresh messages for this device
  oledEspNowRefreshMessages();
}

void oledEspNowBackToList() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
  gOLEDEspNowState.needsRefresh = true;
}

void oledEspNowOpenModeSelector() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_MODE_SELECT;
  // Map current mode to selector index: Text=0, Remote=1, File=2
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
    gOLEDEspNowState.modeSelectorIndex = 0;
  } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
    gOLEDEspNowState.modeSelectorIndex = 1;
  } else {
    gOLEDEspNowState.modeSelectorIndex = 2;  // File
  }
  gOLEDEspNowState.modeSelectorActive = true;
}

void oledEspNowSelectMode() {
  // Map selector index to mode: 0=Text, 1=Remote, 2=File
  if (gOLEDEspNowState.modeSelectorIndex == 0) {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  } else if (gOLEDEspNowState.modeSelectorIndex == 1) {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_REMOTE;
  } else {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_FILE;
  }
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOLEDEspNowState.modeSelectorActive = false;
}

void oledEspNowUnpairDevice() {
  if (!gEspNow) return;
  
  // Find device in paired list
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, gOLEDEspNowState.selectedDeviceMac, 6) == 0) {
      // Remove from ESP-NOW peer list
      esp_now_del_peer(gEspNow->devices[i].mac);
      
      // Shift remaining devices
      for (int j = i; j < gEspNow->deviceCount - 1; j++) {
        gEspNow->devices[j] = gEspNow->devices[j + 1];
      }
      gEspNow->deviceCount--;
      
      // Go back to device list
      oledEspNowBackToList();
      break;
    }
  }
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
  
  oledScrollClear(&gOLEDEspNowState.deviceList);
  
  // Get own MAC to skip self
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  // Static buffers for scroll item text (pointers stored in scroll items)
  static char line1Bufs[16][28];
  static char line2Bufs[16][28];
  
  // Build array of device entries for filtering and sorting
  static DeviceEntry entries[16];
  int entryCount = 0;
  
  for (int i = 0; i < gEspNow->deviceCount && entryCount < 16; i++) {
    EspNowDevice* device = &gEspNow->devices[i];
    
    // Skip own device
    if (memcmp(device->mac, myMac, 6) == 0) {
      continue;
    }
    
    // Look up mesh metadata and health for this device
    MeshPeerMeta* meta = findPeerMeta(device->mac);
    MeshPeerHealth* health = getMeshPeerHealth(device->mac, false);
    bool alive = health ? isMeshPeerAlive(health) : false;
    
    // Apply filter
    if (gOLEDEspNowState.filterMode == 1) {  // Filter by room
      if (!meta || !meta->room[0] || strcasecmp(meta->room, gOLEDEspNowState.filterValue) != 0) {
        continue;  // Skip devices not in selected room
      }
    } else if (gOLEDEspNowState.filterMode == 2) {  // Filter by zone
      if (!meta || !meta->zone[0] || strcasecmp(meta->zone, gOLEDEspNowState.filterValue) != 0) {
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
    if (gOLEDEspNowState.sortMode == 1) {  // Sort by room
      qsort(entries, entryCount, sizeof(DeviceEntry), compareByRoom);
    } else if (gOLEDEspNowState.sortMode == 2) {  // Sort by status
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
    
    oledScrollAddItem(&gOLEDEspNowState.deviceList, line1Bufs[i], line2Bufs[i], true, entry->device);
  }
  
  // If no visible devices (excluding self), show message
  if (entryCount == 0) {
    static const char* noDevLine1 = "No devices";
    static const char* noDevLine2;
    if (gOLEDEspNowState.filterMode > 0) {
      noDevLine2 = "(filtered out)";
    } else {
      noDevLine2 = "Pair via web UI";
    }
    oledScrollAddItem(&gOLEDEspNowState.deviceList, noDevLine1, noDevLine2, false, nullptr);
  }
}

void oledEspNowRefreshMessages() {
  if (!gEspNow) return;
  
  oledScrollClear(&gOLEDEspNowState.messageList);
  
  // Get pointer to peer message history (direct access, no copy)
  PeerMessageHistory* peerHistory = findOrCreatePeerHistory(gOLEDEspNowState.selectedDeviceMac);
  if (!peerHistory || peerHistory->count == 0) {
    // No messages, show placeholder
    static const char* noMsgLine1 = "No messages yet";
    static const char* noMsgLine2 = "Start chatting!";
    oledScrollAddItem(&gOLEDEspNowState.messageList, noMsgLine1, noMsgLine2, false, nullptr);
    return;
  }
  
  // SAFETY: Iterate ring buffer correctly from tail to head
  // This handles wraparound safely - messages are stored in ring buffer order
  int messagesToShow = min(10, (int)peerHistory->count);  // Show last 10 messages
  int startOffset = max(0, (int)peerHistory->count - messagesToShow);
  
  for (int i = startOffset; i < peerHistory->count; i++) {
    // Calculate ring buffer index (handles wraparound)
    uint8_t idx = (peerHistory->tail + i) % MESSAGES_PER_DEVICE;
    ReceivedTextMessage* msg = &peerHistory->messages[idx];
    
    // SAFETY: Skip inactive messages (may have been overwritten)
    if (!msg->active) continue;
    
    // SAFETY: Validate pointer is still within bounds
    if (!oledEspNowValidateMessagePtr(msg, gOLEDEspNowState.selectedDeviceMac)) continue;
    
    // Use direct pointers to message buffer data (no String copies)
    const char* line1 = msg->message;
    const char* line2 = msg->senderName;
    
    // Check if this is a sent or received message
    uint8_t selfMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfMac);
    bool isSent = (memcmp(msg->senderMac, selfMac, 6) == 0);
    
    if (isSent) {
      // For sent messages, use static string for status
      static const char* sentStatus = "Sent";
      line2 = sentStatus;
    } else if (!line2 || line2[0] == '\0') {
      // For received messages with no sender name
      static const char* unknownSender = "Unknown";
      line2 = unknownSender;
    }
    
    // Add item with direct pointers - no data copying
    oledScrollAddItem(&gOLEDEspNowState.messageList, line1, line2, true, (void*)msg);
  }
}

String oledEspNowFormatMac(const uint8_t* mac) {
  if (!mac) return "00:00:00:00:00:00";
  
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
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
  
  // Find the peer history for this MAC
  PeerMessageHistory* history = nullptr;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gEspNow->peerMessageHistories[i].active && 
        memcmp(gEspNow->peerMessageHistories[i].peerMac, peerMac, 6) == 0) {
      history = &gEspNow->peerMessageHistories[i];
      break;
    }
  }
  
  if (!history) return false;
  
  // Check if pointer is within the message array bounds
  const ReceivedTextMessage* msg = (const ReceivedTextMessage*)msgPtr;
  const ReceivedTextMessage* arrayStart = &history->messages[0];
  const ReceivedTextMessage* arrayEnd = &history->messages[MESSAGES_PER_DEVICE];
  
  if (msg < arrayStart || msg >= arrayEnd) return false;
  
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
  if (gOLEDEspNowState.remoteFormField == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> User: ");
  display->println(gOLEDEspNowState.remoteUsername.length() > 0 ? 
                   gOLEDEspNowState.remoteUsername.c_str() : "_____");
  
  // Field 1: Password
  if (gOLEDEspNowState.remoteFormField == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Pass: ");
  // Show asterisks for password
  if (gOLEDEspNowState.remotePassword.length() > 0) {
    for (size_t i = 0; i < gOLEDEspNowState.remotePassword.length(); i++) {
      display->print("*");
    }
    display->println();
  } else {
    display->println("_____");
  }
  
  // Field 2: Command
  if (gOLEDEspNowState.remoteFormField == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Cmd: ");
  display->println(gOLEDEspNowState.remoteCommand.length() > 0 ? 
                   gOLEDEspNowState.remoteCommand.c_str() : "_____");
}

bool oledEspNowHandleRemoteFormInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Check if keyboard is active (inline editing) - handle this first
  if (oledKeyboardIsActive()) {
    // Let keyboard handle input
    oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    // Check if keyboard completed
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      switch (gOLEDEspNowState.remoteFormField) {
        case 0:
          gOLEDEspNowState.remoteUsername = String(text);
          break;
        case 1:
          gOLEDEspNowState.remotePassword = String(text);
          break;
        case 2:
          gOLEDEspNowState.remoteCommand = String(text);
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
  if (gNavEvents.up && gOLEDEspNowState.remoteFormField > 0) {
    gOLEDEspNowState.remoteFormField--;
    return true;
  }
  if (gNavEvents.down && gOLEDEspNowState.remoteFormField < 2) {
    gOLEDEspNowState.remoteFormField++;
    return true;
  }
  
  // A button: Edit current field with keyboard
  if (newlyPressed & GAMEPAD_BUTTON_A) {
    const char* title = "";
    const char* initialText = "";
    
    switch (gOLEDEspNowState.remoteFormField) {
      case 0:
        title = "Username:";
        initialText = gOLEDEspNowState.remoteUsername.c_str();
        break;
      case 1:
        title = "Password:";
        initialText = gOLEDEspNowState.remotePassword.c_str();
        break;
      case 2:
        title = "Command:";
        initialText = gOLEDEspNowState.remoteCommand.c_str();
        break;
    }
    
    oledKeyboardInit(title, initialText, 64);
    return true;
  }
  
  // Y button: Send remote command
  if (newlyPressed & GAMEPAD_BUTTON_Y) {
    oledEspNowSendRemoteCommand();
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  // B button: Cancel form
  if (newlyPressed & GAMEPAD_BUTTON_B) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  return false;
}

void oledEspNowSendTextMessage() {
  if (!gEspNow || gOLEDEspNowState.textMessageBuffer.length() == 0) return;
  
  // Send text message to selected device
  extern void executeOLEDCommand(const String& cmd);
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOLEDEspNowState.selectedDeviceMac[0],
           gOLEDEspNowState.selectedDeviceMac[1],
           gOLEDEspNowState.selectedDeviceMac[2],
           gOLEDEspNowState.selectedDeviceMac[3],
           gOLEDEspNowState.selectedDeviceMac[4],
           gOLEDEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnow send <mac> <message>
  String cmd = "espnow send " + String(macStr) + " " + gOLEDEspNowState.textMessageBuffer;
  executeOLEDCommand(cmd);
  
  // Clear buffer
  gOLEDEspNowState.textMessageBuffer = "";
  
  // Refresh message list
  gOLEDEspNowState.needsRefresh = true;
}

void oledEspNowSendRemoteCommand() {
  if (!gEspNow) return;
  
  // Validate that all fields are filled
  if (gOLEDEspNowState.remoteUsername.length() == 0 ||
      gOLEDEspNowState.remotePassword.length() == 0 ||
      gOLEDEspNowState.remoteCommand.length() == 0) {
    return;  // Don't send if any field is empty
  }
  
  extern void executeOLEDCommand(const String& cmd);
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOLEDEspNowState.selectedDeviceMac[0],
           gOLEDEspNowState.selectedDeviceMac[1],
           gOLEDEspNowState.selectedDeviceMac[2],
           gOLEDEspNowState.selectedDeviceMac[3],
           gOLEDEspNowState.selectedDeviceMac[4],
           gOLEDEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnow remote <mac> <username> <password> <command>
  String cmd = "espnow remote " + String(macStr) + " " + 
               gOLEDEspNowState.remoteUsername + " " +
               gOLEDEspNowState.remotePassword + " " +
               gOLEDEspNowState.remoteCommand;
  
  executeOLEDCommand(cmd);
  
  // Clear form
  gOLEDEspNowState.remoteUsername = "";
  gOLEDEspNowState.remotePassword = "";
  gOLEDEspNowState.remoteCommand = "";
  
  // Refresh message list
  gOLEDEspNowState.needsRefresh = true;
}

// =============================================================================
// ESP-NOW Settings Menu
// =============================================================================

// Settings menu items: 0=Name, 1=Room, 2=Zone, 3=Friendly Name, 4=Tags, 5=Stationary, 6=Passphrase, 7=Role, 8=MasterMAC, 9=BackupMAC
#define ESPNOW_SETTINGS_COUNT 10

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
  "Backup MAC"
};

void oledEspNowOpenSettings() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
  gOLEDEspNowState.settingsMenuIndex = 0;
  gOLEDEspNowState.settingsEditField = -1;
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
  if (gOLEDEspNowState.settingsMenuIndex < settingsScrollOffset) {
    settingsScrollOffset = gOLEDEspNowState.settingsMenuIndex;
  } else if (gOLEDEspNowState.settingsMenuIndex >= settingsScrollOffset + maxVisible) {
    settingsScrollOffset = gOLEDEspNowState.settingsMenuIndex - maxVisible + 1;
  }
  
  for (int v = 0; v < maxVisible && (settingsScrollOffset + v) < ESPNOW_SETTINGS_COUNT; v++) {
    int i = settingsScrollOffset + v;
    int y = startY + v * lineHeight;
    
    // Selection indicator
    if (i == gOLEDEspNowState.settingsMenuIndex) {
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
        value = gSettings.espnowPassphrase.length() > 0 ? "****" : "(not set)";
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
    }
    
    // Truncate value if needed
    int labelLen = strlen(espnowSettingsLabels[i]) + 2;  // label + ": "
    int maxValueLen = (128 - 4 - labelLen * 6) / 6;
    if (value.length() > maxValueLen && maxValueLen > 3) {
      value = value.substring(0, maxValueLen - 1) + "~";
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
  if (gNavEvents.up && gOLEDEspNowState.settingsMenuIndex > 0) {
    gOLEDEspNowState.settingsMenuIndex--;
    return true;
  }
  if (gNavEvents.down && gOLEDEspNowState.settingsMenuIndex < ESPNOW_SETTINGS_COUNT - 1) {
    gOLEDEspNowState.settingsMenuIndex++;
    return true;
  }
  
  // A button: Edit selected item
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    gOLEDEspNowState.settingsEditField = gOLEDEspNowState.settingsMenuIndex;
    
    // Stationary: toggle boolean directly
    if (gOLEDEspNowState.settingsEditField == 5) {
      setSetting(gSettings.espnowStationary, !gSettings.espnowStationary);
      gOLEDEspNowState.settingsEditField = -1;
      return true;
    }
    
    // Role: cycle through options
    if (gOLEDEspNowState.settingsEditField == 7) {
      if (gSettings.meshRole == MESH_ROLE_WORKER) {
        setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_MASTER);
      } else if (gSettings.meshRole == MESH_ROLE_MASTER) {
        setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_BACKUP_MASTER);
      } else {
        setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_WORKER);
      }
      gOLEDEspNowState.settingsEditField = -1;
      return true;
    }
    
    // For other fields, open keyboard
    const char* prompt = espnowSettingsLabels[gOLEDEspNowState.settingsEditField];
    String initialValue = "";
    int maxLen = 32;
    
    switch (gOLEDEspNowState.settingsEditField) {
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
    gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS_KEYBOARD;
    return true;
  }
  
  // B button: Back to main menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
    return true;
  }
  
  return false;
}

void oledEspNowApplySettingsEdit(const String& value) {
  switch (gOLEDEspNowState.settingsEditField) {
    case 0: // Device Name
      setSetting(gSettings.espnowDeviceName, value);
      break;
    case 1: // Room
      setSetting(gSettings.espnowRoom, value);
      break;
    case 2: // Zone
      setSetting(gSettings.espnowZone, value);
      break;
    case 3: // Friendly Name
      setSetting(gSettings.espnowFriendlyName, value);
      break;
    case 4: // Tags
      setSetting(gSettings.espnowTags, value);
      break;
    case 6: // Passphrase
      if (value.length() > 0) {
        setSetting(gSettings.espnowPassphrase, value);
        // Re-derive encryption key if ESP-NOW is initialized
        if (gEspNow && gEspNow->initialized) {
          deriveKeyFromPassphrase(value, gEspNow->derivedKey);
        }
      }
      break;
    case 8: // Master MAC
      setSetting(gSettings.meshMasterMAC, value);
      break;
    case 9: // Backup MAC
      setSetting(gSettings.meshBackupMAC, value);
      break;
  }
  gOLEDEspNowState.settingsEditField = -1;
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
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
  gOLEDEspNowState.deviceConfigMenuIndex = 0;
  gOLEDEspNowState.deviceConfigEditField = -1;
}

void oledEspNowDisplayDeviceConfig(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header with device name
  display->setCursor(0, 0);
  display->print("Config: ");
  String name = gOLEDEspNowState.selectedDeviceName;
  if (name.length() > 14) name = name.substring(0, 13) + "~";
  display->println(name);
  
  display->drawFastHLine(0, 9, 128, DISPLAY_COLOR_WHITE);
  
  // Menu items
  int startY = 12;
  int lineHeight = 10;
  
  for (int i = 0; i < DEVICE_CONFIG_COUNT; i++) {
    int y = startY + i * lineHeight;
    if (y > 48) break;
    
    // Selection indicator
    if (i == gOLEDEspNowState.deviceConfigMenuIndex) {
      display->fillRect(0, y, 2, lineHeight - 1, DISPLAY_COLOR_WHITE);
    }
    
    display->setCursor(4, y);
    display->print(deviceConfigLabels[i]);
  }
  
  // Note: Footer is drawn by global render loop
}

bool oledEspNowHandleDeviceConfigInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Navigation
  if (gNavEvents.up && gOLEDEspNowState.deviceConfigMenuIndex > 0) {
    gOLEDEspNowState.deviceConfigMenuIndex--;
    return true;
  }
  if (gNavEvents.down && gOLEDEspNowState.deviceConfigMenuIndex < DEVICE_CONFIG_COUNT - 1) {
    gOLEDEspNowState.deviceConfigMenuIndex++;
    return true;
  }
  
  // A button: Execute selected action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    extern void executeOLEDCommand(const String& cmd);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             gOLEDEspNowState.selectedDeviceMac[0],
             gOLEDEspNowState.selectedDeviceMac[1],
             gOLEDEspNowState.selectedDeviceMac[2],
             gOLEDEspNowState.selectedDeviceMac[3],
             gOLEDEspNowState.selectedDeviceMac[4],
             gOLEDEspNowState.selectedDeviceMac[5]);
    
    switch (gOLEDEspNowState.deviceConfigMenuIndex) {
      case 0: // Restart Device
        {
          String cmd = "espnow cmd " + String(macStr) + " restart";
          executeOLEDCommand(cmd);
          broadcastOutput("[ESP-NOW] Sent restart command");
          gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        }
        break;
        
      case 1: // Set Role
        gOLEDEspNowState.deviceConfigEditField = 1;
        oledKeyboardInit("Role (master/backup/worker):", "", 16);
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 2: // Set Name
        gOLEDEspNowState.deviceConfigEditField = 2;
        oledKeyboardInit("Device Name:", gOLEDEspNowState.selectedDeviceName.c_str(), 16);
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 3: // Set Room
        gOLEDEspNowState.deviceConfigEditField = 3;
        oledKeyboardInit("Room:", "", 16);
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 4: // Set Zone
        gOLEDEspNowState.deviceConfigEditField = 4;
        oledKeyboardInit("Zone:", "", 16);
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 5: // Set Pretty Name
        gOLEDEspNowState.deviceConfigEditField = 5;
        oledKeyboardInit("Pretty Name:", "", 24);
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
        break;
        
      case 6: // Unpair Device
        oledEspNowUnpairDevice();
        break;
    }
    return true;
  }
  
  // B button: Back to device detail
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  return false;
}

void oledEspNowApplyDeviceConfigEdit(const String& value) {
  if (value.length() == 0) {
    gOLEDEspNowState.deviceConfigEditField = -1;
    return;
  }
  
  extern void executeOLEDCommand(const String& cmd);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOLEDEspNowState.selectedDeviceMac[0],
           gOLEDEspNowState.selectedDeviceMac[1],
           gOLEDEspNowState.selectedDeviceMac[2],
           gOLEDEspNowState.selectedDeviceMac[3],
           gOLEDEspNowState.selectedDeviceMac[4],
           gOLEDEspNowState.selectedDeviceMac[5]);
  
  switch (gOLEDEspNowState.deviceConfigEditField) {
    case 1: // Set Role
      {
        String cmd = "espnow cmd " + String(macStr) + " meshrole " + value;
        executeOLEDCommand(cmd);
        broadcastOutput("[ESP-NOW] Sent role change command");
      }
      break;
      
    case 2: // Set Name
      {
        String cmd = "espnow cmd " + String(macStr) + " espnowname " + value;
        executeOLEDCommand(cmd);
        gOLEDEspNowState.selectedDeviceName = value;
        broadcastOutput("[ESP-NOW] Sent name change command");
      }
      break;
      
    case 3: // Set Room
      {
        String cmd = "espnow cmd " + String(macStr) + " room " + value;
        executeOLEDCommand(cmd);
        broadcastOutput("[ESP-NOW] Sent room change command");
      }
      break;
      
    case 4: // Set Zone
      {
        String cmd = "espnow cmd " + String(macStr) + " zone " + value;
        executeOLEDCommand(cmd);
        broadcastOutput("[ESP-NOW] Sent zone change command");
      }
      break;
      
    case 5: // Set Pretty Name
      {
        String cmd = "espnow cmd " + String(macStr) + " prettyname " + value;
        executeOLEDCommand(cmd);
        broadcastOutput("[ESP-NOW] Sent pretty name change command");
      }
      break;
  }
  
  gOLEDEspNowState.deviceConfigEditField = -1;
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
static RemoteFileBrowseState gRemoteFileBrowse = {0};

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
