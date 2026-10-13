// Lock-discipline harness for the web file streaming paths.
//
// Compiles the shipping streamFileChunks helper, handleFileRead,
// streamHtmlEscapedChecked and the GPS track listing tail of
// handleGPSTracksAPI against an httpd double that records whether the
// filesystem mutex (or a sensor poll pause) was held inside every chunk send,
// and that can fail one send to prove each loop stops there. The File/VFS
// stand-in is deliberately tiny: this suite checks who holds which lock when,
// not persistence. String comes from the shared web_batch_stubs/Arduino.h.
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

#include "Arduino.h"

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
[[maybe_unused]] constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;  // only the checked escaper returns it
constexpr ssize_t HTTPD_RESP_USE_STRLEN = -3;
using TickType_t = uint32_t;

static int gFailures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++gFailures;
  }
}

static inline void logSink(const char*, ...) {}
#define DEBUG_STORAGEF(...) logSink(__VA_ARGS__)
#define ERROR_STORAGEF(...) logSink(__VA_ARGS__)
#define WARN_STORAGEF(...) logSink(__VA_ARGS__)
#define WARN_WEBF(...) logSink(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Filesystem mutex and sensor poll pause doubles (single httpd task).
// ---------------------------------------------------------------------------
static int gFsDepth = 0;        // live FsLockGuard scopes plus a raw fsLock()
static int gFsLockTakes = 0;
static int gPollDepth = 0;
static int gPollUnderflow = 0;

struct FsLockGuard {
  bool held;
  explicit FsLockGuard(const char* owner = nullptr) : held(true) {
    (void)owner;
    ++gFsDepth;
    ++gFsLockTakes;
  }
  FsLockGuard(const char* owner, TickType_t timeoutTicks) : FsLockGuard(owner) { (void)timeoutTicks; }
  ~FsLockGuard() { --gFsDepth; }
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};

// Production fsLock takes the mutex only when this task does not hold it, and
// fsUnlock gives it back whole; a raw unlock while a guard scope is live would
// double-release in production, so the double treats it as a failure.
void fsLock(const char* owner = nullptr) {
  (void)owner;
  if (gFsDepth == 0) {
    gFsDepth = 1;
    ++gFsLockTakes;
  }
}
void fsUnlock() {
  check(gFsDepth == 1, "fsUnlock with a guard scope still live or without the lock");
  gFsDepth = 0;
}

void pollPause(uint8_t bus = 0xFF) { (void)bus; ++gPollDepth; }
void pollResume(uint8_t bus = 0xFF) {
  (void)bus;
  if (gPollDepth == 0) ++gPollUnderflow;
  else --gPollDepth;
}
void delay(unsigned long) {}

// ---------------------------------------------------------------------------
// Tiny File/VFS stand-in: every filesystem touch requires the FS lock.
// ---------------------------------------------------------------------------
struct FakeNode {
  bool dir = false;
  std::string content;
  std::vector<std::string> children;
};
static std::map<std::string, FakeNode> gTree;
static int gFsTouchesWithoutLock = 0;
static int gClosed = 0;

static void requireFsLock() {
  if (gFsDepth == 0) ++gFsTouchesWithoutLock;
}

struct FileState {
  std::string path;
  const FakeNode* node = nullptr;
  size_t pos = 0;
  size_t nextChild = 0;
  bool open = true;
};

