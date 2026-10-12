#include <WString.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static unsigned checks = 0, scenarios = 0;
static void check(bool ok, const std::string& label) {
  ++checks;
  if (!ok) throw std::runtime_error(label);
}
using TickType_t = unsigned;
static constexpr TickType_t portMAX_DELAY = ~TickType_t{0};
static constexpr int pdTRUE = 1;
static void* gFsMutex = reinterpret_cast<void*>(uintptr_t{1});
static bool held = false;
static unsigned takes = 0, gives = 0, unlockedTraversal = 0;
static bool isHeldByCurrentTask(void* mutex) { return mutex == gFsMutex && held; }
static int xSemaphoreTake(void* mutex, TickType_t) {
  check(mutex == gFsMutex && !held, "mutex acquired without accidental recursive take");
  held = true; ++takes; return pdTRUE;
}
static void xSemaphoreGive(void* mutex) {
  check(mutex == gFsMutex && held, "only owner releases filesystem mutex");
  held = false; ++gives;
}
// INSERT_LOCKS

struct Node {
  std::string path, backendName;
  bool directory = false, alive = true;
  unsigned openHandles = 0;
  std::vector<std::shared_ptr<Node>> children;
};
static std::map<std::string, std::shared_ptr<Node>> nodes;
static std::set<std::string> failOpen, failRemove;
static std::vector<std::string> opened, attempted, removed;
static unsigned explicitCloses = 0, implicitCloses = 0, liveHandles = 0, metadataQueries = 0;
static bool virtualMounted = false, virtualTmp = false;
static void traversal() { if (!held) ++unlockedTraversal; }
struct Handle {
  std::shared_ptr<Node> node;
  size_t cursor = 0;
  bool open = true;
  explicit Handle(std::shared_ptr<Node> value) : node(std::move(value)) {
    ++node->openHandles; ++liveHandles;
  }
  void close(bool explicitCall) {
    if (!open) return;
    traversal(); open = false; --node->openHandles; --liveHandles;
    if (explicitCall) ++explicitCloses; else ++implicitCloses;
  }
  ~Handle() { close(false); }
};
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Node> node) : handle_(std::make_shared<Handle>(std::move(node))) {}
  explicit operator bool() const { return handle_ && handle_->open; }
  bool isDirectory() const { traversal(); return *this && handle_->node->directory; }
  const char* name() const { traversal(); return *this ? handle_->node->backendName.c_str() : ""; }
  size_t size() const { traversal(); return 17; }
  time_t getLastWrite() const { ++metadataQueries; return 123; }
  File openNextFile() {
    traversal();
    check(*this && handle_->node->directory, "only a live directory is enumerated");
    auto& children = handle_->node->children;
    while (handle_->cursor < children.size()) {
      auto child = children[handle_->cursor++];
      if (child->alive) return File(child);
    }
    return {};
  }
  void close() { if (handle_) handle_->close(true); }
 private:
  std::shared_ptr<Handle> handle_;
};
struct AuthContext { std::string owner; };
namespace VFS {
static AuthContext systemAuth(const char* owner) { return {owner}; }
static File openGuarded(const String& path, const char* mode, const AuthContext& auth) {
  FsLockGuard guard("fake.open");
  check(auth.owner == "fs.init.tmp_cleanup" && std::strcmp(mode, "r") == 0,
        "boot opens retain trusted system identity and read mode");
  opened.emplace_back(path.c_str());
  const auto found = nodes.find(path.c_str());
  if (failOpen.count(path.c_str()) || found == nodes.end() || !found->second->alive) return {};
  return File(found->second);
}
static bool removeGuarded(const String& path, const AuthContext& auth) {
  FsLockGuard guard("fake.remove");
  check(auth.owner == "fs.init.tmp_cleanup", "boot removal uses trusted system identity");
  attempted.emplace_back(path.c_str());
  const auto found = nodes.find(path.c_str());
  if (found == nodes.end() || !found->second->alive || found->second->directory || failRemove.count(path.c_str())) return false;
  check(found->second->openHandles == 0, "child is closed before unlink");
  found->second->alive = false;
  removed.emplace_back(path.c_str());
  return true;
}
static String normalize(const String& path) {
  std::string value = path.c_str();
  if (value.empty()) value = "/";
  while (value.size() > 1 && value.back() == '/') value.pop_back();
  return String(value.c_str());
}
static String stripSdPrefix(const String& path) {
  if (path == "/sd") return String("/");
  return path.startsWith("/sd/") ? path.substring(3) : path;
}
struct VirtualEntry { String name; bool isFolder = true; };
static size_t listVirtualEntries(const String& parent, VirtualEntry* entries, size_t capacity) {
  traversal();
  size_t count = 0;
  if (parent == "/" && virtualMounted && count < capacity) entries[count++] = {"sd", true};
  // Synthetic VFS mount shape proves the cleanup honors isVirtual even for a
  // future virtual entry named like a temp file. Not a claim about today's SD.
  if (parent == "/" && virtualTmp && count < capacity) entries[count++] = {"virtual.tmp", false};
  return count;
}
}
// INSERT_READER

