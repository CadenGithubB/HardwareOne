#include "OLED_ESPNow.h"
#include "System_DeviceName.h"  // devicename::kMaxLen (the device name's 31 bytes), kDisplayCap
#include "System_SelfDevice.h"  // SelfDevice::deviceNameDisplay
#include "System_CommandReply.h"  // the reply words and how to read them

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_TextField.h"
#include "OLED_UI.h"                  // oledToastShow — owner refusals on unpair/discovery
#include "System_ESPNow.h"
#include "System_ESPNow_HistoryNames.h"  // per-refresh name cache for conversation rows
#include "System_ESPNow_Sessions.h"   // sendStatusGet / SendStatusState — post-send delivery indicator
#include "System_ESPNow_RemoteCmdCore.h"  // a remote command result that came late (Outcome::Late)
#include "System_MeshPeers.h"
#include "System_Utils.h"

#include "i2csensor_seesaw.h"

// A peer's state on these screens (online / connecting / offline) is
// espnowPeerState / espnowPeerCounts (System_ESPNow.h, the rule in
// System_ESPNow_PeerStateCore.h); gMeshPeerMeta and getMeshPeerMeta are
// declared there too. Names come from MeshPeers::displayName (System_MeshPeers.h).

// =============================================================================
// OLED ESP-NOW Interface Implementation
// =============================================================================

// Main menu items (Bluetooth-style)
static const char* sEspNowMenuItems[] = {
  "Status",      // 0 - Network status overview
  "Devices",     // 1 - Device list with filter/sort
  "Rooms",       // 2 - Room-based device grouping
  "Settings",    // 3 - Local device settings
  "Start/Stop",  // 4 - Toggle ESP-NOW on/off
  "Pairing"      // 5 - Enter pairing mode
};
static const int kEspNowMenuItemCount = 6;

// Global state
OledEspNowState gOledEspNowState;

// Match the rest of the OLED subsystem: the large pointer/metadata-only scroll
// states live in external BSS, while gOledEspNowState keeps its non-trivial
// String fields (including the remote-login form) in internal DRAM.
EXT_RAM_BSS_ATTR static OledScrollState sEspNowDeviceListScroll;
EXT_RAM_BSS_ATTR static OledScrollState sEspNowMessageListScroll;

// Sent text is now recorded in the SHARED per-peer history (sent ring) at the
// cmd_espnow_send chokepoint, so the OLED no longer keeps a private sent ring —
// it reads the unified conversation via espnowGetConversation(). This is what
// lets the OLED see messages sent from the web/BLE too, not just ones typed here.

// Per-row direction/status metadata, kept in lockstep with messageList.items by
// oledEspNowRefreshMessages and read by the device-detail renderer (left=received,
// right=sent + live delivery status).
// PSRAM .bss: CPU-only row metadata, written/read solely on the OLED task.
EXT_RAM_BSS_ATTR static struct { bool isSent; uint32_t msgId; uint8_t sendState; uint8_t sendStatus; } gOledRowMeta[OLED_SCROLL_MAX_ITEMS];

// Owner commands preserve their refusal through the shared reply mapping.
static bool oledEspNowRunOwnerCommand(const char* cmd, const char* failToast) {
  char out[oled_message::kMessageBytes] = {};
  if (executeOledCommandWithResult(cmd, out, sizeof(out))) return true;
  char text[oled_message::kMessageBytes];
  oledCommandReplyText(false, out, nullptr, failToast, text, sizeof(text));
  oledToastShow(text, oled_message::kErrorDurationMs, OledUiIcon::ERROR);
  return false;
}

// =============================================================================
// Pairing view — WPS-style toggle (see espnowpairmode / espnowPairMode* core)
// =============================================================================
// Open the timed pairing window on BOTH same-mesh devices; they broadcast a
// discovery beacon and auto secure-pair each other. This screen is just the
// on/off toggle + countdown + a live list of what has paired in. All discovery
// and pairing logic lives in System_ESPNow.cpp; the OLED only drives the window,
// through the owner's `espnowpairmode 120|off` command.

// --- Conversation (device-detail) message-list geometry ---
// The conversation view relies on the GLOBAL header bar (title "ESPNOW: Text"),
// so message rows start at OLED_CONTENT_START_Y like the sibling ESP-NOW views
// and stack down to the GLOBAL footer band. The footer top is
// OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT (the exact value drawOledFooter() uses),
// so deriving the visible-row count and the render/scrollbar bounds from it here
// keeps the shared scroll window (oledScrollUp/Down + the follow-tail restore) in
// lockstep with what actually fits. Hardcoding it (the old visibleLines=3 / yPos<56)
// let a scrolled-to row spill into and below the footer, which also made the list
// look stuck because the cursor lived in a slot that wasn't physically on screen.
#define kMsgListTopY (OLED_CONTENT_START_Y)  // first row Y, below the global header
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

// Scrub the remote-command form. `= ""` on Arduino String only zeroes the
// length — the typed bytes stay in the SSO buffer (and remotePassword is a
// real account password for a peer device). secureClearString overwrites the
// buffer through a volatile pointer before releasing it, so residue cannot
// survive in RAM to be exposed by a later memory-disclosure bug. Used on
// every path that retires the form: send, cancel, open, and (re)init.
static void oledEspNowScrubRemoteForm() {
  secureClearString(gOledEspNowState.remoteUsername);
  secureClearString(gOledEspNowState.remotePassword);
  secureClearString(gOledEspNowState.remoteCommand);
}

// Open the remote-command form with its command line filled in (the Device
// Config rows). The form collects an account of the TARGET device and sends
// nothing until Y.
static void oledEspNowOpenRemoteFormWith(const String& command) {
  oledEspNowScrubRemoteForm();
  gOledEspNowState.remoteCommand = command;
  gOledEspNowState.remoteFormField = 0;
  gOledEspNowState.currentView = ESPNOW_VIEW_REMOTE_FORM;
}

