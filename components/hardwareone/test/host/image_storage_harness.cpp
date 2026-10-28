#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static size_t checks = 0;
static void check(bool okay, const std::string& why) {
  ++checks; if (!okay) throw std::runtime_error(why);
}
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}
  String(char value) : value_(1, value) {}
  size_t length() const { return value_.size(); }
  const char* c_str() const { return value_.c_str(); }
  char operator[](size_t index) const { return value_[index]; }
  bool reserve(size_t size) { value_.reserve(size); return true; }
  bool startsWith(const String& prefix) const { return value_.rfind(prefix.value_, 0) == 0; }
  bool endsWith(const String& suffix) const {
    return value_.size() >= suffix.length() && value_.compare(value_.size() - suffix.length(), suffix.length(), suffix.value_) == 0;
  }
  int indexOf(const String& text) const { const auto pos = value_.find(text.value_); return pos == std::string::npos ? -1 : static_cast<int>(pos); }
  int indexOf(char value) const { return indexOf(String(value)); }
  int indexOf(const String& text, int from) const {
    const auto pos = from < 0 ? std::string::npos : value_.find(text.value_, static_cast<size_t>(from));
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(char value, int from) const { return indexOf(String(value), from); }
  int lastIndexOf(char value) const { const auto pos = value_.rfind(value); return pos == std::string::npos ? -1 : static_cast<int>(pos); }
  bool equalsIgnoreCase(const String& other) const {
    return value_.size() == other.value_.size() && std::equal(value_.begin(), value_.end(), other.value_.begin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
  }
  long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }
  String substring(size_t start) const { return value_.substr(std::min(start, value_.size())); }
  String substring(size_t start, size_t end) const {
    start = std::min(start, value_.size()); end = std::min(std::max(start, end), value_.size());
    return value_.substr(start, end - start);
  }
  void remove(size_t start) { value_.erase(start); }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n"), last = value_.find_last_not_of(" \t\r\n");
    value_ = first == std::string::npos ? "" : value_.substr(first, last - first + 1);
  }
  void replace(const String& from, const String& to) {
    size_t start = 0;
    while ((start = value_.find(from.value_, start)) != std::string::npos) {
      value_.replace(start, from.length(), to.value_); start += to.length();
    }
  }
  String& operator+=(const String& value) { value_ += value.value_; return *this; }
  friend String operator+(const String& a, const String& b) { return a.value_ + b.value_; }
  friend bool operator==(const String& a, const String& b) { return a.value_ == b.value_; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
  friend bool operator<(const String& a, const String& b) { return a.value_ < b.value_; }
 private:
  std::string value_;
};
// The production argument parser, quoted-token rule and validate-only macro,
// so the imagedelete command runs unchanged.
// INSERT_COMMAND_PARSER
bool gCLIValidateOnly = false;

using TickType_t = uint32_t;
using SemaphoreHandle_t = int*;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
static int semaphore;
static SemaphoreHandle_t gFsMutex = &semaphore;
static bool heldFs = false;
static unsigned takes = 0, gives = 0;
static bool isHeldByCurrentTask(SemaphoreHandle_t) { return heldFs; }
static int xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  check(!heldFs, "nested production FsLockGuard must not take the RTOS mutex twice");
  heldFs = true; ++takes; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t) {
  check(heldFs, "only current filesystem owner releases mutex"); heldFs = false; ++gives;
}
// INSERT_FS_GUARD

template <class... Args> static void logIgnored(const char*, Args...) {}
#define INFO_STORAGEF(...) logIgnored(__VA_ARGS__)
#define ERROR_STORAGEF(...) logIgnored(__VA_ARGS__)
#define DEBUG_STORAGEF(...) logIgnored(__VA_ARGS__)
#define WARN_STORAGEF(...) logIgnored(__VA_ARGS__)
#define FILE_READ "r"

