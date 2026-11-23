#pragma once
// Host double of System_SealedLog.h (B11: log files on the card are sealed)
// for harnesses that splice a log writer or a reader of such files.
//
// What it is
//   The SealedLog API with the production rules (the mark first in a new
//   file, one sealed row per line, a file that is not this device's moved
//   aside to "<name>.foreign-<n>" in the log trees only, a torn last row
//   ended before the next one, a write that counts once the flushed file has
//   the expected size, whole rows read back by stored offset, a plain line in
//   a sealed file read as "[unsealed line]") over the harness's own File and
//   VFS doubles, with a toy seal: "ENC1:" and base64 of 28 bytes (three of
//   them stand for the class of the file's path, sealedlog::classOf: a row
//   opens only at a path of the class it was sealed for) plus the text, so a
//   toy row is as long as a real one. The production module itself is executed by
//   test_capped_log_writes.py (under the capped-log writer) and
//   test_sealed_log.py (directly); this double is for the writers around it.
//
// Opting in
//   Include it after the harness's String, File, AuthContext and VFS doubles.
//   Every SealedLog function is a template, so a harness only needs the File
//   members of the calls its production code makes; the functions that go
//   through the VFS are behind two switches, because a namespace's functions
//   are looked up where a template is written:
//     always                     appliesTo; writeMark / writeLines / LineStream
//                                (File::write(const uint8_t*, size_t));
//                                settle (File::flush, File::size); noteTail;
//                                readWindow / looksSealed (File::size,
//                                File::seek, File::read)
//     SEALED_LOG_HOST_OPEN       inspect, moveAside, prepare, openAppend
//                                (VFS::existsGuarded, VFS::renameGuarded,
//                                VFS::openGuarded(path, mode, ctx, create),
//                                File::size, File::read, File::close)
//     SEALED_LOG_HOST_FRESH      openFresh as well (VFS::removeGuarded)
//   #define SEALED_LOG_HOST_MARK_API before the include to also get the
//   capture-crypto names a writer of marked files calls
//   (CAPCRYPT_MARK_LINE_MAX, CaptureMarkOwner, captureCryptoMarkLine,
//   captureCryptoMarkOwner) on the same toy key.
//   #define SEALED_LOG_HOST_CRYPTO_API for the rest of the capture-crypto
//   calls on that key (captureCryptoEnsureKey, SealLine, OpenLine,
//   IsMagicLine, LooksSealed, ForeignNotice), and
//   SEALED_LOG_HOST_CRYPTO_ONLY to leave the SealedLog double out: that is
//   how test_sealed_log.py compiles the production System_SealedLog.cpp over
//   this toy key.
//
// Knobs (namespace sealed_log_host)
//   keyAvailable   false: no mark, no seal, no open (an NVS failure)
//   keyId          this device's key id (16 lower-case hex)
//   cardPrefix     what counts as the card ("/sd/")
//   movedAside     files moved aside so far
//   sealFailAt     the Nth seal call fails (0: none); sealCalls counts
//
//   defaultPath    the path sealRow(text) seals for when none is given
//
// Helpers for expectations: mark() (the mark line with its '\n'),
// sealRow(text[, path]) (one stored row with its '\n', sealed for the class
// of `path`), reveal(stored) (the text of a file whatever class its rows were
// sealed for: the mark dropped, every row opened, '\n' after each),
// view(stored, path) (what a reader at `path` sees: rows of another class
// "[undecryptable row]", plain lines where only rows belong
// "[unsealed line]").

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "System_SealedLogCore.h"  // the production format rules (add the component to the include path)

