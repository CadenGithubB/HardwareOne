#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <ArduinoJson.h>
#include <cstdlib>
#define PSRAM_JSON_DOC(name) JsonDocument name
class TestJsonAllocator : public ArduinoJson::Allocator {
 public: void* allocate(size_t n) override { return malloc(n); }
 void deallocate(void* p) override { free(p); }
 void* reallocate(void* p,size_t n) override { return realloc(p,n); }
};
static ArduinoJson::Allocator* psramJsonAllocator() { static TestJsonAllocator allocator; return &allocator; }
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t n, AllocPref, const char*) { return malloc(n); }
template<typename T> static void ps_delete(T* p) { if (p) { p->~T(); free(p); } }
static uint32_t esp_random() { static uint32_t n = 100; return ++n; }
#include "System_LLMUtf8.h"
#include "G2_NativeTerminalSession.h"
#define EXT_RAM_BSS_ATTR
#define INFO_SYSTEMF(...) ((void)0)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
portMUX_TYPE gRxPacketMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t millis();
// INSERT_PROTOCOL
// INSERT_GLOBALS
uint32_t clockMs = 1;
uint32_t millis() { return clockMs; }
using STTToken = uint64_t;
enum CommandSource { SOURCE_INTERNAL, SOURCE_G2_GLASSES };
struct STTOwner { CommandSource source = SOURCE_INTERNAL; uint32_t epoch = 0; };
struct TranscriptOptions { bool enabled = false; };
enum class STTState { Preparing, Recording, Done, Cancelled, Failed };
struct STTSnapshot { bool workerActive = false; STTState state = STTState::Done; };
struct STTTextChunk { uint32_t sequence = 0; char text[2049] = {}; };
STTState stoppedSpeechState = STTState::Done;
uint32_t liveOwner = 7;
bool sttLive = false, textReady = false, pausedAtTeardown = false;
std::string transcript;
unsigned speechBegins = 0, finishRequests = 0;
struct G2Temple { char side; uint32_t connectionGeneration; bool ready = true, audioNotifyChar = true;
  char firmwareVersion[32] = "2.3.2.14"; } gL{'L', 8}, gR{'R', 9};
