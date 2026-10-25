// The phone pipe owner's runtime on the one BLE setup worker.
//
// BLE_PhonePipe.cpp's runtime is extracted unchanged (test_phone_runtime.py)
// and runs on the REAL setup worker: BLE_SetupWorker.h/.cpp and
// BLE_SetupWorkerCore.h compile as is against the host stubs below. The test
// drives the worker task (serveWorker() is one wake cycle of its loop) and
// co-simulates the phone_pipe task on a fake clock: whenever the worker sleeps
// on its wake semaphore, or an accessory job runs, the phone ticks every 10 ms
// as run() does, so a phone turn plays out end to end. The worker API is
// wrapped by spies that count and probe, then call the real functions. SDK,
// BLE, account and clock operations are host boundaries; the glasses, ring
// and ring-retirement clients are fakes.
#include "BLE_PhonePipeCore.h"
#include "BLE_PhonePipeTypes.h"
#include "BLE_PhonePairCode.h"  // `phone pair new` (phone link v2)
#include "BLE_CentralCoordinator.h"
#include "BLE_PhonePipeScan.h"
#include "BLE_CoexRecords.h"
#include "System_CommandLimits.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#define EXT_RAM_BSS_ATTR
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
// The build under test: Bluetooth with the glasses and the phone pipe, so the
// worker has all four clients.
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_BLE_PHONE_PIPE 1
#define ENABLE_LLM_BACKEND 0
#define ENABLE_LLM_SOURCE_HTTP 0
using Error=hw1pipe::Error;
using TransportSessionEpoch=uint32_t;
enum CommandSource{SOURCE_SERIAL,SOURCE_LOCAL_DISPLAY,SOURCE_WEB};
constexpr int BLE_PEER_PHONE=2,ESP_OK=0,pdTRUE=1,pdFALSE=0,pdPASS=1,APP_CORE=1;
constexpr uint32_t portMAX_DELAY=0xffffffffu;
constexpr int BLE_SCAN_TYPE_PASSIVE=0,BLE_ADDR_TYPE_PUBLIC=0,BLE_SCAN_FILTER_ALLOW_ALL=0,BLE_SCAN_DUPLICATE_ENABLE=1,BLE_SCAN_DUPLICATE_DISABLE=0;

// ---- Logs (kept for assertions; a hook may act on a line) --------------------
static std::vector<std::string> logs;
static std::function<void(const std::string&)> onLog;
__attribute__((format(printf,2,3))) static void hostLog(const char* tag,const char* format,...) {
 char text[256];va_list args;va_start(args,format);std::vsnprintf(text,sizeof(text),format,args);va_end(args);
 logs.push_back(std::string(tag)+": "+text);
 if(onLog){auto hook=onLog;const std::string line=logs.back();hook(line);}
}
static bool logged(const char* text){for(const auto& l:logs)if(l.find(text)!=std::string::npos)return true;return false;}
#if HW1_PAIR_SINK
#define ESP_LOGI(tag,...) hostLog(tag,__VA_ARGS__)  // test_phone_pair_sink: every log line is a sink
#else
#define ESP_LOGI(...) ((void)0)
#endif
#define ESP_LOGW(tag,...) hostLog(tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) hostLog(tag,__VA_ARGS__)

// ---- FreeRTOS surface ---------------------------------------------------------
using StaticSemaphore_t=int;using SemaphoreHandle_t=void*;using TaskHandle_t=void*;using UBaseType_t=unsigned;
using portMUX_TYPE=int;constexpr int portMUX_INITIALIZER_UNLOCKED=0;
using StaticQueue_t=int;using QueueHandle_t=void*;
// Critical sections are a leaf lock: never nested, never around a callback.
// critEnter is defined after the storage (it probes the route publication).
static int critDepth=0;static unsigned critEntries=0;
static void critEnter(const void* mux);
static void critExit(const void*){assert(critDepth==1);--critDepth;}
#define portENTER_CRITICAL(x) critEnter(x)
#define portEXIT_CRITICAL(x) critExit(x)
#define pdMS_TO_TICKS(x) (x)
// The worker's wake semaphore is the co-simulation's scheduling point
// (defined after the worker source).
static int xSemaphoreTake(SemaphoreHandle_t semaphore,uint32_t ticks);
static int xSemaphoreGive(SemaphoreHandle_t semaphore);
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage){return storage;}
static int phoneTaskHandle=0,workerTaskHandle=0;
static uint32_t workerStackBytes=0;static unsigned workerTaskPriority=0;
template<class F> static int xTaskCreateLogged(F,const char* name,uint32_t stack,void*,unsigned priority,TaskHandle_t* out,const char*,int core) {
 assert(!std::strcmp(name,"ble_setup") && core==APP_CORE);
 workerStackBytes=stack;workerTaskPriority=priority;*out=&workerTaskHandle;return pdPASS;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(){return &phoneTaskHandle;}
static uint32_t clockMs=100;
static uint32_t millis(){return clockMs;}
// A command task's wait loop (phonePipeOpen, quiesce, host stop): each delay
// advances the clock and lets the test play the phone task.
static std::function<void()> playWorker;
static void vTaskDelay(uint32_t ms){clockMs+=ms;if(playWorker)playWorker();}

// ---- The worker's record hooks (BLE_CoexRecords) -------------------------------
static std::vector<std::pair<std::string,uint32_t>> setupWaitLogs;
static std::vector<uint32_t> accessoryWaits;static unsigned accessoryWaitClears=0;
void bleCoexLogSetupWait(const char* job,uint32_t ms){setupWaitLogs.emplace_back(job,ms);}
void bleCoexSetAccessoryWait(uint32_t ms){accessoryWaits.push_back(ms);}
void bleCoexClearAccessoryWait(){++accessoryWaitClears;}

// ---- Arduino String, accounts and peers ----------------------------------------
class String {
 public:
  String(const char* text=""):text_(text){}
  bool operator==(const char* other)const{return text_==other;}
  bool operator!=(const String& other)const{return text_!=other.text_;}
  bool startsWith(const char* prefix)const{return text_.rfind(prefix,0)==0;}
  const char* c_str()const{return text_.c_str();}
  size_t length()const{return text_.size();}
 private: std::string text_;
};
static uint32_t authorityIntent=7,authorityIdentity=9;
static bool appIdFails=false;
static bool authority=true,epochLive=true,accessory=false;
static unsigned persistCount=0,acceptCount=0,registrationCount=0,registrationReleases=0;
static std::vector<std::string> events;
static bool forgetCurrent=true,forgetCommit=true;
static unsigned forgetCommitCount=0;
struct BlePeerSavedTarget{char mac1[18]{};};
struct BlePeerConnectRequest{uint32_t intentGeneration=7,identityGeneration=9;bool autoReconnect=false,explicitReseek=false;BlePeerSavedTarget savedTarget;};
struct BlePhoneForget{uint32_t intent=7,identity=9;BlePeerSavedTarget target;};
static bool blePeerPhonePipeForgetIsCurrent(const BlePhoneForget&){return forgetCurrent;}
static bool blePeerCommitPhonePipeForget(const BlePhoneForget&){++forgetCommitCount;return forgetCommit;}
static bool transportSessionEpochIsLive(CommandSource,uint32_t epoch){return epochLive&&epoch==5;}
static bool blePeerIntentIsCurrent(int,uint32_t intent,uint32_t identity){return authority&&intent==authorityIntent&&identity==authorityIdentity;}
static bool blePeerNoteLinkUpIfCurrent(int,const BlePeerConnectRequest& r,uint32_t* outIntent,uint32_t* outIdentity){
 if(!blePeerIntentIsCurrent(0,r.intentGeneration,r.identityGeneration))return false;
 ++acceptCount;*outIntent=++authorityIntent;*outIdentity=authorityIdentity;return true;
}
static bool blePeerCommitPhonePipeEnrollmentIfCurrent(uint32_t& intent,uint32_t& identity,bool* persist){
 if(!blePeerIntentIsCurrent(0,intent,identity))return false;
 ++acceptCount;intent=++authorityIntent;identity=++authorityIdentity;*persist=true;return true;
}
static void blePeerNoteLinkLost(int){}
static bool requestSettingsPersist(){++persistCount;return true;}
// The one-shot reseek a job dropped unrun gives back (the Waiting backstop).
static unsigned reseekRestores=0;static uint32_t reseekIntent=0,reseekIdentity=0;
static bool blePeerRestoreDroppedReseek(int kind,uint32_t intent,uint32_t identity){
 assert(kind==BLE_PEER_PHONE);++reseekRestores;reseekIntent=intent;reseekIdentity=identity;return true;
}
// Manual learning bumps the intent (the enrollment's fence).
static unsigned manualLearns=0;static bool manualLearnOk=true;
static bool blePeerBeginManualLearn(int,uint32_t& intent,uint32_t& identity){
 ++manualLearns;if(!manualLearnOk)return false;intent=++authorityIntent;identity=authorityIdentity;return true;
}
static bool blePeerBeginManualConnectRequest(int,BlePeerConnectRequest& out){
 out=BlePeerConnectRequest{};out.intentGeneration=++authorityIntent;out.identityGeneration=authorityIdentity;return true;
}
static unsigned userDisconnects=0;
static void blePeerNoteUserDisconnect(int){++userDisconnects;++authorityIntent;}
static void bleStampPairedByIfBlank(int){}
struct BlePeerOwnerSession{String user;uint32_t generation=0;};
static const char* ownerUser="admin";  // the phone's owning account
static bool blePeerOwnerSessionSnapshot(int,BlePeerOwnerSession& out){out.user=ownerUser;out.generation=3;return true;}
static bool blePeerBeginPhonePipeForget(uint32_t,BlePhoneForget& out){out=BlePhoneForget{};return true;}
struct AuthContext{CommandSource transport=SOURCE_SERIAL;String user="admin";};
static AuthContext authContext;static bool execAdmin=true;
static const AuthContext& currentAuthContext(){return authContext;}
static bool currentExecIsAdmin(){return execAdmin;}
static TransportSessionEpoch captureEpoch=5;
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext&){return captureEpoch;}
enum class BlePeerConnectAdmission:uint8_t{STARTED,COALESCED,BUSY,ALREADY_UP,NO_TARGET,ROLE_BLOCKED};
static bool requestCurrent=true;
static bool blePeerConnectRequestIsCurrent(int,const BlePeerConnectRequest&){return requestCurrent;}
// F2: admission must not consult the temples. Counted to prove it.
static unsigned armQueries=0;
static bool g2BothConnected(){++armQueries;return false;}
// The `phone` command surface (start/stop are not under test here). The
// validate pass returns before any handler acts, as RETURN_VALID_IF_VALIDATE_CSTR does.
static bool gCLIValidateOnly=false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do{if(::gCLIValidateOnly)return "VALID";}while(0)
struct Settings{int bleMode=0;bool bleEnabled=true;String phonePipeSecret;};static Settings gSettings;constexpr int BLE_MODE_PHONE_CLIENT=3;
template<class T,class V> static void setSetting(T& field,V value){field=value;}
enum class BleRoleTransition:uint8_t{PHONE_START,PHONE_STOP};
static bool bleRoleTransitionBegin(BleRoleTransition){return true;}
static void bleRoleTransitionEnd(){}
// `phone connect|pair|forget` start a stopped link (startLinkIfStopped).
static bool g2Up=false,serverUp=false,startOk=true;static int pipeStarts=0;
static bool (*onPipeStart)()=nullptr;  // set by reset(): marks the pipe active
static bool isG2ClientInitialized(){return g2Up;}
static bool isBleServerInitialized(){return serverUp;}
// The pipe as a device on the shared client stack: an open on a running pipe is
// a no-op, as production's; the stack itself is always up here.
static bool stackReady=true;static bool bleStackReady(){return stackReady;}
static bool phoneLinkOpen();  // defined after the production splice: it reads the pipe's `active`
static bool halStopped=true;
struct HalStatus{bool down;bool stopped()const{return down;}};
static HalStatus bluetoothHalStatus(){return HalStatus{halStopped};}
struct HalDeinit{bool success=true;};
static HalDeinit bluetoothHalDeinit(){return HalDeinit{};}
static void bleStackSetLifecycleFault(bool){}

// ---- Radio and capacity -----------------------------------------------------------
static bool bleCentralTxAccessoryPressure(){return accessory;}
static bool g2MicAfeFeedIsActive(){return false;}
struct esp_ble_scan_params_t{int scan_type,own_addr_type,scan_filter_policy,scan_interval,scan_window,scan_duplicate;};
struct esp_ble_conn_update_params_t{uint8_t bda[6];int min_int,max_int,latency,timeout;};
static esp_ble_scan_params_t lastScan{};
static int esp_ble_gap_set_scan_params(esp_ble_scan_params_t* p){events.emplace_back("scan-params");lastScan=*p;return ESP_OK;}
static unsigned lastScanSeconds=0;
static int stopResult=ESP_OK;
static int esp_ble_gap_start_scanning(unsigned seconds){events.emplace_back("scan");lastScanSeconds=seconds;return ESP_OK;}
static int esp_ble_gap_stop_scanning(){events.emplace_back("stop-scan");return stopResult;}
static std::vector<int> paramRequests; // min_int of each request
static int paramResult=ESP_OK;
static int esp_ble_gap_update_conn_params(esp_ble_conn_update_params_t* p){paramRequests.push_back(p->min_int);return paramResult;}
static bool capacityAvailable=true,gattStuck=false;
static unsigned capacityReservations=0,capacityReleases=0;
static bool lastReserveArms=false;
static std::function<void()> onLinkRelease;
uint32_t bleCentralLinkReserve(BleCentralLink,bool reserveArms){lastReserveArms=reserveArms;if(!capacityAvailable)return 0;return ++capacityReservations;}
// A three-link C6 with the ring still holding its link (its retirement pending).
static bool ringYield=false,ringLinkHeld=false;
bool bleCentralRingShouldYield(){return ringYield;}
bool bleCentralLinkHeld(BleCentralLink link){return link==BleCentralLink::Ring&&ringLinkHeld;}
// Host teardowns drop the ring's kept client first (not exercised here).
static bool g2RingPrepareForStackTeardown(uint32_t){return true;}
static void bleCentralClientsTerminalTeardownAcknowledged(){}
void bleCentralLinkBind(uint32_t,const void*){}
void bleCentralLinkRelease(uint32_t){}
void bleCentralLinkReleaseClient(const void*){++capacityReleases;if(onLinkRelease){auto hook=onLinkRelease;hook();}}
static bool pipeAdmissionEnabled=false;
void bleCentralPhoneEnabled(bool value){pipeAdmissionEnabled=value;}
// The owner mutex (counted: the console's accessors must never take it). The
// calling task holds it while a Lock is in scope; lockRefused makes a take
// time out.
static unsigned ownerLocks=0;static int ownerDepth=0;static bool lockRefused=false;
class Lock {
 public:
  explicit Lock(uint32_t=50):held(!lockRefused){if(held){++ownerLocks;++ownerDepth;}}
  ~Lock(){if(held)--ownerDepth;}
  explicit operator bool()const{return held;}
 private:bool held;
};
static TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t){return ownerDepth>0?&phoneTaskHandle:nullptr;}
// The GAP callback's SDK surface (scan results, security request, parameters).
enum esp_gap_ble_cb_event_t{ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,
 ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,ESP_GAP_BLE_SCAN_RESULT_EVT,ESP_GAP_BLE_SEC_REQ_EVT,ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT,
 ESP_GAP_BLE_OTHER_EVT};
enum{ESP_GAP_SEARCH_INQ_RES_EVT=0,ESP_GAP_SEARCH_INQ_CMPL_EVT=1,ESP_GAP_SEARCH_OTHER_EVT=9};
struct esp_ble_gap_cb_param_t{
 struct{int status;}scan_param_cmpl,scan_start_cmpl,scan_stop_cmpl;
 struct{int search_evt;uint8_t bda[6];int ble_addr_type;int rssi;uint8_t ble_adv[62];uint8_t adv_data_len;}scan_rst;
 struct{struct{uint8_t bd_addr[6];}ble_req;}ble_security;
 struct{int status;uint8_t bda[6];uint16_t conn_int;}update_conn_params;
};
static unsigned securityRefusals=0;
// The shared coexistence log, as BLE_CoexRecords.cpp keeps it.
static hw1coex::Log coexLog;
static WifiCoexTag wifiNow;
WifiCoexTag bleCoexWifiNow(){return wifiNow;}
void bleCoexRecordScan(const hw1coex::ScanRecord& r){coexLog.addScan(r);}
void bleCoexRecordConnect(const hw1coex::ConnectRecord& r){coexLog.addConnect(r);}
size_t bleCoexScanCounters(char* out,size_t cap,hw1coex::Peer p){return coexLog.scanCounters(out,cap,p);}
size_t bleCoexScanLines(char* out,size_t cap,uint32_t now,hw1coex::Peer only,size_t max){return coexLog.scanLines(out,cap,now,only,max);}
size_t bleCoexConnectCounters(char* out,size_t cap,hw1coex::Peer p){return coexLog.connectCounters(out,cap,p);}
size_t bleCoexConnectLines(char* out,size_t cap,uint32_t now,hw1coex::Peer only,size_t max){return coexLog.connectLines(out,cap,now,only,max);}
static uint32_t phoneConnects(hw1coex::ConnectOutcome o){return coexLog.connects[unsigned(hw1coex::Peer::Phone)][unsigned(o)];}
static const hw1coex::ScanRecord& lastScanRecord(){return coexLog.scan[(coexLog.scanTotal-1)%hw1coex::kKept];}
static const hw1coex::ConnectRecord& lastConnect(){return coexLog.connect[(coexLog.connectTotal-1)%hw1coex::kKept];}
static uint32_t phoneScans(hw1coex::ScanOutcome o){return coexLog.scans[unsigned(hw1coex::Peer::Phone)][unsigned(o)];}
static void esp_ble_gap_security_rsp(uint8_t*,bool accept){assert(!accept);++securityRefusals;}
// Every wipe is noted, so a test can see a stack copy of the key wiped.
static std::vector<const void*> memzeroed;
static void sodium_memzero(void* p,size_t n){memzeroed.push_back(p);std::memset(p,0,n);}
// The pipe key's PBKDF2 (System_BleSecureCrypto.h): a fake derivation of the
// secret, never under the owner mutex. onPbkdf2 runs in its middle, as a
// secret change racing a start does (the real one yields every 4096 rounds).
static int sodium_init(){return 0;}
static unsigned pbkdf2Calls=0;static const uint8_t* pbkdf2Out=nullptr;static std::function<void()> onPbkdf2;
static void fakeKey(const char* secret,size_t n,uint8_t out[32]){for(size_t i=0;i<32;++i)out[i]=uint8_t(secret[i%n]^i^0x5a);}
namespace hw1sc {
static bool scPbkdf2(const uint8_t* pw,size_t pwLen,const uint8_t* salt,size_t saltLen,uint32_t iters,uint8_t out[32]){
 assert(ownerDepth==0 && pw && pwLen && saltLen==11 && !std::memcmp(salt,"HW1-PIPE-v1",11) && iters==100000);
 ++pbkdf2Calls;pbkdf2Out=out;fakeKey(reinterpret_cast<const char*>(pw),pwLen,out);
 if(onPbkdf2){auto hook=onPbkdf2;hook();}
 return true;
}
}
constexpr int AF_INET=2,AF_INET6=10;
static int inet_pton(int af,const char* text,void* out){
 unsigned a,b,c,d;if(af!=AF_INET || std::sscanf(text,"%u.%u.%u.%u",&a,&b,&c,&d)!=4)return 0;
 auto* o=static_cast<uint8_t*>(out);o[0]=uint8_t(a);o[1]=uint8_t(b);o[2]=uint8_t(c);o[3]=uint8_t(d);return 1;}
struct BLEDevice {
 static bool allocateAppId(uint16_t& id){if(appIdFails)return false;id=42;++registrationCount;return true;}
 static void releaseAppId(uint16_t){++registrationReleases;}
};
namespace hw1phone {
inline constexpr char kPipeLabel[]="HW1-PIPE-v1";
struct Gatt {
 enum class Phase{Idle,Registering,Opening,Mtu,Discovering,Subscribe,Cccd,Secure,Pipe,Retiring,Quarantined};
 uint32_t entered[9]{},discovered=0;
 uint32_t enteredAt(Phase p)const{return unsigned(p)<=unsigned(Phase::Pipe)?entered[unsigned(p)]:0;}
 // The attempt-end latch, as in production: the first stop after begin().
 struct AttemptEnd{uint32_t at=0;hw1coex::ConnectStage stage=hw1coex::ConnectStage::None;Error error=Error::None;
   int16_t gatt=-1;uint16_t reason=0;bool serviceChanged=false;};
 AttemptEnd endv;bool armed=false,concluded=false;
 hw1coex::ConnectStage stageNow=hw1coex::ConnectStage::Open;
 const AttemptEnd* attemptEnd()const{return concluded?&endv:nullptr;}
 void latch(uint32_t now,Error e,int16_t gatt=-1,uint16_t reason=0){
   if(armed){armed=false;concluded=true;endv=AttemptEnd{now,stageNow,e,gatt,reason,serviceChangedFlag};}}
 // A failure the adapter finds itself on its next poll (a test arms it).
 bool failNext=false;Error failError=Error::Connect;int16_t failGatt=-1;uint16_t failReason=0;
 uint32_t discoveredAt()const{return discovered;}
 // The production adapter's callback slabs (PSRAM) are the caller's; the fake keeps the shape.
 struct Event{uint8_t data[8];};static constexpr unsigned kSlabCount=17;
 Gatt(hw1pipe::Session& session,Event*):pipe(session){}
 hw1pipe::Session& pipe;Phase state=Phase::Idle;uint32_t gen=1;bool authFailed=false;
 uint16_t conn=2;uint16_t connection()const{return conn;}  // the link's id, set at OPEN
 bool begin(uint16_t,const uint8_t*,uint8_t,uint32_t now,const uint8_t*){
   // As production: a new attempt's phase times start from zero.
   pipe.down();std::memset(entered,0,sizeof(entered));
   events.emplace_back("gatt-begin");state=Phase::Opening;++gen;entered[unsigned(Phase::Registering)]=now;
   endv={};concluded=false;armed=true;stageNow=hw1coex::ConnectStage::Open;return true;}
 bool idle()const{return state==Phase::Idle;}
 Phase phase()const{return state;}
 bool ready()const{return state==Phase::Pipe&&pipe.state==hw1pipe::State::Ready;}
 bool authenticationFailed()const{return authFailed;}
 bool serviceChangedFlag=false;
 bool serviceChanged()const{return serviceChangedFlag;}
 // The pressure arguments are recorded: an accessory job is relay pressure,
 // never a cancel; only the display defers the console.
 bool lastPressure=false,lastConsoleBusy=false;unsigned polls=0;
 // Opt-in (pipeLive): Phase::Pipe drives the real Session as the adapter
 // does. A pending start (the secure channel just finished) goes first, then
 // tick, then at most one frame out per poll, accepted at once and handed to
 // the fake phone (phoneRx). A session that fails stops the adapter.
 bool pipeLive=false,startOnPoll=false;uint16_t mtu=247;std::function<void(const uint8_t*,size_t)> phoneRx;
 void poll(uint32_t now,bool relayPressure,bool consoleDisplayBusy=false){
   ++polls;lastPressure=relayPressure;lastConsoleBusy=consoleDisplayBusy;
   if(failNext){failNext=false;latch(now,failError,failGatt,failReason);pipe.down(failError,now);state=Phase::Retiring;return;}
   if(!pipeLive)return;
   if(startOnPoll) {
     startOnPoll=false;const uint8_t nonce[8]={0xa1,2,3,4,5,6,7,8};
     state=Phase::Pipe;entered[unsigned(Phase::Pipe)]=now;
     if(!pipe.start(mtu,nonce,now)){stop(now,Error::Protocol);return;}
   }
   if(state!=Phase::Pipe)return;
   if(!pipe.tick(now,relayPressure,consoleDisplayBusy)){stop(now,pipe.error);return;}
   uint8_t f[hw1pipe::kValue];const size_t n=pipe.peek(f);
   if(n){assert(pipe.accept(now));if(phoneRx)phoneRx(f,n);}
 }
 unsigned stops=0;uint32_t stopAt=0;Error stopError=Error::None;
 // As production: the timed down, so a link loss can be a version strike.
 void stop(uint32_t now,Error e){++stops;stopAt=now;stopError=e;latch(now,e);pipe.down(e,now);state=gattStuck?Phase::Quarantined:Phase::Idle;}
 // The host-stopped reset (tickLocked's branch), counted; a hook sees the moment.
 unsigned hostStops=0;std::function<void()> onHostStopped;
 void hostStopped(){
   ++hostStops;state=Phase::Idle;pipe.down();authFailed=false;armed=false;failNext=false;
   if(onHostStopped){auto hook=onHostStopped;hook();}}
 uint32_t generation()const{return gen;}
};
}
using hw1phone::Gatt;
// INSERT_WORKER_HEADER
// INSERT_TYPES
// INSERT_STORAGE

