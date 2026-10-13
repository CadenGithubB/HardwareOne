#pragma once
#include <stdbool.h>
bool fake_heap_is_psram(const void *p);
static inline bool esp_ptr_external_ram(const void *p) { return fake_heap_is_psram(p); }
static inline bool esp_ptr_internal(const void *p) { return !fake_heap_is_psram(p); }
static inline bool esp_ptr_in_drom(const void *) { return false; }
