#ifndef SYSTEM_CAPTURECRYPTO_H
#define SYSTEM_CAPTURECRYPTO_H

// ============================================================================
// At-rest sealing for capture files — docs/HEALTH_AT_REST_ENCRYPTION_PLAN.md
//
// Per-LINE AEAD (ChaCha20-Poly1305-IETF via libsodium, which is linked
// unconditionally — this module deliberately does NOT go through the
// System_ESPNow_Crypto wrappers so it exists in ESPNOW-less builds):
//
//   #HW1ENC v1 k1\n                          plaintext magic (the "mark")
//   <plaintext structural header line(s)>    schema, not data
//   ENC1:<b64(nonce12 || ct || tag16)>\n     one per data row
//
// Filenames never change; detection is by magic first line only. Per-line
// sealing keeps append/rotation/merge byte-semantics intact and bounds a
// torn tail write to one lost row.
//
// Key: random 256-bit, NVS blob (namespace hw1cap / key k1) — NVS is not
// reachable through any file-serving surface. Held in internal DRAM only
// (PSRAM is probeable). This is RUNTIME protection; flash-dump protection
// arrives later with flash encryption (+ NVS encryption).
//
// Threading: seal/open/reveal share PSRAM scratch behind a mutex. Sealing is
// called from sensorLogTick (main loop); reveal from cmd_exec / httpd / UI
// paths — all human-paced, contention is irrelevant.
// ============================================================================

#include <Arduino.h>
#include <FS.h>      // File — for the open-handle sniff
#include <stddef.h>
#include <stdint.h>

#define CAPCRYPT_MAGIC_LINE   "#HW1ENC v1 k1"
#define CAPCRYPT_MAGIC_PREFIX "#HW1ENC"
#define CAPCRYPT_ROW_PREFIX   "ENC1:"
// What a sealed row becomes when it cannot be opened (torn tail write, or a
// file sealed by another device's key). Shorter than any sealed line, so
// in-place reveal can always substitute it.
#define CAPCRYPT_UNDECRYPTABLE "[undecryptable row]"
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

// Seal one row (no trailing newline in/out). Returns bytes written to out
// (NUL-terminated), or -1 on key failure / oversize row / small buffer.
int captureCryptoSealLine(const char* in, size_t inLen, char* out, size_t outCap);

// Open one "ENC1:..." line (no trailing newline). Returns plaintext length
// (NUL-terminated into out), or -1 on bad prefix/b64/tag — a torn tail line
// or a file sealed by another device's key.
int captureCryptoOpenLine(const char* line, size_t lineLen, char* out, size_t outCap);

bool captureCryptoIsMagicLine(const char* line);   // starts with #HW1ENC
bool captureCryptoIsSealedRow(const char* line);   // starts with ENC1:

// Peek an already-open (caller-authorized) file: true if it begins with the
// magic. Restores the read position to 0.
bool captureCryptoLooksSealed(File& f);

// In-place reveal of one line (no trailing newline): a sealed row becomes its
// plaintext, an unopenable sealed row becomes "[undecryptable row]" (both are
// always shorter than the sealed form), anything else is untouched. Returns
// true if the line was a sealed row.
bool captureCryptoRevealLine(String& line);

// In-place reveal of a whole line-oriented blob that STARTS with the magic
// line (non-marked text returns 0 untouched — stray ENC1 text in ordinary
// files is not interpreted). Returns the number of sealed rows encountered.
// This is the hook for fileview / G2 / OLED viewers, applied AFTER their
// existing auth gates: presentation surfaces reveal, byte surfaces never do.
size_t captureCryptoRevealText(String& text);

#endif  // SYSTEM_CAPTURECRYPTO_H