// ---- Probes on the production storage ---------------------------------------
// Each route publication notes the turn's `ended` it saw, so a give-back can
// prove the route went out before the end did.
static unsigned routeSeq=0;static uint32_t endedAtRoute=0;
static std::function<void()> onRoute;  // runs just before routeSet takes its lock
static void critEnter(const void* mux) {
 if(mux==&routeMux && onRoute){auto hook=onRoute;hook();}
 assert(critDepth==0);++critDepth;++critEntries;
 if(mux==&routeMux){++routeSeq;endedAtRoute=turn.ended.load();}
}
// phone_pipe's priority, as the worker's TurnEnv sets it.
struct PrioritySet{UBaseType_t priority;uint32_t granted,ended;};
static std::vector<PrioritySet> prioritySets;static UBaseType_t phonePriority=1;
static std::function<void(UBaseType_t)> onPriority;
static void vTaskPrioritySet(TaskHandle_t task,UBaseType_t priority) {
 assert(task==worker && critDepth==0);
 prioritySets.push_back({priority,turn.granted.load(),turn.ended.load()});phonePriority=priority;
 if(onPriority){auto hook=onPriority;hook(priority);}
}
// The one vTaskDelete of this unit. The pipe task's exit, as run() takes it
// (if(leaveIfReleased(false))vTaskDelete(nullptr)): the task itself, off every
// lock, once, from its own pass (phonePass). What the status surfaces read at
// that moment is kept: it must already be the final "stopped" state. The
// setup worker's loop has the same exit (BLE_SetupWorker.cpp workerLeaves),
// but no test here releases the worker, so a call from it is a failure.
struct TaskExit{uint32_t at;unsigned hostStops,pass;PhonePipeLinkSummary summary;const char* status;TaskHandle_t handle;};
static std::vector<TaskExit> taskExits;static bool phoneTaskGone=false,inPhonePass=false;static unsigned phonePasses=0;
static void vTaskDelete(TaskHandle_t task) {
 if(!inPhonePass){assert(!"the setup worker never leaves in this unit");}
 assert(!task && critDepth==0 && ownerDepth==0 && !phoneTaskGone);
 taskExits.push_back({clockMs,phoneGatt.hostStops,phonePasses,linkSummary,status.load(),worker});phoneTaskGone=true;
}
// The recovery block: each call notes what the flags and the turn were.
struct BlockCall{bool value,recoveryNeeded,setupHeld;uint32_t ended;};
static std::vector<BlockCall> blockCalls;static bool recoveryBlocked=false;
static std::function<void()> releaseHook;  // as the turn is given back or the block lifted
static std::function<void()> onBlock;      // one shot, just before a block takes effect
void bleCentralRecoveryBlock(bool value) {
 assert(critDepth==0);
 blockCalls.push_back({value,recoveryNeeded.load(),setupHeld,turn.ended.load()});
 if(value && onBlock){auto hook=onBlock;onBlock=nullptr;hook();}
 if(!value && releaseHook)releaseHook();
 recoveryBlocked=value;
}
bool bleCentralRecoveryBlocked(){assert(critDepth==0);return recoveryBlocked;}
// INSERT_WORKER

// ---- The co-simulation ----------------------------------------------------------
// The worker's sleeps are where the phone task runs: up to `ticks` ms in 10 ms
// phone ticks, ending early when someone gives the wake.
static bool wakeGiven=false,phoneTaskRuns=true;
static void phoneTaskStep();
static void simulate(uint32_t ms);
static int xSemaphoreGive(SemaphoreHandle_t semaphore){assert(semaphore==sWake);wakeGiven=true;return pdTRUE;}
static int xSemaphoreTake(SemaphoreHandle_t semaphore,uint32_t ticks) {
 assert(semaphore==sWake && critDepth==0);
 if(wakeGiven){wakeGiven=false;return pdTRUE;}
 if(ticks==portMAX_DELAY)return pdFALSE;  // an idle worker: only the test wakes it
 for(uint32_t slept=0;slept<ticks;slept+=10) {
  phoneTaskStep();
  if(wakeGiven){wakeGiven=false;return pdTRUE;}
 }
 return pdFALSE;
}
// One wake cycle of the worker task's loop (BLE_SetupWorker.cpp loop()).
static void serveWorker(){Env env;hs::serve(sCore,env);}

// ---- Spies over the real worker API ------------------------------------------------
static bool failPublish=false,inPublish=false;
static unsigned claimCalls=0,publishCalls=0,cancelCalls=0,withdrawCalls=0;
static uint32_t lastCancelToken=0;static BleSetupWithdraw lastWithdraw=BleSetupWithdraw::NotLive;
struct ClaimView{bool job,enrolling;Step step;uint32_t want,intent,jobAt;};
static ClaimView atClaim{};
bool bleSetupClaim(BleSetupClient client,uint32_t* token) {
 if(client==BleSetupClient::Phone){++claimCalls;atClaim={job,enrolling,step,wantToken.load(),intent,jobAt};}
 return realSetupClaim(client,token);
}
bool bleSetupPublish(BleSetupClient client,uint32_t token) {
 if(client==BleSetupClient::Phone){++publishCalls;if(failPublish)return false;}
 inPublish=true;const bool ok=realSetupPublish(client,token);inPublish=false;
 return ok;
}
void bleSetupCancelClaim(BleSetupClient client,uint32_t token) {
 if(client==BleSetupClient::Phone){++cancelCalls;lastCancelToken=token;}
 realSetupCancelClaim(client,token);
}
BleSetupWithdraw bleSetupWithdraw(BleSetupClient client,uint32_t token) {
 const BleSetupWithdraw result=realSetupWithdraw(client,token);
 if(client==BleSetupClient::Phone){++withdrawCalls;lastWithdraw=result;}
 return result;
}
// Every wake that is not a publish's is the phone giving its turn back. At that
// moment the route is already current, and was published before `ended`.
struct WakeCall{uint32_t at,ended,granted;bool blocked,routeEnabled,routeScan;};
static std::vector<WakeCall> phoneWakes;static uint32_t endedSeen=0;
void bleSetupWake() {
 if(!inPublish) {
  const uint32_t ended=turn.ended.load();
  phoneWakes.push_back({clockMs,ended,turn.granted.load(),recoveryBlocked,route.enabled,route.scan});
  assert(route.scan==(scanParamsPending || step==Step::Scanning || scanStartPending || scanStarted || scanStopPending));
  if(ended!=endedSeen){assert(endedAtRoute!=ended);endedSeen=ended;}
  if(releaseHook)releaseHook();
 }
 realSetupWake();
}

// ---- Fake accessory clients ----------------------------------------------------------
// Each runs for runMs of fake time, during which the phone keeps ticking.
struct FakeClient{
 hw1setup::Verdict verdict{hw1setup::Ready::Run,hw1setup::kNone};
 uint32_t runMs=0,ranAt=0;unsigned runs=0,finishes=0;hw1setup::End lastEnd=hw1setup::End::Ran;
};
static FakeClient fakes[3];
template<int I> static const char* fakeLabel(uint32_t){static const char* names[]={"glasses","ring-retire","ring"};return names[I];}
template<int I> static hw1setup::Verdict fakeReady(uint32_t,const hw1setup::WaitInfo&){assert(critDepth==0);return fakes[I].verdict;}
template<int I> static hw1setup::End fakeRun(uint32_t,const hw1setup::RunInfo&){
 FakeClient& f=fakes[I];++f.runs;f.ranAt=clockMs;simulate(f.runMs);return hw1setup::End::Ran;}
template<int I> static void fakeFinish(uint32_t,const hw1setup::FinishInfo& info){++fakes[I].finishes;fakes[I].lastEnd=info.end;}
const BleSetupOps g2SetupOps={fakeLabel<0>,fakeReady<0>,nullptr,fakeRun<0>,fakeFinish<0>,true};
const BleSetupOps g2RingRetireSetupOps={fakeLabel<1>,fakeReady<1>,nullptr,fakeRun<1>,fakeFinish<1>,false};
const BleSetupOps g2RingSetupOps={fakeLabel<2>,fakeReady<2>,nullptr,fakeRun<2>,fakeFinish<2>,true};

// The GAP queue: 8 slots, as in production.
static std::vector<GapEvent> gapEvents;
static int xQueueSend(QueueHandle_t,const GapEvent* e,uint32_t){if(gapEvents.size()>=8)return 0;gapEvents.push_back(*e);return pdTRUE;}
static int xQueueReceive(QueueHandle_t,GapEvent* e,uint32_t){if(gapEvents.empty())return 0;*e=gapEvents.front();gapEvents.erase(gapEvents.begin());return pdTRUE;}
void phonePipeClose(PhonePipeStream handle,bool cancelled);
// ---- The console driver (BLE_PhonePipeConsole.cpp) and the BLE row state ----
// Spies on the owner's calls: under which lock they run and what they see.
static uint8_t consoleFlags=0;  // what pipeConsoleLocalFlags() answers
struct ConsoleTick{uint32_t generation,now;bool ready,busy;hw1pipe::State session;};
static std::vector<ConsoleTick> consoleTicks;static unsigned afterUnlocks=0;
static PipeConsoleStatus consoleStatus;
// Counted: only a running pipe's pass may read it (it reads the phone secret).
static unsigned localFlagReads=0;
static uint8_t pipeConsoleLocalFlags(){assert(ownerDepth>0);++localFlagReads;return consoleFlags;}
static void pipeConsoleTickLocked(hw1pipe::Session& session,uint32_t generation,bool ready,uint32_t now,bool busy){
 assert(ownerDepth>0 && critDepth==0 && &session==&phoneSession);
 consoleTicks.push_back({generation,now,ready,busy,session.state});
}
static void pipeConsoleAfterUnlock(){assert(ownerDepth==0 && critDepth==0);++afterUnlocks;}
static void pipeConsoleStatus(PipeConsoleStatus* out){assert(ownerDepth==0);*out=consoleStatus;}
// The pipe's hold on gBLEState takes the lifecycle lock: never under the owner mutex.
static unsigned stateReleases=0;
static void blePipeConsoleStateRelease(){assert(ownerDepth==0 && critDepth==0);++stateReleases;}
static unsigned reseeks=0;
static void blePeerRequestReseek(int kind){assert(kind==BLE_PEER_PHONE && ownerDepth==0);++reseeks;}
// ---- `phone pair new` (phone link v2): randomness, the secret steps, the code's sinks ----
// esp_random() returns these in turn: the code they make is known to the test.
static uint32_t randomWords[3]={0x1d2c3b4au,0x5f6e7d8cu,0x13579bdfu};static unsigned randomCalls=0;
static uint32_t esp_random(){return randomWords[randomCalls++%3];}
// The `phone secret` steps (Bluetooth.cpp, run by test_phone_secret): the real
// stop and key invalidation, then the store, recorded. replaceFail makes the
// store step fail, before (false) or after (true) the secret changed.
bool phonePipeStop();bool phonePipeInvalidateSecret();
struct ReplaceCall{std::string value;bool generated;PhonePairWindow windowAfterStop;};
static std::vector<ReplaceCall> replaceCalls;static const char* replaceFail=nullptr;static bool replaceFailAfter=false;
static std::string storedSecret;static bool storedGenerated=false;
static const char* phonePipeReplaceSecret(const char* value,size_t len,bool generated,bool* replaced){
 assert(ownerDepth==0 && critDepth==0);  // it stops the pipe: never under the owner mutex
 if(replaced)*replaced=false;
 if(!phonePipeStop())return "Error: phone pipe cleanup incomplete; secret unchanged";
 replaceCalls.push_back({std::string(value,len),generated,pairState.load()});
 if(!phonePipeInvalidateSecret())return "Error: pipe key invalidation busy; retry before changing secret";
 if(replaceFail && !replaceFailAfter)return replaceFail;
 storedSecret.assign(value,len);storedGenerated=generated;if(replaced)*replaced=true;
 return replaceFail;
}
#if HW1_PAIR_SINK
// test_phone_pair_sink.py inserts HardwareOne.cpp's one-shot serial sink and
// OLED_Utils.cpp's code-screen sink, as shipped, over a recording Serial, the
// command context they read and recording stand-ins for every output path.
// INSERT_SERIAL_SINK
#else
static bool serialSinkReady=true,serialWriteOk=true;static std::vector<std::string> serialWrites;
static bool serialCommandSecretSinkReady(){return serialSinkReady;}
static bool serialWriteCommandSecret(const char* text,size_t len){
 assert(ownerDepth==0 && critDepth==0);serialWrites.emplace_back(text,len);return serialWriteOk;}
// The OLED's sink: the code screen (OLED_Mode_Bluetooth.cpp reads the window;
// test_oled_bluetooth_link_watch runs it).
static bool displaySinkReady=true,displayShowOk=true;static unsigned displayShows=0;
static bool oledPhonePairCodeSinkReady(){return displaySinkReady;}
static bool oledShowPhonePairCode(){assert(ownerDepth==0);++displayShows;return displayShowOk;}
#endif
// INSERT_PRODUCTION

// ---- Test helpers -----------------------------------------------------------------------
using S=hw1coex::ConnectStage;using O=hw1coex::ConnectOutcome;using D=hw1coex::CauseDetail;
constexpr BleSetupClient kPhone=BleSetupClient::Phone;
constexpr uint8_t kPhoneMask=hw1setup::clientMask(hw1setup::Client::Phone);
static int queueHandle=0;
// S4, after every pass: a valid record means READY, on this session's link;
// an invalid one reads all zero.
static void checkLinkRecord(){
 PhonePipeLinkRecord r{0xdead,0xbeef};const bool valid=phonePipeLinkRecord(&r);
 assert(valid==linkValid);
 if(!valid){assert(!r.generation&&!r.connId);return;}
 assert(ready&&step==Step::Link&&r.generation==phoneGatt.generation()&&r.connId==phoneGatt.connection());
}
// As run() does: one pass under the owner mutex, then the records go to the log.
// The console's row work runs after the mutex is released.
static void tickFlush(uint32_t now){{Lock lock;passLocked(now);}pipeConsoleAfterUnlock();flushRecords();checkLinkRecord();}
// As run() does, exit included: the pass, the row work, the records, the idle
// clock, then the leave check. A task that left never runs again (playWorker
// may still be set).
static uint32_t idleSince=0;
static void phonePass(){
 if(phoneTaskGone)return;
 ++phonePasses;tickFlush(clockMs);
 if(!idleOnHost.load())idleSince=0;else if(!idleSince)idleSince=clockMs?clockMs:1;
 const bool idleRetire=idleSince && hw1pipe::expired(clockMs,idleSince,kPipeIdleRetireMs) &&
                       bleSetupIdle(hw1setup::clientMask(hw1setup::Client::Phone));
 inPhonePass=true;if(leaveIfReleased(idleRetire))vTaskDelete(nullptr);inPhonePass=false;
}
// A pipe advertisement (flags + the 128-bit service UUID) or another device's.
static esp_ble_gap_cb_param_t report(bool pipe,int rssi,int type=1){
 esp_ble_gap_cb_param_t p{};p.scan_rst.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT;p.scan_rst.rssi=rssi;p.scan_rst.ble_addr_type=type;
 static constexpr uint8_t uuid[16]={0xa0,0xc6,0x21,0x50,0x9f,0x3d,0xc4,0xa8,0x2e,0x4b,0x7d,0x6f,0,0,0xe1,0xb7};
 const uint8_t head[]={2,0x01,0x06,17,0x07};std::memcpy(p.scan_rst.ble_adv,head,5);
 std::memcpy(p.scan_rst.ble_adv+5,uuid,16);if(!pipe)p.scan_rst.ble_adv[5]^=0xff;
 p.scan_rst.adv_data_len=21;p.scan_rst.bda[0]=pipe?0xc2:0x11;return p;
}
static void scanEvent(esp_gap_ble_cb_event_t kind,int search=ESP_GAP_SEARCH_INQ_CMPL_EVT,int status=0,bool routed=true){
 esp_ble_gap_cb_param_t p{};p.scan_rst.search_evt=search;
 p.scan_param_cmpl.status=p.scan_start_cmpl.status=p.scan_stop_cmpl.status=status;assert(gapCallback(kind,&p)==routed);
}
// The radio around the phone during a co-simulated turn: the controller answers
// the scan steps, the phone advertises, and the link reaches READY after
// linkMs (never by default).
struct World{bool on=false,advertise=true;uint32_t linkMs=UINT32_MAX;S stage=S::Discovery;};
static World world;
static void worldStep(){
 if(!world.on)return;
 if(scanParamsPending)scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);
 else if(scanStartPending)scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT);
 else if(scanStopPending)scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);
 else if(scanStarted && !candidate && world.advertise){auto p=report(true,-60);assert(gapCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&p));}
 if(step==Step::Link && phoneGatt.state==Gatt::Phase::Opening) {
  phoneGatt.stageNow=world.stage;
  if(world.linkMs!=UINT32_MAX && clockMs-phoneGatt.entered[unsigned(Gatt::Phase::Registering)]>=world.linkMs)
   {phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;}
 }
}
// One 10 ms slice of the phone_pipe task (unless it is stalled), then the
// test's hook. Bounded: a hang is a failure.
static std::function<void()> onStep;
static uint64_t simSteps=0;
static void phoneTaskStep(){
 clockMs+=10;
 if(++simSteps>3000000){std::fprintf(stderr,"co-simulation budget exceeded\n");std::abort();}
 if(phoneTaskRuns){worldStep();tickFlush(clockMs);}
 if(onStep){auto hook=onStep;hook();}
}
static void simulate(uint32_t ms){for(uint32_t t=10;t<=ms;t+=10)phoneTaskStep();clockMs+=ms%10;}
static bool statusIs(const char* text){return !std::strcmp(status.load(),text);}
static bool phoneSlotIdle(){return bleSetupIdle(kPhoneMask);}
static hw1setup::State phoneSlotState(){return sCore.peek(hw1setup::Client::Phone).state;}
static uint32_t admitPhone(const BlePeerConnectRequest& wanted=BlePeerConnectRequest{}){
 assert(admit(wanted)==BlePeerConnectAdmission::STARTED);return wantToken.load();}
static uint32_t enrollPhone(){
 const std::string reply=phonePairCommand("");assert(reply.rfind("Pairing opened for 60 seconds",0)==0);return wantToken.load();}
static uint32_t submitAccessory(int i){
 static const BleSetupClient clients[]={BleSetupClient::G2,BleSetupClient::RingRetire,BleSetupClient::Ring};
 uint32_t t=0;assert(bleSetupClaim(clients[i],&t)&&bleSetupPublish(clients[i],t));return t;}
// A turn the phone holds without a worker behind it (owner-only cases): the
// consumed grant is in place, so the overrun check stays quiet.
static uint32_t fakeToken=1000;
static uint32_t holdTurn(){
 const uint32_t t=++fakeToken;wantToken=t;setupHeld=true;heldToken=t;grantAt=clockMs;turn.granted=t|hw1setup::kUsed;return t;}
static size_t countEvents(const char* name){return size_t(std::count(events.begin(),events.end(),std::string(name)));}
// The phone task stalls (its owner mutex held elsewhere) once its link began.
static void stallOnceLinked(){onStep=[]{if(phoneTaskRuns&&step==Step::Link&&phoneGatt.state==Gatt::Phase::Opening)phoneTaskRuns=false;};}
// ---- The fake phone: its half of the inner protocol v2 ------------------------------
// Plaintext frames (the secure channel is the adapter's). It answers HELLO with
// HELLO_ACK carrying phoneFlags (unless silent, as a 2.7.x app is), answers
// PINGs and notes what it saw.
static uint32_t phoneSeq=0;static uint8_t phoneFlags=0;static bool phoneSilent=false;
static std::vector<uint32_t> pingTimes;static std::vector<uint8_t> helloFlags,linkStates;
// One frame to the P4. A frame the session refuses stops the adapter, as its
// NOTIFY path does.
static bool phoneDeliver(uint8_t version,hw1pipe::Op op,uint16_t id,const uint8_t* body,size_t n){
 uint8_t f[hw1pipe::kValue]{};f[0]=version;f[1]=uint8_t(op);hw1pipe::put16(f+2,id);hw1pipe::put32(f+4,phoneSeq++);
 hw1pipe::put16(f+8,uint16_t(n));f[10]=hw1pipe::kFirst|hw1pipe::kLast;if(n)std::memcpy(f+12,body,n);
 const bool ok=phoneSession.receive(f,12+n,clockMs);
 if(!ok)phoneGatt.stop(clockMs,phoneSession.error);
 return ok;
}
static bool phoneLinkState(uint8_t flags){phoneFlags=flags;const uint8_t b[4]={flags,0,0,0};return phoneDeliver(hw1pipe::kVersion,hw1pipe::Op::LinkState,0,b,4);}
static void phoneAnswer(const uint8_t* f,size_t){
 using hw1pipe::Op;
 if(Op(f[1])==Op::Hello) {
  helloFlags.push_back(f[12+9]);
  if(phoneSilent)return;
  uint8_t b[20]{};std::memcpy(b,f+12,8);for(int i=8;i<16;++i)b[i]=uint8_t(0x40+i);
  b[16]=hw1pipe::kMaxStreams;b[17]=phoneFlags;hw1pipe::put16(b+18,hw1pipe::get16(f+12+10));
  phoneDeliver(hw1pipe::kVersion,Op::HelloAck,0,b,20);
 }
 if(Op(f[1])==Op::Ping){pingTimes.push_back(clockMs);phoneDeliver(hw1pipe::kVersion,Op::Pong,0,f+12,4);}
 if(Op(f[1])==Op::LinkState)linkStates.push_back(f[12]);
}
// The adapter finished the secure channel: the next poll starts the session
// (with the flags the owner set) and sends its HELLO.
static void enterPipe(uint16_t mtu=247){
 phoneGatt.pipeLive=true;phoneGatt.startOnPoll=true;phoneGatt.mtu=mtu;phoneGatt.phoneRx=phoneAnswer;phoneSeq=0;
}
static bool phoneLinkOpen(){if(active.load())return true;++pipeStarts;return startOk&&(!onPipeStart||onPipeStart());}

