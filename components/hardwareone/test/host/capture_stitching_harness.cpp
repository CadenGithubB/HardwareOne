// Production handlers and byte-copy policy are inserted by test_capture_stitching.py.
// The in-memory file backend records identity, lock lifetime, I/O and close order.
#include <WString.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static size_t checks = 0, scenarios = 0;
static void check(bool value, const char* description) {
  ++checks;
  if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
static std::string text(const String& value) { return value.c_str(); }
// INSERT_PARSER

struct AuthContext { std::string identity, scope, purpose; };
static const AuthContext caller{"caller", "/caller-scope", "caller-purpose"};
static std::vector<std::string> events;
static int lockDepth = 0, liveFiles = 0;
static bool bufferReady = true, denyWrite = false;
static bool printError = false, sizeError = false, removeFail = false, failLineRead = false;
static int renameCalls = 0;
static std::set<int> renameFailures;
static std::set<std::string> directories;
bool gCLIValidateOnly = false;
static char debugBuffer[1024];
static std::map<std::string, std::string> disk;
static std::set<std::string> unreadable, refuseOpen;
static int bodyWrites = 0, separatorWrites = 0, failBody = 0, failSeparator = 0;
static size_t partialBody = 0;
static std::string failReadPath;
static int failReadAt = 0, failedReadResult = 0;

static void io(const std::string& event) {
  check(lockDepth == 1, "file operations stay under the caller's lock");
  events.push_back(event);
}
struct FsLockGuard {
  bool held = false;
  explicit FsLockGuard(const char* owner) {
    if (lockDepth) return;
    held = true; ++lockDepth; events.push_back(std::string("lock:") + owner);
  }
  ~FsLockGuard() {
    if (!held) return;
    check(liveFiles == 0, "all files closed before lock release");
    events.push_back("unlock"); --lockDepth;
  }
};
static const AuthContext& currentAuthContext() {
  events.push_back("auth:caller"); return caller;
}
static bool ensureDebugBuffer() { events.push_back("buffer"); return bufferReady; }
static char* getDebugBuffer() { return debugBuffer; }

namespace fs {
class File {
  struct Handle { std::string path, mode; size_t offset = 0; bool closed = false; int copyReads = 0; };
  std::shared_ptr<Handle> handle;
public:
  File() = default;
  File(std::string path, std::string mode) : handle(std::make_shared<Handle>()) {
    handle->path = path; handle->mode = mode; ++liveFiles;
  }
  explicit operator bool() const { return handle && !handle->closed; }
  bool isDirectory() const { return directories.count(handle->path); }
  int getWriteError() const { return printError; }
  size_t position() const { return handle->offset; }
  size_t size() const { return disk.at(handle->path).size() + (sizeError && handle->mode == "w" ? 1 : 0); }
  void flush() { io("flush:" + handle->path); }
  String readStringUntil(char delimiter) {
    String result; if (failLineRead) return result;
    while (available()) { char c = disk.at(handle->path)[handle->offset++]; if(c == delimiter) break; result += c; }
    return result;
  }
  int available() {
    check(bool(*this), "available on open file");
    return static_cast<int>(disk.at(handle->path).size() - handle->offset);
  }
  int read(uint8_t* destination, size_t length) {
    check(bool(*this) && handle->mode == "r", "read uses open input");
    io("read:" + handle->path + ":" + std::to_string(length));
    if (length == 512 && ++handle->copyReads == failReadAt && handle->path == failReadPath)
      return failedReadResult;
    const auto& content = disk.at(handle->path);
    const size_t count = std::min(length, content.size() - handle->offset);
    std::memcpy(destination, content.data() + handle->offset, count);
    handle->offset += count;
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* source, size_t length) {
    check(bool(*this) && handle->mode == "w", "write uses open output");
    io("body:" + std::to_string(length));
    const size_t count = (++bodyWrites == failBody) ? std::min(partialBody, length) : length;
    disk[handle->path].append(reinterpret_cast<const char*>(source), count);
    return count;
  }
  size_t write(uint8_t value) {
    check(bool(*this) && handle->mode == "w", "separator uses open output");
    io("separator");
    if (++separatorWrites == failSeparator) return 0;
    disk[handle->path] += static_cast<char>(value);
    return 1;
  }
  bool seek(size_t position) {
    check(bool(*this), "seek uses open input");
    io("seek:" + handle->path + ":" + std::to_string(position));
    handle->offset = std::min(position, disk.at(handle->path).size()); return true;
  }
  void close() {
    check(bool(*this), "each opened file is closed exactly once");
    io("close:" + handle->path); handle->closed = true; --liveFiles;
  }
};
}
using fs::File;

static std::vector<std::string> fsEvents;
static void logSystemEvent(const char*, const char* format, ...) {
  char line[256];
  va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
  fsEvents.emplace_back(line);
}
namespace VFS {
namespace Scopes {
constexpr const char* CAPTURES = "/logging_captures";
constexpr const char* CAPTURES_SD = "/sd/logging_captures";
}
// INSERT_SCOPES
static AuthContext systemAuth(const char* scope, const char* purpose) {
  check(lockDepth == 0, "write identity prepared before locking");
  events.push_back(std::string("auth:system:") + scope + ":" + purpose);
  return {"system", scope, purpose};
}
static void readIdentity(const AuthContext& auth) {
  check(auth.identity == caller.identity && auth.scope == caller.scope && auth.purpose == caller.purpose,
        "all input access retains the caller identity");
}
static void writeIdentity(const AuthContext& auth) {
  check(auth.identity == "system", "output uses system identity");
  check(auth.scope == Scopes::CAPTURES || auth.scope == Scopes::CAPTURES_SD, "system writer has capture scope");
  check(auth.purpose == "gps.stitch" || auth.purpose == "health.stitch" || auth.purpose == "capcrypt.export", "writer purpose retained");
}
static bool existsGuarded(const String& path, const AuthContext& auth) {
  io("exists:" + text(path)); if (auth.identity == "caller") readIdentity(auth); else writeIdentity(auth);
  String normalized;
  return normalizeFsPath(path, normalized) && disk.count(text(normalized)) && !unreadable.count(text(normalized));
}
static bool mkdirGuarded(const String& path, const AuthContext& auth) {
  io("mkdir:" + text(path)); writeIdentity(auth); return true;
}
static File openGuarded(const String& path, const char* mode, const AuthContext& auth, bool create = false) {
  io(std::string("open:") + mode + ":" + text(path));
  String normalized;
  const bool valid = normalizeFsPath(path, normalized);
  const auto key = text(normalized);
  if (std::strcmp(mode, "r") == 0) {
    if (auth.identity == "caller") readIdentity(auth); else writeIdentity(auth);
    check(!create, "read open does not request create");
    if (!valid || !disk.count(key) || unreadable.count(key) || refuseOpen.count(key)) return {};
  } else {
    writeIdentity(auth); check(create, "output open retains create policy");
    check(auth.scope == captureScopeFor(path), "scope follows output storage tier");
    if (!valid || !pathInCaptureTree(normalized) || denyWrite) return {};
    disk[key].clear();
  }
  return File(key, mode);
}
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return path.startsWith("/sd/") ? SDCARD : INTERNAL; }
static bool guardedNormalize(const String& path, String& normalized, const char*, const AuthContext&) { return normalizeFsPath(path, normalized); }
static bool renameGuarded(const String& from, const String& to, const AuthContext& auth) {
  io("rename:" + text(from) + ":" + text(to)); writeIdentity(auth);
  check(liveFiles == 0, "promotion only after all handles close");
  if (renameFailures.count(++renameCalls) || !disk.count(text(from))) return false;
  if (getStorageType(from) == SDCARD && disk.count(text(to))) return false;
  disk[text(to)] = disk.at(text(from)); disk.erase(text(from)); return true;
}
static bool removeGuarded(const String& path, const AuthContext& auth) {
  io("remove:" + text(path)); writeIdentity(auth);
  if (removeFail || directories.count(text(path))) return false;
  return disk.erase(text(path)) != 0;
}
// INSERT_REPLACEMENT

}
static constexpr const char* CAPTURE_DIR_SENSORS = "/logging_captures/sensors";
struct { int captureEncryptMode = 0; } gSettings;
static bool gSensorLoggingRunning = false, gSensorLogSealed = false;
static const char* captureEncryptModeName(int) { return "off"; }
static bool captureCryptoKeyReady() { return false; }
static void setSetting(int& value, int next) { value = next; }
static constexpr const char* CAPCRYPT_UNDECRYPTABLE = "[UNDECRYPTABLE]";
static bool captureCryptoRevealLine(String& line) {
  if (!line.startsWith("cipher:")) return false;
  line = line == "cipher:bad" ? CAPCRYPT_UNDECRYPTABLE : "decoded";
  return true;
}
// INSERT_PRODUCTION

