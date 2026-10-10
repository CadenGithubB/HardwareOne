#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
using StaticSemaphore_t = Semaphore;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) { return &semaphore; }
static bool admit = true, busAdmit = true, busHeld = false, paused = false;
static bool criticalHeld = false;
static unsigned criticalEntries = 0;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(portMUX_TYPE*) { assert(!criticalHeld); criticalHeld=true; ++criticalEntries; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { assert(criticalHeld); criticalHeld=false; }
static bool cacheHeld() { return semaphore.held || criticalHeld; }
static unsigned long nowMs = 12345;
static std::vector<uint32_t> waits;
static int busCalls = 0;
static std::function<void()> beforeBus, afterBus, rdsHook, seekHook;
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define CACHE_MUTEX_TIMEOUT_MS 100
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  waits.push_back(ms);
  if (!m || m->held || !admit) return 0;
  m->held = true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->held); m->held = false; }
static unsigned long millis() { return nowMs; }
static bool gFmRadioRunning = true, gFmRadioConnected = true, gRadioInitialized = true;
static void* gFmRadioTaskHandle = nullptr;
[[maybe_unused]] static struct { int fmRadioBus = 0; } gSettings;
static bool pollPaused(uint8_t) { return paused; }
[[maybe_unused]] static constexpr int I2C_ADDR_FM_RADIO = 0x11, FM_RADIO_I2C_CLOCK = 100000;
static int initCalls = 0;
static bool initResult = false;
static bool fmRadioInit() {
  ++initCalls;
  if (initResult) gFmRadioConnected = gRadioInitialized = true;
  return initResult;
}

