// Production definitions inserted by test_small_sensor_snapshots.py.
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#define CACHE_MUTEX_TIMEOUT_MS 100
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define OLED_CONTENT_START_Y 10
#define OLED_CONTENT_HEIGHT 48
#define DISPLAY_COLOR_WHITE 1
#define ENABLE_OLED_DISPLAY 0
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
static bool admit = true;
static bool tofTransactionActive = false;
static std::vector<unsigned> waits;
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  assert(!tofTransactionActive); // Cache admission must not retain the I2C bus.
  waits.push_back(ms); if (!m || m->held || !admit) return 0; m->held = true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->held); m->held = false; }
static void renderingAllowed() {
#if !TEST_BASELINE
  assert(!semaphore.held);
  assert(!tofTransactionActive);
#endif
}
static int trackedSnprintf(char* dst, size_t cap, const char* format, ...) {
  renderingAllowed(); va_list args; va_start(args, format);
  int n = std::vsnprintf(dst, cap, format, args); va_end(args); return n;
}
#define snprintf trackedSnprintf
static void diagnostic(const char*, ...) { renderingAllowed(); }
#define DEBUG_TOF_POLLINGF(...) diagnostic(__VA_ARGS__)
#define DEBUG_TOF_VALUESF(...) diagnostic(__VA_ARGS__)
struct FakeDisplay {
  std::string text;
  void clearDisplay() { renderingAllowed(); text.clear(); }
  void setTextSize(int) { renderingAllowed(); }
  void setTextColor(int) { renderingAllowed(); }
  void setCursor(int, int) { renderingAllowed(); }
  void print(const char* value) { renderingAllowed(); text += value; }
  void print(int value) { renderingAllowed(); text += std::to_string(value); }
  void print(float value, int precision) { char b[64]; snprintf(b, sizeof(b), "%.*f", precision, value); print(b); }
  void println(const char* value = "") { print(value); text += '\n'; }
  void println(int value) { print(value); text += '\n'; }
  void println(float value, int precision) { print(value, precision); text += '\n'; }
  void drawRect(int,int,int,int,int) { renderingAllowed(); }
  void fillRect(int,int,int,int,int) { renderingAllowed(); }
} display;
static FakeDisplay* oledDisplay = &display;
static bool oledConnected = true;
static int constrain(int n, int lo, int hi) { return std::clamp(n, lo, hi); }
static int map(int n, int a, int b, int x, int y) { return (n-a)*(y-x)/(b-a)+x; }
static void imuUpdateActions() {}
static void broadcastOutput(const char*) { renderingAllowed(); }
static bool gApdsConnected = true, gApdsColorRunning = true, gApdsProximityRunning = true;
static bool gImuConnected = true, gImuRunning = true, gTofConnected = true, gTofRunning = true;
static bool gRtcConnected = true, gRtcRunning = true;
namespace Clock { static int offset = -300; static int tzOffsetMinutes() { return offset; } }
// INSERT_TYPES
// INSERT_STATE
static unsigned long nowMs = 123456;
static unsigned long millis() { return nowMs; }
static RTCDateTime deviceDate{2026, 1, 1, 2, 3, 4, 5};
static bool rtcReadSucceeds = true;
static bool rtcReadDateTime(RTCDateTime* out) { assert(!semaphore.held); *out = deviceDate; return rtcReadSucceeds; }
static float rtcReadTemperature() { assert(!semaphore.held); return -12.5f; }
using VL53L4CX_Error = int;
static constexpr int VL53L4CX_ERROR_NONE=0, VL53L4CX_RANGESTATUS_SIGNAL_FAIL=1,
  VL53L4CX_RANGESTATUS_SIGMA_FAIL=2, VL53L4CX_RANGESTATUS_WRAP_TARGET_FAIL=3,
  VL53L4CX_RANGESTATUS_XTALK_SIGNAL_FAIL=4;
