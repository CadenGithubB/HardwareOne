#pragma once
// Shared in-memory fake of the HardwareOne storage stack for host suites.
//
// What it models
//   One in-memory filesystem with two backends behind the production routing
//   rule: paths under "/sd" are the SD card (FAT), everything else is LittleFS.
//   Both roots exist after reset(). It supplies directories, a File handle
//   class with the method names production code calls (Arduino FS/Stream/Print
//   surface: read/write/print/printf/readBytes/readString/seek/position/size/
//   available/flush/close/getWriteError/name/path/isDirectory/openNextFile/
//   rewindDirectory/getLastWrite), backend policy knobs, fault injection, a
//   guarded-versus-unguarded lock seam and a call log. It is not a model of
//   power loss, wear, caching or the real allocators.
//
// Dependencies (one approach, used everywhere in this header)
//   C++17 standard library plus the host `String` double from
//   test/host/web_batch_stubs/Arduino.h: the runner passes
//   `-I test/host/web_batch_stubs` and the harness defines
//   `void webBatchTestStringAppend() {}` as the existing suites do. Only
//   `String(const char*)`, `c_str()` and `length()` are required, so a harness
//   that compiles the real Arduino WString instead can point `-I` at a
//   harness-local directory whose Arduino.h includes <WString.h>.
//   AuthContext is a minimal double with the real field names the forwarders
//   read (`user`, `path`, `scope`); define FAKE_VFS_EXTERNAL_AUTH_CONTEXT
//   before including when the harness has already extracted the real struct.
//   FsPerm is not needed by any forwarder signature and is not provided.
//
// Opting in
//   1. Include this header after the String double is visible.
//   2. #define FAKE_VFS_PROVIDE_VFS_NAMESPACE before the include to get the
//      production `VFS::` signatures (openGuarded, open, exists[Guarded],
//      remove[Guarded], rename[Guarded], replaceFileGuarded, mkdir[Guarded],
//      rmdir[Guarded], isSDAvailable, isSDWritable, isLittleFSReady, getStats,
//      getStorageType, normalize, stripSdPrefix, isStagingName, systemAuth) as
//      thin forwarders onto fake_vfs::fs(). Leave it out when the harness
//      extracts the real VFS helpers and only needs the File class.
//   3. #define FAKE_VFS_LOCK_DOUBLE before the include to also get the
//      FreeRTOS semaphore double the real FsLockGuard compiles against
//      (TickType_t, portMAX_DELAY, pdTRUE, SemaphoreHandle_t, gFsMutex,
//      xSemaphoreTake/Give, isHeldByCurrentTask, isFsLockedByCurrentTask),
//      wired to the fake's lock seam. Without it, set fs().lockHeld to a
//      predicate that reports the harness's own lock state.
//   4. The POSIX stat seam: wrap the extracted block in
//        #define stat FakeVfsStat
//        ... extracted code calling ::stat("/littlefs" + path, &info) ...
//        #undef stat
//      FakeVfsStat accepts "/littlefs/...", "/sd/..." and plain VFS paths.
//
// Usage example (a harness for a users.json writer)
//   #define FAKE_VFS_PROVIDE_VFS_NAMESPACE
//   #define FAKE_VFS_LOCK_DOUBLE
//   #include <Arduino.h>            // web_batch_stubs String double
//   #include "fake_vfs.h"
//   void webBatchTestStringAppend() {}
//   // INSERT_FS_GUARD           <- extracted real FsLockGuard
//   // INSERT_WRITERS            <- extracted production functions
//   int main() {
//     auto& fs = fake_vfs::fs();
//     fs.reset();
//     fs.mkdirAll("/system/users");
//     fs.put("/system/users/users.json", "[]");
//     fs.faults.shortWriteAfter["/system/users/users.json.tmp"] = 7;
//     bool ok = saveUsersJson(...);                 // production code
//     assert(!ok && fs.text("/system/users/users.json") == "[]");
//     assert(fs.unlockedCalls() == 0);              // every call under the lock
//     assert(fs.count("rename") == 0);              // no promotion of a short temp
//   }
//
// Interleaving: fs().onCall runs with each call as it is logged (before it
// executes) and may issue further operations, so a suite can let a second
// writer land inside a read-modify-write and check whether the lock was held
// (the Call's `locked` field) at that point.
//
// Threading: the fake is single-threaded; suites that interleave threads must
// serialize their own calls into it.

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef FAKE_VFS_EXTERNAL_AUTH_CONTEXT
// Minimal double with the production field names the forwarders read. The
// real struct also carries `transport` (CommandSource); nothing here needs it.
struct AuthContext {
  String path;   // "system:<reason>" for systemAuth, the request path otherwise
  String ip;
  String user;   // "system" for systemAuth
  String sid;
  void* opaque = nullptr;
  String scope;  // optional path-prefix confinement
};
#endif

#ifndef FILE_READ
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
#endif
enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class File;

