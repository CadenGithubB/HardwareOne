// A Bluetooth command's own broadcastOutput lines, as shipped:
// test_ble_command_output.py inserts System_Debug.cpp's broadcastOutputCore and
// its public overloads, System_CLI.cpp's help menu (renderHelpMain), and
// Bluetooth.cpp's command tail (processBleCommandLine), reply callback, output
// batch, session senders, central TX gate class, console-mirror collector and
// the slot and session helpers both console hosts use. cmd_exec, the debug
// queue, the Secure Channel, the central TX gate, IDF GATTS and transport
// sessions are host boundaries that record what was done.
//
// `help` renders its menu with broadcastOutput and returns "OK". The session
// that sent it must get the menu, then "OK": in server mode and on the phone
// pipe's link (G2 client host) alike, and the same text in both.
#include "Arduino.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <strings.h>
#include <vector>

void webBatchTestStringAppend() {}

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

// The recursive lifecycle mutex: depth only (one task at a time here).
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
using TaskHandle_t = void*;
static int cmdExecTask;
static bool noCurrentTask = false;  // before the scheduler runs: no task handle
static TaskHandle_t xTaskGetCurrentTaskHandle() { return noCurrentTask ? nullptr : &cmdExecTask; }
static TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex) {
  return mutex->depth > 0 ? xTaskGetCurrentTaskHandle() : nullptr;
}
// Send-path faults the doubles below record (checked by every case).
static std::vector<std::string> pathFaults;

#define ENABLE_OLED_DISPLAY 0
#define ENABLE_ESPNOW 0
#define ENABLE_BLUETOOTH 1
#define DEBUG_BLE_CORE 1
#define DEBUG_BLE_GATT 2
#define DEBUG_BLE_DATA 3
#define DEBUG_MSG_SIZE 256
using DebugFlagMask = uint32_t;
// Bluetooth.cpp's BLE_DEBUGF: its lines go through broadcastOutput, so on
// cmd_exec they reach the batch, including from helpers that hold the lock.
static bool coreDebug = false;  // `debugbluetoothcore`
static bool isDebugFlagSet(DebugFlagMask flag) { return coreDebug && flag == DEBUG_BLE_CORE; }
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }
// INSERT_BLE_DEBUGF
static volatile uint32_t gOutputFlags = MSG_ROUTE_SERIAL;  // the boot default
static bool dataDebug = false;  // `debugbluetoothdata`
static bool bleDataDebugEnabled() { return dataDebug; }

// The debug queue: every enqueued line with its route (the drain's input).
struct Queued { std::string text; uint8_t route; };
static std::vector<Queued> queue;
static bool enqueueChunk(const char* text, size_t len, uint8_t routing, DebugFlagMask, bool) {
  queue.push_back(Queued{std::string(text, len), routing});
  return true;
}

// Tasks and the per-task command context (System_AuthIdentity's TLS slot).
bool gThermalRunning = false, gImuRunning = false, gTofRunning = false;
TaskHandle_t gThermalTaskHandle = nullptr, gImuTaskHandle = nullptr, gTofTaskHandle = nullptr;
static void* gCurrentContext = nullptr;
static void* currentCommandContext() { return gCurrentContext; }
static CaptureBufState* currentCaptureState() { return nullptr; }
static volatile bool gInHelpRender = false;

// Transport sessions: never-repeating epochs.
static TransportSessionEpoch nextEpoch = 100;
static TransportSessionEpoch transportSessionOpen(CommandSource) { return ++nextEpoch; }
static void transportSessionClose(CommandSource, TransportSessionEpoch) {}

// The Secure Channel: established per link. An encrypted message is one frame
// ("\x10enc:" + plaintext) through the shipped directed sender.
struct BleScBinding { uint16_t connId = 0; TransportSessionEpoch epoch = 0; uint32_t generation = 0; };
static bool scRequired = false;
static std::set<uint16_t> scEstablished;
static bool bleScRequired() { return scRequired; }
static void bleScReset(uint16_t connId) { scEstablished.erase(connId); }
static bool bleScEstablished(uint16_t connId) { return scEstablished.count(connId) != 0; }
static bool bleScBind(uint16_t connId, TransportSessionEpoch epoch, BleScBinding* out) {
  if (!epoch || !bleScEstablished(connId)) return false;
  if (out) *out = BleScBinding{connId, epoch, 1};
  return true;
}
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len, bool blocking);
static const std::string kEncrypted = std::string("\x10") + "enc:";
static bool bleScSendEncryptedBound(const BleScBinding& binding, const char* text, size_t len,
                                    bool blocking, bool = false) {
  if (gLifecycle.depth != 0) {
    pathFaults.push_back("the paced Secure Channel sender ran under the lifecycle lock");
  }
  const std::string frame = kEncrypted + std::string(text, len);
  return bleRawNotifyToSession(binding.connId, binding.epoch, frame.data(), frame.size(), blocking);
}

