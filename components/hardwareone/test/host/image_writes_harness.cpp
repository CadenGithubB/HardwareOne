#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static unsigned concatCalls = 0, failConcatCall = 0;
// INSERT_BOUNDARIES
// INSERT_FS_GUARD

template <class... Args> static void logIgnored(const char*, Args...) {}
#define INFO_STORAGEF(...) logIgnored(__VA_ARGS__)
#define ERROR_STORAGEF(...) logIgnored(__VA_ARGS__)
#define DEBUG_STORAGEF(...) logIgnored(__VA_ARGS__)
#define WARN_STORAGEF(...) logIgnored(__VA_ARGS__)
#define FILE_READ "r"
#define FILE_WRITE "w"

static constexpr uint8_t jpeg[] = {0xff, 0xd8, 4, 3, 2, 1};
static constexpr size_t reserveBytes = 100 * 1024;
struct Settings { String cameraCaptureFolder = "/photos"; } gSettings;
static bool filesystemReady = true, sdMounted = true;
static bool expectOuterLock = false;
static time_t fakeNow = 0;
static time_t fakeTime(time_t*) { return fakeNow; }
static tm* fakeLocaltime(const time_t*, tm* out) {
  *out = {}; out->tm_year = 126; out->tm_mon = 9; out->tm_mday = 9;
  out->tm_hour = 12; out->tm_min = 34; out->tm_sec = 56; return out;
}
namespace Clock { static bool isValidEpoch(time_t value) { return value != 0; } }

