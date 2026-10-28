// Self-test of the shared fake VFS (fake_vfs.h): every policy knob, fault,
// the lock seam, the call log and the VFS:: forwarders, plus the production
// writeText/writeTextAtomic and FsLockGuard compiled against it unchanged.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include "System_PollPause.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
volatile bool gSensorPollingPaused = false;
static unsigned pauseDepth = 0;
void pollPause(uint8_t) { ++pauseDepth; }
void pollResume(uint8_t) { if (pauseDepth) --pauseDepth; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }

// INSERT_FS_GUARD
// INSERT_WRITERS

static unsigned checks = 0;
static void check(bool ok, const std::string& reason) {
  ++checks;
  if (!ok) throw std::runtime_error(reason);
}
static fake_vfs::Fs& reset() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  return fs;
}

static void rootsAndHelpers() {
  auto& fs = reset();
  check(fs.exists("/") && fs.isDirectory("/") && fs.exists("/sd") && fs.isDirectory("/sd"), "both roots exist after reset");
  check(fs.mkdirAll("/system/users/deep"), "mkdirAll creates the chain");
  check(fs.isDirectory("/system") && fs.isDirectory("/system/users/deep"), "intermediate directories exist");
  fs.put("/system/users/users.json", "[]");
  check(fs.text("/system/users/users.json") == "[]", "put/text round trip");
  fs.put("/sd/VIDEOS/clip.avi", "RIFF");
  check(fs.isDirectory("/sd/VIDEOS") && fs.text("/sd/VIDEOS/clip.avi") == "RIFF", "put creates SD parents");
  const auto names = fs.list("/system/users");
  check(names.size() == 2 && names[0] == "deep" && names[1] == "users.json", "list is sorted leaf names");
  check(fs.log.empty(), "setup helpers are not logged");
  check(!fs.mkdirAll("/system/users/users.json/x"), "mkdirAll refuses a file in the chain");
  check(fake_vfs::Fs::normalizePath("  system//users/ ") == "/system/users", "normalize trims, prefixes, collapses, strips");
  check(fake_vfs::Fs::normalizePath("") == "/" && fake_vfs::Fs::normalizePath("/sd/") == "/sd", "normalize roots");
  check(fake_vfs::Fs::hasTraversal("/a/../b") && !fake_vfs::Fs::hasTraversal("/a/..b"), "traversal detection");
  check(VFS::normalize("sd//VIDEOS/") == "/sd/VIDEOS", "VFS::normalize forwarder");
  check(VFS::getStorageType("/sd/x") == VFS::SDCARD && VFS::getStorageType("/sdx") == VFS::INTERNAL &&
        VFS::getStorageType("/sd") == VFS::SDCARD, "VFS::getStorageType routing rule");
  check(VFS::stripSdPrefix("/sd/VIDEOS") == "/VIDEOS" && VFS::stripSdPrefix("/sd") == "/" &&
        VFS::stripSdPrefix("/system") == "/system", "VFS::stripSdPrefix");
  check(VFS::isStagingName("/a/b.tmp") && VFS::isStagingName("x.bak") && !VFS::isStagingName("x.json"), "isStagingName");
}

// The firmware's VFS has no raw open/exists/... any more, so neither does the
// fake's VFS namespace. These drive the fake backend directly with the
// unguarded Auth the old forwarders used, to test the backend model itself.
static File unguardedOpen(const String& path, const char* mode, bool create = false) {
  return fake_vfs::fs().open(path.c_str(), mode, create, fake_vfs::Fs::Auth{});
}
static bool unguardedExists(const String& path) { return fake_vfs::fs().exists(path.c_str(), fake_vfs::Fs::Auth{}); }
static bool unguardedMkdir(const String& path) { return fake_vfs::fs().mkdir(path.c_str(), fake_vfs::Fs::Auth{}); }
static bool unguardedRemove(const String& path) { return fake_vfs::fs().remove(path.c_str(), fake_vfs::Fs::Auth{}); }
static bool unguardedRename(const String& from, const String& to) { return fake_vfs::fs().rename(from.c_str(), to.c_str(), fake_vfs::Fs::Auth{}); }
static bool unguardedRmdir(const String& path) { return fake_vfs::fs().rmdir(path.c_str(), fake_vfs::Fs::Auth{}); }