// The shared central TX gate and the pipe's activity note (client host).
static int gateHeld = 0;
static unsigned gateTries = 0, gateReliableTries = 0;
static bool bleCentralTxTryPhone(bool bypassPriority) {
  ++gateTries;
  if (bypassPriority) ++gateReliableTries;
  ++gateHeld;
  return true;
}
static void bleCentralTxGive() { --gateHeld; }
static void phonePipeNoteConsoleActivity() {}

enum HostSystemEvent : uint8_t { SYSEVT_BLE_CONNECTED = 1, SYSEVT_BLE_DISCONNECTED };
static void systemEventPost(uint8_t, const char* = nullptr, const char* = nullptr) {}

// IDF GATTS: each post lands in its connection's inbox. Server mode posts under
// the lifecycle lock; the client host with it released, gated and counted in
// flight (BLE_ConsoleAttachCore.h, test_ble_console_phone_link).
#define ESP_GATT_IF_NONE 0xff
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
using esp_gatt_if_t = uint8_t;
static std::atomic<int> sBleGattsIf{3};
struct BLECharacteristic { uint16_t getHandle() const { return 0x2a; } };
static BLECharacteristic gResponse;
static BLECharacteristic* pCmdResponseChar = &gResponse;
static std::map<uint16_t, std::vector<std::string>> inbox;  // per connection, in order
static int postFailures = 0;  // the next posts IDF refuses (a congested link), then it takes them
static esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t, uint16_t connId, uint16_t,
                                             size_t len, uint8_t* value, bool needConfirm);

// Allocation: never zeroed, as on the device.
enum class AllocPref { PreferPSRAM, RequireInternal };
static void* ps_alloc(size_t size, AllocPref, const char*) {
  void* p = std::malloc(size);
  if (p) std::memset(p, 0xA5, size);
  return p;
}
template <typename T>
static void ps_delete(T* object) {
  if (!object) return;
  object->~T();
  std::free(object);
}
BLESystemState* gBLEState = nullptr;

// The phone pipe's link record and the lifecycle mutex the pipe's hold
// creates (the phone link's console row, phone link v2); the leaf lock of its
// identity snapshot (one task here).
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
static bool pipeRecordValid = false;
static PhonePipeLinkRecord pipeRecord;
static bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  if (out) *out = pipeRecordValid ? pipeRecord : PhonePipeLinkRecord{};
  return pipeRecordValid;
}
static bool bleInitLifecycleMutex() { return true; }

// The command system around processBleCommandLine.
static struct Settings { bool bleRequireAuth = true; } gSettings;
static constexpr size_t kPublicUsernameMaxLen = 64;  // System_User.h
static constexpr size_t kPublicPasswordMaxLen = 64;
static String redactCmdForAudit(const String& line) { return line; }
static bool isAdminUser(const String&) { return true; }
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
static bool submitDeferredToCmdExec(DeferredFn, void*) { return false; }
struct BleLoginAsyncJob;
static void bleLoginJobFree(BleLoginAsyncJob* job) { std::free(job); }
static void bleLoginDeferred(void* arg) { bleLoginJobFree(static_cast<BleLoginAsyncJob*>(arg)); }

// cmd_exec: queued requests run later, one at a time, with the request's
// context installed for the calling task (commandExecTask).
struct ExecRequest { Command cmd; ExecAsyncCallback callback; void* userData; };
static std::vector<ExecRequest> execQueue;
static bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData) {
  execQueue.push_back(ExecRequest{cmd, callback, userData});
  return true;
}

// The command registry the harness runs (handlers declared after the production code).
static const char* runRegistry(const char* line);
static void runCmdExec() {
  while (!execQueue.empty()) {
    ExecRequest request = execQueue.front();
    execQueue.erase(execQueue.begin());
    gCurrentContext = &request.cmd.ctx;
    const char* out = runRegistry(request.cmd.line.c_str());
    gCurrentContext = nullptr;
    request.callback(true, out, request.userData);
  }
}

// The command modules the help menu lists.
struct CommandEntry;
static bool connected() { return true; }
static bool notConnected() { return false; }
static std::vector<CommandModule> modules;
static const CommandModule* getCommandModules(size_t& count) {
  count = modules.size();
  return modules.data();
}

void bleCommandOutputLine(const char* text, size_t len);
void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);
TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId);
static bool bleIsAuthed(uint16_t connId, String& outUser);
static bool bleRequiresLogin(uint16_t connId);

