#include "QuickNote_Store.h"
#if ENABLE_QUICKNOTE
#include "System_Clock.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include "Transcription_UI_Policy.h"
#include <esp_timer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace QuickNotePolicy;

extern uint32_t gBootCounter;  // HardwareOne.cpp: the NVS boot counter, "boot #N"

namespace {
constexpr uint32_t kLockMs = 1000;
// One listing slot per kept file name; names are at most kFileNameMax bytes.
constexpr size_t kNameSlot = 40;
static_assert(kNameSlot > kFileNameMax, "a note file name must fit a listing slot");
static_assert(QuickNoteStore::kMaxNames <= UINT16_MAX, "listing slots are indexed by uint16_t");

bool fail(const char** why, const char* reason) {
  if (why) *why = reason;
  return false;
}

// Clears a buffer that held note text on every exit (a header carries the
// title, a list read the start of the text).
struct Wipe {
  char* data;
  size_t size;
  ~Wipe() { TranscriptionUI::clear(data, size); }
};

struct PsBlock {
  void* data = nullptr;
  ~PsBlock() { if (data) ps_free(data); }
};

const char* baseName(const char* name) {
  const char* slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}

// Min-heap of slot indices on name order: the root is the oldest name kept,
// replaced whenever a newer one turns up after the window is full. Names sort
// by their UTC stamp, so this keeps the newest kMaxNames whatever order the
// directory yields them in.
using NameSlot = char[kNameSlot];
bool older(NameSlot* names, uint16_t a, uint16_t b) {
  return strcmp(names[a], names[b]) < 0;
}
void heapUp(NameSlot* names, uint16_t* heap, size_t i) {
  while (i) {
    const size_t parent = (i - 1) / 2;
    if (!older(names, heap[i], heap[parent])) return;
    std::swap(heap[i], heap[parent]);
    i = parent;
  }
}
void heapDown(NameSlot* names, uint16_t* heap, size_t count, size_t i) {
  for (;;) {
    const size_t left = 2 * i + 1, right = left + 1;
    size_t least = i;
    if (left < count && older(names, heap[left], heap[least])) least = left;
    if (right < count && older(names, heap[right], heap[least])) least = right;
    if (least == i) return;
    std::swap(heap[i], heap[least]);
    i = least;
  }
}

// Fills row.header from the file's own header when it parses and agrees with
// the name; otherwise the row keeps the name's id/created only.
void readRowHeader(QuickNoteStore::Row& row, const AuthContext& ctx) {
  File file = VFS::openGuarded(row.path, "r", ctx);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return;
  }
  char head[kHeaderMax];
  Wipe wipe{head, sizeof(head)};
  const size_t wanted = std::min<size_t>(file.size(), sizeof(head));
  const size_t got = wanted ? file.read(reinterpret_cast<uint8_t*>(head), wanted) : 0;
  file.close();
  NoteHeader parsed;
  if (got == wanted && noteParseHeader(head, got, &parsed, nullptr) &&
      parsed.id == row.header.id && !strcmp(parsed.created, row.header.created)) {
    row.header = parsed;
    row.headerValid = true;
  }
}

// A user-supplied path as the guarded VFS will act on it, if it names a note.
bool notePath(const char* path, String& normalized) {
  return path && normalizeFsPath(path, normalized) &&
         notePathParse(normalized.c_str(), nullptr, nullptr, nullptr, nullptr);
}

// The named owner's guarded context, confined to `scope` (never the trusted
// one): every note folder and file access but the transcript root's creation.
__attribute__((noinline)) AuthContext ownerContext(CommandSource source, const char* user,
                                                   const char* scope) {
  AuthContext ctx{};
  ctx.transport = source;
  ctx.user = user;
  ctx.scope = scope;
  return ctx;
}

