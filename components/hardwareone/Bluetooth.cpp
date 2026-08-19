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
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_PollPause.h"   // PollPauseGuard — quiesce sensor polling during BLE init
#include "G2_Glasses.h"  // Header provides stubs when ENABLE_G2_GLASSES=0, so blemode CLI compiles either way
#include "G2_Ring.h"     // g2RingInvalidateLink before BLEDevice::deinit
#include "System_Utils.h"
#include "BLE_Peers.h"        // peer registry + cmd_bleautoreconnect / cmd_blepeers

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
#include "System_OTA.h"     // encrypted BLE OTA binary-frame sink

// Sensor caches for the BLE sensor-stream path (buildSensorDataJSON). Pull in the
// REAL headers — never hand-copy these structs. This file used to re-declare
// ThermalCache/TofCache/ImuCache locally with stale layouts; because it never
// included these headers the compiler could not see the conflict, so it linked to
// the real symbols and read them at the WRONG OFFSETS (the IMU stream published
// imuTemp as "heading", never sent roll, and gated validity on a millis() byte).
// System_SensorStubs.h supplies the types/globals when a sensor is compiled out.
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"   // ThermalCache, gThermalCache, gThermalRunning/Connected
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"   // TofCache, gTofCache, gTofRunning/Connected
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"     // ImuCache, gImuCache, gImuRunning/Connected
#endif
#include "System_SensorStubs.h"     // stubs for disabled sensors
#include "System_SelfDevice.h"  // SelfDevice::firmwareVersion() for the Device Info BLE characteristic
#include "System_Settings.h"
#include "System_BleSecureChannel.h"  // app-layer Secure Channel v1 (replaces BLE link-layer bonding)
#include "System_AuthIdentity.h"      // currentAuthContext() — gate blesecret to trusted transports
#include <atomic>
#include <cctype>                     // passphrase complexity policy
#include <freertos/semphr.h>

#include <esp_gatts_api.h>
#include <esp_bt.h>            // esp_bt_controller_get_status()
#include <esp_bt_main.h>       // esp_bluedroid_get_status()
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

// Application-role transaction token. This is intentionally a tiny task-owner
// state machine rather than a mutex callbacks could block on: Bluedroid may
// invoke GAP/GATT callbacks while a role transition is waiting for host
// disable, so callback acquisition would create an inversion. Public role
// entry points claim/reject; nested calls from the same task are recursive.
static portMUX_TYPE sBleRoleTransitionMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sBleRoleTransitionOwner = nullptr;
static uint8_t sBleRoleTransitionDepth = 0;
static BleRoleTransition sBleRoleTransition = BleRoleTransition::IDLE;
static bool sBleStackLifecycleFaulted = false;

class BleRoleTransitionClaim {
 public:
  explicit BleRoleTransitionClaim(BleRoleTransition state)
      : held_(bleRoleTransitionBegin(state)) {}
  ~BleRoleTransitionClaim() { if (held_) bleRoleTransitionEnd(); }
  explicit operator bool() const { return held_; }
 private:
  bool held_;
};

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

namespace {
// Serializes the app-level BLE connection table with its boot-local session
// epochs.  A recursive mutex keeps the small public helpers composable while
// making connect/disconnect, login binding and directed response admission one
// transaction.  These callbacks run in task context (BTC_TASK), not an ISR.
StaticSemaphore_t sBleLifecycleMutexStorage;
SemaphoreHandle_t sBleLifecycleMutex = nullptr;

bool bleInitLifecycleMutex() {
  if (sBleLifecycleMutex) return true;
  sBleLifecycleMutex =
      xSemaphoreCreateRecursiveMutexStatic(&sBleLifecycleMutexStorage);
  return sBleLifecycleMutex != nullptr;
}

class BleLifecycleGuard {
 public:
  explicit BleLifecycleGuard(TickType_t wait = portMAX_DELAY)
      : locked_(sBleLifecycleMutex &&
                xSemaphoreTakeRecursive(sBleLifecycleMutex, wait) == pdTRUE) {}
  ~BleLifecycleGuard() {
    if (locked_) xSemaphoreGiveRecursive(sBleLifecycleMutex);
  }
  explicit operator bool() const { return locked_; }

 private:
  bool locked_;
};

// Captured from the Arduino-BLE custom GATTS event tap.  The wrapper's public
// notify() broadcasts to every peer; the IDF send primitive needs this
// interface id to target exactly one connection.
std::atomic<int> sBleGattsIf{ESP_GATT_IF_NONE};

void bleGattsEventTap(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf,
                      esp_ble_gatts_cb_param_t* param) {
  if (event == ESP_GATTS_REG_EVT && param && param->reg.status == ESP_GATT_OK) {
    sBleGattsIf.store(static_cast<int>(gattsIf), std::memory_order_release);
  } else if (event == ESP_GATTS_UNREG_EVT) {
    sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  }
}

std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];

int findConnectionSlotByConnIdLocked(uint16_t connId) {
  if (!gBLEState) return -1;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBLEState->connections[i].active && gBLEState->connections[i].connId == connId) {
      return i;
    }
  }
  return -1;
}

bool bleConnectionEpochMatchesLocked(uint16_t connId,
                                     TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  const int slot = findConnectionSlotByConnIdLocked(connId);
  return slot >= 0 &&
         sBleSessionEpoch[slot].load(std::memory_order_acquire) == epoch;
}

// The caller holds sBleLifecycleMutex.
void bleInvalidateSessionSlotLocked(int slot) {
  if (slot < 0 || slot >= BLE_MAX_CONNECTIONS) return;
  TransportSessionEpoch old =
      sBleSessionEpoch[slot].exchange(0, std::memory_order_acq_rel);
  transportSessionClose(SOURCE_BLUETOOTH, old);
}

// The caller holds sBleLifecycleMutex.
TransportSessionEpoch blePublishSessionSlotLocked(int slot) {
  if (slot < 0 || slot >= BLE_MAX_CONNECTIONS) return 0;
  bleInvalidateSessionSlotLocked(slot);
  TransportSessionEpoch epoch = transportSessionOpen(SOURCE_BLUETOOTH);
  if (epoch != 0) {
    sBleSessionEpoch[slot].store(epoch, std::memory_order_release);
  }
  return epoch;
}
}  // namespace

TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return 0;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0) return 0;
  return sBleSessionEpoch[slot].load(std::memory_order_acquire);
}

bool bleSessionEpochMatches(uint16_t connId, TransportSessionEpoch epoch) {
  return epoch != 0 && bleSessionEpochForConnection(connId) == epoch;
}

bool bleSessionEpochIsLive(TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  BleLifecycleGuard guard;
  if (!guard) return false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    if (gBLEState && gBLEState->connections[i].active &&
        sBleSessionEpoch[i].load(std::memory_order_acquire) == epoch) {
      return true;
    }
  }
  return false;
}

// BLE idle-logout window now comes from the shared per-transport policy
// (gSettings.sessionIdleBle via sessionIdleExpired(SOURCE_BLUETOOTH, …) in
// System_User.cpp). lastActivityMs is stamped only on real inbound commands
// (bleMarkActivity at the command handler), never on outbound notifies, so
// automatic chatter can't keep a session alive.

static void bleMarkActivity(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot >= 0) {
    gBLEState->connections[slot].lastActivityMs = millis();
    BLE_DEBUGF(DEBUG_BLE_DATA, "Activity heartbeat conn=%u slot=%d", (unsigned)connId, slot);
  }
}

void bleClearConnectionByConnId(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0) return;

  // Close the epoch before making this slot reusable.  Login/result jobs that
  // captured it can no longer bind or emit, even if connId is reused quickly.
  bleInvalidateSessionSlotLocked(slot);
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
  BleLifecycleGuard guard;
  if (!guard) return false;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0) return false;
  if (!gBLEState->connections[slot].authed) return false;
  outUser = gBLEState->connections[slot].user;
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session authed lookup conn=%u user='%s'", (unsigned)connId, outUser.c_str());
  return outUser.length() > 0;
}

bool bleGetAuthenticatedUser(uint16_t connId, String& outUser) {
  return bleIsAuthed(connId, outUser);
}

// Peer MAC for audit `ip` (e.g. "ble:AA:BB:CC:DD:EE:FF"). Falls back to
// "ble" if the connection slot is gone mid-command.
static String blePeerIpTag(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return String("ble");
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0 || !gBLEState) return String("ble");
  char macStr[18];
  macToDisplay(gBLEState->connections[slot].deviceAddr, macStr, sizeof(macStr));
  char ipBuf[28];
  snprintf(ipBuf, sizeof(ipBuf), "ble:%s", macStr);
  return String(ipBuf);
}

// Identity for BLE-submitted commands. When require-auth is off and nobody
// has logged in on this conn, stamp reserved "AuthBypass" (same as serial /
// OLED) so authorizeCommand accepts the line and audit reads
// AuthBypass@bluetooth with the peer MAC in ip — not a blank user.
static bool bleFillCommandAuth(uint16_t connId, AuthContext& auth, bool forLogin,
                               TransportSessionEpoch& epochOut) {
  epochOut = bleSessionEpochForConnection(connId);
  if (epochOut == 0) return false;
  auth.transport = SOURCE_BLUETOOTH;
  auth.path = forLogin ? "/ble/login" : "/ble/cli";
  auth.ip = blePeerIpTag(connId);
  auth.sid = String(connId);
  auth.opaque = nullptr;
  if (forLogin) {
    // Pre-auth: empty user is allowed only for the login command itself
    // (see authorizeCommand). Peer MAC still lands in ip for the attempt log.
    auth.user = "";
    return true;
  }
  String u;
  if (bleIsAuthed(connId, u)) {
    auth.user = u;
  } else if (!gSettings.bleRequireAuth) {
    auth.user = "AuthBypass";
  } else {
    auth.user = "";
  }
  return true;
}

static TransportSessionEpoch bleLogout(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return 0;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0) return 0;
  bleInvalidateSessionSlotLocked(slot);
  gBLEState->connections[slot].authed = false;
  gBLEState->connections[slot].user = "";
  const TransportSessionEpoch epoch =
      blePublishSessionSlotLocked(slot);  // physical connection remains live
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session logout conn=%u slot=%d", (unsigned)connId, slot);
  return epoch;
}

