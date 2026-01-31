#include "System_ImageManager.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Utils.h"
#include "System_Command.h"
#include "System_ESPNow.h"
#include "System_Mutex.h"
#include "System_VFS.h"

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>

#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"
#endif

// Global instance
ImageManager gImageManager;

// SD card pins are defined in System_BuildConfig.h when available
#ifndef SD_CS_PIN
  #define SD_CS_PIN 21
#endif

extern Settings gSettings;
extern bool filesystemReady;

// ============================================================================
// ImageManager Implementation
// ============================================================================

ImageManager::ImageManager() 
  : sdAvailable(false), littleFSAvailable(false), imageCounter(0) {
}

ImageManager::~ImageManager() {
}

bool ImageManager::init() {
  littleFSAvailable = filesystemReady;
  
  if (littleFSAvailable) {
    ensureCaptureFolder(IMAGE_STORAGE_LITTLEFS);
    INFO_STORAGEF("[ImageManager] LittleFS available");
  }
  
  // Try to init SD card
  initSD();
  
  return littleFSAvailable || sdAvailable;
}

bool ImageManager::initSD() {
  // Use VFS mount instead of reinitializing SD card ourselves
  // This prevents conflicts with the VFS SD mount
  if (VFS::isSDAvailable()) {
    sdAvailable = true;
    // Don't create capture folder here - only create when actually saving an image
    // ensureCaptureFolder() is called in saveImage() when needed
    
    uint64_t totalBytes, usedBytes, freeBytes;
    if (VFS::getStats(VFS::SDCARD, totalBytes, usedBytes, freeBytes)) {
      INFO_STORAGEF("[ImageManager] SD card available via VFS, size: %lluMB", totalBytes / (1024 * 1024));
    } else {
      INFO_STORAGEF("[ImageManager] SD card available via VFS");
    }
    return true;
  } else {
    sdAvailable = false;
    DEBUG_STORAGEF("[ImageManager] SD card not available (VFS mount failed)");
    return false;
  }
}

String ImageManager::getMountPrefix(ImageStorageLocation location) {
  switch (location) {
    case IMAGE_STORAGE_SD:
      return "/sd";
    case IMAGE_STORAGE_LITTLEFS:
    default:
      return "";  // LittleFS is root
  }
}

String ImageManager::getCaptureFolder(ImageStorageLocation location) {
  String prefix = getMountPrefix(location);
  String folder = gSettings.cameraCaptureFolder;
  if (folder.length() == 0) folder = "/photos";
  if (!folder.startsWith("/")) folder = "/" + folder;
  return prefix + folder;
}

static String sdStripPrefix(const String& path) {
  if (path.startsWith("/sd")) {
    String p = path.substring(3);
    if (p.length() == 0) return "/";
    if (!p.startsWith("/")) p = "/" + p;
    return p;
  }
  return path;
}

bool ImageManager::ensureCaptureFolder(ImageStorageLocation location) {
  String folder = getCaptureFolder(location);
  
  if (location == IMAGE_STORAGE_SD) {
    if (!sdAvailable) return false;
    String sdFolder = sdStripPrefix(folder);
    if (!SD.exists(sdFolder)) {
      if (SD.mkdir(sdFolder)) {
        INFO_STORAGEF("[ImageManager] Created folder on SD: %s", folder.c_str());
        return true;
      } else {
        ERROR_STORAGEF("[ImageManager] Failed to create folder on SD: %s", folder.c_str());
        return false;
      }
    }
    return true;
  } else {
    if (!littleFSAvailable) return false;
    {
      FsLockGuard guard("ImageManager.ensureCaptureFolder.lfs");
      if (!LittleFS.exists(folder)) {
        if (LittleFS.mkdir(folder)) {
          INFO_STORAGEF("[ImageManager] Created folder on LittleFS: %s", folder.c_str());
          return true;
        } else {
          ERROR_STORAGEF("[ImageManager] Failed to create folder on LittleFS: %s", folder.c_str());
          return false;
        }
      }
      return true;
    }
  }
}

