#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "System_PollPause.h"

static void check(bool value, const std::string& reason) {
  if (!value) throw std::runtime_error(reason);
}
static std::atomic<bool> failNextStringCopy{false}, failNextReserve{false}, failNextConcat{false};
static std::atomic<unsigned> concatCalls{0}, failConcatCall{0};
class String {
 public:
  String() = default;
  String(const char* text) { if (!failCopy()) value_ = text ? text : ""; }
  String(const String& other) { if (!failCopy()) value_ = other.value_; }
  String(String&&) = default;
  String& operator=(const String&) = default;
  String& operator=(String&&) = default;
  size_t length() const { return value_.length(); }
  const char* c_str() const { return value_.c_str(); }
  bool reserve(size_t bytes) {
    if (failNextReserve.exchange(false)) return false;
    value_.reserve(bytes); return true;
  }
  bool concat(const char* text) {
    const unsigned call = ++concatCalls;
    if (failNextConcat.exchange(false) || call == failConcatCall) return false;
    value_ += text; return true;
  }
  bool concat(const String& text) { return concat(text.c_str()); }
  String& operator+=(const char* text) { concat(text); return *this; }
  friend String operator+(const String& left, const char* right) {
    String result(left);
    if (!result.concat(right)) result.value_.clear();  // StringSumHelper invalidates a failed sum.
    return result;
  }
 private:
  static bool failCopy() { return failNextStringCopy.exchange(false); }
  std::string value_;
};

using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
struct FakeSemaphore { std::mutex mutex; } fsSemaphore;
using SemaphoreHandle_t = FakeSemaphore*;
static SemaphoreHandle_t gFsMutex = &fsSemaphore;
static thread_local bool ownsFs = false;
static thread_local unsigned ownPauses = 0;
static thread_local unsigned worker = 0;
static std::atomic<unsigned> lockTakes{0}, lockGives{0}, pauseDepth{0}, pauseCalls{0}, resumeCalls{0};
volatile bool gSensorPollingPaused = false;
void pollPause(uint8_t) { ++ownPauses; ++pauseDepth; ++pauseCalls; }
void pollResume(uint8_t) { check(ownPauses > 0, "poll resume must match this task's pause"); --ownPauses; --pauseDepth; ++resumeCalls; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }
static bool isHeldByCurrentTask(SemaphoreHandle_t) { return ownsFs; }
static bool isFsLockedByCurrentTask() { return ownsFs; }
static std::mutex scheduleMutex;
static std::condition_variable scheduleChanged;
static bool interleave = false, firstUnlocked = false, secondFinished = false;
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
  check(!ownsFs, "non-recursive RTOS mutex must only be taken by outer guard");
  sem->mutex.lock(); ownsFs = true; ++lockTakes; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t sem) {
  check(ownsFs, "only current owner releases FS mutex");
  ownsFs = false; ++lockGives; sem->mutex.unlock();
  if (interleave && worker == 1) {
    std::unique_lock<std::mutex> lock(scheduleMutex);
    if (!firstUnlocked) {
      firstUnlocked = true; scheduleChanged.notify_all();
      check(scheduleChanged.wait_for(lock, std::chrono::seconds(5), [] { return secondFinished; }),
            "competing save did not complete after first FS unlock");
    }
  }
}
// INSERT_FS_GUARD

