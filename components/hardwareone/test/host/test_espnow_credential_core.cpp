// ESP-NOW 1.0 step 9: the CMD / USER_SYNC credential block (E29) against the
// production header System_ESPNow_CredentialCore.h, as is.
//
// The cases the step names: a ':' in the password (the pre-1.0 colon split
// gave the wrong account and command), an empty password (parsed, then refused
// by the login check like any wrong password), a bond token (16 raw bytes, any
// value, NUL and ':' included). Also: short and garbled blocks, a NUL inside a
// field or the command, an empty command, unknown kinds, the field limits, and
// USER_SYNC's two blocks + flags.
#include "System_ESPNow_CredentialCore.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gFails = 0;
int gChecks = 0;

void check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) {
    ++gFails;
    std::printf("FAIL: %s\n", what);
  }
}

using Bytes = std::vector<uint8_t>;

const uint8_t* u8(const char* s) { return reinterpret_cast<const uint8_t*>(s); }

std::string field(const uint8_t* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

Bytes cmd(uint8_t kind, const char* user, const char* password, const char* command) {
  Bytes out(256);
  const size_t n = espnowcred::packCmd(out.data(), out.size(), kind, u8(user), strlen(user), u8(password),
                                       strlen(password), command, strlen(command));
  out.resize(n);
  return out;
}

void colonInPassword() {
  const Bytes p = cmd(espnowcred::kKindAccount, "admin", "a:b:c", "status");
  check(p.size() == 4 + 5 + 5 + 6, "C1 account block + command length");
  check(p[0] == 0 && p[1] == 5 && p[2] == 5 && p[3] == 0, "C1 header: kind 0, lengths, reserved 0");
  espnowcred::Credential c{};
  const char* text = nullptr;
  size_t textLen = 0;
  check(espnowcred::parseCmd(p.data(), p.size(), c, &text, &textLen) == espnowcred::Parse::Ok, "C1 parses");
  check(field(c.user, c.userLen) == "admin", "C1 user is the whole name");
  check(field(c.password, c.passwordLen) == "a:b:c", "C1 a password with ':' comes back whole");
  check(std::string(text, textLen) == "status", "C1 the command is not taken from the password");
  // A ':' in the user name and in the command are ordinary bytes too.
  const Bytes q = cmd(espnowcred::kKindAccount, "a:dmin", "pw", "espnowsend peer: hi");
  check(espnowcred::parseCmd(q.data(), q.size(), c, &text, &textLen) == espnowcred::Parse::Ok &&
            field(c.user, c.userLen) == "a:dmin" && std::string(text, textLen) == "espnowsend peer: hi",
        "C1 ':' in the user name and the command");
}

void emptyPassword() {
  const Bytes p = cmd(espnowcred::kKindAccount, "admin", "", "status");
  espnowcred::Credential c{};
  const char* text = nullptr;
  size_t textLen = 0;
  check(espnowcred::parseCmd(p.data(), p.size(), c, &text, &textLen) == espnowcred::Parse::Ok,
        "C2 an empty password parses (the login check refuses it)");
  check(c.passwordLen == 0 && field(c.user, c.userLen) == "admin" && std::string(text, textLen) == "status",
        "C2 empty password: user and command intact");
  char buf[32];
  check(espnowcred::copyField(c.password, c.passwordLen, buf, sizeof(buf)) && buf[0] == '\0',
        "C2 copyField gives an empty string");
  // Empty user name and password both.
  const Bytes q = cmd(espnowcred::kKindAccount, "", "", "x");
  check(q.size() == espnowcred::kMinCmdLen, "C2 the shortest CMD is the minimum length");
  check(espnowcred::parseCmd(q.data(), q.size(), c, &text, &textLen) == espnowcred::Parse::Ok, "C2 minimum parses");
}

void bondToken() {
  uint8_t token[16];
  for (int i = 0; i < 16; i++) token[i] = (uint8_t)(i * 17);   // includes 0x00 and 0x3A-like bytes
  token[3] = ':';
  Bytes p(256);
  const size_t n = espnowcred::packCmd(p.data(), p.size(), espnowcred::kKindBondToken, nullptr, 0, token, 16,
                                       "sensors", 7);
  p.resize(n);
  check(n == 4 + 16 + 7, "C3 bond token block length");
  check(p[0] == 1 && p[1] == 0 && p[2] == 16, "C3 header: kind 1, no user, 16-byte token");
  espnowcred::Credential c{};
  const char* text = nullptr;
  size_t textLen = 0;
  check(espnowcred::parseCmd(p.data(), p.size(), c, &text, &textLen) == espnowcred::Parse::Ok, "C3 parses");
  check(c.kind == espnowcred::kKindBondToken && c.passwordLen == 16 && memcmp(c.password, token, 16) == 0,
        "C3 the raw token (a NUL and ':' inside) comes back byte for byte");
  check(std::string(text, textLen) == "sensors", "C3 the command follows the token");
  // A token of another length, or with a user name, is refused.
  Bytes bad = p;
  bad[2] = 15;
  check(espnowcred::parseCmd(bad.data(), bad.size(), c, &text, &textLen) == espnowcred::Parse::BadToken,
        "C3 a 15-byte token is refused");
  check(espnowcred::packBlock(bad.data(), bad.size(), espnowcred::kKindBondToken, u8("u"), 1, token, 16) == 0,
        "C3 pack refuses a token block with a user name");
  check(espnowcred::packBlock(bad.data(), bad.size(), espnowcred::kKindBondToken, nullptr, 0, token, 15) == 0,
        "C3 pack refuses a short token");
}

void malformed() {
  espnowcred::Credential c{};
  const char* text = nullptr;
  size_t textLen = 0;
  const uint8_t three[3] = {0, 0, 0};
  check(espnowcred::parseCmd(three, 3, c, &text, &textLen) == espnowcred::Parse::Short, "M1 under the header");
  const uint8_t longer[6] = {0, 5, 5, 0, 'a', 'b'};
  check(espnowcred::parseCmd(longer, 6, c, &text, &textLen) == espnowcred::Parse::Short,
        "M2 lengths past the payload");
  const uint8_t noCmd[8] = {0, 2, 2, 0, 'u', 's', 'p', 'w'};
  check(espnowcred::parseCmd(noCmd, 8, c, &text, &textLen) == espnowcred::Parse::Short, "M3 no command");
  const uint8_t nulCmd[9] = {0, 2, 2, 0, 'u', 's', 'p', 'w', 0};
  check(espnowcred::parseCmd(nulCmd, 9, c, &text, &textLen) == espnowcred::Parse::Short,
        "M4 a lone trailing NUL is no command");
  const uint8_t trailing[11] = {0, 2, 2, 0, 'u', 's', 'p', 'w', 'l', 's', 0};
  check(espnowcred::parseCmd(trailing, 11, c, &text, &textLen) == espnowcred::Parse::Ok && textLen == 2,
        "M5 one trailing NUL is tolerated and not part of the command");
  const uint8_t inner[11] = {0, 2, 2, 0, 'u', 's', 'p', 'w', 'l', 0, 's'};
  check(espnowcred::parseCmd(inner, 11, c, &text, &textLen) == espnowcred::Parse::BadText,
        "M6 a NUL inside the command");
  const uint8_t nulPw[9] = {0, 2, 2, 0, 'u', 's', 'p', 0, 'x'};
  check(espnowcred::parseCmd(nulPw, 9, c, &text, &textLen) == espnowcred::Parse::BadText,
        "M7 a NUL inside the password");
  const uint8_t kind2[5] = {2, 0, 0, 0, 'x'};
  check(espnowcred::parseCmd(kind2, 5, c, &text, &textLen) == espnowcred::Parse::BadKind, "M8 unknown kind 2");
  const uint8_t reserved[5] = {0, 0, 0, 0xAA, 'x'};
  check(espnowcred::parseCmd(reserved, 5, c, &text, &textLen) == espnowcred::Parse::Ok,
        "M9 the reserved byte is ignored");
  // The pre-1.0 text form is not a block: "admin:pw:status" reads kind 'a' (97).
  const char* old = "admin:pw:status";
  check(espnowcred::parseCmd(u8(old), strlen(old), c, &text, &textLen) != espnowcred::Parse::Ok,
        "M10 the old colon text is refused");
}

void limits() {
  std::string longPw(255, 'p');
  Bytes out(600);
  check(espnowcred::packCmd(out.data(), out.size(), 0, u8("u"), 1, u8(longPw.c_str()), 255, "x", 1) ==
            4 + 1 + 255 + 1,
        "L1 a 255-byte password packs");
  std::string tooLong(256, 'p');
  check(espnowcred::packCmd(out.data(), out.size(), 0, u8("u"), 1, u8(tooLong.c_str()), 256, "x", 1) == 0,
        "L2 a 256-byte field is refused");
  check(espnowcred::packCmd(out.data(), 10, 0, u8("user"), 4, u8("pw"), 2, "status", 6) == 0,
        "L3 no room: refused, never cut");
  check(espnowcred::packCmd(out.data(), out.size(), 0, u8("u"), 1, u8("p"), 1, "", 0) == 0,
        "L4 an empty command is refused");
  const char nulUser[] = {'a', 0, 'b'};
  check(espnowcred::packBlock(out.data(), out.size(), 0, u8(nulUser), 3, u8("p"), 1) == 0,
        "L5 pack refuses a NUL in a field");
  char buf[32];
  std::string name31(31, 'n'), name32(32, 'n');
  check(espnowcred::copyField(u8(name31.c_str()), 31, buf, sizeof(buf)) && strlen(buf) == 31, "L6 31 bytes fit 32");
  check(!espnowcred::copyField(u8(name32.c_str()), 32, buf, sizeof(buf)) && buf[0] == '\0',
        "L7 32 bytes do not fit: refused, not cut");
}

void userSync() {
  Bytes out(256);
  size_t n = espnowcred::packUserSync(out.data(), out.size(), "root", "x:y", "alice", "pw:with:colons",
                                      espnowcred::kUserSyncFlagSourceAdmin);
  out.resize(n);
  check(n == (4 + 4 + 3) + (4 + 5 + 14) + 1, "U1 two blocks and the flags byte");
  espnowcred::Credential admin{}, account{};
  uint8_t flags = 0;
  check(espnowcred::parseUserSync(out.data(), out.size(), admin, account, flags) == espnowcred::Parse::Ok,
        "U1 parses");
  check(field(admin.user, admin.userLen) == "root" && field(admin.password, admin.passwordLen) == "x:y" &&
            field(account.user, account.userLen) == "alice" &&
            field(account.password, account.passwordLen) == "pw:with:colons" && flags == 1,
        "U1 both logins (with ':') and the flags come back");
  Bytes noFlags(out.begin(), out.end() - 1);
  check(espnowcred::parseUserSync(noFlags.data(), noFlags.size(), admin, account, flags) == espnowcred::Parse::Short,
        "U2 no flags byte");
  Bytes extra = out;
  extra.push_back(0xEE);
  check(espnowcred::parseUserSync(extra.data(), extra.size(), admin, account, flags) == espnowcred::Parse::Ok,
        "U3 bytes after the flags are ignored");
  Bytes token = out;
  token[0] = espnowcred::kKindBondToken;
  check(espnowcred::parseUserSync(token.data(), token.size(), admin, account, flags) != espnowcred::Parse::Ok,
        "U4 a bond token is not a USER_SYNC login");
  Bytes empty(64);
  n = espnowcred::packUserSync(empty.data(), empty.size(), "", "", "", "", 0);
  check(n == espnowcred::kMinUserSyncLen, "U5 the shortest USER_SYNC is the minimum length");
  // The old JSON form is not a block.
  const char* json = "{\"recv_admin_user\":\"root\"}";
  check(espnowcred::parseUserSync(u8(json), strlen(json), admin, account, flags) != espnowcred::Parse::Ok,
        "U6 the old JSON is refused");
}

}  // namespace

void wipes() {
  char a[8] = "secret1";
  uint8_t b[4] = {1, 2, 3, 4};
  {
    espnowcred::WipeOnExit w;
    w.p[0] = a; w.n[0] = sizeof(a);
    w.p[1] = b; w.n[1] = sizeof(b);
  }
  bool zero = true;
  for (char c : a) zero = zero && c == 0;
  for (uint8_t c : b) zero = zero && c == 0;
  check(zero, "W1 WipeOnExit clears its buffers when it leaves scope");
  espnowcred::wipe(nullptr, 5);
  check(true, "W2 wipe(nullptr) is harmless");
}

int main() {
  wipes();
  colonInPassword();
  emptyPassword();
  bondToken();
  malformed();
  limits();
  userSync();
  std::printf("%d/%d checks passed\n", gChecks - gFails, gChecks);
  return gFails ? 1 : 0;
}