class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<FileState> s) : s_(std::move(s)) {}
  explicit operator bool() const { return s_ && s_->open; }
  size_t size() const { requireFsLock(); return s_->node->content.size(); }
  size_t readBytes(char* buf, size_t n) {
    requireFsLock();
    const std::string& c = s_->node->content;
    size_t left = c.size() - s_->pos;
    size_t take = left < n ? left : n;
    memcpy(buf, c.data() + s_->pos, take);
    s_->pos += take;
    return take;
  }
  size_t read(uint8_t* buf, size_t n) { return readBytes(reinterpret_cast<char*>(buf), n); }
  bool available() { requireFsLock(); return s_->pos < s_->node->content.size(); }
  String readStringUntil(char term) {
    requireFsLock();
    const std::string& c = s_->node->content;
    size_t end = c.find(term, s_->pos);
    std::string out = c.substr(s_->pos, end == std::string::npos ? std::string::npos : end - s_->pos);
    s_->pos = end == std::string::npos ? c.size() : end + 1;
    return String(out);
  }
  const char* path() const { return s_->path.c_str(); }
  bool isDirectory() const { requireFsLock(); return s_->node->dir; }
  File openNextFile() {
    requireFsLock();
    if (!s_->node->dir || s_->nextChild >= s_->node->children.size()) return File();
    const std::string& child = s_->node->children[s_->nextChild++];
    auto it = gTree.find(child);
    if (it == gTree.end()) return File();
    auto state = std::make_shared<FileState>();
    state->path = child;
    state->node = &it->second;
    return File(state);
  }
  void close() {
    requireFsLock();
    if (s_ && s_->open) {
      s_->open = false;
      ++gClosed;
    }
  }

 private:
  std::shared_ptr<FileState> s_;
};

struct AuthContext {
  String user;
  String ip;
};

namespace VFS {
inline bool isStagingName(const String& nameOrPath) {
  return nameOrPath.endsWith(".tmp") || nameOrPath.endsWith(".bak");
}
File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create = false) {
  (void)mode; (void)ctx; (void)create;
  requireFsLock();
  auto it = gTree.find(path.c_str());
  if (it == gTree.end()) return File();
  auto state = std::make_shared<FileState>();
  state->path = path.c_str();
  state->node = &it->second;
  return File(state);
}
}  // namespace VFS

// ---------------------------------------------------------------------------
// httpd double. Every chunk send records the lock and pause state it saw and
// can fail once (failAt, 1-based over every chunk call including the
// terminator) to prove the production loop stops there.
// ---------------------------------------------------------------------------
struct httpd_req_t {
  std::string query;
  int failAt = 0;
  int chunkCalls = 0;
  int callsAfterFailure = 0;
  bool failed = false;
  int terminators = 0;
  int lockHeldDuringSend = 0;
  int pollPausedDuringSend = 0;
  std::vector<std::string> chunks;
  std::string body;
  std::string contentType;
  std::string status;
  std::string plainResponse;
  std::map<std::string, std::string> headers;
};

esp_err_t httpd_resp_send_chunk(httpd_req_t* r, const char* buf, ssize_t len) {
  if (gFsDepth > 0) ++r->lockHeldDuringSend;
  if (gPollDepth > 0) ++r->pollPausedDuringSend;
  if (r->failed) ++r->callsAfterFailure;
  ++r->chunkCalls;
  if (r->failAt && r->chunkCalls == r->failAt) {
    r->failed = true;
    return ESP_FAIL;
  }
  if (!buf || len == 0) {
    ++r->terminators;
    return ESP_OK;
  }
  r->chunks.emplace_back(buf, static_cast<size_t>(len));
  r->body.append(buf, static_cast<size_t>(len));
  return ESP_OK;
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t* r, const char* s) {
  return httpd_resp_send_chunk(r, s, s ? static_cast<ssize_t>(strlen(s)) : 0);
}
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
  if (gFsDepth > 0) ++r->lockHeldDuringSend;
  r->plainResponse.assign(buf, len == HTTPD_RESP_USE_STRLEN ? strlen(buf) : static_cast<size_t>(len));
  return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) { r->contentType = type; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status) { r->status = status; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* key, const char* value) {
  r->headers[key] = value;
  return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
  if (r->query.empty() || r->query.size() + 1 > len) return ESP_FAIL;
  memcpy(buf, r->query.c_str(), r->query.size() + 1);
  return ESP_OK;
}
esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t len) {
  std::string q(qry), k(std::string(key) + "=");
  size_t at = q.rfind(k, 0);
  if (at != 0) {
    at = q.find("&" + k);
    if (at == std::string::npos) return ESP_FAIL;
    at += 1;
  }
  size_t start = at + k.size();
  size_t end = q.find('&', start);
  std::string value = q.substr(start, end == std::string::npos ? std::string::npos : end - start);
  if (value.size() + 1 > len) return ESP_FAIL;
  memcpy(val, value.c_str(), value.size() + 1);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Auth / path doubles for handleFileRead.
// ---------------------------------------------------------------------------
static bool gAuthOk = true;
bool filesystemReady = true;
AuthContext makeWebAuthCtx(httpd_req_t*) { return AuthContext{String("tester"), String("127.0.0.1")}; }
bool tgRequireAuth(AuthContext&) { return gAuthOk; }
bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return true; }
bool isAdminOnlyPath(const String&) { return false; }
bool isAdminUser(const String&) { return true; }
String urlDecode(const String& s) {
  std::string out;
  const char* p = s.c_str();
  for (size_t i = 0; p[i]; ++i) {
    if (p[i] == '%' && p[i + 1] && p[i + 2]) {
      char hex[3] = {p[i + 1], p[i + 2], 0};
      out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
      i += 2;
    } else if (p[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(p[i]);
    }
  }
  return String(out);
}

// INSERT_PRODUCTION_HERE

// The runner defines this as 0 when the sources carry no streamFileChunks /
// streamHtmlEscapedChecked (a tree from before the helper existed), so the
// handler and listing scenarios still compile and report their behavioural
// failures instead of the unit failing to build.
#ifndef HW1_STREAM_HELPER_PRESENT
#define HW1_STREAM_HELPER_PRESENT 1
#endif

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------
static std::string pattern(size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('a' + (i % 26)));
  return s;
}

