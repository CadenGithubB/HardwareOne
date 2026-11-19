// Host harness for test_espnow_bond_partner_change.py.
//
// The runner pastes the production BondSyncRequestType and CapabilitySummary
// (System_ESPNow.h) at INSERT_TYPES and, at INSERT_PRODUCTION, from
// System_ESPNow.cpp: the bond frame hold (when present), isBondPeerMac,
// isBondModeOnline, isBondSynced, resetBondSync, the CAP_REQ / CAP_RESP /
// STATUS_REQ handlers, bondWorkerAskForCap and bondStartHandshakeNow (when
// present), cmd_bond_connect, cmd_bond_disconnect and the espnowbondmodeenabled
// / espnowbondpeermac commands.
//
// Doubles: settings writes, every use of gEspNow and every INFO log line (each
// lets "espnow_task" run: espnow_task is above cmd_exec on the same core and
// can take the CPU between any two statements of a command; a use of gEspNow
// right after the frame hold ends is where the bench's window was), the
// receive gate (bond mode on and the production
// isBondPeerMac, which is what the receive table asks for bond opcodes), the
// worker's tick stages 9b (answer CAP_REQ, mark bondCapSent) and 9e (answer
// SETTINGS_REQ, mark bondSettingsSent), and a model of the master's sync tick.
//
// At INSERT_TICKS the runner pastes the bond parts of processMeshHeartbeats as
// hbTickProduction (heartbeat send and timeout) and syncTickProduction (the
// sync tick to the streaming setup), with doubles below for what they call;
// cmd_espnow_unpair is pasted with the commands, with doubles for its registry
// and file steps that record their order. The sync tick slice ends with the
// status steps 9j, 9j2 and 9k; runDeferredBondFileSend runs the jobs the tick
// queued (gCmdExecQ) with sender doubles; gTickWait runs a command where a
// step of the tick waits.
//
// When the source has the bond agreement (BOND_REQUEST / BOND_NOTICE,
// HW1_HAVE_AGREE), System_ESPNow_BondAgreeCore.h is included as is and the
// runner also pastes the wire payloads and reason codes, the agreement glue
// with its two handlers, v4h_bond_heartbeat and bondNotifySessionEstablished.
// The doubles record every request and notice sent (gAgreeFrames), every
// event-log line (gEventLines) and every auth-log record (gAuthLines). The
// agreement's lock is modelled too: espnow_task does not run while cmd_exec
// holds it, and does run during the auth-log write, before an event is posted
// and while a command's own send is on the radio. Everything above the "Cases"
// line is also the base of test_espnow_bond_agreement.py, which appends its
// own cases.
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "System_CommandReply.h"
#include "System_MacText.h"
#include "System_DeviceName.h"  // the device-name rule (capability receive)
#if HW1_HAVE_CAPS
#include "System_ESPNow_SessionCapsCore.h"   // the session capability word (copied beside this unit)
#endif
#if HW1_HAVE_AGREE
#include "System_ESPNow_BondAgreeCore.h"
#endif

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++gChecks;                                                        \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

#define ENABLE_BONDED_MODE 1
#define ENABLE_HTTP_SERVER 1   // the auth-log record in bondNotifySessionEstablished
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
template <class... A> static void sink(A&&...) {}
static void preempt();
#define DEBUG_ESPNOW_MESHF(...) sink(__VA_ARGS__)
#define INFO_ESPNOWF(...) do { preempt(); sink(__VA_ARGS__); } while (0)
[[maybe_unused]] static int gPrints = 0;
#define BROADCAST_PRINTF(...) do { ++gPrints; sink(__VA_ARGS__); } while (0)

// ---- String double ------------------------------------------------------------
class String {
 public:
  String() = default;
  String(const char* s) : t_(s ? s : "") {}
  String(const std::string& s) : t_(s) {}
  String& operator=(const char* s) { t_ = s ? s : ""; return *this; }
  size_t length() const { return t_.size(); }
  const char* c_str() const { return t_.c_str(); }
  void trim() {
    const auto first = t_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { t_.clear(); return; }
    t_ = t_.substr(first, t_.find_last_not_of(" \t\r\n") - first + 1);
  }
  bool operator==(const String& o) const { return t_ == o.t_; }
  bool operator!=(const String& o) const { return t_ != o.t_; }
 private:
  std::string t_;
};

// ---- Settings and their writes -------------------------------------------------
struct Settings {
  bool bondModeEnabled = false;
  uint8_t bondRole = 0;
  bool bondRoleChosen = false;
  String bondPeerMac;
  String espnowDeviceName{"this-device"};
};
static Settings gSettings;
#if HW1_HAVE_AGREE
// bondAutoRole (System_ESPNow.cpp, beside bondrole): this device's role was
// automatic and the partner's user chose the same one. The production one
// runs bondChangeRole (the role-dependent handshake reset); here the call and
// the new role are recorded.
static int gAutoRoleCalls = 0;
static int gAutoRoleTo = -1;
static void bondAutoRole(const uint8_t*, uint8_t newRole) {
  if (gSettings.bondRoleChosen || newRole == gSettings.bondRole) return;  // as the production guard
  ++gAutoRoleCalls;
  gAutoRoleTo = newRole;
  gSettings.bondRole = newRole;
}
#endif

// What espnow_task does when it gets the CPU in the middle of a command. What
// it runs (the handlers) does not preempt itself, and it does not get in
// while the command holds a spinlock (gLockDepth: portENTER_CRITICAL).
static std::function<void()> gPreempt;
static bool gInPreempt = false;
static int gLockDepth = 0;
static int gPreemptPoints = 0;    // points passed since a case last set this to 0
static void preempt() {
  if (gInPreempt || gLockDepth > 0) return;
  ++gPreemptPoints;
  if (!gPreempt) return;
  gInPreempt = true;
  gPreempt();
  gInPreempt = false;
}
// Set by the first settings change of a command: the partner change began.
static bool gSettingsTouched = false;

enum SettingType { SETTING_INT, SETTING_U8, SETTING_U16, SETTING_U32, SETTING_FLOAT, SETTING_BOOL, SETTING_STRING };
struct SettingEntry { const char* jsonKey; SettingType type; void* valuePtr; };
static SettingEntry gBondEntries[] = {
  {"bondModeEnabled", SETTING_BOOL, &gSettings.bondModeEnabled},
  {"bondPeerMac", SETTING_STRING, &gSettings.bondPeerMac},
};
static const SettingEntry* findEspNowEntry(const char* key) {
  for (auto& e : gBondEntries) {
    if (std::strcmp(e.jsonKey, key) == 0) return &e;
  }
  return nullptr;
}

template <typename T>
static void setSetting(T& field, const T& value, const SettingEntry* = nullptr) {
  if (field != value) { field = value; gSettingsTouched = true; preempt(); }
}
static void setSetting(String& field, const String& value, const SettingEntry* = nullptr) {
  if (field != value) { field = value; gSettingsTouched = true; preempt(); }
}

// bondconnect tells the firmware which of its staged settings changed.
static std::vector<const void*> gSettingsNotified;
static void notifySettingChanged(const void* field, const SettingEntry* = nullptr) {
  gSettingsNotified.push_back(field);
}

// bondconnect stages its settings in RAM and asks for one write of the file.
static bool requestSettingsPersist() {
  gSettingsTouched = true;
  preempt();
  return true;
}

static int parseBoolArg(const String& arg) {
  const std::string v = arg.c_str();
  if (v == "1" || v == "on" || v == "true") return 1;
  if (v == "0" || v == "off" || v == "false") return 0;
  return -1;
}

// handleSettingCommand: the production parse-and-set for the two types used here.
static int gSettingCommands = 0;
static const char* handleSettingCommand(const SettingEntry* entry, const String& args) {
  ++gSettingCommands;
  if (!entry) return "Error: Setting unavailable";
  String v = args;
  v.trim();
  if (v.length() == 0) return "query";
  if (entry->type == SETTING_BOOL) {
    const int parsed = parseBoolArg(v);
    if (parsed < 0) return "Error: Expected on/off";
    setSetting(*(bool*)entry->valuePtr, parsed != 0, entry);
  } else {
    setSetting(*(String*)entry->valuePtr, v, entry);
  }
  return "[Settings] Configuration updated";
}

// ---- ESP-NOW state double ----------------------------------------------------
// INSERT_TYPES

struct EspNowState {
  bool initialized = true;
  uint32_t bondPeerBootCounter = 0;
  uint32_t bondHeartbeatsSent = 0;
  uint32_t bondHeartbeatsReceived = 0;
  uint32_t bondLocalSettingsHash = 0;
  bool bondNeedsStreamingSetup = false;
  BondSyncRequestType bondSyncInFlight = BOND_SYNC_NONE;
  uint8_t bondSyncRetryCount = 0;
  uint32_t bondSyncLastAttemptMs = 0;
  bool bondCapSent = false;
  bool bondManifestReceived = false;
  bool bondSettingsReceived = false;
  bool bondSchemaReceived = false;
  bool bondSettingsSent = false;
  bool lastRemoteCapValid = false;
  CapabilitySummary lastRemoteCap = {};
  uint32_t lastRemoteCapTime = 0;
  bool bondNeedsCapabilityRequest = false;
  bool bondNeedsCapabilityResponse = false;
  bool bondReceivedCapability = false;
  bool bondNeedsManifestResponse = false;
  bool bondNeedsSettingsResponse = false;
  bool bondNeedsSchemaResponse = false;
  bool bondPeerStatusValid = false;
  bool bondNeedsProactiveStatus = false;
  bool bondStatusReqSentOnce = false;
  bool bondNeedsStatusResponse = false;
  unsigned long bondLastStatusReqMs = 0;
  bool bondPeerOnline = false;
  uint32_t lastBondHeartbeatReceivedMs = 0;
  uint8_t bondPendingResponseMac[6] = {};
  // v4h_bond_heartbeat
  uint32_t bondPeerSettingsHash = 0;
  uint32_t bondPeerUptime = 0;
  int8_t bondRssiLast = -100;
  int8_t bondRssiAvg = -100;
  uint32_t bondLastRssiUpdateMs = 0;
};
static EspNowState gState;
// Every use of gEspNow in a command is a point where espnow_task may run.
struct EspNowRef {
  EspNowState* operator->() const { preempt(); return &gState; }
  explicit operator bool() const { preempt(); return true; }
};
static EspNowRef gEspNow;

