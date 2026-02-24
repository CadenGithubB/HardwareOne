// OLED_Mode_UnifiedMenu.cpp - Unified local+remote actions menu
// Shows actions from both local device and paired remote device
// Allows executing commands on either device from a single interface

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_ESPNow.h"
#include "System_Utils.h"  // For AuthContext, executeCommand
#include <ArduinoJson.h>
#include <LittleFS.h>

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

extern DisplayDriver* oledDisplay;
extern bool oledConnected;
extern Settings gSettings;
extern EspNowState* gEspNow;
extern bool filesystemReady;

extern String getEspNowDeviceName(const uint8_t* mac);
extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);
extern String processCommand(const String& cmd);

// Command execution uses central executeCommand() which handles remote routing
extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

// ==========================
// Unified Menu Item Structure
// ==========================

struct UnifiedMenuItem {
  char name[24];       // Display name (truncated for OLED)
  char command[48];    // CLI command to execute
  bool isRemote;       // true = execute on remote device
  uint8_t category;    // 0=feature, 1=sensor, 2=command
  bool isSubmenu;      // true = opens a submenu
  char moduleName[16]; // module name for submenu items
};

// Menu state
static UnifiedMenuItem* gUnifiedMenuItems = nullptr;
static int gUnifiedMenuItemCount = 0;
static int gUnifiedMenuSelection = 0;
static int gUnifiedMenuScrollOffset = 0;
static bool gUnifiedMenuShowingLocal = true;  // Toggle between local/remote view
static unsigned long gUnifiedMenuLastBuild = 0;
static const int MENU_VISIBLE_ITEMS = 5;  // Items visible on screen at once

// Submenu state
static bool gInSubmenu = false;
static char gSubmenuModuleName[16] = "";
static bool gSubmenuIsRemote = false;
static UnifiedMenuItem* gSubmenuItems = nullptr;
static int gSubmenuItemCount = 0;
static int gSubmenuSelection = 0;
static int gSubmenuScrollOffset = 0;

// Pending command execution state
static bool gPendingRemoteCommand = false;
static unsigned long gPendingCommandTime = 0;
static char gPendingCommandStatus[32] = "";

// ==========================
// Menu Building
// ==========================

static void freeUnifiedMenu() {
  if (gUnifiedMenuItems) {
    free(gUnifiedMenuItems);
    gUnifiedMenuItems = nullptr;
  }
  gUnifiedMenuItemCount = 0;
}

static void freeSubmenu() {
  if (gSubmenuItems) {
    free(gSubmenuItems);
    gSubmenuItems = nullptr;
  }
  gSubmenuItemCount = 0;
  gInSubmenu = false;
  gSubmenuModuleName[0] = '\0';
}

// Build menu from local capabilities
static int buildLocalMenuItems(UnifiedMenuItem* items, int maxItems) {
  int count = 0;
  
  // Add items based on local feature mask (using buildCapabilitySummary logic)
  // These are high-level feature toggles/views
  
#if ENABLE_WIFI
  if (count < maxItems) {
    strncpy(items[count].name, "WiFi Status", 23);
    strncpy(items[count].command, "wifi status", 47);
    items[count].isRemote = false;
    items[count].category = 0;
    count++;
  }
#endif

#if ENABLE_CAMERA_SENSOR
  if (count < maxItems) {
    strncpy(items[count].name, "Camera Capture", 23);
    strncpy(items[count].command, "camera capture", 47);
    items[count].isRemote = false;
    items[count].category = 1;
    count++;
  }
#endif

#if ENABLE_GPS_SENSOR
  if (count < maxItems) {
    strncpy(items[count].name, "GPS Status", 23);
    strncpy(items[count].command, "gps status", 47);
    items[count].isRemote = false;
    items[count].category = 1;
    count++;
  }
#endif

#if ENABLE_THERMAL_SENSOR
  if (count < maxItems) {
    strncpy(items[count].name, "Thermal Read", 23);
    strncpy(items[count].command, "thermal read", 47);
    items[count].isRemote = false;
    items[count].category = 1;
    count++;
  }
#endif

#if ENABLE_IMU_SENSOR
  if (count < maxItems) {
    strncpy(items[count].name, "IMU Status", 23);
    strncpy(items[count].command, "imu status", 47);
    items[count].isRemote = false;
    items[count].category = 1;
    count++;
  }
#endif

  // Common commands
  if (count < maxItems) {
    strncpy(items[count].name, "System Status", 23);
    strncpy(items[count].command, "status", 47);
    items[count].isRemote = false;
    items[count].category = 2;
    count++;
  }
  
  if (count < maxItems) {
    strncpy(items[count].name, "Memory Stats", 23);
    strncpy(items[count].command, "mem", 47);
    items[count].isRemote = false;
    items[count].category = 2;
    count++;
  }

  return count;
}