static void openModes() {
  auto& fs = reset();
  check(!unguardedOpen("/missing.txt", "r"), "read of a missing file fails");
  check(fs.count("open") == 1 && !fs.last("open")->ok && fs.last("open")->other == "r", "failed open is logged with its mode");
  File w = unguardedOpen("/note.txt", "w");
  check(static_cast<bool>(w) && fs.liveHandles() == 1, "write open creates the file");
  check(w.print("hello") == 5 && w.print(String(" world")) == 6, "print accepts const char* and String");
  check(w.size() == 11 && w.position() == 11, "size and position track writes");
  w.close();
  check(fs.liveHandles() == 0 && fs.text("/note.txt") == "hello world", "close releases the handle, bytes stored");
  File a = unguardedOpen("/note.txt", "a");
  check(a.println("!") == 3, "println appends CRLF");
  a.close();
  check(fs.text("/note.txt") == "hello world!\r\n", "append mode appends");
  File t = unguardedOpen("/note.txt", "w");
  check(t.size() == 0, "write mode truncates");
  t.write(static_cast<uint8_t>('x'));
  t.close();
  check(fs.text("/note.txt") == "x", "single-byte write");
  File rw = unguardedOpen("/note.txt", "r+");
  check(rw.read() == 'x' && rw.write(reinterpret_cast<const uint8_t*>("yz"), 2) == 2, "r+ reads then writes in place");
  rw.close();
  check(fs.text("/note.txt") == "xyz", "r+ does not truncate");
  check(!unguardedOpen("/nodir/file.txt", "w"), "write without a parent directory fails");
  check(static_cast<bool>(unguardedOpen("/nodir/file.txt", "w", true)), "create=true makes parents");
  check(fs.isDirectory("/nodir"), "parent was created");
  fs.policy.createFlagMakesParents = false;
  check(!unguardedOpen("/other/file.txt", "w", true), "policy can refuse parent creation");
  fs.policy.createFlagMakesParents = true;
  check(!unguardedOpen("/nodir", "w"), "write open on a directory fails");
  File dir = unguardedOpen("/nodir", "r");
  check(static_cast<bool>(dir) && dir.isDirectory() && dir.size() == 0, "read open of a directory yields a directory handle");
  File copy = dir;
  dir.close();
  check(!copy, "copies share one handle");
  check(!unguardedOpen("/", "w") && !unguardedOpen("/sd", "w"), "roots cannot be opened for writing");
  check(!unguardedOpen("/a/../etc", "r"), "traversal is rejected");
  check(fs.liveHandles() == 0 && fs.implicitCloses == 1, "every handle closed; the dropped temporary closed implicitly");
}

