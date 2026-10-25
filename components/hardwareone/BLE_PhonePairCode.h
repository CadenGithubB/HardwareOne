#pragma once
// Phone link v2 pairing code (`phone pair new`): the device-made phone secret that
// offers the console. 16 symbols of 0123456789ABCDEFGHJKMNPQRSTVWXYZ (value =
// index): 15 random symbols (75 bits) and one check symbol over GF(2^5), modulus
// x^5+x^2+1 (0x25), alpha = x:
//   check = sum_{i=0..14} alpha^(i+1) * v_i   (GF add = XOR)
// Every weight (alpha^1..alpha^15 and the check's own 1) is distinct and nonzero
// (alpha has order 31), so any single wrong symbol and any swap of two different
// symbols changes the sum: both are always rejected.
// Canonical form = the 16 uppercase symbols; it is what is stored and fed to
// PBKDF2, byte for byte. Display form XXXX-XXXX-XXXX-XXXX. Entry is forgiving:
// outer ASCII whitespace trimmed, case folded, '-' and ' ' dropped anywhere,
// I and L read as 1, O as 0; any other byte (U included) means "not a code".
// Pure (no BLE, OS or Arduino) and allocation-free: the P4, the fixtures and the
// host tests share it. Buffers holding a code are wiped here; callers wipe theirs.
#include <cstddef>
#include <cstdint>

namespace hw1code {
inline constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kSymbols = 16, kDataSymbols = 15, kDisplayLen = 19;
constexpr uint8_t kModulus = 0x25;  // x^5 + x^2 + 1, primitive
static_assert(sizeof(kAlphabet) == 33, "32 symbols");

inline void wipe(void* p, size_t n) { volatile uint8_t* q = static_cast<volatile uint8_t*>(p); while (n--) *q++ = 0; }
// x*a and a*b in GF(2^5); inputs use their low 5 bits.
inline uint8_t gfMulX(uint8_t a) { a = uint8_t((a & 31u) << 1); return (a & 0x20u) ? uint8_t(a ^ kModulus) : a; }
inline uint8_t gfMul(uint8_t a, uint8_t b) {
  uint8_t r = 0; a &= 31u; b &= 31u;
  while (b) { if (b & 1u) r ^= a; a = gfMulX(a); b >>= 1; }
  return r;
}
// The check symbol's value (0..31) for 15 data values (low 5 bits used).
inline uint8_t checkValue(const uint8_t values[15]) {
  uint8_t weight = 1, check = 0;
  for (size_t i = 0; i < kDataSymbols; ++i) { weight = gfMulX(weight); check ^= gfMul(weight, values[i]); }
  return check;
}
// One entry byte's value after normalisation (case fold, I/L -> 1, O -> 0), or
// -1 for anything that is not a symbol (separators included).
inline int valueOf(unsigned char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
  if (c == 'I' || c == 'L') return 1;
  if (c == 'O') return 0;
  for (int v = 0; v < 32; ++v) if (kAlphabet[v] == static_cast<char>(c)) return v;
  return -1;
}
inline bool outerSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// True when text[0..len) is a valid code; then canonical (if not null) holds the
// 16 symbols plus NUL. On false a non-null canonical is zeroed. nullptr text is
// "not a code"; canonical == nullptr only validates.
inline bool parse(const char* text, size_t len, char canonical[17]) {
  uint8_t v[kSymbols] = {};
  size_t n = 0;
  bool ok = text != nullptr;
  if (ok) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (len && outerSpace(p[0])) { ++p; --len; }
    while (len && outerSpace(p[len - 1])) --len;
    for (size_t i = 0; ok && i < len; ++i) {
      if (p[i] == '-' || p[i] == ' ') continue;
      const int value = valueOf(p[i]);
      if (value < 0 || n == kSymbols) ok = false;
      else v[n++] = static_cast<uint8_t>(value);
    }
  }
  ok = ok && n == kSymbols && checkValue(v) == v[kDataSymbols];
  if (canonical) {
    for (size_t i = 0; i < kSymbols; ++i) canonical[i] = ok ? kAlphabet[v[i]] : '\0';
    canonical[kSymbols] = '\0';
  }
  wipe(v, sizeof(v));
  return ok;
}
inline bool isCode(const char* text, size_t len) { return parse(text, len, nullptr); }

// The canonical code for 15 data values (low 5 bits used) plus their check.
inline void fromValues(const uint8_t values[15], char canonical[17]) {
  for (size_t i = 0; i < kDataSymbols; ++i) canonical[i] = kAlphabet[values[i] & 31u];
  canonical[kDataSymbols] = kAlphabet[checkValue(values)];
  canonical[kSymbols] = '\0';
}
// Generation from three esp_random() words: value i = bits 5*(i%6).. of word i/6
// (30 bits of words 0 and 1, 15 of word 2), then the check.
inline void fromRandom(const uint32_t words[3], char canonical[17]) {
  uint8_t v[kDataSymbols];
  for (size_t i = 0; i < kDataSymbols; ++i) v[i] = static_cast<uint8_t>((words[i / 6] >> (5 * (i % 6))) & 31u);
  fromValues(v, canonical);
  wipe(v, sizeof(v));
}
// XXXX-XXXX-XXXX-XXXX plus NUL, from the 16 canonical symbols.
inline void display(const char canonical[16], char out[20]) {
  size_t o = 0;
  for (size_t i = 0; i < kSymbols; ++i) { if (i && i % 4 == 0) out[o++] = '-'; out[o++] = canonical[i]; }
  out[o] = '\0';
}
}  // namespace hw1code
