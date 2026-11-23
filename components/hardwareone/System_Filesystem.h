#ifndef SYSTEM_FILESYSTEM_H
#define SYSTEM_FILESYSTEM_H

#include <Arduino.h>
#include "System_User.h"   // AuthContext for the role-aware permission API

// Forward declarations
class String;
class PsramBuffer;
namespace fs { class File; }

// ============================================================================
// Filesystem State
// ============================================================================

// Global filesystem ready flag (defined in filesystem.cpp)
extern bool filesystemReady;

// Partition-table name of the internal LittleFS volume. Anything that calls
// esp_littlefs_* directly must key off this rather than its own literal: the
// Arduino wrapper carries the label the mount actually used, so a duplicated
// string is a silent-divergence surface. A mismatch is not a mount failure —
// esp_littlefs_info() just returns ESP_ERR_NOT_FOUND and storage stats read
// 0/0/0 while every file operation keeps working, which surfaces as
// "free space: 0" and rejected uploads with nothing in the log to explain it.
inline constexpr char kLittleFsPartitionLabel[] = "littlefs";

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

/**
 * Build the storage-stats JSON ({total,used,free,usagePercent}) for the storage
 * tier that owns `path` (SD vs internal LittleFS). Writes a NUL-terminated JSON
 * object into `out`. One reply shape, two readers:
 *
 * buildFilesStatsJson is the web Files page's (/api/files/stats, asked on every
 * folder load). For internal storage it shows the shared VFS capacity reading
 * when that is at most 10 s old and walks LittleFS only otherwise (about 1.3 s
 * on the P4X-EYE). A web upload, a file save (runFileWritePostSaveHooks),
 * `mkdir`, `rmdir`, `filedelete`, `micdelete` and `imagedelete` mark the
 * reading stale, so the page's own reload after them is a fresh one; other
 * writers (captures, logs, settings, files received from a peer) show up
 * within 10 s. The SD card is read fresh every time.
 *
 * buildFilesStatsJsonFresh is the `files stats` command's: always a fresh read
 * (which also renews the shared reading). The two can differ by up to 10 s of
 * internal writes.
 */
void buildFilesStatsJson(const String& path, char* out, size_t outSize);
void buildFilesStatsJsonFresh(const String& path, char* out, size_t outSize);

/**
 * Build the complete directory-listing JSON envelope
 * ({success,dirPerms,files:[...]}) for `path` under `ctx`. Single source of truth
 * for the web /api/files/list handler and the `files json` CLI/BLE command so the
 * shape can't drift. Returns false (and an error envelope in `out`) if the
 * directory can't be listed. Transport-specific concerns (HTTP 403, BLE static
 * buffer, admin-only pre-checks) stay in the callers.
 */
bool buildFilesListJson(const String& path, const AuthContext& ctx, bool hideAdminPaths, String& out);
// Direct owned-output form. On allocation/capacity failure, out.ok() is false
// and the caller must send an allocation-independent error, never its contents.
// HTTP uses request-owned storage; command callers retain their private owner.
bool buildFilesListJson(const String& path, const AuthContext& ctx, bool hideAdminPaths, PsramBuffer& out);

/**
 * Post-save hook shared by every file-write path (web write, web upload, BLE
 * filewrite): keep /system/automations.json free of duplicate IDs and flag the
 * scheduler to refresh (that part no-ops unless the path is automations.json),
 * and mark the internal storage reading stale when the file is on LittleFS, so
 * the next reader of the figure (Files page, status poll) takes a fresh one.
 * Safe to call for any path.
 */
void runFileWritePostSaveHooks(const String& path);

// Boot sequence management
void loadAndIncrementBootSeq();

/**
 * Initialize the filesystem (LittleFS)
 * @return true if initialized successfully
 */