// Bluetooth.cpp's bleFillCommandAuth, but the session id is set through
// std::string (the host String has no integer constructor).
static bool bleFillCommandAuth(uint16_t connId, AuthContext& auth, bool forLogin,
                               TransportSessionEpoch& epochOut) {
  epochOut = bleSessionEpochForConnection(connId);
  if (epochOut == 0) return false;
  auth.transport = SOURCE_BLUETOOTH;
  auth.path = forLogin ? "/ble/login" : "/ble/cli";
  auth.sid = String(std::to_string(connId));
  auth.opaque = nullptr;
  String user;
  if (forLogin) auth.user = "";
  else if (bleIsAuthed(connId, user)) auth.user = user;
  else auth.user = bleRequiresLogin(connId) ? "" : "AuthBypass";
  return true;
}

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// The GATT post (needs the production console state)
// ---------------------------------------------------------------------------
static esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t gattsIf, uint16_t connId, uint16_t,
                                             size_t len, uint8_t* value, bool needConfirm) {
  if (gattsIf == ESP_GATT_IF_NONE || needConfirm) return ESP_FAIL;
  if (sBleConsoleMode.load() == hw1console::Mode::ClientRegistered) {
    if (gLifecycle.depth != 0 || sBleConsoleSendsInFlight.load() != 1 || gateHeld != 1) {
      pathFaults.push_back("a phone-link frame was posted under the lock, uncounted or ungated");
    }
  } else if (gLifecycle.depth == 0) {
    pathFaults.push_back("a server-mode frame was posted outside the lifecycle lock");
  }
  if (postFailures > 0) {  // no room on the link this time: nothing lands
    --postFailures;
    return ESP_FAIL;
  }
  inbox[connId].emplace_back(reinterpret_cast<const char*>(value), len);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
// `help` and `help all` as cmd_help runs them for a mode-independent caller
// (Bluetooth): the menu goes out through broadcastOutput and the result is "OK".
static const char* helpLike(bool showAll) {
  gInHelpRender = true;
  renderHelpMain(showAll);
  gInHelpRender = false;
  return "OK";
}
static uint16_t logoutDuringCommand = 0xffff;
static uint16_t lookupDuringCommand = 0xffff;
static size_t writesWhileHeld = 0;
static const char* runRegistry(const char* line) {
  const std::string command = line;
  if (command == "help") return helpLike(false);
  if (command == "help all") return helpLike(true);
  if (command == "status json") return "{\"uptime\":1,\"ok\":true}";
  if (command == "lines") {
    broadcastOutput("first line");
    broadcastOutput(String("second line"));
    BROADCAST_PRINTF("third line %d", 3);
    return "OK: three lines";
  }
  // Four batches' worth: each line fills a batch, so the next one flushes it.
  if (command == "batches") {
    for (char c : {'a', 'b', 'c', 'd'}) broadcastOutput(std::string(450, c).c_str());
    return "OK";
  }
  if (command == "long") {
    broadcastOutput(std::string(700, 'x').c_str());
    broadcastOutput("after");
    return "OK";
  }
  if (command == "locked lines") {
    String user;
    (void)bleIsAuthed(lookupDuringCommand, user);    // under the lock, fits
    broadcastOutput(std::string(450, 'f').c_str());
    (void)bleIsAuthed(lookupDuringCommand, user);    // under the lock, does not fit
    broadcastOutput("after");                        // fits
    return "OK";
  }
  if (command == "help then logout") {
    helpLike(false);
    (void)bleLogout(logoutDuringCommand);  // the session ends before the result
    return "OK";
  }
  // An owner command printing while this task holds the pipe owner mutex, or
  // the console chain mutex: the second line would need a send.
  if (command == "owner lines" || command == "chain lines") {
    bool& held = command == "owner lines" ? gPipeHost.ownerHeld : gPipeHost.chainHeld;
    held = true;
    const size_t before = gPipeHost.calls.size();
    broadcastOutput(std::string(450, 'o').c_str());
    broadcastOutput(std::string(450, 'p').c_str());
    writesWhileHeld += gPipeHost.calls.size() - before;
    held = false;
    broadcastOutput("after");
    return "OK";
  }
  return "Error: unknown command";
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static int failures = 0;
#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                       \
      std::fprintf(stderr, "\n");                              \
      ++failures;                                              \
    }                                                          \
  } while (0)

static void makeModules() {
  static const char* const kNames[] = {
      "wifi", "espnow", "c6", "mqtt", "bluetooth", "phone", "relay", "filesystem", "sd",
      "oled", "neopixel", "led", "i2c", "automation", "battery", "debug", "settings",
      "sensorlog", "health", "users", "features", "image", "map", "power", "ota", "ring",
      "llm", "cm5"};
  static const char* const kSensors[] = {"thermal", "tof", "imu", "gps", "rtc", "camera",
                                         "microphone", "presence"};
  modules.clear();
  modules.push_back(CommandModule{"cli", "Help and CLI navigation", nullptr, nullptr, nullptr,
                                  CMD_MODULE_CORE, nullptr});
  for (const char* name : kNames) {
    modules.push_back(CommandModule{name, "Network, storage and device management commands",
                                    nullptr, nullptr, nullptr, 0, nullptr});
  }
  bool odd = false;
  for (const char* name : kSensors) {
    modules.push_back(CommandModule{name, "Sensor driver, start/stop and readings", nullptr,
                                    nullptr, nullptr, CMD_MODULE_SENSOR,
                                    (odd = !odd) ? connected : notConnected});
  }
}

