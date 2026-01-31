#include "System_VFS.h"

#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_Mutex.h"

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

namespace VFS {

static bool gSdMounted = false;

static bool tryMountSD() {
#if defined(SD_CS_PIN)
  Serial.printf("[SD] Attempting mount with pins: CS=%d", SD_CS_PIN);
  #if defined(SD_SCK_PIN) && defined(SD_MISO_PIN) && defined(SD_MOSI_PIN)
  Serial.printf(", SCK=%d, MISO=%d, MOSI=%d\n", SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  #else
  Serial.println(" (using default SPI pins)");
  SPI.begin();
  #endif

  // Try different SPI frequencies
  uint32_t frequencies[] = {4000000, 1000000, 400000};
  for (uint32_t freq : frequencies) {
    Serial.printf("[SD] Trying SPI frequency: %lu Hz...\n", freq);
    if (SD.begin(SD_CS_PIN, SPI, freq, "/sd")) {
      Serial.printf("[SD] Mount SUCCESS at %lu Hz\n", freq);
      gSdMounted = true;
      return true;
    }
    Serial.println("[SD] Mount failed at this frequency");
    delay(100);
  }
  Serial.println("[SD] All mount attempts failed");
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
  if (p.length() == 0) return String("/");
  if (!p.startsWith("/")) p = String("/") + p;

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
  if (p == "/sd") return String("/");
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
    return SD.exists(stripSdPrefix(p));
  }

  if (!filesystemReady) return false;
  return LittleFS.exists(p);
}

File open(const String& path, const char* mode, bool create) {
  String p = normalize(path);
  FsLockGuard guard("VFS.open");

  if (getStorageType(p) == SDCARD) {
    if (!gSdMounted) return File();
    String sp = stripSdPrefix(p);
    return SD.open(sp.c_str(), mode, create);
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
    return SD.mkdir(stripSdPrefix(p));
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
    return SD.remove(stripSdPrefix(p));
  }

  if (!filesystemReady) return false;
  return LittleFS.remove(p);
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
    return SD.rename(stripSdPrefix(from), stripSdPrefix(to));
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
    return SD.rmdir(stripSdPrefix(p));
  }

  if (!filesystemReady) return false;
  return LittleFS.rmdir(p);
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
  // Unmount first if already mounted
  if (gSdMounted) {
    SD.end();
    gSdMounted = false;
  }
  
  // Re-initialize SPI and try mounting
  return tryMountSD();
#else
  return false;
#endif
}

// Format SD card as FAT32 using ESP-IDF low-level API
bool formatSD() {
#if defined(SD_CS_PIN)
  Serial.println("[SD FORMAT] Starting format process...");
  
  // Must unmount Arduino SD first and end SPI
  if (gSdMounted) {
    Serial.println("[SD FORMAT] Unmounting Arduino SD...");
    SD.end();
    gSdMounted = false;
  }
  SPI.end();  // End Arduino SPI so ESP-IDF can take over
  
  // Initialize the SPI bus for ESP-IDF
  Serial.printf("[SD FORMAT] Initializing SPI bus: SCK=%d, MISO=%d, MOSI=%d\n", 
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
    Serial.printf("[SD FORMAT] SPI bus init failed: 0x%x\n", ret);
    return false;
  }
  Serial.println("[SD FORMAT] SPI bus initialized");
  
  // Use ESP-IDF SDMMC/SPI host to format
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SD_CS_PIN;
  slot_config.host_id = SPI2_HOST;
  
  Serial.printf("[SD FORMAT] SD slot config: CS=%d, host=%d\n", SD_CS_PIN, SPI2_HOST);
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };
  
  sdmmc_card_t* card = nullptr;
  
  // Mount with format_if_mount_failed - this will format exFAT/unformatted cards
  Serial.println("[SD FORMAT] Attempting ESP-IDF mount...");
  ret = esp_vfs_fat_sdspi_mount("/sdformat", &host, &slot_config, &mount_config, &card);
  
  if (ret != ESP_OK) {
    Serial.printf("[SD FORMAT] ESP-IDF mount failed: 0x%x\n", ret);
    // Try explicit format
    if (card) {
      Serial.println("[SD FORMAT] Trying explicit format...");
      ret = esp_vfs_fat_sdcard_format("/sdformat", card);
    }
    if (ret != ESP_OK) {
      Serial.printf("[SD FORMAT] Format failed: 0x%x\n", ret);
      spi_bus_free(SPI2_HOST);
      return false;
    }
  } else {
    Serial.println("[SD FORMAT] ESP-IDF mount successful, formatting...");
    // Mounted successfully, now format it explicitly to ensure FAT32
    ret = esp_vfs_fat_sdcard_format("/sdformat", card);
    if (ret != ESP_OK) {
      Serial.printf("[SD FORMAT] Format failed: 0x%x\n", ret);
      esp_vfs_fat_sdcard_unmount("/sdformat", card);
      spi_bus_free(SPI2_HOST);
      return false;
    }
  }
  
  Serial.println("[SD FORMAT] Format complete, unmounting ESP-IDF...");
  // Unmount the ESP-IDF mount
  esp_vfs_fat_sdcard_unmount("/sdformat", card);
  spi_bus_free(SPI2_HOST);
  
  Serial.println("[SD FORMAT] Remounting with Arduino SD...");
  // Remount with Arduino SD library
  return tryMountSD();
