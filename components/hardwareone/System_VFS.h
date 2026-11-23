#ifndef SYSTEM_VFS_H
#define SYSTEM_VFS_H

#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include "System_User.h"   // AuthContext (for the *Guarded variants)
#include "System_VFSCapacityCache.h"

// ============================================================================
// VFS — Virtual File System dispatcher
// ============================================================================
// VFS is a thin routing layer that dispatches filesystem operations to either
// LittleFS (internal flash) or the SD card library based on path prefix. It
// does NOT replace LittleFS — it uses LittleFS (and the SD library) under
// the hood.
//
// Routing rule (via `getStorageType`):
//   - Paths beginning with "/sd/..." (or exactly "/sd") → SD card
//   - Everything else → LittleFS
//
// There is no automatic tier switching in the routing layer itself. A call like
//   VFS::open("/system/settings.json", "w")
// will always write to LittleFS. The write may fail if LittleFS is full; it
// will NOT silently end up on SD. This is intentional: state files (users,
// settings, automations) must have a single source of truth.
//
// Log lines are the exception, and they OPT IN through `resolveLogPath` + a
// normal `VFS::open` on the resolved path. Since B11 (2026-11-22) the card
// comes first: while a card takes writes, has room above its reserve and the
// device key is available, a primary path like "/logging_captures/foo.csv"
// resolves to its card twin "/sd/logging_captures/foo.csv", and what a logger
// writes there is sealed (System_SealedLog.h). With no usable card the path
// stays on flash.
//
// Nothing is latched about the tier: every call asks again, so a card pulled
// (or put back) mid-run flips the active tier and one logical day CAN split
// across the two with nothing on disk marking the boundary; readers show both
// twins. Callers that need a stable tier must resolve once per session and
// store the result. The latch that remains is about flash running short: it
// trips when LittleFS free space is below the reserve, re-arms once free
// space is back a reserve's width above the level that tripped it (G-39), and
// while it is set resolveLogPath reports a resolution with no usable card as
// Refused once LittleFS free is below the global floor, so a log does not eat
// the reserve it exists to protect.
//
// Convention for callers:
//   - State files (settings, users, automations, mesh config, etc.)
//     → call VFS::open directly. They stay on LittleFS; writes fail cleanly
//       when flash is full rather than splitting across tiers.
//   - Append-only log data (the capped logs, sensor and automation captures,
//     the system log)
//     → call resolveLogPath first, then VFS::open / VFS::exists / VFS::rename /
//       VFS::remove on the path it gives. Rotation and cap enforcement happen
//       on whichever tier is active, and a path on the card is written
//       through System_SealedLog.

