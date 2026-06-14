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
#include "System_BuildConfig.h"
#include "System_ImageManager.h"
#include "System_Maps.h"
#include "System_VFS.h"
#include "System_AuthIdentity.h"  // currentAuthContext — CLI handlers' per-task identity
#include "System_CLIConfirm.h"    // cliRequestConfirm — yes/no gate for destructive filedelete

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

#if ENABLE_G2_GLASSES
  if (VFS::isSDAvailable()) {
    (void)VFS::mkdirGuarded(String(G2_ICON_ANIMATIONS_VFS_PATH), VFS::systemAuth("filesystem.g2_icon_anim_init"));
  }
#endif

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
  
  // Ensure system directories exist. All run as system — boot init is the
  // canonical trusted-internal context (no user identity exists yet).
  {
    AuthContext sys = VFS::systemAuth("fs.init.mkdirs");
    VFS::mkdirGuarded("/logging_captures",            sys);
    VFS::mkdirGuarded("/system",                      sys);  // settings, automations, devices, etc.
    VFS::mkdirGuarded("/system/sys_logs",             sys);  // protected system logs
    VFS::mkdirGuarded("/system/users",                sys);  // users.json + per-user settings dir
    VFS::mkdirGuarded("/system/users/user_settings",  sys);  // per-user setting files
    VFS::mkdirGuarded("/system/certs",                sys);  // TLS certs (HTTPS, MQTT)
#if ENABLE_ONDEVICE_LLM
    VFS::mkdirGuarded("/system/llm",                  sys);  // LLM1 model files
#endif
#if ENABLE_ESPNOW
    VFS::mkdirGuarded("/espnow",                      sys);  // ESP-NOW related files
    VFS::mkdirGuarded("/system/espnow",               sys);  // ESP-NOW config (mesh peers, devices)
    VFS::mkdirGuarded("/system/espnow/peers",         sys);  // per-peer cached settings
    VFS::mkdirGuarded("/system/espnow/this_device",   sys);  // this device's own bond-self files (temp _schema_out.json today; future: identity/devices/mesh)
#endif
#if ENABLE_MAPS
    VFS::mkdirGuarded("/maps",                        sys);  // GPS map files (.hwmap)
#endif
  }

  // Boot-time cleanup: remove orphaned .tmp files from interrupted writes
  {
    AuthContext sys = VFS::systemAuth("fs.init.tmp_cleanup");
    const char* cleanupDirs[] = {
        "/", "/system", "/system/users", "/system/users/user_settings", "/maps"
#if ENABLE_ONDEVICE_LLM
        , "/system/llm"
#endif
    };
    int cleaned = 0;
    for (const char* dir : cleanupDirs) {
      File d = VFS::openGuarded(dir, "r", sys);
      if (!d || !d.isDirectory()) continue;
      File entry = d.openNextFile();
      while (entry) {
        String name = entry.name();
        entry.close();
        if (name.endsWith(".tmp")) {
          String fullPath = String(dir);
          if (fullPath != "/") fullPath += "/";
          fullPath += name;
          VFS::removeGuarded(fullPath.c_str(), sys);
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
    AuthContext sys = VFS::systemAuth("fs.init.json_validate");
    const char* criticalFiles[] = { "/settings.json", "/system/automations.json", "/system/users/users.json" };
    for (const char* path : criticalFiles) {
      if (!VFS::existsGuarded(path, sys)) continue;
      String content;
      if (readText(path, content) && content.length() > 0) {
        content.trim();
        if (content.length() > 0 && content[0] != '{' && content[0] != '[') {
          ESP_LOGW("FS", "%s appears corrupt (not valid JSON), removing", path);
          VFS::removeGuarded(path, sys);
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
  DebugFlagMask _dbgSaved = getDebugFlags();
  setDebugFlag(DEBUG_AUTO_SCHEDULER);
  if (gSettings.automationsEnabled && VFS::existsGuarded(AUTOMATIONS_JSON_FILE, VFS::systemAuth("fs.init.automations_check"))) {
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

bool buildFilesListing(const String& inPath, String& out, bool asJson, const AuthContext& ctx, bool hideAdminPaths) {
  String dirPath = VFS::normalize(inPath);

  DEBUG_STORAGEF("[buildFilesListing] START path='%s' heap=%u", dirPath.c_str(), (unsigned)ESP.getFreeHeap());

  // SD entry names come back rooted at the SD's own "/", not at our
  // "/sd/..." mount-point convention. Compute the underlying-FS path
  // once so we can strip the right prefix from each entry name below.
  // For LittleFS paths stripSdPrefix is a pass-through.
  String fsDirPath = VFS::stripSdPrefix(dirPath);

  FsLockGuard _dirGuard("dir.list");

  File root = VFS::openGuarded(dirPath, "r", ctx);
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

  // Mount-point entries (e.g. /sd at LittleFS root). The VFS layer is
  // the authority on which synthetic entries belong here; we render each
  // one with the same JSON / text formatting the real entries use below.
  // Item-count display is web-listing polish so it lives here, not in VFS.
  {
    VFS::VirtualEntry virtuals[4];
    const size_t nVirt = VFS::listVirtualEntries(
        dirPath, virtuals, sizeof(virtuals) / sizeof(virtuals[0]));
    for (size_t v = 0; v < nVirt; v++) {
      char fullPath[160];
      snprintf(fullPath, sizeof(fullPath), "%s%s%s",
               dirPath.c_str(), dirPath == "/" ? "" : "/", virtuals[v].name);
      uint32_t childCount = 0;
      if (virtuals[v].isFolder) {
        File mount = VFS::openGuarded(fullPath, "r", ctx);
        if (mount && mount.isDirectory()) {
          File child = mount.openNextFile();
          while (child) { childCount++; child = mount.openNextFile(); }
          mount.close();
        }
      }
      if (asJson) {
        uint8_t perms = getPermissions(String(fullPath), ctx);
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"name\":\"%s\",\"type\":\"folder\",\"size\":\"%u items\",\"count\":%u,\"perms\":%u}",
                 virtuals[v].name, (unsigned)childCount, (unsigned)childCount, (unsigned)perms);
        out += buf;
        first = false;
      } else {
        out += "  ";
        out += virtuals[v].name;
        out += " (";
        out += childCount;
        out += " items) [mount]\n";
        fileCount++;
      }
    }
  }

  File file = root.openNextFile();
  while (file) {
    // Extract display name (strip leading directory)
    String fileName = String(file.name());
    if (fsDirPath != "/") {
      String expectedPrefix = fsDirPath;
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
        File subDir = VFS::openGuarded(subPath, "r", ctx);
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
        uint8_t folderPerms = getPermissions(folderFullPath, ctx);
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
        uint8_t filePerms = getPermissions(fileFullPath, ctx);
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
        File subDir = VFS::openGuarded(subPath, "r", ctx);
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

// Append `len` bytes of `data` to `out` as the body of a JSON string (no
// surrounding quotes), escaping per RFC 8259. Callers only route printable
// ASCII through here; binary / high-bit content goes out base64 instead.
static void appendJsonStringBytes(String& out, const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += hex[(c >> 4) & 0xF];
          out += hex[c & 0xF];
        } else {
          out += (char)c;
        }
    }
  }
}

// True if the buffer holds bytes that can't sit in a UTF-8 JSON string as-is
// (NUL, high-bit, or a control char other than tab/newline/CR). Such content is
// base64-encoded instead so the `enc` field tells the app how to decode.
static bool bytesNeedBase64(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c == 0 || c >= 0x80) return true;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return true;
  }
  return false;
}

// Build the storage-stats JSON ({total,used,free,usagePercent}) for the tier
// that owns `path`. Shared by the web /api/files/stats handler and the
// `files stats json` CLI command so both report identical numbers.
void buildFilesStatsJson(const String& path, char* out, size_t outSize) {
  VFS::StorageType st = VFS::getStorageType(path);
  if (st == VFS::SDCARD && !VFS::isSDAvailable()) {
    snprintf(out, outSize, "{\"success\":false,\"error\":\"SD card not available\"}");
    return;
  }
  uint64_t total = 0, used = 0, freeBytes = 0;
  VFS::getStats(st, total, used, freeBytes);
  int usagePercent = (total == 0) ? 0 : (int)((used * 100) / total);
  snprintf(out, outSize,
           "{\"success\":true,\"total\":%llu,\"used\":%llu,\"free\":%llu,\"usagePercent\":%d}",
           (unsigned long long)total, (unsigned long long)used,
           (unsigned long long)freeBytes, usagePercent);
}

// Build the COMPLETE JSON listing envelope ({success,dirPerms,files:[...]}) for
// `path` under `ctx`. Single source of truth for both the web /api/files/list
// handler and the `files json` CLI/BLE command so the shape can't drift. The
// transport-specific bits (HTTP 403, BLE static buffer) stay in the callers.
bool buildFilesListJson(const String& path, const AuthContext& ctx, bool hideAdminPaths, String& out) {
  String body;
  bool ok = buildFilesListing(path, body, /*asJson=*/true, ctx, hideAdminPaths);
  if (!ok) {
    out = "{\"success\":false,\"error\":\"Directory not found or not accessible\"}";
    return false;
  }
  uint8_t dp = getDirPerms(path, ctx);
  out  = "{\"success\":true,\"dirPerms\":";
  out += (int)dp;
  out += ",\"files\":[";
  out += body;
  out += "]}";
  return true;
}

// Post-save hook shared by every file-write path (web write, web upload, BLE
// filewrite): keep automations.json free of duplicate IDs and flag the scheduler.
// Safe to call for any path — no-ops unless the path is automations.json.
void runFileWritePostSaveHooks(const String& path) {
#if ENABLE_AUTOMATION
  if (path == "/system/automations.json") {
    String json;
    if (readText(AUTOMATIONS_JSON_FILE, json) && sanitizeAutomationsJson(json)) {
      writeAutomationsJsonAtomic(json);  // best-effort atomic writeback
      gAutosDirty = true;                // ensure scheduler refreshes
    }
  }
#else
  (void)path;
#endif
}

// `files json` wrapper: applies the BLE/CLI-specific admin-only pre-check, then
// defers to the shared buildFilesListJson(). Returns a pointer to a static
// buffer (valid until the next call), per the command-return contract.
static const char* filesListingJsonForApp(const String& path) {
  static String s_listJson;
  const AuthContext& ctx = currentAuthContext();
  bool admin = isAdminUser(ctx.user);
  if (isAdminOnlyPath(path) && !admin) {
    s_listJson = "{\"success\":false,\"error\":\"Admin required\"}";
    return s_listJson.c_str();
  }
  buildFilesListJson(path, ctx, /*hideAdminPaths=*/!admin, s_listJson);
  return s_listJson.c_str();
}

const char* cmd_files(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) {
    return "Error: LittleFS not ready";
  }

  // Grammar:  files [stats] [json] ["<path>"]
  // `stats`/`json` are BARE flag tokens; the optional path, when present, must
  // be a quoted token (so a folder literally named "json" — quoted — is read as
  // a path, not the flag). `files stats` is always JSON; a following `json` is
  // accepted for symmetry and ignored.
  CommandArgs a(argsInput);
  int idx = 0;
  bool wantStats = (a.has(idx) && !a.argWasQuoted(idx) && a.arg(idx) == "stats");
  if (wantStats) idx++;
  bool wantJson  = (a.has(idx) && !a.argWasQuoted(idx) && a.arg(idx) == "json");
  if (wantJson) idx++;

  String path = "/";
  if (a.has(idx)) {
    const char* qerr = requireQuotedPath(a, idx, path);
    if (qerr) return qerr;
    idx++;
  }
  if (a.has(idx))
    return "Error: unexpected argument — quote the path, e.g. files \"/My Folder\"";

  if (wantStats) {
    static char statsBuf[160];
    buildFilesStatsJson(path, statsBuf, sizeof(statsBuf));
    return statsBuf;
  }
  if (wantJson) {
    return filesListingJsonForApp(path);
  }

  // Legacy human-readable listing (serial console).
  String out;
  bool ok = buildFilesListing(path, out, /*asJson=*/false, currentAuthContext());
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
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: mkdir \"<path>\"";
  // Phase 4: read the dispatch-time AuthContext explicitly. Each transport
  // (web/serial/BT/internal) sets this before executeCommand fires.
  const AuthContext& ctx = currentAuthContext();
  // mkdirGuarded folds normalize + canCreate(path, ctx) + dispatch into one
  // call. The previous canCreate(path) shim relied on the same global but
  // didn't normalize ".." and skipped the explicit role-aware check.
  if (!VFS::mkdirGuarded(path, ctx)) {
    // mkdir is idempotent for this user-facing command: an already-existing
    // directory is success, not an error. LittleFS.mkdir() returns false on
    // EEXIST, which the catch-all message below previously mislabeled as
    // "(denied)" — e.g. the web file browser pre-creates a download folder
    // (level-by-level, since mkdir isn't recursive) that an earlier
    // bond/file-receive already made. Probe the path (READ-gated via
    // openGuarded) to tell the cases apart: existing dir -> OK; a file is in
    // the way -> specific error; otherwise -> the real create failure.
    File existing = VFS::openGuarded(path, "r", ctx);
    if (existing) {
      const bool isDir = existing.isDirectory();
      existing.close();
      if (isDir) {
        // No "Error:"/"Usage:"/"Invalid" prefix => treated as success by the
        // command framework (System_Command.cpp), so callers like the web
        // download flow don't see a spurious FAIL.
        snprintf(getDebugBuffer(), 1024, "Folder already exists: %s", path.c_str());
        return getDebugBuffer();
      }
      snprintf(getDebugBuffer(), 1024,
               "Error: Cannot create folder — a file already exists at that path: %s",
               path.c_str());
      return getDebugBuffer();
    }
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create folder (denied or fs error): %s", path.c_str());
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "Created folder: %s", path.c_str());
  return getDebugBuffer();
}

const char* cmd_rmdir(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: rmdir \"<path>\"";
  const AuthContext& ctx = currentAuthContext();
  if (!VFS::rmdirGuarded(path, ctx)) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to remove folder (denied, not empty, or fs error): %s", path.c_str());
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "Removed folder: %s", path.c_str());
  return getDebugBuffer();
}

const char* cmd_filecreate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: filecreate \"<path>\"";
  if (path.endsWith("/")) return "Error: Path must be a file (not a directory)";
  const AuthContext& ctx = currentAuthContext();
  // openGuarded("w", create=true) on a non-existent path falls back to
  // canCreate(path, ctx) when canEdit denies (per the openGuarded logic).
  File f = VFS::openGuarded(path, "w", ctx, /*create=*/true);
  if (!f) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to create file (denied or fs error): %s", path.c_str());
    return getDebugBuffer();
  }
  f.close();
  snprintf(getDebugBuffer(), 1024, "Created file: %s", path.c_str());
  return getDebugBuffer();
}

const char* cmd_fileview(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: fileview \"<path>\"";

  const AuthContext& ctx = currentAuthContext();
  // existsGuarded gates by canRead — combines the previous canRead +
  // VFS::exists pair into one decision. If it denies we get the same
  // "not found" UX, with the actual reason in the [PERM] log.
  if (!VFS::existsGuarded(path, ctx)) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Error: File not found or access denied: %s", path.c_str());
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
  String oldPath;
  const char* qerr = requireQuotedPath(a, 0, oldPath);
  if (qerr) return qerr;
  // newName is a bare filename, not a full path — require it quoted (so spaces
  // are kept) but DON'T force a leading slash the way requireQuotedPath does.
  if (!a.has(1) || !a.argWasQuoted(1))
    return "Error: new name must be in quotes — filerename \"<oldpath>\" \"<newname>\"";
  String newName = a.arg(1);
  if (newName.length() == 0) return "Error: new name is empty";
  if (a.has(2))
    return "Error: unexpected argument — filerename \"<oldpath>\" \"<newname>\"";

  int lastSlash = oldPath.lastIndexOf('/');
  String parentDir = (lastSlash > 0) ? oldPath.substring(0, lastSlash) : "";
  String newPath = parentDir + "/" + newName;

  const AuthContext& ctx = currentAuthContext();
  // existsGuarded gates by canRead — must be readable to be checked for
  // existence. renameGuarded then checks RENAME on src AND CREATE on dst.
  if (!VFS::existsGuarded(oldPath, ctx)) return "Error: File does not exist or access denied";
  if (!VFS::renameGuarded(oldPath, newPath, ctx)) {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to rename (denied or fs error): %s -> %s", oldPath.c_str(), newPath.c_str());
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024, "Renamed: %s -> %s", oldPath.c_str(), newPath.c_str());
  return getDebugBuffer();
}

// Chunked, permission-guarded file read for the companion app. The web browser
// streams bytes over HTTP; BLE can't, so the app pulls a file in bounded windows
// by looping on `offset` until `eof`. Returns a JSON envelope; binary / non-ASCII
// content (or an explicit `b64` arg) is base64-encoded and flagged via `enc`.
const char* cmd_fileread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  static String s_readJson;
  CommandArgs a(argsInput);
  String path;
  if (requireQuotedPath(a, 0, path) != nullptr)
    return "{\"success\":false,\"error\":\"path must be a quoted token\"}";
  long offset = a.has(1) ? a.argInt(1, 0) : 0;
  long reqLen = a.has(2) ? a.argInt(2, 0) : 0;
  bool forceB64 = false;
  for (int i = 3; i < a.count(); i++) if (a.arg(i) == "b64") forceB64 = true;

  const size_t MAX_CHUNK = 4096;  // bound per-call memory + reply size
  const AuthContext& ctx = currentAuthContext();

  FsLockGuard _g("fileread");
  File f = VFS::openGuarded(path, "r", ctx);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    s_readJson = "{\"success\":false,\"error\":\"Not found or access denied\"}";
    return s_readJson.c_str();
  }
  size_t total = f.size();

  if (offset < 0) offset = 0;
  if ((size_t)offset > total) offset = (long)total;
  size_t avail = total - (size_t)offset;
  size_t want = (reqLen <= 0) ? MAX_CHUNK : (size_t)reqLen;
  if (want > MAX_CHUNK) want = MAX_CHUNK;
  if (want > avail)     want = avail;

  uint8_t* buf = nullptr;
  size_t got = 0;
  if (want > 0) {
    buf = (uint8_t*)ps_alloc(want, AllocPref::PreferPSRAM, "fileread.chunk");
    if (!buf) {
      f.close();
      s_readJson = "{\"success\":false,\"error\":\"OOM\"}";
      return s_readJson.c_str();
    }
    if (offset > 0) f.seek((uint32_t)offset);
    got = f.read(buf, want);
  }
  f.close();

  bool eof = ((size_t)offset + got >= total);
  bool useB64 = forceB64 || (buf && bytesNeedBase64(buf, got));

  s_readJson  = "{\"success\":true,\"path\":\"";
  appendJsonStringBytes(s_readJson, (const uint8_t*)path.c_str(), path.length());
  s_readJson += "\",\"size\":"; s_readJson += (unsigned long)total;
  s_readJson += ",\"offset\":"; s_readJson += (unsigned long)offset;
  s_readJson += ",\"len\":";    s_readJson += (unsigned long)got;
  s_readJson += ",\"eof\":";    s_readJson += (eof ? "true" : "false");
  if (useB64) {
    s_readJson += ",\"enc\":\"b64\",\"data\":\"";
    if (got) s_readJson += base64Encode(buf, got);
    s_readJson += "\"}";
  } else {
    s_readJson += ",\"enc\":\"utf8\",\"data\":\"";
    if (got) appendJsonStringBytes(s_readJson, buf, got);
    s_readJson += "\"}";
  }
  if (buf) free(buf);
  return s_readJson.c_str();
}

