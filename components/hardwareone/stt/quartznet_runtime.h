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
// Optional replacement for greedy CTC over the final int8 logits [frames x 29]
// (value = q * 2^exponent), e.g. the HW1LM1 beam search. Decoded: text holds
// a NUL-terminated result. Fallback: greedy runs exactly as with no decoder.
// Cancelled fails the call. release frees optional decoder memory (true if
// any); the runtime tries it before reporting a PSRAM shortfall, so a cached
// language model can never be why a call is refused.
enum class DecodeResult : uint8_t { Decoded, Fallback, Cancelled };
struct Decoder {
    void* context = nullptr;
    DecodeResult (*decode)(void* context, const int8_t* logits, size_t frames, int exponent,
                           char* text, size_t capacity, const STTLocalControl& control) = nullptr;
    bool (*release)(void* context) = nullptr;
};
// Retains only the pinned, verified raw weights. Each call still constructs
// a fresh shape-specific model/arena; vendor resize/rebuild is never reused.
// One worker owns this object. Destroy/reset only after transcribe returns.
class ModelCache {
 public:
    ModelCache() = default;
    ~ModelCache();
    void reset();
#if HW1_STT_RUNTIME_DIAGNOSTICS
    // Probe integrity after the vendor model and arena have been destroyed.
    bool verify() const;
#endif
    ModelCache(const ModelCache&) = delete;
    ModelCache& operator=(const ModelCache&) = delete;
 private:
    void* raw_ = nullptr;
    friend bool transcribe(ModelReader, const int16_t*, size_t, char*, size_t,
                           const STTLocalControl&, STTLocalStats&, char*, size_t,
                           Diagnostics*, ModelCache*, const Decoder*);
};
// Per-stage inference time summed over segments since the last reset
// (single STT worker writes; readers accept a racy snapshot for profiling).
size_t stageProfile(uint32_t* totalUs, size_t cap, uint32_t* segments);
void stageProfileReset();
bool transcribe(ModelReader reader, const int16_t* pcm, size_t samples,
                char* text, size_t capacity, const STTLocalControl& control,
                STTLocalStats& stats, char* error, size_t errorCapacity,
                Diagnostics* diagnostics = nullptr, ModelCache* cache = nullptr,
                const Decoder* decoder = nullptr);
}
