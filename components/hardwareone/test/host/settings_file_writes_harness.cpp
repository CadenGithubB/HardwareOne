#include <Arduino.h>
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <ArduinoJson.h>
#include "System_UserSettings.h"
#include "System_PollPause.h"
#include "System_SettingsWriteBatchCore.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static std::atomic<unsigned> checks{0};
static void check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
void webBatchTestStringAppend() {}

struct TestAllocator : ArduinoJson::Allocator {
  bool fail = false;
  void* allocate(size_t count) override { return fail ? nullptr : std::malloc(count); }
  void deallocate(void* pointer) override { std::free(pointer); }
  void* reallocate(void* pointer, size_t count) override {
    return fail ? nullptr : std::realloc(pointer, count);
  }
} jsonAllocator;
#define PSRAM_JSON_DOC(name) JsonDocument name(&jsonAllocator)

using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
struct StaticSemaphore_t { std::recursive_mutex mutex; } fsMutex;
using SemaphoreHandle_t = StaticSemaphore_t*;
static SemaphoreHandle_t gFsMutex = &fsMutex;
static thread_local bool ownsFs = false;
static thread_local unsigned writerDepth = 0, localPauseDepth = 0;
static std::atomic<unsigned> activeFiles{0}, pauses{0}, resumes{0}, lockTakes{0}, lockGives{0};
static std::atomic<unsigned> writerLockAttempts{0}, diagnosticCalls{0};
static bool writerAvailable = true;
static std::mutex scheduleMutex;
static std::condition_variable scheduleChanged;
static bool interleave = false, restampRead = false, resumeRestamp = false;
static bool writerBlocked = false, mainDone = false;
static thread_local bool mainWorker = false;
static void portENTER_CRITICAL(std::mutex* mutex) { mutex->lock(); }
static void portEXIT_CRITICAL(std::mutex* mutex) { mutex->unlock(); }
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t) {
  ++writerLockAttempts;
  if (!writerAvailable) return 0;
  if (!sem->mutex.try_lock()) {
    if (mainWorker) {
      std::lock_guard<std::mutex> lock(scheduleMutex);
      writerBlocked = true; scheduleChanged.notify_all();
    }
    sem->mutex.lock();
  }
  ++writerDepth; return pdTRUE;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t sem) {
  check(writerDepth != 0, "writer guard release without ownership");
  --writerDepth; sem->mutex.unlock();
}
static bool isHeldByCurrentTask(SemaphoreHandle_t) { return ownsFs; }
static bool isFsLockedByCurrentTask() { return ownsFs; }
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
  check(!ownsFs, "filesystem guard recursively took a nonrecursive backend");
  sem->mutex.lock(); ownsFs = true; ++lockTakes; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t sem) {
  check(ownsFs, "filesystem release without ownership");
  check(activeFiles == 0, "filesystem released before closing file");
  ownsFs = false; ++lockGives; sem->mutex.unlock();
}
static void fsLock(const char*) { if (!ownsFs) xSemaphoreTake(gFsMutex, portMAX_DELAY); }
static void fsUnlock() { if (ownsFs) xSemaphoreGive(gFsMutex); }
volatile bool gSensorPollingPaused = false;
void pollPause(uint8_t) { ++localPauseDepth; ++pauses; }
void pollResume(uint8_t) {
  check(localPauseDepth != 0, "unmatched poll resume");
  --localPauseDepth; ++resumes;
}
bool pollPaused(uint8_t) { return localPauseDepth != 0; }
// INSERT_FS_GUARD
// INSERT_WRITER_GUARD

enum class Fault { None, Open, Directory, Short, Size, WriteError, Rename, Cleanup, RenameShort };
static Fault fault = Fault::None;
static bool readOpenFails = false;
static unsigned writeOpens = 0, readOpens = 0, directOpens = 0, renameCalls = 0, removeCalls = 0;
static std::vector<std::string> authOwners;
struct Node { std::string bytes; bool directory = false; };
static std::map<std::string, std::shared_ptr<Node>> files;
struct Handle {
  std::shared_ptr<Node> node;
  size_t offset = 0;
  bool closed = false, writable = false, temporary = false;
  explicit Handle(std::shared_ptr<Node> n, bool w, bool temp)
      : node(std::move(n)), writable(w), temporary(temp) { ++activeFiles; }
};
class File {
 public:
  File() = default;
  File(std::shared_ptr<Node> node, bool write, bool temporary = false)
      : handle_(std::make_shared<Handle>(std::move(node), write, temporary)) {}
  explicit operator bool() const { return handle_ && !handle_->closed; }
  bool isDirectory() const { check(ownsFs, "directory inspection outside FS lock"); return handle_->node->directory; }
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) {
    check(ownsFs && *this && handle_->writable, "serialization outside a live locked writer");
    if (handle_->node->directory) return 0;
    const bool shortWrite = fault == Fault::Short || fault == Fault::Cleanup ||
                           (fault == Fault::RenameShort && !handle_->temporary);
    const size_t limit = shortWrite ? 7 : std::numeric_limits<size_t>::max();
    const size_t available = limit - std::min(limit, handle_->node->bytes.size());
    const size_t accepted = std::min(count, available);
    handle_->node->bytes.append(reinterpret_cast<const char*>(data), accepted);
    return accepted;
  }
  int read() {
    check(ownsFs && *this, "deserialization outside locked reader");
    if (handle_->node->directory) return -1;
    return handle_->offset < handle_->node->bytes.size()
        ? static_cast<unsigned char>(handle_->node->bytes[handle_->offset++]) : -1;
  }
  size_t readBytes(char* output, size_t count) {
    size_t used = 0; int value;
    while (used < count && (value = read()) >= 0) output[used++] = static_cast<char>(value);
    return used;
  }
  void flush() { check(ownsFs && *this, "flush outside locked writer"); }
  size_t size() const {
    check(ownsFs && *this, "size outside locked file");
    return handle_->node->bytes.size() + (fault == Fault::Size && handle_->writable ? 1 : 0);
  }
  int getWriteError() const {
    check(ownsFs && *this, "write error read after close or unlock");
    return fault == Fault::WriteError ? 1 : 0;
  }
  void close() {
    check(ownsFs && *this, "close outside locked file");
    handle_->closed = true; --activeFiles;
  }
 private:
  std::shared_ptr<Handle> handle_;
};