struct Settings { String cameraCaptureFolder = "/photos"; int cameraStorageLocation = 0; } gSettings;
static bool filesystemReady = true, sdMounted = false;
struct AuthContext { String user; String reason; };
struct Access { std::string operation, path, user, reason; };
static std::vector<Access> accesses;
static std::set<std::string> denied, deniedUsers;
// The command executor's per-task identity: an ordinary admin over the CLI.
static const AuthContext caller{"admin", "cli"};
static unsigned callerLookups = 0;
static const AuthContext& currentAuthContext() { ++callerLookups; return caller; }
static bool failOpen = false, failMkdir = false, failRemove = false, failStats = false;
static unsigned metadataUnlocked = 0, closesUnlocked = 0, directoryOpens = 0;
static unsigned activeFiles = 0, closeCalls = 0, statCalls = 0, mtimeCalls = 0;
static int lastStatType = -1;
static bool trackIo = true;
static void metadata() { if (trackIo && !heldFs) ++metadataUnlocked; }
struct Node {
  std::string name;
  bool directory = false;
  size_t size = 12;
  time_t modified = 12345;
  std::vector<std::shared_ptr<Node>> children;
  std::vector<uint8_t> bytes = {0xff, 0xd8, 3, 4};
};
static std::map<std::string, std::shared_ptr<Node>> nodes;
struct Handle {
  explicit Handle(std::shared_ptr<Node> value) : node(std::move(value)) { ++activeFiles; }
  ~Handle() { close(); }
  void close() {
    if (closed) return;
    if (trackIo && !heldFs) ++closesUnlocked;
    closed = true; ++closeCalls; --activeFiles;
  }
  std::shared_ptr<Node> node;
  size_t cursor = 0;
  bool closed = false;
};
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Node> value) : handle_(std::make_shared<Handle>(std::move(value))) {}
  explicit operator bool() const { return handle_ && !handle_->closed; }
  bool isDirectory() const { metadata(); return *this && handle_->node->directory; }
  const char* name() const { metadata(); return *this ? handle_->node->name.c_str() : ""; }
  size_t size() const { metadata(); return *this ? handle_->node->size : 0; }
  time_t getLastWrite() const { metadata(); ++mtimeCalls; return handle_->node->modified; }
  File openNextFile() {
    metadata();
    if (!*this || handle_->cursor >= handle_->node->children.size()) return {};
    return File(handle_->node->children[handle_->cursor++]);
  }
  size_t read(uint8_t* out, size_t length) {
    metadata(); const auto count = std::min(length, handle_->node->bytes.size());
    std::memcpy(out, handle_->node->bytes.data(), count); return count;
  }
  void close() { if (handle_) handle_->close(); handle_.reset(); }
 private:
  std::shared_ptr<Handle> handle_;
};
// INSERT_NORMALIZE_FS_PATH
namespace VFS {
enum StorageType { INTERNAL = 0, SDCARD = 1, AUTO = 2 };
struct VirtualEntry { const char* name; bool isFolder; };
static bool isSDAvailable() { return sdMounted; }
// Advanced by every successful mount, as VFS does for boot, remount and format.
static uint32_t sdGeneration = 0;
static uint32_t sdMountGeneration() { return sdGeneration; }
String normalize(const String&);
}
// INSERT_VFS_PATHS
// INSERT_DIRECTORY_READER

namespace VFS {
static AuthContext systemAuth(const char* reason) { return {"system", reason}; }
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
static AuthContext systemAuth(const char*, const char* reason) { return systemAuth(reason); }
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
static bool allowed(const char* operation, const String& input, const AuthContext& ctx, String& path) {
  check(heldFs, "guarded boundary performs I/O under its own nested guard");
  accesses.push_back({operation, input.c_str(), ctx.user.c_str(), ctx.reason.c_str()});
  check(ctx.user.length() != 0 && ctx.reason.length() != 0, "every guarded access carries a named identity");
  return normalizeFsPath(input, path) && !denied.count(path.c_str()) && !deniedUsers.count(ctx.user.c_str()) &&
         (getStorageType(path) == SDCARD ? sdMounted : filesystemReady);
}
static File openGuarded(const String& input, const char* mode, const AuthContext& ctx, bool = false) {
  FsLockGuard guard("mock.open"); String path;
  if (!allowed("open", input, ctx, path) || failOpen) return {};
  check(std::strcmp(mode, "r") == 0, "inventory opens read-only");
  const auto found = nodes.find(path.c_str());
  if (found == nodes.end()) return {};
  if (found->second->directory) ++directoryOpens;
  return File(found->second);
}
static bool existsGuarded(const String& input, const AuthContext& ctx) {
  FsLockGuard guard("mock.exists"); String path;
  return allowed("exists", input, ctx, path) && nodes.count(path.c_str());
}
static bool mkdirGuarded(const String& input, const AuthContext& ctx) {
  FsLockGuard guard("mock.mkdir"); String path;
  if (!allowed("mkdir", input, ctx, path) || failMkdir) return false;
  auto node = std::make_shared<Node>(); node->directory = true; node->name = stripSdPrefix(path).c_str();
  nodes[path.c_str()] = node; return true;
}
static bool removeGuarded(const String& input, const AuthContext& ctx) {
  FsLockGuard guard("mock.remove"); String path;
  return allowed("remove", input, ctx, path) && !failRemove && nodes.erase(path.c_str()) != 0;
}
static bool getStats(StorageType type, uint64_t& total, uint64_t& used, uint64_t& free) {
  FsLockGuard guard("mock.stats"); ++statCalls; lastStatType = type;
  if (failStats || (type == SDCARD ? !sdMounted : !filesystemReady)) return false;
  total = 4096; used = 1024; free = 3072; return true;
}
}
static constexpr uint32_t MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
enum class AllocPref { RequirePSRAM };
static size_t largestBlock = 4096;
static bool failAlloc = false;
static size_t heap_caps_get_largest_free_block(uint32_t) { return largestBlock; }
static void* ps_alloc(size_t length, AllocPref, const char*) { return failAlloc ? nullptr : std::malloc(length); }
static void ps_free(void* ptr) { std::free(ptr); }
// INSERT_IMAGE_MANAGER
ImageManager gImageManager;
// Command boundaries: the 4 KB debug buffer, ArduinoJson, hints, the camera
// frame source and the video recordings walk.
#include <ArduinoJson.h>
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
static char debugStorage[GLOBAL_DEBUG_BUFFER_SIZE];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugStorage; }
static void cliHint(const char*) {}
// INSERT_DEBUG_JSON
// INSERT_ARG_WANTS_JSON
static ImageStorageLocation capturedTo = IMAGE_STORAGE_LITTLEFS;
static unsigned captures = 0;
String ImageManager::captureAndSave(ImageStorageLocation location) {
  ++captures; capturedTo = location;
  return location == IMAGE_STORAGE_SD ? "/sd/photos/new.jpg" : "/photos/new.jpg";
}
static std::string recordings;
static String getVideoRecordingsList() { return recordings; }
// INSERT_IMAGE_COMMANDS