static void resetWorld(hw1console::Mode mode) {
  if (gBLEState) {
    gBLEState->~BLESystemState();
    std::free(gBLEState);
  }
  gBLEState = bleNewState();
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  sBleConsoleMode.store(mode);
  scRequired = false;
  scEstablished.clear();
  inbox.clear();
  queue.clear();
  execQueue.clear();
  pathFaults.clear();
  postFailures = 0;
  gOutputFlags = MSG_ROUTE_SERIAL;
  gSettings.bleRequireAuth = true;
  dataDebug = false;
  gateHeld = 0;
  gateTries = gateReliableTries = 0;
  logoutDuringCommand = 0xffff;
  lookupDuringCommand = 0xffff;
  coreDebug = false;
  // The phone link's console row and the console driver's doubles.
  sBleStateHolders = 0;
  sBlePipeRowSlot = -1;
  sBlePipeIdentity = BlePipeConsoleIdentity{};
  gPipeHost = PipeHost{};
  writesWhileHeld = 0;
  pipeRecordValid = false;
}

// An app session: the slot (the phone link's maker; server mode's onConnect
// fills the same fields), the app's first write joining it, a Secure Channel
// when asked, and a login as the BLE login job binds it.
static void connectApp(uint16_t connId, const char* user, bool mirrorEligible, bool secure) {
  BleLifecycleGuard lifecycle;
  CHECK(bleConsoleMakeSlotLocked(connId), "the slot for conn %u is made", (unsigned)connId);
  bleConsoleJoinLocked(connId);
  if (secure) scEstablished.insert(connId);
  TransportSessionEpoch bound = 0;
  CHECK(bleBindSession(connId, bleSessionEpochForConnection(connId), String(user), mirrorEligible,
                       bound) && bound != 0,
        "conn %u logs in", (unsigned)connId);
}

// The app writes a command line: the command tail runs (cmd_exec), then cmd_exec
// runs the command and the reply callback.
static void write(uint16_t connId, const char* line, bool encrypted) {
  processBleCommandLine(connId, bleSessionEpochForConnection(connId), line, std::strlen(line),
                        encrypted);
  runCmdExec();
}

// What a connection received, as plaintext messages in order.
static std::vector<std::string> received(uint16_t connId) {
  std::vector<std::string> messages;
  for (const std::string& raw : inbox[connId]) {
    messages.push_back(raw.compare(0, kEncrypted.size(), kEncrypted) == 0
                           ? raw.substr(kEncrypted.size()) : raw);
  }
  return messages;
}

// The lines the command put on the debug queue for Bluetooth, each with '\n'.
static std::string bluetoothLines() {
  std::string text;
  for (const Queued& line : queue) {
    if (line.route & MSG_ROUTE_BLE) text += line.text + "\n";
  }
  return text;
}

static std::string joinedExceptLast(const std::vector<std::string>& messages) {
  std::string text;
  for (size_t i = 0; i + 1 < messages.size(); ++i) text += messages[i];
  return text;
}

// Every batch is whole lines, at most one plaintext notification, in order.
static void checkBatches(const char* label, const std::vector<std::string>& messages) {
  for (size_t i = 0; i + 1 < messages.size(); ++i) {
    CHECK(!messages[i].empty() && messages[i].size() <= 512 && messages[i].back() == '\n',
          "%s: batch %zu is %zu bytes and ends a line", label, i, messages[i].size());
  }
}

static std::vector<std::string> helpOn(hw1console::Mode mode, const char* command) {
  resetWorld(mode);
  const uint16_t app = mode == hw1console::Mode::ClientRegistered ? 3 : 0;
  connectApp(app, "admin", true, true);
  write(app, command, true);
  return received(app);
}

// Server mode: the session that sent `help` gets the menu, then "OK"; another
// signed-in session gets nothing; the debug queue keeps every line (the mirror
// and the other sinks are unchanged).
static void serverModeHelpReachesItsSession() {
  resetWorld(hw1console::Mode::Server);
  scRequired = true;
  connectApp(0, "admin", true, true);
  connectApp(1, "other", true, true);
  write(0, "help", true);
  const std::vector<std::string> got = received(0);
  const std::string menu = bluetoothLines();
  CHECK(menu.find("CLI Help Menu") != std::string::npos &&
            menu.find("Available Modules:") != std::string::npos &&
            menu.find("Not active:") != std::string::npos,
        "the help menu was rendered through broadcastOutput");
  CHECK(got.size() >= 2 && got.back() == "OK",
        "server mode: `help` answered %zu message(s), last '%s' (want the menu, then OK)",
        got.size(), got.empty() ? "" : got.back().c_str());
  CHECK(joinedExceptLast(got) == menu,
        "server mode: the session received %zu of the menu's %zu bytes before OK",
        joinedExceptLast(got).size(), menu.size());
  checkBatches("server help", got);
  CHECK(received(1).empty(), "another signed-in session received %zu message(s)",
        received(1).size());
  for (const Queued& line : queue) {
    CHECK((line.route & MSG_ROUTE_BLE) && (line.route & MSG_ROUTE_FILE),
          "a help line kept its command route (file and the Bluetooth mirror)");
  }
  CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
}

