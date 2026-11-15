// Runs the guarded file path as shipped: normalizeFsPath and the permission
// path (System_Filesystem.cpp), and VFS::normalize, getStorageType, the
// backend dispatchers (exists, open, mkdir, remove, rename, rmdir) and the
// guarded calls in front of them (System_VFS.cpp), on the real Arduino String
// with its heap calls counted. LittleFS, SD, the roster, the FS lock, the
// event post and the debug queue are doubles; the backends record every path
// they are handed. The runner pastes the production text at the markers.
//
// stdout is the report: for each path of a generated corpus, normalizeFsPath
// (answer and text), normalize, getStorageType, and for six identities each
// guarded call with its answer and the backend calls it made, path for path.
// The runner compares it with the report of the tree before the guarded calls
// stopped normalizing a second and a third time (as a digest, and line by line
// against a saved tree with --baseline). stderr carries the String block
// counts per call, checked after the report.
#include <Arduino.h>
#include "string_heap_count.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define ENABLE_BONDED_MODE 1

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "guarded path check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// INSERT_POLICY

enum CommandSource { SOURCE_INTERNAL, SOURCE_SERIAL, SOURCE_WEB, SOURCE_ESPNOW };

// INSERT_IDENTITY

// FS lock: one task, never contended.
using TaskHandle_t = void*;
static int sTaskObject;
static TaskHandle_t sTask = &sTaskObject, sFsOwner = nullptr;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return sTask; }
static bool isFsLockedByCurrentTask() { return sFsOwner == sTask; }
struct FsLockGuard {
  bool held = false;
  explicit FsLockGuard(const char*) {
    if (isFsLockedByCurrentTask()) return;
    sFsOwner = sTask;
    held = true;
  }
  ~FsLockGuard() { if (held) sFsOwner = nullptr; }
};

// Roster: name, stored role, id (texts fit a String inline).
struct Account { const char* name; const char* role; uint32_t id; };
static const Account kAccounts[] = {
  {"guest", "guest", 5}, {"alice", "user", 2}, {"admin", "admin", 4}, {"super", "superadmin", 1},
};
static const Account* findAccount(const String& user) {
  for (const Account& a : kAccounts)
    if (user.c_str() && std::strcmp(user.c_str(), a.name) == 0) return &a;
  return nullptr;
}
static bool getUserRoleAndSuper(const String& user, String& role, bool& isSuper) {
  const Account* a = findAccount(user);
  if (!a) return false;
  role = a->role;
  isSuper = std::strcmp(a->role, "superadmin") == 0;
  return true;
}
static bool getUserIdByUsername(const String& user, uint32_t& id) {
  const Account* a = findAccount(user);
  id = a ? a->id : 0;
  return a != nullptr;
}
static bool isSuperAdminUser(const String&) { return false; }

// Debug queue: the [PERM] lines. Only counted; their text is the permission
// test's subject (fs_permission_strings).
struct { bool storagePermissions = true; } gDebugSubFlags;
static unsigned long sDebugLines = 0;
#define DEBUG_STORAGEF(fmt, ...)                                   \
  do {                                                             \
    char line_[512];                                               \
    std::snprintf(line_, sizeof(line_), fmt, ##__VA_ARGS__);       \
    ++sDebugLines;                                                 \
  } while (0)

// The backends: every call is recorded with the path it was handed. A path
// "exists" when its length is even, so write opens take both permission
// branches of writePermitted across the corpus.
static std::string sCalls;
static unsigned long sBackendCalls = 0;
static bool sdAvailable = true;
static bool filesystemReady = true;
// A path as the report shows it: bytes outside printable ASCII and the
// backslash as \xNN, and a '/' right after "Users" as \/, because the tracked
// tree holds no text shaped like a home directory path (test_tree_hygiene_gate,
// the commit guard) and the path table has a "/Users" rule.
static std::string escaped(const char* p, size_t n) {
  std::string out;
  for (size_t i = 0; p && i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    if (c < 0x20 || c >= 0x7F || c == '\\') {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "\\x%02X", c);
      out += hex;
    } else if (c == '/' && out.size() >= 5 && out.compare(out.size() - 5, 5, "Users") == 0) {
      out += "\\/";
    } else {
      out += char(c);
    }
  }
  return out;
}
static void note(const char* backend, const char* op, const char* path, const char* extra = "") {
  ++sBackendCalls;
  sCalls += ' ';
  sCalls += backend;
  sCalls += '.';
  sCalls += op;
  sCalls += '(';
  sCalls += escaped(path, std::strlen(path));
  sCalls += extra;
  sCalls += ')';
}
static bool present(const char* path) { return (std::strlen(path) % 2) == 0; }

