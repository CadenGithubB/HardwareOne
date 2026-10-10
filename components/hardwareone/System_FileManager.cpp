/**
 * Hardware-Compatible File Manager Implementation
 * For use with TFT/OLED displays and embedded UIs
 */

#include <LittleFS.h>

#include "System_FileManager.h"
#include "System_PollPause.h"   // PollPauseGuard — quiesce sensor polling during file I/O
#include "System_Filesystem.h"
#include "System_Filesystem_Internal.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include "System_AuthIdentity.h"  // currentAuthContext

// FileManager has no AuthContext field of its own, so it reads the calling
// task's installed identity. Every transport (web/serial/BT/OLED) installs
// an identity before invoking the FileManager API. If a future caller wants
// per-instance identity, this should grow into a member field.

// Counts a folder's direct children (capped) for the "[#]" badge — defined
// below loadDirectory(); forward-declared so getItem()'s uncached fallback can
// use it too.
static uint16_t countFolderEntries(const String& fullPath, const AuthContext& ctx);

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

// Unconditional re-scan of the current directory. refresh() above is
// generation-gated and a file delete/rename does NOT bump the identity
// generation — a caller that just mutated the directory must use this or
// the stale cache keeps showing the old listing. Selection is preserved
// (clamped by loadDirectory if the entry count shrank).
bool FileManager::forceRescan() {
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

// Convert to the browser's bounded display record before applying its picker
// predicate. Keep the reader's full path separately for permission metadata.
static FileEntry makeBrowserEntry(const VFS::DirectoryEntry& item) {
  FileEntry entry{};
  strncpy(entry.name, item.name.c_str(), FILE_MANAGER_MAX_NAME - 1);
  entry.isFolder = item.isFolder;
  entry.size = item.size;
  return entry;
}

bool FileManager::isVisible(const FileEntry& entry) const {
  if (!state.showHidden && entry.name[0] == '.') return false;
  return entry.isFolder || !visibilityFilter_ || visibilityFilter_(entry);
}

bool FileManager::getItem(int index, FileEntry& entry) {
  if (index < 0 || index >= state.totalItems) return false;

  if (cacheValid && index < cachedCount) {
    entry = cachedEntries[index];
    return true;
  }

  // Beyond the cache, replay the same entry order and visibility predicate as
  // loadDirectory(), including virtual mounts and picker matches.
  FsLockGuard guard("FileManager.getItem.scan");
  const AuthContext& ctx = currentAuthContext();
  File dir = VFS::openGuarded(state.currentPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  VFS::DirectoryReader reader(dir, state.currentPath);
  VFS::DirectoryEntry item;
  int currentIdx = 0;
  while (reader.next(item)) {
    FileEntry candidate = makeBrowserEntry(item);
    if (!isVisible(candidate)) continue;
    if (currentIdx++ != index) continue;

    entry = candidate;
    // A one-item fallback uses a fresh live query, just as before. This mask
    // is UI metadata; every later file operation still checks its own access.
    entry.permissions = getPermissions(item.path, ctx);
    entry.childCount = (countFolderChildren_ && entry.isFolder)
                           ? countFolderEntries(item.path, ctx) : 0;
    dir.close();
    return true;
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

  // Pause sensor polling during file I/O (RAII — resumes on every return path).
  PollPauseGuard pollGuard;

  FsLockGuard guard("FileManager.readFile");

  // Phase 4: previous code skipped any read permission check entirely —
  // FileManager would happily open sensitive files. openGuarded enforces
  // canRead now (with sensitive-extension blocks).
  File f = VFS::openGuarded(fullPath, "r", ctx);
  if (!f) {
    return false;
  }

  content = "";
  while (f.available()) {
    content += (char)f.read();
  }

  f.close();
  return true;
}

bool FileManager::writeFile(const char* filename, const String& content) {
  String fullPath = formatPath(state.currentPath, filename);
  const AuthContext& ctx = currentAuthContext();

  // Pause sensor polling during file I/O (RAII — resumes on every return path).
  PollPauseGuard pollGuard;

  FsLockGuard guard("FileManager.writeFile");

  File f = VFS::openGuarded(fullPath, "w", ctx, /*create=*/true);
  if (!f) {
    return false;
  }

  size_t written = f.print(content);
  f.close();
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
  
  // Fallback to LittleFS stats. One traverse instead of two, and getStats
  // guards the subtraction that used to be a bare `total - used`.
  uint64_t lfsTotal = 0, lfsUsed = 0, lfsFree = 0;
  (void)VFS::getStats(VFS::INTERNAL, lfsTotal, lfsUsed, lfsFree);
  total = (uint32_t)lfsTotal;
  used = (uint32_t)lfsUsed;
  free = (uint32_t)lfsFree;
  return true;
}

// Count a folder's direct children (capped) for the "[#]" badge. Uses the same
// VFS::openGuarded + openNextFile walk as buildFilesListing()'s `count` field so
// the on-lens badge matches the web/app numbers. Bounded by kFolderCountCap so a
// pathological directory can't stall the scan; the UI renders "<cap>+" there.
static constexpr uint16_t kFolderCountCap = 99;
static uint16_t countFolderEntries(const String& fullPath, const AuthContext& ctx) {
  uint16_t n = 0;
  File sub = VFS::openGuarded(fullPath, "r", ctx);
  if (sub && sub.isDirectory()) {
    File child = sub.openNextFile();
    while (child) {
      if (++n >= kFolderCountCap) break;   // cap the walk; UI shows "<cap>+"
      child = sub.openNextFile();
    }
  }
  if (sub) sub.close();
  return n;
}

bool FileManager::loadDirectory() {
  // Pause sensor polling during directory scan (RAII — resumes on every return).
  PollPauseGuard pollGuard;

  FsLockGuard guard("FileManager.loadDirectory");
  const AuthContext& ctx = currentAuthContext();

  File dir = VFS::openGuarded(state.currentPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    loadedAtGen_ = 0;  // mark "never loaded ok" — refresh() will retry
    return false;
  }

  // Entry masks are display metadata, not access grants. Resolve this named
  // caller once while the existing directory-scan lock remains held; every
  // later VFS operation still performs its own live guarded check.
  FsInternal::LockedListingPermissions listingPermissions(ctx);

  cachedCount = 0;
  state.totalItems = 0;

  VFS::DirectoryReader reader(dir, state.currentPath);
  VFS::DirectoryEntry item;
  while (reader.next(item)) {
    FileEntry entry = makeBrowserEntry(item);
    // Filter before counting/caching so matching files beyond the first 256
    // physical entries remain reachable, using getItem()'s identical view.
    if (!isVisible(entry)) continue;

    if (cachedCount < FILE_MANAGER_MAX_CACHED_ITEMS) {
      entry.permissions = listingPermissions.forPath(item.path);
      entry.childCount = (countFolderChildren_ && entry.isFolder)
                             ? countFolderEntries(item.path, ctx) : 0;
      cachedEntries[cachedCount++] = entry;
    }
    state.totalItems++;
  }
  dir.close();

  cacheValid = true;
  // Tag the cache with the generation it was filled under. refresh() reads
  // this and re-fills only when gIdentityGeneration has advanced past it.
  loadedAtGen_ = gIdentityGeneration.load(std::memory_order_acquire);
  ensureValidSelection();
  state.dirty = false;

  // pollGuard resumes sensor polling on return.
  return true;
}

void FileManager::setVisibilityFilter(VisibilityFilter filter) {
  if (visibilityFilter_ == filter) return;
  visibilityFilter_ = filter;
  // Force a full re-load — the cache was filtered against the old predicate
  // and may be missing entries the new one would let through (or carrying
  // ones it would drop).
  cacheValid = false;
  loadedAtGen_ = 0;  // bypass the gen-versioned refresh fast path
  loadDirectory();
}

void FileManager::setCountFolderChildren(bool on) {
  if (countFolderChildren_ == on) return;
  countFolderChildren_ = on;
  // The cache was filled without (or with stale) child counts — force a
  // re-scan so folder rows get their badge. Cheap when called at init before
  // the first navigate (cache is empty / never loaded).
  cacheValid = false;
  loadedAtGen_ = 0;  // bypass the gen-versioned refresh fast path
  if (state.currentPath[0]) loadDirectory();
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
