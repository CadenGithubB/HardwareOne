#include <Arduino.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>
#define ENABLE_QUICKNOTE 1
#define pdMS_TO_TICKS(ms) (ms)
void webBatchTestStringAppend() {}
using TransportSessionEpoch = uint32_t;
enum CommandSource { SOURCE_INTERNAL, SOURCE_SERIAL, SOURCE_LOCAL_DISPLAY, SOURCE_G2_GLASSES, SOURCE_WEB };
struct AuthContext {
  CommandSource transport = SOURCE_INTERNAL;
  String path, ip, user, sid;
  void* opaque = nullptr;
  String scope;
};
// INSERT_POLICIES
// INSERT_NORMALIZER

// --- Session, roster, clock, allocator --------------------------------------
static unsigned locks = 0, lockTakes = 0;
static bool lockAvailable = true;
struct FsLockGuard {
  bool held;
  FsLockGuard(const char* owner, unsigned ms) : held(lockAvailable && !locks) {
    assert(owner && !std::strcmp(owner, "qnote.store") && ms == 1000);
    if (held) { ++locks; ++lockTakes; }
  }
  ~FsLockGuard() { if (held) --locks; }
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};
static bool isFsLockedByCurrentTask() { return locks != 0; }

static bool live = true, revokeOnLookup = false;
static const uint32_t kEpoch = 7;
static bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  return live && source == SOURCE_G2_GLASSES && epoch == kEpoch;
}
struct Account { uint32_t id; const char* role; };
static std::map<std::string, Account> roster;
static unsigned idLookups = 0;
static bool getUserIdByUsername(const String& user, uint32_t& id) {
  assert(locks);
  ++idLookups;
  if (revokeOnLookup) live = false;
  const auto found = roster.find(user.c_str());
  id = found == roster.end() ? 0 : found->second.id;
  return id != 0;
}
namespace Clock {
static bool synced = true;
static time_t epochNow = 1791127812;  // 2026-10-04T15:30:12Z
static time_t epochSeconds() { return epochNow; }
static int64_t epochMillis() { return int64_t(epochNow) * 1000 + 250; }
static bool isSynced() { return synced; }
static bool isPlausibleEpoch(time_t t) { return t >= 1577836800 && t < 4102444800LL; }
}
// esp_timer: 5.4 s since boot; this is boot #76.
static int64_t uptimeUs = 5400000;
static int64_t esp_timer_get_time() { return uptimeUs; }
uint32_t gBootCounter = 76;
enum class AllocPolicy { PreferPSRAM };
static size_t lastAlloc = 0;
static unsigned allocs = 0, frees = 0;
static bool allocFails = false;
static void* ps_alloc(size_t size, AllocPolicy, const char* tag) {
  assert(tag && !std::strcmp(tag, "qnote.list") && locks);
  lastAlloc = size;
  if (allocFails) return nullptr;
  ++allocs;
  return std::malloc(size);
}
static void ps_free(void* data) { ++frees; std::free(data); }