static constexpr uint8_t BOND_ROLE_WORKER = 0;
static constexpr uint8_t BOND_ROLE_MASTER = 1;
inline bool isBondMaster() { return gSettings.bondRole == BOND_ROLE_MASTER; }
inline bool isBondWorker() { return gSettings.bondRole == BOND_ROLE_WORKER; }

struct RxCtrl { int8_t rssi; };
struct esp_now_recv_info { const uint8_t* src_addr; const RxCtrl* rx_ctrl = nullptr; };
struct V4RxCtx {
  const esp_now_recv_info* recv_info;
  const uint8_t* payload;
  uint16_t payloadLen;
  const char* deviceName;
  bool isPaired = true;
  // As System_ESPNow_RxCtx.h: copy what the frame has, zero the rest.
  template <class T>
  void read(T& out) const {
    std::memset(&out, 0, sizeof(T));
    const size_t n = payloadLen < sizeof(T) ? payloadLen : sizeof(T);
    if (payload && n) std::memcpy(&out, payload, n);
  }
};

// ---- Platform doubles ----------------------------------------------------------
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }
static uint8_t gSelfMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x00};
static void espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gSelfMac, 6); }
static bool parseMacAddress(const String& text, uint8_t mac[6]) { return macParse(text.c_str(), mac); }
static bool resolveDeviceNameOrMac(const String& text, uint8_t mac[6]) { return macParse(text.c_str(), mac); }
static String macToDisplayStr(const uint8_t* mac) {
  char b[18];
  macToDisplay(mac, b, sizeof b);
  return String(b);
}
static String getEspNowDeviceName(const uint8_t*) { return String(); }
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }

// What the doubles record: whether a partner change held bond frames then.
#if HW1_HAVE_HOLD
static bool bondPartnerChanging();
#else
static bool bondPartnerChanging() { return false; }
#endif
static constexpr uint8_t SYSEVT_BOND_OFFLINE = 1;
static constexpr uint8_t SYSEVT_PEER_UNPAIRED = 2;
[[maybe_unused]] static constexpr uint8_t SYSEVT_BOND_ONLINE = 3;   // bondNotifySessionEstablished, v4h_bond_heartbeat
struct PostedEvent { uint8_t type; std::string who; std::string detail; bool held; };
static std::vector<PostedEvent> gPosted;
static void systemEventPost(uint8_t type, const char* who = nullptr, const char* detail = nullptr) {
  preempt();   // the state the event reports is set; the event is not posted yet
  gPosted.push_back({type, who ? who : "", detail ? detail : "", bondPartnerChanging()});
}
static uint32_t gNextMsgId = 100;
static uint32_t generateMessageId() { return ++gNextMsgId; }
static bool sessionIsASide(const uint8_t a[6], const uint8_t b[6]) { return std::memcmp(a, b, 6) < 0; }

// Handshake back-off doubles (System_ESPNow_Handlers_Crypto.cpp is tested by
// test_espnow_handshake_backoff).
static int gBackoffClears = 0;
static int gAutoStarts = 0;
static int gManualStarts = 0;
[[maybe_unused]] static void espnowHandshakeBackoffClear(const uint8_t*) { ++gBackoffClears; }
[[maybe_unused]] static bool espnowHandshakeAutoStart(const uint8_t*) { ++gAutoStarts; return true; }
[[maybe_unused]] static bool espnowHandshakeStartNow(const uint8_t*) { ++gManualStarts; return true; }

