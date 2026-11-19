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
  uint8_t launchedAttempts=0;uint32_t dueMs=0,intentGeneration=7,identityGeneration=9;
};
static PeerReconnectRuntime sReconnect[BLE_PEER_MAX];
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int sReconnectMux=0;
static void reconnectBumpIntentLocked(PeerReconnectRuntime& s){++s.intentGeneration;}
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
int main() {
  // NoteLinkLost uses attempt 0: 5 s for glasses and phone, 2 s for the ring
  // (faceclaw's RING_RECONNECT_DELAY_MS).
  for(auto kind:{BLE_PEER_G2_GLASSES,BLE_PEER_PHONE})assert(bleReconnectBackoffMs(kind,0)==5000);
  assert(bleReconnectBackoffMs(BLE_PEER_R1_RING,0)==2000);
  const uint32_t phone[]={15000,30000,30000,30000,30000,30000,30000,30000},
                 glasses[]={15000,45000,90000,180000,180000,180000,180000,180000},
                 // Step n equals the ring's own hold after failure n
                 // (2 s doubling to 60 s), so that hold decides the gap.
                 ring[]={2000,4000,8000,16000,32000,60000,60000,60000};
  for(unsigned n=1;n<=8;++n) {
    assert(gapAfter(BLE_PEER_PHONE,n)==phone[n-1]);
    assert(gapAfter(BLE_PEER_G2_GLASSES,n)==glasses[n-1]);
    assert(gapAfter(BLE_PEER_R1_RING,n)==ring[n-1]);
  }
  assert(bleReconnectBackoffMs(BLE_PEER_PHONE,255)==30000 && bleReconnectBackoffMs(BLE_PEER_G2_GLASSES,255)==180000);
  assert(bleReconnectBackoffMs(BLE_PEER_R1_RING,255)==60000);
  // Busy/coalesced/role retries are unchanged and independent of the cap.
  for(auto result:{BlePeerConnectAdmission::BUSY,BlePeerConnectAdmission::COALESCED,BlePeerConnectAdmission::ROLE_BLOCKED}) {
    sReconnect[BLE_PEER_PHONE]=PeerReconnectRuntime{};ReconnectAdmissionClaim claim;claim.kind=BLE_PEER_PHONE;
    claim.intentGeneration=7;claim.identityGeneration=9;assert(commitAdmission(claim,result,100));
    const uint32_t expected=result==BlePeerConnectAdmission::BUSY?kAdmissionBusyRetryMs:
        result==BlePeerConnectAdmission::COALESCED?kAdmissionCoalescedRetryMs:kAdmissionRoleRetryMs;
    assert(sReconnect[BLE_PEER_PHONE].dueMs==100+expected && !sReconnect[BLE_PEER_PHONE].launchedAttempts);
  }
  puts("Reconnect schedule: phone 5/15/30 s capped (A-3); glasses 5/15/45/90/180 s; ring 2 s, then 2/4/8/16/32/60 s (faceclaw); busy/role retries unchanged");
}
