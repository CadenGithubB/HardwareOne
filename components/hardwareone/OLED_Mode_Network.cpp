// OLED_Mode_Network.cpp - Network and mesh display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_User.h"
#include "i2csensor-seesaw.h"  // For JOYSTICK_DEADZONE

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include <esp_wifi.h>
#endif

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

#if ENABLE_ESPNOW
extern MeshPeerHealth gMeshPeers[];
extern String macToHexString(const uint8_t* mac);
extern void macFromHexString(const String& s, uint8_t out[6]);
extern bool isSelfMac(const uint8_t* mac);
extern bool isMeshPeerAlive(const MeshPeerHealth* peer);
extern bool meshEnabled();
extern String getEspNowDeviceName(const uint8_t* mac);
#endif

// Network menu state - non-static for extern access from OLED_Display.cpp
int networkMenuSelection = 0;
extern const int NETWORK_MENU_ITEMS = 5;  // extern needed for const to have external linkage
bool networkShowingStatus = false;
bool networkShowingWiFiSubmenu = false;

// WiFi submenu uses shared scrolling framework - non-static for extern access
OLEDScrollState wifiSubmenuScroll;
bool wifiSubmenuScrollInitialized = false;

// Non-static for extern access from OLED_Display.cpp keyboard handler
bool wifiAddingNetwork = false;
bool wifiEnteringSSID = false;
bool wifiEnteringPassword = false;
String wifiNewSSID = "";
String wifiNewPassword = "";

// Initialize and populate the WiFi Management scrolling submenu
// Non-static for extern access from OLED_Display.cpp
void initWifiSubmenuScroll() {
  if (!oledDisplay) return;

  if (!wifiSubmenuScrollInitialized) {
    int visibleLines = oledScrollCalculateVisibleLines(oledDisplay->height(),
                                                       1,
                                                       true,  // hasTitle
                                                       true); // hasFooter
    oledScrollInit(&wifiSubmenuScroll, "WiFi Management", visibleLines);
    wifiSubmenuScroll.footer = "X:Select  B:Back";
    wifiSubmenuScroll.wrapAround = true;
    wifiSubmenuScrollInitialized = true;
  } else {
    oledScrollClear(&wifiSubmenuScroll);
  }

  // Build submenu items in fixed order
  oledScrollAddItem(&wifiSubmenuScroll, "List Networks");
  oledScrollAddItem(&wifiSubmenuScroll, "Add Network");
  oledScrollAddItem(&wifiSubmenuScroll, "Remove Network");
  oledScrollAddItem(&wifiSubmenuScroll, "Connect Best");
  oledScrollAddItem(&wifiSubmenuScroll, "Scan Networks");

  wifiSubmenuScroll.selectedIndex = 0;
  wifiSubmenuScroll.scrollOffset = 0;
}

// ============================================================================
// Network Menu Display Functions
// ============================================================================

void displayNetworkInfo() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  
#if ENABLE_WIFI
  if (networkShowingStatus) {
    // Show detailed status screen
    oledDisplay->println("== NETWORK STATUS ==");
    oledDisplay->println();
    
    if (WiFi.isConnected()) {
      oledDisplay->print("SSID: ");
      String ssid = WiFi.SSID();
      if (ssid.length() > 12) ssid = ssid.substring(0, 11) + "~";
      oledDisplay->println(ssid);
      oledDisplay->print("IP: ");
      oledDisplay->println(WiFi.localIP());
      oledDisplay->print("RSSI: ");
      oledDisplay->print(WiFi.RSSI());
      oledDisplay->println(" dBm");
    } else {
      oledDisplay->println("WiFi: Disconnected");
    }
    
    oledDisplay->println();
    return;
  }

  // WiFi Management submenu: use shared scrolling renderer
  if (networkShowingWiFiSubmenu) {
    if (!wifiSubmenuScrollInitialized || wifiSubmenuScroll.itemCount == 0) {
      initWifiSubmenuScroll();
    }
    oledScrollRender(oledDisplay, &wifiSubmenuScroll, true, true);
    return;
  }
  
  oledDisplay->print("NETWORK ");
  
  // Show current status inline to save vertical space
  bool wifiConnected = WiFi.isConnected();
  if (wifiConnected) {
    oledDisplay->print(WiFi.RSSI());
    oledDisplay->println("dBm");
  } else {
    oledDisplay->println("(off)");
  }
  
  // Menu options - dynamic based on WiFi and HTTP state