// Chunked, permission-guarded file write for the companion app. BLE inbound is
// capped at one ~512-byte command per frame with no reassembly, so the app
// uploads a file as a sequence of small base64 chunks: offset 0 truncates/creates,
// later offsets must equal the current file size (strictly sequential append).
// `final` runs the post-save hooks. Returns a JSON envelope.
const char* cmd_filewrite(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  static char respBuf[192];
  CommandArgs a(argsInput);
  String path;
  if (requireQuotedPath(a, 0, path) != nullptr)
    return "{\"success\":false,\"error\":\"path must be a quoted token\"}";
  if (!a.hasMinArgs(3))
    return "{\"success\":false,\"error\":\"usage: filewrite needs path, offset, b64\"}";
  long offset = a.argInt(1, -1);
  String b64 = a.arg(2);
  bool isFinal = false;
  for (int i = 3; i < a.count(); i++) if (a.arg(i) == "final") isFinal = true;

  if (offset < 0) {
    snprintf(respBuf, sizeof(respBuf), "{\"success\":false,\"error\":\"Bad offset\"}");
    return respBuf;
  }

  const AuthContext& ctx = currentAuthContext();
  // Keep the explicit "Admin required" response for admin-only branches, matching
  // the web handler; openGuarded would also deny but with a vaguer message.
  if (isAdminOnlyPath(path) && !isAdminUser(ctx.user)) {
    snprintf(respBuf, sizeof(respBuf), "{\"success\":false,\"error\":\"Admin required\"}");
    return respBuf;
  }

  String bytes = base64Decode(b64);
  const size_t MAX_BLE_FILE = 256 * 1024;  // BLE is for config/text; large media via web
  if ((size_t)offset + bytes.length() > MAX_BLE_FILE) {
    snprintf(respBuf, sizeof(respBuf),
             "{\"success\":false,\"error\":\"File too large for BLE (cap %u KB); use the web browser\"}",
             (unsigned)(MAX_BLE_FILE / 1024));
    return respBuf;
  }

  FsLockGuard _g("filewrite");
  // offset 0 -> "w" (truncate/create); else "a" (append).
  const char* mode = (offset == 0) ? "w" : "a";
  File f = VFS::openGuarded(path, mode, ctx, /*create=*/true);
  if (!f) {
    snprintf(respBuf, sizeof(respBuf),
             "{\"success\":false,\"error\":\"Writes to this path are not allowed\"}");
    return respBuf;
  }
  // Sequential-integrity check: an append must land exactly at end-of-file. Report
  // the real size so the app can resync after a dropped/duplicated chunk.
  if (offset > 0 && (long)f.size() != offset) {
    long have = (long)f.size();
    f.close();
    snprintf(respBuf, sizeof(respBuf),
             "{\"success\":false,\"error\":\"Offset mismatch\",\"size\":%ld}", have);
    return respBuf;
  }

  size_t toWrite = bytes.length();
  size_t written = toWrite ? f.write((const uint8_t*)bytes.c_str(), toWrite) : 0;
  f.flush();
  long newSize = (long)f.size();
  f.close();

  if (written != toWrite) {
    snprintf(respBuf, sizeof(respBuf),
             "{\"success\":false,\"error\":\"Write failed (short write)\",\"size\":%ld}", newSize);
    return respBuf;
  }

  if (isFinal) runFileWritePostSaveHooks(path);

  snprintf(respBuf, sizeof(respBuf),
           "{\"success\":true,\"size\":%ld,\"final\":%s}", newSize, isFinal ? "true" : "false");
  return respBuf;
}

