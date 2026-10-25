// The Python driver inserts the actual System_STTVocabulary.cpp (includes
// removed) at INSERT_MODULE and links the real stt/stt_transcript.cpp. The
// guarded VFS, the staged writer, the filesystem lock, the allocator, the
// FreeRTOS mutex and logging are fakes that record what the module asked for.
#include "stt/stt_transcript.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#define ENABLE_LOCAL_STT 1
// --- fakes -------------------------------------------------------------------
namespace fake {
struct State {
    std::map<std::string,std::string> files;
    std::set<std::string> folders{"/STT Models", "/sd/STT Models"};
    std::vector<std::string> logs, scopes, reasons, staged;
    bool sdAvailable=true, sdWritable=true, lockAvailable=true, denyWrite=false, denyStage=false, failCommit=false;
    size_t shortWriteAt=0, writes=0, opens=0, commits=0, discards=0, allocations=0, frees=0, failAllocAfter=~size_t(0);
    int mutexHeld=0; bool mutexBusy=false;
} state;
void log(const char* level, const char* fmt, ...) {
    char line[512]; va_list args; va_start(args,fmt); std::vsnprintf(line,sizeof(line),fmt,args); va_end(args);
    state.logs.push_back(std::string(level)+line);
}
}
#define INFO_SYSTEMF(...) fake::log("I ",__VA_ARGS__)
#define WARN_SYSTEMF(...) fake::log("W ",__VA_ARGS__)
// Arduino String (the module uses it only through VFS signatures)
struct String : std::string {
    using std::string::string;
    String()=default;
    String(const std::string& s):std::string(s) {}
    String(const char* s):std::string(s?s:"") {}
};
struct AuthContext { String user, scope, reason; };
// FreeRTOS
typedef void* SemaphoreHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
SemaphoreHandle_t xSemaphoreCreateMutex() { return new int(1); }
int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { if(fake::state.mutexBusy)return 0; ++fake::state.mutexHeld; return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t) { --fake::state.mutexHeld; }
void vSemaphoreDelete(SemaphoreHandle_t h) { delete static_cast<int*>(h); }
// allocator (System_MemUtil)
enum class AllocPref : uint8_t { PreferPSRAM, RequirePSRAM };
void* ps_alloc(size_t n, AllocPref, const char*) {
    if(fake::state.allocations++>=fake::state.failAllocAfter)return nullptr;
    return std::malloc(n);
}
void ps_free(void* p) { if(p)++fake::state.frees; std::free(p); }
// filesystem lock (System_Mutex)
struct FsLockGuard { bool held; FsLockGuard(const char*, TickType_t):held(fake::state.lockAvailable) {} };
bool isFsLockedByCurrentTask() { return false; }
// guarded VFS
struct File {
    std::string path; bool valid=false, dir=false; size_t at=0, cap=0;
    explicit operator bool() const { return valid; }
    bool isDirectory() const { return dir; }
    size_t size() const { return fake::state.files.at(path).size(); }
    size_t read(uint8_t* out, size_t n) {
        const auto& data=fake::state.files.at(path);
        n=std::min({n,data.size()-at,cap?cap:n});
        std::memcpy(out,data.data()+at,n); at+=n; return n;
    }
    size_t write(const uint8_t* bytes, size_t n) {
        if(++fake::state.writes==fake::state.shortWriteAt)n/=2;
        fake::state.files[path].append(reinterpret_cast<const char*>(bytes),n); return n;
    }
    void close() {}
};
namespace VFS {
namespace Scopes { constexpr const char* STT_MODELS="/STT Models"; constexpr const char* STT_MODELS_SD="/sd/STT Models"; }
AuthContext systemAuth(const char* scope, const char* reason) {
    assert(scope && reason && *reason);
    fake::state.scopes.push_back(scope); fake::state.reasons.push_back(reason);
    AuthContext c; c.user="system"; c.scope=scope; c.reason=reason; return c;
}
bool within(const std::string& path, const AuthContext& ctx) {
    return path==ctx.scope || path.compare(0,ctx.scope.size()+1,ctx.scope+"/")==0;
}
bool existsGuarded(const String& path, const AuthContext& ctx) {
    assert(within(path,ctx));
    return fake::state.files.count(path) || fake::state.folders.count(path);
}
File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create=false) {
    assert(within(path,ctx) && !std::strcmp(mode,"r") && !create); ++fake::state.opens;
    File f; f.path=path; f.valid=fake::state.files.count(path)!=0; return f;
}
bool mkdirGuarded(const String& path, const AuthContext& ctx) { assert(within(path,ctx)); fake::state.folders.insert(path); return true; }
bool canWriteGuarded(const String& path, const AuthContext& ctx) { assert(within(path,ctx)); return !fake::state.denyWrite; }
bool isSDAvailable() { return fake::state.sdAvailable; }
bool isSDWritable() { return fake::state.sdAvailable && fake::state.sdWritable; }
// The staged writer: the destination changes only at commit, all or nothing.
class StagedWrite {
 public:
    explicit StagedWrite(const char* writer):writer_(writer) { assert(!std::strcmp(writer_,"stt.vocab")); }
    bool begin(const String& destination, const AuthContext& ctx) {
        assert(within(destination,ctx));
        if(fake::state.denyStage)return false;
        destination_=destination; temporary_=destination+".upload.tmp";
        fake::state.files[temporary_].clear(); file_.path=temporary_; file_.valid=true; open_=true;
        fake::state.staged.push_back(temporary_);
        return true;
    }
    File& file() { return file_; }
    bool commit(const AuthContext& ctx) {
        assert(open_ && within(destination_,ctx)); open_=false; ++fake::state.commits;
        if(fake::state.failCommit) { fake::state.files.erase(temporary_); return false; }
        fake::state.files[destination_]=fake::state.files.at(temporary_); fake::state.files.erase(temporary_); return true;
    }
    void discard(const AuthContext&) { if(open_) { ++fake::state.discards; fake::state.files.erase(temporary_); open_=false; } }
 private:
    [[maybe_unused]] const char* writer_; std::string destination_, temporary_; File file_; bool open_=false;
};
}
// INSERT_MODULE

