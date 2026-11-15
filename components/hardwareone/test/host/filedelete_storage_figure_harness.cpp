// Executes the production doFiledelete (System_Filesystem.cpp), the routine
// behind `filedelete` from the web Files page, glasses, OLED, Bluetooth and
// the console. Status polls read the internal capacity figure at any age, so a
// LittleFS delete must mark that figure stale once, after the file is gone; an
// SD delete, a refused delete and a missing file must leave it alone.
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  const char* c_str() const { return value_.c_str(); }
  bool startsWith(const char* prefix) const { return value_.rfind(prefix, 0) == 0; }
  bool operator==(const String& other) const { return value_ == other.value_; }
 private:
  std::string value_;
};
struct AuthContext { String user; };
#define EXT_RAM_BSS_ATTR
#define ENABLE_MAPS 0
static bool filesystemReady = true;
static std::set<std::string> files;
static bool removeRefused = false;
static unsigned removes = 0, removesAtInvalidation = 0;
static std::vector<int> invalidations;
namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return path.startsWith("/sd/") ? SDCARD : INTERNAL; }
static bool existsGuarded(const String& path, const AuthContext&) { return files.count(path.c_str()) > 0; }
static bool removeGuarded(const String& path, const AuthContext&) {
  if (removeRefused) return false;
  ++removes; return files.erase(path.c_str()) == 1;
}
static void invalidateStatsSnapshot(StorageType type) { invalidations.push_back(int(type)); removesAtInvalidation = removes; }
}  // namespace VFS
// INSERT_DO_FILEDELETE

static void reset() {
  filesystemReady = true; files = {"/user/a.txt", "/sd/b.txt"}; removeRefused = false;
  removes = removesAtInvalidation = 0; invalidations.clear();
}
int main() {
  unsigned passed = 0, failed = 0;
  const AuthContext alice{"alice"};
  auto run = [&](const char* name, void (*test)(const AuthContext&), const AuthContext& ctx) {
    reset();
    try { test(ctx); ++passed; std::printf("PASS %s\n", name); }
    catch (const std::exception& e) { ++failed; std::printf("FAIL %s: %s\n", name, e.what()); }
  };
  run("LittleFS delete marks the internal figure stale once, after the remove", [](const AuthContext& ctx) {
    check(std::strcmp(doFiledelete("/user/a.txt", ctx), "Deleted file: /user/a.txt") == 0, "delete reply");
    check(!files.count("/user/a.txt"), "file removed");
    check(invalidations == std::vector<int>{VFS::INTERNAL} && removesAtInvalidation == 1, "one INTERNAL mark after the remove");
  }, alice);
  run("SD delete leaves the internal figure", [](const AuthContext& ctx) {
    check(std::strcmp(doFiledelete("/sd/b.txt", ctx), "Deleted file: /sd/b.txt") == 0, "delete reply");
    check(invalidations.empty(), "no mark for an SD delete");
  }, alice);
  run("refused delete leaves the figure", [](const AuthContext& ctx) {
    removeRefused = true;
    check(std::strncmp(doFiledelete("/user/a.txt", ctx), "Error: Failed to delete file", 28) == 0, "error reply");
    check(files.count("/user/a.txt") && invalidations.empty(), "nothing deleted, no mark");
  }, alice);
  run("missing file leaves the figure", [](const AuthContext& ctx) {
    check(std::strncmp(doFiledelete("/user/gone.txt", ctx), "Error: File no longer exists", 28) == 0, "missing reply");
    check(invalidations.empty() && removes == 0, "no remove, no mark");
  }, alice);
  run("filesystem not ready leaves the figure", [](const AuthContext& ctx) {
    filesystemReady = false;
    check(std::strcmp(doFiledelete("/user/a.txt", ctx), "Error: LittleFS not ready") == 0, "not-ready reply");
    check(invalidations.empty() && removes == 0, "no remove, no mark");
  }, alice);
  std::printf("%u filedelete storage-figure scenarios passed; %u failed\n", passed, failed);
  return failed ? 1 : 0;
}
