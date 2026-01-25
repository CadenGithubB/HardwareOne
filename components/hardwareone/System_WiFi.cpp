/**
 * WiFi System - WiFi management and command handlers
 * 
 * This file contains WiFi connection management, network scanning,
 * and all WiFi-related command handlers.
 */

#include "System_BuildConfig.h"

#if ENABLE_WIFI

#include "System_WiFi.h"
#include "System_Settings.h"      // For writeSettingsJson()
#include "System_Debug.h"  // For DEBUG_WIFIF and BROADCAST_PRINTF macros
#include "System_Utils.h"  // For RETURN_VALID_IF_VALIDATE_CSTR macro
#include "System_Command.h"  // For CommandModuleRegistrar
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// External dependencies from main .ino
// WifiNetwork struct and gWifiNetworks are now in wifi_system.h
extern bool wifiConnected;
extern void broadcastOutput(const String& msg);
// gDebugBuffer, ensureDebugBuffer now from debug_system.h
extern bool syncNTPAndResolve();  // Synchronous NTP sync
extern httpd_handle_t server;
extern volatile uint32_t gOutputFlags;

// Debug system globals and macros from debug_system.h (gDebugFlags, gDebugOutputQueue, DEBUG_WIFIF, etc.)

// Settings struct and gSettings are now in settings.h

// WebMirror buffer not used in wifi_system - removed to avoid incomplete type issues

// Validation macro
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// ============================================================================
// Forward Declarations for WiFi Helper Functions
// ============================================================================

// Helper functions defined later in this file
static const char* wifiStatusToString(wl_status_t status);
static void wifiEnsureIdle(unsigned long waitMs);
int findWiFiNetwork(const String& ssid);
void sortWiFiByPriority();
void upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
bool removeWiFiNetwork(const String& ssid);
void saveWiFiNetworks();
bool connectWiFiIndex(int index0based, unsigned long timeoutMs, bool showPriority = false);
bool connectWiFiSSID(const String& ssid, unsigned long timeoutMs);

// Global flags (defined later in this file)
extern volatile bool gWifiUserCancelled;
extern bool gSkipNTPInWifiConnect;

// WiFi initialization state (lazy initialization like sensor tasks)
static bool wifiInitialized = false;

// ============================================================================
// WiFi Command Handlers
// ============================================================================

const char* cmd_wifiinfo(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (WiFi.isConnected()) {
    broadcastOutput("WiFi Status:");
    snprintf(getDebugBuffer(), 1024, "  SSID: %s", WiFi.SSID().c_str());
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "  IP: %s", WiFi.localIP().toString().c_str());
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "  RSSI: %d dBm", WiFi.RSSI());
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "  MAC: %s", WiFi.macAddress().c_str());
    broadcastOutput(getDebugBuffer());
  } else {
    broadcastOutput("WiFi: Not connected");
    snprintf(getDebugBuffer(), 1024, "  Saved SSID: %s", gSettings.wifiSSID.c_str());
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "  MAC: %s", WiFi.macAddress().c_str());
    broadcastOutput(getDebugBuffer());
  }

  return "[WiFi] Status displayed";
}

const char* cmd_wifilist(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Networks already loaded from settings.json by readSettingsJson()
  if (gWifiNetworkCount == 0) {
    broadcastOutput("No saved networks.");
    return "[WiFi] Network list displayed";
  }

  broadcastOutput("Saved Networks (priority asc, numbered)");
  broadcastOutput("Use 'wificonnect <index>' to connect to a specific entry.");

  for (int i = 0; i < gWifiNetworkCount; ++i) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

    // Build the line in gDebugBuffer
    char* buf = getDebugBuffer();
    int len = snprintf(buf, 1024, "  %d. [%d] '%s'",
                       i + 1, gWifiNetworks[i].priority, gWifiNetworks[i].ssid.c_str());

    if (gWifiNetworks[i].hidden && len < 1020) {
      len += snprintf(buf + len, 1024 - len, " (hidden)");
    }

    if (i == 0 && len < 1010) {
      len += snprintf(buf + len, 1024 - len, "  <- primary");
    }

    broadcastOutput(buf);
  }

  return "[WiFi] Network list displayed";
}

