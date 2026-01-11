#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <Arduino.h>
#include <LittleFS.h>


/**
 * Hardware-Compatible File Manager API
 * Designed for use with TFT/OLED displays and embedded UIs
 * Provides lightweight navigation and file operations
 */

// Maximum items to display per page
#define FILE_MANAGER_PAGE_SIZE 4
#define FILE_MANAGER_MAX_PATH 128
#define FILE_MANAGER_MAX_NAME 64
#define FILE_MANAGER_MAX_CACHED_ITEMS 64  // Cache up to 64 directory entries

// File manager state
struct FileManagerState {
  char currentPath[FILE_MANAGER_MAX_PATH];
  int selectedIndex;        // Currently selected item
  int scrollOffset;         // Scroll position for pagination
  int totalItems;           // Total items in current directory
  bool showHidden;          // Show hidden files/folders
  bool dirty;               // Needs refresh
};

// File entry for display
struct FileEntry {
  char name[FILE_MANAGER_MAX_NAME];
  bool isFolder;
  uint32_t size;
  uint8_t permissions;      // Bitmask from FilePermission enum
};

// File manager class for hardware displays
class FileManager {
public:
  FileManager();
  
  // Navigation
  bool navigate(const char* path);
  bool navigateUp();
  bool navigateInto();  // Enter selected folder
  
  // Selection
  void moveUp();
  void moveDown();
  void moveToTop();
  void moveToBottom();
  int getSelectedIndex() const { return state.selectedIndex; }
  
  // Item access
  bool getItem(int index, FileEntry& entry);
  bool getCurrentItem(FileEntry& entry);
  int getItemCount() const { return state.totalItems; }
  
  // Path info
  const char* getCurrentPath() const { return state.currentPath; }
  
  // Display helpers
  int getPageStart() const { return state.scrollOffset; }
  int getPageEnd() const;
  bool needsRefresh() const { return state.dirty; }
  void clearDirty() { state.dirty = false; }
  
  // File operations (with permission checks)
  bool createFolder(const char* name);
  bool createFile(const char* name);
  bool deleteItem();  // Delete currently selected item
  bool renameItem(const char* newName);
  
  // Content operations
  bool readFile(const char* filename, String& content);
  bool writeFile(const char* filename, const String& content);
  
  // Status
  bool getStorageStats(uint32_t& total, uint32_t& used, uint32_t& free);
  
private:
  FileManagerState state;
  
  // Directory entry cache to avoid repeated filesystem scans
  FileEntry cachedEntries[FILE_MANAGER_MAX_CACHED_ITEMS];
  int cachedCount;
  bool cacheValid;
  
  bool loadDirectory();
  void ensureValidSelection();
  bool isProtectedPath(const char* path);
};

// Global file manager instance (optional, for simple use)
extern FileManager* gFileManager;

// Helper functions
String formatFileSize(uint32_t bytes);
String formatPath(const char* base, const char* append);

#endif // FILE_MANAGER_H