#define FILE_READ "r"
struct File {
  bool open = false;
  explicit operator bool() const { return open; }
};
struct LittleFsDouble {
  File open(const char* path, const char* mode, bool create) {
    note("lfs", "open", path, create ? (mode[0] == 'r' ? ",r,1" : ",w,1") : (mode[0] == 'r' ? ",r,0" : ",w,0"));
    File f;
    f.open = mode[0] == 'r' ? present(path) : (create || present(path));
    return f;
  }
  bool exists(const String& path) { note("lfs", "exists", path.c_str()); return present(path.c_str()); }
  bool mkdir(const String& path) { note("lfs", "mkdir", path.c_str()); return true; }
  bool remove(const String& path) { note("lfs", "remove", path.c_str()); return present(path.c_str()); }
  bool rmdir(const String& path) { note("lfs", "rmdir", path.c_str()); return present(path.c_str()); }
  bool rename(const String& from, const String& to) {
    note("lfs", "rename", from.c_str());
    note("lfs", "to", to.c_str());
    return present(from.c_str());
  }
} LittleFS;
struct SdDouble {
  File open(const char* path, const char* mode, bool create) {
    note("sd", "open", path, create ? (mode[0] == 'r' ? ",r,1" : ",w,1") : (mode[0] == 'r' ? ",r,0" : ",w,0"));
    File f;
    f.open = mode[0] == 'r' ? present(path) : (create || present(path));
    return f;
  }
  bool exists(const char* path) { note("sd", "exists", path); return present(path); }
  bool mkdir(const char* path) { note("sd", "mkdir", path); return true; }
  bool remove(const char* path) { note("sd", "remove", path); return present(path); }
  bool rmdir(const char* path) { note("sd", "rmdir", path); return present(path); }
  bool rename(const char* from, const char* to) {
    note("sd", "rename", from);
    note("sd", "to", to);
    return present(from);
  }
} SD;
static void invalidateLittleFsFreeCache() {}
enum { SYSEVT_FILE_DELETED = 1 };
static unsigned long sDeletedEvents = 0;
static void systemEventPost(int, const char*, const char*) { ++sDeletedEvents; }

namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static bool isSdAvailable() { return sdAvailable; }
String normalize(const String& path);
StorageType getStorageType(const String& path);
File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create = false);
}

// INSERT_PRODUCTION

namespace VFS {
// INSERT_VFS
}  // namespace VFS

// ---------------------------------------------------------------------------

static std::string shown(const String& s) { return escaped(s.c_str(), s.length()); }

