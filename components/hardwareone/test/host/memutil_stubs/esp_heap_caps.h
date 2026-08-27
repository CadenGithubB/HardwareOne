#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT     (1u << 0)
#define MALLOC_CAP_INTERNAL (1u << 1)
#define MALLOC_CAP_SPIRAM   (1u << 2)

#ifdef __cplusplus
extern "C" {
#endif

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);

void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
void* heap_caps_malloc_prefer(size_t size, size_t num, ...);
void* heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...);
void* heap_caps_realloc_prefer(void* ptr, size_t size, size_t num, ...);

#ifdef __cplusplus
}
#endif

