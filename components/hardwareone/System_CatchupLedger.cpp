// System_CatchupLedger.cpp - file catch-up, device side: the ledger store
//
// See System_CatchupLedger.h. The rules (ids, rotation, trims, the horizon)
// are the pure ones in System_ESPNow_CatchupCore.h; this file walks the
// folders the whitelist names, reads heads, keeps the trim notes, loads and
// saves /system/catchup/ledger.json and reads byte ranges for the server.
//
// Memory: the ledger (about 6.9 KB), its runtime, the trim-note values and a
// 512-byte head buffer are EXT_RAM_BSS statics; the walk scratch (observed
// files and their heads, about 54 KB) is one PSRAM block taken on the first
// reconcile and kept. The mutex is created in PSRAM. Internal RAM: one
// spinlock and one pointer.
//
// Host suite: test/host/test_catchup_ledger.py compiles the part between
// CATCHUP_LEDGER_HOST_BEGIN and CATCHUP_LEDGER_HOST_END over fake_vfs.h with
// its own platform adapters (the cl* functions below).

#include "System_CatchupLedger.h"

#if ENABLE_ESPNOW_SEND

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <string.h>
#include <time.h>

#include <esp_attr.h>     // EXT_RAM_BSS_ATTR
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   // xSemaphoreCreateMutexWithCaps

#include "System_CaptureCrypto.h"     // captureCryptoEnsureKey
#include "System_Clock.h"
#include "System_Debug.h"
#include "System_MemUtil.h"           // ps_alloc, PSRAM_JSON_DOC
#include "System_Mutex.h"             // FsLockGuard
#include "System_Utils.h"             // gBootCounter
#include "System_VFS.h"

// ---------------------------------------------------------------------------
// Platform adapters (the host harness defines its own set)
// ---------------------------------------------------------------------------

namespace {
// The one internal-RAM item of the store: a spinlock must not live in PSRAM.
portMUX_TYPE sNoteMux = portMUX_INITIALIZER_UNLOCKED;
// Published once by cmd_exec; read by any task. The mutex itself is in PSRAM.
std::atomic<SemaphoreHandle_t> sLedgerMutex{nullptr};

uint32_t clMillis() { return millis(); }
uint32_t clRandom() { return esp_random(); }
uint32_t clBootCounter() { return gBootCounter; }
bool clEnsureKey() { return captureCryptoEnsureKey(); }
void* clAlloc(size_t bytes, const char* tag) { return ps_alloc(bytes, AllocPref::RequirePsram, tag); }
bool clSdMounted() { return VFS::isSdAvailable(); }
uint32_t clSdGeneration() { return VFS::sdMountGeneration(); }
void clYield() { vTaskDelay(1); }
void clNoteEnter() { portENTER_CRITICAL(&sNoteMux); }
void clNoteExit() { portEXIT_CRITICAL(&sNoteMux); }
bool clLedgerMutexCreate() {
  if (sLedgerMutex.load()) return true;
  SemaphoreHandle_t m = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) return false;
  sLedgerMutex.store(m);   // only cmd_exec creates it
  return true;
}
bool clLedgerTake(uint32_t waitMs) {
  SemaphoreHandle_t m = sLedgerMutex.load();
  return m && xSemaphoreTake(m, waitMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(waitMs)) == pdTRUE;
}
void clLedgerGive() {
  SemaphoreHandle_t m = sLedgerMutex.load();
  if (m) xSemaphoreGive(m);
}
// "2026-11-19T18:02:11Z", or false while the clock is not set.
bool clUtcNow(char* out, size_t cap) {
  const time_t t = time(nullptr);
  if (!Clock::isValidEpoch(t)) return false;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  return strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv) > 0;
}
const char* clStateScope() { return VFS::Scopes::CATCHUP_STATE; }
const char* clTranscriptScope(const String& path) { return VFS::transcriptScopeFor(path); }
}  // namespace

#define CL_WARN(...) WARN_SYSTEMF(__VA_ARGS__)
#define CL_DEBUG(...) DEBUG_SYSTEMF(__VA_ARGS__)

// CATCHUP_LEDGER_HOST_BEGIN