bool bleHasAuthenticatedSession() {
  BleLifecycleGuard guard;
  if (!guard) return false;
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
  BleLifecycleGuard guard;
  if (!guard) return false;
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
  BleLifecycleGuard guard;
  if (!guard) return 0;
  if (!gBLEState || username.length() == 0) return 0;
  int revoked = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    if (!gBLEState->connections[i].user.equalsIgnoreCase(username)) continue;
    bleInvalidateSessionSlotLocked(i);
    gBLEState->connections[i].authed = false;
    gBLEState->connections[i].user = "";
    (void)blePublishSessionSlotLocked(i);
    revoked++;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked %d BLE sessions for user '%s'", revoked, username.c_str());
  return revoked;
}

int bleRevokeAllSessions() {
  BleLifecycleGuard guard;
  if (!guard) return 0;
  if (!gBLEState) return 0;
  int revoked = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBLEState->connections[i].active) continue;
    if (!gBLEState->connections[i].authed) continue;
    bleInvalidateSessionSlotLocked(i);
    gBLEState->connections[i].authed = false;
    gBLEState->connections[i].user = "";
    (void)blePublishSessionSlotLocked(i);
    revoked++;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked all BLE sessions (%d total)", revoked);
  return revoked;
}

void bleAuthPolicyChanged() {
  BleLifecycleGuard guard;
  if (!guard || !gBLEState) return;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    if (!gBLEState->connections[i].active ||
        gBLEState->connections[i].authed) {
      continue;
    }
    // Named authenticated sessions already received a fresh epoch at login.
    // Replace only bypass/pre-auth incarnations so queued work admitted under
    // an older policy cannot revive after a later on/off transition.
    bleInvalidateSessionSlotLocked(i);
    (void)blePublishSessionSlotLocked(i);
  }
}

// Bind an already-validated user to this BLE connection (password verified elsewhere).
static bool bleBindSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const String& user,
                           TransportSessionEpoch& boundEpochOut) {
  boundEpochOut = 0;
  BleLifecycleGuard guard;
  if (!guard) return false;
  int slot = findConnectionSlotByConnIdLocked(connId);
  if (slot < 0 || !gBLEState ||
      sBleSessionEpoch[slot].load(std::memory_order_acquire) != expectedEpoch) {
    return false;
  }
  bleInvalidateSessionSlotLocked(slot);
  gBLEState->connections[slot].authed = true;
  gBLEState->connections[slot].user = user;
  gBLEState->connections[slot].lastActivityMs = millis();
  boundEpochOut = blePublishSessionSlotLocked(slot);
  if (boundEpochOut == 0) {
    gBLEState->connections[slot].authed = false;
    gBLEState->connections[slot].user = "";
    return false;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Session bound conn=%u slot=%d user='%s'", (unsigned)connId, slot, user.c_str());
  return true;
}

static void bleSendAuthRequired(uint16_t connId,
                                TransportSessionEpoch expectedEpoch) {
  static const char* msg = "Authentication required. Use: login <username> <password>";
  BLE_DEBUGF(DEBUG_BLE_CORE, "Auth required notice conn=%u", (unsigned)connId);
  sendBLEResponseToSession(connId, expectedEpoch, msg, strlen(msg));
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
static void copyMacUpper(char* dst, size_t dstCap, const char* src) {
  if (src && src[0]) {
    strncpy(dst, src, dstCap - 1);
    dst[dstCap - 1] = '\0';
    for (char* p = dst; *p; p++) *p = toupper(*p);
  } else {
    dst[0] = '\0';
  }
}
static void bleUpdateMACCache() {
  BlePeerSavedTargetSnapshot glasses{};
  BlePeerSavedTargetSnapshot ring{};
  BlePeerSavedTargetSnapshot phone{};
  (void)blePeerSavedTargetSnapshot(BLE_PEER_G2_GLASSES, glasses);
  (void)blePeerSavedTargetSnapshot(BLE_PEER_R1_RING, ring);
  (void)blePeerSavedTargetSnapshot(BLE_PEER_PHONE, phone);
  copyMacUpper(gBLEMACCache.leftMAC,  sizeof(gBLEMACCache.leftMAC),
               glasses.target.mac1);
  copyMacUpper(gBLEMACCache.rightMAC, sizeof(gBLEMACCache.rightMAC),
               glasses.target.mac2);
  copyMacUpper(gBLEMACCache.ringMAC,  sizeof(gBLEMACCache.ringMAC),
               ring.target.mac1);
  copyMacUpper(gBLEMACCache.phoneMAC, sizeof(gBLEMACCache.phoneMAC),
               phone.target.mac1);


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
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState || !param) return;
    
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
    // Publish only after the complete connection identity exists. This token
    // fences login jobs, queued commands and their replies from connId reuse.
    (void)blePublishSessionSlotLocked(slot);
    
    gBLEState->activeConnectionCount++;
    gBLEState->totalConnections++;
    gBLEState->connectionState = BLE_STATE_CONNECTED;
    
    // Defer logging to task context
    gBLEState->deferredConnectSlot = slot;
    gBLEState->deferredConnectPending = true;

    // Bus event inline (bounded spinlock copy, BTC-safe). Posted here rather
    // than the deferred drain because the drain is skipped once connection
    // state leaves CONNECTED — fine for connects, but the symmetric
    // disconnect post below would be lost for the LAST disconnect.
    {
      const uint8_t* a = param->connect.remote_bda;
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               a[0], a[1], a[2], a[3], a[4], a[5]);
      systemEventPost(SYSEVT_BLE_CONNECTED,
                      bleDeviceTypeToString(gBLEState->connections[slot].deviceType), macStr);
    }
    
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
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return;
    
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

    {
      char remaining[8];
      snprintf(remaining, sizeof(remaining), "%d", gBLEState->activeConnectionCount);
      systemEventPost(SYSEVT_BLE_DISCONNECTED, remaining);
    }
    
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
    // Zero-copy read: getData()/getLength() return the characteristic's internal
    // buffer (getData() is (uint8_t*)m_value.c_str(), so still NUL-terminated),
    // avoiding the by-value getValue() String heap-copy on BTC_TASK for every
    // inbound write. The buffer is valid for this synchronous callback.
    const uint8_t* data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len > 0) {
      gBLEState->commandsReceived++;

      // Defer logging to task context
      gBLEState->deferredCmdReceivedConnId = param->write.conn_id;
      gBLEState->deferredCmdReceivedLen = len;
      gBLEState->deferredCmdReceivedPending = true;

      BLE_DEBUGF(DEBUG_BLE_GATT,
                 "Command write conn=%u len=%u",
                 (unsigned)param->write.conn_id,
                 (unsigned)len);

      // Route to processIncomingBLECommand which handles lightweight ops directly
      // and routes heavy commands through cmd_exec task
      processIncomingBLECommand(param->write.conn_id, (const char*)data, len);
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

// Result of the most recent notify() on the response characteristic. notify() is
// fire-and-forget at the ATT layer, but the Bluedroid wrapper surfaces the underlying
// esp_ble_gatts_send_indicate() result through onStatus(): SUCCESS_NOTIFY when the packet
// was accepted into the controller's ACL tx buffers, ERROR_GATT (errRc = ESP_ERR_NO_MEM /
// congestion) when the buffers were full and the frame was DROPPED. Without this hook the
// caller can't tell a sent frame from a lost one — which is exactly how big results
// (memreport, espnowmessages json pages) silently lost fragments. bleRawNotify() reads
// this to apply bounded backpressure instead of dropping. Installed on pCmdResponseChar
// in initBluetooth().
enum class BleNotifyResult : uint8_t { PENDING, OK, CONGESTED, TERMINAL };
static volatile BleNotifyResult gLastNotifyResult = BleNotifyResult::PENDING;

class CmdResponseCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* /*c*/, Status s, uint32_t /*code*/) override {
    if (s == Status::SUCCESS_NOTIFY)   gLastNotifyResult = BleNotifyResult::OK;
    else if (s == Status::ERROR_GATT)  gLastNotifyResult = BleNotifyResult::CONGESTED;  // tx buffers full → retryable
    else                               gLastNotifyResult = BleNotifyResult::TERMINAL;   // no client / notify disabled → don't spin
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
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

struct BleLoginAsyncJob {
  uint16_t connId;
  TransportSessionEpoch sessionEpoch;
  char user[kPublicUsernameMaxLen + 1];
  char password[kPublicPasswordMaxLen + 1];
};

struct BleCommandReplyContext {
  uint16_t connId;
  TransportSessionEpoch sessionEpoch;
};

// --- BLE TX trace (debugging) ------------------------------------------------
// Dumps the EXACT device->app payload to serial/web/file but NOT back over BLE
// (routing it over BLE would feed the very pipe we're tracing and recurse). Chunks
// to < DEBUG_MSG_SIZE so the full content reaches serial untruncated — copy the
// "[BLE-TX] === BEGIN ===" .. "=== END ===" block to see what the device actually sent.
extern void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);

// Gate for the verbose BLE TX traces — on only when `debugbluetoothdata` is set
// (DEBUG_BLE_DATA). Non-static so System_BleSecureChannel.cpp's per-frame [BLE-SC]
// traces share the same flag. The traces themselves still route NOT-to-BLE (0x2F)
// to avoid feeding the very pipe they trace; this just makes them off by default.
bool bleDataDebugEnabled() { return isDebugFlagSet(DEBUG_BLE_DATA); }

static void bleTxTrace(const char* tag, uint16_t connId, const char* data, size_t len) {
  if (!bleDataDebugEnabled()) return;
  const uint8_t NOBLE = (uint8_t)(MSG_ROUTE_ALL & ~MSG_ROUTE_BLE);
  size_t frags = (len + 194) / 195; if (frags == 0) frags = 1;   // 195 = SC_MAX_PAY_FRAME
  char hdr[160];
  int n = snprintf(hdr, sizeof(hdr),
                   "[BLE-TX] %s connId=%u len=%zu bleFrags=%zu === BEGIN ===",
                   tag, (unsigned)connId, len, frags);
  if (n > 0) broadcastOutputCore_Routed(hdr, (size_t)n, NOBLE);
  size_t off = 0; int idx = 0; char line[220];
  while (off < len) {
    size_t c = (len - off < 180) ? (len - off) : 180;
    int m = snprintf(line, sizeof(line), "[BLE-TX#%d] %.*s", idx, (int)c, data + off);
    if (m > 0) broadcastOutputCore_Routed(line, (size_t)m, NOBLE);
    off += c; idx++;
  }
  const char* end = "[BLE-TX] === END ===";
  broadcastOutputCore_Routed(end, strlen(end), NOBLE);
}

