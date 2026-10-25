#!/usr/bin/env python3
"""Exercise the patched SDK allocator and complete DISCONNECT case."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');args=p.parse_args()
device=(ROOT/'components/arduino/libraries/BLE/src/BLEDevice.cpp').read_text()
client=(ROOT/'components/arduino/libraries/BLE/src/BLEClient.cpp').read_text()
table=device[device.index('constexpr size_t kReservedAppIdSlots'):device.index('bool BLEDevice::allocateAppId(')].rsplit('}',1)[0]
functions=table+'\n'+'\n'.join(extract_block(device,k) for k in ['bool BLEDevice::allocateAppId(', 'void BLEDevice::releaseAppId('])
case=extract_block(client,'case ESP_GATTC_DISCONNECT_EVT:')
unit=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <cstdio>
struct BLEClientRegistryGuard{~BLEClientRegistryGuard(){}};
struct BLEDevice {inline static uint16_t m_appId=0;inline static std::map<uint16_t,int> m_connectedClientsMap;static bool allocateAppId(uint16_t&);static void releaseAppId(uint16_t);};
// ALLOCATOR
constexpr int ESP_GATTC_DISCONNECT_EVT=1,ESP_GATT_IF_NONE=255,ESP_BD_ADDR_LEN=6;
struct Event{struct {uint16_t conn_id;uint8_t remote_bda[6];}disconnect;};
struct BLESecurity{inline static int resets=0;static void resetSecurity(){++resets;}};
struct Semaphore{int gives=0;void give(int=0){++gives;}};
struct Client {
  struct Address{uint8_t bytes[6]={1,2,3,4,5,6};uint8_t* getNative(){return bytes;}} m_peerAddress;
  struct Callback{int calls=0;void onDisconnect(Client*){++calls;}} callback,*m_pClientCallbacks=&callback;
  bool m_isConnected=true;int m_conn_id=7,m_mtu=247,unregisters=0,fanout=0;
  Semaphore m_semaphoreOpenEvt,m_semaphoreRssiCmplEvt,m_semaphoreSearchCmplEvt;
  int getConnId(){return m_conn_id;}void requestGattcUnregister(){++unregisters;}
  void dispatch(Event* evtParam){switch(ESP_GATTC_DISCONNECT_EVT){
// DISCONNECT
  }++fanout;}
};
int main(){
  uint16_t reserved;assert(BLEDevice::allocateAppId(reserved));
  BLEDevice::m_connectedClientsMap[9]=1;
  for(unsigned i=0;i<100000;++i){uint16_t id;assert(BLEDevice::allocateAppId(id)&&id<=0x7fff&&id!=reserved&&id!=9);BLEDevice::releaseAppId(id);}
  // The table (memory audit BLE-03): 64 ids at once, the one reserved above
  // among them; a release frees exactly one slot; a full table refuses.
  uint16_t id;unsigned count=0;while(BLEDevice::allocateAppId(id))++count;assert(count==kReservedAppIdSlots-1&&sReservedAppIdCount==kReservedAppIdSlots);
  BLEDevice::releaseAppId(reserved);assert(!appIdReserved(reserved)&&sReservedAppIdCount==kReservedAppIdSlots-1);
  assert(BLEDevice::allocateAppId(id)&&id!=9&&sReservedAppIdCount==kReservedAppIdSlots);assert(!BLEDevice::allocateAppId(id));
  BLEDevice::releaseAppId(0x7fff+1);assert(sReservedAppIdCount==kReservedAppIdSlots);  // out of range: ignored
  for(int foreign=0;foreign<2;++foreign){Client c;Event e{};e.disconnect.conn_id=7;memcpy(e.disconnect.remote_bda,c.m_peerAddress.bytes,6);
    if(foreign==0)e.disconnect.conn_id=8;else e.disconnect.remote_bda[5]^=1;
    c.dispatch(&e);assert(c.m_isConnected&&c.m_mtu==247&&!c.unregisters&&!c.fanout&&!c.callback.calls&&!c.m_semaphoreOpenEvt.gives&&!BLESecurity::resets);
  }
  Client c;Event e{};e.disconnect.conn_id=7;memcpy(e.disconnect.remote_bda,c.m_peerAddress.bytes,6);c.dispatch(&e);
  assert(!c.m_isConnected&&c.m_conn_id==ESP_GATT_IF_NONE&&c.m_mtu==23&&c.unregisters==1&&c.fanout==1&&c.callback.calls==1&&BLESecurity::resets==1);
  puts("Phone SDK lifetime: 100000 registrations across wrap, the 64-slot reservation table and its exhaustion, retained client IDs, foreign/own DISCONNECT passed");
}
'''.replace('// ALLOCATOR',functions).replace('// DISCONNECT',case)
with tempfile.TemporaryDirectory(prefix='hw1-phone-sdk-') as tmp:
    d=Path(tmp);(d/'test.cpp').write_text(unit)
    cmd=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',str(d/'test.cpp'),'-o',str(d/'test')]
    if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
    subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True)
