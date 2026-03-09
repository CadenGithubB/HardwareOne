/**
 * Filesystem Module - LittleFS management and utilities
 * Centralized filesystem operations and state
 */

#include <LittleFS.h>

#include <esp_log.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_ImageManager.h"
#include "System_VFS.h"

// External dependencies
extern bool readText(const char* path, String& out);
extern void getTimestampPrefixMsCached(char* buffer, size_t bufferSize);
extern bool sanitizeAutomationsJson(String& json);
extern time_t computeNextRunTime(const char* automationJson, time_t currentTime);
extern void writeAutomationsJsonAtomic(const String& json);
extern void notifyAutomationScheduler();
extern bool gAutosDirty;

// External constants
extern const char AUTOMATIONS_JSON_FILE[];

// Forward declarations (none needed — permissions are table-driven)

// ============================================================================
// Filesystem State (owned by this module)
// ============================================================================

bool filesystemReady = false;

// ============================================================================
// Filesystem Initialization
// ============================================================================

bool initFilesystem() {
  ESP_LOGI("FS", "Initializing LittleFS...");
  Serial.println("[FS] Initializing LittleFS...");
  Serial.flush();
  delay(50);  // Allow serial to flush
  
  // Configure LittleFS using ESP-IDF native API (bypasses Arduino wrapper issues)
  if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    ESP_LOGW("FS", "LittleFS mount failed; formatting and retrying");
    Serial.println("[FS] Mount failed; formatting and retrying...");
    Serial.flush();

    if (!LittleFS.format()) {
      ESP_LOGE("FS", "LittleFS format failed");
      Serial.println("[FS] ERROR: LittleFS format failed");
      Serial.flush();
      filesystemReady = false;
      return false;
    }

    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
      ESP_LOGE("FS", "LittleFS mount failed after format");
      Serial.println("[FS] ERROR: LittleFS mount failed after format");
      Serial.flush();
      filesystemReady = false;
      return false;
    }
  }
  
  ESP_LOGI("FS", "LittleFS mounted successfully");
  Serial.println("[FS] LittleFS mounted successfully");
  Serial.flush();
  filesystemReady = true;

  VFS::init();
  
#if ENABLE_CAMERA_SENSOR
  // Initialize ImageManager now that filesystem is ready (creates photos folder)
  gImageManager.init();
#endif
  
  // Ensure system directories exist
  LittleFS.mkdir("/logging_captures");
  LittleFS.mkdir("/system");  // For settings, automations, devices, etc.
  LittleFS.mkdir("/system/sys_logs");  // For protected system logs (login, errors, command history)
  LittleFS.mkdir("/system/users");  // For users.json and user settings
  LittleFS.mkdir("/system/users/user_settings");  // For per-user setting files
  LittleFS.mkdir("/system/certs");  // For TLS certificates (HTTPS, MQTT)
#if ENABLE_ESPNOW
  LittleFS.mkdir("/espnow");  // For ESP-NOW related files (received subfolder created on-demand)
  LittleFS.mkdir("/system/espnow");  // For ESP-NOW config (mesh peers, devices, bond peer settings)
  LittleFS.mkdir("/system/espnow/peers");  // For per-peer cached settings
#endif
#if ENABLE_MAPS
  LittleFS.mkdir("/maps");  // For GPS map files (.hwmap)
#endif
  
  // Migrate pending_users.json from old location to /system/users/ (one-time)
  if (LittleFS.exists("/system/pending_users.json") && !LittleFS.exists("/system/users/pending_users.json")) {
    LittleFS.rename("/system/pending_users.json", "/system/users/pending_users.json");
    DEBUG_STORAGEF("Migrated pending_users.json to /system/users/");
  }
  
  DEBUG_STORAGEF("Filesystem initialized successfully");

  // Load and increment boot sequence for user creation timestamp tracking
  loadAndIncrementBootSeq();

  // Now safe to broadcast (this may trigger CLI history allocation, which will be logged)
  // Show FS stats
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  BROADCAST_PRINTF("FS Total: %zu bytes, Used: %zu, Free: %zu", total, used, total - used);

