// The Python driver inserts actual headers/container/runtime below. Only the
// external dependencies are fakes. Never copy the production cache algorithm here.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

// INSERT_INTERFACE

namespace hw1::stt::identity {
inline constexpr size_t kSampleRate=16000, kMaxSamples=480000;
// Large enough that a warm free-memory budget cannot accidentally include a
// second copy; compressed input crosses the runtime's 4096-byte read boundary.
inline constexpr size_t kRawBytes=2*1024*1024, kCompressedBytes=5000;
inline constexpr int kInputExponent=-4, kOutputExponent=-2, kNodeCount=3;
inline constexpr const char* kInputName="input";
inline constexpr const char* kOutputName="output";
inline constexpr uint8_t kRawSha[32]={
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42};
inline constexpr uint8_t kFrontendSha[32]={0x73};
}
// INSERT_CONTAINER

namespace fake {
struct Allocation { size_t size; bool sensitive; };
struct State {
    std::map<void*,Allocation> allocations;
    std::vector<std::string> events;
    std::vector<size_t> modelFrames;
    std::vector<int> runStages;
    size_t allocated=0, allocationCalls=0, failAllocation=0;
    size_t psramTotal=32*1024*1024, internalFree=1024*1024;
    size_t largest=32*1024*1024;
    bool fragmentAfterRaw=false;
    int rawAllocations=0, rawFrees=0, models=0, created=0, destroyed=0;
    int inflates=0, rawHashes=0, failHash=0, wrongHash=0;
    bool inflateError=false, inflateShort=false, frontendError=false;
    bool modelAllocationError=false, tensorError=false, decodeError=false, truncate=false;
    bool cancel=false, cancelAfterHash=false, cancelAfterStage=false;
    int cancelAtPhase=-1;
    STTLocalPhase phase=STTLocalPhase::Loading;
    bool mutateInDestructor=false;
    uint8_t* lastRaw=nullptr;
} state;
void clean() { assert(state.allocations.empty()); assert(state.models==0); state=State{}; }
bool hasRaw(const void* p) {
    auto found=state.allocations.find(const_cast<void*>(p));
    return found!=state.allocations.end() && found->second.size==hw1::stt::identity::kRawBytes;
}
size_t rawLive() {
    size_t result=0;
    for(const auto& item:state.allocations)if(item.second.size==hw1::stt::identity::kRawBytes)++result;
    return result;
}
void settled(size_t expectedRaw) {
    assert(state.models==0);
    assert(rawLive()==expectedRaw);
    assert(state.allocations.size()==expectedRaw); // no temporary/workspace leak
}
bool cancelled(void*) { return state.cancel; }
void progress(void*,STTLocalPhase phase) {
    state.phase=phase;
    if(state.cancelAtPhase==int(phase))state.cancel=true;
}
STTLocalControl control() { return {nullptr,cancelled,progress}; }
}
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_INTERNAL=4;
void* heap_caps_aligned_alloc(size_t alignment,size_t bytes,unsigned caps) {
    assert(alignment==16 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ++fake::state.allocationCalls;
    if(fake::state.allocationCalls==fake::state.failAllocation)return nullptr;
    void* p=nullptr;
    if(posix_memalign(&p,alignment,bytes))return nullptr;
    std::memset(p,0xcd,bytes);
    fake::state.allocations.emplace(p,fake::Allocation{bytes,fake::state.phase==STTLocalPhase::Frontend});
    fake::state.allocated+=bytes;
    if(bytes==hw1::stt::identity::kRawBytes) {
        ++fake::state.rawAllocations;fake::state.lastRaw=static_cast<uint8_t*>(p);
        fake::state.events.push_back("allocate weights");
    }
    return p;
}
void heap_caps_free(void* p) {
    if(!p)return;
    auto found=fake::state.allocations.find(p);
    assert(found!=fake::state.allocations.end()); // double-free / unowned pointer
    if(found->second.size==hw1::stt::identity::kRawBytes) {
        assert(fake::state.models==0); // vendor weights cannot outlive backing
        ++fake::state.rawFrees;fake::state.events.push_back("free weights");
    }
    if(found->second.sensitive) {
        auto bytes=static_cast<const uint8_t*>(p);
        for(size_t i=0;i<found->second.size;++i)assert(bytes[i]==0);
    }
    fake::state.allocated-=found->second.size;
    fake::state.allocations.erase(found);std::free(p);
}
size_t heap_caps_get_free_size(unsigned caps) {
    if(caps&MALLOC_CAP_INTERNAL)return fake::state.internalFree;
    assert(caps==MALLOC_CAP_SPIRAM);
    return fake::state.psramTotal>fake::state.allocated?fake::state.psramTotal-fake::state.allocated:0;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(caps==MALLOC_CAP_SPIRAM);
    if(fake::state.fragmentAfterRaw && fake::rawLive())return 1024;
    return fake::state.largest;
}
int64_t esp_timer_get_time() { static int64_t clock=0; return clock+=1000; }
void vTaskDelay(int ticks) { assert(ticks==1); }
int mbedtls_sha256(const unsigned char* input,size_t size,unsigned char* output,int is224) {
    assert(is224==0);
    if(size==hw1::stt::identity::kRawBytes) {
        ++fake::state.rawHashes;
        assert(fake::hasRaw(input));
        if(fake::state.rawHashes==fake::state.failHash)return -1;
        bool valid=true;for(size_t i=0;i<size;++i)if(input[i]!=0x5a){valid=false;break;}
        if(fake::state.rawHashes==fake::state.wrongHash)valid=false;
        std::memset(output,valid?0x42:0x43,32);
        if(fake::state.cancelAfterHash)fake::state.cancel=true;
    } else std::memset(output,0x77,32);
    return 0;
}
struct tinfl_decompressor { unsigned calls; };
enum tinfl_status { TINFL_STATUS_FAILED=-1,TINFL_STATUS_DONE=0,TINFL_STATUS_NEEDS_MORE_INPUT=1 };
constexpr unsigned TINFL_FLAG_PARSE_ZLIB_HEADER=1,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF=2,TINFL_FLAG_HAS_MORE_INPUT=4;
void tinfl_init(tinfl_decompressor* d) { d->calls=0; }
tinfl_status tinfl_decompress(tinfl_decompressor* d,const uint8_t* input,size_t* in,
                              uint8_t* base,uint8_t* output,size_t* out,unsigned flags) {
    ++d->calls;++fake::state.inflates;
    assert(fake::hasRaw(base));assert(*in>0);
    for(size_t i=0;i<*in;++i)assert(input[i]==0xa7);
    if(fake::state.inflateError)return TINFL_STATUS_FAILED;
    const bool more=flags&TINFL_FLAG_HAS_MORE_INPUT;
    const size_t count=more?std::min(*out,size_t(128)):*out-(fake::state.inflateShort?1:0);
    std::memset(output,0x5a,count);*out=count;
    return more?TINFL_STATUS_NEEDS_MORE_INPUT:TINFL_STATUS_DONE;
}
namespace fbs { constexpr int MODEL_LOCATION_IN_FLASH_RODATA=1; }
namespace dl {
constexpr int DATA_TYPE_INT8=1,MEMORY_MANAGER_GREEDY=1,RUNTIME_MODE_MULTI_CORE=1;
struct Exponent { int value; bool is_valid()const{return true;} bool is_per_channel()const{return false;} explicit operator int()const{return value;} };
struct TensorBase {
    std::vector<int> shape;
    std::vector<int8_t> storage;
    void* data=nullptr;
    int dtype=DATA_TYPE_INT8;
    Exponent exponent{0};
    void init(size_t frames,size_t channels,int exp) {
        shape={1,int(frames),1,int(channels)};storage.resize(frames*channels);data=storage.data();exponent.value=exp;
    }
    int get_bytes()const{return int(storage.size());}
};
struct Memory { size_t psram,internal; };
class Model {
    const void* weights;
    TensorBase input,output;
 public:
    static void* operator new(size_t n,const std::nothrow_t&)noexcept {
        return fake::state.modelAllocationError?nullptr:std::malloc(n);
    }
    static void operator delete(void* p)noexcept {std::free(p);}
    static void operator delete(void* p,const std::nothrow_t&)noexcept {std::free(p);}
    Model(const char* raw,int location,int index,int manager,void* unused,bool copy,
          const std::map<std::string,std::vector<int>>& shapes):weights(raw) {
        assert(fake::hasRaw(raw));assert(location==fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        assert(index==0 && manager==MEMORY_MANAGER_GREEDY && !unused && !copy);
        assert(fake::state.models==0);++fake::state.models;++fake::state.created;
        fake::state.events.push_back("create model");
        const auto shape=shapes.at(hw1::stt::identity::kInputName);
        assert(shape[0]==1 && shape[2]==1 && shape[3]==64);
        const size_t frames=size_t(shape[1]);fake::state.modelFrames.push_back(frames);
        input.init(frames,64,hw1::stt::identity::kInputExponent);
        output.init((frames+1)/2,29,hw1::stt::identity::kOutputExponent);
    }
    ~Model() {
        assert(fake::hasRaw(weights));
        if(fake::state.mutateInDestructor)static_cast<uint8_t*>(const_cast<void*>(weights))[17]^=1;
        --fake::state.models;++fake::state.destroyed;fake::state.events.push_back("destroy model");
    }
    TensorBase* get_input(const char*) { if(fake::state.tensorError)input.exponent.value=99;return &input; }
    TensorBase* get_output(const char*) {return &output;}
    std::map<std::string,Memory> get_memory_info()const{return {{"variable",{4096,512}}};}
    void run(int count,int stage,int mode) {
        assert(fake::hasRaw(weights));assert(count==hw1::stt::identity::kNodeCount && mode==RUNTIME_MODE_MULTI_CORE);
        fake::state.runStages.push_back(stage);
        if(fake::state.cancelAfterStage)fake::state.cancel=true;
    }
};
}
namespace hw1::stt {
size_t feature_frames(size_t samples) {return (samples+159)/160;}
Status compute_features(const int16_t*,size_t samples,float* output,size_t count,FrontendWorkspace*,uint32_t,bool) {
    assert(count==feature_frames(samples)*kMelBins);
    if(fake::state.frontendError)return Status::NonFinite;
    for(size_t i=0;i<count;++i)output[i]=float(int(i%5)-2)*0.0625f;
    return Status::Ok;
}
Status ctc_reset(CtcState* state,char* text,size_t cap) {assert(cap>2);*state={};text[0]=0;return Status::Ok;}
Status ctc_feed(CtcState* state,const int8_t*,size_t frames,size_t stride,char* text,size_t cap) {
    assert(stride==29 && cap>2 && frames>0);
    if(fake::state.decodeError)return Status::InvalidArgument;
    state->truncated=fake::state.truncate;std::strcpy(text,"ok");return Status::Ok;
}
Status ctc_finish(CtcState* state,char*,size_t) {state->finished=true;return Status::Ok;}
}

// INSERT_RUNTIME

namespace test {
using namespace hw1::stt;
struct Reader {
    std::vector<uint8_t> data;
    size_t offset=0, bytesRead=0, calls=0, maxRead=73, stopAt=std::numeric_limits<size_t>::max();
    Reader():data(container::kHeaderBytes+identity::kCompressedBytes,0xa7) {
        std::fill(data.begin(),data.begin()+container::kHeaderBytes,0);
        std::memcpy(data.data(),"HW1STT1\0",8);
        word(8,1);word(12,1);word(16,identity::kRawBytes);word(20,identity::kCompressedBytes);
        word(24,identity::kSampleRate);word(28,identity::kMaxSamples);
        std::memcpy(data.data()+32,identity::kRawSha,32);std::memcpy(data.data()+64,identity::kFrontendSha,32);
    }
    void word(size_t offset,uint32_t value) {for(unsigned i=0;i<4;++i)data[offset+i]=uint8_t(value>>(8*i));}
    static size_t read(void* context,void* dest,size_t size) {
        auto& self=*static_cast<Reader*>(context);++self.calls;
        if(self.offset>=self.stopAt)return 0;
        size=std::min({size,self.maxRead,self.data.size()-self.offset,self.stopAt-self.offset});
        std::memcpy(dest,self.data.data()+self.offset,size);self.offset+=size;self.bytesRead+=size;return size;
    }
    ModelReader model() {return {this,data.size(),read};}
};
struct Result { bool ok;STTLocalStats stats;std::string text,error; };
Result call(ModelCache* cache,Reader& reader,size_t samples=320) {
    std::vector<int16_t> pcm(samples,42);char text[64],error[128];STTLocalStats stats;
    Diagnostics diagnostics;
    const bool ok=transcribe(reader.model(),pcm.data(),samples,text,sizeof(text),fake::control(),stats,error,sizeof(error),&diagnostics,cache);
    if(ok) {assert(std::string(text)=="ok");assert(!error[0]);assert(stats.samples==samples);}
    else {assert(!text[0]);assert(error[0]);}
    return {ok,stats,text,error};
}
size_t remainingBudget(size_t samples) {
    const size_t frames=feature_frames(samples),outputs=(frames+1)/2;
    return 2048*outputs+64*1024+frames*kMelBins*sizeof(float)+sizeof(FrontendWorkspace)+1024*1024;
}
void coldWarmShapes() {
    fake::clean();
    {
        ModelCache cache;
        Reader a;auto first=call(&cache,a);
        assert(first.ok && !first.stats.weightsReused && first.stats.featureFrames==2 && first.stats.outputFrames==1);
        assert(a.bytesRead==a.data.size());
        assert(fake::state.inflates==2 && fake::state.rawHashes==1);fake::settled(1);
        Reader b;auto second=call(&cache,b,480);
        assert(second.ok && second.stats.weightsReused && second.stats.featureFrames==3 && second.stats.outputFrames==2);
        assert(b.bytesRead==container::kHeaderBytes && fake::state.inflates==2 && fake::state.rawHashes==2);
        Reader c;auto third=call(&cache,c);
        assert(third.ok && third.stats.weightsReused && third.stats.featureFrames==2 && third.stats.outputFrames==1);
        assert(c.bytesRead==container::kHeaderBytes);
        assert(fake::state.modelFrames==std::vector<size_t>({2,3,2}));
        assert(fake::state.created==3 && fake::state.destroyed==3 && fake::state.rawAllocations==1);
        assert(fake::state.rawHashes==3);fake::settled(1);
        cache.reset();fake::settled(0);assert(fake::state.rawFrees==1);
        cache.reset();assert(fake::state.rawFrees==1);
    }
    assert(fake::state.rawFrees==1);fake::settled(0);
}
void oneShotOrder() {
    fake::clean();
    for(int i=0;i<2;++i){Reader reader;auto result=call(nullptr,reader);assert(result.ok && !result.stats.weightsReused);fake::settled(0);}
    assert(fake::state.rawAllocations==2 && fake::state.rawFrees==2 && fake::state.inflates==4);
    const std::vector<std::string> expected={"allocate weights","create model","destroy model","free weights",
                                           "allocate weights","create model","destroy model","free weights"};
    assert(fake::state.events==expected);
}
void failuresBeforePublish() {
    // Failure at the source envelope, allocation, compressed read, inflate or
    // digest boundary must never publish unverified weights into the cache.
    for(int scenario=0;scenario<11;++scenario) {
        fake::clean();ModelCache cache;Reader reader;
        switch(scenario) {
            case 0:reader.data[0]^=1;break;
            case 1:reader.data.pop_back();break;
            case 2:fake::state.failAllocation=1;break;
            case 3:fake::state.failAllocation=2;break;
            case 4:reader.stopAt=container::kHeaderBytes+4200;break;
            case 5:fake::state.inflateError=true;break;
            case 6:fake::state.inflateShort=true;break;
            case 7:fake::state.failHash=1;break;
            case 8:fake::state.wrongHash=1;break;
            case 9:fake::state.cancelAfterHash=true;break;
            case 10:fake::state.cancel=true;break;
        }
        auto result=call(&cache,reader);assert(!result.ok);assert(fake::state.created==0);fake::settled(0);
        fake::state.failAllocation=0;fake::state.inflateError=false;fake::state.inflateShort=false;
        fake::state.failHash=0;fake::state.wrongHash=0;fake::state.cancelAfterHash=false;fake::state.cancel=false;
        Reader retry;auto recovered=call(&cache,retry);assert(recovered.ok && !recovered.stats.weightsReused);
        assert(retry.bytesRead==retry.data.size());cache.reset();fake::settled(0);
    }
}
void failuresAfterPublish() {
    for(int scenario=0;scenario<11;++scenario) {
        fake::clean();ModelCache cache;Reader reader;
        switch(scenario) {
            case 0:fake::state.fragmentAfterRaw=true;break;
            case 1:fake::state.modelAllocationError=true;break;
            case 2:fake::state.tensorError=true;break;
            case 3:fake::state.failAllocation=3;break;
            case 4:fake::state.failAllocation=4;break;
            case 5:fake::state.frontendError=true;break;
            case 6:fake::state.decodeError=true;break;
            case 7:fake::state.truncate=true;break;
            case 8:fake::state.cancelAtPhase=int(STTLocalPhase::Frontend);break;
            case 9:fake::state.cancelAfterStage=true;break;
            case 10:fake::state.cancelAtPhase=int(STTLocalPhase::Decoding);break;
        }
        auto result=call(&cache,reader);assert(!result.ok);fake::settled(1);
        assert(fake::state.rawHashes==1 && fake::state.rawFrees==0);
        fake::state.fragmentAfterRaw=false;fake::state.modelAllocationError=false;fake::state.tensorError=false;
        fake::state.failAllocation=0;fake::state.frontendError=false;fake::state.decodeError=false;fake::state.truncate=false;
        fake::state.cancelAtPhase=-1;fake::state.cancelAfterStage=false;fake::state.cancel=false;
        Reader retry;auto recovered=call(&cache,retry);assert(recovered.ok && recovered.stats.weightsReused);
        assert(retry.bytesRead==container::kHeaderBytes && fake::state.rawHashes==2);
        cache.reset();fake::settled(0);assert(fake::state.rawFrees==1);
    }
    // A one-shot post-publish failure must also release the retained temporary.
    fake::clean();fake::state.frontendError=true;Reader reader;assert(!call(nullptr,reader).ok);fake::settled(0);
    assert(fake::state.rawFrees==1 && fake::state.destroyed==1);
}
void corruptedAndFailedHash() {
    for(int scenario=0;scenario<3;++scenario) {
        fake::clean();ModelCache cache;Reader cold;assert(call(&cache,cold).ok);
        const int created=fake::state.created;
        if(scenario==0)fake::state.lastRaw[17]^=1;
        if(scenario==1)fake::state.failHash=2;
        if(scenario==2)fake::state.wrongHash=2;
        Reader warm;auto result=call(&cache,warm);
        assert(!result.ok && result.error=="Cached STT model checksum mismatch");
        assert(fake::state.created==created && fake::state.inflates==2 && warm.bytesRead==container::kHeaderBytes);
        assert(fake::state.rawHashes==2 && fake::state.rawFrees==1);fake::settled(0);
        fake::state.failHash=0;fake::state.wrongHash=0;
        Reader retry;assert(call(&cache,retry).ok);assert(retry.bytesRead==retry.data.size());
        assert(fake::state.rawAllocations==2);cache.reset();assert(fake::state.rawFrees==2);
    }
}
void warmBudgetAndEnvelope() {
    fake::clean();ModelCache cache;Reader cold;assert(call(&cache,cold).ok);
    // Free memory admits activations/frontend, but cannot fit a second weights
    // copy. Largest block is similarly smaller than the pinned raw model.
    fake::state.psramTotal=identity::kRawBytes+remainingBudget(320)+1024;
    fake::state.largest=remainingBudget(320)+1024;
    assert(fake::state.largest<identity::kRawBytes);
    Reader warm;assert(call(&cache,warm).ok);assert(warm.bytesRead==container::kHeaderBytes);
    // A changed/invalid current file envelope is still rejected on a cache hit.
    const int created=fake::state.created,hashes=fake::state.rawHashes;
    Reader bad;bad.data[32]^=1;assert(!call(&cache,bad).ok);
    assert(fake::state.created==created && fake::state.rawHashes==hashes);fake::settled(1);
    // Warm calls still enforce temporary-memory guards without throwing away
    // already verified weights; recovery must remain a header-only cache hit.
    fake::state.internalFree=150*1024-1;
    Reader lowInternal;assert(!call(&cache,lowInternal).ok);fake::settled(1);
    assert(fake::state.created==created && fake::state.rawHashes==hashes);
    fake::state.internalFree=1024*1024;
    Reader recovered;auto resumed=call(&cache,recovered);
    assert(resumed.ok && resumed.stats.weightsReused && recovered.bytesRead==container::kHeaderBytes);
    cache.reset();
    const int beforeDenied=fake::state.created;
    fake::state.psramTotal=remainingBudget(320)+1024;
    Reader denied;assert(!call(&cache,denied).ok);assert(fake::state.created==beforeDenied);fake::settled(0);
}
void diagnosticsAfterModelDestruction() {
#if HW1_STT_RUNTIME_DIAGNOSTICS
    fake::clean();ModelCache cache;
    assert(!cache.verify() && fake::state.rawHashes==0);
    Reader cold;assert(call(&cache,cold).ok);assert(fake::state.models==0 && fake::state.destroyed==1);
    assert(cache.verify() && fake::state.rawHashes==2);fake::settled(1);
    // A vendor destructor may be the last writer. The probe must verify after
    // transcribe returns, not merely trust the next call's pre-parse hash.
    fake::state.mutateInDestructor=true;
    Reader warm;assert(call(&cache,warm).ok);assert(fake::state.destroyed==2);
    assert(!cache.verify() && fake::state.rawHashes==4);fake::settled(1);
    assert(fake::state.rawFrees==0); // verify is observational, never resets
    Reader rejected;assert(!call(&cache,rejected).ok);
    assert(fake::state.created==2 && fake::state.rawFrees==1);fake::settled(0);
    assert(!cache.verify() && fake::state.rawHashes==5);
    fake::state.mutateInDestructor=false;
    Reader retry;auto result=call(&cache,retry);assert(result.ok && !result.stats.weightsReused);
    assert(cache.verify());cache.reset();assert(!cache.verify());fake::settled(0);
#endif
}
void guardLimitsAndDestruction() {
    for(int scenario=0;scenario<3;++scenario) {
        fake::clean();ModelCache cache;Reader reader;
        if(scenario==0)fake::state.psramTotal=identity::kRawBytes+remainingBudget(320)-1;
        if(scenario==1)fake::state.largest=identity::kRawBytes-1;
        if(scenario==2)fake::state.internalFree=150*1024-1;
        assert(!call(&cache,reader).ok);assert(fake::state.rawAllocations==0);fake::settled(0);
    }
    fake::clean();
    {ModelCache cache;Reader reader;assert(call(&cache,reader).ok);fake::settled(1);}
    fake::settled(0);assert(fake::state.rawFrees==1);
}
}
int main() {
    test::coldWarmShapes();test::oneShotOrder();test::failuresBeforePublish();test::failuresAfterPublish();
    test::corruptedAndFailedHash();test::warmBudgetAndEnvelope();test::guardLimitsAndDestruction();
    test::diagnosticsAfterModelDestruction();
    fake::clean();
    std::printf("QuartzNet actual runtime cache tests passed (diagnostics=%d)\n",HW1_STT_RUNTIME_DIAGNOSTICS);
}