namespace {
using espnowcatchup::Classified;
using espnowcatchup::EntryState;
using espnowcatchup::Kind;
using espnowcatchup::LedgerEntry;
using espnowcatchup::LedgerState;
using espnowcatchup::Observed;
using espnowcatchup::Seal;
using espnowcatchup::TrimNote;

constexpr const char* kLedgerDir = "/system/catchup";
constexpr const char* kLedgerPath = "/system/catchup/ledger.json";
constexpr int kLedgerSchema = 1;
constexpr size_t kLedgerFileMax = 32768;             // a larger ledger.json is not read
constexpr uint32_t kDeliveredSaveMs = 5u * 60u * 1000u;
// Directory entries (names) one reconcile may list: in all, per root folder
// (the sensors root and /stt; retention never prunes the three newest day
// folders or the eight newest transcripts, the picks offered here)
// and per file folder. Only the picks below are opened and their heads read.
constexpr uint16_t kNameBudget = 2048;
constexpr uint16_t kRootNameMax = 768;
constexpr uint16_t kFolderNameMax = 384;
constexpr uint8_t kSensorDayFolders = 3, kSensorBootFolders = 2, kTranscriptOwners = 8;
constexpr uint8_t kNoteSlots = 6;

// The pick categories: the newest files of each, across both tiers, ranked as
// the horizon ranks them. Each cap is the kind's horizon share plus slack, so
// a renamed or rotated file the horizon keeps is always observed.
constexpr uint8_t kPickSystemLog = 0, kPickR1h = 1, kPickSensorHealth = 2, kPickSensor = 3, kPickTranscript = 4,
                  kPickCats = 5;
constexpr uint8_t kPickCap[kPickCats] = { 6, 4, 4, 11, 8 };
constexpr Kind kPickKind[kPickCats] = { Kind::SystemLog, Kind::Health, Kind::SensorCapture, Kind::SensorCapture,
                                        Kind::Transcript };
constexpr uint8_t kPickMax = 11;
constexpr size_t kWalkObsMax = 9 + 6 + 4 + 4 + 11 + 8;   // the fixed paths and every pick
static_assert(kWalkObsMax <= espnowcatchup::kObservedMax, "the core reconciles at most kObservedMax files");

struct NoteSlot { uint32_t drops; uint8_t restarted, pending; };

struct PickList {
  char    path[kPickMax][espnowcatchup::kPathMax];
  uint8_t n;
};

struct WalkScratch {
  Observed obs[kWalkObsMax];
  uint8_t  heads[kWalkObsMax][espnowcatchup::kHeadMax];
  PickList picks[kPickCats];
  size_t   n;
  uint16_t entries;
  uint8_t  headReads;
  bool     cut;
  TrimNote notes[kNoteSlots];
  size_t   nNotes;
};

struct LedgerRuntime {
  bool     loaded, everReconciled, forceReconcile, deliveredDirty, needSave, keyReady;
  uint32_t lastReconcileAtMs, deliveredDirtySinceMs, sdGeneration;
  WalkScratch* walk;
  CatchupLedgerStats stats;
};

EXT_RAM_BSS_ATTR LedgerState sLedger;
EXT_RAM_BSS_ATTR LedgerRuntime sRt;
EXT_RAM_BSS_ATTR NoteSlot sNotes[kNoteSlots];
EXT_RAM_BSS_ATTR uint8_t sHeadBuf[espnowcatchup::kHeadMax];

// The fixed whitelist paths (FILES_IMPL_SPEC 3.4), the capped logs first.
const char* const kFixedPaths[] = {
  "/system/sys_logs/events.log", "/system/sys_logs/errors.log", "/system/sys_logs/crash-history.log",
  "/sd/system/sys_logs/events.log", "/sd/system/sys_logs/errors.log", "/sd/system/sys_logs/crash-history.log",
  "/battery.csv", "/battery.csv.1", "/battery.csv.2",
};

const char* leafOf(const char* name) {
  const char* s = name ? strrchr(name, '/') : nullptr;
  return s ? s + 1 : (name ? name : "");
}

// The read identity for a path the walk or a READ touches: the narrowest
// scope that holds it, by its tree (never SUPER). Battery is fixed root
// files, read as System_Battery does.
bool underTree(const char* path, const char* tree) {
  const size_t n = strlen(tree);
  return strncmp(path, tree, n) == 0 && (path[n] == 0 || path[n] == '/');
}
AuthContext readAuth(const char* path) {
  const String p(path);
  if (underTree(path, "/system/sys_logs") || underTree(path, "/sd/system/sys_logs"))
    return VFS::systemAuth(VFS::sysLogScopeFor(p), "catchup.read");
  if (underTree(path, "/stt") || underTree(path, "/sd/stt"))
    return VFS::systemAuth(clTranscriptScope(p), "catchup.read");
  if (strncmp(path, "/battery.csv", 12) == 0)
    return VFS::systemAuth("catchup.read.battery");
  return VFS::systemAuth(VFS::captureScopeFor(p), "catchup.read");
}

bool notesPending() {
  bool any = false;
  clNoteEnter();
  for (uint8_t i = 0; i < kNoteSlots; ++i) any = any || sNotes[i].pending;
  clNoteExit();
  return any;
}

// Takes and clears the notes (the caller holds FsLockGuard, so no trim can
// land between this and the stat of the capped logs).
size_t takeNotes(TrimNote* out) {
  size_t n = 0;
  clNoteEnter();
  for (uint8_t i = 0; i < kNoteSlots; ++i) {
    if (!sNotes[i].pending) continue;
    out[n].slot = i;
    out[n].drops = sNotes[i].drops;
    out[n].restarted = sNotes[i].restarted != 0;
    ++n;
    sNotes[i].drops = 0;
    sNotes[i].restarted = 0;
    sNotes[i].pending = 0;
  }
  clNoteExit();
  return n;
}

bool noteForSlot(const WalkScratch& w, uint8_t slot) {
  for (size_t i = 0; i < w.nNotes; ++i)
    if (w.notes[i].slot == slot) return true;
  return false;
}

// One file seen by the walk; `f` is open for reading at position 0.
void observe(WalkScratch& w, const char* path, File& f) {
  if (w.n >= kWalkObsMax) { w.cut = true; return; }
  if (espnowcatchup::boundedLen(path, espnowcatchup::kPathMax - 1) >= espnowcatchup::kPathMax) return;
  Observed& o = w.obs[w.n];
  memset(&o, 0, sizeof(o));
  memcpy(o.path, path, strlen(path) + 1);
  const Classified c = espnowcatchup::classifyPath(o.path);   // rel points into o.path
  if (c.kind == Kind::None) return;
  o.size = (uint32_t)f.size();
  const LedgerEntry* known = espnowcatchup::findByPath(sLedger, o.path);
  const bool notePending = c.group != espnowcatchup::kNoSlot && noteForSlot(w, c.group);
  if (espnowcatchup::needHead(known, o.path, o.size, notePending)) {
    uint8_t* head = w.heads[w.n];
    const size_t want = o.size < espnowcatchup::kHeadMax ? o.size : espnowcatchup::kHeadMax;
    const size_t got = want ? f.read(head, want) : 0;
    if (got != want) { w.cut = true; return; }   // unreadable now: leave its entry as it is
    o.head = head;
    o.headLen = (uint16_t)got;
    o.headRead = true;
    o.sealedAtRest = espnowcatchup::hasSealedMagic(head, got);
    o.c = espnowcatchup::refineWithHead(c, head, got, o.sealedAtRest);
    ++w.headReads;
  } else {
    o.c = c;
    o.sealedAtRest = known && (known->flags & espnowcatchup::kLeSealedAtRest);
  }
  ++w.n;
}

// Lists `folder` under one FsLockGuard hold, calling fn(leaf, File&) per
// entry (hidden names skipped), at most `folderMax` entries and while the
// walk's name budget lasts; a listing cut short marks the walk incomplete
// (nothing it did not reach is removed).
template <typename Fn>
void listFolder(WalkScratch& w, const char* folder, const AuthContext& ctx, uint16_t folderMax, Fn fn) {
  if (w.entries >= kNameBudget) { w.cut = true; return; }
  {
    FsLockGuard guard("catchup.walk");
    const String dirPath(folder);
    if (VFS::existsGuarded(dirPath, ctx)) {
      File dir = VFS::openGuarded(dirPath, "r", ctx);
      if (dir && dir.isDirectory()) {
        uint16_t here = 0;
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
          if (w.entries >= kNameBudget || here >= folderMax) { e.close(); w.cut = true; break; }
          ++w.entries;
          ++here;
          const char* leaf = leafOf(e.name());
          if (leaf[0] && leaf[0] != '.') fn(leaf, e);
          e.close();
        }
      }
      if (dir) dir.close();
    }
  }
  clYield();
}

