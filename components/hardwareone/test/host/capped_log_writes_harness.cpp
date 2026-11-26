// Executes production code against observable storage and allocation boundaries.
#include <Print.h>
#include "string_heap_count.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static size_t checks = 0, cases = 0;
static void check(bool value, const char* description) {
  ++checks;
  if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
static std::string text(const String& value) { return value.c_str(); }
static std::map<std::string, std::string> disk;
static std::vector<std::string> events;
static int lockDepth = 0, liveFiles = 0, allocCalls = 0;
static bool recovering = false, overflow = false, failAlloc = false, failSeek = false, sdAvailable = false;
static bool failRename = false, appendError = false, copyError = false, flushError = false;
static bool failAppendOpen = false, failReadOpen = false, failTempOpen = false;
// G-39: the overflow latch's tier answer. sdWritable is what the resolver
// sees; flashBelowReserve false models a latch that re-armed (space returned).
static bool sdWritable = false, flashBelowReserve = true, failSdAppendOpen = false;
static int sdFailures = 0, reserveDrops = 0;
// B11: a card that silently loses what a handle buffered, and a card file
// that fails while the card itself is fine (see File::write and
// VFS::noteLogCardFileFailure below).
static size_t cardKeepsBytes = SIZE_MAX;
static bool cardProbeFine = false;
static int cardFileFailures = 0;
static std::string heldLog;
static bool isTemp(const std::string& path) { return path.size() > 4 && path.compare(path.size() - 4, 4, ".tmp") == 0; }
static int failAppendWrite = 0, appendWrites = 0, failCopyWrite = 0, copyWrites = 0;
static int failCopyRead = 0, copyReads = 0, failScanRead = 0, scanReads = 0;
static size_t shortCount = 0, bytesNoted = 0, requestedSpace = 0, maxRead = 0;
static size_t tempSizeOffset = 0, inputSizeOffset = 0, readLimit = 1024;
static std::string refuseRemove;
static std::vector<std::unique_ptr<uint8_t[]>> allocations;
// S-8: the batch mutex. Order: the filesystem lock, then the mutex; file I/O
// never runs with the mutex held.
static int mutexDepth = 0, mutexTakes = 0;
static void io(const std::string& event) {
  check(lockDepth == 1 || recovering, "append I/O stays under filesystem lock");
  check(mutexDepth == 0, "no file I/O while the batch mutex is held");
  events.push_back(event);
}
struct FsLockGuard {
  explicit FsLockGuard(const char*) {
    check(lockDepth == 0, "one filesystem lock");
    check(mutexDepth == 0, "the filesystem lock is never taken with the batch mutex held");
    ++lockDepth; events.push_back("lock");
  }
  ~FsLockGuard() { check(liveFiles == 0, "files closed before unlock"); --lockDepth; events.push_back("unlock"); }
};
static bool isFsLockedByCurrentTask() { return lockDepth != 0; }
enum class AllocPref { PreferPsram, RequirePsram };
// log_out's kept line block (appendTimestampedLine): asked outside the FS lock.
#define DEBUG_MSG_SIZE 256
static int lineAllocCalls = 0;
static bool failLineAlloc = false;
static std::vector<std::unique_ptr<char[]>> lineBlocks;
// S-8: the batch buffers and the log names. PSRAM only, never under the
// filesystem lock's I/O accounting.
static int batchAllocCalls = 0;
static bool failBatchAlloc = false;
static std::vector<std::unique_ptr<char[]>> batchBlocks;
static int sealedAllocCalls = 0;
static std::vector<std::unique_ptr<char[]>> sealedBlocks;
static void ps_free(void*) {}
static void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  if (std::string(tag) == "log.batch" || std::string(tag) == "log.batch_paths") {
    ++batchAllocCalls;
    check(pref == AllocPref::RequirePsram, "batch memory is PSRAM only");
    check(size == (std::string(tag) == "log.batch" ? size_t(2560) : size_t(6 * 80)), "batch block sizes");
    if (failBatchAlloc) return nullptr;
    batchBlocks.push_back(std::make_unique<char[]>(size));
    return batchBlocks.back().get();
  }
  if (std::string(tag) == "log.capped_line") {
    ++lineAllocCalls;
    check(size == 48 + DEBUG_MSG_SIZE && pref == AllocPref::RequirePsram && lockDepth == 0,
          "one PSRAM-only line block of prefix + DEBUG_MSG_SIZE, asked outside the lock");
    if (failLineAlloc) return nullptr;
    lineBlocks.push_back(std::make_unique<char[]>(size));
    return lineBlocks.back().get();
  }
  if (std::string(tag) == "log.sealplain" || std::string(tag) == "log.sealbatch") {
    check(pref == AllocPref::PreferPsram && lockDepth == 0,
          "a log batch is sealed outside the filesystem lock");
    check(size > 0 && size <= 6u * 2560u * 46u, "seal staging stays within one batch per slot");
    sealedBlocks.push_back(std::make_unique<char[]>(size));
    return sealedBlocks.back().get();
  }
  if (std::string(tag).rfind("sealedlog.", 0) == 0) {
    // B11: the sealed-log row, plain and carry buffers (System_SealedLog.cpp).
    ++sealedAllocCalls;
    check(pref == AllocPref::PreferPsram && size <= 4097, "the sealed-log buffers are small PSRAM blocks");
    sealedBlocks.push_back(std::make_unique<char[]>(size));
    return sealedBlocks.back().get();
  }
  ++allocCalls; io("alloc");
  check(size == 1024 && pref == AllocPref::PreferPsram && std::string(tag) == "logrot.buf", "retained 1 KB PSRAM buffer policy");
  if (failAlloc) return nullptr;
  allocations.push_back(std::make_unique<uint8_t[]>(size));
  return allocations.back().get();
}
template <typename... Args> static void trace(const char*, Args...) {}
#define STACK_TRACEF(...) trace(__VA_ARGS__)

namespace fs {
class File : public Print {
  struct Handle { std::string path, mode; size_t offset = 0; bool closed = false; };
  std::shared_ptr<Handle> handle;
public:
  File() = default;
  File(std::string path, std::string mode) : handle(std::make_shared<Handle>()) {
    handle->path = std::move(path); handle->mode = std::move(mode);
    if (handle->mode == "a") handle->offset = disk.at(handle->path).size();
    ++liveFiles;
  }
  explicit operator bool() const { return handle && !handle->closed; }
  bool isDirectory() const { return false; }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    check(bool(*this) && handle->mode != "r", "write uses open writable file");
    const bool append = handle->mode == "a";
    if (!append && !isTemp(handle->path)) {  // a G-39 restart of the log itself
      io("restart:" + std::to_string(count));
      disk[handle->path].append(reinterpret_cast<const char*>(bytes), count); handle->offset += count;
      return count;
    }
    const int call = append ? ++appendWrites : ++copyWrites;
    io(std::string(append ? "append:" : "copy:") + std::to_string(count));
    const size_t actual = call == (append ? failAppendWrite : failCopyWrite) ? std::min(shortCount, count) : count;
    // "Write accepted, flush lost": the handle takes every byte and reports
    // it, flush() says nothing, and the card file keeps at most cardKeepsBytes
    // (a card pulled under a buffered handle). Only the size tells.
    size_t landed = actual;
    if (append && cardKeepsBytes != SIZE_MAX && handle->path.rfind("/sd/", 0) == 0) {
      const size_t have = disk[handle->path].size();
      landed = cardKeepsBytes > have ? std::min(actual, cardKeepsBytes - have) : 0;
    }
    disk[handle->path].append(reinterpret_cast<const char*>(bytes), landed);
    handle->offset += actual;
    if (append ? appendError : copyError) setWriteError();
    return actual;
  }
  int read() {
    check(bool(*this) && handle->mode == "r", "scan uses open input");
    io("scan");
    if (++scanReads == failScanRead) return -1;
    if (handle->offset >= disk.at(handle->path).size()) return -1;
    return static_cast<unsigned char>(disk.at(handle->path)[handle->offset++]);
  }
  size_t readBytes(char* bytes, size_t count) {
    check(bool(*this) && handle->mode == "r", "copy read uses open input");
    io("read:" + std::to_string(count)); maxRead = std::max(maxRead, count);
    if (++copyReads == failCopyRead) return 0;
    const auto& contents = disk.at(handle->path);
    const size_t actual = std::min({count, readLimit, contents.size() - handle->offset});
    std::memcpy(bytes, contents.data() + handle->offset, actual); handle->offset += actual;
    return actual;
  }
  size_t read(uint8_t* bytes, size_t count) { return readBytes(reinterpret_cast<char*>(bytes), count); }
  int available() const { return static_cast<int>(disk.at(handle->path).size() - handle->offset); }
  size_t position() const { check(bool(*this), "position uses open file"); return handle->offset; }
  size_t size() const {
    check(bool(*this), "size uses open file"); io("size:" + handle->mode);
    return disk.at(handle->path).size() + (handle->mode == "w" ? tempSizeOffset : handle->mode == "r" ? inputSizeOffset : 0);
  }
  bool seek(size_t offset) {
    check(bool(*this) && handle->mode == "r", "seek uses open input"); io("seek:" + std::to_string(offset));
    if (failSeek || offset > disk.at(handle->path).size()) return false;
    handle->offset = offset; return true;
  }
  void flush() override { check(bool(*this), "flush uses open file"); io("flush:" + handle->mode); if (flushError) setWriteError(); }
  time_t getLastWrite() const { return 0; }
  void close() {
    check(bool(*this), "each handle closed once"); io("close:" + handle->mode); handle->closed = true; --liveFiles;
  }
};
}
using fs::File;

namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return text(path).rfind("/sd/", 0) == 0 ? SDCARD : INTERNAL; }
static bool isSdAvailable() { return sdAvailable; }
// The pre-G-39 resolver (still used by orphan recovery): latched and mounted
// means the mirror, anything else the primary path.
static void resolveOverflowPath(const char* path, size_t need, char* dest, size_t capacity) {
  io("resolve"); requestedSpace = need;
  std::snprintf(dest, capacity, "%s%s", overflow && sdAvailable ? "/sd" : "", path);
}
enum class LogTier { Primary, Overflow, Refused };
static LogTier resolveLogPath(const char* path, size_t need, char* dest, size_t capacity) {
  io("resolve"); requestedSpace = need;
  const bool toSd = overflow && sdWritable && heldLog != path;
  std::snprintf(dest, capacity, "%s%s", toSd ? "/sd" : "", path);
  if (toSd) return LogTier::Overflow;
  return overflow && flashBelowReserve ? LogTier::Refused : LogTier::Primary;
}
static void noteLogOverflowSdFailure(const char*) { io("sd_failed"); ++sdFailures; sdWritable = false; }
// B11: one log's card file failed. The real note asks the card's write probe:
// a card that fails it is reported as a whole (as above); with the card fine
// only this log leaves the card (cardProbeFine: the hold on `heldLog`).
static void noteLogCardFileFailure(const char* path, const char* hint) {
  if (!cardProbeFine) { noteLogOverflowSdFailure(hint); return; }
  io("card_file_failed"); ++cardFileFailures; heldLog = path;
}
static void noteLogReserveDrop() { ++reserveDrops; }
static File open(const String& path, const char* mode, bool create = false) {
  io(std::string("open:") + mode); const auto name = text(path);
  if (std::strcmp(mode, "r") == 0) {
    check(!create, "read opens do not create");
    if (failReadOpen || !disk.count(name)) return {};
  } else {
    check(create, "append/temp opens create");
    if ((std::strcmp(mode, "a") == 0 && (failAppendOpen || (failSdAppendOpen && name.rfind("/sd/", 0) == 0))) ||
        (std::strcmp(mode, "w") == 0 && failTempOpen && isTemp(name))) return {};
    if (std::strcmp(mode, "w") == 0) disk[name].clear(); else disk.try_emplace(name);
  }
  return File(name, mode);
}
static bool exists(const String& path) { io("exists:" + text(path)); return disk.count(text(path)); }
static bool remove(const String& path) {
  io("remove:" + text(path));
  if (text(path) == refuseRemove) return false;
  return disk.erase(text(path)) != 0;
}
static bool rename(const String& from, const String& to) {
  io("rename"); check(liveFiles == 0, "close handles before promotion");
  if (failRename || !disk.count(text(from)) ||
      (getStorageType(to) == SDCARD && disk.count(text(to)))) return false;
  disk[text(to)] = std::move(disk.at(text(from))); disk.erase(text(from)); return true;
}
static void noteLittleFsBytesWritten(size_t count) { io("note:" + std::to_string(count)); bytesNoted += count; }
}
// The production writer now runs as a system identity confined to the
// sys_logs tier it resolved; this harness exercises the write algorithm, so
// the guarded forwarders only record the scope and dispatch to the raw fake.
struct AuthContext { std::string tag; std::string scope; };
static std::vector<std::string> scopesSeen;
namespace VFS {
namespace Scopes {
constexpr const char* SYS_LOGS    = "/system/sys_logs";
constexpr const char* SYS_LOGS_SD = "/sd/system/sys_logs";
}
static const char* sysLogScopeFor(const String& path) { return text(path).rfind("/sd/", 0) == 0 ? Scopes::SYS_LOGS_SD : Scopes::SYS_LOGS; }
static AuthContext systemAuth(const char* scope, const char* tag) { scopesSeen.push_back(scope); return {tag, scope}; }
static File openGuarded(const String& path, const char* mode, const AuthContext&, bool create = false) { return open(path, mode, create); }
static bool existsGuarded(const String& path, const AuthContext&) { return exists(path); }
static bool removeGuarded(const String& path, const AuthContext&) { return remove(path); }
static bool renameGuarded(const String& from, const String& to, const AuthContext&) { return rename(from, to); }
}
struct SerialOutput {
  template <typename... Args> void printf(const char*, Args...) { events.push_back("serial"); }
} Serial;
// The wall clock of buildTimestampPrefix: a set prefix, or "" while unsynced.
static std::string clockPrefix;
static void getTimestampPrefixMsCached(char* out, size_t size) { std::snprintf(out, size, "%s", clockPrefix.c_str()); }
static unsigned long nowMs = 123456;
static unsigned long millis() { return nowMs; }

// S-8 and UP-03 platform pieces of the writer region.
#define EXT_RAM_BSS_ATTR
struct HostMutex { int unused; };
using SemaphoreHandle_t = HostMutex*;
[[maybe_unused]] static const unsigned portMAX_DELAY = 0xffffffffu;
static const unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
static HostMutex hostMutex;
static bool failMutexCreate = false;
static SemaphoreHandle_t xSemaphoreCreateMutexWithCaps(unsigned caps) {
  check(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "the batch mutex lives in PSRAM");
  return failMutexCreate ? nullptr : &hostMutex;
}
static void xSemaphoreTake(SemaphoreHandle_t mutex, unsigned) {
  check(mutex == &hostMutex && mutexDepth == 0, "the batch mutex is not taken twice");
  ++mutexDepth; ++mutexTakes;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {
  check(mutex == &hostMutex && mutexDepth == 1, "the batch mutex is given once");
  --mutexDepth;
}
struct esp_partition_t { size_t size; };
enum { ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_ANY = 0xff };
[[maybe_unused]] static const char kLittleFsPartitionLabel[] = "littlefs";
static esp_partition_t hostPartition{0};
static bool partitionKnown = false;
static const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label) {
  check(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY &&
        std::strcmp(label, "littlefs") == 0, "the cap total is the littlefs partition's size");
  return partitionKnown ? &hostPartition : nullptr;
}
[[maybe_unused]] static bool filesystemReady = true;
// A build that serves file catch-up (ENABLE_ESPNOW_SEND) keeps the copy
// rotation for the three followed logs; the cases switch it.
[[maybe_unused]] static bool kCatchupServesLogs = true;
void cleanupLogOrphan(const char* path);

#include "System_LogCapsCore.h"  // the production header, copied beside this unit

#ifndef HAVE_CATCHUP_NOTES
#define HAVE_CATCHUP_NOTES 0
#endif
// File catch-up hooks (System_CatchupLedger.h): recorded only. The production
// hooks do no I/O, logging or allocation (test_catchup_ledger.py checks that).
static std::vector<std::string> catchupNotes;
[[maybe_unused]] static void catchupNoteTrim(const char* dest, size_t tailStart) {
  check(lockDepth == 1, "the trim note is taken inside the writer's lock");
  catchupNotes.push_back("trim:" + std::string(dest) + ":" + std::to_string(tailStart));
  events.push_back("catchup:trim");
}
[[maybe_unused]] static void catchupNoteRestart(const char* dest) {
  check(lockDepth == 1, "the restart note is taken inside the writer's lock");
  catchupNotes.push_back("restart:" + std::string(dest));
  events.push_back("catchup:restart");
}

// ---------------------------------------------------------------------------
// B11: log files on the card are sealed. The production System_SealedLog.cpp
// is compiled below (it opens, marks, moves aside and seals through the fake
// VFS above); the device key and the cipher are doubles. The toy seal writes
// "ENC1:" and the text in base64, so a row never shows its text or a line
// end, and a 1023-byte row fits the production row buffer as the real one
// does.
//
// `cardSealing` is this harness's one switch on production: the cases written
// before B11 run with it off and keep exercising the append, both rotations
// and every failure branch with plain bytes on either tier; the B11 cases
// (sealedCardCases) run with it on, which is what the device does.
// ---------------------------------------------------------------------------
static bool cardSealing = false;
static uint32_t mountGeneration = 1;
static int movedAsideEvents = 0;
// INSERT_CAPCRYPT_MACROS
// The device key and the cipher: the shared toy ones (a row is as long as a
// real one and opens only for the class of the path it was sealed for).
#define SEALED_LOG_HOST_CRYPTO_ONLY
#define SEALED_LOG_HOST_MARK_API
#define SEALED_LOG_HOST_CRYPTO_API
#include "sealed_log_host.h"  // copied beside this unit, with System_SealedLogCore.h
static bool& keyAvailable = sealed_log_host::keyAvailable;
static std::string& ownKeyId = sealed_log_host::keyId;
static int& sealCalls = sealed_log_host::sealCalls;
static int& failSealAt = sealed_log_host::sealFailAt;
static void logSystemEvent(const char*, const char*, ...) { ++movedAsideEvents; events.push_back("event:moved_aside"); }
namespace VFS {
static uint32_t sdMountGeneration() { return mountGeneration; }
}
// What a reader of the card file sees: the mark line dropped, every row opened.
static std::string revealed(const std::string& stored) {
  std::string out;
  size_t at = 0;
  bool first = true;
  while (at < stored.size()) {
    size_t end = stored.find('\n', at);
    if (end == std::string::npos) end = stored.size();
    const std::string line = stored.substr(at, end - at);
    at = end + 1;
    if (first && line.rfind("#HW1ENC", 0) == 0) { first = false; continue; }
    first = false;
    std::string plain;
    out += sealed_log_host::open(nullptr, line.data(), line.size(), plain) ? plain : "[undecryptable row]";
    out += '\n';
  }
  return out;
}
static std::string markLine() { return "#HW1ENC v1 k1 id=" + ownKeyId + "\n"; }

// INSERT_SEALED_LOG

// INSERT_PRODUCTION

// INSERT_LOG_LINE

