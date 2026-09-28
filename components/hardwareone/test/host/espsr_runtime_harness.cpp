#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>
using String = std::string;
using TaskHandle_t = void*;
using SemaphoreHandle_t = void*;
using TransportSessionEpoch = uint32_t;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
#define INFO_SRF(...) ((void)0)
#define WARN_SRF(...) ((void)0)
static uint32_t nowMs = 0;
static std::function<void()> delayHook;
uint32_t millis() { return nowMs; }
void vTaskDelay(uint32_t ms) { nowMs += ms; if (delayHook) delayHook(); }
static int deletedTasks = 0;
void vTaskDelete(void*) { ++deletedTasks; }

static std::atomic<bool> gESPSRRunning{false}, gSRFeedShouldRun{false}, gSRTaskShouldRun{false};
static std::atomic<bool> gSRFeedExited{true}, gSRTaskExited{true};
static void *gSRFeedTaskHandle = nullptr, *gSRTaskHandle = nullptr;
static int16_t *gSRFeedBuffer = nullptr, *gSRMNBuffer = nullptr;
static size_t gSRFeedSamples = 0, gSRFetchSamples = 0;
static char gWnModelName[64] = {}, gMnModelName[64] = {};
static bool gESPSRWakeDetected = false;
enum class VoiceState { IDLE };
static VoiceState gVoiceState = VoiceState::IDLE;
static String gCurrentCategory, gCurrentSubCategory;
static std::vector<String> destroyed;
static String error;
static std::atomic<const char*> gSRError{nullptr};
void srSetError(const char* message) { error = message; gSRError.store(message); }
static bool mnCanDestroy = true;
bool deinitMultiNet() { if (!mnCanDestroy) return false; destroyed.push_back("mn"); return true; }
void deinitAFE() { destroyed.push_back("afe"); }
void deinitSRModels() { destroyed.push_back("models"); }
void deinitI2SMicrophone() { destroyed.push_back("hal"); }
void restoreMicrophoneAfterSRIfNeeded() { destroyed.push_back("restore"); }
// INSERT_LIFECYCLE

static uint32_t gSrI2SReadZero=0, gSrG2ReadZero=0, gSrI2SReadOk=0, gSrAfeFeedOk=0;
static uint64_t gSrI2SBytesOk=0, gSrG2BytesOk=0;
static int16_t gSrLastPcmMin=0, gSrLastPcmMax=0;
static float gSrLastPcmAbsAvg=0;
static bool gSrSnipEnabled = true, gSrFiltersEnabled = true;
struct SRSnipGuard { explicit operator bool() const { return true; } };
constexpr int AUDIO_SRC_LOCAL_PDM = 1, AUDIO_SRC_G2_LEFT = 2;
static int source = AUDIO_SRC_LOCAL_PDM;
int audioGetSource() { return source; }
static const char* owner = "sr";
const char* audioCaptureOwner() { return owner; }
static std::vector<size_t> readSizes;
static size_t readAt = 0;
static int nextSample = 1;
static std::vector<int16_t> snippets;
size_t audioReadPcm(int16_t* out, size_t capacity, uint32_t timeout) {
  assert(timeout == 100);
  nowMs += 100;
  if (readAt == readSizes.size()) { gSRFeedShouldRun=false; return 0; }
  size_t got = readSizes[readAt++];
  assert(got <= capacity);
  for (size_t i=0; i<got; ++i) out[i]=nextSample++;
  return got;
}
void srSnipRingPush(const int16_t*,size_t) {}
void srSnipFeedSession(const int16_t* pcm,size_t count) { snippets.insert(snippets.end(),pcm,pcm+count); }
static unsigned processCalls = 0;
float getMicSoftwareGainMultiplier() { return 2; }
void applyMicAudioProcessing(int16_t* pcm,size_t count,float gain,bool filters) {
  assert(gain == 2 && filters); ++processCalls;
  for(size_t i=0;i<count;++i) pcm[i]*=2;
}
static std::vector<std::vector<int16_t>> fed;
static int feedResult = 1;
static int mockFeed(void*,const int16_t* pcm) { fed.emplace_back(pcm,pcm+gSRFeedSamples); return feedResult; }
struct AFE { int (*feed)(void*,const int16_t*); };
static AFE afe{mockFeed};
static AFE* gAFE = &afe;
static void* gAFEData = nullptr;
// INSERT_FEED