bool joinPath(char* out, size_t cap, const char* folder, const char* leaf) {
  const int n = snprintf(out, cap, "%s/%s", folder, leaf);
  return n > 0 && (size_t)n < cap;
}

// The fixed paths (capped logs, battery) under one hold, with the trim notes
// taken inside it.
void walkFixed(WalkScratch& w, bool sdMounted) {
  {
    FsLockGuard guard("catchup.walk");
    w.nNotes = takeNotes(w.notes);
    for (const char* path : kFixedPaths) {
      if (!sdMounted && strncmp(path, "/sd/", 4) == 0) continue;
      const AuthContext ctx = readAuth(path);
      const String p(path);
      if (!VFS::existsGuarded(p, ctx)) continue;
      File f = VFS::openGuarded(p, "r", ctx);
      if (f && !f.isDirectory()) observe(w, path, f);
      if (f) f.close();
    }
  }
  clYield();
}

// The newest files of one category, across every folder the walk lists for
// it (both tiers): ranked as the horizon ranks them (newerThan on the name
// relative to the kind's folder, then the path), so the files the horizon
// keeps are always among the picks, whatever order the card lists them in
// (robustness review 2026-11-19: a shared 64-entry budget let FAT order and
// old folders crowd out today's health file and the transcripts).
bool pickNewer(Kind k, const char* a, const char* b) {
  const char* ra = espnowcatchup::classifyPath(a).rel;
  const char* rb = espnowcatchup::classifyPath(b).rel;
  if (!ra || !rb) return strcmp(a, b) > 0;
  if (espnowcatchup::newerThan(k, ra, rb)) return true;
  if (espnowcatchup::newerThan(k, rb, ra)) return false;
  return strcmp(a, b) > 0;
}
void offerPick(WalkScratch& w, uint8_t cat, const char* path) {
  PickList& l = w.picks[cat];
  const Kind k = kPickKind[cat];
  uint8_t at = l.n;
  while (at > 0 && pickNewer(k, path, l.path[at - 1])) --at;
  if (at >= kPickCap[cat]) return;
  const uint8_t last = l.n < kPickCap[cat] ? l.n : uint8_t(kPickCap[cat] - 1);
  for (uint8_t i = last; i > at; --i) memcpy(l.path[i], l.path[i - 1], sizeof(l.path[i]));
  memcpy(l.path[at], path, strlen(path) + 1);
  if (l.n < kPickCap[cat]) ++l.n;
}

