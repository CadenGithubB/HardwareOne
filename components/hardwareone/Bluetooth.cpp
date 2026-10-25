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
#include "System_Mutex.h"  // FsLockGuard for authentication
#include "System_PollPause.h"   // PollPauseGuard — quiesce sensor polling during BLE init
#include "BLE_PhonePipe.h"
#include "BLE_PhonePipeConsole.h"  // the console as pipe stream slot 2 (phone link v2)
#include "BLE_PhonePairCode.h"  // `phone secret` stores a pairing code canonical (phone link v2)
#include "System_InternalSecretString.h"  // the `phone secret` steps copy into internal RAM
#include "BLE_LinkStatus.h"  // blestatus / blemode: role, app console, relay pipe
#include "BLE_CoexRecords.h"
#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"   // the phone-link console shares the controller TX gate
#include "BLE_SetupWorker.h"
#include "BLE_CentralStack.h"  // the shared client stack: openble / closeble, the switch to the console server
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
#include "System_OTA.h"     // encrypted BLE OTA binary-frame sink

// Sensor caches for the BLE sensor-stream path (buildSensorDataJSON). Pull in the
// REAL headers — never hand-copy these structs. This file used to re-declare
// ThermalCache/TofCache/ImuCache locally with stale layouts; because it never
// included these headers the compiler could not see the conflict, so it linked to
// the real symbols and read them at the WRONG OFFSETS (the IMU stream published
// imuTemp as "heading", never sent roll, and gated validity on a millis() byte).
// System_SensorStubs.h supplies the types/globals when a sensor is compiled out.
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"   // ThermalCache, readings, gThermalRunning/Connected
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"   // TofCache, readings, gTofRunning/Connected
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"     // ImuCache, readings, gImuRunning/Connected
#endif
#include "System_SensorStubs.h"     // stubs for disabled sensors
#include "System_SelfDevice.h"  // SelfDevice::firmwareVersion() for the Device Info BLE characteristic
#include "System_Settings.h"
#include "System_BleSecureChannel.h"  // app-layer Secure Channel v1 (replaces BLE link-layer bonding)
#include "System_AuthIdentity.h"      // currentAuthContext() — gate blesecret to trusted transports
#include <atomic>
#include <cctype>                     // passphrase complexity policy
#include <new>                        // placement-new for PSRAM-resident BLESystemState
#include <freertos/semphr.h>

#include <esp_gatts_api.h>
#include "HAL_Bluetooth.h"    // native or companion controller lifecycle
#include "System_RadioCompanion.h"  // radio companion gate (P4X-EYE); no-op with an on-chip radio
#include <stdlib.h>
#include <string.h>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

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

// ESP32 BLE objects
static BLEServer* pServer = nullptr;
static BLEAdvertising* pAdvertising = nullptr;
// Connectable advertising runs while the console has room for another app.
// Bluedroid stops it on every LE connection, so each connect and disconnect
// below the maximum re-arms it. Before, one app link (or a stale one) left the
// device invisible until every link was gone.
static std::atomic<bool> sBleAdvertising{false};
// The console server is up, for status reads that must not touch gBLEState
// (deinit frees it). Set once init completes; cleared when teardown starts.
static std::atomic<bool> sBleServerUp{false};
// Where the command service lives (bleConsoleMode). Server is set with
// gBLEState->initialized and cleared once deinitBluetooth frees the state;
// ClientRegistered lasts from bleConsoleRegisterOnClientHost to the host
// teardown that frees its state.
static std::atomic<hw1console::Mode> sBleConsoleMode{hw1console::Mode::Off};
// The console runs on the phone pipe's link (client host). Builds without the
// pipe have no such link: this folds to false and the client-host paths drop
// out of the image.
static inline bool bleConsoleClientHost() {
  return ENABLE_BLE_PHONE_PIPE &&
         sBleConsoleMode.load() == hw1console::Mode::ClientRegistered;
}
// Client host only: a console slot may be made on the pipe's link. Opened when
// registration completes; closed by the host teardown before the host stops.
static std::atomic<bool> sBleConsoleAdmissionOpen{false};
// Client host only: the console slot on the phone pipe's link and the pipe
// generations whose link has gone (BLE_ConsoleAttachCore.h), with the slot last
// logged. Read and written under the lifecycle lock; reset with the host.
static hw1console::Attach sBleConsoleAttach;
static hw1console::Slot sBleConsoleLogged;
// The slot's connection id (-1: none), mirrored for the lock-free link-event
// check (bleConsoleLinkEventMayMatter). Written with sBleConsoleAttach.
static std::atomic<int> sBleConsoleSlotConnId{-1};
// Client host only: directed posts admitted under the lifecycle lock and made
// after it is released (bleRawNotifyToSession). The host teardown waits for
// them to land before the host stops.
static std::atomic<int> sBleConsoleSendsInFlight{0};
// Who holds gBLEState (phone link v2): the server role, the client host's
// command service and the phone pipe, whose console row lives there. Under the
// lifecycle lock. The last holder out frees it (bleStateReleaseLocked); a host
// teardown ends the pipe's row but leaves its hold, so a pipe that outlives
// the host (recovery) binds into the same state on the next one.
constexpr uint8_t kBleHoldServer = 0x01, kBleHoldClientConsole = 0x02, kBleHoldPipe = 0x04;
static uint8_t sBleStateHolders = 0;
// gBLEState->initialized (the server role's state is up), kept beside it under
// the lifecycle lock for isBleServerInitialized() while that lock is busy.
static std::atomic<bool> sBleServerStateInitialized{false};

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

// Device info characteristics
static BLECharacteristic* pManufacturerChar = nullptr;
static BLECharacteristic* pModelChar = nullptr;
static BLECharacteristic* pFirmwareChar = nullptr;

// Forward declarations
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len);
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len, bool blocking);

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
// The server app's registration status, for the client host, which checks it
// before creating a service (createService waits forever on an app that never
// registered). Arduino releases createServer's wait before this tap runs, so
// the registering task waits briefly for it to leave kBleGattsRegPending.
constexpr int kBleGattsRegPending = -1;
std::atomic<int> sBleGattsRegStatus{kBleGattsRegPending};

void bleGattsEventTap(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf,
                      esp_ble_gatts_cb_param_t* param) {
  if (event == ESP_GATTS_REG_EVT && param) {
    if (param->reg.status == ESP_GATT_OK) {
      sBleGattsIf.store(static_cast<int>(gattsIf), std::memory_order_release);
    }
    sBleGattsRegStatus.store(static_cast<int>(param->reg.status), std::memory_order_release);
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

// The phone link's console row (phone link v2): its slot (-1: none) and the
// leaf snapshot of it that the pipe side reads without the lifecycle lock
// (blePipeConsoleIdentity). Both change only under the lifecycle lock, and the
// snapshot follows every epoch the row's slot takes (the two helpers below).
int sBlePipeRowSlot = -1;
portMUX_TYPE sBlePipeIdentityMux = portMUX_INITIALIZER_UNLOCKED;
BlePipeConsoleIdentity sBlePipeIdentity;

// The caller holds sBleLifecycleMutex.
void blePipeIdentitySyncLocked() {
  BlePipeConsoleIdentity id;
  const int slot = sBlePipeRowSlot;
  if (gBLEState && slot >= 0 && slot < BLE_MAX_CONNECTIONS &&
      gBLEState->connections[slot].active && gBLEState->connections[slot].pipeConsole) {
    const BLEConnection& c = gBLEState->connections[slot];
    id.bound = true;
    id.generation = c.pipeGeneration;
    id.streamId = c.pipeStream;
    id.rowConnId = c.connId;
    id.epoch = sBleSessionEpoch[slot].load(std::memory_order_acquire);
    id.authed = id.epoch != 0 && c.authed && c.user.length() > 0;
  }
  portENTER_CRITICAL(&sBlePipeIdentityMux);
  sBlePipeIdentity = id;
  portEXIT_CRITICAL(&sBlePipeIdentityMux);
}

// The caller holds sBleLifecycleMutex.
void bleInvalidateSessionSlotLocked(int slot) {
  if (slot < 0 || slot >= BLE_MAX_CONNECTIONS) return;
  TransportSessionEpoch old =
      sBleSessionEpoch[slot].exchange(0, std::memory_order_acq_rel);
  transportSessionClose(SOURCE_BLUETOOTH, old);
  if (slot == sBlePipeRowSlot) blePipeIdentitySyncLocked();
}

// The caller holds sBleLifecycleMutex.
TransportSessionEpoch blePublishSessionSlotLocked(int slot) {
  if (slot < 0 || slot >= BLE_MAX_CONNECTIONS) return 0;
  bleInvalidateSessionSlotLocked(slot);
  TransportSessionEpoch epoch = transportSessionOpen(SOURCE_BLUETOOTH);
  if (epoch != 0) {
    sBleSessionEpoch[slot].store(epoch, std::memory_order_release);
  }
  if (slot == sBlePipeRowSlot) blePipeIdentitySyncLocked();
  return epoch;
}
}  // namespace

// The phone link's console row (BLEConnection::pipeConsole); none exist in a
// build without the pipe.
static inline bool blePipeRow(const BLEConnection& c) {
  return ENABLE_BLE_PHONE_PIPE && c.pipeConsole;
}
// connId names the console row. An id never changes kind: Bluedroid's are
// below 0x8000 and a console row's never are (blePipeConsoleRowId).
static bool bleIsPipeRowId(uint16_t connId) {
  BleLifecycleGuard guard;
  if (!guard) return false;
  const int slot = findConnectionSlotByConnIdLocked(connId);
  return slot >= 0 && blePipeRow(gBLEState->connections[slot]);
}
// Ends the phone link's console row (Bluetooth.cpp's console-row section).
static bool blePipeRowEndLocked(bool match = false, uint32_t generation = 0,
                                uint16_t streamId = 0, PipeConsoleTarget* ended = nullptr);

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
  // The phone link's console row: the pipe side's snapshot drops it first.
  if (slot == sBlePipeRowSlot) {
    sBlePipeRowSlot = -1;
    blePipeIdentitySyncLocked();
  }

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
  gBLEState->connections[slot].mirrorEligible = false;
  gBLEState->connections[slot].noChannelRejected = false;
  gBLEState->connections[slot].lastActivityMs = 0;
  gBLEState->connections[slot].pipeConsole = false;
  gBLEState->connections[slot].pipeGeneration = 0;
  gBLEState->connections[slot].pipeStream = 0;
  gBLEState->connections[slot].pipeLinkConnId = 0;
  gBLEState->connections[slot].noticePending = false;
  gBLEState->connections[slot].noticeEpoch = 0;
  gBLEState->connections[slot].noticeUser = "";
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
  // The console on the phone pipe's link stores no address (the pipe owns it):
  // design A's slot on the client host, and the console row in any mode.
  if (bleConsoleClientHost() || blePipeRow(gBLEState->connections[slot])) {
    return String("ble:pipe-link");
  }
  char macStr[18];
  macToDisplay(gBLEState->connections[slot].deviceAddr, macStr, sizeof(macStr));
  char ipBuf[28];
  snprintf(ipBuf, sizeof(ipBuf), "ble:%s", macStr);
  return String(ipBuf);
}

// A session must log in before it runs commands: when blerequireauth is on,
// and always on the phone link's console row, whose pairing authenticates the
// link and never a user (phone link v2).
static bool bleRequiresLogin(uint16_t connId) {
  return gSettings.bleRequireAuth || bleIsPipeRowId(connId);
}

// Identity for BLE-submitted commands. When require-auth is off and nobody
// has logged in on this conn, stamp reserved "AuthBypass" (same as serial /
// OLED) so authorizeCommand accepts the line and audit reads
// AuthBypass@bluetooth with the peer MAC in ip — not a blank user. Never on
// the phone link's console row (bleRequiresLogin).
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
  } else if (!bleRequiresLogin(connId)) {
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

// ConsoleClose reasons the console row asks the pipe for (BLE_PhonePipeCore.h):
// its stream closes after it drains and reopens at once with a fresh row.
constexpr uint16_t kBlePipeCloseRenewed = 2, kBlePipeCloseRevoked = 5;

// The caller holds the lifecycle lock: slot i's sign-in is revoked. The
// session epoch rotates, so queued replies and jobs fail closed. The phone
// link's console row ends instead, and *renew names its stream for CLOSE 5,
// which the caller requests with the lock released.
static void bleRevokeSlotLocked(int i, PipeConsoleTarget* renew) {
  BLEConnection& c = gBLEState->connections[i];
  if (blePipeRow(c)) {
    (void)blePipeRowEndLocked(/*match=*/true, c.pipeGeneration, c.pipeStream, renew);
    return;
  }
  bleInvalidateSessionSlotLocked(i);
  c.authed = false;
  c.user = "";
  (void)blePublishSessionSlotLocked(i);
}

int bleRevokeUserSessions(const String& username) {
  int revoked = 0;
  PipeConsoleTarget renew;
  {
    BleLifecycleGuard guard;
    if (!guard) return 0;
    if (!gBLEState || username.length() == 0) return 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (!gBLEState->connections[i].active) continue;
      if (!gBLEState->connections[i].authed) continue;
      if (!gBLEState->connections[i].user.equalsIgnoreCase(username)) continue;
      bleRevokeSlotLocked(i, &renew);
      revoked++;
    }
    BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked %d BLE sessions for user '%s'", revoked, username.c_str());
  }
  if (renew.streamId) pipeConsoleRequestClose(renew.generation, renew.streamId, kBlePipeCloseRevoked);
  return revoked;
}

int bleRevokeAllSessions() {
  int revoked = 0;
  PipeConsoleTarget renew;
  {
    BleLifecycleGuard guard;
    if (!guard) return 0;
    if (!gBLEState) return 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (!gBLEState->connections[i].active) continue;
      if (!gBLEState->connections[i].authed) continue;
      bleRevokeSlotLocked(i, &renew);
      revoked++;
    }
    BLE_DEBUGF(DEBUG_BLE_CORE, "Revoked all BLE sessions (%d total)", revoked);
  }
  if (renew.streamId) pipeConsoleRequestClose(renew.generation, renew.streamId, kBlePipeCloseRevoked);
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
// mirrorEligible latches the console-mirror rule resolved at login.
static bool bleBindSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const String& user, bool mirrorEligible,
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
  gBLEState->connections[slot].mirrorEligible = mirrorEligible;
  gBLEState->connections[slot].lastActivityMs = millis();
  boundEpochOut = blePublishSessionSlotLocked(slot);
  if (boundEpochOut == 0) {
    gBLEState->connections[slot].authed = false;
    gBLEState->connections[slot].user = "";
    gBLEState->connections[slot].mirrorEligible = false;
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

// Upper-cased copy of a saved peer MAC text into a caller buffer (no heap).
static void copyMacUpper(char* dst, size_t dstCap, const char* src) {
  if (!dst || dstCap == 0) return;
  const size_t len = src ? strnlen(src, dstCap - 1) : 0;
  if (len) memcpy(dst, src, len);
  dst[len] = '\0';
  for (size_t i = 0; i < len; i++) {
    dst[i] = (char)toupper((unsigned char)dst[i]);
  }
}

// True when a saved peer MAC (any case) names the connecting address
// (canonical upper-case display form). An empty saved slot never matches.
static bool bleSavedMacMatches(const char* macStr, const char* saved) {
  char upper[18];
  copyMacUpper(upper, sizeof(upper), saved);
  return upper[0] != '\0' && strcmp(macStr, upper) == 0;
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

// The lifecycle lock is held. No logging: the server callbacks call this on
// BTC_TASK. False when the console is down or full.
static bool bleAdvertiseIfRoomLocked() {
  if (!gBLEState || !gBLEState->initialized || !pAdvertising) return false;
  if (gBLEState->activeConnectionCount >= BLE_MAX_CONNECTIONS) return false;
  if (!pAdvertising->start()) return false;
  sBleAdvertising.store(true);
  if (gBLEState->activeConnectionCount == 0) {
    gBLEState->connectionState = BLE_STATE_ADVERTISING;
  }
  return true;
}

// Identify device type by MAC address (zero heap allocations). Each call reads
// the peer owner's current saved targets (BLE_Peers.h snapshot, a short
// critical-section copy), so a re-paired glasses, ring or phone is typed by
// its new address from the next connection on.
BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t* mac) {
  if (!mac) return BLE_DEVICE_UNKNOWN;
  char macStr[18];
  macToStackBuf(mac, macStr);

  BlePeerSavedTargetSnapshot peer{};
  (void)blePeerSavedTargetSnapshot(BLE_PEER_G2_GLASSES, peer);
  if (bleSavedMacMatches(macStr, peer.target.mac1)) return BLE_DEVICE_GLASSES_LEFT;
  if (bleSavedMacMatches(macStr, peer.target.mac2)) return BLE_DEVICE_GLASSES_RIGHT;
  (void)blePeerSavedTargetSnapshot(BLE_PEER_R1_RING, peer);
  if (bleSavedMacMatches(macStr, peer.target.mac1)) return BLE_DEVICE_RING;
  char phoneConsoleAddress[BLE_PEER_ADDRESS_TEXT_CAPACITY];
  blePeerPhoneConsoleAddressSnapshot(phoneConsoleAddress);
  if (bleSavedMacMatches(macStr, phoneConsoleAddress)) return BLE_DEVICE_PHONE;
  return BLE_DEVICE_UNKNOWN;
}

// =============================================================================
// APP CONSOLE ON THE PHONE PIPE'S LINK (client host)
// =============================================================================
// In G2 client mode the command service sits on the client host and Bluedroid
// reports every link to it. BLE_ConsoleAttachCore.h decides which link holds
// the console slot: only the authenticated, READY pipe's, found by its live
// connection id. These helpers apply its plans under the lifecycle lock. The
// server callbacks and the write fast path run them on BTC_TASK, so they wait
// only for that lock and never log.

// The pipe's link record (a portMUX copy, BTC-safe).
static hw1console::PipeLink bleConsolePipeLink() {
  PhonePipeLinkRecord record;
  const bool valid = phonePipeLinkRecord(&record);
  return hw1console::PipeLink{valid, record.generation, record.connId};
}

// Ends the slot's session: its epoch (queued replies and login jobs fail
// closed), Secure Channel keys and sign-in. The pipe keeps the link. A slot no
// app had joined was never counted, so it ends without a count or event.
static void bleConsoleEndSlotLocked(const hw1console::Slot& old) {
  if (!gBLEState || findConnectionSlotByConnIdLocked(old.connId) < 0) return;
  bleClearConnectionByConnId(old.connId);
  bleScReset(old.connId);
  if (!old.joined) return;
  if (gBLEState->activeConnectionCount > 0) gBLEState->activeConnectionCount--;
  gBLEState->deferredDisconnectActiveCount = gBLEState->activeConnectionCount;
  gBLEState->deferredDisconnectPending = true;
  if (gBLEState->activeConnectionCount == 0) gBLEState->connectionState = BLE_STATE_IDLE;
  char remaining[8];
  snprintf(remaining, sizeof(remaining), "%d", gBLEState->activeConnectionCount);
  systemEventPost(SYSEVT_BLE_DISCONNECTED, remaining);
}

// Makes the slot on the pipe's link, typed Phone, with a fresh session and no
// stored address: a session waiting for the app, not yet counted. False when
// no slot or session is free; the next reconcile tries again.
static bool bleConsoleMakeSlotLocked(uint16_t connId) {
  if (!gBLEState) return false;
  int slot = -1;
  for (int i = 0; i < BLE_MAX_CONNECTIONS && slot < 0; i++) {
    if (!gBLEState->connections[i].active) slot = i;
  }
  if (slot < 0) return false;
  BLEConnection& c = gBLEState->connections[slot];
  c.active = true;
  c.connId = connId;
  c.connectedSince = millis();
  memset(c.deviceAddr, 0, sizeof(c.deviceAddr));
  c.deviceType = BLE_DEVICE_PHONE;
  c.deviceName = bleDeviceTypeToString(BLE_DEVICE_PHONE);
  c.commandsReceived = 0;
  c.authed = false;
  c.user = "";
  c.mirrorEligible = false;
  c.noChannelRejected = false;
  c.lastActivityMs = millis();
  bleScReset(connId);  // a fresh secure channel for this session
  if (blePublishSessionSlotLocked(slot) == 0) {
    bleClearConnectionByConnId(connId);
    return false;
  }
  return true;
}

// The app joined the slot (its first admitted write): from now on it is a
// connected app, for the connection state, the counts and the connect event.
static void bleConsoleJoinLocked(uint16_t connId) {
  const int slot = findConnectionSlotByConnIdLocked(connId);
  if (!gBLEState || slot < 0) return;
  gBLEState->activeConnectionCount++;
  gBLEState->totalConnections++;
  gBLEState->connectionState = BLE_STATE_CONNECTED;
  gBLEState->deferredConnectSlot = slot;
  gBLEState->deferredConnectPending = true;
  systemEventPost(SYSEVT_BLE_CONNECTED, bleDeviceTypeToString(BLE_DEVICE_PHONE));
}

// Stores the attach state with its slot's id mirror.
static void bleConsoleStoreLocked(const hw1console::Attach& next) {
  sBleConsoleAttach = next;
  sBleConsoleSlotConnId.store(next.slot.present ? int(next.slot.connId) : -1);
}

// Applies a plan: the old session ends first, then the new slot is made or
// the app's first write joins the slot.
static void bleConsoleApplyLocked(const hw1console::Plan& plan) {
  if (plan.clear) bleConsoleEndSlotLocked(sBleConsoleAttach.slot);
  hw1console::Attach next = plan.next;
  if (plan.create && !bleConsoleMakeSlotLocked(next.slot.connId)) next.slot = hw1console::Slot{};
  if (plan.join) bleConsoleJoinLocked(next.slot.connId);
  bleConsoleStoreLocked(next);
}

// The slot follows the pipe's record, `bleconsole`, blesecret and admission.
static void bleConsoleReconcileLocked() {
  hw1console::Inputs in;
  in.mode = sBleConsoleMode.load();
  in.enabled = gSettings.bleConsoleOnPipe && sBleConsoleAdmissionOpen.load();
  in.secret = gSettings.bleSecureChannelSecret.length() > 0;
  in.pipe = bleConsolePipeLink();
  bleConsoleApplyLocked(hw1console::reconcile(in, sBleConsoleAttach));
}

// ServerCallbacks, before the lifecycle lock: on the client host only an event
// on the slot's or the pipe record's id can change anything, so the temples
// and the ring return without waiting for the lock. A slot or record that
// appears meanwhile sits on a link still coming up or on a dropped id (the
// race BLE_ConsoleAttachCore.h covers), which that id's next event ends.
static bool bleConsoleLinkEventMayMatter(uint16_t connId) {
  if (!bleConsoleClientHost()) return true;
  const hw1console::PipeLink pipe = bleConsolePipeLink();
  return (pipe.valid && pipe.connId == connId) || sBleConsoleSlotConnId.load() == int(connId);
}

// ServerCallbacks: a link came up or went down on connId. True in server mode,
// whose path runs as before. On the client host nothing is admitted and only a
// slot on that id ends: no other link changes a count, event or advertising.
static bool bleConsoleLinkEventLocked(bool up, uint16_t connId) {
  const hw1console::Mode mode = sBleConsoleMode.load();
  const hw1console::PipeLink pipe = bleConsolePipeLink();
  const hw1console::Plan plan = up ? hw1console::onConnect(mode, connId, pipe, sBleConsoleAttach)
                                   : hw1console::onDisconnect(mode, connId, pipe, sBleConsoleAttach);
  if (plan.server) return true;
  bleConsoleApplyLocked(plan);
  return false;
}

// A request characteristic write (BTC): true in server mode, and on the client
// host only on the console slot's link. Every write reconciles first: the
// phone's first write can beat the main loop's pass after the pipe went READY,
// and a slot whose pipe left READY (closephone, blesecret clear, the setting
// turned off) takes no command meanwhile. The first admitted write joins the
// slot; console writes count as pipe activity.
static bool bleConsoleAdmitWriteLocked(uint16_t connId) {
  if (!bleConsoleClientHost()) return true;
  bleConsoleReconcileLocked();
  if (!sBleConsoleAttach.slot.present || sBleConsoleAttach.slot.connId != connId) return false;
  bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, connId));
  phonePipeNoteConsoleActivity();
  return true;
}

