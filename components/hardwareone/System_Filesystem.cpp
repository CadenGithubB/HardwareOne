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
  
#if ENABLE_CAMERA_SENSOR
  // Initialize ImageManager now that filesystem is ready (creates photos folder)
  gImageManager.init();
#endif
  
  // Ensure system directories exist
  LittleFS.mkdir("/logs");
  LittleFS.mkdir("/system");  // For settings, automations, devices, etc.
  LittleFS.mkdir("/system/users");  // For users.json and user settings
  LittleFS.mkdir("/system/users/user_settings");  // For per-user setting files
  LittleFS.mkdir("/espnow");  // For ESP-NOW related files
  LittleFS.mkdir("/espnow/received");  // For received files from ESP-NOW devices
  LittleFS.mkdir("/maps");  // For GPS map files (.hwmap)
  
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

bool buildFilesListing(const String& inPath, String& out, bool asJson) {
  String dirPath = inPath;
  if (dirPath.length() == 0) dirPath = "/";
  if (!dirPath.startsWith("/")) dirPath = String("/") + dirPath;

  DEBUG_STORAGEF("[buildFilesListing] START path='%s' heap=%u", dirPath.c_str(), (unsigned)ESP.getFreeHeap());

  FsLockGuard _dirGuard("dir.list");
  File root = LittleFS.open(dirPath);
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
    out = String("LittleFS Files (") + dirPath + "):\n";
  } else {
    out = "";  // array body only
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
        File subDir = LittleFS.open(subPath);
        if (subDir && subDir.isDirectory()) {
          File child = subDir.openNextFile();
          while (child) {
            itemCount++;
            child = subDir.openNextFile();
          }
          subDir.close();
        }
        out += String("{\"name\":\"") + fileName + "\",";
        out += String("\"type\":\"folder\",");
        out += String("\"size\":\"") + String(itemCount) + " items\",";
        out += String("\"count\":") + String(itemCount) + "}";
      } else {
        out += String("{\"name\":\"") + fileName + "\",";
        out += String("\"type\":\"file\",");
        out += String("\"size\":\"") + String(file.size()) + " bytes\"}";
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
        File subDir = LittleFS.open(subPath);
        if (subDir && subDir.isDirectory()) {
          File child = subDir.openNextFile();
          while (child) {
            itemCount++;
            child = subDir.openNextFile();
          }
          subDir.close();
        }
        out += String(itemCount) + " items)\n";
      } else {
        out += String(file.size()) + " bytes)\n";
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
      out += String("\nTotal: ") + String(fileCount) + " entries";
    }
  }
  return true;
}

// ============================================================================
// Filesystem CLI Command Handlers
// ============================================================================

// External dependencies
extern bool ensureDebugBuffer();
extern void broadcastOutput(const String& msg);

// Validation macro
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

const char* cmd_files(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) {
    return "Error: LittleFS not ready";
  }

  // Parse optional path argument
  String path = "/";
  String argsTrimmed = args;
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

const char* cmd_mkdir(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = args;
  path.trim();
  if (path.length() == 0) return "Usage: mkdir <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (path == "/logs" || path.startsWith("/logs/") || path == "/system" || path.startsWith("/system/")) {
    snprintf(getDebugBuffer(), 1024, "Error: Creation not allowed: %s", path.c_str());
    return getDebugBuffer();
  }
  if (LittleFS.mkdir(path)) {
    snprintf(getDebugBuffer(), 1024, "Created folder: %s", path.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create folder: %s", path.c_str());
  }
  return getDebugBuffer();
}

const char* cmd_rmdir(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = args;
  path.trim();
  if (path.length() == 0) return "Usage: rmdir <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (path == "/logs" || path.startsWith("/logs/") || path == "/system" || path.startsWith("/system/") || path == "/Users" || path.startsWith("/Users/")) {
    snprintf(getDebugBuffer(), 1024, "Error: Removal not allowed: %s (protected system directory)", path.c_str());
    return getDebugBuffer();
  }
  if (LittleFS.rmdir(path)) {
    snprintf(getDebugBuffer(), 1024, "Removed folder: %s", path.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to remove folder (ensure it is empty): %s", path.c_str());
  }
  return getDebugBuffer();
}

