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
#define BLE_DEBUGF(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
using TransportSessionEpoch=uint32_t;
using TickType_t=int;
using esp_err_t=int;
using esp_gatt_if_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_GATT_IF_NONE=-1;
constexpr uint8_t MSG_ROUTE_ALL=63,MSG_ROUTE_BLE=8;
static bool debugEnabled=true,live=true,secure=false;
static std::vector<std::string> traces,history;
static std::string delivered;
static uint16_t deliveredConn=0;
static uint32_t deliveredEpoch=0;
static std::atomic<int> sBleGattsIf{1};
struct State { int responsesSent=0; } state;
static State* gBLEState=&state;
struct Characteristic {
  std::string bytes;
  int getHandle() const {return 5;}
  void setValue(uint8_t* data,size_t len){bytes.assign(reinterpret_cast<char*>(data),len);}
  void notify(){delivered=bytes;}
} characteristic;
static Characteristic* pCmdResponseChar=&characteristic;
enum class BleNotifyResult{PENDING,OK,TERMINAL};
static BleNotifyResult gLastNotifyResult=BleNotifyResult::OK;
struct BleLifecycleGuard { explicit operator bool()const{return true;} };
struct BleCommandReplyContext { uint16_t connId;TransportSessionEpoch sessionEpoch; };
bool bleDataDebugEnabled(){return debugEnabled;}
bool bleSessionEpochMatches(uint16_t conn,uint32_t epoch){return live&&conn==0&&epoch==7;}
bool bleConnectionEpochMatchesLocked(uint16_t conn,uint32_t epoch){return bleSessionEpochMatches(conn,epoch);}
bool isBLEConnected(){return live;}
bool bleScEstablished(uint16_t){return secure;}
bool bleScRequired(){return false;}
bool bleScSendEncrypted(uint16_t conn,const char* data,size_t len,bool){
  deliveredConn=conn;deliveredEpoch=7;delivered.assign(data,len);return true;
}
void vTaskDelay(int){}
void broadcastOutputCore_Routed(const char* data,size_t len,uint8_t){traces.emplace_back(data,len);}
void bleAddMessageToHistory(const char* data){history.emplace_back(data);}
esp_err_t esp_ble_gatts_send_indicate(int,uint16_t conn,int,size_t len,uint8_t* data,bool){
  deliveredConn=conn;deliveredEpoch=7;delivered.assign(reinterpret_cast<char*>(data),len);return ESP_OK;
}
bool sendBLEResponseToSession(uint16_t,uint32_t,const char*,size_t,bool=true);
// INSERT_CLASSIFY
// INSERT_TRACE
// INSERT_HISTORY
// INSERT_RAW_SESSION
// INSERT_RAW_BROADCAST
// INSERT_SEND
// INSERT_CALLBACK
static void reset(){traces.clear();history.clear();delivered.clear();live=true;secure=false;debugEnabled=true;}
static void callback(const std::string& body){
 auto* reply=static_cast<BleCommandReplyContext*>(malloc(sizeof(BleCommandReplyContext)));
 *reply={0,7};bleCommandResultCallback(true,body.c_str(),reply);
}
static void privateMirrors(){
 for(const auto& line:traces) assert(line.find("SECRET_SENTENCE")==std::string::npos);
 for(const auto& line:history) assert(line.find("SECRET_SENTENCE")==std::string::npos);
}
int main(){
 const std::string secret="{\"sttText\":\"SECRET_SENTENCE "+std::string(430,'x')+"\"}";
 reset();callback(secret);
 assert(delivered==secret&&deliveredConn==0&&deliveredEpoch==7);
 privateMirrors();assert(history.size()==1&&history[0]=="TX:[private STT result]");
 assert(traces.size()==3&&traces[1].find("[private STT result]")!=std::string::npos);
 reset();secure=true;callback(secret);assert(delivered==secret);privateMirrors();
 reset();debugEnabled=false;callback(secret);assert(delivered==secret&&traces.empty());privateMirrors();
 reset();live=false;callback(secret);assert(delivered.empty()&&traces.empty()&&history.empty());
 reset();callback("ordinary reply");assert(delivered=="ordinary reply"&&history[0]=="TX:ordinary reply");
 assert(traces[1].find("ordinary reply")!=std::string::npos);
 reset();bleRawNotify(secret.data(),secret.size());assert(delivered==secret);privateMirrors();
 assert(history[0]=="TX:[private STT result]");
 // Encrypted binary buffers need not contain a NUL or even a full marker.
 const char shortBinary[2]={'x','y'};
 reset();bleRawNotify(shortBinary,sizeof(shortBinary));assert(delivered=="xy"&&history[0]=="TX:xy");
 assert(!bleOutputHasPrivateSTT(shortBinary,sizeof(shortBinary)));
 const char truncatedMarker[]={'"','s','t','t','T','e','x','t'};
 assert(!bleOutputHasPrivateSTT(truncatedMarker,sizeof(truncatedMarker)));
 const std::string late=std::string(195,'p')+secret;
 reset();callback(late);assert(delivered==late);privateMirrors();
 assert(traces.size()==3&&history[0]=="TX:[private STT result]");
 puts("BLE STT privacy: real async callback, exact-session/plain/secure delivery, bounded binary previews, trace/history suppression passed");
}