static void streamSurface() {
  auto& fs = reset();
  fs.put("/data.txt", "line one\nline two\n");
  File f = unguardedOpen("/data.txt", FILE_READ);
  check(f.available() == 18 && f.peek() == 'l', "available and peek");
  check(f.readStringUntil('\n') == "line one", "readStringUntil consumes the terminator");
  check(f.position() == 9, "position after readStringUntil");
  char buf[4] = {};
  check(f.readBytes(buf, 4) == 4 && std::memcmp(buf, "line", 4) == 0, "readBytes");
  check(f.seek(0) && f.position() == 0, "seek to start");
  check(f.seek(4, SeekCur) && f.position() == 4, "SeekCur");
  check(f.seek(0, SeekEnd) && f.position() == 18 && f.available() == 0 && f.read() == -1, "SeekEnd and EOF");
  check(!f.seek(19), "seek past end fails");
  f.seek(9);
  check(f.readString() == "line two\n", "readString reads to EOF");
  check(f.write(reinterpret_cast<const uint8_t*>("x"), 1) == 0, "read handle refuses writes");
  check(f.name() != nullptr && std::string(f.name()) == "data.txt" && std::string(f.path()) == "/data.txt", "name and path");
  f.close();
  File p = unguardedOpen("/fmt.txt", "w");
  check(p.printf("%s=%d", "n", 42) == 4 && p.print('c') == 1 && p.print(7) == 1 && p.println(8) == 3, "printf and numeric prints");
  std::string big(300, 'q');
  check(p.printf("%s", big.c_str()) == 300, "printf beyond the stack buffer");
  p.close();
  check(fs.text("/fmt.txt") == "n=42c78\r\n" + big, "formatted bytes stored");
  check(fs.count("write") >= 5 && fs.count("read") >= 1, "file reads and writes are logged");
}

static void directories() {
  auto& fs = reset();
  check(!unguardedMkdir("/a/b"), "mkdir needs its parent");
  check(unguardedMkdir("/a") && unguardedMkdir("/a/b") && !unguardedMkdir("/a"), "mkdir chain; existing refused");
  fs.put("/a/file", "1");
  check(!unguardedMkdir("/a/file"), "mkdir over a file refused");
  check(!unguardedMkdir("/") && !unguardedMkdir("/sd"), "roots cannot be made");
  check(!unguardedRmdir("/a"), "rmdir of a non-empty directory refused");
  check(!unguardedRemove("/a/b"), "remove refuses a directory by default");
  check(unguardedRmdir("/a/b") && !fs.exists("/a/b"), "rmdir of an empty directory");
  check(!unguardedRmdir("/a/file"), "rmdir of a file refused");
  unguardedMkdir("/a/c");
  fs.policy.removeUnlinksDirectory = true;
  check(unguardedRemove("/a/c") && !fs.exists("/a/c"), "policy lets remove unlink an empty directory");
  fs.put("/a/c/x", "1");
  check(!unguardedRemove("/a/c"), "even then a non-empty directory stays");
  fs.policy.removeUnlinksDirectory = false;
  check(unguardedRemove("/a/file") && !fs.exists("/a/file"), "remove unlinks a file");
  check(!unguardedRemove("/a/file"), "removing it again fails");
  check(!unguardedRemove("/") && !unguardedRemove("/sd"), "roots cannot be removed");

  // Listing, as VFS::DirectoryReader drives it.
  fs.put("/sd/VIDEOS/a.avi", "aaaa");
  fs.put("/sd/VIDEOS/b.avi", "bb");
  fs.mkdirAll("/sd/VIDEOS/sub");
  fs.now = 1234;
  fs.put("/sd/VIDEOS/c.avi", "c");
  File dir = unguardedOpen("/sd/VIDEOS", "r");
  std::vector<std::string> names;
  std::vector<size_t> sizes;
  for (File child = dir.openNextFile(); child; child = dir.openNextFile()) {
    names.emplace_back(child.name());
    sizes.push_back(child.isDirectory() ? 0 : child.size());
    if (std::string(child.name()) == "c.avi") check(child.getLastWrite() == 1234, "modified time follows fs.now");
    child.close();
  }
  check(names == std::vector<std::string>{"a.avi", "b.avi", "c.avi", "sub"} &&
        sizes == std::vector<size_t>{4, 2, 1, 0}, "leaf names in order with sizes");
  check(!dir.openNextFile(), "exhausted listing stays empty");
  dir.rewindDirectory();
  fs.policy.nameStyle = fake_vfs::NameStyle::BackendRooted;
  File first = dir.openNextFile();
  check(std::string(first.name()) == "/VIDEOS/a.avi", "BackendRooted names strip /sd like the FAT backend");
  first.close();
  dir.close();
  File littlefsDir = unguardedOpen("/a", "r");
  File onlyChild = littlefsDir.openNextFile();
  check(std::string(onlyChild.name()) == "/a/c", "BackendRooted on LittleFS is the full path");
  onlyChild.close();
  littlefsDir.close();
  fs.policy.nameStyle = fake_vfs::NameStyle::Leaf;
  File plain = unguardedOpen("/sd/VIDEOS/a.avi", "r");
  check(!plain.openNextFile(), "openNextFile on a file yields nothing");
  plain.close();
  check(fs.liveHandles() == 0, "listing closed every child");
}

