// Runs System_WiFi.cpp's coexistence mirror (the Arduino event handler, the
// install and wifiCoexTag) over fake Arduino events. test_wifi_coex_mirror.py
// inserts the production code; the classifier itself runs in ble_coex_records.
#include "System_WiFiCoex.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif

enum arduino_event_id_t {
  ARDUINO_EVENT_NONE = 0, ARDUINO_EVENT_WIFI_OFF = 100,
  ARDUINO_EVENT_WIFI_STA_START = 110, ARDUINO_EVENT_WIFI_STA_STOP, ARDUINO_EVENT_WIFI_STA_CONNECTED,
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED, ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE, ARDUINO_EVENT_WIFI_STA_GOT_IP,
  ARDUINO_EVENT_WIFI_STA_GOT_IP6, ARDUINO_EVENT_WIFI_STA_LOST_IP,
  ARDUINO_EVENT_WIFI_AP_START = 130, ARDUINO_EVENT_WIFI_AP_STOP,
};
struct wifi_event_sta_disconnected_t { uint8_t ssid[32]; uint8_t ssid_len; uint8_t bssid[6]; uint8_t reason; int8_t rssi; };
union arduino_event_info_t { wifi_event_sta_disconnected_t wifi_sta_disconnected; uint8_t raw[64]; };
using NetworkEventCb = void (*)(arduino_event_id_t, arduino_event_info_t);

static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }
static int gRegistrations = 0;
static bool gRegisterFails = false, gAutoReconnect = true, gTransportUp = true, gEspnow = false;
static NetworkEventCb gHandler = nullptr;
struct FakeWiFi {
  int onEvent(NetworkEventCb cb) {
    if (gRegisterFails) return 0;
    ++gRegistrations; gHandler = cb; return gRegistrations;
  }
  bool getAutoReconnect() const { return gAutoReconnect; }
};
static FakeWiFi WiFi;
static bool radioHalTransportUp() { return gTransportUp; }
static bool espnowOwnsRadio() { return gEspnow; }
static std::atomic<bool> wifiInitialized{false}, sBackgroundConnectActive{false};
static bool gRadioForcedOff = false;

// INSERT_PRODUCTION

static void event(arduino_event_id_t id, uint8_t reason = 0) {
  arduino_event_info_t info{};
  info.wifi_sta_disconnected.reason = reason;
  assert(gHandler);
  gHandler(id, info);
}

int main() {
  // Not installed: unknown. A failed registration is retried later.
  assert(wifiCoexTag().state == WifiCoex::Unknown);
  gRegisterFails = true;
  assert(!wifiCoexMirrorInstall() && wifiCoexTag().state == WifiCoex::Unknown);
  gRegisterFails = false;
  assert(wifiCoexMirrorInstall() && wifiCoexMirrorInstall() && gRegistrations == 1);
  assert(wifiCoexTag().state == WifiCoex::Off);  // installed, radio never started

  // The station starts, joins and gets an address.
  event(ARDUINO_EVENT_WIFI_STA_START);
  WifiCoexTag t = wifiCoexTag();
  assert(t.state == WifiCoex::Idle && (t.flags & WIFI_COEX_STA_UP) && !(t.flags & WIFI_COEX_RECONNECT_ARMED));
  wifiInitialized = true;
  event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
  event(ARDUINO_EVENT_WIFI_STA_GOT_IP);
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Associated && (t.flags & WIFI_COEX_HAS_IP) && (t.flags & WIFI_COEX_RECONNECT_ARMED));
  assert(t.lastDiscAgeS == 255 && !t.disconnects);

  // A Wi-Fi scan and a connect attempt, while associated: counted, and the
  // scan runs the scan scheme.
  {
    CoexScanOnAir scan;
    assert(wifiCoexTag().state == WifiCoex::Scanning && wifiCoexTag().scans == 1);
  }
  assert(wifiCoexTag().state == WifiCoex::Associated);
  {
    CoexConnectScope attempt;
    assert(wifiCoexTag().state == WifiCoex::Associated && wifiCoexTag().connects == 1);
  }

  // The AP drops: hunting while auto-reconnect retries, idle once it is quiet.
  gNow = 50000;
  event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, 201);
  gNow += 3400;
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Hunting && t.lastReason == 201 && t.lastDiscAgeS == 3 && t.disconnects == 1);
  assert(!(t.flags & WIFI_COEX_HAS_IP));
  {
    CoexConnectScope attempt;
    assert(wifiCoexTag().state == WifiCoex::Connecting && wifiCoexTag().connects == 2);
  }
  gNow += kWifiCoexHuntWindowMs;
  assert(wifiCoexTag().state == WifiCoex::Idle);
  event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, 201);
  assert(wifiCoexTag().disconnects == 2);

  // Rejoined, then the address alone is lost.
  event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
  event(ARDUINO_EVENT_WIFI_STA_GOT_IP);
  event(ARDUINO_EVENT_WIFI_STA_LOST_IP);
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Associated && !(t.flags & WIFI_COEX_HAS_IP));

  // A companion restart without STA_STOP: the next STA_START clears the stale
  // association.
  event(ARDUINO_EVENT_WIFI_STA_START);
  assert(wifiCoexTag().state != WifiCoex::Associated);

  // The station stops; ESP-NOW parks the radio on an AP.
  event(ARDUINO_EVENT_WIFI_STA_STOP);
  assert(wifiCoexTag().state == WifiCoex::Off);
  gEspnow = true;
  event(ARDUINO_EVENT_WIFI_AP_START);
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Idle && (t.flags & WIFI_COEX_AP_UP) && (t.flags & WIFI_COEX_ESPNOW));
  event(ARDUINO_EVENT_WIFI_AP_STOP);
  assert(wifiCoexTag().state == WifiCoex::Off && !(wifiCoexTag().flags & WIFI_COEX_AP_UP));
  // A mode change can lose AP_STOP (Arduino unregisters its handler while the
  // event is queued); WIFI_OFF still clears everything.
  event(ARDUINO_EVENT_WIFI_AP_START);
  event(ARDUINO_EVENT_WIFI_STA_START);
  event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
  event(ARDUINO_EVENT_WIFI_OFF);
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Off && !(t.flags & (WIFI_COEX_AP_UP | WIFI_COEX_STA_UP)));
  gEspnow = false;

  // A down companion or `radiopower off` reads off, whatever the mirror holds.
  event(ARDUINO_EVENT_WIFI_STA_START);
  event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
  gTransportUp = false;
  t = wifiCoexTag();
  assert(t.state == WifiCoex::Off && (t.flags & WIFI_COEX_COMPANION_DOWN));
  gTransportUp = true;
  gRadioForcedOff = true;
  assert(wifiCoexTag().state == WifiCoex::Off && (wifiCoexTag().flags & WIFI_COEX_FORCED_OFF));
  gRadioForcedOff = false;

  // The background boot connect reads as connecting.
  event(ARDUINO_EVENT_WIFI_STA_START);
  sBackgroundConnectActive = true;
  assert(wifiCoexTag().state == WifiCoex::Connecting && (wifiCoexTag().flags & WIFI_COEX_BOOT_HUNT));
  sBackgroundConnectActive = false;

  // Events the mirror does not use change nothing.
  const WifiCoexTag before = wifiCoexTag();
  event(ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE);
  event(ARDUINO_EVENT_WIFI_STA_GOT_IP6);
  assert(wifiCoexTag().state == before.state && wifiCoexTag().flags == before.flags);

  std::puts("Wi-Fi coexistence mirror: install once and retry, STA/AP/IP events, hunting window, scan and connect counts,"
            " companion down and forced off");
  return 0;
}
