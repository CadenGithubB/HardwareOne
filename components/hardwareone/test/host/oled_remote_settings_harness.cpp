// Host harness for test_oled_remote_settings.py. The driver splices production
// code in at the INSERT_* markers: the System_Settings and OLED_SettingsEditor
// types, the ESP-NOW command-result record (System_ESPNow), BondedPeer's
// schema-sync and remote-edit policy, OLED_RemoteSettings.cpp whole, the
// editor's module-source / commit functions and the remote-settings mode
// adapter. Everything else here is a double: the OLED command executor, the
// peer caches, the clock, PSRAM allocation and the display.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---- Arduino String (counts live instances) --------------------------------
static long gLiveStrings = 0;

class String {
 public:
  String() { ++gLiveStrings; }
  String(const char* text) : value_(text ? text : "") { ++gLiveStrings; }
  String(const std::string& text) : value_(text) { ++gLiveStrings; }
  String(const String& other) : value_(other.value_) { ++gLiveStrings; }
  String(String&& other) noexcept : value_(std::move(other.value_)) { ++gLiveStrings; }
  template <class T, typename std::enable_if<std::is_integral<T>::value &&
                                                 !std::is_same<T, bool>::value &&
                                                 !std::is_same<T, char>::value,
                                             int>::type = 0>
  String(T number) : value_(std::to_string(number)) { ++gLiveStrings; }
  ~String() { --gLiveStrings; }
  String& operator=(const String& other) { value_ = other.value_; return *this; }
  String& operator=(String&& other) noexcept { value_ = std::move(other.value_); return *this; }
  String& operator=(const char* text) { value_ = text ? text : ""; return *this; }
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool concat(const char* text) { if (text) value_ += text; return true; }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value_.clear(); return; }
    value_ = value_.substr(first, value_.find_last_not_of(" \t\r\n") - first + 1);
  }
  String substring(size_t from, size_t to) const {
    if (from > value_.size()) return String();
    return String(value_.substr(from, to - from));
  }
  String operator+(const String& rhs) const { return String(value_ + rhs.value_); }
  String operator+(const char* rhs) const { return String(value_ + (rhs ? rhs : "")); }
  bool operator==(const char* rhs) const { return value_ == (rhs ? rhs : ""); }
  bool operator==(const String& rhs) const { return value_ == rhs.value_; }
  std::string value_;
};
static String operator+(const char* lhs, const String& rhs) {
  return String(std::string(lhs ? lhs : "") + rhs.value_);
}

#include <ArduinoJson.h>

// ---- checks ------------------------------------------------------------------
static int gFailures = 0;
static const char* gCase = "";
#define EXPECT(cond, msg)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase,                \
                   std::string(msg).c_str(), #cond, __LINE__);                    \
      ++gFailures;                                                                \
    }                                                                             \
  } while (0)

// ---- platform ----------------------------------------------------------------
#define ENABLE_OLED_DISPLAY 1
#define ENABLE_ESPNOW 1
#define ENABLE_BONDED_MODE 1
#define DEBUGF(...) ((void)0)
#define DEBUG_SYSTEMF(...) ((void)0)
#define DEBUG_ESPNOW_ROUTER 0
#define EXT_RAM_BSS_ATTR
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(m) (m)->lock()
#define taskEXIT_CRITICAL(m) (m)->unlock()

static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }
static int gDelayCalls = 0;
static void delay(uint32_t) { ++gDelayCalls; }

// PSRAM allocation doubles: every block is tracked; gFailAllocAt makes the N-th
// allocation (1-based, counted from the last reset) fail.
enum class AllocPref : uint8_t { PreferPSRAM, PreferInternal };
using AllocPolicy = AllocPref;
static std::set<void*> gLiveAllocs;
static int gAllocCalls = 0;
static int gFailAllocAt = 0;
static bool hw1AllocFails() { ++gAllocCalls; return gFailAllocAt != 0 && gAllocCalls == gFailAllocAt; }
static void* hw1Track(void* p) { if (p) gLiveAllocs.insert(p); return p; }
static void* ps_alloc(size_t n, AllocPolicy, const char*) {
  return hw1AllocFails() ? nullptr : hw1Track(std::malloc(n ? n : 1));
}
static void* ps_alloc(size_t n, AllocPolicy p) { return ps_alloc(n, p, nullptr); }
static void* ps_calloc(size_t n, size_t size, AllocPolicy, const char*) {
  return hw1AllocFails() ? nullptr : hw1Track(std::calloc(n ? n : 1, size ? size : 1));
}
static void* ps_calloc(size_t n, size_t size, AllocPolicy p) { return ps_calloc(n, size, p, nullptr); }
static void hw1Free(void* p) { gLiveAllocs.erase(p); std::free(p); }
#define PSRAM_JSON_DOC(name) JsonDocument name

// ---- settings registry types (production) --------------------------------------
// INSERT_SETTINGS_TYPES

