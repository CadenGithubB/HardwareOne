#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "System_PollPause.h"

static void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(std::string value) : value_(std::move(value)) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool reserve(size_t size) { value_.reserve(size); return true; }
  String& operator+=(char value) { value_ += value; return *this; }
  bool concat(const char* value) { value_ += value; return true; }
  bool startsWith(const char* value) const { return value_.rfind(value, 0) == 0; }
  bool endsWith(const char* value) const {
    size_t length = std::strlen(value);
    return value_.size() >= length && value_.compare(value_.size() - length, length, value) == 0;
  }
  int lastIndexOf(char value) const { auto at = value_.rfind(value); return at == std::string::npos ? -1 : int(at); }
  int indexOf(char value, size_t from = 0) const { auto at = value_.find(value, from); return at == std::string::npos ? -1 : int(at); }
  // indexOf(String) and replace() are what the editor save's former private
  // decoder used; they keep a --baseline WebServer_Server.cpp compiling.
  int indexOf(const String& value, size_t from = 0) const { auto at = value_.find(value.value_, from); return at == std::string::npos ? -1 : int(at); }
  void replace(const char* from, const char* to) {
    const size_t fromLength = std::strlen(from), toLength = std::strlen(to);
    for (size_t at = value_.find(from); at != std::string::npos; at = value_.find(from, at + toLength)) value_.replace(at, fromLength, to);
  }
  String substring(size_t start, size_t end = std::string::npos) const { return value_.substr(start, end == std::string::npos ? end : end - start); }
  bool operator==(const char* value) const { return value_ == value; }
  bool operator==(const String& value) const { return value_ == value.value_; }
  char operator[](size_t index) const { return value_[index]; }
  friend String operator+(const String& left, const char* right) { return left.value_ + right; }
 private:
  std::string value_;
};
using std::min;
#define DEBUG_STORAGEF(...) ((void)0)
#define ERROR_STORAGEF(...) ((void)0)
#define WARN_SYSTEMF(...) ((void)0)
#define INFO_WEBF(...) ((void)0)
#define ERROR_WEBF(...) ((void)0)
#define ERROR_MEMORYF(...) ((void)0)
// The production form decoders (System_Utils.cpp) both handlers now share.
// INSERT_FORM_DECODERS

using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
using SemaphoreHandle_t = int*;
static int semaphore;
static SemaphoreHandle_t gFsMutex = &semaphore;
static bool ownsFs = false;
static unsigned pauseDepth = 0, pauseCalls = 0, resumeCalls = 0, takes = 0, gives = 0;
volatile bool gSensorPollingPaused = false;
void pollPause(uint8_t) { ++pauseDepth; ++pauseCalls; }
void pollResume(uint8_t) { check(pauseDepth > 0, "resume without pause"); --pauseDepth; ++resumeCalls; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }
static bool isHeldByCurrentTask(SemaphoreHandle_t) { return ownsFs; }
static int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { check(!ownsFs, "nonrecursive mutex reacquired"); ownsFs = true; ++takes; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) { check(ownsFs, "unowned mutex release"); ownsFs = false; ++gives; }
// INSERT_FS_GUARD