static bool health = false;
static std::string outputPath() {
  return health ? "/logging_captures/sensors/out.csv" : "/logging_captures/tracks/out.log";
}
static std::string writePath() { return outputPath() + (HAS_STAGED ? ".tmp" : ""); }
static void reset() {
  check(lockDepth == 0 && liveFiles == 0, "previous case cleaned up");
  events.clear(); disk.clear(); unreadable.clear(); refuseOpen.clear();
  bodyWrites = separatorWrites = failBody = failSeparator = failReadAt = 0;
  partialBody = 0; failReadPath.clear(); failedReadResult = 0;
  printError = sizeError = removeFail = failLineRead = false; renameCalls = 0; renameFailures.clear(); directories.clear();
  bufferReady = true; denyWrite = gCLIValidateOnly = false;
  std::memset(debugBuffer, 0x55, sizeof(debugBuffer));
}
static bool seen(const std::string& event) {
  return std::find(events.begin(), events.end(), event) != events.end();
}
static size_t where(const std::string& event) {
  const auto item = std::find(events.begin(), events.end(), event);
  check(item != events.end(), event.c_str()); return static_cast<size_t>(item - events.begin());
}
static std::string invoke(const std::string& name, const std::string& args, bool transcript = true) {
  ++scenarios;
  const std::string result = health ? cmd_healthlogmerge(String(args.c_str())) : cmd_gpstrackmerge(String(args.c_str()));
  check(lockDepth == 0 && liveFiles == 0, "handler returns without owned resources");
  if (transcript && result.rfind("Stitched", 0) == 0) {
    std::cout << (health ? "health:" : "gps:") << name << ' ' << std::quoted(result) << '\n';
    for (const auto& file : disk) std::cout << std::quoted(file.first) << '=' << std::quoted(file.second) << '\n';
  }
  return result;
}
static std::string success(int files, size_t bytes, const std::string& path = "") {
  const std::string destination = path.empty() ? outputPath() : path;
  std::string result = "Stitched " + std::to_string(files) + " files into " + destination + " (" + std::to_string(bytes) + " bytes)";
  if (!health) result += ". Load it: gpstrack load \"" + destination + "\"";
  return result;
}
static std::string writeError(size_t bytes) {
  return "Error: write failed (filesystem full?) after " + std::to_string(bytes) + " bytes to " + outputPath();
}