// `espnowremote` acknowledges a command it handed to the radio with the request
// id the peer's answer will carry: the reply ends "(match reqId <n>)", or is
// {"schema":1,"ok":true,"reqId":<n>} when the typed command holds a `json`
// word. Its refusals lead with "Error", or with "Usage:" for too few
// arguments (a JSON one says "ok":false); the id is the proof that a command
// left this device.
static bool oledEspNowRemoteAcknowledged(const char* reply) {
  if (!reply) return false;
  if (reply[0] == '{') return commandReplyJsonOk(reply) && strstr(reply, "\"reqId\":") != nullptr;
  static const char kTag[] = "(match reqId ";
  const char* tag = nullptr;   // the last one: the echoed command sits before it
  for (const char* p = strstr(reply, kTag); p; p = strstr(p + 1, kTag)) tag = p;
  if (!tag) return false;
  const char* digits = tag + sizeof(kTag) - 1;
  const char* end = digits;
  while (*end >= '0' && *end <= '9') end++;
  return end != digits && end[0] == ')' && end[1] == '\0';
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
  oledEspNowScrubRemoteForm();
  
  // Initialize scrolling lists
  // Two-line (16 px) items under the 10 px title, as many as end above the
  // footer: 2 on the 64 px OLED (a fixed 3 put the third under the footer).
  oledScrollInit(&sEspNowDeviceListScroll, "ESP-NOW Devices", oledVisibleRows(16, 10));
  oledScrollInit(&sEspNowMessageListScroll, nullptr, 3);
  // Truthful visible-row count (footer-aware) so the shared scroll helpers and the
  // follow-tail restore never push the selection into/below the footer.
  sEspNowMessageListScroll.visibleLines = oledEspNowMsgVisibleRows();
  
  // Settings menu state (local)
  gOledEspNowState.settingsMenuIndex = 0;
  gOledEspNowState.settingsEditField = -1;
  
  // Device config menu state (remote)
  gOledEspNowState.deviceConfigMenuIndex = 0;
  gOledEspNowState.deviceConfigEditField = -1;
  gOledEspNowState.deviceConfigScrollOffset = 0;
  
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
  oledKeyboardInit("Device Name:", initialText, devicename::kMaxLen,
                   OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
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
    // Paired peers online (heartbeat and session); those still connecting
    // follow in brackets (System_ESPNow_PeerStateCore.h).
    const EspNowPeerCounts peers = espnowPeerCounts();
    char line[22];
    if (peers.connecting > 0) {
      snprintf(line, sizeof(line), "Online: %d (+%d conn)", peers.online, peers.connecting);
    } else {
      snprintf(line, sizeof(line), "Online: %d", peers.online);
    }
    display->println(line);
  } else {
    display->println("Status: OFF");
  }
  
  // Calculate visible menu area (44px content - 10px status line = 34px for menu)
  const int kStatusHeight = 10;
  const int kLineHeight = 8;
  const int kMaxVisibleItems = (OLED_CONTENT_HEIGHT - kStatusHeight) / kLineHeight;  // 4 lines
  const int kTotalItems = kEspNowMenuItemCount;
  
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
    const int y = menuStartY + i * kLineHeight;
    const bool selected = itemIndex == gOledEspNowState.mainMenuSelection;
    display->setCursor(0, y);
    display->print(selected ? "> " : "  ");
    // The label field ends before the fixed marker column (OLED_TextField.h).
    oledTextFieldDraw(display, 12, y, 114 - 12 - 6, sEspNowMenuItems[itemIndex],
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, selected, sEspNowMenuItems);
    if (itemIndex == 4 && gEspNow && gEspNow->initialized) {  // Start/Stop: running marker
      display->setCursor(114, y);
      display->print("*");
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
  
  // Paired peers online / paired, and how many are still connecting. All
  // three come from one walk of the paired registry (espnowPeerCounts), so
  // the numerator never exceeds the denominator.
  {
    const EspNowPeerCounts peers = espnowPeerCounts();
    char line[22];
    if (peers.connecting > 0) {
      snprintf(line, sizeof(line), "Online: %d/%d +%d conn", peers.online, peers.paired(),
               peers.connecting);
    } else {
      snprintf(line, sizeof(line), "Online: %d/%d", peers.online, peers.paired());
    }
    display->println(line);
  }
  
  // Encryption status
  display->print("Encrypt: ");
  display->println(gEspNow && gEspNow->encryptionEnabled ? "Yes" : "No");
  
  // Channel
  display->print("Channel: ");
  display->println(gEspNow ? gEspNow->channel : 0);
  
  // Device name on the rest of the row, bounded (OLED_TextField.h).
  display->print("Name: ");
  {
    // The name in use, "HardwareOne-B2C3 (default)" when none is set.
    char name[devicename::kDisplayCap];
    (void)SelfDevice::deviceNameDisplay(name, sizeof(name));
    const int nameY = display->getCursorY();
    oledTextFieldDraw(display, 36, nameY, SCREEN_WIDTH - 36, name, DISPLAY_COLOR_WHITE,
                      DISPLAY_COLOR_WHITE, false, &gOledEspNowState);
    display->println();
  }
  
  // Note: Footer is drawn by global render loop
}

// A peer row's state glyph: '+' online, '~' connecting (heartbeat, no
// session yet), '-' offline (System_ESPNow_PeerStateCore.h).
static const char* oledEspNowPeerGlyph(EspNowPeerState state) {
  return state == EspNowPeerState::Online ? "+" : state == EspNowPeerState::Connecting ? "~" : "-";
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
  char name[32];  // a device name (31 bytes) + NUL
  uint8_t mac[6];
  EspNowPeerState state;
  bool pending;  // says it is in this room; not accepted here, so room commands skip it
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
      sRoomDevices[sRoomDeviceCount].state = EspNowPeerState::Online;  // this device
      sRoomDevices[sRoomDeviceCount].pending = false;  // its own room needs no acceptance
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
    sRoomDevices[sRoomDeviceCount].state = espnowPeerState(gMeshPeerMeta[i].mac);
    // The rooms shown are the ones peers announce. A peer whose room was not
    // accepted on this device gets a '?' in its row; one without a room has
    // nothing to accept.
    sRoomDevices[sRoomDeviceCount].pending =
        gMeshPeerMeta[i].room[0] != '\0' && !espnowGroupRoomAccepted(gMeshPeerMeta[i].mac, gMeshPeerMeta[i].room);
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
      
      // Room name in its own field; the device count keeps a fixed right column.
      char count[12];
      snprintf(count, sizeof(count), "(%d)", sRoomList[idx].deviceCount);
      const int countW = (int)strlen(count) * 6;
      const int countX = 118 - countW;
      const bool selected = idx == gOledEspNowState.roomsMenuSelection;
      const uint16_t ink = selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
      oledTextFieldDraw(display, 2, y + 1, countX - 6 - 2, sRoomList[idx].name, ink, DISPLAY_COLOR_WHITE,
                        selected, sRoomList);
      oledTextFieldDraw(display, countX, y + 1, countW, count, ink, DISPLAY_COLOR_WHITE, false, sRoomList);
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
      display->print(oledEspNowPeerGlyph(sRoomDevices[idx].state));
      if (sRoomDevices[idx].pending) display->print("?");
      // Device name in a bounded field after the fixed status glyph.
      {
        const bool selected = idx == gOledEspNowState.roomsDeviceSelection;
        oledTextFieldDraw(display, 14, y + 1, 118 - 14, sRoomDevices[idx].name,
                          selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                          selected, sRoomDevices);
      }
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
// from oledUpdate() so it pops on any screen but Login, and only for a display
// identity that may answer it: espnowaccept/espnowreject are admin-only, so the
// prompt needs a logged-in admin (the OLED session role cache, no roster read
// per frame). A request is its sender MAC plus arrival time (the radio's
// retransmits of a pending request keep it; a request made again after this one
// ended is a new one). The answer names the shown sender, and the request is kept as
// answered whatever the owner replies, so a refusal is toasted once instead of
// the dialog re-opening every frame. A session boundary drops the dialog
// without an answer, so the next admin session still sees the request. The
// dialog holds line2 by pointer, so the name buffer is static (stable while
// it's up). Main-loop only.
struct PairPromptKey { bool valid; uint8_t mac[6]; uint32_t atMs; };
EXT_RAM_BSS_ATTR static PairPromptKey sPairPromptShown;     // the request in the dialog
EXT_RAM_BSS_ATTR static PairPromptKey sPairPromptAnswered;  // the last request answered here
static char sPairPromptName[26];

static bool pairPromptIs(const PairPromptKey& key, const uint8_t mac[6], uint32_t atMs) {
  return key.valid && key.atMs == atMs && memcmp(key.mac, mac, 6) == 0;
}

static void oledEspNowAnswerPairRequest(const char* verb, const char* failToast) {
  sPairPromptAnswered = sPairPromptShown;
  const uint8_t* m = sPairPromptShown.mac;
  char cmd[40];   // "espnowaccept " (13) + 17-char MAC + NUL
  snprintf(cmd, sizeof(cmd), "%s " MAC_FMT,
           verb, MAC_ARGS(m));
  oledEspNowRunOwnerCommand(cmd, failToast);
}
static void onPairReqAccept(void* /*ud*/) { oledEspNowAnswerPairRequest("espnowaccept", "Accept failed"); }
static void onPairReqReject(void* /*ud*/) { oledEspNowAnswerPairRequest("espnowreject", "Reject failed"); }

void oledEspNowPollPairRequest() {
  if (oledConfirmIsActive()) return;                       // a dialog is already up
  char reqName[32];
  uint8_t mac[6];
  uint32_t atMs = 0;
  if (!espnowGetIncomingPairRequest(reqName, sizeof(reqName), mac, &atMs)) return;
  if (pairPromptIs(sPairPromptAnswered, mac, atMs)) return;            // answered here
  if (currentOledMode == OLED_LOGIN || !oledSessionIsAdmin()) return;  // cannot answer it
  sPairPromptShown.valid = true;
  memcpy(sPairPromptShown.mac, mac, 6);
  sPairPromptShown.atMs = atMs;
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

    // The header line shows our own request while it runs and its result for a
    // while after: a timed-out request is retried by selecting the peer again
    // (a peer that already paired us answers the repeat with Accept).
    char outName[32];
    char head[22];
    switch (espnowGetPairRequestOutcome(outName, sizeof(outName))) {
      case ESPNOW_PAIR_OUT_WAITING:   snprintf(head, sizeof(head), "Waiting: %.11s", outName); break;
      case ESPNOW_PAIR_OUT_PAIRED:    snprintf(head, sizeof(head), "Paired: %.11s", outName); break;
      case ESPNOW_PAIR_OUT_DECLINED:  snprintf(head, sizeof(head), "Declined: %.11s", outName); break;
      case ESPNOW_PAIR_OUT_TIMED_OUT: snprintf(head, sizeof(head), "No answer: retry"); break;
      default:                        snprintf(head, sizeof(head), "Select to pair:"); break;
    }
    display->setCursor(0, y); display->println(head); y += 11;
    // Candidate rows below the two status lines, as many as fit above the
    // footer (2 on the 64 px OLED, 8 on the P4X-EYE canvas); the window ends
    // on the selected one.
    const int maxRows = oledVisibleRows(10, y - OLED_CONTENT_START_Y);
    int start = (sPairingSel >= maxRows) ? sPairingSel - maxRows + 1 : 0;
    for (int i = start; i < n && i < start + maxRows; i++) {
      char nm[32]; uint8_t mac[6]; int rssi = 0;
      if (!espnowGetPairCandidate(i, nm, sizeof(nm), mac, &rssi)) continue;
      display->setCursor(0, y);
      display->print(i == sPairingSel ? "> " : "  ");
      oledTextFieldDraw(display, 12, y, SCREEN_WIDTH - 12, nm, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                        i == sPairingSel, &sPairingSel);
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
  return kEspNowMenuItemCount;
}

void oledEspNowMainMenuUp() {
  if (gOledEspNowState.mainMenuSelection > 0) {
    gOledEspNowState.mainMenuSelection--;
  }
}

void oledEspNowMainMenuDown() {
  if (gOledEspNowState.mainMenuSelection < kEspNowMenuItemCount - 1) {
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
          executeOledCommand("closeespnow");
        } else {
          executeOledCommand("openespnow");
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
  sEspNowDeviceListScroll.title = titleBuf;
  
  // Render device list using scrolling system
  oledScrollRender(display, &sEspNowDeviceListScroll, true, true);
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
      oledTextFieldDraw(display, 18, y + 1, SCREEN_WIDTH - 18, opts[i],
                        sel ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE,
                        sel ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE, sel, opts);
      y += 14;
    }
    display->setTextColor(DISPLAY_COLOR_WHITE);
    return;
  }
  
  // Render message list below the global header. The visible window, the per-row
  // clip and the scrollbar all derive from the SAME footer geometry as
  // sEspNowMessageListScroll.visibleLines (see kMsgList* above), so a
  // scrolled-to row can never land in or below the global footer.
  const int yOffset    = kMsgListTopY;
  const int footerTopY = oledEspNowMsgFooterTopY();
  const int lineHeight = 8;
  int visibleStart = sEspNowMessageListScroll.scrollOffset;
  int visibleEnd = min(sEspNowMessageListScroll.itemCount,
                       visibleStart + sEspNowMessageListScroll.visibleLines);

  int yPos = yOffset;

  const int rightEdge = SCREEN_WIDTH - 4;  // leave room for the scrollbar
  for (int i = visibleStart; i < visibleEnd && (yPos + kMsgRowHeight) <= footerTopY; i++) {
    OledScrollItem* item = &sEspNowMessageListScroll.items[i];
    bool isSelected = (i == sEspNowMessageListScroll.selectedIndex);
    bool isSent = gOledRowMeta[i].isSent;  // left = received, right = sent (web-style)

    // Line 1: the message's first line in a bounded field. A sent message is
    // right-aligned while it fits; a longer one fills the row from the left
    // and scrolls when selected (OLED_TextField.h).
    const char* msg = item->line1 ? item->line1 : "";
    const int fieldX = (isSelected || isSent) ? 4 : 0;
    const int textW = (int)strlen(msg) * 6;  // 6 px/glyph at text size 1
    const int x = (isSent && textW <= rightEdge - fieldX) ? rightEdge - textW : fieldX;

    // Selection bar on the message's own side.
    if (isSelected) {
      int barX = isSent ? (SCREEN_WIDTH - 2) : 0;
      display->fillRect(barX, yPos, 2, lineHeight * 2, DISPLAY_COLOR_WHITE);
    }
    oledTextFieldDraw(display, x, yPos, rightEdge - x, msg, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                      isSelected, &sEspNowMessageListScroll);
    yPos += lineHeight;

    // Line 2: delivery status (sent, right) or sender (received, left).
    if (isSent) {
      // Durable delivery state from the message record (stamped on ACK by
      // espnowUpdateSentDeliveryState) — NOT the ephemeral sendStatus ring, which
      // is swept after 30s and used to make delivered rows revert to "Sent".
      const char* label; bool icon = true, dbl = false;
      // One word per meaning (ESP-NOW 1.0): the same words as the web page.
      switch (gOledRowMeta[i].sendState) {
        case SEND_STATUS_DELIVERED: label = "Delivered"; dbl = true; break;
        case SEND_STATUS_TIMEOUT:                                        // no ACK in 10 s
        case SEND_STATUS_FAILED:    label = "No answer"; icon = false; break;  // session never opened
        case SEND_STATUS_BUSY:      label = "Busy"; icon = false; break;
        case SEND_STATUS_REFUSED:   label = "Refused"; icon = false; break;
        default:                    label = "Sent"; break;  // PENDING: awaiting ACK
      }
      // "Refused: <reason>" when it fits the row at this rotation; else the word.
      char refusedLabel[32];
      if (gOledRowMeta[i].sendState == SEND_STATUS_REFUSED) {
        const char* why = espnowanswer::sharedStatusName(gOledRowMeta[i].sendStatus);
        if (why) {
          snprintf(refusedLabel, sizeof(refusedLabel), "Refused: %s", why);
          for (char* c = refusedLabel; *c; ++c) if (*c == '_') *c = ' ';
          if ((int)strlen(refusedLabel) * 6 <= rightEdge - 4) label = refusedLabel;
        }
      }
      int iconW = icon ? (dbl ? 8 : 6) : 0;
      int lblW = (int)strlen(label) * 6 + iconW;
      int lx = max(4, rightEdge - lblW);
      if (icon) { oledEspNowDrawStatusIcon(display, lx, yPos, dbl); lx += iconW; }
      display->setCursor(lx, yPos);
      display->print(label);
    } else {
      const int senderX = isSelected ? 4 : 0;
      oledTextFieldDraw(display, senderX, yPos, rightEdge - senderX, item->line2,
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false, &sEspNowMessageListScroll);
    }
    yPos += lineHeight;
  }
  
  // Show scrollbar if needed
  if (sEspNowMessageListScroll.itemCount > sEspNowMessageListScroll.visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    int scrollbarY = yOffset;
    int scrollbarHeight = footerTopY - yOffset;  // content band, above the global footer
    
    display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
    
    int thumbHeight = max(4, (scrollbarHeight * sEspNowMessageListScroll.visibleLines) /
                            sEspNowMessageListScroll.itemCount);
    int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * 
                 sEspNowMessageListScroll.scrollOffset /
                 max(1, sEspNowMessageListScroll.itemCount - sEspNowMessageListScroll.visibleLines);
    
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
          uint8_t mac[6]; char nm[32]; int rssi = 0;
          if (n > 0 && espnowGetPairCandidate(sPairingSel, nm, sizeof(nm), mac, &rssi)) {
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "espnowpairrequest " MAC_FMT,
                     MAC_ARGS(mac));
            executeOledCommandShowResult(cmd);   // the other device must Accept: toast "sent"
          }
          oledMarkDirty();
          return true;
        }
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          // Leaving the screen stops discovery through the owner (admin-gated);
          // a refused close leaves the window to the owner's timer.
          oledEspNowRunOwnerCommand("espnowpairmode off", "Discovery failed");
          gOledEspNowState.currentView = ESPNOW_VIEW_MAIN_MENU;
          return true;
        }
        return false;
      }
      // 3. Window closed → A starts discovery, B back to menu.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        if (!(gEspNow && !gEspNow->encryptionEnabled)) {   // need a mesh key first (screen says so)
          sPairingSel = 0;
          oledEspNowRunOwnerCommand("espnowpairmode 120", "Discovery failed");   // owner-gated open
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
            gOledEspNowState.textMessageBuffer = "";  // an unsent message was for the previous device
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
        oledScrollUp(&sEspNowDeviceListScroll);
        return true;
      }
      if (gNavEvents.down) {
        oledScrollDown(&sEspNowDeviceListScroll);
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
              const size_t length = strnlen(gMeshPeerMeta[i].room, sizeof(gOledEspNowState.filterValue) - 1);
              memcpy(gOledEspNowState.filterValue, gMeshPeerMeta[i].room, length);
              gOledEspNowState.filterValue[length] = '\0';
              break;
            } else if (gOledEspNowState.filterMode == 2 && gMeshPeerMeta[i].zone[0]) {
              const size_t length = strnlen(gMeshPeerMeta[i].zone, sizeof(gOledEspNowState.filterValue) - 1);
              memcpy(gOledEspNowState.filterValue, gMeshPeerMeta[i].zone, length);
              gOledEspNowState.filterValue[length] = '\0';
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
        oledKeyboardReset();
        gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
        // A value moves on to the remote form with the command filled in.
        oledEspNowApplyDeviceConfigEdit(value);
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
            oledFileBrowserStartEspNowReceive(gOledEspNowState.selectedDeviceMac);
            requestOledMode(OLED_FILE_BROWSER, "espnow.receive");  // B returns here
          } else {
            oledFileBrowserStartEspNowSend(gOledEspNowState.selectedDeviceMac);
            requestOledMode(OLED_FILE_BROWSER, "espnow.send");
          }
          return true;
        }
      } else if (gOledEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
        // A button: Open text keyboard
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOledEspNowState.currentView = ESPNOW_VIEW_TEXT_KEYBOARD;
          // A message whose send failed is offered again; a sent one was cleared.
          oledKeyboardInit("Send Message:", gOledEspNowState.textMessageBuffer.c_str(), 128,
                           OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&sEspNowMessageListScroll);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&sEspNowMessageListScroll);
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
          oledEspNowScrubRemoteForm();
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&sEspNowMessageListScroll);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&sEspNowMessageListScroll);
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
  OledScrollItem* selected = oledScrollGetSelected(&sEspNowDeviceListScroll);
  if (!selected || !selected->userData) return;
  
  // Store selected device MAC
  EspNowDevice* device = (EspNowDevice*)selected->userData;
  memcpy(gOledEspNowState.selectedDeviceMac, device->mac, 6);
  gOledEspNowState.selectedDeviceName = String(device->name);
  gOledEspNowState.textMessageBuffer = "";  // an unsent message was for the previous device
  
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

