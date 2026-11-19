#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
enum BlePeerKind : uint8_t { BLE_PEER_G2_GLASSES=0, BLE_PEER_R1_RING=1, BLE_PEER_PHONE=2, BLE_PEER_MAX=8 };
enum class BlePeerConnectAdmission : uint8_t { STARTED=0, COALESCED, BUSY, ALREADY_UP, NO_TARGET, ROLE_BLOCKED };
struct BlePeerSavedTarget { char mac1[18]{}; };
struct PeerReconnectRuntime {
  bool userDisconnect=false,wantReconnect=true,reseekEvenIfNoAuto=false,autoReconnect=true;
  bool hasSavedTarget=true,ownerAuthorityAvailable=true,admissionInFlight=true,droppedBeforeCommit=false;
  uint8_t launchedAttempts=0;uint32_t dueMs=0,reseekUntilMs=0;uint8_t lastAdmission=0xff;
  uint32_t intentGeneration=7,identityGeneration=9;
};
static PeerReconnectRuntime sReconnect[BLE_PEER_MAX];
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define DEBUG_G2F(...) ((void)0)
static int sReconnectMux=0;
static uint32_t clockMs=1000;
static uint32_t millis(){return clockMs;}
static void reconnectBumpIntentLocked(PeerReconnectRuntime& s){++s.intentGeneration;}
// The registry, as blePeerRequestReseek reads it: one connectable phone.
struct BlePeerOps{bool (*connectSaved)(void);void (*disconnect)(void);bool (*isConnected)(void);void* admission;};
struct BlePeerSpec{BlePeerKind kind;const char* name;const BlePeerOps* ops;bool connectable;};
static bool linked=false;
static bool isLinked(){return linked;}
static bool connectSaved(){return true;}
static const BlePeerOps kOps{connectSaved,nullptr,isLinked,nullptr};
static const BlePeerSpec kSpecs[3]={{BLE_PEER_G2_GLASSES,"g2-glasses",&kOps,true},{BLE_PEER_R1_RING,"r1-ring",&kOps,true},
                                    {BLE_PEER_PHONE,"phone",&kOps,true}};