// ---- peer, bond and cache doubles ------------------------------------------------
static const uint8_t kPeerMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t kOtherMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
struct HarnessSettings {
  bool bondModeEnabled = true;
  String bondPeerMac = "02:00:00:00:00:01";
};
static HarnessSettings gSettings;
bool filesystemReady = true;
static bool macParse(const char*, uint8_t out[6]) { std::memcpy(out, kPeerMac, 6); return true; }
static void macToPathToken(const uint8_t*, char* out) { std::snprintf(out, 13, "%s", "020000000001"); }
namespace VFS {
struct Auth {};
static Auth systemAuth(const char*) { return {}; }
static bool existsGuarded(const String&, const Auth&) { return true; }
}  // namespace VFS

static std::string gSchemaText, gSettingsText;
static bool gPaired = true, gMaster = true, gOnline = true, gSendSchemaOk = true;
static int gSchemaRequests = 0;
String loadSettingsFromCache(const uint8_t*) { return String(gSettingsText); }

struct EspNowState { bool bondSchemaReceived = false; };
static EspNowState gEspNowState;
static EspNowState* gEspNow = &gEspNowState;
bool macEqual6(const uint8_t a[6], const uint8_t b[6]) { return std::memcmp(a, b, 6) == 0; }
bool requestBondSchema(const uint8_t*) { ++gSchemaRequests; return gSendSchemaOk; }

// ---- ESP-NOW command-result record (production or stand-in) -------------------
// INSERT_ESPNOW

// ---- BondedPeer (doubles + production schema sync / policy) --------------------
namespace {
const char* sLastError = "";
bool fail(const char* msg) { sLastError = msg ? msg : "unknown"; return false; }
void clearError() { sLastError = ""; }
bool requireMasterOnlinePeer(uint8_t out[6]) {
  std::memcpy(out, kPeerMac, 6);
  if (!gPaired) return fail("No bonded peer");
  if (!gMaster) return fail("Only master can pull from peer");
  if (!gOnline) return fail("Peer offline");
  return true;
}
}  // namespace
namespace BondedPeer {
bool isPaired() { return gPaired; }
bool isMaster() { return gMaster; }
String readCachedSchemaJson() { return String(gSchemaText); }
String readCachedSettingsJson() { return String(gSettingsText); }
bool peerMacBytes(uint8_t out[6]) {
  if (!gPaired) return false;
  std::memcpy(out, kPeerMac, 6);
  return true;
}
const char* lastError() { return sLastError; }
// INSERT_BONDED
}  // namespace BondedPeer

// ---- OLED command executor doubles ---------------------------------------------
static std::vector<std::string> gWithResult;  // executeOLEDCommandWithResult lines
static std::vector<std::string> gLocal;       // executeOLEDCommand lines (local, fire-and-forget)
static bool gExecOk = true;
static std::string gExecText;                 // "" = "Remote command sent: <inner>"
bool executeOLEDCommandWithResult(const String& cmd, char* out, size_t outSize) {
  gWithResult.emplace_back(cmd.c_str());
  std::string text = gExecText;
  if (text.empty()) text = std::string("Remote command sent: ") + (cmd.length() > 7 ? cmd.c_str() + 7 : "");
  std::snprintf(out, outSize, "%s", text.c_str());
  return gExecOk;
}
void executeOLEDCommand(const String& cmd) { gLocal.emplace_back(cmd.c_str()); }

// ---- OLED editor types (production) and mode ------------------------------------
// INSERT_EDITOR_TYPES
SettingsEditorContext gSettingsEditor;
enum OLEDMode { OLED_MENU, OLED_SETTINGS, OLED_REMOTE_SETTINGS };
static OLEDMode currentOLEDMode = OLED_MENU;
void initSettingsEditor();
void settingsEditorShowError(const String& message, uint32_t durationMs);

// One local module with the real OLED brightness row (OLED_Settings.cpp).
static int gLocalBrightness = 200;
static const SettingEntry kLocalOledEntries[] = {
  {"oledBrightness", SETTING_INT, &gLocalBrightness, 255, 0, nullptr, 0, 255, "Brightness",
   nullptr, false, "display", "oledbrightness"},
};
static const SettingsModule kLocalOled = {"oled", "hardware.sensors.oled", kLocalOledEntries, 1,
                                          nullptr, "OLED"};
const SettingsModule** getSettingsModules(size_t& count) {
  static const SettingsModule* modules[] = {&kLocalOled};
  count = 1;
  return modules;
}

