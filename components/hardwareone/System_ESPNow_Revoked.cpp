#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Revoked.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <string.h>

#include "System_Clock.h"
#include "System_Debug.h"
#include "System_Events.h"       // systemEventPost
#include "System_Filesystem.h"   // filesystemReady
#include "System_MemUtil.h"      // PSRAM_JSON_DOC, ps_alloc
#include "System_Mutex.h"        // FsLockGuard
#include "System_Settings.h"     // quarantineCorruptFile
#include "System_StatePaths.h"
#include "System_VFS.h"

namespace {

constexpr const char* kPath = HW1_ESPNOW_REVOKED_JSON_PATH;
constexpr const char* kTempPath = HW1_ESPNOW_REVOKED_JSON_PATH ".tmp";

// 32 entries of 80 bytes: 2.5 KB of PSRAM, never internal SRAM.
EXT_RAM_BSS_ATTR espnowrevoke::Table gRevoked;
// Read by the RX path on espnow_task, changed by the commands on cmd_exec:
// every access is a short copy or scan under this lock, nothing is called
// inside it.
portMUX_TYPE gRevokedLock = portMUX_INITIALIZER_UNLOCKED;
bool gRevokedLoaded = false;
volatile bool gRevokedLocked = false;

AuthContext revokedAuth(const char* what) { return VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, what); }

// Writes `t` to revoked.json through a temp file. The caller holds no lock.
bool writeTable(const espnowrevoke::Table& t) {
  if (!filesystemReady) return false;
  char* text = static_cast<char*>(ps_alloc(espnowrevoke::kFileCap, AllocPref::PreferPsram, "espnow.revoked.save"));
  if (!text) return false;
  size_t len = 0;
  {
    PSRAM_JSON_DOC(doc);
    len = espnowrevoke::serialize(t, text, espnowrevoke::kFileCap, doc);
  }
  bool ok = false;
  if (len > 0) {
    FsLockGuard fsGuard("espnow.revoked.save");
    const AuthContext auth = revokedAuth("espnow.revoked_save");
    if (!VFS::existsGuarded("/system/espnow", auth)) {
      if (!VFS::existsGuarded("/system", auth)) VFS::mkdirGuarded("/system", auth);
      VFS::mkdirGuarded("/system/espnow", auth);
    }
    File f = VFS::openGuarded(kTempPath, "w", auth, true);
    if (f) {
      const size_t written = f.write(reinterpret_cast<const uint8_t*>(text), len);
      f.flush();
      f.close();
      ok = written == len && VFS::renameGuarded(kTempPath, kPath, auth);
      if (!ok) VFS::removeGuarded(kTempPath, auth);
    }
  }
  free(text);
  if (!ok) ERROR_ESPNOWF("[ESP-NOW] %s could not be written; the revoked list is unchanged", kPath);
  return ok;
}

}  // namespace