enum class Fault { None, Open, Short, Zero, WriteError, FlushError, SizeMismatch, Directory, Cleanup, Promotion, PromotionCleanup, RecoveryBackup };
struct Tier {
  Fault fault = Fault::None;
  bool statsFailed = false, mkdirFailed = false, denied = false, openDenied = false;
  uint64_t free = 8 * 1024 * 1024;
  unsigned stats = 0, opens = 0, writes = 0, flushes = 0, closes = 0, removes = 0, mkdirs = 0, promotions = 0;
} tiers[2];
struct AuthContext { String user, reason; };
struct Access { std::string operation, path, reason; };
static std::vector<Access> accesses;
static std::set<std::string> directories;
static unsigned activeFiles = 0, unlockedIo = 0, sdFailureNotes = 0;
static size_t internalBytesNoted = 0;
static void io() { if (!heldFs) ++unlockedIo; }
struct Node {
  std::vector<uint8_t> bytes;
  int tier = 0;
  bool directory = false, error = false, flushed = false;
  std::string name;                               // backend-relative name for directory listings
  std::vector<std::shared_ptr<Node>> children;    // direct children of a listed folder
};
static std::map<std::string, std::shared_ptr<Node>> files;
struct Handle {
  explicit Handle(std::shared_ptr<Node> value) : node(std::move(value)) { ++activeFiles; }
  ~Handle() { close(); }
  void close() {
    if (closed) return;
    io(); closed = true; ++tiers[node->tier].closes; --activeFiles;
  }
  std::shared_ptr<Node> node;
  size_t cursor = 0;
  bool closed = false;
};
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Node> value) : handle_(std::make_shared<Handle>(std::move(value))) {}
  explicit operator bool() const { return handle_ && !handle_->closed; }
  bool isDirectory() const { io(); return handle_->node->directory; }
  const char* name() const { io(); return handle_->node->name.c_str(); }
  time_t getLastWrite() const { io(); return 0; }
  File openNextFile() {
    io();
    if (!*this || handle_->cursor >= handle_->node->children.size()) return {};
    return File(handle_->node->children[handle_->cursor++]);
  }
  size_t write(const uint8_t* data, size_t length) {
    io(); auto& node = *handle_->node; auto& tier = tiers[node.tier]; ++tier.writes;
    // A host fault boundary must not dereference an intentionally oversized
    // request used to verify admission overflow. Baseline still reports success.
    size_t count = length > sizeof(jpeg) ? 0 : length;
    if (tier.fault == Fault::Short || tier.fault == Fault::Cleanup) count = std::min(size_t(2), count);
    if (tier.fault == Fault::Zero || node.directory) count = 0;
    if (!node.directory) node.bytes.assign(data, data + count);
    if (tier.fault == Fault::WriteError) node.error = true;
    return count;
  }
  void flush() {
    io(); auto& node = *handle_->node; ++tiers[node.tier].flushes; node.flushed = true;
    // This is a reported-error fault, not a claim that Arduino currently
    // propagates real fsync/fflush failures through Print::getWriteError.
    if (tiers[node.tier].fault == Fault::FlushError) node.error = true;
  }
  int getWriteError() const { io(); return handle_->node->error; }
  size_t size() const { io(); return handle_->node->bytes.size() + (tiers[handle_->node->tier].fault == Fault::SizeMismatch ? 1 : 0); }
  void close() { if (handle_) handle_->close(); handle_.reset(); }
 private:
  std::shared_ptr<Handle> handle_;
};
// INSERT_NORMALIZE_FS_PATH
namespace VFS {
enum StorageType { INTERNAL = 0, SDCARD = 1, AUTO = 2 };
struct VirtualEntry { const char* name; bool isFolder; };
static bool isSDAvailable() { return sdMounted; }
// Advanced by every successful mount, as VFS does for boot, remount and format.
static uint32_t sdGeneration = 0;
static uint32_t sdMountGeneration() { return sdGeneration; }
String normalize(const String&);
}
// INSERT_VFS_PATHS
// INSERT_DIRECTORY_READER
namespace VFS {
static AuthContext systemAuth(const char* reason) { return {"system", reason}; }
namespace Scopes {  // the production Scopes (System_VFS.h); keep in step
  inline constexpr const char* VIDEOS = "/sd/VIDEOS"; inline constexpr const char* CERTS = "/system/certs";
  inline constexpr const char* OTA = "/system/ota"; inline constexpr const char* ESPNOW_RECEIVED = "/espnow/received";
  inline constexpr const char* CAPTURES = "/logging_captures"; inline constexpr const char* CAPTURES_SD = "/sd/logging_captures";
  inline constexpr const char* STT_MODELS = "/STT Models"; inline constexpr const char* STT_MODELS_SD = "/sd/STT Models";
  inline constexpr const char* SYS_LOGS = "/system/sys_logs"; inline constexpr const char* SYS_LOGS_SD = "/sd/system/sys_logs";
  inline constexpr const char* SYSTEM_STATE = "/system"; inline constexpr const char* USERS = "/system/users";
  inline constexpr const char* MAPS = "/maps"; inline constexpr const char* EI_MODELS = "/EI Models";
  inline constexpr const char* RECORDINGS = "/recordings"; inline constexpr const char* RECORDINGS_SD = "/sd/recordings";
  inline constexpr const char* ESPNOW_STATE = "/system/espnow"; inline constexpr const char* ESPNOW_CAPTURES = "/sd/espnow";
  inline constexpr const char* LLM_MODELS = "/system/llm"; inline constexpr const char* LLM_MODELS_SD = "/sd/llm";
  inline constexpr const char* ESPSR = "/ESPSR"; inline constexpr const char* ESPSR_SD = "/sd/ESPSR";
  inline constexpr const char* PICTURES_SD = "/sd/PICTURES";
}  // namespace Scopes
static AuthContext systemAuth(const char*, const char* reason) { return systemAuth(reason); }
// Tier pickers mirroring System_VFS.h (accept const char* or any String-like).
inline const char* fakeVfsCStr(const char* p) { return p; }
template <class S> inline const char* fakeVfsCStr(const S& s) { return s.c_str(); }
template <class P> inline bool fakeVfsOnSd(const P& p) { return std::strncmp(fakeVfsCStr(p), "/sd/", 4) == 0; }
template <class P> inline const char* captureScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::CAPTURES_SD : Scopes::CAPTURES; }
template <class P> inline const char* sysLogScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::SYS_LOGS_SD : Scopes::SYS_LOGS; }
template <class P> inline const char* recordingScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::RECORDINGS_SD : Scopes::RECORDINGS; }
template <class P> inline const char* llmModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::LLM_MODELS_SD : Scopes::LLM_MODELS; }
template <class P> inline const char* espsrScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::ESPSR_SD : Scopes::ESPSR; }
template <class P> inline const char* sttModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::STT_MODELS_SD : Scopes::STT_MODELS; }
// end tier pickers
static bool allowed(const char* operation, const String& raw, const AuthContext& auth, String& path) {
  io(); check(auth.user == "system" && auth.reason.length(), "image writer keeps explicit trusted identity and audit reason");
  accesses.push_back({operation, raw.c_str(), auth.reason.c_str()});
  if (!normalizeFsPath(raw, path)) return false;
  const auto storage = getStorageType(path);
  return !tiers[storage].denied &&
         !(tiers[storage].openDenied && std::strcmp(operation, "open") == 0) &&
         (storage == SDCARD ? sdMounted : filesystemReady);
}
static bool existsGuarded(const String& raw, const AuthContext& auth) {
  FsLockGuard guard("mock.exists"); String path;
  return allowed("exists", raw, auth, path) && (directories.count(path.c_str()) || files.count(path.c_str()));
}
static bool mkdirGuarded(const String& raw, const AuthContext& auth) {
  FsLockGuard guard("mock.mkdir"); String path;
  if (!allowed("mkdir", raw, auth, path)) return false;
  auto& tier = tiers[getStorageType(path)]; ++tier.mkdirs;
  if (tier.mkdirFailed) return false;
  directories.insert(path.c_str()); return true;
}
static File openGuarded(const String& raw, const char* mode, const AuthContext& auth, bool create = false) {
  FsLockGuard guard("mock.open"); String path;
  const bool admitted = allowed("open", raw, auth, path);
  auto& tier = tiers[getStorageType(raw)];
  if (std::strcmp(mode, "r") == 0) {
    // Read opens list the capture folder for the stale-temporary sweep; they
    // are not counted as image output opens.
    check(!create && directories.count(path.c_str()), "read opens target an existing capture folder");
    if (!admitted || tier.fault == Fault::Open) return {};
    auto folder = std::make_shared<Node>(); folder->tier = getStorageType(path); folder->directory = true;
    const std::string prefix = std::string(path.c_str()) + "/";
    for (const auto& item : files) {
      if (item.first.compare(0, prefix.size(), prefix) != 0 || item.first.find('/', prefix.size()) != std::string::npos) continue;
      item.second->name = stripSdPrefix(item.first.c_str()).c_str();
      folder->children.push_back(item.second);
    }
    return File(folder);
  }
  ++tier.opens;
  check(std::strcmp(mode, "w") == 0 && create, "image saves explicitly create/truncate the guarded output");
  if (!admitted || tier.fault == Fault::Open) return {};
  auto node = std::make_shared<Node>(); node->tier = getStorageType(path); node->directory = tier.fault == Fault::Directory;
  files[path.c_str()] = node; return File(node);
}
static bool removeGuarded(const String& raw, const AuthContext& auth) {
  FsLockGuard guard("mock.remove"); String path;
  const bool admitted = allowed("remove", raw, auth, path);
  auto& tier = tiers[getStorageType(raw)]; ++tier.removes;
  check(activeFiles == 0, "failed output is closed before cleanup removal");
  return admitted && tier.fault != Fault::Cleanup && tier.fault != Fault::PromotionCleanup && files.erase(path.c_str()) != 0;
}
// Promotion is a tested service boundary here; the VFS suite exercises its
// actual LittleFS/SD replacement and rollback implementation separately.
static bool replaceFileGuarded(const String& from, const String& to, const AuthContext& auth) {
  String source, destination;
  if (!allowed("replace-from", from, auth, source) || !allowed("replace-to", to, auth, destination)) return false;
  auto& tier = tiers[getStorageType(source)]; ++tier.promotions;
  check(activeFiles == 0 && heldFs, "complete temporary closes before promotion under the same lock");
  check(files.count(source.c_str()) && files.at(source.c_str())->flushed, "promotion receives the flushed temporary");
  if (tier.fault == Fault::Promotion || tier.fault == Fault::PromotionCleanup) return false;
  if (tier.fault == Fault::RecoveryBackup) {
    // A failed SD rollback may retain the old destination as a recovery backup.
    if (files.count(destination.c_str())) {
      files[std::string(destination.c_str()) + ".bak"] = files.at(destination.c_str());
      files.erase(destination.c_str());
    }
    return false;
  }
  files[destination.c_str()] = files.at(source.c_str()); files.erase(source.c_str());
  return true;
}
static void noteLittleFsBytesWritten(size_t bytes) {
  io(); check(activeFiles == 0 && bytes > 0, "internal write accounting follows close and counts actual positive bytes");
  internalBytesNoted += bytes;
}
static void noteSDWriteFailure(const char* reason) {
  io(); check(activeFiles == 0 && std::strcmp(reason, "image save") == 0, "SD write health records closed incomplete image attempt");
  ++sdFailureNotes;
}
static bool getStats(StorageType type, uint64_t& total, uint64_t& used, uint64_t& free) {
  FsLockGuard guard("mock.stats"); io(); auto& tier = tiers[type]; ++tier.stats;
  if (tier.statsFailed || !(type == SDCARD ? sdMounted : filesystemReady)) return false;
  free = tier.free; total = free + 4096; used = 4096; return true;
}
}
static constexpr int SYSEVT_PHOTO_SAVED = 42;
struct SavedEvent { std::string name, path; };
static std::vector<SavedEvent> events;
static std::vector<int> retentions;
static bool retentionSucceeds = true;
static void systemEventPost(int type, const char* name, const char* path) {
  check(type == SYSEVT_PHOTO_SAVED && activeFiles == 0 && heldFs == expectOuterLock,
        "saved event follows file cleanup and writer-owned lock release");
  events.push_back({name, path});
}
#define time fakeTime
#define localtime_r fakeLocaltime
// INSERT_IMAGE_MANAGER
#undef localtime_r
#undef time
bool ImageManager::enforceMaxImages(ImageStorageLocation location) {
  check(activeFiles == 0 && heldFs == expectOuterLock, "retention follows writer resource release");
  retentions.push_back(location); return retentionSucceeds;
}

