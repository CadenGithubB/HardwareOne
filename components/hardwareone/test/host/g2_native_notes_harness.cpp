#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_QUICKNOTE 1
#define ENABLE_LOCAL_STT 1
#define EXT_RAM_BSS_ATTR
#define WARN_SYSTEMF(...) do { if(false) std::printf(__VA_ARGS__); } while(0)
using TransportSessionEpoch=uint32_t;
enum CommandSource {SOURCE_INTERNAL,SOURCE_G2_GLASSES};
struct AuthContext {CommandSource transport=SOURCE_INTERNAL;const char* user=nullptr;};
// INSERT_POLICY
// INSERT_DECL
// INSERT_PROTOCOL
static uint32_t clockMs=0;
static uint32_t millis(){return clockMs++;}
static uint32_t esp_random(){static uint32_t n=1;return ++n;}
using TickType_t=uint32_t;
constexpr int pdTRUE=1,pdPASS=1,TASK_PRIORITY_LOW=1,PRO_CORE=0;
#define pdMS_TO_TICKS(x) (x)
// Every FreeRTOS double asserts that it is not called inside a critical section.
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned criticalDepth=0;
static void(*criticalEnterHook)()=nullptr;
static void portENTER_CRITICAL(portMUX_TYPE*){assert(criticalDepth++==0);if(criticalEnterHook)criticalEnterHook();}
static void portEXIT_CRITICAL(portMUX_TYPE*){assert(criticalDepth--==1);}
static void(*delayHook)()=nullptr;
static void vTaskDelay(uint32_t n){assert(!criticalDepth);clockMs+=n;if(delayHook)delayHook();}
using TaskHandle_t=void*;
static const TaskHandle_t workerHandle=reinterpret_cast<void*>(1),ownerHandle=reinterpret_cast<void*>(2);
static TaskHandle_t currentTask=ownerHandle;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return currentTask;}
static unsigned selfDeletes=0;
static void vTaskDelete(TaskHandle_t h){assert(!criticalDepth&&!h&&currentTask==workerHandle);++selfDeletes;}
struct Queue {unsigned depth=0;size_t size=0;std::deque<std::vector<uint8_t>> items;};
using QueueHandle_t=Queue*;
using StaticQueue_t=Queue;
static QueueHandle_t xQueueCreateStatic(unsigned n,size_t size,uint8_t*,StaticQueue_t* q){assert(!criticalDepth);q->depth=n;q->size=size;return q;}
static int xQueueSend(QueueHandle_t q,const void* data,uint32_t){assert(!criticalDepth);if(q->items.size()>=q->depth)return 0;const auto* p=static_cast<const uint8_t*>(data);q->items.emplace_back(p,p+q->size);return 1;}
static int xQueueReceive(QueueHandle_t q,void* out,uint32_t wait){assert(!criticalDepth);if(q->items.empty()){clockMs+=wait;return 0;}memcpy(out,q->items.front().data(),q->size);q->items.pop_front();return 1;}
static int xQueueReset(QueueHandle_t q){assert(!criticalDepth);q->items.clear();return pdPASS;}
static int xTaskCreateLogged(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t* t,const char*,int){assert(!criticalDepth);*t=workerHandle;return 1;}
struct STTOwner {CommandSource source=SOURCE_INTERNAL;uint32_t epoch=0;};
using STTToken=uint64_t;
enum class STTState {Recording,Done,Failed};
struct STTSnapshot {bool workerActive=false;STTState state=STTState::Done;};
struct STTTextChunk {uint32_t sequence=1;char text[513]={};};
struct TranscriptOptions {};
static bool brokerActive=false, chunkReady=false, liveContext=true, sdPresent=false;
static unsigned starts=0,writes=0,attempts=0,folderChecks=0;
static bool failSave=false,removeDuringSave=false;
static uint32_t mountGeneration=0;
static std::string savedText,savedPath;
static std::string savedFile;
static QuickNotePolicy::NoteHeader savedHeader{};
static std::vector<std::vector<uint8_t>> transmissions;
static bool quickNoteActive(){return false;}
static uint32_t quickNoteMaxMs(){return 120000;}
static bool sttCancel(STTOwner,STTToken){brokerActive=false;return true;}
static bool sttAnySessionActive(){return brokerActive;}
static void sttKeepWarm(uint32_t,bool){}
static bool sttBeginContinuousG2Native(STTOwner,uint32_t,STTToken* t,char*,size_t,const TranscriptOptions*){*t=++starts;brokerActive=true;return true;}
static bool sttRequestFinish(STTOwner,STTToken){brokerActive=false;return true;}
static bool sttReadChunk(STTOwner,STTToken,STTTextChunk* c){if(!chunkReady)return false;snprintf(c->text,sizeof(c->text),"A synthetic test note.");return true;}
static bool sttAcknowledgeChunk(STTOwner,STTToken,uint32_t){chunkReady=false;return true;}
static bool sttSnapshot(STTOwner,STTToken,STTSnapshot* p){p->workerActive=brokerActive;p->state=brokerActive?STTState::Recording:STTState::Done;return true;}
namespace VFS {
static bool isSDWritable(){return sdPresent;}
static uint32_t sdMountGeneration(){return mountGeneration;}
}
// INSERT_RUNTIME
bool g2NativeNotesContext(G2NoteContext* c){*c={};c->epoch=7;c->left=11;c->right=12;strcpy(c->user,"wearer");return liveContext;}
bool g2NativeNotesContextLive(const G2NoteContext& c){return liveContext&&c.epoch==7&&c.left==11&&c.right==12;}
static bool ackEnabled=true,wrongAck=false;
bool g2NativeNotesWireSend(const G2NoteContext& c,const uint8_t* data,size_t len){
  if(!g2NativeNotesContextLive(c))return false;
  transmissions.emplace_back(data,data+len);
  G2Notes::Reader r{data,len};uint32_t n;uint8_t w;uint64_t v;const uint8_t* p;size_t z;
  Request ack{};ack.context=c;ack.event.command=G2Notes::Ack;
  while(r.field(n,w,v,p,z)){if(n==1)ack.event.acked=v;if(n==2)ack.event.magic=v;}
  assert(r.ok);
  if(wrongAck)++ack.event.magic;
  if(ackEnabled)xQueueSend(ackQueue,&ack,0);
  return true;
}
namespace QuickNoteStore {
bool ensureG2Folders(const AuthContext&,bool,const char**,bool){++folderChecks;return true;}
bool list(const AuthContext&,uint32_t,bool sd,uint32_t offset,Page* page,const char**,bool native){
  assert(native);*page={};
  if(savedFile.empty() || offset || sd!=(savedPath.find("/sd/")==0))return true;
  page->count=page->total=page->nextOffset=1;page->rows[0].header=savedHeader;page->rows[0].headerValid=true;
  snprintf(page->rows[0].path,sizeof(page->rows[0].path),"%s",savedPath.c_str());return true;
}
bool read(const AuthContext&,const char* path,uint32_t offset,char* out,size_t cap,size_t* got,uint32_t* total,const char**){
  if(savedPath!=path||offset>savedFile.size())return false;
  *total=savedFile.size();*got=std::min({kReadWindow,cap-1,savedFile.size()-offset});
  if(offset+*got<savedFile.size())*got=TranscriptionUI::completePrefix(savedFile.data()+offset,*got);
  memcpy(out,savedFile.data()+offset,*got);out[*got]=0;return true;
}
bool create(CommandSource,uint32_t epoch,const char*,QuickNotePolicy::NoteHeader& header,const char* text,size_t length,char* path,size_t cap,const char**,bool native){
  assert(native&&epoch==7&&liveContext);++attempts;
  if(removeDuringSave){removeDuringSave=false;sdPresent=false;return false;}
  if(failSave)return false;
  ++writes;savedText.assign(text,length);
  savedPath=sdPresent?"/sd/g2/quicknotes/test.txt":"/g2/quicknotes/test.txt";
  savedHeader=header;strcpy(savedHeader.created,"20261015T200000Z");
  QuickNotePolicy::noteTitle(text,length,savedHeader.title,sizeof(savedHeader.title));
  char head[QuickNotePolicy::kHeaderMax];const size_t n=QuickNotePolicy::noteFormatHeader(savedHeader,head,sizeof(head));
  assert(n);savedFile.assign(head,n);savedFile.append(text,length);
  snprintf(path,cap,"%s",savedPath.c_str());return true;
}
}
static void event(uint32_t command,uint32_t action,uint32_t magic){G2Notes::Event e{};e.command=command;e.action=action;e.magic=magic;request(e);}
static void connected(){reset();g2NativeNotesContext(&s.context);s.owner={SOURCE_G2_GLASSES,7};s.connected=true;}
int main(){
  using namespace G2Notes;
  uint8_t body[64],wire[128];Writer b{body,sizeof(body)};b.integer(1,1);b.integer(2,2);
  const size_t size=wrap(Draft,9,14,b,wire,sizeof(wire));Event e;
  assert(parse(wire,size,e)&&e.action==1&&e.entry==2&&e.magic==9);
  for(size_t i=0;i<size;++i)assert(!parse(wire,i,e));
  uint8_t zeros[16]={};Writer l{body,sizeof(body)};l.bytes(2,zeros,16);l.integer(3,4);
  assert(parse(wire,wrap(ListRequest,3,7,l,wire,sizeof(wire)),e)&&e.hasId&&e.request==4);
  Writer tiny{wire,1};tiny.bytes(1,zeros,16);assert(!tiny.finish());
  uint8_t bad[]={0x08,0xa7,0x01,0x72,0x04,0x08,1,0x08,3};assert(!parse(bad,sizeof(bad),e));
  DraftState d;d.open=true;assert(d.start());assert(d.append("hello",5));d.stop();d.saveWanted=true;assert(!d.canSave());d.done();assert(d.canSave());
  d.clear();assert(!d.canSave());d.open=true;assert(d.start());assert(!d.append("x",TextLimit+1));
  assert(g2NativeNotesRelease(0)&&!task&&!requestQueue); // nothing to release
  g2NativeNotesTick();connected();
  event(Draft,1,1);event(Voice,1,2);assert(starts==1&&active.load());
  chunkReady=true;speech();assert(writes==0&&s.draft.length);
  event(Voice,2,3);speech();assert(writes==0&&!s.token&&s.draft.open);
  event(Draft,3,4);assert(writes==1&&savedText=="A synthetic test note.");
  event(Draft,3,4);assert(writes==1); // duplicate Save cannot create another file
  event(Draft,1,5);event(Voice,1,6);chunkReady=true;speech();event(Draft,4,7);
  assert(writes==1&&!s.token&&!s.draft.open&&!brokerActive);
  event(Draft,1,8);event(Voice,1,9);chunkReady=true;speech();
  event(Draft,3,10);assert(writes==1);speech();assert(writes==2); // Save waits for STT completion
  event(Draft,1,11);event(Voice,1,12);chunkReady=true;speech();event(Voice,2,13);speech();
  liveContext=false;event(Draft,3,14);assert(writes==2);reset();assert(!s.draft.text[0]);liveContext=true;connected();
  sdPresent=true;event(Draft,1,15);event(Voice,1,16);chunkReady=true;speech();event(Voice,2,17);speech();
  sdPresent=false;event(Draft,3,18);assert(writes==3&&savedPath.find("/g2/quicknotes/")==0);
  sdPresent=true;++mountGeneration;assert(checkStorage(true));
  event(Draft,1,19);event(Voice,1,20);chunkReady=true;speech();event(Voice,2,21);speech();
  const unsigned before=attempts;removeDuringSave=true;event(Draft,3,22);
  assert(attempts==before+2&&writes==4&&savedPath.find("/g2/quicknotes/")==0);
  event(Draft,1,23);event(Voice,1,24);chunkReady=true;speech();event(Voice,2,25);speech();
  failSave=true;event(Draft,3,26);
  assert(writes==4&&s.draft.open&&s.draft.length&&!s.draft.saveWanted);
  failSave=false;event(Draft,3,27);assert(writes==5&&!s.draft.open);
  // A replacement card can be mounted between polls without an observed
  // unavailable state. Its generation must still invalidate cached content.
  sdPresent=true;++mountGeneration;checkStorage(true);
  strcpy(s.id,"0000000000000001");strcpy(s.text,"stale card text");s.textLength=strlen(s.text);
  event(Draft,1,28);s.draft.append("unsaved draft",13);
  ++mountGeneration;checkStorage(true);
  assert(!s.id[0]&&!s.textLength&&s.storageDirty&&s.draft.length==13);
  const unsigned checked=folderChecks;checkStorage();assert(folderChecks==checked);
  reset();assert(!s.storageDirty&&!s.draft.length);
  connected();
  assert(lineCount("",0)==0&&lineCount("first\nsecond",12)==2);
  assert(lineCount("ééé",6,2)==2); // UTF-8 bytes are not columns
  // Exercise the production note reader and metadata/page writer with a
  // multi-window UTF-8 body, including a repeated request with a new magic.
  sdPresent=false;checkStorage(true);
  std::string longText;
  for(unsigned i=0;i<180;++i)longText+="界";
  QuickNotePolicy::NoteHeader h{};h.id=0x123;char saved[QuickNotePolicy::kPathMax];
  assert(QuickNoteStore::create(SOURCE_G2_GLASSES,7,"wearer",h,longText.data(),longText.size(),saved,sizeof(saved),nullptr,true));
  Event detailEvent{};detailEvent.command=DetailRequest;detailEvent.hasId=true;detailEvent.request=42;idText(h.id,detailEvent.id);
  assert(detail(detailEvent)&&pages()==2&&lines(0,s.textLength)==8);
  Event pageEvent=detailEvent;pageEvent.command=PageRequest;pageEvent.page=1;
  assert(page(pageEvent));const auto firstPage=transmissions.back();
  assert(page(pageEvent));const auto secondPage=transmissions.back();
  auto bodyOf=[](const std::vector<uint8_t>& packet){
    Reader r{packet.data(),packet.size()};uint32_t n;uint8_t w;uint64_t v;const uint8_t* p;size_t z;
    while(r.field(n,w,v,p,z))if(n==5)return std::vector<uint8_t>(p,p+z);
    assert(false);return std::vector<uint8_t>{};
  };
  assert(bodyOf(firstPage)==bodyOf(secondPage));
  auto lastPage=bodyOf(firstPage);Reader pageFields{lastPage.data(),lastPage.size()};
  uint32_t field;uint8_t type;uint64_t value;const uint8_t* bytes;size_t length;bool checkedText=false;
  while(pageFields.field(field,type,value,bytes,length)) {
    if(field==3)assert(value==1);
    if(field==4){assert(length==36&&bytes[0]==0xe7);checkedText=true;}
    if(field==5)assert(value==7);
    if(field==6)assert(value==42);
  }
  assert(checkedText&&pageFields.ok);
  pageEvent.page=2;assert(!page(pageEvent));
  // Unknown binary IDs cannot resolve to a saved file or a path.
  memset(detailEvent.id,0,16);assert(!detail(detailEvent));
  wrongAck=true;Writer c{body,sizeof(body)};c.integer(1,4);assert(!send(Control,6,c));wrongAck=false;
  // Cooperative release. The harness never runs worker(); the vTaskDelay hook
  // stands in for the worker reaching the top of its loop while the release
  // polls, exactly as the worker's first statement would.
  static bool leaving=false;
  const auto drain=[]{Request r;size_t n=0;while(xQueueReceive(requestQueue,&r,0)==pdTRUE)++n;return n;};
  // Refused from the worker itself: no request, no sentinel, the task stays.
  connected();currentTask=workerHandle;
  assert(!g2NativeNotesRelease(1000)&&task==workerHandle&&!stopWanted.load()&&requestQueue->items.empty());
  currentTask=ownerHandle;
  // A pending stop ends wire writes at once, without touching the glasses.
  event(Draft,1,30);event(Voice,1,31);chunkReady=true;speech();assert(brokerActive&&s.token&&s.draft.length);
  const size_t sentBefore=transmissions.size();stopWanted.store(true);
  Writer stopped{body,sizeof(body)};stopped.integer(1,4);
  assert(!send(Control,6,stopped)&&transmissions.size()==sentBefore&&!live());
  stopWanted.store(false);assert(live());
  // Timeout: the worker never reaches its loop top. The task, its STT session
  // and its draft are kept; the request is withdrawn; nothing is reset.
  delayHook=nullptr;const uint32_t pollStart=clockMs;
  assert(!g2NativeNotesRelease(30));
  assert(task==workerHandle&&!stopWanted.load()&&!selfDeletes&&brokerActive&&s.token&&s.draft.length);
  assert(clockMs-pollStart>=30&&clockMs-pollStart<60);
  Request sentinel{};assert(xQueueReceive(requestQueue,&sentinel,0)==pdTRUE&&requestQueue->items.empty());
  assert(!sentinel.context.epoch&&!same(sentinel.context,s.context)); // the wake-up is never acted on
  // Cooperating worker: reset() runs before the null handle is published, so
  // at the critical-section entry the STT session is already cancelled and
  // the private text is gone while the handle is still visible.
  leaving=false;
  criticalEnterHook=[]{if(!leaving)return;assert(task==workerHandle&&stopWanted.load());
    assert(!brokerActive&&!s.token&&!s.draft.open&&!s.draft.length&&!s.text[0]&&!s.connected&&!active.load());};
  delayHook=[]{leaving=true;currentTask=workerHandle;if(leaveOnStop())vTaskDelete(nullptr);currentTask=ownerHandle;leaving=false;};
  Request junk{};junk.context=s.context;assert(xQueueSend(ackQueue,&junk,0)==pdTRUE);overflow.store(true);
  assert(g2NativeNotesRelease(1000));
  assert(!task&&!stopWanted.load()&&selfDeletes==1&&!brokerActive&&!s.token);
  assert(requestQueue->items.empty()&&ackQueue->items.empty()&&!overflow.load());
  delayHook=nullptr;criticalEnterHook=nullptr;
  assert(g2NativeNotesRelease(0)&&selfDeletes==1); // idempotent once the worker is gone
  // Without a worker, ingress is dropped instead of filling a dead queue.
  G2NoteContext context{};assert(g2NativeNotesContext(&context));
  Writer late{body,sizeof(body)};late.integer(1,1);late.integer(2,2);
  g2NativeNotesReceive(context,wire,wrap(Draft,40,14,late,wire,sizeof(wire)));assert(requestQueue->items.empty());
  // A later tick recreates the worker from a clean slate: no stale stop
  // request and no leftover request or ack.
  assert(xQueueSend(requestQueue,&junk,0)==pdTRUE&&xQueueSend(ackQueue,&junk,0)==pdTRUE);stopWanted.store(true);overflow.store(true);
  g2NativeNotesTick();
  assert(task==workerHandle&&!stopWanted.load()&&requestQueue->items.empty()&&ackQueue->items.empty()&&!overflow.load());
  g2NativeNotesReceive(context,wire,wrap(Draft,41,14,late,wire,sizeof(wire)));assert(drain()==1);
  // A stop withdrawn while reset() ran must not make the worker leave.
  stopWanted.store(true);connected();
  criticalEnterHook=[]{stopWanted.store(false);};
  assert(!leaveOnStop()&&task==workerHandle&&!s.connected&&selfDeletes==1);
  criticalEnterHook=nullptr;assert(!leaveOnStop());
  puts("Native G2 notes: codec bounds, opaque IDs, drafts, save/stop/discard, duplicate Save, revoked session, SD fallback, correlated ACKs and cooperative release passed");
}