// The write every note file takes (create, restamp): `head`, then the text
// (`text`, or the rest of `source` copied through `head` once that is
// written) into `tmp`, renamed to `path` only when complete, and kept only if
// its length checks out. On failure neither file is left. `head` holds
// kHeaderMax bytes; `bytes` is the whole file.
bool writeNote(const AuthContext& ctx, bool sd, const String& tmp, const String& path,
               char* head, size_t headLength, const char* text, File* source, size_t bytes,
               const char** why) {
  File file = VFS::openGuarded(tmp, "w", ctx, true);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return fail(why, "Cannot open note file");
  }
  size_t written = file.write(reinterpret_cast<const uint8_t*>(head), headLength);
  if (written == headLength && text)
    written += file.write(reinterpret_cast<const uint8_t*>(text), bytes - headLength);
  while (source && written >= headLength && written < bytes) {
    const size_t n = std::min(kHeaderMax, bytes - written);
    if (source->read(reinterpret_cast<uint8_t*>(head), n) != n) break;
    const size_t put = file.write(reinterpret_cast<const uint8_t*>(head), n);
    written += put;
    if (put != n) break;
  }
  file.flush();
  const bool ok = written == bytes && !file.getWriteError() && file.size() == bytes;
  file.close();
  if (sd && !ok) VFS::noteSDWriteFailure("note write");
  if (!sd) VFS::noteLittleFsBytesWritten(written);
  if (!ok) {
    VFS::removeGuarded(tmp, ctx);
    return fail(why, "Note write incomplete");
  }
  if (!VFS::renameGuarded(tmp, path, ctx)) {
    VFS::removeGuarded(tmp, ctx);
    return fail(why, "Cannot save note");
  }
  File check = VFS::openGuarded(path, "r", ctx);
  const bool verified = check && !check.isDirectory() && check.size() == bytes;
  if (check) check.close();
  if (!verified) {
    VFS::removeGuarded(path, ctx);
    return fail(why, "Note verification failed");
  }
  return true;
}

// "<root>/<name>/notes" in `out` when `name` is an account folder's one
// canonical spelling, "u<id>" (TranscriptPathPolicy, as noteFolder writes it).
bool accountNotesFolder(bool sd, const char* name, char* out, size_t cap) {
  char owner[32];
  const int n = snprintf(owner, sizeof(owner), "%s/%s", sd ? "/sd/stt" : "/stt", name);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(owner)) return false;
  const TranscriptPathPolicy::Path where = TranscriptPathPolicy::classify(owner);
  const size_t length = where.kind == TranscriptPathPolicy::Kind::Owner
      ? noteFolder(sd, where.owner, out, cap) : 0;
  return length > static_cast<size_t>(n) && !strncmp(out, owner, n) && out[n] == '/';
}

// Removes `folder`'s in-progress note files, adding them to *removed; every
// other entry is left alone and no file is read. False if one could not be
// removed. The caller holds the lock.
bool sweepFolder(const char* folder, const AuthContext& ctx, uint32_t* removed) {
  if (!VFS::existsGuarded(folder, ctx)) return true;  // no notes in this account
  File dir = VFS::openGuarded(folder, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  bool ok = true;
  for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    char path[kPathMax];
    path[0] = '\0';
    const char* name = baseName(file.name());
    const bool orphan = !file.isDirectory() && noteFileNameParse(name, nullptr, nullptr, true) &&
        snprintf(path, sizeof(path), "%s/%s", folder, name) < static_cast<int>(sizeof(path));
    file.close();  // closed before its removal
    if (!orphan) continue;
    if (VFS::removeGuarded(path, ctx)) ++*removed;
    else ok = false;
  }
  dir.close();
  return ok;
}
}  // namespace