struct Message { char level; std::string text; };
static std::vector<Message> logs;
static std::vector<std::string> systemLogs;
static std::string format(const char* pattern, va_list arguments) {
  char text[640]; std::vsnprintf(text, sizeof(text), pattern, arguments); return text;
}
static void espLog(char level, const char* tag, const char* pattern, ...) {
  check(std::strcmp(tag, "FS") == 0, "cleanup diagnostic tag unchanged");
  va_list arguments; va_start(arguments, pattern);
  logs.push_back({level, format(pattern, arguments)}); va_end(arguments);
}
#define ESP_LOGI(tag, ...) espLog('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) espLog('W', tag, __VA_ARGS__)
static void logSystemEvent(const char* tag, const char* pattern, ...) {
  check(std::strcmp(tag, "FS") == 0, "cleanup system-log category unchanged");
  va_list arguments; va_start(arguments, pattern);
  systemLogs.push_back(format(pattern, arguments)); va_end(arguments);
}
// INSERT_CLEANUP

static const std::vector<std::string> fixedDirs = {
    "/", "/system", "/system/users", "/system/users/user_settings", "/maps"
#if ENABLE_LLM_SOURCE_ONBOARD
    , "/system/llm"
#endif
};
static std::shared_ptr<Node> add(const std::string& path, bool directory = false,
                               const std::string& backendName = "") {
  auto node = std::make_shared<Node>();
  node->path = path; node->backendName = backendName.empty() ? path : backendName;
  node->directory = directory;
  nodes[path] = node;
  if (path != "/") {
    const auto slash = path.rfind('/');
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    check(nodes.count(parent) != 0, "fixture parent exists: " + path);
    nodes.at(parent)->children.push_back(node);
  }
  return node;
}
static void reset() {
  check(!held && liveHandles == 0, "prior fixture released every handle and lock");
  ++scenarios;
  nodes.clear(); failOpen.clear(); failRemove.clear(); opened.clear(); attempted.clear(); removed.clear();
  logs.clear(); systemLogs.clear(); takes = gives = unlockedTraversal = 0;
  explicitCloses = implicitCloses = metadataQueries = 0; virtualMounted = virtualTmp = false;
  add("/", true); add("/system", true); add("/system/users", true);
  add("/system/users/user_settings", true); add("/maps", true); add("/system/llm", true);
}
static bool exists(const std::string& path) { return nodes.count(path) && nodes.at(path)->alive; }
static void finished(bool nested = false) {
  check(liveHandles == 0, "all parent and child handles closed");
  check(held == nested, "cleanup preserves caller's filesystem-lock ownership");
  check(metadataQueries == 0, "cleanup does not request unused timestamps");
  check(opened == fixedDirs, "cleanup visits exactly its fixed directory list in order");
#if !BASELINE
  check(unlockedTraversal == 0, "all traversal and close operations run under the outer guard");
  check(takes == 1 && gives == (nested ? 0u : 1u), "one filesystem guard spans whole cleanup pass");
  check(implicitCloses == 0, "cleanup closes all valid directory handles explicitly");
#endif
}
static unsigned summaries() {
  unsigned count = 0;
  for (const auto& log : logs) if (log.text.find("orphaned .tmp file(s)") != std::string::npos) ++count;
  return count;
}
static void truthful(unsigned expected, unsigned warnings = 0) {
  unsigned successes = 0, failures = 0;
  for (const auto& log : logs) {
    if (log.text.find("Cleaned orphaned temp file:") != std::string::npos) {
      ++successes;
      check(log.level == 'I', "successful removal uses info severity");
      check(std::any_of(removed.begin(), removed.end(), [&](const auto& path) {
        return log.text.find(path) != std::string::npos;
      }), "success log corresponds to an actual deletion");
    }
    if (log.level == 'W') ++failures;
    if (log.text.find("orphaned .tmp file(s)") != std::string::npos)
      check(log.text == "Removed " + std::to_string(expected) + " orphaned .tmp file(s)", "summary contains only successful removals");
  }
  unsigned systemSuccesses = 0, systemFailures = 0;
  for (const auto& text : systemLogs) {
    if (text.find("removed orphaned temp file ") == 0) ++systemSuccesses;
    else if (text.find("orphaned temp cleanup failed for ") == 0) ++systemFailures;
    else check(false, "unexpected cleanup system-log category");
  }
  check(successes == expected && systemSuccesses == expected, "exactly one info and success system log per successful delete");
  check(systemFailures == warnings, "system log distinguishes failed removals");
  check(failures == warnings, "one warning per failed removal");
  check(summaries() == (expected ? 1u : 0u), "summary emitted only when files were deleted");
}
static void normalParity() {
  reset();
  std::vector<std::string> targets;
  for (const auto& directory : fixedDirs) {
    for (unsigned i = 0; i != 3; ++i) {
      const std::string leaf = "save" + std::to_string(i) + ".tmp";
      const std::string path = directory + (directory == "/" ? "" : "/") + leaf;
      add(path, false, leaf); targets.push_back(path);
    }
  }
  cleanupBootTemporaryFiles(); finished();
  check(removed == targets, "all consecutive temp entries deleted without iterator skips");
  truthful(static_cast<unsigned>(targets.size()));
  for (const auto& path : removed) std::printf("removed %s\n", path.c_str());
#if BASELINE
  check(unlockedTraversal > 0, "baseline characterization: traversal lacks one encompassing guard");
#endif
}
static void policyAndPaths() {
  reset(); virtualMounted = virtualTmp = true;
  const std::vector<std::string> removedExpected = {
      "/first.tmp", "/second.tmp", "/system/config.tmp", "/system/leaf.tmp",
      "/system/users/index.tmp", "/system/users/user_settings/42.json.tmp", "/maps/map.hwmap.tmp"
#if ENABLE_LLM_SOURCE_ONBOARD
      , "/system/llm/model.tmp"
#endif
  };
  for (const auto& path : removedExpected) add(path);
  nodes.at("/system/leaf.tmp")->backendName = "leaf.tmp";
  if (!ENABLE_LLM_SOURCE_ONBOARD) add("/system/llm/model.tmp");
  add("/system/header.header.tmp"); add("/system/upper.TMP"); add("/system/recovery.bak");
  add("/system/ordinary.json"); add("/system/trailing.tmp.more"); add("/system/empty-name.tmp")->backendName = "";
  add("/system/folder.tmp", true); add("/system/folder.tmp/nested.tmp");
  add("/system/nested", true); add("/system/nested/deeper.tmp");
  // A backend that reports nested children cannot extend this pass's scope.
  nodes.at("/system")->children.push_back(nodes.at("/system/nested/deeper.tmp"));
  add("/photos", true); add("/photos/photo.jpg.tmp"); add("/recordings", true); add("/recordings/audio.wav.tmp");
  add("/sd", true); add("/sd/keep.tmp");
  cleanupBootTemporaryFiles(); finished();
  check(removed == removedExpected, "rooted and leaf backend paths map to exact VFS children");
  check(attempted == removedExpected, "non-files, virtuals, nested paths and reserved suffixes never reach removal");
  for (const auto& path : {"/system/header.header.tmp", "/system/upper.TMP", "/system/recovery.bak",
       "/system/ordinary.json", "/system/trailing.tmp.more", "/system/empty-name.tmp", "/system/folder.tmp",
       "/system/folder.tmp/nested.tmp", "/system/nested/deeper.tmp", "/photos/photo.jpg.tmp", "/recordings/audio.wav.tmp", "/sd/keep.tmp"})
    check(exists(path), std::string("preserved path: ") + path);
  check(exists("/system/llm/model.tmp") == !ENABLE_LLM_SOURCE_ONBOARD, "LLM cleanup follows compile-time feature");
  truthful(static_cast<unsigned>(removedExpected.size()));
}
static void failuresAndEmpty() {
  reset(); cleanupBootTemporaryFiles(); finished(); truthful(0);
  reset(); add("/system/a.tmp"); add("/system/b.tmp"); add("/system/c.tmp");
  failRemove.insert("/system/b.tmp");
  cleanupBootTemporaryFiles(); finished(); truthful(2, 1);
  check(removed == std::vector<std::string>({"/system/a.tmp", "/system/c.tmp"}) && exists("/system/b.tmp"),
        "failed unlink retains file and does not prevent later deletion");
  reset(); add("/only.tmp"); failRemove.insert("/only.tmp");
  cleanupBootTemporaryFiles(); finished(); truthful(0, 1); check(exists("/only.tmp"), "all-failed pass retains destination");
  reset(); add("/system/unopened.tmp"); add("/maps/later.tmp"); failOpen.insert("/system");
  cleanupBootTemporaryFiles(); finished(); truthful(1);
  check(exists("/system/unopened.tmp") && !exists("/maps/later.tmp"), "failed directory open leaves its contents while continuing fixed list");
  reset(); nodes.at("/maps")->directory = false;
  cleanupBootTemporaryFiles(); finished(); truthful(0);
}
static void nestedGuard() {
  reset(); add("/system/a.tmp");
  { FsLockGuard caller("caller"); cleanupBootTemporaryFiles(); finished(true); truthful(1); }
  check(!held && takes == gives, "outer caller remains responsible for final unlock");
}
static void baselineRegressions() {
  reset(); add("/system/absolute.tmp");
  cleanupBootTemporaryFiles(); finished();
  check(exists("/system/absolute.tmp") && removed.empty(), "baseline rooted name constructs wrong unlink path");
  check(!attempted.empty() && attempted.front() == "/system//system/absolute.tmp", "baseline path defect characterized exactly");
  check(systemLogs.size() == 1 && summaries() == 1, "baseline reports deletion despite unsuccessful unlink");
  reset(); add("/header.header.tmp", false, "header.header.tmp"); add("/folder.tmp", true, "folder.tmp");
  cleanupBootTemporaryFiles(); finished();
  check(!exists("/header.header.tmp"), "baseline deletes header safety artifact");
  check(std::find(attempted.begin(), attempted.end(), "/folder.tmp") != attempted.end() && exists("/folder.tmp"),
        "baseline attempts to unlink a directory and reports it as cleaned");
  check(systemLogs.size() == 2 && removed.size() == 1, "baseline summary overcounts directory unlink failure");
}
#if HAS_STAGING_NAME
// The shared staging-name predicate the track pickers use to skip the files
// checked replacement leaves behind.
static void stagingNames() {
  ++scenarios;
  for (const char* staging : {"/a/b.tmp", "/a/b.header.tmp", "/a/b.bak", "x.tmp", "/logging_captures/tracks/live.csv.tmp"})
    check(VFS::isStagingName(String(staging)), "staging name not recognized");
  for (const char* plain : {"/a/b.csv", "/a/tmp", "/a/b.tmp.csv", "/a/bak", "", "/a/b.bak2"})
    check(!VFS::isStagingName(String(plain)), "plain name treated as staging");
}
#endif
int main() {
  try {
    normalParity();
#if BASELINE
    baselineRegressions();
#else
    policyAndPaths(); failuresAndEmpty(); nestedGuard();
#endif
#if HAS_STAGING_NAME
    stagingNames();
#endif
    std::fprintf(stderr, "boot_temp_cleanup %s LLM=%d: %u scenarios, %u checks passed\n",
                 BASELINE ? "baseline" : "current", ENABLE_LLM_SOURCE_ONBOARD, scenarios, checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "boot_temp_cleanup: %s\n", error.what()); return 1;
  }
}