namespace fake_vfs {

enum class Backend { LittleFS = 0, SD = 1 };

inline const char* backendName(Backend b) { return b == Backend::SD ? "sd" : "littlefs"; }

// How a File reports name(): LittleFS and the SD FAT backend both return leaf
// names on current cores; `BackendRooted` reproduces the older directory-rooted
// form ("/VIDEOS/clip.avi") that VFS::DirectoryReader must also tolerate.
enum class NameStyle { Leaf, BackendRooted };

struct Policy {
  // unlink() of a directory is refused on both backends (EISDIR). Set true to
  // model a backend that silently removes an empty directory on remove().
  bool removeUnlinksDirectory = false;
  // LittleFS rename replaces an existing destination file; FAT refuses.
  bool littlefsRenameOverExisting = true;
  bool sdRenameOverExisting = false;
  // 0 = unlimited. LittleFS on the device limits each name component to 63
  // bytes; set to 63 to make longer new names fail on LittleFS.
  size_t littlefsNameLimit = 0;
  // open("w"/"a") needs an existing parent directory; with create=true the
  // Arduino backends create missing parents. Set false to refuse even then.
  bool createFlagMakesParents = true;
  NameStyle nameStyle = NameStyle::Leaf;
};

// A path set: "*" matches every path. Paths are compared after normalisation.
struct PathSet {
  std::set<std::string> paths;
  bool matches(const std::string& path) const {
    return paths.count("*") != 0 || paths.count(path) != 0;
  }
  void add(const std::string& path) { paths.insert(path); }
  void clear() { paths.clear(); }
  bool empty() const { return paths.empty(); }
};

struct Faults {
  PathSet open;                                 // open() returns an empty File
  PathSet mkdir;                                // mkdir() returns false
  PathSet rmdir;                                // rmdir() returns false
  PathSet remove;                               // remove() returns false, file kept
  PathSet rename;                               // rename(from, ...) returns false (keyed by source)
  PathSet writeError;                           // getWriteError() reports 1 after the first write
  PathSet flush;                                // flush() sets the write error
  PathSet seek;                                 // seek() returns false, position unchanged
  std::map<std::string, size_t> shortWriteAfter; // accept at most N bytes in total per file
  std::map<std::string, int> stat;              // stat() fails with this errno
  void clear() {
    open.clear(); mkdir.clear(); rmdir.clear(); remove.clear(); rename.clear();
    writeError.clear(); flush.clear(); seek.clear(); shortWriteAfter.clear(); stat.clear();
  }
};

struct Node {
  std::string path;          // normalised VFS path ("/system/users/users.json")
  Backend backend = Backend::LittleFS;
  bool directory = false;
  bool alive = true;
  std::string bytes;
  size_t accepted = 0;       // bytes accepted so far, for shortWriteAfter
  bool writeError = false;
  time_t modified = 0;
  unsigned openHandles = 0;
};
using NodePtr = std::shared_ptr<Node>;

struct Call {
  std::string op;      // open, close, exists, remove, rename, replace, mkdir, rmdir, stat, write, flush, ...
  std::string path;
  std::string other;   // rename destination, open mode
  bool guarded = false; // through a *Guarded forwarder
  bool locked = false;  // lockHeld() at the time of the call
  bool ok = true;
  std::string authUser, authReason, authScope;
};

struct Handle {
  NodePtr node;
  size_t offset = 0;
  size_t listCursor = 0;
  bool readable = false, writable = false, append = false;
  bool open = true;
  std::vector<NodePtr> children;  // snapshot taken on first openNextFile
  bool childrenLoaded = false;
  ~Handle();  // an open handle dropped without close() closes implicitly
};

struct Capacity {
  bool available = true;
  bool writable = true;
  uint64_t total = 4u * 1024u * 1024u;
  uint64_t used = 0;
  bool statsFail = false;
};

class Fs {
 public:
  Policy policy;
  Faults faults;
  Capacity capacity[2];
  std::function<bool()> lockHeld = [] { return false; };
  // Run with each call as it is logged, before it executes. The hook may issue
  // further operations (they log after this one); a suite uses it to model a
  // writer on another task landing inside a read-modify-write. Cleared by reset().
  std::function<void(const Call&)> onCall;
  std::vector<Call> log;
  // Reported by isSDAvailable/isLittleFSReady; all /sd operations fail when SD
  // is unavailable and all LittleFS operations when it is not ready.
  bool& sdAvailable() { return capacity[1].available; }
  bool& littlefsReady() { return capacity[0].available; }
  unsigned sdWriteFailureNotes = 0;
  size_t littlefsBytesNoted = 0;
  unsigned freeCacheInvalidations = 0;
  unsigned implicitCloses = 0;  // handles released by destruction, not close()
  time_t now = 1700000000;

  Fs() { reset(); }

  void reset() {
    policy = Policy{}; faults.clear(); log.clear(); onCall = nullptr;
    capacity[0] = Capacity{}; capacity[1] = Capacity{};
    sdWriteFailureNotes = 0; littlefsBytesNoted = 0; freeCacheInvalidations = 0; implicitCloses = 0;
    nodes_.clear(); liveHandles_ = 0;
    makeDir("/", Backend::LittleFS);
    makeDir("/sd", Backend::SD);
  }