// Build menu from cached remote manifest
static int buildRemoteMenuItems(UnifiedMenuItem* items, int maxItems, const uint8_t* peerMac) {
  int count = 0;
  
  if (!filesystemReady || !gEspNow || !gEspNow->lastRemoteCapValid) {
    return 0;
  }
  
  // Get fwHash from last received capability to find cached manifest
  char fwHashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(fwHashHex + (i * 2), 3, "%02x", gEspNow->lastRemoteCap.fwHash[i]);
  }
  
  String manifestPath = String("/system/manifests/") + fwHashHex + ".json";
  if (!LittleFS.exists(manifestPath.c_str())) {
    // No cached manifest - add placeholder items based on capability summary
    CapabilitySummary& cap = gEspNow->lastRemoteCap;
    
    if ((cap.featureMask & CAP_FEATURE_CAMERA) && count < maxItems) {
      strncpy(items[count].name, "[R] Camera Capture", 23);
      strncpy(items[count].command, "camera capture", 47);
      items[count].isRemote = true;
      items[count].category = 1;
      count++;
    }
    
    if ((cap.sensorMask & CAP_SENSOR_GPS) && count < maxItems) {
      strncpy(items[count].name, "[R] GPS Status", 23);
      strncpy(items[count].command, "gps status", 47);
      items[count].isRemote = true;
      items[count].category = 1;
      count++;
    }
    
    if ((cap.sensorMask & CAP_SENSOR_THERMAL) && count < maxItems) {
      strncpy(items[count].name, "[R] Thermal Read", 23);
      strncpy(items[count].command, "thermal read", 47);
      items[count].isRemote = true;
      items[count].category = 1;
      count++;
    }
    
    // Always add basic status commands
    if (count < maxItems) {
      strncpy(items[count].name, "[R] System Status", 23);
      strncpy(items[count].command, "status", 47);
      items[count].isRemote = true;
      items[count].category = 2;
      count++;
    }
    
    return count;
  }
  
  // Parse cached manifest for CLI modules - create submenu entries per module
  File f = LittleFS.open(manifestPath.c_str(), "r");
  if (!f) return count;
  
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  
  if (err) return count;
  
  // Extract CLI modules from manifest - create one submenu entry per module
  JsonArray modules = doc["cliModules"].as<JsonArray>();
  for (JsonObject module : modules) {
    if (count >= maxItems) break;
    
    const char* moduleName = module["name"] | "";
    if (strlen(moduleName) == 0) continue;
    
    JsonArray cmds = module["commands"].as<JsonArray>();
    int cmdCount = cmds.size();
    if (cmdCount == 0) continue;
    
    // Skip modules with only test/debug commands
    int usableCmds = 0;
    for (JsonObject cmd : cmds) {
      const char* cmdName = cmd["name"] | "";
      if (!strstr(cmdName, "test") && !strstr(cmdName, "debug")) usableCmds++;
    }
    if (usableCmds == 0) continue;
    
    // Create submenu entry for this module
    char displayName[24];
    snprintf(displayName, sizeof(displayName), "[R] %s >", moduleName);
    strncpy(items[count].name, displayName, 23);
    items[count].name[23] = '\0';
    items[count].command[0] = '\0';
    items[count].isRemote = true;
    items[count].isSubmenu = true;
    strncpy(items[count].moduleName, moduleName, 15);
    items[count].moduleName[15] = '\0';
    items[count].category = 2;
    count++;
  }
  
  return count;
}