// Thin adapter over the unpair owner. `espnowunpair` (cmd_espnow_unpair) is the
// one place that deletes the radio peer, removes the registry entry, drops the
// crypto identity and live session, clears the mesh-liveness slot, posts
// SYSEVT_PEER_UNPAIRED and saves the registry, behind the admin gate. The OLED
// submits it under the display identity (as the G2 peers page does) and never
// edits devices[] itself.
void oledEspNowUnpairDevice() {
  char cmd[32];   // "espnowunpair " (13) + 17-char MAC + NUL
  snprintf(cmd, sizeof(cmd), "espnowunpair " MAC_FMT,
           MAC_ARGS(gOledEspNowState.selectedDeviceMac));
  if (!oledEspNowRunOwnerCommand(cmd, "Unpair failed")) return;   // refused: stay on the config view
  oledEspNowBackToList();   // peer is gone: the detail view has no target
}

// Helper struct for sorting devices
struct DeviceEntry {
  EspNowDevice* device;
  MeshPeerMeta* meta;
  MeshPeerHealth* health;
  EspNowPeerState state;
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

// Comparison function for sorting by status (online, connecting, offline)
static int compareByStatus(const void* a, const void* b) {
  const DeviceEntry* da = (const DeviceEntry*)a;
  const DeviceEntry* db = (const DeviceEntry*)b;
  
  // Online first, then connecting, then offline (the enum's reverse order)
  if (da->state != db->state) {
    return (int)db->state - (int)da->state;
  }
  
  // Same status, sort by name
  return strcasecmp(da->displayName, db->displayName);
}

void oledEspNowRefreshDeviceList() {
  if (!gEspNow) return;

  // Keep the cursor across this rebuild — this runs on a 1s timer while the
  // device list is open, so a plain oledScrollClear() (which resets selectedIndex
  // to 0) would snap the selection back to the top every second.
  oledScrollClearKeepSelection(&sEspNowDeviceListScroll);
  
  // Own MAC to skip self: ESP-NOW's cached address while it runs. This list
  // rebuilds every second, and the driver query is a round trip to the radio
  // co-processor on a Hosted board; espnowSelfStaMac only falls back to it
  // while ESP-NOW is stopped. One read for the whole loop (isSelfMac would
  // repeat the fallback per row). Zeroed so a failed read matches no device.
  uint8_t myMac[6] = {};
  (void)espnowSelfStaMac(myMac);
  
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
    const EspNowPeerState state = espnowPeerState(device->mac);
    
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
    entries[entryCount].state = state;
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
    
    // Line 1: state glyph + display name
    snprintf(line1Bufs[i], sizeof(line1Bufs[0]), "%s %s",
             oledEspNowPeerGlyph(entry->state), entry->displayName);
    
    // Line 2: room + "E" while the peer's encrypted session is up (1.0 step 9:
    // every paired device is encrypted; the registry keeps no flag), or MAC if no room
    if (entry->meta && entry->meta->room[0]) {
      // '?' after the room: announced by the peer, not accepted on this device.
      snprintf(line2Bufs[i], sizeof(line2Bufs[0]), " %s%s%s",
               entry->meta->room,
               espnowGroupRoomAccepted(entry->device->mac, entry->meta->room) ? "" : "?",
               espnowSessionLive(entry->device->mac) ? " E" : "");
    } else {
      snprintf(line2Bufs[i], sizeof(line2Bufs[0]), " %02X%02X%02X%s",
               entry->device->mac[3], entry->device->mac[4], entry->device->mac[5],
               espnowSessionLive(entry->device->mac) ? " E" : "");
    }
    
    oledScrollAddItem(&sEspNowDeviceListScroll, line1Bufs[i], line2Bufs[i], true, entry->device);
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
    oledScrollAddItem(&sEspNowDeviceListScroll, noDevLine1, noDevLine2, false, nullptr);
  }

