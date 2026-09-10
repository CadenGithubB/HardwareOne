#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "../../System_CommandLookupCore.h"

namespace {

size_t gNewCalls = 0;

const char* const kNames[] = {
    nullptr,
    "",
    "cm5",
    "cm5 power",
    "cm5 power reboot",
    "httpAutoStart",
    "duplicate",
    "DuPlIcAtE",
    "spaced  internal",
    "z",
};

constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

struct ReferenceMatch {
  size_t ordinal = static_cast<size_t>(-1);
  size_t length = 0;
  size_t offset = 0;
  size_t trimmedLength = 0;
};

bool referenceSpace(unsigned char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

unsigned char referenceFold(unsigned char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<unsigned char>(value + ('a' - 'A'));
  }
  return value;
}

ReferenceMatch referenceResolve(const char* line, size_t lineLength) {
  ReferenceMatch result;
  if (!line || lineLength == 0) return result;

  size_t begin = 0;
  while (begin < lineLength &&
         referenceSpace(static_cast<unsigned char>(line[begin]))) {
    ++begin;
  }
  size_t end = lineLength;
  while (end > begin &&
         referenceSpace(static_cast<unsigned char>(line[end - 1]))) {
    --end;
  }
  result.offset = begin;
  result.trimmedLength = end - begin;

  for (size_t ordinal = 0; ordinal < kNameCount; ++ordinal) {
    const char* name = kNames[ordinal];
    if (!name) continue;
    const size_t nameLength = strlen(name);
    if (nameLength == 0 || nameLength > result.trimmedLength) continue;

    bool equal = true;
    for (size_t pos = 0; pos < nameLength; ++pos) {
      if (referenceFold(static_cast<unsigned char>(line[begin + pos])) !=
          referenceFold(static_cast<unsigned char>(name[pos]))) {
        equal = false;
        break;
      }
    }
    if (!equal) continue;
    if (nameLength != result.trimmedLength &&
        !referenceSpace(static_cast<unsigned char>(line[begin + nameLength]))) {
      continue;
    }
    if (nameLength > result.length) {
      result.ordinal = ordinal;
      result.length = nameLength;
    }
  }
  return result;
}

hw1_command_lookup::Match resolveFixture(const char* line, size_t length) {
  return hw1_command_lookup::resolve(
      line, length, kNameCount,
      [](size_t ordinal) -> const char* { return kNames[ordinal]; });
}

void assertEquivalent(const char* line, size_t length) {
  const ReferenceMatch expected = referenceResolve(line, length);
  const hw1_command_lookup::Match actual = resolveFixture(line, length);
  assert(actual.registryOrdinal == expected.ordinal);
  assert(actual.matchedLength == expected.length);
  assert(actual.lineOffset == expected.offset);
  assert(actual.trimmedLength == expected.trimmedLength);
}

void assertMatch(const char* line, size_t ordinal, size_t matchedLength,
                 size_t offset = 0) {
  const hw1_command_lookup::Match match = resolveFixture(line, strlen(line));
  assert(match.found());
  assert(match.registryOrdinal == ordinal);
  assert(match.matchedLength == matchedLength);
  assert(match.lineOffset == offset);
}

void testExactGrammar() {
  const hw1_command_lookup::Match nullMatch =
      resolveFixture(nullptr, 0);
  assert(!nullMatch.found());
  assert(!resolveFixture("", 0).found());

  const char whitespace[] = {' ', '\t', '\n', '\r', '\f', '\v'};
  for (char boundary : whitespace) {
    char line[] = {'c', 'M', '5', boundary, 'x', '\0'};
    assertMatch(line, 2, 3);

    char padded[] = {boundary, 'Z', boundary, '\0'};
    assertMatch(padded, 9, 1, 1);

    char onlySpace[] = {boundary, boundary, '\0'};
    const hw1_command_lookup::Match match =
        resolveFixture(onlySpace, strlen(onlySpace));
    assert(!match.found());
    assert(match.lineOffset == 2);
    assert(match.trimmedLength == 0);
  }

  assertMatch("cm5", 2, 3);
  assertMatch("CM5 POWER", 3, strlen("cm5 power"));
  assertMatch("\tCm5 PoWeR ReBoOt now\r\n", 4,
              strlen("cm5 power reboot"), 1);
  assertMatch("HTTPAUTOSTART 1", 5, strlen("httpAutoStart"));
  assertMatch("duplicate value", 6, strlen("duplicate"));
  assertMatch("spaced  INTERNAL yes", 8, strlen("spaced  internal"));

  // A command-name boundary accepts every isspace byte, but whitespace inside
  // a registered multi-word name remains exact. This therefore falls back to
  // the shorter `cm5` entry rather than normalizing the tab to a space.
  assertMatch("cm5\tpower", 2, 3);

  // Longest prefix wins regardless of registry order; equal-length duplicate
  // names retain the first registration row.
  assertMatch("cm5 power reboot now", 4, strlen("cm5 power reboot"));
  assertMatch("DuPlIcAtE", 6, strlen("duplicate"));

  assert(!resolveFixture("cm50", strlen("cm50")).found());
  assert(!resolveFixture("httpAutoStarter", strlen("httpAutoStarter")).found());
  assert(!resolveFixture("spaced internal", strlen("spaced internal")).found());
}

void testLiveProviderAndRegistrationOrder() {
  const char* liveNames[] = {"alpha", "alpha beta", "ALPHA"};
  size_t liveCount = 1;
  auto resolveLive = [&](const char* line) {
    return hw1_command_lookup::resolve(
        line, strlen(line), liveCount,
        [&](size_t ordinal) -> const char* { return liveNames[ordinal]; });
  };

  // Appending a registration is visible on the very next lookup; no cached
  // index or copied command-name lifetime sits between the registry and core.
  hw1_command_lookup::Match match = resolveLive("alpha beta value");
  assert(match.registryOrdinal == 0);
  assert(match.matchedLength == strlen("alpha"));
  liveCount = 2;
  match = resolveLive("alpha beta value");
  assert(match.registryOrdinal == 1);
  assert(match.matchedLength == strlen("alpha beta"));

  // The provider's pointer is read on each call. This models the firmware's
  // static CommandEntry lifetime without making the core own/copy names.
  liveNames[0] = "omega";
  liveCount = 1;
  assert(!resolveLive("alpha").found());
  assert(resolveLive("omega").registryOrdinal == 0);

  // Longest-match selection does not depend on shortest-first registration.
  const char* reverseNames[] = {"cm5 power reboot", "cm5 power", "cm5"};
  match = hw1_command_lookup::resolve(
      "cm5 power reboot now", strlen("cm5 power reboot now"), 3,
      [&](size_t ordinal) -> const char* { return reverseNames[ordinal]; });
  assert(match.registryOrdinal == 0);
  assert(match.matchedLength == strlen("cm5 power reboot"));
}

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

char randomByte(uint32_t& state) {
  static constexpr char kAlphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:_-./"
      " \t\r\n\f\v";
  return kAlphabet[nextRandom(state) % (sizeof(kAlphabet) - 1)];
}

void appendByte(char* line, size_t capacity, size_t& length, char value) {
  if (length + 1 < capacity) line[length++] = value;
}

void appendRandomCaseName(char* line, size_t capacity, size_t& length,
                          const char* name, uint32_t& state) {
  for (size_t i = 0; name && name[i]; ++i) {
    char value = name[i];
    if (value >= 'a' && value <= 'z' && (nextRandom(state) & 1u)) {
      value = static_cast<char>(value - 'a' + 'A');
    } else if (value >= 'A' && value <= 'Z' && (nextRandom(state) & 1u)) {
      value = static_cast<char>(value - 'A' + 'a');
    }
    appendByte(line, capacity, length, value);
  }
}

void testDifferentialFuzzAndNoAllocation() {
  uint32_t state = 0x5a17c9e3u;
  const size_t allocationsBefore = gNewCalls;

  for (size_t iteration = 0; iteration < 100000; ++iteration) {
    char line[192]{};
    size_t length = 0;

    const size_t leading = nextRandom(state) % 4;
    for (size_t i = 0; i < leading; ++i) {
      appendByte(line, sizeof(line), length,
                 " \t\r\n\f\v"[nextRandom(state) % 6]);
    }

    if ((nextRandom(state) & 3u) != 0) {
      size_t ordinal = nextRandom(state) % kNameCount;
      while (!kNames[ordinal] || !kNames[ordinal][0]) {
        ordinal = nextRandom(state) % kNameCount;
      }
      appendRandomCaseName(line, sizeof(line), length, kNames[ordinal], state);

      switch (nextRandom(state) % 4) {
        case 0:
          break;
        case 1:
          appendByte(line, sizeof(line), length,
                     " \t\r\n\f\v"[nextRandom(state) % 6]);
          break;
        case 2:
          // Deliberately violate the post-name word boundary.
          appendByte(line, sizeof(line), length, 'x');
          break;
        default: {
          appendByte(line, sizeof(line), length,
                     " \t\r\n\f\v"[nextRandom(state) % 6]);
          const size_t tail = nextRandom(state) % 30;
          for (size_t i = 0; i < tail; ++i) {
            appendByte(line, sizeof(line), length, randomByte(state));
          }
          break;
        }
      }
    } else {
      const size_t body = nextRandom(state) % 90;
      for (size_t i = 0; i < body; ++i) {
        appendByte(line, sizeof(line), length, randomByte(state));
      }
    }

    const size_t trailing = nextRandom(state) % 4;
    for (size_t i = 0; i < trailing; ++i) {
      appendByte(line, sizeof(line), length,
                 " \t\r\n\f\v"[nextRandom(state) % 6]);
    }
    line[length] = '\0';
    assertEquivalent(line, length);
  }

  // Both the production resolver and the independent oracle above operate on
  // caller-owned buffers and static registry names. A future String/container
  // regression is caught even if matching parity still happens to pass.
  assert(gNewCalls == allocationsBefore);
}

}  // namespace

void* operator new(size_t size) {
  ++gNewCalls;
  if (void* allocation = malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}

void* operator new[](size_t size) {
  ++gNewCalls;
  if (void* allocation = malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { free(allocation); }
void operator delete[](void* allocation) noexcept { free(allocation); }
void operator delete(void* allocation, size_t) noexcept { free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { free(allocation); }

int main() {
  testExactGrammar();
  testLiveProviderAndRegistrationOrder();
  testDifferentialFuzzAndNoAllocation();
  puts("command lookup core tests passed");
  return 0;
}
