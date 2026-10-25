#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include "System_BuildConfig.h"
#include "System_User.h"
#include "BLE_ConsoleAttachCore.h"  // hw1console::Mode

// =============================================================================
// BLE MODE CONSTANTS (available regardless of ENABLE_BLUETOOTH)
// =============================================================================
// The BLE radio runs in one of two mutually exclusive roles: the shared client
// stack (BLE_CentralStack.cpp) that the glasses, the ring and the phone pipe
// open on, or the app-console server (openconsole). gSettings.bleMode is the
// saved BOOT role, set by `blemode`, `openconsole` / `closeconsole` and the
// Bluetooth web page; the running role is whatever was opened last.
#define BLE_MODE_SERVER     0   // Peripheral / GATT server for the app (Bluetooth)
#define BLE_MODE_G2_CLIENT  1   // The client stack: glasses, ring and phone pipe open on it

inline bool bleModeIsAvailable(int mode) {
  return mode == BLE_MODE_SERVER ||
      ((ENABLE_G2_GLASSES || ENABLE_BLE_PHONE_PIPE) && mode == BLE_MODE_G2_CLIENT);
}

// Result of a bledisconnect request; the close itself completes asynchronously in onDisconnect.
// SESSION_ENDED: a console on the phone link ended its session at once (the
// client host's slot in G2 mode, or the pipe's console row in any client
// role); the link stays and the console starts a fresh signed-out session.
enum class BleDisconnectResult : uint8_t { REQUESTED, NOT_FOUND, PEER_LINK, UNAVAILABLE, SESSION_ENDED };

// The app console as stream slot 2 of the phone pipe (phone link v2,
// BLE_PhonePipeConsole.h). Its session is a gBLEState row with its own id,
// 0x8000 | (generation & 0x7FFF), never a Bluedroid connection id, so design
// A's slot on the same link keeps its identity. The lifecycle lock and the
// pipe's locks are never nested: the pipe side reads the row only through
// blePipeConsoleIdentity()'s leaf snapshot.
struct BlePipeConsoleIdentity {
  bool bound = false;
  uint32_t generation = 0;           // the pipe GATT generation it was bound for
  uint16_t streamId = 0;             // the console stream it serves
  uint16_t rowConnId = 0;            // the row's own id (its sid)
  TransportSessionEpoch epoch = 0;   // the row's live session epoch
  bool authed = false;
};
inline constexpr uint16_t blePipeConsoleRowId(uint32_t generation) {
  return uint16_t(0x8000u | (generation & 0x7FFFu));
}

#if ENABLE_BLUETOOTH

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "BLE_Types.h"
#include "System_Command.h"
#include "System_Debug.h"

// =============================================================================
// BLE SERVICE AND CHARACTERISTIC UUIDs
// =============================================================================
// Using custom UUIDs for HardwareOne services
// Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

// Command Service - single service for all communication
// Glasses send commands, receive responses - uses existing command system
#define BLE_COMMAND_SERVICE_UUID      "12345678-1234-5678-1234-56789abcdef0"
#define BLE_CMD_REQUEST_CHAR_UUID     "12345678-1234-5678-1234-56789abcde01"  // Write - send any command
#define BLE_CMD_RESPONSE_CHAR_UUID    "12345678-1234-5678-1234-56789abcde02"  // Notify - receive response
#define BLE_CMD_STATUS_CHAR_UUID      "12345678-1234-5678-1234-56789abcde03"  // Read - connection status

// Data Streaming Service - continuous sensor and system data
#define BLE_DATA_SERVICE_UUID         "12345678-1234-5678-1234-56789abcdef1"
#define BLE_SENSOR_DATA_CHAR_UUID     "12345678-1234-5678-1234-56789abcde11"  // Notify - sensor data stream
#define BLE_SYSTEM_STATUS_CHAR_UUID   "12345678-1234-5678-1234-56789abcde12"  // Notify - system status stream
#define BLE_EVENT_NOTIFY_CHAR_UUID    "12345678-1234-5678-1234-56789abcde13"  // Notify - event notifications
// Streams are controlled with the `blestream` command; the data service has no
// writable characteristic.

// Device Info (standard BLE service)
#define BLE_DEVICE_INFO_SERVICE_UUID  "180A"
#define BLE_MANUFACTURER_CHAR_UUID    "2A29"
#define BLE_MODEL_CHAR_UUID           "2A24"
#define BLE_FIRMWARE_CHAR_UUID        "2A26"

