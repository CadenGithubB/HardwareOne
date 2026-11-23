#ifndef SYSTEM_CAPTURECRYPTO_H
#define SYSTEM_CAPTURECRYPTO_H

// ============================================================================
// At-rest sealing for capture files — docs/HEALTH_AT_REST_ENCRYPTION_PLAN.md
//
// Per-LINE AEAD (ChaCha20-Poly1305-IETF via libsodium, which is linked
// unconditionally — this module deliberately does NOT go through the
// System_ESPNow_Crypto wrappers so it exists in ESPNOW-less builds):
//
//   #HW1ENC v1 k1 id=<16 hex>\n              plaintext magic (the "mark")
//   <plaintext structural header line(s)>    schema, not data
//   ENC1:<b64(nonce12 || ct || tag16)>\n     one per data row
//
// Filenames never change; detection is by magic first line only. Per-line
// sealing keeps append/rotation/merge byte-semantics intact and bounds a
// torn tail write to one lost row.
//
// The mark names the key that sealed the file (B11, 2026-11-22): id= is
// captureCryptoKeyId in hex, a keyed one-way hash and never the key. Readers
// still detect the "#HW1ENC" prefix only. Every new sealed file gets the id
// (captureCryptoMarkLine); a writer on the card uses it to tell its own file
// from another device's, and a viewer to say whose file it is
// (System_SealedLogCore.h has the format rules, System_SealedLog.h the card
// log writer).
//
// Key: random 256-bit, NVS blob (namespace hw1cap / key k1) — NVS is not
// reachable through any file-serving surface. Held in internal DRAM only
// (PSRAM is probeable). This is RUNTIME protection; flash-dump protection
// arrives later with flash encryption (+ NVS encryption).
//
// Threading: seal/open/reveal share PSRAM scratch behind a mutex. Sealing is
// called from sensorLogTick (main loop) and, for logs on the card, from the
// log task; reveal from cmd_exec / httpd / UI paths.
// ============================================================================

#include <Arduino.h>
#include <FS.h>      // File — for the open-handle sniff
#include <stddef.h>
#include <stdint.h>

#include "System_SealedLogCore.h"  // the file classes and line kinds (pure)

// The mark without its key id: the stem every mark line starts with. Writers
// use captureCryptoMarkLine(), which adds " id=<16 hex>".
#define CAPCRYPT_MAGIC_LINE   "#HW1ENC v1 k1"
// A whole mark line with its id and NUL fits this.
#define CAPCRYPT_MARK_LINE_MAX 48
#define CAPCRYPT_MAGIC_PREFIX "#HW1ENC"
#define CAPCRYPT_ROW_PREFIX   "ENC1:"
// What a sealed row becomes when it cannot be opened (torn tail write, a file
// sealed by another device's key, or a row that was sealed for a file of
// another class and moved here). A malformed/truncated sealed line can be
// SHORTER than this marker; whole-text reveal reserves that expansion.
#define CAPCRYPT_UNDECRYPTABLE "[undecryptable row]"
// What a plain line becomes where only sealed rows belong (after the mark and
// its schema lines): it was not written by a sealed writer, so it is never
// shown as if it were a row. sealedlog::kUnsealedLine.
#define CAPCRYPT_UNSEALED "[unsealed line]"
// Longest plaintext row we seal — matches the 1024-byte row builders in
// System_SensorLogging (buf holds row + NUL).
#define CAPCRYPT_MAX_ROW      1023
// Worst-case sealed line: "ENC1:" + b64(12 + 1023 + 16) + NUL = 1414. Callers
// sizing a seal output buffer should use this.
#define CAPCRYPT_MAX_SEALED   1536

// True once the key is resident (does not mint). For status displays.
bool captureCryptoKeyReady();

// Load the key from NVS, minting + persisting a fresh one on first use.
// False = NVS unavailable/write failed — callers must fail CLOSED (never
// fall back to plaintext in a session that promised sealing).
bool captureCryptoEnsureKey();

// Derive a stable, device-local pseudonym without exposing the capture key or
// the source identifier. `domain` separates independent uses (for example,
// "r1-history-peer-v1"); `value` is never stored by this helper. The output is
// 32 lowercase hex characters plus NUL. Returns false on invalid input or key
// failure. Intended for encrypted-data filenames/indices that must not leak a
// reversible raw MAC or an unkeyed hash of it.
bool captureCryptoPseudonym(const char* domain, const char* value,
                            char* outHex, size_t outCap);

// ---- File catch-up: sealing on send (System_ESPNow_Catchup) -----------------
// The system log, saved transcripts and some sensor captures are plain at rest
// but leave the device sealed with this same key (owner decisions Q10/Q11,
// 2026-11-19). One chunk is one ESP-NOW frame's data:
//
//   out = nonce12 || ct[inLen] || tag16      (inLen + CAPCRYPT_CHUNK_OVERHEAD)
//
// ChaCha20-Poly1305-IETF with a random nonce (never derived from the offset)
// and the caller's additional data (espnowcatchup::buildSealAad binds the
// ledger, file and logical offset). Binary, no base64, no scratch: `in` and
// `out` are the caller's (PSRAM) buffers and must not overlap. Returns the
// bytes written, or -1 on a key failure, bad arguments or a small buffer.
// Callers fail CLOSED: a -1 never falls back to sending plaintext.
#define CAPCRYPT_CHUNK_OVERHEAD 28
int captureCryptoSealChunk(const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen,
                           uint8_t* out, size_t outCap);
// Opens one chunk sealed above (host tests, a later reveal tool): plaintext
// length, or -1 on a short input, a small buffer, a key failure or a tag
// mismatch (wrong AAD, wrong key, damaged bytes). `out` is zeroed on failure.
int captureCryptoOpenChunk(const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen,
                           uint8_t* out, size_t outCap);