const char* cmd_filecreate(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = args;
  path.trim();
  if (path.length() == 0) return "Usage: filecreate <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  if (path.endsWith("/")) return "Error: Path must be a file (not a directory)";
  if (path == "/logs" || path.startsWith("/logs/") || path == "/system" || path.startsWith("/system/")) {
    snprintf(getDebugBuffer(), 1024, "Error: Creation not allowed: %s", path.c_str());
    return getDebugBuffer();
  }
  File f = LittleFS.open(path, "w");
  if (!f) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create file: %s", path.c_str());
    return getDebugBuffer();
  }
  f.close();
  snprintf(getDebugBuffer(), 1024, "Created file: %s", path.c_str());
  return getDebugBuffer();
}

const char* cmd_fileview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  String path = args;
  path.trim();
  if (path.length() == 0) return "Usage: fileview <path>";
  if (!path.startsWith("/")) path = String("/") + path;

  if (!LittleFS.exists(path)) {
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

const char* cmd_filedelete(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  String path = args;
  path.trim();
  if (path.length() == 0) return "Usage: filedelete <path>";
  if (!path.startsWith("/")) path = String("/") + path;
  
  if (path == "/system/settings.json" || path == "/system/automations.json" || 
      path == "/system/pending_users.json" || path.startsWith("/system/")) {
    snprintf(getDebugBuffer(), 1024, "Error: Deletion not allowed: %s (protected system file)", path.c_str());
    return getDebugBuffer();
  }
  
  if (path.startsWith("/logs/")) {
    snprintf(getDebugBuffer(), 1024, "Error: Deletion not allowed: %s (protected log file)", path.c_str());
    return getDebugBuffer();
  }
  
  if (!LittleFS.exists(path)) return "Error: File does not exist";
  if (!LittleFS.remove(path)) return "Error: Failed to delete file";
  snprintf(getDebugBuffer(), 1024, "Deleted file: %s", path.c_str());
  return getDebugBuffer();
}

// ============================================================================
// Filesystem Command Registry
// ============================================================================

const CommandEntry filesystemCommands[] = {
  { "files", "List/inspect files.", false, cmd_files,
    "files [path]        - List files in LittleFS (default '/')\n"
    "Example: files /logs" },
  { "mkdir", "Create directory in LittleFS.", true, cmd_mkdir, "Usage: mkdir <path>" },
  { "rmdir", "Remove directory in LittleFS.", true, cmd_rmdir, "Usage: rmdir <path>" },
  { "filecreate", "Create a file (optionally with content).", true, cmd_filecreate, "Usage: filecreate <path>" },
  { "fileview", "View a file (supports offsets).", false, cmd_fileview, "Usage: fileview <path>" },
  { "filedelete", "Delete a file.", true, cmd_filedelete, "Usage: filedelete <path>" },
};

const size_t filesystemCommandsCount = sizeof(filesystemCommands) / sizeof(filesystemCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _filesystem_cmd_registrar(filesystemCommands, filesystemCommandsCount, "filesystem");

// ============================================================================
// File Permissions and Protection
// ============================================================================

bool canDelete(const String& path) {
  // Protected system directories
  if (path == "/logs" || path == "/system" || path == "/espnow" || path == "/Users") {
    return false;
  }
  
  // Protected system files
  if (path.startsWith("/system/") || path.startsWith("/Users/")) {
    return false;
  }
  
  // Protected log files
  if (path.startsWith("/logs/")) {
    return false;
  }
  
  return true;
}

bool canEdit(const String& path) {
  // Protected system files
  if (path.startsWith("/system/") || path.startsWith("/Users/")) {
    return false;
  }
  
  // Protected log files
  if (path.startsWith("/logs/")) {
    return false;
  }
  
  // Image files cannot be edited (view-only)
  if (path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".png") || 
      path.endsWith(".gif") || path.endsWith(".bmp") || path.endsWith(".webp") ||
      path.endsWith(".ico") || path.endsWith(".avif") || path.endsWith(".heif")) {
    return false;
  }
  
  return true;
}

bool canCreate(const String& path) {
  // Cannot create in protected directories
  if (path == "/logs" || path.startsWith("/logs/")) {
    return false;
  }
  
  if (path == "/system" || path.startsWith("/system/") || path == "/Users" || path.startsWith("/Users/")) {
    return false;
  }
  
  return true;
}

uint8_t getPermissions(const String& path) {
  uint8_t perms = PERM_READ;  // All files are readable
  
  if (canEdit(path)) {
    perms |= PERM_WRITE;
  }
  
  if (canDelete(path)) {
    perms |= PERM_DELETE;
  }
  
  return perms;
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