struct AuthContext { const char* owner; };
// Quarantine copies: <file>.corrupt-<n>. Only the oldest slot is ever
// removed (rotation), never a live destination.
static bool isQuarantineCopy(const char* path) {
  return std::strstr(path, ".corrupt-") != nullptr;
}
// Fails only the renames whose destination contains this text (the
// quarantine move), leaving the writers' own promotions working.
static std::string failRenameInto;
static bool isSettingsTemporary(const char* path) {
  const String value(path);
  return value == "/system/settings.tmp" || value == "/system/debug.tmp" ||
         (value.startsWith("/system/users/user_settings/") && value.endsWith(".json.tmp"));
}
namespace VFS {
static AuthContext systemAuth(const char* owner) { return {owner}; }
namespace Scopes {  // the production Scopes (System_VFS.h); keep in step
  inline constexpr const char* VIDEOS = "/sd/VIDEOS"; inline constexpr const char* CERTS = "/system/certs";
  inline constexpr const char* OTA = "/system/ota"; inline constexpr const char* ESPNOW_RECEIVED = "/espnow/received";
  inline constexpr const char* CAPTURES = "/logging_captures"; inline constexpr const char* CAPTURES_SD = "/sd/logging_captures";
  inline constexpr const char* STT_MODELS = "/STT Models"; inline constexpr const char* STT_MODELS_SD = "/sd/STT Models";
  inline constexpr const char* SYS_LOGS = "/system/sys_logs"; inline constexpr const char* SYS_LOGS_SD = "/sd/system/sys_logs";
  inline constexpr const char* SYSTEM_STATE = "/system"; inline constexpr const char* USERS = "/system/users";
  inline constexpr const char* MAPS = "/maps"; inline constexpr const char* EI_MODELS = "/EI Models";
  inline constexpr const char* RECORDINGS = "/recordings"; inline constexpr const char* RECORDINGS_SD = "/sd/recordings";
  inline constexpr const char* ESPNOW_STATE = "/system/espnow"; inline constexpr const char* ESPNOW_CAPTURES = "/sd/espnow";
  inline constexpr const char* LLM_MODELS = "/system/llm"; inline constexpr const char* LLM_MODELS_SD = "/sd/llm";
  inline constexpr const char* ESPSR = "/ESPSR"; inline constexpr const char* ESPSR_SD = "/sd/ESPSR";
  inline constexpr const char* PICTURES_SD = "/sd/PICTURES";
}  // namespace Scopes
// Tier pickers mirroring System_VFS.h (accept const char* or any String-like).
inline const char* fakeVfsCStr(const char* p) { return p; }
template <class S> inline const char* fakeVfsCStr(const S& s) { return s.c_str(); }
template <class P> inline bool fakeVfsOnSd(const P& p) { return std::strncmp(fakeVfsCStr(p), "/sd/", 4) == 0; }
template <class P> inline const char* captureScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::CAPTURES_SD : Scopes::CAPTURES; }
template <class P> inline const char* sysLogScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::SYS_LOGS_SD : Scopes::SYS_LOGS; }
template <class P> inline const char* recordingScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::RECORDINGS_SD : Scopes::RECORDINGS; }
template <class P> inline const char* llmModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::LLM_MODELS_SD : Scopes::LLM_MODELS; }
template <class P> inline const char* espsrScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::ESPSR_SD : Scopes::ESPSR; }
template <class P> inline const char* sttModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::STT_MODELS_SD : Scopes::STT_MODELS; }
// end tier pickers
static AuthContext systemAuth(const char*, const char* owner) { return {owner}; }
static bool existsGuarded(const char* path, AuthContext) { return files.count(path) != 0; }
static File openGuarded(const char* path, const char* mode, AuthContext auth) {
  check(ownsFs, "file open outside FS lock"); authOwners.emplace_back(auth.owner);
  const bool writing = std::strcmp(mode, "w") == 0;
  if (!writing) {
    ++readOpens;
    if (readOpenFails || !files.count(path)) return {};
    return {files.at(path), false};
  }
  ++writeOpens;
  const bool temporary = isSettingsTemporary(path);
  if (!temporary) ++directOpens;
  if (fault == Fault::Open) return {};
  auto node = std::make_shared<Node>(); node->directory = fault == Fault::Directory;
  files[path] = node;
  return {node, true, temporary};
}
static bool removeGuarded(const char* path, AuthContext auth) {
#if HAS_SNAPSHOT_HELPER
  check(ownsFs, "temp cleanup did not retain caller's whole-operation lock");
#endif
  FsLockGuard guard("test.remove");  // Guarded VFS also supports standalone calls.
  check(ownsFs && activeFiles == 0, "temp cleanup outside lock or before close");
  check(isSettingsTemporary(path) || isQuarantineCopy(path), "cleanup removed destination");
  authOwners.emplace_back(auth.owner); ++removeCalls;
  // Models LittleFS (no SD paths here): unlink refuses a directory (EISDIR),
  // as the map, boot-cleanup and capture doubles do.
  const auto existing = files.find(path);
  if (existing != files.end() && existing->second->directory) return false;
  if (fault == Fault::Cleanup) return false;
  files.erase(path); return true;
}
static bool renameGuarded(const char* from, const char* to, AuthContext auth) {
#if HAS_SNAPSHOT_HELPER
  check(ownsFs, "promotion did not retain caller's whole-operation lock");
#endif
  FsLockGuard guard("test.rename");
  check(ownsFs && activeFiles == 0, "promotion outside lock or before close");
  authOwners.emplace_back(auth.owner); ++renameCalls;
  if (fault == Fault::Rename || fault == Fault::RenameShort) return false;
  if (!failRenameInto.empty() && std::strstr(to, failRenameInto.c_str())) return false;
  if (!files.count(from)) return false;
  files[to] = files.at(from); files.erase(from); return true;
}
// Production's guarded calls take String paths; the quarantine helper uses them.
static bool existsGuarded(const String& path, AuthContext auth) { return existsGuarded(path.c_str(), auth); }
static bool removeGuarded(const String& path, AuthContext auth) { return removeGuarded(path.c_str(), auth); }
static bool renameGuarded(const String& from, const String& to, AuthContext auth) {
  return renameGuarded(from.c_str(), to.c_str(), auth);
}
}  // namespace VFS

static const char* SETTINGS_JSON_FILE = "/system/settings.json";
static const char* DEBUG_JSON_FILE = "/system/debug.json";
static bool filesystemReady = true, gSettingsLoadedOk = true, gDebugLoadedOk = true;
static bool gSettingsVersionRestampPending = false, builderFails = false, builderOverflows = false;
static unsigned builds = 0, debugBuilds = 0, bondHashes = 0;
static int liveValue = 17;
static uint8_t retryMask = hw1_settings_batch::ALL;
static constexpr int SYSEVT_SETTINGS_SAVE_FAILED = 1;
static constexpr int SYSEVT_CONFIG_FILE_CORRUPT = 2;
static constexpr int SYSEVT_FIRMWARE_CHANGED = 3;
struct Event { std::string reason, detail; bool locked; int kind = SYSEVT_SETTINGS_SAVE_FAILED; };
static std::vector<Event> events;
static std::vector<std::string> logs;
static std::vector<bool> logLocks;
static void logSystemEvent(const char*, const char* format, ...) {
  char text[1024]; va_list args; va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args); va_end(args);
  logs.emplace_back(text);
  logLocks.push_back(ownsFs);
}
static void systemEventPost(int kind, const char* reason, const char* detail) {
  check(kind == SYSEVT_SETTINGS_SAVE_FAILED || kind == SYSEVT_CONFIG_FILE_CORRUPT ||
        kind == SYSEVT_FIRMWARE_CHANGED, "unexpected settings event");
  events.push_back({reason, detail, ownsFs, kind});
}
#define ERROR_STORAGEF(...) ((void)++diagnosticCalls)
#define WARN_STORAGEF(...) ((void)++diagnosticCalls)
#define INFO_STORAGEF(...) ((void)++diagnosticCalls)
#define DEBUG_STORAGEF(...) ((void)++diagnosticCalls)
namespace SelfDevice { static const char* firmwareVersion() { return "0.99.97.2"; } }
static void clearSettingsWriteRetry(uint8_t mask) {
  check(writerDepth > 0 && !ownsFs && localPauseDepth == 0, "retry cleared before successful writer cleanup");
  retryMask &= ~mask;
}
static unsigned retains = 0;
static void retainSettingsWriteRetry(uint8_t mask) {
  check(writerDepth > 0 && !ownsFs, "retry retained outside the writer guard or under the filesystem lock");
  retryMask |= mask; ++retains;
}
void computeBondLocalSettingsHash() {
  check(writerDepth > 0 && !ownsFs && localPauseDepth == 0, "bond hash before successful writer cleanup");
  ++bondHashes;
}
static bool buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords, bool mainOnly) {
  check(writerDepth > 0 && localPauseDepth > 0, "main build outside writer/poll ownership");
  check(!excludePasswords && mainOnly, "main persistence changed document scope");
  ++builds;
  if (builderFails) return false;
  jsonAllocator.fail = builderOverflows;
  doc["live"] = liveValue;
  doc["firmwareVersion"] = SelfDevice::firmwareVersion();
  doc["newAllocation"] = std::string(100, 'x');
  doc["wifiPrimarySSID"] = "runtime-only";
  jsonAllocator.fail = false;
  return true;
}
static void registerAllSettingsModules() {}
static size_t writeRegisteredSettings(JsonDocument& doc, const char* path, bool allFiles) {
  check(std::strcmp(path, DEBUG_JSON_FILE) == 0 && !allFiles, "debug persistence changed document scope");
  ++debugBuilds;
  jsonAllocator.fail = builderOverflows;
  doc["system"]["debug"]["flag"] = true;
  doc["newAllocation"] = std::string(100, 'x');
  jsonAllocator.fail = false;
  return 1;
}
// readSettingsJson's section readers are observed boundaries: a successful
// load reaches them, a parse failure must not.
static int gSecretLoadFailures = 0;
static unsigned sectionReads = 0;
static void espnowMeshesReadJson(JsonDocument&) { ++sectionReads; }
static void selectDeviceKeyEpoch(JsonDocument&) {}
static size_t readRegisteredSettings(JsonDocument&, const char*, bool allFiles) {
  check(!allFiles, "boot load changed document scope"); ++sectionReads; return 1;
}
static int wifiNetworksReadJson(JsonDocument&) { return 0; }
static bool readText(const char* path, String& output, const AuthContext&) {
  {
    PollPauseGuard pause;
    FsLockGuard guard("test.read");
    File input = VFS::openGuarded(path, "r", VFS::systemAuth("settings.restamp"));
    if (!input) return false;
    std::string data; int value;
    while ((value = input.read()) >= 0) data.push_back(static_cast<char>(value));
    output = data.c_str(); input.close();
  }
  if (interleave) {
    std::unique_lock<std::mutex> lock(scheduleMutex);
    restampRead = true; scheduleChanged.notify_all();
    check(scheduleChanged.wait_for(lock, std::chrono::seconds(5), [] { return resumeRestamp; }), "restamp read was never released");
  }
  return true;
}
// INSERT_SNAPSHOT
// INSERT_WRITERS

