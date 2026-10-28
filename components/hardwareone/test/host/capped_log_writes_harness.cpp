// Executes production code against observable storage and allocation boundaries.
#include <Print.h>
#include <algorithm>
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
static int failAppendWrite = 0, appendWrites = 0, failCopyWrite = 0, copyWrites = 0;
static int failCopyRead = 0, copyReads = 0, failScanRead = 0, scanReads = 0;
static size_t shortCount = 0, bytesNoted = 0, requestedSpace = 0, maxRead = 0;
static size_t tempSizeOffset = 0, inputSizeOffset = 0, readLimit = 1024;
static std::string refuseRemove;
static std::vector<std::unique_ptr<uint8_t[]>> allocations;
static void io(const std::string& event) {
  check(lockDepth == 1 || recovering, "append I/O stays under filesystem lock");
  events.push_back(event);
}
struct FsLockGuard {
  explicit FsLockGuard(const char*) { check(lockDepth == 0, "one filesystem lock"); ++lockDepth; events.push_back("lock"); }
  ~FsLockGuard() { check(liveFiles == 0, "files closed before unlock"); --lockDepth; events.push_back("unlock"); }
};
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  ++allocCalls; io("alloc");
  check(size == 1024 && pref == AllocPref::PreferPSRAM && std::string(tag) == "logrot.buf", "retained 1 KB PSRAM buffer policy");
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
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    check(bool(*this) && handle->mode != "r", "write uses open writable file");
    const bool append = handle->mode == "a";
    const int call = append ? ++appendWrites : ++copyWrites;
    io(std::string(append ? "append:" : "copy:") + std::to_string(count));
    const size_t actual = call == (append ? failAppendWrite : failCopyWrite) ? std::min(shortCount, count) : count;
    disk[handle->path].append(reinterpret_cast<const char*>(bytes), actual);
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
  void close() {
    check(bool(*this), "each handle closed once"); io("close:" + handle->mode); handle->closed = true; --liveFiles;
  }
};
}
using fs::File;

namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return text(path).rfind("/sd/", 0) == 0 ? SDCARD : INTERNAL; }
static bool isSDAvailable() { return sdAvailable; }
static void resolveOverflowPath(const char* path, size_t need, char* dest, size_t capacity) {
  io("resolve"); requestedSpace = need;
  std::snprintf(dest, capacity, "%s%s", overflow ? "/sd" : "", path);
}
static File open(const String& path, const char* mode, bool create = false) {
  io(std::string("open:") + mode); const auto name = text(path);
  if (std::strcmp(mode, "r") == 0) {
    check(!create, "read opens do not create");
    if (failReadOpen || !disk.count(name)) return {};
  } else {
    check(create, "append/temp opens create");
    if ((std::strcmp(mode, "a") == 0 && failAppendOpen) || (std::strcmp(mode, "w") == 0 && failTempOpen)) return {};
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

// INSERT_PRODUCTION

static std::string destination() { return overflow ? "/sd/logs/events.log" : "/logs/events.log"; }
static bool seen(const std::string& event) { return std::find(events.begin(), events.end(), event) != events.end(); }
static size_t eventIndex(const std::string& event) {
  auto item = std::find(events.begin(), events.end(), event); check(item != events.end(), event.c_str()); return static_cast<size_t>(item - events.begin());
}
static void reset(bool onSd = false) {
  check(lockDepth == 0 && liveFiles == 0, "previous case releases resources");
  ++cases; disk.clear(); events.clear(); recovering = false; overflow = onSd; sdAvailable = onSd;
  failAlloc = failSeek = failRename = appendError = copyError = flushError = false;
  failAppendOpen = failReadOpen = failTempOpen = false;
  failAppendWrite = appendWrites = failCopyWrite = copyWrites = failCopyRead = copyReads = failScanRead = scanReads = 0;
  shortCount = bytesNoted = requestedSpace = maxRead = tempSizeOffset = inputSizeOffset = 0;
  readLimit = 1024; refuseRemove.clear();
}
static std::string rows(int begin, int end) {
  std::string out;
  for (int i = begin; i < end; ++i) { char line[16]; std::snprintf(line, sizeof(line), "old-%02d\r\n", i); out += line; }
  return out;
}
static bool append(const char* line = "new", size_t cap = 100) {
  const bool result = appendLineWithCap("/logs/events.log", String(line), cap);
  check(lockDepth == 0 && liveFiles == 0, "append releases lock and handles");
  check(requestedSpace == std::strlen(line) + 512, "overflow admission retains existing margin");
  return result;
}
static void recover() {
  recovering = true; cleanupLogOrphan("/logs/events.log"); recovering = false;
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
static void normalCases(bool onSd) {
  reset(onSd); check(append("hello", 20), "ordinary append succeeds");
  check(disk.at(destination()) == "hello\r\n" && bytesNoted == 7, "real Print appends CRLF and accounts seven bytes");
  check(!seen("flush:a") && !seen("open:r") && !seen("size:a"), "append fast path avoids extra flush/read/stat");
  normalResult("append", true);
  reset(onSd); disk[destination()] = "older\r\n"; check(append("new", 12), "exact cap does not rotate");
  check(disk.at(destination()) == "older\r\nnew\r\n" && !seen("open:r"), "cap equality retains bytes"); normalResult("exact-cap", true);
  reset(onSd); check(append("", 2), "empty line succeeds"); check(disk.at(destination()) == "\r\n", "empty line keeps CRLF"); normalResult("empty-line", true);
  reset(onSd); disk[destination()] = rows(0, 20); disk[destination() + ".tmp"] = "stale";
  check(append(), "rotation succeeds"); check(disk.at(destination()) == rows(11, 20) + "new\r\n", "85 percent target advances to next whole line");
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
  check(append() == bool(BASELINE), "short tail write rejects false success");
  if (BASELINE) check(disk.at(destination()) == (rows(11, 20) + "new\r\n").substr(0, 7), "baseline reproduces incomplete-tail promotion");
  else untouchedRotation(original);
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
  reset(onSd); disk[destination()] = original; failTempOpen = true; check(!append(), "rotation temp open failure reported"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; copyError = true; check(!append(), "temp Print error rejects full count"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; flushError = true; check(!append(), "temp flush error rejects promotion"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; tempSizeOffset = 1; check(!append(), "temp size mismatch rejects promotion"); untouchedRotation(original);
  reset(onSd); disk[destination()] = original; failCopyRead = 1; check(!append(), "premature copy EOF rejects promotion"); untouchedRotation(original);
  reset(onSd); disk[destination()] = std::string(5000, 'x') + "\n" + std::string(3000, 'y') + "\n";
  const auto longOriginal = disk.at(destination()); failCopyRead = 2; check(!append("new", 5000), "premature read after full chunk rejects promotion"); untouchedRotation(longOriginal);
  reset(onSd); disk[destination()] = longOriginal; failCopyWrite = 2; shortCount = 17; check(!append("new", 5000), "later short write rejects promotion"); untouchedRotation(longOriginal);
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
int main() {
  // Must precede successful rotation: the production allocation persists for the boot.
  reset(); disk[destination()] = rows(0, 20); failAlloc = true; check(!append(), "PSRAM allocation failure reported");
  if (!BASELINE) untouchedRotation(rows(0, 20));
  for (bool onSd : {false, true}) { normalCases(onSd); failureCases(onSd); }
  if (!BASELINE) mirrorRecovery();
  check(allocCalls == 2, "one failed allocation then one reusable PSRAM allocation across all logs");
  reset(); recovering = true; cleanupLogOrphan(nullptr); recovering = false; check(events.empty(), "null recovery path is a no-op");
  std::cerr << cases << " scenarios, " << checks << " assertions passed (real Arduino Print and String)\n";
}