// --- Guarded VFS ------------------------------------------------------------
// Permission = the production transcript owner mask plus the role tiers
// System_Filesystem.cpp layers on it: system/superadmin unrestricted, guest
// read-only, an AuthContext scope confines. The real pipeline is covered by
// test_transcript_path_permissions.py, including the notes folder.
enum : uint8_t { PERM_READ = 1, PERM_WRITE = 2, PERM_DELETE = 4, PERM_RENAME = 8, PERM_CREATE = 16, PERM_ALL = 0x3f };
static uint8_t perms(const std::string& path, const AuthContext& ctx) {
  const std::string scope = ctx.scope.c_str();
  if (!scope.empty() && path != scope && path.rfind(scope + "/", 0) != 0) return 0;
  if (ctx.transport == SOURCE_INTERNAL && ctx.user == "system") return PERM_ALL;
  const auto found = roster.find(ctx.user.c_str());
  if (found == roster.end()) return 0;
  const bool unrestricted = !std::strcmp(found->second.role, "superadmin");
  uint8_t mask = TranscriptPathPolicy::permissionMask(
      TranscriptPathPolicy::classify(path.c_str()), found->second.id, unrestricted);
  if (!std::strcmp(found->second.role, "guest")) mask &= PERM_READ;
  return mask;
}
struct Node { bool dir = false; std::string data; };
static std::map<std::string, Node> tree;
static unsigned filesOpen = 0, writes = 0, shortWrite = 0, reads = 0;
static bool writeError = false, sizeSkew = false, shortRead = false;
static int dirOrder = 0;  // 0 ascending, 1 descending, 2 interleaved
static std::string parentOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == 0 ? "/" : path.substr(0, slash);
}
static bool dirExists(const std::string& path) {
  if (path == "/" || path == "/sd") return true;
  const auto found = tree.find(path);
  return found != tree.end() && found->second.dir;
}
class File {
  std::string path_;
  bool valid_ = false, open_ = false, write_ = false;
  size_t pos_ = 0, next_ = 0;
  std::vector<std::string> children_;
 public:
  File() = default;
  File(const std::string& path, bool write) : path_(path), valid_(true), open_(true), write_(write) {
    ++filesOpen;
    if (!tree.at(path_).dir) return;
    for (const auto& entry : tree)
      if (entry.first != path_ && parentOf(entry.first) == path_) children_.push_back(entry.first);
    if (dirOrder == 1) std::reverse(children_.begin(), children_.end());
    if (dirOrder == 2) {
      std::vector<std::string> mixed;
      for (size_t a = 0, b = children_.size(); a < b; ++a) {
        mixed.push_back(children_[a]);
        if (a != --b) mixed.push_back(children_[b]);
      }
      children_ = mixed;
    }
  }
  File(File&& other) noexcept { *this = std::move(other); }
  File& operator=(File&& other) noexcept {
    if (this == &other) return *this;
    close();
    path_ = other.path_; valid_ = other.valid_; open_ = other.open_; write_ = other.write_;
    pos_ = other.pos_; next_ = other.next_; children_ = other.children_;
    other.valid_ = other.open_ = false;
    return *this;
  }
  File(const File&) = delete;
  ~File() { close(); }
  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return valid_ && tree.at(path_).dir; }
  const char* name() const { return path_.c_str(); }  // full path: the store takes the base name
  size_t size() const { return tree.at(path_).data.size() + (sizeSkew ? 1 : 0); }
  size_t write(const uint8_t* bytes, size_t n) {
    assert(locks && open_ && write_);
    ++writes;
    const size_t used = shortWrite == writes ? n / 2 : n;
    tree.at(path_).data.append(reinterpret_cast<const char*>(bytes), used);
    return used;
  }
  void flush() { assert(locks && open_); }
  int getWriteError() const { return writeError ? 1 : 0; }
  bool seek(size_t offset) {
    assert(locks && open_);
    if (offset > size()) return false;
    pos_ = offset;
    return true;
  }
  size_t read(uint8_t* out, size_t n) {
    assert(locks && open_ && !write_);
    ++reads;
    const std::string& data = tree.at(path_).data;
    size_t got = std::min(n, data.size() - pos_);
    if (shortRead && got) --got;
    std::memcpy(out, data.data() + pos_, got);
    pos_ += got;
    return got;
  }
  File openNextFile() {
    assert(locks && open_);
    return next_ < children_.size() ? File(children_[next_++], false) : File();
  }
  void close() {
    if (!open_) return;
    assert(locks);
    open_ = false;
    --filesOpen;
  }
};
namespace VFS {
enum StorageType { INTERNAL, SDCARD };
static bool sdWritable = false, sdPresent = true, statsOk = true, failRename = false, denyMkdir = false, denyOpen = false;
static bool failRemove = false;
static uint64_t freeBytes = 1 << 20;
static StorageType statsTier = INTERNAL;
static unsigned sdFailures = 0, notedBytes = 0, denials = 0, opens = 0, systemAuths = 0, guardedCalls = 0;
static size_t skewAfterRename = 0;
static std::vector<std::pair<std::string, std::string>> renames;
static std::vector<std::string> contexts, opened;
static void note(const AuthContext& ctx) {
  assert(locks);
  ++guardedCalls;
  contexts.push_back(std::string(ctx.user.c_str()) + "|" + ctx.scope.c_str());
}
static bool allowed(const String& path, const AuthContext& ctx, uint8_t need) {
  note(ctx);
  if (perms(path.c_str(), ctx) & need) return true;
  ++denials;
  return false;
}
static bool isSDWritable() { assert(locks); return sdWritable && sdPresent; }
static bool isSDAvailable() { assert(locks); return sdPresent; }
static bool exists(const String& path) { assert(locks); return tree.count(path.c_str()) != 0; }
static bool existsGuarded(const String& path, const AuthContext& ctx) {
  return allowed(path, ctx, PERM_READ) && exists(path);
}
static bool mkdirGuarded(const String& path, const AuthContext& ctx) {
  if (!allowed(path, ctx, PERM_CREATE) || denyMkdir || !dirExists(parentOf(path.c_str())) ||
      tree.count(path.c_str())) return false;
  tree[path.c_str()] = Node{true, ""};
  return true;
}
static bool removeGuarded(const String& path, const AuthContext& ctx) {
  if (!allowed(path, ctx, PERM_DELETE) || failRemove) return false;
  const auto found = tree.find(path.c_str());
  if (found == tree.end() || found->second.dir) return false;
  tree.erase(found);
  return true;
}
static bool renameGuarded(const String& from, const String& to, const AuthContext& ctx) {
  if (!allowed(from, ctx, PERM_RENAME) || !allowed(to, ctx, PERM_CREATE)) return false;
  renames.emplace_back(from.c_str(), to.c_str());
  const auto found = tree.find(from.c_str());
  if (failRename || found == tree.end()) return false;
  Node node = found->second;
  tree.erase(found);
  node.data.append(skewAfterRename, '!');
  tree[to.c_str()] = node;
  return true;
}
static File openGuarded(const String& p, const char* mode, const AuthContext& ctx, bool create = false) {
  const std::string path = p.c_str();
  const bool reading = mode[0] == 'r';
  ++opens;
  if (!allowed(p, ctx, reading ? PERM_READ : tree.count(path) ? PERM_WRITE : PERM_CREATE)) return File();
  if (reading) {
    if (!tree.count(path)) return File();
    opened.push_back(path);
    return File(path, false);
  }
  assert(create && std::string(mode) == "w");
  if (denyOpen || !dirExists(parentOf(path)) || (tree.count(path) && tree[path].dir)) return File();
  tree[path] = Node{};
  return File(path, true);
}
static AuthContext systemAuth(const char* scope, const char* reason) {
  assert(!std::strcmp(reason, "note root"));
  ++systemAuths;
  AuthContext ctx;
  ctx.transport = SOURCE_INTERNAL;
  ctx.user = "system";
  ctx.scope = scope;
  return ctx;
}
static bool getStats(StorageType type, uint64_t& total, uint64_t& used, uint64_t& free) {
  assert(locks);
  statsTier = type;
  total = 8u << 20;
  used = 0;
  free = freeBytes;
  return statsOk && (type != SDCARD || sdPresent);
}
static void noteSDWriteFailure(const char*) { ++sdFailures; }
static void noteLittleFsBytesWritten(size_t n) { notedBytes += static_cast<unsigned>(n); }
}  // namespace VFS
static bool canRead(const String& path, const AuthContext& ctx) {
  assert(locks);
  return perms(path.c_str(), ctx) & PERM_READ;
}

// INSERT_STORE

// --- Tests --------------------------------------------------------------------
static AuthContext actor(const char* user) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.user = user;
  return ctx;
}
static void reset() {
  assert(!locks && !filesOpen);
  roster = {{"alice", {7, "user"}}, {"bob", {8, "user"}}, {"admin", {4, "admin"}},
            {"guest", {5, "guest"}}, {"super", {1, "superadmin"}}};
  live = true; revokeOnLookup = false; lockAvailable = true; Clock::synced = true;
  lockTakes = idLookups = writes = shortWrite = reads = 0;
  writeError = sizeSkew = shortRead = false; dirOrder = 0;
  lastAlloc = 0; allocFails = false;
  assert(allocs == frees);
  tree.clear();
  VFS::sdWritable = VFS::failRename = VFS::denyMkdir = VFS::denyOpen = VFS::failRemove = false;
  Clock::epochNow = 1791127812;
  uptimeUs = 5400000;
  VFS::sdPresent = VFS::statsOk = true;
  VFS::freeBytes = 1 << 20;
  VFS::sdFailures = VFS::notedBytes = VFS::denials = VFS::opens = VFS::systemAuths = VFS::guardedCalls = 0;
  VFS::skewAfterRename = 0;
  VFS::renames.clear(); VFS::contexts.clear(); VFS::opened.clear();
}
static NoteHeader meta(uint64_t id) {
  NoteHeader header;
  header.id = id;
  header.trigger = Trigger::Hold;
  header.mic = Mic::G2;
  header.durationMs = 4200;
  return header;
}
static const char* kText = "Buy oat milk and call the dentist about Thursday.";
static bool save(uint64_t id, std::string* path = nullptr, const char** why = nullptr,
                 const char* user = "alice", const char* text = kText) {
  NoteHeader header = meta(id);
  char out[QuickNotePolicy::kPathMax];
  const char* reason = nullptr;
  const bool ok = QuickNoteStore::create(SOURCE_G2_GLASSES, kEpoch, user, header, text,
                                         std::strlen(text), out, sizeof(out), &reason);
  assert(!locks && !filesOpen);
  assert(ok == (reason == nullptr));
  if (path) *path = out;
  if (why) *why = reason;
  return ok;
}
static size_t countFiles(const char* suffix) {
  size_t n = 0;
  for (const auto& entry : tree) {
    const std::string& p = entry.first;
    if (!entry.second.dir && p.size() >= std::strlen(suffix) &&
        p.compare(p.size() - std::strlen(suffix), std::string::npos, suffix) == 0) ++n;
  }
  return n;
}