static bool bleIsPeerRegistered(BlePeerKind kind){return kind<=BLE_PEER_PHONE;}
static const BlePeerSpec* bleFindPeer(BlePeerKind kind){return kind<=BLE_PEER_PHONE?&kSpecs[kind]:nullptr;}
static bool peerHasConnectAdmission(const BlePeerSpec* p){return p && p->connectable && p->ops;}
// INSERT_PRODUCTION
// Each STARTED admission that does not reach link-up schedules the next one;
// returns the gap the scheduler leaves after each attempt.
static uint32_t gapAfter(BlePeerKind kind,unsigned attempts) {
  sReconnect[kind]=PeerReconnectRuntime{};uint32_t now=1000,gap=0;
  for(unsigned i=0;i<attempts;++i) {
    ReconnectAdmissionClaim claim;claim.kind=kind;claim.intentGeneration=7;claim.identityGeneration=9;
    sReconnect[kind].admissionInFlight=true;
    assert(commitAdmission(claim,BlePeerConnectAdmission::STARTED,now));
    gap=sReconnect[kind].dueMs-now;now=sReconnect[kind].dueMs;
  }
  return gap;
}
// A fresh phone runtime: auto as given, a saved target, nothing scheduled.
static PeerReconnectRuntime& phone(bool autoReconnect) {
  PeerReconnectRuntime& s=sReconnect[BLE_PEER_PHONE];s=PeerReconnectRuntime{};
  s.autoReconnect=autoReconnect;s.wantReconnect=false;s.admissionInFlight=false;return s;
}
// The scheduler admits (STARTED) at its due time.
static void admitNow(){
  PeerReconnectRuntime& s=sReconnect[BLE_PEER_PHONE];
  ReconnectAdmissionClaim claim;claim.kind=BLE_PEER_PHONE;claim.intentGeneration=s.intentGeneration;
  claim.identityGeneration=s.identityGeneration;s.admissionInFlight=true;
  assert(commitAdmission(claim,BlePeerConnectAdmission::STARTED,clockMs));
}
int main() {
  // NoteLinkLost uses attempt 0: 5 s for the glasses, 2 s for the phone and
  // the ring (faceclaw's RING_RECONNECT_DELAY_MS).
  assert(bleReconnectBackoffMs(BLE_PEER_G2_GLASSES,0)==5000);
  assert(bleReconnectBackoffMs(BLE_PEER_PHONE,0)==2000);
  assert(bleReconnectBackoffMs(BLE_PEER_R1_RING,0)==2000);
  const uint32_t phone8[]={4000,8000,16000,30000,30000,30000,30000,30000},
                 glasses[]={15000,45000,90000,180000,180000,180000,180000,180000},
                 // Step n equals the ring's own hold after failure n
                 // (2 s doubling to 60 s), so that hold decides the gap.
                 ring[]={2000,4000,8000,16000,32000,60000,60000,60000};
  for(unsigned n=1;n<=8;++n) {
    assert(gapAfter(BLE_PEER_PHONE,n)==phone8[n-1]);
    assert(gapAfter(BLE_PEER_G2_GLASSES,n)==glasses[n-1]);
    assert(gapAfter(BLE_PEER_R1_RING,n)==ring[n-1]);
  }
  // The phone: 2/4/8/16 s, 30 s up to attempt 24 (about 10 min), 60 s for good.
  const uint32_t phoneSteps[]={2000,4000,8000,16000,30000};
  for(uint8_t a=0;a<5;++a)assert(bleReconnectBackoffMs(BLE_PEER_PHONE,a)==phoneSteps[a]);
  for(uint8_t a=5;a<=24;++a)assert(bleReconnectBackoffMs(BLE_PEER_PHONE,a)==30000);
  for(unsigned a=25;a<=255;++a)assert(bleReconnectBackoffMs(BLE_PEER_PHONE,uint8_t(a))==60000);
  {uint32_t outage=0;for(uint8_t a=0;a<=24;++a)outage+=bleReconnectBackoffMs(BLE_PEER_PHONE,a);
   assert(outage>=9*60000 && outage<=11*60000);}  // about 10 min before 60 s
  assert(bleReconnectBackoffMs(BLE_PEER_G2_GLASSES,255)==180000 && bleReconnectBackoffMs(BLE_PEER_R1_RING,255)==60000);
  // Busy/coalesced/role retries are unchanged and independent of the cap; the
  // last admission is recorded.
  for(auto result:{BlePeerConnectAdmission::BUSY,BlePeerConnectAdmission::COALESCED,BlePeerConnectAdmission::ROLE_BLOCKED}) {
    sReconnect[BLE_PEER_PHONE]=PeerReconnectRuntime{};ReconnectAdmissionClaim claim;claim.kind=BLE_PEER_PHONE;
    claim.intentGeneration=7;claim.identityGeneration=9;assert(commitAdmission(claim,result,100));
    const uint32_t expected=result==BlePeerConnectAdmission::BUSY?kAdmissionBusyRetryMs:
        result==BlePeerConnectAdmission::COALESCED?kAdmissionCoalescedRetryMs:kAdmissionRoleRetryMs;
    assert(sReconnect[BLE_PEER_PHONE].dueMs==100+expected && !sReconnect[BLE_PEER_PHONE].launchedAttempts);
    assert(sReconnect[BLE_PEER_PHONE].lastAdmission==uint8_t(result));
  }
  // An attempt that ends unlinked schedules the next one from its end at the
  // backoff, without an intent bump (the job's generation stays current).
  {PeerReconnectRuntime& s=phone(true);s.wantReconnect=true;clockMs=1000;admitNow();
   const uint32_t intent=s.intentGeneration;assert(s.launchedAttempts==1);
   clockMs=9000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,intent,9);
   assert(s.intentGeneration==intent && s.wantReconnect && s.dueMs==9000+4000);
   // A stale generation (a newer intent) changes nothing.
   const uint32_t due=s.dueMs;clockMs=9500;blePeerNoteAttemptFailed(BLE_PEER_PHONE,intent-1,9);assert(s.dueMs==due);
   blePeerNoteAttemptFailed(BLE_PEER_PHONE,intent,8);assert(s.dueMs==due);
   // A user disconnect makes it a no-op too.
   s.userDisconnect=true;blePeerNoteAttemptFailed(BLE_PEER_PHONE,intent,9);assert(s.dueMs==due);}
  // Auto on with no episode armed (a manual attempt): its end starts one.
  {PeerReconnectRuntime& s=phone(true);clockMs=2000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);
   assert(s.wantReconnect && !s.reseekEvenIfNoAuto && s.dueMs==2000+2000 && s.intentGeneration==7);}
  // Auto off, no window: an attempt end does not re-arm anything.
  {PeerReconnectRuntime& s=phone(false);clockMs=2000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);
   assert(!s.wantReconnect && !s.dueMs);}
  // Expedite keeps the intent, wants the peer, resets the backoff and pulls
  // the due time to now (never later); refused after a user disconnect, with
  // no saved target or no owner authority.
  {PeerReconnectRuntime& s=phone(true);s.wantReconnect=true;s.launchedAttempts=6;s.dueMs=50000;clockMs=3000;
   assert(blePeerExpediteReconnect(BLE_PEER_PHONE,0));
   assert(s.intentGeneration==7 && s.wantReconnect && !s.launchedAttempts && s.dueMs==3000 && !s.reseekUntilMs);
   s.dueMs=2500;assert(blePeerExpediteReconnect(BLE_PEER_PHONE,60000) && s.dueMs==2500);  // already due: kept
   s.dueMs=0;assert(blePeerExpediteReconnect(BLE_PEER_PHONE,0) && s.dueMs==3000);       // 0 reads as now
   s.userDisconnect=true;s.dueMs=9000;assert(!blePeerExpediteReconnect(BLE_PEER_PHONE,60000) && s.dueMs==9000);
   s.userDisconnect=false;s.hasSavedTarget=false;assert(!blePeerExpediteReconnect(BLE_PEER_PHONE,60000));
   s.hasSavedTarget=true;s.ownerAuthorityAvailable=false;assert(!blePeerExpediteReconnect(BLE_PEER_PHONE,60000));}
  // A linked peer (the phone READY) is never armed: an expedite then would
  // reach the scheduler as ALREADY_UP.
  {PeerReconnectRuntime& s=phone(true);clockMs=3000;linked=true;
   assert(!blePeerExpediteReconnect(BLE_PEER_PHONE,60000) && !s.wantReconnect && !s.dueMs && !s.reseekUntilMs);
   linked=false;assert(blePeerExpediteReconnect(BLE_PEER_PHONE,0) && s.wantReconnect && s.dueMs==3000);}
  // ALREADY_UP: the phone's READY job holds the current intent, so the commit
  // clears the episode without a bump (a bump drops the link as "phone intent
  // changed"); other peers keep the bump.
  for(BlePeerKind kind:{BLE_PEER_PHONE,BLE_PEER_R1_RING,BLE_PEER_G2_GLASSES}) {
    PeerReconnectRuntime& s=sReconnect[kind];s=PeerReconnectRuntime{};s.launchedAttempts=3;s.dueMs=900;
    ReconnectAdmissionClaim claim;claim.kind=kind;claim.intentGeneration=7;claim.identityGeneration=9;
    assert(commitAdmission(claim,BlePeerConnectAdmission::ALREADY_UP,1000));
    assert(!s.wantReconnect && !s.launchedAttempts && !s.dueMs);
    assert(s.intentGeneration==(kind==BLE_PEER_PHONE?7u:8u));
  }
  // Auto off: window 0 adds nothing (the user's auto=off holds) unless a
  // reseek is already armed; a window opens a reseek that lasts.
  {PeerReconnectRuntime& s=phone(false);clockMs=4000;
   assert(!blePeerExpediteReconnect(BLE_PEER_PHONE,0) && !s.wantReconnect && !s.reseekEvenIfNoAuto);
   assert(blePeerExpediteReconnect(BLE_PEER_PHONE,60000) && s.wantReconnect && s.reseekEvenIfNoAuto);
   assert(s.reseekUntilMs==64000 && s.dueMs==4000 && s.intentGeneration==7);
   clockMs=5000;assert(blePeerExpediteReconnect(BLE_PEER_PHONE,0) && s.reseekUntilMs==64000);  // armed: allowed
   assert(blePeerExpediteReconnect(BLE_PEER_PHONE,10000) && s.reseekUntilMs==64000);  // never shortened
   // The reseek window keeps retrying with auto off until it ends, then the
   // next admission consumes it.
   clockMs=6000;admitNow();
   assert(s.wantReconnect && s.reseekEvenIfNoAuto && s.launchedAttempts==1 && s.dueMs==6000+4000);
   clockMs=12000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);assert(s.dueMs==12000+4000);
   clockMs=16000;admitNow();assert(s.wantReconnect && s.launchedAttempts==2);
   clockMs=21000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);assert(s.dueMs==21000+8000);
   clockMs=64000;admitNow();  // the window ended: consumed, the intent kept for the job
   assert(!s.wantReconnect && !s.reseekEvenIfNoAuto && !s.dueMs && s.intentGeneration==7);
   clockMs=70000;blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);assert(!s.wantReconnect && !s.dueMs);}
  // A consumed one-shot whose attempt fails inside its window is re-armed.
  {PeerReconnectRuntime& s=phone(false);s.reseekUntilMs=40000;clockMs=10000;
   blePeerNoteAttemptFailed(BLE_PEER_PHONE,7,9);
   assert(s.wantReconnect && s.reseekEvenIfNoAuto && s.dueMs==10000+2000 && s.intentGeneration==7);}
  // RequestReseek: the phone gets a 120 s window; the ring and the glasses none.
  {PeerReconnectRuntime& s=phone(false);clockMs=20000;blePeerRequestReseek(BLE_PEER_PHONE);
   assert(s.wantReconnect && s.reseekEvenIfNoAuto && s.reseekUntilMs==140000 && s.intentGeneration==8 && s.dueMs==20000);
   linked=true;s=PeerReconnectRuntime{};blePeerRequestReseek(BLE_PEER_PHONE);assert(!s.reseekUntilMs);linked=false;
   for(auto kind:{BLE_PEER_G2_GLASSES,BLE_PEER_R1_RING}){
    sReconnect[kind]=PeerReconnectRuntime{};sReconnect[kind].autoReconnect=false;blePeerRequestReseek(kind);
    assert(sReconnect[kind].reseekEvenIfNoAuto && !sReconnect[kind].reseekUntilMs);}}
  // Link-up ends the window and the episode.
  {PeerReconnectRuntime& s=phone(false);s.reseekUntilMs=99000;s.wantReconnect=true;noteLinkUpLocked(s);
   assert(!s.reseekUntilMs && !s.wantReconnect && !s.launchedAttempts);}
  puts("Reconnect schedule: phone 2/4/8/16 s, 30 s to attempt 24, then 60 s, timed from each attempt's end; "
       "expedite keeps the intent; reseek windows (manual 60 s, restore 120 s); glasses 5/15/45/90/180 s; "
       "ring 2 s, then 2/4/8/16/32/60 s (faceclaw); busy/role retries unchanged");
}