void espnowRevokedLoad() {
  if (gRevokedLoaded) return;
  if (!filesystemReady) {
    // Nothing can be read, so nothing can be trusted to be absent.
    gRevokedLocked = true;
    WARN_ESPNOWF("[ESP-NOW] %s not read (no filesystem): new keys and pairing are refused", kPath);
    return;
  }
  espnowrevoke::Load result = espnowrevoke::Load::Absent;
  // 2.5 KB: never on the caller's stack.
  auto* loaded = static_cast<espnowrevoke::Table*>(
      ps_alloc(sizeof(espnowrevoke::Table), AllocPref::PreferPsram, "espnow.revoked.read"));
  if (!loaded) {
    gRevokedLocked = true;
    WARN_ESPNOWF("[ESP-NOW] no memory to read %s: new keys and pairing are refused", kPath);
    return;
  }
  memset(loaded, 0, sizeof(*loaded));
  {
    FsLockGuard fsGuard("espnow.revoked.load");
    const AuthContext auth = revokedAuth("espnow.revoked_load");
    if (VFS::existsGuarded(kPath, auth)) {
      result = espnowrevoke::Load::Locked;
      File f = VFS::openGuarded(kPath, "r", auth);
      if (f) {
        const size_t size = f.size();
        char* text = size > 0 && size < espnowrevoke::kFileCap
                         ? static_cast<char*>(ps_alloc(size + 1, AllocPref::PreferPsram, "espnow.revoked.load"))
                         : nullptr;
        if (text) {
          const size_t got = f.read(reinterpret_cast<uint8_t*>(text), size);
          text[got] = '\0';
          if (got == size) {
            PSRAM_JSON_DOC(doc);
            result = espnowrevoke::parse(text, got, *loaded, doc);
          }
          free(text);
        }
        f.close();
      }
    }
  }
  if (result != espnowrevoke::Load::Ok) memset(loaded, 0, sizeof(*loaded));
  const uint8_t loadedCount = loaded->count;
  portENTER_CRITICAL(&gRevokedLock);
  gRevoked = *loaded;
  portEXIT_CRITICAL(&gRevokedLock);
  free(loaded);
  gRevokedLocked = result == espnowrevoke::Load::Locked;
  gRevokedLoaded = true;
  if (gRevokedLocked) {
    WARN_ESPNOWF("[ESP-NOW] %s is there but cannot be read: new keys and pairing are refused until a "
                 "super admin runs 'espnowunrevoke --all' (which lifts the revokes in it)", kPath);
    systemEventPost(SYSEVT_CONFIG_FILE_CORRUPT, kPath, "unreadable - revoked list locked, pairing refused");
  } else if (loadedCount) {
    INFO_ESPNOWF("[ESP-NOW] %u revoked device(s) loaded from %s", (unsigned)loadedCount, kPath);
  }
}

bool espnowRevokedLocked() { return gRevokedLocked; }

bool espnowRevokedMac(const uint8_t mac[6]) { return espnowRevokedMacOrKey(mac, nullptr); }

bool espnowRevokedKey(const uint8_t pub[32]) { return espnowRevokedMacOrKey(nullptr, pub); }

bool espnowRevokedMacOrKey(const uint8_t mac[6], const uint8_t pub[32]) {
  portENTER_CRITICAL(&gRevokedLock);
  const bool hit = gRevoked.count && espnowrevoke::findMacOrKey(gRevoked, mac, pub) >= 0;
  portEXIT_CRITICAL(&gRevokedLock);
  return hit;
}

bool espnowRevokedLookup(const uint8_t mac[6], const uint8_t pub[32], espnowrevoke::Entry& out) {
  portENTER_CRITICAL(&gRevokedLock);
  const int i = espnowrevoke::findMacOrKey(gRevoked, mac, pub);
  if (i >= 0) out = gRevoked.e[i];
  portEXIT_CRITICAL(&gRevokedLock);
  return i >= 0;
}

void espnowRevokedSnapshot(espnowrevoke::Table& out) {
  portENTER_CRITICAL(&gRevokedLock);
  out = gRevoked;
  portEXIT_CRITICAL(&gRevokedLock);
}

RevokedChange espnowRevokedAdd(const uint8_t mac[6], const uint8_t pub[32], uint8_t meshSlot,
                               const char* name) {
  if (gRevokedLocked) return RevokedChange::Locked;
  // Changed on a copy, written, then published: the RX path never sees an
  // entry the file does not hold, and a failed write changes nothing.
  auto* next = static_cast<espnowrevoke::Table*>(
      ps_alloc(sizeof(espnowrevoke::Table), AllocPref::PreferPsram, "espnow.revoked.next"));
  if (!next) return RevokedChange::SaveFailed;
  espnowRevokedSnapshot(*next);
  const int64_t nowMs = Clock::epochMillisOrZero();
  const espnowrevoke::AddResult r =
      espnowrevoke::add(*next, mac, pub, meshSlot, nowMs > 0 ? (uint64_t)nowMs : 0, name);
  RevokedChange out = r == espnowrevoke::AddResult::Full ? RevokedChange::Full
                    : r == espnowrevoke::AddResult::Added ? RevokedChange::Added : RevokedChange::Updated;
  if (out != RevokedChange::Full) {
    if (writeTable(*next)) {
      portENTER_CRITICAL(&gRevokedLock);
      gRevoked = *next;
      portEXIT_CRITICAL(&gRevokedLock);
    } else {
      out = RevokedChange::SaveFailed;
    }
  }
  free(next);
  return out;
}