  // Clamp the preserved cursor back into range (device count may have shrunk).
  oledScrollClampSelection(&sEspNowDeviceListScroll);
}

void oledEspNowRefreshMessages() {
  if (!gEspNow) return;

  // Snapshot scroll position BEFORE clear so we can restore it after rebuild.
  // This function is called on a 1-second timer from the render loop — without
  // preserving position the user would get snapped to the top of the message
  // list every tick, making it impossible to scroll up to read older messages
  // while a peer is actively chatting.
  int savedSelectedIndex = sEspNowMessageListScroll.selectedIndex;
  int savedScrollOffset  = sEspNowMessageListScroll.scrollOffset;
  int prevItemCount      = sEspNowMessageListScroll.itemCount;

  oledScrollClear(&sEspNowMessageListScroll);

  // Build the conversation from the SHARED store: the core merges this peer's
  // received + sent rings into one time-ordered list (espnowGetConversation), so
  // the OLED shows the same outgoing history as every other interface — including
  // messages sent from the web or BLE, not just ones typed here. Sent rows carry
  // msgId (the head record's reqId) for the live delivery-status indicator;
  // received rows show the sender name + part count. Each row's head record is
  // copied out of the store (espnowCopyCollapsedHead), which resolves the ref by
  // its seq number (never by a raw ring offset); a ref whose record the peer's
  // own newer messages evicted since the collapse is refused and skipped.
  uint8_t* mac = gOledEspNowState.selectedDeviceMac;

  // The conversation keeps the NEWEST ESPNOW_HISTORY_VIEW_MAX logical messages
  // (older ones are dropped by the collapse); only the last CAP are shown below.
  static EXT_RAM_BSS_ATTR CollapsedMsgRef refs[ESPNOW_HISTORY_VIEW_MAX];
  int rc = espnowGetConversation(mac, refs, ESPNOW_HISTORY_VIEW_MAX);

  // The scroll list keeps pointers, so row strings live in these persistent
  // buffers until the next refresh. Line 1 holds 23 chars: the renderer
  // (oledEspNowDisplayDeviceDetail) prints at most 19 plus '~'; widen this if a
  // renderer ever prints more. Sender names keep the whole display-name size.
  static EXT_RAM_BSS_ATTR char metaBuf[OLED_SCROLL_MAX_ITEMS][24];  // persistent "name (k/n)" labels
  static EXT_RAM_BSS_ATTR char textBuf[OLED_SCROLL_MAX_ITEMS][24];  // message text, line 1
  static EXT_RAM_BSS_ATTR char nameBuf[OLED_SCROLL_MAX_ITEMS][ESPNOW_HISTORY_NAME_CAP];
  // Records carry no name: received rows are named by espnowHistoryDisplayName
  // (the peer's current name, or the MAC text for an unattributed record),
  // resolved once per kind per refresh: one peer, so two entries (attributed,
  // unattributed). Constant-initialised (all zero), OLED task only.
  static EXT_RAM_BSS_ATTR espnowhist::BasicNameCache<2> nameCache;
  nameCache.clear();
  const int CAP = OLED_SCROLL_MAX_ITEMS;
  ReceivedTextMessage head;

  // Show the most recent CAP messages (conversation is oldest→newest).
  int start = (rc > CAP) ? (rc - CAP) : 0;
  int m = 0;
  for (int i = start; i < rc && m < CAP; i++) {
    if (!espnowCopyCollapsedHead(refs[i], &head)) continue;  // evicted since the collapse

    gOledRowMeta[m].isSent    = refs[i].isSent;
    gOledRowMeta[m].msgId     = refs[i].isSent ? head.reqId : 0;
    gOledRowMeta[m].sendState = head.sendState;  // durable; survives the 30s sendStatus sweep
    gOledRowMeta[m].sendStatus = head.sendStatus;  // a refusal's reason (ACK status), durable too

    const char* meta;
    if (refs[i].isSent) {
      meta = nullptr;  // sent rows: delivery status drawn live by the renderer
    } else {
      // Called after espnowCopyCollapsedHead returned: no store lock is held.
      const char* who = nameCache.get(head.peerMac, head.isSent, head.unattributed,
          [&head](char* out, size_t cap) { espnowHistoryDisplayName(head, out, cap); });
      if (head.msgType == MSG_CMD_RESULT && head.sendState == (uint8_t)espnowcmd::Outcome::Late) {
        // A remote command result that came after "No result" was shown
        // (ESP-NOW 1.0): the word replaces the sender's name on line 2.
        strlcpy(metaBuf[m], "Result (late)", sizeof(metaBuf[m]));
        meta = metaBuf[m];
      } else if (refs[i].partsTotal > 1) {
        snprintf(metaBuf[m], sizeof(metaBuf[m]), "%s (%u/%u)", who[0] ? who : "Unknown",
                 refs[i].partsPresent, refs[i].partsTotal);
        meta = metaBuf[m];
      } else if (who[0]) {
        strlcpy(nameBuf[m], who, sizeof(nameBuf[m]));
        meta = nameBuf[m];
      } else {
        meta = "Unknown";  // unreachable: the resolver never gives "" for a received record
      }
    }

    strlcpy(textBuf[m], head.message, sizeof(textBuf[m]));
    oledScrollAddItem(&sEspNowMessageListScroll, textBuf[m], meta, true, nullptr);
    m++;
  }

  if (m == 0) {
    static const char* noMsgLine1 = "No messages yet";
    static const char* noMsgLine2 = "Start chatting!";
    gOledRowMeta[0].isSent = false; gOledRowMeta[0].msgId = 0;
    oledScrollAddItem(&sEspNowMessageListScroll, noMsgLine1, noMsgLine2, false, nullptr);
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
  int newCount = sEspNowMessageListScroll.itemCount;
  if (newCount > 0) {
    int vis = sEspNowMessageListScroll.visibleLines;
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
    sEspNowMessageListScroll.selectedIndex = idx;
    sEspNowMessageListScroll.scrollOffset  = off;
  }
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
    
    // A remote username is plain identity metadata. Passwords and arbitrary
    // command payloads can contain secrets and must never accept dictation.
    const OledKeyboardDictationPolicy policy =
        gOledEspNowState.remoteFormField == 0
            ? OledKeyboardDictationPolicy::ALLOW_PLAINTEXT
            : OledKeyboardDictationPolicy::DENY;
    // The command field holds what the keyboard does: a Device Config row
    // prefills up to "espnowfriendlyname " plus 47 characters (66), and at 64
    // opening the field cut the end of that name off without a word. A line
    // too long for the frame is refused by `espnowremote` with its reason.
    const int maxLength =
        gOledEspNowState.remoteFormField == 2 ? OLED_KEYBOARD_MAX_LENGTH : 64;
    oledKeyboardInit(title, initialText, maxLength, policy,
                     gOledEspNowState.remoteFormField == 1);  // password
    return true;
  }
  
  // Y button: Send remote command. An incomplete form stays open as typed.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    if (oledEspNowSendRemoteCommand()) {
      gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    }
    return true;
  }
  
  // B button: Cancel form. Scrub, don't just leave: before this fix a
  // cancelled form kept the typed password in the struct until the next open.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    oledEspNowScrubRemoteForm();
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
  macToDisplay(gOledEspNowState.selectedDeviceMac, macStr, sizeof(macStr));
  
  // Build command: espnowsend json <mac> <message>. The "json" flag makes the
  // handler return {"ok":true,"msgId":N}. cmd_espnow_send records the sent text
  // into the shared peer history itself, so the conversation row appears on the
  // next refresh (and on every other interface too); we only use the ack here to
  // confirm success and keep the view re-rendering so the row can animate
  // Sent → Delivered as the ACK arrives asynchronously.
  char cmdBuf[256];
  snprintf(cmdBuf, sizeof(cmdBuf), "espnowsend json %s %s", macStr, gOledEspNowState.textMessageBuffer.c_str());
  char resp[96];
  const bool sent = executeOledCommandWithResult(cmdBuf, resp, sizeof(resp)) &&
                    !commandReplyJsonFailed(resp);
  if (!sent) {
    // Nothing was stored in the history. Say so and keep the typed message for
    // the next try; a structured refusal gets the fixed text.
    char text[oled_message::kMessageBytes];
    oledCommandReplyText(false, resp[0] == '{' ? "" : resp, nullptr, "Send failed",
                         text, sizeof(text));
    oledToastShow(text, oled_message::kErrorDurationMs, OledUiIcon::ERROR);
    return;
  }
  const char* p = strstr(resp, "\"msgId\":");  // cheap strstr — avoids a JSON parse
  if (p && (uint32_t)strtoul(p + 8, nullptr, 10) != 0) {
    oledMarkDirtyUntil(millis() + 13000);
  }

  // Clear buffer
  gOledEspNowState.textMessageBuffer = "";

  // Refresh message list
  gOledEspNowState.needsRefresh = true;
}