// stat is an OS boundary, not a second loadUserSettings implementation. The
// extracted loader still handles errno, parsing and object validation itself.
static unsigned statCalls = 0, invalidations = 0, mutations = 0;
static int statFailure = 0;
static std::vector<bool> invalidationLocks;
struct SettingsTestStat {};
static int settingsTestStat(const char* physicalPath, struct SettingsTestStat*) {
  check(ownsFs && writerDepth == 0, "per-user stat preserves transaction lock order");
  ++statCalls;
  const std::string path(physicalPath);
  check(path.rfind("/littlefs/system/users/user_settings/", 0) == 0,
        "per-user loader stats only its trusted physical path");
  if (statFailure) { errno = statFailure; return -1; }
  if (!files.count(path.substr(std::strlen("/littlefs")))) { errno = ENOENT; return -1; }
  return 0;
}
static void notifUserPrefsInvalidate() {
  check(writerDepth == 0 && activeFiles == 0, "invalidation follows file close without taking the main writer lock");
  ++invalidations; invalidationLocks.push_back(ownsFs);
}
// System_FirstTimeSetup writes the owner credential-setup marker; it is
// a boundary here (initial_admin_setup checks the file and the flow it starts).
static const char* kOwnerSetupMarker = "/system/.pending_credential_setup";
static unsigned ownerSetupArms = 0;
static bool ownerSetupArmSucceeds = true;
static bool armOwnerCredentialSetup() {
  check(ownsFs, "owner setup marker armed outside the loader's lock");
  ++ownerSetupArms;
  if (ownerSetupArmSucceeds) files[kOwnerSetupMarker] = std::make_shared<Node>(Node{"1", false});
  return ownerSetupArmSucceeds;
}
// Keep the source body intact while supplying its POSIX stat boundary.
#define stat SettingsTestStat
static int SettingsTestStat(const char* path, struct SettingsTestStat* value) {
  return settingsTestStat(path, value);
}
// INSERT_USER_WRITERS
#undef stat

