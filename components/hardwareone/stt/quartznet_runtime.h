#pragma once
#include "System_STTLocal.h"
#include <cstddef>
#include <cstdint>
// Tensor hashes and vendor memory profiling are probe-only; production keeps
// its user-facing timings/status without retaining this optional code.
#ifndef HW1_STT_RUNTIME_DIAGNOSTICS
#define HW1_STT_RUNTIME_DIAGNOSTICS 0
#endif
namespace hw1::stt {
// A read-only stream already opened by the caller's storage/auth boundary.
struct ModelReader {
    void* context;
    size_t bytes;
    size_t (*read)(void*, void*, size_t);
};
// Ignored unless the runtime is compiled with HW1_STT_RUNTIME_DIAGNOSTICS=1.
struct Diagnostics {
    uint8_t inputSha[32]{}, outputSha[32]{};
    size_t activationPsramBytes = 0;
    size_t activationInternalBytes = 0;
    size_t freePsramAtInference = 0;
    size_t freeInternalAtInference = 0;
};
bool transcribe(ModelReader reader, const int16_t* pcm, size_t samples,
                char* text, size_t capacity, const STTLocalControl& control,
                STTLocalStats& stats, char* error, size_t errorCapacity,
                Diagnostics* diagnostics = nullptr);
}