static void test_create() {
  reset();
  std::string path;
  assert(save(0x6f1c9a2e00000007ull, &path));
  assert(path == "/stt/u7/notes/20261004T153012Z-6f1c9a2e00000007.txt");
  assert(dirExists("/stt") && dirExists("/stt/u7") && dirExists("/stt/u7/notes"));
  // Only the protected root uses the trusted context; everything else is the
  // named owner, confined to her own folder.
  assert(VFS::systemAuths == 1);
  for (size_t i = 0; i < VFS::contexts.size(); ++i)
    assert(VFS::contexts[i] == (i == 0 ? "system|/stt" : "alice|/stt/u7"));
  // tmp then rename, then nothing but the note.
  assert(VFS::renames.size() == 1 &&
         VFS::renames[0].first == "/stt/u7/notes/20261004T153012Z-6f1c9a2e00000007.tmp" &&
         VFS::renames[0].second == path);
  assert(countFiles(".tmp") == 0 && countFiles(".txt") == 1);
  const std::string& data = tree.at(path).data;
  NoteHeader back;
  size_t body = 0;
  assert(noteParseHeader(data.data(), data.size(), &back, &body));
  assert(back.id == 0x6f1c9a2e00000007ull && !std::strcmp(back.created, "20261004T153012Z") &&
         back.boot == 76 && back.uptimeS == 5 &&  // the boot and its uptime, every note
         back.trigger == Trigger::Hold && back.mic == Mic::G2 && back.durationMs == 4200 &&
         back.words == 9 && !std::strcmp(back.title, "Buy oat milk and call the dentist about"));
  assert(data.substr(body) == kText);
  assert(VFS::notedBytes == data.size() && VFS::statsTier == VFS::INTERNAL);
  assert(idLookups == 1 && lockTakes == 1 && !allocs);

  // A second note reuses the folders; the same id cannot overwrite the first.
  const std::string first = data;
  assert(save(0x6f1c9a2e00000008ull) && countFiles(".txt") == 2);
  const char* why = nullptr;
  assert(!save(0x6f1c9a2e00000007ull, nullptr, &why, "alice", "replacement") &&
         !std::strcmp(why, "Note filename already exists"));
  assert(tree.at(path).data == first && countFiles(".tmp") == 0);

  // Card tier when it is writable; boot stamp while the clock is unsynced.
  reset();
  VFS::sdWritable = true;
  Clock::synced = false;
  assert(save(0xabc, &path));
  assert(path == "/sd/stt/u7/notes/boot-0000000000000abc.txt" && VFS::statsTier == VFS::SDCARD);
  assert(VFS::contexts[0] == "system|/sd/stt" && VFS::notedBytes == 0);
  {
    const std::string& boot = tree.at(path).data;
    assert(noteParseHeader(boot.data(), boot.size(), &back, nullptr) && !std::strcmp(back.created, "boot") &&
           back.boot == 76 && back.uptimeS == 5);
  }

  // A caller-supplied stamp is kept; a leftover tmp from a crash is replaced.
  reset();
  tree["/stt"] = Node{true, ""}; tree["/stt/u7"] = Node{true, ""}; tree["/stt/u7/notes"] = Node{true, ""};
  tree["/stt/u7/notes/20250101T000000Z-0000000000000001.tmp"] = Node{false, "stale partial"};
  NoteHeader header = meta(1);
  std::strcpy(header.created, "20250101T000000Z");
  char out[QuickNotePolicy::kPathMax];
  assert(QuickNoteStore::create(SOURCE_G2_GLASSES, kEpoch, "alice", header, "hi there", 8,
                                out, sizeof(out), nullptr));
  assert(!std::strcmp(out, "/stt/u7/notes/20250101T000000Z-0000000000000001.txt") &&
         countFiles(".tmp") == 0 && header.words == 2 && !std::strcmp(header.title, "hi there"));
  assert(header.boot == 0 && header.uptimeS == 0);  // stamped by the caller: left as given
  assert(VFS::systemAuths == 0);  // the root already existed
}