#if ENABLE_HTTP_SERVER
  extern httpd_handle_t server;
  bool httpRunning = (server != nullptr);
#else
  bool httpRunning = false;
#endif
  
  // Build dynamic menu - show Connect OR Disconnect based on state
  const char* options[NETWORK_MENU_ITEMS];
  options[0] = "View Status";
  options[1] = wifiConnected ? "---" : "Connect";        // Hide Connect when connected
  options[2] = "WiFi Management";
  options[3] = wifiConnected ? "Disconnect" : "---";     // Hide Disconnect when not connected
  options[4] = httpRunning ? "Close HTTP" : "Open HTTP";
  
  for (int i = 0; i < NETWORK_MENU_ITEMS; i++) {
    // Skip disabled options in display
    if (strcmp(options[i], "---") == 0) {
      continue;
    }
    if (i == networkMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->print(options[i]);
    if (i == 4 && httpRunning) oledDisplay->print(" *");
    oledDisplay->println();
  }
#else
  oledDisplay->println("=== NETWORK ===");
  oledDisplay->println();
  oledDisplay->println("WiFi: Disabled");
  oledDisplay->println();
  oledDisplay->println("Compile with");
  oledDisplay->println("ENABLE_WIFI=1");
#endif
}

void displayMeshStatus() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  
#if ENABLE_ESPNOW
  if (!gEspNow || !gEspNow->initialized) {
    oledDisplay->println("ESP-NOW not init");
    return;
  }

  if (!meshEnabled()) {
    oledDisplay->println("Mesh disabled");
    oledDisplay->println();
    oledDisplay->println("Use 'espnow mode'");
    oledDisplay->println("to enable mesh");
    return;
  }

  // Get my MAC and name
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myName = getEspNowDeviceName(myMac);
  if (myName.length() == 0) {
    myName = macToHexString(myMac).substring(8);
  }
  if (myName.length() > 10) {
    myName = myName.substring(0, 10);
  }

  // Display based on role
  if (gSettings.meshRole == MESH_ROLE_WORKER && gSettings.meshMasterMAC.length() > 0) {
    uint8_t masterMac[6];
    macFromHexString(gSettings.meshMasterMAC, masterMac);
    String masterName = getEspNowDeviceName(masterMac);
    if (masterName.length() == 0) {
      masterName = gSettings.meshMasterMAC.substring(8);
    }
    if (masterName.length() > 10) {
      masterName = masterName.substring(0, 10);
    }
    
    oledDisplay->print(masterName);
    oledDisplay->println(" [M]");
    oledDisplay->print("  ");
    oledDisplay->print(myName);
    oledDisplay->println(" [W]");
  } else {
    oledDisplay->print(myName);
    if (gSettings.meshRole == MESH_ROLE_MASTER) {
      oledDisplay->println(" [M]");
    } else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) {
      oledDisplay->println(" [B]");
    } else {
      oledDisplay->println(" [W]");
    }
  }

  // Count active peers
  int activePeers = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) && isMeshPeerAlive(&gMeshPeers[i])) {
      activePeers++;
    }
  }
  
  if (activePeers == 0) {
    oledDisplay->println("  No peers");
  } else {
    String indent = (gSettings.meshRole == MESH_ROLE_WORKER && gSettings.meshMasterMAC.length() > 0) ? "    " : "  ";
    oledDisplay->print(indent);
    oledDisplay->print(activePeers);
    oledDisplay->println(" peer(s)");
  }
#else
  oledDisplay->println("ESP-NOW disabled");
#endif
}

// ============================================================================
// Network Menu Navigation
// ============================================================================

// Helper to check if menu item is disabled based on WiFi state
static bool isNetworkMenuItemDisabled(int idx) {
#if ENABLE_WIFI
  bool wifiConnected = WiFi.isConnected();
  if (idx == 1 && wifiConnected) return true;   // Connect disabled when connected
  if (idx == 3 && !wifiConnected) return true;  // Disconnect disabled when not connected
#endif
  return false;
}

void networkMenuUp() {
  if (networkShowingStatus) return;
  if (networkShowingWiFiSubmenu) {
    oledScrollUp(&wifiSubmenuScroll);
    return;
  }
  // Move up, skipping disabled items
  int startIdx = networkMenuSelection;
  do {
    if (networkMenuSelection > 0) {
      networkMenuSelection--;
    } else {
      networkMenuSelection = NETWORK_MENU_ITEMS - 1;
    }
  } while (isNetworkMenuItemDisabled(networkMenuSelection) && networkMenuSelection != startIdx);
}

