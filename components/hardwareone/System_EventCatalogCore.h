// System_EventCatalogCore.h - dependency-light catalog validation and index.
//
// Production and hostile host fixtures instantiate this exact C++17 core.
// It owns no firmware tables, allocation, locks, renderer, JSON, or transport.
#ifndef SYSTEM_EVENTCATALOGCORE_H
#define SYSTEM_EVENTCATALOGCORE_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

namespace hw1_event_catalog_core {

struct TextView {
  const char* data;
  size_t length;
};

struct FamilyDescriptor {
  uint8_t id;
  TextView label;
};

struct KindDescriptor {
  uint8_t id;
  uint8_t family;
  TextView name;
};

enum ValidationError : uint32_t {
  ValidationOk                 = 0,
  FamilyCountUnsupported       = UINT32_C(1) << 0,
  KindCountUnsupported         = UINT32_C(1) << 1,
  InvalidFamilyId              = UINT32_C(1) << 2,
  InvalidKindId                = UINT32_C(1) << 3,
  EmptyFamilyLabel             = UINT32_C(1) << 4,
  FamilyLabelEmbeddedNul       = UINT32_C(1) << 5,
  InvalidFamilyLabelUtf8       = UINT32_C(1) << 6,
  DuplicateFamilyLabel         = UINT32_C(1) << 7,
  EmptyKindName                = UINT32_C(1) << 8,
  KindNameEmbeddedNul          = UINT32_C(1) << 9,
  InvalidKindName              = UINT32_C(1) << 10,
  DuplicateKindName            = UINT32_C(1) << 11,
  InvalidKindFamily            = UINT32_C(1) << 12,
  EmptyFamily                  = UINT32_C(1) << 13,
  ReservedKindName             = UINT32_C(1) << 14,
  IndexOffsetMismatch          = UINT32_C(1) << 15,
  IndexCoverageMismatch        = UINT32_C(1) << 16,
  IndexDuplicateKind           = UINT32_C(1) << 17,
  IndexFamilyMismatch          = UINT32_C(1) << 18,
};

struct ValidationResult {
  uint32_t errors;

