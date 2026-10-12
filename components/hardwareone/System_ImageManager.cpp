#include "System_ImageManager.h"
#include "System_Clock.h"   // Clock::isValidEpoch — one epoch-validity vocabulary
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Utils.h"
#include "System_Command.h"
#include "System_MemUtil.h"  // PSRAM_JSON_DOC
#include <ArduinoJson.h>
#include "System_ESPNow.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include "System_Filesystem.h"  // filesystemReady

#include <esp_heap_caps.h>
#include <stdint.h>
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

const char* ImageManager::captureFolderRejectReason(const String& folder) {
  // Mirror getCaptureFolder()'s leading-slash repair so "sd/x" and "/sd/x"
  // are judged the same way.
  const char* text = folder.c_str();
  if (*text == '/') ++text;
  if (strncmp(text, "sd", 2) == 0 && (text[2] == '\0' || text[2] == '/')) {
    return "Error: cameraCaptureFolder is a folder inside the chosen tier and must not start with /sd; "
           "use cameraStorageLocation to pick the tier";
  }
  if (folder.indexOf("..") >= 0) {
    return "Error: cameraCaptureFolder must not contain '..'";
  }
  return nullptr;
}


bool ImageManager::ensureCaptureFolder(ImageStorageLocation location) {
  const bool isSD = location == IMAGE_STORAGE_SD;
  if (!(isSD ? sdAvailable : littleFSAvailable)) return false;

  String folder = getCaptureFolder(location);
  const char* storageName = isSD ? "SD" : "LittleFS";
  AuthContext sys = VFS::systemAuth(isSD ? "imgmgr.ensure_capture_folder.sd"
                                       : "imgmgr.ensure_capture_folder");
  FsLockGuard guard("ImageManager.ensureCaptureFolder");
  if (VFS::existsGuarded(folder, sys)) return true;
  if (!VFS::mkdirGuarded(folder, sys)) {
    ERROR_STORAGEF("[ImageManager] Failed to create folder on %s: %s", storageName, folder.c_str());
    return false;
  }
  INFO_STORAGEF("[ImageManager] Created folder on %s: %s", storageName, folder.c_str());
  return true;
}

