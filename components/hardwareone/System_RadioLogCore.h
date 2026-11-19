// The radio stacks' own ESP-IDF log tags: quiet unless their debug flag is on.
//
// ESP-Hosted (the P4X-EYE's link to its C6 companion), the C6 console mirror
// and Bluedroid log routine lines at INFO and WARN in normal operation: every
// Hosted session start, every station state change, every connect and
// disconnect, every C6 RPC. By default each tag listed here is held at ERROR,
// so real failures still print; `debugwifidriver` (Hosted, transport) and
// `debugbluetoothcore` (Bluedroid, BLE records) bring the stock level back, and
// either brings back the C6 console. A saved `esplog` entry for a tag wins over
// both, and a saved `esplog *` takes the place of the quiet default.
//
// Dependency-free so the host tests run it as shipped; System_Debug.cpp applies
// the table (applyRadioStackLogLevels) and HAL_Radio.cpp sets the Hosted rows
// quiet before the settings load and classifies the C6 console's lines.
#ifndef SYSTEM_RADIO_LOG_CORE_H
#define SYSTEM_RADIO_LOG_CORE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "System_EspLogCore.h"

namespace radiolog {

// ESP_LOG_ERROR / ESP_LOG_WARN / ESP_LOG_INFO.
constexpr int kError = 1;
constexpr int kWarn = 2;
constexpr int kInfo = 3;
constexpr int kQuiet = kError;  // the default for every tag below

// The C6 console mirror's own tag (HAL_Radio's console task).
constexpr const char* kC6ConsoleTag = "C6>";

// Which debug flag brings a tag back: debugwifidriver, debugbluetoothcore, or
// either (the C6 console carries the companion's Wi-Fi and Bluetooth alike).
enum class Family : uint8_t { WifiDriver, Bluetooth, Companion };

// Which part of the build the tag belongs to; a tag of a stack the build lacks
// is not set (each set tag costs an ESP-IDF tag-list entry).
enum class Stack : uint8_t {
  Hosted,          // ESP-Hosted host side and the C6 console (P4X-EYE companion builds)
  Bluedroid,       // the Bluetooth host stack (every Bluetooth build)
  BleController,   // the on-chip BLE controller's init (ESP32-S3 and the like)
  BtdmController,  // the classic ESP32's controller init
  CoexRecords,     // HardwareOne's BLE_COEX records (ENABLE_BLE_COEX_RECORDS)
};

struct TagRow {
  const char* tag;
  Family family;
  Stack stack;
  int8_t flagOnLevel;  // the stock level: what the tag had before this table
};

// Every tag here was read from this build's sources (.sdk-overrides/esp32p4
// espressif__esp_hosted host/, bt/host/bluedroid bt_trace.h and bt_common.h,
// ESP-IDF bt/controller) and seen logging routine lines on the bench
// (2026-10..11 P4X-EYE logs). Bluedroid's trace levels stop at WARNING in
// every configuration, so its tags print only W and E lines either way.
constexpr TagRow kTags[] = {
  // ESP-Hosted, each session (boot, recovery, release, idle reopen): ~40 lines.
  {"transport", Family::WifiDriver, Stack::Hosted, kInfo},        // EVENT, retry, delayed_init, state
  {"H_SDIO_DRV", Family::WifiDriver, Stack::Hosted, kInfo},       // tasks started; W clock and slave reset
  {"sdio_wrapper", Family::WifiDriver, Stack::Hosted, kInfo},     // slot, pins, queues, block sizes
  {"H_API", Family::WifiDriver, Stack::Hosted, kInfo},            // transport active; each connect attempt
  {"RPC_WRAP", Family::WifiDriver, Stack::Hosted, kInfo},         // each station start/stop/connect/disconnect
  {"rpc_core", Family::WifiDriver, Stack::Hosted, kWarn},         // esp_hosted_init sets WARN
  {"rpc_rsp", Family::WifiDriver, Stack::Hosted, kWarn},          // W per failed RPC answer
  {"rpc_evt", Family::WifiDriver, Stack::Hosted, kWarn},          // esp_hosted_init sets WARN
  {"rpc_utils", Family::WifiDriver, Stack::Hosted, kInfo},        // W "Event: SSID" per config read
  {"vhci_drv", Family::WifiDriver, Stack::Hosted, kInfo},         // BT transport type, each session
  {"os_wrapper_esp", Family::WifiDriver, Stack::Hosted, kInfo},   // "GPIO configured", each session
  {"esp_cli", Family::WifiDriver, Stack::Hosted, kInfo},          // 8 "Registering command", each session
  {"esp_wifi_remote", Family::WifiDriver, Stack::Hosted, kInfo},  // rx callback, each station start
  {kC6ConsoleTag, Family::Companion, Stack::Hosted, kInfo},       // the companion's own log, per RPC
  // Bluedroid (W lines on every connect, disconnect and unmatched notify).
  {"BT_APPL", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_BTM", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_HCI", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_L2CAP", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_GATT", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_SMP", Family::Bluetooth, Stack::Bluedroid, kInfo},
  {"BT_BTC", Family::Bluetooth, Stack::Bluedroid, kInfo},
  // On-chip controller init, each Bluetooth start (the P4's is on the C6).
  {"BLE_INIT", Family::Bluetooth, Stack::BleController, kInfo},
  {"BTDM_INIT", Family::Bluetooth, Stack::BtdmController, kInfo},
  // HardwareOne's own BLE records: two lines per scan and per connect.
  {"BLE_COEX", Family::Bluetooth, Stack::CoexRecords, kInfo},
};
constexpr size_t kTagCount = sizeof(kTags) / sizeof(kTags[0]);

// The row of `tag`, or nullptr.
inline const TagRow* find(const char* tag) {
  if (!tag) return nullptr;
  for (const TagRow& row : kTags)
    if (!std::strcmp(row.tag, tag)) return &row;
  return nullptr;
}

inline bool familyOn(Family family, bool wifiDriver, bool bluetoothCore) {
  switch (family) {
    case Family::WifiDriver: return wifiDriver;
    case Family::Bluetooth: return bluetoothCore;
    case Family::Companion: return wifiDriver || bluetoothCore;
  }
  return false;
}

// The level a tag gets: its saved esplog entry, else its stock level while its
// flag is on, else the saved esplog wildcard, else the quiet default.
inline int levelFor(const TagRow& row, bool wifiDriver, bool bluetoothCore, const char* esplogStored) {
  const int saved = esplog::levelOf(esplogStored, row.tag);
  if (saved >= 0) return saved;
  if (familyOn(row.family, wifiDriver, bluetoothCore)) return row.flagOnLevel;
  const int all = esplog::levelOf(esplogStored, "*");
  return all >= 0 ? all : kQuiet;
}

// ---- the C6 console mirror's line levels ------------------------------------
// The mirror relays the companion's UART console line by line. Each line is
// relayed at the level the C6 printed it with ("E (123) tag: ..."), so the
// quiet default keeps the companion's errors. Lines without that prefix are
// the ROM banner after a reset and raw prints (Info), unless a crash report
// began: from a panic, abort, assert, stack overflow, brownout, failed
// ESP_ERROR_CHECK or heap corruption line, or a task watchdog's
// "E (..) task_wdt: Print CPU n ... registers|backtrace" line, every
// unprefixed line (the failed call's file and expression, register dump,
// backtrace, the reset that follows) is an error until the next prefixed
// line. ESP_ERROR_CHECK and CORRUPT HEAP print without a level prefix
// (esp_rom_printf) before their abort, so they must start the report
// themselves.
enum class LineLevel : uint8_t { Error, Warn, Info };

class C6ConsoleLevels {
 public:
  // `continued` is true for the rest of a line the mirror split because it
  // was longer than its buffer: it keeps the level of the line's start.
  LineLevel classify(const char* line, bool continued = false) {
    if (!line) return LineLevel::Info;
    if (continued) return last_;
    const char* p = line;
    // An ANSI colour prefix (the C6 builds without colours; tolerated).
    if (p[0] == '\x1b' && p[1] == '[') {
      const char* m = std::strchr(p, 'm');
      if (m) p = m + 1;
    }
    if (p[0] && std::strchr("EWIDV", p[0]) && p[1] == ' ' && p[2] == '(') {
      // The task watchdog's register or backtrace dump follows its own E
      // line unprefixed.
      crash_ = p[0] == 'E' && std::strstr(p, "task_wdt: Print CPU") != nullptr;
      last_ = p[0] == 'E' ? LineLevel::Error : p[0] == 'W' ? LineLevel::Warn : LineLevel::Info;
      return last_;
    }
    if (crashMarker(p)) crash_ = true;
    last_ = crash_ ? LineLevel::Error : LineLevel::Info;
    return last_;
  }
  bool inCrashReport() const { return crash_; }

 private:
  static bool crashMarker(const char* p) {
    static constexpr const char* kMarkers[] = {
        "Guru Meditation Error", "abort() was called", "assert failed", "***ERROR***",
        "Stack smashing",        "Brownout detector",  "CPU halted",    "register dump",
        "ESP_ERROR_CHECK",       "CORRUPT HEAP",
    };
    for (const char* marker : kMarkers)
      if (std::strstr(p, marker)) return true;
    return false;
  }
  bool crash_ = false;
  LineLevel last_ = LineLevel::Info;
};

}  // namespace radiolog

#endif  // SYSTEM_RADIO_LOG_CORE_H
