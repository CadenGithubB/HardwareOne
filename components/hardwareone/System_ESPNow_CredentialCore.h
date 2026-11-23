#pragma once
// The credential block of a remote command (CMD, 50) and of USER_SYNC (56):
// ESP-NOW 1.0 step 9 (DESIGN 5.3, owner answer E29). Pure: no radio, no
// account store, so the host tests run this file as is.
//
// Credentials stay in the frame (D2); both frames are sealed in a session with
// a paired peer (row PAIRED + SESSION_ENC). Before 1.0 a CMD was the text
// "user:pass:command" split on its first two colons, so a password with a ':'
// in it could not be used, and USER_SYNC was JSON with snake-case keys. The
// block carries lengths instead of separators:
//
//   0  kind         0 account (user name + password), 1 bond token
//   1  userLen      bytes of the user name that follow (0..255)
//   2  passwordLen  bytes of the password that follow (0..255)
//   3  reserved     sent 0, ignored
//   4  user[userLen], then password[passwordLen]; no NUL on the wire
//
// Kind 0: the user name and the password may hold any byte but NUL (a ':' is
// an ordinary byte); either may be empty here, and the receiver's login check
// refuses an empty one like any wrong password. Kind 1: no user name, and the
// "password" is the bond session token itself, 16 raw bytes (any value).
//
//   CMD       = block, then the command text to the end of the payload (one
//               trailing NUL is tolerated and not part of it; the text may not
//               be empty and may not hold a NUL).
//   USER_SYNC = block (the receiving device's admin, kind 0), block (the account
//               to create, kind 0), then one flags byte (bit 0: the account is
//               an admin on the sender; bits 1-7 sent 0, ignored). Bytes after
//               the flags are ignored.
//
// A payload that breaks any of these rules is answered kBadRequest on its ACK
// (the parse runs in the receive handler, before the frame is accepted).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espnowcred {

constexpr uint8_t kKindAccount = 0;
constexpr uint8_t kKindBondToken = 1;
constexpr size_t kBlockHeaderLen = 4;
constexpr size_t kFieldMax = 255;          // a u8 length
constexpr size_t kBondTokenLen = 16;
constexpr uint8_t kUserSyncFlagSourceAdmin = 0x01;
// The shortest valid frames: an account block with empty fields plus one
// command byte; two empty account blocks plus the flags byte.
constexpr size_t kMinCmdLen = kBlockHeaderLen + 1;
constexpr size_t kMinUserSyncLen = 2 * kBlockHeaderLen + 1;

enum class Parse : uint8_t {
  Ok,
  Short,       // the payload ends inside the block, or before the command / flags
  BadKind,     // a kind this firmware does not know, or one the frame does not take
  BadText,     // a NUL inside the user name, password or command; an empty command
  BadToken,    // kind 1 with a user name or a token that is not 16 bytes
};

inline const char* parseName(Parse p) {
  switch (p) {
    case Parse::Ok:       return "ok";
    case Parse::Short:    return "short";
    case Parse::BadKind:  return "bad_kind";
    case Parse::BadText:  return "bad_text";
    case Parse::BadToken: return "bad_token";
  }
  return "bad";
}

// One block as read: pointers into the payload (not NUL-terminated).
struct Credential {
  uint8_t kind;
  const uint8_t* user;
  uint8_t userLen;
  const uint8_t* password;
  uint8_t passwordLen;
};

inline size_t blockLen(size_t userLen, size_t passwordLen) {
  return kBlockHeaderLen + userLen + passwordLen;
}

inline bool hasNul(const uint8_t* p, size_t n) {
  return n && memchr(p, 0, n) != nullptr;
}

// Writes one block at `out`. Returns the bytes written, or 0 when it does not
// fit `cap`, a field is over 255 bytes, a kind 0 field holds a NUL, or a kind 1
// block is not an empty user name with a 16-byte token.
inline size_t packBlock(uint8_t* out, size_t cap, uint8_t kind, const uint8_t* user, size_t userLen,
                        const uint8_t* password, size_t passwordLen) {
  if (!out || userLen > kFieldMax || passwordLen > kFieldMax) return 0;
  if ((userLen && !user) || (passwordLen && !password)) return 0;
  if (kind == kKindAccount) {
    if (hasNul(user, userLen) || hasNul(password, passwordLen)) return 0;
  } else if (kind == kKindBondToken) {
    if (userLen != 0 || passwordLen != kBondTokenLen) return 0;
  } else {
    return 0;
  }
  const size_t n = blockLen(userLen, passwordLen);
  if (n > cap) return 0;
  out[0] = kind;
  out[1] = (uint8_t)userLen;
  out[2] = (uint8_t)passwordLen;
  out[3] = 0;
  if (userLen) memcpy(out + kBlockHeaderLen, user, userLen);
  if (passwordLen) memcpy(out + kBlockHeaderLen + userLen, password, passwordLen);
  return n;
}

