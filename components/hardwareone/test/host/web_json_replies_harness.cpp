// Hand-built web replies and header storage, run against the production
// handler bodies that test_web_json_replies.py inserts:
//   handleAutomationsExport (?id= and bulk), handleIconGet (?debug=1),
//   handleFilesCreate, handleMicRecordingsList, and the shared
//   webJsonString / sendJsonError helpers where the tree has them.
// httpd_resp_set_hdr keeps the caller's pointer and httpd_resp_send reads it,
// as ESP-IDF does, so a header value that died before the send is a
// use-after-free under ASan. JSON payloads are printed as "<tag>\t<payload>"
// for the runner to parse; header checks fail here.
#include "Arduino.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#define ENABLE_MICROPHONE 1
#define PSRAM_JSON_DOC(name) JsonDocument name
#define DEBUG_HTTPF(...) ((void)0)

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr long HTTPD_RESP_USE_STRLEN = -1;
enum httpd_err_code_t { HTTPD_404_NOT_FOUND = 404, HTTPD_500_INTERNAL_SERVER_ERROR = 500 };

struct httpd_req_t {
  std::string query;
  std::string recvBody;
  size_t content_len = 0;
  std::vector<std::pair<std::string, const char*>> headerPointers;
  std::map<std::string, std::string> sentHeaders;
  std::string type, status, body;
  int sends = 0;
  int errorCode = 0;
};

esp_err_t httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) {
  req->headerPointers.emplace_back(key, value);  // not copied, as in ESP-IDF
  return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) { req->type = type; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t* req, const char* buf, long len) {
  for (const auto& header : req->headerPointers) req->sentHeaders[header.first] = std::string(header.second);
  req->body.assign(buf, len < 0 ? std::strlen(buf) : static_cast<size_t>(len));
  ++req->sends;
  return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t* req, httpd_err_code_t code, const char* msg) {
  req->errorCode = code;
  return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t* req, char* buf, size_t size) {
  if (req->query.empty() || req->query.size() + 1 > size) return ESP_FAIL;
  std::strcpy(buf, req->query.c_str());
  return ESP_OK;
}
esp_err_t httpd_query_key_value(const char* query, const char* key, char* val, size_t size) {
  std::string q(query), k = std::string(key) + "=";
  size_t pos = 0;
  while (pos <= q.size()) {
    size_t end = q.find('&', pos);
    if (end == std::string::npos) end = q.size();
    if (q.compare(pos, k.size(), k) == 0) {
      std::string value = q.substr(pos + k.size(), end - pos - k.size());
      if (value.size() + 1 > size) return ESP_FAIL;
      std::strcpy(val, value.c_str());
      return ESP_OK;
    }
    pos = end + 1;
  }
  return ESP_FAIL;
}
int httpd_req_recv(httpd_req_t* req, char* buf, size_t len) {
  const size_t n = std::min(len, req->recvBody.size());
  std::memcpy(buf, req->recvBody.data(), n);
  req->recvBody.erase(0, n);
  return static_cast<int>(n);
}
inline esp_err_t sendJsonResponse(httpd_req_t* req, const char* body, long len = HTTPD_RESP_USE_STRLEN) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

struct AuthContext { String user; };
AuthContext makeWebAuthCtx(httpd_req_t*) { AuthContext ctx; ctx.user = "admin"; return ctx; }
bool tgRequireAuth(const AuthContext&) { return true; }
bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return true; }
#define WEB_AUTH_OR_RETURN(req, ctx) \
  AuthContext ctx = makeWebAuthCtx(req); \
  if (!tgRequireAuth(ctx)) return ESP_OK; \
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK

String webJsonString(const char* text);
esp_err_t sendJsonError(httpd_req_t* req, const String& message);

// ---- automation export fixtures ----
static std::string gAutomations;
const char* AUTOMATIONS_JSON_FILE = "/system/automations.json";
bool readText(const char*, String& out) { out = gAutomations.c_str(); return true; }
bool extractArrayByKey(const String& src, const char* key, String& outArray) {
  JsonDocument doc;
  if (deserializeJson(doc, src.c_str())) return false;
  if (!doc[key].is<JsonArray>()) return false;
  std::string text;
  serializeJson(doc[key], text);
  outArray = text.c_str();
  return true;
}
bool extractArrayItem(const String& arrayStr, int& pos, String& outItem) {
  JsonDocument doc;
  if (deserializeJson(doc, arrayStr.c_str())) return false;
  JsonArray array = doc.as<JsonArray>();
  if (pos < 0 || static_cast<size_t>(pos) >= array.size()) return false;
  std::string text;
  serializeJson(array[static_cast<size_t>(pos)], text);
  outItem = text.c_str();
  pos += 1;
  return true;
}
bool parseJsonInt(const String& src, const char* key, int& out) {
  JsonDocument doc;
  if (deserializeJson(doc, src.c_str()) || !doc[key].is<int>()) return false;
  out = doc[key].as<int>();
  return true;
}
bool parseJsonString(const String& src, const char* key, String& out) {
  JsonDocument doc;
  if (deserializeJson(doc, src.c_str()) || !doc[key].is<const char*>()) return false;
  out = doc[key].as<const char*>();
  return true;
}
String redactAutomationsJsonForResponse(const String& json) { return json; }

// ---- icon fixtures ----
struct EmbeddedIcon {
  const char* name;
  const uint8_t* pngData;
  size_t pngSize;
};
static const uint8_t kPng[1234] = {0x89, 'P', 'N', 'G'};
static const EmbeddedIcon kIcon = {"probe", kPng, sizeof(kPng)};
const EmbeddedIcon* findEmbeddedIcon(const char* name) { return std::strcmp(name, "probe") == 0 ? &kIcon : nullptr; }
#define pgm_read_ptr(addr) (*(const void* const*)(addr))
#define pgm_read_dword(addr) (*(const size_t*)(addr))

