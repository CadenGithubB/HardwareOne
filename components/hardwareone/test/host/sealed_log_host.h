#pragma once
// Host double of System_SealedLog.h (B11: log files on the card are sealed)
// for harnesses that splice a log writer or a reader of such files.
//
// What it is
//   The SealedLog API with the production rules (the mark first in a new
//   file, one sealed row per line, a file that is not this device's moved
//   aside to "<name>.foreign-<n>", whole rows read back by stored offset) over
//   the harness's own File and VFS doubles, with a toy seal: "ENC1:" and
//   three fixed bytes plus the text in base64. The production module itself is executed by
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
// Helpers for expectations: mark() (the mark line with its '\n'),
// sealRow(text) (one stored row with its '\n'), reveal(stored) (what a reader
// of the file sees: the mark dropped, every row opened, '\n' after each).

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

inline const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// "ENC1:" + base64("HW1" + text), no line end: the three bytes stand in for
// the nonce and tag, so the row of an empty line is not empty either (as on
// the device, where it is 40 characters). False without the key, for a row
// over the seal's limit, or when sealFailAt names this call.
inline const char kToyNonce[] = "HW1";
inline bool seal(const char* plain, size_t plainLength, std::string& row) {
  ++sealCalls;
  if (!keyAvailable || plainLength > sealedlog::kMaxRowBytes || sealCalls == sealFailAt) return false;
  const std::string padded = std::string(kToyNonce) + std::string(plain, plainLength);
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

inline bool open(const char* row, size_t length, std::string& text) {
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
  if (text.compare(0, sizeof(kToyNonce) - 1, kToyNonce) != 0) { text.clear(); return false; }
  text.erase(0, sizeof(kToyNonce) - 1);
  return true;
}

inline std::string mark() { return std::string("#HW1ENC v1 k1 id=") + keyId + "\n"; }

inline std::string sealRow(const std::string& text) {
  const int calls = sealCalls;
  const bool key = keyAvailable;
  keyAvailable = true;
  std::string row;
  sealCalls = -1000000;  // never the failing call
  (void)seal(text.data(), text.size(), row);
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
    if (line.rfind("ENC1:", 0) == 0) out += open(line.data(), line.size(), text) ? text : std::string("[undecryptable row]");
    else out += line;
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
inline bool captureCryptoEnsureKey() { return sealed_log_host::keyAvailable; }
inline int captureCryptoSealLine(const char* in, size_t length, char* out, size_t capacity) {
  std::string row;
  if (!in || !out || !sealed_log_host::seal(in, length, row) || row.size() + 1 > capacity) return -1;
  std::memcpy(out, row.c_str(), row.size() + 1);
  return static_cast<int>(row.size());
}
inline int captureCryptoOpenLine(const char* line, size_t length, char* out, size_t capacity) {
  std::string text;
  if (!line || !out || !sealed_log_host::open(line, length, text) || text.size() + 1 > capacity) return -1;
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

#if defined(SEALED_LOG_HOST_OPEN) || defined(SEALED_LOG_HOST_FRESH)
template <class Path, class Ctx>
Head inspect(const Path& path, const Ctx& ctx) {
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

template <class Path, class Ctx>
bool moveAside(const Path& path, const Ctx& ctx) {
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
    case Head::Ours:    existsOurs = true; return true;
    case Head::NotOurs: return moveAside(path, ctx);
    default:            return false;
  }
}
#endif  // SEALED_LOG_HOST_OPEN

template <class F>
bool writeMark(F& file, size_t& stored) {
  if (!sealed_log_host::keyAvailable) return false;
  const std::string line = sealed_log_host::mark();
  const size_t put = file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  stored += put;
  return put == line.size();
}

template <class F>
bool writeLines(F& file, const char* data, size_t length, size_t& stored, size_t maxRowBytes = 1023) {
  size_t at = 0;
  while (at < length) {
    size_t lineLength = 0, consumed = 0;
    sealedlog::nextLine(data + at, length - at, lineLength, consumed);
    const char* line = data + at;
    size_t left = lineLength;
    do {
      const size_t take = sealedlog::nextRowLength(line, left, maxRowBytes);
      std::string row;
      if (!sealed_log_host::seal(line, take, row)) return false;
      row += '\n';
      const size_t put = file.write(reinterpret_cast<const uint8_t*>(row.data()), row.size());
      stored += put;
      if (put != row.size()) return false;
      line += take;
      left -= take;
    } while (left > 0);
    at += consumed;
  }
  return true;
}

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
// production readWindow gives them.
template <class F>
bool readWindow(F& file, size_t offset, char* out, size_t capacity, size_t& got, size_t& nextOffset) {
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
  size_t at = 0;
  while (at < stage.size()) {
    size_t end = stage.find('\n', at);
    const bool ended = end != std::string::npos;
    if (!ended) end = stage.size();
    const std::string line = stage.substr(at, end - at);
    const size_t consumed = end - at + (ended ? 1 : 0);
    if (offset + at == 0 && line.rfind("#HW1ENC", 0) == 0) {
      char id[17];
      const std::string head = line + "\n";
      if (sealed_log_host::keyAvailable && sealed_log_host::markKind(head.data(), head.size(), id) == sealedlog::Mark::Foreign) {
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
    std::string text = line;
    if (line.rfind("ENC1:", 0) == 0 && !sealed_log_host::open(line.data(), line.size(), text)) text = "[undecryptable row]";
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
  explicit LineStream(F& file) : file_(file) {}
  bool write(const char* data, size_t length) {
    if (failed_) return false;
    carry_.append(data, length);
    const size_t whole = carry_.rfind('\n');
    if (whole == std::string::npos) return true;
    failed_ = !writeLines(file_, carry_.data(), whole + 1, stored_);
    carry_.erase(0, whole + 1);
    return !failed_;
  }
  bool finish() {
    if (!failed_ && !carry_.empty()) {
      failed_ = !writeLines(file_, carry_.data(), carry_.size(), stored_);
      carry_.clear();
    }
    return !failed_;
  }
  size_t stored() const { return stored_; }

 private:
  F& file_;
  std::string carry_;
  size_t stored_ = 0;
  bool failed_ = false;
};
template <class F> LineStream(F&) -> LineStream<F>;

inline uint32_t movedAsideCount() { return static_cast<uint32_t>(sealed_log_host::movedAside); }

}  // namespace SealedLog
#endif  // SEALED_LOG_HOST_CRYPTO_ONLY
