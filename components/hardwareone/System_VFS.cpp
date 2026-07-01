#include "System_VFS.h"
#include <esp_attr.h>

#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Utils.h"  // argWantsJson
#include "System_Mutex.h"
#include "System_Notifications.h"

#include <LittleFS.h>
#include <SD.h>
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

static bool gSdMounted  = false;  // driver-level: SD.begin() succeeded
static bool gSdWritable = false;  // probe-verified: a write round-trip worked

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
#if defined(SD_CS_PIN)
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
#if defined(SD_CS_PIN)
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 80 clocks
  SPI.endTransaction();
  delay(5);
#endif
}

static bool tryMountSD() {
#if defined(SD_CS_PIN)
  // Three full attempts, each with a complete SPI bus reset and both
  // frequencies (fast first, slow fallback).
  uint32_t frequencies[] = {4000000, 400000};
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
        // Prove writability with a round-trip probe. If the card is
        // present but e.g. write-protected or has a bad sector where
        // SD.open lands, we want to know now, not the first time the
        // video recorder tries to create a file.
        gSdWritable = probeSDWriteInternal();
        INFO_STORAGEF("[SD] Write probe: %s", gSdWritable ? "PASS" : "FAIL");
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
// oddly (no 8.3 basename). HWPROBE.TMP is 8.3-safe and all-caps so it round-
// trips cleanly on any FAT implementation.
static bool probeSDWriteInternal() {
#if defined(SD_CS_PIN)
  if (!gSdMounted) return false;
  const char* kProbePath = "/HWPROBE.TMP";
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
  return gSdMounted;
}

// Writable check — re-probes lazily if the state was invalidated via
// noteSDWriteFailure(). This gives us "auto-recover" behaviour: if a card
// was yanked and re-inserted and a later probe succeeds, we flip back to
// writable without needing an explicit remount.
bool isSDWritable() {
  if (!gSdMounted) return false;
  if (gSdWritable) return true;
  // Not currently writable — maybe it was, maybe it is again. Try once.
  FsLockGuard guard("VFS.sdProbeLazy");
  if (probeSDWriteInternal()) {
    INFO_STORAGEF("[SD] Write probe now succeeding — card is writable again");
    gSdWritable = true;
    return true;
  }
  return false;
}

// Callers that attempt an SD write and see it fail should call this to
// invalidate the cached writable flag. Next isSDWritable() query will
// re-probe. Cheap — single bool write.
void noteSDWriteFailure(const char* hint) {
  if (gSdWritable) {
    WARN_STORAGEF("[SD] Write failure reported (%s); marking card not writable",
                  hint ? hint : "unspecified");
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

static FS* fsForPath(const String& path) {
  return (getStorageType(path) == SDCARD) ? (FS*)&SD : (FS*)&LittleFS;
}

bool exists(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.exists");

  if (getStorageType(p) == SDCARD) {
    if (!gSdMounted) return false;
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
    if (!gSdMounted) return File();
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
    if (!gSdMounted) return false;
    if (p == "/sd") return false;
    return SD.mkdir(p.c_str() + 3);
  }

  if (!filesystemReady) return false;
  return LittleFS.mkdir(p);
}

bool remove(const String& path) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return false;  // reject traversal (matches guarded-op rejection)
  FsLockGuard guard("VFS.remove");

  if (getStorageType(p) == SDCARD) {
    if (!gSdMounted) return false;
    if (p == "/sd") return false;
    bool ok = SD.remove(p.c_str() + 3);
    if (ok) notifyFileDeleted(p.c_str());
    return ok;
  }

  if (!filesystemReady) return false;
  bool ok = LittleFS.remove(p);
  if (ok) {
    notifyFileDeleted(p.c_str());
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
    if (!gSdMounted) return false;
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
    if (!gSdMounted) return false;
    if (p == "/sd") return false;
    bool ok = SD.rmdir(p.c_str() + 3);
    if (ok) notifyFileDeleted(p.c_str());
    return ok;
  }

  if (!filesystemReady) return false;
  bool ok = LittleFS.rmdir(p);
  if (ok) notifyFileDeleted(p.c_str());
  return ok;
}

bool getStats(StorageType type, uint64_t& totalBytes, uint64_t& usedBytes, uint64_t& freeBytes) {
  FsLockGuard guard("VFS.getStats");

  if (type == SDCARD) {
    if (!gSdMounted) return false;
    totalBytes = SD.totalBytes();
    usedBytes = SD.usedBytes();
    freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    return true;
  }

  if (!filesystemReady) return false;
  totalBytes = LittleFS.totalBytes();
  usedBytes = LittleFS.usedBytes();
  freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
  return true;
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

static size_t refreshLittleFsFreeCached() {
  unsigned long now = millis();
  const bool stale       = (now - gLogFreeCheckLastMs) > 2000;
  const bool writePressure = gLogFreeBytesWrittenSinceRefresh >= LOG_FREE_CACHE_BYTES_THRESHOLD;
  if (gLogFreeCheckCached == SIZE_MAX || stale || writePressure) {
    FsLockGuard guard("VFS.overflowFreeCheck");
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
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
#if defined(SD_CS_PIN)
  if (gSdMounted) {
    SD.end();
    gSdMounted = false;
    gSdWritable = false;
    return true;
  }
#endif
  return false;
}

bool remountSD() {
#if defined(SD_CS_PIN)
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
#if defined(SD_CS_PIN)
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
  if (needsWrite && !canEdit(norm, ctx)) {
    // If the path doesn't exist yet, this is a create rather than an
    // edit. Try canCreate as a fallback; the rule table author can grant
    // CREATE without WRITE for "drop-in only" directories like
    // /sd/g2_icon_animations.
    if (!exists(norm)) {
      if (!canCreate(norm, ctx)) {
        logFsAccessDeny(norm, ctx, PERM_CREATE, "create");
        return File();
      }
    } else {
      // Existing file + no edit perm = denied
      logFsAccessDeny(norm, ctx, PERM_WRITE, "edit");
      return File();
    }
  }
  return open(norm, mode, create);
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
  static char buf[128];
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board (no SD_CS_PIN defined)");
  return buf;
#else
  if (VFS::isSDAvailable()) {
    snprintf(buf, sizeof(buf), "SD card already mounted at /sd");
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
  static char buf[128];
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  if (!VFS::isSDAvailable()) {
    snprintf(buf, sizeof(buf), "Error: SD card is not mounted");
    return buf;
  }
  
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
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  // Check for confirmation flag
  if (argsInput.indexOf("confirm") < 0) {
    snprintf(buf, sizeof(buf), 
      "WARNING: This will ERASE ALL DATA on the SD card!\n"
      "Run 'sdformat confirm' to proceed.");
    return buf;
  }
  
  snprintf(buf, sizeof(buf), "Formatting SD card as FAT32... (this may take a moment)");
  INFO_STORAGEF("%s", buf);
  
  if (VFS::formatSD()) {
    snprintf(buf, sizeof(buf), "SD card formatted successfully as FAT32 and mounted at /sd");
  } else {
    snprintf(buf, sizeof(buf), "ERROR: Failed to format SD card. Ensure card is inserted properly.");
  }
  return buf;
#endif
}

static const char* cmd_sdinfo(const String& argsInput) {
  EXT_RAM_BSS_ATTR static char buf[512];
  const bool wantJson = argWantsJson(argsInput);

#if !defined(SD_CS_PIN)
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

// Raw SPI diagnostic for SD card
static const char* cmd_sddiag(const String& argsInput) {
  PSRAM_STATIC_BUF(buf, 4096);
  int pos = 0;
  
#if !defined(SD_CS_PIN)
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
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry sdCommands[] = {
  { "sdmount", "Mount SD card", false, cmd_sdmount,
    "sdmount - Attempt to mount SD card at /sd" },
  { "sdunmount", "Unmount SD card", true, cmd_sdunmount,
    "sdunmount - Safely unmount SD card" },
  { "sdformat", "Format SD card as FAT32", false, cmd_sdformat,
    "sdformat confirm - Format SD card (WARNING: erases all data)" },
  { "sdinfo", "Show SD card information", false, cmd_sdinfo,
    "sdinfo - Display SD card type, size, and usage [json]" },
  { "sddiag", "SD card hardware diagnostics", false, cmd_sddiag,
    "sddiag - Test raw SPI communication with SD card" },
};

const size_t sdCommandsCount = sizeof(sdCommands) / sizeof(sdCommands[0]);
// Note: SD commands registered via gCommandModules in System_Utils.cpp (not via static registrar)
// This allows conditional compilation based on SD_CS_PIN for board compatibility