bool g2TempleReadyAtGeneration(const G2Temple& t, uint32_t generation = 0) { return t.ready && (!generation || generation == t.connectionGeneration); }
bool transportSessionEpochIsLive(CommandSource, uint32_t epoch) { return epoch && epoch == liveOwner; }
bool sttAnySessionActive() { return sttLive; }
bool sttBeginContinuousG2Native(STTOwner owner, uint32_t gen, STTToken* token, char*, size_t,
                                const TranscriptOptions* options, void (*)()) {
  assert(owner.epoch == liveOwner && gen == gL.connectionGeneration && options && !options->enabled);
  ++speechBegins; sttLive = true; *token = speechBegins; return true;
}
bool sttCancel(STTOwner, STTToken) { sttLive = false; textReady = false; return true; }
bool sttRequestFinish(STTOwner, STTToken) { ++finishRequests; sttLive = false; textReady = !transcript.empty(); return true; }
bool sttSnapshot(STTOwner, STTToken, STTSnapshot* s) { s->workerActive = sttLive; s->state = sttLive ? STTState::Recording : stoppedSpeechState; return true; }
bool sttReadChunk(STTOwner, STTToken, STTTextChunk* c) { if (!textReady) return false; c->sequence = 1; snprintf(c->text, sizeof(c->text), "%s", transcript.c_str()); return true; }
bool sttAcknowledgeChunk(STTOwner, STTToken, uint32_t) { textReady = false; return true; }
bool g2EvenAiSessionIsActive() { return false; }
bool g2ConversateActive() { return false; }
bool quickNoteActive() { return false; }
bool audioCaptureBusy() { return false; }
bool gMicStreamOn = false, gMicProbeActive = false, gMicRecFile = false, gMicWavFile = false;
constexpr int BLE_PEER_G2_GLASSES = 1;
struct BlePeerOwnerSession { uint32_t transportEpoch = 0; std::string user; bool live() { return transportEpoch != 0; } };
bool blePeerOwnerSessionSnapshot(int, BlePeerOwnerSession& p) { p.transportEpoch = liveOwner; return liveOwner != 0; }
void secureClearString(std::string& s) { s.clear(); }
void g2ControlWake() {}
bool g2ControlWorkerEnsure() { return true; }
static void g2NativeTerminalTick();
static const char* g2NativeTerminalRequestOpen(bool = false, uint32_t = 0, uint32_t = 0);
static bool g2NativeTerminalGuardMatches(uint32_t, const G2Temple&);
void sendHijackShutdown(const char*) {
  // Force a worker interleave in the formerly unsafe admission window.
  pausedAtTeardown = true; g2NativeTerminalTick();
}
bool g2TerminalWireBlocksCreate() { return false; }
bool g2FsmHijackActive() { return false; }
struct LensState { bool containerReady = false; };
LensState g2LensGetState() { return {}; }
uint8_t allocSeq() { return 1; }
uint8_t g2NextSettingsMagic() { static uint8_t magic=1; return ++magic; }
unsigned firmwareQueries=0;
struct Sent { uint8_t command, magic; std::vector<uint8_t> pb; };
std::vector<Sent> sent;
bool sendPbFragmented(G2Temple& arm, uint8_t, uint8_t sid, uint8_t, const uint8_t* pb, size_t len,
                       uint32_t generation, bool*, uint32_t, uint32_t, uint32_t, uint32_t epoch) {
  assert(generation == arm.connectionGeneration);
  if (!g2NativeTerminalGuardMatches(epoch, arm)) return false;
  if (sid==G2_SID_SETTINGS) { assert(arm.side=='R'&&len);++firmwareQueries;return true; }
  assert(sid==G2_SID_TERMINAL);
  uint64_t command = 0, magic = 0, key = 0;
  size_t pos = 0;
  assert(g2PbReadVarint(pb, len, &pos, &key) && key == 8);
  assert(g2PbReadVarint(pb, len, &pos, &command));
  assert(g2PbReadVarint(pb, len, &pos, &key) && key == 16);
  assert(g2PbReadVarint(pb, len, &pos, &magic));
  sent.push_back({uint8_t(command), uint8_t(magic), {pb, pb + len}});
  return true;
}
constexpr size_t HTTP_TERMINAL_JSON_CAP = 4096;
using HttpTerminalToken = uint32_t;
struct HttpTerminalResult { bool done=false; int httpStatus=0; size_t length=0; uint32_t configRevision=0; char error[96]={}; };
uint32_t configRevision=10, nextRpcToken=0, activeRpcToken=0;
bool holdRpc=false, failRpc=false, wrongOpen=false;
std::string rpcResponse, rpcRequest;
std::vector<std::string> rpcRequests;
unsigned submissions=0, permissions=0, questions=0, interrupts=0, createdSessions=0;
std::string hostInstance="12345678-1234-1234-1234-123456789abc", hostView(32,'f');
std::string sessionA="hwo_"+std::string(64,'a'),sessionB="hwo_"+std::string(64,'b'),sessionC="hwo_"+std::string(64,'c');
std::string hostSession=sessionA,hostRun,hostState="idle",lastPrompt,lastDecision,lastInterrupt;
std::vector<std::string> hostEvents;
uint32_t eventCounter=0;
static std::string stringify(const JsonDocument& d) { std::string value; serializeJson(d,value); return value; }
static void content(uint32_t item,const char* text,uint8_t kind=2) {
  JsonDocument d; d["id"]=++eventCounter;d["type"]="content";d["itemId"]=item;d["kind"]=kind;d["text"]=text;
  hostEvents.push_back(stringify(d));
}
static void hostStatus(const char* state) {
  hostState=state; JsonDocument d;d["id"]=++eventCounter;d["type"]="state";d["state"]=state;
  if(hostRun.empty())d["runId"]=nullptr;else d["runId"]=hostRun;
  hostEvents.push_back(stringify(d));
}
static void hostQuery(const char* type,bool multi=false,bool other=false,char pending='d') {
  JsonDocument d;d["id"]=++eventCounter;d["type"]="query";d["pendingId"]=std::string(32,pending);d["queryType"]=type;
  d["questionId"]="q0";d["question"]="Choose exactly what is offered";d["multiSelect"]=multi;d["allowText"]=other;
  d["expiresInMs"]=60000;
  auto options=d["options"].to<JsonArray>();
  for(unsigned i=0;i<2;++i){auto x=options.add<JsonObject>();x["id"]=std::string(type)=="permission"?(i?"deny":"allow-once"):(i?"o1":"o0");x["label"]=i?"Second":"First";}
  hostEvents.push_back(stringify(d));hostState="awaiting";
}
static void hostClearQuery(const std::string& pending) {
  JsonDocument d;d["id"]=++eventCounter;d["type"]="query_clear";d["pendingId"]=pending;
  hostEvents.push_back(stringify(d));hostStatus("idle");
}
static std::string responseFor(const char* body) {
  JsonDocument q,r;assert(!deserializeJson(q,body));
  r["schema"]=1;r["ok"]=true;r["requestId"]=q["requestId"];r["instance"]=hostInstance;
  const std::string op=q["op"]|"";
  if(op=="list") {
    auto rows=r["sessions"].to<JsonArray>();
    for(const auto& handle:q["offset"].as<int>()?std::vector<std::string>{sessionC}:std::vector<std::string>{sessionA,sessionB}) {
      auto row=rows.add<JsonObject>();row["id"]=handle;row["title"]=handle==sessionA?"Existing Mingus A":handle==sessionB?"Existing Mingus B":"Existing Mingus C";row["state"]="idle";
    }
    if(q["offset"].as<int>())r["nextOffset"]=nullptr;else r["nextOffset"]=7;
  } else if(op=="open"||op=="new") {
    if(op=="new"){++createdSessions;hostSession="hwo_"+std::string(64,'e');}else hostSession=q["sessionId"].as<std::string>();
    hostView=std::string(32,char('1'+createdSessions%8));hostEvents.clear();eventCounter=0;
    content(1,"Retained host history",1);content(2,"Previous assistant answer",2);
    r["view"]=hostView;r["sessionId"]=wrongOpen?sessionC:hostSession;r["cursor"]=0;r["state"]=hostState;
    if(hostRun.empty())r["runId"]=nullptr;else r["runId"]=hostRun;
  } else {
    assert(q["view"].as<std::string>()==hostView&&q["instance"].as<std::string>()==hostInstance);
    if(op=="poll") {
      auto events=r["events"].to<JsonArray>();uint32_t cursor=q["after"];unsigned count=0;bool more=false;
      for(const auto& line:hostEvents){JsonDocument e;assert(!deserializeJson(e,line));if(e["id"].as<uint32_t>()<=cursor)continue;
        if(count==3){more=true;break;}events.add(e.as<JsonObjectConst>());cursor=e["id"];++count;}
      r["cursor"]=cursor;r["more"]=more;r["state"]=hostState;
      if(hostRun.empty())r["runId"]=nullptr;else r["runId"]=hostRun;
    } else if(op=="prompt") {
      ++submissions;lastPrompt=q["text"].as<std::string>();hostState="busy";hostRun=q["requestId"].as<std::string>();
      content(++eventCounter,lastPrompt.c_str(),1);r["runId"]=hostRun;r["accepted"]=true;
    } else if(op=="interrupt") {++interrupts;lastInterrupt=q["runId"].as<std::string>();r["accepted"]=lastInterrupt==hostRun;}
    else if(op=="permission") {++permissions;lastDecision=q["decision"].as<std::string>();r["applied"]=true;hostState="busy";}
    else if(op=="question") {++questions;lastPrompt=q["text"]|"";r["applied"]=false;r["complete"]=false;hostState="awaiting";}
    else assert(false);
  }
  return stringify(r);
}
uint32_t httpTerminalConfigRevision(){return configRevision;}
bool httpTerminalStart(const char* json,size_t length,HttpTerminalToken* token) {
  assert(length&&length<=4096&&strlen(json)==length);if(activeRpcToken)return false;
  rpcRequests.emplace_back(json,length);rpcRequest=rpcRequests.back();rpcResponse=responseFor(json);
  *token=activeRpcToken=++nextRpcToken;return true;
}
bool httpTerminalRead(HttpTerminalToken token,char* out,size_t cap,HttpTerminalResult* result) {
  if(token!=activeRpcToken)return false;*result={};out[0]=0;
  if(holdRpc)return true;result->done=true;result->httpStatus=200;result->configRevision=configRevision;
  if(failRpc){strcpy(result->error,"network failure");failRpc=false;}else{assert(rpcResponse.size()<cap);memcpy(out,rpcResponse.c_str(),rpcResponse.size()+1);result->length=rpcResponse.size();}
  activeRpcToken=0;return true;
}
void httpTerminalCancel(HttpTerminalToken token){if(token==activeRpcToken)activeRpcToken=0;}