  // --- setup helpers (not logged, no lock check) ---------------------------
  bool mkdirAll(const std::string& rawPath) {
    const std::string path = normalizePath(rawPath);
    if (path.empty()) return false;
    std::string current;
    size_t pos = 1;
    while (pos <= path.size()) {
      const size_t next = path.find('/', pos);
      current = path.substr(0, next == std::string::npos ? path.size() : next);
      NodePtr existing = find(current);
      if (!existing) makeDir(current, backendOf(current));
      else if (!existing->directory) return false;
      if (next == std::string::npos) break;
      pos = next + 1;
    }
    return true;
  }
  void put(const std::string& rawPath, const std::string& bytes) {
    const std::string path = normalizePath(rawPath);
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) mkdirAll(slash == 0 ? "/" : path.substr(0, slash));
    NodePtr node = find(path);
    if (!node || node->directory) {
      node = std::make_shared<Node>();
      node->path = path; node->backend = backendOf(path);
      nodes_[path] = node;
    }
    node->bytes = bytes; node->accepted = bytes.size(); node->modified = now;
  }
  bool exists(const std::string& rawPath) const { return find(normalizePath(rawPath)) != nullptr; }
  bool isDirectory(const std::string& rawPath) const {
    const NodePtr node = find(normalizePath(rawPath));
    return node && node->directory;
  }
  std::string text(const std::string& rawPath) const {
    const NodePtr node = find(normalizePath(rawPath));
    return node && !node->directory ? node->bytes : std::string();
  }
  std::vector<std::string> list(const std::string& rawPath) const {
    std::vector<std::string> names;
    const std::string dir = normalizePath(rawPath);
    for (const auto& entry : nodes_) {
      if (entry.second->path == dir) continue;
      if (parentOf(entry.second->path) == dir) names.push_back(leafOf(entry.second->path));
    }
    std::sort(names.begin(), names.end());
    return names;
  }
  std::vector<std::string> allPaths() const {
    std::vector<std::string> paths;
    for (const auto& entry : nodes_) paths.push_back(entry.first);
    return paths;
  }
  unsigned liveHandles() const { return liveHandles_; }

  // --- call log queries ------------------------------------------------------
  unsigned count(const char* op) const {
    unsigned n = 0;
    for (const auto& call : log) if (call.op == op) ++n;
    return n;
  }
  unsigned count(const char* op, const std::string& path) const {
    unsigned n = 0;
    const std::string wanted = normalizePath(path);
    for (const auto& call : log) if (call.op == op && call.path == wanted) ++n;
    return n;
  }
  unsigned lockedCalls() const { unsigned n = 0; for (const auto& c : log) if (c.locked) ++n; return n; }
  unsigned unlockedCalls() const { unsigned n = 0; for (const auto& c : log) if (!c.locked) ++n; return n; }
  unsigned guardedCalls() const { unsigned n = 0; for (const auto& c : log) if (c.guarded) ++n; return n; }
  unsigned unguardedCalls() const { unsigned n = 0; for (const auto& c : log) if (!c.guarded) ++n; return n; }
  std::vector<std::string> ops() const {
    std::vector<std::string> out;
    for (const auto& call : log) out.push_back(call.op);
    return out;
  }
  const Call* last(const char* op) const {
    for (auto it = log.rbegin(); it != log.rend(); ++it) if (it->op == op) return &*it;
    return nullptr;
  }

  // --- path helpers (production normalize/getStorageType semantics) --------
  static std::string normalizePath(const std::string& raw) {
    std::string p = raw;
    const auto first = p.find_first_not_of(" \t\r\n");
    p = first == std::string::npos ? std::string() : p.substr(first, p.find_last_not_of(" \t\r\n") - first + 1);
    if (p.empty()) return "/";
    if (p[0] != '/') p.insert(p.begin(), '/');
    std::string collapsed;
    for (char c : p) if (!(c == '/' && !collapsed.empty() && collapsed.back() == '/')) collapsed.push_back(c);
    p = collapsed;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
  }
  static bool hasTraversal(const std::string& path) {
    size_t pos = 0;
    while (pos <= path.size()) {
      const size_t next = path.find('/', pos);
      const std::string part = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
      if (part == "..") return true;
      if (next == std::string::npos) break;
      pos = next + 1;
    }
    return false;
  }
  static Backend backendOf(const std::string& normalized) {
    return (normalized == "/sd" || normalized.rfind("/sd/", 0) == 0) ? Backend::SD : Backend::LittleFS;
  }
  static std::string stripSd(const std::string& normalized) {
    if (normalized == "/sd") return "/";
    if (normalized.rfind("/sd/", 0) == 0) return normalized.substr(3);
    return normalized;
  }
  static std::string parentOf(const std::string& normalized) {
    if (normalized == "/") return "";
    const size_t slash = normalized.rfind('/');
    return slash == 0 ? "/" : normalized.substr(0, slash);
  }
  static std::string leafOf(const std::string& normalized) {
    const size_t slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
  }
  // Physical paths as the device sees them: /littlefs/<path> and /sd/<path>.
  static std::string fromPhysical(const std::string& physical) {
    if (physical == "/littlefs") return "/";
    if (physical.rfind("/littlefs/", 0) == 0) return physical.substr(std::strlen("/littlefs"));
    return physical;
  }

