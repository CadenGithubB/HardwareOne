#include "BLE_PhonePipe.h"
#include "BLE_CentralStack.h"  // bleStackUp / bleStackReady: the pipe is a device on the shared stack
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
#include "BLE_PhoneGatt.h"
#include "BLE_LinkStatusCore.h"  // hw1link::PipeStep mirrors Step
#include "BLE_PhonePipeConsole.h"
#include "BLE_PhonePairCode.h"  // `phone pair new` (phone link v2)
#include "BLE_PhonePipeScan.h"
#include "BLE_CoexRecords.h"
#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"
#include "BLE_SetupWorker.h"
#include "BLE_Peers.h"
#include "BLE_PhoneStatus.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "G2_Ring.h"
#include "HAL_Bluetooth.h"
#include "OLED_Display.h"  // the `phone pair new` code screen (stubs without a display)
#include "System_AuthIdentity.h"
#include "System_CommandLimits.h"
#include "System_CommandRegistry.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"  // RETURN_VALID_IF_VALIDATE_CSTR
#include "System_RadioCompanion.h"
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
#include "System_LLMHttpTransport.h"
#endif
#include <esp_gap_ble_api.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/semphr.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <cstring>
#include <cstdio>
#include <new>
#include <esp_attr.h>
#if !defined(HARDWAREONE_BLE_PHONE_PIPE_API) || HARDWAREONE_BLE_PHONE_PIPE_API < 2
#error "Apply and verify tools/ble-phone-pipe/arduino.patch before enabling the phone pipe"
#endif

namespace {
using hw1phone::Gatt;
using Error=hw1pipe::Error;
enum class Step : uint8_t { Idle, Waiting, ScanParams, Scanning, Link, Retiring, Fault };
// phonePipeStep() hands the step to the status surfaces as hw1link::PipeStep.
static_assert(uint8_t(Step::Fault) == uint8_t(hw1link::PipeStep::Fault) &&
              uint8_t(Step::Link) == uint8_t(hw1link::PipeStep::Link) &&
              uint8_t(Step::Retiring) == uint8_t(hw1link::PipeStep::Retiring) &&
              uint8_t(Step::Scanning) == uint8_t(hw1link::PipeStep::Scanning) &&
              uint8_t(Step::ScanParams) == uint8_t(hw1link::PipeStep::ScanParams) &&
              uint8_t(Step::Waiting) == uint8_t(hw1link::PipeStep::Waiting),
              "keep Step in BLE_LinkStatusCore.h's order");
enum class GapKind : uint8_t { ScanParams, ScanStart, ScanDone, ScanStopped,Candidate,Params };
struct GapEvent {GapKind kind{};int status=0;uint8_t address[6]{},type=0;uint16_t interval=0;};
struct Route {bool enabled=false,scan=false,peer=false;uint8_t address[6]{};};
// Protocol storage and adapter both outlive every registration and quarantine.
// Session is only accessed by task-context code under the owner mutex; post()
// fills one of the PSRAM callback slabs below on the BTC task and queues its
// index, without touching this storage.
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
alignas(hw1pipe::Session) EXT_RAM_BSS_ATTR uint8_t phoneSessionStorage[sizeof(hw1pipe::Session)];
// Explicit construction preserves Session's nonzero defaults in zeroed PSRAM.
hw1pipe::Session& phoneSession = *new (phoneSessionStorage) hw1pipe::Session;
// The callback bodies (17 x 536 B): POD, zero at boot, every field written per fill.
EXT_RAM_BSS_ATTR Gatt::Event phoneGattSlabs[Gatt::kSlabCount];
#else
hw1pipe::Session phoneSession;
Gatt::Event phoneGattSlabs[Gatt::kSlabCount];
#endif
Gatt phoneGatt{phoneSession,phoneGattSlabs};
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex=nullptr;
portMUX_TYPE initMux=portMUX_INITIALIZER_UNLOCKED,routeMux=portMUX_INITIALIZER_UNLOCKED,linkMux=portMUX_INITIALIZER_UNLOCKED;
Route route;
StaticQueue_t gapQueueStorage;
alignas(4) uint8_t gapQueueBytes[8*sizeof(GapEvent)];
QueueHandle_t gapQueue=nullptr;
TaskHandle_t worker=nullptr;
// phonePipeWorkerRelease asks the task to leave. Written only under initMux,
// which also guards worker: the task nulls its own handle there as it leaves
// (leaveIfReleased), so the creator's check in resources() sees a slot free
// only once the task is gone.
bool exitWanted=false;
// The task's last pass found the pipe idle on an up host: stopped, step Idle,
// the GATT adapter idle with its app id released, no job, scan, forget, pair
// window or reset pending — nothing the host could call back into. Published
// at the end of passLocked (owner mutex held); run() keeps a clock on it and
// after kPipeIdleRetireMs, with no phone setup turn held, the task leaves as
// for a release (leaveIfReleased): its 8 KB stack and TCB go back until
// phoneLinkOpen's resources() needs one again. A later host stop finds no
// task and nothing to reset (phonePipeHostStopped).
std::atomic<bool> idleOnHost{false};
constexpr uint32_t kPipeIdleRetireMs=2000;
std::atomic<bool> hostResetWanted{false},abortLink{false};
std::atomic<bool> recoveryNeeded{false},quiesceWanted{false},quiesced{false};
std::atomic<uint32_t> liveGeneration{0};
// S4: the READY session's link for the app console (phonePipeLinkRecord).
// Written only in setReady()/clearReady() under the owner mutex; linkMux is a
// leaf lock so readers (BTC callbacks included) never take the owner mutex.
PhonePipeLinkRecord linkRecord;bool linkValid=false;
uint16_t releaseIds[hw1pipe::kMaxStreams]{};
bool scanParamsPending=false;
std::atomic<bool> gapOverflow{false},active{false},ready{false},stopWanted{false};
// The phone's RELAY_ON (HELLO_ACK, then LINK_STATE), published for relay users
// (phonePipeRelayAvailable). Owner mutex writes; set before READY goes out,
// cleared before it drops, so it never reads true without READY.
std::atomic<bool> relayOn{false};
// Version mismatch (phone link v2): strikes in a row of sessions that ended
// before HELLO_ACK by the phone's version RESET or by silence (the core's
// helloEnd()); three hold auto-reconnect (admit() answers ROLE_BLOCKED) until
// `openphone` / `phone pair` or a reboot. READY resets both. RAM only.
std::atomic<uint8_t> versionStrikes{0};
std::atomic<bool> versionLatched{false};
// This pipe holds gBLEState for its console row (blePipeConsoleStateAcquire).
// Taken and given back outside the owner mutex: the lifecycle lock is never
// taken under it.
std::atomic<bool> stateHeld{false};
std::atomic<const char*> status{"stopped"};
// Atomic so the status surfaces read them without the owner mutex.
std::atomic<Step> step{Step::Idle};
BlePeerConnectRequest request;
bool job=false;
std::atomic<bool> enrolling{false};
// The setup turn (BLE_SetupWorker.h). wantToken is the job's slot token from
// admission until the job ends; setupHeld is set once the worker's grant for
// it was consumed (heldToken, at grantAt) and cleared when the turn is given
// back. The worker raises this task to kTurnPriority for the turn.
std::atomic<uint32_t> wantToken{0};
hw1setup::PhoneTurn turn;
bool setupHeld=false,leaveWanted=false;
// Set while the phone's own cleanup has timed out and asked for host
// recovery, so the worker's escalation never lifts that request.
std::atomic<bool> cleanupFaulted{false};
uint32_t heldToken=0,grantAt=0;
constexpr UBaseType_t kPipePriority=1,kTurnPriority=5;
bool scanStarted=false,scanStartPending=false,scanStopPending=false;
bool candidate=false,published=false,authFailureSeen=false;
uint8_t target[6]{},targetType=0;
uint32_t at=0,jobAt=0,lastStreamAt=0;
// S8: app console traffic, counted by phonePipeNoteConsoleActivity() from any
// task; the owner treats a change since consoleSeen as stream activity.
std::atomic<uint32_t> consoleActivity{0};uint32_t consoleSeen=0;
// A-4: connection parameters, per connection. "applied" only after the
// controller reports success for our request with an interval in the range we
// asked for; refused requests are retried at most three times, 5 s apart, per
// wanted state.
//
// One request at a time. Bluedroid queues a second request behind a running
// one, but its own update timer (2-20 s, l2c_main.c) ends the wait with a
// status-16 event while the controller may still be running the procedure, and
// the C6 answers a new LE Connection Update during that with Command
// Disallowed. Re-asking 5 s later did exactly that on 2026-10-20 (four
// refusals, then the link died 0x22). So "pending" clears only on a real
// answer or a new connection, never on status 16; kParamsAnswerMs is a
// backstop just past the 40 s link-layer response timeout.
//
// The active hold outlasts the app's console polls (every 6 s on 2026-10-20),
// so one burst of polling holds one interval instead of flipping it on every
// poll: about 100 updates in 10 minutes then, and a frame lost about 1 s after
// one of them.
constexpr uint32_t kParamsActiveHoldMs=30000,kParamsAnswerMs=45000;
constexpr uint16_t kParamsActiveMin=24,kParamsActiveMax=36,kParamsIdleMin=72,kParamsIdleMax=96;
constexpr int kParamsHostTimeout=16;  // ESP_BT_STATUS_TIMEOUT: Bluedroid's timer, not an answer
struct ParamState {
  bool applied=false,active=false,wanted=false,pending=false,requested=false;
  bool asked=false;  // the state the request in flight asks for
  uint8_t tries=0;uint16_t interval=0;uint32_t at=0;
};
ParamState params;
uint32_t intent=0,identityGeneration=0;
TransportSessionEpoch enrollmentEpoch=0;
CommandSource enrollmentSource=SOURCE_SERIAL;
uint8_t cachedPsk[32]{};bool pskValid=false;
// Bumped by every phonePipeInvalidateSecret(), under the owner mutex: a key
// derived meanwhile is for a replaced secret and is never published (loadPsk).
uint32_t pskInvalidations=0;
uint16_t nativeAppId=0xffff;
uint32_t lastAuthFailure=0;bool haveAuthFailure=false;
// F3 session counters since boot; owner mutex. No addresses or content.
// endReason and lastEnd are indexed by the pipe error code when it is a known
// one (1..18), else 0. versionMismatches counts strikes, which never reach
// READY. All zero at boot, pipe task and owner commands only: PSRAM.
struct SessionStats {
  uint32_t ready=0,authFailures=0,ended=0,serviceChanged=0,peerReset=0,versionMismatches=0;
  uint32_t endReason[19]{};
  uint16_t lastEnd=0;
};
static_assert(sizeof(SessionStats::endReason)/sizeof(uint32_t)==unsigned(hw1pipe::Error::Paused)+1,
              "one end-reason bucket per pipe error code");
EXT_RAM_BSS_ATTR SessionStats sessionStats;
// The status surfaces' copy (phonePipeLinkSummary), rebuilt under the owner
// mutex every pass and copied under summaryMux, a leaf lock.
EXT_RAM_BSS_ATTR PhonePipeLinkSummary linkSummary;
portMUX_TYPE summaryMux=portMUX_INITIALIZER_UNLOCKED;
// The last session's setup, in ms per step, from the scan that found the
// phone to the pipe's HELLO_ACK (firmware-007). Pipe task only, under the
// owner mutex; all-zero is "none yet", so it may live in PSRAM .bss.
struct SetupTimes {
  bool valid;
  uint32_t scan,connect,discovery,mtu,search,subscribe,secure,hello,total;
};
EXT_RAM_BSS_ATTR SetupTimes lastSetup;
// Reconnect scan (firmware-007): the runtime profile (owner mutex; default at
// boot), the callback's tally for the scan in flight, and the last eight scans.
hw1scan::Profile scanProfile,scanUsed;
hw1coex::Tally scanTally;
// The shared coexistence log (BLE_CoexRecords) keeps the scan and connect
// records. These are the pipe's own pieces of the scan and open in flight, and
// the finished records, which run() hands to the log after releasing the
// owner mutex (the log takes its own lock and prints a line). Pipe task only.
EXT_RAM_BSS_ATTR hw1coex::WifiSpan scanWifi,connectWifi;
EXT_RAM_BSS_ATTR hw1coex::ScanRecord pendingScan;
EXT_RAM_BSS_ATTR hw1coex::ConnectRecord pendingConnect;
bool scanRecordPending=false,connectRecordPending=false;
uint32_t lastScanDurationMs=0;
// The attempt in flight (per job, owner mutex): how long the job waited for
// the setup gate, whether GATT began, and the first stop, which fail()
// latches (it also runs on command tasks) for the pipe task to record.
struct AttemptStop {
  Error why=Error::None;hw1coex::CauseDetail detail=hw1coex::CauseDetail::None;
  bool enrolling=false,gattReady=false,adapterFirst=false;Step step=Step::Idle;
};
AttemptStop attemptStop;
uint32_t gateWaitMs=0;  // admission to grant
bool linkBegun=false,attemptWanted=false;
uint32_t scanBeganAt=0;bool scanOpen=false,scanStopRefused=false;
void endTurn();
void noteScanFailed(uint32_t now);
BlePhoneForget forgetRequest;
bool forgetWanted=false;
TransportSessionEpoch forgetEpoch=0;
CommandSource forgetSource=SOURCE_SERIAL;
// `phone pair new` (phone link v2): the pairing window. While it is open the
// pipe task re-runs the enrollment attempt, kPairRetryMs after the last one
// and never inside the auth cooldown, until READY commits, kPairWindowMs pass
// from the code's issue, the issuing session ends, or it is closed (the OLED
// code screen let go; closephone, secret, forget or pair new). Owner mutex,
// except pairState (atomic, for the status surfaces) and what codeMux guards.
constexpr uint32_t kPairWindowMs=180000,kPairRetryMs=2000;
std::atomic<PhonePairWindow> pairState{PhonePairWindow::None};
uint32_t pairRetryAt=0,pairIds=0;bool pairJobSeen=false;
TransportSessionEpoch pairEpoch=0;
CommandSource pairSource=SOURCE_SERIAL;
// What the OLED code screen reads under codeMux, a leaf lock (the display task
// never takes the owner mutex): the window's id and start, and its code while
// the display shows it, in display form. Internal RAM, never PSRAM; wiped when
// the window closes. Written with the owner mutex held too.
portMUX_TYPE codeMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t pairId=0,pairOpenedAt=0;bool pairOnDisplay=false;
char pairCode[hw1code::kDisplayLen+1]{};
std::atomic<uint32_t> pairCloseWanted{0};  // the window the code screen let go of (any task)

class Lock {
 public:
  explicit Lock(uint32_t wait=50) : held(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(wait))==pdTRUE) {}
  ~Lock(){if(held)xSemaphoreGive(mutex);}
  explicit operator bool() const{return held;}
 private: bool held;
};
class Role {
 public:
  explicit Role(BleRoleTransition kind):held(bleRoleTransitionBegin(kind)){}
  ~Role(){if(held)bleRoleTransitionEnd();}
  explicit operator bool() const{return held;}
 private:bool held;
};
void setStep(Step value,const char* label,uint32_t now){step=value;status=label;at=now;}
void routeSet() {
  portENTER_CRITICAL(&routeMux);
  route.enabled=active.load();
  route.scan=scanParamsPending || step==Step::Scanning || scanStartPending || scanStarted || scanStopPending;
  route.peer=job && candidate;std::memcpy(route.address,target,6);
  portEXIT_CRITICAL(&routeMux);
}
bool matches(const Route& r,const uint8_t* address) {return r.peer && !std::memcmp(r.address,address,6);}
bool hasService(const uint8_t* data,size_t size) {
  static constexpr uint8_t uuid[16]={0xa0,0xc6,0x21,0x50,0x9f,0x3d,0xc4,0xa8,0x2e,0x4b,0x7d,0x6f,0,0,0xe1,0xb7};
  for(size_t offset=0;offset<size;) {
    const size_t length=data[offset++];
    if(!length)break;
    if(length>size-offset)return false;
    if((data[offset]==0x06 || data[offset]==0x07) && (length-1)%16==0)
      for(size_t i=1;i<length;i+=16)if(!std::memcmp(data+offset+i,uuid,16))return true;
    offset+=length;
  }
  return false;
}
bool gapCallback(esp_gap_ble_cb_event_t kind,esp_ble_gap_cb_param_t* p) {
  if(!p)return false;
  Route r;
  portENTER_CRITICAL(&routeMux);r=route;portEXIT_CRITICAL(&routeMux);
  if(!r.enabled)return false;
  GapEvent e;
  switch(kind) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if(!r.scan)return false;
      e.kind=GapKind::ScanParams;e.status=p->scan_param_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if(!r.scan)return false;
      if(!p->scan_start_cmpl.status)scanTally.start(millis());
      e.kind=GapKind::ScanStart;e.status=p->scan_start_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      if(!r.scan)return false;
      if(!p->scan_stop_cmpl.status)scanTally.end(millis());
      e.kind=GapKind::ScanStopped;e.status=p->scan_stop_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if(!r.scan)return false;
      if(p->scan_rst.search_evt==ESP_GAP_SEARCH_INQ_CMPL_EVT){scanTally.end(millis());e.kind=GapKind::ScanDone;break;}
      if(p->scan_rst.search_evt!=ESP_GAP_SEARCH_INQ_RES_EVT)return true;
      scanTally.report();
      if(p->scan_rst.adv_data_len>31 || !hasService(p->scan_rst.ble_adv,p->scan_rst.adv_data_len))return true;
      // Without the duplicate filter the phone repeats on every advertising
      // event; queue only the scan's first usable report (8 queue slots).
      if(!scanTally.match(p->scan_rst.rssi,p->scan_rst.ble_addr_type<=3,millis()))return true;
      e.kind=GapKind::Candidate;e.type=p->scan_rst.ble_addr_type;std::memcpy(e.address,p->scan_rst.bda,6);
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      if(!matches(r,p->ble_security.ble_req.bd_addr))return false;
      // Refuse unsolicited SMP for this peer; never change accessory policy.
      (void)esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr,false);return true;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      if(!matches(r,p->update_conn_params.bda))return false;
      e.kind=GapKind::Params;e.status=p->update_conn_params.status;e.interval=p->update_conn_params.conn_int;break;
    default:return false;
  }
  if(!gapQueue || xQueueSend(gapQueue,&e,0)!=pdTRUE)gapOverflow=true;
  return true;
}
bool gattCallback(esp_gattc_cb_event_t event,esp_gatt_if_t interface,esp_ble_gattc_cb_param_t* p) {
  return phoneGatt.post(event,interface,p);
}