static void renames() {
  auto& fs = reset();
  fs.put("/one", "1");
  fs.put("/two", "2");
  check(!unguardedRename("/missing", "/x"), "rename of a missing source fails");
  check(unguardedRename("/one", "/two") && fs.text("/two") == "1" && !fs.exists("/one"), "LittleFS renames over an existing file");
  fs.put("/sd/a", "a");
  fs.put("/sd/b", "b");
  check(!unguardedRename("/sd/a", "/sd/b") && fs.text("/sd/b") == "b" && fs.exists("/sd/a"), "SD refuses rename over an existing file");
  fs.policy.sdRenameOverExisting = true;
  check(unguardedRename("/sd/a", "/sd/b") && fs.text("/sd/b") == "a", "policy lets SD replace");
  fs.policy.littlefsRenameOverExisting = false;
  fs.put("/p", "p"); fs.put("/q", "q");
  check(!unguardedRename("/p", "/q"), "policy makes LittleFS refuse replacement");
  check(!unguardedRename("/p", "/sd/p"), "cross-backend rename fails");
  check(!unguardedRename("/p", "/nodir/p"), "destination parent must exist");
  check(!unguardedRename("/p", "/p"), "rename onto itself fails");
  fs.mkdirAll("/dir/inner");
  fs.put("/dir/inner/leaf", "L");
  check(!unguardedRename("/dir", "/dir/inner/x"), "rename into own subtree fails");
  check(!unguardedRename("/dir", "/two"), "directory over a file fails");
  check(unguardedRename("/dir", "/moved") && fs.text("/moved/inner/leaf") == "L" && !fs.exists("/dir/inner/leaf"), "directory rename moves the subtree");
  check(!unguardedRename("/two", "/moved"), "file over a directory fails");
  check(!unguardedRename("/", "/x") && !unguardedRename("/two", "/sd"), "roots are not renamed");
  check(fs.freeCacheInvalidations > 0, "rename invalidates the free-space cache");
}

static void nameLimit() {
  auto& fs = reset();
  const std::string ok(63, 'n'), tooLong(64, 'n');
  check(static_cast<bool>(unguardedOpen(("/" + tooLong).c_str(), "w")), "no limit by default");
  fs.reset(); fake_vfs::wireLockDouble();
  fs.policy.littlefsNameLimit = 63;
  check(static_cast<bool>(unguardedOpen(("/" + ok).c_str(), "w")), "63-byte name accepted");
  check(!unguardedOpen(("/" + tooLong).c_str(), "w"), "64-byte name refused on LittleFS");
  check(!unguardedMkdir(("/" + tooLong).c_str()), "mkdir follows the limit");
  check(!unguardedRename(("/" + ok).c_str(), ("/" + tooLong).c_str()), "rename destination follows the limit");
  check(static_cast<bool>(unguardedOpen(("/sd/" + tooLong).c_str(), "w")), "SD has no such limit");
}

