#include "System_VFS.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include <esp_attr.h>

#include "System_AuthIdentity.h"  // currentExecUser (event log attribution)
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Utils.h"  // argWantsJson
#include "System_Mutex.h"
#include "System_Notifications.h"

#include <LittleFS.h>
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
#include "HAL_SDCard.h"
// Preserve one filesystem access path; permissions and /sd routing below are
// independent of the physical transport.
static SdMmcCardFS& SD = gSdMmcCard;
#else
#include <SD.h>
#endif
#include "esp_littlefs.h"  // esp_littlefs_info — total+used in one traverse
#include <SPI.h>

#include <stdarg.h>

// ESP-IDF includes for low-level SD formatting
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// Helper for safe string formatting (file scope, outside namespace)
static int appendf(char* buf, int bufLen, int pos, const char* fmt, ...) {
  if (!buf || bufLen <= 0) return pos;
  if (pos < 0) pos = 0;
  if (pos >= bufLen - 1) {
    buf[bufLen - 1] = '\0';
    return bufLen - 1;
  }

  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + pos, bufLen - pos, fmt, args);
  va_end(args);

  if (written < 0) {
    buf[bufLen - 1] = '\0';
    return pos;
  }
  if (written >= (bufLen - pos)) {
    buf[bufLen - 1] = '\0';
    return bufLen - 1;
  }
  return pos + written;
}

// Stream a large NUL-terminated buffer through broadcastOutput line-by-line (respects OUTPUT_* flags).
static void broadcastMultilineReport(const char* text) {
  if (!text) {
    return;
  }
  const char* lineStart = text;
  for (;;) {
    const char* nl = strchr(lineStart, '\n');
    if (!nl) {
      if (lineStart[0] != '\0') {
        broadcastOutput(String(lineStart));
      }
      break;
    }
    size_t lineLen = (size_t)(nl - lineStart);
    if (lineLen == 0) {
      broadcastOutput("");
    } else {
      char chunk[256];
      size_t off = 0;
      while (off < lineLen) {
        size_t take = lineLen - off;
        if (take > sizeof(chunk) - 1) {
          take = sizeof(chunk) - 1;
        }
        memcpy(chunk, lineStart + off, take);
        chunk[take] = '\0';
        broadcastOutput(String(chunk));
        off += take;
      }
    }
    lineStart = nl + 1;
  }
}

namespace VFS {

static bool gSdMounted  = false;  // driver-level: selected SD transport mounted
static bool gSdWritable = false;  // probe-verified: a write round-trip worked
// Advanced under the filesystem lock by every successful mount (boot,
// remount, format), so per-mount owners can tell a new card from the old one.
static uint32_t gSdMountGeneration = 0;

uint32_t sdMountGeneration() { return gSdMountGeneration; }

// SD-in-use registrations, changed and read under the filesystem lock so a
// lifecycle call cannot interleave with an owner registering.
static constexpr size_t kSdInUseSlots = 4;
static const char* gSdInUse[kSdInUseSlots] = {};

static const char* sdInUseOwnerLocked() {
  for (const char* owner : gSdInUse) {
    if (owner) return owner;
  }
  return nullptr;
}

bool sdInUseBegin(const char* owner) {
  if (!owner) return false;
  FsLockGuard guard("VFS.sdInUseBegin");
  for (const char*& slot : gSdInUse) {
    if (!slot) { slot = owner; return true; }
  }
  return false;
}

void sdInUseEnd(const char* owner) {
  FsLockGuard guard("VFS.sdInUseEnd");
  for (const char*& slot : gSdInUse) {
    if (slot == owner) { slot = nullptr; return; }
  }
}

const char* sdInUseOwner() {
  FsLockGuard guard("VFS.sdInUseOwner");
  return sdInUseOwnerLocked();
}

// Caller holds the filesystem lock.
static bool sdLifecycleRefusedLocked(const char* action) {
  const char* owner = sdInUseOwnerLocked();
  if (!owner) return false;
  WARN_STORAGEF("[SD] %s refused: card in use by %s", action, owner);
  return true;
}

// Presentation-only capacity snapshots. The cache core is dependency-free and
// host-tested; this spinlock makes complete 64-bit snapshots safe to share on
// the ESP32's 32-bit cores. Filesystem I/O is always outside the critical
// section. A generation captured before each slow sample fences publication
// against invalidateStatsSnapshot() racing that sample.
static detail::CapacitySnapshotCache gCapacitySnapshots;
static portMUX_TYPE gCapacitySnapshotMux = portMUX_INITIALIZER_UNLOCKED;

static size_t capacityTierIndex(StorageType type) {
  // Preserve getStats' existing AUTO behavior: every type other than SDCARD
  // routes to the internal LittleFS tier.
  return type == SDCARD ? 1u : 0u;
}

static bool readCapacitySnapshot(StorageType type, uint32_t maxAgeMs,
                                 CapacitySnapshot& out) {
  bool hit = false;
  const size_t tier = capacityTierIndex(type);
  const uint32_t now = millis();
  portENTER_CRITICAL(&gCapacitySnapshotMux);
  hit = gCapacitySnapshots.read(tier, now, maxAgeMs, &out);
  portEXIT_CRITICAL(&gCapacitySnapshotMux);
  return hit;
}

static uint32_t capacitySnapshotGeneration(StorageType type) {
  uint32_t generation = 0;
  const size_t tier = capacityTierIndex(type);
  portENTER_CRITICAL(&gCapacitySnapshotMux);
  generation = gCapacitySnapshots.generation(tier);
  portEXIT_CRITICAL(&gCapacitySnapshotMux);
  return generation;
}

static void publishCapacitySnapshot(StorageType type,
                                    uint32_t expectedGeneration,
                                    const CapacitySnapshot& snapshot) {
  const size_t tier = capacityTierIndex(type);
  portENTER_CRITICAL(&gCapacitySnapshotMux);
  (void)gCapacitySnapshots.publish(tier, expectedGeneration, snapshot);
  portEXIT_CRITICAL(&gCapacitySnapshotMux);
}

// Why both flags exist:
//   A card can be "mounted" (driver initialized OK) but not actually writable:
//   flaky contacts, write-protect tab, wrong filesystem, full disk, card
//   physically yanked since last success. Arduino SD.begin only checks card
//   IDENT; it doesn't prove file ops will succeed. Callers that care about
//   "can I actually write a file here" (the video recorder, log overflow,
//   image saves) should check isSDWritable() and gate their UI on that.
//   Everything that just cares about "driver says it's there" (file browser
//   listing, diagnostics) can stay on isSDAvailable().
//
// Lifecycle:
//   tryMountSD succeeds     → gSdMounted=true, probe runs → gSdWritable=?
//   probe succeeds          → gSdWritable=true
//   any caller's write      → if it fails, noteSDWriteFailure() → gSdWritable=false
//   next isSDWritable() call → re-probe if gSdMounted but !gSdWritable
//   unmount / mount failure → both false
static bool probeSDWriteInternal();  // fwd decl

// Fully tear down SPI + SD so tryMountSD() can start from a clean slate.
static void spiTeardown() {
#if ENABLE_SD_CARD && !ENABLE_SDMMC_CARD && defined(SD_CS_PIN)
  SD.end();
  SPI.end();
  // Drive CS high so the card doesn't interpret noise as a command while the
  // bus is idle.
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
#endif
}

// Send the SD power-up sequence per the SD spec: 74+ clock edges with CS and
// MOSI held high before CMD0.  Arduino's SD.begin() is supposed to handle
// this, but doing it explicitly first helps with cards that just came out of
// a format / reset.
static void sdPowerUpClocks() {
#if ENABLE_SD_CARD && !ENABLE_SDMMC_CARD && defined(SD_CS_PIN)
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 80 clocks
  SPI.endTransaction();
  delay(5);
#endif
}

static bool tryMountSD() {
  // Mount/probe/teardown changes the same SD driver state used by open files.
  // Hold the global FS mutex for the complete transition so no task can enter
  // VFS::open/read/write while SPI and the FAT volume are being rebuilt.
  FsLockGuard guard("VFS.sdMount");
  invalidateStatsSnapshot(SDCARD);
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
  if (SD.begin()) {
    gSdMounted = true;
    ++gSdMountGeneration;
    gSdWritable = probeSDWriteInternal();
    char freeMb[16];
    const uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    snprintf(freeMb, sizeof(freeMb), "%llu",
             (unsigned long long)((total > used ? total - used : 0) / (1024ULL * 1024ULL)));
    systemEventPost(SYSEVT_SD_MOUNTED, freeMb, gSdWritable ? "writable" : "read-only");
    return true;
  }
  WARN_STORAGEF("[SD] SDMMC mount failed: %s", SD.lastError());
#elif ENABLE_SD_CARD && defined(SD_CS_PIN)
  // Three full attempts, each with a complete SPI bus reset, walking a
  // fastest-first frequency ladder.
  //
  // 10 MHz was ADDED above the previous 4 MHz rung rather than replacing it.
  // That matters: 4 MHz is the known-good rate this hardware ran on for a long
  // time, so a card or wiring that cannot sustain 10 MHz lands back on it
  // instead of dropping to the 400 kHz limp rung — which would be 10x SLOWER
  // than before the change. Every rung gets its own SD.end() + settle below, so
  // a failed fast attempt cannot leave stale SPI state behind.
  //
  // Why this is worth doing: cmd_voicefetch reads the whole WAV into PSRAM
  // before it emits a single byte (System_UartLink.cpp), and at 4 MHz SPI
  // (~410 kB/s through FatFs, MEASURED) that read was 2.47 ms per UART frame —
  // 31% of total transfer time, and the largest term after the wire itself.
  //
  // 10 MHz is divisor-exact: the ESP32-S3 SPI clock derives from the 80 MHz
  // APB, so only 80/N is achievable. 80/8 = 10 MHz lands exactly; asking for
  // 12 would quietly give 80/7 = 11.43 MHz.
  //
  // 16 MHz rung added 2026-08-10 (requested "15" — not achievable; 80/5 = 16
  // is the nearest exact divisor, 80/6 = 13.33 the conservative one). Same
  // ladder logic as the 10 MHz addition: a card/wiring that cannot mount at
  // 16 falls to the proven 10 and 4 MHz rungs, never to the 400 kHz limp rung.
  //
  // CAUTION: a successful mount is necessary but NOT sufficient evidence. The
  // mount handshake is short and low-rate; marginal signal integrity can pass
  // it and still corrupt data under sustained reads — and the ladder only
  // falls back at MOUNT time, so a card that mounts at 16 MHz and corrupts
  // under load stays at 16 MHz. The real test is sustained voicefetch traffic
  // with hash comparison — see the note in tryMountSD's caller docs.
  uint32_t frequencies[] = {16000000, 10000000, 4000000, 400000};
  const int FULL_ATTEMPTS = 3;

  for (int attempt = 0; attempt < FULL_ATTEMPTS; attempt++) {
    DEBUG_STORAGEF("[SD] Mount attempt %d/%d — resetting SPI bus",
                   attempt + 1, FULL_ATTEMPTS);

    // Complete teardown before each full attempt so we always start clean.
    spiTeardown();
    delay(50 + attempt * 100);   // back-off: 50 ms, 150 ms, 250 ms

    // Re-initialise SPI with explicit pins if defined.
#if defined(SD_SCK_PIN) && defined(SD_MISO_PIN) && defined(SD_MOSI_PIN)
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
#else
    SPI.begin();
#endif
    delay(10);

    // SD spec power-up sequence.
    sdPowerUpClocks();

    // Try each frequency in order.
    for (uint32_t freq : frequencies) {
      DEBUG_STORAGEF("[SD]   Trying %lu Hz...", freq);
      if (SD.begin(SD_CS_PIN, SPI, freq, "/sd")) {
        INFO_STORAGEF("[SD] Mount SUCCESS at %lu Hz (attempt %d)",
                      freq, attempt + 1);
        gSdMounted = true;
        ++gSdMountGeneration;
        // Prove writability with a round-trip probe. If the card is
        // present but e.g. write-protected or has a bad sector where
        // SD.open lands, we want to know now, not the first time the
        // video recorder tries to create a file.
        gSdWritable = probeSDWriteInternal();
        INFO_STORAGEF("[SD] Write probe: %s", gSdWritable ? "PASS" : "FAIL");
        {
          char freeMb[16];
          snprintf(freeMb, sizeof(freeMb), "%llu",
                   (unsigned long long)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL)));
          systemEventPost(SYSEVT_SD_MOUNTED, freeMb, gSdWritable ? "writable" : "read-only");
        }
        return true;
      }
      DEBUG_STORAGEF("[SD]   Failed at %lu Hz", freq);
      // Tear down SD between frequency changes so SD.begin() always starts
      // with a fresh SPI transaction state.
      SD.end();
      delay(50);
    }