static void addFile(const std::string& path, const std::string& content) {
  gTree[path].content = content;
  gTree[path].dir = false;
}
static void addDir(const std::string& path, std::vector<std::string> children) {
  gTree[path].dir = true;
  gTree[path].children = std::move(children);
}

static void resetCounters() {
  gFsDepth = 0;
  gFsLockTakes = 0;
  gPollDepth = 0;
  gPollUnderflow = 0;
  gFsTouchesWithoutLock = 0;
  gClosed = 0;
  gAuthOk = true;
}

static void checkQuiescent(const httpd_req_t& req, const char* scenario) {
  std::string tag(scenario);
  check(req.lockHeldDuringSend == 0, (tag + ": FS mutex held inside a chunk send").c_str());
  check(req.pollPausedDuringSend == 0, (tag + ": sensor polling paused inside a chunk send").c_str());
  check(gFsDepth == 0, (tag + ": FS mutex still held at the end").c_str());
  check(gPollDepth == 0, (tag + ": sensor polling still paused at the end").c_str());
  check(gPollUnderflow == 0, (tag + ": pollResume without a matching pollPause").c_str());
  check(gFsTouchesWithoutLock == 0, (tag + ": filesystem touched without the lock").c_str());
}

static void testReadStreams() {
  resetCounters();
  const std::string content = pattern(1300);
  addFile("/logs/a.txt", content);
  httpd_req_t req;
  req.query = "name=%2Flogs%2Fa.txt";
  check(handleFileRead(&req) == ESP_OK, "read: handler result");
  check(req.body == content, "read: body matches the file");
  check(req.chunks.size() == 3 && req.chunks[0].size() == 512 && req.chunks[2].size() == 276,
        "read: 512-byte chunks");
  check(req.terminators == 1, "read: exactly one terminating chunk");
  check(req.contentType == "text/plain; charset=utf-8", "read: content type");
  check(gClosed == 1, "read: file closed once");
  // Open, three reads, close: the lock is taken per step, never for the whole transfer.
  check(gFsLockTakes >= 5, "read: lock taken per filesystem step");
  checkQuiescent(req, "read");
}

