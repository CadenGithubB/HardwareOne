#include "System_BondedPeer.h"

#if ENABLE_BONDED_MODE

#include "System_ESPNow.h"
#include "System_ESPNow_Wire.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_VFS.h"
#include "System_Mutex.h"
#include "System_Debug.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// loadSettingsFromCache lives in System_ESPNow.cpp; it already does the
// existsGuarded + FsLockGuard + open + readString dance. Forward-decl here so
// readCachedSettingsJson() can stay a one-liner instead of duplicating it.
extern String loadSettingsFromCache(const uint8_t* peerMac);

// parseMacAddress lives in System_ESPNow.cpp without a public header. Use the
// same extern pattern WebPage_Bond.cpp uses.
extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);

// =============================================================================
// BondedPeer — see header for rationale + threading notes
// =============================================================================

namespace {
// One thread-local-ish error slot per call. Single-task assumption matches the
// rest of the module: web handlers serialize via httpd, OLED/G2 menus are
// single-task. If we ever call BondedPeer from multiple tasks concurrently
// this becomes a per-task TLS slot, but that's not on the horizon.
const char* sLastError = "";

// Convenience: set sLastError and return the same false the caller wants.
bool fail(const char* msg) {
  sLastError = msg ? msg : "unknown";
  return false;
}

// Reset error on entry to a public call that's about to succeed-or-fail.
void clearError() { sLastError = ""; }

// Resolve peer MAC bytes once per call. Returns false (and sets lastError) on
// any of the preconditions: gEspNow null, bond disabled, MAC unset/unparseable.
bool resolvePeerMac(uint8_t out[6]) {
  if (!gEspNow) return fail("ESP-NOW not initialized");
  if (!gSettings.bondModeEnabled) return fail("Bond mode not enabled");
  if (gSettings.bondPeerMac.length() == 0) return fail("No bonded peer");
  if (!parseMacAddress(gSettings.bondPeerMac, out)) return fail("Invalid peer MAC");
  return true;
}

// Master + paired + online — preconditions for any sync trigger.
bool requireMasterOnlinePeer(uint8_t out[6]) {
  if (!resolvePeerMac(out)) return false;
  if (!isBondMaster()) return fail("Only master can pull from peer");
  if (!gEspNow->bondPeerOnline) return fail("Peer offline");
  return true;
}
} // namespace

namespace BondedPeer {

bool isPaired() {
  return gSettings.bondModeEnabled && gSettings.bondPeerMac.length() > 0;
}

bool isMaster() { return ::isBondMaster(); }

bool isOnline() { return gEspNow && gEspNow->bondPeerOnline; }

String peerMacString() {
  return gSettings.bondPeerMac;
}

bool peerMacBytes(uint8_t out[6]) {
  if (!isPaired()) { sLastError = "Not paired"; return false; }
  if (!parseMacAddress(gSettings.bondPeerMac, out)) {
    sLastError = "Invalid peer MAC";
    return false;
  }
  return true;
}

String peerName() {
  if (!gEspNow || !isPaired()) return gSettings.bondPeerMac;
  uint8_t pm[6];
  if (!parseMacAddress(gSettings.bondPeerMac, pm)) return gSettings.bondPeerMac;
  // getEspNowDeviceName checks runtime meta first, then the paired registry —
  // meta-first matches what other UIs (mesh status, OLED) show for the peer.
  String nm = getEspNowDeviceName(pm);
  return nm.length() > 0 ? nm : gSettings.bondPeerMac;
}

uint32_t peerSettingsHash() {
  return gEspNow ? gEspNow->bondPeerSettingsHash : 0;
}

uint32_t cachedSettingsHash() {
  return gEspNow ? gEspNow->bondCachedPeerSettingsHash : 0;
}

bool isSettingsDirty() {
  if (!gEspNow) return false;
  // No "dirty" claim unless we actually have a cache to compare against.
  if (gEspNow->bondCachedPeerSettingsHash == 0) return false;
  return gEspNow->bondPeerSettingsHash != gEspNow->bondCachedPeerSettingsHash;
}

String readCachedSettingsJson() {
  clearError();
  uint8_t pm[6];
  if (!resolvePeerMac(pm)) return "";
  // loadSettingsFromCache handles "no file yet" by returning "".
  String s = loadSettingsFromCache(pm);
  if (s.length() == 0) sLastError = "No cached settings — call requestSettingsSync first";
  return s;
}

String readCachedSchemaJson() {
  clearError();
  uint8_t pm[6];
  if (!resolvePeerMac(pm)) return "";

  char filePath[80];
  peerCachePath(pm, "schema.json", filePath, sizeof(filePath));

  if (!VFS::existsGuarded(filePath, VFS::systemAuth("bond.schema.read"))) {
    sLastError = "No cached schema — call requestSchemaSync first";
    return "";
  }

  String out;
  {
    FsLockGuard fsGuard("bond.schema.read");
    File f = VFS::openGuarded(filePath, "r", VFS::systemAuth("bond.schema.read"));
    if (!f) { sLastError = "Failed to open cached schema"; return ""; }
    out = f.readString();
    f.close();
  }
  return out;
}

bool requestSettingsSync(uint32_t timeoutMs, uint32_t* outElapsedMs) {
  clearError();
  uint8_t pm[6];
  if (!requireMasterOnlinePeer(pm)) return false;

  // Mark settings missing so the sync tick fires SETTINGS_REQ on its next pass.
  // Clearing the in-flight/cooldown gates makes the request go out immediately
  // rather than waiting out the 3s retry window. (Same dance the old
  // handleBondSettingsSync did inline.)
  gEspNow->bondSettingsReceived = false;
  gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
  gEspNow->bondSyncLastAttemptMs = 0;

  const uint32_t POLL_MS = 100;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    if (gEspNow->bondSettingsReceived) {
      if (outElapsedMs) *outElapsedMs = millis() - startMs;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
  return fail("Timed out waiting for settings sync");
}

bool requestSchemaSync(uint32_t timeoutMs, uint32_t* outElapsedMs) {
  clearError();
  uint8_t pm[6];
  if (!requireMasterOnlinePeer(pm)) return false;

  // Same flag-clear-before-send order as settings: file_end's set-flag must not
  // be masked by a stale "received" from an earlier sync.
  gEspNow->bondSchemaReceived = false;

  if (!requestBondSchema(pm)) return fail("Failed to send SCHEMA_REQ");

  const uint32_t POLL_MS = 100;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    if (gEspNow->bondSchemaReceived) {
      if (outElapsedMs) *outElapsedMs = millis() - startMs;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
  return fail("Timed out waiting for schema sync");
}

const char* lastError() { return sLastError; }

} // namespace BondedPeer

#endif // ENABLE_BONDED_MODE