static void test_create_refusals() {
  // Invalid requests never touch storage.
  const std::string big(QuickNotePolicy::kMaxNoteBytes + 1, 'a');
  const std::string max(QuickNotePolicy::kMaxNoteBytes, 'a');
  const char nul[] = {'a', '\0', 'b'};
  struct Case { const char* user; uint32_t epoch; uint64_t id; const char* text; size_t length; };
  const Case cases[] = {
      {"", kEpoch, 1, "x", 1}, {nullptr, kEpoch, 1, "x", 1}, {"alice", 0, 1, "x", 1},
      {"alice", kEpoch, 0, "x", 1}, {"alice", kEpoch, 1, nullptr, 1}, {"alice", kEpoch, 1, "x", 0},
      {"alice", kEpoch, 1, big.c_str(), big.size()}, {"alice", kEpoch, 1, nul, sizeof(nul)},
  };
  for (const Case& c : cases) {
    reset();
    NoteHeader header = meta(c.id);
    const char* why = nullptr;
    char out[8] = "junk";
    assert(!QuickNoteStore::create(SOURCE_G2_GLASSES, c.epoch, c.user, header, c.text, c.length,
                                   out, sizeof(out), &why));
    assert(!std::strcmp(why, "Invalid note") && !out[0] && !lockTakes && tree.empty());
  }
  reset();
  assert(save(2, nullptr, nullptr, "alice", max.c_str()));  // exactly the cap is fine

  struct Fault { const char* name; const char* why; };
  const Fault faults[] = {
      {"busy", "Note storage busy"},
      {"expired", "Note session expired before saving"},
      {"revoked during lookup", "Note session expired before saving"},
      {"unknown account", "Note account unavailable"},
      {"below reserve", "Note storage full or unavailable"},
      {"stats", "Note storage full or unavailable"},
      {"mkdir", "Cannot create notes folder"},
      {"open", "Cannot open note file"},
      {"short header", "Note write incomplete"},
      {"short text", "Note write incomplete"},
      {"write error", "Note write incomplete"},
      {"size", "Note write incomplete"},
      {"rename", "Cannot save note"},
      {"verify", "Note verification failed"},
  };
  for (size_t f = 0; f < sizeof(faults) / sizeof(faults[0]); ++f) {
    reset();
    const std::string name = faults[f].name;
    const char* user = "alice";
    if (name == "busy") lockAvailable = false;
    if (name == "expired") live = false;
    if (name == "revoked during lookup") revokeOnLookup = true;
    if (name == "unknown account") user = "mallory";
    if (name == "below reserve") {
      // Exactly the reserve plus this note plus the 256-byte margin passes; one less fails.
      NoteHeader probe = meta(3);
      std::strcpy(probe.created, "20261004T153012Z");
      probe.boot = 76;      // as create stamps them
      probe.uptimeS = 5;
      noteTitle(kText, std::strlen(kText), probe.title, sizeof(probe.title));
      probe.words = noteWordCount(kText, std::strlen(kText));
      char head[QuickNotePolicy::kHeaderMax];
      const size_t bytes = noteFormatHeader(probe, head, sizeof(head)) + std::strlen(kText);
      VFS::freeBytes = QuickNoteStore::kReserveBytes + bytes + 256;
      assert(save(3) && countFiles(".txt") == 1);
      reset();
      VFS::freeBytes = QuickNoteStore::kReserveBytes + bytes + 255;
    }
    if (name == "stats") VFS::statsOk = false;
    if (name == "mkdir") VFS::denyMkdir = true;
    if (name == "open") VFS::denyOpen = true;
    if (name == "short header") shortWrite = 1;
    if (name == "short text") { shortWrite = 2; VFS::sdWritable = true; }
    if (name == "write error") writeError = true;
    if (name == "size") sizeSkew = true;
    if (name == "rename") VFS::failRename = true;
    if (name == "verify") VFS::skewAfterRename = 3;
    const char* why = nullptr;
    std::string path = "unchanged";
    assert(!save(3, &path, &why, user));
    if (std::strcmp(why, faults[f].why)) {
      std::fprintf(stderr, "fault %s: %s\n", faults[f].name, why);
      assert(false);
    }
    assert(path.empty());
    // Fails cleanly: no note and no partial file anywhere.
    assert(countFiles(".txt") == 0 && countFiles(".tmp") == 0);
    if (name == "short text") assert(VFS::sdFailures == 1);
  }
  // A partial write on internal flash still charges the written bytes.
  reset();
  shortWrite = 2;
  assert(!save(4) && VFS::notedBytes > 0 && VFS::sdFailures == 0);
}

static std::string noteName(int day, uint64_t id) {
  char stamp[32], name[64];
  std::snprintf(stamp, sizeof(stamp), "2026%04dT120000Z", day);
  assert(QuickNotePolicy::noteFileName(stamp, id, false, name, sizeof(name)));
  return name;
}
static void putNote(const std::string& folder, int day, uint64_t id, const char* text) {
  NoteHeader header = meta(id);
  std::snprintf(header.created, sizeof(header.created), "2026%04dT120000Z", day);
  header.words = noteWordCount(text, std::strlen(text));
  noteTitle(text, std::strlen(text), header.title, sizeof(header.title));
  char head[QuickNotePolicy::kHeaderMax];
  const size_t n = noteFormatHeader(header, head, sizeof(head));
  assert(n);
  tree[folder + "/" + noteName(day, id)] = Node{false, std::string(head, n) + text};
}
static void makeFolder(const std::string& folder) {
  for (size_t at = folder.find('/', 1); ; at = folder.find('/', at + 1)) {
    const std::string part = at == std::string::npos ? folder : folder.substr(0, at);
    if (part != "/sd") tree[part] = Node{true, ""};
    if (at == std::string::npos) break;
  }
}
static QuickNoteStore::Page page;
static bool listing(const char* user, uint32_t owner, bool sd, uint32_t offset, const char** why = nullptr) {
  const char* reason = nullptr;
  reads = 0;
  VFS::opened.clear();
  const bool ok = QuickNoteStore::list(actor(user), owner, sd, offset, &page, &reason);
  assert(!locks && !filesOpen && allocs == frees);
  assert(ok == (reason == nullptr));
  if (why) *why = reason;
  return ok;
}

