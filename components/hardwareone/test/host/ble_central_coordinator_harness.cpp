#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"
#include <freertos/FreeRTOS.h>
#include <cassert>
#include <cstdio>
#include <vector>
static void wait(const std::atomic<bool>& value){
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!value.load()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
int main(){
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
 bleCentralTxGive();wait(held);fakeTicks+=100;assert(bleCentralTxAccessoryPressure());assert(!bleCentralTxTryPhone());release=true;accessory.join();
 assert(!bleCentralTxTryPhone());fakeTicks+=20;assert(bleCentralTxTryPhone());bleCentralTxGive();
 // New phone setup cannot overtake a waiting accessory setup.
 assert(bleCentralSetupTake(true,0));started=false;held=false;release=false;
 std::thread setup([&]{started=true;assert(bleCentralSetupTake(false,1000));held=true;wait(release);bleCentralSetupGive();});
 wait(started);end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!bleCentralAccessoryPending()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
 // Same-thread recursive acquisition would succeed in the OS; the priority
 // check must reject it before touching the recursive mutex.
 assert(!bleCentralSetupTake(true,0));bleCentralSetupGive();wait(held);
 assert(bleCentralAccessoryBusy());assert(!bleCentralSetupTake(true,0));release=true;setup.join();
 assert(!bleCentralAccessoryBusy());assert(bleCentralSetupTake(true,0));bleCentralSetupGive();
 // Nested accessory helpers retain the established recursive owner contract.
 assert(bleCentralSetupTake(false,UINT32_MAX));assert(bleCentralSetupTake(false,0));
 bleCentralSetupGive();bleCentralSetupGive();
 std::puts("production BLE setup serialization and accessory TX priority passed");
}