// ---- display / mode-framework doubles (adapter) ----------------------------------
struct HarnessDisplay {
  std::vector<std::string> lines;
  int clears = 0;
  void clearDisplay() { ++clears; lines.clear(); }
  void setTextSize(int) {}
  void setCursor(int, int) {}
  void println(const char* text) { lines.emplace_back(text ? text : ""); }
  void println(const String& text) { lines.emplace_back(text.c_str()); }
};
static HarnessDisplay gDisplay;
[[maybe_unused]] static HarnessDisplay* oledDisplay = &gDisplay;
#define OLED_CONTENT_START_Y 12
static int gDisplayUpdates = 0;
static void displayUpdate() { ++gDisplayUpdates; }
static int gMenuBacks = 0;
static bool oledMenuBack() { ++gMenuBacks; return true; }
static int gDirty = 0;
static void oledMarkDirty() { ++gDirty; }
static unsigned long gDirtyUntil = 0;
static void oledMarkDirtyUntil(unsigned long untilMs) { if (untilMs > gDirtyUntil) gDirtyUntil = untilMs; }
static std::string gToast;
static void oledToastShow(const char* text, uint32_t = 3000) { gToast = text ? text : ""; }
static int gEditorDraws = 0;
void displaySettingsEditor() { ++gEditorDraws; }
static int gEditorInputs = 0;
bool handleSettingsEditorInput(int, int, uint32_t) { ++gEditorInputs; return true; }
constexpr uint32_t INPUT_BUTTON_A = 1, INPUT_BUTTON_B = 2;
#define INPUT_CHECK(buttons, mask) (((buttons) & (mask)) != 0)

// ---- production ---------------------------------------------------------------
#define free hw1Free
// INSERT_REMOTE
// INSERT_EDITOR
// INSERT_ADAPTER
#undef free

// ---- fixtures -------------------------------------------------------------------
static const char* kSchema = R"JSON({"modules":[
 {"name":"oled","section":"hardware.sensors.oled","description":"OLED","connected":true,"entries":[
  {"key":"oledBrightness","label":"Brightness","type":"int","group":"display","cmdKey":"oledbrightness","min":0,"max":255,"default":255},
  {"key":"oledDefaultMode","label":"Default Mode","type":"string","cmdKey":"oleddefaultmode","options":"logo,status,menu","default":"logo"},
  {"key":"uptime","label":"Uptime","type":"int","readOnly":true,"default":0},
  {"key":"apiKey","label":"API key","type":"string","secret":true,"set":true,"cmdKey":"apikey","default":""},
  {"key":"gain","label":"Gain","type":"float","cmdKey":"gain","default":1.5},
  {"key":"rawCount","label":"Raw","type":"int","default":3}]},
 {"name":"clock","section":"system.time","description":"Clock","entries":[
  {"key":"ntpServer","label":"NTP Server","type":"string","cmdKey":"ntpserver","default":"pool.ntp.org"},
  {"key":"tzOffsetMinutes","label":"TZ","type":"int","cmdKey":"tzoffsetminutes","options":"0|UTC,-300|EST,60|CET","default":-300},
  {"key":"ntpEnabled","label":"NTP","type":"bool","cmdKey":"ntpenabled","default":true}]},
 {"name":"wifi","section":"network.wifi","entries":[
  {"key":"wifiEnabled","label":"WiFi","type":"bool","cmdKey":"wifienabled","default":true}]},
 {"name":"users","section":"users","entries":[
  {"key":"x","label":"X","type":"string","cmdKey":"userx","default":""}]},
 {"name":"empty","section":"empty","entries":[
  {"key":"y","label":"Y","type":"int","default":0}]},
 {"name":"misc","section":"system.misc","entries":[
  {"key":"beep","label":"Beep","type":"bool","cmdKey":"beep","default":false}]}
],"count":6})JSON";

static const char* kSettings = R"JSON({"hardware":{"sensors":{"oled":{"display":{"oledBrightness":128}}}},
 "system":{"time":{"ntpServer":"time.example"},"misc":{"beep":true}},"firmwareVersion":"x"})JSON";

static void resetWorld() {
  freeRemoteSettingsModules();
  gSchemaText = kSchema;
  gSettingsText = kSettings;
  gPaired = gMaster = gOnline = gSendSchemaOk = true;
  gSchemaRequests = 0;
  gEspNow->bondSchemaReceived = false;
  gExecOk = true;
  gExecText.clear();
  gWithResult.clear();
  gLocal.clear();
  gFailAllocAt = 0;
  gAllocCalls = 0;
  currentOLEDMode = OLED_MENU;
  gNowMs += 100000;
  initSettingsEditor();
  gDisplay = HarnessDisplay();
  gMenuBacks = gDirty = gEditorDraws = gEditorInputs = gDisplayUpdates = 0;
  gDirtyUntil = 0;
  gToast.clear();
}

