#pragma once
// Synthetic ESP-IDF GATT client and FreeRTOS queue for running the production
// phone adapter (BLE_PhoneGatt.cpp) off target. Shared by the adapter harness
// and the fixture generator so both drive the same callback sequence.
#include "BLE_PhoneGatt.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
using hw1phone::Gatt;
using Phase=Gatt::Phase;
using Bytes=std::vector<uint8_t>;
static std::map<QueueHandle_t,std::deque<Bytes>> queues;
QueueHandle_t xQueueCreateStatic(unsigned n,unsigned size,uint8_t*,StaticQueue_t* q) {
  q->capacity=n;q->size=size;queues[q].clear();return q;
}
int xQueueReset(QueueHandle_t q) {queues[q].clear();return 1;}
int xQueueSend(QueueHandle_t q,const void* p,TickType_t) {
  if(queues[q].size()>=q->capacity)return 0;
  const auto* b=static_cast<const uint8_t*>(p);queues[q].emplace_back(b,b+q->size);return 1;
}
int xQueueReceive(QueueHandle_t q,void* p,TickType_t) {
  if(queues[q].empty())return 0;
  std::memcpy(p,queues[q].front().data(),q->size);queues[q].pop_front();return 1;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q) {return unsigned(queues[q].size());}
static bool held=false, gate=true;
static std::vector<std::string> calls;
static std::vector<Bytes> writes;
static int charCount=1;static unsigned properties=0xff;
static std::string failApi;
bool bleCentralTxTryPhone(bool bypassPriority) {assert(bypassPriority);assert(!held);if(!gate)return false;held=true;return true;}
void bleCentralTxGive() {assert(held);held=false;}
static int api(const char* name) {calls.emplace_back(name);return failApi==name?ESP_FAIL:ESP_OK;}
int esp_ble_gattc_app_register(uint16_t) {return api("register");}
int esp_ble_gattc_app_unregister(esp_gatt_if_t) {return api("unregister");}
int esp_ble_gattc_open(esp_gatt_if_t,uint8_t*,esp_ble_addr_type_t,bool direct) {assert(direct);return api("open");}
int esp_ble_gattc_close(esp_gatt_if_t,uint16_t) {return api("close");}
int esp_ble_gattc_cancel_open(const esp_ble_gattc_cancel_open_params_t*) {return api("cancel");}
int esp_ble_gattc_cache_refresh(uint8_t*) {return api("refresh");}
int esp_ble_gattc_send_mtu_req(esp_gatt_if_t,uint16_t) {return api("mtu");}
int esp_ble_gattc_search_service(esp_gatt_if_t,uint16_t,esp_bt_uuid_t*) {return api("search");}
int esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,esp_bt_uuid_t uuid,
                                 esp_gattc_char_elem_t* out,uint16_t* count) {
  *count=uint16_t(charCount);out[0].char_handle=uuid.uuid.uuid128[12]==1?11:13;
  out[0].properties=uint8_t(properties);return ESP_GATT_OK;
}
int esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t,uint16_t,uint16_t,esp_bt_uuid_t,
                                         esp_gattc_descr_elem_t* out,uint16_t* count) {
  *count=1;out[0].handle=14;return ESP_GATT_OK;
}
int esp_ble_gattc_register_for_notify(esp_gatt_if_t,uint8_t*,uint16_t) {return api("subscribe");}
int esp_ble_gattc_write_char_descr(esp_gatt_if_t,uint16_t,uint16_t,uint16_t n,uint8_t* p,int type,int auth) {
  assert(held && n==2 && p[0]==1 && p[1]==0 && type==ESP_GATT_WRITE_TYPE_RSP && auth==ESP_GATT_AUTH_REQ_NONE);
  return api("cccd");
}
int esp_ble_gattc_write_char(esp_gatt_if_t,uint16_t,uint16_t,uint16_t n,uint8_t* p,int type,int auth) {
  assert(held && type==ESP_GATT_WRITE_TYPE_NO_RSP && auth==ESP_GATT_AUTH_REQ_NONE);
  writes.emplace_back(p,p+n);return api("write");
}
// Pipe HELLO nonce source. Defaults to a fixed nonzero fill.
static void (*fakeFillRandom)(void*,size_t)=nullptr;
void esp_fill_random(void* p,size_t n) {if(fakeFillRandom)fakeFillRandom(p,n);else std::memset(p,0x51,n);}
static const uint8_t address[6]={0xc2,0,0,0,0,1};
static uint32_t now=100;
static void tick(Gatt& g,unsigned ms=1,bool pressure=false) {now+=ms;g.poll(now,pressure);assert(!held);}
static void post(Gatt& g,esp_gattc_cb_event_t type,const esp_ble_gattc_cb_param_t& e) {assert(g.post(type,7,&e));}
static void beginWith(Gatt& g,const uint8_t key[32]) {
  assert(g.begin(42,address,1,now,key));assert(!g.begin(42,address,1,now,key));
  esp_ble_gattc_cb_param_t e{};e.reg.app_id=41;assert(!g.post(ESP_GATTC_REG_EVT,7,&e));
  e.reg.app_id=42;post(g,ESP_GATTC_REG_EVT,e);tick(g);assert(g.phase()==Phase::Opening);
}
static void opened(Gatt& g) {
  esp_ble_gattc_cb_param_t e{};e.open.conn_id=4;std::memcpy(e.open.remote_bda,address,6);
  post(g,ESP_GATTC_OPEN_EVT,e);tick(g);assert(g.phase()==Phase::Mtu);
  size_t count=calls.size();tick(g,100);assert(calls.size()==count); // exactly one MTU request
  e.connect.remote_bda[0]=0xee;assert(!g.post(ESP_GATTC_CONNECT_EVT,ESP_GATT_IF_NONE,&e));
  std::memcpy(e.connect.remote_bda,address,6);assert(g.post(ESP_GATTC_CONNECT_EVT,ESP_GATT_IF_NONE,&e));
}
static void discover(Gatt& g,uint16_t mtu=247) {
  esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=mtu;
  post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);assert(g.phase()==Phase::Discovering);
  e.search_res.conn_id=4;e.search_res.start_handle=10;e.search_res.end_handle=20;e.search_res.is_primary=true;
  e.search_res.srvc_id.uuid.len=16;
  const uint8_t uuid[16]={0xa0,0xc6,0x21,0x50,0x9f,0x3d,0xc4,0xa8,0x2e,0x4b,0x7d,0x6f,0,0,0xe1,0xb7};
  std::memcpy(e.search_res.srvc_id.uuid.uuid.uuid128,uuid,16);post(g,ESP_GATTC_SEARCH_RES_EVT,e);
  e.search_cmpl.conn_id=4;post(g,ESP_GATTC_SEARCH_CMPL_EVT,e);tick(g);
}
[[maybe_unused]] static void subscribed(Gatt& g) {
  assert(g.phase()==Phase::Subscribe);
  esp_ble_gattc_cb_param_t e{};e.reg_for_notify.handle=13;post(g,ESP_GATTC_REG_FOR_NOTIFY_EVT,e);
  gate=false;size_t n=calls.size();tick(g,30);assert(g.phase()==Phase::Cccd && calls.size()==n);
  gate=true;tick(g,30);assert(calls.back()=="cccd");
  n=calls.size();tick(g,30);assert(calls.size()==n); // exactly one outstanding ATT write
  e.write.conn_id=4;e.write.handle=14;post(g,ESP_GATTC_WRITE_DESCR_EVT,e);tick(g,30);
  assert(g.phase()==Phase::Secure && !g.ready());
}
static int completionStatus=ESP_GATT_OK;
static void writeDone(Gatt& g,bool pressure=false) {
  esp_ble_gattc_cb_param_t e{};e.write.conn_id=4;e.write.handle=11;e.write.status=completionStatus;
  post(g,ESP_GATTC_WRITE_CHAR_EVT,e);tick(g,30,pressure);
}
static void notify(Gatt& g,Bytes& bytes) {
  esp_ble_gattc_cb_param_t e{};e.notify.conn_id=4;e.notify.handle=13;
  e.notify.value=bytes.data();e.notify.value_len=uint16_t(bytes.size());
  post(g,ESP_GATTC_NOTIFY_EVT,e);
}
