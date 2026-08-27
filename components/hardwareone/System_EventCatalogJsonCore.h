// System_EventCatalogJsonCore.h - dependency-light catalog JSON emitter.
//
// Production adapts the public typed provider to this callback-shaped view.
// Host tests inject hostile views into this exact implementation.  The core is
// C++17-compatible, allocation-free, and validates every descriptor before it
// invokes the first output sink callback.
#ifndef SYSTEM_EVENTCATALOGJSONCORE_H
#define SYSTEM_EVENTCATALOGJSONCORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_EventCatalogJson.h"

namespace hw1_event_catalog_json_core {

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

// Callbacks and the referenced text must remain stable for the duration of a
// call.  The serializer intentionally makes a validation pass followed by an
// emission pass instead of caching a second catalog.
struct ProviderView {
  void* context;
  FamilyCountFn familyCount;
  FamilyAtFn familyAt;
  FamilyKindAtFn familyKindAt;
};

struct EmitResult {
  SystemEventCatalogJsonStatus status;
  size_t bytesWritten;
};

template <size_t N>
constexpr TextView literalView(const char (&literal)[N]) {
  static_assert(N > 0, "a string literal includes a terminator");
  return TextView{literal, N - 1u};
}

inline bool storageValid(TextView text) {
  return text.length == 0 || text.data != nullptr;
}

inline bool containsNul(TextView text) {
  if (!storageValid(text)) return false;
  for (size_t index = 0; index < text.length; ++index) {
    if (text.data[index] == '\0') return true;
  }
  return false;
}

inline bool isContinuation(uint8_t byte) {
  return (byte & UINT8_C(0xC0)) == UINT8_C(0x80);
}

// Strict Unicode scalar-value UTF-8. JSON C0 controls are valid input here;
// the emitter escapes them after this validation pass.
inline bool isValidUtf8(TextView text) {
  if (!storageValid(text)) return false;
  size_t index = 0;
  while (index < text.length) {
    const uint8_t first = static_cast<uint8_t>(text.data[index]);
    if (first <= UINT8_C(0x7F)) {
      ++index;
      continue;
    }
    if (first >= UINT8_C(0xC2) && first <= UINT8_C(0xDF)) {
      if (index + 1u >= text.length ||
          !isContinuation(static_cast<uint8_t>(text.data[index + 1u]))) {
        return false;
      }
      index += 2u;
      continue;
    }
    if (first >= UINT8_C(0xE0) && first <= UINT8_C(0xEF)) {
      if (index + 2u >= text.length) return false;
      const uint8_t second = static_cast<uint8_t>(text.data[index + 1u]);
      const uint8_t third = static_cast<uint8_t>(text.data[index + 2u]);
      if (!isContinuation(second) || !isContinuation(third)) return false;
      if (first == UINT8_C(0xE0) && second < UINT8_C(0xA0)) return false;
      if (first == UINT8_C(0xED) && second >= UINT8_C(0xA0)) return false;
      index += 3u;
      continue;
    }
    if (first >= UINT8_C(0xF0) && first <= UINT8_C(0xF4)) {
      if (index + 3u >= text.length) return false;
      const uint8_t second = static_cast<uint8_t>(text.data[index + 1u]);
      const uint8_t third = static_cast<uint8_t>(text.data[index + 2u]);
      const uint8_t fourth = static_cast<uint8_t>(text.data[index + 3u]);
      if (!isContinuation(second) || !isContinuation(third) ||
          !isContinuation(fourth)) {
        return false;
      }
      if (first == UINT8_C(0xF0) && second < UINT8_C(0x90)) return false;
      if (first == UINT8_C(0xF4) && second > UINT8_C(0x8F)) return false;
      index += 4u;
      continue;
    }
    return false;
  }
  return true;
}

inline bool validDescriptorText(TextView text) {
  return storageValid(text) && text.length > 0 && !containsNul(text) &&
         isValidUtf8(text);
}

inline SystemEventCatalogJsonStatus validateProvider(
    const ProviderView& provider) {
  if (!provider.familyCount || !provider.familyAt ||
      !provider.familyKindAt) {
    return SystemEventCatalogJsonStatus::InvalidArgument;
  }

  const size_t familyCount = provider.familyCount(provider.context);
  if (familyCount == 0) {
    return SystemEventCatalogJsonStatus::InvalidArgument;
  }

  for (size_t familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
    FamilyView family{};
    if (!provider.familyAt(provider.context, familyIndex, &family) ||
        !validDescriptorText(family.label) || family.kindCount == 0) {
      return SystemEventCatalogJsonStatus::InvalidArgument;
    }
    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      KindView kind{};
      if (!provider.familyKindAt(provider.context, family.key, kindIndex,
                                 &kind) ||
          !validDescriptorText(kind.name)) {
        return SystemEventCatalogJsonStatus::InvalidArgument;
      }
    }
  }
  return SystemEventCatalogJsonStatus::Ok;
}