// =============================================================================
// BLE SYSTEM STATE STRUCTURE
// =============================================================================

struct BLEConnection {
  bool active;
  uint16_t connId;                  // Connection ID from BLE stack
  uint32_t connectedSince;          // millis() when connected
  String deviceName;                // Name of connected device (if available)
  uint8_t deviceAddr[6];            // MAC address of connected device
  BLEDeviceType deviceType;         // Device type (identified by MAC)
  uint32_t commandsReceived;        // Commands from this device

  // Per-connection CLI authentication
  bool authed;
  String user;
  // Console-mirror recipient: the account was verified non-guest when this
  // session logged in (the web console mirror's rule).  Meaningful only while
  // authed; bleBindSession is the single writer of a true value.
  bool mirrorEligible;
  // Client host (the console on the phone pipe's link): this session has had
  // its one SC_REJECT "no channel" (bleScDeferredInbound).
  bool noChannelRejected;
  uint32_t lastActivityMs;
  // The phone link's console row (phone link v2): connId is
  // blePipeConsoleRowId(pipeGeneration), never a Bluedroid id; its output goes
  // to stream pipeStream of that pipe session as console records.
  // pipeLinkConnId is the link's own id, for display only.
  bool pipeConsole;
  uint32_t pipeGeneration;
  uint16_t pipeStream;
  uint16_t pipeLinkConnId;
  // Its idle-logout notice did not fit the stream (TryNow): offered again
  // while the session epoch is still noticeEpoch.
  bool noticePending;
  TransportSessionEpoch noticeEpoch;
  String noticeUser;
};

struct BLESystemState {
  bool initialized;
  BLEConnectionState connectionState;
  BLEConnection connections[BLE_MAX_CONNECTIONS];
  uint8_t activeConnectionCount;
  
  // Statistics
  uint32_t totalConnections;
  uint32_t commandsReceived;
  uint32_t responsesSent;
  
  // Streaming state
  uint8_t streamFlags;              // Active streams (BLEStreamFlags)
  uint32_t sensorStreamInterval;    // ms between sensor updates
  uint32_t systemStreamInterval;    // ms between system updates
  uint32_t lastSensorStream;        // millis() of last sensor stream
  uint32_t lastSystemStream;        // millis() of last system stream
  uint32_t sensorStreamCount;       // Total sensor streams sent
  uint32_t systemStreamCount;       // Total system streams sent
  uint32_t eventCount;              // Total events sent
  
  // Deferred event handling (ISR-safe pattern: callback sets flag, task processes)
  // Connect event
  bool deferredConnectPending;
  int deferredConnectSlot;
  
  // Disconnect event  
  bool deferredDisconnectPending;
  int deferredDisconnectActiveCount;
  
  // Command received event (for logging only - actual processing already deferred)
  bool deferredCmdReceivedPending;
  uint16_t deferredCmdReceivedConnId;
  size_t deferredCmdReceivedLen;
};

extern BLESystemState* gBLEState;

// =============================================================================
// DEBUG FLAGS
// =============================================================================

#define DEBUG_BLE_CORE        DEBUG_BLUETOOTH_CORE
#define DEBUG_BLE_GATT        DEBUG_BLUETOOTH_GATT
#define DEBUG_BLE_DATA        DEBUG_BLUETOOTH_DATA

// =============================================================================
// PUBLIC API
// =============================================================================

// Initialization
bool initBluetooth();
// True once the server is stopped (or was not running). False when the stop
// was deferred (role transition busy, Ring owner still active: the server keeps
// running with its sessions) or the host teardown failed (reboot required);
// the reason is broadcast. Under a RECOVERING claim (companion reset, which
// takes the host down regardless) a Ring-deferred stop closes session admission
// and advertising before it returns false.
bool deinitBluetooth();
void bleSecurityBootNotice();  // boot-time "set a passphrase" nudge when BLE is unencrypted

// Device type management
const char* bleDeviceTypeToString(BLEDeviceType type);
BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t* mac);

