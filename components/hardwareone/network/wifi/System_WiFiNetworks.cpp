#include "System_WiFiNetworks.h"

#if ENABLE_WIFI
#include "System_WiFiNetworksInternal.h"
#include "System_Debug.h"
#include "System_Events.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>
#include <utility>

namespace {

struct WifiNetwork {
  String ssid;
  String password;
  int priority = 1;
  bool hidden = false;
  uint32_t lastConnected = 0;
  uint32_t id = 0;
};

WifiNetwork* sNetworks = nullptr;
int sCount = 0;
uint32_t sNextId = 1;
StaticSemaphore_t sNetworkMutexStorage;
SemaphoreHandle_t sNetworkMutex = nullptr;

// The boot initializer runs before readers/writers. After that every storage
// access takes this mutex. Never carry it into a radio wait, file write, crypto
// operation, broadcast, or event callback: those may re-enter other services.
class NetworkGuard {
 public:
  NetworkGuard() : locked_(sNetworkMutex &&
      xSemaphoreTake(sNetworkMutex, portMAX_DELAY) == pdTRUE) {}
  ~NetworkGuard() { if (locked_) xSemaphoreGive(sNetworkMutex); }
  explicit operator bool() const { return locked_ && sNetworks; }
  NetworkGuard(const NetworkGuard&) = delete;
  NetworkGuard& operator=(const NetworkGuard&) = delete;
 private:
  bool locked_;
};

int findBySsidLocked(const String& ssid) {
  for (int i = 0; i < sCount; ++i) if (sNetworks[i].ssid == ssid) return i;
  return -1;
}

int findByIdLocked(uint32_t id) {
  if (id) for (int i = 0; i < sCount; ++i) if (sNetworks[i].id == id) return i;
  return -1;
}

void sortLocked() {
  // Preserve the existing selection-sort tie ordering.
  for (int i = 0; i < sCount; ++i) {
    int minIdx = i;
    for (int j = i + 1; j < sCount; ++j)
      if (sNetworks[j].priority < sNetworks[minIdx].priority) minIdx = j;
    if (minIdx != i) std::swap(sNetworks[i], sNetworks[minIdx]);
  }
}

bool copyRecord(const WifiNetwork& from, WifiNetwork& to, bool includePassword) {
  to.ssid = from.ssid;
  if (includePassword) to.password = from.password;
  to.priority = from.priority;
  to.hidden = from.hidden;
  to.lastConnected = from.lastConnected;
  to.id = from.id;
  return to.ssid == from.ssid && (!includePassword || to.password == from.password);
}

bool copyConnectionLocked(int index, WifiConnectionTarget& out) {
  if (index < 0 || index >= sCount) return false;
  const WifiNetwork& network = sNetworks[index];
  out.ssid = network.ssid;
  out.password = network.password;
  if (out.ssid != network.ssid || out.password != network.password) {
    out = {};
    return false;
  }
  out.id = network.id;
  out.priority = network.priority;
  out.index = index;
  return true;
}

void removeLocked(int index) {
  for (int i = index + 1; i < sCount; ++i)
    sNetworks[i - 1] = std::move(sNetworks[i]);
  sNetworks[--sCount] = {};  // Release the removed/tail credentials too.
}

} // namespace

bool initWiFiNetworks() {
  if (sNetworks) return true;
  if (!sNetworkMutex) sNetworkMutex = xSemaphoreCreateMutexStatic(&sNetworkMutexStorage);
  if (!sNetworkMutex) return false;
  // Retain the original internal-RAM requirement for inline/SSO credentials.
  // Separately allocated Arduino String payloads retain Arduino's allocator.
  sNetworks = static_cast<WifiNetwork*>(ps_alloc(
      MAX_WIFI_NETWORKS * sizeof(WifiNetwork), AllocPref::RequireInternal, "wifi.networks"));
  if (!sNetworks) return false;
  for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) new (&sNetworks[i]) WifiNetwork();
  return true;
}

int wifiNetworkCount() {
  NetworkGuard guard;
  return guard ? sCount : 0;
}

bool wifiNetworkSnapshot(WifiNetworkSnapshot& out) {
  out = {};
  NetworkGuard guard;
  if (!guard) return false;
  for (int i = 0; i < sCount; ++i) {
    auto& copy = out.entries[i];
    const auto& network = sNetworks[i];
    copy.ssid = network.ssid;
    if (copy.ssid != network.ssid) { out = {}; return false; }
    copy.id = network.id;
    copy.priority = network.priority;
    copy.hidden = network.hidden;
    copy.lastConnected = network.lastConnected;
  }
  out.count = sCount;
  return true;
}

void sortWiFiByPriority() {
  NetworkGuard guard;
  if (guard) sortLocked();
}

bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden) {
  // Block blank SSIDs at the source: a network with no name can only fail to
  // connect (and previously crashed the connect path), so never store one. This
  // validates interactive additions and setup; restore keeps its legacy policy.
  String trimmed = ssid;
  trimmed.trim();
  if (trimmed.length() == 0) {
    ERROR_WIFIF("Refusing to save WiFi network with a blank SSID");
    broadcastOutput("ERROR: cannot save a WiFi network with a blank name");
    return false;
  }
  // Reject over-long credentials loudly rather than silently mangling them at
  // connect time. 32 octets is the 802.11 SSID maximum and 64 the WPA2 password
  // maximum (a raw hex PSK); both match the wifi_sta_config_t field widths, so
  // anything longer could never have been stored usefully.
  if (ssid.length() > 32) {
    ERROR_WIFIF("Refusing to save WiFi network: SSID is %u chars (max 32)",
                (unsigned)ssid.length());
    broadcastOutput("ERROR: WiFi name is too long (max 32 characters)");
    return false;
  }
  if (password.length() > 64) {
    ERROR_WIFIF("Refusing to save WiFi network '%s': password is %u chars (max 64)",
                ssid.c_str(), (unsigned)password.length());
    broadcastOutput("ERROR: WiFi password is too long (max 64 characters)");
    return false;
  }
  // A double-quote in either field cannot survive the quoted command line every
  // UI builds (`wifiadd "<ssid>" "<pass>"`). CommandArgs has no escape support,
  // so the quote ENDS the token early and the rest becomes a separate argument:
  // `wifiadd "My"Net" pass123` stores ssid="My", password='Net"' — the WRONG
  // CREDENTIALS, silently. It also shifts the audit redactor's token positions.
  // G2 already refuses this at its own entry (G2_Page_Network.cpp); doing it
  // here covers OLED, web, CLI, the wizard and first-time setup from the one
  // shared interactive validation path.
  if (ssid.indexOf('"') >= 0 || password.indexOf('"') >= 0) {
    ERROR_WIFIF("Refusing to save WiFi network: SSID or password contains a double-quote");
    broadcastOutput("ERROR: WiFi name and password cannot contain a double-quote");
    return false;
  }

  bool keptPassword = false;
  bool full = false;
  {
    NetworkGuard guard;
    if (!guard) return false;
    int index = findBySsidLocked(ssid);
    if (index >= 0) {
      keptPassword = password.length() == 0 && sNetworks[index].password.length() > 0;
      if (!keptPassword) {
        String copy = password;
        if (copy != password) return false;
        sNetworks[index].password = std::move(copy);
      }
      if (priority > 0) sNetworks[index].priority = priority;
      sNetworks[index].hidden = hidden;
    } else if (sCount >= MAX_WIFI_NETWORKS || sNextId == 0) {
      full = true;
    } else {
      WifiNetwork network;
      network.ssid = ssid;
      network.password = password;
      if (network.ssid != ssid || network.password != password) return false;
      network.priority = priority > 0 ? priority : 1;
      network.hidden = hidden;
      network.id = sNextId++;
      sNetworks[sCount++] = std::move(network);
    }
  }
  if (full) { broadcastOutput("[WiFi] Network list full; cannot add"); return false; }
  if (keptPassword) {
    broadcastOutput("[WiFi] Kept existing password for '" + ssid + "' (empty password given; use wifirm to replace the entry)");
    logSystemEvent("WIFI", "wifiadd '%s' with empty password — kept existing stored password", ssid.c_str());
  }
  return true;
}

bool removeWiFiNetwork(const String& ssid) {
  NetworkGuard guard;
  const int index = guard ? findBySsidLocked(ssid) : -1;
  if (index < 0) return false;
  removeLocked(index);
  return true;
}

bool removeWiFiNetworkById(uint32_t id, String* removedSsid) {
  NetworkGuard guard;
  const int index = guard ? findByIdLocked(id) : -1;
  if (index < 0) return false;
  if (removedSsid) {
    *removedSsid = sNetworks[index].ssid;
    if (*removedSsid != sNetworks[index].ssid) return false;
  }
  removeLocked(index);
  return true;
}

bool setWiFiNetworkPriority(const String& ssid, int priority) {
  NetworkGuard guard;
  const int index = guard ? findBySsidLocked(ssid) : -1;
  if (index < 0) return false;
  sNetworks[index].priority = priority;
  return true;
}

bool saveWiFiNetworks() {
  if (!filesystemReady) return false;
  {
    NetworkGuard guard;
    if (!guard) return false;
    sortLocked();
    for (int i = 0; i < sCount; ++i) sNetworks[i].priority = i + 1;
  }
  return requestSettingsPersist();
}

WifiNetworkSaveResult saveSetupWiFiNetwork(const String& ssid, const String& password) {
  if (!upsertWiFiNetwork(ssid, password, 1, false)) return WifiNetworkSaveResult::Rejected;
  const bool previousAutoStart = gSettings.wifiAutoStart;
  // One compound update, one persistence request. Calling setSetting here would
  // perform an independent write before the saved-list result is known.
  gSettings.wifiAutoStart = true;
  if (!saveWiFiNetworks()) {
    gSettings.wifiAutoStart = previousAutoStart;
    return WifiNetworkSaveResult::PersistFailed;
  }
  if (!previousAutoStart) notifySettingChanged(&gSettings.wifiAutoStart);
  return WifiNetworkSaveResult::Accepted;
}

