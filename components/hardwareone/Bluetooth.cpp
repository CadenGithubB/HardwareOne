/**
 * Bluetooth System - ESP32 Built-in BLE Server Implementation
 * 
 * Provides BLE connectivity for smart glasses and external devices.
 * Uses ESP32 Bluedroid stack (built-in) for better compatibility.
 * 
 * Features:
 * - GATT server with custom services
 * - Command service (send commands, receive responses)
 * - Sensor data notifications (push model)
 */

#include "Bluetooth.h"
#include "G2_Glasses.h"  // Header provides stubs when ENABLE_G2_GLASSES=0, so blemode CLI compiles either way
#include "System_Utils.h"
#include "BLE_Peers.h"        // peer registry + cmd_bleautoconnect / cmd_blepeers

#if ENABLE_BLUETOOTH

#include "OLED_Display.h"
#include "OLED_SettingsEditor.h"
#include "OLED_Utils.h"
#include "System_BuildConfig.h"
#include "System_User.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"

#include <esp_gatts_api.h>
#include <stdlib.h>
#include <string.h>

// Memory allocation

// Debug macros
#define BLE_DEBUGF(flag, fmt, ...) \
  do { \
    if (isDebugFlagSet(flag) && ensureDebugBuffer()) { \
      snprintf(getDebugBuffer(), 1024, "[BLE] " fmt, ##__VA_ARGS__); \
      broadcastOutput(getDebugBuffer()); \
    } \
  } while(0)

// =============================================================================
// GLOBAL STATE
// =============================================================================

BLESystemState* gBLEState = nullptr;

// BLE toggle tracking - ESP32 Bluedroid leaks ~10KB DRAM per init/deinit cycle
static int sBLEToggleCount = 0;
static size_t sBLEHeapBeforeInit = 0;

// Cached normalized MAC addresses (uppercase, no heap allocations on lookup)
static struct {
  char leftMAC[18];
  char rightMAC[18];
  char ringMAC[18];
  char phoneMAC[18];
  bool initialized;
} gBLEMACCache = {{0}, {0}, {0}, {0}, false};

// ESP32 BLE objects
static BLEServer* pServer = nullptr;
static BLEAdvertising* pAdvertising = nullptr;

// Services
static BLEService* pCommandService = nullptr;
static BLEService* pDataService = nullptr;
static BLEService* pDeviceInfoService = nullptr;

// Command service characteristics
static BLECharacteristic* pCmdRequestChar = nullptr;
static BLECharacteristic* pCmdResponseChar = nullptr;
static BLECharacteristic* pCmdStatusChar = nullptr;

// Data streaming service characteristics (non-static for access from streaming module)
BLECharacteristic* pSensorDataChar = nullptr;
BLECharacteristic* pSystemStatusChar = nullptr;
BLECharacteristic* pEventNotifyChar = nullptr;
static BLECharacteristic* pStreamControlChar = nullptr;

// Device info characteristics
static BLECharacteristic* pManufacturerChar = nullptr;
static BLECharacteristic* pModelChar = nullptr;
static BLECharacteristic* pFirmwareChar = nullptr;

// Forward declarations
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len);

static int findConnectionSlotByConnId(uint16_t connId) {
  if (!gBLEState) return -1;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBLEState->connections[i].active && gBLEState->connections[i].connId == connId) {
      return i;
    }
  }
  return -1;
}

static const char* kBleIpTag = "ble";
static const uint32_t kBleSessionIdleTimeoutMs = 15UL * 60UL * 1000UL;  // 15 minutes

static void bleMarkActivity(uint16_t connId) {
  int slot = findConnectionSlotByConnId(connId);
  if (slot >= 0) {
    gBLEState->connections[slot].lastActivityMs = millis();
    BLE_DEBUGF(DEBUG_BLE_DATA, "Activity heartbeat conn=%u slot=%d", (unsigned)connId, slot);
  }
}

void bleClearConnectionByConnId(uint16_t connId) {
  int slot = findConnectionSlotByConnId(connId);
  if (slot < 0) return;

  gBLEState->connections[slot].active = false;
  gBLEState->connections[slot].connId = 0;
  gBLEState->connections[slot].connectedSince = 0;
  gBLEState->connections[slot].deviceName = "";
  memset(gBLEState->connections[slot].deviceAddr, 0, sizeof(gBLEState->connections[slot].deviceAddr));
  gBLEState->connections[slot].deviceType = BLE_DEVICE_UNKNOWN;
  gBLEState->connections[slot].commandsReceived = 0;
  gBLEState->connections[slot].authed = false;
  gBLEState->connections[slot].user = "";
  gBLEState->connections[slot].lastActivityMs = 0;
  BLE_DEBUGF(DEBUG_BLE_CORE, "Cleared connection slot=%d for conn=%u", slot, (unsigned)connId);
}

static bool bleIsAuthed(uint16_t connId, String& outUser) {
  int slot = findConnectionSlotByConnId(connId);
  if (slot < 0) return false;
  if (!gBLEState->connections[slot].authed) return false;
  outUser = gBLEState->connections[slot].user;
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session authed lookup conn=%u user='%s'", (unsigned)connId, outUser.c_str());
  return outUser.length() > 0;
}

static void bleLogout(uint16_t connId) {
  int slot = findConnectionSlotByConnId(connId);
  if (slot < 0) return;
  gBLEState->connections[slot].authed = false;
  gBLEState->connections[slot].user = "";
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session logout conn=%u slot=%d", (unsigned)connId, slot);
}

bool bleHasAuthenticatedSession() {
  if (!gBLEState) return false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    if (gBLEState->connections[i].user.length() == 0) continue;
    return true;
  }
  return false;
}

bool bleGetAuthenticatedSessionInfo(int authedIndex, uint16_t& outConnId, String& outUser) {
  if (!gBLEState || authedIndex < 0) return false;
  int seen = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    if (gBLEState->connections[i].user.length() == 0) continue;
    if (seen == authedIndex) {
      outConnId = gBLEState->connections[i].connId;
      outUser = gBLEState->connections[i].user;
      return true;
    }
    seen++;
  }
  return false;
}

int bleRevokeUserSessions(const String& username) {
  if (!gBLEState || username.length() == 0) return 0;
  int revoked = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    if (!gBLEState->connections[i].user.equalsIgnoreCase(username)) continue;
    gBLEState->connections[i].authed = false;
    gBLEState->connections[i].user = "";
    revoked++;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked %d BLE sessions for user '%s'", revoked, username.c_str());
  return revoked;
}

int bleRevokeAllSessions() {
  if (!gBLEState) return 0;
  int revoked = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    gBLEState->connections[i].authed = false;
    gBLEState->connections[i].user = "";
    revoked++;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked all BLE sessions (%d total)", revoked);
  return revoked;
}

// Bind an already-validated user to this BLE connection (password verified elsewhere).
static bool bleBindSession(uint16_t connId, const String& user) {
  int slot = findConnectionSlotByConnId(connId);
  if (slot < 0 || !gBLEState) return false;
  gBLEState->connections[slot].authed = true;
  gBLEState->connections[slot].user = user;
  gBLEState->connections[slot].lastActivityMs = millis();
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session bound conn=%u slot=%d user='%s'", (unsigned)connId, slot, user.c_str());
  return true;
}

static void bleSendAuthRequired(uint16_t connId) {
  static const char* msg = "Authentication required. Use: login <username> <password>";
  BLE_DEBUGF(DEBUG_BLE_CORE, "Auth required notice conn=%u", (unsigned)connId);
  sendBLEResponseToConn(connId, msg, strlen(msg));
}

// =============================================================================
// DEVICE TYPE IDENTIFICATION
// =============================================================================

