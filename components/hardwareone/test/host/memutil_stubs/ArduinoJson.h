#pragma once

#include <stddef.h>

namespace ArduinoJson {

class Allocator {
 public:
  virtual ~Allocator() = default;
  virtual void* allocate(size_t size) = 0;
  virtual void deallocate(void* ptr) = 0;
  virtual void* reallocate(void* ptr, size_t newSize) = 0;
};

}  // namespace ArduinoJson