static void faults() {
  auto& fs = reset();
  fs.put("/f", "old");
  fs.faults.open.add("/f");
  check(!unguardedOpen("/f", "r") && !unguardedOpen("/f", "w") && fs.text("/f") == "old", "open fault refuses both modes and keeps the file");
  fs.faults.open.clear();
  fs.faults.shortWriteAfter["/g"] = 4;
  File g = unguardedOpen("/g", "w");
  check(g.print("abcdef") == 4 && g.print("gh") == 0 && g.size() == 4 && !g.getWriteError(), "short write caps total bytes");
  g.close();
  check(fs.text("/g") == "abcd", "short-written bytes stored");
  fs.faults.writeError.add("/h");
  File h = unguardedOpen("/h", "w");
  check(!h.getWriteError(), "no error before a write");
  h.print("x");
  check(h.getWriteError() == 1 && h.size() == 1, "write error flag after a write");
  h.clearWriteError();
  check(!h.getWriteError(), "clearWriteError");
  h.close();
  fs.faults.flush.add("/i");
  File i = unguardedOpen("/i", "w");
  i.print("x");
  check(!i.getWriteError(), "flush fault waits for flush");
  i.flush();
  check(i.getWriteError() == 1 && !fs.last("flush")->ok, "flush fault sets the write error and logs failure");
  i.close();
  fs.put("/r1", "1");
  fs.faults.rename.add("/r1");
  check(!unguardedRename("/r1", "/r2") && fs.exists("/r1") && !fs.exists("/r2"), "rename fault keyed by source");
  fs.faults.remove.add("/r1");
  check(!unguardedRemove("/r1") && fs.exists("/r1"), "remove fault keeps the file");
  fs.faults.mkdir.add("/m");
  check(!unguardedMkdir("/m") && !fs.exists("/m"), "mkdir fault");
  fs.mkdirAll("/d");
  fs.faults.rmdir.add("/d");
  check(!unguardedRmdir("/d") && fs.exists("/d"), "rmdir fault");
  fs.faults.stat["/s"] = EACCES;
  fs.put("/s", "s");
  struct FakeVfsStat info;
  errno = 0;
  check(FakeVfsStat("/littlefs/s", &info) == -1 && errno == EACCES, "stat fault sets errno");
  fs.faults.clear();
  fs.faults.open.add("*");
  check(!unguardedOpen("/s", "r") && !unguardedOpen("/sd/anything", "w", true), "wildcard matches every path");
  fs.faults.clear();
  check(static_cast<bool>(unguardedOpen("/s", "r")), "faults.clear restores opens");
}

static void statSeam() {
  auto& fs = reset();
  fs.put("/system/users/user_settings/1.json", "{}");
  fs.mkdirAll("/sd/VIDEOS");
  struct FakeVfsStat info;
  check(FakeVfsStat("/littlefs/system/users/user_settings/1.json", &info) == 0 && info.st_size == 2 && !info.directory, "physical LittleFS path");
  check(FakeVfsStat("/system/users/user_settings/1.json", &info) == 0, "plain VFS path");
  check(FakeVfsStat("/sd/VIDEOS", &info) == 0 && info.directory, "SD directory");
  errno = 0;
  check(FakeVfsStat("/littlefs/system/users/user_settings/2.json", nullptr) == -1 && errno == ENOENT, "missing file is ENOENT");
  check(fs.count("stat") == 4 && !fs.last("stat")->ok, "stat calls are logged");
  fs.sdAvailable() = false;
  errno = 0;
  check(FakeVfsStat("/sd/VIDEOS", &info) == -1 && errno == EIO, "unavailable SD stats as EIO");
}

