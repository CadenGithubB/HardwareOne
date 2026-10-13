// The Python driver inserts the actual System_STTLocal.cpp (includes removed)
// at INSERT_BACKEND, compiled with ENABLE_LOCAL_STT/ENABLE_STT_LM on the P4
// branch, and links the real stt/stt_lm.cpp. VFS, heap, logging and the
// QuartzNet runtime are fakes; the runtime fake drives the decoder hook the
// way quartznet_runtime.cpp does (covered by quartznet_cache_harness.cpp).
#include "stt/quartznet_runtime.h"
#include "stt/stt_lm.h"
#include "stt/stt_model_selection.h"
#include "stt_lm_testlib.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
namespace fake {
struct State {
    std::map<std::string,std::vector<uint8_t>> files;
    std::map<std::string,int> opens, exists;
    std::map<void*,size_t> heap;
    std::vector<std::string> logs;
    std::vector<std::string> modelLogs;  // acoustic selection diagnostics, separate from LM log-once policy
    std::vector<std::string> lmDecodeLogs; // per-call metadata, separate from LM load/fallback log-once policy
    size_t psram=32u<<20, maxRead=4096, failAllocationAbove=~size_t(0);
    int reads=0, cancelAfterReads=-1;
    bool releaseBeforeDecode=false, released=false;
    std::vector<int8_t> logits;
    const void* lastCache=nullptr;
    int residentDrops=0;  // ModelCache::dropResident calls (HW1_STT_RESIDENT_MODEL)
} state;
size_t used() { size_t n=0; for(const auto& a:state.heap)n+=a.second; return n; }
bool cancelled(void*) { return state.cancelAfterReads>=0 && state.reads>=state.cancelAfterReads; }
void log(const char* level, const char* fmt, ...) {
    char line[512]; va_list args; va_start(args,fmt); std::vsnprintf(line,sizeof(line),fmt,args); va_end(args);
    auto& destination=std::strncmp(fmt,"Model ",6)==0 ? state.modelLogs :
        std::strncmp(fmt,"TL lm_decode ",13)==0 ? state.lmDecodeLogs : state.logs;
    destination.push_back(std::string(level)+line);
}
}
#define INFO_SYSTEMF(...) fake::log("I ",__VA_ARGS__)
#define WARN_SYSTEMF(...) fake::log("W ",__VA_ARGS__)
#define ESP_LOGI(tag, ...) fake::log("I [" tag "] ",__VA_ARGS__)
void* heap_caps_malloc(size_t n, unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) && n);
    if(n>fake::state.failAllocationAbove)return nullptr;
    void* p=std::malloc(n); fake::state.heap[p]=n; return p;
}
void* heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned caps) {
    assert(alignment==16);
    if(caps!=(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) || !n || n>fake::state.failAllocationAbove)return nullptr;
    void* p=nullptr; if(posix_memalign(&p,16,n))return nullptr;
    fake::state.heap[p]=n; return p;
}
void heap_caps_free(void* p) {
    if(!p)return;
    auto it=fake::state.heap.find(p); assert(it!=fake::state.heap.end());
    fake::state.heap.erase(it); std::free(p);
}
size_t heap_caps_get_free_size(unsigned caps) { assert(caps==MALLOC_CAP_SPIRAM); return fake::state.psram-std::min(fake::state.psram,fake::used()); }
size_t heap_caps_get_largest_free_block(unsigned caps) { return heap_caps_get_free_size(caps); }

