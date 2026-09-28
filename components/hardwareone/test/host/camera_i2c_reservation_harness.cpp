// Runtime boundaries for the production I2C constructor, initBus and enable
// handler. BuildConfig is included unchanged so reservation policy is real.
#include "System_BuildConfig.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static_assert(ENABLE_CAMERA_SENSOR==EXPECTED_CAMERA_ENABLED,"camera test profile selected");
static_assert(CAMERA_SCCB_I2C_PORT==EXPECTED_RESERVED_PORT,"camera reservation follows the selected backend SDK controller");
#define WARN_I2CF(...) do {} while(0)
#define INFO_I2CF(...) do {} while(0)
#define OUTPUT 1
#define HIGH 1
static unsigned pinModes=0,pinWrites=0,delays=0,events=0,cpu1Begins=0;
void pinMode(int,int){++pinModes;}
void digitalWrite(int,int){++pinWrites;}
void delay(unsigned){++delays;}
void logSystemEvent(const char*,const char*,...){++events;}
struct TwoWire {
  unsigned beginCalls=0,clockCalls=0,timeoutCalls=0;
  int sda=-1,scl=-1;
  uint32_t clock=0,timeout=0;
  void begin(int sdaPin,int sclPin){++beginCalls;sda=sdaPin;scl=sclPin;}
  void setClock(uint32_t value){++clockCalls;clock=value;}
  void setTimeOut(uint32_t value){++timeoutCalls;timeout=value;}
} Wire,Wire1;
void beginBusOnCpu1(TwoWire* wire,int sda,int scl){++cpu1Begins;wire->begin(sda,scl);}
using SemaphoreHandle_t=void*;
struct I2CDevice {};
struct I2CBusMetrics {uint32_t unused;};
struct I2CDeviceStartRequest {unsigned unused;};

// INSERT_PRODUCTION_MANAGER_DECLARATION
// INSERT_PRODUCTION_MANAGER_CONSTANTS
// INSERT_PRODUCTION_MANAGER_CONSTRUCTOR
// INSERT_PRODUCTION_INIT_BUS

class String {
  std::string value;
public:
  String(const char* text):value(text){}
  void trim(){
    auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos){value.clear();return;}
    value=value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
  }
  size_t length()const{return value.size();}
  long toInt()const{return strtol(value.c_str(),nullptr,10);}
};
struct {bool i2cEnabled=false,i2c2Enabled=false;} gSettings;
static unsigned settingCalls=0;
static bool validateOnly=false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do {if(validateOnly)return "valid";} while(0)
void setSetting(bool& slot,bool value){++settingCalls;slot=value;}
char* getDebugBuffer(){static char output[1024];return output;}
// INSERT_PRODUCTION_ENABLE_COMMAND

int main() {
  I2CDeviceManager manager;
  assert(manager.wires[0]==&Wire1 && manager.wires[1]==&Wire);
  assert(!manager.busInitialized[0] && !manager.busInitialized[1]);
  for(unsigned logical=0;logical<2;++logical) {
    const int hardware=logical==0?1:0;
    TwoWire& wire=logical==0?Wire1:Wire;
    const unsigned powerBefore=pinModes+pinWrites+delays,eventsBefore=events,cpuBefore=cpu1Begins;
    const uint32_t clockBefore=manager.currentClockHz[logical],defaultBefore=manager.defaultClockHz[logical];
    manager.initBus(logical,14,13,400000);
    if(hardware==EXPECTED_RESERVED_PORT) {
      assert(wire.beginCalls==0 && wire.clockCalls==0 && wire.timeoutCalls==0);
      assert(!manager.busInitialized[logical]);
      assert(manager.currentClockHz[logical]==clockBefore && manager.defaultClockHz[logical]==defaultBefore);
      assert(pinModes+pinWrites+delays==powerBefore && events==eventsBefore && cpu1Begins==cpuBefore);
      manager.initBus(logical,2,3,100000);
      assert(wire.beginCalls==0 && !manager.busInitialized[logical] && events==eventsBefore);
    } else {
      assert(wire.beginCalls==1 && wire.clockCalls==1 && wire.timeoutCalls==1);
      assert(wire.sda==14 && wire.scl==13 && wire.clock==400000 && wire.timeout==100);
      assert(manager.busInitialized[logical] && manager.currentClockHz[logical]==400000 && manager.defaultClockHz[logical]==400000);
      assert(events==eventsBefore+1 && cpu1Begins==cpuBefore+(logical==0?1:0));
    }
  }
  // Boundaries do not alias an invalid logical bus onto an available bus.
  unsigned totalBegins=Wire.beginCalls+Wire1.beginCalls;
  manager.initBus(2,1,2,100000);manager.initBus(255,1,2,100000);
  for(unsigned logical=0;logical<2;++logical) {
    manager.initBus(logical,-1,2,100000);manager.initBus(logical,1,-1,100000);
    manager.wires[logical]=nullptr;manager.initBus(logical,1,2,100000);
  }
  assert(Wire.beginCalls+Wire1.beginCalls==totalBegins);

  // Each CLI controls its logical bus. The reserved hardware controller is
  // denied before persistence; the other controller remains independently usable.
  const char*(*commands[])(const String&)={cmd_i2cbusenabled,cmd_i2c2busenabled};
  bool* slots[]={&gSettings.i2cEnabled,&gSettings.i2c2Enabled};
  for(unsigned logical=0;logical<2;++logical) {
    const int hardware=logical==0?1:0;
    for(bool initial : {false,true}) {
      *slots[logical]=initial;*slots[1-logical]=!initial;settingCalls=0;
      const char* reply=commands[logical](" 1 ");
      if(hardware==EXPECTED_RESERVED_PORT) {
        assert(strstr(reply,"reserved"));
        assert(*slots[logical]==initial && settingCalls==0);
        reply=commands[logical]("-1");
        assert(strstr(reply,"reserved") && settingCalls==0 && *slots[logical]==initial);
      } else {
        assert(strstr(reply,"set to 1") && *slots[logical] && settingCalls==1);
      }
      assert(*slots[1-logical]==!initial);
      settingCalls=0;reply=commands[logical]("0");
      assert(strstr(reply,"set to 0") && !*slots[logical] && settingCalls==1);
      assert(*slots[1-logical]==!initial);
    }
    settingCalls=0;*slots[logical]=true;
    assert(strstr(commands[logical](" \t"),"invalid arguments"));
    assert(*slots[logical] && settingCalls==0);
    validateOnly=true;
    assert(!strcmp(commands[logical]("0"),"valid"));
    assert(*slots[logical] && settingCalls==0);
    validateOnly=false;
  }
  printf("CAMERA_I2C_RESERVATION camera=%d port=%d PASS\n",ENABLE_CAMERA_SENSOR,CAMERA_SCCB_I2C_PORT);
}
