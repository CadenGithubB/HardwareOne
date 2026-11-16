// =============================================================================
// OLED_ConsoleCore — the local display's console history, as pure data
// =============================================================================
// Dependency-free (no Arduino, FreeRTOS or project includes beyond the ASCII
// fold) so host tests compile the shipping ring, sanitizer and snapshot code
// unchanged. OLEDConsoleBuffer wraps one Ring with the allocation, the producer
// mutex and the identity-boundary fence; screens never touch a Ring directly.
//
// Storage model. Each configured slot is a 12-byte Record plus 64 bytes of a
// shared text arena, the same text budget per slot the former 64-byte line
// ring had. Records keep their complete sanitized envelope (up to 255 bytes,
// one output-queue message) and the arena reclaims the oldest records when a
// long one needs their bytes, so a burst of long output retains fewer records
// than slots. Record IDs are monotonic and contiguous within the retained
// range, which is what makes selection stable while new output arrives:
// `index = id - oldestId`, no timestamp scans, no collisions.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_LLMUtf8.h"  // utf8FoldToAscii: what the classic GFX font can draw

namespace oled_console {

constexpr uint32_t kNoRecord = 0;          // never issued; "no selection"
constexpr size_t kRecordTextMax = 255;     // one queue envelope (DEBUG_MSG_SIZE - 1)
constexpr size_t kArenaBytesPerSlot = 64;  // text budget per configured slot
constexpr size_t kPreviewMax = 24;         // preview text capacity with NUL (>= widest canvas)
constexpr uint16_t kMinSlots = 10;
constexpr uint16_t kMaxSlots = 100;

constexpr size_t arenaBytesFor(uint16_t slots) {
  return static_cast<size_t>(slots) * kArenaBytesPerSlot;
}

enum RecordFlag : uint8_t {
  kFlagTruncated = 0x01,  // stored text is a prefix of the sanitized envelope
  kFlagMultiline = 0x02,  // text holds at least one '\n'
};

struct Record {
  uint32_t id;
  uint32_t timestampMs;  // producer stamp; metadata only, never an identity
  uint16_t offset;       // first text byte in the arena
  uint8_t length;        // text bytes (no NUL stored)
  uint8_t flags;         // RecordFlag bits
};

struct Status {
  uint32_t generation;  // changes on every append and clear
  uint32_t oldestId;    // retained range, inclusive; meaningless when count == 0
  uint32_t newestId;
  uint16_t count;
  uint16_t capacity;
  uint32_t evicted;     // lifetime records displaced by newer output
  uint32_t truncated;   // lifetime records stored shorter than their envelope
};

// Owned copy of one record: a screen can keep reading it after eviction.
struct RecordCopy {
  uint32_t id;
  uint32_t timestampMs;
  uint8_t flags;
  uint8_t length;
  char text[kRecordTextMax + 1];
};

// Owned first-line preview for one list row.
struct Preview {
  uint32_t id;
  uint8_t flags;
  bool cut;  // the row shows less than the record holds (long first line or more lines)
  char text[kPreviewMax];
};

// Copies `src` into `dst` as the OLED font can draw it, within checked bounds:
//   - '\n' is kept; '\r' is dropped (CRLF -> LF); '\t' becomes a space;
//   - escape sequences are removed whole (CSI parameters and final byte, OSC
//     up to BEL/ST, otherwise ESC plus one byte); other control bytes drop;
//   - valid UTF-8 sequences fold to ASCII (cafe, "...", '-'), so a word never
//     vanishes; malformed or truncated sequences drop byte by byte;
//   - trailing newlines are trimmed.
// Never reads past the NUL that ends `src` and never writes past `dstCap`.
// Returns the bytes written (NUL-terminated when dstCap > 0). *truncated is
// set when drawable text had to be left out; *multiline when a '\n' remains.
inline size_t sanitize(const char* src, char* dst, size_t dstCap,
                       bool* truncated, bool* multiline) {
  if (truncated) *truncated = false;
  if (multiline) *multiline = false;
  if (!dst || dstCap == 0) {
    if (truncated && src && *src) *truncated = true;
    return 0;
  }
  size_t w = 0;
  const size_t cap = dstCap - 1;  // keep room for the NUL
  bool overflow = false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(src ? src : "");
  while (*p) {
    const unsigned char c = *p;
    if (c == 0x1B) {  // escape sequence: skip it whole, stopping at the NUL
      ++p;
      if (*p == '[') {
        ++p;
        while (*p >= 0x20 && *p <= 0x3F) ++p;  // parameters and intermediates
        if (*p >= 0x40 && *p <= 0x7E) ++p;     // final byte
      } else if (*p == ']') {
        ++p;
        while (*p && *p != 0x07 && !(*p == 0x1B && p[1] == '\\')) ++p;
        if (*p == 0x07) ++p;
        else if (*p == 0x1B) p += 2;
      } else if (*p) {
        ++p;  // two-byte sequence (ESC + one byte)
      }
      continue;
    }
    if (c == '\r') { ++p; continue; }
    if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
      if (w >= cap) { overflow = true; break; }
      dst[w++] = (c == '\t') ? ' ' : static_cast<char>(c);
      ++p;
      continue;
    }
    if (c < 0x80) { ++p; continue; }  // other C0 controls and DEL
    size_t need = ((c & 0xE0) == 0xC0) ? 2
                : ((c & 0xF0) == 0xE0) ? 3
                : ((c & 0xF8) == 0xF0) ? 4
                                       : 0;  // stray continuation or bad lead
    if (need == 0) { ++p; continue; }
    size_t have = 1;
    while (have < need && (p[have] & 0xC0) == 0x80) ++have;  // stops at NUL too
    if (have < need) { p += have; continue; }                  // truncated sequence
    char folded[4];
    memcpy(folded, p, need);
    const size_t n = utf8FoldToAscii(folded, need);  // never grows
    if (w + n > cap) { overflow = true; break; }
    memcpy(dst + w, folded, n);
    w += n;
    p += need;
  }
  while (w > 0 && dst[w - 1] == '\n') --w;  // trailing breaks draw nothing
  dst[w] = '\0';
  if (truncated && overflow) {
    // Only drawable leftovers count: control-only tails are not lost text.
    for (; *p; ++p) {
      const unsigned char c = *p;
      if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F) || c >= 0xC0) {
        *truncated = true;
        break;
      }
    }
  }
  if (multiline) *multiline = memchr(dst, '\n', w) != nullptr;
  return w;
}