namespace QuickNoteStore {

bool stampAt(time_t t, char* out, size_t cap) {
  if (!out || !cap) return false;
  struct tm utc{};
  if (Clock::isPlausibleEpoch(t) && gmtime_r(&t, &utc) &&
      strftime(out, cap, "%Y%m%dT%H%M%SZ", &utc) == kStampMax) return true;
  out[0] = '\0';
  return false;
}

void stamp(char* out, size_t cap) {
  if (!out || !cap) return;
  if (!Clock::isSynced() || !stampAt(Clock::epochSeconds(), out, cap)) snprintf(out, cap, "boot");
}

void bootClock(uint32_t* boot, uint32_t* uptimeS) {
  if (boot) *boot = gBootCounter;
  if (uptimeS) *uptimeS = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

bool stampSince(uint32_t uptimeS, char* out, size_t cap) {
  static int64_t bootEpochMs = 0;  // wall-clock ms at this boot's uptime 0
  if (out && cap) out[0] = '\0';
  if (!Clock::isSynced()) return false;
  if (!bootEpochMs) bootEpochMs = Clock::epochMillis() - esp_timer_get_time() / 1000;
  return stampAt(static_cast<time_t>((bootEpochMs + int64_t(uptimeS) * 1000) / 1000), out, cap);
}

bool hasRoom(const char** why) {
  if (why) *why = nullptr;
  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(why, "Note storage busy");
  const bool sd = VFS::isSDWritable();
  uint64_t total = 0, used = 0, free = 0;
  if (!VFS::getStats(sd ? VFS::SDCARD : VFS::INTERNAL, total, used, free) ||
      free < kReserveBytes + kHeaderMax + kMaxNoteBytes + 256)
    return fail(why, "Note storage full or unavailable");
  return true;
}

bool create(CommandSource source, TransportSessionEpoch epoch, const char* user,
            NoteHeader& header, const char* text, size_t length,
            char* pathOut, size_t pathCap, const char** why) {
  if (why) *why = nullptr;
  if (pathOut && pathCap) pathOut[0] = '\0';
  if (!user || !*user || !epoch || !header.id || !text || !length ||
      length > kMaxNoteBytes || memchr(text, '\0', length))
    return fail(why, "Invalid note");
  if (!header.created[0]) {
    stamp(header.created, sizeof(header.created));
    bootClock(&header.boot, &header.uptimeS);
  }
  header.words = noteWordCount(text, length);
  noteTitle(text, length, header.title, sizeof(header.title));
  char head[kHeaderMax];
  Wipe wipe{head, sizeof(head)};
  const size_t headLength = noteFormatHeader(header, head, sizeof(head));
  char name[kNameSlot], tmpName[kNameSlot];
  if (!headLength || !noteFileName(header.created, header.id, false, name, sizeof(name)) ||
      !noteFileName(header.created, header.id, true, tmpName, sizeof(tmpName)))
    return fail(why, "Invalid note");
  const size_t bytes = headLength + length;

  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(why, "Note storage busy");
  // The account is bound now, under the lock, by its persistent ID: a
  // username deleted and recreated mid-note cannot inherit the folder.
  if (!transportSessionEpochIsLive(source, epoch))
    return fail(why, "Note session expired before saving");
  uint32_t account = 0;
  if (!getUserIdByUsername(user, account) || !account) return fail(why, "Note account unavailable");
  if (!transportSessionEpochIsLive(source, epoch))
    return fail(why, "Note session expired before saving");
  const bool sd = VFS::isSDWritable();
  const char* root = sd ? "/sd/stt" : "/stt";
  char owner[32], folder[kPathMax], path[kPathMax], tmp[kPathMax];
  snprintf(owner, sizeof(owner), "%s/u%lu", root, static_cast<unsigned long>(account));
  if (!noteFolder(sd, account, folder, sizeof(folder)) ||
      snprintf(path, sizeof(path), "%s/%s", folder, name) >= static_cast<int>(sizeof(path)) ||
      snprintf(tmp, sizeof(tmp), "%s/%s", folder, tmpName) >= static_cast<int>(sizeof(tmp)))
    return fail(why, "Invalid note");
  const AuthContext ctx = ownerContext(source, user, owner);
  uint64_t total = 0, used = 0, free = 0;
  // Fresh capacity, no tier switching and no automatic deletion/rotation.
  if (!VFS::getStats(sd ? VFS::SDCARD : VFS::INTERNAL, total, used, free) ||
      free < kReserveBytes + bytes + 256)
    return fail(why, "Note storage full or unavailable");
  // trusted: only create the protected transcript root; the owner and notes
  // folders and the file below always use the named owner's guarded context.
  if (!VFS::exists(root) && !VFS::mkdirGuarded(root, VFS::systemAuth(root, "note root")))
    return fail(why, "Cannot create notes folder");
  if ((!VFS::existsGuarded(owner, ctx) && !VFS::mkdirGuarded(owner, ctx)) ||
      (!VFS::existsGuarded(folder, ctx) && !VFS::mkdirGuarded(folder, ctx)))
    return fail(why, "Cannot create notes folder");
  if (VFS::exists(path)) return fail(why, "Note filename already exists");
  if (!writeNote(ctx, sd, tmp, path, head, headLength, text, nullptr, bytes, why)) return false;
  if (pathOut && pathCap) snprintf(pathOut, pathCap, "%s", path);
  return true;
}

bool restamp(CommandSource source, const char* user, const char* path, const char* stamp,
             char* pathOut, size_t pathCap, bool* retry, const char** why) {
  if (why) *why = nullptr;
  *retry = false;
  if (pathOut && pathCap) pathOut[0] = '\0';
  uint32_t owner = 0;
  bool sd = false;
  char created[kStampMax + 1] = {};
  uint64_t id = 0;
  char folder[kPathMax], name[kNameSlot], tmpName[kNameSlot], target[kPathMax], tmp[kPathMax];
  // `stamp` is a real one (stampSince): noteFileName checks its form.
  if (!user || !*user || !path || !notePathParse(path, &owner, &sd, created, &id) ||
      strcmp(created, "boot") || !stamp || strnlen(stamp, kStampMax + 1) != kStampMax ||
      !noteFolder(sd, owner, folder, sizeof(folder)) ||
      !noteFileName(stamp, id, false, name, sizeof(name)) ||
      !noteFileName(stamp, id, true, tmpName, sizeof(tmpName)) ||
      snprintf(target, sizeof(target), "%s/%s", folder, name) >= static_cast<int>(sizeof(target)) ||
      snprintf(tmp, sizeof(tmp), "%s/%s", folder, tmpName) >= static_cast<int>(sizeof(tmp)))
    return fail(why, "Not a boot note");  // the last two cannot happen: kPathMax
  const String boot(path), copy(target);

  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) {
    *retry = true;
    return fail(why, "Note storage busy");
  }
  // The folder's account, still under this name (never a recreated one).
  uint32_t account = 0;
  if (!getUserIdByUsername(user, account) || account != owner)
    return fail(why, "Note account unavailable");
  // The owner, confined to this one notes folder.
  const AuthContext ctx = ownerContext(source, user, folder);
  File file = VFS::openGuarded(boot, "r", ctx);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return fail(why, "Note unavailable");
  }
  // The header comes out of the old file; the new one differs only in Created.
  char head[kHeaderMax];
  Wipe wipe{head, sizeof(head)};
  const size_t size = file.size();
  const size_t wanted = std::min(size, sizeof(head));
  NoteHeader header;
  size_t body = 0;
  const bool read = wanted && file.read(reinterpret_cast<uint8_t*>(head), wanted) == wanted;
  const bool parsed = read && noteParseHeader(head, wanted, &header, &body) && header.id == id &&
      !strcmp(header.created, "boot") && size - body <= kMaxNoteBytes;
  memcpy(header.created, stamp, kStampMax + 1);
  const size_t headLength = parsed ? noteFormatHeader(header, head, sizeof(head)) : 0;
  TranscriptionUI::clear(header.title, sizeof(header.title));
  const size_t bytes = headLength + (size - body);
  bool ok = headLength != 0;
  if (!ok) {
    *retry = !read;
    fail(why, read ? "Note header unreadable" : "Note read incomplete");
  } else if (!VFS::exists(copy)) {
    uint64_t total = 0, used = 0, free = 0;
    ok = VFS::getStats(sd ? VFS::SDCARD : VFS::INTERNAL, total, used, free) &&
         free >= kReserveBytes + bytes + 256;
    if (!ok) fail(why, "Note storage full or unavailable");
    else if (!(ok = file.seek(body))) fail(why, "Note read incomplete");
    else ok = writeNote(ctx, sd, tmp, copy, head, headLength, nullptr, &file, bytes, why);
    *retry = !ok;  // the boot file is untouched: a later pass may succeed
  } else {
    // An earlier pass renamed its copy into place but could not delete the
    // boot file: keep that copy, but never take over another file of the name.
    File done = VFS::openGuarded(copy, "r", ctx);
    ok = done && !done.isDirectory() && done.size() == bytes;
    if (done) done.close();
    if (!ok) fail(why, "Note filename already exists");
  }
  file.close();
  if (!ok) return false;
  if (!VFS::removeGuarded(boot, ctx)) {
    *retry = true;
    return fail(why, "Cannot remove boot note");
  }
  if (pathOut && pathCap) snprintf(pathOut, pathCap, "%s", target);
  return true;
}