// filedelete is now a two-step interactive flow built on the CLIMode
// framework's confirm mode. The flow:
//   1. cmd_filedelete validates the path and captures the caller's
//      AuthContext into s_pendingFiledelete{Path,Ctx}.
//   2. Calls cliRequestConfirm() -- enters confirm mode, prints the
//      "Confirm delete of /foo?" prompt via broadcastOutput, frees
//      cmd_exec.
//   3. User's next command line is interpreted by confirm mode:
//        - "yes"/"y"/"true"/"1"/"on" -> filedelete_confirmed runs,
//          performs the actual delete, returns the result.
//        - anything else             -> filedelete_cancelled runs,
//          returns "Cancelled."
// Single-slot statics are safe because only one CLIMode is active at a
// time (cliRequestConfirm returns false if another mode is already up).
static String      s_pendingFiledeletePath;
static AuthContext s_pendingFiledeleteCtx;  // captured by VALUE so it survives between commands

// Perform the actual delete under `ctx`. Shared by the interactive confirm
// callback and the one-shot `filedelete <path> confirm` path. Returns a static
// buffer (the dispatcher copies it before the next command runs).
static const char* doFiledelete(const String& path, const AuthContext& ctx) {
  static char respBuf[256];

  if (!filesystemReady) return "Error: LittleFS not ready";

  // Re-check existence: something else could have removed the file in the
  // meantime (rare, but possible with concurrent SSE/MQTT writes; and the
  // interactive flow gives the user time to look at the prompt).
  if (!VFS::existsGuarded(path, ctx)) {
    return "Error: File no longer exists or access denied";
  }

  // If the file is the currently loaded map, unload it first to close the FD.
#if ENABLE_MAPS
  if (MapCore::hasValidMap() && path == String(MapCore::getCurrentMap().filepath)) {
    MapCore::unloadMap();
  }
#endif

  if (!VFS::removeGuarded(path, ctx)) {
    snprintf(respBuf, sizeof(respBuf),
             "Error: Failed to delete file (denied or fs error): %s", path.c_str());
    return respBuf;
  }
  snprintf(respBuf, sizeof(respBuf), "Deleted file: %s", path.c_str());
  return respBuf;
}

