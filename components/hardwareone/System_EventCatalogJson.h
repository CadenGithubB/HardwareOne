// System_EventCatalogJson.h - transport-neutral System Event catalog JSON.
//
// The typed catalog remains authoritative.  This adapter only serializes that
// immutable view and deliberately exposes no HTTP, BLE, ArduinoJson, or task
// types.
#ifndef SYSTEM_EVENTCATALOGJSON_H
#define SYSTEM_EVENTCATALOGJSON_H

#include <stddef.h>
#include <stdint.h>

using SystemEventCatalogJsonSink =
    bool (*)(void* context, const char* data, size_t len);

enum class SystemEventCatalogJsonStatus : uint8_t {
  Ok,
  InvalidArgument,
  BufferTooSmall,
  SinkFailed,
};

// Exact production JSON payload size, excluding the trailing NUL used by the
// bounded-buffer adapter.
size_t systemEventCatalogJsonSize();

// Stream the complete production catalog.  A sink returning true accepted the
// complete chunk synchronously; false means it accepted none of that chunk.
SystemEventCatalogJsonStatus systemEventCatalogWriteJson(
    SystemEventCatalogJsonSink sink,
    void* context,
    size_t* bytesWritten = nullptr);

// Serialize the complete production catalog to caller-owned storage.
// requiredCapacity includes the trailing NUL; bytesWritten does not.
SystemEventCatalogJsonStatus systemEventCatalogJsonToBuffer(
    char* out,
    size_t capacity,
    size_t* bytesWritten = nullptr,
    size_t* requiredCapacity = nullptr);

#endif  // SYSTEM_EVENTCATALOGJSON_H
