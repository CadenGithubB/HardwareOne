#include "BLE_PhonePipe.h"
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
#include "BLE_PhoneGatt.h"
#include "BLE_PhoneIdentityCore.h"
#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"
#include "BLE_Peers.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "HAL_Bluetooth.h"
#include "System_AuthIdentity.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_RadioCompanion.h"
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include <esp_random.h>
#include <freertos/semphr.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <cstring>
#include <cstdio>
#ifndef HARDWAREONE_BLE_PHONE_PIPE_API
#error "Apply and verify tools/ble-phone-pipe/arduino.patch before enabling the phone pipe"
#endif

namespace {
using hw1phone::Gatt;
using Error=hw1pipe::Error;
enum class Step : uint8_t { Idle, Waiting, Privacy, ScanParams, Scanning, Link, Retiring, Forget, Fault };
enum class GapKind : uint8_t { Privacy, ScanParams, ScanStart, ScanDone, ScanStopped,
  Candidate, SecurityRequest, Numeric, Auth, Identity, Reject, Params, BondRemoved };
struct GapEvent {
  GapKind kind{};
  int status=0;
  uint32_t number=0;
  uint8_t address[6]{},type=0,auth=0;
  uint16_t interval=0;
};
struct Route {
  bool enabled=false, scan=false, privacy=false;
  bool peer=false, identity=false, forget=false;
  uint8_t forgetAddress[6]{};
  uint8_t address[6]{},identityAddress[6]{};
};
// The callback storage and owner outlive every registration, including failures.
Gatt phoneGatt;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex=nullptr;
portMUX_TYPE initMux=portMUX_INITIALIZER_UNLOCKED,routeMux=portMUX_INITIALIZER_UNLOCKED;
Route route;
StaticQueue_t gapQueueStorage;
alignas(4) uint8_t gapQueueBytes[8*sizeof(GapEvent)];
QueueHandle_t gapQueue=nullptr;
TaskHandle_t worker=nullptr;
std::atomic<bool> hostResetWanted{false},abortLink{false};
std::atomic<uint32_t> liveGeneration{0};
uint16_t releaseIds[2]{};
bool privacyPending=false,scanParamsPending=false;
std::atomic<bool> gapOverflow{false},active{false},standalone{false},ready{false},stopWanted{false};
std::atomic<const char*> status{"stopped"};
Step step=Step::Idle;
BlePeerConnectRequest request;
bool job=false,enrolling=false,setupHeld=false,policyChanged=false;
bool privacyReady=false,scanStarted=false,scanStartPending=false,scanStopPending=false;
bool candidate=false,securityStarted=false,confirmed=false,numericPending=false,identitySeen=false;
bool published=false,bondSaved=false,paramsActive=false;
uint8_t target[6]{},targetType=0,identity[6]{},identityType=0;
uint8_t nativeAddress[6]{},nativeType=0;bool nativeKnown=false;
uint32_t at=0,jobAt=0,securityAt=0,pin=0,lastParams=0,lastStreamAt=0;
uint32_t restoreAt=0;unsigned restoreAttempts=0;
uint32_t intent=0,identityGeneration=0;
TransportSessionEpoch enrollmentEpoch=0;
CommandSource enrollmentSource=SOURCE_SERIAL;
char enrollmentUser[kPublicUsernameMaxLen+1]{};
BLESecurity::Policy priorPolicy{};
GapEvent deferredAuth;bool authDeferred=false;
bool configureSecurityPolicy();
BlePhoneForget forgetRequest;
bool forgetWanted=false,forgetPending=false;
uint8_t forgetAddress[6]{};
TransportSessionEpoch forgetEpoch=0;
CommandSource forgetSource=SOURCE_SERIAL;

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
  route.privacy=privacyPending;
  route.forget=forgetPending;std::memcpy(route.forgetAddress,forgetAddress,6);
  route.peer=job && candidate;
  std::memcpy(route.address,target,6);
  route.identity=job && identitySeen;
  std::memcpy(route.identityAddress,identity,6);
  portEXIT_CRITICAL(&routeMux);
}
bool matches(const Route& r,const uint8_t* address) {
  return (r.peer && !std::memcmp(r.address,address,6)) ||
         (r.identity && !std::memcmp(r.identityAddress,address,6));
}
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
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      if(!r.forget || std::memcmp(r.forgetAddress,p->remove_bond_dev_cmpl.bd_addr,6))return false;
      e.kind=GapKind::BondRemoved;e.status=p->remove_bond_dev_cmpl.status;break;
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
      if(!r.privacy)return false;
      e.kind=GapKind::Privacy;e.status=p->local_privacy_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if(!r.scan)return false;
      e.kind=GapKind::ScanParams;e.status=p->scan_param_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if(!r.scan)return false;
      e.kind=GapKind::ScanStart;e.status=p->scan_start_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      if(!r.scan)return false;
      e.kind=GapKind::ScanStopped;e.status=p->scan_stop_cmpl.status;break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if(!r.scan)return false;
      if(p->scan_rst.search_evt==ESP_GAP_SEARCH_INQ_CMPL_EVT)e.kind=GapKind::ScanDone;
      else if(p->scan_rst.search_evt==ESP_GAP_SEARCH_INQ_RES_EVT &&
              p->scan_rst.adv_data_len<=31 && hasService(p->scan_rst.ble_adv,p->scan_rst.adv_data_len)) {
        e.kind=GapKind::Candidate;e.type=p->scan_rst.ble_addr_type;std::memcpy(e.address,p->scan_rst.bda,6);
      } else return true;
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      if(!matches(r,p->ble_security.ble_req.bd_addr))return false;
      e.kind=GapKind::SecurityRequest;break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      if(!matches(r,p->ble_security.key_notif.bd_addr))return false;
      e.kind=GapKind::Numeric;e.number=p->ble_security.key_notif.passkey;break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if(!matches(r,p->ble_security.auth_cmpl.bd_addr))return false;
      e.kind=GapKind::Auth;e.status=p->ble_security.auth_cmpl.success?0:1;
      e.auth=p->ble_security.auth_cmpl.auth_mode;break;
    case ESP_GAP_BLE_KEY_EVT:
      if(!matches(r,p->ble_security.ble_key.bd_addr))return false;
      if(p->ble_security.ble_key.key_type!=ESP_LE_KEY_PID)return true;
      e.kind=GapKind::Identity;e.type=p->ble_security.ble_key.p_key_value.pid_key.addr_type;
      std::memcpy(e.address,p->ble_security.ble_key.p_key_value.pid_key.static_addr,6);
      // AUTH can report the resolved identity before the worker consumes KEY.
      portENTER_CRITICAL(&routeMux);route.identity=true;std::memcpy(route.identityAddress,e.address,6);portEXIT_CRITICAL(&routeMux);
      break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: case ESP_GAP_BLE_OOB_REQ_EVT: case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      if(!matches(r,p->ble_security.ble_req.bd_addr))return false;
      e.kind=GapKind::Reject;break; // numeric comparison is the only enrollment association
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