// Async callback for BLE command results - called on cmd_exec task
static void bleCommandResultCallback(bool ok, const char* result, void* userData) {
  BleCommandReplyContext* reply =
      static_cast<BleCommandReplyContext*>(userData);
  if (!reply) return;
  const char* safeResult = result ? result : "";
  const size_t resultLen = strlen(safeResult);
  BLE_DEBUGF(DEBUG_BLE_DATA, "Async command result: ok=%d len=%zu connId=%u",
             ok, resultLen, reply->connId);
  if (bleSessionEpochMatches(reply->connId, reply->sessionEpoch)) {
    bleTxTrace("result", reply->connId, safeResult, resultLen);
    sendBLEResponseToSession(reply->connId, reply->sessionEpoch,
                             safeResult, resultLen);
  }
  free(reply);
}

static void bleLoginJobFree(BleLoginAsyncJob* job) {
  if (!job) return;
  volatile char* password = job->password;
  for (size_t i = 0; i < sizeof(job->password); ++i) password[i] = 0;
  free(job);
}

// Login is a caller-local BLE session transition, not a registry command.
// It is deferred because PBKDF2 must never run on BTC_TASK. Keeping the
// canonical username and password in this one job also removes the old
// re-serialization hole where `login u p display` could be interpreted by
// cmd_login as a request to replace the OLED session.
static void bleLoginDeferred(void* arg) {
  BleLoginAsyncJob* job = static_cast<BleLoginAsyncJob*>(arg);
  if (!job) return;
  if (!bleSessionEpochMatches(job->connId, job->sessionEpoch)) {
    bleLoginJobFree(job);
    return;
  }

  FsLockGuard authGuard("ble.login_bind");
  const bool authStoreHeld =
      authGuard.held || isFsLockedByCurrentTask();
  const bool authOk = authStoreHeld &&
      validateTransportCredentials(SOURCE_BLUETOOTH, String(job->user),
                                   String(job->password));

  if (authOk) {
    TransportSessionEpoch boundEpoch = 0;
    if (!bleBindSession(job->connId, job->sessionEpoch,
                        String(job->user), boundEpoch)) {
      recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), false,
                                 "Connection changed before session bind");
      bleLoginJobFree(job);
      return;
    }
    recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), true,
                               "Login successful");
    BLE_DEBUGF(DEBUG_BLE_CORE, "Login success conn=%u user='%s'", (unsigned)job->connId, job->user);
    // BLE broadcast/console output stays OFF on connect by default: keeps the
    // load down and the response characteristic clean (no [CMD] audit echo
    // mixed into command replies) for RPC-style clients. The client opts in
    // live with `outble 1` when it wants the streamed console, and `outble 0`
    // to stop it. Disconnect re-clears MSG_ROUTE_BLE regardless (see onDisconnect),
    // so a left-on flag never persists across sessions. Boot default is
    // MSG_ROUTE_SERIAL only (HardwareOne.cpp), so it also starts OFF.
    char out[160];
    snprintf(out, sizeof(out), "[ble] Login successful. User: %s%s", job->user,
             isAdminUser(String(job->user)) ? " (admin)" : "");
    sendBLEResponseToSession(job->connId, boundEpoch, out, strlen(out));
  } else {
    recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), false,
                               authStoreHeld ? "Invalid credentials"
                                             : "Authentication store unavailable");
    BLE_DEBUGF(DEBUG_BLE_CORE, "Login FAILED conn=%u", (unsigned)job->connId);
    const char* msg = "[ble] Authentication failed.";
    sendBLEResponseToSession(job->connId, job->sessionEpoch,
                             msg, strlen(msg));
  }
  bleLoginJobFree(job);
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
  static const char* const kVerbs[] = {
    "files", "fileread", "filewrite", "fileview", "filecreate",
    "filedelete", "filerename", "mkdir", "rmdir", "otawrite"
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
static void processBleCommandLine(uint16_t connId, TransportSessionEpoch ingressEpoch,
                                  const char* data, size_t len) {
  if (!bleSessionEpochMatches(connId, ingressEpoch)) return;
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

  // BLE login is a native session transition rather than a registry command.
  // Redact the entire credential tail even when the submitted grammar is
  // malformed; otherwise `login user pass with spaces` would mask only `pass`
  // and leak the remainder into debug/OLED history before being rejected.
  CommandArgs previewArgs{String(cmdStart)};
  String auditLine;
  if (previewArgs.count() > 0 &&
      previewArgs.arg(0).equalsIgnoreCase("login")) {
    auditLine = "login";
    if (previewArgs.count() > 1) {
      auditLine += " ";
      auditLine += previewArgs.arg(1);
    }
    auditLine += " ********";
  } else {
    auditLine = redactCmdForAudit(String(cmdStart));
  }
  BLE_DEBUGF(DEBUG_BLE_DATA, "Processing command: %s", auditLine.c_str());
  
  // Add to OLED message history
  #if ENABLE_OLED_DISPLAY
  {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "RX:%.*s", (int)(BLE_MSG_MAX_LEN - 4),
             auditLine.c_str());
    bleAddMessageToHistory(tagged);
  }
  #endif

  // Session commands use the same quote-aware grammar as the shared command
  // registry. This keeps the canonical username (without quote characters),
  // supports a quoted password containing spaces, and rejects an unquoted
  // trailing target instead of accidentally treating it as part of a BLE
  // password.
  if (previewArgs.count() > 0 &&
      previewArgs.arg(0).equalsIgnoreCase("login")) {
    if (previewArgs.unterminatedQuote() || previewArgs.count() != 3) {
      const char* msg = "Usage: login <username> <password>";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
      return;
    }

    const String& parsedUser = previewArgs.arg(1);
    const String& parsedPassword = previewArgs.arg(2);
    const size_t ulen = parsedUser.length();
    const size_t plen = parsedPassword.length();
    if (ulen == 0 || ulen > kPublicUsernameMaxLen ||
        plen == 0 || plen > kPublicPasswordMaxLen) {
      const char* msg = "Usage: login <username> <password>";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
      return;
    }
    // Defer to cmd_exec_task — isValidUser() uses too much stack for BTC_TASK.
    // This job carries the PLAINTEXT password until bleLoginDeferred runs, so
    // it must live in internal DRAM, never PSRAM: flash encryption is off on
    // most boards and external PSRAM is a probeable chip. PreferInternal goes
    // through malloc(), which for a ~140 B block is served from internal DRAM
    // (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384); it could only spill to
    // PSRAM if internal DRAM had no 140 B block left, at which point the BT
    // host itself is already failing. bleLoginJobFree wipes the password
    // before free.
    BleLoginAsyncJob* job = (BleLoginAsyncJob*)ps_alloc(sizeof(BleLoginAsyncJob),
                                                       AllocPref::PreferInternal,
                                                       "ble.loginAsyncJob");
    if (!job) {
      const char* msg = "Error: out of memory";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
      return;
    }
    memset(job, 0, sizeof(*job));
    job->connId = connId;
    job->sessionEpoch = ingressEpoch;
    memcpy(job->user, parsedUser.c_str(), ulen + 1);
    memcpy(job->password, parsedPassword.c_str(), plen + 1);

    if (!submitDeferredToCmdExec(bleLoginDeferred, job)) {
      bleLoginJobFree(job);
      const char* msg = "Error: Failed to queue command";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    }
    return;
  }
  if (strcasecmp(cmdStart, "logout") == 0) {
    const TransportSessionEpoch logoutEpoch = bleLogout(connId);
    const char* msg = "[ble] Logged out.";
    sendBLEResponseToSession(connId, logoutEpoch, msg, strlen(msg));
    return;
  }
  if (strcasecmp(cmdStart, "whoami") == 0) {
    String u;
    if (bleIsAuthed(connId, u)) {
      char out[80];
      snprintf(out, sizeof(out), "You are %s%s", u.c_str(), isAdminUser(u) ? " (admin)" : "");
      sendBLEResponseToSession(connId, ingressEpoch, out, strlen(out));
    } else if (!gSettings.bleRequireAuth) {
      const char* msg = "You are AuthBypass";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    } else {
      const char* msg = "You are (unknown)";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    }
    return;
  }

  // Auth gate
  if (gSettings.bleRequireAuth) {
    String u;
    if (!bleIsAuthed(connId, u)) {
      bleSendAuthRequired(connId, ingressEpoch);
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
    sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    return;
  }

  // Execute command via central cmd_exec task (avoids BTC_TASK stack overflow)
  // Build Command structure for async submission
  Command ucmd;
  ucmd.line = cmdStart;
  ucmd.ctx.origin = ORIGIN_BLUETOOTH;
  TransportSessionEpoch commandEpoch = 0;
  if (!bleFillCommandAuth(connId, ucmd.ctx.auth, /*forLogin=*/false,
                          commandEpoch)) {
    return;
  }
  if (commandEpoch != ingressEpoch) return;
  ucmd.ctx.transportSessionEpoch = commandEpoch;
  ucmd.ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  // See the login builder above: BLE commands may execute normally, but may
  // neither open nor answer a process-wide interactive CLI mode in this phase.
  ucmd.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT;
  ucmd.ctx.validateOnly = false;
  ucmd.ctx.outputMask = MSG_ROUTE_FILE | MSG_ROUTE_BLE;
  ucmd.ctx.replyHandle = nullptr;
  ucmd.ctx.httpReq = nullptr;
  ucmd.ctx.id = (uint32_t)millis();
  ucmd.ctx.timestampMs = (uint32_t)millis();

  BleCommandReplyContext* reply =
      static_cast<BleCommandReplyContext*>(ps_alloc(
          sizeof(BleCommandReplyContext), AllocPref::PreferPSRAM,
          "ble.commandReply"));
  if (!reply) {
    const char* msg = "Error: out of memory";
    sendBLEResponseToSession(connId, commandEpoch, msg, strlen(msg));
    return;
  }
  reply->connId = connId;
  reply->sessionEpoch = commandEpoch;

  // Submit async - callback will send BLE response when complete
  if (!submitCommandAsync(ucmd, bleCommandResultCallback, reply)) {
    free(reply);
    const char* msg = "Error: Failed to queue command";
    sendBLEResponseToSession(connId, commandEpoch, msg, strlen(msg));
  }
}

// Defer secure-channel work off BTC_TASK (8 KB) onto cmd_exec_task (deep stack) —
// the X25519/HKDF handshake won't fit comfortably on the BLE callback stack. Mirrors
// how ESP-NOW pushes radio-callback crypto onto cmd_exec via submitDeferredToCmdExec.
struct BleScDeferred {
  uint16_t connId;
  uint16_t len;
  TransportSessionEpoch sessionEpoch;
  uint8_t buf[517];
};

// Runs on cmd_exec_task. Frees its own arg (per submitDeferredToCmdExec contract).
static void bleScDeferredInbound(void* arg) {
  BleScDeferred* d = (BleScDeferred*)arg;
  if (!bleSessionEpochMatches(d->connId, d->sessionEpoch)) {
    free(d);
    return;
  }
  char   plain[512];
  size_t pl = 0;
  BleScResult r = bleScHandleInbound(d->connId, d->buf, d->len, plain, sizeof(plain), &pl);
  if (r == BLE_SC_PLAINTEXT_READY) {
    processBleCommandLine(d->connId, d->sessionEpoch, plain, pl);
  } else if (r == BLE_SC_BINARY_READY) {
    // The secure-channel decoder recognized an opaque application envelope.
    // OTA performs its own live-session/superadmin check for every frame.
    if (!bleSessionEpochMatches(d->connId, d->sessionEpoch)) {
      free(d);
      return;
    }
    bleMarkActivity(d->connId);
    (void)otaBleHandleEncryptedFrame(d->connId,
                                     reinterpret_cast<const uint8_t*>(plain), pl);
  }
  free(d);
}

// Entry from the GATT write callback — runs on BTC_TASK (8 KB, time-critical). Keep it
// light: secure-channel frames are copied + deferred to cmd_exec (where the handshake
// crypto runs); plaintext commands go straight to the command tail (which itself defers
// the heavy command execution via submitCommandAsync).
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {
  const TransportSessionEpoch ingressEpoch =
      bleSessionEpochForConnection(connId);
  if (ingressEpoch == 0) return;
  const uint8_t t = (len > 0) ? (uint8_t)data[0] : 0;
  const bool isFrame = (t == 0x01 /*HELLO*/ || t == 0x03 /*CONFIRM*/ || t == 0x10 /*DATA*/);
  if (isFrame && len <= sizeof(((BleScDeferred*)0)->buf)) {
    BleScDeferred* d = (BleScDeferred*)ps_alloc(sizeof(BleScDeferred), AllocPref::PreferPSRAM, "ble.sc.rx");
    if (d) {
      d->connId = connId;
      d->len = (uint16_t)len;
      d->sessionEpoch = bleSessionEpochForConnection(connId);
      memcpy(d->buf, data, len);
      if (d->sessionEpoch == 0) {
        free(d);
        return;
      }
      if (!submitDeferredToCmdExec(bleScDeferredInbound, d)) free(d);
    }
    return;
  }
  // Plaintext path: refuse if the secure channel is required, else run the command.
  if (bleScRequired()) {
    const char* msg = "Secure channel required — connect with the encrypted app.";
    sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    return;
  }
  processBleCommandLine(connId, ingressEpoch, data, len);
}

// =============================================================================
// INITIALIZATION
// =============================================================================

bool initBluetooth() {
  BleRoleTransitionClaim role(BleRoleTransition::SERVER_START);
  if (!role) {
    broadcastOutput("[BLE] Server start deferred: another BLE role transition is active");
    return false;
  }
  if (bleStackLifecycleFaulted() &&
      bleRoleTransitionState() != BleRoleTransition::RECOVERING) {
    broadcastOutput("[BLE] Server start blocked: prior host teardown was incomplete; reboot required");
    return false;
  }
  if (!bleInitLifecycleMutex()) {
    broadcastOutput("[BLE] Failed to create lifecycle mutex");
    return false;
  }
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
    if (!deinitG2Client()) {
      broadcastOutput("[BLE] Cannot start server mode: G2 control owner did not stop");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
#endif

  if (BLEDevice::getInitialized() || isBluedroidHostEnabled() ||
      isBleControllerEnabled()) {
    if (!g2RingPrepareForStackTeardown(2000)) {
      broadcastOutput("[BLE] Cannot start server mode: Ring owner did not quiesce");
      return false;
    }
    BLEDevice::setCustomGapHandler(nullptr);
    const BLEDeviceDeinitResult reset = BLEDevice::deinitChecked(false);
    if (!reset.success) {
      bleStackSetLifecycleFault(true);
      broadcastOutput("[BLE] Existing client host could not be normalized; reboot required");
      return false;
    }
    bleCentralClientsTerminalTeardownAcknowledged();
    vTaskDelay(pdMS_TO_TICKS(100));
  }


  // Check memory before initializing BLE stack (52KB floor; BLE init measured at ~43KB + headroom)
  if (!checkMemoryAvailable("bluetooth", nullptr)) {
    if (sBLEToggleCount > 0) {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need 52KB DRAM)");
      broadcastOutput("[BLE] ESP32 BLE leaks ~10KB DRAM per stop/start cycle. Reboot to recover.");
    } else {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need 52KB DRAM)");
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

  if (!BLEDevice::getInitialized() || !isBluedroidHostEnabled() ||
      !isBleControllerEnabled()) {
    broadcastOutput("[BLE] Init failed (host/controller incomplete)");
    const BLEDeviceDeinitResult rollback = BLEDevice::deinitChecked(false);
    if (rollback.success) bleCentralClientsTerminalTeardownAcknowledged();
    if (!rollback.success) bleStackSetLifecycleFault(true);
    if (gBLEState) {
      free(gBLEState);
      gBLEState = nullptr;
    }
    return false;
  }

  // Raise the GATT MTU ceiling so command responses aren't capped at the BLE
  // default of 23 (= 20 usable bytes). 517 is the BLE max; payload becomes
  // MTU-3 = 514 bytes, which fully covers the BLE_OUTPUT_BUFFER_MAX (512) flush
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
  
  // Capture this server app's GATT interface so plaintext command replies can
  // use IDF's connection-directed notify instead of Arduino's peer broadcast.
  sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  BLEDevice::setCustomGattsHandler(bleGattsEventTap);

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
  pCmdResponseChar->setCallbacks(new CmdResponseCallbacks());  // onStatus → notify backpressure
  
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
  systemEventPost(SYSEVT_BLE_ON, deviceName);

  return true;
}

void deinitBluetooth() {
  BleRoleTransitionClaim role(BleRoleTransition::SERVER_STOP);
  if (!role) {
    broadcastOutput("[BLE] Server stop deferred: another BLE role transition is active");
    return;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState || !gBLEState->initialized) return;
    // Admission closes before the stack and characteristic objects are torn
    // down.  Any queued result carrying an old epoch now fails closed.
    for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
      bleInvalidateSessionSlotLocked(i);
    }
  }
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Deinitializing Bluetooth...");
  
  stopBLEAdvertising();

  // Drop R1 GATT pointers before the stack dies. openg2 often deinit's
  // server mode while ringconnect left a live client; without this,
  // Health polls write through a freed characteristic → LoadProhibited.
  if (!g2RingPrepareForStackTeardown(2000)) {
    broadcastOutput("[BLE] Server stop deferred: Ring owner did not quiesce");
    return;
  }
  
  // ESP32 BLE doesn't have a clean disconnect API like NimBLE
  // Just deinit the device
  sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  BLEDevice::setCustomGattsHandler(nullptr);
  const BLEDeviceDeinitResult deinitResult = BLEDevice::deinitChecked(false);
  if (!deinitResult.success) {
    bleStackSetLifecycleFault(true);
    char buf[196];
    snprintf(buf, sizeof(buf),
             "[BLE] Server teardown incomplete (phase=%u host=%ld->%ld "
             "controller=%ld->%ld quarantined=%d) — reboot required",
             (unsigned)deinitResult.phase,
             (long)deinitResult.hostStatusBefore,
             (long)deinitResult.hostStatusAfter,
             (long)deinitResult.controllerStatusBefore,
             (long)deinitResult.controllerStatusAfter,
             deinitResult.clientQuarantined ? 1 : 0);
    broadcastOutput(buf);
    return;
  }
  bleCentralClientsTerminalTeardownAcknowledged();
  bleStackSetLifecycleFault(false);

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
  {
    BleLifecycleGuard lifecycle;
    if (lifecycle && gBLEState) {
      free(gBLEState);
      gBLEState = nullptr;
    }
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
  systemEventPost(SYSEVT_BLE_OFF);
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
//
// Bounded backpressure: a single notify() silently drops its frame when the controller's
// ACL tx buffers are full (the common case mid-burst). We detect that via onStatus
// (CONGESTED) and retry after yielding ~one connection interval, so bursts ride out
// transient congestion instead of losing fragments. The retry budget caps total wait so a
// stalled/dead link can never hang the calling task — under *sustained* congestion we give
// up and return false, and the caller reports the failure (never silent). vTaskDelay yields
// the CPU, so waiting costs latency, not cycles. In the common (uncongested) case the first
// notify succeeds and there is zero added delay.
//
// blocking=false collapses the retry budget to a single attempt: a best-effort
// producer running on the main loop (the notification sink) must not pay up to
// 6 x 15 ms of vTaskDelay per send. It gives up on the first CONGESTED instead,
// and its caller counts the drop. Reliable senders keep the default.
static bool bleRawNotifyToSession(uint16_t connId,
                                  TransportSessionEpoch expectedEpoch,
                                  const char* data, size_t len,
                                  bool blocking = true) {
  if (!data || len == 0 || expectedEpoch == 0) return false;

  const int kMaxTries = blocking ? 6 : 1;
  const TickType_t kBackoff = pdMS_TO_TICKS(15);
  esp_err_t lastError = ESP_FAIL;
  int tries = 0;

  for (int attempt = 0; attempt < kMaxTries; ++attempt) {
    tries = attempt + 1;
    {
      BleLifecycleGuard lifecycle;
      if (!lifecycle ||
          !bleConnectionEpochMatchesLocked(connId, expectedEpoch) ||
          !pCmdResponseChar) {
        return false;
      }

      const int gattsIf = sBleGattsIf.load(std::memory_order_acquire);
      if (gattsIf == ESP_GATT_IF_NONE) return false;

      // Unlike BLECharacteristic::notify(), this queues only to connId.  Keep
      // the lifecycle mutex through the IDF admission call so disconnect/slot
      // reuse is ordered either before this send (drop) or after it (old peer).
      lastError = esp_ble_gatts_send_indicate(
          static_cast<esp_gatt_if_t>(gattsIf), connId,
          pCmdResponseChar->getHandle(), len,
          reinterpret_cast<uint8_t*>(const_cast<char*>(data)), false);
      if (lastError == ESP_OK) {
        if (gBLEState) gBLEState->responsesSent++;
      }
    }

    if (lastError == ESP_OK) break;
    // Preserve the existing bounded retry behavior.  Releasing the lifecycle
    // mutex during backoff lets disconnect run; the next attempt revalidates
    // the exact epoch before touching the radio.  No backoff after the FINAL
    // attempt — it buys nothing because no retry follows it, and with
    // blocking=false (kMaxTries == 1) it would reintroduce the very stall the
    // flag exists to avoid.
    if (attempt + 1 < kMaxTries) vTaskDelay(kBackoff);
  }

  const bool sent = lastError == ESP_OK;
  if ((!sent || tries > 1) && bleDataDebugEnabled()) {
    char b[144];
    int n = snprintf(b, sizeof(b),
                     "[BLE-NOTIFY] conn=%u epoch=%lu len=%zu sent=%d tries=%d err=%d",
                     (unsigned)connId, (unsigned long)expectedEpoch, len,
                     sent ? 1 : 0, tries, (int)lastError);
    if (n > 0) {
      broadcastOutputCore_Routed(
          b, (size_t)n, (uint8_t)(MSG_ROUTE_ALL & ~MSG_ROUTE_BLE));
    }
  }

  #if ENABLE_OLED_DISPLAY
  if (sent) {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "TX:%.*s",
             (int)(BLE_MSG_MAX_LEN - 4), data);
    bleAddMessageToHistory(tagged);
  }
  #endif
  return sent;
}

bool bleRawNotify(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "bleRawNotify dropped (connected=%d char=%p)",
               isBLEConnected(), (void*)pCmdResponseChar);
    return false;
  }

  const int        BLE_NOTIFY_MAX_TRIES = 6;
  const TickType_t BLE_NOTIFY_BACKOFF   = pdMS_TO_TICKS(15);  // ~one conn interval; 6*15 = ~90ms cap

  // Set the characteristic value once, not per retry: the payload is identical on
  // every attempt and setValue() heap-copies it into the wrapper's String, so only
  // notify() needs to repeat under congestion. (The additional getValue() copies
  // inside notify() itself are in vendored Arduino-BLE code and can't be removed
  // without reimplementing its multi-peer + onStatus congestion path — left as-is;
  // see audit finding Bluetooth.cpp:1191.)
  pCmdResponseChar->setValue((uint8_t*)data, len);

  bool sent = false;
  int  tries = 0;
  for (int attempt = 0; attempt < BLE_NOTIFY_MAX_TRIES; attempt++) {
    tries = attempt + 1;
    gLastNotifyResult = BleNotifyResult::PENDING;
    pCmdResponseChar->notify();                 // fires onStatus synchronously on this task
    BleNotifyResult r = gLastNotifyResult;
    // PENDING means onStatus never ran (no callbacks / unexpected path) — assume delivered
    // rather than spin. OK = accepted into tx buffers.
    if (r == BleNotifyResult::OK || r == BleNotifyResult::PENDING) { sent = true; break; }
    if (r == BleNotifyResult::TERMINAL) break;  // no client / disabled — retrying won't help
    if (!isBLEConnected()) break;               // link dropped mid-retry
    vTaskDelay(BLE_NOTIFY_BACKOFF);             // CONGESTED — let the controller drain, then retry
  }

  // Trace only when interesting (a retry happened or we gave up) so it doesn't spam.
  if ((!sent || tries > 1) && bleDataDebugEnabled()) {
    char b[120];
    int n = snprintf(b, sizeof(b), "[BLE-NOTIFY] len=%zu sent=%d tries=%d", len, sent ? 1 : 0, tries);
    if (n > 0) broadcastOutputCore_Routed(b, (size_t)n, (uint8_t)(MSG_ROUTE_ALL & ~MSG_ROUTE_BLE));
  }

  if (sent && gBLEState) gBLEState->responsesSent++;

  #if ENABLE_OLED_DISPLAY
  {
    char tagged[BLE_MSG_MAX_LEN];
    snprintf(tagged, sizeof(tagged), "TX:%.*s", (int)(BLE_MSG_MAX_LEN - 4), data ? data : "");
    bleAddMessageToHistory(tagged);
  }
  #endif
  return sent;
}