namespace VFS {

enum StorageType {
  INTERNAL = 0,
  SDCARD = 1,
  AUTO = 2
};

bool init();

bool isLittleFsReady();

/**
 * SD availability: true iff the selected SD transport mount has succeeded and the
 * mount hasn't been torn down. This does NOT prove files can actually be
 * written — a card can mount but fail writes (bad sector where the next
 * file happens to land, card yanked since, write-protect tab, filesystem
 * errors). SDMMC also checks the card-detect switch and latches observed
 * removal until remount. For "can we record to SD" use isSdWritable().
 */
bool isSdAvailable();

/**
 * Round-trip-verified writability. Returns true only if a write+read+delete
 * probe has succeeded on the card recently. If the cached state is false
 * but the card is mounted, a lazy re-probe is attempted — this gives free
 * auto-recovery for cards that were briefly glitchy. Cheap when cached,
 * ~a few ms when re-probing. Use this for anything that's about to do a
 * real write (video recording, log overflow, image save).
 */
bool isSdWritable();

/**
 * Callers that attempt an SD write and see it fail should call this with
 * a short reason string. It invalidates the cached writable flag so the
 * next isSdWritable() query re-probes. Cheap.
 */
void noteSdWriteFailure(const char* hint);

// SD card management. Stop active writers and close their files before
// unmount/format. All three refuse (return false) while an SD-in-use
// registration is held. A card is mounted at boot and by remountSd()
// (`sdmount`); on a board with a card-detect switch (the P4X-EYE) the card
// watch below also mounts a card that is put in after boot, or pulled and put
// back, once it has been in for a second (UP-06).
bool remountSd();
bool unmountSd();
bool formatSd();

// UP-06: the card watch. Call on every pass of the main loop. On a board
// without a card-detect switch the watch does nothing. It reads the switch (no
// filesystem lock) and, every 500 ms, decides by System_SdWatchCore.h: a card
// that left a mount (the switch open for 100 ms, or a card access that found
// it open) is reported once (SD-unmounted event); a card that has
// been in for 1 s with no usable mount gets one mount (1 to 2 s after it was
// put in), run on cmd_exec, with
// retries 5 s, 30 s and 5 min after a failure. `sdunmount` holds it until
// `sdmount`, `sdformat`, a restart, or the slot is empty. A contact that
// opens for a moment does none of that. No mount is
// started while the card is in use (a registered owner, a recording, a
// transcription).
// On every board the first call also marks the end of boot for the capped-log
// writer (cappedLogsNoteLoopRunning).
void sdWatchTick();

// SD-in-use registration. An owner that keeps an SD file open across calls
// (the video recorder) registers before it opens the file and ends the
// registration after closing it. VFS only records the owner name (a string
// with static lifetime); it never calls into an owner. sdInUseBegin returns
// false when every slot is taken.
bool sdInUseBegin(const char* owner);
void sdInUseEnd(const char* owner);
// One current holder's name, or nullptr when the card is free.
const char* sdInUseOwner();

// Counts successful SD mounts (boot, remount, format); it only increases.
// Owners with once-per-mount work compare it with the value they last acted
// on instead of caching availability, which they ask isSdAvailable() for.
uint32_t sdMountGeneration();

StorageType getStorageType(const String& path);
String normalize(const String& path);
String stripSdPrefix(const String& path);

// ============================================================================
// Virtual entries — mount points surfaced as folders during enumeration
// ============================================================================
// `VFS::open(parentPath, "r")` then iterating entries does NOT include
// mount points like /sd at the LittleFS root, because the underlying
// LittleFS knows nothing about them. Without help, every consumer that
// browses the filesystem (FileManager, the web file listing, future
// browsers) has to special-case "and don't forget to inject /sd at root
// when SD is mounted." That's a known smell; this API centralises it.
//
// Callers walk the underlying FS as usual via VFS::open, then call this
// to get any extra synthetic entries that should appear at the same
// level. Today: returns "sd" once when parentPath == "/" and SD is
// mounted. Tomorrow: USB MSC, network share, anything else mounted at a
// well-known path.
struct VirtualEntry {
  const char* name;     // display name only (no leading slash)
  bool        isFolder; // always true today; reserved for future
};

/** Fill `out` with up to `cap` virtual mount-point entries that should
 *  appear when the caller is enumerating `parentPath`. Returns the
 *  number actually written. Cheap (no FS I/O — just checks mount flags).
 */
size_t listVirtualEntries(const String& parentPath, VirtualEntry* out, size_t cap);

// Staging names used by checked replacement: `<file>.tmp` (including
// `.header.tmp`) and the SD backup `<file>.bak`. Listings offered to a user as
// loadable files should skip them; they are never a finished output.
inline bool isStagingName(const String& nameOrPath) {
  return nameOrPath.endsWith(".tmp") || nameOrPath.endsWith(".bak");
}

// One entry in the VFS directory view. Names and full VFS paths are owned
// strings; no child File or backend-relative path escapes the reader.
struct DirectoryEntry {
  String name;
  String path;
  bool isFolder = false;
  bool isVirtual = false;
  size_t size = 0;
  time_t modifiedAt = 0;
};

// Iterate virtual mounts first, then direct children in backend order.
// The caller must hold its filesystem lock and supply a guarded-open directory
// at parentPath. The reader borrows it without closing or rewinding it. Keep
// both the lock and File alive until iteration ends; next() closes child files
// before returning. Filtering, permissions, counts and presentation stay with
// callers. This reader neither opens paths nor grants access.
// Modification times are opt-in because the Arduino backend performs another
// stat for each requested file; folders, virtual mounts and default readers
// leave modifiedAt at zero.
class DirectoryReader {
public:
  DirectoryReader(File& directory, const String& parentPath, bool includeModifiedTime = false);
  DirectoryReader(const DirectoryReader&) = delete;
  DirectoryReader& operator=(const DirectoryReader&) = delete;
  bool next(DirectoryEntry& entry);

private:
  File& directory_;
  String parentPath_;
  String backendPrefix_;
  bool includeModifiedTime_;
  VirtualEntry virtuals_[4];
  size_t virtualCount_;
  size_t virtualIndex_ = 0;
};

// There is no unguarded exists/open/mkdir/remove/rename/rmdir in this API.
// The backend dispatchers that pick LittleFS or SD for a path are private to
// System_VFS.cpp; every access from outside goes through the guarded calls
// below with an identity, and internal code uses systemAuth(scope, reason).

// Authoritative capacity query: always samples the backing filesystem and
// never serves the snapshot cache below. Keep admission and correctness
// decisions (uploads, captures, log start) on this API. Returns false for an
// unavailable tier, a reported backend error or zero total capacity; outputs
// remain unchanged on failure. SD cannot distinguish zero used bytes from a
// failed usedBytes() call, so that value alone is not a failure signal.
bool getStats(StorageType type, uint64_t& totalBytes, uint64_t& usedBytes, uint64_t& freeBytes);

/**
 * Opt-in capacity snapshot for presentation/telemetry callers.
 *
 * maxAgeMs == 0 is explicitly fresh and samples the backing filesystem. A
 * non-zero max age may reuse the newest successful sample for that storage
 * tier. Successful fresh getStats() calls also seed this cache, but cached
 * calls never change getStats()'s fresh semantics.
 *
 * On a cache miss, the same validity rules as getStats() apply. On failure,
 * returns false and leaves `out` unchanged. A prior valid sample remains
 * eligible for reuse within the caller's explicitly allowed age.
 */
bool getStatsSnapshot(StorageType type, CapacitySnapshot& out,
                      uint32_t maxAgeMs = 0);

/** maxAgeMs for getStatsSnapshot: reuse the tier's newest successful sample
 *  however old it is, and sample only when it has none (before the first
 *  reading, or after invalidateStatsSnapshot). For periodic status figures
 *  that must not wait on a LittleFS walk. The INTERNAL reading is renewed by
 *  any getStats(), by the web Files page (when its reading is older than
 *  10 s) and by the log writer's free-space check, which runs with log lines
 *  (every 2, 5 or 15 s by headroom; every 30 s while logs are moved to SD).
 *  While nothing is logged it keeps its last value, so it can lag a file
 *  write for as long as the device stays quiet; a web upload, a file save,
 *  `mkdir`, `rmdir`, or a `filedelete`, `micdelete` or `imagedelete` on
 *  LittleFS, marks it stale (invalidateStatsSnapshot), so the next reader
 *  samples once. */
static constexpr uint32_t kSnapshotAnyAgeMs = UINT32_MAX;

/** Cache-only capacity read for periodic telemetry on a loop that must never
 *  wait (the system metrics sampler on the main loop): the tier's newest
 *  successful snapshot of any age, copied under the short cache spinlock.
 *  Never takes the filesystem lock and never samples. Returns false and
 *  leaves `out` unchanged when the tier has none: before its first reading,
 *  after invalidateStatsSnapshot(), and after every SD mount, unmount, remount
 *  or format (each invalidates the SD tier), so an SD hit also means the card
 *  was mounted when it was last read. */
bool peekStatsSnapshot(StorageType type, CapacitySnapshot& out);

/** Invalidate one tier's presentation snapshot. AUTO follows the existing
 * getStats routing rule and addresses INTERNAL. Generation fencing prevents a
 * slow in-flight refresh from publishing across this invalidation. */
void invalidateStatsSnapshot(StorageType type);

// ============================================================================
// Guarded VFS — single enforcement point for filesystem permissions
// ============================================================================
// Every guarded call runs the same pipeline:
//   1. normalizeFsPath() — reject ".." traversal, collapse double slashes,
//      strip trailing slash. Failure → deny + log.
//   2. canX(path, ctx) — three-role permission lookup. Failure → log + deny.
//   3. Dispatch to the underlying VFS::open/exists/etc.
//
// These are the only file operations the firmware exposes. Pass the
// AuthContext you have (web handlers, CLI handlers via currentAuthContext(),
// etc.). For internal trusted code, construct a context via
// systemAuth(scope, "reason") with the narrowest scope that holds the files
// it touches, and pass it through.
//
// Naming convention: every callsite that uses systemAuth() should be
// commented "// trusted: <reason>" so a future reader sees the explicit
// privilege escalation rather than wondering why the check passed.
//
// Returns: open returns an empty File on denial (caller must check `!file`
// or `file.operator bool()`). The bool-returning variants return false on
// denial AND log to [PERM]. Grants are silent.

File openGuarded   (const String& path, const char* mode, const AuthContext& ctx, bool create = false);
bool existsGuarded (const String& path, const AuthContext& ctx);
bool removeGuarded (const String& path, const AuthContext& ctx);
bool renameGuarded (const String& pathFrom, const String& pathTo, const AuthContext& ctx);
// Promote a closed, completely checked temporary on the same storage backend.
// Caller holds FsLockGuard across preparation, this call and failed-temp cleanup.
// LittleFS replaces directly. SD preserves an existing destination in .bak until
// promotion succeeds; failed rollback retains that backup for manual recovery.
// An existing .bak is never overwritten; one that blocks a replacement, survives
// a failed restoration or outlives a successful promotion is reported to the
// system log. This is not a power-loss atomicity/durability guarantee.
bool replaceFileGuarded(const String& temporary, const String& destination,
                        const AuthContext& ctx);
bool mkdirGuarded  (const String& path, const AuthContext& ctx);
bool rmdirGuarded  (const String& path, const AuthContext& ctx);

// The permission openGuarded applies to a write mode, without the open itself
// (which creates or truncates): WRITE on an existing path, CREATE on a missing
// one, after the same normalization, sensitive-extension and image rules.
// Denials are logged as the open's would be. Check the destination with it
// before staging a replacement, whose temporary carries a different name and
// so a different extension.
bool canWriteGuarded(const String& path, const AuthContext& ctx);

/**
 * Staged replacement of one file under the caller's identity: the shared
 * writer behind the web upload and the web file editor.
 *
 * begin() opens the staged temporary through openGuarded, so the
 * temporary is subject to the same directory rules as the destination (".tmp"
 * is not a sensitive extension; gate the destination itself first, with
 * canImport or canWriteGuarded). The caller writes to file() and checks every
 * write count. commit() flushes, closes and promotes the temporary with
 * replaceFileGuarded: the destination keeps its previous bytes until that
 * succeeds, and a failed promotion removes only the temporary. The promotion
 * runs as the caller when the rule table grants it RENAME and CREATE there
 * (and DELETE for the backup of an existing SD file); a caller with WRITE on
 * an existing destination, or CREATE on a new one, but without those bits is
 * promoted by the writer's own system identity, named `writer` (for example
 * "web.filewrite"), since that write right is the permission the replacement
 * represents. A caller with neither gets no promotion and keeps the previous
 * file. discard() closes and removes whatever this write produced, first as
 * the caller and then as the same system identity.
 *
 * The temporary is "<destination>.upload.tmp" unless that name would exceed
 * the backend's name limit, in which case a short fixed name in the same
 * directory is used, so every write is staged and the destination is never
 * opened for writing. begin() probes and opens the temporary under one lock
 * and takes "<destination>.<n>.upload.tmp" (or "~stage.<n>.upload.tmp") when
 * the plain name is already taken, so each instance owns its temporary. A
 * refused temporary open (permission, full filesystem, transient error), or
 * no free name, makes begin() return false with the destination untouched,
 * so the caller reports the failed save and the previous file survives.
 *
 * The caller holds FsLockGuard around begin(), each file() write, and
 * commit() or discard(); it need not hold it in between (the web upload
 * releases it while it receives). Both finish the write; later calls are
 * no-ops. Boot cleanup removes any ".tmp" left behind by a reset.
 */
class StagedWrite {
 public:
  explicit StagedWrite(const char* writer) : writer_(writer) {}
  bool begin(const String& destination, const AuthContext& ctx);
  File& file() { return file_; }
  // The staged temporary being written.
  const String& path() const { return temporary_; }
  bool commit(const AuthContext& ctx);
  void discard(const AuthContext& ctx);