RevokedChange espnowRevokedRemove(const char* text, espnowrevoke::Entry* removed) {
  if (gRevokedLocked) return RevokedChange::Locked;
  auto* next = static_cast<espnowrevoke::Table*>(
      ps_alloc(sizeof(espnowrevoke::Table), AllocPref::PreferPsram, "espnow.revoked.next"));
  if (!next) return RevokedChange::SaveFailed;
  espnowRevokedSnapshot(*next);
  const int i = espnowrevoke::findByText(*next, text);
  RevokedChange out = RevokedChange::NotFound;
  if (i >= 0) {
    if (removed) *removed = next->e[i];
    espnowrevoke::removeAt(*next, i);
    if (writeTable(*next)) {
      portENTER_CRITICAL(&gRevokedLock);
      gRevoked = *next;
      portEXIT_CRITICAL(&gRevokedLock);
      out = RevokedChange::Removed;
    } else {
      out = RevokedChange::SaveFailed;
    }
  }
  free(next);
  return out;
}

RevokedChange espnowRevokedClear(uint8_t* count, char* movedTo, size_t movedToLen) {
  if (movedTo && movedToLen) movedTo[0] = '\0';
  // Locked without a read (no filesystem or no memory at start-up): read it
  // now. A file that is valid is never moved aside unread.
  if (espnowrevoke::clearAction(gRevokedLocked, gRevokedLoaded) == espnowrevoke::ClearAction::RetryLoad) {
    espnowRevokedLoad();
    if (!gRevokedLoaded) return RevokedChange::SaveFailed;   // still nothing read: leave the file
  }
  portENTER_CRITICAL(&gRevokedLock);
  const uint8_t listed = gRevoked.count;
  portEXIT_CRITICAL(&gRevokedLock);
  if (count) *count = listed;
  if (espnowrevoke::clearAction(gRevokedLocked, gRevokedLoaded) == espnowrevoke::ClearAction::Quarantine) {
    if (!filesystemReady) return RevokedChange::SaveFailed;
    FsLockGuard fsGuard("espnow.revoked.clear");
    const AuthContext auth = revokedAuth("espnow.revoked_clear");
    String moved;
    if (VFS::existsGuarded(kPath, auth) && !quarantineCorruptFile(kPath, auth, moved)) {
      return RevokedChange::SaveFailed;
    }
    if (movedTo && movedToLen) strlcpy(movedTo, moved.c_str(), movedToLen);
    gRevokedLocked = false;
    return RevokedChange::Removed;
  }
  auto* empty = static_cast<espnowrevoke::Table*>(
      ps_alloc(sizeof(espnowrevoke::Table), AllocPref::PreferPsram, "espnow.revoked.next"));
  if (!empty) return RevokedChange::SaveFailed;
  memset(empty, 0, sizeof(*empty));
  const bool written = writeTable(*empty);
  if (written) {
    portENTER_CRITICAL(&gRevokedLock);
    memset(&gRevoked, 0, sizeof(gRevoked));
    portEXIT_CRITICAL(&gRevokedLock);
  }
  free(empty);
  return written ? RevokedChange::Removed : RevokedChange::SaveFailed;
}

void espnowRevokedFormatDate(uint64_t epochMs, char* out, size_t outLen) {
  if (!out || !outLen) return;
  if (epochMs == 0) {
    strlcpy(out, "an unknown date", outLen);
    return;
  }
  Clock::formatDateLocal((time_t)(epochMs / 1000), out, outLen);
}

#endif  // ENABLE_ESPNOW