// Convert MAC address to stack buffer (zero heap allocations)
static void macToStackBuf(const uint8_t* mac, char* buf) {
  snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Update MAC address cache from settings (call when settings change).
// Source-of-truth is now gBlePeerData[] (see BLE_Peers.h); the cache
// keeps an upper-cased fast-path copy for hot identification paths
// that don't want to allocate or call toUpperCase on every check.
static void copyMacUpper(char* dst, size_t dstCap, const String& src) {
  if (src.length() > 0) {
    strncpy(dst, src.c_str(), dstCap - 1);
    dst[dstCap - 1] = '\0';
    for (char* p = dst; *p; p++) *p = toupper(*p);
  } else {
    dst[0] = '\0';
  }
}
static void bleUpdateMACCache() {
  copyMacUpper(gBLEMACCache.leftMAC,  sizeof(gBLEMACCache.leftMAC),
               gBlePeerData[BLE_PEER_G2_GLASSES].mac1);
  copyMacUpper(gBLEMACCache.rightMAC, sizeof(gBLEMACCache.rightMAC),
               gBlePeerData[BLE_PEER_G2_GLASSES].mac2);
  copyMacUpper(gBLEMACCache.ringMAC,  sizeof(gBLEMACCache.ringMAC),
               gBlePeerData[BLE_PEER_R1_RING].mac1);
  copyMacUpper(gBLEMACCache.phoneMAC, sizeof(gBLEMACCache.phoneMAC),
               gBlePeerData[BLE_PEER_PHONE].mac1);


  gBLEMACCache.initialized = true;
}

// Convert device type to human-readable string
const char* bleDeviceTypeToString(BLEDeviceType type) {
  switch (type) {
    case BLE_DEVICE_GLASSES_LEFT: return "Glasses (Left)";
    case BLE_DEVICE_GLASSES_RIGHT: return "Glasses (Right)";
    case BLE_DEVICE_RING: return "Smart Ring";
    case BLE_DEVICE_PHONE: return "Phone";
    case BLE_DEVICE_CUSTOM: return "Custom Device";
    default: return "Unknown";
  }
}

// Identify device type by MAC address (zero heap allocations)
BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t* mac) {
  // Ensure cache is initialized
  if (!gBLEMACCache.initialized) {
    bleUpdateMACCache();
  }
  
  // Convert incoming MAC to uppercase string on stack
  char macStr[18];
  macToStackBuf(mac, macStr);
  
  // Check against cached normalized MACs
  if (gBLEMACCache.leftMAC[0] != '\0' && strcmp(macStr, gBLEMACCache.leftMAC) == 0) {
    return BLE_DEVICE_GLASSES_LEFT;
  }
  if (gBLEMACCache.rightMAC[0] != '\0' && strcmp(macStr, gBLEMACCache.rightMAC) == 0) {
    return BLE_DEVICE_GLASSES_RIGHT;
  }
  if (gBLEMACCache.ringMAC[0] != '\0' && strcmp(macStr, gBLEMACCache.ringMAC) == 0) {
    return BLE_DEVICE_RING;
  }
  if (gBLEMACCache.phoneMAC[0] != '\0' && strcmp(macStr, gBLEMACCache.phoneMAC) == 0) {
    return BLE_DEVICE_PHONE;
  }
  
  return BLE_DEVICE_UNKNOWN;
}

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    // NOTE: This callback runs on BTC_TASK with limited stack - avoid heavy operations
    // Use deferred flag pattern for logging (ISR-safe)
    if (!gBLEState) return;
    
    // Find free connection slot
    int slot = -1;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (!gBLEState->connections[i].active) {
        slot = i;
        break;
      }
    }
    
    if (slot == -1) {
      // No logging here - callback context
      return;
    }
    
    // Store connection info (minimal state updates - ISR-safe)
    gBLEState->connections[slot].active = true;
    gBLEState->connections[slot].connId = param->connect.conn_id;
    gBLEState->connections[slot].connectedSince = millis();
    memcpy(gBLEState->connections[slot].deviceAddr, param->connect.remote_bda, 6);
    gBLEState->connections[slot].commandsReceived = 0;
    gBLEState->connections[slot].authed = false;
    gBLEState->connections[slot].user = "";
    gBLEState->connections[slot].lastActivityMs = millis();
    
    // Identify device type by MAC address (uses static lookup - ISR-safe)
    gBLEState->connections[slot].deviceType = bleIdentifyDeviceByMAC(param->connect.remote_bda);
    gBLEState->connections[slot].deviceName = bleDeviceTypeToString(gBLEState->connections[slot].deviceType);
    
    gBLEState->activeConnectionCount++;
    gBLEState->totalConnections++;
    gBLEState->connectionState = BLE_STATE_CONNECTED;
    
    // Defer logging to task context
    gBLEState->deferredConnectSlot = slot;
    gBLEState->deferredConnectPending = true;
    
    // Keep advertising if we haven't reached max connections
    if (gBLEState->activeConnectionCount >= BLE_MAX_CONNECTIONS) {
      BLEDevice::stopAdvertising();
    }

    BLE_DEBUGF(DEBUG_BLE_CORE,
               "ISR connect conn=%u slot=%d type=%d active=%d/%d",
               (unsigned)param->connect.conn_id,
               slot,
               (int)gBLEState->connections[slot].deviceType,
               gBLEState->activeConnectionCount,
               BLE_MAX_CONNECTIONS);
  }
  
  void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    // NOTE: This callback runs on BTC_TASK with limited stack - avoid heavy operations
    // Use deferred flag pattern for logging (ISR-safe)
    if (!gBLEState) return;
    
    if (param) {
      bleClearConnectionByConnId(param->disconnect.conn_id);
    }
    
    if (gBLEState->activeConnectionCount > 0) {
      gBLEState->activeConnectionCount--;
    }
    
    // Defer logging to task context
    gBLEState->deferredDisconnectActiveCount = gBLEState->activeConnectionCount;
    gBLEState->deferredDisconnectPending = true;
    
    if (gBLEState->activeConnectionCount == 0) {
      gBLEState->connectionState = BLE_STATE_IDLE;
    }
    
    // Auto-restart advertising if we're below max connections
    if (gBLEState->activeConnectionCount < BLE_MAX_CONNECTIONS && gBLEState->initialized) {
      startBLEAdvertising();
    }

    BLE_DEBUGF(DEBUG_BLE_CORE,
               "ISR disconnect conn=%u remaining=%d state=%d",
               param ? (unsigned)param->disconnect.conn_id : 0,
               gBLEState->activeConnectionCount,
               (int)gBLEState->connectionState);
  }
};

// =============================================================================
// CHARACTERISTIC CALLBACKS
// =============================================================================

// Command Request Characteristic - receives commands from client
// NOTE: This callback runs on BTC_TASK with limited stack (~3KB)
// Heavy command processing is routed through the central cmd_exec task via submitCommandAsync
class CmdRequestCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) override {
    // NOTE: Callback runs on BTC_TASK - defer logging to task context (ISR-safe pattern)
    if (!param || !gBLEState) return;
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      gBLEState->commandsReceived++;
      
      // Defer logging to task context
      gBLEState->deferredCmdReceivedConnId = param->write.conn_id;
      gBLEState->deferredCmdReceivedLen = value.length();
      gBLEState->deferredCmdReceivedPending = true;

      BLE_DEBUGF(DEBUG_BLE_GATT,
                 "Command write conn=%u len=%u",
                 (unsigned)param->write.conn_id,
                 (unsigned)value.length());
      
      // Route to processIncomingBLECommand which handles lightweight ops directly
      // and routes heavy commands through cmd_exec task
      processIncomingBLECommand(param->write.conn_id, value.c_str(), value.length());
    }
  }
};

// Status Characteristic - returns connection status
class CmdStatusCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pCharacteristic) override {
    char statusBuf[128];
    snprintf(statusBuf, sizeof(statusBuf),
             "{\"state\":\"connected\",\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu}",
             millis() / 1000,
             gBLEState->commandsReceived,
             gBLEState->responsesSent);
    pCharacteristic->setValue(statusBuf);
    BLE_DEBUGF(DEBUG_BLE_GATT, "Status read: %s", statusBuf);
  }
};

// =============================================================================
// COMMAND PROCESSING
// =============================================================================

extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);
extern AuthContext gExecAuthContext;

// Command types + ExecAsyncCallback from shared header
#include "System_CommandTypes.h"

// External async command submission
extern bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData);

struct BleLoginAsyncJob {
  uint16_t connId;
  char user[64];
};

// Async callback for BLE command results - called on cmd_exec task
static void bleCommandResultCallback(bool ok, const char* result, void* userData) {
  uint16_t connId = (uint16_t)(uintptr_t)userData;
  BLE_DEBUGF(DEBUG_BLE_DATA, "Async command result: ok=%d len=%zu connId=%u", ok, strlen(result), connId);
  sendBLEResponseToConn(connId, result, strlen(result));
}

// Login runs password verification (mbedtls / PBKDF2) — must NOT run on BTC_TASK (~3KB stack).
// cmd_login + loginTransport run on cmd_exec_task; we then bind the GATT connection session here.
static void bleLoginAsyncCallback(bool cmdExecOk, const char* result, void* userData) {
  BleLoginAsyncJob* job = (BleLoginAsyncJob*)userData;
  if (!job) return;

  const char* res = result ? result : "";
  bool authOk = cmdExecOk && (strstr(res, "Login successful") != nullptr);

  if (authOk) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Login success conn=%u user='%s'", (unsigned)job->connId, job->user);
    (void)bleBindSession(job->connId, String(job->user));
    // Auto-enable BLE broadcast output on first authenticated session
    gOutputFlags |= OUTPUT_BLE;
    char out[160];
    snprintf(out, sizeof(out), "[ble] Login successful. User: %s%s", job->user,
             isAdminUser(String(job->user)) ? " (admin)" : "");
    sendBLEResponseToConn(job->connId, out, strlen(out));
  } else {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Login FAILED conn=%u result='%s'", (unsigned)job->connId, res);
    const char* msg = (res[0] != '\0') ? res : "[ble] Authentication failed.";
    sendBLEResponseToConn(job->connId, msg, strlen(msg));
  }
  free(job);
}

// Forward declaration for OLED message history
#if ENABLE_OLED_DISPLAY
// Message history for OLED display
#define BLE_MSG_HISTORY_SIZE 4
#define BLE_MSG_MAX_LEN 32

static char bleMessageHistory[BLE_MSG_HISTORY_SIZE][BLE_MSG_MAX_LEN];
static uint8_t bleMessageCount = 0;
static uint8_t bleMessageHead = 0;

void bleAddMessageToHistory(const char* msg);
#endif