struct Controls {
  bool auth = true, guestAllowed = true, sd = true, import = true, admin = true;
  bool openFails = false, parentExists = true, shortWrite = false, cleanupUser = true, cleanupSystem = true;
  bool temporaryOpenFails = false;          // the backend refuses only the staged ".upload.tmp" open (full, transient, denied)
  bool magicMap = false, mapOrganization = true, replaceFails = false, writeAllowed = true;
  bool renameAllowed = true, createAllowed = true, deleteAllowed = true;  // the caller's rule-table bits at promotion
  bool requirePause = true;                 // the editor save never paused polling; the upload does
  bool hooksOutsideLock = true;             // the editor keeps its function-scoped lock through the post-save hooks
  size_t freeBytes = 1024 * 1024, failAllocation = 0, chunkSize = 4096;
  size_t disconnectAt = SIZE_MAX, maxNameLength = SIZE_MAX;
  unsigned timeouts = 0;                    // consecutive timeouts before the first byte
  std::map<size_t, unsigned> timeoutAt;     // body offset -> timeouts injected at that offset
} control;
struct Metrics {
  unsigned allocations = 0, frees = 0, opens = 0, closes = 0, flushes = 0, writes = 0;
  unsigned posthooks = 0, mapProbes = 0, mapHooks = 0, waypointHooks = 0, mkdirs = 0;
  unsigned receives = 0, responses = 0, promotions = 0;
  bool badIoScope = false, badHookResources = false, badCloseScope = false;
  std::vector<std::string> cleanupUsers, openedUsers, openedPaths, promotedUsers, systemReasons;
} metrics;
static std::set<void*> buffers;
static bool filesystemReady = true;
static bool outerLock = false;
static void io() { if (!ownsFs || (control.requirePause && !pauseDepth)) metrics.badIoScope = true; }
namespace AllocPref { enum Kind { PreferPSRAM }; }
static void* ps_alloc(size_t size, AllocPref::Kind) {
  if (++metrics.allocations == control.failAllocation) return nullptr;
  void* result = std::malloc(size); check(result != nullptr, "host allocation failed"); buffers.insert(result); return result;
}
static void* ps_alloc(size_t size, AllocPref::Kind kind, const char*) { return ps_alloc(size, kind); }
static void trackedFree(void* allocation) {
  if (allocation) { check(buffers.erase(allocation) == 1, "unowned buffer free"); ++metrics.frees; std::free(allocation); }
}
struct EspMock { uint32_t getFreeHeap() const { return 100000; } } ESP;
static uint32_t millis() { return 1234; }
static void delay(int) {}

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_SOCK_ERR_TIMEOUT = -2, HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
  size_t content_len = 0, position = 0;
  std::string body, query, response;
};
struct AuthContext { String user = "alice", ip = "127.0.0.1"; };
static AuthContext makeWebAuthCtx(httpd_req_t*) { return {}; }
static bool tgRequireAuth(const AuthContext&) { return control.auth; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return control.guestAllowed; }
static bool canImport(const String&, const AuthContext&) { return control.import; }
// The silent rule-table queries the staged writer asks before promoting as the caller.
static bool canRename(const String&, const AuthContext& ctx) { return ctx.user == "system" || control.renameAllowed; }
static bool canCreate(const String&, const AuthContext& ctx) { return ctx.user == "system" || control.createAllowed; }
static bool canDelete(const String&, const AuthContext& ctx) { return ctx.user == "system" || control.deleteAllowed; }
static bool isAdminOnlyPath(const String& path) { return path.startsWith("/system/"); }
static bool isAdminUser(const String&) { return control.admin; }
static void sendJsonResponse(httpd_req_t* req, const char* value) { req->response = value; ++metrics.responses; }
static int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
static int httpd_resp_send(httpd_req_t* req, const char* value, int) { sendJsonResponse(req, value); return ESP_OK; }
static int httpd_req_get_url_query_str(httpd_req_t* req, char* output, size_t size) {
  if (req->query.empty() || req->query.size() >= size) return ESP_FAIL;
  std::strcpy(output, req->query.c_str()); return ESP_OK;
}
static int httpd_query_key_value(const char* query, const char* key, char* output, size_t size) {
  std::string prefix = std::string(key) + "=";
  std::string text(query);
  if (text.rfind(prefix, 0) != 0 || text.size() - prefix.size() >= size) return ESP_FAIL;
  std::strcpy(output, text.c_str() + prefix.size()); return ESP_OK;
}
static int httpd_req_recv(httpd_req_t* req, char* output, int length) {
  ++metrics.receives;
  if (control.timeouts) { --control.timeouts; return HTTPD_SOCK_ERR_TIMEOUT; }
  const auto idle = control.timeoutAt.find(req->position);
  if (idle != control.timeoutAt.end() && idle->second) { --idle->second; return HTTPD_SOCK_ERR_TIMEOUT; }
  if (req->position >= control.disconnectAt || req->position >= req->body.size()) return -3;
  size_t count = std::min({size_t(length), control.chunkSize, req->body.size() - req->position, control.disconnectAt - req->position});
  std::memcpy(output, req->body.data() + req->position, count); req->position += count; return int(count);
}

struct Node { std::string bytes; };
static std::map<std::string, std::shared_ptr<Node>> files;
static unsigned liveHandles = 0;
struct Handle {
  std::shared_ptr<Node> node;
  bool open = true;
  explicit Handle(std::shared_ptr<Node> value) : node(std::move(value)) { ++liveHandles; }
  void close() {
    if (!open) return;
    if (!ownsFs || (control.requirePause && !pauseDepth)) metrics.badCloseScope = true;
    open = false; --liveHandles; ++metrics.closes;
  }
  ~Handle() { close(); }
};
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Node> node) : handle_(std::make_shared<Handle>(std::move(node))) {}
  explicit operator bool() const { return handle_ && handle_->open; }
  size_t write(const uint8_t* bytes, size_t size) {
    io(); check(bool(*this), "write closed file"); ++metrics.writes;
    size_t count = control.shortWrite && size ? size - 1 : size;
    handle_->node->bytes.append(reinterpret_cast<const char*>(bytes), count); return count;
  }
  void close() { if (handle_) handle_->close(); handle_.reset(); }
  void flush() { io(); ++metrics.flushes; }
 private:
  std::shared_ptr<Handle> handle_;
};
namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return path == "/sd" || path.startsWith("/sd/") ? SDCARD : INTERNAL; }
static bool isSDAvailable() { return control.sd; }
static bool getStats(StorageType, uint64_t& total, uint64_t& used, uint64_t& free) { io(); free = control.freeBytes; total = free * 2; used = free; return true; }
static bool existsGuarded(const String&, const AuthContext&) { io(); return control.parentExists; }
static bool mkdirGuarded(const String&, const AuthContext&) { io(); ++metrics.mkdirs; return true; }
static File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create) {
  FsLockGuard guard("VFS.open"); io(); ++metrics.opens; metrics.openedUsers.emplace_back(ctx.user.c_str()); metrics.openedPaths.emplace_back(path.c_str());
  check(std::strcmp(mode, "w") == 0 && create, "upload still opens create/truncate under user auth");
  if (control.openFails) return {};
  if (control.temporaryOpenFails && path.endsWith(".upload.tmp")) return {};
  const std::string full = path.c_str();
  if (full.size() - full.rfind('/') - 1 > control.maxNameLength) return {};  // backend name limit
  auto node = std::make_shared<Node>(); files[path.c_str()] = node; return File(node);
}
// Staged promotion (System_VFS): the caller holds the lock and has closed the temporary.
static bool replaceFileGuarded(const String& from, const String& to, const AuthContext& ctx) {
  FsLockGuard guard("VFS.replace"); io(); check(liveHandles == 0, "close the staged file before promotion");
  ++metrics.promotions; metrics.promotedUsers.emplace_back(ctx.user.c_str());
  // renameGuarded's demands on a caller: RENAME on the source, CREATE on the destination,
  // and DELETE for the backup of an existing SD file. The system identity has them all.
  const bool sdBackup = getStorageType(to) == SDCARD && files.count(to.c_str());
  if (ctx.user != "system" && !(control.renameAllowed && control.createAllowed && (!sdBackup || control.deleteAllowed))) return false;
  if (control.replaceFails || !files.count(from.c_str())) return false;
  files[to.c_str()] = files.at(from.c_str()); files.erase(from.c_str()); return true;
}
static AuthContext systemAuth(const char* reason) {
  check(std::strcmp(reason, "web.upload") == 0 || std::strcmp(reason, "web.filewrite") == 0, "the writer's system identity names its writer");
  metrics.systemReasons.emplace_back(reason); AuthContext result; result.user = "system"; return result;
}
static bool exists(const String& path) { io(); return files.count(path.c_str()) != 0; }
static bool guardedNormalize(const String& in, String& out, const char*, const AuthContext&) { out = in; return true; }
// The destination's write right (WRITE on an existing file, CREATE on a new one): the
// same decision canWriteGuarded below takes, as the writer asks it before a system promotion.
static bool writePermitted(const String&, const AuthContext&) { io(); return control.writeAllowed; }
static bool removeGuarded(const String& path, const AuthContext& ctx) {
  FsLockGuard guard("VFS.remove"); io(); check(liveHandles == 0, "close file before removing failed partial");
  metrics.cleanupUsers.emplace_back(ctx.user.c_str());
  if (ctx.user == "system" ? !control.cleanupSystem : !control.cleanupUser) return false;
  return files.erase(path.c_str()) != 0;
}
// The editor's destination gate (the decision a write-mode openGuarded makes).
static bool canWriteGuarded(const String&, const AuthContext&) { io(); return control.writeAllowed; }
// The shared staged writer (System_VFS) is compiled here over these fakes.
// INSERT_STAGED_WRITE_DECL
// INSERT_STAGED_WRITE_IMPL
}
static void hookScope() {
  if (!buffers.empty() || liveHandles || (control.hooksOutsideLock && ownsFs != outerLock)) metrics.badHookResources = true;
}
static void runFileWritePostSaveHooks(const String&) { hookScope(); ++metrics.posthooks; }
static bool isMapFileByMagic(const String&) { hookScope(); ++metrics.mapProbes; return control.magicMap; }
static bool organizeMapFromAnyPath(const String&, String&) { hookScope(); ++metrics.mapHooks; return control.mapOrganization; }
static bool tryOrganizeLegacyWaypointsAtRoot(const String&, String&) { hookScope(); ++metrics.waypointHooks; return control.mapOrganization; }

