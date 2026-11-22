#define EXT_RAM_BSS_ATTR
#define DISPLAY_FG 1
#define SSD1306_WHITE 1
static Adafruit_SSD1306* oledDisplay=nullptr;
static struct {bool up=false,down=false,left=false,right=false;} gNavEvents;
enum { INPUT_BUTTON_A=1,INPUT_BUTTON_B=2,INPUT_BUTTON_X=4 };
#define INPUT_CHECK(value,mask) ((value)&(mask))
static bool connected=true,guest=false;
static bool bleSubsystemActive(){return true;}
static unsigned polls=0;
static std::vector<std::string> commands;
static bool g2RingIsConnected(){return connected;}
static bool g2RingHealthPageRefreshSupported(){return true;}
static bool healthStartPollBurst(){++polls;return connected;}
static bool healthPollBurstActive(){return false;}
static bool healthLoggingIsActive(){return false;}
static struct {bool healthLoggingEnabled=false;} gSettings;
static void g2RingGetTelemetry(G2RingTelemetry& out){out={};out.hr=72;out.hrValid=true;}
static const char* g2RingWearName(bool,uint8_t){return "--";}
static const char* g2RingDesiredStateName(G2RingDesiredState){return "preserve";}
static const char* g2RingObservedStateName(G2RingObservedState){return "unknown";}
static constexpr int G2_RING_ERR_NONE=0;
struct G2RingControlStatus {
  G2RingDesiredState healthDesired=G2_RING_PRESERVE,lowPowerDesired=G2_RING_PRESERVE;
  G2RingObservedState healthObserved=G2_RING_OBS_UNKNOWN,lowPowerObserved=G2_RING_OBS_UNKNOWN;
  bool healthPending=false,lowPowerPending=false;
  int healthLastError=0,lowPowerLastError=0;
};
static void g2RingGetControlStatus(G2RingControlStatus& out){out={};}
struct G2HealthHistorySummary {int fetchState;};
static void g2HealthHistoryGetSummary(G2HealthHistorySummary& out){out={};}
static const char* r1HealthHistoryFetchStateName(int){return "idle";}
static bool g2RingRequestHistoryRefresh(bool){return true;}
static bool oledGuestBlocksMutate(){return guest;}
static bool oledRunCommand(const char* line){commands.emplace_back(line);return true;}
static void executeOledCommand(const char* line){(void)oledRunCommand(line);}  // background; lands at once here
static void oledMarkDirty(){}
static bool oledScrollHandleNav(OledScrollState* s){if(gNavEvents.up){oledScrollUp(s);return true;}if(gNavEvents.down){oledScrollDown(s);return true;}return false;}
// INSERT_PAGE
static void frame(){assert(!locked);oledDisplay->textRows.clear();std::fill(oledDisplay->buffer.begin(),oledDisplay->buffer.end(),0);displayR1Health();for(int y:oledDisplay->textRows)assert(y>=OLED_CONTENT_START_Y && y+8<=OLED_CONTENT_START_Y+OLED_CONTENT_HEIGHT);}
static void preview(const char* dir,const char* name){if(!dir)return;std::ofstream out(std::string(dir)+"/"+name,std::ios::binary);out<<"P5\n128 "<<canvasHeight<<"\n255\n";for(int y=0;y<canvasHeight;++y)for(int x=0;x<128;++x)out.put((oledDisplay->buffer[(y/8)*128+x]>>(y&7))&1?char(255):char(0));}
static void runChecks(const char* dir){
  // The snapshot owns its bytes, orders appended backfill by time, handles
  // rollover, and releases the model lock before any rendering.
  hw1_g2_health::pushSeries(&sHr,80,1,90000);
  hw1_g2_health::pushSeries(&sHr,60,2,10000);
  hw1_g2_health::pushSeries(&sHr,70,3,50000);
  G2HealthGraphSnapshot copy{};
  assert(g2HealthGraphSnapshot(HEALTH_METRIC_HR,false,copy));
  assert(!locked && copy.count==3 && copy.points[0].value==60 && copy.points[2].value==80);
  sHr.buf[0].value=81;assert(copy.points[2].value==80);
  allowLock=false;assert(!g2HealthGraphSnapshot(HEALTH_METRIC_HR,false,copy)&&copy.count==0);allowLock=true;
  assert(!g2HealthGraphSnapshot(HEALTH_METRIC_TEMP,true,copy));
  assert(g2HealthGraphSnapshot(HEALTH_METRIC_TEMP,false,copy)&&copy.count==0);
  hw1_g2_health::clearSeries(&sBat);now=50;hw1_g2_health::pushSeries(&sBat,80,0,UINT32_MAX-49);
  assert(g2HealthGraphSnapshot(HEALTH_METRIC_BATTERY,false,copy)&&copy.points[0].ageMs==100);now=100000;
  for(size_t i=0;i<120;++i)hw1_g2_health::pushSeriesRaw(&sHrv,int16_t(i),uint32_t(i+1),uint32_t(i+1));
  assert(g2HealthGraphSnapshot(HEALTH_METRIC_HRV,false,copy)&&copy.count==96);
  assert(copy.points[0].value==24 && copy.points[95].value==119);
  hw1_g2_health::pushSeriesRaw(&sTrendHr,82,12000,80000);
  hw1_g2_health::pushSeriesRaw(&sTrendHr,72,10000,70000);
  sTrendMetaHr={10000,13000,2,true,77,72,82};
  assert(g2HealthGraphSnapshot(HEALTH_METRIC_HR,true,copy)&&copy.points[0].epoch==10000 && copy.startEpoch==10000);
  sHr.hasBackfill=true;assert(g2HealthGraphSnapshot(HEALTH_METRIC_HR,false,copy)&&copy.backfilled);
  for(int height:{64,88,128}){
    canvasHeight=height;canvasHeader=height==128?12:10;canvasFooter=height==128?16:10;Adafruit_SSD1306 canvas;oledDisplay=&canvas;
    healthOnEnter(true);frame();
    if(height==128)assert(sHealthActions.visibleLines>=HEALTH_ACTION_COUNT && sHealthActions.scrollOffset==0);
    sHealthSel=HEALTH_ACTION_GRAPHS;frame();
    assert(sHealthActions.selectedIndex==HEALTH_ACTION_GRAPHS);
    assert(sHealthActions.scrollOffset+sHealthActions.visibleLines>HEALTH_ACTION_GRAPHS);
    preview(dir,height==128?"health-128.pgm":height==64?"health-64.pgm":"health-88.pgm");
    guest=true;assert(healthInputHandler(0,0,INPUT_BUTTON_A)&&sHealthView==HEALTH_GRAPHS);
    frame();assert(sHealthGraphs.itemCount==8);
    for(int i=0;i<kHealthGraphCount;++i){
      sHealthGraphs.selectedIndex=i;assert(healthInputHandler(0,0,INPUT_BUTTON_A));frame();
      if(i==0)preview(dir,height==128?"graph-128.pgm":height==64?"graph-64.pgm":"graph-88.pgm");
      const unsigned before=polls;assert(healthInputHandler(0,0,INPUT_BUTTON_X)&&polls==before);
      assert(healthInputHandler(0,0,INPUT_BUTTON_B)&&sHealthView==HEALTH_GRAPHS);
    }
    guest=false;sHealthGraphs.selectedIndex=0;healthInputHandler(0,0,INPUT_BUTTON_A);
    const unsigned before=polls;healthInputHandler(0,0,INPUT_BUTTON_X);assert(polls==before+1);
    gNavEvents.right=true;healthInputHandler(0,0,0);gNavEvents={};assert(sHealthGraphs.selectedIndex==1);
    connected=false;frame();connected=true;
    healthInputHandler(0,0,INPUT_BUTTON_B);healthInputHandler(0,0,INPUT_BUTTON_B);assert(sHealthView==HEALTH_OVERVIEW);
    assert(!healthInputHandler(0,0,INPUT_BUTTON_B));
    sHealthSel=HEALTH_ACTION_LOGGING;healthInputHandler(0,0,INPUT_BUTTON_A);assert(commands.back()=="healthlogging toggle");
    char value[16];healthGraphValue(value,sizeof(value),-5,HEALTH_METRIC_TEMP);assert(std::string(value)=="-0.5");
  }
  puts("OLED Health: expanding lists, graph bounds/navigation, ordered owned snapshots, rollover and disabled API passed");
}
