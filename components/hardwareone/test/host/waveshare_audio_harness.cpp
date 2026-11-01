#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#define MIC_BACKEND_ES7210 1
#ifndef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 1
#endif
#define ENABLE_SPEAKER 1
#define IRAM_ATTR
#define DRAM_ATTR
#define WARN_SYSTEMF(...) ((void)0)
#define INFO_SYSTEMF(...) ((void)0)
#define WS_MIC_I2C_ADDRESS 0x40
#define WS_MIC_I2C_BUS 0
#define WS_MIC_DATA_PIN 11
#define WS_SPEAKER_MCLK_PIN 13
#define WS_SPEAKER_BCLK_PIN 12
#define WS_SPEAKER_WS_PIN 10
#define WS_SPEAKER_DATA_PIN 9
#define WS_SPEAKER_AMP_PIN 53
#define WS_SPEAKER_I2C_BUS 0
#define WS_SPEAKER_I2C_PORT 1
using esp_err_t=int;using gpio_num_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=2,ESP_ERR_INVALID_STATE=3,ESP_ERR_INVALID_ARG=4,ESP_ERR_NOT_FOUND=5,ESP_ERR_NO_MEM=6;
constexpr int GPIO_NUM_NC=-1,I2S_NUM_0=0,I2S_ROLE_MASTER=1,I2S_DATA_BIT_WIDTH_16BIT=16,I2S_SLOT_MODE_MONO=1,I2S_SLOT_MODE_STEREO=2,I2S_STD_SLOT_LEFT=1,OUTPUT=1,LOW=0,HIGH=1;
const char* esp_err_to_name(int){return "fake error";}
static int failStage=0,lockDepth=0,transactions=0,reads=0,creates=0,deletes=0,amp=0;
static bool present=true;
struct Write {int address,value;};static std::vector<Write> writes;
struct TwoWire {
 int reg=-1,value=-1;
 void beginTransmission(int address){assert(lockDepth&&address==0x40);reg=value=-1;}
 size_t write(uint8_t v){if(reg<0)reg=v;else value=v;return 1;}
 int endTransmission(){if(failStage==5)return 1;writes.push_back({reg,value});return 0;}
};static TwoWire wire;
struct I2CDeviceManager {static I2CDeviceManager* getInstance(){static I2CDeviceManager m;return &m;}TwoWire* getWire(int bus){assert(bus==0);return &wire;}};
template<class F>bool i2cDeviceTransaction(int bus,int address,int hz,int wait,F f){assert(bus==0&&(address==0x40||address==0x18)&&hz==100000&&wait==500);++lockDepth;++transactions;bool result=f();--lockDepth;return result;}
bool i2cPingAddress(int,int,int,int){assert(!lockDepth);return present;}
void pinMode(int,int){}void digitalWrite(int,int value){amp=value;}void delay(int){assert(!lockDepth);}
struct Channel {bool allocated=false,enabled=false,tx=false;};static Channel rxChannel,txChannel;
using i2s_chan_handle_t=Channel*;
enum i2s_mclk_multiple_t {I2S_MCLK_MULTIPLE_256=256,I2S_MCLK_MULTIPLE_512=512};
struct Clock{uint32_t rate;i2s_mclk_multiple_t mclk_multiple;};
struct Slot{int width,mode,slot_mask;};struct Pins{int mclk,bclk,ws,dout,din;};
struct i2s_std_config_t {Clock clk_cfg;Slot slot_cfg;Pins gpio_cfg;};
struct i2s_chan_config_t{int id,role,dma_desc_num,dma_frame_num;};
#define I2S_CHANNEL_DEFAULT_CONFIG(i,r) i2s_chan_config_t{i,r,0,0}
#define I2S_STD_CLK_DEFAULT_CONFIG(r) Clock{r,I2S_MCLK_MULTIPLE_256}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(w,m) Slot{w,m,I2S_STD_SLOT_LEFT}
struct i2s_event_data_t{};using Callback=bool(*)(i2s_chan_handle_t,i2s_event_data_t*,void*);
struct i2s_event_callbacks_t {Callback on_recv_q_ovf=nullptr;};static Callback overflowCallback;static void* overflowContext;
static int readError=ESP_OK;static size_t readLimit=SIZE_MAX;static bool warmupFail=false;
esp_err_t i2s_new_channel(const i2s_chan_config_t*,i2s_chan_handle_t* tx,i2s_chan_handle_t* rx){++creates;if(failStage==1)return ESP_FAIL;auto* h=tx?&txChannel:&rxChannel;assert(!h->allocated);h->allocated=true;h->tx=tx!=nullptr;*(tx?tx:rx)=h;return ESP_OK;}
esp_err_t i2s_channel_init_std_mode(Channel* h,const i2s_std_config_t* c){assert(h->allocated);assert(c->gpio_cfg.mclk==13&&c->gpio_cfg.bclk==12&&c->gpio_cfg.ws==10);assert(c->gpio_cfg.din==(h->tx?-1:11));if(!h->tx)assert(c->clk_cfg.mclk_multiple==((c->clk_cfg.rate==8000||c->clk_cfg.rate==32000)?I2S_MCLK_MULTIPLE_512:I2S_MCLK_MULTIPLE_256));if(!h->tx)assert(c->slot_cfg.slot_mask==I2S_STD_SLOT_LEFT&&c->slot_cfg.mode==I2S_SLOT_MODE_MONO);return failStage==2?ESP_FAIL:ESP_OK;}
esp_err_t i2s_channel_register_event_callback(Channel*,const i2s_event_callbacks_t* c,void* context){if(failStage==3)return ESP_FAIL;overflowCallback=c->on_recv_q_ovf;overflowContext=context;return ESP_OK;}
esp_err_t i2s_channel_enable(Channel* h){if(failStage==4)return ESP_FAIL;h->enabled=true;return ESP_OK;}
esp_err_t i2s_channel_disable(Channel* h){assert(h->enabled);h->enabled=false;return ESP_OK;}
esp_err_t i2s_del_channel(Channel* h){assert(h->allocated&&!h->enabled);h->allocated=false;++deletes;return ESP_OK;}
esp_err_t i2s_channel_read(Channel* h,void* out,size_t bytes,size_t* got,uint32_t){assert(h->enabled&&!lockDepth);++reads;*got=warmupFail?0:std::min(bytes,readLimit);std::memset(out,0x12,*got);return warmupFail?ESP_ERR_TIMEOUT:readError;}
esp_err_t i2s_channel_write(Channel* h,const void*,size_t bytes,size_t* got,uint32_t){assert(h->enabled&&h->tx&&!lockDepth);*got=bytes;return ESP_OK;}
using es8311_handle_t=void*;constexpr int ES8311_ADDRESS_0=0x18,ES8311_RESOLUTION_16=16;
struct es8311_clock_config_t {bool a,b,c;uint32_t mclk,rate;};
es8311_handle_t es8311_create(int,int){return &txChannel;}int es8311_delete(void*){return 0;}
int es8311_init(void*,const es8311_clock_config_t*,int,int){return failStage==6?ESP_FAIL:ESP_OK;}
int es8311_voice_volume_set(void*,int,void*){return ESP_OK;}
// INSERT_HEADER
// INSERT_MIC
// INSERT_SPEAKER
static void idle(){assert(!rxChannel.allocated&&!txChannel.allocated&&!amp);assert(waveshareAudioTryClaim());waveshareAudioRelease();}
int main(){
#if ENABLE_MICROPHONE_SENSOR
 assert(!waveshareMicStart(12345)&&creates==0);idle();
 present=false;assert(!waveshareMicStart(16000));idle();present=true;
 for(int failure=1;failure<=5;++failure){failStage=failure;assert(!waveshareMicStart(16000));idle();}failStage=0;
 warmupFail=true;assert(!waveshareMicStart(16000));idle();warmupFail=false;
 for(uint32_t rate:{8000,16000,32000,44100,48000}){
  writes.clear();assert(waveshareMicStart(rate));assert(rxChannel.enabled);
  assert(std::any_of(writes.begin(),writes.end(),[](Write w){return w.address==0x43&&w.value==0x1e;}));
  assert(std::any_of(writes.begin(),writes.end(),[rate](Write w){return w.address==0x04&&w.value==((rate==8000||rate==32000)?2:1);}));
  assert(std::any_of(writes.begin(),writes.end(),[](Write w){return w.address==0x4c&&w.value==0xff;}));
  int16_t pcm[20];readLimit=10;readError=ESP_ERR_TIMEOUT;
  assert(waveshareMicRead(pcm,20,5)==5&&pcm[0]==0x1212);
  readError=ESP_FAIL;assert(waveshareMicRead(pcm,20,5)==0);readError=ESP_OK;readLimit=SIZE_MAX;
  i2s_event_data_t event;assert(!overflowCallback(&rxChannel,&event,overflowContext));assert(waveshareMicOverruns()==1);
  const int before=creates;assert(speakerPlayTone(660,50,10)==ESP_ERR_INVALID_STATE&&creates==before&&rxChannel.enabled);
  waveshareMicStop();waveshareMicStop();idle();
 }
 assert(waveshareAudioTryClaim());assert(!waveshareMicStart(16000));assert(!waveshareAudioTryClaim());waveshareAudioRelease();idle();
 #endif
 assert(speakerPlayTone(660,50,10)==ESP_OK);idle();
 for(int failure:{1,2,4,6}){failStage=failure;assert(speakerPlayTone(660,50,10)!=ESP_OK);idle();}failStage=0;
 assert(speakerPlayTone(0,50,10)==ESP_ERR_INVALID_ARG);idle();
 std::puts("Waveshare production mic/speaker: rate, failure cleanup, PCM timeout, overflow and shared-pin exclusion passed");
}