// ---- The two ends of the link ----------------------------------------------------
static const uint8_t kPartnerA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t kPartnerB[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
static bool gSessionUp = true;
static const uint8_t* gSessionDownFor = nullptr;   // this peer's session is not ACTIVE
[[maybe_unused]] static bool bondSessionActiveWith(const uint8_t* mac) {
  return gSessionUp && !(gSessionDownFor && std::memcmp(mac, gSessionDownFor, 6) == 0);
}
// The capability word a peer's session handshake carried (production:
// System_ESPNow.cpp). Unknown (false) unless a case sets gCapsFor: the older
// cases model peers whose word does not gate anything.
static const uint8_t* gCapsFor = nullptr;
static uint32_t gCapsWord = 0;
static bool gCapsConfirmed = true;
[[maybe_unused]] static bool espnowPeerSessionCaps(const uint8_t* mac, uint32_t* caps, bool* confirmed = nullptr) {
  if (!gCapsFor || !mac || std::memcmp(mac, gCapsFor, 6) != 0 || !bondSessionActiveWith(mac)) return false;
  if (caps) *caps = gCapsWord;
  if (confirmed) *confirmed = gCapsConfirmed;
  return true;
}

enum RxKind { RX_CAP_REQ, RX_CAP_RESP, RX_STATUS_REQ, RX_SETTINGS_REQ };

// Model of the master (the partner): what its sync tick sends and when.
struct MasterModel {
  const uint8_t* mac = kPartnerA;
  bool hasWorkerCap = false;    // got our CAP_RESP
  bool sentOwnCap = false;      // its bondCapSent: the reciprocal goes out once
  bool hasWorkerSettings = false;
  uint32_t lastStatusReqMs = 0;
  bool synced() const { return hasWorkerCap && hasWorkerSettings; }
};
static MasterModel gMaster;
static int gAccepted = 0;
static int gDropped = 0;

static bool deliver(const uint8_t* src, RxKind kind);

// The worker's own sends to the master (the production bondSendEncryptedAsync
// is replaced: the frame reaches the master model at once).
static void masterOnWorkerCapResp();
static void masterOnWorkerCapReq();
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_CAP_REQ = 1;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_HEARTBEAT = 2;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_CAP_RESP = 3;
static constexpr uint8_t ESPNOW_V4_TYPE_MANIFEST_REQ = 4;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_STATUS_REQ = 6;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_STATUS_RESP = 7;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_REQUEST = 8;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_NOTICE = 9;
static constexpr uint8_t kSentSettingsReq = 5;      // requestBondSettings
static constexpr uint8_t kSentFileBase = 100;       // submitBondFileSend + kind
static constexpr uint8_t kSentStreamCtrl = 200;     // sendBondStreamCtrl
[[maybe_unused]] static constexpr uint16_t ESPNOW_V4_FLAG_ACK_REQ = 0x01;
static std::vector<uint8_t> gSentTypes;
// Every bond frame the device sends, to whom, and whether a hold was up.
struct SentFrame { uint8_t mac[6]; uint8_t type; bool held; };
static std::vector<SentFrame> gFrames;
static void noteFrame(const uint8_t* mac, uint8_t type) {
  SentFrame f{};
  if (mac) std::memcpy(f.mac, mac, 6);
  f.type = type;
  f.held = bondPartnerChanging();
  gFrames.push_back(f);
}
// Every BOND_REQUEST and BOND_NOTICE the device sends, with what it says
// (request: role, flags; notice: reason, role). `direct` marks the one sent
// from the command itself (v4_send_session_wrapped) instead of the send queue.
struct AgreeFrame { uint8_t mac[6]; uint8_t type; uint8_t b0; uint8_t b1; uint16_t seq; uint8_t b4; bool held; bool direct; };
static std::vector<AgreeFrame> gAgreeFrames;
static void noteAgreeFrame(const uint8_t* mac, uint8_t type, const uint8_t* payload, uint16_t len, bool direct) {
  if (type != ESPNOW_V4_TYPE_BOND_REQUEST && type != ESPNOW_V4_TYPE_BOND_NOTICE) return;
  AgreeFrame f{};
  std::memcpy(f.mac, mac, 6);
  f.type = type;
  if (payload && len >= 4) {
    f.b0 = payload[0];
    f.b1 = payload[1];
    std::memcpy(&f.seq, payload + 2, 2);
  }
  if (payload && len >= 5) f.b4 = payload[4];  // a notice's flags
  f.held = bondPartnerChanging();
  f.direct = direct;
  gAgreeFrames.push_back(f);
}
static bool gTxQueueFull = false;   // the send queue refuses the job
[[maybe_unused]] static bool bondSendEncryptedAsync(const uint8_t* mac, uint8_t type, uint16_t, uint32_t,
                                                    const uint8_t* payload, uint16_t len) {
  if (gTxQueueFull) return false;
  noteFrame(mac, type);
  noteAgreeFrame(mac, type, payload, len, false);
  gSentTypes.push_back(type);
  if (type == ESPNOW_V4_TYPE_BOND_CAP_REQ) masterOnWorkerCapReq();
  return true;
}
// The send a command makes itself: it goes out now over an ACTIVE session or
// not at all (no queue, no handshake). Recorded among the steps of a command
// (gSteps), with whether bond mode still named a partner then.
static void step(const char* name);
[[maybe_unused]] static bool v4_send_session_wrapped(const uint8_t dst[6], uint8_t type, uint16_t, uint32_t,
                                                     const uint8_t* payload, uint16_t len, uint8_t,
                                                     char* err, size_t errLen) {
  if (!bondSessionActiveWith(dst)) {
    if (err && errLen) std::snprintf(err, errLen, "no active session");
    return false;
  }
  noteFrame(dst, type);
  noteAgreeFrame(dst, type, payload, len, true);
  step("directSend");
  preempt();   // the send waits for the radio; the command has not gone on yet
  return true;
}

// ---- Doubles for the bond agreement glue ---------------------------------------
struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(lock) ((void)(lock), ++gLockDepth)
#define portEXIT_CRITICAL(lock) ((void)(lock), --gLockDepth)
[[maybe_unused]] static bool isBondSessionTokenValid() { return gSessionUp; }
// Sends whose acknowledgement is watched (an unanswered run drops the session).
static std::vector<uint32_t> gWatchedSends;
[[maybe_unused]] static void sendStatusRegister(uint32_t msgId, const uint8_t*) { gWatchedSends.push_back(msgId); }
[[maybe_unused]] static void v4_dedup_flush_origin(const uint8_t*) {}
// The partner's boot counter recorded for the peer-restart rule
// (espnow_peer_restart_tests runs it).
[[maybe_unused]] static void sessionPeerBootSeen(const uint8_t*, uint32_t) {}
// The auth log: the reason text of every record (bondNotifySessionEstablished
// writes one per bond session).
static std::vector<std::string> gAuthLines;
static bool gInAuthWrite = false;
[[maybe_unused]] static void logAuthAttempt(bool, const char*, const String&, const String&, const char* reason) {
  // A blocking file append: espnow_task runs meanwhile.
  gInAuthWrite = true;
  preempt();
  gInAuthWrite = false;
  gAuthLines.push_back(reason ? reason : "");
}

static void resetBondSync();   // declared ahead of its users, as in the source

// ---- Doubles for cmd_espnow_unpair: each step is recorded in order -------------
#define EXT_RAM_BSS_ATTR
class CommandArgs {
 public:
  explicit CommandArgs(const String& s) : s_(s) { s_.trim(); }
  bool hasMinArgs(int n) const { return n <= 0 || (n == 1 && s_.length() > 0); }
  String arg(int) const { return s_; }
 private:
  String s_;
};
static std::vector<std::string> gSteps;
static void step(const char* name) {
  // The bond state each step sees: "on" while bond mode still names a partner.
  std::string s = name;
  if (gSettings.bondModeEnabled || gSettings.bondPeerMac.length() > 0) s += "/bond";
  gSteps.push_back(s);
}
static size_t stepAt(const char* name) {
  for (size_t i = 0; i < gSteps.size(); ++i) {
    if (gSteps[i].rfind(name, 0) == 0) return i;
  }
  return SIZE_MAX;
}
static bool stepSawBond(const char* name) {
  const size_t i = stepAt(name);
  return i != SIZE_MAX && gSteps[i].find("/bond") != std::string::npos;
}
static bool gDeleteOk = true;
[[maybe_unused]] static bool espnowDeletePeerRuntime(const uint8_t*) { step("deleteRuntime"); return gDeleteOk; }
[[maybe_unused]] static void removeEspNowDevice(const uint8_t*) { step("removeDevice"); }
[[maybe_unused]] static void espnowKeyExCancel(const uint8_t*) { step("keyExCancel"); }
[[maybe_unused]] static uint8_t pendingFrameDropForPeer(const uint8_t*) { step("pendingDrop"); return 0; }
[[maybe_unused]] static bool espnowForgetPeerCrypto(const uint8_t*, bool* removedAny = nullptr) {
  step("forgetCrypto");
  if (removedAny) *removedAny = true;
  return true;
}
[[maybe_unused]] static bool manifestRefRead(const uint8_t*, char hex[33]) {
  step("manifestRefRead");
  hex[0] = '\0';
  return false;
}
struct PeerFilesRemoved { unsigned files; };
[[maybe_unused]] static bool peerFilesRemove(const uint8_t*, PeerFilesRemoved&) { step("peerFilesRemove"); return true; }
[[maybe_unused]] static uint32_t manifestDropIfUnused(const char*) { return 0; }
[[maybe_unused]] static int peerCacheDir(const uint8_t*, char* out, size_t n) {
  return std::snprintf(out, n, "/system/espnow/peers/x");
}
static bool filesystemReady = true;
namespace VFS {
enum class Scopes { ESPNOW_RECEIVED };
struct Auth {};
[[maybe_unused]] static Auth systemAuth(Scopes, const char*) { return {}; }
[[maybe_unused]] static bool existsGuarded(const char*, Auth) { return false; }
}  // namespace VFS
[[maybe_unused]] static void formatUnpairFilesLine(char* out, size_t outLen, bool, const PeerFilesRemoved&, uint32_t,
                                                   const char*, const char*) {
  std::snprintf(out, outLen, "Removed: crypto identity.");
}
[[maybe_unused]] static void meshPeerSlotsRelease(const uint8_t*) { step("slotsRelease"); }
[[maybe_unused]] static bool saveEspNowDevices() { return true; }

// ---- Doubles for the bond parts of processMeshHeartbeats ------------------------
static uint32_t gLastBondHeartbeatSentMs = 0;
static uint32_t gBondHeartbeatSeqNum = 0;
static uint32_t gBootCounter = 7;
struct V4PayloadBondHeartbeat {
  uint8_t role; int8_t rssi; uint8_t reserved[2];
  uint32_t uptimeSec, freeHeap, seqNum, bootCounter, settingsHash;
};
namespace SelfDevice { [[maybe_unused]] static uint32_t uptimeSeconds() { return gNowMs / 1000; } }
struct EspClass { uint32_t getFreeHeap() const { return 100000; } };
[[maybe_unused]] static EspClass ESP;
[[maybe_unused]] static constexpr int ESP_OK = 0;
// System_ESPNow.cpp's joined-only AP lookup (the heartbeats' RSSI).
[[maybe_unused]] static int8_t heartbeatStaRssi() { return -50; }
// The event log: every line, as "CATEGORY: text".
static std::vector<std::string> gEventLines;
[[maybe_unused]] static void logSystemEvent(const char* category, const char* fmt, ...) {
  char text[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof text, fmt, args);
  va_end(args);
  gEventLines.push_back(std::string(category ? category : "") + ": " + text);
}
struct PeerIdentity { uint8_t meshId; };
static PeerIdentity gAnyIdentity{0};
[[maybe_unused]] static const PeerIdentity* peerIdentityFindByMac(const uint8_t*) { return &gAnyIdentity; }
struct SessionState { uint8_t mac[6]; };
static SessionState gSessionSlot{};
[[maybe_unused]] static SessionState* sessionFindByPeer(const uint8_t* mac, uint8_t) {
  std::memcpy(gSessionSlot.mac, mac, 6);
  return &gSessionSlot;
}
struct ClearedSession { uint8_t mac[6]; bool held; };
static std::vector<ClearedSession> gSessionClears;
[[maybe_unused]] static void sessionClear(SessionState* s) {
  ClearedSession c{};
  std::memcpy(c.mac, s->mac, 6);
  c.held = bondPartnerChanging();
  gSessionClears.push_back(c);
}
// A step of the tick that waits (the FS lock in the manifest lookup, the gap
// between stream commands): cmd_exec runs meanwhile. Runs once, then clears.
static std::function<void()> gTickWait;
static void tickWaits() {
  if (!gTickWait) return;
  auto fn = std::move(gTickWait);
  gTickWait = nullptr;
  fn();
}
// The manifest cache: every lookup is a hit and is recorded for that peer.
struct ManifestHit { uint8_t mac[6]; uint8_t hash0; bool held; };
static std::vector<ManifestHit> gManifestHits;
[[maybe_unused]] static bool manifestCacheHit(const uint8_t* mac, const uint8_t fwHash[16]) {
  tickWaits();
  ManifestHit h{};
  std::memcpy(h.mac, mac, 6);
  h.hash0 = fwHash[0];
  h.held = bondPartnerChanging();
  gManifestHits.push_back(h);
  return true;
}
// As production: the request is in flight from the tick's `now` on.
[[maybe_unused]] static void requestBondSettings(const uint8_t* mac, uint32_t nowMs) {
  gEspNow->bondSyncInFlight = BOND_SYNC_SETTINGS;
  gEspNow->bondSyncLastAttemptMs = nowMs;
  noteFrame(mac, kSentSettingsReq);
}
[[maybe_unused]] static bool espnowInboundFileActiveFrom(const uint8_t*) { return false; }
struct PostSync { uint8_t mac[6]; bool held; };
static std::vector<PostSync> gPostSync;
[[maybe_unused]] static void firePostSyncSideEffects(const uint8_t mac[6]) {
  PostSync p{};
  std::memcpy(p.mac, mac, 6);
  p.held = bondPartnerChanging();
  gPostSync.push_back(p);
}
#define MAC_STR(m) ((void)(m), "mac")
[[maybe_unused]] static void buildCapabilitySummary(CapabilitySummary& c) { c = CapabilitySummary{}; }
[[maybe_unused]] static bool bondSendReadyOrDeferred(const uint8_t*) { return true; }
enum BondFileKind : uint8_t { BOND_FILE_KIND_MANIFEST = 0, BOND_FILE_KIND_SETTINGS = 1, BOND_FILE_KIND_SCHEMA = 2 };
// cmd_exec's queue: submitBondFileSend puts the job there, runQueuedBondFiles
// runs the production runDeferredBondFileSend on each, later.
struct BondFileSendWork { uint8_t destMac[6]; uint8_t kind; };
static std::vector<BondFileSendWork*> gCmdExecQ;
[[maybe_unused]] static bool submitBondFileSend(const uint8_t destMac[6], uint8_t kind) {
  noteFrame(destMac, (uint8_t)(kSentFileBase + kind));
  auto* w = (BondFileSendWork*)std::malloc(sizeof(BondFileSendWork));
  std::memcpy(w->destMac, destMac, 6);
  w->kind = kind;
  gCmdExecQ.push_back(w);
  return true;
}
// The senders the job calls, with their production entry checks: settings
// and schema return when the partner is offline, the manifest does not. A
// send goes through sendFileToMac, which key-exchanges a peer with no
// identity (what an unpair removed).
#define SYSTEM_IDENTITY_SCOPE(name) sink(name)
struct FileSend { uint8_t mac[6]; uint8_t kind; };
static std::vector<FileSend> gFileSends;
static void noteFileSend(const uint8_t* mac, uint8_t kind) {
  FileSend f{};
  std::memcpy(f.mac, mac, 6);
  f.kind = kind;
  gFileSends.push_back(f);
}
[[maybe_unused]] static void sendBondManifest(const uint8_t* mac) { noteFileSend(mac, BOND_FILE_KIND_MANIFEST); }
[[maybe_unused]] static void sendBondSettings(const uint8_t* mac) {
  if (!gEspNow->bondPeerOnline) return;
  noteFileSend(mac, BOND_FILE_KIND_SETTINGS);
}
[[maybe_unused]] static void sendBondSchema(const uint8_t* mac) {
  if (!gEspNow->bondPeerOnline) return;
  noteFileSend(mac, BOND_FILE_KIND_SCHEMA);
}
// Status (processMeshHeartbeats 9j / 9j2).
struct BondPeerStatus { uint32_t sensorEnabledMask; uint32_t sensorConnectedMask; uint32_t freeHeap; };
[[maybe_unused]] static void buildLocalBondStatus(BondPeerStatus& st) { st = BondPeerStatus{1, 1, 100000}; }
enum RemoteSensorType { REMOTE_SENSOR_THERMAL = 0, REMOTE_SENSOR_TOF = 1, REMOTE_SENSOR_MAX = 2 };
static bool gStreamThermal = true;
[[maybe_unused]] static bool* bondStreamSetting(RemoteSensorType) { return &gStreamThermal; }
[[maybe_unused]] static const char* sensorTypeToString(RemoteSensorType) { return "thermal"; }
[[maybe_unused]] static void sendBondStreamCtrl(RemoteSensorType, bool) {
  uint8_t mac[6] = {};
  parseMacAddress(gSettings.bondPeerMac, mac);
  noteFrame(mac, kSentStreamCtrl);
}
#define pdMS_TO_TICKS(x) (x)
[[maybe_unused]] static void vTaskDelay(uint32_t) { tickWaits(); }

// INSERT_PRODUCTION

// INSERT_TICKS

// ---- Worker tick stages (processMeshHeartbeats 9b / 9e) ---------------------------
static void workerTick() {
  if (gEspNow->bondNeedsCapabilityResponse) {     // 9b: CAP_RESP to the requester
    gEspNow->bondNeedsCapabilityResponse = false;
    gEspNow->bondCapSent = true;
    masterOnWorkerCapResp();
  }
  if (gEspNow->bondNeedsSettingsResponse) {       // 9e: settings file to the requester
    gEspNow->bondNeedsSettingsResponse = false;
    gEspNow->bondSettingsSent = true;
    gMaster.hasWorkerSettings = true;
  }
#if HW1_HAVE_ASK
  bondWorkerAskForCap(gNowMs);
#endif
}

static bool deliver(const uint8_t* src, RxKind kind) {
  // The receive table: bond opcodes need bond mode and isBondPeerMac(src).
  if (!(gSettings.bondModeEnabled && isBondPeerMac(src))) {
    ++gDropped;
    return false;
  }
  esp_now_recv_info info{src};
  CapabilitySummary cap = {};
  V4RxCtx ctx{&info, nullptr, 0, "partner"};
  switch (kind) {
    case RX_CAP_REQ: v4h_bond_cap_req(ctx); break;
    case RX_CAP_RESP:
      ctx.payload = reinterpret_cast<const uint8_t*>(&cap);
      ctx.payloadLen = sizeof(cap);
      v4h_bond_cap_resp(ctx);
      break;
    case RX_STATUS_REQ: v4h_bond_status_req(ctx); break;
    case RX_SETTINGS_REQ:   // v4h_settings_req: defer to the tick
      std::memcpy(gEspNow->bondPendingResponseMac, src, 6);
      gEspNow->bondNeedsSettingsResponse = true;
      break;
  }
  ++gAccepted;
  return true;
}

static void masterOnWorkerCapResp() {
  gMaster.hasWorkerCap = true;
  if (!gMaster.sentOwnCap) {          // its reciprocal CAP_RESP, once
    gMaster.sentOwnCap = true;
    deliver(gMaster.mac, RX_CAP_RESP);
  }
}
static void masterOnWorkerCapReq() {
  gMaster.sentOwnCap = true;          // its 9b answers and marks bondCapSent
  deliver(gMaster.mac, RX_CAP_RESP);
}

// The master's sync tick once it sees the worker online: CAP_REQ until it has
// the worker's capability (in-flight retries), SETTINGS_REQ until it has the
// settings, then a status request every 30 s.
static void masterTick() {
  if (!gMaster.hasWorkerCap) {
    deliver(gMaster.mac, RX_CAP_REQ);
    workerTick();
    return;
  }
  if (!gMaster.hasWorkerSettings) {
    deliver(gMaster.mac, RX_SETTINGS_REQ);
    workerTick();
    return;
  }
  if (gMaster.lastStatusReqMs == 0 || gNowMs - gMaster.lastStatusReqMs >= 30000) {
    gMaster.lastStatusReqMs = gNowMs;
    deliver(gMaster.mac, RX_STATUS_REQ);
  }
}

static void runFor(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 100) {
    gNowMs += 100;
    workerTick();
    if (gNowMs % 1000 == 0) masterTick();
  }
}