    WARN_STORAGEF("[SD] Attempt %d/%d failed", attempt + 1, FULL_ATTEMPTS);
  }

  WARN_STORAGEF("[SD] All mount attempts exhausted");
#endif
  gSdMounted = false;
  gSdWritable = false;
  return false;
}

bool init() {
  // LittleFS is initialized in initFilesystem(). We only mount SD here.
  return tryMountSD();
}

bool isLittleFSReady() {
  return filesystemReady;
}

// Write a tiny probe file, read it back, delete it. Returns true only if all
// three succeed. Deliberately minimal — 13 bytes — so the cost is negligible.
// Logs which stage failed so the operator can tell a write-protected / dirty
// filesystem ("fopen failed") apart from a real data-integrity issue
// ("readback mismatch") apart from path quirks.
//
// Filename choice matters: some FAT drivers handle leading-dot filenames
// oddly (no 8.3 basename). The temporary names are 8.3-safe and all-caps so
// they round-trip cleanly on any FAT implementation.
static bool probeSDWriteInternal() {
#if ENABLE_SD_CARD
  if (!isSDAvailable()) return false;
  // A user may already have a similarly named file on a removable card.
  // Pick a vacant 8.3 name; never truncate or delete existing card contents.
  char kProbePath[16];
  bool found = false;
  for (unsigned i = 0; i < 16; ++i) {
    snprintf(kProbePath, sizeof(kProbePath), "/HWPR%04u.TMP", i);
    if (!SD.exists(kProbePath)) { found = true; break; }
  }
  if (!found) return false;
  const char* kProbeData = "hwone-sdtest\n";  // 13 bytes
  const size_t kProbeLen = 13;

  // Stage 1: create + write.
  // NOTE: the third arg `create=true` is essential on ESP32 Arduino's SD
  // library. The two-arg form `SD.open(path, FILE_WRITE)` defaults create
  // to false, so if the path doesn't already exist the call returns a null
  // File — even though the card is fully writable. This bit us hard with
  // the original probe and the first version of the video recorder.
  File f = SD.open(kProbePath, FILE_WRITE, true);
  if (!f) {
    INFO_STORAGEF("[SD probe] FAIL stage=open_write path=%s — card refuses file creation (RO / dirty / full)", kProbePath);
    return false;
  }
  size_t wrote = f.write((const uint8_t*)kProbeData, kProbeLen);
  f.flush();
  f.close();
  if (wrote != kProbeLen) {
    INFO_STORAGEF("[SD probe] FAIL stage=write wrote=%u expected=%u — partial write, likely FS full or corrupt",
                  (unsigned)wrote, (unsigned)kProbeLen);
    SD.remove(kProbePath);
    return false;
  }

  // Stage 2: read back
  File r = SD.open(kProbePath, FILE_READ);
  if (!r) {
    INFO_STORAGEF("[SD probe] FAIL stage=open_read path=%s — write succeeded but file vanished", kProbePath);
    return false;
  }
  char buf[16] = {0};
  size_t got = r.read((uint8_t*)buf, kProbeLen);
  r.close();
  if (got != kProbeLen || memcmp(buf, kProbeData, kProbeLen) != 0) {
    INFO_STORAGEF("[SD probe] FAIL stage=readback got=%u expected=%u match=%d — data integrity issue",
                  (unsigned)got, (unsigned)kProbeLen,
                  (got == kProbeLen && memcmp(buf, kProbeData, kProbeLen) == 0) ? 1 : 0);
    SD.remove(kProbePath);
    return false;
  }

  // Stage 3: cleanup
  if (!SD.remove(kProbePath)) {
    INFO_STORAGEF("[SD probe] warn: write+read OK but delete failed path=%s", kProbePath);
    // Not a fatal probe failure — we successfully wrote data, reading it back
    // proved writes work. A stuck orphan file is cosmetic.
  }
  return true;
#else
  return false;
#endif
}

bool isSDAvailable() {
  FsLockGuard guard("VFS.sdAvailable");
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
  if (gSdMounted && !SD.available()) {
    gSdWritable = false;
    invalidateStatsSnapshot(SDCARD);
    return false;
  }
#endif
  return gSdMounted;
}

