#!/usr/bin/env python3
"""Characterize the production saved-WiFi list helpers with host boundaries."""

import argparse
from pathlib import Path
import shutil
import re
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
WIFI_MODULE = Path("network/wifi")
CASES = ("validation", "password_update", "capacity", "ordering", "persistence_failure", "remove_final")
OWNER_CASES = ("stable_ids", "setup_result", "json_roundtrip", "json_load_contract",
               "coherent_connection_copy", "copy_failure", "json_overflow", "init_failure",
               "removal_commands", "credential_scrub")
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

READ_ADAPTER = '''
static void initializeNetworks() {
  assert(initWiFiNetworks());
  const auto allocations = networkAllocationCount;
  assert(initWiFiNetworks() && networkAllocationCount == allocations);
}
static int networkCount() { return wifiNetworkCount(); }
static int findNetwork(const String& ssid) {
  WifiConnectionTarget target;
  return wifiNetworkConnectionBySSID(ssid, target) ? target.index : -1;
}
static ObservedNetwork readNetwork(int index) {
  WifiNetworkSnapshot snapshot;
  assert(wifiNetworkSnapshot(snapshot) && index >= 0 && index < snapshot.count);
  const auto& n = snapshot.entries[index];
  WifiConnectionTarget target;
  assert(wifiNetworkConnectionById(n.id, target) && target.ssid == n.ssid);
  return {n.ssid, target.password, n.priority, n.hidden, n.lastConnected};
}
'''


def string_boundary():
    # Reuse the established String adapter. Only this test's local copy gains
    # Arduino-style failed-copy injection; the shared test stub stays unchanged.
    header = (HERE / "web_batch_stubs/Arduino.h").read_text()
    header = header.replace("class String {", '''inline thread_local int wifiTestCopiesBeforeFailure = -1;
inline thread_local int wifiTestFailedCopiesRemaining = 1;
inline thread_local const char* wifiTestFailureValue = nullptr;
inline bool wifiTestFailStringCopy(const char* text) {
  if (wifiTestFailureValue && std::string(text) != wifiTestFailureValue) return false;
  if (wifiTestCopiesBeforeFailure < 0) return false;
  if (wifiTestCopiesBeforeFailure > 0) { --wifiTestCopiesBeforeFailure; return false; }
  if (--wifiTestFailedCopiesRemaining == 0) {
    wifiTestCopiesBeforeFailure = -1;
    wifiTestFailedCopiesRemaining = 1;
  }
  return true;
}
class String {''')
    header = header.replace('String(const char* text) : text_(text ? text : "") {}',
                            'String(const char* text) { *this = text; }')
    header = header.replace('    text_ = text ? text : "";',
                            '    text_ = text && wifiTestFailStringCopy(text) ? "" : (text ? text : "");')
    return header.replace("  String() = default;", '''  String() = default;
  String(const String& other) { *this = other; }
  String(String&&) = default;
  String& operator=(String&&) = default;
  String& operator=(const String& other) {
    if (!other.text_.empty() && wifiTestFailStringCopy(other.c_str())) text_.clear();
    else text_ = other.text_;
    return *this;
  }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator!=(const char* other) const { return !(*this == other); }''')