static void resetWorld(const uint8_t self[6]) {
  gState = EspNowState();
  gSettings = Settings();
  gMaster = MasterModel();
  gPreempt = nullptr;
  gSettingsTouched = false;
  gSessionUp = true;
  gSessionDownFor = nullptr;
  gCapsFor = nullptr;
  gCapsWord = 0;
  gCapsConfirmed = true;
  gSentTypes.clear();
  gFrames.clear();
  gPosted.clear();
  gSessionClears.clear();
  gManifestHits.clear();
  gPostSync.clear();
  gSteps.clear();
  gDeleteOk = true;
  gTickWait = nullptr;
  for (auto* w : gCmdExecQ) std::free(w);
  gCmdExecQ.clear();
  gFileSends.clear();
  gAgreeFrames.clear();
#if HW1_HAVE_AGREE
  gAutoRoleCalls = 0;
  gAutoRoleTo = -1;
#endif
  gEventLines.clear();
  gWatchedSends.clear();
  gAuthLines.clear();
  gTxQueueFull = false;
  gLastBondHeartbeatSentMs = 0;
  gBackoffClears = gAutoStarts = gManualStarts = 0;
  std::memcpy(gSelfMac, self, 6);
#if HW1_HAVE_AGREE
  gBondAgree = bondagree::State();
  gBondOnlinePostPending = gBondOfflinePostOwed = false;
#endif
  gLockDepth = 0;
  gPreemptPoints = 0;
}

static const uint8_t kWorkerSelf[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x00};   // lower: worker, responder
static const uint8_t kMasterSelf[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x09};   // higher: master, initiator

// ---- Cases -----------------------------------------------------------------------