// Writable check — re-probes lazily if the state was invalidated via
// noteSDWriteFailure(). This gives us "auto-recover" behaviour: if a card
// was yanked and re-inserted and a later probe succeeds, we flip back to
// writable without needing an explicit remount.
bool isSDWritable() {
  FsLockGuard guard("VFS.sdWritable");
  if (!isSDAvailable()) return false;
  if (gSdWritable) return true;
  // Not currently writable — maybe it was, maybe it is again. Try once.
  if (probeSDWriteInternal()) {
    INFO_STORAGEF("[SD] Write probe now succeeding — card is writable again");
    gSdWritable = true;
    // Edge from failed->ok: recovery counterpart to SYSEVT_SD_WRITE_FAILED.
    // Only reached while gSdWritable was false, so this fires once per recovery.
    systemEventPost(SYSEVT_SD_WRITE_RECOVERED, "sd", "write probe succeeded after prior failure");
    return true;
  }
  return false;
}

// Callers that attempt an SD write and see it fail should call this to
// invalidate the cached writable flag. Next isSDWritable() query will
// re-probe. Cheap — single bool write.
void noteSDWriteFailure(const char* hint) {
  FsLockGuard guard("VFS.sdWriteFailure");
  if (gSdWritable) {
    WARN_STORAGEF("[SD] Write failure reported (%s); marking card not writable",
                  hint ? hint : "unspecified");
    logSystemEvent("SD", "SD write failure (hint=%s) — card marked not writable",
                   hint ? hint : "unspecified");
    // Inside the writable->not transition gate: once per failure episode.
    systemEventPost(SYSEVT_SD_WRITE_FAILED, hint ? hint : "unspecified");
  }
  gSdWritable = false;
}

StorageType getStorageType(const String& path) {
  String p = normalize(path);
  if (p == "/sd" || p.startsWith("/sd/")) return SDCARD;
  return INTERNAL;
}

String normalize(const String& path) {
  String p = path;
  p.trim();
  if (p.length() == 0) return "/";
  if (!p.startsWith("/")) {
    // Pre-reserve to avoid two separate allocations for the prepend.
    String prepended;
    prepended.reserve(p.length() + 1);
    prepended = '/';
    prepended += p;
    p = prepended;
  }

  // Collapse repeated slashes (best-effort)
  while (p.indexOf("//") >= 0) {
    p.replace("//", "/");
  }

  // Remove trailing slash except for root and /sd
  if (p.length() > 1 && p.endsWith("/") && p != "/sd/") {
    while (p.length() > 1 && p.endsWith("/")) {
      p.remove(p.length() - 1);
    }
  }

  if (p == "/sd/") p = "/sd";
  return p;
}

String stripSdPrefix(const String& path) {
  String p = normalize(path);
  if (p == "/sd") return "/";
  if (p.startsWith("/sd/")) {
    return p.substring(3);
  }
  return p;
}

size_t listVirtualEntries(const String& parentPath, VirtualEntry* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  const String p = normalize(parentPath);

  // Today's only virtual entry: /sd, surfaced at the LittleFS root when
  // the card is mounted. New mount points get added here — keep entries
  // alphabetised by name so consumer ordering is stable.
  if (p == "/" && isSDAvailable()) {
    if (n < cap) {
      out[n].name = "sd";
      out[n].isFolder = true;
      n++;
    }
  }

  return n;
}

DirectoryReader::DirectoryReader(File& directory, const String& parentPath, bool includeModifiedTime)
    : directory_(directory), parentPath_(normalize(parentPath)),
      backendPrefix_(stripSdPrefix(parentPath_)),
      includeModifiedTime_(includeModifiedTime),
      virtualCount_(listVirtualEntries(parentPath_, virtuals_,
                                      sizeof(virtuals_) / sizeof(virtuals_[0]))) {
  if (!backendPrefix_.endsWith("/")) backendPrefix_ += "/";
}

bool DirectoryReader::next(DirectoryEntry& entry) {
  if (!directory_ || !directory_.isDirectory()) return false;

  DirectoryEntry next;
  if (virtualIndex_ < virtualCount_) {
    const VirtualEntry& mount = virtuals_[virtualIndex_++];
    next.name = mount.name;
    next.isFolder = mount.isFolder;
    next.isVirtual = true;
  } else {
    for (;;) {
      File child = directory_.openNextFile();
      if (!child) return false;
      next.name = child.name();
      if (next.name.startsWith(backendPrefix_)) {
        next.name = next.name.substring(backendPrefix_.length());
      }
      next.isFolder = child.isDirectory();
      next.size = next.isFolder ? 0 : child.size();
      next.modifiedAt = !next.isFolder && includeModifiedTime_ ? child.getLastWrite() : 0;
      child.close();
      // Backends may return leaf names or directory-rooted names. Anything
      // still containing a slash is not a direct child of this directory.
      if (next.name.length() && next.name.indexOf('/') == -1) break;
    }
  }
  next.path = parentPath_;
  if (!next.path.endsWith("/")) next.path += "/";
  next.path += next.name;
  entry = next;
  return true;
}

static FS* fsForPath(const String& path) {
  return (getStorageType(path) == SDCARD) ? (FS*)&SD : (FS*)&LittleFS;
}

bool exists(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.exists");

  if (getStorageType(p) == SDCARD) {
    if (!isSDAvailable()) return false;
    if (p == "/sd") return true;
    return SD.exists(p.c_str() + 3);
  }

  if (!filesystemReady) return false;
  return LittleFS.exists(p);
}

File open(const String& path, const char* mode, bool create) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return File();  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.open");

  if (getStorageType(p) == SDCARD) {
    if (!isSDAvailable()) return File();
    const char* sdPath = (p == "/sd") ? "/" : p.c_str() + 3;
    return SD.open(sdPath, mode, create);
  }

  if (!filesystemReady) return File();
  return LittleFS.open(p.c_str(), mode, create);
}

bool mkdir(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.mkdir");

  if (getStorageType(p) == SDCARD) {
    if (!isSDAvailable()) return false;
    if (p == "/sd") return false;
    return SD.mkdir(p.c_str() + 3);
  }

  if (!filesystemReady) return false;
  return LittleFS.mkdir(p);
}

// Post the file_deleted event: basename as subject (matchable), full path as
// detail. Replaces the old notifyFileDeleted wrapper (Phase-1 cutover).
static void postFileDeleted(const char* path) {
  const char* name = path;
  const char* slash = path ? strrchr(path, '/') : nullptr;
  if (slash) name = slash + 1;
  systemEventPost(SYSEVT_FILE_DELETED, name, path);
}

bool remove(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.remove");

  if (getStorageType(p) == SDCARD) {
    if (!isSDAvailable()) return false;
    if (p == "/sd") return false;
    bool ok = SD.remove(p.c_str() + 3);
    if (ok) postFileDeleted(p.c_str());
    return ok;
  }

  if (!filesystemReady) return false;
  bool ok = LittleFS.remove(p);
  if (ok) {
    postFileDeleted(p.c_str());
    invalidateLittleFsFreeCache();  // free space just grew; don't trust the stale reading
  }
  return ok;
}

bool rename(const String& pathFrom, const String& pathTo) {
  String from = normalize(pathFrom);
  String to = normalize(pathTo);
  if (from.indexOf("..") >= 0 || to.indexOf("..") >= 0) return false;  // reject traversal

  StorageType tf = getStorageType(from);
  StorageType tt = getStorageType(to);
  if (tf != tt) {
    return false;
  }

  FsLockGuard guard("VFS.rename");

  if (tf == SDCARD) {
    if (!isSDAvailable()) return false;
    if (from == "/sd" || to == "/sd") return false;
    return SD.rename(from.c_str() + 3, to.c_str() + 3);
  }

  if (!filesystemReady) return false;
  bool ok = LittleFS.rename(from, to);
  if (ok) invalidateLittleFsFreeCache();  // rename-over-existing freed the overwritten file
  return ok;
}

bool rmdir(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.rmdir");

  if (getStorageType(p) == SDCARD) {
    if (!isSDAvailable()) return false;
    if (p == "/sd") return false;
    bool ok = SD.rmdir(p.c_str() + 3);
    if (ok) postFileDeleted(p.c_str());
    return ok;
  }

  if (!filesystemReady) return false;
  bool ok = LittleFS.rmdir(p);
  if (ok) postFileDeleted(p.c_str());
  return ok;
}

struct FreshCapacitySample {
  CapacitySnapshot snapshot = {};
  bool valid = false;
};