String ImageManager::generateFilename() {
  // Generate filename with timestamp if available, otherwise use counter
  char filename[64];
  time_t now = time(nullptr);
  
  if (now > 1704067200) {  // After 2024-01-01 (valid time)
    struct tm* tm = localtime(&now);
    snprintf(filename, sizeof(filename), "img_%04d%02d%02d_%02d%02d%02d.jpg",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
  } else {
    // No valid time, use counter
    snprintf(filename, sizeof(filename), "img_%06d.jpg", imageCounter++);
  }
  
  return String(filename);
}

String ImageManager::captureAndSave(ImageStorageLocation location) {
#if ENABLE_CAMERA_SENSOR
  extern bool cameraEnabled;
  if (!cameraEnabled) {
    ERROR_SENSORSF("[ImageManager] Camera not enabled");
    return "";
  }
  
  size_t len = 0;
  uint8_t* data = captureFrame(&len);
  if (!data || len == 0) {
    ERROR_SENSORSF("[ImageManager] Failed to capture frame");
    return "";
  }
  
  String result = saveImage(data, len, location);
  free(data);
  
  return result;
#else
  ERROR_SENSORSF("[ImageManager] Camera not compiled in");
  return "";
#endif
}

// Minimum free space required before saving (100KB safety margin)
#define MIN_FREE_SPACE_BYTES (100 * 1024)

String ImageManager::saveImage(const uint8_t* data, size_t len, ImageStorageLocation location) {
  if (!data || len == 0) return "";
  
  // Check for sufficient free space before saving
  size_t requiredSpace = len + MIN_FREE_SPACE_BYTES;
  if (location == IMAGE_STORAGE_SD || location == IMAGE_STORAGE_BOTH) {
    if (sdAvailable) {
      size_t freeSD = SD.totalBytes() - SD.usedBytes();
      if (freeSD < requiredSpace) {
        ERROR_STORAGEF("[ImageManager] SD card low on space: %lu free, need %lu", (unsigned long)freeSD, (unsigned long)requiredSpace);
        if (location == IMAGE_STORAGE_SD) return "";
        // For BOTH, fall through to try LittleFS
      }
    }
  }
  if (location == IMAGE_STORAGE_LITTLEFS || location == IMAGE_STORAGE_BOTH) {
    if (littleFSAvailable) {
      size_t total = 0, used = 0;
      {
        FsLockGuard guard("ImageManager.saveImage.stats");
        total = LittleFS.totalBytes();
        used = LittleFS.usedBytes();
      }
      size_t freeLFS = (total > used) ? (total - used) : 0;
      if (freeLFS < requiredSpace) {
        ERROR_STORAGEF("[ImageManager] LittleFS low on space: %lu free, need %lu", (unsigned long)freeLFS, (unsigned long)requiredSpace);
        if (location == IMAGE_STORAGE_LITTLEFS) return "";
      }
    }
  }
  
  String filename = generateFilename();
  String folder = getCaptureFolder(location);
  String fullPath = folder + "/" + filename;
  
  bool success = false;
  
  // Handle saving to both locations
  if (location == IMAGE_STORAGE_BOTH) {
    bool savedLittleFS = false;
    bool savedSD = false;
    
    if (littleFSAvailable) {
      ensureCaptureFolder(IMAGE_STORAGE_LITTLEFS);
      String lfsPath = getCaptureFolder(IMAGE_STORAGE_LITTLEFS) + "/" + filename;
      {
        FsLockGuard guard("ImageManager.saveImage.lfs_both");
        File f = LittleFS.open(lfsPath, "w");
        if (f) {
          f.write(data, len);
          f.close();
          savedLittleFS = true;
          INFO_STORAGEF("[ImageManager] Saved to LittleFS: %s (%u bytes)", lfsPath.c_str(), len);
        }
      }
    }
    
    if (sdAvailable) {
      ensureCaptureFolder(IMAGE_STORAGE_SD);
      String sdDisplayPath = getCaptureFolder(IMAGE_STORAGE_SD) + "/" + filename;
      String sdFsPath = sdStripPrefix(sdDisplayPath);
      File f = SD.open(sdFsPath, FILE_WRITE);
      if (f) {
        f.write(data, len);
        f.close();
        savedSD = true;
        INFO_STORAGEF("[ImageManager] Saved to SD: %s (%u bytes)", sdDisplayPath.c_str(), len);
      }
    }
    
    success = savedLittleFS || savedSD;
    if (savedLittleFS) {
      fullPath = getCaptureFolder(IMAGE_STORAGE_LITTLEFS) + "/" + filename;
    } else if (savedSD) {
      fullPath = getCaptureFolder(IMAGE_STORAGE_SD) + "/" + filename;
    }
  } else if (location == IMAGE_STORAGE_SD) {
    if (!sdAvailable) {
      ERROR_STORAGEF("[ImageManager] SD card not available");
      return "";
    }
    ensureCaptureFolder(IMAGE_STORAGE_SD);
    String sdFsPath = sdStripPrefix(fullPath);
    File f = SD.open(sdFsPath, FILE_WRITE);
    if (f) {
      f.write(data, len);
      f.close();
      success = true;
      INFO_STORAGEF("[ImageManager] Saved to SD: %s (%u bytes)", fullPath.c_str(), len);
    }
  } else {
    if (!littleFSAvailable) {
      ERROR_STORAGEF("[ImageManager] LittleFS not available");
      return "";
    }
    ensureCaptureFolder(IMAGE_STORAGE_LITTLEFS);
    {
      FsLockGuard guard("ImageManager.saveImage.lfs");
      File f = LittleFS.open(fullPath, "w");
      if (f) {
        f.write(data, len);
        f.close();
        success = true;
        INFO_STORAGEF("[ImageManager] Saved to LittleFS: %s (%u bytes)", fullPath.c_str(), len);
      }
    }
  }
  
  if (!success) {
    ERROR_STORAGEF("[ImageManager] Failed to save image: %s", fullPath.c_str());
    return "";
  }
  
  // Enforce max images limit
  enforceMaxImages(location);
  
  return fullPath;
}

std::vector<ImageInfo> ImageManager::listImages(ImageStorageLocation location) {
  std::vector<ImageInfo> images;
  String folder = getCaptureFolder(location);
  
  if (location == IMAGE_STORAGE_SD) {
    if (!sdAvailable) return images;
    
    File dir = SD.open(sdStripPrefix(folder));
    if (!dir || !dir.isDirectory()) return images;
    
    File file = dir.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String name = String(file.name());
        int lastSlash = name.lastIndexOf('/');
        if (lastSlash >= 0) name = name.substring(lastSlash + 1);
        if (name.endsWith(".jpg") || name.endsWith(".jpeg")) {
          ImageInfo info;
          info.filename = name;
          info.fullPath = folder + "/" + name;
          info.size = file.size();
          info.timestamp = file.getLastWrite();
          info.location = IMAGE_STORAGE_SD;
          info.isOnSD = true;
          images.push_back(info);
        }
      }
      file = dir.openNextFile();
    }
    dir.close();
  } else {
    if (!littleFSAvailable) return images;

    FsLockGuard guard("ImageManager.listImages.lfs");
    File dir = LittleFS.open(folder);
    if (!dir || !dir.isDirectory()) return images;
    
    File file = dir.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String name = String(file.name());
        // Strip path if present
        int lastSlash = name.lastIndexOf('/');
        if (lastSlash >= 0) name = name.substring(lastSlash + 1);
        
        if (name.endsWith(".jpg") || name.endsWith(".jpeg")) {
          ImageInfo info;
          info.filename = name;
          info.fullPath = folder + "/" + name;
          info.size = file.size();
          info.timestamp = 0;  // LittleFS doesn't track timestamps well
          info.location = IMAGE_STORAGE_LITTLEFS;
          info.isOnSD = false;
          images.push_back(info);
        }
      }
      file = dir.openNextFile();
    }
    dir.close();
  }
  
  return images;
}

