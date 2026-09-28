#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "System_DictationPolicy.h"
#define ENABLE_ESP_SR 1
#define DICTATION_MAX_TEXT 256
#define EXT_RAM_BSS_ATTR
using String=std::string;
using TransportSessionEpoch=uint32_t;
constexpr uint32_t kNoTransportSessionEpoch=0;
enum CommandSource { SOURCE_WEB,SOURCE_SERIAL,SOURCE_INTERNAL,SOURCE_ESPNOW,
 SOURCE_LOCAL_DISPLAY,SOURCE_BLUETOOTH,SOURCE_MQTT,SOURCE_VOICE,SOURCE_G2_GLASSES,SOURCE_UART };
// INSERT_HEADERS
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)x)
#define portEXIT_CRITICAL(x) ((void)x)
uint32_t millis();
// INSERT_CONTROL
static uint32_t nowMs=1;
uint32_t millis(){return nowMs;}
uint32_t esp_random(){return 0x12345678;}
static uint32_t gDictBootNonce=0,gDictCounter=0;
static bool workerOK=true;
static int wakeCount=0;
static bool dictationEnsureWorker(){return workerOK;}
static void dictationWakeWorker(){++wakeCount;}
static std::set<std::pair<CommandSource,uint32_t>> sessions;
static std::function<void()> sessionHook;
static bool displaySessionStillLive(CommandSource s,uint32_t epoch){
 if(sessionHook){auto hook=sessionHook;sessionHook=nullptr;hook();}
 return sessions.count({s,epoch});
}
static bool micBusy=false,srBusy=false;
bool gMicRunning=false;
bool micRecordingBusy(){return micBusy;}
bool isESPSRRunning(){return srBusy;}
static std::string halOwner;
bool audioCaptureOwnedBy(const char* o){return halOwner==o;}
bool audioCaptureBusy(){return !halOwner.empty();}
static bool modelReady=true;
static int readyChecks=0;
bool sttLocalAvailable(char*,size_t){++readyChecks;return modelReady;}
static int beginCalls=0,cancelCalls=0,finishCalls=0,ackCalls=0;
static bool beginOK=true,ackOK=true;
static STTOwner brokerOwner;
static STTSnapshot broker;
static std::deque<STTTextChunk> chunks;
static std::function<void()> beginHook,ackHook,runActiveHook;
bool sttBeginContinuous(STTOwner owner,STTToken* token,char* error,size_t cap){
 ++beginCalls;if(beginHook)beginHook();
 if(!beginOK){snprintf(error,cap,"run closemic first");return false;}
 brokerOwner=owner;broker=STTSnapshot{};broker.token=42;broker.continuous=true;
 broker.state=STTState::Preparing;broker.workerActive=true;*token=42;return true;
}
bool sttRunActive(STTToken t){
 if(runActiveHook){auto hook=runActiveHook;runActiveHook=nullptr;hook();}
 return broker.token==t&&broker.workerActive;
}
bool same(STTOwner a,STTOwner b){return a.source==b.source&&a.epoch==b.epoch;}
bool sttCancel(STTOwner o,STTToken t){
 if(!same(o,brokerOwner)||t!=broker.token||!displaySessionStillLive(o.source,o.epoch))return false;
 ++cancelCalls;chunks.clear();return true;
}
bool sttRequestFinish(STTOwner o,STTToken t){
 if(!same(o,brokerOwner)||t!=broker.token)return false;++finishCalls;return true;
}
bool sttSnapshot(STTOwner o,STTToken t,STTSnapshot* out){
 if(!same(o,brokerOwner)||t!=broker.token||!displaySessionStillLive(o.source,o.epoch))return false;
 *out=broker;out->pendingTexts=chunks.size();return true;
}
bool sttReadChunk(STTOwner o,STTToken t,STTTextChunk* out){
 if(!same(o,brokerOwner)||t!=broker.token||chunks.empty()||!displaySessionStillLive(o.source,o.epoch))return false;
 *out=chunks.front();return true;
}
bool sttAcknowledgeChunk(STTOwner o,STTToken t,uint32_t seq){
 if(ackHook){auto hook=ackHook;ackHook=nullptr;hook();}
 if(!ackOK||!same(o,brokerOwner)||t!=broker.token||chunks.empty()||chunks.front().sequence!=seq||!displaySessionStillLive(o.source,o.epoch))return false;
 ++ackCalls;chunks.pop_front();return true;
}
bool dictationAvailable(const char** why);
static std::vector<uint64_t> cleanup,stops,cancelEvents;
void requestStopRecordingOwned(uint64_t id,bool){stops.push_back(id);}
bool dictationQueueCleanup(uint64_t id,bool){cleanup.push_back(id);return true;}
void dictationQueueCancelEvent(uint64_t id,uint32_t){cancelEvents.push_back(id);}
// INSERT_LOCAL
bool dictationAvailable(const char** why){
#if ENABLE_LOCAL_STT
 return dictationLocalAvailable(why);
#else
 (void)why;return true;
#endif
}
// INSERT_DRAIN
// INSERT_STOP
static STTOwner glasses{SOURCE_G2_GLASSES,3};
static void reset(){
 gDict=DictationControl{};gDict.state=DictationState::IDLE;
#if ENABLE_LOCAL_STT
 gLocalDict=LocalDictation{};gLocalReadyKnown=gLocalReady=gLocalReadyPending=false;gLocalReadyCheckedMs=0;
#endif
 nowMs=1;wakeCount=readyChecks=beginCalls=cancelCalls=finishCalls=ackCalls=0;
 sessions={{SOURCE_G2_GLASSES,3},{SOURCE_LOCAL_DISPLAY,4}};
 workerOK=modelReady=beginOK=ackOK=true;micBusy=srBusy=gMicRunning=false;halOwner.clear();
 beginHook=ackHook=runActiveHook=sessionHook=nullptr;broker=STTSnapshot{};chunks.clear();cleanup.clear();stops.clear();cancelEvents.clear();
}
static void stagePi(uint64_t id,const std::string& text){
 gDict.state=DictationState::WAITING;gDict.owner=id;gDict.requestHostEpoch=9;
 gDict.displaySource=glasses.source;gDict.displayEpoch=glasses.epoch;
 auto reply=dictDeliver(id,text.c_str(),text.size(),9);
 assert(std::string(reply).find("OK:")==0);
 assert(gDict.owner==0&&gDict.deliveryExchange==id&&gDict.deliverySequence==1);
}
static void testPiMailbox(){
 reset();stagePi(123,"abcdef");assert(cleanup==std::vector<uint64_t>{123});
 char text[257];DictationTextReceipt a,b;
 assert(!dictationPeekTextFor(SOURCE_LOCAL_DISPLAY,text,sizeof(text),&a));
 assert(dictationPeekTextFor(glasses.source,text,4,&a)&&std::string(text)=="abc");
 assert(dictationPeekTextFor(glasses.source,text,4,&b)&&a.offset==b.offset);
 assert(!dictationCommitTextFor(SOURCE_LOCAL_DISPLAY,a,3));
 assert(!dictationCommitTextFor(glasses.source,a,4));
 assert(dictationCommitTextFor(glasses.source,a,2));
 assert(gDict.state==DictationState::WAITING&&gDict.deliveryExchange==123);
 assert(!dictationCommitTextFor(glasses.source,a,1)); // stale offset/replay
 assert(dictationTakeTextFor(glasses.source,text,sizeof(text))&&std::string(text)=="cdef");
 assert(gDict.state==DictationState::IDLE&&!gDict.deliveryExchange);
 assert(!dictationTakeTextFor(glasses.source,text,sizeof(text)));
 assert(std::string(dictDeliver(123,"replay",6,9)).find("Error:")==0);
 reset();stagePi(124,"private");assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&a));
 sessions.erase({glasses.source,glasses.epoch});sessions.insert({glasses.source,99});
 assert(!dictationCommitTextFor(glasses.source,a,a.length));
 assert(!dictationPeekTextFor(glasses.source,text,sizeof(text),&b)&&!text[0]);
 assert(!gDict.textPending&&gDict.state==DictationState::IDLE);
 reset();stagePi(125,"old");assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&a));
 dictationClearDeliveryLocked();stagePi(126,"new");assert(!dictationCommitTextFor(glasses.source,a,a.length));
 reset();gDict.state=DictationState::WAITING;gDict.owner=127;gDict.requestHostEpoch=9;gDict.displaySource=glasses.source;gDict.displayEpoch=3;
 assert(std::string(dictDeliver(127,"wrong host",10,10)).find("Error:")==0&&!gDict.textPending);