// The caller owns gFsMutex. Only a usable sample succeeds or enters the cache;
// public callers leave their output parameters untouched on failure.
static FreshCapacitySample sampleCapacityFreshLocked(StorageType type) {
  FreshCapacitySample result;

  if (type == SDCARD) {
    if (!isSDAvailable()) return result;
    result.snapshot.totalBytes = SD.totalBytes();
    // Arduino SD reports zero on failure. A mounted volume with zero total
    // capacity is unusable; zero used bytes alone can also mean an empty card.
    if (result.snapshot.totalBytes == 0) return result;
    result.snapshot.usedBytes = SD.usedBytes();
  } else {
    if (!filesystemReady) return result;
    // Sample total and used in one driver call. Calling both Arduino getters
    // would traverse LittleFS twice and could straddle a concurrent write.
    size_t lfsTotal = 0, lfsUsed = 0;
    const esp_err_t infoErr =
        esp_littlefs_info(kLittleFsPartitionLabel, &lfsTotal, &lfsUsed);
    if (infoErr != ESP_OK || lfsTotal == 0) return result;
    result.snapshot.totalBytes = lfsTotal;
    result.snapshot.usedBytes = lfsUsed;
  }

  result.snapshot.freeBytes =
      (result.snapshot.totalBytes > result.snapshot.usedBytes)
          ? (result.snapshot.totalBytes - result.snapshot.usedBytes)
          : 0;
  result.snapshot.sampledAtMs = millis();
  result.valid = true;
  return result;
}

bool getStats(StorageType type, uint64_t& totalBytes, uint64_t& usedBytes,
              uint64_t& freeBytes) {
  // This API is intentionally always fresh. Snapshot users must opt in through
  // getStatsSnapshot with a non-zero maxAgeMs.
  FsLockGuard guard("VFS.getStats");
  const uint32_t generation = capacitySnapshotGeneration(type);
  const FreshCapacitySample result = sampleCapacityFreshLocked(type);
  if (!result.valid) return false;

  totalBytes = result.snapshot.totalBytes;
  usedBytes = result.snapshot.usedBytes;
  freeBytes = result.snapshot.freeBytes;
  publishCapacitySnapshot(type, generation, result.snapshot);
  return true;
}

bool getStatsSnapshot(StorageType type, CapacitySnapshot& out,
                      uint32_t maxAgeMs) {
  // Fast presentation path: copy one complete snapshot under the short cache
  // spinlock, without waiting behind an unrelated long filesystem operation.
  if (maxAgeMs > 0 && readCapacitySnapshot(type, maxAgeMs, out)) return true;

  FsLockGuard guard("VFS.getStatsSnapshot");

  // Two callers can miss together. The first performs the slow sample while
  // holding gFsMutex; after the second acquires it, recheck so it reuses the
  // first result instead of immediately repeating a LittleFS traversal.
  if (maxAgeMs > 0 && readCapacitySnapshot(type, maxAgeMs, out)) return true;

  const uint32_t generation = capacitySnapshotGeneration(type);
  const FreshCapacitySample result = sampleCapacityFreshLocked(type);
  if (!result.valid) return false;  // Leave `out` untouched on failure.

  out = result.snapshot;
  publishCapacitySnapshot(type, generation, result.snapshot);
  return true;
}

void invalidateStatsSnapshot(StorageType type) {
  const size_t tier = capacityTierIndex(type);
  portENTER_CRITICAL(&gCapacitySnapshotMux);
  gCapacitySnapshots.invalidate(tier);
  portEXIT_CRITICAL(&gCapacitySnapshotMux);
}

// ============================================================================
// Overflow-aware path resolution (opt-in tier)
// ============================================================================
// See System_VFS.h for the design contract. Tl;dr: append-only log code asks
// resolveOverflowPath to either return the primary LittleFS path OR switch to
// the /sd mirror once LittleFS free space is below the reserve floor. The
// latch is session-scoped (reboot clears it) to keep a single log stream on
// one tier.

// Global floor: LittleFS free bytes below which any caller's request tips the
// overflow latch. This headroom is preserved for NON-log writes (settings
// saves, automations.json edits, user sessions, image captures) so those
// operations continue to succeed even after logs have moved to SD.
static constexpr size_t LOG_OVERFLOW_DEFAULT_RESERVE = 100 * 1024;  // 100 KB margin

static bool           gLogOverflowActive   = false;
static bool           gLogOverflowWarned   = false;
static unsigned long  gLogFreeCheckLastMs  = 0;
static size_t         gLogFreeCheckCached  = SIZE_MAX;

// Rough bytes-written-since-refresh counter. Incremented by
// `noteLittleFsBytesWritten` after any successful LittleFS-targeted write,
// checked by `refreshLittleFsFreeCached` to force a refresh when cumulative
// writes exceed a threshold — prevents stale cached-free-space from misleading
// the overflow decision during write-heavy bursts that land between the
// time-based 2s refreshes.
static size_t gLogFreeBytesWrittenSinceRefresh = 0;

// Threshold that forces a cache refresh regardless of age. 32 KB chosen as
// roughly one-tenth the default LOG_OVERFLOW_DEFAULT_RESERVE of 100 KB, so
// the cache can at worst be wrong by ~32 KB before overflow kicks in.
static constexpr size_t LOG_FREE_CACHE_BYTES_THRESHOLD = 32 * 1024;

// How long a free-space reading stays good, as a function of how much headroom
// it reported. A full traverse costs 216-776 ms (measured) under the global FS
// mutex, and appendLineWithCap calls this on EVERY logged line — so with a flat
// 2 s window every interactive command typed more than 2 s after the last one
// paid a whole volume walk before it did any work.
//
// The relaxation is bounded on purpose. gLogFreeBytesWrittenSinceRefresh has
// exactly ONE caller (appendLineWithCap), so the byte counter sees log appends
// and NOTHING else — photos, settings, captures and OTA staging all consume
// space invisibly to it. The time-based refresh is the only thing that catches
// those, which is why it is scaled rather than removed, and why the tight 2 s
// cadence is kept unchanged whenever the reading is anywhere near the reserve.
static uint32_t logFreeCheckIntervalMs(size_t cachedFree) {
  if (cachedFree <= LOG_OVERFLOW_DEFAULT_RESERVE * 2) return 2000;   // near the edge: unchanged
  if (cachedFree <= LOG_OVERFLOW_DEFAULT_RESERVE * 8) return 5000;   // comfortable
  return 15000;                                                      // >800 KB spare
}

static size_t refreshLittleFsFreeCached() {
  unsigned long now = millis();
  const bool stale = (now - gLogFreeCheckLastMs) >
                     logFreeCheckIntervalMs(gLogFreeCheckCached);
  const bool writePressure = gLogFreeBytesWrittenSinceRefresh >= LOG_FREE_CACHE_BYTES_THRESHOLD;
  if (gLogFreeCheckCached == SIZE_MAX || stale || writePressure) {
    FsLockGuard guard("VFS.overflowFreeCheck");
    // Same one-traverse-not-two rule as VFS::getStats above, and it matters
    // more here: this runs on a 2 s cadence (plus write-pressure forcing)
    // while holding the global FS mutex, and ESP-IDF's httpd is single-task,
    // so every extra lfs_fs_size() here stalls every pending web request.
    size_t total = 0, used = 0;
    (void)esp_littlefs_info(kLittleFsPartitionLabel, &total, &used);
    gLogFreeCheckCached = (total > used) ? (total - used) : 0;
    gLogFreeCheckLastMs = now;
    gLogFreeBytesWrittenSinceRefresh = 0;
  }
  return gLogFreeCheckCached;
}

// Called by LittleFS-targeted writers to keep the free-space cache honest
// during write bursts. Cheap: just a counter add. Accuracy is approximate —
// we count bytes the caller claimed to write, not actual LittleFS overhead.
// That's fine: this is a "force an early refresh" hint, not an accounting ledger.
void noteLittleFsBytesWritten(size_t bytes) {
  gLogFreeBytesWrittenSinceRefresh += bytes;
}

// Explicit invalidation. Callers that remove files or do an unusual large
// operation can call this to force the next free-space query to hit the
// filesystem. Cheap (single volatile-ish write).
void invalidateLittleFsFreeCache() {
  gLogFreeCheckCached = SIZE_MAX;
  gLogFreeBytesWrittenSinceRefresh = 0;
}

size_t getCachedLittleFsFree() { return refreshLittleFsFreeCached(); }

bool isLogOverflowActive() { return gLogOverflowActive; }

