#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <strings.h>
#include "WebAssets_Generated.h"
using esp_err_t = int;
static constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = -2;
static constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
  std::map<std::string, std::string> incoming, outgoing;
  std::string status, type, body;
  std::string failedHeader;
  bool typeFailure = false;
  int sendResult = ESP_OK;
  int sends = 0;
};
static esp_err_t httpd_resp_set_hdr(httpd_req_t* req, const char* name, const char* value) {
  if (req->failedHeader == name) return ESP_FAIL;
  req->outgoing[name] = value; return ESP_OK;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t* req, const char* name) {
  auto it = req->incoming.find(name); return it == req->incoming.end() ? 0 : it->second.size();
}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* req, const char* name, char* out, size_t size) {
  auto it = req->incoming.find(name); if (it == req->incoming.end()) return ESP_ERR_NOT_FOUND;
  if (it->second.size() >= size) return ESP_FAIL;
  std::memcpy(out, it->second.c_str(), it->second.size() + 1); return ESP_OK;
}
static esp_err_t httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
static esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) {
  if (req->typeFailure) return ESP_FAIL;
  req->type = type; return ESP_OK;
}
static esp_err_t httpd_resp_send(httpd_req_t* req, const char* body, int size) {
  ++req->sends; req->body.assign(body, size < 0 ? std::strlen(body) : static_cast<size_t>(size));
  return req->sendResult;
}
// ACTUAL_HANDLER
static void expectAccepted(const char* value) {
  httpd_req_t req;
  if (value) req.incoming["Accept-Encoding"] = value;
  assert(handleEspNowCoreAsset(&req) == ESP_OK);
  assert(req.sends == 1 && req.status.empty());
  assert(req.type == "application/javascript; charset=utf-8");
  assert(req.outgoing["Content-Encoding"] == "gzip");
  assert(req.outgoing["Cache-Control"] == "no-store");
  assert(req.outgoing["Vary"] == "Accept-Encoding");
  assert(req.outgoing["X-Content-Type-Options"] == "nosniff");
  assert(req.body.size() == hw1::web_assets::kEspNowCoreGzipSize);
  assert(!std::memcmp(req.body.data(), hw1::web_assets::kEspNowCoreGzip, req.body.size()));
}
static void expectRejected(const char* value) {
  httpd_req_t req; req.incoming["Accept-Encoding"] = value;
  assert(handleEspNowCoreAsset(&req) == ESP_OK);
  assert(req.sends == 1 && req.status == "406 Not Acceptable");
  assert(req.outgoing.count("Content-Encoding") == 0);
  assert(req.body.find("requires gzip") != std::string::npos);
}
int main() {
  for (const char* value : {static_cast<const char*>(nullptr), "gzip", "br, gzip, deflate", "GZIP; q=1.000", "gzip;q=0.001", "*", "*;q=0.7", "identity;q=0, gzip;q=1", "gzip; q=1."}) expectAccepted(value);
  for (const char* value : {"", "identity", "br, deflate", "gzip;q=0", "gzip;q=0.000, *;q=1", "*;q=0", "gzip;q=0.0001", "gzip;q=1.1", "gzip;q=banana", "gzip;q=", "gzip;x=1", "gzip;q=0,gzip;q=1", "xgzip", "gzipfoo"}) expectRejected(value);
  expectRejected(std::string(256, 'a').c_str());
  for (const char* name : {"Cache-Control", "Vary", "X-Content-Type-Options", "Content-Encoding"}) {
    httpd_req_t req; req.failedHeader = name;
    assert(handleEspNowCoreAsset(&req) == ESP_FAIL && req.sends == 0);
  }
  httpd_req_t req; req.typeFailure = true;
  assert(handleEspNowCoreAsset(&req) == ESP_FAIL && req.sends == 0);
  req = {}; req.sendResult = -42;
  assert(handleEspNowCoreAsset(&req) == -42 && req.sends == 1);
  std::puts("Shipping gzip asset handler: encoding, exact body, headers and failures PASS");
}