static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {
  // Build printable command string (filter non-printable bytes) - stack buffer, zero heap allocations
  char cmdBuf[512];
  size_t outIdx = 0;
  
  for (size_t i = 0; i < len && outIdx < sizeof(cmdBuf) - 1; i++) {
    uint8_t b = ((const uint8_t*)data)[i];
    if (b == 0) continue;  // Skip NUL bytes
    if (b == '\r' || b == '\n' || b == '\t') {
      cmdBuf[outIdx++] = ' ';
    } else if (b >= 32 && b <= 126) {  // Printable ASCII only
      cmdBuf[outIdx++] = (char)b;
    }
  }
  cmdBuf[outIdx] = '\0';
  
  // Trim leading spaces
  char* cmdStart = cmdBuf;
  while (*cmdStart == ' ') cmdStart++;
  
  // Trim trailing spaces in-place
  while (outIdx > 0 && cmdBuf[outIdx - 1] == ' ') cmdBuf[--outIdx] = '\0';
  
  // Ignore empty commands
  if (*cmdStart == '\0') {
    BLE_DEBUGF(DEBUG_BLE_DATA, "Ignoring empty/non-printable command");
    return;
  }

  bleMarkActivity(connId);
  
  BLE_DEBUGF(DEBUG_BLE_DATA, "Processing command: %s", cmdStart);
  
  // Add to OLED message history
  #if ENABLE_OLED_DISPLAY
  {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "RX:%.*s", (int)(BLE_MSG_MAX_LEN - 4), cmdStart);
    bleAddMessageToHistory(tagged);
  }
  #endif

  // Session commands - use case-insensitive comparison without String allocation
  if (strncasecmp(cmdStart, "login ", 6) == 0) {
    // Parse username and password from "login <user> <pass>"
    const char* rest = cmdStart + 6;
    while (*rest == ' ') rest++;  // Skip leading spaces
    
    const char* sp = strchr(rest, ' ');
    if (!sp) {
      const char* msg = "Usage: login <username> <password>";
      sendBLEResponseToConn(connId, msg, strlen(msg));
      return;
    }
    
    size_t ulen = sp - rest;
    char u[64];
    strncpy(u, rest, ulen < sizeof(u) - 1 ? ulen : sizeof(u) - 1);
    u[ulen < sizeof(u) - 1 ? ulen : sizeof(u) - 1] = '\0';
    
    const char* pstart = sp + 1;
    while (*pstart == ' ') pstart++;  // Skip spaces before password
    char p[128];
    strncpy(p, pstart, sizeof(p) - 1);
    p[sizeof(p) - 1] = '\0';
    // Defer to cmd_exec_task — isValidUser() uses too much stack for BTC_TASK (see bleLoginAsyncCallback).
    // BTC_TASK is a regular FreeRTOS task (not an ISR), so PSRAM-backed
    // allocations are safe. ps_alloc falls back to internal heap if PSRAM
    // is exhausted.
    BleLoginAsyncJob* job = (BleLoginAsyncJob*)ps_alloc(sizeof(BleLoginAsyncJob),
                                                       AllocPref::PreferPSRAM,
                                                       "ble.loginAsyncJob");
    if (!job) {
      const char* msg = "Error: out of memory";
      sendBLEResponseToConn(connId, msg, strlen(msg));
      return;
    }
    memset(job, 0, sizeof(*job));
    job->connId = connId;
    strncpy(job->user, u, sizeof(job->user) - 1);

    Command ucmd;
    char loginCmd[256];
    snprintf(loginCmd, sizeof(loginCmd), "login %s %s bluetooth", u, p);
    ucmd.line = loginCmd;
    ucmd.ctx.origin = ORIGIN_BLUETOOTH;
    ucmd.ctx.auth.transport = SOURCE_BLUETOOTH;
    ucmd.ctx.auth.path = "/ble/cli";
    ucmd.ctx.auth.ip = kBleIpTag;
    ucmd.ctx.auth.sid = String(connId);
    ucmd.ctx.auth.opaque = nullptr;
    ucmd.ctx.auth.user = "";
    ucmd.ctx.validateOnly = false;
    ucmd.ctx.outputMask = CMD_OUT_LOG | CMD_OUT_BLE;
    ucmd.ctx.replyHandle = nullptr;
    ucmd.ctx.httpReq = nullptr;
    ucmd.ctx.id = (uint32_t)millis();
    ucmd.ctx.timestampMs = (uint32_t)millis();

    if (!submitCommandAsync(ucmd, bleLoginAsyncCallback, job)) {
      free(job);
      const char* msg = "Error: Failed to queue command";
      sendBLEResponseToConn(connId, msg, strlen(msg));
    }
    return;
  }
  if (strcasecmp(cmdStart, "logout") == 0) {
    bleLogout(connId);
    const char* msg = "[ble] Logged out.";
    sendBLEResponseToConn(connId, msg, strlen(msg));
    return;
  }
  if (strcasecmp(cmdStart, "whoami") == 0) {
    String u;
    if (bleIsAuthed(connId, u)) {
      char out[80];
      snprintf(out, sizeof(out), "You are %s%s", u.c_str(), isAdminUser(u) ? " (admin)" : "");
      sendBLEResponseToConn(connId, out, strlen(out));
    } else {
      const char* msg = "You are (unknown)";
      sendBLEResponseToConn(connId, msg, strlen(msg));
    }
    return;
  }

  // Auth gate
  if (gSettings.bluetoothRequireAuth) {
    String u;
    if (!bleIsAuthed(connId, u)) {
      bleSendAuthRequired(connId);
      return;
    }
  }

  // Execute command via central cmd_exec task (avoids BTC_TASK stack overflow)
  // Build Command structure for async submission
  Command ucmd;
  ucmd.line = cmdStart;
  ucmd.ctx.origin = ORIGIN_BLUETOOTH;
  ucmd.ctx.auth.transport = SOURCE_BLUETOOTH;
  ucmd.ctx.auth.path = "/ble/cli";
  ucmd.ctx.auth.ip = kBleIpTag;
  ucmd.ctx.auth.sid = String(connId);
  ucmd.ctx.auth.opaque = nullptr;
  if (gSettings.bluetoothRequireAuth) {
    String u;
    (void)bleIsAuthed(connId, u);
    ucmd.ctx.auth.user = u;
  } else {
    ucmd.ctx.auth.user = "";
  }
  ucmd.ctx.validateOnly = false;
  ucmd.ctx.outputMask = CMD_OUT_LOG | CMD_OUT_BLE;
  ucmd.ctx.replyHandle = nullptr;
  ucmd.ctx.httpReq = nullptr;
  ucmd.ctx.id = (uint32_t)millis();

  // Submit async - callback will send BLE response when complete
  if (!submitCommandAsync(ucmd, bleCommandResultCallback, (void*)(uintptr_t)connId)) {
    const char* msg = "Error: Failed to queue command";
    sendBLEResponseToConn(connId, msg, strlen(msg));
  }
}

// =============================================================================
// INITIALIZATION
// =============================================================================

bool initBluetooth() {
  if (gBLEState && gBLEState->initialized) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Already initialized");
    return true;
  }

#if ENABLE_G2_GLASSES
  // Mirror what initG2Client() does on its side: the two roles share the BLE
  // controller and cannot coexist, so tear down the other side first.
  if (isG2ClientInitialized()) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Stopping G2 client mode first");
    broadcastOutput("[BLE] Stopping G2 client mode first");
    deinitG2Client();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
#endif

  // Check memory before initializing BLE stack (~60KB DRAM for controller + host tasks)
  if (!checkMemoryAvailable("bluetooth", nullptr)) {
    if (sBLEToggleCount > 0) {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need ~60KB DRAM)");
      broadcastOutput("[BLE] ESP32 BLE leaks ~10KB DRAM per stop/start cycle. Reboot to recover.");
    } else {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need ~60KB DRAM)");
    }
    return false;
  }
  
  // Track DRAM before init to measure leak on deinit
  sBLEHeapBeforeInit = ESP.getFreeHeap();
  
  // Allocate state structure
  gBLEState = (BLESystemState*)ps_alloc(sizeof(BLESystemState), AllocPref::PreferPSRAM, "ble.state");
  if (!gBLEState) {
    broadcastOutput("[BLE] Failed to allocate state");
    return false;
  }
  memset(gBLEState, 0, sizeof(BLESystemState));
  
  // Initialize connection slots
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    gBLEState->connections[i].active = false;
    gBLEState->connections[i].connId = 0;
    gBLEState->connections[i].deviceType = BLE_DEVICE_UNKNOWN;
    gBLEState->connections[i].authed = false;
    gBLEState->connections[i].user = "";
    gBLEState->connections[i].lastActivityMs = 0;
  }
  gBLEState->activeConnectionCount = 0;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Initializing ESP32 BLE...");
  
  // Initialize ESP32 BLE with configured device name
  const char* deviceName = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName.c_str() : "HardwareOne";
  BLEDevice::init(deviceName);

  if (!BLEDevice::getInitialized()) {
    broadcastOutput("[BLE] Init failed (controller not started)");
    if (gBLEState) {
      free(gBLEState);
      gBLEState = nullptr;
    }
    return false;
  }
  
  // Set TX power level (ESP_PWR_LVL_N12 to ESP_PWR_LVL_P9)
  // Map 0-7 to actual power levels
  esp_power_level_t powerLevel = (esp_power_level_t)constrain(gSettings.bleTxPower, 0, 7);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, powerLevel);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, powerLevel);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, powerLevel);
  
  // Create server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // --------------------------------------------
  // Device Info Service (standard 0x180A)
  // --------------------------------------------
  pDeviceInfoService = pServer->createService(BLE_DEVICE_INFO_SERVICE_UUID);
  
  pManufacturerChar = pDeviceInfoService->createCharacteristic(
    BLE_MANUFACTURER_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pManufacturerChar->setValue("HardwareOne");
  
  pModelChar = pDeviceInfoService->createCharacteristic(
    BLE_MODEL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pModelChar->setValue("ESP32-S3 Hub");
  
  pFirmwareChar = pDeviceInfoService->createCharacteristic(
    BLE_FIRMWARE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pFirmwareChar->setValue("2.1.0");
  
  pDeviceInfoService->start();
  
  // --------------------------------------------
  // Command Service (single service for all communication)
  // --------------------------------------------
  pCommandService = pServer->createService(BLE_COMMAND_SERVICE_UUID);
  
  // Request characteristic (write from client - any command)
  pCmdRequestChar = pCommandService->createCharacteristic(
    BLE_CMD_REQUEST_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdRequestChar->setCallbacks(new CmdRequestCallbacks());
  
  // Response characteristic (notify to client - command results)
  pCmdResponseChar = pCommandService->createCharacteristic(
    BLE_CMD_RESPONSE_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCmdResponseChar->addDescriptor(new BLE2902());  // Required for notifications
  
  // Status characteristic (read - connection info)
  pCmdStatusChar = pCommandService->createCharacteristic(
    BLE_CMD_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pCmdStatusChar->setCallbacks(new CmdStatusCallbacks());
  
  pCommandService->start();
  
  // --------------------------------------------
  // Data Streaming Service
  // --------------------------------------------
  pDataService = pServer->createService(BLE_DATA_SERVICE_UUID);
  
  // Sensor data characteristic (notify - continuous sensor updates)
  pSensorDataChar = pDataService->createCharacteristic(
    BLE_SENSOR_DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSensorDataChar->addDescriptor(new BLE2902());
  
  // System status characteristic (notify - system health updates)
  pSystemStatusChar = pDataService->createCharacteristic(
    BLE_SYSTEM_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSystemStatusChar->addDescriptor(new BLE2902());
  
  // Event notification characteristic (notify - important events)
  pEventNotifyChar = pDataService->createCharacteristic(
    BLE_EVENT_NOTIFY_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pEventNotifyChar->addDescriptor(new BLE2902());
  
  // Stream control characteristic (write - enable/disable streams)
  pStreamControlChar = pDataService->createCharacteristic(
    BLE_STREAM_CONTROL_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  
  pDataService->start();
  
  // --------------------------------------------
  // Setup Advertising
  // --------------------------------------------
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_COMMAND_SERVICE_UUID);
  pAdvertising->addServiceUUID(BLE_DATA_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Help with iPhone connection issues
  pAdvertising->setMinPreferred(0x12);
  
  // Initialize streaming state
  gBLEState->streamFlags = BLE_STREAM_NONE;
  gBLEState->sensorStreamInterval = 1000;  // Default 1 second
  gBLEState->systemStreamInterval = 5000;  // Default 5 seconds
  gBLEState->lastSensorStream = 0;
  gBLEState->lastSystemStream = 0;
  gBLEState->sensorStreamCount = 0;
  gBLEState->systemStreamCount = 0;
  gBLEState->eventCount = 0;
  
  gBLEState->initialized = true;
  gBLEState->connectionState = BLE_STATE_IDLE;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Bluetooth initialized successfully");
  broadcastOutput("[BLE] Initialized - ready to advertise");
  
  return true;
}

void deinitBluetooth() {
  if (!gBLEState || !gBLEState->initialized) return;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Deinitializing Bluetooth...");
  
  stopBLEAdvertising();
  
  // ESP32 BLE doesn't have a clean disconnect API like NimBLE
  // Just deinit the device
  BLEDevice::deinit(false);

  vTaskDelay(pdMS_TO_TICKS(25));
  
  // Clear all BLE object pointers (they're invalid after deinit)
  pServer = nullptr;
  pAdvertising = nullptr;
  pCommandService = nullptr;
  pDataService = nullptr;
  pDeviceInfoService = nullptr;
  pCmdRequestChar = nullptr;
  pCmdResponseChar = nullptr;
  pCmdStatusChar = nullptr;
  pSensorDataChar = nullptr;
  pSystemStatusChar = nullptr;
  pEventNotifyChar = nullptr;
  pStreamControlChar = nullptr;
  pManufacturerChar = nullptr;
  pModelChar = nullptr;
  pFirmwareChar = nullptr;
  
  // Free and clear state
  if (gBLEState) {
    free(gBLEState);
    gBLEState = nullptr;
  }
  
  sBLEToggleCount++;
  size_t heapAfterDeinit = ESP.getFreeHeap();
  int leaked = (int)sBLEHeapBeforeInit - (int)heapAfterDeinit;
  if (leaked > 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "[BLE] Deinitialized (DRAM leak: ~%dKB this cycle, %d toggle%s total)",
             leaked / 1024, sBLEToggleCount, sBLEToggleCount == 1 ? "" : "s");
    broadcastOutput(buf);
  } else {
    broadcastOutput("[BLE] Deinitialized");
  }
}

// =============================================================================
// ADVERTISING CONTROL
// =============================================================================

bool startBLEAdvertising() {
  if (!gBLEState || !gBLEState->initialized) {
    broadcastOutput("[BLE] Not initialized");
    return false;
  }
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Already connected, not advertising");
    return false;
  }
  
  if (pAdvertising) {
    BLEDevice::startAdvertising();
    gBLEState->connectionState = BLE_STATE_ADVERTISING;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising started");
    broadcastOutput("[BLE] Advertising started - device visible as 'HardwareOne'");
    return true;
  }
  
  return false;
}

void stopBLEAdvertising() {
  if (!gBLEState || !gBLEState->initialized) return;
  
  if (pAdvertising && gBLEState->connectionState == BLE_STATE_ADVERTISING) {
    BLEDevice::stopAdvertising();
    gBLEState->connectionState = BLE_STATE_IDLE;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising stopped");
  }
}

// =============================================================================
// CONNECTION MANAGEMENT
// =============================================================================

bool isBLEConnected() {
  return gBLEState && gBLEState->connectionState == BLE_STATE_CONNECTED;
}

void disconnectBLE() {
  if (!gBLEState || !pServer) return;
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Disconnecting client...");
    // ESP32 BLE uses disconnect() with connection ID (0 for first client)
    pServer->disconnect(0);
  }
}

uint32_t getBLEConnectionDuration() {
  if (!gBLEState || gBLEState->connectionState != BLE_STATE_CONNECTED) {
    return 0;
  }
  // Return duration of first active connection
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBLEState->connections[i].active) {
      return millis() - gBLEState->connections[i].connectedSince;
    }
  }
  return 0;
}

// =============================================================================
// DATA TRANSMISSION
// =============================================================================

bool sendBLEResponse(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "sendBLEResponse dropped (connected=%d char=%p)",
               isBLEConnected(), (void*)pCmdResponseChar);
    return false;
  }
  
  pCmdResponseChar->setValue((uint8_t*)data, len);
  pCmdResponseChar->notify();  // ESP32 BLE notify() works the same
  gBLEState->responsesSent++;

  #if ENABLE_OLED_DISPLAY
  {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "TX:%.*s", (int)(BLE_MSG_MAX_LEN - 4), data ? data : "");
    bleAddMessageToHistory(tagged);
  }
  #endif
  
  BLE_DEBUGF(DEBUG_BLE_DATA, "Response sent (%d bytes)", len);
  return true;
}

bool sendBLEResponseToConn(uint16_t connId, const char* data, size_t len) {
  if (!data || len == 0) return false;

  if (!gBLEState || gBLEState->activeConnectionCount <= 1) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "sendBLEResponseToConn passthrough conn=%u", (unsigned)connId);
    return sendBLEResponse(data, len);
  }

  if (!ensureDebugBuffer()) {
    return sendBLEResponse(data, len);
  }

  char* tagged = getDebugBuffer();
  snprintf(tagged, 1024, "[ble conn:%u] %.*s", (unsigned)connId, (int)len, data);
  BLE_DEBUGF(DEBUG_BLE_DATA, "sendBLEResponseToConn fallback broadcast (conn=%u)", (unsigned)connId);
  return sendBLEResponse(tagged, strlen(tagged));
}