static void reset() {
 // The worker: a fresh scheduler, already started.
 sCore=hw1setup::Core{};sInit=Init::Ready;wakeGiven=false;phoneTaskRuns=true;
 for(auto& f:fakes)f=FakeClient{};
 onStep=nullptr;onRoute=nullptr;onPriority=nullptr;onBlock=nullptr;onLog=nullptr;onLinkRelease=nullptr;
 releaseHook=nullptr;playWorker=nullptr;world=World{};
 // The phone's turn and the probes.
 turn.granted=0;turn.ended=0;wantToken=0;setupHeld=false;leaveWanted=false;heldToken=0;grantAt=0;worker=&phoneTaskHandle;
 exitWanted=false;phoneTaskGone=inPhonePass=false;taskExits.clear();phonePasses=0;idleSince=0;idleOnHost=false;
 cleanupFaulted=false;ringYield=ringLinkHeld=false;
 phonePriority=1;prioritySets.clear();blockCalls.clear();phoneWakes.clear();endedSeen=0;routeSeq=0;endedAtRoute=0;
 failPublish=inPublish=false;claimCalls=publishCalls=cancelCalls=withdrawCalls=0;lastCancelToken=0;
 lastWithdraw=BleSetupWithdraw::NotLive;atClaim={};
 reseekRestores=0;reseekIntent=reseekIdentity=0;manualLearns=0;manualLearnOk=true;userDisconnects=0;
 execAdmin=true;authContext=AuthContext{};halStopped=true;
 g2Up=serverUp=false;startOk=true;stackReady=true;pipeStarts=0;
 onPipeStart=[]()->bool{active=true;return true;};
 logs.clear();setupWaitLogs.clear();accessoryWaits.clear();accessoryWaitClears=0;
 // The owner.
 pipeAdmissionEnabled=true;recoveryNeeded=quiesceWanted=quiesced=false;recoveryBlocked=gattStuck=false;
 capacityAvailable=true;capacityReservations=capacityReleases=0;registrationCount=registrationReleases=0;
 forgetWanted=false;forgetRequest={};forgetEpoch=5;forgetCurrent=forgetCommit=true;forgetCommitCount=0;
 phoneGatt.onHostStopped=nullptr;phoneGatt.hostStopped();for(auto& stream:phoneGatt.pipe.streams)stream.reset();phoneGatt.hostStops=0;
 active=true;ready=false;stopWanted=false;gapOverflow=false;abortLink=false;hostResetWanted=false;
 step=Step::Idle;status="disconnected";job=enrolling=false;scanParamsPending=false;
 scanStarted=scanStartPending=scanStopPending=candidate=published=authFailureSeen=false;params={};paramRequests.clear();paramResult=ESP_OK;
 jobAt=at=lastStreamAt=0;for(auto& id:releaseIds)id=0;request={};
 authorityIntent=intent=7;authorityIdentity=identityGeneration=9;authority=epochLive=true;
 sessionStats={};lastSetup={};std::memset(phoneGatt.entered,0,sizeof(phoneGatt.entered));phoneGatt.discovered=0;
 accessory=false;requestCurrent=true;armQueries=0;lastReserveArms=false;enrollmentEpoch=5;clockMs=100;persistCount=acceptCount=0;
 events.clear();route={};pskValid=true;haveAuthFailure=false;lastAuthFailure=0;nativeAppId=0xffff;
 scanProfile=hw1scan::kDefault;scanUsed={};scanTally.reset();coexLog=hw1coex::Log{};scanOpen=false;scanBeganAt=0;
 lastScanDurationMs=0;scanWifi={};connectWifi={};pendingScan={};pendingConnect={};scanRecordPending=connectRecordPending=false;
 linkBegun=attemptWanted=false;attemptStop={};gateWaitMs=0;appIdFails=false;
 phoneGatt.endv={};phoneGatt.concluded=false;phoneGatt.serviceChangedFlag=false;phoneGatt.failNext=false;phoneGatt.authFailed=false;
 phoneGatt.failError=Error::Connect;phoneGatt.failGatt=-1;phoneGatt.failReason=0;phoneGatt.stageNow=hw1coex::ConnectStage::Open;
 phoneGatt.lastPressure=false;phoneGatt.polls=0;phoneGatt.stops=0;phoneGatt.stopAt=0;phoneGatt.stopError=Error::None;
 gapEvents.clear();gapQueue=&queueHandle;securityRefusals=0;lastScan={};lastScanSeconds=0;stopResult=ESP_OK;
 scanStopRefused=false;wifiNow=WifiCoexTag{};wifiNow.state=WifiCoex::Associated;
 linkRecord={};linkValid=false;consoleActivity=0;consoleSeen=0;phoneGatt.conn=2;
 // Phone link v2: a fresh Session (its HELLO latch and flags too), relay,
 // the version hold, the gBLEState hold, the snapshot and the console spies.
 phoneSession=hw1pipe::Session{};relayOn=false;versionStrikes=0;versionLatched=false;stateHeld=false;linkSummary={};
 consoleFlags=0;consoleTicks.clear();afterUnlocks=0;consoleStatus={};stateReleases=0;reseeks=0;lockRefused=false;
 localFlagReads=0;memzeroed.clear();pbkdf2Calls=0;pbkdf2Out=nullptr;onPbkdf2=nullptr;pskInvalidations=0;
 phoneGatt.pipeLive=phoneGatt.startOnPoll=false;phoneGatt.mtu=247;phoneGatt.phoneRx=nullptr;phoneGatt.lastConsoleBusy=false;
 phoneSeq=0;phoneFlags=0;phoneSilent=false;pingTimes.clear();helloFlags.clear();linkStates.clear();
 // `phone pair new`: no window, the sinks ready, the known random words.
 pairState=PhonePairWindow::None;pairRetryAt=0;pairJobSeen=false;pairEpoch=0;pairSource=SOURCE_SERIAL;
 pairId=pairOpenedAt=0;pairOnDisplay=false;std::memset(pairCode,0,sizeof(pairCode));pairCloseWanted=0;
 replaceCalls.clear();replaceFail=nullptr;replaceFailAfter=false;storedSecret.clear();storedGenerated=false;randomCalls=0;
 ownerUser="admin";captureEpoch=5;gSettings.bleEnabled=true;gSettings.phonePipeSecret="";
#if !HW1_PAIR_SINK
 serialSinkReady=serialWriteOk=true;serialWrites.clear();displaySinkReady=displayShowOk=true;displayShows=0;
#endif
}
static void drainGap(){GapEvent e;while(xQueueReceive(gapQueue,&e,0)==pdTRUE)consume(e,clockMs);flushRecords();}
// A job's scan up to its start: parameters, then the start.
static void startScan(){
 job=true;beginJob(clockMs);
 scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);drainGap();
 scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT);drainGap();
}
static void reports(unsigned pipeReports,unsigned others,int firstType=1){
 for(unsigned i=0;i<others;++i){auto p=report(false,-90);assert(gapCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&p));}
 for(unsigned i=0;i<pipeReports;++i){clockMs+=i?0:30;auto p=report(true,-70+int(i%7),i?1:firstType);assert(gapCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&p));}
}
// One whole scan whose reports all queue before its timer ends it (the
// early stop and the timer cross).
static void runScan(unsigned pipeReports,unsigned others,int firstType=1){
 startScan();const uint32_t began=clockMs;reports(pipeReports,others,firstType);
 clockMs=began+scanUsed.seconds*1000u;scanEvent(ESP_GAP_BLE_SCAN_RESULT_EVT);  // the scan's own timer
}
static void pendingEnrollment(){job=enrolling=candidate=true;holdTurn();step=Step::Link;phoneGatt.state=Gatt::Phase::Secure;}
// The next client may scan as soon as the turn is given back or the block
// lifted: by then the phone's interceptor must no longer claim scan events.
static void assertScanRouteClear(){assert(!route.scan);}

// ---- The owner on its own: step machine, admission refusals, parameters, counters ----
static void ownerTests() {
 assert(&phoneGatt.pipe==&phoneSession && phoneSession.value==20);
 assert(phoneSession.state==hw1pipe::State::Down);
 {reset();stopWanted=true;tickFlush(clockMs);assert(!active&&!pipeAdmissionEnabled);}
 // Rotating candidates: every usable address type starts the link.
 for(unsigned type=0;type<4;++type){reset();job=enrolling=true;holdTurn();beginJob(clockMs);
  assert(step==Step::ScanParams&&events.size()==1);GapEvent e;
  e.kind=GapKind::ScanParams;consume(e,clockMs);e.kind=GapKind::ScanStart;consume(e,clockMs);assert(scanStarted);
  e.kind=GapKind::Candidate;e.type=type;e.address[0]=0x40;e.address[5]=type;
  consume(e,clockMs);assert(candidate&&!ready&&scanStopPending);e.kind=GapKind::ScanStopped;consume(e,clockMs);
  assert(registrationCount==1&&events.back()=="gatt-begin"&&!ready);}
 // Key enrollment commits once at READY, which gives the turn back (once).
 {reset();pendingEnrollment();const uint32_t t=heldToken;tickFlush(clockMs);assert(!ready&&acceptCount==0&&persistCount==0&&setupHeld);
  {using P=Gatt::Phase;const uint32_t base=clockMs;  // the setup breakdown (firmware-007)
   const std::pair<P,uint32_t> marks[]={{P::Registering,0},{P::Opening,40},{P::Mtu,900},{P::Discovering,5300},
     {P::Subscribe,5450},{P::Cccd,5500},{P::Secure,5600},{P::Pipe,6100}};
   for(auto [p,ms]:marks)phoneGatt.entered[unsigned(p)]=base-7000+ms;
   phoneGatt.discovered=base-7000+5000;lastScanDurationMs=180;}
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);
  assert(ready&&!enrolling&&!setupHeld&&acceptCount==1&&persistCount==1);
  assert(turn.ended==t&&phoneWakes.size()==1&&wantToken==0&&heldToken==0);
  assert(lastSetup.valid && lastSetup.scan==180 && lastSetup.connect==900 && lastSetup.discovery==4100);
  assert(lastSetup.mtu==300 && lastSetup.search==150 && lastSetup.subscribe==150 && lastSetup.secure==500);
  assert(lastSetup.hello==900 && lastSetup.total==7000);
  tickFlush(++clockMs);assert(acceptCount==1&&persistCount==1&&phoneWakes.size()==1);}
 // A crossed early stop still unanswered at READY: the turn goes back only
 // once that answer arrives, so the route no longer claims scan events.
 {reset();pendingEnrollment();const uint32_t t=heldToken;scanStopPending=true;
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);
  assert(ready&&setupHeld&&leaveWanted&&turn.ended!=t&&phoneWakes.empty());
  tickFlush(clockMs+=10);assert(setupHeld&&phoneWakes.empty());
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);tickFlush(clockMs+=10);
  assert(!setupHeld&&!leaveWanted&&turn.ended==t&&phoneWakes.size()==1&&!route.scan);}
 for(unsigned revoked=0;revoked<3;++revoked){reset();pendingEnrollment();
  if(revoked==0)epochLive=false;if(revoked==1)authority=false;if(revoked==2)++authorityIntent;
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);
  assert(!ready&&acceptCount==0&&persistCount==0);}
 {reset();clockMs=1;assert(!authCoolingDown(clockMs));
  pendingEnrollment();phoneGatt.authFailed=false;phoneGatt.state=Gatt::Phase::Retiring;tickFlush(clockMs);
  assert(!haveAuthFailure&&!authCoolingDown(clockMs));}
 {reset();pendingEnrollment();phoneGatt.authFailed=true;phoneGatt.state=Gatt::Phase::Retiring;tickFlush(clockMs);
  assert(haveAuthFailure&&authCoolingDown(clockMs));const auto failed=lastAuthFailure;
  clockMs=failed+29999;assert(authCoolingDown(clockMs));clockMs=failed+30000;assert(!authCoolingDown(clockMs));
  haveAuthFailure=true;lastAuthFailure=0xfffffff0u;assert(authCoolingDown(20));assert(!authCoolingDown(30000));}
 {reset();job=enrolling=true;holdTurn();haveAuthFailure=true;lastAuthFailure=clockMs;beginJob(clockMs);
  assert(events.empty()&&registrationCount==0&&step==Step::Retiring);}
 {reset();forgetWanted=true;startForget(clockMs);assert(!forgetWanted&&forgetCommitCount==1&&persistCount==1);}
 {reset();forgetWanted=true;epochLive=false;startForget(clockMs);assert(!forgetWanted&&!forgetCommitCount&&!persistCount);}
 // A late cleanup after the bound: the turn went back at the bound; the
 // recovery is settled once the adapter finally idles.
 {reset();pendingEnrollment();const uint32_t t=heldToken;gattStuck=true;fail(clockMs,Error::Deadline,"late cleanup");clockMs+=4000;tickFlush(clockMs);
  assert(recoveryNeeded&&!setupHeld&&turn.ended==t&&phoneWakes.size()==1);phoneGatt.state=Gatt::Phase::Idle;tickFlush(++clockMs);
  assert(!recoveryNeeded&&!recoveryBlocked&&capacityReleases==1&&phoneWakes.size()==1&&withdrawCalls==0);}
 // Admission refusals (none consults the temples, none claims a slot).
 // A closed pipe: with the client stack down there is nothing to open on;
 // with it up the admission opens the pipe itself (boot, `openble`) and goes on.
 {reset();BlePeerConnectRequest wanted;active=false;stackReady=false;
  assert(admit(wanted)==BlePeerConnectAdmission::ROLE_BLOCKED && !active && pipeStarts==0);
  stackReady=true;assert(admit(wanted)==BlePeerConnectAdmission::STARTED && active && pipeStarts==1 && job);
  reset();stopWanted=true;assert(admit(wanted)==BlePeerConnectAdmission::ROLE_BLOCKED);
  reset();ready=true;assert(admit(wanted)==BlePeerConnectAdmission::ALREADY_UP);
  reset();haveAuthFailure=true;lastAuthFailure=clockMs;assert(admit(wanted)==BlePeerConnectAdmission::BUSY);
  reset();forgetWanted=true;assert(admit(wanted)==BlePeerConnectAdmission::BUSY);
  reset();requestCurrent=false;assert(admit(wanted)==BlePeerConnectAdmission::NO_TARGET&&!job);
  assert(claimCalls==0&&phoneSlotIdle());
  reset();assert(admit(wanted)==BlePeerConnectAdmission::STARTED);assert(admit(wanted)==BlePeerConnectAdmission::BUSY);
  assert(claimCalls==1&&!armQueries);}
 // A-4: per-connection parameters. A reconnect starts idle and requests idle
 // parameters at once, whatever the previous connection left behind.
 {
  const auto readyNow=[&]{reset();pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;phoneGatt.state=Gatt::Phase::Pipe;
   phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);assert(ready);};
  const auto paramsEvent=[&](int status,uint16_t interval){GapEvent e;e.kind=GapKind::Params;e.status=status;e.interval=interval;consume(e,clockMs);};
  {readyNow();assert((paramRequests==std::vector<int>{72}) && params.pending);
   paramsEvent(0,80);assert(params.applied && !params.active && params.interval==80);
   for(unsigned i=0;i<20;++i)tickFlush(clockMs+=1000);assert(paramRequests.size()==1);   // settled: no more
   // Previous connection's state cannot suppress the next one's request.
   // A new connection starts idle: its activity stamp sits exactly one hold back.
   job=true;beginJob(clockMs);assert(!params.applied && !params.requested &&
     hw1pipe::expired(clockMs,lastStreamAt,kParamsActiveHoldMs) && !hw1pipe::expired(clockMs,lastStreamAt,kParamsActiveHoldMs+1));}
  {readyNow();// unanswered: no second request until the answer backstop (45 s), three tries per wanted state
   for(unsigned i=0;i<449;++i)tickFlush(clockMs+=100);assert(paramRequests.size()==1 && params.pending);
   tickFlush(clockMs+=100);assert(paramRequests.size()==2);
   for(unsigned i=0;i<2000;++i)tickFlush(clockMs+=100);assert(paramRequests.size()==3 && !params.applied);}
  {readyNow();paramsEvent(0x13,0);assert(!params.applied && !params.pending);  // refused
   tickFlush(clockMs+=4999);assert(paramRequests.size()==1);tickFlush(clockMs+=1);assert(paramRequests.size()==2);
   paramsEvent(0,72);assert(params.applied);}
  {reset();paramResult=-1;pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
   tickFlush(clockMs);assert(paramRequests.size()==1 && !params.pending);   // API refused: not applied
   tickFlush(clockMs+=5000);assert(paramRequests.size()==2);}
  {readyNow();paramsEvent(0,80);  // a live stream asks for active, then idle again after 30 s quiet
   uint8_t host[4]={192,0,2,1};const auto id=phoneSession.open(1,host,4,443,clockMs);assert(id);
   tickFlush(clockMs+=1000);assert(paramRequests.size()==1);   // 5 s since the last request
   tickFlush(clockMs+=4000);assert((paramRequests==std::vector<int>{72,24}));paramsEvent(0,30);assert(params.active);
   phoneSession.down();phoneSession.release(id);
   tickFlush(clockMs+=29999);assert(paramRequests.size()==2);tickFlush(clockMs+=1);
   assert((paramRequests==std::vector<int>{72,24,72}));paramsEvent(0,80);assert(params.applied && !params.active);}
  {readyNow();GapEvent e;e.kind=GapKind::Params;e.status=0;e.interval=90;params.pending=false;consume(e,clockMs);
   assert(params.interval==90 && !params.applied);}  // phone-initiated update: measured, not ours
  {readyNow();paramsEvent(16,0);assert(params.pending);   // Bluedroid's own update timer: still in flight
   for(unsigned i=0;i<100;++i)tickFlush(clockMs+=100);assert(paramRequests.size()==1 && params.pending);
   paramsEvent(0,80);assert(params.applied && !params.active && !params.pending);}
  {readyNow();// the wanted state moves on while the idle request runs: credit what was asked
   phonePipeNoteConsoleActivity();tickFlush(clockMs+=10);assert(params.wanted && paramRequests.size()==1);
   paramsEvent(0,80);assert(params.applied && !params.active);
   tickFlush(clockMs+=10);assert(paramRequests.size()==1);   // 5 s since the last request
   tickFlush(clockMs+=5000);assert((paramRequests==std::vector<int>{72,24}));
   paramsEvent(0,30);assert(params.applied && params.active);}
  {readyNow();paramsEvent(0,50);assert(!params.applied && !params.pending);  // landed outside the asked range
   tickFlush(clockMs+=5000);assert(paramRequests.size()==2);}
 }
 // F3 session counters: READY once per published session; an end is counted
 // once, by reason, and separately when Service Changed caused it.
 {reset();pendingEnrollment();phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  tickFlush(clockMs);tickFlush(++clockMs);assert(ready && sessionStats.ready==1 && !sessionStats.ended);
  phoneGatt.serviceChangedFlag=true;phoneGatt.pipe.error=Error::Protocol;phoneGatt.state=Gatt::Phase::Retiring;
  tickFlush(++clockMs);tickFlush(++clockMs);
  assert(!ready && sessionStats.ended==1 && sessionStats.serviceChanged==1 && sessionStats.endReason[unsigned(Error::Protocol)]==1);
  fail(clockMs,Error::Connect,"again");assert(sessionStats.ended==1);}  // already down: not counted twice
 {reset();pendingEnrollment();phoneGatt.authFailed=true;phoneGatt.state=Gatt::Phase::Retiring;tickFlush(clockMs);
  assert(sessionStats.authFailures==1 && !sessionStats.ready && !sessionStats.ended);}  // never READY: no end counted
}

// ---- A-1: OPEN phases on the real owner loop ------------------------------------------
// The session is READY at MTU 64 (P 36, 24-byte bodies), so a DNS OPEN is two fragments.
static void openTests() {
 struct Script{uint32_t submitAt=UINT32_MAX,lastAt=UINT32_MAX,answerAt=UINT32_MAX,dropAt=UINT32_MAX,cancelAt=UINT32_MAX;
  hw1pipe::Op answer=hw1pipe::Op::OpenOk;uint16_t code=0;};
 std::atomic<bool> cancel{false};uint32_t peerSeq=0,start=0;Script script;uint16_t opened=0;
 const auto prepare=[&](const Script& next){
  reset();phoneGatt.state=Gatt::Phase::Pipe;ready=true;liveGeneration=phoneGatt.gen;
  const uint8_t nonce[8]={0xa1,2,3,4,5,6,7,8};assert(phoneSession.start(64,nonce,clockMs,25));
  uint8_t f[512];assert(phoneSession.peek(f)&&phoneSession.accept(clockMs));
  uint8_t ack[32]{};ack[0]=hw1pipe::kVersion;ack[1]=uint8_t(hw1pipe::Op::HelloAck);hw1pipe::put16(ack+8,20);ack[10]=3;
  std::memcpy(ack+12,nonce,8);ack[20]=9;ack[28]=hw1pipe::kMaxStreams;ack[29]=hw1pipe::kPhoneRelayOn;hw1pipe::put16(ack+30,36);
  assert(phoneSession.receive(ack,32,clockMs)&&phoneSession.state==hw1pipe::State::Ready);
  relayOn=true;  // what setReady() takes from this HELLO_ACK
  peerSeq=1;cancel=false;script=next;start=clockMs;opened=0;
  playWorker=[&]{
   const uint32_t t=clockMs-start;uint8_t f[512];
   if(t>=script.cancelAt)cancel=true;
   if(t>=script.dropAt && ready){fail(clockMs,Error::Connect,"lost");phoneGatt.state=Gatt::Phase::Idle;}
   if(t>=script.submitAt && phoneSession.peek(f)) {
    const bool last=(f[10]&hw1pipe::kLast)!=0;
    if(!last || t>=script.lastAt){assert(phoneSession.accept(clockMs));if(!opened)opened=hw1pipe::get16(f+2);}
   }
   if(t>=script.answerAt && opened) {
    uint8_t a[16]{};a[0]=hw1pipe::kVersion;a[1]=uint8_t(script.answer);hw1pipe::put16(a+2,opened);hw1pipe::put32(a+4,peerSeq++);
    a[10]=3;size_t n=12;
    if(script.answer==hw1pipe::Op::OpenOk){hw1pipe::put16(a+8,4);hw1pipe::put32(a+12,1024);n=16;}
    else {hw1pipe::put16(a+8,2);hw1pipe::put16(a+12,script.code);n=14;}
    assert(phoneSession.receive(a,n,clockMs));script.answerAt=UINT32_MAX;
   }
  };
 };
 const auto open=[&](uint32_t budget,const char* host="relay.example.test"){
  PhonePipeStream h;const auto r=phonePipeOpen(h,host,443,budget,cancel,nullptr);return std::make_pair(r,h);};
 using St=PhonePipeOpenStatus;
 {Script x;x.submitAt=30;x.lastAt=30;x.answerAt=500;prepare(x);auto [r,h]=open(15000);
  assert(r.status==St::Opened && h.id && h.openedAt-start>=500 && h.corr==0xa102030405060708ull);
  assert(phoneSession.find(h.id)->state==hw1pipe::StreamState::Open);}
 {Script x;x.submitAt=30;x.lastAt=30;x.answer=hw1pipe::Op::OpenErr;x.code=6;x.answerAt=400;prepare(x);auto [r,h]=open(15000);
  assert(r.status==St::Refused && r.code==6 && h.id);}
 {Script x;prepare(x);auto [r,h]=open(15000);  // never submitted: bounded by 2 s
  assert(r.status==St::SubmitTimeout && clockMs-start>=2000 && clockMs-start<2020);
  assert(phoneSession.find(h.id)->state==hw1pipe::StreamState::Closing);}  // CLOSE(cancelled) queued
 {Script x;x.submitAt=10;x.lastAt=1900;prepare(x);auto [r,h]=open(15000);  // 4 s from the LAST fragment
  assert(r.status==St::ResponseTimeout && clockMs-start>=5900 && clockMs-start<5920);(void)h;}
 {Script x;x.submitAt=10;x.lastAt=1000;prepare(x);auto [r,h]=open(4500);   // caller deadline wins
  assert(r.status==St::Cancelled && clockMs-start>=4500 && clockMs-start<4520);(void)h;}
 {Script x;x.submitAt=10;x.lastAt=10;x.dropAt=300;prepare(x);auto [r,h]=open(15000);
  assert(r.status==St::LinkLost && clockMs-start<320);(void)h;}
 {Script x;x.submitAt=10;x.lastAt=10;x.cancelAt=200;prepare(x);auto [r,h]=open(15000);
  assert(r.status==St::Cancelled && clockMs-start<220);(void)h;}
 {Script x;prepare(x);auto [r,h]=open(3999);assert(r.status==St::NoBudget && !h.id && !phoneSession.find(1));}
 {Script x;prepare(x);ready=false;auto [r,h]=open(15000);assert(r.status==St::NotReady && !h.id);}
 // RELAY_ON 0: refused locally, so no OPEN goes out for the phone to refuse (16).
 {Script x;x.submitAt=10;x.lastAt=10;prepare(x);relayOn=false;auto [r,h]=open(15000);
  assert(r.status==St::NotReady && !h.id && !phoneSession.controlQueued() && !phoneSession.find(1));}
 {Script x;prepare(x);auto [r,h]=open(15000,"bad_host");assert(r.status==St::Rejected && !h.id);}
 {Script x;x.submitAt=30;x.lastAt=30;x.answerAt=200;prepare(x);auto [r,h]=open(15000,"192.0.2.10");
  assert(r.status==St::Opened);}
 playWorker={};
 // Replacing the secret clears a cooldown charged against the old key (the
 // stopped pipe only; a live pipe refuses invalidation and keeps it).
 {reset();static int handle;mutex=&handle;pskValid=true;cachedPsk[0]=0x5a;haveAuthFailure=true;lastAuthFailure=clockMs;
  assert(!phonePipeInvalidateSecret()&&pskValid&&authCoolingDown(clockMs));  // active
  active=false;assert(phonePipeInvalidateSecret()&&!pskValid&&!cachedPsk[0]&&!authCoolingDown(clockMs));
  mutex=nullptr;}
}

