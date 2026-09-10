// Reuse the established heap-capability backend mocks, not an allocator model.
// The renamed entrypoint also keeps its tests typechecked; this harness invokes
// its own cases against the same real System_MemUtil.cpp and PsramBuffer.
#define main memutilBackendTestsMain
#include "test_memutil.cpp"
#undef main
#include "System_CommandLimits.h"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

static size_t checks = 0;
static void check(bool condition, const char* why) {
  ++checks;
  if (!condition) throw std::runtime_error(why);
}
static void debugStub(const char*, ...) {}
#define DEBUG_STORAGEF(...) debugStub(__VA_ARGS__)
#define ERROR_STORAGEF(...) debugStub(__VA_ARGS__)
#define DEBUG_HTTPF(...) debugStub(__VA_ARGS__)
struct { unsigned getFreeHeap() const { return 123456; } } ESP;

// Minimal Arduino String boundary; production serializers and traversal are
// not rewritten in this fixture. Its allocation failure is independently
// injectable for the retained String compatibility adapter.
static bool failStringConcat = false;
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  char charAt(size_t i) const { return i < text_.size() ? text_[i] : 0; }
  char operator[](size_t i) const { return charAt(i); }
  bool reserve(size_t n) { text_.reserve(n); return true; }
  bool concat(const char* bytes, size_t n) {
    if (failStringConcat) return false;
    text_.append(bytes, n);
    return true;
  }
  bool startsWith(const String& prefix) const { return text_.rfind(prefix.text_, 0) == 0; }
  bool endsWith(const String& suffix) const {
    return text_.size() >= suffix.length() &&
        text_.compare(text_.size() - suffix.length(), suffix.length(), suffix.text_) == 0;
  }
  int indexOf(char c) const {
    const size_t pos = text_.find(c);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t i) const { return text_.substr(std::min(i, text_.size())); }
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  String& operator+=(char c) { text_ += c; return *this; }
  String& operator+=(int n) { text_ += std::to_string(n); return *this; }
  String& operator+=(unsigned int n) { text_ += std::to_string(n); return *this; }
  String& operator+=(unsigned long n) { text_ += std::to_string(n); return *this; }
  friend bool operator==(const String& a, const String& b) { return a.text_ == b.text_; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
  friend String operator+(const String& a, const String& b) { return a.text_ + b.text_; }
 private:
  std::string text_;
};

struct AuthContext { String user = "fixture-user"; };
static AuthContext auth;
static bool admin = false, authenticated = true, filesystemReady = true;
static const AuthContext& currentAuthContext() { return auth; }
static bool currentExecIsAdmin() { return admin; }
static bool isAdminUser(const String&) { return admin; }
static bool isAdminOnlyPath(const String& path) {
  return path == "/system" || path.startsWith("/system/");
}

static int fsDepth = 0, permissionViews = 0;
static size_t permissionConstructors = 0, metadataQueries = 0, directoryOpens = 0;
static size_t failAtMetadataQuery = SIZE_MAX;
static uint8_t toolbarMask = 7;
static std::vector<std::string> events;
static std::map<std::string, uint8_t> permissionMasks;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++fsDepth; }
  ~FsLockGuard() { --fsDepth; }
};
namespace FsInternal {
struct LockedListingPermissions {
  FsLockGuard guard{"permission-fixture"};
  explicit LockedListingPermissions(const AuthContext& ctx) {
    check(fsDepth > 0 && ctx.user == auth.user, "permission view captures caller under FS lock");
    ++permissionViews;
    ++permissionConstructors;
  }
  ~LockedListingPermissions() { --permissionViews; }
  uint8_t forPath(const String& path) const {
    check(fsDepth > 0 && permissionViews == 1, "entry permissions queried within one locked view");
    events.push_back("perms:" + std::string(path.c_str()));
    if (++metadataQueries == failAtMetadataQuery) gFailPsram = gFailInternal = true;
    const auto it = permissionMasks.find(path.c_str());
    return it == permissionMasks.end() ? 7 : it->second;
  }
  uint8_t forChildOf(const String& path) const {
    check(fsDepth > 0 && permissionViews == 1, "toolbar mask shares traversal's locked view");
    events.push_back("toolbar:" + std::string(path.c_str()));
    return toolbarMask;
  }
};
}

