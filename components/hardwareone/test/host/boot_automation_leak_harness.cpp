// Host harness for the production boot-automation pass (runAutomationsOnBoot).
// The runner inserts the real function and its JSON object scanner; the
// filesystem read, condition evaluator, command executor, logging and event
// post are boundary doubles. strdup/free inside the inserted owner code are
// counted so a skipped automation that keeps its command copies is visible.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

// Minimal Arduino String: only what the boot pass uses.
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  char operator[](size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
  int indexOf(const char* value, int from = 0) const { return find(text_.find(value, clamp(from))); }
  int indexOf(char value, int from = 0) const { return find(text_.find(value, clamp(from))); }
  int lastIndexOf(char value, int from) const {
    return from < 0 ? -1 : find(text_.rfind(value, static_cast<size_t>(from)));
  }
  String substring(int from, int to) const {
    const size_t f = clamp(from), t = clamp(to);
    return t > f ? String(text_.substr(f, t - f)) : String();
  }
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(" \t\r\n") - first + 1);
  }
  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }
  const char* c_str() const { return text_.c_str(); }
  unsigned length() const { return static_cast<unsigned>(text_.size()); }

 private:
  size_t clamp(int value) const {
    return value < 0 ? 0 : std::min(static_cast<size_t>(value), text_.size());
  }
  static int find(size_t pos) { return pos == std::string::npos ? -1 : static_cast<int>(pos); }
  std::string text_;
};

#define DEBUGF(...) do {} while (false)
static constexpr int DEBUG_AUTOMATIONS = 0;
static const char* AUTOMATIONS_JSON_FILE = "/system/automations.json";
static constexpr unsigned SYSEVT_AUTOMATION_FIRED = 7;

struct AuthContext { const char* scope; };
namespace VFS {
namespace Scopes { static constexpr const char* SYSTEM_STATE = "system-state"; }
static AuthContext systemAuth(const char* scope, const char*) { return {scope}; }
}

static bool filesystemReady = true;
static std::string gFile;
static bool readText(const char* path, String& out, const AuthContext&) {
  if (std::strcmp(path, AUTOMATIONS_JSON_FILE) != 0 || gFile.empty()) return false;
  out = String(gFile);
  return true;
}
// The owner's checked read; the format-version refusal is covered by
// automation_scheduler, so this boundary reads the file directly.
static bool readAutomationsJson(String& out) {
  return readText(AUTOMATIONS_JSON_FILE, out, VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "automation.state"));
}

static time_t testTime(time_t*) { return 0; }  // clock invalid: no nextAt rewrite
#define time testTime
static void delay(int) {}

// Condition double: "ROOM=here" is true, every other expression false.
static unsigned gConditionCalls = 0;
static bool evaluateCondition(const char* wrapped) {
  ++gConditionCalls;
  return std::strstr(wrapped, "IF ROOM=here THEN") != nullptr;
}

static std::vector<std::string> gExecuted;
static const char* executeConditionalCommand(const char* command, const char*, const char*) {
  gExecuted.emplace_back(command);
  return "ok";
}
static bool isAutoInternalResult(const char*) { return true; }
static void broadcastOutput(const char*) {}
static bool automationLogActive() { return true; }
static std::vector<std::string> gLog;
static bool appendAutoLogEntry(const char* type, const char* message) {
  gLog.emplace_back(std::string(type) + " " + message);
  return true;
}
static unsigned gFiredEvents = 0;
static void systemEventPost(unsigned kind, const char*, const char*) {
  if (kind == SYSEVT_AUTOMATION_FIRED) ++gFiredEvents;
}
static bool extractJsonString(const char*, const char*, char* out, size_t outSize) {
  if (outSize) out[0] = '\0';
  return false;
}
static time_t computeNextRunTime(const char*, time_t) { return 0; }
static bool updateAutomationNextAt(long, time_t) { return true; }

