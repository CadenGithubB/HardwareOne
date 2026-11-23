#ifndef SYSTEM_SEALED_LOG_H
#define SYSTEM_SEALED_LOG_H

// ============================================================================
// Sealed log files on the card (B11, 2026-11-22)
//
// Every log file a logger writes on the card is sealed with the device key:
// the capped logs under /sd/system/sys_logs, the system log, sensor, track
// and automation captures under /sd/logging_captures, and saved transcripts
// under /sd/stt. Files on flash stay as they are. The format is the per-line
// at-rest seal of System_CaptureCrypto (System_SealedLogCore.h has the rules):
//
//   #HW1ENC v1 k1 id=<16 hex>            the mark, and which device key
//   <plain schema line(s)>               sensor CSV / track header only
//   ENC1:<base64(nonce12 | ct | tag16)>  one per row
//
// A writer here never puts a plain row on the card and never appends to a card
// file that lacks the mark or carries another device's key id: such a file is
// moved aside to "<name>.foreign-<n>" and a new sealed file is started.
// Nothing in it is read or converted.
//
// What the seal protects: a row cannot be read or forged without the key. What
// it does not: the additional data is a constant, so someone with the card can
// delete, reorder or copy rows between files unnoticed; file names, sizes,
// times, the mark line and CSV column names stay visible.
//
// Locking: every function here does file I/O and shares one PSRAM row buffer.
// The caller holds the filesystem lock (every log writer and transcript
// reader already does).
// ============================================================================

#include <Arduino.h>
#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "System_User.h"  // AuthContext

namespace SealedLog {

// True when log files at `path` are sealed: it is on the card.
bool appliesTo(const String& path);

enum class Head : uint8_t {
  Absent,      // no such file
  Empty,       // exists, holds nothing
  Ours,        // starts with this device's mark: rows may be appended
  NotOurs,     // no mark, another device's key id, or a mark without an id
  Unreadable,  // could not be opened or read (the card, or a permission)
};
// Looks at the start of the card file `path` as `ctx` (which needs READ).
Head inspect(const String& path, const AuthContext& ctx);

// Moves `path` to the first free "<path>.foreign-<n>" as `ctx` (RENAME on the
// file, CREATE beside it). False when no name was free or the rename failed.
bool moveAside(const String& path, const AuthContext& ctx);

// Makes `path` ready for this device's sealed rows without opening it for
// writing: a file that is not ours is moved aside. True when the path is then
// absent, empty or ours. `existsOurs` tells which: true for a file that
// already carries this device's mark. The answer for a file found ours is
// remembered for the card's current mount, so a writer that appends row by
// row does not read the head again each time.
bool prepare(const String& path, const AuthContext& ctx, bool& existsOurs);

// Opens `path` for sealed appends as `ctx` (create = true): prepares it as
// above and writes the mark line into a new or empty file. `stored` receives
// the file's size after that (the mark included). An empty File when the card
// or the key failed; nothing plain was written then.
File openAppend(const String& path, const AuthContext& ctx, size_t& stored);

// Like openAppend for a file that starts anew (the system log's boot
// auto-start, the boot debug file): this device's own file of the name is
// removed first; one that is not ours is moved aside, as always.
File openFresh(const String& path, const AuthContext& ctx, size_t& stored);

// Writes the mark line and its '\n' into a file the caller just created
// empty (a staged header, a rotation's copy). `stored` grows by the bytes the
// handle took. False on a key failure or a short write.
bool writeMark(File& file, size_t& stored);

// Seals `data` (`length` bytes: one or more lines ending in "\n" or "\r\n";
// text after the last line end is a line too) and appends one row per line.
// A line longer than `maxRowBytes` is split into several rows, never dropped
// (at most 1023, the seal's limit). Each row is one write, so a card pulled
// mid-write tears at most the last row. `stored` grows by the bytes the
// handle took. False on a key failure or a short write: the caller treats the
// card as failed, and no plain byte was written.
bool writeLines(File& file, const char* data, size_t length, size_t& stored,
                size_t maxRowBytes = 1023);

// Text that arrives in pieces which do not end at line ends (a buffer written
// chunk by chunk): whole lines are sealed as they come and the unfinished end
// of a piece is carried into the next one, so a line is one row however the
// pieces fall. finish() seals what is left. The carry buffer is PSRAM, taken
// at construction and freed at destruction.
class LineStream {
 public:
  explicit LineStream(File& file);
  ~LineStream();
  LineStream(const LineStream&) = delete;
  LineStream& operator=(const LineStream&) = delete;
  bool write(const char* data, size_t length);  // false: the card or the key failed
  bool finish();
  size_t stored() const { return stored_; }

 private:
  File&  file_;
  char*  carry_;
  size_t carried_ = 0;
  size_t stored_ = 0;
  bool   failed_ = false;
};

// Reads revealed text from the sealed file `file` (open for reading), from
// the stored byte offset `offset` (0, or a `nextOffset` this function gave):
// whole rows while they fit `capacity` - 1 bytes, each followed by '\n'. The
// mark line is skipped; a plain schema line is passed through; a row that
// cannot be opened reads "[undecryptable row]". A file another device sealed
// yields one notice line and ends. `got` is the text length (NUL-terminated),
// `nextOffset` the stored offset to continue from (the file's size at its
// end). False when the read failed. `out` held plain text: the caller clears
// it after use.
bool readWindow(File& file, size_t offset, char* out, size_t capacity,
                size_t& got, size_t& nextOffset);

// True when the open file starts with the mark (its read position is put back
// to 0).
bool looksSealed(File& file);

// Files moved aside since boot (shown by `logtier`).
uint32_t movedAsideCount();

}  // namespace SealedLog

#endif  // SYSTEM_SEALED_LOG_H
