// =============================================================================
// Time anchors — implementation. See System_TimeAnchors.h for the design.
// =============================================================================
// Runs entirely on the main loop task (same context as sensorLogTick's file
// writes). Sweep work is bounded per tick (kMaxRenamesPerPass) so a folder
// full of dark-boot captures promotes over a few seconds instead of stalling
// one loop pass. The tick never waits for the filesystem lock: it tries it
// once for all of its file work and comes back on the next lap when another
// task holds it.
//
// The same tick steps the log retention pass (System_LogRetentionCore.h): the
// policy is there, the directory listing, removal, clock and free-space
// figures are below.
#include "System_TimeAnchors.h"
#include "System_LogRetentionCore.h"

#include "System_BuildConfig.h"   // CAPTURE_DIR_SENSORS
#include "System_Clock.h"         // Clock::isSynced / isValidEpoch
#include "System_Filesystem.h"    // filesystemReady
#include "System_VFS.h"           // VFS::*Guarded + systemAuth
#include "System_Mutex.h"         // FsLockGuard
#include "System_SensorLogging.h" // gSensorLoggingRunning / gSensorLogPath
#include "System_Utils.h"         // everyMs
#include "System_Debug.h"

#include <esp_attr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace {

constexpr const char* kAnchorsPath = CAPTURE_DIR_SENSORS "/.anchors.csv";

// Registry cache — one entry per boot that learned time and still has (or
// recently had) a boot-<N> folder. Pruned lines keep this small; the cap is
// a safety bound, oldest lines fall off first.
struct Anchor {
  uint32_t boot;
  uint32_t msAtSync;
  time_t   epochAtSync;
};
constexpr size_t kMaxAnchors = 32;
EXT_RAM_BSS_ATTR static Anchor gAnchors[kMaxAnchors];
static size_t  sAnchorCount   = 0;
static bool    sRegistryLoaded = false;

// Promoted epoch must land within this window of its anchor, else the file
// is left boot-named (guards millis() wrap: a wrap shifts the result by
// ±49.7 days, which this window rejects).
constexpr time_t kSanityWindowSec = 40LL * 24 * 3600;

constexpr int kMaxRenamesPerPass = 4;

static bool sAnchoredThisBoot = false;
static bool sSweepDone        = false;

// Set when registryLoad() skipped at least one line it could not parse. The
// dominant corruption mode here is a TRUNCATED FINAL LINE from a power loss
// mid-append, which leaves a partial table — and registryPrune() would then
// persist that partial table over the good file, dropping the anchors that were
// never read. A bool is enough because the only consumer refuses outright.
static bool sRegistryPartial = false;

// Set when the file ends in a row without its newline: an append that stopped
// part-way. The next row must not be appended onto that fragment.
static bool sRegistryTailOpen = false;

static void registryLoad() {
  sAnchorCount = 0;
  sRegistryLoaded = true;
  sRegistryPartial = false;
  sRegistryTailOpen = false;
  File f = VFS::openGuarded(kAnchorsPath, "r",
                            VFS::systemAuth(VFS::Scopes::CAPTURES, "timeanchor.load"));
  if (!f) return;
  char line[64];
  size_t ll = 0;
  while (f.available()) {
    int c = f.read();
    if (c < 0) break;
    if (c != '\n') {
      if (ll + 1 < sizeof(line)) line[ll++] = (char)c;
      continue;
    }
    line[ll] = '\0';
    const bool ll_had_content = (ll > 0);
    ll = 0;
    unsigned long b = 0, ms = 0;
    long long ep = 0;
    if (sscanf(line, "%lu,%lu,%lld", &b, &ms, &ep) != 3) {
      // Unparseable line — almost always a truncated tail. Remember it so the
      // partial table is never written back over the file it came from.
      if (ll_had_content) sRegistryPartial = true;
    } else {
      if (sAnchorCount == kMaxAnchors) {
        // Cap hit — drop the OLDEST (front) so recent boots always fit.
        memmove(&gAnchors[0], &gAnchors[1],
                (kMaxAnchors - 1) * sizeof(Anchor));
        sAnchorCount = kMaxAnchors - 1;
      }
      gAnchors[sAnchorCount].boot        = (uint32_t)b;
      gAnchors[sAnchorCount].msAtSync    = (uint32_t)ms;
      gAnchors[sAnchorCount].epochAtSync = (time_t)ep;
      sAnchorCount++;
    }
  }
  sRegistryTailOpen = (ll > 0);
  f.close();
}

