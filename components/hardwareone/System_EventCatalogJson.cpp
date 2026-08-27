// System_EventCatalogJson.cpp - production adapter over the typed catalog.

#include "System_EventCatalogJson.h"

#include <limits.h>
#include <string.h>

#include "System_EventCatalog.h"
#include "System_EventCatalogJsonCore.h"

namespace json_core = hw1_event_catalog_json_core;

namespace {

size_t productionFamilyCount(void*) {
  return systemEventCatalogFamilyCount();
}

bool productionFamilyAt(void*, size_t index, json_core::FamilyView* out) {
  if (!out) return false;
  SystemEventCatalogFamilyInfo family{};
  if (!systemEventCatalogFamilyAt(index, &family) || !family.label) {
    return false;
  }
  json_core::FamilyView resolved{};
  resolved.key = static_cast<size_t>(family.id);
  resolved.label = json_core::TextView{family.label, strlen(family.label)};
  resolved.kindCount = family.kindCount;
  *out = resolved;
  return true;
}

bool productionFamilyKindAt(void*,
                            size_t familyKey,
                            size_t index,
                            json_core::KindView* out) {
  if (!out || familyKey > static_cast<size_t>(UINT8_MAX)) return false;
  SystemEventCatalogKindInfo kind{};
  if (!systemEventCatalogFamilyKindAt(
          static_cast<SystemEventFamily>(familyKey), index, &kind) ||
      !kind.name) {
    return false;
  }
  json_core::KindView resolved{};
  resolved.name = json_core::TextView{kind.name, strlen(kind.name)};
  *out = resolved;
  return true;
}

static constexpr json_core::ProviderView kProductionProvider = {
    nullptr,
    productionFamilyCount,
    productionFamilyAt,
    productionFamilyKindAt,
};

struct BufferSinkContext {
  char* out;
  size_t capacity;
  size_t used;
};

bool bufferSink(void* opaque, const char* data, size_t length) {
  BufferSinkContext& destination =
      *static_cast<BufferSinkContext*>(opaque);
  if (!data || destination.used > destination.capacity ||
      length > destination.capacity - destination.used) {
    return false;
  }
  memcpy(destination.out + destination.used, data, length);
  destination.used += length;
  return true;
}

}  // namespace

size_t systemEventCatalogJsonSize() {
  const json_core::EmitResult measured =
      json_core::measureJson(kProductionProvider);
  return measured.status == SystemEventCatalogJsonStatus::Ok
             ? measured.bytesWritten
             : 0;
}

SystemEventCatalogJsonStatus systemEventCatalogWriteJson(
    SystemEventCatalogJsonSink sink,
    void* context,
    size_t* bytesWritten) {
  if (bytesWritten) *bytesWritten = 0;
  const json_core::EmitResult result =
      json_core::writeJson(kProductionProvider, sink, context);
  if (bytesWritten) *bytesWritten = result.bytesWritten;
  return result.status;
}

SystemEventCatalogJsonStatus systemEventCatalogJsonToBuffer(
    char* out,
    size_t capacity,
    size_t* bytesWritten,
    size_t* requiredCapacity) {
  if (bytesWritten) *bytesWritten = 0;
  if (requiredCapacity) *requiredCapacity = 0;
  if (!out) return SystemEventCatalogJsonStatus::InvalidArgument;

  const json_core::EmitResult measured =
      json_core::measureJson(kProductionProvider);
  if (measured.status != SystemEventCatalogJsonStatus::Ok ||
      measured.bytesWritten == static_cast<size_t>(-1)) {
    if (capacity != 0) out[0] = '\0';
    return SystemEventCatalogJsonStatus::InvalidArgument;
  }

  const size_t required = measured.bytesWritten + 1u;
  if (requiredCapacity) *requiredCapacity = required;
  if (capacity < required) {
    if (capacity != 0) out[0] = '\0';
    return SystemEventCatalogJsonStatus::BufferTooSmall;
  }

  BufferSinkContext destination{out, measured.bytesWritten, 0};
  const json_core::EmitResult written =
      json_core::writeJson(kProductionProvider, bufferSink, &destination);
  if (written.status != SystemEventCatalogJsonStatus::Ok ||
      written.bytesWritten != measured.bytesWritten ||
      destination.used != measured.bytesWritten) {
    out[0] = '\0';
    if (written.status == SystemEventCatalogJsonStatus::SinkFailed) {
      // The exact preflight makes this unreachable for the production view,
      // but preserve BufferTooSmall's required-capacity contract defensively.
      return SystemEventCatalogJsonStatus::BufferTooSmall;
    }
    if (requiredCapacity) *requiredCapacity = 0;
    return SystemEventCatalogJsonStatus::InvalidArgument;
  }

  out[destination.used] = '\0';
  if (bytesWritten) *bytesWritten = destination.used;
  return SystemEventCatalogJsonStatus::Ok;
}
