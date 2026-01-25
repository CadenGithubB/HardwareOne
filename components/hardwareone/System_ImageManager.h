#ifndef SYSTEM_IMAGEMANAGER_H
#define SYSTEM_IMAGEMANAGER_H

#include <Arduino.h>
#include <vector>
#include "System_BuildConfig.h"

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
  // Returns the filename of the saved image, or empty string on failure
  String captureAndSave(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Save existing buffer to storage
  String saveImage(const uint8_t* data, size_t len, ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // List images
  std::vector<ImageInfo> listImages(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  int getImageCount(ImageStorageLocation location = IMAGE_STORAGE_LITTLEFS);
  
  // Get image data
  uint8_t* getImage(const String& path, size_t* outLen);
  bool getImageInfo(const String& path, ImageInfo& info);
  
  // Delete image
  bool deleteImage(const String& path);
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
  
  // Helper to get mount prefix for location
  String getMountPrefix(ImageStorageLocation location);
};

// Global instance
extern ImageManager gImageManager;

// CLI commands
const char* cmd_capture(const String& cmd);
const char* cmd_images(const String& cmd);
const char* cmd_imageview(const String& cmd);
const char* cmd_imagedelete(const String& cmd);
const char* cmd_imagesend(const String& cmd);

#endif // SYSTEM_IMAGEMANAGER_H