namespace sealed_log_host {

inline bool keyAvailable = true;
inline std::string keyId = "0123456789abcdef";
inline const char* cardPrefix = "/sd/";
inline int movedAside = 0;
inline int sealCalls = 0;
inline int sealFailAt = 0;
inline std::string defaultPath = "/sd/system/sys_logs/host.log";

inline const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// What stands in for the nonce and the tag of a row: 28 bytes, as on the
// device, so a toy row is exactly as long as a real one
// (sealedlog::sealedRowBytes). The first three stand for the class of `path`
// (the device binds the class through the seal's additional data). Empty when
// the path has no class: such a file gets no rows.
constexpr size_t kToyOverhead = 28;
inline std::string classTag(const char* path) {
  char cls[sealedlog::kClassMax];
  if (!path || !sealedlog::classOf(path, cls, sizeof(cls))) return std::string();
  uint32_t h = 2166136261u;
  for (const char* c = cls; *c; ++c) { h ^= static_cast<unsigned char>(*c); h *= 16777619u; }
  std::string tag;
  for (int i = 0; i < 3; ++i) tag += kAlphabet[(h >> (6 * i)) & 63];
  tag.append(kToyOverhead - 3, '.');
  return tag;
}

// "ENC1:" + base64(<class tag> + text), no line end: the row of an empty line
// is 40 characters plus the prefix, as on the device, and the tag binds the
// row to the class of `path`. False without the key, for a path without a class, for a
// row over the seal's limit, or when sealFailAt names this call.
inline bool seal(const char* path, const char* plain, size_t plainLength, std::string& row) {
  ++sealCalls;
  const std::string tag = classTag(path);
  if (!keyAvailable || tag.empty() || plainLength > sealedlog::kMaxRowBytes || sealCalls == sealFailAt) return false;
  const std::string padded = tag + std::string(plain, plainLength);
  const char* text = padded.data();
  const size_t length = padded.size();
  row = "ENC1:";
  for (size_t i = 0; i < length; i += 3) {
    const unsigned a = static_cast<unsigned char>(text[i]);
    const unsigned b = i + 1 < length ? static_cast<unsigned char>(text[i + 1]) : 0;
    const unsigned c = i + 2 < length ? static_cast<unsigned char>(text[i + 2]) : 0;
    const unsigned triple = (a << 16) | (b << 8) | c;
    row += kAlphabet[(triple >> 18) & 63];
    row += kAlphabet[(triple >> 12) & 63];
    row += i + 1 < length ? kAlphabet[(triple >> 6) & 63] : '=';
    row += i + 2 < length ? kAlphabet[triple & 63] : '=';
  }
  return true;
}

// Opens a toy row read at `path`; nullptr opens a row of any class (for
// expectations about a file's text).
inline bool open(const char* path, const char* row, size_t length, std::string& text) {
  text.clear();
  if (!keyAvailable || length < 5 || std::memcmp(row, "ENC1:", 5) != 0 || (length - 5) % 4 != 0) return false;
  for (size_t i = 5; i < length; i += 4) {
    unsigned triple = 0;
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char ch = row[i + k];
      const char* at = ch == '=' ? nullptr : std::strchr(kAlphabet, ch);
      if (ch == '=') {
        if (i + 4 != length || k < 2) return false;
        ++pad;
      } else if (!at || ch == '\0' || pad) {
        return false;
      }
      triple = (triple << 6) | (at ? static_cast<unsigned>(at - kAlphabet) : 0u);
    }
    for (int k = 0; k < 3 - pad; ++k) text += static_cast<char>((triple >> (16 - 8 * k)) & 0xff);
  }
  if (text.size() < kToyOverhead || (path && text.compare(0, kToyOverhead, classTag(path)) != 0)) { text.clear(); return false; }
  text.erase(0, kToyOverhead);
  return true;
}

inline std::string mark() { return std::string("#HW1ENC v1 k1 id=") + keyId + "\n"; }

inline std::string sealRow(const std::string& text, const char* path = nullptr) {
  const int calls = sealCalls;
  const bool key = keyAvailable;
  keyAvailable = true;
  std::string row;
  sealCalls = -1000000;  // never the failing call
  (void)seal(path ? path : defaultPath.c_str(), text.data(), text.size(), row);
  sealCalls = calls;
  keyAvailable = key;
  return row + "\n";
}

