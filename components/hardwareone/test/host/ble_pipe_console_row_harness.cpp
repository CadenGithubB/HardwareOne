// The phone link's console row (phone link v2), as shipped:
// test_ble_pipe_console_row.py inserts Bluetooth.cpp's session table, epoch
// helpers, the console row's lifecycle, the auth rule, revocation, the
// disconnect owner functions, the command tail, the record ingress, the output
// router and design A's slot helpers, and System_OTA.cpp's
// currentSecureBleConnection. The console driver, transport sessions, the
// Secure Channel, the executor and OTA's frame sink are host boundaries that
// record what was done. Ids: the phone link is Bluedroid connection 3; the
// console row of pipe generation 7 is 0x8007.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <strings.h>
#include <vector>

// A host Arduino String with the surface the spliced code uses, an integer
// constructor included (bleFillCommandAuth stamps the session id as String(connId)).
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  explicit String(unsigned value) : text_(std::to_string(value)) {}
  explicit String(int value) : text_(std::to_string(value)) {}
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  String& operator+=(const char* other) { text_ += other ? other : ""; return *this; }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator!=(const String& other) const { return text_ != other.text_; }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  bool equalsIgnoreCase(const String& other) const {
    return text_.size() == other.text_.size() && strcasecmp(text_.c_str(), other.text_.c_str()) == 0;
  }
  bool equalsIgnoreCase(const char* other) const { return equalsIgnoreCase(String(other)); }

 private:
  std::string text_;
};

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Host boundaries
// ---------------------------------------------------------------------------
static uint32_t nowMs = 1000;
static uint32_t millis() { return nowMs; }
using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(TickType_t ticks) { nowMs += ticks; }

// The recursive lifecycle mutex (depth; one task at a time here).
struct HostMutex { int depth = 0; };
using SemaphoreHandle_t = HostMutex*;
static HostMutex gLifecycle;
static SemaphoreHandle_t sBleLifecycleMutex = &gLifecycle;
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
  ++mutex->depth;
  return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  --mutex->depth;
  return pdTRUE;
}
static bool bleInitLifecycleMutex() { return true; }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

#define DEBUG_BLE_CORE 1
#define DEBUG_BLE_GATT 2
#define DEBUG_BLE_DATA 3
#define BLE_DEBUGF(flag, ...) ((void)(flag))
static volatile uint32_t gOutputFlags = MSG_ROUTE_SERIAL;

static struct Settings {
  bool bleRequireAuth = true;
  bool bleConsoleOnPipe = true;
  String bleSecureChannelSecret = "Pass-phrase1!";
} gSettings;

// Transport sessions: 12 global slots (System_User.cpp), never-repeating epochs.
static constexpr size_t kTransportSlots = 12;
static std::set<TransportSessionEpoch> liveSessions;
static TransportSessionEpoch nextEpoch = 100;
static unsigned sessionOpens = 0, sessionCloses = 0;
static TransportSessionEpoch transportSessionOpen(CommandSource) {
  if (liveSessions.size() >= kTransportSlots) return 0;
  ++sessionOpens;
  liveSessions.insert(++nextEpoch);
  return nextEpoch;
}
static void transportSessionClose(CommandSource, TransportSessionEpoch epoch) {
  if (epoch == 0) return;
  ++sessionCloses;
  liveSessions.erase(epoch);
}

// Events.
enum HostSystemEvent : uint8_t { SYSEVT_BLE_CONNECTED = 1, SYSEVT_BLE_DISCONNECTED };
static unsigned connectedEvents = 0, disconnectedEvents = 0;
static void systemEventPost(uint8_t kind, const char* = nullptr, const char* = nullptr) {
  if (kind == SYSEVT_BLE_CONNECTED) ++connectedEvents;
  if (kind == SYSEVT_BLE_DISCONNECTED) ++disconnectedEvents;
}

// The Secure Channel (System_BleSecureChannel.h): established per link id; an
// encrypted send is recorded with its binding and whether it is binary.
struct BleScBinding { uint16_t connId = 0; TransportSessionEpoch epoch = 0; uint32_t generation = 0; };
static std::set<uint16_t> scEstablished;
static bool scRequired = false;
static bool bleScRequired() { return scRequired; }
static void bleScReset(uint16_t connId) { scEstablished.erase(connId); }
static bool bleScEstablished(uint16_t connId) { return scEstablished.count(connId) != 0; }
static bool bleScBind(uint16_t connId, TransportSessionEpoch epoch, BleScBinding* out) {
  if (!epoch || !bleScEstablished(connId)) return false;
  if (out) *out = BleScBinding{connId, epoch, 1};
  return true;
}
struct GattSend { uint16_t connId; TransportSessionEpoch epoch; std::string text; bool encrypted, binary; };
static std::vector<GattSend> gattSends;
static unsigned gattUnderLock = 0;
static bool bleScSendEncryptedBound(const BleScBinding& b, const char* text, size_t len, bool,
                                    bool binaryFrame = false) {
  if (gLifecycle.depth) ++gattUnderLock;
  gattSends.push_back(GattSend{b.connId, b.epoch, std::string(text, len), true, binaryFrame});
  return true;
}
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch, const char* data,
                           size_t len, bool) {
  gattSends.push_back(GattSend{connId, expectedEpoch, std::string(data, len), false, false});
  return true;
}