static const std::string oldMain = R"({"firmwareVersion":"older","live":1,"unknown":{"values":[2,"keep",true]},"secret":"AES:opaque-fixture","system":{"debug":{"legacy":1},"retained":7},"wifiPrimarySSID":"old-runtime"})";
static const std::string oldDebug = R"({"system":{"debug":{"flag":false}}})";
static void store(const char* path, const std::string& data) { files[path] = std::make_shared<Node>(Node{data, false}); }
static void reset() {
  check(!ownsFs && writerDepth == 0 && localPauseDepth == 0 && activeFiles == 0, "previous operation leaked ownership");
  files.clear(); store(SETTINGS_JSON_FILE, oldMain); store(DEBUG_JSON_FILE, oldDebug);
  fault = Fault::None; readOpenFails = false; writerAvailable = true;
  gFsMutex = &fsMutex;
  filesystemReady = gSettingsLoadedOk = gDebugLoadedOk = true;
  gSettingsVersionRestampPending = true; builderFails = builderOverflows = false;
  jsonAllocator.fail = false; retryMask = hw1_settings_batch::ALL; retains = 0;
  writeOpens = readOpens = directOpens = renameCalls = removeCalls = 0;
  builds = debugBuilds = bondHashes = 0; liveValue = 17;
  events.clear(); logs.clear(); logLocks.clear(); authOwners.clear();
  pauses = resumes = lockTakes = lockGives = 0;
  writerLockAttempts = diagnosticCalls = 0;
  statCalls = invalidations = mutations = 0; statFailure = 0; invalidationLocks.clear();
  sectionReads = 0; failRenameInto.clear(); ownerSetupArms = 0; ownerSetupArmSucceeds = true;
  interleave = restampRead = resumeRestamp = writerBlocked = mainDone = false;
}
static void balanced() {
  check(!ownsFs && writerDepth == 0 && localPauseDepth == 0 && activeFiles == 0, "writer leaked resources");
  check(pauses == resumes && lockTakes == lockGives, "unbalanced polling or filesystem ownership");
  for (const auto& log : logs) check(log.find("opaque-fixture") == std::string::npos, "secret document value leaked into logs");
}
enum class Writer { Main, Debug, Restamp };
static const char* destination(Writer writer) { return writer == Writer::Debug ? DEBUG_JSON_FILE : SETTINGS_JSON_FILE; }
static const char* temporary(Writer writer) { return writer == Writer::Debug ? "/system/debug.tmp" : "/system/settings.tmp"; }
static bool run(Writer writer) {
  if (writer == Writer::Main) return writeSettingsJson();
  if (writer == Writer::Debug) return writeDebugJson();
  return settingsRestampFirmwareVersion();
}
static JsonDocument parse(const std::string& text) {
  JsonDocument doc;
  check(!deserializeJson(doc, text), "saved JSON not parseable"); return doc;
}
static void successCases() {
  for (Writer writer : {Writer::Main, Writer::Debug, Writer::Restamp}) {
    reset();
    check(run(writer), "ordinary save failed");
    balanced();
    check(writeOpens == 1 && directOpens == 0 && renameCalls == 1 && removeCalls == 0, "successful save did not promote exactly one root temp");
    check(!files.count(temporary(writer)) && events.empty(), "success retained temp or emitted failure");
    auto doc = parse(files.at(destination(writer))->bytes);
    check(doc["firmwareVersion"] == SelfDevice::firmwareVersion(), "firmware version missing after save");
    if (writer == Writer::Debug) {
      check(doc["system"]["debug"]["flag"] == true, "debug builder result missing");
      check(files.at(SETTINGS_JSON_FILE)->bytes == oldMain, "debug save changed main document");
    } else {
      check(doc["secret"] == "AES:opaque-fixture" && doc["unknown"]["values"][1] == "keep", "unknown or encrypted values lost");
      check(files.at(DEBUG_JSON_FILE)->bytes == oldDebug, "main/restamp changed debug document");
      if (writer == Writer::Main) {
        check(doc["live"] == 17 && doc["wifiPrimarySSID"].isNull() && doc["system"]["debug"].isNull(), "main scope/filtering changed");
      } else {
        auto expected = parse(oldMain); expected["firmwareVersion"] = SelfDevice::firmwareVersion();
        check(doc == expected && builds == 0 && debugBuilds == 0, "restamp rebuilt live state or changed other JSON values");
      }
    }
    const uint8_t expectedRetry = writer == Writer::Main ? hw1_settings_batch::DEBUG : writer == Writer::Debug ? hw1_settings_batch::MAIN : hw1_settings_batch::ALL;
    check(retryMask == expectedRetry, "success cleared unrelated retry intent");
    check(bondHashes == (ENABLE_BONDED_MODE && writer != Writer::Restamp ? 1u : 0u), "wrong success bond hash behavior");
    // A complete main save stamps the running version too, so only the debug
    // writer leaves a pending re-stamp behind.
    check(settingsFirmwareVersionRestampPending() == (writer == Writer::Debug), "wrong restamp pending state after success");
  }
  reset(); gSettingsVersionRestampPending = false;
  check(settingsRestampFirmwareVersion() && readOpens == 0 && writeOpens == 0, "unneeded restamp performed I/O"); balanced();
}
static void failureCase(Writer writer, Fault injected) {
  reset(); fault = injected;
  const std::string previous = files.at(destination(writer))->bytes;
  const bool persisted = run(writer);
  balanced();
  check(files.at(destination(writer))->bytes == previous, "failed replacement damaged prior destination");
  check(!persisted, "failed replacement reported success");
  check(directOpens == 0, "failed rename fell back to destructive destination write");
  check(removeCalls == 1, "failed temp not cleaned exactly once");
  // A temporary that cannot be removed (cleanup fault, or a directory at that
  // path) stays; it is never promoted and the destination is intact.
  check(files.count(temporary(writer)) == ((injected == Fault::Cleanup || injected == Fault::Directory) ? 1u : 0u),
        "temp cleanup outcome not preserved");
  check(retryMask == hw1_settings_batch::ALL && bondHashes == 0 && settingsFirmwareVersionRestampPending(), "failed save published success or discarded retry/restamp intent");
  check(renameCalls == (injected == Fault::Rename ? 1u : 0u), "invalid temp reached rename");
  if (writer == Writer::Restamp) {
    // A failed re-stamp is cosmetic: it logs its own line and raises none of
    // the full writers' error/save-failed reporting, which means a lost change.
    check(events.empty() && diagnosticCalls == 0, "restamp failure raised the full writers' failure reporting");
    check(!logs.empty() && logs.back().find("firmwareVersion re-stamp FAILED") == 0 && !logLocks.back(),
          "restamp failure not logged once after filesystem release");
  } else {
    check(events.size() == 1 && !events[0].locked && events[0].detail == destination(writer), "helper failure not reported once after FS release");
    const char* stage = injected == Fault::Open || injected == Fault::Directory ? "open" : injected == Fault::Rename ? "rename" : "serialize";
    check(events[0].reason == stage, "wrong failure stage");
    check(!logs.empty() && logs.back().find("save FAILED for ") == 0 && !logLocks.back(),
          "helper failure logging occurred before filesystem release");
  }
  if (injected == Fault::Cleanup || writer == Writer::Restamp) {
    // A directory at the temporary path is removed by boot cleanup or an
    // operator before a save can succeed; the device cannot write through it.
    if (injected == Fault::Directory) files.erase(temporary(writer));
    fault = Fault::None; events.clear();
    check(run(writer), "save did not recover after transient failure"); balanced();
    check(!files.count(temporary(writer)), "successful retry left stale temp");
  }
}
static void admissionAndBuildFailures() {
  for (Writer writer : {Writer::Main, Writer::Debug, Writer::Restamp}) {
    for (unsigned admission = 0; admission != 3; ++admission) {
      reset();
      if (admission == 0) filesystemReady = false;
      if (admission == 1) writerAvailable = false;
      if (admission == 2) { if (writer == Writer::Main) gSettingsLoadedOk = false; else if (writer == Writer::Debug) gDebugLoadedOk = false; else readOpenFails = true; }
      check(!run(writer), "unavailable/unsafe writer accepted save"); balanced();
      check(writeOpens == 0 && files.at(SETTINGS_JSON_FILE)->bytes == oldMain && files.at(DEBUG_JSON_FILE)->bytes == oldDebug, "admission failure mutated storage");
      check(bondHashes == 0 && retryMask == hw1_settings_batch::ALL && settingsFirmwareVersionRestampPending(), "admission failure discarded success state");
    }
  }
  reset(); builderFails = true;
  check(!writeSettingsJson() && writeOpens == 0, "failed main builder reached temp write"); balanced();
  for (Writer writer : {Writer::Main, Writer::Debug}) {
    reset(); files.erase(SETTINGS_JSON_FILE); builderOverflows = true;
    check(!run(writer) && writeOpens == 0, "overflowed builder reached storage"); balanced();
  }
  for (const std::string& text : {std::string{}, std::string{"{\"truncated\":"}}) {
    reset(); store(SETTINGS_JSON_FILE, text);
    check(!run(Writer::Restamp) && files.at(SETTINGS_JSON_FILE)->bytes == text && writeOpens == 0, "unreadable source was overwritten"); balanced();
    check(settingsFirmwareVersionRestampPending(), "parse failure discarded restamp retry intent");
  }
  reset(); files.erase(SETTINGS_JSON_FILE);
  check(writeSettingsJson() && files.count(SETTINGS_JSON_FILE), "fresh-device main save refused absent destination"); balanced();
}