 private:
  void removePartial(const AuthContext& ctx);
  const char* writer_;
  String destination_;
  String temporary_;
  File file_;
  bool open_ = false;
};

/**
 * Construct an AuthContext representing the trusted internal "system"
 * identity. Pass `reason` describing why this code legitimately needs to
 * bypass user-level permission checks (it shows up in [PERM] denial logs
 * and any future audit trail).
 *
 * Use deliberately and sparingly. Examples of valid use:
 *   - loadSettings() — system owns the global settings file
 *   - boot-time TLS cert read — no user identity exists yet
 *   - log rotation — internal infrastructure
 *
 * Examples of MISUSE:
 *   - "I'm a CLI handler" — use currentAuthContext() instead
 *   - "I'm in a web request" — use makeWebAuthCtx(req) instead
 *   - "I don't want to figure out the right context" — figure it out
 */
AuthContext systemAuth(const char* reason);

/**
 * Scoped variant of systemAuth: the same trusted-internal identity, but
 * additionally CONFINED to paths within `scope` (a path prefix). Any guarded
 * op outside `scope` is denied in checkPerm regardless of role — so a confined
 * system context can touch its own subtree and nothing else (e.g. it can read
 * /sd/VIDEOS but never /system/certs). Defense-in-depth: it shrinks the blast
 * radius of a bug or unsanitized path at a request-facing site to one subtree.
 *
 * Always pass a named Scopes:: constant below, never a freeform prefix string.
 */
AuthContext systemAuth(const char* scope, const char* reason);

// ── Filesystem capability scopes ─────────────────────────────────────────────
// Named confinement boundaries for systemAuth(scope, reason). Keep these aligned
// with where data actually lives; add new domains HERE rather than inlining a
// prefix at a call site, so the set of "what each context may touch" stays
// enumerable and auditable in one place.
namespace Scopes {
  constexpr const char* VIDEOS          = "/sd/VIDEOS";        // recorded AVI clips
  constexpr const char* CERTS           = "/system/certs";    // TLS certs / keys
  constexpr const char* OTA             = "/system/ota";      // signed OTA staging only
  constexpr const char* ESPNOW_RECEIVED = "/espnow/received"; // inbound files from peers
  // Capture tree root, NOT the sensors/tracks leaves: the log-stitch commands
  // mkdir "/logging_captures" itself before their subfolder, so a leaf scope
  // would reject that first call. Root is still tight enough to keep an
  // admin-invoked stitch out of /system/sys_logs, which grants systemPerms
  // PERM_ALL where admins are deliberately PERM_READ.
  constexpr const char* CAPTURES        = "/logging_captures"; // sensor/track/system captures
  // The overflow mirror of the same tree. Auth does NOT fold /sd paths onto
  // their primary — "/sd" is its own rule — so a caller that accepts either
  // tier must pick the matching scope. captureScopeFor() below does that.
  constexpr const char* CAPTURES_SD     = "/sd/logging_captures";
  // The local speech model folder (model, LM, speaker model, user vocabulary).
  // Only the user vocabulary is ever written by firmware (System_STTVocabulary).
  constexpr const char* STT_MODELS      = "/STT Models";
  constexpr const char* STT_MODELS_SD   = "/sd/STT Models";
  // Raw STT input recordings (System_STTRawCapture `sttraw`), SD only.
  constexpr const char* STT_RAW_SD      = "/sd/STT Raw";
  // The capped system logs (logins, I2C, errors, events, command audit, crash
  // history). Only appendLineWithCap and its orphan recovery write here, and
  // they confine themselves to the tier the overflow latch resolved;
  // sysLogScopeFor() below picks the twin.
  constexpr const char* SYS_LOGS        = "/system/sys_logs";
  constexpr const char* SYS_LOGS_SD     = "/sd/system/sys_logs";
  // Device state files directly under /system (settings.json, debug.json,
  // automations.json, boot_anchors.json, notifications.json, the pending-
  // credential marker) and the ".tmp" sibling every atomic writer stages
  // through. A directory scope on purpose: pathWithinScope is boundary-aware,
  // so an exact-file scope would reject that sibling.
  constexpr const char* SYSTEM_STATE    = "/system";
  // The account roster family: users.json, pending_users.json, ip_bans.json
  // and the per-user settings folder.
  constexpr const char* USERS           = "/system/users";
  // Map tiles and tracks; the only tree outside /system a backup may restore.
  constexpr const char* MAPS            = "/maps";
  // Edge Impulse models and their label files (System_EdgeImpulse MODEL_DIR).
  constexpr const char* EI_MODELS       = "/EI Models";
  // Microphone recordings (System_Microphone), on either tier.
  constexpr const char* RECORDINGS      = "/recordings";
  constexpr const char* RECORDINGS_SD   = "/sd/recordings";
  // ESP-NOW device state: devices.json, this device's identity, peer
  // identities and the bond caches (System_ESPNow, System_ESPNow_Identity).
  constexpr const char* ESPNOW_STATE    = "/system/espnow";
  // ESP-NOW frame captures (System_ESPNow ensureCaptureFile), SD only.
  constexpr const char* ESPNOW_CAPTURES = "/sd/espnow";
  // On-device LLM model folders (System_LLMBackend / System_LLMModel).
  constexpr const char* LLM_MODELS      = "/system/llm";
  constexpr const char* LLM_MODELS_SD   = "/sd/llm";
  // ESP-SR command file, snippets and models (System_ESPSR), either tier.
  constexpr const char* ESPSR           = "/ESPSR";
  constexpr const char* ESPSR_SD        = "/sd/ESPSR";
  // Lens camera snapshots (G2_Glasses cam snapshot), SD only.
  constexpr const char* PICTURES_SD     = "/sd/PICTURES";
  // Saved transcripts (sttsavetranscripts: /stt/u<id>/...), either tier. File
  // catch-up reads them (System_CatchupLedger); QuickNotes under them are
  // never offered.
  constexpr const char* TRANSCRIPTS     = "/stt";
  constexpr const char* TRANSCRIPTS_SD  = "/sd/stt";
  // File catch-up, device side: the ledger (System_CatchupLedger).
  constexpr const char* CATCHUP_STATE   = "/system/catchup";
  // File catch-up, collector side: what this hub pulled (System_CollectStore).
  constexpr const char* COLLECT_SD      = "/sd/collect";
  // NOTE: domains with user-configurable folders (images → gSettings.cameraCaptureFolder)
  // or dual SD+LittleFS locations (LLM models) can't use a static constant — they need a
  // computed scope at the call site. Add those when those domains are scoped.
}

// Pick the capture-tree scope matching the tier `path` names. Callers that take
// a user-supplied capture path (the log-stitch commands) need this so scoping
// them doesn't silently reject captures that overflowed to SD.
inline const char* captureScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::CAPTURES_SD : Scopes::CAPTURES;
}

