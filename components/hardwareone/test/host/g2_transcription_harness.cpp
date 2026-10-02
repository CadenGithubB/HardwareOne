#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_DICTATION 1
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
// INSERT_POLICY
// INSERT_PAGER
static uint32_t clockMs=1000,liveEpoch=10,menuGen=1;
static bool ownerAuthed=true,queueOkay=true,commandOkay=true,uiOkay=true;
uint32_t millis(){return clockMs;}
enum {BLE_PEER_G2_GLASSES=1};
struct BlePeerOwnerSession {uint32_t transportEpoch=0;bool live()const{return transportEpoch!=0;}};
bool blePeerOwnerSessionSnapshot(int,BlePeerOwnerSession& owner){owner.transportEpoch=ownerAuthed?liveEpoch:0;return ownerAuthed;}
struct G2HijackCtxGuard{uint32_t epoch=liveEpoch;bool stillCurrent()const{return ownerAuthed&&epoch==liveEpoch;}};
enum G2HijackPage:uint8_t{G2_HIJACK_PAGE_APPS=11,G2_HIJACK_PAGE_TRANSCRIPTION=23};
struct LensState {bool hijackActive=true;};
static LensState lens;
LensState g2LensGetState(){return lens;}
static G2HijackPage page=G2_HIJACK_PAGE_APPS;
G2HijackPage g2GetHijackPage(){return page;}
void g2SetHijackPage(G2HijackPage p){if(page!=p)++menuGen;page=p;}
void g2BumpMenuGen(){++menuGen;}
uint32_t g2CurrentMenuGen(){return menuGen;}
void g2ShowAppsMenu(){g2SetHijackPage(G2_HIJACK_PAGE_APPS);}
struct G2CmdCookie{uint64_t seq=0;uint32_t menuGen=0;G2HijackPage targetPage=G2_HIJACK_PAGE_APPS;uint8_t targetNetSub=0;};
using Callback=void(*)(bool,const char*,const G2CmdCookie&,void*);
struct PendingCommand{std::string line;G2CmdCookie cookie;Callback callback;void* opaque;uint32_t epoch;};
static std::vector<PendingCommand> commands;
bool g2SubmitHijackCommand(const char* line,const G2CmdCookie& cookie,Callback cb,void* opaque,uint32_t expectedEpoch){
    if(!commandOkay||!ownerAuthed||expectedEpoch!=liveEpoch)return false;auto c=cookie;c.menuGen=menuGen;
    commands.push_back({line,c,cb,opaque,liveEpoch});return true;
}
struct RedrawSpec{void(*render)()=nullptr;};
enum class LensJobKind{Redraw};enum class G2LensEnqueueWait{NoWait};
struct LensUiJob{LensJobKind kind;uint32_t submitMenuGen=0;G2HijackPage targetPage=G2_HIJACK_PAGE_APPS;struct{RedrawSpec* redraw=nullptr;}payload;};
static std::vector<LensUiJob*> jobs;
bool g2EnqueueLensJob(LensUiJob* job,G2LensEnqueueWait){if(!queueOkay)return false;jobs.push_back(job);return true;}
static void drainUi(){while(!jobs.empty()){auto* job=jobs.front();jobs.erase(jobs.begin());if(job->submitMenuGen==menuGen&&job->targetPage==page)job->payload.redraw->render();delete job->payload.redraw;delete job;}}
struct G2ContainerGeom{uint32_t x=0,y=0,w=0,h=0;};
static const G2ContainerGeom G2_GEOM_LARGE{},G2_GEOM_SPLIT_LIST{};
constexpr size_t G2_TEXT_DEFAULT_COLS=48;
struct G2TextChildSpec{const char* containerName;const char* content;uint32_t containerId;G2ContainerGeom geom;bool eventCapture;};
struct G2TextPageChrome{const char *title,*navHint,*singleHint,*separator,*emptyMsg;};
enum G2TapKind{G2_TAP_PAGE_NEXT,G2_TAP_PAGE_PREV};using G2TapFn=void(*)(G2TapKind);
static std::string displayed,displayedHead;static std::vector<std::string> menu;static unsigned renders=0;
static void(*exitCallback)()=nullptr;static G2TapFn navCallback=nullptr;
bool g2ShowListPage(const char*const* rows,size_t count){if(!uiOkay)return false;menu.assign(rows,rows+count);displayed="";++renders;return true;}
bool g2ShowMixedListText(const char*const* rows,size_t count,const G2ContainerGeom&,const G2TextChildSpec& side){if(!uiOkay)return false;menu.assign(rows,rows+count);displayed=side.content;++renders;return true;}
bool g2ShowMixedListText2(const char*const* rows,size_t count,const G2ContainerGeom&,const G2TextChildSpec& head,const G2TextChildSpec& body){if(!uiOkay)return false;menu.assign(rows,rows+count);displayedHead=head.content;displayed=body.content;++renders;return true;}
bool g2UpdateMixedTextChild(const char* name,uint32_t,const char* text){if(!uiOkay)return false;(std::string(name)=="trhead"?displayedHead:displayed)=text;++renders;return true;}
bool g2ShowTextPage(const char* text,const G2ContainerGeom&,void(*exit)(),G2TapFn nav){if(!uiOkay)return false;displayed=text;exitCallback=exit;navCallback=nav;++renders;return true;}
// INSERT_PRODUCTION
static void answer(const char* json,bool okay=true){assert(commands.size()==1);auto c=commands.front();commands.clear();if(c.epoch==liveEpoch&&ownerAuthed)c.callback(okay,json,c.cookie,c.opaque);}
static void tick(unsigned ms=1000){clockMs+=ms;g2TranscriptionTick();drainUi();}
static const char* id="0123456789abcdef";
static std::string status(bool active,bool done=false){return std::string("{\"success\":true,\"exchange\":\"")+id+"\",\"active\":"+(active?"true":"false")+",\"done\":"+(done?"true":"false")+",\"available\":true,\"captureActive\":"+(active?"true":"false")+",\"saveDefault\":false,\"elapsedMs\":1000}";}
static void home(){g2ShowTranscriptionMenu();drainUi();tick();assert(commands.front().line=="transcription status");answer("{\"success\":true,\"exchange\":\"\",\"available\":true}");tick(1);}
static void start(){g2TranscriptionHandleTap(1);tick(1);assert(commands.front().line=="transcription start");answer(status(true).c_str());tick(1);}
static void clear(){commands.clear();while(!jobs.empty()){auto*j=jobs.back();jobs.pop_back();delete j->payload.redraw;delete j;}state=State{};renderState=State{};renderedCount=0;renderedEpoch=0;ownerAuthed=true;liveEpoch=10;page=G2_HIJACK_PAGE_APPS;queueOkay=commandOkay=uiOkay=true;clockMs=1000;++menuGen;displayed.clear();}
int main(){
    // Lens idle timeout (DISPLAY_OFF) ends the hijack but must not cancel a
    // running session: it keeps receiving text and is shown again on reopen.
    {home();start();lens.hijackActive=false;tick();
     assert(commands.front().line==std::string("transcription next ")+id&&!state.exiting);
     answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":true,\"sequence\":1,\"offset\":0,\"length\":5,\"sttText\":\"while\"}");tick(1);
     assert(commands.front().line==std::string("transcription ack ")+id+" 1 0 5");answer("{\"success\":true}");
     lens.hijackActive=true;g2ShowTranscriptionMenu();drainUi();tick();
     assert(state.active&&std::string(state.exchange)==id&&std::string(state.tail)=="while"&&menu[0]=="<- Apps (cancel session)");
     assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());tick(1);
     g2TranscriptionHandleTap(0);tick(1);assert(commands.front().line==std::string("transcription cancel ")+id);answer("{\"success\":true}");tick(1);
     clear();}
    home();assert(menu.size()==7&&menu[1]=="Start transcription"&&menu[6]=="Mic: Auto");start();assert(menu[0]=="<- Apps (cancel session)");
    tick();assert(commands.front().line==std::string("transcription next ")+id);
    answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":true,\"sequence\":2,\"offset\":0,\"length\":12,\"sttText\":\"hello world!\"}");
    assert(std::string(state.tail)=="hello world!"&&state.ackPending);tick(1);
    assert(commands.front().line==std::string("transcription ack ")+id+" 2 0 12");answer("{\"success\":true}");tick(1);
    g2TranscriptionHandleTap(2);drainUi();assert(displayed.find("hello world!")!=std::string::npos&&navCallback&&exitCallback);
    const unsigned before=renders;enqueueRender();drainUi();assert(renders==before); // unchanged live text does not repeatedly recreate
    exitCallback();drainUi();g2TranscriptionHandleTap(1);tick(1);assert(commands.front().line==std::string("transcription stop ")+id);answer(status(false,true).c_str());tick(1);
    // Explicit Back cancels only the exact lease; no keyboard/source-only call exists.
    g2TranscriptionHandleTap(0);tick(1);assert(commands.front().line==std::string("transcription cancel ")+id);answer("{\"success\":true,\"active\":false}");assert(!state.epoch&&state.tail[0]==0);
    clear();home();g2TranscriptionHandleTap(1);tick(1);assert(commands.front().line=="transcription start");
    g2TranscriptionHandleTap(0);answer(status(true).c_str());tick(1);assert(commands.front().line==std::string("transcription cancel ")+id);answer("{\"success\":true}");assert(page==G2_HIJACK_PAGE_APPS&&!state.epoch);
    // Revocation drops callbacks and clears both the private model and the visible page.
    clear();home();start();std::strcpy(state.tail,"old owner secret");state.dirty=true;tick(1);ownerAuthed=false;tick(1);assert(!state.epoch&&state.tail[0]==0&&displayed.find("secret")==std::string::npos);
    liveEpoch=11;ownerAuthed=true;commands.clear();home();assert(state.tail[0]==0);
    // Saved list/read is asynchronous, uses only returned owner paths, and pages exact UTF-8 boundaries.
    g2TranscriptionHandleTap(4);tick(1);assert(commands.front().line=="transcripts list internal 0");
    answer("{\"success\":true,\"offset\":0,\"nextOffset\":1,\"more\":false,\"entries\":[{\"name\":\"one.txt\",\"path\":\"/stt/u2/one.txt\",\"bytes\":2000}]}");tick(1);
    assert(menu[1]=="one.txt");g2TranscriptionHandleTap(1);tick(1);assert(commands.front().line=="transcripts read \"/stt/u2/one.txt\" 0");
    answer("{\"success\":true,\"sttText\":\"first window\",\"offset\":0,\"nextOffset\":509,\"eof\":false}");tick(1);assert(navCallback&&displayed.find("first window")!=std::string::npos);
    navCallback(G2_TAP_PAGE_NEXT);tick(1);assert(commands.front().line=="transcripts read \"/stt/u2/one.txt\" 509");
    answer("{\"success\":true,\"sttText\":\"second window\",\"offset\":509,\"nextOffset\":1021,\"eof\":false}");tick(1);
    navCallback(G2_TAP_PAGE_PREV);tick(1);assert(commands.front().line=="transcripts read \"/stt/u2/one.txt\" 0");answer("{\"success\":true,\"sttText\":\"first window\",\"offset\":0,\"nextOffset\":509,\"eof\":false}");tick(1);
    // A late saved response cannot replace another view after navigation.
    navCallback(G2_TAP_PAGE_NEXT);tick(1);exitCallback();answer("{\"success\":true,\"sttText\":\"late secret\",\"offset\":509,\"nextOffset\":600,\"eof\":true}");tick(1);assert(state.view==View::List&&std::string(state.fileText).find("late secret")==std::string::npos);
    clear();home();g2TranscriptionHandleTap(3);tick(1);assert(commands.front().line=="sttsavetranscripts 1");answer("Error: Admin required");tick(1);assert(commands.front().line=="transcription status");answer("{\"success\":true,\"available\":true,\"saveDefault\":false}");tick(1);assert(!state.saveDefault);
    // A long recent tail keeps each right-column child inside one UPDATE_TEXT
    // write (MTU 244): the body shows only the newest words, the header status.
    clear();home();{std::string words;for(int i=1;i<=80;++i)words+=std::to_string(i)+" ";
    std::strcpy(state.tail,words.c_str());state.dirty=true;tick(1);
    assert(displayed.size()<=kBodyMax&&displayed.size()>120&&displayedHead.size()<=kSidebarUpdateMax);
    assert(displayed.find("80 ")!=std::string::npos&&displayed.find(" 1 ")==std::string::npos);
    // A live draft follows the confirmed words, and a header-only change
    // patches just the header child without re-sending the body.
    std::strcpy(state.draft,"still talking");state.dirty=true;tick(1);
    assert(displayed.size()<=kBodyMax&&displayed.find("80 still talking")!=std::string::npos);
    const std::string bodyBefore=displayed;const unsigned before=renders;
    std::strcpy(state.message,"New status");state.dirty=true;tick(1);
    assert(displayedHead.find("New status")!=std::string::npos&&displayed==bodyBefore&&renders==before+1);}
    // Mic row cycles only reachable sources, refreshes status, and is refused mid-session.
    clear();g2ShowTranscriptionMenu();drainUi();tick();answer("{\"success\":true,\"exchange\":\"\",\"available\":true,\"micSource\":\"auto\",\"micPdm\":true,\"micG2\":true}");tick(1);
    assert(menu[6]=="Mic: Auto");g2TranscriptionHandleTap(6);tick(1);assert(commands.front().line=="micsource pdm");answer("Mic source preference set to 'pdm'");tick(1);
    assert(commands.front().line=="transcription status");answer("{\"success\":true,\"available\":true,\"micSource\":\"g2\",\"micPdm\":false,\"micG2\":true}");tick(1);
    assert(menu[6]=="Mic: Glasses");g2TranscriptionHandleTap(6);tick(1);assert(commands.front().line=="micsource auto");answer("ok");tick(1);answer(status(true).c_str());tick(1);
    g2TranscriptionHandleTap(6);assert(state.queued==Op::None&&std::string(state.message)=="Stop transcription to change mic");
    clear();home();
    // Queue admission failure retries submission, not an already-executed mutation.
    commandOkay=false;g2TranscriptionHandleTap(1);tick(1);assert(commands.empty()&&state.queued==Op::Start);commandOkay=true;tick(1);assert(commands.front().line=="transcription start");answer("not JSON");assert(!state.active&&state.recoveringStart);
    // A possibly admitted Start is never repeated. Failed read-only recovery
    // remains bounded/throttled; Start and final Back cleanup stay gated.
    g2TranscriptionHandleTap(1);tick(1);assert(commands.empty());
    g2TranscriptionHandleTap(0);assert(page==G2_HIJACK_PAGE_TRANSCRIPTION&&state.exiting);
    tick();assert(commands.front().line=="transcription status");
    answer("{\"success\":false,\"error\":\"temporary failure\"}");tick(1);assert(commands.empty()&&state.recoveringStart);
    tick();assert(commands.front().line=="transcription status");answer("{\"success\":true}");assert(state.recoveringStart);
    tick();assert(commands.front().line=="transcription status");answer(status(true).c_str());tick(1);
    assert(commands.front().line==std::string("transcription cancel ")+id);answer("{\"success\":true}");tick(1);
    assert(page==G2_HIJACK_PAGE_APPS&&!state.epoch);
    // Leaving before an ambiguous reply, then reopening, cannot admit a new
    // run. Recover only this page's outstanding Start and cancel that exact ID.
    clear();home();g2TranscriptionHandleTap(1);tick(1);auto stale=commands.front();
    g2TranscriptionHandleTap(0);answer("broken",false);assert(state.recoveringStart);
    g2ShowTranscriptionMenu();g2TranscriptionHandleTap(1);tick(1);assert(commands.empty());
    tick();assert(commands.front().line=="transcription status");
    stale.callback(true,status(true).c_str(),stale.cookie,stale.opaque);assert(state.recoveringStart&&commands.size()==1);
    answer(status(true).c_str());tick(1);assert(commands.front().line==std::string("transcription cancel ")+id);
    answer("{\"success\":true}");tick(1);assert(!state.epoch&&page==G2_HIJACK_PAGE_APPS);
    // A definitive no-current result completes exit without inventing an ID.
    clear();home();g2TranscriptionHandleTap(1);tick(1);answer("broken");g2TranscriptionHandleTap(0);
    tick();answer("{\"success\":true,\"exchange\":\"\",\"available\":true}");tick(1);
    assert(commands.empty()&&!state.epoch&&page==G2_HIJACK_PAGE_APPS);
    // Revocation during recovery drops the old callback and never queries or
    // cancels a replacement owner's App lease.
    clear();home();g2TranscriptionHandleTap(1);tick(1);answer("broken");tick();
    assert(commands.front().line=="transcription status");liveEpoch=11;answer(status(true).c_str());tick(1);
    assert(commands.empty()&&!state.epoch&&!state.recoveringStart);
    clear();std::puts("G2 transcription actual-source ownership/navigation/receipt tests passed");
}