bool resolveOverflowPath(const char* primaryPath, size_t reserveBytes,
                        char* outPath, size_t outPathLen) {
  if (!primaryPath || !outPath || outPathLen == 0) return false;

  // Latch into overflow mode if we drop below the reserve margin. The global
  // default acts as a FLOOR — callers can request a LARGER margin (e.g. the
  // sensor log passes its rotation-chunk size so overflow triggers before a
  // full rotation fails) but can't shrink the floor below the global 100KB
  // safety zone that non-log writes (settings, automations, sessions) depend
  // on.
  if (!gLogOverflowActive) {
    size_t want = LOG_OVERFLOW_DEFAULT_RESERVE;
    if (reserveBytes > want) want = reserveBytes;
    if (refreshLittleFsFreeCached() < want) {
      gLogOverflowActive = true;
      if (!gLogOverflowWarned) {
        gLogOverflowWarned = true;
        WARN_SYSTEMF("[LOG] LittleFS low on free space (%u < %u bytes); new log writes will route to SD overflow",
                     (unsigned)gLogFreeCheckCached, (unsigned)want);
        logSystemEvent("FS", "LittleFS free below reserve (%u < %u bytes) — log writes latched to SD overflow (SD mounted: %s%s)",
                       (unsigned)gLogFreeCheckCached, (unsigned)want,
                       isSDAvailable() ? "yes" : "no",
                       isSDAvailable() ? "" : "; log data may be dropped");
        // Inside the once-per-boot latch: zero spam by construction.
        char freeB[16], wantB[24];
        snprintf(freeB, sizeof(freeB), "%u", (unsigned)gLogFreeCheckCached);
        snprintf(wantB, sizeof(wantB), "reserve %u", (unsigned)want);
        systemEventPost(SYSEVT_FS_LOW_SPACE, freeB, wantB);
      }
    }
  }

  if (!gLogOverflowActive) {
    snprintf(outPath, outPathLen, "%s", primaryPath);
    return false;
  }

  // Overflow active: use /sd mirror if SD is mounted. Otherwise fall back
  // to primary path and accept the silent-loss behavior that existed before
  // this tiering was added.
  if (!isSDAvailable()) {
    snprintf(outPath, outPathLen, "%s", primaryPath);
    return false;
  }

  // Defensive: if the caller already passed a /sd path, leave it alone.
  if (strncmp(primaryPath, "/sd/", 4) == 0 || strcmp(primaryPath, "/sd") == 0) {
    snprintf(outPath, outPathLen, "%s", primaryPath);
    return true;
  }

  if (primaryPath[0] == '/') {
    snprintf(outPath, outPathLen, "/sd%s", primaryPath);
  } else {
    snprintf(outPath, outPathLen, "/sd/%s", primaryPath);
  }
  return true;
}

bool unmountSD() {
  FsLockGuard guard("VFS.sdUnmount");
  if (sdLifecycleRefusedLocked("Unmount")) return false;
  invalidateStatsSnapshot(SDCARD);
#if ENABLE_SD_CARD
  if (gSdMounted) {
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
    const bool ended = SD.end();
    gSdMounted = false;
    gSdWritable = false;
    if (!ended) return false;
#else
    SD.end();
#endif
    gSdMounted = false;
    gSdWritable = false;
    systemEventPost(SYSEVT_SD_UNMOUNTED);
    return true;
  }
#endif
  return false;
}

bool remountSD() {
  FsLockGuard guard("VFS.sdRemount");
  if (sdLifecycleRefusedLocked("Remount")) return false;
  invalidateStatsSnapshot(SDCARD);
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
  const bool ended = SD.end();
  gSdMounted = false;
  gSdWritable = false;
  if (!ended) return false;
  return tryMountSD();
#elif ENABLE_SD_CARD && defined(SD_CS_PIN)
  // Full teardown regardless of current mount state — guarantees a clean bus.
  spiTeardown();
  gSdMounted = false;
  gSdWritable = false;
  delay(100);
  return tryMountSD();
#else
  return false;
#endif
}

// Format SD card as FAT32 using ESP-IDF low-level API
bool formatSD() {
  FsLockGuard guard("VFS.sdFormat");
  if (sdLifecycleRefusedLocked("Format")) return false;
  invalidateStatsSnapshot(SDCARD);
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
  // Authorization and exact confirmation are enforced by cmd_sdformat.
  gSdMounted = false;
  gSdWritable = false;
  if (!SD.format()) return false;
  gSdMounted = true;
  ++gSdMountGeneration;
  gSdWritable = probeSDWriteInternal();
  return true;
#elif ENABLE_SD_CARD && defined(SD_CS_PIN)
  INFO_STORAGEF("[SD FORMAT] Starting format process...");
  
  // Must unmount Arduino SD first and end SPI
  if (gSdMounted) {
    DEBUG_STORAGEF("[SD FORMAT] Unmounting Arduino SD...");
    SD.end();
    gSdMounted = false;
    gSdWritable = false;
  }
  SPI.end();  // End Arduino SPI so ESP-IDF can take over
  
  // Initialize the SPI bus for ESP-IDF
  DEBUG_STORAGEF("[SD FORMAT] Initializing SPI bus: SCK=%d, MISO=%d, MOSI=%d", 
                SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  
  spi_bus_config_t bus_cfg = {
    .mosi_io_num = SD_MOSI_PIN,
    .miso_io_num = SD_MISO_PIN,
    .sclk_io_num = SD_SCK_PIN,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000,
  };
  
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ERROR_STORAGEF("[SD FORMAT] SPI bus init failed: 0x%x", ret);
    return false;
  }
  DEBUG_STORAGEF("[SD FORMAT] SPI bus initialized");
  
  // Use ESP-IDF SDMMC/SPI host to format
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SD_CS_PIN;
  slot_config.host_id = SPI2_HOST;
  
  DEBUG_STORAGEF("[SD FORMAT] SD slot config: CS=%d, host=%d", SD_CS_PIN, SPI2_HOST);
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };
  
  sdmmc_card_t* card = nullptr;
  
  // Mount with format_if_mount_failed - this will format exFAT/unformatted cards
  DEBUG_STORAGEF("[SD FORMAT] Attempting ESP-IDF mount...");
  ret = esp_vfs_fat_sdspi_mount("/sdformat", &host, &slot_config, &mount_config, &card);
  
  if (ret != ESP_OK) {
    ERROR_STORAGEF("[SD FORMAT] ESP-IDF mount failed: 0x%x", ret);
    // Try explicit format
    if (card) {
      DEBUG_STORAGEF("[SD FORMAT] Trying explicit format...");
      ret = esp_vfs_fat_sdcard_format("/sdformat", card);
    }
    if (ret != ESP_OK) {
      ERROR_STORAGEF("[SD FORMAT] Format failed: 0x%x", ret);
      spi_bus_free(SPI2_HOST);
      spiTeardown();
      delay(200);
      return false;
    }
  } else {
    DEBUG_STORAGEF("[SD FORMAT] ESP-IDF mount successful, formatting...");
    // Mounted successfully, now format it explicitly to ensure FAT32
    ret = esp_vfs_fat_sdcard_format("/sdformat", card);
    if (ret != ESP_OK) {
      ERROR_STORAGEF("[SD FORMAT] Format failed: 0x%x", ret);
      esp_vfs_fat_sdcard_unmount("/sdformat", card);
      spi_bus_free(SPI2_HOST);
      return false;
    }
  }
  
  INFO_STORAGEF("[SD FORMAT] Format complete, unmounting ESP-IDF...");
  esp_vfs_fat_sdcard_unmount("/sdformat", card);
  spi_bus_free(SPI2_HOST);

  // The ESP-IDF SPI master driver fully resets the peripheral when freed.
  // The Arduino SPI library needs the hardware to settle before it can
  // reinitialise it — without this delay SD.begin() either hangs or
  // immediately returns false.
  INFO_STORAGEF("[SD FORMAT] SPI bus released, settling before remount...");
  spiTeardown();    // make sure CS is high and Arduino side is clean too
  delay(500);       // 500 ms is enough for all tested cards

  DEBUG_STORAGEF("[SD FORMAT] Remounting with Arduino SD...");
  return tryMountSD();
#else
  return false;
#endif
}

// ============================================================================
// Guarded VFS — single enforcement point
// ============================================================================
// Pipeline:
//   1. normalizeFsPath — reject .., collapse //, strip trailing /. Failure
//      → log + deny. Failure here is treated as a path-injection attempt
//      so we log a stronger reason than a normal perm denial.
//   2. canX(normalized, ctx) — three-role check. Logs its own denial.
//   3. Dispatch to the underlying VFS operation.
//
// File openGuarded returns an empty File on denial (caller checks `!file`).

static bool guardedNormalize(const String& in, String& out, const char* op,
                             const AuthContext& ctx) {
  if (!normalizeFsPath(in, out)) {
    DEBUG_STORAGEF("[PERM] DENY %s '%s' role=%s reason=path-rejected (traversal/empty)",
                   op, in.c_str(),
                   ctx.user.length() == 0 ? "anon" : ctx.user.c_str());
    return false;
  }
  return true;
}