enum class Fault { None, Open, Short, Zero, WriteError, FlushError, SizeMismatch, Rename, Cleanup, RenameCleanup, Directory };
static Fault fault = Fault::None;
struct Node { std::string bytes; bool writeError = false; bool flushed = false; bool directory = false; };
static std::map<std::string, std::shared_ptr<Node>> files;
static unsigned opens = 0, prints = 0, flushes = 0, closes = 0, renames = 0, removes = 0, unpausedIo = 0;
static std::vector<std::string> promoted;
static void io() {
  check(ownsFs, "filesystem operation must run under FS lock");
  if (!ownPauses) ++unpausedIo;
}
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Node> node) : node_(std::move(node)) {}
  explicit operator bool() const { return !!node_; }
  bool isDirectory() const { return node_->directory; }
  size_t print(const String& text) {
    io(); ++prints;
    size_t count = text.length();
    if (fault == Fault::Short || fault == Fault::Cleanup) count = std::min(size_t(2), count);
    if (fault == Fault::Zero) count = 0;
    node_->bytes.assign(text.c_str(), count);
    if (fault == Fault::WriteError) node_->writeError = true;
    return count;
  }
  void flush() {
    io(); ++flushes; node_->flushed = true;
    if (fault == Fault::FlushError) node_->writeError = true;
  }
  int getWriteError() const { io(); check(node_->flushed, "completion error checked after flush"); return node_->writeError; }
  size_t size() const {
    io(); check(node_->flushed, "completion size checked after flush");
    return node_->bytes.size() + (fault == Fault::SizeMismatch ? 1 : 0);
  }
  void close() { io(); ++closes; node_.reset(); }
 private:
  std::shared_ptr<Node> node_;
};
struct AuthContext {};
namespace VFS {
File open(const String& path, const char* mode, bool create) {
  FsLockGuard guard("mock.VFS.open"); io(); ++opens;
  check(std::strcmp(mode, "w") == 0 && create, "writer requests explicit create/truncate");
  if (fault == Fault::Open) return {};
  auto node = std::make_shared<Node>(); node->directory = fault == Fault::Directory;
  files[path.c_str()] = node; return File(node);
}
bool rename(const String& from, const String& to) {
  FsLockGuard guard("mock.VFS.rename"); io(); ++renames;
  auto found = files.find(from.c_str());
  if (fault == Fault::Rename || fault == Fault::RenameCleanup || found == files.end()) return false;
  promoted.push_back(std::to_string(worker) + ":" + found->second->bytes);
  files[to.c_str()] = found->second; files.erase(found); return true;
}
bool remove(const String& path) {
  FsLockGuard guard("mock.VFS.remove"); io(); ++removes;
  if (fault == Fault::Cleanup || fault == Fault::RenameCleanup) return false;
  return files.erase(path.c_str()) != 0;
}
// The writers run every open, rename and remove through the guarded VFS as
// the identity their caller passes. This harness exercises the write
// algorithm, so the guarded forwarders dispatch straight to the raw fakes.
File openGuarded(const String& path, const char* mode, const AuthContext&, bool create = false) { return open(path, mode, create); }
bool renameGuarded(const String& from, const String& to, const AuthContext&) { return rename(from, to); }
bool removeGuarded(const String& path, const AuthContext&) { return remove(path); }
}
static const AuthContext kWriterCtx{};
// INSERT_WRITERS