bool list(const AuthContext& ctx, uint32_t owner, bool sd, uint32_t offset,
          Page* out, const char** why) {
  if (why) *why = nullptr;
  if (!out) return fail(why, "Invalid request");
  out->count = 0;
  out->total = 0;
  out->nextOffset = offset;
  out->more = out->capped = false;
  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(why, "Note storage busy");
  if (!owner && (!getUserIdByUsername(ctx.user, owner) || !owner))
    return fail(why, "Note account unavailable");
  if (sd && !VFS::isSDAvailable()) return fail(why, "No SD card");
  char folder[kPathMax];
  if (!noteFolder(sd, owner, folder, sizeof(folder))) return fail(why, "Invalid request");
  if (!canRead(folder, ctx)) return fail(why, "Notes unavailable");
  if (!VFS::existsGuarded(folder, ctx)) return true;  // no notes yet

  // Only the newest offset + one page are ever shown; keep just those.
  const size_t keep = offset < kMaxNames ? std::min<size_t>(kMaxNames, offset + kPageRows) : 0;
  PsBlock block;
  NameSlot* names = nullptr;
  uint16_t* heap = nullptr;
  if (keep) {
    block.data = ps_alloc(keep * (sizeof(NameSlot) + sizeof(uint16_t)),
                          AllocPolicy::PreferPSRAM, "qnote.list");
    if (!block.data) return fail(why, "Out of memory");
    names = static_cast<NameSlot*>(block.data);
    heap = reinterpret_cast<uint16_t*>(static_cast<char*>(block.data) + keep * sizeof(NameSlot));
  }
  File dir = VFS::openGuarded(folder, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return fail(why, "Notes folder unavailable");
  }
  size_t kept = 0;
  uint32_t total = 0;
  for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const char* name = baseName(file.name());
    if (!file.isDirectory() && noteFileNameParse(name, nullptr, nullptr)) {
      if (total < UINT32_MAX) ++total;
      const size_t length = strlen(name);  // a parsed name is <= kFileNameMax
      if (kept < keep) {
        memcpy(names[kept], name, length + 1);
        heap[kept] = static_cast<uint16_t>(kept);
        heapUp(names, heap, kept);
        ++kept;
      } else if (keep && strcmp(name, names[heap[0]]) > 0) {
        memcpy(names[heap[0]], name, length + 1);
        heapDown(names, heap, kept, 0);
      }
    }
    file.close();
  }
  dir.close();
  if (kept)
    std::sort(heap, heap + kept, [names](uint16_t a, uint16_t b) { return older(names, b, a); });

  for (size_t i = offset; i < kept && out->count < kPageRows; ++i) {
    Row& row = out->rows[out->count];
    row.header = NoteHeader{};
    row.headerValid = false;
    const int n = snprintf(row.path, sizeof(row.path), "%s/%s", folder, names[heap[i]]);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(row.path)) continue;  // cannot happen: kPathMax
    noteFileNameParse(names[heap[i]], row.header.created, &row.header.id);
    readRowHeader(row, ctx);
    ++out->count;
  }
  out->total = total;
  out->nextOffset = offset + static_cast<uint32_t>(out->count);
  out->more = out->nextOffset < std::min<uint32_t>(total, kMaxNames);
  out->capped = total > kMaxNames;
  return true;
}

