#ifndef SYSTEM_IMAGEMANAGER_H
#define SYSTEM_IMAGEMANAGER_H

#include <Arduino.h>
#include <vector>
#include "System_BuildConfig.h"

struct AuthContext;  // System_User.h; deleteImage removes as the identity it is given

// Storage location enum
enum ImageStorageLocation {
  IMAGE_STORAGE_LITTLEFS = 0,
  IMAGE_STORAGE_SD = 1,
  IMAGE_STORAGE_BOTH = 2
};

// Image metadata structure
struct ImageInfo {
  String filename;          // Just the filename (e.g., "img_001.jpg")
  String fullPath;          // Full path (e.g., "/photos/img_001.jpg")
  size_t size;              // File size in bytes
  time_t timestamp;         // Capture timestamp (0 if unknown)
  ImageStorageLocation location;  // Where the image is stored
  bool isOnSd;              // True if on SD card
};

// Storage stats. 64-bit: a card above 4 GB does not fit size_t on this target.
struct StorageStats {
  uint64_t totalBytes;
  uint64_t usedBytes;
  uint64_t freeBytes;
  int imageCount;
  bool available;
};

// Why a save took no photo, or wrote no copy on one tier.
enum class ImageSaveFailure : uint8_t {
  None,               // copy written, or tier not asked
  CameraOff,
  NoCamera,           // camera support is not in this build
  NoFrame,
  TierUnavailable,
  FolderOutsideTier,  // cameraCaptureFolder does not lie inside the tier
  LowSpace,
  WriteFailed,
};

// What a save did, for the reply of the command that asked for it.
struct ImageSaveReport {
  ImageSaveFailure capture = ImageSaveFailure::None;   // no photo to save
  ImageSaveFailure internal = ImageSaveFailure::None;  // the internal copy
  ImageSaveFailure sd = ImageSaveFailure::None;        // the SD copy

  // The complete reply for a save that wrote no copy.
  const char* failureReply() const;
  // For a save that returned a path: true when it wrote one copy where two
  // were asked, with the tier of the missing copy and a few words on why.
  bool missingCopy(const char*& tier, const char*& reason) const;
};

inline const char* ImageSaveReport::failureReply() const {
  switch (capture) {
    case ImageSaveFailure::CameraOff: return "Error: Camera not enabled - run opencamera first";
    case ImageSaveFailure::NoCamera:  return "Error: Photo not saved: this firmware build has no camera support.";
    case ImageSaveFailure::NoFrame:
      return "Error: Photo not saved: no frame from the camera. Check cameraread and retry.";
    default: break;
  }
  const bool onSd = internal == ImageSaveFailure::None;
  switch (onSd ? sd : internal) {
    case ImageSaveFailure::TierUnavailable:
      return onSd ? "Error: Photo not saved: SD card not available. Run sdmount or change camerastoragelocation."
                  : "Error: Photo not saved: internal storage not available. Check fsusage, or save to the card with capture sd.";
    case ImageSaveFailure::FolderOutsideTier:
      return "Error: Photo not saved: cameraCaptureFolder is not inside the selected storage. "
             "Set it with cameracapturefolder.";
    case ImageSaveFailure::LowSpace:
      return "Error: Photo not saved: storage is low on space. Delete photos with imagedelete and retry.";
    default:
      return onSd ? "Error: Photo not saved: write failed. Check the card with sdinfo."
                  : "Error: Photo not saved: write failed. Check the storage with fsusage.";
  }
}

inline bool ImageSaveReport::missingCopy(const char*& tier, const char*& reason) const {
  const bool sdMissing = sd != ImageSaveFailure::None;
  if (sdMissing == (internal != ImageSaveFailure::None)) return false;
  tier = sdMissing ? "SD" : "internal";
  switch (sdMissing ? sd : internal) {
    case ImageSaveFailure::TierUnavailable:   reason = sdMissing ? "card not available" : "storage not available"; break;
    case ImageSaveFailure::FolderOutsideTier: reason = "capture folder is not inside it"; break;
    case ImageSaveFailure::LowSpace:          reason = "low on space"; break;
    default:                                  reason = "write failed"; break;
  }
  return true;
}

class ImageManager {
public:
  ImageManager();
  ~ImageManager();
  
  // Initialization
  bool init();
  // VFS owns the SD mount: these ask it on every call, so a card mounted
  // (sdmount, sdformat) or removed after boot is seen without a reboot.
  bool initSd();  // logs the card VFS has mounted; true when one is
  bool isSdAvailable() const;
  bool isLittleFsAvailable() const { return littleFsAvailable; }
  
  // Capture and save
  // Returns the saved image's full path, or empty string on failure.
  // `report`, when given, says why nothing or only one of two copies was saved.
  String captureAndSave(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS,
                        ImageSaveReport* report = nullptr);
  
  // Save directly to generated paths; failed-write cleanup is best-effort.
  // BOTH tries each tier independently, succeeds if either completes, and
  // prefers the internal path when both complete. Empty means neither saved.
  String saveImage(const uint8_t* data, size_t len, ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS,
                   ImageSaveReport* report = nullptr);

  // cameraCaptureFolder names a folder inside the tier that cameraStorageLocation
  // selects, so it must not carry a tier prefix itself or step outside it.
  // Returns nullptr when `folder` is acceptable, else a complete "Error: ..."
  // reply for the setting's writers.
  static const char* captureFolderRejectReason(const String& folder);

  // List images
  std::vector<ImageInfo> listImages(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  int getImageCount(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Get image data. Only a managed photo (see managedImagePath) is served.
  uint8_t* getImage(const String& path, size_t* outLen);

  // Delete one managed photo as `ctx`: a command handler passes its caller's
  // identity, so the filesystem rules decide what that caller may remove.
  // Anything outside the capture folder, or not a .jpg/.jpeg, is refused
  // before any filesystem access. Retention supplies the system identity
  // because its paths come from listImages. On false, `why` (when given)
  // receives the complete reply for the command that asked.
  bool deleteImage(const String& path, const AuthContext& ctx, const char** why = nullptr);
  bool deleteOldestImages(int count, ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Storage management
  StorageStats getStorageStats(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  bool ensureCaptureFolder(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Rotation/cleanup (delete oldest when max reached)
  bool enforceMaxImages(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Generate unique filename
  String generateFilename();
  
  // Get the capture folder path for a location
  String getCaptureFolder(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
private:
  bool littleFsAvailable;
  int imageCounter;  // For unique filenames
  // One stale-temporary sweep per tier per mount: a reset mid-save leaves
  // `<image>.jpg.tmp`, which no listing or retention would otherwise reclaim.
  // LittleFS is swept once per init(); SD once per VFS::sdMountGeneration().
  bool lfsStaleSwept = false;
  uint32_t sdSweptGeneration = 0;
  bool tierAvailable(bool isSd) const;
  void sweepStaleTemporaries(ImageStorageLocation location, const String& folder);

  // ImageManager owns "this is a managed photo": a .jpg/.jpeg file inside the
  // capture folder of the tier the path names. Reports the normalized path and
  // its tier when `path` qualifies; readers and deleters refuse anything else.
  bool managedImagePath(const String& path, String& normalizedPath, bool& isSd);

  // Helper to get mount prefix for location
  String getMountPrefix(ImageStorageLocation location);
};

// Global instance
extern ImageManager gImageManager;

// CLI commands
const char* cmd_capture(const String& argsInput);
const char* cmd_images(const String& argsInput);
const char* cmd_imagedelete(const String& argsInput);
const char* cmd_imagesend(const String& argsInput);

#endif // SYSTEM_IMAGEMANAGER_H