// Tier pickers for the folders that live on either storage: each returns the
// scope constant above that holds `path`.
inline const char* llmModelScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::LLM_MODELS_SD : Scopes::LLM_MODELS;
}
inline const char* espsrScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::ESPSR_SD : Scopes::ESPSR;
}
inline const char* sttModelScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::STT_MODELS_SD : Scopes::STT_MODELS;
}

// Pick the saved-transcripts scope matching the tier `path` names.
inline const char* transcriptScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::TRANSCRIPTS_SD : Scopes::TRANSCRIPTS;
}

// Pick the recordings scope matching the tier `path` names.
inline const char* recordingScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::RECORDINGS_SD : Scopes::RECORDINGS;
}

// Pick the system-log scope matching the tier `path` names: the overflow
// mirror of /system/sys_logs is /sd/system/sys_logs.
inline const char* sysLogScopeFor(const String& path) {
  return path.startsWith("/sd/") ? Scopes::SYS_LOGS_SD : Scopes::SYS_LOGS;
}

/** True if `path` names a location inside the capture tree on either tier.
 *  Boundary-aware the same way pathWithinScope is, so "/logging_capturesX"
 *  does not pass. The log-stitch commands call this BEFORE their destructive
 *  open: their scoped systemAuth would reject an out-of-tree output anyway,
 *  but only after the "w" open had already truncated whatever was there, and
 *  the failure surfaced as a generic "cannot create". */
