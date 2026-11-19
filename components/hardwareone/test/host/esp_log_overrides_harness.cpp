// Executes the shipping esplog core (System_EspLogCore.h, included as is) and
// the shipping apply/command block from System_Debug.cpp (inserted by
// test_esp_log_overrides.py). esp_log_level_set and the debug-settings
// persistence request are recorded boundaries; CommandArgs is a whitespace
// splitter (no quotes are used here).
#include "Arduino.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_EspLogCore.h"
#include "System_RadioLogCore.h"

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

static std::vector<std::string> events;
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static const char* const kEspNames[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
void esp_log_level_set(const char* tag, esp_log_level_t level) {
  events.push_back(std::string("esp:") + tag + "=" + kEspNames[level]);
}
#define CONFIG_LOG_MAXIMUM_LEVEL 3

struct HostSettings { String espLogLevels; } gSettings;
// The esplog lock (System_Debug.cpp espLogLock): taken once per save, never
// nested, released before the debug.json write.
static int gLockDepth = 0, gLockTakes = 0;
static void espLogLock() {
  CHECK(gLockDepth == 0);
  ++gLockDepth;
  ++gLockTakes;
}
static void espLogUnlock() {
  CHECK(gLockDepth == 1);
  --gLockDepth;
}
bool requestDebugSettingsPersist() {
  CHECK(gLockDepth == 0);
  events.push_back("persist");
  return true;
}

class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    std::string text(input.c_str());
    const auto first = text.find_first_not_of(" \t\r\n");
    raw_ = first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    size_t pos = 0;
    while (pos < raw_.size()) {
      while (pos < raw_.size() && std::isspace(static_cast<unsigned char>(raw_[pos]))) ++pos;
      if (pos >= raw_.size()) break;
      const size_t start = pos;
      while (pos < raw_.size() && !std::isspace(static_cast<unsigned char>(raw_[pos]))) ++pos;
      args_.push_back(String(raw_.substr(start, pos - start)));
    }
    rawString_ = String(raw_);
  }
  const String& arg(int index) const { return index >= 0 && index < count() ? args_[index] : empty_; }
  int count() const { return static_cast<int>(args_.size()); }
  const String& raw() const { return rawString_; }
 private:
  std::string raw_;
  String rawString_;
  std::vector<String> args_;
  String empty_;
};

bool gCliValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (::gCliValidateOnly) return "VALID"; } while (0)
static char* gDebugBuffer = nullptr;
static bool ensureDebugBuffer() {
  static char storage[1024];
  gDebugBuffer = storage;
  return true;
}

void applyEspLogOverrides(bool includeWildcard);
// The radio stack tags' apply (test_radio_stack_logs runs it); here only the
// call and its place are recorded.
void applyRadioStackLogLevels() { events.push_back("radio"); }
// INSERT_PRODUCTION

static std::string run(const char* args) {
  events.clear();
  return cmd_esplog(String(args));
}
static std::string stored() { return gSettings.espLogLevels.c_str(); }
static bool has(const std::string& reply, const char* part) { return reply.find(part) != std::string::npos; }
using Events = std::vector<std::string>;

