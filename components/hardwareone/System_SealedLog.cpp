// =============================================================================
// Sealed log files on the card (B11) — implementation. See System_SealedLog.h
// for the contract and System_SealedLogCore.h for the format rules.
// =============================================================================

#include "System_SealedLog.h"

#include "System_CaptureCrypto.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_SealedLogCore.h"
#include "System_VFS.h"

#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include <string.h>

namespace SealedLog {

namespace {

// Files this boot found carrying this device's mark, by path hash and card
// mount. A writer that appends row by row (the sensor log) then reads a file's
// head once per mount, not once per row. A file removed meanwhile is caught by
// its size (0: the mark is written again).
constexpr size_t kVerifiedSlots = 8;
struct Verified {
  uint32_t hash;        // 0: free
  uint32_t generation;  // VFS::sdMountGeneration() when it was seen
};
struct State {
  char*    row;    // PSRAM: one sealed row and its '\n' (CAPCRYPT_MAX_SEALED + 2)
  char*    plain;  // PSRAM: one opened row (readWindow), wiped after each call
  Verified verified[kVerifiedSlots];
  uint8_t  nextSlot;
  uint32_t movedAside;
};
// External BSS: nothing here is in internal RAM. Guarded by the filesystem
// lock every caller holds.
EXT_RAM_BSS_ATTR State sState;

uint32_t pathHash(const String& path) {
  uint32_t h = 2166136261u;  // FNV-1a
  const char* text = path.c_str();
  for (size_t i = 0; i < path.length(); ++i) {
    h ^= static_cast<uint8_t>(text[i]);
    h *= 16777619u;
  }
  return h ? h : 1u;
}

Verified* verifiedSlot(uint32_t hash) {
  for (Verified& slot : sState.verified) {
    if (slot.hash == hash) return &slot;
  }
  return nullptr;
}

bool isVerified(const String& path) {
  const Verified* slot = verifiedSlot(pathHash(path));
  return slot && slot->generation == VFS::sdMountGeneration();
}

void noteVerified(const String& path) {
  const uint32_t hash = pathHash(path);
  Verified* slot = verifiedSlot(hash);
  if (!slot) {
    slot = &sState.verified[sState.nextSlot];
    sState.nextSlot = static_cast<uint8_t>((sState.nextSlot + 1) % kVerifiedSlots);
  }
  slot->hash = hash;
  slot->generation = VFS::sdMountGeneration();
}

void forget(const String& path) {
  Verified* slot = verifiedSlot(pathHash(path));
  if (slot) slot->hash = 0;
}

char* rowBuffer() {
  if (!sState.row) {
    sState.row = static_cast<char*>(ps_alloc(CAPCRYPT_MAX_SEALED + 2, AllocPref::PreferPsram, "sealedlog.row"));
  }
  return sState.row;
}

char* plainBuffer() {
  if (!sState.plain) {
    sState.plain = static_cast<char*>(ps_alloc(CAPCRYPT_MAX_ROW + 1, AllocPref::PreferPsram, "sealedlog.plain"));
  }
  return sState.plain;
}

// The stored bytes readWindow looks at in one call: several rows of a
// transcript, or one maximum row with room to spare.
constexpr size_t kReadStageBytes = 4096;

}  // namespace

bool appliesTo(const String& path) { return VFS::getStorageType(path) == VFS::SDCARD; }

Head inspect(const String& path, const AuthContext& ctx) {
  if (!VFS::existsGuarded(path, ctx)) return Head::Absent;
  File file = VFS::openGuarded(path, "r", ctx);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return Head::Unreadable;
  }
  char head[sealedlog::kMarkProbeBytes];
  const size_t size = file.size();
  const size_t want = size < sizeof(head) ? size : sizeof(head);
  const size_t got = want ? file.read(reinterpret_cast<uint8_t*>(head), want) : 0;
  file.close();
  if (size == 0) return Head::Empty;
  if (got != want) return Head::Unreadable;
  return captureCryptoMarkOwner(head, got) == CaptureMarkOwner::Ours ? Head::Ours : Head::NotOurs;
}

bool moveAside(const String& path, const AuthContext& ctx) {
  // The name is built in the PSRAM row buffer, which is free here (rows are
  // sealed only once the file is open): the log task that gets here through
  // the capped-log writer has a small stack.
  char* name = rowBuffer();
  if (!name) return false;
  const size_t nameCapacity = 192;
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) {
    if (!sealedlog::foreignName(path.c_str(), n, name, nameCapacity)) return false;
    const String target(name);
    if (target.length() != strlen(name)) return false;
    if (VFS::existsGuarded(target, ctx)) continue;
    if (!VFS::renameGuarded(path, target, ctx)) return false;
    forget(path);
    ++sState.movedAside;
    logSystemEvent("FS", "%s on the card is not this device's sealed file; moved aside to %s", path.c_str(), name);
    return true;
  }
  return false;
}