static void buildSubmenuForModule(const char* moduleName, bool isRemote) {
  freeSubmenu();
  
  strncpy(gSubmenuModuleName, moduleName, 15);
  gSubmenuModuleName[15] = '\0';
  gSubmenuIsRemote = isRemote;
  gSubmenuSelection = 0;
  gSubmenuScrollOffset = 0;
  
  const int maxItems = 24;
  gSubmenuItems = (UnifiedMenuItem*)malloc(sizeof(UnifiedMenuItem) * maxItems);
  if (!gSubmenuItems) return;
  memset(gSubmenuItems, 0, sizeof(UnifiedMenuItem) * maxItems);
  
  int count = 0;
  
  if (isRemote && gEspNow && gEspNow->lastRemoteCapValid && filesystemReady) {
    char fwHashHex[33];
    for (int i = 0; i < 16; i++) {
      snprintf(fwHashHex + (i * 2), 3, "%02x", gEspNow->lastRemoteCap.fwHash[i]);
    }
    
    String manifestPath = String("/system/manifests/") + fwHashHex + ".json";
    File f = LittleFS.open(manifestPath.c_str(), "r");
    if (f) {
      DynamicJsonDocument doc(4096);
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      
      if (!err) {
        JsonArray modules = doc["cliModules"].as<JsonArray>();
        for (JsonObject module : modules) {
          const char* modName = module["name"] | "";
          if (strcmp(modName, moduleName) != 0) continue;
          
          JsonArray cmds = module["commands"].as<JsonArray>();
          for (JsonObject cmd : cmds) {
            if (count >= maxItems) break;
            
            const char* cmdName = cmd["name"] | "";
            if (strstr(cmdName, "test") || strstr(cmdName, "debug")) continue;
            
            strncpy(gSubmenuItems[count].name, cmdName, 23);
            gSubmenuItems[count].name[23] = '\0';
            strncpy(gSubmenuItems[count].command, cmdName, 47);
            gSubmenuItems[count].command[47] = '\0';
            gSubmenuItems[count].isRemote = true;
            gSubmenuItems[count].isSubmenu = false;
            count++;
          }
          break;
        }
      }
    }
  }
  
  gSubmenuItemCount = count;
  gInSubmenu = (count > 0);
}

static void buildUnifiedMenu() {
  freeUnifiedMenu();
  
  // Allocate space for menu items
  const int maxItems = 32;
  gUnifiedMenuItems = (UnifiedMenuItem*)malloc(sizeof(UnifiedMenuItem) * maxItems);
  if (!gUnifiedMenuItems) return;
  
  memset(gUnifiedMenuItems, 0, sizeof(UnifiedMenuItem) * maxItems);
  
  int count = 0;
  
  // Build local items
  count += buildLocalMenuItems(&gUnifiedMenuItems[count], maxItems - count);
  
  // Build remote items if paired
  if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() > 0) {
    uint8_t peerMac[6];
    if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
      count += buildRemoteMenuItems(&gUnifiedMenuItems[count], maxItems - count, peerMac);
    }
  }
  
  gUnifiedMenuItemCount = count;
  gUnifiedMenuLastBuild = millis();
}

// ==========================
// Command Execution
// ==========================