// Saved sources from before deleteImage took the caller's identity (--baseline)
// still compile, so the identity and scope regressions replay against them.
#ifndef IMAGE_DELETE_TAKES_CTX
#define IMAGE_DELETE_TAKES_CTX 1
#endif
static bool deleteAs(ImageManager& owner, const String& path, const AuthContext& ctx) {
#if IMAGE_DELETE_TAKES_CTX
  return owner.deleteImage(path, ctx);
#else
  (void)ctx; return owner.deleteImage(path);
#endif
}

static void balanced(bool expectOuter = false) {
  check(heldFs == expectOuter && gives + unsigned(expectOuter) == takes, "all filesystem guard scopes balance");
  check(activeFiles == 0, "all directory/child/image handles are closed");
}
static void reset(bool sd = false, bool internal = true) {
  balanced(); nodes.clear(); accesses.clear(); denied.clear();
  sdMounted = sd; filesystemReady = internal; gSettings.cameraCaptureFolder = "/photos";
  if (sd) ++VFS::sdGeneration;
  gSettings.cameraStorageLocation = 0; captures = 0; capturedTo = IMAGE_STORAGE_LITTLEFS; recordings.clear();
  failOpen = failMkdir = failRemove = failStats = failAlloc = false; largestBlock = 4096;
  metadataUnlocked = closesUnlocked = directoryOpens = closeCalls = statCalls = mtimeCalls = 0;
  takes = gives = 0; lastStatType = -1;
  auto root = std::make_shared<Node>(); root->name = "/"; root->directory = true; nodes["/"] = root;
  if (sd) { auto sdroot = std::make_shared<Node>(*root); nodes["/sd"] = sdroot; }
}
static std::shared_ptr<Node> add(const std::string& path, bool directory = false, const std::string& backend = "") {
  auto node = std::make_shared<Node>(); node->directory = directory;
  node->name = backend.empty() ? std::string(VFS::stripSdPrefix(path.c_str()).c_str()) : backend;
  nodes[path] = node;
  const auto slash = path.rfind('/'); const auto parent = slash == 0 ? "/" : path.substr(0, slash);
  if (nodes.count(parent)) nodes.at(parent)->children.push_back(node);
  return node;
}
static ImageManager manager() { ImageManager out; out.init(); accesses.clear(); statCalls = 0; takes = gives = 0; return out; }
static void expectIdentity(const std::string& operation, const std::string& reason, const std::string& user = "system") {
  auto found = std::find_if(accesses.begin(), accesses.end(), [&](const Access& access) { return access.operation == operation; });
  check(found != accesses.end() && found->user == user && found->reason == reason, "operation carries the expected identity and audit reason: " + operation);
}
static void routeDelete() {
  // "/sdata" is an internal folder, not the SD mount: configured as the capture
  // folder it must be deleted with internal availability while SD is absent.
  reset(); gSettings.cameraCaptureFolder = "/sdata"; add("/sdata", true); add("/sdata/photo.jpg"); auto owner = manager();
  check(deleteAs(owner, "/sdata/photo.jpg", caller) && !nodes.count("/sdata/photo.jpg"), "/sdata deletion must use internal availability when SD is absent");
  expectIdentity("remove", "cli", "admin"); balanced();
  // Under the default root the same path is outside the capture folder.
  reset(); add("/sdata/photo.jpg"); owner = manager();
  check(!deleteAs(owner, "/sdata/photo.jpg", caller) && nodes.count("/sdata/photo.jpg") && accesses.empty(), "/sdata is refused without filesystem access when the root is /photos"); balanced();
}
static void scopeRefusals(bool sd) {
  reset(sd); auto owner = manager(); const std::string prefix = sd ? "/sd" : "";
  const AuthContext system = VFS::systemAuth("imgmgr.delete_image");
  for (const std::string& folder : {"/system", "/system/users", "/system/ota", "/photos", "/photosX"}) if (!nodes.count(prefix + folder)) add(prefix + folder, true);
  // Every fixture exists, so an unscoped delete would remove it.
  const std::vector<std::string> outside = {"/system/settings.json", "/system/users/users.json", "/system/ota/staged.bin",
                                            "/system/photo.jpg", "/photosX/photo.jpg", "/photos/notes.txt", "/photos/UPPER.JPG", "/photos"};
  for (const auto& relative : outside) if (!nodes.count(prefix + relative)) add(prefix + relative);
  for (const auto& relative : outside) {
    const std::string path = prefix + relative;
    check(!deleteAs(owner, path.c_str(), caller) && nodes.count(path), "delete refuses a path outside the capture folder or not a JPEG: " + path);
    check(!deleteAs(owner, path.c_str(), system) && nodes.count(path), "the managed-photo rule does not depend on the identity: " + path);
  }
  for (const char* traversal : {"/photos/../system/settings.json", "/photos/./../system/users/users.json"}) {
    const std::string path = prefix + traversal;
    check(!deleteAs(owner, path.c_str(), caller) && nodes.count(prefix + "/system/settings.json") && nodes.count(prefix + "/system/users/users.json"), std::string("traversal spellings are refused: ") + path);
  }
  size_t count = 0;
  check(owner.getImage((prefix + "/system/settings.json").c_str(), &count) == nullptr && count == 0, "the reader shares the managed-photo rule");
  check(accesses.empty() && callerLookups == 0, "refusals happen before any guarded filesystem call and without ImageManager reading the command identity");
  // A different configured folder moves the boundary with it; the root folder admits any JPEG.
  gSettings.cameraCaptureFolder = "/pics"; add(prefix + "/pics", true); add(prefix + "/pics/photo.jpg"); add(prefix + "/photos/photo.jpg");
  check(!deleteAs(owner, (prefix + "/photos/photo.jpg").c_str(), caller) && nodes.count(prefix + "/photos/photo.jpg") && accesses.empty(), "the old folder is outside the configured capture folder");
  check(deleteAs(owner, (prefix + "/pics/photo.jpg").c_str(), caller) && !nodes.count(prefix + "/pics/photo.jpg"), "a JPEG inside the configured folder is deleted");
  expectIdentity("remove", "cli", "admin"); accesses.clear();
  gSettings.cameraCaptureFolder = "/"; add(prefix + "/top.jpeg");
  check(deleteAs(owner, (prefix + "/top.jpeg").c_str(), caller) && !nodes.count(prefix + "/top.jpeg"), "the root capture folder admits a top-level JPEG");
  check(!deleteAs(owner, (prefix + "/system/settings.json").c_str(), caller) && nodes.count(prefix + "/system/settings.json"), "the root capture folder still refuses a non-JPEG");
  balanced();
}
static void callerIdentity(bool sd) {
  reset(sd); auto owner = manager(); const auto location = sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  const std::string folder = sd ? "/sd/photos" : "/photos";
  if (!nodes.count(folder)) add(folder, true); add(folder + "/a.jpg"); add(folder + "/b.jpg");
  check(deleteAs(owner, (folder + "/a.jpg").c_str(), caller) && !nodes.count(folder + "/a.jpg"), "a managed photo is removed for the caller");
  expectIdentity("remove", "cli", "admin");
  check(accesses.size() == 1 && accesses[0].path == folder + "/a.jpg", "one guarded removal of the normalized path");
  accesses.clear(); deniedUsers.insert("admin");
  check(!deleteAs(owner, (folder + "/b.jpg").c_str(), caller) && nodes.count(folder + "/b.jpg"), "the filesystem rules decide for the caller: a denied caller keeps the file");
  check(accesses.size() == 1 && accesses[0].user == "admin", "the denied removal was attempted as the caller, not as system");
  deniedUsers.clear(); accesses.clear();
  // The mock's removal leaves the deleted entry in the directory listing; give
  // retention a listing that holds only the surviving photo.
  nodes.at(folder)->children.clear(); nodes.erase(folder + "/b.jpg"); add(folder + "/b.jpg");
  check(owner.deleteOldestImages(1, location) && !nodes.count(folder + "/b.jpg"), "retention keeps system authority for paths from its own listing");
  expectIdentity("remove", sd ? "imgmgr.delete.sd" : "imgmgr.delete_image");
  check(callerLookups == 0, "ImageManager never reads the command identity itself"); balanced();
}
static void commandPath() {
  reset(); add("/photos", true); add("/photos/a.jpg"); add("/photos/b.jpg"); add("/system/settings.json");
  gImageManager.init(); accesses.clear(); statCalls = 0; takes = gives = 0; callerLookups = 0;
  check(std::strcmp(cmd_imagedelete("\"/system/settings.json\""), "Error: Failed to delete image") == 0 && nodes.count("/system/settings.json") && accesses.empty(),
        "imagedelete refuses a path outside the capture folder and the file stays");
  check(std::strcmp(cmd_imagedelete("\"/photos/a.jpg\""), "Image deleted") == 0 && !nodes.count("/photos/a.jpg"), "imagedelete removes a managed photo");
  expectIdentity("remove", "cli", "admin");
  check(callerLookups == 2, "the command supplies its caller identity on every delete");
  accesses.clear(); deniedUsers.insert("admin");
  check(std::strcmp(cmd_imagedelete("\"/photos/b.jpg\""), "Error: Failed to delete image") == 0 && nodes.count("/photos/b.jpg"), "a caller the filesystem rules deny cannot delete through imagedelete");
  deniedUsers.clear(); accesses.clear();
  // The command stays a parser: quoting, argument count and validate-only are unchanged.
  check(std::strncmp(cmd_imagedelete("/photos/b.jpg"), "Error: name must be in quotes", 29) == 0, "unquoted path is refused by the parser");
  check(std::strncmp(cmd_imagedelete("\"/photos/b.jpg\" extra"), "Error: unexpected argument", 26) == 0, "extra argument is refused by the parser");
  gCLIValidateOnly = true;
  check(std::strcmp(cmd_imagedelete("\"/photos/b.jpg\""), "VALID") == 0, "validate-only returns VALID");
  gCLIValidateOnly = false;
  check(accesses.empty() && nodes.count("/photos/b.jpg"), "parser refusals and validate-only touch no file"); balanced();
}
static void listLock() {
  reset(true); add("/sd/photos", true); add("/sd/photos/image.jpg"); auto owner = manager();
  check(owner.listImages(IMAGE_STORAGE_SD).size() == 1, "SD fixture image is listed");
  check(metadataUnlocked == 0 && closesUnlocked == 0, "SD directory traversal and child/parent cleanup must remain locked"); balanced();
}
static void failedCapacity(bool sd) {
  reset(sd); auto owner = manager(); failStats = true; const auto stats = owner.getStorageStats(sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS);
  check(!stats.available && !stats.totalBytes && !stats.usedBytes && !stats.freeBytes && !stats.imageCount,
        "failed capacity query returns unavailable zero-initialized stats");
  check(statCalls == 1 && accesses.empty(), "failed capacity query must not scan image directory"); balanced();
}
static void pathForms(bool sd) {
  for (const char* path : (sd ? std::vector<const char*>{"sd/photos/image.jpg", " /sd//photos/image.jpg "} :
                                std::vector<const char*>{"photos/image.jpg", " /photos//image.jpg "})) {
    reset(sd); auto owner = manager(); const std::string canonical = sd ? "/sd/photos/image.jpg" : "/photos/image.jpg"; add(canonical);
    check(deleteAs(owner, path, caller) && !nodes.count(canonical), "normalized alternate spellings delete the canonical managed photo");
    check(accesses.size() == 1 && accesses[0].operation == "remove" && accesses[0].path == canonical, "the guarded removal receives the normalized path"); balanced();
  }
}
static void inventory(ImageStorageLocation location, const char* capture = "/photos") {
  const bool sd = location == IMAGE_STORAGE_SD;
  reset(true); gSettings.cameraCaptureFolder = capture; auto owner = manager();
  const std::string folder = VFS::normalize(owner.getCaptureFolder(location)).c_str();
  if (!nodes.count(folder)) add(folder, true);
  const std::string join = folder + (folder == "/" ? "" : "/");
  add(join + "z.jpg")->modified = 11;
  add(join + "b.jpeg", false, "b.jpeg")->modified = 22;
  add(join + "UPPER.JPG"); add(join + "text.txt"); add(join + "dir.jpg", true);
  add(join + ".hidden.jpg")->modified = 33;
  // The shared reader excludes malformed/non-direct backend entries. Image
  // inventory still keeps dotfiles because it has no browser visibility policy.
  add(join + "empty.jpg")->name = "";
  add(join + "nested.jpg")->name = std::string(VFS::stripSdPrefix(folder.c_str()).c_str()) + "/nested/file.jpg";
  const auto images = owner.listImages(location);
  check(images.size() == 3 && images[0].filename == "z.jpg" && images[1].filename == "b.jpeg" && images[2].filename == ".hidden.jpg", "only direct lowercase JPEG files retain backend order and dotfile visibility; virtual mounts/directories excluded");
  check(images[0].fullPath == String((join + "z.jpg").c_str()) && images[1].fullPath == String((join + "b.jpeg").c_str()), "listed paths use canonical VFS folder join");
  check(images[0].size == 12 && images[1].size == 12 && images[0].isOnSD == sd && images[0].location == (sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS), "listing retains image metadata and tier");
  check(images[0].timestamp == (sd ? 11 : 0) && images[1].timestamp == (sd ? 22 : 0) && images[2].timestamp == (sd ? 33 : 0), "only SD exposes native timestamps");
  if (!sd) check(mtimeCalls == 0, "internal listing avoids unnecessary backend timestamp queries");
  expectIdentity("open", sd ? "imgmgr.list_sd" : "imgmgr.list_images");
  check(metadataUnlocked == 0 && closesUnlocked == 0, "list operations and cleanup keep filesystem lock"); balanced();
}
static void availability() {
  reset(true, false); auto owner = manager(); const auto location = IMAGE_STORAGE_LITTLEFS;
  const char* path = "/photos/no.jpg";
  // LittleFS is mounted once before init; a later ready flag does not bypass
  // the manager's init snapshot.
  filesystemReady = true; add(path);
  check(!owner.ensureCaptureFolder(location) && owner.listImages(location).empty() && !deleteAs(owner, path, caller) && nodes.count(path), "unavailable manager tier rejects inventory operations");
  check(!owner.getStorageStats(location).available && accesses.empty() && !statCalls, "unavailable tier avoids I/O and capacity queries"); balanced();
}
// VFS owns SD availability: a card mounted after init (sdmount, sdformat) is
// listed, counted and deletable at once, and an unmounted card is refused.
static void sdFollowsVfs() {
  reset(false); auto owner = manager();
  check(owner.listImages(IMAGE_STORAGE_SD).empty() && !owner.getStorageStats(IMAGE_STORAGE_SD).available && !owner.isSDAvailable(),
        "no card at init: SD is unavailable");
  sdMounted = true; ++VFS::sdGeneration;
  add("/sd", true); add("/sd/photos", true); add("/sd/photos/a.jpg"); add("/sd/photos/b.jpg");
  check(owner.isSDAvailable(), "a card mounted after init is reported available");
  check(owner.listImages(IMAGE_STORAGE_SD).size() == 2, "a card mounted after init is listed");
  const auto stats = owner.getStorageStats(IMAGE_STORAGE_SD);
  check(stats.available && stats.imageCount == 2, "a card mounted after init has storage stats");
  check(owner.ensureCaptureFolder(IMAGE_STORAGE_SD), "a card mounted after init can prepare the capture folder");
  check(deleteAs(owner, "/sd/photos/a.jpg", caller) && !nodes.count("/sd/photos/a.jpg"), "a photo on a card mounted after init can be deleted");
  sdMounted = false; accesses.clear(); statCalls = 0;
  check(!owner.isSDAvailable() && owner.listImages(IMAGE_STORAGE_SD).empty() && !owner.getStorageStats(IMAGE_STORAGE_SD).available,
        "after unmount SD is unavailable, unlisted and has no stats");
  check(!owner.ensureCaptureFolder(IMAGE_STORAGE_SD) && !deleteAs(owner, "/sd/photos/b.jpg", caller) && nodes.count("/sd/photos/b.jpg"),
        "after unmount SD folder preparation and deletes are refused");
  check(accesses.empty() && !statCalls, "an unmounted card gets no guarded I/O or capacity query"); balanced();
}
static void ensureFolder(bool sd, unsigned failure) {
  reset(sd); auto owner = manager(); const auto location = sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  const std::string folder = sd ? "/sd/photos" : "/photos"; nodes.erase(folder);
  if (failure == 1) failMkdir = true;
  if (failure == 2) denied.insert(folder);
  check(owner.ensureCaptureFolder(location) == (failure == 0), "capture folder reports mkdir/permission outcome");
  expectIdentity("mkdir", sd ? "imgmgr.ensure_capture_folder.sd" : "imgmgr.ensure_capture_folder");
  check(takes == 1 && gives == 1, "capture-folder exists/create stays in one filesystem lock scope");
  if (!failure) { accesses.clear(); check(owner.ensureCaptureFolder(location), "existing capture folder succeeds"); check(accesses.size() == 1 && accesses[0].operation == "exists", "existing folder is not recreated"); }
  balanced();
}
static void failures(bool sd, unsigned failure) {
  reset(sd); auto owner = manager(); const auto location = sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  const std::string folder = sd ? "/sd/photos" : "/photos", path = folder + "/photo.jpg";
  if (!nodes.count(folder)) add(folder, true); add(path);
  if (failure == 0) failOpen = true;
  if (failure == 1) { denied.insert(folder); denied.insert(path); }
  if (failure == 2) { nodes[folder]->directory = false; nodes.erase(path); }
  check(owner.listImages(location).empty(), "missing/denied/non-directory list gives empty inventory");
  if (failure == 0) failRemove = true;
  check(!deleteAs(owner, path.c_str(), caller), "delete failure/denial/missing path reports false"); balanced();
  check(!metadataUnlocked && !closesUnlocked, "failure cleanup retains lock for all File activity");
}
static void capacitySuccess(ImageStorageLocation location) {
  reset(true); auto owner = manager(); const bool sd = location == IMAGE_STORAGE_SD;
  const std::string folder = sd ? "/sd/photos" : "/photos";
  if (!nodes.count(folder)) add(folder, true); add(folder + "/one.jpg"); add(folder + "/skip.txt");
  const auto stats = owner.getStorageStats(location);
  check(stats.available && stats.totalBytes == 4096 && stats.usedBytes == 1024 && stats.freeBytes == 3072 && stats.imageCount == 1, "fresh successful stats include selected inventory count");
  check(statCalls == 1 && lastStatType == (sd ? VFS::SDCARD : VFS::INTERNAL), "BOTH keeps legacy internal inventory/capacity semantics"); balanced();
}
static void nested(bool sd) {
  reset(sd); auto owner = manager(); const auto location = sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  const char* path = sd ? "/sd/photos/test.jpg" : "/photos/test.jpg";
  { FsLockGuard outer("outer caller");
    check(owner.ensureCaptureFolder(location), "nested ensure succeeds"); add(path);
    check(owner.listImages(location).size() == 1, "nested directory scan succeeds");
    check(owner.getStorageStats(location).available && deleteAs(owner, path, caller), "nested stats/delete succeed");
    check(heldFs && takes == 1 && gives == 0, "nested methods retain caller's original lock ownership"); balanced(true);
  }
  balanced();
}
static void retention(bool sd, unsigned failure) {
  reset(sd); auto owner = manager(); const auto location = sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  const std::string folder = sd ? "/sd/photos" : "/photos";
  if (!nodes.count(folder)) add(folder, true);
  const std::string alpha = folder + "/a.jpg", omega = folder + "/z.jpg";
  add(alpha)->modified = 200; add(omega)->modified = 100;
  const std::string first = sd ? omega : alpha, second = sd ? alpha : omega;
  if (failure == 1) failRemove = true;
  if (failure == 2) denied.insert(first);
  check(owner.deleteOldestImages(failure == 2 ? 2 : 1, location) == (failure != 1),
        "retention preserves success/failed-removal/partial-success outcomes");
  expectIdentity("remove", sd ? "imgmgr.delete.sd" : "imgmgr.delete_image");
  if (failure == 0) check(!nodes.count(first) && nodes.count(second),
        "SD selects oldest timestamp; internal selects earliest filename");
  if (failure == 1) check(nodes.count(first) && nodes.count(second), "failed retention removal leaves both files");
  if (failure == 2) check(nodes.count(first) && !nodes.count(second), "retention continues after one denied removal");
  check(!metadataUnlocked && !closesUnlocked, "retention uses locked shared inventory"); balanced();
}
static void imageRead(bool sd) {
  reset(sd); auto owner = manager(); const char* path = sd ? "/sd/photos/data.jpg" : "/photos/data.jpg";
  auto node = add(path); node->size = node->bytes.size(); size_t count = 0;
  uint8_t* bytes = owner.getImage(path, &count);
  check(bytes && count == 4 && bytes[0] == 0xff && bytes[1] == 0xd8, "image reader retains JPEG/PSRAM path and storage routing"); ps_free(bytes);
  check(!metadataUnlocked && !closesUnlocked, "reader metadata/data/close keep existing locking"); balanced();
  accesses.clear(); add(sd ? "/sd/system/settings.json" : "/system/settings.json");
  check(owner.getImage(sd ? "/sd/system/settings.json" : "/system/settings.json", &count) == nullptr && count == 0 && accesses.empty(),
        "image reader refuses a path outside the managed JPEG folder before any guarded open"); balanced();
}
// capture and images take their tier as a first token, the way the
// dispatcher hands over arguments ("capture sd" arrives as "sd").
static void captureArguments() {
  reset(true); add("/photos", true); add("/photos/in.jpg"); add("/sd/photos", true); add("/sd/photos/card.jpg");
  gImageManager.init(); gSettings.cameraStorageLocation = IMAGE_STORAGE_LITTLEFS;
  const struct { const char* args; ImageStorageLocation expected; } routes[] = {
    {"sd", IMAGE_STORAGE_SD}, {"both", IMAGE_STORAGE_BOTH}, {"SD", IMAGE_STORAGE_SD}, {"", IMAGE_STORAGE_LITTLEFS}};
  for (const auto& route : routes) {
    captures = 0; capturedTo = IMAGE_STORAGE_BOTH;
    const char* reply = cmd_capture(route.args);
    check(captures == 1 && capturedTo == route.expected, std::string("capture '") + route.args + "' saves to the tier it names");
    check(std::strncmp(reply, "Captured: ", 10) == 0, "a capture reports its saved path");
  }
  gSettings.cameraStorageLocation = IMAGE_STORAGE_SD;
  for (const char* internal : {"littlefs", "lfs"}) {
    captures = 0; cmd_capture(internal);
    check(captures == 1 && capturedTo == IMAGE_STORAGE_LITTLEFS, std::string("capture '") + internal + "' overrides an SD default");
  }
  captures = 0; cmd_capture("");
  check(captures == 1 && capturedTo == IMAGE_STORAGE_SD, "no token keeps cameraStorageLocation");
  captures = 0;
  check(std::strncmp(cmd_capture("tape"), "Error: unknown storage", 22) == 0 && captures == 0, "an unknown tier is refused, not replaced by the default");
  check(std::strncmp(cmd_capture("sd both"), "Error: unexpected argument", 26) == 0 && captures == 0, "a second token is refused");
  balanced();
}
static void imagesArguments() {
  reset(true); add("/photos", true); add("/photos/in.jpg"); add("/sd/photos", true); add("/sd/photos/card.jpg");
  gImageManager.init();
  for (const char* args : {"sd json", "json sd", "SD json"}) {
    JsonDocument doc;
    check(!deserializeJson(doc, cmd_images(args)), std::string("images '") + args + "' replies with JSON");
    check(doc["location"] == "SD" && doc["images"].size() == 1 && doc["images"][0]["filename"] == "card.jpg",
          std::string("images '") + args + "' lists the SD card");
  }
  for (const char* args : {"json", "lfs json", "littlefs json"}) {
    JsonDocument doc;
    check(!deserializeJson(doc, cmd_images(args)) && doc["location"] == "LittleFS" && doc["images"][0]["filename"] == "in.jpg",
          std::string("images '") + args + "' lists internal storage");
  }
  check(std::strstr(cmd_images("sd"), "=== Images on SD ===") != nullptr, "images sd lists the SD card as text");
  check(std::strstr(cmd_images(""), "=== Images on LittleFS ===") != nullptr, "images lists internal storage as text");
  check(std::strncmp(cmd_images("sd extra"), "Error: unexpected argument", 26) == 0, "images refuses an unknown token");
  balanced();
}
static std::string photoName(int index) {
  char name[32]; std::snprintf(name, sizeof(name), "img_20261013_%06d.jpg", 120000 + index); return name;
}
// The app/BLE JSON listings must parse whatever the photo or recording count:
// complete when they fit the 4 KB debug buffer, otherwise trimmed with
// "truncated":true and the total still reported.
static void photoListings() {
  for (int photos : {40, 160}) {
    reset(true); add("/sd/photos", true);
    for (int i = 0; i < photos; ++i) add("/sd/photos/" + photoName(i))->size = 123456;
    gImageManager.init();
    // "json sd" is the one spelling the old token test routed to SD, so this
    // case isolates the buffer size from the argument parsing.
    const char* reply = cmd_images("json sd");
    JsonDocument doc;
    check(std::strlen(reply) < GLOBAL_DEBUG_BUFFER_SIZE, "the photo listing stays inside the debug buffer");
    check(!deserializeJson(doc, reply), std::to_string(photos) + "-photo JSON listing parses");
    const size_t listed = doc["images"].size();
    check(doc["imageCount"] == photos && listed > 0, "the listing reports the photo total");
    if (photos == 40) check(listed == 40 && doc["truncated"].isNull(), "40 photos are listed completely");
    else check(listed < size_t(photos) && doc["truncated"] == true, "an oversized listing is trimmed and says so");
    check(doc["images"][listed - 1]["filename"] == photoName(int(listed) - 1).c_str() && doc["images"][listed - 1]["size"] == 123456,
          "listed entries are whole");
    balanced();
  }
}
static void recordingListings() {
  for (int count : {10, 40, 200}) {
    reset(); recordings.clear();
    for (int i = 0; i < count; ++i) {
      char line[48]; std::snprintf(line, sizeof(line), "%srec_20261013_%06d.avi:1234567", i ? "\n" : "", 120000 + i);
      recordings += line;
    }
    const char* reply = cmd_cameravideolist("json");
    JsonDocument doc;
    check(std::strlen(reply) < GLOBAL_DEBUG_BUFFER_SIZE && !deserializeJson(doc, reply), std::to_string(count) + "-recording JSON listing parses");
    const size_t listed = doc["recordings"].size();
    check(doc["count"] == count && listed > 0 && doc["recordings"][listed - 1]["size"] == 1234567, "the recording listing reports its total and whole entries");
    if (count <= 40) check(listed == size_t(count) && doc["truncated"].isNull(), "recordings that fit are listed completely");
    else check(listed < size_t(count) && doc["truncated"] == true, "an oversized recording listing is trimmed and says so");
    const std::string text = cmd_cameravideolist("");
    check(text.rfind("Recordings (" + std::to_string(count) + "):", 0) == 0, "the text listing states the total");
    const size_t lines = std::count(text.begin(), text.end(), '\n');
    if (count <= 40) {
      char last[40]; std::snprintf(last, sizeof(last), "rec_20261013_%06d.avi:1234567", 120000 + count - 1);
      check(lines == size_t(count) && text.size() > std::strlen(last) && text.compare(text.size() - std::strlen(last), std::string::npos, last) == 0,
            "every recording that fits is listed in full");
    } else {
      const size_t note = text.rfind("\n... (");
      check(note != std::string::npos && text.compare(text.size() - 6, 6, " more)") == 0, "recordings that do not fit are counted, not cut silently");
      const int more = std::atoi(text.c_str() + note + 6);
      check(size_t(more) + lines - 1 == size_t(count), "shown plus counted recordings make the total");
      const size_t previous = text.rfind('\n', note - 1);
      check(text.compare(note - 8, 8, ":1234567") == 0 && previous != std::string::npos, "the last shown recording line is whole");
    }
  }
}
int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  unsigned failuresCount = 0, scenarios = 0;
  const auto run = [&](const std::string& label, const std::function<void()>& test) {
    ++scenarios;
    try { test(); std::printf("PASS %s\n", label.c_str()); }
    catch (const std::exception& error) { ++failuresCount; std::printf("FAIL %s: %s\n", label.c_str(), error.what()); }
  };
  if (selected == "all" || selected == "mount") {
    run("SD availability follows the VFS mount after init", sdFollowsVfs);
  }
  if (selected == "all" || selected == "commands") {
    run("capture parses its tier token", captureArguments);
    run("images parses its tier and json tokens", imagesArguments);
    run("photo JSON listing fits the debug buffer and parses", photoListings);
    run("recording listings fit the debug buffer, parse and count what they leave out", recordingListings);
  }
  if (selected == "all" || selected == "routing") {
    run("internal /sdata delete with SD absent", routeDelete);
  }
  if (selected == "all" || selected == "scope") {
    for (bool sd : {false, true}) {
      const std::string tier = sd ? "SD " : "internal ";
      run(tier + "delete refuses paths outside the capture folder and non-JPEGs", [=] { scopeRefusals(sd); });
      run(tier + "delete removes as the caller; retention keeps system authority", [=] { callerIdentity(sd); });
    }
    run("imagedelete command deletes as its caller", commandPath);
  }
  if (selected == "all" || selected == "locking") {
    run("SD directory lifetime lock", listLock);
  }
  if (selected == "all" || selected == "capacity") {
    for (bool sd : {false, true}) run(std::string(sd ? "SD" : "internal") + " failed capacity", [=] { failedCapacity(sd); });
  }
  if (selected == "all") {
    for (bool sd : {false, true}) {
      const std::string tier = sd ? "SD " : "internal ";
      run(tier + "alternate path spellings and metadata compatibility", [=] { pathForms(sd); });
      run(tier + "nested filesystem guards", [=] { nested(sd); });
      run(tier + "JPEG buffer read compatibility", [=] { imageRead(sd); });
      for (unsigned fault = 0; fault < 3; ++fault) {
        run(tier + "retention outcome " + std::to_string(fault), [=] { retention(sd, fault); });
        run(tier + "ensure folder outcome " + std::to_string(fault), [=] { ensureFolder(sd, fault); });
        run(tier + "failed list/metadata/delete " + std::to_string(fault), [=] { failures(sd, fault); });
      }
    }
    run("internal unavailable manager gate", availability);
    for (auto location : {IMAGE_STORAGE_LITTLEFS, IMAGE_STORAGE_SD, IMAGE_STORAGE_BOTH}) {
      run("ordered JPEG inventory tier " + std::to_string(location), [=] { inventory(location); });
      run("canonical root inventory tier " + std::to_string(location), [=] { inventory(location, "/"); });
      run("successful capacity/count tier " + std::to_string(location), [=] { capacitySuccess(location); });
    }
  }
  std::printf("ImageManager production storage: %u scenarios, %zu assertions, %u failed\n", scenarios, checks, failuresCount);
  return failuresCount ? 1 : 0;
}