static void normalCases() {
  for (size_t length : {size_t(0), size_t(1), size_t(511), size_t(512), size_t(513), size_t(1537)}) {
    for (bool newline : {false, true}) {
      reset();
      disk["/one"] = std::string(length, 'a');
      if (newline) disk["/one"] += '\n';
      disk["/two"] = "second\n";
      std::string expected = disk["/one"];
      if (!expected.empty() && expected.back() != '\n') expected += '\n';
      expected += disk["/two"];
      check(invoke("length-" + std::to_string(length) + (newline ? "-nl" : "-raw"), "\"out\" \"/one\" \"/two\"") == success(2, expected.size()), "exact successful response");
      check(disk.at(outputPath()) == expected, "input order and clean boundaries");
      check(where("exists:/two") < where("open:w:" + writePath()), "every input validated before output creation");
      check(where("close:/one") < where("open:r:/two"), "previous input closed before next input");
    }
  }
  for (const std::string& destination : {"/logging_captures/out.csv", "/logging_captures/out.log", "/logging_captures/out.txt", "/sd/logging_captures/out.txt"}) {
    reset(); disk["/a b"] = "row";
    check(invoke(destination, "\"" + destination + "\" \"a b\"") == success(1, 4, destination), "explicit extension and quoted spaces preserved");
  }
  reset(); disk["/one"] = "row\n";
  std::string args = "\"out\"";
  for (int i = 0; i < 9; ++i) args += " \"/one\"";
  check(invoke("max-nine", args) == success(9, 36), "nine inputs accepted");
  reset(); disk["/one"] = "row\n";
  check(invoke("extra-token", args + " \"/missing\"") == success(9, 36), "existing parser truncates beyond ten tokens");
}

