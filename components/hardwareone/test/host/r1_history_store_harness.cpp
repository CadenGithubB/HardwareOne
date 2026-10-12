// Execute the complete R1 store source. Fake VFS/crypto/RTOS boundaries make
// write completion and lock ownership observable; they do not emulate flash.
#include <Print.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#define ENABLE_R1_HEALTH 1
#define EXT_RAM_BSS_ATTR
// INSERT_TYPES

static size_t checks = 0, cases = 0;
static void check(bool condition, const char* message) {
  ++checks;
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static std::map<std::string, std::string> disk;
static std::set<std::string> directories;
static std::map<std::string, int> openFiles;
static std::vector<std::string> operations;
static int liveFiles = 0, unguardedIo = 0, writeCalls = 0, renameCalls = 0, renamesWithoutOuterGuard = 0;
static int failWrite = 0, failSeal = 0, sealCalls = 0;
static std::set<int> failRenames;
static bool failOpen = false, failMkdir = false, failCleanup = false;
static bool loseByteAtFlush = false, errorAtFlush = false, keyReady = true, ready = true;
static std::function<void()> duringWrite;
static unsigned long nowMs = 10000;
static time_t fixedEpoch = 1800057600;
static unsigned long millis() { return nowMs; }
static time_t controlledTime(time_t* value) { if (value) *value = fixedEpoch; return fixedEpoch; }
#define time controlledTime
using TickType_t = unsigned;
struct StaticSemaphore_t { int id = 0; bool held = false; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static constexpr int pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
static StaticSemaphore_t fsMutexStorage;
static SemaphoreHandle_t gFsMutex = &fsMutexStorage;
static std::vector<SemaphoreHandle_t> lockStack;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  static int next = 0; storage->id = ++next; return storage;
}
static bool isHeldByCurrentTask(SemaphoreHandle_t mutex) { return mutex->held; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  check(!mutex->held, "non-recursive mutex acquired once");
  mutex->held = true; lockStack.push_back(mutex); return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {
  check(mutex->held && !lockStack.empty() && lockStack.back() == mutex, "release locks in reverse order");
  mutex->held = false; lockStack.pop_back();
}
// INSERT_LOCKS
// The shared filesystem guard covers each VFS call, the publish sequence and
// the recovery scan; the store's own temp writes run under its private I/O
// mutex only, so other filesystem users are not held off for a whole commit.
static void io(const std::string& name) {
  if (!gFsMutex->held) ++unguardedIo;
  operations.push_back(name);
}
static std::string str(const String& value) { return value.c_str(); }
namespace fs {
class File : public Print {
  struct Handle {
    std::string path, mode;
    bool directory = false, closed = false;
    size_t position = 0, next = 0;
    std::vector<std::string> entries;
  };
  std::shared_ptr<Handle> handle;
public:
  File() = default;
  File(const std::string& path, const std::string& mode, bool directory = false)
      : handle(std::make_shared<Handle>()) {
    handle->path = path; handle->mode = mode; handle->directory = directory; ++liveFiles;
    if (!directory) ++openFiles[path];
    if (directory) for (const auto& item : disk) {
      const std::string prefix = path + "/";
      if (item.first.compare(0, prefix.size(), prefix) == 0 &&
          item.first.find('/', prefix.size()) == std::string::npos) handle->entries.push_back(item.first);
    }
  }
  explicit operator bool() const { return handle && !handle->closed; }
  bool isDirectory() const { io("isDirectory"); check(bool(*this), "open directory query"); return handle->directory; }
  const char* name() const { io("name"); return handle->path.c_str(); }
  File openNextFile() {
    io("next"); check(bool(*this) && handle->directory, "iterate open directory");
    while (handle->next < handle->entries.size()) {
      const auto name = handle->entries[handle->next++];
      if (disk.count(name)) return File(name, "r");
    }
    return {};
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    io("write"); check(bool(*this) && handle->mode == "w", "write temporary opened for replacement");
    ++writeCalls;
    size_t accepted = writeCalls == failWrite ? count - 1 : count;
    disk.at(handle->path).append(reinterpret_cast<const char*>(bytes), accepted);
    if (duringWrite) { auto callback = std::move(duringWrite); duringWrite = {}; callback(); }
    return accepted;
  }
  void flush() override {
    io("flush"); check(bool(*this), "flush before close");
    if (loseByteAtFlush && !disk.at(handle->path).empty()) disk.at(handle->path).pop_back();
    // This is an exposed Print error, NOT a simulated detectable fsync return:
    // Arduino File::flush is void and its backend may hide fsync failures.
    if (errorAtFlush) setWriteError();
  }
  size_t size() const { io("size"); check(bool(*this), "size before close"); return disk.at(handle->path).size(); }
  bool seek(size_t position) { io("seek"); handle->position = position; return position <= disk.at(handle->path).size(); }
  int available() const { io("available"); return handle->position < disk.at(handle->path).size(); }
  String readStringUntil(char delimiter) {
    io("read"); check(bool(*this) && handle->mode == "r", "read from open input");
    const auto& bytes = disk.at(handle->path);
    size_t end = bytes.find(delimiter, handle->position);
    if (end == std::string::npos) end = bytes.size();
    const auto result = bytes.substr(handle->position, end - handle->position);
    handle->position = end < bytes.size() ? end + 1 : end;
    return String(result.c_str());
  }
  void close() { io("close"); check(bool(*this), "close exactly once"); handle->closed = true; --liveFiles; if (!handle->directory) --openFiles[handle->path]; }
};
}
using fs::File;
struct AuthContext { std::string scope, reason; };
namespace VFS {
namespace Scopes { static const char* CAPTURES = "captures"; }
static AuthContext systemAuth(const char* scope, const char* reason) { return {scope, reason}; }
static bool isLittleFSReady() { return ready; }
static void authorized(const String& path, const AuthContext& auth) {
  check(auth.scope == Scopes::CAPTURES && auth.reason.rfind("R1 history ", 0) == 0, "confined system capture authorization");
  check(str(path) == "/logging_captures" || str(path).rfind("/logging_captures/r1-health-history", 0) == 0,
        "R1 operations remain in capture subtree");
}
static File openGuarded(const String& path, const char* mode, const AuthContext& auth, bool create = false) {
  FsLockGuard guard("vfs.open"); io("open:" + str(path)); authorized(path, auth);
  if (std::string(mode) == "w") {
    check(create && str(path).size() > 4 && str(path).substr(str(path).size() - 4) == ".tmp", "only temporary is truncated");
    if (failOpen) return {};
    disk[str(path)] = ""; return File(str(path), mode);
  }
  check(!create && std::string(mode) == "r", "read-only store opens");
  if (directories.count(str(path))) return File(str(path), mode, true);
  return disk.count(str(path)) ? File(str(path), mode) : File();
}
static bool existsGuarded(const String& path, const AuthContext& auth) {
  FsLockGuard guard("vfs.exists"); io("exists"); authorized(path, auth);
  return disk.count(str(path)) || directories.count(str(path));
}
static bool mkdirGuarded(const String& path, const AuthContext& auth) {
  FsLockGuard guard("vfs.mkdir"); io("mkdir"); authorized(path, auth);
  if (failMkdir) return false; directories.insert(str(path)); return true;
}
static bool removeGuarded(const String& path, const AuthContext& auth) {
  FsLockGuard guard("vfs.remove"); io("remove:" + str(path)); authorized(path, auth);
  if (failCleanup && str(path).size() > 4 && str(path).substr(str(path).size() - 4) == ".tmp") return false;
  return disk.erase(str(path)) != 0;
}
static bool renameGuarded(const String& from, const String& to, const AuthContext& auth) {
  // Every rename belongs to a publish or recovery sequence that the store
  // wraps in one guard, so the guard must already be held on entry.
  if (!gFsMutex->held) ++renamesWithoutOuterGuard;
  FsLockGuard guard("vfs.rename"); io("rename:" + str(from)); authorized(from, auth); authorized(to, auth);
  check(openFiles[str(from)] == 0 && openFiles[str(to)] == 0, "close source handles before promotion"); ++renameCalls;
  if (failRenames.count(renameCalls) || !disk.count(str(from))) return false;
  disk[str(to)] = disk.at(str(from)); disk.erase(str(from)); return true;
}
}
struct { int captureEncryptMode = 0; } gSettings;
static bool captureCryptoEnsureKey() { return keyReady; }
static bool captureCryptoPseudonym(const char* domain, const char* value, char* output, size_t capacity) {
  check(std::string(domain) == "r1-history-peer-v1" && std::strlen(value) == 17, "existing pseudonym domain and identity preserved");
  if (!keyReady || capacity < 33) return false;
  // Fixture pseudonym is deterministic, opaque, and contains no real identity.
  std::snprintf(output, capacity, "%s", "0123456789abcdef0123456789abcdef"); return true;
}
static int captureCryptoSealLine(const char* input, size_t length, char* output, size_t capacity) {
  ++sealCalls; if (!keyReady || sealCalls == failSeal) return -1;
  static const char hex[] = "0123456789abcdef";
  std::string encoded = "ENC1:";
  for (size_t i = 0; i < length; ++i) { const auto byte = static_cast<unsigned char>(input[i]); encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
  if (encoded.size() >= capacity) return -1;
  std::memcpy(output, encoded.c_str(), encoded.size() + 1); return static_cast<int>(encoded.size());
}
static int captureCryptoOpenLine(const char* line, size_t length, char* output, size_t capacity) {
  if (!keyReady || length < 5 || strncmp(line, "ENC1:", 5) || (length - 5) % 2) return -1;
  size_t count = (length - 5) / 2; if (count >= capacity) return -1;
  auto nibble = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
  for (size_t i = 0; i < count; ++i) {
    int hi = nibble(line[5 + 2 * i]), lo = nibble(line[6 + 2 * i]); if (hi < 0 || lo < 0) return -1;
    output[i] = static_cast<char>((hi << 4) | lo);
  }
  output[count] = 0; return static_cast<int>(count);
}
static bool captureCryptoIsMagicLine(const char* line) { return strncmp(line, "#HW1ENC", 7) == 0; }
// INSERT_PRODUCTION

static void released() { check(liveFiles == 0 && lockStack.empty(), "all handles and locks released"); }
static void reset() {
  released(); ++cases; disk.clear(); openFiles.clear(); directories = {"/logging_captures", kHistoryDir}; operations.clear();
  failWrite = writeCalls = renameCalls = failSeal = sealCalls = 0; failRenames.clear();
  failOpen = failMkdir = failCleanup = loseByteAtFlush = errorAtFlush = false;
  keyReady = ready = true; duringWrite = {}; nowMs = 10000; gSettings.captureEncryptMode = 0;
  sStagedGeneration = 0; sCommitPending = false; sNextRetryMs = 0; sRetryDelayMs = kRetryInitialMs; sStatus = {};
  sStatus.initialized = true; sStatus.available = true;
}
static R1HealthHistoryDay fixture(bool dense = false) {
  R1HealthHistoryDay day; r1HealthHistoryDayClear(day);
  snprintf(day.peerId, sizeof(day.peerId), "%s", "02:00:00:00:00:01");
  day.protocolProfile = R1_HISTORY_LAYOUT_DAILY_V1; day.dayStart = static_cast<uint32_t>(fixedEpoch);
  day.timezoneMinutes = -240; day.fetchState = R1_HISTORY_COMPLETE; day.sleepState = R1_HISTORY_SLEEP_PRESENT;
  day.lastSuccessEpoch = day.dayStart + 3600;
  for (auto metric : {&day.heartRate, &day.hrv, &day.spo2}) {
    metric->have = metric->latestValid = true; metric->count = dense ? 24 : 1;
    metric->sourceSerial = 7; metric->sourceCrc32 = 0xA0B1C2D3; metric->latestTimestamp = day.dayStart + 3600; metric->latestValue = 72;
    for (size_t i = 0; i < metric->count; ++i) {
      auto& bucket = metric->slots[i]; bucket.valid = true; bucket.hourSlot = static_cast<uint8_t>(i);
      bucket.average = 72; bucket.minimum = 60; bucket.maximum = 85; bucket.bucketEpoch = day.dayStart + static_cast<uint32_t>(i * 3600);
    }
  }
  day.activity.have = day.activity.fullDayVerified = true; day.activity.count = dense ? 144 : 1;
  day.activity.sourceSerial = 10; day.activity.sourceCrc32 = 0x12345678;
  for (size_t i = 0; i < day.activity.count; ++i) {
    auto& bucket = day.activity.slots[i]; bucket.valid = true; bucket.tenMinuteSlot = static_cast<uint8_t>(i);
    bucket.steps = 100; bucket.activeKcal = 2; bucket.restingKcal = 3; bucket.totalKcal = 5;
    bucket.bucketEpoch = day.dayStart + static_cast<uint32_t>(i * 600);
  }
  check(r1HealthHistoryDayValidate(day), "fixture satisfies production validation"); return day;
}
static std::string primary(const R1HealthHistoryDay& day) {
  char path[160]; check(makePath(day.peerId, day.dayStart, day.timezoneMinutes, path, sizeof(path)), "fixture path"); return path;
}
static R1HealthHistoryStoreStatus status() { R1HealthHistoryStoreStatus value; r1HealthHistoryStoreGetStatus(value); return value; }
static void save(const R1HealthHistoryDay& day, uint32_t generation) {
  check(r1HealthHistoryStoreStage(day, generation), "stage accepted"); r1HealthHistoryStoreTick(); released();
}
static void good(const R1HealthHistoryDay& day, uint32_t generation) {
  auto st = status(); check(st.error == R1_HISTORY_STORE_OK && st.committedGeneration == generation && !st.commitPending, "complete generation acknowledged");
  check(st.lastContentHash == contentHash(day), "committed content hash matches input");
  R1HealthHistoryDay loaded; check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, loaded) == R1_HISTORY_STORE_OK, "saved file reloads");
  check(std::memcmp(&day, &loaded, sizeof(day)) == 0, "real parser round trips all history fields"); released();
}
static void normal() {
  for (bool dense : {false, true}) for (int mode : {0, 1, 2, 99}) {
    reset(); gSettings.captureEncryptMode = mode; auto day = fixture(dense); const auto path = primary(day);
    save(day, 1); good(day, 1);
    check(disk.size() == 1 && disk.count(path), "one primary; no backup or temporary");
    check(path.find(day.peerId) == std::string::npos, "filename uses pseudonym");
    check((disk.at(path).find(day.peerId) == std::string::npos) == (mode != 0), "nonzero policy keeps identity sealed");
    const auto original = disk.at(path); int writes = writeCalls;
    save(day, 2); good(day, 2); check(status().lastCommitSkipped && writeCalls == writes, "identical content skips replacement");
    day.activity.slots[0].steps += 5; save(day, 3); good(day, 3);
    check(disk.at(path) != original && disk.size() == 1 && !status().lastCommitSkipped, "changed content replaces previous generation");
    std::cout << "normal:" << dense << ':' << mode << ':' << std::quoted(path) << '\n' << disk.at(path);
  }
}
static void failureCases() {
  for (int mode : {0, 1}) for (int fault = 0; fault < 9; ++fault) {
    reset(); gSettings.captureEncryptMode = mode; auto day = fixture(); const auto path = primary(day);
    save(day, 1); good(day, 1); const auto before = disk.at(path);
    day.activity.slots[0].steps += 1; writeCalls = renameCalls = sealCalls = 0;
    switch (fault) {
      case 0: failWrite = 1; break; // header body
      case 1: failWrite = 2; break; // header LF
      case 2: failWrite = 7; break; // later payload
      case 3: loseByteAtFlush = true; break; // exposed final-size mismatch
      case 4: errorAtFlush = true; break; // sticky Print error
      case 5: failOpen = true; break;
      case 6: failRenames = {1}; break; // primary -> backup
      case 7: failRenames = {2}; break; // temp -> primary; rollback succeeds
      case 8: failWrite = 8; failCleanup = true; break;
    }
    save(day, 2);
    if (BASELINE && (fault == 3 || fault == 4)) {
      check(!status().commitPending && status().committedGeneration == 2, "baseline reproduces unchecked completion acknowledgement");
      check(disk.at(path) != before, "baseline promotes temporary despite exposed failure");
      continue;
    }
    auto st = status(); check(st.error != R1_HISTORY_STORE_OK && st.commitPending && st.stagedGeneration == 2 && st.committedGeneration == 1,
                             "failed new generation remains pending and preserves acknowledgement");
    check(disk.at(path) == before && !disk.count(path + ".bak"), "failure preserves previous primary");
    check(disk.count(path + ".tmp") == static_cast<size_t>(fault == 8), "failed temporary removed unless backend rejects cleanup");
    const auto failedWrites = writeCalls; nowMs += 1999; r1HealthHistoryStoreTick(); released();
    check(writeCalls == failedWrites && status().commitPending, "retry delay prevents immediate rewrite");
    failWrite = 0; failOpen = failCleanup = loseByteAtFlush = errorAtFlush = false; failRenames.clear();
    nowMs += 1; r1HealthHistoryStoreTick(); released(); good(day, 2);
    check(disk.size() == 1 && disk.at(path) != before, "retry removes orphan and commits complete generation");
  }
  reset(); gSettings.captureEncryptMode = 1; auto day = fixture(); const auto path = primary(day);
  save(day, 1); const auto before = disk.at(path); ++day.activity.slots[0].steps;
  sealCalls = 0; failSeal = 2; save(day, 2);
  check(status().error == R1_HISTORY_STORE_WRITE_FAILED && status().commitPending && disk.at(path) == before, "seal failure fails closed preserving old generation");
  reset(); day = fixture(); keyReady = false; save(day, 1);
  check(status().error == R1_HISTORY_STORE_KEY_UNAVAILABLE && status().commitPending && disk.empty(), "missing pseudonym key never writes plaintext");
  keyReady = true; nowMs += 2000; r1HealthHistoryStoreTick(); good(day, 1);
  reset(); day = fixture(); directories.clear(); failMkdir = true; save(day, 1);
  check(status().error == R1_HISTORY_STORE_NOT_READY && status().commitPending && disk.empty(), "directory failure remains retryable");
}
static void firstSaveFailures() {
  for (int fault = 0; fault < 4; ++fault) {
    reset(); const auto day = fixture(); const auto path = primary(day);
    if (fault == 0) failWrite = 4;
    if (fault == 1) loseByteAtFlush = true;
    if (fault == 2) errorAtFlush = true;
    if (fault == 3) failRenames = {1};
    save(day, 7);
    if (BASELINE && (fault == 1 || fault == 2)) {
      check(status().committedGeneration == 7 && disk.count(path), "baseline exposes unchecked first-save completion");
      continue;
    }
    check(status().commitPending && status().committedGeneration == 0 && disk.empty(), "failed first save publishes no primary or acknowledgement");
    failWrite = 0; loseByteAtFlush = errorAtFlush = false; failRenames.clear();
    nowMs += 2000; r1HealthHistoryStoreTick(); good(day, 7);
  }
  reset(); const auto day = fixture(); const auto path = primary(day); save(day, 1);
  disk[path][disk[path].find("I,") + 2] = 'f';
  R1HealthHistoryDay loaded;
  check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, loaded) == R1_HISTORY_STORE_READ_FAILED,
        "real parser rejects modified integrity metadata");
  check(loaded.peerId[0] == 0 && loaded.dayStart == 0 && loaded.schemaVersion == R1_HEALTH_HISTORY_SCHEMA_VERSION,
        "failed restore clears output instead of publishing partial parse"); released();
}
static void recoveryAndGeneration() {
  reset(); auto day = fixture(); const auto path = primary(day); save(day, 1); const auto before = disk.at(path);
  ++day.activity.slots[0].steps; renameCalls = 0; failRenames = {2, 3}; save(day, 2);
  check(status().error == R1_HISTORY_STORE_RENAME_FAILED && status().commitPending && !disk.count(path) && disk.at(path + ".bak") == before,
        "failed promotion and rollback retain authoritative backup for recovery");
  failRenames.clear(); nowMs += 2000; r1HealthHistoryStoreTick(); good(day, 2); check(disk.size() == 1, "retry recovers backup then replaces it");
  reset(); day = fixture(); auto newer = day; ++newer.activity.slots[0].steps;
  duringWrite = [&] { check(r1HealthHistoryStoreStage(newer, 2), "new generation stages while snapshot writes"); };
  save(day, 1); check(status().committedGeneration == 1 && status().stagedGeneration == 2 && status().commitPending, "old snapshot cannot acknowledge newer generation");
  r1HealthHistoryStoreTick(); good(newer, 2);
  reset(); day = fixture(); {
    FsLockGuard outer("test.caller"); check(r1HealthHistoryStoreStage(day, 1), "stage with outer guard"); r1HealthHistoryStoreTick();
    check(gFsMutex->held, "nested store preserves caller's FS ownership");
    R1HealthHistoryDay loaded; check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, loaded) == R1_HISTORY_STORE_OK, "nested exact load");
    check(gFsMutex->held, "load preserves caller's FS ownership");
  } released(); good(day, 1);
}
int main() {
  // Exercise actual boot-time recovery before the function-static initialization.
  reset(); ready = false; r1HealthHistoryStoreTick(); released();
  check(!status().initialized && status().error == R1_HISTORY_STORE_NOT_READY, "failed first initialization remains retryable");
  ready = true; const auto path = primary(fixture()); disk[path + ".bak"] = "old backup"; disk[path + ".tmp"] = "partial";
  auto other = fixture(); other.dayStart -= 86400; const auto otherPath = primary(other);
  disk[otherPath] = "authoritative primary"; disk[otherPath + ".bak"] = "stale backup"; disk[otherPath + ".tmp"] = "partial";
  r1HealthHistoryStoreTick(); released(); check(status().initialized && disk.size() == 2 && disk.at(path) == "old backup" && disk.at(otherPath) == "authoritative primary",
                                             "boot restores missing primary but retains existing primary, discarding stale backups and temps");
  normal(); failureCases(); firstSaveFailures(); recoveryAndGeneration();
  if (!BASELINE) {
    check(unguardedIo > 0, "store data writes still run under the shared filesystem guard");
    check(renamesWithoutOuterGuard == 0, "a publish or recovery rename ran outside its sequence guard");
  }
  std::cerr << cases << " scenarios; " << checks << " assertions passed\n";
}