void networkMenuDown() {
  if (networkShowingStatus) return;
  if (networkShowingWiFiSubmenu) {
    oledScrollDown(&wifiSubmenuScroll);
    return;
  }
  // Move down, skipping disabled items
  int startIdx = networkMenuSelection;
  do {
    if (networkMenuSelection < NETWORK_MENU_ITEMS - 1) {
      networkMenuSelection++;
    } else {
      networkMenuSelection = 0;
    }
  } while (isNetworkMenuItemDisabled(networkMenuSelection) && networkMenuSelection != startIdx);
}

// Confirmation callbacks for HTTP Start/Stop
static void httpStartConfirmedNetwork(void* userData) {
  (void)userData;
  executeOLEDCommand("openhttp");
}

static void httpStopConfirmedNetwork(void* userData) {
  (void)userData;
  executeOLEDCommand("closehttp");
}

void executeNetworkAction() {
  if (networkShowingStatus) {
    networkShowingStatus = false;
    return;
  }
  
  if (networkShowingWiFiSubmenu) {
    int idx = wifiSubmenuScroll.selectedIndex;
    switch (idx) {
      case 0: // List Networks
        executeOLEDCommand("wifilist");
        break;
      case 1: // Add Network
        wifiAddingNetwork = true;
        wifiEnteringSSID = true;
        oledKeyboardInit("Enter SSID:", "");
        break;
      case 2: // Remove Network
        executeOLEDCommand("wifilist");
        break;
      case 3: // Connect Best
        executeOLEDCommand("wificonnect --best");
        networkShowingWiFiSubmenu = false;
        break;
      case 4: // Scan Networks
        executeOLEDCommand("wifiscan");
        break;
    }
    return;
  }
  
  switch (networkMenuSelection) {
    case 0: // View Status
      networkShowingStatus = true;
      break;
      
    case 1: // Connect (best available network)
      executeOLEDCommand("wificonnect --best");
      break;
      
    case 2: // WiFi Management submenu
      networkShowingWiFiSubmenu = true;
      initWifiSubmenuScroll();
      break;
      
    case 3: // Disconnect
      executeOLEDCommand("wifidisconnect");
      break;
      
    case 4: // Toggle HTTP (Start or Stop based on current state)
#if ENABLE_HTTP_SERVER
      {
        extern httpd_handle_t server;
        if (server != nullptr) {
          oledConfirmRequest("Stop HTTP?", nullptr, httpStopConfirmedNetwork, nullptr, false);
        } else {
          oledConfirmRequest("Start HTTP?", nullptr, httpStartConfirmedNetwork, nullptr);
        }
      }
#endif
      break;
  }
}

void networkMenuBack() {
  if (networkShowingStatus) {
    networkShowingStatus = false;
  } else if (networkShowingWiFiSubmenu) {
    networkShowingWiFiSubmenu = false;
    if (wifiSubmenuScrollInitialized) {
      oledScrollClear(&wifiSubmenuScroll);
    }
  }
}

// ============================================================================
// Network Input Handler
// ============================================================================

bool networkInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (currentOLEDMode != OLED_NETWORK_INFO) return false;
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    executeNetworkAction();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (networkShowingStatus || networkShowingWiFiSubmenu) {
      networkMenuBack();
      return true;
    }
    // Let main handler do popOLEDMode
    return false;
  }
  if (deltaY < -JOYSTICK_DEADZONE) {
    networkMenuUp();
    return true;
  }
  if (deltaY > JOYSTICK_DEADZONE) {
    networkMenuDown();
    return true;
  }
  return false;
}

// ============================================================================
// ESP-NOW Display
// ============================================================================

extern void enterUnavailablePage(const String& title, const String& reason);
extern void oledEspNowDisplay(Adafruit_SSD1306* display);
extern void oledEspNowShowInitPrompt();

void displayEspNow() {
#if !ENABLE_ESPNOW
  enterUnavailablePage("ESP-NOW", "Disabled at\ncompile time");
  return;
#else
  // Check if keyboard is active first (user entering device name for setup)
  // This allows keyboard to show even when espnowenabled is 0
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }
  
  if (!gSettings.espnowenabled && (!gEspNow || !gEspNow->initialized)) {
    enterUnavailablePage("ESP-NOW", "Disabled\nRun: espnowenabled 1\nReboot required");
    return;
  }

  // Check if ESP-NOW is initialized
  if (!gEspNow || !gEspNow->initialized) {
    
    // Show initialization prompt (Y button to start)
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("=== ESP-NOW ===");
    oledDisplay->println();
    oledDisplay->println("ESP-NOW not");
    oledDisplay->println("initialized");
    oledDisplay->println();
    oledDisplay->println("Press Y to enter");
    oledDisplay->println("device name");
    // Note: Button hints now handled by global footer
    return;
  }

  // Use new modular OLED ESP-NOW interface
  oledEspNowDisplay(oledDisplay);