static void errorCases() {
  const std::vector<std::pair<std::string, std::string>> bad = {
    {"", "Usage:"}, {"\"out\"", "Usage:"}, {"out \"/one\"", "name must be in quotes"},
    {"\"\" \"/one\"", "name is empty"}, {"\"out\" /one", "path must be in quotes"},
    {"\"out\" \"\"", "path is empty"}, {"\"out\" \"/one", "unmatched quote"},
    {"\"out\" \"/missing\"", "input not found"}, {"\"out\" \"/one\" \"/missing\"", "input not found"},
    {"\"out\" \"/../one\"", "input not found"}, {"\"out\" \"/bad\nname\"", "input not found"},
    {"\"/system/log.txt\" \"/one\"", "output must be inside"},
    {"\"/logging_capturesX/a.txt\" \"/one\"", "output must be inside"},
    {"\"/sd/logging_capturesX/a.txt\" \"/one\"", "output must be inside"},
  };
  for (const auto& test : bad) {
    reset(); disk["/one"] = "a\n"; disk[outputPath()] = "keep"; disk["/system/log.txt"] = "private";
    const auto before = disk;
    check(invoke(test.second, test.first).find(test.second) != std::string::npos, "argument validation response");
    check(disk == before && bodyWrites == 0, "invalid inputs preserve existing output");
    check(std::none_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("open:w:", 0) == 0; }), "invalid inputs never open output for write");
  }
  reset(); disk[outputPath()] = "keep";
  check(invoke("same-file", "\"out\" \"" + outputPath() + "\"") == "Error: an input file is the same as the output", "output equals input refused");
  check(disk.at(outputPath()) == "keep", "same file not truncated");
  reset(); disk["/one"] = "private"; unreadable.insert("/one");
  check(invoke("read-denied", "\"out\" \"/one\"") == "Error: input not found: /one", "read permission denial retained");
  reset(); disk["/one"] = "a"; denyWrite = true;
  check(invoke("write-denied", "\"out\" \"/one\"").find("Error: cannot create output:") == 0, "output creation failure retained");
  check(!seen("open:r:/one"), "no input opened after output failure");
  for (const std::string& path : {"/one", "/two"}) {
    reset(); disk["/one"] = "first\n"; disk["/two"] = "second\n"; refuseOpen.insert(path);
    check(invoke("open-fail-" + path, "\"out\" \"/one\" \"/two\"") == "Error: cannot open input: " + path, "failed input open response");
    check(HAS_STAGED ? !disk.count(outputPath()) : disk.at(outputPath()) == (path == "/one" ? "" : "first\n"), "failed output is unpublished");
  }
  for (bool validate : {false, true}) {
    reset(); bufferReady = false; gCLIValidateOnly = validate;
    check(invoke("early-" + std::to_string(validate), "nonsense") == (validate ? "VALID" : "Error: Debug buffer unavailable"), "early return contract");
    check(events.size() == (validate ? 0U : 1U), "validation and buffer failure perform no I/O");
  }
}

