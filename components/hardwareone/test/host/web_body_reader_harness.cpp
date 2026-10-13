// Template compiled by test_web_body_reader.py, never as a separate target.
// The production readers replace the marker below. The mocks script the httpd
// receive results and the allocator; they do not reimplement the reader.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}

using esp_err_t = int;
static constexpr int ESP_OK = 0, ESP_FAIL = -1;
static constexpr int HTTPD_RESP_USE_STRLEN = -1;
static constexpr int HTTPD_SOCK_ERR_FAIL = -1;
static constexpr int HTTPD_SOCK_ERR_TIMEOUT = -3;
enum class AllocPref { PreferPSRAM };
#define DEBUG_HTTPF(...) ((void)0)
#define ERROR_MEMORYF(...) ((void)0)

struct httpd_req_t {
  size_t content_len = 0;
  std::string wire;          // bytes the peer actually sent
  size_t consumed = 0;
  std::vector<int> script;   // per recv call: bytes to deliver (>0) or an error code (<=0)
  size_t scriptIndex = 0;
  int recvCalls = 0;
  std::string status = "200 OK", contentType, output;
  int sends = 0;
};

static size_t allocCalls = 0, lastAllocSize = 0;
static bool allocFail = false;
static void* ps_alloc(size_t size, AllocPref, const char*) {
  ++allocCalls;
  lastAllocSize = size;
  if (allocFail) return nullptr;
  return std::malloc(size);
}
static int httpd_req_recv(httpd_req_t* req, char* destination, size_t length) {
  ++req->recvCalls;
  check(destination != nullptr, "reader never receives into a null buffer");
  if (req->scriptIndex >= req->script.size())
    throw std::runtime_error("reader kept calling recv after the scripted results ran out");
  const int step = req->script[req->scriptIndex++];
  if (step <= 0) return step;
  const size_t count = std::min({static_cast<size_t>(step), length, req->wire.size() - req->consumed});
  std::memcpy(destination, req->wire.data() + req->consumed, count);
  req->consumed += count;
  return static_cast<int>(count);
}
static void httpd_resp_set_type(httpd_req_t* req, const char* type) { req->contentType = type; }
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static esp_err_t httpd_resp_send(httpd_req_t* req, const char* body, ssize_t) {
  req->output = body ? body : "";
  ++req->sends;
  return ESP_OK;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t* req, const char* body) {
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// INSERT_PRODUCTION_READERS_HERE

static httpd_req_t request(size_t contentLen, std::string wire, std::vector<int> script) {
  httpd_req_t req;
  req.content_len = contentLen;
  req.wire = std::move(wire);
  req.script = std::move(script);
  allocCalls = 0; lastAllocSize = 0; allocFail = false;
  return req;
}

static void testRecvIntoBuffer() {
  char buf[16];
  size_t len = 99;
  auto req = request(0, "", {});
  check(webRecvRequestBody(&req, buf, sizeof(buf), &len) == WebBodyStatus::Empty && len == 0 &&
        buf[0] == '\0' && req.recvCalls == 0, "Content-Length 0 is Empty without a receive");
  req = request(16, "", {});
  check(webRecvRequestBody(&req, buf, sizeof(buf), &len) == WebBodyStatus::TooLarge && req.recvCalls == 0,
        "a body that leaves no room for the NUL is TooLarge before any receive");
  req = request(10, "0123456789", {4, 6});
  std::memset(buf, 'z', sizeof(buf));
  check(webRecvRequestBody(&req, buf, sizeof(buf), &len) == WebBodyStatus::Ok && len == 10 &&
        std::strcmp(buf, "0123456789") == 0 && req.recvCalls == 2, "partial receives are joined and terminated");
  req = request(10, "0123456789", {HTTPD_SOCK_ERR_TIMEOUT, 4, HTTPD_SOCK_ERR_TIMEOUT, 6});
  check(webRecvRequestBody(&req, buf, sizeof(buf), &len) == WebBodyStatus::Ok && len == 10 && req.recvCalls == 4,
        "a single idle timeout is retried and data resets the count");
  req = request(10, "0123456789", {4, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT});
  check(webRecvRequestBody(&req, buf, sizeof(buf), &len) == WebBodyStatus::RecvFailed && len == 0 &&
        buf[0] == '\0' && req.recvCalls == 3, "two idle timeouts in a row end the read and clear the buffer");
  req = request(10, "", {HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT});
  check(webRecvRequestBody(&req, buf, sizeof(buf)) == WebBodyStatus::RecvFailed && req.recvCalls == 2,
        "a peer that never sends is given up after exactly two timeouts");
  req = request(10, "0123456789", {4, HTTPD_SOCK_ERR_FAIL});
  check(webRecvRequestBody(&req, buf, sizeof(buf)) == WebBodyStatus::RecvFailed && req.recvCalls == 2,
        "a socket error ends the read at once");
  req = request(10, "01234", {5, 0});
  check(webRecvRequestBody(&req, buf, sizeof(buf)) == WebBodyStatus::RecvFailed,
        "a short body is a failure, never a truncated success");
  check(webRecvRequestBody(&req, nullptr, sizeof(buf)) == WebBodyStatus::NoMemory &&
        webRecvRequestBody(&req, buf, 0) == WebBodyStatus::NoMemory, "a missing buffer is refused");
}

static void testReadAllocated() {
  auto req = request(0, "", {});
  WebBodyBuffer body = webReadRequestBody(&req, 100, "test");
  check(body.status == WebBodyStatus::Empty && !body.ok() && !body.data && body.len == 0 &&
        std::strcmp(body.c_str(), "") == 0 && allocCalls == 0, "Content-Length 0 allocates nothing");
  req = request(100000000, "", {});
  body = webReadRequestBody(&req, 100, "test");
  check(body.status == WebBodyStatus::TooLarge && !body.data && allocCalls == 0 && req.recvCalls == 0,
        "a Content-Length over the cap is refused before allocation or receive");
  req = request(100, std::string(100, 'a'), {100});
  body = webReadRequestBody(&req, 100, "test");
  check(body.ok() && body.len == 100 && lastAllocSize == 101 && std::strlen(body.c_str()) == 100 &&
        allocCalls == 1, "a body exactly at the cap is read whole into a cap+1 buffer");
  req = request(50, "", {});
  allocFail = true;
  body = webReadRequestBody(&req, 100, "test");
  check(body.status == WebBodyStatus::NoMemory && !body.data && allocCalls == 1 && req.recvCalls == 0,
        "a failed allocation is reported and nothing is received into it");
  req = request(50, "", {HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT});
  body = webReadRequestBody(&req, 100, "test");
  check(body.status == WebBodyStatus::RecvFailed && !body.data && body.len == 0 && req.recvCalls == 2,
        "a stalled body releases the buffer after two timeouts");
  const size_t allocsBefore = allocCalls;
  body = webReadRequestBody(nullptr, 100, "test");
  check(body.status == WebBodyStatus::Empty && allocCalls == allocsBefore, "a null request reads nothing");
}

static void testErrorReplies() {
  struct Case { WebBodyStatus status; const char* code; esp_err_t result; int sends; };
  const Case cases[] = {
      {WebBodyStatus::Ok, "200", ESP_OK, 0},
      {WebBodyStatus::Empty, "400", ESP_OK, 1},
      {WebBodyStatus::TooLarge, "413", ESP_FAIL, 1},
      {WebBodyStatus::NoMemory, "413", ESP_FAIL, 1},
      {WebBodyStatus::RecvFailed, "400", ESP_FAIL, 1},
  };
  for (bool json : {false, true}) {
    for (const Case& c : cases) {
      httpd_req_t req;
      const esp_err_t result = webSendBodyReadError(&req, c.status, json);
      check(result == c.result, "reply helper returns the handler's result code");
      check(req.status.rfind(c.code, 0) == 0 && req.sends == c.sends, "reply helper sends the mapped status once");
      if (c.sends == 0) continue;
      if (json) {
        check(req.contentType == "application/json" &&
              req.output.rfind("{\"success\":false,\"error\":\"", 0) == 0 &&
              req.output.size() > 2 && req.output.compare(req.output.size() - 2, 2, "\"}") == 0,
              "JSON replies keep the success/error contract");
      } else {
        check(req.contentType == "text/plain" && !req.output.empty(), "text replies are plain text");
      }
    }
  }
}

static void testReadPostBody() {
  char body[256];
  auto req = request(0, "", {});
  check(!readPostBody(&req, body, sizeof(body)) && req.recvCalls == 0, "LLM reader rejects an empty body");
  req = request(256, "", {});
  check(!readPostBody(&req, body, sizeof(body)) && req.recvCalls == 0,
        "LLM reader rejects a body that does not fit the buffer");
  req = request(9, "{\"a\":1}  ", {9});
  check(readPostBody(&req, body, sizeof(body)) && std::strcmp(body, "{\"a\":1}  ") == 0,
        "LLM reader fills and terminates the caller's buffer");
  req = request(10, "", {HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT});
  check(!readPostBody(&req, body, sizeof(body)) && req.recvCalls == 2,
        "LLM reader gives up after two idle timeouts instead of retrying forever");
}

int main() {
  try {
    testRecvIntoBuffer();
    testReadAllocated();
    testErrorReplies();
    testReadPostBody();
    std::cout << "web body reader tests passed (" << assertions
              << " assertions; production readers, scripted httpd socket)\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << "web body reader failure: " << failure.what() << '\n';
    return 1;
  }
}