#endif // ENABLE_ESPNOW
}

// ============================================================================
// Network Info Rendered (two-phase rendering)
// ============================================================================

// Pre-gathered network data to avoid WiFi operations inside I2C transaction
struct NetworkRenderData {
  bool wifiConnected;
  char ssid[16];  // Truncated SSID
  char ip[16];     // IP address string
  int rssi;
  bool valid;
};
static NetworkRenderData networkRenderData = {0};

// Gather network data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareNetworkData() {
#if ENABLE_WIFI
  networkRenderData.wifiConnected = WiFi.isConnected();
  
  if (networkRenderData.wifiConnected) {
    // Get WiFi data OUTSIDE I2C transaction
    String ssid = WiFi.SSID();
    if (ssid.length() > 15) ssid = ssid.substring(0, 15);
    strncpy(networkRenderData.ssid, ssid.c_str(), 15);
    networkRenderData.ssid[15] = '\0';
    
    String ip = WiFi.localIP().toString();
    strncpy(networkRenderData.ip, ip.c_str(), 15);
    networkRenderData.ip[15] = '\0';
    
    networkRenderData.rssi = WiFi.RSSI();
  }
  
  networkRenderData.valid = true;
#else
  networkRenderData.wifiConnected = false;
  networkRenderData.valid = true;
#endif
}

// Render network info from pre-gathered data (called INSIDE I2C transaction)
void displayNetworkInfoRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!networkRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("Network data");
    oledDisplay->println("unavailable");
    return;
  }
  
  oledDisplay->setTextSize(1);
  
#if ENABLE_WIFI
  if (networkShowingStatus) {
    // Show detailed status screen
    oledDisplay->println("== NETWORK STATUS ==");
    oledDisplay->println();
    
    if (networkRenderData.wifiConnected) {
      oledDisplay->print("SSID: ");
      oledDisplay->println(networkRenderData.ssid);
      oledDisplay->print("IP: ");
      oledDisplay->println(networkRenderData.ip);
      oledDisplay->print("RSSI: ");
      oledDisplay->print(networkRenderData.rssi);
      oledDisplay->println(" dBm");
    } else {
      oledDisplay->println("WiFi: Disconnected");
    }
    
    oledDisplay->println();
    return;
  }

  // WiFi Management submenu: use shared scrolling renderer
  if (networkShowingWiFiSubmenu) {
    if (!wifiSubmenuScrollInitialized || wifiSubmenuScroll.itemCount == 0) {
      initWifiSubmenuScroll();
    }
    oledScrollRender(oledDisplay, &wifiSubmenuScroll, true, true);
    return;
  }

  // Render main network menu using the same dynamic hiding logic as displayNetworkInfo()
  // (so Connect/Disconnect are hidden based on WiFi connection state).
  if (!networkRenderData.wifiConnected && networkMenuSelection == 3) {
    networkMenuSelection = 0;
  }
  if (networkRenderData.wifiConnected && networkMenuSelection == 1) {
    networkMenuSelection = 0;
  }
  oledDisplay->print("NETWORK ");
  if (networkRenderData.wifiConnected) {
    oledDisplay->print(networkRenderData.rssi);
    oledDisplay->println("dBm");
  } else {
    oledDisplay->println("(off)");
  }

  // Menu options - dynamic based on WiFi and HTTP state
#if ENABLE_HTTP_SERVER
  extern httpd_handle_t server;
  bool httpRunning = (server != nullptr);
#else
  bool httpRunning = false;
#endif

  const char* options[NETWORK_MENU_ITEMS];
  options[0] = "View Status";
  options[1] = networkRenderData.wifiConnected ? "---" : "Connect";
  options[2] = "WiFi Management";
  options[3] = networkRenderData.wifiConnected ? "Disconnect" : "---";
  options[4] = httpRunning ? "Close HTTP" : "Open HTTP";

  for (int i = 0; i < NETWORK_MENU_ITEMS; i++) {
    if (strcmp(options[i], "---") == 0) {
      continue;
    }
    if (i == networkMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->print(options[i]);
    if (i == 4 && httpRunning) oledDisplay->print(" *");
    oledDisplay->println();
  }
#else
  oledDisplay->println("=== NETWORK ===");
  oledDisplay->println();
  oledDisplay->println("WiFi: Disabled");
  oledDisplay->println();
  oledDisplay->println("Compile with");
  oledDisplay->println("ENABLE_WIFI=1");
#endif
}