enum CommandSource { SOURCE_INTERNAL, SOURCE_SERIAL, SOURCE_WEB, SOURCE_LOCAL_DISPLAY,
  SOURCE_BLUETOOTH, SOURCE_UART, SOURCE_G2_GLASSES, SOURCE_VOICE };
struct AuthContext { CommandSource transport = SOURCE_INTERNAL; String user, ip, path, sid; };
struct CommandContext {
  int origin=0; AuthContext auth; uint32_t id=0,timestampMs=0,outputMask=0,behaviorFlags=0;
};
struct Command { String line; CommandContext ctx; };
struct ExecReq { using DeferredFn=void(*)(void*); };
constexpr int ORIGIN_VOICE=7, MSG_ROUTE_FILE=4, COMMAND_CONTEXT_MODE_INDEPENDENT=1;
constexpr size_t CMD_RESULT_MAX=4096;
enum class AllocPref { PreferPSRAM };
static bool allocFails=false, mutexAvailable=true, queueAvailable=true;
void* ps_alloc(size_t n,AllocPref,const char*) { return allocFails ? nullptr : malloc(n); }
static SemaphoreHandle_t gVoiceArmMutex = reinterpret_cast<void*>(1);
int xSemaphoreTake(SemaphoreHandle_t,uint32_t) { return mutexAvailable ? pdTRUE : 0; }
void xSemaphoreGive(SemaphoreHandle_t) {}
void ensureVoiceArmMutex() {}
static bool gVoiceArmed=false;
static String gVoiceArmedUser,gVoiceArmedByIp;
static CommandSource gVoiceArmedByTransport=SOURCE_INTERNAL;
static uint32_t gVoiceArmedAtMs=0,gVoiceArmGeneration=0;
static TransportSessionEpoch gVoiceArmEpoch=0,liveEpoch=1;
static bool gVoiceArmRequiresEpoch=false;
struct Settings { int srModelSource=0; int microphoneGain=70; bool serialRequireAuth=true,uartRequireAuth=true,localDisplayRequireAuth=true,bleRequireAuth=true; };
static Settings gSettings;
TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx) {
 return ctx.transport == SOURCE_WEB && ctx.sid.empty() ? 0 : liveEpoch;
}
bool transportSessionEpochIsLive(CommandSource source,uint32_t epoch) {
 assert(source != SOURCE_VOICE); return epoch==liveEpoch;
}
constexpr int SYSEVT_VOICE_DISARMED=1,SYSEVT_VOICE_ARMED=2,SYSEVT_VOICE_COMMAND=3;
static unsigned completedEvents=0, executions=0;
void systemEventPost(int kind,const char*) { if(kind==SYSEVT_VOICE_COMMAND) ++completedEvents; }
static void* currentContext = reinterpret_cast<void*>(0x1234);
void* currentCommandContext() { return currentContext; }
void setCurrentCommandContext(void* context) { currentContext=context; }
static std::vector<String> outputs;
void broadcastOutput(const String& text) { outputs.push_back(text); }
static std::atomic<uint32_t> gCommandCount{0};
static bool execResult = true;
static std::function<void()> executeHook;
bool executeCommand(AuthContext& ctx,const char*,char* out,size_t size) {
 assert(ctx.transport==SOURCE_VOICE && !ctx.user.empty());
 auto* cc=static_cast<CommandContext*>(currentContext);
 assert(cc->origin==ORIGIN_VOICE && cc->behaviorFlags==COMMAND_CONTEXT_MODE_INDEPENDENT);
 ++executions;
 if(executeHook) executeHook();
 snprintf(out,size,"%s",execResult?"OK":"denied"); return execResult;
}
static ExecReq::DeferredFn queuedFn=nullptr;
static void* queuedArg=nullptr;
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn,void* arg) {
 if(!queueAvailable) return false;
 assert(!queuedFn); queuedFn=fn; queuedArg=arg; return true;
}
static SemaphoreHandle_t gMNCommandMutex=nullptr;
static void* gMNModel=nullptr;
static void* gMNData=nullptr;
static bool gMNCommandsAllocated=false;
static int commandAllocCalls=0;
static int commandAllocResult=0;
constexpr int ESP_OK=0;
using esp_err_t=int;
bool ensureSRMutex(SemaphoreHandle_t& slot) {
 if(!slot && !allocFails) slot=reinterpret_cast<void*>(1);
 return slot!=nullptr;
}
int esp_mn_commands_alloc(void*,void*) { ++commandAllocCalls; return commandAllocResult; }
// INSERT_MN_LOCK
struct srmodel_list_t { int num; };
static srmodel_list_t modelList{2};
static srmodel_list_t* gSRModels=nullptr;
static srmodel_list_t* existingModels=nullptr;
static const char* gSRModelSource="none";
static bool fileLoads=false,partitionPresent=true;
static std::string requestedModelPath;
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_ANY=255;
srmodel_list_t* get_static_srmodels() { return existingModels; }
bool loadSRModelFile(const char* path) {
 requestedModelPath=path;
 if(!fileLoads) srSetError("file missing");
 return fileLoads;
}
void logSystemEvent(const char*,const char*) {}
void* esp_partition_find_first(int,int,const char* label) { assert(!strcmp(label,"model")); return partitionPresent?reinterpret_cast<void*>(1):nullptr; }
srmodel_list_t* esp_srmodel_init(const char*) { return &modelList; }
// INSERT_MODEL_SELECTION
static int micGain=50;
static unsigned dspResetCalls=0;
void resetMicAudioProcessingState() { ++dspResetCalls; }
// INSERT_DSP_SETTINGS
// INSERT_VOICE
static void runQueued() { auto fn=queuedFn; auto arg=queuedArg; queuedFn=nullptr; queuedArg=nullptr; fn(arg); }