void bleSessionTick() {
  if (!gBLEState) return;
  
  // Handle deferred connect event (set by callback, processed here with proper stack)
  if (gBLEState->deferredConnectPending) {
    gBLEState->deferredConnectPending = false;
    int slot = gBLEState->deferredConnectSlot;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Client connected (slot %d, total active: %d/%d)", 
               slot, gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
    if (gBLEState->activeConnectionCount >= BLE_MAX_CONNECTIONS) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Max connections reached - stopped advertising");
    }
  }
  
  // Handle deferred disconnect event (set by callback, processed here with proper stack)
  if (gBLEState->deferredDisconnectPending) {
    gBLEState->deferredDisconnectPending = false;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Client disconnected (active connections: %d)", 
               gBLEState->deferredDisconnectActiveCount);
    if (gBLEState->deferredDisconnectActiveCount < BLE_MAX_CONNECTIONS) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Auto-restarted advertising (slots available)");
    }
    // Auto-disable BLE broadcast output when no authenticated sessions remain
    if (!bleHasAuthenticatedSession()) {
      gOutputFlags &= ~OUTPUT_BLE;
    }
  }
  
  // Handle deferred command received event (set by callback, processed here with proper stack)
  if (gBLEState->deferredCmdReceivedPending) {
    gBLEState->deferredCmdReceivedPending = false;
    BLE_DEBUGF(DEBUG_BLE_GATT, "Command received (%d bytes) conn_id=%u", 
               (int)gBLEState->deferredCmdReceivedLen, (unsigned)gBLEState->deferredCmdReceivedConnId);
  }
  
  if (!isBLEConnected()) return;
  uint32_t now = millis();
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    if (gBLEState->connections[i].lastActivityMs == 0) continue;

    if (now - gBLEState->connections[i].lastActivityMs > kBleSessionIdleTimeoutMs) {
      char msg[80];
      snprintf(msg, sizeof(msg), "[ble] Session expired for user '%s'", gBLEState->connections[i].user.c_str());
      sendBLEResponseToConn(gBLEState->connections[i].connId, msg, strlen(msg));
      gBLEState->connections[i].authed = false;
      gBLEState->connections[i].user = "";
      BLE_DEBUGF(DEBUG_BLE_CORE,
                 "Session expired conn=%u user='%s' idle_ms=%lu",
                 gBLEState->connections[i].connId,
                 gBLEState->connections[i].user.c_str(),
                 (unsigned long)(now - gBLEState->connections[i].lastActivityMs));
      // Auto-disable BLE broadcast output when no authenticated sessions remain
      if (!bleHasAuthenticatedSession()) {
        gOutputFlags &= ~OUTPUT_BLE;
      }
    }
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session tick");
}

// =============================================================================
// STATUS
// =============================================================================

bool isBLERunning() {
  return gBLEState && gBLEState->initialized;
}

const char* getBLEStateString() {
  if (!gBLEState || !gBLEState->initialized) return "uninitialized";
  
  switch (gBLEState->connectionState) {
    case BLE_STATE_IDLE:        return "idle";
    case BLE_STATE_ADVERTISING: return "advertising";
    case BLE_STATE_SCANNING:    return "scanning";
    case BLE_STATE_CONNECTING:  return "connecting";
    case BLE_STATE_CONNECTED:   return "connected";
    case BLE_STATE_DISCONNECTING: return "disconnecting";
    default: return "unknown";
  }
}

void getBLEStatus(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize < 64) return;
  
  if (!gBLEState || !gBLEState->initialized) {
    snprintf(buffer, bufferSize, "Bluetooth: disabled");
    return;
  }
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    snprintf(buffer, bufferSize, 
             "BLE: %d/%d connected (rx:%lu tx:%lu)",
             gBLEState->activeConnectionCount,
             BLE_MAX_CONNECTIONS,
             gBLEState->commandsReceived,
             gBLEState->responsesSent);
  } else {
    snprintf(buffer, bufferSize, 
             "BLE: %s (total: %lu)",
             getBLEStateString(),
             gBLEState->totalConnections);
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE status requested");
}

void bleApplySettings() {
  // Refresh MAC address cache when settings change
  bleUpdateMACCache();
  BLE_DEBUGF(DEBUG_BLE_CORE, "Settings applied, MAC cache refreshed");
}

// =============================================================================
// COMMAND HANDLERS
// =============================================================================