// ============================================================================
// Web Stats Rendered (two-phase rendering)
// ============================================================================

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
extern httpd_handle_t server;
#endif

extern SessionEntry* gSessions;

// Pre-gathered web server stats to avoid session operations inside I2C transaction
struct WebStatsRenderData {
  int activeSessions;
  int totalSessions;
  unsigned long uptimeSeconds;
  int failedLoginAttempts;
  bool httpServerRunning;
  bool valid;
};
static WebStatsRenderData webStatsRenderData = {0};

// Gather web stats data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareWebStatsData() {
  // Count active sessions
  webStatsRenderData.activeSessions = 0;
  webStatsRenderData.totalSessions = 0;
  
  if (gSessions) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (gSessions[i].sid.length() > 0) {
        webStatsRenderData.totalSessions++;
        // Check if session is not expired
        unsigned long now = millis();
        if (!(gSessions[i].expiresAt > 0 && (long)(now - gSessions[i].expiresAt) >= 0)) {
          webStatsRenderData.activeSessions++;
        }
      }
    }
  }
  
  // Get uptime
  webStatsRenderData.uptimeSeconds = millis() / 1000;
  
  // Check if HTTP server is running
#if ENABLE_HTTP_SERVER
  webStatsRenderData.httpServerRunning = (server != nullptr);
#else
  webStatsRenderData.httpServerRunning = false;
#endif
  
  // TODO: Track failed login attempts (needs global counter)
  webStatsRenderData.failedLoginAttempts = 0;
  
  webStatsRenderData.valid = true;
}

// Render web stats from pre-gathered data (called INSIDE I2C transaction)
void displayWebStatsRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!webStatsRenderData.valid) {
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("Web Stats Error");
    return;
  }
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  oledDisplay->println("=== WEB SERVER ===");
  oledDisplay->println();
  
  // Server status
  oledDisplay->print("Status: ");
  if (webStatsRenderData.httpServerRunning) {
    oledDisplay->println("Running");
  } else {
    oledDisplay->println("Stopped");
  }
  
  // Active sessions
  oledDisplay->print("Active: ");
  oledDisplay->print(webStatsRenderData.activeSessions);
  oledDisplay->print("/");
  oledDisplay->print(MAX_SESSIONS);
  oledDisplay->println(" users");
  
  // Total sessions (including expired)
  oledDisplay->print("Total: ");
  oledDisplay->print(webStatsRenderData.totalSessions);
  oledDisplay->println(" sessions");
  
  // Uptime
  oledDisplay->print("Uptime: ");
  unsigned long hours = webStatsRenderData.uptimeSeconds / 3600;
  unsigned long minutes = (webStatsRenderData.uptimeSeconds % 3600) / 60;
  oledDisplay->print(hours);
  oledDisplay->print("h ");
  oledDisplay->print(minutes);
  oledDisplay->println("m");
  
  // Failed logins (placeholder)
  if (webStatsRenderData.failedLoginAttempts > 0) {
    oledDisplay->print("Failed: ");
    oledDisplay->println(webStatsRenderData.failedLoginAttempts);
  }
}

// ============================================================================
// Mesh Status Rendered (two-phase rendering)
// ============================================================================

// Pre-gathered mesh status data to avoid WiFi/ESP-NOW operations inside I2C transaction
struct MeshStatusRenderData {
  bool espNowEnabled;
  bool meshEnabled;
  char myName[12];
  char masterName[12];
  int meshRole;
  bool isWorker;
  int activePeers;
  bool valid;
};
static MeshStatusRenderData meshStatusRenderData = {0};