// The pipe's link record and console activity (BLE_PhonePipe.h).
static bool recordValid = false;
static PhonePipeLinkRecord record;
static bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  if (out) *out = recordValid ? record : PhonePipeLinkRecord{};
  return recordValid;
}
static void phonePipeNoteConsoleActivity() {}

// OTA's frame sink and upload liveness (System_OTA.h).
struct OtaFrame { uint16_t connId; size_t len; uint32_t stampAtCall; };
static std::vector<OtaFrame> otaFrames;
static uint32_t activityStampOf(uint16_t connId);
static bool otaBleHandleEncryptedFrame(uint16_t connId, const uint8_t*, size_t len) {
  otaFrames.push_back(OtaFrame{connId, len, activityStampOf(connId)});
  return true;
}
static bool otaBleUploadActiveOnConnection(uint16_t) { return false; }

// The main loop around bleUpdateStreams: OTA housekeeping and design A's
// reconcile do nothing here; the session tick (test_ble_session_idle runs it)
// is counted; each data-stream sample is recorded with whether the lifecycle
// lock was held.
static void otaBleUploadHousekeeping() {}
static void bleConsoleReconcile() {}
static unsigned sessionTicks = 0;
static void bleSessionTick() { ++sessionTicks; }
static void buildSensorDataJSON(char* buf, size_t n) { std::snprintf(buf, n, "{}"); }
static void buildSystemStatusJSON(char* buf, size_t n) { std::snprintf(buf, n, "{}"); }
static unsigned sensorPushes = 0, systemPushes = 0, pushesUnderLock = 0;
static bool blePushSensorData(const char*, size_t) {
  ++sensorPushes;
  pushesUnderLock += gLifecycle.depth != 0;
  return true;
}
static bool blePushSystemStatus(const char*, size_t) {
  ++systemPushes;
  pushesUnderLock += gLifecycle.depth != 0;
  return true;
}

// Allocation.
enum class AllocPref { PreferPSRAM, RequireInternal };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::calloc(1, size); }
template <typename T>
static void ps_delete(T* object) {
  if (!object) return;
  object->~T();
  std::free(object);
}
// gBLEState through one access point (K1): its last holder frees it on its own
// task (`phone stop` gives the pipe's hold back). Armed, that release runs at
// the first read made without the lifecycle lock, as a preempting task would,
// and the read gets the pointer it had loaded: freed (ASan reports the use). A
// read under the lock is never preempted by it (the release waits for the lock).
BLESystemState* gBLEStateStorage = nullptr;
static BLESystemState*& bleStateAccess();
#define gBLEState (bleStateAccess())

// The command system around processBleCommandLine.
static constexpr size_t kPublicUsernameMaxLen = 64;  // System_User.h
static constexpr size_t kPublicPasswordMaxLen = 64;
static String redactCmdForAudit(const String& line) { return line; }
static std::set<std::string> admins, superadmins;
static bool isAdminUser(const String& user) { return admins.count(user.c_str()) != 0; }
static bool isSuperAdminUser(const String& user) { return superadmins.count(user.c_str()) != 0; }
class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    const std::string text = input.c_str();
    size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && text[i] == ' ') ++i;
      const size_t start = i;
      while (i < text.size() && text[i] != ' ') ++i;
      if (i > start) args_.emplace_back(text.substr(start, i - start));
    }
  }
  const String& arg(int index) const {
    static const String empty;
    return (index >= 0 && index < count()) ? args_[static_cast<size_t>(index)] : empty;
  }
  int count() const { return static_cast<int>(args_.size()); }
  bool unterminatedQuote() const { return false; }

 private:
  std::vector<String> args_;
};
typedef void (*DeferredFn)(void* arg);
static unsigned deferredJobs = 0;
static bool submitDeferredToCmdExec(DeferredFn, void* arg) {
  ++deferredJobs;
  std::free(arg);
  return true;
}
struct BleLoginAsyncJob;
static void bleLoginJobFree(BleLoginAsyncJob* job) { std::free(job); }
static void bleLoginDeferred(void* arg) { std::free(arg); }
static void bleCommandResultCallback(bool, const char*, void* reply) { std::free(reply); }
// What reached the executor: the line, its stamp and its session.
struct Queued { std::string line, user, sid, ip, path; TransportSessionEpoch epoch; uint8_t flags; };
static std::vector<Queued> queued;
static bool submitCommandAsync(const Command& cmd, ExecAsyncCallback, void* reply) {
  queued.push_back(Queued{cmd.line.c_str(), cmd.ctx.auth.user.c_str(), cmd.ctx.auth.sid.c_str(),
                          cmd.ctx.auth.ip.c_str(), cmd.ctx.auth.path.c_str(),
                          cmd.ctx.transportSessionEpoch, cmd.ctx.behaviorFlags});
  std::free(reply);
  return true;
}
// The running command's context (System_AuthIdentity's per-task slot).
static void* gCurrentContext = nullptr;
static void* currentCommandContext() { return gCurrentContext; }

// The command service on the client host (design A), for the disconnect owner.
struct BLEServer {
  unsigned closes = 0;
  void disconnect(uint16_t) { ++closes; }
};
static BLEServer gServer;
static BLEServer* pServer = nullptr;
#define ENABLE_OLED_DISPLAY 0

// The senders' public declarations (Bluetooth.h's defaults).
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);
bool sendBLEResponse(const char* data, size_t len, bool blocking = true);