static const char* filedelete_confirmed(void* /*userData*/) {
  return doFiledelete(s_pendingFiledeletePath, s_pendingFiledeleteCtx);
}

static const char* filedelete_cancelled(void* /*userData*/) {
  // Static buffer so the response survives until the dispatcher reads it.
  static char respBuf[160];
  snprintf(respBuf, sizeof(respBuf), "Cancelled. %s not deleted.",
           s_pendingFiledeletePath.c_str());
  return respBuf;
}

const char* cmd_filedelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;

  // One-shot confirm token for programmatic clients (companion app / BLE):
  // `filedelete "<path>" confirm` deletes immediately, skipping the interactive
  // two-step yes/no gate that a stateful console session needs. It's a BARE
  // token after the quoted path — so a file literally named "/foo confirm"
  // (quoted) can no longer have its own safety gate swallowed.
  bool oneShot = false;
  if (a.has(1)) {
    String tok = a.arg(1);
    if (!a.argWasQuoted(1) &&
        (tok == "confirm" || tok == "--yes" || tok == "-y" || tok == "yes")) {
      oneShot = true;
    } else {
      return "Error: unexpected argument — usage: filedelete \"<path>\" [confirm]";
    }
  }
  if (a.has(2)) return "Error: unexpected argument — usage: filedelete \"<path>\" [confirm]";

  const AuthContext& ctx = currentAuthContext();
  if (!VFS::existsGuarded(path, ctx)) return "Error: File does not exist or access denied";

  if (oneShot) {
    return doFiledelete(path, ctx);
  }

  // Stash for the confirm callbacks. Capture the AuthContext BY VALUE
  // because currentAuthContext() returns a reference to per-task TLS
  // state that's owned by the current ExecIdentityGuard -- when this
  // function returns and the next command starts, the TLS reference
  // points at a different AuthContext.
  s_pendingFiledeletePath = path;
  s_pendingFiledeleteCtx  = ctx;

  String prompt = "Confirm delete of " + path + "? (cannot be undone)";
  // Originating command line stored for the resolution audit -- shows up
  // in [CMD] log as "filedelete /foo (confirm: yes) -> Deleted file: /foo"
  // (or "(confirm: no) -> Cancelled. /foo not deleted." on cancel).
  String origCmd = "filedelete " + quotePath(path);
  if (!cliRequestConfirm(prompt, origCmd, filedelete_confirmed, filedelete_cancelled, nullptr)) {
    return "Error: cannot request confirm (another interactive mode is active)";
  }

  // cliRequestConfirm already printed `prompt` via broadcastOutput from
  // confirm_onEnter. Return the yes/no hint as our command response so
  // the user sees a single coherent prompt.
  return "Type 'yes' to confirm or anything else to cancel.";
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
  { "files", "List files [\"path\"] | files json [\"path\"] | files stats json [\"path\"]", true, cmd_files,
    "files [\"path\"]            - List files in LittleFS (default '/')\n"
    "files json [\"path\"]       - List as JSON (app/BLE): {success,dirPerms,files[]}\n"
    "files stats json [\"path\"] - Storage usage JSON for the path's tier\n"
    "Paths are always double-quoted, e.g. files \"/logging_captures\"" },
  { "mkdir", "Create directory: \"<path>\"", true, cmd_mkdir, "Usage: mkdir \"<path>\"" },
  { "rmdir", "Remove directory: \"<path>\"", true, cmd_rmdir, "Usage: rmdir \"<path>\"" },
  { "filecreate", "Create file: \"<path>\"", true, cmd_filecreate, "Usage: filecreate \"<path>\"" },
  { "fileview", "View file: \"<path>\"", true, cmd_fileview, "Usage: fileview \"<path>\"" },
  { "fileread", "Read file chunk as JSON: \"<path>\" [offset] [len] [b64]", true, cmd_fileread,
    "fileread \"<path>\" [offset] [len] [b64] - Chunked permission-guarded read (app/BLE).\n"
    "Returns {success,size,offset,len,eof,enc,data}; loop offset until eof." },
  { "filewrite", "Write file chunk: \"<path>\" <offset> <b64chunk> [final]", true, cmd_filewrite,
    "filewrite \"<path>\" <offset> <b64chunk> [final] - Sequential chunked write (app/BLE).\n"
    "offset 0 truncates/creates; later offsets must equal current size; 'final' runs post-save hooks." },
  { "filedelete", "Delete file: \"<path>\" [confirm]", true, cmd_filedelete, "Usage: filedelete \"<path>\" [confirm]" },
  { "filerename", "Rename file: \"<oldpath>\" \"<newname>\"", true, cmd_filerename, "Usage: filerename \"<oldpath>\" \"<newname>\"" },
  { "logtier", "Show current log storage tier (LittleFS vs SD overflow).", false, cmd_logtier,
    "logtier             - Report which tier logs are writing to and free space on each." },
};