bool read(const AuthContext& ctx, const char* path, uint32_t offset, char* out,
          size_t cap, size_t* got, uint32_t* total, const char** why) {
  if (why) *why = nullptr;
  if (got) *got = 0;
  if (total) *total = 0;
  if (!out || cap < kReadWindow + 1) return fail(why, "Invalid request");
  out[0] = '\0';
  String normalized;
  if (!notePath(path, normalized)) return fail(why, "Not a note");
  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(why, "Note storage busy");
  File file = VFS::openGuarded(normalized, "r", ctx);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return fail(why, "Note unavailable");
  }
  const size_t size = file.size();
  if (offset > size || (offset && !file.seek(offset))) {
    file.close();
    return fail(why, "Invalid offset");
  }
  const size_t wanted = std::min<size_t>(kReadWindow, size - offset);
  size_t n = wanted ? file.read(reinterpret_cast<uint8_t*>(out), wanted) : 0;
  file.close();
  if (n != wanted) {
    TranscriptionUI::clear(out, cap);
    return fail(why, "Note read incomplete");
  }
  if (offset + n < size) n = TranscriptionUI::completePrefix(out, n);
  out[n] = '\0';
  if (got) *got = n;
  if (total) *total = static_cast<uint32_t>(size);
  return true;
}

bool remove(const AuthContext& ctx, const char* path, const char** why) {
  if (why) *why = nullptr;
  String normalized;
  if (!notePath(path, normalized)) return fail(why, "Not a note");
  FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(why, "Note storage busy");
  if (!VFS::existsGuarded(normalized, ctx)) return fail(why, "Note unavailable");
  if (!VFS::removeGuarded(normalized, ctx)) return fail(why, "Cannot delete note");
  return true;
}

