#include "System_BuildConfig.h"
#include "System_STTLocal.h"
#include "System_MemDiag.h"
#if ENABLE_LOCAL_STT
#include <cstdio>
#include <cstring>
#include <new>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "System_VFS.h"
#include "stt/quartznet_runtime.h"
#include "stt/stt_model_selection.h"
#include "esp_log.h"
#include <atomic>
#if HW1_STT_SPEAKER_LABELS
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "stt/speaker_cluster.h"
#include "stt/speaker_model.h"
#include "stt/stt_dl_metadata.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <algorithm>
#include <cmath>
#include <cstring>
extern "C" int hw1_dl_log_kernel_binds;   // ESP-DL (speaker diagnostics): log each kernel bound, once
using hw1::stt::sttPsramAlloc;
using hw1::stt::sttPsramFree;
#endif
#if ENABLE_STT_LM
#include "System_Debug.h"
#include "System_STTVocabulary.h"
#include "esp_heap_caps.h"
#include "stt/stt_lm.h"
#if HW1_STT_CITRINET
#include "stt/citrinet_runtime.h"
#include "stt/stt_lm_wordpiece.h"
#endif
#include <algorithm>
#endif
#if HW1_STT_CITRINET
// The weights' boot-lifetime block, and cutting audio the trimmed image cannot take in one call.
#include "stt/citrinet_runtime.h"
#include "stt/stt_model_slot.h"
#endif
namespace {
// Selection pairs the fixed path with its generated, SHA-pinned identity.
// Explicit path overrides remain available to standalone deployment experiments.
constexpr const char* kModelPath=HW1_STT_MODEL_PATH;
File openModel() {
    // trusted: fixed, pinned inference weights; no caller-selected file path.
    return VFS::openGuarded(kModelPath,"r",VFS::systemAuth(VFS::sttModelScopeFor(kModelPath),"stt.model_read"));
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
static_assert(HW1_STT_LM_STRENGTH>=0.0f && HW1_STT_LM_STRENGTH<=1.0f,
              "HW1_STT_LM_STRENGTH must be finite and within [0, 1]");
#if HW1_STT_CITRINET
// Citrinet decodes meeting.lm through the word-piece prefix beam search (stt/stt_lm_wordpiece.h) with its own
// LM weight and word bonus, tuned on the 600 held-out dev clips of the speech repository's export kit
// (private/citrinet-lm/tune.py: greedy 14.44% -> 12.73%); the file's header values are QuartzNet's. Beam
// width, OOV score, prune and hotword bonus stay the file's. HW1_STT_LM_STRENGTH scales all three as for
// QuartzNet.
constexpr double kCitrinetLmAlpha=0.2, kCitrinetLmBeta=0.75;
lm::wordpiece::Params citrinetParams(const lm::Lm& view) {
    lm::wordpiece::Params p=lm::wordpiece::fileParams(view);
    p.alpha=kCitrinetLmAlpha*HW1_STT_LM_STRENGTH;
    p.beta=kCitrinetLmBeta*HW1_STT_LM_STRENGTH;
    p.hotwordBonus*=HW1_STT_LM_STRENGTH;
    return p;
}
#endif
// The microSD copy wins when present, so the LM can be swapped with the card
// (like the 15x5 model) and the onboard copy removed to free LittleFS. The
// hotword list beside it is the user vocabulary (System_STTVocabulary.h); the
// former fixed custom_words.txt is no longer read, and its presence is noted
// once so a card prepared for an older build is not silently ignored.
struct LmFiles { const char* lm; const char* legacyWords; };
constexpr LmFiles kLmFiles[]={
    {"/sd/STT Models/meeting.lm","/sd/STT Models/custom_words.txt"},
    {"/STT Models/meeting.lm","/STT Models/custom_words.txt"},
};
// Free PSRAM kept beside a resident LM: a warm worst-case (30 s) QuartzNet call
// needs ~5 MiB. A Citrinet session has ~7.6 MB here beside 22.7 MB of weights;
// its resident model then takes 1.5 MB and returns 1.3 MB of the weights image
// (citrinet_runtime.cpp, kResidentKeepBytes), so it keeps 3 MiB. The optional
// speaker labels and feature pool check their own room afterwards; the beam
// workspace is checked per decode (Citrinet: lent from the model's arena).
constexpr size_t kLmMaxBytes=32u<<20, kLmReserveBytes=(HW1_STT_CITRINET ? 3u : 6u)<<20;
// HW1_STT_CITRINET_LM=0 builds Citrinet without the language model: greedy
// word-piece decoding, meeting.lm is not read (2.5 MB of PSRAM less).
#ifndef HW1_STT_CITRINET_LM
#define HW1_STT_CITRINET_LM 1
#endif
constexpr bool kLmEnabled=!HW1_STT_CITRINET || HW1_STT_CITRINET_LM;
enum class LmState : uint8_t { Unloaded, Ready, Unavailable };
enum LmNote : uint32_t { kLmMissing=1, kLmInvalid=2, kLmMemory=4, kLmLoaded=8, kLmDecode=16, kLmShed=32, kLmWords=64, kLmLegacyWords=128, kLmOff=256 };
std::atomic<uint32_t> gLmNoted{0};
bool firstNote(LmNote note) { return !(gLmNoted.fetch_or(note)&note); }
struct LanguageModel {
    uint8_t* blob=nullptr;
    lm::CustomWords* words=nullptr;   // PSRAM, trivially destructible; null while the vocabulary is empty
    uint32_t wordsGeneration=0;      // the vocabulary generation `words` reflects
    lm::Lm view;
    const LmFiles* files=nullptr;      // where the loaded copy came from
    LmState state=LmState::Unloaded;
    uint32_t decodeCalls=0;          // metadata only; includes draft/final calls in this backend lifetime
    ~LanguageModel() { reset(); }
    void reset() { view=lm::Lm{}; heap_caps_free(words); words=nullptr; wordsGeneration=0; heap_caps_free(blob); blob=nullptr; }
};
bool stopRequested(const SttLocalControl& c) { return c.cancelled && c.cancelled(c.context); }
bool readAll(File& file,uint8_t* dest,size_t bytes,const SttLocalControl& control) {
    for(size_t done=0;done<bytes;) {
        if(stopRequested(control))return false;
        const size_t n=file.read(dest+done,std::min(bytes-done,size_t(64*1024)));
        if(!n || n>bytes-done)return false;
        done+=n;
    }
    return true;
}
// The decoder's hotwords are the user vocabulary's spoken forms. Taken as one
// complete snapshot (never a half-edited list) and parsed into the model's own
// table, so a decode in progress is never affected; re-taken between phrases
// when the vocabulary generation moved (decodeWithLanguageModel). An empty
// vocabulary leaves `words` null: the LM then decodes with no hotwords.
void loadCustomWords(LanguageModel& model) {
    char* text=static_cast<char*>(heap_caps_malloc(STT_VOCAB_HOTWORD_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!text) {
        if(firstNote(kLmWords))WARN_SYSTEMF("[STT] No memory for the vocabulary hotwords; decoding without them");
        return;
    }
    size_t bytes=0;
    const uint32_t generation=sttVocabularyHotwords(text,STT_VOCAB_HOTWORD_BYTES,&bytes);
    if(bytes) {
        if(!model.words) {
            void* storage=heap_caps_malloc(sizeof(lm::CustomWords),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(storage)model.words=new(storage) lm::CustomWords;
            else if(firstNote(kLmWords))WARN_SYSTEMF("[STT] No memory for the vocabulary hotwords; decoding without them");
        }
        if(model.words && !model.words->parse(text,bytes)) { heap_caps_free(model.words); model.words=nullptr; }
    } else { heap_caps_free(model.words); model.words=nullptr; }
    model.wordsGeneration=generation;
    heap_caps_free(text);
}
// One attempt per LanguageModel lifetime (state Ready or Unavailable after);
// only cancellation returns false and leaves it Unloaded for a later retry.
bool loadLanguageModel(LanguageModel& model,const SttLocalControl& control) {
    model.state=LmState::Unavailable;
    model.files=nullptr;
    if(!kLmEnabled) {
        if(firstNote(kLmOff))INFO_SYSTEMF("[STT] Citrinet build without the language model (HW1_STT_CITRINET_LM 0); using greedy decoding");
        return true;
    }
    // trusted: fixed LM path beside the pinned model; content is SHA-checked.
    // Each candidate LM lives on its own tier; confine the probe and the read
    // to that tier's model folder.
    auto authFor=[](const char* path){ return VFS::systemAuth(VFS::sttModelScopeFor(path),"stt.lm_read"); };
    for(const LmFiles& f:kLmFiles)
        if(VFS::existsGuarded(f.lm,authFor(f.lm))) { model.files=&f; break; }
    if(!model.files) {
        if(firstNote(kLmMissing))INFO_SYSTEMF("[STT] No %s or %s; using greedy decoding",kLmFiles[0].lm,kLmFiles[1].lm);
        return true;
    }
    const char* kLmPath=model.files->lm;
    File file=VFS::openGuarded(kLmPath,"r",authFor(kLmPath));
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
    // trusted: an existence probe of the retired fixed hotword file beside this LM.
    if(VFS::existsGuarded(model.files->legacyWords,authFor(model.files->legacyWords)) && firstNote(kLmLegacyWords))
        WARN_SYSTEMF("[STT] %s is no longer read; add its words with sttvocab (user_vocab.tsv)",model.files->legacyWords);
#if HW1_STT_CITRINET
    // The word-piece decoder's own weight and bonus (citrinetParams), not the file's QuartzNet values.
    const double alpha0=kCitrinetLmAlpha, beta0=kCitrinetLmBeta;
    const char* decoderName="wordpiece";
#else
    const double alpha0=double(model.view.alpha()), beta0=double(model.view.beta());
    const char* decoderName="char";
#endif
    if(firstNote(kLmLoaded))
        ESP_LOGI("STT","Language model %s (%s decoder): %u words, %u bigrams, %u trigrams, beam %u, %u custom words, %u bytes, "
                      "lm_strength=%.3f alpha=%.3f->%.3f beta=%.3f->%.3f hotword_bonus=%.3f->%.3f",
                     kLmPath,decoderName,unsigned(model.view.vocab()),unsigned(model.view.bigrams()),unsigned(model.view.trigrams()),
                     unsigned(model.view.beamWidth()),unsigned(model.words ? model.words->size() : 0),unsigned(bytes),
                     double(HW1_STT_LM_STRENGTH),alpha0,alpha0*HW1_STT_LM_STRENGTH,
                     beta0,beta0*HW1_STT_LM_STRENGTH,
                     double(model.view.hotwordBonus()),double(model.view.hotwordBonus())*HW1_STT_LM_STRENGTH);
    return true;
}
hw1::stt::DecodeResult decodeWithLanguageModel(void* context,const int8_t* logits,size_t frames,int exponent,
                                               char* text,size_t capacity,const SttLocalControl& control,
                                               const hw1::stt::DecodeScratch& scratch) {
    using hw1::stt::DecodeResult;
    auto& model=*static_cast<LanguageModel*>(context);
    const uint32_t call=++model.decodeCalls;
    lm::DecodeStats stats;
    auto note=[&](const char* status) {
        ESP_LOGI("STT","TL lm_decode call=%lu frames=%u strength=%.3f status=%s history_start=%d history_end=%d workspace=%u lent=%d",
                 (unsigned long)call,unsigned(frames),double(HW1_STT_LM_STRENGTH),status,
                 stats.startHistoryWords==UINT8_MAX ? -1 : int(stats.startHistoryWords),
                 stats.endHistoryWords==UINT8_MAX ? -1 : int(stats.endHistoryWords),
                 unsigned(stats.workspaceBytes),stats.workspaceLent ? 1 : 0);
    };
    if(model.state==LmState::Unloaded && !loadLanguageModel(model,control)) {
        note("load_cancelled"); return DecodeResult::Cancelled;
    }
    if(model.state!=LmState::Ready) { note("unavailable"); return DecodeResult::Fallback; }
    // Between phrases only: an edit never changes a decode already running.
    if(model.wordsGeneration!=sttVocabularyGeneration())loadCustomWords(model);
#if HW1_STT_CITRINET
    const lm::wordpiece::Pieces* pieces=hw1::stt::citrinetPieces();   // set by the runtime before it decodes
    if(!pieces) { note("unavailable"); return DecodeResult::Fallback; }
    const lm::DecodeStatus status=lm::wordpiece::decode(logits,frames,size_t(hw1::stt::identity::kOutputClasses),exponent,
                                                        model.view,*pieces,citrinetParams(model.view),model.words,text,
                                                        capacity,{control.context,control.cancelled},&stats,
                                                        scratch.data,scratch.bytes);
#else
    (void)scratch;   // the character search keeps its own workspace
    const lm::DecodeStatus status=lm::decode(logits,frames,exponent,model.view,model.words,text,capacity,
                                             {control.context,control.cancelled},0,&stats,HW1_STT_LM_STRENGTH);
#endif
    switch(status) {
        case lm::DecodeStatus::Ok: note("ok"); break;
        case lm::DecodeStatus::InvalidArgument: note("invalid_argument"); break;
        case lm::DecodeStatus::OutOfMemory: note("out_of_memory"); break;
        case lm::DecodeStatus::Cancelled: note("cancelled"); break;
        case lm::DecodeStatus::OutputTooSmall: note("output_too_small"); break;
    }
    if(status==lm::DecodeStatus::Ok)return DecodeResult::Decoded;
    if(status==lm::DecodeStatus::Cancelled)return DecodeResult::Cancelled;
    // Workspace allocation failure or an over-long result: greedy decides.
#if HW1_STT_CITRINET
    const size_t workspace=stats.workspaceBytes ? stats.workspaceBytes
        : lm::wordpiece::workspace_bytes(frames,model.view.beamWidth(),size_t(hw1::stt::identity::kOutputClasses));
#else
    const size_t workspace=lm::workspace_bytes(frames,model.view.beamWidth());
#endif
    if(firstNote(kLmDecode))WARN_SYSTEMF("[STT] Language-model decode failed (%d, %u frames, %u workspace bytes); using greedy decoding",
                                          int(status),unsigned(frames),unsigned(workspace));
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
#if HW1_STT_SPEAKER_LABELS
// The session's speaker model (all PSRAM: the file, ESP-DL's arena and
// metadata, the workspace, centroids).
struct SpeakerJob { uint32_t seq=0, submittedMs=0, samples=0, phraseSamples=0; };
struct SpeakerLabelMsg { uint32_t seq; int8_t speaker; };
constexpr size_t kSpeakerJobs=4;
struct SpeakerState {
    uint8_t* blob=nullptr;
    hw1::stt::speaker::Model model;
    hw1::stt::speaker::Clusters clusters;
    float* centroids=nullptr;
    float* embedding=nullptr;
    // background labelling
    int16_t* jobPcm=nullptr;            // kSpeakerJobs * jobSpan samples (PSRAM)
    size_t jobSpan=0;
    SpeakerJob jobs[kSpeakerJobs];
    QueueHandle_t ready=nullptr, free=nullptr;
    QueueHandle_t labels=nullptr;       // SpeakerLabelMsg, read by the STT worker (transcript lines)
    uint8_t* diagBuf=nullptr;           // diagnostics: int8 voiceprint + base64 (PSRAM)
    SemaphoreHandle_t stopped=nullptr;
    TaskHandle_t task=nullptr;
    uint32_t submitted=0;
    std::atomic<uint32_t> skipped{0}, labelled{0};
    std::atomic<bool> stopping{false};
    void stopTask();
    ~SpeakerState();
};
SpeakerState* gSpeakerActive=nullptr;   // for sttSpeakerProfileLog (debug only; the worker owns it)
void SpeakerState::stopTask() {
    if(task) {
        // The model has no mid-inference cancellation. Stop admission, wake an
        // idle worker and join its current embed before freeing any state.
        // A timeout may report a slow join; it must never release live queues
        // or the ESP-DL arena underneath the worker.
        stopping.store(true,std::memory_order_release);
        const uint8_t stop=0xFF;
        (void)xQueueSend(ready,&stop,0); // one reserved slot beyond the four jobs
        const int64_t start=esp_timer_get_time();
        bool noted=false;
        while(xSemaphoreTake(stopped,pdMS_TO_TICKS(20))!=pdTRUE) {
            if(!noted && esp_timer_get_time()-start>=8000000) {
                ESP_LOGW("STT","TL spk_stop waiting_for_worker=1; keeping model and queues alive");
                noted=true;
            }
        }
        vTaskDeleteWithCaps(task);
        task=nullptr;
    }
    if(ready) { vQueueDeleteWithCaps(ready); ready=nullptr; }
    if(labels) { vQueueDeleteWithCaps(labels); labels=nullptr; }
    if(free) { vQueueDeleteWithCaps(free); free=nullptr; }
    if(stopped) { vSemaphoreDeleteWithCaps(stopped); stopped=nullptr; }
}
SpeakerState::~SpeakerState() {
    stopTask();
    if(gSpeakerActive==this)gSpeakerActive=nullptr;
    model.reset(); sttPsramFree(centroids); sttPsramFree(embedding); sttPsramFree(jobPcm); sttPsramFree(diagBuf);
    heap_caps_free(blob);
}
#endif
// Session-owned backend state: verified weights plus the optional LM.
struct Backend {
    hw1::stt::ModelCache weights;
#if ENABLE_STT_LM
    LanguageModel lm;
#endif
#if HW1_STT_SPEAKER_LABELS
    SpeakerState* speaker=nullptr;
    bool speakerTried=false;
    // The speaker model and the voices it has heard belong to one run: freed when the run's labelling ends, so
    // a parked session (sttKeepWarm) holds only the weights and the LM, and the next run starts at Person 1.
    void releaseSpeaker() {
        if(speaker) { speaker->~SpeakerState(); sttPsramFree(speaker); speaker=nullptr; }
        speakerTried=false;
    }
    ~Backend() { releaseSpeaker(); }
#endif
};
}
bool sttLocalAvailable(char* error,size_t cap,const char** reason) {
    File file=openModel();
    const char* problem=nullptr;
    if(strncmp(kModelPath,"/sd/",4)==0 && !VFS::isSdAvailable())
        problem="Speech model SD card unavailable. Insert the model card and mount SD.";
    else if(!file || file.isDirectory())
        problem="Speech model cannot be opened. Install the matching model at " HW1_STT_MODEL_PATH ".";
    else if(file.size()!=96+hw1::stt::identity::kCompressedBytes)
        problem="Speech model has the wrong size. Reinstall the matching model at " HW1_STT_MODEL_PATH ".";
    if(reason)*reason=problem;
    if(error&&cap)snprintf(error,cap,"%s",problem ? problem : "");
    return !problem;
}
bool sttLocalTranscribe(const int16_t* pcm,size_t samples,char* text,size_t textCap,
                       const SttLocalControl& control,SttLocalStats& stats,
                       char* error,size_t errorCap,SttLocalSession* session,
                       const SttFeaturePrefix* prefix) {
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
    if(!backend || !backend->weights.loaded())
        ESP_LOGI("STT","Model %s: file=%u expected=%u raw=%u nodes=%u output_exp=%d",
                 kModelPath,unsigned(file.size()),unsigned(96+hw1::stt::identity::kCompressedBytes),
                 unsigned(hw1::stt::identity::kRawBytes),unsigned(hw1::stt::identity::kNodeCount),
                 hw1::stt::identity::kOutputExponent);
#if HW1_BENCH_MEMDIAG
    if(!backend || !backend->weights.loaded()) memDiagCheckpoint("stt-model-open");
#endif
#if ENABLE_STT_LM
    // One-shot calls load (and free) the LM for this call only, like weights.
    LanguageModel oneShot;
    LanguageModel& model=backend ? backend->lm : oneShot;
    const hw1::stt::Decoder decoder{&model,decodeWithLanguageModel,releaseLanguageModel};
    const hw1::stt::Decoder* decoding=&decoder;
#else
    const hw1::stt::Decoder* decoding=nullptr;
#endif
    hw1::stt::ModelCache* const weights=backend ? &backend->weights : nullptr;
#if HW1_STT_CITRINET
    // Once the weights are trimmed to the 10 s resident model (after the boot's first session), longer audio
    // (`stt record` takes up to 20 s) is transcribed in pieces cut at quiet points and joined with a space.
    const size_t piece=hw1::stt::singleCallSamples();
    if(piece && pcm && samples>piece) {
        size_t cuts[4];
        const size_t count=hw1::stt::splitSpeech(pcm,samples,piece,hw1::stt::identity::kSampleRate,cuts,4);
        auto refuse=[&](const char* why) {
            if(text&&textCap)text[0]=0;
            if(error&&errorCap)snprintf(error,errorCap,"%s",why);
            return false;
        };
        if(!count)return refuse("STT audio too long to transcribe in pieces");
        size_t used=0, from=0;
        if(text&&textCap)text[0]=0;
        for(size_t k=0;k<=count;++k) {
            const size_t to=k<count ? cuts[k] : samples, gap=used ? 1 : 0;
            if(!text || textCap<used+gap+2)return refuse("STT transcript exceeds result capacity");
            File part=k ? openModel() : file;
            if(!part || part.isDirectory())return refuse("Cannot open local STT model");
            SttLocalStats s;
            if(!hw1::stt::transcribe({&part,part.size(),readModel},pcm+from,to-from,text+used+gap,textCap-used-gap,control,s,
                                     error,errorCap,nullptr,weights,decoding,nullptr)) {
                text[0]=0;
                return false;
            }
            const size_t len=strlen(text+used+gap);
            if(len) { if(gap)text[used]=' '; used+=gap+len; }
            if(!k) { stats=s; stats.samples=uint32_t(samples); }
            else {
                stats.featureFrames+=s.featureFrames; stats.outputFrames+=s.outputFrames;
                stats.loadMs+=s.loadMs; stats.frontendMs+=s.frontendMs; stats.inferenceMs+=s.inferenceMs; stats.decodeMs+=s.decodeMs;
                stats.lmUsed=stats.lmUsed && s.lmUsed;
            }
            from=to;
        }
        return true;
    }
#endif
    return hw1::stt::transcribe({&file,file.size(),readModel},pcm,samples,text,textCap,control,stats,error,errorCap,
                              nullptr,weights,decoding,prefix);
}
#if HW1_STT_SPEAKER_LABELS
namespace {
bool readFully(File& file,uint8_t* dest,size_t bytes) {
    for(size_t done=0;done<bytes;) {
        const size_t n=file.read(dest+done,std::min(bytes-done,size_t(64*1024)));
        if(!n || n>bytes-done)return false;
        done+=n;
    }
    return true;
}
constexpr const char* kSpeakerPath="/sd/STT Models/speaker.spk";
constexpr const char* kSpeakerFixturePath="/sd/STT Models/speaker_fixture.bin";
// kSpeakerKeepFree: PSRAM the speaker model must leave for the session's phrases. A QuartzNet session keeps
// 3 MiB. A Citrinet session with the LM has 5.3-5.6 MB here (bench, 16 Nov 2026) and the speaker model takes
// 3.33 MB of it; what a phrase then needs at once is its features and call margin (0.6 MB: the LM search works
// in the acoustic model's arena), so 1.5 MiB. The feature pool keeps its own 2 MiB and stays out when short.
constexpr size_t kSpeakerMaxBytes=16u<<20, kSpeakerArenaEstimate=1u<<20, kSpeakerKeepFree=(HW1_STT_CITRINET ? 3u<<19 : 3u<<20);
// Diagnostics: the fixture (PCM + the Mac's embedding) kept in PSRAM, re-embedded before every background
// job: a cosine below ~0.99 during a session means the trunk's result was corrupted (e.g. PIE state across a
// context switch), not the audio.
uint8_t* gSpeakerFixture=nullptr; [[maybe_unused]] size_t gSpeakerFixtureBytes=0;
float speakerFixtureAgain(SpeakerState& s) {
    if(!gSpeakerFixture)return -2.0f;
    uint32_t samples=0,dim=0; memcpy(&samples,gSpeakerFixture,4); memcpy(&dim,gSpeakerFixture+4,4);
    if(!s.model.embed(reinterpret_cast<const int16_t*>(gSpeakerFixture+8),samples,s.embedding))return -3.0f;
    const float* want=reinterpret_cast<const float*>(gSpeakerFixture+8+size_t(samples)*2);
    double dot=0,a=0,b=0;
    for(size_t i=0;i<dim;++i) { dot+=double(s.embedding[i])*want[i]; a+=double(s.embedding[i])*s.embedding[i]; b+=double(want[i])*want[i]; }
    return a>0 && b>0 ? float(dot/std::sqrt(a*b)) : -3.0f;
}
#if HW1_STT_SPEAKER_DIAGNOSTICS
// Optional parity check: a phrase and the embedding the Mac computed for it.
float speakerFixtureCosine(SpeakerState& s) {
    const auto auth=VFS::systemAuth(VFS::sttModelScopeFor(kSpeakerFixturePath),"stt.speaker_read");
    if(!VFS::existsGuarded(kSpeakerFixturePath,auth))return -2.0f;
    File f=VFS::openGuarded(kSpeakerFixturePath,"r",auth);
    const size_t bytes=f && !f.isDirectory() ? f.size() : 0;
    if(bytes<8 || bytes>(1u<<20))return -3.0f;
    auto* buf=static_cast<uint8_t*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    float cos=-3.0f;
    if(buf && readFully(f,buf,bytes)) {
        uint32_t samples=0,dim=0;
        memcpy(&samples,buf,4); memcpy(&dim,buf+4,4);
        if(dim==s.model.info().dim && 8+size_t(samples)*2+size_t(dim)*4==bytes &&
           s.model.embed(reinterpret_cast<const int16_t*>(buf+8),samples,s.embedding)) {
            const float* want=reinterpret_cast<const float*>(buf+8+size_t(samples)*2);
            double dot=0,a=0,b=0;
            for(size_t i=0;i<dim;++i) { dot+=double(s.embedding[i])*want[i]; a+=double(s.embedding[i])*s.embedding[i]; b+=double(want[i])*want[i]; }
            cos=a>0 && b>0 ? float(dot/std::sqrt(a*b)) : -3.0f;
        }
    }
    if(cos>-2.0f && !gSpeakerFixture) { gSpeakerFixture=buf; gSpeakerFixtureBytes=bytes; buf=nullptr; }
    heap_caps_free(buf);
    return cos;
}
#endif
}  // namespace
size_t speakerBase64(const uint8_t* in,size_t n,char* out) {
    static const char k[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o=0;
    for(size_t i=0;i<n;i+=3) {
        const uint32_t v=uint32_t(in[i])<<16 | (i+1<n ? uint32_t(in[i+1])<<8 : 0) | (i+2<n ? in[i+2] : 0);
        out[o++]=k[v>>18&63]; out[o++]=k[v>>12&63]; out[o++]=i+1<n ? k[v>>6&63] : '='; out[o++]=i+2<n ? k[v&63] : '=';
    }
    out[o]=0;
    return o;
}
// Terminal results never block the worker behind their consumer. The queue
// holds twelve results for four PCM jobs; overflow is an explicit diagnostic.
void speakerPostLabel(SpeakerState& s,uint32_t seq,int8_t speaker) {
    const SpeakerLabelMsg m{seq,speaker};
    if(!s.labels || xQueueSend(s.labels,&m,0)!=pdTRUE)
        ESP_LOGW("STT","TL spk_result_lost seq=%lu spk=%d",(unsigned long)seq,int(speaker));
}
// One label: embed + cluster + the TL lines (spk: label; spkt: sections; spke: the unit embedding as int8 x127,
// base64, for offline threshold tuning on device voiceprints).
void speakerLabelAndLog(SpeakerState& s,const SpeakerJob& job,const int16_t* pcm) {
    const int64_t t0=esp_timer_get_time();
    hw1::stt::speaker::EmbedTiming tm;
    if(!s.model.embed(pcm,job.samples,s.embedding,&tm)) {
        ++s.skipped;
        speakerPostLabel(s,job.seq,-1);
        ESP_LOGW("STT","TL spk seq=%lu spk=-1 fail=1",(unsigned long)job.seq);
        return;
    }
    const int64_t c0=esp_timer_get_time();
    const auto a=s.clusters.assign(s.embedding,job.phraseSamples);
    const uint32_t cluUs=uint32_t(esp_timer_get_time()-c0), totalUs=uint32_t(esp_timer_get_time()-t0);
    const uint32_t lagMs=uint32_t(esp_timer_get_time()/1000)-job.submittedMs;
    ++s.labelled;
    ESP_LOGI("STT","TL spk seq=%lu spk=%d sim=%.3f new=%d tent=%d samples=%u fe_us=%lu nn_us=%lu head_us=%lu us=%lu lag_ms=%lu",
             (unsigned long)job.seq,int(a.speaker),double(a.similarity),a.created ? 1 : 0,a.tentative ? 1 : 0,
             unsigned(job.phraseSamples),(unsigned long)tm.featureUs,(unsigned long)tm.trunkUs,(unsigned long)tm.headUs,
             (unsigned long)totalUs,(unsigned long)lagMs);
    ESP_LOGI("STT","TL spkt seq=%lu frames=%u crop=%d fft=%lu mel=%lu norm=%lu q=%lu nn=%lu dq=%lu sap=%lu pool=%lu fc=%lu clu=%lu",
             (unsigned long)job.seq,unsigned(tm.frames),job.phraseSamples>job.samples ? 1 : 0,(unsigned long)tm.fftUs,
             (unsigned long)tm.melUs,(unsigned long)tm.normUs,(unsigned long)tm.quantUs,(unsigned long)tm.trunkUs,
             (unsigned long)tm.dequantUs,(unsigned long)tm.sapUs,(unsigned long)tm.poolUs,(unsigned long)tm.fcUs,(unsigned long)cluUs);
    ESP_LOGI("STT","TL spk_match seq=%lu profiles_before=%u profiles_after=%u best=%d best_sim=%.3f "
                  "second=%d second_sim=%.3f matches=%u updated=%d reason=%s phrase_samples=%u",
             (unsigned long)job.seq,unsigned(a.profilesBefore),unsigned(a.profilesAfter),int(a.bestSpeaker),
             double(a.bestSimilarity),int(a.secondSpeaker),double(a.secondSimilarity),unsigned(a.matches),
             a.centroidUpdated ? 1 : 0,hw1::stt::speaker::assignmentReasonName(a.reason),unsigned(job.phraseSamples));
    speakerPostLabel(s,job.seq,a.speaker);
#if HW1_STT_SPEAKER_DIAGNOSTICS
    const size_t dim=s.model.info().dim;
    if(dim<=1024 && s.diagBuf) {
        uint8_t* q=s.diagBuf;
        char* b64=reinterpret_cast<char*>(s.diagBuf+1024);
        double n2=0; for(size_t i=0;i<dim;++i)n2+=double(s.embedding[i])*s.embedding[i];
        const float inv=n2>0 ? float(127.0/std::sqrt(n2)) : 0.0f;
        for(size_t i=0;i<dim;++i) { const float v=std::nearbyint(s.embedding[i]*inv); q[i]=uint8_t(int8_t(v>127.f?127.f:(v<-127.f?-127.f:v))); }
        speakerBase64(q,dim,b64);
        ESP_LOGI("STT","TL spke seq=%lu dim=%u e=%s",(unsigned long)job.seq,unsigned(dim),b64);
    }
    {
        size_t module=0;
        const size_t dip=s.model.worstRamDip(&module);
        ESP_LOGI("STT","TL spkram seq=%lu worst_dip=%u module=%u int_free=%u int_min=%u",(unsigned long)job.seq,
                 unsigned(dip),unsigned(module),unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    }
#endif
}
void speakerTask(void* arg) {
    auto* s=static_cast<SpeakerState*>(arg);
    for(;;) {
        uint8_t i=0xFF;
        if(xQueueReceive(s->ready,&i,portMAX_DELAY)!=pdTRUE)continue;
        if(i>=kSpeakerJobs)break;   // stop
        if(s->stopping.load(std::memory_order_acquire)) {
            ++s->skipped;
            speakerPostLabel(*s,s->jobs[i].seq,-1);
            xQueueSend(s->free,&i,0);
            break;
        }
        if(HW1_STT_SPEAKER_DIAGNOSTICS && s->labelled%16==0) {   // diagnostics: the fixture again every 16th job
            const int64_t f0=esp_timer_get_time();
            const float fc=speakerFixtureAgain(*s);
            ESP_LOGI("STT","TL spkfix seq=%lu cos=%.4f us=%lu",(unsigned long)s->jobs[i].seq,double(fc),
                     (unsigned long)(esp_timer_get_time()-f0));
            vTaskDelay(1);
        }
        speakerLabelAndLog(*s,s->jobs[i],s->jobPcm+size_t(i)*s->jobSpan);
        vTaskDelay(1);   // let IDLE1 run between jobs
        xQueueSend(s->free,&i,0);
    }
    // Shutdown discards pending work only after any active embed returned.
    uint8_t pending=0xFF;
    while(xQueueReceive(s->ready,&pending,0)==pdTRUE) {
        if(pending<kSpeakerJobs) {
            ++s->skipped;
            speakerPostLabel(*s,s->jobs[pending].seq,-1);
            xQueueSend(s->free,&pending,0);
        }
    }
    xSemaphoreGive(s->stopped);
    vTaskSuspend(nullptr);   // deleted by stopTask() (vTaskDeleteWithCaps)
}
bool sttSpeakerBegin(SttLocalSession* session,size_t maxSamples,SttSpeakerStatus* status) {
    SttSpeakerStatus local;
    SttSpeakerStatus& st=status ? *status : local;
    st=SttSpeakerStatus{};
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    auto why=[&](const char* w) { snprintf(st.why,sizeof(st.why),"%s",w); return false; };
    if(!b)return why("no session");
    if(b->speaker) { st.ready=b->speaker->model.ready(); return st.ready; }
    if(b->speakerTried)return why("already tried");
    b->speakerTried=true;
    // trusted: fixed optional model path beside the STT model; content is SHA-checked.
    const auto auth=VFS::systemAuth(VFS::sttModelScopeFor(kSpeakerPath),"stt.speaker_read");
    if(!VFS::existsGuarded(kSpeakerPath,auth))return why("no speaker.spk");
    File file=VFS::openGuarded(kSpeakerPath,"r",auth);
    const size_t bytes=file && !file.isDirectory() ? file.size() : 0;
    st.bytes=bytes;
    if(bytes<192 || bytes>kSpeakerMaxBytes)return why("wrong size");
    const int64_t t0=esp_timer_get_time();
    const size_t psram0=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t int0=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(psram0<bytes+kSpeakerArenaEstimate+kSpeakerKeepFree || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)<bytes)
        return why("PSRAM short");
    void* mem=sttPsramAlloc(sizeof(SpeakerState),"stt.speaker");
    if(!mem)return why("no memory");
    auto* s=new(mem) SpeakerState;
    s->blob=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    hw1::stt::speaker::LoadStatus ls=hw1::stt::speaker::LoadStatus::NoMemory;
#if HW1_STT_SPEAKER_DIAGNOSTICS
    // Internal-RAM low point of each load step (file read through VFS/SD, ESP-DL build + first run).
    const size_t intBefore=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_start();
#endif
    const int64_t r0=esp_timer_get_time();
    const bool readOk=s->blob && readFully(file,s->blob,bytes);
    st.readMs=uint32_t((esp_timer_get_time()-r0)/1000);
#if HW1_STT_SPEAKER_DIAGNOSTICS
    const size_t lowRead=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_monitor_local_minimum_free_size_start();
#endif
#if HW1_STT_SPEAKER_DIAGNOSTICS
    hw1::stt::speaker::Model::setLoadSelfTest(true);
    hw1_dl_log_kernel_binds=1;
#endif
    if(readOk)
        ls=s->model.load(s->blob,bytes,maxSamples,sttPsramAlloc,sttPsramFree);
#if HW1_STT_SPEAKER_DIAGNOSTICS
    const size_t lowBuild=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    ESP_LOGI("STT","TL spkram_load before=%u read_low=%u (dip %d) build_low=%u (dip %d) after=%u",unsigned(intBefore),
             unsigned(lowRead),int(intBefore)-int(lowRead),unsigned(lowBuild),int(intBefore)-int(lowBuild),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
#endif
    if(ls==hw1::stt::speaker::LoadStatus::Ok) {
        const auto& info=s->model.info();
        s->centroids=static_cast<float*>(sttPsramAlloc(size_t(8)*info.dim*sizeof(float),"stt.speaker"));
        s->embedding=static_cast<float*>(sttPsramAlloc(size_t(info.dim)*sizeof(float),"stt.speaker"));
        hw1::stt::speaker::ClusterConfig cc;
        cc.threshold=info.threshold; cc.min_new_samples=size_t(info.minNewMs)*16; cc.max_speakers=8;
        if(!s->centroids || !s->embedding || !s->clusters.reset(s->centroids,info.dim,cc))
            ls=hw1::stt::speaker::LoadStatus::NoMemory;
    }
    if(ls!=hw1::stt::speaker::LoadStatus::Ok) {
        s->~SpeakerState(); sttPsramFree(s);
        return why(hw1::stt::speaker::describe(ls));
    }
    b->speaker=s;
    gSpeakerActive=s;
    {
        const auto& r=s->model.loadReport();
        st.shaUs=r.shaUs; st.buildUs=r.buildUs; st.testUs=r.testUs; st.testResult=r.testResult;
        st.arenaPsram=r.arenaPsram; st.arenaInternal=r.arenaInternal; st.workspaceBytes=r.workspaceBytes;
        st.modules=r.modules;
    }
#if HW1_STT_SPEAKER_DIAGNOSTICS
    const int64_t f0=esp_timer_get_time();
    st.fixtureCos=speakerFixtureCosine(*s);
    st.fixtureUs=uint32_t(esp_timer_get_time()-f0);
    s->model.resetProfile();   // the fixture embed is not a phrase
    s->model.setRamProbe(true);
    s->diagBuf=static_cast<uint8_t*>(sttPsramAlloc(1024+1400,"stt.speaker"));
#endif
    // Background labelling: job slots hold the centre window the frontend uses (it crops the same span).
    {
        const auto& f=s->model.info().features;
        s->jobSpan=size_t(f.window_frames-1)*f.hop;
        s->jobPcm=static_cast<int16_t*>(sttPsramAlloc(kSpeakerJobs*s->jobSpan*sizeof(int16_t),"stt.speaker"));
        s->ready=xQueueCreateWithCaps(kSpeakerJobs+1,1,MALLOC_CAP_SPIRAM);
        s->labels=xQueueCreateWithCaps(2*kSpeakerJobs+4,sizeof(SpeakerLabelMsg),MALLOC_CAP_SPIRAM);
        s->free=xQueueCreateWithCaps(kSpeakerJobs,1,MALLOC_CAP_SPIRAM);
        s->stopped=xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM);
        if(s->jobPcm && s->ready && s->free && s->stopped && s->labels) {
            for(uint8_t i=0;i<kSpeakerJobs;++i)xQueueSend(s->free,&i,0);
            s->model.setMultiCore(false);   // never contend for ESP-DL's dual-core workers with the STT model
#if HW1_STT_SPEAKER_DIAGNOSTICS
            ESP_LOGI("STT","TL spkfix seq=0 single_core_at_load cos=%.4f",double(speakerFixtureAgain(*s)));
#endif
            // Priority 1 on core 1: below the "stt" worker (2), so it runs only while that worker waits.
            if(xTaskCreatePinnedToCoreWithCaps(speakerTask,"stt_spk",8192,s,1,&s->task,1,MALLOC_CAP_SPIRAM)!=pdPASS)
                s->task=nullptr;
        }
        if(!s->task)snprintf(st.why,sizeof(st.why),"%s","no background task (inline)");
    }
    st.ready=true;
    st.threshold=s->model.info().threshold;
    st.loadMs=uint32_t((esp_timer_get_time()-t0)/1000);
    st.psramUsed=psram0-heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st.internalUsed=long(int0)-long(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return true;
}
bool sttSpeakerLabel(SttLocalSession* session,const int16_t* pcm,size_t samples,SttSpeakerLabel* out) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    if(!b || !b->speaker || !b->speaker->model.ready() || !out)return false;
    SpeakerState& s=*b->speaker;
    const int64_t t0=esp_timer_get_time();
    hw1::stt::speaker::EmbedTiming tm;
    if(!s.model.embed(pcm,samples,s.embedding,&tm))return false;
    const int64_t c0=esp_timer_get_time();
    const auto a=s.clusters.assign(s.embedding,samples);
    out->clusterUs=uint32_t(esp_timer_get_time()-c0);
    out->speaker=a.speaker; out->similarity=a.similarity; out->created=a.created; out->tentative=a.tentative;
    out->featureUs=tm.featureUs; out->trunkUs=tm.trunkUs; out->headUs=tm.headUs;
    out->fftUs=tm.fftUs; out->melUs=tm.melUs; out->normUs=tm.normUs; out->quantUs=tm.quantUs;
    out->dequantUs=tm.dequantUs; out->sapUs=tm.sapUs; out->poolUs=tm.poolUs; out->fcUs=tm.fcUs;
    out->frames=tm.frames; out->cropped=tm.cropped;
    out->totalUs=uint32_t(esp_timer_get_time()-t0);
    return a.speaker>=0;
}
bool sttSpeakerSubmit(SttLocalSession* session,uint32_t seq,const int16_t* pcm,size_t samples,
                      const SttLocalControl& control,bool (*canWait)(void*)) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    if(!b || !b->speaker || !b->speaker->task)return false;
    SpeakerState& s=*b->speaker;
    ++s.submitted;
    const int64_t start=esp_timer_get_time(), deadline=start+30000000;
    auto cancelled=[&] { return control.cancelled && control.cancelled(control.context); };
    auto unknown=[&](const char* reason) {
        ++s.skipped;
        speakerPostLabel(s,seq,-1);
        ESP_LOGW("STT","TL spk_submit seq=%lu admitted=0 spk=-1 wait_ms=%lu reason=%s",
                 (unsigned long)seq,(unsigned long)((esp_timer_get_time()-start)/1000),reason);
        return true; // Background handled it; never run the same model inline.
    };
    if(!pcm || !samples)return unknown("invalid_pcm");
    uint8_t i=0xFF;
    for(;;) {
        if(cancelled())return unknown("cancelled");
        if(s.stopping.load(std::memory_order_acquire))return unknown("stopping");
        const int64_t left=deadline-esp_timer_get_time();
        if(left<=0)return unknown("timeout");
        // An available speaker slot needs only a bounded copy. If it would wait,
        // let the capture owner preserve room for arriving audio instead.
        if(xQueueReceive(s.free,&i,0)==pdTRUE)break;
        if(canWait && !canWait(control.context))return unknown("audio_pressure");
        const uint32_t waitMs=uint32_t(std::min<int64_t>(20,(left+999)/1000));
        if(xQueueReceive(s.free,&i,std::max<TickType_t>(1,pdMS_TO_TICKS(waitMs)))==pdTRUE)break;
    }
    // A cancellation racing with admission must return the slot unmodified.
    const char* rejected=cancelled() ? "cancelled" :
        s.stopping.load(std::memory_order_acquire) ? "stopping" :
        esp_timer_get_time()>=deadline ? "timeout" : nullptr;
    if(rejected) { xQueueSend(s.free,&i,0); return unknown(rejected); }
    size_t n=samples; const int16_t* src=pcm;
    if(n>s.jobSpan) { src+=(n-s.jobSpan)/2; n=s.jobSpan; }
    memcpy(s.jobPcm+size_t(i)*s.jobSpan,src,n*sizeof(int16_t));
    s.jobs[i].seq=seq; s.jobs[i].samples=uint32_t(n); s.jobs[i].phraseSamples=uint32_t(samples);
    s.jobs[i].submittedMs=uint32_t(esp_timer_get_time()/1000);
    if(xQueueSend(s.ready,&i,0)!=pdTRUE) {
        xQueueSend(s.free,&i,0);
        return unknown("queue_failed");
    }
    ESP_LOGI("STT","TL spk_submit seq=%lu admitted=1 wait_ms=%lu backlog=%u",
             (unsigned long)seq,(unsigned long)((esp_timer_get_time()-start)/1000),
             unsigned(kSpeakerJobs-uxQueueMessagesWaiting(s.free)));
    return true;
}
bool sttSpeakerBackground(SttLocalSession* session) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    return b && b->speaker && b->speaker->task && b->speaker->labels;
}
bool sttSpeakerTakeLabel(SttLocalSession* session,uint32_t* seq,int8_t* speaker) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    if(!b || !b->speaker || !b->speaker->labels || !seq || !speaker)return false;
    SpeakerLabelMsg m;
    if(xQueueReceive(b->speaker->labels,&m,0)!=pdTRUE)return false;
    *seq=m.seq; *speaker=m.speaker;
    return true;
}
void sttSpeakerDrain(SttLocalSession* session,uint32_t timeoutMs,const SttLocalControl& control) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    if(!b || !b->speaker || !b->speaker->task)return;
    const int64_t start=esp_timer_get_time(), end=start+int64_t(timeoutMs)*1000;
    const char* outcome="complete";
    while(uxQueueMessagesWaiting(b->speaker->free)<kSpeakerJobs) {
        if(control.cancelled && control.cancelled(control.context)) { outcome="cancelled"; break; }
        const int64_t left=end-esp_timer_get_time();
        if(left<=0) { outcome="timeout"; break; }
        const uint32_t waitMs=uint32_t(std::min<int64_t>(20,(left+999)/1000));
        vTaskDelay(std::max<TickType_t>(1,pdMS_TO_TICKS(waitMs)));
    }
    ESP_LOGI("STT","TL spk_drain wait_ms=%lu outcome=%s pending=%u",
             (unsigned long)((esp_timer_get_time()-start)/1000),outcome,
             unsigned(kSpeakerJobs-uxQueueMessagesWaiting(b->speaker->free)));
}
void sttSpeakerEnd(SttLocalSession* session) {
    Backend* b=session ? static_cast<Backend*>(session->backendState_) : nullptr;
    if(!b)return;
    if(b->speaker) {
        b->speaker->stopTask(); // join before sampling final completion/discard counters
        ESP_LOGI("STT","TL spk_end submitted=%lu labelled=%lu skipped=%lu",(unsigned long)b->speaker->submitted,
                 (unsigned long)b->speaker->labelled,(unsigned long)b->speaker->skipped);
    }
    b->releaseSpeaker();   // destructor's stopTask is idempotent; model remains alive through the summary
}
void sttSpeakerProfileLog() {
    SpeakerState* s=gSpeakerActive;
    if(!s || !s->model.ready())return;
    const uint32_t n=s->model.embeds();
    uint64_t total=0;
    for(size_t i=0;i<s->model.stages();++i)total+=s->model.stage(i).us;
    ESP_LOGI("STT","TL spkstg_begin embeds=%lu modules=%u trunk_us_total=%llu trunk_us_mean=%llu",(unsigned long)n,
             unsigned(s->model.stages()),(unsigned long long)total,(unsigned long long)(n ? total/n : 0));
    for(size_t i=0;i<s->model.stages();++i) {
        const auto p=s->model.stage(i);
        ESP_LOGI("STT","TL spkstg i=%u name=%s n=%lu us_mean=%llu pct=%.1f",unsigned(i),p.name ? p.name : "?",
                 (unsigned long)p.calls,(unsigned long long)(p.calls ? p.us/p.calls : 0),
                 total ? 100.0*double(p.us)/double(total) : 0.0);
        if((i&15)==15)vTaskDelay(1);   // pace the console
    }
    ESP_LOGI("STT","TL spkstg_end");
    s->model.resetProfile();
}
#endif
SttLocalCallBudget sttLocalCallBudget(size_t samples,const SttLocalSession* session) {
    const Backend* backend=session ? static_cast<const Backend*>(session->backendState_) : nullptr;
    SttLocalCallBudget out;
    out.total=hw1::stt::callBudget(samples,backend ? &backend->weights : nullptr,&out.contiguous);
    return out;
}
void sttLocalReserveModel() {
#if HW1_STT_CITRINET
    // Only for a model this boot can load (the card is mounted by now, initFilesystem): without the card or the
    // matching file the block would sit unused all boot. A card inserted later gets the block from its first
    // call; a restart with the card in reserves it here again.
    const char* missing=nullptr;
    if(!sttLocalAvailable(nullptr,0,&missing)) {
        ESP_LOGW("STT","TL slot reserved=0 reason=model (%s)",missing ? missing : "unavailable");
        return;
    }
    (void)hw1::stt::reserveModel();
#endif
}
#else
void sttLocalReserveModel() {}
SttLocalCallBudget sttLocalCallBudget(size_t,const SttLocalSession*) { return {}; }
bool sttLocalAvailable(char* error,size_t cap,const char** reason) {
    const char* problem="Local transcription is unsupported on this target. Use a supported build.";
    if(reason)*reason=problem;
    if(error&&cap)snprintf(error,cap,"%s",problem);
    return false;
}
bool sttLocalTranscribe(const int16_t*,size_t,char* text,size_t cap,const SttLocalControl&,
                       SttLocalStats& stats,char* error,size_t errorCap,SttLocalSession*,const SttFeaturePrefix*) {
    if(text&&cap)text[0]=0;stats={};return sttLocalAvailable(error,errorCap);
}
#endif
SttLocalSession::~SttLocalSession() { reset(); }
void SttLocalSession::reset() {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    delete static_cast<Backend*>(backendState_);
#endif
    backendState_=nullptr;
}
bool SttLocalSession::warm() const {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    return backendState_ && static_cast<const Backend*>(backendState_)->weights.loaded();
#else
    return false;
#endif
}
void SttLocalSession::moveTo(SttLocalSession& to) {
    if(&to==this)return;
    to.reset();
#if defined(CONFIG_IDF_TARGET_ESP32P4) && ENABLE_STT_LM
    // A shed LM stays Unavailable only for the rest of its own session.
    auto* backend=static_cast<Backend*>(backendState_);
    if(backend && backend->lm.state==LmState::Unavailable && !backend->lm.blob)backend->lm.state=LmState::Unloaded;
#endif
    dropCallState();  // the resident model's arena still holds this run's last activations
#if HW1_STT_SPEAKER_LABELS
    if(backendState_)static_cast<Backend*>(backendState_)->releaseSpeaker();
#endif
    to.backendState_=backendState_;
    backendState_=nullptr;
}
void SttLocalSession::dropCallState() {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && HW1_STT_RESIDENT_MODEL
    if(backendState_)static_cast<Backend*>(backendState_)->weights.dropResident();
#endif
}
#endif