// Every file of one folder whose name the whitelist accepts, offered to the
// category's picks (names only: nothing is opened for reading here).
void pickFolderFiles(WalkScratch& w, const char* folder, uint8_t cat) {
  const AuthContext ctx = readAuth(folder);
  listFolder(w, folder, ctx, kFolderNameMax, [&](const char* leaf, File& e) {
    if (e.isDirectory()) return;
    char path[espnowcatchup::kPathMax];
    if (!joinPath(path, sizeof(path), folder, leaf)) return;
    if (espnowcatchup::classifyPath(path).kind == Kind::None) return;
    uint8_t c = cat;
    if (cat == kPickSensor && strncmp(leaf, "health", 6) == 0) c = kPickSensorHealth;
    offerPick(w, c, path);
  });
}

// Opens and observes every pick, one category per FsLockGuard hold.
void observePicks(WalkScratch& w) {
  for (uint8_t cat = 0; cat < kPickCats; ++cat) {
    const PickList& l = w.picks[cat];
    if (!l.n) continue;
    {
      FsLockGuard guard("catchup.walk");
      for (uint8_t i = 0; i < l.n; ++i) {
        const char* path = l.path[i];
        const AuthContext ctx = readAuth(path);
        File f = VFS::openGuarded(String(path), "r", ctx);
        if (f && !f.isDirectory()) observe(w, path, f);
        else if (!f) w.cut = true;   // listed a moment ago and not openable now: leave its entry
        if (f) f.close();
      }
    }
    clYield();
  }
}

// boot-<N>: N, or -1 for any other name.
long bootFolderNumber(const char* name) {
  if (strncmp(name, "boot-", 5) != 0 || !name[5]) return -1;
  long v = 0;
  for (const char* p = name + 5; *p; ++p) {
    if (*p < '0' || *p > '9' || v > 99999999L) return -1;
    v = v * 10 + (*p - '0');
  }
  return v;
}

// The sensors folder of one tier: the 3 newest day folders and the 2 newest
// boot folders (TimeAnchors moves a boot folder's files into a day folder
// once the clock is set), chosen by name over the whole root listing (its
// own, larger budget: older day folders stay until retention prunes them),
// then their files.
void walkSensors(WalkScratch& w, const char* tier) {
  char root[48];
  snprintf(root, sizeof(root), "%s/logging_captures/sensors", tier);
  const AuthContext ctx = readAuth(root);
  char days[kSensorDayFolders][40] = {};
  char boots[kSensorBootFolders][40] = {};
  long bootN[kSensorBootFolders] = {};
  for (uint8_t i = 0; i < kSensorBootFolders; ++i) bootN[i] = -1;
  listFolder(w, root, ctx, kRootNameMax, [&](const char* leaf, File& e) {
    if (!e.isDirectory() || strlen(leaf) >= sizeof(days[0])) return;
    const long b = bootFolderNumber(leaf);
    if (b >= 0) {
      uint8_t at = kSensorBootFolders;
      for (uint8_t i = 0; i < kSensorBootFolders && at == kSensorBootFolders; ++i)
        if (b > bootN[i]) at = i;
      if (at == kSensorBootFolders) return;
      for (uint8_t i = kSensorBootFolders - 1; i > at; --i) { bootN[i] = bootN[i - 1]; memcpy(boots[i], boots[i - 1], sizeof(boots[i])); }
      bootN[at] = b;
      memcpy(boots[at], leaf, strlen(leaf) + 1);
      return;
    }
    uint8_t at = kSensorDayFolders;
    for (uint8_t i = 0; i < kSensorDayFolders && at == kSensorDayFolders; ++i)
      if (!days[i][0] || strcmp(leaf, days[i]) > 0) at = i;
    if (at == kSensorDayFolders) return;
    for (uint8_t i = kSensorDayFolders - 1; i > at; --i) memcpy(days[i], days[i - 1], sizeof(days[i]));
    memcpy(days[at], leaf, strlen(leaf) + 1);
  });
  char folder[espnowcatchup::kPathMax];
  for (uint8_t i = 0; i < kSensorBootFolders; ++i)
    if (bootN[i] >= 0 && joinPath(folder, sizeof(folder), root, boots[i])) pickFolderFiles(w, folder, kPickSensor);
  for (uint8_t i = 0; i < kSensorDayFolders; ++i)
    if (days[i][0] && joinPath(folder, sizeof(folder), root, days[i])) pickFolderFiles(w, folder, kPickSensor);
}

