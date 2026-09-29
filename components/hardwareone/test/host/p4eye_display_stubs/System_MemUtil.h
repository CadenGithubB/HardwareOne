#pragma once
#include <cstdlib>
enum class AllocPolicy { PreferInternal };
inline void* ps_calloc(size_t n, size_t size, AllocPolicy, const char*) {
  return calloc(n, size);
}
