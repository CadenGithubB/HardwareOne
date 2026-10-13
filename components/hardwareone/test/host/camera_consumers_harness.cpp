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
#include <type_traits>

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
// The runner inserts G2HijackPage (G2_Glasses.h), G2CmdCookie and the declared
// g2EnqueueRedraw (G2_HijackCmd.h, as header_boundary::g2EnqueueRedraw), so the
// stubs below use the production types and fail to build when they drift.
// INSERT_G2_BOUNDARY_TYPES
using Completion = void(*)(bool,const char*,const G2CmdCookie&,void*);
struct PendingCommand {G2CmdCookie cookie;Completion done;void* userData;};
struct PendingRedraw {G2CmdCookie cookie;void(*render)();};
static std::vector<PendingCommand> pendingCommands;
static std::vector<PendingRedraw> pendingRedraws;
static std::vector<std::string> shownRows, submitted;
// acceptList=false models a refused lens render; the old rows stay visible.
static bool acceptCommand=true,acceptRedraw=true,acceptList=true;
static G2HijackPage page=G2_HIJACK_PAGE_MAIN;
static unsigned menuGen=0,listsShown=0;
static uint64_t commandSeq=0;
bool g2CamStreamSettingsExitRelaunch=false;
G2HijackPage g2GetHijackPage(){return page;}
void g2SetHijackPage(G2HijackPage value){if(page!=value){page=value;++menuGen;}}
bool g2ShowListPage(const char*const* rows,size_t count){
  if(!acceptList) return false;
  ++listsShown;shownRows.assign(rows,rows+count);return true;
}
void g2ReshowSensorsDetail(){}
bool g2ShowCameraStream(void(*)()){return true;}
bool g2EnqueueRedraw(const G2CmdCookie& cookie,void(*render)(),const char* tag) {
  assert(render && tag && strcmp(tag,"Camera settings")==0);
  if(!acceptRedraw) return false;
  pendingRedraws.push_back({cookie,render});
  return true;
}
static_assert(std::is_same<decltype(header_boundary::g2EnqueueRedraw),decltype(g2EnqueueRedraw)>::value,
              "g2EnqueueRedraw stub no longer matches the G2_HijackCmd.h declaration");
bool g2SubmitHijackCommand(const char* command,const G2CmdCookie& cookie,
                           Completion done,void* userData) {
  if (!acceptCommand) return false;
  submitted.emplace_back(command);
  G2CmdCookie captured=cookie;captured.seq=++commandSeq;captured.menuGen=menuGen;
  pendingCommands.push_back({captured,done,userData});
  return true;
}
static G2CmdCookie completeCommand(bool ok,const char* result) {
  assert(!pendingCommands.empty());
  const auto command=pendingCommands.front();pendingCommands.erase(pendingCommands.begin());
  assert(command.done);command.done(ok,result,command.cookie,command.userData);
  return command.cookie;
}
// This boundary explicitly delivers a queued renderer; the real generation
// fence and queue/allocation ownership are covered by the g2_redraw suite.
static void flushRedraws() {
  const auto queued=pendingRedraws;pendingRedraws.clear();
  for(const auto& redraw:queued) redraw.render();
}
// INSERT_LIST_PUBLISH_HELPER
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
  const auto pendingBefore=pendingCommands.size();
  g2CameraSettingsHandleTap(1);
  assert(gSettings.cameraQuality==12 && submitted.size()==before);
  assert(pendingCommands.size()==pendingBefore && pendingRedraws.empty());

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

  // Deliver prior test commands as failures without changing committed state.
  while(!pendingCommands.empty()) completeCommand(false,"Error: test rejection");
  flushRedraws();acceptCommand=true;

  showSubMenu(CAM_CAT_POSTPROC);
  g2CameraSettingsHandleTap(1);
  assert(shownRows[1]=="Quality: 12");
  const auto rowsBefore=shownRows;
  const auto drawsBefore=listsShown;
  gSettings.cameraQuality=16; // The executor, never the page, commits settings.
  const auto cookie=completeCommand(true,"SUCCESS");
  assert(shownRows==rowsBefore && listsShown==drawsBefore && pendingRedraws.size()==1);
  const auto redraw=pendingRedraws.front();
  assert(redraw.cookie.seq==cookie.seq && redraw.cookie.menuGen==cookie.menuGen);
  assert(redraw.cookie.targetPage==cookie.targetPage && redraw.cookie.targetNetSub==cookie.targetNetSub);
  assert(cookie.targetPage==G2_HIJACK_PAGE_CAMERA_SETTINGS && cookie.targetNetSub==CAM_LEVEL_SUB_POSTPROC);
  assert(redraw.render==redrawCurrentCameraSettings);
  flushRedraws();
  assert(shownRows[1]=="Quality: 16" && listsShown==drawsBefore+1);

  // Completion redraws the current subpage, not the submitted command's level.
  g2CameraSettingsHandleTap(1);
  showSubMenu(CAM_CAT_TRANSFORM);
  completeCommand(false,"Error: rejected");
  assert(pendingRedraws.size()==1 && gSettings.cameraQuality==16);
  flushRedraws();
  assert(gLevel==CAM_LEVEL_SUB_TRANSFORM && shownRows[1]=="No supported controls");

  // Redraw rejection has no inline fallback or optimistic settings mutation.
  showSubMenu(CAM_CAT_POSTPROC);g2CameraSettingsHandleTap(1);
  acceptRedraw=false;
  const auto rejectedDraws=listsShown;
  completeCommand(true,"SUCCESS");
  assert(pendingRedraws.empty() && listsShown==rejectedDraws && gSettings.cameraQuality==16);
  acceptRedraw=true;

  // Even a manually delivered renderer is harmless after leaving the page.
  g2CameraSettingsHandleTap(1);g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
  completeCommand(true,"SUCCESS");
  const auto awayDraws=listsShown;flushRedraws();
  assert(listsShown==awayDraws && pendingCommands.empty() && pendingRedraws.empty());

  // A refused category list leaves TOP on the lens: its "FPS" row (2) must
  // cycle FPS, not the Camera list's row-2 knob.
  g2ShowCameraSettingsMenu();
  assert(page==G2_HIJACK_PAGE_CAMERA_SETTINGS && shownRows[2].find("FPS:")==0);
  acceptList=false;g2CameraSettingsHandleTap(3);acceptList=true;
  assert(shownRows[2].find("FPS:")==0);
  before=submitted.size();
  g2CameraSettingsHandleTap(2);
  const bool refusedSubListCycledCameraKnob=
    submitted.size()>before && submitted.back().find("camerafps ")!=0;
  assert(!refusedSubListCycledCameraKnob);
  assert(submitted.size()==before+1 && submitted.back()=="camerafps 10");
  completeCommand(true,"SUCCESS");flushRedraws();
  // A refused re-render after a cycle keeps the rendered row identities.
  showSubMenu(CAM_CAT_POSTPROC);
  assert(gLevel==CAM_LEVEL_SUB_POSTPROC && shownRows[1].find("Quality:")==0);
  acceptList=false;showSubMenu(CAM_CAT_TRANSFORM);acceptList=true;
  assert(gLevel==CAM_LEVEL_SUB_POSTPROC);
  g2CameraSettingsHandleTap(1);
  assert(submitted.back().find("cameraquality ")==0);
  completeCommand(true,"SUCCESS");flushRedraws();
  puts("CAMERA_CONSUMERS PASS");
}