static const char* cmd_blestart(const String& argsInput) {
  // Pause sensor polling during BLE init to avoid interrupt contention
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete
  
  bool initOk = initBluetooth();
  bool advOk = initOk ? startBLEAdvertising() : false;
  
  gSensorPollingPaused = wasPaused;
  
  if (!initOk) {
    return "Failed to initialize Bluetooth";
  }
  if (!advOk) {
    return "Failed to start advertising";
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE started and advertising");
  return "Bluetooth started and advertising";
}

static const char* cmd_blestop(const String& argsInput) {
  deinitBluetooth();
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE stopped");
  return "Bluetooth stopped";
}

static const char* cmd_blestatus(const String& argsInput) {
  if (!gBLEState || !gBLEState->initialized) {
    return "Bluetooth not initialized. Run 'openble' first.";
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char* buf = getDebugBuffer();
  int offset = 0;
  int remaining = 1024;
  
  // Build the full status string
  int written = snprintf(buf + offset, remaining, "BLE Status: %s\n", getBLEStateString());
  if (written > 0) { offset += written; remaining -= written; }
  
  written = snprintf(buf + offset, remaining, "Active connections: %d/%d\n", gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
  if (written > 0) { offset += written; remaining -= written; }
  
  // Show each active connection
  for (int i = 0; i < BLE_MAX_CONNECTIONS && remaining > 100; i++) {
    if (gBLEState->connections[i].active) {
      uint32_t duration = (millis() - gBLEState->connections[i].connectedSince) / 1000;
      char macStr[18];
      macToStackBuf(gBLEState->connections[i].deviceAddr, macStr);
      written = snprintf(buf + offset, remaining, "[%d] %s\n    MAC: %s | %lu sec | %lu cmds\n", 
               i, gBLEState->connections[i].deviceName.c_str(),
               macStr, duration, gBLEState->connections[i].commandsReceived);
      if (written > 0) { offset += written; remaining -= written; }
    }
  }
  
  written = snprintf(buf + offset, remaining, "Total connections: %lu\n", gBLEState->totalConnections);
  if (written > 0) { offset += written; remaining -= written; }
  written = snprintf(buf + offset, remaining, "Commands received: %lu\n", gBLEState->commandsReceived);
  if (written > 0) { offset += written; remaining -= written; }
  written = snprintf(buf + offset, remaining, "Responses sent: %lu", gBLEState->responsesSent);
  
  // Also broadcast to serial for backwards compatibility
  broadcastOutput(buf);
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE status requested. Active=%d", gBLEState->activeConnectionCount);

  return buf;
}

static const char* cmd_bledisconnect(const String& argsInput) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Manual disconnect requested");
  disconnectBLE();
  return "Disconnecting client...";
}

static const char* cmd_bleadv(const String& argsInput) {
  // Optional argument: start (default) / stop / toggle. Toggle relies on
  // startBLEAdvertising returning false when already advertising — same
  // signal the legacy G2 tap handler used. No arg keeps back-compat with
  // the original "start-only" form.
  String a = argsInput; a.trim(); a.toLowerCase();
  if (a == "stop") {
    stopBLEAdvertising();
    BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv stop");
    return "Advertising stopped";
  }
  if (a == "toggle") {
    if (startBLEAdvertising()) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv toggle → start");
      return "Advertising started";
    }
    stopBLEAdvertising();
    BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv toggle → stop");
    return "Advertising stopped";
  }
  if (startBLEAdvertising()) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv manual trigger success");
    return "Advertising started";
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv manual trigger failed");
  return "Failed to start advertising";
}

static const char* cmd_blesend(const String& argsInput) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  
  // Extract message after first space (skip command name) - zero heap allocations
  const char* msg = argsInput.c_str();
  while (*msg && *msg != ' ') msg++;  // Skip command name
  while (*msg == ' ') msg++;  // Skip spaces
  
  if (*msg == '\0') {
    return "Usage: blesend <message>";
  }
  
  if (sendBLEResponse(msg, strlen(msg))) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "blesend transmitted len=%u", (unsigned)strlen(msg));
    return "Message sent via BLE";
  }
  BLE_DEBUGF(DEBUG_BLE_DATA, "blesend failed len=%u", (unsigned)strlen(msg));
  return "Failed to send message";
}

static const char* cmd_blestream(const String& argsInput) {
  if (!gBLEState || !gBLEState->initialized) {
    return "Bluetooth not initialized";
  }
  
  // Parse args - skip command name, zero heap allocations
  const char* args = argsInput.c_str();
  while (*args && *args != ' ') args++;  // Skip command name
  while (*args == ' ') args++;  // Skip spaces
  
  if (*args == '\0') {
    // Show current status
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "Streaming: sensors=%s system=%s events=%s",
             bleIsStreamEnabled(BLE_STREAM_SENSORS) ? "ON" : "OFF",
             bleIsStreamEnabled(BLE_STREAM_SYSTEM) ? "ON" : "OFF",
             bleIsStreamEnabled(BLE_STREAM_EVENTS) ? "ON" : "OFF");
    broadcastOutput(buf);
    snprintf(buf, 1024, "Intervals: sensor=%lums system=%lums",
             (unsigned long)gBLEState->sensorStreamInterval,
             (unsigned long)gBLEState->systemStreamInterval);
    broadcastOutput(buf);
    snprintf(buf, 1024, "Stats: sensors=%lu system=%lu events=%lu",
             (unsigned long)gBLEState->sensorStreamCount,
             (unsigned long)gBLEState->systemStreamCount,
             (unsigned long)gBLEState->eventCount);
    return buf;
  }
  
  // Parse command: blestream <on|off|sensors|system|events|interval>
  if (strncasecmp(args, "on", 2) == 0 && (args[2] == '\0' || args[2] == ' ')) {
    bleEnableStream(BLE_STREAM_ALL);
    BLE_DEBUGF(DEBUG_BLE_DATA, "blestream all=ON");
    return "All streams enabled";
  } else if (strncasecmp(args, "off", 3) == 0 && (args[3] == '\0' || args[3] == ' ')) {
    bleDisableStream(BLE_STREAM_ALL);
    BLE_DEBUGF(DEBUG_BLE_DATA, "blestream all=OFF");
    return "All streams disabled";
  } else if (strncasecmp(args, "sensors", 7) == 0) {
    if (strstr(args, "off") != nullptr) {
      bleDisableStream(BLE_STREAM_SENSORS);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream sensors=OFF");
      return "Sensor stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_SENSORS);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream sensors=ON");
      return "Sensor stream enabled";
    }
  } else if (strncasecmp(args, "system", 6) == 0) {
    if (strstr(args, "off") != nullptr) {
      bleDisableStream(BLE_STREAM_SYSTEM);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream system=OFF");
      return "System stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_SYSTEM);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream system=ON");
      return "System stream enabled";
    }
  } else if (strncasecmp(args, "events", 6) == 0) {
    if (strstr(args, "off") != nullptr) {
      bleDisableStream(BLE_STREAM_EVENTS);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream events=OFF");
      return "Event stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_EVENTS);
      BLE_DEBUGF(DEBUG_BLE_DATA, "blestream events=ON");
      return "Event stream enabled";
    }
  } else if (strncasecmp(args, "interval", 8) == 0) {
    // Parse: blestream interval <sensor_ms> <system_ms>
    const char* p = args + 8;
    while (*p == ' ') p++;
    uint32_t sensorMs = atoi(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    uint32_t systemMs = atoi(p);
    if (sensorMs > 0 && systemMs > 0) {
      if (sensorMs >= 100 && systemMs >= 100) {
        bleSetStreamInterval(sensorMs, systemMs);
        if (!ensureDebugBuffer()) return "Intervals set";
        char* buf = getDebugBuffer();
        snprintf(buf, 1024, "Intervals set: sensor=%lums system=%lums",
                 (unsigned long)sensorMs, (unsigned long)systemMs);
        BLE_DEBUGF(DEBUG_BLE_DATA,
                   "blestream interval sensor=%lu system=%lu",
                   (unsigned long)sensorMs,
                   (unsigned long)systemMs);
        return buf;
      }
      return "Intervals must be >= 100ms";
    }
    return "Usage: blestream interval <sensor_ms> <system_ms>";
  }
  
  return "Usage: blestream <on|off|sensors|system|events|interval>";
}

static const char* cmd_bleevent(const String& argsInput) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  
  // Extract message after first space - zero heap allocations
  const char* msg = argsInput.c_str();
  while (*msg && *msg != ' ') msg++;  // Skip command name
  while (*msg == ' ') msg++;  // Skip spaces
  
  if (*msg == '\0') {
    return "Usage: bleevent <message>";
  }
  
  if (blePushEvent(BLE_EVENT_CUSTOM, msg)) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "bleevent sent" );
    return "Event sent via BLE";
  }
  BLE_DEBUGF(DEBUG_BLE_DATA, "bleevent failed");
  return "Failed to send event";
}

static const char* cmd_blename(const String& argsInput) {
  // Parse args - skip command name
  const char* args = argsInput.c_str();
  while (*args && *args != ' ') args++;  // Skip command name
  while (*args == ' ') args++;  // Skip spaces
  
  if (*args != '\0') {
    // Extract new name
    size_t len = strlen(args);
    // Trim trailing spaces
    while (len > 0 && args[len - 1] == ' ') len--;
    
    if (len == 0 || len > 29) {
      return "Name must be 1-29 characters";
    }
    
    String newName(args, len);
    setSetting(gSettings.bleDeviceName, newName);
    
    if (!ensureDebugBuffer()) return "Name saved (restart BLE to apply)";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "BLE name set to '%s'. Restart Bluetooth to apply (closeble && openble)", newName.c_str());
    BLE_DEBUGF(DEBUG_BLE_CORE, "BLE device name updated to '%s'", newName.c_str());
    return buf;
  }
  
  // Show current name
  if (!ensureDebugBuffer()) return gSettings.bleDeviceName.c_str();
  char* buf = getDebugBuffer();
  snprintf(buf, 1024, "BLE Device Name: %s", gSettings.bleDeviceName.c_str());
  return buf;
}

static const char* cmd_bletxpower(const String& argsInput) {
  // Parse args - skip command name
  const char* args = argsInput.c_str();
  while (*args && *args != ' ') args++;  // Skip command name
  while (*args == ' ') args++;  // Skip spaces
  
  if (*args != '\0') {
    int level = atoi(args);
    
    if (level < 0 || level > 7) {
      return "TX power must be 0-7 (0=min/-12dBm, 7=max/+9dBm)";
    }
    
    setSetting(gSettings.bleTxPower, level);
    
    // Apply immediately if BLE is running
    if (gBLEState && gBLEState->initialized) {
      esp_power_level_t powerLevel = (esp_power_level_t)level;
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, powerLevel);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, powerLevel);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, powerLevel);
    }
    
    if (!ensureDebugBuffer()) return "TX power updated";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "BLE TX power set to level %d", level);
    BLE_DEBUGF(DEBUG_BLE_CORE, "TX power set to %d", level);
    return buf;
  }
  
  // Show current power level
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  snprintf(buf, 1024, "BLE TX Power: %d (0=min/-12dBm, 7=max/+9dBm)", gSettings.bleTxPower);
  return buf;
}