struct VL53L4CX_MultiRangingData_t {
  int NumberOfObjectsFound=0;
  struct { int RangeMilliMeter=0, RangeStatus=0; uint32_t SignalRateRtnMegaCps=0; } RangeData[4];
};
static VL53L4CX_MultiRangingData_t tofDevice;
static int tofError = 0, tofClears = 0;
static struct { int tofBus=1, i2cClockToFHz=200000; } gSettings;
static constexpr uint8_t I2C_ADDR_TOF = 0x29;
template <class F>
static bool i2cTaskWithTimeout(uint8_t bus, uint8_t address, uint32_t hz, uint32_t wait, F&& operation) {
  assert(!tofTransactionActive && !semaphore.held);
  assert(bus==1 && address==I2C_ADDR_TOF && hz==200000 && wait==500);
  tofTransactionActive=true;
  bool result=std::forward<F>(operation)();
  tofTransactionActive=false;
  return result;
}
static void tofHardwareAllowed() {
  assert(!semaphore.held);
#if !TEST_BASELINE
  assert(tofTransactionActive);
#endif
}
struct ToFDriver {
  int VL53L4CX_GetMeasurementDataReady(uint8_t* ready) { tofHardwareAllowed(); *ready=1; return tofError; }
  int VL53L4CX_GetMultiRangingData(VL53L4CX_MultiRangingData_t* out) { tofHardwareAllowed(); *out=tofDevice; return tofError; }
  int VL53L4CX_ClearInterruptAndStartMeasurement() { tofHardwareAllowed(); ++tofClears; return tofError; }
} tofDriver;
static ToFDriver* gVL53L4CX = &tofDriver;
static bool gTofNear = false;
static constexpr int SYSEVT_TOF_OBJECT_DETECTED = 1;
static std::vector<std::string> events;
static void systemEventPost(int, const char* state, const char* data) { assert(!semaphore.held); events.push_back(std::string(state)+":"+data); }
// INSERT_FUNCTIONS

