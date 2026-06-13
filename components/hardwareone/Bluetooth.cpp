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
#include "System_PollPause.h"   // PollPauseGuard — quiesce sensor polling during BLE init
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
#include "System_Mutex.h"   // SensorCacheGuard — used by the G2 sensor-stream paths below
#include "System_SelfDevice.h"  // SelfDevice::firmwareVersion() for the Device Info BLE characteristic
#include "System_Settings.h"
#include "System_BleSecureChannel.h"  // app-layer Secure Channel v1 (replaces BLE link-layer bonding)
#include "System_AuthIdentity.h"      // currentAuthContext() — gate blesecret to trusted transports
#include <cctype>                     // passphrase complexity policy

#include <esp_gatts_api.h>
#include <esp_bt.h>            // esp_bt_controller_get_status() — used by isBLERunning()
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
// BLE idle-logout window now comes from the shared per-transport policy
// (gSettings.sessionIdleBle via sessionIdleExpired(SOURCE_BLUETOOTH, …) in
// System_User.cpp). lastActivityMs is stamped only on real inbound commands
// (bleMarkActivity at the command handler), never on outbound notifies, so
// automatic chatter can't keep a session alive.

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
  macToDisplay(mac, buf, 18);  // canonical DISPLAY form (System_Utils.h)
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
    bleScReset(param->connect.conn_id);  // fresh secure-channel state for this connection

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
      bleScReset(param->disconnect.conn_id);  // wipe secure-channel keys on disconnect
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

// True if the command line is a filesystem-browser verb. These move file
// contents and paths over the link, so they must run only over the app-layer
// Secure Channel — see the gate in processBleCommandLine. The boundary check
// (NUL or space after the verb) keeps "files" from matching "fileread" etc.
static bool bleIsFileBrowserCommand(const char* line) {
  static const char* kVerbs[] = {
    "files", "fileread", "filewrite", "fileview", "filecreate",
    "filedelete", "filerename", "mkdir", "rmdir"
  };
  for (const char* v : kVerbs) {
    size_t n = strlen(v);
    if (strncasecmp(line, v, n) == 0 && (line[n] == '\0' || line[n] == ' ')) return true;
  }
  return false;
}

