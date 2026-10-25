#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
enum BlePeerKind : uint8_t { BLE_PEER_G2_GLASSES=0, BLE_PEER_R1_RING=1, BLE_PEER_PHONE=2 };
enum class BleRoleTransition { RECOVERING };
static bool roleAvailable=true;
struct G2BleRoleTransitionClaim {
  explicit G2BleRoleTransitionClaim(BleRoleTransition) {}
  explicit operator bool() const { return roleAvailable; }
};
static const uint8_t kBleWedgeStreakTrip=2;
static volatile uint8_t gBleWedgeStreak=0;
static volatile uint8_t gBleClientRetirementPeerMask=0;
static const uint32_t kBleRetirementGraceMs=3000;
static const uint32_t kBleRecycleMinGapMs=10UL*60UL*1000UL;
static uint32_t sBleLastRecycleMs=0,retirementAge=0,clockMs=1000;
static bool clientRuntime=true,ringUp=false,ringOpen=false,phoneReady=false,halUp=true;
struct G2LinkSnapshot { bool left=false,right=false; };
struct Temple { bool connected=false; };
static Temple gL,gR;
static std::vector<std::string> calls;
static std::vector<int> reseeks;
static uint32_t bleReapRetiredClients() { return retirementAge; }
static bool isG2ClientInitialized() { return clientRuntime; }
// The setup worker: a recycle needs every client idle, the phone's turn too.
namespace hw1setup { constexpr uint8_t kAll = 15; }
static bool setupIdle=true;
static uint8_t setupIdleMask=0;
static bool bleSetupIdle(uint8_t mask) { setupIdleMask=mask; return setupIdle; }
static G2LinkSnapshot g2LinkSnapshot() { G2LinkSnapshot s; s.left=gL.connected; s.right=gR.connected; return s; }
static bool g2RingIsConnected() { return ringUp; }
static bool phonePipeReady() { return phoneReady; }
// The device owners on the shared stack. A READY phone is an open pipe.
static bool g2RingIsOpen() { return ringOpen; }
static bool g2RingClose(bool) { calls.emplace_back("ring-close"); ringOpen=ringUp=false; return true; }
static bool phonePipeActive() { return phoneReady; }
static bool phoneLinkClose(bool) { calls.emplace_back("phone-close"); phoneReady=false; return true; }
// The stack owner's verified teardown and bring-up (BLE_CentralStack.cpp).
static bool stackDownOk=true;
static bool bleStackDown() { calls.emplace_back("stack-down"); if(!stackDownOk) return false; halUp=false; return true; }
static bool bleStackUp() { calls.emplace_back("stack-up"); halUp=true; return true; }
static bool bleStackLifecycleFaulted() { return false; }
static const char* bleStackLastRefusal() { return "busy"; }
static bool isBleServerInitialized() { return false; }
static uint32_t millis() { return clockMs; }
#define BROADCAST_PRINTF(...) ((void)0)
enum { SYSEVT_BLE_STACK_RECYCLED };
static void systemEventPost(int,const char*,const char*) {}
// Glasses only now: the phone pipe is a device owner of its own (phoneLinkClose).
static bool deinitG2Client() { calls.emplace_back("g2-deinit"); clientRuntime=false; return true; }
static bool g2RingPrepareForStackTeardown(uint32_t) { return true; }
struct BLEDevice { static void setCustomGapHandler(void*) {} };
struct BluetoothHalShutdownResult {
  bool success=true,clientQuarantined=false; unsigned phase=0;
  long hostStatusBefore=0,hostStatusAfter=0,controllerStatusBefore=0,controllerStatusAfter=0;
};
static BluetoothHalShutdownResult bluetoothHalDeinit() { calls.emplace_back("host-deinit"); halUp=false; return {}; }
#define vTaskDelay(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
static void bleStackSetLifecycleFault(bool) {}
static void bleCentralClientsTerminalTeardownAcknowledged() {}
struct BluetoothHalStatus {
  bool up=false,hostUninitialized=true; int controller=0;
  bool stopped() const { return !up; }
  bool ready() const { return up; }
};
static BluetoothHalStatus bluetoothHalStatus() { BluetoothHalStatus s; s.up=halUp; return s; }
static struct { bool bleEnabled=true; } gSettings;
static bool initG2Client() { calls.emplace_back("g2-init"); clientRuntime=halUp=true; return true; }
static void blePeerRequestReseek(BlePeerKind kind) { reseeks.push_back(kind); }
// INSERT_PRODUCTION
static void reset() {
  roleAvailable=true; gBleWedgeStreak=kBleWedgeStreakTrip; gBleClientRetirementPeerMask=0;
  sBleLastRecycleMs=0; retirementAge=0; clockMs+=kBleRecycleMinGapMs+1;
  clientRuntime=halUp=true; ringUp=ringOpen=phoneReady=false; gL={}; gR={}; calls.clear(); reseeks.clear();
  setupIdle=true; setupIdleMask=0; stackDownOk=true;
}
int main() {
  // Glasses off, phone READY, accessory admission wedged: keep the pipe.
  {reset();phoneReady=true;assert(!bleStackRecycleIfWedged());assert(calls.empty()&&reseeks.empty()&&phoneReady);}
  // Baseline unchanged: nothing live, the wedge still recycles the host —
  // through the stack owner now: close the runtime, stack down, stack up, reopen.
  {reset();assert(bleStackRecycleIfWedged());
   assert((calls==std::vector<std::string>{"g2-deinit","stack-down","stack-up","g2-init"})&&reseeks.empty());}
  // An open ring owner (its link already gone) is closed for the recycle and reseeked.
  {reset();ringOpen=true;assert(bleStackRecycleIfWedged());
   assert((calls==std::vector<std::string>{"g2-deinit","ring-close","stack-down","stack-up","g2-init"}));}
  // A refused stack teardown (ring owner busy) aborts without a fault and without a re-init.
  {reset();stackDownOk=false;assert(!bleStackRecycleIfWedged());
   assert((calls==std::vector<std::string>{"g2-deinit","stack-down"}));}
  // With the glasses runtime closed, only the stack is recycled: no g2-init.
  {reset();clientRuntime=false;gBleWedgeStreak=0;gBleClientRetirementPeerMask=uint8_t(1U<<BLE_PEER_R1_RING);
   retirementAge=kBleRetirementGraceMs;assert(bleStackRecycleIfWedged());
   assert((calls==std::vector<std::string>{"stack-down","stack-up"}));
   assert((reseeks==std::vector<int>{BLE_PEER_R1_RING}));}
  // A setup in progress on any client (a phone turn included) blocks it.
  {reset();setupIdle=false;assert(!bleStackRecycleIfWedged()&&calls.empty());
   assert(setupIdleMask==hw1setup::kAll);}
  // A live temple still blocks the wedge recycle, as before.
  {reset();gL.connected=true;assert(!bleStackRecycleIfWedged()&&calls.empty());}
  // A lost native route must still be retired: recycle, then restore the
  // phone that was READY along with the retiring peer.
  {reset();gBleWedgeStreak=0;gBleClientRetirementPeerMask=uint8_t(1U<<BLE_PEER_G2_GLASSES);
   retirementAge=kBleRetirementGraceMs;phoneReady=true;
   assert(bleStackRecycleIfWedged());
   // The READY phone is a device owner: closed for the recycle, reseeked after.
   assert((calls==std::vector<std::string>{"g2-deinit","phone-close","stack-down","stack-up","g2-init"}));
   assert((reseeks==std::vector<int>{BLE_PEER_G2_GLASSES,BLE_PEER_PHONE}));}
  {reset();gBleWedgeStreak=0;gBleClientRetirementPeerMask=uint8_t(1U<<BLE_PEER_R1_RING);
   retirementAge=kBleRetirementGraceMs;assert(bleStackRecycleIfWedged());
   assert((reseeks==std::vector<int>{BLE_PEER_R1_RING}));}  // no phone reseek when it was down
  puts("BLE recycle guard: READY phone blocks a wedge recycle with glasses off; forced retirement restores the phone");
}