// An unparseable settings.json is quarantined, not left to freeze the
// device. Covers the boot load, the save-time merge-read, rotation, and the
// cases that must still refuse (out of memory: the file is intact).
static const char* kQuarantine1 = "/system/settings.json.corrupt-1";
static bool corruptEventFor(const char* subject) {
  for (const auto& event : events)
    if (event.kind == SYSEVT_CONFIG_FILE_CORRUPT && event.reason == subject) return true;
  return false;
}
static bool loggedQuarantine(const char* movedTo) {
  for (const auto& log : logs)
    if (log.find("quarantined to ") != std::string::npos && log.find(movedTo) != std::string::npos) return true;
  return false;
}
static bool bootLoad() {
  // The boot loader runs before any task contends; the outer guard stands in
  // for the guarded VFS's own locking around its open and read.
  FsLockGuard boot("test.boot");
  return readSettingsJson();
}
static void corruptSettingsQuarantine() {
  for (const std::string& text : {std::string{}, std::string{"{\"truncated\":"}, std::string{"\xff\xfe garbage"}}) {
    // Boot: the damaged file moves aside, RAM stays on defaults, nothing is
    // read into it, and the save gate opens as on a fresh device.
    reset(); gSettingsLoadedOk = false; store(SETTINGS_JSON_FILE, text);
    check(!bootLoad(), "unparseable settings.json reported as loaded");
    check(!files.count(SETTINGS_JSON_FILE) && files.count(kQuarantine1) && files.at(kQuarantine1)->bytes == text,
          "unparseable settings.json was not quarantined byte-for-byte at boot");
    check(sectionReads == 0, "a failed parse reached the section readers");
    check(corruptEventFor(SETTINGS_JSON_FILE) && loggedQuarantine(kQuarantine1),
          "boot quarantine posted no config_file_corrupt event or log line");
    balanced();
    events.clear();
    // HardwareOne then persists the defaults: the next save succeeds ...
    check(writeSettingsJson(), "save still refused after the boot quarantine");
    auto saved = parse(files.at(SETTINGS_JSON_FILE)->bytes);
    check(saved["live"] == liveValue && events.empty(), "post-quarantine save did not write the loaded defaults");
    // ... and factoryreset's pipe-secret persist is no longer blocked.
    check(writeSettingsJsonForFactoryReset(), "factory reset persist still blocked after the boot quarantine");
    check(files.at(kQuarantine1)->bytes == text, "later saves touched the quarantine copy");
    balanced();

    // Save time: a file damaged after a good load is quarantined and the save
    // (an ordinary one and factory reset's) proceeds from RAM.
    for (bool factory : {false, true}) {
      reset(); store(SETTINGS_JSON_FILE, text);
      check(factory ? writeSettingsJsonForFactoryReset() : writeSettingsJson(),
            "save refused an unparseable file instead of quarantining it");
      check(files.at(kQuarantine1)->bytes == text, "merge-read quarantine lost the damaged bytes");
      auto doc = parse(files.at(SETTINGS_JSON_FILE)->bytes);
      check(doc["live"] == liveValue && doc["firmwareVersion"] == SelfDevice::firmwareVersion(),
            "post-quarantine save did not persist live settings");
      check(corruptEventFor(SETTINGS_JSON_FILE) && loggedQuarantine(kQuarantine1),
            "save-time quarantine posted no event or log line");
      check(writeSettingsJson(), "save after the save-time quarantine failed");
      balanced();
    }
  }

  // Rotation keeps the newest three copies and never grows past them.
  reset(); gSettingsLoadedOk = false; store(SETTINGS_JSON_FILE, "{bad");
  store("/system/settings.json.corrupt-1", "old1");
  store("/system/settings.json.corrupt-2", "old2");
  store("/system/settings.json.corrupt-3", "old3");
  check(!bootLoad(), "rotation fixture reported as loaded");
  check(files.at("/system/settings.json.corrupt-1")->bytes == "{bad" &&
        files.at("/system/settings.json.corrupt-2")->bytes == "old1" &&
        files.at("/system/settings.json.corrupt-3")->bytes == "old2" &&
        !files.count("/system/settings.json.corrupt-4"), "quarantine rotation is unbounded or misordered");
  balanced();

  // Out of memory: the file is intact. It stays, and saves still refuse.
  reset(); gSettingsLoadedOk = false; jsonAllocator.fail = true;
  check(!bootLoad(), "boot load succeeded without memory");
  jsonAllocator.fail = false;
  check(files.at(SETTINGS_JSON_FILE)->bytes == oldMain && !files.count(kQuarantine1) && !corruptEventFor(SETTINGS_JSON_FILE),
        "an intact file was quarantined on an out-of-memory parse");
  check(!writeSettingsJson() && files.at(SETTINGS_JSON_FILE)->bytes == oldMain, "save after an out-of-memory load overwrote the file");
  balanced();
  reset(); jsonAllocator.fail = true;
  check(!writeSettingsJson(), "merge-read without memory saved");
  jsonAllocator.fail = false;
  check(files.at(SETTINGS_JSON_FILE)->bytes == oldMain && !files.count(kQuarantine1),
        "an intact file was quarantined on an out-of-memory merge-read");
  balanced();

  // A readable file still loads normally and is not touched.
  reset(); gSettingsLoadedOk = false;
  check(bootLoad() && sectionReads > 0 && files.at(SETTINGS_JSON_FILE)->bytes == oldMain && !files.count(kQuarantine1),
        "a readable settings.json did not load normally");
  check(writeSettingsJson(), "save after a good load failed");
  balanced();

  // A quarantine that cannot move the file keeps the 2026-08-07 guard: the
  // damaged bytes stay where they are and nothing is written over them.
  for (const std::string& text : {std::string{"{\"truncated\":"}, std::string{"\xff\xfe garbage"}}) {
    for (bool factory : {false, true}) {
      reset(); store(SETTINGS_JSON_FILE, text); failRenameInto = ".corrupt-";
      check(!(factory ? writeSettingsJsonForFactoryReset() : writeSettingsJson()),
            "save went ahead after the quarantine rename failed");
      check(files.at(SETTINGS_JSON_FILE)->bytes == text && writeOpens == 0 && !files.count(kQuarantine1),
            "a failed quarantine let the save replace the unreadable file");
      check(events.size() == 1 && events[0].kind == SYSEVT_SETTINGS_SAVE_FAILED &&
            events[0].reason == "merge_read_failed", "failed save-time quarantine posted no merge_read_failed event");
      balanced();
    }
    reset(); gSettingsLoadedOk = false; store(SETTINGS_JSON_FILE, text); failRenameInto = ".corrupt-";
    check(!bootLoad() && !gSettingsLoadedOk, "boot load opened the save gate after a failed quarantine");
    check(files.at(SETTINGS_JSON_FILE)->bytes == text && !files.count(kQuarantine1) && sectionReads == 0,
          "a failed boot quarantine moved or read the file");
    check(events.size() == 1 && events[0].kind == SYSEVT_CONFIG_FILE_CORRUPT &&
          events[0].detail.find("quarantine failed") != std::string::npos,
          "failed boot quarantine posted no config_file_corrupt event");
    events.clear();
    check(!writeSettingsJson() && files.at(SETTINGS_JSON_FILE)->bytes == text && writeOpens == 0,
          "save after a failed boot quarantine overwrote the file");
    check(events.size() == 1 && events[0].reason == "load_failed", "save was not refused at the load gate");
    balanced();
  }
}
static void mergeOpenFailure() {
  reset(); readOpenFails = true; retryMask = 0; retains = 0;
  check(!writeSettingsJson(), "unopenable existing merge document was replaced from live RAM"); balanced();
  check(files.at(SETTINGS_JSON_FILE)->bytes == oldMain && builds == 0 && writeOpens == 0, "merge open failure lost original document");
  // A refused open is transient: the main file stays dirty for the background
  // retry whichever caller asked for the save.
  check(retryMask == hw1_settings_batch::MAIN && retains == 1, "refused merge open did not retain the main retry intent");
}
static void helperOverflowAndReentry() {
#if HAS_SNAPSHOT_HELPER
  reset();
  { SettingsFileWriteGuard writer; FsLockGuard outer("test.outer"); JsonDocument doc; doc["ok"] = true;
    check(writeSettingsSnapshot(doc, SETTINGS_JSON_FILE, "/system/settings.tmp", "test.helper"), "nested helper failed");
    check(ownsFs && lockTakes == 1 && lockGives == 0, "nested helper released caller's filesystem lock"); }
  balanced();
  reset();
  { PSRAM_JSON_DOC(doc); jsonAllocator.fail = true; doc["fail"] = std::string(8192, 'x'); jsonAllocator.fail = false;
    check(doc.overflowed(), "allocation boundary did not overflow real ArduinoJson");
    SettingsFileWriteGuard writer;
    check(!writeSettingsSnapshot(doc, SETTINGS_JSON_FILE, "/system/settings.tmp", "test.helper"), "overflowed document persisted"); }
  balanced();
  check(writeOpens == 0 && events.size() == 1 && events[0].reason == "overflow", "overflow not rejected before temp open");
#endif
}
static void restampOverlap() {
  reset(); interleave = true;
  std::exception_ptr restampError, mainError;
  bool restamped = false, saved = false;
  std::thread restamp([&] { try { restamped = settingsRestampFirmwareVersion(); } catch (...) { restampError = std::current_exception(); } });
  {
    std::unique_lock<std::mutex> lock(scheduleMutex);
    check(scheduleChanged.wait_for(lock, std::chrono::seconds(5), [] { return restampRead; }), "restamp failed before controlled read boundary");
  }
  std::thread main([&] {
    mainWorker = true;
    try { saved = writeSettingsJson(); } catch (...) { mainError = std::current_exception(); }
    std::lock_guard<std::mutex> lock(scheduleMutex); mainDone = true; scheduleChanged.notify_all();
  });
  bool serialized;
  {
    std::unique_lock<std::mutex> lock(scheduleMutex);
    check(scheduleChanged.wait_for(lock, std::chrono::seconds(5), [] { return writerBlocked || mainDone; }), "full writer never reached restamp guard");
    serialized = writerBlocked && !mainDone;
    resumeRestamp = true; scheduleChanged.notify_all();
  }
  restamp.join(); main.join();
  if (restampError) std::rethrow_exception(restampError);
  if (mainError) std::rethrow_exception(mainError);
  check(serialized, "restamp did not hold shared writer guard across read and replacement");
  check(restamped && saved, "serialized writers failed"); balanced();
  auto doc = parse(files.at(SETTINGS_JSON_FILE)->bytes);
  check(doc["live"] == liveValue && doc["secret"] == "AES:opaque-fixture", "restamp overwrote newer full settings snapshot");
}
static constexpr uint32_t fixtureUser = 42;
static const char* fixturePath = "/system/users/user_settings/42.json";
static const char* fixtureTemp = "/system/users/user_settings/42.json.tmp";
static const std::string oldUser = R"({"theme":"light","password":"synthetic-hash","gamepad_password":"synthetic-pattern","notificationMuted":["fixture"],"notifyLevel":1,"unknown":{"values":[2,"keep",true]}})";
static std::string serialized(const JsonDocument& doc) {
  std::string bytes; serializeJson(doc, bytes); return bytes;
}
static void userContract(unsigned expectedInvalidations, bool nested = false) {
  check(invalidations == expectedInvalidations, "per-user invalidation count differs from save attempts");
  check(writerLockAttempts == 0 && writerDepth == 0, "per-user save acquired or attempted the main settings writer mutex");
  check(events.empty() && logs.empty() && diagnosticCalls == 0, "per-user save introduced diagnostics or settings events");
  check(retryMask == hw1_settings_batch::ALL && bondHashes == 0 && builds == 0 && debugBuilds == 0 &&
        settingsFirmwareVersionRestampPending(), "per-user save changed global settings persistence policy");
  check(files.at(SETTINGS_JSON_FILE)->bytes == oldMain && files.at(DEBUG_JSON_FILE)->bytes == oldDebug,
        "per-user save changed main/debug snapshots");
  check(ownsFs == nested && activeFiles == 0 && pauses == 0 && resumes == 0,
        "per-user save changed caller ownership or polling state");
  for (const auto& owner : authOwners)
    check(owner == "user_settings.save" || owner == "user_settings.load", "per-user VFS identity changed");
  if (!nested) balanced();
}
static uint64_t byteHash(const std::string& text) {
  uint64_t value = UINT64_C(14695981039346656037);
  for (unsigned char byte : text) { value ^= byte; value *= UINT64_C(1099511628211); }
  return value;
}
static void userSuccessCases(bool transcript) {
  for (uint32_t id : {uint32_t{0}, fixtureUser, UINT32_MAX}) {
    for (bool existing : {false, true}) {
      reset(); writerAvailable = false;
      const std::string path = getUserSettingsPath(id).c_str();
      check(path == "/system/users/user_settings/" + std::to_string(id) + ".json", "numeric per-user path changed");
      if (existing) store(path.c_str(), oldUser);
      auto doc = parse(oldUser); doc["theme"] = "dark"; doc["notifyLevel"] = 2;
      const std::string expected = serialized(doc);
      check(saveUserSettings(id, doc), "ordinary per-user save failed");
      userContract(1);
      check(files.at(path)->bytes == expected && !files.count(path + ".tmp"), "per-user byte serialization or promotion changed");
      check(writeOpens == 1 && directOpens == 0 && renameCalls == 1 && removeCalls == 0,
            "ordinary per-user save did not promote exactly one sibling temp");
      check(invalidationLocks == std::vector<bool>{false}, "standalone invalidation occurred before helper released FS");
      if (transcript) std::printf("USER_JSON %u existing=%d bytes=%zu hash=%016llx\n", id, existing,
                                  expected.size(), static_cast<unsigned long long>(byteHash(files.at(path)->bytes)));
    }
  }
}
static void userAdmissionCases() {
  reset(); store(fixturePath, oldUser); auto doc = parse(oldUser);
  filesystemReady = false;
  check(!saveUserSettings(fixtureUser, doc), "unavailable filesystem accepted per-user save");
  userContract(1);
  check(writeOpens == 0 && lockTakes == 0 && files.at(fixturePath)->bytes == oldUser,
        "unavailable per-user save performed filesystem I/O");
  reset(); store(fixturePath, oldUser);
  PSRAM_JSON_DOC(overflow);
  jsonAllocator.fail = true; overflow["oversized"] = std::string(8192, 'x'); jsonAllocator.fail = false;
  check(overflow.overflowed(), "real ArduinoJson fixture did not overflow");
  check(!saveUserSettings(fixtureUser, overflow), "overflowed per-user document was saved");
  userContract(1);
  check(writeOpens == 0 && lockTakes == 0 && files.at(fixturePath)->bytes == oldUser,
        "overflow touched per-user destination");
}
static void userFailureCases() {
  for (bool existing : {false, true}) {
    for (Fault injected : {Fault::Open, Fault::Directory, Fault::Short, Fault::Size,
                           Fault::WriteError, Fault::Rename, Fault::Cleanup, Fault::RenameShort}) {
      reset(); writerAvailable = false; fault = injected;
      if (existing) store(fixturePath, oldUser);
      auto doc = parse(oldUser); doc["theme"] = "dark";
      const std::string expected = serialized(doc);
      const bool saved = saveUserSettings(fixtureUser, doc);
      userContract(1);
#if HAS_SHARED_JSON
      check(!saved && directOpens == 0, "failed per-user save reported success or opened the destination directly");
      check(files.count(fixturePath) == (existing ? 1u : 0u), "failed save created or removed the live per-user file");
      if (existing) check(files.at(fixturePath)->bytes == oldUser, "failed per-user write changed existing credentials/preferences");
      check(renameCalls == (injected == Fault::Rename || injected == Fault::RenameShort ? 1u : 0u),
            "incomplete per-user temp reached promotion");
      check(removeCalls == 1 &&
            files.count(fixtureTemp) == ((injected == Fault::Cleanup || injected == Fault::Directory) ? 1u : 0u),
            "failed per-user temp cleanup outcome incorrect");
#else
      if (injected == Fault::WriteError || injected == Fault::Rename) {
        check(saved && files.at(fixturePath)->bytes == expected,
              "baseline no longer demonstrates accepted Print-error/rename-fallback writes");
        check(directOpens == (injected == Fault::Rename ? 1u : 0u), "baseline fallback write characterization changed");
      } else if (injected == Fault::RenameShort) {
        check(!saved && directOpens == 1 && files.at(fixturePath)->bytes == expected.substr(0, 7),
              "baseline failed fallback no longer demonstrates live-file truncation");
      } else {
        check(!saved && directOpens == 0, "baseline ordinary failure behavior changed");
        check(files.count(fixturePath) == (existing ? 1u : 0u), "baseline failure changed destination existence");
        if (existing) check(files.at(fixturePath)->bytes == oldUser, "baseline pre-rename failure damaged destination");
      }
#endif
      fault = Fault::None;
      if (injected == Fault::Directory) files.erase(fixtureTemp);  // removed before a retry can succeed
      check(saveUserSettings(fixtureUser, doc), "per-user retry failed after transient storage fault");
      userContract(2);
      check(files.at(fixturePath)->bytes == expected && !files.count(fixtureTemp), "per-user retry left partial or stale bytes");
    }
  }
}
static void userNestedOwnership() {
  for (Fault injected : {Fault::None, Fault::Open, Fault::Short, Fault::Rename}) {
    reset(); writerAvailable = false; fault = injected; store(fixturePath, oldUser);
    auto doc = parse(oldUser); doc["theme"] = "dark";
    {
      FsLockGuard caller("outer.user.fixture");
      const bool saved = saveUserSettings(fixtureUser, doc);
      check(saved == (injected == Fault::None || (!HAS_SHARED_JSON && injected == Fault::Rename)),
            "nested per-user save returned an unexpected result");
      userContract(1, true);
      check(lockTakes == 1 && lockGives == 0 && invalidationLocks == std::vector<bool>{true},
            "nested helper or invalidation released the transaction owner's FS mutex");
    }
    balanced();
  }
}
static bool changeTheme(JsonDocument& doc, void* context) {
  check(ownsFs && writerDepth == 0 && activeFiles == 0 && invalidations == 0,
        "mutation did not run inside the uninterrupted load/save FS transaction");
  ++mutations;
  const int mode = context ? *static_cast<int*>(context) : 0;
  doc["theme"] = "dark";
  if (mode == 2) {
    jsonAllocator.fail = true; doc["overflow"] = std::string(8192, 'z'); jsonAllocator.fail = false;
    check(doc.overflowed(), "transaction fixture did not overflow real JSON allocator");
  }
  return mode != 1;
}
static void transactionCases() {
  for (bool existing : {false, true}) {
    reset(); writerAvailable = false;
    if (existing) store(fixturePath, oldUser);
    check(runUserSettingsTransaction(fixtureUser, changeTheme, nullptr) == UserSettingsTxnStatus::Committed,
          "ordinary user transaction failed");
    userContract(1);
    check(mutations == 1 && statCalls == 1 && lockTakes == 1 && lockGives == 1 &&
          invalidationLocks == std::vector<bool>{true}, "load, mutation, save and invalidation did not share one FS owner");
    auto doc = parse(files.at(fixturePath)->bytes);
    check(doc["theme"] == "dark", "transaction omitted mutation");
    if (existing) check(doc["password"] == "synthetic-hash" && doc["gamepad_password"] == "synthetic-pattern" &&
                        doc["unknown"]["values"][1] == "keep" && doc["notificationMuted"][0] == "fixture",
                        "transaction lost unrelated credential or preference fields");
  }
  for (int reason = 0; reason != 10; ++reason) {
    reset(); store(fixturePath, oldUser); writerAvailable = false;
    UserSettingsTxnStatus expected = UserSettingsTxnStatus::LoadFailed;
    UserSettingsMutator callback = changeTheme;
    int mode = 0;
    if (reason == 0) { callback = nullptr; expected = UserSettingsTxnStatus::InvalidArgument; }
    if (reason == 1) { filesystemReady = false; expected = UserSettingsTxnStatus::FilesystemUnavailable; }
    if (reason == 2) { gFsMutex = nullptr; expected = UserSettingsTxnStatus::LockUnavailable; }
    if (reason == 3) statFailure = EIO;
    if (reason == 4) statFailure = EACCES;
    if (reason == 5) readOpenFails = true;
    if (reason == 6 || reason == 7) continue;  // corruptUserSettingsQuarantine
    if (reason == 8) { mode = 1; expected = UserSettingsTxnStatus::MutationRejected; }
    if (reason == 9) { mode = 2; expected = UserSettingsTxnStatus::MutationRejected; }
    const std::string previous = files.at(fixturePath)->bytes;
    check(runUserSettingsTransaction(fixtureUser, callback, &mode) == expected, "wrong transaction admission/load/mutation failure");
    userContract(0);
    check(writeOpens == 0 && files.at(fixturePath)->bytes == previous, "rejected transaction attempted a save");
  }
  for (Fault injected : {Fault::Open, Fault::Short, Fault::Size, Fault::WriteError, Fault::Rename, Fault::Cleanup, Fault::RenameShort}) {
    reset(); writerAvailable = false; store(fixturePath, oldUser); fault = injected;
    const auto result = runUserSettingsTransaction(fixtureUser, changeTheme, nullptr);
    const bool oldFalseSuccess = !HAS_SHARED_JSON && (injected == Fault::WriteError || injected == Fault::Rename);
    check(result == (oldFalseSuccess ? UserSettingsTxnStatus::Committed : UserSettingsTxnStatus::SaveFailed),
          "transaction did not forward checked-save result");
    userContract(1);
    check(invalidationLocks == std::vector<bool>{true} && lockTakes == 1 && lockGives == 1,
          "failed save/invalidation lost outer transaction ownership");
#if HAS_SHARED_JSON
    check(files.at(fixturePath)->bytes == oldUser, "failed transaction replaced prior settings");
#endif
  }
  reset(); store(fixturePath, oldUser); writerAvailable = false;
  JsonDocument patch; patch["notifyLevel"] = 2; patch["theme"] = "dark";
  check(mergeAndSaveUserSettings(fixtureUser, patch), "ordinary preference merge failed");
  userContract(1);
  auto merged = parse(files.at(fixturePath)->bytes);
  check(merged["password"] == "synthetic-hash" && merged["notifyLevel"] == 2 && merged["theme"] == "dark" &&
        merged["unknown"]["values"][1] == "keep", "preference merge lost unrelated fields");
}


