#include "quartznet_runtime.h"
#include "quartznet_frontend.h"
#include "stt_model_identity.h"
#include "stt_model_container.h"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>
namespace hw1::stt {
// Per-stage inference time accumulated across segments (sttperf stages).
static constexpr int kStageProfileMax = 256;
static uint32_t gStageTotalUs[kStageProfileMax];
static uint32_t gStageSegments = 0;
size_t stageProfile(uint32_t* totalUs, size_t cap, uint32_t* segments) {
    const size_t n = std::min<size_t>(cap, std::min<int>(identity::kNodeCount, kStageProfileMax));
    for (size_t i = 0; i < n; ++i) totalUs[i] = gStageTotalUs[i];
    if (segments) *segments = gStageSegments;
    return n;
}
void stageProfileReset() {
    for (auto& v : gStageTotalUs) v = 0;
    gStageSegments = 0;
}
namespace {
static_assert(kSampleRate==identity::kSampleRate && kMaxSamples==identity::kMaxSamples);
uint32_t millis() { return uint32_t(esp_timer_get_time()/1000); }
// Inference is deliberately CPU-bound on both cores: ESP-DL runs each layer
// on two workers at this task's priority, so core 0's idle task can go
// unscheduled for several seconds on a long 15x5 segment and the task
// watchdog reports it (a false alarm; nothing is stuck). For the duration
// of one inference only, stop watching IDLE0; everything else keeps the
// sdkconfig watchdog settings, which are restored exactly afterwards.
// Lowering the workers' priority instead halved inference speed while BLE
// was busy on core 0 (measured 2026-09-30), so priority is left unchanged.
class InferenceWatchdogScope {
 public:
    InferenceWatchdogScope() {
#if CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
        active_ = esp_task_wdt_reconfigure(&config(false)) == ESP_OK;
#endif
    }
    ~InferenceWatchdogScope() {
#if CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
        if(active_) esp_task_wdt_reconfigure(&config(true));
#endif
    }
    InferenceWatchdogScope(const InferenceWatchdogScope&)=delete;
    InferenceWatchdogScope& operator=(const InferenceWatchdogScope&)=delete;
 private:
#if CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    static const esp_task_wdt_config_t& config(bool watchCore0) {
        static esp_task_wdt_config_t c;
        c.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
        c.idle_core_mask = (watchCore0 ? 1u : 0u)
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
                         | 2u
#endif
                         ;
#if CONFIG_ESP_TASK_WDT_PANIC
        c.trigger_panic = true;
#else
        c.trigger_panic = false;
#endif
        return c;
    }
#endif
    bool active_ = false;
};
bool cancelled(const STTLocalControl& c) { return c.cancelled && c.cancelled(c.context); }
void progress(const STTLocalControl& c, STTLocalPhase phase) { if(c.progress)c.progress(c.context,phase); }
struct Buffer {
    void* p = nullptr;
    size_t bytes = 0;
    bool sensitive = false;
    explicit Buffer(size_t n, bool secret=false):bytes(n),sensitive(secret) {
        p=heap_caps_aligned_alloc(16,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
    ~Buffer() { if(sensitive && p) { volatile uint8_t* d=static_cast<volatile uint8_t*>(p); for(size_t i=0;i<bytes;++i)d[i]=0; } heap_caps_free(p); }
    void* release() { void* value=p; p=nullptr; return value; }
    Buffer(const Buffer&)=delete;
    Buffer& operator=(const Buffer&)=delete;
};
bool readExact(ModelReader& r, void* dest, size_t size) {
    auto p=static_cast<uint8_t*>(dest);
    while(size) { size_t n=r.read(r.context,p,size); if(!n || n>size)return false; p+=n;size-=n; }
    return true;
}
bool digest(const void* data, size_t bytes, uint8_t* hash) {
    return mbedtls_sha256(static_cast<const unsigned char*>(data),bytes,hash,0)==0;
}
// ROM streaming inflate uses a non-wrapping output buffer of exactly the pinned
// size. No untrusted length reaches ESP-DL's unchecked flatbuffer parser.
// Time spent inside reader calls during the current load (card/file I/O only).
int64_t gLoadReadUs = 0;
bool timedRead(ModelReader& r, void* dest, size_t size) {
    const int64_t start=esp_timer_get_time();
    const bool ok=readExact(r,dest,size);
    gLoadReadUs+=esp_timer_get_time()-start;
    return ok;
}
// Stored (codec 0) graph: read straight into the final buffer in large blocks.
// A 20 MB graph in 4 KB reads spends most of its time in per-call file-system
// and SDMMC overhead; 256 KB transfers let the driver stream multi-block DMA.
bool readStoredModel(ModelReader& r, uint8_t* raw, const STTLocalControl& control) {
    constexpr size_t kBlock=256*1024;
    for(size_t done=0;done<identity::kRawBytes;) {
        if(cancelled(control))return false;
        const size_t n=std::min(kBlock,identity::kRawBytes-done);
        if(!timedRead(r,raw+done,n))return false;
        done+=n;
        vTaskDelay(1);
    }
    return true;
}
bool inflateModel(ModelReader& r, uint8_t* raw, const STTLocalControl& control) {
    Buffer workspace(sizeof(tinfl_decompressor)+4096);
    if(!workspace.p)return false;
    auto dec=static_cast<tinfl_decompressor*>(workspace.p);
    auto in=reinterpret_cast<uint8_t*>(dec+1);
    tinfl_init(dec);
    size_t left=identity::kCompressedBytes, available=0, offset=0, output=0, chunks=0;
    for(;;) {
        if(cancelled(control))return false;
        if(available==offset && left) {
            available=std::min(left,size_t(4096));offset=0;
            if(!timedRead(r,in,available))return false;
            left-=available;
        }
        size_t n=available-offset, out=identity::kRawBytes-output;
        uint32_t flags=TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if(left)flags|=TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status status=tinfl_decompress(dec,in+offset,&n,raw,raw+output,&out,flags);
        offset+=n;output+=out;
        if(status==TINFL_STATUS_DONE)return output==identity::kRawBytes && !left && offset==available;
        if(status<0 || (n==0 && out==0) || output>=identity::kRawBytes)return false;
        if(!left && offset==available && status==TINFL_STATUS_NEEDS_MORE_INPUT)return false;
        if(++chunks%32==0)vTaskDelay(1);
    }
}
bool tensor(dl::TensorBase* t, size_t frames, size_t channels, int exponent) {
    return t && t->data && t->dtype==dl::DATA_TYPE_INT8 && t->exponent.is_valid()
        && !t->exponent.is_per_channel() && int(t->exponent)==exponent
        && t->shape==std::vector<int>({1,int(frames),1,int(channels)})
        && t->get_bytes()==int(frames*channels);
}
}
ModelCache::~ModelCache() { reset(); }
void ModelCache::reset() { heap_caps_free(raw_); raw_=nullptr; }
#if HW1_STT_RUNTIME_DIAGNOSTICS
bool ModelCache::verify() const {
    uint8_t hash[32];
    return raw_ && digest(raw_,identity::kRawBytes,hash) && !memcmp(hash,identity::kRawSha,32);
}
#endif

bool transcribe(ModelReader reader, const int16_t* pcm, size_t samples,
                char* text, size_t capacity, const STTLocalControl& control,
                STTLocalStats& stats, char* error, size_t errorCapacity,
                Diagnostics* diagnostics, ModelCache* cache, const Decoder* decoder) {
    stats={}; if(text && capacity)text[0]=0;if(error && errorCapacity)error[0]=0;
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics)*diagnostics={};
#else
    (void)diagnostics;
#endif
    auto fail=[&](const char* why) { if(text && capacity)text[0]=0; if(error && errorCapacity)snprintf(error,errorCapacity,"%s",why);return false; };
    if(!pcm || !text || capacity<2 || !reader.read || samples<kMinSamples || samples>kMaxSamples)return fail("Invalid STT audio buffer");
    if(cancelled(control))return fail("Cancelled");
    const size_t frames=feature_frames(samples), outputs=(frames+1)/2;
    stats.samples=samples;stats.featureFrames=frames;stats.outputFrames=outputs;
    if(!container::valid_size(reader.bytes))return fail("STT model size does not match this firmware");
    progress(control,STTLocalPhase::Loading);uint32_t start=millis();
    if(!container::read_header(reader.context,reader.bytes,reader.read))return fail("STT model identity mismatch");
    // This pinned graph's 16-byte greedy arena is 2048*ceil(T/2), including
    // residual lifetimes/fragmentation. Keep extra alignment and metadata/
    // kernel-scratch margins: vendor constructors do not report every OOM.
    const size_t arenaBudget=2048*outputs+64*1024;
    const size_t frontendBytes=frames*kMelBins*sizeof(float)+sizeof(FrontendWorkspace);
    constexpr size_t metadataMargin=1024*1024, internalMinimum=150*1024;
    const size_t remainingBudget=arenaBudget+frontendBytes+metadataMargin;
    // A null cache keeps one-shot calls self-contained. Continuous sessions
    // retain this allocation only until their worker has joined/closed.
    ModelCache temporary;
    ModelCache& weights=cache ? *cache : temporary;
    const bool reused=weights.raw_!=nullptr;
    const size_t newWeights=reused ? 0 : identity::kRawBytes;
    // Optional decoder memory (a cached LM) yields before any refusal.
    auto shed=[&] { return decoder && decoder->release && decoder->release(decoder->context); };
    auto admitted=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=newWeights+remainingBudget
            && (reused || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=identity::kRawBytes)
            && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=internalMinimum;
    };
    if(!admitted() && (!shed() || !admitted()))
        return fail("Not enough free memory; stop camera, speech and other large models");
    if(!reused) {
        Buffer candidate(identity::kRawBytes);
        if(!candidate.p)return fail("Cannot allocate STT model");
        gLoadReadUs=0;
        const int64_t loadStart=esp_timer_get_time();
        const bool loaded=identity::kCodec==0
            ? readStoredModel(reader,static_cast<uint8_t*>(candidate.p),control)
            : inflateModel(reader,static_cast<uint8_t*>(candidate.p),control);
        if(!loaded)return fail(cancelled(control)?"Cancelled":"STT model read failed");
        const int64_t hashStart=esp_timer_get_time();
        uint8_t hash[32];
        if(!digest(candidate.p,identity::kRawBytes,hash) || memcmp(hash,identity::kRawSha,32))return fail("STT model checksum mismatch");
        const int64_t hashEnd=esp_timer_get_time();
        // Once per session (weights are cached after this), so always logged.
        ESP_LOGI("STT","model %u bytes %s: read %lu ms, inflate %lu ms, sha256 %lu ms, total %lu ms",
                 unsigned(identity::kRawBytes),identity::kCodec==0?"stored":"zlib",
                 (unsigned long)(gLoadReadUs/1000),
                 (unsigned long)((hashStart-loadStart-gLoadReadUs)/1000),
                 (unsigned long)((hashEnd-hashStart)/1000),
                 (unsigned long)((hashEnd-loadStart)/1000));
        if(cancelled(control))return fail("Cancelled");
        weights.raw_=candidate.release(); // publish only fully verified weights
    }
    // Cached weights were fully verified (SHA-256) when this session loaded
    // them. Re-hashing all 7.2 MB before every segment cost ~0.6 s per
    // segment on the P4 (sttperf), so production reuse trusts that check;
    // diagnostics probes keep the per-segment re-validation.
#if HW1_STT_RUNTIME_DIAGNOSTICS
    else {
        uint8_t hash[32];
        if(!digest(weights.raw_,identity::kRawBytes,hash) || memcmp(hash,identity::kRawSha,32)) {
            weights.reset();
            return fail("Cached STT model checksum mismatch");
        }
    }
#endif
    stats.modelBytes=identity::kRawBytes;
    stats.weightsReused=reused;
    if(cancelled(control))return fail("Cancelled");
    // Raw model allocation can split the largest block. Check again before
    // the vendor constructor requests a contiguous arena and creates metadata.
    auto contiguous=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=remainingBudget
            && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=arenaBudget+metadataMargin
            && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=internalMinimum;
    };
    if(!contiguous() && (!shed() || !contiguous()))
        return fail("Not enough contiguous memory for STT activations");
    // A fresh model per utterance avoids Model::build's resize allocation leak.
    // Aligned EDL2+ in PSRAM allows zero-copy parameters; weights outlive model.
    std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(frames),1,64}}};
    std::unique_ptr<dl::Model> model(new(std::nothrow) dl::Model(
        static_cast<const char*>(weights.raw_),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
        dl::MEMORY_MANAGER_GREEDY,nullptr,false,shapes));
    if(!model)return fail("Cannot create STT model");
    auto input=model->get_input(identity::kInputName);auto output=model->get_output(identity::kOutputName);
    if(!tensor(input,frames,64,identity::kInputExponent) || !tensor(output,outputs,29,identity::kOutputExponent))return fail("STT runtime tensor mismatch");
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics) {
        const auto memory=model->get_memory_info();
        const auto variable=memory.find("variable");
        if(variable!=memory.end()) {
            diagnostics->activationPsramBytes=variable->second.psram;
            diagnostics->activationInternalBytes=variable->second.internal;
        }
    }