// Output chokepoint for broadcast/streamed device output (the debug-drain "path A" via
// System_Debug.cpp, plus any other caller). If a Secure Channel is established for a
// connected client, encrypt+frame the output for it — so STREAMED CLI output (menus,
// help, reports) is confidential, not just the short command result. In secure-required
// mode with no established channel, DROP rather than leak plaintext over the air.
bool sendBLEResponse(const char* data, size_t len, bool blocking) {
  if (!data || len == 0) return false;
  if (gBLEState) {
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (gBLEState->connections[i].active &&
          bleScEstablished(gBLEState->connections[i].connId)) {
        return bleScSendEncrypted(gBLEState->connections[i].connId, data, len, blocking);
      }
    }
  }
  if (bleScRequired()) return false;     // never emit plaintext on a must-be-secure link
  return bleRawNotify(data, len);        // plaintext mode (no channel) — as before
}

bool sendBLEResponseToConn(uint16_t connId, const char* data, size_t len) {
  const TransportSessionEpoch epoch =
      bleSessionEpochForConnection(connId);
  return sendBLEResponseToSession(connId, epoch, data, len);
}

bool sendBLEResponseToSession(uint16_t connId,
                              TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking) {
  if (!data || len == 0) return false;

  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle ||
        !bleConnectionEpochMatchesLocked(connId, expectedEpoch)) {
      return false;
    }

    // Keep lifecycle ownership across secure-channel selection and emission.
    // Otherwise connId could be reused after the epoch check, and a stale
    // result could be encrypted with the replacement peer's newly established
    // keys.  The secure sender is bounded to 255 paced fragments by protocol.
    if (bleScEstablished(connId)) {
      return bleScSendEncrypted(connId, data, len, blocking);
    }
    if (bleScRequired()) return false;
  }

  // Plaintext command replies are never characteristic-wide.  The session
  // epoch is checked in the same lifecycle critical section as the directed
  // GATT admission, closing both connId reuse and check-then-send races.
  return bleRawNotifyToSession(connId, expectedEpoch, data, len, blocking);
}