#if ENABLE_AUTOMATION
  // Boot-time automations.json sanitation: ensure no duplicate IDs persist from manual edits
  // Skip if automation system is disabled
  uint32_t _dbgSaved = getDebugFlags();
  setDebugFlag(DEBUG_AUTO_SCHEDULER);
  if (gSettings.automationsEnabled && LittleFS.exists(AUTOMATIONS_JSON_FILE)) {
    String json;
    if (readText(AUTOMATIONS_JSON_FILE, json)) {
      bool modified = false;

      // First: sanitize duplicate IDs
      if (sanitizeAutomationsJson(json)) {
        modified = true;
        DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Boot sanitize: fixed duplicate IDs");
      } else {
        DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Boot sanitize: no duplicate IDs found");
      }

      // Write back if any changes were made
      if (modified) {
        writeAutomationsJsonAtomic(json);
        gAutosDirty = true;
        notifyAutomationScheduler();
        DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Boot: wrote updated automations.json; scheduler refresh queued");
      }
    } else {
      DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Boot sanitize: failed to read automations.json");
    }
  } else {
    DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Boot sanitize: /system/automations.json not found, skipping");
  }
  setDebugFlags(_dbgSaved);  // restore debug flags
#endif
  
  return true;
}

// ============================================================================
// Directory Listing Helper
// ============================================================================

