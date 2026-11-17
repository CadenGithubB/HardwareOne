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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "System_MacText.h"

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
  String bondPeerMac;
};
static Settings gSettings;

// What espnow_task does when it gets the CPU in the middle of a command. What
// it runs (the handlers) does not preempt itself.
static std::function<void()> gPreempt;
static bool gInPreempt = false;
static void preempt() {
  if (!gPreempt || gInPreempt) return;
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
  bool bondPeerOnline = false;
  uint32_t lastBondHeartbeatReceivedMs = 0;
  uint8_t bondPendingResponseMac[6] = {};
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

struct esp_now_recv_info { const uint8_t* src_addr; };
struct V4RxCtx {
  const esp_now_recv_info* recv_info;
  const uint8_t* payload;
  uint16_t payloadLen;
  const char* deviceName;
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
static constexpr uint8_t SYSEVT_BOND_OFFLINE = 1;
static void systemEventPost(uint8_t, const char* = nullptr, const char* = nullptr) {}
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
[[maybe_unused]] static bool bondSessionActiveWith(const uint8_t*) { return gSessionUp; }

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
[[maybe_unused]] static constexpr uint16_t ESPNOW_V4_FLAG_ACK_REQ = 0x01;
static std::vector<uint8_t> gSentTypes;
[[maybe_unused]] static bool bondSendEncryptedAsync(const uint8_t*, uint8_t type, uint16_t, uint32_t,
                                                    const uint8_t*, uint16_t) {
  gSentTypes.push_back(type);
  if (type == ESPNOW_V4_TYPE_BOND_CAP_REQ) masterOnWorkerCapReq();
  return true;
}

static void resetBondSync();   // declared ahead of its users, as in the source

// INSERT_PRODUCTION

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
  gSentTypes.clear();
  gBackoffClears = gAutoStarts = gManualStarts = 0;
  std::memcpy(gSelfMac, self, 6);
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
  const char* reply = cmd_bond_connect(String("02:11:22:33:44:01"));
  gPreempt = nullptr;
  CHECK(std::strncmp(reply, "Bond mode enabled", 17) == 0, "bondconnect reply: %s", reply);
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

int main() {
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
  std::printf("espnow_bond_partner_change: %d checks, %d failed (%d frames accepted, %d dropped)\n",
              gChecks, gFailures, gAccepted, gDropped);
  return gFailures ? 1 : 0;
}
