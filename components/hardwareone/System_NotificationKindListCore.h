#ifndef SYSTEM_NOTIFICATIONKINDLISTCORE_H
#define SYSTEM_NOTIFICATIONKINDLISTCORE_H

// =============================================================================
// Personal notification-kind list mutation core
// =============================================================================
//
// This header owns only grammar and list semantics.  It deliberately has no
// Arduino, JSON, filesystem, identity, or SystemEvent dependency.  A firmware
// wrapper supplies:
//
//   * a stable catalog resolver (input/alias -> canonical token + kind index),
//   * canonical catalog enumeration for `all`,
//   * a repeatable indexed view of the currently persisted JSON strings, and
//   * a fallible output sink used to build the replacement JSON array.
//
// `prepare()` validates command-only state and can run before a filesystem
// transaction. `apply()` validates all current stored tokens and computes the
// complete result before making its first sink call.  The current sequence,
// catalog, prepared argument bytes, and canonical token storage must remain
// stable and repeatable for the duration of apply().
//
// No token matrix is materialized. Known membership/delta state uses two
// 256-bit sets; preserved unknowns are de-duplicated by bounded, repeatable
// O(n^2) visitation. Known canonical tokens have no 63-byte limit. The limit
// applies only to unknown strings retained from a newer firmware image.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace hw1_notification_kind_list {

static constexpr size_t kMaxEntries = 256;
static constexpr size_t kUnknownTokenMax = 63;
static constexpr size_t kKindIndexLimit = 256;
static constexpr size_t kKindBitsBytes = kKindIndexLimit / 8;
static_assert(kKindIndexLimit % 8 == 0, "kind bitset must use whole bytes");

struct TokenView {
  const char* data = nullptr;
  size_t length = 0;
};

struct CanonicalToken {
  TokenView token{};       // stable for the whole prepare/apply operation
  uint16_t kindIndex = 0;  // dense membership identity, range [0, 255]
};

using TokenAtFn = bool (*)(void* context, size_t index, TokenView& out);
using ResolveFn = bool (*)(void* context, TokenView input,
                           CanonicalToken& out);
using CatalogAtFn = bool (*)(void* context, size_t ordinal,
                             CanonicalToken& out);
using EmitFn = bool (*)(void* context, TokenView token);

// `at` must return the same view on every call during apply(). Empty sequences
// may leave `at` null because it will not be called.
struct TokenSequence {
  void* context = nullptr;
  size_t count = 0;
  TokenAtFn at = nullptr;
};

// resolve() returns false only for an unknown input token. A true result must
// contain a stable, non-empty canonical token and an index below 256.
// Enumeration contains canonical kinds only, in the order `all` must persist.
struct CatalogView {
  void* context = nullptr;
  ResolveFn resolve = nullptr;
  size_t count = 0;
  CatalogAtFn at = nullptr;
};

struct OutputSink {
  void* context = nullptr;
  EmitFn emit = nullptr;
};

enum class Operation : uint8_t {
  Invalid = 0,
  Replace,
  Set,
  Patch,
  All,
  None,
};

enum class Status : uint8_t {
  Ok = 0,
  InvalidArgument,
  InvalidGrammar,
  UnknownKind,
  InvalidStoredToken,
  TooManyEntries,
  DuplicatePatchOperation,
  ContradictoryPatchOperation,
  CatalogError,
  CurrentVisitError,
  InvalidPreparedMutation,
  SinkFailed,
  InconsistentCallbacks,
};

static constexpr size_t kNoErrorIndex = static_cast<size_t>(-1);

struct Result {
  Status status = Status::InvalidArgument;
  Operation operation = Operation::Invalid;
  size_t outputCount = 0;   // complete planned count when validation succeeds
  size_t emittedCount = 0;  // fully accepted sink calls only
  size_t errorIndex = kNoErrorIndex;

  constexpr bool ok() const { return status == Status::Ok; }
};

