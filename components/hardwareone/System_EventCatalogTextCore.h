// System_EventCatalogTextCore.h - bounded human catalog line emitter.
//
// This dependency-light C++17 core owns no firmware tables, allocation,
// renderer, transport, or mutable state. Production adapts the immutable typed
// System Event provider to ProviderView; host tests inject synthetic providers.
#ifndef SYSTEM_EVENTCATALOGTEXTCORE_H
#define SYSTEM_EVENTCATALOGTEXTCORE_H

#include <stddef.h>
#include <stdint.h>

namespace hw1_event_catalog_text_core {

struct TextView {
  const char* data;
  size_t length;
};

struct FamilyView {
  size_t key;
  TextView label;
  size_t kindCount;
};

struct KindView {
  TextView name;
};

using FamilyCountFn = size_t (*)(void* context);
using FamilyAtFn =
    bool (*)(void* context, size_t index, FamilyView* out);
using FamilyKindAtFn = bool (*)(void* context,
                                size_t familyKey,
                                size_t index,
                                KindView* out);

struct ProviderView {
  void* context;
  FamilyCountFn familyCount;
  FamilyAtFn familyAt;
  FamilyKindAtFn familyKindAt;
};

// A true return synchronously accepts the complete line. A false return
// accepts none of it and stops emission; the adapter never calls the sink
// again after failure.
using LineSink =
    bool (*)(void* context, const char* line, size_t length);

enum class Status : uint8_t {
  Ok,
  InvalidArgument,
  ProviderFailed,
  LineTooLong,
  SinkFailed,
};

namespace detail {

inline bool validText(TextView text) {
  if (!text.data || text.length == 0) return false;
  for (size_t index = 0; index < text.length; ++index) {
    if (text.data[index] == '\0') return false;
  }
  return true;
}

inline size_t decimalDigits(size_t value) {
  size_t digits = 1;
  while (value >= 10u) {
    value /= 10u;
    ++digits;
  }
  return digits;
}

inline bool checkedAdd(size_t& total, size_t amount) {
  const size_t maximum = static_cast<size_t>(-1);
  if (amount > maximum - total) return false;
  total += amount;
  return true;
}

inline bool lineFits(size_t payloadCapacity,
                     size_t first,
                     size_t second = 0,
                     size_t third = 0,
                     size_t fourth = 0,
                     size_t fifth = 0) {
  size_t total = 0;
  return checkedAdd(total, first) && checkedAdd(total, second) &&
         checkedAdd(total, third) && checkedAdd(total, fourth) &&
         checkedAdd(total, fifth) && total <= payloadCapacity;
}

struct PreflightResult {
  Status status;
  size_t familyCount;
  size_t kindCount;
};

inline PreflightResult preflight(const ProviderView& provider,
                                 size_t payloadCapacity) {
  const size_t familyCount = provider.familyCount(provider.context);
  size_t kindCount = 0;

  for (size_t familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
    FamilyView family{};
    if (!provider.familyAt(provider.context, familyIndex, &family) ||
        !validText(family.label)) {
      return {Status::ProviderFailed, familyCount, 0};
    }
    if (!checkedAdd(kindCount, family.kindCount)) {
      return {Status::ProviderFailed, familyCount, 0};
    }

    // "  " + label + " (" + decimal count + "):"
    if (!lineFits(payloadCapacity, 2u, family.label.length, 2u,
                  decimalDigits(family.kindCount), 2u)) {
      return {Status::LineTooLong, familyCount, kindCount};
    }

    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      KindView kind{};
      if (!provider.familyKindAt(provider.context, family.key, kindIndex,
                                 &kind) ||
          !validText(kind.name)) {
        return {Status::ProviderFailed, familyCount, 0};
      }
      // Every token must fit when retried on a fresh, four-space-indented line.
      if (!lineFits(payloadCapacity, 4u, kind.name.length)) {
        return {Status::LineTooLong, familyCount, kindCount};
      }
    }
  }