static void executeMenuItem(UnifiedMenuItem& item) {
  if (item.isSubmenu) {
    buildSubmenuForModule(item.moduleName, item.isRemote);
    return;
  }
  
  String cmdToExecute = item.command;
  if (item.isRemote) {
    cmdToExecute = "remote:" + String(item.command);
  }
  
  strncpy(gPendingCommandStatus, "Running...", sizeof(gPendingCommandStatus));
  
  AuthContext ctx;
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user = "oled";
  ctx.ip = "local";
  ctx.path = "/oled/unified";
  ctx.sid = "";
  
  char out[512];
  bool success = executeCommand(ctx, cmdToExecute.c_str(), out, sizeof(out));
  
  if (item.isRemote) {
    if (success) {
      strncpy(gPendingCommandStatus, "Sent", sizeof(gPendingCommandStatus));
      gPendingRemoteCommand = true;
      gPendingCommandTime = millis();
    } else {
      strncpy(gPendingCommandStatus, out, sizeof(gPendingCommandStatus));
      gPendingCommandStatus[sizeof(gPendingCommandStatus) - 1] = '\0';
    }
  } else {
    strncpy(gPendingCommandStatus, success ? "Done" : "Failed", sizeof(gPendingCommandStatus));
    if (strlen(out) > 0) {
      broadcastOutput(out);
    }
  }
}

static void executeUnifiedMenuItem(int index) {
  if (index < 0 || index >= gUnifiedMenuItemCount || !gUnifiedMenuItems) return;
  executeMenuItem(gUnifiedMenuItems[index]);
}

static void executeSubmenuItem(int index) {
  if (index < 0 || index >= gSubmenuItemCount || !gSubmenuItems) return;
  executeMenuItem(gSubmenuItems[index]);
}

// ==========================
// Display Function
// ==========================

static void displaySubmenu() {
  oledDisplay->print("< ");
  oledDisplay->println(gSubmenuModuleName);
  
  if (gSubmenuItemCount == 0) {
    oledDisplay->println();
    oledDisplay->println("No commands.");
    return;
  }
  
  if (gSubmenuSelection < gSubmenuScrollOffset) {
    gSubmenuScrollOffset = gSubmenuSelection;
  } else if (gSubmenuSelection >= gSubmenuScrollOffset + MENU_VISIBLE_ITEMS) {
    gSubmenuScrollOffset = gSubmenuSelection - MENU_VISIBLE_ITEMS + 1;
  }
  
  for (int i = 0; i < MENU_VISIBLE_ITEMS && (gSubmenuScrollOffset + i) < gSubmenuItemCount; i++) {
    int idx = gSubmenuScrollOffset + i;
    UnifiedMenuItem& item = gSubmenuItems[idx];
    
    if (idx == gSubmenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    
    char displayName[22];
    strncpy(displayName, item.name, 21);
    displayName[21] = '\0';
    oledDisplay->println(displayName);
  }
  
  if (gSubmenuScrollOffset > 0) {
    oledDisplay->setCursor(120, 16);
    oledDisplay->print("^");
  }
  if (gSubmenuScrollOffset + MENU_VISIBLE_ITEMS < gSubmenuItemCount) {
    oledDisplay->setCursor(120, OLED_CONTENT_HEIGHT - 8);
    oledDisplay->print("v");
  }
}

void displayUnifiedMenu() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    oledDisplay->println("Not bonded.");
    oledDisplay->println();
    oledDisplay->println("Use CLI:");
    oledDisplay->println("  bond connect <device>");
    return;
  }
  
  if (!gUnifiedMenuItems || (millis() - gUnifiedMenuLastBuild > 30000)) {
    buildUnifiedMenu();
  }
  
  if (gInSubmenu) {
    displaySubmenu();
    return;
  }
  
  uint8_t peerMac[6];
  String peerName = gSettings.bondPeerMac;
  if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    String name = getEspNowDeviceName(peerMac);
    if (name.length() > 0) peerName = name;
  }
  
  oledDisplay->print("Peer: ");
  oledDisplay->println(peerName.substring(0, 10));
  
  if (gPendingRemoteCommand) {
    if (millis() - gPendingCommandTime > 5000) {
      gPendingRemoteCommand = false;
      strncpy(gPendingCommandStatus, "Timeout", sizeof(gPendingCommandStatus));
    }
  }
  if (strlen(gPendingCommandStatus) > 0) {
    oledDisplay->print("Status: ");
    oledDisplay->println(gPendingCommandStatus);
  }
  
  if (gUnifiedMenuItemCount == 0) {
    oledDisplay->println();
    oledDisplay->println("No actions available.");
    oledDisplay->println("Request manifest first.");
    return;
  }
  
  if (gUnifiedMenuSelection < gUnifiedMenuScrollOffset) {
    gUnifiedMenuScrollOffset = gUnifiedMenuSelection;
  } else if (gUnifiedMenuSelection >= gUnifiedMenuScrollOffset + MENU_VISIBLE_ITEMS) {
    gUnifiedMenuScrollOffset = gUnifiedMenuSelection - MENU_VISIBLE_ITEMS + 1;
  }
  
  for (int i = 0; i < MENU_VISIBLE_ITEMS && (gUnifiedMenuScrollOffset + i) < gUnifiedMenuItemCount; i++) {
    int idx = gUnifiedMenuScrollOffset + i;
    UnifiedMenuItem& item = gUnifiedMenuItems[idx];
    
    if (idx == gUnifiedMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    
    char displayName[22];
    strncpy(displayName, item.name, 21);
    displayName[21] = '\0';
    oledDisplay->println(displayName);
  }
  
  if (gUnifiedMenuScrollOffset > 0) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
    oledDisplay->print("^");
  }
  if (gUnifiedMenuScrollOffset + MENU_VISIBLE_ITEMS < gUnifiedMenuItemCount) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
    oledDisplay->print("v");
  }
}