  // --- operations (logged; used by File and the VFS forwarders) -----------
  struct Auth { std::string user, reason, scope; bool guarded = false; };

  ::File open(const std::string& rawPath, const char* mode, bool create, const Auth& auth);

  bool exists(const std::string& rawPath, const Auth& auth) {
    const std::string path = normalizePath(rawPath);
    const size_t call = record("exists", path, "", auth);
    if (!admitted(path)) return finish(call, false);
    return finish(call, find(path) != nullptr);
  }

  bool mkdir(const std::string& rawPath, const Auth& auth) {
    const std::string path = normalizePath(rawPath);
    const size_t call = record("mkdir", path, "", auth);
    if (!admitted(path) || faults.mkdir.matches(path) || path == "/" || path == "/sd") return finish(call, false);
    if (find(path)) return finish(call, false);  // EEXIST for files and directories alike
    const NodePtr parent = find(parentOf(path));
    if (!parent || !parent->directory || !nameAllowed(path)) return finish(call, false);
    makeDir(path, backendOf(path));
    return finish(call, true);
  }

  bool rmdir(const std::string& rawPath, const Auth& auth) {
    const std::string path = normalizePath(rawPath);
    const size_t call = record("rmdir", path, "", auth);
    if (!admitted(path) || faults.rmdir.matches(path) || path == "/" || path == "/sd") return finish(call, false);
    const NodePtr node = find(path);
    if (!node || !node->directory || !list(path).empty()) return finish(call, false);
    erase(node);
    return finish(call, true);
  }

  bool remove(const std::string& rawPath, const Auth& auth) {
    const std::string path = normalizePath(rawPath);
    const size_t call = record("remove", path, "", auth);
    if (!admitted(path) || faults.remove.matches(path) || path == "/" || path == "/sd") return finish(call, false);
    const NodePtr node = find(path);
    if (!node) return finish(call, false);
    if (node->directory) {
      if (!policy.removeUnlinksDirectory || !list(path).empty()) return finish(call, false);
    }
    erase(node);
    ++freeCacheInvalidations;
    return finish(call, true);
  }

  bool rename(const std::string& rawFrom, const std::string& rawTo, const Auth& auth) {
    const std::string from = normalizePath(rawFrom), to = normalizePath(rawTo);
    const size_t call = record("rename", from, to, auth);
    if (!admitted(from) || !admitted(to) || faults.rename.matches(from)) return finish(call, false);
    if (from == "/" || from == "/sd" || to == "/" || to == "/sd" || from == to) return finish(call, false);
    if (backendOf(from) != backendOf(to)) return finish(call, false);
    const NodePtr source = find(from);
    if (!source) return finish(call, false);
    const NodePtr toParent = find(parentOf(to));
    if (!toParent || !toParent->directory || !nameAllowed(to)) return finish(call, false);
    if (to.rfind(from + "/", 0) == 0) return finish(call, false);  // into its own subtree
    const NodePtr existing = find(to);
    if (existing) {
      if (existing->directory || source->directory) return finish(call, false);
      const bool allowed = backendOf(to) == Backend::SD ? policy.sdRenameOverExisting
                                                        : policy.littlefsRenameOverExisting;
      if (!allowed) return finish(call, false);
      erase(existing);
    }
    // Move the node and, for a directory, its subtree.
    std::vector<NodePtr> moving;
    for (const auto& entry : nodes_) {
      if (entry.first == from || entry.first.rfind(from + "/", 0) == 0) moving.push_back(entry.second);
    }
    for (const NodePtr& node : moving) {
      nodes_.erase(node->path);
      node->path = to + node->path.substr(from.size());
      nodes_[node->path] = node;
    }
    ++freeCacheInvalidations;
    return finish(call, true);
  }

  // Production replaceFileGuarded semantics: same backend, LittleFS renames
  // directly, SD keeps the previous destination in .bak until promotion.
  bool replaceFile(const std::string& rawFrom, const std::string& rawTo, const Auth& auth) {
    const std::string from = normalizePath(rawFrom), to = normalizePath(rawTo);
    const size_t call = record("replace", from, to, auth);
    if (hasTraversal(from) || hasTraversal(to) || from == to) return finish(call, false);
    if (backendOf(from) != backendOf(to)) return finish(call, false);
    if (backendOf(from) != Backend::SD || !exists(to, auth)) return finish(call, rename(from, to, auth));
    const NodePtr previous = find(to);
    if (previous->directory) return finish(call, false);
    const std::string backup = to + ".bak";
    if (backup == from || exists(backup, auth)) return finish(call, false);
    if (!rename(to, backup, auth)) return finish(call, false);
    if (!rename(from, to, auth)) {
      rename(backup, to, auth);
      return finish(call, false);
    }
    remove(backup, auth);  // a retained backup is reported by production, not fatal here
    return finish(call, true);
  }

