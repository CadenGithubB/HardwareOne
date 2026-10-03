#include "System_BuildConfig.h"
#include "System_STTLocal.h"
#if ENABLE_LOCAL_STT
#include <cstdio>
#include <new>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "System_VFS.h"
#include "stt/quartznet_runtime.h"
#include "stt/stt_model_identity.h"
#if ENABLE_STT_LM
#include "System_Debug.h"
#include "esp_heap_caps.h"
#include "stt/stt_lm.h"
#include <algorithm>
#include <atomic>
#endif
namespace {
// A build profile may relocate the model (e.g. a larger model on the SD card).
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/STT Models/quartznet5x5.p4.stt"
#endif
constexpr const char* kModelPath=HW1_STT_MODEL_PATH;
File openModel() {
    // trusted: fixed, pinned inference weights; no caller-selected file path.
    return VFS::openGuarded(kModelPath,"r",VFS::systemAuth("stt.model_read"));
}
size_t readModel(void* context,void* dest,size_t bytes) {
    return static_cast<File*>(context)->read(static_cast<uint8_t*>(dest),bytes);
}
#if ENABLE_STT_LM
// Optional word n-gram LM beside the model. It is loaded at the first decode
// of a session (or one-shot call), after that call's weights and arena exist,
// so it only changes how a call decodes, never whether it runs: any problem
// logs once per boot and the runtime decodes greedily. The runtime may also
// release it to admit a later call under memory pressure.
namespace lm=hw1::stt::lm;
// The microSD copy wins when present, so the LM can be swapped with the card
// (like the 15x5 model) and the onboard copy removed to free LittleFS.
struct LmFiles { const char* lm; const char* words; };
constexpr LmFiles kLmFiles[]={
    {"/sd/STT Models/meeting.lm","/sd/STT Models/custom_words.txt"},
    {"/STT Models/meeting.lm","/STT Models/custom_words.txt"},
};
// Free PSRAM kept beside a resident LM: a warm worst-case (30 s) runtime call
// needs ~5 MiB; the beam workspace is separately checked per decode.
constexpr size_t kLmMaxBytes=32u<<20, kWordsMaxBytes=64u<<10, kLmReserveBytes=6u<<20;
enum class LmState : uint8_t { Unloaded, Ready, Unavailable };
enum LmNote : uint32_t { kLmMissing=1, kLmInvalid=2, kLmMemory=4, kLmLoaded=8, kLmDecode=16, kLmShed=32, kLmWords=64 };
std::atomic<uint32_t> gLmNoted{0};
bool firstNote(LmNote note) { return !(gLmNoted.fetch_or(note)&note); }
struct LanguageModel {
    uint8_t* blob=nullptr;
    lm::CustomWords* words=nullptr;   // PSRAM, trivially destructible
    lm::Lm view;
    const LmFiles* files=nullptr;      // where the loaded copy came from
    LmState state=LmState::Unloaded;
    ~LanguageModel() { reset(); }
    void reset() { view=lm::Lm{}; heap_caps_free(words); words=nullptr; heap_caps_free(blob); blob=nullptr; }
};
bool stopRequested(const STTLocalControl& c) { return c.cancelled && c.cancelled(c.context); }
bool readAll(File& file,uint8_t* dest,size_t bytes,const STTLocalControl& control) {
    for(size_t done=0;done<bytes;) {
        if(stopRequested(control))return false;
        const size_t n=file.read(dest+done,std::min(bytes-done,size_t(64*1024)));
        if(!n || n>bytes-done)return false;
        done+=n;
    }
    return true;
}
void loadCustomWords(LanguageModel& model) {
    // trusted: fixed optional hotword list beside the model.
    const auto auth=VFS::systemAuth("stt.lm_words_read");
    const char* kWordsPath=model.files->words;   // beside the LM that was loaded
    if(!VFS::existsGuarded(kWordsPath,auth))return;
    File file=VFS::openGuarded(kWordsPath,"r",auth);
    const size_t bytes=file && !file.isDirectory() ? file.size() : 0;
    char* text=bytes && bytes<=kWordsMaxBytes ? static_cast<char*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)) : nullptr;
    void* storage=text ? heap_caps_malloc(sizeof(lm::CustomWords),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : nullptr;
    if(storage && readAll(file,reinterpret_cast<uint8_t*>(text),bytes,{})) {
        model.words=new(storage) lm::CustomWords;
        if(!model.words->parse(text,bytes)) { heap_caps_free(storage); model.words=nullptr; }
    } else {
        heap_caps_free(storage);
        if(firstNote(kLmWords))WARN_SYSTEMF("[STT] Ignoring %s (empty, over %u bytes, unreadable or no memory)",kWordsPath,unsigned(kWordsMaxBytes));
    }
    heap_caps_free(text);
}
// One attempt per LanguageModel lifetime (state Ready or Unavailable after);
// only cancellation returns false and leaves it Unloaded for a later retry.
bool loadLanguageModel(LanguageModel& model,const STTLocalControl& control) {
    model.state=LmState::Unavailable;
    model.files=nullptr;
    // trusted: fixed LM path beside the pinned model; content is SHA-checked.
    const auto auth=VFS::systemAuth("stt.lm_read");
    for(const LmFiles& f:kLmFiles)
        if(VFS::existsGuarded(f.lm,auth)) { model.files=&f; break; }
    if(!model.files) {
        if(firstNote(kLmMissing))INFO_SYSTEMF("[STT] No %s or %s; using greedy decoding",kLmFiles[0].lm,kLmFiles[1].lm);
        return true;
    }
    const char* kLmPath=model.files->lm;
    File file=VFS::openGuarded(kLmPath,"r",auth);
    const size_t bytes=file && !file.isDirectory() ? file.size() : 0;
    if(bytes<96 || bytes>kLmMaxBytes) {
        if(firstNote(kLmInvalid))WARN_SYSTEMF("[STT] %s unreadable or wrong size (%u bytes); using greedy decoding",kLmPath,unsigned(bytes));
        return true;
    }
    if(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)<bytes
       || heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<bytes+kLmReserveBytes
       || !(model.blob=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)))) {
        if(firstNote(kLmMemory))WARN_SYSTEMF("[STT] Not enough PSRAM for %s (%u bytes); using greedy decoding",kLmPath,unsigned(bytes));
        return true;
    }
    if(!readAll(file,model.blob,bytes,control)) {
        model.reset();
        if(stopRequested(control)) { model.state=LmState::Unloaded; return false; }
        if(firstNote(kLmInvalid))WARN_SYSTEMF("[STT] Reading %s failed; using greedy decoding",kLmPath);
        return true;
    }
    const lm::LoadStatus status=model.view.open(model.blob,bytes);
    if(status!=lm::LoadStatus::Ok) {
        model.reset();
        if(firstNote(kLmInvalid))WARN_SYSTEMF("[STT] Rejected %s: %s; using greedy decoding",kLmPath,lm::describe(status));
        return true;
    }
    loadCustomWords(model);
    model.state=LmState::Ready;
    if(firstNote(kLmLoaded))
        INFO_SYSTEMF("[STT] Language model %s: %u words, %u bigrams, %u trigrams, beam %u, %u custom words, %u bytes",
                     kLmPath,unsigned(model.view.vocab()),unsigned(model.view.bigrams()),unsigned(model.view.trigrams()),
                     unsigned(model.view.beamWidth()),unsigned(model.words ? model.words->size() : 0),unsigned(bytes));
    return true;
}
hw1::stt::DecodeResult decodeWithLanguageModel(void* context,const int8_t* logits,size_t frames,int exponent,
                                               char* text,size_t capacity,const STTLocalControl& control) {
    using hw1::stt::DecodeResult;
    auto& model=*static_cast<LanguageModel*>(context);
    if(model.state==LmState::Unloaded && !loadLanguageModel(model,control))return DecodeResult::Cancelled;
    if(model.state!=LmState::Ready)return DecodeResult::Fallback;
    const lm::DecodeStatus status=lm::decode(logits,frames,exponent,model.view,model.words,text,capacity,
                                             {control.context,control.cancelled});
    if(status==lm::DecodeStatus::Ok)return DecodeResult::Decoded;
    if(status==lm::DecodeStatus::Cancelled)return DecodeResult::Cancelled;
    // Workspace allocation failure or an over-long result: greedy decides.
    if(firstNote(kLmDecode))WARN_SYSTEMF("[STT] Language-model decode failed (%d, %u frames, %u workspace bytes); using greedy decoding",
                                          int(status),unsigned(frames),unsigned(lm::workspace_bytes(frames,model.view.beamWidth())));
    return DecodeResult::Fallback;
}
bool releaseLanguageModel(void* context) {
    auto& model=*static_cast<LanguageModel*>(context);
    if(model.state!=LmState::Ready)return false;
    model.reset(); model.state=LmState::Unavailable; // no reload thrash in this session
    if(firstNote(kLmShed))WARN_SYSTEMF("[STT] Released language model to admit transcription; greedy until the session restarts");
#ifdef ESP_PLATFORM
    // Always on the console: an LM too large for PSRAM silently costs accuracy otherwise.
    ESP_LOGW("STT","TL lm_released: PSRAM short for inference; greedy decoding for the rest of this session");
#endif
    return true;
}
#endif
// Session-owned backend state: verified weights plus the optional LM.
struct Backend {
    hw1::stt::ModelCache weights;
#if ENABLE_STT_LM
    LanguageModel lm;
#endif
};
}
bool sttLocalAvailable(char* error,size_t cap) {
    if(error&&cap)error[0]=0;
    File file=openModel();
    const bool ok=file&&!file.isDirectory()&&file.size()==96+hw1::stt::identity::kCompressedBytes;
    if(!ok&&error&&cap)snprintf(error,cap,"Install the matching local STT model in /STT Models");
    return ok;
}
bool sttLocalTranscribe(const int16_t* pcm,size_t samples,char* text,size_t textCap,
                       const STTLocalControl& control,STTLocalStats& stats,
                       char* error,size_t errorCap,STTLocalSession* session) {
    stats={};
    File file=openModel();
    if(!file||file.isDirectory()) {
        if(text&&textCap)text[0]=0;
        if(error&&errorCap)snprintf(error,errorCap,"Cannot open local STT model");
        return false;
    }
    if(session && !session->backendState_) {
        session->backendState_=new(std::nothrow) Backend;
        if(!session->backendState_) {
            if(text&&textCap)text[0]=0;
            if(error&&errorCap)snprintf(error,errorCap,"Cannot allocate STT session");
            return false;
        }
    }
    Backend* backend=session ? static_cast<Backend*>(session->backendState_) : nullptr;
#if ENABLE_STT_LM
    // One-shot calls load (and free) the LM for this call only, like weights.
    LanguageModel oneShot;
    LanguageModel& model=backend ? backend->lm : oneShot;
    const hw1::stt::Decoder decoder{&model,decodeWithLanguageModel,releaseLanguageModel};
    const hw1::stt::Decoder* decoding=&decoder;
#else
    const hw1::stt::Decoder* decoding=nullptr;
#endif
    return hw1::stt::transcribe({&file,file.size(),readModel},pcm,samples,text,textCap,control,stats,error,errorCap,
                              nullptr,backend ? &backend->weights : nullptr,decoding);
}
#else
bool sttLocalAvailable(char* error,size_t cap) {
    if(error&&cap)snprintf(error,cap,"No local STT backend is qualified for this target");
    return false;
}
bool sttLocalTranscribe(const int16_t*,size_t,char* text,size_t cap,const STTLocalControl&,
                       STTLocalStats& stats,char* error,size_t errorCap,STTLocalSession*) {
    if(text&&cap)text[0]=0;stats={};return sttLocalAvailable(error,errorCap);
}
#endif
STTLocalSession::~STTLocalSession() { reset(); }
void STTLocalSession::reset() {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    delete static_cast<Backend*>(backendState_);
#endif
    backendState_=nullptr;
}
#endif
