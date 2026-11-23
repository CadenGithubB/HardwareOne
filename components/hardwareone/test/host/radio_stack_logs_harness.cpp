// Executes the shipping System_RadioLogCore.h (included as is), the esplog
// apply/command block and the radio stack apply from System_Debug.cpp, and
// HAL_Radio's quietHostedLogs() (inserted by test_radio_stack_logs.py) against
// a model of ESP-IDF's tag levels: a default level, per-tag levels, and "*"
// setting the default and dropping every tag's own level, as
// log_level/tag_log_level does. Which stacks the build has and which debug
// flags are on are the harness's (System_Debug.cpp reads them from the build
// configuration and the runtime debug layers; the driver pins those in source).
#include "Arduino.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "System_EspLogCore.h"
#include "System_RadioLogCore.h"

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static std::vector<std::string> events;
static std::map<std::string, int> gTagLevel;
static int gDefaultLevel = ESP_LOG_INFO;  // CONFIG_LOG_DEFAULT_LEVEL=3
// The esplog lock (System_Debug.cpp espLogLock: a FreeRTOS mutex, not
// recursive). The harness checks it is never taken twice, always released,
// and counts the tag sets made while it is held.
static int gLockDepth = 0, gLockTakes = 0, gLockedSets = 0;
static void espLogLock() {
  CHECK(gLockDepth == 0);
  ++gLockDepth;
  ++gLockTakes;
}
static void espLogUnlock() {
  CHECK(gLockDepth == 1);
  --gLockDepth;
}
void esp_log_level_set(const char* tag, esp_log_level_t level) {
  events.push_back(tag);
  if (gLockDepth) ++gLockedSets;
  if (!std::strcmp(tag, "*")) {
    gDefaultLevel = level;
    gTagLevel.clear();
  } else {
    gTagLevel[tag] = level;
  }
}
// The firmware sets its tags through the boot debug capture
// (System_BootDebug.h), which is esp_log_level_set outside the boot.
#define bootDebugEspLogSet esp_log_level_set
static int effective(const char* tag) {
  const auto it = gTagLevel.find(tag);
  return it == gTagLevel.end() ? gDefaultLevel : it->second;
}
static void resetLog() {
  gTagLevel.clear();
  gDefaultLevel = ESP_LOG_INFO;
  events.clear();
}
#define CONFIG_LOG_MAXIMUM_LEVEL 3

struct HostSettings { String espLogLevels; } gSettings;
bool requestDebugSettingsPersist() { CHECK(gLockDepth == 0); return true; }  // debug.json is written unlocked

