// Runs the file permission path as shipped: the path-rule table, lookupRule,
// the sensitive-extension and image tests, pathWithinScope, resolveRole,
// checkPerm with the six canX queries, logFsAccessDeny, getPermissions and
// isAdminOnlyPath (System_Filesystem.cpp), and VFS::systemAuth
// (System_VFS.cpp), on the real Arduino String with its heap calls counted.
// The roster, the FS lock and the debug queue are doubles. The runner pastes
// the production text at the markers.
//
// It prints one line per path: the matched rule, the two filename tests,
// isAdminOnlyPath and, per identity, the six canX answers, getPermissions and
// the reason logFsAccessDeny names for each refusal. The runner compares that
// report byte for byte with fs_permission_answers.txt (the answers of the
// tree before these functions stopped copying) on stdout; the String block
// counts go to stderr and are checked here: a permission query makes none.
#include <Arduino.h>
#include "string_heap_count.h"

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
  std::fprintf(stderr, "fs permission strings check failed at line %d: %s\n", line, text);
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

// Roster: name, stored role, id. Role texts fit a String inline, so the
// double itself makes no block.
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

// Debug queue: the [PERM] lines logFsAccessDeny writes.
struct { bool storagePermissions = true; } gDebugSubFlags;
static std::vector<std::string> sDenyLines;
#define DEBUG_STORAGEF(fmt, ...)                                   \
  do {                                                             \
    char line_[512];                                               \
    std::snprintf(line_, sizeof(line_), fmt, ##__VA_ARGS__);       \
    sDenyLines.push_back(line_);                                   \
  } while (0)

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------

// A path as the report shows it: NUL as \0, a backslash doubled, and a '/'
// right after "Users" as \/, because the tracked tree holds no text shaped
// like a home directory path (test_tree_hygiene_gate, the commit guard) and
// the table has a "/Users" rule.
static std::string shown(const String& s) {
  std::string out;
  const char* p = s.c_str();
  for (size_t i = 0; p && i < s.length(); i++) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    if (c == 0) out += "\\0";
    else if (c == '\\') out += "\\\\";
    else if (c == '/' && out.size() >= 5 && out.compare(out.size() - 5, 5, "Users") == 0) out += "\\/";
    else out += char(c);
  }
  return out;
}

static String withNul(const char* text, size_t length) { return String(text, length); }

static std::vector<String> pathSet() {
  std::vector<String> paths;
  static const char* const kTails[] = {"x", "/x", "/../secret.txt", "/a.KEY", "/IMG.Jpeg",
                                       "/My-Password.txt", "/fw.bin", "/notes.txt"};
  for (const PathRule& rule : sPathRules) {
    if (!rule.path) continue;
    const String p(rule.path);
    paths.push_back(p);
    for (const char* tail : kTails) paths.push_back(p + tail);
    // A trailing slash added or taken away.
    if (p.endsWith("/")) paths.push_back(p.substring(0, p.length() - 1));
    else paths.push_back(p + "/");
    // A case difference.
    String upper = p;
    upper.toUpperCase();
    paths.push_back(upper);
    paths.push_back(upper + "/x");
  }
  static const char* const kExtras[] = {
    "", "/", "//", "..", "/..", "/./x", "x", "system/settings.json", " /system/settings.json",
    "/x.key", "/X/Y.PEM", "/a.Cert", "/a.cer", "/b.credentials", "/c.HEX", "/d.elf", "/e.crt",
    "/dir/secret/file.txt", "/dir/Credentials", "/dir/my-SECRET-notes.txt", "/passwordless/x.txt",
    "/photo.webp.txt", "/a.heif", "/a.AVIF", "/a.ico", "/a.bmp", "/a.gif", "/a.png", "/a.jpg",
    ".key", "key", "/.jpg", "/x/", "/stt", "/stt/u2", "/stt/u2/a.txt", "/stt/u3/a.txt",
    "/sd/stt/u2/a.txt", "/sd/STT/u2", "/sd/VIDEOS", "/sd/VIDEOS/clip.mp4", "/sd/VIDEOS_secret/x",
  };
  for (const char* extra : kExtras) paths.push_back(String(extra));
  // Paths at the length limits: FILE_MANAGER_MAX_PATH (128 with its NUL) and
  // the LittleFS name limit (255).
  for (size_t length : {size_t(127), size_t(255)}) {
    std::string text = "/system/sys_logs/";
    while (text.size() < length) text += 'a';
    paths.push_back(String(text.c_str()));
    text.replace(length - 4, 4, ".BIN");
    paths.push_back(String(text.c_str()));
  }
  // Embedded NULs: String::toLowerCase stops at the first one.
  paths.push_back(withNul("/a\0B.KEY", 8));
  paths.push_back(withNul("/a\0b.key", 8));
  paths.push_back(withNul("/a.k\0ey", 7));
  paths.push_back(withNul("/pass\0word", 10));
  paths.push_back(withNul("/x/PASSWORD\0/y", 14));
  paths.push_back(withNul("/system/certs/\0x", 16));
  paths.push_back(withNul("/IMG\0.JPG", 9));
  return paths;
}