int ImageManager::getImageCount(ImageStorageLocation location) {
  return listImages(location).size();
}

uint8_t* ImageManager::getImage(const String& path, size_t* outLen) {
  if (outLen) *outLen = 0;
  
  File f;
  bool isSD = path.startsWith("/sd");
  
  if (isSD) {
    if (!sdAvailable) return nullptr;
    f = SD.open(path.substring(3));  // Remove /sd prefix
  } else {
    if (!littleFSAvailable) return nullptr;
    FsLockGuard guard("ImageManager.getImage.open");
    f = LittleFS.open(path);
  }
  
  if (!f) return nullptr;
  
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) {
    f.close();
    return nullptr;
  }
  
  if (isSD) {
    f.read(buf, len);
    f.close();
  } else {
    FsLockGuard guard("ImageManager.getImage.read");
    f.read(buf, len);
    f.close();
  }
  
  if (outLen) *outLen = len;
  return buf;
}

bool ImageManager::getImageInfo(const String& path, ImageInfo& info) {
  bool isSD = path.startsWith("/sd");
  File f;
  
  if (isSD) {
    if (!sdAvailable) return false;
    f = SD.open(path.substring(3));
  } else {
    if (!littleFSAvailable) return false;
    FsLockGuard guard("ImageManager.getImageInfo.open");
    f = LittleFS.open(path);
  }
  
  if (!f) return false;
  
  info.fullPath = path;
  info.filename = path.substring(path.lastIndexOf('/') + 1);
  info.size = f.size();
  info.timestamp = isSD ? f.getLastWrite() : 0;
  info.location = isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  info.isOnSD = isSD;
  
  f.close();
  return true;
}