inline bool pathInCaptureTree(const String& path) {
  const String base = captureScopeFor(path);
  return path == base || path.startsWith(base + "/");
}

// ============================================================================
// Overflow-aware path resolution (opt-in for append-only data)
// ============================================================================

/** Where a log line should go. Overflow: the card twin of the path (the name
 *  is from before B11, when the card was only used once flash ran short).
 *  Primary: the path as given. Refused: the flash latch is set, no usable card
 *  can take the line and LittleFS free is below the global floor
 *  (logReserveBytes); outPath still names the primary path (for readers and
 *  recovery), but a writer must not append there (G-39). A latched resolution
 *  with no card above the floor is Primary. */
enum class LogTier : uint8_t { Primary, Overflow, Refused };

/** Map a primary LittleFS log path to its card twin while a card can take the
 *  line (B11: it accepts writes, has room above logcaps::kCardReserveBytes and
 *  the device key is available to seal with), and to itself otherwise. Every
 *  call asks again: a card pulled or put back flips the answer, and one
 *  [EVENT] line records each change. A path already under /sd is returned as
 *  it is.
 *
 *  Use for append-only log data that's safe to split across tiers: logs and
 *  captures. DO NOT use for state files like settings.json, users.json, or
 *  automations.json — those must stay single-source-of-truth.
 *
 *  @param primaryPath LittleFS path, e.g. "/system/log.txt"
 *  @param reserveBytes Minimum LittleFS free bytes before the flash latch
 *                     trips. The global floor (100 KB) is applied as a minimum.
 *  @param outPath Destination buffer for the resolved path
 *  @param outPathLen Size of `outPath`
 *  @return the tier the resolved path is on, or Refused (see LogTier)
 */