struct Identity { const char* name; AuthContext ctx; };

static AuthContext named(CommandSource source, const char* user, const char* scope = nullptr) {
  AuthContext ctx{};
  ctx.transport = source;
  ctx.user = user;
  if (scope) ctx.scope = scope;
  return ctx;
}

static std::string denyCode(const String& path, const AuthContext& ctx, uint8_t needed) {
  sDenyLines.clear();
  logFsAccessDeny(path, ctx, needed, "op");
  check(sDenyLines.size() == 1);
  const std::string& line = sDenyLines[0];
  if (line.find("role=anon") != std::string::npos) return "a";
  if (line.find("reason=sensitive-extension") != std::string::npos) return "s";
  if (line.find("reason=image-not-editable") != std::string::npos) return "i";
  const size_t at = line.find("granted=0x");
  check(at != std::string::npos);
  return "g" + line.substr(at + 10, 2);
}

static unsigned long sAuthBlocks[3];

int main() {
  // --- VFS::systemAuth: the context, and the blocks it makes. ---
  {
    gStringHeap = StringHeapCount{};
    const AuthContext a = VFS::systemAuth("sensorlog.rotate.data");
    const unsigned long plain = gStringHeap.made;
    gStringHeap = StringHeapCount{};
    const AuthContext b = VFS::systemAuth("/logging_captures/sensors", "sensorlog.rotate.data");
    const unsigned long scoped = gStringHeap.made;
    gStringHeap = StringHeapCount{};
    const AuthContext c = VFS::systemAuth(nullptr);
    const AuthContext d = VFS::systemAuth("");
    const AuthContext e = VFS::systemAuth(nullptr, "unit");
    const AuthContext f = VFS::systemAuth("/sd/x", nullptr);
    const unsigned long shortOnes = gStringHeap.made;
    std::fprintf(stderr, "systemAuth: %lu String blocks (reason), %lu (scope and reason), %lu (short texts)\n",
                plain, scoped, shortOnes);
    for (const AuthContext* ctx : {&a, &b, &c, &d, &e, &f})
      std::printf("systemAuth ctx: transport=%d user='%s' path='%s' ip='%s' sid='%s' opaque=%p scope='%s'\n",
                  int(ctx->transport), shown(ctx->user).c_str(), shown(ctx->path).c_str(),
                  shown(ctx->ip).c_str(), shown(ctx->sid).c_str(), ctx->opaque, shown(ctx->scope).c_str());
    check(isSystemIdentity(a) && isSystemIdentity(b));
    sAuthBlocks[0] = plain;
    sAuthBlocks[1] = scoped;
    sAuthBlocks[2] = shortOnes;
  }

  std::vector<Identity> identities = {
    {"anon", named(SOURCE_WEB, "")},
    {"guest", named(SOURCE_WEB, "guest")},
    {"user", named(SOURCE_WEB, "alice")},
    {"admin", named(SOURCE_WEB, "admin")},
    {"super", named(SOURCE_WEB, "super")},
    {"bypass", named(SOURCE_SERIAL, "AuthBypass")},
    {"unknown", named(SOURCE_WEB, "mallory")},
    {"system", VFS::systemAuth("unit")},
    {"sys@videos", VFS::systemAuth("/sd/VIDEOS/", "unit")},
    {"sys@root", VFS::systemAuth("/", "unit")},
    {"user@stt", named(SOURCE_WEB, "alice", "/stt/u2")},
  };

  const std::vector<String> paths = pathSet();
  unsigned long queryBlocks = 0;
  for (const String& path : paths) {
    gStringHeap = StringHeapCount{};
    const PathRule& rule = lookupRule(path);
    const bool sensitive = hasSensitiveExtension(path);
    const bool image = isImageFile(path);
    const bool adminOnly = isAdminOnlyPath(path);
    queryBlocks += gStringHeap.made;
    std::printf("%s\trule=%d sens=%d img=%d adminOnly=%d", shown(path).c_str(),
                int(&rule - sPathRules), int(sensitive), int(image), int(adminOnly));
    for (const Identity& id : identities) {
      gStringHeap = StringHeapCount{};
      const bool can[6] = {canRead(path, id.ctx), canEdit(path, id.ctx), canDelete(path, id.ctx),
                           canRename(path, id.ctx), canCreate(path, id.ctx), canImport(path, id.ctx)};
      queryBlocks += gStringHeap.made;
      static const uint8_t kNeeded[6] = {PERM_READ, PERM_WRITE, PERM_DELETE, PERM_RENAME,
                                         PERM_CREATE, PERM_IMPORT};
      std::string bits, denied;
      for (int i = 0; i < 6; i++) {
        bits += can[i] ? "RWDNCI"[i] : '-';
        if (!can[i]) denied += denyCode(path, id.ctx, kNeeded[i]);
      }
      std::printf(" | %s %s %02X %s", id.name, bits.c_str(), unsigned(getPermissions(path, id.ctx)),
                  denied.empty() ? "." : denied.c_str());
    }
    std::printf("\n");
  }

  // pathWithinScope on its own, with the boundary cases.
  static const char* const kScopes[] = {"", "/", "//", "/sd/VIDEOS", "/sd/VIDEOS/", "/sd/VIDEOS//",
                                        "/stt/u2", "/logging_captures/sensors"};
  static const char* const kScoped[] = {"", "/", "/sd", "/sd/VIDEOS", "/sd/VIDEOS/", "/sd/VIDEOS/a",
                                        "/sd/VIDEOS_secret", "/sd/videos/a", "/stt/u2", "/stt/u20",
                                        "/stt/u2/n.txt", "/logging_captures/sensors/a.csv",
                                        "/logging_captures/sensorsX"};
  std::vector<String> scopes;
  for (const char* s : kScopes) scopes.push_back(String(s));
  scopes.push_back(withNul("/sd\0x", 5));
  std::vector<String> scoped;
  for (const char* s : kScoped) scoped.push_back(String(s));
  scoped.push_back(withNul("/sd\0x", 5));
  scoped.push_back(withNul("/sd\0y/a", 7));
  scoped.push_back(withNul("/sd/VIDEOS\0/a", 13));
  scoped.push_back(withNul("/sd\0zzz", 7));
  for (const String& scope : scopes) {
    std::printf("scope '%s':", shown(scope).c_str());
    for (const String& path : scoped) {
      gStringHeap = StringHeapCount{};
      const bool within = pathWithinScope(path, scope);
      queryBlocks += gStringHeap.made;
      std::printf(" %d", int(within));
    }
    std::printf("\n");
  }

  // The block checks come after the report, so an older tree still prints
  // its answers. A permission query makes no String.
  std::fprintf(stderr, "permission queries: %lu String blocks over %zu paths\n", queryBlocks, paths.size());
  check(queryBlocks == 0);
  std::fprintf(stderr, "fs permission strings passed (%u checks)\n", checks);
  return 0;
}
