// The production mode below owns the state machine. Only platform edges are fake.
#include "CameraMonochrome.h"
#include "HAL_Camera.h"
#include "HAL_JPEG.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#define ENABLE_OLED_DISPLAY 1
#define ENABLE_CAMERA_SENSOR 1
#define DISPLAY_WIDTH 128
#define OLED_CONTENT_HEIGHT (TEST_HEIGHT-21)
#define OLED_CONTENT_START_Y 11
#define DISPLAY_COLOR_WHITE 1
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if(validateOnly)return "VALID"; } while(0)
constexpr unsigned GLOBAL_DEBUG_BUFFER_SIZE=1024;
static char replyBuffer[GLOBAL_DEBUG_BUFFER_SIZE];
static bool replyBufferAvailable=true;
bool ensureDebugBuffer(){return replyBufferAvailable;}
char* getDebugBuffer(){return replyBuffer;}
using String=std::string;
using TransportSessionEpoch=uint64_t;
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static unsigned criticalDepth=0;
#define portENTER_CRITICAL(p) do{(p)->lock();++criticalDepth;}while(0)
#define portEXIT_CRITICAL(p) do{--criticalDepth;(p)->unlock();}while(0)
constexpr int SOURCE_LOCAL_DISPLAY=1,pdPASS=1,TASK_PRIORITY_LOW=1,APP_CORE=1;
static uint32_t nowMs=10000;
uint32_t millis(){return nowMs;}
#define pdMS_TO_TICKS(n) (n)
static std::function<void()> onDelay,onCapture,onDecode;
void vTaskDelay(unsigned n){nowMs+=n;if(onDelay)onDelay();}
void vTaskDelete(void*){}
unsigned uxTaskGetStackHighWaterMark(void*){return 6000;}
void taskStackRecord(const char*,unsigned){}
static void(*task)(void*)=nullptr;
static bool refuseTask=false;
int xTaskCreatePinnedToCore(void(*f)(void*),const char*,unsigned,void*,int,void*,int){task=f;return refuseTask?0:pdPASS;}
static std::set<void*> allocations;
static bool refuseScratch=false;
void* trackedAlloc(size_t n){void* p=std::malloc(n);assert(p);allocations.insert(p);return p;}
void trackedFree(void* p){if(p){assert(allocations.erase(p)==1);std::free(p);}}
enum class AllocPref{PreferPSRAM};
void* ps_alloc(size_t n,AllocPref,const char*){return refuseScratch?nullptr:trackedAlloc(n);}
static bool authed=true,awake=true,guest=false,sourceRunning=true,snapshotBusy=false,decodeFail=false;
static uint64_t liveEpoch=1;
static unsigned captures=0,sessions=0,toasts=0;
bool gOledRunning=true,oledConnected=true,validateOnly=false;
bool displayIsReady(){return true;}
bool powerLocalDisplayAwake(){return awake;}
bool oledIsGuestSession(){return guest;}
uint64_t localDisplayTransportSessionSnapshot(String& user,bool& auth){user="user";auth=authed;return liveEpoch;}
bool transportSessionEpochIsLive(int,uint64_t epoch){return authed&&epoch==liveEpoch;}
enum class CameraSessionKind{Stream,Recording};
void cameraSessionBegin(CameraSessionKind){++sessions;}
void cameraSessionEnd(CameraSessionKind){assert(sessions);--sessions;}
// INSERT_CAMERA_SNAPSHOT
bool cameraStateSnapshot(CameraStateSnapshot& out){out.running=sourceRunning;return !snapshotBusy;}
bool captureCameraFrame(CameraFrame& frame){++captures;if(onCapture)onCapture();frame.data=(uint8_t*)trackedAlloc(8);frame.length=8;return true;}
void cameraFrameRelease(CameraFrame& frame){trackedFree(frame.data);frame={};}
namespace hwjpeg {
Image::~Image(){reset();}
void Image::reset(){trackedFree(pixels);pixels=nullptr;}
bool decode(const uint8_t*,size_t,Image& image,const DecodeOptions&,const char** error){
  if(onDecode)onDecode();if(decodeFail){*error="no memory";return false;}
  image.width=16;image.height=16;image.stride=48;image.size=768;image.pixels=(uint8_t*)trackedAlloc(image.size);
  for(unsigned p=0;p<256;++p)for(unsigned c=0;c<3;++c)image.pixels[p*3+c]=p;
  return true;
}
}
enum class OledUIIcon{ERROR};
namespace oled_message{constexpr unsigned kErrorDurationMs=8000;}
void oledToastShow(const char*,unsigned){++toasts;}
void oledToastShow(const char*,unsigned,OledUIIcon){++toasts;}
void oledMarkDirty(){}
struct {bool up=false,down=false,left=false,right=false;} gNavEvents;
enum {INPUT_BUTTON_A=1,INPUT_BUTTON_B=2,INPUT_BUTTON_X=4,INPUT_BUTTON_Y=8};
#define INPUT_CHECK(buttons,flag) ((buttons)&(flag))
using OledCommandDoneFn=void(*)(bool,const void*);
static std::vector<std::string> commands;
static OledCommandDoneFn completion=nullptr;
static const void* completionContext=nullptr;
static bool acceptCommand=true;
bool oledRunCommandAsync(const char* cmd,OledCommandDoneFn fn,const void* context){
  if(!acceptCommand)return false;commands.emplace_back(cmd);completion=fn;completionContext=context;return true;
}
struct Display {
  void setTextSize(int){}void setCursor(int,int){}void print(const char*){}void println(const char*){}
  void drawBitmap(int x,int y,const uint8_t*,unsigned w,unsigned h,int){
    assert(!criticalDepth&&x==0&&w==128&&y>=OLED_CONTENT_START_Y&&y+h<=OLED_CONTENT_START_Y+OLED_CONTENT_HEIGHT);
  }
} display;
static Display* oledDisplay=&display;
// INSERT_OLED_ENUM
static OLEDMode currentOLEDMode=OLED_CAMERA;
using OLEDDisplayFunc=void(*)();using OLEDAvailabilityFunc=bool(*)(String*);
using OLEDInputFunc=bool(*)(int,int,uint32_t);using OLEDModeEnterFunc=void(*)(bool);
// INSERT_OLED_ENTRY
#define REGISTER_OLED_MODE_MODULE(...)
#define free trackedFree
// INSERT_PRODUCTION
#undef free