static void testReadStopsOnSendError() {
  resetCounters();
  addFile("/logs/b.txt", pattern(5000));
  httpd_req_t req;
  req.query = "name=/logs/b.txt";
  req.failAt = 2;
  // ESP_FAIL is what httpd_uri.c uses to close the socket; ESP_OK would keep
  // the keep-alive session open behind an unterminated chunked body.
  check(handleFileRead(&req) == ESP_FAIL, "read-fail: handler returns ESP_FAIL so httpd closes the socket");
  check(req.chunkCalls == 2, "read-fail: loop ends at the failed send");
  check(req.callsAfterFailure == 0, "read-fail: nothing sent after the failure");
  check(req.terminators == 0, "read-fail: no terminator after a failed send");
  check(gClosed == 1, "read-fail: file still closed");
  checkQuiescent(req, "read-fail");
}

static void testReadTerminatorFails() {
  // The terminating chunk is the last send; when it fails the session is just
  // as unusable as after a failed body chunk.
  resetCounters();
  const std::string content = pattern(1300);
  addFile("/logs/c.txt", content);
  httpd_req_t req;
  req.query = "name=/logs/c.txt";
  req.failAt = 4;  // three body chunks, then the terminator
  check(handleFileRead(&req) == ESP_FAIL, "read-term-fail: handler returns ESP_FAIL after a failed terminator");
  check(req.body == content, "read-term-fail: the body itself went out");
  check(req.chunkCalls == 4 && req.callsAfterFailure == 0, "read-term-fail: nothing sent after the failure");
  check(req.terminators == 0, "read-term-fail: no second terminator");
  check(gClosed == 1, "read-term-fail: file closed");
  checkQuiescent(req, "read-term-fail");
}

static void testReadMissingAndRefused() {
  resetCounters();
  httpd_req_t req;
  req.query = "name=/logs/none.txt";
  check(handleFileRead(&req) == ESP_OK, "read-missing: handler result");
  check(req.plainResponse == "File not found", "read-missing: plain error body");
  check(req.chunkCalls == 0, "read-missing: no chunks");
  checkQuiescent(req, "read-missing");

  resetCounters();
  gAuthOk = false;
  httpd_req_t refused;
  refused.query = "name=/logs/a.txt";
  check(handleFileRead(&refused) == ESP_OK, "read-refused: handler result");
  check(refused.chunkCalls == 0 && refused.plainResponse.empty(), "read-refused: nothing sent by the handler");
  checkQuiescent(refused, "read-refused");
}

#if HW1_STREAM_HELPER_PRESENT
static void testEscapedStreaming() {
  resetCounters();
  std::string content;
  for (int i = 0; i < 300; ++i) content += "a<b>&\"c'";
  addFile("/logs/t.txt", content);
  std::string expected;
  for (int i = 0; i < 300; ++i) expected += "a&lt;b&gt;&amp;&quot;c&#39;";

  httpd_req_t req;
  File f;
  {
    FsLockGuard g("test");
    f = VFS::openGuarded("/logs/t.txt", "r", AuthContext{});
  }
  char buf[4096];
  size_t total = 0;
  int chunks = 0;
  check(streamFileChunks(&req, f, buf, sizeof(buf), true, "test", &total, &chunks) == ESP_OK,
        "escaped: helper result");
  check(req.body == expected, "escaped: body is HTML-escaped");
  check(total == content.size() && chunks == 1, "escaped: counts the file bytes read");
  check(gClosed == 1, "escaped: helper closes the file");
  checkQuiescent(req, "escaped");

  resetCounters();
  httpd_req_t failing;
  failing.failAt = 1;
  File g2;
  {
    FsLockGuard g("test");
    g2 = VFS::openGuarded("/logs/t.txt", "r", AuthContext{});
  }
  check(streamFileChunks(&failing, g2, buf, 64, true, "test", &total, &chunks) == ESP_FAIL,
        "escaped-fail: helper returns the send error");
  check(failing.chunkCalls == 1 && failing.callsAfterFailure == 0, "escaped-fail: stops at the first failed send");
  check(total == 0 && chunks == 0, "escaped-fail: nothing counted as sent");
  check(gClosed == 1, "escaped-fail: file closed after the failure");
  checkQuiescent(failing, "escaped-fail");
}

