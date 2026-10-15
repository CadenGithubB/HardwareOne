#include "BLE_PhonePipeCore.h"
#include "BLE_PhoneIdentityCore.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using Error=hw1pipe::Error;
using TransportSessionEpoch=uint32_t;
enum CommandSource{SOURCE_SERIAL};
constexpr size_t kPublicUsernameMaxLen=64;
constexpr int BLE_PEER_PHONE=2,ESP_OK=0,pdTRUE=1;
constexpr int ESP_LE_KEY_PENC=1,ESP_LE_KEY_PID=2,CONFIG_BT_SMP_MAX_BONDS=3;
constexpr int ESP_LE_AUTH_REQ_SC_MITM_BOND=13,ESP_IO_CAP_IO=1;
constexpr int ESP_BLE_ENC_KEY_MASK=1,ESP_BLE_ID_KEY_MASK=2,ESP_BLE_SEC_ENCRYPT_MITM=1;
constexpr int BLE_SCAN_TYPE_PASSIVE=0,BLE_ADDR_TYPE_PUBLIC=0,BLE_SCAN_FILTER_ALLOW_ALL=0,BLE_SCAN_DUPLICATE_ENABLE=1;
using StaticSemaphore_t=int;using SemaphoreHandle_t=void*;using TaskHandle_t=void*;
using portMUX_TYPE=int;constexpr int portMUX_INITIALIZER_UNLOCKED=0;
using StaticQueue_t=int;using QueueHandle_t=void*;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
struct BlePeerSavedTarget{char mac1[18]{};bool addressType1Known=true;uint8_t addressType1=1;};
struct BlePeerConnectRequest{BlePeerSavedTarget savedTarget;uint32_t intentGeneration=7,identityGeneration=9;};
struct BlePhoneForget{uint32_t intent=7,identity=9;BlePeerSavedTarget target;};
struct esp_ble_bond_dev_t {
 uint8_t bd_addr[6]{};uint8_t bd_addr_type=1;
 struct {
  uint8_t key_mask=3;
  struct{uint8_t key_size=16,sec_level=4;}penc_key;
  struct{uint8_t irk[16]{1};uint8_t static_addr[6]{0xc2,0,0,0,0,1};uint8_t addr_type=1;}pid_key;
 } bond_key;
};
struct esp_ble_scan_params_t{int scan_type,own_addr_type,scan_filter_policy,scan_interval,scan_window,scan_duplicate;};
struct esp_ble_conn_update_params_t{uint8_t bda[6];int min_int,max_int,latency,timeout;};
using esp_err_t=int;
static uint32_t clockMs=100,authorityIntent=7,authorityIdentity=9;
static bool authority=true,epochLive=true,accessory=false,gateAvailable=false,policyFailure=false;
static unsigned giveCount=0,restoreCount=0,encryptCount=0,authenticatedCount=0,persistCount=0,acceptCount=0;
static std::vector<std::string> events;
static bool lastSecurityReply=false;
static esp_ble_bond_dev_t bond;
static bool forgetCurrent=true,forgetCommit=true;
static int bondCount=1,bondListResult=ESP_OK,removeResult=ESP_OK;
static unsigned removeCount=0,forgetCommitCount=0;
static uint8_t removedAddress[6]{};
static bool blePeerPhonePipeForgetIsCurrent(const BlePhoneForget&){return forgetCurrent;}
static bool blePeerCommitPhonePipeForget(const BlePhoneForget&){++forgetCommitCount;return forgetCommit;}
static int esp_ble_remove_bond_device(uint8_t* address){++removeCount;std::memcpy(removedAddress,address,6);return removeResult;}
static uint32_t millis(){return clockMs;}
static bool transportSessionEpochIsLive(CommandSource,uint32_t epoch){return epochLive && epoch==5;}
static bool blePeerIntentIsCurrent(int,uint32_t intent,uint32_t identity){return authority && intent==authorityIntent && identity==authorityIdentity;}
static bool blePeerNoteLinkUpIfCurrent(int,const BlePeerConnectRequest& r,uint32_t* outIntent,uint32_t* outIdentity){
 if(!blePeerIntentIsCurrent(0,r.intentGeneration,r.identityGeneration))return false;
 ++acceptCount;*outIntent=++authorityIntent;*outIdentity=authorityIdentity;return true;
}
static bool blePeerCommitPhonePipeIdentityIfCurrent(uint32_t& intent,uint32_t& identity,const uint8_t*,uint8_t,bool* persist){
 if(!blePeerIntentIsCurrent(0,intent,identity))return false;
 ++acceptCount;intent=++authorityIntent;identity=++authorityIdentity;*persist=true;return true;
}
static void blePeerNoteLinkLost(int){}
static bool requestSettingsPersist(){++persistCount;return true;}
static bool bleCentralAccessoryPending(){return accessory;}
static bool bleCentralAccessoryBusy(){return accessory;}
static bool bleCentralTxAccessoryPressure(){return accessory;}
static bool g2MicAfeFeedIsActive(){return false;}
static bool bleCentralSetupTake(bool,uint32_t){return gateAvailable;}
static void bleCentralSetupGive(){++giveCount;}
static int esp_ble_get_bond_device_num(){return bondCount;}
static int esp_ble_get_bond_device_list(int* count,esp_ble_bond_dev_t* out){*count=bondCount;out[0]=bond;return bondListResult;}
static int esp_ble_gap_set_scan_params(esp_ble_scan_params_t*){events.emplace_back("scan-params");return ESP_OK;}
static int esp_ble_gap_start_scanning(unsigned){events.emplace_back("scan");return ESP_OK;}
static int esp_ble_gap_stop_scanning(){events.emplace_back("stop-scan");return ESP_OK;}
static int esp_ble_gap_config_local_privacy(bool){return ESP_OK;}
static int esp_ble_gap_security_rsp(uint8_t*,bool allow){lastSecurityReply=allow;return ESP_OK;}
static int esp_ble_confirm_reply(uint8_t*,bool){return ESP_OK;}
static int esp_ble_set_encryption(uint8_t*,int){++encryptCount;return ESP_OK;}
static int esp_ble_gap_update_conn_params(esp_ble_conn_update_params_t*){return ESP_OK;}
struct BLESecurity {
 struct Policy{uint8_t authentication,capability,initiatorKeys,responderKeys,keySize;};
 inline static Policy current{0,0,0,0,16};
 static Policy policy(){return current;}
 static int applyPolicy(const Policy& p){events.emplace_back(p.authentication?"strict-policy":"restore-policy");
  if(!p.authentication)++restoreCount;
  if(policyFailure)return -1;
  current=p;return ESP_OK;
 }
};
struct BLEDevice {inline static uint16_t m_appId=0;};
namespace hw1phone {
struct Gatt {
 enum class Phase{Idle,Registering,Opening,Security,Mtu,Pipe,Retiring,Quarantined};
 hw1pipe::Session pipe;
 Phase state=Phase::Idle;uint32_t gen=1;
 bool begin(uint16_t,const uint8_t*,uint8_t,uint32_t){events.emplace_back("gatt-begin");state=Phase::Opening;++gen;return true;}
 bool idle()const{return state==Phase::Idle;}
 Phase phase()const{return state;}
 bool ready()const{return state==Phase::Pipe && pipe.state==hw1pipe::State::Ready;}
 void poll(uint32_t,bool){}
 void stop(uint32_t,Error e){pipe.down(e);state=Phase::Idle;}
 bool authenticated(uint32_t){assert(state==Phase::Security);++authenticatedCount;state=Phase::Mtu;return true;}
 void hostStopped(){state=Phase::Idle;pipe.down();}
 uint32_t generation()const{return gen;}
};
}
using hw1phone::Gatt;
// INSERT_TYPES
// INSERT_STORAGE
static int xQueueReceive(QueueHandle_t,GapEvent*,uint32_t){return 0;}
// INSERT_PRODUCTION
static void reset() {
 forgetWanted=forgetPending=false;forgetRequest={};forgetEpoch=5;forgetCurrent=forgetCommit=true;
 bondCount=1;bondListResult=removeResult=ESP_OK;removeCount=forgetCommitCount=0;
 phoneGatt.hostStopped();phoneGatt.pipe.streams[0].reset();phoneGatt.pipe.streams[1].reset();
 active=true;standalone=true;ready=false;stopWanted=false;gapOverflow=false;abortLink=false;hostResetWanted=false;
 step=Step::Idle;job=enrolling=setupHeld=policyChanged=privacyReady=nativeKnown=false;
 privacyPending=scanParamsPending=false;
 scanStarted=scanStartPending=scanStopPending=candidate=securityStarted=confirmed=numericPending=identitySeen=published=bondSaved=authDeferred=false;
 restoreAttempts=0;restoreAt=0;jobAt=0;at=0;securityAt=0;lastParams=0;lastStreamAt=0;
 releaseIds[0]=releaseIds[1]=0;request={};authorityIntent=intent=7;authorityIdentity=identityGeneration=9;
 authority=epochLive=true;accessory=gateAvailable=policyFailure=false;enrollmentEpoch=5;clockMs=100;
 giveCount=restoreCount=encryptCount=authenticatedCount=persistCount=acceptCount=0;events.clear();
 bond={};std::memcpy(identity,bond.bond_key.pid_key.static_addr,6);identityType=1;
 std::memcpy(target,identity,6);targetType=1;BLESecurity::current={0,0,0,0,16};route={};
}
static void pendingForget() {
 forgetWanted=true;gateAvailable=true;
 std::strcpy(forgetRequest.target.mac1,"C2:00:00:00:00:01");
 const uint8_t stackAddress[6]{0x42,0,0,0,0,9};std::memcpy(bond.bd_addr,stackAddress,6);
}
static void pendingEnrollment() {
 job=enrolling=setupHeld=candidate=identitySeen=confirmed=true;
 step=Step::Link;jobAt=0;phoneGatt.state=Gatt::Phase::Security;
 assert(configureSecurityPolicy());
}
int main() {
 // A restored Android bond may be reported as a native pseudo-address. We
 // never issue a duplicate controller resolving-list add, or trust that address
 // alone: absent/wrong/weak bonds fail and authenticated encryption still gates.
 for(int representation=0;representation<4;++representation) {
  reset();job=setupHeld=identitySeen=true;
  const uint8_t pseudo[6]{0x42,0,0,0,0,9};std::memcpy(bond.bd_addr,pseudo,6);
  prepareBond(clockMs);assert(step==Step::ScanParams && nativeKnown);
  assert(events.size()==1 && events[0]=="scan-params");
  GapEvent e;e.kind=GapKind::Candidate;e.type=representation<2?1:3;
  std::memcpy(e.address,representation%2?identity:pseudo,6);consume(e,clockMs);
  assert(candidate && !ready && authenticatedCount==0);
  routeSet();assert(matches(route,pseudo) || matches(route,identity));
 }
 for(int rejection=0;rejection<3;++rejection) {
  reset();job=setupHeld=identitySeen=true;
  const uint8_t pseudo[6]{0x42,0,0,0,0,9};std::memcpy(bond.bd_addr,pseudo,6);prepareBond(clockMs);
  GapEvent e;e.kind=GapKind::Candidate;e.type=1;std::memcpy(e.address,pseudo,6);
  if(rejection==0)e.address[5]^=1;
  if(rejection==1)e.type=0;
  if(rejection==2)nativeKnown=false;
  consume(e,clockMs);assert(!candidate && !ready);
 }
 for(int rejection=0;rejection<4;++rejection) {
  reset();job=setupHeld=identitySeen=true;
  if(rejection==0)bondCount=0;
  if(rejection==1)bond.bond_key.penc_key.key_size=15;
  if(rejection==2)bond.bond_key.pid_key.static_addr[5]^=1;
  if(rejection==3)bond.bd_addr_type=4;
  prepareBond(clockMs);assert(step==Step::Retiring && !nativeKnown && events.empty());
 }

 {reset();job=enrolling=setupHeld=candidate=true;step=Step::Scanning;
  GapEvent e;e.kind=GapKind::ScanDone;consume(e,clockMs);
  assert(events.size()>=2 && events[0]=="strict-policy" && events[1]=="gatt-begin");
  assert(policyChanged && phoneGatt.state==Gatt::Phase::Opening);
 }
 {reset();pendingEnrollment();phoneGatt.state=Gatt::Phase::Opening;
  GapEvent e;e.kind=GapKind::Auth;e.auth=13;consume(e,clockMs);
  assert(authDeferred && authenticatedCount==0);
  phoneGatt.state=Gatt::Phase::Security;tickLocked(clockMs);
  assert(!authDeferred && authenticatedCount==1 && !policyChanged && restoreCount==1);
 }
 {reset();pendingEnrollment();GapEvent e;e.kind=GapKind::Auth;e.auth=13;consume(e,clockMs);
  phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  tickLocked(clockMs);assert(ready && !enrolling && !setupHeld && persistCount==1 && giveCount==1);
  epochLive=false;clockMs=70000;tickLocked(clockMs);
  assert(ready && acceptCount==1); // completed enrollment is not a perpetual local-session lease
 }
 // HELLO failure after authenticated pairing retains the known approved bond.
 // Discovery is not READY, and later connect/unpair use the committed identity.
 {reset();pendingEnrollment();GapEvent e;e.kind=GapKind::Auth;e.auth=13;consume(e,clockMs);
  assert(bondSaved && !enrolling && !ready && persistCount==1 && acceptCount==1);
  assert(currentJob());
  fail(clockMs,Error::Protocol,"HELLO rejected");tickLocked(clockMs);
  assert(!job && !ready && !setupHeld && persistCount==1 && acceptCount==1);
 }
 {reset();pendingEnrollment();authority=false;GapEvent e;e.kind=GapKind::Auth;e.auth=13;consume(e,clockMs);
  assert(!bondSaved && !persistCount && !acceptCount && !ready);
 }
 for(int weak=0;weak<4;++weak) {
  reset();pendingEnrollment();GapEvent e;e.kind=GapKind::Auth;e.auth=13;
  if(weak==0)confirmed=false;
  if(weak==1)e.auth=9; // SC without MITM
  if(weak==2)bond.bond_key.penc_key.key_size=15;
  if(weak==3)std::memset(bond.bond_key.pid_key.irk,0,16);
  consume(e,clockMs);assert(authenticatedCount==0 && !ready && step==Step::Retiring);
 }
 {reset();job=enrolling=setupHeld=true;step=Step::Retiring;privacyPending=true;
  tickLocked(clockMs);assert(setupHeld && job);routeSet();assert(route.privacy);
  GapEvent e;e.kind=GapKind::Privacy;consume(e,clockMs);tickLocked(clockMs);
  assert(!setupHeld && !job && giveCount==1);
 }
 {reset();pendingEnrollment();accessory=true;tickLocked(clockMs);
  assert(!ready && !job && !setupHeld && restoreCount==1 && giveCount==1);
 }
 {reset();pendingEnrollment();authority=false;phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  tickLocked(clockMs);assert(!ready && acceptCount==0);
 }
 {reset();job=enrolling=true;step=Step::Waiting;clockMs=60000;tickLocked(clockMs);
  assert(!job && !ready); // explicit enrollment deadline includes time waiting for radio
 }
 {reset();job=true;step=Step::Link;GapEvent e;e.kind=GapKind::SecurityRequest;
  consume(e,clockMs);assert(!lastSecurityReply);
  assert(configureSecurityPolicy());consume(e,clockMs);assert(lastSecurityReply && securityStarted);
 }
 {reset();pendingEnrollment();policyFailure=true;fail(clockMs,Error::Cancelled,"cancel");
  for(int i=0;i<20;++i){clockMs+=10;tickLocked(clockMs);}
  assert(restoreCount==1 && setupHeld);
  for(int i=0;i<6;++i){clockMs+=1000;tickLocked(clockMs);}
  assert(restoreCount==3 && setupHeld && step==Step::Fault);
  hostResetWanted=true;tickLocked(clockMs);assert(!hostResetWanted && !setupHeld && !active);
 }
 {reset();job=true;step=Step::Link;published=ready=true;phoneGatt.state=Gatt::Phase::Pipe;phoneGatt.pipe.state=hw1pipe::State::Ready;
  auto& stream=phoneGatt.pipe.streams[0];stream.id=2;stream.state=hw1pipe::StreamState::Ended;releaseIds[0]=2;
  tickLocked(clockMs);assert(releaseIds[0]==0 && stream.state==hw1pipe::StreamState::Free);
 }
 {reset();pendingForget();tickLocked(clockMs);
  assert(forgetPending && setupHeld && step==Step::Forget && removeCount==1);
  assert(!std::memcmp(removedAddress,bond.bd_addr,6)); // remove only native address behind enrolled PID
  assert(forgetCommitCount==0);routeSet();assert(route.forget);
  GapEvent e;e.kind=GapKind::BondRemoved;consume(e,clockMs);
  assert(!forgetPending && !forgetWanted && !setupHeld && forgetCommitCount==1 && persistCount==1);
 }
 {reset();pendingForget();bondCount=0;tickLocked(clockMs);
  assert(!forgetWanted && removeCount==0 && forgetCommitCount==1 && persistCount==1);
 }
 for(int fault=0;fault<4;++fault) {
  reset();pendingForget();
  if(fault==0)epochLive=false;
  if(fault==1)forgetCurrent=false;
  if(fault==2)bondListResult=-1;
  if(fault==3)removeResult=-1;
  tickLocked(clockMs);assert(!forgetWanted && !setupHeld && forgetCommitCount==0 && persistCount==0);
 }
 {reset();pendingForget();bond.bond_key.penc_key.key_size=7;tickLocked(clockMs);
  assert(forgetPending && removeCount==1); // damaged bond can be explicitly removed, never used
  stopWanted=true;clockMs+=4100;tickLocked(clockMs);
  assert(forgetPending && setupHeld && active && step==Step::Fault);
  GapEvent e;e.kind=GapKind::BondRemoved;consume(e,clockMs);tickLocked(clockMs);
  assert(!active && !setupHeld && forgetCommitCount==1);
 }
 {reset();pendingForget();tickLocked(clockMs);forgetCommit=false;
  GapEvent e;e.kind=GapKind::BondRemoved;consume(e,clockMs);
  assert(!forgetWanted && !setupHeld && persistCount==0); // replaced metadata must survive
 }
 {reset();job=false;identitySeen=true;routeSet();assert(!route.peer && !route.identity);}
 puts("Phone runtime: verified enrollment, asynchronous auth, setup retirement, accessory priority and deferred stream release passed");
}
