#include <cassert>
#include <cmath>
#include <cstdlib>
#define DEBUG_AUTOMATIONSF(...) ((void)0)
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
// System_Clock.h, as rtcLocalTime reads it: the timezone setting in minutes.
namespace Clock { static int tzOffset=0; static int tzOffsetMinutes() { return tzOffset; } }
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
#if !TEST_BASELINE
  gImuCache.hasHeading=true;  // a reading whose yaw is a heading (the BNO055's), as recorded
#endif
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
    char ble[1024];buildSensorDataJson(ble,sizeof(ble));emit(ble);assert(!held);
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
  // A yaw that is no heading (no calibrated magnetometer: the Adafruit 4517
  // for now) is not shown as one: the lens row leaves it out, the readout
  // marks it relative, and the BLE stream keeps its "heading" key with null.
  setup(0); gImuCache.hasHeading=false;
  { char row[32];imuG2FormatValue(row,sizeof(row));assert(std::string(row)=="P-24 R3");
    char readout[160];g2BuildImuReadout(readout,sizeof(readout));
    assert(std::string(readout)=="Yaw  : 123 rel\nPitch: -24\nRoll : 3\nAcc 1.2 -2.5 9.8\nGyr -0.5 0.2 4.5\nTemp: 28 C");
    char ble[1024];buildSensorDataJson(ble,sizeof(ble));
    assert(std::string(ble).find("\"imu\":{\"heading\":null,\"pitch\":-24.5,\"roll\":3.5,\"valid\":true}")!=std::string::npos);
    assert(std::string(ble).find("123.5")==std::string::npos); }
  // The sensor log's IMU columns carry whether there is a reading.
  setup(0); assert(logReadings(LOG_IMU).imuValid);
  setup(1); assert(!logReadings(LOG_IMU).imuValid);
  setup(2); char fm[160];buildFmTunerReadout(fm,sizeof(fm));assert(std::string(fm)=="Radio busy\n(reading...)");
  // The lens shows the RTC in local time (the chip keeps UTC): Thu 2026-10-08
  // 23:58:42 UTC is Fri 01:58:42 at +120 min, with the date and the weekday
  // carried over midnight, and Thu 18:58:42 at -300 min. Row and readout agree.
  struct Local { int offset; const char* row; const char* readout; };
  const Local locals[]={
    {120,"01:58","Fri 2026-10-09\n   01:58:42\nTemp: -1.5 C"},
    {-300,"18:58","Thu 2026-10-08\n   18:58:42\nTemp: -1.5 C"},
    {0,"23:58","Thu 2026-10-08\n   23:58:42\nTemp: -1.5 C"},
  };
  for(const Local& local:locals) {
    setup(0); Clock::tzOffset=local.offset; char rtc[96];
    rtcG2FormatValue(rtc,sizeof(rtc));assert(std::string(rtc)==local.row);
    g2BuildRtcReadout(rtc,sizeof(rtc));assert(std::string(rtc)==local.readout);
  }
  Clock::tzOffset=120;  // the waiting and busy words are not times: unchanged
  setup(1); { char rtc[96];rtcG2FormatValue(rtc,sizeof(rtc));assert(std::string(rtc)=="...");
    g2BuildRtcReadout(rtc,sizeof(rtc));assert(std::string(rtc)=="RTC\n(reading...)"); }
  setup(2); { char rtc[96];rtcG2FormatValue(rtc,sizeof(rtc));assert(std::string(rtc)=="busy"); }
  Clock::tzOffset=0;
#endif
}