// bleUpdateStreams (main loop) and `bleconsole`: the slot follows the pipe.
// Task context; an attach or end is logged once, whichever path made it.
static void bleConsoleReconcile() {
  if (!bleConsoleClientHost()) return;
  const char* change = nullptr;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return;
    bleConsoleReconcileLocked();
    if (!hw1console::sameSlot(sBleConsoleLogged, sBleConsoleAttach.slot)) {
      change = sBleConsoleAttach.slot.present ? "App console attached to the phone pipe's link"
                                              : "App console session on the phone link ended";
      sBleConsoleLogged = sBleConsoleAttach.slot;
    }
  }
  if (change) BLE_DEBUGF(DEBUG_BLE_CORE, "%s", change);
}

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    // NOTE: This callback runs on BTC_TASK with limited stack - avoid heavy operations
    // Use deferred flag pattern for logging (ISR-safe)
    // On the client host Bluedroid reports every link here (temples, ring,
    // phone): none is admitted by a connect, and none gets a slot, count,
    // event or advertising call; one on the console slot's id ends it
    // (BLE_ConsoleAttachCore.h), and the others return before the lock.
    // Server mode admits its apps below.
    if (!param || !bleConsoleLinkEventMayMatter(param->connect.conn_id)) return;
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return;
    if (!bleConsoleLinkEventLocked(/*up=*/true, param->connect.conn_id)) return;
    
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
    gBLEState->connections[slot].mirrorEligible = false;
    gBLEState->connections[slot].lastActivityMs = millis();
    bleScReset(param->connect.conn_id);  // fresh secure-channel state for this connection

    // Type the link from the peer owner's saved targets (stack only, no heap)
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
    
    // Bluedroid stopped advertising for this connection: re-arm it while
    // another app still fits (bleAdvertiseIfRoomLocked, BTC-safe).
    sBleAdvertising.store(false);
    if (!bleAdvertiseIfRoomLocked()) BLEDevice::stopAdvertising();

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
    // Client host: only the console slot on that id ends; a temple or ring
    // going down changes no count, event, connection state or advertising,
    // and returns before the lock.
    if (param && !bleConsoleLinkEventMayMatter(param->disconnect.conn_id)) return;
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return;
    const bool server = param ? bleConsoleLinkEventLocked(/*up=*/false, param->disconnect.conn_id)
                              : bleConsoleMode() == hw1console::Mode::Server;
    if (!server) return;
    
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
    
    // Re-arm advertising: there is room again (BTC-safe, no logging).
    (void)bleAdvertiseIfRoomLocked();

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
// Every write is copied and processed on the central cmd_exec task (processIncomingBLECommand)
class CmdRequestCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) override {
    // NOTE: Callback runs on BTC_TASK - defer logging to task context (ISR-safe pattern)
    if (!param) return;
    // Zero-copy read: getData()/getLength() return the characteristic's internal
    // buffer (getData() is (uint8_t*)m_value.c_str(), so still NUL-terminated),
    // avoiding the by-value getValue() String heap-copy on BTC_TASK for every
    // inbound write. The buffer is valid for this synchronous callback.
    const uint8_t* data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len > 0) {
      {
        BleLifecycleGuard lifecycle;
        if (!lifecycle || !gBLEState) return;
        // Client host: only the console slot's link takes commands.
        if (!bleConsoleAdmitWriteLocked(param->write.conn_id)) return;
        gBLEState->commandsReceived++;

        // Defer logging to task context
        gBLEState->deferredCmdReceivedConnId = param->write.conn_id;
        gBLEState->deferredCmdReceivedLen = len;
        gBLEState->deferredCmdReceivedPending = true;
      }

      BLE_DEBUGF(DEBUG_BLE_GATT,
                 "Command write conn=%u len=%u",
                 (unsigned)param->write.conn_id,
                 (unsigned)len);

      // processIncomingBLECommand copies the write into a pool slot and defers
      // it to cmd_exec_task; nothing on this task waits for a reply to go out
      processIncomingBLECommand(param->write.conn_id, (const char*)data, len);
    }
  }
};

// Status Characteristic - returns connection status. Bluedroid lets every link
// read it, and on the client host those are the temples, the ring and the
// phone: only the console slot's link reads the console's counters there;
// any other link (or the phone outside a console session) reads
// {"state":"unavailable"}. The slot's id is the lock-free mirror (BTC).
class CmdStatusCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) override {
    if (!gBLEState || (bleConsoleClientHost() &&
                       (!param || sBleConsoleSlotConnId.load() != int(param->read.conn_id)))) {
      pCharacteristic->setValue("{\"state\":\"unavailable\"}");
      return;
    }
    char statusBuf[128];
    snprintf(statusBuf, sizeof(statusBuf),
             "{\"state\":\"connected\",\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu}",
             (unsigned long)(millis() / 1000),
             (unsigned long)gBLEState->commandsReceived,
             (unsigned long)gBLEState->responsesSent);
    pCharacteristic->setValue(statusBuf);
    BLE_DEBUGF(DEBUG_BLE_GATT, "Status read: %s", statusBuf);
  }
};

// Arduino borrows these callback pointers. Their stateless implementations
// survive role changes; allocating them on every Server start leaks them.
static ServerCallbacks sServerCallbacks;
static CmdRequestCallbacks sCmdRequestCallbacks;
static CmdStatusCallbacks sCmdStatusCallbacks;

// =============================================================================
// COMMAND PROCESSING
// =============================================================================

// Command types + ExecAsyncCallback from shared header
#include "System_CommandTypes.h"

struct BleLoginAsyncJob {
  uint16_t connId;
  TransportSessionEpoch sessionEpoch;
  char user[kPublicUsernameMaxLen + 1];
  char password[kPublicPasswordMaxLen + 1];
};

// One queued command's reply target, and the command's own broadcastOutput
// lines waiting for it (bleCommandOutputLine). A batch is at most one plaintext
// notification, like the console mirror's flush. sendFailed: a batch's send to
// the phone link's console row failed (its stream took nothing within the
// driver's stall bounds), so the later batches are dropped rather than each
// tried on cmd_exec. Other rows keep every batch: their sends give up after
// 6 tries 15 ms apart, so a short congestion costs one batch, not the rest.
struct BleCommandReplyContext {
  static constexpr size_t kOutputMax = 512;
  uint16_t connId;
  TransportSessionEpoch sessionEpoch;
  size_t outputLen;
  bool sendFailed;
  char output[kOutputMax];
};
static void bleCommandOutputFlush(BleCommandReplyContext* reply);

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

// Inspect the complete bounded payload before previews/trace fragmentation.
// Raw notify also carries non-NUL-terminated encrypted frames, so never use
// strstr/String construction here or read beyond the caller's byte count.
static bool bleOutputHasPrivateSTT(const char* data, size_t len) {
  static constexpr char textMarker[] = "\"sttText\"";
  static constexpr char pathMarker[] = "\"transcriptPath\"";
  constexpr size_t textLen = sizeof(textMarker) - 1;
  constexpr size_t pathLen = sizeof(pathMarker) - 1;
  if (!data || len < textLen) return false;
  for (size_t i = 0; i <= len - textLen; ++i) {
    if (memcmp(data + i, textMarker, textLen) == 0 ||
        (len - i >= pathLen && memcmp(data + i, pathMarker, pathLen) == 0)) return true;
  }
  return false;
}

static void bleTxTrace(const char* tag, uint16_t connId, const char* data, size_t len) {
  if (!bleDataDebugEnabled()) return;
  if (bleOutputHasPrivateSTT(data, len)) {
    data = "[private STT result]";
    len = strlen(data);
  }
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
    bleCommandOutputFlush(reply);  // the command's own lines go before its result
    bleTxTrace("result", reply->connId, safeResult, resultLen);
    // Reliable even after a failed batch (results must not drop): on the
    // console row the driver's stall latch already makes it one try.
    sendBLEResponseToSession(reply->connId, reply->sessionEpoch, safeResult, resultLen);
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

  // Every central counts under Bluetooth's one attempt-table key (never the
  // connId, which the stack reuses, nor an address a central can rotate). A
  // locked key refuses before any password check, on this session only.
  const char* attemptPeer = credentialAttemptConsolePeer(SOURCE_BLUETOOTH);
  unsigned long lockoutRetryMs = 0;
  if (!credentialAttemptAllowed(SOURCE_BLUETOOTH, attemptPeer, &lockoutRetryMs)) {
    recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), false,
                               "Locked out");
    char locked[80];
    snprintf(locked, sizeof(locked),
             "[ble] Login locked out. Retry in %lu seconds.",
             (lockoutRetryMs + 999UL) / 1000UL);
    sendBLEResponseToSession(job->connId, job->sessionEpoch,
                             locked, strlen(locked));
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
    // Resolve the console-mirror rule once, from the same locked roster the
    // credentials were checked against; fail closed like the web mirror.
    const bool mirrorEligible = userMayControlOtherSessions(String(job->user));
    TransportSessionEpoch boundEpoch = 0;
    if (!bleBindSession(job->connId, job->sessionEpoch,
                        String(job->user), mirrorEligible, boundEpoch)) {
      recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), false,
                                 "Connection changed before session bind");
      bleLoginJobFree(job);
      return;
    }
    credentialAttemptRecord(SOURCE_BLUETOOTH, attemptPeer, job->user, true);
    recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), true,
                               "Login successful");
    systemEventPost(SYSEVT_LOGIN_OK, job->user, "ble");
    BLE_DEBUGF(DEBUG_BLE_CORE, "Login success conn=%u user='%s' mirror=%d",
               (unsigned)job->connId, job->user, mirrorEligible ? 1 : 0);
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
    // Only a checked wrong password is a guess; an unavailable store is not.
    if (authStoreHeld) {
      credentialAttemptRecord(SOURCE_BLUETOOTH, attemptPeer, job->user, false);
    }
    recordTransportLoginResult(SOURCE_BLUETOOTH, String(job->user), false,
                               authStoreHeld ? "Invalid credentials"
                                             : "Authentication store unavailable");
    if (authStoreHeld) systemEventPost(SYSEVT_LOGIN_FAIL, job->user, "ble");
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

EXT_RAM_BSS_ATTR static char bleMessageHistory[BLE_MSG_HISTORY_SIZE][BLE_MSG_MAX_LEN];  // PSRAM: BTC/cmd_exec task context; entries are pre-redacted previews
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
    "filedelete", "filerename", "mkdir", "rmdir", "otawrite", "imagedelete"
  };
  for (const char* v : kVerbs) {
    size_t n = strlen(v);
    if (strncasecmp(line, v, n) == 0 && (line[n] == '\0' || line[n] == ' ')) return true;
  }
  return false;
}

// Command tail — runs on cmd_exec_task (deep stack), called by the deferred plaintext
// job (blePlaintextDeferredInbound) or by the deferred secure-channel job after a DATA
// frame is decrypted. Its replies may wait for the secure-channel tx mutex, which is
// why it never runs on BTC_TASK.
// `data` is already plaintext here (no secure-channel parsing in this function).
// `arrivedEncrypted` is true only for a command decrypted from a DATA frame.
static void processBleCommandLine(uint16_t connId, TransportSessionEpoch ingressEpoch,
                                  const char* data, size_t len,
                                  bool arrivedEncrypted) {
  if (!bleSessionEpochMatches(connId, ingressEpoch)) return;
  // Build printable command string (filter non-printable bytes) - stack buffer, zero heap allocations
  char cmdBuf[512];
  if (len > sizeof(cmdBuf) - 1) {
    const char* msg = "Error: BLE command exceeds 511-byte input limit; discarded";
    sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    return;
  }
  size_t outIdx = 0;
  bool lineTooLong = false;
  
  for (size_t i = 0; i < len; i++) {
    uint8_t b = ((const uint8_t*)data)[i];
    if (b == 0) continue;  // Skip NUL bytes
    char normalized = 0;
    if (b == '\r' || b == '\n' || b == '\t') {
      normalized = ' ';
    } else if (b >= 32 && b <= 126) {  // Printable ASCII only
      normalized = (char)b;
    }
    if (normalized == 0) continue;
    if (outIdx >= sizeof(cmdBuf) - 1) {
      lineTooLong = true;
      break;
    }
    cmdBuf[outIdx++] = normalized;
  }
  cmdBuf[outIdx] = '\0';

  // A transport may be narrower than CMD_INPUT_MAX, but it must never turn an
  // over-limit command into an executable prefix. Discard the complete BLE
  // record and make the rejection visible to the exact admitted session.
  if (lineTooLong) {
    const char* msg = "Error: BLE command exceeds 511-byte input limit; discarded";
    sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    return;
  }
  
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
    // The credential check runs as its own cmd_exec job (bleLoginDeferred).
    // This job carries the PLAINTEXT password until bleLoginDeferred runs, so
    // it must live in internal DRAM, never PSRAM: flash encryption is off on
    // most boards and external PSRAM is a probeable chip. RequireInternal
    // makes that placement a hard requirement; bleLoginJobFree wipes the
    // password before free. A login written in plaintext mode reaches this
    // tail through a slot of the PSRAM-preferred inbound pool (copied there on
    // BTC_TASK, held until this cmd_exec job runs); blePlaintextDeferredInbound
    // wipes that slot when it releases it.
    BleLoginAsyncJob* job = (BleLoginAsyncJob*)ps_alloc(sizeof(BleLoginAsyncJob),
                                                       AllocPref::RequireInternal,
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
    } else if (!bleRequiresLogin(connId)) {
      const char* msg = "You are AuthBypass";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    } else {
      const char* msg = "You are (unknown)";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg));
    }
    return;
  }

  // Auth gate (always on the phone link's console row: bleRequiresLogin)
  if (bleRequiresLogin(connId)) {
    String u;
    if (!bleIsAuthed(connId, u)) {
      bleSendAuthRequired(connId, ingressEpoch);
      return;
    }
  }

  // Secure Channel gate for the file browser. File contents and paths must never
  // cross the link in cleartext, so these commands run only when this command
  // arrived as a decrypted DATA frame, regardless of the global plaintext setting
  // and of whether this connection also holds a channel: a cleartext file verb is
  // refused even on a connection with an established channel. The operator must
  // have set a `blesecret` for the app to be able to establish the channel.
  if (bleIsFileBrowserCommand(cmdStart) && !arrivedEncrypted) {
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
  reply->outputLen = 0;
  reply->sendFailed = false;
  // The command's broadcastOutput lines join this reply (bleCommandOutputLine);
  // it lives until bleCommandResultCallback has sent them and the result.
  ucmd.ctx.replyHandle = reply;

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

/*
 * Fixed slot pool for inbound writes: secure-channel frames and plaintext
 * command lines both travel to cmd_exec_task in a slot. A plaintext line can be
 * `login <user> <password>`, so a plaintext slot is wiped before every release
 * (secure-channel frames carry no cleartext command).
 *
 * The copy itself is irreducible -- the GATT callback's buffer is only valid
 * for the duration of the callback, so deferring the work requires copying the
 * frame. sizeof(BleScDeferred) is 528 B, which is already the floor: 8 B of
 * header plus a 517-byte frame (the max at MTU 517).
 *
 * What was NOT irreducible was doing that as a ps_alloc/free pair per frame,
 * ~8,700 times per 4 MB OTA image. A pool claimed by index costs one allocation
 * for the whole session, removes the allocator from the per-frame hot path
 * entirely, and cannot fragment the heap -- which is what makes this viable on
 * a board with no PSRAM, where ps_alloc falls back to internal DRAM.
 *
 * 24 slots = 12,672 B, sized to hold one full Android OTA burst
 * (OTA_BLE_BATCH_CHUNKS = 16) plus headroom, matching gCmdExecQ's depth. For
 * comparison the old scheme could have 8 frames in flight at 6,912 B each
 * (ExecReq + BleScDeferred) = 55,296 B, i.e. this is ~4.4x LESS memory at 3x
 * the depth.
 *
 * Allocated lazily on the first inbound write, so a device that never takes a
 * BLE command pays nothing -- and Bluetooth is off by default on
 * this build. Never freed: the pool is bounded and reused, and freeing it would
 * need a proof that no slot is still queued on cmd_exec_task.
 */
// 26, deliberately NOT 24. A pool slot is held from the claim on BTC_TASK until
// the deferred job finishes on cmd_exec_task, but the QUEUE slot is released at
// dequeue -- so peak pool usage is (frames queued) + (1 being processed), i.e.
// gCmdExecQ's depth of 24 plus one, plus a slot of margin. At 24 the pool, not
// the queue, would have been the binding constraint, and frames would have been
// dropped while the queue still had room. Keep this > gCmdExecQ's depth.
static constexpr size_t kBleScPoolSlots = 26;
static BleScDeferred* gBleScPool = nullptr;
static uint32_t gBleScPoolInUse = 0;  // bit i => slot i claimed (24 <= 32 bits)
static portMUX_TYPE gBleScPoolMux = portMUX_INITIALIZER_UNLOCKED;

// Claim a slot. Called only from BTC_TASK (the sole inbound producer), so the
// lazy allocation below needs no lock; only the in-use mask is shared, and it
// is released from cmd_exec_task.
static BleScDeferred* bleScPoolClaim() {
  if (!gBleScPool) {
    gBleScPool = (BleScDeferred*)ps_alloc(sizeof(BleScDeferred) * kBleScPoolSlots,
                                          AllocPref::PreferPSRAM, "ble.sc.pool");
    if (!gBleScPool) return nullptr;
    gBleScPoolInUse = 0;
  }
  int slot = -1;
  portENTER_CRITICAL(&gBleScPoolMux);
  for (size_t i = 0; i < kBleScPoolSlots; ++i) {
    if ((gBleScPoolInUse & (1u << i)) == 0) {
      gBleScPoolInUse |= (1u << i);
      slot = (int)i;
      break;
    }
  }
  portEXIT_CRITICAL(&gBleScPoolMux);
  return (slot < 0) ? nullptr : &gBleScPool[slot];
}

// Return a slot. Must be called exactly once for every successful claim, on
// whichever task finished with it.
static void bleScPoolRelease(BleScDeferred* d) {
  if (!gBleScPool || !d) return;
  const size_t slot = (size_t)(d - gBleScPool);
  if (slot >= kBleScPoolSlots) return;  // not ours; never free() a pool slot
  portENTER_CRITICAL(&gBleScPoolMux);
  gBleScPoolInUse &= ~(1u << slot);
  portEXIT_CRITICAL(&gBleScPoolMux);
}

// The client host's console session on the phone pipe's link can be renewed
// under a live link (bledisconnect, bleconsole off then on, blesecret cleared
// and set again): the new session holds no channel, while the app still holds
// its old one. Its next DATA frame finds none here; the first such frame of a
// session claims one SC_REJECT "no channel" (bleScRejectNoChannel), so the app
// runs the handshake again rather than waiting on a dead channel. Frames it
// sent before hearing that get nothing.
static bool bleConsoleClaimNoChannelReject(uint16_t connId, TransportSessionEpoch epoch,
                                           const uint8_t* frame, size_t len) {
  if (!bleConsoleClientHost() || len == 0 ||
      frame[0] != 0x10 /*DATA*/ || gSettings.bleSecureChannelSecret.length() == 0) {
    return false;
  }
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !bleConnectionEpochMatchesLocked(connId, epoch) || bleScEstablished(connId)) {
    return false;
  }
  BLEConnection& c = gBLEState->connections[findConnectionSlotByConnIdLocked(connId)];
  if (c.noChannelRejected) return false;
  c.noChannelRejected = true;
  return true;
}

