// The checked number parsers for command arguments (System_ArgParse.h).
// Numbers are decimal and the whole token has to be the number. Only a bus
// address (parseAddressArg) and a bit mask (parseMaskArg) may also be written
// as 0x and hexadecimal digits.
#include "System_ArgParse.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

int main() {
  int n = -7;

  // Decimal, with a sign and with spaces around the token.
  assert(parseIntArg("250", n) && n == 250);
  assert(parseIntArg("-12", n) && n == -12);
  assert(parseIntArg("+3", n) && n == 3);
  assert(parseIntArg("  42\t", n) && n == 42);
  assert(parseIntArg("010", n) && n == 10);  // a leading zero is not octal
  assert(parseIntArg("0", n) && n == 0);

  // Refused, and the value is left alone.
  n = -7;
  for (const char* text : {"", " ", "abc", "12x", "100ms", "12.5", "1 2", "--1", "+", "-",
                           "0x10", "0X10", "0x", "x10", "1e3", "\"5\""}) {
    assert(!parseIntArg(text, n));
    assert(n == -7);
  }
  assert(!parseIntArg(static_cast<const char*>(nullptr), n));

  // The target type and the given range bound the value.
  uint8_t byte = 9;
  assert(parseIntArg("255", byte) && byte == 255);
  assert(!parseIntArg("256", byte) && byte == 255);
  assert(!parseIntArg("-1", byte));
  assert(!parseIntArg("99999999999999999999999", n));
  assert(parseIntArg("5", 0, 5, n) && n == 5);
  assert(!parseIntArg("6", 0, 5, n) && n == 5);
  unsigned long long wide = 0;
  assert(parseIntArg("9223372036854775807", wide) && wide == 9223372036854775807ULL);
  assert(!parseIntArg("9223372036854775808", wide));

  // Text objects: the same rules, and text behind an embedded NUL is refused.
  assert(parseIntArg(std::string("77"), n) && n == 77);
  assert(parseIntArg(std::string(" 8 "), 0, 9, n) && n == 8);
  assert(!parseIntArg(std::string("0x10"), n));
  assert(!parseIntArg(std::string("7\0junk", 6), n) && n == 8);

  // A bus address: decimal or hexadecimal, inside its range.
  uint8_t address = 0;
  assert(parseAddressArg("0x5A", 1, 127, address) && address == 0x5A);
  assert(parseAddressArg("0X5a", 1, 127, address) && address == 0x5A);
  assert(parseAddressArg("90", 1, 127, address) && address == 90);
  assert(parseAddressArg(std::string(" 0x70 "), 0x70, 0x77, address) && address == 0x70);
  for (const char* text : {"0x", "0xZZ", "5A", "0x80", "0", "0x5A junk", "0x5.A"}) {
    address = 1;
    assert(!parseAddressArg(text, 1, 127, address));
    assert(address == 1);
  }
  assert(!parseAddressArg(std::string("0x5A\0x", 6), 1, 127, address));

  // A 32-bit bit mask: decimal or hexadecimal, 0 to 0xFFFFFFFF.
  uint32_t mask = 7;
  assert(parseMaskArg("0x05", mask) && mask == 0x05);
  assert(parseMaskArg("0XffFFffFF", mask) && mask == 0xFFFFFFFFu);
  assert(parseMaskArg("4294967295", mask) && mask == 0xFFFFFFFFu);
  assert(parseMaskArg("0", mask) && mask == 0);
  assert(parseMaskArg("0x0", mask) && mask == 0);
  assert(parseMaskArg(" 18 ", mask) && mask == 18);
  assert(parseMaskArg(std::string(" 0x7F "), mask) && mask == 0x7F);
  for (const char* text : {"", " ", "0x", "0xZZ", "5A", "ff", "all", "-1", "-0x1", "4294967296",
                           "0x100000000", "0x05 junk", "0x0.5", "1e3", "0b101"}) {
    mask = 3;
    assert(!parseMaskArg(text, mask));
    assert(mask == 3);
  }
  assert(!parseMaskArg(static_cast<const char*>(nullptr), mask));
  assert(!parseMaskArg(std::string("0x05\0x", 6), mask) && mask == 3);

  // A decimal number: sign, digits, at most one decimal point.
  float f = -7.0f;
  assert(parseFloatArg("0.5", f) && f == 0.5f);
  assert(parseFloatArg("-12.25", f) && f == -12.25f);
  assert(parseFloatArg("+3", f) && f == 3.0f);
  assert(parseFloatArg(".5", f) && f == 0.5f);
  assert(parseFloatArg("5.", f) && f == 5.0f);
  assert(parseFloatArg("  47.6062\t", f) && f > 47.606f && f < 47.607f);
  assert(parseFloatArg("0", f) && f == 0.0f);
  assert(parseFloatArg("-0.0", f) && f == 0.0f);
  double d = 0.0;
  assert(parseFloatArg("-122.3321", d) && d == -122.3321);

  // Refused, and the value is left alone.
  f = -7.0f;
  for (const char* text : {"", " ", "abc", "0.5x", "12abc", "1e3", "1E3", "nan", "inf",
                           "-inf", "infinity", "0x10", "0x1p3", "1.2.3", ".", "-", "+", "-.",
                           "1 2", "--1", "1,5", "\"0.5\"", "high"}) {
    assert(!parseFloatArg(text, f));
    assert(f == -7.0f);
  }
  assert(!parseFloatArg(static_cast<const char*>(nullptr), f));

  // The given range and the target type bound the value.
  assert(parseFloatArg("1.0", 0.0, 1.0, f) && f == 1.0f);
  assert(!parseFloatArg("1.01", 0.0, 1.0, f) && f == 1.0f);
  assert(!parseFloatArg("-0.1", 0.0, 1.0, f));
  assert(parseFloatArg("90", -90, 90, d) && d == 90.0);
  assert(!parseFloatArg("90.0001", -90, 90, d) && d == 90.0);
  const std::string huge(400, '9');
  assert(!parseFloatArg(huge.c_str(), f));
  assert(!parseFloatArg(huge.c_str(), d));
  assert(!parseFloatArg("1000000000000000000000000000000000000000", f));  // 1e39 > float max
  assert(parseFloatArg("1000000000000000000000000000000000000000", d));

  // Text objects: the same rules, and text behind an embedded NUL is refused.
  assert(parseFloatArg(std::string(" 0.25 "), f) && f == 0.25f);
  assert(parseFloatArg(std::string("0.75"), 0.0, 1.0, f) && f == 0.75f);
  assert(!parseFloatArg(std::string("0.5x"), f) && f == 0.75f);
  assert(!parseFloatArg(std::string("0.5\0junk", 8), f) && f == 0.75f);

  std::puts("arg parse: ok");
  return 0;
}