struct Node {
  std::string name;
  bool directory;
  size_t bytes;
  std::vector<std::shared_ptr<Node>> children;
};
static std::map<std::string, std::shared_ptr<Node>> nodes;
static std::set<std::string> denied;
static bool sdMounted = true;
struct File {
  std::shared_ptr<Node> node;
  size_t cursor = 0;
  explicit operator bool() const { return !!node; }
  bool isDirectory() const { return node && node->directory; }
  const char* name() const { return node ? node->name.c_str() : ""; }
  size_t size() const { return node ? node->bytes : 0; }
  File openNextFile() {
    check(fsDepth > 0, "directory cursor remains under FS lock");
    if (!node || cursor >= node->children.size()) return {};
    const auto child = node->children[cursor++];
    events.push_back("entry:" + child->name);
    return {child, 0};
  }
  void close() {}
};
namespace VFS {
struct VirtualEntry { const char* name; bool isFolder; };
static String normalize(const String& path) { return path.length() ? path : String("/"); }
static String stripSdPrefix(const String& path) {
  if (path == "/sd") return "/";
  return path.startsWith("/sd/") ? path.substring(3) : path;
}
static File openGuarded(const String& path, const char*, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "every VFS open remains guarded with caller identity");
  ++directoryOpens;
  events.push_back("open:" + std::string(path.c_str()));
  if (denied.count(path.c_str())) return {};
  const auto it = nodes.find(path.c_str());
  return it == nodes.end() ? File{} : File{it->second, 0};
}
static size_t listVirtualEntries(const String& path, VirtualEntry* out, size_t capacity) {
  if (path == "/" && sdMounted && capacity) { out[0] = {"sd", true}; return 1; }
  return 0;
}
}

// INSERT_PRODUCTION_LISTING_HERE

using esp_err_t = int;
static constexpr int ESP_OK = 0;
struct httpd_req_t {
  std::string query, status = "200 OK", response;
  std::function<void(const char*)> onSend;
};
#define WEB_AUTH_OR_RETURN(req, ctx) \
  if (!authenticated) { (req)->status = "401 Unauthorized"; return ESP_OK; } \
  const AuthContext& ctx = currentAuthContext()
static void broadcastOutput(const char*) {}
static int httpd_req_get_url_query_str(httpd_req_t* req, char* out, size_t size) {
  if (req->query.empty() || req->query.size() >= size) return -1;
  memcpy(out, req->query.c_str(), req->query.size() + 1);
  return ESP_OK;
}
static int httpd_query_key_value(const char* query, const char* key, char* out, size_t size) {
  const std::string prefix = std::string(key) + "=";
  const std::string q(query);
  if (q.rfind(prefix, 0) != 0) return -1;
  const std::string value = q.substr(prefix.size());
  if (value.size() >= size) return -1;
  memcpy(out, value.c_str(), value.size() + 1);
  return ESP_OK;
}
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static void sendJsonResponse(httpd_req_t* req, const char* response) {
  check(fsDepth == 0 && permissionViews == 0, "network send never holds traversal lock or role view");
  const std::string before(response);
  if (req->onSend) req->onSend(response);
  check(std::string(response) == before, "interleaved command cannot overwrite HTTP's source buffer");
  req->response = response;
}
// INSERT_PRODUCTION_HTTP_HERE