// ---- file create fixtures ----
static std::string gLastCommand;
static bool gCommandOk = false;
static std::string gCommandOut;
String urlDecode(const String& s) {
  std::string out, in = s.c_str();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') out += ' ';
    else if (in[i] == '%' && i + 2 < in.size()) { out += static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
    else out += in[i];
  }
  return out.c_str();
}
bool isAdminOnlyPath(const String&) { return false; }
bool isAdminUser(const String&) { return true; }
bool executeUnifiedWebCommand(httpd_req_t*, AuthContext&, const String& cmd, String& out) {
  gLastCommand = cmd.c_str();
  out = gCommandOut.c_str();
  return gCommandOk;
}

// ---- recordings fixtures ----
static std::string gRecordings;
String getRecordingsList() { return gRecordings.c_str(); }
struct ExecIdentityGuard { explicit ExecIdentityGuard(const AuthContext&) {} };

// INSERT_PRODUCTION

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL\t%s\n", what); ++failures; }
}
static void emit(const char* tag, const std::string& payload) {
  std::string escaped;
  for (char c : payload) {
    if (c == '\\') escaped += "\\\\";
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (c == '\t') escaped += "\\t";
    else escaped += c;
  }
  std::printf("%s\t%s\n", tag, escaped.c_str());
}

static void caseExport() {
  gAutomations =
      "{\"automations\":["
      "{\"id\":7,\"name\":\"Wake \\\"up\\\"/now\\\\caf\\u00e9\\r\\nX-Evil: 1\"},"
      "{\"id\":8,\"name\":\"" + std::string(100, 'L') + "\"},"
      "{\"id\":9,\"name\":\"plain-name_1.v2\"}]}";
  struct { const char* id; const char* expected; } rows[] = {
      {"7", "attachment; filename=\"Wake__up__now_caf____X-Evil__1.json\""},
      {"8", nullptr},
      {"9", "attachment; filename=\"plain-name_1.v2.json\""},
  };
  for (const auto& row : rows) {
    httpd_req_t req;
    req.query = std::string("id=") + row.id;
    check(handleAutomationsExport(&req) == ESP_OK, "export returns ESP_OK");
    check(req.sends == 1, "export sends once");
    const std::string value = req.sentHeaders["Content-Disposition"];
    emit("EXPORT_HEADER", value);
    if (row.expected) check(value == row.expected, "export filename reduced to the safe set");
    else check(value == "attachment; filename=\"" + std::string(48, 'L') + ".json\"",
               "export filename capped at 48 characters");
    check(value.find_first_of("\r\n") == std::string::npos, "no line break in the header");
  }
  httpd_req_t bulk;
  check(handleAutomationsExport(&bulk) == ESP_OK && bulk.sends == 1, "bulk export sends");
  check(bulk.sentHeaders["Content-Disposition"].rfind("attachment; filename=\"automations-backup-", 0) == 0,
        "bulk export keeps its dated filename");
}

static void caseIcon() {
  httpd_req_t req;
  req.query = "name=probe&debug=1";
  check(handleIconGet(&req) == ESP_OK, "icon returns ESP_OK");
  check(req.sends == 1, "icon sends once");
  check(req.sentHeaders["X-Icon-Size"] == "1234", "debug size header read at the send");
  check(req.sentHeaders["X-Icon-Name"] == "probe", "debug name header");
  check(req.body.size() == 1234, "icon body");
}

static void caseFilesCreate() {
  const std::string error = "Error: Cannot create \"/a\\b\"\nsecond line\t\x01 end";
  for (const char* type : {"folder", "txt"}) {
    httpd_req_t req;
    req.recvBody = std::string("name=dir%2Fnote&type=") + type;
    gCommandOk = false;
    gCommandOut = error;
    check(handleFilesCreate(&req) == ESP_OK, "create returns ESP_OK");
    emit(std::strcmp(type, "folder") == 0 ? "CREATE_FOLDER_ERROR" : "CREATE_FILE_ERROR", req.body);
  }
  // A long name reaches the executor whole, as one quoted token.
  httpd_req_t req;
  const std::string name = "d/" + std::string(150, 'x');
  req.recvBody = "name=" + name + "&type=txt";
  gCommandOk = true;
  gCommandOut.clear();
  check(handleFilesCreate(&req) == ESP_OK, "long create returns ESP_OK");
  emit("CREATE_LONG_COMMAND", gLastCommand);
  check(gLastCommand == "filecreate \"/" + name + ".txt\"", "long file name is not truncated");
  check(req.body == "{\"success\":true}", "long create succeeds");
}

static void caseRecordings() {
  gRecordings =
      "plain.wav:10,take\\1.wav:20,a,b.wav:30,x:y.wav:40,q\"uote.wav:5,"
      "ctl\x01.wav:6,zero.wav:0,lead,0.wav:12,trunc.wa";
  httpd_req_t req;
  check(handleMicRecordingsList(&req) == ESP_OK, "recordings returns ESP_OK");
  emit("RECORDINGS", req.body);
  gRecordings.clear();
  httpd_req_t empty;
  check(handleMicRecordingsList(&empty) == ESP_OK, "empty recordings returns ESP_OK");
  emit("RECORDINGS_EMPTY", empty.body);
}

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "all";
  if (which == "export" || which == "all") caseExport();
  if (which == "icon" || which == "all") caseIcon();
  if (which == "files_create" || which == "all") caseFilesCreate();
  if (which == "recordings" || which == "all") caseRecordings();
  std::printf("DONE\t%d\n", failures);
  return failures ? 1 : 0;
}