bool buildFilesListing(const String& inPath, String& out, bool asJson, bool hideAdminPaths) {
  String dirPath = VFS::normalize(inPath);

  DEBUG_STORAGEF("[buildFilesListing] START path='%s' heap=%u", dirPath.c_str(), (unsigned)ESP.getFreeHeap());

  // Determine if we're listing SD card content
  bool sdRequested = (VFS::getStorageType(dirPath) == VFS::SDCARD);
  String fsDirPath = sdRequested ? VFS::stripSdPrefix(dirPath) : dirPath;

  FsLockGuard _dirGuard("dir.list");

  // Virtual root: show /sd folder if SD is available and we're at LittleFS root
  bool includeVirtualSd = (!sdRequested && dirPath == "/" && VFS::isSDAvailable());

  File root = VFS::open(dirPath, "r");
  if (!root || !root.isDirectory()) {
    ERROR_STORAGEF("Cannot open directory '%s'", dirPath.c_str());
    if (asJson) {
      out = "";  // caller will wrap error
    } else {
      out = String("Error: Cannot open directory '") + dirPath + "'";
    }
    return false;
  }

  bool first = true;
  int fileCount = 0;
  if (!asJson) {
    out = String("Files (") + dirPath + "):\n";
  } else {
    out = "";  // array body only
  }

  // Inject virtual /sd folder at root when SD card is available
  if (includeVirtualSd) {
    // Count items on SD root for display
    uint32_t sdCount = 0;
    File sdRoot = VFS::open("/sd", "r");
    if (sdRoot && sdRoot.isDirectory()) {
      File child = sdRoot.openNextFile();
      while (child) {
        sdCount++;
        child = sdRoot.openNextFile();
      }
      sdRoot.close();
    }
    if (asJson) {
      uint8_t sdPerms = getPermissions("/sd");
      char sdBuf[96];
      snprintf(sdBuf, sizeof(sdBuf), "{\"name\":\"sd\",\"type\":\"folder\",\"size\":\"%u items\",\"count\":%u,\"perms\":%u}", (unsigned)sdCount, (unsigned)sdCount, (unsigned)sdPerms);
      out += sdBuf;
      first = false;
    } else {
      out += "  sd (";
      out += sdCount;
      out += " items) [SD Card]\n";
      fileCount++;
    }
  }

  File file = root.openNextFile();
  while (file) {
    // Extract display name (strip leading directory)
    String fileName = String(file.name());
    DEBUG_STORAGEF("[buildFilesListing] Processing file: '%s' heap=%u", fileName.c_str(), (unsigned)ESP.getFreeHeap());
    if (dirPath != "/") {
      String expectedPrefix = dirPath;
      if (!expectedPrefix.endsWith("/")) expectedPrefix += "/";
      if (fileName.startsWith(expectedPrefix)) fileName = fileName.substring(expectedPrefix.length());
    } else {
      if (fileName.startsWith("/")) fileName = fileName.substring(1);
    }
    // Skip nested paths that still contain '/'
    if (fileName.length() == 0 || fileName.indexOf('/') != -1) {
      file = root.openNextFile();
      continue;
    }

    // Hide admin-only folders from non-admin users
    if (hideAdminPaths) {
      String entryFullPath = (dirPath == "/") ? String("/") + fileName : dirPath + "/" + fileName;
      if (isAdminOnlyPath(entryFullPath)) {
        file = root.openNextFile();
        continue;
      }
    }

    bool isDirEntry = file.isDirectory();
    if (asJson) {
      if (!first) out += ",";
      first = false;
      if (isDirEntry) {
        // Count children in subdirectory
        String subPath = dirPath;
        if (!subPath.endsWith("/")) subPath += "/";
        subPath += fileName;
        int itemCount = 0;
        File subDir = VFS::open(subPath, "r");
        if (subDir && subDir.isDirectory()) {
          File child = subDir.openNextFile();
          while (child) {
            itemCount++;
            child = subDir.openNextFile();
          }
          subDir.close();
        }
        String folderFullPath = (dirPath == "/") ? String("/") + fileName : dirPath + "/" + fileName;
        uint8_t folderPerms = getPermissions(folderFullPath);
        char entryBuf[128];
        snprintf(entryBuf, sizeof(entryBuf), "{\"name\":\"%s\",\"type\":\"folder\",\"size\":\"%d items\",\"count\":%d,\"perms\":%d}",
                 fileName.c_str(), itemCount, itemCount, folderPerms);
        out += entryBuf;
      } else {
        String fileFullPath = (dirPath == "/") ? String("/") + fileName : dirPath + "/" + fileName;
        uint8_t filePerms = getPermissions(fileFullPath);
        char entryBuf[128];
        snprintf(entryBuf, sizeof(entryBuf), "{\"name\":\"%s\",\"type\":\"file\",\"size\":\"%lu bytes\",\"perms\":%d}",
                 fileName.c_str(), (unsigned long)file.size(), filePerms);
        out += entryBuf;
      }
    } else {
      // Human-readable text
      out += "  " + fileName + " (";
      if (isDirEntry) {
        // Count children for display
        String subPath = dirPath;
        if (!subPath.endsWith("/")) subPath += "/";
        subPath += fileName;
        int itemCount = 0;
        File subDir = VFS::open(subPath, "r");
        if (subDir && subDir.isDirectory()) {
          File child = subDir.openNextFile();
          while (child) {
            itemCount++;
            child = subDir.openNextFile();
          }
          subDir.close();
        }
        out += itemCount;
        out += " items)\n";
      } else {
        out += (unsigned long)file.size();
        out += " bytes)\n";
      }
      fileCount++;
    }

    file = root.openNextFile();
  }
  root.close();

  DEBUG_STORAGEF("[buildFilesListing] COMPLETE path='%s' fileCount=%d outLen=%d heap=%u",
                 dirPath.c_str(), fileCount, out.length(), (unsigned)ESP.getFreeHeap());

  if (!asJson) {
    if (fileCount == 0) {
      out += "  No files found\n";
    } else {
      out += "\nTotal: ";
      out += fileCount;
      out += " entries";
    }
  }
  return true;
}

// ============================================================================
// Filesystem CLI Command Handlers
// ============================================================================