bool wifiNetworkConnectionByIndex(int index, WifiConnectionTarget& out) {
  out = {};
  NetworkGuard guard;
  return guard && copyConnectionLocked(index, out);
}

bool wifiNetworkConnectionById(uint32_t id, WifiConnectionTarget& out) {
  out = {};
  NetworkGuard guard;
  return guard && copyConnectionLocked(findByIdLocked(id), out);
}

bool wifiNetworkConnectionBySSID(const String& ssid, WifiConnectionTarget& out) {
  out = {};
  NetworkGuard guard;
  return guard && copyConnectionLocked(findBySsidLocked(ssid), out);
}

void wifiNetworkMarkConnected(uint32_t id, uint32_t when) {
  NetworkGuard guard;
  const int index = guard ? findByIdLocked(id) : -1;
  if (index >= 0) sNetworks[index].lastConnected = when;
}

bool wifiNetworksWriteJson(JsonDocument& doc, bool excludePasswords) {
  WifiNetwork saved[MAX_WIFI_NETWORKS];
  int count = 0;
  {
    NetworkGuard guard;
    if (!guard) return false;
    for (int i = 0; i < sCount; ++i)
      if (!copyRecord(sNetworks[i], saved[i], !excludePasswords)) return false;
    count = sCount;
  }
  // Match old ciphertext by SSID before replacing the array. Keep Settings'
  // existing preservation policy and pass the prior blob without another copy.
  String oldBlob[MAX_WIFI_NETWORKS];
  JsonArrayConst oldNetworks = doc["network"]["wifi"]["networks"].as<JsonArrayConst>();
  if (!excludePasswords) {
    for (int i = 0; i < count; ++i) {
      int oldCount = 0;
      for (JsonObjectConst oldNet : oldNetworks) {
        const char* blob = oldNet["password"] | "";
        if (strncmp(blob, "AES:", 4) != 0) continue;
        if (oldCount++ >= MAX_WIFI_NETWORKS) break;
        if (saved[i].ssid == (oldNet["ssid"] | "")) {
          oldBlob[i] = blob;
          if (oldBlob[i] != blob) return false;
          break;
        }
      }
    }
  }
  JsonArray networks = doc["network"]["wifi"]["networks"].to<JsonArray>();
  for (int i = 0; i < count; ++i) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = saved[i].ssid;
    if (!excludePasswords) {
      putSecretPreserving(net, "password", saved[i].password, oldBlob[i]);
    }
    net["priority"] = saved[i].priority;
    net["hidden"] = saved[i].hidden;
    net["lastConnected"] = saved[i].lastConnected;
  }
  return !doc.overflowed();
}

unsigned wifiNetworksReadJson(const JsonDocument& doc) {
  // Boot-only restore. Keep its legacy acceptance/defaults separate from upsert.
  JsonArrayConst networks = doc["network"]["wifi"]["networks"].as<JsonArrayConst>();
  if (networks.isNull()) { WARN_STORAGEF("No wifiNetworks array found in JSON"); return 0; }
  WifiNetwork loaded[MAX_WIFI_NETWORKS];
  int count = 0;
  unsigned failures = 0;
  for (JsonObjectConst net : networks) {
    if (count >= MAX_WIFI_NETWORKS) {
      DEBUG_STORAGEF("[WiFi Networks] Max networks reached (%d), skipping rest", MAX_WIFI_NETWORKS);
      break;
    }
    const char* ssid = net["ssid"];
    const char* password = net["password"];
    if (!ssid || !password) continue;
    WifiNetwork& record = loaded[count++];
    record.ssid = ssid;
    record.password = getSecret(net, "password");
    if (strncmp(password, "AES:", 4) == 0 && record.password.length() == 0) {
      ++failures;
      logSystemEvent("CRYPTO", "wifi password for '%s' failed to decrypt at load — stored blob preserved", ssid);
      char subject[48];
      snprintf(subject, sizeof(subject), "wifi:%s", ssid);
      systemEventPost(SYSEVT_SECRET_DECRYPT_FAILED, subject, "stored blob preserved");
    }
    record.priority = net["priority"] | 99;
    record.hidden = net["hidden"] | false;
    record.lastConnected = net["lastConnected"] | 0;
  }
  {
    NetworkGuard guard;
    if (!guard) return failures;
    for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
      if (i < count) loaded[i].id = sNextId++;
      sNetworks[i] = std::move(loaded[i]);
    }
    sCount = count;
  }
  DEBUG_STORAGEF("[WiFi Networks] Loaded %d networks from JSON", count);
  return failures;
}

#endif // ENABLE_WIFI