String ImageManager::generateFilename() {
  // Generate filename with timestamp if available, otherwise use counter
  char filename[64];
  time_t now = time(nullptr);
  
  if (Clock::isValidEpoch(now)) {  // was a hand-rolled year-2024 threshold
    struct tm tmLocal;
    localtime_r(&now, &tmLocal);
    snprintf(filename, sizeof(filename), "img_%04d%02d%02d_%02d%02d%02d.jpg",
             tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
             tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
  } else {
    // No valid time, use counter
    snprintf(filename, sizeof(filename), "img_%06d.jpg", imageCounter++);
  }
  
  return String(filename);
}

String ImageManager::captureAndSave(ImageStorageLocation location) {
#if ENABLE_CAMERA_SENSOR
  extern bool gCameraRunning;
  if (!gCameraRunning) {
    ERROR_CAMERAF("[ImageManager] Camera not enabled");
    return "";
  }
  
  size_t len = 0;
  uint8_t* data = captureFrame(&len);
  if (!data || len == 0) {
    ERROR_CAMERAF("[ImageManager] Failed to capture frame");
    return "";
  }
  
  String result = saveImage(data, len, location);
  free(data);
  
  return result;
#else
  ERROR_CAMERAF("[ImageManager] Camera not compiled in");
  return "";
#endif
}

// Minimum free space required before saving (100KB safety margin)
#define MIN_FREE_SPACE_BYTES (100 * 1024)

String ImageManager::saveImage(const uint8_t* data, size_t len, ImageStorageLocation location) {
  if (!data || len == 0 || len > SIZE_MAX - MIN_FREE_SPACE_BYTES) return "";

  const size_t requiredSpace = len + MIN_FREE_SPACE_BYTES;
  const String filename = generateFilename();
  auto saveTo = [&](ImageStorageLocation target) -> String {
    const bool isSD = target == IMAGE_STORAGE_SD;
    const char* storageName = isSD ? "SD" : "LittleFS";
    if (!(isSD ? sdAvailable : littleFSAvailable)) {
      ERROR_STORAGEF("[ImageManager] %s not available", storageName);
      return "";
    }

    String path = getCaptureFolder(target);
    if (!path.concat("/") || !path.concat(filename)) return "";
    String normalizedPath;
    const VFS::StorageType storage = isSD ? VFS::SDCARD : VFS::INTERNAL;
    if (!normalizeFsPath(path, normalizedPath) || VFS::getStorageType(normalizedPath) != storage) {
      ERROR_STORAGEF("[ImageManager] Invalid capture path for %s: %s (cameraCaptureFolder=\"%s\" must be a "
                     "folder inside the tier cameraStorageLocation selects)",
                     storageName, path.c_str(), gSettings.cameraCaptureFolder.c_str());
      return "";
    }
    path = normalizedPath;
    String temporary = path;
    if (temporary.length() != path.length() || !temporary.concat(".tmp")) return "";
    AuthContext sys = VFS::systemAuth(isSD ? "imgmgr.save_sd" : "imgmgr.save_image_lfs");
    FsLockGuard guard("ImageManager.saveImage");
    uint64_t total = 0, used = 0, free = 0;
    if (!VFS::getStats(storage, total, used, free)) {
      ERROR_STORAGEF("[ImageManager] %s storage statistics unavailable", storageName);
      return "";
    }
    if (free < requiredSpace) {
      ERROR_STORAGEF("[ImageManager] %s low on space: %llu free, need %u",
                     storageName, (unsigned long long)free, (unsigned)requiredSpace);
      return "";
    }
    if (!ensureCaptureFolder(target)) return "";
    if (VFS::existsGuarded(temporary, sys)) {
      ERROR_STORAGEF("[ImageManager] Temporary image already exists: %s", temporary.c_str());
      return "";
    }

    File f = VFS::openGuarded(temporary, "w", sys, /*create=*/true);
    if (!f || f.isDirectory()) return "";
    const size_t written = f.write(data, len);
    f.flush();
    // Match checked text saves: Arduino exposes no flush/close result, so
    // require every available completion signal before publishing success.
    const bool complete = written == len && !f.getWriteError() && f.size() == len;
    f.close();
    if (!isSD && written) VFS::noteLittleFsBytesWritten(written);
    if (!complete || !VFS::replaceFileGuarded(temporary, path, sys)) {
      if (!complete) {
        ERROR_STORAGEF("[ImageManager] Incomplete image write: %s (%u/%u bytes)",
                       temporary.c_str(), (unsigned)written, (unsigned)len);
      } else {
        ERROR_STORAGEF("[ImageManager] Failed to replace image: %s%s", path.c_str(),
                       isSD ? " (a recovery .bak may remain)" : "");
      }
      if (isSD) VFS::noteSDWriteFailure("image save");
      // This attempt owns only its temporary; preserve the previous image and
      // any recovery backup retained by a failed SD promotion.
      if (!VFS::removeGuarded(temporary, sys)) {
        ERROR_STORAGEF("[ImageManager] Failed to remove temporary image: %s", temporary.c_str());
      }
      return "";
    }
    INFO_STORAGEF("[ImageManager] Saved to %s: %s (%u bytes)", storageName, path.c_str(), (unsigned)len);
    return path;
  };

  String internalPath;
  String sdPath;
  if (location != IMAGE_STORAGE_SD) internalPath = saveTo(IMAGE_STORAGE_LITTLEFS);
  if (location == IMAGE_STORAGE_SD || location == IMAGE_STORAGE_BOTH) sdPath = saveTo(IMAGE_STORAGE_SD);
  const String& fullPath = internalPath.length() ? internalPath : sdPath;
  if (fullPath.length() == 0) {
    ERROR_STORAGEF("[ImageManager] Failed to save image: %s", filename.c_str());
    return "";
  }

  // Emit one event after both attempts have closed their files and released
  // their guards. BOTH retains its preferred internal path and retention policy.
  systemEventPost(SYSEVT_PHOTO_SAVED, filename.c_str(), fullPath.c_str());
  enforceMaxImages(location);
  return fullPath;
}

std::vector<ImageInfo> ImageManager::listImages(ImageStorageLocation location) {
  std::vector<ImageInfo> images;
  const bool isSD = location == IMAGE_STORAGE_SD;
  if (!(isSD ? sdAvailable : littleFSAvailable)) return images;

  String folder = getCaptureFolder(location);
  FsLockGuard guard("ImageManager.listImages");
  File dir = VFS::openGuarded(folder, "r",
                            VFS::systemAuth(isSD ? "imgmgr.list_sd" : "imgmgr.list_images"));
  if (!dir || !dir.isDirectory()) return images;

  // SD modification times determine retention order. LittleFS retains its
  // filename-based ordering and does not need the extra timestamp stat.
  VFS::DirectoryReader reader(dir, folder, /*includeModifiedTime=*/isSD);
  VFS::DirectoryEntry entry;
  while (reader.next(entry)) {
    if (entry.isFolder ||
        !(entry.name.endsWith(".jpg") || entry.name.endsWith(".jpeg"))) continue;
    ImageInfo info;
    info.filename = entry.name;
    info.fullPath = entry.path;
    info.size = entry.size;
    info.timestamp = isSD ? entry.modifiedAt : 0;
    info.location = isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
    info.isOnSD = isSD;
    images.push_back(info);
  }
  dir.close();
  return images;
}

int ImageManager::getImageCount(ImageStorageLocation location) {
  return listImages(location).size();
}

uint8_t* ImageManager::getImage(const String& path, size_t* outLen) {
  if (outLen) *outLen = 0;

  String normalizedPath;
  if (!normalizeFsPath(path, normalizedPath)) {
    WARN_STORAGEF("[ImageManager] Rejected invalid image path: %s", path.c_str());
    return nullptr;
  }

  const bool isSD = VFS::getStorageType(normalizedPath) == VFS::SDCARD;
  const ImageStorageLocation location =
      isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  String normalizedRoot;
  if (!normalizeFsPath(getCaptureFolder(location), normalizedRoot)) {
    ERROR_STORAGEF("[ImageManager] Invalid configured capture folder");
    return nullptr;
  }
  const bool underCaptureRoot =
      normalizedRoot == "/"
          ? normalizedPath.length() > 1 && normalizedPath[0] == '/'
          : normalizedPath.length() > normalizedRoot.length() &&
                normalizedPath.startsWith(normalizedRoot) &&
                normalizedPath[normalizedRoot.length()] == '/';
  if (!underCaptureRoot ||
      !(normalizedPath.endsWith(".jpg") || normalizedPath.endsWith(".jpeg"))) {
    WARN_STORAGEF("[ImageManager] Rejected path outside managed JPEG folder: %s",
                  normalizedPath.c_str());
    return nullptr;
  }

  File f;
  size_t len = 0;
  {
    FsLockGuard guard("ImageManager.getImage.open");
    if (isSD) {
      if (!sdAvailable) return nullptr;
    } else if (!littleFSAvailable) {
      return nullptr;
    }
    f = VFS::openGuarded(normalizedPath, "r",
                         VFS::systemAuth("imgmgr.get_image"));
    if (f) len = f.size();
  }
  if (!f) return nullptr;

  if (len < 2) {
    WARN_STORAGEF("[ImageManager] Rejected image length %u for %s",
                  (unsigned)len, normalizedPath.c_str());
    FsLockGuard guard("ImageManager.getImage.close_invalid_length");
    f.close();
    return nullptr;
  }

  const uint32_t imageCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (len > heap_caps_get_largest_free_block(imageCaps)) {
    WARN_STORAGEF("[ImageManager] No contiguous PSRAM block for %u-byte image",
                  (unsigned)len);
    FsLockGuard guard("ImageManager.getImage.close_no_psram_block");
    f.close();
    return nullptr;
  }

  uint8_t* buf = (uint8_t*)ps_alloc(len, AllocPref::RequirePSRAM,
                                    "image.manager.jpeg");
  if (!buf) {
    FsLockGuard guard("ImageManager.getImage.close_psram_oom");
    f.close();
    return nullptr;
  }

  size_t bytesRead = 0;
  {
    FsLockGuard guard("ImageManager.getImage.read");
    bytesRead = f.read(buf, len);
    f.close();
  }

  if (bytesRead != len || buf[0] != 0xFF || buf[1] != 0xD8) {
    WARN_STORAGEF("[ImageManager] Invalid/short JPEG read: got=%u expected=%u path=%s",
                  (unsigned)bytesRead, (unsigned)len, normalizedPath.c_str());
    ps_free(buf);
    return nullptr;
  }

  if (outLen) *outLen = len;
  return buf;
}

bool ImageManager::getImageInfo(const String& path, ImageInfo& info) {
  const bool isSD = VFS::getStorageType(path) == VFS::SDCARD;
  if (!(isSD ? sdAvailable : littleFSAvailable)) return false;

  FsLockGuard guard("ImageManager.getImageInfo");
  File f = VFS::openGuarded(path, "r", VFS::systemAuth("imgmgr.get_image_info"));
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
  const bool isSD = VFS::getStorageType(path) == VFS::SDCARD;
  if (!(isSD ? sdAvailable : littleFSAvailable)) return false;

  FsLockGuard guard("ImageManager.deleteImage");
  return VFS::removeGuarded(path, VFS::systemAuth(isSD ? "imgmgr.delete.sd"
                                                     : "imgmgr.delete_image"));
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
  const bool isSD = location == IMAGE_STORAGE_SD;
  if (!(isSD ? sdAvailable : littleFSAvailable)) return stats;

  uint64_t total = 0, used = 0, free = 0;
  if (!VFS::getStats(isSD ? VFS::SDCARD : VFS::INTERNAL, total, used, free)) return stats;
  stats.totalBytes = total;
  stats.usedBytes = used;
  stats.freeBytes = free;
  stats.imageCount = getImageCount(isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS);
  stats.available = true;
  return stats;
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_capture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  ImageStorageLocation location = (ImageStorageLocation)gSettings.cameraStorageLocation;
  
  // Check for location argument
  if (argsInput.indexOf(" sd") > 0) {
    location = IMAGE_STORAGE_SD;
  } else if (argsInput.indexOf(" littlefs") > 0 || argsInput.indexOf(" lfs") > 0) {
    location = IMAGE_STORAGE_LITTLEFS;
  } else if (argsInput.indexOf(" both") > 0) {
    location = IMAGE_STORAGE_BOTH;
  }
  
  String result = gImageManager.captureAndSave(location);
  
  if (result.length() > 0) {
    EXT_RAM_BSS_ATTR static char buf[128];
    snprintf(buf, sizeof(buf), "Captured: %s", result.c_str());
    return buf;
  }
  cliHint("if the camera is not started, run 'opencamera' first; for storage/space errors check 'sdinfo'");
  return "Error: Capture failed";
}

const char* cmd_images(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS;
  if (argsInput.indexOf(" sd") > 0) {
    location = IMAGE_STORAGE_SD;
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  int pos = 0;
  
  // Get storage stats
  StorageStats stats = gImageManager.getStorageStats(location);
  const char* locName = (location == IMAGE_STORAGE_SD) ? "SD" : "LittleFS";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["location"] = locName;
    doc["available"] = stats.available;
    if (stats.available) {
      doc["usedBytes"]  = (unsigned long)stats.usedBytes;
      doc["totalBytes"] = (unsigned long)stats.totalBytes;
      doc["imageCount"] = stats.imageCount;
      JsonArray arr = doc["images"].to<JsonArray>();
      for (const auto& img : gImageManager.listImages(location)) {
        JsonObject o = arr.add<JsonObject>();
        o["filename"] = img.filename;
        o["size"]     = (unsigned long)img.size;
      }
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (!stats.available) {
    snprintf(buf, 1024, "Error: %s not available", locName);
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
    if (location == IMAGE_STORAGE_SD) {
      pos += snprintf(buf + pos, 1024 - pos, "Hint: to take a photo, run 'capture sd'; for internal storage, run 'images'\n");
    } else {
      pos += snprintf(buf + pos, 1024 - pos, "Hint: to take a photo, run 'capture'; for SD-card photos, run 'images sd'\n");
    }
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

const char* cmd_imagedelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedToken(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: imagedelete \"<path>\"";

  if (gImageManager.deleteImage(path)) {
    return "Image deleted";
  }
  return "Error: Failed to delete image";
}

const char* cmd_imagesend(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse: <device> "<path>"  (both required — no implicit "send most recent")

  CommandArgs a(argsInput);
  if (!a.has(1)) {
    return "Error: invalid arguments — Usage: imagesend <device> \"<path>\"";
  }

  String device = a.arg(0);
  String path;
  const char* qerr = requireQuotedToken(a, 1, path);
  if (qerr) return qerr;

  // Use ESP-NOW file send (stubs return false when ESP-NOW disabled)
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(device, mac)) {
    EXT_RAM_BSS_ATTR static char errBuf[128];
    snprintf(errBuf, sizeof(errBuf), "Error: Device '%s' not found", device.c_str());
    return errBuf;
  }
  
  if (sendFileToMac(mac, path)) {
    EXT_RAM_BSS_ATTR static char buf[128];
    snprintf(buf, sizeof(buf), "Sent %s to %s", path.c_str(), device.c_str());
    cliHint("the image lands in the peer's /espnow/received/ inbox");
    return buf;
  }

  return "Error: Failed to send image (send aborted or receiver cancelled)";
}

// Command registration
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
extern const CommandEntry imageCommands[] = {
  {"capture", "Capture and save image: capture [littlefs|sd|both]", false, cmd_capture, "Usage: capture [littlefs|lfs|sd|both]"},
  {"images", "List saved images: images [littlefs|sd]", false, cmd_images, "Usage: images [sd] [json]"},
  {"imagedelete", "Delete image: imagedelete \"<path>\"", true, cmd_imagedelete, "Usage: imagedelete \"<path>\""},
  {"imagesend", "Send image via ESP-NOW: imagesend <device> \"<path>\" (synchronous send; fails if the receiver rejects/cancels)", true, cmd_imagesend, "Usage: imagesend <device> \"<path>\"\n       Blocks until the image is sent; it is written to the peer's /espnow/received/ inbox. An error result means the send aborted or the receiver cancelled."},
};

extern const size_t imageCommandsCount = sizeof(imageCommands) / sizeof(imageCommands[0]);
