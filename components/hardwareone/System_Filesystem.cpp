/**
 * Filesystem Module - LittleFS management and utilities
 * Centralized filesystem operations and state
 */

#include <LittleFS.h>

#include <esp_log.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_Logging.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_ImageManager.h"
#include "System_Maps.h"
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
extern const char* AUTOMATIONS_JSON_FILE;

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
  delay(50);  // Allow USB serial to attach before early boot logs

  // Configure LittleFS using ESP-IDF native API (bypasses Arduino wrapper issues)
  if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    ESP_LOGW("FS", "LittleFS mount failed; formatting and retrying");

    if (!LittleFS.format()) {
      ESP_LOGE("FS", "LittleFS format failed");
      filesystemReady = false;
      return false;
    }

    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
      ESP_LOGE("FS", "LittleFS mount failed after format");
      filesystemReady = false;
      return false;
    }
  }

  ESP_LOGI("FS", "LittleFS mounted successfully");
  filesystemReady = true;

  VFS::init();

  // Recover any log files left in an inconsistent state by a prior crash or
  // power-cut during rotation. Each call is cheap when no orphan exists
  // (single exists() check). Kept to the known logs that go through
  // appendLineWithCap; see System_Logging.cpp for their path definitions.
  cleanupLogOrphan(LOG_OK_FILE);
  cleanupLogOrphan(LOG_FAIL_FILE);
  cleanupLogOrphan(LOG_I2C_FILE);
  cleanupLogOrphan(LOG_ERROR_FILE);

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
#if ENABLE_ONDEVICE_LLM
  LittleFS.mkdir("/system/llm");  // LLM1 model files (tokenizer embedded)
