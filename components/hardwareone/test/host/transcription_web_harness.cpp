#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Transcription_UI_Policy.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#define ENABLE_LOCAL_STT 1
#define DICTATION_MAX_TEXT 256
#define PSRAM_JSON_DOC(name) JsonDocument name
using TransportSessionEpoch=uint32_t;
constexpr TransportSessionEpoch kNoTransportSessionEpoch=0;
enum CommandSource {SOURCE_INTERNAL, SOURCE_WEB, SOURCE_LOCAL_DISPLAY, SOURCE_G2_GLASSES};
// INSERT_PRODUCTION_TYPES
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1;
struct httpd_req_t {
  const char* uri="/api/transcription";
  std::string input, query, output, status="200 OK";
  size_t content_len=0, received=0;
  bool failRead=false, failHeader=false;
  std::unordered_map<std::string,std::string> headers;
};
struct AuthContext {String user="alice", sid="cookie"; CommandSource transport=SOURCE_WEB;};
static struct Scenario {
  bool authenticated=true, guestAllowed=true, live=true, stateful=true, named=true;
  bool revokeOnPeek=false, revokeOnSerialize=false, revokeOnBegin=false;
  bool haveLease=true, active=true, text=true, accept=true;
  bool identity=false;
  int calls=0, beginCalls=0, commitCalls=0, stopCalls=0, cancelCalls=0;
  TransportSessionEpoch epoch=42;
  uint32_t account=7;
  uint64_t exchange=0x1122334455667788ULL;
  DictationTextReceipt last{};
} scenario;
static struct {bool sttSaveTranscripts=true;} gSettings;
void webBatchTestStringAppend(){if(scenario.revokeOnSerialize)scenario.live=false;}
static AuthContext makeWebAuthCtx(httpd_req_t*){AuthContext c;if(!scenario.stateful)c.sid="";if(!scenario.named)c.user="";return c;}
static bool tgRequireAuth(const AuthContext&){return scenario.authenticated;}
static bool webGuestAccessAllowed(httpd_req_t*,const AuthContext&){return scenario.guestAllowed;}
#define WEB_AUTH_OR_RETURN(req,ctx) AuthContext ctx=makeWebAuthCtx(req); if(!tgRequireAuth(ctx)||!webGuestAccessAllowed(req,ctx))return ESP_OK
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext&){return scenario.epoch;}
static bool transportSessionEpochIsLive(CommandSource s,TransportSessionEpoch e){return s==SOURCE_WEB&&e==scenario.epoch&&scenario.live;}
struct ExecIdentityGuard {explicit ExecIdentityGuard(const AuthContext& c){assert(c.user=="alice");scenario.identity=true;}~ExecIdentityGuard(){scenario.identity=false;}};
static bool isAdminUser(const String& u){assert(scenario.identity&&u=="alice");return true;}
static bool getUserIdByUsername(const String& u,uint32_t& id){assert(scenario.identity&&u=="alice");id=scenario.account;return true;}
static bool dictationAvailable(const char** why){assert(scenario.identity);*why=nullptr;return true;}
static bool owns(const DictationAppLease& l){return scenario.haveLease&&l.source==SOURCE_WEB&&l.epoch==scenario.epoch&&l.exchange==scenario.exchange;}
static bool dictationAppCurrent(CommandSource source,TransportSessionEpoch epoch,DictationAppLease* lease){assert(scenario.identity);scenario.calls++;*lease=DictationAppLease{};if(!scenario.haveLease)return false;*lease={source,epoch,scenario.exchange};return true;}
static bool dictationAppSnapshot(const DictationAppLease& l,DictationAppSnapshot* out){assert(scenario.identity);scenario.calls++;*out=DictationAppSnapshot{};if(!transportSessionEpochIsLive(l.source,l.epoch))return false;out->busy=scenario.active;if(!owns(l))return false;out->valid=true;out->active=scenario.active;out->done=!scenario.active;out->status.captureActive=scenario.active;out->status.transcript.enabled=true;std::strcpy(out->status.transcript.path,"/stt/u7/private.txt");return true;}
static bool dictationAppPeekText(const DictationAppLease& l,char* out,size_t size,DictationTextReceipt* r){assert(owns(l));scenario.calls++;if(!scenario.text)return false;std::snprintf(out,size,"private \"text\"");*r={l.exchange,9,0,14};if(scenario.revokeOnPeek)scenario.live=false;return true;}
static bool dictationAppBegin(CommandSource source,TransportSessionEpoch epoch,DictationAppLease* lease){assert(scenario.identity);scenario.calls++;scenario.beginCalls++;*lease={source,epoch,scenario.exchange};if(scenario.revokeOnBegin)scenario.live=false;return scenario.accept;}
static bool dictationAppRequestStop(const DictationAppLease& l){scenario.calls++;scenario.stopCalls++;return owns(l)&&scenario.accept;}
static bool dictationAppCancel(const DictationAppLease& l){scenario.calls++;scenario.cancelCalls++;return owns(l)&&scenario.accept;}
static bool dictationAppCommitText(const DictationAppLease& l,const DictationTextReceipt& r,size_t accepted){scenario.calls++;scenario.commitCalls++;scenario.last=r;assert(accepted==r.length);return owns(l)&&scenario.accept;}
static esp_err_t httpd_resp_set_hdr(httpd_req_t* r,const char* k,const char* v){r->headers[k]=v;return r->failHeader?ESP_FAIL:ESP_OK;}
static void httpd_resp_set_status(httpd_req_t* r,const char* s){r->status=s;}
static esp_err_t sendJsonResponse(httpd_req_t* r,const char* data,size_t size){r->output.assign(data,size);return ESP_OK;}
static size_t httpd_req_get_url_query_len(httpd_req_t* r){return r->query.size();}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t* r,char* out,size_t cap){if(r->query.size()>=cap)return ESP_FAIL;std::strcpy(out,r->query.c_str());return ESP_OK;}
static esp_err_t httpd_query_key_value(const char* query,const char* key,char* out,size_t cap){std::string s(query),want=std::string(key)+"=";size_t pos=0;while(pos<s.size()){size_t end=s.find('&',pos);if(end==std::string::npos)end=s.size();if(s.compare(pos,want.size(),want)==0){const auto value=s.substr(pos+want.size(),end-pos-want.size());if(value.size()>=cap)return ESP_FAIL;std::strcpy(out,value.c_str());return ESP_OK;}pos=end+1;}return ESP_FAIL;}
static int httpd_req_recv(httpd_req_t* r,char* out,size_t cap){if(r->failRead)return -1;size_t n=std::min({cap,r->input.size()-r->received,size_t(7)});memcpy(out,r->input.data()+r->received,n);r->received+=n;return int(n);}
static void secureClearString(String& s){s="";}
// INSERT_PRODUCTION_HTTP
static httpd_req_t get(const std::string& query=""){httpd_req_t r;r.query=query;handleTranscriptionGet(&r);assert(!scenario.identity);assert(r.headers["Cache-Control"]=="no-store");return r;}
static httpd_req_t post(const std::string& body,bool ack=false){httpd_req_t r;r.input=body;r.content_len=body.size();if(ack)r.uri="/api/transcription/ack";handleTranscriptionPost(&r);assert(!scenario.identity);assert(r.headers["Cache-Control"]=="no-store");return r;}
static JsonDocument json(const httpd_req_t& r){JsonDocument d;assert(!deserializeJson(d,r.output));return d;}
static void reset(){scenario={};}
int main(){
  reset();auto r=get();auto d=json(r);assert(d["success"]==true);assert(d["id"]=="1122334455667788");assert(d["roots"][0]=="/stt/u7");assert(d["roots"][1]=="/sd/stt/u7");assert(d["sttText"]=="private \"text\"");assert(d["receipt"]["sequence"]==9);assert(d["canSave"]==true);
  reset();r=get("identity=1");d=json(r);assert(d["session"]==42&&scenario.calls==0&&!d.containsKey("sttText"));
  reset();scenario.authenticated=false;r=get();assert(r.output.empty()&&scenario.calls==0);
  reset();scenario.guestAllowed=false;r=post("action=start");assert(r.output.empty()&&scenario.calls==0);
  for(int kind=0;kind<3;kind++){reset();if(kind==0)scenario.stateful=false;if(kind==1)scenario.named=false;if(kind==2)scenario.live=false;r=get();assert(r.status=="401 Unauthorized"&&scenario.calls==0);}
  // Current clears the lease when keyboard/another app owns the broker. Busy
  // still needs the original authenticated web owner, with no private fields.
  reset();scenario.haveLease=false;r=get();d=json(r);assert(d["valid"]==false&&d["busy"]==true&&!d.containsKey("sttText")&&!d.containsKey("transcriptPath"));
  reset();scenario.haveLease=false;scenario.active=false;r=get();d=json(r);assert(d["valid"]==false&&d["busy"]==false&&!d.containsKey("sttText"));
  reset();r=get("id=9999999999999999");assert(r.status=="410 Gone"&&r.output.find("private")==std::string::npos);
  for(const auto& query:std::initializer_list<std::string>{"id=1","id=0000000000000000","id=zz22334455667788","id="+std::string(80,'a')}){reset();r=get(query);assert(r.status=="400 Bad Request");}
  reset();scenario.revokeOnPeek=true;r=get("id=1122334455667788");assert(r.status=="401 Unauthorized"&&r.output.find("private")==std::string::npos);
  reset();scenario.revokeOnSerialize=true;r=get();assert(r.status=="401 Unauthorized"&&r.output.find("private")==std::string::npos);
  reset();r=post("action=start&source=3&epoch=999");assert(r.status=="200 OK"&&scenario.beginCalls==1);
  reset();scenario.revokeOnBegin=true;r=post("action=start");assert(r.status=="401 Unauthorized");
  reset();r=post("action=stop&id=1122334455667788");assert(r.status=="200 OK"&&scenario.stopCalls==1);
  reset();r=post("action=cancel&id=9999999999999999");assert(r.status=="409 Conflict");
  reset();r=post("id=1122334455667788&sequence=9&offset=0&length=14",true);assert(r.status=="200 OK"&&scenario.commitCalls==1&&scenario.last.sequence==9&&scenario.last.length==14);
  for(const auto& extra:{"sequence=4294967296&offset=0&length=14","sequence=0&offset=0&length=14","sequence=9&offset=65536&length=14","sequence=9&offset=0&length=257","sequence=9&offset=0&length=-1","sequence=9&offset=0&length=0"}){reset();r=post(std::string("id=1122334455667788&")+extra,true);assert(r.status=="400 Bad Request"&&scenario.commitCalls==0);}
  reset();r=post(std::string(256,'x'));assert(r.status=="400 Bad Request"&&scenario.calls==0);
  reset();r=post(std::string("action=start\0evil",17));assert(r.status=="400 Bad Request"&&scenario.calls==0);
  reset();httpd_req_t broken;broken.input="action=start";broken.content_len=12;broken.failRead=true;handleTranscriptionPost(&broken);assert(broken.status=="400 Bad Request"&&scenario.calls==0);
  reset();httpd_req_t header;header.failHeader=true;assert(handleTranscriptionGet(&header)==ESP_FAIL&&scenario.calls==0);
  std::cout<<"Transcription HTTP: actual handlers passed auth, epoch, receipt and bounded-input cases\n";
}