struct Wipe {
  void* memory;size_t size;
  ~Wipe(){volatile uint8_t* p=static_cast<volatile uint8_t*>(memory);for(size_t i=0;i<size;++i)p[i]=0;}
};
// Returns -1 on an unreadable/ambiguous store, 0 when absent, 1 with its exact
// stack address. Unpair must also work for a damaged/weak bond, without trusting
// that bond for a connection. Matching uses only the enrolled canonical PID.
int bondAddress(const uint8_t expected[6],uint8_t type,uint8_t out[6]) {
  const int available=esp_ble_get_bond_device_num();
  if(available<0 || available>CONFIG_BT_SMP_MAX_BONDS)return -1;
  if(!available)return 0;
  esp_ble_bond_dev_t bonds[CONFIG_BT_SMP_MAX_BONDS]{};Wipe wipe{bonds,sizeof(bonds)};
  int count=available;
  if(esp_ble_get_bond_device_list(&count,bonds)!=ESP_OK || count<0 || count>available)return -1;
  int found=0;
  for(int i=0;i<count;++i) {
    const auto& b=bonds[i];
    if(!(b.bond_key.key_mask&ESP_LE_KEY_PID) || b.bond_key.pid_key.addr_type!=type ||
       std::memcmp(b.bond_key.pid_key.static_addr,expected,6))continue;
    if(found)return -1;
    std::memcpy(out,b.bd_addr,6);found=1;
  }
  return found;
}
bool bondFor(const uint8_t expected[6],uint8_t type,esp_ble_bond_dev_t& out,bool scVerified) {
  const int available=esp_ble_get_bond_device_num();
  if(available<=0 || available>CONFIG_BT_SMP_MAX_BONDS)return false;
  // Stack is deliberately sized for the configured, bounded bond capacity.
  esp_ble_bond_dev_t bonds[CONFIG_BT_SMP_MAX_BONDS]{};Wipe wipe{bonds,sizeof(bonds)};int count=available;
  if(esp_ble_get_bond_device_list(&count,bonds)!=ESP_OK || count<0 || count>available)return false;
  bool found=false;
  for(int i=0;i<count;++i) {
    const auto& b=bonds[i];const auto& keys=b.bond_key;
    if(keys.pid_key.addr_type!=type || std::memcmp(keys.pid_key.static_addr,expected,6))continue;
    if(found || !hw1phone::validIdentity(expected,type) ||
       !hw1phone::strongBond(scVerified,keys.key_mask&ESP_LE_KEY_PENC,keys.key_mask&ESP_LE_KEY_PID,
           keys.penc_key.key_size,(keys.penc_key.sec_level&4)!=0,keys.pid_key.irk)) {found=false;break;}
    out=b;found=true;
  }
  // No key material is persisted by this owner or printed in diagnostics.
  return found;
}
bool currentJob() {
  if(!job)return false;
  if(enrolling && (!enrollmentEpoch || !transportSessionEpochIsLive(enrollmentSource,enrollmentEpoch)))return false;
  return blePeerIntentIsCurrent(BLE_PEER_PHONE,intent,identityGeneration);
}
void fail(uint32_t now,Error why,const char* label) {
  ready=false;numericPending=false;authDeferred=false;phoneGatt.stop(now,why);
  if(step!=Step::Retiring && step!=Step::Fault)setStep(Step::Retiring,label,now);
}
bool restorePolicy() {
  if(!policyChanged)return true;
  const uint32_t now=millis();
  if(restoreAttempts>=3 || (restoreAttempts && !hw1pipe::expired(now,restoreAt,500)))return false;
  ++restoreAttempts;restoreAt=now;
  if(BLESecurity::applyPolicy(priorPolicy)!=ESP_OK) {status="security policy restore failed";return false;}
  policyChanged=false;return true;
}
void releaseSetup() {
  if(setupHeld){bleCentralSetupGive();setupHeld=false;}
}
void configureScan(uint32_t now) {
  scanParamsPending=true;setStep(Step::ScanParams,"scanning",now);routeSet();
  esp_ble_scan_params_t params{};params.scan_type=BLE_SCAN_TYPE_PASSIVE;
  params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;params.scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL;
  // 30 ms of every 100 ms for up to 2 s (inside the 3 s setup deadline): about
  // twenty of a phone's 100 ms advertising events instead of three, which
  // missed the phone about one attempt in four.
  params.scan_interval=160;params.scan_window=48;params.scan_duplicate=BLE_SCAN_DUPLICATE_ENABLE;
  if(esp_ble_gap_set_scan_params(&params)!=ESP_OK){scanParamsPending=false;fail(now,Error::Connect,"scan setup failed");}
}
void prepareBond(uint32_t now) {
  if(enrolling){configureScan(now);return;}
  esp_ble_bond_dev_t bond{};Wipe wipe{&bond,sizeof(bond)};
  if(!bondFor(identity,identityType,bond,true)){fail(now,Error::Unauthorized,"saved bond unavailable");return;}
  // The pinned Bluedroid restores PID/LID keys into its own resolving list.
  // A second raw HCI add can collide with that entry. Its scan callback may
  // expose the bond's saved pseudo-address instead of the canonical PID.
  // Use only the exact native address returned with this verified bond; this
  // is candidate selection, never proof of encryption or authentication.
  if(bond.bd_addr_type>3) {fail(now,Error::Unauthorized,"saved bond address invalid");return;}
  std::memcpy(nativeAddress,bond.bd_addr,6);nativeType=bond.bd_addr_type;nativeKnown=true;
  configureScan(now);
}
void finishForget(uint32_t now,bool removed) {
  forgetPending=forgetWanted=false;nativeKnown=false;
  const bool committed=removed && blePeerCommitPhonePipeForget(forgetRequest);
  if(committed)(void)requestSettingsPersist();
  releaseSetup();
  setStep(Step::Idle,committed?"phone bond forgotten":(removed?"bond removed; metadata changed":"phone bond removal failed"),now);
  routeSet();
}
void startForget(uint32_t now) {
  if(!transportSessionEpochIsLive(forgetSource,forgetEpoch) ||
     !blePeerPhonePipeForgetIsCurrent(forgetRequest)) {
    forgetWanted=false;status="phone unpair authority changed";return;
  }
  if(!bleCentralSetupTake(true,0))return;
  setupHeld=true;
  uint8_t address[6];
  if(!forgetRequest.target.addressType1Known ||
     !hw1phone::parseIdentity(forgetRequest.target.mac1,forgetRequest.target.addressType1,address)) {
    finishForget(now,false);return;
  }
  const int present=bondAddress(address,forgetRequest.target.addressType1,forgetAddress);
  if(present<=0){finishForget(now,present==0);return;}
  forgetPending=true;setStep(Step::Forget,"removing phone bond",now);routeSet();
  if(esp_ble_remove_bond_device(forgetAddress)!=ESP_OK)finishForget(now,false);
}
void consume(const GapEvent& e,uint32_t now) {
  const bool retiring=step==Step::Retiring || step==Step::Fault;
  switch(e.kind) {
    case GapKind::BondRemoved:
      if(forgetPending)finishForget(now,e.status==0);
      break;
    case GapKind::Privacy:
      privacyPending=false;
      if(e.status){fail(now,Error::Connect,"privacy setup failed");break;}
      privacyReady=true;
      if(!retiring)prepareBond(now);
      break;
    case GapKind::ScanParams:
      scanParamsPending=false;
      if(e.status){fail(now,Error::Connect,"scan setup failed");break;}
      if(retiring)break;
      scanStartPending=true;setStep(Step::Scanning,"scanning",now);routeSet();
      if(esp_ble_gap_start_scanning(2)!=ESP_OK){scanStartPending=false;fail(now,Error::Connect,"scan start failed");}
      break;
    case GapKind::ScanStart:
      scanStartPending=false;scanStarted=e.status==0;
      if(e.status)fail(now,Error::Connect,"scan start failed");
      break;
    case GapKind::Candidate:
      if(retiring || candidate || e.type>3)break;
      // Bluedroid may return canonical PID or the mapped native pseudo-address.
      // An unrelated raw RPA/UUID is not enough. READY still requires this
      // bond's authenticated SC encryption and protocol negotiation below.
      if(!enrolling) {
        const bool canonical=!std::memcmp(e.address,identity,6) && (e.type&1)==identityType;
        const bool mapped=nativeKnown && !std::memcmp(e.address,nativeAddress,6) &&
                          (e.type&1)==(nativeType&1);
        if(!canonical && !mapped)break;
      }
      std::memcpy(target,e.address,6);targetType=e.type;candidate=true;routeSet();break;
    case GapKind::ScanDone: case GapKind::ScanStopped:
      if(e.kind==GapKind::ScanStopped && e.status){fail(now,Error::Connect,"scan stop failed");break;}
      scanStarted=scanStartPending=false;
      if(e.kind==GapKind::ScanStopped)scanStopPending=false;
      if(retiring || scanStopPending)break;
      if(!candidate){fail(now,Error::Connect,"phone not found");break;}
      setStep(Step::Link,"connecting",now);routeSet();
      if(!configureSecurityPolicy()) {fail(now,Error::Unauthorized,"security policy setup failed");break;}
      if(BLEDevice::m_appId>=0x7ffe || !phoneGatt.begin(++BLEDevice::m_appId,target,targetType,now))
        fail(now,Error::Resources,"GATT registration unavailable");
      liveGeneration=phoneGatt.generation();releaseIds[0]=releaseIds[1]=0;
      break;
    case GapKind::SecurityRequest:
      if(!retiring && currentJob() && policyChanged) {securityStarted=true;securityAt=now;}
      if(esp_ble_gap_security_rsp(target,!retiring && currentJob() && policyChanged)!=ESP_OK)
        fail(now,Error::Unauthorized,"security request failed");
      break;
    case GapKind::Numeric:
      if(retiring || !enrolling || !currentJob() || e.number>999999 || confirmed || numericPending) {
        (void)esp_ble_confirm_reply(target,false);fail(now,Error::Unauthorized,"unexpected pairing request");break;
      }
      pin=e.number;numericPending=true;status="numeric comparison required";break;
    case GapKind::Identity:
      if(!hw1phone::validIdentity(e.address,e.type) ||
         (!enrolling && (e.type!=identityType || std::memcmp(e.address,identity,6)))) {
        fail(now,Error::Unauthorized,"bond identity mismatch");break;
      }
      std::memcpy(identity,e.address,6);identityType=e.type;identitySeen=true;break;
    case GapKind::Auth: {
      if(retiring)break;
      if(phoneGatt.phase()!=Gatt::Phase::Security) {
        if(e.status || authDeferred)fail(now,Error::Unauthorized,"unexpected authentication result");
        else {deferredAuth=e;authDeferred=true;}
        break;
      }
      esp_ble_bond_dev_t bond{};Wipe wipe{&bond,sizeof(bond)};
      const bool strong=(e.auth&ESP_LE_AUTH_REQ_SC_MITM_BOND)==ESP_LE_AUTH_REQ_SC_MITM_BOND;
      const bool valid=!e.status && strong && identitySeen && (!enrolling || confirmed) && currentJob() &&
                       bondFor(identity,identityType,bond,strong);
      if(!valid || !restorePolicy()) {
        fail(now,Error::Unauthorized,"authenticated SC bond required");break;
      }
      // Numeric comparison and authenticated SC establish the approved bond.
      // Persist it before discovering/negotiating the pipe: a later HELLO/CCCD
      // failure must leave a known, explicitly forgettable bond, not an orphan
      // that silently skips numeric comparison on the next enrollment attempt.
      if(enrolling) {
        bool persist=false;
        if(!blePeerCommitPhonePipeIdentityIfCurrent(intent,identityGeneration,identity,identityType,&persist)) {
          fail(now,Error::Cancelled,"phone enrollment authority changed");break;
        }
        bondSaved=true;enrolling=false;
        if(persist)(void)requestSettingsPersist();
      }
      if(!phoneGatt.authenticated(now)) {
        fail(now,Error::Connect,"bond saved; pipe setup failed");break;
      }
      status="opening pipe";break;
    }
    case GapKind::Reject:
      fail(now,Error::Unauthorized,"numeric comparison required");break;
    case GapKind::Params:
      // Negotiation is advisory; traffic remains paced even when refused.
      break;
  }
}
bool configureSecurityPolicy() {
  priorPolicy=BLESecurity::policy();policyChanged=true;restoreAttempts=0;
  const BLESecurity::Policy strict{ESP_LE_AUTH_REQ_SC_MITM_BOND,ESP_IO_CAP_IO,
      ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK,ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK,16};
  return BLESecurity::applyPolicy(strict)==ESP_OK;
}
void startSecurity(uint32_t now) {
  securityStarted=true;securityAt=now;
  if(!policyChanged || esp_ble_set_encryption(target,ESP_BLE_SEC_ENCRYPT_MITM)!=ESP_OK)
    fail(now,Error::Unauthorized,"security setup failed");
  else status=enrolling?"pairing":"authenticating saved bond";
}
void beginJob(uint32_t now) {
  candidate=securityStarted=confirmed=numericPending=published=bondSaved=authDeferred=false;
  scanStarted=scanStartPending=scanStopPending=false;paramsActive=false;
  at=now;identitySeen=!enrolling;nativeKnown=false;
  if(!enrolling && (!request.savedTarget.addressType1Known ||
      !hw1phone::parseIdentity(request.savedTarget.mac1,request.savedTarget.addressType1,identity))) {
    fail(now,Error::Unauthorized,"no verified phone identity");return;
  }
  identityType=request.savedTarget.addressType1;routeSet();
  if(!privacyReady) {
    privacyPending=true;setStep(Step::Privacy,"configuring privacy",now);routeSet();
    if(esp_ble_gap_config_local_privacy(true)!=ESP_OK){privacyPending=false;fail(now,Error::Connect,"privacy setup failed");}
  } else prepareBond(now);
}
void tickLocked(uint32_t now) {
  if(hostResetWanted.load()) {
    phoneGatt.hostStopped();forgetWanted=forgetPending=false;liveGeneration=0;releaseIds[0]=releaseIds[1]=0;
    active=false;ready=false;standalone=false;stopWanted=false;
    privacyReady=nativeKnown=privacyPending=scanParamsPending=false;
    job=enrolling=policyChanged=scanStarted=scanStartPending=scanStopPending=false;
    releaseSetup();setStep(Step::Idle,"stopped",now);routeSet();hostResetWanted=false;
  }
  if(!active.load())return;
  if(abortLink.exchange(false))fail(now,Error::Resources,"stream cleanup could not be queued");
  if(gapOverflow.exchange(false))fail(now,Error::Resources,"BLE callback queue full");
  GapEvent event;
  for(unsigned i=0;i<8 && xQueueReceive(gapQueue,&event,0)==pdTRUE;++i)consume(event,now);
  if(job && (stopWanted.load() || !currentJob() ||
      (setupHeld && bleCentralAccessoryPending()) ||
      (enrolling && hw1pipe::expired(now,jobAt,60000))))
    fail(now,Error::Cancelled,"phone operation cancelled");
  const bool pressure=bleCentralTxAccessoryPressure() || bleCentralAccessoryBusy() || g2MicAfeFeedIsActive();
  phoneGatt.poll(now,pressure);
  for(auto& id:releaseIds)if(id) {
    auto* stream=phoneGatt.pipe.find(id);
    if(!stream || !stream->live()){phoneGatt.pipe.release(id);id=0;}
  }
  if(step==Step::Waiting) {
    if(stopWanted.load()){job=false;setStep(Step::Idle,"stopped",now);}
    else if(bleCentralSetupTake(true,0)){setupHeld=true;beginJob(now);}
  }
  if(step==Step::Link) {
    if(authDeferred && phoneGatt.phase()==Gatt::Phase::Security) {
      const GapEvent auth=deferredAuth;authDeferred=false;consume(auth,now);
    }
    if(phoneGatt.phase()==Gatt::Phase::Security && !securityStarted)startSecurity(now);
    if(securityStarted && policyChanged && hw1pipe::expired(now,securityAt,enrolling?60000:10000))
      fail(now,Error::Deadline,"authentication timed out");
    if(phoneGatt.ready() && !published) {
      const bool accepted=!enrolling && (bondSaved ? currentJob()
          : blePeerNoteLinkUpIfCurrent(BLE_PEER_PHONE,request,&intent,&identityGeneration));
      if(!accepted){fail(now,Error::Cancelled,"phone intent replaced");}
      else {
        published=true;enrolling=false;ready=true;status="ready";releaseSetup();
      }
    }
    if(phoneGatt.idle() || phoneGatt.phase()==Gatt::Phase::Retiring || phoneGatt.phase()==Gatt::Phase::Quarantined)
      fail(now,phoneGatt.pipe.error,"phone link lost");
    if(ready.load()) {
      bool stream=false;for(const auto& s:phoneGatt.pipe.streams)stream|=s.live();
      if(stream)lastStreamAt=now;
      const bool wantActive=stream || !hw1pipe::expired(now,lastStreamAt,5000);
      if((lastParams==0 || wantActive!=paramsActive) && hw1pipe::expired(now,lastParams,5000)) {
        esp_ble_conn_update_params_t params{};std::memcpy(params.bda,target,6);
        params.min_int=wantActive?24:72;params.max_int=wantActive?36:96;
        params.latency=wantActive?0:4;params.timeout=600;
        (void)esp_ble_gap_update_conn_params(&params);lastParams=now;paramsActive=wantActive;
      }
    }
  }
  if((step==Step::Privacy || step==Step::ScanParams || step==Step::Scanning) &&
     hw1pipe::expired(now,at,3000))fail(now,Error::Deadline,"BLE setup timed out");
  if((step==Step::Retiring || step==Step::Fault) && !forgetPending) {
    if(scanStarted && !scanStopPending) {
      scanStopPending=true;
      if(esp_ble_gap_stop_scanning()!=ESP_OK){setStep(Step::Fault,"scan retirement failed",now);}
    }
    if(phoneGatt.idle() && !privacyPending && !scanParamsPending &&
       !scanStarted && !scanStartPending && !scanStopPending && restorePolicy()) {
      const bool lost=job;job=false;enrolling=false;ready=false;numericPending=false;releaseSetup();
      setStep(Step::Idle,stopWanted.load()?"stopped":"disconnected",now);routeSet();
      if(lost)blePeerNoteLinkLost(BLE_PEER_PHONE);
    } else if(hw1pipe::expired(now,at,4000))setStep(Step::Fault,"phone cleanup incomplete",now);
  }
  if(forgetPending && hw1pipe::expired(now,at,4000))
    setStep(Step::Fault,"phone bond removal incomplete",now);
  if(forgetWanted && !forgetPending && step==Step::Idle)startForget(now);
  if(stopWanted.load() && step==Step::Idle && !forgetWanted) {
    active=false;standalone=false;status="stopped";routeSet();
  }
  routeSet();
}
void run(void*) {
  for(;;) {
    {Lock lock;if(lock)tickLocked(millis());}
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
bool resources() {
  portENTER_CRITICAL(&initMux);
  if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutexStorage);
  if(!gapQueue)gapQueue=xQueueCreateStatic(8,sizeof(GapEvent),gapQueueBytes,&gapQueueStorage);
  portEXIT_CRITICAL(&initMux);
  if(!mutex || !gapQueue)return false;
  if(!worker && xTaskCreateLogged(run,"phone_pipe",8192,nullptr,1,&worker,"BLE phone pipe")!=pdPASS)return false;
  return true;
}
BlePeerConnectAdmission admit(const BlePeerConnectRequest& wanted) {
  if(!active.load() || stopWanted.load())return BlePeerConnectAdmission::ROLE_BLOCKED;
  if(!standalone.load() && !g2BothConnected())return BlePeerConnectAdmission::BUSY;
  Lock lock;if(!lock)return BlePeerConnectAdmission::BUSY;
  if(ready.load())return BlePeerConnectAdmission::ALREADY_UP;
  if(job || forgetWanted || step!=Step::Idle)return BlePeerConnectAdmission::BUSY;
  if(!blePeerConnectRequestIsCurrent(BLE_PEER_PHONE,wanted))return BlePeerConnectAdmission::NO_TARGET;
  request=wanted;intent=wanted.intentGeneration;identityGeneration=wanted.identityGeneration;
  enrolling=false;job=true;jobAt=millis();setStep(Step::Waiting,"waiting for radio",jobAt);return BlePeerConnectAdmission::STARTED;
}
void disconnect() {
  blePeerNoteUserDisconnect(BLE_PEER_PHONE);
  Lock lock;if(lock && job)fail(millis(),Error::Cancelled,"disconnected");
}
const BlePeerOps ops{nullptr,disconnect,phonePipeReady,admit};
const BlePeerSpec spec{BLE_PEER_PHONE,"phone","Phone pipe",1,true,&ops};
}