void bleSessionTick() {
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return;

    // Handle deferred callback flags while the backing state cannot be freed or
    // reused underneath this task.
    if (gBLEState->deferredConnectPending) {
      gBLEState->deferredConnectPending = false;
      int slot = gBLEState->deferredConnectSlot;
      BLE_DEBUGF(DEBUG_BLE_CORE, "Client connected (slot %d, total active: %d/%d)",
                 slot, gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
      if (gBLEState->activeConnectionCount >= BLE_MAX_CONNECTIONS) {
        BLE_DEBUGF(DEBUG_BLE_CORE, "Max connections reached - stopped advertising");
      }
    }

    if (gBLEState->deferredDisconnectPending) {
      gBLEState->deferredDisconnectPending = false;
      BLE_DEBUGF(DEBUG_BLE_CORE, "Client disconnected (active connections: %d)",
                 gBLEState->deferredDisconnectActiveCount);
      if (gBLEState->deferredDisconnectActiveCount < BLE_MAX_CONNECTIONS) {
        BLE_DEBUGF(DEBUG_BLE_CORE, "Auto-restarted advertising (slots available)");
      }
      if (!bleHasAuthenticatedSession()) gOutputFlags &= ~MSG_ROUTE_BLE;
    }

    if (gBLEState->deferredCmdReceivedPending) {
      gBLEState->deferredCmdReceivedPending = false;
      BLE_DEBUGF(DEBUG_BLE_GATT, "Command received (%d bytes) conn_id=%u",
                 (int)gBLEState->deferredCmdReceivedLen,
                 (unsigned)gBLEState->deferredCmdReceivedConnId);
    }
  }

  uint32_t now = millis();
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    uint16_t connId = 0;
    uint32_t idleMs = 0;
    String expiredUser;
    TransportSessionEpoch logoutEpoch = 0;
    {
      BleLifecycleGuard lifecycle;
      if (!lifecycle || !gBLEState) return;
      BLEConnection& conn = gBLEState->connections[i];
      if (!conn.active || !conn.authed || conn.lastActivityMs == 0 ||
          !sessionIdleExpired(SOURCE_BLUETOOTH, conn.lastActivityMs, now)) {
        continue;
      }
      connId = conn.connId;
      idleMs = now - conn.lastActivityMs;
      expiredUser = conn.user;
      bleInvalidateSessionSlotLocked(i);
      conn.authed = false;
      conn.user = "";
      logoutEpoch = blePublishSessionSlotLocked(i);
      if (!bleHasAuthenticatedSession()) gOutputFlags &= ~MSG_ROUTE_BLE;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "[ble] Session expired for user '%s'",
             expiredUser.c_str());
    sendBLEResponseToSession(connId, logoutEpoch, msg, strlen(msg));
    BLE_DEBUGF(DEBUG_BLE_CORE,
               "Session expired conn=%u user='%s' idle_ms=%lu",
               connId, expiredUser.c_str(), (unsigned long)idleMs);
  }
}

// =============================================================================
// STATUS
// =============================================================================