// Bench step c1: the master runs its exchange as soon as it hears the worker,
// which happens while the worker's bondconnect is still running.
static void caseBenchOrder() {
  resetWorld(kWorkerSelf);
  bool acceptedDuring = false;
  gPreempt = [&] {
    // The worker's heartbeat went out the moment bond mode was on; the
    // master's CAP_REQ (and its CAP_RESP after ours) come back at once.
    if (!gMaster.hasWorkerCap && deliver(gMaster.mac, RX_CAP_REQ)) {
      acceptedDuring = true;
      workerTick();
    }
  };
  gSettingsNotified.clear();
  const char* reply = cmd_bond_connect(String("02:11:22:33:44:01"));
  gPreempt = nullptr;
  CHECK(std::strncmp(reply, "Bond mode enabled", 17) == 0, "bondconnect reply: %s", reply);
  {
    auto notified = [](const void* field) {
      for (const void* f : gSettingsNotified) if (f == field) return true;
      return false;
    };
    CHECK(notified(&gSettings.bondPeerMac) && notified(&gSettings.bondModeEnabled),
          "bondconnect did not report the settings it changed (%u notified)",
          (unsigned)gSettingsNotified.size());
  }
  CHECK(gSettings.bondModeEnabled && isBondWorker(), "bondconnect: bond on, worker role");
  if (acceptedDuring) {
    CHECK(gEspNow->bondCapSent && gEspNow->lastRemoteCapValid,
          "frames accepted during bondconnect were wiped by its sync reset "
          "(capSent=%d cap=%d): the bench's stuck worker",
          (int)gEspNow->bondCapSent, (int)gEspNow->lastRemoteCapValid);
  }
  // The rest of the bench run: the bond comes up, the master syncs and polls.
  gEspNow->bondPeerOnline = true;
  runFor(90000);
  CHECK(gMaster.synced(), "the master reached SYNCED");
  CHECK(isBondSynced(), "the worker reached SYNCED within 90 s (cap=%d capSent=%d settTx=%d)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondCapSent, (int)gEspNow->bondSettingsSent);
}

// Switching partners with bondconnect while bond mode is on: the new
// partner's frames are not acted on and then wiped; the old partner's state
// does not carry over.
static void caseSwitchPartner() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->lastRemoteCapValid = true;   // known about A
  gEspNow->bondCapSent = true;
  gEspNow->bondSettingsSent = true;
  gEspNow->bondPeerOnline = true;
  gMaster.mac = kPartnerB;
  int aAccepted = 0;
  bool bAcceptedDuring = false;
  bool changing = false;
  gPreempt = [&] {
    changing = changing || gSettingsTouched;
    if (deliver(kPartnerA, RX_CAP_RESP) && gSettingsTouched) ++aAccepted;
    if (!gMaster.hasWorkerCap && deliver(kPartnerB, RX_CAP_REQ)) {
      bAcceptedDuring = true;
      workerTick();
    }
  };
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  CHECK(changing, "the switch changed settings");
  CHECK(aAccepted == 0, "the old partner's frames were accepted during the switch (%d)", aAccepted);
  if (bAcceptedDuring) {
    CHECK(gEspNow->bondCapSent && gEspNow->lastRemoteCapValid,
          "the new partner's frames accepted during the switch were wiped");
  } else {
    CHECK(!gEspNow->lastRemoteCapValid && !gEspNow->bondCapSent && !gEspNow->bondSettingsSent,
          "the old partner's sync state survived the switch");
  }
  CHECK(!deliver(kPartnerA, RX_CAP_REQ), "the old partner is still accepted after the switch");
  CHECK(deliver(kPartnerB, RX_CAP_REQ), "the new partner is not accepted after the switch");
}

// espnowbondpeermac: the same partner change through the setting command.
static void casePeerMacSetting() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondCapSent = true;
  gEspNow->bondSettingsSent = true;
  gEspNow->bondPeerOnline = true;
  int aAccepted = 0;
  gPreempt = [&] { if (deliver(kPartnerA, RX_CAP_RESP) && gSettingsTouched) ++aAccepted; };
  cmd_espnow_bondpeermac(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  CHECK(gSettings.bondPeerMac == String("02:11:22:33:44:02"), "espnowbondpeermac did not set the partner");
  CHECK(aAccepted == 0, "the old partner was accepted during espnowbondpeermac");
  CHECK(!gEspNow->lastRemoteCapValid && !gEspNow->bondCapSent && !gEspNow->bondSettingsSent,
        "espnowbondpeermac kept the old partner's sync state (cap=%d capSent=%d settTx=%d)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondCapSent, (int)gEspNow->bondSettingsSent);
  CHECK(!gEspNow->bondPeerOnline, "espnowbondpeermac kept the old partner online");
  CHECK(gBackoffClears >= 1, "espnowbondpeermac did not end the new partner's handshake back-off");
}

// espnowbondmodeenabled 1 from off: whatever was left from before is cleared,
// and a frame accepted while the command runs is not wiped.
static void caseModeEnabledSetting() {
  resetWorld(kWorkerSelf);
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->lastRemoteCapValid = true;      // left from an earlier bond
  gEspNow->bondSettingsSent = true;
  bool acceptedDuring = false;
  gPreempt = [&] {
    if (!acceptedDuring && deliver(kPartnerA, RX_CAP_REQ)) { acceptedDuring = true; workerTick(); }
  };
  cmd_espnow_bondmodeenabled(String("1"));
  gPreempt = nullptr;
  CHECK(gSettings.bondModeEnabled, "espnowbondmodeenabled 1 did not enable bond mode");
  if (acceptedDuring) {
    CHECK(gEspNow->bondCapSent, "a frame accepted during espnowbondmodeenabled was wiped");
  }
  // A capability exchange accepted during the command may set cap again; the
  // settings the worker sent to an earlier bond must be gone either way.
  CHECK(!gEspNow->bondSettingsSent && (acceptedDuring || !gEspNow->lastRemoteCapValid),
        "espnowbondmodeenabled kept sync state from before (cap=%d settTx=%d)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondSettingsSent);
}

// A settings page saves every field: the same value changes and resets nothing.
static void caseSameValueSave() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondCapSent = true;
  gEspNow->bondSettingsSent = true;
  gEspNow->bondPeerOnline = true;
  cmd_espnow_bondmodeenabled(String("1"));
  cmd_espnow_bondpeermac(String("02:11:22:33:44:01"));
  CHECK(gEspNow->lastRemoteCapValid && gEspNow->bondCapSent && gEspNow->bondSettingsSent &&
        gEspNow->bondPeerOnline, "saving the same bond settings reset the bond");
  CHECK(deliver(kPartnerA, RX_CAP_REQ), "the partner is not accepted after an unchanged save");
  const char* q = cmd_espnow_bondpeermac(String(""));
  CHECK(std::strcmp(q, "query") == 0, "a query went somewhere else: %s", q);
  // A value that is not a MAC address is refused and changes nothing.
  const char* refused = cmd_espnow_bondpeermac(String("kitchen"));
  CHECK(commandReplyIsFailure(refused), "a name was taken as the partner address: %s", refused);
  CHECK(gSettings.bondPeerMac == String("02:11:22:33:44:01") && gEspNow->lastRemoteCapValid,
        "a refused partner address changed the bond");
  // A MAC written in lower case is stored in the display form bondconnect compares with.
  cmd_espnow_bondpeermac(String("02:11:22:33:44:0a"));
  CHECK(gSettings.bondPeerMac == String("02:11:22:33:44:0A"), "the partner is not stored in display form: %s",
        gSettings.bondPeerMac.c_str());
}

// bonddisconnect: the partner's frames stop and its state goes.
static void caseDisconnect() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondPeerOnline = true;
  int accepted = 0;
  gPreempt = [&] { if (deliver(kPartnerA, RX_CAP_RESP)) ++accepted; };
  cmd_bond_disconnect(String(""));
  gPreempt = nullptr;
  CHECK(accepted == 0 || !gEspNow->lastRemoteCapValid, "bonddisconnect kept state from a frame it accepted");
  CHECK(!gSettings.bondModeEnabled && !gEspNow->lastRemoteCapValid && !gEspNow->bondPeerOnline,
        "bonddisconnect left bond state behind");
  CHECK(!deliver(kPartnerA, RX_CAP_REQ), "the partner is accepted after bonddisconnect");
}

// A worker that lost its own record (its bondresync, a heartbeat timeout only
// it saw) while the master stayed SYNCED gets back to SYNCED by itself.
static void caseWorkerLostRecord() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->bondPeerOnline = true;
  gMaster.hasWorkerCap = gMaster.sentOwnCap = gMaster.hasWorkerSettings = true;   // master SYNCED
  runFor(65000);
  CHECK(isBondSynced(), "a worker that lost its sync record stays SYNCING while the master is SYNCED "
        "(cap=%d capSent=%d settTx=%d)", (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondCapSent,
        (int)gEspNow->bondSettingsSent);
  // A master's STATUS_REQ never marks the master's own obligations.
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  deliver(kPartnerA, RX_STATUS_REQ);
  CHECK(!gEspNow->bondCapSent && !gEspNow->bondSettingsSent, "a master took a status request as 'sent'");
}

// The worker's capability request does not fire while the bond is healthy.
static void caseNoAskWhenHealthy() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = gEspNow->bondCapSent = gEspNow->bondSettingsSent = true;
  gMaster.hasWorkerCap = gMaster.sentOwnCap = gMaster.hasWorkerSettings = true;
  runFor(60000);
  size_t capReqs = 0;
  for (uint8_t t : gSentTypes) capReqs += (t == ESPNOW_V4_TYPE_BOND_CAP_REQ);
  CHECK(capReqs == 0, "a synced worker asked for the capability %zu times", capReqs);
  // Session down: no asking either.
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->bondPeerOnline = true;
  gSessionUp = false;
  gMaster.hasWorkerCap = gMaster.sentOwnCap = gMaster.hasWorkerSettings = true;
  gMaster.lastStatusReqMs = 1;   // keeps the model quiet
  for (int i = 0; i < 600; ++i) { gNowMs += 100; workerTick(); }
  CHECK(gSentTypes.empty(), "the worker asked without a session");
}