// Command tail — runs on cmd_exec_task (deep stack), called directly for plaintext
// commands or by the deferred secure-channel handler after a DATA frame is decrypted.
// `data` is already plaintext here (no secure-channel parsing in this function).
static void processBleCommandLine(uint16_t connId, const char* data, size_t len) {
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

  // Secure Channel gate for the file browser. File contents and paths must never
  // cross the link in cleartext, so these commands require an established Secure
  // Channel regardless of the global plaintext setting. When the command arrived
  // as a decrypted DATA frame the channel is already established (bleScEstablished
  // is true), so this only rejects the cleartext path. The operator must have set
  // a `blesecret` for the app to be able to establish the channel.
  if (bleIsFileBrowserCommand(cmdStart) && !bleScEstablished(connId)) {
    const char* msg = "{\"success\":false,\"error\":\"secure_channel_required\"}";
    sendBLEResponseToConn(connId, msg, strlen(msg));
    return;
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

// Defer secure-channel work off BTC_TASK (8 KB) onto cmd_exec_task (deep stack) —
// the X25519/HKDF handshake won't fit comfortably on the BLE callback stack. Mirrors
// how ESP-NOW pushes radio-callback crypto onto cmd_exec via submitDeferredToCmdExec.
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

struct BleScDeferred { uint16_t connId; uint16_t len; uint8_t buf[517]; };

// Runs on cmd_exec_task. Frees its own arg (per submitDeferredToCmdExec contract).
static void bleScDeferredInbound(void* arg) {
  BleScDeferred* d = (BleScDeferred*)arg;
  char   plain[512];
  size_t pl = 0;
  BleScResult r = bleScHandleInbound(d->connId, d->buf, d->len, plain, sizeof(plain), &pl);
  if (r == BLE_SC_PLAINTEXT_READY) {
    processBleCommandLine(d->connId, plain, pl);   // execute the decrypted command
  }
  free(d);
}

// Entry from the GATT write callback — runs on BTC_TASK (8 KB, time-critical). Keep it
// light: secure-channel frames are copied + deferred to cmd_exec (where the handshake
// crypto runs); plaintext commands go straight to the command tail (which itself defers
// the heavy command execution via submitCommandAsync).
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {
  const uint8_t t = (len > 0) ? (uint8_t)data[0] : 0;
  const bool isFrame = (t == 0x01 /*HELLO*/ || t == 0x03 /*CONFIRM*/ || t == 0x10 /*DATA*/);
  if (isFrame && len <= sizeof(((BleScDeferred*)0)->buf)) {
    BleScDeferred* d = (BleScDeferred*)ps_alloc(sizeof(BleScDeferred), AllocPref::PreferPSRAM, "ble.sc.rx");
    if (d) {
      d->connId = connId; d->len = (uint16_t)len; memcpy(d->buf, data, len);
      if (!submitDeferredToCmdExec(bleScDeferredInbound, d)) free(d);
    }
    return;
  }
  // Plaintext path: refuse if the secure channel is required, else run the command.
  if (bleScRequired()) {
    const char* msg = "Secure channel required — connect with the encrypted app.";
    sendBLEResponseToConn(connId, msg, strlen(msg));
    return;
  }
  processBleCommandLine(connId, data, len);
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

  // Raise the GATT MTU ceiling so command responses aren't capped at the BLE
  // default of 23 (= 20 usable bytes). 517 is the BLE max; payload becomes
  // MTU-3 = 514 bytes, which fully covers the 512-byte gBLEOutputBuffer flush
  // (System_Debug.cpp) — so the existing line-batched output streams to a phone
  // untruncated without any per-notification chunking. The client must also
  // request a large MTU; the negotiated value is min(client, this). NOTE: this
  // is a global Bluedroid setting — the G2 client path sets its own (244).
  BLEDevice::setMTU(517);

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
  pFirmwareChar->setValue(SelfDevice::firmwareVersion());  // real build version, not a hardcoded string
  
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

  // Secure Channel: create the tx mutex, then pre-derive the PSK here (init runs on a
  // large-stack task) so the per-connection handshake never pays the PBKDF2 cost.
  bleScInit();
  bleScWarmPsk();

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

// RAW notify to the RESPONSE characteristic — the ONLY function that actually touches
// the radio for output. The secure-channel encryptor and the handshake replies emit
// through this; it does NOT encrypt. Everything that is *output* (results, streamed
// CLI text) must go through sendBLEResponse()/sendBLEResponseToConn() instead, which
// encrypt when a channel is up.
bool bleRawNotify(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "bleRawNotify dropped (connected=%d char=%p)",
               isBLEConnected(), (void*)pCmdResponseChar);
    return false;
  }
  pCmdResponseChar->setValue((uint8_t*)data, len);
  pCmdResponseChar->notify();
  gBLEState->responsesSent++;

  #if ENABLE_OLED_DISPLAY
  {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "TX:%.*s", (int)(BLE_MSG_MAX_LEN - 4), data ? data : "");
    bleAddMessageToHistory(tagged);
  }
  #endif
  return true;
}

// Output chokepoint for broadcast/streamed device output (the debug-drain "path A" via
// System_Debug.cpp, plus any other caller). If a Secure Channel is established for a
// connected client, encrypt+frame the output for it — so STREAMED CLI output (menus,
// help, reports) is confidential, not just the short command result. In secure-required
// mode with no established channel, DROP rather than leak plaintext over the air.
bool sendBLEResponse(const char* data, size_t len) {
  if (!data || len == 0) return false;
  if (gBLEState) {
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (gBLEState->connections[i].active &&
          bleScEstablished(gBLEState->connections[i].connId)) {
        return bleScSendEncrypted(gBLEState->connections[i].connId, data, len);
      }
    }
  }
  if (bleScRequired()) return false;     // never emit plaintext on a must-be-secure link
  return bleRawNotify(data, len);        // plaintext mode (no channel) — as before
}