bool phonePipeStart(bool ownHost) {
  Role role(BleRoleTransition::PHONE_START);if(!role)return false;
  if(active.load())return standalone.load()==ownHost;
  if(!gSettings.bleEnabled || bleStackLifecycleFaulted())return false;
  const char* why=nullptr;if(radioCompanionBlocksRadio(&why))return false;
  if(!resources())return false;
  if(ownHost) {
    if(isG2ClientInitialized() && !deinitG2Client())return false;
    if(isBleServerInitialized() && !deinitBluetooth())return false;
    if(!bluetoothHalStatus().stopped()) {
      const auto stopped=bluetoothHalDeinit();if(!stopped.success){bleStackSetLifecycleFault(true);return false;}
    }
    if(!phonePipeHostStopped())return false;
    if(!bluetoothHalInit(gSettings.bleDeviceName.c_str())) {
      const auto rollback=bluetoothHalDeinit();
      if(!rollback.success)bleStackSetLifecycleFault(true);
      else phonePipeHostStopped();
      return false;
    }
  } else if(!isG2ClientInitialized() || !bluetoothHalStatus().ready())return false;
  Lock lock;if(!lock || step==Step::Fault || !phoneGatt.idle())return false;
  BLEDevice::setGapInterceptor(gapCallback);BLEDevice::setGattcInterceptor(gattCallback);
  bleCentralTxInit();(void)bleRegisterPeer(spec);
  xQueueReset(gapQueue);stopWanted=false;active=true;standalone=ownHost;ready=false;
  setStep(Step::Idle,"disconnected",millis());routeSet();
  return true;
}
bool phonePipeStop() {
  if(!active.load())return true;
  stopWanted=true;
  const uint32_t start=millis();
  while(active.load() && !hw1pipe::expired(millis(),start,4500))vTaskDelay(pdMS_TO_TICKS(10));
  return !active.load();
}
bool phonePipeHostStopped() {
  if(!bluetoothHalStatus().stopped())return false;
  if(!worker){privacyReady=nativeKnown=false;return true;}
  hostResetWanted=true;
  const uint32_t start=millis();
  while(hostResetWanted.load() && !hw1pipe::expired(millis(),start,1000))vTaskDelay(pdMS_TO_TICKS(10));
  return !hostResetWanted.load();
}
bool phonePipeActive(){return active.load();}
bool phonePipeStandalone(){return active.load() && standalone.load();}
bool phonePipeReady(){return ready.load();}
const char* phonePipeStatus(){return status.load();}
void phonePipeTick(){}