static constexpr const char* target = "/system/test.json";
static constexpr const char* temporary = "/system/test.json.tmp";
static void balanced() {
  check(!ownsFs && !ownPauses && pauseDepth == 0, "all lock and pause scopes released");
  check(lockTakes == lockGives && pauseCalls == resumeCalls, "balanced mutex and poll operations");
}
static void reset(Fault next = Fault::None) {
  balanced(); files.clear(); promoted.clear(); fault = next;
  opens = prints = flushes = closes = renames = removes = unpausedIo = 0;
  lockTakes = lockGives = pauseCalls = resumeCalls = 0;
  interleave = firstUnlocked = secondFinished = false;
  failNextStringCopy = failNextReserve = failNextConcat = false;
  concatCalls = failConcatCall = 0;
  files[target] = std::make_shared<Node>(Node{"original", false, false});
}
static std::string destination() { return files.at(target)->bytes; }
static void failedWrite(Fault failure, bool atomic) {
  reset(failure);
  const bool result = atomic ? writeTextAtomic(target, "replacement", kWriterCtx) : writeText(target, "replacement", kWriterCtx);
  check(!result, std::string(atomic ? "atomic" : "direct") + " writer returned success; destination='" + destination() + "'");
  if (atomic) {
    check(destination() == "original", "failed atomic save preserves prior destination");
    check(!renames, "incomplete data never reaches rename");
    if (failure != Fault::Cleanup) check(!files.count(temporary), "failed atomic save removes partial temp");
    else check(files.count(temporary) == 1 && removes == 1, "cleanup refusal is attempted without reporting success");
  }
  check(closes == (failure == Fault::Open ? 0u : 1u), "every opened file is closed");
  check(!unpausedIo, "entire write/cleanup remains polling-paused"); balanced();
}
static void successfulWrite(bool atomic, const char* text) {
  reset();
  check(atomic ? writeTextAtomic(target, text, kWriterCtx) : writeText(target, text, kWriterCtx), "valid write succeeds");
  check(destination() == text && !files.count(temporary), "saved bytes match including empty content");
  check(opens == 1 && prints == 1 && flushes == 1 && closes == 1, "one complete file lifecycle");
  check(renames == unsigned(atomic) && !removes && !unpausedIo, "only complete atomic save promotes under pause"); balanced();
}
static void renameFailure() {
  reset(Fault::Rename);
  check(!writeTextAtomic(target, "replacement", kWriterCtx), "rename refusal returns false");
  check(destination() == "original" && !files.count(temporary), "rename failure preserves original and removes temp");
  check(renames == 1 && removes == 1 && !unpausedIo, "failed promotion cleans up within operation scope"); balanced();
}
static void nestedScopes(Fault failure = Fault::None) {
  reset(failure);
  {
    PollPauseGuard poll;
    FsLockGuard fs("outer.caller");
    check(writeTextAtomic(target, "nested", kWriterCtx) == (failure == Fault::None), "nested caller receives actual save result");
    check(ownsFs && ownPauses == 1 && pauseDepth == 1 && lockTakes == 1 && lockGives == 0,
          "nested helper preserves caller's mutex and pause ownership");
  }
  check(destination() == (failure == Fault::None ? "nested" : "original") && !unpausedIo,
        "nested save or cleanup completes under caller scopes"); balanced();
}
static void existingTemp(Fault failure) {
  reset(failure);
  files[temporary] = std::make_shared<Node>(Node{"stale temp"});
  const bool okay = writeTextAtomic(target, "replacement", kWriterCtx);
  check(okay == (failure == Fault::None), "stale temporary file does not alter write outcome");
  check(destination() == (okay ? "replacement" : "original"), "stale temp cannot replace preserved destination on failure");
  if (failure == Fault::RenameCleanup) check(files.count(temporary) == 1 && removes == 1,
        "failed rename and failed cleanup retain false with original intact");
  else check(!files.count(temporary), "success or failed-write cleanup leaves no stale temp");
  check(!unpausedIo, "stale-temp handling stays inside polling scope"); balanced();
}
static void newEmptyDestination() {
  reset(); files.erase(target);
  check(writeTextAtomic(target, "", kWriterCtx), "new empty destination is a valid atomic save");
  check(destination().empty() && !files.count(temporary) && renames == 1,
        "new empty file is promoted exactly once"); balanced();
}
static void invalidPaths() {
  for (const char* path : {static_cast<const char*>(nullptr), ""}) {
    for (bool atomic : {false, true}) {
      reset();
      check(!(atomic ? writeTextAtomic(path, "value", kWriterCtx) : writeText(path, "value", kWriterCtx)), "missing path rejected");
      check(!opens && !renames && !removes && destination() == "original", "missing path performs no filesystem operation");
      balanced();
    }
  }
}
static void allocationFailures() {
  for (bool atomic : {false, true}) {
    for (unsigned stage = 1; stage <= (atomic ? 3u : 1u); ++stage) {
      reset(); files[temporary] = std::make_shared<Node>(Node{"stale temp"});
      const String input("replacement"); failConcatCall = stage;
      check(!(atomic ? writeTextAtomic(target, input, kWriterCtx) : writeText(target, input, kWriterCtx)), "path concat allocation failure rejected");
      check(!opens && !renames && !removes && destination() == "original" && files.at(temporary)->bytes == "stale temp",
            "partial path construction touches neither destination nor temp"); balanced();
    }
  }
  reset(); failConcatCall = 4;
  check(!writeTextAtomic(target, "replacement", kWriterCtx), "nested writer's path allocation failure propagates");
  check(!opens && !renames && removes == 1 && destination() == "original" && !unpausedIo,
        "inner allocation failure preserves destination and balances best-effort cleanup"); balanced();
}
static const char* faultName(Fault failure) {
  switch (failure) {
    case Fault::Open: return "open refused";
    case Fault::Zero: return "zero-byte write";
    case Fault::WriteError: return "write-error indicator";
    case Fault::FlushError: return "error indicator after flush";
    case Fault::SizeMismatch: return "post-flush size mismatch";
    case Fault::Directory: return "directory rejected";
    default: return "other fault";
  }
}
static void overlap() {
  reset(); interleave = true;
  bool firstResult = false, secondResult = false;
  std::exception_ptr firstError, secondError;
  std::thread first([&] {
    worker = 1;
    try { firstResult = writeTextAtomic(target, "first", kWriterCtx); } catch (...) { firstError = std::current_exception(); }
  });
  std::thread second([&] {
    worker = 2;
    try {
      std::unique_lock<std::mutex> lock(scheduleMutex);
      check(scheduleChanged.wait_for(lock, std::chrono::seconds(5), [] { return firstUnlocked; }), "first save never releases mutex");
      lock.unlock(); secondResult = writeTextAtomic(target, "second", kWriterCtx);
    } catch (...) { secondError = std::current_exception(); }
    { std::lock_guard<std::mutex> lock(scheduleMutex); secondFinished = true; }
    scheduleChanged.notify_all();
  });
  first.join(); second.join(); interleave = false;
  if (firstError) std::rethrow_exception(firstError);
  if (secondError) std::rethrow_exception(secondError);
  check(firstResult && secondResult, "overlap consumed another save's temp: first=" + std::to_string(firstResult) + " second=" + std::to_string(secondResult));
  check(promoted == std::vector<std::string>({"1:first", "2:second"}), "each save promotes its own complete bytes in mutex order");
  check(destination() == "second" && !files.count(temporary) && !unpausedIo, "later serialized save wins with no temp left"); balanced();
}
int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  unsigned failures = 0, scenarios = 0;
  const auto run = [&](const std::string& name, const std::function<void()>& test) {
    ++scenarios;
    try { test(); std::printf("PASS %s\n", name.c_str()); }
    catch (const std::exception& error) { ++failures; std::printf("FAIL %s: %s\n", name.c_str(), error.what()); }
  };
  if (selected == "short-write" || selected == "all") {
    for (bool atomic : {false, true}) run(atomic ? "atomic short write" : "direct short write", [=] { failedWrite(Fault::Short, atomic); });
  }
  if (selected == "overlap" || selected == "all") run("competing same-target saves", overlap);
  if (selected == "all") {
    for (bool atomic : {false, true}) {
      const std::string name = atomic ? "atomic " : "direct ";
      for (Fault f : {Fault::Zero, Fault::WriteError, Fault::FlushError, Fault::SizeMismatch, Fault::Open, Fault::Directory})
        run(name + faultName(f), [=] { failedWrite(f, atomic); });
      run(name + "success", [=] { successfulWrite(atomic, "replacement"); });
      run(name + "empty success", [=] { successfulWrite(atomic, ""); });
    }
    run("cleanup failure retains false", [] { failedWrite(Fault::Cleanup, true); });
    run("rename refusal preserves destination", renameFailure);
    run("nested caller scopes", [] { nestedScopes(); });
    run("nested failed-write scopes", [] { nestedScopes(Fault::Short); });
    for (Fault f : {Fault::None, Fault::Short, Fault::Open, Fault::RenameCleanup})
      run("preexisting temporary file " + std::to_string(static_cast<int>(f)), [=] { existingTemp(f); });
    run("new empty destination", newEmptyDestination);
    run("invalid paths before I/O", invalidPaths);
    run("checked path allocation before I/O", allocationFailures);
  }
  std::printf("Checked production text writes: %u scenarios, %u failures\n", scenarios, failures);
  return failures ? 1 : 0;
}