static const char* cmd_bleinfo(const String& argsInput) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  int pos = 0;
  int rem = 1024;
  int w;

  w = snprintf(buf + pos, rem, "=== BLE Configuration ===\n"); if (w > 0) { pos += w; rem -= w; }
  w = snprintf(buf + pos, rem, "Device Name:  %s\n", gSettings.bleDeviceName.c_str());                    if (w > 0) { pos += w; rem -= w; }
  w = snprintf(buf + pos, rem, "TX Power:     %d (0=min/-12dBm, 7=max/+9dBm)\n", gSettings.bleTxPower);  if (w > 0) { pos += w; rem -= w; }
  w = snprintf(buf + pos, rem, "Auto-Start:   %s\n", gSettings.bluetoothAutoStart  ? "Yes" : "No");       if (w > 0) { pos += w; rem -= w; }
  w = snprintf(buf + pos, rem, "Require Auth: %s\n", gSettings.bluetoothRequireAuth ? "Yes" : "No");      if (w > 0) { pos += w; rem -= w; }

  if (gBLEState && gBLEState->initialized) {
    w = snprintf(buf + pos, rem, "Status:       %s\n", getBLEStateString());                              if (w > 0) { pos += w; rem -= w; }
    w = snprintf(buf + pos, rem, "Connections:  %d/%d", gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS); if (w > 0) { pos += w; rem -= w; }
  } else {
    w = snprintf(buf + pos, rem, "Status:       Not initialized");                                        if (w > 0) { pos += w; rem -= w; }
  }

  return buf;
}

// Migrated to BOOL_CMD — see System_Utils.h. Equivalent to the previous
// 12-line on/off handler. Output strings preserved verbatim so external
// matchers (web UI, tests) keep working.
BOOL_CMD(bleautostart, gSettings.bluetoothAutoStart, "[BLE] Auto-start")

// Backward-compat shims. Both forward to the generic `bleautoconnect <name>`
// in BLE_Peers.cpp. Kept so existing muscle memory + the BT web panel's
// checkboxes keep working without a string change.
static const char* cmd_g2autoconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String forwarded = String("g2-glasses ") + argsInput;
  forwarded.trim();
  return cmd_bleautoconnect(forwarded);
}
static const char* cmd_ringautoconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String forwarded = String("r1-ring ") + argsInput;
  forwarded.trim();
  return cmd_bleautoconnect(forwarded);
}

BOOL_CMD(blerequireauth, gSettings.bluetoothRequireAuth, "[BLE] Require auth")

// -----------------------------------------------------------------------------
// Mode (server vs. G2 client)
// -----------------------------------------------------------------------------

const char* getBleModeString() {
  return (gSettings.bleMode == BLE_MODE_G2_CLIENT) ? "client" : "server";
}

// Aggregate-status helpers: mirror the ESPNow mesh-or-direct pattern so the
// dashboard sees BLE as "running" whether server mode or G2 client mode is up.
bool bleSubsystemActive() {
  if (isBLERunning()) return true;
#if ENABLE_G2_GLASSES
  if (isG2ClientInitialized()) return true;
#endif
  return false;
}

const char* bleSubsystemStateString() {
#if ENABLE_G2_GLASSES
  // Prefer G2 state when in client mode and the client is up — server may not
  // even be initialized in that mode.
  if (gSettings.bleMode == BLE_MODE_G2_CLIENT && isG2ClientInitialized()) {
    return getG2StateString();
  }
#endif
  if (isBLERunning()) return getBLEStateString();
#if ENABLE_G2_GLASSES
  if (isG2ClientInitialized()) return getG2StateString();
#endif
  return "uninitialized";
}

static const char* cmd_blemode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return getBleModeString();
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "BLE mode: %s", getBleModeString());
    return buf;
  }
  arg.toLowerCase();

  if (arg == "server" || arg == "phone") {
#if ENABLE_G2_GLASSES
    if (isG2ClientInitialized()) {
      broadcastOutput("[BLE] Stopping G2 client mode");
      deinitG2Client();
    }
#endif
    setSetting(gSettings.bleMode, (int)BLE_MODE_SERVER);
    return "[BLE] Mode set to server";
  }

  if (arg == "client" || arg == "g2") {
#if !ENABLE_G2_GLASSES
    return "[BLE] G2 client not compiled (ENABLE_G2_GLASSES=0)";
#else
    if (gBLEState && gBLEState->initialized) {
      broadcastOutput("[BLE] Stopping BLE server mode");
      deinitBluetooth();
    }
    setSetting(gSettings.bleMode, (int)BLE_MODE_G2_CLIENT);
    return "[BLE] Mode set to client (G2)";
#endif
  }

  return "Usage: blemode [server|client]";
}

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry bluetoothCommands[] = {
  // Start/Stop (3-level voice: "connection" -> "bluetooth" -> "open/close")
  { "openble",      "Start Bluetooth LE and begin advertising.", false, cmd_blestart, nullptr, "connection", "bluetooth", "open" },
  { "closeble",     "Stop Bluetooth LE and deinitialize.",       false, cmd_blestop,  nullptr, "connection", "bluetooth", "close" },
  { "bleread",      "Read Bluetooth connection status.",         false, cmd_blestatus },
  { "blestatus",    "Show Bluetooth connection status.",         false, cmd_blestatus },
  { "bleinfo",      "Show BLE configuration and settings.",      false, cmd_bleinfo },
  { "blename",      "Get/set BLE device name [name].",           false, cmd_blename },
  { "bletxpower",   "Get/set BLE TX power [0-7].",               false, cmd_bletxpower },
  { "bledisconnect","Disconnect current BLE client.",            false, cmd_bledisconnect },
  { "bleadv",       "Start/stop/toggle BLE advertising [start|stop|toggle].", false, cmd_bleadv, "Usage: bleadv [start|stop|toggle]" },
  { "blesend",      "Send message to BLE client: <message>.",    false, cmd_blesend },
  { "blestream",    "Control streaming: <on|off|sensors|system>.",false, cmd_blestream },
  { "bleevent",     "Send event to BLE client: <event>.",        false, cmd_bleevent },
  
  // Auto-start
  { "bleautostart",    "Enable/disable BLE auto-start after boot [on|off].",   false, cmd_bleautostart,    "Usage: bleautostart [on|off]" },
  { "blerequireauth", "Enable/disable BLE authentication requirement [on|off].", true, cmd_blerequireauth, "Usage: blerequireauth [on|off]" },

  // Mode (server vs. G2 client) - mutually exclusive at runtime
  { "blemode",        "Get/set BLE mode [server|client].",                     false, cmd_blemode,         "Usage: blemode [server|client]" },

  // Auto-reconnect at boot to saved-MAC peers (no scan fallback). Pairing is
  // separate (`openg2 auto`, `ringconnect`) and always saves the MAC; these
  // flags only control whether boot reconnects automatically. The generic
  // form `bleautoconnect <peer-name> [on|off]` works for any registered
  // peer; the per-peer commands are kept as muscle-memory shims.
  { "bleautoconnect",  "Per-peer auto-reconnect at boot: bleautoconnect <name> [on|off]. `blepeers` lists names.", false, cmd_bleautoconnect, "Usage: bleautoconnect <peer-name> [on|off]" },
  { "blepeers",        "List all registered BLE peers and their state.",                                          false, cmd_blepeers,        nullptr },
  { "g2autoconnect",   "Alias for `bleautoconnect g2-glasses [on|off]`.",                                          false, cmd_g2autoconnect,   "Usage: g2autoconnect [on|off]" },
  { "ringautoconnect", "Alias for `bleautoconnect r1-ring [on|off]`.",                                             false, cmd_ringautoconnect, "Usage: ringautoconnect [on|off]" },
};

const size_t bluetoothCommandsCount = sizeof(bluetoothCommands) / sizeof(bluetoothCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// =============================================================================
// SETTINGS
// =============================================================================

// Settings entries for Bluetooth
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
// Per-peer rows are emitted programmatically in registerBluetoothPeerSettings()
// (see below) so they stay in sync with BLE_Peers.h's BlePeerKind enum. The
// static rows here cover only the non-peer bluetooth settings.
const SettingEntry bluetoothSettingsEntries[] = {
  { "bluetoothAutoStart",    SETTING_BOOL,   &gSettings.bluetoothAutoStart,    true, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "bleautostart" },
  { "bluetoothRequireAuth",  SETTING_BOOL,   &gSettings.bluetoothRequireAuth,  true, 0, nullptr, 0, 1, "Require Authentication", nullptr, false, nullptr, "blerequireauth" },
  { "bluetoothDeviceName",   SETTING_STRING, &gSettings.bleDeviceName,         true, 0, nullptr, 0, 0, "Device Name", nullptr },
  { "bluetoothTxPower",      SETTING_INT,    &gSettings.bleTxPower,            true, 3, nullptr, 0, 7, "TX Power (0-7)", nullptr, false, nullptr, "bletxpower" },
  { "bluetoothMode",         SETTING_INT,    &gSettings.bleMode,               0,    0, nullptr, 0, 1, "Mode (0=server, 1=g2)", nullptr, false, nullptr, "blemode" }
};

const size_t bluetoothSettingsCount = sizeof(bluetoothSettingsEntries) / sizeof(bluetoothSettingsEntries[0]);

// Register Bluetooth settings module
// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule bluetoothSettingsModule = {
  "bluetooth",
  "bluetooth",
  bluetoothSettingsEntries,
  bluetoothSettingsCount,
  nullptr,
  "Bluetooth classic and BLE settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// =============================================================================
// OLED DISPLAY MODE
// =============================================================================

#if ENABLE_OLED_DISPLAY

// Add message to history (called when command received)
void bleAddMessageToHistory(const char* msg) {
  // Truncate if needed
  strncpy(bleMessageHistory[bleMessageHead], msg, BLE_MSG_MAX_LEN - 1);
  bleMessageHistory[bleMessageHead][BLE_MSG_MAX_LEN - 1] = '\0';
  
  bleMessageHead = (bleMessageHead + 1) % BLE_MSG_HISTORY_SIZE;
  if (bleMessageCount < BLE_MSG_HISTORY_SIZE) bleMessageCount++;
}

// =============================================================================
// BLUETOOTH OLED MENU SYSTEM
// =============================================================================

// Menu state
static int bluetoothMenuSelection = 0;
bool gBluetoothShowingStatus = false;

// G2 Glasses submenu state
static bool bluetoothInG2Menu = false;
static int g2MenuSelection = 0;

// Get number of visible menu items based on BT state
static int getBluetoothMenuItemCount() {
  // When BT is off, only show: Status, Settings, Start/Stop (3 items)
  // When BT is on and connected: show all 5 items + G2 Glasses
  // When BT is on but not connected: show 4 items (no Disconnect) + G2 Glasses
#if ENABLE_G2_GLASSES
  if (!gBLEState || !gBLEState->initialized) return 4;  // +1 for G2 Glasses
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) return 6;
  return 5;  // BT on but not connected
#else
  if (!gBLEState || !gBLEState->initialized) return 3;
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) return 5;
  return 4;  // BT on but not connected - hide Disconnect