  struct StatInfo { size_t size = 0; time_t modified = 0; bool directory = false; };
  // Returns 0 or -1 and sets errno, like POSIX stat.
  int stat(const std::string& physicalOrVfsPath, StatInfo* out) {
    const std::string path = normalizePath(fromPhysical(physicalOrVfsPath));
    const size_t call = record("stat", path, "", Auth{});
    const auto fault = faults.stat.find(path);
    if (fault != faults.stat.end()) { errno = fault->second; finish(call, false); return -1; }
    if (!admitted(path)) { errno = EIO; finish(call, false); return -1; }
    const NodePtr node = find(path);
    if (!node) { errno = ENOENT; finish(call, false); return -1; }
    if (out) { out->size = node->directory ? 0 : node->bytes.size(); out->modified = node->modified; out->directory = node->directory; }
    return finish(call, true) ? 0 : -1;
  }

  bool getStats(Backend backend, uint64_t& total, uint64_t& used, uint64_t& free) {
    Capacity& c = capacity[backend == Backend::SD ? 1 : 0];
    const size_t call = record("stats", backend == Backend::SD ? "/sd" : "/", "", Auth{});
    if (!c.available || c.statsFail || c.total == 0) return finish(call, false);
    uint64_t bytes = c.used;
    for (const auto& entry : nodes_) if (entry.second->backend == backend && !entry.second->directory) bytes += entry.second->bytes.size();
    total = c.total; used = std::min(bytes, c.total); free = total - used;
    return finish(call, true);
  }

  // --- used by File ---------------------------------------------------------
  // Returns the log index; nested operations may append to the log, so callers
  // keep the index and finish() it rather than holding a reference.
  size_t record(const char* op, const std::string& path, const std::string& other, const Auth& auth) {
    Call call;
    call.op = op; call.path = path; call.other = other;
    call.guarded = auth.guarded; call.locked = lockHeld ? lockHeld() : false;
    call.authUser = auth.user; call.authReason = auth.reason; call.authScope = auth.scope;
    log.push_back(call);
    const size_t index = log.size() - 1;
    if (onCall) onCall(Call(call));  // a copy: the hook may append to the log
    return index;
  }
  bool finish(size_t index, bool ok) { log[index].ok = ok; return ok; }
  void handleOpened() { ++liveHandles_; }
  void handleClosed() { if (liveHandles_) --liveHandles_; }
  std::vector<NodePtr> childrenOf(const std::string& dir) const {
    std::vector<NodePtr> children;
    for (const auto& entry : nodes_) {
      if (entry.second->path != dir && parentOf(entry.second->path) == dir) children.push_back(entry.second);
    }
    return children;
  }

 private:
  std::map<std::string, NodePtr> nodes_;
  unsigned liveHandles_ = 0;

  NodePtr find(const std::string& normalized) const {
    const auto it = nodes_.find(normalized);
    return it == nodes_.end() ? nullptr : it->second;
  }
  void makeDir(const std::string& path, Backend backend) {
    NodePtr node = std::make_shared<Node>();
    node->path = path; node->backend = backend; node->directory = true; node->modified = now;
    nodes_[path] = node;
  }
  void erase(const NodePtr& node) { node->alive = false; nodes_.erase(node->path); }
  bool admitted(const std::string& normalized) const {
    if (hasTraversal(normalized)) return false;
    return capacity[backendOf(normalized) == Backend::SD ? 1 : 0].available;
  }
  bool nameAllowed(const std::string& normalized) const {
    if (policy.littlefsNameLimit == 0 || backendOf(normalized) == Backend::SD) return true;
    return leafOf(normalized).size() <= policy.littlefsNameLimit;
  }
  friend class ::File;
};

inline Fs& fs() {
  static Fs instance;
  return instance;
}

inline Handle::~Handle() {
  if (!open) return;
  open = false;
  if (node && node->openHandles) --node->openHandles;
  Fs& owner = fs();
  owner.handleClosed();
  ++owner.implicitCloses;
}

}  // namespace fake_vfs