inline std::string reveal(const std::string& stored) {
  std::string out;
  size_t at = 0;
  bool first = true;
  while (at < stored.size()) {
    size_t end = stored.find('\n', at);
    const bool ended = end != std::string::npos;
    if (!ended) end = stored.size();
    std::string line = stored.substr(at, end - at);
    at = end + 1;
    if (first && line.rfind("#HW1ENC", 0) == 0) { first = false; continue; }
    first = false;
    std::string text;
    if (line.rfind("ENC1:", 0) == 0) out += open(nullptr, line.data(), line.size(), text) ? text : std::string("[undecryptable row]");
    else out += line;
    if (ended) out += '\n';
  }
  return out;
}

// What a reader of the whole file at `path` is shown (the mark line kept).
inline std::string view(const std::string& stored, const char* path) {
  std::string out;
  sealedlog::LineState state = sealedlog::stateFor(path);
  size_t at = 0;
  while (at < stored.size()) {
    size_t end = stored.find('\n', at);
    const bool ended = end != std::string::npos;
    if (!ended) end = stored.size();
    const std::string line = stored.substr(at, end - at);
    at = end + 1;
    std::string text;
    switch (sealedlog::classifyLine(state, line.data(), line.size())) {
      case sealedlog::Line::Row:
        out += open(path, line.data(), line.size(), text) ? text : std::string("[undecryptable row]");
        break;
      case sealedlog::Line::Unsealed: out += sealedlog::kUnsealedLine; break;
      default:                        out += line; break;
    }
    if (ended) out += '\n';
  }
  return out;
}

inline sealedlog::Mark markKind(const char* head, size_t length, char keyIdOut[17] = nullptr) {
  const sealedlog::MarkInfo info = sealedlog::classifyMark(head, length, keyAvailable ? keyId.c_str() : nullptr);
  if (keyIdOut) std::memcpy(keyIdOut, info.keyId, sizeof(info.keyId));
  return info.kind;
}

}  // namespace sealed_log_host

#ifdef SEALED_LOG_HOST_MARK_API
#ifndef CAPCRYPT_MARK_LINE_MAX
#define CAPCRYPT_MARK_LINE_MAX 48
#endif
enum class CaptureMarkOwner : uint8_t { None, Ours, Other, Unknown };
inline size_t captureCryptoMarkLine(char* out, size_t capacity) {
  return sealed_log_host::keyAvailable ? sealedlog::formatMark(sealed_log_host::keyId.c_str(), out, capacity) : 0;
}
inline CaptureMarkOwner captureCryptoMarkOwner(const char* head, size_t length, char keyIdOut[17] = nullptr) {
  switch (sealed_log_host::markKind(head, length, keyIdOut)) {
    case sealedlog::Mark::Unmarked: return CaptureMarkOwner::None;
    case sealedlog::Mark::Own:      return CaptureMarkOwner::Ours;
    // Without this device's own id a named id cannot be told from ours (as in production).
    case sealedlog::Mark::Foreign:  return sealed_log_host::keyAvailable ? CaptureMarkOwner::Other : CaptureMarkOwner::Unknown;
    default:                        return CaptureMarkOwner::Unknown;
  }
}
#endif  // SEALED_LOG_HOST_MARK_API