// Advertising control
bool startBLEAdvertising();
void stopBLEAdvertising();
// Connectable advertising is running (re-armed while another app fits).
bool bleAdvertising();
// The console server is up; lock-free, for status surfaces.
bool bleServerUp();
// The app console (this device's command service) for the status surfaces:
// BLE_LinkStatus.h turns it into words. Never blocks.
struct BleConsoleView {
  bool up = false;           // the console GATT server is initialized (server mode)
  bool advertising = false;  // visible to a scanning app
  uint8_t clients = 0;       // app sessions: centrals connected (peer links excluded)
                             // and the phone link's console row, once bound
  bool signedIn = false;     // one of them is logged in
  // The client host (G2 mode): the command service on the phone pipe's link.
  bool registered = false;   // registered on this host
  bool attachable = false;   // ... and `bleconsole` is on
  bool attached = false;     // the app has joined the console slot on the pipe's link
  bool secret = false;       // blesecret is set: the phone link serves Secure Channel only
};
BleConsoleView bleConsoleView();
// Where the app console's command service lives (BLE_ConsoleAttachCore.h):
// Server while openble owns the host (isBleServerInitialized()), and
// ClientRegistered from bleConsoleRegisterOnClientHost() until the host
// teardown that frees its state. Lock-free, any task (BTC included).
hw1console::Mode bleConsoleMode();
// Registers the command service on the G2 client host initG2Client has just
// started, before any scan or connect: with no link up, starting it sends no
// Service Changed. Allocates the console state (initialized stays false, so
// isBleServerInitialized() still means server mode), never advertises and
// leaves the MTU and TX power to the client. Call it there only, under the
// role claim, never on BTC or under the lifecycle lock. False (reason logged)
// leaves the console off; the glasses and pipe are unaffected. Never start a
// GATT service later in client mode: every open link would be sent Service
// Changed.
bool bleConsoleRegisterOnClientHost();
// The console's side of every host teardown; only bluetoothHalDeinit() calls
// them. Begin closes admission, ends every console session and drops the GATT
// pointers before the host stops; End(stopped) frees the client-host state once
// the stop is terminal (a failed stop keeps it, closed). Server mode is left
// to deinitBluetooth().
void bleConsoleHostTeardownBegin();
void bleConsoleHostTeardownEnd(bool stopped);

// Connection management
bool isBLEConnected();
// Close one app client by its BLE connection id. Peer links (G2 temples, R1 ring)
// are refused; the slot is cleared later by onDisconnect. A console on the phone
// link (the G2 client host's slot, or the pipe's console row, whose id is its
// blePipeConsoleRowId) ends its session instead, never a GATT close: the pipe
// owns the link, and that console starts a fresh signed-out session.
BleDisconnectResult bleDisconnectConnection(uint16_t connId);
// Close every app client (never a peer link); returns the number of close
// requests issued plus the phone link's console sessions ended.
int bleDisconnectAppClients();

// Console mirror / blesend: delivered as a directed notify (or the session's own
// encrypted frames) to every logged-in non-guest session; never characteristic-wide.
// Returns true when at least one session accepted it. blocking=false makes each send
// best-effort (used by the debug console mirror so it never stalls the debug task).
bool sendBLEResponse(const char* data, size_t len, bool blocking = true);
// Number of sessions sendBLEResponse would reach right now (the same
// logged-in, non-guest eligibility). The debug console mirror buffers only
// while this is non-zero; `outble` reports it.
int bleMirrorRecipientCount();

// Send to one session: the connection id and the session epoch captured when
// the command (or the subscription) was admitted. A stale result is dropped if
// the peer disconnected, logged out, or connId was reused by a new session.
// blocking=false makes the secure-channel send try-lock, exactly like the
// sendBLEResponse console-mirror path: a best-effort producer (the notification
// sink, called from the main loop) must never stall behind a paced multi-frame
// command result that currently holds the tx mutex. Command results keep the
// default and must not drop.
bool sendBLEResponseToSession(uint16_t connId,
                              TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);

// broadcastOutputCore passes each line routed to Bluetooth. While cmd_exec runs
// a command a Bluetooth session sent, the line goes to that session (batched,
// before the command's result) unless the console mirror already streams to it;
// any other caller is ignored.
void bleCommandOutputLine(const char* text, size_t len);

// Push one notification card to a specific authenticated session. The line is
// framed as `#NOTIF {json}\n` — see the wire-format contract at the definition
// in Bluetooth.cpp. Best-effort: returns false if the link is busy or gone.
bool blePushNotification(uint16_t connId, TransportSessionEpoch expectedEpoch,
                         const char* kindName, const char* level,
                         const char* title, const char* msg, uint16_t durMs);