#endif
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
  
  // Boot-time cleanup: remove orphaned .tmp files from interrupted writes
  {
    const char* cleanupDirs[] = {
        "/", "/system", "/system/users", "/system/users/user_settings", "/maps"
#if ENABLE_ONDEVICE_LLM
        , "/system/llm"
#endif
    };
    int cleaned = 0;
    for (const char* dir : cleanupDirs) {
      File d = LittleFS.open(dir);
      if (!d || !d.isDirectory()) continue;
      File entry = d.openNextFile();
      while (entry) {
        String name = entry.name();
        entry.close();
        if (name.endsWith(".tmp")) {
          String fullPath = String(dir);
          if (fullPath != "/") fullPath += "/";
          fullPath += name;
          LittleFS.remove(fullPath.c_str());
          ESP_LOGI("FS", "Cleaned orphaned temp file: %s", fullPath.c_str());
          cleaned++;
        }
        entry = d.openNextFile();
      }
      d.close();
    }
    if (cleaned > 0) {
      ESP_LOGI("FS", "Removed %d orphaned .tmp file(s)", cleaned);
    }
  }

  // Boot-time JSON validation: warn about corrupt critical config files
  {
    const char* criticalFiles[] = { "/settings.json", "/system/automations.json", "/system/users/users.json" };
    for (const char* path : criticalFiles) {
      if (!LittleFS.exists(path)) continue;
      String content;
      if (readText(path, content) && content.length() > 0) {
        content.trim();
        if (content.length() > 0 && content[0] != '{' && content[0] != '[') {
          ESP_LOGW("FS", "%s appears corrupt (not valid JSON), removing", path);
          LittleFS.remove(path);
        }
      }
    }
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
      char errBuf[96];
      snprintf(errBuf, sizeof(errBuf), "Error: Cannot open directory '%s'", dirPath.c_str());
      out = errBuf;
    }
    return false;
  }

  bool first = true;
  int fileCount = 0;
  if (!asJson) {
    char hdrBuf[96];
    snprintf(hdrBuf, sizeof(hdrBuf), "Files (%s):\n", dirPath.c_str());
    out = hdrBuf;
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
      char entryFullPath[128];
      snprintf(entryFullPath, sizeof(entryFullPath), "%s%s%s", dirPath.c_str(), dirPath == "/" ? "" : "/", fileName.c_str());
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
        // Build the full path for permission lookup using String to avoid fixed-size overflow
        String folderFullPath = dirPath;
        if (dirPath != "/") folderFullPath += "/";
        folderFullPath += fileName;
        uint8_t folderPerms = getPermissions(folderFullPath.c_str());
        // Build JSON entry directly into out — no fixed buffer, handles any filename length
        out += "{\"name\":\"";
        for (size_t ci = 0; ci < fileName.length(); ci++) {
          char c = fileName.charAt(ci);
          if (c == '"' || c == '\\') out += '\\';
          out += c;
        }
        out += "\",\"type\":\"folder\",\"size\":\"";
        out += itemCount;
        out += " items\",\"count\":";
        out += itemCount;
        out += ",\"perms\":";
        out += (int)folderPerms;
        out += "}";
      } else {
        String fileFullPath = dirPath;
        if (dirPath != "/") fileFullPath += "/";
        fileFullPath += fileName;
        uint8_t filePerms = getPermissions(fileFullPath.c_str());
        out += "{\"name\":\"";
        for (size_t ci = 0; ci < fileName.length(); ci++) {
          char c = fileName.charAt(ci);
          if (c == '"' || c == '\\') out += '\\';
          out += c;
        }
        out += "\",\"type\":\"file\",\"size\":\"";
        out += (unsigned long)file.size();
        out += " bytes\",\"perms\":";
        out += (int)filePerms;
        out += "}";
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
  if (!path.startsWith("/")) { path = "/" + path; }
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
  if (!path.startsWith("/")) { path = "/" + path; }
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
  if (!path.startsWith("/")) { path = "/" + path; }
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
  if (!path.startsWith("/")) { path = "/" + path; }

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

const char* cmd_filerename(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Usage: filerename <oldpath> <newname>";

  String oldPath = a.arg(0);
  String newName = a.arg(1);

  if (!oldPath.startsWith("/")) oldPath = "/" + oldPath;
  if (newName.length() == 0) return "Usage: filerename <oldpath> <newname>";

  int lastSlash = oldPath.lastIndexOf('/');
  String parentDir = (lastSlash > 0) ? oldPath.substring(0, lastSlash) : "";
  String newPath = parentDir + "/" + newName;

  if (!canRename(oldPath)) {
    snprintf(getDebugBuffer(), 1024, "Error: Rename not allowed: %s (protected)", oldPath.c_str());
    return getDebugBuffer();
  }
  if (!VFS::exists(oldPath)) return "Error: File does not exist";
  if (!VFS::rename(oldPath, newPath)) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to rename: %s -> %s", oldPath.c_str(), newPath.c_str());
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "Renamed: %s -> %s", oldPath.c_str(), newPath.c_str());
  return getDebugBuffer();
}

const char* cmd_filedelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = argsInput;
  path.trim();
  if (path.length() == 0) return "Usage: filedelete <path>";
  if (!path.startsWith("/")) { path = "/" + path; }
  
  if (!canDelete(path)) {
    snprintf(getDebugBuffer(), 1024, "Error: Deletion not allowed: %s (protected)", path.c_str());
    return getDebugBuffer();
  }
  
  if (!VFS::exists(path)) return "Error: File does not exist";

  // If the file to delete is the currently loaded map, unload it first to close the FD
#if ENABLE_MAPS
  if (MapCore::hasValidMap() && path == String(MapCore::getCurrentMap().filepath)) {
    MapCore::unloadMap();
  }
#endif

  if (!VFS::remove(path)) return "Error: Failed to delete file";
  snprintf(getDebugBuffer(), 1024, "Deleted file: %s", path.c_str());
  return getDebugBuffer();
}

// ============================================================================
// Filesystem Command Registry
// ============================================================================