static void availability() {
  auto& fs = reset();
  fs.put("/sd/x", "x");
  fs.put("/y", "y");
  check(VFS::isSDAvailable() && VFS::isSDWritable() && VFS::isLittleFSReady(), "both tiers up by default");
  fs.sdAvailable() = false;
  check(!VFS::isSDAvailable() && !VFS::isSDWritable(), "SD reported unavailable");
  check(!unguardedExists("/sd/x") && !unguardedOpen("/sd/x", "r") && !unguardedMkdir("/sd/d") && !unguardedRemove("/sd/x") &&
        !unguardedRename("/sd/x", "/sd/z"), "every /sd operation fails");
  check(unguardedExists("/y"), "LittleFS unaffected");
  fs.sdAvailable() = true;
  fs.capacity[1].writable = false;
  check(VFS::isSDAvailable() && !VFS::isSDWritable() && unguardedExists("/sd/x"), "read-only SD stays mounted");
  fs.littlefsReady() = false;
  check(!VFS::isLittleFSReady() && !unguardedExists("/y") && !unguardedOpen("/y", "r"), "LittleFS not ready fails its operations");
  fs.littlefsReady() = true;
  VFS::noteSDWriteFailure("probe");
  VFS::noteLittleFsBytesWritten(10);
  VFS::invalidateLittleFsFreeCache();
  check(fs.sdWriteFailureNotes == 1 && fs.littlefsBytesNoted == 10 && fs.freeCacheInvalidations == 1, "hint forwarders count");
}

static void stats() {
  auto& fs = reset();
  uint64_t total = 0, used = 0, free = 0;
  fs.put("/a", std::string(100, 'a'));
  fs.put("/sd/b", std::string(50, 'b'));
  check(VFS::getStats(VFS::INTERNAL, total, used, free) && total == 4u * 1024u * 1024u && used == 100 && free == total - 100, "LittleFS stats sum file bytes");
  fs.capacity[1].total = 1000; fs.capacity[1].used = 900;
  check(VFS::getStats(VFS::SDCARD, total, used, free) && total == 1000 && used == 950 && free == 50, "SD stats add the configured baseline");
  fs.capacity[1].statsFail = true;
  check(!VFS::getStats(VFS::SDCARD, total, used, free) && total == 1000, "stats fault leaves outputs unchanged");
  fs.capacity[1].statsFail = false;
  fs.capacity[1].total = 0;
  check(!VFS::getStats(VFS::SDCARD, total, used, free), "zero capacity is a failure");
  fs.capacity[0].available = false;
  check(!VFS::getStats(VFS::AUTO, total, used, free), "AUTO follows INTERNAL and its availability");
}

static void lockSeam() {
  auto& fs = reset();
  fs.put("/x", "x");
  unguardedExists("/x");
  check(fs.unlockedCalls() == 1 && fs.lockedCalls() == 0 && fs.unguardedCalls() == 1, "raw call outside the lock");
  {
    FsLockGuard guard("test");
    check(guard.held && isFsLockedByCurrentTask(), "real FsLockGuard takes the double");
    const AuthContext ctx = VFS::systemAuth("users.save");
    VFS::existsGuarded("/x", ctx);
    {
      FsLockGuard nested("nested");
      check(!nested.held && gFakeFsSemaphore.recursiveTakes == 0, "nested guard re-enters without taking");
    }
    check(isFsLockedByCurrentTask(), "inner guard did not release the outer lock");
    File f = VFS::openGuarded("/x", "r", ctx);
    f.read();
    f.close();
  }
  check(!isFsLockedByCurrentTask() && gFakeFsSemaphore.takes == 1 && gFakeFsSemaphore.gives == 1, "guard released once");
  check(fs.lockedCalls() == 4 && fs.guardedCalls() == 2 && fs.unlockedCalls() == 1, "locked and guarded counters per call");
  const fake_vfs::Call* guarded = fs.last("exists");
  check(guarded && guarded->guarded && guarded->locked && guarded->authUser == "system" &&
        guarded->authReason == "system:users.save" && guarded->authScope.empty(), "auth identity recorded");
  const AuthContext scoped = VFS::systemAuth("/sd/VIDEOS", "video.save");
  VFS::existsGuarded("/sd/VIDEOS/clip", scoped);
  check(fs.last("exists")->authScope == "/sd/VIDEOS" && fs.last("exists")->authReason == "system:video.save", "scoped systemAuth recorded");
  unguardedExists("/x");
  check(!fs.last("exists")->guarded && fs.last("exists")->authUser.empty(), "raw calls carry no identity");
  bool heldDuringProbe = false;
  fs.lockHeld = [&] { heldDuringProbe = true; return true; };
  unguardedExists("/x");
  check(heldDuringProbe && fs.last("exists")->locked, "custom lockHeld predicate is consulted");
  fake_vfs::wireLockDouble();
  xSemaphoreTake(gFsMutex, portMAX_DELAY);
  check(xSemaphoreTake(gFsMutex, 0) == 0 && gFakeFsSemaphore.recursiveTakes == 1, "a second take of the held double fails and counts");
  xSemaphoreGive(gFsMutex);
}