static const Anchor* registryFind(uint32_t boot) {
  if (!sRegistryLoaded) registryLoad();
  for (size_t i = 0; i < sAnchorCount; i++) {
    if (gAnchors[i].boot == boot) return &gAnchors[i];
  }
  return nullptr;
}

static void formatAnchorRow(char* out, size_t cap, const Anchor& a) {
  snprintf(out, cap, "%lu,%lu,%lld\n", (unsigned long)a.boot,
           (unsigned long)a.msAtSync, (long long)a.epochAtSync);
}

constexpr size_t kNoSkip = (size_t)-1;

// Save the cached table as the whole registry, leaving out row `skip` and
// adding `extra` when given. writeTextAtomic promotes the new file only after
// every byte was written, so a full or failing filesystem keeps the previous
// registry. Callers change the cache only after this returns true.
static bool registryRewrite(size_t skip, const Anchor* extra) {
  String text;
  char row[64];
  for (size_t i = 0; i < sAnchorCount; i++) {
    if (i == skip) continue;
    formatAnchorRow(row, sizeof(row), gAnchors[i]);
    if (!text.concat(row)) return false;
  }
  if (extra) {
    formatAnchorRow(row, sizeof(row), *extra);
    if (!text.concat(row)) return false;
  }
  if (!writeTextAtomic(kAnchorsPath, text, VFS::systemAuth(VFS::Scopes::CAPTURES, "timeanchor.rewrite"))) return false;
  sRegistryTailOpen = false;  // the rewrite holds whole rows only
  return true;
}

// Drop a fully promoted boot's row from the file, then from the cache.
static void registryPrune(uint32_t boot) {
  for (size_t i = 0; i < sAnchorCount; i++) {
    if (gAnchors[i].boot != boot) continue;
    if (sRegistryPartial) {
      // Refuse: the in-RAM table is missing whatever the unparseable lines held,
      // and persisting it would make that loss permanent.
      WARN_SYSTEMF("[anchor] registry parsed only partially - skipping persist");
    } else if (!registryRewrite(i, nullptr)) {
      // The file still holds this row; keep the cache matching the file.
      WARN_SYSTEMF("[anchor] registry rewrite failed - row for boot %lu kept",
                   (unsigned long)boot);
      return;
    }
    memmove(&gAnchors[i], &gAnchors[i + 1],
            (sAnchorCount - i - 1) * sizeof(Anchor));
    sAnchorCount--;
    return;
  }
}

// Record this boot's anchor row. Returns false when the row did not reach the
// file completely; the tick then tries again later.
static bool writeAnchorLine() {
  if (!sRegistryLoaded) registryLoad();
  const Anchor fresh = {gBootCounter, (uint32_t)millis(), time(nullptr)};
  bool ok = false;
  if (sRegistryTailOpen && !sRegistryPartial) {
    // An earlier append stopped mid-row. Rewrite the registry from the rows
    // that parsed plus this one, which drops the fragment.
    ok = registryRewrite(kNoSkip, &fresh);
  } else {
    // A registry with unparseable rows is never rewritten (sRegistryPartial),
    // so a fragment there is closed off as its own line, not extended.
    char row[64];
    const size_t lead = sRegistryTailOpen ? 1 : 0;
    row[0] = '\n';
    formatAnchorRow(row + lead, sizeof(row) - lead, fresh);
    const size_t len = strlen(row);
    FsLockGuard guard("timeAnchors.append");
    File f = VFS::openGuarded(kAnchorsPath, "a",
                              VFS::systemAuth(VFS::Scopes::CAPTURES, "timeanchor.append"));
    if (f) {
      const size_t before = f.size();
      const size_t written = f.print(row);
      f.flush();
      ok = written == len && !f.getWriteError() && f.size() == before + len;
      f.close();
    }
  }
  if (!ok) {
    sRegistryLoaded = false;  // re-read before the retry: a short append leaves a fragment
    return false;
  }
  sRegistryTailOpen = false;
  // Refresh the cache so the sweep sees the new line without a reload.
  if (sRegistryLoaded && sAnchorCount < kMaxAnchors) {
    gAnchors[sAnchorCount] = fresh;
    sAnchorCount++;
  } else {
    sRegistryLoaded = false;  // reload lazily
  }
  DEBUG_SYSTEMF("[TimeAnchors] boot #%lu anchored: ms=%lu epoch=%lld",
                (unsigned long)fresh.boot, (unsigned long)fresh.msAtSync,
                (long long)fresh.epochAtSync);
  return true;
}