static void resetListing() {
  check(fsDepth == 0 && permissionViews == 0, "all previous lock/view scopes released");
  resetMocks();
  admin = false;
  authenticated = filesystemReady = sdMounted = true;
  failStringConcat = false;
  toolbarMask = 7;
  permissionConstructors = metadataQueries = directoryOpens = 0;
  failAtMetadataQuery = SIZE_MAX;
  events.clear(); permissionMasks.clear(); nodes.clear(); denied.clear();
}
static std::shared_ptr<Node> add(const char* path, bool directory, size_t bytes = 0,
                               const char* parent = nullptr, const char* display = nullptr) {
  auto node = std::make_shared<Node>(Node{display ? display : path, directory, bytes, {}});
  nodes[path] = node;
  if (parent) nodes.at(parent)->children.push_back(node);
  return node;
}
static void fixture() {
  resetListing();
  add("/", true);
  add("/sd", true);
  add("/sd/card.txt", false, 55, "/sd", "/card.txt");
  add("/alpha.txt", false, 123, "/");
  add("/folder", true, 0, "/");
  add("/folder/one", false, 1, "/folder");
  add("/folder/two", false, 2, "/folder");
  add("/denied", true, 0, "/");
  add("/denied/secret", false, 42, "/denied");
  denied.insert("/denied");
  permissionMasks["/denied"] = 0;
  add("/system", true, 0, "/");
  add("/system/private", false, 90, "/system");
  add("/bad/nested", false, 999, "/"); // malformed/nested names remain skipped
}
static JsonDocument parse(const char* text) {
  JsonDocument doc;
  check(!deserializeJson(doc, text), "response is valid complete JSON");
  return doc;
}
static void testInitialTransportOom() {
  fixture();
  gFailPsram = gFailInternal = true;
  check(std::string(filesListingJsonForApp("/")) == "Error: Out of memory building file listing" &&
        directoryOpens == 0, "initial command OOM returns allocation-independent error before traversal");
  httpd_req_t request;
  check(handleFilesList(&request) == ESP_OK && request.status == "500 Internal Server Error" &&
        directoryOpens == 0 && parse(request.response.c_str())["success"] == false,
        "initial HTTP OOM returns complete independent error before traversal");
}
static void testParity() {
  fixture();
  for (bool json : {false, true}) {
    for (bool hide : {false, true}) {
      String legacy;
      check(buildFilesListing("/", legacy, json, auth, hide), "retained public String listing works");
      PsramBuffer direct(SIZE_MAX, "listing.test");
      check(direct.reserve(1), "sink owns initial NUL storage");
      ListingPsramOutput sink(direct);
      check(buildFilesListingImpl("/", sink, json, auth, hide, nullptr), "PSRAM sink uses same walk");
      check(std::string(direct.c_str()) == legacy.c_str(), "text/array body byte parity with String adapter");
    }
  }
  events.clear();
  permissionConstructors = 0;
  PsramBuffer full(SIZE_MAX, "listing.full");
  check(buildFilesListJson("/", auth, true, full), "complete envelope succeeds");
  check(permissionConstructors == 1 && events.back() == "toolbar:/", "one role snapshot with toolbar query last");
  JsonDocument doc = parse(full.c_str());
  check(doc["success"] == true && doc["dirPerms"] == 7, "complete envelope schema preserved");
  auto files = doc["files"].as<JsonArray>();
  check(files.size() == 4, "admin and nested entries skipped, unreadable entry retained as metadata");
  check(files[0]["name"] == "sd" && files[0]["count"] == 1 && files[0]["size"] == "1 items",
        "synthetic SD mount remains first with numeric count and string display size");
  check(files[1]["name"] == "alpha.txt" && files[1]["size"] == "123 bytes" &&
        !files[1]["count"].is<int>(), "file schema has string size and no folder count");
  check(files[2]["count"] == 2 && files[3]["count"] == 0 && files[3]["perms"] == 0,
        "folder counts preserved and denied child opens show zero without filtering entry");
  String body;
  check(buildFilesListing("/", body, true, auth, true), "legacy array body builds");
  check(std::string(full.c_str()) == "{\"success\":true,\"dirPerms\":7,\"files\":[" +
        std::string(body.c_str()) + "]}", "complete envelope retains historical key order and exact bytes");
  String compatibility;
  check(buildFilesListJson("/", auth, true, compatibility) && compatibility == full.c_str(),
        "public String complete-envelope overload remains compatible");
  for (unsigned mask : {0u, 9u, 10u, 99u, 100u, 255u}) {
    toolbarMask = static_cast<uint8_t>(mask);
    check(buildFilesListJson("/", auth, true, full), "permission backfill succeeds for one to three digits");
    check(parse(full.c_str())["dirPerms"] == mask, "permission backfill does not corrupt files array");
  }
  events.clear(); permissionConstructors = 0;
  check(buildFilesListing("/", body, false, auth, true) && permissionConstructors == 0,
        "text listings create no role-view/permission metadata work");
  check(std::string(body.c_str()).find("  sd (1 items) [mount]\n") != std::string::npos &&
        std::string(body.c_str()).find("\nTotal: 4 entries") != std::string::npos,
        "text mount formatting and total retained");
  check(buildFilesListJson("/sd", auth, false, full), "SD-root listing succeeds");
  check(parse(full.c_str())["files"][0]["name"] == "card.txt", "SD-root prefix stripped correctly");
  add("/sd/deep", true, 0, "/sd", "/deep");
  add("/sd/deep/inside", false, 8, "/sd/deep", "/deep/inside");
  check(buildFilesListJson("/sd/deep", auth, false, full), "SD-subfolder listing succeeds");
  check(parse(full.c_str())["files"][0]["name"] == "inside", "SD subfolder prefix stripped correctly");
  add("/empty", true);
  check(buildFilesListing("/empty", body, false, auth, false) &&
        std::string(body.c_str()) == "Files (/empty):\n  No files found\n", "empty text listing unchanged");
  sdMounted = false;
  check(buildFilesListJson("/", auth, true, full) && parse(full.c_str())["files"][0]["name"] == "alpha.txt",
        "unavailable SD contributes no virtual mount");
}
static void testEscapingAndExactLimits() {
  resetListing(); sdMounted = false;
  add("/", true);
  const std::string name = "quote\"slash\\tab\tnewline\ncontrol\001";
  add(("/" + name).c_str(), false, 12, "/");
  PsramBuffer full(SIZE_MAX, "listing.test");
  check(buildFilesListJson("/", auth, false, full), "escaped name serializes");
  JsonDocument doc = parse(full.c_str());
  check(doc["files"][0]["name"].as<std::string>() == name, "all JSON-significant/control filename bytes round trip");
  const size_t exact = full.size() + 1;
  PsramBuffer exactFit(exact, "listing.exact"), shortFit(exact - 1, "listing.short");
  check(buildFilesListJson("/", auth, false, exactFit) && exactFit.size() + 1 == exact,
        "exact final capacity including NUL succeeds");
  check(!buildFilesListJson("/", auth, false, shortFit) &&
        shortFit.failure() == PsramBuffer::Failure::Limit, "one-byte-short final capacity is explicit limit failure");
}
static void largeFixture() {
  resetListing(); sdMounted = false;
  add("/", true);
  for (unsigned i = 0; i < 100; ++i) {
    const std::string path = "/file-" + std::to_string(i) + std::string(32, 'x');
    add(path.c_str(), false, i, "/");
  }
  add("/small", true);
  add("/small/other", false, 1, "/small");
}
static void testTransports() {
  largeFixture();
  const char* tooLarge = filesListingJsonForApp("/");
  check(std::string(tooLarge).find("Error: result too large for this transport") == 0,
        "command explicitly rejects listings outside existing 4096-byte envelope");
  httpd_req_t request;
  check(handleFilesList(&request) == ESP_OK && request.status == "200 OK" &&
        request.response.size() > CMD_RESULT_MAX, "HTTP listing is not capped by command transport");
  check(parse(request.response.c_str())["files"].size() == 100, "HTTP sends entire large listing without pagination");
  const std::string expected = request.response;
  const char* commandReply = filesListingJsonForApp("/small");
  const std::string commandBefore(commandReply);
  request.onSend = [&](const char* httpBytes) {
    check(httpBytes != commandReply, "HTTP and CLI buffers have independent ownership");
    const char* again = filesListingJsonForApp("/small");
    check(std::string(again) == commandBefore && std::string(httpBytes) == expected,
          "command interleaving during HTTP send retains both complete responses");
  };
  check(handleFilesList(&request) == ESP_OK && request.response == expected,
        "HTTP source lifetime spans send callback even with command execution");
  check(std::string(commandReply) == commandBefore, "subsequent HTTP call does not invalidate command response");
  request.onSend = {};
  add("/punctuation +&é", true);
  request.query = "path=%2Fpunctuation+%2B%26%C3%A9";
  check(handleFilesList(&request) == ESP_OK && parse(request.response.c_str())["success"] == true,
        "actual shared URL decoder preserves encoded punctuation, plus, and UTF-8 directory paths");
  request.query = "path=%2Fsystem";
  check(handleFilesList(&request) == ESP_OK && request.status == "403 Forbidden",
        "HTTP retains explicit admin-only path precheck and 403");
  check(std::string(filesListingJsonForApp("/system")) == "{\"success\":false,\"error\":\"Admin required\"}",
        "command retains admin-only path error envelope");
  add("/system", true);
  add("/system/private", false, 50, "/system");
  admin = true;
  request = {};
  request.query = "path=%2Fsystem";
  check(handleFilesList(&request) == ESP_OK && request.status == "200 OK" &&
        parse(request.response.c_str())["files"][0]["name"] == "private",
        "admin can still list an explicitly requested system directory");
  admin = false;
  request = {};
  request.query = "path=%2Fmissing";
  check(handleFilesList(&request) == ESP_OK && request.status == "200 OK" &&
        parse(request.response.c_str())["success"] == false, "ordinary missing-directory error status remains compatible");
  request = {};
  filesystemReady = false;
  check(handleFilesList(&request) == ESP_OK && parse(request.response.c_str())["success"] == false,
        "filesystem-not-ready envelope remains valid");
  filesystemReady = true; authenticated = false;
  request = {};
  const size_t opened = directoryOpens;
  check(handleFilesList(&request) == ESP_OK && request.status == "401 Unauthorized" &&
        directoryOpens == opened, "HTTP auth failure never opens a directory");
}
static void testFailureAndFallback() {
  fixture();
  PsramBuffer initial(SIZE_MAX, "listing.initial");
  gFailPsram = gFailInternal = true;
  check(!buildFilesListJson("/", auth, false, initial) && !initial.ok() && directoryOpens == 0,
        "initial OOM fails before filesystem walk and cannot publish partial success");
  const unsigned attempts = gDebugCalls;
  check(!initial.append("tail") && gDebugCalls == attempts, "allocation failure is sticky and cannot append/retry");
  initial.clear();
  gFailPsram = gFailInternal = false;
  check(buildFilesListJson("/", auth, false, initial), "next operation can reuse failed owner after explicit clear");
  largeFixture();
  PsramBuffer growth(SIZE_MAX, "listing.growth");
  failAtMetadataQuery = 3;
  check(!buildFilesListJson("/", auth, false, growth) &&
        growth.failure() == PsramBuffer::Failure::Allocation, "mid-list growth OOM is explicit");
  check(metadataQueries < 100 && events.back().find("toolbar:") != 0,
        "failed output stops traversal without false toolbar-success query");
  check(fsDepth == 0 && permissionViews == 0, "mid-list failure releases all locks/views");
  fixture();
  httpd_req_t request;
  failAtMetadataQuery = 3;
  check(handleFilesList(&request) == ESP_OK && request.status == "500 Internal Server Error" &&
        parse(request.response.c_str())["success"] == false &&
        request.response.find("files") == std::string::npos, "HTTP growth OOM sends independent error, never partial listing");
  fixture();
  gFailPsram = true;
  PsramBuffer fallback(SIZE_MAX, "listing.fallback");
  check(buildFilesListJson("/", auth, false, fallback) && !esp_ptr_external_ram(fallback.data()),
        "PSRAM exhaustion falls back to internal RAM with complete JSON");
  check(parse(fallback.c_str())["success"] == true, "fallback serialization is valid");
  fixture();
  gPsramTotal = 0;
  PsramBuffer absent(SIZE_MAX, "listing.no_psram");
  check(buildFilesListJson("/", auth, false, absent) && !esp_ptr_external_ram(absent.data()),
        "board without registered PSRAM still lists normally");
  fixture();
  String compatibility;
  failStringConcat = true;
  check(!buildFilesListJson("/", auth, false, compatibility) &&
        std::string(compatibility.c_str()) == "{\"success\":false,\"error\":\"Out of memory\"}",
        "legacy String adapter reports copy allocation failure explicitly");
  failStringConcat = false;
  check(!buildFilesListJson("/missing", auth, false, compatibility) &&
        parse(compatibility.c_str())["success"] == false, "legacy String overload preserves missing-directory false/error");
}
int main() {
  testInitialTransportOom();
  testParity();
  testEscapingAndExactLimits();
  testTransports();
  testFailureAndFallback();
  printf("Filesystem PSRAM listings: %zu checks passed (%s)\n", checks,
         hasPSRAMAvail() ? "PSRAM capable" : "no PSRAM build");
}