static void coreChecks() {
  using namespace esplog;
  // Level names, any case; "warning" too; nothing else.
  CHECK(parseLevel("none") == 0 && parseLevel("ERROR") == 1 && parseLevel("Warn") == 2 &&
        parseLevel("warning") == 2 && parseLevel("info") == 3 && parseLevel("debug") == 4 &&
        parseLevel("VERBOSE") == 5);
  for (const char* bad : {"", "w", "3", "loud", "default", "verbosely", static_cast<const char*>(nullptr)}) CHECK(parseLevel(bad) == -1);

  // Tags: the real ones pass (the C6 mirror's "C6>" too); separators, spaces,
  // quotes, a non-lone '*' and over-long names do not.
  for (const char* good : {"*", "BT_BTM", "BT_APPL", "rpc_utils", "C6>", "i2c.master", "esp-tls-mbedtls",
                           "HTTP_CLIENT", "a", "abcdefghijklmnopqrstuvwxyz01234"}) CHECK(validTag(good));
  for (const char* bad : {"", "a b", "a,b", "a:b", "**", "a*", "\"x\"", "a\\b", "\tx",
                          "abcdefghijklmnopqrstuvwxyz012345"}) CHECK(!validTag(bad));

  char out[kMaxStoredLen + 1];
  // set: appends, replaces in place, keeps the wildcard first.
  CHECK(set("", "BT_BTM", 1, out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "BT_BTM:E"));
  CHECK(set("BT_BTM:E", "rpc_utils", 2, out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "BT_BTM:E,rpc_utils:W"));
  CHECK(set("BT_BTM:E,rpc_utils:W", "*", 0, out, sizeof(out)) == Edit::Ok &&
        !std::strcmp(out, "*:N,BT_BTM:E,rpc_utils:W"));
  CHECK(set("*:N,BT_BTM:E,rpc_utils:W", "BT_BTM", 0, out, sizeof(out)) == Edit::Ok &&
        !std::strcmp(out, "*:N,BT_BTM:N,rpc_utils:W"));
  CHECK(set("*:N,BT_BTM:E", "BT_BTM", 1, out, sizeof(out)) == Edit::Unchanged && !std::strcmp(out, "*:N,BT_BTM:E"));
  CHECK(set("", "a b", 1, out, sizeof(out)) == Edit::BadTag);
  CHECK(set("", "x", 6, out, sizeof(out)) == Edit::BadLevel && set("", "x", -1, out, sizeof(out)) == Edit::BadLevel);
  // Malformed and repeated entries are skipped by readers and dropped by editors.
  CHECK(levelOf("x,::,BT:Q,ok:E,ok:N,:E,a b:W", "ok") == 1);
  CHECK(forEach("x,::,BT:Q,ok:E,ok:N,:E,a b:W", [](const char*, size_t, int) {}) == 1);
  CHECK(set("x,::,ok:E,ok:N", "new", 3, out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "ok:E,new:I"));
  CHECK(forEach(nullptr, [](const char*, size_t, int) {}) == 0 && levelOf(nullptr, "x") == -1);

  // Capacity: 16 entries, then refused; 240 characters, then refused.
  std::string sixteen;
  for (int i = 0; i < 16; ++i) sixteen += (i ? "," : "") + std::string("t") + std::to_string(i) + ":E";
  CHECK(forEach(sixteen.c_str(), [](const char*, size_t, int) {}) == 16);
  CHECK(set(sixteen.c_str(), "extra", 1, out, sizeof(out)) == Edit::TooMany);
  CHECK(set(sixteen.c_str(), "t3", 2, out, sizeof(out)) == Edit::Ok);  // a replace still fits
  std::string longTags;
  for (int i = 0; i < 7; ++i) longTags += (i ? "," : "") + std::string(31, char('a' + i)) + ":W";
  CHECK(std::strlen(longTags.c_str()) == 7 * 33 + 6);
  CHECK(set(longTags.c_str(), "bb", 1, out, sizeof(out)) == Edit::TooLong);
  char tiny[4];
  CHECK(set("", "abcd", 1, tiny, sizeof(tiny)) == Edit::TooLong);

  // remove
  CHECK(remove("*:N,BT_BTM:E,rpc_utils:W", "BT_BTM", out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "*:N,rpc_utils:W"));
  CHECK(remove("*:N,rpc_utils:W", "*", out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "rpc_utils:W"));
  CHECK(remove("rpc_utils:W", "rpc_utils", out, sizeof(out)) == Edit::Ok && !std::strcmp(out, ""));
  CHECK(remove("rpc_utils:W", "nope", out, sizeof(out)) == Edit::NotFound);
  CHECK(remove("rpc_utils:W", "a,b", out, sizeof(out)) == Edit::BadTag);

  // replaceAll: names or letters, spaces around entries, wildcard moved first;
  // strict on anything malformed or repeated.
  CHECK(replaceAll(" BT_BTM:error , *:n,C6>:W ", out, sizeof(out)) == Edit::Ok && !std::strcmp(out, "*:N,BT_BTM:E,C6>:W"));
  CHECK(replaceAll("", out, sizeof(out)) == Edit::Ok && !std::strcmp(out, ""));
  CHECK(replaceAll("BT_BTM", out, sizeof(out)) == Edit::BadTag);
  CHECK(replaceAll("BT_BTM:loud", out, sizeof(out)) == Edit::BadLevel);
  CHECK(replaceAll("BT_BTM:", out, sizeof(out)) == Edit::BadLevel);
  CHECK(replaceAll("a b:E", out, sizeof(out)) == Edit::BadTag);
  CHECK(replaceAll("x:E,,y:W", out, sizeof(out)) == Edit::BadTag);
  CHECK(replaceAll("x:E,x:W", out, sizeof(out)) == Edit::Duplicate);
  std::string seventeen = sixteen + ",t16:E";
  CHECK(replaceAll(seventeen.c_str(), out, sizeof(out)) == Edit::TooMany);
  CHECK(replaceAll((longTags + ",bb:E").c_str(), out, sizeof(out)) == Edit::TooLong);
}