// Arduino-style file handle. Copies share one handle, as in the Arduino core,
// and the handle stays open until close(); dropping every copy closes it.
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<fake_vfs::Handle> handle) : handle_(std::move(handle)) {}
  File(const File&) = default;
  File& operator=(const File&) = default;
  File(File&&) = default;
  File& operator=(File&&) = default;
  ~File() = default;

  explicit operator bool() const { return handle_ && handle_->open; }

  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) {
    auto& fs = fake_vfs::fs();
    const size_t call = fs.record("write", pathOrEmpty(), "", {});
    if (!*this || !handle_->writable || handle_->node->directory) { fs.finish(call, false); return 0; }
    fake_vfs::Node& node = *handle_->node;
    size_t accepted = count;
    const auto cap = fs.faults.shortWriteAfter.find(node.path);
    if (cap != fs.faults.shortWriteAfter.end()) {
      accepted = cap->second > node.accepted ? std::min(count, cap->second - node.accepted) : 0;
    }
    if (handle_->append) handle_->offset = node.bytes.size();
    if (handle_->offset > node.bytes.size()) node.bytes.resize(handle_->offset, '\0');
    node.bytes.replace(handle_->offset, std::min(accepted, node.bytes.size() - handle_->offset),
                       reinterpret_cast<const char*>(data), accepted);
    handle_->offset += accepted;
    node.accepted += accepted;
    node.modified = fs.now;
    if (fs.faults.writeError.matches(node.path)) node.writeError = true;
    if (accepted < count) fs.finish(call, false);
    return accepted;
  }
  size_t print(const char* text) { return text ? write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)) : 0; }
  size_t print(const String& text) { return write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length()); }
  size_t print(const std::string& text) { return write(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  size_t print(int value) { return printf("%d", value); }
  size_t print(unsigned value) { return printf("%u", value); }
  size_t print(long value) { return printf("%ld", value); }
  size_t print(unsigned long value) { return printf("%lu", value); }
  size_t println() { return print("\r\n"); }
  size_t println(const char* text) { return print(text) + println(); }
  size_t println(const String& text) { return print(text) + println(); }
  size_t println(const std::string& text) { return print(text) + println(); }
  size_t println(char c) { return print(c) + println(); }
  size_t println(int value) { return print(value) + println(); }
  size_t println(unsigned value) { return print(value) + println(); }
  size_t println(long value) { return print(value) + println(); }
  size_t println(unsigned long value) { return print(value) + println(); }
  size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    char stack[256];
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(stack, sizeof(stack), format, args);
    va_end(args);
    if (needed < 0) { va_end(copy); return 0; }
    if (static_cast<size_t>(needed) < sizeof(stack)) { va_end(copy); return print(stack); }
    std::string large(static_cast<size_t>(needed) + 1, '\0');
    std::vsnprintf(&large[0], large.size(), format, copy);
    va_end(copy);
    large.pop_back();
    return print(large);
  }

  int read() {
    uint8_t value;
    return read(&value, 1) == 1 ? value : -1;
  }
  size_t read(uint8_t* out, size_t count) {
    fake_vfs::fs().record("read", pathOrEmpty(), "", {});
    if (!*this || !handle_->readable || handle_->node->directory) return 0;
    const std::string& bytes = handle_->node->bytes;
    if (handle_->offset >= bytes.size()) return 0;
    const size_t n = std::min(count, bytes.size() - handle_->offset);
    std::memcpy(out, bytes.data() + handle_->offset, n);
    handle_->offset += n;
    return n;
  }
  size_t readBytes(char* out, size_t count) { return read(reinterpret_cast<uint8_t*>(out), count); }
  size_t readBytes(uint8_t* out, size_t count) { return read(out, count); }
  int peek() {
    if (!*this || !handle_->readable || handle_->node->directory) return -1;
    const std::string& bytes = handle_->node->bytes;
    return handle_->offset < bytes.size() ? static_cast<unsigned char>(bytes[handle_->offset]) : -1;
  }
  int available() {
    if (!*this || handle_->node->directory) return 0;
    const size_t size = handle_->node->bytes.size();
    return handle_->offset < size ? static_cast<int>(size - handle_->offset) : 0;
  }
  String readString() {
    std::string out;
    int c;
    while ((c = read()) >= 0) out.push_back(static_cast<char>(c));
    return String(out.c_str());
  }
  String readStringUntil(char terminator) {
    std::string out;
    int c;
    while ((c = read()) >= 0 && c != terminator) out.push_back(static_cast<char>(c));
    return String(out.c_str());
  }

  bool seek(uint32_t pos, SeekMode mode) {
    if (!*this || handle_->node->directory) return false;
    if (fake_vfs::fs().faults.seek.matches(handle_->node->path)) return false;
    const size_t size = handle_->node->bytes.size();
    size_t target;
    if (mode == SeekSet) target = pos;
    else if (mode == SeekCur) target = handle_->offset + pos;
    else target = size + pos;
    if (target > size) return false;
    handle_->offset = target;
    return true;
  }
  bool seek(uint32_t pos) { return seek(pos, SeekSet); }
  size_t position() const { return *this ? handle_->offset : static_cast<size_t>(-1); }
  size_t size() const { return *this && !handle_->node->directory ? handle_->node->bytes.size() : 0; }
  bool setBufferSize(size_t) { return static_cast<bool>(*this); }

  void flush() {
    auto& fs = fake_vfs::fs();
    const size_t call = fs.record("flush", pathOrEmpty(), "", {});
    if (!*this) { fs.finish(call, false); return; }
    if (fs.faults.flush.matches(handle_->node->path)) { handle_->node->writeError = true; fs.finish(call, false); }
  }
  int getWriteError() const { return *this && handle_->node->writeError ? 1 : 0; }
  void clearWriteError() { if (*this) handle_->node->writeError = false; }

  void close() {
    if (!handle_) return;
    auto& fs = fake_vfs::fs();
    fs.record("close", handle_->node->path, "", {});
    if (handle_->open) {
      handle_->open = false;
      if (handle_->node->openHandles) --handle_->node->openHandles;
      fs.handleClosed();
    }
    handle_.reset();
  }

  const char* path() const { return handle_ ? handle_->node->path.c_str() : ""; }
  const char* name() const {
    if (!handle_) return "";
    const fake_vfs::Node& node = *handle_->node;
    if (fake_vfs::fs().policy.nameStyle == fake_vfs::NameStyle::BackendRooted) {
      nameBuffer_ = fake_vfs::Fs::stripSd(node.path);
    } else {
      nameBuffer_ = fake_vfs::Fs::leafOf(node.path);
    }
    return nameBuffer_.c_str();
  }
  bool isDirectory() const { return *this && handle_->node->directory; }
  time_t getLastWrite() { return *this ? handle_->node->modified : 0; }

  File openNextFile(const char* mode = FILE_READ) {
    auto& fs = fake_vfs::fs();
    fs.record("openNext", pathOrEmpty(), mode, {});
    if (!*this || !handle_->node->directory) return File();
    if (!handle_->childrenLoaded) {
      handle_->children = fs.childrenOf(handle_->node->path);
      handle_->childrenLoaded = true;
    }
    while (handle_->listCursor < handle_->children.size()) {
      fake_vfs::NodePtr child = handle_->children[handle_->listCursor++];
      if (!child->alive) continue;
      auto handle = std::make_shared<fake_vfs::Handle>();
      handle->node = child; handle->readable = true;
      ++child->openHandles;
      fs.handleOpened();
      return File(handle);
    }
    return File();
  }
  void rewindDirectory() { if (handle_) { handle_->listCursor = 0; handle_->childrenLoaded = false; } }

 private:
  std::shared_ptr<fake_vfs::Handle> handle_;
  mutable std::string nameBuffer_;
  std::string pathOrEmpty() const { return handle_ ? handle_->node->path : std::string(); }
};