// Hand the form to `espnowremote` and show its verdict. Returns false only
// when a field is still empty: nothing was sent and the form stays as typed.
// Otherwise the form is scrubbed, whether the command left or was refused; the
// peer's own answer (its result, or a refusal such as "Error: Login failed.
// ...") arrives later as a row of the conversation.
bool oledEspNowSendRemoteCommand() {
  if (!gEspNow) return true;
  
  // Validate that all fields are filled
  if (gOledEspNowState.remoteUsername.length() == 0 ||
      gOledEspNowState.remotePassword.length() == 0 ||
      gOledEspNowState.remoteCommand.length() == 0) {
    oledToastShow("Fill all 3 fields", 2500);
    return false;  // Don't send if any field is empty
  }
  
  
  // Format MAC address
  char macStr[18];
  macToDisplay(gOledEspNowState.selectedDeviceMac, macStr, sizeof(macStr));
  
  // Build command: espnowremote <mac> <username> <password> <command>
  char cmdBuf[384];
  snprintf(cmdBuf, sizeof(cmdBuf), "espnowremote %s %s %s %s", macStr,
           gOledEspNowState.remoteUsername.c_str(),
           gOledEspNowState.remotePassword.c_str(),
           gOledEspNowState.remoteCommand.c_str());
  // The owner's reply is 255 bytes at most plus the executor's "OK: " stamp;
  // the request id that marks an accepted command is at its end.
  char reply[272];
  const bool executed = executeOledCommandWithResult(cmdBuf, reply, sizeof(reply));

  // Scrub the stack copy too — cmdBuf holds the full "espnowremote ... <pass>"
  // line in plaintext. Volatile so the optimizer can't elide a memset on a
  // buffer it thinks is dead.
  volatile char* vcmd = cmdBuf;
  for (size_t i = 0; i < sizeof(cmdBuf); i++) vcmd[i] = 0;

  // A refusal shows its reason, as for the other owner commands of this
  // screen; a reply without one (lost, structured, usage) gets the fixed text.
  if (!executed || !oledEspNowRemoteAcknowledged(reply)) {
    char text[oled_message::kMessageBytes];
    oledCommandReplyText(false, commandReplyIsError(reply) ? reply : "", nullptr,
                         "Remote not sent", text, sizeof(text));
    oledToastShow(text, oled_message::kErrorDurationMs, OledUiIcon::ERROR);
  }

  // Clear form (scrubbing, not just truncating)
  oledEspNowScrubRemoteForm();
  
  // Refresh message list
  gOledEspNowState.needsRefresh = true;
  return true;
}