bool prepare(const String& path, const AuthContext& ctx, bool& existsOurs) {
  existsOurs = false;
  if (isVerified(path)) {
    existsOurs = true;
    return true;
  }
  switch (inspect(path, ctx)) {
    case Head::Absent:
    case Head::Empty:
      return true;
    case Head::Ours:
      noteVerified(path);
      existsOurs = true;
      return true;
    case Head::NotOurs:
      return moveAside(path, ctx);
    case Head::Unreadable:
      break;
  }
  return false;
}

bool writeMark(File& file, size_t& stored) {
  char mark[CAPCRYPT_MARK_LINE_MAX + 1];
  const size_t length = captureCryptoMarkLine(mark, CAPCRYPT_MARK_LINE_MAX);
  if (!length) return false;
  mark[length] = '\n';
  const size_t put = file.write(reinterpret_cast<const uint8_t*>(mark), length + 1);
  stored += put;
  return put == length + 1;
}

File openAppend(const String& path, const AuthContext& ctx, size_t& stored) {
  stored = 0;
  // The key first: without it nothing is created or moved on the card.
  if (!captureCryptoEnsureKey()) return File();
  bool ours = false;
  if (!prepare(path, ctx, ours)) return File();
  File file = VFS::openGuarded(path, "a", ctx, true);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return File();
  }
  stored = file.size();
  if (stored == 0) {
    if (!writeMark(file, stored)) {
      file.close();
      forget(path);
      return File();
    }
    noteVerified(path);
  }
  return file;
}

File openFresh(const String& path, const AuthContext& ctx, size_t& stored) {
  stored = 0;
  if (!captureCryptoEnsureKey()) return File();
  const Head head = inspect(path, ctx);
  if (head == Head::Ours || head == Head::Empty) {
    forget(path);
    if (!VFS::removeGuarded(path, ctx)) return File();
  }
  return openAppend(path, ctx, stored);
}

bool writeLines(File& file, const char* data, size_t length, size_t& stored, size_t maxRowBytes) {
  char* row = rowBuffer();
  if (!row || (!data && length)) return false;
  size_t at = 0;
  while (at < length) {
    size_t lineLength = 0, consumed = 0;
    sealedlog::nextLine(data + at, length - at, lineLength, consumed);
    const char* line = data + at;
    size_t left = lineLength;
    do {  // an empty line is one empty row
      const size_t take = sealedlog::nextRowLength(line, left, maxRowBytes);
      const int sealed = captureCryptoSealLine(line, take, row, CAPCRYPT_MAX_SEALED);
      if (sealed < 0) return false;
      row[sealed] = '\n';
      const size_t put = file.write(reinterpret_cast<const uint8_t*>(row), static_cast<size_t>(sealed) + 1);
      stored += put;
      if (put != static_cast<size_t>(sealed) + 1) return false;
      line += take;
      left -= take;
    } while (left > 0);
    at += consumed;
  }
  return !file.getWriteError();
}

LineStream::LineStream(File& file)
    : file_(file),
      carry_(static_cast<char*>(ps_alloc(CAPCRYPT_MAX_ROW + 1, AllocPref::PreferPsram, "sealedlog.carry"))) {
  failed_ = carry_ == nullptr;
}

LineStream::~LineStream() {
  if (carry_) {
    memset(carry_, 0, CAPCRYPT_MAX_ROW + 1);
    ps_free(carry_);
  }
}

bool LineStream::write(const char* data, size_t length) {
  if (failed_) return false;
  size_t at = 0;
  // The line an earlier piece left unfinished ends at this piece's first line
  // end. One that outgrows a row is sealed in row-sized parts as it grows.
  while (carried_ > 0 && at < length) {
    const char c = data[at++];
    if (c == '\n') {
      if (carried_ > 0 && carry_[carried_ - 1] == '\r') --carried_;
      failed_ = !writeLines(file_, carry_, carried_, stored_);
      if (carried_ == 0 && !failed_) {  // the line was a lone CR: still a line
        failed_ = !writeLines(file_, "\n", 1, stored_);
      }
      carried_ = 0;
      if (failed_) return false;
      break;
    }
    if (carried_ == CAPCRYPT_MAX_ROW) {
      if (!writeLines(file_, carry_, carried_, stored_)) { failed_ = true; return false; }
      carried_ = 0;
    }
    carry_[carried_++] = c;
  }
  if (at == length) return true;
  // Whole lines of the rest go straight through; what follows the last line
  // end is carried.
  size_t whole = length;
  while (whole > at && data[whole - 1] != '\n') --whole;
  if (whole > at && !writeLines(file_, data + at, whole - at, stored_)) { failed_ = true; return false; }
  size_t rest = length - whole;
  const char* tail = data + whole;
  while (rest > CAPCRYPT_MAX_ROW) {  // an unfinished line longer than a row
    const size_t take = sealedlog::nextRowLength(tail, rest);
    if (!writeLines(file_, tail, take, stored_)) { failed_ = true; return false; }
    tail += take;
    rest -= take;
  }
  memcpy(carry_, tail, rest);
  carried_ = rest;
  return true;
}

