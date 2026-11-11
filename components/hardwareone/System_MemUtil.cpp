#include "System_MemUtil.h"

#include <esp_log.h>

std::atomic<uint32_t> gPsAllocFallbacks{0};

namespace {

constexpr uint32_t kInternal8BitCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsram8BitCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr const char* kJsonPoolTag = "json.pool";
constexpr uint32_t kFallbackLogsBeforeRateLimit = 8;

bool policyRequestsPsram(AllocPolicy policy) {
  return policy == AllocPolicy::PreferPsram ||
         policy == AllocPolicy::RequirePsram;
}

bool policyMayUsePsram(AllocPolicy policy) {
  return policy == AllocPolicy::PreferPsram ||
         policy == AllocPolicy::PreferInternal ||
         policy == AllocPolicy::RequirePsram;
}

bool shouldLogFallback(uint32_t occurrence) {
  if (occurrence <= kFallbackLogsBeforeRateLimit) return true;
  return occurrence != 0 && (occurrence & (occurrence - 1)) == 0;
}

void reportPsramFallback(size_t size, const char* tag) {
  const uint32_t occurrence =
      gPsAllocFallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!shouldLogFallback(occurrence)) return;

  ESP_LOGW("mem",
           "ps_alloc fallback to internal heap: %zu bytes count=%u%s%s",
           size,
           static_cast<unsigned>(occurrence),
           tag ? " tag=" : "",
           tag ? tag : "");
}

void reportAllocation(const char* op, void* ptr, size_t size,
                      AllocPolicy policy, const char* tag,
                      bool attemptedPsramFirst) {
  const bool usedPsram = ptr != nullptr && esp_ptr_external_ram(ptr);
  const bool fellBack =
      attemptedPsramFirst && ptr != nullptr && !usedPsram;
  if (fellBack) {
    reportPsramFallback(size, tag);
  }
  memAllocDebug(op, ptr, size, policyRequestsPsram(policy), usedPsram,
                fellBack, tag);
}

void* allocateRaw(size_t size, AllocPolicy policy, bool bypass,
                  bool psramRegistered) {
  switch (policy) {
    case AllocPolicy::DefaultHeap:
      return bypass ? heap_caps_malloc(size, kInternal8BitCaps) : ::malloc(size);

    case AllocPolicy::PreferPsram:
      if (!psramRegistered) {
        return heap_caps_malloc(size, kInternal8BitCaps);
      }
      return heap_caps_malloc_prefer(size, 2,
                                     kPsram8BitCaps, kInternal8BitCaps);

    case AllocPolicy::PreferInternal:
      if (!psramRegistered) {
        return heap_caps_malloc(size, kInternal8BitCaps);
      }
      return heap_caps_malloc_prefer(size, 2,
                                     kInternal8BitCaps, kPsram8BitCaps);

    case AllocPolicy::RequirePsram:
      return psramRegistered ? heap_caps_malloc(size, kPsram8BitCaps) : nullptr;

    case AllocPolicy::RequireInternal:
      return heap_caps_malloc(size, kInternal8BitCaps);
  }
  return nullptr;
}

void* callocateRaw(size_t n, size_t size, AllocPolicy policy, bool bypass,
                   bool psramRegistered) {
  switch (policy) {
    case AllocPolicy::DefaultHeap:
      return bypass ? heap_caps_calloc(n, size, kInternal8BitCaps)
                    : ::calloc(n, size);

    case AllocPolicy::PreferPsram:
      if (!psramRegistered) {
        return heap_caps_calloc(n, size, kInternal8BitCaps);
      }
      return heap_caps_calloc_prefer(n, size, 2,
                                     kPsram8BitCaps, kInternal8BitCaps);

    case AllocPolicy::PreferInternal:
      if (!psramRegistered) {
        return heap_caps_calloc(n, size, kInternal8BitCaps);
      }
      return heap_caps_calloc_prefer(n, size, 2,
                                     kInternal8BitCaps, kPsram8BitCaps);

    case AllocPolicy::RequirePsram:
      return psramRegistered
                 ? heap_caps_calloc(n, size, kPsram8BitCaps)
                 : nullptr;

    case AllocPolicy::RequireInternal:
      return heap_caps_calloc(n, size, kInternal8BitCaps);
  }
  return nullptr;
}