bool ImageManager::deleteImage(const String& path) {
  bool isSD = path.startsWith("/sd");
  
  if (isSD) {
    if (!sdAvailable) return false;
    return SD.remove(path.substring(3));
  } else {
    if (!littleFSAvailable) return false;
    FsLockGuard guard("ImageManager.deleteImage");
    return LittleFS.remove(path);
  }
}

bool ImageManager::deleteOldestImages(int count, ImageStorageLocation location) {
  std::vector<ImageInfo> images = listImages(location);
  if (images.empty() || count <= 0) return true;
  
  // Sort by timestamp (oldest first) - for SD
  // For LittleFS, sort by filename which should be chronological
  std::sort(images.begin(), images.end(), [](const ImageInfo& a, const ImageInfo& b) {
    if (a.timestamp != 0 && b.timestamp != 0) {
      return a.timestamp < b.timestamp;
    }
    return a.filename < b.filename;
  });
  
  int deleted = 0;
  for (int i = 0; i < count && i < (int)images.size(); i++) {
    if (deleteImage(images[i].fullPath)) {
      deleted++;
      INFO_STORAGEF("[ImageManager] Deleted old image: %s", images[i].fullPath.c_str());
    }
  }
  
  return deleted > 0;
}

bool ImageManager::enforceMaxImages(ImageStorageLocation location) {
  int maxImages = gSettings.cameraMaxStoredImages;
  if (maxImages <= 0) return true;  // Unlimited
  
  int currentCount = getImageCount(location);
  if (currentCount > maxImages) {
    int toDelete = currentCount - maxImages;
    return deleteOldestImages(toDelete, location);
  }
  
  return true;
}

StorageStats ImageManager::getStorageStats(ImageStorageLocation location) {
  StorageStats stats = {0, 0, 0, 0, false};
  
  if (location == IMAGE_STORAGE_SD) {
    if (!sdAvailable) return stats;
    stats.totalBytes = SD.totalBytes();
    stats.usedBytes = SD.usedBytes();
    stats.freeBytes = stats.totalBytes - stats.usedBytes;
    stats.imageCount = getImageCount(IMAGE_STORAGE_SD);
    stats.available = true;
  } else {
    if (!littleFSAvailable) return stats;
    {
      FsLockGuard guard("ImageManager.getStorageStats.lfs");
      stats.totalBytes = LittleFS.totalBytes();
      stats.usedBytes = LittleFS.usedBytes();
    }
    stats.freeBytes = stats.totalBytes - stats.usedBytes;
    stats.imageCount = getImageCount(IMAGE_STORAGE_LITTLEFS);
    stats.available = true;
  }
  
  return stats;
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_capture(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  ImageStorageLocation location = (ImageStorageLocation)gSettings.cameraStorageLocation;
  
  // Check for location argument
  if (cmd.indexOf(" sd") > 0) {
    location = IMAGE_STORAGE_SD;
  } else if (cmd.indexOf(" littlefs") > 0 || cmd.indexOf(" lfs") > 0) {
    location = IMAGE_STORAGE_LITTLEFS;
  } else if (cmd.indexOf(" both") > 0) {
    location = IMAGE_STORAGE_BOTH;
  }
  
  String result = gImageManager.captureAndSave(location);
  
  if (result.length() > 0) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "Captured: %s", result.c_str());
    return buf;
  }
  return "Capture failed";
}