const size_t filesystemCommandsCount = sizeof(filesystemCommands) / sizeof(filesystemCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// File Permissions — Table-Driven, Role-Aware
// ============================================================================
//
// Three roles, three perm columns per rule:
//   USER    — authenticated non-admin
//   ADMIN   — authenticated user where isAdminUser(ctx.user) == true
//   SYSTEM  — internal trusted code (transport==SOURCE_INTERNAL && user=="system")
//
// Anonymous (empty ctx.user) callers are denied EVERYTHING regardless of
// rule. Reaching this state from FS code is a bug above; the deny + [PERM]
// log line surfaces the upstream auth gap.
//
// Permission flags (System_Filesystem.h):
//   PERM_READ   0x01  PERM_WRITE  0x02  PERM_DELETE 0x04
//   PERM_RENAME 0x08  PERM_CREATE 0x10  PERM_IMPORT 0x20
//   PERM_ALL    = OR of all six.
//
// Rule order matters: first match wins, so put more-specific paths first.
// ============================================================================

struct PathRule {
  const char* path;          // Path prefix (or exact path if exactMatch is true)
  uint8_t     userPerms;     // Bitmask granted to authenticated non-admin users
  uint8_t     adminPerms;    // Bitmask granted to admins (typically ⊇ userPerms)
  uint8_t     systemPerms;   // Bitmask granted to internal trusted code
  bool        exactMatch;    // true = match path exactly, false = prefix match
  bool        exemptSensitiveExt; // true = hasSensitiveExtension() does not block this path
                                  // Use for directories that intentionally hold .bin/.key/etc.
                                  // (e.g. /system/llm/ holds model files, /system/certs/ holds TLS)
};

// Columns: path, userPerms, adminPerms, systemPerms, exactMatch
//
// Sizing intent recap (preserves the prior single-column behavior, plus
// admin/system grants where the old `adminOnly` flag implied it):
//
//   Old `{perms=X, adminOnly=true}`  →  user=0, admin=X, system=PERM_ALL
//   Old `{perms=X, adminOnly=false}` →  user=X, admin=X, system=PERM_ALL
//   Old `{perms=0,  adminOnly=true}` →  user=0, admin=PERM_READ, system=PERM_ALL
//                                       (NEW: admin can now read user_settings,
//                                        which fixes the original bug that
//                                        triggered this whole refactor.)
static const PathRule sPathRules[] = {
  // Columns: path, userPerms, adminPerms, systemPerms, exactMatch, exemptSensitiveExt

  // ---- Sensitive credentials ----
  // Per-user settings: admin can READ them (the lens-Files admin-view bug fix);
  // system has full access (loadUserSettings/saveUserSettings); user gets nothing.
  {"/system/users/user_settings",       0,         PERM_READ,                                          PERM_ALL,  false, false},
  // pending_users.json: same — admin reads, system writes, user nothing.
  {"/system/users/pending_users.json",  0,         PERM_READ,                                          PERM_ALL,  true,  false},

  // ---- Immutable config files: read-only for admin, full for system ----
  {"/system/settings.json",             0,         PERM_READ,                                          PERM_ALL,  true,  false},
  {"/system/automations.json",          0,         PERM_READ,                                          PERM_ALL,  true,  false},
  {"/system/espnow/devices.json",       0,         PERM_READ,                                          PERM_ALL,  true,  false},

  // ---- TLS certificates: admin can read/delete/import; system full ----
  // exemptSensitiveExt: .pem/.crt/.key files live here by design.
  {"/system/certs/",                    0,         PERM_READ | PERM_DELETE | PERM_IMPORT,              PERM_ALL,  false, true},

  // ---- On-device LLM model files: admin can upload/read/delete .bin models ----
  // exemptSensitiveExt: .bin model files live here by design — this is NOT firmware.
  // PERM_WRITE + PERM_CREATE let the file-upload path overwrite an existing model
  // (canEdit) or create a new one (canCreate). PERM_IMPORT covers the import flow.
  {"/system/llm/",                      0,         PERM_READ | PERM_WRITE | PERM_CREATE | PERM_DELETE | PERM_IMPORT, PERM_ALL, false, true},

  // ---- G2 animated icon packs (SD; test bench + web upload) ----
  {"/sd/g2_icon_animations/",           0,         PERM_READ | PERM_DELETE | PERM_IMPORT | PERM_CREATE, PERM_ALL, false, false},

  // ---- System logs: admin read-only; system full ----
  {"/system/sys_logs/",                 0,         PERM_READ,                                          PERM_ALL,  false, false},

  // ---- Protected root directories (browse only) ----
  {"/system",                           0,         PERM_READ,                                          PERM_ALL,  true,  false},
  {"/logging_captures",                 0,         PERM_READ,                                          PERM_ALL,  true,  false},
  {"/espnow",                           PERM_READ, PERM_READ,                                          PERM_ALL,  true,  false},
  {"/maps",                             PERM_READ, PERM_READ,                                          PERM_ALL,  true,  false},
  {"/sd",                               PERM_READ, PERM_READ,                                          PERM_ALL,  true,  false},
  {"/Users",                            PERM_READ, PERM_READ,                                          PERM_ALL,  true,  false},

  // ---- General system paths: admin read-only; system full ----
  {"/system/",                          0,         PERM_READ,                                          PERM_ALL,  false, false},

  // ---- Logging captures: admin can read + delete; system full ----
  {"/logging_captures/",                0,         PERM_READ | PERM_DELETE,                            PERM_ALL,  false, false},

  // ---- ESP-NOW data: user can read+write+delete; admin same; system full ----
  {"/espnow/",                          PERM_READ | PERM_WRITE | PERM_DELETE,
                                        PERM_READ | PERM_WRITE | PERM_DELETE,                          PERM_ALL,  false, false},

  // ---- Default: user data (maps, photos, recordings, etc.) — full for everyone authenticated ----
  {nullptr,                             PERM_ALL,  PERM_ALL,                                           PERM_ALL,  false, false},
};

// Look up the first matching rule for a path.
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

// ----------------------------------------------------------------------------
// Filename-based sensitivity check.
//
// Suffix-matched (NOT substring) so that benign filenames containing the
// substring "secret" or "password" aren't false-positive blocked. The
// previous indexOf()-based check would block "my-secret-notes.txt" but
// would also miss "config.crt" / "model.bin" / etc.
// ----------------------------------------------------------------------------
static bool hasSensitiveExtension(const String& path) {
  String lower = path;
  lower.toLowerCase();

  // Block specific extensions (suffix match).
  static const char* kBlockedSuffixes[] = {
    ".key",  ".pem",  ".crt",  ".cert",  ".cer",   // TLS / cryptographic material
    ".credentials",                                 // generic credential file
    ".bin",  ".hex",  ".elf",                       // firmware images / executables
  };
  for (const char* sfx : kBlockedSuffixes) {
    if (lower.endsWith(sfx)) return true;
  }

  // Block filenames whose final segment contains the words "password" or
  // "secret" (whole-name substring on the basename only, not the directory).
  int lastSlash = lower.lastIndexOf('/');
  String base = (lastSlash >= 0) ? lower.substring(lastSlash + 1) : lower;
  if (base.indexOf("password") >= 0)   return true;
  if (base.indexOf("secret") >= 0)     return true;
  if (base.indexOf("credential") >= 0) return true;

  return false;
}

// Image files: can be viewed but not text-edited.
static bool isImageFile(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") ||
          lower.endsWith(".gif") || lower.endsWith(".bmp") || lower.endsWith(".webp") ||
          lower.endsWith(".ico") || lower.endsWith(".avif") || lower.endsWith(".heif"));
}