bool sendBLEResponseToConn(uint16_t connId, const char* data, size_t len) {
  if (!data || len == 0) return false;

  // Established Secure Channel → frame+encrypt for THIS connection (shares the same
  // per-connection txCtr as path A, serialized by the channel's tx mutex).
  if (bleScEstablished(connId)) {
    return bleScSendEncrypted(connId, data, len);
  }
  if (bleScRequired()) return false;     // don't leak plaintext for a secure-required link

  // Plaintext mode (no channel): original behavior.
  if (!gBLEState || gBLEState->activeConnectionCount <= 1) {
    return bleRawNotify(data, len);
  }
  if (!ensureDebugBuffer()) {
    return bleRawNotify(data, len);
  }
  char* tagged = getDebugBuffer();
  snprintf(tagged, 1024, "[ble conn:%u] %.*s", (unsigned)connId, (int)len, data);
  return bleRawNotify(tagged, strlen(tagged));
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

    if (sessionIdleExpired(SOURCE_BLUETOOTH, gBLEState->connections[i].lastActivityMs, now)) {
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
  // True whenever the BT controller is up, regardless of which sub-stack
  // is using it. Mirrors how the WiFi/HTTP modules report runtime liveness
  // (WiFi.status() == WL_CONNECTED, server != nullptr).
  //
  // The previous implementation only checked `gBLEState->initialized`,
  // which is server-mode state. In g2-client mode (bluetoothMode=1) the
  // BLE radio is active and connected to the glasses/ring, but the
  // server-mode struct is never initialized — so the settings UI showed
  // "Disabled" while BT was clearly running. Checking the controller
  // status covers both modes plus any future sub-stack.
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    return true;
  }
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
  // Pause sensor polling during BLE init to avoid interrupt contention (RAII —
  // resumes on every return path; the trailing string checks do no I2C work).
  PollPauseGuard pollGuard;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete

  bool initOk = initBluetooth();
  bool advOk = initOk ? startBLEAdvertising() : false;

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
      // Append the signed-in user once this connection authenticates, so the
      // status text — and the web "Connected Clients" card, which parses this
      // line — shows who's logged in, not just the device type ("Unknown").
      char userSuffix[72];
      if (gBLEState->connections[i].authed && gBLEState->connections[i].user.length() > 0) {
        snprintf(userSuffix, sizeof(userSuffix), " (%s)", gBLEState->connections[i].user.c_str());
      } else {
        userSuffix[0] = '\0';
      }
      written = snprintf(buf + offset, remaining, "[%d] %s%s\n    MAC: %s | %lu sec | %lu cmds\n",
               i, gBLEState->connections[i].deviceName.c_str(), userSuffix,
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

// ---- Secure Channel (app-layer encryption) commands ----
// Passphrase policy: >=10 chars with an upper, lower, digit, and symbol.
static const char* blePassphrasePolicyError(const String& p) {
  if (p.length() < 10) return "be at least 10 characters";
  bool up = false, lo = false, di = false, sy = false;
  for (size_t i = 0; i < p.length(); i++) {
    unsigned char c = (unsigned char)p[i];
    if (isupper(c)) up = true; else if (islower(c)) lo = true;
    else if (isdigit(c)) di = true; else if (ispunct(c)) sy = true;
  }
  if (!up) return "include an uppercase letter";
  if (!lo) return "include a lowercase letter";
  if (!di) return "include a digit";
  if (!sy) return "include a symbol";
  return nullptr;
}

static const char* cmd_blesecret(const String& argsInput) {
  String a = argsInput; a.trim();
  if (a.length() == 0) {
    return gSettings.bleSecureChannelSecret.length()
      ? "BLE secure-channel secret is SET (hidden). 'blesecret clear' to remove."
      : "BLE secure-channel secret NOT set. Set one: blesecret <passphrase>";
  }
  // Provision the master secret only over a trusted local transport — never let it
  // traverse a (plaintext) BLE link where it could be sniffed.
  if (currentAuthContext().transport == SOURCE_BLUETOOTH) {
    return "Set the secure-channel passphrase via serial, OLED, or web — not over Bluetooth.";
  }
  if (a.equalsIgnoreCase("clear")) {
    setSetting(gSettings.bleSecureChannelSecret, String(""));
    bleScInvalidatePsk();
    return "BLE secure-channel secret cleared. (Encryption no longer enforced.)";
  }
  if (const char* need = blePassphrasePolicyError(a)) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "Error: passphrase must %s (>=10 with upper, lower, digit, symbol).", need);
    return buf;
  }
  setSetting(gSettings.bleSecureChannelSecret, a);
  bleScInvalidatePsk();
  bleScWarmPsk();  // derive PSK now (cmd_exec task, big stack) so BTC_TASK handshake doesn't
  return gSettings.bleRequireSecureChannel
    ? "Secret set — encryption is now REQUIRED. Enter the SAME passphrase in the app. ('blesecure off' to allow plaintext.)"
    : "Secret set. Run 'blesecure on' to require encryption, and enter the SAME passphrase in the app.";
}