const char* cmd_wifiadd(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // upsertWiFiNetwork, sortWiFiByPriority, saveWiFiNetworks are now defined in this file
  
  // wifiadd <ssid> <pass> [priority] [hidden0|1]
  String args = originalCmd;
  args.trim();
  int sp1 = args.indexOf(' ');
  if (sp1 <= 0) return "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]";
  String ssid = args.substring(0, sp1);
  String rest = args.substring(sp1 + 1);
  rest.trim();
  int sp2 = rest.indexOf(' ');
  String pass = (sp2 < 0) ? rest : rest.substring(0, sp2);
  String more = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
  more.trim();
  int pri = 0;
  bool hid = (ssid.length() == 0);
  if (more.length() > 0) {
    int sp3 = more.indexOf(' ');
    String priStr = (sp3 < 0) ? more : more.substring(0, sp3);
    pri = priStr.toInt();
    if (pri <= 0) pri = 1;
    String hidStr = (sp3 < 0) ? "" : more.substring(sp3 + 1);
    hid = (hidStr == "1" || hidStr == "true");
  }
  // Networks already in memory from settings.json
  upsertWiFiNetwork(ssid, pass, pri, hid);
  sortWiFiByPriority();
  saveWiFiNetworks();
  snprintf(getDebugBuffer(), 1024, "Saved network '%s' with priority %d%s",
           ssid.c_str(), pri == 0 ? 1 : pri, hid ? " (hidden)" : "");
  return getDebugBuffer();
}

const char* cmd_wifirm(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // removeWiFiNetwork, saveWiFiNetworks are now defined in this file
  
  String ssid = originalCmd;
  ssid.trim();
  if (ssid.length() == 0) return "Usage: wifirm <ssid>";
  // Networks already in memory from settings.json
  bool ok = removeWiFiNetwork(ssid);
  if (ok) {
    saveWiFiNetworks();
    snprintf(getDebugBuffer(), 1024, "Removed network '%s'", ssid.c_str());
    return getDebugBuffer();
  } else {
    snprintf(getDebugBuffer(), 1024, "Network not found: '%s'", ssid.c_str());
    return getDebugBuffer();
  }
}

const char* cmd_wifipromote(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // findWiFiNetwork, sortWiFiByPriority, saveWiFiNetworks are now defined in this file
  
  // wifipromote <ssid> [newPriority]
  String rest = originalCmd;
  rest.trim();
  if (rest.length() == 0) return "Usage: wifipromote <ssid> [newPriority]";
  int sp = rest.indexOf(' ');
  String ssid = (sp < 0) ? rest : rest.substring(0, sp);
  int newPri = (sp < 0) ? 1 : rest.substring(sp + 1).toInt();
  if (newPri <= 0) newPri = 1;
  // Networks already in memory from settings.json
  int idx = findWiFiNetwork(ssid);
  if (idx < 0) {
    snprintf(getDebugBuffer(), 1024, "Network not found: '%s'", ssid.c_str());
    return getDebugBuffer();
  }
  gWifiNetworks[idx].priority = newPri;
  sortWiFiByPriority();
  saveWiFiNetworks();
  snprintf(getDebugBuffer(), 1024, "Priority updated for '%s' -> %d", ssid.c_str(), newPri);
  return getDebugBuffer();
}

// Core WiFi connection logic - shared by command and boot sequence
// Returns true if connected, false otherwise
bool connectToBestWiFiNetwork() {
  if (gWifiNetworkCount == 0) {
    return false;
  }

  sortWiFiByPriority();
  gWifiUserCancelled = false;
  bool connected = false;

  for (int i = 0; i < gWifiNetworkCount && !connected; ++i) {
    connected = connectWiFiIndex(i, 20000, true);
    
    if (gWifiUserCancelled) {
      esp_wifi_disconnect();
      WiFi.disconnect();
      delay(100);
      broadcastOutput("*** WiFi connection cancelled by user ***");
      return false;
    }
  }

  if (connected && !gSkipNTPInWifiConnect) {
    syncNTPAndResolve();
  }

  return connected;
}