// ============================================================================
// Path normalization
// ============================================================================
// Used by the guarded VFS layer (Phase 1+) before any rule lookup. Rejects
// path traversal, collapses double slashes, strips a trailing slash (except
// for "/" itself), and rejects empty paths. Callers that fail this should
// treat the request as denied — `out` is undefined on failure.
bool normalizeFsPath(const String& in, String& out) {
  if (in.length() == 0) return false;

  // Reject ".." anywhere — even resolved-to-here ("/foo/bar/.." should
  // collapse to "/foo" but Arduino LittleFS does not collapse it, so the
  // permission check would see "/foo/bar/.." literally and either bypass
  // the rule (no startsWith match) or behave unpredictably). Easier and
  // safer to reject outright.
  if (in.indexOf("..") >= 0) return false;

  // Paths cross the command line as quoted tokens and the tokenizer has no
  // escape, so a literal double-quote is ambiguous — reject it (and control
  // chars) at this single chokepoint rather than in every producer.
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || (unsigned char)c < 0x20) return false;
  }

  out.reserve(in.length());
  out = "";
  bool prevSlash = false;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '/') {
      if (prevSlash) continue;  // collapse //
      prevSlash = true;
    } else {
      prevSlash = false;
    }
    out += c;
  }
  // Strip trailing slash unless the whole path is "/".
  if (out.length() > 1 && out[out.length() - 1] == '/') {
    out.remove(out.length() - 1);
  }
  return out.length() > 0;
}