static void test_list() {
  reset();
  const std::string folder = "/stt/u7/notes";
  makeFolder(folder);
  for (int i = 0; i < 11; ++i) putNote(folder, 1001 + i, 100 + i, "note words here");
  // Noise a listing must skip: in-progress, other spellings, a folder,
  // a transcript and a note-shaped name in the wrong place.
  tree[folder + "/20261231T120000Z-00000000000000aa.tmp"] = Node{false, "partial"};
  tree[folder + "/20261231T120000Z-00000000000000AA.txt"] = Node{false, "x"};
  tree[folder + "/readme.txt"] = Node{false, "x"};
  tree[folder + "/20261231T120000Z-00000000000000bb.txt"] = Node{true, ""};
  tree["/stt/u7/20261231T120000Z-00000000000000cc.txt"] = Node{false, "transcript tree"};
  // A note whose header is damaged, and one whose header names another id.
  tree[folder + "/" + noteName(1100, 900)] = Node{false, "HardwareOne note v1\ngarbage"};
  putNote(folder, 1101, 901, "mismatch");
  {
    std::string& data = tree.at(folder + "/" + noteName(1101, 901)).data;
    data.replace(data.find("Id: 0000000000000385"), 20, "Id: 0000000000000999");
  }
  for (int order = 0; order < 3; ++order) {
    dirOrder = order;
    assert(listing("alice", 0, false, 0));
    assert(page.count == 8 && page.total == 13 && page.more && page.nextOffset == 8 && !page.capped);
    assert(lastAlloc == 8 * (40 + 2));
    // Newest first: the two damaged ones are newest, then 1011 down.
    assert(page.rows[0].path == folder + "/" + noteName(1101, 901) && !page.rows[0].headerValid &&
           page.rows[0].header.id == 901 && !std::strcmp(page.rows[0].header.created, "20261101T120000Z") &&
           !page.rows[0].header.title[0]);
    assert(page.rows[1].header.id == 900 && !page.rows[1].headerValid);
    for (size_t r = 2; r < 8; ++r) {
      const auto& row = page.rows[r];
      assert(row.headerValid && row.header.id == 100 + 11 - (r - 1) && row.header.words == 3 &&
             !std::strcmp(row.header.title, "note words here") && row.header.trigger == Trigger::Hold);
      assert(row.path == folder + "/" + noteName(1001 + 11 - int(r - 1), row.header.id));
    }
    // Headers are read for the visible rows only.
    assert(VFS::opened.size() == 1 + 8 && reads == 8);
    assert(listing("alice", 7, false, 8));
    assert(page.count == 5 && !page.more && page.nextOffset == 13 && page.total == 13);
    assert(page.rows[4].header.id == 100 && page.rows[4].headerValid);
    assert(listing("alice", 7, false, 13) && page.count == 0 && !page.more && page.nextOffset == 13);
  }
  // Same folder on the card tier is a different folder; none there yet.
  assert(listing("alice", 0, true, 0) && page.count == 0 && page.total == 0 && !page.more);
  VFS::sdPresent = false;
  const char* why = nullptr;
  assert(!listing("alice", 0, true, 0, &why) && !std::strcmp(why, "No SD card"));
  VFS::sdPresent = true;

  // Owner / other / admin / guest / superadmin.
  assert(!listing("bob", 7, false, 0, &why) && !std::strcmp(why, "Notes unavailable"));
  assert(!listing("admin", 7, false, 0, &why) && !std::strcmp(why, "Notes unavailable"));
  assert(!listing("guest", 7, false, 0, &why) && !std::strcmp(why, "Notes unavailable"));
  assert(listing("super", 7, false, 0) && page.count == 8);
  assert(listing("bob", 0, false, 0) && page.count == 0);  // his own, empty
  makeFolder("/stt/u5/notes");
  putNote("/stt/u5/notes", 1001, 1, "guest note");
  assert(listing("guest", 0, false, 0) && page.count == 1 && page.rows[0].headerValid);
  assert(!listing("mallory", 0, false, 0, &why) && !std::strcmp(why, "Note account unavailable"));
  lockAvailable = false;
  assert(!listing("alice", 0, false, 0, &why) && !std::strcmp(why, "Note storage busy"));
  lockAvailable = true;
  allocFails = true;
  assert(!listing("alice", 0, false, 0, &why) && !std::strcmp(why, "Out of memory"));
  allocFails = false;
  assert(!page.count);

  // Over kMaxNames: the newest 500 stay listable whatever the directory order.
  for (int order = 0; order < 3; ++order) {
    reset();
    dirOrder = order;
    makeFolder(folder);
    for (int i = 0; i < 520; ++i) putNote(folder, 101 + i, 1000 + i, "x");
    assert(listing("alice", 0, false, 0) && page.count == 8 && page.total == 520 && page.capped && page.more);
    assert(page.rows[0].header.id == 1519 && page.rows[7].header.id == 1512);
    assert(listing("alice", 0, false, 496) && page.count == 4 && !page.more && page.nextOffset == 500);
    assert(page.rows[3].header.id == 1020 && lastAlloc == 500 * 42);
    assert(listing("alice", 0, false, 500) && page.count == 0 && !page.more && page.capped);
    assert(listing("alice", 0, false, 4000000000u) && page.count == 0 && !page.more);
  }
}

static void test_boot_order() {
  // With the clock unset notes are boot-<id>.txt, ids from noteNewId: they
  // list newest first, this boot's above an earlier boot's, whatever the
  // random bits, and above every note with a time (a list orders names).
  reset();
  const std::string folder = "/stt/u7/notes";
  makeFolder(folder);
  putNote(folder, 1001, 0xfeed, "timed note");
  Clock::synced = false;
  const uint64_t ids[] = {QuickNotePolicy::noteNewId(false, 75, 90000, 0xffff),
                          QuickNotePolicy::noteNewId(false, 76, 5, 0xffff),
                          QuickNotePolicy::noteNewId(false, 76, 59, 0x0001),
                          QuickNotePolicy::noteNewId(false, 76, 600, 0x8000)};
  for (const uint64_t id : ids) assert(save(id));
  for (int order = 0; order < 3; ++order) {
    dirOrder = order;
    assert(listing("alice", 0, false, 0) && page.count == 5 && page.total == 5);
    for (size_t r = 0; r < 4; ++r)
      assert(page.rows[r].headerValid && page.rows[r].header.id == ids[3 - r] &&
             !std::strcmp(page.rows[r].header.created, "boot"));
    assert(page.rows[4].header.id == 0xfeed);
  }
}

static std::string bigNote() {
  // Header + 600 bytes whose 512-byte window boundary lands inside "é".
  return std::string(511, 'a') + "\xc3\xa9" + std::string(87, 'b');
}
static bool reading(const char* user, const char* path, uint32_t offset, std::string* text,
                    uint32_t* total = nullptr, const char** why = nullptr, size_t cap = 513) {
  char out[600];
  std::memset(out, 'x', sizeof(out));
  size_t got = 99;
  uint32_t size = 99;
  const char* reason = nullptr;
  VFS::opened.clear();
  const bool ok = QuickNoteStore::read(actor(user), path, offset, out, cap, &got, &size, &reason);
  assert(!locks && !filesOpen && ok == (reason == nullptr));
  if (!ok) assert(got == 0 && size == 0 && (cap < 513 || !out[0]));
  if (ok) assert(std::strlen(out) == got);
  if (text) *text = ok ? std::string(out, got) : "";
  if (total) *total = size;
  if (why) *why = reason;
  return ok;
}