// ==========================
// Input Handling
// ==========================

bool handleUnifiedMenuInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (gInSubmenu) {
    if (deltaY < -10) {
      if (gSubmenuSelection > 0) {
        gSubmenuSelection--;
        return true;
      }
    } else if (deltaY > 10) {
      if (gSubmenuSelection < gSubmenuItemCount - 1) {
        gSubmenuSelection++;
        return true;
      }
    }
    
    if (newlyPressed & 0x01) {
      executeSubmenuItem(gSubmenuSelection);
      return true;
    }
    
    if (newlyPressed & 0x02) {
      freeSubmenu();
      return true;
    }
    
    return false;
  }
  
  if (deltaY < -10) {
    if (gUnifiedMenuSelection > 0) {
      gUnifiedMenuSelection--;
      return true;
    }
  } else if (deltaY > 10) {
    if (gUnifiedMenuSelection < gUnifiedMenuItemCount - 1) {
      gUnifiedMenuSelection++;
      return true;
    }
  }
  
  if (newlyPressed & 0x01) {
    executeUnifiedMenuItem(gUnifiedMenuSelection);
    return true;
  }
  
  if (newlyPressed & 0x04) {
    buildUnifiedMenu();
    gUnifiedMenuSelection = 0;
    gUnifiedMenuScrollOffset = 0;
    return true;
  }
  
  return false;
}

// ==========================
// Mode Registration
// ==========================

static bool unifiedMenuAvailable(String* outReason) {
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    if (outReason) *outReason = "Not in bond mode";
    return false;
  }
  return true;
}

static const OLEDModeEntry unifiedMenuModeEntry = {
  OLED_UNIFIED_MENU,
  "Actions",
  "notify_espnow",
  displayUnifiedMenu,
  unifiedMenuAvailable,
  handleUnifiedMenuInput
};

static const OLEDModeEntry unifiedMenuModes[] = { unifiedMenuModeEntry };

REGISTER_OLED_MODE_MODULE(unifiedMenuModes, 1, "UnifiedMenu");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