// Runs on cmd_exec_task. Frees its own arg (per submitDeferredToCmdExec contract).
static void bleScDeferredInbound(void* arg) {
  BleScDeferred* d = (BleScDeferred*)arg;
  if (!bleSessionEpochMatches(d->connId, d->sessionEpoch)) {
    bleScPoolRelease(d);
    return;
  }
  char   plain[512];
  size_t pl = 0;
  // Handshake replies go to this frame's own connection and session epoch only.
  BleScResult r = bleScHandleInbound(d->connId, d->sessionEpoch, d->buf, d->len,
                                     plain, sizeof(plain), &pl);
  if (r == BLE_SC_PLAINTEXT_READY) {
    processBleCommandLine(d->connId, d->sessionEpoch, plain, pl,
                          /*arrivedEncrypted=*/true);
  } else if (r == BLE_SC_BINARY_READY) {
    // The secure-channel decoder recognized an opaque application envelope.
    // OTA performs its own live-session/superadmin check for every frame.
    if (!bleSessionEpochMatches(d->connId, d->sessionEpoch)) {
      bleScPoolRelease(d);
      return;
    }
    bleMarkActivity(d->connId);
    (void)otaBleHandleEncryptedFrame(d->connId,
                                     reinterpret_cast<const uint8_t*>(plain), pl);
  } else if (r == BLE_SC_ERROR &&
             bleConsoleClaimNoChannelReject(d->connId, d->sessionEpoch, d->buf, d->len)) {
    bleScRejectNoChannel(d->connId, d->sessionEpoch);
  }
  bleScPoolRelease(d);
}

// A slot is longer than the command tail's 511-byte input limit, so a plaintext
// write clipped to a slot is still discarded whole with the over-limit error.
static_assert(sizeof(BleScDeferred::buf) > 511,
              "a clipped plaintext write must stay over the command input limit");

// Plaintext commands and output are refused where the operator requires the
// secure channel, and always on the client host: its console on the phone
// pipe's link serves Secure Channel only (it attaches only with a blesecret).
static bool bleSecureChannelOnly() {
  return bleScRequired() || bleConsoleClientHost();
}

// Runs on cmd_exec_task: a plaintext write copied off BTC_TASK by
// processIncomingBLECommand. Wipes, then releases, its pool slot: the line may be
// `login <user> <password>` and the pool prefers PSRAM. When the secure channel
// is required the write gets a fixed refusal that carries no command output or
// user data: over the channel when this connection holds one, otherwise in
// plaintext through the directed per-session sender, because the session sender
// refuses plaintext in required mode. Otherwise the command tail runs.
static void blePlaintextDeferredInbound(void* arg) {
  BleScDeferred* d = (BleScDeferred*)arg;
  if (bleSessionEpochMatches(d->connId, d->sessionEpoch)) {
    if (bleSecureChannelOnly()) {
      static const char kMsg[] = "Secure channel required — connect with the encrypted app.";
      if (bleScEstablished(d->connId)) {
        sendBLEResponseToSession(d->connId, d->sessionEpoch, kMsg, sizeof(kMsg) - 1);
      } else {
        bleRawNotifyToSession(d->connId, d->sessionEpoch, kMsg, sizeof(kMsg) - 1,
                              /*blocking=*/true);
      }
    } else {
      processBleCommandLine(d->connId, d->sessionEpoch,
                            reinterpret_cast<const char*>(d->buf), d->len,
                            /*arrivedEncrypted=*/false);
    }
  }
  volatile uint8_t* p = d->buf;
  for (size_t i = 0; i < d->len; ++i) p[i] = 0;
  bleScPoolRelease(d);
}

#if ENABLE_BLE_PHONE_PIPE
// Console records the pipe's console driver hands over (BLE_PhonePipeConsoleCore.h).
constexpr uint8_t kBlePipeRecordCommand = 0x81, kBlePipeRecordBinary = 0x82;

// Runs on cmd_exec_task: one record the console driver admitted from the phone
// link's console stream, fenced on the epoch captured at its admission (a
// logout or revoke since then drops it silently, as on the other paths). Only
// the console row takes it: the pipe's authenticated encryption carried it,
// which is what arrivedEncrypted vouches for. processBleCommandLine copies and
// filters the line into its own buffer; OTA checks every frame itself.
void blePipeConsoleInbound(uint16_t rowConnId, TransportSessionEpoch epoch, uint8_t type,
                           const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0 || (type != kBlePipeRecordCommand && type != kBlePipeRecordBinary)) {
    return;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !bleConnectionEpochMatchesLocked(rowConnId, epoch) ||
        !blePipeRow(gBLEState->connections[findConnectionSlotByConnIdLocked(rowConnId)])) {
      return;
    }
    gBLEState->commandsReceived++;
  }
  if (type == kBlePipeRecordCommand) {
    processBleCommandLine(rowConnId, epoch, reinterpret_cast<const char*>(bytes), len,
                          /*arrivedEncrypted=*/true);
    return;
  }
  bleMarkActivity(rowConnId);
  (void)otaBleHandleEncryptedFrame(rowConnId, bytes, len);
}
#endif

// Entry from the GATT write callback — runs on BTC_TASK (8 KB, time-critical), which
// serves the GATT/GAP callbacks of every link. It only copies the write into a pool slot
// and defers it to cmd_exec: secure-channel frames to bleScDeferredInbound (where the
// handshake crypto runs), plaintext writes to blePlaintextDeferredInbound. Replies wait
// for the secure-channel tx mutex while a paced reply holds it, so they are sent from
// cmd_exec; the only notices sent from here are best-effort (blocking=false).
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {
  const TransportSessionEpoch ingressEpoch =
      bleSessionEpochForConnection(connId);
  if (ingressEpoch == 0) return;
  const uint8_t t = (len > 0) ? (uint8_t)data[0] : 0;
  const bool isFrame = (t == 0x01 /*HELLO*/ || t == 0x03 /*CONFIRM*/ || t == 0x10 /*DATA*/);
  if (isFrame) {
    if (len > sizeof(((BleScDeferred*)0)->buf)) {
      const char* msg = "Error: BLE secure frame exceeds 517-byte input limit; discarded";
      sendBLEResponseToSession(connId, ingressEpoch, msg, strlen(msg),
                               /*blocking=*/false);
      return;
    }
    BleScDeferred* d = bleScPoolClaim();
    if (d) {
      d->connId = connId;
      d->len = (uint16_t)len;
      d->sessionEpoch = bleSessionEpochForConnection(connId);
      memcpy(d->buf, data, len);
      if (d->sessionEpoch == 0) {
        bleScPoolRelease(d);
        return;
      }
      if (!submitDeferredToCmdExec(bleScDeferredInbound, d)) bleScPoolRelease(d);
    } else {
      // Pool exhausted: the peer is bursting faster than cmd_exec_task drains.
      // Same visible outcome as a full queue (the frame is dropped and the
      // sender's next checkpoint re-sends from the accepted offset), so it is
      // counted in the same place rather than failing silently.
      cmdExecDropCountIncrement();
    }
    return;
  }
  // Plaintext path: the secure-channel gate and the command tail run on cmd_exec
  // (blePlaintextDeferredInbound). A write longer than a slot is clipped to it and
  // still exceeds the tail's input limit (static_assert above). The slot may hold
  // a cleartext password, so every release of a plaintext slot wipes it first.
  BleScDeferred* d = bleScPoolClaim();
  if (d) {
    d->connId = connId;
    d->sessionEpoch = ingressEpoch;
    d->len = (uint16_t)(len < sizeof(d->buf) ? len : sizeof(d->buf));
    memcpy(d->buf, data, d->len);
    if (submitDeferredToCmdExec(blePlaintextDeferredInbound, d)) return;
    volatile uint8_t* p = d->buf;
    for (size_t i = 0; i < d->len; ++i) p[i] = 0;
    bleScPoolRelease(d);  // the queue-full drop is counted by submitDeferredToCmdExec
  } else {
    cmdExecDropCountIncrement();
  }
  // Not queued. One best-effort notice: blocking=false never waits for the
  // secure-channel tx mutex and makes one admission attempt.
  static const char kQueueFull[] = "Error: Failed to queue command";
  sendBLEResponseToSession(connId, ingressEpoch, kQueueFull, sizeof(kQueueFull) - 1,
                           /*blocking=*/false);
}

// =============================================================================
// INITIALIZATION
// =============================================================================

// The console state both console hosts allocate (initBluetooth, and
// bleConsoleRegisterOnClientHost on the G2 client host); nullptr when out of
// memory. BLESystemState owns Arduino String members. ps_alloc() supplies raw
// storage, so start their lifetimes explicitly; byte-zeroing the aggregate
// would leave invalid String representations and free() would skip their
// destructors.
static BLESystemState* bleNewState() {
  void* stateStorage = ps_alloc(sizeof(BLESystemState), AllocPref::PreferPSRAM, "ble.state");
  if (!stateStorage) return nullptr;
  BLESystemState* state = new (stateStorage) BLESystemState{};

  // Initialize connection slots
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    state->connections[i].active = false;
    state->connections[i].connId = 0;
    state->connections[i].deviceType = BLE_DEVICE_UNKNOWN;
    state->connections[i].authed = false;
    state->connections[i].user = "";
    state->connections[i].mirrorEligible = false;
    state->connections[i].lastActivityMs = 0;
  }
  state->activeConnectionCount = 0;

  // Initialize streaming state
  state->streamFlags = BLE_STREAM_NONE;
  state->sensorStreamInterval = 1000;  // Default 1 second
  state->systemStreamInterval = 5000;  // Default 5 seconds
  state->lastSensorStream = 0;
  state->lastSystemStream = 0;
  state->sensorStreamCount = 0;
  state->systemStreamCount = 0;
  state->eventCount = 0;
  return state;
}

// =============================================================================
// THE APP CONSOLE AS A PHONE PIPE STREAM (phone link v2)
// =============================================================================
// The console is stream slot 2 of the phone pipe (BLE_PhonePipeConsole.h). Its
// session is one gBLEState row per stream: made signed out when the stream is
// bound, ended when it closes, with its own id (blePipeConsoleRowId), so design
// A's slot on the same link keeps its identity. The pipe calls in only with
// its own locks released and reads the row through the leaf snapshot; this
// file calls the pipe only with the lifecycle lock released, except
// pipeConsoleRequestClose, which never blocks. A bound row is a connected app
// (connection state, counts and events), as design A's slot is once the app
// joins it: the phone asked for the console (CONSOLE_WANTED) and took the
// stream.

// The caller holds the lifecycle lock: the state goes, its sessions closed.
static void bleStateRetireLocked() {
  (void)blePipeRowEndLocked();
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) bleInvalidateSessionSlotLocked(i);
  ps_delete(gBLEState);
  gBLEState = nullptr;
  sBleStateHolders = 0;
  sBleServerStateInitialized.store(false);
}

// The caller holds the lifecycle lock: `holder` leaves, and the last one out
// retires the state.
static void bleStateReleaseLocked(uint8_t holder) {
  sBleStateHolders &= uint8_t(~holder);
  if (!sBleStateHolders && gBLEState) bleStateRetireLocked();
}

// A host role (the server, or a client host not yet torn down) holds the
// state; a state only the pipe holds is one the client host's service joins.
static bool bleStateHeldByHostRole() {
  BleLifecycleGuard lifecycle;
  return lifecycle && gBLEState && (sBleStateHolders & uint8_t(~kBleHoldPipe)) != 0;
}

static bool blePipeRowEndLocked(bool match, uint32_t generation, uint16_t streamId,
                                PipeConsoleTarget* ended) {
  const int slot = sBlePipeRowSlot;
  if (!gBLEState || slot < 0 || slot >= BLE_MAX_CONNECTIONS) return false;
  const BLEConnection& c = gBLEState->connections[slot];
  if (!c.active || !c.pipeConsole) {  // bookkeeping without its row: drop it
    sBlePipeRowSlot = -1;
    blePipeIdentitySyncLocked();
    return false;
  }
  if (match && (c.pipeGeneration != generation || c.pipeStream != streamId)) return false;
  if (ended) *ended = PipeConsoleTarget{c.pipeGeneration, c.pipeStream};
  // The snapshot drops it first, then its epoch (queued replies and login jobs
  // fail closed) and sign-in.
  bleClearConnectionByConnId(c.connId);
  if (gBLEState->activeConnectionCount > 0) gBLEState->activeConnectionCount--;
  gBLEState->deferredDisconnectActiveCount = gBLEState->activeConnectionCount;
  gBLEState->deferredDisconnectPending = true;
  if (gBLEState->activeConnectionCount == 0) gBLEState->connectionState = BLE_STATE_IDLE;
  // `outble` ends with the last signed-in session, as a disconnect's does.
  if (!bleHasAuthenticatedSession()) gOutputFlags &= ~MSG_ROUTE_BLE;
  char remaining[8];
  snprintf(remaining, sizeof(remaining), "%d", gBLEState->activeConnectionCount);
  systemEventPost(SYSEVT_BLE_DISCONNECTED, remaining);
  return true;
}

// A host teardown without the client host's service (the pipe alone on the
// client stack): the console row ends with the link (no CLOSE) and every
// session epoch closes before the host stops. The pipe keeps its hold; the
// server role's state is deinitBluetooth's.
static void blePipeHostTeardown() {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState || (sBleStateHolders & kBleHoldServer)) return;
  (void)blePipeRowEndLocked();
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) bleInvalidateSessionSlotLocked(i);
}

// Main loop: a row whose pipe session is no longer READY ends before the pipe
// task's own pass gets to it (pipeConsoleAfterUnlock). The link record is a
// leaf portMUX copy, read under the lifecycle lock as design A's reconcile
// reads it, so a row bound meanwhile for a new session is never mistaken.
static void blePipeConsoleReconcile() {
#if ENABLE_BLE_PHONE_PIPE
  BlePipeConsoleIdentity id;
  blePipeConsoleIdentity(&id);
  if (!id.bound) return;
  BleLifecycleGuard lifecycle;
  if (!lifecycle) return;
  PhonePipeLinkRecord record;
  if (phonePipeLinkRecord(&record) && record.generation == id.generation) return;
  (void)blePipeRowEndLocked(/*match=*/true, id.generation, id.streamId);
#endif
}

#if ENABLE_BLE_PHONE_PIPE
bool blePipeConsoleStateAcquire() {
  if (!bleInitLifecycleMutex()) return false;
  BleLifecycleGuard lifecycle;
  // The server role never shares its host with the pipe.
  if (!lifecycle || (sBleStateHolders & kBleHoldServer)) return false;
  if (!gBLEState) {
    gBLEState = bleNewState();  // initialized stays false: the server role is not up
    if (!gBLEState) return false;
  }
  sBleStateHolders |= kBleHoldPipe;
  return true;
}

void blePipeConsoleStateRelease() {
  BleLifecycleGuard lifecycle;
  if (!lifecycle) return;
  (void)blePipeRowEndLocked();
  bleStateReleaseLocked(kBleHoldPipe);
}

bool blePipeConsoleBind(uint32_t generation, uint16_t streamId, uint16_t linkConnId) {
  const uint16_t rowId = blePipeConsoleRowId(generation);
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState || !(sBleStateHolders & kBleHoldPipe) || streamId == 0) return false;
  const int bound = sBlePipeRowSlot;
  if (bound >= 0 && gBLEState->connections[bound].active &&
      gBLEState->connections[bound].pipeConsole &&
      gBLEState->connections[bound].pipeGeneration == generation &&
      gBLEState->connections[bound].pipeStream == streamId) {
    return true;  // already this stream's row
  }
  // One console row at a time: one left from an earlier stream ends first.
  (void)blePipeRowEndLocked();
  // The id is outside Bluedroid's range; a row already using it is refused,
  // never shared (two rows on one id would share a sign-in).
  if (findConnectionSlotByConnIdLocked(rowId) >= 0) return false;
  int slot = -1;
  for (int i = 0; i < BLE_MAX_CONNECTIONS && slot < 0; i++) {
    if (!gBLEState->connections[i].active) slot = i;
  }
  if (slot < 0) return false;
  BLEConnection& c = gBLEState->connections[slot];
  c.active = true;
  c.connId = rowId;
  c.connectedSince = millis();
  memset(c.deviceAddr, 0, sizeof(c.deviceAddr));
  c.deviceType = BLE_DEVICE_PHONE;
  c.deviceName = bleDeviceTypeToString(BLE_DEVICE_PHONE);
  c.commandsReceived = 0;
  c.authed = false;
  c.user = "";
  c.mirrorEligible = false;
  c.noChannelRejected = false;
  c.lastActivityMs = millis();
  c.pipeConsole = true;
  c.pipeGeneration = generation;
  c.pipeStream = streamId;
  c.pipeLinkConnId = linkConnId;
  c.noticePending = false;
  c.noticeEpoch = 0;
  c.noticeUser = "";
  sBlePipeRowSlot = slot;
  if (blePublishSessionSlotLocked(slot) == 0) {  // no transport session left
    bleClearConnectionByConnId(rowId);
    return false;
  }
  gBLEState->activeConnectionCount++;
  gBLEState->totalConnections++;
  gBLEState->connectionState = BLE_STATE_CONNECTED;
  gBLEState->deferredConnectSlot = slot;
  gBLEState->deferredConnectPending = true;
  systemEventPost(SYSEVT_BLE_CONNECTED, bleDeviceTypeToString(BLE_DEVICE_PHONE));
  return true;
}

void blePipeConsoleEnd(uint32_t generation, uint16_t streamId) {
  BleLifecycleGuard lifecycle;
  if (lifecycle) (void)blePipeRowEndLocked(/*match=*/true, generation, streamId);
}

void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) {
  if (!out) return;
  portENTER_CRITICAL(&sBlePipeIdentityMux);
  *out = sBlePipeIdentity;
  portEXIT_CRITICAL(&sBlePipeIdentityMux);
}

bool bleLifecycleHeldByCurrentTask() {
  // No task (before the scheduler) and an unheld lock both read as nullptr.
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  return self && sBleLifecycleMutex && xSemaphoreGetMutexHolder(sBleLifecycleMutex) == self;
}
#endif

