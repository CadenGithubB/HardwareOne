/**
 * Hardware-Compatible File Manager Implementation
 * For use with TFT/OLED displays and embedded UIs
 */

#include <LittleFS.h>

#include "System_FileManager.h"
#include "System_Filesystem.h"
#include "System_Mutex.h"

// Global instance (optional)
FileManager* gFileManager = nullptr;

FileManager::FileManager() {
  memset(&state, 0, sizeof(state));
  strncpy(state.currentPath, "/", FILE_MANAGER_MAX_PATH - 1);
  state.selectedIndex = 0;
  state.scrollOffset = 0;
  state.totalItems = 0;
  state.showHidden = false;
  state.dirty = true;
  cachedCount = 0;
  cacheValid = false;
}

bool FileManager::navigate(const char* path) {
  if (!path || strlen(path) == 0) return false;
  
  // Ensure path starts with /
  if (path[0] != '/') return false;

  FsLockGuard guard("FileManager.navigate");
  
  // Check if directory exists
  if (!LittleFS.exists(path)) return false;
  
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  dir.close();
  
  // Update path
  strncpy(state.currentPath, path, FILE_MANAGER_MAX_PATH - 1);
  state.currentPath[FILE_MANAGER_MAX_PATH - 1] = '\0';
  
  // Reset navigation
  state.selectedIndex = 0;
  state.scrollOffset = 0;
  state.dirty = true;
  cacheValid = false;  // Invalidate cache on navigation
  
  return loadDirectory();
}

bool FileManager::navigateUp() {
  // Find last slash
  char* lastSlash = strrchr(state.currentPath, '/');
  if (!lastSlash || lastSlash == state.currentPath) {
    // Already at root
    if (strcmp(state.currentPath, "/") == 0) return false;
    return navigate("/");
  }
  
  // Truncate at last slash
  *lastSlash = '\0';
  if (strlen(state.currentPath) == 0) {
    strcpy(state.currentPath, "/");
  }
  
  state.selectedIndex = 0;
  state.scrollOffset = 0;
  state.dirty = true;
  cacheValid = false;  // Invalidate cache on navigation
  
  return loadDirectory();
}

bool FileManager::navigateInto() {
  FileEntry entry;
  if (!getCurrentItem(entry)) return false;
  
  if (!entry.isFolder) return false;  // Not a folder
  
  // Build new path
  String newPath = formatPath(state.currentPath, entry.name);
  return navigate(newPath.c_str());
}

void FileManager::moveUp() {
  if (state.selectedIndex > 0) {
    state.selectedIndex--;
    if (state.selectedIndex < state.scrollOffset) {
      state.scrollOffset = state.selectedIndex;
    }
  }
}

void FileManager::moveDown() {
  if (state.selectedIndex < state.totalItems - 1) {
    state.selectedIndex++;
    if (state.selectedIndex >= state.scrollOffset + FILE_MANAGER_PAGE_SIZE) {
      state.scrollOffset = state.selectedIndex - FILE_MANAGER_PAGE_SIZE + 1;
    }
  }
}

void FileManager::moveToTop() {
  state.selectedIndex = 0;
  state.scrollOffset = 0;
}

void FileManager::moveToBottom() {
  state.selectedIndex = state.totalItems - 1;
  if (state.totalItems > FILE_MANAGER_PAGE_SIZE) {
    state.scrollOffset = state.totalItems - FILE_MANAGER_PAGE_SIZE;
  }
}

