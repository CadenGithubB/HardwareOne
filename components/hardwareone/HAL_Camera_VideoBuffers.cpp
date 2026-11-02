#include "System_BuildConfig.h"
#if ENABLE_CAMERA_SENSOR && defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_video_buffer.h"

extern "C" esp_video_buffer* __real_esp_video_buffer_create(const esp_video_buffer_info*);

// The pinned esp_video allocates descriptors and pixels with the same caps.
// Pixels belong in PSRAM; the CSI ISR's small intrusive-list descriptors must
// stay in internal RAM during flash writes. Relocate only the fresh, unqueued
// descriptors, leaving SDK allocation, pixel alignment and destruction intact.
// This private SDK layout is covered against its checked header/implementation.
extern "C" esp_video_buffer* __wrap_esp_video_buffer_create(const esp_video_buffer_info* info) {
  auto* original = __real_esp_video_buffer_create(info);
  if (!original || esp_ptr_internal(original)) return original;
  const size_t count = original->info.count;
  if (count > (SIZE_MAX - sizeof(*original)) / sizeof(original->element[0])) {
    esp_video_buffer_destroy(original);
    return nullptr;
  }
  const size_t bytes = sizeof(*original) + count * sizeof(original->element[0]);
  auto* internal = static_cast<esp_video_buffer*>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!internal) {
    esp_video_buffer_destroy(original);
    return nullptr;
  }
  memcpy(internal, original, bytes);
  for (size_t i = 0; i < count; ++i) internal->element[i].video_buffer = internal;
  free(original);
  return internal;
}
#endif
