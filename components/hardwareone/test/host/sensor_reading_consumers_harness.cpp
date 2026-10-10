#include <cassert>
#include <cmath>
#include <cstdlib>
#define DEBUGF(...) ((void)0)
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#define ENABLE_THERMAL_SENSOR 1
#define ENABLE_TOF_SENSOR 1
#define ENABLE_IMU_SENSOR 1
#define ENABLE_GAMEPAD_SENSOR 1
#define ENABLE_OLED_INPUT 1
#define ENABLE_APDS_SENSOR 1
#define LOG_THERMAL 1
#define LOG_TOF 2
#define LOG_IMU 4
#define LOG_GAMEPAD 8
#define LOG_APDS 16
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
using SemaphoreHandle_t = int*;
using TickType_t = uint32_t;
#if TEST_BASELINE
static int semaphore;
#endif
static bool held=false, admit=true;
struct SensorCacheGuard {
  bool held;
  SensorCacheGuard(SemaphoreHandle_t, uint32_t, const char*) : held(admit) { ::held=held; }
  ~SensorCacheGuard() { ::held=false; }
};
static bool lockThermalCache(uint32_t) { held=admit; return admit; }
static void unlockThermalCache() { held=false; }
static int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { held=admit; return admit; }
static void xSemaphoreGive(SemaphoreHandle_t) { held=false; }
static unsigned long millis() { return 12345; }
// CONSTANTS
// TYPES
// STATE
// MOCKS
static bool gThermalRunning=true, gThermalConnected=true;
static bool gImuRunning=true, gImuConnected=true;
static bool gTofRunning=true, gTofConnected=true;
static bool gApdsColorRunning=true, gApdsProximityRunning=true, gApdsGestureRunning=true, gApdsConnected=true;
static bool gInputRunning=true, gInputConnected=true;
static bool fmOn=true;
static bool fmTunerRadioOn() { return fmOn; }
static int trackedSnprintf(char* dst, size_t cap, const char* fmt, ...) {
#if !TEST_BASELINE
  assert(!held);
#endif
  va_list args; va_start(args,fmt);
  int n=vsnprintf(dst,cap,fmt,args); va_end(args); return n;
}
#define snprintf trackedSnprintf
// FUNCTIONS
static void setup(int state) {
  admit=state!=2; held=false;
  gImuCache={}; gTofCache={}; gApdsCache={}; gRtcCache={};
  gThermalCache={}; gInputCache={}; gAnoEncoderCache={}; gFmRadioCache={};
  // RESET_MUTEX
  gImuCache.imuDataValid=gTofCache.tofDataValid=gApdsCache.apdsDataValid=state!=1;
  gRtcCache.dataValid=gThermalCache.thermalDataValid=gInputCache.dataValid=state!=1;
  gAnoEncoderCache.dataValid=gFmRadioCache.dataValid=state!=1;
  gImuCache.oriYaw=123.5f; gImuCache.oriPitch=-24.5f; gImuCache.oriRoll=3.5f;
  gImuCache.accelX=1.25f; gImuCache.accelY=-2.5f; gImuCache.accelZ=9.75f;
  gImuCache.gyroX=-0.5f; gImuCache.gyroY=0.25f; gImuCache.gyroZ=4.5f; gImuCache.imuTemp=28.5f;
  gTofCache.tofTotalObjects=state==3 ? 0 : 4;
  for(int i=0;i<4;i++) { auto& o=gTofCache.tofObjects[i]; o.valid=i%2==0; o.detected=true; o.distance_mm=135+i*242; o.distance_cm=o.distance_mm/10.0f; o.status=i; }
  gApdsCache.apdsRed=110;gApdsCache.apdsGreen=220;gApdsCache.apdsBlue=330;gApdsCache.apdsClear=440;
  gApdsCache.apdsProximity=77;gApdsCache.apdsGesture=3;
  gRtcCache.dateTime={2026,10,8,23,58,42,5};gRtcCache.temperature=-1.5f;
  gThermalCache.thermalMinTemp=-12.5f;gThermalCache.thermalAvgTemp=23.5f;gThermalCache.thermalMaxTemp=120.5f;
  gInputCache.joyX=state==3 ? 1023 : 512;gInputCache.joyY=0;
  gInputCache.buttons=~uint32_t(GAMEPAD_BUTTON_A|GAMEPAD_BUTTON_X|GAMEPAD_BUTTON_START);
  gInputCache.buttonPressedAccum=GAMEPAD_BUTTON_B;
  gAnoEncoderCache.encoderPosition=state==3 ? -15 : 42;gAnoEncoderCache.encoderDelta=-3;
  gAnoEncoderCache.currentAxis=state==3 ? ANO_AXIS_HORIZONTAL : ANO_AXIS_VERTICAL;
  gAnoEncoderCache.buttons=ANO_BTN_UP|ANO_BTN_IN|ANO_VIRT_START;
  gFmRadioCache.frequency=state==3 ? 8750 : 10390;gFmRadioCache.volume=12;gFmRadioCache.muted=state==3;
  gFmRadioCache.seekInProgress=state==3;gFmRadioCache.seekDirUp=false;
  gFmRadioCache.rssi=78;gFmRadioCache.snr=21;strcpy(gFmRadioCache.stationName,"TEST FM");
}
static void emit(const char* value) { std::cout<<value<<'\n'; }
int main() {
  using Formatter=void(*)(char*,size_t);
  Formatter formatters[]={// FORMATTERS
  };
  for(int state=0;state<4;state++) {
    setup(state);
    for(auto fn:formatters) {
      char out[1024];fn(out,sizeof(out));emit(out);assert(!held);
      // Guard both ends across empty/tiny buffers; presentation must respect cap.
      for(size_t cap=0;cap<6;cap++) {
        char tiny[8];memset(tiny,'Z',sizeof(tiny));fn(tiny+1,cap);
        assert(tiny[0]=='Z' && tiny[cap+1]=='Z');
      }
    }
    char ble[1024];buildSensorDataJSON(ble,sizeof(ble));emit(ble);assert(!held);
    auto log=logReadings(31);
    std::cout<<log.thermalValid<<','<<log.thermalMin<<','<<log.thermalAvg<<','<<log.thermalMax<<','
      <<log.tofValid<<','<<log.tofTotal<<','<<log.tof[3].distance_mm<<','<<log.tof[3].status<<','
      <<log.yaw<<','<<log.pitch<<','<<log.roll<<','<<log.ax<<','<<log.gy<<','<<log.imuTemp<<','
      <<log.inputValid<<','<<log.buttons<<','<<log.joyX<<','<<log.joyY<<','<<log.apdsValid<<','
      <<log.apdsClear<<','<<unsigned(log.apdsProximity)<<','<<unsigned(log.apdsGesture)<<'\n';
    assert(!held && gInputCache.buttonPressedAccum==GAMEPAD_BUTTON_B);
    for (const char* sensor : {"TEMP", "DISTANCE", "LIGHT", "MOTION"}) {
      for (const char* op : {">", "<", "=", "!=", ">=", "<=", "CONTAINS"}) {
        for (const char* target : {"0", "23.5", "37.7", "440", "DETECTED", "NONE"}) {
          char value[64];strcpy(value,target);
          std::cout << sensorCondition(sensor,op,value);
        }
      }
      std::cout << '\n';
    }
    if(state!=2) { char fm[160];buildFmTunerReadout(fm,sizeof(fm));emit(fm); }
  }
#if !TEST_BASELINE
  setup(2); char fm[160];buildFmTunerReadout(fm,sizeof(fm));assert(std::string(fm)=="Radio busy\n(reading...)");
#endif
}
