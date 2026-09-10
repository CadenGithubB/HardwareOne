#pragma once

#include "System_MemUtil.h"
#include <cstring>
#include <limits>

// Task-context, caller-owned text storage. Capacity includes the terminating
// NUL; an allocation/limit failure is sticky until clear(). No partial output
// may be published unless ok() is true. PreferPSRAM always retains the shared
// allocator's internal fallback and bypass policy.
class PsramBuffer final {
public:
  enum class Failure : uint8_t { None, Allocation, Limit };

  explicit PsramBuffer(size_t maxCapacity, const char* tag)
      : limit_(maxCapacity), tag_(tag) {}
  ~PsramBuffer() { ps_free(data_); }
  PsramBuffer(const PsramBuffer&) = delete;
  PsramBuffer& operator=(const PsramBuffer&) = delete;

  char* data() { return data_; }
  const char* data() const { return data_; }
  const char* c_str() const { return data_ ? data_ : ""; }
  size_t size() const { return size_; }
  size_t length() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool ok() const { return failure_ == Failure::None; }
  Failure failure() const { return failure_; }

  void clear() {
    size_ = 0;
    failure_ = Failure::None;
    if (data_) data_[0] = '\0';
  }

  bool reserve(size_t bytes) {
    if (!ok()) return false;
    if (bytes > limit_) return fail(Failure::Limit);
    if (bytes <= capacity_) return true;
    size_t next = capacity_ ? capacity_ : (limit_ < 256 ? limit_ : 256);
    while (next < bytes) {
      if (next > limit_ / 2) { next = limit_; break; }
      next *= 2;
    }
    void* grown = ps_realloc(data_, next, AllocPref::PreferPSRAM, tag_);
    if (!grown) return fail(Failure::Allocation);
    data_ = static_cast<char*>(grown);
    capacity_ = next;
    data_[size_] = '\0';
    return true;
  }

  bool reserveAdditional(size_t count) {
    if (!ok()) return false;
    if (size_ == std::numeric_limits<size_t>::max() ||
        count > std::numeric_limits<size_t>::max() - size_ - 1) {
      return fail(Failure::Limit);
    }
    return reserve(size_ + count + 1);
  }

  // Commit bytes already written through data(); does not initialize/grow the
  // data. In particular, a File::read result must not be zeroed on commit.
  bool setSize(size_t bytes) {
    if (!ok()) return false;
    if (bytes >= capacity_) return fail(Failure::Limit);
    size_ = bytes;
    data_[size_] = '\0';
    return true;
  }

  bool append(const char* bytes, size_t count) {
    if (!ok()) return false;
    if (count == 0) return true;
    if (!bytes || size_ == std::numeric_limits<size_t>::max() ||
        count > std::numeric_limits<size_t>::max() - size_ - 1) {
      return fail(Failure::Limit);
    }
    // Preserve self-appends even if growth moves the allocation.
    const uintptr_t src = reinterpret_cast<uintptr_t>(bytes);
    const uintptr_t base = reinterpret_cast<uintptr_t>(data_);
    const bool owned = data_ && src >= base && src - base < capacity_;
    const size_t offset = owned ? static_cast<size_t>(src - base) : 0;
    if (owned && count > capacity_ - offset) return fail(Failure::Limit);
    if (!reserveAdditional(count)) return false;
    if (owned) bytes = data_ + offset;
    memmove(data_ + size_, bytes, count);
    size_ += count;
    data_[size_] = '\0';
    return true;
  }
  bool append(const char* text) {
    return text ? append(text, strlen(text)) : fail(Failure::Limit);
  }
  bool append(char byte) { return append(&byte, 1); }

private:
  bool fail(Failure reason) {
    if (ok()) failure_ = reason;
    return false;
  }
  char* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  const size_t limit_;
  const char* const tag_;
  Failure failure_ = Failure::None;
};
