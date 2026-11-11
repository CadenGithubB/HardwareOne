// System_ArgParse.h - the one checked number parser for command arguments.
//
// String::toInt() and atoi() return 0 for text that is not a number and stop
// at the first character they cannot use, so "abc" reads as 0 and "12x" as 12.
// A command that takes a number parses it here instead: the whole token has to
// be a number, or the command answers kCommandReplyNotANumber
// (System_CommandReply.h).
//
// Accepted: optional spaces around the token, an optional sign, decimal
// digits, or hexadecimal digits after 0x. Refused: empty text, any other
// character, a value outside the target type or outside the given range.
//
// Pure header: no Arduino or ESP-IDF dependency, so host tests include it.
// The text overloads take anything with c_str() and length() (String, std::string).
#pragma once

#include <errno.h>
#include <limits.h>
#include <limits>
#include <stdlib.h>
#include <string.h>

inline bool parseWholeNumber(const char* text, long long& value) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t') ++text;
  const char* digits = text;
  if (*digits == '+' || *digits == '-') ++digits;
  int base = 10;
  if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    base = 16;
    digits += 2;
  }
  // strtoll would accept leading spaces and a second sign here.
  const bool startsWithDigit =
      (*digits >= '0' && *digits <= '9') ||
      (base == 16 && ((*digits >= 'a' && *digits <= 'f') || (*digits >= 'A' && *digits <= 'F')));
  if (!startsWithDigit) return false;
  errno = 0;
  char* end = nullptr;
  const long long parsed = strtoll(text, &end, base);
  if (end == text || errno == ERANGE) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
  if (*end != '\0') return false;
  value = parsed;
  return true;
}

// The largest value of T this parser can deliver (a 64-bit unsigned type is
// read up to the largest long long).
template <typename T>
constexpr long long parseIntArgMax() {
  return static_cast<unsigned long long>(std::numeric_limits<T>::max()) >
                 static_cast<unsigned long long>((std::numeric_limits<long long>::max)())
             ? (std::numeric_limits<long long>::max)()
             : static_cast<long long>(std::numeric_limits<T>::max());
}

// Whole number inside [low, high]. `value` is written only on success.
template <typename T>
inline bool parseIntArg(const char* text, long long low, long long high, T& value) {
  long long parsed = 0;
  if (!parseWholeNumber(text, parsed)) return false;
  if (parsed < low || parsed > high) return false;
  if (parsed < static_cast<long long>(std::numeric_limits<T>::min()) ||
      parsed > parseIntArgMax<T>()) return false;
  value = static_cast<T>(parsed);
  return true;
}

// Whole number that fits the type of `value`.
template <typename T>
inline bool parseIntArg(const char* text, T& value) {
  return parseIntArg(text, static_cast<long long>(std::numeric_limits<T>::min()),
                     parseIntArgMax<T>(), value);
}

// For a String (anything with c_str() and length()). Text behind an embedded
// NUL makes the argument invalid; it is not cut off silently.
template <typename Text>
inline auto argTextIsWhole(const Text& text) -> decltype(text.c_str(), text.length(), bool()) {
  return strlen(text.c_str()) == static_cast<size_t>(text.length());
}

template <typename Text, typename T>
inline auto parseIntArg(const Text& text, T& value) -> decltype(argTextIsWhole(text)) {
  return argTextIsWhole(text) && parseIntArg(text.c_str(), value);
}

template <typename Text, typename T>
inline auto parseIntArg(const Text& text, long long low, long long high, T& value)
    -> decltype(argTextIsWhole(text)) {
  return argTextIsWhole(text) && parseIntArg(text.c_str(), low, high, value);
}
