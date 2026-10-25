#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
using String=std::string;
constexpr int BLE_PEER_PHONE=2;
struct BlePeerSavedTarget {char mac1[18]{};bool addressType1Known=false;uint8_t addressType1=0;};
struct BlePhoneForget {uint32_t intent=0,identity=0;BlePeerSavedTarget target;};
struct PeerReconnectRuntime {
 uint32_t intentGeneration=7,identityGeneration=9,ownerGeneration=11;
 bool ownerAuthorityAvailable=true,hasSavedTarget=true,userDisconnect=false,wantReconnect=true,reseekEvenIfNoAuto=true;
 bool autoReconnect=false;
 unsigned launchedAttempts=3,dueMs=9000;
 BlePeerSavedTarget savedTarget;
};
struct BlePeerData {String pipeEnrollment="app-key-v1",mac1="C2:00:00:00:00:02";bool autoReconnect=false;};
static PeerReconnectRuntime sReconnect[3];
static BlePeerData gBlePeerData[3];
static bool lockAvailable=true;
struct PeerDataGuard {explicit operator bool()const{return lockAvailable;}};
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static void reconnectBumpIntentLocked(PeerReconnectRuntime& s){++s.intentGeneration;}
static void reconnectBumpIdentityLocked(PeerReconnectRuntime& s){++s.identityGeneration;}
#define strlcpy fixture_strlcpy
static size_t fixture_strlcpy(char* dst,const char* src,size_t cap){size_t n=strlen(src);if(cap){size_t k=n<cap-1?n:cap-1;memcpy(dst,src,k);dst[k]=0;}return n;}
static bool savedTargetMatches(const BlePeerSavedTarget& a,const BlePeerSavedTarget& b){return !strcmp(a.mac1,b.mac1)&&a.addressType1Known==b.addressType1Known;}
static void noteLinkUpLocked(PeerReconnectRuntime& s){s.wantReconnect=false;}
// INSERT_PRODUCTION
static void reset(){for(auto& s:sReconnect){s={};std::strcpy(s.savedTarget.mac1,"app-key-v1");}for(auto& p:gBlePeerData)p={};lockAvailable=true;}
int main(){
 {reset();BlePhoneForget request;
  assert(!blePeerBeginPhonePipeForget(12,request) && request.intent==0);
  assert(blePeerBeginPhonePipeForget(11,request));
  assert(sReconnect[2].userDisconnect && !sReconnect[2].wantReconnect && !sReconnect[2].dueMs);
  assert(request.intent==8 && request.identity==9 && !std::strcmp(request.target.mac1,"app-key-v1"));
  assert(blePeerPhonePipeForgetIsCurrent(request));
  assert(blePeerCommitPhonePipeForget(request));
  assert(!blePeerPhonePipeForgetIsCurrent(request) && !blePeerCommitPhonePipeForget(request));
  assert(gBlePeerData[2].pipeEnrollment.empty() && gBlePeerData[2].mac1=="C2:00:00:00:00:02");
  assert(!sReconnect[2].hasSavedTarget && !sReconnect[2].savedTarget.mac1[0] && sReconnect[2].ownerGeneration==11);
  assert(gBlePeerData[0].pipeEnrollment=="app-key-v1" && sReconnect[0].wantReconnect); // other peers untouched
 }
 for(int changed=0;changed<6;++changed){reset();BlePhoneForget request;assert(blePeerBeginPhonePipeForget(11,request));
  auto& s=sReconnect[2];
  if(changed==0)++s.intentGeneration;
  if(changed==1)++s.identityGeneration;
  if(changed==2)s.ownerAuthorityAvailable=false;
  if(changed==3)s.userDisconnect=false;
  if(changed==4)s.hasSavedTarget=false;
  if(changed==5)lockAvailable=false;
  assert(!blePeerCommitPhonePipeForget(request));
  assert(gBlePeerData[2].pipeEnrollment=="app-key-v1");
 }
 {reset();assert(blePeerClearPhonePipeEnrollment());assert(gBlePeerData[2].pipeEnrollment.empty());
  assert(!sReconnect[2].hasSavedTarget&&sReconnect[2].userDisconnect&&sReconnect[2].ownerGeneration==11);
  assert(gBlePeerData[2].mac1=="C2:00:00:00:00:02"&&gBlePeerData[0].pipeEnrollment=="app-key-v1");
  auto& state=sReconnect[2];state.userDisconnect=false;uint32_t intent=state.intentGeneration,identity=state.identityGeneration;bool persist=false;
  assert(blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist)&&persist);
  assert(state.hasSavedTarget&&!state.savedTarget.addressType1Known&&gBlePeerData[2].pipeEnrollment=="app-key-v1");
  assert(identity==state.identityGeneration&&intent==state.intentGeneration);
  ++state.intentGeneration;assert(!blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist)&&!persist);
 }
 // F1: enrollment turns on automatic reconnect (persisted mirror and live
 // policy), persists a policy-only change, and rolls back with a lost fence.
 {reset();auto& state=sReconnect[2];auto& data=gBlePeerData[2];
  uint32_t intent=state.intentGeneration,identity=state.identityGeneration;bool persist=false;
  assert(blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist));
  assert(persist&&data.autoReconnect&&state.autoReconnect&&identity==9);  // same target: policy-only change
  persist=true;assert(blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist)&&!persist);
  assert(!gBlePeerData[0].autoReconnect&&!sReconnect[0].autoReconnect);  // other peers untouched
 }
 for(int lost=0;lost<4;++lost){reset();auto& state=sReconnect[2];auto& data=gBlePeerData[2];
  data.pipeEnrollment="";uint32_t intent=state.intentGeneration,identity=state.identityGeneration;bool persist=true;
  if(lost==0)++state.intentGeneration;
  if(lost==1)++state.identityGeneration;
  if(lost==2)state.userDisconnect=true;
  if(lost==3)state.ownerAuthorityAvailable=false;
  assert(!blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist)&&!persist);
  assert(!data.autoReconnect&&!state.autoReconnect&&data.pipeEnrollment.empty());
 }
 {reset();auto& state=sReconnect[2];state.autoReconnect=true;gBlePeerData[2].autoReconnect=true;
  state.savedTarget={};uint32_t intent=state.intentGeneration,identity=state.identityGeneration;bool persist=false;
  assert(blePeerCommitPhonePipeEnrollmentIfCurrent(intent,identity,&persist)&&persist&&identity==10);  // target change only
 }
 puts("Phone unpair registry: owner/intent fences, one-shot commit, enrollment enables auto-reconnect with rollback, console and other-peer preservation passed");
}