namespace detail {

static constexpr uint32_t kPreparedTag = UINT32_C(0x4E4B4C31);  // "NKL1"

inline bool viewStorageValid(TokenView view) {
  return view.length == 0 || view.data != nullptr;
}

inline TokenView trimAsciiSpace(TokenView view) {
  if (!viewStorageValid(view)) return TokenView{};
  size_t begin = 0;
  size_t end = view.length;
  while (begin < end) {
    const char c = view.data[begin];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    ++begin;
  }
  while (end > begin) {
    const char c = view.data[end - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    --end;
  }
  TokenView out;
  out.data = view.data ? view.data + begin : nullptr;
  out.length = end - begin;
  return out;
}

inline char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool equals(TokenView a, TokenView b) {
  return a.length == b.length &&
         (a.length == 0 ||
          (a.data != nullptr && b.data != nullptr &&
           memcmp(a.data, b.data, a.length) == 0));
}

inline bool equalsLiteralIgnoreCase(TokenView view, const char* literal) {
  if (!literal || !viewStorageValid(view)) return false;
  const size_t literalLength = strlen(literal);
  if (view.length != literalLength) return false;
  for (size_t i = 0; i < view.length; ++i) {
    if (asciiLower(view.data[i]) != asciiLower(literal[i])) return false;
  }
  return true;
}

inline bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool takeWord(TokenView input, size_t& offset, TokenView& word) {
  while (offset < input.length && isAsciiSpace(input.data[offset])) ++offset;
  const size_t begin = offset;
  while (offset < input.length && !isAsciiSpace(input.data[offset])) ++offset;
  word.data = input.data ? input.data + begin : nullptr;
  word.length = offset - begin;
  return word.length != 0;
}

inline bool bitTest(const uint8_t bits[kKindBitsBytes], uint16_t index) {
  return index < kKindIndexLimit &&
         (bits[index >> 3] & static_cast<uint8_t>(1u << (index & 7))) != 0;
}

inline void assignKindBit(uint8_t bits[kKindBitsBytes], uint16_t index,
                          bool value) {
  if (index >= kKindIndexLimit) return;
  const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
  if (value) bits[index >> 3] |= mask;
  else bits[index >> 3] &= static_cast<uint8_t>(~mask);
}

inline size_t bitCount(const uint8_t bits[kKindBitsBytes]) {
  size_t count = 0;
  for (size_t i = 0; i < kKindBitsBytes; ++i) {
    uint8_t value = bits[i];
    while (value != 0) {
      value = static_cast<uint8_t>(value & static_cast<uint8_t>(value - 1));
      ++count;
    }
  }
  return count;
}

inline bool canonicalValid(const CanonicalToken& canonical) {
  return canonical.kindIndex < kKindIndexLimit &&
         canonical.token.data != nullptr && canonical.token.length != 0;
}

enum class ResolveState : uint8_t { Unknown, Known, InvalidCatalog };

inline ResolveState resolve(const CatalogView& catalog, TokenView input,
                            CanonicalToken& canonical) {
  canonical = CanonicalToken{};
  if (!catalog.resolve || !catalog.resolve(catalog.context, input, canonical)) {
    return ResolveState::Unknown;
  }
  return canonicalValid(canonical) ? ResolveState::Known
                                   : ResolveState::InvalidCatalog;
}

inline bool reservedUnknown(TokenView token) {
  // Unknown stored tokens are already required to be lowercase, so exact
  // comparisons are sufficient. `boot` remains readable only when the catalog
  // resolver recognizes it as the legacy alias before this check.
  return equalsLiteralIgnoreCase(token, "boot") ||
         equalsLiteralIgnoreCase(token, "set") ||
         equalsLiteralIgnoreCase(token, "patch") ||
         equalsLiteralIgnoreCase(token, "all") ||
         equalsLiteralIgnoreCase(token, "none") ||
         equalsLiteralIgnoreCase(token, "list");
}

inline bool validPreservedUnknown(TokenView token) {
  if (!token.data || token.length == 0 || token.length > kUnknownTokenMax ||
      reservedUnknown(token)) {
    return false;
  }
  for (size_t i = 0; i < token.length; ++i) {
    const char c = token.data[i];
    const bool valid = (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '_';
    if (!valid) return false;
  }
  return true;
}

inline Result failure(Status status, Operation operation,
                      size_t index = kNoErrorIndex) {
  Result out;
  out.status = status;
  out.operation = operation;
  out.errorIndex = index;
  return out;
}

struct ApplyWorkspace {
  uint8_t targetKnown[kKindBitsBytes];
  uint8_t emittedKnown[kKindBitsBytes];
  size_t uniqueUnknownCount;
};

}  // namespace detail

// Caller-owned command-only plan. Do not modify it after a successful
// prepare(); `argument` points into the caller's original command storage.
struct PreparedMutation {
  uint32_t preparedTag = 0;
  Operation operation = Operation::Invalid;
  TokenView argument{};
  CanonicalToken setToken{};
  size_t preparedOutputCount = 0;  // final for replace/all/none
  size_t inputEntryCount = 0;
  uint8_t primaryBits[kKindBitsBytes] = {};    // replace kinds / patch mentions
  uint8_t secondaryBits[kKindBitsBytes] = {};  // patch additions
  bool setEnabled = false;
};

// This bound includes the public prepared plan and apply workspace even though
// they are not simultaneously hidden on the stack in the two-stage API. Small
// scalar/helper frames leave substantial room below the required 512 bytes.
static constexpr size_t kDeclaredCoreScratchBytes =
    sizeof(PreparedMutation) + sizeof(detail::ApplyWorkspace) + 128;
static_assert(kDeclaredCoreScratchBytes < 512,
              "notification-kind core scratch exceeded 512 bytes");

inline Result prepare(TokenView arguments, const CatalogView& catalog,
                      PreparedMutation& out) {
  out = PreparedMutation{};
  if (!detail::viewStorageValid(arguments) || !catalog.resolve) {
    return detail::failure(Status::InvalidArgument, Operation::Invalid);
  }

  const TokenView input = detail::trimAsciiSpace(arguments);
  if (input.length == 0) {
    return detail::failure(Status::InvalidGrammar, Operation::Invalid);
  }

  size_t wordOffset = 0;
  TokenView firstWord{};
  (void)detail::takeWord(input, wordOffset, firstWord);
  TokenView tail{input.data + wordOffset, input.length - wordOffset};
  tail = detail::trimAsciiSpace(tail);

  Operation operation = Operation::Replace;
  if (detail::equalsLiteralIgnoreCase(firstWord, "set")) {
    operation = Operation::Set;
  } else if (detail::equalsLiteralIgnoreCase(firstWord, "patch")) {
    operation = Operation::Patch;
  } else if (detail::equalsLiteralIgnoreCase(firstWord, "all")) {
    if (tail.length != 0)
      return detail::failure(Status::InvalidGrammar, Operation::All);
    operation = Operation::All;
  } else if (detail::equalsLiteralIgnoreCase(firstWord, "none")) {
    if (tail.length != 0)
      return detail::failure(Status::InvalidGrammar, Operation::None);
    operation = Operation::None;
  }

  out.operation = operation;
  out.argument = input;

  if (operation == Operation::None) {
    out.preparedTag = detail::kPreparedTag;
    return Result{Status::Ok, operation, 0, 0, kNoErrorIndex};
  }

  if (operation == Operation::All) {
    if (catalog.count > kMaxEntries || (catalog.count != 0 && !catalog.at)) {
      return detail::failure(catalog.count > kMaxEntries
                                 ? Status::TooManyEntries
                                 : Status::InvalidArgument,
                             operation);
    }
    for (size_t i = 0; i < catalog.count; ++i) {
      CanonicalToken canonical{};
      if (!catalog.at(catalog.context, i, canonical) ||
          !detail::canonicalValid(canonical) ||
          detail::bitTest(out.primaryBits, canonical.kindIndex)) {
        return detail::failure(Status::CatalogError, operation, i);
      }
      CanonicalToken roundTrip{};
      if (detail::resolve(catalog, canonical.token, roundTrip) !=
              detail::ResolveState::Known ||
          roundTrip.kindIndex != canonical.kindIndex ||
          !detail::equals(roundTrip.token, canonical.token)) {
        return detail::failure(Status::CatalogError, operation, i);
      }
      detail::assignKindBit(out.primaryBits, canonical.kindIndex, true);
    }
    out.preparedOutputCount = catalog.count;
    out.preparedTag = detail::kPreparedTag;
    return Result{Status::Ok, operation, catalog.count, 0, kNoErrorIndex};
  }

  if (operation == Operation::Set) {
    size_t offset = 0;
    TokenView kind{}, state{}, extra{};
    if (!detail::takeWord(tail, offset, kind) ||
        !detail::takeWord(tail, offset, state) ||
        detail::takeWord(tail, offset, extra)) {
      return detail::failure(Status::InvalidGrammar, operation);
    }
    CanonicalToken canonical{};
    const detail::ResolveState resolved = detail::resolve(catalog, kind, canonical);
    if (resolved == detail::ResolveState::Unknown)
      return detail::failure(Status::UnknownKind, operation);
    if (resolved == detail::ResolveState::InvalidCatalog)
      return detail::failure(Status::CatalogError, operation);
    if (detail::equalsLiteralIgnoreCase(state, "on")) out.setEnabled = true;
    else if (detail::equalsLiteralIgnoreCase(state, "off")) out.setEnabled = false;
    else return detail::failure(Status::InvalidGrammar, operation);
    out.setToken = canonical;
    out.inputEntryCount = 1;
    out.preparedTag = detail::kPreparedTag;
    return Result{Status::Ok, operation, 0, 0, kNoErrorIndex};
  }

  if (operation == Operation::Patch) {
    if (tail.length == 0)
      return detail::failure(Status::InvalidGrammar, operation);
    size_t begin = 0;
    size_t operationCount = 0;
    while (begin <= tail.length) {
      size_t end = begin;
      while (end < tail.length && tail.data[end] != ',') ++end;
      TokenView delta{tail.data + begin, end - begin};
      delta = detail::trimAsciiSpace(delta);
      if (delta.length < 2 || (delta.data[0] != '+' && delta.data[0] != '-')) {
        return detail::failure(Status::InvalidGrammar, operation, operationCount);
      }
      TokenView kind{delta.data + 1, delta.length - 1};
      // Whitespace is allowed around comma-delimited operations, never between
      // the sign and the canonical token advertised by the command grammar.
      if (!detail::equals(kind, detail::trimAsciiSpace(kind))) {
        return detail::failure(Status::InvalidGrammar, operation, operationCount);
      }
      CanonicalToken canonical{};
      const detail::ResolveState resolved = detail::resolve(catalog, kind, canonical);
      if (resolved == detail::ResolveState::Unknown)
        return detail::failure(Status::UnknownKind, operation, operationCount);
      if (resolved == detail::ResolveState::InvalidCatalog)
        return detail::failure(Status::CatalogError, operation, operationCount);

      const bool enable = delta.data[0] == '+';
      if (detail::bitTest(out.primaryBits, canonical.kindIndex)) {
        const bool previousEnable =
            detail::bitTest(out.secondaryBits, canonical.kindIndex);
        return detail::failure(previousEnable == enable
                                   ? Status::DuplicatePatchOperation
                                   : Status::ContradictoryPatchOperation,
                               operation, operationCount);
      }
      detail::assignKindBit(out.primaryBits, canonical.kindIndex, true);
      detail::assignKindBit(out.secondaryBits, canonical.kindIndex, enable);
      ++operationCount;
      if (operationCount > kMaxEntries)
        return detail::failure(Status::TooManyEntries, operation,
                               operationCount - 1);
      if (end == tail.length) break;
      begin = end + 1;
    }
    out.inputEntryCount = operationCount;
    out.preparedTag = detail::kPreparedTag;
    return Result{Status::Ok, operation, 0, 0, kNoErrorIndex};
  }

  // Legacy replacement: preserve its historical tolerance for empty comma
  // fields, but every non-empty field must resolve to a known catalog kind.
  size_t begin = 0;
  size_t entryCount = 0;
  size_t uniqueCount = 0;
  while (begin <= input.length) {
    size_t end = begin;
    while (end < input.length && input.data[end] != ',') ++end;
    TokenView token{input.data + begin, end - begin};
    token = detail::trimAsciiSpace(token);
    if (token.length != 0) {
      ++entryCount;
      if (entryCount > kMaxEntries)
        return detail::failure(Status::TooManyEntries, operation,
                               entryCount - 1);
      CanonicalToken canonical{};
      const detail::ResolveState resolved = detail::resolve(catalog, token, canonical);
      if (resolved == detail::ResolveState::Unknown)
        return detail::failure(Status::UnknownKind, operation, entryCount - 1);
      if (resolved == detail::ResolveState::InvalidCatalog)
        return detail::failure(Status::CatalogError, operation, entryCount - 1);
      if (!detail::bitTest(out.primaryBits, canonical.kindIndex)) {
        detail::assignKindBit(out.primaryBits, canonical.kindIndex, true);
        ++uniqueCount;
      }
    }
    if (end == input.length) break;
    begin = end + 1;
  }
  if (entryCount == 0)
    return detail::failure(Status::InvalidGrammar, operation);
  out.inputEntryCount = entryCount;
  out.preparedOutputCount = uniqueCount;
  out.preparedTag = detail::kPreparedTag;
  return Result{Status::Ok, operation, uniqueCount, 0, kNoErrorIndex};
}

namespace detail {

inline Status firstUnknownOccurrence(const TokenSequence& current,
                                     size_t currentIndex,
                                     TokenView candidate,
                                     const CatalogView& catalog,
                                     bool& first) {
  first = true;
  for (size_t priorIndex = 0; priorIndex < currentIndex; ++priorIndex) {
    TokenView prior{};
    if (!current.at || !current.at(current.context, priorIndex, prior))
      return Status::CurrentVisitError;
    if (!viewStorageValid(prior)) return Status::InvalidStoredToken;
    CanonicalToken ignored{};
    const ResolveState resolved = resolve(catalog, prior, ignored);
    if (resolved == ResolveState::InvalidCatalog) return Status::CatalogError;
    if (resolved == ResolveState::Unknown && equals(prior, candidate)) {
      first = false;
      return Status::Ok;
    }
  }
  return Status::Ok;
}

inline Status emitToken(const OutputSink& sink, TokenView token,
                        Result& result) {
  if (!sink.emit || !sink.emit(sink.context, token)) return Status::SinkFailed;
  ++result.emittedCount;
  return Status::Ok;
}

inline Status emitReplacement(const PreparedMutation& prepared,
                              const CatalogView& catalog,
                              const OutputSink& sink, Result& result) {
  uint8_t emitted[kKindBitsBytes] = {};
  size_t begin = 0;
  while (begin <= prepared.argument.length) {
    size_t end = begin;
    while (end < prepared.argument.length && prepared.argument.data[end] != ',') ++end;
    TokenView token{prepared.argument.data + begin, end - begin};
    token = trimAsciiSpace(token);
    if (token.length != 0) {
      CanonicalToken canonical{};
      if (resolve(catalog, token, canonical) != ResolveState::Known)
        return Status::InconsistentCallbacks;
      if (!bitTest(emitted, canonical.kindIndex)) {
        const Status status = emitToken(sink, canonical.token, result);
        if (status != Status::Ok) return status;
        assignKindBit(emitted, canonical.kindIndex, true);
      }
    }
    if (end == prepared.argument.length) break;
    begin = end + 1;
  }
  return result.emittedCount == result.outputCount
             ? Status::Ok : Status::InconsistentCallbacks;
}

inline Status emitAll(const PreparedMutation& prepared,
                      const CatalogView& catalog, const OutputSink& sink,
                      Result& result) {
  (void)prepared;
  for (size_t i = 0; i < catalog.count; ++i) {
    CanonicalToken canonical{};
    if (!catalog.at || !catalog.at(catalog.context, i, canonical) ||
        !canonicalValid(canonical)) {
      return Status::InconsistentCallbacks;
    }
    const Status status = emitToken(sink, canonical.token, result);
    if (status != Status::Ok) return status;
  }
  return result.emittedCount == result.outputCount
             ? Status::Ok : Status::InconsistentCallbacks;
}

inline Status emitPreserved(const PreparedMutation& prepared,
                            const TokenSequence& current,
                            const CatalogView& catalog,
                            ApplyWorkspace& workspace,
                            const OutputSink& sink, Result& result) {
  for (size_t i = 0; i < current.count; ++i) {
    TokenView token{};
    if (!current.at || !current.at(current.context, i, token))
      return Status::CurrentVisitError;
    CanonicalToken canonical{};
    const ResolveState resolved = resolve(catalog, token, canonical);
    if (resolved == ResolveState::InvalidCatalog) return Status::CatalogError;
    if (resolved == ResolveState::Known) {
      if (bitTest(workspace.targetKnown, canonical.kindIndex) &&
          !bitTest(workspace.emittedKnown, canonical.kindIndex)) {
        const Status status = emitToken(sink, canonical.token, result);
        if (status != Status::Ok) return status;
        assignKindBit(workspace.emittedKnown, canonical.kindIndex, true);
      }
      continue;
    }
    bool first = false;
    const Status firstStatus =
        firstUnknownOccurrence(current, i, token, catalog, first);
    if (firstStatus != Status::Ok) return firstStatus;
    if (first) {
      const Status status = emitToken(sink, token, result);
      if (status != Status::Ok) return status;
    }
  }

  if (prepared.operation == Operation::Set) {
    const uint16_t kind = prepared.setToken.kindIndex;
    if (bitTest(workspace.targetKnown, kind) &&
        !bitTest(workspace.emittedKnown, kind)) {
      const Status status = emitToken(sink, prepared.setToken.token, result);
      if (status != Status::Ok) return status;
      assignKindBit(workspace.emittedKnown, kind, true);
    }
  } else {
    // Emit additions that were absent from current storage in patch order.
    size_t firstSpace = 0;
    TokenView ignored{};
    (void)takeWord(prepared.argument, firstSpace, ignored);  // skip "patch"
    TokenView tail{prepared.argument.data + firstSpace,
                   prepared.argument.length - firstSpace};
    tail = trimAsciiSpace(tail);
    size_t begin = 0;
    while (begin <= tail.length) {
      size_t end = begin;
      while (end < tail.length && tail.data[end] != ',') ++end;
      TokenView delta{tail.data + begin, end - begin};
      delta = trimAsciiSpace(delta);
      if (delta.length < 2) return Status::InconsistentCallbacks;
      if (delta.data[0] == '+') {
        TokenView kind{delta.data + 1, delta.length - 1};
        CanonicalToken canonical{};
        if (resolve(catalog, kind, canonical) != ResolveState::Known)
          return Status::InconsistentCallbacks;
        if (bitTest(workspace.targetKnown, canonical.kindIndex) &&
            !bitTest(workspace.emittedKnown, canonical.kindIndex)) {
          const Status status = emitToken(sink, canonical.token, result);
          if (status != Status::Ok) return status;
          assignKindBit(workspace.emittedKnown, canonical.kindIndex, true);
        }
      }
      if (end == tail.length) break;
      begin = end + 1;
    }
  }

  return result.emittedCount == result.outputCount
             ? Status::Ok : Status::InconsistentCallbacks;
}

}  // namespace detail

inline Result apply(const PreparedMutation& prepared,
                    const TokenSequence& current,
                    const CatalogView& catalog,
                    const OutputSink& sink) {
  if (prepared.preparedTag != detail::kPreparedTag ||
      prepared.operation == Operation::Invalid) {
    return detail::failure(Status::InvalidPreparedMutation,
                           prepared.operation);
  }

  Result result;
  result.status = Status::Ok;
  result.operation = prepared.operation;

  if (prepared.operation == Operation::None) return result;

  if (prepared.operation == Operation::Replace) {
    if (prepared.inputEntryCount == 0 ||
        prepared.inputEntryCount > kMaxEntries ||
        prepared.preparedOutputCount > kMaxEntries ||
        !detail::viewStorageValid(prepared.argument)) {
      return detail::failure(Status::InvalidPreparedMutation,
                             prepared.operation);
    }
    result.outputCount = prepared.preparedOutputCount;
    if (result.outputCount != 0 && !sink.emit) {
      result.status = Status::InvalidArgument;
      return result;
    }
    result.status = detail::emitReplacement(prepared, catalog, sink, result);
    return result;
  }

  if (prepared.operation == Operation::All) {
    if (prepared.preparedOutputCount > kMaxEntries) {
      return detail::failure(Status::InvalidPreparedMutation,
                             prepared.operation);
    }
    result.outputCount = prepared.preparedOutputCount;
    if (catalog.count != prepared.preparedOutputCount ||
        (result.outputCount != 0 && (!sink.emit || !catalog.at))) {
      result.status = Status::InvalidArgument;
      return result;
    }
    result.status = detail::emitAll(prepared, catalog, sink, result);
    return result;
  }

  if (!catalog.resolve || current.count > kMaxEntries ||
      (current.count != 0 && !current.at)) {
    return detail::failure(current.count > kMaxEntries
                               ? Status::TooManyEntries
                               : Status::InvalidArgument,
                           prepared.operation);
  }

  detail::ApplyWorkspace workspace{};

  if ((prepared.operation == Operation::Set &&
       (prepared.inputEntryCount != 1 ||
        !detail::canonicalValid(prepared.setToken))) ||
      (prepared.operation == Operation::Patch &&
       (prepared.inputEntryCount == 0 ||
        prepared.inputEntryCount > kMaxEntries ||
        !detail::viewStorageValid(prepared.argument)))) {
    return detail::failure(Status::InvalidPreparedMutation,
                           prepared.operation);
  }

  // Full validation/build pass. No sink callback occurs in this loop.
  for (size_t i = 0; i < current.count; ++i) {
    TokenView token{};
    if (!current.at(current.context, i, token)) {
      return detail::failure(Status::CurrentVisitError,
                             prepared.operation, i);
    }
    if (!detail::viewStorageValid(token)) {
      return detail::failure(Status::InvalidStoredToken,
                             prepared.operation, i);
    }
    CanonicalToken canonical{};
    const detail::ResolveState resolved = detail::resolve(catalog, token, canonical);
    if (resolved == detail::ResolveState::InvalidCatalog) {
      return detail::failure(Status::CatalogError, prepared.operation, i);
    }
    if (resolved == detail::ResolveState::Known) {
      detail::assignKindBit(workspace.targetKnown, canonical.kindIndex, true);
      continue;
    }
    if (!detail::validPreservedUnknown(token)) {
      return detail::failure(Status::InvalidStoredToken,
                             prepared.operation, i);
    }
    bool first = false;
    const Status firstStatus = detail::firstUnknownOccurrence(
        current, i, token, catalog, first);
    if (firstStatus != Status::Ok) {
      return detail::failure(firstStatus, prepared.operation, i);
    }
    if (first) ++workspace.uniqueUnknownCount;
  }

  if (prepared.operation == Operation::Set) {
    detail::assignKindBit(workspace.targetKnown, prepared.setToken.kindIndex,
                          prepared.setEnabled);
  } else if (prepared.operation == Operation::Patch) {
    for (uint16_t kind = 0; kind < kKindIndexLimit; ++kind) {
      if (detail::bitTest(prepared.primaryBits, kind)) {
        detail::assignKindBit(
            workspace.targetKnown, kind,
            detail::bitTest(prepared.secondaryBits, kind));
      }
    }
  } else {
    return detail::failure(Status::InvalidPreparedMutation,
                           prepared.operation);
  }

  result.outputCount = detail::bitCount(workspace.targetKnown) +
                       workspace.uniqueUnknownCount;
  if (result.outputCount > kMaxEntries) {
    return detail::failure(Status::TooManyEntries, prepared.operation);
  }
  if (result.outputCount != 0 && !sink.emit) {
    return detail::failure(Status::InvalidArgument, prepared.operation);
  }

  result.status = detail::emitPreserved(prepared, current, catalog,
                                        workspace, sink, result);
  return result;
}

// One-shot convenience for callers that do not need to move command-only
// parsing outside their storage transaction.
inline Result mutate(TokenView arguments, const TokenSequence& current,
                     const CatalogView& catalog, const OutputSink& sink) {
  PreparedMutation prepared;
  Result result = prepare(arguments, catalog, prepared);
  if (!result.ok()) return result;
  return apply(prepared, current, catalog, sink);
}

inline const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::InvalidGrammar: return "invalid_grammar";
    case Status::UnknownKind: return "unknown_kind";
    case Status::InvalidStoredToken: return "invalid_stored_token";
    case Status::TooManyEntries: return "too_many_entries";
    case Status::DuplicatePatchOperation: return "duplicate_patch_operation";
    case Status::ContradictoryPatchOperation: return "contradictory_patch_operation";
    case Status::CatalogError: return "catalog_error";
    case Status::CurrentVisitError: return "current_visit_error";
    case Status::InvalidPreparedMutation: return "invalid_prepared_mutation";
    case Status::SinkFailed: return "sink_failed";
    case Status::InconsistentCallbacks: return "inconsistent_callbacks";
  }
  return "invalid_status";
}

}  // namespace hw1_notification_kind_list

#endif  // SYSTEM_NOTIFICATIONKINDLISTCORE_H