inline constexpr size_t kStageCapacity = 256;

class BufferedWriter {
 public:
  BufferedWriter(SystemEventCatalogJsonSink sink, void* context)
      : sink_(sink), context_(context) {}

  bool append(const char* data, size_t length) {
    if (status_ != SystemEventCatalogJsonStatus::Ok) return false;
    if (!data && length != 0) {
      status_ = SystemEventCatalogJsonStatus::InvalidArgument;
      return false;
    }
    while (length != 0) {
      if (used_ == kStageCapacity && !flush()) return false;
      const size_t available = kStageCapacity - used_;
      const size_t copyLength = length < available ? length : available;
      memcpy(stage_ + used_, data, copyLength);
      used_ += copyLength;
      data += copyLength;
      length -= copyLength;
    }
    return true;
  }

  bool finish() {
    return status_ == SystemEventCatalogJsonStatus::Ok && flush();
  }

  SystemEventCatalogJsonStatus status() const { return status_; }
  size_t bytesWritten() const { return bytesWritten_; }

 private:
  bool flush() {
    if (used_ == 0) return true;
    if (bytesWritten_ > static_cast<size_t>(-1) - used_) {
      status_ = SystemEventCatalogJsonStatus::InvalidArgument;
      return false;
    }
    if (!sink_(context_, stage_, used_)) {
      status_ = SystemEventCatalogJsonStatus::SinkFailed;
      return false;
    }
    bytesWritten_ += used_;
    used_ = 0;
    return true;
  }

  SystemEventCatalogJsonSink sink_;
  void* context_;
  char stage_[kStageCapacity]{};
  size_t used_ = 0;
  size_t bytesWritten_ = 0;
  SystemEventCatalogJsonStatus status_ = SystemEventCatalogJsonStatus::Ok;
};

inline bool appendEscaped(BufferedWriter& writer, TextView text) {
  static constexpr char kHex[] = "0123456789abcdef";
  size_t runStart = 0;
  for (size_t index = 0; index < text.length; ++index) {
    const uint8_t value = static_cast<uint8_t>(text.data[index]);
    const char* escape = nullptr;
    size_t escapeLength = 0;
    char unicodeEscape[6] = {'\\', 'u', '0', '0', '0', '0'};

    switch (value) {
      case static_cast<uint8_t>('"'):
        escape = "\\\"";
        escapeLength = 2;
        break;
      case static_cast<uint8_t>('\\'):
        escape = "\\\\";
        escapeLength = 2;
        break;
      case UINT8_C(0x08):
        escape = "\\b";
        escapeLength = 2;
        break;
      case UINT8_C(0x0C):
        escape = "\\f";
        escapeLength = 2;
        break;
      case static_cast<uint8_t>('\n'):
        escape = "\\n";
        escapeLength = 2;
        break;
      case static_cast<uint8_t>('\r'):
        escape = "\\r";
        escapeLength = 2;
        break;
      case static_cast<uint8_t>('\t'):
        escape = "\\t";
        escapeLength = 2;
        break;
      default:
        if (value >= UINT8_C(0x01) && value <= UINT8_C(0x1F)) {
          unicodeEscape[4] = kHex[(value >> 4u) & UINT8_C(0x0F)];
          unicodeEscape[5] = kHex[value & UINT8_C(0x0F)];
          escape = unicodeEscape;
          escapeLength = sizeof(unicodeEscape);
        }
        break;
    }

    if (!escape) continue;
    if (index > runStart &&
        !writer.append(text.data + runStart, index - runStart)) {
      return false;
    }
    if (!writer.append(escape, escapeLength)) return false;
    runStart = index + 1u;
  }
  return runStart == text.length ||
         writer.append(text.data + runStart, text.length - runStart);
}

