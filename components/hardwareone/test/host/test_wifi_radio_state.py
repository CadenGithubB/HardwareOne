#!/usr/bin/env python3
"""The Wi-Fi radio-state read asks the driver only when it has to.

On the P4X-EYE the Wi-Fi driver runs on the ESP32-C6, so every esp_wifi_*
query is an RPC, and one against a deinitialized driver logs on both chips.
After `radiopower off` the OLED Radio row, Network and System screens read
the radio state every frame and flooded the console with failed
Req_GetWifiMode RPCs.

System_WiFi.cpp's wifiRadioState, wifiRadioOn and the per-frame
wifiRadioDisplayState/wifiRadioDisplayOn are compiled unchanged together with
Arduino's own WiFiGenericClass::getMode (components/arduino), against a fake
driver that counts calls. Checked: no driver call while the radio was never
started, after it was stopped, or while associated; ESP-NOW's AP_STA hold and
a restart report correctly; the display read asks the driver at most once a
second while the interfaces are unchanged, and at once after WiFi.mode()
enables or disables one. A source contract keeps the per-frame OLED callers
on the display read and their SSID/RSSI reads behind a rate gate.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ARDUINO = COMPONENT.parent / "arduino"

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

// ---- the driver (on the P4: the C6 behind an RPC) ----
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_WIFI_NOT_INIT 0x3001
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
static int gDriverCalls = 0;
static bool gDriverInit = false;
static wifi_mode_t gDriverMode = WIFI_MODE_NULL;
static esp_err_t esp_wifi_get_mode(wifi_mode_t* mode) {
  ++gDriverCalls;
  if (!gDriverInit) return ESP_ERR_WIFI_NOT_INIT;
  *mode = gDriverMode;
  return ESP_OK;
}

// ---- Arduino's WiFiGeneric state and its real getMode ----
#define log_w(...) ((void)0)
static bool lowLevelInitDone = false;
static bool _esp_wifi_started = false;
class WiFiGenericClass {
public:
  static wifi_mode_t getMode();
};
// INSERT_ARDUINO

struct esp_netif_t { int id; };
static esp_netif_t gStaNetif{1}, gApNetif{2};
struct FakeInterface {
  esp_netif_t* nif = nullptr;
  esp_netif_t* netif() { return nif; }
};
struct FakeWiFi : WiFiGenericClass {
  bool connected = false;
  bool isConnected() { return connected; }
  FakeInterface STA, AP;
};
static FakeWiFi WiFi;

static uint32_t gNow = 5000;
static uint32_t millis() { return gNow; }
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
static bool gEspNow = false;
bool isEspNowInitialized() { return gEspNow; }

// INSERT_ENUM

// INSERT_PRODUCTION

// What WiFi.mode() leaves behind: Arduino's flags, the driver and the
// interface netifs, which it sets or clears as it enables or disables them.
static void arduinoMode(wifi_mode_t mode) {
  const bool on = mode != WIFI_MODE_NULL;
  lowLevelInitDone = _esp_wifi_started = gDriverInit = on;
  gDriverMode = mode;
  WiFi.STA.nif = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) ? &gStaNetif : nullptr;
  WiFi.AP.nif = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? &gApNetif : nullptr;
  if (!on) WiFi.connected = false;
}

static int driverCallsFor(WifiRadioState (*read)(), WifiRadioState expect, int reads) {
  const int before = gDriverCalls;
  for (int i = 0; i < reads; ++i) CHECK(read() == expect);
  return gDriverCalls - before;
}

int main() {
  // Never started (boot with Wi-Fi off): no driver call, exact or display.
  CHECK(driverCallsFor(wifiRadioState, WIFI_RADIO_OFF, 8) == 0);
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_OFF, 8) == 0);
  CHECK(!wifiRadioOn() && !wifiRadioDisplayOn());

  // Station up, not associated: the exact read asks the driver each time.
  arduinoMode(WIFI_MODE_STA);
  CHECK(driverCallsFor(wifiRadioState, WIFI_RADIO_UP_DISCONNECTED, 3) == 3);
  CHECK(wifiRadioOn());

  // Associated: answered from Arduino's event bits alone.
  WiFi.connected = true;
  CHECK(driverCallsFor(wifiRadioState, WIFI_RADIO_CONNECTED, 8) == 0);
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_CONNECTED, 8) == 0);
  WiFi.connected = false;

  // ESP-NOW holds the radio in AP_STA, and after closeespnow leaves AP_STA up.
  arduinoMode(WIFI_MODE_APSTA);
  gEspNow = true;
  CHECK(wifiRadioState() == WIFI_RADIO_UP_FOR_ESPNOW && wifiRadioOn());
  gEspNow = false;
  CHECK(wifiRadioState() == WIFI_RADIO_UP_DISCONNECTED);

  // radiopower off: WiFi.mode(NULL) stopped and deinitialized the driver.
  // This is the flood: every read was an RPC failing with 0x3001.
  arduinoMode(WIFI_MODE_NULL);
  CHECK(driverCallsFor(wifiRadioState, WIFI_RADIO_OFF, 50) == 0);
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_OFF, 50) == 0);
  CHECK(!wifiRadioOn() && !wifiRadioDisplayOn());

  // radiopower on restores the station, and the next frame shows it.
  arduinoMode(WIFI_MODE_STA);
  CHECK(wifiRadioOn() && wifiRadioDisplayOn());

  // Per-frame reads while up and unassociated: one driver call a second.
  gNow += 2000;
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_DISCONNECTED, 8) == 1);
  gNow += 999;
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_DISCONNECTED, 8) == 0);
  gNow += 1;
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_DISCONNECTED, 8) == 1);
  // ESP-NOW starting is seen at once: its flag is read live and its AP
  // interface changes the key.
  gNow += 10;
  arduinoMode(WIFI_MODE_APSTA);
  gEspNow = true;
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_FOR_ESPNOW, 8) == 1);
  gEspNow = false;
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_DISCONNECTED, 1) == 0);

  // A toggle within the second shows on the next frame, both ways.
  gNow += 10;
  arduinoMode(WIFI_MODE_NULL);
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_OFF, 8) == 0);
  gNow += 10;
  arduinoMode(WIFI_MODE_STA);
  CHECK(driverCallsFor(wifiRadioDisplayState, WIFI_RADIO_UP_DISCONNECTED, 8) == 1);

  // The driver went away behind Arduino (a companion that lost its state):
  // the exact read reports what the driver says, the display read within a
  // second of its last answer.
  gDriverInit = false;
  CHECK(driverCallsFor(wifiRadioState, WIFI_RADIO_OFF, 1) == 1);
  CHECK(wifiRadioDisplayState() == WIFI_RADIO_UP_DISCONNECTED);
  gNow += 1000;
  CHECK(wifiRadioDisplayState() == WIFI_RADIO_OFF);

  std::printf("wifi radio state: %d checks passed\n", checks);
  return 0;
}
'''

PER_FRAME = {
    "OLED_SettingsEditor.cpp": ("static bool getQuickRadioState(",),
    "OLED_Mode_Network.cpp": ("void prepareNetworkData(",),
    "OLED_Mode_System.cpp": ("void prepareSystemStatusData(",),
}
GATED = ("void prepareNetworkData(", "void prepareSystemStatusData(")


def code_only(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def production(wifi: str, header: str) -> tuple[str, str]:
    markers = ("static WifiRadioState unassociatedRadioState(", "WifiRadioState wifiRadioState()",
               "bool wifiRadioOn()", "WifiRadioState wifiRadioDisplayState()", "bool wifiRadioDisplayOn()")
    for marker in markers:
        require(wifi.count(marker) == 1, f"System_WiFi.cpp: one {marker}")
    exact = extract_block(wifi, "WifiRadioState wifiRadioState()")
    require("esp_wifi_get_mode" not in code_only(exact),
            "wifiRadioState asks Arduino (WiFi.getMode), not the driver")
    start = wifi.index("static constexpr uint32_t kRadioDisplayMaxAgeMs")
    state = wifi[start:wifi.index("WifiRadioState wifiRadioDisplayState()", start)]
    code = "\n\n".join([extract_block(wifi, markers[0]), exact, extract_block(wifi, markers[2]),
                        state, extract_block(wifi, markers[3]), extract_block(wifi, markers[4])])
    enum = re.search(r"enum WifiRadioState \{.*?\};", header, flags=re.S)
    require(enum is not None, "System_WiFi.h declares enum WifiRadioState")
    return code, enum[0]


def arduino_get_mode(generic: str) -> str:
    marker = "wifi_mode_t WiFiGenericClass::getMode()"
    require(generic.count(marker) == 1, f"WiFiGeneric.cpp: one {marker}")
    body = extract_block(generic, marker)
    require("lowLevelInitDone" in body and "_esp_wifi_started" in body,
            "Arduino's getMode still answers from its own flags before asking the driver")
    return body


def oled_contract(component: Path) -> None:
    for name, markers in PER_FRAME.items():
        source = (component / name).read_text()
        for marker in markers:
            require(source.count(marker) == 1, f"{name}: one {marker}")
            body = code_only(extract_block(source, marker))
            require("wifiRadioDisplayOn()" in body, f"{name} {marker} reads wifiRadioDisplayOn()")
            require("wifiRadioOn()" not in body and "wifiRadioState()" not in body,
                    f"{name} {marker} runs every frame; use the display read")
            if marker in GATED:
                require("everyMs(" in body, f"{name} {marker} rate-gates its SSID/RSSI reads")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    parser.add_argument("--arduino", type=Path, default=ARDUINO,
                        help="Arduino core directory (components/arduino)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    oled_contract(args.component)
    code, enum = production((args.component / "System_WiFi.cpp").read_text(),
                            (args.component / "System_WiFi.h").read_text())
    generic = (args.arduino / "libraries/WiFi/src/WiFiGeneric.cpp").read_text()
    unit_text = (HARNESS.replace("// INSERT_ARDUINO", arduino_get_mode(generic))
                 .replace("// INSERT_ENUM", enum).replace("// INSERT_PRODUCTION", code))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-radio-state-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