static void replaceFile() {
  auto& fs = reset();
  const AuthContext ctx = VFS::systemAuth("replace");
  fs.put("/dest", "old"); fs.put("/dest.tmp", "new");
  check(VFS::replaceFileGuarded("/dest.tmp", "/dest", ctx) && fs.text("/dest") == "new" && !fs.exists("/dest.tmp"), "LittleFS replaces directly");
  check(fs.count("rename") == 1 && fs.count("replace") == 1, "one rename behind the replace");
  fs.log.clear();
  fs.put("/sd/d", "old"); fs.put("/sd/d.tmp", "new");
  check(VFS::replaceFileGuarded("/sd/d.tmp", "/sd/d", ctx) && fs.text("/sd/d") == "new" && !fs.exists("/sd/d.bak") && !fs.exists("/sd/d.tmp"), "SD replaces through .bak");
  check(fs.ops() == std::vector<std::string>{"replace", "exists", "exists", "rename", "rename", "remove"}, "SD replace call sequence");
  fs.put("/sd/e", "old"); fs.put("/sd/e.tmp", "new"); fs.put("/sd/e.bak", "stale");
  check(!VFS::replaceFileGuarded("/sd/e.tmp", "/sd/e", ctx) && fs.text("/sd/e") == "old" && fs.exists("/sd/e.tmp"), "an existing .bak blocks SD replacement");
  fs.put("/sd/f", "old"); fs.put("/sd/f.tmp", "new");
  fs.faults.rename.add("/sd/f.tmp");
  check(!VFS::replaceFileGuarded("/sd/f.tmp", "/sd/f", ctx) && fs.text("/sd/f") == "old" && !fs.exists("/sd/f.bak") && fs.text("/sd/f.tmp") == "new", "failed promotion restores the destination");
  fs.faults.clear();
  check(!VFS::replaceFileGuarded("/sd/f.tmp", "/f", ctx), "cross-backend replace refused");
  check(!VFS::replaceFileGuarded("/sd/f.tmp", "/sd/f.tmp", ctx), "replace onto itself refused");
  fs.put("/sd/g.tmp", "new");
  check(VFS::replaceFileGuarded("/sd/g.tmp", "/sd/g", ctx) && fs.text("/sd/g") == "new", "SD without a destination renames directly");
  fs.mkdirAll("/sd/dir"); fs.put("/sd/dir.tmp", "x");
  check(!VFS::replaceFileGuarded("/sd/dir.tmp", "/sd/dir", ctx), "a directory destination is refused");
  check(!VFS::replaceFileGuarded("/sd/../x.tmp", "/sd/x", ctx), "traversal refused");
  check(!fs.last("replace")->ok && fs.last("replace")->guarded && fs.last("replace")->authReason == "system:replace", "replace logged with identity");
}