// The record ring over caller-owned storage. Not synchronized: the owner
// serializes every call. Text lives in a circular byte FIFO: retained records
// sit back to back in age order, so admission is plain byte accounting and
// the oldest record is always the next one to leave.
class Ring {
 public:
  Ring() = default;

  // Binds caller-owned storage: `capacity` Records and `arenaBytes` of text.
  // Returns false and stays unbound when the geometry cannot hold a record.
  // `firstId` exists for tests of the ID wrap; production starts at 1.
  bool attach(Record* records, uint16_t capacity, char* arena, size_t arenaBytes,
              uint32_t firstId = 1) {
    if (!records || !arena || capacity == 0 || arenaBytes == 0 ||
        arenaBytes > UINT16_MAX) {
      return false;
    }
    records_ = records;
    capacity_ = capacity;
    arena_ = arena;
    arenaBytes_ = arenaBytes;
    nextId_ = firstId ? firstId : 1;
    head_ = 0;
    count_ = 0;
    writeOff_ = 0;
    used_ = 0;
    return true;
  }
  bool bound() const { return records_ != nullptr; }

  // Stores `text` as the newest record and returns its ID, or kNoRecord when
  // unbound. Evicts the oldest records while the slot or byte budget needs
  // them. When the ID counter would wrap, the history restarts at 1 so IDs
  // stay contiguous and 0 is never issued.
  uint32_t append(const char* text, uint32_t timestampMs) {
    if (!bound()) return kNoRecord;
    if (nextId_ == 0) { clear(); nextId_ = 1; }

    char clean[kRecordTextMax + 1];
    bool truncated = false, multiline = false;
    size_t n = sanitize(text, clean, sizeof(clean), &truncated, &multiline);
    if (n > arenaBytes_) { n = arenaBytes_; truncated = true; }  // unreachable geometry guard

    while (count_ > 0 && (count_ == capacity_ || used_ + n > arenaBytes_)) evictOldest();

    Record& r = records_[head_];
    r.id = nextId_++;
    r.timestampMs = timestampMs;
    r.offset = static_cast<uint16_t>(writeOff_);
    r.length = static_cast<uint8_t>(n);
    r.flags = static_cast<uint8_t>((truncated ? kFlagTruncated : 0) |
                                   (multiline ? kFlagMultiline : 0));
    for (size_t i = 0; i < n; ++i) arena_[(writeOff_ + i) % arenaBytes_] = clean[i];
    writeOff_ = (writeOff_ + n) % arenaBytes_;
    used_ += n;
    head_ = static_cast<uint16_t>((head_ + 1) % capacity_);
    ++count_;
    ++generation_;
    if (truncated) ++truncated_;
    return r.id;
  }

