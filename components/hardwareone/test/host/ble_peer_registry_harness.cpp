#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
static void quietLog(const char*,...){}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(ms) (ms)
static uint32_t gNow=1000;
static uint32_t millis(){return gNow;}
static void vTaskDelay(uint32_t ticks){gNow+=ticks;}
static bool bleSubsystemActive(){return true;}
static bool isBleServerInitialized(){return false;}
// INSERT_TYPES
// INSERT_PRODUCTION
// The pipe's callbacks, bound by the production ops table below.
static bool gPhoneUp=false;
static bool phonePipeReady(){return gPhoneUp;}
namespace pipe {
static unsigned admits=0;
static BlePeerConnectRequest last;
static BlePeerConnectAdmission admit(const BlePeerConnectRequest& request){++admits;last=request;return BlePeerConnectAdmission::STARTED;}
static void disconnect(){}
// INSERT_PIPE_SPEC
}
// A second connectable peer that registers between the builtins and the pipe.
static unsigned glassesAdmits=0;
static bool dropDuringAdmit=false;  // the worker drops the job before the commit
static BlePeerConnectAdmission glassesAdmit(const BlePeerConnectRequest& request){
  ++glassesAdmits;
  if(dropDuringAdmit)(void)blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,request.intentGeneration,request.identityGeneration);
  return BlePeerConnectAdmission::STARTED;
}
static bool glassesUp(){return false;}
static const BlePeerOps kGlassesOps{nullptr,nullptr,glassesUp,glassesAdmit};
static const BlePeerSpec kGlasses{BLE_PEER_G2_GLASSES,"g2","G2 Glasses",2,true,&kGlassesOps};
static void enroll(BlePeerKind kind) {
  PeerReconnectRuntime& s=sReconnect[kind];
  s.autoReconnect=s.hasSavedTarget=s.ownerAuthorityAvailable=true;std::strcpy(s.savedTarget.mac1,"app-key-v1");
}
int main() {
  // Boot order: builtins, then the peer modules, then phonePipeStart.
  bleRegisterBuiltinPeers();
  assert(bleRegisterPeer(kGlasses));
  const BlePeerSpec* builtin=bleFindPeerByName("phone");
  assert(builtin && !builtin->connectable && bleRegisteredPeerCount()==2);
  assert(bleRegisterPeer(pipe::spec) && bleRegisterPeer(pipe::spec));
  // The pipe's spec replaces the builtin in place: same slot, no duplicate.
  assert(bleRegisteredPeerCount()==2);
  assert(bleRegisteredPeerAt(0)==&pipe::spec && bleRegisteredPeerAt(1)==&kGlasses);
  assert(bleFindPeerByName("phone")==&pipe::spec && bleFindPeerByName("phone")->connectable);
  assert(bleFindPeer(BLE_PEER_PHONE)==&pipe::spec);
  // A different kind still cannot take the phone's name.
  static const BlePeerSpec imposter{BLE_PEER_R1_RING,"phone","Imposter",1,false,nullptr};
  assert(!bleRegisterPeer(imposter) && bleRegisteredPeerCount()==2);

  // F1 enrollment leaves autoReconnect and a saved target: boot reconnect
  // reaches the pipe's admission once, then paces the glasses 2 s later.
  enroll(BLE_PEER_PHONE);enroll(BLE_PEER_G2_GLASSES);
  const uint32_t intent=sReconnect[BLE_PEER_PHONE].intentGeneration;
  bleBootReconnect();
  assert(pipe::admits==1 && glassesAdmits==1 && gNow==3000);
  assert(pipe::last.autoReconnect && !pipe::last.explicitReseek && pipe::last.intentGeneration==intent+1);
  assert(!std::strcmp(pipe::last.savedTarget.mac1,"app-key-v1"));
  assert(!sReconnect[BLE_PEER_PHONE].admissionInFlight && sReconnect[BLE_PEER_PHONE].dueMs==1000+15000);

  // Link up, then lost: the tick redials the phone 5 s later (A-3).
  blePeerNoteLinkUp(BLE_PEER_PHONE);blePeerNoteLinkUp(BLE_PEER_G2_GLASSES);
  gNow=20000;blePeerNoteLinkLost(BLE_PEER_PHONE);
  assert(sReconnect[BLE_PEER_PHONE].wantReconnect && sReconnect[BLE_PEER_PHONE].dueMs==25000);
  gNow=24999;bleAutoReconnectTick();assert(pipe::admits==1);
  gNow=25000;bleAutoReconnectTick();assert(pipe::admits==2 && pipe::last.autoReconnect && glassesAdmits==1);
  // That attempt didn't connect: the next one is 15 s later.
  gNow=40000;bleAutoReconnectTick();assert(pipe::admits==3);
  // Once the link is up, a due tick reports ALREADY_UP without admitting.
  gPhoneUp=true;gNow=70000;bleAutoReconnectTick();
  assert(pipe::admits==3 && !sReconnect[BLE_PEER_PHONE].wantReconnect);

  // A one-shot reseek (auto-reconnect off) is consumed when admitted. If its
  // job is then dropped unrun, the job's generations put it back 15 s later;
  // a newer intent, another identity or a user disconnect refuses.
  PeerReconnectRuntime& g=sReconnect[BLE_PEER_G2_GLASSES];
  g.autoReconnect=false;
  gNow=80000;blePeerRequestReseek(BLE_PEER_G2_GLASSES);
  const uint32_t nudge=g.intentGeneration,identity=g.identityGeneration;
  bleAutoReconnectTick();
  assert(glassesAdmits==2 && !g.wantReconnect && !g.reseekEvenIfNoAuto && g.intentGeneration==nudge);
  assert(blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,nudge,identity));
  assert(g.wantReconnect && g.reseekEvenIfNoAuto && g.dueMs==80000+15000);
  assert(!blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,nudge,identity));  // already armed
  gNow=94999;bleAutoReconnectTick();assert(glassesAdmits==2);
  gNow=95000;bleAutoReconnectTick();assert(glassesAdmits==3 && !g.wantReconnect);
  assert(!blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,nudge+1,identity));
  assert(!blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,nudge,identity+1));
  g.userDisconnect=true;
  assert(!blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,nudge,identity) && !g.wantReconnect);
  g.userDisconnect=false;
  // Auto-reconnect keeps its own intent; there is nothing to put back.
  g.autoReconnect=true;blePeerNoteLinkLost(BLE_PEER_G2_GLASSES);
  assert(g.wantReconnect && !blePeerRestoreDroppedReseek(BLE_PEER_G2_GLASSES,g.intentGeneration,g.identityGeneration));

  // The worker can drop the job while its admission is still in flight, before
  // the main loop commits: the commit then keeps the one-shot nudge (due 15 s
  // later) instead of consuming it, and the next run consumes it normally.
  g.autoReconnect=false;g.wantReconnect=false;g.reseekEvenIfNoAuto=false;g.dueMs=0;
  gNow=200000;blePeerRequestReseek(BLE_PEER_G2_GLASSES);
  dropDuringAdmit=true;bleAutoReconnectTick();dropDuringAdmit=false;
  assert(glassesAdmits==4 && g.wantReconnect && g.reseekEvenIfNoAuto && g.dueMs==200000+15000);
  assert(!g.droppedBeforeCommit && !g.admissionInFlight);
  gNow=214999;bleAutoReconnectTick();assert(glassesAdmits==4);
  gNow=215000;bleAutoReconnectTick();assert(glassesAdmits==5 && !g.wantReconnect && !g.reseekEvenIfNoAuto);
  puts("Peer registry: the pipe's spec replaces the builtin phone in place; boot reconnect and the tick reach it;"
       " a dropped one-shot reseek is put back only while its generations are current, also before its commit");
}