static void callbackAllowed() {
#if !TEST_BASELINE
  assert(!cacheHeld());
#endif
}
template<typename... Args> static void debugLog(const char*, Args...) { callbackAllowed(); }
#define DEBUG_FMRADIO_VALUESF(...) debugLog(__VA_ARGS__)
#define DEBUG_FMRADIO_LIFECYCLEF(...) debugLog(__VA_ARGS__)
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while(false)
static std::vector<std::string> broadcasts, events, banners;
static std::string formatted(const char* fmt, va_list args) {
  char buffer[2048]; const int n=std::vsnprintf(buffer,sizeof(buffer),fmt,args);
  assert(n>=0 && n<static_cast<int>(sizeof(buffer))); return buffer;
}
static void broadcastPrintf(const char* fmt, ...) {
  callbackAllowed(); assert(!busHeld);
  va_list args; va_start(args,fmt); broadcasts.push_back(formatted(fmt,args)); va_end(args);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
enum { SYSEVT_FM_RDS_STATION, SYSEVT_FM_TUNED };
static void systemEventPost(int event, const char* subject, const char* detail="") {
  callbackAllowed();
  events.push_back(std::to_string(event)+":"+subject+":"+detail);
}
class String {
  std::string text;
public:
  String(const char* value): text(value) {}
  void trim() { auto begin=text.find_first_not_of(" \n\r\t"); if(begin==text.npos) { text.clear(); return; }
    text=text.substr(begin,text.find_last_not_of(" \n\r\t")-begin+1); }
  void toLowerCase() { for(char& c:text) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  size_t length() const { return text.size(); }
  const char* c_str() const { return text.c_str(); }
  float toFloat() const { return std::strtof(text.c_str(),nullptr); }
  int toInt() const { return std::atoi(text.c_str()); }
  bool operator!=(const char* rhs) const { return text!=rhs; }
};
struct RADIO_INFO { int rssi=35, snr=8; bool stereo=true, tuned=true; } radioInfo;
static struct Radio {
  uint16_t frequency=9950;
  int seekUpCalls=0, seekDownCalls=0, tuneCalls=0, volumeCalls=0, muteCalls=0, infoCalls=0;
  int volume=6; bool muted=false;
  void checkRDS() { assert(busHeld && !cacheHeld()); if(rdsHook) std::exchange(rdsHook,{})(); }
  void getRadioInfo(RADIO_INFO* out) { assert(busHeld && !cacheHeld()); ++infoCalls; *out=radioInfo; }
  uint16_t getFrequency() { assert(busHeld && !cacheHeld()); return frequency; }
  void seekUp(bool) { assert(busHeld && !cacheHeld()); if(seekHook) std::exchange(seekHook,{})(); ++seekUpCalls; }
  void seekDown(bool) { assert(busHeld && !cacheHeld()); if(seekHook) std::exchange(seekHook,{})(); ++seekDownCalls; }
  void setFrequency(uint16_t value) { assert(busHeld && !cacheHeld()); ++tuneCalls; frequency=value; }
  void setVolume(int value) { assert(busHeld && !cacheHeld()); ++volumeCalls; volume=value; }
  void setMute(bool value) { assert(busHeld && !cacheHeld()); ++muteCalls; muted=value; }
} radio;
template<typename Fn> static void transact(Fn fn) {
  assert(!cacheHeld() && !busHeld); ++busCalls;
  if(beforeBus) std::exchange(beforeBus,{})();
  if(busAdmit) { busHeld=true; fn(); busHeld=false; }
  if(afterBus) std::exchange(afterBus,{})();
}
template<typename Fn> static void i2cDeviceTransactionVoid(uint8_t,int,int,int,Fn fn) { transact(fn); }
template<typename Fn> static void i2cTransactionNACKTolerant(uint8_t,int,int,int,Fn fn) { transact(fn); }
#define ENABLE_OLED_DISPLAY 1
enum class PairingRibbonIcon { INFO_ICON };
static void oledNotificationBannerShow(const char* message,PairingRibbonIcon,int) {
  callbackAllowed(); assert(!busHeld); banners.emplace_back(message);
}

// INSERT_TYPES
// INSERT_STATE

static int trackedSnprintf(char* dst,size_t size,const char* fmt,...) {
  callbackAllowed(); va_list args; va_start(args,fmt); const int n=std::vsnprintf(dst,size,fmt,args); va_end(args); return n;
}
#define snprintf trackedSnprintf
#define OLED_CONTENT_START_Y 8
#define OLED_CONTENT_HEIGHT 48
static int dirty=0;
static void oledMarkDirty() { callbackAllowed(); ++dirty; }
struct Display {
  std::string text;
  void setTextSize(int size) { callbackAllowed(); text+="[size"+std::to_string(size)+"]"; }
  void setCursor(int x,int y) { callbackAllowed(); text+="@"+std::to_string(x)+","+std::to_string(y); }
  void print(const char* value) { callbackAllowed(); text+=value; }
  void print(int value) { callbackAllowed(); text+=std::to_string(value); }
  void println(const char* value) { print(value); text+='\n'; }
  void printf(const char* fmt,...) { callbackAllowed(); va_list args; va_start(args,fmt); text+=formatted(fmt,args); va_end(args); }
} display;
static Display* oledDisplay=&display;
void oledDrawIcon(int x,int y,const char* icon,int size) { callbackAllowed(); display.text+="[icon:"+std::string(icon)+":"+std::to_string(x)+","+std::to_string(y)+","+std::to_string(size)+"]"; }
void oledDrawLevelBars(int x,int y,int level,int max,int height) { callbackAllowed(); display.text+="[bars:"+std::to_string(x)+","+std::to_string(y)+","+std::to_string(level)+","+std::to_string(max)+","+std::to_string(height)+"]"; }

// INSERT_FUNCTIONS

static void reset() {
  DATA={}; semaphore={}; admit=busAdmit=true; busHeld=paused=criticalHeld=false; criticalEntries=0;
#if TEST_BASELINE
  MUTEX=&semaphore;
#endif
  gFmRadioRunning=gFmRadioConnected=gRadioInitialized=true;
  waits.clear(); broadcasts.clear(); events.clear(); banners.clear(); display={}; dirty=busCalls=initCalls=0;
  initResult=false;
  beforeBus=afterBus=rdsHook=seekHook={}; nowMs=12345; radio={}; radioInfo={};
}
static void setSeek(bool seek,bool up=true,unsigned long stamp=12000) {
  DATA.seekInProgress=seek; DATA.seekDirUp=up; DATA.seekStartMs=stamp;
}
static std::string json() {
  char buffer[1024]; const int n=fmRadioBuildDataJSON(buffer,sizeof(buffer));
  assert(n>0 && n==static_cast<int>(strlen(buffer))); return buffer;
}
static void commonOutputs() {
  for(int state=0;state<5;++state) {
    reset(); DATA.dataValid=state!=1; DATA.lastUpdate=43210; DATA.frequency=9950;
    DATA.volume=11; DATA.rssi=42; DATA.headphonesConnected=true;
    DATA.muted=state==2; DATA.stereo=state!=2;
    if(state==3) setSeek(true,false);
    if(state==4) gFmRadioConnected=gRadioInitialized=false;
    if(state!=1) { RDS_ServiceNameCallback("MUSIC FM"); RDS_TextCallback("Artist - Track"); }
    std::cout<<json()<<'\n'; displayFmRadio();
    std::cout<<"OLED:"<<display.text<<"|dirty="<<dirty<<'\n';
  }
  reset(); DATA.dataValid=true;
  RDS_ServiceNameCallback("AB\"C\\D\001\377MORE");
  RDS_TextCallback("hello\n\"radio\\text\177\200");
  const auto before=events.size();
  RDS_ServiceNameCallback("AB\"C\\D\001\377MORE"); RDS_ServiceNameCallback(nullptr); RDS_TextCallback("");
  assert(events.size()==before && strlen(DATA.stationName)==8);
  std::cout<<json()<<'\n'; for(const auto& e:events) std::cout<<"EVENT:"<<e<<'\n';
  const std::string longText(90,'T'); RDS_TextCallback(longText.c_str());
  assert(strlen(DATA.stationText)==64); std::cout<<json()<<'\n';
}

#if !TEST_BASELINE
static void snapshotAndRdsTests() {
  reset(); DATA.dataValid=true; DATA.lastUpdate=789; strcpy(DATA.stationName,"OLD");
  FMRadioCache out;
  assert(fmRadioCacheSnapshot(out) && out.dataValid && out.lastUpdate==789 && criticalEntries==1);
  DATA.stationName[0]='N'; assert(std::string(out.stationName)=="OLD");
  // Timeout remains source-compatible; fixed-size critical copies have no RTOS admission failure.
  for(uint32_t timeout:{0,5,10,50,100}) {
    const auto entries=criticalEntries; assert(fmRadioCacheSnapshot(out,timeout));
    assert(criticalEntries==entries+1 && !cacheHeld() && waits.empty());
  }
  const auto full=json();
  for(size_t capacity=0;capacity<=full.size()+1;++capacity) {
    std::vector<char> bytes(capacity+2,'!'); int n=fmRadioBuildDataJSON(bytes.data()+1,capacity);
    assert(bytes.front()=='!' && bytes.back()=='!');
    if(capacity<=full.size()) assert(n==0); else assert(n==static_cast<int>(full.size()));
  }
  assert(fmRadioBuildDataJSON(nullptr,0)==0 && fmRadioBuildDataJSON(nullptr,100)==0);
  setSeek(true); fmRadioInvalidate();
  assert(!DATA.dataValid && !DATA.seekInProgress && DATA.muted && !DATA.stationName[0] && !cacheHeld());
}
static void seekTests() {
  reset(); nowMs=100;
  beforeBus=[] {  // A preceding bus owner polls before seek admission; no reservation exists yet.
    FMRadioCache pending; assert(fmRadioCacheSnapshot(pending) && !pending.seekInProgress);
    nowMs=1000; updateFMRadio(); assert(!DATA.seekInProgress && events.empty());
  };
  seekHook=[] {  // UI can copy a pending seek; radio polling remains serialized by the held bus.
    FMRadioCache pending; assert(busHeld && fmRadioCacheSnapshot(pending) && pending.seekInProgress);
    assert(!cacheHeld()); nowMs=2000;
  };
  assert(std::string(cmd_fmradio_seek("up"))=="OK" && radio.seekUpCalls==1);
  FMRadioCache pending; assert(fmRadioCacheSnapshot(pending) && pending.seekStartMs==2000);
  assert(std::string(cmd_fmradio_seek("down")).find("already in progress")!=std::string::npos && radio.seekDownCalls==0);
  nowMs=2299; updateFMRadio(); assert(DATA.seekInProgress && events.empty());
  nowMs=2300; updateFMRadio(); assert(!DATA.seekInProgress && DATA.frequency==9950 && events.size()==1);
  assert(events[0]=="1:99.5:seek" && DATA.dataValid && DATA.rssi==35 && DATA.headphonesConnected);
  reset(); nowMs=100; radioInfo.tuned=false;
  assert(std::string(cmd_fmradio_seek(" DOWN "))=="OK" && radio.seekDownCalls==1);
  nowMs=6100; updateFMRadio(); assert(DATA.seekInProgress && events.empty());
  nowMs=6101; updateFMRadio(); assert(!DATA.seekInProgress && events==std::vector<std::string>{"1:99.5:seek timeout"});
  reset(); busAdmit=false; strcpy(DATA.stationName,"OLD");
  assert(std::string(cmd_fmradio_seek("up"))=="Error: [FM Radio] Seek could not start");
  assert(!DATA.seekInProgress && radio.seekUpCalls==0 && criticalEntries==0 && std::string(DATA.stationName)=="OLD");
  busAdmit=true; assert(fmRadioCacheSnapshot(pending) && !pending.seekInProgress);
  assert(std::string(cmd_fmradio_seek("up"))=="OK");  // Failure left no orphan reservation.
  reset(); nowMs=100; assert(std::string(cmd_fmradio_seek("up"))=="OK"); nowMs=500;
  afterBus=[] {  // The older poll must finish its state commit before releasing the bus.
    assert(!DATA.seekInProgress && DATA.frequency==9950); nowMs=800;
    assert(std::string(cmd_fmradio_seek("up"))=="OK");
  };
  updateFMRadio(); assert(DATA.seekInProgress && DATA.seekStartMs==800 && events.size()==1 && DATA.frequency==9950);
  nowMs=1100; updateFMRadio(); assert(!DATA.seekInProgress && events.size()==2);
  reset(); setSeek(true,true,100); nowMs=500; busAdmit=false;
  updateFMRadio(); assert(DATA.seekInProgress && !DATA.dataValid && events.empty());
  busAdmit=true; updateFMRadio(); assert(!DATA.seekInProgress && DATA.dataValid && events.size()==1);
  reset(); rdsHook=[] { RDS_ServiceNameCallback("ON AIR"); RDS_TextCallback("Song"); };
  updateFMRadio(); assert(DATA.dataValid && std::string(DATA.stationName)=="ON AIR" && events.size()==1);
  assert(!cacheHeld() && !busHeld);
}
static void commandTests() {
  reset(); assert(std::string(cmd_fmradio_tune(" 98.5 "))=="OK");
  assert(DATA.frequency==9850 && radio.frequency==9850 && radio.tuneCalls==1 && events==std::vector<std::string>{"1:98.5:"});
  assert(broadcasts==std::vector<std::string>{"Tune command sent: 98.5 MHz"});
  reset(); afterBus=[] {
    assert(DATA.frequency==9850 && radio.frequency==9850);
    assert(std::string(cmd_fmradio_tune("10400"))=="OK");
  };
  assert(std::string(cmd_fmradio_tune("9850"))=="OK");
  assert(radio.frequency==10400 && DATA.frequency==10400);
  reset(); assert(std::string(cmd_fmradio_volume("12"))=="OK" && DATA.volume==12 && radio.volume==12);
  assert(broadcasts==std::vector<std::string>{"Volume command sent: 12"});
  assert(banners==std::vector<std::string>{"Vol: 12/15"});
  reset(); afterBus=[] {
    assert(DATA.volume==12 && radio.volume==12);
    assert(std::string(cmd_fmradio_volume("3"))=="OK");
  };
  assert(std::string(cmd_fmradio_volume("12"))=="OK");
  assert(radio.volume==3 && DATA.volume==3);
  reset(); gFmRadioConnected=false; assert(std::string(cmd_fmradio_volume("9"))=="OK" && DATA.volume==9 && busCalls==0);
  assert(broadcasts==std::vector<std::string>{"Volume set to 9 (will apply when radio starts)"});
  assert(radio.volumeCalls==0 && banners.empty() && events.empty());
  reset(); assert(std::string(cmd_fmradio_mute(""))=="OK" && DATA.muted && radio.muted);
  assert(std::string(cmd_fmradio_unmute(""))=="OK" && !DATA.muted && !radio.muted);
  afterBus=[] { assert(DATA.muted && radio.muted); assert(std::string(cmd_fmradio_unmute(""))=="OK"); };
  assert(std::string(cmd_fmradio_mute(""))=="OK" && !DATA.muted && !radio.muted);
  // A command ordered after polling must not be overwritten when the old poll returns.
  reset(); setSeek(true,true,100); nowMs=500;
  afterBus=[] { assert(DATA.frequency==9950); assert(std::string(cmd_fmradio_tune("10400"))=="OK"); };
  updateFMRadio(); assert(DATA.frequency==10400 && radio.frequency==10400);
}
using Command = const char* (*)(const String&);
static void rejectedCommand(Command command, const char* input) {
  for (bool connected : {false, true}) {
    reset(); gFmRadioConnected=gRadioInitialized=connected; initResult=true;
    DATA.dataValid=true; DATA.frequency=10130; DATA.volume=5;
    strcpy(DATA.stationName,"OLD"); strcpy(DATA.stationText,"Old song");
    const auto before=json();
    assert(std::string(command(input)).find("Error:")==0);
    assert(json()==before && busCalls==0 && initCalls==0);
    assert(radio.tuneCalls==0 && radio.volumeCalls==0 && radio.muteCalls==0);
    assert(radio.seekUpCalls==0 && radio.seekDownCalls==0);
    assert(broadcasts.empty() && events.empty() && banners.empty());
  }
}
static void invalidCommandTests() {
  for (const char* input : {"", " ", "NaN", "nan(1)", "inf", "-inf", "1e309", "1e-309",
                           "99999999999999999999999999999999999999", "75436", "-100", "75.99",
                           "108.01", "7599", "10801", "103.9MHz", "9850 tail", "10390 0", "0", "200"}) {
    rejectedCommand(cmd_fmradio_tune,input);
  }
  for (const char* input : {"abc", "9foo", "1.5", "NaN", "1e1", "0x8", "16", "-1",
                           "4294967296", "18446744073709551616", "-18446744073709551616", "9 8", "+", "-"}) {
    rejectedCommand(cmd_fmradio_volume,input);
  }
  for (const char* input : {"sideways", "up down", "down 1"}) rejectedCommand(cmd_fmradio_seek,input);
  for (const char* input : {"moot", "of", "on off", "0", "1"}) rejectedCommand(cmd_fmradio_mute,input);
  for (const char* input : {"on", "off", "garbage"}) rejectedCommand(cmd_fmradio_unmute,input);
}
static void blockedCommandTests() {
  const std::vector<std::pair<Command,const char*>> commands = {
    {cmd_fmradio_tune,"99.5"}, {cmd_fmradio_volume,"12"}, {cmd_fmradio_mute,""},
    {cmd_fmradio_mute,"off"}, {cmd_fmradio_unmute,""}, {cmd_fmradio_seek,"up"}
  };
  for (const auto& [command,input] : commands) {
    reset(); busAdmit=false; DATA.dataValid=true; DATA.muted=command==cmd_fmradio_unmute;
    strcpy(DATA.stationName,"OLD"); strcpy(DATA.stationText,"Old song");
    const auto before=json();
    assert(std::string(command(input)).find("Error:")==0);
    assert(json()==before && busCalls==1 && initCalls==0);
    assert(radio.tuneCalls==0 && radio.volumeCalls==0 && radio.muteCalls==0);
    assert(radio.seekUpCalls==0 && radio.seekDownCalls==0);
    assert(broadcasts.empty() && events.empty() && banners.empty());
    // A later accepted command can run; failure leaves no pending state.
    busAdmit=true; assert(std::string(command(input))=="OK");
    assert(busCalls==2 && broadcasts.size()==1);
  }
  reset(); gFmRadioConnected=gRadioInitialized=false;
  assert(std::string(cmd_fmradio_tune("99.5")).find("Not initialized")!=std::string::npos);
  assert(initCalls==1 && busCalls==0 && events.empty() && broadcasts.empty());
  reset(); gFmRadioConnected=gRadioInitialized=false; initResult=true;
  assert(std::string(cmd_fmradio_tune("99.5"))=="OK");
  assert(initCalls==1 && busCalls==1 && radio.tuneCalls==1 && DATA.frequency==9950);
}
static void acceptedArgumentTests() {
  for (const auto& [input,frequency] : std::vector<std::pair<const char*,int>>{
         {"76",7600}, {"108.0",10800}, {"7600",7600}, {"10800",10800},
         {"103.9",10390}, {"10390",10390}, {" 80.57 ",8057}}) {
    reset(); assert(std::string(cmd_fmradio_tune(input))=="OK");
    assert(radio.tuneCalls==1 && radio.frequency==frequency && DATA.frequency==frequency);
    assert(events.size()==1 && broadcasts.size()==1);
  }
  for (const auto& [input,volume] : std::vector<std::pair<const char*,int>>{{"0",0},{"15",15},{" +9 ",9}}) {
    reset(); assert(std::string(cmd_fmradio_volume(input))=="OK");
    assert(radio.volumeCalls==1 && radio.volume==volume && DATA.volume==volume && banners.size()==1);
  }
  reset(); assert(std::string(cmd_fmradio_volume("  "))=="OK");
  assert(broadcasts==std::vector<std::string>{"Current volume: 6"} && busCalls==0 && banners.empty());
  for (const auto& [input,muted] : std::vector<std::pair<const char*,bool>>{
         {"",true},{" on ",true},{"MUTE",true},{" OFF ",false},{"unmute",false}}) {
    reset(); DATA.muted=!muted; assert(std::string(cmd_fmradio_mute(input))=="OK");
    assert(radio.muteCalls==1 && radio.muted==muted && DATA.muted==muted);
    assert(broadcasts==std::vector<std::string>{muted ? "FM Radio mute command sent" : "FM Radio unmute command sent"});
  }
  reset(); assert(std::string(cmd_fmradio_unmute("  "))=="OK" && radio.muteCalls==1 && !DATA.muted);
  assert(broadcasts==std::vector<std::string>{"FM Radio unmute command sent"});
  reset(); assert(std::string(cmd_fmradio_seek(""))=="OK" && radio.seekUpCalls==1);
}
#endif
int main() {
  commonOutputs();
#if !TEST_BASELINE
  snapshotAndRdsTests(); seekTests(); commandTests();
  invalidCommandTests(); blockedCommandTests(); acceptedArgumentTests();
#endif
}
