#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define ENABLE_OLED_DISPLAY 1
#define BLE_MSG_MAX_LEN 32
#define BLE_MAX_CONNECTIONS 4
#define BLE_DEBUGF(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
using TransportSessionEpoch=uint32_t;
using TickType_t=int;
using esp_err_t=int;
using esp_gatt_if_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_GATT_IF_NONE=-1;
constexpr uint8_t MSG_ROUTE_ALL=63,MSG_ROUTE_BLE=8;
static bool debugEnabled=true,scRequired=false;
static std::vector<std::string> traces,history;
static std::string delivered;
static uint16_t deliveredConn=0;
static uint32_t deliveredEpoch=0;
static std::atomic<int> sBleGattsIf{1};
// Four-slot GATT peer table.  A directed send lands in the addressed slot's
// inbox; the characteristic-wide notify lands in every connected slot's inbox,
// as the vendored Arduino wrapper sends it to every peer behind one shared CCCD.
struct BLEConnection { bool active=false; uint16_t connId=0; bool authed=false; std::string user; bool mirrorEligible=false; };
struct State { int responsesSent=0; BLEConnection connections[BLE_MAX_CONNECTIONS]; } state;
static State* gBLEState=&state;
static std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];
static bool slotSecure[BLE_MAX_CONNECTIONS],slotCongested[BLE_MAX_CONNECTIONS];
static std::vector<std::string> inbox[BLE_MAX_CONNECTIONS];
static int broadcastNotifies=0;
static int slotFor(uint16_t conn){
  for(int i=0;i<BLE_MAX_CONNECTIONS;++i) if(state.connections[i].active&&state.connections[i].connId==conn) return i;
  return -1;
}
static void land(int slot,const std::string& bytes,const std::string& plain){
  inbox[slot].push_back(bytes);delivered=plain;
  deliveredConn=state.connections[slot].connId;deliveredEpoch=sBleSessionEpoch[slot].load();
}
struct Characteristic {
  std::string bytes;
  int getHandle() const {return 5;}
  void setValue(uint8_t* data,size_t len){bytes.assign(reinterpret_cast<char*>(data),len);}
  void notify(){
    ++broadcastNotifies;delivered=bytes;
    for(int i=0;i<BLE_MAX_CONNECTIONS;++i) if(state.connections[i].active) inbox[i].push_back(bytes);
  }
} characteristic;
static Characteristic* pCmdResponseChar=&characteristic;
enum class BleNotifyResult{PENDING,OK,TERMINAL};
[[maybe_unused]] static BleNotifyResult gLastNotifyResult=BleNotifyResult::OK;
static int guardDepth=0; static unsigned bindCalls=0, fenceCalls=0; static uint16_t lastBindConn=0xffff;
struct BleLifecycleGuard { BleLifecycleGuard(){++guardDepth;} ~BleLifecycleGuard(){--guardDepth;} explicit operator bool()const{return true;} };
struct BleCommandReplyContext { uint16_t connId;TransportSessionEpoch sessionEpoch; };
bool bleDataDebugEnabled(){return debugEnabled;}
bool bleConnectionEpochMatchesLocked(uint16_t conn,uint32_t epoch){
  const int slot=slotFor(conn);return epoch!=0&&slot>=0&&sBleSessionEpoch[slot].load()==epoch;
}
bool bleSessionEpochMatches(uint16_t conn,uint32_t epoch){return bleConnectionEpochMatchesLocked(conn,epoch);}
bool isBLEConnected(){for(const auto& c:state.connections) if(c.active) return true; return false;}
bool bleScEstablished(uint16_t conn){const int slot=slotFor(conn);return slot>=0&&slotSecure[slot];}
bool bleScRequired(){return scRequired;}
bool bleScSendEncrypted(uint16_t conn,const char* data,size_t len,bool){
  const int slot=slotFor(conn);
  if(slot<0||slotCongested[slot]) return false;
  land(slot,"enc:"+std::string(data,len),std::string(data,len));return true;
}
struct BleScBinding { uint16_t connId=0; TransportSessionEpoch epoch=0; uint32_t generation=0; };
// Both binding shapes: pre-fix sources bind the slot generation only and fence
// with a callback; fixed sources bind the session epoch too, and that epoch is
// the per-fragment fence.
bool bleScBind(uint16_t conn,BleScBinding* out){
  assert(guardDepth==1&&"the binding is resolved under the lifecycle mutex");
  ++bindCalls; lastBindConn=conn; if(out){out->connId=conn;out->generation=42u+conn;} return true;
}
bool bleScBind(uint16_t conn,TransportSessionEpoch epoch,BleScBinding* out){
  assert(guardDepth==1&&"the binding is resolved under the lifecycle mutex");
  assert(bleConnectionEpochMatchesLocked(conn,epoch)&&"the binding carries the live session epoch");
  ++bindCalls; lastBindConn=conn; if(out){out->connId=conn;out->epoch=epoch;out->generation=42u+conn;} return true;
}
bool bleScSendEncryptedBound(const BleScBinding& b,const char* data,size_t len,bool,bool(*fence)(void*),void* ctx,bool=false){
  assert(guardDepth==0&&"the paced secure sender runs with the lifecycle mutex released");
  assert(b.generation==42u+b.connId);
  if(fence){++fenceCalls;assert(fence(ctx));}
  const int slot=slotFor(b.connId);
  if(slot<0||slotCongested[slot]) return false;
  land(slot,"enc:"+std::string(data,len),std::string(data,len));return true;
}
bool bleScSendEncryptedBound(const BleScBinding& b,const char* data,size_t len,bool,bool=false){
  assert(guardDepth==0&&"the paced secure sender runs with the lifecycle mutex released");
  assert(b.generation==42u+b.connId);
  if(b.epoch!=0){++fenceCalls;assert(bleSessionEpochMatches(b.connId,b.epoch));}
  const int slot=slotFor(b.connId);
  if(slot<0||slotCongested[slot]) return false;
  land(slot,"enc:"+std::string(data,len),std::string(data,len));return true;
}
struct BleSessionFence { uint16_t connId; TransportSessionEpoch epoch; };
// INSERT_FENCE
void vTaskDelay(int){}
void broadcastOutputCore_Routed(const char* data,size_t len,uint8_t){traces.emplace_back(data,len);}
void bleAddMessageToHistory(const char* data){history.emplace_back(data);}
esp_err_t esp_ble_gatts_send_indicate(int,uint16_t conn,int,size_t len,uint8_t* data,bool){
  assert(guardDepth==1&&"directed admission runs under exactly one lifecycle guard");
  const int slot=slotFor(conn);
  if(slot<0||slotCongested[slot]) return ESP_FAIL;
  const std::string bytes(reinterpret_cast<char*>(data),len);
  land(slot,bytes,bytes);return ESP_OK;
}
bool sendBLEResponseToSession(uint16_t,uint32_t,const char*,size_t,bool=true);
// INSERT_CLASSIFY
// INSERT_TRACE
// INSERT_HISTORY
// INSERT_RAW_SESSION
// INSERT_RAW_BROADCAST
// INSERT_SEND
// INSERT_CALLBACK
// INSERT_MIRROR_COLLECT
// INSERT_MIRROR_SEND
// Slot 0 = {connected, connId 0, epoch 7, not logged in}; every other slot empty.
static void reset(){
  traces.clear();history.clear();delivered.clear();deliveredConn=0;deliveredEpoch=0;
  debugEnabled=true;scRequired=false;broadcastNotifies=0;
  for(int i=0;i<BLE_MAX_CONNECTIONS;++i){
    state.connections[i]=BLEConnection{};sBleSessionEpoch[i].store(0);
    slotSecure[i]=slotCongested[i]=false;inbox[i].clear();
  }
  state.connections[0].active=true;sBleSessionEpoch[0].store(7);
}
static void callback(const std::string& body){
 auto* reply=static_cast<BleCommandReplyContext*>(malloc(sizeof(BleCommandReplyContext)));
 *reply={0,7};bleCommandResultCallback(true,body.c_str(),reply);
}
static void privateMirrors(){
 for(const auto& line:traces) assert(line.find("SECRET_SENTENCE")==std::string::npos);
 for(const auto& line:history) assert(line.find("SECRET_SENTENCE")==std::string::npos);
}
// Console-mirror cases report every failed expectation and exit non-zero at
// the end, so a pre-fix source shows each defect rather than only the first.
static int mirrorFailures=0;
static const char* mirrorCase="";
#define EXPECT(cond,msg) do{ if(!(cond)){ \
  fprintf(stderr,"FAIL %s: %s  [%s]\n",mirrorCase,msg,#cond);++mirrorFailures;} }while(0)
// A connected slot; user==nullptr means it never logged in.
static void peer(int slot,uint16_t conn,uint32_t epoch,const char* user,bool eligible,bool secure=false){
  BLEConnection& c=state.connections[slot];
  c.active=true;c.connId=conn;c.authed=user!=nullptr;c.user=user?user:"";c.mirrorEligible=eligible;
  sBleSessionEpoch[slot].store(epoch);slotSecure[slot]=secure;
}
static void mirrorReset(const char* name){reset();bindCalls=fenceCalls=0;lastBindConn=0xffff;mirrorCase=name;}
static bool onlyInbox(int slot,const std::vector<std::string>& expected){
  for(int i=0;i<BLE_MAX_CONNECTIONS;++i){
    if(i==slot){ if(inbox[i]!=expected) return false; }
    else if(!inbox[i].empty()) return false;
  }
  return true;
}
static void mirrorCases(){
 const char line[]="line\n";const size_t n=sizeof(line)-1;
 // M1: plaintext pair.  Slot 0 connected without a login, slot 1 logged in.
 mirrorReset("M1 plaintext pair");peer(1,1,9,"alice",true);
 EXPECT(sendBLEResponse(line,n,false),"the logged-in session did not receive the mirror");
 EXPECT(inbox[0].empty(),"console mirror reached a peer that never logged in");
 EXPECT(broadcastNotifies==0,"console mirror used the characteristic-wide notify");
 EXPECT(inbox[1]==std::vector<std::string>{"line\n"},"logged-in session missing its directed copy");
 EXPECT(guardDepth==0,"lifecycle guard leaked");
 // M2: both slots hold a Secure Channel; only slot 1 logged in.
 mirrorReset("M2 secure pair");peer(0,0,7,nullptr,false,true);peer(1,1,9,"alice",true,true);
 EXPECT(sendBLEResponse(line,n,false),"the logged-in secure session did not receive the mirror");
 EXPECT(inbox[0].empty(),"console mirror was encrypted for a channel holder that never logged in");
 EXPECT(inbox[1]==std::vector<std::string>{"enc:line\n"},"logged-in secure session missing its encrypted copy");
 EXPECT(bindCalls==1&&lastBindConn==1,"secure binding not resolved for the logged-in session");
 EXPECT(fenceCalls>=1,"paced secure send ran without the session fence");
 // M3: only a guest is logged in (eligibility latched false at login).
 mirrorReset("M3 guest");peer(1,1,9,"gus",false);
 EXPECT(!sendBLEResponse(line,n,false),"mirror reported delivery with only a guest logged in");
 EXPECT(onlyInbox(-1,{}),"console mirror reached a guest session or an unauthenticated peer");
 // M4: one recipient congested; a chunk is offered once and never resent.
 mirrorReset("M4 congested recipient");peer(1,1,9,"alice",true);peer(2,2,11,"bob",true);slotCongested[2]=true;
 EXPECT(sendBLEResponse("a\n",2,false),"a flush that reached one recipient must count as delivered");
 EXPECT(inbox[1]==std::vector<std::string>{"a\n"}&&inbox[2].empty(),"congested flush landed wrongly");
 slotCongested[2]=false;
 EXPECT(sendBLEResponse("b\n",2,false),"uncongested flush not delivered");
 EXPECT((inbox[1]==std::vector<std::string>{"a\n","b\n"}),"a recipient received a chunk twice or missed one");
 EXPECT(inbox[2]==std::vector<std::string>{"b\n"},"a recipient was resent a chunk it missed or lost the next one");
 slotCongested[1]=slotCongested[2]=true;
 EXPECT(!sendBLEResponse("c\n",2,false),"a flush nobody accepted must report failure so the drain keeps it");
 EXPECT(inbox[1].size()==2&&inbox[2].size()==1,"a congested recipient received data");
 EXPECT(inbox[0].empty(),"console mirror reached a peer that never logged in");
 // M5: secure channel required, the logged-in session has none.
 mirrorReset("M5 secure required");scRequired=true;peer(1,1,9,"alice",true);
 EXPECT(!sendBLEResponse(line,n,false),"plaintext emitted although the secure channel is required");
 EXPECT(onlyInbox(-1,{}),"plaintext reached a peer in secure-required mode");
 // M6: mixed links; each eligible session gets its own copy over its own link.
 mirrorReset("M6 mixed links");peer(1,1,9,"alice",true);peer(2,2,11,"root",true,true);
 EXPECT(sendBLEResponse(line,n,false),"mixed recipients not delivered");
 EXPECT(inbox[1]==std::vector<std::string>{"line\n"},"plaintext recipient missing its copy");
 EXPECT(inbox[2]==std::vector<std::string>{"enc:line\n"},"secure recipient missing its encrypted copy");
 EXPECT(inbox[0].empty()&&inbox[3].empty()&&broadcastNotifies==0,"mirror reached a non-recipient");
 // M7: blocking send (the blesend path) follows the same recipient rule.
 mirrorReset("M7 blesend");peer(1,1,9,"alice",true);
 EXPECT(sendBLEResponse(line,n,true),"blesend not delivered to the logged-in session");
 EXPECT(onlyInbox(1,{"line\n"}),"blesend reached a peer that never logged in");
 // M8: a stale eligibility bit after logout, and a disconnected slot, admit nobody.
 mirrorReset("M8 logged out / disconnected");peer(1,1,9,nullptr,true);
 peer(2,2,11,"alice",true);state.connections[2].active=false;
 peer(3,3,0,"bob",true);
 EXPECT(!sendBLEResponse(line,n,false),"mirror reported delivery with no live logged-in session");
 EXPECT(onlyInbox(-1,{}),"mirror reached a logged-out, disconnected or unpublished session");
 // No eligible session at all.
 mirrorReset("M9 nobody connected");state.connections[0].active=false;
 EXPECT(!sendBLEResponse(line,n,false),"mirror reported delivery with nobody connected");
}
int main(){
 const std::string secret="{\"sttText\":\"SECRET_SENTENCE "+std::string(430,'x')+"\"}";
 reset();callback(secret);
 assert(delivered==secret&&deliveredConn==0&&deliveredEpoch==7);
 privateMirrors();assert(history.size()==1&&history[0]=="TX:[private STT result]");
 assert(traces.size()==3&&traces[1].find("[private STT result]")!=std::string::npos);
 reset();slotSecure[0]=true;callback(secret);assert(delivered==secret);privateMirrors();
 assert(bindCalls==1&&fenceCalls==1&&guardDepth==0);
 reset();debugEnabled=false;callback(secret);assert(delivered==secret&&traces.empty());privateMirrors();
 reset();state.connections[0].active=false;callback(secret);assert(delivered.empty()&&traces.empty()&&history.empty());
 reset();callback("ordinary reply");assert(delivered=="ordinary reply"&&history[0]=="TX:ordinary reply");
 assert(traces[1].find("ordinary reply")!=std::string::npos);
 // A second connected peer never sees another session's command result.
 reset();state.connections[1].active=true;state.connections[1].connId=1;sBleSessionEpoch[1].store(9);
 callback("ordinary reply");assert(inbox[0].size()==1&&inbox[1].empty()&&broadcastNotifies==0);
 reset();bleRawNotifyToSession(0,7,secret.data(),secret.size(),true);assert(delivered==secret);privateMirrors();
 assert(history[0]=="TX:[private STT result]");
 const std::string privatePath="/stt/u123/PRIVATE_TRANSCRIPT.txt";
 const std::string status="{\"transcriptPath\":\""+privatePath+"\"}";
 for (bool encrypted : {false,true}) {
  reset();slotSecure[0]=encrypted;callback(status);assert(delivered==status);
  for(const auto& line:traces)assert(line.find(privatePath)==std::string::npos);
  for(const auto& line:history)assert(line.find(privatePath)==std::string::npos);
  if(!encrypted)assert(history.size()==1&&history[0]=="TX:[private STT result]");
 }
 const std::string latePath=std::string(195,'p')+status;
 reset();callback(latePath);assert(delivered==latePath&&history[0]=="TX:[private STT result]");
 const char shortPathMarker[]={'"','t','r','a','n','s','c','r','i','p','t','P','a','t','h'};
 assert(!bleOutputHasPrivateSTT(shortPathMarker,sizeof(shortPathMarker)));
 // Encrypted binary buffers need not contain a NUL or even a full marker.
 const char shortBinary[2]={'x','y'};
 reset();bleRawNotifyToSession(0,7,shortBinary,sizeof(shortBinary),true);assert(delivered=="xy"&&history[0]=="TX:xy");
 assert(!bleOutputHasPrivateSTT(shortBinary,sizeof(shortBinary)));
 const char truncatedMarker[]={'"','s','t','t','T','e','x','t'};
 assert(!bleOutputHasPrivateSTT(truncatedMarker,sizeof(truncatedMarker)));
 const std::string late=std::string(195,'p')+secret;
 reset();callback(late);assert(delivered==late);privateMirrors();
 assert(traces.size()==3&&history[0]=="TX:[private STT result]");
 puts("BLE STT privacy: real async callback, exact-session/plain/secure delivery, bounded binary previews, trace/history suppression passed");
 mirrorCases();
 if(mirrorFailures){
  fprintf(stderr,"BLE console mirror: %d expectation(s) failed\n",mirrorFailures);
  return 1;
 }
 puts("BLE console mirror: only logged-in non-guest sessions receive it, each by directed or own-channel delivery; a congested recipient misses one chunk without any resend; secure-required refuses plaintext; no characteristic-wide notify");
}