  // "OK: " + count + " event kinds in " + count + " families:"
  if (!lineFits(payloadCapacity, 4u, decimalDigits(kindCount), 16u,
                decimalDigits(familyCount), 10u)) {
    return {Status::LineTooLong, familyCount, kindCount};
  }
  return {Status::Ok, familyCount, kindCount};
}

struct LineBuffer {
  char* data;
  size_t payloadCapacity;
  size_t length;
};

inline bool canAppend(const LineBuffer& line, size_t amount) {
  return line.length <= line.payloadCapacity &&
         amount <= line.payloadCapacity - line.length;
}

inline void appendUnchecked(LineBuffer& line,
                            const char* text,
                            size_t length) {
  for (size_t index = 0; index < length; ++index) {
    line.data[line.length + index] = text[index];
  }
  line.length += length;
}

inline void terminate(LineBuffer& line) {
  line.data[line.length] = '\0';
}

inline bool appendWhole(LineBuffer& line,
                        TextView first,
                        TextView second = {nullptr, 0}) {
  size_t addition = first.length;
  if (!checkedAdd(addition, second.length) || !canAppend(line, addition)) {
    return false;
  }
  appendUnchecked(line, first.data, first.length);
  appendUnchecked(line, second.data, second.length);
  terminate(line);
  return true;
}

template <size_t N>
inline TextView literalView(const char (&literal)[N]) {
  static_assert(N > 0, "a string literal includes a terminator");
  return TextView{literal, N - 1u};
}

inline bool appendDecimal(LineBuffer& line, size_t value) {
  char reversed[sizeof(size_t) * 3u]{};
  size_t length = 0;
  do {
    reversed[length++] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  } while (value != 0);
  if (!canAppend(line, length)) return false;
  while (length > 0) line.data[line.length++] = reversed[--length];
  terminate(line);
  return true;
}

inline Status flush(LineBuffer& line,
                    LineSink sink,
                    void* sinkContext,
                    size_t* linesWritten) {
  if (line.length == 0) return Status::Ok;
  terminate(line);
  if (!sink(sinkContext, line.data, line.length)) return Status::SinkFailed;
  if (linesWritten) ++*linesWritten;
  line.length = 0;
  line.data[0] = '\0';
  return Status::Ok;
}

}  // namespace detail

// Emits the current human `events kinds` listing. The provider must remain
// immutable for the duration of this call: it is traversed once for complete
// preflight and again for emission. lineCapacity includes the trailing NUL;
// payloadCapacity does not. No sink callback occurs unless every header and
// token in the preflight view can fit without truncation.
inline Status writeCatalog(const ProviderView& provider,
                           LineSink sink,
                           void* sinkContext,
                           char* lineBuffer,
                           size_t lineCapacity,
                           size_t payloadCapacity,
                           size_t* linesWritten = nullptr) {
  if (linesWritten) *linesWritten = 0;
  if (!provider.familyCount || !provider.familyAt ||
      !provider.familyKindAt || !sink || !lineBuffer || lineCapacity == 0 ||
      payloadCapacity >= lineCapacity) {
    return Status::InvalidArgument;
  }
  lineBuffer[0] = '\0';

  const detail::PreflightResult checked =
      detail::preflight(provider, payloadCapacity);
  if (checked.status != Status::Ok) return checked.status;

  detail::LineBuffer line{lineBuffer, payloadCapacity, 0};
  const auto appendLiteral = [&line](auto const& literal) {
    return detail::appendWhole(line, detail::literalView(literal));
  };
  const auto flush = [&line, sink, sinkContext, linesWritten]() {
    return detail::flush(line, sink, sinkContext, linesWritten);
  };

  if (!appendLiteral("OK: ") ||
      !detail::appendDecimal(line, checked.kindCount) ||
      !appendLiteral(" event kinds in ") ||
      !detail::appendDecimal(line, checked.familyCount) ||
      !appendLiteral(" families:")) {
    return Status::LineTooLong;
  }
  Status status = flush();
  if (status != Status::Ok) return status;

  for (size_t familyIndex = 0; familyIndex < checked.familyCount;
       ++familyIndex) {
    FamilyView family{};
    if (!provider.familyAt(provider.context, familyIndex, &family) ||
        !detail::validText(family.label)) {
      return Status::ProviderFailed;
    }

    if (!appendLiteral("  ") || !detail::appendWhole(line, family.label) ||
        !appendLiteral(" (") ||
        !detail::appendDecimal(line, family.kindCount) ||
        !appendLiteral("):")) {
      return Status::LineTooLong;
    }
    status = flush();
    if (status != Status::Ok) return status;

    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      KindView kind{};
      if (!provider.familyKindAt(provider.context, family.key, kindIndex,
                                 &kind) ||
          !detail::validText(kind.name)) {
        return Status::ProviderFailed;
      }

      const TextView prefix = line.length == 0
                                  ? detail::literalView("    ")
                                  : detail::literalView(" ");
      if (!detail::appendWhole(line, prefix, kind.name)) {
        status = flush();
        if (status != Status::Ok) return status;
        if (!detail::appendWhole(line, detail::literalView("    "),
                                 kind.name)) {
          // Defensive against a provider changing after successful preflight.
          return Status::LineTooLong;
        }
      }
    }
    status = flush();
    if (status != Status::Ok) return status;
  }

  return Status::Ok;
}

}  // namespace hw1_event_catalog_text_core

#endif  // SYSTEM_EVENTCATALOGTEXTCORE_H