static const char* cmd_blesecure(const String& argsInput) {
  String a = argsInput; a.trim(); a.toLowerCase();
  if (a.length() == 0) {
    return gSettings.bleRequireSecureChannel ? "BLE secure channel: REQUIRED"
                                             : "BLE secure channel: optional (plaintext allowed)";
  }
  bool on  = (a == "on"  || a == "1" || a == "true"  || a == "yes");
  bool off = (a == "off" || a == "0" || a == "false" || a == "no");
  if (!on && !off) return "Usage: blesecure [on|off]";
  if (on && gSettings.bleSecureChannelSecret.length() == 0)
    return "Set a secret first: blesecret <passphrase>";
  setSetting(gSettings.bleRequireSecureChannel, on);
  return on ? "BLE secure channel REQUIRED — plaintext commands now refused."
            : "BLE secure channel optional — plaintext allowed.";
}

// Boot-time security nudge: encryption is wanted (bleRequireSecureChannel, default on)
// but no passphrase is set, so the channel can't be enforced and BLE is PLAINTEXT.
// Printed as the last lines of boot so the operator can't miss it. No-op once a secret
// is set, or if the user explicitly opted into plaintext (blesecure off), or in G2 mode.
void bleSecurityBootNotice() {
  if (!gSettings.bluetoothAutoStart) return;                    // BT not running
  if (gSettings.bleMode != BLE_MODE_SERVER) return;             // G2 client mode: N/A
  if (!gSettings.bleRequireSecureChannel) return;               // user chose plaintext
  if (gSettings.bleSecureChannelSecret.length() > 0) return;    // already provisioned
  broadcastOutput("");
  broadcastOutput("************************* BLE SECURITY *************************");
  broadcastOutput("Bluetooth is UNENCRYPTED — no secure-channel passphrase is set.");
  broadcastOutput("Any phone in range can connect in plaintext. To secure it, set one:");
  broadcastOutput("   blesecret <passphrase>   (serial/OLED/web; >=10 chars, upper+");
  broadcastOutput("                             lower+digit+symbol), then enter the SAME");
  broadcastOutput("                             passphrase in the app.");
  broadcastOutput("   (or 'blesecure off' to intentionally allow plaintext.)");
  broadcastOutput("***************************************************************");
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
  // Structured path: BLE config + live state, one verbatim JSON blob via the
  // return value. No broadcastOutput. ArduinoJson escapes the user-settable
  // device name correctly. No secrets emitted.
  if (argWantsJson(argsInput)) {
    bool init = (gBLEState && gBLEState->initialized);
    PSRAM_JSON_DOC(doc);
    doc["v"]              = 1;
    doc["deviceName"]     = gSettings.bleDeviceName;
    doc["txPower"]        = gSettings.bleTxPower;
    doc["autoStart"]      = gSettings.bluetoothAutoStart;
    doc["requireAuth"]    = gSettings.bluetoothRequireAuth;
    doc["secureChannelRequired"] = bleScRequired();
    doc["initialized"]    = init;
    doc["state"]          = getBLEStateString();
    doc["connections"]    = init ? gBLEState->activeConnectionCount : 0;
    doc["maxConnections"] = BLE_MAX_CONNECTIONS;
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "bleinfo.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 512);
    return jbuf;
  }

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

  // App-layer Secure Channel v1 (X25519+PSK+ChaCha20-Poly1305; no BLE bonding)
  { "blesecret", "Set/clear the BLE Secure Channel passphrase: blesecret <phrase|clear>.", true, cmd_blesecret, "Usage: blesecret <passphrase|clear>" },
  { "blesecure", "Require app-layer BLE encryption [on|off].",                            true, cmd_blesecure, "Usage: blesecure [on|off]" },

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
  { "bluetoothDeviceName", SETTING_STRING, &gSettings.bleDeviceName, 0, 0, "HardwareOne", 0, 0, "Device Name", nullptr, false, nullptr, nullptr },
  { "bluetoothTxPower",      SETTING_INT,    &gSettings.bleTxPower,            3, 0, nullptr, 0, 7, "TX Power (0-7)", nullptr, false, nullptr, "bletxpower" },
  { "bluetoothMode",         SETTING_INT,    &gSettings.bleMode,               0,    0, nullptr, 0, 1, "Mode (0=server, 1=g2)", nullptr, false, nullptr, "blemode" },
  { "bleRequireSecureChannel", SETTING_BOOL, &gSettings.bleRequireSecureChannel, true, 0, nullptr, 0, 1, "Require Secure Channel", nullptr, false, nullptr, "blesecure" },
  { "bleSecureChannelSecret",  SETTING_STRING, &gSettings.bleSecureChannelSecret, 0, 0, "", 0, 0, "Secure Channel Secret", nullptr, true, nullptr, "blesecret" }
};