bool bleRoleTransitionBegin(BleRoleTransition state) {
  if (state == BleRoleTransition::IDLE) return false;
  TaskHandle_t current = xTaskGetCurrentTaskHandle();
  if (!current) return false;
  bool acquired = false;
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  if (!sBleRoleTransitionOwner || sBleRoleTransitionOwner == current) {
    if (!sBleRoleTransitionOwner) {
      sBleRoleTransitionOwner = current;
      sBleRoleTransition = state;
      sBleRoleTransitionDepth = 1;
    } else if (sBleRoleTransitionDepth < 0xFF) {
      ++sBleRoleTransitionDepth;
    }
    acquired = true;
  }
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
  return acquired;
}

void bleRoleTransitionEnd() {
  TaskHandle_t current = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  if (sBleRoleTransitionOwner == current && sBleRoleTransitionDepth > 0) {
    if (--sBleRoleTransitionDepth == 0) {
      sBleRoleTransitionOwner = nullptr;
      sBleRoleTransition = BleRoleTransition::IDLE;
    }
  }
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
}

BleRoleTransition bleRoleTransitionState() {
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  const BleRoleTransition state = sBleRoleTransition;
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
  return state;
}

bool bleRoleTransitionHeldByCurrentTask() {
  TaskHandle_t current = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  const bool held = current && sBleRoleTransitionOwner == current;
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
  return held;
}

bool bleStackLifecycleFaulted() {
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  const bool faulted = sBleStackLifecycleFaulted;
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
  return faulted;
}

void bleStackSetLifecycleFault(bool faulted) {
  portENTER_CRITICAL(&sBleRoleTransitionMux);
  sBleStackLifecycleFaulted = faulted;
  portEXIT_CRITICAL(&sBleRoleTransitionMux);
}

bool isBleControllerEnabled() {
#if CONFIG_BT_CONTROLLER_ENABLED
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    return true;
  }
#endif
  return false;
}

bool isBluedroidHostEnabled() {
#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
#else
  return false;
#endif
}

bool isBleServerInitialized() {
  return gBLEState && gBLEState->initialized;
}

bool isBLERunning() {
  // Preserve the historical controller-wide contract for compatibility. The
  // server fallback also preserves behavior in tests/partial teardown states
  // where app ownership remains published after the controller drops.
  return isBleControllerEnabled() || isBleServerInitialized();
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
  if (!gSettings.bleEnabled) {
    return "ERROR: Bluetooth is disabled - run 'bleenabled 1' first";
  }
  // `openble` means "be the phone-facing server." Record server as the desired
  // BLE role and cancel any pending G2/R1 client reconnect intent BEFORE
  // initBluetooth(): its G2 teardown fires the temples' onDisconnect
  // synchronously, which would otherwise re-arm auto-reconnect and let the loop
  // tick flip the radio back to client mode — the mode-thrash that double-freed
  // the BLE stack. setSetting persists so the tick honors the role across boots.
  setSetting(gSettings.bleMode, (int)BLE_MODE_SERVER);
  // Stamp peer suppression and advance each family's in-flight cancel epoch
  // as one ordered transition before the server role takes ownership.
  g2Disconnect(/*userInitiated=*/true);
  g2RingDisconnect(/*userInitiated=*/true);
  // Pause sensor polling during BLE init to avoid interrupt contention (RAII —
  // resumes on every return path; the trailing string checks do no I2C work).
  PollPauseGuard pollGuard;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete

  bool initOk = initBluetooth();
  bool advOk = initOk ? startBLEAdvertising() : false;

  if (!initOk) {
    return "Error: Failed to initialize Bluetooth";
  }
  if (!advOk) {
    return "Error: Failed to start advertising";
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE started and advertising");
  return "Bluetooth started and advertising";
}

static const char* cmd_blestop(const String& argsInput) {
  deinitBluetooth();
  // Stopping the server returns the device to its default (G2 client) role so
  // the glasses can auto-reconnect again — closeble means "stop the server,"
  // not "stay off BLE" (use `bleenabled 0` for that). Re-opens the tick guard.
  setSetting(gSettings.bleMode, (int)BLE_MODE_G2_CLIENT);
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE stopped");
  return "Bluetooth stopped";
}

static const char* cmd_blestatus(const String& argsInput) {
  const bool wantJson = argWantsJson(argsInput);

  if (!gBLEState || !gBLEState->initialized) {
    if (wantJson) return "{\"schema\":1,\"initialized\":false}";
    return "Error: Bluetooth not initialized. Run 'openble' first.";
  }

  // JSON: machine-readable connection state for the app. Returned to the caller
  // only (no broadcastOutput) so it never lands on the shared consoles.
  if (wantJson) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["initialized"] = true;
    doc["state"] = getBLEStateString();
    doc["activeConnections"] = gBLEState->activeConnectionCount;
    doc["maxConnections"] = BLE_MAX_CONNECTIONS;
    doc["totalConnections"] = gBLEState->totalConnections;
    doc["commandsReceived"] = gBLEState->commandsReceived;
    doc["responsesSent"] = gBLEState->responsesSent;
    JsonArray conns = doc["connections"].to<JsonArray>();
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (!gBLEState->connections[i].active) continue;
      JsonObject c = conns.add<JsonObject>();
      char macStr[18];
      macToStackBuf(gBLEState->connections[i].deviceAddr, macStr);
      const char* who = (gBLEState->connections[i].authed &&
                         gBLEState->connections[i].user.length() > 0)
                          ? gBLEState->connections[i].user.c_str() : "Unknown";
      c["index"]        = i;
      c["user"]         = String(who);
      c["mac"]          = String(macStr);
      c["connectedSec"] = (uint32_t)((millis() - gBLEState->connections[i].connectedSince) / 1000);
      c["commands"]     = gBLEState->connections[i].commandsReceived;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
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
      // Identity shown is the AUTHENTICATED user (firmware-proven via login),
      // NOT the device-advertised name (which the client controls and we don't
      // trust). A connection that hasn't logged in shows "Unknown"; once it
      // authenticates it flips to that user's username. The web "Connected
      // Clients" card parses this line; the MAC still identifies the device.
      const char* who = (gBLEState->connections[i].authed &&
                         gBLEState->connections[i].user.length() > 0)
                          ? gBLEState->connections[i].user.c_str() : "Unknown";
      written = snprintf(buf + offset, remaining, "[%d] %s\n    MAC: %s | %lu sec | %lu cmds\n",
               i, who, macStr, duration, gBLEState->connections[i].commandsReceived);
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
    return "Error: Set the secure-channel passphrase via serial, OLED, or web — not over Bluetooth.";
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
  if (!on && !off) return "Error: invalid arguments — Usage: blesecure [on|off]";
  if (on && gSettings.bleSecureChannelSecret.length() == 0)
    return "Error: Set a secret first: blesecret <passphrase>";
  setSetting(gSettings.bleRequireSecureChannel, on);
  return on ? "BLE secure channel REQUIRED — plaintext commands now refused."
            : "BLE secure channel optional — plaintext allowed.";
}

// Boot-time security nudge: encryption is wanted (bleRequireSecureChannel, default on)
// but no passphrase is set, so the channel can't be enforced and BLE is PLAINTEXT.
// Printed as the last lines of boot so the operator can't miss it. No-op once a secret
// is set, or if the user explicitly opted into plaintext (blesecure off), or in G2 mode.
void bleSecurityBootNotice() {
  if (!gSettings.bleAutoStart) return;                    // BT not running
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
    return "Error: No client connected";
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
  return "Error: Failed to start advertising";
}

static const char* cmd_blesend(const String& argsInput) {
  if (!isBLEConnected()) {
    return "Error: No client connected";
  }
  
  // Extract message after first space (skip command name) - zero heap allocations
  const char* msg = argsInput.c_str();
  while (*msg && *msg != ' ') msg++;  // Skip command name
  while (*msg == ' ') msg++;  // Skip spaces
  
  if (*msg == '\0') {
    return "Error: invalid arguments — Usage: blesend <message>";
  }
  
  if (sendBLEResponse(msg, strlen(msg))) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "blesend transmitted len=%u", (unsigned)strlen(msg));
    cliHint("this pushes raw text to the client; it does not run a command - to check the connection, run 'blestatus'");
    return "Message sent via BLE";
  }
  BLE_DEBUGF(DEBUG_BLE_DATA, "blesend failed len=%u", (unsigned)strlen(msg));
  return "Error: Failed to send message";
}

static const char* cmd_blestream(const String& argsInput) {
  if (!gBLEState || !gBLEState->initialized) {
    return "Error: Bluetooth not initialized";
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
      return "Error: Intervals must be >= 100ms";
    }
    return "Error: invalid arguments — Usage: blestream interval <sensor_ms> <system_ms>";
  }
  
  return "Error: invalid arguments — Usage: blestream <on|off|sensors|system|events|interval>";
}

