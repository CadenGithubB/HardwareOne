#pragma once
// Host fake of the ESP-IDF heap (fake_heap.cpp): every block is registered with where it
// lives (PSRAM or internal) and which allocator made it.
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_EXEC (1u << 0)
#define MALLOC_CAP_32BIT (1u << 1)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT (1u << 12)
#define MALLOC_CAP_SIMD (1u << 20)
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