static void test_read_and_remove() {
  for (bool sd : {false, true}) {
    reset();
    const std::string folder = sd ? "/sd/stt/u7/notes" : "/stt/u7/notes";
    makeFolder(folder);
    const std::string path = folder + "/" + noteName(1001, 42);
    tree[path] = Node{false, bigNote()};
    std::string text;
    uint32_t total = 0;
    assert(reading("alice", path.c_str(), 0, &text, &total) && text == std::string(511, 'a') &&
           total == 600);
    assert(reading("alice", path.c_str(), 511, &text) && text == "\xc3\xa9" + std::string(87, 'b'));
    assert(reading("alice", path.c_str(), 600, &text) && text.empty());
    const char* why = nullptr;
    assert(!reading("alice", path.c_str(), 601, nullptr, nullptr, &why) && !std::strcmp(why, "Invalid offset"));
    // The guarded VFS sees the normalized spelling.
    const std::string messy = " /" + path.substr(0, folder.size()) + "/./" + noteName(1001, 42) + " ";
    assert(reading("alice", messy.c_str(), 0, &text) && VFS::opened[0] == path);
    // Owner / other / admin / guest / superadmin.
    assert(!reading("bob", path.c_str(), 0, nullptr, nullptr, &why) && !std::strcmp(why, "Note unavailable"));
    assert(!reading("admin", path.c_str(), 0, nullptr, nullptr, &why) && !std::strcmp(why, "Note unavailable"));
    assert(!reading("guest", path.c_str(), 0, nullptr, nullptr, &why));
    assert(reading("super", path.c_str(), 0, &text) && text.size() == 511);
    shortRead = true;
    assert(!reading("alice", path.c_str(), 0, nullptr, nullptr, &why) && !std::strcmp(why, "Note read incomplete"));
    shortRead = false;
    assert(!reading("alice", path.c_str(), 0, nullptr, nullptr, &why, 512) && !std::strcmp(why, "Invalid request"));
    lockAvailable = false;
    assert(!reading("alice", path.c_str(), 0, nullptr, nullptr, &why) && !std::strcmp(why, "Note storage busy"));
    lockAvailable = true;

    // Delete: only the owner (or an unrestricted role); the file stays otherwise.
    const char* reason = nullptr;
    for (const char* who : {"bob", "admin"}) {
      assert(!QuickNoteStore::remove(actor(who), path.c_str(), &reason) &&
             !std::strcmp(reason, "Note unavailable") && tree.count(path));
    }
    const std::string guestPath = (sd ? "/sd/stt/u5/notes/" : "/stt/u5/notes/") + noteName(1001, 5);
    makeFolder(parentOf(guestPath));
    tree[guestPath] = Node{false, "guest note"};
    assert(reading("guest", guestPath.c_str(), 0, &text) && text == "guest note");
    assert(!QuickNoteStore::remove(actor("guest"), guestPath.c_str(), &reason) &&
           !std::strcmp(reason, "Cannot delete note") && tree.count(guestPath));
    assert(QuickNoteStore::remove(actor("alice"), path.c_str(), &reason) && !reason && !tree.count(path));
    assert(!QuickNoteStore::remove(actor("alice"), path.c_str(), &reason) && !std::strcmp(reason, "Note unavailable"));
    tree[path] = Node{false, "again"};
    assert(QuickNoteStore::remove(actor("super"), path.c_str(), &reason) && !tree.count(path));
    assert(!locks && !filesOpen);
  }

  // Anything that is not a canonical note path is refused before any access:
  // transcripts beside the notes, in-progress files, aliases and traversal.
  reset();
  makeFolder("/stt/u7/notes");
  const std::string name = noteName(1001, 42);
  tree["/stt/u7/notes/" + name] = Node{false, "note"};
  tree["/stt/u7/" + name] = Node{false, "transcript"};
  tree["/stt/u7/notes/20261001T120000Z-000000000000002a.tmp"] = Node{false, "partial"};
  const std::string bad[] = {
      "/stt/u7/" + name, "/stt/u7/notes/20261001T120000Z-000000000000002a.tmp",
      "/stt/u7/notes/../" + name, "/stt/u7/notes\\" + name, "/sd/stt/u7/notes\\" + name,
      "/sd/STT/u7/notes/" + name, "/sd/stt./u7/notes/" + name, "/sd/stt/u07/notes/" + name,
      "/stt/u7/Notes/" + name, "/stt/u7/notes/" + name + "\"", "/stt/u7/notes/BOOT~1.TXT",
      "/stt2/u7/notes/" + name, "/system/users.json", "", "/",
  };
  for (const std::string& path : bad) {
    const char* why = nullptr;
    VFS::guardedCalls = 0;
    assert(!reading("super", path.c_str(), 0, nullptr, nullptr, &why) && !std::strcmp(why, "Not a note"));
    assert(!QuickNoteStore::remove(actor("super"), path.c_str(), &why) && !std::strcmp(why, "Not a note"));
    assert(!VFS::guardedCalls && !lockTakes);
  }
  const char* why = nullptr;
  assert(!reading("alice", nullptr, 0, nullptr, nullptr, &why) && !std::strcmp(why, "Not a note"));
  assert(!QuickNoteStore::remove(actor("alice"), nullptr, &why));
  assert(tree.count("/stt/u7/" + name) && tree.count("/stt/u7/notes/" + name));
}

static void test_room_and_stamp() {
  reset();
  const uint64_t need = QuickNoteStore::kReserveBytes + QuickNotePolicy::kHeaderMax +
                        QuickNotePolicy::kMaxNoteBytes + 256;
  const char* why = nullptr;
  VFS::freeBytes = need;
  assert(QuickNoteStore::hasRoom(&why) && !why && VFS::statsTier == VFS::INTERNAL);
  VFS::freeBytes = need - 1;
  assert(!QuickNoteStore::hasRoom(&why) && !std::strcmp(why, "Note storage full or unavailable"));
  VFS::freeBytes = need;
  VFS::sdWritable = true;
  assert(QuickNoteStore::hasRoom(nullptr) && VFS::statsTier == VFS::SDCARD);
  VFS::statsOk = false;
  assert(!QuickNoteStore::hasRoom(&why));
  VFS::statsOk = true;
  lockAvailable = false;
  assert(!QuickNoteStore::hasRoom(&why) && !std::strcmp(why, "Note storage busy"));
  lockAvailable = true;
  assert(!locks && tree.empty());
  char stamp[QuickNotePolicy::kStampMax + 1];
  QuickNoteStore::stamp(stamp, sizeof(stamp));
  assert(!std::strcmp(stamp, "20261004T153012Z"));
  Clock::synced = false;
  QuickNoteStore::stamp(stamp, sizeof(stamp));
  assert(!std::strcmp(stamp, "boot"));
  Clock::synced = true;
  char small[8];
  QuickNoteStore::stamp(small, sizeof(small));  // does not fit: falls back, never truncates a stamp
  assert(!std::strcmp(small, "boot"));
}