// The session may receive confidential output (`fileread ... bin`, `otawrite
// begin`): it holds an established Secure Channel, or it is a phone-link
// console row (the pipe's authenticated encryption). epoch fences the check.
bool bleSessionConfidential(uint16_t connId, TransportSessionEpoch epoch);
// A raw binary body for one confidential session (`fileread ... bin`), fenced on
// the command's epoch: a BLOB chain on a phone-link console row, one binary
// Secure Channel message otherwise; never plaintext. cmd_exec, before the
// command's result.
bool bleSendBinaryToSession(uint16_t connId, TransportSessionEpoch epoch,
                            const uint8_t* data, size_t len);
#if ENABLE_BLE_PHONE_PIPE
// Holder bit for gBLEState: the pipe takes it at start (before its owner lock)
// and gives it back at stop or host teardown. The state is freed when no
// holder (server, client-host console, pipe) is left.
bool blePipeConsoleStateAcquire();
void blePipeConsoleStateRelease();
// Pipe task, owner mutex released: make a fresh, signed-out row for the
// console stream (false: no state or no free row/transport slot), or end it.
// End is idempotent and matches generation and stream.
bool blePipeConsoleBind(uint32_t generation, uint16_t streamId, uint16_t linkConnId);
void blePipeConsoleEnd(uint32_t generation, uint16_t streamId);
// Leaf snapshot of the bound row; any task, never blocks.
void blePipeConsoleIdentity(BlePipeConsoleIdentity* out);
// cmd_exec: one COMMAND (0x81) or BINARY (0x82) record admitted by the
// console driver, fenced on the epoch captured at its admission.
void blePipeConsoleInbound(uint16_t rowConnId, TransportSessionEpoch epoch, uint8_t type,
                           const uint8_t* bytes, size_t len);
// The calling task holds the BLE lifecycle lock (never blocks; false before
// the scheduler runs or the lock exists): a pipe-side write refuses then, as
// the lock is never held with the pipe's locks.
bool bleLifecycleHeldByCurrentTask();
#else
inline bool blePipeConsoleStateAcquire() { return false; }
inline void blePipeConsoleStateRelease() {}
inline bool blePipeConsoleBind(uint32_t, uint16_t, uint16_t) { return false; }
inline void blePipeConsoleEnd(uint32_t, uint16_t) {}
inline void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) { if (out) *out = {}; }
inline void blePipeConsoleInbound(uint16_t, TransportSessionEpoch, uint8_t, const uint8_t*, size_t) {}
inline bool bleLifecycleHeldByCurrentTask() { return false; }
#endif

// BLE auth/session maintenance
void bleClearConnectionByConnId(uint16_t connId);
void bleSessionTick();
bool bleHasAuthenticatedSession();
// Resolve the live authenticated identity for one connection. Used by binary
// application protocols that bypass the text-command authorization funnel but
// must re-check authorization on every frame.
bool bleGetAuthenticatedUser(uint16_t connId, String& outUser);
bool bleGetAuthenticatedSessionInfo(int authedIndex, uint16_t& outConnId, String& outUser);
int bleRevokeUserSessions(const String& username);
int bleRevokeAllSessions();
TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId);
bool bleSessionEpochMatches(uint16_t connId, TransportSessionEpoch epoch);
bool bleSessionEpochIsLive(TransportSessionEpoch epoch);
// Rotate every unauthenticated connection incarnation after a live auth-policy
// change; named logged-in sessions remain intact.
void bleAuthPolicyChanged();

// Data streaming pipeline API
bool blePushSensorData(const char* jsonData, size_t len);
bool blePushSystemStatus(const char* jsonData, size_t len);
bool blePushEvent(BLEEventType eventType, const char* message, const char* details = nullptr);

// Stream control
void bleEnableStream(uint8_t streamFlags);
void bleDisableStream(uint8_t streamFlags);
void bleSetStreamInterval(uint32_t sensorMs, uint32_t systemMs);
bool bleIsStreamEnabled(uint8_t streamFlag);

// Auto-streaming (call from main loop)
void bleUpdateStreams();

// Runtime status. These predicates deliberately answer different questions:
// controller reports the local driver's state or the last acknowledged remote
// lifecycle state (Hosted has no live status RPC); host reports the local BLE
// host state, server reports ownership of the phone-facing GATT profile, and
// bleSubsystemActive() reports either logical
// application role (server or G2 client) initialized.
bool isBleControllerEnabled();
bool isBluedroidHostEnabled();
bool isBleServerInitialized();

