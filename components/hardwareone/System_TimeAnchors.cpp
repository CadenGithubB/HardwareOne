// =============================================================================
// Time anchors — implementation. See System_TimeAnchors.h for the design.
// =============================================================================
// Runs entirely on the main loop task (same context as sensorLogTick's file
// writes). Sweep work is bounded per tick (kMaxRenamesPerPass) so a folder
// full of dark-boot captures promotes over a few seconds instead of stalling
// one loop pass.
#include "System_TimeAnchors.h"

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

extern uint32_t gBootCounter;

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
static size_t  gAnchorCount   = 0;
static bool    gRegistryLoaded = false;

// Promoted epoch must land within this window of its anchor, else the file
// is left boot-named (guards millis() wrap: a wrap shifts the result by
// ±49.7 days, which this window rejects).
constexpr time_t kSanityWindowSec = 40LL * 24 * 3600;

constexpr int kMaxRenamesPerPass = 4;

static bool gAnchoredThisBoot = false;
static bool gSweepDone        = false;

static void registryLoad() {
  gAnchorCount = 0;
  gRegistryLoaded = true;
  File f = VFS::openGuarded(kAnchorsPath, "r",
                            VFS::systemAuth("timeanchor.load"));
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
    ll = 0;
    unsigned long b = 0, ms = 0;
    long long ep = 0;
    if (sscanf(line, "%lu,%lu,%lld", &b, &ms, &ep) == 3) {
      if (gAnchorCount == kMaxAnchors) {
        // Cap hit — drop the OLDEST (front) so recent boots always fit.
        memmove(&gAnchors[0], &gAnchors[1],
                (kMaxAnchors - 1) * sizeof(Anchor));
        gAnchorCount = kMaxAnchors - 1;
      }
      gAnchors[gAnchorCount].boot        = (uint32_t)b;
      gAnchors[gAnchorCount].msAtSync    = (uint32_t)ms;
      gAnchors[gAnchorCount].epochAtSync = (time_t)ep;
      gAnchorCount++;
    }
  }
  f.close();
}

static const Anchor* registryFind(uint32_t boot) {
  if (!gRegistryLoaded) registryLoad();
  for (size_t i = 0; i < gAnchorCount; i++) {
    if (gAnchors[i].boot == boot) return &gAnchors[i];
  }
  return nullptr;
}

// Rewrite the registry from the in-RAM cache (used after pruning a fully
// promoted boot). Write-temp-then-rename so a power cut mid-write can't
// truncate the live registry.
static void registryPersist() {
  FsLockGuard guard("timeAnchors.persist");
  const char* tmpPath = CAPTURE_DIR_SENSORS "/.anchors.tmp";
  File f = VFS::openGuarded(tmpPath, "w", VFS::systemAuth("timeanchor.persist"));
  if (!f) return;
  for (size_t i = 0; i < gAnchorCount; i++) {
    char line[64];
    snprintf(line, sizeof(line), "%lu,%lu,%lld\n",
             (unsigned long)gAnchors[i].boot,
             (unsigned long)gAnchors[i].msAtSync,
             (long long)gAnchors[i].epochAtSync);
    f.print(line);
  }
  f.close();
  VFS::renameGuarded(tmpPath, kAnchorsPath,
                     VFS::systemAuth("timeanchor.persist"));
}

static void registryPrune(uint32_t boot) {
  bool changed = false;
  for (size_t i = 0; i < gAnchorCount; i++) {
    if (gAnchors[i].boot == boot) {
      memmove(&gAnchors[i], &gAnchors[i + 1],
              (gAnchorCount - i - 1) * sizeof(Anchor));
      gAnchorCount--;
      changed = true;
      break;
    }
  }
  if (changed) registryPersist();
}

