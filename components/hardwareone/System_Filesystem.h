#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <Arduino.h>
#include "System_User.h"   // AuthContext for the role-aware permission API

// Forward declarations
class String;

// ============================================================================
// Filesystem State
// ============================================================================

// Global filesystem ready flag (defined in filesystem.cpp)
extern bool filesystemReady;

// ============================================================================
// Filesystem Helper Functions
// ============================================================================

/**
 * Build a directory listing as either JSON or human-readable text
 * @param inPath Directory path to list
 * @param out Output string (by reference)
 * @param asJson true for JSON format, false for text
 * @return true on success, false on error
 */
bool buildFilesListing(const String& inPath, String& out, bool asJson, const AuthContext& ctx, bool hideAdminPaths = false);

// Filesystem command registry (for system_utils.cpp)
struct CommandEntry;  // Forward declaration

// Boot sequence management
void loadAndIncrementBootSeq();
extern const CommandEntry filesystemCommands[];
extern const size_t filesystemCommandsCount;

/**
 * Initialize the filesystem (LittleFS)
 * @return true if initialized successfully
 */
bool initFilesystem();

// ============================================================================
// File Permissions and Protection
// ============================================================================

/**
 * File permission flags
 */
enum FilePermission {
  PERM_READ   = 0x01,
  PERM_WRITE  = 0x02,
  PERM_DELETE = 0x04,
  PERM_RENAME = 0x08,
  PERM_CREATE = 0x10,   // Create new files/folders (CLI mkdir/filecreate)
  PERM_IMPORT = 0x20,   // Upload/import files via web
  PERM_EDIT   = (PERM_READ | PERM_WRITE),
  PERM_ALL    = (PERM_READ | PERM_WRITE | PERM_DELETE | PERM_RENAME | PERM_CREATE | PERM_IMPORT)
};

// ============================================================================
// Permission API — three-role rule table
// ============================================================================
//
// Every check resolves the caller's effective role:
//   - SYSTEM   — internal trusted code (transport==SOURCE_INTERNAL && user=="system")
//   - ADMIN    — authenticated user whose username is admin per isAdminUser()
//   - USER     — authenticated non-admin user
//   - ANON     — empty `ctx.user` (no auth). DENIED for every operation.
//                Reaching this state from FS code is a bug above; the deny
//                + [PERM] log surfaces the upstream gap.
//
// Each PathRule grants three independent perm masks (userPerms / adminPerms /
// systemPerms). adminPerms is typically a superset of userPerms; systemPerms
// is typically PERM_ALL for files the system manages.
//
// Anonymous callers always get 0 perms regardless of the rule. This is
// intentional — there is no legitimate code path that should reach the FS
// without auth.

/**
 * Role-aware read check.
 * @param path Absolute path to check
 * @param ctx  Caller identity (transport + user)
 * @return true if reading is allowed for this caller
 */
bool canRead   (const String& path, const AuthContext& ctx);
bool canEdit   (const String& path, const AuthContext& ctx);
bool canDelete (const String& path, const AuthContext& ctx);
bool canRename (const String& path, const AuthContext& ctx);
bool canCreate (const String& path, const AuthContext& ctx);
bool canImport (const String& path, const AuthContext& ctx);

/**
 * Aggregate permissions for a path under the caller's identity.
 * Pure query: never logs. Use freely for UI button-state computation.
 * @return Bitmask of FilePermission flags
 */
uint8_t getPermissions(const String& path, const AuthContext& ctx);

/**
 * Emit a single [PERM] DENY line for an actual access attempt that was
 * refused. Called from VFS::*Guarded after canX returns false — the canX
 * functions themselves are silent so aggregate queries (getPermissions,
 * file-listing UI) don't spam the log.
 *
 * `needed` is the FilePermission bit being checked (PERM_READ etc.).
 * `op` is a short verb for the log line ("read", "edit", "create", ...).
 */
void logFsAccessDeny(const String& path, const AuthContext& ctx,
                     uint8_t needed, const char* op);

/**
 * Aggregate permissions a child of `dirPath` would receive under the
 * caller's identity. Used to drive toolbar enable/disable in file UIs.
 */
uint8_t getDirPerms(const String& dirPath, const AuthContext& ctx);

/**
 * True if the path's rule was authored as "admin-only territory" — i.e.
 * userPerms is 0 AND (adminPerms != 0 OR systemPerms != 0). Used by the
 * file-listing UI to hide entire branches from non-admins. Identity-free
 * because it's about the path's classification, not the caller.
 */
bool isAdminOnlyPath(const String& path);

/**
 * Path normalization for guarded VFS access. Rejects path traversal
 * sequences (".."), collapses double slashes, strips a trailing slash
 * (except for "/"), and rejects empty paths. Returns true if the path
 * was acceptable; on success `out` holds the canonicalised form.
 */
bool normalizeFsPath(const String& in, String& out);

// ============================================================================
// File I/O Helpers
// ============================================================================

/**
 * Read up to maxBytes from a file into a String
 * @param path File path
 * @param out Output string
 * @param maxBytes Maximum bytes to read
 * @return true on success
 */
bool readTextLimited(const char* path, String& out, size_t maxBytes);

/**
 * Append a line to a file, enforcing a maximum file size
 * @param path File path
 * @param line Line to append
 * @param capBytes Maximum file size in bytes
 * @return true on success
 */
bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

/**
 * Recover a log file from a partial rotation. `appendLineWithCap` rotates by
 * writing a sibling `.tmp` file and renaming over the original, which leaves
 * a small crash-unsafe window. If the reboot hits that window, this call
 * promotes the `.tmp` back to the canonical path (or removes a stale `.tmp`
 * if the original survived). Safe to call even when no orphan exists —
 * it short-circuits on the first `exists()` check.
 *
 * Should be called once per known log-file path during boot, after the
 * filesystem is mounted.
 */
void cleanupLogOrphan(const char* path);

// Log-overflow tiering lives in the VFS namespace now. See System_VFS.h for
// VFS::resolveOverflowPath, VFS::isLogOverflowActive, VFS::getCachedLittleFsFree.

#endif // FILESYSTEM_H
