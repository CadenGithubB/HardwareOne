#include "G2_NativeNotes.h"
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_QUICKNOTE && ENABLE_LOCAL_STT
#include "G2_NativeNotesProtocol.h"
#include "G2_Glasses.h"
#include "QuickNote_Store.h"
#include "System_QuickNote.h"
#include "System_STT.h"
#include "System_Transcript.h"
#include "System_TaskUtils.h"
#include "System_VFS.h"
#include "System_Debug.h"
#include "Transcription_UI_Policy.h"
#include <esp_attr.h>
#include <esp_random.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {
using namespace G2Notes;
struct Request { G2NoteContext context; Event event; };
constexpr unsigned Depth=8;
static StaticQueue_t requestControl, ackControl;
EXT_RAM_BSS_ATTR static uint8_t requests[Depth*sizeof(Request)], acks[Depth*sizeof(Request)];
static QueueHandle_t requestQueue=nullptr, ackQueue=nullptr;
static TaskHandle_t task=nullptr;  // guarded by taskMux; nulled only by the worker itself
static portMUX_TYPE taskMux=portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> active{false}, overflow{false}, stopWanted{false};
// Single worker owns all private text and filesystem work. STT owns audio.
struct State {
  G2NoteContext context{};
  STTOwner owner{}; STTToken token=0;
  DraftState draft{};
  uint32_t magic=0, session=0, started=0, stopped=0, touched=0, duration=0;
  uint32_t lastDraftMagic=0, lastVoiceMagic=0;
  bool haveDraftMagic=false, haveVoiceMagic=false;
  bool connected=false, sd=false, storageDirty=false, storageReady=false;
  uint32_t sdMount=0, storageChecked=0;
  QuickNoteStore::Page listing{};
  char text[QuickNotePolicy::kMaxNoteBytes+QuickNotePolicy::kHeaderMax+1]={};
  char path[QuickNotePolicy::kPathMax]={};
  char window[QuickNoteStore::kReadWindow+1]={};
  char title[QuickNotePolicy::kTitleMax+1]={};
  char id[17]={};
  uint32_t draftActions[16]={}, voiceActions[16]={};
  unsigned draftActionCount=0, voiceActionCount=0;
  size_t textLength=0;
  uint8_t body[WireLimit]={}, wire[WireLimit]={}, item[512]={};
  STTSnapshot speech{}; STTTextChunk chunk{};
};
EXT_RAM_BSS_ATTR static State s;

