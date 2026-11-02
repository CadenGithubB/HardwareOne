#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <new>
#include <vector>
#define ENABLE_CAMERA_SENSOR 1
#define CONFIG_IDF_TARGET_ESP32P4 1
#ifndef CAMERA_SCCB_SHARED
#define CAMERA_SCCB_SHARED 1
#endif
using esp_err_t=int;using i2c_master_bus_handle_t=void*;using i2c_port_num_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_TIMEOUT=3,ESP_ERR_NO_MEM=4;
struct esp_sccb_io_t;
using esp_sccb_io_handle_t=esp_sccb_io_t*;
struct sccb_i2c_config_t {int device_address;uint32_t scl_speed_hz;};
// INSERT_INTERFACE
static int realCalls=0,retains=0,releases=0,transactions=0;static bool busReady=true,lockOK=true;
static int transportError=0;static size_t shortRead=0;static uint32_t lastClock=0,lastWait=0;
static bool held=false;
struct TwoWire {
 std::vector<uint8_t> written;size_t left=0;
 void beginTransmission(uint8_t a){assert(held&&a==0x36);written.clear();}
 size_t write(const uint8_t* data,size_t n){assert(held);written.assign(data,data+n);return n;}
 uint8_t endTransmission(bool=true){assert(held);return transportError;}
 size_t requestFrom(uint8_t a,size_t n,bool){assert(held&&a==0x36);left=shortRead?shortRead:n;return left;}
 int read(){assert(held&&left);--left;return 0x56;}
 int available(){return left;}
};static TwoWire wire;
struct I2CTransactionOptions {uint32_t clockHz,lockWaitMs;};
struct I2CDevice {
 enum class Mode{NACK_TOLERANT};
 template<class F>bool transaction(F f,I2CTransactionOptions o,Mode){++transactions;lastClock=o.clockHz;lastWait=o.lockWaitMs;if(!lockOK)return false;assert(!held);held=true;bool ok=f();held=false;return ok;}
};static I2CDevice device;
struct I2CDeviceManager {
 static I2CDeviceManager* existingInstance(){static I2CDeviceManager m;return &m;}
 bool retainBus(uint8_t b){assert(b==0);if(!busReady)return false;++retains;return true;}
 void releaseBus(uint8_t b){assert(b==0);++releases;}
 I2CDevice* registerDevice(unsigned address,const char*,uint8_t b){assert(address==0x36&&b==0);return &device;}
 TwoWire* getWire(uint8_t b){assert(b==0);return &wire;}
};
esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port,void** out){assert(port==1);*out=reinterpret_cast<void*>(1);return ESP_OK;}
extern "C" esp_err_t __real_sccb_new_i2c_io(void*,const sccb_i2c_config_t*,esp_sccb_io_handle_t* out){++realCalls;*out=reinterpret_cast<esp_sccb_io_handle_t>(2);return ESP_OK;}
// INSERT_PRODUCTION
int main(){
 void* native=nullptr;sccb_i2c_config_t config{0x36,100000};esp_sccb_io_handle_t io=nullptr;
 assert(__wrap_sccb_new_i2c_io(reinterpret_cast<void*>(3),&config,&io)==ESP_OK&&realCalls==1);
#if CAMERA_SCCB_SHARED
 busReady=false;assert(!cameraSharedSccbAcquire(0,1,&native));busReady=true;
 assert(cameraSharedSccbAcquire(0,1,&native)&&native&&retains==1);
 assert(!cameraSharedSccbAcquire(0,1,&native));
 assert(__wrap_sccb_new_i2c_io(reinterpret_cast<void*>(1),&config,&io)==ESP_OK&&realCalls==1);
 uint8_t tx[]={0x30,0x0a},rx[2]{};
 assert(io->transmit_receive_reg_a16v8(io,tx,2,rx,1,200)==ESP_OK&&rx[0]==0x56);
 assert(wire.written==std::vector<uint8_t>({0x30,0x0a})&&lastClock==100000&&lastWait==200);
 assert(io->transmit_reg_a16v8(io,tx,2,1000)==ESP_OK&&lastWait==500);
 transportError=2;assert(io->transmit_reg_a16v8(io,tx,2,200)==ESP_FAIL);transportError=0;
 shortRead=1;assert(io->transmit_receive_reg_a16v16(io,tx,2,rx,2,200)==ESP_FAIL&&!wire.left);shortRead=0;
 lockOK=false;assert(io->transmit_reg_a16v8(io,tx,2,200)==ESP_ERR_TIMEOUT);lockOK=true;
 assert(io->transmit_reg_a16v8(io,nullptr,2,200)==ESP_ERR_INVALID_ARG);
 assert(io->del(io)==ESP_OK);cameraSharedSccbRelease();cameraSharedSccbRelease();assert(releases==1);
 assert(__wrap_sccb_new_i2c_io(reinterpret_cast<void*>(1),&config,&io)==ESP_OK&&realCalls==2);
#endif
 std::puts("Production SCCB adapter: managed transfers, failures, lifetime and dedicated-bus pass-through passed");
}