// Reads one block from `in`. `used` gets its length on Ok.
inline Parse parseBlock(const uint8_t* in, size_t len, Credential& out, size_t& used) {
  used = 0;
  if (!in || len < kBlockHeaderLen) return Parse::Short;
  const uint8_t kind = in[0];
  const size_t n = blockLen(in[1], in[2]);
  if (len < n) return Parse::Short;
  out.kind = kind;
  out.userLen = in[1];
  out.passwordLen = in[2];
  out.user = in + kBlockHeaderLen;
  out.password = in + kBlockHeaderLen + in[1];
  if (kind == kKindAccount) {
    if (hasNul(out.user, out.userLen) || hasNul(out.password, out.passwordLen)) return Parse::BadText;
  } else if (kind == kKindBondToken) {
    if (out.userLen != 0 || out.passwordLen != kBondTokenLen) return Parse::BadToken;
  } else {
    return Parse::BadKind;
  }
  used = n;
  return Parse::Ok;
}

// A CMD payload: the block, then the command text.
inline Parse parseCmd(const uint8_t* in, size_t len, Credential& cred, const char** cmd, size_t* cmdLen) {
  size_t used = 0;
  const Parse p = parseBlock(in, len, cred, used);
  if (p != Parse::Ok) return p;
  size_t n = len - used;
  const uint8_t* text = in + used;
  if (n && text[n - 1] == 0) n--;      // one trailing NUL is not part of the command
  if (n == 0) return Parse::Short;
  if (hasNul(text, n)) return Parse::BadText;
  if (cmd) *cmd = reinterpret_cast<const char*>(text);
  if (cmdLen) *cmdLen = n;
  return Parse::Ok;
}

// Builds a CMD payload. Returns its length, or 0 when it does not fit `cap` or
// a part breaks the rules above (an empty command included).
inline size_t packCmd(uint8_t* out, size_t cap, uint8_t kind, const uint8_t* user, size_t userLen,
                      const uint8_t* password, size_t passwordLen, const char* cmd, size_t cmdLen) {
  if (!cmd || cmdLen == 0 || hasNul(reinterpret_cast<const uint8_t*>(cmd), cmdLen)) return 0;
  const size_t n = packBlock(out, cap, kind, user, userLen, password, passwordLen);
  if (n == 0 || cmdLen > cap - n) return 0;
  memcpy(out + n, cmd, cmdLen);
  return n + cmdLen;
}

// A USER_SYNC payload: the receiving admin, the account, the flags byte. Both
// blocks must be kind 0.
inline Parse parseUserSync(const uint8_t* in, size_t len, Credential& admin, Credential& account, uint8_t& flags) {
  size_t usedA = 0, usedB = 0;
  Parse p = parseBlock(in, len, admin, usedA);
  if (p != Parse::Ok) return p;
  p = parseBlock(in + usedA, len - usedA, account, usedB);
  if (p != Parse::Ok) return p;
  if (admin.kind != kKindAccount || account.kind != kKindAccount) return Parse::BadKind;
  if (len - usedA - usedB < 1) return Parse::Short;
  flags = in[usedA + usedB];
  return Parse::Ok;
}

inline size_t packUserSync(uint8_t* out, size_t cap, const char* adminUser, const char* adminPassword,
                           const char* user, const char* password, uint8_t flags) {
  if (!adminUser || !adminPassword || !user || !password) return 0;
  const size_t a = packBlock(out, cap, kKindAccount, reinterpret_cast<const uint8_t*>(adminUser), strlen(adminUser),
                             reinterpret_cast<const uint8_t*>(adminPassword), strlen(adminPassword));
  if (a == 0) return 0;
  const size_t b = packBlock(out + a, cap - a, kKindAccount, reinterpret_cast<const uint8_t*>(user), strlen(user),
                             reinterpret_cast<const uint8_t*>(password), strlen(password));
  if (b == 0 || cap - a - b < 1) return 0;
  out[a + b] = flags;
  return a + b + 1;
}

// Copies a field into a NUL-terminated buffer. False (and `out` empty) when it
// does not fit: a name or password is refused, never cut (the first 31 bytes
// of a longer name can be another account's whole name).
inline bool copyField(const uint8_t* p, size_t n, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  if (n >= cap) {
    out[0] = '\0';
    return false;
  }
  if (n) memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

}  // namespace espnowcred
