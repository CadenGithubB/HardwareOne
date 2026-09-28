#include "quartznet_runtime.h"
#include "quartznet_frontend.h"
#include "stt_model_identity.h"
#include "stt_model_container.h"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
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
namespace {
static_assert(kSampleRate==identity::kSampleRate && kMaxSamples==identity::kMaxSamples);
uint32_t millis() { return uint32_t(esp_timer_get_time()/1000); }
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
            if(!readExact(r,in,available))return false;
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
bool transcribe(ModelReader reader, const int16_t* pcm, size_t samples,
                char* text, size_t capacity, const STTLocalControl& control,
                STTLocalStats& stats, char* error, size_t errorCapacity,
                Diagnostics* diagnostics) {
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
    if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<identity::kRawBytes+remainingBudget
       || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)<identity::kRawBytes
       || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internalMinimum)
        return fail("Not enough free memory; stop camera, speech and other large models");
    Buffer raw(identity::kRawBytes);
    if(!raw.p)return fail("Cannot allocate STT model");
    if(!inflateModel(reader,static_cast<uint8_t*>(raw.p),control))return fail(cancelled(control)?"Cancelled":"STT model decompression failed");
    uint8_t hash[32];
    if(!digest(raw.p,identity::kRawBytes,hash) || memcmp(hash,identity::kRawSha,32))return fail("STT model checksum mismatch");
    stats.modelBytes=identity::kRawBytes;
    if(cancelled(control))return fail("Cancelled");
    // Raw model allocation can split the largest block. Check again before
    // the vendor constructor requests a contiguous arena and creates metadata.
    if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<remainingBudget
       || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)<arenaBudget+metadataMargin
       || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internalMinimum)
        return fail("Not enough contiguous memory for STT activations");
    // A fresh model per utterance avoids Model::build's resize allocation leak.
    // Aligned EDL2+ in PSRAM allows zero-copy parameters; raw outlives model.
    std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(frames),1,64}}};
    std::unique_ptr<dl::Model> model(new(std::nothrow) dl::Model(
        static_cast<const char*>(raw.p),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
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
        if(compute_features(pcm,samples,f,frames*64,static_cast<FrontendWorkspace*>(workspace.p))!=Status::Ok)return fail("STT frontend failed");
        auto q=static_cast<int8_t*>(input->data);
        const float scale=std::ldexp(1.0f,-identity::kInputExponent);
        for(size_t i=0;i<frames*64;++i) {
            // nearest-even, matching ESP-PPQ/P4 quantization, independent of
            // ambient FPU rounding mode (roundf would round ties away).
            float x=f[i]*scale, lo=std::floor(x), part=x-lo;
            int value=part>0.5f?int(lo)+1:part<0.5f?int(lo):(int(lo)&1)?int(lo)+1:int(lo);
            q[i]=static_cast<int8_t>(std::max(-128,std::min(127,value)));
        }
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
    for(int stage=0;stage<identity::kNodeCount;++stage) {
        if(cancelled(control))return fail("Cancelled");
        model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
        vTaskDelay(1);
    }
    stats.inferenceMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Decoding);start=millis();
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics)digest(output->data,output->get_bytes(),diagnostics->outputSha);
#endif
    CtcState ctc;
    if(ctc_reset(&ctc,text,capacity)!=Status::Ok
       || ctc_feed(&ctc,static_cast<const int8_t*>(output->data),outputs,29,text,capacity)!=Status::Ok
       || ctc_finish(&ctc,text,capacity)!=Status::Ok || ctc.truncated)return fail("STT transcript exceeds result capacity");
    stats.decodeMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
    // Model destructor releases its arena before the backing raw blob is freed.
    return true;
}
}
