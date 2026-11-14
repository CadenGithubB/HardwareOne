// Host harness for the External Sensors table of the MQTT page.
//
// test_web_mqtt_sensor_rows.py inserts the production streamHtmlEscapedChecked
// and streamHtmlEscaped (WebServer_Utils.cpp) and streamMqttExternalSensorRow
// (WebPage_MQTT.cpp). Only String and the httpd chunk send are host
// boundaries. The send double refuses a zero-length chunk: on a chunked
// response that is the terminator, so one inside a row would end the page.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const std::string& text) : text_(text) {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
 private:
  std::string text_;
};

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr long HTTPD_RESP_USE_STRLEN = -1;

struct httpd_req_t {
  std::string body;
  int chunks = 0;
  int emptyChunks = 0;
};

static esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* buf, long len) {
  const size_t n = len < 0 ? std::strlen(buf) : static_cast<size_t>(len);
  if (n == 0) ++req->emptyChunks;
  req->body.append(buf, n);
  ++req->chunks;
  return ESP_OK;
}

esp_err_t streamHtmlEscapedChecked(httpd_req_t* req, const char* buf, size_t len);
void streamHtmlEscaped(httpd_req_t* req, const char* buf, size_t len);

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static const char kOpen[] = "<tr><td style=\"width:140px;font-weight:500;\">";
static const char kMid[] = "</td><td>";
static const char kAgeOpen[] = "</td><td style=\"width:80px;color:var(--panel-fg);font-size:0.85em;\">";

static std::string row(const std::string& name, const std::string& value, unsigned long age,
                       const char* what) {
  httpd_req_t req;
  streamMqttExternalSensorRow(&req, String(name), String(value), age);
  check(req.emptyChunks == 0, std::string(what) + ": no zero-length chunk ends the page");
  return req.body;
}

// Undo the five entities the escaper writes; anything else stays.
static std::string decoded(std::string text) {
  static const char* const pairs[][2] = {
      {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}};
  for (const auto& pair : pairs) {
    for (size_t at = text.find(pair[0]); at != std::string::npos; at = text.find(pair[0], at + 1)) {
      text.replace(at, std::strlen(pair[0]), pair[1]);
    }
  }
  return text;
}

// The two cells of a row, as the page carries them (still escaped).
static bool cells(const std::string& body, std::string& name, std::string& value) {
  if (body.compare(0, sizeof(kOpen) - 1, kOpen) != 0) return false;
  const size_t nameAt = sizeof(kOpen) - 1;
  const size_t mid = body.find(kMid, nameAt);
  if (mid == std::string::npos) return false;
  const size_t valueAt = mid + sizeof(kMid) - 1;
  const size_t age = body.find(kAgeOpen, valueAt);
  if (age == std::string::npos) return false;
  name = body.substr(nameAt, mid - nameAt);
  value = body.substr(valueAt, age - valueAt);
  return true;
}

int main() {
  // A clean row is the same bytes the page sent before.
  check(row("temperature", "21.5", 12, "clean") ==
            "<tr><td style=\"width:140px;font-weight:500;\">temperature</td><td>21.5</td>"
            "<td style=\"width:80px;color:var(--panel-fg);font-size:0.85em;\">12s ago</td></tr>",
        "a clean name and value render unchanged");

  // Markup in the topic segment or the payload is text, not markup.
  const std::string evilName = "<img src=x onerror=alert(1)>";
  const std::string evilValue = "</td></tr></table><script>alert('x')</script>&\"q\"";
  const std::string body = row(evilName, evilValue, 3, "markup");
  std::string name, value;
  check(cells(body, name, value), "row keeps its three cells around hostile text");
  for (const std::string& cell : {name, value}) {
    check(cell.find('<') == std::string::npos && cell.find('>') == std::string::npos &&
              cell.find('"') == std::string::npos && cell.find('\'') == std::string::npos,
          "no raw <, >, \" or ' inside a cell: " + cell);
  }
  check(decoded(name) == evilName, "the name reads back as the text that was received");
  check(decoded(value) == evilValue, "the value reads back as the text that was received");
  check(body.find("<script") == std::string::npos && body.find("<img") == std::string::npos,
        "no element from a payload reaches the page");

  // The value is cut at 100 received bytes, counted before escaping.
  const std::string longValue = std::string(99, 'v') + "<" + std::string(50, 'w');
  check(cells(row("n", longValue, 0, "long"), name, value), "long value: row shape");
  check(value == std::string(99, 'v') + "&lt;", "the first 100 bytes of a value are shown, escaped");

  // A payload with an embedded NUL is shown up to it, as %s showed it; an
  // empty value sends no chunk at all.
  check(cells(row("n", std::string("12\0<b>", 6), 0, "nul"), name, value) && value == "12",
        "a value is shown up to its first NUL");
  check(cells(row("n", "", 0, "empty"), name, value) && value.empty(), "an empty value renders an empty cell");
  check(cells(row("", "v", 0, "empty name"), name, value) && name.empty() && value == "v",
        "an empty name renders an empty cell");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d MQTT sensor row checks failed\n", failures, checks);
    return 1;
  }
  std::printf("MQTT sensor rows: %d checks passed\n", checks);
  return 0;
}