static void failureCases() {
  for (int failedWrite : {1, 2, 4}) {
    reset(); disk["/one"] = std::string(1025, 'a') + '\n'; disk["/two"] = std::string(600, 'b');
    failBody = failedWrite; partialBody = 3;
    const size_t completeBytes = failedWrite == 4 ? 1026 : (failedWrite - 1) * 512;
    check(invoke("short-body-" + std::to_string(failedWrite), "\"out\" \"/one\" \"/two\"") == writeError(completeBytes), "short write counts only completed chunks");
    check(HAS_STAGED ? !disk.count(outputPath()) : disk.at(outputPath()).size() == completeBytes + 3, "partial writes are unpublished");
    check(separatorWrites == 0, "body failure does not append separator");
    if (failedWrite < 4) check(!seen("open:r:/two"), "copy stops before later input");
  }
  for (int readResult : {0, -1}) {
    for (int readAt : {1, 2}) {
      reset(); disk["/one"] = std::string(700, 'a'); disk["/two"] = "b\n";
      failReadPath = "/one"; failReadAt = readAt; failedReadResult = readResult;
      const size_t size = readAt == 1 ? 2 : 515;
      const auto reply = invoke("read-stop", "\"out\" \"/one\" \"/two\"", false);
      check(HAS_STAGED ? reply.rfind("Error:", 0) == 0 : reply == success(2, size), "premature input read cannot publish output");
      check(HAS_STAGED ? !disk.count(outputPath()) : disk.at(outputPath()).size() == size, "read-stop output preservation");
    }
  }
  for (bool laterInput : {false, true}) {
    reset(); disk["/one"] = "first"; disk["/two"] = "next\n"; failSeparator = 1;
    const auto result = invoke("separator-failure", std::string("\"out\" \"/one\"") + (laterInput ? " \"/two\"" : ""), false);
    if (HAS_COPY_HELPER) {
      check(result == writeError(5), "failed separator must report failure without counting byte");
      check(HAS_STAGED ? !disk.count(outputPath()) : disk.at(outputPath()) == "first", "no later data after failed boundary");
      check(!seen("open:r:/two"), "failed separator prevents opening next input");
    } else {
      check(result == success(laterInput ? 2 : 1, laterInput ? 11 : 6), "baseline demonstrates false separator success");
      check(disk.at(outputPath()) == (laterInput ? "firstnext\n" : "first"), "baseline documents merged-line corruption");
    }
    check(where("close:/one") < where("separator"), "input closed before separator");
    check(where("separator") < where("close:" + writePath()), "output closed after separator attempt");
  }
}

static void sealedCases() {
  for (bool first : {false, true}) for (bool second : {false, true}) {
    reset(); disk["/one"] = first ? "#HW1ENC v1\nsealed1\n" : "plain1\n";
    disk["/two"] = second ? "#HW1ENC v1\nsealed2\n" : "plain2\n";
    const auto expected = disk["/one"] + disk["/two"];
    const auto result = invoke("sealed-" + std::to_string(first) + std::to_string(second), "\"out\" \"/one\" \"/two\"");
    if (health && first != second) {
      check(result == "Error: cannot stitch sealed and plaintext captures together — decrypt the sealed input first with capturecrypt export (/two)", "health refuses either direction of sealed mismatch");
      check(HAS_STAGED ? !disk.count(outputPath()) : disk.at(outputPath()) == disk.at("/one"), "mismatch leaves no new output");
    } else {
      check(result == success(2, expected.size()), "matching sealed state succeeds; GPS retains uninspected byte copy");
      check(disk.at(outputPath()) == expected, "sealed probe restores input position");
    }
  }
}