#endif
}

// Menu items - full list, items shown based on BT/G2 state
static const char* bluetoothMenuItems[] = {
  "Status",
  "Settings",
  "Start/Stop",
#if ENABLE_G2_GLASSES
  "G2 Glasses >>",
#endif
  "Advertising",
  "Disconnect"
};

#if ENABLE_G2_GLASSES
// G2 submenu items
static const char* g2MenuItems[] = {
  "<< Back",
  "Connect",
  "Disconnect",
  "Status",
  "Show Text",
  "Nav Mode"
};
static const int G2_MENU_ITEM_COUNT = 6;

static int getG2MenuItemCount() {
  return G2_MENU_ITEM_COUNT;
}
#endif

void bluetoothMenuUp() {
  if (gBluetoothShowingStatus) return;
#if ENABLE_G2_GLASSES
  if (bluetoothInG2Menu) {
    int maxItems = getG2MenuItemCount();
    if (g2MenuSelection > 0) {
      g2MenuSelection--;
    } else {
      g2MenuSelection = maxItems - 1;
    }
    return;
  }
#endif
  int maxItems = getBluetoothMenuItemCount();
  if (bluetoothMenuSelection > 0) {
    bluetoothMenuSelection--;
  } else {
    bluetoothMenuSelection = maxItems - 1;
  }
}

void bluetoothMenuDown() {
  if (gBluetoothShowingStatus) return;
#if ENABLE_G2_GLASSES
  if (bluetoothInG2Menu) {
    int maxItems = getG2MenuItemCount();
    if (g2MenuSelection < maxItems - 1) {
      g2MenuSelection++;
    } else {
      g2MenuSelection = 0;
    }
    return;
  }
#endif
  int maxItems = getBluetoothMenuItemCount();
  if (bluetoothMenuSelection < maxItems - 1) {
    bluetoothMenuSelection++;
  } else {
    bluetoothMenuSelection = 0;
  }
}

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

#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"

// G2 text input buffer for Show Text feature
static char g2TextInputBuffer[64] = "Hello from ESP32!";
static bool g2ShowingTextInput = false;

// Execute G2 submenu action
static void executeG2Action() {
  switch (g2MenuSelection) {
    case 0: // Back
      bluetoothInG2Menu = false;
      g2MenuSelection = 0;
      break;
      
    case 1: // Connect
      if (!isG2Connected()) {
        // Initialize G2 client if needed (this will stop BLE server mode)
        if (!isG2ClientInitialized()) {
          initG2Client();
        }
        g2Connect(G2_EYE_AUTO);
      }
      break;
      
    case 2: // Disconnect
      if (isG2Connected()) {
        g2Disconnect();
      }
      break;
      
    case 3: // Status
      gBluetoothShowingStatus = true;  // Reuse status display flag for G2 status
      break;
      
    case 4: // Show Text
      if (isG2Connected()) {
        g2ShowText(g2TextInputBuffer);
      }
      break;
      
    case 5: // Nav Mode toggle
      {
        extern bool gG2MenuNavEnabled;
        gG2MenuNavEnabled = !gG2MenuNavEnabled;
      }
      break;
  }
}
#endif

void executeBluetoothAction() {
  if (gBluetoothShowingStatus) {
    gBluetoothShowingStatus = false;
    return;
  }
  
#if ENABLE_G2_GLASSES
  if (bluetoothInG2Menu) {
    executeG2Action();
    return;
  }
#endif
  
  // Menu indices shift when G2 is enabled
#if ENABLE_G2_GLASSES
  // With G2: 0=Status, 1=Settings, 2=Start/Stop, 3=G2 Glasses, 4=Advertising, 5=Disconnect
  switch (bluetoothMenuSelection) {
    case 0: // Status
      gBluetoothShowingStatus = true;
      break;
      
    case 1: // Settings
      if (openSettingsEditorForModule("bluetooth")) {
        requestOLEDMode(OLED_SETTINGS,"bluetooth.settings.g2", true);
      }
      break;
      
    case 2: // Start/Stop
      if (gBLEState && gBLEState->initialized) {
        oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr, false);
      } else {
        oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr);
      }
      break;
      
    case 3: // G2 Glasses submenu
      bluetoothInG2Menu = true;
      g2MenuSelection = 0;
      break;
      
    case 4: // Advertising
      if (gBLEState && gBLEState->initialized) {
        if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
          stopBLEAdvertising();
        } else {
          startBLEAdvertising();
        }
      }
      break;
      
    case 5: // Disconnect
      if (gBLEState && gBLEState->initialized && 
          gBLEState->connectionState == BLE_STATE_CONNECTED) {
        disconnectBLE();
      }
      break;
  }
#else
  // Without G2: 0=Status, 1=Settings, 2=Start/Stop, 3=Advertising, 4=Disconnect
  switch (bluetoothMenuSelection) {
    case 0: // Status
      gBluetoothShowingStatus = true;
      break;
      
    case 1: // Settings
      if (openSettingsEditorForModule("bluetooth")) {
        requestOLEDMode(OLED_SETTINGS,"bluetooth.settings", true);
      }
      break;
      
    case 2: // Start/Stop
      if (gBLEState && gBLEState->initialized) {
        oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr, false);
      } else {
        oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedMenu, nullptr);
      }
      break;
      
    case 3: // Advertising
      if (gBLEState && gBLEState->initialized) {
        if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
          stopBLEAdvertising();
        } else {
          startBLEAdvertising();
        }
      }
      break;
      
    case 4: // Disconnect
      if (gBLEState && gBLEState->initialized && 
          gBLEState->connectionState == BLE_STATE_CONNECTED) {
        disconnectBLE();
      }
      break;
  }
#endif
}

void bluetoothMenuBack() {
#if ENABLE_G2_GLASSES
  if (bluetoothInG2Menu) {
    if (gBluetoothShowingStatus) {
      gBluetoothShowingStatus = false;
    } else {
      bluetoothInG2Menu = false;
      g2MenuSelection = 0;
    }
    return;
  }
#endif
  if (gBluetoothShowingStatus) {
    gBluetoothShowingStatus = false;
  }
}

// Display detailed status screen
static void displayBluetoothStatusDetail() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  if (!gBLEState || !gBLEState->initialized) {
    oledDisplay->println("BLE: Disabled");
    oledDisplay->println();
    oledDisplay->println("Select Start/Stop");
    oledDisplay->println("to enable");
    return;
  }
  
  // Show device name (truncate if needed, zero heap allocations)
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
  
  // Show state with advertising indicator
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

  // Streaming state summary
  oledDisplay->print("Streams: ");
  bool sensorsOn = (gBLEState->streamFlags & BLE_STREAM_SENSORS);
  bool systemOn = (gBLEState->streamFlags & BLE_STREAM_SYSTEM);
  bool eventsOn = (gBLEState->streamFlags & BLE_STREAM_EVENTS);
  oledDisplay->print(sensorsOn ? "S" : "s");
  oledDisplay->print(systemOn ? " Sys" : " sys");
  oledDisplay->print(eventsOn ? " Ev" : " ev");
  oledDisplay->println();
  oledDisplay->print("Int:" );
  oledDisplay->print(gBLEState->sensorStreamInterval);
  oledDisplay->print("/" );
  oledDisplay->println(gBLEState->systemStreamInterval);
  if (bleMessageCount > 0) {
    oledDisplay->println();
    oledDisplay->println("Last:");
    int toShow = (bleMessageCount > 2) ? 2 : bleMessageCount;
    for (int i = 0; i < toShow; i++) {
      int idx = (int)bleMessageHead - 1 - i;
      while (idx < 0) idx += BLE_MSG_HISTORY_SIZE;
      oledDisplay->println(bleMessageHistory[idx]);
    }
  }
}

#if ENABLE_G2_GLASSES
// Display G2 glasses status screen
static void displayG2StatusDetail() {
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

// Display G2 submenu
static void displayG2Menu() {
  // Show title with connection status
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
  
  // Draw G2 menu items
  int visibleItems = getG2MenuItemCount();
  
  // Clamp selection
  if (g2MenuSelection >= visibleItems) {
    g2MenuSelection = visibleItems - 1;
  }
  
  for (int i = 0; i < visibleItems; i++) {
    if (i == g2MenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->print(g2MenuItems[i]);
    
    // Show state indicators
    if (i == 1) { // Connect
      if (isG2Connected()) oledDisplay->print(" *");
    } else if (i == 5) { // Nav Mode
      extern bool gG2MenuNavEnabled;
      oledDisplay->print(gG2MenuNavEnabled ? " *" : "");
    }
    oledDisplay->println();
  }
}
#endif

// OLED display function for Bluetooth mode
static void displayBluetoothStatus() {
  if (!oledDisplay) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  
#if ENABLE_G2_GLASSES
  // Show G2 submenu if active
  if (bluetoothInG2Menu) {
    if (gBluetoothShowingStatus) {
      displayG2StatusDetail();
    } else {
      displayG2Menu();
    }
    return;
  }
#endif
  
  // Show status detail screen or menu
  if (gBluetoothShowingStatus) {
    displayBluetoothStatusDetail();
    return;
  }
  
  // Draw menu items with scrolling (no separate status line - show in status detail)
  int visibleItems = getBluetoothMenuItemCount();
  
  // Clamp selection to visible range (in case BT was just turned off)
  if (bluetoothMenuSelection >= visibleItems) {
    bluetoothMenuSelection = visibleItems - 1;
  }
  
  // Calculate scrolling window (full content area for menu)
  const int menuStartY = OLED_CONTENT_START_Y;
  const int lineHeight = 8;
  const int maxVisibleMenuItems = OLED_CONTENT_HEIGHT / lineHeight;  // 43px / 8px = 5 items
  
  // Calculate scroll offset to keep selection visible
  static int scrollOffset = 0;
  if (bluetoothMenuSelection < scrollOffset) {
    scrollOffset = bluetoothMenuSelection;
  } else if (bluetoothMenuSelection >= scrollOffset + maxVisibleMenuItems) {
    scrollOffset = bluetoothMenuSelection - maxVisibleMenuItems + 1;
  }
  
  // Draw visible menu items
  for (int i = 0; i < maxVisibleMenuItems && (scrollOffset + i) < visibleItems; i++) {
    int itemIdx = scrollOffset + i;
    oledDisplay->setCursor(0, menuStartY + i * lineHeight);
    if (itemIdx == bluetoothMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->print(bluetoothMenuItems[itemIdx]);
    
    // Show state indicators inline
    if (itemIdx == 2) { // Start/Stop
      oledDisplay->print(gBLEState && gBLEState->initialized ? " *" : "");
#if ENABLE_G2_GLASSES
    } else if (itemIdx == 3) { // G2 Glasses
      if (isG2Connected()) oledDisplay->print(" *");
    } else if (itemIdx == 4) { // Advertising (shifted index with G2)
      if (gBLEState && gBLEState->connectionState == BLE_STATE_ADVERTISING) {
        oledDisplay->print(" *");
      }
#else
    } else if (itemIdx == 3) { // Advertising (original index without G2)
      if (gBLEState && gBLEState->connectionState == BLE_STATE_ADVERTISING) {
        oledDisplay->print(" *");
      }
#endif
    }
  }
  
  // Draw scroll indicators if needed
  if (scrollOffset > 0) {
    oledDisplay->setCursor(120, menuStartY);
    oledDisplay->print("\x18");  // Up arrow
  }
  if (scrollOffset + maxVisibleMenuItems < visibleItems) {
    oledDisplay->setCursor(120, menuStartY + (maxVisibleMenuItems - 1) * lineHeight);
    oledDisplay->print("\x19");  // Down arrow
  }
}

// Availability check for Bluetooth OLED mode
static bool bluetoothOLEDModeAvailable(String* outReason) {
  return true;  // Always show in menu
}

// Input handler for Bluetooth OLED mode - menu navigation
static bool bluetoothInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Use centralized navigation events (computed with proper debounce/auto-repeat)
  if (gNavEvents.up) {
    bluetoothMenuUp();
    return true;
  }
  if (gNavEvents.down) {
    bluetoothMenuDown();
    return true;
  }
  
  // A or X button: Execute action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executeBluetoothAction();
    return true;
  }
  
  // B button: Back
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
#if ENABLE_G2_GLASSES
    if (bluetoothInG2Menu) {
      bluetoothMenuBack();
      return true;
    }
#endif
    if (gBluetoothShowingStatus) {
      bluetoothMenuBack();
      return true;
    }
    // Return false to let main handler exit to menu
    return false;
  }
  
  return false;
}