const char* cmd_wificonnect(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Ensure WiFi is initialized (lazy init)
  if (!ensureWiFiInitialized()) {
    return "ERROR: Failed to initialize WiFi";
  }
  
  String arg = originalCmd;
  arg.trim();
  String prevSSID = WiFi.isConnected() ? WiFi.SSID() : String("");
  bool connected = false;

  // Parse flags: --best, --index N, or legacy positional index
  bool useBest = false;
  int index1 = -1;
  if (arg.length() == 0) {
    useBest = true;  // default behavior
  } else if (arg.startsWith("--best")) {
    useBest = true;
  } else if (arg.startsWith("--index ")) {
    String n = arg.substring(8);
    n.trim();
    index1 = n.toInt();
    if (index1 <= 0 || index1 > gWifiNetworkCount) {
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Usage: wificonnect --index <1..%d>", gWifiNetworkCount);
      return getDebugBuffer();
    }
  } else {
    // Legacy: numeric positional index
    int sel = arg.toInt();
    if (sel > 0) index1 = sel;
    else {
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Usage: wificonnect [--best | --index <1..%d>]", gWifiNetworkCount);
      return getDebugBuffer();
    }
  }

  if (useBest) {
    // Use shared connection logic
    connected = connectToBestWiFiNetwork();
  } else if (index1 > 0) {
    connected = connectWiFiIndex(index1 - 1, 20000);
    if (!connected && prevSSID.length() > 0) {
      connectWiFiSSID(prevSSID, 15000);
    }
  }

  if (connected) {
    return "[WiFi] Connected successfully";
  }

  broadcastOutput("Failed to connect");
  if (prevSSID.length() > 0) {
    broadcastOutput("Attempted rollback to previous connection");
  }
  broadcastOutput("Check 'wifiinfo' for status");
  return "ERROR";
}

const char* cmd_wifidisconnect(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Stop HTTP server to free heap
  if (server != NULL) {
    httpd_stop(server);
    server = NULL;
  }
  // Disable web output flag
  gOutputFlags &= ~OUTPUT_WEB;
  gSettings.outWeb = false;
  writeSettingsJson();
  // Note: Web mirror buffer clearing removed - handled by debug_system.cpp
  WiFi.disconnect();
  return "WiFi disconnected. HTTP server stopped and web output disabled to free heap.";
}

const char* cmd_wifiscan(const String& command) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Ensure WiFi is initialized (lazy init) before scanning
  if (!ensureWiFiInitialized()) {
    return "ERROR: Failed to initialize WiFi";
  }
  
  String args = command;
  args.trim();
  bool json = (args == "json");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n < 0) return "WiFi scan failed";

  if (json) {
    // Build JSON array and return it directly (for web API consumption)
    static String jsonResult;
    jsonResult = "[";
    for (int i = 0; i < n; ++i) {
      if (i > 0) jsonResult += ",";
      char entry[256];
      snprintf(entry, sizeof(entry), "{\"ssid\":\"%s\",\"rssi\":%ld,\"bssid\":\"%s\",\"channel\":%d,\"auth\":\"%d\"}",
               WiFi.SSID(i).c_str(), (long)WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(),
               WiFi.channel(i), (int)WiFi.encryptionType(i));
      jsonResult += entry;
    }
    jsonResult += "]";
    return jsonResult.c_str();
  } else {
    snprintf(getDebugBuffer(), 1024, "%d networks found:", n);
    broadcastOutput(getDebugBuffer());
    for (int i = 0; i < n; ++i) {
      snprintf(getDebugBuffer(), 1024, "  %d) '%s'  RSSI=%ld  BSSID=%s",
               i + 1, WiFi.SSID(i).c_str(), (long)WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str());
      broadcastOutput(getDebugBuffer());
    }
  }

  snprintf(getDebugBuffer(), 1024, "Scan complete: %d networks found", n);
  return getDebugBuffer();
}

const char* cmd_wifitxpower(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: wifitxpower <dBm>";
  float dBm = valStr.toFloat();
  int q = (int)roundf(dBm * 4.0f);
  if (q < 8) q = 8;    // ~2 dBm
  if (q > 84) q = 84;  // ~21 dBm (hardware dependent)
  esp_err_t err = esp_wifi_set_max_tx_power((int8_t)q);
  if (err != ESP_OK) {
    snprintf(getDebugBuffer(), 1024, "Failed to set tx power: %d", err);
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "TX power set to %.2f dBm (raw=%d)", q / 4.0f, q);
  return getDebugBuffer();
}

const char* cmd_wifigettxpower(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  int8_t q = 0;
  esp_err_t err = esp_wifi_get_max_tx_power(&q);
  if (err != ESP_OK) {
    snprintf(getDebugBuffer(), 1024, "Failed to get tx power: %d", err);
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "TX power: %.2f dBm (raw=%d)", q / 4.0f, (int)q);
  return getDebugBuffer();
}

const char* cmd_wifiautoreconnect(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = args;
  valStr.trim();
  int v = valStr.toInt();
  gSettings.wifiAutoReconnect = (v != 0);
  writeSettingsJson();
  return gSettings.wifiAutoReconnect ? "wifiAutoReconnect set to 1" : "wifiAutoReconnect set to 0";
}

// ============================================================================
// WiFi Helper Functions
// ============================================================================

// External dependencies (already declared at top of file)
extern bool filesystemReady;