// A note saved before the clock was set, and the stamp the clock later gives it.
static const char* kLater = "20261004T153017Z";
static std::string bootNote(uint64_t id, const char* text = kText, bool sd = false) {
  const bool synced = Clock::synced;
  Clock::synced = false;
  VFS::sdWritable = sd;
  std::string path;
  assert(save(id, &path, nullptr, "alice", text));
  Clock::synced = synced;
  return path;
}
static bool restamping(const char* user, const std::string& path, const char* stamp, std::string* out = nullptr,
                       bool* retry = nullptr, const char** why = nullptr) {
  char target[QuickNotePolicy::kPathMax] = "junk";
  bool again = true;
  const char* reason = nullptr;
  const bool ok = QuickNoteStore::restamp(SOURCE_G2_GLASSES, user, path.c_str(), stamp, target, sizeof(target),
                                          &again, &reason);
  assert(!locks && !filesOpen && ok == (reason == nullptr));
  assert(ok ? !again && target[0] : !target[0]);
  if (out) *out = target;
  if (retry) *retry = again;
  if (why) *why = reason;
  return ok;
}

static void test_restamp() {
  for (bool sd : {false, true}) {
    reset();
    const std::string boot = bootNote(0x51, kText, sd);
    const std::string folder = sd ? "/sd/stt/u7/notes/" : "/stt/u7/notes/";
    assert(boot == folder + "boot-0000000000000051.txt");
    const std::string before = tree.at(boot).data;
    VFS::contexts.clear(); VFS::renames.clear(); lockTakes = 0; idLookups = 0; VFS::notedBytes = 0;
    VFS::systemAuths = 0;
    std::string target;
    // The owner's session need not be live any more.
    live = false;
    assert(restamping("alice", boot, kLater, &target));
    assert(target == folder + "20261004T153017Z-0000000000000051.txt" && !tree.count(boot));
    assert(countFiles(".tmp") == 0 && countFiles(".txt") == 1 && lockTakes == 1 && idLookups == 1);
    // Copy to tmp, rename into place, and only then the boot file goes; all as
    // the owner confined to her own folder, never the trusted context.
    assert(VFS::renames.size() == 1 && VFS::renames[0].second == target &&
           VFS::renames[0].first == folder + "20261004T153017Z-0000000000000051.tmp");
    assert(!VFS::systemAuths);
    // Confined tighter than create: to this one notes folder.
    for (const auto& c : VFS::contexts) assert(c == (sd ? "alice|/sd/stt/u7/notes" : "alice|/stt/u7/notes"));
    const std::string& after = tree.at(target).data;
    NoteHeader was, now;
    size_t wasBody = 0, nowBody = 0;
    assert(noteParseHeader(before.data(), before.size(), &was, &wasBody) &&
           noteParseHeader(after.data(), after.size(), &now, &nowBody));
    assert(!std::strcmp(now.created, kLater) && now.boot == 76 && now.uptimeS == 5 && now.id == was.id &&
           now.words == was.words && !std::strcmp(now.title, was.title) && now.durationMs == was.durationMs &&
           now.trigger == was.trigger && now.mic == was.mic);
    assert(after.substr(nowBody) == before.substr(wasBody) && after.substr(nowBody) == kText);
    assert(after.size() == before.size() + 12);  // "boot" -> a 16-character stamp
    assert(sd ? VFS::notedBytes == 0 : VFS::notedBytes == after.size());
    // It now lists like any note saved with the clock set.
    live = true;
    assert(listing("alice", 0, sd, 0) && page.count == 1 && page.rows[0].headerValid &&
           page.rows[0].path == target && !std::strcmp(page.rows[0].header.created, kLater));
  }

  // A full-size note crosses several copy pieces intact.
  reset();
  const std::string longest(QuickNotePolicy::kMaxNoteBytes, 'q');
  std::string boot = bootNote(0x52, longest.c_str()), target;
  assert(restamping("alice", boot, kLater, &target) && tree.at(target).data.size() > longest.size());
  {
    const std::string& data = tree.at(target).data;
    size_t body = 0;
    NoteHeader h;
    assert(noteParseHeader(data.data(), data.size(), &h, &body) && data.substr(body) == longest);
  }

  // An earlier pass renamed its copy but the delete failed: both exist; a
  // repeat only deletes the boot copy.
  reset();
  boot = bootNote(0x53);
  const std::string original = tree.at(boot).data;
  assert(restamping("alice", boot, kLater, &target));
  tree[boot] = Node{false, original};
  const std::string copy = tree.at(target).data;
  VFS::renames.clear();
  writes = 0;
  assert(restamping("alice", boot, kLater, &target) && !tree.count(boot) && tree.at(target).data == copy);
  assert(VFS::renames.empty() && writes == 0);
  // A different file under the target name is never replaced or removed.
  tree[boot] = Node{false, original};
  tree.at(target).data = "someone else";
  bool retry = true;
  const char* why = nullptr;
  assert(!restamping("alice", boot, kLater, nullptr, &retry, &why) && !retry &&
         !std::strcmp(why, "Note filename already exists"));
  assert(tree.at(boot).data == original && tree.at(target).data == "someone else");

  // Refusals that a retry cannot fix: no lock is taken for a bad request.
  reset();
  boot = bootNote(0x54);
  std::string stamped;
  assert(save(0x55, &stamped));  // a note saved with the clock set
  struct Bad { const char* user; std::string path; const char* stamp; const char* why; };
  const Bad bad[] = {
      {"alice", stamped, kLater, "Not a boot note"},
      {"alice", boot, "boot", "Not a boot note"},
      {"alice", boot, "20261004T15301Z", "Not a boot note"},
      {"alice", boot, "2026-10-04T15:30", "Not a boot note"},
      {"alice", boot, nullptr, "Not a boot note"},
      {"alice", "/stt/u7/boot-0000000000000054.txt", kLater, "Not a boot note"},
      {"", boot, kLater, "Not a boot note"},
      {nullptr, boot, kLater, "Not a boot note"},
  };
  for (const Bad& b : bad) {
    lockTakes = 0;
    assert(!restamping(b.user, b.path, b.stamp, nullptr, &retry, &why) && !retry && !std::strcmp(why, b.why));
    assert(!lockTakes);
  }
  // Another account, a deleted account, or the name now on another account.
  assert(!restamping("bob", boot, kLater, nullptr, &retry, &why) && !retry &&
         !std::strcmp(why, "Note account unavailable"));
  assert(!restamping("mallory", boot, kLater, nullptr, &retry, &why) && !retry);
  roster["alice"] = {9, "user"};
  assert(!restamping("alice", boot, kLater, nullptr, &retry, &why) && !retry &&
         !std::strcmp(why, "Note account unavailable"));
  roster["alice"] = {7, "user"};
  // Gone, or not the note its name says.
  assert(!restamping("alice", "/stt/u7/notes/boot-00000000000000ff.txt", kLater, nullptr, &retry, &why) &&
         !retry && !std::strcmp(why, "Note unavailable"));
  const std::string original54 = tree.at(boot).data;
  for (const char* damage : {"Id: 0000000000000054", "Created: boot"}) {
    std::string& data = tree.at(boot).data;
    data = original54;
    data.replace(data.find(damage), std::strlen(damage),
                 damage[0] == 'I' ? "Id: 0000000000000099" : "Created: 20261004T153012Z");
    assert(!restamping("alice", boot, kLater, nullptr, &retry, &why) && !retry &&
           !std::strcmp(why, "Note header unreadable"));
  }
  tree.at(boot).data = "not a note";
  assert(!restamping("alice", boot, kLater, nullptr, &retry, &why) && !retry);
  tree.at(boot).data = original54;
  assert(countFiles(".tmp") == 0 && tree.at(boot).data == original54);

  // Faults a later pass may get past: the boot note is always left intact and
  // nothing partial remains.
  struct Fault { const char* name; const char* why; };
  const Fault faults[] = {
      {"busy", "Note storage busy"}, {"short read", "Note read incomplete"},
      {"below reserve", "Note storage full or unavailable"}, {"stats", "Note storage full or unavailable"},
      {"open", "Cannot open note file"}, {"short header", "Note write incomplete"},
      {"short text", "Note write incomplete"}, {"write error", "Note write incomplete"},
      {"size", "Note read incomplete"},  // the file's size and its bytes disagree
      {"rename", "Cannot save note"},
      {"verify", "Note verification failed"}, {"remove", "Cannot remove boot note"},
  };
  for (const Fault& f : faults) {
    reset();
    boot = bootNote(0x56);
    const std::string data = tree.at(boot).data;
    const std::string name = f.name;
    if (name == "busy") lockAvailable = false;
    if (name == "short read") shortRead = true;
    if (name == "below reserve") VFS::freeBytes = QuickNoteStore::kReserveBytes + data.size() + 12 + 255;
    if (name == "stats") VFS::statsOk = false;
    if (name == "open") VFS::denyOpen = true;
    if (name == "short header") shortWrite = writes + 1;
    if (name == "short text") shortWrite = writes + 2;
    if (name == "write error") writeError = true;
    if (name == "size") sizeSkew = true;
    if (name == "rename") VFS::failRename = true;
    if (name == "verify") VFS::skewAfterRename = 1;
    if (name == "remove") VFS::failRemove = true;
    retry = false;
    if (restamping("alice", boot, kLater, nullptr, &retry, &why) || !retry || std::strcmp(why, f.why)) {
      std::fprintf(stderr, "restamp fault %s: %s\n", f.name, why ? why : "(ok)");
      assert(false);
    }
    assert(tree.count(boot) && countFiles(".tmp") == 0);
    if (name != "short read" && name != "size" && name != "busy") assert(tree.at(boot).data == data);
    // Only the remove fault leaves the verified copy (the next pass deletes the boot file).
    assert(countFiles(".txt") == (name == "remove" ? 2u : 1u));
    shortRead = sizeSkew = writeError = false; lockAvailable = true; shortWrite = 0;
    VFS::freeBytes = 1 << 20; VFS::statsOk = true; VFS::denyOpen = VFS::failRename = VFS::failRemove = false;
    VFS::skewAfterRename = 0;
    assert(restamping("alice", boot, kLater, &target) && !tree.count(boot) && countFiles(".txt") == 1);
  }
  // Exactly the reserve plus the copy plus the margin is enough.
  reset();
  boot = bootNote(0x57);
  VFS::freeBytes = QuickNoteStore::kReserveBytes + tree.at(boot).data.size() + 12 + 256;
  assert(restamping("alice", boot, kLater));
}