bool LineStream::finish() {
  if (failed_) return false;
  if (carried_ > 0) {
    failed_ = !writeLines(file_, carry_, carried_, stored_);
    carried_ = 0;
  }
  return !failed_;
}

bool readWindow(File& file, size_t offset, char* out, size_t capacity, size_t& got, size_t& nextOffset) {
  got = 0;
  nextOffset = offset;
  if (!out || capacity < 2) return false;
  out[0] = '\0';
  const size_t size = file.size();
  if (offset > size) return false;
  if (offset == size) return true;

  char* stage = static_cast<char*>(ps_alloc(kReadStageBytes + 1, AllocPref::PreferPsram, "sealedlog.read"));
  char* plain = plainBuffer();
  if (!stage || !plain) {
    if (stage) ps_free(stage);
    return false;
  }
  const size_t want = size - offset < kReadStageBytes ? size - offset : kReadStageBytes;
  size_t have = 0;
  bool ok = file.seek(static_cast<uint32_t>(offset));
  while (ok && have < want) {
    const size_t n = file.read(reinterpret_cast<uint8_t*>(stage) + have, want - have);
    if (n == 0) break;
    have += n;
  }
  ok = ok && have == want;
  stage[have] = '\0';

  const size_t rowPrefix = sizeof(CAPCRYPT_ROW_PREFIX) - 1;
  size_t at = 0;
  while (ok && at < have) {
    const char* line = stage + at;
    const char* lineEnd = static_cast<const char*>(memchr(line, '\n', have - at));
    const size_t lineLength = lineEnd ? static_cast<size_t>(lineEnd - line) : have - at;
    const size_t consumed = lineEnd ? lineLength + 1 : lineLength;
    // A line that runs past what was read is taken up again by the next call,
    // from its start. Only a line longer than the whole stage is passed on in
    // pieces (no row is: a sealed row is far shorter).
    const bool whole = lineEnd || offset + have == size;
    if (!whole && at > 0) break;
    size_t textLength = lineLength;
    if (lineEnd && textLength > 0 && line[textLength - 1] == '\r') --textLength;

    if (offset + at == 0 && captureCryptoIsMagicLine(line)) {
      char keyId[sealedlog::kKeyIdHexLength + 1];
      const size_t probe = consumed < sealedlog::kMarkProbeBytes ? consumed : sealedlog::kMarkProbeBytes;
      if (captureCryptoMarkOwner(line, probe, keyId) == CaptureMarkOwner::Other) {
        // Another device's file: say whose, once, and end.
        char notice[CAPCRYPT_FOREIGN_NOTICE_MAX];
        size_t n = captureCryptoForeignNotice(keyId, notice, sizeof(notice));
        if (n + 1 > capacity - 1) n = capacity >= 2 ? capacity - 2 : 0;
        memcpy(out, notice, n);
        out[n] = '\n';
        got = n + 1;
        at = size - offset;  // nothing more to read here
        break;
      }
      at += consumed;
      continue;
    }

    const char* text = line;
    size_t n = textLength;
    if (textLength > rowPrefix && strncmp(line, CAPCRYPT_ROW_PREFIX, rowPrefix) == 0) {
      const int opened = captureCryptoOpenLine(line, textLength, plain, CAPCRYPT_MAX_ROW + 1);
      if (opened >= 0) {
        text = plain;
        n = static_cast<size_t>(opened);
      } else {
        text = CAPCRYPT_UNDECRYPTABLE;
        n = sizeof(CAPCRYPT_UNDECRYPTABLE) - 1;
      }
    }
    const size_t lineEndBytes = lineEnd ? 1 : 0;
    const size_t room = capacity - 1 - got;
    if (n + lineEndBytes > room) {
      if (got > 0) break;  // the next call starts with this row
      n = room - lineEndBytes;  // one row wider than the whole window: cut to it
    }
    memcpy(out + got, text, n);
    got += n;
    if (lineEnd) out[got++] = '\n';
    at += consumed;
  }
  out[got] = '\0';
  nextOffset = offset + at;
  memset(stage, 0, have);
  memset(plain, 0, CAPCRYPT_MAX_ROW + 1);
  ps_free(stage);
  if (!ok) {
    memset(out, 0, capacity);
    got = 0;
    nextOffset = offset;
  }
  return ok;
}

bool looksSealed(File& file) { return captureCryptoLooksSealed(file); }

uint32_t movedAsideCount() { return sState.movedAside; }

}  // namespace SealedLog