// Global flags (exported for use in .ino and commands)
// Declarations are at the top of the file, definitions here
volatile bool gWifiUserCancelled = false;
bool gSkipNTPInWifiConnect = false;

// Helper to decode WiFi status codes for debugging
static const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// Ensure STA is not mid-connection before starting a new one
static void wifiEnsureIdle(unsigned long waitMs) {
  // Stop any ongoing connection attempt
  wl_status_t beforeStatus = WiFi.status();
  DEBUG_WIFIF("[wifiEnsureIdle] Status before disconnect: %s (%d)",
              wifiStatusToString(beforeStatus), beforeStatus);

  esp_wifi_disconnect();
  unsigned long start = millis();
  // Give the driver a moment to settle
  while (millis() - start < waitMs) {
    // Consider idle if not connected; Arduino core does not expose a 'connecting' state distinctly
    if (WiFi.status() != WL_CONNECTED) break;
    delay(20);
  }

  wl_status_t afterStatus = WiFi.status();
  DEBUG_WIFIF("[wifiEnsureIdle] Status after disconnect: %s (%d), elapsed=%lums",
              wifiStatusToString(afterStatus), afterStatus, millis() - start);
}

int findWiFiNetwork(const String& ssid) {
  for (int i = 0; i < gWifiNetworkCount; ++i)
    if (gWifiNetworks[i].ssid == ssid) return i;
  return -1;
}

void sortWiFiByPriority() {
  // Simple selection sort by ascending priority (1 is highest)
  for (int i = 0; i < gWifiNetworkCount; ++i) {
    int minIdx = i;
    for (int j = i + 1; j < gWifiNetworkCount; ++j) {
      if (gWifiNetworks[j].priority < gWifiNetworks[minIdx].priority) minIdx = j;
    }
    if (minIdx != i) {
      WifiNetwork tmp = gWifiNetworks[i];
      gWifiNetworks[i] = gWifiNetworks[minIdx];
      gWifiNetworks[minIdx] = tmp;
    }
  }
}

void upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden) {
  int idx = findWiFiNetwork(ssid);
  if (idx >= 0) {
    gWifiNetworks[idx].password = password;
    if (priority > 0) gWifiNetworks[idx].priority = priority;
    gWifiNetworks[idx].hidden = hidden;
    return;
  }
  if (gWifiNetworkCount >= MAX_WIFI_NETWORKS) {
    broadcastOutput("[WiFi] Network list full; cannot add");
    return;
  }
  WifiNetwork nw;
  nw.ssid = ssid;
  nw.password = password;
  nw.priority = (priority > 0) ? priority : 1;
  nw.hidden = hidden;
  nw.lastConnected = 0;
  gWifiNetworks[gWifiNetworkCount++] = nw;
}

bool removeWiFiNetwork(const String& ssid) {
  int idx = findWiFiNetwork(ssid);
  if (idx < 0) return false;
  for (int i = idx + 1; i < gWifiNetworkCount; ++i) gWifiNetworks[i - 1] = gWifiNetworks[i];
  gWifiNetworkCount--;
  return true;
}

static void normalizeWiFiPriorities() {
  // Ensure priorities are unique and >=1, compacted ascending by sort order
  sortWiFiByPriority();
  int nextPri = 1;
  for (int i = 0; i < gWifiNetworkCount; ++i) {
    gWifiNetworks[i].priority = nextPri++;
  }
}

void saveWiFiNetworks() {
  if (!filesystemReady) return;
  sortWiFiByPriority();
  normalizeWiFiPriorities();
  // Persist via unified settings.json only
  writeSettingsJson();
}

