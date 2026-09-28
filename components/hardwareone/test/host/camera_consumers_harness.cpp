// Compile the entire shipping G2 camera settings page; mock only its runtime
// boundaries. The HAL size/control descriptors are linked from production.
#include "HAL_Camera.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <new>

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_CAMERA_SENSOR 1
#define EXT_RAM_BSS_ATTR
#define DEBUG_G2F(...) do {} while (0)
#define BROADCAST_PRINTF(...) do {} while (0)
struct {
  int cameraFramesize=0, cameraBrightness=0, cameraContrast=0, cameraAELevel=0;
  int cameraSharpness=0, cameraQuality=12, cameraDenoise=0, g2StreamToneMap=1;
  int cameraStreamFps=5, g2StreamWidth=192, g2StreamHeight=144;
  bool cameraHMirror=false, cameraVFlip=false;
} gSettings;
static uint64_t supportedControls=0;
static uint32_t supportedResolutions=0;
CameraCapabilities getCameraCapabilities() {return {"host",supportedResolutions,supportedControls,false,false,0,0};}
bool cameraSupportsResolution(CameraFrameSize size) {return (supportedResolutions & cameraResolutionBit(size)) != 0;}
bool cameraSupportsControl(CameraControl control) {return (supportedControls & cameraControlBit(control)) != 0;}
struct G2CmdCookie {unsigned menuGen=0,seq=0,targetPage=0;uint8_t targetNetSub=0;};
struct RedrawSpec {void(*render)()=nullptr;};
enum class LensJobKind {Redraw};
struct LensUiJob {
  LensJobKind kind=LensJobKind::Redraw;
  unsigned submitMenuGen=0,cmdSeq=0,targetPage=0,targetNetSub=0;
  struct {RedrawSpec* redraw=nullptr;} payload;
};
static std::vector<std::string> shownRows, submitted;
static bool acceptCommand=true;
static unsigned page=0;
static constexpr unsigned G2_HIJACK_PAGE_CAMERA_SETTINGS=23;
bool g2CamStreamSettingsExitRelaunch=false;
unsigned g2GetHijackPage(){return page;}
void g2SetHijackPage(unsigned value){page=value;}
bool g2ShowListPage(const char*const* rows,size_t count){shownRows.assign(rows,rows+count);return true;}
void g2ReshowSensorsDetail(){}
bool g2ShowCameraStream(void(*)()){return true;}
bool g2EnqueueLensJob(LensUiJob* job){delete job->payload.redraw;delete job;return true;}
bool g2SubmitHijackCommand(const char* command,const G2CmdCookie&,
                           void(*)(bool,const char*,const G2CmdCookie&,void*),void*) {
  if (!acceptCommand) return false;
  submitted.emplace_back(command);
  return true;
}
// INSERT_PRODUCTION_CAMERA_SETTINGS

int main() {
  // CSI profile: scaled outputs + native HD, no unsupported OV sensor knobs.
  supportedResolutions=cameraResolutionBit(CameraFrameSize::QVGA)|
    cameraResolutionBit(CameraFrameSize::VGA)|cameraResolutionBit(CameraFrameSize::HD);
  gSettings.cameraFramesize=5; // A saved S3 size survives an unsupported P4 fallback.
  showSubMenu(CAM_CAT_CAMERA);
  assert(shownRows[1].find("uxga [N/A]")!=std::string::npos);
  gSettings.cameraFramesize=0;
  g2ShowCameraSettingsMenu();
  g2CameraSettingsHandleTap(3); // Camera category.
  assert(shownRows.size()==2 && shownRows[1].find("Resolution:")==0);
  g2CameraSettingsHandleTap(1); // Resolution picker.
  assert(shownRows.size()==4);
  assert(shownRows[1].find("320x240")!=std::string::npos);
  assert(shownRows[2].find("640x480")!=std::string::npos);
  assert(shownRows[3].find("1280x720")!=std::string::npos);
  g2CameraSettingsHandleTap(3);
  assert(submitted.size()==1 && submitted.back()=="cameraframesize 11");
  assert(gSettings.cameraFramesize==0); // The command executor owns mutation.

  showSubMenu(CAM_CAT_TRANSFORM);
  assert(shownRows.size()==2 && shownRows[1]=="No supported controls");
  g2CameraSettingsHandleTap(1);
  assert(submitted.size()==1);
  showSubMenu(CAM_CAT_POSTPROC);
  assert(shownRows.size()==3 && shownRows[1].find("Quality:")==0 && shownRows[2].find("Tone Map:")==0);
  g2CameraSettingsHandleTap(1);
  assert(submitted.back()=="cameraquality 16" && gSettings.cameraQuality==12);

  // Ordinary DVP knobs remain selectable, without moving persisted IDs.
  supportedControls=cameraControlBit(CameraControl::Brightness)|cameraControlBit(CameraControl::Contrast);
  showSubMenu(CAM_CAT_CAMERA);
  assert(shownRows.size()==4 && shownRows[2]=="Brightness: +0" && shownRows[3]=="Contrast: +0");
  g2CameraSettingsHandleTap(2);
  assert(submitted.back()=="camerabrightness 1");
  assert(gSettings.cameraBrightness==0);

  // Capabilities may change after draw (e.g. driver refined its sensor probe).
  // A stale tap targets its displayed row or is rejected, never another knob.
  showSubMenu(CAM_CAT_CAMERA);
  supportedControls=cameraControlBit(CameraControl::Contrast);
  size_t before=submitted.size();
  g2CameraSettingsHandleTap(2);
  assert(submitted.size()==before);
  showSubMenu(CAM_CAT_CAMERA);
  assert(shownRows.size()==3 && shownRows[2]=="Contrast: +0");
  g2CameraSettingsHandleTap(2);
  assert(submitted.back()=="cameracontrast 1");

  showResolutionPicker();
  supportedResolutions &= ~cameraResolutionBit(CameraFrameSize::VGA);
  before=submitted.size();
  g2CameraSettingsHandleTap(2);
  assert(submitted.size()==before); // Old VGA row cannot silently become HD.
  showResolutionPicker();
  assert(shownRows.size()==3 && shownRows[2].find("1280x720")!=std::string::npos);
  g2CameraSettingsHandleTap(99);
  assert(submitted.size()==before);

  // Queue failure leaves persisted state unchanged and no optimistic text.
  acceptCommand=false;
  showSubMenu(CAM_CAT_POSTPROC);
  g2CameraSettingsHandleTap(1);
  assert(gSettings.cameraQuality==12 && submitted.size()==before);

  // Text output omits unsupported controls and is bounded at every capacity.
  char info[1024];g2BuildCameraSettingsInfo(info,sizeof(info));
  assert(strstr(info,"Contrast:") && !strstr(info,"Brightness:") && !strstr(info,"Denoise:"));
  for(size_t capacity=1;capacity<sizeof(info);++capacity) {
    char* buffer=new char[capacity];
    memset(buffer,0x7e,capacity);
    g2BuildCameraSettingsInfo(buffer,capacity);
    assert(memchr(buffer,'\0',capacity));
    delete[] buffer;
  }
  g2BuildCameraSettingsInfo(nullptr,0);
  puts("CAMERA_CONSUMERS PASS");
}