template <size_t N>
inline bool appendLiteral(BufferedWriter& writer, const char (&literal)[N]) {
  return writer.append(literal, N - 1u);
}

// The caller has already run validateProvider(). Provider callbacks must be
// deterministic for this second pass, as documented on ProviderView.
inline EmitResult emitValidated(const ProviderView& provider,
                                SystemEventCatalogJsonSink sink,
                                void* sinkContext) {
  BufferedWriter writer(sink, sinkContext);
  const size_t familyCount = provider.familyCount(provider.context);

  if (!appendLiteral(writer, "{\"families\":[")) {
    return EmitResult{writer.status(), writer.bytesWritten()};
  }
  for (size_t familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
    FamilyView family{};
    if (!provider.familyAt(provider.context, familyIndex, &family)) {
      return EmitResult{SystemEventCatalogJsonStatus::InvalidArgument,
                        writer.bytesWritten()};
    }
    if (familyIndex != 0 && !appendLiteral(writer, ",")) {
      return EmitResult{writer.status(), writer.bytesWritten()};
    }
    if (!appendLiteral(writer, "{\"n\":\"") ||
        !appendEscaped(writer, family.label) ||
        !appendLiteral(writer, "\",\"k\":[")) {
      return EmitResult{writer.status(), writer.bytesWritten()};
    }
    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      KindView kind{};
      if (!provider.familyKindAt(provider.context, family.key, kindIndex,
                                 &kind)) {
        return EmitResult{SystemEventCatalogJsonStatus::InvalidArgument,
                          writer.bytesWritten()};
      }
      if ((kindIndex != 0 && !appendLiteral(writer, ",")) ||
          !appendLiteral(writer, "\"") ||
          !appendEscaped(writer, kind.name) ||
          !appendLiteral(writer, "\"")) {
        return EmitResult{writer.status(), writer.bytesWritten()};
      }
    }
    if (!appendLiteral(writer, "]}")) {
      return EmitResult{writer.status(), writer.bytesWritten()};
    }
  }
  if (!appendLiteral(writer, "]}") || !writer.finish()) {
    return EmitResult{writer.status(), writer.bytesWritten()};
  }
  return EmitResult{SystemEventCatalogJsonStatus::Ok,
                    writer.bytesWritten()};
}

inline EmitResult writeJson(const ProviderView& provider,
                            SystemEventCatalogJsonSink sink,
                            void* sinkContext) {
  if (!sink) {
    return EmitResult{SystemEventCatalogJsonStatus::InvalidArgument, 0};
  }
  const SystemEventCatalogJsonStatus validation = validateProvider(provider);
  if (validation != SystemEventCatalogJsonStatus::Ok) {
    return EmitResult{validation, 0};
  }
  return emitValidated(provider, sink, sinkContext);
}

struct CountContext {
  size_t bytes = 0;
  bool overflow = false;
};

inline bool countSink(void* opaque, const char*, size_t length) {
  CountContext& count = *static_cast<CountContext*>(opaque);
  if (count.bytes > static_cast<size_t>(-1) - length) {
    count.overflow = true;
    return false;
  }
  count.bytes += length;
  return true;
}

// Status-bearing counting is exposed for malformed synthetic provider tests.
// Production's public size function has no invalid-input channel because its
// provider is compile-time validated.
inline EmitResult measureJson(const ProviderView& provider) {
  const SystemEventCatalogJsonStatus validation = validateProvider(provider);
  if (validation != SystemEventCatalogJsonStatus::Ok) {
    return EmitResult{validation, 0};
  }
  CountContext count{};
  EmitResult result = emitValidated(provider, countSink, &count);
  if (count.overflow) {
    return EmitResult{SystemEventCatalogJsonStatus::InvalidArgument, 0};
  }
  result.bytesWritten = count.bytes;
  return result;
}

}  // namespace hw1_event_catalog_json_core

#endif  // SYSTEM_EVENTCATALOGJSONCORE_H