// NOTE on logging: canX() are silent queries. Each *Guarded function below
// emits a single [PERM] DENY line via logFsAccessDeny() ONLY when an actual
// access attempt is refused. This keeps the audit trail intact for real
// security events while leaving aggregate UI permission queries (e.g.
// getPermissions in a file listing) noise-free.

// The permission a write-mode open needs on a normalized path. canEdit covers
// an existing file; a path that does not exist yet is a create rather than an
// edit, so canCreate is the fallback (the rule table author can grant CREATE
// without WRITE for "drop-in only" directories like /sd/g2_icon_animations).
// Logs the denial.
static bool writePermitted(const String& norm, const AuthContext& ctx) {
  if (canEdit(norm, ctx)) return true;
  if (!exists(norm)) {
    if (canCreate(norm, ctx)) return true;
    logFsAccessDeny(norm, ctx, PERM_CREATE, "create");
    return false;
  }
  // Existing file + no edit perm = denied
  logFsAccessDeny(norm, ctx, PERM_WRITE, "edit");
  return false;
}

File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create) {
  String norm;
  if (!guardedNormalize(path, norm, "open", ctx)) return File();

  // Determine which permission the requested mode needs. "r" → read,
  // anything else → edit (the underlying VFS::open with a write/append
  // mode will create-or-truncate, which is an edit).
  bool needsRead  = (mode != nullptr && mode[0] == 'r');
  bool needsWrite = !needsRead;

  if (needsRead && !canRead(norm, ctx)) {
    logFsAccessDeny(norm, ctx, PERM_READ, "read");
    return File();
  }
  if (needsWrite && !writePermitted(norm, ctx)) return File();
  return open(norm, mode, create);
}

bool canWriteGuarded(const String& path, const AuthContext& ctx) {
  String norm;
  if (!guardedNormalize(path, norm, "write", ctx)) return false;
  return writePermitted(norm, ctx);
}

bool existsGuarded(const String& path, const AuthContext& ctx) {
  String norm;
  if (!guardedNormalize(path, norm, "exists", ctx)) return false;
  // exists() leaks information about whether a file is present; gate it
  // behind READ. If you can't read the file, you can't probe its presence.
  if (!canRead(norm, ctx)) {
    logFsAccessDeny(norm, ctx, PERM_READ, "exists");
    return false;
  }
  return exists(norm);
}

bool removeGuarded(const String& path, const AuthContext& ctx) {
  String norm;
  if (!guardedNormalize(path, norm, "remove", ctx)) return false;
  if (!canDelete(norm, ctx)) {
    logFsAccessDeny(norm, ctx, PERM_DELETE, "delete");
    return false;
  }
  return remove(norm);
}

bool renameGuarded(const String& pathFrom, const String& pathTo, const AuthContext& ctx) {
  String normFrom, normTo;
  if (!guardedNormalize(pathFrom, normFrom, "rename(from)", ctx)) return false;
  if (!guardedNormalize(pathTo,   normTo,   "rename(to)",   ctx)) return false;
  // Rename requires RENAME on source AND CREATE on destination — moving a
  // file into a directory you can't create in is denied.
  if (!canRename(normFrom, ctx)) {
    logFsAccessDeny(normFrom, ctx, PERM_RENAME, "rename");
    return false;
  }
  if (!canCreate(normTo, ctx)) {
    logFsAccessDeny(normTo, ctx, PERM_CREATE, "rename-dst");
    return false;
  }
  return rename(normFrom, normTo);
}

bool replaceFileGuarded(const String& temporary, const String& destination,
                        const AuthContext& ctx) {
  String from, to;
  if (!guardedNormalize(temporary, from, "replace(from)", ctx) ||
      !guardedNormalize(destination, to, "replace(to)", ctx) || from == to) return false;
  const StorageType storage = getStorageType(from);
  if (storage != getStorageType(to)) return false;

  // A backup that outlives this call is reported once the lock is released:
  // no other code looks for it, and the next replacement of `to` refuses it.
  enum class Backup { None, Blocks, Unrestored, Retained } note = Backup::None;
  String backup;
  bool promoted = false;
  {
    FsLockGuard guard("VFS.replace");
    if (storage != SDCARD || !existsGuarded(to, ctx)) {
      promoted = renameGuarded(from, to, ctx);
    } else {
      File previous = openGuarded(to, "r", ctx);
      if (!previous) return false;
      const bool destinationIsDirectory = previous.isDirectory();
      previous.close();
      if (destinationIsDirectory) return false;

      // FAT cannot rename over an existing file. Keep its bytes under a sibling
      // name until promotion completes, and never consume an unresolved backup.
      backup = to;
      if (backup.length() != to.length() || !backup.concat(".bak") || backup == from) return false;
      if (existsGuarded(backup, ctx)) {
        note = Backup::Blocks;
      } else if (!renameGuarded(to, backup, ctx)) {
        return false;
      } else if (!renameGuarded(from, to, ctx)) {
        // If restoration also fails, retain the old file at .bak. The caller may
        // remove its temporary, but must not delete this remaining recovery copy.
        if (!renameGuarded(backup, to, ctx)) note = Backup::Unrestored;
      } else {
        promoted = true;
        if (!removeGuarded(backup, ctx)) note = Backup::Retained;  // the complete destination is live
      }
    }
  }
  switch (note) {
    case Backup::Blocks:
      logSystemEvent("FS", "replace of %s refused: unresolved backup %s must be inspected or removed first",
                     to.c_str(), backup.c_str());
      break;
    case Backup::Unrestored:
      logSystemEvent("FS", "replace of %s failed and its previous contents could not be restored; they remain in %s",
                     to.c_str(), backup.c_str());
      break;
    case Backup::Retained:
      logSystemEvent("FS", "replace of %s complete, but its backup %s could not be removed; later replacements are refused until it is",
                     to.c_str(), backup.c_str());
      break;
    case Backup::None:
      break;
  }
  return promoted;
}

// The staged copy's path: "<destination>.upload.tmp" beside the destination,
// or a short fixed name in the same directory when that name would be longer
// than the backend may take. LittleFS keeps the limit of whoever formatted the
// partition (64 bytes on the flashed image, 255 when the device formatted it)
// and sizes directory-entry names by CONFIG_LITTLEFS_OBJ_NAME_LEN, so the
// smaller bound applies; FAT long names stop at CONFIG_FATFS_MAX_LFN. Reusing
// the short name is safe because the caller holds the filesystem lock from
// begin() to commit() or discard(), and boot cleanup removes any ".tmp" a
// reset leaves behind. Returns an empty String when the copy came back short.
static String stagingPathFor(const String& destination) {
#ifdef CONFIG_LITTLEFS_OBJ_NAME_LEN
  constexpr size_t kLittleFsNameMax = CONFIG_LITTLEFS_OBJ_NAME_LEN - 1;
#else
  constexpr size_t kLittleFsNameMax = 63;
#endif
#ifdef CONFIG_FATFS_MAX_LFN
  constexpr size_t kFatNameMax = CONFIG_FATFS_MAX_LFN;
#else
  constexpr size_t kFatNameMax = 255;
#endif
  static const char kSuffix[] = ".upload.tmp";
  static const char kShortName[] = "~stage.upload.tmp";
  const size_t directoryLength = (size_t)(destination.lastIndexOf('/') + 1);
  const size_t nameLength = destination.length() - directoryLength;
  const size_t nameMax = getStorageType(destination) == SDCARD ? kFatNameMax : kLittleFsNameMax;
  String staged;
  size_t expected = 0;
  if (nameLength + strlen(kSuffix) <= nameMax) {
    staged = destination;
    staged.concat(kSuffix);
    expected = destination.length() + strlen(kSuffix);
  } else {
    staged = destination.substring(0, directoryLength);
    staged.concat(kShortName);
    expected = directoryLength + strlen(kShortName);
  }
  return staged.length() == expected ? staged : String();
}

bool StagedWrite::begin(const String& destination, const AuthContext& ctx) {
  if (open_) return false;
  destination_ = destination;
  temporary_ = stagingPathFor(destination);
  // No I/O on a truncated copy (a String that came back short).
  if (destination_.length() != destination.length() || temporary_.length() == 0) {
    temporary_ = "";
    return false;
  }
  // A refused temporary (permission, full filesystem, transient error) ends
  // the write with the destination untouched. The destination is never
  // opened for writing here: that open would truncate the very file this
  // staged write preserves, and a later short write would then remove it.
  file_ = openGuarded(temporary_, "w", ctx, /*create=*/true);
  if (!file_) {
    temporary_ = "";
    return false;
  }
  open_ = true;
  return true;
}