LogTier resolveLogPath(const char* primaryPath, size_t reserveBytes,
                       char* outPath, size_t outPathLen);

/** resolveLogPath for callers that only need the tier: true if the resolved
 *  path is on SD (overflow active), false if LittleFS (Refused included). */
bool resolveOverflowPath(const char* primaryPath, size_t reserveBytes,
                         char* outPath, size_t outPathLen);

/** The LittleFS free-space floor the overflow latch keeps for non-log writes
 *  (settings, users, automations): the one shared reserve (FILE-3). Writers
 *  that never use the SD mirror (the `log start` system log, the battery log)
 *  and the writers of whole files (photos, transcripts, QuickNotes, web
 *  uploads) check it directly. */
size_t logReserveBytes();

/** Count one log line a writer dropped because resolveLogPath returned
 *  Refused, and read the count for this boot. */
void noteLogReserveDrop();
uint32_t logReserveDrops();

/** B11: where flash-named log lines went at the last resolution, for status
 *  displays (`logtier`). */
struct LogCardStatus {
  bool     onCard;          // the card took the last line (sealed there)
  bool     cardFull;        // the card is below its reserve: lines are on flash
  bool     keyUnavailable;  // no device key: lines are on flash, unsealed
  uint32_t noKeyLines;      // resolutions kept on flash for want of the key, this boot
};
LogCardStatus logCardStatus();