bool currentJob() {
  if(!job)return false;
  if(enrolling && (!enrollmentEpoch || !transportSessionEpochIsLive(enrollmentSource,enrollmentEpoch)))return false;
  return blePeerIntentIsCurrent(BLE_PEER_PHONE,intent,identityGeneration);
}
// READY and the console's link record change together, under the owner mutex.
// The record goes out after ready and is cleared before it, so a valid record
// always means READY; it is set only while this pipe's client holds the link.
void setReady() {
  const PhonePipeLinkRecord record{phoneGatt.generation(),phoneGatt.connection()};
  relayOn=(phoneGatt.pipe.peerFlags()&hw1pipe::kPhoneRelayOn)!=0;  // HELLO_ACK's, before READY
  ready=true;consoleSeen=consoleActivity.load();  // console traffic counts from READY on
  portENTER_CRITICAL(&linkMux);linkRecord=record;linkValid=true;portEXIT_CRITICAL(&linkMux);
}
// Every end of READY goes through here. True when the session was READY.
bool clearReady() {
  portENTER_CRITICAL(&linkMux);linkRecord={};linkValid=false;portEXIT_CRITICAL(&linkMux);
  relayOn=false;
  return ready.exchange(false);
}
void fail(uint32_t now,Error why,const char* label,hw1coex::CauseDetail detail=hw1coex::CauseDetail::None) {
  // The first stop of an attempt that found the phone names its record (the
  // step then leaves Scanning/Link, and READY sets published).
  if(candidate && !published && (step==Step::Scanning || step==Step::Link)) {
    attemptWanted=true;
    attemptStop=AttemptStop{why,detail,enrolling,phoneGatt.ready(),linkBegun && phoneGatt.attemptEnd(),step};
  }
  if(clearReady()) {
    // Codes past 18 never fold onto a known one: they share bucket 0.
    const uint16_t code=uint16_t(why),slot=hw1pipe::knownError(code)?code:0;
    ++sessionStats.ended;++sessionStats.endReason[slot];sessionStats.lastEnd=slot;
    if(phoneGatt.serviceChanged())++sessionStats.serviceChanged;
    if(phoneGatt.pipe.peerReset)++sessionStats.peerReset;
  }
  phoneGatt.stop(now,why);
  if(step!=Step::Retiring && step!=Step::Fault)setStep(Step::Retiring,label,now);
}
void cleanupExpired(uint32_t now,const char* label) {
  setStep(Step::Fault,label,now);
  clearReady();cleanupFaulted=true;recoveryNeeded=true;
  // Keep the native GATT owner and capacity reservation alive for late events,
  // but never hold the setup turn indefinitely after the cleanup bound.
  bleCentralRecoveryBlock(true);endTurn();
}
// Gives the consumed turn back. The route is published first, so the GAP
// interceptor stops claiming scan events before the next client scans.
void leaveSetup() {
  if(!setupHeld)return;
  routeSet();
  setupHeld=false;leaveWanted=false;wantToken.store(0);
  turn.ended.store(heldToken);heldToken=0;
  bleSetupWake();
}
// The job is over: give back a consumed turn, or take back the queued job (a
// grant the worker already made but this task never consumed ends unused).
void endTurn() {
  if(setupHeld){leaveSetup();return;}
  const uint32_t t=wantToken.exchange(0);if(!t)return;
  if(bleSetupWithdraw(BleSetupClient::Phone,t)==BleSetupWithdraw::Running) {
    uint32_t granted=t;(void)turn.granted.compare_exchange_strong(granted,0);
    turn.ended.store(t);bleSetupWake();
  }
}
void configureScan(uint32_t now) {
  scanTally.reset();scanUsed=scanProfile;scanOpen=false;scanStopRefused=false;
  scanWifi.begin(bleCoexWifiNow());scanBeganAt=now;
  scanParamsPending=true;setStep(Step::ScanParams,"scanning",now);routeSet();
  esp_ble_scan_params_t params{};params.scan_type=BLE_SCAN_TYPE_PASSIVE;
  params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;params.scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL;
  // The runtime profile; see hw1scan::Profile for the default. The scan stops
  // at the first pipe advertisement, and the setup deadline is its length + 1 s.
  params.scan_interval=scanUsed.interval;params.scan_window=scanUsed.window;
  params.scan_duplicate=scanUsed.duplicates?BLE_SCAN_DUPLICATE_ENABLE:BLE_SCAN_DUPLICATE_DISABLE;
  if(esp_ble_gap_set_scan_params(&params)!=ESP_OK){
    scanParamsPending=false;noteScanFailed(now);fail(now,Error::Connect,"scan setup failed");
  }
}
void startForget(uint32_t now) {
  if(!transportSessionEpochIsLive(forgetSource,forgetEpoch) ||
     !blePeerPhonePipeForgetIsCurrent(forgetRequest)) {
    forgetWanted=false;status="phone forget authority changed";return;
  }
  const bool committed=blePeerCommitPhonePipeForget(forgetRequest);
  forgetWanted=false;
  if(committed)(void)requestSettingsPersist();
  setStep(Step::Idle,committed?"phone pipe enrollment forgotten":"phone enrollment changed",now);
}
// One record per scan: whether the phone's advertisement was heard, and how
// much the scan heard at all, timed by the controller's start and end when
// both were seen. No addresses. run() hands it to the shared log.
hw1coex::ScanRecord scanRecord(uint32_t now,hw1coex::ScanOutcome caller) {
  hw1coex::ScanRecord r{};
  const auto timing=hw1coex::concludeScan(scanTally,caller,scanBeganAt,now,scanUsed.seconds);
  r.endedAt=now;r.durationMs=timing.durationMs;r.controllerTimed=timing.controller;r.outcome=timing.outcome;
  r.reports=scanTally.reports.load();r.matches=scanTally.matches.load();r.rssi=int8_t(scanTally.bestRssi.load());
  r.firstMatchMs=scanTally.firstMatchMs();
  r.peer=hw1coex::Peer::Phone;r.purpose=enrolling?hw1coex::Purpose::Enroll:hw1coex::Purpose::Reconnect;
  r.interval=scanUsed.interval;r.window=scanUsed.window;r.active=false;r.duplicateFilter=scanUsed.duplicates;
  r.seconds=scanUsed.seconds;
  r.wifi=scanWifi;r.wifi.finish(bleCoexWifiNow());
  return r;
}
void noteScan(uint32_t now,bool cut) {
  if(!scanOpen)return;
  scanOpen=false;
  pendingScan=scanRecord(now,scanTally.matches.load()?hw1coex::ScanOutcome::Found
                             :cut?hw1coex::ScanOutcome::Aborted:hw1coex::ScanOutcome::Missed);
  lastScanDurationMs=pendingScan.durationMs;scanRecordPending=true;
}
// The scan never started: its parameters or start refused, synchronously or
// by the controller.
void noteScanFailed(uint32_t now) {
  pendingScan=scanRecord(now,hw1coex::ScanOutcome::StartFailed);scanRecordPending=true;
}
// One record per attempt that found the phone, from GATT registration to
// READY or the first stop: the step reached, the first non-OK GATT status,
// the phone's own disconnect reason and the cause. Pipe task only (fail()
// latches the stop); run() hands it to the shared log. No addresses.
void noteAttempt(uint32_t now,bool readyNow) {
  using S=hw1coex::ConnectStage;
  hw1coex::ConnectRecord r{};
  r.peer=hw1coex::Peer::Phone;r.endedAt=now;
  r.waitMs=hw1coex::ConnectRecord::capWait(gateWaitMs);
  const uint32_t began=phoneGatt.enteredAt(Gatt::Phase::Registering);
  if(readyNow) {
    r.purpose=enrolling?hw1coex::Purpose::Enroll:hw1coex::Purpose::Reconnect;
    r.outcome=hw1coex::ConnectOutcome::Connected;r.stage=S::Ready;r.gattStatus=-1;r.elapsedMs=now-began;
  } else {
    const auto& stop=attemptStop;
    r.purpose=stop.enrolling?hw1coex::Purpose::Enroll:hw1coex::Purpose::Reconnect;
    const auto* end=linkBegun?phoneGatt.attemptEnd():nullptr;
    // The adapter's own first stop (a deadline, a link loss, the phone's
    // RESET) outranks a cancel that came after it, whose source then does not
    // apply.
    const Error why=end?end->error:stop.why;
    r.cause=uint8_t(why);r.detail=stop.adapterFirst?hw1coex::CauseDetail::None:stop.detail;
    r.outcome=why==Error::Cancelled?hw1coex::ConnectOutcome::Cancelled:hw1coex::ConnectOutcome::Failed;
    r.gattStatus=-1;
    if(end) {
      r.stage=stop.gattReady?S::Ready:end->stage;r.gattStatus=end->gatt;r.reason=end->reason;
      r.endedAt=end->at;r.elapsedMs=end->at-began;
      if(end->serviceChanged && r.detail==hw1coex::CauseDetail::None)r.detail=hw1coex::CauseDetail::ServiceChanged;
    } else {
      // GATT never began (the scan was cut after the match, or registration
      // was unavailable), or it ended before registering.
      r.stage=linkBegun?S::RegisterEvent:stop.step==Step::Link?S::RegisterApi:S::Precheck;
      r.elapsedMs=linkBegun?now-began:0;
    }
  }
  r.wifi=connectWifi;r.wifi.finish(bleCoexWifiNow());
  pendingConnect=r;connectRecordPending=true;
}
void noteAttemptIfWanted(uint32_t now) {
  if(attemptWanted){attemptWanted=false;noteAttempt(now,false);}
}
// After the owner mutex is released (pipe task only).
void flushRecords() {
  if(scanRecordPending){scanRecordPending=false;bleCoexRecordScan(pendingScan);}
  if(connectRecordPending){connectRecordPending=false;bleCoexRecordConnect(pendingConnect);}
}
void consume(const GapEvent& e,uint32_t now) {
  const bool retiring=step==Step::Retiring || step==Step::Fault;
  switch(e.kind) {
    case GapKind::ScanParams:
      if(!scanParamsPending)break;  // not ours (F3)
      scanParamsPending=false;
      if(e.status){noteScanFailed(now);fail(now,Error::Connect,"scan setup failed");break;}
      if(retiring)break;
      scanStartPending=true;setStep(Step::Scanning,"scanning",now);routeSet();
      if(esp_ble_gap_start_scanning(scanUsed.seconds)!=ESP_OK){
        scanStartPending=false;noteScanFailed(now);fail(now,Error::Connect,"scan start failed");
      }
      break;
    case GapKind::ScanStart:
      if(!scanStartPending)break;   // not ours (F3)
      scanStartPending=false;scanStarted=e.status==0;
      if(scanStarted)scanOpen=true;
      if(e.status){noteScanFailed(now);fail(now,Error::Connect,"scan start failed");}
      break;
    case GapKind::Candidate:
      if(retiring || candidate || e.type>3)break;
      // UUID finds a candidate, including a rotating address. Only the new
      // dedicated-key handshake may authenticate it.
      std::memcpy(target,e.address,6);targetType=e.type;candidate=true;connectWifi.begin(bleCoexWifiNow());
      // Stop listening at the first match instead of at the scan's end; the
      // stop's answer (or the scan's own end, if they cross) starts the link.
      if(scanStarted && !scanStopPending && !scanStopRefused) {
        scanStopPending=esp_ble_gap_stop_scanning()==ESP_OK;
        scanStopRefused=!scanStopPending;  // the scan's own end still arrives
      }
      routeSet();break;
    case GapKind::ScanDone: case GapKind::ScanStopped:
      // Only our own scan's end counts (F3). Bluedroid answers our stop with
      // ScanStopped and no ScanDone, and its timer ends a scan with ScanDone.
      // It fails a stop only when no scan is running, i.e. the stop crossed
      // the timer, so either answer means the scan is over and no pending
      // flag is left behind (F1).
      if(e.kind==GapKind::ScanStopped){if(!scanStopPending)break;scanStopPending=false;}
      if(!scanStarted && !scanStartPending)break;  // already over: a late answer
      scanStarted=scanStartPending=false;
      noteScan(now,retiring);
      if(retiring)break;
      if(!candidate){fail(now,Error::Connect,"phone not found");break;}
      setStep(Step::Link,"authenticating pipe",now);routeSet();
      if(!pskValid || !BLEDevice::allocateAppId(nativeAppId)) {
        fail(now,Error::Resources,"GATT registration unavailable");break;
      }
      if(!phoneGatt.begin(nativeAppId,target,targetType,now,cachedPsk)) {
        BLEDevice::releaseAppId(nativeAppId);nativeAppId=0xffff;
        fail(now,Error::Resources,"GATT registration unavailable");break;
      }
      linkBegun=true;liveGeneration=phoneGatt.generation();for(auto& id:releaseIds)id=0;
      break;
    case GapKind::Params: {
      if(!e.status)params.interval=e.interval; // also when the phone itself renegotiates
      if(e.status==kParamsHostTimeout)break;   // still in flight: the real answer follows
      // Credit the state that was asked for, and only when the link landed in
      // its range: the wanted state may have moved on while the request ran.
      const bool inRange=params.asked ? e.interval>=kParamsActiveMin && e.interval<=kParamsActiveMax
                                      : e.interval>=kParamsIdleMin && e.interval<=kParamsIdleMax;
      if(params.pending && !e.status && inRange){params.applied=true;params.active=params.asked;params.tries=0;}
      else if(params.pending && !e.status)params.applied=false;  // counter-offered: retried 5 s apart
      params.pending=false;
      break;
    }
  }
}
void noteSetup(uint32_t now) {
  using P=Gatt::Phase;
  const auto at=[](P p){return phoneGatt.enteredAt(p);};
  const uint32_t found=phoneGatt.discoveredAt();
  SetupTimes s{};s.valid=true;
  s.scan=lastScanDurationMs;
  s.connect=at(P::Mtu)-at(P::Registering);            // app registration and LE connect
  s.discovery=found?found-at(P::Mtu):0;                // Bluedroid's automatic discovery
  s.mtu=at(P::Discovering)-(found?found:at(P::Mtu));   // the queued Exchange MTU
  s.search=at(P::Subscribe)-at(P::Discovering);
  s.subscribe=at(P::Secure)-at(P::Subscribe);          // notifications and the CCCD write
  s.secure=at(P::Pipe)-at(P::Secure);                  // the secure-channel handshake
  s.hello=now-at(P::Pipe);                             // pipe HELLO / HELLO_ACK
  s.total=now-at(P::Registering);
  lastSetup=s;
  ESP_LOGI("PIPE_SETUP","scan=%lu connect=%lu discovery=%lu mtu=%lu search=%lu subscribe=%lu secure=%lu hello=%lu total=%lu",
      (unsigned long)s.scan,(unsigned long)s.connect,(unsigned long)s.discovery,(unsigned long)s.mtu,
      (unsigned long)s.search,(unsigned long)s.subscribe,(unsigned long)s.secure,(unsigned long)s.hello,
      (unsigned long)s.total);
}
bool authCoolingDown(uint32_t now) {
  return haveAuthFailure && !hw1pipe::expired(now,lastAuthFailure,30000);
}
void beginJob(uint32_t now) {
  candidate=published=authFailureSeen=false;
  linkBegun=false;gateWaitMs=now-jobAt;
  scanStarted=scanStartPending=scanStopPending=false;at=now;
  params={};lastStreamAt=now-kParamsActiveHoldMs; // a new connection starts idle with nothing requested
  if(!pskValid || authCoolingDown(now)){fail(now,Error::Unauthorized,"pipe authentication cooldown or secret unavailable");return;}
  configureScan(now);
}
// One enrollment attempt (`phone pair`, and each try of `phone pair new`'s
// window) for the admin session that asked, queued on the setup worker;
// nullptr when queued. Owner mutex held, step Idle with no job.
const char* startEnrollmentLocked(uint32_t now,TransportSessionEpoch epoch,CommandSource source) {
  uint32_t t=0;if(!bleSetupClaim(BleSetupClient::Phone,&t))return "Error: phone pipe busy; disconnect first";
  if(!blePeerBeginManualLearn(BLE_PEER_PHONE,intent,identityGeneration)) {
    bleSetupCancelClaim(BleSetupClient::Phone,t);return "Error: enrollment authority unavailable";
  }
  enrollmentEpoch=epoch;enrollmentSource=source;
  enrolling=job=true;request={};jobAt=now;wantToken.store(t);setStep(Step::Waiting,"waiting to enroll",jobAt);
  if(!bleSetupPublish(BleSetupClient::Phone,t)) {
    bleSetupCancelClaim(BleSetupClient::Phone,t);wantToken.store(0);job=enrolling=false;
    setStep(Step::Idle,"disconnected",jobAt);return "Error: phone pipe busy; disconnect first";
  }
  return nullptr;
}
// Opens `phone pair new`'s window for the session that issued the code. shown
// (display form) is kept for the OLED code screen, or nullptr when serial
// printed it. Owner mutex held. The window's id (never 0).
uint32_t openPairWindow(uint32_t now,TransportSessionEpoch epoch,CommandSource source,const char* shown) {
  pairRetryAt=now;pairJobSeen=false;pairCloseWanted=0;
  if(!++pairIds)++pairIds;
  portENTER_CRITICAL(&codeMux);
  // The issuing session goes with the code: the code screen draws it only
  // while that session is live (phonePipePairView).
  pairEpoch=epoch;pairSource=source;
  pairId=pairIds;pairOpenedAt=now;pairOnDisplay=shown!=nullptr;
  if(shown)std::memcpy(pairCode,shown,sizeof(pairCode));
  portEXIT_CRITICAL(&codeMux);
  pairState=PhonePairWindow::Open;
  return pairIds;
}
// Ends the open window: no more attempts, its code wiped. Owner mutex held.
void endPairWindow(PhonePairWindow why) {
  if(pairState.load()!=PhonePairWindow::Open)return;
  portENTER_CRITICAL(&codeMux);hw1code::wipe(pairCode,sizeof(pairCode));pairOnDisplay=false;portEXIT_CRITICAL(&codeMux);
  pairState=why;pairCloseWanted=0;
}
// The user ended an open window (the code screen let go, phone forget): an
// attempt in flight is cancelled. True when one was open. Owner mutex held.
bool closePairWindowLocked(uint32_t now) {
  if(pairState.load()!=PhonePairWindow::Open)return false;
  const bool attempt=job && enrolling && step!=Step::Retiring && step!=Step::Fault;
  endPairWindow(PhonePairWindow::Closed);
  if(attempt)fail(now,Error::Cancelled,"pairing code closed",hw1coex::CauseDetail::Stopped);
  else status="pairing code closed";
  return true;
}
void tickLocked(uint32_t now) {
  noteAttemptIfWanted(now);  // a stop from a command task since the last pass
  if(hostResetWanted.load()) {
    phoneGatt.hostStopped();forgetWanted=false;
    if(nativeAppId!=0xffff){BLEDevice::releaseAppId(nativeAppId);nativeAppId=0xffff;}liveGeneration=0;
    for(auto& id:releaseIds)id=0;
    active=false;clearReady();stopWanted=false;bleCentralPhoneEnabled(false);
    scanParamsPending=false;
    job=enrolling=scanStarted=scanStartPending=scanStopPending=false;
    endTurn();bleCentralLinkReleaseClient(&phoneGatt);
    cleanupFaulted=false;recoveryNeeded=false;quiesceWanted=false;quiesced=false;
    setStep(Step::Idle,"stopped",now);routeSet();hostResetWanted=false;
  }
  if(quiesceWanted.load()) {
    clearReady();active=false;endTurn();quiesced=true;routeSet();return;
  }
  if(!active.load())return;
  if(abortLink.exchange(false))fail(now,Error::Resources,"stream cleanup could not be queued");
  if(gapOverflow.exchange(false))fail(now,Error::Resources,"BLE callback queue full");
  GapEvent event;
  for(unsigned i=0;i<8 && xQueueReceive(gapQueue,&event,0)==pdTRUE;++i)consume(event,now);
  // A link failure still queued in this tick is consumed after the cancel, so
  // its attempt reads as the cancel (the record's 10 ms blind spot). A setup
  // in progress is never cancelled for another client's setup: the setup
  // worker gives the next client its turn when this one ends
  // (docs/BLE_SETUP_WORKER_PLAN.md). Enrollment has 60 s from its turn, and
  // at most 60 s waiting for it.
  if(job) {
    if(stopWanted.load())fail(now,Error::Cancelled,"phone close requested",hw1coex::CauseDetail::Stopped);
    else if(!currentJob())fail(now,Error::Cancelled,"phone intent changed",hw1coex::CauseDetail::Replaced);
    else if(enrolling && hw1pipe::expired(now,setupHeld?grantAt:jobAt,60000))
      fail(now,Error::Cancelled,"phone enrollment expired",hw1coex::CauseDetail::Expired);
    else if(setupHeld && !hw1setup::grantHeld(turn,heldToken))
      fail(now,Error::Deadline,"BLE setup turn overran");
  }
  // Relays defer within their 2 s budget for the display, a setup or the mic
  // AFE; the console yields to the display alone, for as long as it lasts.
  const bool displayBusy=bleCentralTxAccessoryPressure();
  const bool pressure=displayBusy || bleSetupAccessoryBusy() || g2MicAfeFeedIsActive();
  // CONSOLE_OFFERED for the HELLO, and a LINK_STATE when it changes.
  phoneGatt.pipe.setLocalFlags(pipeConsoleLocalFlags());
  phoneGatt.poll(now,pressure,displayBusy);
  if(ready.load())relayOn=(phoneGatt.pipe.peerFlags()&hw1pipe::kPhoneRelayOn)!=0;  // LINK_STATE
  for(auto& id:releaseIds)if(id) {
    auto* stream=phoneGatt.pipe.find(id);
    if(!stream || !stream->live()){phoneGatt.pipe.release(id);id=0;}
  }
  if(step==Step::Waiting) {
    const uint32_t t=wantToken.load();
    if(hw1setup::consumeGrant(turn,t)) {
      setupHeld=true;heldToken=t;grantAt=now;
      BleCentralLinkGuard link(BleCentralLink::Phone,ENABLE_G2_GLASSES);  // the arms keep their links where glasses can open
      if(!link) {
        job=false;enrolling=false;
        setStep(Step::Idle,"central link capacity unavailable",now);leaveSetup();
      } else {link.retain(&phoneGatt);beginJob(now);}
    } else if(!bleSetupLive(BleSetupClient::Phone,t)) {
      // The worker ended this job without a turn (the pipe stopping, or a
      // grant that went unused): never stay Waiting and answer BUSY forever.
      const bool nudge=!enrolling && request.explicitReseek && !request.autoReconnect;
      job=false;enrolling=false;wantToken.store(0);
      setStep(Step::Idle,"setup turn ended unused",now);
      if(nudge)(void)blePeerRestoreDroppedReseek(BLE_PEER_PHONE,intent,identityGeneration);
    }
  }
  if(step==Step::Link) {
    if(phoneGatt.authenticationFailed() && !authFailureSeen) {
      authFailureSeen=true;haveAuthFailure=true;lastAuthFailure=now;++sessionStats.authFailures;
    }
    if(phoneGatt.ready() && !published) {
      bool persist=false;
      const bool accepted=currentJob() && (enrolling
          ? blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identityGeneration,&persist)
          : blePeerNoteLinkUpIfCurrent(BLE_PEER_PHONE,request,&intent,&identityGeneration));
      if(accepted && persist)(void)requestSettingsPersist();
      if(!accepted){fail(now,Error::Cancelled,"phone intent replaced",hw1coex::CauseDetail::Replaced);}
      else {
        noteAttempt(now,true);
        // The turn goes back once no scan flag is left (below), so the route
        // has stopped claiming scan events.
        published=true;enrolling=false;setReady();status="ready";leaveWanted=true;++sessionStats.ready;
        endPairWindow(PhonePairWindow::Paired);  // the first success ends `phone pair new`'s window
        versionStrikes=0;if(versionLatched.exchange(false))bleCentralPhoneEnabled(true);  // it speaks v2
        noteSetup(now);
      }
    }
    if(phoneGatt.idle() || phoneGatt.phase()==Gatt::Phase::Retiring || phoneGatt.phase()==Gatt::Phase::Quarantined) {
      // A strike: this attempt's HELLO went out and the session ended before
      // HELLO_ACK by the phone's version RESET or by silence (the core latched
      // which; start() resets it, so the attempt must have reached Pipe).
      if(!published && phoneGatt.enteredAt(Gatt::Phase::Pipe) && phoneGatt.pipe.helloEnd()!=hw1pipe::HelloEnd::None) {
        ++sessionStats.versionMismatches;
        const uint8_t strikes=uint8_t(versionStrikes.load()<255?versionStrikes.load()+1:255);versionStrikes=strikes;
        if(strikes>=3) {
          // Held: the ring gets its link back while the phone cannot use one.
          versionLatched=true;bleCentralPhoneEnabled(false);
          fail(now,phoneGatt.pipe.error,"phone app too old");
          // The hold ends `phone pair new`'s window too: its attempts skip
          // admit() and would keep claiming a link the ring may now hold. The
          // reason stays shown; `phone pair new` after the app update lifts it.
          endPairWindow(PhonePairWindow::Closed);
        } else fail(now,phoneGatt.pipe.error,"phone app version mismatch");
      } else fail(now,phoneGatt.pipe.error,"phone link lost");
    }
    if(ready.load()) {
      // A-4: relay streams and the console's own traffic (COMMAND/BINARY in,
      // TEXT/BLOB out, noted by its driver) are activity; an idle open console
      // stream is not, nor are cards, the mirror or PINGs.
      const bool stream=phoneGatt.pipe.relayLive();
      // S8: app console traffic on this link since the last pass counts too.
      const uint32_t console=consoleActivity.load();
      if(stream || console!=consoleSeen)lastStreamAt=now;
      consoleSeen=console;
      const bool wantActive=stream || !hw1pipe::expired(now,lastStreamAt,kParamsActiveHoldMs);
      if(params.pending && hw1pipe::expired(now,params.at,kParamsAnswerMs))params.pending=false; // never answered
      if(wantActive!=params.wanted){params.wanted=wantActive;params.tries=0;}
      if(!(params.applied && params.active==wantActive) && !params.pending && params.tries<3 &&
         (!params.requested || hw1pipe::expired(now,params.at,5000))) {
        esp_ble_conn_update_params_t request{};std::memcpy(request.bda,target,6);
        request.min_int=wantActive?kParamsActiveMin:kParamsIdleMin;
        request.max_int=wantActive?kParamsActiveMax:kParamsIdleMax;
        request.latency=wantActive?0:4;request.timeout=600;
        params.requested=true;params.at=now;++params.tries;params.asked=wantActive;
        params.pending=esp_ble_gap_update_conn_params(&request)==ESP_OK;
      }
    }
  }
  if(leaveWanted && !scanParamsPending && !scanStarted && !scanStartPending && !scanStopPending)leaveSetup();
  if((step==Step::ScanParams || step==Step::Scanning) &&
     hw1pipe::expired(now,at,scanUsed.seconds*1000u+1000u))fail(now,Error::Deadline,"BLE setup timed out");
  if(step==Step::Retiring || step==Step::Fault) {
    if(scanStarted && !scanStopPending && !scanStopRefused) {
      // A refused stop leaves the scan to end on its own timer (F1).
      scanStopPending=esp_ble_gap_stop_scanning()==ESP_OK;
      scanStopRefused=!scanStopPending;
    }
    if(phoneGatt.idle() && !scanParamsPending &&
       !scanStarted && !scanStartPending && !scanStopPending) {
      const bool lost=job;job=false;enrolling=false;clearReady();
      // Publish the idle route before the next setup can start: while
      // route.scan still reads true the GAP interceptor swallows the next
      // client's scan events (consume() ends a retiring scan without routeSet).
      // An attempt that never reached READY keeps its failure ("phone not
      // found", "BLE setup timed out"); only a link that was up reads disconnected.
      setStep(Step::Idle,stopWanted.load()?"stopped":published?"disconnected":status.load(),now);routeSet();
      if(nativeAppId!=0xffff){BLEDevice::releaseAppId(nativeAppId);nativeAppId=0xffff;}
      bleCentralLinkReleaseClient(&phoneGatt);
      // Lift the block before the turn is given back, so the worker's next
      // pick is not refused. A turn the worker revoked and escalated meanwhile
      // is settled by the second check (the worker re-checks too).
      const bool held=setupHeld;
      cleanupFaulted=false;
      if(recoveryNeeded.exchange(false))bleCentralRecoveryBlock(false);
      endTurn();
      if(held && recoveryNeeded.exchange(false))bleCentralRecoveryBlock(false);
      if(lost)blePeerNoteLinkLost(BLE_PEER_PHONE);
    } else if(!recoveryNeeded.load() &&
        hw1pipe::expired(now,at,scanStopRefused?scanUsed.seconds*1000u+1000u:4000u))
      cleanupExpired(now,"phone cleanup incomplete");
  }
  if(forgetWanted && step==Step::Idle)startForget(now);
  if(stopWanted.load() && step==Step::Idle && !forgetWanted) {
    active=false;bleCentralPhoneEnabled(false);status="stopped";routeSet();
  }
  noteAttemptIfWanted(now);
  routeSet();
}
// `phone pair new`'s window, every pass after the step machine: its ends, then
// the next attempt once the last one is over (kPairRetryMs after it ended, the
// setup turn given back in between) and the auth cooldown has passed.
void pairTickLocked(uint32_t now) {
  if(pairState.load()!=PhonePairWindow::Open)return;
  const uint32_t letGo=pairCloseWanted.exchange(0);
  if(!active.load() || stopWanted.load())endPairWindow(PhonePairWindow::Closed);  // the stop ends its attempt
  else if(letGo && letGo==pairId)(void)closePairWindowLocked(now);
  else if(hw1pipe::expired(now,pairOpenedAt,kPairWindowMs)) {
    const bool attempt=job && enrolling && step!=Step::Retiring && step!=Step::Fault;
    endPairWindow(PhonePairWindow::Expired);
    if(attempt)fail(now,Error::Cancelled,"pairing code expired",hw1coex::CauseDetail::Expired);
    else status="pairing code expired";
  } else if(!transportSessionEpochIsLive(pairSource,pairEpoch)) {
    // The issuing admin's session ended; an attempt in flight already failed
    // its own epoch check in the step machine.
    endPairWindow(PhonePairWindow::Closed);status="pairing code closed";
  } else if(job)pairJobSeen=true;
  else {
    if(pairJobSeen){pairJobSeen=false;pairRetryAt=now+kPairRetryMs;}  // 2 s from the end of the last attempt
    if(step==Step::Idle && !forgetWanted && !authCoolingDown(now) && int32_t(now-pairRetryAt)>=0 &&
       startEnrollmentLocked(now,pairEpoch,pairSource))
      pairRetryAt=now+kPairRetryMs;  // the setup worker or the peer authority refused: later
  }
}
// The status surfaces' snapshot (phonePipeLinkSummary): counts and flags only.
void publishSummary() {
  PhonePipeLinkSummary s;
  s.active=active.load();s.ready=ready.load();
  const uint8_t peer=s.ready?phoneGatt.pipe.peerFlags():0;
  s.relayOn=s.ready && relayOn.load();s.consoleWanted=(peer&hw1pipe::kPhoneConsoleWanted)!=0;
  // What the session announces, as this pass's tickLocked set it; a stopped
  // pipe announces nothing. Never the secret itself: `phone secret`, `phone
  // pair new` and a factory reset rewrite it, off this mutex, once stopped.
  s.consoleOffered=s.active && (phoneGatt.pipe.localFlags()&hw1pipe::kP4ConsoleOffered)!=0;
  s.versionLatched=versionLatched.load();s.strikes=versionStrikes.load();
  for(const auto& stream:phoneGatt.pipe.streams)if(stream.live())++(stream.console?s.consoleStreams:s.relayStreams);
  const auto& st=sessionStats;
  s.ended=st.ended;s.lastEnd=st.lastEnd;s.versionMismatches=st.versionMismatches;
  std::memcpy(s.endReason,st.endReason,sizeof(s.endReason));
  portENTER_CRITICAL(&summaryMux);linkSummary=s;portEXIT_CRITICAL(&summaryMux);
}
// One pass of the pipe task, owner mutex held. The console driver and the
// snapshot run after the step machine on every pass, a stopped pipe's
// included, so both see a session's end in the pass that ended it.
void passLocked(uint32_t now) {
  tickLocked(now);
  pairTickLocked(now);
  pipeConsoleTickLocked(phoneGatt.pipe,phoneGatt.generation(),ready.load(),now,bleCentralTxAccessoryPressure());
  publishSummary();
  idleOnHost=!active.load() && step==Step::Idle && phoneGatt.idle() && nativeAppId==0xffff &&
             !job && !forgetWanted && pairState.load()!=PhonePairWindow::Open &&
             !scanStarted && !scanStartPending && !scanStopPending && !scanParamsPending &&
             !hostResetWanted.load() && !quiesceWanted.load();
}
// The published handle, under the lock resources() publishes it with.
TaskHandle_t workerHandle() {
  portENTER_CRITICAL(&initMux);const TaskHandle_t handle=worker;portEXIT_CRITICAL(&initMux);
  return handle;
}
// The task's exit check, once per pass after its records are flushed. It
// leaves once the pipe is inactive with no host reset pending, on one of two
// grounds: a pending release (the reset branch of tickLocked has run on this
// task — the GATT adapter told of the host stop, the app id released, the
// pair window ended, the turn given back — and the pass that ran it
// published the "stopped" summary and flushed the console and coex records),
// or its own idle exit on an up host (idleRetire: kPipeIdleRetireMs of
// idleOnHost with no phone setup turn held, run() keeps that clock; the pass
// that found the pipe idle published "stopped" the same way). A reset wanted
// after the release was asked keeps the task for the pass that runs it. The
// handle is nulled by the task itself, under the lock its creator checks, in
// the same step that consumes the request; active is read in that step too,
// against phoneLinkOpen, which publishes active and then looks for the task
// under the same lock (so it finds this one, or none and makes one).
bool leaveIfReleased(bool idleRetire) {
  portENTER_CRITICAL(&initMux);
  const bool leave=(exitWanted || idleRetire) && !active.load() && !hostResetWanted.load();
  if(leave){worker=nullptr;exitWanted=false;}
  portEXIT_CRITICAL(&initMux);
  return leave;
}
void run(void*) {
  uint32_t idleSince=0;  // first pass that found the pipe idle on an up host (0: none)
  for(;;) {
    {Lock lock;if(lock)passLocked(millis());}
    pipeConsoleAfterUnlock();  // binds or ends the console's row: lifecycle lock, owner mutex released
    flushRecords();
    const uint32_t now=millis();
    if(!idleOnHost.load())idleSince=0;else if(!idleSince)idleSince=now?now:1;
    // A phone setup turn claimed, queued, running or finishing sets this task's
    // priority by handle: the idle exit waits for the slot, as a release does.
    const bool idleRetire=idleSince && hw1pipe::expired(now,idleSince,kPipeIdleRetireMs) &&
                          bleSetupIdle(hw1setup::clientMask(hw1setup::Client::Phone));
    // The summary this task last published reads "stopped" and is final: the
    // status surfaces stay right with no task ticking.
    if(leaveIfReleased(idleRetire))vTaskDelete(nullptr);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
bool resources() {
  portENTER_CRITICAL(&initMux);
  if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutexStorage);
  if(!gapQueue)gapQueue=xQueueCreateStatic(8,sizeof(GapEvent),gapQueueBytes,&gapQueueStorage);
  portEXIT_CRITICAL(&initMux);
  if(!mutex || !gapQueue)return false;
  if(!worker && xTaskCreateLogged(run,"phone_pipe",8192,nullptr,kPipePriority,&worker,"BLE phone pipe")!=pdPASS)return false;
  return true;
}
BlePeerConnectAdmission admit(const BlePeerConnectRequest& wanted) {
  // A closed pipe on an up client stack opens for the reconnect (boot,
  // `openble`, a recycle), as `openphone` opens it; with the stack down there
  // is nothing to open on (the scheduler never starts the stack).
  if(!active.load() && (!bleStackReady() || !phoneLinkOpen()))return BlePeerConnectAdmission::ROLE_BLOCKED;
  // A version-mismatch hold keeps the scheduler's intent (it retries every
  // 15 s without work) until a user's `openphone` or `phone pair` lifts it.
  if(stopWanted.load() || versionLatched.load())return BlePeerConnectAdmission::ROLE_BLOCKED;
  // Glasses may be off. In G2 mode the arms keep their capacity through
  // reserveArms (Waiting step) and win setup through the accessory gate.
  Lock lock;if(!lock)return BlePeerConnectAdmission::BUSY;
  if(ready.load())return BlePeerConnectAdmission::ALREADY_UP;
  if(job || forgetWanted || step!=Step::Idle || authCoolingDown(millis()))return BlePeerConnectAdmission::BUSY;
  if(!blePeerConnectRequestIsCurrent(BLE_PEER_PHONE,wanted))return BlePeerConnectAdmission::NO_TARGET;
  uint32_t t=0;if(!bleSetupClaim(BleSetupClient::Phone,&t))return BlePeerConnectAdmission::BUSY;
  request=wanted;intent=wanted.intentGeneration;identityGeneration=wanted.identityGeneration;
  enrolling=false;job=true;jobAt=millis();wantToken.store(t);setStep(Step::Waiting,"waiting for radio",jobAt);
  if(!bleSetupPublish(BleSetupClient::Phone,t)) {
    bleSetupCancelClaim(BleSetupClient::Phone,t);wantToken.store(0);job=false;
    setStep(Step::Idle,"disconnected",jobAt);return BlePeerConnectAdmission::BUSY;
  }
  return BlePeerConnectAdmission::STARTED;
}
void disconnect() {
  // Hold the pipe across both steps: a tick between them would see the bumped
  // intent first and record the user's stop as "replaced".
  Lock lock;
  blePeerNoteUserDisconnect(BLE_PEER_PHONE);
  if(!lock)return;
  const uint32_t now=millis();
  // It ends `phone pair new`'s window too: its next attempt's manual learn
  // would clear the user's disconnect.
  (void)closePairWindowLocked(now);
  if(job)fail(now,Error::Cancelled,"disconnected",hw1coex::CauseDetail::Stopped);
}
// A stopped pipe gives its hold on gBLEState back, which ends its console row
// (the lifecycle lock: never with the owner mutex held).
void giveStateBack(){if(stateHeld.exchange(false))blePipeConsoleStateRelease();}
// The pipe key for the stored secret, for phonePipeStart: 100k PBKDF2 rounds
// off the owner mutex into this stack, published under it only if no
// phonePipeInvalidateSecret() ran since the key was found missing. A secret
// change racing a background start (boot, the radio companion, stuck-phone
// recovery) then never leaves a key for neither secret: that start fails and
// the next derives the new key. Retry: such a race, or the owner mutex busy.
enum class PskLoad : uint8_t { Ready, NoSecret, Retry };
PskLoad loadPsk() {
  uint32_t seen=0;
  {Lock lock(1000);if(!lock)return PskLoad::Retry;if(pskValid)return PskLoad::Ready;seen=pskInvalidations;}
  const String& secret=gSettings.phonePipeSecret;
  uint8_t psk[sizeof(cachedPsk)];
  const bool derived=secret.length() && sodium_init()>=0 &&
      hw1sc::scPbkdf2(reinterpret_cast<const uint8_t*>(secret.c_str()),secret.length(),
          reinterpret_cast<const uint8_t*>(hw1phone::kPipeLabel),strlen(hw1phone::kPipeLabel),100000,psk);
  PskLoad got=derived?PskLoad::Retry:PskLoad::NoSecret;
  if(derived){Lock lock(1000);
    if(lock && pskInvalidations==seen){std::memcpy(cachedPsk,psk,sizeof(psk));pskValid=true;got=PskLoad::Ready;}}
  sodium_memzero(psk,sizeof(psk));
  return got;
}
const BlePeerOps ops{nullptr,disconnect,phonePipeReady,admit};
const BlePeerSpec spec{BLE_PEER_PHONE,"phone","Phone pipe",1,true,&ops};

// The phone's client on the setup worker. Its setup runs here on phone_pipe;
// the worker grants the turn and waits for it to end (hw1setup::runPhoneTurn).
struct TurnEnv {
  uint32_t now(){return millis();}
  void waitWake(uint32_t ms){bleSetupWaitWake(ms);}
  void grant(uint32_t t){if(worker)vTaskPrioritySet(worker,kTurnPriority);turn.granted.store(t);}
  void release(uint32_t){if(worker)vTaskPrioritySet(worker,kPipePriority);}
  void overran(uint32_t,bool used) {
    ESP_LOGW("BLE_SETUP","phone turn overran %lu s%s",(unsigned long)(hw1setup::kPhoneTurnCeilingMs/1000),
             used?"; stopping the phone setup":", unused");
  }
  // The block first, so a concurrent clean end either lifts both or neither.
  void escalate(uint32_t) {
    bleCentralRecoveryBlock(true);recoveryNeeded.store(true);
    ESP_LOGE("BLE_SETUP","phone setup did not stop; host recovery requested");
  }
  // Undo the escalation only: a request from the phone's own timed-out
  // cleanup stays until that cleanup ends.
  void settle(uint32_t){if(!cleanupFaulted.load() && recoveryNeeded.exchange(false))bleCentralRecoveryBlock(false);}
};
const char* setupLabel(uint32_t){return "phone";}
hw1setup::Verdict setupReady(uint32_t t,const hw1setup::WaitInfo& info) {
  if(!active.load() || stopWanted.load() || quiesceWanted.load() || wantToken.load()!=t)
    return {hw1setup::Ready::Drop,hw1setup::kInactive};
  if(bleCentralRecoveryBlocked())return {hw1setup::Ready::Wait,hw1setup::kRecovery};
  // On a C6 with fewer than four links the ring gives its link up for the
  // pipe; until its retirement completes the phone cannot get one. Wait for
  // it, at most 10 s.
  if(bleCentralRingShouldYield() && bleCentralLinkHeld(BleCentralLink::Ring))
    return {info.queuedMs<10000?hw1setup::Ready::Wait:hw1setup::Ready::Drop,hw1setup::kYield};
  return {hw1setup::Ready::Run,hw1setup::kNone};
}
hw1setup::End setupRun(uint32_t t,const hw1setup::RunInfo&){TurnEnv env;return hw1setup::runPhoneTurn(turn,t,env);}
void setupFinish(uint32_t,const hw1setup::FinishInfo&){}
}

