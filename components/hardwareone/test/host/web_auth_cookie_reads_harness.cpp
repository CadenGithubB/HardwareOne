// Runs the web request prologue as shipped: WEB_AUTH_OR_RETURN
// (WebServer_Utils.h), makeWebAuthCtx (WebServer_Server.h) and tgRequireAuth
// (System_User.cpp) with the production AuthContext, on the real Arduino
// String with its heap calls counted. The cookie reader, the session check
// (isAuthed), the client IP, the auth log and the two gates are doubles. The
// runner pastes the production text at the marker.
#include <Arduino.h>
#include "string_heap_count.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "web auth cookie reads check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
struct httpd_req_t {
  char uri[128];
  std::string cookie;  // the session cookie value; empty: no cookie
};

// ---------------------------------------------------------------------------
// Doubles. getCookieSid returns a new String per call, as WebServer_Utils.cpp
// does; isAuthed records the sid it was handed and answers by the scenario.
// ---------------------------------------------------------------------------
static struct {
  bool authed = true;
  unsigned cookieReads = 0;
  unsigned denials = 0;
  std::string sidSeenByIsAuthed;
  std::string logPath, logUser, logIp, logReason;
  bool logSuccess = false;
  unsigned logs = 0;
  unsigned long blocksAtGate = 0;  // String blocks made when the prologue ends
} sc;

String getCookieSid(httpd_req_t* req) {
  ++sc.cookieReads;
  return String(req->cookie.c_str());
}
void getClientIp(httpd_req_t*, String& ipOut) { ipOut = "client"; }
bool isAuthed(httpd_req_t*, const String& sid, String& outUser) {
  sc.sidSeenByIsAuthed = sid.c_str();
  if (sc.authed) outUser = "alice";
  return sc.authed;
}
// The two-argument form reads the cookie itself (WebServer_Server.cpp).
bool isAuthed(httpd_req_t* req, String& outUser) { return isAuthed(req, getCookieSid(req), outUser); }
void sendAuthRequiredResponse(httpd_req_t*) { ++sc.denials; }
template <typename Reason>
void logAuthAttempt(bool success, const char* path, const String& user, const String& ip,
                    const Reason& reason) {
  ++sc.logs;
  sc.logSuccess = success;
  sc.logPath = path;
  sc.logUser = user.c_str();
  sc.logIp = ip.c_str();
  sc.logReason = String(reason).c_str();
}
bool webPasswordChangeAllows(httpd_req_t*, const String&) { return true; }

// INSERT_PRODUCTION

bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) {
  sc.blocksAtGate = gStringHeap.made;
  return true;
}

static std::string seenSid, seenUser, seenPath;
static esp_err_t handler(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  seenSid = ctx.sid.c_str();
  seenUser = ctx.user.c_str();
  seenPath = ctx.path.c_str();
  return ESP_OK;
}

static httpd_req_t request(const char* uri, const char* cookie) {
  httpd_req_t req;
  std::snprintf(req.uri, sizeof(req.uri), "%s", uri);
  req.cookie = cookie;
  return req;
}

static void reset() {
  sc = {};
  seenSid.clear(); seenUser.clear(); seenPath.clear();
  gStringHeap = StringHeapCount{};
}

int main() {
  // A 32-character session id and a URI past the inline String capacity: each
  // copy of either is one heap block.
  static const char kSid[] = "0123456789abcdef0123456789abcdef";
  static const char kUri[] = "/api/sensors/status/with-updates";

  // A cookie session: the cookie is read once, by makeWebAuthCtx; isAuthed
  // and the handler see that value.
  reset();
  httpd_req_t req = request(kUri, kSid);
  check(handler(&req) == ESP_OK);
  check(sc.cookieReads == 1);
  check(sc.sidSeenByIsAuthed == kSid);
  check(seenSid == kSid && seenUser == "alice" && seenPath == kUri);
  check(sc.denials == 0);
  // The auth log line is unchanged: success, the URI, the user, the IP, no reason.
  check(sc.logs == 1 && sc.logSuccess && sc.logPath == kUri && sc.logUser == "alice" &&
        sc.logIp == "client" && sc.logReason.empty());
  // Two String blocks for the whole prologue: ctx.path and ctx.sid. Before,
  // isAuthed and tgRequireAuth each read the cookie again (four blocks).
  check(sc.blocksAtGate == 2);
  std::printf("cookie session: %u cookie read, %lu String blocks in the prologue\n",
              sc.cookieReads, sc.blocksAtGate);

  // No cookie (Basic Auth): one read, an empty sid everywhere.
  reset();
  req = request(kUri, "");
  check(handler(&req) == ESP_OK);
  check(sc.cookieReads == 1 && sc.sidSeenByIsAuthed.empty() && seenSid.empty());
  check(seenUser == "alice" && sc.logs == 1);

  // Refused: the denial is sent, nothing is logged, the handler body is not run.
  reset();
  sc.authed = false;
  req = request(kUri, kSid);
  check(handler(&req) == ESP_OK);
  check(sc.cookieReads == 1 && sc.sidSeenByIsAuthed == kSid);
  check(sc.denials == 1 && sc.logs == 0 && seenSid.empty() && sc.blocksAtGate == 0);

  std::printf("web auth cookie reads passed (%u checks)\n", checks);
  return 0;
}