// Saved transcripts of one tier: /stt/u<id>/*.txt (QuickNotes live one level
// deeper, in notes/, and are never offered).
void walkTranscripts(WalkScratch& w, const char* tier) {
  char root[16];
  snprintf(root, sizeof(root), "%s/stt", tier);
  const AuthContext ctx = readAuth(root);
  char owners[kTranscriptOwners][16] = {};
  uint8_t nOwners = 0;
  listFolder(w, root, ctx, kRootNameMax, [&](const char* leaf, File& e) {
    if (!e.isDirectory() || strlen(leaf) >= sizeof(owners[0])) return;
    if (!espnowcatchup::detail::transcriptOwner(leaf, strlen(leaf))) return;
    if (nOwners >= kTranscriptOwners) { w.cut = true; return; }
    memcpy(owners[nOwners++], leaf, strlen(leaf) + 1);
  });
  char folder[espnowcatchup::kPathMax];
  for (uint8_t i = 0; i < nOwners; ++i)
    if (joinPath(folder, sizeof(folder), root, owners[i])) pickFolderFiles(w, folder, kPickTranscript);
}

// The whole walk: the fixed paths, then the names of every folder (each
// category keeps its own newest picks, so none crowds out another), then the
// picks are opened and observed.
void walk(WalkScratch& w, bool sdMounted) {
  w.n = 0;
  w.entries = 0;
  w.headReads = 0;
  w.cut = false;
  w.nNotes = 0;
  for (uint8_t c = 0; c < kPickCats; ++c) w.picks[c].n = 0;
  walkFixed(w, sdMounted);
  const char* tiers[2] = { "", "/sd" };
  const int nTiers = sdMounted ? 2 : 1;
  char folder[64];
  for (int t = 0; t < nTiers; ++t) {
    snprintf(folder, sizeof(folder), "%s/logging_captures/system", tiers[t]);
    pickFolderFiles(w, folder, kPickSystemLog);
  }
  for (int t = 0; t < nTiers; ++t) {
    snprintf(folder, sizeof(folder), "%s/logging_captures/r1-health-history", tiers[t]);
    pickFolderFiles(w, folder, kPickR1h);
  }
  for (int t = 0; t < nTiers; ++t) walkSensors(w, tiers[t]);
  for (int t = 0; t < nTiers; ++t) walkTranscripts(w, tiers[t]);
  observePicks(w);
}

// ---- ledger.json -----------------------------------------------------------

void hex32(uint32_t v, char out[9]) {
  static const char kHex[] = "0123456789abcdef";
  for (int i = 7; i >= 0; --i) { out[i] = kHex[v & 15u]; v >>= 4; }
  out[8] = 0;
}
bool parseHex32(const char* s, uint32_t* out) {
  if (!s || strlen(s) != 8) return false;
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = s[i];
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | (uint32_t)d;
  }
  *out = v;
  return true;
}

// The seal a kind must have whatever ledger.json says (a hand-edited file
// cannot send a system log in clear). Sensor captures keep what the head
// refinement decided; reconcile re-reads their heads before any serve.
Seal requiredSeal(Kind k, uint8_t stored) {
  switch (k) {
    case Kind::Health:     return Seal::AtRestOnly;
    case Kind::EventsLog:
    case Kind::SystemLog:
    case Kind::Transcript: return Seal::OnSend;
    case Kind::SensorCapture: return stored == uint8_t(Seal::OnSend) ? Seal::OnSend : Seal::Plain;
    default:               return Seal::Plain;
  }
}

// Builds the document under the ledger mutex (waiting up to `waitMs`), then
// writes it staged. False when busy or on any write failure.
bool saveLedger(uint32_t waitMs = UINT32_MAX) {
  PSRAM_JSON_DOC(doc);
  char hex[9];
  if (!clLedgerTake(waitMs)) return false;
  doc["schema"] = kLedgerSchema;
  hex32(sLedger.ledgerId, hex);
  doc["ledgerId"] = hex;
  doc["gen"] = sLedger.gen;
  doc["nextFileId"] = sLedger.nextFileId;
  char utc[24];
  if (clUtcNow(utc, sizeof(utc))) doc["updatedUtc"] = utc;
  JsonArray files = doc["files"].to<JsonArray>();
  for (uint8_t i = 0; i < sLedger.count && i < espnowcatchup::kLedgerMax; ++i) {
    const LedgerEntry& e = sLedger.e[i];
    JsonObject o = files.add<JsonObject>();
    o["id"] = e.fileId;
    o["kind"] = e.kind;
    o["seal"] = e.seal;
    o["sealedAtRest"] = (e.flags & espnowcatchup::kLeSealedAtRest) != 0;
    if (e.state == uint8_t(EntryState::MissingCardOut)) o["cardOut"] = true;
    o["path"] = e.path;
    o["base"] = e.base;
    o["headLen"] = e.headLen;
    hex32(e.headCrc, hex);
    o["headCrc"] = hex;
    o["createdBoot"] = e.createdBoot;
    o["size"] = e.size;
    o["delivered"] = e.delivered;
  }
  clLedgerGive();
  if (doc.overflowed()) { ++sRt.stats.saveFailures; return false; }
  const AuthContext ctx = VFS::systemAuth(clStateScope(), "catchup.ledger_write");
  bool ok = false;
  {
    FsLockGuard guard("catchup.ledger");
    const String dir(kLedgerDir);
    if (VFS::existsGuarded(dir, ctx) || VFS::mkdirGuarded(dir, ctx)) {
      VFS::StagedWrite save("catchup.ledger");
      if (save.begin(String(kLedgerPath), ctx)) {
        const size_t want = measureJson(doc);
        const size_t wrote = serializeJson(doc, save.file());
        if (wrote == want && !save.file().getWriteError()) ok = save.commit(ctx);
        else save.discard(ctx);
      }
    }
  }
  if (!ok) {
    ++sRt.stats.saveFailures;
    CL_WARN("[CATCHUP] ledger save failed; kept in RAM, retried with the next change");
    return false;
  }
  sRt.needSave = false;
  sRt.deliveredDirty = false;
  return true;
}