// Connect to a saved network by index (0-based). Update lastConnected on success.
// If showPriority is true, displays priority in connection message (for --best mode)
// Returns: true if connected, false if failed or user cancelled
bool connectWiFiIndex(int index0based, unsigned long timeoutMs, bool showPriority) {
  if (index0based < 0 || index0based >= gWifiNetworkCount) {
    DEBUG_WIFIF("[connectWiFiIndex] Invalid index: %d (count=%d)", index0based, gWifiNetworkCount);
    return false;
  }
  const WifiNetwork& nw = gWifiNetworks[index0based];
  DEBUG_WIFIF("[connectWiFiIndex] Attempting connection to [%d] SSID='%s'", index0based + 1, nw.ssid.c_str());

  // Display connection message with or without priority
  if (showPriority) {
    BROADCAST_PRINTF("Connecting to '%s' (priority %d) ...", nw.ssid.c_str(), nw.priority);
  } else {
    BROADCAST_PRINTF("Connecting to [%d] '%s'...", index0based + 1, nw.ssid.c_str());
  }

  // Ensure previous attempts are not in-progress
  DEBUG_WIFIF("[connectWiFiIndex] Ensuring WiFi idle state");
  wifiEnsureIdle(200);

  // Debug: Show SSID and password details
  DEBUG_WIFIF("[connectWiFiIndex] SSID: '%s' (length=%d)", nw.ssid.c_str(), nw.ssid.length());
  DEBUG_WIFIF("[connectWiFiIndex] Password length: %d", nw.password.length());
  // DEBUG_WIFIF("[connectWiFiIndex] Password (VISIBLE): '%s'", nw.password.c_str());  // SECURITY: Uncomment only for troubleshooting

  // Validate credentials are not empty
  if (nw.ssid.length() == 0) {
    ERROR_WIFIF("SSID is empty!");
    broadcastOutput("ERROR: SSID is empty");
    return false;
  }
  if (nw.password.length() == 0) {
    DEBUG_WIFIF("[connectWiFiIndex] WARNING: Password is empty (open network?)");
  }

  // Check if password is encrypted and decrypt it
  String actualPassword = nw.password;
  if (nw.password.startsWith("ENC:")) {
    DEBUG_WIFIF("[connectWiFiIndex] Password is encrypted, decrypting...");
    actualPassword = decryptWifiPassword(nw.password);
    DEBUG_WIFIF("[connectWiFiIndex] Decrypted password length: %d", actualPassword.length());
  }

  // CRITICAL: Use ESP-IDF WiFi API directly to bypass Arduino WiFi library bug
  // The Arduino WiFi.begin() sometimes fails to start the connection state machine
  // Solution: Use esp_wifi_* functions directly for reliable connection
  DEBUG_WIFIF("[connectWiFiIndex] Current WiFi mode before reset: %d", WiFi.getMode());
  DEBUG_WIFIF("[connectWiFiIndex] Using ESP-IDF WiFi API for connection...");

  // Step 1: Stop any existing connection
  esp_err_t err = esp_wifi_disconnect();
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_disconnect() returned: %d", err);
  delay(100);

  // Step 2: Stop WiFi completely
  err = esp_wifi_stop();
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_stop() returned: %d", err);
  delay(200);  // Longer delay for full stop

  // Step 3: Set WiFi mode to STA
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_set_mode(STA) returned: %d", err);

  // Step 4: Start WiFi
  err = esp_wifi_start();
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_start() returned: %d", err);
  delay(300);  // Longer delay for WiFi radio to initialize

  // Step 5: Configure WiFi credentials using ESP-IDF
  wifi_config_t wifi_config = {};
  memset(&wifi_config, 0, sizeof(wifi_config_t));

  // Copy SSID
  size_t ssid_len = min((size_t)nw.ssid.length(), sizeof(wifi_config.sta.ssid) - 1);
  memcpy(wifi_config.sta.ssid, nw.ssid.c_str(), ssid_len);
  wifi_config.sta.ssid[ssid_len] = '\0';

  // Copy password (use decrypted version)
  size_t pass_len = min((size_t)actualPassword.length(), sizeof(wifi_config.sta.password) - 1);
  memcpy(wifi_config.sta.password, actualPassword.c_str(), pass_len);
  wifi_config.sta.password[pass_len] = '\0';

  // Try WPA_WPA2_PSK (auto-detect) instead of forcing WPA2
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  // CRITICAL: Set scan method to FAST to reduce connection time and improve reliability
  // ALL_CHANNEL can take too long and cause timeouts
  wifi_config.sta.scan_method = WIFI_FAST_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;  // Connect to strongest signal

  DEBUG_WIFIF("[connectWiFiIndex] Configuring WiFi:");
  DEBUG_WIFIF("[connectWiFiIndex]   SSID: '%s' (len=%d)", wifi_config.sta.ssid, ssid_len);
  DEBUG_WIFIF("[connectWiFiIndex]   Password: '%s' (len=%d)", wifi_config.sta.password, pass_len);
  DEBUG_WIFIF("[connectWiFiIndex]   Auth mode: WPA_WPA2_PSK");
  DEBUG_WIFIF("[connectWiFiIndex]   Scan method: FAST, Sort: BY_SIGNAL");

  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_set_config() returned: %d", err);

  // Step 6: Connect
  DEBUG_WIFIF("[connectWiFiIndex] Calling esp_wifi_connect()...");
  err = esp_wifi_connect();
  DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_connect() returned: %d (%s)", err,
              err == ESP_OK ? "ESP_OK" : "ERROR");

  // CRITICAL: Give the driver MORE time to scan channels and find the AP
  // The ESP32 needs time to scan for the SSID beacon
  delay(500);  // Increased from 100ms to 500ms
  DEBUG_WIFIF("[connectWiFiIndex] WiFi status 500ms after connect(): %s (%d)",
              wifiStatusToString(WiFi.status()), WiFi.status());

  unsigned long start = millis();
  int statusCheckCount = 0;
  wl_status_t lastStatus = WiFi.status();
  DEBUG_WIFIF("[connectWiFiIndex] Initial WiFi status after begin(): %s (%d)",
              wifiStatusToString(lastStatus), lastStatus);

  // Track if we hit the IDLE bug (status STAYS in IDLE for too long)
  bool hitIdleBug = false;
  unsigned long firstIdleTime = 0;  // When we first saw IDLE status

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
    statusCheckCount++;

    wl_status_t currentStatus = WiFi.status();

    // Track IDLE status timing
    if (currentStatus == WL_IDLE_STATUS) {
      if (firstIdleTime == 0) {
        firstIdleTime = millis();
        DEBUG_WIFIF("[connectWiFiIndex] First IDLE status seen at %lums", millis() - start);
      }
    } else {
      // If we leave IDLE, reset the timer (this is normal during scanning)
      firstIdleTime = 0;
    }

    // CRITICAL: Detect ESP32 WiFi IDLE bug
    // Only trigger if status STAYS in IDLE for 3+ seconds (not just transitions through it)
    if (currentStatus == WL_IDLE_STATUS && firstIdleTime > 0 && (millis() - firstIdleTime) > 3000 && !hitIdleBug) {
      hitIdleBug = true;
      DEBUG_WIFIF("[connectWiFiIndex] ⚠ ESP32 IDLE BUG DETECTED - stuck in IDLE for %lums",
                  millis() - firstIdleTime);

      // NUCLEAR OPTION: Complete WiFi deinit/reinit
      DEBUG_WIFIF("[connectWiFiIndex] Doing COMPLETE WiFi deinit/reinit...");

      esp_wifi_disconnect();
      delay(50);
      esp_wifi_stop();
      delay(50);
      esp_wifi_deinit();  // Complete deinit
      delay(500);         // Wait for full cleanup

      DEBUG_WIFIF("[connectWiFiIndex] Re-initializing WiFi subsystem...");
      wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
      err = esp_wifi_init(&cfg);
      DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_init() returned: %d", err);

      err = esp_wifi_set_mode(WIFI_MODE_STA);
      DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_set_mode(STA) returned: %d", err);

      err = esp_wifi_start();
      DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_start() returned: %d", err);
      delay(200);

      // Reconfigure credentials
      err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
      DEBUG_WIFIF("[connectWiFiIndex] esp_wifi_set_config() returned: %d", err);

      // Try connecting again
      err = esp_wifi_connect();
      DEBUG_WIFIF("[connectWiFiIndex] Retry esp_wifi_connect() returned: %d", err);
      delay(500);

      // Reset the timer so we get full timeout for the retry
      start = millis();
      statusCheckCount = 0;
    }

    // Log every 2 seconds OR when status changes
    if (statusCheckCount % 10 == 0 || currentStatus != lastStatus) {
      DEBUG_WIFIF("[connectWiFiIndex] Status: %s (%d), elapsed=%lums",
                  wifiStatusToString(currentStatus), currentStatus, millis() - start);
      lastStatus = currentStatus;
    }
    // Check for user escape input during WiFi connection
    if (Serial.available()) {
      while (Serial.available()) Serial.read();  // consume all pending input
      DEBUG_WIFIF("[connectWiFiIndex] Connection cancelled by user");
      gWifiUserCancelled = true;  // Set global flag for multi-attempt cancellation
      broadcastOutput("*** WiFi connection cancelled by user ***");
      return false;  // connection cancelled
    }
    // progress dots omitted from unified output to reduce noise
  }

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_WIFIF("[connectWiFiIndex] SUCCESS! Connected to '%s', IP=%s",
                nw.ssid.c_str(), WiFi.localIP().toString().c_str());
    BROADCAST_PRINTF("WiFi connected: %s", WiFi.localIP().toString().c_str());
    gWifiNetworks[index0based].lastConnected = millis();
    saveWiFiNetworks();
    return true;
  }

  wl_status_t finalStatus = WiFi.status();
  WARN_WIFIF("Connection FAILED after %lums, final status=%s (%d)",
             millis() - start, wifiStatusToString(finalStatus), finalStatus);

  // Additional diagnostics
  DEBUG_WIFIF("[connectWiFiIndex] WiFi diagnostics: RSSI=%ld, Channel=%ld",
              (long)WiFi.RSSI(), (long)WiFi.channel());

  // Check WiFi driver state
  DEBUG_WIFIF("[connectWiFiIndex] WiFi mode at failure: %d", WiFi.getMode());
  DEBUG_WIFIF("[connectWiFiIndex] WiFi isConnected(): %s", WiFi.isConnected() ? "true" : "false");
  DEBUG_WIFIF("[connectWiFiIndex] WiFi SSID(): '%s'", WiFi.SSID().c_str());

  // Display failure message with WiFi status
  if (showPriority) {
    BROADCAST_PRINTF("Failed connecting to '%s' - WiFi status: %d", nw.ssid.c_str(), WiFi.status());
  } else {
    broadcastOutput("Connection failed.");
  }
  return false;
}