static void enter() {
  resetOLEDCameraPreview();currentOLEDMode=OLED_CAMERA;authed=true;awake=true;guest=false;sourceRunning=true;
  nowMs+=4000;prepareCameraPreviewData();assert(snapshot().active&&snapshot().worker&&task);
}
int main() {
  enter();unsigned delays=0;
  onDelay=[&]{++delays;if(delays==1){assert(snapshot().ready);draw();input(0,0,INPUT_BUTTON_A);}
    else if(delays==2){assert(snapshot().frozen&&captures==1);input(0,0,INPUT_BUTTON_A);}
    else {assert(captures==2);currentOLEDMode=OLED_MENU;prepareCameraPreviewData();}};
  task(nullptr);assert(!snapshot().worker&&!snapshot().ready&&!sessions&&allocations.empty());
  assert(!input(0,0,INPUT_BUTTON_B));
  // Stopping an externally paused source must clear hold, so restarting can
  // produce a frame rather than waiting for an impossible resume of no image.
  enter();delays=0;
  onDelay=[&]{++delays;if(delays==1){input(0,0,INPUT_BUTTON_A);sourceRunning=false;}
    else if(delays==2){assert(!snapshot().ready&&!snapshot().frozen);sourceRunning=true;}
    else {assert(snapshot().ready);currentOLEDMode=OLED_MENU;prepareCameraPreviewData();}};
  task(nullptr);assert(allocations.empty()&&!sessions);
  enter();onDelay=[&]{currentOLEDMode=OLED_MENU;prepareCameraPreviewData();};
  onDecode=[&]{currentOLEDMode=OLED_MENU;prepareCameraPreviewData();};
  task(nullptr);assert(!snapshot().ready&&!snapshot().sequence&&allocations.empty()&&!sessions);onDecode=nullptr;
  enter();onDecode=[&]{authed=false;liveEpoch=2;prepareCameraPreviewData();};
  task(nullptr);assert(!snapshot().ready&&!snapshot().active&&allocations.empty()&&!sessions);onDecode=nullptr;
  enter();decodeFail=true;onDelay=[&]{assert(strstr(snapshot().error,"no memory"));currentOLEDMode=OLED_MENU;prepareCameraPreviewData();};
  task(nullptr);assert(allocations.empty()&&!sessions);decodeFail=false;
  // Photo waits until this preview's capture/decode has drained; frozen frames
  // reject a late producer, and a completed save restores the prior live state.
  enter();commands.clear();unsigned turns=0;onCapture=[&]{if(turns==1){input(0,0,INPUT_BUTTON_Y);prepareCameraPreviewData();assert(commands.empty());}};
  onDelay=[&]{++turns;if(turns==2){prepareCameraPreviewData();assert(commands.size()==1&&commands[0]=="camerasave");
      assert(snapshot().frozen);input(0,0,INPUT_BUTTON_A);assert(snapshot().frozen);
      assert(completion);completion(true,completionContext);assert(!snapshot().frozen);}
    if(turns==3){currentOLEDMode=OLED_MENU;prepareCameraPreviewData();}};
  task(nullptr);assert(allocations.empty()&&!sessions);onCapture=nullptr;
  enter();refuseScratch=true;task(nullptr);assert(!snapshot().worker&&strstr(snapshot().error,"memory")&&allocations.empty());refuseScratch=false;
  resetOLEDCameraPreview();refuseTask=true;nowMs+=4000;prepareCameraPreviewData();assert(!snapshot().worker);refuseTask=false;
  resetOLEDCameraPreview();guest=true;prepareCameraPreviewData();assert(!snapshot().active);guest=false;
  validateOnly=true;assert(std::string(cmd_oledcameraread(""))=="VALID");validateOnly=false;
  assert(strstr(cmd_oledcameraread(""),"workerActive"));
  replyBufferAvailable=false;
  assert(strstr(cmd_oledcameraread(""),"needs memory"));
  replyBufferAvailable=true;
  puts("Actual OLED camera: freeze/resume, drained photo action, stale view/session rejection, allocation failure and worker cleanup passed");
}