static const SettingEntry* findIn(const SettingsModule** mods, size_t n, const char* key) {
  for (size_t m = 0; m < n; m++) {
    if (!mods || !mods[m]) continue;
    for (size_t i = 0; i < mods[m]->count; i++) {
      const SettingEntry& e = mods[m]->entries[i];
      if (e.jsonKey && std::strcmp(e.jsonKey, key) == 0) return &e;
    }
  }
  return nullptr;
}
static const SettingEntry* findRemote(const char* key) {
  size_t n = 0;
  const SettingsModule** mods = getRemoteSettingsModules(n);
  return findIn(mods, n, key);
}
static size_t remoteModuleCount() {
  size_t n = 0;
  getRemoteSettingsModules(n);
  return n;
}
static int intValue(const SettingEntry* e) {
  return (e && e->valuePtr && e->type == SETTING_INT) ? *(const int*)e->valuePtr : -99999;
}
static std::string stringValue(const SettingEntry* e) {
  return (e && e->valuePtr && e->type == SETTING_STRING) ? std::string(((const String*)e->valuePtr)->c_str())
                                                         : std::string("<none>");
}
static bool sameText(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
static bool commandsAre(const std::vector<std::string>& got, const std::vector<std::string>& want) {
  return got == want;
}
static std::string joined(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : " | ") + s;
  return out.empty() ? "(none)" : out;
}
static void peerReplies(const uint8_t* mac, bool ok, const char* text) {
  espnowPublishCmdResult(mac, 42, ok, text);
}

// ---- cases ------------------------------------------------------------------------
static void case_cmd_result_record() {
  gCase = "cmd_result_record";
  EspNowCmdResult r;
  EXPECT(espnowCmdResultSeq() == 0 && !espnowCmdResultGet(r),
         "no command result is readable before the first CMD_RESP");
  std::string longLine(200, 'a');
  espnowPublishCmdResult(kPeerMac, 7, true, "first line\nsecond line");
  EXPECT(espnowCmdResultSeq() == 1, "the CMD_RESP drain publishes a readable record (seq 1)");
  EXPECT(espnowCmdResultGet(r) && r.seq == 1 && r.reqId == 7 && r.success &&
             std::memcmp(r.srcMac, kPeerMac, 6) == 0 && r.receivedMs == gNowMs,
         "record copies sequence, request id, flag, source MAC and time");
  EXPECT(sameText(r.text, "first line"), "record keeps the first line of the result");
  espnowPublishCmdResult(kOtherMac, 8, false, longLine.c_str());
  EXPECT(espnowCmdResultGet(r) && r.seq == 2 && r.reqId == 8 && !r.success &&
             std::memcmp(r.srcMac, kOtherMac, 6) == 0,
         "a second result advances the sequence");
  EXPECT(std::strlen(r.text) == EspNowCmdResult::TEXT_MAX - 1, "a long result is cut at TEXT_MAX-1");
}

static void case_build_uses_schema_cmdkeys() {
  gCase = "build_uses_schema_cmdkeys";
  resetWorld();
  EXPECT(loadRemoteSettingsModules(), "the remote module set loads from the peer caches");
  size_t n = 0;
  const SettingsModule** mods = getRemoteSettingsModules(n);
  std::vector<std::string> names;
  for (size_t m = 0; m < n; m++) {
    names.emplace_back(mods[m]->name ? mods[m]->name : "(null)");
    for (size_t i = 0; i < mods[m]->count; i++) {
      const SettingEntry& e = mods[m]->entries[i];
      const std::string where = std::string(mods[m]->name ? mods[m]->name : "?") + "." +
                                (e.jsonKey ? e.jsonKey : "?");
      EXPECT(e.cmdKey && e.cmdKey[0],
             "every entry the remote editor builds carries the schema's setter command (cmdKey): " + where);
      EXPECT(!e.readOnly && !e.isSecret, "no read-only or secret row is offered: " + where);
      EXPECT(e.valuePtr != nullptr, "every entry has a value slot the editor can read: " + where);
    }
  }
  EXPECT((names == std::vector<std::string>{"oled", "clock", "misc"}),
         "modules are the peer's exposed schema modules [oled, clock, misc], got: " + joined(names));
  if (n >= 1) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < mods[0]->count; i++) keys.emplace_back(mods[0]->entries[i].jsonKey);
    EXPECT((keys == std::vector<std::string>{"oledBrightness", "oledDefaultMode"}),
           "oled lists only rows with a setter, not read-only/secret/float: " + joined(keys));
  }
  if (n >= 2) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < mods[1]->count; i++) keys.emplace_back(mods[1]->entries[i].jsonKey);
    EXPECT((keys == std::vector<std::string>{"ntpServer", "tzOffsetMinutes"}),
           "clock exposes only ntpServer and tzOffsetMinutes: " + joined(keys));
  }
  const SettingEntry* b = findRemote("oledBrightness");
  EXPECT(b && b->type == SETTING_INT && sameText(b->cmdKey, "oledbrightness") && b->minVal == 0 &&
             b->maxVal == 255 && sameText(b->group, "display") && sameText(b->label, "Brightness"),
         "oledBrightness keeps the schema's type, setter, range, group and label");
  EXPECT(intValue(b) == 128, "oledBrightness shows the cached value (section + group + key)");
  const SettingEntry* mode = findRemote("oledDefaultMode");
  EXPECT(mode && mode->type == SETTING_STRING && sameText(mode->options, "logo,status,menu") &&
             stringValue(mode) == "logo",
         "oledDefaultMode is a string pick-list with the schema default");
  EXPECT(stringValue(findRemote("ntpServer")) == "time.example", "ntpServer shows the cached value");
  const SettingEntry* tz = findRemote("tzOffsetMinutes");
  EXPECT(tz && sameText(tz->options, "0|UTC,-300|EST,60|CET") && intValue(tz) == -300,
         "tzOffsetMinutes keeps its options and the schema default");
  const SettingEntry* beep = findRemote("beep");
  EXPECT(beep && beep->type == SETTING_BOOL && beep->minVal == 0 && beep->maxVal == 1 &&
             beep->valuePtr && *(const bool*)beep->valuePtr,
         "a bool row gets the slider's 0..1 range and the cached value");
  for (const char* hidden : {"uptime", "apiKey", "gain", "rawCount", "ntpEnabled", "wifiEnabled", "x", "y",
                             "firmwareVersion"}) {
    EXPECT(findRemote(hidden) == nullptr, std::string("row is not offered: ") + hidden);
  }
}