  constexpr bool ok() const { return errors == ValidationOk; }
  constexpr bool has(ValidationError error) const {
    return (errors & static_cast<uint32_t>(error)) != 0;
  }
};

inline constexpr size_t kNotFound = static_cast<size_t>(-1);

constexpr bool storageValid(TextView text) {
  return text.length == 0 || text.data != nullptr;
}

constexpr unsigned char asciiLower(unsigned char value) {
  return value >= static_cast<unsigned char>('A') &&
                 value <= static_cast<unsigned char>('Z')
             ? static_cast<unsigned char>(
                   value + static_cast<unsigned char>('a' - 'A'))
             : value;
}

constexpr bool textEquals(TextView left, TextView right) {
  if (!storageValid(left) || !storageValid(right) ||
      left.length != right.length) {
    return false;
  }
  for (size_t index = 0; index < left.length; ++index) {
    if (left.data[index] != right.data[index]) return false;
  }
  return true;
}

constexpr bool textEqualsAsciiFolded(TextView left, TextView right) {
  if (!storageValid(left) || !storageValid(right) ||
      left.length != right.length) {
    return false;
  }
  for (size_t index = 0; index < left.length; ++index) {
    if (asciiLower(static_cast<unsigned char>(left.data[index])) !=
        asciiLower(static_cast<unsigned char>(right.data[index]))) {
      return false;
    }
  }
  return true;
}

template <size_t N>
constexpr TextView literalView(const char (&literal)[N]) {
  static_assert(N > 0, "a string literal always includes a terminator");
  return TextView{literal, N - 1};
}

constexpr bool containsNul(TextView text) {
  if (!storageValid(text)) return false;
  for (size_t index = 0; index < text.length; ++index) {
    if (text.data[index] == '\0') return true;
  }
  return false;
}

constexpr bool isContinuation(unsigned char byte) {
  return (byte & UINT8_C(0xC0)) == UINT8_C(0x80);
}

// Strict Unicode scalar-value UTF-8. This rejects truncation, overlong forms,
// surrogate encodings, and values above U+10FFFF.
constexpr bool isValidUtf8(TextView text) {
  if (!storageValid(text)) return false;
  size_t index = 0;
  while (index < text.length) {
    const unsigned char first =
        static_cast<unsigned char>(text.data[index]);
    if (first <= UINT8_C(0x7F)) {
      ++index;
      continue;
    }
    if (first >= UINT8_C(0xC2) && first <= UINT8_C(0xDF)) {
      if (index + 1 >= text.length ||
          !isContinuation(
              static_cast<unsigned char>(text.data[index + 1]))) {
        return false;
      }
      index += 2;
      continue;
    }
    if (first >= UINT8_C(0xE0) && first <= UINT8_C(0xEF)) {
      if (index + 2 >= text.length) return false;
      const unsigned char second =
          static_cast<unsigned char>(text.data[index + 1]);
      const unsigned char third =
          static_cast<unsigned char>(text.data[index + 2]);
      if (!isContinuation(second) || !isContinuation(third)) return false;
      if (first == UINT8_C(0xE0) && second < UINT8_C(0xA0)) return false;
      if (first == UINT8_C(0xED) && second >= UINT8_C(0xA0)) return false;
      index += 3;
      continue;
    }
    if (first >= UINT8_C(0xF0) && first <= UINT8_C(0xF4)) {
      if (index + 3 >= text.length) return false;
      const unsigned char second =
          static_cast<unsigned char>(text.data[index + 1]);
      const unsigned char third =
          static_cast<unsigned char>(text.data[index + 2]);
      const unsigned char fourth =
          static_cast<unsigned char>(text.data[index + 3]);
      if (!isContinuation(second) || !isContinuation(third) ||
          !isContinuation(fourth)) {
        return false;
      }
      if (first == UINT8_C(0xF0) && second < UINT8_C(0x90)) return false;
      if (first == UINT8_C(0xF4) && second > UINT8_C(0x8F)) return false;
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

constexpr bool isCanonicalKindName(TextView text) {
  if (!storageValid(text) || text.length == 0) return false;
  for (size_t index = 0; index < text.length; ++index) {
    const unsigned char value =
        static_cast<unsigned char>(text.data[index]);
    const bool valid = (value >= static_cast<unsigned char>('a') &&
                        value <= static_cast<unsigned char>('z')) ||
                       (value >= static_cast<unsigned char>('0') &&
                        value <= static_cast<unsigned char>('9')) ||
                       value == static_cast<unsigned char>('_');
    if (!valid) return false;
  }
  return true;
}

constexpr bool isBootAlias(TextView text) {
  return textEqualsAsciiFolded(text, literalView("boot"));
}

// These exact tokens already carry catalog-adjacent command semantics. A
// canonical kind claiming one would become ambiguous in at least one current
// lookup, persistence, or notification-policy interface.
constexpr bool isReservedKindName(TextView text) {
  return isBootAlias(text) ||
         textEqualsAsciiFolded(text, literalView("none")) ||
         textEqualsAsciiFolded(text, literalView("set")) ||
         textEqualsAsciiFolded(text, literalView("patch")) ||
         textEqualsAsciiFolded(text, literalView("all")) ||
         textEqualsAsciiFolded(text, literalView("list"));
}

template <size_t FamilyCount, size_t KindCount>
constexpr ValidationResult validate(
    const FamilyDescriptor (&families)[FamilyCount],
    const KindDescriptor (&kinds)[KindCount]) {
  uint32_t errors = ValidationOk;
  if (FamilyCount == 0 || FamilyCount > UINT8_MAX) {
    errors |= FamilyCountUnsupported;
  }
  // NONE occupies zero and COUNT must remain representable, leaving live ids
  // 1..254 in the current uint8_t enum contract.
  if (KindCount > static_cast<size_t>(UINT8_MAX - 1u)) {
    errors |= KindCountUnsupported;
  }

  for (size_t familyIndex = 0; familyIndex < FamilyCount; ++familyIndex) {
    const FamilyDescriptor& family = families[familyIndex];
    if (family.id != familyIndex) errors |= InvalidFamilyId;
    if (!storageValid(family.label) || family.label.length == 0) {
      errors |= EmptyFamilyLabel;
    }
    if (containsNul(family.label)) errors |= FamilyLabelEmbeddedNul;
    if (!isValidUtf8(family.label)) errors |= InvalidFamilyLabelUtf8;
    for (size_t prior = 0; prior < familyIndex; ++prior) {
      if (textEquals(family.label, families[prior].label)) {
        errors |= DuplicateFamilyLabel;
      }
    }
  }

  for (size_t kindIndex = 0; kindIndex < KindCount; ++kindIndex) {
    const KindDescriptor& kind = kinds[kindIndex];
    if (kind.id == 0 || kind.id != kindIndex + 1u) {
      errors |= InvalidKindId;
    }
    if (!storageValid(kind.name) || kind.name.length == 0) {
      errors |= EmptyKindName;
    }
    if (containsNul(kind.name)) errors |= KindNameEmbeddedNul;
    if (!isCanonicalKindName(kind.name)) errors |= InvalidKindName;
    if (kind.family >= FamilyCount) errors |= InvalidKindFamily;
    if (isReservedKindName(kind.name)) errors |= ReservedKindName;
    for (size_t prior = 0; prior < kindIndex; ++prior) {
      if (textEqualsAsciiFolded(kind.name, kinds[prior].name)) {
        errors |= DuplicateKindName;
      }
    }
  }

  for (size_t familyIndex = 0; familyIndex < FamilyCount; ++familyIndex) {
    bool found = false;
    for (size_t kindIndex = 0; kindIndex < KindCount; ++kindIndex) {
      if (kinds[kindIndex].family == families[familyIndex].id) {
        found = true;
        break;
      }
    }
    if (!found) errors |= EmptyFamily;
  }
  return ValidationResult{errors};
}

template <size_t FamilyCount, size_t KindCount>
struct FamilyIndex {
  static constexpr size_t kFamilyStorage =
      FamilyCount == 0 ? 1 : FamilyCount;
  static constexpr size_t kKindStorage = KindCount == 0 ? 1 : KindCount;

  uint8_t kindIds[kKindStorage]{};
  uint8_t offsets[kFamilyStorage]{};
  uint8_t counts[kFamilyStorage]{};
  uint8_t total{};
};

template <size_t FamilyCount, size_t KindCount>
constexpr FamilyIndex<FamilyCount, KindCount> buildFamilyIndex(
    const FamilyDescriptor (&families)[FamilyCount],
    const KindDescriptor (&kinds)[KindCount]) {
  FamilyIndex<FamilyCount, KindCount> result{};
  size_t cursor = 0;
  for (size_t familyIndex = 0; familyIndex < FamilyCount; ++familyIndex) {
    const size_t begin = cursor;
    result.offsets[familyIndex] = static_cast<uint8_t>(begin);
    for (size_t kindIndex = 0; kindIndex < KindCount; ++kindIndex) {
      if (kinds[kindIndex].family != families[familyIndex].id) continue;
      // Invalid synthetic descriptors may assign one kind to duplicate family
      // ids. Keep construction memory-safe; validateFamilyIndex() reports the
      // resulting coverage/duplicate errors.
      if (cursor < KindCount) result.kindIds[cursor] = kinds[kindIndex].id;
      ++cursor;
    }
    result.counts[familyIndex] =
        static_cast<uint8_t>(cursor - begin);
  }
  result.total = static_cast<uint8_t>(cursor);
  return result;
}

template <size_t KindCount>
constexpr size_t descriptorIndexForId(
    const KindDescriptor (&kinds)[KindCount], uint8_t id) {
  for (size_t index = 0; index < KindCount; ++index) {
    if (kinds[index].id == id) return index;
  }
  return kNotFound;
}

template <size_t FamilyCount, size_t KindCount>
constexpr ValidationResult validateFamilyIndex(
    const FamilyDescriptor (&families)[FamilyCount],
    const KindDescriptor (&kinds)[KindCount],
    const FamilyIndex<FamilyCount, KindCount>& index) {
  uint32_t errors = validate(families, kinds).errors;
  bool seen[FamilyIndex<FamilyCount, KindCount>::kKindStorage]{};
  size_t expectedOffset = 0;

  for (size_t familyIndex = 0; familyIndex < FamilyCount; ++familyIndex) {
    if (index.offsets[familyIndex] != expectedOffset) {
      errors |= IndexOffsetMismatch;
    }
    const size_t count = index.counts[familyIndex];
    if (expectedOffset + count > KindCount) {
      errors |= IndexCoverageMismatch;
      continue;
    }
    for (size_t within = 0; within < count; ++within) {
      const size_t position = expectedOffset + within;
      const size_t descriptorIndex =
          descriptorIndexForId(kinds, index.kindIds[position]);
      if (descriptorIndex == kNotFound) {
        errors |= IndexCoverageMismatch;
        continue;
      }
      if (seen[descriptorIndex]) errors |= IndexDuplicateKind;
      seen[descriptorIndex] = true;
      if (kinds[descriptorIndex].family != families[familyIndex].id) {
        errors |= IndexFamilyMismatch;
      }
    }
    expectedOffset += count;
  }

  if (expectedOffset != KindCount || index.total != KindCount) {
    errors |= IndexCoverageMismatch;
  }
  for (size_t kindIndex = 0; kindIndex < KindCount; ++kindIndex) {
    if (!seen[kindIndex]) errors |= IndexCoverageMismatch;
  }
  return ValidationResult{errors};
}

template <size_t KindCount>
constexpr size_t longestKindTokenCapacity(
    const KindDescriptor (&kinds)[KindCount]) {
  size_t capacity = 1;
  for (size_t index = 0; index < KindCount; ++index) {
    if (kinds[index].name.length + 1u > capacity) {
      capacity = kinds[index].name.length + 1u;
    }
  }
  return capacity;
}

template <size_t KindCount>
constexpr size_t findKindOrdinal(
    const KindDescriptor (&kinds)[KindCount], TextView input) {
  if (!storageValid(input) || input.length == 0) return kNotFound;
  const TextView canonicalInput =
      isBootAlias(input) ? literalView("boot_finished") : input;
  for (size_t index = 0; index < KindCount; ++index) {
    if (textEqualsAsciiFolded(canonicalInput, kinds[index].name)) {
      return index;
    }
  }
  return kNotFound;
}

}  // namespace hw1_event_catalog_core

#endif  // SYSTEM_EVENTCATALOGCORE_H
