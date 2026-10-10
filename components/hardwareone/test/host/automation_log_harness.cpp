// Production owner/parser/guard/commands/hooks are inserted by the runner.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static std::atomic<unsigned> checks{0};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)
static thread_local bool failNextReserve = false, failNextConcat = false;
static thread_local const char* failCopyValue = nullptr;
class String {
 public:
  String() = default;
  String(const char* text) { assign(text ? text : ""); }
  String(const std::string& text) { assign(text.c_str()); }
  String(const String& text) { assign(text.c_str()); }
  String(String&&) = default;
  String& operator=(const String& text) { assign(text.c_str()); return *this; }
  String& operator=(String&&) = default;
  String& operator=(const char* text) { assign(text ? text : ""); return *this; }
  size_t length() const { return value_.length(); }
  const char* c_str() const { return value_.c_str(); }
  char operator[](size_t i) const { return value_[i]; }
  bool reserve(size_t bytes) {
    if (failNextReserve) { failNextReserve = false; return false; }
    value_.reserve(bytes); return true;
  }
  bool concat(const char* text) {
    if (failNextConcat) { failNextConcat = false; return false; }
    value_ += text ? text : ""; return true;
  }
  bool concat(const String& text) { return concat(text.c_str()); }
  bool concat(char text) { char value[]{text, '\0'}; return concat(value); }
  String& operator+=(const String& text) { concat(text); return *this; }
  String& operator+=(char text) { concat(text); return *this; }
  String operator+(const String& text) const { return value_ + text.value_; }
  bool operator==(const String& text) const { return value_ == text.value_; }
  bool operator!=(const String& text) const { return !(*this == text); }
  bool equals(const String& text) const { return *this == text; }
  bool isEmpty() const { return value_.empty(); }
  int indexOf(const char* text, size_t from = 0) const { return index(value_.find(text, from)); }
  int indexOf(char text, size_t from = 0) const { return index(value_.find(text, from)); }
  int lastIndexOf(char text) const { return index(value_.rfind(text)); }
  String substring(size_t from) const { return value_.substr(from); }
  String substring(size_t from, size_t to) const { return value_.substr(from, to - from); }
  bool startsWith(const char* text) const { return value_.rfind(text, 0) == 0; }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    if (first == value_.npos) value_.clear();
    else value_ = value_.substr(first, value_.find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(value_.begin(), value_.end(), value_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }
 private:
  static int index(size_t index) { return index == std::string::npos ? -1 : static_cast<int>(index); }
  void assign(const char* text) {
    if (failCopyValue && std::strcmp(failCopyValue, text) == 0) {
      value_.clear(); failCopyValue = nullptr;
    } else value_ = text;
  }
  std::string value_;
};

// INSERT_AUTH_TYPES
static thread_local AuthContext caller{};
static AuthContext currentAuthContext() { return caller; }
static AuthContext identity(const char* user, const char* scope = "/logs/") {
  return {SOURCE_WEB, "/api/command", "fixture-ip", user, "fixture-session",
          reinterpret_cast<void*>(uintptr_t(0x1234)), scope};
}
using TickType_t = uint32_t;
using TaskHandle_t = uintptr_t;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
static std::atomic<uintptr_t> nextTask{1};
static thread_local uintptr_t taskId = nextTask++;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return taskId; }
struct StaticSemaphore_t { std::mutex mutex; std::atomic<uintptr_t> owner{0}; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static StaticSemaphore_t fsStorage;
static SemaphoreHandle_t gFsMutex = &fsStorage;
static std::atomic<unsigned> fsWaiters{0}, fsTakes{0}, fsGives{0};
static thread_local unsigned stateLockDepth = 0;
static bool isHeldByCurrentTask(SemaphoreHandle_t sem) { return sem && sem->owner == taskId; }
static bool isFsLockedByCurrentTask() { return isHeldByCurrentTask(gFsMutex); }
static TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) { return sem->owner; }
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
  CHECK(sem && !isHeldByCurrentTask(sem));
  if (sem == gFsMutex) ++fsWaiters;
  sem->mutex.lock(); sem->owner = taskId;
  if (sem == gFsMutex) { --fsWaiters; ++fsTakes; }
  else ++stateLockDepth;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t sem) {
  CHECK(isHeldByCurrentTask(sem));
  if (sem == gFsMutex) ++fsGives;
  else --stateLockDepth;
  sem->owner = 0; sem->mutex.unlock();
}
#define configASSERT(condition) CHECK(condition)
// INSERT_FS_GUARD

