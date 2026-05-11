/**
 * Hardware-Compatible File Manager Implementation
 * For use with TFT/OLED displays and embedded UIs
 */

#include <LittleFS.h>

#include "System_FileManager.h"
#include "System_Filesystem.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include "System_AuthIdentity.h"  // currentAuthContext

// FileManager has no AuthContext field of its own, so it reads the calling
// task's installed identity. Every transport (web/serial/BT/OLED) installs
// an identity before invoking the FileManager API. If a future caller wants
// per-instance identity, this should grow into a member field.

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
  const AuthContext& ctx = currentAuthContext();

  // Check if directory exists (use VFS for unified access)
  if (!VFS::existsGuarded(path, ctx)) return false;

  File dir = VFS::openGuarded(path, "r", ctx);
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

bool FileManager::refresh() {
  // Cache-invalidation against the identity generation counter. See
  // System_AuthIdentity.h for the full protocol — this is the canonical
  // consumer site, so the logic is worth reading.
  //
  // Two cases re-fill:
  //   1. loadedAtGen_ == 0: we've never successfully loaded under any
  //      identity (e.g. first-ever Files-menu tap, or every prior load
  //      hit PERM DENY). Always retry.
  //   2. loadedAtGen_ != gIdentityGeneration: a producer (pairing, user
  //      add/del/promote/demote) bumped the counter since we last filled
  //      the cache. Re-fill under the now-current permission topology.
  //
  // Otherwise the cache is still authoritative — return without a scan.
  // This is the hot path: typical menu redraw, returning from a file
  // viewer, scrolling. Re-scanning these would compound per-entry
  // VFS::openGuarded permission checks into 1-2 s of menu latency.
  const uint32_t curGen =
      gIdentityGeneration.load(std::memory_order_acquire);
  if (loadedAtGen_ != 0 && loadedAtGen_ == curGen) return true;
  cacheValid = false;
  state.dirty = true;
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
  const AuthContext& ctx = currentAuthContext();

  File dir = VFS::openGuarded(state.currentPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  // Same SD prefix-translation as loadDirectory — needed when the user
  // is browsing /sd/* with >64 items per directory.
  String effectivePath = VFS::stripSdPrefix(String(state.currentPath));

  int currentIdx = 0;
  File file = dir.openNextFile();

  while (file) {
    // Extract display name
    String fileName = String(file.name());
    if (effectivePath != "/") {
      char prefix[96];
      snprintf(prefix, sizeof(prefix), "%s/", effectivePath.c_str());
      if (fileName.startsWith(prefix)) {
        fileName = fileName.substring(strlen(prefix));
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
      
      // Get permissions for the *current* caller (so the toolbar in the
      // UI reflects what the user can actually do, not what some stale
      // context allows).
      String fullPath = formatPath(state.currentPath, entry.name);
      entry.permissions = getPermissions(fullPath, ctx);

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
  const AuthContext& ctx = currentAuthContext();

  FsLockGuard guard("FileManager.createFolder");

  bool success = VFS::mkdirGuarded(fullPath, ctx);
  if (success) {
    state.dirty = true;
    loadDirectory();
  }
  return success;
}

bool FileManager::createFile(const char* name) {
  if (!name || strlen(name) == 0) return false;
  String fullPath = formatPath(state.currentPath, name);
  const AuthContext& ctx = currentAuthContext();

  FsLockGuard guard("FileManager.createFile");

  File f = VFS::openGuarded(fullPath, "w", ctx, /*create=*/true);
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
  const AuthContext& ctx = currentAuthContext();

  FsLockGuard guard("FileManager.deleteItem");

  bool success = entry.isFolder
                   ? VFS::rmdirGuarded(fullPath, ctx)
                   : VFS::removeGuarded(fullPath, ctx);

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
  const AuthContext& ctx = currentAuthContext();

  FsLockGuard guard("FileManager.renameItem");

  bool success = VFS::renameGuarded(oldPath, newPath, ctx);
  if (success) {
    state.dirty = true;
    loadDirectory();
  }
  return success;
}

bool FileManager::readFile(const char* filename, String& content) {
  String fullPath = formatPath(state.currentPath, filename);
  const AuthContext& ctx = currentAuthContext();

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("FileManager.readFile");

  // Phase 4: previous code skipped any read permission check entirely —
  // FileManager would happily open sensitive files. openGuarded enforces
  // canRead now (with sensitive-extension blocks).
  File f = VFS::openGuarded(fullPath, "r", ctx);
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
  const AuthContext& ctx = currentAuthContext();

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("FileManager.writeFile");

  File f = VFS::openGuarded(fullPath, "w", ctx, /*create=*/true);
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
  
  // Determine storage type based on current path
  VFS::StorageType storageType = VFS::getStorageType(state.currentPath);
  uint64_t totalBytes = 0, usedBytes = 0, freeBytes = 0;
  
  if (VFS::getStats(storageType, totalBytes, usedBytes, freeBytes)) {
    total = (uint32_t)totalBytes;
    used = (uint32_t)usedBytes;
    free = (uint32_t)freeBytes;
    return true;
  }
  
  // Fallback to LittleFS stats
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
  const AuthContext& ctx = currentAuthContext();

  File dir = VFS::openGuarded(state.currentPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    gSensorPollingPaused = wasPaused;
    loadedAtGen_ = 0;  // mark "never loaded ok" — refresh() will retry
    return false;
  }

  // The SD library returns entry names rooted at the SD card root ("/")
  // — it doesn't know about our "/sd" mount-point convention. So when
  // currentPath is "/sd" or "/sd/foo", the prefix the underlying FS
  // sees is the SD-relative path (with "/sd" stripped). Compute that
  // once so we can strip it from each entry name correctly. For
  // LittleFS paths this is a no-op (stripSdPrefix returns the input
  // unchanged when there's no /sd prefix).
  String effectivePath = VFS::stripSdPrefix(String(state.currentPath));

  // Load and cache directory entries
  cachedCount = 0;
  state.totalItems = 0;

  // Mount points (e.g. /sd at LittleFS root) — the VFS layer is the
  // authority on which synthetic entries belong at this path. Each one
  // gets translated into a FileEntry so the rest of the FileManager
  // pipeline treats it like any other folder. Permissions are looked up
  // per-mount via the regular getPermissions() rule table (e.g. /sd is
  // PERM_READ — browse but don't delete the mount point).
  {
    VFS::VirtualEntry virtuals[4];
    const size_t nVirt = VFS::listVirtualEntries(
        String(state.currentPath), virtuals,
        sizeof(virtuals) / sizeof(virtuals[0]));
    for (size_t v = 0; v < nVirt; v++) {
      if (cachedCount >= FILE_MANAGER_MAX_CACHED_ITEMS) break;
      strncpy(cachedEntries[cachedCount].name, virtuals[v].name,
              FILE_MANAGER_MAX_NAME - 1);
      cachedEntries[cachedCount].name[FILE_MANAGER_MAX_NAME - 1] = '\0';
      cachedEntries[cachedCount].isFolder = virtuals[v].isFolder;
      cachedEntries[cachedCount].size = 0;
      String fullPath = formatPath(state.currentPath, virtuals[v].name);
      cachedEntries[cachedCount].permissions = getPermissions(fullPath, ctx);
      cachedCount++;
      state.totalItems++;
    }
  }

  File file = dir.openNextFile();

  while (file) {
    String fileName = String(file.name());

    // Extract display name and filter. Strip the *effective* path
    // prefix so SD entries (where the FS-level path is SD-rooted) are
    // handled the same as LittleFS entries.
    if (effectivePath != "/") {
      char prefix[96];
      snprintf(prefix, sizeof(prefix), "%s/", effectivePath.c_str());
      if (fileName.startsWith(prefix)) {
        fileName = fileName.substring(strlen(prefix));
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

      // Get permissions for the *current* caller — drives toolbar
      // enable/disable in the UI per actual identity.
      String fullPath = formatPath(state.currentPath, cachedEntries[cachedCount].name);
      cachedEntries[cachedCount].permissions = getPermissions(fullPath, ctx);

      cachedCount++;
    }

    state.totalItems++;
    file = dir.openNextFile();
  }

  dir.close();
  cacheValid = true;
  // Tag the cache with the generation it was filled under. refresh() reads
  // this and re-fills only when gIdentityGeneration has advanced past it.
  loadedAtGen_ = gIdentityGeneration.load(std::memory_order_acquire);
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
  // "Protected" is per-caller: a path is protected if the active caller
  // can't delete it.
  return !canDelete(String(path), currentAuthContext());
}

// Helper functions
String formatFileSize(uint32_t bytes) {
  if (bytes >= 1048576) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0); return buf;
  } else if (bytes >= 1024) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f KB", bytes / 1024.0); return buf;
  } else {
    char buf[16]; snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes); return buf;
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