// Whether `ctx` may itself promote `from` over `to` with replaceFileGuarded:
// RENAME on the temporary and CREATE on the destination, plus DELETE for the
// backup that replacing an existing SD file leaves behind. Both paths are
// normalized.
static bool canReplaceAsCaller(const String& from, const String& to, const AuthContext& ctx) {
  if (!canRename(from, ctx) || !canCreate(to, ctx)) return false;
  return getStorageType(to) != SDCARD || !exists(to) || canDelete(to, ctx);
}

bool StagedWrite::commit(const AuthContext& ctx) {
  if (!open_) return false;
  open_ = false;
  file_.flush();
  file_.close();
  // The caller's WRITE on an existing destination (CREATE on a new one) is the
  // permission this replacement represents; RENAME and DELETE are the mechanics
  // of the staged copy. A caller the rule table lets move files here promotes
  // as itself. Otherwise the writer promotes as its system identity once that
  // write right is established, so a grant of WRITE without RENAME (such as
  // /espnow/) still saves through the staged path instead of failing here.
  String from, to;
  bool promoted = false;
  if (guardedNormalize(temporary_, from, "replace(from)", ctx) &&
      guardedNormalize(destination_, to, "replace(to)", ctx)) {
    if (canReplaceAsCaller(from, to, ctx)) {
      promoted = replaceFileGuarded(from, to, ctx);
    } else if (writePermitted(to, ctx)) {
      promoted = replaceFileGuarded(from, to, systemAuth(writer_));
    }
  }
  if (promoted) return true;
  // The previous destination is still in place; only the staged copy goes.
  removePartial(ctx);
  return false;
}

void StagedWrite::discard(const AuthContext& ctx) {
  if (!open_) return;
  open_ = false;
  file_.close();
  removePartial(ctx);
}

// Remove only what this write produced, the staged temporary: as the caller
// first, then as the writer's system identity, so a caller without DELETE on
// the directory does not leave its partial copy behind.
void StagedWrite::removePartial(const AuthContext& ctx) {
  if (!removeGuarded(temporary_, ctx)) {
    removeGuarded(temporary_, systemAuth(writer_));
  }
}

bool mkdirGuarded(const String& path, const AuthContext& ctx) {
  String norm;
  if (!guardedNormalize(path, norm, "mkdir", ctx)) return false;
  if (!canCreate(norm, ctx)) {
    logFsAccessDeny(norm, ctx, PERM_CREATE, "mkdir");
    return false;
  }
  return mkdir(norm);
}

bool rmdirGuarded(const String& path, const AuthContext& ctx) {
  String norm;
  if (!guardedNormalize(path, norm, "rmdir", ctx)) return false;
  if (!canDelete(norm, ctx)) {
    logFsAccessDeny(norm, ctx, PERM_DELETE, "rmdir");
    return false;
  }
  return rmdir(norm);
}

// ============================================================================
// systemAuth — explicit trusted-internal identity, per-call
// ============================================================================
// Constructed fresh on each call. The `reason` string is folded into the
// AuthContext.path so that any [PERM] denial (which won't happen for
// system, but the pattern stays consistent) and any future audit log
// records why this code was running as system. Cheap — three String
// assignments.
AuthContext systemAuth(const char* reason) {
  AuthContext ctx;
  ctx.transport = SOURCE_INTERNAL;
  ctx.user      = "system";
  ctx.path      = reason ? String("system:") + reason : String("system:");
  ctx.ip        = "local";
  ctx.sid       = "";
  ctx.opaque    = nullptr;
  return ctx;
}

// Scoped trusted-internal identity: base systemAuth plus a path-prefix
// confinement enforced in checkPerm. See systemAuth(reason) above + Scopes.
AuthContext systemAuth(const char* scope, const char* reason) {
  AuthContext ctx = systemAuth(reason);
  ctx.scope = scope ? String(scope) : String();
  return ctx;
}

}  // namespace VFS

// ============================================================================
// SD Card CLI Commands
// ============================================================================

static const char* cmd_sdmount(const String& argsInput) {
  EXT_RAM_BSS_ATTR static char buf[128];
  
#if !ENABLE_SD_CARD
  snprintf(buf, sizeof(buf), "ERROR: SD card support is disabled or unavailable on this board");
  return buf;
#else
  if (VFS::isSDAvailable()) {
    snprintf(buf, sizeof(buf), "SD card already mounted at /sd");
    return buf;
  }
  if (const char* owner = VFS::sdInUseOwner()) {
    snprintf(buf, sizeof(buf), "ERROR: SD card in use by %s; stop it first", owner);
    return buf;
  }
  
  if (VFS::remountSD()) {
    uint64_t total, used, free;
    if (VFS::getStats(VFS::SDCARD, total, used, free)) {
      snprintf(buf, sizeof(buf), "SD card mounted successfully at /sd\nSize: %llu MB, Used: %llu MB, Free: %llu MB",
               total / (1024*1024), used / (1024*1024), free / (1024*1024));
    } else {
      snprintf(buf, sizeof(buf), "SD card mounted successfully at /sd");
    }
  } else {
    snprintf(buf, sizeof(buf), "ERROR: Failed to mount SD card. Check if card is inserted and formatted as FAT32.");
  }
  return buf;
#endif
}

static const char* cmd_sdunmount(const String& argsInput) {
  EXT_RAM_BSS_ATTR static char buf[128];
  
#if !ENABLE_SD_CARD
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  if (const char* owner = VFS::sdInUseOwner()) {
    snprintf(buf, sizeof(buf), "ERROR: SD card in use by %s; stop it first", owner);
    return buf;
  }
  // Release a stale mount too (e.g. an accidentally removed SDMMC card).
  if (VFS::unmountSD()) {
    snprintf(buf, sizeof(buf), "SD card unmounted successfully");
  } else {
    snprintf(buf, sizeof(buf), "ERROR: Failed to unmount SD card");
  }
  return buf;
#endif
}

static const char* cmd_sdformat(const String& argsInput) {
  EXT_RAM_BSS_ATTR static char buf[256];
  
#if !ENABLE_SD_CARD
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  // Check for confirmation flag
  String confirmation = argsInput;
  confirmation.trim();
  if (confirmation != "confirm") {
    snprintf(buf, sizeof(buf), 
      "WARNING: This will ERASE ALL DATA on the SD card!\n"
      "Run 'sdformat confirm' to proceed.");
    return buf;
  }
  if (const char* owner = VFS::sdInUseOwner()) {
    snprintf(buf, sizeof(buf), "ERROR: SD card in use by %s; stop it first", owner);
    return buf;
  }
  
  snprintf(buf, sizeof(buf), "Formatting SD card as FAT32... (this may take a moment)");
  INFO_STORAGEF("%s", buf);
  
  if (VFS::formatSD()) {
    snprintf(buf, sizeof(buf), "SD card formatted successfully as FAT32 and mounted at /sd");
    logSystemEvent("SD", "SD card formatted by '%s' — ALL card data erased", currentExecUser().c_str());
    systemEventPost(SYSEVT_STORAGE_FORMATTED, "sd", "FAT32 — all card data erased");
  } else {
    snprintf(buf, sizeof(buf), "ERROR: Failed to format SD card. Ensure card is inserted properly.");
  }
  return buf;
#endif
}

static const char* cmd_sdinfo(const String& argsInput) {
  EXT_RAM_BSS_ATTR static char buf[512];
  const bool wantJson = argWantsJson(argsInput);

#if !ENABLE_SD_CARD
  if (wantJson) return "{\"schema\":1,\"supported\":false}";
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  if (!VFS::isSDAvailable()) {
    if (wantJson) return "{\"schema\":1,\"supported\":true,\"mounted\":false}";
    snprintf(buf, sizeof(buf), "Error: SD card not mounted. Use 'sdmount' to mount.");
    return buf;
  }

  uint8_t cardType = SD.cardType();
  const char* typeStr = "Unknown";
  switch (cardType) {
    case CARD_MMC:  typeStr = "MMC"; break;
    case CARD_SD:   typeStr = "SD"; break;
    case CARD_SDHC: typeStr = "SDHC"; break;
    default: break;
  }

  uint64_t total, used, free;
  bool haveStats = VFS::getStats(VFS::SDCARD, total, used, free);

  if (wantJson) {
    if (haveStats) {
      snprintf(buf, sizeof(buf),
        "{\"schema\":1,\"supported\":true,\"mounted\":true,\"type\":\"%s\","
        "\"totalMB\":%llu,\"usedMB\":%llu,\"freeMB\":%llu,\"mount\":\"/sd\"}",
        typeStr, total / (1024*1024), used / (1024*1024), free / (1024*1024));
    } else {
      snprintf(buf, sizeof(buf),
        "{\"schema\":1,\"supported\":true,\"mounted\":true,\"type\":\"%s\",\"statsAvailable\":false}",
        typeStr);
    }
    return buf;
  }

  if (haveStats) {
    snprintf(buf, sizeof(buf),
      "SD Card Info:\n"
      "  Type: %s\n"
      "  Size: %llu MB\n"
      "  Used: %llu MB\n"
      "  Free: %llu MB\n"
      "  Mount: /sd",
      typeStr, total / (1024*1024), used / (1024*1024), free / (1024*1024));
  } else {
    snprintf(buf, sizeof(buf), "SD Card Type: %s (unable to read stats)", typeStr);
  }
  return buf;
#endif
}

