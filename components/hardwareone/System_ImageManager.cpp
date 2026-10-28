#include "System_ImageManager.h"
#include "System_Clock.h"   // Clock::isValidEpoch — one epoch-validity vocabulary
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_DebugJson.h"  // serializeJsonListToDebugBuffer
#include "System_Utils.h"
#include "System_Command.h"
#include "System_AuthIdentity.h"  // currentAuthContext — imagedelete removes as its caller
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
  : littleFSAvailable(false), imageCounter(0) {
}

ImageManager::~ImageManager() {
}

bool ImageManager::init() {
  littleFSAvailable = filesystemReady;
  lfsStaleSwept = false;  // a fresh mount gets one stale-temporary sweep

  if (littleFSAvailable) {
    ensureCaptureFolder(IMAGE_STORAGE_LITTLEFS);
    INFO_STORAGEF("[ImageManager] LittleFS available");
  }
  
  // Report the boot card; SD availability is asked of VFS per operation.
  const bool sd = initSD();

  return littleFSAvailable || sd;
}

bool ImageManager::initSD() {
  // VFS owns the SD mount; this only reports it. The capture folder is made,
  // and swept once per mount, when a save first prepares it.
  if (VFS::isSDAvailable()) {
    uint64_t totalBytes, usedBytes, freeBytes;
    if (VFS::getStats(VFS::SDCARD, totalBytes, usedBytes, freeBytes)) {
      INFO_STORAGEF("[ImageManager] SD card available via VFS, size: %lluMB", totalBytes / (1024 * 1024));
    } else {
      INFO_STORAGEF("[ImageManager] SD card available via VFS");
    }
    return true;
  } else {
    DEBUG_STORAGEF("[ImageManager] SD card not available (VFS mount failed)");
    return false;
  }
}

bool ImageManager::isSDAvailable() const { return VFS::isSDAvailable(); }

bool ImageManager::tierAvailable(bool isSD) const {
  return isSD ? VFS::isSDAvailable() : littleFSAvailable;
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
  if (!tierAvailable(isSD)) return false;

  String folder = getCaptureFolder(location);
  const char* storageName = isSD ? "SD" : "LittleFS";
  // trusted: confined to the configured capture folder of this tier.
  AuthContext sys = VFS::systemAuth(folder.c_str(), isSD ? "imgmgr.ensure_capture_folder.sd"
                                                        : "imgmgr.ensure_capture_folder");
  FsLockGuard guard("ImageManager.ensureCaptureFolder");
  if (!VFS::existsGuarded(folder, sys)) {
    if (!VFS::mkdirGuarded(folder, sys)) {
      ERROR_STORAGEF("[ImageManager] Failed to create folder on %s: %s", storageName, folder.c_str());
      return false;
    }
    INFO_STORAGEF("[ImageManager] Created folder on %s: %s", storageName, folder.c_str());
  }
  // Lazily, once per tier per mount, under the same lock: reclaim the
  // `.jpg.tmp` a reset mid-save leaves behind, which listings skip and
  // retention never sees. A card mounted after boot, or remounted, carries a
  // new VFS mount generation and gets its own sweep.
  if (isSD) {
    const uint32_t mount = VFS::sdMountGeneration();
    if (sdSweptGeneration != mount) {
      sdSweptGeneration = mount;
      sweepStaleTemporaries(location, folder);
    }
  } else if (!lfsStaleSwept) {
    lfsStaleSwept = true;
    sweepStaleTemporaries(location, folder);
  }
  return true;
}