bool FileManager::getItem(int index, FileEntry& entry) {
  if (index < 0 || index >= state.totalItems) return false;
  
  // Use cached entries if available
  if (cacheValid && index < cachedCount) {
    memcpy(&entry, &cachedEntries[index], sizeof(FileEntry));
    return true;
  }
  
  // Fallback: scan filesystem if index is beyond cache
  // (This should rarely happen with FILE_MANAGER_MAX_CACHED_ITEMS=64)

  FsLockGuard guard("FileManager.getItem.scan");

  File dir = LittleFS.open(state.currentPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  
  int currentIdx = 0;
  File file = dir.openNextFile();
  
  while (file) {
    // Extract display name
    String fileName = String(file.name());
    if (strcmp(state.currentPath, "/") != 0) {
      String prefix = String(state.currentPath) + "/";
      if (fileName.startsWith(prefix)) {
        fileName = fileName.substring(prefix.length());
      }
    } else if (fileName.startsWith("/")) {
      fileName = fileName.substring(1);
    }
    
    // Skip nested paths
    if (fileName.indexOf('/') != -1) {
      file = dir.openNextFile();
      continue;
    }
    
    // Skip hidden files if configured
    if (!state.showHidden && fileName.startsWith(".")) {
      file = dir.openNextFile();
      continue;
    }
    
    if (currentIdx == index) {
      // Found it
      strncpy(entry.name, fileName.c_str(), FILE_MANAGER_MAX_NAME - 1);
      entry.name[FILE_MANAGER_MAX_NAME - 1] = '\0';
      entry.isFolder = file.isDirectory();
      entry.size = entry.isFolder ? 0 : file.size();
      
      // Get permissions
      String fullPath = formatPath(state.currentPath, entry.name);
      entry.permissions = getPermissions(fullPath);
      
      file.close();
      dir.close();
      return true;
    }
    
    currentIdx++;
    file = dir.openNextFile();
  }
  
  dir.close();
  return false;
}

bool FileManager::getCurrentItem(FileEntry& entry) {
  return getItem(state.selectedIndex, entry);
}

int FileManager::getPageEnd() const {
  int end = state.scrollOffset + FILE_MANAGER_PAGE_SIZE;
  return (end > state.totalItems) ? state.totalItems : end;
}

bool FileManager::createFolder(const char* name) {
  if (!name || strlen(name) == 0) return false;
  
  String fullPath = formatPath(state.currentPath, name);
  
  if (!canCreate(fullPath)) return false;

  FsLockGuard guard("FileManager.createFolder");
  
  bool success = LittleFS.mkdir(fullPath.c_str());
  if (success) {
    state.dirty = true;
    loadDirectory();
  }
  return success;
}

bool FileManager::createFile(const char* name) {
  if (!name || strlen(name) == 0) return false;
  
  String fullPath = formatPath(state.currentPath, name);
  
  if (!canCreate(fullPath)) return false;

  FsLockGuard guard("FileManager.createFile");
  
  File f = LittleFS.open(fullPath.c_str(), "w");
  if (!f) return false;
  
  f.close();
  state.dirty = true;
  loadDirectory();
  return true;
}

bool FileManager::deleteItem() {
  FileEntry entry;
  if (!getCurrentItem(entry)) return false;
  
  String fullPath = formatPath(state.currentPath, entry.name);
  
  if (!canDelete(fullPath)) return false;

  FsLockGuard guard("FileManager.deleteItem");
  
  bool success;
  if (entry.isFolder) {
    success = LittleFS.rmdir(fullPath.c_str());
  } else {
    success = LittleFS.remove(fullPath.c_str());
  }
  
  if (success) {
    state.dirty = true;
    loadDirectory();
    ensureValidSelection();
  }
  
  return success;
}

bool FileManager::renameItem(const char* newName) {
  if (!newName || strlen(newName) == 0) return false;
  
  FileEntry entry;
  if (!getCurrentItem(entry)) return false;
  
  String oldPath = formatPath(state.currentPath, entry.name);
  String newPath = formatPath(state.currentPath, newName);
  
  if (!canDelete(oldPath)) return false;  // Need delete permission to rename

  FsLockGuard guard("FileManager.renameItem");
  
  bool success = LittleFS.rename(oldPath.c_str(), newPath.c_str());
  if (success) {
    state.dirty = true;
    loadDirectory();
  }
  
  return success;
}

bool FileManager::readFile(const char* filename, String& content) {
  String fullPath = formatPath(state.currentPath, filename);
  
  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("FileManager.readFile");
  
  File f = LittleFS.open(fullPath.c_str(), "r");
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  content = "";
  while (f.available()) {
    content += (char)f.read();
  }
  
  f.close();
  gSensorPollingPaused = wasPaused;
  return true;
}

bool FileManager::writeFile(const char* filename, const String& content) {
  String fullPath = formatPath(state.currentPath, filename);
  
  if (!canEdit(fullPath)) return false;
  
  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("FileManager.writeFile");
  
  File f = LittleFS.open(fullPath.c_str(), "w");
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  size_t written = f.print(content);
  f.close();
  
  gSensorPollingPaused = wasPaused;
  return (written == content.length());
}

bool FileManager::getStorageStats(uint32_t& total, uint32_t& used, uint32_t& free) {
  FsLockGuard guard("FileManager.getStorageStats");
  total = LittleFS.totalBytes();
  used = LittleFS.usedBytes();
  free = total - used;
  return true;
}

bool FileManager::loadDirectory() {
  // Pause sensor polling during directory scan
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("FileManager.loadDirectory");
  
  File dir = LittleFS.open(state.currentPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Load and cache directory entries
  cachedCount = 0;
  state.totalItems = 0;
  File file = dir.openNextFile();
  
  while (file) {
    String fileName = String(file.name());
    
    // Extract display name and filter
    if (strcmp(state.currentPath, "/") != 0) {
      String prefix = String(state.currentPath) + "/";
      if (fileName.startsWith(prefix)) {
        fileName = fileName.substring(prefix.length());
      }
    } else if (fileName.startsWith("/")) {
      fileName = fileName.substring(1);
    }
    
    // Skip nested paths
    if (fileName.indexOf('/') != -1) {
      file = dir.openNextFile();
      continue;
    }
    
    // Skip hidden files if configured
    if (!state.showHidden && fileName.startsWith(".")) {
      file = dir.openNextFile();
      continue;
    }
    
    // Cache this entry if we have space
    if (cachedCount < FILE_MANAGER_MAX_CACHED_ITEMS) {
      strncpy(cachedEntries[cachedCount].name, fileName.c_str(), FILE_MANAGER_MAX_NAME - 1);
      cachedEntries[cachedCount].name[FILE_MANAGER_MAX_NAME - 1] = '\0';
      cachedEntries[cachedCount].isFolder = file.isDirectory();
      cachedEntries[cachedCount].size = cachedEntries[cachedCount].isFolder ? 0 : file.size();
      
      // Get permissions
      String fullPath = formatPath(state.currentPath, cachedEntries[cachedCount].name);
      cachedEntries[cachedCount].permissions = getPermissions(fullPath);
      
      cachedCount++;
    }
    
    state.totalItems++;
    file = dir.openNextFile();
  }
  
  dir.close();
  cacheValid = true;
  ensureValidSelection();
  state.dirty = false;
  
  gSensorPollingPaused = wasPaused;
  return true;
}

void FileManager::ensureValidSelection() {
  if (state.selectedIndex >= state.totalItems) {
    state.selectedIndex = (state.totalItems > 0) ? (state.totalItems - 1) : 0;
  }
  
  if (state.scrollOffset > state.selectedIndex) {
    state.scrollOffset = state.selectedIndex;
  }
  
  if (state.scrollOffset + FILE_MANAGER_PAGE_SIZE <= state.selectedIndex) {
    state.scrollOffset = state.selectedIndex - FILE_MANAGER_PAGE_SIZE + 1;
    if (state.scrollOffset < 0) state.scrollOffset = 0;
  }
}

bool FileManager::isProtectedPath(const char* path) {
  return !canDelete(String(path));
}

// Helper functions
String formatFileSize(uint32_t bytes) {
  if (bytes >= 1048576) {
    return String(bytes / 1048576.0, 2) + " MB";
  } else if (bytes >= 1024) {
    return String(bytes / 1024.0, 2) + " KB";
  } else {
    return String(bytes) + " B";
  }
}

String formatPath(const char* base, const char* append) {
  String result = String(base);
  
  // Ensure base doesn't end with / unless it's root
  if (result.length() > 1 && result.endsWith("/")) {
    result.remove(result.length() - 1);
  }
  
  // Add separator if needed
  if (!result.endsWith("/")) {
    result += "/";
  }
  
  // Append new part
  result += append;
  
  return result;
}