#ifdef SEALED_LOG_HOST_CRYPTO_API
#ifndef CAPCRYPT_UNSEALED
#define CAPCRYPT_UNSEALED "[unsealed line]"
#endif
inline bool captureCryptoEnsureKey() { return sealed_log_host::keyAvailable; }
inline bool captureCryptoKeyIdHex(char out[17]) {
  if (!sealed_log_host::keyAvailable) { out[0] = '\0'; return false; }
  std::memcpy(out, sealed_log_host::keyId.c_str(), 17);
  return true;
}
inline int captureCryptoSealLine(const char* path, const char* in, size_t length, char* out, size_t capacity) {
  std::string row;
  if (!path || !in || !out || !sealed_log_host::seal(path, in, length, row) || row.size() + 1 > capacity) return -1;
  std::memcpy(out, row.c_str(), row.size() + 1);
  return static_cast<int>(row.size());
}
inline int captureCryptoOpenLine(const char* path, const char* line, size_t length, char* out, size_t capacity) {
  std::string text;
  if (!path || !line || !out || !sealed_log_host::open(path, line, length, text) || text.size() + 1 > capacity) return -1;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return static_cast<int>(text.size());
}
inline bool captureCryptoIsMagicLine(const char* line) { return line && std::strncmp(line, "#HW1ENC", 7) == 0; }
template <class F>
bool captureCryptoLooksSealed(F& file) {
  char head[7];
  file.seek(0);
  const size_t got = file.read(reinterpret_cast<uint8_t*>(head), sizeof(head));
  file.seek(0);
  return got == sizeof(head) && std::memcmp(head, "#HW1ENC", 7) == 0;
}
inline size_t captureCryptoForeignNotice(const char* keyIdHex, char* out, size_t capacity) {
  const int n = std::snprintf(out, capacity, "[sealed by device %s: its rows can only be read on that device]", keyIdHex);
  return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}
#endif  // SEALED_LOG_HOST_CRYPTO_API

#ifndef SEALED_LOG_HOST_CRYPTO_ONLY
namespace SealedLog {

enum class Head : uint8_t { Absent, Empty, Ours, NotOurs, Unreadable };

template <class Path>
bool appliesTo(const Path& path) {
  return std::strncmp(path.c_str(), sealed_log_host::cardPrefix, std::strlen(sealed_log_host::cardPrefix)) == 0;
}

template <class F>
bool writeMark(F& file, size_t& stored) {
  if (!sealed_log_host::keyAvailable) return false;
  const std::string line = sealed_log_host::mark();
  const size_t put = file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  stored += put;
  return put == line.size();
}

#if defined(SEALED_LOG_HOST_OPEN) || defined(SEALED_LOG_HOST_FRESH)
template <class Path, class Ctx>
Head inspect(const Path& path, const Ctx& ctx) {
  if (!sealed_log_host::keyAvailable) return Head::Unreadable;  // nothing is judged without the key
  if (!VFS::existsGuarded(path, ctx)) return Head::Absent;
  auto file = VFS::openGuarded(path, "r", ctx, false);
  if (!file) return Head::Unreadable;
  char head[sealedlog::kMarkProbeBytes];
  const size_t size = file.size();
  const size_t want = size < sizeof(head) ? size : sizeof(head);
  const size_t got = want ? file.read(reinterpret_cast<uint8_t*>(head), want) : 0;
  file.close();
  if (size == 0) return Head::Empty;
  if (got != want) return Head::Unreadable;
  return sealed_log_host::markKind(head, got) == sealedlog::Mark::Own ? Head::Ours : Head::NotOurs;
}

// Only in the log trees, as in production. (The production module also
// replaces the oldest set-aside file when all names are taken; this double
// gives up, which test_sealed_log.py covers on the real module.)
template <class Path, class Ctx>
bool moveAside(const Path& path, const Ctx& ctx) {
  if (!sealedlog::inLogTree(path.c_str())) return false;
  char name[192];
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) {
    if (!sealedlog::foreignName(path.c_str(), n, name, sizeof(name))) return false;
    const Path target(name);
    if (VFS::existsGuarded(target, ctx)) continue;
    if (!VFS::renameGuarded(path, target, ctx)) return false;
    ++sealed_log_host::movedAside;
    return true;
  }
  return false;
}

