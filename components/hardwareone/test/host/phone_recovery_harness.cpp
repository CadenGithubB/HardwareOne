#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif
#define ENABLE_BLE_PHONE_PIPE 1
enum class BleRoleTransition { RECOVERING };
constexpr int SYSEVT_BLE_STACK_RECYCLED=1;
constexpr int BLE_PEER_G2_GLASSES=0,BLE_PEER_R1_RING=1,BLE_PEER_PHONE=2;
static bool needed=true,roleAvailable=true,glasses=false,ring=false;
static bool quiesceOk=true,setupIdleOk=true,glassesStopOk=true,ringStopOk=true,halOk=true;
static bool terminal=true,ackOk=true,startOk=true,changeIntent=false;
static bool fault=false,blocked=true;
static unsigned hostStops=0,starts=0,acks=0,roleEvents=0;
static uint32_t now=100;
static std::vector<std::string> logs,operations;
static std::vector<int> reseeks;
static struct {bool bleEnabled=true;int bleMode=2;} gSettings;
struct BleRoleTransitionClaim {
 explicit BleRoleTransitionClaim(BleRoleTransition){}
 explicit operator bool()const{return roleAvailable;}
};
static bool phonePipeRecoveryNeeded(){return needed;}
static bool isBleServerInitialized(){return false;}
static uint32_t millis(){return now;}
static bool isG2ClientInitialized(){return glasses;}
static bool g2RingIsOpen(){return ring;}
static bool phonePipeQuiesceForHostRecovery(){operations.push_back("phone-quiet");return quiesceOk;}
// Quiescing ends the phone's turn; recovery waits for only that slot here.
// Glasses and ring setups end inside deinitG2Client, which cancels first.
namespace hw1setup {
enum class Client : uint8_t { G2=0, RingRetire=1, Ring=2, Phone=3 };
constexpr uint8_t clientMask(Client c){return uint8_t(1u<<unsigned(c));}
}
static bool bleSetupWaitIdle(uint8_t mask,uint32_t limit) {
 assert(mask==hw1setup::clientMask(hw1setup::Client::Phone) && limit==2000);
 operations.push_back("phone-setup-idle");return setupIdleOk;
}
static bool deinitG2Client(){operations.push_back("glasses-quiet");return glassesStopOk;}
// The ring closes as a device owner (its reconnect intent kept: userInitiated=false).
static bool g2RingClose(bool userInitiated){assert(!userInitiated);operations.push_back("ring-quiet");return ringStopOk;}
static void bleStackSetLifecycleFault(bool value){fault=value;}
static void logSystemEvent(const char*,const char* message){logs.emplace_back(message);}
static void systemEventPost(int,const char*,const char*){++roleEvents;}
// The stack owner's verified teardown (BLE_CentralStack.cpp): the checked HAL
// stop, the terminal acknowledgements, the phone pipe's host-stopped handshake
// and the recovery block release, in that order. The fake keeps the old
// sub-step records so each case still reads the same.
struct Status {bool stopped()const{return terminal;}bool ready()const{return startOk;}};
static Status bluetoothHalStatus(){return {};}
static bool bleStackDown(){
 assert(!logs.empty() && logs.back().find("last resort")!=std::string::npos);
 ++hostStops;operations.push_back("host-stop");
 if(changeIntent)gSettings.bleEnabled=false;
 if(!halOk || !terminal){fault=true;return false;}
 operations.push_back("clients-release");operations.push_back("capacity-release");
 ++acks;operations.push_back("phone-release");
 if(!ackOk){fault=true;return false;}
 needed=false;blocked=false;return true;
}
// The stack comes back first, then the glasses runtime if it was open.
static bool bleStackUp(){assert(!fault && !blocked);++starts;return startOk;}
static bool initG2Client(){assert(!fault && !blocked);++starts;return startOk;}
static void blePeerRequestReseek(int peer){reseeks.push_back(peer);}
// INSERT_PRODUCTION
int main(int argc,char** argv) {
 assert(argc==2);const int test=std::atoi(argv[1]);
 if(test==1){glasses=ring=true;}
 if(test==2)quiesceOk=false;
 if(test==3){glasses=true;glassesStopOk=false;}
 if(test==4){ring=true;ringStopOk=false;}
 if(test==5)halOk=false;
 if(test==6)terminal=false;
 if(test==7)ackOk=false;
 if(test==8)changeIntent=true;
 if(test==9)startOk=false;
 if(test==10)setupIdleOk=false;
 // No recovery on normal cleanup, or while another role transition owns it.
 needed=false;bleRecoverStuckPhone();assert(operations.empty());
 needed=true;roleAvailable=false;bleRecoverStuckPhone();assert(operations.empty());
 roleAvailable=true;bleRecoverStuckPhone();
 if((test>=2 && test<=4) || test==10){assert(fault && blocked && hostStops==0 && starts==0 && acks==0);}
 else {
   assert(hostStops==1 && roleEvents==1);
   if(test==5 || test==6){assert(fault && blocked && acks==0 && starts==0);}
   else if(test==7){assert(fault && blocked && acks==1 && starts==0);}
   else if(test==8){assert(!fault && !blocked && starts==0 && reseeks.empty());}
   else if(test==9){assert(fault && starts==1 && reseeks.empty());}
   // test 1 restores the stack and then the glasses runtime: two starts.
   else {assert(!fault && !blocked && starts==(test==1?2u:1u));assert(reseeks.size()==(test==1?3u:1u));}
 }
 assert(!logs.empty());
 // The phone's turn is over before anything else is torn down.
 if(test!=2) {
   assert(operations.size()>=2 && operations[0]=="phone-quiet" && operations[1]=="phone-setup-idle");
   if(test==10)assert(operations.size()==2 && logs.back().find("setup turn")!=std::string::npos);
 }
 const auto stops=hostStops;
 // Failed transactions are attempted once, not a restart loop. A new incident
 // after a successful transaction is rate limited for ten minutes.
 for(int i=0;i<10;++i){now+=100;bleRecoverStuckPhone();}
 if(test<2){needed=true;bleRecoverStuckPhone();assert(hostStops==stops);}
 assert(hostStops==stops);
 std::printf("Phone recovery transaction case %d passed\n",test);
}