// "boot-<N>" folder names are parsed by LogRetention::parseBootFolder
// (System_LogRetentionCore.h), so the promote sweep and retention agree on
// which folders are boot folders.
using LogRetention::parseBootFolder;

// Parse "<base>-boot<N>-<M><ext>" capture names. On success fills the boot,
// the ms stamp, and copies "<base>" + "<ext>" out.
static bool parseBootFile(const char* name, uint32_t* bootOut, uint32_t* msOut,
                          char* baseOut, size_t baseCap,
                          char* extOut, size_t extCap) {
  const char* tag = strstr(name, "-boot");
  if (!tag) return false;
  char* end = nullptr;
  unsigned long b = strtoul(tag + 5, &end, 10);
  if (!end || *end != '-' || end == tag + 5) return false;
  char* end2 = nullptr;
  unsigned long m = strtoul(end + 1, &end2, 10);
  if (!end2 || end2 == end + 1) return false;
  // end2 now points at the extension (".csv") or NUL.
  if (*end2 != '.' && *end2 != '\0') return false;
  size_t baseLen = (size_t)(tag - name);
  if (baseLen == 0 || baseLen >= baseCap) return false;
  memcpy(baseOut, name, baseLen);
  baseOut[baseLen] = '\0';
  snprintf(extOut, extCap, "%s", end2);
  *bootOut = (uint32_t)b;
  *msOut   = (uint32_t)m;
  return true;
}

// B11: captures go to the card while one takes log lines, so a boot folder can
// be on flash, on the card, or on both. The sweep runs on each tier: `root` is
// "" for flash and "/sd" for the card. The registry itself stays on flash.
static AuthContext sweepAuth(const char* root, const char* reason) {
  return VFS::systemAuth(root[0] ? VFS::Scopes::CAPTURES_SD : VFS::Scopes::CAPTURES, reason);
}

// True when `folderName` (boot-<N>) still exists on the tier `root`.
static bool bootFolderOn(const char* root, const char* folderName) {
  char path[100];
  snprintf(path, sizeof(path), "%s%s/%s", root, CAPTURE_DIR_SENSORS, folderName);
  return VFS::existsGuarded(path, sweepAuth(root, "timeanchor.sweep"));
}

// True when `srcPath` on the tier `root` is the file of the running session:
// its own path, or the card twin of it.
static bool isActiveSessionFile(const char* root, const char* srcPath) {
  if (gSensorLogPath == srcPath) return true;
  const size_t rootLength = strlen(root);
  return rootLength != 0 && strncmp(srcPath, root, rootLength) == 0 && gSensorLogPath == srcPath + rootLength;
}