// The phone pipe's link (G2 client host): the same messages as server mode,
// each frame on the shared gate as a reliable send, posted with the lock released.
static void phoneLinkHelpMatchesServerMode() {
  for (const char* command : {"help", "help all"}) {
    const std::vector<std::string> server = helpOn(hw1console::Mode::Server, command);
    const std::vector<std::string> phone = helpOn(hw1console::Mode::ClientRegistered, command);
    CHECK(phone.size() >= 2 && phone.back() == "OK" && joinedExceptLast(phone) == bluetoothLines(),
          "phone link: `%s` answered %zu message(s) with %zu menu bytes (want the menu, then OK)",
          command, phone.size(), joinedExceptLast(phone).size());
    CHECK(phone == server, "phone link: `%s` differs from server mode (%zu vs %zu messages)",
          command, phone.size(), server.size());
    checkBatches(command, phone);
    CHECK(gateTries >= phone.size() && gateReliableTries == gateTries && gateHeld == 0,
          "phone link: every frame took one reliable gate try (%u tries, %u reliable)",
          gateTries, gateReliableTries);
    CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
  }
  const std::vector<std::string> all = helpOn(hw1console::Mode::ClientRegistered, "help all");
  CHECK(all.size() >= 4, "`help all` (%zu bytes) needs several batches, got %zu message(s)",
        joinedExceptLast(all).size(), all.size());
}

// A command that returns its result and prints nothing (JSON) is unchanged;
// lines with a non-trivial result come first and the result stays as returned.
static void resultsStayAsReturned() {
  for (hw1console::Mode mode : {hw1console::Mode::Server, hw1console::Mode::ClientRegistered}) {
    resetWorld(mode);
    const uint16_t app = 3;
    connectApp(app, "admin", true, true);
    write(app, "status json", true);
    std::vector<std::string> got = received(app);
    CHECK(got.size() == 1 && got[0] == "{\"uptime\":1,\"ok\":true}",
          "a JSON result is one byte-exact message (got %zu)", got.size());
    write(app, "lines", true);
    got = received(app);
    CHECK(got.size() == 3 && got[1] == "first line\nsecond line\nthird line 3\n" &&
              got[2] == "OK: three lines",
          "the lines, then the result as returned (got %zu message(s))", got.size());
    write(app, "long", true);
    got = received(app);
    CHECK(got.size() == 6 && got[3] == std::string(506, 'x') + "[CUT]\n" &&
              got[4] == "after\n" && got[5] == "OK",
          "an over-long line keeps its first 506 bytes and a [CUT] mark in its own batch "
          "(got %zu, batch '%.12s...%s')", got.size(), got.size() > 3 ? got[3].c_str() : "",
          got.size() > 3 && got[3].size() > 8 ? got[3].substr(got[3].size() - 8).c_str() : "");
  }
}

// With `outble` on, a mirror recipient takes the lines from the mirror only, so
// it never gets them twice; a session the mirror skips still gets them.
static void mirrorRecipientsAreNotSentTwice() {
  resetWorld(hw1console::Mode::Server);
  connectApp(0, "admin", true, true);
  connectApp(1, "viewer", false, true);  // not a mirror recipient
  gOutputFlags |= MSG_ROUTE_BLE;         // `outble 1`
  write(0, "lines", true);
  std::vector<std::string> got = received(0);
  CHECK(got.size() == 1 && got[0] == "OK: three lines",
        "a mirror recipient got the lines directly too (%zu messages)", got.size());
  CHECK(bluetoothLines() == "first line\nsecond line\nthird line 3\n",
        "the lines stay on the queue for the mirror");
  write(1, "lines", true);
  got = received(1);
  CHECK(got.size() == 2 && got[0] == "first line\nsecond line\nthird line 3\n",
        "a session the mirror skips got %zu message(s)", got.size());
  gOutputFlags &= ~MSG_ROUTE_BLE;        // `outble 0`
  inbox.clear();
  write(0, "lines", true);
  CHECK(received(0).size() == 2, "with the mirror off the recipient gets the lines directly");
}

