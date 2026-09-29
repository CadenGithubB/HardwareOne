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
#define ENABLE_DICTATION 1
#define INFO_SYSTEMF(...) ((void)0)
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
#define portENTER_CRITICAL(x) ((void)x, ++criticalDepth)
#define portEXIT_CRITICAL(x) ((void)x, assert(criticalDepth > 0), --criticalDepth)
static unsigned criticalDepth=0;
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
static bool transportSessionEpochIsLive(CommandSource s,uint32_t epoch){
 if(sessionHook){auto hook=sessionHook;sessionHook=nullptr;hook();}
 return sessions.count({s,epoch});
}
static TransportSessionEpoch localDisplayTransportSessionSnapshot(String& user,bool& authed){
 if(sessionHook){auto hook=sessionHook;sessionHook=nullptr;hook();}
 for(auto session:sessions) if(session.first==SOURCE_LOCAL_DISPLAY){user="wearer";authed=true;return session.second;}
 user.clear();authed=false;return 0;
}
static void secureClearString(String& s){s.clear();}
// INSERT_LIVE
// The real sink has its own filesystem tests. This spy enforces the Dictation
// seam: capture only snapshots admission metadata; writes run on its worker and
// still belong to that original live identity at the first-write boundary.
static bool savePreference=false, inTranscriptWorker=false, transcriptWriteOK=true;
static std::string transcriptUser="wearer";
static unsigned transcriptCaptureCalls=0,transcriptAppendCalls=0,transcriptFinishCalls=0;
static std::function<void()> transcriptAppendHook;
struct SavedTranscript { TranscriptOptions options; uint64_t id; uint32_t sequence; std::string provider,text; };
static std::vector<SavedTranscript> savedTranscripts;
TranscriptOptions transcriptCaptureOptions(CommandSource source,TransportSessionEpoch epoch){
 assert(criticalDepth==0);++transcriptCaptureCalls;
 TranscriptOptions out;out.enabled=savePreference;out.source=source;out.epoch=epoch;
 if(sessions.count({source,epoch}))snprintf(out.user,sizeof(out.user),"%s",transcriptUser.c_str());
 return out;
}
void TranscriptSession::begin(const TranscriptOptions& options,const char* provider,uint64_t id){
 options_=options;status_={};status_.enabled=options.enabled;id_=id;sequence_=0;finished_=false;
 snprintf(provider_,sizeof(provider_),"%s",provider);
}
bool TranscriptSession::append(uint32_t sequence,const char* text,size_t length){
 assert(inTranscriptWorker&&criticalDepth==0);++transcriptAppendCalls;
 if(transcriptAppendHook){auto hook=transcriptAppendHook;transcriptAppendHook=nullptr;hook();}
 if(!options_.enabled)return true;
 if(!options_.user[0]||!sessions.count({options_.source,options_.epoch})||transcriptUser!=options_.user){
  snprintf(status_.error,sizeof(status_.error),"original session changed");return false;
 }
 if(!transcriptWriteOK){snprintf(status_.error,sizeof(status_.error),"storage unavailable");return false;}
 if(sequence==sequence_)return true;
 assert(sequence==sequence_+1);sequence_=sequence;
 savedTranscripts.push_back({options_,id_,sequence,provider_,std::string(text,length)});
 status_.saved=true;++status_.chunks;status_.bytes+=length;
 snprintf(status_.path,sizeof(status_.path),"/users/wearer/transcripts/test.txt");return true;
}
void TranscriptSession::finish(const char*){
 assert(inTranscriptWorker&&criticalDepth==0);++transcriptFinishCalls;finished_=true;
 status_.complete=status_.error[0]==0;
}
static bool micBusy=false,srBusy=false;
bool gMicRunning=false;
bool micRecordingBusy(){return micBusy;}
bool isESPSRRunning(){return srBusy;}
static std::string halOwner;
bool audioCaptureOwnedBy(const char* o){return halOwner==o;}
bool audioCaptureBusy(){return !halOwner.empty();}
static constexpr uint32_t kDictationVadSilenceMs=1200;
struct MicRecordingResult { bool failed=false; char failure[48]={}; };
enum class MicRecordingOwnedOp { OK, NOT_FOUND };
enum AudioSource { AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
static AudioSource audioGetSource(){return AUDIO_SRC_LOCAL_PDM;}
static int getAudioLevel(){return 0;}
static const char* sourceLabel(AudioSource){return "PDM";}
static bool initMicrophone(){gMicRunning=true;return true;}
static bool startRecordingOwned(uint64_t,uint32_t,bool){return true;}
static MicRecordingOwnedOp getRecordingResultOwned(uint64_t,MicRecordingResult*){return MicRecordingOwnedOp::NOT_FOUND;}
static void dictationResolveCleanup(uint64_t){}
static void dictFormatId(uint64_t id,char out[17]){snprintf(out,17,"%016llx",static_cast<unsigned long long>(id));}
static bool modelReady=true;
static int readyChecks=0;
bool sttLocalAvailable(char*,size_t){++readyChecks;return modelReady;}
static int beginCalls=0,cancelCalls=0,finishCalls=0,ackCalls=0;
static bool beginOK=true,ackOK=true;
static STTOwner brokerOwner;
static TranscriptOptions brokerTranscriptOptions;
static STTSnapshot broker;
static std::deque<STTTextChunk> chunks;
static std::function<void()> beginHook,ackHook,runActiveHook;
bool sttBeginContinuous(STTOwner owner,STTToken* token,char* error,size_t cap,const TranscriptOptions* options){
 assert(options);brokerTranscriptOptions=*options;
 ++beginCalls;if(beginHook)beginHook();
 if(!beginOK){snprintf(error,cap,"run closemic first");return false;}
 brokerOwner=owner;broker=STTSnapshot{};broker.token=42;broker.continuous=true;
 broker.state=STTState::Preparing;broker.workerActive=true;broker.transcript.enabled=options->enabled;*token=42;return true;
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
// INSERT_BEGIN
static constexpr uint32_t MIC_STT_MAX_CAPTURE_MS=30000;
static uint32_t hostEpoch=9;
static bool hostRunning=true,hostReady=true;
static uint32_t uartLinkSessionEpoch(){return hostEpoch;}
static bool uartLinkIsRunning(){return hostRunning;}
static bool dictationHostReadyForEpoch(uint32_t epoch,const char**){return hostReady&&epoch==hostEpoch;}
// INSERT_SAVE
static void runSaveWorker(){assert(!inTranscriptWorker);inTranscriptWorker=true;dictationProcessSave();inTranscriptWorker=false;}
static STTOwner glasses{SOURCE_G2_GLASSES,3};
static void reset(){
 gDict=DictationControl{};gDict.state=DictationState::IDLE;gDictSave=DictationSaveJob{};brokerTranscriptOptions={};
#if ENABLE_LOCAL_STT
 gLocalDict=LocalDictation{};gLocalReadyKnown=gLocalReady=gLocalReadyPending=false;gLocalReadyCheckedMs=0;
#endif
 assert(criticalDepth==0);savePreference=inTranscriptWorker=false;transcriptWriteOK=true;
 transcriptUser="wearer";transcriptCaptureCalls=transcriptAppendCalls=transcriptFinishCalls=0;
 transcriptAppendHook=nullptr;savedTranscripts.clear();
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
#if !ENABLE_LOCAL_STT
static uint64_t beginPi(){
 gMicRunning=true; // An already-open mic keeps unrelated stop debt out of these tests.
 assert(dictationBeginFor(glasses.source,glasses.epoch));
 assert(gDict.state==DictationState::RECORDING&&gDict.owner&&transcriptAppendCalls==0);
 const uint64_t id=gDict.owner;gDict.state=DictationState::WAITING;gDict.requestHostEpoch=9;return id;
}
static void acceptPi(uint64_t id,const std::string& text){
 assert(std::string(dictDeliver(id,text.c_str(),text.size(),9)).find("OK:")==0);
 assert(transcriptAppendCalls==0); // UART acceptance never writes a file.
}
static void drainPi(){
 char out[257];assert(dictationTakeTextFor(glasses.source,out,sizeof(out)));
 assert(gDict.state==DictationState::IDLE&&!gDict.textPending);
}
static void testPiSaving(){
 // Real admission captures the preference once; later toggles affect only new sessions.
 reset();assert(!dictationBeginFor(SOURCE_SERIAL,3));assert(!dictationBeginFor(glasses.source,0));
 uint64_t id=beginPi();assert(transcriptCaptureCalls==1&&!gDict.transcriptOptions.enabled);
 savePreference=true;acceptPi(id,"off at admission");assert(!gDictSave.pending);drainPi();runSaveWorker();
 assert(savedTranscripts.empty()&&transcriptAppendCalls==0);

 reset();savePreference=true;id=beginPi();savePreference=false;
 assert(gDict.transcriptOptions.enabled&&gDict.transcriptStatus.enabled);
 acceptPi(id,"accepted before cancel");
 assert(gDictSave.pending&&!gDictSave.inFlight&&gDictSave.exchange==id&&dictationWorkerHasWork());
 assert(gDictSave.options.enabled&&gDictSave.options.source==glasses.source&&gDictSave.options.epoch==glasses.epoch);
 assert(std::string(gDictSave.options.user)=="wearer"&&std::string(gDictSave.text)=="accepted before cancel");
 assert(std::string(dictDeliver(id,"replay",6,9)).find("Error:")==0);
 dictationCancelImpl(glasses.source,false); // UI cancellation cannot discard an accepted save.
 assert(!gDict.textPending&&gDictSave.pending);
 assert(!dictationBeginFor(glasses.source,glasses.epoch));
 runSaveWorker();assert(savedTranscripts.size()==1&&savedTranscripts[0].id==id);
 assert(savedTranscripts[0].text=="accepted before cancel"&&savedTranscripts[0].provider=="pi");
 assert(savedTranscripts[0].options.source==glasses.source&&savedTranscripts[0].options.user==std::string("wearer"));
 assert(!gDictSave.pending&&!gDictSave.inFlight&&!gDictSave.exchange&&!gDictSave.text[0]);
 assert(gDict.transcriptStatus.saved&&gDict.transcriptStatus.complete);
 auto publicSnapshot=dictationSnapshotNow();
 assert(publicSnapshot.transcript.saved&&!publicSnapshot.transcript.path[0]&&gDict.transcriptStatus.path[0]);
 runSaveWorker();assert(savedTranscripts.size()==1&&transcriptAppendCalls==1);

 // The independent save slot survives a complete UI drain and remains an
 // admission fence through the entire blocking append, including reentry.
 reset();savePreference=true;id=beginPi();acceptPi(id,"already consumed by field");drainPi();
 assert(gDictSave.pending&&!dictationBeginFor(glasses.source,glasses.epoch));
 transcriptAppendHook=[&]{
  assert(!gDictSave.pending&&gDictSave.inFlight&&dictationWorkerHasWork());
  assert(!dictationBeginFor(glasses.source,glasses.epoch));
  dictationProcessSave(); // A second worker pass cannot claim an in-flight job.
  assert(transcriptAppendCalls==1);
 };
 runSaveWorker();assert(savedTranscripts.size()==1&&transcriptFinishCalls==1);
 assert(dictationBeginFor(glasses.source,glasses.epoch));

 // Original user/epoch is checked at the deferred first write, never replaced
 // by the UART actor or by whoever happens to own the UI at worker time.
 reset();savePreference=true;id=beginPi();acceptPi(id,"revoked before first write");drainPi();
 sessions.erase({glasses.source,glasses.epoch});sessions.insert({glasses.source,99});
 runSaveWorker();assert(savedTranscripts.empty()&&gDict.transcriptStatus.error[0]);
 reset();savePreference=true;id=beginPi();acceptPi(id,"renamed identity");drainPi();transcriptUser="different-user";
 runSaveWorker();assert(savedTranscripts.empty()&&gDict.transcriptStatus.error[0]);

 // A storage failure does not revoke an accepted, still-readable UI result.
 reset();savePreference=true;id=beginPi();acceptPi(id,"display remains available");transcriptWriteOK=false;
 runSaveWorker();assert(savedTranscripts.empty()&&gDict.transcriptStatus.error[0]&&gDict.textPending);
 drainPi();assert(gDict.state==DictationState::IDLE);

 // Maximum admitted result remains a single bounded job; queue copy owns its bytes.
 reset();savePreference=true;id=beginPi();std::string longest(DICTATION_MAX_TEXT,'x');acceptPi(id,longest);
 longest.assign(DICTATION_MAX_TEXT,'z');drainPi();runSaveWorker();
 assert(savedTranscripts.size()==1&&savedTranscripts[0].text==std::string(DICTATION_MAX_TEXT,'x'));
}
#endif
#if ENABLE_LOCAL_STT
static void ready(){const char* why=nullptr;assert(!dictationAvailable(&why));assert(readyChecks==0);dictationProcessLocal();assert(dictationAvailable(&why));assert(readyChecks==1);}
static void begin(){assert(dictationBeginLocal(glasses.source,glasses.epoch));assert(beginCalls==0);dictationProcessLocal();assert(beginCalls==1&&gLocalDict.token==42);broker.state=STTState::Recording;broker.captureActive=true;}
static void produce(uint32_t seq,const std::string& text){STTTextChunk chunk;chunk.sequence=seq;snprintf(chunk.text,sizeof(chunk.text),"%s",text.c_str());chunks.push_back(chunk);}
static void terminal(){broker.workerActive=broker.captureActive=broker.inferenceActive=false;broker.state=STTState::Done;}
static void testLocalSaving(){
 reset();ready();savePreference=true;
 assert(dictationBeginFor(glasses.source,glasses.epoch));
 assert(transcriptCaptureCalls==1&&gLocalDict.transcriptOptions.enabled);
 savePreference=false;dictationProcessLocal();
 assert(beginCalls==1&&brokerTranscriptOptions.enabled&&brokerTranscriptOptions.source==glasses.source);
 assert(brokerTranscriptOptions.epoch==glasses.epoch&&std::string(brokerTranscriptOptions.user)=="wearer");
 broker.state=STTState::Recording;broker.captureActive=true;produce(1,"broker already saved this");
 broker.transcript.saved=true;broker.transcript.chunks=1;broker.transcript.bytes=24;
 snprintf(broker.transcript.path,sizeof(broker.transcript.path),"/private/broker-path");
 dictationProcessLocal();char text[257];assert(dictationTakeTextFor(glasses.source,text,sizeof(text)));
 dictationProcessLocal();assert(ackCalls==1&&gDict.transcriptStatus.saved&&gDict.transcriptStatus.chunks==1);
 auto publicSnapshot=dictationSnapshotNow();
 assert(publicSnapshot.transcript.saved&&!publicSnapshot.transcript.path[0]&&broker.transcript.path[0]);
 assert(!gDictSave.pending&&!gDictSave.inFlight);runSaveWorker();
 assert(savedTranscripts.empty()&&transcriptAppendCalls==0); // Local backend owns saving exactly once.
 terminal();dictationProcessLocal();assert(!gLocalDict.exchange);
 reset();ready();assert(dictationBeginFor(glasses.source,glasses.epoch));
 savePreference=true;dictationProcessLocal();assert(!brokerTranscriptOptions.enabled&&transcriptCaptureCalls==1);
 // Pending/in-flight shared job debt cannot be overwritten even in a local build.
 reset();ready();gDictSave.pending=true;gDictSave.exchange=51;
 assert(!dictationBeginFor(glasses.source,glasses.epoch));
 gDictSave.pending=false;gDictSave.inFlight=true;
 assert(!dictationBeginFor(glasses.source,glasses.epoch));
}
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
static void testAppAdmission() {
 reset();
 assert(displaySessionStillLive(SOURCE_LOCAL_DISPLAY,4));
 assert(!displaySessionStillLive(SOURCE_LOCAL_DISPLAY,3));
 assert(displaySessionStillLive(SOURCE_G2_GLASSES,3));
 sessions.insert({SOURCE_SERIAL,7});
 assert(!displaySessionStillLive(SOURCE_SERIAL,7));
 assert(!displaySessionStillLive(SOURCE_WEB,0)); sessions.insert({SOURCE_WEB,7});
#if ENABLE_LOCAL_STT
 ready();
#endif
 DictationAppLease app;
 assert(!dictationBeginFor(SOURCE_WEB,7));
 assert(!dictationAppBegin(SOURCE_SERIAL,7,&app));
 assert(!dictationAppBegin(SOURCE_WEB,8,&app));
 assert(!dictationAppBegin(SOURCE_WEB,7,nullptr));
 assert(dictationAppBegin(SOURCE_WEB,7,&app));
 assert(app.source==SOURCE_WEB&&app.epoch==7&&app.exchange==gDict.owner);
 assert(gDict.appConsumer&&gDict.appLease.exchange==app.exchange);
 DictationAppLease second;
 assert(!dictationAppBegin(SOURCE_WEB,7,&second)&&second.exchange==0);
 assert(!dictationBeginFor(SOURCE_LOCAL_DISPLAY,4));
}
static void testAppControls() {
 reset(); sessions.insert({SOURCE_WEB,7});
#if ENABLE_LOCAL_STT
 ready();
#endif
 DictationAppLease app;assert(dictationAppBegin(SOURCE_WEB,7,&app));
 // Same transport or even same session cannot use keyboard control wrappers.
 dictationRequestStopFor(SOURCE_WEB);dictationCancelImpl(SOURCE_WEB,false);
 dictationFieldFullFor(SOURCE_WEB);
 assert(gDict.owner==app.exchange&&!gDict.stopRequested&&stops.empty());
 auto other=app;other.epoch=8;sessions.insert({SOURCE_WEB,8});
 assert(!dictationAppRequestStop(other)&&!dictationAppCancel(other));
 other=app;other.exchange++;assert(!dictationAppCancel(other));
 assert(dictationAppRequestStop(app));
#if ENABLE_LOCAL_STT
 assert(gDict.stopRequested);dictationProcessLocal();
#else
 assert(stops==std::vector<uint64_t>{app.exchange});
#endif
 sessions.erase({SOURCE_WEB,7});assert(!dictationAppCancel(app));
 sessions.insert({SOURCE_WEB,7});assert(dictationAppCancel(app));
 assert(!gDict.owner&&gDict.state==DictationState::IDLE);
 // A retained terminal lease is not a keyboard handle.
 assert(!dictationCancelImpl(SOURCE_WEB,false));
}
static void testAppDelivery() {
 reset();sessions.insert({SOURCE_WEB,7});sessions.insert({SOURCE_WEB,8});
 DictationAppLease app;dictationAdmitConsumerLocked(SOURCE_WEB,7,123,&app);
 gDict.displaySource=SOURCE_WEB;gDict.displayEpoch=7;gDict.state=DictationState::WAITING;
 dictationStageTextLocked(123,1,"abcdef",6,0,false);
 char text[257];DictationTextReceipt a,b;
 assert(!dictationPeekTextFor(SOURCE_WEB,text,sizeof(text),&a));
 auto other=app;other.epoch=8;assert(!dictationAppPeekText(other,text,sizeof(text),&a));
 assert(dictationAppPeekText(app,text,4,&a)&&std::string(text)=="abc");
 assert(dictationAppPeekText(app,text,4,&b)&&a.offset==b.offset);
 assert(!dictationCommitTextFor(SOURCE_WEB,a,3));
 assert(!dictationAppCommitText(other,a,3));
 assert(dictationAppCommitText(app,a,2));
 assert(dictationAppCommitText(app,a,2)); // HTTP ACK reply lost: no second consume.
 assert(!dictationAppCommitText(app,a,3));
 assert(dictationAppPeekText(app,text,sizeof(text),&b)&&std::string(text)=="cdef");
 assert(dictationAppCommitText(app,b,4));
 assert(!dictationAppCommitText(app,a,2)); // Superseded receipt is stale.
 assert(dictationAppCommitText(app,b,4)); // Exact retry survives terminal drain.
 assert(gDict.state==DictationState::IDLE&&!gDict.textPending);
 sessions.erase({SOURCE_WEB,7});assert(!dictationAppCommitText(app,b,4));
 sessions.insert({SOURCE_WEB,7});
 dictationAdmitConsumerLocked(SOURCE_WEB,7,124,&other);
 assert(!dictationAppCommitText(app,b,4));
}
static void testAppSnapshotAndRecovery() {
 reset();sessions.insert({SOURCE_WEB,7});sessions.insert({SOURCE_WEB,8});
 DictationAppLease app;dictationAdmitConsumerLocked(SOURCE_WEB,7,123,&app);
 gDict.displaySource=SOURCE_WEB;gDict.displayEpoch=7;gDict.owner=123;
 gDict.transcriptExchange=123;gDict.transcriptStatus.enabled=true;
 snprintf(gDict.transcriptStatus.path,sizeof(gDict.transcriptStatus.path),"/stt/u1/private.txt");
 DictationAppLease recovered;assert(dictationAppCurrent(SOURCE_WEB,7,&recovered));
 assert(recovered.exchange==123);
 assert(!dictationAppCurrent(SOURCE_WEB,8,&recovered)&&recovered.exchange==0);
 DictationAppSnapshot snap;
 assert(!dictationAppSnapshot({SOURCE_WEB,8,0},&snap)&&snap.busy&&!snap.valid);
 assert(!snap.status.transcript.path[0]&&!snap.status.transcript.enabled);
 assert(dictationAppSnapshot(app,&snap)&&snap.active&&!snap.done&&snap.valid);
 assert(std::string(snap.status.transcript.path)=="/stt/u1/private.txt");
 gDict.owner=0;gDict.displaySource=SOURCE_INTERNAL;gDict.displayEpoch=0;
 gDictSave.pending=true;gDictSave.exchange=123;
 assert(dictationAppSnapshot(app,&snap)&&snap.active); // Saving survives delivery drain.
 gDictSave.pending=false;gDictSave.inFlight=true;
 assert(dictationAppSnapshot(app,&snap)&&snap.active);
 gDictSave={};assert(dictationAppSnapshot(app,&snap)&&snap.done&&!snap.active);
 assert(dictationAppCurrent(SOURCE_WEB,7,&recovered)&&recovered.exchange==123);
 sessionHook=[] {sessions.erase({SOURCE_WEB,7});};
 assert(!dictationAppSnapshot(app,&snap)&&!snap.status.transcript.path[0]);
 sessions.insert({SOURCE_WEB,7});
 dictationAdmitConsumerLocked(SOURCE_LOCAL_DISPLAY,4,124,nullptr);
 assert(!dictationAppCurrent(SOURCE_WEB,7,&recovered)&&!recovered.exchange);
 assert(!dictationAppSnapshot(app,&snap)&&!snap.status.transcript.path[0]);
}
static void testWorkerSupervision() {
#if !ENABLE_LOCAL_STT
 reset();sessions.insert({SOURCE_WEB,7});hostEpoch=9;hostRunning=hostReady=true;
 DictationAppLease app;assert(dictationAppBegin(SOURCE_WEB,7,&app));
 assert(dictationWorkerHasWork());nowMs+=MIC_STT_MAX_CAPTURE_MS;
 const auto wakes=wakeCount;dictationSupervise();assert(wakeCount==wakes);
 assert(stops==std::vector<uint64_t>{app.exchange});
 sessions.erase({SOURCE_WEB,7});dictationSupervise();
 assert(!gDict.owner&&gDict.state==DictationState::FAILED&&cleanup.back()==app.exchange);
 reset();sessions.insert({SOURCE_WEB,7});assert(dictationAppBegin(SOURCE_WEB,7,&app));
 gDict.state=DictationState::WAITING;gDict.requestHostEpoch=9;gDict.requestWasPushed=true;
 nowMs+=90000;dictationSupervise();
 assert(!gDict.owner&&gDict.state==DictationState::FAILED&&cancelEvents.back()==app.exchange);
 reset();sessions.insert({SOURCE_WEB,7});assert(dictationAppBegin(SOURCE_WEB,7,&app));
 gDict.state=DictationState::WAITING;gDict.requestHostEpoch=9;gDict.requestWasPushed=true;
 hostEpoch=10;dictationSupervise();
 assert(!gDict.owner&&std::string(gDict.failure)=="host session lost");hostEpoch=9;
 reset();sessions.insert({SOURCE_WEB,7});assert(dictationAppBegin(SOURCE_WEB,7,&app));
 gDict.owner=0;gDict.state=DictationState::WAITING;
 dictationStageTextLocked(app.exchange,1,"private",7,0,false);
 assert(dictationWorkerHasWork());sessions.erase({SOURCE_WEB,7});dictationSupervise();
 assert(!gDict.textPending&&!gDict.deliveryExchange&&gDict.state==DictationState::IDLE);
#endif
}
static void testAppLocalLifecycle() {
#if ENABLE_LOCAL_STT
 for(auto source:{SOURCE_LOCAL_DISPLAY,SOURCE_G2_GLASSES,SOURCE_WEB}) {
  reset();sessions.insert({SOURCE_WEB,7});ready();
  const uint32_t epoch=source==SOURCE_LOCAL_DISPLAY?4:source==SOURCE_G2_GLASSES?3:7;
  DictationAppLease app;assert(dictationAppBegin(source,epoch,&app));
  dictationProcessLocal();broker.state=STTState::Recording;broker.captureActive=true;
  dictationCancelImpl(source,false);dictationFieldFullFor(source);dictationRequestStopFor(source);
  assert(gDict.owner==app.exchange&&!gDict.stopRequested);
  produce(1,std::string(512,'a'));dictationProcessLocal();
  char text[257];DictationTextReceipt receipt;
  assert(!dictationPeekTextFor(source,text,sizeof(text),&receipt));
  assert(dictationAppPeekText(app,text,sizeof(text),&receipt)&&receipt.length==256);
  assert(dictationAppCommitText(app,receipt,256));assert(dictationAppCommitText(app,receipt,256));
  dictationProcessLocal();assert(ackCalls==0);
  assert(dictationAppPeekText(app,text,sizeof(text),&receipt)&&receipt.offset==256);
  assert(dictationAppCommitText(app,receipt,256));dictationProcessLocal();assert(ackCalls==1);
  produce(2,"");dictationProcessLocal();dictationProcessLocal();assert(ackCalls==2);
  assert(dictationAppRequestStop(app));produce(3,"last");terminal();dictationProcessLocal();
  DictationAppSnapshot snap;assert(dictationAppSnapshot(app,&snap)&&snap.active&&snap.textPending);
  assert(dictationAppPeekText(app,text,sizeof(text),&receipt)&&std::string(text)=="last");
  assert(dictationAppCommitText(app,receipt,receipt.length));dictationProcessLocal();
  assert(ackCalls==3&&dictationAppSnapshot(app,&snap)&&snap.done&&!snap.active);
  assert(dictationAppCommitText(app,receipt,receipt.length)); // Last ACK response lost after worker retirement.
  auto old=app;assert(dictationAppBegin(source,epoch,&app));
  assert(app.exchange!=old.exchange&&!dictationAppCancel(old));
  assert(!dictationAppCommitText(old,receipt,receipt.length));
  sessions.erase({source,epoch});dictationProcessLocal();
  assert(!dictationAppSnapshot(app,&snap)&&!snap.status.transcript.path[0]);
 }
#endif
}
static void testConsumerIdentity() {
 reset(); gDict.displaySource=SOURCE_LOCAL_DISPLAY;
 assert(dictationConsumerMatchesLocked(SOURCE_LOCAL_DISPLAY));
 DictationAppLease app;
 dictationAdmitConsumerLocked(SOURCE_WEB,7,100,&app);
 assert(app.source==SOURCE_WEB && app.epoch==7 && app.exchange==100);
 assert(dictationConsumerMatchesLocked(SOURCE_WEB,&app));
 assert(!dictationConsumerMatchesLocked(SOURCE_WEB));
 auto other=app; other.epoch=8; assert(!dictationConsumerMatchesLocked(SOURCE_WEB,&other));
 other=app;other.exchange=99;assert(!dictationConsumerMatchesLocked(SOURCE_WEB,&other));
 other=app;other.source=SOURCE_LOCAL_DISPLAY;assert(!dictationConsumerMatchesLocked(SOURCE_LOCAL_DISPLAY,&other));
 dictationAdmitConsumerLocked(SOURCE_WEB,7,101,&other);
 assert(!dictationConsumerMatchesLocked(SOURCE_WEB,&app));
 dictationAdmitConsumerLocked(SOURCE_LOCAL_DISPLAY,4,102,nullptr);
 assert(!dictationConsumerMatchesLocked(SOURCE_WEB,&other));
 assert(dictationConsumerMatchesLocked(SOURCE_LOCAL_DISPLAY));
}
int main(){testAppLocalLifecycle();testWorkerSupervision();testAppSnapshotAndRecovery();testAppDelivery();testAppControls();testAppAdmission();testConsumerIdentity();testPiMailbox();
#if ENABLE_LOCAL_STT
 testLocalSaving();testLocal();
#else
 testPiSaving();
#endif
 printf("Shared Dictation: admission save latch, worker-only writes, drain/cancel/replay fences, local=%d passed\n",ENABLE_LOCAL_STT);
}