template <class Path, class Ctx>
bool prepare(const Path& path, const Ctx& ctx, bool& existsOurs) {
  existsOurs = false;
  switch (inspect(path, ctx)) {
    case Head::Absent:
    case Head::Empty:   return true;
    case Head::Ours:    break;
    case Head::NotOurs: return moveAside(path, ctx);
    default:            return false;
  }
  existsOurs = true;
  // A torn last row gets its line end, so the next row is not lost with it.
  auto file = VFS::openGuarded(path, "r", ctx, false);
  if (!file) return false;
  const size_t size = file.size();
  uint8_t last = '\n';
  const bool read = size == 0 || (file.seek(size - 1) && file.read(&last, 1) == 1);
  file.close();
  if (!read) return false;
  if (last == '\n') return true;
  auto tail = VFS::openGuarded(path, "a", ctx, true);
  if (!tail) return false;
  const uint8_t lineEnd = '\n';
  const bool put = tail.write(&lineEnd, 1) == 1;
  tail.close();
  return put;
}
#endif  // SEALED_LOG_HOST_OPEN

template <class F, class Path>
bool writeLines(F& file, const Path& path, const char* data, size_t length, size_t& stored,
                size_t maxRowBytes = 1023, size_t* consumedOut = nullptr) {
  if (consumedOut) *consumedOut = 0;
  size_t at = 0;
  while (at < length) {
    size_t lineLength = 0, consumed = 0;
    sealedlog::nextLine(data + at, length - at, lineLength, consumed);
    const char* line = data + at;
    size_t left = lineLength;
    do {
      const size_t take = sealedlog::nextRowLength(line, left, maxRowBytes);
      std::string row;
      if (!sealed_log_host::seal(path.c_str(), line, take, row)) return false;
      row += '\n';
      const size_t put = file.write(reinterpret_cast<const uint8_t*>(row.data()), row.size());
      stored += put;
      if (put != row.size()) return false;
      line += take;
      left -= take;
    } while (left > 0);
    at += consumed;
    if (consumedOut) *consumedOut = at;
  }
  return true;
}

// The write counts once the flushed file has the size its rows make.
template <class F, class Path>
bool settle(F& file, const Path&, size_t expectedStored) {
  file.flush();
  return static_cast<size_t>(file.size()) == expectedStored;
}

template <class Path>
void noteTail(const Path&, bool) {}

#if defined(SEALED_LOG_HOST_OPEN) || defined(SEALED_LOG_HOST_FRESH)
template <class Path, class Ctx>
auto openAppend(const Path& path, const Ctx& ctx, size_t& stored) -> decltype(VFS::openGuarded(path, "a", ctx, true)) {
  using FileType = decltype(VFS::openGuarded(path, "a", ctx, true));
  stored = 0;
  bool ours = false;
  if (!sealed_log_host::keyAvailable || !prepare(path, ctx, ours)) return FileType();
  FileType file = VFS::openGuarded(path, "a", ctx, true);
  if (!file) return FileType();
  stored = file.size();
  if (stored == 0 && !writeMark(file, stored)) {
    file.close();
    return FileType();
  }
  return file;
}

#endif  // SEALED_LOG_HOST_OPEN

#ifdef SEALED_LOG_HOST_FRESH
template <class Path, class Ctx>
auto openFresh(const Path& path, const Ctx& ctx, size_t& stored) -> decltype(VFS::openGuarded(path, "a", ctx, true)) {
  using FileType = decltype(VFS::openGuarded(path, "a", ctx, true));
  stored = 0;
  if (!sealed_log_host::keyAvailable) return FileType();
  const Head head = inspect(path, ctx);
  if (head == Head::Unreadable) return FileType();
  if ((head == Head::Ours || head == Head::Empty) && !VFS::removeGuarded(path, ctx)) return FileType();
  return openAppend(path, ctx, stored);
}
#endif  // SEALED_LOG_HOST_FRESH