// ----------------------------------------------------------------------------
// Quoted-path contract helpers (see System_Filesystem.h). One definition that
// every path-taking command shares, so "how a path is read" lives in one place.
// ----------------------------------------------------------------------------
const char* requireQuotedPath(const CommandArgs& a, int idx, String& out) {
  if (a.unterminatedQuote())
    return "Error: unmatched quote — wrap the path, e.g. fileview \"/dir/name\"";
  if (!a.has(idx) || !a.argWasQuoted(idx))
    return "Error: path must be in quotes, e.g. fileview \"/system/notes\"";
  out = a.arg(idx);
  if (out.length() == 0)
    return "Error: path is empty — e.g. fileview \"/system/notes\"";
  if (!out.startsWith("/")) out = "/" + out;
  return nullptr;
}

const char* requireQuotedToken(const CommandArgs& a, int idx, String& out) {
  if (a.unterminatedQuote())
    return "Error: unmatched quote — wrap it in double-quotes, e.g. \"my file\"";
  if (!a.has(idx) || !a.argWasQuoted(idx))
    return "Error: name must be in quotes, e.g. \"my recording.wav\"";
  out = a.arg(idx);
  if (out.length() == 0)
    return "Error: name is empty";
  return nullptr;
}

String quotePath(const String& path) {
  String out;
  out.reserve(path.length() + 2);
  out = "\"";
  out += path;
  out += "\"";
  return out;
}

// ============================================================================
// Role resolution + permission lookup
// ============================================================================

// Forward decl from System_User.cpp.
extern bool isAdminUser(const String& who);

enum class FsRole : uint8_t { ANON, USER, ADMIN, SYSTEM };

static FsRole resolveRole(const AuthContext& ctx) {
  if (ctx.user.length() == 0) return FsRole::ANON;
  if (ctx.transport == SOURCE_INTERNAL && ctx.user == "system") return FsRole::SYSTEM;
  if (isAdminUser(ctx.user)) return FsRole::ADMIN;
  return FsRole::USER;
}

static const char* roleName(FsRole r) {
  switch (r) {
    case FsRole::ANON:   return "anon";
    case FsRole::USER:   return "user";
    case FsRole::ADMIN:  return "admin";
    case FsRole::SYSTEM: return "system";
  }
  return "?";
}

static uint8_t permsForRole(const PathRule& rule, FsRole r) {
  switch (r) {
    case FsRole::ANON:   return 0;
    case FsRole::USER:   return rule.userPerms;
    case FsRole::ADMIN:  return rule.adminPerms;
    case FsRole::SYSTEM: return rule.systemPerms;
  }
  return 0;
}

// Capability scope: true if `path` (already normalized) is within `scope` (a
// path prefix). Empty scope = unconfined. Boundary-aware so "/sd/VIDEOS" does
// NOT match "/sd/VIDEOS_secret" — only the scope dir itself and paths beneath it.
static bool pathWithinScope(const String& path, const String& scope) {
  if (scope.length() == 0) return true;
  String base = scope;
  while (base.length() > 1 && base.endsWith("/")) base.remove(base.length() - 1);
  if (path == base) return true;
  return path.startsWith(base + "/");
}

// Single decision point. Pure: returns true/false, no logging. The
// per-denial [PERM] log line is emitted only at actual-access boundaries
// (VFS::*Guarded) via logFsAccessDeny() — see the comment on that function
// for the rationale. Calling checkPerm from an aggregate query like
// getPermissions() must NOT spam the log — those are UI button-state probes,
// not security events.
static bool checkPerm(const String& path, const AuthContext& ctx,
                      uint8_t needed,
                      bool sensitiveExtensionApplies,
                      bool imageEditApplies) {
  FsRole role = resolveRole(ctx);
  if (role == FsRole::ANON) return false;
  // Capability scope (defense-in-depth): a confined context may only touch its
  // own subtree, regardless of role. Empty scope (the default) is unconfined,
  // so this is a no-op for every existing context.
  if (!pathWithinScope(path, ctx.scope)) return false;
  // Directories marked exemptSensitiveExt (e.g. /system/llm/, /system/certs/)
  // intentionally hold files with otherwise-blocked extensions (.bin, .pem, etc.)
  // and must not be caught by the blanket sensitive-extension guard.
  const PathRule& rule = lookupRule(path);
  bool sensitiveBlocked = sensitiveExtensionApplies && !rule.exemptSensitiveExt
                          && hasSensitiveExtension(path) && role != FsRole::SYSTEM;
  if (sensitiveBlocked) return false;
  if (imageEditApplies && isImageFile(path) && role != FsRole::SYSTEM) return false;
  uint8_t granted = permsForRole(lookupRule(path), role);
  return (granted & needed) == needed;
}