// Connect to a saved network by SSID.
bool connectWiFiSSID(const String& ssid, unsigned long timeoutMs) {
  int idx = findWiFiNetwork(ssid);
  if (idx < 0) return false;
  return connectWiFiIndex(idx, timeoutMs, false);
}

// ============================================================================
// HTTP Server and NTP Commands
// ============================================================================

const char* cmd_ntpsync(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Allow any authenticated user to sync NTP
  bool ok = syncNTPAndResolve();
  return ok ? "NTP sync complete" : "NTP sync failed";
}

const char* cmd_httpstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!WiFi.isConnected()) {
    return "ERROR: WiFi not connected. Connect to WiFi before starting HTTP server.";
  }

  if (server != NULL) {
    return "HTTP server is already running";
  }

  extern void startHttpServer();
  startHttpServer();
  
  // Check if server started successfully
  if (server != NULL) {
    return "HTTP server started";
  }
  return "ERROR: Failed to start HTTP server";
}

const char* cmd_httpstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (server == NULL) {
    return "HTTP server is not running";
  }

  httpd_stop(server);
  server = NULL;
  
  // Disable web output when server stops
  extern volatile uint32_t gOutputFlags;
  gOutputFlags &= ~OUTPUT_WEB;
  
  return "[HTTP] Server stopped successfully";
}