// INSERT_PRODUCTION

static bool releaseArmed = false;
static unsigned unlockedStateReads = 0;
static BLESystemState* staleState = nullptr;
static BLESystemState*& bleStateAccess() {
  if (gLifecycle.depth == 0) {
    ++unlockedStateReads;
    // The reader keeps the pointer it loaded (a register copy) until the case
    // moves on (noUnlockedRead).
    if (staleState) return staleState;
    if (releaseArmed && gBLEStateStorage) {
      releaseArmed = false;
      staleState = gBLEStateStorage;
      blePipeConsoleStateRelease();  // the other task: takes the free lock, frees the state
      return staleState;
    }
  }
  return gBLEStateStorage;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static unsigned checks = 0, failures = 0;
static std::string currentCase = "setup";
#define EXPECT(condition, ...)                                        \
  do {                                                                \
    ++checks;                                                         \
    if (!(condition)) {                                               \
      ++failures;                                                     \
      std::printf("FAIL [%s]: ", currentCase.c_str());                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

static constexpr uint16_t kLink = 3;  // the phone link's Bluedroid id
static constexpr uint32_t kGen = 7;   // its pipe generation
static constexpr uint16_t kRow = blePipeConsoleRowId(kGen);

static uint32_t activityStampOf(uint16_t connId) {
  const int slot = findConnectionSlotByConnIdLocked(connId);
  return slot >= 0 ? gBLEState->connections[slot].lastActivityMs : 0;
}

// Pipe calls made under the lifecycle lock, over every case (the oracle).
static unsigned pipeCallsUnderLock = 0;
static void resetWorld(const std::string& name, hw1console::Mode mode = hw1console::Mode::Off) {
  for (const PipeHostCall& call : gPipeHost.calls) pipeCallsUnderLock += call.underLifecycle;
  currentCase = name;
  if (gBLEState) ps_delete(gBLEState);
  gBLEState = nullptr;
  sBleStateHolders = 0;
  sBlePipeRowSlot = -1;
  sBlePipeIdentity = BlePipeConsoleIdentity{};
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  liveSessions.clear();
  sessionOpens = sessionCloses = 0;
  connectedEvents = disconnectedEvents = 0;
  scEstablished.clear();
  scRequired = false;
  gattSends.clear();
  gattUnderLock = 0;
  queued.clear();
  otaFrames.clear();
  deferredJobs = 0;
  gPipeHost = PipeHost{};
  gSettings = Settings{};
  gOutputFlags = MSG_ROUTE_SERIAL;
  admins = {"bob", "alice"};
  superadmins = {"bob"};
  gCurrentContext = nullptr;
  recordValid = true;
  record = PhonePipeLinkRecord{kGen, kLink};
  nowMs = 1000;
  sBleConsoleMode.store(mode);
  sBleConsoleAdmissionOpen.store(mode == hw1console::Mode::ClientRegistered);
  sBleConsoleAttach = hw1console::Attach{};
  sBleConsoleLogged = hw1console::Slot{};
  sBleConsoleSlotConnId.store(-1);
  pServer = mode == hw1console::Mode::ClientRegistered ? &gServer : nullptr;
  gServer = BLEServer{};
}

static BlePipeConsoleIdentity identity() {
  BlePipeConsoleIdentity id;
  blePipeConsoleIdentity(&id);
  return id;
}
static TransportSessionEpoch epochOf(uint16_t connId) { return bleSessionEpochForConnection(connId); }
static void login(uint16_t connId, const char* user, bool mirror = true) {
  TransportSessionEpoch bound = 0;
  EXPECT(bleBindSession(connId, epochOf(connId), String(user), mirror, bound) && bound != 0,
         "conn %u logs in as %s", (unsigned)connId, user);
}
// One line as the session sent it (the console stream's is encrypted).
static void line(uint16_t connId, const char* text, bool encrypted) {
  processBleCommandLine(connId, epochOf(connId), text, std::strlen(text), encrypted);
}
static std::vector<std::string> pipeTexts() {
  std::vector<std::string> out;
  for (const PipeHostCall& call : pipeHostWrites()) {
    if (call.kind == PipeConsoleKind::Text && call.sent) out.push_back(call.data);
  }
  return out;
}
static bool lastPipeText(const char* text) {
  const std::vector<std::string> texts = pipeTexts();
  return !texts.empty() && texts.back() == text;
}
// The phone-client host with the pipe holding the state and a bound row.
static void boundRow(const std::string& name, hw1console::Mode mode = hw1console::Mode::Off) {
  resetWorld(name, mode);
  EXPECT(blePipeConsoleStateAcquire(), "the pipe holds the state");
  EXPECT(blePipeConsoleBind(kGen, 5, kLink), "the console row binds");
}

// A: bind, the snapshot through login, logout and end; rebind; a bind with no
// transport session left leaves nothing behind.
static void bindAndSnapshot() {
  boundRow("A1 bind");
  BlePipeConsoleIdentity id = identity();
  EXPECT(id.bound && id.generation == kGen && id.streamId == 5 && id.rowConnId == kRow &&
             id.epoch == epochOf(kRow) && id.epoch != 0 && !id.authed,
         "the snapshot does not name the fresh signed-out row");
  const BLEConnection& c = gBLEState->connections[findConnectionSlotByConnIdLocked(kRow)];
  EXPECT(c.pipeConsole && c.pipeLinkConnId == kLink && c.deviceType == BLE_DEVICE_PHONE &&
             !c.authed && c.user.length() == 0 && findConnectionSlotByConnIdLocked(kLink) < 0,
         "the row is not a signed-out phone row on its own id");
  EXPECT(isBLEConnected() && gBLEState->activeConnectionCount == 1 && connectedEvents == 1,
         "a bound row is not a connected app");
  login(kRow, "bob");
  id = identity();
  EXPECT(id.authed && id.epoch == epochOf(kRow), "the snapshot missed the login");
  line(kRow, "logout", true);
  id = identity();
  EXPECT(!id.authed && id.epoch == epochOf(kRow) && id.epoch != 0, "the snapshot missed the logout");
  EXPECT(lastPipeText("[ble] Logged out."), "the logout notice did not reach the stream");
  blePipeConsoleEnd(kGen, 5);
  id = identity();
  EXPECT(!id.bound && id.epoch == 0 && !isBLEConnected() && disconnectedEvents == 1 &&
             liveSessions.empty(),
         "End did not end the row, its session and its count");
  EXPECT(!bleSessionEpochIsLive(epochOf(kRow)) && findConnectionSlotByConnIdLocked(kRow) < 0,
         "the row outlived End");

  resetWorld("A2 no transport session left");
  EXPECT(blePipeConsoleStateAcquire(), "test setup: hold");
  for (size_t i = 0; i < kTransportSlots; ++i) transportSessionOpen(SOURCE_BLUETOOTH);
  EXPECT(!blePipeConsoleBind(kGen, 5, kLink), "a bind without a transport session succeeded");
  EXPECT(!identity().bound && findConnectionSlotByConnIdLocked(kRow) < 0 &&
             gBLEState->activeConnectionCount == 0 && connectedEvents == 0,
         "a refused bind left a row, a count or an event");

  resetWorld("A3 no hold, no row");
  EXPECT(!blePipeConsoleBind(kGen, 5, kLink), "a bind without the pipe's hold succeeded");
}

// B: the console row must log in, also with blerequireauth off; AuthBypass is
// never stamped on it; a server session keeps the setting's rule.
static void loginAlwaysRequired() {
  for (bool requireAuth : {true, false}) {
    boundRow(requireAuth ? "B1 auth on" : "B1 auth off");
    gSettings.bleRequireAuth = requireAuth;
    line(kRow, "whoami", true);
    EXPECT(lastPipeText("You are (unknown)"), "whoami on the signed-out row");
    line(kRow, "status", true);
    EXPECT(queued.empty() &&
               lastPipeText("Authentication required. Use: login <username> <password>"),
           "the signed-out row ran a command");
    AuthContext auth;
    TransportSessionEpoch epoch = 0;
    EXPECT(bleFillCommandAuth(kRow, auth, false, epoch) && auth.user.length() == 0 &&
               auth.ip == "ble:pipe-link" && auth.sid == String(unsigned(kRow)),
           "the stamp for the signed-out row is not empty on its own id (user '%s')",
           auth.user.c_str());
    login(kRow, "bob");
    line(kRow, "status", true);
    EXPECT(queued.size() == 1 && queued[0].user == "bob" && queued[0].sid == std::to_string(kRow) &&
               queued[0].ip == "ble:pipe-link" && queued[0].path == "/ble/cli" &&
               queued[0].epoch == epochOf(kRow) &&
               (queued[0].flags & COMMAND_CONTEXT_REQUIRE_LIVE_SESSION),
           "the signed-in row's command is not stamped with its user, id and session");
    line(kRow, "whoami", true);
    EXPECT(lastPipeText("You are bob (admin)"), "whoami on the signed-in row");
  }
  // A server session (plaintext, server mode) with blerequireauth off.
  resetWorld("B2 server session, auth off", hw1console::Mode::Server);
  gSettings.bleRequireAuth = false;
  gBLEState = bleNewState();
  {
    BleLifecycleGuard lifecycle;
    EXPECT(bleConsoleMakeSlotLocked(1), "test setup: a server session");
  }
  line(1, "status", false);
  EXPECT(queued.size() == 1 && queued[0].user == "AuthBypass",
         "a server session lost AuthBypass with blerequireauth off");
  // A blerequireauth toggle rotates the signed-out row's session and closes nothing.
  boundRow("B3 policy toggle");
  const TransportSessionEpoch before = epochOf(kRow);
  bleAuthPolicyChanged();
  EXPECT(identity().bound && epochOf(kRow) != before && identity().epoch == epochOf(kRow) &&
             pipeHostCloses().empty(),
         "a policy change closed the stream or left the snapshot behind");
}

// C: design A's slot and the console row on the same link, signed in as
// different users, never authorise each other.
static void noIdentityCrossing() {
  resetWorld("C coexisting", hw1console::Mode::ClientRegistered);
  gBLEState = bleNewState();
  sBleStateHolders = kBleHoldClientConsole;
  {
    BleLifecycleGuard lifecycle;
    bleConsoleReconcileLocked();
    bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, kLink));
  }
  EXPECT(sBleConsoleAttach.slot.present && sBleConsoleAttach.slot.connId == kLink,
         "test setup: design A's slot on the link");
  EXPECT(blePipeConsoleStateAcquire() && blePipeConsoleBind(kGen, 5, kLink),
         "the console row binds beside design A's slot");
  login(kLink, "alice");
  login(kRow, "bob");
  String user;
  EXPECT(bleGetAuthenticatedUser(kLink, user) && user == "alice", "design A's user");
  EXPECT(bleGetAuthenticatedUser(kRow, user) && user == "bob", "the row's user");
  line(kLink, "status", true);
  line(kRow, "status", true);
  EXPECT(queued.size() == 2 && queued[0].user == "alice" && queued[0].sid == "3" &&
             queued[1].user == "bob" && queued[1].sid == std::to_string(kRow) &&
             queued[0].epoch != queued[1].epoch,
         "the two sessions' commands crossed identities");

  // Confidential output and OTA: the row (the pipe's encryption) is; design
  // A's slot only with its own channel.
  EXPECT(bleSessionConfidential(kRow, epochOf(kRow)) && !bleSessionConfidential(kLink, epochOf(kLink)),
         "confidentiality crossed between the slot and the row");
  EXPECT(!bleSessionConfidential(kRow, epochOf(kRow) + 1000), "a stale epoch is confidential");
  CommandContext ota;
  ota.origin = ORIGIN_BLUETOOTH;
  ota.auth.transport = SOURCE_BLUETOOTH;
  char reason[96];
  uint16_t conn = 0;
  gCurrentContext = &ota;
  ota.auth.sid = String(unsigned(kRow));
  ota.transportSessionEpoch = epochOf(kRow);
  EXPECT(currentSecureBleConnection(conn, reason, sizeof(reason)) && conn == kRow,
         "otawrite refused the superadmin's console row");
  ota.auth.sid = "3";
  ota.transportSessionEpoch = epochOf(kLink);
  EXPECT(!currentSecureBleConnection(conn, reason, sizeof(reason)) &&
             std::strstr(reason, "encrypted Bluetooth channel"),
         "otawrite accepted design A's slot without its channel (the row's encryption crossed)");
  scEstablished.insert(kLink);
  EXPECT(!currentSecureBleConnection(conn, reason, sizeof(reason)) && std::strstr(reason, "superadmin"),
         "otawrite accepted alice (not superadmin) beside bob's console row");
  superadmins.insert("alice");
  EXPECT(currentSecureBleConnection(conn, reason, sizeof(reason)) && conn == kLink,
         "otawrite refused design A's superadmin with its channel");
  ota.auth.sid = String(unsigned(kRow));
  ota.transportSessionEpoch = epochOf(kRow) + 1000;
  EXPECT(!currentSecureBleConnection(conn, reason, sizeof(reason)),
         "otawrite accepted a renewed console session's command");
  gCurrentContext = nullptr;

  // Revocation: each user's own session only; the row's ends with CLOSE 5.
  EXPECT(bleRevokeUserSessions("alice") == 1, "alice's revocation");
  EXPECT(!bleGetAuthenticatedUser(kLink, user) && bleGetAuthenticatedUser(kRow, user) && user == "bob" &&
             pipeHostCloses().empty(),
         "revoking alice touched bob's console row");
  login(kLink, "alice");
  EXPECT(bleRevokeUserSessions("bob") == 1, "bob's revocation");
  const std::vector<PipeHostCall> closes = pipeHostCloses();
  EXPECT(!identity().bound && findConnectionSlotByConnIdLocked(kRow) < 0 &&
             bleGetAuthenticatedUser(kLink, user) && user == "alice",
         "revoking bob did not end his row, or touched alice");
  EXPECT(closes.size() == 1 && closes[0].reason == 5 && closes[0].generation == kGen &&
             closes[0].streamId == 5 && !closes[0].underLifecycle,
         "revoking the row did not ask for CLOSE 5 once, with the lifecycle lock released");
}

// D: bledisconnect ends the row and renews its stream (CLOSE 2); design A's
// slot and the row end separately; with no command service the row is the app.
static void disconnects() {
  resetWorld("D1 bledisconnect by id", hw1console::Mode::ClientRegistered);
  gBLEState = bleNewState();
  sBleStateHolders = kBleHoldClientConsole;
  {
    BleLifecycleGuard lifecycle;
    bleConsoleReconcileLocked();
    bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, kLink));
  }
  EXPECT(blePipeConsoleStateAcquire() && blePipeConsoleBind(kGen, 5, kLink), "test setup");
  login(kLink, "alice");
  login(kRow, "bob");
  EXPECT(bleDisconnectConnection(kRow) == BleDisconnectResult::SESSION_ENDED, "bledisconnect <row>");
  std::vector<PipeHostCall> closes = pipeHostCloses();
  String user;
  EXPECT(closes.size() == 1 && closes[0].reason == 2 && closes[0].streamId == 5 &&
             !closes[0].underLifecycle && !identity().bound &&
             bleGetAuthenticatedUser(kLink, user) && user == "alice" && gServer.closes == 0,
         "bledisconnect <row> did not end only the row and ask for CLOSE 2");
  EXPECT(blePipeConsoleBind(kGen, 6, kLink), "the renewed stream binds a fresh row");
  EXPECT(!bleGetAuthenticatedUser(kRow, user), "the renewed row inherited a sign-in");
  login(kRow, "bob");
  EXPECT(bleDisconnectConnection(kLink) == BleDisconnectResult::SESSION_ENDED &&
             pipeHostCloses().size() == 1 && identity().bound && bleGetAuthenticatedUser(kRow, user),
         "bledisconnect <link> touched the console row");
  EXPECT(bleDisconnectAppClients() >= 1 && pipeHostCloses().size() == 2 &&
             pipeHostCloses()[1].reason == 2 && pipeHostCloses()[1].streamId == 6 && !identity().bound,
         "bledisconnect (all) did not end the row with CLOSE 2");

  boundRow("D2 phone-client: no command service");
  EXPECT(bleDisconnectAppClients() == 1 && pipeHostCloses().size() == 1 && !identity().bound,
         "the console row is not the phone-client host's app");
  EXPECT(bleDisconnectConnection(kRow) == BleDisconnectResult::UNAVAILABLE,
         "an ended row was still found without a command service");
  boundRow("D3 phone-client: bledisconnect <row>");
  EXPECT(bleDisconnectConnection(kRow) == BleDisconnectResult::SESSION_ENDED &&
             pipeHostCloses().size() == 1,
         "bledisconnect <row> without a command service");
  boundRow("D4 revoke all");
  login(kRow, "bob");
  EXPECT(bleRevokeAllSessions() == 1 && pipeHostCloses().size() == 1 &&
             pipeHostCloses()[0].reason == 5 && !identity().bound,
         "revoking every session did not end the row with CLOSE 5");
}