namespace fake_vfs {

inline ::File Fs::open(const std::string& rawPath, const char* mode, bool create, const Auth& auth) {
  const std::string path = normalizePath(rawPath);
  const std::string modeText = mode ? mode : "r";
  const size_t call = record("open", path, modeText, auth);
  auto refused = [&] { finish(call, false); return ::File(); };
  if (!admitted(path) || faults.open.matches(path)) return refused();
  const bool plus = modeText.find('+') != std::string::npos;
  const bool writing = modeText.find('w') != std::string::npos;
  const bool appending = modeText.find('a') != std::string::npos;
  const bool reading = modeText.find('r') != std::string::npos;
  NodePtr node = find(path);
  if (writing || appending) {
    if (node && node->directory) return refused();
    if (!node) {
      if (path == "/" || path == "/sd" || !nameAllowed(path)) return refused();
      const std::string parentPath = parentOf(path);
      NodePtr parent = find(parentPath);
      if (!parent) {
        if (!create || !policy.createFlagMakesParents) return refused();
        if (!mkdirAll(parentPath)) return refused();
        parent = find(parentPath);
      }
      if (!parent->directory) return refused();
      node = std::make_shared<Node>();
      node->path = path; node->backend = backendOf(path); node->modified = now;
      nodes_[path] = node;
    }
    if (writing) { node->bytes.clear(); node->accepted = 0; node->writeError = false; }
  } else {
    if (!node) return refused();
  }
  finish(call, true);
  auto handle = std::make_shared<Handle>();
  handle->node = node;
  handle->readable = reading || plus || node->directory;
  handle->writable = writing || appending || (reading && plus);
  handle->append = appending;
  handle->offset = 0;
  ++node->openHandles;
  handleOpened();
  return ::File(handle);
}

// Minimal struct and function sharing one name so an extracted block can use
// `#define stat FakeVfsStat` around its `struct stat info; ::stat(path, &info)`.
}  // namespace fake_vfs

struct FakeVfsStat {
  size_t st_size = 0;
  time_t st_mtime = 0;
  bool directory = false;
};
inline int FakeVfsStat(const char* physicalPath, struct FakeVfsStat* out) {
  fake_vfs::Fs::StatInfo info;
  const int result = fake_vfs::fs().stat(physicalPath ? physicalPath : "", &info);
  if (result == 0 && out) { out->st_size = info.size; out->st_mtime = info.modified; out->directory = info.directory; }
  return result;
}

#ifdef FAKE_VFS_LOCK_DOUBLE
// FreeRTOS semaphore double for the real FsLockGuard (extracted from
// System_Mutex.h/.cpp). One non-recursive mutex; the fake's lock seam reads it.
using TickType_t = uint32_t;
inline constexpr TickType_t portMAX_DELAY = UINT32_MAX;
inline constexpr int pdTRUE = 1;
struct FakeFsSemaphore { bool held = false; unsigned takes = 0, gives = 0, recursiveTakes = 0; };
inline FakeFsSemaphore gFakeFsSemaphore;
using SemaphoreHandle_t = FakeFsSemaphore*;
inline SemaphoreHandle_t gFsMutex = &gFakeFsSemaphore;
inline bool isHeldByCurrentTask(SemaphoreHandle_t mutex) { return mutex && mutex->held; }
inline bool isFsLockedByCurrentTask() { return isHeldByCurrentTask(gFsMutex); }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  if (mutex->held) { ++mutex->recursiveTakes; return 0; }  // a non-recursive mutex would deadlock
  mutex->held = true; ++mutex->takes; return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) {
  mutex->held = false; ++mutex->gives;
}
namespace fake_vfs {
struct LockDoubleWiring {
  LockDoubleWiring() { fs().lockHeld = [] { return gFakeFsSemaphore.held; }; }
};
inline LockDoubleWiring gLockDoubleWiring;
// reset() keeps lockHeld; call this after reset() if a suite replaced it.
inline void wireLockDouble() { fs().lockHeld = [] { return gFakeFsSemaphore.held; }; }
}  // namespace fake_vfs
#endif