static void helperCases() {
#if HAS_COPY_HELPER
  for (int fault : {0, 1, 2, 3}) {
    reset(); disk["/one"] = fault == 0 ? "" : "prefix" + std::string(700, 'x');
    disk["/output"] = "existing:";
    unsigned long total = 17; uint8_t last = 'z';
    {
      FsLockGuard lock("helper-owner"); File input("/one", "r"), output("/output", "w");
      if (fault) input.seek(6);
      if (fault == 2) { failBody = 2; partialBody = 3; }
      if (fault == 3) { failReadAt = 1; failReadPath = "/one"; failedReadResult = -1; }
      check(copyFileContents(input, output, total, last) == (fault != 2), "helper false only for short write");
      check(bool(input) && bool(output) && liveFiles == 2, "helper leaves both file lifetimes to caller");
      check(separatorWrites == 0, "helper does not append separators");
      const size_t completed = fault == 1 ? 700 : fault == 2 ? 512 : 0;
      check(total == 17 + completed && last == (completed ? 'x' : 'z'), "helper accumulates and preserves last byte on empty/read failure");
      check(disk.at("/output") == "existing:" + std::string(completed + (fault == 2 ? 3 : 0), 'x'), "helper starts at current input/output positions");
      input.close(); output.close();
    }
  }
#endif
}


