#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
using String=std::string;
constexpr int BLE_PEER_PHONE=2;
struct BlePeerSavedTarget {char mac1[18]{};bool addressType1Known=true;uint8_t addressType1=1;};
struct BlePhoneForget {uint32_t intent=0,identity=0;BlePeerSavedTarget target;};
struct PeerReconnectRuntime {
 uint32_t intentGeneration=7,identityGeneration=9,ownerGeneration=11;
 bool ownerAuthorityAvailable=true,hasSavedTarget=true,userDisconnect=false,wantReconnect=true,reseekEvenIfNoAuto=true;
 unsigned launchedAttempts=3,dueMs=9000;
 BlePeerSavedTarget savedTarget;
};
struct BlePeerData {String pipeIdentity="C2:00:00:00:00:01",mac1="C2:00:00:00:00:02";uint8_t pipeIdentityType=1;};
static PeerReconnectRuntime sReconnect[3];
static BlePeerData gBlePeerData[3];
static bool lockAvailable=true;
struct PeerDataGuard {explicit operator bool()const{return lockAvailable;}};
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static void reconnectBumpIntentLocked(PeerReconnectRuntime& s){++s.intentGeneration;}
static void reconnectBumpIdentityLocked(PeerReconnectRuntime& s){++s.identityGeneration;}
// INSERT_PRODUCTION
static void reset(){for(auto& s:sReconnect){s={};std::strcpy(s.savedTarget.mac1,"C2:00:00:00:00:01");}for(auto& p:gBlePeerData)p={};lockAvailable=true;}
int main(){
 {reset();BlePhoneForget request;
  assert(!blePeerBeginPhonePipeForget(12,request) && request.intent==0);
  assert(blePeerBeginPhonePipeForget(11,request));
  assert(sReconnect[2].userDisconnect && !sReconnect[2].wantReconnect && !sReconnect[2].dueMs);
  assert(request.intent==8 && request.identity==9 && !std::strcmp(request.target.mac1,"C2:00:00:00:00:01"));
  assert(blePeerPhonePipeForgetIsCurrent(request));
  assert(blePeerCommitPhonePipeForget(request));
  assert(!blePeerPhonePipeForgetIsCurrent(request) && !blePeerCommitPhonePipeForget(request));
  assert(gBlePeerData[2].pipeIdentity.empty() && gBlePeerData[2].mac1=="C2:00:00:00:00:02");
  assert(!sReconnect[2].hasSavedTarget && !sReconnect[2].savedTarget.mac1[0] && sReconnect[2].ownerGeneration==11);
  assert(gBlePeerData[0].pipeIdentity=="C2:00:00:00:00:01" && sReconnect[0].wantReconnect); // other peers untouched
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
  assert(gBlePeerData[2].pipeIdentity=="C2:00:00:00:00:01" && gBlePeerData[2].pipeIdentityType==1);
 }
 puts("Phone unpair registry: owner/intent fences, one-shot commit, console and other-peer preservation passed");
}