static void productionWriters() {
  auto& fs = reset();
  fs.mkdirAll("/system");
  fs.put("/system/users.json", "[old]");
  check(writeTextAtomic("/system/users.json", String("[new]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]" &&
        !fs.exists("/system/users.json.tmp"), "writeTextAtomic promotes a complete temporary");
  check(fs.unlockedCalls() == 0 && fs.count("rename") == 1 && fs.count("remove") == 0, "every call under the lock; no cleanup after success");
  check(!isFsLockedByCurrentTask() && pauseDepth == 0, "lock and poll pause released");
  fs.log.clear();
  fs.faults.shortWriteAfter["/system/users.json.tmp"] = 2;
  check(!writeTextAtomic("/system/users.json", String("[newer]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]" &&
        !fs.exists("/system/users.json.tmp"), "short write keeps the previous file and removes the temp");
  check(fs.count("rename") == 0 && fs.count("remove") == 1, "no promotion of an incomplete temporary");
  fs.faults.clear();
  fs.faults.flush.add("/system/users.json.tmp");
  check(!writeTextAtomic("/system/users.json", String("[flush]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]", "flush fault is a reported write error");
  fs.faults.clear();
  fs.faults.writeError.add("/system/users.json.tmp");
  check(!writeTextAtomic("/system/users.json", String("[err]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]", "write error flag refuses promotion");
  fs.faults.clear();
  fs.faults.rename.add("/system/users.json.tmp");
  check(!writeTextAtomic("/system/users.json", String("[ren]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]" &&
        !fs.exists("/system/users.json.tmp"), "failed rename keeps the destination, temp cleaned");
  fs.faults.clear();
  fs.faults.open.add("/system/users.json.tmp");
  check(!writeTextAtomic("/system/users.json", String("[open]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]", "open fault");
  fs.faults.clear();
  fs.mkdirAll("/system/users.json.tmp");
  check(!writeTextAtomic("/system/users.json", String("[dir]"), VFS::systemAuth("self-test")) && fs.text("/system/users.json") == "[new]", "a directory in the temporary's place");
  check(writeText("/deep/parent/x", String("y"), VFS::systemAuth("self-test")) && fs.text("/deep/parent/x") == "y", "writeText creates parents via create=true");
  check(fs.unlockedCalls() == 0 && fs.liveHandles() == 0 && pauseDepth == 0, "no unlocked call, no leaked handle");
}

static void callLog() {
  auto& fs = reset();
  fs.put("/a", "1");
  unguardedExists("/a"); unguardedExists("/b"); unguardedRemove("/a");
  check(fs.count("exists") == 2 && fs.count("exists", "/b") == 1 && fs.count("exists", "b") == 1, "count by op and normalised path");
  check(fs.last("exists")->path == "/b" && !fs.last("exists")->ok && fs.last("remove")->ok, "last() finds the newest entry");
  check(fs.last("rename") == nullptr, "absent op yields null");
  check(fs.ops() == std::vector<std::string>{"exists", "exists", "remove"}, "ops in order");
  check(fs.allPaths() == std::vector<std::string>{"/", "/sd"}, "allPaths after removal");
  fs.reset();
  check(fs.log.empty() && fs.allPaths().size() == 2 && fs.faults.open.empty(), "reset clears log, tree and faults");
}

int main() {
  struct Case { const char* name; void (*run)(); } cases[] = {
      {"roots-and-helpers", rootsAndHelpers}, {"open-modes", openModes}, {"stream-surface", streamSurface},
      {"directories", directories}, {"renames", renames}, {"name-limit", nameLimit}, {"faults", faults},
      {"stat-seam", statSeam}, {"availability", availability}, {"stats", stats}, {"lock-seam", lockSeam},
      {"replace-file", replaceFile}, {"production-writers", productionWriters}, {"call-log", callLog}};
  for (const Case& c : cases) {
    try {
      c.run();
    } catch (const std::exception& error) {
      std::fprintf(stderr, "fake_vfs self-test failed in %s: %s\n", c.name, error.what());
      return 1;
    }
  }
  std::printf("fake_vfs self-test: %u checks across %zu cases\n", checks, sizeof(cases) / sizeof(cases[0]));
  return 0;
}