  // Forgets every record and wipes the text. IDs keep counting so a stale
  // selection from before the clear can never alias a new record.
  void clear() {
    if (!bound()) return;
    memset(arena_, 0, arenaBytes_);
    memset(records_, 0, sizeof(Record) * capacity_);
    head_ = 0;
    count_ = 0;
    writeOff_ = 0;
    used_ = 0;
    ++generation_;
  }

  Status status() const {
    Status s{};
    s.generation = generation_;
    s.count = count_;
    s.capacity = capacity_;
    s.evicted = evicted_;
    s.truncated = truncated_;
    if (count_ > 0) {
      s.oldestId = records_[tail()].id;
      s.newestId = s.oldestId + count_ - 1;
    }
    return s;
  }

  bool contains(uint32_t id) const { return indexOf(id) >= 0; }

  bool copyRecord(uint32_t id, RecordCopy& out) const {
    const int index = indexOf(id);
    if (index < 0) return false;
    const Record& r = records_[slot(index)];
    out.id = r.id;
    out.timestampMs = r.timestampMs;
    out.flags = r.flags;
    out.length = r.length;
    for (size_t i = 0; i < r.length; ++i) out.text[i] = byteAt(r, i);
    out.text[r.length] = '\0';
    return true;
  }

  // Copies up to `rows` previews of consecutive records starting at `firstId`
  // (which must be retained) into `out`, each holding the first line cut to
  // `cols` columns. Returns the rows copied; 0 when `firstId` is not retained.
  size_t copyWindow(uint32_t firstId, size_t rows, size_t cols, Preview* out) const {
    if (!out || rows == 0) return 0;
    const int first = indexOf(firstId);
    if (first < 0) return 0;
    if (cols >= kPreviewMax) cols = kPreviewMax - 1;
    size_t copied = 0;
    for (int index = first; index < count_ && copied < rows; ++index, ++copied) {
      const Record& r = records_[slot(index)];
      Preview& pv = out[copied];
      pv.id = r.id;
      pv.flags = r.flags;
      size_t line = 0;
      while (line < r.length && byteAt(r, line) != '\n') ++line;
      const size_t shown = line < cols ? line : cols;
      for (size_t i = 0; i < shown; ++i) pv.text[i] = byteAt(r, i);
      pv.text[shown] = '\0';
      pv.cut = line > cols || line < r.length;
    }
    return copied;
  }

 private:
  uint16_t tail() const {
    return static_cast<uint16_t>((head_ + capacity_ - count_) % capacity_);
  }
  uint16_t slot(int index) const {
    return static_cast<uint16_t>((tail() + index) % capacity_);
  }
  char byteAt(const Record& r, size_t i) const {
    return arena_[(static_cast<size_t>(r.offset) + i) % arenaBytes_];
  }
  // Retained IDs are contiguous, so one subtraction finds the index.
  int indexOf(uint32_t id) const {
    if (count_ == 0) return -1;
    const uint32_t offset = id - records_[tail()].id;
    return offset < count_ ? static_cast<int>(offset) : -1;
  }
  void evictOldest() {
    Record& r = records_[tail()];
    for (size_t i = 0; i < r.length; ++i) arena_[(static_cast<size_t>(r.offset) + i) % arenaBytes_] = 0;
    used_ -= r.length;
    r = Record{};
    --count_;
    ++evicted_;
  }

  Record* records_ = nullptr;
  char* arena_ = nullptr;
  size_t arenaBytes_ = 0;
  uint16_t capacity_ = 0;
  uint16_t head_ = 0;    // next slot to write
  uint16_t count_ = 0;
  size_t writeOff_ = 0;  // next arena byte to write (circular)
  size_t used_ = 0;      // arena bytes held by retained records
  uint32_t nextId_ = 1;
  uint32_t generation_ = 0;
  uint32_t evicted_ = 0;
  uint32_t truncated_ = 0;
};

}  // namespace oled_console