enum class AllocPref { PreferPSRAM };
static thread_local bool failNextAllocation = false;
static void* ps_alloc(size_t bytes, AllocPref, const char*) {
  if (failNextAllocation) { failNextAllocation = false; return nullptr; }
  return std::malloc(bytes);
}

static std::atomic<bool> filesystemReady{true};
static thread_local bool gCLIValidateOnly = false, debugAvailable = true;
static thread_local char debugBuffer[1024];
static bool ensureDebugBuffer() { return debugAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (gCLIValidateOnly) return "VALID"; } while (false)
static bool withTimestamp = true;
static void getTimestampPrefixMsCached(char* out, size_t size) {
  std::snprintf(out, size, "%s", withTimestamp ? "[2026-01-02 03:04:05.006] | " : "");
}

enum class Fault { None, Open, Short, Zero, WriteError, FlushError, SizeMismatch, Directory, Mkdir };
static Fault fault = Fault::None;
struct Node { std::string bytes; bool directory = false, writeError = false, flushed = false; };
struct Access { std::string operation, path; AuthContext owner; };
struct Written { std::string path, bytes, user; };
static std::map<std::string, std::shared_ptr<Node>> files;
static std::set<std::string> directories, deniedUsers;
static std::vector<Access> accesses;
static std::vector<Written> written;
static bool overflow = false;
static unsigned opens = 0, closes = 0;
static void io() { CHECK(isFsLockedByCurrentTask() && stateLockDepth == 0); }
static bool permitted(const AuthContext& ctx) { return !deniedUsers.count(ctx.user.c_str()); }
static void record(const char* operation, const String& path, const AuthContext& ctx) {
  io(); accesses.push_back({operation, path.c_str(), ctx});
}
static std::mutex blockMutex;
static std::condition_variable blockChanged;
static bool blockNextPrint = false, printEntered = false, printReleased = false;
class File {
 public:
  File() = default;
  File(std::string path, std::shared_ptr<Node> node, std::string user)
      : path_(std::move(path)), node_(std::move(node)), user_(std::move(user)) {}
  explicit operator bool() const { return !!node_; }
  bool isDirectory() const { io(); return node_->directory; }
  size_t size() const {
    io(); return node_->bytes.size() + (fault == Fault::SizeMismatch && node_->flushed ? 1 : 0);
  }
  size_t print(const String& line) {
    io();
    {
      std::unique_lock<std::mutex> lock(blockMutex);
      if (blockNextPrint) {
        blockNextPrint = false; printEntered = true; blockChanged.notify_all();
        CHECK(blockChanged.wait_for(lock, std::chrono::seconds(5), [] { return printReleased; }));
      }
    }
    size_t count = line.length();
    if (fault == Fault::Short) count /= 2;
    if (fault == Fault::Zero) count = 0;
    node_->bytes.append(line.c_str(), count);
    if (fault == Fault::WriteError) node_->writeError = true;
    written.push_back({path_, std::string(line.c_str(), count), user_});
    return count;
  }
  void flush() { io(); node_->flushed = true; if (fault == Fault::FlushError) node_->writeError = true; }
  int getWriteError() const { io(); return node_->writeError; }
  void close() { io(); ++closes; node_.reset(); }
 private:
  std::string path_;
  std::shared_ptr<Node> node_;
  std::string user_;
};
namespace VFS {
bool resolveOverflowPath(const char* source, size_t bytes, char* out, size_t capacity) {
  io(); CHECK(bytes > 512);
  const std::string path = (overflow ? "/sd" : "") + std::string(source);
  CHECK(capacity > path.size());
  std::snprintf(out, capacity, "%s", path.c_str()); return overflow;
}
bool existsGuarded(const String& path, const AuthContext& ctx) {
  record("exists", path, ctx); return permitted(ctx) && directories.count(path.c_str());
}
bool mkdirGuarded(const String& path, const AuthContext& ctx) {
  record("mkdir", path, ctx);
  if (fault == Fault::Mkdir || !permitted(ctx)) return false;
  directories.insert(path.c_str()); return true;
}
File openGuarded(const String& path, const char* mode, const AuthContext& ctx) {
  record("open", path, ctx); CHECK(std::strcmp(mode, "a") == 0);
  if (fault == Fault::Open || !permitted(ctx)) return {};
  auto& node = files[path.c_str()];
  if (!node) node = std::make_shared<Node>();
  node->directory = fault == Fault::Directory; node->flushed = false; node->writeError = false;
  ++opens; return File(path.c_str(), node, ctx.user.c_str());
}
}
// INSERT_COMMAND_PARSER
// INSERT_OWNER
// INSERT_AUTOLOG_COMMAND
// INSERT_REDACTOR
struct CommandContext { char automationName[64]{}; };
static void commandHook(CommandContext* cmdCtx, const String& safeCommandForTrace) {
  // INSERT_COMMAND_HOOK
}
static void outputHook(CommandContext* cmdCtx, const char* out) {
  // INSERT_OUTPUT_HOOK
}