// The command service (request write, response notify, status read), the
// same on both console hosts. pServer is registered: createService waits
// forever on an app that never registered.
static void bleBuildCommandService() {
  pCommandService = pServer->createService(BLE_COMMAND_SERVICE_UUID);

  // Request characteristic (write from client - any command)
  pCmdRequestChar = pCommandService->createCharacteristic(
    BLE_CMD_REQUEST_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  // One characteristic value is one executable or secure-channel record.
  // Refuse ATT prepared writes: the generic Arduino accumulator is shared by
  // connections and an allocation failure can otherwise commit a prefix.
  pCmdRequestChar->setPreparedWriteEnabled(false);
  // Write-only: Arduino keeps the last write as the value, and Bluedroid checks
  // the attribute's permission (not its properties) on a read, so any other
  // central or link could read back the last command, a plaintext login too.
  pCmdRequestChar->setAccessPermissions(ESP_GATT_PERM_WRITE);
  pCmdRequestChar->setCallbacks(&sCmdRequestCallbacks);

  // Response characteristic (notify to client - command results)
  pCmdResponseChar = pCommandService->createCharacteristic(
    BLE_CMD_RESPONSE_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCmdResponseChar->addDescriptor(new BLE2902(), true);  // Owned CCCD for notifications

  // Status characteristic (read - connection info)
  pCmdStatusChar = pCommandService->createCharacteristic(
    BLE_CMD_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pCmdStatusChar->setCallbacks(&sCmdStatusCallbacks);

  pCommandService->start();
}

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
  {
    const char* companionReason = nullptr;
    if (radioCompanionBlocksRadio(&companionReason)) {
      BROADCAST_PRINTF("[BLE] Server start blocked: %s", companionReason);
      return false;
    }
  }
  if (!bleInitLifecycleMutex()) {
    broadcastOutput("[BLE] Failed to create lifecycle mutex");
    return false;
  }
  if (gBLEState && gBLEState->initialized) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Already initialized");
    return true;
  }

  // The console server and the client stack share the controller and never
  // coexist. Close every device owner first (a role switch is not a user
  // disconnect: each peer keeps its reconnect intent for the next client
  // stack), then take the shared stack down; it releases the setup worker.
#if ENABLE_G2_GLASSES
  if (isG2ClientInitialized()) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Closing the glasses first");
    broadcastOutput("[BLE] Closing the glasses first");
    if (!deinitG2Client()) {
      broadcastOutput("[BLE] Cannot start server mode: G2 control owner did not stop");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
#endif
  if (g2RingIsOpen() && !g2RingClose(/*userInitiated=*/false)) {
    broadcastOutput("[BLE] Cannot start server mode: Ring owner did not stop");
    return false;
  }
#if ENABLE_BLE_PHONE_PIPE
  if (phonePipeActive() && !phoneLinkClose(/*userInitiated=*/false)) {
    broadcastOutput("[BLE] Cannot start server mode: phone pipe did not stop");
    return false;
  }
#endif
  if (!bluetoothHalStatus().hostAbsent() && !bleStackDown()) {
    BROADCAST_PRINTF("[BLE] Cannot start server mode: %s", bleStackLastRefusal());
    return false;
  }


  // Check memory before initializing BLE stack (52KB floor; BLE init measured at ~43KB + headroom)
  if (!checkMemoryAvailable("bluetooth", nullptr)) {
    if (sBLEToggleCount > 0) {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need 52KB DRAM)");
      broadcastOutput("[BLE] Free internal DRAM before restarting Bluetooth.");
    } else {
      broadcastOutput("[BLE] Insufficient memory for Bluetooth (need 52KB DRAM)");
    }
    return false;
  }
  
  // Track total free-heap change; other subsystems can allocate in this interval.
  sBLEHeapBeforeInit = ESP.getFreeHeap();
  
  BLESystemState* state = bleNewState();
  if (!state) {
    broadcastOutput("[BLE] Failed to allocate state");
    return false;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle) {
      ps_delete(state);
      return false;
    }
    // The pipe stopped above and gave its hold back; a state still left
    // (its stop failed partway) ends its row and goes with the old host.
    if (gBLEState) bleStateRetireLocked();
    gBLEState = state;
    sBleStateHolders = kBleHoldServer;
  }

  BLE_DEBUGF(DEBUG_BLE_CORE, "Initializing ESP32 BLE...");
  
  // Initialize ESP32 BLE with configured device name
  const char* deviceName = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName.c_str() : "HardwareOne";
  const bool stackReady = bluetoothHalInit(deviceName);

  if (!stackReady) {
    broadcastOutput("[BLE] Init failed (host/controller incomplete)");
    const BluetoothHalShutdownResult rollback = bluetoothHalDeinit();
    if (rollback.success) bleCentralClientsTerminalTeardownAcknowledged();
    if (!rollback.success) bleStackSetLifecycleFault(true);
    BleLifecycleGuard lifecycle;
    if (lifecycle && gBLEState) bleStateRetireLocked();
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

  // Apply the existing setting only where the controller exposes power control.
  if (bluetoothHalSupportsTxPower()) {
    const esp_err_t powerErr = bluetoothHalSetTxPower((uint8_t)constrain(gSettings.bleTxPower, 0, 7));
    if (powerErr != ESP_OK) {
      BROADCAST_PRINTF("[BLE] TX power setting failed: %s", esp_err_to_name(powerErr));
    }
  } else {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Radio backend uses its default BLE TX power");
  }
  
  // Capture this server app's GATT interface so plaintext command replies can
  // use IDF's connection-directed notify instead of Arduino's peer broadcast.
  sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  BLEDevice::setCustomGattsHandler(bleGattsEventTap);

  // Create server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(&sServerCallbacks);
  
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
  const char* boardModel = BOARD_NAME;
  pModelChar->setValue(boardModel[0] && strcmp(boardModel, "Unknown/Unsupported") != 0
                          ? boardModel : "HardwareOne");
  
  pFirmwareChar = pDeviceInfoService->createCharacteristic(
    BLE_FIRMWARE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pFirmwareChar->setValue(SelfDevice::firmwareVersion());  // real build version, not a hardcoded string
  
  pDeviceInfoService->start();
  
  // --------------------------------------------
  // Command Service (single service for all communication)
  // --------------------------------------------
  bleBuildCommandService();

  // --------------------------------------------
  // Data Streaming Service
  // --------------------------------------------
  pDataService = pServer->createService(BLE_DATA_SERVICE_UUID);
  
  // Sensor data characteristic (notify - continuous sensor updates)
  pSensorDataChar = pDataService->createCharacteristic(
    BLE_SENSOR_DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSensorDataChar->addDescriptor(new BLE2902(), true);
  
  // System status characteristic (notify - system health updates)
  pSystemStatusChar = pDataService->createCharacteristic(
    BLE_SYSTEM_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSystemStatusChar->addDescriptor(new BLE2902(), true);
  
  // Event notification characteristic (notify - important events)
  pEventNotifyChar = pDataService->createCharacteristic(
    BLE_EVENT_NOTIFY_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pEventNotifyChar->addDescriptor(new BLE2902(), true);

  // Streams are switched on and off with the authenticated `blestream`
  // command over the command service; this service only notifies.
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

  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return false;
    gBLEState->initialized = true;
    sBleServerStateInitialized.store(true);
    gBLEState->connectionState = BLE_STATE_IDLE;
    sBleConsoleMode.store(hw1console::Mode::Server);
  }
  sBleServerUp.store(true);

  // Secure Channel: create the tx mutex, then pre-derive the PSK here (init runs on a
  // large-stack task) so the per-connection handshake never pays the PBKDF2 cost.
  bleScInit();
  bleScWarmPsk();

  BLE_DEBUGF(DEBUG_BLE_CORE, "Bluetooth initialized successfully");
  broadcastOutput("[BLE] Initialized - ready to advertise");
  systemEventPost(SYSEVT_BLE_ON, deviceName);

  return true;
}

bool deinitBluetooth() {
  BleRoleTransitionClaim role(BleRoleTransition::SERVER_STOP);
  if (!role) {
    broadcastOutput("[BLE] Server stop deferred: another BLE role transition is active");
    return false;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState || !gBLEState->initialized) return true;
  }

  // Drop R1 GATT pointers before the stack dies. openg2 often deinit's
  // server mode while a ring connect left a live client; without this,
  // Health polls write through a freed characteristic → LoadProhibited.
  // This runs while app sessions and advertising are still open, so a
  // deferred stop leaves the server's sessions and advertising as they were.
  if (!g2RingPrepareForStackTeardown(2000)) {
    broadcastOutput("[BLE] Server stop deferred: Ring owner did not quiesce");
    // A companion reset (the RECOVERING claim this nested claim joined) honours
    // this result: it retries the stop a bounded number of times and never
    // forces the host down while the Ring owner is busy. If no attempt
    // completes, it leaves the host up while the companion reboots underneath
    // it and keeps Bluetooth off until a reboot (System_RadioCompanion.cpp
    // shutDownBleForCompanionReset). No link survives that reboot, so close
    // session admission and advertising now: no directed sender queues a
    // result into a link that is about to vanish, and no new peer connects to
    // a host that will be left stale. Every other caller keeps the server
    // running.
    if (bleRoleTransitionState() == BleRoleTransition::RECOVERING) {
      {
        BleLifecycleGuard lifecycle;
        if (lifecycle) {
          for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
            bleInvalidateSessionSlotLocked(i);
          }
        }
        sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
      }
      stopBLEAdvertising();
    }
    return false;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState || !gBLEState->initialized) return true;
    sBleServerUp.store(false);
    // Admission closes before the stack and characteristic objects are torn
    // down.  Any queued result carrying an old epoch now fails closed.
    for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
      bleInvalidateSessionSlotLocked(i);
    }
  }
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Deinitializing Bluetooth...");
  
  stopBLEAdvertising();

  // ESP32 BLE doesn't have a clean disconnect API like NimBLE
  // Just deinit the device
  sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  BLEDevice::setCustomGattsHandler(nullptr);
  const BluetoothHalShutdownResult deinitResult = bluetoothHalDeinit();
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
    return false;
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
  pManufacturerChar = nullptr;
  pModelChar = nullptr;
  pFirmwareChar = nullptr;
  
  // Free and clear state (the pipe stopped above: no other holder shares it)
  {
    BleLifecycleGuard lifecycle;
    if (lifecycle && gBLEState) {
      bleStateRetireLocked();
      sBleConsoleMode.store(hw1console::Mode::Off);
    }
  }

  sBLEToggleCount++;
  size_t heapAfterDeinit = ESP.getFreeHeap();
  int leaked = (int)sBLEHeapBeforeInit - (int)heapAfterDeinit;
  if (leaked > 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "[BLE] Deinitialized (heap change since start: -%dKB, %d toggle%s total)",
             leaked / 1024, sBLEToggleCount, sBLEToggleCount == 1 ? "" : "s");
    broadcastOutput(buf);
  } else {
    broadcastOutput("[BLE] Deinitialized");
  }
  systemEventPost(SYSEVT_BLE_OFF);
  return true;
}

// =============================================================================
// APP CONSOLE ON THE G2 CLIENT HOST
// =============================================================================

hw1console::Mode bleConsoleMode() { return sBleConsoleMode.load(); }

bool bleConsoleRegisterOnClientHost() {
  // No pipe, no phone link to serve: nothing to register (and the client-host
  // paths fold away, bleConsoleClientHost()).
  if (!ENABLE_BLE_PHONE_PIPE) return false;
  // Everything is checked before anything is created. The creation waits
  // below are released by BTC callbacks that take the lifecycle lock, so this
  // runs inside a role transition (never on BTC) without that lock. A link up
  // or being set up would be sent Service Changed when the service starts.
  // The server app's id comes from the reserved 15-bit allocator every client
  // uses (BLEDevice::allocateAppId): createServer registers m_appId as it
  // stands, a counter each client connect advances, and it waits forever once
  // that passes ESP_APP_ID_MAX; a client could also take the same id later.
  // No client registers on this fresh host meanwhile (no link is held), and a
  // terminal host teardown releases the reservation with the clients'.
  uint16_t appId = 0;
  const char* refused = nullptr;
  if (!bleRoleTransitionHeldByCurrentTask()) {
    refused = "outside a BLE role transition";
  } else if (sBleConsoleMode.load() != hw1console::Mode::Off || bleStateHeldByHostRole()) {
    refused = "the last host's console state is still held";
  } else if (!bleInitLifecycleMutex()) {
    refused = "no lifecycle mutex";
  } else if (xSemaphoreGetMutexHolder(sBleLifecycleMutex) == xTaskGetCurrentTaskHandle()) {
    refused = "called under the lifecycle lock";
  } else if (!bluetoothHalStatus().ready()) {
    refused = "host not ready";
  } else if (isG2ClientInitialized() || bleCentralLinkHeld(BleCentralLink::Left) ||
             bleCentralLinkHeld(BleCentralLink::Right) ||
             bleCentralLinkHeld(BleCentralLink::Ring) ||
             bleCentralLinkHeld(BleCentralLink::Phone)) {
    refused = "a link may already be up";
  } else if (!BLEDevice::allocateAppId(appId)) {
    refused = "no free GATT app id";
  }
  if (refused) {
    BROADCAST_PRINTF("[BLE] App console not registered: %s", refused);
    return false;
  }

  BLESystemState* state = bleNewState();
  if (!state) {
    broadcastOutput("[BLE] App console not registered: no memory for its state");
    return false;
  }
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle) {
      ps_delete(state);
      return false;
    }
    // The pipe may hold a state already (its console row lives there): the
    // command service joins it, and the new one goes unused.
    if (!gBLEState) {
      gBLEState = state;  // initialized stays false: the server role is not up
      state = nullptr;
    }
    sBleStateHolders |= kBleHoldClientConsole;
    bleConsoleStoreLocked(hw1console::Attach{});  // no slot yet: reconcile makes it
    sBleConsoleLogged = hw1console::Slot{};
  }
  if (state) ps_delete(state);

  sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  sBleGattsRegStatus.store(kBleGattsRegPending, std::memory_order_release);
  BLEDevice::setCustomGattsHandler(bleGattsEventTap);
  BLEDevice::m_appId = appId;  // createServer registers m_appId++
  pServer = BLEDevice::createServer();
  const uint32_t regDeadline = millis() + 200;
  while (sBleGattsRegStatus.load(std::memory_order_acquire) == kBleGattsRegPending &&
         (int32_t)(millis() - regDeadline) < 0) {
    vTaskDelay(1);
  }
  const int regStatus = sBleGattsRegStatus.load(std::memory_order_acquire);
  if (regStatus != ESP_GATT_OK ||
      sBleGattsIf.load(std::memory_order_acquire) == ESP_GATT_IF_NONE) {
    // No app registered and no service exists, so nothing calls back into
    // the state. Arduino frees its server object with the host. A state the
    // pipe also holds stays for its row.
    BLEDevice::setCustomGattsHandler(nullptr);
    {
      BleLifecycleGuard lifecycle;
      pServer = nullptr;
      bleStateReleaseLocked(kBleHoldClientConsole);
    }
    BROADCAST_PRINTF("[BLE] App console not registered: GATT registration status %d", regStatus);
    return false;
  }
  // From here the server app is on this host: the teardown hook owns it, and
  // ServerCallbacks admits none of the links Bluedroid reports to it.
  sBleConsoleMode.store(hw1console::Mode::ClientRegistered);
  pServer->setCallbacks(&sServerCallbacks);
  bleBuildCommandService();
  sBleConsoleAdmissionOpen.store(true);

  // Secure Channel is the only output this mode serves: create the tx mutex
  // and derive the PSK here, off BTC, as initBluetooth does.
  bleScInit();
  bleScWarmPsk();

  broadcastOutput("[BLE] App console registered on the client host (not advertised)");
  return true;
}

void bleConsoleHostTeardownBegin() {
  // Without the client host's service only the phone link's console row has
  // to end (and the server role is deinitBluetooth's).
  if (!bleConsoleClientHost()) {
    blePipeHostTeardown();
    return;
  }
  {
    BleLifecycleGuard lifecycle;
    if (lifecycle) {
      sBleConsoleAdmissionOpen.store(false);
      // Admission closed: the console slot's session ends, and the next host
      // starts with no slot and no closed generations.
      bleConsoleReconcileLocked();
      bleConsoleStoreLocked(hw1console::Attach{});
      // The link goes with the host: the console row ends too, with no CLOSE.
      (void)blePipeRowEndLocked();
      // Queued results and login jobs carrying an old epoch now fail closed.
      for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
        bleInvalidateSessionSlotLocked(i);
      }
      pCmdRequestChar = nullptr;
      pCmdResponseChar = nullptr;
      pCmdStatusChar = nullptr;
      pCommandService = nullptr;
      pServer = nullptr;
    }
    sBleGattsIf.store(ESP_GATT_IF_NONE, std::memory_order_release);
  }
  // A send admitted before the epochs closed may still be posting with the
  // lock released (bleRawNotifyToSession): it lands before the host stops.
  // BTC_TASK drains its queue meanwhile, since nothing holds the lock now.
  while (sBleConsoleSendsInFlight.load() > 0) vTaskDelay(1);
  BLEDevice::setCustomGattsHandler(nullptr);
}

void bleConsoleHostTeardownEnd(bool stopped) {
  // An incomplete stop keeps Arduino's callback targets, and the status read
  // callback reads gBLEState: the state stays, closed, until a stop completes.
  // A pipe still holding it (its row ended at Begin) keeps it for its next host.
  if (!stopped || !bleConsoleClientHost()) return;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle) return;
    bleStateReleaseLocked(kBleHoldClientConsole);
    sBleConsoleMode.store(hw1console::Mode::Off);
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "App console state released with the client host");
}

// =============================================================================
// ADVERTISING CONTROL
// =============================================================================

bool startBLEAdvertising() {
  // gBLEState is read under the lifecycle lock (isBLEConnected).
  bool up = false, started = false;
  {
    BleLifecycleGuard lifecycle;
    up = lifecycle && gBLEState && gBLEState->initialized;
    started = up && bleAdvertiseIfRoomLocked();
  }
  if (!up) {
    broadcastOutput("[BLE] Not initialized");
    return false;
  }
  if (!started) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "No room for another app, not advertising");
    return false;
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising started");
  broadcastOutput("[BLE] Advertising started - device visible as 'HardwareOne'");
  return true;
}

void stopBLEAdvertising() {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState || !gBLEState->initialized) return;
  if (pAdvertising && sBleAdvertising.exchange(false)) {
    BLEDevice::stopAdvertising();
    if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
      gBLEState->connectionState = BLE_STATE_IDLE;
    }
    BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising stopped");
  }
}

bool bleAdvertising() { return sBleAdvertising.load(); }
bool bleServerUp() { return sBleServerUp.load(); }

static bool bleIsPeerLinkType(BLEDeviceType type);

BleConsoleView bleConsoleView() {
  // Never blocks: read under the lifecycle lock when it is free, otherwise the
  // last such reading (a display frame must not wait on a BLE teardown).
  static portMUX_TYPE sViewMux = portMUX_INITIALIZER_UNLOCKED;
  static BleConsoleView sLast;
  BleConsoleView view;
  {
    BleLifecycleGuard lifecycle(0);
    if (lifecycle) {
      const bool server = gBLEState && gBLEState->initialized;
      // The client host's console rides the phone pipe's link: one slot at
      // most, an app only once it has joined the slot.
      const bool client = gBLEState && !server &&
                          bleConsoleClientHost();
      if (server || client) {
        view.up = server;
        view.advertising = server && sBleAdvertising.load();
        view.registered = client;
        view.attachable = client && hw1console::attachable(hw1console::Mode::ClientRegistered,
                                                           gSettings.bleConsoleOnPipe);
        view.attached = client && sBleConsoleAttach.slot.present && sBleConsoleAttach.slot.joined;
      }
      // The pipe's console stream has its own row on the client stack (the
      // pipe holds the state when the glasses are closed): it counts once bound.
      for (int i = 0; gBLEState && i < BLE_MAX_CONNECTIONS; ++i) {
        const BLEConnection& c = gBLEState->connections[i];
        if (!c.active || bleIsPeerLinkType(c.deviceType)) continue;
        if (!blePipeRow(c) && !server && !view.attached) continue;
        ++view.clients;
        if (c.authed && c.user.length()) view.signedIn = true;
      }
      view.secret = gSettings.bleSecureChannelSecret.length() > 0;
      portENTER_CRITICAL(&sViewMux);
      sLast = view;
      portEXIT_CRITICAL(&sViewMux);
      return view;
    }
  }
  portENTER_CRITICAL(&sViewMux);
  view = sLast;
  portEXIT_CRITICAL(&sViewMux);
  return view;
}

// =============================================================================
// CONNECTION MANAGEMENT
// =============================================================================

// Any task but one holding a pipe lock. gBLEState's last holder frees it on
// its own task (the pipe's on `closephone` when the glasses are closed), so it
// is read under the lifecycle lock.
bool isBLEConnected() {
  BleLifecycleGuard lifecycle;
  return lifecycle && gBLEState && gBLEState->connectionState == BLE_STATE_CONNECTED;
}

// G2 temples and the R1 ring are links this device opened as central; the GATT
// server still gives them a slot (typed by MAC in onConnect). bledisconnect
// never closes them; their owners have their own disconnect commands.
static bool bleIsPeerLinkType(BLEDeviceType type) {
  return type == BLE_DEVICE_GLASSES_LEFT || type == BLE_DEVICE_GLASSES_RIGHT ||
         type == BLE_DEVICE_RING;
}

BleDisconnectResult bleDisconnectConnection(uint16_t connId) {
  BLEServer* server = nullptr;
  PipeConsoleTarget renew;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return BleDisconnectResult::UNAVAILABLE;
    const int slot = findConnectionSlotByConnIdLocked(connId);
    // The phone link's console row, in any mode: its session ends now and its
    // stream is renewed once it drains (CLOSE 2); the pipe keeps the link.
    if (slot >= 0 && blePipeRow(gBLEState->connections[slot])) {
      (void)blePipeRowEndLocked(/*match=*/false, 0, 0, &renew);
    } else {
      if (!pServer) return BleDisconnectResult::UNAVAILABLE;
      if (slot < 0) return BleDisconnectResult::NOT_FOUND;
      if (bleIsPeerLinkType(gBLEState->connections[slot].deviceType)) {
        return BleDisconnectResult::PEER_LINK;
      }
      // The client host's console: the pipe owns the link, so only the session
      // ends (never a GATT close); the next reconcile starts a fresh one there.
      if (bleConsoleClientHost()) {
        const hw1console::Plan plan = hw1console::endSession(sBleConsoleAttach, connId);
        bleConsoleApplyLocked(plan);
        return plan.clear ? BleDisconnectResult::SESSION_ENDED : BleDisconnectResult::NOT_FOUND;
      }
      server = pServer;
    }
  }
  if (renew.streamId) {
    pipeConsoleRequestClose(renew.generation, renew.streamId, kBlePipeCloseRenewed);
    return BleDisconnectResult::SESSION_ENDED;
  }
  // esp_ble_gatts_close posts the close to the BTC task and can block while
  // that queue is full; BTC's onConnect/onDisconnect take the lifecycle guard,
  // so the close is issued with the guard released. onDisconnect clears the
  // slot when the stack confirms.
  BLE_DEBUGF(DEBUG_BLE_CORE, "Disconnecting client conn=%u", (unsigned)connId);
  server->disconnect(connId);
  return BleDisconnectResult::REQUESTED;
}

