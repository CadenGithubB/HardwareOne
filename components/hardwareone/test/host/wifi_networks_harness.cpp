#include <Arduino.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <thread>
#include <type_traits>
#if WIFI_NETWORKS_OWNER
#include "wifi_networks_test_platform.h"
#include "System_WiFiNetworks.h"
#include "System_WiFiNetworksInternal.h"
#endif

void webBatchTestStringAppend() {}
String operator+(const char* left, const String& right) { return String(left) + right; }
static std::vector<String> messages;
void broadcastOutput(const String& message) {
#if WIFI_NETWORKS_OWNER
  assert(wifiTestSemaphoreDepth == 0);
#endif
  messages.push_back(message);
}
void logSystemEvent(const char*, const char* format, ...) {
#if WIFI_NETWORKS_OWNER
  assert(wifiTestSemaphoreDepth == 0);
#endif
  char buffer[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  messages.emplace_back(buffer);
}
#define ERROR_WIFIF(...) ((void)0)
bool filesystemReady = true;
static bool persistResult = true;
static int persistCalls = 0;
bool requestSettingsPersist();

struct ObservedNetwork {
  String ssid, password;
  int priority;
  bool hidden;
  uint32_t lastConnected;
};

// INSERT_PRODUCTION_AND_READ_ADAPTER

static std::vector<ObservedNetwork> persisted;
static void expect(int index, const char* ssid, const char* password, int priority, bool hidden = false);
bool requestSettingsPersist() {
#if WIFI_NETWORKS_OWNER
  assert(wifiTestSemaphoreDepth == 0);
#endif
  ++persistCalls;
  persisted.clear();
  for (int i = 0; i < networkCount(); ++i) persisted.push_back(readNetwork(i));
  return persistResult;
}

#if WIFI_NETWORKS_OWNER
FakeWifiSettings gSettings;
static int settingNotifications = 0;
static bool encryptionFails = false;
static int gSecretLoadFailures = 0;
static std::vector<std::string> cryptoEvents;
void notifySettingChanged(const void* field) {
  assert(wifiTestSemaphoreDepth == 0);
  assert(field == &gSettings.wifiAutoStart);
  ++settingNotifications;
}
void systemEventPost(int kind, const char* source, const char*) {
  assert(wifiTestSemaphoreDepth == 0);
  assert(kind == SYSEVT_SECRET_DECRYPT_FAILED || kind == SYSEVT_WIFI_NET_REMOVED);
  cryptoEvents.emplace_back(source);
}
String encryptString(const String& plaintext) {
  assert(wifiTestSemaphoreDepth == 0);
  return encryptionFails ? String("") : "AES:" + plaintext;
}
String decryptString(const String& encoded) {
  assert(wifiTestSemaphoreDepth == 0);
  return encoded.startsWith("AES:") && encoded != "AES:damaged" ? encoded.substring(4) : String("");
}
// INSERT_PRODUCTION_SECRET_HELPERS

static char commandDebugBuffer[1024];
static char* getDebugBuffer() { return commandDebugBuffer; }
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
// INSERT_PRODUCTION_REMOVAL_COMMANDS

template<class T, class = void> struct HasPassword : std::false_type {};
template<class T> struct HasPassword<T, std::void_t<decltype(std::declval<T>().password)>> : std::true_type {};
static_assert(!HasPassword<SavedWifiNetwork>::value, "presentation records cannot expose credentials");

static uint32_t networkId(const char* ssid) {
  WifiNetworkSnapshot snapshot;
  assert(wifiNetworkSnapshot(snapshot));
  for (int i = 0; i < snapshot.count; ++i) if (snapshot.entries[i].ssid == ssid) return snapshot.entries[i].id;
  assert(false && "missing test network");
  return 0;
}
static void ownerCases(const std::string& scenario) {
  if (scenario == "removal_commands") {
    for (const char* invalid : {"", "0", "-1", "+1", "1x", "1 2", "4294967296"}) {
      assert(parseSavedNetworkId(invalid) == 0);
      assert(String(cmd_wifirmid(invalid)).startsWith("Error:"));
    }
    assert(parseSavedNetworkId("4294967295") == UINT32_MAX);
    assert(parseSavedNetworkId("0001") == 1);
    assert(upsertWiFiNetwork("plain name", "p", 1, false));
    assert(upsertWiFiNetwork("quoted name", "p", 2, false));
    assert(upsertWiFiNetwork("--saved-id 7", "p", 3, false));
    assert(String(cmd_wifirm(" plain name ")).startsWith("Removed network"));
    assert(String(cmd_wifirm("\"quoted name\"")).startsWith("Removed network"));
    assert(String(cmd_wifirm("--saved-id 7")).startsWith("Removed network"));
    assert(networkCount() == 0 && persistCalls == 3);
    assert(upsertWiFiNetwork("selected", "p", 1, false));
    const uint32_t removed = networkId("selected");
    assert(removeWiFiNetworkById(removed));
    assert(upsertWiFiNetwork("replacement", "p", 1, false));
    assert(String(cmd_wifirmid(std::to_string(removed))).indexOf("no longer exists") >= 0);
    assert(networkCount() == 1 && persistCalls == 3);
    persistResult = false;
    assert(String(cmd_wifirmid(std::to_string(networkId("replacement")))).indexOf("FAILED") >= 0);
    assert(networkCount() == 0 && persistCalls == 4);
  } else if (scenario == "stable_ids") {
    assert(upsertWiFiNetwork("A", "a", 1, false));
    assert(upsertWiFiNetwork("B", "b", 2, false));
    const uint32_t a = networkId("A"), b = networkId("B");
    WifiNetworkSnapshot before;
    assert(wifiNetworkSnapshot(before));
    assert(setWiFiNetworkPriority("A", 9));
    sortWiFiByPriority();
    assert(networkId("A") == a && networkId("B") == b);
    assert(before.entries[0].ssid == "A" && before.entries[0].priority == 1);
    WifiConnectionTarget target;
    assert(wifiNetworkConnectionById(a, target) && target.ssid == "A" && target.password == "a");
    assert(removeWiFiNetworkById(a));
    assert(upsertWiFiNetwork("A", "new", 1, false));
    assert(networkId("A") != a);
    assert(!wifiNetworkConnectionById(a, target) && !removeWiFiNetworkById(a));
    assert(wifiNetworkCount() == 2 && findNetwork("B") >= 0);
    wifiNetworkMarkConnected(a, 900); // A removed identity cannot stamp its replacement.
    assert(wifiNetworkSnapshot(before));
    for (int i = 0; i < before.count; ++i) assert(before.entries[i].lastConnected == 0);
    wifiNetworkMarkConnected(b, 123);
    assert(wifiNetworkSnapshot(before));
    for (int i = 0; i < before.count; ++i) if (before.entries[i].id == b) assert(before.entries[i].lastConnected == 123);
    assert(!wifiNetworkConnectionByIndex(-1, target) && !wifiNetworkConnectionByIndex(2, target));
  } else if (scenario == "setup_result") {
    assert(saveSetupWiFiNetwork("", "p") == WifiNetworkSaveResult::Rejected);
    assert(!gSettings.wifiAutoStart && persistCalls == 0 && settingNotifications == 0);
    persistResult = false;
    assert(saveSetupWiFiNetwork("home", "p") == WifiNetworkSaveResult::PersistFailed);
    assert(!gSettings.wifiAutoStart && persistCalls == 1 && settingNotifications == 0);
    assert(wifiNetworkCount() == 1); // Existing RAM-before-persistence behavior remains explicit.
    persistResult = true;
    assert(saveSetupWiFiNetwork("home", "") == WifiNetworkSaveResult::Accepted);
    assert(gSettings.wifiAutoStart && persistCalls == 2 && settingNotifications == 1);
    assert(saveSetupWiFiNetwork("home", "p") == WifiNetworkSaveResult::Accepted);
    assert(settingNotifications == 1 && persistCalls == 3);
    persistResult = false;
    assert(saveSetupWiFiNetwork("home", "p") == WifiNetworkSaveResult::PersistFailed);
    assert(gSettings.wifiAutoStart && settingNotifications == 1); // Restore a prior true value too.
    for (int i = 1; i < MAX_WIFI_NETWORKS; ++i)
      assert(upsertWiFiNetwork(std::string("full-") + std::to_string(i), "p", i, false));
    assert(saveSetupWiFiNetwork("extra", "p") == WifiNetworkSaveResult::Rejected);
    assert(persistCalls == 4 && settingNotifications == 1 && gSettings.wifiAutoStart);
  } else if (scenario == "json_roundtrip") {
    JsonDocument doc;
    auto nets = doc["network"]["wifi"]["networks"].to<JsonArray>();
    auto a = nets.add<JsonObject>(); a["ssid"] = "A"; a["password"] = "AES:damaged"; a["priority"] = 9;
    auto b = nets.add<JsonObject>(); b["ssid"] = "B"; b["password"] = "AES:b"; b["priority"] = 1;
    doc["orphan"]["keep"] = 42;
    gSecretLoadFailures += wifiNetworksReadJson(doc);
    assert(gSecretLoadFailures == 1 && cryptoEvents.size() == 1);
    expect(0, "A", "", 9); expect(1, "B", "b", 1);
    sortWiFiByPriority();
    assert(wifiNetworksWriteJson(doc, false));
    auto saved = doc["network"]["wifi"]["networks"].as<JsonArray>();
    assert(saved[0]["ssid"] == "B" && saved[0]["password"] == "AES:b");
    assert(saved[1]["ssid"] == "A" && saved[1]["password"] == "AES:damaged");
    assert(upsertWiFiNetwork("B", "changed", 0, false));
    encryptionFails = true;
    assert(wifiNetworksWriteJson(doc, false));
    assert(doc["network"]["wifi"]["networks"][0]["password"] == "AES:b");
    assert(wifiNetworksWriteJson(doc, true));
    for (JsonObject net : doc["network"]["wifi"]["networks"].as<JsonArray>()) assert(net["password"].isNull());
    assert(doc["orphan"]["keep"] == 42);
    assert(removeWiFiNetwork("A") && removeWiFiNetwork("B"));
    assert(wifiNetworksWriteJson(doc, false));
    assert(doc["network"]["wifi"]["networks"].as<JsonArray>().size() == 0);
  } else if (scenario == "json_load_contract") {
    assert(upsertWiFiNetwork("old", "p", 1, false));
    const uint32_t oldId = networkId("old");
    JsonDocument missing;
    assert(wifiNetworksReadJson(missing) == 0 && wifiNetworkCount() == 1);
    JsonDocument doc;
    auto nets = doc["network"]["wifi"]["networks"].to<JsonArray>();
    nets.add<JsonObject>()["ssid"] = "missing-password";
    nets.add<JsonObject>()["password"] = "AES:p";
    for (int i = 0; i < 10; ++i) {
      auto net = nets.add<JsonObject>();
      net["ssid"] = i == 0 ? "raw\"legacy" : String(std::string("loaded-") + std::to_string(i));
      net["password"] = "AES:p";
    }
    assert(wifiNetworksReadJson(doc) == 0 && wifiNetworkCount() == 8);
    expect(0, "raw\"legacy", "p", 99); // Load does not newly apply upsert validation.
    WifiConnectionTarget target;
    assert(!wifiNetworkConnectionById(oldId, target));
    doc["network"]["wifi"]["networks"].to<JsonArray>();
    assert(wifiNetworksReadJson(doc) == 0 && wifiNetworkCount() == 0);
  } else if (scenario == "copy_failure") {
    assert(upsertWiFiNetwork("home", "secret", 1, false));
    const uint32_t id = networkId("home");
    WifiNetworkSnapshot snapshot;
    wifiTestCopiesBeforeFailure = 0;
    assert(!wifiNetworkSnapshot(snapshot) && snapshot.count == 0);
    WifiConnectionTarget target;
    wifiTestCopiesBeforeFailure = 0;
    assert(!wifiNetworkConnectionById(id, target) && target.id == 0 && target.password.length() == 0);
    JsonDocument doc;
    auto old = doc["network"]["wifi"]["networks"].to<JsonArray>().add<JsonObject>();
    old["ssid"] = "existing"; old["password"] = "AES:recoverable";
    wifiTestCopiesBeforeFailure = 0;
    assert(!wifiNetworksWriteJson(doc, false));
    assert(doc["network"]["wifi"]["networks"][0]["ssid"] == "existing");
    assert(doc["network"]["wifi"]["networks"][0]["password"] == "AES:recoverable");
    assert(persistCalls == 0);
    // Failed ciphertext preservation must leave the original JSON intact.
    // Compare with the original bytes, without another fallible String copy.
    old["ssid"] = "home";
    encryptionFails = true;
    wifiTestFailureValue = "AES:recoverable";
    wifiTestCopiesBeforeFailure = 0;
    wifiTestFailedCopiesRemaining = 2;
    assert(!wifiNetworksWriteJson(doc, false));
    wifiTestFailedCopiesRemaining = 1;
    // One preservation copy suffices; the secret writer must reuse it.
    wifiTestCopiesBeforeFailure = 1;
    assert(wifiNetworksWriteJson(doc, false));
    assert(wifiTestCopiesBeforeFailure == 0);
    assert(doc["network"]["wifi"]["networks"][0]["password"] == "AES:recoverable");
    // Public JSON does not need to allocate or copy any plaintext credential.
    wifiTestFailureValue = "secret";
    wifiTestCopiesBeforeFailure = 0;
    assert(wifiNetworksWriteJson(doc, true));
    assert(wifiTestCopiesBeforeFailure == 0);
    assert(doc["network"]["wifi"]["networks"][0]["password"].isNull());
    wifiTestCopiesBeforeFailure = -1;
    wifiTestFailureValue = nullptr;
  } else if (scenario == "json_overflow") {
    struct RejectAllocator : ArduinoJson::Allocator {
      void* allocate(size_t) override { return nullptr; }
      void deallocate(void*) override {}
      void* reallocate(void*, size_t) override { return nullptr; }
    } allocator;
    assert(upsertWiFiNetwork("home", "secret", 1, false));
    JsonDocument doc(&allocator);
    assert(!wifiNetworksWriteJson(doc, false) && doc.overflowed());
  } else {
    assert(scenario == "coherent_connection_copy");
    assert(upsertWiFiNetwork("same", "1", 1, false));
    const uint32_t id = networkId("same");
    std::thread writer([] {
      for (int i = 1; i <= 1000; ++i) assert(upsertWiFiNetwork("same", std::to_string(i), i, false));
    });
    for (int i = 0; i < 1000; ++i) {
      WifiConnectionTarget target;
      assert(wifiNetworkConnectionById(id, target));
      assert(target.password == String(std::to_string(target.priority)));
    }
    writer.join();
  }
}
#endif

static void expect(int index, const char* ssid, const char* password, int priority, bool hidden) {
  const auto value = readNetwork(index);
  assert(value.ssid == ssid && value.password == password);
  assert(value.priority == priority && value.hidden == hidden);
  assert(value.lastConnected == 0);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
#if WIFI_NETWORKS_OWNER
  if (scenario == "init_failure") {
    failNetworkAllocation = true;
    assert(!initWiFiNetworks() && wifiNetworkCount() == 0);
    assert(!upsertWiFiNetwork("home", "p", 1, false));
    WifiNetworkSnapshot snapshot;
    assert(!wifiNetworkSnapshot(snapshot));
    JsonDocument doc;
    doc["keep"] = 42;
    assert(!wifiNetworksWriteJson(doc, false) && doc["keep"] == 42);
    failNetworkAllocation = false;
    initializeNetworks();
    assert(wifiNetworkCount() == 0);
    return 0;
  }
#endif
  initializeNetworks();
  assert(networkCount() == 0 && findNetwork("missing") == -1);
#if WIFI_NETWORKS_OWNER
  if (scenario == "stable_ids" || scenario == "setup_result" || scenario == "json_roundtrip" ||
      scenario == "json_load_contract" || scenario == "coherent_connection_copy" ||
      scenario == "copy_failure" || scenario == "json_overflow" || scenario == "removal_commands") {
    ownerCases(scenario);
    std::printf("saved WiFi owner: %s passed\n", argv[1]);
    return 0;
  }
#endif
  if (scenario == "validation") {
    const std::vector<std::pair<String, String>> invalid = {
      {"", "pass"}, {" \t\r\n", "pass"}, {std::string(33, 's'), "pass"},
      {"valid", std::string(65, 'p')}, {"bad\"ssid", "pass"}, {"valid", "bad\"pass"},
    };
    for (const auto& value : invalid) {
      assert(!upsertWiFiNetwork(value.first, value.second, 1, false));
      assert(networkCount() == 0 && persistCalls == 0);
    }
    const String ssid(std::string(32, 's')), password(std::string(64, 'p'));
    assert(upsertWiFiNetwork(ssid, password, -1, false));
    expect(0, ssid.c_str(), password.c_str(), 1);
    assert(upsertWiFiNetwork(" spaced ", "", 0, true));
    expect(1, " spaced ", "", 1, true); // Trim validates blankness; it does not rename.
    assert(findNetwork("spaced") == -1 && findNetwork(" spaced ") == 1);
  } else if (scenario == "password_update") {
    assert(upsertWiFiNetwork("home", "original-secret", 7, false));
    assert(upsertWiFiNetwork("home", "", 0, true));
    assert(networkCount() == 1);
    expect(0, "home", "original-secret", 7, true);
    for (const auto& message : messages) assert(message.indexOf("original-secret") < 0);
    assert(!upsertWiFiNetwork("home", std::string(65, 'p'), 9, false));
    expect(0, "home", "original-secret", 7, true);
    assert(upsertWiFiNetwork("home", "replacement", 3, false));
    expect(0, "home", "replacement", 3);
    assert(removeWiFiNetwork("home"));
    assert(upsertWiFiNetwork("home", "", 0, false));
    expect(0, "home", "", 1);
  } else if (scenario == "capacity") {
    for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
      assert(upsertWiFiNetwork(std::string("network-") + std::to_string(i), "p", i + 10, false));
    }
    assert(MAX_WIFI_NETWORKS == 8 && networkCount() == 8);
    assert(!upsertWiFiNetwork("overflow", "p", 1, false));
    assert(upsertWiFiNetwork("network-3", "changed", 0, true));
    expect(3, "network-3", "changed", 13, true);
    assert(removeWiFiNetwork("network-3") && networkCount() == 7);
    expect(3, "network-4", "p", 14);
    assert(upsertWiFiNetwork("replacement", "", 99, false) && networkCount() == 8);
  } else if (scenario == "ordering") {
    assert(upsertWiFiNetwork("A", "a", 2, false));
    assert(upsertWiFiNetwork("B", "b", 2, false));
    assert(upsertWiFiNetwork("C", "c", 1, false));
    sortWiFiByPriority();
    // Preserve the existing selection-sort tie behavior, including indices.
    expect(0, "C", "c", 1); expect(1, "B", "b", 2); expect(2, "A", "a", 2);
    assert(findNetwork("A") == 2 && saveWiFiNetworks());
    expect(0, "C", "c", 1); expect(1, "B", "b", 2); expect(2, "A", "a", 3);
    assert(persistCalls == 1 && persisted.size() == 3 && persisted[2].priority == 3);
  } else if (scenario == "persistence_failure") {
    assert(upsertWiFiNetwork("A", "a", 90, false));
    assert(upsertWiFiNetwork("B", "b", 50, false));
    filesystemReady = false;
    assert(!saveWiFiNetworks() && persistCalls == 0);
    expect(0, "A", "a", 90); expect(1, "B", "b", 50);
    filesystemReady = true;
    persistResult = false;
    assert(!saveWiFiNetworks() && persistCalls == 1);
    expect(0, "B", "b", 1); expect(1, "A", "a", 2);
    assert(persisted.size() == 2 && persisted[0].ssid == "B");
    assert(removeWiFiNetwork("B") && !saveWiFiNetworks());
    assert(networkCount() == 1 && findNetwork("B") == -1);
    expect(0, "A", "a", 1); // Failure reports durability; it does not roll RAM back.
  } else {
    assert(scenario == "remove_final");
    assert(upsertWiFiNetwork("only", "p", 1, false) && saveWiFiNetworks());
    assert(!removeWiFiNetwork("absent") && networkCount() == 1);
    assert(removeWiFiNetwork("only") && networkCount() == 0);
    assert(saveWiFiNetworks() && persistCalls == 2 && persisted.empty());
    assert(!removeWiFiNetwork("only"));
  }
  std::printf("saved WiFi: %s passed\n", argv[1]);
}