// E: the console stream's records reach the command tail (as encrypted lines)
// and OTA, fenced on their session; nothing else enters through it.
static void recordIngress() {
  boundRow("E1 records");
  login(kRow, "bob");
  const TransportSessionEpoch epoch = epochOf(kRow);
  const char kFile[] = "fileread /notes.txt 0 64 bin";
  blePipeConsoleInbound(kRow, epoch, 0x81, reinterpret_cast<const uint8_t*>(kFile), sizeof(kFile) - 1);
  EXPECT(queued.size() == 1 && queued[0].line == kFile && queued[0].user == "bob",
         "a file verb from the console stream did not run as an encrypted line");
  EXPECT(gBLEState->commandsReceived == 1, "the record was not counted");
  nowMs += 5000;
  const uint8_t frame[20] = {0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01};
  blePipeConsoleInbound(kRow, epoch, 0x82, frame, sizeof(frame));
  EXPECT(otaFrames.size() == 1 && otaFrames[0].connId == kRow && otaFrames[0].len == sizeof(frame) &&
             otaFrames[0].stampAtCall == nowMs,
         "a BINARY record did not reach OTA after its activity stamp");
  blePipeConsoleInbound(kRow, epoch, 0x05, frame, sizeof(frame));
  blePipeConsoleInbound(kRow, epoch + 1000, 0x81, reinterpret_cast<const uint8_t*>("status"), 6);
  EXPECT(queued.size() == 1 && otaFrames.size() == 1, "an unknown type or a stale epoch got in");
  line(kRow, "logout", true);
  blePipeConsoleInbound(kRow, epoch, 0x81, reinterpret_cast<const uint8_t*>("status"), 6);
  EXPECT(queued.size() == 1, "a record admitted before the logout ran after it");

  resetWorld("E2 another row's id", hw1console::Mode::ClientRegistered);
  gBLEState = bleNewState();
  {
    BleLifecycleGuard lifecycle;
    bleConsoleReconcileLocked();
    bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, kLink));
  }
  login(kLink, "alice");
  blePipeConsoleInbound(kLink, epochOf(kLink), 0x81, reinterpret_cast<const uint8_t*>(kFile),
                        sizeof(kFile) - 1);
  EXPECT(queued.empty() && otaFrames.empty(),
         "a record named design A's slot and ran as an encrypted line");
}