// =============================================================================
// ESP-NOW Settings Menu
// =============================================================================

// Settings menu items: 0=Name, 1=Room, 2=Zone, 3=Friendly Name, 4=Tags, 5=Stationary, 6=Passphrase, 7=Role, 8=MasterMAC, 9=BackupMAC, 10=Channel
#define ESPNOW_SETTINGS_COUNT 11

static const char* sEspNowSettingsLabels[ESPNOW_SETTINGS_COUNT] = {
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
    
    // Label and value keep separate fields: the value in a fixed right-aligned
    // column, the label scrolling when selected (OLED_TextField.h).
    const bool selected = i == gOledEspNowState.settingsMenuIndex;
    String value;
    switch (i) {
      case 0: { // Device Name: "HardwareOne-B2C3 (default)" when none is set
        char name[devicename::kDisplayCap];
        (void)SelfDevice::deviceNameDisplay(name, sizeof(name));
        value = name;
        break;
      }
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
        value = gSettings.meshMasterMac;
        if (value.length() == 0) value = "(auto)";
        break;
      case 9: // Backup MAC
        value = gSettings.meshBackupMac;
        if (value.length() == 0) value = "(none)";
        break;
      case 10: // Channel
        value = gSettings.espnowChannel == 0 ? "Auto" : String(gSettings.espnowChannel);
        break;
    }
    
    int valueW = (int)value.length() * 6;
    if (valueW > 60) valueW = 60;
    const int valueX = 118 - valueW;
    oledTextFieldDraw(display, 4, y, valueX - 6 - 4, sEspNowSettingsLabels[i],
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, selected, sEspNowSettingsLabels);
    oledTextFieldDraw(display, valueX, y, valueW, value.c_str(),
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false, sEspNowSettingsLabels);
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
      executeOledCommand(gSettings.espnowStationary ? "espnowstationary 0" : "espnowstationary 1");
      gOledEspNowState.settingsEditField = -1;
      return true;
    }
    
    // Role: cycle through options via command
    if (gOledEspNowState.settingsEditField == 7) {
      if (gSettings.meshRole == MESH_ROLE_WORKER) {
        executeOledCommand("espnowmeshrole master");
      } else if (gSettings.meshRole == MESH_ROLE_MASTER) {
        executeOledCommand("espnowmeshrole backup");
      } else {
        executeOledCommand("espnowmeshrole worker");
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
      executeOledCommand(String("espnowchannel ") + next);
      gOledEspNowState.settingsEditField = -1;
      return true;
    }

    // For other fields, open keyboard
    const char* prompt = sEspNowSettingsLabels[gOledEspNowState.settingsEditField];
    String initialValue = "";
    int maxLen = 32;
    
    // The name and metadata editors accept what their commands accept, no
    // more (a longer entry was refused after typing) and no less: the device
    // name's rule, and for the others the metadata frame's text field without
    // its NUL (V4PayloadMetadata: room 31, zone 31, friendly name 47, tags 53).
    switch (gOledEspNowState.settingsEditField) {
      case 0: // Device Name
        initialValue = gSettings.espnowDeviceName;
        maxLen = (int)devicename::kMaxLen;
        break;
      case 1: // Room
        initialValue = gSettings.espnowRoom;
        maxLen = (int)(sizeof(V4PayloadMetadata::room) - 1);
        break;
      case 2: // Zone
        initialValue = gSettings.espnowZone;
        maxLen = (int)(sizeof(V4PayloadMetadata::zone) - 1);
        break;
      case 3: // Friendly Name
        initialValue = gSettings.espnowFriendlyName;
        maxLen = (int)(sizeof(V4PayloadMetadata::friendlyName) - 1);
        break;
      case 4: // Tags
        initialValue = gSettings.espnowTags;
        maxLen = (int)(sizeof(V4PayloadMetadata::tags) - 1);
        break;
      case 6: // Passphrase
        initialValue = "";  // Don't show existing passphrase
        maxLen = 32;
        break;
      case 8: // Master MAC
        initialValue = gSettings.meshMasterMac;
        maxLen = 17;  // XX:XX:XX:XX:XX:XX
        break;
      case 9: // Backup MAC
        initialValue = gSettings.meshBackupMac;
        maxLen = 17;
        break;
    }
    
    const OledKeyboardDictationPolicy policy =
        gOledEspNowState.settingsEditField == 6
            ? OledKeyboardDictationPolicy::DENY
            : OledKeyboardDictationPolicy::ALLOW_PLAINTEXT;
    oledKeyboardInit(prompt, initialValue.c_str(), maxLen, policy,
                     gOledEspNowState.settingsEditField == 6);  // passphrase
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
      cmd = "devicename " + value;
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
    // The list shows each value, except the passphrase ("****"): its success
    // is the toast.
    if (gOledEspNowState.settingsEditField == 6) executeOledCommandShowResult(cmd);
    else executeOledCommand(cmd);
  }
  gOledEspNowState.settingsEditField = -1;
}