/** A log append on the SD overflow mirror failed: report the card unwritable
 *  (noteSdWriteFailure) and keep the overflow path off the card for a minute
 *  even if its write probe still passes, so a damaged log tree on a card that
 *  probes fine is not retried, and re-announced, on every line (G-39). */
void noteLogOverflowSdFailure(const char* hint);

/** B11: one log's file on the card could not be opened, prepared, written or
 *  verified (`path`: its flash name or its card twin). If the card no longer
 *  takes a probe file this is noteLogOverflowSdFailure: the card as a whole.
 *  Otherwise only this log stays off the card (resolveLogPath answers flash
 *  for it) for 1, 4, 16, then 60 minutes while the retries keep failing; the
 *  other logs stay on the card and no card failure is reported. */
void noteLogCardFileFailure(const char* path, const char* hint);

/** True if the flash latch is set (LittleFS free fell below a reserve and
 *  has not recovered). NOTE: this is not the tier in use — since B11 lines go
 *  to the card whenever it can take them (logCardStatus), latch or not. */
bool isLogOverflowActive();

/** Cached LittleFS free bytes (refreshed every 2, 5 or 15 s by headroom, or
 *  after 32 KB of writes reported through noteLittleFsBytesWritten: log
 *  lines, image saves, QuickNotes, transcripts). */
