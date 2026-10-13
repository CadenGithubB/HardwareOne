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
  bool isOnSD;              // True if on SD card
};

// Storage stats
struct StorageStats {
  size_t totalBytes;
  size_t usedBytes;
  size_t freeBytes;
  int imageCount;
  bool available;
};

class ImageManager {
public:
  ImageManager();
  ~ImageManager();
  
  // Initialization
  bool init();
  bool initSD();  // Initialize SD card if present
  bool isSDAvailable() const { return sdAvailable; }
  bool isLittleFSAvailable() const { return littleFSAvailable; }
  
  // Capture and save
  // Returns the saved image's full path, or empty string on failure.
  String captureAndSave(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Save directly to generated paths; failed-write cleanup is best-effort.
  // BOTH tries each tier independently, succeeds if either completes, and
  // prefers the internal path when both complete. Empty means neither saved.
  String saveImage(const uint8_t* data, size_t len, ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);

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
  // because its paths come from listImages.
  bool deleteImage(const String& path, const AuthContext& ctx);
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
  bool sdAvailable;
  bool littleFSAvailable;
  int imageCounter;  // For unique filenames
  // One stale-temporary sweep per tier per mount: a reset mid-save leaves
  // `<image>.jpg.tmp`, which no listing or retention would otherwise reclaim.
  bool lfsStaleSwept = false;
  bool sdStaleSwept = false;
  void sweepStaleTemporaries(ImageStorageLocation location, const String& folder);

  // ImageManager owns "this is a managed photo": a .jpg/.jpeg file inside the
  // capture folder of the tier the path names. Reports the normalized path and
  // its tier when `path` qualifies; readers and deleters refuse anything else.
  bool managedImagePath(const String& path, String& normalizedPath, bool& isSD);

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