template <class F>
bool looksSealed(F& file) {
  char head[7];
  file.seek(0);
  const size_t got = file.read(reinterpret_cast<uint8_t*>(head), sizeof(head));
  file.seek(0);
  return got == sizeof(head) && std::memcmp(head, "#HW1ENC", 7) == 0;
}

// Whole rows from the stored offset while they fit capacity - 1, as the
// production readWindow gives them: rows open with the class of `path`, and a
// plain line where only rows belong reads "[unsealed line]".
template <class F, class Path>
bool readWindow(F& file, const Path& path, size_t offset, char* out, size_t capacity, size_t& got, size_t& nextOffset) {
  got = 0;
  nextOffset = offset;
  if (!out || capacity < 2) return false;
  out[0] = '\0';
  const size_t size = file.size();
  if (offset > size) return false;
  if (offset == size) return true;
  std::string stage(size - offset, '\0');
  if (!file.seek(offset)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(&stage[0]), stage.size()) != stage.size()) return false;
  sealedlog::LineState lines = sealedlog::stateFor(path.c_str(), offset != 0);
  size_t at = 0;
  while (at < stage.size()) {
    size_t end = stage.find('\n', at);
    const bool ended = end != std::string::npos;
    if (!ended) end = stage.size();
    const std::string line = stage.substr(at, end - at);
    const size_t consumed = end - at + (ended ? 1 : 0);
    std::string text = line;
    const sealedlog::Line kind = sealedlog::classifyLine(lines, line.data(), line.size());
    if (kind == sealedlog::Line::Mark) {
      char id[17];
      const std::string head = line + "\n";
      if (offset + at == 0 && sealed_log_host::keyAvailable &&
          sealed_log_host::markKind(head.data(), head.size(), id) == sealedlog::Mark::Foreign) {
        const std::string notice = std::string("[sealed by device ") + id + "]\n";
        const size_t n = notice.size() < capacity - 1 ? notice.size() : capacity - 1;
        std::memcpy(out, notice.data(), n);
        got = n;
        at = stage.size();
        break;
      }
      at += consumed;
      continue;
    }
    if (kind == sealedlog::Line::Row) {
      if (!sealed_log_host::open(path.c_str(), line.data(), line.size(), text)) text = "[undecryptable row]";
    } else if (kind == sealedlog::Line::Unsealed) {
      text = sealedlog::kUnsealedLine;
    }
    const size_t need = text.size() + (ended ? 1 : 0);
    if (need > capacity - 1 - got) {
      if (got > 0) break;
      text.resize(capacity - 1 - (ended ? 1 : 0));
    }
    std::memcpy(out + got, text.data(), text.size());
    got += text.size();
    if (ended) out[got++] = '\n';
    at += consumed;
  }
  out[got] = '\0';
  nextOffset = offset + at;
  return true;
}

template <class F>
class LineStream {
 public:
  template <class Path>
  LineStream(F& file, const Path& path) : file_(file), path_(path.c_str()) {}
  bool write(const char* data, size_t length) {
    if (failed_) return false;
    carry_.append(data, length);
    const size_t whole = carry_.rfind('\n');
    if (whole == std::string::npos) return true;
    failed_ = !writeLines(file_, path_, carry_.data(), whole + 1, stored_);
    carry_.erase(0, whole + 1);
    return !failed_;
  }
  bool finish() {
    if (!failed_ && !carry_.empty()) {
      failed_ = !writeLines(file_, path_, carry_.data(), carry_.size(), stored_);
      carry_.clear();
    }
    return !failed_;
  }
  size_t stored() const { return stored_; }

 private:
  F& file_;
  std::string path_;
  std::string carry_;
  size_t stored_ = 0;
  bool failed_ = false;
};
template <class F, class Path> LineStream(F&, const Path&) -> LineStream<F>;

inline uint32_t movedAsideCount() { return static_cast<uint32_t>(sealed_log_host::movedAside); }

}  // namespace SealedLog
#endif  // SEALED_LOG_HOST_CRYPTO_ONLY