// Promote up to kMaxRenamesPerPass files out of one boot folder. Returns the
// number of renames performed; sets *folderClear when no promotable files
// remain (everything moved, or the leftovers are unpromotable/skipped).
static int promoteFolderPass(const char* root, const char* folderName, const Anchor& a,
                             bool* folderClear) {
  *folderClear = true;
  int renames = 0;

  char folderPath[100];
  snprintf(folderPath, sizeof(folderPath), "%s%s/%s", root, CAPTURE_DIR_SENSORS,
           folderName);

  const AuthContext ctx = sweepAuth(root, "timeanchor.sweep");
  FsLockGuard guard("timeAnchors.sweep");

  File dir = VFS::openGuarded(folderPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  File f = dir.openNextFile();
  while (f) {
    // Strip any path prefix the FS layer includes.
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;

    char name[80];
    snprintf(name, sizeof(name), "%s", nm);
    const bool isDir = f.isDirectory();
    f.close();

    if (isDir) { f = dir.openNextFile(); continue; }

    uint32_t fb = 0, fms = 0;
    char base[32], ext[12];
    if (!parseBootFile(name, &fb, &fms, base, sizeof(base),
                       ext, sizeof(ext))) {
      // Unrecognized name — leave it. rmdir below fails while anything
      // remains; do not clear-flag false or the 5 s sweep never latches done.
      f = dir.openNextFile();
      continue;
    }

    char srcPath[128];
    snprintf(srcPath, sizeof(srcPath), "%s/%s", folderPath, name);

    // Skip the file an active session still has open — it promotes after
    // the session stops (running→stopped edge re-arms the sweep).
    if (gSensorLoggingRunning && isActiveSessionFile(root, srcPath)) {
      *folderClear = false;
      f = dir.openNextFile();
      continue;
    }

    // Date the file off the anchor. Signed math: files written after the
    // sync (fms > msAtSync) land later than the anchor.
    time_t fileEpoch =
        a.epochAtSync - (time_t)((int64_t)((int64_t)a.msAtSync -
                                           (int64_t)fms) / 1000);
    if (!Clock::isValidEpoch(fileEpoch) ||
        fileEpoch < a.epochAtSync - kSanityWindowSec ||
        fileEpoch > a.epochAtSync + kSanityWindowSec) {
      // millis wrap or garbage stamp — leave boot-named.
      f = dir.openNextFile();
      continue;
    }

    struct tm lt;
    localtime_r(&fileEpoch, &lt);
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", &lt);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%S", &lt);

    char dayDir[100];
    snprintf(dayDir, sizeof(dayDir), "%s%s/%s", root, CAPTURE_DIR_SENSORS, day);
    if (!VFS::existsGuarded(dayDir, ctx)) VFS::mkdirGuarded(dayDir, ctx);

    // Unique by construction: wall stamp + boot + file-ms. Same-second
    // collisions across boots (and LittleFS rename-over-existing) can't
    // clobber a prior promote or a synced session file that landed on the
    // same second. dstPath[160] worst case ~125 B.
    char dstPath[160];
    snprintf(dstPath, sizeof(dstPath), "%s/%s-%s-boot%lu-%09lu%s", dayDir,
             base, stamp, (unsigned long)fb, (unsigned long)fms, ext);

    // Belt-and-suspenders: never rename onto an existing path. Counter
    // suffixes if somehow taken; skip (leave in boot folder) if exhausted.
    bool haveDest = false;
    if (!VFS::existsGuarded(dstPath, ctx)) {
      haveDest = true;
    } else {
      for (int n = 2; n <= 9; n++) {
        snprintf(dstPath, sizeof(dstPath),
                 "%s/%s-%s-boot%lu-%09lu-%d%s", dayDir, base, stamp,
                 (unsigned long)fb, (unsigned long)fms, n, ext);
        if (!VFS::existsGuarded(dstPath, ctx)) {
          haveDest = true;
          break;
        }
      }
    }
    if (!haveDest) {
      DEBUG_SYSTEMF("[TimeAnchors] skip promote %s — no free dest name", srcPath);
      *folderClear = false;
      f = dir.openNextFile();
      continue;
    }

    if (VFS::renameGuarded(srcPath, dstPath, ctx)) {
      DEBUG_SYSTEMF("[TimeAnchors] promoted %s -> %s", srcPath, dstPath);
      renames++;
      if (renames >= kMaxRenamesPerPass) {
        // Budget spent — more may remain; not clear yet.
        *folderClear = false;
        break;
      }
    } else {
      DEBUG_SYSTEMF("[TimeAnchors] rename FAILED %s -> %s", srcPath, dstPath);
      // Leave the source in place; don't treat the folder as clear.
      *folderClear = false;
    }
    f = dir.openNextFile();
  }
  dir.close();

  if (*folderClear) {
    // Nothing promotable left. rmdir succeeds only when truly empty —
    // leftovers (unparseable / wrap-guarded files) keep the folder, which
    // is correct: they're still browsable in order.
    if (VFS::rmdirGuarded(folderPath, ctx)) {
      DEBUG_SYSTEMF("[TimeAnchors] removed empty %s", folderPath);
      // The anchor row goes once neither tier holds the boot's folder: the
      // other tier's files still need it. (A card that is out cannot be
      // asked; its folder, if any, then stays boot-named.)
      const char* other = root[0] ? "" : "/sd";
      if (!(other[0] && !VFS::isSdAvailable()) && bootFolderOn(other, folderName)) {
        DEBUG_SYSTEMF("[TimeAnchors] boot %lu's anchor kept: %s%s/%s remains",
                      (unsigned long)a.boot, other, CAPTURE_DIR_SENSORS, folderName);
      } else {
        registryPrune(a.boot);
      }
    }
  }
  return renames;
}