// ============================================================================
// Device Configuration Menu (Remote Device)
// ============================================================================

// Six rows act on the selected peer. Each fills the command line of the remote
// form (ESPNOW_VIEW_REMOTE_FORM) and opens it; the form collects an account of
// the target device and sends `espnowremote <mac> <user> <pass> <command>` on
// Y, so the peer checks the account and runs a registered command. Nothing
// leaves this device before that. "Unpair Device" acts on this device and goes
// to the unpair owner. Host suite oled_espnow_remote_actions checks that each
// command here is a registered one.
struct DeviceConfigAction {
  const char* label;
  const char* remoteCommand;   // what the peer runs; nullptr = not a remote action
  const char* valuePrompt;     // keyboard title when the command takes a value
  uint8_t     valueMax;        // longest value the keyboard accepts
  bool        startsWithPeerName;
};
// A name, room, zone or friendly name is as long as the peer's command
// accepts: the device name's rule, and the metadata frame's text field
// without its NUL (V4PayloadMetadata: room 31, zone 31, friendly name 47).
static const DeviceConfigAction kDeviceConfigActions[] = {
  { "Restart Device",    "reboot",             nullptr,                        0,  false },
  { "Set Role",          "espnowmeshrole",     "Role (master/backup/worker):", 16, false },
  { "Set Name",          "devicename",         "Device Name:",                 devicename::kMaxLen, true },
  { "Set Room",          "espnowroom",         "Room:",                        sizeof(V4PayloadMetadata::room) - 1, false },
  { "Set Zone",          "espnowzone",         "Zone:",                        sizeof(V4PayloadMetadata::zone) - 1, false },
  { "Set Friendly Name", "espnowfriendlyname", "Friendly Name:",               sizeof(V4PayloadMetadata::friendlyName) - 1, false },
  { "Unpair Device",     nullptr,              nullptr,                        0,  false },
};
static const int kDeviceConfigCount =
    (int)(sizeof(kDeviceConfigActions) / sizeof(kDeviceConfigActions[0]));