// F: the output mapping on the console row; other rows keep their senders.
static void outputMapping() {
  boundRow("F1 TEXT");
  login(kRow, "bob");
  const TransportSessionEpoch epoch = epochOf(kRow);
  EXPECT(sendBLEResponseToSession(kRow, epoch, "OK", 2), "a result send");
  gPipeHost.onCmdExec = false;
  EXPECT(sendBLEResponseToSession(kRow, epoch, "late", 4), "a send off cmd_exec");
  gPipeHost.onCmdExec = true;
  EXPECT(sendBLEResponseToSession(kRow, epoch, "best effort", 11, /*blocking=*/false), "a best-effort send");
  std::vector<PipeHostCall> writes = pipeHostWrites();
  EXPECT(writes.size() == 3 && writes[0].kind == PipeConsoleKind::Text &&
             writes[0].mode == PipeConsoleMode::Reliable && writes[1].mode == PipeConsoleMode::TryNow &&
             writes[2].mode == PipeConsoleMode::TryNow && writes[0].epoch == epoch &&
             writes[0].generation == kGen && writes[0].streamId == 5,
         "TEXT is reliable only when blocking on cmd_exec");
  EXPECT(!sendBLEResponseToSession(kRow, epoch + 1000, "stale", 5) && pipeHostWrites().size() == 3,
         "a stale epoch reached the stream");
  EXPECT(gattSends.empty(), "console row output went out on GATT");

  gPipeHost = PipeHost{};
  const std::string mirror(1200, 'm');
  gOutputFlags |= MSG_ROUTE_BLE;
  EXPECT(sendBLEResponse(mirror.data(), mirror.size(), /*blocking=*/true), "a mirror buffer");
  writes = pipeHostWrites();
  EXPECT(writes.size() == 3 && writes[0].data.size() == 512 && writes[1].data.size() == 512 &&
             writes[2].data.size() == 176 && writes[0].kind == PipeConsoleKind::Mirror &&
             writes[0].mode == PipeConsoleMode::TryNow && writes[1].mode == PipeConsoleMode::TryNow &&
             writes[0].data + writes[1].data + writes[2].data == mirror,
         "a long mirror buffer did not go as TryNow MIRROR records of at most 512 bytes");
  gPipeHost = PipeHost{};
  gPipeHost.room = 1;
  EXPECT(sendBLEResponse(mirror.data(), mirror.size()) && pipeHostWrites().size() == 2 &&
             !pipeHostWrites()[1].sent,
         "a mirror that got its first record out is not delivered, or went on past a drop");

  gPipeHost = PipeHost{};
  EXPECT(blePushNotification(kRow, epoch, "sensor", "warn", "Temp", "High \"hot\"", 3000),
         "a notification card");
  writes = pipeHostWrites();
  EXPECT(writes.size() == 1 && writes[0].kind == PipeConsoleKind::Card &&
             writes[0].mode == PipeConsoleMode::TryNow &&
             writes[0].data ==
                 "{\"kind\":\"sensor\",\"level\":\"warn\",\"title\":\"Temp\",\"msg\":\"High \\\"hot\\\"\",\"ms\":3000}",
         "the card is not the bare JSON object as one TryNow CARD record (got '%s')",
         writes.empty() ? "" : writes[0].data.c_str());

  gPipeHost = PipeHost{};
  const uint8_t body[3] = {0x00, 0xFF, 0x10};
  EXPECT(bleSendBinaryToSession(kRow, epoch, body, sizeof(body)), "a BLOB body");
  writes = pipeHostWrites();
  EXPECT(writes.size() == 1 && writes[0].kind == PipeConsoleKind::Blob &&
             writes[0].mode == PipeConsoleMode::Reliable && writes[0].data == std::string("\x00\xFF\x10", 3) &&
             writes[0].epoch == epoch,
         "the binary body is not one reliable BLOB chain on the command's epoch");
  EXPECT(!bleSendBinaryToSession(kRow, epoch + 1000, body, sizeof(body)) && pipeHostWrites().size() == 1,
         "a BLOB for a renewed session went out");

  // Design A's slot: a card keeps its framing; a binary body needs its channel.
  resetWorld("F2 design A's senders", hw1console::Mode::ClientRegistered);
  gBLEState = bleNewState();
  {
    BleLifecycleGuard lifecycle;
    bleConsoleReconcileLocked();
    bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, kLink));
  }
  login(kLink, "alice");
  const TransportSessionEpoch linkEpoch = epochOf(kLink);
  EXPECT(!bleSendBinaryToSession(kLink, linkEpoch, body, sizeof(body)) && gattSends.empty(),
         "a binary body went out in plaintext");
  scEstablished.insert(kLink);
  EXPECT(blePushNotification(kLink, linkEpoch, "k", "info", "t", "m", 1) && gattSends.size() == 1 &&
             gattSends[0].encrypted && gattSends[0].text.rfind("#NOTIF {", 0) == 0 &&
             gattSends[0].text.back() == '\n',
         "design A's card lost its framing");
  EXPECT(bleSendBinaryToSession(kLink, linkEpoch, body, sizeof(body)) && gattSends.size() == 2 &&
             gattSends[1].binary && gattSends[1].epoch == linkEpoch,
         "design A's binary body is not one binary channel message on the command's epoch");
  EXPECT(pipeHostWrites().empty(), "design A's output reached the console stream");
}