void freshLedger() {
  uint32_t id = 0;
  for (int i = 0; i < 4 && id == 0; ++i) id = clRandom();
  espnowcatchup::ledgerInit(sLedger, id);
  sRt.needSave = true;
  ++sRt.stats.loadResets;
}

// Loads ledger.json; an absent, unreadable or invalid file starts a new
// ledger (a new ledgerId: a collector starts this device's files afresh).
// Entries that fail a check are dropped one by one.
void loadLedger() {
  PSRAM_JSON_DOC(doc);
  bool parsed = false;
  {
    const AuthContext ctx = VFS::systemAuth(clStateScope(), "catchup.ledger_read");
    FsLockGuard guard("catchup.ledger");
    const String path(kLedgerPath);
    if (VFS::existsGuarded(path, ctx)) {
      File f = VFS::openGuarded(path, "r", ctx);
      if (f && !f.isDirectory() && f.size() > 0 && f.size() <= kLedgerFileMax)
        parsed = !deserializeJson(doc, f);
      if (f) f.close();
    }
  }
  uint32_t ledgerId = 0;
  if (!parsed || doc["schema"].as<int>() != kLedgerSchema || !parseHex32(doc["ledgerId"].as<const char*>(), &ledgerId) ||
      ledgerId == 0) {
    freshLedger();
    sRt.loaded = true;
    return;
  }
  espnowcatchup::ledgerInit(sLedger, ledgerId);
  sLedger.gen = doc["gen"].as<uint32_t>();
  if (sLedger.gen == 0) sLedger.gen = 1;
  sLedger.nextFileId = doc["nextFileId"].as<uint32_t>();
  if (sLedger.nextFileId == 0) sLedger.nextFileId = 1;
  uint32_t lastId = 0;
  bool dropped = false;
  for (JsonObjectConst o : doc["files"].as<JsonArrayConst>()) {
    if (sLedger.count >= espnowcatchup::kLedgerMax) { dropped = true; break; }
    const uint32_t id = o["id"].as<uint32_t>();
    const char* path = o["path"].as<const char*>();
    const uint8_t kind = o["kind"].as<uint8_t>();
    const uint32_t base = o["base"].as<uint32_t>();
    const uint32_t size = o["size"].as<uint32_t>();
    const uint16_t headLen = o["headLen"].as<uint16_t>();
    uint32_t headCrc = 0;
    const Classified c = espnowcatchup::classifyPath(path);
    const bool kindOk = c.kind != Kind::None && kind >= 1 && kind <= espnowcatchup::kKindCount &&
                        (kind == uint8_t(c.kind) || (c.kind == Kind::SensorCapture && kind == uint8_t(Kind::Health)));
    if (id == 0 || id <= lastId || id >= sLedger.nextFileId || !kindOk ||
        espnowcatchup::boundedLen(path, espnowcatchup::kPathMax - 1) >= espnowcatchup::kPathMax ||
        base >= espnowcatchup::kBaseLimit || size > UINT32_MAX - base || headLen > espnowcatchup::kHeadMax ||
        !parseHex32(o["headCrc"].as<const char*>(), &headCrc)) {
      dropped = true;
      continue;
    }
    LedgerEntry& e = sLedger.e[sLedger.count];
    memset(&e, 0, sizeof(e));
    e.fileId = id;
    e.kind = kind;
    e.seal = uint8_t(requiredSeal(Kind(kind), o["seal"].as<uint8_t>()));
    e.flags = uint8_t((o["sealedAtRest"].as<bool>() ? espnowcatchup::kLeSealedAtRest : 0) |
                      (c.onSd ? espnowcatchup::kLeOnSd : 0));
    e.state = uint8_t(o["cardOut"].as<bool>() ? EntryState::MissingCardOut : EntryState::Present);
    memcpy(e.path, path, strlen(path) + 1);
    e.base = base;
    e.headLen = headLen;
    e.headCrc = headCrc;
    e.createdBoot = o["createdBoot"].as<uint32_t>();
    e.size = size;
    const uint32_t delivered = o["delivered"].as<uint32_t>();
    e.delivered = delivered < base + size ? delivered : base + size;
    lastId = id;
    ++sLedger.count;
  }
  if (dropped) {
    ++sLedger.gen;
    sRt.needSave = true;
    CL_WARN("[CATCHUP] ledger.json had entries that failed a check; they were dropped");
  }
  sRt.loaded = true;
}