// An unasked CAP_RESP does not restart a manifest transfer under way; the
// answer to our CAP_REQ still ends that request.
static void caseCapRespInFlight() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->bondCapSent = true;
  gEspNow->bondSyncInFlight = BOND_SYNC_MANIFEST;
  gEspNow->bondSyncRetryCount = 2;
  gEspNow->bondSyncLastAttemptMs = 5000;
  deliver(kPartnerA, RX_CAP_RESP);
  CHECK(gEspNow->bondSyncInFlight == BOND_SYNC_MANIFEST && gEspNow->bondSyncRetryCount == 2 &&
        gEspNow->bondSyncLastAttemptMs == 5000,
        "an unasked CAP_RESP restarted the manifest request (inFlight=%d)", (int)gEspNow->bondSyncInFlight);
  CHECK(gEspNow->lastRemoteCapValid, "the CAP_RESP was not recorded");
  gEspNow->bondSyncInFlight = BOND_SYNC_CAP;
  gEspNow->bondSyncRetryCount = 1;
  deliver(kPartnerA, RX_CAP_RESP);
  CHECK(gEspNow->bondSyncInFlight == BOND_SYNC_NONE && gEspNow->bondSyncRetryCount == 0,
        "the answer to CAP_REQ did not end the request");
  // A CAP_RESP from a later sender (a field appended after 1.0): read up to the
  // fields known here and cached (it used to need exactly sizeof).
  gEspNow->lastRemoteCapValid = false;
  {
    uint8_t longer[sizeof(CapabilitySummary) + 8];
    std::memset(longer, 0, sizeof longer);
    CapabilitySummary sent = {};
    sent.sensorMask = 0x80000001u;
    std::memcpy(longer, &sent, sizeof sent);
    std::memset(longer + sizeof sent, 0x77, 8);
    esp_now_recv_info info{kPartnerA};
    V4RxCtx ctx{&info, longer, (uint16_t)sizeof longer, "partner"};
    v4h_bond_cap_resp(ctx);
  }
  CHECK(gEspNow->lastRemoteCapValid && gEspNow->lastRemoteCap.sensorMask == 0x80000001u,
        "a CAP_RESP longer than the struct was not cached (valid=%d)", (int)gEspNow->lastRemoteCapValid);
}

// bondconnect starts the handshake at once on the initiator side, as a manual
// start (its failure is printed, not rate-limited), and ends the partner's
// back-off on both sides.
static void caseConnectKicksHandshake() {
  resetWorld(kMasterSelf);
  cmd_bond_connect(String("02:11:22:33:44:01"));
  CHECK(isBondMaster(), "the higher address becomes master");
  CHECK(gManualStarts == 1 && gAutoStarts == 0 && gBackoffClears >= 1,
        "bondconnect on the initiator: %d manual and %d automatic handshake starts, %d back-off clears "
        "(want 1, 0, >=1)", gManualStarts, gAutoStarts, gBackoffClears);
  resetWorld(kWorkerSelf);
  cmd_bond_connect(String("02:11:22:33:44:01"));
  CHECK(gManualStarts == 0 && gAutoStarts == 0 && gBackoffClears >= 1,
        "bondconnect on the responder: %d handshake starts, %d back-off clears (want 0, >=1)",
        gManualStarts + gAutoStarts, gBackoffClears);
}

// ---- The bond parts of the espnow_task loop during a partner change (X5) --------
static const uint8_t kPartnerC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03};
static void runTicks() {
  hbTickProduction(gNowMs);
  syncTickProduction(gNowMs);
}
static size_t heldFrames(uint8_t type = 0) {
  size_t n = 0;
  for (const auto& f : gFrames) n += f.held && (type == 0 || f.type == type);
  return n;
}
static size_t framesTo(const uint8_t* mac, uint8_t type, bool held) {
  size_t n = 0;
  for (const auto& f : gFrames) n += f.held == held && f.type == type && macEquals(f.mac, mac);
  return n;
}
static size_t heldPosts(uint8_t type) {
  size_t n = 0;
  for (const auto& e : gPosted) n += e.held && e.type == type;
  return n;
}
// espnow_task's view while a command runs: `stepMs` pass each time it takes
// the CPU, and the old partner is heard whenever its frames are not held.
static void preemptWithTicks(uint32_t stepMs) {
  gPreempt = [stepMs] {
    gNowMs += stepMs;
    if (!bondPartnerChanging() && gSettings.bondPeerMac == String("02:11:22:33:44:01")) {
      gState.lastBondHeartbeatReceivedMs = gNowMs;
    }
    runTicks();
  };
}

// Finding X5: master bonded to A, which is online with its capability known
// and its manifest not yet looked up, but A's session is rekeying, so the
// tick waits. bondconnect B (session up): while the partner setting already
// names B and the state is still A's, the tick must not record A's manifest
// for B, send anything, time the partner out or clear B's session.
static void caseSwitchHoldsTick() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->lastRemoteCap.fwHash[0] = 0xA1;   // A's firmware
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
  gSessionDownFor = kPartnerA;
  preemptWithTicks(5000);
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  size_t forB = 0;
  for (const auto& h : gManifestHits) forB += macEquals(h.mac, kPartnerB);
  CHECK(forB == 0, "X5 the tick recorded A's manifest for the new partner B during bondconnect (%zu)", forB);
  CHECK(heldFrames() == 0, "X5 %zu bond frames were sent while the partner change held bond frames "
        "(heartbeats %zu, MANIFEST_REQ %zu)", heldFrames(), heldFrames(ESPNOW_V4_TYPE_BOND_HEARTBEAT),
        heldFrames(ESPNOW_V4_TYPE_MANIFEST_REQ));
  size_t heldClears = 0;
  for (const auto& c : gSessionClears) heldClears += c.held;
  CHECK(heldClears == 0 && heldPosts(SYSEVT_BOND_OFFLINE) == 0,
        "X5 the heartbeat timeout acted during the partner change (%zu sessions cleared, %zu offline events)",
        heldClears, heldPosts(SYSEVT_BOND_OFFLINE));
  CHECK(framesTo(kPartnerB, ESPNOW_V4_TYPE_BOND_HEARTBEAT, false) >= 1,
        "the heartbeat did not go to the new partner after the change");
  // The tick works again once the hold is over: B comes online with its own
  // capability and the master looks its manifest up.
  gManifestHits.clear();
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->lastRemoteCap.fwHash[0] = 0xB2;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  runTicks();
  CHECK(gManifestHits.size() == 1 && macEquals(gManifestHits[0].mac, kPartnerB) && gManifestHits[0].hash0 == 0xB2 &&
        gEspNow->bondManifestReceived, "after the change the master did not look up B's own manifest");
}

// Finding X5: the post-sync status request and the deferred answers to the
// old partner wait too. Master fully synced with A (status request not yet
// sent, A's session rekeying), an answer to A's CAP_REQ and a streaming setup
// pending; bondconnect B.
static void caseSwitchHoldsPostSync() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondManifestReceived = true;
  gEspNow->bondSettingsReceived = true;
  gEspNow->bondCapSent = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
  gSessionDownFor = kPartnerA;
  gPreempt = [] {
    // Set as the hold begins: frames A sent just before it, acted on in it.
    if (bondPartnerChanging() && !gEspNow->bondNeedsCapabilityResponse && !gEspNow->bondNeedsStreamingSetup &&
        gSettings.bondPeerMac == String("02:11:22:33:44:02") && gEspNow->bondManifestReceived) {
      std::memcpy(gEspNow->bondPendingResponseMac, kPartnerA, 6);
      gEspNow->bondNeedsCapabilityResponse = true;
      gEspNow->bondNeedsStreamingSetup = true;
    }
    gNowMs += 10;
    runTicks();
  };
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  size_t heldSync = 0;
  for (const auto& p : gPostSync) heldSync += p.held;
  CHECK(heldSync == 0, "X5 the post-sync status request went out during the partner change (%zu)", heldSync);
  CHECK(heldFrames() == 0, "X5 %zu bond frames were sent during the partner change (CAP_RESP %zu, stream %zu)",
        heldFrames(), heldFrames(ESPNOW_V4_TYPE_BOND_CAP_RESP), heldFrames(kSentStreamCtrl));
}

// Finding X5: a worker whose capability from A is missing (A's session
// rekeying) does not ask the new partner for it from A's flags.
static void caseSwitchHoldsWorkerAsk() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
  gSessionDownFor = kPartnerA;
  runTicks();                       // not missing yet: A's session is down
  gPreempt = [] {
    gNowMs += 10000;
    gState.lastBondHeartbeatReceivedMs = gNowMs;   // no timeout here
    runTicks();
  };
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  CHECK(heldFrames(ESPNOW_V4_TYPE_BOND_CAP_REQ) == 0,
        "X5 the worker asked the new partner for a capability during the change (%zu)",
        heldFrames(ESPNOW_V4_TYPE_BOND_CAP_REQ));
}

// Round 4: the deferred answers (9d, 9e, 9e2) and the status steps (9j, 9j2,
// 9k) wait too. Master SYNCED with A; as the hold begins, A's MANIFEST_REQ,
// SETTINGS_REQ, SCHEMA_REQ and STATUS_REQ are pending, a sensor change asks
// for a status push and the 30 s status poll is due. bondconnect B: nothing
// goes out while held (to A, or to B from A's state), and nothing of A's goes
// to A after the change either.
static void caseSwitchHoldsAnswersAndStatus() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondManifestReceived = true;
  gEspNow->bondSettingsReceived = true;
  gEspNow->bondCapSent = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
  bool armed = false;
  size_t armedAt = 0;
  gPreempt = [&armed, &armedAt] {
    if (!armed && bondPartnerChanging() && gSettings.bondPeerMac == String("02:11:22:33:44:02") &&
        gState.bondManifestReceived) {
      armed = true;
      armedAt = gFrames.size();
      std::memcpy(gState.bondPendingResponseMac, kPartnerA, 6);
      gState.bondNeedsManifestResponse = true;
      gState.bondNeedsSettingsResponse = true;
      gState.bondNeedsSchemaResponse = true;
      gState.bondNeedsStatusResponse = true;
      gState.bondNeedsProactiveStatus = true;
      gState.bondLastStatusReqMs = gNowMs - 30000;
    }
    gNowMs += 10;
    runTicks();
  };
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  CHECK(armed, "the case did not reach the hold with A's state");
  const size_t heldFiles = heldFrames(kSentFileBase + BOND_FILE_KIND_MANIFEST) +
                           heldFrames(kSentFileBase + BOND_FILE_KIND_SETTINGS) +
                           heldFrames(kSentFileBase + BOND_FILE_KIND_SCHEMA);
  CHECK(heldFrames() == 0,
        "X5 %zu bond frames went out during the partner change (file answers %zu, status answer/push %zu, "
        "status poll %zu)", heldFrames(), heldFiles, heldFrames(ESPNOW_V4_TYPE_BOND_STATUS_RESP),
        heldFrames(ESPNOW_V4_TYPE_BOND_STATUS_REQ));
  // The rest of bondconnect and a few seconds of ticks: A's requests stay unanswered.
  for (int i = 0; i < 50; ++i) { gNowMs += 100; runTicks(); }
  size_t toA = 0;
  for (size_t i = armedAt; i < gFrames.size(); ++i) toA += macEquals(gFrames[i].mac, kPartnerA);
  CHECK(toA == 0, "X5 after the change %zu answers to the old partner's requests still went to it", toA);
}