static void commandChecks() {
  gSettings.espLogLevels = "";
  std::string reply = run("");
  CHECK(reply == "Saved ESP-IDF log levels (esplog): none (the firmware's own levels apply)" && events.empty());

  // A tag: saved, then applied at once.
  CHECK(run("BT_BTM error") == "BT_BTM: error (saved)");
  CHECK(stored() == "BT_BTM:E" && events == (Events{"persist", "esp:BT_BTM=ERROR"}));
  CHECK(run("rpc_utils WARNING") == "rpc_utils: warn (saved)");
  CHECK(stored() == "BT_BTM:E,rpc_utils:W" && events == (Events{"persist", "esp:rpc_utils=WARN"}));
  // The same level again: nothing to persist, applied again.
  CHECK(run("BT_BTM Error") == "BT_BTM: error (saved)" && events == (Events{"esp:BT_BTM=ERROR"}));
  // Above the build's maximum: saved and said.
  reply = run("BT_APPL debug");
  CHECK(has(reply, "BT_APPL: debug (saved) (this build compiles ESP-IDF lines only up to info)"));
  CHECK(stored() == "BT_BTM:E,rpc_utils:W,BT_APPL:D");

  // The wildcard: every tag, HTTP_CLIENT capped, the radio stack tags from
  // their flags again, then the tag entries again.
  reply = run("* none");
  CHECK(has(reply, "*: none (saved)"));
  CHECK(stored() == "*:N,BT_BTM:E,rpc_utils:W,BT_APPL:D");
  CHECK(events == (Events{"persist", "esp:*=NONE", "esp:HTTP_CLIENT=NONE", "radio", "esp:BT_BTM=ERROR",
                          "esp:rpc_utils=WARN", "esp:BT_APPL=DEBUG"}));
  run("* verbose");
  CHECK(events == (Events{"persist", "esp:*=VERBOSE", "esp:HTTP_CLIENT=WARN", "radio", "esp:BT_BTM=ERROR",
                          "esp:rpc_utils=WARN", "esp:BT_APPL=DEBUG"}));

  // The list shows each entry by name.
  reply = run("  ");
  CHECK(reply == "Saved ESP-IDF log levels (esplog):\n  * verbose\n  BT_BTM error\n  rpc_utils warn\n  BT_APPL debug");
  CHECK(events.empty());

  // The tag that prints credentials stays at warn or below.
  CHECK(has(run("HTTP_CLIENT debug"), "Error: HTTP_CLIENT stays at warn") && events.empty());
  CHECK(run("HTTP_CLIENT error") == "HTTP_CLIENT: error (saved)");
  CHECK(events == (Events{"persist", "esp:HTTP_CLIENT=ERROR"}));

  // Applying: the boot (wildcard) and the owners' re-apply (tags only).
  events.clear();
  applyEspLogOverrides(false);
  CHECK(events == (Events{"esp:BT_BTM=ERROR", "esp:rpc_utils=WARN", "esp:BT_APPL=DEBUG", "esp:HTTP_CLIENT=ERROR"}));
  // A stored value written by another path is read defensively; an
  // HTTP_CLIENT entry above warn is capped when applied.
  gSettings.espLogLevels = "x,::,BT:Q,ok:E,HTTP_CLIENT:V,a b:W";
  events.clear();
  applyEspLogOverrides(true);
  CHECK(events == (Events{"esp:ok=ERROR", "esp:HTTP_CLIENT=WARN"}));
  gSettings.espLogLevels = "";
  events.clear();
  applyEspLogOverrides(true);
  CHECK(events.empty());
  gSettings.espLogLevels = "*:N,BT_BTM:E,rpc_utils:W,BT_APPL:D,HTTP_CLIENT:E";

  // Removing a radio stack tag: saved, and the tag follows its debug flag
  // again at once.
  reply = run("BT_APPL default");
  CHECK(reply == "BT_APPL: saved level removed; it follows its debug flag again" &&
        events == (Events{"persist", "radio"}));
  CHECK(stored() == "*:N,BT_BTM:E,rpc_utils:W,HTTP_CLIENT:E");
  CHECK(run("BT_APPL default") == "No saved level for that tag" && events.empty());
  // Any other tag: saved, nothing applied (the firmware's level returns at boot).
  reply = run("HTTP_CLIENT default");
  CHECK(reply == "HTTP_CLIENT: saved level removed; its firmware level returns at the next reboot" &&
        events == (Events{"persist"}));
  CHECK(stored() == "*:N,BT_BTM:E,rpc_utils:W");
  CHECK(run("HTTP_CLIENT error") == "HTTP_CLIENT: error (saved)" && stored() == "*:N,BT_BTM:E,rpc_utils:W,HTTP_CLIENT:E");

  // Refusals change nothing.
  const std::string before = stored();
  CHECK(run("x:E,x:W") == "Error: a tag appears twice" && events.empty() && stored() == before);
  for (const char* bad : {"BT_BTM loud", "BT_BTM", "a,b error", "BT_BTM error now", "* ", "x:E,x:W",
                          "abcdefghijklmnopqrstuvwxyz0123456 error", "HTTP_CLIENT:D"}) {
    reply = run(bad);
    CHECK((has(reply, "Error") || has(reply, "Usage")) && events.empty() && stored() == before);
  }
  gCliValidateOnly = true;
  CHECK(run("BT_BTM none") == "VALID" && events.empty() && stored() == before);
  gCliValidateOnly = false;
  // Short of memory: the old value is kept and nothing is applied; the lock
  // is released.
  gHostStringReserveFails = true;
  for (const char* args : {"BT_BTM none", "BT_BTM default", "C6>:N", "clear"}) {
    gLockTakes = 0;
    CHECK(run(args) == "Error: Not enough memory to save tag levels" && events.empty() && stored() == before);
    CHECK(gLockTakes == 1 && gLockDepth == 0);
  }
  // The same value again needs no room: saved as it was, nothing persisted.
  CHECK(run("BT_BTM error") == "BT_BTM: error (saved)" && stored() == before &&
        events == (Events{"esp:BT_BTM=ERROR"}));
  gHostStringReserveFails = false;

  // The settings editors' whole string replaces every entry.
  reply = run("C6>:N, BT_APPL:error");
  CHECK(has(reply, "ESP-IDF log levels saved: C6>:N,BT_APPL:E"));
  CHECK(stored() == "C6>:N,BT_APPL:E" &&
        events == (Events{"persist", "radio", "esp:C6>=NONE", "esp:BT_APPL=ERROR"}));
  CHECK(run("C6>:N,BT_APPL:E") == "ESP-IDF log levels unchanged" && events.empty());

  // clear
  CHECK(has(run("clear"), "cleared; the radio stack tags follow their debug flags again") && stored().empty() &&
        events == (Events{"persist", "radio"}));
  CHECK(run("CLEAR") == "No saved ESP-IDF log levels" && events.empty());
}

int main() {
  coreChecks();
  commandChecks();
  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("ESP_LOG_OVERRIDES PASS");
  return 0;
}