class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    std::string text(input.c_str());
    const auto first = text.find_first_not_of(" \t\r\n");
    raw_ = first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    size_t pos = 0;
    while (pos < raw_.size()) {
      while (pos < raw_.size() && raw_[pos] == ' ') ++pos;
      if (pos >= raw_.size()) break;
      const size_t start = pos;
      while (pos < raw_.size() && raw_[pos] != ' ') ++pos;
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

// The build and the flags (System_Debug.cpp: radioLogStackPresent and the
// runtime debug layers). The P4X-EYE: Hosted, Bluedroid, the BLE records, no
// on-chip controller.
static bool gStack[5] = {true, true, false, false, true};
static bool gWifiDriver = false, gBluetoothCore = false;
static bool radioLogStackPresent(radiolog::Stack stack) { return gStack[static_cast<int>(stack)]; }
static bool radioLogWifiDriverOn() { return gWifiDriver; }
static bool radioLogBluetoothCoreOn() { return gBluetoothCore; }

void applyEspLogOverrides(bool includeWildcard);
void applyRadioStackLogLevels();
// INSERT_ESPLOG
// INSERT_RADIO_APPLY
namespace hal {
// INSERT_QUIET_HOSTED
}  // namespace hal

static bool has(const std::string& reply, const char* part) { return reply.find(part) != std::string::npos; }
static std::string run(const char* args) {
  events.clear();
  return cmd_esplog(String(args));
}
using radiolog::kTags;

static const char* const kHosted[] = {"transport", "H_SDIO_DRV", "sdio_wrapper", "H_API", "RPC_WRAP", "rpc_core",
                                      "rpc_rsp", "rpc_evt", "rpc_utils", "vhci_drv", "os_wrapper_esp", "esp_cli",
                                      "esp_wifi_remote"};
static const char* const kBluedroid[] = {"BT_APPL", "BT_BTM", "BT_HCI", "BT_L2CAP", "BT_GATT", "BT_SMP", "BT_BTC"};

static int stockOf(const char* tag) { return radiolog::find(tag)->flagOnLevel; }

// Every present tag at `level`, or (level < 0) at its stock level.
static bool allAt(const char* const* tags, size_t n, int level) {
  for (size_t i = 0; i < n; ++i)
    if (effective(tags[i]) != (level < 0 ? stockOf(tags[i]) : level)) return false;
  return true;
}
#define ALL_AT(list, level) allAt(list, sizeof(list) / sizeof(list[0]), level)

static void coreChecks() {
  using namespace radiolog;
  // The table: unique tags, each a valid esplog tag, so `esplog <tag> ...` can
  // name every one; every Hosted and Bluedroid tag above is in it.
  CHECK(kTagCount == 24);
  for (size_t i = 0; i < kTagCount; ++i) {
    CHECK(esplog::validTag(kTags[i].tag));
    CHECK(find(kTags[i].tag) == &kTags[i]);
    CHECK(kTags[i].flagOnLevel == kWarn || kTags[i].flagOnLevel == kInfo);
  }
  for (const char* tag : kHosted) CHECK(find(tag) && find(tag)->family == Family::WifiDriver && find(tag)->stack == Stack::Hosted);
  for (const char* tag : kBluedroid) CHECK(find(tag) && find(tag)->family == Family::Bluetooth && find(tag)->stack == Stack::Bluedroid);
  CHECK(find("C6>")->family == Family::Companion && find("C6>")->stack == Stack::Hosted && find("C6>")->flagOnLevel == kInfo);
  CHECK(find("BLE_COEX")->stack == Stack::CoexRecords && find("BLE_COEX")->family == Family::Bluetooth);
  CHECK(find("BLE_INIT")->stack == Stack::BleController && find("BTDM_INIT")->stack == Stack::BtdmController);
  // The stock levels: esp_hosted_init's WARN for its three RPC tags, the
  // default INFO for the rest.
  for (const char* tag : {"rpc_core", "rpc_rsp", "rpc_evt"}) CHECK(stockOf(tag) == kWarn);
  for (const char* tag : {"transport", "rpc_utils", "RPC_WRAP", "BT_BTM", "BLE_COEX"}) CHECK(stockOf(tag) == kInfo);
  CHECK(!find("wifi") && !find("HTTP_CLIENT") && !find(nullptr) && !find("c6>"));
  CHECK(kQuiet == ESP_LOG_ERROR && kError == ESP_LOG_ERROR && kWarn == ESP_LOG_WARN && kInfo == ESP_LOG_INFO);

  // Which flag owns which family.
  CHECK(!familyOn(Family::WifiDriver, false, true) && familyOn(Family::WifiDriver, true, false));
  CHECK(!familyOn(Family::Bluetooth, true, false) && familyOn(Family::Bluetooth, false, true));
  CHECK(!familyOn(Family::Companion, false, false) && familyOn(Family::Companion, true, false) &&
        familyOn(Family::Companion, false, true));

  // levelFor: saved entry > flag > saved wildcard > quiet.
  const TagRow& rsp = *find("rpc_rsp");
  CHECK(levelFor(rsp, false, false, "") == kError);
  CHECK(levelFor(rsp, true, false, "") == kWarn);
  CHECK(levelFor(rsp, false, true, "") == kError);
  CHECK(levelFor(rsp, false, false, "rpc_rsp:I") == kInfo && levelFor(rsp, true, false, "rpc_rsp:N") == 0);
  CHECK(levelFor(rsp, false, false, "*:V") == 5 && levelFor(rsp, true, false, "*:N") == kWarn);
  CHECK(levelFor(rsp, true, false, "*:N,rpc_rsp:E") == kError);
  CHECK(levelFor(rsp, false, false, "x,::,rpc_rsp:Q") == kError);  // malformed entries are skipped
  CHECK(levelFor(rsp, false, false, nullptr) == kError);
  CHECK(levelFor(*find("C6>"), false, true, "") == kInfo && levelFor(*find("C6>"), true, false, "") == kInfo);
}

static void c6ConsoleChecks() {
  using radiolog::LineLevel;
  radiolog::C6ConsoleLevels c6;
  // Prefixed lines keep the C6's own level; D and V are info.
  CHECK(c6.classify("I (5123) slave_rpc: RPC Req [0x15e] received") == LineLevel::Info);
  CHECK(c6.classify("W (5130) wifi:<ba-add>idx:0, ifx:0, tid:0") == LineLevel::Warn);
  CHECK(c6.classify("E (5140) slave_wifi_std: req_wifi_get_mode:1 failed") == LineLevel::Error);
  CHECK(c6.classify("D (1) x: y") == LineLevel::Info && c6.classify("V (1) x: y") == LineLevel::Info);
  CHECK(c6.classify("\x1b[0;31mE (9) x: y\x1b[0m") == LineLevel::Error);
  CHECK(c6.classify("\x1b[0;33mW (9) x: y\x1b[0m") == LineLevel::Warn);
  // Unprefixed: the ROM banner of a plain reset, raw prints, near-misses.
  for (const char* line : {"ESP-ROM:esp32c6-20220919", "rst:0x1 (POWERON),boot:0x6c (SPI_FAST_FLASH_BOOT)",
                           "load:0x4086c410,len:0xcf8", "entry 0x4086c410", "E", "E(1)", "Error in", "",
                           "Ia (1) x", "*********************"}) {
    CHECK(c6.classify(line) == LineLevel::Info);
  }
  CHECK(!c6.inCrashReport() && c6.classify(nullptr) == LineLevel::Info);
  // A panic: every unprefixed line is an error until the next prefixed one,
  // the ROM banner of the reset that follows included.
  CHECK(c6.classify("Guru Meditation Error: Core  0 panic'ed (Load access fault). Exception was unhandled.") ==
        LineLevel::Error);
  CHECK(c6.inCrashReport());
  for (const char* line : {"", "Core  0 register dump:", "MEPC    : 0x42001234  RA      : 0x42005678",
                           "Stack memory:", "ELF file SHA256: 0123456789abcdef", "Rebooting...",
                           "ESP-ROM:esp32c6-20220919", "rst:0xc (SW_CPU),boot:0x6c (SPI_FAST_FLASH_BOOT)"}) {
    CHECK(c6.classify(line) == LineLevel::Error);
  }
  CHECK(c6.classify("I (24) boot: ESP-IDF v5.5 2nd stage bootloader") == LineLevel::Info && !c6.inCrashReport());
  CHECK(c6.classify("rst:0x1 (POWERON)") == LineLevel::Info);
  for (const char* marker : {"abort() was called at PC 0x420123 on core 0", "assert failed: xQueueSend queue.c:123",
                             "***ERROR*** A stack overflow in task sys_evt has been detected.",
                             "Stack smashing protect failure!", "Brownout detector was triggered",
                             "CPU halted."}) {
    radiolog::C6ConsoleLevels fresh;
    CHECK(fresh.classify(marker) == LineLevel::Error && fresh.inCrashReport());
    CHECK(fresh.classify("Backtrace: 0x4200:0x3fc8") == LineLevel::Error);
    CHECK(fresh.classify("W (1) x: y") == LineLevel::Warn && !fresh.inCrashReport());
  }
  // The rest of a line split by the mirror keeps its start's level.
  radiolog::C6ConsoleLevels split;
  CHECK(split.classify("E (5) slave_wifi_std: a very long line") == LineLevel::Error);
  CHECK(split.classify("I (6) x: looks prefixed but is the rest", true) == LineLevel::Error);
  CHECK(split.classify("I (7) x: a new line") == LineLevel::Info);
  CHECK(split.classify("E (8) x: the rest", true) == LineLevel::Info);
}

static void applyChecks() {
  // Default, P4X-EYE: every present tag ERROR, in table order; no controller
  // tags (the P4's controller is on the C6, behind C6>).
  gSettings.espLogLevels = "";
  resetLog();
  gLockTakes = gLockedSets = 0;
  applyRadioStackLogLevels();
  CHECK(events.size() == 22);
  CHECK(gLockTakes == 1 && gLockedSets == 22 && gLockDepth == 0);  // one apply, whole, under the lock
  CHECK(events.front() == "transport" && events.back() == "BLE_COEX");
  CHECK(effective("BLE_INIT") == ESP_LOG_INFO && effective("BTDM_INIT") == ESP_LOG_INFO);  // never set
  CHECK(ALL_AT(kHosted, ESP_LOG_ERROR) && ALL_AT(kBluedroid, ESP_LOG_ERROR));
  CHECK(effective("C6>") == ESP_LOG_ERROR && effective("BLE_COEX") == ESP_LOG_ERROR);
  CHECK(effective("wifi") == ESP_LOG_INFO && effective("HW1_RADIO") == ESP_LOG_INFO);  // not radio stack tags

  // debugwifidriver on: Hosted and C6> at their stock level; Bluetooth stays quiet.
  gWifiDriver = true;
  applyRadioStackLogLevels();
  CHECK(ALL_AT(kHosted, -1) && effective("rpc_core") == ESP_LOG_WARN && effective("transport") == ESP_LOG_INFO);
  CHECK(effective("C6>") == ESP_LOG_INFO && ALL_AT(kBluedroid, ESP_LOG_ERROR) && effective("BLE_COEX") == ESP_LOG_ERROR);
  // ... and off again: quiet again.
  gWifiDriver = false;
  applyRadioStackLogLevels();
  CHECK(ALL_AT(kHosted, ESP_LOG_ERROR) && effective("C6>") == ESP_LOG_ERROR);

  // debugbluetoothcore on: Bluedroid, BLE_COEX and C6> at stock; Hosted quiet.
  gBluetoothCore = true;
  applyRadioStackLogLevels();
  CHECK(ALL_AT(kBluedroid, ESP_LOG_INFO) && effective("BLE_COEX") == ESP_LOG_INFO && effective("C6>") == ESP_LOG_INFO);
  CHECK(ALL_AT(kHosted, ESP_LOG_ERROR));
  gBluetoothCore = false;
  applyRadioStackLogLevels();
  CHECK(ALL_AT(kBluedroid, ESP_LOG_ERROR) && effective("BLE_COEX") == ESP_LOG_ERROR && effective("C6>") == ESP_LOG_ERROR);

  // A saved esplog level wins over the default and over the flag, and keeps
  // winning when the flag changes.
  CHECK(run("rpc_utils warn") == "rpc_utils: warn (saved)" && effective("rpc_utils") == ESP_LOG_WARN);
  CHECK(run("BT_BTM none") == "BT_BTM: none (saved)" && effective("BT_BTM") == ESP_LOG_NONE);
  CHECK(run("C6> warn") == "C6>: warn (saved)");
  for (int flags = 0; flags < 4; ++flags) {
    gWifiDriver = flags & 1;
    gBluetoothCore = flags & 2;
    applyRadioStackLogLevels();
    CHECK(effective("rpc_utils") == ESP_LOG_WARN && effective("BT_BTM") == ESP_LOG_NONE && effective("C6>") == ESP_LOG_WARN);
    CHECK(effective("transport") == (gWifiDriver ? ESP_LOG_INFO : ESP_LOG_ERROR));
    CHECK(effective("BT_APPL") == (gBluetoothCore ? ESP_LOG_INFO : ESP_LOG_ERROR));
  }
  // Removing the entry: the tag follows its flag again at once.
  gWifiDriver = true;
  gBluetoothCore = false;
  applyRadioStackLogLevels();
  CHECK(run("rpc_utils default") == "rpc_utils: saved level removed; it follows its debug flag again");
  CHECK(effective("rpc_utils") == ESP_LOG_INFO);
  gWifiDriver = false;
  CHECK(has(run("C6> default"), "it follows its debug flag again") && effective("C6>") == ESP_LOG_ERROR);
  CHECK(has(run("clear"), "cleared") && effective("BT_BTM") == ESP_LOG_ERROR && effective("rpc_utils") == ESP_LOG_ERROR);
  // A save takes the lock for its assignment, then the apply takes it again
  // (never nested); a refused edit does not take it.
  gLockTakes = 0;
  CHECK(run("rpc_utils warn") == "rpc_utils: warn (saved)" && gLockTakes == 1 && gLockDepth == 0);
  CHECK(has(run("rpc_utils default"), "follows its debug flag again") && gLockTakes == 3 && gLockDepth == 0);
  CHECK(has(run("rpc_utils loud"), "Error") && gLockTakes == 3);

  // The wildcard: in place of the quiet default while a flag is off; a flag
  // that is on wins over it; tag entries win over both.
  gBluetoothCore = true;
  CHECK(has(run("* warn"), "*: warn (saved)"));
  CHECK(gDefaultLevel == ESP_LOG_WARN && ALL_AT(kHosted, ESP_LOG_WARN) && effective("C6>") == ESP_LOG_INFO);
  CHECK(ALL_AT(kBluedroid, ESP_LOG_INFO) && effective("wifi") == ESP_LOG_WARN);
  gBluetoothCore = false;
  CHECK(has(run("rpc_rsp:error,*:none"), "ESP-IDF log levels saved: *:N,rpc_rsp:E"));
  CHECK(gDefaultLevel == ESP_LOG_NONE && effective("rpc_rsp") == ESP_LOG_ERROR && effective("transport") == ESP_LOG_NONE);
  CHECK(ALL_AT(kBluedroid, ESP_LOG_NONE) && effective("C6>") == ESP_LOG_NONE);
  // Removing the wildcard puts the radio tags back on the quiet default at
  // once (other tags keep the old default until the next reboot).
  CHECK(has(run("* default"), "*: saved level removed; the radio stack tags follow their debug flags again"));
  CHECK(effective("transport") == ESP_LOG_ERROR && effective("rpc_rsp") == ESP_LOG_ERROR);
  run("clear");
}

// The boot: HAL_Radio's quiet start before the settings load, applySettings'
// apply, then the boot's esplog apply (wildcard included).
static void boot(const char* stored, bool wifiDriver, bool bluetoothCore) {
  resetLog();
  gSettings.espLogLevels = stored;
  gWifiDriver = gBluetoothCore = false;  // not loaded yet
  if (gStack[0]) hal::quietHostedLogs();  // HAL_Radio exists only in companion builds
  gWifiDriver = wifiDriver;
  gBluetoothCore = bluetoothCore;
  applyRadioStackLogLevels();   // applySettings()
  applyEspLogOverrides(true);   // hardwareone_setup(), network phase
}
// A new Hosted session: esp_hosted_init() sets its three RPC tags to WARN,
// then the companion's TRANSPORT_UP (or failure) handler applies again.
static void hostedSession() {
  esp_log_level_set("rpc_core", ESP_LOG_WARN);
  esp_log_level_set("rpc_rsp", ESP_LOG_WARN);
  esp_log_level_set("rpc_evt", ESP_LOG_WARN);
  applyRadioStackLogLevels();
}

static void bootChecks() {
  // HAL's quiet start sets exactly the Hosted rows (C6> included) to ERROR.
  resetLog();
  hal::quietHostedLogs();
  CHECK(events.size() == 14 && ALL_AT(kHosted, ESP_LOG_ERROR) && effective("C6>") == ESP_LOG_ERROR);
  CHECK(effective("BT_BTM") == ESP_LOG_INFO);

  boot("", false, false);
  CHECK(ALL_AT(kHosted, ESP_LOG_ERROR) && ALL_AT(kBluedroid, ESP_LOG_ERROR) && effective("C6>") == ESP_LOG_ERROR);
  hostedSession();
  CHECK(effective("rpc_core") == ESP_LOG_ERROR && effective("rpc_rsp") == ESP_LOG_ERROR && effective("rpc_evt") == ESP_LOG_ERROR);

  boot("", true, false);  // debugwifidriver saved on
  CHECK(ALL_AT(kHosted, -1) && effective("C6>") == ESP_LOG_INFO && ALL_AT(kBluedroid, ESP_LOG_ERROR));
  hostedSession();
  CHECK(ALL_AT(kHosted, -1));

  boot("*:I", false, false);  // a saved wildcard takes the quiet default's place
  CHECK(ALL_AT(kHosted, ESP_LOG_INFO) && ALL_AT(kBluedroid, ESP_LOG_INFO) && gTagLevel.empty() == false);
  hostedSession();  // and holds over a reconnect
  CHECK(effective("rpc_core") == ESP_LOG_INFO && effective("rpc_rsp") == ESP_LOG_INFO);

  boot("*:N,rpc_rsp:W,BT_HCI:I", true, false);
  CHECK(effective("rpc_rsp") == ESP_LOG_WARN && effective("rpc_core") == ESP_LOG_WARN && effective("transport") == ESP_LOG_INFO);
  CHECK(effective("BT_HCI") == ESP_LOG_INFO && effective("BT_BTM") == ESP_LOG_NONE && effective("wifi") == ESP_LOG_NONE);
  hostedSession();
  CHECK(effective("rpc_rsp") == ESP_LOG_WARN && effective("rpc_evt") == ESP_LOG_WARN);

  boot("rpc_core:I,BT_GATT:W", false, true);
  hostedSession();
  CHECK(effective("rpc_core") == ESP_LOG_INFO && effective("rpc_rsp") == ESP_LOG_ERROR && effective("BT_GATT") == ESP_LOG_WARN);
  CHECK(effective("BT_SMP") == ESP_LOG_INFO && effective("C6>") == ESP_LOG_INFO);

  // A native build (ESP32-S3: on-chip controller, no Hosted): the controller
  // init tag and Bluedroid, nothing of the companion's.
  gStack[0] = false;
  gStack[2] = true;
  boot("", false, false);
  CHECK(effective("BLE_INIT") == ESP_LOG_ERROR && effective("BTDM_INIT") == ESP_LOG_INFO);
  CHECK(effective("transport") == ESP_LOG_INFO && effective("C6>") == ESP_LOG_INFO && ALL_AT(kBluedroid, ESP_LOG_ERROR));
  gStack[0] = true;
  gStack[2] = false;
}

int main() {
  coreChecks();
  c6ConsoleChecks();
  applyChecks();
  bootChecks();
  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("RADIO_STACK_LOGS PASS");
  return 0;
}