// Round 4: a step of the tick that waits lets cmd_exec run a whole partner
// change. The tick must not go on with the old partner's state afterwards.
static void caseTickWaitsThroughSwitch() {
  // The gap between stream commands (9f): master SYNCED with A, two sensors
  // to stream; bondconnect B runs during the first gap.
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondManifestReceived = gEspNow->bondSettingsReceived = true;
  gEspNow->bondStatusReqSentOnce = true;
  gEspNow->bondLastStatusReqMs = gNowMs;
  gEspNow->bondNeedsStreamingSetup = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
  gStreamThermal = true;
  gTickWait = [] { cmd_bond_connect(String("02:11:22:33:44:02")); };
  syncTickProduction(gNowMs);
  CHECK(gSettings.bondPeerMac == String("02:11:22:33:44:02"), "the partner change did not run in the gap");
  CHECK(framesTo(kPartnerA, kSentStreamCtrl, false) == 1, "the first stream command did not go to A");
  CHECK(framesTo(kPartnerB, kSentStreamCtrl, false) == 0,
        "X5 the stream setup went on after a partner change in its gap and sent A's stream settings to B");

  // The manifest lookup waits for the FS lock: master with A's capability,
  // A's manifest not yet looked up; bondconnect B runs during the wait.
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->lastRemoteCap.fwHash[0] = 0xA1;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gTickWait = [] { cmd_bond_connect(String("02:11:22:33:44:02")); };
  syncTickProduction(gNowMs);
  CHECK(gSettings.bondPeerMac == String("02:11:22:33:44:02"), "the partner change did not run in the lookup");
  CHECK(!gEspNow->bondManifestReceived,
        "X5 a partner change during the manifest lookup: A's cache hit marked B's manifest as received");
  size_t forB = 0;
  for (const auto& h : gManifestHits) forB += macEquals(h.mac, kPartnerB);
  CHECK(forB == 0 && framesTo(kPartnerB, ESPNOW_V4_TYPE_MANIFEST_REQ, false) == 0,
        "X5 a partner change during the manifest lookup: B got A's manifest reference or a MANIFEST_REQ");
}

// ---- espnowunpair of the bond partner (X4) -----------------------------------
static bool replyHas(const char* reply, const char* text) { return std::strstr(reply, text) != nullptr; }

// Finding X4: bonded to A (initiator side), A online and synced. Unpairing A
// ends the bond before anything about A is removed, the way bonddisconnect
// does; afterwards the heartbeat starts nothing toward A.
static void caseUnpairPartner() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondManifestReceived = gEspNow->bondSettingsReceived = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gPreempt = [] {
    gNowMs += 10;
    runTicks();
  };
  const char* reply = cmd_espnow_unpair(String("02:11:22:33:44:01"));
  gPreempt = nullptr;
  CHECK(!gSettings.bondModeEnabled && gSettings.bondPeerMac.length() == 0,
        "X4 unpairing the bond partner left bond mode on with it (mode=%d peer='%s')",
        (int)gSettings.bondModeEnabled, gSettings.bondPeerMac.c_str());
  CHECK(!gEspNow->bondPeerOnline && !gEspNow->lastRemoteCapValid && !isBondSynced(),
        "X4 the unpaired partner's bond state was kept (online=%d cap=%d)",
        (int)gEspNow->bondPeerOnline, (int)gEspNow->lastRemoteCapValid);
  size_t offline = 0;
  for (const auto& e : gPosted) offline += e.type == SYSEVT_BOND_OFFLINE && e.detail == "02:11:22:33:44:01";
  CHECK(offline == 1, "X4 the bond offline event for the unpaired partner was posted %zu times (want 1)", offline);
  CHECK(replyHas(reply, "Unpaired device") && replyHas(reply, "bond"),
        "X4 the reply does not say the bond was ended: %s", reply);
  // Order: the bond ends before the peer is deleted, before anything already
  // under way is called off and before its identity and folder go.
  CHECK(stepAt("deleteRuntime") != SIZE_MAX && !stepSawBond("deleteRuntime") && !stepSawBond("forgetCrypto") &&
        !stepSawBond("peerFilesRemove"),
        "X4 the identity or folder was removed while bond mode still named the partner");
  CHECK(stepAt("keyExCancel") < stepAt("forgetCrypto") && stepAt("pendingDrop") < stepAt("forgetCrypto") &&
        stepAt("forgetCrypto") < stepAt("peerFilesRemove"),
        "X4 unpair order: call off the key exchange and queued frames, forget the identity, remove the folder");
  // A minute later: no heartbeat to A, no automatic handshake toward it.
  const int autoStarts = gAutoStarts;
  const size_t framesBefore = gFrames.size();
  for (int i = 0; i < 600; ++i) { gNowMs += 100; runTicks(); }
  size_t toA = 0;
  for (size_t i = framesBefore; i < gFrames.size(); ++i) toA += macEquals(gFrames[i].mac, kPartnerA);
  CHECK(toA == 0 && gAutoStarts == autoStarts,
        "X4 after the unpair the bond still sent %zu frames to the unpaired partner and started %d handshakes",
        toA, gAutoStarts - autoStarts);
}

// Unpairing another device leaves the bond alone; a partner set while bond
// mode is off is cleared without an offline event; a failed peer delete says
// the bond went.
static void caseUnpairOther() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gEspNow->bondPeerOnline = true;
  const char* reply = cmd_espnow_unpair(String("02:11:22:33:44:03"));
  CHECK(gSettings.bondModeEnabled && gSettings.bondPeerMac == String("02:11:22:33:44:01") &&
        gEspNow->bondPeerOnline && !replyHas(reply, "bond") && gPosted.size() == 1,
        "unpairing another device touched the bond: %s", reply);

  resetWorld(kMasterSelf);
  gSettings.bondPeerMac = "02:11:22:33:44:01";      // bond mode off, partner still set
  reply = cmd_espnow_unpair(String("02:11:22:33:44:01"));
  CHECK(gSettings.bondPeerMac.length() == 0 && !gSettings.bondModeEnabled,
        "X4 a partner set while bond mode is off still names the unpaired device");
  CHECK(heldPosts(SYSEVT_BOND_OFFLINE) == 0 && gPosted.size() == 1 && replyHas(reply, "bond partner"),
        "X4 bond mode off: no offline event, and the reply says it is no longer the partner: %s", reply);

  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gDeleteOk = false;
  reply = cmd_espnow_unpair(String("02:11:22:33:44:01"));
  CHECK(std::strncmp(reply, "Error:", 6) == 0 && replyHas(reply, "bond") && !gSettings.bondModeEnabled,
        "a failed unpair of the partner does not say the bond was ended: %s", reply);
}

// Round 4: a bond file answer queued on cmd_exec before an unpair or partner
// change (9d/9e/9e2 submit it; it runs when cmd_exec gets to it) is dropped
// when it runs: sending it would key-exchange the unpaired device again.
static void runQueuedBondFiles() {
  std::vector<BondFileSendWork*> q;
  q.swap(gCmdExecQ);
  for (auto* w : q) runDeferredBondFileSend(w);
}
static void queueBondAnswers(const uint8_t* to) {
  std::memcpy(gState.bondPendingResponseMac, to, 6);
  gState.bondNeedsManifestResponse = gState.bondNeedsSettingsResponse = gState.bondNeedsSchemaResponse = true;
  syncTickProduction(gNowMs);
}
static void bondedWorkerOfA() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
}
static size_t fileSendsTo(const uint8_t* mac) {
  size_t n = 0;
  for (const auto& f : gFileSends) n += macEquals(f.mac, mac);
  return n;
}
static void caseQueuedBondFileAfterUnpair() {
  bondedWorkerOfA();
  queueBondAnswers(kPartnerA);
  CHECK(gCmdExecQ.size() == 3, "the tick queued %zu bond file answers (want 3)", gCmdExecQ.size());
  runQueuedBondFiles();
  CHECK(fileSendsTo(kPartnerA) == 3, "queued answers to the online partner were not sent (%zu of 3)",
        fileSendsTo(kPartnerA));

  bondedWorkerOfA();
  queueBondAnswers(kPartnerA);
  cmd_espnow_unpair(String("02:11:22:33:44:01"));
  runQueuedBondFiles();
  CHECK(gFileSends.empty(), "X4 a bond file answer queued before the unpair still went to the unpaired device "
        "(%zu sends, manifest %d): its key exchange brings the identity back",
        gFileSends.size(), (int)(!gFileSends.empty() && gFileSends[0].kind == BOND_FILE_KIND_MANIFEST));

  bondedWorkerOfA();
  queueBondAnswers(kPartnerA);
  cmd_bond_connect(String("02:11:22:33:44:02"));
  runQueuedBondFiles();
  CHECK(fileSendsTo(kPartnerA) == 0, "a bond file answer queued before bondconnect B still went to A (%zu)",
        fileSendsTo(kPartnerA));
}