// One bounded sweep pass over the sensors dir of one tier (`root`: "" flash,
// "/sd" the card). Returns true when a full pass found nothing to do there.
static bool sweepTierPass(const char* root) {
  char sensorsDir[40];
  snprintf(sensorsDir, sizeof(sensorsDir), "%s%s", root, CAPTURE_DIR_SENSORS);

  // Collect ANCHORED boot-* folder names first (small, bounded), then
  // promote — renaming while iterating the parent dir handle is asking for
  // trouble. Anchor-less folders (boots that never learned time) never take
  // a slot, so they can't shadow promotable folders behind them; promoted
  // folders rmdir away, sliding the window until sawAll holds.
  char folders[6][20];
  size_t folderCount = 0;
  bool sawAll = true;
  {
    FsLockGuard guard("timeAnchors.scan");
    File dir = VFS::openGuarded(sensorsDir, "r", sweepAuth(root, "timeanchor.scan"));
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return true;  // nothing to sweep without the capture tree
    }
    File f = dir.openNextFile();
    while (f) {
      const char* nm = f.name();
      const char* slash = strrchr(nm, '/');
      if (slash) nm = slash + 1;
      uint32_t b = 0;
      if (f.isDirectory() && parseBootFolder(nm, &b) &&
          registryFind(b) != nullptr) {
        if (folderCount < 6) {
          snprintf(folders[folderCount], sizeof(folders[0]), "%s", nm);
          folderCount++;
        } else {
          sawAll = false;  // promotable folders beyond this pass's window
          f.close();
          break;
        }
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }

  bool allClear = true;
  int renames = 0;
  for (size_t i = 0; i < folderCount; i++) {
    uint32_t b = 0;
    parseBootFolder(folders[i], &b);
    const Anchor* a = registryFind(b);
    if (!a) continue;  // pruned mid-pass — nothing to do
    bool clear = false;
    renames += promoteFolderPass(root, folders[i], *a, &clear);
    if (!clear) allClear = false;
    if (renames >= kMaxRenamesPerPass) return false;  // budget spent, come back
  }
  // Complete only when we saw every anchored folder, all came up clear,
  // and no work was performed.
  return sawAll && allClear && renames == 0;
}

// One bounded sweep pass: flash, then the card when one is mounted (B11).
// Returns true when a full pass found nothing to do on either (sweep complete
// until re-armed).
static bool sweepPass() {
  // Registry load happens OUTSIDE the scan's FsLockGuard so the guard never
  // nests around another guarded open (safe today — FileManager nests the
  // same way — but no reason to depend on it here).
  if (!sRegistryLoaded) registryLoad();
  bool done = sweepTierPass("");
  if (VFS::isSdAvailable()) done = sweepTierPass("/sd") && done;
  return done;
}

// ===========================================================================
// Log retention: the device side of System_LogRetentionCore.h
// ===========================================================================
// The pass object (its listing scratch included) lives in PSRAM; a zeroed
// object is an idle pass.
EXT_RAM_BSS_ATTR static LogRetention::Pass gRetentionPass;
static uint32_t sRetentionLastPassMs = 0;   // millis() of the last pass decision (0: none yet)
static uint32_t sRetentionWaitMs = LogRetention::kPassIntervalMs;  // from that decision to the next one
static bool sRetentionPruning[2] = {false, false};  // per volume: below the low-water mark until the high one
static uint32_t sRetentionFloorDay = 0;     // the local day the pass's "nothing here" memory is from
static uint64_t sRetentionFreeAtStart = 0;
static uint64_t sRetentionLowWater = 0;
static uint32_t sTickLockMisses = 0;        // tick laps that found the filesystem lock busy

// Captures only: transcripts (stt/) are never removed (B12), so the pass holds
// no transcript scope.
static AuthContext retentionAuth(const char* path) {
  const bool card = strncmp(path, "/sd/", 4) == 0;
  return VFS::systemAuth(card ? VFS::Scopes::CAPTURES_SD : VFS::Scopes::CAPTURES, "retention");
}

struct RetentionVfsPort final : LogRetention::Port {
  // The entries are named from the directory itself (File::getNextFileName):
  // none is opened, so one call costs at most skip + capacity directory reads,
  // and the pass keeps skip below LogRetention::kWalkMaxEntries. (openNextFile
  // would stat and open every entry it passes, each a path lookup through the
  // folder.) An entry that is neither a file nor a folder ends the listing
  // there, which the pass treats like any shorter listing.
  bool list(const char* directory, size_t skip, LogRetention::Entry* out, size_t capacity,
            size_t* count, bool* end) override {
    *count = 0;
    *end = false;
    if (skip >= LogRetention::kWalkMaxEntries) {
      *end = true;
      return true;
    }
    File dir = VFS::openGuarded(directory, "r", retentionAuth(directory));
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
    }
    for (size_t index = 0; index < skip + capacity; ++index) {
      bool isDirectory = false;
      const String path = dir.getNextFileName(&isDirectory);
      if (path.length() == 0) {
        *end = true;
        break;
      }
      if (index < skip) continue;
      const char* nm = path.c_str();
      const char* slash = strrchr(nm, '/');
      if (slash) nm = slash + 1;
      LogRetention::Entry& entry = out[*count];
      // A name that does not fit is reported empty, which the pass never removes.
      if (strlen(nm) < sizeof(entry.name)) snprintf(entry.name, sizeof(entry.name), "%s", nm);
      else entry.name[0] = '\0';
      entry.directory = isDirectory;
      ++*count;
    }
    dir.close();
    return true;
  }
  // The one file a step removes is the only one that is opened: for its size,
  // which is what the pass counts towards the high-water mark.
  bool removeFile(const char* path, uint32_t* freedBytes) override {
    *freedBytes = 0;
    const AuthContext auth = retentionAuth(path);
    File file = VFS::openGuarded(path, "r", auth);
    if (file) {
      const bool isDirectory = file.isDirectory();
      if (!isDirectory) *freedBytes = (uint32_t)file.size();
      file.close();
      if (isDirectory) return false;  // never a folder, whatever the listing said
    }
    return VFS::removeGuarded(path, auth);
  }
  bool removeEmptyDirectory(const char* path) override {
    return VFS::rmdirGuarded(path, retentionAuth(path));
  }
  bool bootIsAnchored(uint32_t boot) override { return registryFind(boot) != nullptr; }
};