// G: the main loop ends a row whose pipe session is no longer READY.
static void reconcile() {
  boundRow("G1 same session");
  blePipeConsoleReconcile();
  EXPECT(identity().bound, "reconcile ended the live session's row");
  record.generation = kGen + 1;
  blePipeConsoleReconcile();
  EXPECT(!identity().bound && pipeHostCloses().empty(), "a row of an ended session survived");
  boundRow("G2 no READY session");
  recordValid = false;
  blePipeConsoleReconcile();
  EXPECT(!identity().bound && liveSessions.empty(), "a row without a READY session survived");
}

// H: start/stop cycles with sign-ins return every transport session; a
// revoked or disconnected stream's next row starts signed out.
static void cycles() {
  resetWorld("H start/stop cycles");
  for (int cycle = 0; cycle < 30; ++cycle) {
    EXPECT(blePipeConsoleStateAcquire(), "a cycle's hold");
    for (uint16_t stream = 1; stream <= 3; ++stream) {
      EXPECT(blePipeConsoleBind(uint32_t(200 + cycle), stream, kLink), "a stream's row binds");
      login(blePipeConsoleRowId(uint32_t(200 + cycle)), "bob");
      if (stream == 2) bleRevokeAllSessions();
    }
    if (cycle % 3 == 0) blePipeHostTeardown();  // a host teardown (phone-client)
    blePipeConsoleStateRelease();
  }
  EXPECT(!gBLEState && sBleStateHolders == 0 && liveSessions.empty() &&
             sessionOpens == sessionCloses,
         "cycles leaked a transport session (%zu live)", liveSessions.size());
}