static std::string command(const char* text) { return cmd_autolog(text); }
static void balanced() { CHECK(!isFsLockedByCurrentTask() && fsTakes == fsGives && opens == closes); }
static void reset() {
  fault = Fault::None; filesystemReady = true; debugAvailable = true;
  deniedUsers.clear(); failNextReserve = failNextConcat = failNextAllocation = false; failCopyValue = nullptr;
  if (automationLogActive()) command("stop");
  balanced(); files.clear(); accesses.clear(); written.clear(); directories.clear();
  opens = closes = 0; overflow = false; withTimestamp = true; caller = identity("alice");
}
static void start(const char* path = "/logs/alice.log") {
  const std::string result = command((std::string("start ") + path).c_str());
  CHECK(result.rfind("Automation logging started:", 0) == 0 && automationLogActive());
}
static void basicLifecycle() {
  reset();
  const unsigned takes = fsTakes;
  CHECK(!appendAutoLogEntry("IGNORED", "inactive") && fsTakes == takes);
  CHECK(command("status") == "Automation logging INACTIVE");
  CHECK(command("stop").rfind("Error:", 0) == 0);
  CHECK(command("start").rfind("Error:", 0) == 0);
  gCLIValidateOnly = true;
  CHECK(command("start /logs/unused.log") == "VALID" && !automationLogActive() && files.empty());
  gCLIValidateOnly = false;
  debugAvailable = false;
  CHECK(command("start /logs/unused.log").rfind("Error:", 0) == 0 && !automationLogActive());
  debugAvailable = true;
  start();
  CHECK(command("status") == "Automation logging ACTIVE: /logs/alice.log");
  caller = identity("bob", "/other/");
  CHECK(appendAutoLogEntry("EVENT", "accepted"));
  CHECK(command("stop") == "Automation logging stopped: /logs/alice.log");
  CHECK(!automationLogActive());
  CHECK(files.at("/logs/alice.log")->bytes ==
        "[2026-01-02 03:04:05.006] | LOG_START | Automation logging started\n"
        "[2026-01-02 03:04:05.006] | EVENT | accepted\n"
        "[2026-01-02 03:04:05.006] | LOG_STOP | Automation logging stopped\n");
  for (const auto& access : accesses) {
    CHECK(access.owner.user == "alice" && access.owner.scope == "/logs/");
    CHECK(access.owner.transport == SOURCE_WEB && access.owner.path == "/api/command");
    CHECK(access.owner.ip == "fixture-ip" && access.owner.sid.length() == 0 && !access.owner.opaque);
  }
  balanced();
}
static void failures() {
  for (const auto failure : {Fault::Open, Fault::Short, Fault::Zero, Fault::WriteError,
                            Fault::FlushError, Fault::SizeMismatch, Fault::Directory, Fault::Mkdir}) {
    reset(); fault = failure;
    CHECK(command("start /logs/alice.log").rfind("Error:", 0) == 0 && !automationLogActive());
    balanced();
    reset(); start(); caller = identity("bob"); fault = failure;
    CHECK(command("start /other/bob.log").rfind("Error:", 0) == 0);
    CHECK(command("status") == "Automation logging ACTIVE: /logs/alice.log");
    fault = Fault::None;
    CHECK(appendAutoLogEntry("EVENT", "prior session retained"));
    CHECK(written.back().path == "/logs/alice.log" && written.back().user == "alice");
    balanced();
  }
  for (const auto failure : {Fault::Open, Fault::Short, Fault::Zero, Fault::WriteError,
                            Fault::FlushError, Fault::SizeMismatch, Fault::Directory}) {
    reset(); start(); fault = failure;
    CHECK(!appendAutoLogEntry("EVENT", "incomplete"));
    const auto result = command("stop");
    CHECK(result.rfind("Error:", 0) == 0 && result.find("LOG_STOP") != result.npos);
    CHECK(!automationLogActive()); balanced();
  }
  reset(); start(); deniedUsers.insert("alice"); caller = identity("bob");
  CHECK(!appendAutoLogEntry("EVENT", "revoked"));
  CHECK(command("stop").find("LOG_STOP") != std::string::npos && !automationLogActive());
  balanced();
  reset(); filesystemReady = false;
  CHECK(command("start /logs/alice.log").rfind("Error:", 0) == 0 && files.empty());
  balanced();
  reset(); start();
  gFsMutex = nullptr;
  CHECK(!appendAutoLogEntry("EVENT", "no filesystem lock"));
  CHECK(command("start /logs/replacement.log").rfind("Error:", 0) == 0);
  CHECK(command("stop").rfind("Error:", 0) == 0 && automationLogActive());
  CHECK(command("status") == "Automation logging ACTIVE: /logs/alice.log");
  CHECK(written.size() == 1);
  gFsMutex = &fsStorage; balanced();
}
static void routingAndAllocation() {
  reset(); overflow = true; withTimestamp = false;
  const std::string path = "/logs/" + std::string(150, 'x') + ".log";
  start(path.c_str());
  CHECK(files.count("/sd" + path));
  CHECK(written.front().bytes == "LOG_START | Automation logging started\n");
  {
    FsLockGuard callerGuard("existing.caller");
    CHECK(appendAutoLogEntry("NESTED", "already holds filesystem lock"));
    CHECK(isFsLockedByCurrentTask());
  }
  balanced();
  for (const char* field : {"bob", "/other/", "/api/replacement", "replacement-ip"}) {
    reset(); start();
    AuthContext candidate = identity("bob", "/other/");
    candidate.path = "/api/replacement"; candidate.ip = "replacement-ip";
    failCopyValue = field;
    CHECK(!startAutomationLog("/other/bob.log", candidate));
    CHECK(command("status") == "Automation logging ACTIVE: /logs/alice.log");
    CHECK(written.size() == 1); balanced();
  }
  reset(); start();
  const String candidatePath("/other/bob.log");
  const AuthContext candidate = identity("bob");
  failCopyValue = candidatePath.c_str();
  CHECK(!startAutomationLog(candidatePath, candidate));
  CHECK(command("status") == "Automation logging ACTIVE: /logs/alice.log");
  CHECK(written.size() == 1); balanced();
  for (const char* field : {"/logs/alice.log", "/logs"}) {
    failCopyValue = field;
    CHECK(!appendAutoLogEntry("EVENT", "destination copy failed"));
    CHECK(written.size() == 1); balanced();
  }
  reset(); start(); failNextReserve = true;
  CHECK(!appendAutoLogEntry("EVENT", "allocation failed"));
  CHECK(written.size() == 1); balanced();
  failNextConcat = true;
  CHECK(!appendAutoLogEntry("EVENT", "concatenation failed"));
  CHECK(written.size() == 1); balanced();
  failNextAllocation = true;
  CHECK(!appendAutoLogEntry("EVENT", "path allocation failed"));
  CHECK(written.size() == 1); balanced();
}
static void executorHooks() {
  reset(); start(); CommandContext ctx;
  commandHook(nullptr, "safe"); outputHook(nullptr, "safe");
  commandHook(&ctx, "safe"); outputHook(&ctx, "safe");
  CHECK(written.size() == 1);
  std::snprintf(ctx.automationName, sizeof(ctx.automationName), "%s", "fixture automation");
  commandHook(&ctx, "wifiadd fixture-network ***");
  CHECK(written.back().bytes.find("COMMAND | [fixture automation] wifiadd fixture-network ***\n") != std::string::npos);
  outputHook(&ctx, "{\"sttText\":\"private words\"}");
  CHECK(written.back().bytes.find("OUTPUT | [private STT result]\n") != std::string::npos);
  outputHook(&ctx, "{\"password\":\"fixture-secret\",\"sid\":\"fixture-token\"}\nnext\rline");
  CHECK(written.back().bytes.find("fixture-secret") == std::string::npos);
  CHECK(written.back().bytes.find("fixture-token") == std::string::npos);
  CHECK(written.back().bytes.find(" next line\n") != std::string::npos);
  outputHook(&ctx, std::string(250, 'x').c_str());
  CHECK(written.back().bytes.find(std::string(197, 'x') + "...\n") != std::string::npos);
  CHECK(written.back().bytes.find(std::string(198, 'x')) == std::string::npos);
  command("stop"); const auto count = written.size();
  commandHook(&ctx, "safe"); outputHook(&ctx, "safe"); CHECK(written.size() == count);
  balanced();
}
static void armBlockedPrint() {
  std::lock_guard<std::mutex> lock(blockMutex);
  blockNextPrint = true; printEntered = printReleased = false;
}
static void awaitBlockedPrint() {
  std::unique_lock<std::mutex> lock(blockMutex);
  CHECK(blockChanged.wait_for(lock, std::chrono::seconds(5), [] { return printEntered; }));
}
static void releaseBlockedPrint() {
  std::lock_guard<std::mutex> lock(blockMutex);
  printReleased = true; blockChanged.notify_all();
}
static void concurrency() {
  reset(); start(); armBlockedPrint();
  auto append = std::async(std::launch::async, [] { return appendAutoLogEntry("EVENT", "in flight"); });
  awaitBlockedPrint();
  auto status = std::async(std::launch::async, [] { return command("status"); });
  CHECK(status.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(status.get() == "Automation logging ACTIVE: /logs/alice.log");
  auto replacement = std::async(std::launch::async, [] {
    caller = identity("bob"); return command("start /logs/bob.log");
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!fsWaiters && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  CHECK(fsWaiters > 0); releaseBlockedPrint();
  CHECK(append.get()); CHECK(replacement.get() == "Automation logging started: /logs/bob.log");
  CHECK(written.size() == 3 && written[1].user == "alice" && written[1].path == "/logs/alice.log");
  CHECK(written[2].user == "bob" && written[2].path == "/logs/bob.log");
  armBlockedPrint();
  auto second = std::async(std::launch::async, [] { return appendAutoLogEntry("EVENT", "before stop"); });
  awaitBlockedPrint();
  auto stop = std::async(std::launch::async, [] { return command("stop"); });
  const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!fsWaiters && std::chrono::steady_clock::now() < stopDeadline) std::this_thread::yield();
  CHECK(fsWaiters > 0); releaseBlockedPrint();
  CHECK(second.get()); CHECK(stop.get() == "Automation logging stopped: /logs/bob.log");
  CHECK(!appendAutoLogEntry("EVENT", "after stop"));
  CHECK(written.back().bytes.find("LOG_STOP") != std::string::npos); balanced();
  reset();
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([worker] {
    const std::string user = "worker" + std::to_string(worker);
    caller = identity(user.c_str());
    for (unsigned i = 0; i < 100; ++i) {
      command(("start /logs/" + user + ".log").c_str());
      appendAutoLogEntry("EVENT", "concurrent entry");
      command("status"); command("stop");
    }
  });
  for (auto& worker : workers) worker.join();
  CHECK(!written.empty());
  for (const auto& line : written) CHECK(line.path == "/logs/" + line.user + ".log");
  if (automationLogActive()) command("stop"); balanced();
}
int main() {
  basicLifecycle(); failures(); routingAndAllocation(); executorHooks(); concurrency();
  reset();
  std::printf("automation logging: %u checks passed (owner, permissions, checked writes, hooks and concurrency)\n", checks.load());
}