// Gather mesh status data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareMeshStatusData() {
#if ENABLE_ESPNOW
  meshStatusRenderData.espNowEnabled = (gEspNow && gEspNow->initialized);
  meshStatusRenderData.meshEnabled = meshEnabled();
  
  if (meshStatusRenderData.espNowEnabled && meshStatusRenderData.meshEnabled) {
    // Get MAC and device name OUTSIDE I2C transaction
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String myName = getEspNowDeviceName(myMac);
    if (myName.length() == 0) {
      myName = macToHexString(myMac).substring(8);
    }
    if (myName.length() > 11) {
      myName = myName.substring(0, 11);
    }
    strncpy(meshStatusRenderData.myName, myName.c_str(), 11);
    meshStatusRenderData.myName[11] = '\0';
    
    meshStatusRenderData.meshRole = gSettings.meshRole;
    meshStatusRenderData.isWorker = (gSettings.meshRole == MESH_ROLE_WORKER && gSettings.meshMasterMAC.length() > 0);
    
    if (meshStatusRenderData.isWorker) {
      uint8_t masterMac[6];
      macFromHexString(gSettings.meshMasterMAC, masterMac);
      String masterName = getEspNowDeviceName(masterMac);
      if (masterName.length() == 0) {
        masterName = gSettings.meshMasterMAC.substring(8);
      }
      if (masterName.length() > 11) {
        masterName = masterName.substring(0, 11);
      }
      strncpy(meshStatusRenderData.masterName, masterName.c_str(), 11);
      meshStatusRenderData.masterName[11] = '\0';
    }
    
    // Count active peers OUTSIDE I2C transaction
    int activePeers = 0;
    for (int i = 0; i < MESH_PEER_MAX; i++) {
      if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) && isMeshPeerAlive(&gMeshPeers[i])) {
        activePeers++;
      }
    }
    meshStatusRenderData.activePeers = activePeers;
  }
  
  meshStatusRenderData.valid = true;
#else
  meshStatusRenderData.espNowEnabled = false;
  meshStatusRenderData.valid = true;
#endif
}

// Render mesh status from pre-gathered data (called INSIDE I2C transaction)
void displayMeshStatusRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!meshStatusRenderData.valid) {
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("Mesh Error");
    return;
  }
  
  oledDisplay->setTextSize(1);
  
#if ENABLE_ESPNOW
  if (!meshStatusRenderData.espNowEnabled) {
    oledDisplay->println("ESP-NOW not init");
    return;
  }
  
  if (!meshStatusRenderData.meshEnabled) {
    oledDisplay->println("Mesh disabled");
    oledDisplay->println();
    oledDisplay->println("Use 'espnow mode'");
    oledDisplay->println("to enable mesh");
    return;
  }

  // Display based on role
  if (meshStatusRenderData.isWorker) {
    oledDisplay->print(meshStatusRenderData.masterName);
    oledDisplay->println(" [M]");
    oledDisplay->print("  ");
    oledDisplay->print(meshStatusRenderData.myName);
    oledDisplay->println(" [W]");
  } else {
    oledDisplay->print(meshStatusRenderData.myName);
    if (meshStatusRenderData.meshRole == MESH_ROLE_MASTER) {
      oledDisplay->println(" [M]");
    } else if (meshStatusRenderData.meshRole == MESH_ROLE_BACKUP_MASTER) {
      oledDisplay->println(" [B]");
    } else {
      oledDisplay->println(" [W]");
    }
  }

  if (meshStatusRenderData.activePeers == 0) {
    oledDisplay->println("  No peers");
  } else {
    String indent = meshStatusRenderData.isWorker ? "    " : "  ";
    oledDisplay->print(indent);
    oledDisplay->print(meshStatusRenderData.activePeers);
    oledDisplay->println(" peer(s)");
  }
#else
  oledDisplay->println("ESP-NOW disabled");
#endif // ENABLE_ESPNOW
}

// ==========================
// Remote Sensors Display (moved from System_ESPNow_Sensors.cpp)
// ==========================

#if ENABLE_ESPNOW
#include "System_ESPNow_Sensors.h"
#include <ArduinoJson.h>

static int remoteSensorScrollIndex = 0;