struct AuthContext { std::string reason; };
class File {
 public:
    File()=default;
    explicit File(const std::vector<uint8_t>* data):data_(data) {}
    explicit operator bool() const { return data_!=nullptr; }
    bool isDirectory() const { return false; }
    size_t size() const { return data_ ? data_->size() : 0; }
    size_t read(uint8_t* dest, size_t n) {
        ++fake::state.reads;
        n=std::min({n,fake::state.maxRead,size()-offset_});
        std::memcpy(dest,data_->data()+offset_,n); offset_+=n; return n;
    }
 private:
    const std::vector<uint8_t>* data_=nullptr;
    size_t offset_=0;
};
namespace VFS {
AuthContext systemAuth(const char* reason) { return {reason}; }
bool existsGuarded(const char* path, const AuthContext& auth) {
    assert(!auth.reason.empty()); ++fake::state.exists[path];
    return fake::state.files.count(path)!=0;
}
File openGuarded(const char* path, const char* mode, const AuthContext& auth) {
    assert(!std::strcmp(mode,"r") && !auth.reason.empty()); ++fake::state.opens[path];
    auto it=fake::state.files.find(path);
    return it==fake::state.files.end() ? File() : File(&it->second);
}
}
namespace hw1::stt {
ModelCache::~ModelCache() { reset(); }
void ModelCache::reset() { raw_=nullptr; }
#if HW1_STT_RESIDENT_MODEL
void ModelCache::dropResident() { ++fake::state.residentDrops; }
#endif
size_t callBudget(size_t, const ModelCache*, size_t* contiguous) { if(contiguous)*contiguous=0; return 0; }
static int fakeWeights=0;  // stands in for the verified raw weights a call leaves cached
// Mirrors quartznet_runtime.cpp's hook contract around a fixed logit tensor.
bool transcribe(ModelReader reader, const int16_t*, size_t, char* text, size_t capacity,
                const STTLocalControl& control, STTLocalStats&, char* error, size_t errorCapacity,
                Diagnostics*, ModelCache* cache, const Decoder* decoder, const STTFeaturePrefix* prefix) {
    assert(!prefix);  // no capture-time rows in these calls
    assert(reader.read && reader.bytes==fake::state.files.at("/STT Models/quartznet5x5.p4.stt").size());
    fake::state.lastCache=cache;
    if(cache && !cache->loaded())cache->raw_=&fakeWeights;
    text[0]=0; error[0]=0;
    assert(bool(decoder)==bool(ENABLE_STT_LM) && (!decoder || (decoder->decode && decoder->release)));
    if(decoder && fake::state.releaseBeforeDecode)fake::state.released=decoder->release(decoder->context);
    const DecodeResult r=decoder ? decoder->decode(decoder->context,fake::state.logits.data(),fake::state.logits.size()/29,
                                                   identity::kOutputExponent,text,capacity,control)
                                 : DecodeResult::Fallback;
    if(r==DecodeResult::Cancelled) { text[0]=0; std::snprintf(error,errorCapacity,"Cancelled"); return false; }
    if(r==DecodeResult::Fallback)std::snprintf(text,capacity,"greedy");
    return true;
}
}

// INSERT_BACKEND

