#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#define ENABLE_OLED_DISPLAY 1
#define ENABLE_DICTATION 1
#define EXT_RAM_BSS_ATTR
#define REGISTER_OLED_MODE_MODULE(...)
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
// INSERT_POLICY
class String {
 public:
  std::string value;
  String()=default;
  String(const char* text):value(text?text:""){}
  String(const std::string& text):value(text){}
  template<class T, typename std::enable_if<std::is_integral<T>::value,int>::type = 0>
  String(T number):value(std::to_string(number)){}
  size_t length()const{return value.size();}
  const char* c_str()const{return value.c_str();}
  void setCharAt(size_t at,char c){value.at(at)=c;}
  String operator+(const String& rhs)const{return value+rhs.value;}
};
using TransportSessionEpoch=uint32_t;
using ExecAsyncCallback=void(*)(bool,const char*,void*);
constexpr int COMMAND_CONTEXT_MODE_INDEPENDENT=1;
enum CommandSource{SOURCE_INTERNAL,SOURCE_LOCAL_DISPLAY};
struct DictationAppLease{CommandSource source;TransportSessionEpoch epoch;uint64_t exchange;};
static std::vector<DictationAppLease> cancellations;
bool dictationAppCancel(const DictationAppLease& lease){cancellations.push_back(lease);return true;}
static uint32_t clockMs=1000,liveEpoch=10;
static bool authed=true,guest=false,queueOkay=true;
uint32_t millis(){return clockMs;}
TransportSessionEpoch localDisplayTransportSessionSnapshot(String& user,bool& yes){yes=authed;user=authed?"alice":"";return liveEpoch;}
bool oledGuestBlocksMutate(){return guest;}
struct Command {String line;struct{TransportSessionEpoch transportSessionEpoch;unsigned behaviorFlags=0;}ctx;};
static Command buildOLEDCommand(const String& line){Command cmd;cmd.line=line;cmd.ctx.transportSessionEpoch=liveEpoch;return cmd;}
struct Request{Command command;ExecAsyncCallback callback;void* context;};
static std::vector<Request> requests;
bool submitCommandAsync(const Command& command,ExecAsyncCallback callback,void* opaque){if(!queueOkay)return false;requests.push_back({command,callback,opaque});return true;}
// INSERT_BRIDGE
struct OLEDScrollState {const char* items[16]{};int count=0,selectedIndex=0,visibleLines=4;};
enum OLEDMode {OLED_MENU,OLED_TRANSCRIPTION,OLED_TRANSCRIPTION_LIVE,OLED_TRANSCRIPTS,OLED_TRANSCRIPT_VIEW};
static OLEDMode currentOLEDMode=OLED_MENU;
static struct{bool up=false,down=false;}gNavEvents;
constexpr uint32_t INPUT_BUTTON_A=1,INPUT_BUTTON_B=2,INPUT_BUTTON_X=4;
#define INPUT_CHECK(buttons,mask) ((buttons)&(mask))
static std::string toast;
void oledToastShow(const char* text,unsigned){toast=text;}
void oledMarkDirty(){}
void requestOLEDMode(OLEDMode mode,const char*){currentOLEDMode=mode;}
constexpr int OLED_CONTENT_START_Y=12;
struct Display{std::string printed,body;void setTextSize(int){}void setCursor(int,int){}void write(char c){body+=c;}void print(const char* text){printed=text;}};
static Display display;
static Display* oledDisplay=&display;
void oledScrollInit(OLEDScrollState* s,const char*,int visible){*s={};s->visibleLines=visible;}
void oledScrollClearKeepSelection(OLEDScrollState* s){s->count=0;}
void oledScrollAddItem(OLEDScrollState* s,const char* text){assert(s->count<16);s->items[s->count++]=text;}
void oledScrollClampSelection(OLEDScrollState* s){s->selectedIndex=std::max(0,std::min(s->selectedIndex,s->count-1));}
bool oledScrollHandleNav(OLEDScrollState* s){if(gNavEvents.up){if(s->selectedIndex)--s->selectedIndex;return true;}if(gNavEvents.down){if(s->selectedIndex+1<s->count)++s->selectedIndex;return true;}return false;}
void oledScrollRenderSimple(Display*,OLEDScrollState*,bool=false,int=0){}
struct OLEDModeEntry{OLEDMode mode;const char* name;const char* icon;void(*draw)();bool(*available)();bool(*input)(int,int,uint32_t);bool shown;int order;const char* hint;};
void resetOLEDTranscription();
// INSERT_PRODUCTION
static const char* id="0123456789abcdef";
static std::string status(bool active,const char* exchange=id){return std::string("{\"success\":true,\"exchange\":\"")+exchange+"\",\"active\":"+(active?"true":"false")+",\"busy\":false,\"available\":true,\"continuous\":true,\"preparing\":false,\"captureActive\":"+(active?"true":"false")+",\"saveDefault\":false}";}
static void tick(unsigned ms=1000){clockMs+=ms;prepareTranscriptionData();}
static std::string request(){assert(requests.size()==1);assert(requests[0].command.ctx.behaviorFlags&COMMAND_CONTEXT_MODE_INDEPENDENT);return requests[0].command.line.value;}
static void answer(const std::string& text,bool okay=true){assert(requests.size()==1);auto request=requests.front();requests.clear();std::thread worker([&]{request.callback(okay,text.c_str(),request.context);});worker.join();}
static void reset(){
  // Complete owned callbacks even when abandoning a test scenario.
  while(!requests.empty()){answer("{\"success\":false,\"error\":\"test cleanup\"}",false);currentOLEDMode=OLED_MENU;tick(1);}
  resetOLEDTranscription();v=View{};inFlight=false;ready=false;pending=String();pendingAck=String();
  currentOLEDMode=OLED_MENU;authed=queueOkay=true;guest=false;liveEpoch=10;clockMs=1000;gNavEvents={};cancellations.clear();toast.clear();display={};
}
static void home(){currentOLEDMode=OLED_TRANSCRIPTION;tick();assert(request()=="transcription status");answer(status(false,""));tick(1);}
static void start(){v.menu.selectedIndex=0;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="transcription start");answer(status(true));tick(1);}
static void list(){v.menu.selectedIndex=3;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="transcripts list internal 0");answer("{\"success\":true,\"offset\":0,\"nextOffset\":2,\"more\":true,\"entries\":[{\"name\":\"one.txt\",\"path\":\"/stt/u7/one.txt\"},{\"name\":\"two.txt\",\"path\":\"/stt/u7/two.txt\"}]}");tick(1);}
int main(){
  reset();home();start();tick();assert(request()==std::string("transcription next ")+id);
  answer("{\"success\":true,\"available\":true,\"sequence\":1,\"offset\":0,\"length\":6,\"sttText\":\"hello \"}");assert(v.recent[0]==0);tick(1);assert(std::string(v.recent)=="hello ");
  tick();assert(request()==std::string("transcription ack ")+id+" 1 0 6");answer("transport reply lost",false);tick(1);assert(std::string(v.recent)=="hello ");tick();assert(request()==std::string("transcription ack ")+id+" 1 0 6");answer("{\"success\":true}");tick(1);
  tick();assert(request()==std::string("transcription status ")+id);answer(status(true));tick(1);tick();assert(request()==std::string("transcription next ")+id);
  answer("{\"success\":true,\"available\":true,\"sequence\":1,\"offset\":6,\"length\":5,\"sttText\":\"world\"}");tick(1);assert(std::string(v.recent)=="hello world");tick();answer("{\"success\":true}");tick(1);
  // Back invalidates the model and cancels its exact lease only.
  currentOLEDMode=OLED_MENU;tick(1);assert(v.recent[0]==0&&!v.entered&&!cancellations.empty());assert(cancellations.back().exchange==0x0123456789abcdefULL);
  // A start which finishes after exit/re-entry cannot publish into its successor.
  reset();home();v.menu.selectedIndex=0;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="transcription start");currentOLEDMode=OLED_MENU;tick(1);currentOLEDMode=OLED_TRANSCRIPTION;tick(1);assert(requests.size()==1);answer(status(true));tick(1);assert(!v.active&&v.exchange[0]==0&&cancellations.size()==1);assert(request()=="transcription status");answer(status(false,""));tick(1);
  // Queue rejection keeps the action pending and retries without duplicating it.
  queueOkay=false;v.menu.selectedIndex=0;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(requests.empty()&&pending.length());queueOkay=true;tick(1);assert(request()=="transcription start");answer(status(false,""));tick(1);
  // Exact bridge epoch fence is independent of a caller's command contents.
  assert(!submitOLEDCommandForSession("transcription start",99,nullptr,nullptr));
  // Numeric toggle reports ordinary command failure and refreshes state.
  v.menu.selectedIndex=2;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="sttsavetranscripts 1");answer("Error: Admin required",true);tick(1);assert(std::string(v.message)=="Setting change failed");assert(request()=="transcription status");answer(status(false,""));tick(1);
  // List and file reads cannot publish after Back or selecting another file.
  // Mic row cycles reachable sources through micsource, then refreshes status.
  v.micPdm=true;v.micG2=false;std::strcpy(v.micSource,"auto");populateMenu();assert(std::string(v.menu.items[5])=="Mic: Auto");
  v.menu.selectedIndex=5;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="micsource pdm");answer("Mic source preference set to 'pdm'",true);tick(1);assert(std::string(v.message)=="Setting updated");
  assert(request()=="transcription status");answer("{\"success\":true,\"available\":true,\"micSource\":\"pdm\",\"micPdm\":true,\"micG2\":false}");tick(1);
  populateMenu();assert(std::string(v.menu.items[5])=="Mic: Onboard");
  v.menu.selectedIndex=5;menuInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="micsource auto");answer("ok",true);tick(1);answer(status(false,""));tick(1);
  list();v.list.selectedIndex=0;listInput(0,0,INPUT_BUTTON_A);tick(1);assert(request()=="transcripts read \"/stt/u7/one.txt\" 0");currentOLEDMode=OLED_TRANSCRIPTS;tick(1);v.list.selectedIndex=1;listInput(0,0,INPUT_BUTTON_A);tick(1);assert(requests.size()==1);answer("{\"success\":true,\"sttText\":\"late private one\",\"offset\":0,\"nextOffset\":10,\"eof\":true}");tick(1);assert(v.text[0]==0);assert(request()=="transcripts read \"/stt/u7/two.txt\" 0");answer("{\"success\":true,\"sttText\":\"second file\",\"offset\":0,\"nextOffset\":509,\"eof\":false}");tick(1);assert(std::string(v.text)=="second file");
  gNavEvents.down=true;fileInput(0,0,0);gNavEvents={};tick(1);assert(request()=="transcripts read \"/stt/u7/two.txt\" 509");answer("{\"success\":true,\"sttText\":\"next window\",\"offset\":509,\"nextOffset\":1021,\"eof\":false}");tick(1);gNavEvents.up=true;fileInput(0,0,0);gNavEvents={};tick(1);assert(request()=="transcripts read \"/stt/u7/two.txt\" 0");answer("{\"success\":true,\"sttText\":\"first window\",\"offset\":0,\"nextOffset\":509,\"eof\":false}");tick(1);
  // A queued read is dropped if navigation leaves before command admission.
  queueOkay=false;gNavEvents.down=true;fileInput(0,0,0);gNavEvents={};tick(1);assert(pending.length());currentOLEDMode=OLED_TRANSCRIPTS;tick(1);assert(!pending.length());queueOkay=true;
  // Session boundary reset clears cached private file/live text before rendering.
  std::strcpy(v.recent,"old user");std::strcpy(v.text,"old file");resetOLEDTranscription();liveEpoch=11;currentOLEDMode=OLED_MENU;tick(1);assert(v.recent[0]==0&&v.text[0]==0);
  // Compact display status cannot wrap into the menu; UTF-8 glyph walking is bounded.
  std::memset(v.message,'M',sizeof(v.message)-1);v.message[sizeof(v.message)-1]=0;displayTranscriptionHome();assert(display.printed.size()==21);
  assert(lines("\xc3\xa9\xe2\x82\xac\nend")==2);v.preparing=true;display.body.clear();displayTranscriptionLive();assert(display.body=="Preparing...");
  reset();std::puts("OLED transcription: actual source + bridge lifecycle, receipts, navigation and rendering passed");
}