def original_helpers(source_dir):
    source = (source_dir / "System_WiFi.cpp").read_text()
    source = source[source.index("int findWiFiNetwork(const String& ssid) {"):]
    functions = [extract_block(source, signature) for signature in (
        "int findWiFiNetwork(", "void sortWiFiByPriority(", "bool upsertWiFiNetwork(",
        "bool removeWiFiNetwork(", "static void normalizeWiFiPriorities(", "bool saveWiFiNetworks(")]
    header = (source_dir / "System_WiFi.h").read_text()
    types = extract_block(header, "struct WifiNetwork") + ";"
    capacity = re.search(r'^#define MAX_WIFI_NETWORKS \d+', header, re.MULTILINE).group()
    return '\n'.join([capacity, types,
        'static WifiNetwork storage[MAX_WIFI_NETWORKS];',
        'static WifiNetwork* gWifiNetworks = storage;', 'static int gWifiNetworkCount = 0;',
        *functions,
        'static void initializeNetworks() {}',
        'static int networkCount() { return gWifiNetworkCount; }',
        'static int findNetwork(const String& ssid) { return findWiFiNetwork(ssid); }',
        'static ObservedNetwork readNetwork(int index) { assert(index >= 0 && index < gWifiNetworkCount); const auto& n = gWifiNetworks[index]; return {n.ssid, n.password, n.priority, n.hidden, n.lastConnected}; }'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="pre-extraction source directory")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    harness = (HERE / "wifi_networks_harness.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-networks-") as temp:
        work = Path(temp)
        (work / "Arduino.h").write_text(string_boundary())
        source = work / "harness.cpp"
        executable = work / "networks"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(work), "-I", str(HERE / "web_batch_stubs"), "-I", str(JSON_INCLUDE),
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0", "-pthread"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        if args.baseline:
            source.write_text(harness.replace("// INSERT_PRODUCTION_AND_READ_ADAPTER", original_helpers(args.baseline)))
            subprocess.run(command + ["-DWIFI_NETWORKS_OWNER=0", str(source), "-o", str(executable)], check=True, timeout=120)
            for case in CASES:
                subprocess.run([str(executable), case], check=True, timeout=60)
            print("saved WiFi: original helper characterization passed", flush=True)
        # Keep the production module layout so includes from callers and local
        # includes within the module resolve through the same root as firmware.
        (work / WIFI_MODULE).mkdir(parents=True)
        for name in ("System_WiFiNetworks.h", "System_WiFiNetworksInternal.h", "System_WiFiNetworks.cpp"):
            shutil.copyfile(COMPONENT / WIFI_MODULE / name, work / WIFI_MODULE / name)
        (work / "System_BuildConfig.h").write_text("#pragma once\n")
        # No platform substitute headers yet: disabled source must need only
        # its public String/JSON types and leave persisted disabled data intact.
        disabled = work / "disabled.cpp"
        disabled.write_text('''#include "network/wifi/System_WiFiNetworks.h"
#include <cassert>
void webBatchTestStringAppend() {}
int main() {
  assert(!initWiFiNetworks() && wifiNetworkCount() == 0);
  assert(!upsertWiFiNetwork("n", "p", 1, false));
  assert(!removeWiFiNetwork("n") && !removeWiFiNetworkById(1));
  assert(!setWiFiNetworkPriority("n", 1) && !saveWiFiNetworks());
  assert(saveSetupWiFiNetwork("n", "p") == WifiNetworkSaveResult::Rejected);
  sortWiFiByPriority();
  WifiNetworkSnapshot snapshot; snapshot.count = 7;
  assert(!wifiNetworkSnapshot(snapshot) && snapshot.count == 0);
  JsonDocument doc;
  doc["network"]["wifi"]["networks"][0]["password"] = "AES:keep";
  assert(wifiNetworksWriteJson(doc, false) && wifiNetworksReadJson(doc) == 0);
  assert(doc["network"]["wifi"]["networks"][0]["password"] == "AES:keep");
}
''')
        subprocess.run(command + ["-DENABLE_WIFI=0", str(work / WIFI_MODULE / "System_WiFiNetworks.cpp"),
                                  str(disabled), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
        for name in ("System_Settings.h", "System_Debug.h", "System_MemUtil.h", "System_Filesystem.h",
                     "System_SystemEvents.h", "System_Events.h", "System_Utils.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h"):
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#pragma once\n#include "wifi_networks_test_platform.h"\n')
        shutil.copyfile(HERE / "wifi_networks_test_platform.h", work / "wifi_networks_test_platform.h")
        settings = (COMPONENT / "System_Settings.cpp").read_text()
        secrets = '\n'.join(extract_block(settings, signature) for signature in (
            "void putSecretPreserving(", "void putSecret(", "String getSecret("))
        wifi = (COMPONENT / "System_WiFi.cpp").read_text()
        removal = '\n'.join(extract_block(wifi, signature) for signature in (
            "static uint32_t parseSavedNetworkId(", "static const char* finishWiFiRemoval(",
            "const char* cmd_wifirm(", "static const char* cmd_wifirmid("))
        source.write_text(harness.replace("// INSERT_PRODUCTION_AND_READ_ADAPTER", READ_ADAPTER)
                          .replace("// INSERT_PRODUCTION_SECRET_HELPERS", secrets)
                          .replace("// INSERT_PRODUCTION_REMOVAL_COMMANDS", removal))
        subprocess.run(command + ["-DWIFI_NETWORKS_OWNER=1", "-DENABLE_WIFI=1", str(work / WIFI_MODULE / "System_WiFiNetworks.cpp"),
                                  str(source), "-o", str(executable)], check=True, timeout=120)
        for case in CASES + OWNER_CASES:
            subprocess.run([str(executable), case], check=True, timeout=60)
        print("saved WiFi: disabled API and source passed without platform headers")


if __name__ == "__main__":
    main()
