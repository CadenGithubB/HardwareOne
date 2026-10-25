// Phone link v2 pairing code (BLE_PhonePairCode.h) as shipped: the GF(2^5) field
// and its check symbol, every single-symbol substitution and every swap of two
// symbols rejected (exhaustive over positions and values for a sample of codes),
// entry normalisation, generation and the display form. Prints VEC lines for the
// driver's independent recomputation.
#include "BLE_PhonePairCode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
using namespace hw1code;

static unsigned long checks = 0;
#define REQUIRE(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "%s:%d: REQUIRE(%s)\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

struct Rng {
  uint64_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 32); }
};

// parse() over exactly the text's bytes (ASan: nothing past len is read). Checks
// the output contract both ways and that isCode() agrees.
static bool parses(const std::string& text, std::string* canonical = nullptr) {
  std::unique_ptr<char[]> exact(new char[text.size() ? text.size() : 1]);
  if (!text.empty()) std::memcpy(exact.get(), text.data(), text.size());
  char out[17]; std::memset(out, 'Q', sizeof(out));
  const bool ok = parse(exact.get(), text.size(), out);
  if (ok) {
    REQUIRE(out[16] == '\0' && std::strlen(out) == 16);
    for (int i = 0; i < 16; ++i) REQUIRE(valueOf(static_cast<unsigned char>(out[i])) >= 0 && kAlphabet[valueOf(static_cast<unsigned char>(out[i]))] == out[i]);
    if (canonical) *canonical = out;
  } else {
    for (char c : out) REQUIRE(c == '\0');  // a failed parse leaves nothing of the input behind
  }
  REQUIRE(isCode(exact.get(), text.size()) == ok);
  return ok;
}
static std::string codeOf(const uint8_t values[15]) { char c[17]; fromValues(values, c); return c; }
static std::string displayOf(const std::string& canonical) {
  char d[20]; std::memset(d, 'Q', sizeof(d)); display(canonical.data(), d);
  REQUIRE(std::strlen(d) == kDisplayLen); return d;
}

static void alphabet() {
  REQUIRE(std::strlen(kAlphabet) == 32 && kSymbols == 16 && kDataSymbols == 15 && kDisplayLen == 19);
  for (int v = 0; v < 32; ++v) {
    REQUIRE(valueOf(static_cast<unsigned char>(kAlphabet[v])) == v);
    for (int w = v + 1; w < 32; ++w) REQUIRE(kAlphabet[v] != kAlphabet[w]);
  }
  for (char excluded : {'I', 'L', 'O', 'U'}) REQUIRE(std::strchr(kAlphabet, excluded) == nullptr);
  REQUIRE(valueOf('I') == 1 && valueOf('i') == 1 && valueOf('L') == 1 && valueOf('l') == 1);
  REQUIRE(valueOf('O') == 0 && valueOf('o') == 0 && valueOf('U') == -1 && valueOf('u') == -1);
  REQUIRE(valueOf('-') == -1 && valueOf(' ') == -1 && valueOf(0) == -1 && valueOf(0x80) == -1 && valueOf(0xFF) == -1);
  int symbols = 0;
  for (int c = 0; c < 256; ++c) {
    const int v = valueOf(static_cast<unsigned char>(c));
    if (v < 0) continue;
    ++symbols;
    const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
    REQUIRE(upper || lower || digit);
    if (lower) REQUIRE(valueOf(static_cast<unsigned char>(c - 'a' + 'A')) == v);
  }
  REQUIRE(symbols == 10 + 2 * 22 + 6);  // digits, both cases of the 22 letters, I/L/O either case
}