// Only this owner writes `<image>.jpg.tmp` names, and it holds the filesystem
// lock while it does, so under the caller's lock every such entry is stale.
// Entries are collected first; the directory is closed before anything is
// removed.
void ImageManager::sweepStaleTemporaries(ImageStorageLocation location, const String& folder) {
  const bool isSD = location == IMAGE_STORAGE_SD;
  const AuthContext sys = VFS::systemAuth(folder.c_str(), isSD ? "imgmgr.sweep_stale.sd" : "imgmgr.sweep_stale");
  File dir = VFS::openGuarded(folder, "r", sys);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  std::vector<String> stale;
  VFS::DirectoryReader reader(dir, folder);
  VFS::DirectoryEntry entry;
  while (reader.next(entry)) {
    if (entry.isFolder || entry.isVirtual) continue;
    if (entry.name.endsWith(".jpg.tmp") || entry.name.endsWith(".jpeg.tmp")) stale.push_back(entry.path);
  }
  dir.close();
  unsigned removed = 0;
  for (const String& path : stale) {
    if (VFS::removeGuarded(path, sys)) ++removed;
    else WARN_STORAGEF("[ImageManager] Could not remove stale photo temporary %s", path.c_str());
  }
  if (removed) {
    INFO_STORAGEF("[ImageManager] Removed %u stale photo temporar%s on %s", removed,
                  removed == 1 ? "y" : "ies", isSD ? "SD" : "LittleFS");
  }
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
    if (!tierAvailable(isSD)) {
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
    // trusted: the file and its ".tmp" sibling sit in this tier's capture folder.
    AuthContext sys = VFS::systemAuth(getCaptureFolder(target).c_str(),
                                      isSD ? "imgmgr.save_sd" : "imgmgr.save_image_lfs");
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
  if (!tierAvailable(isSD)) return images;

  String folder = getCaptureFolder(location);
  FsLockGuard guard("ImageManager.listImages");
  File dir = VFS::openGuarded(folder, "r",
                            VFS::systemAuth(folder.c_str(), isSD ? "imgmgr.list_sd" : "imgmgr.list_images"));
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

bool ImageManager::managedImagePath(const String& path, String& normalizedPath,
                                    bool& isSD) {
  if (!normalizeFsPath(path, normalizedPath)) {
    WARN_STORAGEF("[ImageManager] Rejected invalid image path: %s", path.c_str());
    return false;
  }

  isSD = VFS::getStorageType(normalizedPath) == VFS::SDCARD;
  const ImageStorageLocation location =
      isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS;
  String normalizedRoot;
  if (!normalizeFsPath(getCaptureFolder(location), normalizedRoot)) {
    ERROR_STORAGEF("[ImageManager] Invalid configured capture folder");
    return false;
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
    return false;
  }
  return true;
}

uint8_t* ImageManager::getImage(const String& path, size_t* outLen) {
  if (outLen) *outLen = 0;

  String normalizedPath;
  bool isSD = false;
  if (!managedImagePath(path, normalizedPath, isSD)) return nullptr;

  File f;
  size_t len = 0;
  {
    FsLockGuard guard("ImageManager.getImage.open");
    if (!tierAvailable(isSD)) return nullptr;
    f = VFS::openGuarded(normalizedPath, "r",
                         VFS::systemAuth(getCaptureFolder(isSD ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS).c_str(),
                                         "imgmgr.get_image"));
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

bool ImageManager::deleteImage(const String& path, const AuthContext& ctx) {
  String normalizedPath;
  bool isSD = false;
  if (!managedImagePath(path, normalizedPath, isSD)) return false;
  if (!tierAvailable(isSD)) return false;

  FsLockGuard guard("ImageManager.deleteImage");
  return VFS::removeGuarded(normalizedPath, ctx);
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
  
  // trusted: retention removes only paths listImages produced inside the
  // capture folder, so no caller identity exists to confine it further.
  const AuthContext retention = VFS::systemAuth(
      getCaptureFolder(location).c_str(),
      location == IMAGE_STORAGE_SD ? "imgmgr.delete.sd" : "imgmgr.delete_image");
  int deleted = 0;
  for (int i = 0; i < count && i < (int)images.size(); i++) {
    if (deleteImage(images[i].fullPath, retention)) {
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
  if (!tierAvailable(isSD)) return stats;

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

  // One optional tier token; without it cameraStorageLocation decides.
  CommandArgs a(argsInput);
  if (a.has(1)) return "Error: unexpected argument — usage: capture [littlefs|lfs|sd|both]";
  if (a.has(0)) {
    const String& tier = a.arg(0);
    if (tier.equalsIgnoreCase("sd")) {
      location = IMAGE_STORAGE_SD;
    } else if (tier.equalsIgnoreCase("littlefs") || tier.equalsIgnoreCase("lfs")) {
      location = IMAGE_STORAGE_LITTLEFS;
    } else if (tier.equalsIgnoreCase("both")) {
      location = IMAGE_STORAGE_BOTH;
    } else {
      return "Error: unknown storage — usage: capture [littlefs|lfs|sd|both]";
    }
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
  
  // An optional tier (littlefs|lfs|sd) and an optional json, in either order.
  ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS;
  bool wantJson = false, tierGiven = false;
  CommandArgs a(argsInput);
  for (int i = 0; i < a.count(); ++i) {
    const String& token = a.arg(i);
    if (!wantJson && token.equalsIgnoreCase("json")) {
      wantJson = true;
    } else if (!tierGiven && token.equalsIgnoreCase("sd")) {
      location = IMAGE_STORAGE_SD;
      tierGiven = true;
    } else if (!tierGiven && (token.equalsIgnoreCase("littlefs") || token.equalsIgnoreCase("lfs"))) {
      tierGiven = true;
    } else {
      return "Error: unexpected argument — usage: images [littlefs|lfs|sd] [json]";
    }
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  int pos = 0;
  
  // Get storage stats
  StorageStats stats = gImageManager.getStorageStats(location);
  const char* locName = (location == IMAGE_STORAGE_SD) ? "SD" : "LittleFS";

  if (wantJson) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["location"] = locName;
    doc["available"] = stats.available;
    JsonArray arr;
    if (stats.available) {
      doc["usedBytes"]  = (unsigned long)stats.usedBytes;
      doc["totalBytes"] = (unsigned long)stats.totalBytes;
      doc["imageCount"] = stats.imageCount;
      arr = doc["images"].to<JsonArray>();
      for (const auto& img : gImageManager.listImages(location)) {
        JsonObject o = arr.add<JsonObject>();
        o["filename"] = img.filename;
        o["size"]     = (unsigned long)img.size;
      }
    }
    // More photos than the buffer holds: the last-listed entries are dropped
    // and the reply says "truncated"; imageCount still gives the total.
    const char* reply = serializeJsonListToDebugBuffer(doc, arr);
    return reply ? reply : "{\"schema\":1,\"error\":\"images_json_too_large\"}";
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

  if (gImageManager.deleteImage(path, currentAuthContext())) {
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