const char* cmd_httpstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (server != NULL) {
    return "HTTP server: RUNNING";
  } else {
    return "HTTP server: STOPPED";
  }
}

// ============================================================================
// WiFi Command Registry
// ============================================================================

const CommandEntry wifiCommands[] = {
  // Network Management
  { "wifiinfo", "Show current WiFi connection info.", false, cmd_wifiinfo },
  { "wifilist", "List saved WiFi networks.", false, cmd_wifilist },
  { "wifiadd", "Add/overwrite a WiFi network.", true, cmd_wifiadd, "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]" },
  { "wifirm", "Remove a WiFi network.", true, cmd_wifirm, "Usage: wifirm <ssid>" },
  { "wifipromote", "Promote a WiFi network to top priority.", true, cmd_wifipromote, "Usage: wifipromote <ssid>" },
  
  // Connection Control
  { "wificonnect", "Connect to WiFi (auto-select or specify SSID).", false, cmd_wificonnect, "Usage: wificonnect [ssid]" },
  { "wifidisconnect", "Disconnect from WiFi.", false, cmd_wifidisconnect },
  { "wifiscan", "Scan for available WiFi networks.", false, cmd_wifiscan },
  { "wifigettxpower", "Get WiFi TX power.", false, cmd_wifitxpower },
  
  // Network Services
  { "ntpsync", "Sync time with NTP server.", false, cmd_ntpsync },
  { "httpstart", "Start HTTP server.", false, cmd_httpstart },
  { "httpstop", "Stop HTTP server.", false, cmd_httpstop },
  { "httpstatus", "Show HTTP server status.", false, cmd_httpstatus }
};