// K: every reader of gBLEState reads it under the lifecycle lock. The pipe holds
// the state in phone-client mode and `phone stop` frees it on another task;
// each reader runs with that release armed (it fires at an unlocked read).
static bool noUnlockedRead(const char* what) {
  const bool clean = unlockedStateReads == 0 && gBLEStateStorage;
  releaseArmed = false;
  staleState = nullptr;
  EXPECT(clean, "%s read gBLEState %u time(s) without the lifecycle lock (the release ran: %s)",
         what, unlockedStateReads, gBLEStateStorage ? "no" : "yes, the state was freed under it");
  return clean;
}
#define ARMED(call) (releaseArmed = true, unlockedStateReads = 0, (call))
static void stateReaders() {
  boundRow("K1 readers under the lock, phone-client mode");
  EXPECT(ARMED(isBLEConnected()), "the bound console row is a connected app");
  noUnlockedRead("isBLEConnected");
  EXPECT(!ARMED(isBleServerInitialized()), "the pipe's state is not the server's");
  noUnlockedRead("isBleServerInitialized");
  EXPECT(std::strcmp(ARMED(getBLEStateString()), "uninitialized") == 0, "no server state word");
  noUnlockedRead("getBLEStateString");
  ARMED(bleEnableStream(BLE_STREAM_SENSORS | BLE_STREAM_SYSTEM));
  noUnlockedRead("bleEnableStream");
  ARMED(bleSetStreamInterval(1000, 1000));
  noUnlockedRead("bleSetStreamInterval");
  EXPECT(ARMED(bleIsStreamEnabled(BLE_STREAM_SENSORS)), "the stream flag reads back");
  noUnlockedRead("bleIsStreamEnabled");
  // The main loop: the row's session is maintained (its idle logout, which
  // leaves the stream and the link alone), and no stream is sampled without
  // the server's data service, even with its flags set.
  nowMs = 5000;
  ARMED(bleUpdateStreams());
  noUnlockedRead("bleUpdateStreams");
  EXPECT(sessionTicks == 1 && sensorPushes == 0 && systemPushes == 0 && pipeHostCloses().empty(),
         "phone-client main loop: %u tick(s), %u+%u stream push(es), %zu close request(s)",
         sessionTicks, sensorPushes, systemPushes, pipeHostCloses().size());
  ARMED(bleDisableStream(BLE_STREAM_SYSTEM));
  noUnlockedRead("bleDisableStream");
  releaseArmed = false;
  // `phone stop` now runs: the state goes, and every reader sees none.
  blePipeConsoleStateRelease();
  EXPECT(!gBLEStateStorage && !isBLEConnected() && !isBleServerInitialized() &&
             !bleIsStreamEnabled(BLE_STREAM_SENSORS),
         "the released state is still read");
  bleUpdateStreams();
  EXPECT(sessionTicks == 1, "the main loop ticked a session with no state");

  // The server role's state: its data service samples the due streams, each
  // sent with the lifecycle lock released, and the next one waits its interval.
  resetWorld("K2 the server's streams");
  sessionTicks = sensorPushes = systemPushes = pushesUnderLock = 0;
  gBLEState = bleNewState();
  sBleStateHolders = kBleHoldServer;
  gBLEState->initialized = true;
  gBLEState->connectionState = BLE_STATE_CONNECTED;
  bleEnableStream(BLE_STREAM_SENSORS | BLE_STREAM_SYSTEM);
  nowMs = 6000;
  unlockedStateReads = 0;
  bleUpdateStreams();
  EXPECT(sensorPushes == 1 && systemPushes == 1 && pushesUnderLock == 0 && unlockedStateReads == 0 &&
             gBLEState->lastSensorStream == 6000 && gBLEState->lastSystemStream == 6000,
         "server streams: %u+%u push(es), %u under the lock", sensorPushes, systemPushes,
         pushesUnderLock);
  nowMs = 7000;
  bleUpdateStreams();
  EXPECT(sensorPushes == 2 && systemPushes == 1 && sessionTicks == 2,
         "the sensor interval (1 s) is due again, the system one (5 s) is not");
  sBleStateHolders = 0;
}

// The lock-order oracle over every case: the pipe was never called with the
// lifecycle lock held, and every lock was released.
static void lockOrder(const char* after) {
  for (const PipeHostCall& call : gPipeHost.calls) {
    EXPECT(!call.underLifecycle, "%s: a pipe call (%c) ran under the lifecycle lock", after, call.op);
  }
  EXPECT(pipeCallsUnderLock == 0, "%s: %u pipe call(s) ran under the lifecycle lock", after,
         pipeCallsUnderLock);
  EXPECT(gLifecycle.depth == 0, "%s: the lifecycle lock leaked", after);
  EXPECT(gattUnderLock == 0, "%s: a paced send ran under the lifecycle lock", after);
}

int main() {
  bindAndSnapshot();
  lockOrder("bind");
  loginAlwaysRequired();
  lockOrder("auth");
  noIdentityCrossing();
  lockOrder("identity");
  disconnects();
  lockOrder("disconnect");
  recordIngress();
  lockOrder("ingress");
  outputMapping();
  lockOrder("output");
  reconcile();
  lockOrder("reconcile");
  cycles();
  lockOrder("cycles");
  stateReaders();
  lockOrder("state readers");
  if (gBLEState) ps_delete(gBLEState);
  std::printf("BLE console row: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