#define free trackedFree
// INSERT_UPLOAD_HANDLER
// INSERT_FILE_WRITE_HANDLER
#undef free

static void reset() {
  for (void* buffer : buffers) std::free(buffer);
  buffers.clear(); files.clear(); control = {}; metrics = {}; filesystemReady = true;
  ownsFs = outerLock = false; pauseDepth = pauseCalls = resumeCalls = takes = gives = 0; liveHandles = 0;
}
static std::string encode(const std::string& input) {
  static const char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : input) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.') result += char(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
static std::string base64(const std::string& input) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (size_t pos = 0; pos < input.size(); pos += 3) {
    uint32_t value = uint32_t(uint8_t(input[pos])) << 16;
    if (pos + 1 < input.size()) value |= uint32_t(uint8_t(input[pos + 1])) << 8;
    if (pos + 2 < input.size()) value |= uint8_t(input[pos + 2]);
    result += table[(value >> 18) & 63]; result += table[(value >> 12) & 63];
    result += pos + 1 < input.size() ? table[(value >> 6) & 63] : '=';
    result += pos + 2 < input.size() ? table[value & 63] : '=';
  }
  return result;
}
static httpd_req_t request(const std::string& path = "/user/upload.txt", const std::string& bytes = "hello", bool binary = false) {
  httpd_req_t req;
  req.body = "path=" + encode(path) + "&binary=" + (binary ? "1" : "0") + "&content=" + encode(binary ? base64(bytes) : bytes);
  req.content_len = req.body.size(); return req;
}
// JavaScript encodeURIComponent: only A-Z a-z 0-9 - _ . ! ~ * ' ( ) stay literal; every other byte becomes %XX.
static std::string encodeURIComponent(const std::string& input) {
  static const char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : input) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("-_.!~*'()", c)) result += char(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
// The Files editor save: hw.postForm('/api/files/write', { name, content }).
static httpd_req_t editorRequest(const std::string& name, const std::string& bytes) {
  httpd_req_t req;
  req.body = "name=" + encodeURIComponent(name) + "&content=" + encodeURIComponent(bytes);
  req.content_len = req.body.size(); return req;
}
static void balanced(unsigned expectedPause = 0, bool expectedLock = false) {
  check(buffers.empty(), "upload buffers leaked");
  check(liveHandles == 0, "file handle leaked");
  check(pauseDepth == expectedPause, "polling pause leaked (depth=" + std::to_string(pauseDepth) + ")");
  check(ownsFs == expectedLock, "filesystem mutex ownership changed");
  check(pauseCalls - resumeCalls == expectedPause, "polling calls unbalanced");
  check(takes - gives == unsigned(expectedLock), "mutex calls unbalanced");
  check(!metrics.badIoScope && !metrics.badCloseScope, "filesystem work/close outside lock and polling pause");
  check(!metrics.badHookResources, "post-save hooks ran with upload resources retained");
}
static void failed(httpd_req_t& req, const char* reason) {
  check(handleFileUpload(&req) == ESP_OK, "handler response convention changed");
  check(req.response.find("\"success\":false") != std::string::npos && req.response.find(reason) != std::string::npos,
        "expected upload failure '" + std::string(reason) + "', got " + req.response);
  check(metrics.responses == 1, "one error response");
  check(metrics.posthooks == 0 && metrics.mapHooks == 0 && metrics.mapProbes == 0 && metrics.waypointHooks == 0, "failed upload ran post-save hooks");
  balanced();
}
static void succeeded(httpd_req_t& req, const std::string& path, const std::string& bytes) {
  check(handleFileUpload(&req) == ESP_OK, "handler response convention changed");
  check(req.response == "{\"success\":true}", "success response changed: " + req.response);
  check(files.count(path) && files.at(path)->bytes == bytes, "uploaded bytes differ");
  check(files.size() == 1, "staged temporary left behind after promotion");
  check(metrics.opens == 1 && metrics.closes == 1 && metrics.flushes == 1, "one complete file lifecycle");
  check(metrics.openedUsers == std::vector<std::string>{"alice"}, "opening preserves authenticated user");
  check(metrics.promotions == 1 && metrics.promotedUsers == std::vector<std::string>{"alice"},
        "staged copy promoted once under the authenticated user");
  check(metrics.cleanupUsers.empty() && metrics.posthooks == 1, "success hooks exactly once without cleanup");
  balanced();
}
static std::shared_ptr<Node> original() { return std::make_shared<Node>(Node{"original"}); }
// A /user/ path whose last component is exactly `bytes` long. The writer keeps "<name>.upload.tmp" within
// 63 bytes on LittleFS (the suffix adds 11) and stages longer names as /user/~stage.upload.tmp.
static std::string longName(size_t bytes) { return "/user/" + std::string(bytes - 4, 'n') + ".txt"; }
static void editorFailed(httpd_req_t& req, const char* reason) {
  check(handleFileWrite(&req) == ESP_OK, "editor handler response convention changed");
  check(req.response.find("\"success\":false") != std::string::npos && req.response.find(reason) != std::string::npos,
        "expected editor failure '" + std::string(reason) + "', got " + req.response);
  check(metrics.responses == 1 && metrics.posthooks == 0, "one error response and no post-save hooks");
  balanced();
}
static void editorSucceeded(httpd_req_t& req, const std::string& path, const std::string& bytes) {
  check(handleFileWrite(&req) == ESP_OK, "editor handler response convention changed");
  check(req.response == "{\"success\":true}", "editor success response changed: " + req.response);
  check(files.count(path), "editor saved under a different name");
  check(files.at(path)->bytes == bytes, "editor saved different bytes: " + files.at(path)->bytes);
  check(files.size() == 1, "staged temporary left behind after promotion");
  check(metrics.opens == 1 && metrics.closes == 1 && metrics.flushes == 1 && metrics.writes == (bytes.size() + 511) / 512, "one complete checked file lifecycle");
  check(metrics.openedUsers == std::vector<std::string>{"alice"} && metrics.promotedUsers == std::vector<std::string>{"alice"},
        "staged copy opened and promoted once under the authenticated user");
  check(metrics.cleanupUsers.empty() && metrics.posthooks == 1, "success hooks exactly once without cleanup");
  balanced();
}
struct Test { const char* name; const char* group; std::function<void()> run; };
int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  std::vector<Test> tests;
  auto add = [&](const char* name, const char* group, std::function<void()> test) { tests.push_back({name, group, std::move(test)}); };
  add("unauthenticated", "reject", [] { control.auth = false; auto req = request(); handleFileUpload(&req); check(metrics.allocations == 0 && metrics.opens == 0, "auth rejected before resources"); balanced(); });
  add("guest denied", "reject", [] { control.guestAllowed = false; auto req = request(); handleFileUpload(&req); check(metrics.allocations == 0 && metrics.opens == 0, "guest rejected before resources"); balanced(); });
  add("filesystem unavailable", "reject", [] { filesystemReady = false; auto req = request(); failed(req, "Filesystem not initialized"); check(metrics.allocations == 0, "readiness rejected before allocation"); });
  add("SD unavailable encoded query", "reject", [] { control.sd = false; auto req = request("/sd/upload.txt"); req.query = "path=%2Fsd%2Fupload.txt"; failed(req, "SD card not available"); check(metrics.allocations == 0, "SD rejected before allocation"); });
  add("size limit", "reject", [] { control.freeBytes = 10; auto req = request(); failed(req, "File too large"); check(metrics.allocations == 0, "size rejected before allocation"); });
  for (size_t allocation : {size_t(1), size_t(2)}) add(allocation == 1 ? "output allocation fails" : "receive allocation fails", "allocation", [allocation] { control.failAllocation = allocation; auto req = request(); failed(req, "Memory allocation failed"); check(metrics.frees == allocation - 1 && metrics.opens == 0, "allocation failure releases earlier resources"); });
  add("empty request body", "missing-content", [] { httpd_req_t req; failed(req, "Missing content"); check(metrics.opens == 0 && metrics.cleanupUsers.empty(), "missing body does not open or delete a destination"); });
  add("path without content field", "missing-content", [] { httpd_req_t req; req.body = "path=/user/upload.txt&binary=0"; req.content_len = req.body.size(); failed(req, "Missing content"); check(metrics.opens == 0 && metrics.cleanupUsers.empty(), "missing content does not open or delete a destination"); });
  add("empty path", "invalid-open", [] { auto req = request(""); failed(req, "Invalid path"); check(metrics.opens == 0 && metrics.cleanupUsers.empty(), "invalid path never creates or removes files"); });
  add("import denied", "invalid-open", [] { control.import = false; auto req = request(); failed(req, "Invalid path"); check(metrics.opens == 0 && metrics.cleanupUsers.empty(), "denied path never creates or removes files"); });
  add("admin path denied", "invalid-open", [] { control.admin = false; auto req = request("/system/secure.json"); failed(req, "Invalid path"); check(metrics.opens == 0, "admin-only check retained"); });
  add("open fails", "invalid-open", [] { control.openFails = true; files["/user/upload.txt"] = std::make_shared<Node>(Node{"original"}); auto req = request(); failed(req, "Invalid path"); check(files.at("/user/upload.txt")->bytes == "original" && metrics.cleanupUsers.empty(), "failed open must not delete existing target"); });
  add("disconnect before open", "disconnect", [] { control.disconnectAt = 0; auto req = request(); failed(req, "Recv error"); check(metrics.opens == 0 && metrics.cleanupUsers.empty(), "no cleanup before open"); });
  add("disconnect after open before flush", "disconnect", [] { control.disconnectAt = 60; auto req = request("/user/upload.txt", std::string(100, 'a')); failed(req, "Recv error"); check(metrics.writes == 0 && metrics.closes == 1 && files.empty(), "failed empty opened upload removed"); });
  add("disconnect after written chunk", "disconnect", [] { control.disconnectAt = 8192; auto req = request("/user/upload.txt", std::string(12000, 'a')); failed(req, "Recv error"); check(metrics.writes > 0 && files.empty(), "disconnected upload left partial file"); check(metrics.cleanupUsers == std::vector<std::string>{"alice"}, "cleanup uses original user"); });
  add("short write", "write", [] { control.shortWrite = true; auto req = request(); failed(req, "short write"); check(files.empty() && metrics.closes == 1, "short write removed and closed"); });
  add("storage exceeded", "write", [] { control.freeBytes = 5000; auto req = request("/user/upload.txt", std::string(5600, 'a')); failed(req, "Insufficient storage space"); check(files.empty() && metrics.writes == 2, "space exhaustion cleans streamed data"); });
  add("cleanup system fallback", "disconnect", [] { control.cleanupUser = false; control.disconnectAt = 8192; auto req = request("/user/upload.txt", std::string(12000, 'a')); failed(req, "Recv error"); check(files.empty() && metrics.cleanupUsers == std::vector<std::string>({"alice", "system"}), "cleanup uses same-user then established system fallback"); });
  add("both cleanup attempts fail", "disconnect", [] { control.cleanupUser = control.cleanupSystem = false; control.disconnectAt = 8192; auto req = request("/user/upload.txt", std::string(12000, 'a')); failed(req, "Recv error"); check(files.size() == 1 && metrics.cleanupUsers == std::vector<std::string>({"alice", "system"}), "cleanup refusal remains a failed upload"); });
  add("text plus and percent decoding", "text", [] { control.chunkSize = 1; auto req = request("/user/space name.txt", "a b%&+\r\nz"); auto at = req.body.find("a%20b"); req.body.replace(at, 5, "a+b"); req.content_len = req.body.size(); succeeded(req, "/user/space name.txt", "a b%&+\r\nz"); });
  add("text larger than write buffer", "text", [] { control.chunkSize = 73; std::string bytes(17000, 'x'); bytes.replace(4094, 5, "%\n+& "); auto req = request("/user/upload.txt", bytes); succeeded(req, "/user/upload.txt", bytes); check(metrics.writes > 4, "multiple output writes exercised"); });
  add("empty text", "text", [] { auto req = request("/user/upload.txt", ""); succeeded(req, "/user/upload.txt", ""); });
  add("parent creation", "text", [] { control.parentExists = false; auto req = request(); succeeded(req, "/user/upload.txt", "hello"); check(metrics.mkdirs == 1, "parent auto-creation preserved"); });
  add("receive timeout then success", "timeout", [] { control.timeouts = 1; auto req = request(); succeeded(req, "/user/upload.txt", "hello"); check(metrics.receives == 2, "one idle wait retried"); });
  add("single timeouts between chunks succeed", "timeout", [] { control.chunkSize = 16; control.timeoutAt = {{0, 1}, {16, 1}, {64, 1}}; auto req = request("/user/upload.txt", std::string(100, 'a')); succeeded(req, "/user/upload.txt", std::string(100, 'a')); check(metrics.receives >= 12, "idle waits separated by data are each retried"); });
  add("two consecutive timeouts before open abort", "timeout", [] { control.timeouts = 2; auto req = request(); failed(req, "Recv timeout"); check(metrics.receives == 2 && metrics.opens == 0 && metrics.cleanupUsers.empty(), "half-open socket abandoned before any file work"); });
  add("two consecutive timeouts after open release the partial", "timeout", [] { control.chunkSize = 16; control.timeoutAt = {{64, 2}}; auto req = request("/user/upload.txt", std::string(100, 'a')); failed(req, "Recv timeout"); check(metrics.opens == 1 && metrics.closes == 1 && files.empty() && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "abandoned upload closes and removes its partial under the file lock"); });
  add("re-upload keeps the previous content on disconnect", "replace", [] { files["/user/upload.txt"] = original(); control.disconnectAt = 8192; auto req = request("/user/upload.txt", std::string(12000, 'a')); failed(req, "Recv error"); check(files.size() == 1 && files.at("/user/upload.txt")->bytes == "original" && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "failed replacement must leave the destination untouched and remove only its staged copy"); });
  add("re-upload promotes over the previous content", "replace", [] { files["/user/upload.txt"] = original(); auto req = request("/user/upload.txt", "replacement"); succeeded(req, "/user/upload.txt", "replacement"); });
  add("promotion failure keeps the previous content", "replace", [] { files["/user/upload.txt"] = original(); control.replaceFails = true; auto req = request("/user/upload.txt", "replacement"); failed(req, "Could not replace destination"); check(files.size() == 1 && files.at("/user/upload.txt")->bytes == "original" && metrics.promotions == 1 && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "failed promotion leaves the destination and removes the staged copy"); });
  // An IMPORT|WRITE|CREATE grant without RENAME (/system/llm/): the upload promotes through the writer's system identity.
  add("upload without RENAME promotes as the writer", "replace", [] { files["/user/upload.txt"] = original(); control.renameAllowed = false; auto req = request("/user/upload.txt", "replacement"); check(handleFileUpload(&req) == ESP_OK && req.response == "{\"success\":true}", "an IMPORT|WRITE grant must still upload: " + req.response); check(files.size() == 1 && files.at("/user/upload.txt")->bytes == "replacement" && metrics.openedUsers == std::vector<std::string>{"alice"} && metrics.promotions == 1 && metrics.promotedUsers == std::vector<std::string>{"system"} && metrics.systemReasons == std::vector<std::string>{"web.upload"}, "temporary opened as the caller, promoted once by the upload's system identity"); check(metrics.cleanupUsers.empty() && metrics.posthooks == 1, "success hooks exactly once without cleanup"); balanced(); });
  // CREATE|IMPORT without WRITE over an existing file (/sd/g2_icon_animations/): no write right, so no promotion.
  add("upload without RENAME or a write right keeps the previous content", "replace", [] { files["/user/upload.txt"] = original(); control.renameAllowed = control.writeAllowed = false; auto req = request("/user/upload.txt", "replacement"); failed(req, "Could not replace destination"); check(files.size() == 1 && files.at("/user/upload.txt")->bytes == "original" && metrics.promotions == 0 && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "no promotion without a write right; the staged copy is removed as the caller"); });
  add("upload with RENAME promotes as the caller", "replace", [] { files["/user/upload.txt"] = original(); control.writeAllowed = false; auto req = request("/user/upload.txt", "replacement"); succeeded(req, "/user/upload.txt", "replacement"); });
  // The in-place fallback exists only for a temporary whose name cannot exist on the backend (LittleFS: 63 bytes, so a
  // destination name over 52). Any other refused temporary ends the upload with the destination untouched.
  add("long name stages under the short staging name", "replace", [] { control.maxNameLength = 63; const std::string path = longName(60); files[path] = original(); auto req = request(path, "hello"); succeeded(req, path, "hello"); check(metrics.openedPaths == std::vector<std::string>{"/user/~stage.upload.tmp"}, "a 60-byte name stages under the short name, never opening the destination"); });
  add("64-byte name on an image-formatted partition is staged", "replace", [] { control.maxNameLength = 64; const std::string path = longName(64); files[path] = original(); auto req = request(path, "hello"); succeeded(req, path, "hello"); check(metrics.openedPaths == std::vector<std::string>{"/user/~stage.upload.tmp"}, "a name the image takes is staged, not written in place"); });
  add("long name on a device-formatted partition is staged", "replace", [] { control.maxNameLength = 255; const std::string path = longName(200); files[path] = original(); auto req = request(path, "hello"); succeeded(req, path, "hello"); check(metrics.openedPaths == std::vector<std::string>{"/user/~stage.upload.tmp"}, "a 200-byte name is staged, not written in place"); });
  add("name that still fits with its staging suffix is staged", "replace", [] { control.maxNameLength = 63; const std::string path = longName(52); files[path] = original(); auto req = request(path, "hello"); succeeded(req, path, "hello"); check(metrics.openedPaths == std::vector<std::string>{path + ".upload.tmp"}, "a 52-byte name keeps the <name>.upload.tmp staging name"); });
  add("refused temporary on a short name keeps the previous content", "replace", [] { control.temporaryOpenFails = true; files["/user/upload.txt"] = original(); auto req = request("/user/upload.txt", "replacement"); failed(req, "Invalid path"); check(files.size() == 1 && files.at("/user/upload.txt")->bytes == "original" && metrics.opens == 1 && metrics.cleanupUsers.empty(), "a refused temporary must not fall back to truncating the destination"); });
  add("binary across percent/base64 chunks", "binary", [] { control.chunkSize = 1; const std::string bytes("\0\xff\xfe\x01\x02\x7f\x80+/%", 10); auto req = request("/user/upload.bin", bytes, true); succeeded(req, "/user/upload.bin", bytes); });
  add("large binary", "binary", [] { control.chunkSize = 101; std::string bytes; for (unsigned i = 0; i < 12001; ++i) bytes += char(i % 256); auto req = request("/user/upload.bin", bytes, true); succeeded(req, "/user/upload.bin", bytes); check(metrics.writes > 2, "binary output spans buffers"); });
  add("empty binary", "binary", [] { auto req = request("/user/upload.bin", "", true); succeeded(req, "/user/upload.bin", ""); });
  add("nested caller scopes success", "nested", [] { { PollPauseGuard poll; FsLockGuard lock("outer"); outerLock = true; auto req = request(); check(handleFileUpload(&req) == ESP_OK && req.response == "{\"success\":true}", "nested success response"); balanced(1, true); } outerLock = false; balanced(); });
  add("nested caller scopes denied path", "nested", [] { { PollPauseGuard poll; FsLockGuard lock("outer"); outerLock = true; control.import = false; auto req = request(); handleFileUpload(&req); check(req.response.find("Invalid path") != std::string::npos && metrics.opens == 0, "nested denied response"); balanced(1, true); } outerLock = false; balanced(); });
  add("nested caller scopes failure", "nested", [] { { PollPauseGuard poll; FsLockGuard lock("outer"); outerLock = true; control.shortWrite = true; auto req = request(); handleFileUpload(&req); check(req.response.find("short write") != std::string::npos && files.empty(), "nested failure cleanup"); balanced(1, true); } outerLock = false; balanced(); });
  add("map extension posthook", "maps", [] { auto req = request("/user/map.hwmap"); succeeded(req, "/user/map.hwmap", "hello"); check(metrics.mapHooks == ENABLE_WEB_MAPS && metrics.mapProbes == 0, "map extension hook follows feature flag"); });
  add("map magic posthook", "maps", [] { control.magicMap = true; auto req = request("/user/map.bin"); succeeded(req, "/user/map.bin", "hello"); check(metrics.mapHooks == ENABLE_WEB_MAPS && metrics.mapProbes == ENABLE_WEB_MAPS, "map magic hook follows feature flag"); });
  add("legacy waypoint posthook", "maps", [] { auto req = request("/maps/waypoints_map.json"); succeeded(req, "/maps/waypoints_map.json", "hello"); check(metrics.waypointHooks == ENABLE_WEB_MAPS && metrics.mapProbes == 0, "waypoint hook follows feature flag"); });
  add("map organizer refusal preserves upload result", "maps", [] { control.mapOrganization = false; auto req = request("/user/map.hwmap"); succeeded(req, "/user/map.hwmap", "hello"); check(metrics.mapHooks == ENABLE_WEB_MAPS, "map organizer attempted once when enabled"); });
  // /api/files/write (the Files editor save) through the same staged writer.
  auto editor = [&](const char* name, std::function<void()> test) { add(name, "filewrite", [test] { control.requirePause = control.hooksOutsideLock = false; test(); }); };
  const std::string alphabet = "a=b&c+d \xC3\xA9\t#%";  // the characters the old private decoder left as %XX
  editor("editor round-trips the encodeURIComponent alphabet byte-exact", [alphabet] { control.chunkSize = 7; const std::string name = "/user/" + alphabet; auto req = editorRequest(name, alphabet); editorSucceeded(req, name, alphabet); check(metrics.receives > 1, "chunked receive exercised"); });
  editor("editor replaces the previous content", [] { files["/user/notes.txt"] = original(); auto req = editorRequest("/user/notes.txt", std::string(1300, 'n') + "\r\n"); editorSucceeded(req, "/user/notes.txt", std::string(1300, 'n') + "\r\n"); });
  editor("editor saves empty content", [] { files["/user/notes.txt"] = original(); auto req = editorRequest("/user/notes.txt", ""); editorSucceeded(req, "/user/notes.txt", ""); });
  editor("editor literal plus decodes as a space", [] { auto req = editorRequest("/user/notes.txt", "a b"); req.body = "name=%2Fuser%2Fnotes.txt&content=a+b"; req.content_len = req.body.size(); editorSucceeded(req, "/user/notes.txt", "a b"); });
  editor("editor short write keeps the previous bytes", [] { files["/user/notes.txt"] = original(); control.shortWrite = true; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "short write"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original", "a failed save must leave the previous file intact"); check(metrics.closes == 1 && metrics.flushes == 0 && metrics.promotions == 0 && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "short write closes and removes only the staged copy"); });
  editor("editor promotion failure keeps the previous bytes", [] { files["/user/notes.txt"] = original(); control.replaceFails = true; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "Could not replace destination"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.promotions == 1 && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "failed promotion leaves the destination and removes the staged copy"); });
  editor("editor cleanup system fallback names its writer", [] { files["/user/notes.txt"] = original(); control.shortWrite = true; control.cleanupUser = false; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "short write"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.cleanupUsers == std::vector<std::string>({"alice", "system"}) && metrics.systemReasons == std::vector<std::string>{"web.filewrite"}, "cleanup uses the caller, then the system identity with the editor's reason"); });
  // A rule-table grant of WRITE without RENAME (/espnow/, /system/llm/) or CREATE without RENAME (/sd/g2_icon_animations/):
  // the save must still complete through the staged copy, promoted by the writer's system identity.
  editor("editor save without RENAME promotes as the writer", [] { files["/user/notes.txt"] = original(); control.renameAllowed = false; auto req = editorRequest("/user/notes.txt", "replacement"); check(handleFileWrite(&req) == ESP_OK && req.response == "{\"success\":true}", "a WRITE-only grant must still save: " + req.response); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "replacement", "staged copy promoted over the previous bytes"); check(metrics.opens == 1 && metrics.openedUsers == std::vector<std::string>{"alice"} && metrics.promotions == 1 && metrics.promotedUsers == std::vector<std::string>{"system"} && metrics.systemReasons == std::vector<std::string>{"web.filewrite"}, "temporary opened as the caller, promoted once by the editor's system identity"); check(metrics.cleanupUsers.empty() && metrics.posthooks == 1, "success hooks exactly once without cleanup"); balanced(); });
  editor("editor new file without RENAME promotes as the writer", [] { control.renameAllowed = false; auto req = editorRequest("/user/notes.txt", "hello"); check(handleFileWrite(&req) == ESP_OK && req.response == "{\"success\":true}", "a CREATE-only grant must still save a new file: " + req.response); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "hello" && metrics.promotedUsers == std::vector<std::string>{"system"}, "new file promoted by the writer"); balanced(); });
  editor("editor short write without RENAME keeps the previous bytes", [] { files["/user/notes.txt"] = original(); control.renameAllowed = false; control.shortWrite = true; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "short write"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.promotions == 0 && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "the staged path is kept for a WRITE-only grant, so a failed save still leaves the previous file"); });
  editor("editor promotion as the writer can still fail", [] { files["/user/notes.txt"] = original(); control.renameAllowed = false; control.replaceFails = true; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "Could not replace destination"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.promotedUsers == std::vector<std::string>{"system"} && metrics.cleanupUsers == std::vector<std::string>{"alice"}, "a failed system promotion leaves the destination and removes the staged copy as the caller"); });
  editor("editor with RENAME promotes as the caller", [] { files["/user/notes.txt"] = original(); control.deleteAllowed = false; auto req = editorRequest("/user/notes.txt", "replacement"); editorSucceeded(req, "/user/notes.txt", "replacement"); });
  editor("editor SD replacement without DELETE promotes as the writer", [] { files["/sd/notes.txt"] = original(); control.deleteAllowed = false; auto req = editorRequest("/sd/notes.txt", "replacement"); check(handleFileWrite(&req) == ESP_OK && req.response == "{\"success\":true}", "SD replacement response: " + req.response); check(files.size() == 1 && files.at("/sd/notes.txt")->bytes == "replacement" && metrics.promotedUsers == std::vector<std::string>{"system"}, "the FAT backup needs DELETE, so the writer promotes"); balanced(); });
  editor("editor SD new file without DELETE promotes as the caller", [] { control.deleteAllowed = false; auto req = editorRequest("/sd/notes.txt", "hello"); editorSucceeded(req, "/sd/notes.txt", "hello"); });
  editor("editor write denied before staging", [] { files["/user/notes.txt"] = original(); control.writeAllowed = false; auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "Writes to this path are not allowed"); check(metrics.opens == 0 && files.size() == 1 && files.at("/user/notes.txt")->bytes == "original", "denied destination never opens a temporary"); });
  editor("editor admin path denied", [] { control.admin = false; auto req = editorRequest("/system/secure.json", "x"); editorFailed(req, "Admin required"); check(metrics.opens == 0, "admin-only check retained"); });
  editor("editor long name stages under the short staging name", [] { control.maxNameLength = 63; const std::string path = longName(60); files[path] = original(); auto req = editorRequest(path, "hello"); editorSucceeded(req, path, "hello"); check(metrics.openedPaths == std::vector<std::string>{"/user/~stage.upload.tmp"}, "a 60-byte name stages under the short name, never opening the destination"); });
  editor("editor refused temporary on a short name keeps the previous bytes", [] { control.temporaryOpenFails = true; files["/user/notes.txt"] = original(); auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "Could not save: storage full or unavailable"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.opens == 1 && metrics.cleanupUsers.empty(), "a refused temporary must not fall back to truncating the destination"); });
  editor("editor refused temporary then short write keeps the previous bytes", [] { control.temporaryOpenFails = true; control.shortWrite = true; files["/user/notes.txt"] = original(); auto req = editorRequest("/user/notes.txt", "replacement"); editorFailed(req, "Could not save: storage full or unavailable"); check(files.size() == 1 && files.at("/user/notes.txt")->bytes == "original" && metrics.writes == 0 && metrics.cleanupUsers.empty(), "an in-place fallback would have truncated the destination and then removed it"); });
  editor("editor missing name", [] { httpd_req_t req; req.body = "content=abc"; req.content_len = req.body.size(); editorFailed(req, "Name required"); check(metrics.opens == 0, "no file work without a name"); });
  editor("editor rejects an empty body", [] { httpd_req_t req; editorFailed(req, "Invalid content length"); check(metrics.allocations == 0, "rejected before allocation"); });
  editor("editor two consecutive timeouts abort", [] { control.timeouts = 2; auto req = editorRequest("/user/notes.txt", "hello"); editorFailed(req, "Error receiving data"); check(metrics.receives == 2 && metrics.opens == 0 && metrics.frees == 1, "half-open socket abandoned before any file work"); });
  editor("editor unauthenticated", [] { control.auth = false; auto req = editorRequest("/user/notes.txt", "hello"); handleFileWrite(&req); check(metrics.allocations == 0 && metrics.opens == 0, "auth rejected before resources"); balanced(); });
  editor("editor filesystem unavailable", [] { filesystemReady = false; auto req = editorRequest("/user/notes.txt", "hello"); editorFailed(req, "Filesystem not initialized"); check(metrics.allocations == 0, "readiness rejected before allocation"); });
  unsigned failedCount = 0, ran = 0;
  for (const auto& test : tests) {
    if (selected != "all" && selected != test.group) continue;
    reset(); ++ran;
    try { test.run(); std::printf("PASS %s\n", test.name); }
    catch (const std::exception& error) { ++failedCount; std::printf("FAIL %s: %s\n", test.name, error.what()); }
  }
  reset();
  std::printf("%u upload lifecycle scenarios, %u failures (maps=%d)\n", ran, failedCount, ENABLE_WEB_MAPS);
  return failedCount ? 1 : 0;
}