const char* cmd_images(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS;
  if (cmd.indexOf(" sd") > 0) {
    location = IMAGE_STORAGE_SD;
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  int pos = 0;
  
  // Get storage stats
  StorageStats stats = gImageManager.getStorageStats(location);
  const char* locName = (location == IMAGE_STORAGE_SD) ? "SD" : "LittleFS";
  
  if (!stats.available) {
    snprintf(buf, 1024, "%s not available", locName);
    return buf;
  }
  
  pos += snprintf(buf + pos, 1024 - pos, "=== Images on %s ===\n", locName);
  pos += snprintf(buf + pos, 1024 - pos, "Storage: %lu/%lu KB (%d images)\n\n",
                  (unsigned long)(stats.usedBytes / 1024),
                  (unsigned long)(stats.totalBytes / 1024),
                  stats.imageCount);
  
  std::vector<ImageInfo> images = gImageManager.listImages(location);
  
  if (images.empty()) {
    pos += snprintf(buf + pos, 1024 - pos, "(no images)\n");
  } else {
    for (const auto& img : images) {
      pos += snprintf(buf + pos, 1024 - pos, "  %s (%lu bytes)\n",
                      img.filename.c_str(), (unsigned long)img.size);
      if (pos >= 900) {
        pos += snprintf(buf + pos, 1024 - pos, "  ... (%d more)\n", (int)(images.size() - (&img - &images[0])));
        break;
      }
    }
  }
  
  return buf;
}

const char* cmd_imageview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String path = args;
  path.trim();
  
  if (path.length() == 0) {
    return "Usage: imageview <path>";
  }
  
  ImageInfo info;
  if (!gImageManager.getImageInfo(path, info)) {
    return "Image not found";
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  snprintf(buf, 1024, 
           "File: %s\n"
           "Path: %s\n"
           "Size: %lu bytes\n"
           "Location: %s\n",
           info.filename.c_str(),
           info.fullPath.c_str(),
           (unsigned long)info.size,
           info.isOnSD ? "SD Card" : "LittleFS");
  
  return buf;
}

const char* cmd_imagedelete(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String path = args;
  path.trim();
  
  if (path.length() == 0) {
    return "Usage: imagedelete <path>";
  }
  
  if (gImageManager.deleteImage(path)) {
    return "Image deleted";
  }
  return "Failed to delete image";
}

const char* cmd_imagesend(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse: <device> <path>
  // Or: <device> (sends most recent image)
  
  String rest = args;
  rest.trim();
  
  int firstSpace = rest.indexOf(' ');
  if (rest.length() == 0) {
    return "Usage: imagesend <device> [path]";
  }
  
  String device, path;
  
  if (firstSpace < 0) {
    device = rest;
    device.trim();
    // Get most recent image
    std::vector<ImageInfo> images = gImageManager.listImages(IMAGE_STORAGE_LITTLEFS);
    if (images.empty()) {
      return "No images to send";
    }
    path = images.back().fullPath;
  } else {
    device = rest.substring(0, firstSpace);
    path = rest.substring(firstSpace + 1);
    device.trim();
    path.trim();
  }
  
  // Use ESP-NOW file send (stubs return false when ESPNOW disabled)
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(device, mac)) {
    static char errBuf[128];
    snprintf(errBuf, sizeof(errBuf), "Device '%s' not found", device.c_str());
    return errBuf;
  }
  
  if (sendFileToMac(mac, path)) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "Sending %s to %s", path.c_str(), device.c_str());
    return buf;
  }
  
  return "Failed to send image";
}

// Command registration
static const CommandEntry imageCommands[] = {
  {"capture", "Capture and save image: capture [littlefs|sd|both]", false, cmd_capture},
  {"images", "List saved images: images [littlefs|sd]", false, cmd_images},
  {"imageview", "View image info: imageview <path>", false, cmd_imageview},
  {"imagedelete", "Delete image: imagedelete <path>", true, cmd_imagedelete},
  {"imagesend", "Send image via ESP-NOW: imagesend <device> [path]", false, cmd_imagesend},
};

static const size_t imageCommandsCount = sizeof(imageCommands) / sizeof(imageCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _image_cmd_registrar(imageCommands, imageCommandsCount, "image");