void displayRemoteSensors() {
  if (!oledDisplay) return;
  
  // Clear only content area (not footer) to prevent flickering
  oledDisplay->fillRect(0, 0, SCREEN_WIDTH, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("Remote Sensors");
  oledDisplay->drawLine(0, 9, SCREEN_WIDTH - 1, 9, DISPLAY_COLOR_WHITE);
  
  // Check if ESP-NOW/mesh is properly configured
  bool meshOn = meshEnabled();
  bool isMaster = (gSettings.meshRole == MESH_ROLE_MASTER);
  
  if (!meshOn) {
    // Mesh not enabled - show setup instructions
    oledDisplay->setCursor(0, 14);
    oledDisplay->println("Mesh not enabled!");
    oledDisplay->println("");
    oledDisplay->println("To enable:");
    oledDisplay->println(" espnow mode mesh");
    oledDisplay->println(" espnowenabled 1");
    oledDisplay->println(" (reboot required)");
    return;
  }
  
  if (!isMaster) {
    // Not master - show role setup instructions
    oledDisplay->setCursor(0, 14);
    oledDisplay->println("Not a master device!");
    oledDisplay->println("");
    oledDisplay->println("To set as master:");
    oledDisplay->println(" espnow meshrole master");
    oledDisplay->println("");
    oledDisplay->println("Role: ");
    oledDisplay->print(gSettings.meshRole == MESH_ROLE_WORKER ? "worker" : "backup");
    return;
  }
  
  // Count valid entries
  int validCount = 0;
  int validIndices[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].valid) {
      validIndices[validCount++] = i;
    }
  }
  
  if (validCount == 0) {
    oledDisplay->setCursor(0, 14);
    oledDisplay->println("No remote sensors");
    oledDisplay->println("connected yet.");
    oledDisplay->println("");
    oledDisplay->println("Waiting for workers");
    oledDisplay->println("to send sensor data...");
    return;
  }
  
  // Clamp scroll index
  if (remoteSensorScrollIndex >= validCount) remoteSensorScrollIndex = 0;
  if (remoteSensorScrollIndex < 0) remoteSensorScrollIndex = validCount - 1;
  
  // Display current sensor
  int idx = validIndices[remoteSensorScrollIndex];
  RemoteSensorData* entry = &gRemoteSensorCache[idx];
  
  // Device name and sensor type
  oledDisplay->setCursor(0, 12);
  oledDisplay->print(entry->deviceName);
  oledDisplay->print(" - ");
  oledDisplay->println(sensorTypeToString(entry->sensorType));
  
  // Parse and display sensor data based on type
  if (entry->jsonLength > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, entry->jsonData) == DeserializationError::Ok) {
      switch (entry->sensorType) {
        case REMOTE_SENSOR_GAMEPAD: {
          int x = doc["x"] | 512;
          int y = doc["y"] | 512;
          uint32_t buttons = doc["buttons"] | 0xFFFFFFFF;
          
          // Draw joystick position (small box)
          int joyX = 10 + ((x * 20) / 1023);
          int joyY = 35 + ((y * 15) / 1023);
          oledDisplay->drawRect(10, 35, 22, 17, DISPLAY_COLOR_WHITE);
          oledDisplay->fillCircle(joyX, joyY, 2, DISPLAY_COLOR_WHITE);
          
          oledDisplay->setCursor(40, 28);
          oledDisplay->print("X:");
          oledDisplay->print(x);
          oledDisplay->setCursor(40, 38);
          oledDisplay->print("Y:");
          oledDisplay->print(y);
          
          // Button indicators
          oledDisplay->setCursor(85, 28);
          oledDisplay->print((buttons & (1<<6)) ? " " : "X");
          oledDisplay->print((buttons & (1<<2)) ? " " : "Y");
          oledDisplay->setCursor(85, 38);
          oledDisplay->print((buttons & (1<<5)) ? " " : "A");
          oledDisplay->print((buttons & (1<<1)) ? " " : "B");
          oledDisplay->setCursor(85, 48);
          oledDisplay->print((buttons & (1<<0)) ? " " : "Sel");
          oledDisplay->print((buttons & (1<<16)) ? " " : "St");
          break;
        }
        
        case REMOTE_SENSOR_IMU: {
          // IMU JSON: {"ori":{"yaw":..,"pitch":..,"roll":..},...}
          JsonObject ori = doc["ori"];
          float roll = ori["roll"] | 0.0f;
          float pitch = ori["pitch"] | 0.0f;
          float yaw = ori["yaw"] | 0.0f;
          
          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Roll:  ");
          oledDisplay->print(roll, 1);
          oledDisplay->println(" deg");
          oledDisplay->print("Pitch: ");
          oledDisplay->print(pitch, 1);
          oledDisplay->println(" deg");
          oledDisplay->print("Yaw:   ");
          oledDisplay->print(yaw, 1);
          oledDisplay->println(" deg");
          break;
        }
        
        case REMOTE_SENSOR_GPS: {
          // GPS JSON: {"val":1,"fix":1,"sats":8,"lat":..,"lon":..}
          float lat = doc["lat"] | 0.0f;
          float lon = doc["lon"] | 0.0f;
          int sats = doc["sats"] | 0;
          int fix = doc["fix"] | 0;
          
          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Lat: ");
          oledDisplay->println(lat, 5);
          oledDisplay->print("Lon: ");
          oledDisplay->println(lon, 5);
          oledDisplay->print("Sats: ");
          oledDisplay->print(sats);
          oledDisplay->print(fix ? " (Fix)" : " (No fix)");
          break;
        }
        
        case REMOTE_SENSOR_TOF: {
          // ToF JSON: {"objects":[{"distance_mm":123,"status":0},...]}
          JsonArray objects = doc["objects"];
          if (objects.size() > 0) {
            JsonObject obj = objects[0];
            int dist = obj["distance_mm"] | 0;
            int status = obj["status"] | -1;
            bool detected = obj["detected"] | false;
            
            oledDisplay->setCursor(0, 28);
            if (detected) {
              oledDisplay->print("Distance: ");
              oledDisplay->print(dist);
              oledDisplay->println(" mm");
              oledDisplay->print("Status: ");
              oledDisplay->println(status == 0 ? "OK" : "Error");
            } else {
              oledDisplay->println("No object detected");
            }
          } else {
            oledDisplay->setCursor(0, 28);
            oledDisplay->println("No ToF data");
          }
          break;
        }
        
        case REMOTE_SENSOR_FMRADIO: {
          // FM JSON: {"frequency":101.5,"rssi":45,"station":"..."}
          float freq = doc["frequency"] | 0.0f;
          int rssi = doc["rssi"] | 0;
          const char* station = doc["station"] | "";
          
          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Freq: ");
          oledDisplay->print(freq, 1);
          oledDisplay->println(" MHz");
          oledDisplay->print("RSSI: ");
          oledDisplay->println(rssi);
          if (strlen(station) > 0) {
            oledDisplay->print("Stn: ");
            oledDisplay->println(station);
          }
          break;
        }
        
        default: {
          // Generic JSON display for unknown sensors
          oledDisplay->setCursor(0, 24);
          char truncated[64];
          strncpy(truncated, entry->jsonData, 63);
          truncated[63] = '\0';
          oledDisplay->print(truncated);
          break;
        }
      }
    }
  } else {
    oledDisplay->setCursor(0, 24);
    oledDisplay->println("No data");
  }
  
  // Navigation hint
  oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 8);
  oledDisplay->print(remoteSensorScrollIndex + 1);
  oledDisplay->print("/");
  oledDisplay->print(validCount);
  
  // Note: display() call removed - main OLED loop handles it
}