const size_t wifiCommandsCount = sizeof(wifiCommands) / sizeof(wifiCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _wifi_registrar(wifiCommands, wifiCommandsCount, "wifi");

// ============================================================================
// WiFi Lazy Initialization (similar to sensor task pattern)
// ============================================================================

// Ensure WiFi is initialized (lazy initialization to save ~32KB at boot)
bool ensureWiFiInitialized() {
  if (wifiInitialized) {
    DEBUG_WIFIF("[WiFi] Already initialized");
    return true;
  }

  DEBUG_WIFIF("[WiFi] Initializing WiFi subsystem (lazy init)");
  WiFi.mode(WIFI_STA);
  DEBUG_WIFIF("[WiFi] Mode set to WIFI_STA");
  wifiInitialized = true;
  
  broadcastOutput("WiFi subsystem initialized");
  return true;
}

// ============================================================================
// WiFi Setup (moved from .ino)
// ============================================================================

void setupWiFi() {
  // Ensure WiFi is initialized before attempting connection
  if (!ensureWiFiInitialized()) {
    broadcastOutput("ERROR: Failed to initialize WiFi");
    return;
  }
  
  DEBUG_WIFIF("[WiFi Setup] Starting WiFi connection");

  // Inform user they can escape WiFi connection attempts
  broadcastOutput("Starting WiFi connection... (Press any key in Serial Monitor to skip)");

  // WiFi networks already loaded by readSettingsJson() during boot
  DEBUG_WIFIF("[WiFi Setup] Using WiFi networks loaded from settings");
  DEBUG_WIFIF("[WiFi Setup] Found %d saved networks", gWifiNetworkCount);
  
  char debugBuf[128];
  snprintf(debugBuf, sizeof(debugBuf), "DEBUG: Found %d saved networks", gWifiNetworkCount);
  broadcastOutput(debugBuf);

  if (gWifiNetworkCount > 0) {
    snprintf(debugBuf, sizeof(debugBuf), "DEBUG: First network SSID: '%s'", gWifiNetworks[0].ssid.c_str());
    broadcastOutput(debugBuf);
    DEBUG_WIFIF("[WiFi Setup] First network: SSID='%s', priority=%d",
                gWifiNetworks[0].ssid.c_str(), gWifiNetworks[0].priority);
  }

  bool connected = false;
  if (gWifiNetworkCount > 0) {
    broadcastOutput("Attempting WiFi connection directly...");
    DEBUG_WIFIF("[WiFi Setup] Calling connectToBestWiFiNetwork() directly");
    DEBUG_WIFIF("[DEBUG] About to call connectToBestWiFiNetwork");
    // Call WiFi connection function directly instead of going through command system
    // This avoids potential deadlock with command executor task during boot
    connected = connectToBestWiFiNetwork();
    DEBUG_WIFIF("[DEBUG] connectToBestWiFiNetwork returned");
    DEBUG_WIFIF("[WiFi Setup] Connection result: %s (status=%d)",
                connected ? "SUCCESS" : "FAILED", WiFi.status());
  } else {
    DEBUG_WIFIF("[WiFi Setup] No saved networks found - skipping connection");
  }

  if (!connected) {
    broadcastOutput("WiFi connect timed out; continuing without network");
    DEBUG_WIFIF("[WiFi Setup] Final WiFi status: %d", WiFi.status());
  } else {
    DEBUG_WIFIF("[WiFi Setup] Connected to: %s, IP: %s",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  }
}

// ============================================================================
// WiFi Settings Module
// ============================================================================

static const SettingEntry wifiSettingsEntries[] = {
  { "wifiSSID",           SETTING_STRING, &gSettings.wifiSSID,          0, 0, "", 0, 0, "WiFi SSID", nullptr },
  { "wifiPassword",       SETTING_STRING, &gSettings.wifiPassword,      0, 0, "", 0, 0, "WiFi Password", nullptr },
  { "wifiAutoReconnect",  SETTING_BOOL,   &gSettings.wifiAutoReconnect, true, 0, nullptr, 0, 1, "Auto-reconnect", nullptr },
  { "wifiNtpServer",      SETTING_STRING, &gSettings.ntpServer,         0, 0, "pool.ntp.org", 0, 0, "NTP Server", nullptr },
  { "wifiTzOffsetMinutes",SETTING_INT,    &gSettings.tzOffsetMinutes,   -240, 0, nullptr, -720, 840, "Timezone Offset (min)", nullptr }
};

extern const SettingsModule wifiSettingsModule = {
  "wifi",
  "wifi",
  wifiSettingsEntries,
  sizeof(wifiSettingsEntries) / sizeof(wifiSettingsEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// HTTP Settings Module
// ============================================================================

static const SettingEntry httpSettingsEntries[] = {
  { "httpAutoStart", SETTING_BOOL, &gSettings.httpAutoStart, true, 0, nullptr, 0, 1, "Auto-start at boot", nullptr }
};

extern const SettingsModule httpSettingsModule = {
  "http",
  "http",
  httpSettingsEntries,
  sizeof(httpSettingsEntries) / sizeof(httpSettingsEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif // ENABLE_WIFI