static uint32_t dayKeyOf(const struct tm& t) {
  return (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
}

// The free-space figure a pass decision acts on; false when there is none.
// The card is read at the decision (VFS::getStats, the authoritative reading:
// one free-cluster query under the tick's lock). Nothing renews the card's
// cached figure by itself: the log writer that fills the card never samples
// it, only a status page, the glasses' Files page and a transcript write do,
// so a cache-only read would find no figure on a device nobody is looking at.
// The internal flash is never sampled here (a fresh LittleFS reading walks the
// whole volume, about 1.3 s on the P4X-EYE): only the figure that is already
// cached, and only a recent one. A device that is filling its flash is
// writing, and its log writer renews that figure every few seconds; on a
// quiet device it may be old or missing, and nothing is removed.
static bool retentionFreeFigure(bool card, VFS::CapacitySnapshot& capacity) {
  if (card) {
    uint64_t total = 0, used = 0, freeBytes = 0;
    if (!VFS::getStats(VFS::SDCARD, total, used, freeBytes)) return false;
    capacity.totalBytes = total;
    capacity.usedBytes = used;
    capacity.freeBytes = freeBytes;
    capacity.sampledAtMs = millis();
    return true;
  }
  if (!VFS::peekStatsSnapshot(VFS::INTERNAL, capacity)) return false;
  return (uint32_t)(millis() - capacity.sampledAtMs) <= LogRetention::kFreeFigureMaxAgeMs;
}

// Decides whether a pass starts on one of the volumes, and starts it. False
// when no usable free-space figure was there to decide on. The internal flash
// is pruned only while no card can take the capture overflow: with a writable
// card new rows go to the card and the days on flash stay, as before.
static bool retentionConsiderPass(uint32_t today) {
  // The card when it is writable, else the internal flash: never both.
  const bool card = VFS::isSdWritable();
  VFS::CapacitySnapshot capacity{};
  if (!retentionFreeFigure(card, capacity)) return false;
  uint64_t need = 0;
  if (!LogRetention::passWanted(capacity.freeBytes, capacity.totalBytes, VFS::logReserveBytes(),
                                &sRetentionPruning[card ? 1 : 0], &need)) {
    return true;
  }
  sRetentionFreeAtStart = capacity.freeBytes;
  sRetentionLowWater = LogRetention::lowWaterBytes(capacity.totalBytes, VFS::logReserveBytes());
  // What earlier passes learned about folders with nothing to remove holds
  // for the rest of the local day (and the volume: begin() checks that).
  gRetentionPass.begin(card ? "/sd" : "", need, today != sRetentionFloorDay);
  sRetentionFloorDay = today;
  return true;
}

// A pass is running, or the next pass decision is due.
static bool retentionHasWork() {
  if (gRetentionPass.active()) return true;
  const uint32_t nowMs = millis();
  return sRetentionLastPassMs == 0 ? nowMs >= LogRetention::kFirstPassDelayMs
                                   : (uint32_t)(nowMs - sRetentionLastPassMs) >= sRetentionWaitMs;
}

// One bounded step of the pass, under the tick's filesystem lock.
static void retentionStep() {
  const time_t now = time(nullptr);
  struct tm local {};
  localtime_r(&now, &local);
  const uint32_t today = dayKeyOf(local);
  if (!gRetentionPass.active()) {
    // Never in the first minutes after boot, then one pass decision an hour.
    // A decision that had no usable free-space figure does not cost the hour:
    // it is taken again a minute later.
    if (!retentionHasWork()) return;
    const uint32_t nowMs = millis();
    sRetentionLastPassMs = nowMs ? nowMs : 1;
    sRetentionWaitMs = retentionConsiderPass(today) ? LogRetention::kPassIntervalMs
                                                    : LogRetention::kFreeFigureRetryMs;
    if (!gRetentionPass.active()) return;
  }
  char openFile[128] = "";
  if (gSensorLoggingRunning) snprintf(openFile, sizeof(openFile), "%s", gSensorLogPath.c_str());
  const LogRetention::Limits limits = {today, gBootCounter, openFile};
  RetentionVfsPort port;
  if (gRetentionPass.step(port, limits) != LogRetention::Step::Finished) return;
  const bool card = gRetentionPass.root()[0] != '\0';
  if (gRetentionPass.removedFiles() == 0) {
    // Nothing on this volume may go: no line, and no further pass until free
    // space has fallen below the low-water mark anew.
    sRetentionPruning[card ? 1 : 0] = false;
    return;
  }
  // The cached free-space figure is older than these removals.
  VFS::invalidateStatsSnapshot(card ? VFS::SDCARD : VFS::INTERNAL);
  char day[16] = "boot folder";
  if (gRetentionPass.lastDay() != 0) LogRetention::formatDayFolder(gRetentionPass.lastDay(), day, sizeof(day));
  logSystemEvent("STORAGE", "retention: removed %u capture file(s) of %s, %lu KB, on %s "
                 "(free was %lu KB, below the %lu KB low-water mark)",
                 gRetentionPass.removedFiles(), day,
                 (unsigned long)(gRetentionPass.freedBytes() / 1024), card ? "the SD card" : "internal flash",
                 (unsigned long)(sRetentionFreeAtStart / 1024), (unsigned long)(sRetentionLowWater / 1024));
}

}  // namespace