extern const BleSetupOps phonePipeSetupOps={setupLabel,setupReady,nullptr,setupRun,setupFinish,false};

bool phoneLinkOpen() {
  Role role(BleRoleTransition::PHONE_START);if(!role)return false;
  if(active.load())return true;
  if(!gSettings.bleEnabled || bleStackLifecycleFaulted())return false;
  const char* why=nullptr;if(radioCompanionBlocksRadio(&why))return false;
  // The pipe is a device on the shared client stack: bring the stack up when
  // it is down (it stays up until `closeble`); the pipe never owns a host.
  if(!bleStackUp()){status=bleStackLastRefusal();return false;}
  if(!resources() || !bleSetupWorkerStart())return false;
  switch(loadPsk()) {
    case PskLoad::Ready:break;
    case PskLoad::NoSecret:status="configure phone secret first";return false;
    case PskLoad::Retry:return false;  // as the refusals above: no status of its own
  }
  if(!bleStackReady() || !bluetoothHalStatus().ready())return false;
  // The console's row lives in gBLEState: hold it once the host is up and
  // before the owner lock (the lifecycle lock is never taken under it). Without
  // it the link and relay still start; the console's binds fail and its driver
  // backs off.
  if(blePipeConsoleStateAcquire())stateHeld=true;
  else ESP_LOGW("PIPE","no BLE session state; the phone link starts without its console");
  bool started=false;
  {Lock lock;if(lock && step!=Step::Fault && phoneGatt.idle()) {
    BLEDevice::setGapInterceptor(gapCallback);BLEDevice::setGattcInterceptor(gattCallback);
    bleCentralTxInit();(void)bleRegisterPeer(spec);
    xQueueReset(gapQueue);stopWanted=false;active=true;clearReady();
    // The task, looked for after active is published: one on its idle exit
    // reads active under initMux in the step that nulls its handle
    // (leaveIfReleased), so it is either still here or already gone and made
    // again now; resources() above may have seen one about to go.
    if(!workerHandle() && !resources()) {
      active=false;
    } else {
      bleCentralPhoneEnabled(!versionLatched.load());  // a held pipe leaves the ring its link
      setStep(Step::Idle,"disconnected",millis());routeSet();started=true;
    }
  }}
  if(!started)giveStateBack();
  return started;
}
bool phonePipeStop() {
  if(active.load()) {
    stopWanted=true;
    const uint32_t start=millis();
    while(active.load() && !hw1pipe::expired(millis(),start,4500))vTaskDelay(pdMS_TO_TICKS(10));
    if(active.load())return false;
  }
  // Stopped: the console row and the hold on gBLEState go with it (initBluetooth
  // takes the state right after).
  giveStateBack();
  return true;
}
bool phoneLinkClose(bool userInitiated) {
  Role role(BleRoleTransition::PHONE_STOP);if(!role)return false;
  if(!active.load())return true;
  // A user's close stamps the peer's intent (auto-reconnect stays quiet until
  // `openphone`); a stack close (`closeble`, a recovery) keeps it so `openble`
  // brings the phone back. The pipe stops; the stack stays up. The phone_pipe
  // task leaves on its own kPipeIdleRetireMs after its step machine is idle
  // again (run), or sooner when bleStackDown() releases it.
  if(userInitiated)disconnect();
  return phonePipeStop();
}
bool phonePipeHostStopped() {
  if(!bluetoothHalStatus().stopped())return false;
  // No task: nothing to reset. One that left on its idle exit left the adapter
  // idle with its app id released (idleOnHost); one released left after its
  // reset pass.
  if(!worker)return true;
  hostResetWanted=true;
  const uint32_t start=millis();
  while(hostResetWanted.load() && !hw1pipe::expired(millis(),start,1000))vTaskDelay(pdMS_TO_TICKS(10));
  if(hostResetWanted.load())return false;
  giveStateBack();  // the reset left the pipe stopped
  bleCentralRecoveryBlock(false);
  return true;
}
bool phonePipeWorkerRelease(uint32_t timeoutMs) {
  if(!workerHandle())return true;
  // The reset runs on the task before it may leave: asked of phonePipeHostStopped
  // with the task still published, so it never answers for a missing task over
  // stale GATT state. A pipe still active is the caller's stop to make first.
  if(active.load() || !phonePipeHostStopped())return false;
  // No phone setup turn may be claimed, queued, running or finishing: its
  // grant and release raise and lower this task's priority by handle.
  if(!bleSetupIdle(hw1setup::clientMask(hw1setup::Client::Phone)))return false;
  portENTER_CRITICAL(&initMux);exitWanted=true;portEXIT_CRITICAL(&initMux);
  const uint32_t start=millis();
  while(workerHandle() && !hw1pipe::expired(millis(),start,timeoutMs))vTaskDelay(pdMS_TO_TICKS(10));
  // Withdrawn and read in one step: a task that did not leave in time keeps
  // running with no request pending, so no late pass takes it down under a
  // caller that already gave up (fail closed).
  portENTER_CRITICAL(&initMux);exitWanted=false;const bool gone=!worker;portEXIT_CRITICAL(&initMux);
  return gone;
}
bool phonePipeInvalidateSecret() {
  if(!mutex)return true; // no resources means no cached key
  Lock lock(1000);if(!lock || active.load())return false;
  // A failure charged against the replaced key says nothing about the new one.
  // A start deriving meanwhile throws its key away (loadPsk).
  sodium_memzero(cachedPsk,sizeof(cachedPsk));pskValid=false;haveAuthFailure=false;++pskInvalidations;return true;
}
bool phonePipeRecoveryNeeded(){return recoveryNeeded.load();}
bool phonePipeQuiesceForHostRecovery() {
  {Lock lock;if(!lock || !recoveryNeeded.load())return false;quiesceWanted=true;}
  const uint32_t start=millis();
  while(!quiesced.load() && !hw1pipe::expired(millis(),start,1000))vTaskDelay(pdMS_TO_TICKS(10));
  return quiesced.load();
}
bool phonePipeActive(){return active.load();}
// Publishes the peer spec so the scheduler can admit a closed pipe (admit() opens it).
void phonePipeRegisterPeer(){(void)bleRegisterPeer(spec);}
bool phonePipeReady(){return ready.load();}
bool phonePipeRelayAvailable(){return ready.load() && relayOn.load();}
bool phonePipeOwnerHeldByCurrentTask(){return mutex && xSemaphoreGetMutexHolder(mutex)==xTaskGetCurrentTaskHandle();}
PhonePipePush phonePipeConsolePush(uint32_t generation,uint16_t streamId,const uint8_t* head,size_t headLen,
                                   const uint8_t* payload,size_t n,size_t reserve,uint32_t lockWaitMs) {
  if(!mutex)return PhonePipePush::Gone;  // never started: no session
  // The owner's own task would wait on itself: it is the only drainer.
  if(phonePipeOwnerHeldByCurrentTask())return PhonePipePush::Busy;
  Lock lock(lockWaitMs);if(!lock)return PhonePipePush::Busy;
  if(!ready.load() || !generation || generation!=phoneGatt.generation())return PhonePipePush::Gone;
  const auto* stream=phoneGatt.pipe.find(streamId);
  if(!stream || !stream->console)return PhonePipePush::Gone;  // console records go to slot 2 only
  switch(phoneGatt.pipe.pushRecord(streamId,head,headLen,payload,n,reserve)) {
    case hw1pipe::PushResult::Ok:return PhonePipePush::Ok;
    case hw1pipe::PushResult::NoSpace:return PhonePipePush::NoSpace;
    case hw1pipe::PushResult::Gone:break;
  }
  return PhonePipePush::Gone;
}
bool phonePipeLinkSummary(PhonePipeLinkSummary* out) {
  if(!mutex){if(out)*out={};return false;}  // never started: nothing to report
  portENTER_CRITICAL(&summaryMux);const PhonePipeLinkSummary s=linkSummary;portEXIT_CRITICAL(&summaryMux);
  if(out)*out=s;
  return true;
}
uint32_t phonePipeGeneration(){return liveGeneration.load();}
bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  portENTER_CRITICAL(&linkMux);const bool valid=linkValid;const PhonePipeLinkRecord record=linkRecord;portEXIT_CRITICAL(&linkMux);
  if(out)*out=valid?record:PhonePipeLinkRecord{};
  return valid;
}
void phonePipeNoteConsoleActivity(){consoleActivity.fetch_add(1);}
const char* phonePipeStatus(){return status.load();}
uint8_t phonePipeStep(){return uint8_t(step.load());}
bool phonePipeEnrolling(){return enrolling.load() || pairState.load()==PhonePairWindow::Open;}
PhonePairView phonePipePairView(char code[20]) {
  PhonePairView v;
  const uint32_t now=millis();
  portENTER_CRITICAL(&codeMux);
  v.state=pairState.load();
  const bool open=v.state==PhonePairWindow::Open,shown=open && pairOnDisplay;
  if(open) {
    // A window opened after this clock read (cmd_exec, while this task was
    // preempted) is new, never ~49 days old.
    const uint32_t age=now-pairOpenedAt;
    v.id=pairId;v.onDisplay=pairOnDisplay;v.ageMs=int32_t(age)<0?0:age;
    v.msLeft=v.ageMs<kPairWindowMs?kPairWindowMs-v.ageMs:0;
    v.source=pairSource;v.epoch=pairEpoch;
  }
  if(code){if(shown)std::memcpy(code,pairCode,sizeof(pairCode));else std::memset(code,0,sizeof(pairCode));}
  portEXIT_CRITICAL(&codeMux);
  return v;
}
void phonePipePairClose(uint32_t id) {
  if(!id)return;
  portENTER_CRITICAL(&codeMux);
  if(id==pairId && pairOnDisplay){hw1code::wipe(pairCode,sizeof(pairCode));pairOnDisplay=false;}
  portEXIT_CRITICAL(&codeMux);
  pairCloseWanted=id;
}
void phonePipeTick(){if(active.load())g2RingYieldToPhonePipe();}