// The log the single-line cases write. The cases from before B11 keep their
// short name; the sealed cases use one under the system log folder, where the
// card's log files live (a file that is not this device's is moved aside
// only in the log trees, and the rows are sealed for that folder's class).
static std::string logName = "/logs/events.log";
static std::string destination() { return (overflow ? "/sd" : "") + logName; }
static bool seen(const std::string& event) { return std::find(events.begin(), events.end(), event) != events.end(); }
static size_t eventIndex(const std::string& event) {
  auto item = std::find(events.begin(), events.end(), event); check(item != events.end(), event.c_str()); return static_cast<size_t>(item - events.begin());
}
static void reset(bool onSd = false) {
  check(lockDepth == 0 && liveFiles == 0, "previous case releases resources");
  ++cases; disk.clear(); events.clear(); recovering = false; overflow = onSd; sdAvailable = sdWritable = onSd;
  flashBelowReserve = true; failSdAppendOpen = false; sdFailures = reserveDrops = 0;
  cardKeepsBytes = SIZE_MAX; cardProbeFine = false; cardFileFailures = 0; heldLog.clear();
  logName = "/logs/events.log";
  failAlloc = failSeek = failRename = appendError = copyError = flushError = false;
  failAppendOpen = failReadOpen = failTempOpen = false;
  failAppendWrite = appendWrites = failCopyWrite = copyWrites = failCopyRead = copyReads = failScanRead = scanReads = 0;
  shortCount = bytesNoted = requestedSpace = maxRead = tempSizeOffset = inputSizeOffset = 0;
  readLimit = 1024; refuseRemove.clear(); catchupNotes.clear();
}
static std::string rows(int begin, int end) {
  std::string out;
  for (int i = begin; i < end; ++i) { char line[16]; std::snprintf(line, sizeof(line), "old-%02d\r\n", i); out += line; }
  return out;
}
static bool append(const char* line = "new", size_t cap = 100) {
  const bool result = appendLineWithCap(logName.c_str(), String(line), cap);
  check(lockDepth == 0 && liveFiles == 0, "append releases lock and handles");
  check(requestedSpace == std::strlen(line) + 512, "overflow admission retains existing margin");
  return result;
}
static void recover() {
  recovering = true; cleanupLogOrphan(logName.c_str()); recovering = false;
  check(lockDepth == 0 && liveFiles == 0, "recovery releases resources");
  check(requestedSpace == 0, "recovery resolves without reserving write space");
}
static void normalResult(const char* name, bool result) {
  std::cout << (overflow ? "sd:" : "flash:") << name << ':' << result << ':' << bytesNoted << '\n';
  for (const auto& file : disk) std::cout << std::quoted(file.first) << '=' << std::quoted(file.second) << '\n';
}
static void untouchedRotation(const std::string& original) {
  check(disk.count(destination()) && disk.at(destination()) == original + "new\r\n", "rotation failure preserves complete original including append");
  check(!disk.count(destination() + ".tmp"), "failed rotation temp removed");
  check(!seen("remove:" + destination()), "never delete authoritative original before promotion");
  check(!seen("rename"), "incomplete copy never promoted");
}
// G-39: a rotation whose copy could not be written restarts the log: one drop
// marker, then the line that was being appended.
static std::string restartMarker(size_t dropped) {
  return "[LOG] rotation blocked (low space): " + std::to_string(dropped) + " older bytes dropped\r\n";
}
static void restartedRotation(const std::string& original, const char* line = "new", size_t cap = 100) {
  const std::string expected = restartMarker(original.size()) + line + "\r\n";
  check(disk.count(destination()) && disk.at(destination()) == expected, "failed copy restarts the log with a drop marker and the new line");
  check(disk.at(destination()).size() <= cap + std::strlen(line) + 2, "restarted log within cap plus one line");
  check(!disk.count(destination() + ".tmp"), "failed rotation temp removed");
  check(!seen("rename") && seen("serial"), "restart is reported and never promotes the incomplete copy");
  if (HAVE_CATCHUP_NOTES)
    check(catchupNotes == std::vector<std::string>{"restart:" + destination()}, "a restart leaves one restart note for its path");
}
static void normalCases(bool onSd) {
  reset(onSd); check(append("hello", 20), "ordinary append succeeds");
  check(catchupNotes.empty(), "an append without rotation leaves no note");
  check(disk.at(destination()) == "hello\r\n" && bytesNoted == 7, "real Print appends CRLF and accounts seven bytes");
  check(!seen("flush:a") && !seen("open:r") && !seen("size:a"), "append fast path avoids extra flush/read/stat");
  normalResult("append", true);
  reset(onSd); disk[destination()] = "older\r\n"; check(append("new", 12), "exact cap does not rotate");
  check(disk.at(destination()) == "older\r\nnew\r\n" && !seen("open:r"), "cap equality retains bytes"); normalResult("exact-cap", true);
  reset(onSd); check(append("", 2), "empty line succeeds"); check(disk.at(destination()) == "\r\n", "empty line keeps CRLF"); normalResult("empty-line", true);
  reset(onSd); disk[destination()] = rows(0, 20); disk[destination() + ".tmp"] = "stale";
  check(append(), "rotation succeeds"); check(disk.at(destination()) == rows(11, 20) + "new\r\n", "85 percent target advances to next whole line");
  if (HAVE_CATCHUP_NOTES) {
    check(catchupNotes == std::vector<std::string>{"trim:" + destination() + ":88"}, "one trim note with the dropped byte count");
    check(eventIndex("rename") < eventIndex("catchup:trim"), "the trim note follows the successful promotion");
  }
  check(!disk.count(destination() + ".tmp") && maxRead <= 1024, "rotation promotes temp with bounded buffer");
  if (!BASELINE) { check(seen("flush:w"), "rotation flush checked before promotion"); check(eventIndex("flush:w") < eventIndex("rename"), "flush precedes promotion"); }
  normalResult("rotate", true);
  reset(onSd); disk[destination()] = std::string(10000, 'x'); check(append("new", 9000), "bounded newline scan succeeds");
  check(scanReads == 4096 && disk.at(destination()) == std::string(3549, 'x') + "new\r\n", "4 KB scan limit retains partial leading line"); normalResult("scan-limit", true);
  reset(onSd); disk[destination()] = "old"; check(append("new", 0), "zero cap retains EOF-scan behavior");
  check(disk.at(destination()) == "oldnew\r\n" && !seen("open:w"), "nothing after EOF scan leaves original"); normalResult("zero-cap", true);
  reset(onSd); check(append("longline", 9), "complete-line trimming can produce empty tail");
  check(disk.at(destination()).empty() && seen("open:w") && !disk.count(destination() + ".tmp"), "empty verified tail is promoted"); normalResult("empty-tail", true);
  reset(onSd); disk[destination()] = std::string(5000, 'x') + "\n" + std::string(3000, 'y') + "\n";
  readLimit = 113; check(append("new", 5000), "short positive reads continue");
  check(disk.at(destination()) == std::string(3000, 'y') + "\nnew\r\n" && copyReads > 20, "multi-chunk tail exact despite fragmented reads"); normalResult("fragmented-read", true);
  reset(onSd); disk[destination()] = "authoritative"; disk[destination() + ".tmp"] = "partial"; recover();
  check(disk.at(destination()) == "authoritative" && !disk.count(destination() + ".tmp"), "recovery discards stale temp beside original"); normalResult("stale-temp", true);
  reset(onSd); disk[destination() + ".tmp"] = "recoverable"; recover(); check(disk.at(destination()) == "recoverable", "legacy orphan promoted"); normalResult("orphan", true);
  reset(onSd); disk[destination()] = "normal"; recover(); check(disk.at(destination()) == "normal" && !seen("rename"), "recovery leaves normal log"); normalResult("no-orphan", true);
  reset(onSd); recover(); check(disk.empty(), "recovery with neither file is a no-op"); normalResult("no-files", true);
  reset(onSd); disk[destination() + ".tmp"] = "orphan"; failRename = true; recover();
  check(disk.empty() && seen("serial"), "existing failed recovery policy discards orphan and reports"); normalResult("recovery-failed", false);
  reset(onSd); disk[destination()] = "authoritative"; disk[destination() + ".tmp"] = "stale"; refuseRemove = destination() + ".tmp"; recover();
  check(disk.at(destination()) == "authoritative" && disk.at(destination() + ".tmp") == "stale", "failed stale cleanup preserves both files"); normalResult("stale-cleanup-failed", false);
  // Line lengths around the log line limits (DEBUG_MSG_SIZE, a 47-character
  // prefix plus the longest text), with and without a rotation.
  for (size_t length : {size_t(0), size_t(1), size_t(255), size_t(256), size_t(257), size_t(47 + DEBUG_MSG_SIZE - 1)}) {
    std::string line(length, 'L');
    for (size_t i = 0; i < length; i += 7) line[i] = char('a' + i % 26);
    for (bool rotate : {false, true}) {
      reset(onSd);
      disk[destination()] = rotate ? rows(0, 60) : rows(0, 2);
      check(append(line.c_str(), rotate ? 400 : 100000), "line of each length appends");
      if (!rotate) check(disk.at(destination()) == rows(0, 2) + line + "\r\n", "the line and CRLF follow the old lines");
      normalResult(("length-" + std::to_string(length) + (rotate ? "-rotate" : "")).c_str(), true);
    }
  }
}
static void failureCases(bool onSd) {
  const auto original = rows(0, 20);
  for (int failedWrite : {1, 2}) {
    for (size_t partial : {size_t(0), size_t(1)}) {
      reset(onSd); failAppendWrite = failedWrite; shortCount = partial;
      const bool ok = append("body", 100);
      check(ok == bool(BASELINE), "short body/CRLF append rejects false success");
      const size_t actual = (failedWrite == 1 ? partial + 2 : 4 + partial);
      check(bytesNoted == (BASELINE ? 6 : actual), "failed append counts only accepted bytes");
      check(!seen("open:r"), "failed append does not rotate");
    }
  }
  reset(onSd); disk[destination()] = original; failCopyWrite = 1; shortCount = 7;
  if (BASELINE) {
    check(append(), "short tail write rejects false success");
    check(disk.at(destination()) == (rows(11, 20) + "new\r\n").substr(0, 7), "baseline reproduces incomplete-tail promotion");
  } else {
    check(append(), "short tail write restarts the log and keeps the line"); restartedRotation(original);
  }
  reset(onSd); disk[destination()] = original; failRename = true;
  check(!append(), "rename failure returns false");
  if (BASELINE || onSd) check(!disk.count(destination()) && disk.at(destination() + ".tmp") == rows(11, 20) + "new\r\n", "failed SD/legacy promotion leaves verified recovery temp");
  else check(disk.at(destination()) == original + "new\r\n" && !disk.count(destination() + ".tmp") && !seen("remove:" + destination()), "LittleFS promotion failure preserves original and removes temp");
  if (BASELINE) return;

  reset(onSd); disk[destination()] = original; failAppendWrite = 2; shortCount = 1;
  check(!append(), "incomplete append above cap rejected");
  check(disk.at(destination()) == original + "new\r" && !seen("open:r") && bytesNoted == 4, "failed append leaves original prefix and skips rotation even above cap");
  reset(onSd); failAppendOpen = true; check(!append(), "append open failure reported"); check(bytesNoted == 0 && disk.empty(), "failed append open does not account or create");
  reset(onSd); appendError = true; check(!append(), "append Print error rejects full count"); check(bytesNoted == 5 && !seen("open:r"), "accepted bytes counted despite reported error");
  reset(onSd); disk[destination()] = original; failReadOpen = true; check(!append(), "rotation input open failure reported"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; inputSizeOffset = 1; check(!append(), "reopened input size mismatch reported"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; failScanRead = 2; check(!append(), "premature newline scan read reported"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; failSeek = true; check(!append(), "rotation seek failure reported"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; failTempOpen = true; check(append(), "rotation temp open failure restarts the log"); restartedRotation(original);
  reset(onSd); disk[destination()] = original; copyError = true; check(append(), "temp Print error restarts the log"); restartedRotation(original);
  reset(onSd); disk[destination()] = original; flushError = true; check(append(), "temp flush error restarts the log"); restartedRotation(original);
  reset(onSd); disk[destination()] = original; tempSizeOffset = 1; check(append(), "temp size mismatch restarts the log"); restartedRotation(original);
  reset(onSd); disk[destination()] = original; failCopyRead = 1; check(!append(), "premature copy EOF rejects promotion"); untouchedRotation(original);
  reset(onSd); disk[destination()] = std::string(5000, 'x') + "\n" + std::string(3000, 'y') + "\n";
  const auto longOriginal = disk.at(destination()); failCopyRead = 2; check(!append("new", 5000), "premature read after full chunk rejects promotion"); untouchedRotation(longOriginal);
  reset(onSd); disk[destination()] = longOriginal; failCopyWrite = 2; shortCount = 17; check(append("new", 5000), "later short write restarts the log"); restartedRotation(longOriginal, "new", 5000);
  if (onSd) {
    reset(true); disk[destination()] = original; refuseRemove = destination();
    check(!append(), "SD failed original removal reported"); check(disk.at(destination()) == original + "new\r\n" && !disk.count(destination() + ".tmp") && !seen("rename"), "SD failed removal retains original and discards checked temp");
  }
}
// Boot recovery also checks the SD mirror of each capped log: a rotation that
// ran while overflow was latched in an earlier boot leaves its orphan there,
// where the current (unlatched) resolution never looks.
static void mirrorRecovery() {
  const std::string mirror = "/sd/logs/events.log";
  reset(false); sdAvailable = true; disk[mirror + ".tmp"] = "mirror orphan"; recover();
  check(disk.count(mirror) && disk.at(mirror) == "mirror orphan" && !disk.count(mirror + ".tmp") && !disk.count(destination()),
        "mirror orphan promoted while the overflow latch is off");
  reset(false); sdAvailable = true; disk[mirror] = "mirror"; disk[mirror + ".tmp"] = "stale"; disk[destination()] = "flash"; recover();
  check(disk.at(mirror) == "mirror" && !disk.count(mirror + ".tmp") && disk.at(destination()) == "flash",
        "stale mirror temp removed while both logs stay");
  reset(true); sdAvailable = true; disk[mirror + ".tmp"] = "once"; recover();
  check(disk.count(mirror) && disk.at(mirror) == "once" && std::count(events.begin(), events.end(), "rename") == 1,
        "latched overflow checks the mirror exactly once");
  reset(false); sdAvailable = false; disk[mirror + ".tmp"] = "unreachable"; recover();
  check(disk.count(mirror + ".tmp") && !disk.count(mirror), "mirror left alone without a mounted card");
}
// G-39: a flash too full for the rotation copy must not let the log outgrow
// its cap, and must not repeat the failing copy on every line.
static void lowSpaceHoldsCap(bool onSd) {
  reset(onSd); disk[destination()] = rows(0, 20); copyError = true;
  int rotations = 0;
  for (int i = 0; i < 60; ++i) {
    events.clear();
    check(append(), "append under persistent copy failure keeps the line");
    check(disk.at(destination()).size() <= 100 + 5, "log never exceeds cap plus one line");
    if (seen("open:r")) ++rotations;
    check(disk.at(destination()).size() >= 5 && disk.at(destination()).compare(disk.at(destination()).size() - 5, 5, "new\r\n") == 0,
          "newest line always kept");
  }
  check(rotations <= 12, "failing copy attempted once per cap's worth of lines, not per line");
  check(!disk.count(destination() + ".tmp"), "no temp left behind");
}
// G-39: the overflow latch and the reserve. A latched resolution with no
// writable card is refused and counted; a failed SD append marks the card
// unwritable and retries once where the latch resolves next.
static void reserveAndSdLatch() {
  reset(true); sdAvailable = sdWritable = false;
  check(!append(), "latched with no card refuses the line");
  check(disk.empty() && !seen("open:a") && reserveDrops == 1, "refused line touches no tier and is counted");
  reset(true); failSdAppendOpen = true; flashBelowReserve = false;
  check(append(), "SD append failure retries on flash once space returned");
  check(sdFailures == 1 && !sdWritable && disk.count("/logs/events.log") && disk.at("/logs/events.log") == "new\r\n" &&
        !disk.count("/sd/logs/events.log"), "line lands on flash and the card is reported unwritable");
  check(std::count(events.begin(), events.end(), "open:a") == 2, "exactly one retry");
  reset(true); failSdAppendOpen = true;
  check(!append(), "SD append failure below the reserve is a counted drop");
  check(sdFailures == 1 && reserveDrops == 1 && !disk.count("/logs/events.log"), "flash reserve kept after the card failed");
  reset(true); sdWritable = false; flashBelowReserve = false; failSdAppendOpen = true;
  check(append() && disk.at("/logs/events.log") == "new\r\n" && sdFailures == 0, "latched with no card above the floor writes to flash without touching the card");
  reset(false); failAppendOpen = true;
  check(!append() && sdFailures == 0 && reserveDrops == 0, "a flash append failure never reports the card");
}
#if HAVE_SEALED_LOG
// ---------------------------------------------------------------------------
// B11: the capped logs on the card, sealed (production System_SealedLog under
// the writer; `cardSealing` on, as on the device).
// ---------------------------------------------------------------------------
static const std::string kCard = "/sd/system/sys_logs/t/events.log";   // copy rotation (followed by catch-up)
static std::string sealedRow(const std::string& text) {  // sealed for the system logs' class
  return sealed_log_host::sealRow(text, kCard.c_str());
}
static std::string sealedRows(int begin, int end) {
  std::string out;
  for (int i = begin; i < end; ++i) { char line[16]; std::snprintf(line, sizeof(line), "old-%02d", i); out += sealedRow(line); }
  return out;
}
static void sealReset() {
  reset(true);
  logName = "/system/sys_logs/t/events.log";
  cardSealing = true; keyAvailable = true; flashBelowReserve = false;
  sealCalls = failSealAt = movedAsideEvents = 0;
  ++mountGeneration;  // every case is a new mount: no head is remembered from the case before
}
// Every line of a stored card file is the mark (first) or a whole sealed row.
static void wholeRows(const std::string& stored) {
  check(stored.rfind(markLine(), 0) == 0, "the card file starts with the mark and this device's key id");
  check(stored.find("#HW1ENC", 1) == std::string::npos, "the mark appears once, at the head");
  check(!stored.empty() && stored.back() == '\n', "the card file ends at a row boundary");
  size_t at = markLine().size();
  while (at < stored.size()) {
    const size_t end = stored.find('\n', at);
    check(end != std::string::npos && stored.compare(at, 5, "ENC1:") == 0 && (end - at - 5) % 4 == 0,
          "every line after the mark is one whole sealed row");
    at = end + 1;
  }
  check(revealed(stored).find("[undecryptable row]") == std::string::npos, "every row opens with this device's key");
}
static void sealedCardCases() {
  // A first line: the mark, then the line as one sealed row. Nothing of the
  // text is on the card in the clear, and flash is not touched.
  sealReset();
  check(append("hello card", 200), "a line is appended on the card");
  check(disk.count(kCard) && disk.at(kCard) == markLine() + sealedRow("hello card"), "mark first, then one sealed row");
  check(revealed(disk.at(kCard)) == "hello card\n" && disk.at(kCard).find("hello") == std::string::npos,
        "the row opens to the line and does not show it");
  check(!disk.count("/system/sys_logs/t/events.log") && bytesNoted == 0, "nothing is written or counted on flash");
  check(std::count(events.begin(), events.end(), "open:a") == 1 && !seen("open:r"),
        "a new file needs one append open and no read");
  // Further lines: rows are appended, the mark is not repeated, and the head
  // of a file found ours is read once per mount, not once per line.
  check(append("second", 200), "a second line");
  events.clear();
  check(append("third", 200), "a third line");
  check(disk.at(kCard) == markLine() + sealedRow("hello card") + sealedRow("second") + sealedRow("third"),
        "rows follow in order under the one mark");
  check(!seen("open:r"), "no head read for a file this mount already found ours");
  wholeRows(disk.at(kCard));
  // An existing file of ours from before this mount: its head is read, then rows are appended.
  sealReset(); disk[kCard] = markLine() + sealedRows(0, 2);
  check(append("new", 200) && disk.at(kCard) == markLine() + sealedRows(0, 2) + sealedRow("new"),
        "this device's file takes the new row");
  check(std::count(events.begin(), events.end(), "open:r") == 2 && movedAsideEvents == 0,
        "its mark is judged and its last byte looked at, once");
  events.clear();
  check(append("next", 200) && !seen("open:r"), "a settled file is not read again within the mount");
  // An empty file gets the mark.
  sealReset(); disk[kCard] = "";
  check(append("new", 200) && disk.at(kCard) == markLine() + sealedRow("new"), "an empty card file starts with the mark");
  // An empty line is one (empty) row.
  sealReset();
  check(append("", 200) && disk.at(kCard) == markLine() + sealedRow("") && revealed(disk.at(kCard)) == "\n",
        "an empty line is one row");

  // Q9: a card file that is not this device's sealed file is moved aside, byte
  // for byte, and a new sealed file is started. Nothing in it is read beyond
  // its first line, converted, or appended to.
  const std::string otherDevice = "#HW1ENC v1 k1 id=fedcba9876543210\n" + sealedRows(0, 3);
  const std::string plainFile = "[2026-11-22 10:00:00] | a plain log line\r\n";
  const std::string oldMark = "#HW1ENC v1 k1\n" + sealedRows(0, 1);   // sealed before the mark named its key
  const std::string tornMark = "#HW1ENC v1 k1 id=0123";                // a pull during the first write
  for (const std::string* foreign : {&otherDevice, &plainFile, &oldMark, &tornMark}) {
    sealReset(); disk[kCard] = *foreign;
    check(append("new", 200), "the line is written although the name was taken");
    check(disk.count(kCard + ".foreign-1") && disk.at(kCard + ".foreign-1") == *foreign,
          "the other file is moved aside unchanged");
    check(disk.at(kCard) == markLine() + sealedRow("new"), "a new sealed file takes the name");
    check(movedAsideEvents == 1 && !disk.count("/system/sys_logs/t/events.log"), "one event line; flash is not involved");
  }
  // The first free name is taken: -1 and -2 exist, so -3.
  sealReset(); disk[kCard] = plainFile; disk[kCard + ".foreign-1"] = "a"; disk[kCard + ".foreign-2"] = "b";
  check(append("new", 200) && disk.at(kCard + ".foreign-3") == plainFile && disk.at(kCard + ".foreign-1") == "a",
        "an earlier set-aside file is never replaced");
  // It cannot be moved (the rename fails): the card does not take the line,
  // the plain file is neither appended to nor removed, and the line goes to flash.
  sealReset(); disk[kCard] = plainFile; failRename = true;
  check(append("new", 200), "the line is kept");
  check(disk.at(kCard) == plainFile && sdFailures == 1 && disk.at("/system/sys_logs/t/events.log") == "new\r\n",
        "an unmovable foreign file stays as it is and the line lands on flash");

  // No device key: nothing is created on the card; the line lands on flash, plain.
  sealReset(); keyAvailable = false;
  check(append("new", 200), "the line is kept without the key");
  check(!disk.count(kCard) && disk.at("/system/sys_logs/t/events.log") == "new\r\n" && sdFailures == 1,
        "no key: no card file, the line on flash");
  // The key fails between the open and a row (it cannot on the device; the
  // seal call is what is checked): no plain row follows on the card.
  sealReset(); disk[kCard] = markLine() + sealedRows(0, 2); failSealAt = 1;
  check(append("secret line", 200), "the line is kept");
  check(disk.at(kCard) == markLine() + sealedRows(0, 2) && disk.at("/system/sys_logs/t/events.log") == "secret line\r\n",
        "a failed seal writes nothing to the card and the line goes to flash");

  // The card is pulled mid-write: a short write of a row. The card is
  // reported, the line is written again on flash, and the card holds at most
  // one torn row, never plain text.
  for (size_t partial : {size_t(0), size_t(7)}) {
    sealReset(); disk[kCard] = markLine() + sealedRows(0, 2); failAppendWrite = 1; shortCount = partial;
    check(append("pulled", 200), "the line is kept when the card fails");
    check(sdFailures == 1 && disk.at("/system/sys_logs/t/events.log") == "pulled\r\n", "the card is reported and flash takes the line");
    const std::string card = disk.at(kCard);
    check(card.size() == (markLine() + sealedRows(0, 2)).size() + partial &&
          card.find("pulled") == std::string::npos, "at most one torn row, and no plain byte, on the card");
    check(revealed(card).rfind("old-00\nold-01\n", 0) == 0, "the rows before the tear still open");
  }

  // A line over 1023 bytes is split into rows of 1023 or less, never dropped.
  {
    sealReset();
    std::string longLine(2500, 'x');
    for (size_t i = 0; i < longLine.size(); i += 97) longLine[i] = char('a' + i % 26);
    check(appendLineWithCap("/system/sys_logs/t/events.log", longLine.c_str(), longLine.size(), 100000), "a long line is appended");
    const std::string card = disk.at(kCard);
    wholeRows(card);
    check(card == markLine() + sealedRow(longLine.substr(0, 1023)) + sealedRow(longLine.substr(1023, 1023)) +
                      sealedRow(longLine.substr(2046)), "three rows: 1023 + 1023 + 454 bytes");
    std::string joined = revealed(card);
    joined.erase(std::remove(joined.begin(), joined.end(), '\n'), joined.end());
    check(joined == longLine, "nothing of the line is lost");
  }

  // The cap on the card is twice the caller's, in stored bytes: a file that
  // would be over a 100-byte cap on flash is within its 200 bytes here.
  sealReset(); disk[kCard] = markLine() + sealedRows(0, 5);          // 34 + 5 * 54 = 304 stored bytes
  check(append("new", 200), "append within the card cap");
  check(disk.at(kCard).size() == 354 && disk.at(kCard) == markLine() + sealedRows(0, 5) + sealedRow("new") &&
        !seen("open:w") && catchupNotes.empty(), "354 stored bytes do not rotate under the 400-byte card cap");

  // Copy rotation on the card (events.log is followed by catch-up): the copy
  // starts with the mark, keeps whole rows from a row boundary, and ends with
  // the new row. The trim note is the row bytes that went (the mark line
  // stays where it was, at the head).
  {
    sealReset(); disk[kCard] = markLine() + sealedRows(0, 20); disk[kCard + ".tmp"] = "stale";
    check(append("new", 100), "rotation on the card succeeds");
    const std::string before = markLine() + sealedRows(0, 20) + sealedRow("new");   // 34 + 20 * 54 + 50 = 1164 > 200
    const std::string after = disk.at(kCard);
    wholeRows(after);
    check(after.size() <= 170 + markLine().size() && after.size() < before.size(), "the copy holds about 85 percent of the card cap");
    const std::string kept = after.substr(markLine().size());
    check(before.size() > kept.size() && before.compare(before.size() - kept.size(), kept.size(), kept) == 0,
          "the kept rows are the newest ones, unchanged");
    check(revealed(after).size() >= 4 && revealed(after).compare(revealed(after).size() - 4, 4, "new\n") == 0,
          "the new line is the last row");
    check(!disk.count(kCard + ".tmp") && seen("flush:w") && eventIndex("flush:w") < eventIndex("rename"),
          "the copy is verified, then promoted");
    const size_t dropped = before.size() - after.size();
    check(catchupNotes == std::vector<std::string>{"trim:" + kCard + ":" + std::to_string(dropped)},
          "one trim note: the row bytes that went, the mark line not counted");
    // The next line goes on under the same mark.
    check(append("after", 100), "append after the rotation");
    wholeRows(disk.at(kCard));
  }
  // The copy cannot be written (the card is full): the log is restarted as a
  // sealed file, with the drop marker and the new line as rows.
  {
    sealReset(); disk[kCard] = markLine() + sealedRows(0, 20); copyError = true;
    check(append("new", 100), "the line is kept when the copy fails");
    const std::string after = disk.at(kCard);
    wholeRows(after);
    const std::string shown = revealed(after);
    check(shown.rfind("[LOG] rotation blocked (low space): ", 0) == 0 && shown.find("older bytes dropped\nnew\n") != std::string::npos,
          "a sealed drop marker, then the new line");
    check(!disk.count(kCard + ".tmp") && catchupNotes == std::vector<std::string>{"restart:" + kCard},
          "no temp left, one restart note");
  }
  // ---- Review of 2026-11-22 ----
  // "Write accepted, flush lost": the card is pulled under the handle. Every
  // write reports all its bytes and the flush says nothing; the file never
  // grows. The size after the flush tells: the card is reported and the line
  // is written on flash. Before the fix the line was silently gone.
  {
    sealReset(); disk[kCard] = markLine() + sealedRows(0, 2);
    const std::string before = disk.at(kCard);
    cardKeepsBytes = before.size();                       // nothing more arrives
    check(append("pulled under the handle", 200), "the line is kept when the flush is lost");
    check(disk.at(kCard) == before && disk.at(logName) == "pulled under the handle\r\n" && sdFailures == 1,
          "the lost row is noticed by the file's size and the line lands on flash");
    check(seen("flush:a") && seen("size:a") && eventIndex("flush:a") < eventIndex("sd_failed"),
          "the append is flushed and its size read before it counts");
    // Part of the row arrived (a torn row): the same, and nothing plain is on the card.
    sealReset(); disk[kCard] = before; cardKeepsBytes = before.size() + 9;
    check(append("torn", 200) && disk.at(logName) == "torn\r\n" && disk.at(kCard).size() == before.size() + 9 &&
          disk.at(kCard).find("torn") == std::string::npos, "a torn row is a failed append");
    // The card comes back: the torn row is ended first, so the next row is a
    // row of its own and not lost with it.
    cardKeepsBytes = SIZE_MAX; sdWritable = true; ++mountGeneration;
    check(append("after", 200), "the next line goes to the card again");
    check(disk.at(kCard) == before + sealedRow("torn").substr(0, 9) + "\n" + sealedRow("after"),
          "a line end is written after the torn row before the next row");
    check(revealed(disk.at(kCard)) == "old-00\nold-01\n[undecryptable row]\nafter\n", "only the torn row is lost");
    check(disk.at(logName) == "torn\r\n", "and that line is on flash, once");
  }
  // A card file found torn at the next mount (a power cut mid-row).
  sealReset(); disk[kCard] = markLine() + sealedRows(0, 2) + "ENC1:dG9yb";
  check(append("next", 200) && disk.at(kCard) == markLine() + sealedRows(0, 2) + "ENC1:dG9yb\n" + sealedRow("next"),
        "a torn tail is ended before the next row");
  // The tail cannot be mended (the card takes no byte): the card file is
  // given up for this line and nothing is appended to it.
  sealReset(); disk[kCard] = markLine() + sealedRows(0, 2) + "ENC1:dG9yb"; failAppendWrite = 1; shortCount = 0;
  check(append("next", 200) && disk.at(kCard) == markLine() + sealedRows(0, 2) + "ENC1:dG9yb" &&
        disk.at(logName) == "next\r\n" && sdFailures == 1, "an unmendable tail sends the line to flash");

  // One bad file, a good card (the write probe passes): only this log leaves
  // the card; the card is not reported as failed.
  sealReset(); cardProbeFine = true; disk[kCard] = plainFile; failRename = true;
  check(append("kept", 200) && disk.at(kCard) == plainFile && disk.at(logName) == "kept\r\n",
        "the line lands on flash");
  check(sdFailures == 0 && sdWritable && cardFileFailures == 1 && heldLog == logName && seen("card_file_failed"),
        "the failure is this log's file, not the card");
  events.clear();
  check(append("kept two", 200) && disk.at(logName) == "kept\r\nkept two\r\n" && !seen("open:r") && cardFileFailures == 1,
        "while the file is held the log writes to flash without touching the card");

  // A file that is not this device's is never judged without the key: it is
  // not moved aside (before the fix a key that did not load made every card
  // file look foreign).
  sealReset(); keyAvailable = false; disk[kCard] = otherDevice;
  check(append("no key", 200) && disk.at(kCard) == otherDevice && !disk.count(kCard + ".foreign-1") &&
        movedAsideEvents == 0 && disk.at(logName) == "no key\r\n", "without the key nothing on the card is moved");

  // Recovery of a rotation cut short on the card promotes the staged copy,
  // which is a complete sealed file (the mark at its head).
  sealReset(); disk[kCard + ".tmp"] = markLine() + sealedRows(3, 6); recover();
  check(disk.count(kCard) && !disk.count(kCard + ".tmp"), "the staged copy is promoted");
  wholeRows(disk.at(kCard));
  cardSealing = false;
}
#endif
#if HAVE_TIMESTAMPED_LINE
// log_out's capped-log line: production appendTimestampedLine against the
// statements it replaced (String line = buildTimestampPrefix(); line += text;
// appendLineWithCap), byte for byte and file call for file call, for each
// prefix form (wall clock, "[ms=...]" before the clock is set, the longest
// prefix getTimestampPrefixMsCached can write), both tiers, with and without a
// rotation, text lengths up to past the kept block, with PSRAM refused first.
static std::string snapshot() {
  std::string out;
  for (const auto& file : disk) out += file.first + "=" + file.second + "\n";
  for (const auto& event : events) out += event + ",";
  return out;
}
static void timestampedLines() {
  const std::string synced = "[2026-11-15 12:34:56.789] | ";
  const std::string longest(47, 'P');
  const std::string uptime = "[ms=123456] ";
  int cases = 0;
  for (bool refuse : {true, false}) {
    for (const std::string& prefix : {synced, std::string(), longest}) {
      const std::string shown = prefix.empty() ? uptime : prefix;
      const size_t room = 48 + DEBUG_MSG_SIZE - 1 - shown.size();
      for (bool onSd : {false, true}) {
        for (bool rotate : {false, true}) {
          for (size_t length : {size_t(0), size_t(1), size_t(13), size_t(14), size_t(255), size_t(256), size_t(257),
                                room - 1, room, room + 1, size_t(400)}) {
            std::string text(length, 't');
            for (size_t i = 0; i < length; i += 5) text[i] = char('A' + i % 26);
            const size_t cap = rotate ? 400 : 100000;
            auto run = [&](bool production) {
              reset(onSd);
              clockPrefix = prefix;
              failLineAlloc = refuse;
              disk[destination()] = rotate ? rows(0, 60) : rows(0, 2);
              gStringHeap = StringHeapCount{};
              if (production) {
                appendTimestampedLine("/logs/events.log", text.c_str(), cap);
              } else {
                String line = buildTimestampPrefix();
                line += text.c_str();
                check(appendLineWithCap("/logs/events.log", line, cap), "the String line appends");
              }
              check(lockDepth == 0 && liveFiles == 0, "the line releases lock and handles");
              return std::make_pair(snapshot(), gStringHeap.made);
            };
            const int asked = lineAllocCalls;
            const bool hadBlock = !lineBlocks.empty();
            const auto now = run(true);
            const int asks = lineAllocCalls - asked;
            const auto old = run(false);
            check(now.first == old.first, "the same bytes and file calls as the String line");
            if (!rotate) check(disk.at(destination()) == rows(0, 2) + shown + text + "\r\n", "prefix, text and CRLF");
            check(requestedSpace == shown.size() + length + 512, "the same overflow admission");
            const bool block = !refuse && length < room;
            if (refuse) check(asks == 1, "a refused block is asked for again on the next line");
            else check(asks == (hadBlock ? 0 : 1), "the block is asked for once and kept");
            if (block && !rotate) {
              check(now.second == (onSd ? 1u : 0u), "the line makes no String block (only the SD path's own)");
              check(old.second == now.second + (shown.size() + length >= 19 ? 1u : 0u), "the String line made one");
            }
            if (!block) check(now.second == old.second, "without the block the String line is made as before");
            ++cases;
          }
        }
      }
    }
  }
  check(lineBlocks.size() == 1, "one line block made and kept for the boot");
  std::cerr << "timestamped lines: " << cases << " cases equal; ";
}
#endif

#ifndef HAVE_BATCHING
#define HAVE_BATCHING 0
#endif
#if HAVE_BATCHING
// S-8 and UP-03: batched appends, two-generation rotation and scaled caps, on
// the production writer region. The cases above ran with batching off (no
// cappedLogsInit: every line written through), as a board without PSRAM does.
static long countEvent(const std::string& event) { return std::count(events.begin(), events.end(), event); }
static long countPrefix(const std::string& prefix) {
  return std::count_if(events.begin(), events.end(), [&](const std::string& e) { return e.rfind(prefix, 0) == 0; });
}
static bool put(const char* path, const std::string& line, size_t cap = 100000) {
  const bool ok = appendLineWithCap(path, line.c_str(), line.size(), cap);
  check(lockDepth == 0 && liveFiles == 0 && mutexDepth == 0, "append releases lock, handles and mutex");
  return ok;
}
static void batchReset(bool onSd = false) {
  cappedLogsFlush(false);
  check(!cappedLogsPending(), "nothing pending between cases");
  reset(onSd);
  sCappedLogs.writeThrough = false;
  nowMs += 100000;
}
// What a reader shows: the older generation, then the current file.
static std::string wholeLog(const std::string& path) {
  return (disk.count(path + ".1") ? disk.at(path + ".1") : std::string()) +
         (disk.count(path) ? disk.at(path) : std::string());
}
static void batchingCases() {
  const char* kEvents = "/system/sys_logs/system-events.log";
  const char* kAudit = "/system/sys_logs/command-audit.log";
  const char* kLogin = "/system/sys_logs/failed_login.log";
  const char* kCrash = "/system/sys_logs/crash-history.log";
  const char* kErrors = "/system/sys_logs/errors.log";
  const std::string events_ = kEvents, audit = kAudit, errors = kErrors;

  // The late sweep: the log task's first tick removes a ".tmp" that older
  // firmware left beside the two logs boot recovery does not list. Once.
  reset(); disk[audit] = "audit"; disk[audit + ".tmp"] = "stale copy";
  disk[std::string(kCrash) + ".tmp"] = "orphan";
  cappedLogsFlushDue();
  check(disk.at(audit) == "audit" && !disk.count(audit + ".tmp"), "a stale command-audit.log.tmp is removed after boot");
  check(disk.count(kCrash) && disk.at(kCrash) == "orphan" && !disk.count(std::string(kCrash) + ".tmp"),
        "an orphaned crash-history.log.tmp is promoted");
  check(lockDepth == 0 && liveFiles == 0, "the late sweep releases the lock");
  events.clear(); disk[audit + ".tmp"] = "again"; cappedLogsFlushDue();
  check(events.empty() && disk.count(audit + ".tmp"), "the late sweep runs once per boot");

  // No PSRAM (no buffer, or no mutex): every line is written through.
  reset(); failBatchAlloc = true; cappedLogsInit(); failBatchAlloc = false;
  check(sCappedLogs.mutex == nullptr, "no batching without PSRAM buffers");
  check(put(kEvents, "direct") && countEvent("open:a") == 1 && disk.at(events_) == "direct\r\n" && mutexTakes == 0,
        "without PSRAM a line is written at once");
  reset(); failMutexCreate = true; cappedLogsInit(); failMutexCreate = false;
  check(sCappedLogs.mutex == nullptr, "no batching without the mutex");
  cappedLogsFlush(false); cappedLogsFlush(true); cappedLogsFlushDue();
  check(events.empty() && !cappedLogsPending(), "flushes are no-ops without batching");
  sCappedLogs.writeThrough = false;

  cappedLogsInit();
  check(sCappedLogs.mutex != nullptr && sCappedLogs.partitionBytes == 0, "batching on; unknown partition size leaves caps unscaled");
  check(cappedLogScaledCap(696969) == 696969, "an unscaled cap is the caller's");
  const int blocksAtInit = batchAllocCalls;
  cappedLogsInit();
  check(batchAllocCalls == blocksAtInit, "a second init allocates nothing");

  // 20 lines within 2 s: no file is touched until the log task's tick finds
  // the oldest line 1.5 s old, then one append open carries all of them.
  batchReset();
  std::string expected;
  for (int i = 0; i < 20; ++i) {
    const std::string line = "event line " + std::to_string(i);
    check(put(kEvents, line), "a batched line is accepted");
    expected += line + "\r\n";
    nowMs += 50;
  }
  check(events.empty() && disk.empty() && cappedLogsPending(), "20 lines within 2 s take no filesystem lock and touch no file");
  cappedLogsFlushDue();
  check(events.empty(), "not written before the oldest line is 1.5 s old");
  nowMs += 500;
  cappedLogsFlushDue();
  check(countEvent("open:a") == 1 && countPrefix("append:") == 1 && countEvent("lock") == 1,
        "one append open and one write for the 20 lines");
  check(disk.at(events_) == expected && bytesNoted == expected.size() && requestedSpace == expected.size() + 512,
        "the file holds the 20 lines in order; one space request and one byte note for the batch");
  check(!cappedLogsPending() && lockDepth == 0 && liveFiles == 0, "nothing pending after the flush");
  events.clear(); cappedLogsFlushDue();
  check(events.empty(), "an idle tick does nothing");

  // About 2 KB pending is written at once, by the appender.
  batchReset();
  const std::string row(198, 'x');  // 200 bytes with CRLF
  for (int i = 0; i < 10; ++i) check(put(kEvents, row), "row buffered");
  check(events.empty(), "2000 bytes pending are below the threshold");
  check(put(kEvents, row), "the row that passes 2 KB");
  check(countEvent("open:a") == 1 && disk.at(events_).size() == 2200 && !cappedLogsPending(), "2 KB pending flushes in one append");

  // A line that does not fit behind the pending ones: they are written first.
  batchReset();
  for (int i = 0; i < 10; ++i) check(put(kEvents, row), "row buffered");
  const std::string wide(600, 'y');
  check(put(kEvents, wide), "a line that needs room");
  check(countEvent("open:a") == 1 && disk.at(events_).size() == 2000 && cappedLogsPending(), "the full buffer is written, the line waits");
  cappedLogsFlush(false);
  check(disk.at(events_).size() == 2602 && disk.at(events_).compare(2000, 602, wide + "\r\n") == 0, "order kept");

  // A line longer than a buffer is written through, after the pending lines.
  batchReset();
  check(put(kEvents, "first") && put(kEvents, "second"), "two short lines wait");
  const std::string huge(3000, 'z');
  check(put(kEvents, huge), "a line longer than the buffer");
  check(countEvent("open:a") == 2 && countEvent("lock") == 1, "pending lines and the long line in one lock hold");
  check(disk.at(events_) == "first\r\nsecond\r\n" + huge + "\r\n" && !cappedLogsPending(), "the long line follows the earlier ones");

  // Without a log-task tick the next appender writes a batch 2 s old.
  batchReset();
  check(put(kEvents, "old"), "a line waits");
  nowMs += 1999; check(put(kEvents, "younger") && events.empty(), "1999 ms: still waiting");
  nowMs += 1; check(put(kEvents, "late"), "2 s after the oldest line");
  check(countEvent("open:a") == 1 && disk.at(events_) == "old\r\nyounger\r\nlate\r\n", "the appender writes a 2 s old batch");

  // The login logs and the crash history are never held.
  batchReset();
  check(put(kLogin, "FAILED | user=x") && countEvent("open:a") == 1 && disk.at(kLogin) == "FAILED | user=x\r\n",
        "a login line is on file when the call returns");
  check(put(kCrash, "=== boot died ===") && countEvent("open:a") == 2 && !cappedLogsPending(), "so is a crash-history line");

  // A flush with nothing pending takes no lock; with lines pending it writes every log.
  batchReset();
  cappedLogsFlush(false);
  check(events.empty(), "an empty flush takes no filesystem lock");
  check(put(kEvents, "e") && put(kAudit, "a") && events.empty(), "two logs pending");
  cappedLogsFlush(false);
  check(countEvent("open:a") == 2 && countEvent("lock") == 1 && disk.at(events_) == "e\r\n" && disk.at(audit) == "a\r\n",
        "one flush writes both logs under one lock hold");

  // An announced restart: everything pending is written, and the rest of the
  // boot's lines are written at once, in order.
  batchReset();
  check(put(kEvents, "before") && put(kAudit, "audit before"), "lines pending at the restart");
  cappedLogsFlush(true);
  check(disk.at(events_) == "before\r\n" && disk.at(audit) == "audit before\r\n" && !cappedLogsPending(), "flushed");
  events.clear();
  check(put(kEvents, "[EVENT][REBOOT] command") && countEvent("open:a") == 1 &&
        disk.at(events_) == "before\r\n[EVENT][REBOOT] command\r\n", "after the announcement a line is written at once");
  check(put(kAudit, "audit after") && disk.at(audit) == "audit before\r\naudit after\r\n" && !cappedLogsPending(),
        "the deferred restart's audit line too");
  batchReset();
  cappedLogsFlush(true);
  check(put(kEvents, "x") && countEvent("open:a") == 1, "the announcement latches with nothing pending too");

  // A batch refused for the reserve counts one drop per line.
  batchReset(true); sdAvailable = sdWritable = false;
  check(put(kEvents, "1") && put(kEvents, "2") && put(kEvents, "3"), "three lines wait");
  cappedLogsFlush(false);
  check(disk.empty() && reserveDrops == 3 && !seen("open:a"), "a refused batch is three counted drops");

  // A followed log in a serving build: the batch is one append, the copy
  // rotation trims once and leaves one trim note; a copy that cannot be
  // written restarts the log with the marker and the whole batch.
  batchReset(); disk[errors] = rows(0, 20);
  check(put(kErrors, "new-1", 100) && put(kErrors, "new-2", 100) && put(kErrors, "new-3", 100) && events.empty(), "three error lines wait");
  cappedLogsFlush(false);
  check(countEvent("open:a") == 1 && countEvent("rename") == 1 && catchupNotes.size() == 1 &&
        catchupNotes[0].rfind("trim:" + errors + ":", 0) == 0, "one append, one copy rotation, one trim note");
  check(disk.at(errors).size() <= 85 && disk.at(errors).size() >= 85 - 8 &&
        (rows(0, 20) + "new-1\r\nnew-2\r\nnew-3\r\n").compare(181 - disk.at(errors).size(), std::string::npos, disk.at(errors)) == 0 &&
        !disk.count(errors + ".1"), "the newest 85 percent stay in the one file");
  batchReset(); disk[errors] = rows(0, 20); copyError = true;
  check(put(kErrors, "new-1", 100) && put(kErrors, "new-2", 100), "two error lines wait");
  cappedLogsFlush(false);
  check(disk.at(errors) == restartMarker(160) + "new-1\r\nnew-2\r\n" && catchupNotes == std::vector<std::string>{"restart:" + errors},
        "a blocked copy restarts the log with the batch, one CRLF per line");

  // Slots: six batched logs; a seventh, or a name too long for a slot, is written through.
  batchReset();
  for (const char* extra : {"/system/sys_logs/i2c_errors.log", "/system/sys_logs/extra-a.log", "/system/sys_logs/extra-b.log"})
    check(put(extra, "slot") && events.empty(), "a further log gets a buffer");
  check(put("/system/sys_logs/extra-c.log", "seventh") && countEvent("open:a") == 1, "a seventh log is written at once");
  const std::string longName = "/system/sys_logs/" + std::string(70, 'n') + ".log";
  check(put(longName.c_str(), "long name") && countEvent("open:a") == 2, "a name longer than a slot is written at once");

  // Two-generation rotation, on flash and on the SD mirror: one rename, at
  // most one remove (FAT), no byte copied; a reader of "<log>.1" then "<log>"
  // sees the same newest lines.
  for (bool onSd : {false, true}) {
    const std::string dest = std::string(onSd ? "/sd" : "") + kEvents;
    batchReset(onSd); sCappedLogs.writeThrough = true;  // written at once: the rotation is what is under test
    disk[dest] = rows(0, 11); disk[dest + ".1"] = "older generation\r\n";
    check(put(kEvents, "new", 200), "a line below the generation size");
    check(!seen("rename") && disk.at(dest) == rows(0, 11) + "new\r\n" && disk.at(dest + ".1") == "older generation\r\n",
          "half the cap is not passed: no rotation");
    batchReset(onSd); sCappedLogs.writeThrough = true;
    disk[dest] = rows(0, 12); disk[dest + ".1"] = "older generation\r\n";
    check(put(kEvents, "new", 200), "a two-generation rotation succeeds");
    check(countEvent("rename") == 1 && countPrefix("remove:") == (onSd ? 1 : 0) && countPrefix("copy:") == 0 &&
          countPrefix("read:") == 0 && !seen("scan") && !seen("open:r") && !seen("open:w") && allocCalls == 2,
          "one rename, at most one remove, zero bytes copied");
    check(disk.at(dest + ".1") == rows(0, 12) + "new\r\n" && !disk.count(dest) && catchupNotes.empty(),
          "the full file is the older generation now; no catch-up note");
    check(put(kEvents, "next", 200) && disk.at(dest) == "next\r\n", "the next line starts a new file");
    check(wholeLog(dest) == rows(0, 12) + "new\r\nnext\r\n", "the reader returns the same newest lines");
    // A rename that fails keeps the file; the next line tries again.
    batchReset(onSd); sCappedLogs.writeThrough = true;
    disk[dest] = rows(0, 12); failRename = true;
    check(!put(kEvents, "new", 200) && disk.at(dest) == rows(0, 12) + "new\r\n", "a failed rename reports false and keeps every line");
    failRename = false;
    check(put(kEvents, "again", 200) && disk.at(dest + ".1") == rows(0, 12) + "new\r\nagain\r\n" && !disk.count(dest),
          "the next line rotates");

    // A file older firmware left far above the generation size: its newest
    // generation is copied to ".1" once, from a line start, and the file removed.
    std::string legacy;
    for (int i = 0; legacy.size() < 30000; ++i) legacy += "legacy row " + std::to_string(i) + "\r\n";
    batchReset(onSd); sCappedLogs.writeThrough = true;
    disk[dest] = legacy; disk[dest + ".1"] = "older generation\r\n";
    check(put(kEvents, "new", 20000), "an oversized legacy log is cut once");
    const std::string whole = legacy + "new\r\n";
    check(!disk.count(dest) && !disk.count(dest + ".tmp") && disk.count(dest + ".1"), "only the older generation is left");
    const std::string kept = disk.at(dest + ".1");
    check(kept.size() <= 10000 && kept.size() > 10000 - 32 && kept.rfind("legacy row ", 0) == 0 &&
          whole.compare(whole.size() - kept.size(), kept.size(), kept) == 0, "it holds the newest half cap from a line start");
    check(countEvent("rename") == 1 && maxRead <= 1024 && catchupNotes.empty(), "one promotion through the bounded copy buffer");
    check(put(kEvents, "next", 20000) && disk.at(dest) == "next\r\n" && wholeLog(dest) == kept + "next\r\n", "then a new file");
    // The copy cannot be made: the whole file becomes the older generation.
    batchReset(onSd); sCappedLogs.writeThrough = true;
    disk[dest] = legacy; failTempOpen = true;
    check(put(kEvents, "new", 20000) && disk.at(dest + ".1") == whole && !disk.count(dest) && !disk.count(dest + ".tmp"),
          "without room for the copy the file is renamed whole; nothing is lost");
    // Within one batch of the generation size: this writer's own rotation, a plain rename.
    batchReset(onSd); sCappedLogs.writeThrough = true;
    disk[dest] = legacy.substr(0, 10000 + 4096 - 5);
    check(put(kEvents, "new", 20000) && countEvent("rename") == 1 && !seen("open:r") && disk.at(dest + ".1").size() == 10000 + 4096,
          "a file within the slack is renamed, not copied");
  }

  // Boot does not wait for the cuts an update brings. Until the main loop
  // runs (cappedLogsNoteLoopRunning), a file above its new size is copied only
  // where older firmware copied too: above the cap its caller names. A plain
  // generation rename copies nothing and is not put off.
  {
    std::string legacy;
    for (int i = 0; legacy.size() < 15000; ++i) legacy += "legacy row " + std::to_string(i) + "\r\n";
    const std::string dest = kEvents;
    sCappedLogs.loopRunning = false;
    // Oversized for the two-generation writer (15 000 > 10 000 + 4096) but
    // within the caller's cap: older firmware would only have appended.
    batchReset(); sCappedLogs.writeThrough = true;
    disk[dest] = legacy;
    check(put(kEvents, "boot line", 20000) && disk.at(dest) == legacy + "boot line\r\n" && !disk.count(dest + ".1"),
          "during boot an oversized log only takes the line");
    check(!seen("open:r") && !seen("open:w") && !seen("rename") && countPrefix("copy:") == 0 && countPrefix("remove:") == 0,
          "during boot nothing is copied, renamed or removed for it");
    // Above the caller's cap older firmware rotated at this line too: cut now.
    batchReset(); sCappedLogs.writeThrough = true;
    std::string over = legacy;
    while (over.size() <= 20000) over += "more legacy\r\n";
    disk[dest] = over;
    check(put(kEvents, "boot line", 20000) && !disk.count(dest) && disk.at(dest + ".1").size() <= 10000,
          "during boot a log above the caller's cap is still cut (a restart loop cannot grow it)");
    // A plain generation rename is not put off.
    batchReset(); sCappedLogs.writeThrough = true;
    disk[dest] = rows(0, 12);
    check(put(kEvents, "new", 200) && countEvent("rename") == 1 && !seen("open:r") && disk.at(dest + ".1") == rows(0, 12) + "new\r\n",
          "during boot a full generation is renamed as always");
    // The copy rotation of a followed log, with a scaled cap (1856 KiB layout:
    // errors.log 45 000 of 262 144).
    partitionKnown = true; hostPartition.size = 1856u * 1024u; cappedLogsInit();
    batchReset(); sCappedLogs.writeThrough = true;
    std::string old;
    while (old.size() < 200000) old += "an error line from before the update\r\n";
    disk[errors] = old;
    check(put(kErrors, "boot error", 256 * 1024) && disk.at(errors) == old + "boot error\r\n" && !seen("open:r") &&
          !seen("rename") && catchupNotes.empty(), "during boot a followed log above its scaled cap only takes the line");
    // Boot is over: the first write does the cut, once.
    cappedLogsNoteLoopRunning();
    check(sCappedLogs.loopRunning, "the loop's tick ends the boot phase");
    batchReset(); sCappedLogs.writeThrough = true;
    disk[errors] = old;
    check(put(kErrors, "first line after boot", 256 * 1024) && disk.at(errors).size() <= 45000 * 85 / 100 + 64 &&
          disk.at(errors).size() > 30000 && seen("rename") && catchupNotes.size() == 1,
          "after boot the followed log is trimmed by copy to 85 % of its scaled cap");
    partitionKnown = false; cappedLogsInit();
    batchReset(); sCappedLogs.writeThrough = true;
    disk[dest] = legacy;
    check(put(kEvents, "first line after boot", 20000) && !disk.count(dest) && disk.count(dest + ".1") &&
          disk.at(dest + ".1").size() <= 10000, "after boot the oversized log is cut to its newest half cap");
    check(logcaps::copyAllowedNow(false, 100, 100) == false && logcaps::copyAllowedNow(false, 101, 100) &&
          logcaps::copyAllowedNow(true, 1, 100), "the rule: boot over, or above the caller's cap");
  }

  // In a build that serves no catch-up the three followed logs are
  // two-generation logs too: no copy, no note.
  batchReset(); sCappedLogs.writeThrough = true; kCatchupServesLogs = false;
  disk[errors] = rows(0, 12);
  check(put(kErrors, "new", 200) && countEvent("rename") == 1 && !seen("open:r") && catchupNotes.empty() &&
        disk.at(errors + ".1") == rows(0, 12) + "new\r\n" && !disk.count(errors), "errors.log rotates by rename without catch-up");
  kCatchupServesLogs = true;

  // UP-03: the writer scales the cap its caller passes to the partition.
  batchReset();
  partitionKnown = true; hostPartition.size = 1856u * 1024u; cappedLogsInit();
  check(cappedLogScaledCap(696969) == 113927 && cappedLogScaledCap(500 * 1024) == 84607 &&
        cappedLogScaledCap(256 * 1024) == 45000 && cappedLogScaledCap(64 * 1024) == 13835 &&
        cappedLogScaledCap(100) == 100,
        "caps on the 1856 KiB layout; a cap below the floor is left alone");
  disk[kLogin] = std::string(56960, 'l');
  check(put(kLogin, "abcd", 696969) && countEvent("rename") == 1 && disk.count(std::string(kLogin) + ".1") && !disk.count(kLogin),
        "a login log rotates at half its scaled cap (56963 bytes), not half of 696969");
  hostPartition.size = 9068u * 1024u; cappedLogsInit();
  batchReset(); disk[kLogin] = std::string(56960, 'l');
  check(cappedLogScaledCap(696969) == 572323 && put(kLogin, "abcd", 696969) && !seen("rename"), "the P4X-EYE layout keeps it");
  partitionKnown = false; cappedLogsInit();
  check(cappedLogScaledCap(696969) == 696969, "back to unscaled");
  batchReset();
  check(mutexDepth == 0 && lockDepth == 0 && liveFiles == 0, "batching cases release everything");
}
#if HAVE_SEALED_LOG
// B11 with batching on: a flushed batch is sealed line by line, and the
// two-generation rotation starts a new marked file.
static void sealedBatchCases() {
  const char* kEvents = "/system/sys_logs/system-events.log";   // two generations (not followed by catch-up)
  const std::string card = std::string("/sd") + kEvents;
  auto sealBatchReset = [] {
    batchReset(true);
    cardSealing = true; keyAvailable = true; flashBelowReserve = false; ++mountGeneration;
  };
  // Five lines inside the batch window: no file is touched. The flush then
  // writes them with one append open: the mark, and one sealed row per line.
  sealBatchReset();
  std::string expected = markLine(), shown;
  for (int i = 0; i < 5; ++i) {
    const std::string line = "event line " + std::to_string(i);
    check(put(kEvents, line), "a batched line is accepted");
    expected += sealedRow(line);
    shown += line + "\n";
  }
  check(!disk.count(card) && cappedLogsPending() && countEvent("open:a") == 0, "nothing reaches the card before the flush");
  events.clear();
  cappedLogsFlush(false);
  check(disk.count(card) && disk.at(card) == expected && revealed(disk.at(card)) == shown,
        "the flushed batch is the mark and one sealed row per line, in order");
  check(countEvent("open:a") == 1 && countPrefix("append:") == 6, "one open; the mark and each row are one write each");
  check(disk.at(card).find("event line") == std::string::npos && !disk.count(kEvents), "no plain line on the card, none on flash");
  wholeRows(disk.at(card));
  // A second batch goes on under the same mark, without reading the head again.
  check(put(kEvents, "later one") && put(kEvents, "later two"), "more lines");
  events.clear();
  cappedLogsFlush(false);
  check(disk.at(card) == expected + sealedRow("later one") + sealedRow("later two") && countEvent("open:r") == 0,
        "the next batch appends its rows");
  // A batch with a line over 1023 bytes: that line is split, the others are not.
  sealBatchReset();
  const std::string longLine(1500, 'q');
  check(put(kEvents, "before") && put(kEvents, longLine) && put(kEvents, "after"), "three lines, one long");
  cappedLogsFlush(false);
  check(disk.at(card) == markLine() + sealedRow("before") + sealedRow(longLine.substr(0, 1023)) +
                             sealedRow(longLine.substr(1023)) + sealedRow("after"), "the long line is two rows, in place");

  // Two-generation rotation on the card. The cap there is twice the caller's
  // (200), so a generation is 100 stored bytes: the file that passes it is
  // renamed to ".1" whole, and the next line starts a new file with its mark.
  sealBatchReset();
  const std::string older = markLine() + sealedRows(0, 5);            // 104 stored bytes
  disk[card] = older;
  check(put(kEvents, "roll", 100), "a line past the generation size");
  cappedLogsFlush(false);
  check(!disk.count(card) && disk.count(card + ".1") && disk.at(card + ".1") == older + sealedRow("roll"),
        "the full generation is renamed with its mark and every row");
  wholeRows(disk.at(card + ".1"));
  check(put(kEvents, "next", 100), "the next line");
  cappedLogsFlush(false);
  check(disk.at(card) == markLine() + sealedRow("next"), "a new generation starts with the mark");
  check(revealed(disk.at(card + ".1")) + revealed(disk.at(card)) ==
            "old-00\nold-01\nold-02\nold-03\nold-04\nroll\nnext\n",
        "a reader of both generations sees every line in order");
  // Review of 2026-11-22: the card is pulled in the middle of a batch. The
  // rows it verifiably took stay there and are not written a second time;
  // flash gets the lines from the first one the card does not hold whole.
  sealBatchReset();
  check(put(kEvents, "batch one") && put(kEvents, "batch two") && put(kEvents, "batch three"), "three lines pending");
  cardKeepsBytes = (markLine() + sealedRow("batch one")).size() + 11;   // the first row and a piece of the second
  cappedLogsFlush(false);
  check(disk.at(card).rfind(markLine() + sealedRow("batch one"), 0) == 0 &&
            disk.at(card).size() == (markLine() + sealedRow("batch one")).size() + 11,
        "the card holds the first row and a torn second one");
  check(disk.at(kEvents) == "batch two\r\nbatch three\r\n" && sdFailures == 1,
        "flash takes the lines the card did not take whole, each once");
  // Nothing of the batch arrived: all of it goes to flash.
  sealBatchReset();
  check(put(kEvents, "lost one") && put(kEvents, "lost two"), "two lines pending");
  cardKeepsBytes = 0;
  cappedLogsFlush(false);
  check(disk.at(kEvents) == "lost one\r\nlost two\r\n" && sdFailures == 1 && disk.at(card).empty(),
        "a batch the card lost whole lands on flash whole");
  // The card fails under a batch: the whole batch lands on flash, plain.
  sealBatchReset();
  check(put(kEvents, "kept one") && put(kEvents, "kept two"), "two lines pending");
  failSdAppendOpen = true;
  cappedLogsFlush(false);
  check(!disk.count(card) && disk.at(kEvents) == "kept one\r\nkept two\r\n" && sdFailures == 1,
        "a card that does not open sends the batch to flash");
  batchReset();
  cardSealing = false;
}
#endif
#endif

int main() {
#if HAVE_BATCHING
  // The main loop is running (its tick called this): the device is past boot.
  // The boot-phase cases in batchingCases() switch it off for themselves.
  cappedLogsNoteLoopRunning();
#endif
  // Must precede successful rotation: the production allocation persists for the boot.
  reset(); disk[destination()] = rows(0, 20); failAlloc = true;
  if (BASELINE) check(!append(), "PSRAM allocation failure reported");
  else { check(append(), "PSRAM allocation failure restarts the log"); restartedRotation(rows(0, 20)); }
  for (bool onSd : {false, true}) { normalCases(onSd); failureCases(onSd); }
  if (!BASELINE) { mirrorRecovery(); lowSpaceHoldsCap(false); lowSpaceHoldsCap(true); reserveAndSdLatch(); }
#if HAVE_SEALED_LOG
  if (!BASELINE) sealedCardCases();
#endif
#if HAVE_TIMESTAMPED_LINE
  if (!BASELINE) timestampedLines();
#endif
#if HAVE_BATCHING
  if (!BASELINE) batchingCases();
#if HAVE_SEALED_LOG
  if (!BASELINE) sealedBatchCases();
#endif
#endif
  check(allocCalls == 2, "one failed allocation then one reusable PSRAM allocation across all logs");
  reset(); recovering = true; cleanupLogOrphan(nullptr); recovering = false; check(events.empty(), "null recovery path is a no-op");
  std::cerr << cases << " scenarios, " << checks << " assertions passed (real Arduino Print and String)\n";
}