// Plaintext server session: each batch is one directed notification of at most 512 bytes.
static void plaintextServerSession() {
  resetWorld(hw1console::Mode::Server);
  connectApp(0, "admin", true, false);
  write(0, "help all", false);
  const std::vector<std::string> got = received(0);
  CHECK(got.size() >= 4 && got.back() == "OK" && joinedExceptLast(got) == bluetoothLines(),
        "plaintext: `help all` answered %zu message(s)", got.size());
  for (const std::string& raw : inbox[0]) {
    CHECK(raw.compare(0, kEncrypted.size(), kEncrypted) != 0 && raw.size() <= 512,
          "plaintext: a %zu-byte notification", raw.size());
  }
  CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
}

// A session that ends during the command gets nothing more: the batch and the
// result are fenced on the session that sent the command. Lines from outside a
// Bluetooth command never reach a session.
static void fencesAndOtherCallers() {
  for (hw1console::Mode mode : {hw1console::Mode::Server, hw1console::Mode::ClientRegistered}) {
    resetWorld(mode);
    const uint16_t app = 3;
    connectApp(app, "admin", true, true);
    logoutDuringCommand = app;
    write(app, "help then logout", true);
    const std::string menu = bluetoothLines();
    std::string got;
    for (const std::string& message : received(app)) got += message;
    CHECK(got.size() < menu.size() && got.find("OK") == std::string::npos,
          "after the session ended its batch or result still went out (%zu bytes)", got.size());
    CHECK(menu.compare(0, got.size(), got) == 0, "what went out before the logout is the menu's start");

    inbox.clear();
    broadcastOutput("background line");                  // no command context
    Command serial;
    serial.ctx.origin = ORIGIN_SERIAL;
    serial.ctx.outputMask = MSG_ROUTE_SERIAL | MSG_ROUTE_BLE;
    gCurrentContext = &serial.ctx;
    broadcastOutput("serial command line");              // another transport's command
    gCurrentContext = nullptr;
    CHECK(inbox.empty(), "a line from outside a Bluetooth command reached a session");
  }
}

// A line printed while cmd_exec holds the lifecycle lock (BLE_DEBUGF in a
// locked helper such as bleIsAuthed, with `debugbluetoothcore` on) joins the
// batch when it fits; when it would need a send, the batch is not sent under
// the lock (the paced sender and the phone link's post need it free) and the
// line is left to the other sinks.
static void linesUnderTheLockAreNotSentUnderIt() {
  for (hw1console::Mode mode : {hw1console::Mode::Server, hw1console::Mode::ClientRegistered}) {
    resetWorld(mode);
    const uint16_t app = 3;
    connectApp(app, "admin", true, true);
    coreDebug = true;
    lookupDuringCommand = app;
    write(app, "locked lines", true);
    const std::string lookup = "[BLE] Session authed lookup conn=3 user='admin'\n";
    const std::string fill = std::string(450, 'f') + "\n";
    CHECK(lookup.size() * 2 + fill.size() > 512 && lookup.size() + fill.size() + 6 <= 512,
          "the case's second lookup needs a send and its last line does not");
    const std::vector<std::string> got = received(app);
    CHECK(got.size() == 2 && got[0] == lookup + fill + "after\n" && got[1] == "OK",
          "%s: the lines that fit, then OK (got %zu message(s), first %zu bytes)",
          mode == hw1console::Mode::Server ? "server mode" : "phone link", got.size(),
          got.empty() ? 0 : got[0].size());
    size_t queued = 0;
    for (const Queued& line : queue) {
      // The command's own lines (its route, without serial; the command tail's
      // lookup before cmd_exec has none).
      if (line.text + "\n" == lookup && (line.route & MSG_ROUTE_FILE) &&
          !(line.route & MSG_ROUTE_SERIAL)) {
        ++queued;
      }
    }
    CHECK(queued == 2, "both of the command's lookups stay on the debug queue for the other "
                       "sinks (%zu)", queued);
    CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
  }
}

// The send traces (`debugbluetoothdata`), which the batch's own sends make while
// the command still runs, stay off Bluetooth: they never join the batch.
static void tracesStayOffTheBatch() {
  resetWorld(hw1console::Mode::ClientRegistered);
  connectApp(3, "admin", true, true);
  dataDebug = true;
  write(3, "help", true);
  const std::vector<std::string> got = received(3);
  std::string menu;
  for (const Queued& line : queue) {
    if ((line.route & MSG_ROUTE_BLE) && line.text.rfind("[BLE-", 0) != 0) menu += line.text + "\n";
  }
  CHECK(got.size() >= 3 && got.back() == "OK" && joinedExceptLast(got) == menu &&
            joinedExceptLast(got).find("[BLE-") == std::string::npos,
        "with traces on the session got %zu message(s), %zu menu bytes of %zu", got.size(),
        joinedExceptLast(got).size(), menu.size());
  bool traced = false;
  for (const Queued& line : queue) {
    if (line.text.rfind("[BLE-TX]", 0) == 0) {
      traced = true;
      CHECK(!(line.route & MSG_ROUTE_BLE), "a trace was routed to Bluetooth");
    }
  }
  CHECK(traced, "the send traces ran");
}