static void case_apply_sends_schema_command() {
  gCase = "apply_sends_schema_command";
  resetWorld();
  loadRemoteSettingsModules();
  const SettingEntry* b = findRemote("oledBrightness");
  EXPECT(b != nullptr, "oledBrightness is listed");
  if (!b) return;
  String err;
  EXPECT(applyRemoteSettingChange(b, "7", err), "the commit is accepted for delivery");
  EXPECT(commandsAre(gWithResult, {"remote:oledbrightness 7"}),
         "the commit sends remote:<cmdKey> <value> (the schema setter), sent: " + joined(gWithResult));
  EXPECT(gLocal.empty(), "nothing runs locally");
  String text;
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_PENDING,
         "the local 'Remote command sent' is not reported as applied; the commit waits for the peer");
}

static void case_apply_reports_peer_failure() {
  gCase = "apply_reports_peer_failure";
  resetWorld();
  loadRemoteSettingsModules();
  const SettingEntry* b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed"); return; }
  String err, text;
  applyRemoteSettingChange(b, "7", err);
  peerReplies(kPeerMac, false, "Unknown command: oledbrightness 7\n(help)");
  const RemoteSettingsOutcome o = remoteSettingsPollOutcome(text);
  EXPECT(o == REMOTE_OUTCOME_FAILED, "the peer's CMD_RESP failure is reported as FAILED");
  EXPECT(std::string(text.c_str()).find("Unknown command") != std::string::npos,
         "the peer's answer text is surfaced: " + std::string(text.c_str()));
  EXPECT(intValue(b) == 128, "a refused value does not reach the cached slot");

  applyRemoteSettingChange(b, "8", err);
  peerReplies(kPeerMac, true, "Error: value must be 0..255");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_FAILED,
         "an 'Error' reply is a failure even when the peer flags success");
  EXPECT(intValue(b) == 128, "the slot keeps the old value");
}

static void case_apply_success_updates_cached_value() {
  gCase = "apply_success_updates_cached_value";
  resetWorld();
  loadRemoteSettingsModules();
  String err, text;
  const SettingEntry* b = findRemote("oledBrightness");
  const SettingEntry* beep = findRemote("beep");
  const SettingEntry* ntp = findRemote("ntpServer");
  if (!b || !beep || !ntp) { EXPECT(false, "oledBrightness, beep and ntpServer are listed"); return; }
  applyRemoteSettingChange(b, "7", err);
  peerReplies(kPeerMac, true, "oledBrightness set to 7");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_APPLIED && intValue(b) == 7,
         "the peer's success applies the int value to the list");
  EXPECT(std::string(text.c_str()) == "oledBrightness set to 7", "the peer's text comes back");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_NONE, "the outcome is consumed once");
  applyRemoteSettingChange(beep, "0", err);
  peerReplies(kPeerMac, true, "beep = 0");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_APPLIED && !*(const bool*)beep->valuePtr,
         "a bool value is applied");
  applyRemoteSettingChange(ntp, "pool.example", err);
  peerReplies(kPeerMac, true, "ntpServer = pool.example");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_APPLIED && stringValue(ntp) == "pool.example",
         "a string value is applied");
}

static void case_apply_reports_no_reply() {
  gCase = "apply_reports_no_reply";
  resetWorld();
  loadRemoteSettingsModules();
  const SettingEntry* b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed"); return; }
  String err, text;
  applyRemoteSettingChange(b, "7", err);
  const uint32_t sent = gNowMs;
  gNowMs = sent + 4999;
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_PENDING, "still pending at 4999 ms");
  gNowMs = sent + 5000;
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_TIMEOUT && text.length() == 0,
         "no reply from the peer within 5 s is reported as TIMEOUT");
  EXPECT(intValue(b) == 128, "an unanswered value does not reach the cached slot");
}