bool reconcileLocked(uint32_t nowMs) {
  if (!sRt.walk) {
    sRt.walk = static_cast<WalkScratch*>(clAlloc(sizeof(WalkScratch), "catchup.walk"));
    if (!sRt.walk) {
      CL_WARN("[CATCHUP] no PSRAM for the reconcile walk (%u B)", (unsigned)sizeof(WalkScratch));
      return false;
    }
  }
  WalkScratch& w = *sRt.walk;
  const uint32_t t0 = clMillis();
  const bool sdMounted = clSdMounted();
  const uint32_t sdGen = clSdGeneration();
  walk(w, sdMounted);
  espnowcatchup::ReconcileEnv env = {};
  env.bootCounter = clBootCounter();
  env.nowMs = nowMs;
  env.sdMounted = sdMounted;
  env.walkComplete = !w.cut;
  espnowcatchup::ReconcileOut out = {};
  if (!clLedgerTake(UINT32_MAX)) return false;
  out = espnowcatchup::reconcile(sLedger, w.obs, w.n, w.notes, w.nNotes, env);
  clLedgerGive();
  sRt.everReconciled = true;
  sRt.forceReconcile = false;
  sRt.lastReconcileAtMs = nowMs;
  sRt.sdGeneration = sdGen;
  if (out.changed) sRt.needSave = true;
  CatchupLedgerStats& st = sRt.stats;
  ++st.reconciles;
  st.lastReconcileMs = clMillis() - t0;
  st.lastReconcileAtMs = nowMs ? nowMs : 1;
  st.lastWalkEntries = w.entries;
  st.lastWalkCut = w.cut;
  st.lastHeadReads = w.headReads;
  if (out.changed)
    CL_DEBUG("[CATCHUP] reconcile: %u new, %u renamed, %u trimmed, %u removed; gen %lu (%lu ms)", out.added,
             out.renamed, out.trimmed, out.removed, (unsigned long)sLedger.gen, (unsigned long)st.lastReconcileMs);
  return true;
}

bool ensureLoaded() {
  if (!clLedgerMutexCreate()) return false;
  if (!sRt.loaded) loadLedger();
  sRt.keyReady = clEnsureKey();
  sRt.stats.keyReady = sRt.keyReady;
  sRt.stats.loaded = sRt.loaded;
  return sRt.loaded;
}

void saveIfDue(uint32_t nowMs) {
  if (sRt.needSave || (sRt.deliveredDirty && uint32_t(nowMs - sRt.deliveredDirtySinceMs) >= kDeliveredSaveMs))
    (void)saveLedger();
}
}  // namespace

bool catchupLedgerEnsure(uint32_t nowMs, bool fullListRequested) {
  if (!ensureLoaded()) return false;
  const bool due = !sRt.everReconciled || sRt.forceReconcile || notesPending() ||
                   clSdGeneration() != sRt.sdGeneration ||
                   (fullListRequested && uint32_t(nowMs - sRt.lastReconcileAtMs) >= espnowcatchup::kReconcileMinMs);
  bool ok = true;
  if (due) ok = reconcileLocked(nowMs);
  saveIfDue(nowMs);
  return ok;
}

bool catchupLedgerReconcileNow(uint32_t nowMs) {
  if (!ensureLoaded()) return false;
  const bool ok = reconcileLocked(nowMs);
  saveIfDue(nowMs);
  return ok;
}

const espnowcatchup::LedgerState* catchupLedgerLock() {
  if (!sRt.loaded || !clLedgerTake(2000)) return nullptr;
  if (!sRt.loaded) { clLedgerGive(); return nullptr; }
  return &sLedger;
}

void catchupLedgerUnlock() { clLedgerGive(); }

bool catchupLedgerNoteDelivered(uint32_t fileId, uint32_t claim) {
  if (!sRt.loaded || !clLedgerTake(UINT32_MAX)) return false;
  LedgerEntry* e = espnowcatchup::findById(sLedger, fileId);
  bool raised = false;
  if (e) {
    const uint32_t before = e->delivered;
    espnowcatchup::noteDelivered(*e, claim);
    raised = e->delivered != before;
  }
  clLedgerGive();
  if (raised && !sRt.deliveredDirty) {
    sRt.deliveredDirty = true;
    sRt.deliveredDirtySinceMs = clMillis();
  }
  return e != nullptr;
}