// ---- firmware-007: the reconnect scan's profile, coalesced candidates and records ----
static void scanTests() {
 {reset();
  // Default: continuous, no duplicate filter.
  runScan(50,200);
  assert(lastScan.scan_interval==hw1scan::kDefault.interval && lastScan.scan_window==hw1scan::kDefault.window);
  assert(lastScan.scan_duplicate==BLE_SCAN_DUPLICATE_DISABLE && lastScanSeconds==4);
  // 50 pipe reports and 200 others: one queued candidate, no overflow.
  assert(gapEvents.size()==2 && gapEvents[0].kind==GapKind::Candidate && gapEvents[1].kind==GapKind::ScanDone);
  assert(!gapOverflow && scanTally.reports==250 && scanTally.matches==50 && scanTally.bestRssi==-64);
  drainGap();
  // The match asked for an early stop; the timer's end crossed it and starts the link.
  assert(candidate && target[0]==0xc2 && step==Step::Link && events.back()=="gatt-begin");
  assert(countEvents("stop-scan")==1 && scanStopPending);
  assert(coexLog.scanTotal==1 && phoneScans(hw1coex::ScanOutcome::Found)==1);
  const auto& found=lastScanRecord();
  assert(found.seq==1 && found.outcome==hw1coex::ScanOutcome::Found && found.durationMs==4000 && found.controllerTimed);
  assert(found.peer==hw1coex::Peer::Phone && found.purpose==hw1coex::Purpose::Reconnect && !found.active);
  assert(found.reports==250 && found.matches==50 && found.rssi==-64 && !found.duplicateFilter && found.interval==96);
  assert(found.firstMatchMs==30 && found.wifi.start==WifiCoex::Associated && found.wifi.end==WifiCoex::Associated && found.seconds==4);
  // Bluedroid then fails the crossed stop ("scan not active"): harmless (F1).
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,ESP_GAP_SEARCH_INQ_CMPL_EVT,1);drainGap();
  assert(!scanStopPending && step==Step::Link && coexLog.scanTotal==1 && events.back()=="gatt-begin");
  assert(registrationCount==1 && countEvents("gatt-begin")==1);
 }
 {reset();
  // The usual early stop: the match is handled mid-scan, our stop is answered
  // (Bluedroid sends no ScanDone after an explicit stop) and the link starts.
  startScan();wifiNow.state=WifiCoex::Hunting;reports(3,10);drainGap();
  assert(candidate && scanStopPending && step==Step::Scanning && events.back()=="stop-scan");
  clockMs+=5;scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);drainGap();
  assert(!scanStopPending && !scanStarted && step==Step::Link && events.back()=="gatt-begin");
  assert(phoneScans(hw1coex::ScanOutcome::Found)==1 && lastScanRecord().durationMs==35);
  assert(lastScanRecord().wifi.start==WifiCoex::Associated && lastScanRecord().wifi.end==WifiCoex::Hunting);
  // Outside our scan, scan events go to their owners (the G2's BLEScan).
  scanEvent(ESP_GAP_BLE_SCAN_RESULT_EVT,1,0,false);scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,0,1,false);
  scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,1,0,false);scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,1,0,false);
  assert(gapEvents.empty());
  // Routed to us but not ours (a pending answer that never comes, then strays): ignored (F3).
  {GapEvent e;for(auto k:{GapKind::ScanDone,GapKind::ScanStopped,GapKind::ScanParams,GapKind::ScanStart}){e.kind=k;e.status=1;consume(e,clockMs);}}
  assert(step==Step::Link && coexLog.scanTotal==1 && !scanStarted && !scanStartPending && !scanParamsPending);
  assert(registrationCount==1 && countEvents("gatt-begin")==1);
 }
 {reset();
  // A foreign failed stop during our parameters, then a foreign end while our
  // start is in flight: both ignored, our scan still runs (F3).
  job=true;beginJob(clockMs);scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,0,1);drainGap();
  assert(step==Step::ScanParams && scanParamsPending);
  scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);drainGap();assert(scanStartPending);
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);drainGap();assert(scanStartPending && step==Step::Scanning);
  scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT);drainGap();assert(scanStarted && scanOpen);
  // The deadline is the scan length plus 1 s.
  clockMs+=4999;tickFlush(clockMs);assert(step==Step::Scanning);
  clockMs+=1;tickFlush(clockMs);assert(step==Step::Retiring && statusIs("BLE setup timed out"));
 }
 {reset();
  // Retirement whose stop crosses the timer: the end, then a failed stop. The
  // cleanup completes instead of faulting into a host restart (F1).
  startScan();clockMs+=700;fail(clockMs,Error::Cancelled,"phone operation cancelled");tickFlush(clockMs);
  assert(scanStopPending);
  scanEvent(ESP_GAP_BLE_SCAN_RESULT_EVT);scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,0,1);drainGap();
  tickFlush(clockMs);assert(step==Step::Idle && !recoveryNeeded && phoneScans(hw1coex::ScanOutcome::Aborted)==1);
  // A stop the API refuses leaves the scan to its own timer, with no retries.
  reset();stopResult=1;
  startScan();fail(clockMs,Error::Cancelled,"phone operation cancelled");tickFlush(clockMs);tickFlush(clockMs);
  assert(!scanStopPending && scanStopRefused && countEvents("stop-scan")==1);
  clockMs+=4000;tickFlush(clockMs);assert(step==Step::Retiring && !recoveryNeeded); // waits for the timer
  scanEvent(ESP_GAP_BLE_SCAN_RESULT_EVT);tickFlush(clockMs);assert(step==Step::Idle && !recoveryNeeded);
  // An early stop the API refuses: the timer's end still starts the link.
  reset();stopResult=1;startScan();reports(1,0);drainGap();
  assert(candidate && !scanStopPending && scanStopRefused && step==Step::Scanning);
  clockMs+=4000;scanEvent(ESP_GAP_BLE_SCAN_RESULT_EVT);drainGap();assert(step==Step::Link);
 }
 {reset();
  // Nothing heard: a missed scan, then "phone not found".
  runScan(0,30);drainGap();
  assert(statusIs("phone not found") && step==Step::Retiring);
  assert(coexLog.scanTotal==1 && phoneScans(hw1coex::ScanOutcome::Missed)==1 && lastScanRecord().reports==30 && !lastScanRecord().matches);
  // An unusable address type is counted but never queued; the next usable report is.
  reset();runScan(2,0,7);
  assert(gapEvents.size()==2 && gapEvents[0].kind==GapKind::Candidate && gapEvents[0].type==1 && scanTally.matches==2);
  // Reports outside our scan belong to other owners and are not counted.
  reset();active=true;routeSet();
  {auto p=report(true,-50);assert(!gapCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&p));assert(!scanTally.reports && gapEvents.empty());}
 }
 {reset();
  // A scan the controller refuses to start is a start-failed record, handed
  // to the log only after the pass that made it (run() releases the owner
  // mutex first).
  job=true;beginJob(clockMs);scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);drainGap();
  scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,ESP_GAP_SEARCH_INQ_CMPL_EVT,1);
  {GapEvent e;while(xQueueReceive(gapQueue,&e,0)==pdTRUE)consume(e,clockMs);}
  assert(step==Step::Retiring && scanRecordPending && coexLog.scanTotal==0);
  flushRecords();
  assert(!scanRecordPending && coexLog.scanTotal==1 && phoneScans(hw1coex::ScanOutcome::StartFailed)==1);
  assert(!lastScanRecord().controllerTimed && lastScanRecord().firstMatchMs==hw1coex::Tally::kNone);
  // Refused parameters too.
  reset();job=true;beginJob(clockMs);
  scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,ESP_GAP_SEARCH_INQ_CMPL_EVT,1);drainGap();
  assert(coexLog.scanTotal==1 && phoneScans(hw1coex::ScanOutcome::StartFailed)==1);
 }
 {reset();
  // A scan cut short by retirement is recorded as aborted, not missed.
  job=true;configureScan(clockMs);scanEvent(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);drainGap();
  scanEvent(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT);drainGap();assert(scanStarted&&scanOpen);
  clockMs+=700;fail(clockMs,Error::Cancelled,"phone operation cancelled");tickFlush(clockMs);assert(scanStopPending);
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);drainGap();
  assert(coexLog.scanTotal==1 && phoneScans(hw1coex::ScanOutcome::Aborted)==1 && lastScanRecord().durationMs>=700);
 }
}

// ---- One record per attempt that found the phone ----------------------------------------
static void recordTests() {
 {reset();
  // R1: a found scan whose open fails (0x3e behind the generic 0x85): one
  // failed record with the adapter's stage, status and reason; the Wi-Fi span
  // starts at the candidate; later passes, an accessory waiting, add nothing.
  wifiNow.state=WifiCoex::Connecting;wifiNow.scans=4;runScan(1,0);drainGap();
  assert(step==Step::Link && linkBegun && connectWifi.start==WifiCoex::Connecting);
  tickFlush(clockMs);assert(coexLog.connectTotal==0);
  wifiNow.state=WifiCoex::Associated;wifiNow.scans=5;clockMs+=1630;holdTurn();
  phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;phoneGatt.failGatt=0x85;phoneGatt.failReason=0x3e;
  tickFlush(clockMs);
  assert(coexLog.connectTotal==1 && phoneConnects(O::Failed)==1 && statusIs("phone link lost"));
  assert(step==Step::Retiring && scanStopPending && setupHeld && phoneWakes.empty());  // the crossed stop is still unanswered
  const auto& c=lastConnect();
  assert(c.peer==hw1coex::Peer::Phone && c.purpose==hw1coex::Purpose::Reconnect && c.outcome==O::Failed);
  assert(c.stage==S::Open && c.gattStatus==0x85 && c.reason==0x3e && c.cause==uint8_t(Error::Connect) && c.detail==D::None);
  assert(c.elapsedMs==1630 && c.wifi.start==WifiCoex::Connecting && c.wifi.end==WifiCoex::Associated && c.wifi.scans==1);
  accessory=true;submitAccessory(0);for(int i=0;i<5;++i)tickFlush(clockMs+=10);
  assert(coexLog.connectTotal==1);
 }
 {reset();
  // The cancel is decided before the adapter's events are consumed (no new
  // link work on a cancelled attempt), so a link failure still queued in the
  // same tick reads as the cancel: the record's 10 ms blind spot.
  runScan(1,0);drainGap();holdTurn();phoneGatt.stageNow=S::Discovery;
  phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;phoneGatt.failReason=0x08;stopWanted=true;tickFlush(clockMs+=10);
  assert(coexLog.connectTotal==1 && lastConnect().outcome==O::Cancelled && lastConnect().detail==D::Stopped);
  // A link failure a tick earlier keeps its own cause.
  reset();runScan(1,0);drainGap();holdTurn();phoneGatt.stageNow=S::Discovery;
  phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;phoneGatt.failReason=0x08;tickFlush(clockMs+=10);
  stopWanted=true;tickFlush(clockMs+=10);
  assert(coexLog.connectTotal==1 && lastConnect().outcome==O::Failed && lastConnect().reason==0x08 &&
         lastConnect().detail==D::None && lastConnect().cause==uint8_t(Error::Connect));
  // The adapter stopped first (the phone's RESET reads as Cancelled too): a
  // later local stop's source does not apply.
  reset();runScan(1,0);drainGap();phoneGatt.latch(clockMs,Error::Cancelled);
  fail(clockMs,Error::Cancelled,"disconnected",D::Stopped);tickFlush(clockMs);
  assert(coexLog.connectTotal==1 && lastConnect().cause==uint8_t(Error::Cancelled) && lastConnect().detail==D::None);
 }
 // Retiring -> Idle after a stopped scan: consume() ends a retiring scan
 // without republishing the route, so Idle must publish it before lifting the
 // recovery block and giving the turn back; otherwise the next client's scan
 // events would still be claimed by the phone. The block is lifted while the
 // turn is still held.
 {reset();startScan();const uint32_t t=holdTurn();recoveryNeeded=true;recoveryBlocked=true;
  assert(route.scan);
  stopWanted=true;tickFlush(clockMs+=10);
  assert(step==Step::Retiring && scanStopPending);
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);drainGap();
  assert(!scanStarted && !scanStopPending);
  releaseHook=assertScanRouteClear;tickFlush(clockMs+=10);releaseHook=nullptr;
  assert(step==Step::Idle && !setupHeld && turn.ended==t && phoneWakes.size()==1 && !recoveryBlocked && !route.scan);
  assert(blockCalls.size()==1 && !blockCalls[0].value && blockCalls[0].setupHeld && !phoneWakes[0].blocked);}
 // R3: each cancel source has its own status and detail.
 for(int source=0;source<3;++source) {
  reset();runScan(1,0);drainGap();phoneGatt.stageNow=S::Mtu;
  if(source==0)stopWanted=true;
  if(source==1)++authorityIntent;
  if(source==2){enrolling=true;enrollmentEpoch=5;jobAt=clockMs-60000;}
  tickFlush(clockMs);
  const char* labels[]={"phone stop requested","phone intent changed","phone enrollment expired"};
  const D details[]={D::Stopped,D::Replaced,D::Expired};
  assert(coexLog.connectTotal==1 && lastConnect().outcome==O::Cancelled && lastConnect().detail==details[source]);
  assert(statusIs(labels[source]) || source==0);
  assert(lastConnect().purpose==(source==2?hw1coex::Purpose::Enroll:hw1coex::Purpose::Reconnect));
 }
 {reset();
  // The intent is replaced just as the link reaches READY: stage ready.
  runScan(1,0);drainGap();phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  phoneGatt.stageNow=S::Hello;authority=false;tickFlush(clockMs);
  assert(coexLog.connectTotal==1 && lastConnect().stage==S::Ready && lastConnect().detail==D::Replaced);
 }
 {reset();
  // R4: cancelled between the match and the scan's end: GATT never began.
  startScan();reports(1,0);drainGap();assert(candidate && step==Step::Scanning);
  stopWanted=true;tickFlush(clockMs);
  assert(coexLog.connectTotal==1 && lastConnect().stage==S::Precheck && lastConnect().elapsedMs==0 &&
         lastConnect().detail==D::Stopped);
  // No GATT app id: registration unavailable.
  reset();appIdFails=true;runScan(1,0);drainGap();tickFlush(clockMs);
  assert(coexLog.connectTotal==1 && lastConnect().stage==S::RegisterApi && lastConnect().cause==uint8_t(Error::Resources) &&
         lastConnect().outcome==O::Failed);
 }
 {reset();
  // R5: READY writes one connected record that matches `last setup_ms`; a
  // later link loss adds none. R6: a missed scan writes none.
  runScan(1,0);drainGap();clockMs+=7480;phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  tickFlush(clockMs);
  assert(ready && coexLog.connectTotal==1 && lastConnect().outcome==O::Connected && lastConnect().stage==S::Ready);
  assert(lastConnect().elapsedMs==lastSetup.total && lastConnect().elapsedMs==7480 && lastConnect().gattStatus==-1);
  phoneGatt.failNext=true;tickFlush(clockMs+=10);tickFlush(clockMs+=10);assert(coexLog.connectTotal==1);
  reset();runScan(0,10);drainGap();tickFlush(clockMs);
  assert(phoneScans(hw1coex::ScanOutcome::Missed)==1 && coexLog.connectTotal==0);
 }
 {reset();
  // A stop from a command task (fail() under the owner mutex) is recorded by
  // the pipe task on its next pass.
  runScan(1,0);drainGap();fail(clockMs,Error::Cancelled,"disconnected",D::Stopped);
  assert(attemptWanted && !connectRecordPending && coexLog.connectTotal==0);
  tickFlush(clockMs);assert(coexLog.connectTotal==1 && lastConnect().detail==D::Stopped);
 }
 {reset();
  // `phone scan`: show for anyone, change for a local admin only, the
  // change applies from the next scan, legacy restores the old parameters.
  std::string text=scanCommand("",false);
  assert(text.find("Phone pipe scan: interval_ms=60.00 window_ms=60.00 duplicates=off up_to_s=4 (default; runtime only; default at boot)\nscans total=0")==0);
  text=scanCommand(" legacy",false);assert(text.rfind("Error: ",0)==0 && hw1scan::same(scanProfile,hw1scan::kDefault));
  text=scanCommand(" 100 200 dup",true);assert(text.rfind("Error: phone scan: window",0)==0 && hw1scan::same(scanProfile,hw1scan::kDefault));
  text=scanCommand(" legacy",true);assert(text.find("(legacy; from the next scan until reboot)")!=std::string::npos);
  runScan(1,5);
  assert(lastScan.scan_interval==160 && lastScan.scan_window==48 && lastScan.scan_duplicate==BLE_SCAN_DUPLICATE_ENABLE);
  assert(lastScanSeconds==2);
  drainGap();assert(lastScanRecord().duplicateFilter && lastScanRecord().interval==160);
  text=scanCommand(" 90 30 nodup 3",true);assert(text.find("interval_ms=90.00 window_ms=30.00 duplicates=off up_to_s=3 (custom")!=std::string::npos);
  for(unsigned i=0;i<9;++i){step=Step::Idle;runScan(i%2,3);drainGap();}
  text=scanCommand("",false);
  assert(text.find("scans total=10 found=")!=std::string::npos && text.find("\n#10 ")!=std::string::npos);
  assert(text.find("\n#5 ")!=std::string::npos && text.find("\n#4 ")==std::string::npos);  // the newest six
  assert(text.find("\nconnects total=")!=std::string::npos);
  // R9: a worst case of six scan and six attempt lines fits the reply.
  {hw1coex::ScanRecord r{};r.peer=hw1coex::Peer::Phone;r.purpose=hw1coex::Purpose::Reconnect;r.outcome=hw1coex::ScanOutcome::StartFailed;
   r.durationMs=r.reports=r.matches=4294967295u;r.firstMatchMs=4294967294u;r.rssi=-128;r.interval=r.window=16384;r.seconds=255;
   r.wifi.start=r.wifi.end=WifiCoex::Associated;r.wifi.flagsStart=r.wifi.flagsEnd=0xff;r.wifi.ageS=254;
   r.wifi.scans=r.wifi.connects=r.wifi.disconnects=255;
   hw1coex::ConnectRecord c{};c.peer=hw1coex::Peer::Phone;c.purpose=hw1coex::Purpose::Reconnect;c.outcome=hw1coex::ConnectOutcome::Cancelled;
   c.stage=hw1coex::ConnectStage::RegisterTimeout;c.gattStatus=255;c.reason=0x100;c.cause=14;c.detail=hw1coex::CauseDetail::ServiceChanged;
   c.elapsedMs=4294967295u;c.waitMs=65534;c.wifi=r.wifi;
   coexLog=hw1coex::Log{};for(int i=0;i<16;++i){coexLog.addScan(r);coexLog.addConnect(c);}
   coexLog.scanTotal=coexLog.connectTotal=4294967295u-16;for(int i=0;i<16;++i){coexLog.addScan(r);coexLog.addConnect(c);}
   scanProfile=hw1scan::kDefault;text=scanCommand("",false);
   assert(text.size()<CMD_RESULT_MAX-64 && std::count(text.begin(),text.end(),'#')==12);}
 }
}

// ---- The phone's client on the worker: wiring, setupReady and TurnEnv ------------------
static void opsTests() {
 assert(kOps[size_t(hw1setup::Client::Phone)]==&phonePipeSetupOps);
 assert(phonePipeSetupOps.ready==setupReady && phonePipeSetupOps.run==setupRun && !phonePipeSetupOps.waiting);
 assert(!phonePipeSetupOps.recordWait && !std::strcmp(phonePipeSetupOps.label(1),"phone"));
 // setupReady: drop when the pipe cannot use a turn, wait out the recovery block.
 {reset();const uint32_t t=admitPhone();const hw1setup::WaitInfo info{};
  const auto is=[&](uint32_t token,hw1setup::Ready r,uint8_t why){const auto v=setupReady(token,info);return v.ready==r&&v.reason==why;};
  assert(is(t,hw1setup::Ready::Run,hw1setup::kNone));
  assert(is(t+1,hw1setup::Ready::Drop,hw1setup::kInactive));  // a stale token
  stopWanted=true;assert(is(t,hw1setup::Ready::Drop,hw1setup::kInactive));stopWanted=false;
  quiesceWanted=true;assert(is(t,hw1setup::Ready::Drop,hw1setup::kInactive));quiesceWanted=false;
  active=false;assert(is(t,hw1setup::Ready::Drop,hw1setup::kInactive));active=true;
  recoveryBlocked=true;assert(is(t,hw1setup::Ready::Wait,hw1setup::kRecovery));
  stopWanted=true;assert(is(t,hw1setup::Ready::Drop,hw1setup::kInactive));  // a drop outranks the wait
 }
 // The worker sees a stop before the phone task does: the job is dropped
 // unrun, and the phone's own stop then finds nothing to take back.
 {reset();admitPhone();stopWanted=true;serveWorker();
  assert(prioritySets.empty()&&phoneSlotIdle()&&turn.granted==0&&step==Step::Waiting);
  tickFlush(clockMs+=10);
  assert(step==Step::Idle&&!job&&wantToken==0&&withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::NotLive&&!active);}
 // While the recovery block stands the phone waits (never a grant); it runs
 // once the block lifts.
 {reset();world.on=true;world.linkMs=100;recoveryBlocked=true;const uint32_t t=admitPhone();serveWorker();
  assert(prioritySets.empty()&&phoneSlotState()==hw1setup::State::Queued&&step==Step::Waiting);
  assert(sCore.peek(hw1setup::Client::Phone).waitReason==hw1setup::kRecovery);
  serveWorker();assert(prioritySets.empty()&&step==Step::Waiting);  // the 100 ms recheck: still blocked
  recoveryBlocked=false;serveWorker();
  assert(prioritySets.size()==2&&ready&&turn.ended==t&&phoneSlotIdle());}
 // TurnEnv: the worker's half of the turn.
 {reset();TurnEnv env;
  env.grant(42);  // the priority first, then the grant
  assert(prioritySets.size()==1&&prioritySets[0].priority==5&&prioritySets[0].granted==0&&turn.granted==42&&phonePriority==5);
  env.release(42);assert(prioritySets.size()==2&&phonePriority==1);
  worker=nullptr;env.grant(43);env.release(43);assert(prioritySets.size()==2&&turn.granted==43);worker=&phoneTaskHandle;
  env.overran(43,false);assert(logged("BLE_SETUP: phone turn overran 75 s, unused"));
  env.overran(43,true);assert(logged("BLE_SETUP: phone turn overran 75 s; stopping the phone setup"));
  // escalate: the block, then recoveryNeeded.
  env.escalate(43);
  assert(blockCalls.size()==1&&blockCalls[0].value&&!blockCalls[0].recoveryNeeded&&recoveryNeeded&&recoveryBlocked);
  assert(logged("BLE_SETUP: phone setup did not stop; host recovery requested"));
  // settle undoes only an escalation still pending.
  env.settle(43);assert(!recoveryNeeded&&!recoveryBlocked&&blockCalls.size()==2);
  recoveryBlocked=true;env.settle(43);assert(recoveryBlocked&&blockCalls.size()==2);}
 // settle never lifts a request from the phone's own timed-out cleanup: the
 // escalation and the cleanup can land together.
 {reset();TurnEnv env;holdTurn();step=Step::Retiring;job=true;at=clockMs;
  cleanupExpired(clockMs,"phone cleanup incomplete");
  assert(cleanupFaulted&&recoveryNeeded&&recoveryBlocked&&!setupHeld);
  env.escalate(1);env.settle(1);
  assert(recoveryNeeded&&recoveryBlocked);
  // The cleanup ends at Idle: the request and the marker go together.
  phoneGatt.hostStopped();step=Step::Fault;scanStarted=scanStartPending=scanStopPending=scanParamsPending=false;
  tickFlush(clockMs+=10);
  assert(step==Step::Idle&&!cleanupFaulted&&!recoveryNeeded&&!recoveryBlocked);}
 // On a C6 with fewer than four links the phone waits, at most 10 s, while
 // the ring still holds the link it gives up (the pipe shares the stack with
 // the ring whether or not the glasses are open: no pipe-owned host any more).
 {reset();const uint32_t t=admitPhone();hw1setup::WaitInfo w{};
  const auto is=[&](hw1setup::Ready r,uint8_t why){const auto v=setupReady(t,w);return v.ready==r&&v.reason==why;};
  ringYield=true;ringLinkHeld=true;w.queuedMs=9999;assert(is(hw1setup::Ready::Wait,hw1setup::kYield));
  w.queuedMs=10000;assert(is(hw1setup::Ready::Drop,hw1setup::kYield));
  w.queuedMs=0;ringLinkHeld=false;assert(is(hw1setup::Ready::Run,hw1setup::kNone));
  ringLinkHeld=true;ringYield=false;assert(is(hw1setup::Ready::Run,hw1setup::kNone));
  ringYield=true;assert(is(hw1setup::Ready::Wait,hw1setup::kYield));
  ringYield=false;recoveryBlocked=true;assert(is(hw1setup::Ready::Wait,hw1setup::kRecovery));}
}