static const char* cmd_bleevent(const String& argsInput) {
  if (!isBLEConnected()) {
    return "Error: No client connected";
  }
  
  // Extract message after first space - zero heap allocations
  const char* msg = argsInput.c_str();
  while (*msg && *msg != ' ') msg++;  // Skip command name
  while (*msg == ' ') msg++;  // Skip spaces
  
  if (*msg == '\0') {
    return "Error: invalid arguments — Usage: bleevent <message>";
  }
  
  if (blePushEvent(BLE_EVENT_CUSTOM, msg)) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "bleevent sent" );
    return "Event sent via BLE";
  }
  BLE_DEBUGF(DEBUG_BLE_DATA, "bleevent failed");
  return "Error: Failed to send event";
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
      return "Error: Name must be 1-29 characters";
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
      return "Error: TX power must be 0-7 (0=min/-12dBm, 7=max/+9dBm)";
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
    doc["schema"]              = 1;
    doc["deviceName"]     = gSettings.bleDeviceName;
    doc["txPower"]        = gSettings.bleTxPower;
    doc["autoStart"]      = gSettings.bleAutoStart;
    doc["requireAuth"]    = gSettings.bleRequireAuth;
    doc["secureChannelRequired"] = bleScRequired();
    doc["initialized"]    = init;
    doc["state"]          = getBLEStateString();
    doc["connections"]    = init ? gBLEState->activeConnectionCount : 0;
    doc["maxConnections"] = BLE_MAX_CONNECTIONS;
    doc["totalConnections"] = init ? gBLEState->totalConnections : 0;  // lifetime
    doc["commandsReceived"] = init ? gBLEState->commandsReceived : 0;  // lifetime
    // Per-connection identity for the web "Connected Clients" card. Emit the
    // AUTHENTICATED facts (authed + raw user) rather than a pre-baked "Unknown",
    // so the UI can tell an unauthenticated connection from a user named
    // "Unknown" — the ambiguity the old text format couldn't represent.
    JsonArray clients = doc["clients"].to<JsonArray>();
    if (init) {
      for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        const BLEConnection& c = gBLEState->connections[i];
        if (!c.active) continue;
        JsonObject o = clients.add<JsonObject>();
        o["connId"]   = c.connId;
        o["authed"]   = c.authed;
        o["user"]     = c.user;   // "" when not authed; UI renders authed ? user : "Unknown"
        o["since"]    = (uint32_t)((millis() - c.connectedSince) / 1000);
        o["commands"] = c.commandsReceived;
        char macStr[18]; macToStackBuf(c.deviceAddr, macStr);
        o["mac"]      = String(macStr);
      }
    }
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "bleinfo.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 2048);
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
  w = snprintf(buf + pos, rem, "Auto-Start:   %s\n", gSettings.bleAutoStart  ? "Yes" : "No");       if (w > 0) { pos += w; rem -= w; }
  w = snprintf(buf + pos, rem, "Require Auth: %s\n", gSettings.bleRequireAuth ? "Yes" : "No");      if (w > 0) { pos += w; rem -= w; }

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
BOOL_CMD(bleautostart, gSettings.bleAutoStart, "[BLE] Auto-start")

static const char* cmd_blerequireauth(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool before = gSettings.bleRequireAuth;
  const char* result =
      settingBoolToggle(gSettings.bleRequireAuth, a, "[BLE] Require auth");
  if (gSettings.bleRequireAuth != before) bleAuthPolicyChanged();
  return result;
}

// -----------------------------------------------------------------------------
// Mode (server vs. G2 client)
// -----------------------------------------------------------------------------

const char* getBleModeString() {
  return (gSettings.bleMode == BLE_MODE_G2_CLIENT) ? "client" : "server";
}

// Logical aggregate status: a bare enabled controller/host is not sufficient.
// One application role must own initialized runtime state.
bool bleSubsystemActive() {
  if (isBleServerInitialized()) return true;
#if ENABLE_G2_GLASSES
  if (isG2ClientInitialized()) return true;
#endif
  return false;
}

const char* bleSubsystemStateString() {
#if ENABLE_G2_GLASSES
  // Persisted mode is preference, not proof of runtime ownership. Prefer the
  // sole live client role; if an invariant violation publishes both roles,
  // the server branch below gives deterministic diagnostics.
  if (isG2ClientInitialized() && !isBleServerInitialized()) {
    return getG2StateString();
  }
#endif
  if (isBleServerInitialized()) return getBLEStateString();
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

  // The web settings dropdown stores bleMode as its int enum value (see the
  // "bluetoothMode" SettingEntry: 0=server, 1=client/g2) and saves via
  // `blemode <value>`. Accept those numeric forms in addition to the keywords.
  if (arg == "0") arg = "server";
  else if (arg == "1") arg = "client";

  if (arg == "server" || arg == "phone") {
#if ENABLE_G2_GLASSES
    if (isG2ClientInitialized()) {
      broadcastOutput("[BLE] Stopping G2 client mode");
      if (!deinitG2Client()) {
        return "Error: [BLE] G2 client teardown deferred; mode unchanged";
      }
    }
#endif
    setSetting(gSettings.bleMode, (int)BLE_MODE_SERVER);
    return "[BLE] Mode set to server";
  }

  if (arg == "client" || arg == "g2") {
#if !ENABLE_G2_GLASSES
    return "Error: [BLE] G2 client not compiled (ENABLE_G2_GLASSES=0)";
#else
    if (isBleServerInitialized()) {
      broadcastOutput("[BLE] Stopping BLE server mode");
      deinitBluetooth();
    }
    setSetting(gSettings.bleMode, (int)BLE_MODE_G2_CLIENT);
    return "[BLE] Mode set to client (G2)";
#endif
  }

  return "Error: invalid arguments — Usage: blemode [server|client]";
}

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry bluetoothCommands[] = {
  // Start/Stop (voice phrases live in kVoiceRoutes, System_ESPSR.cpp)
  { "openble",      "Start Bluetooth LE and begin advertising.", true, cmd_blestart },
  { "closeble",     "Stop Bluetooth LE and deinitialize.",       true, cmd_blestop },
  { "bleread",      "Read Bluetooth connection status. (add 'json' for JSON output)",         false, cmd_blestatus },
  { "blestatus",    "Show Bluetooth connection status. (add 'json' for JSON output)",         false, cmd_blestatus },
  { "bleinfo",      "Show BLE configuration and settings. (add 'json' for JSON output)",      false, cmd_bleinfo },
  { "blename",      "Get/set BLE device name [name].",           false, cmd_blename, "Usage: blename [name]" },
  { "bletxpower",   "Get/set BLE TX power [0-7].",               false, cmd_bletxpower, "Usage: bletxpower [0..7]" },
  { "bledisconnect","Disconnect current BLE client.",            true, cmd_bledisconnect },
  { "bleadv",       "Start/stop/toggle BLE advertising [start|stop|toggle].", true, cmd_bleadv, "Usage: bleadv [start|stop|toggle]" },
  { "blesend",      "Send message to BLE client: <message>.",    false, cmd_blesend, "Usage: blesend <message>" },
  { "blestream",    "Control streaming: <on|off|sensors|system|events|interval>.",false, cmd_blestream, "Usage: blestream [on|off|sensors|system|events|interval] | interval <sensor_ms> <system_ms>" },
  { "bleevent",     "Send event to BLE client: <event>.",        false, cmd_bleevent, "Usage: bleevent <message>" },
  
  // Auto-start
  { "bleautostart",    "Enable/disable BLE auto-start after boot [on|off].",   true, cmd_bleautostart,    "Usage: bleautostart [on|off]" },
  { "blerequireauth", "Enable/disable BLE authentication requirement [on|off].", true, cmd_blerequireauth, "Usage: blerequireauth [on|off]", /*requiresSuperAdmin=*/true },

  // Mode (server vs. G2 client) - mutually exclusive at runtime
  { "blemode",        "Get/set BLE mode [server|client].",                     true, cmd_blemode,         "Usage: blemode [server|client]" },

  // App-layer Secure Channel v1 (X25519+PSK+ChaCha20-Poly1305; no BLE bonding)
  { "blesecret", "Set/clear the BLE Secure Channel passphrase: blesecret <phrase|clear>.", true, cmd_blesecret, "Usage: blesecret <passphrase|clear>", /*requiresSuperAdmin=*/true },
  { "blesecure", "Require app-layer BLE encryption [on|off].",                            true, cmd_blesecure, "Usage: blesecure [on|off]" },

  // Auto-reconnect (boot + mid-session drop) to saved-MAC peers. Pairing is
  // separate (`openg2 auto`, `ringconnect`) and always saves the MAC; this
  // flag controls whether reconnect runs automatically.
  { "bleautoreconnect",  "Per-peer auto-reconnect (boot + mid-session drop): bleautoreconnect <name> [on|off]. `blepeers` lists names.", true, cmd_bleautoreconnect, "Usage: bleautoreconnect <peer-name> [on|off]\n  on: reconnect at boot and reseek after unexpected drops (not after ringdisconnect/closeg2)" },
  { "blepeers",        "List all registered BLE peers and their state.",                                          false, cmd_blepeers,        nullptr },
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
  { "bleEnabled", SETTING_BOOL, &gSettings.bleEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "bleenabled" },
  { "bluetoothAutoStart",    SETTING_BOOL,   &gSettings.bleAutoStart,    false, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "bleautostart" },
  { "bluetoothRequireAuth",  SETTING_BOOL,   &gSettings.bleRequireAuth,  true, 0, nullptr, 0, 1, "Require Authentication", nullptr, false, nullptr, "blerequireauth" },
  { "bluetoothDeviceName", SETTING_STRING, &gSettings.bleDeviceName, 0, 0, "HardwareOne", 0, 0, "Device Name", nullptr, false, nullptr, "blename" },
  { "bluetoothTxPower",      SETTING_INT,    &gSettings.bleTxPower,            3, 0, nullptr, 0, 7, "TX Power (0-7)", nullptr, false, nullptr, "bletxpower" },
  { "bluetoothMode",         SETTING_INT,    &gSettings.bleMode,               1,    0, nullptr, 0, 1, "Mode (0=server, 1=g2)", "0|Server,1|Client (G2)", false, nullptr, "blemode" },
  { "bleRequireSecureChannel", SETTING_BOOL, &gSettings.bleRequireSecureChannel, true, 0, nullptr, 0, 1, "Require Secure Channel", nullptr, false, nullptr, "blesecure" },
  { "bleSecureChannelSecret",  SETTING_STRING, &gSettings.bleSecureChannelSecret, 0, 0, "", 0, 0, "Secure Channel Secret", nullptr, true, nullptr, "blesecret" },
};

const size_t bluetoothSettingsCount = sizeof(bluetoothSettingsEntries) / sizeof(bluetoothSettingsEntries[0]);

// Register Bluetooth settings module
// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule bluetoothSettingsModule = {
  "bluetooth",
  "network.bluetooth",
  bluetoothSettingsEntries,
  bluetoothSettingsCount,
  bleSubsystemActive,
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

// Sensor cache types/globals come from the real sensor headers (included at the
// top of this file) — deliberately NOT re-declared here. See that include block.

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

