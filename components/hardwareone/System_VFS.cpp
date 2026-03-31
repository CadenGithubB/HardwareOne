#include "System_VFS.h"

#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
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

static bool gSdMounted = false;

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
  return false;
}

bool init() {
  // LittleFS is initialized in initFilesystem(). We only mount SD here.
  return tryMountSD();
}

bool isLittleFSReady() {
  return filesystemReady;
}

bool isSDAvailable() {
  return gSdMounted;
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

static FS* fsForPath(const String& path) {
  return (getStorageType(path) == SDCARD) ? (FS*)&SD : (FS*)&LittleFS;
}

bool exists(const String& path) {
  String p = normalize(path);
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
  if (ok) notifyFileDeleted(p.c_str());
  return ok;
}

bool rename(const String& pathFrom, const String& pathTo) {
  String from = normalize(pathFrom);
  String to = normalize(pathTo);

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
  return LittleFS.rename(from, to);
}

bool rmdir(const String& path) {
  String p = normalize(path);
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

bool unmountSD() {
#if defined(SD_CS_PIN)
  if (gSdMounted) {
    SD.end();
    gSdMounted = false;
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
    snprintf(buf, sizeof(buf), "SD card is not mounted");
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
  static char buf[256];
  
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
  static char buf[512];
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  if (!VFS::isSDAvailable()) {
    snprintf(buf, sizeof(buf), "SD card not mounted. Use 'sdmount' to mount.");
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
  if (VFS::getStats(VFS::SDCARD, total, used, free)) {
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
  return "sddiag complete (see serial log output)";
  
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
    "sdinfo - Display SD card type, size, and usage" },
  { "sddiag", "SD card hardware diagnostics", false, cmd_sddiag,
    "sddiag - Test raw SPI communication with SD card" },
};

const size_t sdCommandsCount = sizeof(sdCommands) / sizeof(sdCommands[0]);
// Note: SD commands registered via gCommandModules in System_Utils.cpp (not via static registrar)
// This allows conditional compilation based on SD_CS_PIN for board compatibility