int bleDisconnectAppClients() {
  uint16_t ids[BLE_MAX_CONNECTIONS];
  int count = 0;
  int ended = 0;
  BLEServer* server = nullptr;
  PipeConsoleTarget renew;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState) return 0;
    // The phone link's console row ends at once, in any mode (bleDisconnectConnection).
    if (blePipeRowEndLocked(/*match=*/false, 0, 0, &renew)) ++ended;
    if (!pServer) {
      // No command service on this host: the console row was the only app.
    } else if (bleConsoleClientHost()) {
      // The client host's console session ends at once (bleDisconnectConnection).
      const hw1console::Plan plan =
          hw1console::endSession(sBleConsoleAttach, sBleConsoleAttach.slot.connId);
      bleConsoleApplyLocked(plan);
      if (plan.clear) ++ended;
    } else {
      for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        const BLEConnection& c = gBLEState->connections[i];
        if (!c.active || bleIsPeerLinkType(c.deviceType)) continue;
        ids[count++] = c.connId;
      }
      server = pServer;
    }
  }
  if (renew.streamId) pipeConsoleRequestClose(renew.generation, renew.streamId, kBlePipeCloseRenewed);
  for (int i = 0; i < count; i++) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Disconnecting client conn=%u", (unsigned)ids[i]);
    server->disconnect(ids[i]);
  }
  return ended + count;
}

// =============================================================================
// DATA TRANSMISSION
// =============================================================================

// Directed RAW notify to the RESPONSE characteristic (bleRawNotifyToSession, below) —
// the ONLY function that touches the radio for command-service output. Each frame goes
// to exactly one connection through esp_ble_gatts_send_indicate, never through the
// wrapper's characteristic-wide notify, and is admitted under the BLE lifecycle mutex
// only while `expectedEpoch` is still that connection's live session epoch. A frame for
// a closed, rotated or reused session is dropped, and server teardown (which invalidates
// every epoch under the same mutex before the GATT tree is deleted) cannot race a send.
// The secure-channel encryptor and its handshake replies (System_BleSecureChannel.cpp)
// emit through this; it does NOT encrypt. Everything that is *output* (results, streamed
// CLI text) goes through sendBLEResponse()/sendBLEResponseToSession() instead, which
// encrypt when a channel is up.
//
// Bounded backpressure: an admission that fails (the controller's ACL tx buffers are
// full, the common case mid-burst) is retried after yielding ~one connection interval,
// so bursts ride out transient congestion instead of losing fragments. The retry budget
// caps total wait so a stalled/dead link can never hang the calling task — under
// *sustained* congestion it gives up and returns false, and the caller reports the
// failure (never silent). vTaskDelay yields the CPU, so waiting costs latency, not
// cycles. In the common (uncongested) case the first attempt succeeds with no delay.
//
// blocking=false collapses the retry budget to a single attempt: a best-effort
// producer (the console mirror, the notification sink) must not pay up to
// 6 x 15 ms of vTaskDelay per send. It gives up on the first failure instead,
// and its caller counts the drop. Reliable senders pass blocking=true.
#if ENABLE_OLED_DISPLAY
static void bleRememberOutput(const char* data, size_t len) {
  if (!data || !len) return;
  if (bleOutputHasPrivateSTT(data, len)) {
    data = "[private STT result]";
    len = strlen(data);
  }
  char tagged[BLE_MSG_MAX_LEN];
  const size_t previewLen = len < BLE_MSG_MAX_LEN - 4 ? len : BLE_MSG_MAX_LEN - 4;
  snprintf(tagged, sizeof(tagged), "TX:%.*s", static_cast<int>(previewLen), data);
  bleAddMessageToHistory(tagged);
}
#endif

// On the client host every command-service send is one central TX gate
// submission (BLE_CentralTx.h), so the console's notifications share the
// controller with the glasses' and the ring's writes. The gate is taken before
// the lifecycle lock and only tried, never waited for (BTC sends too): a
// reliable send skips the accessories' soft priority, a best-effort one yields
// to it, and a busy gate is a failed attempt the sender's retry covers. A sent
// reliable frame (a command's output) counts as pipe activity; best-effort
// frames (the mirror, notification cards) do not, as on the v2 stream, so a
// chatty mirror cannot hold the link's active parameters. Server mode is not
// gated.
class BleConsoleTxGate {
 public:
  explicit BleConsoleTxGate(bool reliable)
      : gated_(bleConsoleClientHost()),
        reliable_(reliable),
        held_(!gated_ || bleCentralTxTryPhone(/*bypassPriority=*/reliable)) {}
  ~BleConsoleTxGate() {
    if (gated_ && held_) bleCentralTxGive();
  }
  explicit operator bool() const { return held_; }
  bool gated() const { return gated_; }  // the client host's send
  void sent() const {
    if (gated_ && reliable_) phonePipeNoteConsoleActivity();
  }

 private:
  bool gated_;
  bool reliable_;
  bool held_;
};

bool bleRawNotifyToSession(uint16_t connId,
                           TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len,
                           bool blocking) {
  if (!data || len == 0 || expectedEpoch == 0) return false;

  const int kMaxTries = blocking ? 6 : 1;
  const TickType_t kBackoff = pdMS_TO_TICKS(15);
  esp_err_t lastError = ESP_FAIL;
  int tries = 0;

  for (int attempt = 0; attempt < kMaxTries; ++attempt) {
    tries = attempt + 1;
    {
      // Released with the lifecycle lock before any backoff.
      BleConsoleTxGate gate(blocking);
      if (!gate) {
        lastError = ESP_FAIL;  // the gate is busy: retried below like congestion
      } else {
        // The client host posts after releasing the lifecycle mutex:
        // esp_ble_gatts_send_indicate waits forever for room on BTC_TASK's
        // queue, which there carries every link, and BTC_TASK takes this mutex
        // for the phone's writes and link events, so a post holding it could
        // stop every link. The epoch is still checked under the mutex; a frame
        // that lands on a reused id after it is ciphertext (Secure Channel
        // only), as one queued behind that id's link events could be anyway.
        // The host teardown waits for the post (sBleConsoleSendsInFlight).
        const bool unlocked = gate.gated();
        esp_gatt_if_t gattsIf = 0;
        uint16_t handle = 0;
        {
          BleLifecycleGuard lifecycle;
          if (!lifecycle ||
              !bleConnectionEpochMatchesLocked(connId, expectedEpoch) ||
              !pCmdResponseChar) {
            return false;
          }

          const int liveIf = sBleGattsIf.load(std::memory_order_acquire);
          if (liveIf == ESP_GATT_IF_NONE) return false;
          gattsIf = static_cast<esp_gatt_if_t>(liveIf);
          handle = pCmdResponseChar->getHandle();

          if (unlocked) {
            sBleConsoleSendsInFlight.fetch_add(1);
          } else {
            // Unlike BLECharacteristic::notify(), this queues only to connId.
            // Server mode keeps the lifecycle mutex through the IDF admission
            // call so disconnect/slot reuse is ordered either before this send
            // (drop) or after it (old peer).
            lastError = esp_ble_gatts_send_indicate(
                gattsIf, connId, handle, len,
                reinterpret_cast<uint8_t*>(const_cast<char*>(data)), false);
            if (lastError == ESP_OK && gBLEState) gBLEState->responsesSent++;
          }
        }
        if (unlocked) {
          lastError = esp_ble_gatts_send_indicate(
              gattsIf, connId, handle, len,
              reinterpret_cast<uint8_t*>(const_cast<char*>(data)), false);
          if (lastError == ESP_OK) {
            BleLifecycleGuard lifecycle;
            if (lifecycle && gBLEState) gBLEState->responsesSent++;
          }
          sBleConsoleSendsInFlight.fetch_sub(1);
        }
        if (lastError == ESP_OK) gate.sent();
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
  if (sent) bleRememberOutput(data, len);
  #endif
  return sent;
}

// One session as a send target: a connection and the session epoch captured
// for it (the console-mirror collector's recipient record).
struct BleSessionFence { uint16_t connId; TransportSessionEpoch epoch; };

extern TaskHandle_t gCmdExecTaskHandle;  // HardwareOne.cpp
// cmd_exec is the only task a console chain may wait on (BLE_PhonePipeConsole.h).
static bool bleOnCmdExecTask() {
  return gCmdExecTaskHandle && xTaskGetCurrentTaskHandle() == gCmdExecTaskHandle;
}

// A MIRROR record carries at most 512 B (BLE_PhonePipeConsoleCore.h): a longer
// mirror buffer or `blesend` goes as several, in order.
constexpr size_t kBlePipeMirrorMax = 512;

// Console records for the phone link's console row, written with the
// lifecycle lock released (it is never held with the pipe's locks). TEXT and
// BLOB from cmd_exec wait for room within the driver's stall bounds; anything
// else is offered once (TryNow) and dropped, counted by the driver, when the
// stream has no room. A mirror whose first record went out counts as
// delivered: sendBLEResponse never resends to a session that has part of it.
static bool blePipeWrite(const PipeConsoleTarget& target, TransportSessionEpoch epoch,
                         PipeConsoleKind kind, const char* data, size_t len, bool blocking) {
  const bool chain = kind == PipeConsoleKind::Text || kind == PipeConsoleKind::Blob;
  const PipeConsoleMode mode = chain && blocking && bleOnCmdExecTask() ? PipeConsoleMode::Reliable
                                                                       : PipeConsoleMode::TryNow;
  const size_t piece = kind == PipeConsoleKind::Mirror ? kBlePipeMirrorMax : len;
  size_t sent = 0;
  while (sent < len) {
    const size_t n = len - sent < piece ? len - sent : piece;
    if (pipeConsoleWrite(target, epoch, kind, reinterpret_cast<const uint8_t*>(data) + sent, n,
                         mode) != PipeConsoleWrite::Sent) {
      break;
    }
    sent += n;
  }
  return sent > 0;
}

// One session's output. The phone link's console row takes it as console
// records of `kind` on its stream (TEXT for results and notices, BLOB, CARD,
// MIRROR); every other row as before, kind aside: its own Secure Channel, or
// directed plaintext where that is allowed. A BLOB is binary for a confidential
// session only, never plaintext. The console row comes first: design A's slot
// may sit on the same link, and that slot's channel is not this session's.
static bool bleSendToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                             PipeConsoleKind kind, const char* data, size_t len, bool blocking) {
  if (!data || len == 0) return false;

  BleScBinding binding;
  bool secure = false;
  bool pipe = false;
  PipeConsoleTarget target;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle ||
        !bleConnectionEpochMatchesLocked(connId, expectedEpoch)) {
      return false;
    }
    const BLEConnection& c = gBLEState->connections[findConnectionSlotByConnIdLocked(connId)];
    pipe = blePipeRow(c);
    if (pipe) {
      target = PipeConsoleTarget{c.pipeGeneration, c.pipeStream};
    } else {
      // Resolve the secure-channel binding while the slot cannot be reused, then
      // let the mutex go: the paced sender can run for seconds (up to 255
      // fragments), and BTC_TASK takes this mutex for every inbound write, so
      // holding it across the transfer starves the BT host task and can deadlock
      // it against our own notify queue. The binding carries the slot generation
      // and this session's epoch: the epoch is re-checked before every fragment
      // and again by the directed raw sender under the lifecycle mutex, so a
      // replacement peer or a logged-out session never receives this result, and
      // each fragment reaches only this connection.
      secure = bleScEstablished(connId);
      if (secure && !bleScBind(connId, expectedEpoch, &binding)) return false;
      if (!secure && kind == PipeConsoleKind::Blob) return false;
      if (!secure && bleSecureChannelOnly()) return false;
    }
  }

  // Records re-check the epoch against the bound row (the driver's leaf read).
  if (pipe) return blePipeWrite(target, expectedEpoch, kind, data, len, blocking);
  if (secure) {
    return bleScSendEncryptedBound(binding, data, len, blocking,
                                   /*binaryFrame=*/kind == PipeConsoleKind::Blob);
  }

  // Plaintext command replies are never characteristic-wide.  The session
  // epoch is checked in the same lifecycle critical section as the directed
  // GATT admission, closing both connId reuse and check-then-send races.
  return bleRawNotifyToSession(connId, expectedEpoch, data, len, blocking);
}

bool sendBLEResponseToSession(uint16_t connId,
                              TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking) {
  return bleSendToSession(connId, expectedEpoch, PipeConsoleKind::Text, data, len, blocking);
}

bool bleSendBinaryToSession(uint16_t connId, TransportSessionEpoch epoch,
                            const uint8_t* data, size_t len) {
  return bleSendToSession(connId, epoch, PipeConsoleKind::Blob,
                          reinterpret_cast<const char*>(data), len, /*blocking=*/true);
}

bool bleSessionConfidential(uint16_t connId, TransportSessionEpoch epoch) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !bleConnectionEpochMatchesLocked(connId, epoch)) return false;
  return blePipeRow(gBLEState->connections[findConnectionSlotByConnIdLocked(connId)]) ||
         bleScEstablished(connId);
}

// Console-mirror recipients: logged-in sessions whose account was verified
// non-guest at login.  Resolved in one lifecycle critical section so every
// (connId, epoch) pair is consistent; the sends run with the mutex released,
// like every other paced sender in this file.
static int bleCollectMirrorSessions(BleSessionFence* out, int cap) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState) return 0;
  int count = 0;
  for (int i = 0; i < BLE_MAX_CONNECTIONS && count < cap; ++i) {
    const BLEConnection& c = gBLEState->connections[i];
    if (!c.active || !c.authed || !c.mirrorEligible || c.user.length() == 0) continue;
    const TransportSessionEpoch epoch =
        sBleSessionEpoch[i].load(std::memory_order_acquire);
    if (epoch == 0) continue;
    out[count++] = BleSessionFence{c.connId, epoch};
  }
  return count;
}

// The debug console mirror's "has a recipient" predicate and the `outble`
// status count: the same collector sendBLEResponse uses, so the drain buffers
// exactly when a flush could reach somebody (with only guest sessions logged
// in it used to fill and drop its buffer for nothing).
int bleMirrorRecipientCount() {
  BleSessionFence sessions[BLE_MAX_CONNECTIONS];
  return bleCollectMirrorSessions(sessions, BLE_MAX_CONNECTIONS);
}

// Output chokepoint for broadcast/streamed device output (the debug-drain console
// mirror in System_Debug.cpp, and `blesend`).  Each logged-in non-guest session
// gets its own directed notify, or its own encrypted frames when it holds a
// Secure Channel; a peer that never logged in, a peer holding only a channel and
// a guest session receive nothing, and the characteristic-wide notify is never
// used.  sendBLEResponseToSession refuses plaintext for a session without a
// channel when the channel is required.  Returns true when at least one session
// accepted the buffer, false only when nobody received it, so a caller that
// retries on false never resends to a session that already has the data.
bool sendBLEResponse(const char* data, size_t len, bool blocking) {
  if (!data || len == 0) return false;
  BleSessionFence sessions[BLE_MAX_CONNECTIONS];
  const int count = bleCollectMirrorSessions(sessions, BLE_MAX_CONNECTIONS);
  bool delivered = false;
  for (int i = 0; i < count; ++i) {
    // The per-session fence drops a session that logged out or was replaced
    // between the collection above and this send.  Offered once per call: a
    // busy link misses this chunk (traced per session) and is never resent.
    // The phone link's console row takes it as MIRROR records (TryNow).
    if (bleSendToSession(sessions[i].connId, sessions[i].epoch, PipeConsoleKind::Mirror,
                         data, len, blocking)) {
      delivered = true;
    }
  }
  return delivered;
}

// =============================================================================
// A COMMAND'S OWN OUTPUT, FOR THE SESSION THAT SENT IT
// =============================================================================
// Many commands print their output with broadcastOutput and return only "OK"
// (`help`, listings). Serial prints those lines and the web terminal folds them
// into its reply, but a Bluetooth session received them only through the
// console mirror, which is off until `outble 1`: `help` answered an app, in
// server mode and on the phone link alike, with a bare "OK".
//
// broadcastOutputCore passes every line routed to Bluetooth here. While
// cmd_exec runs a command from a Bluetooth session (ORIGIN_BLUETOOTH, whose
// replyHandle is its BleCommandReplyContext) the line joins that command's
// batch. A full batch is sent at once and the rest just before the result,
// through sendBLEResponseToSession like the result itself: reliable, on the
// session's own channel and fenced on its epoch. A session the mirror already
// streams to gets the lines from the mirror only, never twice. Only cmd_exec
// holds a Bluetooth command context, so none of this runs on BTC_TASK.

// The debug drain's mirror condition (System_Debug.cpp), for one session:
// `outble` is on and the session is one of sendBLEResponse's recipients.
static bool bleMirrorReachesSession(TransportSessionEpoch epoch) {
  if (!(gOutputFlags & MSG_ROUTE_BLE) || !isBLEConnected()) return false;
  BleSessionFence sessions[BLE_MAX_CONNECTIONS];
  const int count = bleCollectMirrorSessions(sessions, BLE_MAX_CONNECTIONS);
  for (int i = 0; i < count; ++i) {
    if (sessions[i].epoch == epoch) return true;
  }
  return false;
}

// cmd_exec: sends the command's batched lines to its session and empties it.
// After a failed send to the console row the command's later batches are
// dropped (sendFailed).
static void bleCommandOutputFlush(BleCommandReplyContext* reply) {
  const size_t len = reply->outputLen;
  reply->outputLen = 0;
  if (len == 0 || reply->sendFailed || bleMirrorReachesSession(reply->sessionEpoch)) return;
  bleTxTrace("output", reply->connId, reply->output, len);
  if (!sendBLEResponseToSession(reply->connId, reply->sessionEpoch, reply->output, len) &&
      bleIsPipeRowId(reply->connId)) {
    reply->sendFailed = true;
  }
}

void bleCommandOutputLine(const char* text, size_t len) {
  const auto* ctx = static_cast<const CommandContext*>(currentCommandContext());
  if (!text || !ctx || ctx->origin != ORIGIN_BLUETOOTH || !ctx->replyHandle) return;
  auto* reply = static_cast<BleCommandReplyContext*>(ctx->replyHandle);
  constexpr size_t kMax = BleCommandReplyContext::kOutputMax;
  // An over-long line keeps its start, marked [CUT] like the debug queue's.
  const bool cut = len > kMax - 1;
  if (cut) len = kMax - 1;
  if (reply->outputLen + len + 1 > kMax) {
    // A send needs the lifecycle lock free (sendBLEResponseToSession), so a
    // line printed under it (BLE_DEBUGF in a locked helper) that would need
    // one is left to the other sinks. So is one printed while this task holds
    // the console chain or pipe owner mutex (an owner command's output): the
    // console row's send would wait on a lock this task holds.
    if ((sBleLifecycleMutex &&
         xSemaphoreGetMutexHolder(sBleLifecycleMutex) == xTaskGetCurrentTaskHandle()) ||
        pipeConsoleHeldByCurrentTask() || phonePipeOwnerHeldByCurrentTask()) {
      return;
    }
    bleCommandOutputFlush(reply);
  }
  memcpy(reply->output + reply->outputLen, text, len);
  if (cut) memcpy(reply->output + reply->outputLen + len - 5, "[CUT]", 5);
  reply->outputLen += len;
  reply->output[reply->outputLen++] = '\n';
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

  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    uint16_t connId = 0;
    uint32_t idleMs = 0;
    String expiredUser;
    TransportSessionEpoch logoutEpoch = 0;
    bool pipe = false;
    bool retry = false;
    {
      BleLifecycleGuard lifecycle;
      if (!lifecycle || !gBLEState) return;
      // Read the clock under the lock every lastActivityMs writer holds. A
      // clock read before it (while cmd_exec_task stamped a command, or while
      // an earlier slot's notice was sent) is older than that stamp, so the
      // idle time underflowed and a session that had just run a command was
      // logged out.
      const uint32_t now = millis();
      BLEConnection& conn = gBLEState->connections[i];
      // A console row's notice that found its stream full is offered again
      // while that signed-out session lasts; a login, logout or end drops it.
      if (conn.active && conn.noticePending &&
          sBleSessionEpoch[i].load(std::memory_order_acquire) == conn.noticeEpoch) {
        retry = pipe = true;
        connId = conn.connId;
        logoutEpoch = conn.noticeEpoch;
        expiredUser = conn.noticeUser;
      } else {
        if (conn.noticePending) {
          conn.noticePending = false;
          conn.noticeUser = "";
        }
        if (!conn.active || !conn.authed || conn.lastActivityMs == 0 ||
            !sessionIdleExpired(SOURCE_BLUETOOTH, conn.lastActivityMs, now)) {
          continue;
        }
        // A bulk OTA upload in flight IS liveness, so it outranks the idle
        // heuristic rather than depending on it. Observed 2026-08-30: a staging
        // transfer was logged out mid-flight while its checkpoints were still
        // completing every 1.4 s. Its frames did stamp activity; this loop read
        // the clock before the lock (fixed above), so a frame stamped meanwhile
        // looked idle for ~49 days. A multi-minute authenticated transfer still
        // must not race a logout clock. Bounded: housekeepBleUpload() drops the
        // upload after 60 s without frames, so this cannot pin a dead session.
        if (otaBleUploadActiveOnConnection(conn.connId)) continue;
        connId = conn.connId;
        idleMs = now - conn.lastActivityMs;
        expiredUser = conn.user;
        pipe = blePipeRow(conn);
        bleInvalidateSessionSlotLocked(i);
        conn.authed = false;
        conn.user = "";
        logoutEpoch = blePublishSessionSlotLocked(i);
        if (!bleHasAuthenticatedSession()) gOutputFlags &= ~MSG_ROUTE_BLE;
      }
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "[ble] Session expired for user '%s'",
             expiredUser.c_str());
    if (!pipe) {
      sendBLEResponseToSession(connId, logoutEpoch, msg, strlen(msg));
    } else {
      // The console row's notice is one TEXT record offered now (this is the
      // main loop, which never waits for a stream); the app reads it as the
      // session's end, so a full stream keeps it for the next tick.
      const bool sent = sendBLEResponseToSession(connId, logoutEpoch, msg, strlen(msg),
                                                 /*blocking=*/false);
      BleLifecycleGuard lifecycle;
      if (lifecycle && bleConnectionEpochMatchesLocked(connId, logoutEpoch)) {
        BLEConnection& conn = gBLEState->connections[findConnectionSlotByConnIdLocked(connId)];
        conn.noticePending = !sent;
        conn.noticeEpoch = sent ? 0 : logoutEpoch;
        conn.noticeUser = sent ? String() : expiredUser;
      }
    }
    if (retry) continue;
    BLE_DEBUGF(DEBUG_BLE_CORE,
               "Session expired conn=%u user='%s' idle_ms=%lu",
               connId, expiredUser.c_str(), (unsigned long)idleMs);
  }
}

