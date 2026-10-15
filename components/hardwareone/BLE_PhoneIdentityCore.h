#pragma once
#include <cstdint>
#include <cstring>

namespace hw1phone {
// Persist only public/static identity addresses. Random private addresses are
// observations; resolving them is the controller's job using the bonded IRK.
inline bool validIdentity(const uint8_t* address, uint8_t type) {
  if (!address || type > 1 || (type == 1 && (address[0] & 0xc0) != 0xc0)) return false;
  bool nonzero = false, nonff = false;
  for (unsigned i = 0; i < 6; ++i) {
    const uint8_t mask = type == 1 && i == 0 ? 0x3f : 0xff;
    nonzero |= (address[i] & mask) != 0; nonff |= (address[i] & mask) != mask;
  }
  return nonzero && nonff;
}
inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool parseIdentity(const char* text, uint8_t type, uint8_t out[6]) {
  if (!text || !out || std::strlen(text) != 17) return false;
  uint8_t address[6];
  for (unsigned i = 0; i < 6; ++i) {
    int hi = hexDigit(text[3*i]), lo = hexDigit(text[3*i+1]);
    if (hi < 0 || lo < 0 || (i < 5 && text[3*i+2] != ':')) return false;
    address[i] = uint8_t(hi * 16 + lo);
  }
  if (!validIdentity(address, type)) return false;
  std::memcpy(out, address, 6); return true;
}
inline bool formatIdentity(const uint8_t* address, uint8_t type, char out[18]) {
  if (!out || !validIdentity(address, type)) return false;
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 6; ++i) {
    out[3*i] = digits[address[i] >> 4]; out[3*i+1] = digits[address[i] & 15];
    out[3*i+2] = i == 5 ? '\0' : ':';
  }
  return true;
}
// Semantic inputs supplied by the native adapter from public stack fields.
// A persisted identity alone is not proof of a usable authenticated bond.
inline bool strongBond(bool authenticatedSc, bool peerEncryptionKey,
                       bool peerIdentityKey, uint8_t keyBytes,
                       bool authenticatedKey, const uint8_t* irk) {
  if (!authenticatedSc || !peerEncryptionKey || !peerIdentityKey ||
      keyBytes != 16 || !authenticatedKey || !irk) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 16; ++i) nonzero |= irk[i] != 0;
  return nonzero;
}
}