static void field() {
  REQUIRE(kModulus == 0x25);
  uint8_t power = 1, seen[32] = {};
  for (int k = 1; k <= 31; ++k) {
    power = gfMulX(power);
    REQUIRE(power != 0 && power < 32);
    REQUIRE((power == 1) == (k == 31));  // alpha = x has order 31: x^5+x^2+1 is primitive
    REQUIRE(!seen[power]); seen[power] = 1;
  }
  for (unsigned a = 0; a < 32; ++a) {
    REQUIRE(gfMulX(uint8_t(a)) == gfMul(uint8_t(a), 2) && gfMul(uint8_t(a), 1) == a && gfMul(uint8_t(a), 0) == 0);
    REQUIRE(gfMul(uint8_t(a | 0xE0), 3) == gfMul(uint8_t(a), 3));  // low 5 bits only
    bool inverse = a == 0;
    for (unsigned b = 0; b < 32; ++b) {
      REQUIRE(gfMul(uint8_t(a), uint8_t(b)) == gfMul(uint8_t(b), uint8_t(a)) && gfMul(uint8_t(a), uint8_t(b)) < 32);
      if (gfMul(uint8_t(a), uint8_t(b)) == 1) inverse = true;
      for (unsigned c = 0; c < 32; ++c) {
        REQUIRE(gfMul(uint8_t(a), gfMul(uint8_t(b), uint8_t(c))) == gfMul(gfMul(uint8_t(a), uint8_t(b)), uint8_t(c)));
        REQUIRE(gfMul(uint8_t(a), uint8_t(b ^ c)) == (gfMul(uint8_t(a), uint8_t(b)) ^ gfMul(uint8_t(a), uint8_t(c))));
      }
    }
    REQUIRE(inverse);
  }
}

static void pinned() {
  uint8_t v[15] = {};
  REQUIRE(checkValue(v) == 0 && codeOf(v) == "0000000000000000");
  v[0] = 1; REQUIRE(checkValue(v) == 2 && codeOf(v) == "1000000000000002");    // alpha^1 = x
  v[0] = 0; v[4] = 1; REQUIRE(checkValue(v) == 5 && codeOf(v) == "0000100000000005");  // alpha^5 = x^2+1
  v[4] = 0; v[14] = 1; REQUIRE(checkValue(v) == 31 && codeOf(v) == "000000000000001Z");  // alpha^15 = 0x1F
  v[14] = 0x21; REQUIRE(checkValue(v) == 31);  // low 5 bits
  // Linear: the check of a sum is the sum of the checks.
  Rng rng{0x9E3779B97F4A7C15ull};
  for (int n = 0; n < 2000; ++n) {
    uint8_t a[15], b[15], s[15];
    for (int i = 0; i < 15; ++i) { a[i] = uint8_t(rng.next() & 31); b[i] = uint8_t(rng.next() & 31); s[i] = uint8_t(a[i] ^ b[i]); }
    REQUIRE(checkValue(s) == (checkValue(a) ^ checkValue(b)));
  }
  for (int n = 0; n < 64; ++n) {  // the driver recomputes these independently
    uint8_t a[15];
    for (int i = 0; i < 15; ++i) a[i] = uint8_t(n < 32 ? (i == n % 15 ? (n + 1) & 31 : 0) : rng.next() & 31);
    const std::string c = codeOf(a);
    std::printf("VEC ");
    for (int i = 0; i < 15; ++i) std::printf("%02x", a[i]);
    std::printf(" %s %s\n", c.c_str(), displayOf(c).c_str());
  }
}

static std::string withSymbol(std::string c, size_t pos, int value) { c[pos] = kAlphabet[value]; return c; }

static void errors() {
  Rng rng{0xC0FFEE1234567ull};
  unsigned long substitutions = 0, swaps = 0;
  for (int n = 0; n < 300; ++n) {
    const uint32_t words[3] = {rng.next(), rng.next(), rng.next()};
    char raw[17]; fromRandom(words, raw);
    const std::string code = raw;
    std::string back;
    REQUIRE(parses(code, &back) && back == code);
    for (size_t pos = 0; pos < 16; ++pos) {
      const int value = valueOf(static_cast<unsigned char>(code[pos]));
      for (int other = 0; other < 32; ++other) {
        if (other == value) continue;
        REQUIRE(!parses(withSymbol(code, pos, other)));
        ++substitutions;
      }
      // An alias of another value is that value: still one wrong symbol.
      if (value != 1) { std::string t = code; t[pos] = 'l'; REQUIRE(!parses(t)); }
      if (value != 0) { std::string t = code; t[pos] = 'O'; REQUIRE(!parses(t)); }
    }
    for (size_t i = 0; i < 16; ++i) {
      for (size_t j = i + 1; j < 16; ++j) {
        if (code[i] == code[j]) continue;
        std::string t = code; std::swap(t[i], t[j]);
        REQUIRE(!parses(t));
        ++swaps;
      }
    }
  }
  REQUIRE(substitutions == 300ul * 16 * 31);
  REQUIRE(swaps > 300ul * 100);
  // Exhaustively by linearity: any nonzero error on any one position, and any
  // swap of two different values, moves the syndrome off zero.
  for (int pos = 0; pos < 16; ++pos) {
    for (int e = 1; e < 32; ++e) {
      uint8_t v[15] = {};
      uint8_t check = 0;
      if (pos < 15) v[pos] = uint8_t(e); else check = uint8_t(e);
      REQUIRE(checkValue(v) != check);
    }
  }
}