#ifdef FAKE_VFS_PROVIDE_VFS_NAMESPACE
namespace VFS {

enum StorageType { INTERNAL = 0, SDCARD = 1, AUTO = 2 };

inline fake_vfs::Fs::Auth authOf(const AuthContext& ctx) {
  fake_vfs::Fs::Auth auth;
  auth.user = ctx.user.c_str(); auth.reason = ctx.path.c_str(); auth.scope = ctx.scope.c_str();
  auth.guarded = true;
  return auth;
}
inline fake_vfs::Fs::Auth unguarded() { return fake_vfs::Fs::Auth{}; }
inline fake_vfs::Backend backendOf(StorageType type) {
  return type == SDCARD ? fake_vfs::Backend::SD : fake_vfs::Backend::LittleFS;
}

inline bool isLittleFSReady() { return fake_vfs::fs().littlefsReady(); }
inline bool isSDAvailable() { return fake_vfs::fs().sdAvailable(); }
inline bool isSDWritable() { return fake_vfs::fs().sdAvailable() && fake_vfs::fs().capacity[1].writable; }
inline void noteSDWriteFailure(const char*) { ++fake_vfs::fs().sdWriteFailureNotes; }
inline void noteLittleFsBytesWritten(size_t bytes) { fake_vfs::fs().littlefsBytesNoted += bytes; }
inline void invalidateLittleFsFreeCache() { ++fake_vfs::fs().freeCacheInvalidations; }

inline String normalize(const String& path) { return String(fake_vfs::Fs::normalizePath(path.c_str()).c_str()); }
inline StorageType getStorageType(const String& path) {
  return fake_vfs::Fs::backendOf(fake_vfs::Fs::normalizePath(path.c_str())) == fake_vfs::Backend::SD ? SDCARD : INTERNAL;
}
inline String stripSdPrefix(const String& path) {
  return String(fake_vfs::Fs::stripSd(fake_vfs::Fs::normalizePath(path.c_str())).c_str());
}
inline bool isStagingName(const String& nameOrPath) {
  const std::string value = nameOrPath.c_str();
  auto ends = [&](const char* suffix) {
    const size_t n = std::strlen(suffix);
    return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
  };
  return ends(".tmp") || ends(".bak");
}

inline bool exists(const String& path) { return fake_vfs::fs().exists(path.c_str(), unguarded()); }
inline File open(const String& path, const char* mode = FILE_READ, bool create = false) {
  return fake_vfs::fs().open(path.c_str(), mode, create, unguarded());
}
inline bool mkdir(const String& path) { return fake_vfs::fs().mkdir(path.c_str(), unguarded()); }
inline bool remove(const String& path) { return fake_vfs::fs().remove(path.c_str(), unguarded()); }
inline bool rename(const String& from, const String& to) { return fake_vfs::fs().rename(from.c_str(), to.c_str(), unguarded()); }
inline bool rmdir(const String& path) { return fake_vfs::fs().rmdir(path.c_str(), unguarded()); }

inline bool getStats(StorageType type, uint64_t& total, uint64_t& used, uint64_t& free) {
  return fake_vfs::fs().getStats(backendOf(type), total, used, free);
}

inline File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create = false) {
  return fake_vfs::fs().open(path.c_str(), mode, create, authOf(ctx));
}
inline bool existsGuarded(const String& path, const AuthContext& ctx) { return fake_vfs::fs().exists(path.c_str(), authOf(ctx)); }
inline bool removeGuarded(const String& path, const AuthContext& ctx) { return fake_vfs::fs().remove(path.c_str(), authOf(ctx)); }
inline bool renameGuarded(const String& from, const String& to, const AuthContext& ctx) {
  return fake_vfs::fs().rename(from.c_str(), to.c_str(), authOf(ctx));
}
inline bool replaceFileGuarded(const String& temporary, const String& destination, const AuthContext& ctx) {
  return fake_vfs::fs().replaceFile(temporary.c_str(), destination.c_str(), authOf(ctx));
}
inline bool mkdirGuarded(const String& path, const AuthContext& ctx) { return fake_vfs::fs().mkdir(path.c_str(), authOf(ctx)); }
inline bool rmdirGuarded(const String& path, const AuthContext& ctx) { return fake_vfs::fs().rmdir(path.c_str(), authOf(ctx)); }

inline AuthContext systemAuth(const char* reason) {
  AuthContext ctx{};
#ifdef FAKE_VFS_EXTERNAL_AUTH_CONTEXT
  ctx.transport = SOURCE_INTERNAL;  // the real struct; the harness extracted CommandSource with it
#endif
  ctx.user = "system";
  ctx.path = String((std::string("system:") + (reason ? reason : "")).c_str());
  ctx.ip = "local";
  ctx.sid = "";
  ctx.opaque = nullptr;
  return ctx;
}
inline AuthContext systemAuth(const char* scope, const char* reason) {
  AuthContext ctx = systemAuth(reason);
  ctx.scope = scope ? scope : "";
  return ctx;
}

}  // namespace VFS
#endif  // FAKE_VFS_PROVIDE_VFS_NAMESPACE
