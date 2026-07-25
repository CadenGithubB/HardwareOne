#ifndef SYSTEM_VFS_H
#define SYSTEM_VFS_H

#include <Arduino.h>
#include <FS.h>
#include "System_User.h"   // AuthContext (for the *Guarded variants)

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
// There is no automatic overflow in the routing layer itself. A call like
//   VFS::open("/system/settings.json", "w")
// will always write to LittleFS. The write may fail if LittleFS is full; it
// will NOT silently end up on SD. This is intentional: state files (users,
// settings, automations) must have a single source of truth.
//
// Overflow is OPT-IN, via `resolveOverflowPath` + a normal `VFS::open` on
// the resolved path. When LittleFS free space drops below a threshold, that
// function rewrites primary paths like "/logging_captures/foo.csv" to their SD mirror
// "/sd/logging_captures/foo.csv". The rewrite is latched until reboot so a single log
// stream doesn't split across tiers mid-session.
//
// Convention for callers:
//   - State files (settings, users, automations, mesh config, etc.)
//     → call VFS::open directly. They stay on LittleFS; writes fail cleanly
//       when flash is full rather than splitting across tiers.
//   - Append-only bulk data (logs, sensor captures, recordings)
//     → call resolveOverflowPath first to get the overflow-aware path, then
//       VFS::open / VFS::exists / VFS::rename / VFS::remove on that path.
//       Rotation and cap enforcement happens on whichever tier is active.

namespace VFS {

enum StorageType {
  INTERNAL = 0,
  SDCARD = 1,
  AUTO = 2
};

bool init();

bool isLittleFSReady();

/**
 * "Driver-level" SD availability: true iff SD.begin() has succeeded and the
 * mount hasn't been torn down. This does NOT prove files can actually be
 * written — a card can mount but fail writes (bad sector where the next
 * file happens to land, card yanked since, write-protect tab, filesystem
 * errors). For gating UI around "can we record to SD" use isSDWritable().
 */
bool isSDAvailable();

/**
 * Round-trip-verified writability. Returns true only if a write+read+delete
 * probe has succeeded on the card recently. If the cached state is false
 * but the card is mounted, a lazy re-probe is attempted — this gives free
 * auto-recovery for cards that were briefly glitchy. Cheap when cached,
 * ~a few ms when re-probing. Use this for anything that's about to do a
 * real write (video recording, log overflow, image save).
 */
bool isSDWritable();

/**
 * Callers that attempt an SD write and see it fail should call this with
 * a short reason string. It invalidates the cached writable flag so the
 * next isSDWritable() query re-probes. Cheap.
 */
void noteSDWriteFailure(const char* hint);

// SD card management
bool remountSD();
bool unmountSD();
bool formatSD();

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

bool exists(const String& path);
File open(const String& path, const char* mode = FILE_READ, bool create = false);
bool mkdir(const String& path);
bool remove(const String& path);
bool rename(const String& pathFrom, const String& pathTo);
bool rmdir(const String& path);

bool getStats(StorageType type, uint64_t& totalBytes, uint64_t& usedBytes, uint64_t& freeBytes);

// ============================================================================
// Guarded VFS — single enforcement point for filesystem permissions
// ============================================================================
// Every guarded call runs the same pipeline:
//   1. normalizeFsPath() — reject ".." traversal, collapse double slashes,
//      strip trailing slash. Failure → deny + log.
//   2. canX(path, ctx) — three-role permission lookup. Failure → log + deny.
//   3. Dispatch to the underlying VFS::open/exists/etc.
//
// Use these instead of raw VFS::open / LittleFS.open / etc. anywhere you
// have an AuthContext available (web handlers, CLI handlers via
// currentAuthContext(), etc.). For internal trusted code, construct a
// context via systemAuth("reason") and pass it through.
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
bool mkdirGuarded  (const String& path, const AuthContext& ctx);
bool rmdirGuarded  (const String& path, const AuthContext& ctx);

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
  constexpr const char* ESPNOW_RECEIVED = "/espnow/received"; // inbound files from peers
  // NOTE: domains with user-configurable folders (images → gSettings.cameraCaptureFolder)
  // or dual SD+LittleFS locations (LLM models) can't use a static constant — they need a
  // computed scope at the call site. Add those when those domains are scoped.
}

// ============================================================================
// Overflow-aware path resolution (opt-in for append-only data)
// ============================================================================

/** Map a primary LittleFS log path to itself, OR to its /sd mirror once
 *  LittleFS free space drops below `reserveBytes`. Once the overflow latch
 *  fires, all subsequent resolutions return the SD mirror (until reboot).
 *
 *  Use for append-only bulk data that's safe to split across tiers: logs,
 *  captures, recordings. DO NOT use for state files like settings.json,
 *  users.json, or automations.json — those must stay single-source-of-truth.
 *
 *  @param primaryPath LittleFS path, e.g. "/system/log.txt"
 *  @param reserveBytes Minimum LittleFS free bytes before overflow triggers.
 *                     The global floor (100 KB) is applied as a minimum.
 *  @param outPath Destination buffer for the resolved path
 *  @param outPathLen Size of `outPath`
 *  @return true if the resolved path is on SD (overflow active), false if LittleFS
 */
bool resolveOverflowPath(const char* primaryPath, size_t reserveBytes,
                         char* outPath, size_t outPathLen);

/** True if the overflow latch has fired this session. */
bool isLogOverflowActive();

/** Cached LittleFS free bytes (refreshed at most every 2s). */
size_t getCachedLittleFsFree();

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

// SD card command registry (for gCommandModules in System_Utils.cpp)
struct CommandEntry;  // Forward declaration
extern const CommandEntry sdCommands[];
extern const size_t sdCommandsCount;

#endif // SYSTEM_VFS_H