void* reallocateRaw(void* ptr, size_t size, AllocPolicy policy, bool bypass,
                    bool psramRegistered) {
  switch (policy) {
    case AllocPolicy::DefaultHeap:
      return bypass ? heap_caps_realloc(ptr, size, kInternal8BitCaps)
                    : ::realloc(ptr, size);

    case AllocPolicy::PreferPsram:
      if (!psramRegistered) {
        return heap_caps_realloc(ptr, size, kInternal8BitCaps);
      }
      return heap_caps_realloc_prefer(ptr, size, 2,
                                      kPsram8BitCaps, kInternal8BitCaps);

    case AllocPolicy::PreferInternal:
      if (!psramRegistered) {
        return heap_caps_realloc(ptr, size, kInternal8BitCaps);
      }
      return heap_caps_realloc_prefer(ptr, size, 2,
                                      kInternal8BitCaps, kPsram8BitCaps);

    case AllocPolicy::RequirePsram:
      return psramRegistered
                 ? heap_caps_realloc(ptr, size, kPsram8BitCaps)
                 : nullptr;

    case AllocPolicy::RequireInternal:
      return heap_caps_realloc(ptr, size, kInternal8BitCaps);
  }
  return nullptr;
}

}  // namespace

std::atomic_bool& psramBypassGlobal() {
  static std::atomic_bool bypass{false};
  return bypass;
}

uint32_t psAllocFallbackCount() {
  return gPsAllocFallbacks.load(std::memory_order_relaxed);
}

bool psramAvailableRuntime() {
  if (!hasPsramAvail()) return false;
  return heap_caps_get_total_size(kPsram8BitCaps) > 0;
}

void* ps_alloc(size_t size, AllocPolicy policy, const char* tag) {
  if (size == 0) return nullptr;

  const bool bypass = psramBypassEnabled();
  const bool psramRegistered = !bypass && policyMayUsePsram(policy) &&
                               psramAvailableRuntime();
  void* ptr = allocateRaw(size, policy, bypass, psramRegistered);
  reportAllocation("malloc", ptr, size, policy, tag,
                   policy == AllocPolicy::PreferPsram && psramRegistered);
  return ptr;
}

void* ps_calloc(size_t n, size_t size, AllocPolicy policy, const char* tag) {
  if (n == 0 || size == 0) return nullptr;

  size_t total = 0;
  if (__builtin_mul_overflow(n, size, &total)) return nullptr;

  const bool bypass = psramBypassEnabled();
  const bool psramRegistered = !bypass && policyMayUsePsram(policy) &&
                               psramAvailableRuntime();
  void* ptr = callocateRaw(n, size, policy, bypass, psramRegistered);
  reportAllocation("calloc", ptr, total, policy, tag,
                   policy == AllocPolicy::PreferPsram && psramRegistered);
  return ptr;
}

void* ps_realloc(void* ptr, size_t size, AllocPolicy policy, const char* tag) {
  // ESP-IDF heap_caps_realloc(p, 0, ...) frees p. Falling through to a second
  // realloc after that would double-free, so normalize the contract up front.
  if (size == 0) {
    ps_free(ptr);
    return nullptr;
  }

  const bool bypass = psramBypassEnabled();
  const bool psramRegistered = !bypass && policyMayUsePsram(policy) &&
                               psramAvailableRuntime();
  void* result = reallocateRaw(ptr, size, policy, bypass, psramRegistered);
  // For size > 0, both libc realloc and heap_caps_realloc preserve ptr when
  // they return nullptr. The wrapper never frees ptr on this failure path.
  reportAllocation("realloc", result, size, policy, tag,
                   policy == AllocPolicy::PreferPsram && psramRegistered);
  return result;
}

void ps_free(void* ptr) {
  // ESP-IDF's free() delegates to heap_caps_free(), so this handles blocks from
  // either explicit-cap pool as well as libc's default heap.
  ::free(ptr);
}

void* PsramJsonAllocator::allocate(size_t size) {
  return ps_alloc(size, AllocPolicy::PreferPsram, kJsonPoolTag);
}

void PsramJsonAllocator::deallocate(void* ptr) {
  ps_free(ptr);
}

void* PsramJsonAllocator::reallocate(void* ptr, size_t newSize) {
  return ps_realloc(ptr, newSize, AllocPolicy::PreferPsram, kJsonPoolTag);
}

PsramJsonAllocator* PsramJsonAllocator::instance() {
  static PsramJsonAllocator allocator;
  return &allocator;
}