static void normalisation() {
  const uint8_t values[15] = {1, 0, 18, 22, 31, 7, 0, 1, 20, 9, 13, 26, 4, 30, 11};
  const std::string code = codeOf(values);
  const std::string shown = displayOf(code);
  std::string lower = shown; for (char& c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  REQUIRE(shown.size() == 19 && shown[4] == '-' && shown[9] == '-' && shown[14] == '-');
  REQUIRE(shown.substr(0, 4) + shown.substr(5, 4) + shown.substr(10, 4) + shown.substr(15, 4) == code);
  std::string out;
  auto same = [&](const std::string& text) { out.clear(); return parses(text, &out) && out == code; };
  REQUIRE(same(code) && same(shown) && same(lower));
  REQUIRE(same(" \t\r\n" + shown + "\r\n") && same("\v\f" + code + " \t") && same("\n" + lower + "\n"));
  // Separators anywhere: leading, trailing, doubled, between every symbol, mixed.
  REQUIRE(same("-" + code) && same(code + "--") && same("--" + shown + "-"));
  std::string spaced, dashed, mixed;
  for (size_t i = 0; i < code.size(); ++i) { spaced += code[i]; spaced += ' '; dashed += '-'; dashed += code[i]; mixed += code[i]; if (i % 3 == 1) mixed += i % 2 ? " - " : "--"; }
  REQUIRE(same(spaced) && same(dashed) && same(mixed));
  REQUIRE(same(code.substr(0, 2) + "  " + code.substr(2, 7) + " -- " + code.substr(9)));
  // I and L read as 1, O as 0, in either case.
  REQUIRE(code[0] == '1' && code[1] == '0' && code[7] == '1' && code[6] == '0');
  std::string alias = code; alias[0] = 'I'; alias[1] = 'o'; alias[6] = 'O'; alias[7] = 'l';
  REQUIRE(same(alias));
  alias[0] = 'i'; alias[7] = 'L'; REQUIRE(same(alias));
  // Inner whitespace other than ' ' is not a separator.
  for (const char* inner : {"\t", "\n", "\r", "\v", "\f"}) REQUIRE(!parses(code.substr(0, 8) + inner + code.substr(8)));
  // U, other bytes and a NUL are never symbols, anywhere.
  static const int junkBytes[] = {'U', 'u', '_', '.', '+', '/', '*', '~', '$', '=', ':', 0, 0x7F, 0x80, 0xC3, 0xFF};
  for (int junk : junkBytes) {
    for (size_t pos : {size_t(0), size_t(5), size_t(15)}) {
      std::string t = code; t[pos] = char(junk); REQUIRE(!parses(t));
      t = code; t.insert(pos, 1, char(junk)); REQUIRE(!parses(t));
    }
    REQUIRE(!parses(code + char(junk)));
  }
  // Exactly 16 symbols.
  REQUIRE(!parses(code.substr(0, 15)) && !parses(shown.substr(0, 18)));
  for (int extra = 0; extra < 32; ++extra) {
    REQUIRE(!parses(code + kAlphabet[extra]) && !parses(std::string(1, kAlphabet[extra]) + code));
  }
  REQUIRE(!parses(code + code));
  REQUIRE(!parses("") && !parses("----") && !parses("    ") && !parses(" \t\r\n"));
  char canonical[17]; std::memset(canonical, 'Q', sizeof(canonical));
  REQUIRE(!parse(nullptr, 0, canonical) && canonical[0] == '\0' && canonical[16] == '\0');
  REQUIRE(!parse(nullptr, 16, nullptr) && !isCode(nullptr, 5));
  REQUIRE(parse(code.data(), code.size(), nullptr));
  // Only the given length counts: the code followed by junk outside len parses.
  const std::string tail = code + "U!";
  REQUIRE(parse(tail.data(), 16, nullptr) && !parse(tail.data(), 17, nullptr));
}

static void generation() {
  // Value i is bits 5*(i%6) of word i/6; bits 30-31 of words 0-1 and 15-31 of word 2 are unused.
  const uint32_t words[3] = {6u | 5u << 5 | 4u << 10 | 3u << 15 | 2u << 20 | 1u << 25,
                             12u | 11u << 5 | 10u << 10 | 9u << 15 | 8u << 20 | 7u << 25,
                             31u | 30u << 5 | 29u << 10};
  const uint8_t expect[15] = {6, 5, 4, 3, 2, 1, 12, 11, 10, 9, 8, 7, 31, 30, 29};
  char a[17], b[17];
  fromRandom(words, a); fromValues(expect, b);
  REQUIRE(std::strcmp(a, b) == 0 && parses(a));
  const uint32_t noisy[3] = {words[0] | 0xC0000000u, words[1] | 0xC0000000u, words[2] | 0xFFFF8000u};
  fromRandom(noisy, b);
  REQUIRE(std::strcmp(a, b) == 0);
  const uint32_t ones[3] = {0xFFFFFFFFu, 0, 0xFFFFFFFFu};
  fromRandom(ones, a);
  REQUIRE(std::strncmp(a, "ZZZZZZ000000ZZZ", 15) == 0 && parses(a));
  uint8_t high[15]; for (int i = 0; i < 15; ++i) high[i] = uint8_t(expect[i] | 0xE0);
  fromValues(high, b); fromValues(expect, a);
  REQUIRE(std::strcmp(a, b) == 0);
  // Every generated code parses, round-trips through display, and the values are spread out.
  Rng rng{42};
  unsigned counts[16][32] = {};
  for (int n = 0; n < 32 * 400; ++n) {
    const uint32_t w[3] = {rng.next(), rng.next(), rng.next()};
    char c[17]; fromRandom(w, c);
    std::string back;
    REQUIRE(parses(c, &back) && back == c && parses(displayOf(c), &back) && back == c);
    for (int i = 0; i < 16; ++i) counts[i][valueOf(static_cast<unsigned char>(c[i]))]++;
  }
  for (int i = 0; i < 16; ++i) for (int v = 0; v < 32; ++v) REQUIRE(counts[i][v] > 250 && counts[i][v] < 560);
  // About one random 16-symbol string in 32 is a code.
  unsigned valid = 0;
  for (int n = 0; n < 64000; ++n) {
    char c[16];
    for (char& s : c) s = kAlphabet[rng.next() & 31];
    valid += parse(c, 16, nullptr);
  }
  REQUIRE(valid > 1700 && valid < 2300);
}

static void fuzz() {
  static const char pool[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZabcdefghijklmnopqrstuvwxyzILOUilou- -\t\r\n_.\x01\x80\xff";
  Rng rng{7};
  unsigned ok = 0;
  for (int n = 0; n < 200000; ++n) {
    std::string t;
    const size_t len = rng.next() % 28;
    for (size_t i = 0; i < len; ++i) t += pool[rng.next() % (sizeof(pool) - 1)];
    std::string c;
    if (parses(t, &c)) { ++ok; std::string again; REQUIRE(parses(c, &again) && again == c); }
  }
  std::printf("fuzz valid=%u\n", ok);
}

int main() {
  alphabet();
  field();
  pinned();
  errors();
  normalisation();
  generation();
  fuzz();
  std::printf("pair code: %lu checks passed\n", checks);
  return 0;
}
