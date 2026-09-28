#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_DEFAULT 4u
#define MALLOC_CAP_INTERNAL 8u
#ifdef __cplusplus
extern "C" {
#endif
void* heap_caps_malloc(size_t bytes, uint32_t caps);
#ifdef __cplusplus
}
#endif