size_t getCachedLittleFsFree();

/** FILE-3. The stored LittleFS free figure without refreshing it: no volume
 *  walk and no lock, so it is safe on espnow_task. It can be as old as the
 *  last log write (2 s with lines flowing, longer on a silent device), and
 *  right after a remove or rename it is the reading from before that. SIZE_MAX
 *  when nothing was sampled yet. getCachedLittleFsFree() above is unchanged:
 *  it may walk the volume under the filesystem lock. */
size_t peekLittleFsFree();

/** Which free figure an admission uses. The stored figure is an estimate
 *  when something was removed or renamed since it was read (it is then the
 *  reading from before, possibly too low) or when admitted writers took from
 *  it (noteBulkWriteTaken).
 *  Peek: the stored figure, never a walk and no lock (espnow_task). A refusal
 *    decided on an estimate asks cmd_exec for a fresh reading, so the next
 *    try is decided on the volume as it is.
 *  Refresh: the overflow check's reading, walked again when it is old (takes
 *    the filesystem lock, about 1.3 s on the P4X-EYE: never on espnow_task or
 *    just before a capture has to start).
 *  RefreshIfShort: the stored figure, and the volume is read only when that
 *    figure is an estimate and shows less room than the caller wants: for a
 *    caller that must start at once when there is room and may wait when
 *    there seems to be too little (a recording). */
enum class BulkFigure : uint8_t { Peek, Refresh, RefreshIfShort };

/** FILE-3: may a writer put `bytes` on `tier`? One rule for the writers of
 *  large files (a microphone recording, a received ESP-NOW file): on LittleFS
 *  the log reserve (logReserveBytes, 100 KB) stays free for settings, users and
 *  automations, so the answer is free >= reserve + bytes, where free is the
 *  figure less what earlier admissions took since it was read
 *  (noteBulkWriteTaken). A card always passes. An unknown free figure passes.
 *  `roomBytes`, when given, receives what LittleFS has free above the reserve
 *  (SIZE_MAX for a card or an unknown figure), so a writer that can stop early
 *  may take less than it asked for. `wantBytes` (at least `bytes`) is what
 *  such a writer would use if it were there: with Refresh or RefreshIfShort an
 *  estimate that shows less room than that is replaced by a fresh reading
 *  before `roomBytes` is filled in.
 *  A refusal is counted (bulkWriteRefusals) and logged with `why`, at most one
 *  line per 10 s. The writers with their own free-space checks (transcripts,
 *  QuickNotes, photos, web uploads) keep the same reserve, logReserveBytes(). */
bool admitBulkWrite(StorageType tier, size_t bytes, const char* why,
                    BulkFigure figure = BulkFigure::Peek, size_t* roomBytes = nullptr,
                    size_t wantBytes = 0);
/** A bulk writer says what it has put on LittleFS (a received file, each
 *  write of a streamed one) or is about to (a recording: the room it was
 *  limited to), so the next admission decided on the same stored figure sees
 *  it; and gives back what it did not use after all. No lock, no I/O. Not for
 *  a card. The count is dropped whenever the figure is read from the volume
 *  again. */
void noteBulkWriteTaken(size_t bytes);
void noteBulkWriteReturned(size_t bytes);
uint32_t bulkWriteRefusals();

/** Invalidate the cached free-space reading immediately. Call after unusual
 *  events that change free space by a lot — VFS::remove and VFS::rename do
 *  this automatically. */
void invalidateLittleFsFreeCache();

/** Tell the free-space cache that approximately `bytes` have been written
 *  to LittleFS since its last refresh. When the cumulative hint exceeds an
 *  internal threshold the cache is forced to refresh on the next query,
 *  so heavy write bursts can't sneak past a stale reading. Cheap: a single
 *  counter add. High-volume writers (appendLineWithCap, image saves) call
 *  this after a successful write. */
void noteLittleFsBytesWritten(size_t bytes);

}  // namespace VFS

#endif // SYSTEM_VFS_H