static void setupPipeline() {
 destroyed.clear(); error.clear(); nowMs=0; delayHook=nullptr; mnCanDestroy=true;
 gSRFeedExited=false; gSRTaskExited=false; gSRFeedShouldRun=true; gSRTaskShouldRun=true;
 gSRFeedTaskHandle=reinterpret_cast<void*>(1); gSRTaskHandle=reinterpret_cast<void*>(2);
 gSRFeedBuffer=static_cast<int16_t*>(malloc(32)); gSRMNBuffer=static_cast<int16_t*>(malloc(32));
 gSRFeedSamples=gSRFetchSamples=16; gESPSRRunning=true;
}
static void testTeardown() {
 setupPipeline(); auto* feed=gSRFeedBuffer;
 assert(!stopESPSRLocked()); // feed blocked: detector must remain available to drain
 assert(!gSRFeedShouldRun && gSRTaskShouldRun && destroyed.empty());
 assert(gSRFeedBuffer==feed && gSRFeedTaskHandle && gSRTaskHandle);
 gSRFeedExited=true;
 assert(!stopESPSRLocked()); // detector stuck: retain every resource again
 assert(!gSRTaskShouldRun && destroyed.empty() && gSRFeedBuffer==feed);
 gSRTaskExited=true; mnCanDestroy=false;
 assert(!stopESPSRLocked() && destroyed.empty() && gSRFeedBuffer==feed);
 mnCanDestroy=true;
 assert(stopESPSRLocked());
 assert((destroyed==std::vector<String>{"mn","afe","models","hal","restore"}));
 assert(!gSRFeedBuffer && !gSRMNBuffer && !gSRTaskHandle && !gSRFeedTaskHandle);
 // Concurrent worker acknowledgements are observed before any destruction.
 setupPipeline();
 delayHook=[] { if(nowMs==20) gSRFeedExited=true; if(nowMs==40) gSRTaskExited=true; };
 assert(stopESPSRLocked() && nowMs==40); delayHook=nullptr;
 // Deadline arithmetic survives millis wrap.
 std::atomic<bool> done{false}; nowMs=UINT32_MAX-4;
 assert(!srWaitForWorker(done,20)); assert(nowMs==15);
}
static void testFeed() {
 for(int src: {AUDIO_SRC_LOCAL_PDM,AUDIO_SRC_G2_LEFT}) {
  source=src; nextSample=1; readAt=0; nowMs=0; processCalls=0; fed.clear(); snippets.clear();
  readSizes={1,0,2,1,4,2}; feedResult=1; owner="sr";
  int16_t buffer[4]={}; gSRFeedBuffer=buffer; gSRFeedSamples=4; gSRFeedExited=false; gSRFeedShouldRun=true;
  srFeedTask(nullptr);
  const int scale=src==AUDIO_SRC_LOCAL_PDM?2:1;
  assert(fed.size()==2);
  for(int chunk=0;chunk<2;++chunk) for(int i=0;i<4;++i) assert(fed[chunk][i]==(chunk*4+i+1)*scale);
  assert(snippets.size()==10 && snippets[0]==1 && snippets[9]==10);
  assert(processCalls==(src==AUDIO_SRC_LOCAL_PDM?2u:0u));
  assert(gSRFeedExited && !gSRFeedShouldRun);
 }
 int16_t buffer[4]={}; gSRFeedBuffer=buffer; gSRFeedSamples=4;
 readSizes={0}; readAt=0; owner=""; error.clear(); gESPSRRunning=true; gSRFeedShouldRun=true;
 srFeedTask(nullptr); assert(!gESPSRRunning && !error.empty() && gSRFeedExited);
 readSizes={4}; readAt=0; owner="sr"; feedResult=0; error.clear(); gESPSRRunning=true; gSRFeedShouldRun=true;
 srFeedTask(nullptr); assert(!gESPSRRunning && !error.empty() && gSRFeedExited);
 gSRFeedBuffer=nullptr;
}
static void testMNLock() {
 assert(!lockMN(10)); // A mutex is insufficient after the model was destroyed.
 gMNModel=reinterpret_cast<void*>(1); gMNData=reinterpret_cast<void*>(2);
 gMNCommandMutex=nullptr; allocFails=true;
 assert(!lockMN(10) && !mnCommandsReady()); // No fail-open mutex path.
 allocFails=false; mutexAvailable=false;
 assert(!lockMN(10)); mutexAvailable=true;
 commandAllocResult=-1;
 assert(!mnCommandsReady() && !gMNCommandsAllocated);
 commandAllocResult=ESP_OK;
 assert(mnCommandsReady() && gMNCommandsAllocated && commandAllocCalls==2);
 assert(mnCommandsReady() && commandAllocCalls==2);
 gMNData=nullptr;
 assert(!lockMN(10) && !mnCommandsReady());
}
static void testModelSelection() {
 gSettings.srModelSource=2; fileLoads=false; partitionPresent=true; gSRError=nullptr;
 assert(initSRModels());
 assert(requestedModelPath=="/ESP-SR Models/srmodels.bin");
 assert(gSRModels==&modelList && !strcmp(gSRModelSource,"partition:model"));
 assert(gSRError.load()==nullptr); // A successful fallback must not abort startup.
 gSettings.srModelSource=1; partitionPresent=false; gSRError=nullptr;
 assert(!initSRModels() && gSRError.load());
 assert(requestedModelPath=="/sd/ESP-SR Models/srmodels.bin");
 fileLoads=true; gSRError=nullptr; assert(initSRModels());
 gSettings.srModelSource=0; assert(!initSRModels());
 partitionPresent=true; modelList.num=0; assert(!initSRModels()); modelList.num=2;
 existingModels=&modelList; assert(!initSRModels()); existingModels=nullptr;
}
static void testDSPSettings() {
 micGain=50; gSettings.microphoneGain=70; prepareSRAudioProcessing();
 assert(micGain==70 && dspResetCalls==1);
 gSettings.microphoneGain=0; prepareSRAudioProcessing(); assert(micGain==0);
 gSettings.microphoneGain=100; prepareSRAudioProcessing(); assert(micGain==100);
 gSettings.microphoneGain=-1; prepareSRAudioProcessing(); assert(micGain==100);
 gSettings.microphoneGain=101; prepareSRAudioProcessing(); assert(micGain==100);
 assert(dspResetCalls==5);
}
static void testVoice() {
 AuthContext auth; auth.transport=SOURCE_SERIAL; auth.user="alice";
 assert(voiceArmFromContextInternal(auth));
 char out[128];
 assert(executeVoiceCommandAsArmedUser("status",out,sizeof(out)));
 assert(executions==0 && gCommandCount==0 && completedEvents==0 && strstr(out,"Queued"));
 runQueued(); assert(executions==1 && gCommandCount==1 && completedEvents==1);
 assert(currentContext==reinterpret_cast<void*>(0x1234));
 // Queue admission and execution are separate; stop/disarm invalidates queued work.
 assert(executeVoiceCommandAsArmedUser("reboot",out,sizeof(out))); voiceDisarmInternal();
 runQueued(); assert(executions==1 && gCommandCount==1);
 assert(voiceArmFromContextInternal(auth));
 assert(executeVoiceCommandAsArmedUser("reboot",out,sizeof(out)));
 assert(voiceArmFromContextInternal(auth)); // same user, new arming generation
 runQueued(); assert(executions==1);
 assert(executeVoiceCommandAsArmedUser("reboot",out,sizeof(out))); ++liveEpoch;
 runQueued(); assert(executions==1); // logout/relogin, same user
 assert(voiceArmFromContextInternal(auth));
 queueAvailable=false; assert(!executeVoiceCommandAsArmedUser("status",out,sizeof(out))); queueAvailable=true;
 allocFails=true; assert(!executeVoiceCommandAsArmedUser("status",out,sizeof(out))); allocFails=false;
 mutexAvailable=false; assert(!executeVoiceCommandAsArmedUser("status",out,sizeof(out))); mutexAvailable=true;
 assert(executeVoiceCommandAsArmedUser("status",out,sizeof(out))); execResult=false;
 runQueued(); assert(executions==2 && gCommandCount==1 && completedEvents==1); execResult=true;
 // Executing a voice stop can finish without waiting on the recognition caller.
 assert(executeVoiceCommandAsArmedUser("closesr",out,sizeof(out)));
 executeHook=[] { voiceDisarmInternal(); };
 runQueued(); executeHook=nullptr;
 assert(executions==3 && gCommandCount==2 && !gVoiceArmed);
 // Stateful sessions must fail closed when epoch capture fails; Basic auth is stateless.
 liveEpoch=0; assert(!voiceArmFromContextInternal(auth));
 auth.transport=SOURCE_WEB; auth.sid=""; assert(voiceArmFromContextInternal(auth));
 assert(executeVoiceCommandAsArmedUser("status",out,sizeof(out))); runQueued(); assert(executions==4);
 auth.transport=SOURCE_SERIAL; auth.user="AuthBypass"; liveEpoch=4; gSettings.serialRequireAuth=false;
 assert(voiceArmFromContextInternal(auth)); assert(executeVoiceCommandAsArmedUser("status",out,sizeof(out)));
 gSettings.serialRequireAuth=true; runQueued(); assert(executions==4);
}
int main() { testTeardown(); testFeed(); testMNLock(); testModelSelection(); testDSPSettings(); testVoice(); puts("ESP-SR lifecycle, short-read PCM and deferred voice authority tests passed"); }