void catchupLedgerNoteSize(uint32_t fileId, uint32_t physicalSize, uint32_t nowMs) {
  if (!sRt.loaded || !clLedgerTake(UINT32_MAX)) return;
  LedgerEntry* e = espnowcatchup::findById(sLedger, fileId);
  if (e && physicalSize > e->size && physicalSize <= UINT32_MAX - e->base) {
    e->size = physicalSize;
    e->lastGrowMs = nowMs;
    e->flags |= espnowcatchup::kLeGrowSeen;
  }
  clLedgerGive();
}

void catchupLedgerSummary(uint32_t* ledgerId, uint32_t* gen, uint32_t* endSum) {
  uint32_t id = 0, g = 0, sum = 0;
  if (sRt.loaded && clLedgerTake(100)) {
    id = sLedger.ledgerId;
    g = sLedger.gen;
    sum = espnowcatchup::endSum(sLedger, sRt.keyReady);
    clLedgerGive();
  }
  if (ledgerId) *ledgerId = id;
  if (gen) *gen = g;
  if (endSum) *endSum = sum;
}

bool catchupLedgerKeyReady() { return sRt.keyReady; }

void catchupNoteTrim(const char* destPath, size_t tailStart) {
  const uint8_t slot = espnowcatchup::trimSlot(destPath);
  if (slot == espnowcatchup::kNoSlot || slot >= kNoteSlots || tailStart == 0) return;
  clNoteEnter();
  NoteSlot& n = sNotes[slot];
  const uint64_t drops = uint64_t(n.drops) + tailStart;
  n.drops = drops > UINT32_MAX ? UINT32_MAX : uint32_t(drops);
  n.pending = 1;
  clNoteExit();
}

void catchupNoteRestart(const char* destPath) {
  const uint8_t slot = espnowcatchup::trimSlot(destPath);
  if (slot == espnowcatchup::kNoSlot || slot >= kNoteSlots) return;
  clNoteEnter();
  sNotes[slot].restarted = 1;
  sNotes[slot].drops = 0;
  sNotes[slot].pending = 1;
  clNoteExit();
}

void catchupLedgerFlush() {
  if (!sRt.loaded || !sRt.deliveredDirty) return;
  (void)saveLedger(200);
}

void catchupLedgerStats(CatchupLedgerStats* out) {
  if (!out) return;
  *out = sRt.stats;
  out->loaded = sRt.loaded;
  out->keyReady = sRt.keyReady;
}

// The I/O the server needs, here so every VFS access of the feature has one
// owner (and one host suite). cmd_exec only.
CatchupReadResult catchupLedgerRead(uint32_t fileId, bool checkHead, uint32_t physOffset, uint8_t* out,
                                    size_t maxBytes, uint32_t* sizeNow, size_t* got) {
  if (sizeNow) *sizeNow = 0;
  if (got) *got = 0;
  const LedgerEntry* e = sRt.loaded ? espnowcatchup::findById(sLedger, fileId) : nullptr;
  if (!e || e->state != uint8_t(EntryState::Present)) return CatchupReadResult::Missing;
  char path[espnowcatchup::kPathMax];
  memcpy(path, e->path, sizeof(path));
  path[sizeof(path) - 1] = 0;
  const uint16_t headLen = e->headLen;
  const uint32_t headCrc = e->headCrc;
  const uint32_t knownSize = e->size;
  const AuthContext ctx = readAuth(path);
  CatchupReadResult result = CatchupReadResult::Ok;
  {
    FsLockGuard guard("catchup.read");
    const String p(path);
    if (!VFS::existsGuarded(p, ctx)) return CatchupReadResult::Missing;
    File f = VFS::openGuarded(p, "r", ctx);
    if (!f || f.isDirectory()) {
      if (f) f.close();
      return CatchupReadResult::Missing;
    }
    const uint32_t size = (uint32_t)f.size();
    if (sizeNow) *sizeNow = size;
    if (size < knownSize) {
      result = CatchupReadResult::Shrunk;
    } else if (checkHead && headLen) {
      const size_t h = f.read(sHeadBuf, headLen);
      if (h != headLen || espnowcatchup::crc32(sHeadBuf, headLen) != headCrc) result = CatchupReadResult::Replaced;
    }
    if (result == CatchupReadResult::Ok && maxBytes && out) {
      if (physOffset > size) {
        result = CatchupReadResult::IoError;
      } else {
        const uint32_t avail = size - physOffset;
        const size_t want = maxBytes < avail ? maxBytes : avail;
        if (want) {
          if (!f.seek(physOffset)) {
            result = CatchupReadResult::IoError;
          } else {
            size_t n = 0;
            while (n < want) {
              const int r = f.read(out + n, want - n);
              if (r <= 0) break;
              n += (size_t)r;
            }
            if (got) *got = n;
            if (n != want) result = CatchupReadResult::IoError;
          }
        }
      }
    }
    f.close();
  }
  return result;
}

// CATCHUP_LEDGER_HOST_END

#endif  // ENABLE_ESPNOW_SEND