bool sweep(uint32_t* removed, const char** why) {
  if (why) *why = nullptr;
  uint32_t count = 0;
  bool ok = true;
  for (int tier = 0; tier < 2; ++tier) {
    const bool sd = tier == 1;
    FsLockGuard lock("qnote.store", pdMS_TO_TICKS(kLockMs));
    if (!lock.held && !isFsLockedByCurrentTask()) {
      ok = fail(why, "Note storage busy");
      continue;
    }
    // A card that cannot be written now keeps its files for a later sweep.
    if (sd && !VFS::isSDWritable()) continue;
    const char* root = sd ? "/sd/stt" : "/stt";
    if (!VFS::exists(root)) continue;
    // trusted: lists account folders by name and removes only in-progress
    // note files, never reading one; confined to this tier's transcript root.
    const AuthContext sys = VFS::systemAuth(root, "note sweep");
    File top = VFS::openGuarded(root, "r", sys);
    if (!top || !top.isDirectory()) {
      if (top) top.close();
      ok = fail(why, "Notes folder unavailable");
      continue;
    }
    for (File entry = top.openNextFile(); entry; entry = top.openNextFile()) {
      char folder[kPathMax];
      const bool account = entry.isDirectory() &&
          accountNotesFolder(sd, baseName(entry.name()), folder, sizeof(folder));
      entry.close();
      if (account && !sweepFolder(folder, sys, &count)) ok = fail(why, "Cannot remove unfinished note");
    }
    top.close();
  }
  if (removed) *removed = count;
  return ok;
}

}  // namespace QuickNoteStore
#endif