// Every strdup/free the inserted owner code makes is counted. The live set
// catches a pointer that is never freed even if the totals happened to match.
static unsigned gStrdups = 0, gFrees = 0;
static std::vector<void*> gLive;
static char* countedStrdup(const char* text) {
  char* copy = static_cast<char*>(std::malloc(std::strlen(text) + 1));
  if (!copy) return nullptr;
  std::strcpy(copy, text);
  ++gStrdups;
  gLive.push_back(copy);
  return copy;
}
static void countedFree(void* memory) {
  if (!memory) return;
  for (auto it = gLive.begin(); it != gLive.end(); ++it) {
    if (*it == memory) { gLive.erase(it); ++gFrees; std::free(memory); return; }
  }
  CHECK(!"free of a pointer the boot pass did not strdup");
}

static bool gTestBootRan = false;

#define strdup countedStrdup
#define free countedFree
// INSERT_OBJECT_END
// INSERT_BOOT_PASS
#undef free
#undef strdup

static void reset(const std::string& file) {
  gFile = file;
  gTestBootRan = false;
  gConditionCalls = 0;
  gExecuted.clear();
  gLog.clear();
  gFiredEvents = 0;
  gStrdups = gFrees = 0;
  CHECK(gLive.empty());
}

static bool logged(const char* prefix) {
  for (const auto& line : gLog) if (line.rfind(prefix, 0) == 0) return true;
  return false;
}

int main() {
  // 1. One boot automation whose global condition is false, three commands:
  //    nothing runs, and every command copy made while parsing is released.
  reset(R"({"automations":[
    {"id": 11, "name": "bedroom-lights", "enabled": true,
     "triggers": [{"type": "boot"}], "condition": "ROOM=bedroom",
     "commands": ["ledcolor red", "status", "uptime"]}
  ]})");
  runAutomationsOnBoot();
  CHECK(gConditionCalls == 1);
  CHECK(gExecuted.empty());
  CHECK(gFiredEvents == 0);
  CHECK(logged("AUTO_SKIP "));
  CHECK(gStrdups == 3);
  CHECK(gFrees == gStrdups);
  CHECK(gLive.empty());

  // 2. The same automation with a true condition runs all three in order and
  //    still frees them (the existing post-run path).
  reset(R"({"automations":[
    {"id": 12, "name": "here-lights", "enabled": true,
     "triggers": [{"type": "boot"}], "condition": "ROOM=here",
     "commands": ["ledcolor red", "status", "uptime"]}
  ]})");
  runAutomationsOnBoot();
  CHECK(gExecuted.size() == 3 && gExecuted[0] == "ledcolor red" && gExecuted[2] == "uptime");
  CHECK(gFiredEvents == 1);
  CHECK(gStrdups == 3 && gFrees == 3 && gLive.empty());

  // 3. A mixed file: a skipped multi-command automation, a skipped
  //    single-"command" automation, one with no condition and a disabled one.
  //    Only the unconditioned automation runs; nothing outlives the pass.
  reset(R"({"automations":[
    {"id": 21, "name": "away", "enabled": true, "triggers": [{"type":"boot"}],
     "condition": "ROOM=kitchen", "commands": ["a1", "a2"]},
    {"id": 22, "name": "single", "enabled": true, "triggers": [{"type":"boot"}],
     "condition": "WIFI=NONE", "command": "openwifi"},
    {"id": 23, "name": "always", "enabled": true, "triggers": [{"type":"boot"}],
     "commands": ["b1"]},
    {"id": 24, "name": "off", "enabled": false, "triggers": [{"type":"boot"}],
     "commands": ["c1"]}
  ]})");
  runAutomationsOnBoot();
  CHECK(gConditionCalls == 2);
  CHECK(gExecuted.size() == 1 && gExecuted[0] == "b1");
  CHECK(gFiredEvents == 1);
  CHECK(gStrdups == 4);
  CHECK(gFrees == gStrdups);
  CHECK(gLive.empty());

  // 4. The pass runs once per boot: a second call does nothing.
  const unsigned before = gStrdups;
  runAutomationsOnBoot();
  CHECK(gStrdups == before && gExecuted.size() == 1);

  std::printf("boot automation pass: %u checks passed\n", checks);
  return 0;
}