static void case_apply_ignores_other_peer_reply() {
  gCase = "apply_ignores_other_peer_reply";
  resetWorld();
  loadRemoteSettingsModules();
  const SettingEntry* b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed"); return; }
  String err, text;
  applyRemoteSettingChange(b, "7", err);
  peerReplies(kOtherMac, true, "something else");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_PENDING,
         "another device's CMD_RESP does not decide the commit");
  peerReplies(kPeerMac, false, "Error: busy");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_FAILED, "the bonded peer's reply decides it");
}

static void case_apply_refuses() {
  gCase = "apply_refuses";
  resetWorld();
  loadRemoteSettingsModules();
  String err, text;
  SettingEntry noCmd = {};
  noCmd.jsonKey = "rawCount";
  noCmd.type = SETTING_INT;
  EXPECT(!applyRemoteSettingChange(&noCmd, "3", err) && std::string(err.c_str()) == "No command",
         "a row without a setter is not sent (no command derived from the key)");
  EXPECT(gWithResult.empty(), "nothing was sent for it");
  const SettingEntry* b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed"); return; }
  EXPECT(!applyRemoteSettingChange(b, "  ", err) && std::string(err.c_str()) == "Empty value",
         "an empty value is refused");
  EXPECT(applyRemoteSettingChange(b, "9", err), "first commit accepted");
  EXPECT(!applyRemoteSettingChange(b, "10", err) && std::string(err.c_str()) == "Busy",
         "a second commit while one is in flight is refused");
  freeRemoteSettingsModules();
  loadRemoteSettingsModules();
  b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed after a reload"); return; }
  gWithResult.clear();
  gExecOk = false;
  gExecText = "Error: No session token - bond not ready or passphrase mismatch";
  EXPECT(!applyRemoteSettingChange(b, "9", err) && std::string(err.c_str()) == gExecText,
         "the executor's refusal is returned, not hidden");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_NONE, "a refused send leaves nothing pending");
}

static void case_load_step_fetches_schema_once() {
  gCase = "load_step_fetches_schema_once";
  resetWorld();
  gSchemaText.clear();
  EXPECT(remoteSettingsLoadStep() == REMOTE_LOAD_SYNCING, "no cached schema: the screen starts one schema sync");
  EXPECT(gSchemaRequests == 1, "exactly one SCHEMA_REQ is sent");
  const uint32_t start = gNowMs;
  gNowMs = start + 5999;
  EXPECT(remoteSettingsLoadStep() == REMOTE_LOAD_SYNCING && gSchemaRequests == 1,
         "still syncing at 5999 ms, no second request");
  gSchemaText = kSchema;
  gEspNow->bondSchemaReceived = true;
  EXPECT(remoteSettingsLoadStep() == REMOTE_LOAD_READY && remoteModuleCount() == 3,
         "the landed schema is loaded");

  resetWorld();
  gSchemaText.clear();
  remoteSettingsLoadStep();
  gNowMs += 6000;
  EXPECT(remoteSettingsLoadStep() == REMOTE_LOAD_FAILED &&
             std::string(remoteSettingsLastError()) == "Schema sync timed out",
         "a schema that never lands fails after 6 s");

  resetWorld();
  gSchemaText.clear();
  gOnline = false;
  EXPECT(remoteSettingsLoadStep() == REMOTE_LOAD_FAILED &&
             std::string(remoteSettingsLastError()) == "Peer offline" && gSchemaRequests == 0,
         "an offline peer fails with the BondedPeer reason and no request");
}

static void case_editor_commits_remote_in_remote_mode() {
  gCase = "editor_commits_remote_in_remote_mode";
  resetWorld();
  loadRemoteSettingsModules();
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  size_t n = 0;
  const SettingsModule** mods = editorModules(n);
  const SettingEntry* b = findIn(mods, n, "oledBrightness");
  if (!b) { EXPECT(false, "the remote screen lists oledBrightness"); return; }
  setSettingValue(b, 5);
  EXPECT(gLocal.empty(), "the remote screen never runs a local setter, ran: " + joined(gLocal));
  EXPECT(commandsAre(gWithResult, {"remote:oledbrightness 5"}),
         "the slider/pick-list commit goes to the bonded peer, sent: " + joined(gWithResult));
  peerReplies(kPeerMac, true, "ok");
  String text;
  remoteSettingsPollOutcome(text);
  const SettingEntry* ntp = findIn(mods, n, "ntpServer");
  if (!ntp) { EXPECT(false, "the remote screen lists ntpServer"); return; }
  gWithResult.clear();
  EXPECT(setSettingValueStr(ntp, "x") && commandsAre(gWithResult, {"remote:ntpserver x"}),
         "the keyboard commit goes to the bonded peer, sent: " + joined(gWithResult));
  peerReplies(kPeerMac, true, "ok");
  remoteSettingsPollOutcome(text);
  gExecOk = false;
  gExecText = "Error: No session token - bond not ready or passphrase mismatch";
  EXPECT(!setSettingValueStr(ntp, "y") &&
             std::string(gSettingsEditor.errorMessage.c_str()) == gExecText &&
             gSettingsEditor.errorDisplayUntil > gNowMs,
         "a refused remote send shows the executor's error on the editor line");
}