// INSERT_ADAPTER
#if ENABLE_LOCAL_STT && ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
void event(uint8_t command,uint32_t value=0,uint32_t second=0,uint32_t epoch=0){
  uint8_t pb[64];size_t n=0,body=0;
  assert(g2PbWriteUint32(pb,sizeof(pb),&n,1,command));
  if(command==G2_TERMINAL_CMD_ACK)assert(g2PbWriteUint32(pb,sizeof(pb),&n,2,value));
  uint32_t tag=command==G2_TERMINAL_CMD_ACK?40:command==G2_TERMINAL_CMD_EXIT?21:command==G2_TERMINAL_CMD_VOICE?22:
    command==G2_TERMINAL_CMD_SESSION_SWITCH?25:command==G2_TERMINAL_CMD_NEW_SESSION?26:
    command==G2_TERMINAL_CMD_QUERY_REPLY?23:command==G2_TERMINAL_CMD_AWAIT_DISMISS?32:24;
  assert(g2PbBeginNested(pb,sizeof(pb),&n,tag,&body));
  if(command!=G2_TERMINAL_CMD_ACK&&command!=G2_TERMINAL_CMD_EXIT)assert(g2PbWriteUint32(pb,sizeof(pb),&n,1,value));
  if(command==G2_TERMINAL_CMD_QUERY_REPLY)assert(g2PbWriteUint32(pb,sizeof(pb),&n,2,second));
  assert(g2PbEndNested(pb,sizeof(pb),&n,body));
  g2NativeTerminalOnRx(gR,gR.connectionGeneration,epoch?epoch:gNativeTerminal.epoch,pb,n);
}
// Exact 2.3.0 DISPLAY_STATE: state/session/overlay in fields 1/2/3.
// SESSION_LIST=2, SESSION_ACTIVE=3; NONE=0 and QUERY=3 overlays.
void display(uint8_t state,uint32_t session,uint8_t overlay=0){
  uint8_t pb[64];size_t n=0,body=0;
  assert(g2PbWriteUint32(pb,sizeof(pb),&n,1,G2_TERMINAL_CMD_DISPLAY_STATE));
  assert(g2PbBeginNested(pb,sizeof(pb),&n,28,&body));
  assert(g2PbWriteUint32(pb,sizeof(pb),&n,1,state));
  assert(g2PbWriteUint32(pb,sizeof(pb),&n,2,session));
  assert(g2PbWriteUint32(pb,sizeof(pb),&n,3,overlay));
  assert(g2PbEndNested(pb,sizeof(pb),&n,body));
  g2NativeTerminalOnRx(gR,gR.connectionGeneration,gNativeTerminal.epoch,pb,n);
}
static std::vector<uint8_t> wireBody(const Sent& frame,uint32_t tag){
  size_t pos=0;uint32_t field;uint8_t wire;
  while(pos<frame.pb.size()){
    assert(g2PbReadTagStrict(frame.pb.data(),frame.pb.size(),&pos,&field,&wire));
    if(field==tag){const uint8_t* data=nullptr;size_t n=0;
      assert(wire==G2_PB_WIRE_LEN_DELIM&&g2PbReadDelimited(frame.pb.data(),frame.pb.size(),&pos,&data,&n));
      return {data,data+n};}
    assert(g2PbSkipFieldStrict(frame.pb.data(),frame.pb.size(),&pos,wire));
  }
  assert(false);return {};
}
static uint32_t wireNumber(const Sent& frame,uint32_t tag,uint32_t wanted){
  const auto body=wireBody(frame,tag);size_t pos=0;uint32_t field;uint8_t wire;uint64_t value;
  while(pos<body.size()){
    assert(g2PbReadTagStrict(body.data(),body.size(),&pos,&field,&wire));
    if(field==wanted){assert(wire==G2_PB_WIRE_VARINT&&g2PbReadVarintStrict(body.data(),body.size(),&pos,&value));return uint32_t(value);}
    assert(g2PbSkipFieldStrict(body.data(),body.size(),&pos,wire));
  }
  assert(false);return 0;
}
// Compare the complete offered wire question/options/session, allowing only
// the revocable native query ID (field 1) to change across presentation.
static std::vector<uint8_t> queryOffer(const Sent& frame){
  const auto body=wireBody(frame,8);std::vector<uint8_t> offer;size_t pos=0;
  while(pos<body.size()){
    const size_t start=pos;uint32_t field;uint8_t wire;
    assert(g2PbReadTagStrict(body.data(),body.size(),&pos,&field,&wire));
    assert(g2PbSkipFieldStrict(body.data(),body.size(),&pos,wire));
    if(field!=1)offer.insert(offer.end(),body.begin()+start,body.begin()+pos);
  }
  return offer;
}
static const Sent& lastQuery(){for(auto i=sent.rbegin();i!=sent.rend();++i)if(i->command==G2_TERMINAL_CMD_QUERY)return *i;assert(false);return sent.back();}
static size_t queryCount(size_t begin=0){size_t count=0;for(size_t i=begin;i<sent.size();++i)count+=sent[i].command==G2_TERMINAL_CMD_QUERY;return count;}
static size_t stateCount(size_t begin,uint32_t state){size_t count=0;for(size_t i=begin;i<sent.size();++i)if(sent[i].command==G2_TERMINAL_CMD_STATE)count+=wireNumber(sent[i],6,2)==state;return count;}
static std::string lastRequest(const char* operation){
  for(auto i=rpcRequests.rbegin();i!=rpcRequests.rend();++i){JsonDocument request;assert(!deserializeJson(request,*i));if(!strcmp(request["op"]|"",operation))return *i;}
  assert(false);return {};
}
void ack(){if(gNativeTerminal.policy.pending!=NativeTerm::Tx::None)event(G2_TERMINAL_CMD_ACK,gNativeTerminal.policy.magic);}
void tick(unsigned count=1){while(count--){clockMs+=100;g2NativeTerminalTick();}}
void settle(unsigned count=50){while(count--&&g2NativeTerminalActive()){tick();ack();}}
void untilIdle(){for(unsigned i=0;i<200&&g2NativeTerminalActive()&&gNativeTerminal.policy.phase!=NativeTerm::Phase::Idle;++i){tick();ack();}assert(gNativeTerminal.policy.phase==NativeTerm::Phase::Idle);}
void open(){assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));assert(pausedAtTeardown);untilIdle();}
void close(){gNativeTerminalSubmitGate.store(0);gNativeTerminalCloseRequested.store(true);tick();ack();assert(!g2NativeTerminalActive());}
void dictate(const char* text){transcript=text;event(G2_TERMINAL_CMD_VOICE,1);event(G2_TERMINAL_CMD_VOICE,1);event(G2_TERMINAL_CMD_VOICE,2);event(G2_TERMINAL_CMD_VOICE,2);
  for(unsigned i=0;i<20&&gNativeTerminal.policy.phase!=NativeTerm::Phase::AwaitConfirm;++i){tick();ack();}assert(gNativeTerminal.policy.phase==NativeTerm::Phase::AwaitConfirm);}