#if ENABLE_SD_CARD && !ENABLE_SDMMC_CARD && defined(SD_CS_PIN)
// Helper to test SD card with specific pins
static uint8_t testSDPins(int cs, int sck, int miso, int mosi, char* buf, int* pos, int maxLen) {
  *pos = appendf(buf, maxLen, *pos, "\n--- Testing CS=%d, SCK=%d, MISO=%d, MOSI=%d ---\n", cs, sck, miso, mosi);
  
  SPI.end();
  delay(50);
  SPI.begin(sck, miso, mosi, cs);
  delay(50);
  
  // Configure CS
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  delay(10);
  
  SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
  
  // Power up sequence: 74+ clocks with CS high
  digitalWrite(cs, HIGH);
  for (int i = 0; i < 16; i++) {
    SPI.transfer(0xFF);
  }
  delay(10);
  
  // Send CMD0 (GO_IDLE_STATE)
  digitalWrite(cs, LOW);
  delayMicroseconds(200);
  
  // Extra 0xFF before command
  SPI.transfer(0xFF);
  
  uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
  for (int i = 0; i < 6; i++) {
    SPI.transfer(cmd0[i]);
  }
  
  // Wait for response with more attempts
  uint8_t response = 0xFF;
  for (int i = 0; i < 64; i++) {
    response = SPI.transfer(0xFF);
    if (response != 0xFF) {
      *pos = appendf(buf, maxLen, *pos, "  Got response 0x%02X at attempt %d\n", response, i);
      break;
    }
  }
  
  digitalWrite(cs, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  
  if (response == 0xFF) {
    *pos = appendf(buf, maxLen, *pos, "  Result: NO RESPONSE (0xFF)\n");
  } else if (response == 0x01) {
    *pos = appendf(buf, maxLen, *pos, "  Result: SUCCESS! Card in idle state\n");
  } else {
    *pos = appendf(buf, maxLen, *pos, "  Result: Got 0x%02X\n", response);
  }
  
  return response;
}

#endif

// Transport-aware diagnostics; SDMMC mode never resets the C6 or LCD bus.
static const char* cmd_sddiag(const String& argsInput) {
  PSRAM_STATIC_BUF(buf, 4096);
  int pos = 0;
  
#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD
  FsLockGuard guard("VFS.sdDiag");
  snprintf(buf, buf_SIZE,
      "SDMMC slot %d, 4-bit, max %d kHz\n"
      "CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d\n"
      "Power GPIO%d (active %d), LDO%d, detect GPIO%d\n"
      "Card: %s; mount: %s; last error: %s\n"
      "Use sdunmount before removal and sdmount after insertion.",
      SD_MMC_SLOT, SD_MMC_MAX_FREQ_KHZ,
      SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN, SD_MMC_D1_PIN,
      SD_MMC_D2_PIN, SD_MMC_D3_PIN, SD_MMC_POWER_PIN,
      SD_MMC_POWER_ACTIVE_LEVEL, SD_MMC_LDO_CHANNEL, SD_MMC_DETECT_PIN,
      SD.cardPresent() ? "inserted" : "absent",
      VFS::isSDAvailable() ? "mounted" : "not mounted", SD.lastError());
  return buf;
#elif !ENABLE_SD_CARD
  snprintf(buf, buf_SIZE, "ERROR: SD card not supported on this board");
  return buf;
#else
  pos = appendf(buf, buf_SIZE, pos, "=== SD Card Diagnostics ===\n");
  pos = appendf(buf, buf_SIZE, pos, "Build config: XIAO_ESP32S3_SENSE_ENABLED=%d\n",
  #ifdef XIAO_ESP32S3_SENSE_ENABLED
    1
  #else
    0
  #endif
  );
  
  // Current pin configuration from build
  pos = appendf(buf, buf_SIZE, pos, "\nConfigured Pins (System_BuildConfig.h):\n");
  pos = appendf(buf, buf_SIZE, pos, "  CS:   GPIO%d\n", SD_CS_PIN);
  #if defined(SD_SCK_PIN)
  pos = appendf(buf, buf_SIZE, pos, "  SCK:  GPIO%d\n", SD_SCK_PIN);
  #endif
  #if defined(SD_MISO_PIN)
  pos = appendf(buf, buf_SIZE, pos, "  MISO: GPIO%d\n", SD_MISO_PIN);
  #endif
  #if defined(SD_MOSI_PIN)
  pos = appendf(buf, buf_SIZE, pos, "  MOSI: GPIO%d\n", SD_MOSI_PIN);
  #endif
  
  // Raw GPIO state check
  pos = appendf(buf, buf_SIZE, pos, "\nGPIO Pin States (raw read):\n");
  int pins[] = {3, 7, 8, 9, 10, 21};
  for (int p : pins) {
    pinMode(p, INPUT);
    pos = appendf(buf, buf_SIZE, pos, "  GPIO%d: %d\n", p, digitalRead(p));
  }
  
  // Test with CONFIGURED pins first
  pos = appendf(buf, buf_SIZE, pos, "\n=== Testing CONFIGURED pins ===");
  uint8_t r1 = testSDPins(SD_CS_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, buf, &pos, buf_SIZE);
  
  // If that failed, try alternative pin configs from Seeed docs
  if (r1 == 0xFF) {
    pos = appendf(buf, buf_SIZE, pos, "\n=== Trying ALTERNATIVE pin configs ===");
    
    // Alt 1: CS=21, SCK=7, MISO=8, MOSI=9 (some Seeed docs)
    uint8_t r2 = testSDPins(21, 7, 8, 9, buf, &pos, buf_SIZE);
    
    if (r2 == 0xFF) {
      // Alt 2: CS=21, SCK=7, MISO=8, MOSI=10
      testSDPins(21, 7, 8, 10, buf, &pos, buf_SIZE);
    }
  }
  
  pos = appendf(buf, buf_SIZE, pos, "\n\n=== Summary ===\n");
  pos = appendf(buf, buf_SIZE, pos, "SD Mount Status: %s\n", VFS::isSDAvailable() ? "Mounted" : "Not mounted");
  
  if (r1 == 0xFF) {
    pos = appendf(buf, buf_SIZE, pos, "\nTROUBLESHOOTING:\n");
    pos = appendf(buf, buf_SIZE, pos, "1. Check if J3 jumper on expansion board is connected\n");
    pos = appendf(buf, buf_SIZE, pos, "2. Try a different SD card\n");
    pos = appendf(buf, buf_SIZE, pos, "3. Reseat the expansion board\n");
    pos = appendf(buf, buf_SIZE, pos, "4. Clean SD card contacts\n");
    pos = appendf(buf, buf_SIZE, pos, "5. Check if card clicks into slot\n");
  }

  // Full report via broadcast (serial/web/file) — avoids CLI return size limits.
  broadcastMultilineReport(buf);
  return "sddiag complete (full report sent to this session's output)";
  
#endif
}

// Command registry for SD card commands
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry sdCommands[] = {
  { "sdmount", "Mount SD card", false, cmd_sdmount,
    "sdmount - Attempt to mount SD card at /sd" },
  { "sdunmount", "Unmount SD card", true, cmd_sdunmount,
    "sdunmount - Safely unmount SD card" },
  { "sdformat", "Format SD card as FAT32", true, cmd_sdformat,
    "sdformat confirm - Format SD card (WARNING: erases all data)",
    /*requiresSuperAdmin=*/true },
  { "sdinfo", "Show SD card information", false, cmd_sdinfo,
    "sdinfo - Display SD card type, size, and usage [json]" },
  { "sddiag", "SD card hardware diagnostics", false, cmd_sddiag,
    "sddiag - Show SD transport diagnostics (SPI boards also run a bus test)" },
};

const size_t sdCommandsCount = sizeof(sdCommands) / sizeof(sdCommands[0]);
// Note: SD commands registered via gCommandModules in System_CommandRegistry.cpp (not via static registrar)
// This allows conditional compilation based on ENABLE_SD_CARD for board compatibility