// Every editor limit taken from the name rule or the metadata frame is a
// length the on-screen keyboard holds, which also keeps it inside valueMax.
static_assert(OLED_KEYBOARD_MAX_LENGTH <= UINT8_MAX &&
              devicename::kMaxLen <= OLED_KEYBOARD_MAX_LENGTH &&
              sizeof(V4PayloadMetadata::room) - 1 <= OLED_KEYBOARD_MAX_LENGTH &&
              sizeof(V4PayloadMetadata::zone) - 1 <= OLED_KEYBOARD_MAX_LENGTH &&
              sizeof(V4PayloadMetadata::friendlyName) - 1 <= OLED_KEYBOARD_MAX_LENGTH &&
              sizeof(V4PayloadMetadata::tags) - 1 <= OLED_KEYBOARD_MAX_LENGTH,
              "an OLED name or metadata editor limit does not fit the keyboard");

void oledEspNowOpenDeviceConfig() {
  gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG;
  gOledEspNowState.deviceConfigMenuIndex = 0;
  gOledEspNowState.deviceConfigEditField = -1;
  gOledEspNowState.deviceConfigScrollOffset = 0;
}

// The menu below its title line (the peer's name over a rule): one 10 px row
// per action, as many as the content area holds (3 on the 64 px OLED, every
// row on the P4X-EYE's 128 px canvas). The window follows the cursor.
static constexpr int kDeviceConfigTitleH = 11;   // title row + rule + 1 px gap
static constexpr int kDeviceConfigRowH = 10;
static int oledEspNowDeviceConfigRows() {
  return oledVisibleRows(kDeviceConfigRowH, kDeviceConfigTitleH);
}
static void oledEspNowDeviceConfigFollow() {
  const int rows = oledEspNowDeviceConfigRows();
  const int selected = gOledEspNowState.deviceConfigMenuIndex;
  int& top = gOledEspNowState.deviceConfigScrollOffset;
  if (selected < top) top = selected;
  if (selected >= top + rows) top = selected - rows + 1;
  const int maxTop = kDeviceConfigCount > rows ? kDeviceConfigCount - rows : 0;
  if (top > maxTop) top = maxTop;
  if (top < 0) top = 0;
}

void oledEspNowDisplayDeviceConfig(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Title: the peer being configured, under the global "ESP-NOW" header.
  const int titleY = OLED_CONTENT_START_Y;
  display->setCursor(0, titleY);
  display->print("Config:");
  oledTextFieldDraw(display, 48, titleY, SCREEN_WIDTH - 48, gOledEspNowState.selectedDeviceName.c_str(),
                    DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false, &gOledEspNowState);
  display->drawFastHLine(0, titleY + 9, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
  
  // The visible window of the menu; the label field ends before the arrows.
  oledEspNowDeviceConfigFollow();
  const int rows = oledEspNowDeviceConfigRows();
  const int top = gOledEspNowState.deviceConfigScrollOffset;
  const int listY = OLED_CONTENT_START_Y + kDeviceConfigTitleH;
  for (int row = 0; row < rows && top + row < kDeviceConfigCount; row++) {
    const int i = top + row;
    const int y = listY + row * kDeviceConfigRowH;
    const bool selected = i == gOledEspNowState.deviceConfigMenuIndex;
    if (selected) display->fillRect(0, y, 2, kDeviceConfigRowH - 1, DISPLAY_COLOR_WHITE);
    oledTextFieldDraw(display, 4, y, 118 - 4, kDeviceConfigActions[i].label,
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, selected, kDeviceConfigActions);
  }
  if (top > 0) {
    display->setCursor(120, listY);
    display->print("\x18");
  }
  if (top + rows < kDeviceConfigCount) {
    display->setCursor(120, listY + (rows - 1) * kDeviceConfigRowH);
    display->print("\x19");
  }
  
  // Note: Footer is drawn by global render loop
}

bool oledEspNowHandleDeviceConfigInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Navigation
  if (gNavEvents.up && gOledEspNowState.deviceConfigMenuIndex > 0) {
    gOledEspNowState.deviceConfigMenuIndex--;
    oledEspNowDeviceConfigFollow();
    return true;
  }
  if (gNavEvents.down && gOledEspNowState.deviceConfigMenuIndex < kDeviceConfigCount - 1) {
    gOledEspNowState.deviceConfigMenuIndex++;
    oledEspNowDeviceConfigFollow();
    return true;
  }
  
  // A button: the selected action. A remote action only fills in and opens
  // the remote form (after a keyboard for its value, if it takes one).
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    const int index = gOledEspNowState.deviceConfigMenuIndex;
    if (index < 0 || index >= kDeviceConfigCount) return true;
    const DeviceConfigAction& action = kDeviceConfigActions[index];
    if (!action.remoteCommand) {
      oledEspNowUnpairDevice();
    } else if (!action.valuePrompt) {
      oledEspNowOpenRemoteFormWith(action.remoteCommand);
    } else {
      gOledEspNowState.deviceConfigEditField = index;
      oledKeyboardInit(
          action.valuePrompt,
          action.startsWithPeerName ? gOledEspNowState.selectedDeviceName.c_str() : "",
          action.valueMax, OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
      gOledEspNowState.currentView = ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD;
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

// The value typed for a Device Config row: "<command> <value>" goes into the
// remote form, which opens for the target account. Nothing is sent here and
// nothing shown on this device changes; an empty value returns to the menu.
void oledEspNowApplyDeviceConfigEdit(const String& value) {
  const int field = gOledEspNowState.deviceConfigEditField;
  gOledEspNowState.deviceConfigEditField = -1;
  if (value.length() == 0 || field < 0 || field >= kDeviceConfigCount) return;
  const DeviceConfigAction& action = kDeviceConfigActions[field];
  if (!action.remoteCommand || !action.valuePrompt) return;
  String command = action.remoteCommand;
  command += ' ';
  command += value;
  oledEspNowOpenRemoteFormWith(command);
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
