#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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
static std::function<void()> snapshotHook;
uint32_t millis(){return clockMs;}
enum {BLE_PEER_G2_GLASSES=1};
struct BlePeerOwnerSession {uint32_t transportEpoch=0;bool live()const{return transportEpoch!=0;}};
bool blePeerOwnerSessionSnapshot(int,BlePeerOwnerSession& owner){if(snapshotHook){auto hook=std::move(snapshotHook);snapshotHook={};hook();}owner.transportEpoch=ownerAuthed?liveEpoch:0;return ownerAuthed;}
struct G2HijackCtxGuard{uint32_t epoch=liveEpoch;bool stillCurrent()const{return ownerAuthed&&epoch==liveEpoch;}};
enum G2HijackPage:uint8_t{G2_HIJACK_PAGE_APPS=11,G2_HIJACK_PAGE_TRANSCRIPTION=23};
struct LensState {bool hijackActive=true,containerReady=true;};
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
static std::vector<std::string> lineNames,lineText;
static unsigned creates=0;static std::vector<std::string> patches;
static bool textReady=false,swapPending=false;
static std::string failedChild;
static void joinedLines(){displayed.clear();for(size_t i=1;i<lineText.size();++i){if(i>1)displayed+='\n';displayed+=lineText[i];}}
bool g2TextPageReady(void(*exit)(),G2TapFn nav,bool* pending){if(pending)*pending=swapPending;return !swapPending&&textReady&&lens.containerReady&&exit==exitCallback&&nav==navCallback;}
bool g2ShowMultiTextPage(const G2TextChildSpec* children,size_t count,void(*exit)(),G2TapFn nav){
    if(!uiOkay||swapPending)return false;lineNames.clear();lineText.clear();
    assert(count==4); // current header + three text bands; never regress to nine child objects
    for(size_t i=0;i<count;++i){
        // One input owner: the header captures gestures; body bands only display text.
        assert(children[i].eventCapture==(i==0));
        assert(children[i].content&&*children[i].content); // visible bounded initial content
        assert(children[i].geom.y+children[i].geom.h<=280);
        if(i)assert(children[i-1].geom.y+children[i-1].geom.h<=children[i].geom.y);
        lineNames.emplace_back(children[i].containerName);lineText.emplace_back(children[i].content);
    }
    exitCallback=exit;navCallback=nav;textReady=true;lens.containerReady=true;++creates;++renders;joinedLines();return true;
}
bool g2ShowListPage(const char*const* rows,size_t count){if(!uiOkay)return false;menu.assign(rows,rows+count);displayed="";++renders;return true;}
bool g2ShowMixedListText(const char*const* rows,size_t count,const G2ContainerGeom&,const G2TextChildSpec& side){if(!uiOkay)return false;menu.assign(rows,rows+count);displayed=side.content;++renders;return true;}
bool g2ShowMixedListText2(const char*const* rows,size_t count,const G2ContainerGeom&,const G2TextChildSpec& head,const G2TextChildSpec& body){if(!uiOkay)return false;textReady=false;lens.containerReady=true;menu.assign(rows,rows+count);displayedHead=head.content;displayed=body.content;++renders;return true;}
bool g2UpdateMixedTextChild(const char* name,uint32_t,const char* text){
    assert(std::strlen(text)<=180);if(!uiOkay||failedChild==name)return false;
    patches.emplace_back(name);
    if(textReady){auto at=std::find(lineNames.begin(),lineNames.end(),name);assert(at!=lineNames.end());lineText[at-lineNames.begin()]=text;joinedLines();}
    else (std::string(name)=="trhead"?displayedHead:displayed)=text;
    ++renders;return true;
}
bool g2ShowTextPage(const char* text,const G2ContainerGeom&,void(*exit)(),G2TapFn nav){if(!uiOkay)return false;displayed=text;exitCallback=exit;navCallback=nav;++renders;return true;}
// INSERT_PRODUCTION
static void answer(const char* json,bool okay=true){assert(commands.size()==1);auto c=commands.front();commands.clear();if(c.epoch==liveEpoch&&ownerAuthed)c.callback(okay,json,c.cookie,c.opaque);}
static void tick(unsigned ms=1000){clockMs+=ms;g2TranscriptionTick();drainUi();}
static const char* id="0123456789abcdef";
static std::string status(bool active,bool done=false){return std::string("{\"success\":true,\"exchange\":\"")+id+"\",\"active\":"+(active?"true":"false")+",\"done\":"+(done?"true":"false")+",\"available\":true,\"captureActive\":"+(active?"true":"false")+",\"saveDefault\":false,\"elapsedMs\":1000}";}
static void home(){g2ShowTranscriptionMenu();drainUi();tick();assert(commands.front().line=="transcription status");answer("{\"success\":true,\"exchange\":\"\",\"available\":true}");tick(1);}
static void start(){g2TranscriptionHandleTap(1);tick(1);assert(commands.front().line=="transcription start");answer(status(true).c_str());tick(1);}
static void clear(){snapshotHook={};commands.clear();while(!jobs.empty()){auto*j=jobs.back();jobs.pop_back();delete j->payload.redraw;delete j;}state=State{};renderState=State{};renderedCount=0;renderedEpoch=0;ownerAuthed=true;liveEpoch=10;page=G2_HIJACK_PAGE_APPS;queueOkay=commandOkay=uiOkay=true;clockMs=1000;++menuGen;displayed.clear();lens={};textReady=swapPending=false;creates=0;patches.clear();lineNames.clear();lineText.clear();failedChild.clear();}
static void liveFixture(){clear();home();start();g2TranscriptionHandleTap(2);drainUi();enqueueRender();drainUi();patches.clear();}
static void nextReply(JsonDocument& doc){
    assert(state.pending==Op::None);doc["success"]=true;doc["exchange"]=id;
    std::string json;serializeJson(doc,json);state.queued=Op::Next;tick(1);
    assert(commands.size()==1&&commands.front().line==std::string("transcription next ")+id);answer(json.c_str());tick(0);
    if(state.pending==Op::Ack){assert(commands.front().line.find("transcription ack ")==0);answer("{\"success\":true}");tick(0);}
}
static void phraseReply(uint32_t sequence,const std::string& text,int speaker=-1,uint32_t offset=0,bool available=true){
    JsonDocument doc;doc["available"]=true;doc["sequence"]=sequence;doc["offset"]=offset;
    doc["length"]=text.size();doc["sttText"]=text;doc["speakersAvailable"]=available;
    doc["draftVersion"]=state.draftVersion+1;doc["draft"]="";
    if(speaker>=0){auto label=doc["speakers"].to<JsonArray>().add<JsonObject>();label["sequence"]=sequence;label["ready"]=true;label["speaker"]=speaker;}
    nextReply(doc);
}
static void labelReply(std::initializer_list<std::pair<uint32_t,int>> labels){
    JsonDocument doc;doc["available"]=false;doc["speakersAvailable"]=true;
    doc["draftVersion"]=state.draftVersion;doc["draft"]=state.draft;
    auto entries=doc["speakers"].to<JsonArray>();
    for(auto item:labels){auto label=entries.add<JsonObject>();label["sequence"]=item.first;label["speaker"]=item.second;label["ready"]=true;}
    nextReply(doc);
}
static void draftReply(const std::string& text){
    JsonDocument doc;doc["available"]=false;doc["draftVersion"]=state.draftVersion+1;doc["draft"]=text;nextReply(doc);
}
static void scroll(G2TapKind kind){clockMs+=121;navCallback(kind);drainUi();}
int main(){
    // Visible Live text fetches every 800 ms, independently of 1 s status.
    // Repeat across millis() rollover; unchanged replies send no lens writes.
    for(uint32_t initialClock : {1000u, UINT32_MAX-2100u}) {
        clear();clockMs=initialClock;home();start();const uint32_t began=state.lastPoll;
        g2TranscriptionHandleTap(2);drainUi();enqueueRender();drainUi();patches.clear();
        auto at=[&](uint32_t elapsed){assert(elapsed>=uint32_t(clockMs-began));tick(elapsed-uint32_t(clockMs-began));};
        const char* draft="{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":false,\"draftVersion\":1,\"draft\":\"still talking\"}";
        at(799);assert(commands.empty());at(800);
        assert(commands.front().line==std::string("transcription next ")+id);answer(draft);tick(0);
        assert(displayed.find("still talking")!=std::string::npos&&patches==std::vector<std::string>{"trl0"}&&creates==1);patches.clear();
        at(999);assert(commands.empty());at(1000);
        assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());tick(0);
        assert(patches.empty());at(1599);assert(commands.empty());at(1600);
        assert(commands.front().line==std::string("transcription next ")+id);answer(draft);tick(0);assert(patches.empty());
        at(2000);assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());tick(0);
        at(2400);assert(commands.front().line==std::string("transcription next ")+id);
        answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":false,\"draftVersion\":2,\"draft\":\"still talking now\"}");tick(0);
        assert(patches==std::vector<std::string>{"trl0"}&&creates==1);patches.clear();
        at(3000);assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());tick(0);
        at(3200);assert(commands.front().line==std::string("transcription next ")+id);
        at(5000);assert(commands.size()==1); // a slow request cannot overlap another
        answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":true,\"sequence\":1,\"offset\":0,\"length\":5,\"sttText\":\"hello\",\"draftVersion\":3,\"draft\":\"\"}");
        tick(1);assert(commands.front().line==std::string("transcription ack ")+id+" 1 0 5");answer("{\"success\":true}");
        tick(1);assert(commands.front().line==std::string("transcription next ")+id);
        answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":false,\"draftVersion\":3,\"draft\":\"\"}");
        tick(1);assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());
        // ACK did not reset either clock; the overdue status cannot starve.
        assert(creates==1&&std::string(state.tail)=="hello");clear();
    }
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
    g2TranscriptionHandleTap(2);drainUi();enqueueRender();drainUi();assert(displayed.find("Hello world!")!=std::string::npos&&navCallback&&exitCallback);
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
    // A live compound is created once; only changed text bands are patched.
    clear();home();start();std::strcpy(state.draft,"a few new words");
    g2TranscriptionHandleTap(2);drainUi();assert(creates==1&&lineText.size()==4&&lineText[0]=="Live text"&&displayed.find("Loading text...")!=std::string::npos);enqueueRender();drainUi();
    assert(displayed.find("a few new words")!=std::string::npos);patches.clear();
    std::strcpy(state.draft,"a few new words added");enqueueRender();drainUi();
    assert(creates==1&&patches==std::vector<std::string>{"trl0"});
    patches.clear();enqueueRender();drainUi();assert(patches.empty());
    // Shorter replacements clear suffixes and emptied line slots.
    std::strcpy(state.draft,"first line\nsecond line\nthird line\nfourth line\nfifth line\nsixth line\nseventh line");enqueueRender();drainUi();
    assert(displayed.find("third line")!=std::string::npos);
    std::strcpy(state.draft,"first");enqueueRender();drainUi();
    assert(lineText[1]=="~ first"&&lineText[2].empty()&&lineText[3].empty()&&creates==1);
    // A failed line update retries itself, never destroys the page or resends successful siblings.
    failedChild="trl1";patches.clear();std::strcpy(state.draft,"new first\nnew second\nnew third\nnew fourth");enqueueRender();drainUi();
    assert(creates==1&&state.dirty&&patches==std::vector<std::string>{"trl0"});
    failedChild.clear();patches.clear();enqueueRender();drainUi();
    assert(creates==1&&patches==std::vector<std::string>{"trl1"}&&!state.dirty);
    // A queued swap cannot receive patches or trigger another CREATE.
    swapPending=true;patches.clear();std::strcpy(state.draft,"queued");enqueueRender();drainUi();
    assert(creates==1&&patches.empty()&&state.dirty);swapPending=false;enqueueRender();drainUi();
    assert(displayed.find("queued")!=std::string::npos);
    // Lost presentation / same-owner re-entry recreates and repopulates once.
    textReady=false;enqueueRender();drainUi();assert(creates==2);enqueueRender();drainUi();
    assert(displayed.find("queued")!=std::string::npos);
    g2ShowTranscriptionMenu();drainUi();g2TranscriptionHandleTap(2);drainUi();assert(creates==3);enqueueRender();drainUi();
    assert(displayed.find("queued")!=std::string::npos);
    // Valid UTF-8 stays intact at line/byte boundaries, including unbroken words.
    std::string unicode;for(int i=0;i<100;++i)unicode+=u8"界é🙂";
    size_t lines=splitLiveLines(unicode.c_str());std::string joined;
    for(size_t i=0;i<lines;++i){size_t n=std::strlen(layoutLines[i]);assert(n<=kLiveLineBytes);assert(TranscriptionUI::completePrefix(layoutLines[i],n)==n);joined+=layoutLines[i];}
    assert(joined==unicode);
    std::string boundedUnicode=unicode.substr(0,450);assert(TranscriptionUI::completePrefix(boundedUnicode.c_str(),boundedUnicode.size())==boundedUnicode.size());
    std::strcpy(state.draft,boundedUnicode.c_str());state.page=255;state.followTail=true;enqueueRender();drainUi();
    for(size_t i=1;i<lineText.size();++i){assert(lineText[i].size()<=180);assert(TranscriptionUI::completePrefix(lineText[i].c_str(),lineText[i].size())==lineText[i].size());}
    // New text follows the most recent eight lines; paging back stays put.
    std::strcpy(state.draft,"one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine");state.page=255;state.followTail=true;enqueueRender();drainUi();
    assert(state.pages==2&&state.page==1&&lineText[1]=="two\nthree\nfour"&&lineText[3]=="eight\nnine");
    navCallback(G2_TAP_PAGE_PREV);drainUi();assert(state.page==0&&lineText[1]=="~ one\ntwo\nthree");
    state.sequence=1;appendTailLocked("finished",8,true);assert(state.page==0);
    std::strcpy(state.draft,"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq");state.page=255;state.followTail=true;enqueueRender();drainUi();
    clockMs+=200;navCallback(G2_TAP_PAGE_PREV);drainUi();assert(state.page==1);
    clockMs+=20;navCallback(G2_TAP_PAGE_PREV);drainUi();assert(state.page==1);
    clockMs+=200;navCallback(G2_TAP_PAGE_PREV);drainUi();assert(state.page==0);
    // Phrase labels arrive after ACK, without duplicating text or rebuilding.
    clear();home();start();g2TranscriptionHandleTap(2);drainUi();tick(1);tick();
    assert(commands.front().line==std::string("transcription next ")+id);
    answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":true,\"sequence\":1,\"offset\":0,\"length\":5,\"sttText\":\"hello\",\"speakersAvailable\":true,\"speakers\":[{\"sequence\":1,\"speaker\":-1,\"ready\":false}]}");tick(1);
    assert(displayed.find("?: hello")!=std::string::npos);answer("{\"success\":true}");tick();
    assert(commands.front().line==std::string("transcription status ")+id);answer(status(true).c_str());tick(1);
    assert(commands.front().line==std::string("transcription next ")+id);
    answer("{\"success\":true,\"exchange\":\"0123456789abcdef\",\"available\":false,\"speakersAvailable\":true,\"speakerVersion\":2,\"speakers\":[{\"sequence\":1,\"speaker\":1,\"ready\":true}]}");tick(1);
    assert(displayed.find("Person 2: Hello")!=std::string::npos&&std::string(state.tail)=="hello"&&creates==1);
    // A foreign reply cannot relabel; trimming preserves sequence associations.
    tick(800u-uint32_t(clockMs-state.lastTextPoll));assert(commands.front().line==std::string("transcription next ")+id);
    answer("{\"success\":true,\"exchange\":\"fedcba9876543210\",\"available\":false,\"speakers\":[{\"sequence\":1,\"speaker\":8,\"ready\":true}]}");tick(1);
    assert(state.phrases[0].speaker==1);
    for(unsigned i=2;i<30;++i){state.sequence=i;std::string phrase(70,'a'+i%20);appendTailLocked(phrase.c_str(),phrase.size(),true);}
    assert(state.phraseCount<=kPhraseCount&&state.phrases[state.phraseCount-1].sequence==29);
    for(size_t i=0;i<state.phraseCount;++i){assert(state.phrases[i].begin<state.phrases[i].end&&state.phrases[i].end<=std::strlen(state.tail));}
    // A fresh admission resets draft, labels, and dedup receipts from the previous run.
    state.active=false;g2ShowTranscriptionMenu();drainUi();commands.clear();state.pending=Op::None;state.queued=Op::None;
    g2TranscriptionHandleTap(1);tick(1);answer(status(true).c_str());
    assert(!state.phraseCount&&!state.draft[0]&&!state.speakersAvailable&&!state.receiptLength);
    ownerAuthed=false;tick(1);assert(!state.epoch&&!state.phraseCount&&!labelledText[0]&&!renderedLive[1][0]);

    // Thirteen same-person phrases form one paragraph without losing their
    // receipt/label identities. These fixtures are synthetic, not recordings.
    liveFixture();{
        std::string words;
        for(unsigned i=1;i<=13;++i){std::string chunk="topic"+std::to_string(i)+" continues";if(!words.empty())words+=' ';words+=chunk;phraseReply(i,chunk,0);}
        std::string paragraph=words;paragraph[0]=char(paragraph[0]-('a'-'A'));  // the turn starts with a capital; the tail stays raw
        assert(std::string(labelledText)=="Person 1: "+paragraph&&std::string(state.tail)==words&&state.phraseCount==13&&creates==1);
        patches.clear();labelReply({{13,0},{1,0}});assert(patches.empty()&&creates==1);
    }
    // Unknown gaps do not inherit a known ID. Late, out-of-order labels can
    // join and split existing runs without recreating the lens page.
    liveFixture();phraseReply(1,"alpha",0);phraseReply(2,"beta");phraseReply(3,"gamma",0);
    assert(std::string(labelledText)=="Person 1: Alpha\n?: beta\nPerson 1: Gamma");
    labelReply({{3,0},{2,0}});assert(std::string(labelledText)=="Person 1: Alpha beta gamma");
    labelReply({{2,1}});assert(std::string(labelledText)=="Person 1: Alpha\nPerson 2: Beta\nPerson 1: Gamma"&&creates==1);
    // Unknown final phrases stay separate from each other and the live draft.
    phraseReply(4,"delta");phraseReply(5,"epsilon");draftReply("maybe zeta");
    assert(std::string(labelledText)=="Person 1: Alpha\nPerson 2: Beta\nPerson 1: Gamma\n?: delta\n?: epsilon\n~ maybe zeta");
    const uint32_t pendingDraftVersion=state.draftVersion;
    patches.clear();labelReply({{4,-1},{5,-1}}); // completed but unassigned is still unknown
    assert(patches.empty()&&state.draftVersion==pendingDraftVersion&&std::string(state.draft)=="maybe zeta");
    labelReply({{5,2}});assert(std::string(labelledText).find("?: delta\nPerson 3: Epsilon\n~ maybe zeta")!=std::string::npos);
    labelReply({{4,2}});assert(std::string(labelledText).find("Person 3: Delta epsilon\n~ maybe zeta")!=std::string::npos);
    assert(state.draftVersion==pendingDraftVersion&&std::string(state.tail)=="alpha beta gamma delta epsilon"&&creates==1);
    draftReply("zeta");assert(std::string(labelledText).find("maybe")==std::string::npos);
    phraseReply(6,"zeta",0);assert(std::string(labelledText).find("~")==std::string::npos&&std::string(labelledText).find("Person 1: Zeta")!=std::string::npos);
    // Receipt fragments of the same unknown phrase still form one line.
    // A new phrase starts another unknown line; only matching known labels join them.
    liveFixture();phraseReply(1,"inter");phraseReply(1,"national",-1,5);
    assert(std::string(labelledText)=="?: international"&&state.phraseCount==1);
    patches.clear();phraseReply(1,"national",-1,5);assert(std::string(state.tail)=="international"&&patches.empty());
    phraseReply(2,"team");assert(std::string(labelledText)=="?: international\n?: team"&&state.phraseCount==2);
    labelReply({{2,0}});assert(std::string(labelledText)=="?: international\nPerson 1: Team");
    labelReply({{1,0}});assert(std::string(labelledText)=="Person 1: International team"&&creates==1);
    // Speaker-disabled text flows across finalization boundaries, preserving
    // supplied punctuation. Draft replacement remains visibly provisional.
    liveFixture();phraseReply(1,"hello,",-1,0,false);phraseReply(2,"world!",-1,0,false);draftReply("more words");
    assert(std::string(labelledText)=="Hello, world!\n~ more words");
    phraseReply(3,"more words",-1,0,false);assert(std::string(labelledText)=="Hello, world! more words");
    // Metadata eviction preserves the orphan text, with neutral attribution.
    liveFixture();{
        std::string prefix;
        for(unsigned i=1;i<=20;++i){std::string chunk="word"+std::to_string(i);phraseReply(i,chunk,0);if(i<=4){if(!prefix.empty())prefix+=' ';prefix+=chunk;}}
        assert(state.phraseCount==16&&std::string(labelledText).find("?: "+prefix+"\nPerson 1: Word5")==0);
        assert(std::string(labelledText).find("word20")!=std::string::npos);
    }
    // Tail eviction may clip a retained phrase. Its surviving UTF-8 bytes
    // keep their identity and all text children remain within transport bounds.
    liveFixture();{
        std::string unicode;for(int i=0;i<80;++i)unicode+=u8"界é ";
        phraseReply(1,unicode,0);phraseReply(2,unicode,0);phraseReply(3,unicode,0);
        assert(state.tailOrigin>0&&state.phraseCount==3&&state.phrases[0].begin==0);
        std::string retained=state.tail;while(!retained.empty()&&retained.back()==' ')retained.pop_back();
        while(!retained.empty()&&retained.front()==' ')retained.erase(0,1);
        // Only whitespace at phrase joins is normalized; every UTF-8 token remains.
        auto tokens=[](const std::string& value){std::string result;for(char c:value)if(c!=' ')result+=c;return result;};
        std::string shownWords=std::string(labelledText).substr(10);
        if(!shownWords.empty()&&shownWords[0]>='A'&&shownWords[0]<='Z')shownWords[0]=char(shownWords[0]+('a'-'A'));  // display casing of the turn start only
        assert(tokens(shownWords)==tokens(retained));
        for(const auto& line:lineText)assert(line.size()<=180&&TranscriptionUI::completePrefix(line.c_str(),line.size())==line.size());
        assert(creates==1);
    }
    // Metadata-only grouping changes preserve follow-tail, and reading older
    // text stays anchored even when its new line number lies between pages.
    liveFixture();{
        for(unsigned i=1;i<=16;++i)phraseReply(i,"word"+std::to_string(i)+" has several extra words in this sentence here",i%2);
        assert(state.followTail&&state.liveFirstLine==state.liveNewestLine&&state.pages>1);
        scroll(G2_TAP_PAGE_PREV);assert(!state.followTail);
        const LiveAnchor reading=state.liveAnchor;
        labelReply({{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,0},{12,0},{13,0},{14,0},{15,0},{16,0}});
        assert(!state.followTail&&state.liveAnchor.offset==reading.offset&&!state.liveAnchor.draft);
        const std::string anchored=std::string(state.tail).substr(reading.offset-state.tailOrigin,6);
        assert(displayed.find(anchored)!=std::string::npos);
        const unsigned reflowedFirst=state.liveFirstLine;
        scroll(G2_TAP_PAGE_PREV);assert(state.liveFirstLine<reflowedFirst&&reflowedFirst-state.liveFirstLine<=8);
        // Return to newest, then split labels without new text/draft versions.
        while(!state.followTail)scroll(G2_TAP_PAGE_NEXT);
        labelReply({{2,1},{4,1},{6,1},{8,1},{10,1},{12,1},{14,1},{16,1}});
        assert(state.followTail&&state.liveFirstLine==state.liveNewestLine&&displayed.find("Word16")!=std::string::npos&&creates==1);  // its own turn now: capitalized
    }
    // Navigation arriving after the render snapshot cannot be overwritten by
    // that stale snapshot's follow-tail anchor/layout commit.
    liveFixture();{
        for(unsigned i=1;i<=16;++i)phraseReply(i,"line"+std::to_string(i)+" contains enough words to wrap this phrase twice",i%2);
        assert(state.followTail&&state.liveNewestLine>8);
        snapshotHook=[](){navCallback(G2_TAP_PAGE_PREV);};enqueueRender();drainUi();
        assert(!state.followTail&&state.liveFirstLine<state.liveNewestLine&&creates==1);
    }
    // A reflow can land within the first page: Prev must still reach line0.
    // Shrinking an older view to the end must not silently resume following;
    // explicit Next at that position does resume it.
    liveFixture();{
        for(unsigned i=1;i<=16;++i)phraseReply(i,"unit"+std::to_string(i),i%2);
        scroll(G2_TAP_PAGE_PREV);assert(!state.followTail);
        // Read a source position that lands partway through the first grouped paragraph.
        state.liveAnchor={uint32_t(state.phrases[8].begin),false,true};state.followTail=false;enqueueRender();drainUi();
        labelReply({{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,0},{12,0},{13,0},{14,0},{15,0},{16,0}});
        assert(!state.followTail);scroll(G2_TAP_PAGE_NEXT);assert(state.followTail);
        // A longer retained paragraph gives an intermediate first-page anchor.
        clear();home();start();g2TranscriptionHandleTap(2);drainUi();enqueueRender();drainUi();
        std::string text;for(int i=0;i<60;++i)text+="token"+std::to_string(i)+" ";
        phraseReply(1,text,0);state.followTail=false;state.liveAnchor={80,false,true};enqueueRender();drainUi();
        assert(state.liveFirstLine>0&&state.liveFirstLine<8&&state.page==0);
        scroll(G2_TAP_PAGE_PREV);assert(state.liveFirstLine==0&&!state.followTail);
    }
    // An old draft's reading offset must not attach to a new utterance after
    // finalization. Fall back to the committed boundary when lineage is unknown.
    liveFixture();{
        std::string longDraft;for(int i=0;i<65;++i)longDraft+="draft ";draftReply(longDraft);
        scroll(G2_TAP_PAGE_PREV);assert(!state.followTail&&state.liveAnchor.draft);
        JsonDocument doc;doc["available"]=true;doc["sequence"]=1;doc["offset"]=0;
        doc["sttText"]=longDraft;doc["length"]=longDraft.size();doc["draftVersion"]=state.draftVersion+1;doc["draft"]="different utterance";
        nextReply(doc);assert(!state.followTail&&!state.liveAnchor.draft&&displayed.find("different utterance")==std::string::npos);
        assert(displayed.find("draft")!=std::string::npos&&creates==1);
    }
    // If a draft vanishes before any final text arrives, a later draft must
    // not pull a reader at the retained-history boundary to its newest lines.
    liveFixture();{
        std::string oldDraft;for(int i=0;i<65;++i)oldDraft+="older ";draftReply(oldDraft);
        scroll(G2_TAP_PAGE_PREV);assert(!state.followTail&&state.liveAnchor.draft);
        draftReply("");assert(!state.liveAnchor.draft&&!state.followTail);
        std::string newDraft;for(int i=0;i<65;++i)newDraft+="newer ";draftReply(newDraft);
        assert(state.liveFirstLine==0&&state.liveNewestLine>0&&!state.followTail);
        scroll(G2_TAP_PAGE_NEXT);assert(state.followTail&&state.liveFirstLine==state.liveNewestLine);
    }
    clear();std::puts("G2 transcription actual-source ownership/navigation/receipt tests passed");
}