void timeAnchorsTick() {
  if (!filesystemReady) return;

  // Re-arm the sweep when a logging session ends: the boot-named file it
  // held open is now promotable.
  static bool sWasRunning = false;
  const bool running = gSensorLoggingRunning;
  if (sWasRunning && !running) sSweepDone = false;
  sWasRunning = running;
  // And when a card is mounted (B11): its boot folders are in reach now.
  static uint32_t sSweepMountGeneration = 0;
  const uint32_t mountGeneration = VFS::sdMountGeneration();
  if (mountGeneration != sSweepMountGeneration) {
    sSweepMountGeneration = mountGeneration;
    sSweepDone = false;
  }

  if (!Clock::isSynced()) return;

  static uint32_t sLastMs = 0;
  const uint32_t lastBefore = sLastMs;
  if (!everyMs(&sLastMs, 5000)) return;

  // Anchored, swept and no retention step due: no file work, no lock.
  if (sAnchoredThisBoot && sSweepDone && !retentionHasWork()) return;

  // All of this tick's file work (the anchor row, the promote sweep and one
  // retention step) runs under one filesystem lock, and the main loop does not
  // wait for it: a try-lock, and when another task holds the lock the cadence
  // stamp goes back so the tick is due again on the next lap. Nothing is
  // recorded as failed and nothing is taken for "nothing to do": the anchor
  // stays unwritten, the sweep stays armed and the retention pass keeps its
  // place. `held` is also false on this task's own reentry and before the
  // mutex exists; neither is "busy". The guards further down are reentrant
  // under this one.
  FsLockGuard tickGuard("timeAnchors.tick", 0);
  if (!tickGuard.held && !isFsLockedByCurrentTask() && gFsMutex != nullptr) {
    sLastMs = lastBefore;
    ++sTickLockMisses;
    static uint32_t sBusyNoteMs = 0;
    if (everyMs(&sBusyNoteMs, 60000)) {
      DEBUG_SYSTEMF("[TimeAnchors] filesystem busy: tick deferred (%lu deferral(s) this boot)",
                    (unsigned long)sTickLockMisses);
    }
    return;
  }

  if (!sAnchoredThisBoot) {
    // Guard against double-append across a same-boot soft restart path:
    // if a line for this boot already exists, adopt it instead.
    if (registryFind(gBootCounter) != nullptr) {
      sAnchoredThisBoot = true;
    } else if (writeAnchorLine()) {
      sAnchoredThisBoot = true;
      sSweepDone = false;  // a sweep that ran while unanchored skipped this boot
    } else {
      // Not recorded: stay unanchored so the next tick tries again.
      static bool sWarned = false;
      if (!sWarned) {
        sWarned = true;
        WARN_SYSTEMF("[anchor] could not record boot %lu's anchor - retrying",
                     (unsigned long)gBootCounter);
      }
    }
  }

  if (!sSweepDone) sSweepDone = sweepPass();

  // Log retention: at most one bounded step per tick (System_LogRetentionCore.h).
  retentionStep();
}