// =============================================================================
// STATUS
// =============================================================================

void bleRecoverStuckPhone() {
#if ENABLE_BLE_PHONE_PIPE
  static bool attempted=false;
  static uint32_t lastRestart=0;
  if(!phonePipeRecoveryNeeded()){attempted=false;return;}
  if(attempted || isBleServerInitialized())return;
  const uint32_t now=millis();
  if(lastRestart && uint32_t(now-lastRestart)<600000)return;
  BleRoleTransitionClaim role(BleRoleTransition::RECOVERING);
  if(!role)return;
  const bool hadGlasses=isG2ClientInitialized();
  const bool hadRing=g2RingIsOpen();
  const auto wantedMode=gSettings.bleMode;
  attempted=true;
  // Quiescence acknowledges only that the worker stopped issuing SDK calls.
  // Callback storage and capacity remain owned until checked HAL teardown.
  if(!phonePipeQuiesceForHostRecovery()) {
    if(!phonePipeRecoveryNeeded()){attempted=false;return;}
    bleStackSetLifecycleFault(true);
    logSystemEvent("BLE", "Stuck phone cleanup: phone worker did not quiesce; host restart refused");
    return;
  }
  // Quiescing ends the phone's setup turn. Glasses and ring setups are ended
  // by deinitG2Client below, which cancels them before it waits.
  if(!bleSetupWaitIdle(hw1setup::clientMask(hw1setup::Client::Phone),2000)) {
    bleStackSetLifecycleFault(true);
    logSystemEvent("BLE", "Stuck phone cleanup: phone setup turn did not end; host restart refused");
    return;
  }
  // The device owners close for the recycle (their reconnect intent is kept);
  // the phone is already quiesced (active=false), which is what bleStackDown
  // requires of it.
  if((hadGlasses && !deinitG2Client()) || (hadRing && !g2RingClose(/*userInitiated=*/false))) {
    bleStackSetLifecycleFault(true);
    logSystemEvent("BLE", "Stuck phone cleanup: owner quiescence failed; host restart refused");
    return;
  }
  lastRestart=now?now:1;
  logSystemEvent("BLE", "Stuck phone cleanup exceeded 4 s; Bluetooth host restart is the last resort");
  systemEventPost(SYSEVT_BLE_STACK_RECYCLED,"phone-cleanup","bounded retirement failed");
  // The verified terminal teardown: ring owner quiesce, the checked HAL
  // teardown, the retired-client acknowledgement, the phone pipe's
  // host-stopped handshake, the setup worker release. It sets the lifecycle
  // fault itself on failure.
  if(!bleStackDown()) {
    bleStackSetLifecycleFault(true);
    logSystemEvent("BLE", "Stuck phone host restart failed terminal teardown; Bluetooth remains blocked");
    return;
  }
  if(!gSettings.bleEnabled || gSettings.bleMode!=wantedMode) {
    logSystemEvent("BLE", "Stuck phone host retired; changed user intent prevents role restart");
    return;
  }
  const bool restored=bleStackUp() && (!hadGlasses || initG2Client());
  if(!restored || !bluetoothHalStatus().ready()) {
    bleStackSetLifecycleFault(true);
    logSystemEvent("BLE", "Stuck phone host restart failed to restore the central role");
    return;
  }
  if(hadGlasses)blePeerRequestReseek(BLE_PEER_G2_GLASSES);
  if(hadRing)blePeerRequestReseek(BLE_PEER_R1_RING);
  blePeerRequestReseek(BLE_PEER_PHONE);
  logSystemEvent("BLE", "Stuck phone Bluetooth host restart complete; central reconnect requested");
#endif
}

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
  return bluetoothHalStatus().controller == BluetoothHalControllerState::Enabled;
}

bool isBluedroidHostEnabled() {
  return bluetoothHalStatus().hostEnabled;
}

// Any task, never blocks (display frames ask): gBLEState is read under the
// lifecycle lock when that is free, otherwise its mirror. Its last holder
// frees it on its own task (the pipe's on `closephone`).
bool isBleServerInitialized() {
  BleLifecycleGuard lifecycle(0);
  if (!lifecycle) return sBleServerStateInitialized.load();
  return gBLEState && gBLEState->initialized;
}

// The caller holds the lifecycle lock (one snapshot reads it with the rest).
static const char* bleStateTextLocked() {
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

const char* getBLEStateString() {
  BleLifecycleGuard lifecycle;  // gBLEState is read under it (isBLEConnected)
  return lifecycle ? bleStateTextLocked() : "uninitialized";
}

// =============================================================================
// COMMAND HANDLERS
// =============================================================================

// `openconsole`: the app-console server, a direct connection for the app. It
// is the one role that cannot share the controller with the client stack, so
// the switch closes every device owner (their reconnect intent is kept: a role
// switch is not a user disconnect), takes the client stack down and brings the
// server host up. `closeconsole` returns the saved role to client.
static const char* cmd_openconsole(const String& argsInput) {
  if (!gSettings.bleEnabled) {
    return "ERROR: Bluetooth is disabled - run 'bleenabled 1' first";
  }
  // Record server as the desired boot role before initBluetooth(); setSetting
  // persists it. A start that fails puts the saved role back below: the switch
  // did not happen.
  const int savedMode = gSettings.bleMode;
  setSetting(gSettings.bleMode, (int)BLE_MODE_SERVER);
  // Each peer keeps its reconnect intent: the reconnect tick holds it as
  // ROLE_BLOCKED while the server owns the radio (the tick never starts a
  // client stack, so this cannot flip the role back), and the peer reconnects
  // once the client stack is back. Peers that are up now come back even with
  // auto-reconnect off, as after a stack recycle; an explicit close of one
  // stays honoured.
  const bool hadGlasses = isG2Connected();
  const bool hadRing = g2RingIsConnected();
  const bool hadPhone = phonePipeReady();
  // Pause sensor polling during BLE init to avoid interrupt contention (RAII —
  // resumes on every return path; the trailing string checks do no I2C work).
  PollPauseGuard pollGuard;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete

  bool initOk = initBluetooth();   // closes the devices and the client stack first
  bool advOk = initOk ? startBLEAdvertising() : false;
  // A failed start leaves the radio as it was: saved role back, and the client
  // stack back for the devices initBluetooth closed before its host start
  // failed (a refusal before any teardown left the stack up; bleStackUp is then
  // a no-op). The reseeks below bring those devices back on it.
  bool stackBack = true;
  if (!initOk) {
    setSetting(gSettings.bleMode, savedMode);
    stackBack = isBleServerInitialized() || bleStackUp();
  }
  if (hadGlasses) blePeerRequestReseek(BLE_PEER_G2_GLASSES);
  if (hadRing) blePeerRequestReseek(BLE_PEER_R1_RING);
  if (hadPhone) blePeerRequestReseek(BLE_PEER_PHONE);

  if (!initOk) {
    return stackBack ? "Error: Failed to start the app console server (the client stack is up; closed devices reconnect)"
                     : "Error: Failed to start the app console server; Bluetooth is down ('openble' brings the client stack back)";
  }
  if (!advOk) {
    return "Error: Failed to start advertising";
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "App console server started and advertising");
  return "App console server started and advertising (glasses, ring and phone closed until 'openble')";
}

static const char* cmd_closeconsole(const String& argsInput) {
  if (!isBleServerInitialized()) {
    return "App console server not running";
  }
  // deinitBluetooth() has broadcast why a stop did not finish; the stored
  // role stays as it is while the server is still up.
  if (!deinitBluetooth()) return "Error: [BLE] App console server did not stop; mode unchanged";
  // Return to the role this build can use by default. Client builds select
  // client so the devices can auto-reconnect; server-only builds must not
  // persist the unavailable client role (which also suppresses the server
  // security notice).
  setSetting(gSettings.bleMode, kBleModeDefaultForBuild);
  BLE_DEBUGF(DEBUG_BLE_CORE, "App console server stopped");
  return "App console server stopped (Bluetooth host off; 'openble' brings the client stack back)";
}

// `openble` / `closeble`: the whole Bluetooth runtime, as openwifi/closewifi
// are for Wi-Fi. `openble` brings the shared client stack up; the peers with
// auto-reconnect on open themselves, as at boot. `closeble` closes every
// device (their reconnect intent is kept for the next `openble`), the console
// server, and stops the stack: the host, its controller link and the setup
// worker go, and the device workers went with their devices. Runtime only;
// `bleenabled 0` is `closeble` plus the persisted master switch.
static const char* cmd_openble(const String& /*argsInput*/) {
  if (!gSettings.bleEnabled) {
    return "ERROR: Bluetooth is disabled - run 'bleenabled 1' first";
  }
  if (isBleServerInitialized()) {
    return "Error: the app console server owns the Bluetooth host; run 'closeconsole' first";
  }
  PollPauseGuard pollGuard;
  vTaskDelay(pdMS_TO_TICKS(50));
  if (!bleStackUp()) {
    if (!ensureDebugBuffer()) return "Error: Bluetooth could not start";
    snprintf(getDebugBuffer(), 1024, "Error: Bluetooth could not start: %s", bleStackLastRefusal());
    return getDebugBuffer();
  }
  return "Bluetooth on (client stack up; devices with auto-reconnect are reconnecting; "
         "openg2 / openring / openphone open one by hand)";
}

static const char* cmd_closeble(const String& /*argsInput*/) {
  // One claim across the whole close: the reconnect tick's admissions see a
  // transition in progress and return ROLE_BLOCKED, so a device closed here
  // cannot be reopened before the stack goes down.
  BleRoleTransitionClaim role(BleRoleTransition::STACK_DOWN);
  if (!role) return "Error: BLE role transition busy";
#if ENABLE_G2_GLASSES
  if (isG2ClientInitialized()) {
    g2Disconnect(/*userInitiated=*/false);
    if (!deinitG2Client()) return "Error: the glasses runtime did not stop; retry closeble";
  }
#endif
  if (g2RingIsOpen() && !g2RingClose(/*userInitiated=*/false)) {
    return "Error: the ring owner did not stop; retry closeble";
  }
#if ENABLE_BLE_PHONE_PIPE
  if (phonePipeActive() && !phoneLinkClose(/*userInitiated=*/false)) {
    return "Error: the phone pipe did not stop; retry closeble";
  }
#endif
  if (isBleServerInitialized() && !deinitBluetooth()) {
    return "Error: the app console server did not stop; retry closeble";
  }
  if (!bleStackDown()) {
    if (!ensureDebugBuffer()) return "Error: Bluetooth did not stop";
    snprintf(getDebugBuffer(), 1024, "Error: Bluetooth did not stop: %s%s", bleStackLastRefusal(),
             bleStackLifecycleFaulted() ? " (reboot required)" : "");
    return getDebugBuffer();
  }
  BLE_DEBUGF(DEBUG_BLE_CORE, "Bluetooth off");
  return "Bluetooth off (devices closed, host stopped, workers released)";
}

// One guarded copy of the live BLE status. buildBLEInfoJson (bleinfo json and
// the web status) and blestatus/bleread (text and json) all render from it, so
// every view reports the same client set with the same identity facts.
struct BleStatusSnapshot {
  struct Client {
    int slot = 0;
    uint16_t connId = 0;
    bool authed = false;
    String user;            // raw: "" until the connection logs in
    char mac[18] = {};
    uint32_t sinceSec = 0;
    uint32_t commands = 0;
  };
  bool initialized = false;
  const char* state = "uninitialized";
  int activeConnections = 0;
  uint32_t totalConnections = 0;   // lifetime
  uint32_t commandsReceived = 0;   // lifetime
  uint32_t responsesSent = 0;      // lifetime
  int clientCount = 0;
  Client clients[BLE_MAX_CONNECTIONS];
};

// Copies the live status under the lifecycle guard (bounded 100 ms wait).
// Before the first server start there is no mutex and no published state, so
// the snapshot stays "uninitialized" without dereferencing gBLEState. Once
// created, the mutex survives server stops and protects this copy against the
// BTC_TASK callbacks and state deletion. Returns false only when the mutex
// exists and stays busy past the wait.
static bool bleCaptureStatusSnapshot(BleStatusSnapshot& out) {
  BleLifecycleGuard lifecycle(pdMS_TO_TICKS(100));
  if (sBleLifecycleMutex && !lifecycle) return false;
  if (!lifecycle || !gBLEState || !gBLEState->initialized) return true;
  out.initialized = true;
  out.state = bleStateTextLocked();
  out.activeConnections = gBLEState->activeConnectionCount;
  out.totalConnections = gBLEState->totalConnections;
  out.commandsReceived = gBLEState->commandsReceived;
  out.responsesSent = gBLEState->responsesSent;
  const uint32_t now = millis();
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    const BLEConnection& c = gBLEState->connections[i];
    if (!c.active) continue;
    BleStatusSnapshot::Client& o = out.clients[out.clientCount++];
    o.slot = i;
    o.connId = c.connId;
    o.authed = c.authed;
    o.user = c.user;
    macToStackBuf(c.deviceAddr, o.mac);
    o.sinceSec = (uint32_t)((now - c.connectedSince) / 1000);
    o.commands = c.commandsReceived;
  }
  return true;
}

// blestatus outside the app console server: how to reach the console. With the
// pipe running it is the pipe's console stream (phone link v2), which a device
// code offers; beside the glasses design A's command service also rides the
// link. The console's own state says what opens it (none once open); with
// only the glasses, the hint is what `openconsole` (server mode) closes.
static const char* bleConsoleSwitchHint(const BleLinkStatus& s) {
  using hw1link::Console;
  using hw1link::Role;
  switch (s.role) {
    case Role::Console:
      return nullptr;
    case Role::Off:
      return "to start the app console, run 'openconsole'";
    case Role::Glasses:
    case Role::GlassesPipe:
    case Role::PhonePipe:
      break;
  }
  const bool pipe = s.role != Role::Glasses;
  switch (s.console) {
    case Console::Connected:
    case Console::SignedIn:
      return nullptr;
    case Console::WaitingPhone:
      if (pipe && s.phone.consoleOffered) {  // the stream: the app opens it on a READY link
        return s.pipe.phase == hw1link::Pipe::Ready
            ? "the phone link is ready; the app console opens when the app asks for it"
            : "the app console opens on the phone link once it is ready; see 'phone status'";
      }
      // Design A: the pipe ready, the app has not used its console session yet.
      if (s.pipe.phase == hw1link::Pipe::Ready) {
        return "the phone link is ready; the app console attaches when the app opens it";
      }
      return "the app console attaches to the phone link once the relay pipe is ready; "
             "see 'phone status'";
    case Console::NoSecret:
      return "run 'phone pair new' for the app console on the phone link "
             "(or set 'blesecret' for its Secure Channel attach)";
    case Console::Off:
      return "'bleconsole on' attaches the app console to the phone link";
    default:
      break;  // not here
  }
  if (pipe) {  // not offered: the setting is off, or the phone secret is no device code
    return s.phone.consoleEnabled
        ? "the app console on the phone link needs a device pairing code: "
          "run 'phone pair new' on serial or the OLED"
        : "'bleconsole on' offers the app console on the phone link";
  }
  return "'openconsole' switches to the app console server for a direct app connection (closes the glasses, ring and phone)";
}

// The lines blestatus starts with, in BLE_LinkStatusCore.h's words:
//   Role: Glasses+pipe
//   App console: app signed in
//   Phone link: ready
//     Paired: yes (device code)
//     ... (bleLinkPhoneLines)
// A running console server adds a "  Clients: 1/4, advertising on" line (the
// phone link's console never advertises); the phone link block needs a pipe
// build. Bounded; returns the length written.
const char* bleHostStateWord(bool roleRunning) {
  if (bleStackLifecycleFaulted()) return "fault";
  if (roleRunning) return "on";
  if (bleStackReady()) return "idle";
  return bluetoothHalStatus().hostAbsent() ? "off" : "parked";
}

static int bleFormatLinkLines(char* buf, int size, const BleLinkStatus& s) {
  char role[64];
  int used = snprintf(buf, size, "Role: %s\nApp console: %s",
                      hw1link::roleLine(s.role, s.saved, role, sizeof(role)),
                      hw1link::consoleLongText(s.console));
  if (used < 0) return 0;
  if (used >= size) return size - 1;
  // With no role running, say what the host is actually doing: a verified
  // stop, a client stack that is up with nothing open on it, or a teardown
  // that faulted. "Role: Off" alone used to hide a parked Bluedroid host. The
  // role comes from the status (read under the lifecycle guard by the caller;
  // the owner flags are not re-read here, where the guard may be held).
  if (s.role == hw1link::Role::Off) {
    const char* word = bleHostStateWord(/*roleRunning=*/false);
    const char* host = !strcmp(word, "fault") ? "fault (reboot required)"
                     : !strcmp(word, "idle")  ? "on, client stack up, no device open"
                     : !strcmp(word, "off")   ? "off"
                                              : "host still up (teardown pending)";
    const int w = snprintf(buf + used, size - used, "\nBluetooth: %s", host);
    if (w > 0) used = (used + w < size) ? used + w : size - 1;
  }
  if (s.consoleView.up && s.console != hw1link::Console::Off &&
      s.console != hw1link::Console::NotHere) {
    const int w = snprintf(buf + used, size - used, "\n  Clients: %u/%d, advertising %s",
                           (unsigned)s.consoleView.clients, BLE_MAX_CONNECTIONS,
                           s.consoleView.advertising ? "on" : "off");
    if (w > 0) used = (used + w < size) ? used + w : size - 1;
  }
  if (s.pipeBuilt && used + 1 < size) {
    const int w = bleLinkPhoneLines(buf + used + 1, size_t(size - used - 1), s);
    if (w > 0) { buf[used] = '\n'; used += 1 + w; }
  }
  return used;
}