#else
  return false;
#endif
}

}  // namespace VFS

// ============================================================================
// SD Card CLI Commands
// ============================================================================

static const char* cmd_sdmount(const String& cmd) {
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

static const char* cmd_sdunmount(const String& cmd) {
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

static const char* cmd_sdformat(const String& cmd) {
  static char buf[256];
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  // Check for confirmation flag
  if (cmd.indexOf("confirm") < 0) {
    snprintf(buf, sizeof(buf), 
      "WARNING: This will ERASE ALL DATA on the SD card!\n"
      "Run 'sdformat confirm' to proceed.");
    return buf;
  }
  
  snprintf(buf, sizeof(buf), "Formatting SD card as FAT32... (this may take a moment)");
  Serial.println(buf);
  Serial.flush();
  
  if (VFS::formatSD()) {
    snprintf(buf, sizeof(buf), "SD card formatted successfully as FAT32 and mounted at /sd");
  } else {
    snprintf(buf, sizeof(buf), "ERROR: Failed to format SD card. Ensure card is inserted properly.");
  }
  return buf;
#endif
}

static const char* cmd_sdinfo(const String& cmd) {
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
static const char* cmd_sddiag(const String& cmd) {
  static char buf[4096];
  int pos = 0;
  
#if !defined(SD_CS_PIN)
  snprintf(buf, sizeof(buf), "ERROR: SD card not supported on this board");
  return buf;
#else
  pos = appendf(buf, sizeof(buf), pos, "=== SD Card Diagnostics ===\n");
  pos = appendf(buf, sizeof(buf), pos, "Build config: XIAO_ESP32S3_SENSE_ENABLED=%d\n",
  #ifdef XIAO_ESP32S3_SENSE_ENABLED
    1
  #else
    0
  #endif
  );
  
  // Current pin configuration from build
  pos = appendf(buf, sizeof(buf), pos, "\nConfigured Pins (System_BuildConfig.h):\n");
  pos = appendf(buf, sizeof(buf), pos, "  CS:   GPIO%d\n", SD_CS_PIN);
  #if defined(SD_SCK_PIN)
  pos = appendf(buf, sizeof(buf), pos, "  SCK:  GPIO%d\n", SD_SCK_PIN);
  #endif
  #if defined(SD_MISO_PIN)
  pos = appendf(buf, sizeof(buf), pos, "  MISO: GPIO%d\n", SD_MISO_PIN);
  #endif
  #if defined(SD_MOSI_PIN)
  pos = appendf(buf, sizeof(buf), pos, "  MOSI: GPIO%d\n", SD_MOSI_PIN);
  #endif
  
  // Raw GPIO state check
  pos = appendf(buf, sizeof(buf), pos, "\nGPIO Pin States (raw read):\n");
  int pins[] = {3, 7, 8, 9, 10, 21};
  for (int p : pins) {
    pinMode(p, INPUT);
    pos = appendf(buf, sizeof(buf), pos, "  GPIO%d: %d\n", p, digitalRead(p));
  }
  
  // Test with CONFIGURED pins first
  pos = appendf(buf, sizeof(buf), pos, "\n=== Testing CONFIGURED pins ===");
  uint8_t r1 = testSDPins(SD_CS_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, buf, &pos, sizeof(buf));
  
  // If that failed, try alternative pin configs from Seeed docs
  if (r1 == 0xFF) {
    pos = appendf(buf, sizeof(buf), pos, "\n=== Trying ALTERNATIVE pin configs ===");
    
    // Alt 1: CS=21, SCK=7, MISO=8, MOSI=9 (some Seeed docs)
    uint8_t r2 = testSDPins(21, 7, 8, 9, buf, &pos, sizeof(buf));
    
    if (r2 == 0xFF) {
      // Alt 2: CS=21, SCK=7, MISO=8, MOSI=10
      testSDPins(21, 7, 8, 10, buf, &pos, sizeof(buf));
    }
  }
  
  pos = appendf(buf, sizeof(buf), pos, "\n\n=== Summary ===\n");
  pos = appendf(buf, sizeof(buf), pos, "SD Mount Status: %s\n", VFS::isSDAvailable() ? "Mounted" : "Not mounted");
  
  if (r1 == 0xFF) {
    pos = appendf(buf, sizeof(buf), pos, "\nTROUBLESHOOTING:\n");
    pos = appendf(buf, sizeof(buf), pos, "1. Check if J3 jumper on expansion board is connected\n");
    pos = appendf(buf, sizeof(buf), pos, "2. Try a different SD card\n");
    pos = appendf(buf, sizeof(buf), pos, "3. Reseat the expansion board\n");
    pos = appendf(buf, sizeof(buf), pos, "4. Clean SD card contacts\n");
    pos = appendf(buf, sizeof(buf), pos, "5. Check if card clicks into slot\n");
  }

  // Also stream to Serial so output isn't lost/truncated by CLI return size limits.
  Serial.println(buf);
  return "sddiag complete (see serial log output)";
  
#endif
}

// Command registry for SD card commands
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