// The key's public id: the first 8 bytes of
// captureCryptoPseudonym("hw1-capture-key-id-v1", "k1"). It tells which key
// sealed something, never the key itself (a keyed one-way hash). Shown by
// espnowcatchup; the health key share (package 7) carries it. False on a key
// failure. Mints the key on first use, like every other sealing call.
bool captureCryptoKeyId(uint8_t out[8]);
// The same id as 16 lower-case hex digits and a NUL.
bool captureCryptoKeyIdHex(char out[17]);
// The mark line of a new sealed file, "#HW1ENC v1 k1 id=<16 hex>", without a
// line end. Returns its length, 0 on a key failure or a small buffer (callers
// fail closed: no mark, no sealed file).
size_t captureCryptoMarkLine(char* out, size_t outCap);

// Whose mark a file starts with, judged from its first bytes (the first line
// with its line end; CAPCRYPT_MARK_LINE_MAX bytes are enough).
enum class CaptureMarkOwner : uint8_t {
  None,     // no mark: a plain file
  Ours,     // sealed with this device's key
  Other,    // sealed with another device's key; its id is in keyIdOut
  Unknown,  // a mark without a key id, a damaged one, or this device's key did not load
};
CaptureMarkOwner captureCryptoMarkOwner(const char* head, size_t headLen, char keyIdOut[17] = nullptr);
// What a viewer shows once for a file another device sealed, in place of its
// rows: "[sealed by device <id>: its rows can only be read on that device]".
// Returns the length written (NUL-terminated), 0 when `out` is too small.
#define CAPCRYPT_FOREIGN_NOTICE_MAX 96
size_t captureCryptoForeignNotice(const char* keyIdHex, char* out, size_t outCap);

// Rows are bound to the class of the file they are in (B11 review,
// 2026-11-23; sealedlog::classOf in System_SealedLogCore.h): the system logs,
// the boot debug files, the capture tree, one account's transcript folder, or
// the exact path for anything else. Every call below that seals or opens a row
// takes `path`, the NORMALIZED path of the file the row is written to or read
// from (normalizeFsPath; with or without "/sd": a file and its card twin are
// one class). A row read at a path of another class does not open, so the
// device cannot be made to reveal a file through another path's permissions.

// Seal one row (no trailing newline in/out). Returns bytes written to out
// (NUL-terminated), or -1 on key failure / oversize row / small buffer / a
// path without a class.
int captureCryptoSealLine(const char* path, const char* in, size_t inLen, char* out, size_t outCap);

// Open one "ENC1:..." line (no trailing newline). Returns plaintext length
// (NUL-terminated into out), or -1 on bad prefix/b64/tag — a torn tail line,
// a file sealed by another device's key, or a row of another class.
int captureCryptoOpenLine(const char* path, const char* line, size_t lineLen, char* out, size_t outCap);

bool captureCryptoIsMagicLine(const char* line);   // starts with #HW1ENC
bool captureCryptoIsSealedRow(const char* line);   // starts with ENC1:

// Peek an already-open (caller-authorized) file: true if it begins with the
// magic. Restores the read position to 0.
bool captureCryptoLooksSealed(File& f);

// In-place reveal of one line (no trailing newline): a sealed row becomes its
// plaintext, an unopenable sealed row becomes "[undecryptable row]",
// anything else is untouched. Returns
// true if the line was a sealed row.
bool captureCryptoRevealLine(const char* path, String& line);

// The same for a reader that walks a file line by line (the web viewer, the
// track loader, the plaintext export): `state` starts as a default
// sealedlog::LineState at the file's first line (sealedlog::midFileState()
// for a read that starts inside a marked file) and says what each line is. A
// row is revealed; a plain line where only rows belong becomes
// "[unsealed line]" (kind Unsealed: a parser skips it); the mark, its schema
// lines, blank lines and every line of an unmarked file are left as stored.
sealedlog::Line captureCryptoRevealFileLine(const char* path, sealedlog::LineState& state, String& line);

// In-place reveal of a whole line-oriented blob that STARTS with the magic
// line (non-marked text returns 0 untouched — stray ENC1 text in ordinary
// files is not interpreted). Returns the number of sealed rows encountered.
// A blob whose mark names another device's key shows the notice above once
// where its first sealed row was and drops the other sealed rows, instead of
// a column of "[undecryptable row]". A plain line after the mark's schema
// lines is shown as "[unsealed line]".
// This is the hook for fileview / G2 / OLED viewers, applied AFTER their
// existing auth gates: presentation surfaces reveal, byte surfaces never do.
size_t captureCryptoRevealText(const char* path, String& text);

// Conservative capacity INCLUDING NUL needed by the bounded reveal overload.
// Accounts for short damaged rows expanding into the failure marker and short
// plain lines into theirs. Returns 0 for invalid input/size overflow;
// non-marked text needs only length + 1. `afterHeader` as below.
size_t captureCryptoRevealCapacity(const char* text, size_t length, bool afterHeader = false);

// Checked, caller-owned presentation buffer. On insufficient capacity returns
// false without modifying bytes or length; on success updates length and NUL.
// The caller may reserve captureCryptoRevealCapacity() before calling. Never
// used by raw download/transfer paths. The optional row count is reset on entry.
// `afterHeader`: the text is a later part of a file with the file's mark line
// put in front (fileview's pages): no plain line of it counts as schema.
bool captureCryptoRevealText(const char* path, char* text, size_t& length, size_t capacity,
                             size_t* sealedRows = nullptr, bool afterHeader = false);

#endif  // SYSTEM_CAPTURECRYPTO_H