// --- Public permission API (role-aware) ---
//
// All six canX() functions are pure queries — they decide true/false and
// stay silent. Use them freely for UI button-state computation (e.g.
// getPermissions in a file listing) without flooding the log.
//
// To audit an actual access denial, call logFsAccessDeny() yourself after
// canX returns false at the point where the access was being attempted
// (usually inside VFS::*Guarded — the caller of canX in those wrappers
// emits the log line themselves).

bool canRead   (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_READ,   /*sensitive*/ true,  /*imageEdit*/ false);
}
bool canEdit   (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_WRITE,  /*sensitive*/ true,  /*imageEdit*/ true);
}
bool canDelete (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_DELETE, /*sensitive*/ false, /*imageEdit*/ false);
}
bool canRename (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_RENAME, /*sensitive*/ false, /*imageEdit*/ false);
}
bool canCreate (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_CREATE, /*sensitive*/ false, /*imageEdit*/ false);
}
bool canImport (const String& path, const AuthContext& ctx) {
  return checkPerm(path, ctx, PERM_IMPORT, /*sensitive*/ false, /*imageEdit*/ false);
}

// Emit a single [PERM] DENY line for an actual access attempt that was
// refused. Re-runs role/rule resolution to format a useful message — only
// called on real denials, so the cost is bounded by how often legitimate
// access attempts are blocked.
//
// Why this exists as a separate function: the canX query API is silent so
// aggregate queries (getPermissions, isAdminOnlyPath, file-listing UI button
// state) don't spam the log. When VFS::openGuarded actually decides to
// refuse a read, IT calls this to record the security event.
//
// Gated by gDebugSubFlags.storagePermissions (default true). Allows muting
// the denial audit trail independently of other Storage debug subflags.
void logFsAccessDeny(const String& path, const AuthContext& ctx,
                     uint8_t needed, const char* op) {
  if (!gDebugSubFlags.storagePermissions) return;
  FsRole role = resolveRole(ctx);
  if (role == FsRole::ANON) {
    DEBUG_STORAGEF("[PERM] DENY %s '%s' role=anon user='' transport=%d (anonymous never permitted)",
                   op, path.c_str(), (int)ctx.transport);
    return;
  }
  // Re-check the special-case denials so the log line names the actual reason.
  const PathRule& rule = lookupRule(path);
  bool sensitiveApplies = (needed & (PERM_READ | PERM_WRITE)) != 0;
  bool imageEditApplies = (needed == PERM_WRITE);
  if (sensitiveApplies && !rule.exemptSensitiveExt && hasSensitiveExtension(path) && role != FsRole::SYSTEM) {
    DEBUG_STORAGEF("[PERM] DENY %s '%s' role=%s reason=sensitive-extension",
                   op, path.c_str(), roleName(role));
    return;
  }
  if (imageEditApplies && isImageFile(path) && role != FsRole::SYSTEM) {
    DEBUG_STORAGEF("[PERM] DENY %s '%s' role=%s reason=image-not-editable",
                   op, path.c_str(), roleName(role));
    return;
  }
  uint8_t granted = permsForRole(rule, role);
  DEBUG_STORAGEF("[PERM] DENY %s '%s' role=%s granted=0x%02X needed=0x%02X",
                 op, path.c_str(), roleName(role), (unsigned)granted, (unsigned)needed);
}

uint8_t getPermissions(const String& path, const AuthContext& ctx) {
  // Hot path — called once per entry by buildFilesListing for UI button state.
  // Naive impl was six calls to canX() each of which redid resolveRole +
  // lookupRule + hasSensitiveExtension + isImageFile. That's 6× redundant
  // work on every directory listing. This version computes each input once
  // and applies the special-case masks directly.
  FsRole role = resolveRole(ctx);
  if (role == FsRole::ANON) return 0;

  const PathRule& rule = lookupRule(path);
  uint8_t granted = permsForRole(rule, role);

  // Special-case denials, applied as bitmask filters. These mirror the
  // sensitiveExtensionApplies / imageEditApplies flags that canRead/canEdit
  // pass to checkPerm:
  //   - hasSensitiveExtension blocks PERM_READ and PERM_WRITE (used by
  //     canRead and canEdit). Other ops (delete/rename/create/import) are
  //     unaffected — you can still delete a .key file you can't read.
  //   - isImageFile blocks PERM_WRITE only (used by canEdit). Reading,
  //     deleting, renaming an image stays allowed.
  // SYSTEM identity is exempt from both — internal code can read certs etc.
  // Paths with exemptSensitiveExt=true (e.g. /system/llm/, /system/certs/)
  // are also exempt — they intentionally contain .bin/.pem/etc. files.
  if (role != FsRole::SYSTEM) {
    if (!rule.exemptSensitiveExt && hasSensitiveExtension(path)) granted &= ~(PERM_READ | PERM_WRITE);
    if (isImageFile(path))                                        granted &= ~PERM_WRITE;
  }
  return granted;
}

uint8_t getDirPerms(const String& dirPath, const AuthContext& ctx) {
  // Probe what permissions a child of this directory would receive.
  String testPath = dirPath;
  if (!testPath.endsWith("/")) testPath += "/";
  testPath += "_";
  return getPermissions(testPath, ctx);
}

// `isAdminOnlyPath` classifies the rule itself (does the path belong to the
// "admin/system territory" branch?) so it stays identity-free. Used by file
// listings to hide entire subtrees from non-admin browsers.
bool isAdminOnlyPath(const String& path) {
  const PathRule& rule = lookupRule(path);
  return rule.userPerms == 0 && (rule.adminPerms != 0 || rule.systemPerms != 0);
}

// ============================================================================
// File I/O Helpers (moved from .ino)
// ============================================================================

bool readTextLimited(const char* path, String& out, size_t maxBytes) {
  out = "";
  FsLockGuard guard("readTextLimited");
  // Unguarded VFS::open by design — readTextLimited is a policy-free byte
  // shovel called from many trusted callers that already gated their own
  // access checks. Routing through VFS keeps the LittleFS-vs-SD dispatch
  // consistent without re-running perms here.
  File f = VFS::open(String(path), "r");
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