static void case_editor_commits_local_otherwise() {
  gCase = "editor_commits_local_otherwise";
  resetWorld();
  loadRemoteSettingsModules();
  currentOLEDMode = OLED_SETTINGS;
  setSettingValue(&kLocalOledEntries[0], 5);
  EXPECT(commandsAre(gLocal, {"oledbrightness 5"}) && gWithResult.empty(),
         "the local Settings screen keeps its local setter");
}

static void case_editor_module_source_follows_mode() {
  gCase = "editor_module_source_follows_mode";
  resetWorld();
  loadRemoteSettingsModules();
  size_t remoteN = 0, localN = 0, n = 0;
  const SettingsModule** remote = getRemoteSettingsModules(remoteN);
  const SettingsModule** local = getSettingsModules(localN);
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  const SettingsModule** got = editorModules(n);
  EXPECT(n == remoteN && n > 0 && got[0] == remote[0], "the remote screen edits the peer's module set");
  currentOLEDMode = OLED_SETTINGS;
  got = editorModules(n);
  EXPECT(n == localN && got[0] == local[0], "every other mode edits the local registry");
}

static void case_editor_drops_foreign_module() {
  gCase = "editor_drops_foreign_module";
  resetWorld();
  loadRemoteSettingsModules();
  size_t n = 0;
  const SettingsModule** remote = getRemoteSettingsModules(n);
  if (n == 0) { EXPECT(false, "remote modules loaded"); return; }
  currentOLEDMode = OLED_SETTINGS;
  gSettingsEditor.state = SETTINGS_ITEM_SELECT;
  gSettingsEditor.categoryIndex = 1;
  gSettingsEditor.currentModule = remote[1];
  editorDropForeignModule();
  EXPECT(gSettingsEditor.state == SETTINGS_CATEGORY_SELECT && gSettingsEditor.currentModule == nullptr,
         "the local screen does not keep editing a peer module left behind by the remote screen");
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  gSettingsEditor.state = SETTINGS_ITEM_SELECT;
  gSettingsEditor.categoryIndex = 1;
  gSettingsEditor.currentModule = remote[1];
  editorDropForeignModule();
  EXPECT(gSettingsEditor.state == SETTINGS_ITEM_SELECT && gSettingsEditor.currentModule == remote[1],
         "the remote screen keeps its own module");
}

static void case_policy_matches_web_table() {
  gCase = "policy_matches_web_table";
  for (const char* m : {"users", "bond", "wifi"}) {
    EXPECT(!BondedPeer::remoteEditAllowed(m, nullptr) && !BondedPeer::remoteEditAllowed(m, "anything"),
           std::string("module is never exposed: ") + m);
  }
  EXPECT(BondedPeer::remoteEditAllowed("clock", nullptr) && BondedPeer::remoteEditAllowed("clock", "ntpServer") &&
             BondedPeer::remoteEditAllowed("clock", "tzOffsetMinutes") &&
             !BondedPeer::remoteEditAllowed("clock", "ntpEnabled"),
         "clock exposes only ntpServer and tzOffsetMinutes");
  EXPECT(BondedPeer::remoteEditAllowed("oled", "oledBrightness"), "other modules are exposed");
  EXPECT(!BondedPeer::remoteEditAllowed(nullptr, "x"), "a null module is not exposed");
}

static void case_free_releases_everything() {
  gCase = "free_releases_everything";
  resetWorld();
  String err, text;
  const size_t allocs = gLiveAllocs.size();
  const long strings = gLiveStrings;
  loadRemoteSettingsModules();
  loadRemoteSettingsModules();
  const SettingEntry* b = findRemote("oledBrightness");
  if (b) applyRemoteSettingChange(b, "7", err);
  freeRemoteSettingsModules();
  EXPECT(gLiveAllocs.size() == allocs, "every PSRAM block is released");
  EXPECT(gLiveStrings == strings, "every value-slot String is destroyed");
  EXPECT(remoteModuleCount() == 0, "no module is left");
  EXPECT(remoteSettingsPollOutcome(text) == REMOTE_OUTCOME_NONE, "the pending commit is dropped");
}