// The corpus: every rule prefix with tails, slashes, dots, case changes;
// token sequences of up to three pieces; every byte 0x80-0xFF; paths at the
// length limits (128 with its NUL for the file manager and the log path
// buffer, 255 for a LittleFS name) and one past; embedded NULs.
static std::vector<String> corpus() {
  std::vector<String> paths;
  auto add = [&](const std::string& text) { paths.push_back(String(text.c_str(), text.size())); };
  static const char* const kTails[] = {"", "/", "//", "x", "/x", "/x/", "/./x", "/.", "/..", "/../x",
                                       "/a b", "/a /", "/ a", " ", "\t", "/x\t", "/x ", "/.x", "/x.",
                                       "/errors.log", "/events.log.tmp"};
  for (const PathRule& rule : sPathRules) {
    if (!rule.path) continue;
    std::string p = rule.path;
    for (const char* tail : kTails) add(p + tail);
    std::string upper = p;
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    add(upper);
    add(upper + "/x");
    if (p.size() > 1 && p.back() == '/') add(p.substr(0, p.size() - 1));
    add(" " + p);
    add("\t" + p + " ");
    add(p.substr(1));  // no leading slash
    add("/" + p);
  }
  static const char* const kTokens[] = {"/", "//", ".", "..", "a", "sd", "SD", "Sd", " ", "\t",
                                        "x y", "\xA0", "\xFF", "system", "sys_logs", "errors.log"};
  const size_t nTokens = sizeof(kTokens) / sizeof(kTokens[0]);
  for (size_t a = 0; a < nTokens; a++) {
    add(kTokens[a]);
    for (size_t b = 0; b < nTokens; b++) {
      add(std::string(kTokens[a]) + kTokens[b]);
      for (size_t c = 0; c < nTokens; c++) add(std::string(kTokens[a]) + kTokens[b] + kTokens[c]);
    }
  }
  for (const char* prefix : {"", "/", "/sd", "/sd/", "/SD/", "/sd//", "//sd/", " /sd/", "/sd/.",
                             "/system/sys_logs/", "/sd/system/sys_logs/", "sd/"}) {
    for (const char* rest : {"", "/", "x", "a/b", "a//b/", "./a", "a/./b", ".a", "a.", " a ", "\ta"})
      add(std::string(prefix) + rest);
  }
  for (int byte = 0x80; byte <= 0xFF; byte++) {
    const char c = char(byte);
    add(std::string("/a") + c);
    add(std::string("/a") + c + "/");
    add(std::string("/sd/") + c);
    add(std::string(1, c) + "/x");
  }
  for (size_t length : {size_t(1), size_t(13), size_t(14), size_t(18), size_t(19), size_t(126),
                        size_t(127), size_t(128), size_t(129), size_t(254), size_t(255), size_t(256),
                        size_t(257)}) {
    for (const char* stem : {"/system/sys_logs/", "/sd/", "/"}) {
      std::string text = stem;
      while (text.size() < length) text += 'a';
      text.resize(length);
      add(text);
      add(text + "/");
      add(" " + text);
    }
  }
  add(std::string("/a\0b", 4));
  add(std::string("/sd\0/x", 6));
  add(std::string("/x/\0", 4));
  add(std::string("\0/x", 3));
  return paths;
}

struct Identity { const char* name; AuthContext ctx; };

static AuthContext named(CommandSource source, const char* user) {
  AuthContext ctx{};
  ctx.transport = source;
  ctx.user = user;
  return ctx;
}

static std::string sLine;
template <typename Call>
static void op(const char* name, Call call) {
  sCalls.clear();
  const bool result = call();
  sLine += ' ';
  sLine += name;
  sLine += '=';
  sLine += result ? '1' : '0';
  sLine += sCalls.empty() ? std::string(" -") : sCalls;
  sLine += ';';
}

// Blocks one call makes, on a path given as a ready String.
template <typename Call>
static unsigned long blocks(Call call) {
  gStringHeap = StringHeapCount{};
  call();
  return gStringHeap.made;
}