const char* cmd_files(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) {
    return "Error: LittleFS not ready";
  }

  // Parse optional path argument
  String path = "/";
  String argsTrimmed = argsInput;
  argsTrimmed.trim();
  if (argsTrimmed.length() > 0) path = argsTrimmed;

  String out;
  bool ok = buildFilesListing(path, out, /*asJson=*/false);
  if (!ok) {
    broadcastOutput(out);
    return "ERROR";
  }

  broadcastOutput(out);
  return "[FS] Listing complete";
}

const char* cmd_mkdir(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: mkdir <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (!canCreate(path)) {
    snprintf(getDebugBuffer(), 1024, "Error: Creation not allowed: %s", path.c_str());
    return getDebugBuffer();
  }
  if (VFS::mkdir(path)) {
    snprintf(getDebugBuffer(), 1024, "Created folder: %s", path.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create folder: %s", path.c_str());
  }
  return getDebugBuffer();
}

const char* cmd_rmdir(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: rmdir <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (!canDelete(path)) {
    snprintf(getDebugBuffer(), 1024, "Error: Removal not allowed: %s (protected)", path.c_str());
    return getDebugBuffer();
  }
  if (VFS::rmdir(path)) {
    snprintf(getDebugBuffer(), 1024, "Removed folder: %s", path.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to remove folder (ensure it is empty): %s", path.c_str());
  }
  return getDebugBuffer();
}

const char* cmd_filecreate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: filecreate <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (path.endsWith("/")) return "Error: Path must be a file (not a directory)";
  if (!canCreate(path)) {
    snprintf(getDebugBuffer(), 1024, "Error: Creation not allowed: %s", path.c_str());
    return getDebugBuffer();
  }
  File f = VFS::open(path, "w", true);
  if (!f) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create file: %s", path.c_str());
    return getDebugBuffer();
  }
  f.close();
  snprintf(getDebugBuffer(), 1024, "Created file: %s", path.c_str());
  return getDebugBuffer();
}

const char* cmd_fileview(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: fileview <path>";
  if (!path.startsWith("/")) path = String("/") + path;

  // Security: Block reading sensitive files (credentials, passwords, keys)
  if (!canRead(path)) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Error: Access denied - %s contains sensitive data", path.c_str());
      broadcastOutput(getDebugBuffer());
    }
    return "ERROR: Access denied";
  }

  if (!VFS::exists(path)) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Error: File not found: %s", path.c_str());
      broadcastOutput(getDebugBuffer());
    }
    return "ERROR";
  }

  String content;
  if (!readText(path.c_str(), content)) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Error: Unable to open: %s", path.c_str());
      broadcastOutput(getDebugBuffer());
    }
    return "ERROR";
  }

  const size_t MAX_SHOW = 8000;
  if (content.length() > MAX_SHOW) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "--- BEGIN (truncated) %s ---", path.c_str());
      broadcastOutput(getDebugBuffer());
    }
    String truncated = content.substring(0, MAX_SHOW);
    broadcastOutput(truncated);
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "--- TRUNCATED (%u bytes total) ---", content.length());
      broadcastOutput(getDebugBuffer());
    }
  } else {
    broadcastOutput(content);
  }

  return "[FS] File displayed";
}

const char* cmd_filedelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: filedelete <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  
  if (!canDelete(path)) {
    snprintf(getDebugBuffer(), 1024, "Error: Deletion not allowed: %s (protected)", path.c_str());
    return getDebugBuffer();
  }
  
  if (!VFS::exists(path)) return "Error: File does not exist";
  if (!VFS::remove(path)) return "Error: Failed to delete file";
  snprintf(getDebugBuffer(), 1024, "Deleted file: %s", path.c_str());
  return getDebugBuffer();
}

// ============================================================================
// Filesystem Command Registry
// ============================================================================