#if HW1_HAVE_AGREE
// ---- The partner a command drops is told (BOND_NOTICE ENDED) ---------------------
static size_t endedTo(const uint8_t* mac) {
  size_t n = 0;
  for (const auto& f : gAgreeFrames) {
    n += f.type == ESPNOW_V4_TYPE_BOND_NOTICE && f.b0 == BOND_NOTICE_ENDED && macEquals(f.mac, mac);
  }
  return n;
}
static void bondedMasterOfA() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gLastBondHeartbeatSentMs = gNowMs;
}
// One ENDED to `mac`: sent by the command itself (not left in the send queue,
// where it would go out after an unpair removed the session), before the frame
// hold, while bond mode still named the partner. It carries the number of the
// last request this device made (0: none), and no request follows it: one
// made before it may still be in the send queue, and the partner takes the
// requests up to that number as void.
static void checkEndedOnce(const uint8_t* mac, const char* what) {
  const AgreeFrame* ended = nullptr;
  uint16_t lastRequest = 0;
  size_t requestsAfter = 0;
  for (const auto& f : gAgreeFrames) {
    if (f.type == ESPNOW_V4_TYPE_BOND_NOTICE && macEquals(f.mac, mac)) ended = &f;
    if (f.type == ESPNOW_V4_TYPE_BOND_REQUEST && macEquals(f.mac, mac)) {
      if (ended) ++requestsAfter; else lastRequest = f.seq;
    }
  }
  CHECK(endedTo(mac) == 1 && ended && ended->direct && !ended->held,
        "%s: the dropped partner was told ENDED %zu times (want once, by the command, before the hold)",
        what, endedTo(mac));
  CHECK(ended && ended->seq == lastRequest && requestsAfter == 0,
        "%s: ENDED names request %u, the last one made was %u, and %zu requests followed it (want none)",
        what, ended ? (unsigned)ended->seq : 0u, (unsigned)lastRequest, requestsAfter);
  CHECK(stepSawBond("directSend"), "%s: ENDED went out after the bond setting was already changed", what);
  size_t held = 0;
  for (const auto& f : gAgreeFrames) held += f.held;
  CHECK(held == 0, "%s: %zu requests or notices went out while bond frames were held", what, held);
}

static void caseEndedNotice() {
  bondedMasterOfA();
  preemptWithTicks(10);
  cmd_bond_disconnect(String(""));
  gPreempt = nullptr;
  checkEndedOnce(kPartnerA, "bonddisconnect");

  bondedMasterOfA();
  preemptWithTicks(10);
  cmd_bond_connect(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  checkEndedOnce(kPartnerA, "bondconnect to another device");
  CHECK(endedTo(kPartnerB) == 0, "the new partner was told ENDED");

  bondedMasterOfA();
  cmd_bond_connect(String("02:11:22:33:44:01"));
  CHECK(gAgreeFrames.empty(), "bondconnect to the partner it already has told it ENDED");

  bondedMasterOfA();
  preemptWithTicks(10);
  cmd_espnow_bondmodeenabled(String("0"));
  gPreempt = nullptr;
  checkEndedOnce(kPartnerA, "espnowbondmodeenabled 0");

  bondedMasterOfA();
  preemptWithTicks(10);
  cmd_espnow_bondpeermac(String("02:11:22:33:44:02"));
  gPreempt = nullptr;
  checkEndedOnce(kPartnerA, "espnowbondpeermac to another device");

  // The same partner written another way is not a new partner.
  bondedMasterOfA();
  gSettings.bondPeerMac = "02:11:22:33:44:0A";
  cmd_espnow_bondpeermac(String("02-11-22-33-44-0a"));
  CHECK(endedTo(kPartnerA) == 0 && gAgreeFrames.empty(), "the same partner in another spelling was told ENDED");

  // Unpair: the notice goes out before the peer, its session and its identity go.
  bondedMasterOfA();
  cmd_espnow_unpair(String("02:11:22:33:44:01"));
  checkEndedOnce(kPartnerA, "espnowunpair of the partner");
  CHECK(stepAt("directSend") < stepAt("deleteRuntime") && stepAt("directSend") < stepAt("forgetCrypto"),
        "espnowunpair: ENDED was sent after the peer or its session was removed");
  bondedMasterOfA();
  cmd_espnow_unpair(String("02:11:22:33:44:03"));
  CHECK(gAgreeFrames.empty(), "unpairing another device told someone ENDED");

  // Bond mode off (a partner is set but is not a partner): nobody to tell.
  resetWorld(kMasterSelf);
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  cmd_bond_connect(String("02:11:22:33:44:02"));
  cmd_espnow_unpair(String("02:11:22:33:44:01"));
  CHECK(endedTo(kPartnerA) == 0, "a partner set while bond mode was off was told ENDED");

  // No session with the partner: nothing is sent, nothing waits, and no
  // handshake is started for it.
  bondedMasterOfA();
  gSessionDownFor = kPartnerA;
  cmd_bond_disconnect(String(""));
  CHECK(gAgreeFrames.empty() && gAutoStarts == 0 && gManualStarts == 0 && !gSettings.bondModeEnabled,
        "bonddisconnect without a session: %zu frames, %d handshake starts (want none)", gAgreeFrames.size(),
        gAutoStarts + gManualStarts);
}
#endif

// Bench 2026-11-18: every settings fetch sent SETTINGS_REQ twice ("Requesting
// SETTINGS" and "Retry SETTINGS_REQ (2/3)" in the same millisecond): the
// request was stamped later than the tick's `now`. One request per fetch; the
// retry comes BOND_SYNC_RETRY_MS later.
static void caseSettingsRequestOnce() {
  resetWorld(kMasterSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  gMaster.mac = kPartnerC;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastRemoteCapValid = true;
  gEspNow->bondManifestReceived = true;
  gEspNow->bondSettingsReceived = false;   // the partner's settings hash changed
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  gFrames.clear();
  syncTickProduction(gNowMs);
  CHECK(framesTo(kPartnerA, kSentSettingsReq, false) == 1, "one tick sent %zu SETTINGS_REQ, not 1",
        framesTo(kPartnerA, kSentSettingsReq, false));
  gNowMs += BOND_SYNC_RETRY_MS - 1;
  gEspNow->lastBondHeartbeatReceivedMs = gNowMs;
  syncTickProduction(gNowMs);
  CHECK(framesTo(kPartnerA, kSentSettingsReq, false) == 1, "the retry came before BOND_SYNC_RETRY_MS");
  gNowMs += 1;
  syncTickProduction(gNowMs);
  CHECK(framesTo(kPartnerA, kSentSettingsReq, false) == 2 && gEspNow->bondSyncRetryCount == 2,
        "no retry after BOND_SYNC_RETRY_MS (%zu requests)", framesTo(kPartnerA, kSentSettingsReq, false));
}

// BOND_CAP_RESP's device name follows the one rule (System_DeviceName.h): an
// invalid or unterminated name is stored as none, so every page that prints
// lastRemoteCap.deviceName gets a valid name or "".
static void caseCapRespName() {
  resetWorld(kWorkerSelf);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = "02:11:22:33:44:01";
  esp_now_recv_info info{kPartnerA};
  auto send = [&](const char* name, size_t len) {
    CapabilitySummary cap = {};
    std::memcpy(cap.deviceName, name, len);
    V4RxCtx ctx{&info, reinterpret_cast<const uint8_t*>(&cap), sizeof(cap), "partner"};
    v4h_bond_cap_resp(ctx);
  };
  send("bad name!", 9);
  CHECK(gEspNow->lastRemoteCapValid && gEspNow->lastRemoteCap.deviceName[0] == '\0',
        "an invalid capability name is stored as none (got '%.32s')", gEspNow->lastRemoteCap.deviceName);
  const std::string n31(31, 'q');
  send(n31.c_str(), 31);
  CHECK(n31 == gEspNow->lastRemoteCap.deviceName, "a 31-byte capability name is stored whole");
  const std::string full(sizeof(CapabilitySummary::deviceName), 'x');
  send(full.c_str(), full.size());
  CHECK(gEspNow->lastRemoteCap.deviceName[0] == '\0', "a name field without its NUL is stored as none");
}

int main() {
  caseCapRespName();
  caseSettingsRequestOnce();
  caseBenchOrder();
  caseSwitchPartner();
  casePeerMacSetting();
  caseModeEnabledSetting();
  caseSameValueSave();
  caseDisconnect();
  caseWorkerLostRecord();
  caseNoAskWhenHealthy();
  caseCapRespInFlight();
  caseConnectKicksHandshake();
  caseSwitchHoldsTick();
  caseSwitchHoldsPostSync();
  caseSwitchHoldsWorkerAsk();
  caseUnpairPartner();
  caseUnpairOther();
  caseSwitchHoldsAnswersAndStatus();
  caseTickWaitsThroughSwitch();
  caseQueuedBondFileAfterUnpair();
#if HW1_HAVE_AGREE
  caseEndedNotice();
#endif
  std::printf("espnow_bond_partner_change: %d checks, %d failed (%d frames accepted, %d dropped)\n",
              gChecks, gFailures, gAccepted, gDropped);
  return gFailures ? 1 : 0;
}