// Exclusive application-role transition token. It is recursive for the
// owning task (server start may call G2 deinit, and host recovery calls G2
// deinit/init as one transaction) and rejects unrelated tasks immediately.
// BLE callbacks never acquire it; they observe the published state and
// finish/drop work through their normal lifecycle epochs.
enum class BleRoleTransition : uint8_t {
  IDLE = 0,
  SERVER_START,
  SERVER_STOP,
  G2_START,
  G2_STOP,
  PHONE_START,
  PHONE_STOP,
  RECOVERING,
  STACK_UP,     // bleStackUp(): the shared central stack comes up (BLE_CentralStack.cpp)
  STACK_DOWN,   // bleStackDown(): verified terminal teardown of the shared stack
};
bool bleRoleTransitionBegin(BleRoleTransition state);
void bleRoleTransitionEnd();
BleRoleTransition bleRoleTransitionState();
bool bleRoleTransitionHeldByCurrentTask();
// Set only after a destructive host/controller transition fails to reach a
// verified terminal state. Normal init paths must fail closed until reboot;
// the recovery transaction clears it only after a fully verified re-init.
bool bleStackLifecycleFaulted();
// Main-loop recovery after bounded phone retirement fails; never from a BLE callback.
void bleRecoverStuckPhone();
void bleStackSetLifecycleFault(bool faulted);
const char* getBLEStateString();
// Shared bleinfo schema for CLI and web. Copies live connection fields into
// caller-owned JSON under the lifecycle lock; settings retain their existing
// read contract. False means unavailable: discard the partial object. Callers
// also check their document's overflowed() before serializing.
bool buildBLEInfoJson(JsonObject out);
// Recent BLE message history, newest first (used by the OLED status screen in
// OLED_Mode_Bluetooth.cpp). Fills out[] with up to maxLines entries; returns count.
int bleGetRecentMessages(const char* out[], int maxLines);

// Mode (server vs. G2 client) — reads gSettings.bleMode
const char* getBleModeString();

// Aggregate subsystem status — true when EITHER server OR G2 client is initialized.
// Mirrors the ESPNow mesh-or-direct pattern so the dashboard reports BLE as
// running regardless of which mode is active.
bool bleSubsystemActive();
const char* bleSubsystemStateString();
// What the Bluetooth host is doing, for the status surfaces (`blestatus`'s
// "Bluetooth:" line and JSON `host`, /api/ble/status `host`): "on" = a role
// runs (the console server, or a device on the client stack); "idle" = the
// client stack is up with no device open; "off" = verified stopped; "fault" =
// a teardown did not complete (reboot required); "parked" = a host generation
// is still up with no owner (teardown pending). `roleRunning` is the link
// status's running role != Off, read by the caller under the lifecycle guard
// (the owner flags are not re-read here, where the guard may be held).
const char* bleHostStateWord(bool roleRunning);

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

extern const CommandEntry bluetoothCommands[];
extern const size_t bluetoothCommandsCount;
#if ENABLE_BLE_PHONE_PIPE
// `phone secret <passphrase|clear>`, a row of BLE_PhonePipe.cpp's phoneCommands.
const char* phoneSecretCommand(const String& argsInput);
// The `phone secret` steps (stop, key, store with the device-made mark, clear
// the enrollment, persist), shared with `phone pair new`. nullptr when done.
const char* phonePipeReplaceSecret(const char* value, size_t len, bool generated, bool* replaced);
#endif

// =============================================================================
// SETTINGS ENTRIES
// =============================================================================

struct SettingEntry;
struct SettingsModule;
extern const SettingEntry bluetoothSettingsEntries[];
extern const size_t bluetoothSettingsCount;

#else // !ENABLE_BLUETOOTH

// Stub declarations when Bluetooth is disabled
inline bool initBluetooth() { return false; }
inline bool deinitBluetooth() { return true; }
inline void bleSecurityBootNotice() {}
inline bool startBLEAdvertising() { return false; }
inline void stopBLEAdvertising() {}
inline bool bleAdvertising() { return false; }
inline bool bleServerUp() { return false; }
struct BleConsoleView {
  bool up = false, advertising = false;
  uint8_t clients = 0;
  bool signedIn = false;
  bool registered = false, attachable = false, attached = false, secret = false;
};
inline BleConsoleView bleConsoleView() { return {}; }
inline hw1console::Mode bleConsoleMode() { return hw1console::Mode::Off; }
inline bool bleConsoleRegisterOnClientHost() { return false; }
inline void bleConsoleHostTeardownBegin() {}
inline void bleConsoleHostTeardownEnd(bool) {}
inline bool isBLEConnected() { return false; }
inline BleDisconnectResult bleDisconnectConnection(uint16_t) { return BleDisconnectResult::UNAVAILABLE; }
inline int bleDisconnectAppClients() { return 0; }
inline bool sendBLEResponse(const char*, size_t, bool = true) { return false; }
inline int bleMirrorRecipientCount() { return 0; }
inline bool sendBLEResponseToSession(uint16_t, TransportSessionEpoch,
                                     const char*, size_t, bool = true) { return false; }
