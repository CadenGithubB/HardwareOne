#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
static void wait(const std::atomic<bool>& value){
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!value.load()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
static uint8_t reportedLinks=3;
uint8_t radioHalCompanionBleLinks(){return reportedLinks;}
int main(){
 // A regression that leaves a thread waiting fails here instead of hanging.
 std::thread([]{std::this_thread::sleep_for(std::chrono::seconds(60));
                std::fputs("coordinator harness timed out\n",stderr);std::_Exit(1);}).detach();
 // All four named owners share pending/live capacity. Refusal returns before
 // the caller can create a client or invoke a connect/wedge classifier.
 unsigned creates=0,connects=0;
 auto attempt=[&](BleCentralLink who,const void* client,bool arms=false){
   BleCentralLinkGuard reservation(who,arms);
   if(!reservation)return false;
   ++creates;reservation.retain(client);++connects;return true;
 };
 int left,right,ring,phone;
 // Pipe absent/runtime off: legacy three-link glasses + ring admission works.
 assert(!bleCentralRingShouldYield());
 assert(attempt(BleCentralLink::Left,&left));
 assert(attempt(BleCentralLink::Right,&right));
 assert(attempt(BleCentralLink::Ring,&ring));
 assert(!attempt(BleCentralLink::Phone,&phone,true));
 // Enable while full: do not free or overbook the ring's pending/live slot.
 bleCentralPhoneEnabled(true);assert(bleCentralRingShouldYield());
 assert(!attempt(BleCentralLink::Phone,&phone,true));
 bleCentralLinkReleaseClient(&ring);
 assert(!attempt(BleCentralLink::Ring,&ring));
 assert(attempt(BleCentralLink::Phone,&phone,true));
 bleCentralLinkReleaseClient(&phone);bleCentralPhoneEnabled(false);
 assert(!bleCentralRingShouldYield());assert(attempt(BleCentralLink::Ring,&ring));
 bleCentralLinksHostStopped();creates=connects=0;bleCentralPhoneEnabled(true);

 assert(attempt(BleCentralLink::Left,&left));
 assert(attempt(BleCentralLink::Right,&right));
 assert(attempt(BleCentralLink::Phone,&phone,true));
 assert(!attempt(BleCentralLink::Ring,&ring));
 assert(!attempt(BleCentralLink::Phone,&phone));
 assert(creates==3 && connects==3);
 bleCentralLinkReleaseClient(&left); // terminal client retirement
 assert(!attempt(BleCentralLink::Ring,&ring)); // yields even with spare capacity
 assert(attempt(BleCentralLink::Left,&left));
 reportedLinks=4;assert(attempt(BleCentralLink::Ring,&ring));
 assert(!attempt(BleCentralLink::Ring,&ring));
 bleCentralLinksHostStopped();reportedLinks=2;
 assert(!attempt(BleCentralLink::Phone,&phone,true)); // reserve both arms
 assert(attempt(BleCentralLink::Phone,&phone,false));
 assert(attempt(BleCentralLink::Left,&left));
 const auto before=creates;
 assert(!attempt(BleCentralLink::Right,&right));assert(creates==before && connects==creates);
 bleCentralLinksHostStopped();reportedLinks=4;
 const auto old=bleCentralLinkReserve(BleCentralLink::Phone);
 bleCentralLinkRelease(old);
 const auto replacement=bleCentralLinkReserve(BleCentralLink::Phone);
 assert(replacement && replacement!=old);bleCentralLinkRelease(old);
 assert(!bleCentralLinkReserve(BleCentralLink::Phone));bleCentralLinkRelease(replacement);
 {BleCentralLinkGuard allocationFailure(BleCentralLink::Left);assert(allocationFailure);}
 assert(bleCentralLinkReserve(BleCentralLink::Left));bleCentralLinksHostStopped();
 assert(!bleCentralRecoveryBlocked());
 bleCentralRecoveryBlock(true);assert(bleCentralRecoveryBlocked());
 assert(!attempt(BleCentralLink::Phone,&phone) && !attempt(BleCentralLink::Left,&left));
 bleCentralRecoveryBlock(false);assert(!bleCentralRecoveryBlocked());

 // Concurrent reservations for duplicate owners cannot oversubscribe.
 std::vector<std::thread> contenders;
 std::atomic<unsigned> admitted{0};
 for(unsigned i=0;i<16;++i)contenders.emplace_back([&,i]{
   if(bleCentralLinkReserve(static_cast<BleCentralLink>(i%4)))++admitted;
 });
 for(auto& t:contenders)t.join();
 assert(admitted==4);bleCentralLinksHostStopped();
 // Concurrent first initialization must create exactly one controller gate.
 std::vector<std::thread> init;
 for(int i=0;i<8;++i)init.emplace_back([]{bleCentralTxInit();});
 for(auto& t:init)t.join();
 assert(bleCentralTxTryPhone());
 std::atomic<bool> started{false},held{false},release{false};
 std::thread accessory([&]{started=true;assert(bleCentralTxTake(1000));held=true;wait(release);bleCentralTxGive();});
 wait(started);
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!bleCentralTxAccessoryPressure()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
 bleCentralTxGive();wait(held);fakeTicks+=100;assert(bleCentralTxAccessoryPressure());assert(!bleCentralTxTryPhone());assert(!bleCentralTxTryPhone(true));release=true;accessory.join();
 assert(!bleCentralTxTryPhone());assert(bleCentralTxTryPhone(true));bleCentralTxGive();fakeTicks+=20;assert(bleCentralTxTryPhone());bleCentralTxGive();
 std::puts("production BLE capacity/refusal, recovery block and accessory TX priority passed");
}