// The phone link's console row (phone link v2, the console as pipe stream 2):
// bound for generation 7, stream 5, on the link whose Bluedroid id is 3, then
// logged in as the BLE login job binds it. Its output is console records.
static constexpr uint32_t kPipeGeneration = 7;
static constexpr uint16_t kPipeStream = 5;
static uint16_t pipeRowApp(const char* user) {
  CHECK(blePipeConsoleStateAcquire(), "the pipe holds the console state");
  CHECK(blePipeConsoleBind(kPipeGeneration, kPipeStream, 3), "the console row is bound");
  const uint16_t row = blePipeConsoleRowId(kPipeGeneration);
  TransportSessionEpoch bound = 0;
  CHECK(bleBindSession(row, bleSessionEpochForConnection(row), String(user), true, bound) &&
            bound != 0,
        "the console row logs in");
  return row;
}
static std::vector<std::string> pipeTexts(TransportSessionEpoch epoch) {
  std::vector<std::string> texts;
  for (const PipeHostCall& call : pipeHostWrites()) {
    CHECK(call.kind == PipeConsoleKind::Text && call.mode == PipeConsoleMode::Reliable &&
              call.generation == kPipeGeneration && call.streamId == kPipeStream &&
              call.epoch == epoch && !call.underLifecycle,
          "a console row's output is a reliable TEXT chain for its stream and session, "
          "written with the lifecycle lock released");
    texts.push_back(call.data);
  }
  return texts;
}

// `help` and `help all` reach the console row as TEXT chains: the menu in
// whole-line batches of at most 512 bytes, then "OK", the same messages server
// mode sends; nothing goes out on GATT.
static void pipeRowHelpIsText() {
  for (const char* command : {"help", "help all"}) {
    const std::vector<std::string> server = helpOn(hw1console::Mode::Server, command);
    resetWorld(hw1console::Mode::Off);  // phone-client mode: no command service
    const uint16_t row = pipeRowApp("admin");
    write(row, command, /*encrypted=*/true);
    const std::vector<std::string> texts = pipeTexts(bleSessionEpochForConnection(row));
    CHECK(texts.size() >= 2 && texts.back() == "OK" && joinedExceptLast(texts) == bluetoothLines(),
          "console row: `%s` answered %zu TEXT chain(s) with %zu menu bytes (want the menu, then OK)",
          command, texts.size(), joinedExceptLast(texts).size());
    CHECK(texts == server, "console row: `%s` differs from server mode (%zu vs %zu messages)",
          command, texts.size(), server.size());
    checkBatches(command, texts);
    CHECK(inbox.empty(), "console row output went out on GATT");
    CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
  }
}

// A line printed while this task holds the pipe owner mutex or the console
// chain mutex (an owner command on cmd_exec) joins the batch when it fits and
// never flushes it: the console row's send would wait on the lock it holds.
static void pipeLocksHeldNeverFlush() {
  for (const char* command : {"owner lines", "chain lines"}) {
    resetWorld(hw1console::Mode::Off);
    const uint16_t row = pipeRowApp("admin");
    write(row, command, true);
    const std::vector<std::string> texts = pipeTexts(bleSessionEpochForConnection(row));
    CHECK(writesWhileHeld == 0, "`%s`: %zu record(s) were written while the lock was held",
          command, writesWhileHeld);
    CHECK(texts.size() == 2 && texts[0] == std::string(450, 'o') + "\nafter\n" && texts[1] == "OK",
          "`%s`: the line that fit, the next line after the lock, then OK (got %zu chain(s))",
          command, texts.size());
  }
}

// CONC-1: a console row whose stream takes nothing (the phone grants no
// CREDIT, or the display keeps it busy): the first batch's reliable send
// fails, so the command's later batches are dropped instead of each waiting
// on cmd_exec. The result is still a reliable write: the driver's stall latch
// makes it one try once a write stalled (phone_console's stallEpisodes), and a
// result must not drop. A stream that takes the first batch fails on the
// second, and stops there.
static void stalledStreamSendsOnce() {
  for (int room : {0, 1}) {
    resetWorld(hw1console::Mode::Off);
    const uint16_t row = pipeRowApp("admin");
    gPipeHost.room = room;
    write(row, "batches", true);
    const std::vector<PipeHostCall> writes = pipeHostWrites();
    const size_t batches = static_cast<size_t>(room) + 1;
    bool shape = writes.size() == batches + 1;
    for (size_t i = 0; shape && i < writes.size(); ++i) {
      const bool last = i + 1 == writes.size();
      shape = writes[i].kind == PipeConsoleKind::Text &&
              writes[i].mode == PipeConsoleMode::Reliable &&
              writes[i].sent == (static_cast<int>(i) < room) &&
              writes[i].data == (last ? std::string("OK")
                                      : std::string(450, char('a' + i)) + "\n");
    }
    CHECK(shape, "room %d: want %zu reliable batch send(s) then the result as a reliable write, "
          "got %zu write(s), last %s", room, batches, writes.size(),
          writes.empty() ? "none"
                         : writes.back().mode == PipeConsoleMode::Reliable ? "Reliable" : "TryNow");
  }
  // A stream with room keeps every batch reliable (pipeRowHelpIsText).
  resetWorld(hw1console::Mode::Off);
  const uint16_t row = pipeRowApp("admin");
  write(row, "batches", true);
  const std::vector<std::string> texts = pipeTexts(bleSessionEpochForConnection(row));
  CHECK(texts.size() == 5 && texts.back() == "OK", "a stream with room got %zu chain(s)", texts.size());
}