bool phonePipeOpen(PhonePipeStream& out,const char* host,uint16_t port,uint32_t timeoutMs,const std::atomic<bool>& cancelled,const std::atomic<bool>* interrupted) {
  const auto isCancelled=[&]{return cancelled.load() || (interrupted && interrupted->load());};
  out={};if(!host || !port || !timeoutMs || !ready.load())return false;
  const size_t length=strnlen(host,254);if(!length || length>253)return false;
  uint8_t address[16];uint8_t type=3;char name[254];
  if(inet_pton(AF_INET,host,address)==1)type=1;
  else if(inet_pton(AF_INET6,host,address)==1)type=2;
  else {for(size_t i=0;i<=length;++i){const char c=host[i];name[i]=(c>='A'&&c<='Z')?char(c+32):c;}}
  const uint32_t start=millis();
  {Lock lock;if(!lock || !ready.load() || isCancelled())return false;
    out.generation=phoneGatt.generation();
    out.id=phoneGatt.pipe.open(type,type==3?reinterpret_cast<const uint8_t*>(name):address,type==3?length:(type==1?4:16),port,start);
    if(!out.id)return false;
  }
  while(!isCancelled() && !hw1pipe::expired(millis(),start,timeoutMs)) {
    {Lock lock;if(lock) {
      auto* stream=out.generation==phoneGatt.generation()?phoneGatt.pipe.find(out.id):nullptr;
      if(!stream || !ready.load() || stream->state==hw1pipe::StreamState::Ended)break;
      if(stream->state==hw1pipe::StreamState::Open)return true;
    }}
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  phonePipeClose(out,true);phonePipeRelease(out);out={};return false;
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
const char* phonePipeCommand(const String& args) {
  if(args=="start") {
    const bool ownHost=!isG2ClientInitialized();
    if(!phonePipeStart(ownHost))return "Error: phone pipe could not start";
    if(ownHost)setSetting(gSettings.bleMode,int(BLE_MODE_PHONE_CLIENT));
    return "Phone pipe started";
  }
  if(args=="stop") {
    Role role(BleRoleTransition::PHONE_STOP);if(!role)return "Error: BLE role transition busy";
    const bool ownHost=phonePipeStandalone();
    if(!phonePipeStop())return "Error: phone pipe cleanup incomplete";
    if(ownHost) {
      const auto stopped=bluetoothHalDeinit();
      if(!stopped.success){bleStackSetLifecycleFault(true);return "Error: BLE host cleanup incomplete";}
      if(!phonePipeHostStopped())return "Error: phone owner cleanup incomplete";
    }
    return "Phone pipe stopped";
  }
  if(args=="disconnect"){disconnect();return "Phone disconnect requested";}
  if(args=="connect") {
    BlePeerConnectRequest wanted;
    if(!blePeerBeginManualConnectRequest(BLE_PEER_PHONE,wanted))return "Error: no authorized phone bond";
    return admit(wanted)==BlePeerConnectAdmission::STARTED?"Phone connect accepted":"Error: phone connect unavailable or busy";
  }
  const auto& auth=currentAuthContext();
  const bool local=(auth.transport==SOURCE_SERIAL || auth.transport==SOURCE_LOCAL_DISPLAY) && currentExecIsAdmin();
  if(args=="forget") {
    if(!local)return "Error: phone unpair requires an authenticated local admin";
    if(!active.load())return "Error: start the phone pipe first";
    BlePeerOwnerSession owner;
    if(!blePeerOwnerSessionSnapshot(BLE_PEER_PHONE,owner) || owner.user!=auth.user)
      return "Error: phone is owned by another account";
    const auto epoch=captureTransportSessionEpoch(auth);
    if(!epoch)return "Error: local authenticated session required";
    Lock lock;if(!lock || forgetWanted || step==Step::Fault)return "Error: phone unpair busy";
    if(!blePeerBeginPhonePipeForget(owner.generation,forgetRequest))return "Error: no current phone pipe identity";
    forgetEpoch=epoch;forgetSource=auth.transport;forgetWanted=true;
    if(job)fail(millis(),Error::Cancelled,"draining phone for unpair");
    return "Phone unpair accepted; check phonepipe status for completion. Remove the P4 bond/allowlist on Android too.";
  }
  if(args=="enroll") {
    if(!local)return "Error: phone enrollment requires an authenticated local admin";
    if(!active.load())return "Error: start the phone pipe first";
    bleStampPairedByIfBlank(BLE_PEER_PHONE);
    BlePeerOwnerSession owner;
    if(!blePeerOwnerSessionSnapshot(BLE_PEER_PHONE,owner) || owner.user!=auth.user)return "Error: phone is owned by another account";
    const auto epoch=captureTransportSessionEpoch(auth);
    if(!epoch)return "Error: local authenticated session required";
    Lock lock;if(!lock || job || forgetWanted || step!=Step::Idle)return "Error: phone pipe busy; disconnect first";
    if(!blePeerBeginManualLearn(BLE_PEER_PHONE,intent,identityGeneration))return "Error: enrollment authority unavailable";
    enrollmentEpoch=epoch;enrollmentSource=auth.transport;strlcpy(enrollmentUser,auth.user.c_str(),sizeof(enrollmentUser));
    enrolling=job=true;request={};jobAt=millis();setStep(Step::Waiting,"waiting to enroll",jobAt);
    return "Enrollment opened for 60 seconds. Compare the phone number with phonepipe status, then phonepipe confirm <six digits>";
  }
  if(args.startsWith("confirm ")) {
    if(!local || args.length()!=14)return "Error: use local phonepipe confirm <six digits>";
    uint32_t number=0;for(unsigned i=8;i<14;++i){if(args[i]<'0'||args[i]>'9')return "Error: six digits required";number=number*10+unsigned(args[i]-'0');}
    Lock lock;if(!lock || !enrolling || !numericPending || !currentJob() ||
        auth.transport!=enrollmentSource || auth.user!=enrollmentUser || captureTransportSessionEpoch(auth)!=enrollmentEpoch)
      return "Error: no matching enrollment in this session";
    if(number!=pin){(void)esp_ble_confirm_reply(target,false);fail(millis(),Error::Unauthorized,"numeric comparison rejected");return "Error: comparison rejected";}
    if(esp_ble_confirm_reply(target,true)!=ESP_OK){fail(millis(),Error::Unauthorized,"comparison reply failed");return "Error: comparison reply failed";}
    confirmed=true;numericPending=false;status="confirming bond";return "Numeric comparison accepted; waiting for authenticated bond";
  }
  if(args.length()==0 || args=="status") {
    static char text[128];Lock lock;if(!lock)return "Phone pipe busy";
    if(numericPending && local && auth.user==enrollmentUser && captureTransportSessionEpoch(auth)==enrollmentEpoch)
      snprintf(text,sizeof(text),"Phone pairing number: %06lu. Compare both displays before confirming.",(unsigned long)pin);
    else snprintf(text,sizeof(text),"Phone pipe: %s",status.load());
    return text;
  }
  return "Usage: phonepipe [status|start|stop|enroll|confirm <six digits>|connect|disconnect|forget]";
}
#endif