#if !ENABLE_LOCAL_STT
 reset();stagePi(128,"rest");dictationFieldFullFor(glasses.source);
 assert(!gDict.textPending&&gDict.state==DictationState::FAILED&&strstr(gDict.failure,"field full"));
 reset();gDict.owner=129;gDict.state=DictationState::WAITING;gDict.requestHostEpoch=9;gDict.requestWasPushed=true;gDict.displaySource=glasses.source;gDict.displayEpoch=3;
 dictationFieldFullFor(glasses.source);assert(stops==std::vector<uint64_t>{129}&&cancelEvents==std::vector<uint64_t>{129}&&cleanup==std::vector<uint64_t>{129});
#endif
}
#if ENABLE_LOCAL_STT
static void ready(){const char* why=nullptr;assert(!dictationAvailable(&why));assert(readyChecks==0);dictationProcessLocal();assert(dictationAvailable(&why));assert(readyChecks==1);}
static void begin(){assert(dictationBeginLocal(glasses.source,glasses.epoch));assert(beginCalls==0);dictationProcessLocal();assert(beginCalls==1&&gLocalDict.token==42);broker.state=STTState::Recording;broker.captureActive=true;}
static void produce(uint32_t seq,const std::string& text){STTTextChunk chunk;chunk.sequence=seq;snprintf(chunk.text,sizeof(chunk.text),"%s",text.c_str());chunks.push_back(chunk);}
static void terminal(){broker.workerActive=broker.captureActive=broker.inferenceActive=false;broker.state=STTState::Done;}
static void testLocal(){
 char text[257];DictationTextReceipt receipt,old;
 reset();ready();for(int i=0;i<100;++i)assert(dictationAvailable(nullptr));assert(readyChecks==1);
 srBusy=true;const char* why=nullptr;assert(!dictationAvailable(&why)&&std::string(why)=="run closesr first");srBusy=false;
 gMicRunning=true;assert(!dictationAvailable(&why)&&std::string(why)=="run closemic first");gMicRunning=false;
 begin();assert(gDict.requestHostEpoch==0&&!gDict.requestWasPushed&&!gDict.cleanupOwner);
 produce(1,std::string(512,'a'));dictationProcessLocal();assert(gDict.state==DictationState::RECORDING&&gDict.textPending&&ackCalls==0);
 assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&receipt)&&receipt.length==256);
 old=receipt;assert(dictationCommitTextFor(glasses.source,receipt,100));assert(!dictationCommitTextFor(glasses.source,old,1));
 assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&receipt)&&receipt.offset==100&&receipt.length==156);
 assert(dictationCommitTextFor(glasses.source,receipt,156));dictationProcessLocal();assert(ackCalls==0);
 assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&receipt)&&receipt.offset==256&&receipt.length==256);
 assert(dictationCommitTextFor(glasses.source,receipt,256));assert(ackCalls==0&&gDict.deliveryAckPending);
 dictationProcessLocal();assert(ackCalls==1&&gDict.owner&&gDict.state==DictationState::RECORDING);
 produce(2,"");dictationProcessLocal();assert(gDict.deliveryAckPending&&!gDict.textPending);dictationProcessLocal();assert(ackCalls==2&&gDict.state==DictationState::RECORDING);
 produce(3,"last");dictationRequestStopFor(glasses.source);terminal();dictationProcessLocal();assert(gDict.state==DictationState::WAITING&&gLocalDict.exchange);
 assert(dictationTakeTextFor(glasses.source,text,sizeof(text))&&std::string(text)=="last");
 assert(gLocalDict.exchange);dictationProcessLocal();assert(ackCalls==3&&!gLocalDict.exchange&&gDict.state==DictationState::IDLE);
 // Final publication occurs after snapshot + empty read, before the active query.
 reset();ready();begin();runActiveHook=[] {produce(1,"late final text");terminal();};
 dictationProcessLocal();assert(gLocalDict.exchange&&gDict.owner&&chunks.size()==1&&cancelCalls==0);
 dictationProcessLocal();assert(dictationTakeTextFor(glasses.source,text,sizeof(text))&&std::string(text)=="late final text");
 dictationProcessLocal();assert(ackCalls==1&&chunks.empty()&&!gLocalDict.exchange&&gDict.state==DictationState::IDLE);
 // A terminal failure at the same boundary must not be reported as normal completion.
 reset();ready();begin();runActiveHook=[] {terminal();broker.state=STTState::Failed;snprintf(broker.error,sizeof(broker.error),"late inference failure");};
 dictationProcessLocal();assert(gDict.state==DictationState::FAILED&&strstr(gDict.failure,"late inference failure"));
 reset();ready();assert(dictationBeginLocal(glasses.source,glasses.epoch));dictationCancelLocal(glasses.source,false);assert(!dictationBeginLocal(glasses.source,glasses.epoch));dictationProcessLocal();assert(beginCalls==0&&!gLocalDict.exchange&&gDict.state==DictationState::IDLE);
 reset();ready();beginHook=[] {dictationCancelLocal(SOURCE_G2_GLASSES,false);};begin();assert(cancelCalls==1&&gLocalDict.exchange&&broker.workerActive);terminal();dictationProcessLocal();assert(!gLocalDict.exchange&&!gDict.textPending);
 reset();ready();begin();dictationCancelLocal(SOURCE_LOCAL_DISPLAY,false);dictationRequestStopFor(SOURCE_LOCAL_DISPLAY);dictationProcessLocal();assert(finishCalls==0&&gDict.owner);
 dictationRequestStopFor(glasses.source);dictationProcessLocal();assert(finishCalls==1&&gDict.state==DictationState::WAITING);
 reset();ready();begin();produce(1,"still private");dictationProcessLocal();assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&old));sessions.erase({glasses.source,3});assert(!dictationCommitTextFor(glasses.source,old,old.length));dictationProcessLocal();assert(gLocalDict.exchange&&!gDict.textPending);terminal();dictationProcessLocal();assert(!gLocalDict.exchange);
 reset();ready();begin();produce(1,"unseen tail");dictationProcessLocal();assert(dictationPeekTextFor(glasses.source,text,sizeof(text),&old));assert(dictationCommitTextFor(glasses.source,old,3));dictationFieldFullFor(glasses.source);assert(gDict.state==DictationState::FAILED&&!gDict.textPending&&strstr(gDict.failure,"field full")&&ackCalls==0);dictationProcessLocal();assert(cancelCalls==1);terminal();dictationProcessLocal();assert(!gLocalDict.exchange&&gDict.state==DictationState::FAILED);
 reset();ready();begin();produce(1,"ok");dictationProcessLocal();assert(dictationTakeTextFor(glasses.source,text,sizeof(text)));ackOK=false;dictationProcessLocal();assert(gDict.state==DictationState::FAILED&&strstr(gDict.failure,"acknowledgment"));
 reset();ready();begin();produce(1,"\n control");dictationProcessLocal();assert(gDict.state==DictationState::FAILED&&!gDict.textPending);
 reset();ready();beginOK=false;assert(dictationBeginLocal(glasses.source,glasses.epoch));dictationProcessLocal();assert(gDict.state==DictationState::FAILED&&strstr(gDict.failure,"closemic")&&!gLocalDict.exchange);
 reset();modelReady=false;assert(!dictationAvailable(&why));dictationProcessLocal();assert(!dictationAvailable(&why)&&!dictationBeginLocal(glasses.source,glasses.epoch));
}
#endif
int main(){testPiMailbox();
#if ENABLE_LOCAL_STT
 testLocal();
#endif
 printf("Shared Dictation delivery: Pi mailbox, partial commits, identity/replay fences, local=%d passed\n",ENABLE_LOCAL_STT);
}