int main() {
  std::vector<Identity> identities = {
    {"anon", named(SOURCE_WEB, "")},
    {"user", named(SOURCE_WEB, "alice")},
    {"admin", named(SOURCE_WEB, "admin")},
    {"system", VFS::systemAuth("unit")},
    {"sys@logs", VFS::systemAuth("/system/sys_logs", "unit")},
    {"sys@sdlogs", VFS::systemAuth("/sd/system/sys_logs", "unit")},
  };

  const std::vector<String> paths = corpus();
  unsigned long normalizedPaths = 0, mismatches = 0;
  for (const String& path : paths) {
    String nfs;
    const bool ok = normalizeFsPath(path, nfs);
    sLine = shown(path);
    sLine += "\tnfs=";
    sLine += ok ? "1:" + shown(nfs) : std::string("0");
    sLine += " norm=" + shown(VFS::normalize(path));
    sLine += " type=" + std::to_string(int(VFS::getStorageType(path)));
    for (const Identity& id : identities) {
      sLine += " | ";
      sLine += id.name;
      const AuthContext& ctx = id.ctx;
      op("r", [&] { return bool(VFS::openGuarded(path, "r", ctx)); });
      op("a", [&] { return bool(VFS::openGuarded(path, "a", ctx, true)); });
      op("w", [&] { return bool(VFS::openGuarded(path, "w", ctx)); });
      op("cw", [&] { return VFS::canWriteGuarded(path, ctx); });
      op("ex", [&] { return VFS::existsGuarded(path, ctx); });
      op("rm", [&] { return VFS::removeGuarded(path, ctx); });
      op("md", [&] { return VFS::mkdirGuarded(path, ctx); });
      op("rd", [&] { return VFS::rmdirGuarded(path, ctx); });
      op("mv", [&] { return VFS::renameGuarded(path, String("/system/sys_logs/moved.log"), ctx); });
      op("mv2", [&] { return VFS::renameGuarded(String("/system/sys_logs/from.log"), path, ctx); });
    }
    std::printf("%s\n", sLine.c_str());

    // The path a guarded open hands its backend is normalize(normalizeFsPath),
    // as it always was: the in-binary oracle beside the report comparison.
    if (ok) {
      ++normalizedPaths;
      sCalls.clear();
      (void)VFS::openGuarded(path, "r", identities[3].ctx);
      const String expected = VFS::normalize(nfs);
      const bool sd = VFS::getStorageType(expected) == VFS::SDCARD;
      std::string want;
      if (expected.indexOf("..") >= 0) {
        want = "";
      } else {
        const char* backendPath = sd ? (expected == "/sd" ? "/" : expected.c_str() + 3) : expected.c_str();
        want = std::string(sd ? " sd" : " lfs") + ".open(" + escaped(backendPath, std::strlen(backendPath)) + ",r,0)";
      }
      if (want != sCalls) {
        ++mismatches;
        std::fprintf(stderr, "backend path differs for '%s': want '%s' got '%s'\n",
                     shown(path).c_str(), want.c_str(), sCalls.c_str());
      }
    }
  }
  std::fprintf(stderr, "%zu paths (%lu normalized), %lu backend calls, %lu [PERM] lines, %lu delete events\n",
               paths.size(), normalizedPaths, sBackendCalls, sDebugLines, sDeletedEvents);

  // --- String blocks per call (the device holds 13 characters inline, the
  // host more; every path here is long enough to need a block on both). ---
  const AuthContext& logs = identities[4].ctx;
  const AuthContext& sys = identities[3].ctx;
  const String events("/system/sys_logs/events.log");
  const String sdEvents("/sd/system/sys_logs/events.log");
  const String sensor("/logging_captures/sensors/sensors_2026-11-15.csv");
  const String messy(" /system//sys_logs/./events.log/");
  const String spaced("/system/sys_logs/abcdef /");
  const unsigned long nfsBlocks = blocks([&] { String out; (void)normalizeFsPath(events, out); });
  const unsigned long nfsMessy = blocks([&] { String out; (void)normalizeFsPath(messy, out); });
  const unsigned long typeNormal = blocks([&] { (void)VFS::getStorageType(events); });
  const unsigned long typeSd = blocks([&] { (void)VFS::getStorageType(sdEvents); });
  const unsigned long typeMessy = blocks([&] { (void)VFS::getStorageType(messy); });
  const unsigned long append = blocks([&] { (void)VFS::openGuarded(events, "a", logs, true); });
  const unsigned long appendSd = blocks([&] { (void)VFS::openGuarded(sdEvents, "a", identities[5].ctx, true); });
  const unsigned long read = blocks([&] { (void)VFS::openGuarded(sensor, "r", sys); });
  const unsigned long exists = blocks([&] { (void)VFS::existsGuarded(sensor, sys); });
  const unsigned long remove = blocks([&] { (void)VFS::removeGuarded(sensor, sys); });
  const unsigned long appendMessy = blocks([&] { (void)VFS::openGuarded(messy, "a", logs, true); });
  const unsigned long appendSpaced = blocks([&] { (void)VFS::openGuarded(spaced, "a", logs, true); });
  std::fprintf(stderr,
               "String blocks per call: normalizeFsPath %lu (messy %lu); getStorageType %lu (sd %lu, messy %lu); "
               "openGuarded append %lu (sd %lu, messy %lu, trailing-space segment %lu), read %lu; "
               "existsGuarded %lu; removeGuarded %lu\n",
               nfsBlocks, nfsMessy, typeNormal, typeSd, typeMessy, append, appendSd, appendMessy, appendSpaced,
               read, exists, remove);

  // The checks come after the report, so an older tree still prints it.
  check(mismatches == 0);
  check(nfsBlocks == 1 && nfsMessy == 1);          // the result only, no trimmed copy
  check(typeNormal == 0 && typeSd == 0);            // a normalized path is read in place
  check(typeMessy == 1);                            // anything else is normalized as before
  check(append == 1 && appendSd == 1 && read == 1 && exists == 1 && remove == 1);
  check(appendMessy == 1);                          // normalizeFsPath's text is normal
  check(appendSpaced == 2);                         // "/abcdef " is normalized again by open
  std::fprintf(stderr, "guarded path normalize passed (%u checks)\n", checks);
  return 0;
}
