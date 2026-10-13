#include "System_STTLocal.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
static size_t checks=0;
#define CHECK(v) do { ++checks; if(!(v)) { std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#v); std::abort(); } } while(0)
using TickType_t=uint32_t;
constexpr int pdTRUE=1,pdFALSE=0;
constexpr TickType_t portMAX_DELAY=~TickType_t(0);
static TickType_t pdMS_TO_TICKS(uint32_t n){return n;}
struct Queue { size_t capacity,width; std::deque<std::vector<unsigned char>> items; std::mutex mutex; std::condition_variable cv; bool failSend=false; };
using QueueHandle_t=Queue*;
struct Semaphore { unsigned count=0;std::mutex mutex;std::condition_variable cv; };
using SemaphoreHandle_t=Semaphore*;
struct Task {std::thread thread;};using TaskHandle_t=Task*;
namespace fake {
std::atomic<int64_t> clock{0};
bool asynchronous=false;
std::function<void()> onWait,onReceive;
std::mutex logMutex,embedMutex;
std::condition_variable embedCv;
std::vector<std::string> logs;
std::atomic<bool> holdEmbed{false},enteredEmbed{false},failEmbed{false};
std::atomic<int> activeEmbeds{0},embedCalls{0},queueDeletes{0},taskDeletes{0},modelResets{0};
void log(const char* fmt,...) {char b[384];va_list a;va_start(a,fmt);std::vsnprintf(b,sizeof(b),fmt,a);va_end(a);std::lock_guard<std::mutex> lock(logMutex);logs.emplace_back(b);}
bool logged(const char* needle){std::lock_guard<std::mutex> lock(logMutex);return std::any_of(logs.begin(),logs.end(),[&](const auto& l){return l.find(needle)!=std::string::npos;});}
void advance(TickType_t ticks){clock.fetch_add(int64_t(ticks)*1000);if(onWait)onWait();}
void clear(){clock=0;asynchronous=false;onWait={};onReceive={};holdEmbed=false;enteredEmbed=false;failEmbed=false;activeEmbeds=0;embedCalls=0;queueDeletes=0;taskDeletes=0;modelResets=0;logs.clear();}
}
#define ESP_LOGI(tag,...) fake::log(__VA_ARGS__)
#define ESP_LOGW(tag,...) fake::log(__VA_ARGS__)
static int64_t esp_timer_get_time(){return fake::clock.load();}
static int xQueueSend(Queue* q,const void* value,TickType_t){
 std::lock_guard<std::mutex> lock(q->mutex);
 if(q->failSend||q->items.size()==q->capacity)return pdFALSE;
 const auto* p=static_cast<const unsigned char*>(value);q->items.emplace_back(p,p+q->width);q->cv.notify_one();return pdTRUE;
}
static int xQueueReceive(Queue* q,void* value,TickType_t wait){
 auto take=[&](){std::lock_guard<std::mutex> lock(q->mutex);if(q->items.empty())return false;std::memcpy(value,q->items.front().data(),q->width);q->items.pop_front();return true;};
 if(take()){if(fake::onReceive)fake::onReceive();return pdTRUE;}
 if(!wait)return pdFALSE;
 if(fake::asynchronous){std::unique_lock<std::mutex> lock(q->mutex);if(wait==portMAX_DELAY)q->cv.wait(lock,[&]{return !q->items.empty();});else q->cv.wait_for(lock,std::chrono::milliseconds(1),[&]{return !q->items.empty();});}
 if(wait!=portMAX_DELAY)fake::advance(wait);
 if(take()){if(fake::onReceive)fake::onReceive();return pdTRUE;}return pdFALSE;
}
static size_t uxQueueMessagesWaiting(Queue* q){std::lock_guard<std::mutex> lock(q->mutex);return q->items.size();}
static void vQueueDeleteWithCaps(Queue* q){assert(fake::activeEmbeds==0);++fake::queueDeletes;delete q;}
static int xSemaphoreTake(Semaphore* s,TickType_t wait){
 {std::lock_guard<std::mutex> lock(s->mutex);if(s->count){--s->count;return pdTRUE;}}
 if(!wait)return pdFALSE;
 if(fake::asynchronous){std::unique_lock<std::mutex> lock(s->mutex);s->cv.wait_for(lock,std::chrono::milliseconds(1),[&]{return s->count>0;});if(s->count){--s->count;return pdTRUE;}}
 fake::advance(wait);return pdFALSE;
}
static void xSemaphoreGive(Semaphore* s){std::lock_guard<std::mutex> lock(s->mutex);++s->count;s->cv.notify_one();}
static void vSemaphoreDeleteWithCaps(Semaphore* s){assert(fake::activeEmbeds==0);delete s;}
static void vTaskDelay(TickType_t n){fake::advance(n);if(fake::asynchronous)std::this_thread::yield();}
static void vTaskSuspend(void*){}
static void vTaskDeleteWithCaps(Task* t){assert(fake::activeEmbeds==0);if(t->thread.joinable())t->thread.join();++fake::taskDeletes;delete t;}
static void sttPsramFree(void* p){assert(fake::activeEmbeds==0);std::free(p);}
static void heap_caps_free(void* p){sttPsramFree(p);}
namespace hw1::stt::speaker {
struct EmbedTiming {uint32_t featureUs=0,trunkUs=0,headUs=0,fftUs=0,melUs=0,normUs=0,quantUs=0,dequantUs=0,sapUs=0,poolUs=0,fcUs=0;unsigned frames=0;};
struct Model {
 void reset(){assert(fake::activeEmbeds==0);++fake::modelResets;}
 bool embed(const int16_t*,size_t,float*,EmbedTiming*){
  assert(++fake::activeEmbeds==1);++fake::embedCalls;fake::enteredEmbed=true;fake::embedCv.notify_all();
  std::unique_lock<std::mutex> lock(fake::embedMutex);fake::embedCv.wait(lock,[]{return !fake::holdEmbed.load();});
  --fake::activeEmbeds;return !fake::failEmbed.load();
 }
};
enum class AssignmentReason:uint8_t { Match };
static const char* assignmentReasonName(AssignmentReason){return "match";}
struct Assignment {
 int speaker=1;float similarity=1;bool created=false,tentative=false;
 uint8_t profilesBefore=2,profilesAfter=2,matches=1;
 int8_t bestSpeaker=1,secondSpeaker=0;float bestSimilarity=0.93f,secondSimilarity=0.44f;
 bool centroidUpdated=true;AssignmentReason reason=AssignmentReason::Match;
};
struct Clusters {Assignment assign(float*,size_t){return {};}};
}
// INSERT_STATE
struct Backend {
 SpeakerState* speaker=nullptr;bool speakerTried=false;
 // INSERT_RELEASE
 ~Backend(){releaseSpeaker();}
};
static float speakerFixtureAgain(SpeakerState&){return 1;}
// INSERT_FUNCTIONS
STTLocalSession::~STTLocalSession(){reset();}
void STTLocalSession::reset(){delete static_cast<Backend*>(backendState_);backendState_=nullptr;}
bool sttSpeakerBegin(STTLocalSession* session,size_t,STTSpeakerStatus*){
 auto* b=new Backend;auto* s=b->speaker=new(std::malloc(sizeof(SpeakerState))) SpeakerState;session->backendState_=b;
 s->jobSpan=8;s->jobPcm=static_cast<int16_t*>(std::calloc(kSpeakerJobs*s->jobSpan,sizeof(int16_t)));
 s->ready=new Queue{kSpeakerJobs+1,1,{}, {}, {},false};s->free=new Queue{kSpeakerJobs,1,{}, {}, {},false};
 s->labels=new Queue{12,sizeof(SpeakerLabelMsg),{}, {}, {},false};s->stopped=new Semaphore;s->task=new Task;
 for(uint8_t i=0;i<kSpeakerJobs;++i)xQueueSend(s->free,&i,0);
 if(fake::asynchronous)s->task->thread=std::thread(speakerTask,s);else s->stopped->count=1;
 gSpeakerActive=s;return true;
}
static std::vector<int16_t> input(){std::vector<int16_t> p(20);for(size_t i=0;i<p.size();++i)p[i]=int16_t(i+10);return p;}
static void fill(STTLocalSession& session){auto p=input();for(uint32_t i=1;i<=4;++i)CHECK(sttSpeakerSubmit(&session,i,p.data(),p.size()));CHECK(uxQueueMessagesWaiting(gSpeakerActive->free)==0);}
static bool cancelFlag(void* p){return *static_cast<bool*>(p);}
static void unknown(STTLocalSession& session,uint32_t wanted){uint32_t seq=0;int8_t who=0;CHECK(sttSpeakerTakeLabel(&session,&seq,&who));CHECK(seq==wanted&&who==-1);}
static void testAdmission(){
 fake::clear();STTLocalSession session;auto p=input();CHECK(!sttSpeakerSubmit(&session,1,p.data(),p.size()));
 sttSpeakerBegin(&session,0,nullptr);auto* s=gSpeakerActive;fill(session);
 uint8_t done=0;CHECK(xQueueReceive(s->ready,&done,0)==pdTRUE);
 fake::onWait=[&]{if(fake::clock>=60000&&uxQueueMessagesWaiting(s->free)==0)xQueueSend(s->free,&done,0);};
 CHECK(sttSpeakerSubmit(&session,5,p.data(),p.size()));fake::onWait={};
 CHECK(fake::clock==60000&&s->submitted==5&&s->skipped==0);CHECK(fake::logged("admitted=1 wait_ms=60"));
 CHECK(uxQueueMessagesWaiting(s->ready)==4&&uxQueueMessagesWaiting(s->free)==0);
 CHECK(s->jobs[done].seq==5&&s->jobs[done].samples==8&&s->jobs[done].phraseSamples==20);
 CHECK(std::equal(s->jobPcm+done*8,s->jobPcm+done*8+8,p.begin()+6));CHECK(fake::embedCalls==0);
}
static void testCancellationAndDeadline(){
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto p=input();bool cancelled=false;
  fake::onWait=[&]{if(fake::clock>=60000)cancelled=true;};
  CHECK(sttSpeakerSubmit(&session,8,p.data(),p.size(),{&cancelled,cancelFlag,nullptr}));fake::onWait={};
  CHECK(fake::clock==60000&&gSpeakerActive->skipped==1);unknown(session,8);CHECK(fake::embedCalls==0);CHECK(fake::logged("reason=cancelled"));}
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto p=input();
  CHECK(sttSpeakerSubmit(&session,9,p.data(),p.size()));CHECK(fake::clock==30000000);unknown(session,9);CHECK(fake::logged("reason=timeout"));CHECK(fake::embedCalls==0);}
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto p=input();bool cancelled=false;
  fake::onReceive=[&]{cancelled=true;};CHECK(sttSpeakerSubmit(&session,10,p.data(),p.size(),{&cancelled,cancelFlag,nullptr}));fake::onReceive={};
  CHECK(uxQueueMessagesWaiting(gSpeakerActive->free)==4&&uxQueueMessagesWaiting(gSpeakerActive->ready)==0);unknown(session,10);CHECK(gSpeakerActive->jobs[0].samples==0);}
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto p=input();gSpeakerActive->ready->failSend=true;
  CHECK(sttSpeakerSubmit(&session,11,p.data(),p.size()));CHECK(uxQueueMessagesWaiting(gSpeakerActive->free)==4);unknown(session,11);CHECK(fake::logged("reason=queue_failed"));gSpeakerActive->ready->failSend=false;}
}
static void testCapturePressure(){
 // A full speaker queue must not monopolize the audio slot needed by capture.
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto p=input();bool allowed=false;
  CHECK(sttSpeakerSubmit(&session,20,p.data(),p.size(),{&allowed,nullptr,nullptr},cancelFlag));
  CHECK(fake::clock==0&&gSpeakerActive->skipped==1);unknown(session,20);CHECK(fake::logged("reason=audio_pressure"));CHECK(fake::embedCalls==0);}
 // Newly arriving audio stops a sliced wait without marking the session cancelled.
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto p=input();bool allowed=true;
  fake::onWait=[&]{if(fake::clock>=60000)allowed=false;};
  CHECK(sttSpeakerSubmit(&session,21,p.data(),p.size(),{&allowed,nullptr,nullptr},cancelFlag));fake::onWait={};
  CHECK(fake::clock==60000);unknown(session,21);CHECK(fake::logged("reason=audio_pressure")&&!fake::logged("reason=cancelled"));}
 // Pressure forbids waiting, not immediate use of an available speaker slot.
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto p=input();int calls=0;
  auto refuse=[](void* p){++*static_cast<int*>(p);return false;};
  CHECK(sttSpeakerSubmit(&session,22,p.data(),p.size(),{&calls,nullptr,nullptr},refuse));
  CHECK(fake::clock==0&&calls==0&&gSpeakerActive->skipped==0&&uxQueueMessagesWaiting(gSpeakerActive->ready)==1);}
 // A slot returned while a slice was waiting is admitted even if pressure rose.
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto* s=gSpeakerActive;auto p=input();bool allowed=true;uint8_t done=0;
  CHECK(xQueueReceive(s->ready,&done,0)==pdTRUE);
  fake::onWait=[&]{allowed=false;xQueueSend(s->free,&done,0);};
  CHECK(sttSpeakerSubmit(&session,23,p.data(),p.size(),{&allowed,nullptr,nullptr},cancelFlag));fake::onWait={};
  CHECK(fake::clock==20000&&s->skipped==0&&s->jobs[done].seq==23&&!fake::logged("reason=audio_pressure"));}
}
static void testDrain(){
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);bool cancelled=false;
  fake::onWait=[&]{if(fake::clock>=40000)cancelled=true;};sttSpeakerDrain(&session,30000,{&cancelled,cancelFlag,nullptr});fake::onWait={};
  CHECK(fake::clock==40000&&fake::queueDeletes==0&&fake::modelResets==0);CHECK(fake::logged("outcome=cancelled"));}
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);sttSpeakerDrain(&session,35);
  CHECK(fake::clock==35000&&fake::queueDeletes==0);CHECK(fake::logged("outcome=timeout"));}
 {fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fill(session);auto* s=gSpeakerActive;
  fake::onWait=[&]{if(fake::clock>=60000){uint8_t i;while(xQueueReceive(s->ready,&i,0)==pdTRUE)xQueueSend(s->free,&i,0);}};
  sttSpeakerDrain(&session,30000);fake::onWait={};CHECK(fake::clock==60000&&fake::logged("outcome=complete"));}
}
static void testMatchMetadata(){
 fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto p=input();SpeakerJob job;job.seq=43;job.samples=8;job.phraseSamples=20;
 speakerLabelAndLog(*gSpeakerActive,job,p.data());
 CHECK(fake::embedCalls==1); // metadata never triggers another model pass
 CHECK(fake::logged("TL spk_match seq=43 profiles_before=2 profiles_after=2 best=1 best_sim=0.930 second=0 second_sim=0.440 matches=1 updated=1 reason=match phrase_samples=20"));
 CHECK(!fake::logged("TL spke "));
}
static void testTerminalFailure(){
 fake::clear();STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);fake::failEmbed=true;auto p=input();SpeakerJob job;job.seq=42;job.samples=8;job.phraseSamples=20;
 speakerLabelAndLog(*gSpeakerActive,job,p.data());unknown(session,42);CHECK(gSpeakerActive->skipped==1&&fake::embedCalls==1);
 for(unsigned i=0;i<12;++i)speakerPostLabel(*gSpeakerActive,100+i,-1);
 speakerPostLabel(*gSpeakerActive,999,-1);CHECK(fake::logged("spk_result_lost seq=999"));
}
static void waitFor(const std::function<bool()>& done){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!done()){if(std::chrono::steady_clock::now()>end)std::abort();std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
static void testJoin(){
 fake::clear();fake::asynchronous=true;fake::holdEmbed=true;STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto* s=gSpeakerActive;auto p=input();
 CHECK(sttSpeakerSubmit(&session,1,p.data(),p.size()));waitFor([]{return fake::enteredEmbed.load();});
 CHECK(sttSpeakerSubmit(&session,2,p.data(),p.size()));CHECK(sttSpeakerSubmit(&session,3,p.data(),p.size()));
 std::thread stop([&]{s->stopTask();});waitFor([]{return fake::clock>=9000000;});
 CHECK(fake::activeEmbeds==1&&fake::queueDeletes==0&&fake::taskDeletes==0&&fake::modelResets==0);
 CHECK(fake::logged("keeping model and queues alive"));
 fake::holdEmbed=false;fake::embedCv.notify_all();stop.join();
 CHECK(fake::activeEmbeds==0&&fake::embedCalls==1&&s->labelled==1&&s->skipped==2);
 CHECK(fake::taskDeletes==1&&fake::queueDeletes==3&&s->task==nullptr);
 session.reset();CHECK(fake::modelResets==1);
}
static void testEndSummary(){
 fake::clear();fake::asynchronous=true;fake::holdEmbed=true;STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);auto p=input();
 CHECK(sttSpeakerSubmit(&session,1,p.data(),p.size()));waitFor([]{return fake::enteredEmbed.load();});
 CHECK(sttSpeakerSubmit(&session,2,p.data(),p.size()));CHECK(sttSpeakerSubmit(&session,3,p.data(),p.size()));
 std::thread end([&]{sttSpeakerEnd(&session);});waitFor([]{return fake::clock>=9000000;});
 CHECK(!fake::logged("TL spk_end ")); // final totals must wait for the active embed and queued discards
 CHECK(fake::activeEmbeds==1&&fake::queueDeletes==0&&fake::modelResets==0);
 fake::holdEmbed=false;fake::embedCv.notify_all();end.join();
 CHECK(fake::logged("TL spk_end submitted=3 labelled=1 skipped=2"));
 CHECK(fake::activeEmbeds==0&&fake::embedCalls==1&&fake::queueDeletes==3&&fake::taskDeletes==1&&fake::modelResets==1);
 CHECK(gSpeakerActive==nullptr);
 size_t summaries=0;for(const auto& line:fake::logs)if(line.find("TL spk_end ")!=std::string::npos)++summaries;
 CHECK(summaries==1);
 sttSpeakerEnd(&session);session.reset(); // explicit end plus destructor must not free or join twice
 CHECK(fake::queueDeletes==3&&fake::taskDeletes==1&&fake::modelResets==1);
}
static void testIdleStop(){fake::clear();fake::asynchronous=true;STTLocalSession session;sttSpeakerBegin(&session,0,nullptr);gSpeakerActive->stopTask();CHECK(fake::taskDeletes==1&&fake::embedCalls==0);}
int main(){testAdmission();testCancellationAndDeadline();testCapturePressure();testDrain();testMatchMetadata();testTerminalFailure();testJoin();testEndSummary();testIdleStop();std::printf("speaker queue: %zu checks passed (actual submit/drain/worker/join; no model inference)\n",checks);}