const size_t bluetoothSettingsCount = sizeof(bluetoothSettingsEntries) / sizeof(bluetoothSettingsEntries[0]);

// Register Bluetooth settings module
// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule bluetoothSettingsModule = {
  "bluetooth",
  "network.bluetooth",
  bluetoothSettingsEntries,
  bluetoothSettingsCount,
  isBLERunning,
  "Bluetooth Classic and BLE"
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

// Accessor for the OLED status screen (now in OLED_Mode_Bluetooth.cpp). Fills
// out[] with up to maxLines recent messages, newest first; returns the count.
// Keeps bleMessageHistory/Count/Head file-local instead of exposing the buffer.
int bleGetRecentMessages(const char* out[], int maxLines) {
  int n = (bleMessageCount < maxLines) ? bleMessageCount : maxLines;
  for (int i = 0; i < n; i++) {
    int idx = (int)bleMessageHead - 1 - i;
    while (idx < 0) idx += BLE_MSG_HISTORY_SIZE;
    out[i] = bleMessageHistory[idx];
  }
  return n;
}

// The Bluetooth OLED mode (menus, status, G2 submenu) moved to its own file,
// OLED_Mode_Bluetooth.cpp, to match the one-file-per-mode layout.

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
  // The Data-service streams are NOT part of the Secure Channel (separate chars).
  // When encryption is required, suppress them rather than emit plaintext over the air.
  if (bleScRequired()) return false;

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
  if (bleScRequired()) return false;  // not on the Secure Channel — don't leak plaintext

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
  if (bleScRequired()) return false;  // not on the Secure Channel — don't leak plaintext

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
  if (gThermalEnabled && gThermalConnected) {
    SensorCacheGuard g(gThermalCache.mutex, pdMS_TO_TICKS(10), "ble.thermalStream");
    if (g.held && gThermalCache.thermalDataValid) {
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"thermal\":{\"min\":%.1f,\"max\":%.1f,\"center\":%.1f,\"valid\":true},",
                      gThermalCache.thermalMinTemp,
                      gThermalCache.thermalMaxTemp,
                      gThermalCache.thermalCenterTemp);
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gTofEnabled && gTofConnected) {
    SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(10), "ble.tofStream");
    if (g.held && gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"tof\":{\"dist_mm\":%d,\"valid\":true},",
                      gTofCache.tofObjects[0].distance_mm);
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gImuEnabled && gImuConnected) {
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "ble.imuStream");
    if (g.held && gImuCache.imuDataValid) {
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"imu\":{\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"valid\":true},",
                      gImuCache.heading,
                      gImuCache.pitch,
                      gImuCache.roll);
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
