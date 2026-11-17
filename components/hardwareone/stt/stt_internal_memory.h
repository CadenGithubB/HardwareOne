#pragma once
#include <cstddef>
#include <cstdint>

namespace hw1::stt {
// One task's allocations during a single transcribe call, not a system-heap
// subtraction. Incomplete/failed observations never relax admission.
struct InternalMemorySample {
    size_t peak = 0, largest = 0;
    bool complete = false;
};
struct InternalMemoryBudget { size_t total, contiguous; };
InternalMemoryBudget internalMemoryBudget(bool resident);
class InternalMemoryScope {
 public:
    InternalMemoryScope();
    ~InternalMemoryScope();
    void success(bool resident);
    InternalMemoryScope(const InternalMemoryScope&) = delete;
    InternalMemoryScope& operator=(const InternalMemoryScope&) = delete;
 private:
    bool active_ = false;
};
// Diagnostic builds only (HW1_STT_HEAP_WATCH=1: the internal heap, 2: PSRAM blocks of 4 KiB
// and more; stt_heap_watch.inc): record every task's allocations in that heap from
// heapWatchBegin() and log who held them at its low point.
#ifndef HW1_STT_HEAP_WATCH
#define HW1_STT_HEAP_WATCH 0
#endif
#if HW1_STT_HEAP_WATCH
void heapWatchBegin();
void heapWatchReport(const char* when);
#endif
}