const CommandEntry filesystemCommands[] = {
  { "files", "List files [path]", true, cmd_files,
    "files [path]        - List files in LittleFS (default '/')\n"
    "Example: files /logging_captures" },
  { "mkdir", "Create directory: <path>", true, cmd_mkdir, "Usage: mkdir <path>" },
  { "rmdir", "Remove directory: <path>", true, cmd_rmdir, "Usage: rmdir <path>" },
  { "filecreate", "Create file: <path> [content]", true, cmd_filecreate, "Usage: filecreate <path>" },
  { "fileview", "View file: <path> [offset]", true, cmd_fileview, "Usage: fileview <path>" },
  { "filedelete", "Delete file: <path>", true, cmd_filedelete, "Usage: filedelete <path>" },
};

const size_t filesystemCommandsCount = sizeof(filesystemCommands) / sizeof(filesystemCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// File Permissions — Table-Driven System
// ============================================================================
//
// All path permission rules are defined in a single table (sPathRules).
// Rules are matched in order; first match wins.  More specific paths go first.
//
// Permission flags (from System_Filesystem.h):
//   PERM_READ   0x01  — can view/download file contents
//   PERM_WRITE  0x02  — can edit file contents
//   PERM_DELETE 0x04  — can delete file/folder
//   PERM_RENAME 0x08  — can rename file/folder
//   PERM_CREATE 0x10  — can create new files/folders (CLI mkdir/filecreate)
//   PERM_IMPORT 0x20  — can upload/import files via web
//
// To add a new path rule, insert it in the table below.
// ============================================================================

struct PathRule {
  const char* path;      // Path prefix (or exact path if exactMatch is true)
  uint8_t     perms;     // Bitmask of allowed FilePermission flags
  bool        exactMatch;// true = match path exactly, false = prefix match
  bool        adminOnly; // true = requires admin role to access
};

static const PathRule sPathRules[] = {
  // ---- Sensitive credentials: no access ----
  {"/system/users/user_settings",       0,                                          false, true},
  {"/system/users/pending_users.json",  0,                                          true,  true},

  // ---- Immutable config files: read-only ----
  {"/system/settings.json",             PERM_READ,                                  true,  true},
  {"/system/automations.json",          PERM_READ,                                  true,  true},
  {"/system/devices.json",              PERM_READ,                                  true,  true},
  {"/system/espnow/devices.json",       PERM_READ,                                  true,  true},

  // ---- TLS certificates: read + delete + import (no edit/rename/create) ----
  {"/system/certs/",                    PERM_READ | PERM_DELETE | PERM_IMPORT,      false, true},

  // ---- System logs: read-only ----
  {"/system/sys_logs/",                 PERM_READ,                                  false, true},

  // ---- Protected root directories (browse only — no delete/rename) ----
  {"/system",                           PERM_READ,                                  true,  true},
  {"/logging_captures",                 PERM_READ,                                  true,  true},
  {"/espnow",                           PERM_READ,                                  true,  false},
  {"/maps",                             PERM_READ,                                  true,  false},
  {"/sd",                               PERM_READ,                                  true,  false},
  {"/Users",                            PERM_READ,                                  true,  false},

  // ---- General system paths: read-only ----
  {"/system/",                          PERM_READ,                                  false, true},

  // ---- Logging captures: read + delete ----
  {"/logging_captures/",                PERM_READ | PERM_DELETE,                    false, true},

  // ---- ESP-NOW data: read + edit + delete ----
  {"/espnow/",                          PERM_READ | PERM_WRITE | PERM_DELETE,       false, false},

  // ---- Default: full access (user data, maps, etc.) ----
  {nullptr,                             PERM_ALL,                                   false, false},
};

// Look up the first matching rule for a path
static const PathRule& lookupRule(const String& path) {
  for (size_t i = 0; i < sizeof(sPathRules) / sizeof(sPathRules[0]); i++) {
    const PathRule& rule = sPathRules[i];
    if (rule.path == nullptr) return rule;  // Default catch-all (must be last)
    if (rule.exactMatch) {
      if (path == rule.path) return rule;
    } else {
      if (path.startsWith(rule.path)) return rule;
    }
  }
  return sPathRules[sizeof(sPathRules) / sizeof(sPathRules[0]) - 1];
}

// Filename-based sensitivity check (blocks reading contents of credential files)
static bool hasSensitiveExtension(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.indexOf("password") >= 0)   return true;
  if (lower.indexOf("secret") >= 0)     return true;
  if (lower.indexOf("credential") >= 0) return true;
  if (lower.indexOf(".key") >= 0)       return true;
  if (lower.indexOf(".pem") >= 0)       return true;
  return false;
}