namespace {
using lmtest::Logits;
constexpr const char* kModel="/STT Models/quartznet5x5.p4.stt";
constexpr const char* kLm="/STT Models/meeting.lm";
constexpr const char* kWords="/STT Models/custom_words.txt";
std::vector<uint8_t> meetingLm() {
    lmtest::Spec s; s.alpha=1; s.beta=0; s.unk=-6; s.prune=-5; s.beam=8; s.bonus=20;
    s.uni("</s>",-1000).uni("<s>",-32768,-200).uni("<unk>",-6000,-1000).uni("cat",-1500,-300);
    return lmtest::build(s);
}
void reset() {
    assert(fake::state.heap.empty()); // every scenario ends without leaks
    fake::state=fake::State{};
#if ENABLE_STT_LM
    gLmNoted=0;
#endif
    fake::state.files[kModel]=std::vector<uint8_t>(96+hw1::stt::identity::kCompressedBytes,0);
    Logits ck; ck.frame({{'c',30},{'k',31}}); ck.pattern("_a_t__");   // greedy "kat", LM "cat"
    fake::state.logits=ck.q;
}
std::string run(STTLocalSession* session, bool* ok=nullptr) {
    char text[64]="unset", error[64]; STTLocalStats stats;
    const STTLocalControl control{nullptr,fake::cancelled,nullptr};
    const int16_t pcm[320]={};
    const bool result=sttLocalTranscribe(pcm,320,text,sizeof(text),control,stats,error,sizeof(error),session);
    if(ok)*ok=result; else assert(result);
    return result ? text : std::string("!")+error;
}
size_t logged(const char* needle) {
    size_t n=0; for(const auto& l:fake::state.logs)n+=l.find(needle)!=std::string::npos; return n;
}
#if ENABLE_STT_LM
void missing() {
    reset();
    { STTLocalSession session; assert(run(&session)=="greedy" && run(&session)=="greedy"); }
    { STTLocalSession session; assert(run(&session)=="greedy"); }
    assert(run(nullptr)=="greedy");
    // One existence probe per session/one-shot, never an open; logged once per boot.
    assert(fake::state.exists[kLm]==3 && fake::state.opens[kLm]==0 && fake::state.exists[kWords]==0);
    assert(fake::state.logs.size()==1 && logged("I [STT] No /sd/STT Models/meeting.lm or /STT Models/meeting.lm"));
    assert(fake::state.lmDecodeLogs.size()==4);
    for(const auto& log:fake::state.lmDecodeLogs)assert(log.find("status=unavailable history_start=-1 history_end=-1")!=std::string::npos);
}
void crossPhraseMetadata() {
    reset(); fake::state.files[kLm]=meetingLm();
    STTLocalSession session;
    assert(run(&session)=="cat" && run(&session)=="cat");
    assert(fake::state.lmDecodeLogs.size()==2);
    assert(fake::state.lmDecodeLogs[0].find("call=1 ")!=std::string::npos);
    assert(fake::state.lmDecodeLogs[1].find("call=2 ")!=std::string::npos);
    session.reset();
    assert(run(&session)=="cat");
    assert(fake::state.lmDecodeLogs.size()==3);
    assert(fake::state.lmDecodeLogs[2].find("call=1 ")!=std::string::npos);
    for(const auto& log:fake::state.lmDecodeLogs) {
        assert(log.find("status=ok history_start=0 history_end=1")!=std::string::npos);
        assert(log.find("cat")==std::string::npos && log.find("text=")==std::string::npos && log.find("logits=")==std::string::npos);
    }
}
void configuredStrengthReachesDecoder() {
    reset();
    lmtest::Spec spec; spec.alpha=1; spec.beta=0; spec.bonus=0; spec.unk=-5; spec.prune=-8; spec.beam=8;
    spec.uni("</s>",0).uni("<s>",0).uni("<unk>",-5000).uni("a",0).uni("b",-1000);
    fake::state.files[kLm]=lmtest::build(spec);
    Logits logits; logits.frame({{'a',30},{'b',38}},-128); fake::state.logits=logits.q;
    hw1::stt::lm::Lm view;
    assert(view.open(fake::state.files[kLm].data(),fake::state.files[kLm].size())==hw1::stt::lm::LoadStatus::Ok);
    char original[16], reduced[16], configured[16];
    assert(hw1::stt::lm::decode(logits.q.data(),1,-2,view,nullptr,original,sizeof(original))==hw1::stt::lm::DecodeStatus::Ok);
    assert(hw1::stt::lm::decode(logits.q.data(),1,-2,view,nullptr,reduced,sizeof(reduced),{},0,nullptr,0.75f)==hw1::stt::lm::DecodeStatus::Ok);
    assert(!std::strcmp(original,"a") && !std::strcmp(reduced,"b")); // fixture distinguishes the two policies
    assert(hw1::stt::lm::decode(logits.q.data(),1,-2,view,nullptr,configured,sizeof(configured),{},0,nullptr,HW1_STT_LM_STRENGTH)==hw1::stt::lm::DecodeStatus::Ok);
    STTLocalSession session;
    assert(run(&session)==configured); // actual backend must forward the configured scalar
    char strengthLog[48]; std::snprintf(strengthLog,sizeof(strengthLog),"strength=%.3f",double(HW1_STT_LM_STRENGTH));
    assert(fake::state.lmDecodeLogs.size()==1 && fake::state.lmDecodeLogs[0].find(strengthLog)!=std::string::npos);
}
void loadedOncePerSession() {
    reset(); fake::state.files[kLm]=meetingLm();
    const size_t bytes=fake::state.files[kLm].size();
    {
        STTLocalSession session;
        assert(run(&session)=="cat");
        assert(fake::used()==bytes); // blob resident between calls; no word list
        assert(run(&session)=="cat" && run(&session)=="cat");
        assert(fake::state.opens[kLm]==1 && fake::state.exists[kWords]==1);
        assert(fake::state.lastCache!=nullptr);
    }
    assert(fake::state.heap.empty()); // session destruction frees the LM
    assert(run(nullptr)=="cat" && fake::state.heap.empty() && fake::state.lastCache==nullptr);
    assert(fake::state.opens[kLm]==2 && logged("I [STT] Language model /STT Models/meeting.lm: 4 words, 0 bigrams, 0 trigrams, beam 8, 0 custom words")==1);
    char weighting[160];
    std::snprintf(weighting,sizeof(weighting),
                  "lm_strength=%.3f alpha=1.000->%.3f beta=0.000->0.000 hotword_bonus=20.000->%.3f",
                  double(HW1_STT_LM_STRENGTH),double(HW1_STT_LM_STRENGTH),20.0*HW1_STT_LM_STRENGTH);
    assert(logged(weighting)==1);
    assert(fake::state.logs.size()==1);
    assert(fake::state.modelLogs.size()==2); // cold session plus one-shot, never warm calls
    assert(fake::state.modelLogs[0].find("Model /STT Models/quartznet5x5.p4.stt: file=5487504 expected=5487504 raw=7171984 nodes=71 output_exp=-2")!=std::string::npos);
}
void customWords() {
    reset(); fake::state.files[kLm]=meetingLm();
    const std::string list="Kat\nnot a word\n";
    fake::state.files[kWords]=std::vector<uint8_t>(list.begin(),list.end());
    { STTLocalSession session; assert(run(&session)=="kat" && run(&session)=="kat"); assert(fake::state.opens[kWords]==1); }
    assert(logged("1 custom words")==1);
    // Oversized list: ignored with one warning; the LM itself still decodes.
    reset(); fake::state.files[kLm]=meetingLm();
    fake::state.files[kWords]=std::vector<uint8_t>(64*1024+1,'a');
    { STTLocalSession session; assert(run(&session)=="cat" && run(&session)=="cat"); }
    assert(logged("W [STT] Ignoring /STT Models/custom_words.txt")==1 && logged("0 custom words")==1);
    // Empty after normalisation: no list, no warning.
    reset(); fake::state.files[kLm]=meetingLm(); fake::state.files[kWords]={'\n','-','\n'};
    { STTLocalSession session; assert(run(&session)=="cat"); }
    assert(fake::state.logs.size()==1 && logged("0 custom words")==1);
}
void invalid() {
    for(int scenario=0;scenario<4;++scenario) {
        reset(); fake::state.files[kLm]=meetingLm();
        auto& blob=fake::state.files[kLm];
        if(scenario==0)blob[70]^=1;                         // SHA-256 mismatch
        if(scenario==1)blob.resize(blob.size()-1);          // truncated
        if(scenario==2)blob.resize(10);                     // implausible size
        if(scenario==3)blob.clear();
        { STTLocalSession session; assert(run(&session)=="greedy" && run(&session)=="greedy"); }
        { STTLocalSession session; assert(run(&session)=="greedy"); }
        assert(fake::state.opens[kLm]==2 && fake::state.logs.size()==1 && logged("W [STT]")==1);
        if(scenario==0)assert(logged("SHA-256 mismatch"));
        if(scenario==1)assert(logged("file size does not match header"));
        if(scenario>=2)assert(logged("wrong size"));
    }
}
void memory() {
    reset(); fake::state.files[kLm]=meetingLm();
    const size_t bytes=fake::state.files[kLm].size();
    fake::state.psram=bytes+(6u<<20)-1; // below LM + reserve: never allocated
    { STTLocalSession session; assert(run(&session)=="greedy" && run(&session)=="greedy"); }
    assert(fake::state.logs.size()==1 && logged("W [STT] Not enough PSRAM"));
    // Allocation failure despite the budget: same fallback, same single note.
    fake::state.psram=64u<<20; fake::state.failAllocationAbove=bytes-1;
    { STTLocalSession session; assert(run(&session)=="greedy"); }
    assert(fake::state.logs.size()==1);
    reset(); fake::state.files[kLm]=meetingLm();
    { STTLocalSession session; assert(run(&session)=="cat"); }
    // Decode failure other than cancellation (here: over kMaxFrames): greedy.
    fake::state.logits.assign((hw1::stt::lm::kMaxFrames+1)*29,0);
    { STTLocalSession session; assert(run(&session)=="greedy" && run(&session)=="greedy"); }
    assert(logged("W [STT] Language-model decode failed")==1);
}
void released() {
    reset(); fake::state.files[kLm]=meetingLm();
    {
        STTLocalSession session;
        fake::state.releaseBeforeDecode=true;
        assert(run(&session)=="cat" && !fake::state.released); // nothing resident yet
        assert(run(&session)=="greedy" && fake::state.released && fake::state.heap.empty());
        fake::state.releaseBeforeDecode=false;
        assert(run(&session)=="greedy" && fake::state.opens[kLm]==1); // no reload thrash
    }
    assert(logged("W [STT] Released language model")==1);
    { STTLocalSession session; assert(run(&session)=="cat" && fake::state.opens[kLm]==2); }
}
// The broker hands an idle session to its next run (sttKeepWarm): weights and
// a loaded LM move whole; an LM shed under memory pressure gets one load
// attempt again, as in a new session; the source is left empty.
void handOff() {
    reset(); fake::state.files[kLm]=meetingLm();
    const size_t bytes=fake::state.files[kLm].size();
    STTLocalSession first, next;
    assert(!first.warm() && !next.warm());
    assert(run(&first)=="cat" && first.warm() && fake::used()==bytes);
    const int drops=fake::state.residentDrops;
    first.moveTo(next);
    assert(!first.warm() && next.warm() && fake::used()==bytes);
    // A resident model (and its arena) never travels with the weights.
    assert(fake::state.residentDrops==drops+(HW1_STT_RESIDENT_MODEL ? 1 : 0));
    assert(run(&next)=="cat" && fake::state.opens[kLm]==1);   // LM and weights reused, not reloaded
    first.moveTo(first); next.moveTo(next);                    // self-moves change nothing
    assert(next.warm() && !first.warm());
    first.reset(); assert(fake::used()==bytes);                 // an empty session owns nothing
    // Shed for this session, then handed on: the next owner loads it again.
    fake::state.releaseBeforeDecode=true;
    assert(run(&next)=="greedy" && fake::state.released && fake::state.heap.empty());
    fake::state.releaseBeforeDecode=false;
    assert(run(&next)=="greedy" && fake::state.opens[kLm]==1);
    next.moveTo(first);
    assert(first.warm() && !next.warm());
    assert(run(&first)=="cat" && fake::state.opens[kLm]==2);
    // Moving over a session that holds something frees what it held.
    STTLocalSession other;
    assert(run(&other)=="cat" && fake::state.opens[kLm]==3 && fake::used()==2*bytes);
    first.moveTo(other);
    assert(other.warm() && !first.warm() && fake::used()==bytes);
    other.reset(); assert(fake::state.heap.empty() && !other.warm());
}
void cancellation() {
    reset(); fake::state.files[kLm]=meetingLm();
    fake::state.maxRead=16; fake::state.cancelAfterReads=3; // mid-read (file is ~150 bytes)
    STTLocalSession session;
    bool ok=true;
    assert(run(&session,&ok)=="!Cancelled" && !ok && fake::state.heap.empty());
    assert(fake::state.lmDecodeLogs.size()==1 && fake::state.lmDecodeLogs[0].find("status=load_cancelled history_start=-1 history_end=-1")!=std::string::npos);
    fake::state.cancelAfterReads=-1;
    assert(run(&session)=="cat" && fake::state.opens[kLm]==2); // cancellation allows a retry
    assert(fake::state.logs.size()==1 && logged("I [STT] Language model"));
    session.reset(); assert(fake::state.heap.empty());
}
#endif
}

int main() {
#if ENABLE_STT_LM
    missing(); crossPhraseMetadata(); configuredStrengthReachesDecoder(); loadedOncePerSession(); customWords(); invalid(); memory(); released(); handOff(); cancellation();
#else
    // Flag off: no decoder, no LM file access, greedy text unchanged.
    reset(); fake::state.files[kLm]=meetingLm();
    { STTLocalSession session; assert(run(&session)=="greedy" && run(nullptr)=="greedy"); }
    assert(fake::state.exists.empty() && !fake::state.opens.count(kLm) && !fake::state.opens.count(kWords) && fake::state.logs.empty() && fake::state.lmDecodeLogs.empty());
    { STTLocalSession session, next; run(&session); assert(session.warm()); session.moveTo(next); assert(next.warm() && !session.warm()); }
#endif
    reset();
    std::printf("System_STTLocal language-model integration tests passed (ENABLE_STT_LM=%d, strength=%.2f)\n",ENABLE_STT_LM,double(HW1_STT_LM_STRENGTH));
}