static void stagedCases() {
#if HAS_STAGED
  for (bool kind : {false, true}) for (bool sd : {false, true}) {
    health = kind;
    const std::string path = sd ? "/sd/logging_captures/result.csv" : outputPath();
    const std::string args = "\"" + path + "\" \"/one\"";
    for (int fault = 0; fault < 9; ++fault) {
      reset(); disk[path] = "prior"; disk["/one"] = "new data\n";
      if (fault == 1) { failBody = 1; partialBody = 2; }
      if (fault == 2) printError = true;
      if (fault == 3) sizeError = true;
      if (fault == 4) renameFailures.insert(sd ? 2 : 1);
      if (fault == 5) disk[path + ".tmp"] = "unowned neighbor";
      if (fault == 6) { failBody = 1; removeFail = true; }
      if (fault == 7) { failReadPath = "/one"; failReadAt = 1; }
      if (fault == 8) directories.insert("/one");
      auto result = invoke("staged-fault", args, false);
      // fault 5: a temporary left by an interrupted run is this command's own
      // leftover; the next run overwrites it and completes.
      const bool failure = fault && fault != 5;
      check(failure ? result.rfind("Error:", 0) == 0 : result == success(1, 9, path), "completion failure reaches command result");
      check(disk.at(path) == (failure ? "prior" : "new data\n"), "existing destination preserved until complete promotion");
      check(disk.at("/one") == "new data\n", "source input unchanged");
      check(disk.count(path + ".tmp") == (fault == 6 ? 1U : 0U), "only a failed-cleanup temp remains");
    }
    for (const auto& suffix : {".tmp", ".bak"}) {
      reset(); disk[path] = "prior"; disk[path + suffix] = "input";
      check(invoke("staging-alias", "\"" + path + "\" \"" + path + suffix + "\"", false).find("staging") != std::string::npos,
            "staging path cannot alias an input");
      check(disk.at(path) == "prior" && disk.at(path + suffix) == "input", "alias refusal preserves both files");
    }
  }
  for (bool sd : {false, true}) for (int fault = 0; fault < 8; ++fault) {
    reset(); ++scenarios; fsEvents.clear();
    const std::string path = sd ? "/sd/logging_captures/result.csv" : "/logging_captures/result.csv";
    const String temp((path + ".tmp").c_str()), dest(path.c_str());
    disk[path] = "prior"; disk[path + ".tmp"] = "complete";
    if (fault == 1) renameFailures.insert(1);
    if (fault == 2) renameFailures.insert(2);
    if (fault == 3) renameFailures = {2, 3};
    if (fault == 4) disk[path + ".bak"] = "unresolved";
    if (fault == 5) removeFail = true;
    if (fault == 6) disk.erase(path);
    if (fault == 7) disk.erase(path + ".tmp");
    auto auth = VFS::systemAuth(VFS::captureScopeFor(dest), "gps.stitch");
    bool ok;
    { FsLockGuard guard("test.owner"); ok = VFS::replaceFileGuarded(temp, dest, auth); check(lockDepth == 1, "nested helper retains caller lock"); }
    const bool expected = fault != 1 && fault != 7 && (!sd || (fault != 2 && fault != 3 && fault != 4));
    check(ok == expected, "backend replacement result");
    if (ok) check(disk.at(path) == "complete", "promoted file has complete bytes");
    else if (sd && fault == 3) check(!disk.count(path) && disk.at(path + ".bak") == "prior", "failed rollback retains recovery copy");
    else check(disk.at(path) == "prior", "failed promotion keeps old destination");
    if (fault == 4) check(disk.at(path + ".bak") == "unresolved", "unknown backup never consumed");
    if (sd && fault == 5) check(disk.at(path + ".bak") == "prior", "backup cleanup failure retains old bytes");
    // A backup that outlives the call is reported once; clean outcomes are silent.
    auto reported = [&](const char* word) {
      return fsEvents.size() == 1 && fsEvents[0].find(word) != std::string::npos && fsEvents[0].find(path + ".bak") != std::string::npos;
    };
    if (sd && fault == 4) check(reported("refused"), "blocking backup is reported");
    else if (sd && fault == 3) check(reported("could not be restored"), "unrestored backup is reported");
    else if (sd && fault == 5) check(reported("could not be removed"), "retained backup is reported");
    else check(fsEvents.empty(), "clean replacement outcomes log nothing");
  }

  for (bool kind : {false, true}) for (const auto& suffix : {"", ".tmp", ".bak"}) {
    reset(); health = kind;
    const auto output = outputPath();
    disk[output] = "prior"; disk[output + suffix] = "input";
    const auto before = disk;
    const auto split = output.rfind('/');
    const auto alias = output.substr(0, split) + "/./" + output.substr(split) + suffix;
    const std::string expected = std::string(suffix).empty()
        ? "Error: an input file is the same as the output"
        : "Error: input conflicts with output staging paths";
    check(invoke("normalized-alias", "\"" + output + "\" \"" + alias + "\"", false) == expected,
          "normalized input alias refused explicitly, not by occupied-temp fallback");
    check(disk == before, "normalized alias leaves files unchanged");
    check(std::none_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("open:w:", 0) == 0; }),
          "normalized alias rejected before any output write-open");
  }
  reset(); ++scenarios;
  {
    const String dest("/sd/logging_captures/directory.log"), temp("/sd/logging_captures/directory.log.tmp");
    disk[text(dest)] = "directory sentinel"; directories.insert(text(dest)); disk[text(temp)] = "new";
    auto auth = VFS::systemAuth(VFS::Scopes::CAPTURES_SD, "gps.stitch");
    FsLockGuard guard("directory");
    check(!VFS::replaceFileGuarded(temp, dest, auth) && renameCalls == 0, "SD directory is refused before any rename");
    check(directories.count(text(dest)) && disk.at(text(dest)) == "directory sentinel", "directory retained at its original path");
  }
  for (const auto& destination : {"/sd/logging_captures/to", "/logging_captures/from", "/logging_captures/../to"}) {
    reset(); ++scenarios; disk["/logging_captures/from"] = "input";
    auto auth = VFS::systemAuth(VFS::Scopes::CAPTURES, "gps.stitch");
    { FsLockGuard guard("invalid"); check(!VFS::replaceFileGuarded("/logging_captures/from", destination, auth), "cross-backend/self/traversal replacement refused"); }
    check(renameCalls == 0 && disk.at("/logging_captures/from") == "input", "invalid replacement has no effects");
  }
