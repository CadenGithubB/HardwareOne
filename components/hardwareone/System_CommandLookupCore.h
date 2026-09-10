// System_CommandLookupCore.h - allocation-free command-name matching.
//
// This header deliberately has no Arduino, ESP-IDF, registry, or handler
// dependencies.  The firmware adapter supplies command names in registration
// order; host tests exercise this exact implementation directly.
#ifndef SYSTEM_COMMANDLOOKUPCORE_H
#define SYSTEM_COMMANDLOOKUPCORE_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

namespace hw1_command_lookup {

static constexpr size_t NO_MATCH = static_cast<size_t>(-1);

struct Match {
  size_t registryOrdinal = NO_MATCH;
  size_t matchedLength = 0;
  size_t lineOffset = 0;
  size_t trimmedLength = 0;

  constexpr bool found() const { return registryOrdinal != NO_MATCH; }
};

inline bool isCommandSpace(char value) {
  return isspace(static_cast<unsigned char>(value)) != 0;
}

inline unsigned char foldAscii(unsigned char value) {
  return (value >= static_cast<unsigned char>('A') &&
          value <= static_cast<unsigned char>('Z'))
             ? static_cast<unsigned char>(value - 'A' + 'a')
             : value;
}

inline bool equalAsciiIgnoreCase(const char* left, const char* right,
                                 size_t length) {
  if (!left || !right) return false;
  for (size_t i = 0; i < length; ++i) {
    if (foldAscii(static_cast<unsigned char>(left[i])) !=
        foldAscii(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

// Resolve one line against a provider of NUL-terminated command names.
// nameAt(index) must return names in registration order.  Equal-length
// duplicates intentionally keep the first row; only a strictly longer match
// replaces the current winner.
template <typename NameAt>
Match resolve(const char* line, size_t lineLength, size_t registrySize,
              NameAt nameAt) {
  Match result;
  if (!line || lineLength == 0) return result;

  size_t begin = 0;
  while (begin < lineLength && isCommandSpace(line[begin])) ++begin;

  size_t end = lineLength;
  while (end > begin && isCommandSpace(line[end - 1])) --end;

  result.lineOffset = begin;
  result.trimmedLength = end - begin;
  if (begin == end) return result;

  const char* trimmed = line + begin;
  for (size_t i = 0; i < registrySize; ++i) {
    const char* name = nameAt(i);
    if (!name || !name[0]) continue;

    const size_t nameLength = strlen(name);
    if (nameLength <= result.matchedLength ||
        nameLength > result.trimmedLength) {
      continue;
    }
    if (!equalAsciiIgnoreCase(trimmed, name, nameLength)) continue;

    if (nameLength != result.trimmedLength &&
        !isCommandSpace(trimmed[nameLength])) {
      continue;
    }

    result.registryOrdinal = i;
    result.matchedLength = nameLength;
  }
  return result;
}

}  // namespace hw1_command_lookup

#endif  // SYSTEM_COMMANDLOOKUPCORE_H