// An unreadable per-user credential file is quarantined with an event,
// so the account's failed logins have a visible cause, and the next password
// set writes a fresh file instead of being refused forever.
static void corruptUserSettingsQuarantine() {
  const std::string quarantine = std::string(fixturePath) + ".corrupt-1";
  for (const std::string& text : {std::string{}, std::string{"{invalid"}, std::string{"[]"},
                                  std::string{"null"}, std::string{"{\"password\":\"synthetic-hash\""}}) {
    reset(); writerAvailable = false; store(fixturePath, text);
    PSRAM_JSON_DOC(doc);
    check(!loadUserSettings(fixtureUser, doc) && doc.isNull(), "corrupt per-user file loaded");
    check(!files.count(fixturePath) && files.count(quarantine) && files.at(quarantine)->bytes == text,
          "corrupt per-user file was not quarantined byte-for-byte");
    check(corruptEventFor(fixturePath) && loggedQuarantine(quarantine.c_str()),
          "per-user quarantine posted no config_file_corrupt event or log line");
    check(writeOpens == 0 && invalidations == 0, "a failed load wrote or invalidated anything");
    check(writerLockAttempts == 0 && files.at(SETTINGS_JSON_FILE)->bytes == oldMain,
          "per-user quarantine touched the main settings writer");
    check(ownerSetupArms == 0 && !files.count(kOwnerSetupMarker), "a non-owner quarantine armed owner setup");
    // The recovery path (a password set runs this transaction) now commits.
    check(runUserSettingsTransaction(fixtureUser, changeTheme, nullptr) == UserSettingsTxnStatus::Committed,
          "password-set transaction still refused after the quarantine");
    check(parse(files.at(fixturePath)->bytes)["theme"] == "dark" && files.at(quarantine)->bytes == text,
          "recovery write lost the mutation or touched the quarantine copy");
    balanced();
  }
  // Inside a transaction the quarantine keeps the caller's lock and reports LoadFailed.
  reset(); writerAvailable = false; store(fixturePath, "{invalid");
  check(runUserSettingsTransaction(fixtureUser, changeTheme, nullptr) == UserSettingsTxnStatus::LoadFailed,
        "corrupt transaction load did not fail");
  check(mutations == 0 && writeOpens == 0 && files.at(quarantine)->bytes == "{invalid" && lockTakes == 1,
        "transaction quarantine mutated, wrote, or took the lock twice");
  balanced();
  // Out of memory: the file is intact and stays put.
  reset(); writerAvailable = false; store(fixturePath, oldUser);
  { PSRAM_JSON_DOC(doc); jsonAllocator.fail = true;
    const bool loaded = loadUserSettings(fixtureUser, doc);
    jsonAllocator.fail = false;
    check(!loaded && files.at(fixturePath)->bytes == oldUser && !files.count(quarantine) && events.empty(),
          "an intact per-user file was quarantined on an out-of-memory parse"); }
  balanced();

  // A failed quarantine rename leaves the file in place and arms nothing.
  for (uint32_t id : {fixtureUser, uint32_t{1}}) {
    reset(); writerAvailable = false;
    const std::string path = getUserSettingsPath(id).c_str();
    store(path.c_str(), "{invalid"); failRenameInto = ".corrupt-";
    PSRAM_JSON_DOC(doc);
    check(!loadUserSettings(id, doc) && files.at(path)->bytes == "{invalid" &&
          !files.count(path + ".corrupt-1"), "failed per-user quarantine moved the file");
    check(events.size() == 1 && events[0].detail.find("quarantine failed") != std::string::npos,
          "failed per-user quarantine posted no event");
    check(ownerSetupArms == 0 && !files.count(kOwnerSetupMarker), "owner setup armed without a quarantine");
    balanced();
  }

  // The owner: nobody can reset user 1, so its quarantine arms the
  // credential-setup marker. Every other file stays byte-identical.
  const std::string ownerPath = "/system/users/user_settings/1.json";
  for (bool armOk : {true, false}) {
    for (const std::string& text : {std::string{"{\"password\":"}, std::string{"[]"}}) {
      reset(); writerAvailable = false; ownerSetupArmSucceeds = armOk;
      store(ownerPath.c_str(), text); store(fixturePath, oldUser);
      std::map<std::string, std::string> before;
      for (const auto& entry : files) before[entry.first] = entry.second->bytes;
      PSRAM_JSON_DOC(doc);
      check(!loadUserSettings(1, doc), "corrupt owner file loaded");
      check(!files.count(ownerPath) && files.at(ownerPath + ".corrupt-1")->bytes == text,
            "corrupt owner file was not quarantined byte-for-byte");
      check(ownerSetupArms == 1 && files.count(kOwnerSetupMarker) == (armOk ? 1u : 0u),
            "owner quarantine did not arm the credential-setup marker");
      for (const auto& entry : before)
        if (entry.first != ownerPath)
          check(files.count(entry.first) && files.at(entry.first)->bytes == entry.second,
                "owner quarantine changed another file");
      check(files.size() == before.size() + (armOk ? 1u : 0u), "owner quarantine created an unexpected file");
      bool told = false;
      for (const auto& log : logs)
        told = told || log.find(armOk ? "next boot asks for a new owner password" : "marker write FAILED") != std::string::npos;
      check(told && corruptEventFor(ownerPath.c_str()), "owner quarantine was not logged");
      balanced();
    }
  }
  // Out of memory on the owner file: intact, nothing armed.
  reset(); writerAvailable = false; store(ownerPath.c_str(), oldUser);
  { PSRAM_JSON_DOC(doc); jsonAllocator.fail = true;
    const bool loaded = loadUserSettings(1, doc);
    jsonAllocator.fail = false;
    check(!loaded && files.at(ownerPath)->bytes == oldUser && ownerSetupArms == 0,
          "an intact owner file was quarantined or armed setup on an out-of-memory parse"); }
  balanced();
}