static void balanced(bool outer = false) {
  check(activeFiles == 0 && unlockedIo == 0, "all file resources close under filesystem lock");
  check(heldFs == outer && gives + unsigned(outer) == takes, "filesystem scopes preserve external ownership and balance");
}
static void clearObservations() {
  for (auto& tier : tiers) tier.stats = tier.opens = tier.writes = tier.flushes = tier.closes = tier.removes = tier.mkdirs = tier.promotions = 0;
  accesses.clear(); events.clear(); retentions.clear(); concatCalls = failConcatCall = 0;
  takes = gives = 0; unlockedIo = 0; internalBytesNoted = 0; sdFailureNotes = 0;
}
static ImageManager setup(bool internal = true, bool sd = true) {
  balanced(); files.clear(); directories.clear(); accesses.clear(); events.clear(); retentions.clear();
  tiers[0] = {}; tiers[1] = {}; filesystemReady = internal; sdMounted = sd;
  if (sd) ++VFS::sdGeneration;
  gSettings.cameraCaptureFolder = "/photos"; fakeNow = 0; retentionSucceeds = true; expectOuterLock = false;
  // Prepare both tiers before observing a save, as a device has by the time
  // it captures: LittleFS is swept at init, SD at its first folder
  // preparation. Per-save observations then exclude the one-time sweep.
  ImageManager out; out.init();
  if (sd) out.ensureCaptureFolder(IMAGE_STORAGE_SD);
  clearObservations(); return out;
}
static const char* pathFor(bool sd) { return sd ? "/sd/photos/img_000000.jpg" : "/photos/img_000000.jpg"; }
static std::string writePathFor(bool sd) { return std::string(pathFor(sd)) + (HAS_TEMP_SAVE ? ".tmp" : ""); }
static ImageStorageLocation locationFor(bool sd) { return sd ? IMAGE_STORAGE_SD : IMAGE_STORAGE_LITTLEFS; }
static void hooks(const String& result, ImageStorageLocation location) {
  if (!result.length()) check(events.empty() && retentions.empty(), "failed save must publish no saved event or retention work");
  else check(events.size() == 1 && events[0].path == result.c_str() && retentions == std::vector<int>{location},
             "successful save publishes once and retains once with original location policy");
}
static void cleanupIdentity(bool sd) {
  const std::string expected = sd ? "imgmgr.save_sd" : "imgmgr.save_image_lfs";
  for (const auto& access : accesses) {
    if ((access.operation == "open" || access.operation == "remove" || access.operation.find("replace-") == 0) && VFS::getStorageType(access.path.c_str()) == (sd ? VFS::SDCARD : VFS::INTERNAL))
      check(access.reason == expected, "write and failed-output cleanup use the same existing save identity");
  }
}
static void incomplete(bool sd, Fault failure) {
  auto owner = setup(); tiers[sd].fault = failure;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(!result.length(), "incomplete or rejected image output must not report success");
  auto& tier = tiers[sd];
  check(tier.opens == 1 && tier.closes == unsigned(failure != Fault::Open), "every acquired output file is closed once");
  const bool writable = failure != Fault::Open && failure != Fault::Directory;
  check(tier.removes == unsigned(writable), "only failed opened regular outputs receive cleanup");
  if (writable && failure != Fault::Cleanup) check(!files.count(writePathFor(sd)), "failed output is removed best-effort");
  if (failure == Fault::Cleanup) check(files.count(writePathFor(sd)) == 1, "cleanup refusal keeps failure visible despite residual partial bytes");
  if (failure == Fault::Directory) check(files.count(writePathFor(sd)) == 1 && !tier.writes, "directory output is never written or removed");
  check(!tier.promotions, "incomplete bytes never reach promotion");
  const size_t expectedBytes = !writable || failure == Fault::Zero ? 0 :
      failure == Fault::Short || failure == Fault::Cleanup ? 2 : sizeof(jpeg);
  check(internalBytesNoted == (sd ? 0 : expectedBytes) && sdFailureNotes == unsigned(sd && writable),
        "actual partial internal bytes are accounted; only incomplete attempted SD writes update SD health");
  cleanupIdentity(sd); hooks(result, locationFor(sd)); balanced();
}
static void preserveBeforeOpen(bool sd, bool denial) {
  auto owner = setup();
  auto original = std::make_shared<Node>(); original->tier = sd;
  original->bytes = {7, 8, 9}; files[pathFor(sd)] = original;
  if (denial) tiers[sd].openDenied = true;
  else tiers[sd].fault = Fault::Open;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(!result.length() && tiers[sd].opens == 1 && !tiers[sd].writes && !tiers[sd].closes && !tiers[sd].removes,
        "denied/failed open cannot write, close or remove an unacquired destination");
  check(files.at(pathFor(sd)) == original && original->bytes == std::vector<uint8_t>({7, 8, 9}),
        "existing destination sentinel survives failed/denied open unchanged");
  check(!internalBytesNoted && !sdFailureNotes, "unacquired file causes no write accounting or SD health failure");
  cleanupIdentity(sd); hooks(result, locationFor(sd)); balanced();
}
static void bothShort(bool failInternal, bool failSD) {
  auto owner = setup(); if (failInternal) tiers[0].fault = Fault::Short; if (failSD) tiers[1].fault = Fault::Short;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH);
  const String expected = !failInternal ? pathFor(false) : !failSD ? pathFor(true) : "";
  check(result == expected, "BOTH reports only a fully saved tier and prefers successful internal output");
  for (unsigned tier = 0; tier < 2; ++tier) {
    const bool failed = tier == 0 ? failInternal : failSD;
    check(tiers[tier].writes == 1 && tiers[tier].removes == unsigned(failed) && files.count(pathFor(tier)) == unsigned(!failed), "BOTH attempts and cleans each tier independently");
    cleanupIdentity(tier);
  }
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
static void failedFolder(bool sd) {
  auto owner = setup(); directories.erase(sd ? "/sd/photos" : "/photos"); tiers[sd].mkdirFailed = true;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(!result.length() && tiers[sd].opens == 0, "failed capture-folder creation must prevent opening the image output");
  hooks(result, locationFor(sd)); balanced();
}
static void lowSpaceBoth(unsigned lowTier, bool bothLow = false) {
  auto owner = setup(); tiers[lowTier].free = reserveBytes + sizeof(jpeg) - 1;
  if (bothLow) tiers[1 - lowTier].free = 0;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH);
  check(tiers[lowTier].opens == 0 && tiers[lowTier].writes == 0, "BOTH must not write a tier that failed free-space admission");
  check(result == (bothLow ? String("") : String(pathFor(!lowTier))), "BOTH selects only the admitted tier");
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
static void overflowLength() {
  auto owner = setup();
  const auto result = owner.saveImage(jpeg, SIZE_MAX, IMAGE_STORAGE_BOTH);
  check(!result.length() && !tiers[0].stats && !tiers[1].stats && !tiers[0].opens && !tiers[1].opens,
        "oversized length cannot wrap required capacity and must be rejected before I/O");
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
static void successful(ImageStorageLocation location) {
  auto owner = setup(); const auto result = owner.saveImage(jpeg, sizeof(jpeg), location);
  check(result == pathFor(location == IMAGE_STORAGE_SD), "successful output keeps existing filename and preferred path");
  for (unsigned tier = 0; tier < 2; ++tier) {
    const bool wanted = location == IMAGE_STORAGE_BOTH || unsigned(location) == tier;
    check(tiers[tier].stats == unsigned(wanted) && tiers[tier].opens == unsigned(wanted) && tiers[tier].flushes == unsigned(wanted), "each requested available tier samples fresh capacity and completes one output");
    if (wanted) check(files.at(pathFor(tier))->bytes == std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg)) && !tiers[tier].removes, "complete image bytes are retained without cleanup");
    cleanupIdentity(tier);
  }
  check(internalBytesNoted == (location == IMAGE_STORAGE_SD ? 0 : sizeof(jpeg)) && sdFailureNotes == 0,
        "complete internal writes update existing accounting; complete SD writes do not invalidate health");
  hooks(result, location); balanced();
}
static void admission(bool sd, unsigned failure) {
  auto owner = setup(); auto& tier = tiers[sd];
  if (failure == 0) tier.statsFailed = true;
  if (failure == 1) tier.free = reserveBytes + sizeof(jpeg) - 1;
  if (failure == 2) tier.free = reserveBytes + sizeof(jpeg);
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(bool(result.length()) == (failure == 2) && tier.opens == unsigned(failure == 2), "single-tier admission checks query outcome and exact safety-margin threshold");
  check(sdFailureNotes == 0, "admission/query failures do not mark SD write health failed");
  hooks(result, locationFor(sd)); balanced();
}
static void unavailable(bool internal, bool sd, ImageStorageLocation location) {
  auto owner = setup(internal, sd);
  // Both backends come up after init: LittleFS stays gated by the manager's
  // init snapshot, while SD is asked of VFS at save time.
  filesystemReady = true;
  if (!sdMounted) { sdMounted = true; ++VFS::sdGeneration; }
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), location);
  const bool expectedInternal = internal && location != IMAGE_STORAGE_SD;
  const bool expectedSD = location != IMAGE_STORAGE_LITTLEFS;
  check(result == (expectedInternal ? String(pathFor(false)) : expectedSD ? String(pathFor(true)) : String("")), "init snapshot gates LittleFS, VFS gates SD, BOTH falls back");
  check(tiers[0].stats == unsigned(expectedInternal) && tiers[1].stats == unsigned(expectedSD), "unavailable manager tiers perform no capacity query");
  hooks(result, location); balanced();
}
static void invalidInput(bool nullData) {
  auto owner = setup(); const auto result = owner.saveImage(nullData ? nullptr : jpeg, nullData ? sizeof(jpeg) : 0, IMAGE_STORAGE_BOTH);
  check(!result.length() && accesses.empty() && !tiers[0].stats && !tiers[1].stats, "null/empty image input performs no storage work");
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
static void failedConcat(bool sd, unsigned call) {
  auto owner = setup(); failConcatCall = call;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(!result.length() && !tiers[sd].opens && files.empty(), "failed complete output-path construction cannot open a truncated path");
  hooks(result, locationFor(sd)); balanced();
}
#if HAS_FOLDER_VALIDATOR
// The setting's writers refuse a folder that names a tier or steps outside it,
// with a reply that points at cameraStorageLocation; plain folders pass.
static void folderValidator() {
  for (const char* rejected : {"/sd", "/sd/", "/sd/photos", "sd/photos", "sd", "/../photos", "/photos/../x", ".."}) {
    const char* why = ImageManager::captureFolderRejectReason(rejected);
    check(why && std::strncmp(why, "Error:", 6) == 0, "tier-prefixed or traversing folder accepted");
    check(std::strstr(why, "cameraCaptureFolder") != nullptr, "rejection does not name the setting");
  }
  check(std::strstr(ImageManager::captureFolderRejectReason("/sd/photos"), "cameraStorageLocation") != nullptr,
        "tier rejection does not point at the tier setting");
  for (const char* accepted : {"/photos", "photos", "/sdata", "/sd-card/photos", "/sdx", "/", "", "/photos/2026", "/photos/"}) {
    check(ImageManager::captureFolderRejectReason(accepted) == nullptr, "plain folder rejected");
  }
}
#endif
static void configuredPath(const char* folder, ImageStorageLocation location, const char* expected) {
  auto owner = setup(); gSettings.cameraCaptureFolder = folder;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), location);
  check(result == expected, "configured capture path is validated against the tier and returned canonically");
  if (!result.length()) {
    check(!tiers[0].stats && !tiers[1].stats && !tiers[0].opens && !tiers[1].opens && accesses.empty(),
          "invalid/tier-mismatched capture configuration cannot query or touch another storage tier");
  } else {
    check(files.count(expected) == 1, "reported canonical output path identifies the completed image");
    const bool sd = VFS::getStorageType(result) == VFS::SDCARD;
    check(tiers[sd].writes == 1 && tiers[!sd].writes == 0, "only admitted matching storage tier receives data");
  }
  hooks(result, location); balanced();
}
static void bothRejected(unsigned rejected, unsigned failure) {
  auto owner = setup(); auto& tier = tiers[rejected];
  if (failure == 0) tier.statsFailed = true;
  if (failure == 1) { tier.mkdirFailed = true; directories.erase(rejected ? "/sd/photos" : "/photos"); }
  if (failure == 2) tier.fault = Fault::Open;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH);
  check(result == pathFor(!rejected) && tier.writes == 0 && tier.removes == 0 && sdFailureNotes == 0,
        "BOTH independently tries other tier after query, folder or open failure without deleting/marking the rejected tier");
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
static void nestedSave() {
  auto owner = setup();
  { FsLockGuard outer("caller"); expectOuterLock = true;
    const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH);
    check(result == pathFor(false) && takes == 1 && gives == 0, "nested image save retains original caller lock");
    hooks(result, IMAGE_STORAGE_BOTH); balanced(true);
  }
  expectOuterLock = false; balanced();
}
static void filenameAndRetention() {
  auto owner = setup();
  check(owner.saveImage(jpeg, sizeof(jpeg)) == "/photos/img_000000.jpg", "untimed image uses current counter");
  check(owner.saveImage(jpeg, sizeof(jpeg)) == "/photos/img_000001.jpg", "counter advances once per save call");
  fakeNow = 1; retentionSucceeds = false;
  check(owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_SD) == "/sd/photos/img_20261009_123456.jpg", "valid clock selects timestamp filename; post-save retention failure keeps saved result");
  check(events.size() == 3 && events.back().name == "img_20261009_123456.jpg" && retentions.size() == 3, "generated filename agrees with event payload"); balanced();
}
static void collision(bool sd, Fault failure) {
  auto owner = setup(); fakeNow = 1; tiers[sd].fault = failure;
  const std::string destination = std::string(sd ? "/sd/photos/" : "/photos/") + "img_20261009_123456.jpg";
  const std::string temporary = destination + ".tmp";
  auto original = std::make_shared<Node>(); original->tier = sd; original->bytes = {9, 8, 7};
  files[destination] = original;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  const bool success = failure == Fault::None;
  check(bool(result.length()) == success, "only a completed and promoted image reports success");
  if (success) {
    check(result == destination.c_str() && files.at(destination)->bytes == std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg)),
          "same-second successful save replaces the previous image completely");
  } else if (failure == Fault::RecoveryBackup) {
    check(!files.count(destination) && files.at(destination + ".bak") == original && original->bytes == std::vector<uint8_t>({9, 8, 7}),
          "caller preserves old image retained by failed SD rollback");
  } else {
    check(files.count(destination) && files.at(destination) == original && original->bytes == std::vector<uint8_t>({9, 8, 7}),
          "write or promotion failure preserves previous timestamp image bytes");
  }
  check(files.count(temporary) == unsigned(failure == Fault::Cleanup || failure == Fault::PromotionCleanup),
        "owned temporary removed or promoted; cleanup failure leaves identifiable temporary only");
  if (HAS_TEMP_SAVE) {
    for (const auto& access : accesses) {
      if (access.operation == "open") check(access.path == temporary, "write opens only the temporary");
      if (access.operation == "remove") check(access.path == temporary, "caller removes only its temporary, never destination or backup");
    }
  }
  cleanupIdentity(sd); hooks(result, locationFor(sd)); balanced();
}
#if HAS_STALE_SWEEP
// A remount through VFS alone (sdunmount + sdmount, sdformat): ImageManager is
// not told, it sees a new mount generation.
static void remountSd() { sdMounted = false; sdMounted = true; ++VFS::sdGeneration; }
// `<image>.jpg.tmp` in the capture folder can only be this owner's leftover of
// an interrupted save: the first ensure on a tier after a mount sweeps such
// entries (LittleFS at manager init, SD at its first use), then the save
// completes. Other temporaries, and a leftover appearing later in the same
// mount, are left alone until the next mount.
static void preexistingTemporary(bool sd) {
  auto owner = setup();
  const std::string folder = sd ? "/sd/photos" : "/photos";
  auto original = std::make_shared<Node>(); original->tier = sd; original->bytes = {9, 8, 7};
  auto previousTemp = std::make_shared<Node>(); previousTemp->tier = sd; previousTemp->bytes = {4, 5, 6};
  auto older = std::make_shared<Node>(); older->tier = sd; older->bytes = {1, 2};
  auto unrelated = std::make_shared<Node>(); unrelated->tier = sd; unrelated->bytes = {3};
  files[pathFor(sd)] = original; files[std::string(pathFor(sd)) + ".tmp"] = previousTemp;
  files[folder + "/img_older.jpeg.tmp"] = older; files[folder + "/notes.txt.tmp"] = unrelated;
  // A fresh mount sweeps at the next folder preparation: LittleFS inside
  // init(), SD inside the first save after the card is seen again.
  if (sd) remountSd(); else owner.init();
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(result == pathFor(sd), "save after stale temporaries did not complete");
  check(!files.count(std::string(pathFor(sd)) + ".tmp") && !files.count(folder + "/img_older.jpeg.tmp"),
        "stale photo temporaries were not swept");
  check(files.count(folder + "/notes.txt.tmp") && files.at(folder + "/notes.txt.tmp") == unrelated,
        "sweep removed a temporary that is not a photo");
  check(files.at(pathFor(sd))->bytes == std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg)), "save did not publish the new image");
  hooks(result, locationFor(sd)); balanced();
  auto later = std::make_shared<Node>(); later->tier = sd; later->bytes = {7};
  files[folder + "/img_later.jpg.tmp"] = later;
  check(owner.ensureCaptureFolder(locationFor(sd)) && files.count(folder + "/img_later.jpg.tmp"), "sweep repeated within one mount");
  if (sd) remountSd(); else owner.init();
  check(owner.ensureCaptureFolder(locationFor(sd)) && !files.count(folder + "/img_later.jpg.tmp"), "remount did not sweep again");
  balanced();
}
#else
static void preexistingTemporary(bool sd) {
  auto owner = setup();
  auto original = std::make_shared<Node>(); original->tier = sd; original->bytes = {9, 8, 7};
  auto previousTemp = std::make_shared<Node>(); previousTemp->tier = sd; previousTemp->bytes = {4, 5, 6};
  files[pathFor(sd)] = original; files[std::string(pathFor(sd)) + ".tmp"] = previousTemp;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), locationFor(sd));
  check(result == pathFor(sd) && !files.count(std::string(pathFor(sd)) + ".tmp"),
        "stale photo temporary not reclaimed before the save");
  hooks(result, locationFor(sd)); balanced();
}
#endif
// No card at boot; one is mounted later with sdmount: an SD save succeeds,
// the card's stale photo temporaries are swept once for that mount, and after
// sdunmount SD saves are refused before any storage work.
static void lateMountSave() {
  auto owner = setup(true, false);
  check(!owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_SD).length() && !tiers[1].stats, "no card: SD save refused");
  clearObservations();
  sdMounted = true; ++VFS::sdGeneration;
  auto stale = std::make_shared<Node>(); stale->tier = 1; stale->bytes = {1};
  files["/sd/photos/img_crash.jpg.tmp"] = stale;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_SD);
  // The refused attempt above used counter name 000000.
  check(result == "/sd/photos/img_000001.jpg" && files.at("/sd/photos/img_000001.jpg")->bytes == std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg)),
        "a card mounted after init accepts an SD save");
  check(!files.count("/sd/photos/img_crash.jpg.tmp"), "the newly mounted card's stale temporary is swept");
  hooks(result, IMAGE_STORAGE_SD); balanced();
  auto later = std::make_shared<Node>(); later->tier = 1; later->bytes = {2};
  files["/sd/photos/img_later.jpg.tmp"] = later; clearObservations(); fakeNow = 1;
  check(owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_SD).length() && files.count("/sd/photos/img_later.jpg.tmp"),
        "the sweep runs once per mount, not per save");
  sdMounted = false; clearObservations();
  check(!owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_SD).length() && !tiers[1].stats && !tiers[1].opens && accesses.empty(),
        "after unmount SD saves are refused before any storage work");
  check(owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH) == "/photos/img_20261009_123456.jpg" && !tiers[1].opens,
        "after unmount BOTH saves internally only");
  balanced();
}
static void bothPromotion(unsigned failedTier) {
  auto owner = setup(); tiers[failedTier].fault = Fault::Promotion;
  auto prior = std::make_shared<Node>(); prior->tier = failedTier; prior->bytes = {9, 8, 7}; files[pathFor(failedTier)] = prior;
  const auto result = owner.saveImage(jpeg, sizeof(jpeg), IMAGE_STORAGE_BOTH);
  check(result == pathFor(!failedTier), "BOTH reports the tier whose promotion succeeded");
  check(files.at(pathFor(failedTier)) == prior && prior->bytes == std::vector<uint8_t>({9, 8, 7}), "BOTH preserves previous image on failed tier");
  check(files.at(pathFor(!failedTier))->bytes == std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg)), "other tier independently retains complete image");
  check(tiers[0].promotions == 1 && tiers[1].promotions == 1, "both eligible tiers attempt promotion independently");
  check(!files.count(std::string(pathFor(false)) + ".tmp") && !files.count(std::string(pathFor(true)) + ".tmp"), "BOTH leaves no owned temporary after handled promotion failure");
  hooks(result, IMAGE_STORAGE_BOTH); balanced();
}
int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  unsigned scenarios = 0, failures = 0;
  const auto run = [&](const std::string& name, const std::function<void()>& test) {
    ++scenarios;
    try { test(); std::printf("PASS %s\n", name.c_str()); }
    catch (const std::exception& error) { ++failures; std::printf("FAIL %s: %s\n", name.c_str(), error.what()); }
  };
  if (selected == "all" || selected == "short") {
    for (bool sd : {false, true}) run(std::string(sd ? "SD" : "internal") + " short write", [=] { incomplete(sd, Fault::Short); });
    for (const auto pair : {std::pair<bool, bool>{true, false}, {false, true}, {true, true}})
      run("BOTH short internal=" + std::to_string(pair.first) + " sd=" + std::to_string(pair.second), [=] { bothShort(pair.first, pair.second); });
  }
  if (selected == "all" || selected == "folder") {
    for (bool sd : {false, true}) run(std::string(sd ? "SD" : "internal") + " capture-folder failure", [=] { failedFolder(sd); });
  }
  if (selected == "all" || selected == "capacity") {
    for (unsigned tier = 0; tier < 2; ++tier) run("BOTH low capacity tier " + std::to_string(tier), [=] { lowSpaceBoth(tier); });
    run("BOTH neither tier admitted", [] { lowSpaceBoth(0, true); });
  }
  if (selected == "all" || selected == "overflow") run("capacity addition overflow", overflowLength);
  if (selected == "all" || selected == "collision") {
    for (bool sd : {false, true}) {
      const std::string tier = sd ? "SD " : "internal ";
      for (Fault failure : {Fault::None, Fault::Short, Fault::Zero, Fault::WriteError, Fault::FlushError,
                            Fault::SizeMismatch, Fault::Cleanup, Fault::Promotion, Fault::PromotionCleanup})
        run(tier + "timestamp collision " + std::to_string(int(failure)), [=] { collision(sd, failure); });
      run(tier + "stale temporaries swept once per mount", [=] { preexistingTemporary(sd); });
      run(tier + "BOTH promotion failure", [=] { bothPromotion(sd); });
    }
    run("SD recovery backup belongs to promotion owner", [] { collision(true, Fault::RecoveryBackup); });
    run("SD mounted after init: save, sweep once, refuse after unmount", lateMountSave);
  }
  if (selected == "all") {
    for (bool sd : {false, true}) {
      const std::string tier = sd ? "SD " : "internal ";
      for (const auto fault : {Fault::Open, Fault::Zero, Fault::WriteError, Fault::FlushError, Fault::SizeMismatch, Fault::Directory, Fault::Cleanup})
        run(tier + "write fault " + std::to_string(int(fault)), [=] { incomplete(sd, fault); });
      for (bool denial : {false, true}) run(tier + (denial ? "denied open preserves prior file" : "failed open preserves prior file"), [=] { preserveBeforeOpen(sd, denial); });
      for (unsigned failure = 0; failure < 3; ++failure) run(tier + "admission case " + std::to_string(failure), [=] { admission(sd, failure); });
      for (unsigned call = 1; call <= (HAS_TEMP_SAVE ? 3U : 2U); ++call) run(tier + "path allocation " + std::to_string(call), [=] { failedConcat(sd, call); });
    }
    for (auto location : {IMAGE_STORAGE_LITTLEFS, IMAGE_STORAGE_SD, IMAGE_STORAGE_BOTH}) {
      run("successful tier selection " + std::to_string(location), [=] { successful(location); });
      for (auto availability : {std::pair<bool, bool>{false, false}, {true, false}, {false, true}})
        run("availability " + std::to_string(availability.first) + std::to_string(availability.second) + " selection " + std::to_string(location),
            [=] { unavailable(availability.first, availability.second, location); });
    }
    run("null input", [] { invalidInput(true); }); run("empty input", [] { invalidInput(false); });
    for (unsigned tier = 0; tier < 2; ++tier) for (unsigned failure = 0; failure < 3; ++failure)
      run("BOTH rejected tier " + std::to_string(tier) + " reason " + std::to_string(failure), [=] { bothRejected(tier, failure); });
    for (const char* folder : {"/sd/photos", "/./sd/photos", "//sd/photos"}) {
      run(std::string("configured tier mismatch ") + folder, [=] { configuredPath(folder, IMAGE_STORAGE_LITTLEFS, ""); });
      run(std::string("BOTH configured SD destination ") + folder, [=] { configuredPath(folder, IMAGE_STORAGE_BOTH, "/sd/sd/photos/img_000000.jpg"); });
    }
    run("internal /sdata destination", [] { configuredPath("/sdata", IMAGE_STORAGE_LITTLEFS, "/sdata/img_000000.jpg"); });
    run("configured internal root", [] { configuredPath("/", IMAGE_STORAGE_LITTLEFS, "/img_000000.jpg"); });
    run("configured SD root", [] { configuredPath("/", IMAGE_STORAGE_SD, "/sd/img_000000.jpg"); });
    run("configured trailing slash", [] { configuredPath("/photos//", IMAGE_STORAGE_SD, "/sd/photos/img_000000.jpg"); });
    run("configured relative folder", [] { configuredPath("pictures", IMAGE_STORAGE_LITTLEFS, "/pictures/img_000000.jpg"); });
    run("configured empty default", [] { configuredPath("", IMAGE_STORAGE_LITTLEFS, "/photos/img_000000.jpg"); });
    run("configured traversal refused", [] { configuredPath("/../photos", IMAGE_STORAGE_BOTH, ""); });
#if HAS_FOLDER_VALIDATOR
    run("capture folder setting validator", folderValidator);
#endif
    run("nested filesystem guard", nestedSave);
    run("counter/clock/event/retention compatibility", filenameAndRetention);
  }
  std::printf("ImageManager production writes: %u scenarios, %zu assertions, %u failed\n", scenarios, checks, failures);
  return failures ? 1 : 0;
}