// Append this boot's anchor line (once per boot; caller guards).
static void writeAnchorLine() {
  const uint32_t ms  = millis();
  const time_t   now = time(nullptr);
  FsLockGuard guard("timeAnchors.append");
  File f = VFS::openGuarded(kAnchorsPath, "a",
                            VFS::systemAuth("timeanchor.append"));
  if (!f) return;
  char line[64];
  snprintf(line, sizeof(line), "%lu,%lu,%lld\n",
           (unsigned long)gBootCounter, (unsigned long)ms, (long long)now);
  f.print(line);
  f.close();
  // Refresh the cache so the sweep sees the new line without a reload.
  if (gRegistryLoaded && gAnchorCount < kMaxAnchors) {
    gAnchors[gAnchorCount].boot        = gBootCounter;
    gAnchors[gAnchorCount].msAtSync    = ms;
    gAnchors[gAnchorCount].epochAtSync = now;
    gAnchorCount++;
  } else {
    gRegistryLoaded = false;  // reload lazily
  }
  DEBUG_SYSTEMF("[TimeAnchors] boot #%lu anchored: ms=%lu epoch=%lld",
                (unsigned long)gBootCounter, (unsigned long)ms,
                (long long)now);
}

// Parse "boot-<N>" folder names. Returns false for anything else.
static bool parseBootFolder(const char* name, uint32_t* bootOut) {
  if (strncmp(name, "boot-", 5) != 0) return false;
  char* end = nullptr;
  unsigned long v = strtoul(name + 5, &end, 10);
  if (!end || *end != '\0' || end == name + 5) return false;
  *bootOut = (uint32_t)v;
  return true;
}

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

// Promote up to kMaxRenamesPerPass files out of one boot folder. Returns the
// number of renames performed; sets *folderClear when no promotable files
// remain (everything moved, or the leftovers are unpromotable/skipped).
static int promoteFolderPass(const char* folderName, const Anchor& a,
                             bool* folderClear) {
  *folderClear = true;
  int renames = 0;

  char folderPath[96];
  snprintf(folderPath, sizeof(folderPath), "%s/%s", CAPTURE_DIR_SENSORS,
           folderName);

  const AuthContext ctx = VFS::systemAuth("timeanchor.sweep");
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
    if (gSensorLoggingRunning && gSensorLogPath == srcPath) {
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

    char dayDir[96];
    snprintf(dayDir, sizeof(dayDir), "%s/%s", CAPTURE_DIR_SENSORS, day);
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
      registryPrune(a.boot);
    }
  }
  return renames;
}

// One bounded sweep pass over the sensors dir. Returns true when a full
// pass found nothing to do (sweep complete until re-armed).
static bool sweepPass() {
  // Registry load happens OUTSIDE the scan's FsLockGuard so the guard never
  // nests around another guarded open (safe today — FileManager nests the
  // same way — but no reason to depend on it here).
  if (!gRegistryLoaded) registryLoad();

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
    File dir = VFS::openGuarded(CAPTURE_DIR_SENSORS, "r",
                                VFS::systemAuth("timeanchor.scan"));
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
    renames += promoteFolderPass(folders[i], *a, &clear);
    if (!clear) allClear = false;
    if (renames >= kMaxRenamesPerPass) return false;  // budget spent, come back
  }
  // Complete only when we saw every anchored folder, all came up clear,
  // and no work was performed.
  return sawAll && allClear && renames == 0;
}

}  // namespace

void timeAnchorsTick() {
  if (!filesystemReady) return;

  // Re-arm the sweep when a logging session ends: the boot-named file it
  // held open is now promotable.
  static bool sWasRunning = false;
  const bool running = gSensorLoggingRunning;
  if (sWasRunning && !running) gSweepDone = false;
  sWasRunning = running;

  if (!Clock::isSynced()) return;

  static uint32_t sLastMs = 0;
  if (!everyMs(&sLastMs, 5000)) return;

  if (!gAnchoredThisBoot) {
    // Guard against double-append across a same-boot soft restart path:
    // if a line for this boot already exists, adopt it instead.
    if (registryFind(gBootCounter) == nullptr) writeAnchorLine();
    gAnchoredThisBoot = true;
  }

  if (!gSweepDone) gSweepDone = sweepPass();
}