// Messages as "<first byte><size>" (or the message when short), for failures.
static std::string shapeOf(const std::vector<std::string>& messages) {
  std::string shape;
  for (const std::string& m : messages) {
    if (!shape.empty()) shape += ',';
    shape += m.size() > 8 ? m.substr(0, 1) + std::to_string(m.size()) : m;
  }
  return shape.empty() ? "nothing" : shape;
}

// CONC-1's batch drop is the console row's alone. A server-mode session
// (plaintext or Secure Channel) or design A's slot whose GATT post is refused
// for one batch's six tries (about 90 ms of congestion) loses that batch only:
// its own retries bound each send, so the later batches and the result still
// go, the result with its retries (command results must not drop).
static void congestedLinkKeepsLaterBatches() {
  struct Lane { hw1console::Mode mode; bool secure; const char* name; };
  const Lane lanes[] = {{hw1console::Mode::Server, false, "server plaintext"},
                        {hw1console::Mode::Server, true, "server Secure Channel"},
                        {hw1console::Mode::ClientRegistered, true, "design A"}};
  const std::vector<std::string> want = {std::string(450, 'b') + "\n", std::string(450, 'c') + "\n",
                                         std::string(450, 'd') + "\n", "OK"};
  for (const Lane& lane : lanes) {
    // 6: every try of the first batch is refused; 7: the second batch's first try too.
    for (int refused : {6, 7}) {
      resetWorld(lane.mode);
      const uint16_t app = 3;
      connectApp(app, "admin", true, lane.secure);
      postFailures = refused;
      write(app, "batches", lane.secure);
      const std::vector<std::string> got = received(app);
      CHECK(postFailures == 0 && got == want,
            "%s, %d refused post(s): want b451,c451,d451,OK, got %s", lane.name, refused,
            shapeOf(got).c_str());
      CHECK(pathFaults.empty(), "%s", pathFaults.empty() ? "" : pathFaults.front().c_str());
    }
  }
}

// K2: bleLifecycleHeldByCurrentTask() is this task's hold of the lifecycle
// lock, never blocks, and is false before the lock exists or the scheduler runs
// (no holder and no task are both nullptr there).
static void lifecycleHolderProbe() {
  resetWorld(hw1console::Mode::Off);
  CHECK(!bleLifecycleHeldByCurrentTask(), "an unheld lifecycle lock reads as held");
  {
    BleLifecycleGuard lifecycle;
    CHECK(lifecycle && bleLifecycleHeldByCurrentTask(), "the held lifecycle lock reads as free");
  }
  CHECK(!bleLifecycleHeldByCurrentTask(), "a released lifecycle lock reads as held");
  noCurrentTask = true;
  CHECK(!bleLifecycleHeldByCurrentTask(), "no task and no holder read as held");
  noCurrentTask = false;
  const SemaphoreHandle_t lock = sBleLifecycleMutex;
  sBleLifecycleMutex = nullptr;
  CHECK(!bleLifecycleHeldByCurrentTask(), "no lifecycle lock yet reads as held");
  sBleLifecycleMutex = lock;
}

int main() {
  makeModules();
  stalledStreamSendsOnce();
  congestedLinkKeepsLaterBatches();
  lifecycleHolderProbe();
  pipeRowHelpIsText();
  pipeLocksHeldNeverFlush();
  serverModeHelpReachesItsSession();
  phoneLinkHelpMatchesServerMode();
  resultsStayAsReturned();
  mirrorRecipientsAreNotSentTwice();
  plaintextServerSession();
  fencesAndOtherCallers();
  linesUnderTheLockAreNotSentUnderIt();
  tracesStayOffTheBatch();
  CHECK(gLifecycle.depth == 0, "every lifecycle lock was released");
  if (gBLEState) {
    gBLEState->~BLESystemState();
    std::free(gBLEState);
  }
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("BLE command output: `help` reaches the session that sent it, then OK, the same "
              "in server mode and on the phone link\n");
  return 0;
}