static const char* cmd_blestatus(const String& argsInput) {
  const bool wantJson = argWantsJson(argsInput);

  BleStatusSnapshot snap;
  if (!bleCaptureStatusSnapshot(snap)) {
    if (wantJson) return "{\"schema\":1,\"error\":\"unavailable\"}";
    return "Error: BLE status unavailable (Bluetooth busy); try again";
  }
  // The role running now, the app console and the relay pipe (never blocks).
  // Outside the app console server the table below is empty (the phone link's
  // console shows in the link words), so these are the whole answer, with how
  // to reach the console.
  const BleLinkStatus link = bleLinkStatus();
  const char* hint = snap.initialized ? nullptr : bleConsoleSwitchHint(link);

  // JSON: machine-readable connection state for the app. Returned to the caller
  // only (no broadcastOutput) so it never lands on the shared consoles. `user`
  // is the display name ("Unknown" until the connection logs in); `authed`
  // tells an anonymous peer from a user named "Unknown", and `connId` is the id
  // bleinfo json and bledisconnect use. `link` is bleLinkStatusJson's object,
  // `phoneLink` (pipe builds) the phone link block (bleLinkPhoneJson).
  if (wantJson) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["initialized"] = snap.initialized;
    // What the Bluetooth host is doing (bleHostStateWord: on / idle / off /
    // fault / parked), as the text form's "Bluetooth:" line.
    doc["host"] = bleHostStateWord(link.role != hw1link::Role::Off);
    if (snap.initialized) {
      doc["state"] = snap.state;
      doc["activeConnections"] = snap.activeConnections;
      doc["maxConnections"] = BLE_MAX_CONNECTIONS;
      doc["totalConnections"] = snap.totalConnections;
      doc["commandsReceived"] = snap.commandsReceived;
      doc["responsesSent"] = snap.responsesSent;
      JsonArray conns = doc["connections"].to<JsonArray>();
      for (int i = 0; i < snap.clientCount; i++) {
        const BleStatusSnapshot::Client& c = snap.clients[i];
        JsonObject o = conns.add<JsonObject>();
        o["index"]        = c.slot;
        o["connId"]       = c.connId;
        o["authed"]       = c.authed;
        o["user"]         = (c.authed && c.user.length() > 0) ? c.user.c_str() : "Unknown";
        o["mac"]          = JsonString(c.mac, false);
        o["connectedSec"] = c.sinceSec;
        o["commands"]     = c.commands;
      }
    }
    bleLinkStatusJson(doc["link"].to<JsonObject>(), link);
    if (link.pipeBuilt) bleLinkPhoneJson(doc["phoneLink"].to<JsonObject>(), link, true);
    if (hint) doc["hint"] = hint;
    if (doc.overflowed()) return "{\"schema\":1,\"error\":\"oom\"}";
    if (measureJson(doc) >= GLOBAL_DEBUG_BUFFER_SIZE) return "{\"schema\":1,\"error\":\"status too large\"}";
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
    return getDebugBuffer();
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  char* buf = getDebugBuffer();
  int offset = bleFormatLinkLines(buf, 1024, link);
  int remaining = 1024 - offset;
  if (!snap.initialized) {
    if (hint) cliHint(hint);
    return buf;
  }

  // Show each active connection
  int written = 0;
  for (int i = 0; i < snap.clientCount && remaining > 100; i++) {
    const BleStatusSnapshot::Client& c = snap.clients[i];
    // Identity shown is the AUTHENTICATED user (firmware-proven via login),
    // NOT the device-advertised name (which the client controls and we don't
    // trust). A connection that hasn't logged in shows "Unknown"; once it
    // authenticates it flips to that user's username. The MAC still
    // identifies the device.
    const char* who = (c.authed && c.user.length() > 0) ? c.user.c_str() : "Unknown";
    written = snprintf(buf + offset, remaining, "\n[%d] %s\n    MAC: %s | %lu sec | %lu cmds",
             c.slot, who, c.mac, (unsigned long)c.sinceSec, (unsigned long)c.commands);
    if (written > 0) { offset += written; remaining -= written; }
  }
  
  written = snprintf(buf + offset, remaining, "\nTotal connections: %lu\n", (unsigned long)snap.totalConnections);
  if (written > 0) { offset += written; remaining -= written; }
  written = snprintf(buf + offset, remaining, "Commands received: %lu\n", (unsigned long)snap.commandsReceived);
  if (written > 0) { offset += written; remaining -= written; }
  written = snprintf(buf + offset, remaining, "Responses sent: %lu", (unsigned long)snap.responsesSent);
  
  // Also broadcast to serial for backwards compatibility
  broadcastOutput(buf);
  BLE_DEBUGF(DEBUG_BLE_CORE, "BLE status requested. Active=%d", snap.activeConnections);

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

#if ENABLE_BLE_PHONE_PIPE
// The `phone secret` steps, shared with `phone pair new` (BLE_PhonePipe.cpp):
// stop the pipe, drop its key, store the secret with its device-made mark,
// clear the enrollment, persist. The mark is set first, so the secret's own
// persist carries both: a code typed with `phone secret` is never on disk
// marked device-made. value is copied into internal RAM; len 0 clears.
// nullptr when done; *replaced says the stored secret changed before a later
// step failed. The BLE role is held from the stop through the store, as
// `closephone` holds it, so no link start (boot, the radio companion, stuck-
// phone recovery) derives its key from the secret being replaced; it is this
// task's (re-entrant) and released before the caller starts the link again.
const char* phonePipeReplaceSecret(const char* value,size_t len,bool generated,bool* replaced) {
  if(replaced)*replaced=false;
  {
    BleRoleTransitionClaim role(BleRoleTransition::PHONE_STOP);
    if(!role)return "Error: BLE role transition busy";
    if(!phonePipeStop())return "Error: phone pipe cleanup incomplete; secret unchanged";
    if(!phonePipeInvalidateSecret())return "Error: pipe key invalidation busy; retry before changing secret";
    String copy=makeInternalSecretString(value,len);
    struct Wipe {String& value;~Wipe(){secureClearString(value);}} wipe{copy};
    if(copy.length()!=len)return "Error: internal secret storage unavailable";
    const bool was=gSettings.phonePipeSecretGenerated;gSettings.phonePipeSecretGenerated=generated;
    if(!setInternalSecretSetting(gSettings.phonePipeSecret,copy)) {
      gSettings.phonePipeSecretGenerated=was;return "Error: internal secret storage unavailable";
    }
  }
  if(replaced)*replaced=true;
  if(!blePeerClearPhonePipeEnrollment())return "Error: secret changed; enrollment cleanup busy, retry phone secret";
  if(!requestSettingsPersist())return "Error: secret changed in RAM but persistence failed; retry savesettings";
  return nullptr;
}

// `phone secret`: the phone pipe's pairing passphrase (BLE_PhonePipe.cpp's
// phoneCommands row). It shares the BLE passphrase policy above. A pairing
// code (phone link v2), in any spelling the code accepts, is stored in its
// canonical form; typed here it is not device-made, so it carries the relay
// only and never offers the app console.
const char* phoneSecretCommand(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String value=argsInput;value.trim();
  struct Wipe {String& value;~Wipe(){secureClearString(value);}} wipe{value};
  if(!value.length())return gSettings.phonePipeSecret.length()?"Phone pipe secret is SET (hidden)":"Phone pipe secret is not set";
  const auto source=currentAuthContext().transport;
  if(source==SOURCE_BLUETOOTH || source==SOURCE_G2_GLASSES)
    return "Error: provision the pipe secret from a non-Bluetooth source";
  char code[hw1code::kSymbols+1]{};
  struct WipeCode {char* text;~WipeCode(){hw1code::wipe(text,hw1code::kSymbols+1);}} wipeCode{code};
  bool isCode=false;
  if(value.equalsIgnoreCase("clear"))value="";
  else {
    if(value.length()>128)return "Error: pipe passphrase must be at most 128 bytes";
    isCode=hw1code::parse(value.c_str(),value.length(),code);
    if(const char* why=isCode?nullptr:blePassphrasePolicyError(value)) {
      static char error[64];snprintf(error,sizeof(error),"Error: pipe passphrase must %s",why);return error;
    }
  }
  if(const char* why=isCode?phonePipeReplaceSecret(code,hw1code::kSymbols,false,nullptr)
                            :phonePipeReplaceSecret(value.c_str(),value.length(),false,nullptr))return why;
  if(isCode)return "Pairing code set as the pipe secret (relay only: the console needs a code from phone pair new); enter the same code on Android, then openphone and phone pair";
  return value.length()?"Pipe secret set; enter the same passphrase on Android, then openphone and phone pair":"Pipe secret cleared; pipe enrollment removed";
}
#endif

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

// `bledisconnect` closes every app client; `bledisconnect <connId>` closes the
// one whose BLE connection id (bleinfo json clients[].connId) matches. A console
// on the phone link (the G2 client host's slot, or the pipe's console row in
// any client role) ends its session only: the link stays, and that console
// starts a fresh signed-out session (the row's stream reopens, CLOSE 2).
static const char* cmd_bledisconnect(const String& argsInput) {
  static const char kSessionEnded[] =
      "App console session ended; the phone link stays and the console starts a new "
      "signed-out session";
  // The dispatcher supplies the arguments without the command name.
  const char* args = argsInput.c_str();
  while (isspace((unsigned char)*args)) args++;
  if (*args == '\0') {
    // The phone link's console sessions end; its link stays (either console).
    BlePipeConsoleIdentity pipeRow;
    blePipeConsoleIdentity(&pipeRow);
    const bool phoneLink = bleConsoleClientHost() || pipeRow.bound;
    const int count = bleDisconnectAppClients();
    if (count == 0) return "Error: No client connected";
    if (phoneLink) return kSessionEnded;
    if (count == 1) return "Disconnecting client...";
    if (!ensureDebugBuffer()) return "Disconnecting clients...";
    snprintf(getDebugBuffer(), 1024, "Disconnecting %d clients...", count);
    return getDebugBuffer();
  }
  // Strict decimal id: "abc", "-1", "1x" or "0x1" must not become conn 0.
  char* end = nullptr;
  const unsigned long id = strtoul(args, &end, 10);
  while (end && isspace((unsigned char)*end)) end++;
  if (!isdigit((unsigned char)*args) || !end || *end != '\0' || id > 0xFFFFu) {
    return "Error: invalid arguments — Usage: bledisconnect [connId]";
  }
  switch (bleDisconnectConnection((uint16_t)id)) {
    case BleDisconnectResult::REQUESTED:
      if (!ensureDebugBuffer()) return "Disconnecting client...";
      snprintf(getDebugBuffer(), 1024, "Disconnecting client %lu...", id);
      return getDebugBuffer();
    case BleDisconnectResult::NOT_FOUND:
      return "Error: No client with that connection id";
    case BleDisconnectResult::PEER_LINK:
      return "Error: That connection is a paired peer link, not an app client";
    case BleDisconnectResult::SESSION_ENDED:
      return kSessionEnded;
    case BleDisconnectResult::UNAVAILABLE:
    default:
    {
      // The console role with the lock busy, or an attached phone-link
      // console, has nothing to suggest.
      const char* hint = bleConsoleSwitchHint(bleLinkStatus());
      if (!hint) return "Error: App console busy; retry";
      if (!ensureDebugBuffer()) return "Error: App console not running";
      snprintf(getDebugBuffer(), 1024, "Error: App console not running; %s.", hint);
      return getDebugBuffer();
    }
  }
}

// The client host never advertises: its console rides the relay pipe's link,
// and advertising would invite a fifth link onto the four-link radio.
static const char kBlePhoneLinkNoAdvertising[] =
    "Error: Not advertising in glasses mode: the app console rides the relay pipe's link "
    "('openconsole' for a direct app connection)";
// Nor does it serve the data service: the phone link carries the command
// service alone, Secure Channel only.
static const char kBlePhoneLinkNoDataService[] =
    "Error: Data streams need the app console server ('openconsole'); "
    "the phone link carries commands only";

static const char* cmd_bleadv(const String& argsInput) {
  if (bleConsoleClientHost()) return kBlePhoneLinkNoAdvertising;
  // Optional argument: start (default) / stop / toggle.
  String a = argsInput; a.trim(); a.toLowerCase();
  if (a == "stop" || (a == "toggle" && bleAdvertising())) {
    stopBLEAdvertising();
    BLE_DEBUGF(DEBUG_BLE_CORE, "bleadv stop");
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
  
  // The command dispatcher supplies arguments without the command name.
  const char* msg = argsInput.c_str();
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

// One stream selector for `blestream <sensors|system|events> [on|off]`:
// no second token or "on" enables, "off" disables, anything else is a usage
// error. Exact tokens, so a word such as "offset" is not read as "off".
static const char* bleStreamSelect(const CommandArgs& a, uint8_t flag,
                                   const char* enabled, const char* disabled) {
  const String& toggle = a.arg(1);
  if (toggle.length() == 0 || toggle.equalsIgnoreCase("on")) {
    bleEnableStream(flag);
    return enabled;
  }
  if (toggle.equalsIgnoreCase("off")) {
    bleDisableStream(flag);
    return disabled;
  }
  return "Error: invalid arguments — Usage: blestream <sensors|system|events> [on|off]";
}

static const char* cmd_blestream(const String& argsInput) {
  // The data service is the server role's alone: the phone link (design A's
  // slot, or the pipe's console stream in any client role) has none.
  const bool server = isBleServerInitialized();
  if (!server && (bleConsoleClientHost() || phonePipeActive())) return kBlePhoneLinkNoDataService;
  if (!server) {
    return "Error: Bluetooth not initialized";
  }
  
  // The command dispatcher supplies arguments without the command name.
  const CommandArgs a(argsInput);
  
  if (a.count() == 0) {
    // Show current status
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    // One reading, under the lifecycle lock (isBLEConnected).
    uint8_t flags = 0;
    uint32_t sensorMs = 0, systemMs = 0, sensors = 0, systems = 0, events = 0;
    {
      BleLifecycleGuard lifecycle;
      if (!lifecycle || !gBLEState) return "Error: Bluetooth not initialized";
      flags = gBLEState->streamFlags;
      sensorMs = gBLEState->sensorStreamInterval;
      systemMs = gBLEState->systemStreamInterval;
      sensors = gBLEState->sensorStreamCount;
      systems = gBLEState->systemStreamCount;
      events = gBLEState->eventCount;
    }
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "Streaming: sensors=%s system=%s events=%s",
             (flags & BLE_STREAM_SENSORS) ? "ON" : "OFF",
             (flags & BLE_STREAM_SYSTEM) ? "ON" : "OFF",
             (flags & BLE_STREAM_EVENTS) ? "ON" : "OFF");
    broadcastOutput(buf);
    snprintf(buf, 1024, "Intervals: sensor=%lums system=%lums",
             (unsigned long)sensorMs, (unsigned long)systemMs);
    broadcastOutput(buf);
    snprintf(buf, 1024, "Stats: sensors=%lu system=%lu events=%lu",
             (unsigned long)sensors, (unsigned long)systems, (unsigned long)events);
    return buf;
  }
  
  const String& sub = a.arg(0);
  if (sub.equalsIgnoreCase("on")) {
    bleEnableStream(BLE_STREAM_ALL);
    BLE_DEBUGF(DEBUG_BLE_DATA, "blestream all=ON");
    return "All streams enabled";
  }
  if (sub.equalsIgnoreCase("off")) {
    bleDisableStream(BLE_STREAM_ALL);
    BLE_DEBUGF(DEBUG_BLE_DATA, "blestream all=OFF");
    return "All streams disabled";
  }
  if (sub.equalsIgnoreCase("sensors")) {
    return bleStreamSelect(a, BLE_STREAM_SENSORS, "Sensor stream enabled", "Sensor stream disabled");
  }
  if (sub.equalsIgnoreCase("system")) {
    return bleStreamSelect(a, BLE_STREAM_SYSTEM, "System stream enabled", "System stream disabled");
  }
  if (sub.equalsIgnoreCase("events")) {
    return bleStreamSelect(a, BLE_STREAM_EVENTS, "Event stream enabled", "Event stream disabled");
  }
  if (sub.equalsIgnoreCase("interval")) {
    // blestream interval <sensor_ms> <system_ms>
    const int sensorMs = a.argInt(1, 0);
    const int systemMs = a.argInt(2, 0);
    if (sensorMs <= 0 || systemMs <= 0) {
      return "Error: invalid arguments — Usage: blestream interval <sensor_ms> <system_ms>";
    }
    if (sensorMs < 100 || systemMs < 100) {
      return "Error: Intervals must be >= 100ms";
    }
    bleSetStreamInterval((uint32_t)sensorMs, (uint32_t)systemMs);
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
  
  return "Error: invalid arguments — Usage: blestream <on|off|sensors|system|events|interval>";
}

static const char* cmd_bleevent(const String& argsInput) {
  // Before the connected check: the pipe's bound console row is a connected
  // app, but the data service is the server role's alone (cmd_blestream).
  if (!isBleServerInitialized() && (bleConsoleClientHost() || phonePipeActive())) {
    return kBlePhoneLinkNoDataService;
  }
  if (!isBLEConnected()) {
    return "Error: No client connected";
  }
  
  // The command dispatcher supplies arguments without the command name.
  const char* msg = argsInput.c_str();
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
  // The command dispatcher supplies arguments without the command name.
  const char* args = argsInput.c_str();
  while (*args == ' ') args++;
  
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
  if (!bluetoothHalSupportsTxPower()) {
    return "Error: BLE TX power control is unavailable on this radio backend";
  }
  // The dispatcher supplies arguments without the command name.
  const char* args = argsInput.c_str();
  while (*args == ' ') args++;  // Skip spaces
  
  if (*args != '\0') {
    int level = atoi(args);
    
    if (level < 0 || level > 7) {
      return "Error: TX power must be 0-7 (0=min/-12dBm, 7=max/+9dBm)";
    }
    
    // Apply first: a failed controller operation must not be reported as saved
    // and applied. With BLE off, preserve the setting for the next start.
    if (bluetoothHalStatus().ready()) {
      const esp_err_t powerErr = bluetoothHalSetTxPower((uint8_t)level);
      if (powerErr != ESP_OK) {
        if (!ensureDebugBuffer()) return "Error: BLE TX power update failed";
        snprintf(getDebugBuffer(), 1024, "Error: BLE TX power update failed: %s", esp_err_to_name(powerErr));
        return getDebugBuffer();
      }
    }
    setSetting(gSettings.bleTxPower, level);
    
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

bool buildBLEInfoJson(JsonObject out) {
  if (out.isNull()) return false;
  out["schema"]              = 1;
  out["deviceName"]     = gSettings.bleDeviceName;
  out["txPower"]        = gSettings.bleTxPower;
  out["autoStart"]      = gSettings.bleAutoStart;
  out["requireAuth"]    = gSettings.bleRequireAuth;
  out["secureChannelRequired"] = bleScRequired();
  BleStatusSnapshot snap;
  if (!bleCaptureStatusSnapshot(snap)) return false;
  out["initialized"]    = snap.initialized;
  out["state"]          = snap.state;
  out["connections"]    = snap.activeConnections;
  out["maxConnections"] = BLE_MAX_CONNECTIONS;
  out["totalConnections"] = snap.totalConnections;  // lifetime
  out["commandsReceived"] = snap.commandsReceived;  // lifetime
  // Per-connection identity for the web "Connected Clients" card. Emit the
  // AUTHENTICATED facts (authed + raw user) rather than a pre-baked "Unknown",
  // so the UI can tell an unauthenticated connection from a user named
  // "Unknown" — the ambiguity the old text format couldn't represent.
  JsonArray clients = out["clients"].to<JsonArray>();
  for (int i = 0; i < snap.clientCount; i++) {
    const BleStatusSnapshot::Client& c = snap.clients[i];
    JsonObject o = clients.add<JsonObject>();
    o["connId"]   = c.connId;
    o["authed"]   = c.authed;
    o["user"]     = c.user;   // "" when not authed; UI renders authed ? user : "Unknown"
    o["since"]    = c.sinceSec;
    o["commands"] = c.commands;
    o["mac"]      = JsonString(c.mac, false);
  }
  return true;
}

static const char* cmd_bleinfo(const String& argsInput) {
  // Structured path: BLE config + live state, one verbatim JSON blob via the
  // return value. No broadcastOutput. ArduinoJson escapes the user-settable
  // device name correctly. No secrets emitted. A pipe build adds the phone
  // link block beside buildBLEInfoJson's keys (that object is also
  // /api/ble/status `ble`, which carries the block as its own `phoneLink`).
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    const bool available = buildBLEInfoJson(doc.to<JsonObject>());
    if (available && ENABLE_BLE_PHONE_PIPE) {
      bleLinkPhoneJson(doc["phoneLink"].to<JsonObject>(), bleLinkStatus(), true);
    }
    if (doc.overflowed()) return "{\"error\":\"oom\"}";
    if (!available) return "{\"error\":\"unavailable\"}";
    if (measureJson(doc) >= 2048) return "{\"error\":\"status too large\"}";
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

  // One reading under the lifecycle lock (isBLEConnected).
  const char* stateText = nullptr;
  int connections = 0;
  {
    BleLifecycleGuard lifecycle;
    if (lifecycle && gBLEState && gBLEState->initialized) {
      stateText = bleStateTextLocked();
      connections = gBLEState->activeConnectionCount;
    }
  }
  if (stateText) {
    w = snprintf(buf + pos, rem, "Status:       %s\n", stateText);                                        if (w > 0) { pos += w; rem -= w; }
    w = snprintf(buf + pos, rem, "Connections:  %d/%d", connections, BLE_MAX_CONNECTIONS);                if (w > 0) { pos += w; rem -= w; }
  } else {
    w = snprintf(buf + pos, rem, "Status:       Not initialized");                                        if (w > 0) { pos += w; rem -= w; }
  }
  // The phone link block (state, console, relay, streams, ends; no addresses).
  if (ENABLE_BLE_PHONE_PIPE && pos >= 0 && pos + 1 < 1024) {
    const int n = bleLinkPhoneLines(buf + pos + 1, size_t(1024 - pos - 1), bleLinkStatus());
    if (n > 0) { buf[pos] = '\n'; pos += 1 + n; }
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

// `bleconsole [on|off|status]`: the app console on the phone link. The pipe
// offers its console stream with a device code (`phone pair new`) and closes
// it when this turns off; beside the glasses the client host's command service
// (BLE_ConsoleAttachCore.h, blesecret required) is registered whenever the
// host starts. Either way a change applies at once: off ends the session now.
static const char* cmd_bleconsole(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = argsInput; a.trim(); a.toLowerCase();
  if (a.length() && a != "status") {
    const int on = parseBoolArg(a);
    if (on < 0) return "Error: invalid arguments — Usage: bleconsole [on|off|status]";
    setSetting(gSettings.bleConsoleOnPipe, on == 1);
    bleConsoleReconcile();
  }
  const bool on = gSettings.bleConsoleOnPipe;
  if (!ensureDebugBuffer()) {
    return on ? "App console on the phone link: on" : "App console on the phone link: off";
  }
  // The console's own words (BLE_LinkStatusCore.h) where it can run: a client role.
  const BleLinkStatus link = bleLinkStatus();
  const bool clientRole = link.role == hw1link::Role::Glasses ||
                          link.role == hw1link::Role::GlassesPipe ||
                          link.role == hw1link::Role::PhonePipe;
  char* buf = getDebugBuffer();
  if (!on) {
    snprintf(buf, 1024, "App console on the phone link: off");
  } else if (clientRole) {
    snprintf(buf, 1024, "App console on the phone link: on (%s)",
             hw1link::consoleLongText(link.console));
  } else {
    snprintf(buf, 1024, "App console on the phone link: on (used while the phone link runs)");
  }
  // Neither way can open: no device code, and (beside the glasses) no blesecret.
  const bool code = link.phone.secret == hw1link::Secret::Code;
  if (on && !code && link.role == hw1link::Role::PhonePipe) {
    cliHint("the app console on the phone link needs a device pairing code - run 'phone pair new'");
  } else if (on && !code && !link.consoleView.secret) {
    cliHint("the app console on the phone link needs a device pairing code ('phone pair new'), "
            "or beside the glasses 'blesecret' for the Secure Channel attach");
  }
  return buf;
}

// -----------------------------------------------------------------------------
// Boot role: server (App console) or client (the shared stack: Glasses, + relay
// pipe). The saved keyword; BLE_LinkStatus.h has what runs now.
// -----------------------------------------------------------------------------

const char* getBleModeString() {
  return (gSettings.bleMode == BLE_MODE_G2_CLIENT) ? "client" : "server";
}

// Logical aggregate status: a bare enabled controller/host is not sufficient.
// One application role must own initialized runtime state.
bool bleSubsystemActive() {
  if (bleStackReady()) return true;
  if (phonePipeActive()) return true;
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
  // The pipe's shared word, not its raw label (the dashboard's State row).
  if (phonePipeActive()) return hw1link::pipeText(bleLinkStatus().pipe.phase);
  return "uninitialized";
}

static const char* cmd_blerecords(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String value=args;value.trim();
  return bleCoexCommand(value.c_str());
}

static const char* cmd_blemode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    // The saved boot role (what boot brings back) and the role running now.
    // They differ when, e.g., `openphone` brought the client stack up while
    // the saved role is server.
    if (!ensureDebugBuffer()) return getBleModeString();
    const BleLinkStatus link = bleLinkStatus();
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "Saved mode: %s (%s); running: %s", getBleModeString(),
             hw1link::roleText(link.saved), hw1link::roleText(link.role));
    return buf;
  }
  arg.toLowerCase();

  // The saved BOOT role only. The web settings dropdown stores bleMode as its
  // int enum value (see the "bluetoothMode" SettingEntry: 0=server, 1=client)
  // and saves via `blemode <value>`; accept those numeric forms and the words
  // the roles are known by. The running role is changed by the verbs:
  // `openconsole` / `closeconsole` switch to and from the server (and persist
  // it), `openble` / `openg2` / `openring` / `openphone` use the client stack.
  if (arg == "0") arg = "server";
  else if (arg == "1" || arg == "g2" || arg == "glasses") arg = "client";
  if (arg != "server" && arg != "client") {
    return "Error: invalid arguments — Usage: blemode [client|server]";
  }
  if (arg == "client" && !bleModeIsAvailable(BLE_MODE_G2_CLIENT)) {
    return "Error: [BLE] no client device is compiled in (ENABLE_G2_GLASSES / ENABLE_BLE_PHONE_PIPE)";
  }
  setSetting(gSettings.bleMode, arg == "server" ? (int)BLE_MODE_SERVER : (int)BLE_MODE_G2_CLIENT);
  if (!ensureDebugBuffer()) return "[BLE] Boot role set";
  const bool serverRunning = isBleServerInitialized();
  const bool clientRunning = bleStackReady();
  const char* next = "";
  if (arg == "server") {
    if (!serverRunning) next = clientRunning ? "; 'openconsole' switches to it now" : "; 'openconsole' starts it now";
  } else if (!clientRunning) {
    next = serverRunning ? "; 'closeconsole', then 'openble' starts it" : "; 'openble' starts it now";
  }
  snprintf(getDebugBuffer(), 1024, "[BLE] Boot role set to %s (%s); running now: %s%s",
           getBleModeString(),
           hw1link::roleText(hw1link::roleOfMode(gSettings.bleMode, ENABLE_BLE_PHONE_PIPE)),
           serverRunning ? "app console server" : clientRunning ? "client stack" : "nothing", next);
  return getDebugBuffer();
}

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry bluetoothCommands[] = {
  // Start/Stop (voice phrases live in kVoiceRoutes, System_ESPSR.cpp)
  { "openble",      "Bluetooth on: bring the shared client stack up; devices with auto-reconnect come back (openg2 / openring / openphone open one).", true, cmd_openble },
  { "closeble",     "Bluetooth off: close the glasses, ring, phone and the app console, then stop the host (bleenabled 0 also persists it).", true, cmd_closeble },
  { "openconsole",  "Start the app console server for a direct app connection (closes the glasses, ring and phone; saved boot role = server).", true, cmd_openconsole },
  { "closeconsole", "Stop the app console server (saved boot role back to client; 'openble' brings the client stack back).", true, cmd_closeconsole },
  { "bleread",      "Read the Bluetooth role, app console and relay pipe. (add 'json' for JSON output)", false, cmd_blestatus },
  { "blestatus",    "Show the Bluetooth role, app console and relay pipe. (add 'json' for JSON output)", false, cmd_blestatus },
  { "bleinfo",      "Show BLE configuration and settings. (add 'json' for JSON output)",      false, cmd_bleinfo },
  { "blename",      "Get/set BLE device name [name].",           false, cmd_blename, "Usage: blename [name]" },
  { "bletxpower",   "Get/set BLE TX power [0-7].",               false, cmd_bletxpower, "Usage: bletxpower [0..7]" },
  { "bledisconnect","Disconnect BLE app clients: all, or one by connection id [connId].", true, cmd_bledisconnect, "Usage: bledisconnect [connId]" },
  { "bleadv",       "Start/stop/toggle BLE advertising [start|stop|toggle].", true, cmd_bleadv, "Usage: bleadv [start|stop|toggle]" },
  { "blesend",      "Send message to BLE client: <message>.",    false, cmd_blesend, "Usage: blesend <message>" },
  { "blestream",    "Control streaming: <on|off|sensors|system|events|interval>.",false, cmd_blestream, "Usage: blestream [on|off|sensors|system|events|interval] | interval <sensor_ms> <system_ms>" },
  { "bleevent",     "Send event to BLE client: <event>.",        false, cmd_bleevent, "Usage: bleevent <message>" },
  
  // Auto-start
  { "bleautostart",    "Enable/disable BLE auto-start after boot [on|off].",   true, cmd_bleautostart,    "Usage: bleautostart [on|off]" },
  { "blerequireauth", "Enable/disable BLE authentication requirement [on|off].", true, cmd_blerequireauth, "Usage: blerequireauth [on|off]", /*requiresSuperAdmin=*/true },

  // Mode: one host owner at a time; with the pipe built the app console rides the phone link
  { "blemode", "Get/set the saved Bluetooth boot role: client = the shared stack for glasses, ring and phone (default); server = the app console server (legacy).", true, cmd_blemode,
    "Usage: blemode [client|server]\n"
    "  client = the shared client stack: glasses, ring and phone open on it (openg2 / openring / openphone; auto-reconnect at boot)\n"
    "  server = the app console server, advertising at boot (openconsole starts it now; it never coexists with the client stack)" },
  { "bleconsole", "App console on the phone link: its console stream (a device code from 'phone pair new'), and beside the glasses the Secure Channel attach [on|off|status].", true, cmd_bleconsole,
    "Usage: bleconsole [on|off|status]" },

  { "blerecords", "BLE scan/connect records with the Wi-Fi state (no addresses).", true, cmd_blerecords,
    "Usage: blerecords [scans|connects|clear]" },

  // App-layer Secure Channel v1 (X25519+PSK+ChaCha20-Poly1305; no BLE bonding)
  { "blesecret", "Set/clear the BLE Secure Channel passphrase: blesecret <phrase|clear>.", true, cmd_blesecret, "Usage: blesecret <passphrase|clear>", /*requiresSuperAdmin=*/true },
  { "blesecure", "Require app-layer BLE encryption [on|off].",                            true, cmd_blesecure, "Usage: blesecure [on|off]", /*requiresSuperAdmin=*/true },

  // Auto-reconnect (boot + mid-session drop) to saved-MAC peers. Pairing is
  // separate (`openg2 auto`, `openring scan`, `phone pair`) and always saves the MAC; this
  // flag controls whether reconnect runs automatically.
  { "bleautoreconnect",  "Per-peer auto-reconnect (boot + mid-session drop): bleautoreconnect <name> [on|off]. `blepeers` lists names.", true, cmd_bleautoreconnect, "Usage: bleautoreconnect <peer-name> [on|off]\n  on: reconnect at boot and reseek after unexpected drops (not after closering / closeg2 / closephone)" },
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
// The bluetoothMode dropdown names each boot role by the role it starts, in
// BLE_LinkStatusCore.h's words (hw1link::roleText of roleOfMode; the wizard,
// blemode and the lens use the same; test_ble_info_status pins the two
// together). The client role is offered when a client device is compiled in.
static constexpr const char* kBleModeChoices = "0|App console"
#if ENABLE_BLE_PHONE_PIPE
  ",1|Glasses+pipe"
#elif ENABLE_G2_GLASSES
  ",1|Glasses"
#endif
  ;
static constexpr int kBleModeMaximum = (ENABLE_G2_GLASSES || ENABLE_BLE_PHONE_PIPE) ? 1 : 0;
const SettingEntry bluetoothSettingsEntries[] = {
  { "bleEnabled", SETTING_BOOL, &gSettings.bleEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "bleenabled" },
  { "bluetoothAutoStart",    SETTING_BOOL,   &gSettings.bleAutoStart,    false, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "bleautostart" },
  { "bluetoothRequireAuth",  SETTING_BOOL,   &gSettings.bleRequireAuth,  true, 0, nullptr, 0, 1, "Require Authentication", nullptr, false, nullptr, "blerequireauth" },
  { "bluetoothDeviceName", SETTING_STRING, &gSettings.bleDeviceName, 0, 0, "HardwareOne", 0, 0, "Device Name", nullptr, false, nullptr, "blename" },
  { "bluetoothTxPower",      SETTING_INT,    &gSettings.bleTxPower,            3, 0, nullptr, 0, 7, "TX Power (0-7)", nullptr, false, nullptr, "bletxpower" },
  { "bluetoothMode",         SETTING_INT,    &gSettings.bleMode,               kBleModeDefaultForBuild, 0, nullptr, 0, kBleModeMaximum, "Mode", kBleModeChoices, false, nullptr, "blemode" },
  { "bleRequireSecureChannel", SETTING_BOOL, &gSettings.bleRequireSecureChannel, true, 0, nullptr, 0, 1, "Require Secure Channel", nullptr, false, nullptr, "blesecure" },
  { "bleSecureChannelSecret",  SETTING_STRING, &gSettings.bleSecureChannelSecret, 0, 0, "", 0, 0, "Secure Channel Secret", nullptr, true, nullptr, "blesecret" },
  { "bleConsoleOnPipe", SETTING_BOOL, &gSettings.bleConsoleOnPipe, true, 0, nullptr, 0, 1, "App console on the phone link", nullptr, false, nullptr, "bleconsole" },
  { "phonePipeSecret", SETTING_STRING, &gSettings.phonePipeSecret, 0, 0, "", 0, 128, "Phone pipe secret", nullptr, true, nullptr, "phone secret", false, true },
  // Set by `phone pair new` only, cleared by `phone secret` and factory reset: no
  // command (no cmdKey) and display-only, so no surface can mark a passphrase
  // device-made and offer it the app console.
  { "phonePipeSecretGenerated", SETTING_BOOL, &gSettings.phonePipeSecretGenerated, false, 0, nullptr, 0, 1, "Phone secret is a device code", nullptr, false, nullptr, nullptr, true },

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
  const size_t length = strnlen(msg, BLE_MSG_MAX_LEN - 1);
  memcpy(bleMessageHistory[bleMessageHead], msg, length);
  bleMessageHistory[bleMessageHead][length] = '\0';
  
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

// Data-service streams (sensor, system and event characteristics) are plaintext
// and outside the Secure Channel; callers suppress them when a channel is
// required. Otherwise they go to the console-mirror recipients (logged-in,
// verified non-guest sessions, the rule sendBLEResponse uses) as one directed
// notify per recipient, never the characteristic-wide notify(), which the
// Arduino wrapper fans out to every connected peer. One attempt per recipient:
// the streams are periodic best-effort, so a busy link misses one sample.
// Returns true when at least one recipient accepted the frame.
static bool bleStreamNotifyDirected(BLECharacteristic* ch, const char* data, size_t len) {
  if (!ch || !data || len == 0) return false;
  BleSessionFence sessions[BLE_MAX_CONNECTIONS];
  const int count = bleCollectMirrorSessions(sessions, BLE_MAX_CONNECTIONS);
  bool delivered = false;
  for (int i = 0; i < count; ++i) {
    // Same directed IDF primitive and the same lifecycle critical section as
    // bleRawNotifyToSession, so slot reuse is ordered against this send.
    BleLifecycleGuard lifecycle;
    if (!lifecycle ||
        !bleConnectionEpochMatchesLocked(sessions[i].connId, sessions[i].epoch)) {
      continue;
    }
    const int gattsIf = sBleGattsIf.load(std::memory_order_acquire);
    if (gattsIf == ESP_GATT_IF_NONE) break;
    if (esp_ble_gatts_send_indicate(static_cast<esp_gatt_if_t>(gattsIf),
                                    sessions[i].connId, ch->getHandle(),
                                    static_cast<uint16_t>(len),
                                    reinterpret_cast<uint8_t*>(const_cast<char*>(data)),
                                    false) == ESP_OK) {
      delivered = true;
    }
  }
  return delivered;
}

bool blePushSensorData(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSensorDataChar) {
    return false;
  }
  // The Data-service streams are NOT part of the Secure Channel (separate chars).
  // When encryption is required, suppress them rather than emit plaintext over the air.
  if (bleScRequired()) return false;

  if (!bleStreamNotifyDirected(pSensorDataChar, jsonData, len)) return false;
  
  BleLifecycleGuard lifecycle;  // the count lives in gBLEState (isBLEConnected)
  if (lifecycle && gBLEState) {
    gBLEState->sensorStreamCount++;
  }
  
  return true;
}

bool blePushSystemStatus(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSystemStatusChar) {
    return false;
  }
  if (bleScRequired()) return false;  // not on the Secure Channel — don't leak plaintext

  if (!bleStreamNotifyDirected(pSystemStatusChar, jsonData, len)) return false;
  
  BleLifecycleGuard lifecycle;  // the count lives in gBLEState (isBLEConnected)
  if (lifecycle && gBLEState) {
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

  if (!bleStreamNotifyDirected(pEventNotifyChar, eventJson, (size_t)pos)) return false;
  
  BleLifecycleGuard lifecycle;  // the count lives in gBLEState (isBLEConnected)
  if (lifecycle && gBLEState) {
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
  // The phone link's console row takes one CARD record (TryNow): the bare JSON
  // object, without the `#NOTIF ` prefix and the newline (phone link v2).
  if (bleIsPipeRowId(connId)) {
    constexpr size_t kPrefix = sizeof("#NOTIF ") - 1;
    return bleSendToSession(connId, expectedEpoch, PipeConsoleKind::Card, line + kPrefix,
                            (size_t)n - kPrefix - 1, /*blocking=*/false);
  }
  return sendBLEResponseToSession(connId, expectedEpoch, line, (size_t)n,
                                  /*blocking=*/false);
}

// =============================================================================
// STREAM CONTROL
// =============================================================================

// The stream settings live in gBLEState: read and written under the lifecycle
// lock (isBLEConnected).
void bleEnableStream(uint8_t streamFlags) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState) return;
  gBLEState->streamFlags |= streamFlags;
}

void bleDisableStream(uint8_t streamFlags) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState) return;
  gBLEState->streamFlags &= ~streamFlags;
}

void bleSetStreamInterval(uint32_t sensorMs, uint32_t systemMs) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState) return;
  gBLEState->sensorStreamInterval = sensorMs;
  gBLEState->systemStreamInterval = systemMs;
}

