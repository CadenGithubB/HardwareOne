// The one checked number parser for command arguments (System_ArgParse.h).
// Numbers are decimal and the whole token has to be the number. Only a bus
// address (parseAddressArg) may also be written as 0x and hexadecimal digits.
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

  std::puts("arg parse: ok");
  return 0;
}