static bool remoteSensorsAvailable(String* outReason) {
  // Always available - display function shows setup instructions if not configured
  // But provide hint about current state
  if (!meshEnabled()) {
    if (outReason) *outReason = "Mesh off";
    return true;  // Still allow access to see setup instructions
  }
  if (gSettings.meshRole != MESH_ROLE_MASTER) {
    if (outReason) *outReason = "Not master";
    return true;  // Still allow access to see setup instructions
  }
  return true;
}

static bool remoteSensorsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Count valid entries for navigation
  int validCount = 0;
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].valid) validCount++;
  }
  if (validCount == 0) return false;
  
  // Left/Up = previous, Right/Down = next
  if (deltaY > JOYSTICK_DEADZONE || deltaX > JOYSTICK_DEADZONE) {
    remoteSensorScrollIndex++;
    if (remoteSensorScrollIndex >= validCount) remoteSensorScrollIndex = 0;
    return true;
  } else if (deltaY < -JOYSTICK_DEADZONE || deltaX < -JOYSTICK_DEADZONE) {
    remoteSensorScrollIndex--;
    if (remoteSensorScrollIndex < 0) remoteSensorScrollIndex = validCount - 1;
    return true;
  }
  
  return false;
}

// Remote Sensors OLED mode entry
static const OLEDModeEntry remoteSensorsOLEDModes[] = {
  {
    OLED_REMOTE_SENSORS,       // mode enum
    "Remote",                  // menu name
    "notify_sensor",           // icon name
    displayRemoteSensors,      // displayFunc
    remoteSensorsAvailable,    // availFunc
    remoteSensorsInputHandler, // inputFunc
    true,                      // showInMenu
    30                         // menuOrder
  }
};

// Auto-register Remote Sensors OLED mode
REGISTER_OLED_MODE_MODULE(remoteSensorsOLEDModes, sizeof(remoteSensorsOLEDModes) / sizeof(remoteSensorsOLEDModes[0]), "RemoteSensors");

#endif // ENABLE_ESPNOW

#endif // ENABLE_OLED_DISPLAY