bool initFilesystem();
// After initFilesystem() failed: what went wrong and how to fix it, one
// sentence for the FATAL line (no partition, a partition that does not mount,
// or a blank one that could not be formatted). "" before any failure.
const char* filesystemMountFailureHelp();

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
//   - SUPER    — superadmin / live bond-admin (unrestricted)
//   - ADMIN    — authenticated user whose username is admin per isAdminUser()
//   - USER     — authenticated non-admin, non-guest user
//   - GUEST    — authenticated guest; userPerms masked to PERM_READ only
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
 * Pure query: canonicalizes the path, applies ctx.scope, and never logs. Use
 * freely for one-shot UI button-state computation.
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
// Quoted-path command contract (uniform across all path-taking commands)
// ----------------------------------------------------------------------------
// Every command that takes a filesystem path reads it as a DOUBLE-QUOTED token,
// e.g.  fileview "/system/notes"  — even when the name has no spaces. This one
// rule lets a single tokenizer handle names with spaces and keeps bare flags
// (json/confirm) unambiguous vs. a folder literally named "json".
// ============================================================================

class CommandArgs;  // fwd decl (System_Command.h)

// Validate that arg `idx` is a non-empty quoted path token. On success returns
// nullptr and fills `out` with the path (leading slash ensured). On failure
// returns a ready-to-return "Error:" cstring describing the quoting problem.
const char* requireQuotedPath(const CommandArgs& a, int idx, String& out);

// A file command's reply for a path that is missing or that the caller may not
// read: one text for both, so a refusal does not reveal that the path exists.
// The OLED file browser shows its own toast for it.
constexpr char kFileReplyMissingOrDenied[] = "Error: File does not exist or access denied";

// Like requireQuotedPath but returns the token verbatim (no leading slash
// forced) — for commands whose name/path is resolved against a feature folder
// (e.g. a recording name, a model filename) rather than an absolute VFS path.
const char* requireQuotedToken(const CommandArgs& a, int idx, String& out);

// Wrap a path for embedding in a command string a producer hands to the parser
// (web / ESP-NOW / app glue). Returns "\"<path>\"".
String quotePath(const String& path);

// ============================================================================
// File I/O Helpers
// ============================================================================

// Write a prepared row followed by LF to a borrowed file. Both writes are
// attempted; written reports their accepted bytes even on failure. True means
// both counts were complete with no reported Print error, not durable storage.
// Callers own locking, file lifetime, formatting, flushing and failure policy.
bool writeLineChecked(fs::File& file, const uint8_t* data, size_t length,
                      size_t& written);

// Copy from the input's current position into an already-open output. Callers
// own permissions, locking and both file lifetimes. Accumulate only fully
// written chunks in totalBytes; update lastByte only after a complete write
// (leave it unchanged for empty input). A nonpositive read ends the copy,
// preserving the capture stitchers' existing behavior; false means short write.
bool copyFileContents(fs::File& input, fs::File& output,
                      unsigned long& totalBytes, uint8_t& lastByte);

/**
 * Read up to maxBytes from a file into a String, through the guarded VFS as
 * `ctx`. A denial reads as "cannot open" and is logged by the VFS.
 * @param path File path
 * @param out Output string
 * @param maxBytes Maximum bytes to read
 * @param ctx Identity the read is authorized as
 * @return true on success
 */
bool readTextLimited(const char* path, String& out, size_t maxBytes, const AuthContext& ctx);