void awaitQuery(){const uint32_t through=eventCounter;for(unsigned i=0;i<100&&(!gNativeTerminal.queryActive||gNativeTerminal.queryDirty||gNativeTerminal.cursor<through);++i){tick();ack();}assert(gNativeTerminal.queryActive&&!gNativeTerminal.queryDirty&&gNativeTerminal.cursor>=through);}
void holdPoll(){holdRpc=true;for(unsigned i=0;i<30&&gNativeTerminal.rpc!=NativeRpc::Poll;++i){tick();ack();}assert(gNativeTerminal.rpc==NativeRpc::Poll);ack();}
int main(){
  // Cold-open metadata acquisition is asynchronous, one-shot, and cannot
  // contact the host or open native Terminal until both versions are known.
  gL.firmwareVersion[0]=gR.firmwareVersion[0]=0;
  assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));tick(5);
  assert(firmwareQueries==1&&rpcRequests.empty()&&sent.empty());
  strcpy(gL.firmwareVersion,"2.3.2.14");tick();assert(rpcRequests.empty());
  strcpy(gR.firmwareVersion,"2.3.2.14");untilIdle();close();
  const size_t coldRequests=rpcRequests.size(),coldNative=sent.size();
  gL.firmwareVersion[0]=gR.firmwareVersion[0]=0;
  assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));tick(17);
  assert(!g2NativeTerminalActive()&&firmwareQueries==2&&rpcRequests.size()==coldRequests&&sent.size()==coldNative);
  assert(strstr(gNativeTerminalReason.load(),"metadata-unavailable"));
  assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));tick();++gR.connectionGeneration;tick();
  assert(!g2NativeTerminalActive()&&firmwareQueries==3&&rpcRequests.size()==coldRequests);
  strcpy(gL.firmwareVersion,"2.3.2.15");strcpy(gR.firmwareVersion,"2.3.2.14");
  assert(!strncmp(g2NativeTerminalRequestOpen(),"Error:",6)&&firmwareQueries==3);
  strcpy(gL.firmwareVersion,"2.3.2.14");
  open();assert(hostSession==sessionA&&submissions==0);assert(gNativeTerminal.cursor==2);
  // Existing host session selection never creates a device-chat turn.
  const uint32_t sessionBId=gNativeTerminal.rows[1].id;
  event(G2_TERMINAL_CMD_SESSION_SWITCH,sessionBId);settle();assert(hostSession==sessionB&&submissions==0);
  dictate("Continue this existing conversation");assert(submissions==0&&speechBegins==1&&finishRequests==1);
  event(G2_TERMINAL_CMD_VOICE,4);event(G2_TERMINAL_CMD_VOICE,4);settle();assert(submissions==1&&lastPrompt=="Continue this existing conversation");
  // Absolute partial replacements keep host content identity; no local 2KiB cap.
  for(unsigned i=0;i<33;++i)content(10+i,std::string(i==32?511:512,'a').c_str());
  hostStatus("idle");untilIdle();settle(180);assert(gNativeTerminal.cursor==eventCounter);
  const unsigned accepted=submissions;close();open();assert(hostSession==sessionB&&submissions==accepted);
  // Pagination uses native control rows, never aliases a host conversation.
  event(G2_TERMINAL_CMD_SESSION_SWITCH,kNativeMore);settle();assert(gNativeTerminal.listOffset==7&&hostSession==sessionB);
  event(G2_TERMINAL_CMD_SESSION_SWITCH,kNativePrevious);settle();assert(gNativeTerminal.listOffset==0&&hostSession==sessionB);
  // Exact offered permission, duplicate reply ignored, no magic deduplication.
  hostQuery("permission");awaitQuery();const uint32_t permissionId=gNativeTerminal.queryId;
  event(G2_TERMINAL_CMD_QUERY_REPLY,permissionId,1);event(G2_TERMINAL_CMD_QUERY_REPLY,permissionId,1);settle();
  assert(permissions==1&&lastDecision=="allow-once");
  // Multi-panel staging is a valid applied:false,complete:false response.
  hostQuery("question",true);awaitQuery();event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);settle(10);
  assert(gNativeTerminal.queryActive&&gNativeTerminal.choices[0].selected);
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,100);settle();assert(questions==1&&g2NativeTerminalActive());
  // Other answer cannot turn into a new prompt on native dismissal.
  hostQuery("question",false,true);awaitQuery();const std::string voicePending=gNativeTerminal.pending;
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,101);settle(8);
  dictate("private answer draft");event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_VOICE,4);settle();assert(submissions==accepted&&questions==1&&!sttLive);
  assert(gNativeTerminal.queryActive&&!gNativeTerminal.questionVoice&&!gNativeTerminal.promptLen);
  hostClearQuery(voicePending);untilIdle();
  // Dismissing the waiting cue must preserve the host-owned question. Returning
  // from the selector restores its exact choices with a new presentation ID;
  // neither navigation nor a stale choice sends an answer.
  display(3,gNativeTerminal.sessionId);
  const size_t directQuestionBegin=sent.size();
  // A current awaiting snapshot may arrive before its ordered query event.
  for(unsigned i=0;i<8;++i)content(800+i,"Earlier tool activity",3);
  hostQuery("question");awaitQuery();settle(10);
  assert(stateCount(directQuestionBegin,G2_TERMINAL_STATE_AWAIT_USER)==0);
  const auto firstOffer=queryOffer(lastQuery());
  const std::string restorePending=gNativeTerminal.pending,restoreQuestion=gNativeTerminal.questionKey;
  const uint32_t dismissedId=gNativeTerminal.queryId,restoreDeadline=gNativeTerminal.queryDeadline;
  const unsigned beforeRestoreQuestions=questions;
  display(2,gNativeTerminal.sessionId);
  settle(10);assert(stateCount(directQuestionBegin,G2_TERMINAL_STATE_AWAIT_USER)>0);
  const size_t repeatedListBegin=sent.size();display(2,gNativeTerminal.sessionId);settle(10);
  assert(stateCount(repeatedListBegin,G2_TERMINAL_STATE_AWAIT_USER)==0);
  event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);
  assert(gNativeTerminal.queryActive&&std::string(gNativeTerminal.pending)==restorePending);
  assert(gNativeTerminal.queryId!=dismissedId&&gNativeTerminal.queryDeadline==restoreDeadline);
  const size_t restoreBegin=sent.size();settle(10);assert(queryCount(restoreBegin)==0);
  event(G2_TERMINAL_CMD_QUERY_REPLY,dismissedId,1);settle(5);assert(questions==beforeRestoreQuestions);
  display(3,gNativeTerminal.sessionId);settle(10);
  assert(queryCount(restoreBegin)==1&&queryOffer(lastQuery())==firstOffer);
  assert(wireNumber(lastQuery(),8,1)==gNativeTerminal.queryId);
  assert(std::string(gNativeTerminal.questionKey)==restoreQuestion&&gNativeTerminal.hostState==G2_TERMINAL_STATE_AWAIT_USER);
  display(3,gNativeTerminal.sessionId);settle(10);assert(queryCount(restoreBegin)==1);
  assert(questions==beforeRestoreQuestions&&submissions==accepted);
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,2);settle();
  assert(questions==beforeRestoreQuestions+1);
  {JsonDocument request;assert(!deserializeJson(request,lastRequest("question")));
    assert(request["pendingId"].as<std::string>()==restorePending&&request["questionId"].as<std::string>()==restoreQuestion);
    assert(request["optionIds"].size()==1&&request["optionIds"][0].as<std::string>()=="o1");}
  hostClearQuery(restorePending);untilIdle();
  // A foreground approval uses the same direct native query, but never loses
  // its offered decisions or becomes permission to submit an ordinary prompt.
  const size_t directPermissionBegin=sent.size();hostQuery("permission");awaitQuery();settle(10);
  assert(stateCount(directPermissionBegin,G2_TERMINAL_STATE_AWAIT_USER)==0);
  const std::string queuedPending=gNativeTerminal.pending;const unsigned beforeDismissPermissions=permissions;
  const uint32_t queuedId=gNativeTerminal.queryId;holdPoll();
  event(G2_TERMINAL_CMD_QUERY_REPLY,queuedId,1);assert(gNativeTerminal.nextRpc==NativeRpc::Permission);
  display(2,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);
  assert(gNativeTerminal.nextRpc!=NativeRpc::Permission&&!gNativeTerminal.decision[0]);
  holdRpc=false;settle(10);display(3,gNativeTerminal.sessionId);settle(10);
  assert(gNativeTerminal.queryActive&&permissions==beforeDismissPermissions&&gNativeTerminal.queryId!=queuedId);
  event(G2_TERMINAL_CMD_QUERY_REPLY,queuedId,1);settle(5);assert(permissions==beforeDismissPermissions);
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,2);settle();
  assert(permissions==beforeDismissPermissions+1&&lastDecision=="deny");
  hostClearQuery(queuedPending);untilIdle();
  // Queries arriving behind the selector, display-off or another session wait
  // for this session's content. Overlay hints do not own the local speech draft.
  for(const auto blocked:std::vector<std::pair<uint8_t,uint8_t>>{{2,0},{1,0},{3,3}}){
    const uint32_t displaySession=blocked.second==3?gNativeTerminal.sessionId+100:gNativeTerminal.sessionId;
    display(blocked.first,displaySession,blocked.second);const size_t hiddenBegin=sent.size();
    hostQuery("question");settle(15);assert(queryCount(hiddenBegin)==0&&gNativeTerminal.queryActive);
    const std::string hiddenPending=gNativeTerminal.pending;
    display(3,gNativeTerminal.sessionId);settle(10);assert(queryCount(hiddenBegin)==1);
    display(3,gNativeTerminal.sessionId,3);settle(10);assert(queryCount(hiddenBegin)==1);
    hostClearQuery(hiddenPending);untilIdle();
  }
  for(const uint8_t overlay:{1,2}){
    display(3,gNativeTerminal.sessionId,overlay);const size_t overlayBegin=sent.size();
    hostQuery("question");settle(15);assert(queryCount(overlayBegin)==1&&gNativeTerminal.queryActive);
    const std::string overlayPending=gNativeTerminal.pending;
    hostClearQuery(overlayPending);untilIdle();
  }
  // A reported VOICE overlay may survive voice cancellation. Local draft
  // ownership blocks query writes during recording; Cancel restores the same
  // question without requiring a fresh DISPLAY_STATE from the glasses.
  display(3,gNativeTerminal.sessionId);hostQuery("question",false,true);awaitQuery();
  const auto otherOffer=queryOffer(lastQuery());const std::string otherPending=gNativeTerminal.pending;
  const unsigned beforeOtherQuestions=questions;
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,101);settle(8);
  display(3,gNativeTerminal.sessionId,2);event(G2_TERMINAL_CMD_VOICE,1);assert(sttLive);
  const size_t otherBegin=sent.size();settle(10);assert(queryCount(otherBegin)==0);
  event(G2_TERMINAL_CMD_VOICE,5);settle(15);
  assert(!sttLive&&!gNativeTerminal.questionVoice&&gNativeTerminal.queryActive);
  assert(queryCount(otherBegin)==1&&queryOffer(lastQuery())==otherOffer&&questions==beforeOtherQuestions);
  // The same stale overlay must not strand the next panel after a confirmed
  // voice answer. The fixture returns complete:false just as a multi-panel
  // question does, then emits its next host-owned panel with the same ID.
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,101);settle(8);
  dictate("The explicit answer for the first panel");event(G2_TERMINAL_CMD_VOICE,4);settle(15);
  assert(questions==beforeOtherQuestions+1&&submissions==accepted);
  {JsonDocument request;assert(!deserializeJson(request,lastRequest("question")));
    assert(request["pendingId"].as<std::string>()==otherPending&&request["text"].as<std::string>()=="The explicit answer for the first panel");}
  const size_t nextPanelBegin=sent.size();hostQuery("question");
  {JsonDocument panel;assert(!deserializeJson(panel,hostEvents.back()));panel["questionId"]="q1";panel["question"]="Next panel choices";hostEvents.back()=stringify(panel);}
  settle(15);assert(gNativeTerminal.queryActive&&queryCount(nextPanelBegin)==1&&!strcmp(gNativeTerminal.questionKey,"q1"));
  assert(questions==beforeOtherQuestions+1&&submissions==accepted);
  hostClearQuery(otherPending);untilIdle();
  // Returning cannot resurrect an expired or host-revoked offer.
  hostQuery("question");awaitQuery();const std::string expiredPending=gNativeTerminal.pending;
  display(2,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);settle(5);
  clockMs=gNativeTerminal.queryDeadline+1;const size_t expiredBegin=sent.size();tick();ack();
  display(3,gNativeTerminal.sessionId);settle(10);assert(!gNativeTerminal.queryActive&&queryCount(expiredBegin)==0);
  hostClearQuery(expiredPending);untilIdle();
  hostQuery("question");awaitQuery();const std::string revokedPending=gNativeTerminal.pending;
  display(2,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);
  hostClearQuery(revokedPending);settle(15);const size_t revokedBegin=sent.size();
  display(3,gNativeTerminal.sessionId);settle(10);assert(!gNativeTerminal.queryActive&&queryCount(revokedBegin)==0);
  // Dismissal also revokes staged multi-select choices and a reply deferred
  // behind a native ACK. Neither can migrate to the restored presentation.
  hostQuery("question",true);awaitQuery();const std::string deferredPending=gNativeTerminal.pending;
  const unsigned beforeDeferredQuestions=questions;
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);settle(10);assert(gNativeTerminal.choices[0].selected);
  display(2,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);
  assert(!gNativeTerminal.choices[0].selected);
  display(3,gNativeTerminal.sessionId);
  for(unsigned i=0;i<30&&gNativeTerminal.policy.pending!=NativeTerm::Tx::Query;++i){tick();if(gNativeTerminal.policy.pending!=NativeTerm::Tx::Query)ack();}
  assert(gNativeTerminal.policy.pending==NativeTerm::Tx::Query);
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);assert(gNativeTerminal.deferredQueryId);
  display(2,gNativeTerminal.sessionId);event(G2_TERMINAL_CMD_AWAIT_DISMISS,gNativeTerminal.sessionId);
  assert(!gNativeTerminal.deferredQueryId&&!gNativeTerminal.deferredOptionId);ack();
  display(3,gNativeTerminal.sessionId);settle(15);
  assert(questions==beforeDeferredQuestions&&!gNativeTerminal.choices[0].selected);
  hostClearQuery(deferredPending);untilIdle();
  assert(submissions==accepted);
  // A binding ACK acknowledges that write, not the screen's current focus.
  // LIST/OFF notifications received after the write but before its ACK
  // remain authoritative for both OPEN and SESSION_CHANGE bindings.
  for(const auto focus:std::vector<std::pair<uint8_t,uint8_t>>{{2,0},{1,0}}){
    for(const bool initialOpen:{false,true}){
      if(initialOpen){
        close();assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));
        for(unsigned i=0;i<100&&gNativeTerminal.policy.pending!=NativeTerm::Tx::Open;++i){tick();if(gNativeTerminal.policy.pending!=NativeTerm::Tx::Open)ack();}
        assert(gNativeTerminal.policy.pending==NativeTerm::Tx::Open);
      }else assert(g2NativeTerminalSend(NativeTerm::Tx::SessionChange));
      display(focus.first,gNativeTerminal.sessionId,focus.second);ack();
      const size_t bindingBegin=sent.size();hostQuery("question");settle(25);
      assert(gNativeTerminal.queryActive&&queryCount(bindingBegin)==0);
      const std::string bindingPending=gNativeTerminal.pending;
      display(3,gNativeTerminal.sessionId);settle(10);assert(queryCount(bindingBegin)==1);
      hostClearQuery(bindingPending);untilIdle();
    }
  }
  // New view creates no prompt. Follow-up must still be explicit.
  const auto newWireOrder = [](size_t begin) {
    size_t result=sent.size(),list=sent.size(),change=sent.size(),sync=sent.size();
    for(size_t i=begin;i<sent.size();++i) {
      if(sent[i].command==G2_TERMINAL_CMD_NEW_RESULT)result=std::min(result,i);
      if(sent[i].command==G2_TERMINAL_CMD_SESSION_LIST)list=std::min(list,i);
      if(sent[i].command==G2_TERMINAL_CMD_SESSION_CHANGE)change=std::min(change,i);
      if(sent[i].command==G2_TERMINAL_CMD_SYNC)sync=std::min(sync,i);
    }
    // Stock's successful NewResult clears the native current ID/content;
    // authoritative binding and history must therefore follow its ACK.
    assert(result<list&&list<change&&change<sync&&sync<sent.size());
  };
  size_t newBegin=sent.size();
  event(G2_TERMINAL_CMD_NEW_SESSION,1);settle();assert(createdSessions==1&&submissions==accepted&&hostSession!=sessionB);
  newWireOrder(newBegin);
  // Current stock omits the default-zero agent field for its one-agent New
  // selector. It still targets sole Mingus and never submits a prompt.
  const uint8_t defaultNewPb[]={0x08,0xa6,0x01,0xd2,0x01,0x00};
  newBegin=sent.size();
  g2NativeTerminalOnRx(gR,gR.connectionGeneration,gNativeTerminal.epoch,defaultNewPb,sizeof(defaultNewPb));
  settle();assert(createdSessions==2&&submissions==accepted&&g2NativeTerminalActive());
  newWireOrder(newBegin);newBegin=sent.size();
  event(G2_TERMINAL_CMD_NEW_SESSION,0);settle();
  assert(createdSessions==3&&submissions==accepted&&g2NativeTerminalActive());
  newWireOrder(newBegin);
  event(G2_TERMINAL_CMD_NEW_SESSION,2);settle();
  assert(createdSessions==3&&submissions==accepted&&!g2NativeTerminalActive());
  assert(!strcmp(gNativeTerminalReason.load(),"unknown-native-agent"));
  // No retry after an ambiguous mutation failure; no retained draft on exit.
  open();dictate("one attempt");failRpc=true;event(G2_TERMINAL_CMD_VOICE,4);settle();assert(!g2NativeTerminalActive()&&submissions==accepted+1);
  hostState="idle";hostRun.clear();open();const uint32_t old=gNativeTerminal.epoch;
  const uint8_t exitPb[]={0x08,0xab,0x01,0xaa,0x01,0};uint8_t frame[32];
  size_t n=g2BuildEnvelope(1,G2_SID_TERMINAL,G2_FLAG_RESPONSE,exitPb,sizeof(exitPb),frame,sizeof(frame));assert(n);frame[1]=G2_PREAMBLE_RX;
  g2NativeTerminalEarlyRx(gR,frame,n);assert(!g2NativeTerminalGuardMatches(old,gR));tick();assert(!g2NativeTerminalActive());
  open();event(G2_TERMINAL_CMD_VOICE,1,0,old);assert(!sttLive);
  ++configRevision;tick();ack();assert(!g2NativeTerminalActive());
  open();dictate("revoked before confirmation");gNativeTerminalSubmitGate.store(0);event(G2_TERMINAL_CMD_VOICE,4);assert(submissions==accepted+1);gNativeTerminalCloseRequested.store(true);tick();ack();
  // Host must return exactly the selected handle, never just any valid handle.
  wrongOpen=true;assert(strncmp(g2NativeTerminalRequestOpen(),"Error:",6));settle();assert(!g2NativeTerminalActive());wrongOpen=false;
  // An interrupt stays bound to the run displayed at the gesture, even when
  // an outstanding poll announces a different run before HTTP admission.
  hostState="idle";hostRun.clear();open();hostRun="old-run";hostStatus("busy");settle();
  assert(std::string(gNativeTerminal.run)=="old-run");holdPoll();
  event(G2_TERMINAL_CMD_INTERRUPT,gNativeTerminal.sessionId);
  assert(gNativeTerminal.nextRpc==NativeRpc::Interrupt);
  hostRun="new-run";hostStatus("busy");rpcResponse=responseFor(rpcRequest.c_str());holdRpc=false;settle();
  assert(lastInterrupt=="old-run"&&interrupts==1&&!g2NativeTerminalActive());
  // A replacement card revokes a queued permission; even identical pending
  // and question IDs cannot carry a staged selection into a replacement card.
  hostState="idle";hostRun.clear();open();hostQuery("permission");awaitQuery();holdPoll();
  const unsigned beforePermissions=permissions,beforeQuestions=questions;
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);
  assert(gNativeTerminal.nextRpc==NativeRpc::Permission);
  hostQuery("permission",false,false,'e');rpcResponse=responseFor(rpcRequest.c_str());holdRpc=false;settle();
  assert(permissions==beforePermissions&&std::string(gNativeTerminal.pending)==std::string(32,'e'));
  hostQuery("question",true);awaitQuery();event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);settle(10);
  holdPoll();event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,100);
  assert(gNativeTerminal.nextRpc==NativeRpc::Question);
  hostQuery("question",true);rpcResponse=responseFor(rpcRequest.c_str());holdRpc=false;settle();
  assert(questions==beforeQuestions&&gNativeTerminal.queryActive&&!gNativeTerminal.choices[0].selected);
  // A session selection drops an unacknowledged old-card notice. Interrupts
  // during Open cannot combine the old view/run with the new native session.
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);assert(gNativeTerminal.choiceNotice);
  const uint32_t selectedId=gNativeTerminal.rows[1].id;
  event(G2_TERMINAL_CMD_SESSION_SWITCH,selectedId);holdRpc=true;const size_t selectionSent=sent.size();tick();
  assert(gNativeTerminal.rpc==NativeRpc::Open&&!gNativeTerminal.choiceNotice);
  event(G2_TERMINAL_CMD_INTERRUPT,selectedId);tick();
  assert(gNativeTerminal.nextRpc!=NativeRpc::Interrupt);
  for(size_t i=selectionSent;i<sent.size();++i)assert(sent[i].command!=G2_TERMINAL_CMD_CONTENT);
  holdRpc=false;hostState="idle";rpcResponse=responseFor(rpcRequest.c_str());settle();close();
  // Optional activity changes display only; all busy phases still reject a
  // new utterance even if the preceding display phase had been idle.
  open();JsonDocument activity;activity["state"]="busy";
  for(const auto& entry:std::vector<std::pair<const char*,uint8_t>>{{"thinking",2},{"tool",3},{"responding",4}}){
    activity["activity"]=entry.first;assert(g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));assert(gNativeTerminal.state==entry.second);
  }
  activity["state"]="idle";assert(g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));assert(gNativeTerminal.state==G2_TERMINAL_STATE_IDLE);
  activity["activity"]="unknown";assert(!g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));
  activity["activity"]=nullptr;assert(!g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));
  activity.remove("activity");activity["state"]="busy";assert(g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));assert(gNativeTerminal.state==G2_TERMINAL_STATE_THINKING);
  activity["activity"]="responding";assert(g2NativeTerminalStateFrom(activity.as<JsonObjectConst>()));
  const unsigned beforeSpeech=speechBegins;event(G2_TERMINAL_CMD_VOICE,1);assert(speechBegins==beforeSpeech&&gNativeTerminalCloseRequested.load());tick();ack();assert(!g2NativeTerminalActive());
  // Repeated idle polls do not reset native Listening. A queued/read-only poll
  // is compatible with voice; a held nonempty batch never presents over ASR.
  hostState="idle";hostRun.clear();open();settle(15);
  const size_t idleBegin=sent.size();settle(65);
  for(size_t i=idleBegin;i<sent.size();++i)assert(sent[i].command!=G2_TERMINAL_CMD_STATE);
  g2NativeTerminalCancelRpc();g2NativeTerminalQueueRpc(NativeRpc::Poll);
  event(G2_TERMINAL_CMD_VOICE,1);assert(sttLive&&gNativeTerminal.policy.phase==NativeTerm::Phase::Recording);
  const size_t voiceBegin=sent.size();settle(10);
  content(700,"History received during speech");hostStatus("idle");settle(65);
  assert(gNativeTerminal.batch&&gNativeTerminal.policy.phase==NativeTerm::Phase::Recording);
  bool heartbeat=false;for(size_t i=voiceBegin;i<sent.size();++i){assert(sent[i].command==G2_TERMINAL_CMD_HEARTBEAT);heartbeat=true;}assert(heartbeat);
  transcript="A confirmed draft survives a pending batch";event(G2_TERMINAL_CMD_VOICE,2);
  for(unsigned i=0;i<20&&gNativeTerminal.policy.phase!=NativeTerm::Phase::AwaitConfirm;++i){tick();ack();}
  assert(gNativeTerminal.policy.phase==NativeTerm::Phase::AwaitConfirm);
  const unsigned beforeDraftSubmit=submissions;event(G2_TERMINAL_CMD_VOICE,4);settle(80);
  assert(submissions==beforeDraftSubmit+1&&lastPrompt==transcript&&gNativeTerminal.cursor==eventCounter);
  hostStatus("idle");untilIdle();close();
  // An in-flight poll may finish after CONFIRM: its busy snapshot wins before
  // mutation admission, and CLOSE preserves that original diagnostic.
  hostRun.clear();open();dictate("must not submit into a busy host");holdPoll();
  const unsigned beforeBusySubmit=submissions;event(G2_TERMINAL_CMD_VOICE,4);
  hostStatus("busy");rpcResponse=responseFor(rpcRequest.c_str());holdRpc=false;settle();
  assert(submissions==beforeBusySubmit&&!g2NativeTerminalActive());
  assert(!strcmp(gNativeTerminalReason.load(),"host-became-busy-during-speech"));
  // Replacing an Other-answer card cancels speech, never reclassifies its text
  // as a prompt and presents the replacement only after releasing the draft.
  hostState="idle";hostRun.clear();open();hostQuery("question",false,true);awaitQuery();
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,101);settle(8);holdPoll();
  event(G2_TERMINAL_CMD_VOICE,1);assert(sttLive);
  hostQuery("question",false,true,'e');rpcResponse=responseFor(rpcRequest.c_str());holdRpc=false;settle();
  assert(!sttLive&&!gNativeTerminal.questionVoice&&std::string(gNativeTerminal.pending)==std::string(32,'e'));
  event(G2_TERMINAL_CMD_VOICE,4);assert(submissions==beforeBusySubmit);
  g2NativeTerminalFail("first-failure");g2NativeTerminalFail("later-close-failure");tick();ack();
  assert(!g2NativeTerminalActive()&&!strcmp(gNativeTerminalReason.load(),"first-failure"));
  // Native menu START has an ingress-only ticket, is one-shot and bound to the
  // original link/owner. It opens history only, with a bounded connecting wait.
  const uint8_t startPb[]={0x08,0xa1,0x01,0xa2,0x01,0};
  n=g2BuildEnvelope(1,G2_SID_TERMINAL,G2_FLAG_RESPONSE,startPb,sizeof(startPb),frame,sizeof(frame));frame[1]=G2_PREAMBLE_RX;
  auto menuStart=[&](){return g2NativeTerminalEarlyRx(gR,frame,n);};
  auto menuDeliver=[&](uint32_t ticket,uint32_t generation){g2NativeTerminalOnRx(gR,generation,ticket,startPb,sizeof(startPb));};
  hostState="idle";uint32_t ticket=menuStart();assert(ticket&0x80000000u);
  const unsigned beforeMenuSubmit=submissions;menuDeliver(ticket,gR.connectionGeneration);untilIdle();
  menuDeliver(ticket,gR.connectionGeneration);assert(submissions==beforeMenuSubmit);close();
  menuDeliver(ticket,gR.connectionGeneration);assert(!g2NativeTerminalActive());
  ticket=menuStart();++liveOwner;menuDeliver(ticket,gR.connectionGeneration);assert(!g2NativeTerminalActive());
  ticket=menuStart();const uint32_t previousGeneration=gR.connectionGeneration++;menuDeliver(ticket,previousGeneration);assert(!g2NativeTerminalActive());
  ticket=menuStart();g2NativeTerminalRevoke("connection-changed");menuDeliver(ticket,gR.connectionGeneration);assert(!g2NativeTerminalActive());
  ticket=menuStart();clockMs+=15000;menuDeliver(ticket,gR.connectionGeneration);assert(!g2NativeTerminalActive());
  ticket=menuStart();holdRpc=true;menuDeliver(ticket,gR.connectionGeneration);tick();
  clockMs+=15000;tick();assert(!g2NativeTerminalActive()&&!strcmp(gNativeTerminalReason.load(),"native-menu-open-timeout-retry"));holdRpc=false;
  // A brief release can finish normally without any recognized speech. It is
  // a discarded draft, not a failed Terminal session or an empty host prompt.
  hostState="idle";hostRun.clear();open();settle(10);
  const unsigned beforeEmptyPrompts=submissions,beforeEmptyQuestions=questions;
  const std::string emptyView=gNativeTerminal.view,emptySession=gNativeTerminal.selected;
  transcript.clear();const size_t emptyBegin=sent.size();
  event(G2_TERMINAL_CMD_VOICE,1);tick(2);ack();event(G2_TERMINAL_CMD_VOICE,2);settle(20);
  assert(g2NativeTerminalActive()&&gNativeTerminal.policy.phase==NativeTerm::Phase::Idle);
  assert(!sttLive&&!gNativeTerminal.speechToken&&!gNativeTerminal.promptLen);
  assert(std::string(gNativeTerminal.view)==emptyView&&std::string(gNativeTerminal.selected)==emptySession);
  assert(submissions==beforeEmptyPrompts&&questions==beforeEmptyQuestions);
  for(size_t i=emptyBegin;i<sent.size();++i)assert(sent[i].command!=G2_TERMINAL_CMD_CLOSE&&sent[i].command!=G2_TERMINAL_CMD_ASR);
  assert(stateCount(emptyBegin,G2_TERMINAL_STATE_IDLE)==1);
  event(G2_TERMINAL_CMD_VOICE,4);settle(20);
  assert(g2NativeTerminalActive()&&submissions==beforeEmptyPrompts&&questions==beforeEmptyQuestions);
  assert(stateCount(emptyBegin,G2_TERMINAL_STATE_IDLE)==1);
  // A state write accepted just before recording can be acknowledged after
  // the empty worker finishes. Recovery waits so that old ACK cannot erase
  // the one forced IDLE update which exits the native speech interface.
  const size_t delayedEmptyBegin=sent.size();assert(g2NativeTerminalSend(NativeTerm::Tx::State));
  event(G2_TERMINAL_CMD_VOICE,1);tick(2);event(G2_TERMINAL_CMD_VOICE,2);tick(2);
  assert(g2NativeTerminalActive()&&gNativeTerminal.policy.phase==NativeTerm::Phase::Finishing);
  assert(gNativeTerminal.policy.pending==NativeTerm::Tx::State);ack();settle(20);
  assert(g2NativeTerminalActive()&&gNativeTerminal.policy.phase==NativeTerm::Phase::Idle);
  assert(stateCount(delayedEmptyBegin,G2_TERMINAL_STATE_IDLE)==2);
  assert(submissions==beforeEmptyPrompts&&questions==beforeEmptyQuestions);
  for(size_t i=delayedEmptyBegin;i<sent.size();++i)assert(sent[i].command!=G2_TERMINAL_CMD_CLOSE&&sent[i].command!=G2_TERMINAL_CMD_ASR);
  dictate("A real follow-up after a short accidental hold");event(G2_TERMINAL_CMD_VOICE,4);settle(20);
  assert(submissions==beforeEmptyPrompts+1&&lastPrompt=="A real follow-up after a short accidental hold");
  hostStatus("idle");untilIdle();
  // Empty Other-answer speech restores the exact pending offer with a fresh
  // presentation ID, keeping its expiry and rejecting the abandoned replies.
  hostQuery("question",false,true);awaitQuery();
  const std::string emptyOtherPending=gNativeTerminal.pending,emptyOtherQuestion=gNativeTerminal.questionKey;
  const uint32_t emptyOtherId=gNativeTerminal.queryId,emptyOtherDeadline=gNativeTerminal.queryDeadline;
  const auto emptyOtherOffer=queryOffer(lastQuery());
  event(G2_TERMINAL_CMD_QUERY_REPLY,emptyOtherId,101);settle(8);display(3,gNativeTerminal.sessionId,2);
  transcript.clear();const size_t emptyOtherBegin=sent.size();
  event(G2_TERMINAL_CMD_VOICE,1);tick(2);ack();event(G2_TERMINAL_CMD_VOICE,2);settle(20);
  assert(g2NativeTerminalActive()&&gNativeTerminal.queryActive&&!gNativeTerminal.questionVoice);
  assert(!sttLive&&!gNativeTerminal.speechToken&&!gNativeTerminal.promptLen);
  assert(gNativeTerminal.queryId!=emptyOtherId&&gNativeTerminal.queryDeadline==emptyOtherDeadline);
  assert(std::string(gNativeTerminal.pending)==emptyOtherPending&&std::string(gNativeTerminal.questionKey)==emptyOtherQuestion);
  assert(queryCount(emptyOtherBegin)==1&&queryOffer(lastQuery())==emptyOtherOffer);
  assert(stateCount(emptyOtherBegin,G2_TERMINAL_STATE_IDLE)==1&&stateCount(emptyOtherBegin,G2_TERMINAL_STATE_AWAIT_USER)==0);
  size_t emptyOtherIdle=sent.size(),emptyOtherQuery=sent.size();
  for(size_t i=emptyOtherBegin;i<sent.size();++i){
    if(sent[i].command==G2_TERMINAL_CMD_STATE&&wireNumber(sent[i],6,2)==G2_TERMINAL_STATE_IDLE)emptyOtherIdle=std::min(emptyOtherIdle,i);
    if(sent[i].command==G2_TERMINAL_CMD_QUERY)emptyOtherQuery=std::min(emptyOtherQuery,i);}
  assert(emptyOtherIdle<emptyOtherQuery);
  for(size_t i=emptyOtherBegin;i<sent.size();++i)assert(sent[i].command!=G2_TERMINAL_CMD_CLOSE&&sent[i].command!=G2_TERMINAL_CMD_ASR);
  event(G2_TERMINAL_CMD_VOICE,4);event(G2_TERMINAL_CMD_QUERY_REPLY,emptyOtherId,1);settle(10);
  assert(submissions==beforeEmptyPrompts+1&&questions==beforeEmptyQuestions);
  event(G2_TERMINAL_CMD_QUERY_REPLY,gNativeTerminal.queryId,1);settle(20);
  assert(questions==beforeEmptyQuestions+1&&submissions==beforeEmptyPrompts+1);
  {JsonDocument request;assert(!deserializeJson(request,lastRequest("question")));
    assert(request["pendingId"].as<std::string>()==emptyOtherPending&&request["questionId"].as<std::string>()==emptyOtherQuestion);
    assert(request["optionIds"].size()==1&&request["optionIds"][0].as<std::string>()=="o0");}
  hostClearQuery(emptyOtherPending);untilIdle();
  // A real STT failure is still fatal; empty successful capture recovery must
  // not silently convert Failed into a new listening turn.
  stoppedSpeechState=STTState::Failed;transcript.clear();const size_t failedSpeechBegin=sent.size();
  event(G2_TERMINAL_CMD_VOICE,1);tick(2);ack();event(G2_TERMINAL_CMD_VOICE,2);settle(20);
  assert(!g2NativeTerminalActive()&&!sttLive);
  bool failedSpeechClosed=false;for(size_t i=failedSpeechBegin;i<sent.size();++i){
    failedSpeechClosed|=sent[i].command==G2_TERMINAL_CMD_CLOSE;assert(sent[i].command!=G2_TERMINAL_CMD_ASR);}
  assert(failedSpeechClosed&&submissions==beforeEmptyPrompts+1&&questions==beforeEmptyQuestions+1);
  stoppedSpeechState=STTState::Done;
  NativeTerm magic;for(unsigned i=0;i<255;++i)assert(magic.allocate(100));assert(!magic.allocate(100));assert(magic.allocate(3101));
  puts("native Terminal host-session lifecycle tests passed");
}
#else
int main(){assert(!g2NativeTerminalActive());assert(!strncmp(g2NativeTerminalRequestOpen(),"Error:",6));g2NativeTerminalTick();puts("native Terminal disabled-feature test passed");}
#endif