bool same(const G2NoteContext& a,const G2NoteContext& b) {
  return a.epoch && a.epoch==b.epoch && a.left==b.left && a.right==b.right && !strcmp(a.user,b.user);
}
// A stop request ends every remaining wire write and ack wait at once, so a
// release never waits out the 2 s ack timeout against an owner that is gone.
bool live() {return s.connected && !stopWanted.load() && g2NativeNotesContextLive(s.context);}
TaskHandle_t taskHandle() {portENTER_CRITICAL(&taskMux);const TaskHandle_t t=task;portEXIT_CRITICAL(&taskMux);return t;}
AuthContext auth() {AuthContext a{};a.transport=SOURCE_G2_GLASSES;a.user=s.context.user;return a;}
bool checkStorage(bool force=false) {
  const uint32_t now=millis();
  // A failed card write probe performs I/O. Do not repeat it on every 50ms
  // speech pass; commands still check immediately before their operations.
  if(!force && uint32_t(now-s.storageChecked)<1000)return true;
  s.storageChecked=now;
  const bool onSD=VFS::isSDWritable();
  const uint32_t mount=VFS::sdMountGeneration();
  const bool changed=onSD!=s.sd || (onSD && mount!=s.sdMount);
  if(changed) {
    s.sd=onSD;s.storageDirty=true;s.listing={};s.id[0]=s.path[0]=0;
    memset(s.text,0,sizeof(s.text));s.textLength=0;
    s.storageReady=false;
  }
  s.sdMount=mount;
  const char* why=nullptr;
  const bool ready=QuickNoteStore::ensureG2Folders(auth(),s.sd,&why,!s.storageReady);
  s.storageReady=ready;
  return ready;
}
void clearDraft() {
  if(s.token) (void)sttCancel(s.owner,s.token);
  s.token=0;s.draft.clear();s.duration=0;s.chunk={};active.store(false);
}
void reset() {
  clearDraft(); s.context={};s.connected=false;s.storageDirty=false;s.storageReady=false;s.listing={};s.textLength=0;
  memset(s.text,0,sizeof(s.text));memset(s.path,0,sizeof(s.path));memset(s.title,0,sizeof(s.title));
  memset(s.id,0,sizeof(s.id));memset(s.body,0,sizeof(s.body));memset(s.wire,0,sizeof(s.wire));
  s.haveDraftMagic=s.haveVoiceMagic=false;
  s.draftActionCount=s.voiceActionCount=0;
}
bool send(uint32_t command,uint32_t field,Writer& body) {
  if(!live()) return false;
  if(!++s.magic) ++s.magic;
  const size_t length=wrap(command,s.magic,field,body,s.wire,sizeof(s.wire));
  if(!length || !g2NativeNotesWireSend(s.context,s.wire,length)) return false;
  const uint32_t start=millis(); Request ack;
  while(live() && uint32_t(millis()-start)<2000) {
    if(xQueueReceive(ackQueue,&ack,pdMS_TO_TICKS(25))==pdTRUE && same(ack.context,s.context) &&
       ack.event.magic==s.magic && ack.event.acked==command) return !ack.event.error;
  }
  WARN_SYSTEMF("[G2-NOTES] response timeout cmd=%lu",static_cast<unsigned long>(command));
  return false;
}
bool control(uint32_t command,uint32_t error=0) {
  Writer b{s.body,sizeof(s.body)};b.integer(1,command);b.integer(2,0);b.integer(3,error);b.bytes(4,nullptr,0);
  return send(Control,6,b);
}
void fail(const char* why) {
  WARN_SYSTEMF("[G2-NOTES] %s",why);clearDraft();
  // NOTE_CTRL_CLOSE is in the stock NoteCtrlCmd schema; never fake a saved note.
  (void)control(2);s.connected=false;
}
bool asr(bool final) {
  Writer b{s.body,sizeof(s.body)};b.integer(1,0);b.bytes(2,s.draft.text,s.draft.length);
  b.integer(3,lineCount(s.draft.text,s.draft.length));b.integer(4,final?1:0);return send(Asr,13,b);
}
void idText(uint64_t id,char out[17]) {
  snprintf(out,17,"%08lx%08lx",static_cast<unsigned long>(id>>32),static_cast<unsigned long>(id&0xffffffff));
}
bool listAt(uint32_t offset,const Event& req) {
  const char* why=nullptr;
  if(!live() || !QuickNoteStore::list(auth(),0,s.sd,offset,&s.listing,&why,true)) return false;
  Writer b{s.body,sizeof(s.body)}; b.integer(1,++s.session);b.integer(2,1);
  b.integer(3,req.direction);b.integer(4,s.listing.count?0:1);
  for(size_t i=0;i<s.listing.count;++i) {
    const auto& row=s.listing.rows[i];char id[17];idText(row.header.id,id);
    Writer item{s.item,sizeof(s.item)};item.bytes(1,id,16);
    char time[6]={};
    if(strlen(row.header.created)==16)snprintf(time,sizeof(time),"%.2s:%.2s",row.header.created+9,row.header.created+11);
    item.bytes(2,time,strlen(time));item.integer(3,0);
    item.bytes(4,row.header.title,strlen(row.header.title));item.bytes(5,nullptr,0);
    if(!item.ok)return false;
    b.bytes(7,item.p,item.size);
  }
  b.integer(8,0);b.bytes(9,req.id,req.hasId?16:0);b.integer(10,req.request);
  return send(List,3,b);
}
bool find(const char* id) {
  if(!id || strlen(id)!=16)return false;
  uint64_t number=0;if(!QuickNotePolicy::noteParseId(id,16,&number))return false;
  const char* why=nullptr;
  // The UI lists at most the existing store's 500-note bound. Never accepts a path from BLE.
  for(uint32_t offset=0;offset<QuickNoteStore::kMaxNames;offset+=QuickNoteStore::kPageRows) {
    if(!live() || !QuickNoteStore::list(auth(),0,s.sd,offset,&s.listing,&why,true))return false;
    for(size_t i=0;i<s.listing.count;++i) if(s.listing.rows[i].header.id==number) {
      snprintf(s.path,sizeof(s.path),"%s",s.listing.rows[i].path);
      snprintf(s.title,sizeof(s.title),"%s",s.listing.rows[i].header.title);return true;
    }
    if(!s.listing.more)break;
  }
  return false;
}
bool readNote(const char* id) {
  if(!find(id))return false;
  size_t got=0, used=0;uint32_t total=0;const char* why=nullptr;
  do {
    if(!live() || !QuickNoteStore::read(auth(),s.path,used,s.window,sizeof(s.window),&got,&total,&why) ||
       total>=sizeof(s.text) || used+got>=sizeof(s.text) || (!got&&used<total))return false;
    memcpy(s.text+used,s.window,got);used+=got;
    memset(s.window,0,sizeof(s.window));
  }while(used<total);
  QuickNotePolicy::NoteHeader header{};size_t body=0;
  if(!QuickNotePolicy::noteParseHeader(s.text,used,&header,&body))return false;
  s.textLength=used-body;memmove(s.text,s.text+body,s.textLength);s.text[s.textLength]=0;
  // Wrap backwards-safe through the spare wire buffer; native-created notes
  // are at most 1024 bytes. Reject oversized externally edited native files.
  if(s.textLength>TextLimit)return false;
  size_t out=0;unsigned column=0;
  for(size_t i=0;i<s.textLength;++i) {
    const uint8_t c=uint8_t(s.text[i]);
    if((c&0xc0)!=0x80) {
      if(c=='\n')column=0;
      else {if(column==24){s.wire[out++]='\n';column=0;}++column;}
    }
    s.wire[out++]=c;
  }
  memcpy(s.text,s.wire,out);s.textLength=out;s.text[out]=0;
  snprintf(s.id,sizeof(s.id),"%s",id);return true;
}
// Stock detail pages hold seven lines on page zero, nine on later pages.
// Use conservative 24-codepoint lines at the stock 20px font (510px width).
// This does not require redistributing the phone app's font assets.
size_t pageEnd(size_t start) {
  const unsigned limit=start?9:7;unsigned count=0;size_t at=start;
  for(;at<s.textLength;++at)if(s.text[at]=='\n' && ++count==limit)return at+1;
  return at;
}
uint32_t pages() {uint32_t n=0;for(size_t at=0;at<s.textLength;at=pageEnd(at))++n;return n?n:1;}
uint32_t lines(size_t start,size_t end) {uint32_t n=start<end?1:0;for(size_t i=start;i+1<end;++i)if(s.text[i]=='\n')++n;return n;}
bool detail(const Event& e) {
  if(!readNote(e.id))return false;
  Writer b{s.body,sizeof(s.body)};b.bytes(1,e.id,16);b.bytes(2,s.title,strlen(s.title));b.bytes(3,nullptr,0);
  b.integer(4,0);b.integer(5,pages());b.integer(6,lines(0,s.textLength));b.integer(7,0);b.integer(8,e.request);
  return send(Meta,4,b);
}
bool page(const Event& e) {
  if(strcmp(s.id,e.id) && !readNote(e.id))return false;
  size_t start=0;
  for(uint32_t i=0;i<e.page;++i) {if(start>=s.textLength)return false;start=pageEnd(start);}
  if(e.page>=pages())return false;
  const size_t end=pageEnd(start);
  Writer b{s.body,sizeof(s.body)};b.bytes(1,e.id,16);b.integer(2,e.page);b.integer(3,lines(start,end));
  b.bytes(4,s.text+start,end-start);b.integer(5,start?lines(0,start):0);b.integer(6,e.request);b.integer(7,0);
  return send(Page,5,b);
}
bool publishList(const Event& e) {
  uint32_t offset=0;
  if(e.direction && e.id[0]) {
    if(!find(e.id))return listAt(0,e);
    for(size_t i=0;i<s.listing.count;++i) {
      char id[17];idText(s.listing.rows[i].header.id,id);
      if(!strcmp(id,e.id)) {
        const uint32_t index=s.listing.nextOffset-s.listing.count+i;
        offset=e.direction==2?index+1:(index>QuickNoteStore::kPageRows?index-QuickNoteStore::kPageRows:0);break;
      }
    }
  }
  return listAt(offset,e);
}
void save() {
  if(!s.draft.canSave()) {fail("empty draft cannot be saved");return;}
  QuickNotePolicy::NoteHeader h{};
  do {h.id=(uint64_t(esp_random())<<32)|esp_random();}while(!h.id);
  h.trigger=QuickNotePolicy::Trigger::Lens;h.mic=QuickNotePolicy::Mic::G2;h.durationMs=s.duration;
  const char* why=nullptr;
  if(!live()) return;
  const bool startedOnSD=VFS::isSDWritable();
  bool saved=QuickNoteStore::create(SOURCE_G2_GLASSES,s.context.epoch,s.context.user,h,
       s.draft.text,s.draft.length,s.path,sizeof(s.path),&why,true);
  // A card removed during the atomic write may fail after admission. Retry
  // only on the now-selected internal tier, retaining the exact draft/id.
  if(!saved && startedOnSD && !VFS::isSDWritable() && live())
    saved=QuickNoteStore::create(SOURCE_G2_GLASSES,s.context.epoch,s.context.user,h,
        s.draft.text,s.draft.length,s.path,sizeof(s.path),&why,true);
  if(!saved) {
    // Keep the only copy until another Save, Discard, disconnect or the idle
    // timeout. A full/unavailable tier must never masquerade as a saved note.
    WARN_SYSTEMF("[G2-NOTES] save failed: %s",why?why:"storage unavailable");
    s.draft.saveWanted=false;
    return;
  }
  clearDraft();s.sd=VFS::isSDWritable();
  s.id[0]=0;s.textLength=0;memset(s.text,0,sizeof(s.text));
  if(!control(4)||!listAt(0,Event{}))fail("saved; list refresh failed");
}
void request(const Event& e) {
  s.touched=millis();
  if(!checkStorage(true))WARN_SYSTEMF("[G2-NOTES] storage folders unavailable");
  if(e.command==Draft) {
    for(unsigned i=0;i<std::min(s.draftActionCount,16u);++i)if(s.draftActions[i]==e.magic)return;
    s.draftActions[s.draftActionCount++%16]=e.magic;
    s.haveDraftMagic=true;s.lastDraftMagic=e.magic;
    if(e.action==1) {clearDraft();s.draft.open=true;active.store(true);sttKeepWarm(120000,true);}
    else if(e.action==4) {clearDraft();}
    else if(e.action==2) {if(s.draft.undo()&&!asr(true))fail("undo display failed");}
    else if(e.action==3 && s.draft.open) {
      s.draft.saveWanted=true;
      if(s.draft.recording){s.draft.stop();s.stopped=millis();(void)sttRequestFinish(s.owner,s.token);}
      if(!s.token)save();
    }
  } else if(e.command==Voice) {
    for(unsigned i=0;i<std::min(s.voiceActionCount,16u);++i)if(s.voiceActions[i]==e.magic)return;
    s.voiceActions[s.voiceActionCount++%16]=e.magic;
    s.haveVoiceMagic=true;s.lastVoiceMagic=e.magic;
    if(e.action==1) {
      if(quickNoteActive() || sttAnySessionActive() || !s.draft.start()) {fail("microphone busy");return;}
      TranscriptOptions opts{};char error[96]={};s.started=millis();
      if(!sttBeginContinuousG2Native(s.owner,s.context.left,&s.token,error,sizeof(error),&opts))fail("transcription unavailable");
    } else if(s.token && s.draft.recording) {
      s.draft.stop();s.stopped=millis();
      if(!sttRequestFinish(s.owner,s.token))fail("cannot finish recording");
    }
  } else if(e.command==ListRequest) {if(!publishList(e))fail("list unavailable");}
  else if(e.command==DetailRequest) {if(!detail(e))fail("note unavailable");}
  else if(e.command==PageRequest) {if(!page(e))fail("page unavailable");}
  else if(e.command==Status && e.action==2)clearDraft();
}
void speech() {
  if(!s.token)return;
  if(!sttSnapshot(s.owner,s.token,&s.speech)) {fail("speech session expired");return;}
  if(sttReadChunk(s.owner,s.token,&s.chunk)) {
    const size_t n=strnlen(s.chunk.text,sizeof(s.chunk.text));
    if(!s.draft.append(s.chunk.text,n)) {fail("draft exceeds 1024 bytes");return;}
    if(!sttAcknowledgeChunk(s.owner,s.token,s.chunk.sequence)) {fail("speech delivery expired");return;}
    s.chunk={};if(!asr(false))fail("transcript display failed");
  } else if(!s.speech.workerActive) {
    if(s.speech.state!=STTState::Done) {fail("transcription failed");return;}
    s.duration+=millis()-s.started;s.token=0;s.draft.done();
    if(!asr(true)) {fail("final transcript display failed");return;}
    if(s.draft.saveWanted)save();
  }
  if(s.token && s.draft.recording && uint32_t(millis()-s.started)>=quickNoteMaxMs()) {
    s.draft.stop();s.stopped=millis();(void)sttRequestFinish(s.owner,s.token);
  }
  if(s.token && s.draft.finishing && uint32_t(millis()-s.stopped)>60000)fail("transcription finish timeout");
}
// Cooperative exit, checked once per loop. The STT token and all private text
// are gone before the null handle is published; after the publish the worker
// only leaves the critical section and deletes itself, so a caller that read
// the null may release everything else. The re-read under taskMux covers a
// release that timed out and withdrew its request while reset() ran.
bool leaveOnStop() {
  if(!stopWanted.load())return false;
  reset();
  portENTER_CRITICAL(&taskMux);
  const bool leave=stopWanted.load();
  if(leave){task=nullptr;stopWanted.store(false);}
  portEXIT_CRITICAL(&taskMux);
  return leave;
}
void worker(void*) {
  for(;;) {
    if(leaveOnStop())vTaskDelete(nullptr);
    G2NoteContext current{};
    if(!g2NativeNotesContext(&current)) {reset();vTaskDelay(pdMS_TO_TICKS(100));continue;}
    if(!same(current,s.context)) {
      reset();s.context=current;s.owner={SOURCE_G2_GLASSES,current.epoch};s.connected=true;s.sd=VFS::isSDWritable();
      s.touched=millis();
      if(!checkStorage(true) ||
         !control(4)||!listAt(0,Event{}))fail("initial list unavailable");
    }
    if(!checkStorage())WARN_SYSTEMF("[G2-NOTES] storage folders unavailable");
    if(s.storageDirty && !s.draft.open && live()) {
      s.storageDirty=false;
      if(!control(4)||!listAt(0,Event{}))fail("storage list refresh failed");
    }
    if(overflow.exchange(false))fail("native request queue full");
    Request r;
    if(xQueueReceive(requestQueue,&r,pdMS_TO_TICKS(50))==pdTRUE && same(r.context,s.context) &&
       g2NativeNotesContextLive(s.context)) {
      s.connected=true;request(r.event);
    }
    if(!live()) {clearDraft();continue;}
    speech();
    if(s.draft.open && !s.token && uint32_t(millis()-s.touched)>300000)clearDraft();
  }
}
} // namespace
bool g2NativeNotesActive() {return active.load();}
void g2NativeNotesTick() {
  if(taskHandle())return;
  G2NoteContext context{};if(!g2NativeNotesContext(&context))return;
  if(!requestQueue)requestQueue=xQueueCreateStatic(Depth,sizeof(Request),requests,&requestControl);
  if(!ackQueue)ackQueue=xQueueCreateStatic(Depth,sizeof(Request),acks,&ackControl);
  if(!requestQueue||!ackQueue)return;
  // Start clean after a released worker: no stale stop request, no request or
  // ack from the previous session. Queue resets are FreeRTOS calls, so they
  // stay outside the critical section; this task is the only producer.
  portENTER_CRITICAL(&taskMux);
  stopWanted.store(false);
  portEXIT_CRITICAL(&taskMux);
  (void)xQueueReset(requestQueue);(void)xQueueReset(ackQueue);overflow.store(false);
  (void)xTaskCreateLogged(worker,"g2_notes",8192,nullptr,TASK_PRIORITY_LOW,&task,"g2.notes",PRO_CORE);
}
void g2NativeNotesReceive(const G2NoteContext& context,const uint8_t* data,size_t len) {
  if(!taskHandle())return;
  Request r{};r.context=context;if(!parse(data,len,r.event))return;
  QueueHandle_t queue=r.event.command==G2Notes::Ack?ackQueue:requestQueue;
  if(xQueueSend(queue,&r,0)!=pdTRUE)overflow.store(true);
}
// Precondition: the control owner (the only producer) is already gone and the
// temple objects have not been reset yet. Returns true only once the worker
// has published its null handle; on a timeout the worker keeps running and
// still owns the queues, so nothing is reset (fail closed).
bool g2NativeNotesRelease(uint32_t timeoutMs) {
  const TaskHandle_t self=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&taskMux);
  const TaskHandle_t running=task;
  if(running && running!=self)stopWanted.store(true);
  portEXIT_CRITICAL(&taskMux);
  if(!running)return true;
  if(running==self)return false;  // the worker leaves through leaveOnStop(), it cannot wait for itself
  // Cut the worker's 50 ms receive short. same() rejects epoch 0, so this
  // request is never acted on; a full queue already wakes the worker.
  Request wake{};(void)xQueueSend(requestQueue,&wake,0);
  const uint32_t start=millis();
  while(taskHandle() && uint32_t(millis()-start)<timeoutMs)vTaskDelay(pdMS_TO_TICKS(10));
  portENTER_CRITICAL(&taskMux);
  stopWanted.store(false);
  const bool gone=task==nullptr;
  portEXIT_CRITICAL(&taskMux);
  if(!gone)return false;
  (void)xQueueReset(requestQueue);(void)xQueueReset(ackQueue);overflow.store(false);
  return true;
}
#endif