int main(int argc, char** argv) {
  // No registered Bluetooth module in this fixture: its orphaned credentials
  // survive ordinary merge writes, but factory reset strips only pipe fields.
  reset();store(SETTINGS_JSON_FILE,R"({"network":{"bluetooth":{"phonePipeSecret":"AES:fixture","bleSecureChannelSecret":"AES:console","peers":{"phone":{"pipeChannelVersion":1,"pipeEnrolled":true,"pipeBondVersion":1,"pipeIdentity":"legacy","pipeIdentityType":1,"mac1":"console-address","pairedByUser":"owner"},"r1-ring":{"mac1":"ring-address"}}}}})");
  check(writeSettingsJson(),"ordinary merge failed");
  auto preserved=parse(files[SETTINGS_JSON_FILE]->bytes);
  check(preserved["network"]["bluetooth"]["phonePipeSecret"]=="AES:fixture","ordinary save unexpectedly revoked pipe");
  check(writeSettingsJsonForFactoryReset(),"factory credential clear failed");
  auto cleared=parse(files[SETTINGS_JSON_FILE]->bytes);auto bluetooth=cleared["network"]["bluetooth"].as<JsonObject>();
  check(bluetooth["phonePipeSecret"].isNull(),"disabled-module pipe secret survived factory reset");
  auto phone=bluetooth["peers"]["phone"].as<JsonObject>();
  for(const char* key:{"pipeChannelVersion","pipeEnrolled","pipeBondVersion","pipeIdentity","pipeIdentityType"})check(phone[key].isNull(),"pipe metadata survived factory reset");
  check(bluetooth["bleSecureChannelSecret"]=="AES:console"&&phone["mac1"]=="console-address"&&phone["pairedByUser"]=="owner"&&bluetooth["peers"]["r1-ring"]["mac1"]=="ring-address","factory pipe clear changed unrelated credentials");
  balanced();

  try {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "all") {
      successCases(); admissionAndBuildFailures(); helperOverflowAndReentry();
      for (Writer writer : {Writer::Main, Writer::Debug, Writer::Restamp})
        for (Fault injected : {Fault::Open, Fault::Directory, Fault::Short, Fault::Size, Fault::WriteError, Fault::Rename, Fault::Cleanup})
          failureCase(writer, injected);
      corruptSettingsQuarantine();
    }
    if (selected == "all" || selected == "user" || selected == "user-parity") userSuccessCases(selected == "user-parity");
    if (selected == "all" || selected == "user") {
      userAdmissionCases(); userFailureCases(); userNestedOwnership(); transactionCases();
      corruptUserSettingsQuarantine();
    }
    if (selected == "rename") failureCase(Writer::Main, Fault::Rename);
    if (selected == "restamp-short") failureCase(Writer::Restamp, Fault::Short);
    if (selected == "all" || selected == "merge-open") mergeOpenFailure();
    if (selected == "all" || selected == "restamp-overlap") restampOverlap();
    std::printf("settings_file_writes (bonded=%d): %u checks passed\n", ENABLE_BONDED_MODE, checks.load());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "settings_file_writes: %s\n", error.what()); return 1;
  }
}