// ---- Admission: claim first, then state; every failure gives the slot back -------------
static void admissionTests() {
 // The slot is still in use (the previous job's finish has not run): BUSY,
 // and nothing about the pipe changed.
 {reset();intent=1;identityGeneration=2;uint32_t other=0;assert(realSetupClaim(kPhone,&other));
  BlePeerConnectRequest wanted;wanted.explicitReseek=true;
  assert(admit(wanted)==BlePeerConnectAdmission::BUSY&&claimCalls==1&&publishCalls==0);
  assert(!atClaim.job&&atClaim.step==Step::Idle&&!atClaim.want&&atClaim.intent==1);
  assert(!job&&step==Step::Idle&&statusIs("disconnected")&&!wantToken&&!jobAt&&intent==1&&identityGeneration==2&&!request.explicitReseek);}
 // The worker is not running: claims fail.
 {reset();sInit=Init::Uninitialized;BlePeerConnectRequest wanted;
  assert(admit(wanted)==BlePeerConnectAdmission::BUSY&&!job&&step==Step::Idle&&phoneSlotIdle());sInit=Init::Ready;}
 // A refused publish cancels the claim and returns to Idle.
 {reset();failPublish=true;BlePeerConnectRequest wanted;
  assert(admit(wanted)==BlePeerConnectAdmission::BUSY&&claimCalls==1&&publishCalls==1&&cancelCalls==1&&lastCancelToken);
  assert(phoneSlotIdle()&&!job&&!wantToken&&step==Step::Idle&&statusIs("disconnected"));
  failPublish=false;assert(admit(wanted)==BlePeerConnectAdmission::STARTED);}  // the slot is free again
 // Admitted: queued on the worker with the job's token, the worker woken.
 {reset();BlePeerConnectRequest wanted;wanted.explicitReseek=true;const uint32_t t=admitPhone(wanted);
  assert(t&&job&&!enrolling&&step==Step::Waiting&&statusIs("waiting for radio")&&jobAt==clockMs&&request.explicitReseek);
  assert(phoneSlotState()==hw1setup::State::Queued&&bleSetupLive(kPhone,t)&&wakeGiven&&prioritySets.empty());
  assert(!atClaim.job&&atClaim.step==Step::Idle);}
 // `openphone` on a running pipe admits a fresh manual request; a second one
 // finds that job pending.
 {reset();assert(!std::strcmp(cmd_openphone(""),"Phone: open; connecting to the paired phone")&&job&&intent==authorityIntent);
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connect pending (busy, the scheduler retries)")&&claimCalls==1&&pipeStarts==0);}
 // A closed pipe opens first, on the shared stack (beside the glasses or
 // alone: the same open), then connects. Never over the app console server.
 {reset();active=false;g2Up=true;
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connecting to the paired phone")&&pipeStarts==1&&job);}
 {reset();active=false;
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connecting to the paired phone")&&pipeStarts==1&&job);}
 {reset();active=false;serverUp=true;
  assert(!std::strcmp(cmd_openphone(""),"Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'"));
  assert(pipeStarts==0&&!job&&claimCalls==0);}
 {reset();active=false;startOk=false;status="configure phone secret first";
  assert(!std::strcmp(cmd_openphone(""),"Error: phone link could not open (configure phone secret first)"));
  assert(pipeStarts==1&&!job&&claimCalls==0);}
 // An already-open pipe that is connected says so, with no new job.
 {reset();ready=true;assert(!std::strcmp(cmd_openphone(""),"Phone: open and connected")&&!job&&claimCalls==0);}
 // `phone pair`: local admin, cooldown before the intent bump, claim
 // before the intent bump, cancel the claim when learning or publishing fails.
 {reset();execAdmin=false;assert(!std::strcmp(phonePairCommand(""),"Error: phone pairing requires an authenticated local admin"));
  // A stopped link starts after the admin and owner checks, then pairs.
  reset();active=false;execAdmin=false;assert(!std::strcmp(phonePairCommand(""),"Error: phone pairing requires an authenticated local admin"));
  assert(pipeStarts==0);
  reset();active=false;serverUp=true;
  assert(!std::strcmp(phonePairCommand(""),"Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'"));
  assert(!claimCalls&&!manualLearns&&pipeStarts==0);}
 {reset();active=false;g2Up=true;const std::string reply=phonePairCommand("");
  assert(reply.rfind("Pairing opened for 60 seconds",0)==0&&pipeStarts==1&&enrolling);}
 {reset();haveAuthFailure=true;lastAuthFailure=clockMs;
  assert(!std::strcmp(phonePairCommand(""),"Error: pipe authentication cooldown (30 seconds)"));
  assert(manualLearns==0&&claimCalls==0&&authorityIntent==7&&!job&&!enrolling);}
 {reset();uint32_t other=0;assert(realSetupClaim(kPhone,&other));
  assert(!std::strcmp(phonePairCommand(""),"Error: phone pipe busy; disconnect first"));
  assert(claimCalls==1&&manualLearns==0&&authorityIntent==7&&!job&&!enrolling);}
 {reset();manualLearnOk=false;
  assert(!std::strcmp(phonePairCommand(""),"Error: enrollment authority unavailable"));
  assert(manualLearns==1&&cancelCalls==1&&phoneSlotIdle()&&!job&&!enrolling&&!wantToken&&step==Step::Idle);}
 {reset();failPublish=true;
  assert(!std::strcmp(phonePairCommand(""),"Error: phone pipe busy; disconnect first"));
  assert(cancelCalls==1&&phoneSlotIdle()&&!job&&!enrolling&&!wantToken&&step==Step::Idle&&statusIs("disconnected"));}
 {reset();const uint32_t t=enrollPhone();
  assert(t&&job&&enrolling&&step==Step::Waiting&&statusIs("waiting to enroll")&&bleSetupLive(kPhone,t));
  assert(manualLearns==1&&intent==authorityIntent&&enrollmentEpoch==5&&jobAt==clockMs&&!atClaim.job);}
 // The validate pass (automation checks) answers VALID before any handler
 // acts, and never starts a stopped link.
 {reset();active=false;gCLIValidateOnly=true;
  for(auto* handler:{phonePairCommand,cmd_openphone,phoneForgetCommand,cmd_closephone})
   assert(!std::strcmp(handler(""),"VALID"));
  assert(!std::strcmp(phoneScanCommand("legacy"),"VALID")&&hw1scan::same(scanProfile,hw1scan::kDefault));
  gCLIValidateOnly=false;assert(!job&&!enrolling&&!claimCalls&&!manualLearns&&!userDisconnects&&!pipeStarts&&step==Step::Idle);}
 // The row wrappers take the local-admin rule from the caller's identity.
 {reset();authContext.transport=SOURCE_WEB;
  assert(!std::strcmp(phonePairCommand(""),"Error: phone pairing requires an authenticated local admin"));
  assert(!std::strcmp(phoneForgetCommand(""),"Error: phone forget requires an authenticated local admin"));
  assert(std::string(phoneScanCommand("legacy")).rfind("Error: changing the phone scan requires an authenticated local admin",0)==0);
  assert(std::string(phoneScanCommand("")).rfind("Phone pipe scan: ",0)==0&&hw1scan::same(scanProfile,hw1scan::kDefault));
  authContext.transport=SOURCE_LOCAL_DISPLAY;execAdmin=false;
  assert(std::string(phoneScanCommand("legacy")).rfind("Error: ",0)==0&&hw1scan::same(scanProfile,hw1scan::kDefault));
  execAdmin=true;assert(std::string(phoneScanCommand("legacy")).find("(legacy; from the next scan until reboot)")!=std::string::npos);
  assert(!claimCalls&&!manualLearns);}
 // `phone stats` keeps the link counters; the correlation id and the relay
 // users' path report are `relay status`.
 {reset();sessionStats.ready=2;sessionStats.endReason[unsigned(Error::Protocol)]=1;
  const std::string stats=phoneStatsCommand("");
  assert(stats.rfind("Phone pipe: ",0)==0&&stats.find("sessions ready=2 ")!=std::string::npos);
  assert(stats.find(" "+std::to_string(unsigned(Error::Protocol))+"=1")!=std::string::npos&&stats.find("corr=")==std::string::npos);
  const std::string relay=relayStatusCommand("");
  assert(relay=="Relay: phone down relay=unknown corr=-\nNo relay users in this build (LLM HTTP source off)");}
}

// ---- Waiting: the grant for the job's own token, consumed once; the backstop ------------
static void waitingTests() {
 {reset();const uint32_t t=admitPhone();
  // The phone task alone never starts a setup.
  for(int i=0;i<20;++i)tickFlush(clockMs+=10);
  assert(step==Step::Waiting&&!setupHeld&&events.empty());
  // A grant for another token, used or not, or none at all, is never consumed.
  for(uint32_t other:{t+1,(t+1)|hw1setup::kUsed,t|hw1setup::kUsed,0u}){
   turn.granted=other;tickFlush(clockMs+=10);
   assert(step==Step::Waiting&&!setupHeld&&turn.granted==other&&events.empty()&&!heldToken);}
  // Its own grant: consumed exactly once.
  turn.granted=t;tickFlush(clockMs+=10);const uint32_t at0=clockMs;
  assert(setupHeld&&heldToken==t&&grantAt==at0&&turn.granted==(t|hw1setup::kUsed));
  assert(step==Step::ScanParams&&countEvents("scan-params")==1&&gateWaitMs==at0-jobAt);
  tickFlush(clockMs+=10);assert(grantAt==at0&&countEvents("scan-params")==1&&setupHeld);}
 // The backstop: the worker ended the job without a turn (here, taken back
 // as a drop would). The phone goes Idle instead of answering BUSY forever,
 // and gives back an explicit one-shot reseek only.
 struct Case{bool enroll,explicitReseek,autoReconnect,nudge;};
 for(const Case c:{Case{false,true,false,true},Case{false,false,false,false},Case{false,true,true,false},Case{true,true,false,false}}){
  reset();
  BlePeerConnectRequest wanted;wanted.explicitReseek=c.explicitReseek;wanted.autoReconnect=c.autoReconnect;
  const uint32_t t=c.enroll?enrollPhone():admitPhone(wanted);
  if(c.enroll)request.explicitReseek=true;  // never a nudge for an enrollment
  assert(realSetupWithdraw(kPhone,t)==BleSetupWithdraw::Withdrawn);
  tickFlush(clockMs+=10);
  assert(step==Step::Idle&&statusIs("setup turn ended unused")&&!job&&!enrolling&&!wantToken&&!setupHeld);
  assert(reseekRestores==(c.nudge?1u:0u)&&withdrawCalls==0&&phoneWakes.empty());
  if(c.nudge)assert(reseekIntent==intent&&reseekIdentity==identityGeneration);
  tickFlush(clockMs+=10);assert(reseekRestores==(c.nudge?1u:0u));
  BlePeerConnectRequest again;assert(admit(again)==BlePeerConnectAdmission::STARTED);}  // not stuck BUSY
}

// ---- Exits without a turn: every one takes the queued job back, once ---------------------
static void queuedExitTests() {
 for(int cause=0;cause<3;++cause){reset();
  const uint32_t t=cause==2?enrollPhone():admitPhone();
  if(cause==0)stopWanted=true;
  if(cause==1)++authorityIntent;
  if(cause==2){clockMs=jobAt+59990;tickFlush(clockMs);assert(step==Step::Waiting&&job);clockMs=jobAt+60000;}
  tickFlush(clockMs);
  assert(withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::Withdrawn&&phoneSlotIdle()&&!bleSetupLive(kPhone,t));
  assert(!wantToken&&!job&&step==Step::Idle&&phoneWakes.empty()&&!turn.ended&&!turn.granted);
  // A job that never reached READY keeps its reason, not "disconnected".
  assert(statusIs(cause==0?"stopped":cause==1?"phone intent changed":"phone enrollment expired"));
  tickFlush(clockMs+=10);assert(withdrawCalls==1);
  serveWorker();assert(prioritySets.empty());}  // the worker never grants it
 // Quiesce and host reset take a queued job back too.
 {reset();admitPhone();recoveryNeeded=true;quiesceWanted=true;tickFlush(clockMs+=10);
  assert(quiesced&&!active&&withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::Withdrawn&&phoneSlotIdle()&&!wantToken);}
 {reset();admitPhone();hostResetWanted=true;tickFlush(clockMs+=10);
  assert(withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::Withdrawn&&phoneSlotIdle()&&!wantToken&&!job&&step==Step::Idle);}
 // A held turn is given back by quiesce and by a host reset, once.
 {reset();job=true;const uint32_t t=holdTurn();step=Step::Link;phoneGatt.state=Gatt::Phase::Opening;
  recoveryNeeded=true;quiesceWanted=true;tickFlush(clockMs+=10);
  assert(quiesced&&!setupHeld&&!wantToken&&turn.ended==t&&phoneWakes.size()==1&&!phoneWakes[0].routeEnabled);
  tickFlush(clockMs+=10);assert(phoneWakes.size()==1);}
 {reset();job=true;const uint32_t t=holdTurn();step=Step::Link;phoneGatt.state=Gatt::Phase::Opening;
  hostResetWanted=true;tickFlush(clockMs+=10);
  assert(!setupHeld&&!wantToken&&turn.ended==t&&phoneWakes.size()==1&&step==Step::Idle&&statusIs("stopped"));}
 // R2 (pressure): an accessory job queued or running is pressure on the
 // phone's adapter; the phone's own slot never is.
 {reset();const uint32_t t=admitPhone();(void)t;tickFlush(clockMs+=10);assert(!phoneGatt.lastPressure);
  for(int i=0;i<3;++i){
   const uint32_t a=submitAccessory(i);tickFlush(clockMs+=10);assert(phoneGatt.lastPressure && !phoneGatt.lastConsoleBusy);
   static const BleSetupClient clients[]={BleSetupClient::G2,BleSetupClient::RingRetire,BleSetupClient::Ring};
   assert(bleSetupWithdraw(clients[i],a)==BleSetupWithdraw::Withdrawn);tickFlush(clockMs+=10);assert(!phoneGatt.lastPressure);}}
}

// ---- The pipe task's exit: phonePipeWorkerRelease and leaveIfReleased ----------------------
static void workerReleaseTests() {
 constexpr uint32_t kRelease=1000;
 // leaveIfReleased: a pending release leaves only an inactive pipe with no
 // reset pending, and consumes the request in the step that nulls the handle.
 {reset();assert(!leaveIfReleased(false) && worker);
  exitWanted=true;assert(!leaveIfReleased(false) && worker && exitWanted);                     // active
  active=false;hostResetWanted=true;assert(!leaveIfReleased(false) && worker && exitWanted);  // a reset pending
  hostResetWanted=false;assert(leaveIfReleased(false) && !worker && !exitWanted);
  assert(!leaveIfReleased(false) && !worker);}
 // The idle exit's own ground, through the same step: never active, never
 // with a reset pending; no request is consumed by it.
 {reset();assert(!leaveIfReleased(true) && worker && !exitWanted);
  active=false;hostResetWanted=true;assert(!leaveIfReleased(true) && worker);
  hostResetWanted=false;assert(leaveIfReleased(true) && !worker && !exitWanted);}
 // The idle exit on an up host: a stopped pipe whose step machine is idle
 // again (GATT adapter idle, app id released, nothing pending) leaves
 // kPipeIdleRetireMs after the pass that found it so, on its own: no release
 // asked, no host reset, the final summary "stopped". A release afterwards has
 // nothing to do; the host stop later finds no task and nothing to reset.
 {reset();halStopped=false;active=false;status="stopped";playWorker=nullptr;
  phonePass();assert(idleOnHost && worker==&phoneTaskHandle && !phoneTaskGone && !exitWanted);
  clockMs+=kPipeIdleRetireMs-1;phonePass();assert(worker==&phoneTaskHandle && !phoneTaskGone);
  clockMs+=2;phonePass();
  assert(!worker && phoneTaskGone && !exitWanted && !hostResetWanted && !phoneGatt.hostStops && taskExits.size()==1);
  assert(!std::strcmp(taskExits[0].status,"stopped") && !taskExits[0].summary.active);
  assert(phonePipeWorkerRelease(kRelease) && taskExits.size()==1 && !phoneGatt.hostStops);
  halStopped=true;assert(phonePipeHostStopped() && !phoneGatt.hostStops);
  const unsigned passes=phonePasses;phonePass();assert(phonePasses==passes && taskExits.size()==1);}
 // Not idle: an app id still held, a job or an active pipe keeps the task
 // (the pass publishes the whole predicate; the other terms are pinned by
 // the test's text check), as does a phone setup turn claimed once the pipe
 // is idle. Each clears to idle again.
 {reset();halStopped=false;active=false;playWorker=nullptr;
  auto stays=[&]{clockMs+=kPipeIdleRetireMs+1;phonePass();assert(worker==&phoneTaskHandle && !phoneTaskGone);};
  nativeAppId=5;stays();assert(!idleOnHost);nativeAppId=0xffff;
  job=true;stays();assert(!idleOnHost);job=false;
  active=true;stays();assert(!idleOnHost);active=false;
  phonePass();assert(idleOnHost && worker==&phoneTaskHandle);
  uint32_t t=0;assert(bleSetupClaim(kPhone,&t) && !phoneSlotIdle());
  stays();assert(idleOnHost && worker==&phoneTaskHandle);  // the slot, not the pipe, holds it
  bleSetupCancelClaim(kPhone,t);assert(phoneSlotIdle());
  clockMs+=1;phonePass();assert(!worker && phoneTaskGone && taskExits.size()==1);}
 // No task: nothing to release and nothing asked of the host.
 {reset();worker=nullptr;
  assert(phonePipeWorkerRelease(kRelease) && !exitWanted && taskExits.empty() && !hostResetWanted && !phoneGatt.hostStops);}
 // An active pipe: refused at once, no reset, no request, no pass.
 {reset();playWorker=phonePass;
  assert(!phonePipeWorkerRelease(kRelease) && active && worker==&phoneTaskHandle && !exitWanted);
  assert(taskExits.empty() && !hostResetWanted && !phoneGatt.hostStops && !phonePasses);}
 // The host still up: refused (the reset cannot run), the task kept.
 {reset();active=false;halStopped=false;playWorker=phonePass;
  assert(!phonePipeWorkerRelease(kRelease) && worker==&phoneTaskHandle && !exitWanted);
  assert(taskExits.empty() && !hostResetWanted && !phoneGatt.hostStops && !phonePasses);}
 // The reset not yet run on the task (it is not ticking): refused after the
 // host-stop wait with the request never raised. Once it ticks, the adapter's
 // hostStopped runs on the task, with it still published and before the
 // request, and a later pass leaves.
 {reset();active=false;playWorker=nullptr;const uint32_t asked=clockMs;
  assert(!phonePipeWorkerRelease(kRelease) && worker==&phoneTaskHandle && !exitWanted && !phoneTaskGone);
  assert(clockMs==asked+1000 && hostResetWanted && !phoneGatt.hostStops && taskExits.empty());
  unsigned resetPass=0;
  phoneGatt.onHostStopped=[&]{resetPass=phonePasses;assert(worker==&phoneTaskHandle && !exitWanted && ownerDepth==1);};
  playWorker=phonePass;
  assert(phonePipeWorkerRelease(kRelease) && !worker && phoneTaskGone && !exitWanted && !hostResetWanted);
  assert(phoneGatt.hostStops==1 && resetPass==1 && taskExits.size()==1 && taskExits[0].hostStops==1 && taskExits[0].pass>resetPass);}
 // The happy path: a stopped pipe on a torn-down host. The reset pass gives
 // gBLEState back and publishes the final "stopped" summary; the leave pass
 // follows it; the task nulls its own handle, so the worker's TurnEnv has
 // nothing to raise afterwards and nothing can queue a phone turn.
 {reset();static int handle;mutex=&handle;active=false;stateHeld=true;linkSummary.active=linkSummary.ready=true;
  status="disconnected";playWorker=phonePass;unsigned resetPass=0;phoneGatt.onHostStopped=[&]{resetPass=phonePasses;};
  assert(phonePipeWorkerRelease(kRelease));
  assert(!worker && !exitWanted && phoneTaskGone && taskExits.size()==1 && phoneGatt.hostStops==1 && resetPass==1);
  const TaskExit& e=taskExits[0];
  assert(!e.handle && e.hostStops==1 && e.pass>resetPass && !e.summary.active && !e.summary.ready && !std::strcmp(e.status,"stopped"));
  PhonePipeLinkSummary s;assert(phonePipeLinkSummary(&s) && !s.active && !s.ready && statusIs("stopped") && !active && !ready);
  assert(stateReleases==1 && !stateHeld && !pipeAdmissionEnabled && !recoveryBlocked);
  TurnEnv env;const size_t sets=prioritySets.size();env.grant(44);env.release(44);
  assert(prioritySets.size()==sets && turn.granted==44 && phonePriority==1);
  // The host is down with the worker gone (bleStackDown released it): nothing to open on.
  stackReady=false;assert(admit(BlePeerConnectRequest{})==BlePeerConnectAdmission::ROLE_BLOCKED && phoneSlotIdle());
  // A second release: a no-op, nothing asked of the host again.
  assert(phonePipeWorkerRelease(kRelease) && !worker && taskExits.size()==1 && phoneGatt.hostStops==1 && !hostResetWanted);
  assert(phonePipeHostStopped() && phoneGatt.hostStops==1);  // no task: nothing to reset
  // The task never runs again.
  const unsigned passes=phonePasses;phonePass();assert(phonePasses==passes && taskExits.size()==1);mutex=nullptr;}
 // A phone setup job still claimed (its turn would set the task's priority by
 // handle): refused after the reset, the task kept; released once the slot
 // is idle.
 {reset();active=false;playWorker=phonePass;uint32_t t=0;assert(bleSetupClaim(kPhone,&t) && !phoneSlotIdle());
  assert(!phonePipeWorkerRelease(kRelease) && worker==&phoneTaskHandle && !exitWanted && taskExits.empty() && phoneGatt.hostStops==1);
  bleSetupCancelClaim(kPhone,t);assert(phoneSlotIdle());
  assert(phonePipeWorkerRelease(kRelease) && !worker && taskExits.size()==1 && phoneGatt.hostStops==2);}
 // A starved task (the reset ran, then it never got the CPU again): at the
 // timeout the request is withdrawn in the step that keeps the task, so a
 // late pass stays; asked again with the task running, it leaves.
 {reset();active=false;playWorker=[]{if(hostResetWanted.load())phonePass();};const uint32_t asked=clockMs;
  assert(!phonePipeWorkerRelease(300) && worker==&phoneTaskHandle && !exitWanted && !phoneTaskGone);
  assert(clockMs==asked+10+300 && phoneGatt.hostStops==1 && phonePasses==1 && taskExits.empty());
  playWorker=nullptr;phonePass();assert(worker==&phoneTaskHandle && !phoneTaskGone && taskExits.empty() && phonePasses==2);
  playWorker=phonePass;assert(phonePipeWorkerRelease(kRelease) && !worker && phoneTaskGone && taskExits.size()==1 && phoneGatt.hostStops==2);
  // A zero timeout: one check, no wait; the task has had no pass.
  reset();active=false;playWorker=phonePass;phoneGatt.onHostStopped=nullptr;
  const uint32_t at=clockMs;assert(!phonePipeWorkerRelease(0) && worker==&phoneTaskHandle && !exitWanted && clockMs==at+10);
  assert(phoneGatt.hostStops==1 && taskExits.empty());}
}

// ---- Turns on the real worker, end to end -------------------------------------------------
static void turnTests() {
 // A whole turn: the phone task alone never starts; the worker raises
 // phone_pipe to 5, then grants; the phone consumes once, scans, links and
 // gives the turn back at READY; the worker restores priority 1 at once.
 {reset();world.on=true;world.linkMs=500;
  const uint32_t t=admitPhone();const uint32_t admittedAt=jobAt;
  simulate(200);
  assert(step==Step::Waiting&&!setupHeld&&events.empty()&&!turn.granted&&prioritySets.empty());
  uint32_t readyAt=0;bool turnPressure=false;
  onStep=[&]{if(setupHeld)turnPressure|=phoneGatt.lastPressure;if(ready&&!readyAt)readyAt=clockMs;};
  const uint32_t servedAt=clockMs;serveWorker();
  assert(readyAt&&ready&&published&&step==Step::Link&&clockMs==readyAt);  // the worker returned with the give-back
  assert(grantAt==servedAt+10&&gateWaitMs==grantAt-admittedAt);
  assert(!setupHeld&&!heldToken&&!wantToken&&turn.ended==t&&!turn.granted);
  assert(phoneWakes.size()==1&&phoneWakes[0].at==readyAt&&phoneWakes[0].ended==t);
  assert(prioritySets.size()==2&&prioritySets[0].priority==5&&!prioritySets[0].granted&&!prioritySets[0].ended);
  assert(prioritySets[1].priority==1&&!prioritySets[1].granted&&prioritySets[1].ended==t&&phonePriority==1);
  assert(!turnPressure&&!logged("overran")&&phoneSlotIdle()&&countEvents("scan-params")==1);
  assert(coexLog.connectTotal==1&&lastConnect().outcome==O::Connected&&lastConnect().waitMs==grantAt-admittedAt);
  // The worker logs the phone's wait for other setups; the phone keeps no accessory wait.
  assert(setupWaitLogs.size()==1&&setupWaitLogs[0].first=="phone"&&setupWaitLogs[0].second==200);
  assert(accessoryWaits.empty()&&!accessoryWaitClears);
  // The session outlives the turn; its later end gives nothing back again.
  onStep=nullptr;phoneGatt.failNext=true;tickFlush(clockMs+=10);tickFlush(clockMs+=10);
  assert(!ready&&step==Step::Idle&&phoneWakes.size()==1&&withdrawCalls==0);}
 // F2: G2 mode admits the phone with both temples down; the glasses' setup
 // goes first, and the phone reserves the arms' capacity when its turn comes.
 {reset();world.on=true;world.linkMs=100;fakes[0].runMs=100;submitAccessory(0);BlePeerConnectRequest wanted;
  assert(admit(wanted)==BlePeerConnectAdmission::STARTED&&job&&!enrolling&&step==Step::Waiting&&!armQueries);
  tickFlush(clockMs);assert(step==Step::Waiting&&!setupHeld);
  bool seen=false;
  onStep=[&]{if(setupHeld&&!seen){seen=true;
   assert(lastReserveArms&&capacityReservations==1&&step==Step::ScanParams&&events.back()=="scan-params"&&fakes[0].finishes==1);}};
  serveWorker();assert(seen&&ready&&fakes[0].runs==1);}
 {reset();world.on=true;world.linkMs=100;admitPhone();bool seen=false;
  onStep=[&]{if(setupHeld&&!seen){seen=true;assert(lastReserveArms);}};  // the arms keep their links wherever glasses can open (ENABLE_G2_GLASSES)
  serveWorker();assert(seen);}
 // No link capacity at the grant: Idle, and the turn goes straight back.
 {reset();capacityAvailable=false;const uint32_t t=admitPhone();const uint32_t servedAt=clockMs;serveWorker();
  assert(statusIs("central link capacity unavailable")&&step==Step::Idle&&!job&&!setupHeld&&!wantToken);
  assert(turn.ended==t&&!turn.granted&&phoneWakes.size()==1&&clockMs==servedAt+10&&phonePriority==1);
  assert(lastReserveArms&&!capacityReservations&&!registrationCount&&events.empty()&&!recoveryNeeded&&!logged("overran"));}
 // R2: an accessory job submitted during discovery never cancels the
 // phone's setup; it is pressure, and it runs once the phone's turn ends.
 {reset();world.on=true;
  const uint32_t t=admitPhone();uint32_t glasses=0;unsigned discovery=0;bool pressureAlone=true,pressureWhileRunning=false;uint32_t readyAt=0;
  fakes[0].runMs=100;
  onStep=[&]{
   if(step==Step::Link&&phoneGatt.state==Gatt::Phase::Opening){
    if(!glasses){pressureAlone=phoneGatt.lastPressure;glasses=submitAccessory(0);return;}
    assert(phoneGatt.lastPressure);
    if(++discovery==50){
     assert(coexLog.connectTotal==0&&step==Step::Link&&job&&setupHeld&&!fakes[0].runs&&!phoneGatt.stops&&phoneWakes.empty());
     phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;}
   }
   if(ready&&!readyAt)readyAt=clockMs;
   if(fakes[0].runs&&!fakes[0].finishes)pressureWhileRunning|=phoneGatt.lastPressure;
  };
  serveWorker();
  assert(glasses&&!pressureAlone&&pressureWhileRunning&&readyAt&&ready&&phoneWakes.size()==1&&turn.ended==t);
  assert(fakes[0].runs==1&&fakes[0].ranAt==readyAt&&fakes[0].finishes==1);
  assert(coexLog.connectTotal==1&&lastConnect().outcome==O::Connected&&lastConnect().detail==D::None);
  tickFlush(clockMs+=10);assert(!phoneGatt.lastPressure);}
 // R7: wait_ms is admission to grant, here behind a glasses setup; a wait past
 // the record's range is capped and printed as a lower bound, not wrapped.
 for(const auto& c:{std::pair<uint32_t,uint32_t>{1224,1234},std::pair<uint32_t,uint32_t>{69990,70000}}){
  reset();world.on=true;world.stage=S::Search;fakes[0].runMs=c.first;submitAccessory(0);
  const uint32_t t=admitPhone();const uint32_t admittedAt=jobAt;const bool small=c.first<2000;
  onStep=[&]{if(step==Step::Link&&phoneGatt.state==Gatt::Phase::Opening&&!phoneGatt.failNext&&!phoneGatt.concluded){
   phoneGatt.serviceChangedFlag=small;phoneGatt.failNext=true;phoneGatt.failError=small?Error::Protocol:Error::Connect;}};
  serveWorker();
  assert(fakes[0].runs==1&&gateWaitMs==c.second&&grantAt-admittedAt==c.second&&turn.ended==t&&phoneWakes.size()==1);
  assert(setupWaitLogs.size()==2&&setupWaitLogs[1].first=="phone"&&setupWaitLogs[1].second==c.first);
  assert(accessoryWaits.size()==1&&accessoryWaitClears==1);  // the glasses' own record wait only
  assert(coexLog.connectTotal==1);
  const auto& r=lastConnect();char line[400];hw1coex::Log::connectBody(line,sizeof(line),r);
  if(small){
   assert(r.detail==D::ServiceChanged&&r.cause==uint8_t(Error::Protocol)&&r.stage==S::Search&&r.waitMs==1234);
   assert(std::strstr(line," stage=search gatt=- reason=0x00(-) cause=1(service-changed) wait_ms=1234 wifi="));
  } else {
   assert(r.waitMs==hw1coex::ConnectRecord::kWaitCap&&std::strstr(line," wait_ms=65534+ wifi="));
  }}
 // Picked, granted, but stopped or replaced before the phone consumed the
 // grant: the phone takes the running job back (withdraw says Running),
 // clears the grant and ends the turn, so the worker moves on at once.
 for(int cause=0;cause<2;++cause){reset();const uint32_t t=admitPhone();
  onPriority=[cause](UBaseType_t p){if(p!=5)return;if(cause==0)stopWanted=true;else ++authorityIntent;};
  const uint32_t servedAt=clockMs;serveWorker();
  assert(withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::Running&&turn.ended==t&&!turn.granted);
  assert(phoneWakes.size()==1&&phoneWakes[0].ended==t&&!phoneWakes[0].granted);
  assert(!setupHeld&&events.empty()&&step==Step::Idle&&clockMs==servedAt+10&&!logged("overran"));
  assert(prioritySets.size()==2&&phonePriority==1&&phoneSlotIdle()&&!wantToken);
  tickFlush(clockMs+=10);assert(withdrawCalls==1&&phoneWakes.size()==1);}
 // Overrun: a setup still running at 75 s has its grant revoked; the
 // phone's next tick fails it with Deadline and the cleanup gives the turn back.
 {reset();world.on=true;const uint32_t t=admitPhone();const uint32_t servedAt=clockMs;serveWorker();
  assert(clockMs==servedAt+75010&&logged("BLE_SETUP: phone turn overran 75 s; stopping the phone setup"));
  assert(!logged("host recovery requested")&&!recoveryNeeded&&!recoveryBlocked&&blockCalls.empty());
  assert(phoneGatt.stops==1&&phoneGatt.stopError==Error::Deadline&&phoneGatt.stopAt==servedAt+75010);
  assert(coexLog.connectTotal==1&&lastConnect().cause==uint8_t(Error::Deadline)&&lastConnect().outcome==O::Failed);
  assert(step==Step::Idle&&!job&&!setupHeld&&turn.ended==t&&!turn.granted&&phoneWakes.size()==1&&phonePriority==1);}
 // The worker's escalation races the phone's clean end, between the phone's
 // first recovery check and its give-back: the second check settles it.
 {reset();world.on=true;const uint32_t t=admitPhone();bool armed=false,fired=false;
  onLinkRelease=[&]{if(setupHeld)armed=true;};
  onRoute=[&]{if(armed&&!fired&&setupHeld){fired=true;TurnEnv env;env.escalate(t);}};
  serveWorker();
  assert(fired&&!recoveryNeeded&&!recoveryBlocked&&step==Step::Idle&&!setupHeld&&turn.ended==t);
  assert(blockCalls.size()==2&&blockCalls[0].value&&!blockCalls[1].value);}
 // The phone task stalls past the turn: the worker revokes at 75 s, waits
 // 10 s, then escalates: the block first, then recoveryNeeded.
 {reset();world.on=true;const uint32_t t=admitPhone();stallOnceLinked();const uint32_t servedAt=clockMs;serveWorker();
  assert(clockMs==servedAt+85000);
  assert(logged("BLE_SETUP: phone turn overran 75 s; stopping the phone setup")&&logged("BLE_SETUP: phone setup did not stop; host recovery requested"));
  assert(blockCalls.size()==1&&blockCalls[0].value&&!blockCalls[0].recoveryNeeded&&recoveryNeeded&&recoveryBlocked);
  assert(setupHeld&&!turn.granted&&phonePriority==1&&phoneSlotIdle()&&phoneWakes.empty());
  // The phone resumes: the revoked grant stops it at once, and the clean end
  // lifts both flags before the turn is given back.
  onStep=nullptr;phoneTaskRuns=true;phoneTaskStep();
  assert(phoneGatt.stopError==Error::Deadline&&phoneGatt.stopAt==clockMs&&step==Step::Idle&&!setupHeld&&turn.ended==t);
  assert(!recoveryNeeded&&!recoveryBlocked&&blockCalls.size()==2&&!blockCalls[1].value&&blockCalls[1].setupHeld);
  assert(phoneWakes.size()==1&&!phoneWakes[0].blocked);}
 // ...the phone's clean end lands just before the worker's block: the worker's
 // own settle undoes the escalation (never one flag without the other).
 {reset();world.on=true;const uint32_t t=admitPhone();stallOnceLinked();
  onBlock=[]{onStep=nullptr;phoneTaskRuns=true;phoneTaskStep();};
  serveWorker();
  assert(turn.ended==t&&!setupHeld&&step==Step::Idle&&phoneWakes.size()==1);
  assert(!recoveryNeeded&&!recoveryBlocked);
  assert(blockCalls.size()==2&&blockCalls[0].value&&!blockCalls[0].recoveryNeeded&&!blockCalls[1].value);}
 // ...the phone's clean end lands just after the escalation: the phone lifts
 // both, and settle finds nothing left to undo.
 {reset();world.on=true;const uint32_t t=admitPhone();stallOnceLinked();
  onLog=[](const std::string& line){if(line.find("host recovery requested")==std::string::npos)return;
   onLog=nullptr;onStep=nullptr;phoneTaskRuns=true;phoneTaskStep();};
  serveWorker();
  assert(turn.ended==t&&!recoveryNeeded&&!recoveryBlocked);
  assert(blockCalls.size()==2&&blockCalls[0].value&&!blockCalls[1].value&&blockCalls[1].setupHeld);}
 // ...or host recovery comes first: quiesce gives the held turn back, the
 // route disabled before the end; the host reset settles the recovery.
 {reset();world.on=true;const uint32_t t=admitPhone();stallOnceLinked();serveWorker();
  assert(recoveryNeeded&&setupHeld);
  onStep=nullptr;phoneTaskRuns=true;playWorker=[]{tickFlush(clockMs);};
  assert(phonePipeQuiesceForHostRecovery()&&quiesced&&!active);
  assert(!setupHeld&&!wantToken&&turn.ended==t&&phoneWakes.size()==1&&!phoneWakes[0].routeEnabled);
  assert(phonePipeHostStopped()&&!recoveryNeeded&&!recoveryBlocked&&!quiesced&&step==Step::Idle&&phoneWakes.size()==1);
  playWorker=nullptr;}
 // A grant never consumed (the phone task stalled before it ticked): revoked
 // at 75 s with no escalation; the phone, back, cannot consume the revoked
 // grant and its backstop gives back the one-shot reseek.
 {reset();BlePeerConnectRequest wanted;wanted.explicitReseek=true;const uint32_t t=admitPhone(wanted);
  phoneTaskRuns=false;const uint32_t servedAt=clockMs;serveWorker();
  assert(clockMs==servedAt+75000&&logged("BLE_SETUP: phone turn overran 75 s, unused")&&!logged("host recovery requested"));
  assert(!recoveryNeeded&&!recoveryBlocked&&!turn.granted&&phonePriority==1&&phoneSlotIdle()&&step==Step::Waiting&&job);
  phoneTaskRuns=true;phoneTaskStep();
  assert(step==Step::Idle&&statusIs("setup turn ended unused")&&!job&&!wantToken&&!setupHeld&&events.empty());
  assert(reseekRestores==1&&reseekIntent==intent&&reseekIdentity==identityGeneration&&!bleSetupLive(kPhone,t));
  BlePeerConnectRequest again;assert(admit(again)==BlePeerConnectAdmission::STARTED);}
 // Enrollment held: 60 s from the grant (not from the admission, here 30 s
 // earlier behind the glasses); grant+59.99 s still runs.
 {reset();world.on=true;fakes[0].runMs=30000;submitAccessory(0);const uint32_t t=enrollPhone();const uint32_t enrolledAt=jobAt;
  serveWorker();
  assert(fakes[0].runs==1&&grantAt==enrolledAt+30010);
  assert(phoneGatt.stops==1&&phoneGatt.stopError==Error::Cancelled&&phoneGatt.stopAt==grantAt+60000);
  assert(coexLog.connectTotal==1&&lastConnect().detail==D::Expired&&lastConnect().purpose==hw1coex::Purpose::Enroll);
  assert(!setupHeld&&turn.ended==t&&step==Step::Idle&&!enrolling&&!logged("overran")&&phonePriority==1);}
 // Enrollment queued: at most 60 s waiting for its turn; taken back unrun.
 {reset();fakes[0].runMs=70000;submitAccessory(0);const uint32_t t=enrollPhone();const uint32_t enrolledAt=jobAt;
  serveWorker();
  assert(fakes[0].runs==1&&phoneGatt.stops==1&&phoneGatt.stopAt==enrolledAt+60000&&phoneGatt.stopError==Error::Cancelled);
  assert(prioritySets.empty()&&!turn.granted&&withdrawCalls==1&&lastWithdraw==BleSetupWithdraw::Withdrawn);
  assert(phoneSlotIdle()&&!bleSetupLive(kPhone,t)&&step==Step::Idle&&!job&&!enrolling);}
 // A cleanup past its bound: Fault, recovery requested, the block set while
 // the turn is still held, then the turn given back (the worker moves on);
 // later quiesce and host reset finish the recovery.
 {reset();world.on=true;gattStuck=true;const uint32_t t=admitPhone();uint32_t failAt=0;bool heldAtBound=false;
  onStep=[&]{
   if(!failAt&&step==Step::Link&&phoneGatt.state==Gatt::Phase::Opening){failAt=clockMs;fail(clockMs,Error::Deadline,"stuck phone");}
   if(failAt&&clockMs==failAt+3990)heldAtBound=setupHeld&&!recoveryNeeded;};
  serveWorker();
  assert(heldAtBound&&clockMs==failAt+4000&&step==Step::Fault&&recoveryNeeded&&recoveryBlocked&&!ready);
  assert(!setupHeld&&turn.ended==t&&phoneWakes.size()==1&&phoneWakes[0].blocked&&phonePriority==1&&!logged("overran"));
  assert(blockCalls.size()==1&&blockCalls[0].value&&blockCalls[0].setupHeld);
  assert(phoneGatt.phase()==Gatt::Phase::Quarantined&&capacityReleases==0);
  onStep=nullptr;clockMs+=10000;tickFlush(clockMs);assert(recoveryNeeded&&capacityReleases==0&&phoneWakes.size()==1);
  playWorker=[]{tickFlush(clockMs);};
  assert(phonePipeQuiesceForHostRecovery()&&quiesced&&!active&&phoneWakes.size()==1);
  assert(phonePipeHostStopped());
  assert(!recoveryNeeded&&!recoveryBlocked&&!quiesced&&phoneGatt.idle()&&capacityReleases==1&&!pipeAdmissionEnabled);
  playWorker=nullptr;}
}

// ---- The Idle label: an attempt that never reached READY keeps its reason ----------------
static void idleLabelTests() {
 {reset();runScan(0,30);drainGap();assert(statusIs("phone not found")&&step==Step::Retiring);
  for(int i=0;i<3&&step!=Step::Idle;++i)tickFlush(clockMs+=10);
  assert(step==Step::Idle&&statusIs("phone not found")&&!published);}
 // A link that was READY reads "disconnected" once it is gone, or "stopped".
 for(int stop=0;stop<2;++stop){reset();pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;phoneGatt.state=Gatt::Phase::Pipe;
  phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);assert(ready&&published&&statusIs("ready"));
  if(stop)stopWanted=true;
  fail(clockMs,Error::Connect,"phone link lost");
  for(int i=0;i<3&&step!=Step::Idle;++i)tickFlush(clockMs+=10);
  assert(step==Step::Idle&&statusIs(stop?"stopped":"disconnected"));}
 // An open that fails before READY keeps "phone link lost".
 {reset();runScan(1,0);drainGap();holdTurn();
  phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;phoneGatt.failReason=0x3e;tickFlush(clockMs);
  assert(statusIs("phone link lost")&&!published);
  scanEvent(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);drainGap();
  for(int i=0;i<3&&step!=Step::Idle;++i)tickFlush(clockMs+=10);
  assert(step==Step::Idle&&statusIs("phone link lost"));}
}

// ---- S4/S8: the app console's view of the pipe ------------------------------------------
// BLE_ConsoleAttachCore.h attaches the console to the link this record names:
// set at READY with the session's generation and link id, cleared by every
// end, and copied without the owner mutex. Console traffic counts as activity.
static void linkRecordTests() {
 PhonePipeLinkRecord r{};
 const auto has=[&](uint32_t generation,uint16_t connId){
  r={0xdead,0xbeef};return phonePipeLinkRecord(&r)&&r.generation==generation&&r.connId==connId&&phonePipeLinkRecord(nullptr);};
 const auto none=[&]{r={0xdead,0xbeef};return !phonePipeLinkRecord(&r)&&!r.generation&&!r.connId&&!phonePipeLinkRecord(nullptr);};
 const auto readyNow=[&]{reset();pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;phoneGatt.conn=1;
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);assert(ready);};
 // Not before READY: the pipe's client holds the link but has not authenticated.
 {reset();pendingEnrollment();phoneGatt.conn=1;tickFlush(clockMs);assert(!ready&&none());}
 // READY: this session's generation and link id, from the READY pass on. The
 // copy takes one leaf lock and never the owner mutex (BTC callbacks read it).
 {readyNow();assert(has(phoneGatt.gen,1));
  const unsigned locks=ownerLocks,crits=critEntries;assert(phonePipeLinkRecord(&r));
  assert(ownerLocks==locks&&critEntries==crits+1);
  for(int i=0;i<5;++i){tickFlush(clockMs+=10);assert(has(phoneGatt.gen,1));}}
 // Every end clears it. A stop from a command task (fail() under the owner
 // mutex) clears it at once, before the next pass.
 {readyNow();disconnect();assert(!ready&&none());tickFlush(clockMs+=10);assert(none());}
 // The link lost: cleared by the pass that sees the loss. Until then the record
 // stays, and the console's linkChanged() closes that generation on the drop.
 {readyNow();phoneGatt.state=Gatt::Phase::Retiring;assert(has(phoneGatt.gen,1));
  tickFlush(clockMs+=10);assert(!ready&&none()&&phoneGatt.stops==1&&statusIs("disconnected"));}
 {readyNow();phoneGatt.failNext=true;tickFlush(clockMs+=10);assert(!ready&&none());}
 // phone stop, the intent replaced, a stream cleanup or the callback queue failing.
 for(int cause=0;cause<4;++cause){readyNow();
  if(cause==0)stopWanted=true;
  if(cause==1)++authorityIntent;
  if(cause==2)abortLink=true;
  if(cause==3)gapOverflow=true;
  tickFlush(clockMs+=10);assert(!ready&&none());
  for(int i=0;i<3&&step!=Step::Idle;++i)tickFlush(clockMs+=10);
  assert(step==Step::Idle&&none()&&active==(cause!=0));}
 // Host reset and quiesce.
 {readyNow();hostResetWanted=true;tickFlush(clockMs+=10);assert(!ready&&!active&&step==Step::Idle&&none());}
 {readyNow();recoveryNeeded=true;quiesceWanted=true;tickFlush(clockMs+=10);assert(quiesced&&!ready&&none());}
 // A new session (a new generation, another link id) gets only its own
 // record, at its own READY; nothing of the last one is ever visible meanwhile.
 {reset();world.on=true;world.linkMs=100;phoneGatt.conn=1;admitPhone();serveWorker();
  const uint32_t first=phoneGatt.gen;assert(ready&&has(first,1));
  phoneGatt.failNext=true;tickFlush(clockMs+=10);tickFlush(clockMs+=10);assert(step==Step::Idle&&none());
  phoneGatt.conn=3;bool linking=false;
  onStep=[&]{if(!ready){assert(none());linking|=step==Step::Link;}};
  BlePeerConnectRequest again;again.intentGeneration=authorityIntent;again.identityGeneration=authorityIdentity;
  admitPhone(again);serveWorker();onStep=nullptr;
  assert(linking&&ready&&phoneGatt.gen==first+1&&has(first+1,3));}
 // S8: console traffic is stream activity (A-4). The idle link asks for the
 // active parameters, stays active while traffic continues, and asks for idle
 // 30 s after the last of it. The note itself takes no lock.
 const auto paramsEvent=[&](int status,uint16_t interval){GapEvent e;e.kind=GapKind::Params;e.status=status;e.interval=interval;consume(e,clockMs);};
 {readyNow();assert((paramRequests==std::vector<int>{72}));paramsEvent(0,80);assert(params.applied&&!params.active);
  const unsigned locks=ownerLocks,crits=critEntries;phonePipeNoteConsoleActivity();
  assert(ownerLocks==locks&&critEntries==crits&&lastStreamAt==clockMs-kParamsActiveHoldMs);  // counted for the owner's next pass
  tickFlush(clockMs+=1000);assert(params.wanted&&lastStreamAt==clockMs&&paramRequests.size()==1);  // 5 s since the last request
  tickFlush(clockMs+=4000);assert((paramRequests==std::vector<int>{72,24}));paramsEvent(0,30);assert(params.active);
  for(int i=0;i<10;++i){phonePipeNoteConsoleActivity();tickFlush(clockMs+=2000);}
  assert((paramRequests==std::vector<int>{72,24})&&params.active&&params.wanted);
  tickFlush(clockMs+=29999);assert(paramRequests.size()==2&&params.wanted);
  tickFlush(clockMs+=1);assert((paramRequests==std::vector<int>{72,24,72})&&!params.wanted);
  paramsEvent(0,80);assert(params.applied&&!params.active);}
 // Traffic noted before READY (the last session's replies) never counts for
 // the new link, which starts idle; outside READY a note changes nothing.
 {reset();phonePipeNoteConsoleActivity();tickFlush(clockMs+=10);assert(paramRequests.empty()&&!lastStreamAt);
  pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;for(int i=0;i<3;++i)phonePipeNoteConsoleActivity();
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;tickFlush(clockMs);
  assert(ready&&lastStreamAt==clockMs-kParamsActiveHoldMs&&(paramRequests==std::vector<int>{72}));}
}

// ---- Phone link v2: the owner's side ---------------------------------------------------
// A whole v2 session to READY on the owner loop: an enrollment holding its
// turn, the secure channel done, the HELLO (with the P4's flags) out and the
// fake phone's HELLO_ACK with `phone`. Idle parameters requested and applied.
static void linkUp(uint8_t phone,uint8_t p4=0){
 reset();consoleFlags=p4;pendingEnrollment();lastStreamAt=clockMs-kParamsActiveHoldMs;phoneFlags=phone;enterPipe();
 for(int i=0;i<5 && !ready;++i)tickFlush(clockMs+=10);
 assert(ready && published && phoneSession.state==hw1pipe::State::Ready && consoleTicks.back().ready);
 GapEvent e;e.kind=GapKind::Params;e.status=0;e.interval=80;consume(e,clockMs);
 assert((paramRequests==std::vector<int>{72}) && params.applied && !params.active);
}
// One more attempt on the same Session (no reset: its HELLO latch carries
// over): the job holds its turn, the secure channel is done and the next poll
// sends the HELLO, which the 2.7.x-like phone never answers. `ends` then ends
// the session; the owner retires it to Idle.
static void pipeAttempt(const std::function<void()>& ends){
 job=candidate=true;published=enrolling=false;holdTurn();step=Step::Link;status="authenticating pipe";
 std::memset(phoneGatt.entered,0,sizeof(phoneGatt.entered));phoneGatt.state=Gatt::Phase::Secure;phoneSilent=true;enterPipe();
 tickFlush(clockMs+=10);assert(phoneSession.state==hw1pipe::State::Hello && !ready && helloFlags.size());
 ends();
 for(int i=0;i<400 && step!=Step::Idle;++i)tickFlush(clockMs+=10);
 assert(step==Step::Idle && !ready && !job);
}
static void v2Tests() {
 using hw1pipe::Op;using St=PhonePipeOpenStatus;const auto npos=std::string::npos;
 static int handle;
 // releaseIds: one per slot; every reset clears them all.
 {static_assert(sizeof(releaseIds)/sizeof(releaseIds[0])==hw1pipe::kMaxStreams,"one parked id per stream slot");
  reset();for(auto& id:releaseIds)id=77;runScan(1,0);drainGap();
  assert(step==Step::Link && events.back()=="gatt-begin");for(auto id:releaseIds)assert(!id);   // GATT begin
  for(auto& id:releaseIds)id=77;hostResetWanted=true;tickFlush(clockMs+=10);
  assert(!active);for(auto id:releaseIds)assert(!id);}                                         // host reset
 // End reasons by code: 16, 17 and 18 have their own bucket (no &15 fold onto
 // 0..2); a code the pipe does not define counts as 0, never as a known one.
 for(const auto& c:{std::pair<uint16_t,uint16_t>{16,16},{17,17},{18,18},{2,2},{40,0},{0xffff,0}}){
  linkUp(0);fail(clockMs,Error(c.first),"phone link lost");
  assert(sessionStats.ended==1 && sessionStats.endReason[c.second]==1 && sessionStats.lastEnd==c.second);
  for(unsigned i=0;i<19;++i)assert(sessionStats.endReason[i]==(i==c.second?1u:0u));
  tickFlush(clockMs+=10);PhonePipeLinkSummary sum;mutex=&handle;assert(phonePipeLinkSummary(&sum));mutex=nullptr;
  assert(sum.ended==1 && sum.lastEnd==c.second && sum.endReason[c.second]==1 && !sum.ready);}
 // The pipe task's pass: flags before the poll (the HELLO carries them), the
 // console driver after the step machine under the owner mutex, its row work
 // after it; a stopped pipe's passes reach the driver too.
 {linkUp(hw1pipe::kPhoneConsoleWanted,hw1pipe::kP4ConsoleOffered);
  assert(helloFlags.size()==1 && helloFlags[0]==hw1pipe::kP4ConsoleOffered && phoneSession.localFlags()==1);
  assert(consoleTicks.back().generation==phoneGatt.generation() && consoleTicks.back().session==hw1pipe::State::Ready);
  assert(afterUnlocks==consoleTicks.size() && !linkStates.size());
  consoleFlags=0;tickFlush(clockMs+=10);   // bleconsole off: a LINK_STATE on the next poll
  assert(phoneSession.localFlags()==0 && (linkStates==std::vector<uint8_t>{0}));
  phoneGatt.failNext=true;tickFlush(clockMs+=10);   // the pass that ends READY shows the end
  assert(!ready && !consoleTicks.back().ready && consoleTicks.back().session!=hw1pipe::State::Ready);
  hostResetWanted=true;const size_t n=consoleTicks.size();tickFlush(clockMs+=10);
  assert(!active && consoleTicks.size()==n+1 && !consoleTicks.back().ready && afterUnlocks==consoleTicks.size());}
 // Pressure: the display defers relays (budgeted) and the console; a setup or
 // the mic defers relays only (queuedExitTests R2).
 {linkUp(0);accessory=true;tickFlush(clockMs+=10);
  assert(phoneGatt.lastPressure && phoneGatt.lastConsoleBusy && consoleTicks.back().busy);
  accessory=false;tickFlush(clockMs+=10);assert(!phoneGatt.lastPressure && !phoneGatt.lastConsoleBusy && !consoleTicks.back().busy);}
 // RELAY_ON: from HELLO_ACK at READY, then each LINK_STATE (published after
 // the poll); without it no OPEN goes out. Cleared with READY.
 {linkUp(0);assert(!relayOn && !phonePipeRelayAvailable());
  PhonePipeStream h;std::atomic<bool> cancel{false};
  assert(phonePipeOpen(h,"relay.example.test",443,15000,cancel,nullptr).status==St::NotReady && !h.id && !phoneSession.find(1));
  // Relay off decides before the caller's budget: a relay user reads it as no phone path.
  assert(phonePipeOpen(h,"relay.example.test",443,3999,cancel,nullptr).status==St::NotReady && !h.id);
  assert(phoneLinkState(hw1pipe::kPhoneRelayOn) && !phonePipeRelayAvailable());  // the owner's next pass publishes it
  tickFlush(clockMs+=10);assert(phonePipeRelayAvailable());
  assert(phoneLinkState(0));tickFlush(clockMs+=10);assert(ready && !phonePipeRelayAvailable());}
 {linkUp(hw1pipe::kPhoneRelayOn);assert(relayOn && phonePipeRelayAvailable());
  phoneGatt.failNext=true;tickFlush(clockMs+=10);assert(!ready && !relayOn && !phonePipeRelayAvailable());
  // relay status reads both the link and the phone's switch.
  linkUp(hw1pipe::kPhoneRelayOn);assert(std::string(relayStatusCommand("")).rfind("Relay: phone up relay=on corr=a102030405060708\n",0)==0);
  linkUp(0);assert(std::string(relayStatusCommand("")).rfind("Relay: phone up relay=off corr=",0)==0);}
 // The activity rule (A-4): an open console stream alone is idle; the
 // driver's note (COMMAND/BINARY in, TEXT/BLOB out) and relay streams are activity.
 {linkUp(hw1pipe::kPhoneConsoleWanted,hw1pipe::kP4ConsoleOffered);
  const uint16_t id=phoneSession.openConsole(clockMs);assert(id && phoneSession.streams[hw1pipe::kConsoleSlot].live());
  for(int i=0;i<100;++i)tickFlush(clockMs+=100);
  assert((paramRequests==std::vector<int>{72}) && !params.wanted && phoneSession.streams[hw1pipe::kConsoleSlot].live());
  phonePipeNoteConsoleActivity();tickFlush(clockMs+=10);assert(params.wanted && lastStreamAt==clockMs);
  assert((paramRequests==std::vector<int>{72,24}));   // linkUp answered the idle request: active goes at once
  tickFlush(clockMs+=5000);assert((paramRequests==std::vector<int>{72,24}));
  linkUp(hw1pipe::kPhoneRelayOn);uint8_t host[4]={192,0,2,1};assert(phoneSession.open(1,host,4,443,clockMs));
  tickFlush(clockMs+=5000);assert(params.wanted && (paramRequests==std::vector<int>{72,24}));}
 // The console's writer: one whole record into slot 2 of the live READY session.
 {reset();mutex=&handle;const uint8_t head[4]={2,0,2,0},text[2]={'O','K'};
  assert(phonePipeConsolePush(1,1,head,4,text,2,4,0)==PhonePipePush::Gone);   // no session
  linkUp(hw1pipe::kPhoneConsoleWanted|hw1pipe::kPhoneRelayOn,hw1pipe::kP4ConsoleOffered);mutex=&handle;
  const uint32_t gen=phoneGatt.generation();const uint16_t id=phoneSession.openConsole(clockMs);
  assert(phonePipeConsolePush(gen,id,head,4,text,2,4,0)==PhonePipePush::Gone);  // OPEN not answered
  tickFlush(clockMs+=10);uint8_t limit[4];hw1pipe::put32(limit,4096);assert(phoneDeliver(hw1pipe::kVersion,Op::OpenOk,id,limit,4));
  assert(phoneSession.find(id)->state==hw1pipe::StreamState::Open);
  const unsigned locks=ownerLocks;
  assert(phonePipeConsolePush(gen,id,head,4,text,2,4,0)==PhonePipePush::Ok && ownerLocks==locks+1 && phoneSession.find(id)->tx.size==6);
  assert(phonePipeConsolePush(gen+1,id,head,4,text,2,4,0)==PhonePipePush::Gone);   // another session's
  uint8_t host[4]={192,0,2,1};const uint16_t relay=phoneSession.open(1,host,4,443,clockMs);
  tickFlush(clockMs+=10);hw1pipe::put32(limit,1024);assert(phoneDeliver(hw1pipe::kVersion,Op::OpenOk,relay,limit,4));
  assert(phoneSession.find(relay)->state==hw1pipe::StreamState::Open && phoneSession.txSpace(relay));
  assert(phonePipeConsolePush(gen,relay,head,4,text,2,4,0)==PhonePipePush::Gone && !phoneSession.find(relay)->tx.size);  // slot 2 only
  static uint8_t big[4096];
  assert(phonePipeConsolePush(gen,id,head,4,big,4086,4,0)==PhonePipePush::NoSpace && phoneSession.find(id)->tx.size==6);
  assert(phonePipeConsolePush(gen,id,head,4,big,4082,4,0)==PhonePipePush::Ok);  // fills it, the 4 B reserve kept
  assert(!phonePipeOwnerHeldByCurrentTask());
  {Lock held;assert(phonePipeOwnerHeldByCurrentTask() && phonePipeConsolePush(gen,id,head,4,text,2,4,50)==PhonePipePush::Busy);}
  lockRefused=true;assert(phonePipeConsolePush(gen,id,head,4,text,2,4,10)==PhonePipePush::Busy);lockRefused=false;
  fail(clockMs,Error::Connect,"phone link lost");assert(phonePipeConsolePush(gen,id,head,4,text,2,4,0)==PhonePipePush::Gone);
  mutex=nullptr;assert(phonePipeConsolePush(gen,id,head,4,text,2,4,0)==PhonePipePush::Gone && !phonePipeOwnerHeldByCurrentTask());}
 // The status snapshot: copied under its leaf lock, never the owner mutex;
 // false and zeroed before the pipe has resources.
 {reset();PhonePipeLinkSummary sum;sum.ended=7;sum.active=true;assert(!phonePipeLinkSummary(&sum) && !sum.ended && !sum.active);
  linkUp(hw1pipe::kPhoneRelayOn|hw1pipe::kPhoneConsoleWanted,hw1pipe::kP4ConsoleOffered);mutex=&handle;
  uint8_t host[4]={192,0,2,1};assert(phoneSession.open(1,host,4,443,clockMs) && phoneSession.openConsole(clockMs));
  versionStrikes=2;sessionStats.versionMismatches=5;tickFlush(clockMs+=10);
  const unsigned locks=ownerLocks,crits=critEntries;assert(phonePipeLinkSummary(&sum) && phonePipeLinkSummary(nullptr));
  assert(ownerLocks==locks && critEntries==crits+2);
  assert(sum.active && sum.ready && sum.relayOn && sum.consoleWanted && sum.consoleOffered && !sum.versionLatched);
  assert(sum.strikes==2 && sum.versionMismatches==5 && sum.relayStreams==1 && sum.consoleStreams==1 && !sum.ended);
  consoleFlags=0;assert(phoneLinkState(0));tickFlush(clockMs+=10);assert(phonePipeLinkSummary(&sum));
  assert(!sum.relayOn && !sum.consoleWanted && !sum.consoleOffered && sum.ready);
  versionLatched=true;fail(clockMs,Error::Paused,"phone link lost");tickFlush(clockMs+=10);assert(phonePipeLinkSummary(&sum));
  assert(!sum.ready && !sum.relayOn && sum.versionLatched && sum.ended==1 && sum.lastEnd==18 && sum.endReason[18]==1);
  mutex=nullptr;}
 // A stopped pipe's passes never read the console flags (their reader parses
 // the phone secret, which `phone secret`, `phone pair new` and a factory
 // reset rewrite off the owner mutex once the pipe stopped): the snapshot
 // offers the console only as a running session announces it.
 {reset();mutex=&handle;consoleFlags=hw1pipe::kP4ConsoleOffered;tickFlush(clockMs+=10);
  PhonePipeLinkSummary sum;assert(phonePipeLinkSummary(&sum) && sum.active && sum.consoleOffered && localFlagReads);
  stopWanted=true;tickFlush(clockMs+=10);assert(!active && phonePipeLinkSummary(&sum) && !sum.active && !sum.consoleOffered);
  const unsigned reads=localFlagReads;
  for(int i=0;i<100;++i){tickFlush(clockMs+=10);assert(phonePipeLinkSummary(&sum) && !sum.consoleOffered);}
  assert(localFlagReads==reads && consoleTicks.size()>=100);
  mutex=nullptr;}
 // The pipe key: derived off the owner mutex into the start's own stack
 // (wiped after), published only if no invalidation ran meanwhile.
 {reset();mutex=&handle;active=false;pskValid=false;std::memset(cachedPsk,0,sizeof(cachedPsk));
  uint8_t want[32];gSettings.phonePipeSecret="first pipe secret";
  assert(loadPsk()==PskLoad::Ready && pskValid && pbkdf2Calls==1 && pbkdf2Out && pbkdf2Out!=cachedPsk);
  fakeKey("first pipe secret",17,want);assert(!std::memcmp(cachedPsk,want,32) && memzeroed.back()==pbkdf2Out);
  assert(loadPsk()==PskLoad::Ready && pbkdf2Calls==1);   // a key in place: none derived
  // `phone pair new` (or `phone secret`, a factory reset) races a background
  // start: its invalidation and store land while the start derives the old key.
  assert(phonePipeInvalidateSecret() && !pskValid);
  onPbkdf2=[]{assert(phonePipeInvalidateSecret());gSettings.phonePipeSecret="second pipe secret";};
  assert(loadPsk()==PskLoad::Retry && !pskValid && pbkdf2Calls==2 && memzeroed.back()==pbkdf2Out);
  for(uint8_t b:cachedPsk)assert(!b);
  onPbkdf2=nullptr;   // the next start derives the new secret's key
  assert(loadPsk()==PskLoad::Ready && pskValid && pbkdf2Calls==3);
  fakeKey("second pipe secret",18,want);assert(!std::memcmp(cachedPsk,want,32));
  // No secret; the owner mutex not free (nothing derived, nothing published).
  assert(phonePipeInvalidateSecret());gSettings.phonePipeSecret="";
  assert(loadPsk()==PskLoad::NoSecret && !pskValid && pbkdf2Calls==3);
  gSettings.phonePipeSecret="first pipe secret";lockRefused=true;
  assert(loadPsk()==PskLoad::Retry && !pskValid && pbkdf2Calls==3);lockRefused=false;
  mutex=nullptr;}
 // `phone stats`: the version counters and the console driver's counts (no content).
 {reset();consoleStatus.opens=3;consoleStatus.cardDrops=1;consoleStatus.mirrorDrops=2;consoleStatus.unknown=4;
  consoleStatus.malformed=5;consoleStatus.aborts=6;sessionStats.endReason[16]=1;sessionStats.endReason[18]=2;
  consoleStatus.withheldMs=4321;consoleStatus.withheldMaxMs=900;consoleStatus.poolFull=7;
  consoleStatus.executorBusy=8;consoleStatus.rowUnbound=9;
  sessionStats.versionMismatches=3;versionStrikes=2;
  std::string stats=phoneStatsCommand("");
  assert(stats.find(" version_mismatch=3 strikes=2\nend reasons: 16=1 18=2\n"
                    "console opens=3 card_drop=1 mirror_drop=2 unknown=4 malformed=5 aborts=6"
                    " withheld_ms=4321 max_ms=900 pool_full=7 exec_busy=8 row_unbound=9\nscans total=")!=npos);
  versionLatched=true;stats=phoneStatsCommand("");assert(stats.find(" strikes=2 held\nend reasons:")!=npos);}
 // Version mismatch (phone link v2). Strikes: the phone's version RESET under a
 // v1 header, a link loss within 1.5 s of the HELLO, the 3 s HELLO deadline.
 const uint8_t versionReset[2]={2,0},paused[2]={18,0};
 {reset();
  pipeAttempt([&]{assert(!phoneDeliver(1,Op::Reset,0,versionReset,2));});
  assert(phoneSession.helloEnd()==hw1pipe::HelloEnd::VersionSignal && phoneSession.peerReset);
  assert(versionStrikes==1 && !versionLatched && statusIs("phone app version mismatch") && sessionStats.versionMismatches==1);
  assert(!sessionStats.ended && !sessionStats.endReason[2] && pipeAdmissionEnabled);   // never READY: no end counted
  // Neither adds nor resets: an attempt that never reached the pipe (the last
  // session's latch is still in the Session), and an auth failure.
  job=candidate=true;holdTurn();step=Step::Link;std::memset(phoneGatt.entered,0,sizeof(phoneGatt.entered));
  phoneGatt.state=Gatt::Phase::Opening;phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;tickFlush(clockMs+=10);
  assert(statusIs("phone link lost") && versionStrikes==1 && phoneSession.helloEnd()==hw1pipe::HelloEnd::VersionSignal);
  for(int i=0;i<4 && step!=Step::Idle;++i)tickFlush(clockMs+=10);
  job=candidate=true;holdTurn();step=Step::Link;std::memset(phoneGatt.entered,0,sizeof(phoneGatt.entered));
  phoneGatt.authFailed=true;phoneGatt.state=Gatt::Phase::Retiring;tickFlush(clockMs+=10);
  assert(haveAuthFailure && versionStrikes==1);phoneGatt.authFailed=false;haveAuthFailure=false;
  for(int i=0;i<4 && step!=Step::Idle;++i)tickFlush(clockMs+=10);
  // A link loss 1.5 s after the HELLO, or a PAUSED RESET, is an ordinary end.
  pipeAttempt([&]{clockMs+=1490;phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;});
  assert(statusIs("phone link lost") && versionStrikes==1 && phoneSession.helloEnd()==hw1pipe::HelloEnd::None);
  pipeAttempt([&]{assert(!phoneDeliver(hw1pipe::kVersion,Op::Reset,0,paused,2));});
  assert(statusIs("phone link lost") && versionStrikes==1 && phoneSession.error==Error::Paused);
  // Strike 2: silence, the link lost within 1.5 s.
  pipeAttempt([&]{clockMs+=500;phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;});
  assert(phoneSession.helloEnd()==hw1pipe::HelloEnd::Silent && versionStrikes==2 && !versionLatched);
  assert(statusIs("phone app version mismatch"));
  // Strike 3: the HELLO deadline. Held: no reconnect admission (the scheduler
  // keeps its intent), the ring gets its link back, the reason is shown.
  const uint32_t at=clockMs;pipeAttempt([]{});
  assert(phoneSession.helloEnd()==hw1pipe::HelloEnd::Silent && clockMs-at>=3000 && phoneGatt.stopError==Error::Deadline);
  assert(versionStrikes==3 && versionLatched && statusIs("phone app too old") && sessionStats.versionMismatches==3);
  assert(!pipeAdmissionEnabled && !sessionStats.ended);
  const unsigned claims=claimCalls;BlePeerConnectRequest wanted;
  assert(admit(wanted)==BlePeerConnectAdmission::ROLE_BLOCKED && claimCalls==claims && step==Step::Idle && !job);
  tickFlush(clockMs+=10);PhonePipeLinkSummary sum;mutex=&handle;assert(phonePipeLinkSummary(&sum));mutex=nullptr;
  assert(sum.versionLatched && sum.strikes==3 && sum.versionMismatches==3 && !sum.ended);
  // The validate pass lifts nothing.
  gCLIValidateOnly=true;assert(!std::strcmp(cmd_openphone(""),"VALID") && versionLatched);gCLIValidateOnly=false;
  // `openphone` lifts it and connects now (its own job: no reseek).
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connecting to the paired phone"));
  assert(!versionLatched && versionStrikes==0 && pipeAdmissionEnabled && job && reseeks==0);}
 // `openphone` on a held, open pipe lifts the hold and connects; a second one
 // finds the job pending. A pipe that is stopping keeps the ring's way clear
 // (a real open enables it itself) and admits nothing.
 {reset();versionLatched=true;versionStrikes=3;bleCentralPhoneEnabled(false);
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connecting to the paired phone"));
  assert(!versionLatched && !versionStrikes && pipeAdmissionEnabled && job && reseeks==0);
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connect pending (busy, the scheduler retries)") && reseeks==0);
  versionLatched=true;stopWanted=true;bleCentralPhoneEnabled(false);
  assert(!std::strcmp(cmd_openphone(""),"Phone: open; connect pending (busy, the scheduler retries)") && !versionLatched && !pipeAdmissionEnabled && reseeks==0);}
 // `phone pair` lifts it too and starts its enrollment (no reseek).
 {reset();versionLatched=true;versionStrikes=3;bleCentralPhoneEnabled(false);
  assert(std::string(phonePairCommand("")).rfind("Pairing opened for 60 seconds",0)==0);
  assert(!versionLatched && !versionStrikes && pipeAdmissionEnabled && enrolling && reseeks==0);}
 // READY resets the streak and lifts a hold.
 {reset();
  pipeAttempt([&]{assert(!phoneDeliver(1,Op::Reset,0,versionReset,2));});
  pipeAttempt([&]{clockMs+=100;phoneGatt.failNext=true;phoneGatt.failError=Error::Connect;});
  assert(versionStrikes==2);versionLatched=true;bleCentralPhoneEnabled(false);
  pendingEnrollment();std::memset(phoneGatt.entered,0,sizeof(phoneGatt.entered));phoneSilent=false;enterPipe();
  for(int i=0;i<5 && !ready;++i)tickFlush(clockMs+=10);
  assert(ready && !versionStrikes && !versionLatched && pipeAdmissionEnabled && sessionStats.versionMismatches==2);
  // A PAUSED RESET after READY: an ordinary end (bucket 18), not a strike.
  assert(!phoneDeliver(hw1pipe::kVersion,Op::Reset,0,paused,2));tickFlush(clockMs+=10);
  assert(!ready && sessionStats.endReason[18]==1 && sessionStats.lastEnd==18 && sessionStats.peerReset==1 && !versionStrikes);}
 // The pipe's hold on gBLEState: given back once the pipe has stopped, outside
 // the owner mutex; a stop that does not finish keeps it.
 {reset();active=false;stateHeld=true;assert(phonePipeStop() && stateReleases==1 && !stateHeld);
  assert(phonePipeStop() && stateReleases==1);
  reset();stateHeld=true;playWorker=[]{tickFlush(clockMs);};assert(phonePipeStop() && !active && stateReleases==1);
  reset();stateHeld=true;playWorker=nullptr;assert(!phonePipeStop() && active && stateReleases==0 && stateHeld);
  reset();stateHeld=true;playWorker=[]{tickFlush(clockMs);};
  assert(phonePipeHostStopped() && !active && stateReleases==1 && !stateHeld);playWorker=nullptr;}
 // android-011 §3.3: the link comes up and stays up with RELAY_ON 0 (and no
 // relay destination: the owner never consults one) for 10 min: one link
 // PING a minute (the phone answers), idle parameters, READY throughout.
 {linkUp(0);const uint32_t up=clockMs;
  for(uint32_t t=0;t<600000;t+=100){tickFlush(clockMs+=100);assert(ready && !phonePipeRelayAvailable());}
  assert(step==Step::Link && statusIs("ready") && phoneSession.state==hw1pipe::State::Ready && !sessionStats.ended);
  assert(pingTimes.size()>=9 && pingTimes.size()<=10);
  for(size_t i=0;i<pingTimes.size();++i){const uint32_t gap=pingTimes[i]-(i?pingTimes[i-1]:up);assert(gap>=60000 && gap<=60200);}
  assert((paramRequests==std::vector<int>{72}) && params.applied && !params.active && !params.wanted);}
}


// ---- `phone pair new` (phone link v2): refusals first, one sink, the window ---------------
static std::string expectedCode(){char c[17];hw1code::fromRandom(randomWords,c);return c;}
static std::string displayForm(const std::string& c){char d[20];hw1code::display(c.c_str(),d);return d;}
// Any form of the code a text could carry: canonical, display, or one group.
static bool carriesCode(const std::string& text){
 const std::string c=expectedCode(),d=displayForm(c);
 if(text.find(c)!=std::string::npos || text.find(d)!=std::string::npos)return true;
 for(size_t g=0;g<4;++g)if(text.find(c.substr(g*4,4))!=std::string::npos)return true;
 return false;
}
static bool windowCodeWiped(){for(char ch:pairCode)if(ch)return false;return true;}
static const char* const kSerialReply="Pairing open 3 min with the code above (shown once, never logged). In the app: Phone link > Pair, then type it.";
static const char* const kDisplayReply="Pairing code on the OLED: pairing open 3 min (B closes it). In the app: Phone link > Pair, then type the code.";
// The pipe runs; a stop lets the phone task finish it, and a start leaves the
// pipe as phonePipeStart does (running, its key derived, the ring yield set).
static void pairNewReset(){
 reset();playWorker=[]{tickFlush(clockMs);};
 onPipeStart=[]()->bool{active=true;stopWanted=false;pskValid=true;bleCentralPhoneEnabled(!versionLatched.load());return true;};
}
static void pairNewIdle(){for(int i=0;i<20 && step!=Step::Idle;++i)tickFlush(clockMs+=10);assert(step==Step::Idle && !job);}

#if !HW1_PAIR_SINK
static void pairNewTests() {
 const std::string code=expectedCode(),shown=displayForm(code);
 assert(hw1code::isCode(code.c_str(),16) && shown.size()==19 && !carriesCode(kSerialReply) && !carriesCode(kDisplayReply));
 // Every refusal comes before any state change: no random draw, no secret
 // step, no link start, no window, no sink, the version hold as it was.
 struct Refusal{const char* reply;const char* args;std::function<void()> arrange;};
 const Refusal refusals[]={
  {"VALID","",[]{gCLIValidateOnly=true;}},
  {"Error: invalid arguments — Usage: phone pair new","now",[]{}},
  {"Error: phone pair new requires an authenticated local admin on serial or the OLED","",[]{execAdmin=false;}},
  {"Error: phone pair new requires an authenticated local admin on serial or the OLED","",[]{authContext.transport=SOURCE_WEB;}},
  {"Error: phone pair new prints its code once on this console; turn serial output on","",[]{serialSinkReady=false;}},
  {"Error: the OLED cannot show a pairing code now","",[]{authContext.transport=SOURCE_LOCAL_DISPLAY;displaySinkReady=false;}},
  {"Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'","",[]{serverUp=true;}},
  {"Error: Bluetooth is disabled; run bleenabled on first","",[]{active=false;gSettings.bleEnabled=false;}},
  {"Error: phone is owned by another account","",[]{ownerUser="other";}},
  {"Error: local authenticated session required","",[]{captureEpoch=0;}},
  {"Error: pipe authentication cooldown (30 seconds)","",[]{haveAuthFailure=true;lastAuthFailure=clockMs;}},
  {"Error: phone pipe busy","",[]{lockRefused=true;}},
 };
 for(const auto& r:refusals){
  pairNewReset();versionLatched=true;versionStrikes=3;r.arrange();
  const std::string reply=phonePairNewCommand(r.args);
  gCLIValidateOnly=false;lockRefused=false;
  assert(reply==r.reply);
  assert(!randomCalls && replaceCalls.empty() && serialWrites.empty() && !displayShows && !pipeStarts && !claimCalls);
  assert(pairState==PhonePairWindow::None && !phonePipeEnrolling() && versionLatched && versionStrikes==3);
 }
 // Serial: the `phone secret` steps with the code marked device-made, the link
 // started again, one serial write of the code, the window open for the
 // issuing session. The result never carries it; the window keeps no copy.
 {pairNewReset();versionLatched=true;versionStrikes=3;bleCentralPhoneEnabled(false);
  const std::string reply=phonePairNewCommand("");
  assert(reply==kSerialReply && !carriesCode(reply));
  assert(randomCalls==3 && replaceCalls.size()==1 && replaceCalls[0].value==code && replaceCalls[0].generated);
  assert(storedSecret==code && storedGenerated && pipeStarts==1 && active && !stopWanted);
  assert(serialWrites.size()==1 && serialWrites[0]=="\nPairing code (shown once): "+shown+"\n" && !displayShows);
  assert(pairState==PhonePairWindow::Open && phonePipeEnrolling() && pairEpoch==5 && pairSource==SOURCE_SERIAL);
  assert(windowCodeWiped() && !pairOnDisplay);
  char seen[20];std::memset(seen,'x',sizeof(seen));const PhonePairView v=phonePipePairView(seen);
  assert(v.state==PhonePairWindow::Open && v.id && !v.onDisplay && v.msLeft==180000 && !v.ageMs);
  for(char ch:seen)assert(!ch);
  // A user's pairing lifts the version hold; no reseek (the window starts its own attempt).
  assert(!versionLatched && !versionStrikes && pipeAdmissionEnabled && !reseeks);
  tickFlush(clockMs+=10);
  assert(job && enrolling && step==Step::Waiting && statusIs("waiting to enroll") && claimCalls==1 && enrollmentEpoch==5);
  assert(phonePipePairView(nullptr).ageMs==10);}
 // The window re-runs the enrollment attempt: 2 s after one ends, the setup
 // turn free in between (the glasses' setup runs), never inside the auth
 // cooldown; READY ends it, and no attempt follows.
 {pairNewReset();world.on=true;world.advertise=false;
  assert(phonePairNewCommand("")==std::string(kSerialReply));
  tickFlush(clockMs+=10);assert(job && claimCalls==1);
  serveWorker();  // attempt 1: no phone advertises, the scan times out
  assert(!job && step==Step::Idle && phoneSlotIdle() && !setupHeld && phonePipeEnrolling() && pairState==PhonePairWindow::Open);
  const uint32_t endAt=clockMs;
  fakes[0].runMs=500;submitAccessory(0);serveWorker();
  assert(fakes[0].runs==1 && claimCalls==1 && !job);
  while(claimCalls==1)tickFlush(clockMs+=10);
  assert(clockMs-endAt>=2000 && clockMs-endAt<=2010 && job && enrolling && step==Step::Waiting && enrollmentEpoch==5);
  serveWorker();  // attempt 2 misses too; then a wrong code's auth failure
  const uint32_t missAt=clockMs;haveAuthFailure=true;lastAuthFailure=missAt;
  const unsigned claims=claimCalls;
  while(claimCalls==claims)tickFlush(clockMs+=10);
  assert(clockMs-missAt>=30000 && clockMs-missAt<=30010 && pairState==PhonePairWindow::Open);
  world.advertise=true;world.linkMs=300;serveWorker();
  assert(ready && published && acceptCount==1 && pairState==PhonePairWindow::Paired && !phonePipeEnrolling());
  const unsigned after=claimCalls;for(int i=0;i<500;++i)tickFlush(clockMs+=10);
  char seen[20];assert(claimCalls==after && phonePipePairView(seen).state==PhonePairWindow::Paired && !seen[0]);}
 // 3 min from the code it expires, with an attempt in flight cancelled; no
 // attempt follows. (Queued, never granted: each attempt's own 60 s cap.)
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply));
  const uint32_t opened=pairOpenedAt;
  while(clockMs-opened<kPairWindowMs-10)tickFlush(clockMs+=10);
  assert(pairState==PhonePairWindow::Open && phonePipeEnrolling() && claimCalls>=3 && job);
  tickFlush(clockMs+=10);
  assert(pairState==PhonePairWindow::Expired && statusIs("pairing code expired") && step==Step::Retiring);
  pairNewIdle();assert(!phonePipeEnrolling());
  const unsigned claims=claimCalls;for(int i=0;i<500;++i)tickFlush(clockMs+=10);
  assert(claimCalls==claims && statusIs("pairing code expired") && phoneSlotIdle());}
 // The OLED: no serial write; the window holds the code for its screen. B
 // (phonePipePairClose with the shown window's id) wipes it at once; the next
 // pass ends the window and cancels its attempt. Another window's id does nothing.
 {pairNewReset();authContext.transport=SOURCE_LOCAL_DISPLAY;
  const std::string reply=phonePairNewCommand("");
  assert(reply==kDisplayReply && !carriesCode(reply) && serialWrites.empty() && displayShows==1);
  assert(pairSource==SOURCE_LOCAL_DISPLAY && storedSecret==code && storedGenerated);
  char seen[20]{};PhonePairView v=phonePipePairView(seen);
  assert(v.state==PhonePairWindow::Open && v.onDisplay && v.id && shown==seen && v.msLeft==180000);
  tickFlush(clockMs+=10);assert(job && step==Step::Waiting);
  v=phonePipePairView(seen);assert(v.ageMs==10 && v.msLeft==179990 && shown==seen);
  phonePipePairClose(v.id+1);tickFlush(clockMs+=10);
  assert(pairState==PhonePairWindow::Open && phonePipePairView(seen).onDisplay && shown==seen);
  phonePipePairClose(v.id);v=phonePipePairView(seen);
  assert(v.state==PhonePairWindow::Open && !v.onDisplay && !seen[0] && windowCodeWiped());
  tickFlush(clockMs+=10);
  assert(pairState==PhonePairWindow::Closed && statusIs("pairing code closed") && step==Step::Retiring);
  pairNewIdle();assert(!phonePipeEnrolling());const unsigned claims=claimCalls;for(int i=0;i<500;++i)tickFlush(clockMs+=10);
  assert(claimCalls==claims && phoneSlotIdle());}
 // It also ends on phone stop (and phone secret, whose steps stop the pipe),
 // on a second phone pair new (whose stop ends the first; a new one opens), on
 // phone forget (with its attempt), and when the issuing session ends.
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply));
  assert(phonePipeStop() && !active && pairState==PhonePairWindow::Closed && !phonePipeEnrolling());}
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply));const uint32_t first=pairId;
  assert(phonePairNewCommand("")==std::string(kSerialReply));
  assert(replaceCalls.size()==2 && replaceCalls[1].windowAfterStop==PhonePairWindow::Closed);
  assert(pairId!=first && pairState==PhonePairWindow::Open && serialWrites.size()==2);}
 {pairNewReset();authContext.transport=SOURCE_LOCAL_DISPLAY;assert(phonePairNewCommand("")==std::string(kDisplayReply));
  tickFlush(clockMs+=10);assert(job);
  assert(std::string(phoneForgetCommand("")).rfind("Phone forget accepted",0)==0);
  assert(pairState==PhonePairWindow::Closed && statusIs("pairing code closed") && windowCodeWiped() && forgetWanted);}
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply));epochLive=false;tickFlush(clockMs+=10);
  assert(pairState==PhonePairWindow::Closed && statusIs("pairing code closed") && !phonePipeEnrolling() && !claimCalls);}
 // A code that could not be shown ends its window at once.
 {pairNewReset();serialWriteOk=false;
  assert(!std::strcmp(phonePairNewCommand(""),"Error: the pairing code could not be shown; run phone pair new again"));
  assert(serialWrites.size()==1 && pairState==PhonePairWindow::Closed && windowCodeWiped() && !phonePipeEnrolling());}
 {pairNewReset();authContext.transport=SOURCE_LOCAL_DISPLAY;displayShowOk=false;
  assert(!std::strcmp(phonePairNewCommand(""),"Error: the pairing code could not be shown; run phone pair new again"));
  char seen[20];const PhonePairView v=phonePipePairView(seen);
  assert(displayShows==1 && v.state==PhonePairWindow::Closed && !seen[0] && windowCodeWiped());}
 // The secret steps fail before the secret changed (their own message), or
 // after (run it again); the link does not start: no window, no code shown.
 {pairNewReset();replaceFail="Error: pipe key invalidation busy; retry before changing secret";
  assert(!std::strcmp(phonePairNewCommand(""),replaceFail) && serialWrites.empty() && pairState==PhonePairWindow::None);
  pairNewReset();replaceFail="Error: secret changed; enrollment cleanup busy, retry phone secret";replaceFailAfter=true;
  assert(!std::strcmp(phonePairNewCommand(""),"Error: the new code replaced the phone secret, then a cleanup step failed; run phone pair new again"));
  assert(serialWrites.empty() && pairState==PhonePairWindow::None && storedSecret==code);
  pairNewReset();startOk=false;
  assert(!std::strcmp(phonePairNewCommand(""),"Error: phone link could not open (stopped)"));
  assert(serialWrites.empty() && pairState==PhonePairWindow::None && storedSecret==code);}
 // Bluetooth set off while the pipe still runs is no refusal (the link runs).
 {pairNewReset();gSettings.bleEnabled=false;assert(phonePairNewCommand("")==std::string(kSerialReply));}
 // `phone pair` takes no argument: `phone pair new` is its own row.
 {pairNewReset();assert(!std::strcmp(phonePairCommand("news"),"Error: invalid arguments — Usage: phone pair [new]"));
  assert(!claimCalls && !manualLearns && !job);}
 // The version hold ends the window. G2 mode (the attempts reserve the arms):
 // three attempts reach the HELLO and get a 2.7.x app's version RESET; the
 // third latches the hold, which gives the ring its link back (the phone no
 // longer reserves one) and closes the window, so no attempt follows. The
 // reason stays shown.
 {pairNewReset();g2Up=true;world.on=true;
  assert(phonePairNewCommand("")==std::string(kSerialReply) && pipeAdmissionEnabled);
  onStep=[]{
   static const uint8_t versionReset[2]={2,0};
   if(step==Step::Link && phoneGatt.state==Gatt::Phase::Opening){phoneGatt.state=Gatt::Phase::Secure;phoneSilent=true;enterPipe();}
   else if(phoneGatt.state==Gatt::Phase::Pipe && phoneSession.state==hw1pipe::State::Hello)
    assert(!phoneDeliver(1,hw1pipe::Op::Reset,0,versionReset,2));
  };
  for(unsigned strike=1;strike<=3;++strike){
   while(!job)tickFlush(clockMs+=10);   // the window's next attempt
   serveWorker();                        // its turn: found, HELLO, the version RESET
   assert(versionStrikes==strike && lastReserveArms && !job && step==Step::Idle);
   assert(phoneSession.helloEnd()==hw1pipe::HelloEnd::VersionSignal);
   assert(statusIs(strike<3?"phone app version mismatch":"phone app too old"));
  }
  onStep=nullptr;
  assert(versionLatched && !pipeAdmissionEnabled && pairState==PhonePairWindow::Closed && !phonePipeEnrolling());
  const unsigned claims=claimCalls,learns=manualLearns;
  for(int i=0;i<500;++i)tickFlush(clockMs+=10);
  assert(claimCalls==claims && manualLearns==learns && !job && statusIs("phone app too old"));
  assert(pairState==PhonePairWindow::Closed && versionLatched && !pipeAdmissionEnabled);}
 // The user's disconnect (the first step of `closephone`) ends the window:
 // its attempt is cancelled and none follows, so no manual learn clears the
 // user's disconnect. Between two attempts, the same.
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply));
  tickFlush(clockMs+=10);assert(job && enrolling && claimCalls==1 && manualLearns==1);
  disconnect();assert(userDisconnects==1);
  assert(pairState==PhonePairWindow::Closed && statusIs("pairing code closed") && step==Step::Retiring);
  pairNewIdle();for(int i=0;i<300;++i)tickFlush(clockMs+=10);
  assert(claimCalls==1 && manualLearns==1 && !phonePipeEnrolling() && statusIs("pairing code closed"));}
 {pairNewReset();assert(phonePairNewCommand("")==std::string(kSerialReply) && !job);
  disconnect();assert(userDisconnects==1);
  assert(pairState==PhonePairWindow::Closed && statusIs("pairing code closed") && step==Step::Idle);
  for(int i=0;i<300;++i)tickFlush(clockMs+=10);
  assert(!claimCalls && !manualLearns && !phonePipeEnrolling());}
 // A window opened after the view read its clock (cmd_exec, while the display
 // task was preempted) reads as new, never as ~49 days old (the OLED watcher's
 // 2 s grace would close it at once). The view carries the issuing session.
 {pairNewReset();{Lock lock;(void)openPairWindow(clockMs+1,5,SOURCE_LOCAL_DISPLAY,"ABCD-EFGH-JKMN-PQRS");}
  char seen[20];const PhonePairView v=phonePipePairView(seen);
  assert(v.state==PhonePairWindow::Open && v.onDisplay && !v.ageMs && v.msLeft==180000 && !std::strcmp(seen,"ABCD-EFGH-JKMN-PQRS"));
  assert(v.source==SOURCE_LOCAL_DISPLAY && v.epoch==5);
  clockMs+=11;assert(phonePipePairView(nullptr).ageMs==10);
  {Lock lock;(void)closePairWindowLocked(clockMs);}
  const PhonePairView closed=phonePipePairView(seen);
  assert(closed.state==PhonePairWindow::Closed && !closed.id && !closed.epoch && !seen[0]);}
 // The sink again, right before the window: a session that ended while the
 // pipe stopped and the key was derived gets no window and no code. The
 // secret did change (it was never shown).
 for(int display=0;display<2;++display){
  pairNewReset();if(display)authContext.transport=SOURCE_LOCAL_DISPLAY;
  onPipeStart=[]()->bool{active=true;stopWanted=false;pskValid=true;serialSinkReady=displaySinkReady=false;return true;};
  assert(!std::strcmp(phonePairNewCommand(""),"Error: the session ended; run phone pair new again"));
  assert(replaceCalls.size()==1 && storedSecret==code && pipeStarts==1 && active);
  assert(pairState==PhonePairWindow::None && serialWrites.empty() && !displayShows && windowCodeWiped());
  for(int i=0;i<300;++i)tickFlush(clockMs+=10);
  assert(!claimCalls && !phonePipeEnrolling() && pairState==PhonePairWindow::None);}
 // A session's end only ever ends a pairing window: the link and its relay
 // never expire on a login basis.
 {linkUp(hw1pipe::kPhoneRelayOn);epochLive=false;captureEpoch=0;
  for(int i=0;i<1200;++i){tickFlush(clockMs+=100);assert(ready && phonePipeRelayAvailable());}
  assert(statusIs("ready") && !sessionStats.ended && step==Step::Link);}
}
#endif