#endif
    stats.loadMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Frontend);start=millis();
    {
        Buffer features(frames*64*sizeof(float),true), workspace(sizeof(FrontendWorkspace),true);
        if(!features.p || !workspace.p)return fail("Cannot allocate STT frontend");
        auto f=static_cast<float*>(features.p);
        FrontendTiming timing;
        timing.now_us=[]() -> uint64_t { return static_cast<uint64_t>(esp_timer_get_time()); };
        // No dither on device: the microphone noise floor is ~10x its 1e-5
        // amplitude, and the Gaussian generator cost ~1.2 s per 8 s segment.
        if(compute_features(pcm,samples,f,frames*64,static_cast<FrontendWorkspace*>(workspace.p),
                            kDitherSeed,false,&timing)!=Status::Ok)return fail("STT frontend failed");
        stats.frontendConditionUs=timing.conditionUs; stats.frontendFftUs=timing.fftUs;
        stats.frontendMelUs=timing.melUs; stats.frontendNormUs=timing.normUs;
        const int64_t quantizeStart=esp_timer_get_time();
        auto q=static_cast<int8_t*>(input->data);
        const float scale=std::ldexp(1.0f,-identity::kInputExponent);
        for(size_t i=0;i<frames*64;++i) {
            // nearest-even, matching ESP-PPQ/P4 quantization, independent of
            // ambient FPU rounding mode (roundf would round ties away).
            float x=f[i]*scale, lo=std::floor(x), part=x-lo;
            int value=part>0.5f?int(lo)+1:part<0.5f?int(lo):(int(lo)&1)?int(lo)+1:int(lo);
            q[i]=static_cast<int8_t>(std::max(-128,std::min(127,value)));
        }
        stats.quantizeUs=static_cast<uint32_t>(esp_timer_get_time()-quantizeStart);
    }
    stats.frontendMs=millis()-start;
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics) {
        digest(input->data,input->get_bytes(),diagnostics->inputSha);
        diagnostics->freePsramAtInference=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        diagnostics->freeInternalAtInference=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
#endif
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Inference);start=millis();
    InferenceWatchdogScope watchdogScope;
    for(int stage=0;stage<identity::kNodeCount;++stage) {
        if(cancelled(control))return fail("Cancelled");
        const int64_t stageStart=esp_timer_get_time();
        model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
        const int64_t stageEnd=esp_timer_get_time();
        // Keep the three slowest stages (sorted, slowest first) for sttperf.
        uint32_t us=static_cast<uint32_t>(stageEnd-stageStart);
        uint16_t index=static_cast<uint16_t>(stage);
        for(int k=0;k<3;++k) if(stats.slowStage[k]==0xffff || us>stats.slowStageUs[k]) {
            std::swap(us,stats.slowStageUs[k]); std::swap(index,stats.slowStage[k]);
        }
        if(stage<kStageProfileMax){ gStageTotalUs[stage]+=static_cast<uint32_t>(stageEnd-stageStart); }
        // Yield every 5 stages (was every stage: 201 x ~1 ms on the 15x5).
        if((stage+1)%5==0 || stage+1==identity::kNodeCount) {
            vTaskDelay(1);
            stats.inferenceYieldUs+=static_cast<uint32_t>(esp_timer_get_time()-stageEnd);
        }
    }
    ++gStageSegments;
    stats.inferenceMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Decoding);start=millis();
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics)digest(output->data,output->get_bytes(),diagnostics->outputSha);
#endif
    // Any decoder failure other than cancellation decodes greedily instead.
    const auto logits=static_cast<const int8_t*>(output->data);
    DecodeResult decoded=decoder && decoder->decode
        ? decoder->decode(decoder->context,logits,outputs,identity::kOutputExponent,text,capacity,control)
        : DecodeResult::Fallback;
    if(decoded==DecodeResult::Cancelled)return fail("Cancelled");
    if(decoded==DecodeResult::Decoded && !memchr(text,0,capacity))decoded=DecodeResult::Fallback;
    if(decoded!=DecodeResult::Decoded) {
        CtcState ctc;
        if(ctc_reset(&ctc,text,capacity)!=Status::Ok
           || ctc_feed(&ctc,logits,outputs,29,text,capacity)!=Status::Ok
           || ctc_finish(&ctc,text,capacity)!=Status::Ok || ctc.truncated)return fail("STT transcript exceeds result capacity");
    }
    stats.lmUsed=decoded==DecodeResult::Decoded;
    stats.decodeMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
    // Model destructor releases its arena before temporary weights are freed.
    // Session weights remain immutable and are rechecked on the next call.
    return true;
}
}