// Bluetooth OLED mode entry
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry bluetoothOLEDModes[] = {
  {
    OLED_BLUETOOTH,          // mode enum
    "Bluetooth",             // menu name
    "bt_idle",               // icon name (Bluetooth glyph)
    displayBluetoothStatus,  // displayFunc
    bluetoothOLEDModeAvailable, // availFunc
    bluetoothInputHandler,   // inputFunc - X toggles BLE state
    true,                    // showInMenu
    45,                      // menuOrder (near ESP-NOW)
    nullptr                  // dynamic hints
  }
};

// Auto-register Bluetooth OLED mode
REGISTER_OLED_MODE_MODULE(bluetoothOLEDModes, sizeof(bluetoothOLEDModes) / sizeof(bluetoothOLEDModes[0]), "Bluetooth");

#endif // ENABLE_OLED_DISPLAY

// ============================================================================
// Bluetooth Streaming Extensions (merged from Bluetooth_Streaming.cpp)
// ============================================================================
// Data Pipeline and Event System - provides continuous data streaming and 
// event notifications over BLE.

// External sensor cache references
#if ENABLE_THERMAL_SENSOR
extern struct ThermalCache {
  SemaphoreHandle_t mutex;
  int16_t* thermalFrame;
  float* thermalInterpolated;
  float thermalMinTemp;
  float thermalMaxTemp;
  float thermalCenterTemp;
  int thermalHottestX;
  int thermalHottestY;
  uint32_t thermalLastUpdate;
  bool thermalDataValid;
  uint32_t thermalSeq;
} gThermalCache;
extern bool gThermalEnabled;
extern bool gThermalConnected;
#endif

#if ENABLE_TOF_SENSOR
extern struct TofCache {
  SemaphoreHandle_t mutex;
  struct TofObject {
    bool detected;
    int distance_mm;
    float distance_cm;
    uint8_t status;
    bool valid;
    float smoothed_distance_mm;
    float smoothed_distance_cm;
    bool hasHistory;
  } tofObjects[4];
  int tofTotalObjects;
  uint32_t tofLastUpdate;
  bool tofDataValid;
  uint32_t tofSeq;
} gTofCache;
extern bool gTofEnabled;
extern bool gTofConnected;
#endif

#if ENABLE_IMU_SENSOR
extern struct ImuCache {
  SemaphoreHandle_t mutex;
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  float heading, pitch, roll;
  uint32_t imuLastUpdate;
  bool imuDataValid;
  uint32_t imuSeq;
} gImuCache;
extern bool gImuEnabled;
extern bool gImuConnected;
#endif

// =============================================================================
// DATA STREAMING PIPELINE
// =============================================================================

bool blePushSensorData(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSensorDataChar) {
    return false;
  }
  
  pSensorDataChar->setValue((uint8_t*)jsonData, len);
  pSensorDataChar->notify();
  
  if (gBLEState) {
    gBLEState->sensorStreamCount++;
  }
  
  return true;
}

bool blePushSystemStatus(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSystemStatusChar) {
    return false;
  }
  
  pSystemStatusChar->setValue((uint8_t*)jsonData, len);
  pSystemStatusChar->notify();
  
  if (gBLEState) {
    gBLEState->systemStreamCount++;
  }
  
  return true;
}

bool blePushEvent(BLEEventType eventType, const char* message, const char* details) {
  if (!isBLEConnected() || !pEventNotifyChar) {
    return false;
  }
  
  // Build event JSON
  char eventJson[256];
  int pos = snprintf(eventJson, sizeof(eventJson),
                     "{\"type\":%d,\"msg\":\"%s\"", eventType, message);
  
  if (details && details[0] != '\0') {
    pos += snprintf(eventJson + pos, sizeof(eventJson) - pos,
                    ",\"details\":\"%s\"", details);
  }
  
  pos += snprintf(eventJson + pos, sizeof(eventJson) - pos,
                  ",\"ts\":%lu}", millis());
  
  pEventNotifyChar->setValue((uint8_t*)eventJson, pos);
  pEventNotifyChar->notify();
  
  if (gBLEState) {
    gBLEState->eventCount++;
  }
  
  return true;
}

// =============================================================================
// STREAM CONTROL
// =============================================================================

void bleEnableStream(uint8_t streamFlags) {
  if (!gBLEState) return;
  gBLEState->streamFlags |= streamFlags;
}

void bleDisableStream(uint8_t streamFlags) {
  if (!gBLEState) return;
  gBLEState->streamFlags &= ~streamFlags;
}

void bleSetStreamInterval(uint32_t sensorMs, uint32_t systemMs) {
  if (!gBLEState) return;
  gBLEState->sensorStreamInterval = sensorMs;
  gBLEState->systemStreamInterval = systemMs;
}

bool bleIsStreamEnabled(uint8_t streamFlag) {
  if (!gBLEState) return false;
  return (gBLEState->streamFlags & streamFlag) != 0;
}

// =============================================================================
// AUTO-STREAMING (Call from main loop)
// =============================================================================

static void buildSensorDataJSON(char* buf, size_t bufSize) {
  int pos = snprintf(buf, bufSize, "{\"sensors\":{");
  
  #if ENABLE_THERMAL_SENSOR
  if (gThermalEnabled && gThermalConnected && gThermalCache.mutex) {
    if (xSemaphoreTake(gThermalCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gThermalCache.thermalDataValid) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"thermal\":{\"min\":%.1f,\"max\":%.1f,\"center\":%.1f,\"valid\":true},",
                        gThermalCache.thermalMinTemp,
                        gThermalCache.thermalMaxTemp,
                        gThermalCache.thermalCenterTemp);
      }
      xSemaphoreGive(gThermalCache.mutex);
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gTofEnabled && gTofConnected && gTofCache.mutex) {
    if (xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"tof\":{\"dist_mm\":%d,\"valid\":true},",
                        gTofCache.tofObjects[0].distance_mm);
      }
      xSemaphoreGive(gTofCache.mutex);
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gImuEnabled && gImuConnected && gImuCache.mutex) {
    if (xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gImuCache.imuDataValid) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"imu\":{\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"valid\":true},",
                        gImuCache.heading,
                        gImuCache.pitch,
                        gImuCache.roll);
      }
      xSemaphoreGive(gImuCache.mutex);
    }
  }
  #endif
  
  // Remove trailing comma if any sensors were added
  if (pos > 12 && buf[pos - 1] == ',') {
    pos--;
  }
  
  pos += snprintf(buf + pos, bufSize - pos, "},\"ts\":%lu}", millis());
  buf[pos] = '\0';
}

static void buildSystemStatusJSON(char* buf, size_t bufSize) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t freePSRAM = ESP.getFreePsram();
  uint32_t uptime = millis() / 1000;
  
  snprintf(buf, bufSize,
           "{\"system\":{\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,\"uptime\":%lu},\"ts\":%lu}",
           (unsigned long)freeHeap,
           (unsigned long)minHeap,
           (unsigned long)freePSRAM,
           (unsigned long)uptime,
           (unsigned long)millis());
}

void bleUpdateStreams() {
  if (!gBLEState || !isBLEConnected()) return;
  
  uint32_t now = millis();

  // Maintain BLE CLI sessions
  bleSessionTick();
  
  // Stream sensor data
  if (bleIsStreamEnabled(BLE_STREAM_SENSORS)) {
    if (now - gBLEState->lastSensorStream >= gBLEState->sensorStreamInterval) {
      char sensorBuf[512];
      buildSensorDataJSON(sensorBuf, sizeof(sensorBuf));
      blePushSensorData(sensorBuf, strlen(sensorBuf));
      gBLEState->lastSensorStream = now;
    }
  }
  
  // Stream system status
  if (bleIsStreamEnabled(BLE_STREAM_SYSTEM)) {
    if (now - gBLEState->lastSystemStream >= gBLEState->systemStreamInterval) {
      char systemBuf[256];
      buildSystemStatusJSON(systemBuf, sizeof(systemBuf));
      blePushSystemStatus(systemBuf, strlen(systemBuf));
      gBLEState->lastSystemStream = now;
    }
  }
}

#endif // ENABLE_BLUETOOTH