// Image files: can be viewed but not text-edited
static bool isImageFile(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") ||
          lower.endsWith(".gif") || lower.endsWith(".bmp") || lower.endsWith(".webp") ||
          lower.endsWith(".ico") || lower.endsWith(".avif") || lower.endsWith(".heif"));
}

// --- Public permission API (all derived from the table) ---

bool canRead(const String& path) {
  if (hasSensitiveExtension(path)) return false;
  return (lookupRule(path).perms & PERM_READ) != 0;
}

bool canEdit(const String& path) {
  if (hasSensitiveExtension(path)) return false;
  if (isImageFile(path)) return false;
  return (lookupRule(path).perms & PERM_WRITE) != 0;
}

bool canDelete(const String& path) {
  return (lookupRule(path).perms & PERM_DELETE) != 0;
}

bool canRename(const String& path) {
  return (lookupRule(path).perms & PERM_RENAME) != 0;
}

bool canCreate(const String& path) {
  return (lookupRule(path).perms & PERM_CREATE) != 0;
}

bool canImport(const String& path) {
  return (lookupRule(path).perms & PERM_IMPORT) != 0;
}

bool isAdminOnlyPath(const String& path) {
  return lookupRule(path).adminOnly;
}

uint8_t getPermissions(const String& path) {
  uint8_t perms = 0;
  if (canRead(path))   perms |= PERM_READ;
  if (canEdit(path))   perms |= PERM_WRITE;
  if (canDelete(path)) perms |= PERM_DELETE;
  if (canRename(path)) perms |= PERM_RENAME;
  if (canCreate(path)) perms |= PERM_CREATE;
  if (canImport(path)) perms |= PERM_IMPORT;
  return perms;
}

uint8_t getDirPerms(const String& dirPath) {
  // What permissions would a child of this directory have?
  String testPath = dirPath;
  if (!testPath.endsWith("/")) testPath += "/";
  testPath += "_";
  return lookupRule(testPath).perms;
}

// ============================================================================
// File I/O Helpers (moved from .ino)
// ============================================================================

bool readTextLimited(const char* path, String& out, size_t maxBytes) {
  out = "";
  FsLockGuard guard("readTextLimited");
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out.reserve(maxBytes);
  const size_t chunk = 512;
  static char* buf = nullptr;
  if (!buf) {
    buf = (char*)ps_alloc(chunk, AllocPref::PreferPSRAM, "file.read");
    if (!buf) return false;
  }
  size_t total = 0;
  while (total < maxBytes) {
    size_t toRead = maxBytes - total;
    if (toRead > chunk) toRead = chunk;
    int n = f.readBytes(buf, toRead);
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) out += buf[i];
    total += n;
  }
  f.close();
  return true;
}

bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {
  FsLockGuard guard("appendLineWithCap");
  {
    File a = LittleFS.open(path, "a");
    if (!a) return false;
    a.println(line);
    a.close();
  }

  File r = LittleFS.open(path, "r");
  if (!r) return false;
  size_t sz = r.size();
  if (sz <= capBytes) {
    r.close();
    return true;
  }
  String content = r.readString();
  r.close();

  int start = 0;
  while (content.length() > 0 && content.length() > (int)capBytes) {
    int nl = content.indexOf('\n', start);
    if (nl < 0) break;
    content.remove(0, nl + 1);
  }
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}