// Escape `src` as JSON string CONTENT into a fixed buffer, never emitting a
// partial escape sequence. Returns bytes written (excluding the NUL).
//
// Deliberately not System_Utils.cpp's jsonEscape(): that one takes and returns a
// String (heap churn on a BLE notify path) and gives the caller no way to bound
// the result. Here the whole point is a hard bound. If the next character's
// escape would not fit we stop on a character boundary, so the output is always
// valid JSON string content — the failure mode is a shortened message, never a
// dangling backslash that corrupts the document.
static size_t jsonEscapeInto(char* dst, size_t dstCap, const char* src) {
  if (!dst || dstCap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; src && src[i]; i++) {
    const unsigned char c = (unsigned char)src[i];
    char        u[7];
    const char* esc = nullptr;
    switch (c) {
      case '"':  esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\n': esc = "\\n";  break;
      case '\r': esc = "\\r";  break;
      case '\t': esc = "\\t";  break;
      default:
        // Other C0 controls are illegal raw in a JSON string; \u-escape them.
        if (c < 0x20) { snprintf(u, sizeof(u), "\\u%04X", (unsigned)c); esc = u; }
        break;
    }
    const size_t need = esc ? strlen(esc) : 1;
    if (o + need >= dstCap) break;          // clean stop, never mid-sequence
    if (esc) { memcpy(dst + o, esc, need); o += need; }
    else     { dst[o++] = (char)c; }
  }
  dst[o] = '\0';
  return o;
}

bool blePushEvent(BLEEventType eventType, const char* message, const char* details) {
  if (!isBLEConnected() || !pEventNotifyChar) {
    return false;
  }
  if (bleScRequired()) return false;  // not on the Secure Channel — don't leak plaintext

  // Build event JSON.
  //
  // `message` reaches here straight from an unbounded CLI argument (cmd_bleevent,
  // registered requiresAdmin=false), so BOTH of the following are required and
  // neither was present before:
  //
  //  1. Bounds. snprintf returns the WOULD-BE length, so a long message drove
  //     `pos` past sizeof(eventJson); the next call then got a base pointer past
  //     the array AND a size_t-underflowed size (sizeof - pos), writing off the
  //     stack frame — and setValue() was handed that same oversize `pos` to read
  //     back. Every append is now bounds-checked before it is trusted.
  //  2. Escaping. The strings are interpolated with a bare %s INSIDE JSON string
  //     literals. A message containing a quote didn't just corrupt the document —
  //     since it lands mid-object, it could close the string and inject keys. Same
  //     class as the FM RDS fix (i2csensor_rda5807.cpp rdsScrub); escaped rather
  //     than scrubbed here because the message is operator-authored text a client
  //     should receive intact.
  //
  // Escape-then-measure, never clamp-then-escape: clamping raw input first can
  // still overflow (escaping expands), and clamping escaped output can cut a
  // \" in half and emit invalid JSON. jsonEscapeInto stops on a clean boundary.
  char msgEsc[192], detEsc[96];
  jsonEscapeInto(msgEsc, sizeof(msgEsc), message ? message : "");
  const bool haveDetails = (details && details[0] != '\0');
  if (haveDetails) jsonEscapeInto(detEsc, sizeof(detEsc), details);

  char eventJson[512];
  int pos = snprintf(eventJson, sizeof(eventJson),
                     "{\"type\":%d,\"msg\":\"%s\"", eventType, msgEsc);
  if (pos < 0 || (size_t)pos >= sizeof(eventJson)) return false;

  if (haveDetails) {
    int n = snprintf(eventJson + pos, sizeof(eventJson) - pos,
                     ",\"details\":\"%s\"", detEsc);
    if (n < 0 || (size_t)n >= sizeof(eventJson) - pos) return false;
    pos += n;
  }

  int n = snprintf(eventJson + pos, sizeof(eventJson) - pos,
                   ",\"ts\":%lu}", millis());
  if (n < 0 || (size_t)n >= sizeof(eventJson) - pos) return false;
  pos += n;

  pEventNotifyChar->setValue((uint8_t*)eventJson, pos);
  pEventNotifyChar->notify();
  
  if (gBLEState) {
    gBLEState->eventCount++;
  }
  
  return true;
}

// -----------------------------------------------------------------------------
// Notification sink (NSINK_APP) — one card to one authenticated session.
//
// WIRE FORMAT (the contract the companion app parses):
//
//   #NOTIF {"kind":"...","level":"...","title":"...","msg":"...","ms":N}\n
//
// It rides the ordinary CMD_RESPONSE reply lane rather than the Data service's
// event_notify characteristic, for two reasons: that characteristic is plaintext
// and self-suppresses whenever the Secure Channel is required (blePushEvent
// above), and it is characteristic-wide — it cannot be aimed at one session.
// Notifications are viewer-filtered per logged-in user, so they MUST be directed.
//
// The `#NOTIF ` sentinel is load-bearing, not decoration. The app diverts reply
// text into an off-console capture buffer while a command is in flight, and a
// capture only begins claiming on a fragment starting with '{'. Leading with '#'
// means an unsolicited card can never *start* a capture and be mistaken for the
// reply someone is waiting on. (A card arriving mid-capture is the app's problem
// to reject on this same prefix — that is the one app-side change this needs.)
//
// Everything interpolated below except `msg` is an internal literal (kind names
// from kEventKindNames, family titles, levelName()), but `msg` carries formatted
// event text — usernames, SSIDs, file names. It is escaped, never scrubbed, with
// the same escape-then-measure discipline as blePushEvent: clamping raw input
// first can still overflow once escaping expands it, and clamping escaped output
// can cut a \" in half. jsonEscapeInto stops on a clean character boundary.
//
// SIZING — a card is capped at 195 bytes, which is exactly SC_MAX_PAY_FRAME
// (the Secure Channel's 200-byte plaintext frame minus its 5-byte app header):
//
//  - Secure link: it is provably ONE frame, so a card costs one notify and can
//    never pay the 30 ms inter-fragment pacing.
//  - Plaintext link: bleRawNotifyToSession does ONE esp_ble_gatts_send_indicate
//    with the whole length and does not fragment, so an over-MTU card would be
//    dropped by the stack while we counted it as sent. Staying inside the SC
//    frame budget means any peer that can carry ordinary traffic can carry a
//    card: the channel itself already refuses to establish below ~230 MTU (see
//    SC_MAX_PT_PER_FRAME), and 195 bytes needs only ~198. That is why this needs
//    no MTU probe — the firmware tracks no negotiated MTU anywhere, and a card
//    is strictly smaller than the frames the rest of the link already depends on.
//
// `msg` is escaped DIRECTLY into the envelope with the tail space reserved,
// rather than through a scratch buffer, so the cap is enforced in exactly one
// place. Escaping can expand a byte 6x (\u00XX for a C0 control), and event
// subjects carry attacker-influenced text — remote peer names, SSIDs — so a
// 63-char body CAN exceed the envelope. jsonEscapeInto stops on a clean
// character boundary, so the failure mode is a visibly shortened message and
// never a malformed document. §9 of docs/APP_JSON_CONTRACT.md states this.
bool blePushNotification(uint16_t connId, TransportSessionEpoch expectedEpoch,
                         const char* kindName, const char* level,
                         const char* title, const char* msg, uint16_t durMs) {
  if (!isBLEConnected()) return false;

  char line[196];  // 195 usable + NUL == SC_MAX_PAY_FRAME, i.e. one frame exactly
  // Reserve the closing `","ms":65535}\n` + NUL so escaping can never consume
  // the space the document needs to terminate.
  const size_t kTailReserve = 20;

  int pos = snprintf(line, sizeof(line),
                     "#NOTIF {\"kind\":\"%s\",\"level\":\"%s\",\"title\":\"%s\",\"msg\":\"",
                     kindName ? kindName : "", level ? level : "",
                     title ? title : "");
  if (pos < 0 || (size_t)pos + kTailReserve >= sizeof(line)) return false;

  pos += (int)jsonEscapeInto(line + pos, sizeof(line) - (size_t)pos - kTailReserve,
                             msg ? msg : "");

  const int tail = snprintf(line + pos, sizeof(line) - (size_t)pos,
                            "\",\"ms\":%u}\n", (unsigned)durMs);
  if (tail < 0 || (size_t)tail >= sizeof(line) - (size_t)pos) return false;
  const int n = pos + tail;

  // blocking=false: the notify tick runs on the main loop, and this fires once
  // per authenticated session per event. On a secure link it skips waiting for
  // the tx mutex (a 4 KB paced reply holds it ~630 ms); on a plaintext link it
  // collapses the congestion retry budget to one attempt (else up to 6 x 15 ms
  // of vTaskDelay). Either way: drop rather than stall — the caller counts it.
  return sendBLEResponseToSession(connId, expectedEpoch, line, (size_t)n,
                                  /*blocking=*/false);
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
  if (gThermalRunning && gThermalConnected) {
    SensorCacheGuard g(gThermalCache.mutex, pdMS_TO_TICKS(10), "ble.thermalStream");
    if (g.held && gThermalCache.thermalDataValid) {
      // "avg" (was "center"): ThermalCache has no center field — the old local
      // struct invented thermalCenterTemp/HottestX/HottestY, so this read landed
      // on thermalMinTemp while min/max read the interpolated width/height ints
      // punned as floats (both printed 0.0). thermalAvgTemp is the real field.
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"thermal\":{\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f,\"valid\":true},",
                      gThermalCache.thermalMinTemp,
                      gThermalCache.thermalMaxTemp,
                      gThermalCache.thermalAvgTemp);
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gTofRunning && gTofConnected) {
    SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(10), "ble.tofStream");
    if (g.held && gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"tof\":{\"dist_mm\":%d,\"valid\":true},",
                      gTofCache.tofObjects[0].distance_mm);
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gImuRunning && gImuConnected) {
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "ble.imuStream");
    if (g.held && gImuCache.imuDataValid) {
      // Real field names are oriYaw/oriPitch/oriRoll. The old local struct omitted
      // imuTemp, shifting everything after it: "heading" read imuTemp (streamed
      // TEMPERATURE), pitch read oriYaw, roll read oriPitch, and oriRoll was never
      // sent at all. JSON keys kept (heading == yaw) so the wire format is stable.
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"imu\":{\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"valid\":true},",
                      gImuCache.oriYaw,
                      gImuCache.oriPitch,
                      gImuCache.oriRoll);
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
  // This entry point is called every main-loop lap even when there is no BLE
  // client. Keep OTA upload cleanup ahead of the connection early-return so a
  // disconnect cannot retain an open partial file indefinitely.
  otaBleUploadHousekeeping();
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