// Print the log-overflow tier status: whether LittleFS is primary or if we've
// latched into SD overflow, plus free-space snapshots on each tier.
static const char* cmd_logtier(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;

  size_t flashFree = VFS::getCachedLittleFsFree();
  size_t flashTotal = LittleFS.totalBytes();
  size_t flashUsed = LittleFS.usedBytes();
  bool overflow = VFS::isLogOverflowActive();
  bool sdOk = VFS::isSDAvailable();
  uint64_t sdTotal = 0, sdUsed = 0, sdFree = 0;
  if (sdOk) VFS::getStats(VFS::SDCARD, sdTotal, sdUsed, sdFree);

  const char* tierStr;
  if (!overflow)        tierStr = "LittleFS primary";
  else if (sdOk)        tierStr = "SD overflow (active)";
  else                  tierStr = "SD overflow wanted — card not mounted, writes may drop";

  const char* routeStr;
  if (!overflow)        routeStr = "their primary LittleFS paths";
  else if (sdOk)        routeStr = "/sd mirror paths";
  else                  routeStr = "primary paths (data will drop if flash is full)";

  char* buf = getDebugBuffer();
  if (sdOk) {
    snprintf(buf, 1024,
      "Log storage tier: %s\n"
      "  LittleFS: %uKB used / %uKB total (%uKB free)\n"
      "  SD card:  mounted — %uKB used / %uKB total (%uKB free)\n"
      "Overflow latches on next reboot. Once latched, new log writes go to %s.",
      tierStr,
      (unsigned)(flashUsed / 1024), (unsigned)(flashTotal / 1024), (unsigned)(flashFree / 1024),
      (unsigned)(sdUsed / 1024), (unsigned)(sdTotal / 1024), (unsigned)(sdFree / 1024),
      routeStr);
  } else {
    snprintf(buf, 1024,
      "Log storage tier: %s\n"
      "  LittleFS: %uKB used / %uKB total (%uKB free)\n"
      "  SD card:  not mounted\n"
      "Overflow latches on next reboot. Once latched, new log writes go to %s.",
      tierStr,
      (unsigned)(flashUsed / 1024), (unsigned)(flashTotal / 1024), (unsigned)(flashFree / 1024),
      routeStr);
  }
  return buf;
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry filesystemCommands[] = {
  { "files", "List files [path]", true, cmd_files,
    "files [path]        - List files in LittleFS (default '/')\n"
    "Example: files /logging_captures" },
  { "mkdir", "Create directory: <path>", true, cmd_mkdir, "Usage: mkdir <path>" },
  { "rmdir", "Remove directory: <path>", true, cmd_rmdir, "Usage: rmdir <path>" },
  { "filecreate", "Create file: <path> [content]", true, cmd_filecreate, "Usage: filecreate <path>" },
  { "fileview", "View file: <path> [offset]", true, cmd_fileview, "Usage: fileview <path>" },
  { "filedelete", "Delete file: <path>", true, cmd_filedelete, "Usage: filedelete <path>" },
  { "filerename", "Rename file: <oldpath> <newname>", true, cmd_filerename, "Usage: filerename <oldpath> <newname>" },
  { "logtier", "Show current log storage tier (LittleFS vs SD overflow).", false, cmd_logtier,
    "logtier             - Report which tier logs are writing to and free space on each." },
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

// Columns: path (prefix or exact), perms (PERM_* bitmask), exactMatch, adminOnly
static const PathRule sPathRules[] = {
  // ---- Sensitive credentials: no access ----
  {"/system/users/user_settings",       0,                                          false, true},
  {"/system/users/pending_users.json",  0,                                          true,  true},

  // ---- Immutable config files: read-only ----
  {"/system/settings.json",             PERM_READ,                                  true,  true},
  {"/system/automations.json",          PERM_READ,                                  true,  true},
  {"/system/espnow/devices.json",       PERM_READ,                                  true,  true},

  // ---- TLS certificates: read + delete + import (no edit/rename/create) ----
  {"/system/certs/",                    PERM_READ | PERM_DELETE | PERM_IMPORT,      false, true},


  // ---- On-device LLM: LLM1 model files (same policy as certs) ----
  {"/system/llm/",                      PERM_READ | PERM_DELETE | PERM_IMPORT,      false, true},


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

// Log-overflow tiering implementation moved to System_VFS.cpp alongside the
// rest of the VFS dispatcher. See VFS::resolveOverflowPath,
// VFS::isLogOverflowActive, VFS::getCachedLittleFsFree.

// Streaming + hysteresis rotation.
//
// Previously this function slurped the whole file into a String and called
// String::remove(0, n) in a loop to trim old lines — O(N²) memmove, which
// under a flood of [ERROR] writes ate seconds of fs-lock time and once
// crashed the debug_out task via a stack overflow.
//
// Now:
//   • Rotate trigger: when the file reaches capBytes (a hard ceiling). File
//     size never exceeds cap, matching the semantic the cap name implies.
//   • Trim target:    capBytes * 85/100. Dropping to ~85% gives ~15% of
//     the cap as write room between rotations, so rotations amortize over
//     many appends instead of firing on every line past cap.
//   • Method: stream the surviving tail into a sibling ".tmp" file in 1 KB
//     chunks, then rename over the original. O(N) work, ~1 KB stack buf,
//     no large heap String allocations.
//
// Crash safety: between removing the original and renaming the .tmp in,
// there is a small window where the log file is missing. `cleanupLogOrphan`
// (called at boot) recovers this: if dest is missing but dest.tmp exists,
// it promotes dest.tmp to dest.
bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {
  STACK_TRACEF("appendLineWithCap.enter path=%s line_len=%u cap=%u",
               path ? path : "(null)", (unsigned)line.length(), (unsigned)capBytes);

  FsLockGuard guard("appendLineWithCap");  // reentrant-safe; nested VFS calls are fine.

  // Resolve destination (LittleFS primary, or /sd overflow mirror if flash full).
  char dest[128];
  VFS::resolveOverflowPath(path, line.length() + 512, dest, sizeof(dest));
  STACK_TRACEF("appendLineWithCap.resolved dest=%s", dest);

  // 1. Append the new line — fast path, same as before.
  {
    File a = VFS::open(String(dest), "a", true);
    if (!a) { STACK_TRACEF("appendLineWithCap.open_a_failed"); return false; }
    STACK_TRACEF("appendLineWithCap.opened_for_append");
    a.println(line);
    STACK_TRACEF("appendLineWithCap.println_done");
    a.close();
    STACK_TRACEF("appendLineWithCap.close_a_done");
  }

  // Tell the VFS free-space cache how much data we just added (approximate —
  // LittleFS overhead isn't counted). Lets the overflow decision refresh on
  // the next check if enough cumulative writes have happened, instead of
  // trusting a stale 2s-old reading through a large write burst.
  VFS::noteLittleFsBytesWritten(line.length() + 2);  // +2 for CRLF

  // 2. Check size. Rotate only if we've reached the hard cap.
  File r = VFS::open(String(dest), "r");
  if (!r) { STACK_TRACEF("appendLineWithCap.open_r_failed"); return false; }
  size_t sz = r.size();
  STACK_TRACEF("appendLineWithCap.size_checked sz=%u cap=%u", (unsigned)sz, (unsigned)capBytes);
  if (sz <= capBytes) { r.close(); STACK_TRACEF("appendLineWithCap.no_rotate_exit"); return true; }

  STACK_TRACEF("appendLineWithCap.rotate_begin");
  // 3. Compute trim math. Drop enough bytes that the survivors are ~85% of cap.
  const size_t TARGET_SIZE = (capBytes * 85) / 100;
  if (TARGET_SIZE >= sz) { r.close(); return true; }  // defensive; shouldn't happen
  const size_t bytesToDrop = sz - TARGET_SIZE;

  // 4. Seek to the first byte we intend to keep, then advance to the next
  //    newline so we don't begin mid-line. Bounded scan: if no newline
  //    within 4 KB, give up and accept the partial leading line (matches
  //    old behaviour where a single line > cap was left alone).
  if (!r.seek(bytesToDrop)) { r.close(); return false; }
  const size_t MAX_SCAN = 4096;
  for (size_t scanned = 0; scanned < MAX_SCAN; scanned++) {
    int c = r.read();
    if (c < 0) { r.close(); return true; }   // hit EOF mid-scan — nothing to keep
    if (c == '\n') break;                    // cursor is now past the \n
  }

  // 5. Stream the tail into a temp file. The copy buffer lives in PSRAM and
  //    is allocated once per boot, then reused — keeping it off the debug_out
  //    task's stack. LittleFS internals already use several KB of stack in
  //    deep write paths, and a 1 KB automatic here was enough to overflow
  //    the task during a first-of-session error-log write.
  char tmpPath[140];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", dest);
  VFS::remove(String(tmpPath));  // clear any stale .tmp from a prior crash

  static uint8_t* sRotateBuf = nullptr;
  static const size_t ROTATE_BUF_SIZE = 1024;
  if (!sRotateBuf) {
    sRotateBuf = (uint8_t*)ps_alloc(ROTATE_BUF_SIZE, AllocPref::PreferPSRAM, "logrot.buf");
    if (!sRotateBuf) { r.close(); STACK_TRACEF("appendLineWithCap.rotatebuf_alloc_failed"); return false; }
  }

  File w = VFS::open(String(tmpPath), "w", true);
  if (!w) { r.close(); STACK_TRACEF("appendLineWithCap.open_tmp_failed"); return false; }
  STACK_TRACEF("appendLineWithCap.tmp_opened copying...");

  size_t copied = 0;
  while (true) {
    int n = r.readBytes((char*)sRotateBuf, ROTATE_BUF_SIZE);
    if (n <= 0) break;
    w.write(sRotateBuf, (size_t)n);
    copied += n;
  }
  r.close();
  w.close();
  STACK_TRACEF("appendLineWithCap.copy_done bytes=%u", (unsigned)copied);

  // 6. Atomic-ish swap. LittleFS and SD both require the destination to be
  //    absent before rename, so remove + rename. The vulnerable window is
  //    recovered by cleanupLogOrphan on next boot.
  if (!VFS::remove(String(dest))) {
    VFS::remove(String(tmpPath));
    STACK_TRACEF("appendLineWithCap.remove_orig_failed");
    return false;
  }
  STACK_TRACEF("appendLineWithCap.removed_orig");
  if (!VFS::rename(String(tmpPath), String(dest))) {
    STACK_TRACEF("appendLineWithCap.rename_failed — orphan .tmp left for boot recovery");
    return false;
  }
  STACK_TRACEF("appendLineWithCap.rotate_complete");
  return true;
}

// Recover a log file's partner .tmp left over from a crashed or power-cut
// rotation. Three possible on-disk states after a crash:
//
//   a. dest present, dest.tmp absent  → normal; nothing to do.
//   b. dest present, dest.tmp present → crash between tmp-write and rename;
//                                       .tmp is stale, delete it.
//   c. dest absent,  dest.tmp present → crash between remove(dest) and
//                                       rename(tmp→dest); promote .tmp.
//
// Case (c) is the only data-preserving recovery. Case (b) is cleanup.
void cleanupLogOrphan(const char* path) {
  if (!path) return;

  char dest[128];
  // Use the same overflow-aware resolution so we look at the actual location
  // writes land in. Pass 0 for requiredSpace — we're not writing.
  VFS::resolveOverflowPath(path, 0, dest, sizeof(dest));

  char tmpPath[140];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", dest);

  if (!VFS::exists(String(tmpPath))) return;

  if (VFS::exists(String(dest))) {
    // Case (b): stale temp, destination is authoritative.
    VFS::remove(String(tmpPath));
  } else {
    // Case (c): promote the temp. Logs what it did via Serial since our
    // own logging infrastructure may depend on this path being valid.
    if (VFS::rename(String(tmpPath), String(dest))) {
      Serial.printf("[FS] Recovered log file from partial rotation: %s\n", dest);
    } else {
      // Rename failed — delete the orphan so we don't try again next boot.
      VFS::remove(String(tmpPath));
      Serial.printf("[FS] Could not recover %s from .tmp; discarded orphan\n", dest);
    }
  }
}