#endif
}

static void exportCases() {
  for (bool sd : {false, true}) for (int fault = 0; fault < (HAS_STAGED ? 10 : 1); ++fault) {
    reset(); ++scenarios;
    const std::string path = sd ? "/sd/logging_captures/plain.txt" : "/logging_captures/plain.txt";
    disk["/sealed"] = "#HW1ENC v1\n# columns\ncipher:good\ncipher:bad\n";
    disk[path] = "prior";
    if (fault == 1) { failBody = 1; partialBody = 2; }
    if (fault == 2) failSeparator = 1;
    if (fault == 3) printError = true;
    if (fault == 4) sizeError = true;
    if (fault == 5) renameFailures.insert(sd ? 2 : 1);
    if (fault == 6) disk[path + ".tmp"] = "unowned";
    if (fault == 7) failLineRead = true;
    if (fault == 8) { failBody = 1; removeFail = true; }
    if (fault == 9) disk["/sealed"] = "not sealed\n";
    const std::string result = cmd_capturecrypt(String(("export \"/sealed\" \"" + path + "\"").c_str()));
    check(lockDepth == 0 && liveFiles == 0, "export releases all handles and lock");
    // fault 6: a temporary left by an interrupted export is overwritten by the next run.
    const bool failure = fault && !(HAS_STAGED && fault == 6);
    check(failure ? result.rfind("Error:", 0) == 0 : result.rfind("SUCCESS:", 0) == 0, "export reports completion honestly");
    check(disk.at(path) == (failure ? "prior" : "# columns\ndecoded\n[UNDECRYPTABLE]\n"), "export preserves old destination on failure and plaintext schema on success");
    if (!failure) {
      check(result.find("1 rows decrypted, 1 undecryptable") != std::string::npos, "export retains partial decryption policy");
      if (!fault) std::cout << "export:" << path << ' ' << std::quoted(result) << '\n' << std::quoted(disk.at(path)) << '\n';
    }
    if (HAS_STAGED) check(disk.count(path + ".tmp") == (fault == 8 ? 1U : 0U), "export cleanup only leaves a failed-cleanup temp");
  }
#if HAS_STAGED
  for (const auto& separator : {"/", "/./", "//"}) for (const auto& suffix : {"", ".tmp", ".bak"}) {
    reset(); ++scenarios;
    const std::string out = "/logging_captures/plain.txt", in = out + suffix;
    disk[in] = "#HW1ENC v1\ncipher:good\n";
    const auto before = disk;
    const auto split = out.rfind('/');
    const auto alias = out.substr(0, split) + separator + out.substr(split + 1) + suffix;
    const std::string result = cmd_capturecrypt(String(("export \"" + alias + "\" \"" + out + "\"").c_str()));
    const std::string expected = std::string(suffix).empty()
        ? "Error: output is the same as the input"
        : "Error: input conflicts with output staging paths";
    check(result == expected, "export alias refused explicitly after normalization");
    check(disk == before, "export alias refusal preserves every file");
    check(std::none_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("open:w:", 0) == 0; }),
          "export alias rejected before any output write-open");
    check(lockDepth == 0 && liveFiles == 0, "export alias refusal releases resources");
  }
#endif
}

int main() {
  for (bool kind : {false, true}) {
    health = kind;
    normalCases(); errorCases(); failureCases(); sealedCases();
  }
  stagedCases(); exportCases();
  const size_t handlerChecks = checks;
  helperCases();
  std::cerr << scenarios << " handler scenarios; " << handlerChecks << " handler assertions; "
            << (checks - handlerChecks) << " helper assertions; separator regression "
            << (HAS_COPY_HELPER ? "fixed" : "reproduced") << '\n';
}