static void case_alloc_failure_is_clean() {
  gCase = "alloc_failure_is_clean";
  for (int failAt = 1; failAt <= 4; failAt++) {
    resetWorld();
    const size_t allocs = gLiveAllocs.size();
    const long strings = gLiveStrings;
    gAllocCalls = 0;
    gFailAllocAt = failAt;
    const bool loaded = loadRemoteSettingsModules();
    gFailAllocAt = 0;
    EXPECT(!loaded && std::string(remoteSettingsLastError()) == "Out of memory",
           "allocation " + std::to_string(failAt) + " failing fails the load with 'Out of memory'");
    EXPECT(gLiveAllocs.size() == allocs && gLiveStrings == strings && remoteModuleCount() == 0,
           "nothing is retained after allocation " + std::to_string(failAt) + " fails");
  }
}

static bool shown(const char* text) {
  for (const auto& l : gDisplay.lines) if (l.find(text) != std::string::npos) return true;
  return false;
}

static void case_adapter_reports_peer_answer() {
  gCase = "adapter_reports_peer_answer";
  resetWorld();
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  onEnterRemoteSettings(true);
  gSettingsEditor.state = SETTINGS_ITEM_SELECT;
  displayRemoteSettingsMode();
  EXPECT(gEditorDraws == 1 && gSettingsEditor.state == SETTINGS_CATEGORY_SELECT && remoteModuleCount() == 3,
         "a forward entry loads the peer's set, resets the editor and draws it");
  const SettingEntry* b = findRemote("oledBrightness");
  if (!b) { EXPECT(false, "oledBrightness is listed"); return; }

  setSettingValue(b, 9);
  displayRemoteSettingsMode();
  EXPECT(gDirtyUntil >= gNowMs + 250, "frames keep coming while the commit is pending");
  peerReplies(kPeerMac, false, "Error: value out of range");
  displayRemoteSettingsMode();
  EXPECT(std::string(gSettingsEditor.errorMessage.c_str()) == "Peer: Error: value out of range" &&
             gSettingsEditor.errorDisplayUntil >= gNowMs + 3000,
         "the peer's failure is shown on the editor line, not a local success");
  EXPECT(gToast.empty() && intValue(b) == 128, "no 'Applied' and no value change on failure");

  setSettingValue(b, 10);
  peerReplies(kPeerMac, true, "oledBrightness = 10");
  displayRemoteSettingsMode();
  EXPECT(gToast == "Applied" && intValue(b) == 10, "the peer's success shows 'Applied' and the new value");

  setSettingValue(b, 11);
  gNowMs += 5000;
  displayRemoteSettingsMode();
  EXPECT(std::string(gSettingsEditor.errorMessage.c_str()) == "No reply from peer",
         "silence from the peer is shown as 'No reply from peer'");

  onEnterRemoteSettings(false);
  EXPECT(remoteModuleCount() == 3, "back-navigation keeps the loaded set");
  EXPECT(handleRemoteSettingsInput(0, 0, INPUT_BUTTON_A) && gEditorInputs == 1,
         "input is delegated to the shared editor");
  gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
  EXPECT(!handleRemoteSettingsInput(0, 0, INPUT_BUTTON_B) && remoteModuleCount() == 0,
         "B at the category list frees the set and lets the menu go back");
}

static void case_adapter_load_screens() {
  gCase = "adapter_load_screens";
  resetWorld();
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  gSchemaText.clear();
  onEnterRemoteSettings(true);
  displayRemoteSettingsMode();
  EXPECT(shown("Fetching peer") && gEditorDraws == 0 && gMenuBacks == 0,
         "a missing schema shows the fetch screen");
  EXPECT(!handleRemoteSettingsInput(0, 0, INPUT_BUTTON_B) && gEditorInputs == 0,
         "B while fetching leaves the screen");

  resetWorld();
  currentOLEDMode = OLED_REMOTE_SETTINGS;
  gSchemaText.clear();
  gOnline = false;
  onEnterRemoteSettings(true);
  displayRemoteSettingsMode();
  EXPECT(shown("No remote settings") && shown("Peer offline") && gMenuBacks == 1,
         "a failed load says why and returns to the menu");
}

int main() {
  case_cmd_result_record();
  case_build_uses_schema_cmdkeys();
  case_apply_sends_schema_command();
  case_apply_reports_peer_failure();
  case_apply_success_updates_cached_value();
  case_apply_reports_no_reply();
  case_apply_ignores_other_peer_reply();
  case_apply_refuses();
  case_load_step_fetches_schema_once();
  case_editor_commits_remote_in_remote_mode();
  case_editor_commits_local_otherwise();
  case_editor_module_source_follows_mode();
  case_editor_drops_foreign_module();
  case_policy_matches_web_table();
  case_free_releases_everything();
  case_alloc_failure_is_clean();
  case_adapter_reports_peer_answer();
  case_adapter_load_screens();
  freeRemoteSettingsModules();
  if (gFailures) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("oled_remote_settings: all cases passed\n");
  return 0;
}