// --- cases -------------------------------------------------------------------
namespace {
constexpr const char* kSd="/sd/STT Models/user_vocab.tsv";
constexpr const char* kInternal="/STT Models/user_vocab.tsv";
size_t logged(const char* needle) { size_t n=0; for(const auto& l:fake::state.logs)n+=l.find(needle)!=std::string::npos; return n; }
std::string hotwords() { char text[STT_VOCAB_HOTWORD_BYTES]; size_t n=0; sttVocabularyHotwords(text,sizeof(text),&n); assert(n==std::strlen(text)); return text; }
std::string shown(const char* raw, bool paragraph=false) { char out[1024]; const bool ok=sttFormatDisplay(raw,paragraph,out,sizeof(out)); return ok?out:std::string("!")+out; }
// The module's store is file-static and outlives each case; the cases below
// start it the way a boot does (empty, never loaded).
void reset() { fake::state=fake::State{}; if(gStore.active)gStore.active->clear(); gStore.generation=0; gStore.path[0]=0; gStore.sd=false; }
void loadsCardFirst() {
    reset();
    fake::state.files[kSd]="HardwareOne\nESP32\tesp\nOpenAI\topenai\n";
    fake::state.files[kInternal]="Internal\n";
    sttVocabularyInit();
    assert(sttVocabularyCount()==3 && !std::strcmp(sttVocabularyPath(),kSd) && sttVocabularyGeneration()==1);
    assert(hotwords()=="hardwareone\nesp\nopenai\n");
    assert(shown("i said hardwareone runs on esp",true)=="I said HardwareOne runs on ESP32");
    assert(shown("openai  and\tinternal")=="OpenAI and internal");   // the internal file was not merged
    hw1::transcript::Word w; assert(sttVocabularyEntry(1,&w) && !std::strcmp(w.display,"ESP32") && !std::strcmp(w.spoken,"esp"));
    assert(!sttVocabularyEntry(3,&w));
    assert(logged("I [STT] Vocabulary: 3 terms from /sd/STT Models/user_vocab.tsv")==1);
    // Reads are confined to the model folder, never unscoped.
    for(const auto& s:fake::state.scopes)assert(s=="/sd/STT Models" || s=="/STT Models");
}
void internalWhenNoCardCopy() {
    reset(); fake::state.files[kInternal]="Only\n";
    sttVocabularyInit();
    assert(sttVocabularyCount()==1 && !std::strcmp(sttVocabularyPath(),kInternal) && hotwords()=="only\n");
    // No file anywhere: empty list, future saves go to the card when present.
    reset(); sttVocabularyInit();
    assert(sttVocabularyCount()==0 && !std::strcmp(sttVocabularyPath(),kSd) && hotwords().empty() && fake::state.logs.empty());
    reset(); fake::state.sdAvailable=false; sttVocabularyInit();
    assert(!std::strcmp(sttVocabularyPath(),kInternal));
}
void invalidFileKeepsActive() {
    reset(); fake::state.files[kSd]="Good\n"; sttVocabularyInit();
    const uint32_t g0=sttVocabularyGeneration();   // the module's store outlives each case: compare relatively
    assert(sttVocabularyCount()==1 && g0);
    char error[128];
    fake::state.files[kSd]="Good\nbad word\n";
    assert(!sttVocabularyReload(error,sizeof(error)) && std::strstr(error,"Line 2"));
    assert(sttVocabularyCount()==1 && sttVocabularyGeneration()==g0 && hotwords()=="good\n");
    fake::state.files[kSd]=std::string(49*1024,'a');
    assert(!sttVocabularyReload(error,sizeof(error)) && std::strstr(error,"too large") && sttVocabularyCount()==1);
    std::string many; for(int i=0;i<257;++i)many+="w"+std::to_string(i)+"\tw\n";
    fake::state.files[kSd]=many;
    assert(!sttVocabularyReload(error,sizeof(error)) && std::strstr(error,"256") && sttVocabularyCount()==1);
    fake::state.files[kSd]="Blank\n\n\r\nLines\r\n";   // blank lines and CRLF are fine
    assert(sttVocabularyReload(error,sizeof(error)) && sttVocabularyCount()==2 && sttVocabularyGeneration()==g0+1);
    // A boot with an invalid file: warning, empty list, nothing crashes.
    reset(); fake::state.files[kSd]="x y\n"; sttVocabularyInit();
    assert(sttVocabularyCount()==0 && logged("W [STT] Vocabulary not loaded (Line 1:")==1);
}
void editsStageThenAdopt() {
    reset(); fake::state.files[kSd]="HardwareOne\n"; sttVocabularyInit();
    const uint32_t g0=sttVocabularyGeneration();
    char error[128]={};
    assert(sttVocabularyAdd("ESP32","esp",error,sizeof(error)) && !error[0]);
    assert(fake::state.files.at(kSd)=="HardwareOne\thardwareone\nESP32\tesp\n");
    assert(fake::state.commits==1 && fake::state.staged.size()==1 && fake::state.staged[0]==std::string(kSd)+".upload.tmp");
    assert(sttVocabularyGeneration()==g0+1 && hotwords()=="hardwareone\nesp\n");
    assert(sttVocabularyAdd("OpenAI",nullptr,error,sizeof(error)) && sttVocabularyCount()==3 && sttVocabularyGeneration()==g0+2);
    assert(sttVocabularyAdd("HARDWAREONE","hardwareone",error,sizeof(error)) && sttVocabularyCount()==3); // replace
    assert(shown("hardwareone")=="HARDWAREONE");
    assert(sttVocabularyRemove("esp",error,sizeof(error)) && sttVocabularyCount()==2 && hotwords()=="hardwareone\nopenai\n");
    assert(!sttVocabularyRemove("esp",error,sizeof(error)) && std::strstr(error,"not in"));
    assert(!sttVocabularyRemove("",error,sizeof(error)) && !sttVocabularyRemove(nullptr,error,sizeof(error)));
    assert(fake::state.files.at(kSd)=="HARDWAREONE\thardwareone\nOpenAI\topenai\n");
    // Invalid terms never touch the file or the active list.
    const size_t commits=fake::state.commits, generation=sttVocabularyGeneration();
    assert(!sttVocabularyAdd("two words",nullptr,error,sizeof(error)) && error[0]);
    assert(!sttVocabularyAdd("ESP32",nullptr,error,sizeof(error)));
    assert(fake::state.commits==commits && sttVocabularyGeneration()==generation);
    // Writes go through the scoped system identity of the active tier only.
    for(const auto& s:fake::state.scopes)assert(s=="/sd/STT Models");
    assert(fake::state.mutexHeld==0);
}
void failedSaveKeepsPrevious() {
    reset(); fake::state.files[kSd]="Keep\n"; sttVocabularyInit();
    const uint32_t g0=sttVocabularyGeneration();
    char error[128];
    const std::string before=fake::state.files.at(kSd);
    fake::state.denyStage=true;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"stage"));
    fake::state.denyStage=false; fake::state.failCommit=true;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"previous file kept"));
    fake::state.failCommit=false; fake::state.shortWriteAt=fake::state.writes+1;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"incomplete") && fake::state.discards==1);
    fake::state.shortWriteAt=0; fake::state.lockAvailable=false;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"busy"));
    fake::state.lockAvailable=true; fake::state.denyWrite=true;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"not writable"));
    fake::state.denyWrite=false; fake::state.sdWritable=false;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"SD card"));
    fake::state.sdWritable=true; fake::state.failAllocAfter=fake::state.allocations;
    assert(!sttVocabularyAdd("New",nullptr,error,sizeof(error)) && std::strstr(error,"memory"));
    fake::state.failAllocAfter=~size_t(0);
    assert(fake::state.files.at(kSd)==before && sttVocabularyCount()==1 && sttVocabularyGeneration()==g0 && hotwords()=="keep\n");
    for(const auto& f:fake::state.files)assert(f.first.find(".upload.tmp")==std::string::npos); // no leftovers
    assert(fake::state.mutexHeld==0);
}
void busyListShowsRaw() {
    reset(); fake::state.files[kSd]="Name\n"; sttVocabularyInit();
    fake::state.mutexBusy=true;
    char out[64]="stale";
    assert(!sttFormatDisplay("name",false,out,sizeof(out)) && !out[0]);
    assert(sttVocabularyHotwords(out,sizeof(out),nullptr)==0 && !out[0] && sttVocabularyCount()==0);
    char error[64]; assert(!sttVocabularyAdd("X",nullptr,error,sizeof(error)) && std::strstr(error,"busy"));
    fake::state.mutexBusy=false;
    assert(shown("name")=="Name");
    // Bounded output: an alias that does not fit yields an empty string, never a cut term.
    char tight[4]; assert(!sttFormatDisplay("name",false,tight,sizeof(tight)) && !tight[0]);
    assert(!sttFormatDisplay(nullptr,false,out,sizeof(out)) && !sttFormatDisplay("x",false,nullptr,1));
}
}
int main() {
    loadsCardFirst(); internalWhenNoCardCopy(); invalidFileKeepsActive(); editsStageThenAdopt();
    failedSaveKeepsPrevious(); busyListShowsRaw();
    std::puts("System_STTVocabulary: load, reload, staged edits, failures and formatting passed");
}