bool bleIsStreamEnabled(uint8_t streamFlag) {
  BleLifecycleGuard lifecycle;
  if (!lifecycle || !gBLEState) return false;
  return (gBLEState->streamFlags & streamFlag) != 0;
}

// =============================================================================
// AUTO-STREAMING (Call from main loop)
// =============================================================================

static void buildSensorDataJSON(char* buf, size_t bufSize) {
  int pos = snprintf(buf, bufSize, "{\"sensors\":{");
  
  #if ENABLE_THERMAL_SENSOR
  if (gThermalRunning && gThermalConnected) {
    ThermalCache thermal{};
    if (thermalCacheSnapshot(thermal, 10) && thermal.thermalDataValid) {
      // "avg" (was "center"): ThermalCache has no center field — the old local
      // struct invented thermalCenterTemp/HottestX/HottestY, so this read landed
      // on thermalMinTemp while min/max read the interpolated width/height ints
      // punned as floats (both printed 0.0). thermalAvgTemp is the real field.
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"thermal\":{\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f,\"valid\":true},",
                      thermal.thermalMinTemp,
                      thermal.thermalMaxTemp,
                      thermal.thermalAvgTemp);
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gTofRunning && gTofConnected) {
    TofCache tof{};
    if (tofCacheSnapshot(tof, 10) && tof.tofDataValid && tof.tofTotalObjects > 0) {
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"tof\":{\"dist_mm\":%d,\"valid\":true},",
                      tof.tofObjects[0].distance_mm);
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gImuRunning && gImuConnected) {
    ImuCache imu{};
    if (imuCacheSnapshot(imu, 10) && imu.imuDataValid) {
      // Real field names are oriYaw/oriPitch/oriRoll. The old local struct omitted
      // imuTemp, shifting everything after it: "heading" read imuTemp (streamed
      // TEMPERATURE), pitch read oriYaw, roll read oriPitch, and oriRoll was never
      // sent at all. JSON keys kept (heading == yaw) so the wire format is stable.
      pos += snprintf(buf + pos, bufSize - pos,
                      "\"imu\":{\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"valid\":true},",
                      imu.oriYaw,
                      imu.oriPitch,
                      imu.oriRoll);
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
  // The client host's console slot follows the phone pipe's link record, so
  // it is reconciled before the no-client return (BLE_ConsoleAttachCore.h).
  bleConsoleReconcile();
  // So does the phone link's console row (its session READY).
  blePipeConsoleReconcile();
  // gBLEState's last holder frees it on its own task (the pipe's on `phone
  // stop`): it is read only under the lifecycle lock, never across a send.
  if (!isBLEConnected()) return;

  uint32_t now = millis();

  // Maintain BLE CLI sessions (the phone link's console row included)
  bleSessionTick();
  
  // The streams are the server role's data service; no other holder has one.
  // What is due is taken under the lock, and each sample goes out with it
  // released (a push takes it per recipient).
  bool sensorsDue = false, systemDue = false;
  {
    BleLifecycleGuard lifecycle;
    if (!lifecycle || !gBLEState || !(sBleStateHolders & kBleHoldServer)) return;
    BLESystemState& s = *gBLEState;
    sensorsDue = (s.streamFlags & BLE_STREAM_SENSORS) &&
                 now - s.lastSensorStream >= s.sensorStreamInterval;
    if (sensorsDue) s.lastSensorStream = now;
    systemDue = (s.streamFlags & BLE_STREAM_SYSTEM) &&
                now - s.lastSystemStream >= s.systemStreamInterval;
    if (systemDue) s.lastSystemStream = now;
  }

  // Stream sensor data
  if (sensorsDue) {
    char sensorBuf[512];
    buildSensorDataJSON(sensorBuf, sizeof(sensorBuf));
    blePushSensorData(sensorBuf, strlen(sensorBuf));
  }
  
  // Stream system status
  if (systemDue) {
    char systemBuf[256];
    buildSystemStatusJSON(systemBuf, sizeof(systemBuf));
    blePushSystemStatus(systemBuf, strlen(systemBuf));
  }
}

#endif // ENABLE_BLUETOOTH