static void testHtmlEscapedCheckedStops() {
  // A run longer than the 512-byte staging buffer goes straight to the socket;
  // when that send fails the escaper must not emit the entity that follows.
  std::string text = pattern(700) + "<" + pattern(10);
  httpd_req_t ok;
  check(streamHtmlEscapedChecked(&ok, text.data(), text.size()) == ESP_OK, "escaper: result");
  check(ok.body == pattern(700) + "&lt;" + pattern(10), "escaper: output");
  httpd_req_t failing;
  failing.failAt = 1;
  check(streamHtmlEscapedChecked(&failing, text.data(), text.size()) == ESP_FAIL, "escaper-fail: returns the error");
  check(failing.chunkCalls == 1 && failing.callsAfterFailure == 0, "escaper-fail: no send after the failure");
  check(streamHtmlEscapedChecked(nullptr, text.data(), text.size()) == ESP_ERR_INVALID_ARG, "escaper: null request");
}
#endif  // HW1_STREAM_HELPER_PRESENT

static void buildGpsTree() {
  gTree.clear();
  addFile("/logging_captures/x.csv", "1,2,3,4,5,6,7,8,9,0\n");
  addFile("/logging_captures/2026-01-01/t.log", "gps: 1.0,2.0\n");
  addFile("/logging_captures/2026-01-01/n.txt", "#comment\nshort\n");
  addFile("/logging_captures/2026-01-01/t.csv.tmp", "1,2,3,4,5,6,7,8,9,0\n");
  addDir("/logging_captures/2026-01-01", {"/logging_captures/2026-01-01/t.log",
                                          "/logging_captures/2026-01-01/n.txt",
                                          "/logging_captures/2026-01-01/t.csv.tmp"});
  addDir("/logging_captures", {"/logging_captures/x.csv", "/logging_captures/2026-01-01"});
  addFile("/logging_captures/tracks/a.csv", "9,8,7,6,5,4,3,2,1,0\n");
  addDir("/logging_captures/tracks", {"/logging_captures/tracks/a.csv"});
}

static void testGpsListing() {
  resetCounters();
  buildGpsTree();
  AuthContext ctx{String("tester"), String("127.0.0.1")};
  httpd_req_t req;
  check(gpsTrackListing(&req, ctx) == ESP_OK, "gps: handler result");
  check(req.body ==
            "{\"success\":true,\"files\":["
            "{\"path\":\"/logging_captures/x.csv\",\"size\":20},"
            "{\"path\":\"/logging_captures/2026-01-01/t.log\",\"size\":13},"
            "{\"path\":\"/logging_captures/tracks/a.csv\",\"size\":20}]}",
        "gps: listing body");
  check(req.terminators == 1, "gps: terminating chunk");
  check(req.contentType == "application/json", "gps: content type");
  checkQuiescent(req, "gps");

  resetCounters();
  buildGpsTree();
  httpd_req_t failing;
  failing.failAt = 2;  // the first file entry; call 1 is the JSON prefix
  check(gpsTrackListing(&failing, ctx) == ESP_FAIL, "gps-fail: handler returns ESP_FAIL so httpd closes the socket");
  check(failing.chunkCalls == 2 && failing.callsAfterFailure == 0, "gps-fail: scan stops at the failed send");
  check(failing.terminators == 0, "gps-fail: no terminator after the failure");
  checkQuiescent(failing, "gps-fail");
}

int main() {
  testReadStreams();
  testReadStopsOnSendError();
  testReadTerminatorFails();
  testReadMissingAndRefused();
#if HW1_STREAM_HELPER_PRESENT
  testEscapedStreaming();
  testHtmlEscapedCheckedStops();
#else
  std::cerr << "FAIL: streamFileChunks / streamHtmlEscapedChecked are missing; "
               "escaped streaming scenarios not run\n";
  ++gFailures;
#endif
  testGpsListing();
  if (gFailures) {
    std::cerr << gFailures << " check(s) failed\n";
    return 1;
  }
  std::cout << "web file streaming lock discipline passed\n";
  return 0;
}