static void test_clock_helpers() {
  reset();
  uint32_t boot = 0, uptime = 0;
  QuickNoteStore::bootClock(&boot, &uptime);
  assert(boot == 76 && uptime == 5);
  QuickNoteStore::bootClock(nullptr, nullptr);
  char stamp[QuickNotePolicy::kStampMax + 1];
  assert(QuickNoteStore::stampAt(1791127812, stamp, sizeof(stamp)) && !std::strcmp(stamp, "20261004T153012Z"));
  assert(!QuickNoteStore::stampAt(0, stamp, sizeof(stamp)) && !stamp[0]);           // 1970: no wall clock
  assert(!QuickNoteStore::stampAt(4102444800LL, stamp, sizeof(stamp)) && !stamp[0]); // 2100
  char small[8];
  assert(!QuickNoteStore::stampAt(1791127812, small, sizeof(small)) && !small[0]);
  // A note's moment from its uptime, once the clock is set. Uptime 0 of this
  // boot was 15:30:06.85, fixed by the first call, so a retry names the same file.
  Clock::synced = false;
  assert(!QuickNoteStore::stampSince(5, stamp, sizeof(stamp)) && !stamp[0]);
  Clock::synced = true;
  assert(QuickNoteStore::stampSince(5, stamp, sizeof(stamp)) && !std::strcmp(stamp, "20261004T153011Z"));
  assert(QuickNoteStore::stampSince(0, stamp, sizeof(stamp)) && !std::strcmp(stamp, "20261004T153006Z"));
  Clock::epochNow += 3600;  // later, and the wall clock stepped by a second meanwhile
  uptimeUs += 3599LL * 1000000;
  assert(QuickNoteStore::stampSince(5, stamp, sizeof(stamp)) && !std::strcmp(stamp, "20261004T153011Z"));
  assert(QuickNoteStore::stampSince(3605, stamp, sizeof(stamp)) && !std::strcmp(stamp, "20261004T163011Z"));
}

int main() {
  test_create();
  test_create_refusals();
  test_list();
  test_boot_order();
  test_read_and_remove();
  test_room_and_stamp();
  test_restamp();
  test_clock_helpers();
  assert(!locks && !filesOpen && allocs == frees);
  std::puts("QuickNote store: owner-only create/list/read/delete/restamp, tmp+rename, reserve, faults, aliases and boot clock passed");
}