inline void bleCommandOutputLine(const char*, size_t) {}
inline bool blePushNotification(uint16_t, TransportSessionEpoch, const char*,
                                const char*, const char*, const char*,
                                uint16_t) { return false; }
inline bool bleSessionConfidential(uint16_t, TransportSessionEpoch) { return false; }
inline bool bleSendBinaryToSession(uint16_t, TransportSessionEpoch, const uint8_t*, size_t) {
  return false;
}
inline bool blePipeConsoleStateAcquire() { return false; }
inline void blePipeConsoleStateRelease() {}
inline bool blePipeConsoleBind(uint32_t, uint16_t, uint16_t) { return false; }
inline void blePipeConsoleEnd(uint32_t, uint16_t) {}
inline void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) { if (out) *out = {}; }
inline void blePipeConsoleInbound(uint16_t, TransportSessionEpoch, uint8_t, const uint8_t*, size_t) {}
inline bool bleLifecycleHeldByCurrentTask() { return false; }
inline void bleClearConnectionByConnId(uint16_t) {}
inline void bleSessionTick() {}
inline bool bleHasAuthenticatedSession() { return false; }
inline bool bleGetAuthenticatedUser(uint16_t, String&) { return false; }
inline bool bleGetAuthenticatedSessionInfo(int, uint16_t&, String&) { return false; }
inline int bleRevokeUserSessions(const String&) { return 0; }
inline int bleRevokeAllSessions() { return 0; }
inline TransportSessionEpoch bleSessionEpochForConnection(uint16_t) { return 0; }
inline bool bleSessionEpochMatches(uint16_t, TransportSessionEpoch) { return false; }
inline bool bleSessionEpochIsLive(TransportSessionEpoch) { return false; }
inline void bleAuthPolicyChanged() {}
inline bool isBleControllerEnabled() { return false; }
inline bool isBluedroidHostEnabled() { return false; }
inline bool isBleServerInitialized() { return false; }
enum class BleRoleTransition : uint8_t {
  IDLE = 0, SERVER_START, SERVER_STOP, G2_START, G2_STOP, PHONE_START, PHONE_STOP, RECOVERING,
  STACK_UP, STACK_DOWN,
};
inline bool bleRoleTransitionBegin(BleRoleTransition) { return false; }
inline void bleRoleTransitionEnd() {}
inline BleRoleTransition bleRoleTransitionState() { return BleRoleTransition::IDLE; }
inline bool bleRoleTransitionHeldByCurrentTask() { return false; }
inline bool bleStackLifecycleFaulted() { return false; }
inline void bleStackSetLifecycleFault(bool) {}
inline const char* getBLEStateString() { return "disabled"; }
inline bool buildBLEInfoJson(JsonObject) { return false; }
inline const char* getBleModeString() { return "disabled"; }
inline bool bleSubsystemActive() { return false; }
inline const char* bleSubsystemStateString() { return "disabled"; }
inline const char* bleHostStateWord(bool) { return "off"; }

// Streaming API stubs
inline bool blePushSensorData(const char*, size_t) { return false; }
inline bool blePushSystemStatus(const char*, size_t) { return false; }
inline bool blePushEvent(int, const char*, const char* = nullptr) { return false; }
inline void bleEnableStream(uint8_t) {}
inline void bleDisableStream(uint8_t) {}
inline void bleSetStreamInterval(uint32_t, uint32_t) {}
inline bool bleIsStreamEnabled(uint8_t) { return false; }
inline void bleUpdateStreams() {}

// OLED menu stubs
inline void bluetoothMenuUp() {}
inline void bluetoothMenuDown() {}
inline void executeBluetoothAction() {}
inline void bluetoothMenuBack() {}
inline void displayBluetoothPage() {}
inline bool handleBluetoothInput(uint16_t, uint16_t) { return false; }

// Global state stub
extern bool gBluetoothShowingStatus;

#endif // ENABLE_BLUETOOTH

#endif // BLUETOOTH_H