static void reset() {
  APDS={}; IMU={}; TOF={}; RTC={}; semaphore={};
  APDS_MUTEX=IMU_MUTEX=TOF_MUTEX=RTC_MUTEX=&semaphore;
  admit=true; waits.clear(); display.text.clear(); tofError=0; tofClears=0; tofDevice={}; events.clear(); gTofNear=false;
  APDS.apdsRed=65535; APDS.apdsGreen=1234; APDS.apdsBlue=17; APDS.apdsClear=42;
  APDS.apdsProximity=201; APDS.apdsGesture=4; APDS.apdsDataValid=true; APDS.apdsLastUpdate=1234;
#if !TEST_BASELINE
  APDS.apdsColorValid=APDS.apdsProximityValid=APDS.apdsGestureValid=true;
#endif
  IMU.accelX=1.25f; IMU.accelY=-2.5f; IMU.accelZ=9.75f;
  IMU.gyroX=0.125f; IMU.gyroY=-0.25f; IMU.gyroZ=1.5f;
  IMU.oriYaw=359.5f; IMU.oriPitch=-12.5f; IMU.oriRoll=90.0f; IMU.imuTemp=-12;
  IMU.imuDataValid=true; IMU.imuLastUpdate=2345; IMU.imuSeq=8;
  TOF.tofDataValid=true; TOF.tofTotalObjects=2; TOF.tofLastUpdate=3456; TOF.tofSeq=9;
  for (int i : {0,2}) { auto& o=TOF.tofObjects[i]; o.detected=true; o.valid=(i==0); o.distance_mm=255+i*1000; o.distance_cm=o.distance_mm/10.f; o.status=i; }
  RTC.dateTime=deviceDate; RTC.temperature=-12.5f; RTC.dataValid=true; RTC.lastUpdate=4567;
  gApdsConnected=gImuConnected=gTofConnected=gRtcConnected=true;
  gApdsColorRunning=gApdsProximityRunning=gImuRunning=gTofRunning=gRtcRunning=true;
  rtcReadSucceeds=true;
}
static void waited(unsigned n) { assert(waits.size()==1 && waits[0]==n && !semaphore.held); waits.clear(); }
static void emit(const std::string& text) { std::cout << text << '\n'; }
using Builder = int(*)(char*, size_t);
static std::string json(Builder builder, unsigned ms) {
  char b[1024]; int n=builder(b,sizeof(b)); assert(n>0 && n==static_cast<int>(std::strlen(b))); waited(ms); return b;
}
static void bufferLimits(Builder builder, const std::string& expected, bool tof) {
  for (size_t cap=0; cap<=expected.size()+2; ++cap) {
    std::vector<char> b(cap+2,'Z'); const int n=builder(b.data()+1,cap);
    assert(b.front()=='Z' && b.back()=='Z');
    if (cap>expected.size()) assert(n==static_cast<int>(expected.size()) && std::string(b.data()+1)==expected);
#if !TEST_BASELINE
    else { assert(n==0); if (tof && cap) assert(b[1]=='\0'); }
#else
    else if (!tof) assert(n==0);
#endif
  }
  assert(builder(nullptr, 5)==0); waits.clear();
}
static void presentations() {
  Builder builders[]={apdsBuildDataJSON,imuBuildDataJSON,tofBuildDataJSON,rtcBuildDataJSON};
  const unsigned ms[]={50,100,100,50};
  for (int state=0;state<4;++state) {
    reset();
    if(state==1) APDS.apdsDataValid=IMU.imuDataValid=TOF.tofDataValid=RTC.dataValid=false;
    if(state==2) admit=false;
    if(state==3) gApdsConnected=gImuConnected=gTofConnected=gRtcConnected=false;
    for(int i=0;i<4;++i) { const auto text=json(builders[i],ms[i]); emit(text); bufferLimits(builders[i],text,i==2); }
    for(auto draw:{displayAPDSData,displayIMUActions,displayToFData,displayRTCData}) {
      display.text.clear(); draw(); emit(display.text);
      if(state!=3) waited(10); else assert(waits.empty());
    }
    if(state!=2) {
      emit(std::to_string(apdsGetColorR())+":"+std::to_string(apdsGetColorG())+":"+
           std::to_string(apdsGetColorB())+":"+std::to_string(apdsGetColorC())+":"+std::to_string(apdsGetProximity()));
      emit(std::to_string(rtcGetYear())+":"+std::to_string(rtcGetMonth())+":"+std::to_string(rtcGetDay())+":"+
           std::to_string(rtcGetHour())+":"+std::to_string(rtcGetMinute())+":"+std::to_string(rtcGetSecond())+":"+
           std::to_string(rtcGetTemperature())); waits.clear();
    }
  }
}
#if !TEST_BASELINE
static auto values(const APDSCache& a) { return std::make_tuple(a.apdsRed,a.apdsGreen,a.apdsBlue,a.apdsClear,a.apdsProximity,a.apdsGesture,a.apdsDataValid,a.apdsLastUpdate,a.apdsColorValid,a.apdsProximityValid,a.apdsGestureValid); }
static auto values(const ImuCache& a) { return std::make_tuple(a.accelX,a.accelY,a.accelZ,a.gyroX,a.gyroY,a.gyroZ,a.oriYaw,a.oriPitch,a.oriRoll,a.imuTemp,a.imuDataValid,a.imuLastUpdate,a.imuSeq); }
static auto values(const RTCCache& a) { const auto& d=a.dateTime; return std::make_tuple(d.year,d.month,d.day,d.hour,d.minute,d.second,d.dayOfWeek,a.temperature,a.dataValid,a.lastUpdate); }
static auto values(const TofCache& a) {
  std::vector<double> out{double(a.tofDataValid),double(a.tofLastUpdate),double(a.tofSeq),double(a.tofTotalObjects)};
  for(const auto& o:a.tofObjects) { for(double v:{double(o.detected),double(o.valid),double(o.distance_mm),double(o.distance_cm),double(o.status),double(o.smoothed_distance_mm),double(o.smoothed_distance_cm),double(o.hasHistory)}) out.push_back(v); }
  return out;
}
template<class T> static void snapshotContract(T& data, SemaphoreHandle_t& mutex, bool(*copy)(T&,uint32_t)) {
  const T expected=data; T out{};
  assert(copy(out,10)); waited(10);
  assert(values(out)==values(expected));
  out={}; assert(values(data)==values(expected));
  for(unsigned ms:{5,10,50,100}) { out=expected; admit=false; assert(!copy(out,ms)); waited(ms); T zero{}; assert(values(out)==values(zero)); }
  admit=true; mutex=nullptr; out=expected; assert(!copy(out,10)); assert(waits.empty());
  T zero{}; assert(values(out)==values(zero)); mutex=&semaphore;
  semaphore.held=true; assert(!copy(out,10) && semaphore.held && waits.empty()); semaphore.held=false;
}
static void ownership() {
  reset(); APDSCache a{}; ImuCache i{}; TofCache t{}; RTCCache r{};
  assert(apdsCacheSnapshot(a)); waited(10); assert(imuCacheSnapshot(i)); waited(10);
  assert(tofCacheSnapshot(t)); waited(10); assert(rtcCacheSnapshot(r)); waited(10);
  snapshotContract(APDS,APDS_MUTEX,apdsCacheSnapshot); snapshotContract(IMU,IMU_MUTEX,imuCacheSnapshot);
  snapshotContract(TOF,TOF_MUTEX,tofCacheSnapshot); snapshotContract(RTC,RTC_MUTEX,rtcCacheSnapshot);
  assert(apdsCacheStorageBytes()==sizeof(APDSCache)+sizeof(SemaphoreHandle_t));
  assert(imuCacheStorageBytes()==sizeof(ImuCache)+sizeof(SemaphoreHandle_t));
  assert(tofCacheStorageBytes()==sizeof(TofCache)+sizeof(SemaphoreHandle_t));
  reset(); admit=false;
  assert(!resetApdsReading()); waited(100); assert(APDS.apdsDataValid && APDS.apdsRed==65535);
  assert(!resetImuReading()); waited(100); assert(IMU.imuDataValid && IMU.imuSeq==8);
  assert(!resetTofReading()); waited(100); assert(TOF.tofDataValid && TOF.tofTotalObjects==2);
  assert(!resetRtcReading()); waited(100); assert(RTC.dataValid && RTC.dateTime.year==2026);
  admit=true;
  assert(resetApdsReading()); waited(100); assert(!APDS.apdsDataValid && APDS.apdsRed==0 && APDS.apdsLastUpdate==1234);
  assert(!APDS.apdsColorValid && !APDS.apdsProximityValid && !APDS.apdsGestureValid);
  assert(resetImuReading()); waited(100); assert(!IMU.imuDataValid && IMU.accelX==0 && IMU.imuSeq==8 && IMU.imuLastUpdate==2345);
  assert(resetTofReading()); waited(100); assert(!TOF.tofDataValid && TOF.tofTotalObjects==0 && !TOF.tofObjects[0].hasHistory && TOF.tofSeq==9 && TOF.tofLastUpdate==3456);
  assert(resetRtcReading()); waited(100); assert(!RTC.dataValid && RTC.temperature==0 && RTC.dateTime.year==0 && RTC.lastUpdate==4567);
  reset(); admit=false;
  assert(!apdsInvalidateCache()); waited(50); assert(APDS.apdsDataValid);
  assert(!imuInvalidateCache()); waited(50); assert(IMU.imuDataValid && IMU.imuSeq==8);
  assert(!tofInvalidateCache()); waited(50); assert(TOF.tofDataValid && TOF.tofTotalObjects==2);
  assert(!rtcInvalidateCache()); waited(50); assert(RTC.dataValid);
  assert(apdsGetColorR()==0 && rtcGetYear()==0 && rtcGetTemperature()==0); waits.clear();
  admit=true; assert(apdsInvalidateCache()); waited(50); assert(!APDS.apdsDataValid && APDS.apdsRed==65535);
  assert(!APDS.apdsColorValid && !APDS.apdsProximityValid && !APDS.apdsGestureValid);
  assert(imuInvalidateCache()); waited(50); assert(!IMU.imuDataValid && IMU.imuSeq==0 && IMU.accelX==1.25f);
  assert(tofInvalidateCache()); waited(50); assert(!TOF.tofDataValid && TOF.tofTotalObjects==0);
  for(auto& o:TOF.tofObjects)assert(!o.detected && !o.valid);
  assert(rtcInvalidateCache()); waited(50); assert(!RTC.dataValid && RTC.temperature==-12.5f);
}
#endif
static void polling() {
  reset(); TOF={}; TOF_MUTEX=&semaphore;
  tofDevice.NumberOfObjectsFound=4;
  tofDevice.RangeData[0]={250,0,65536}; tofDevice.RangeData[1]={1200,1,65536};
  tofDevice.RangeData[2]={3500,0,32768}; tofDevice.RangeData[3]={6001,0,65536};
  assert(tofPoll()); waited(50); assert(TOF.tofTotalObjects==2 && TOF.tofSeq==1);
  assert(TOF.tofObjects[0].distance_mm==250 && TOF.tofObjects[1].distance_mm==3500);
  assert(events.size()==1 && events[0]=="near:250mm"); emit(json(tofBuildDataJSON,100));
  tofDevice.RangeData[0].RangeMilliMeter=750;
  assert(tofPoll()); waited(50); assert(TOF.tofObjects[0].distance_mm==450);
  assert(events.size()==2 && events[1]=="far:450mm"); emit(json(tofBuildDataJSON,100));
  admit=false;
#if TEST_BASELINE
  assert(!tofPoll()); waited(50); assert(TOF.tofSeq==2 && tofClears==2);
#else
  // Hardware succeeds/rearms before cache admission; a busy cache only skips publication.
  assert(tofPoll()); waited(50); assert(TOF.tofSeq==2 && tofClears==3);
#endif
  int previousClears=tofClears;
  admit=true; tofError=1; assert(!tofPoll()); assert(waits.empty() && tofClears==previousClears);
  reset(); rtcReadAndCacheOnce(7890); waited(50); assert(RTC.lastUpdate==7890 && RTC.dataValid);
  admit=false; rtcReadAndCacheOnce(8901); waited(50); assert(RTC.lastUpdate==7890);
  admit=true; rtcReadSucceeds=false; rtcReadAndCacheOnce(9012); assert(waits.empty() && RTC.lastUpdate==7890);
}
int main() { presentations(); polling();
#if !TEST_BASELINE
  ownership();
#endif
}
