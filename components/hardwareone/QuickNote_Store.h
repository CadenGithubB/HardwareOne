#pragma once
#include "System_BuildConfig.h"
#include "System_User.h"
#include "QuickNote_Policy.h"
#include <cstddef>
#include <cstdint>
#include <ctime>

#if ENABLE_QUICKNOTE
// Private QuickNote files: /<root>/u<accountId>/notes/<stamp>-<id16>.txt, root
// /sd/stt when the card is writable, else /stt (the TranscriptSession rule).
// They sit inside the transcript tree, so every guarded VFS call here applies
// Transcript_PathPolicy: a note is open to the account that spoke it and to
// the unrestricted roles (superadmin, system) only - the same rights as a
// transcript. Nothing here logs, posts or returns note text except read()'s
// window, to its caller.
//
// Every call takes the filesystem lock (bounded at 1 s) and can block on
// flash/SD: run it on a worker or cmd_exec, never in a critical section,
// display task, BLE owner task or audio callback. The legacy QuickNote and
// native G2 workers share this store; the filesystem lock serializes complete
// writes and temporary-file sweeps.
namespace QuickNoteStore {
// Device-wide G2 directories on the currently writable tier. Called after a
// paired G2 connects and after card availability changes, outside BLE callbacks.
// cleanInterrupted sweeps only this native folder, under the same lock as saves.
bool ensureG2Folders(const AuthContext& ctx, bool sd, const char** why, bool cleanInterrupted = false);
constexpr uint64_t kReserveBytes = 100 * 1024;  // free space a note never eats into
constexpr size_t kPageRows = 8;
constexpr size_t kMaxNames = 500;               // newest names a listing considers
constexpr size_t kReadWindow = 512;

struct Row {
  char path[QuickNotePolicy::kPathMax];
  // From the file's header; when that does not parse (headerValid false) only
  // id and created are set, from the file name.
  QuickNotePolicy::NoteHeader header;
  bool headerValid;
};
struct Page {
  Row rows[kPageRows];
  size_t count;
  uint32_t total;       // note files in the folder
  uint32_t nextOffset;
  bool more;
  bool capped;          // over kMaxNames: only the newest kMaxNames are listable
};

// "yyyymmddThhmmssZ" (UTC) when the clock is synced, else "boot". `out` needs
// QuickNotePolicy::kStampMax + 1 bytes.
void stamp(char* out, size_t cap);
// `t` as "yyyymmddThhmmssZ" (UTC); false (and "") unless it is a plausible
// wall-clock time that fits `out`.
bool stampAt(time_t t, char* out, size_t cap);
// The boot counter (gBootCounter, "boot #N" in the log; 0 if NVS failed) and
// whole seconds since this boot, on a clock that never wraps. Either may be null.
void bootClock(uint32_t* boot, uint32_t* uptimeS);
// The stamp of the moment `uptimeS` seconds into this boot, once the clock is
// set (false before). The boot's wall-clock start is fixed by the first call
// after that, so a repeated call names the same file. QuickNote worker only.
bool stampSince(uint32_t uptimeS, char* out, size_t cap);

// Room for one more maximal note on the tier a save would use now, above the
// reserve. Checked before a recording starts so it never starts without room.
bool hasRoom(const char** why);

// Saves one note for `user`, the named owner of the live source/epoch that
// started it. The caller sets header.id/trigger/mic/durationMs; an empty
// header.created is stamped now, with header.boot/uptimeS (bootClock);
// header.title and header.words are derived from `text` here (and left in
// `header` for the caller). Writes <name>.tmp, renames it into place and
// verifies the length; on failure no file is left. False with a short reason
// in *why (never note text).
bool create(CommandSource source, TransportSessionEpoch epoch, const char* user,
            QuickNotePolicy::NoteHeader& header, const char* text, size_t length,
            char* pathOut, size_t pathCap, const char** why, bool nativeG2 = false);

// Native G2 notes use device-wide /[sd/]g2/quicknotes, created lazily.
// The native adapter still supplies a live paired-session identity.

// Gives a note saved before the clock was set its real time: rewrites `path`,
// a boot-<id>.txt in `user`'s own notes folder, as <stamp>-<id>.txt with
// Created: <stamp> (Boot, Uptime-s, the other fields and the text unchanged),
// through <stamp>-<id>.tmp, and only once that copy is verified deletes the
// boot file. Runs as `user` (the owner's guarded context, as create does); the
// owner's session need not be live, but the account must still be the one the
// folder names. If the delete fails after the rename, both files remain and
// the same call again only deletes the boot copy (a power cut there leaves
// both for good: nothing is lost). False with a short reason (never note
// text); *retry is set true when a later call might succeed.
bool restamp(CommandSource source, const char* user, const char* path, const char* stamp,
             char* pathOut, size_t pathCap, bool* retry, const char** why);

// One page of `owner`'s notes on one tier, newest name first; owner 0 means
// the caller's own account. Reads only the visible rows' headers.
bool list(const AuthContext& ctx, uint32_t owner, bool sd, uint32_t offset,
          Page* out, const char** why, bool nativeG2 = false);

// A kReadWindow window of a note file from `offset`, never ending inside a
// UTF-8 character unless that is the end of the file. `out` needs
// kReadWindow + 1 bytes; the caller clears it after use.
bool read(const AuthContext& ctx, const char* path, uint32_t offset, char* out,
          size_t cap, size_t* got, uint32_t* total, const char** why);

// Deletes one note file.
bool remove(const AuthContext& ctx, const char* path, const char** why);

// Removes the in-progress files (<stamp>-<id>.tmp) that saves interrupted by a
// power cut, crash or failed cleanup left in every account's notes folder, on
// flash and, when it is writable, the card: a save that never finished leaves
// neither file (create's contract), and the header carries no length that
// could prove a .tmp complete. Names only: no file is opened for reading, and
// nothing but those names is removed. Runs as system confined to each tier's
// transcript root or native G2 quicknotes folder. The filesystem lock excludes
// concurrent writes from either worker.
// *removed (optional) counts the files removed. False with a short reason
// when a tier could not be walked or a file could not be removed; the rest
// are still tried.
bool sweep(uint32_t* removed, const char** why);
}  // namespace QuickNoteStore
#endif