#if HW1_PAIR_SINK
// INSERT_SINK_TESTS
#endif

int main(){
 alarm(110);  // a hung run fails instead of hanging
 assert(bleSetupWorkerStart()&&workerStackBytes==5120&&workerTaskPriority==5&&bleSetupWorkerStart());
#if HW1_PAIR_SINK
 sinkTests();
 return 0;
#endif
 ownerTests();
 openTests();
 scanTests();
 recordTests();
 opsTests();
 admissionTests();
 waitingTests();
 queuedExitTests();
 workerReleaseTests();
 idleLabelTests();
 linkRecordTests();
 v2Tests();
#if !HW1_PAIR_SINK
 pairNewTests();
#endif
 turnTests();
 assert(critDepth==0);
 puts("Phone runtime on the setup worker: whole turns (priority 5 then 1, grant after the raise, one consume, give-back at READY), "
      "claim-first admission and enrollment, Waiting backstop and one-shot nudge, every exit gives the turn back once "
      "(READY, capacity, cleanup bound, quiesce, host reset, Retiring, withdraw of queued and picked jobs), enrollment from the grant "
      "and 60 s queued cap, a failed attempt's reason kept at Idle, overrun revoke and escalation races, wait_ms admission to grant, accessory pressure without cancel; "
      "firmware-007 scan profile and records, A-4 parameters, A-1 OPEN phases, F3 counters, secret replacement, scoped forget, "
      "the console's link record (set at READY, cleared by every end, lock-free copy) and console activity; "
      "phone link v2: releaseIds per slot, end reasons 0..18, flags before the poll and the console pass after it, "
      "display-only console yield, RELAY_ON and local OPEN refusal, the activity rule over relay slots, the console "
      "writer and the status snapshot, the version strikes, hold and lift, the gBLEState hold, 10 min with relay off; "
      "phone pair new: every refusal before any change, one sink, the window's paced re-arm, turn release, cooldown "
      "and ends (READY, 3 min, B, stop, forget, a new code, the session, phone disconnect, the version hold), "
      "a window opened after the view's clock read as new, the sink checked again before the window, a session's "
      "end never ending the link or its relay; no secret read on a stopped pass, the key published only "
      "if no invalidation raced it; the pipe task's exit: refused while active, the host up, the reset unrun or a phone "
      "turn pending, the reset on the task before the request, the final stopped summary before the leave, no priority "
      "call after, a timeout withdrawn with the task kept, a second release a no-op, the idle exit on an up host (2 s after the pipe is idle, held by an app id, a job, an active pipe or a phone turn) passed");
}