PhonePipeOpenResult phonePipeOpen(PhonePipeStream& out,const char* host,uint16_t port,uint32_t budgetMs,const std::atomic<bool>& cancelled,const std::atomic<bool>* interrupted) {
  using Status=PhonePipeOpenStatus;
  const auto isCancelled=[&]{return cancelled.load() || (interrupted && interrupted->load());};
  out={};
  if(isCancelled())return {Status::Cancelled};
  // The phone's relay off: refused here, so no OPEN goes out to be refused (16).
  if(!ready.load() || !relayOn.load())return {Status::NotReady};
  // A-1: never start an OPEN the caller's deadline cannot give a full answer window.
  if(budgetMs<kPhonePipeOpenResponseMs)return {Status::NoBudget};
  const size_t length=host?strnlen(host,254):0;if(!port || !length || length>253)return {Status::Rejected};
  uint8_t address[16];uint8_t type=3;char name[254];
  if(inet_pton(AF_INET,host,address)==1)type=1;
  else if(inet_pton(AF_INET6,host,address)==1)type=2;
  else {for(size_t i=0;i<=length;++i){const char c=host[i];name[i]=(c>='A'&&c<='Z')?char(c+32):c;}}
  const uint32_t start=millis();
  {Lock lock;if(!lock)return {Status::Rejected};
    if(!ready.load() || !relayOn.load())return {Status::NotReady};
    out.generation=phoneGatt.generation();
    const uint8_t* nonce=phoneGatt.pipe.nonce();
    for(unsigned i=0;i<8;++i)out.corr=(out.corr<<8)|nonce[i];
    out.id=phoneGatt.pipe.open(type,type==3?reinterpret_cast<const uint8_t*>(name):address,type==3?length:(type==1?4:16),port,start);
    if(!out.id)return {ready.load()?Status::Rejected:Status::LinkLost};
  }
  // Two bounded phases: submission of every OPEN fragment, then the phone's
  // answer measured from local acceptance of the last fragment. The caller's
  // own deadline and cancellation always win.
  PhonePipeOpenResult result{Status::Cancelled};
  for(bool done=false;!done;) {
    const uint32_t now=millis();
    if(isCancelled() || hw1pipe::expired(now,start,budgetMs))break;
    {Lock lock;if(lock) {
      auto* stream=out.generation==phoneGatt.generation()?phoneGatt.pipe.find(out.id):nullptr;
      if(!stream || !ready.load()){result={Status::LinkLost};done=true;}
      else if(stream->state==hw1pipe::StreamState::Open){out.openedAt=stream->openedAt;return {Status::Opened};}
      else if(stream->state==hw1pipe::StreamState::Ended) {
        result=stream->remoteEnd?PhonePipeOpenResult{Status::Refused,uint16_t(stream->error)}:PhonePipeOpenResult{Status::LinkLost};
        done=true;
      } else if(!stream->openSubmitted) {
        if(hw1pipe::expired(now,start,kPhonePipeOpenSubmitMs)){result={Status::SubmitTimeout};done=true;}
      } else if(hw1pipe::expired(now,stream->openSubmittedAt,kPhonePipeOpenResponseMs)){result={Status::ResponseTimeout};done=true;}
    }}
    if(!done)vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Keep the handle for the caller: a failed OPEN may still await CLOSE_ACK.
  phonePipeClose(out,true);return result;
}
int phonePipeRead(PhonePipeStream handle,uint8_t* bytes,size_t size) {
  if(size && !bytes)return -1;
  Lock lock(0);if(!lock)return 0;
  if(handle.generation!=phoneGatt.generation() || !ready.load())return -1;
  auto* stream=phoneGatt.pipe.find(handle.id);if(!stream || stream->error!=Error::None || stream->state!=hw1pipe::StreamState::Open)return -1;
  const size_t n=phoneGatt.pipe.read(handle.id,bytes,size);return n?int(n):(stream->eof?-2:0);
}
int phonePipeWrite(PhonePipeStream handle,const uint8_t* bytes,size_t size) {
  if(size && !bytes)return -1;
  Lock lock(0);if(!lock)return 0;
  if(handle.generation!=phoneGatt.generation() || !ready.load())return -1;
  auto* stream=phoneGatt.pipe.find(handle.id);if(!stream || stream->state!=hw1pipe::StreamState::Open)return -1;
  return int(phoneGatt.pipe.write(handle.id,bytes,size));
}
void phonePipeClose(PhonePipeStream handle,bool cancelled) {
  if(!handle.id || handle.generation!=liveGeneration.load())return;
  Lock lock;
  if(!lock){if(handle.generation==liveGeneration.load())abortLink=true;return;}
  if(handle.generation==phoneGatt.generation())phoneGatt.pipe.close(handle.id,millis(),cancelled);
}
void phonePipeRelease(PhonePipeStream handle) {
  if(!handle.id || handle.generation!=liveGeneration.load())return;
  Lock lock;
  if(!lock){if(handle.generation==liveGeneration.load())abortLink=true;return;}
  if(handle.generation!=phoneGatt.generation())return;
  auto* stream=phoneGatt.pipe.find(handle.id);if(!stream)return;
  if(!stream->live()){phoneGatt.pipe.release(handle.id);return;}
  phoneGatt.pipe.close(handle.id,millis(),true);
  for(auto& id:releaseIds)if(!id || id==handle.id){id=handle.id;return;}
  abortLink=true;
}
bool phonePipeRetired(PhonePipeStream handle) {
  if(!handle.id)return true;
  Lock lock(0);if(!lock)return false;
  return handle.generation!=phoneGatt.generation() || !phoneGatt.pipe.find(handle.id);
}
// `phone scan [default|legacy|<interval_ms> <window_ms> dup|nodup]`: show the
// reconnect scan profile and the last eight scans, or change the profile.
static const char* scanCommand(const char* args,bool local) {
  // The profile, then the phone's scan and attempt counters with up to the
  // newest six of each, as whole lines within the executor's limit.
  EXT_RAM_BSS_ATTR static char text[CMD_RESULT_MAX];
  while(*args==' ')++args;
  hw1scan::Profile wanted;
  const bool change=*args!=0;
  if(change) {
    if(!local)return "Error: changing the phone scan requires an authenticated local admin";
    if(const char* why=hw1scan::parse(args,wanted)){snprintf(text,sizeof(text),"Error: phone scan: %s",why);return text;}
  }
  Lock lock;if(!lock)return "Error: phone pipe busy";
  if(change)scanProfile=wanted;
  size_t used=size_t(snprintf(text,sizeof(text),"Phone pipe scan: "));
  used+=size_t(hw1scan::format(text+used,sizeof(text)-used,scanProfile));
  used+=size_t(snprintf(text+used,sizeof(text)-used," (%s; %s)\n",
      hw1scan::same(scanProfile,hw1scan::kDefault)?"default":hw1scan::same(scanProfile,hw1scan::kLegacy)?"legacy":"custom",
      change?"from the next scan until reboot":"runtime only; default at boot"));
  const uint32_t now=millis();
  used+=bleCoexScanCounters(text+used,sizeof(text)-used,hw1coex::Peer::Phone);
  used+=bleCoexScanLines(text+used,(sizeof(text)-used)/2,now,hw1coex::Peer::Phone,6);  // half for the attempts
  if(used+1<sizeof(text)) {
    text[used++]='\n';text[used]=0;
    used+=bleCoexConnectCounters(text+used,sizeof(text)-used,hw1coex::Peer::Phone);
    bleCoexConnectLines(text+used,sizeof(text)-used,now,hw1coex::Peer::Phone,6);
  }
  return text;
}
// =============================================================================
// `phone`: the Android phone as a device. `relay`: what its pipe carries.
// =============================================================================
// HardwareOne.cpp: the one-shot serial sink for `phone pair new`'s code.
extern bool serialCommandSecretSinkReady();
extern bool serialWriteCommandSecret(const char* text,size_t len);
// The registry rows set the tier; pairing and forgetting also need the admin
// on serial or the OLED, checked here, because they bind the phone to a user.
static bool localAdmin(const AuthContext& auth) {
  return (auth.transport==SOURCE_SERIAL || auth.transport==SOURCE_LOCAL_DISPLAY) && currentExecIsAdmin();
}
// Pair and forget open a closed link first, on the shared client stack. They
// never take the host from the app console server: `closeconsole`, then
// `openphone`.
static const char* startLinkIfStopped() {
  if(active.load())return nullptr;
  if(isBleServerInitialized())return "Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'";
  if(phoneLinkOpen())return nullptr;
  EXT_RAM_BSS_ATTR static char text[96];
  snprintf(text,sizeof(text),"Error: phone link could not open (%s)",status.load());
  return text;
}
// A user's `openphone` or `phone pair` (and `phone pair new`) lifts the
// version-mismatch hold and clears the strikes; the ring yields to a running
// pipe again. True when a hold was lifted. Command tasks, owner mutex not held.
static bool liftVersionHold() {
  bool lifted=false;
  {Lock lock;versionStrikes=0;lifted=versionLatched.exchange(false);}  // serialized with a strike
  if(lifted && active.load() && !stopWanted.load())bleCentralPhoneEnabled(true);
  return lifted;
}
static const char* phoneStatusRow(const String& args){RETURN_VALID_IF_VALIDATE_CSTR();return phoneStatusCommand(args);}
// openphone: open the phone link on the shared client stack (bringing the
// stack up when it is off) and connect to the paired phone now.
static const char* cmd_openphone(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if(isBleServerInitialized())return "Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'";
  (void)liftVersionHold();  // before the open, which reads it
  if(!phoneLinkOpen()) {
    EXT_RAM_BSS_ATTR static char text[96];
    snprintf(text,sizeof(text),"Error: phone link could not open (%s)",status.load());
    return text;
  }
  if(ready.load())return "Phone: open and connected";
  BlePeerConnectRequest wanted;
  if(!blePeerBeginManualConnectRequest(BLE_PEER_PHONE,wanted))
    return "Phone: open on the client stack (no enrolled phone to connect; 'phone pair new' enrols one)";
  return admit(wanted)==BlePeerConnectAdmission::STARTED
      ?"Phone: open; connecting to the paired phone"
      :"Phone: open; connect pending (busy, the scheduler retries)";
}
// closephone: stop the pipe; the glasses, the ring and the stack stay up.
static const char* cmd_closephone(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Closed already (a stack close or `openconsole` did it without the user's
  // stamp): the user's close still means "stay down until openphone".
  if(!active.load()){blePeerNoteUserDisconnect(BLE_PEER_PHONE);return "Phone: already closed (auto-reconnect stays quiet until openphone)";}
  if(!phoneLinkClose(/*userInitiated=*/true))return "Error: phone link did not close (role transition busy or pipe cleanup incomplete); retry closephone";
  return "Phone: closed (link dropped, pipe stopped; glasses, ring and Bluetooth stay up)";
}
static const char* phoneScanCommand(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return scanCommand(args.c_str(),localAdmin(currentAuthContext()));
}
static const char* phoneForgetCommand(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const auto& auth=currentAuthContext();
  if(!localAdmin(auth))return "Error: phone forget requires an authenticated local admin";
  BlePeerOwnerSession owner;
  if(!blePeerOwnerSessionSnapshot(BLE_PEER_PHONE,owner) || owner.user!=auth.user)
    return "Error: phone is owned by another account";
  const auto epoch=captureTransportSessionEpoch(auth);
  if(!epoch)return "Error: local authenticated session required";
  if(const char* refused=startLinkIfStopped())return refused;
  Lock lock;if(!lock || forgetWanted || step==Step::Fault)return "Error: phone forget busy";
  // Forgetting also ends `phone pair new`'s window, with its attempt.
  const bool closed=closePairWindowLocked(millis());
  if(!blePeerBeginPhonePipeForget(owner.generation,forgetRequest))
    return closed?"Pairing window closed; no phone was paired":"Error: no current phone pipe enrollment";
  forgetEpoch=epoch;forgetSource=auth.transport;forgetWanted=true;
  if(job)fail(millis(),Error::Cancelled,"draining phone for forget",hw1coex::CauseDetail::Stopped);
  return "Phone forget accepted; check phone status. Remove the pipe allowlist on Android separately.";
}
static const char* phonePairCommand(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // `phone pair new` is its own row; anything else after `pair` is a mistake.
  if(args.length())return "Error: invalid arguments — Usage: phone pair [new]";
  const auto& auth=currentAuthContext();
  if(!localAdmin(auth))return "Error: phone pairing requires an authenticated local admin";
  bleStampPairedByIfBlank(BLE_PEER_PHONE);
  BlePeerOwnerSession owner;
  if(!blePeerOwnerSessionSnapshot(BLE_PEER_PHONE,owner) || owner.user!=auth.user)return "Error: phone is owned by another account";
  const auto epoch=captureTransportSessionEpoch(auth);
  if(!epoch)return "Error: local authenticated session required";
  (void)liftVersionHold();
  if(const char* refused=startLinkIfStopped())return refused;
  Lock lock;if(!lock || job || forgetWanted || step!=Step::Idle)return "Error: phone pipe busy; disconnect first";
  const uint32_t now=millis();
  if(authCoolingDown(now))return "Error: pipe authentication cooldown (30 seconds)";
  if(const char* refused=startEnrollmentLocked(now,epoch,auth.transport))return refused;
  return "Pairing opened for 60 seconds. Android must advertise the pipe with the same dedicated passphrase.";
}
// `phone pair new` (phone link v2): a device-made pairing code replaces the
// phone secret (the `phone secret` steps, marked device-made, which offers the
// app console) and a 3 min pairing window opens. The code is shown once: one
// direct serial write, or the OLED code screen. It is never in the result, a
// broadcast, the debug queue, a log or a history; it lives on this stack and,
// for the OLED, in the window's internal-RAM copy until the window ends.
// Every refusal comes before the secret changes.
static const char* phonePairNewCommand(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if(args.length())return "Error: invalid arguments — Usage: phone pair new";
  const auto& auth=currentAuthContext();
  if(!localAdmin(auth))return "Error: phone pair new requires an authenticated local admin on serial or the OLED";
  // The code's one sink: this serial console with its output on, or this display.
  const bool display=auth.transport==SOURCE_LOCAL_DISPLAY;
  if(display && !oledPhonePairCodeSinkReady())return "Error: the OLED cannot show a pairing code now";
  if(!display && !serialCommandSecretSinkReady())
    return "Error: phone pair new prints its code once on this console; turn serial output on";
  if(isBleServerInitialized())return "Error: the app console server owns Bluetooth; run 'closeconsole', then 'openphone'";
  if(!active.load() && !gSettings.bleEnabled)return "Error: Bluetooth is disabled; run bleenabled on first";
  bleStampPairedByIfBlank(BLE_PEER_PHONE);
  BlePeerOwnerSession owner;
  if(!blePeerOwnerSessionSnapshot(BLE_PEER_PHONE,owner) || owner.user!=auth.user)return "Error: phone is owned by another account";
  const auto epoch=captureTransportSessionEpoch(auth);
  if(!epoch)return "Error: local authenticated session required";
  {Lock lock;if(!lock)return "Error: phone pipe busy";
    if(authCoolingDown(millis()))return "Error: pipe authentication cooldown (30 seconds)";}
  (void)liftVersionHold();  // no reseek: the window starts its own attempt
  // 75 random bits and the check symbol, on this stack only (internal RAM).
  uint32_t words[3]={esp_random(),esp_random(),esp_random()};
  char code[hw1code::kSymbols+1],shown[hw1code::kDisplayLen+1];
  hw1code::fromRandom(words,code);hw1code::display(code,shown);hw1code::wipe(words,sizeof(words));
  struct Wipe {char* code;char* shown;
    ~Wipe(){hw1code::wipe(code,hw1code::kSymbols+1);hw1code::wipe(shown,hw1code::kDisplayLen+1);}} wipe{code,shown};
  bool replaced=false;
  if(const char* why=phonePipeReplaceSecret(code,hw1code::kSymbols,true,&replaced))
    return replaced?"Error: the new code replaced the phone secret, then a cleanup step failed; run phone pair new again":why;
  if(const char* refused=startLinkIfStopped())return refused;  // the code was never shown
  {Lock lock(1000);if(!lock)return "Error: phone pipe busy; run phone pair new again";
    // The sink again, under the lock that opens the window: the session may
    // have ended (a logout, another login) during the stop and the key.
    // A session that is gone gets no window and no code.
    if(!(display?oledPhonePairCodeSinkReady():serialCommandSecretSinkReady()))
      return "Error: the session ended; run phone pair new again";
    (void)openPairWindow(millis(),epoch,auth.transport,display?shown:nullptr);}
  if(display) {
    if(oledShowPhonePairCode())return "Pairing code on the OLED: pairing open 3 min (B closes it). In the app: Phone link > Pair, then type the code.";
  } else {
    char line[64];
    const int n=snprintf(line,sizeof(line),"\nPairing code (shown once): %s\n",shown);
    const bool printed=n>0 && size_t(n)<sizeof(line) && serialWriteCommandSecret(line,size_t(n));
    hw1code::wipe(line,sizeof(line));
    if(printed)return "Pairing open 3 min with the code above (shown once, never logged). In the app: Phone link > Pair, then type it.";
  }
  {Lock lock(1000);if(lock)(void)closePairWindowLocked(millis());}
  return "Error: the pairing code could not be shown; run phone pair new again";
}
// `phone stats`: the link's sessions, end reasons, scan/connect counters and
// last setup timings. What the link carried is `relay status`.
static const char* phoneStatsCommand(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char text[1536];
  // The console driver's counters before the owner mutex (its chain lock is outer to it).
  PipeConsoleStatus console;pipeConsoleStatus(&console);
  Lock lock;if(!lock)return "Error: phone pipe busy";
  const auto& st=sessionStats;
  // Connection interval unit is 1.25 ms; 0 until the controller reports one.
  size_t used=size_t(snprintf(text,sizeof(text),"Phone pipe: %s interval_ms=%u.%02u params=%s\n"
      "sessions ready=%lu auth_failures=%lu ended=%lu service_changed=%lu peer_reset=%lu "
      "version_mismatch=%lu strikes=%u%s\nend reasons:",
      status.load(),unsigned(params.interval*125u/100u),unsigned(params.interval*125u%100u),
      params.pending?"pending":!params.applied?(params.tries>=3?"refused":"unset"):params.active?"active":"idle",
      (unsigned long)st.ready,(unsigned long)st.authFailures,(unsigned long)st.ended,(unsigned long)st.serviceChanged,
      (unsigned long)st.peerReset,(unsigned long)st.versionMismatches,unsigned(versionStrikes.load()),
      versionLatched.load()?" held":""));
  for(unsigned i=0;i<sizeof(st.endReason)/sizeof(st.endReason[0]) && used<sizeof(text);++i)if(st.endReason[i])
    used+=size_t(snprintf(text+used,sizeof(text)-used," %u=%lu",i,(unsigned long)st.endReason[i]));
  // Console stream counters: counts only, never content. withheld_ms/max_ms:
  // ingress credit held back while a whole record waited on a gate (pool slot,
  // cmd_exec places, bound row), each gate counted once per record.
  if(used+1<sizeof(text))
    used+=size_t(snprintf(text+used,sizeof(text)-used,"\nconsole opens=%lu card_drop=%lu mirror_drop=%lu unknown=%lu malformed=%lu aborts=%lu"
        " withheld_ms=%lu max_ms=%lu pool_full=%lu exec_busy=%lu row_unbound=%lu",
        (unsigned long)console.opens,(unsigned long)console.cardDrops,(unsigned long)console.mirrorDrops,
        (unsigned long)console.unknown,(unsigned long)console.malformed,(unsigned long)console.aborts,
        (unsigned long)console.withheldMs,(unsigned long)console.withheldMaxMs,(unsigned long)console.poolFull,
        (unsigned long)console.executorBusy,(unsigned long)console.rowUnbound));
  if(used+1<sizeof(text)){text[used++]='\n';used+=bleCoexScanCounters(text+used,sizeof(text)-used,hw1coex::Peer::Phone);}
  if(used+1<sizeof(text)){text[used++]='\n';used+=bleCoexConnectCounters(text+used,sizeof(text)-used,hw1coex::Peer::Phone);}
  if(lastSetup.valid && used<sizeof(text)) {
    const auto& s=lastSetup;
    snprintf(text+used,sizeof(text)-used,
        "\nlast setup_ms scan=%lu connect=%lu discovery=%lu mtu=%lu search=%lu subscribe=%lu secure=%lu hello=%lu total=%lu",
        (unsigned long)s.scan,(unsigned long)s.connect,(unsigned long)s.discovery,(unsigned long)s.mtu,
        (unsigned long)s.search,(unsigned long)s.subscribe,(unsigned long)s.secure,(unsigned long)s.hello,
        (unsigned long)s.total);
  }
  return text;
}
// `relay status`: the pipe session's correlation id and how its users' HTTP
// requests (LLM HTTP source, Terminal) reached the network.
static const char* relayStatusCommand(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Worst case is about 2.5 KB (eight full records); under CMD_RESULT_MAX.
  EXT_RAM_BSS_ATTR static char text[3072];
  size_t used=0;
  {Lock lock;if(!lock)return "Error: phone pipe busy";
    char corr[17]="-";
    if(ready.load()){const uint8_t* n=phoneGatt.pipe.nonce();for(unsigned i=0;i<8;++i)snprintf(corr+2*i,3,"%02x",n[i]);}
    // relay: the phone's RELAY_ON (unknown without a session).
    used=size_t(snprintf(text,sizeof(text),"Relay: phone %s relay=%s corr=%s\n",ready.load()?"up":"down",
        !ready.load()?"unknown":relayOn.load()?"on":"off",corr));
  }
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
  if(used<sizeof(text))llmHttpPathReport(text+used,sizeof(text)-used);
#else
  if(used<sizeof(text))snprintf(text+used,sizeof(text)-used,"No relay users in this build (LLM HTTP source off)");
#endif
  return text;
}
static const char* phoneUsage(const String&) {
  return "Error: invalid arguments — Usage: phone <status|start|stop|connect|disconnect|pair [new]|forget|scan|stats|secret>";
}
static const char* relayUsage(const String&){return "Error: invalid arguments — Usage: relay status";}

// Longest prefix picks a verb row over the root, so each verb keeps its own
// tier. The roots only answer with usage and never dispatch a verb.
extern const CommandEntry phoneCommands[] = {
  { "phone status",     "Show the phone link. (add 'json' for JSON output)", false, phoneStatusRow },
  { "openphone",        "Open the phone link on the Bluetooth client stack and connect to the paired phone.", true, cmd_openphone },
  { "closephone",       "Close the phone link (glasses, ring and Bluetooth stay up).", true, cmd_closephone },
  { "phone pair",       "Open a 60 s pairing window (serial or OLED admin).", true, phonePairCommand },
  { "phone pair new",   "Make a new pairing code (the phone secret), show it once on serial or the OLED and pair for 3 min.", true,
    phonePairNewCommand, "Usage: phone pair new", /*requiresSuperAdmin=*/true },
  { "phone forget",     "Forget the paired phone (serial or OLED admin).",    true, phoneForgetCommand },
  { "phone scan",       "Show or set the phone reconnect scan.",     true, phoneScanCommand,
    "Usage: phone scan [default|legacy|<interval_ms> <window_ms> dup|nodup]" },
  { "phone stats",      "Phone link sessions, counters and setup timings.", true, phoneStatsCommand },
  { "phone secret",     "Set/clear the phone pairing passphrase.",   true, phoneSecretCommand,
    "Usage: phone secret <passphrase|clear>", /*requiresSuperAdmin=*/true },
  { "phone",            "Android phone link.",                       false, phoneUsage },
};
extern const size_t phoneCommandsCount=sizeof(phoneCommands)/sizeof(phoneCommands[0]);
extern const CommandEntry relayCommands[] = {
  { "relay status", "How relay users' requests reached the network (Wi-Fi or phone).", true, relayStatusCommand },
  { "relay",        "What the phone link carries.", false, relayUsage },
};
extern const size_t relayCommandsCount=sizeof(relayCommands)/sizeof(relayCommands[0]);
#endif