/**
 * Append a CRLF-terminated line and trim old lines when the size exceeds cap.
 * The writer is the system identity confined to the capped-log tree
 * (VFS::Scopes::SYS_LOGS, or its SD mirror once the overflow latch fires), so
 * no caller can point it at another file; every caller's path lives there.
 * Checks accepted append bytes and the complete temporary rotation copy.
 * LittleFS replacement preserves the original on failure; SD retains its
 * remove/rename recovery window. A failed append may leave a partial line.
 * File's void flush/close API cannot establish durable-storage success.
 * @param path File path
 * @param line Line to append: `lineLength` bytes, no terminator needed; the
 *        file receives those bytes and CRLF
 * @param lineLength Bytes of `line`
 * @param capBytes Maximum size of the log in bytes, as its fixed constant. The
 *        writer scales it to the LittleFS partition (UP-03: all capped logs
 *        together use at most a quarter of it, cappedLogScaledCap). A
 *        two-generation log keeps "<path>.1" and `path`, each up to half the
 *        scaled cap; events.log, errors.log and crash-history.log stay one
 *        file trimmed by copy in a build with ENABLE_ESPNOW_SEND.
 *
 * Lines are batched (S-8): except for the login logs and the crash history,
 * a line waits in a PSRAM buffer and reaches the file within 2 s or when about
 * 2 KB is pending; the call then returns true before anything is written.
 * @return false on a detected append or rotation failure. A short append also
 *         skips rotation. A rotation whose copy cannot be written (low space,
 *         no copy buffer) restarts the log with a one-line drop marker and the
 *         new line, so the file never passes its cap by more than one line
 *         (G-39). With the overflow latch set, no writable card and LittleFS
 *         below the 100 KB floor the line is refused (counted by
 *         VFS::logReserveDrops) to keep the reserve; a failed append on the SD mirror marks the card unwritable
 *         and retries once on the re-resolved tier.
 */
bool appendLineWithCap(const char* path, const char* line, size_t lineLength, size_t capBytes);
// The same, with the text and its length taken from a String. Inline, so a
// caller on a small task stack pays no extra call frame (Xtensa makes no
// sibling calls).
inline bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {
  return appendLineWithCap(path, line.c_str(), line.length(), capBytes);
}

// Capped-log batching and sizes (S-8, UP-03).
// After the LittleFS mount: reads the partition size the caps are scaled to
// and makes the batch buffers (PSRAM only; without them every line is written
// through).
void cappedLogsInit();
// The cap a log with fixed cap `capBytes` has on this device.
size_t cappedLogScaledCap(size_t capBytes);
// Writes every pending line now. With `thenWriteThrough`, later lines of this
// boot are written at once: call it when a restart or deep sleep is certain
// (the log task does when it writes the [EVENT][REBOOT] line). Takes the
// filesystem lock; not for a task with a small stack (esp_timer).
void cappedLogsFlush(bool thenWriteThrough);
// The log task's tick: writes the logs whose oldest pending line is 1.5 s old.
// Two reads when nothing is pending.
void cappedLogsFlushDue();
bool cappedLogsPending();
// True once cappedLogsFlush(true) ran: a restart or deep sleep was announced.
bool cappedLogsRestartAnnounced();
// The main loop is running, so boot is over (one flag write; VFS::sdWatchTick
// calls it on every pass). Until then a log that older firmware left above
// its new size is not cut down: that cut copies up to half the log's cap
// under the filesystem lock, and boot must not wait for it. During boot a
// file is copied only above the cap its caller names, where older firmware
// rotated too; the first write after boot cuts the rest.
void cappedLogsNoteLoopRunning();

/**
 * Recover a log file from a partial rotation. SD rotation (and older LittleFS
 * firmware) removes the original before renaming a sibling `.tmp` into place,
 * leaving a small crash-unsafe window. If the reboot hits that window, this call
 * promotes the `.tmp` back to the canonical path (or removes a stale `.tmp`
 * if the original survived). Safe to call even when no orphan exists —
 * it short-circuits on the first `exists()` check. The `/sd` mirror of `path`
 * gets the same check whenever the card is mounted, so an orphan left while
 * log overflow was latched on an earlier boot is recovered too.
 *
 * Should be called once per known log-file path during boot, after the
 * filesystem is mounted.
 */
void cleanupLogOrphan(const char* path);

// Log-overflow tiering lives in the VFS namespace now. See System_VFS.h for
// VFS::resolveOverflowPath, VFS::isLogOverflowActive, VFS::getCachedLittleFsFree.

#endif // SYSTEM_FILESYSTEM_H
