#pragma once
// The device's own Wi-Fi activity as the Wi-Fi/Bluetooth coexistence scheduler
// sees it, for the BLE measurement records (docs/WIFI_BLE_COEXISTENCE_STUDY.md,
// lever 1). The coexistence scheme switches on these states: while a BLE scan
// runs it gets about 40% of the radio with Wi-Fi associated, 30% while Wi-Fi
// connects and 50% during a Wi-Fi scan.
//
// No Arduino, ESP-IDF or project dependencies, so host tests include it as is.
// System_WiFi samples the inputs from cached state; the classification is here.
#include <cstdint>

enum class WifiCoex : uint8_t {
  NoWifi = 0,  // Wi-Fi compiled out
  Unknown,     // the event mirror is not installed yet
  Off,         // radio off, forced off, or the P4's companion is down
  Idle,        // radio up, not associated and not trying
  Connecting,  // a connect attempt (including the boot connect) is running
  Hunting,     // auto-reconnect retrying after a recent disconnect (inferred)
  Scanning,    // a Wi-Fi scan is on air (checked before Associated)
  Associated,
};

enum : uint8_t {
  WIFI_COEX_ESPNOW = 1,            // ESP-NOW owns the radio (AP+STA parking)
  WIFI_COEX_AP_UP = 2,
  WIFI_COEX_STA_UP = 4,
  WIFI_COEX_HAS_IP = 8,
  WIFI_COEX_FORCED_OFF = 16,       // `radiopower off`
  WIFI_COEX_COMPANION_DOWN = 32,   // P4: the C6 transport is down
  WIFI_COEX_RECONNECT_ARMED = 64,  // Arduino auto-reconnect is on
  WIFI_COEX_BOOT_HUNT = 128,       // the background boot connect is running
};

// No addresses or network names. The counters run from boot and wrap; a
// record keeps their change over its span, so activity that starts and ends
// inside a long BLE scan or connect is still seen.
struct WifiCoexTag {
  WifiCoex state = WifiCoex::Unknown;
  uint8_t flags = 0;
  uint8_t lastReason = 0;      // last STA disconnect reason this boot (0: none)
  uint8_t lastDiscAgeS = 255;  // seconds since it, capped at 254 (255: none)
  uint16_t scans = 0;          // Wi-Fi scans started by this firmware
  uint16_t connects = 0;       // connect attempts started by this firmware
  uint16_t disconnects = 0;    // STA disconnect events, including auto-reconnect failures
};

struct WifiCoexInputs {
  bool installed = false, transportUp = false, staUp = false, apUp = false;
  bool associated = false, hasIp = false, scanOnAir = false, connecting = false;
  bool bootHunt = false, reconnectArmed = false, forcedOff = false, espnow = false;
  uint8_t lastReason = 0;
  uint32_t lastDiscAgeMs = UINT32_MAX;  // UINT32_MAX: no disconnect this boot
  uint16_t scans = 0, connects = 0, disconnects = 0;
};

// Arduino's reconnect retries at once on a reconnectable reason, so a hunting
// station disconnects again within one failed attempt. The window is a
// heuristic: the raw reason and its age are kept to re-derive it from records.
constexpr uint32_t kWifiCoexHuntWindowMs = 10000;
constexpr uint8_t kWifiReasonAssocLeave = 8;  // voluntary leave: never retried

inline WifiCoexTag wifiCoexClassify(const WifiCoexInputs& in) {
  WifiCoexTag t;
  t.flags = uint8_t((in.espnow ? WIFI_COEX_ESPNOW : 0) | (in.apUp ? WIFI_COEX_AP_UP : 0) |
                    (in.staUp ? WIFI_COEX_STA_UP : 0) | (in.hasIp ? WIFI_COEX_HAS_IP : 0) |
                    (in.forcedOff ? WIFI_COEX_FORCED_OFF : 0) |
                    (in.transportUp ? 0 : WIFI_COEX_COMPANION_DOWN) |
                    (in.reconnectArmed ? WIFI_COEX_RECONNECT_ARMED : 0) |
                    (in.bootHunt ? WIFI_COEX_BOOT_HUNT : 0));
  const bool seen = in.lastDiscAgeMs != UINT32_MAX;
  t.lastReason = seen ? in.lastReason : 0;
  t.lastDiscAgeS = !seen ? 255 : in.lastDiscAgeMs / 1000 > 254 ? 254 : uint8_t(in.lastDiscAgeMs / 1000);
  t.scans = in.scans; t.connects = in.connects; t.disconnects = in.disconnects;
  if (!in.installed) { t.state = WifiCoex::Unknown; return t; }
  if (in.forcedOff || !in.transportUp || !(in.staUp || in.apUp)) { t.state = WifiCoex::Off; return t; }
  // A scan while associated runs the scan scheme, so it wins.
  if (in.scanOnAir) { t.state = WifiCoex::Scanning; return t; }
  if (in.associated) { t.state = WifiCoex::Associated; return t; }
  if (in.connecting || in.bootHunt) { t.state = WifiCoex::Connecting; return t; }
  if (in.staUp && in.reconnectArmed && seen && in.lastDiscAgeMs < kWifiCoexHuntWindowMs &&
      in.lastReason != kWifiReasonAssocLeave) {
    t.state = WifiCoex::Hunting; return t;
  }
  t.state = WifiCoex::Idle;
  return t;
}

inline const char* wifiCoexName(WifiCoex s) {
  switch (s) {
    case WifiCoex::NoWifi: return "none";
    case WifiCoex::Unknown: return "unknown";
    case WifiCoex::Off: return "off";
    case WifiCoex::Idle: return "idle";
    case WifiCoex::Connecting: return "connecting";
    case WifiCoex::Hunting: return "hunting";
    case WifiCoex::Scanning: return "scanning";
    case WifiCoex::Associated: return "associated";
  }
  return "unknown";
}
