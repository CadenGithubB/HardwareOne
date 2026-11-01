/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2021 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
// Codec sequence adapted from Waveshare's 10_Mic_Record reference.
// See docs/WAVESHARE_P4_SUPPORT.md for the pinned source and board wiring.
#include "System_BuildConfig.h"
#if MIC_BACKEND_ES7210
#include "HAL_Audio_Waveshare.h"
#include "System_I2C.h"
#include "System_Debug.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include <atomic>

namespace {
std::atomic_flag audioBusBusy=ATOMIC_FLAG_INIT;
#if ENABLE_MICROPHONE_SENSOR
i2s_chan_handle_t rx=nullptr;
bool enabled=false,claimed=false,codecTouched=false;
DRAM_ATTR uint32_t overruns=0;
bool IRAM_ATTR overrun(i2s_chan_handle_t,i2s_event_data_t*,void* context) {
  __atomic_fetch_add(static_cast<uint32_t*>(context),1u,__ATOMIC_RELAXED);
  return false;
}
struct Register { uint8_t address,value; };
// All bus access uses the manager's mutex/clock/recovery path. No Wire.begin,
// register reads during discovery, or delays while holding the I2C lock.
bool registers(const Register* values,size_t count) {
  return i2cDeviceTransaction(WS_MIC_I2C_BUS,WS_MIC_I2C_ADDRESS,100000,500,[&]() {
    auto* manager=I2CDeviceManager::getInstance();
    TwoWire* wire=manager?manager->getWire(WS_MIC_I2C_BUS):nullptr;
    if(!wire)return false;
    for(size_t i=0;i<count;++i) {
      wire->beginTransmission(WS_MIC_I2C_ADDRESS);
      if(wire->write(values[i].address)!=1 || wire->write(values[i].value)!=1) {
        wire->endTransmission();return false;
      }
      if(wire->endTransmission()!=0)return false;
    }
    return true;
  });
}
// Vendor coefficients at 256x MCLK (8k/32k use 512x; lower clock not qualified).
bool clockFor(uint32_t rate,i2s_mclk_multiple_t& multiple,uint8_t& mainClock) {
  multiple=I2S_MCLK_MULTIPLE_256;
  switch(rate) {
    case 8000: multiple=I2S_MCLK_MULTIPLE_512;mainClock=0x81;return true;
    case 16000:case 44100:case 48000:mainClock=0xc1;return true;
    case 32000:multiple=I2S_MCLK_MULTIPLE_512;mainClock=0x81;return true;
    default:return false;
  }
}
#endif
}
bool waveshareAudioTryClaim() { return !audioBusBusy.test_and_set(); }
void waveshareAudioRelease() { audioBusBusy.clear(); }
#if ENABLE_MICROPHONE_SENSOR
void waveshareMicStop() {
  if(!claimed)return;
  if(enabled) {i2s_channel_disable(rx);enabled=false;}
  if(codecTouched) {
    const Register stop[]={{0x47,0xff},{0x48,0xff},{0x49,0xff},{0x4a,0xff},
                           {0x4b,0xff},{0x4c,0xff},{0x01,0x7f},{0x06,0x07}};
    if(!registers(stop,sizeof(stop)/sizeof(stop[0])))
      WARN_SYSTEMF("[HAL_AUDIO] ES7210 power-down failed");
    codecTouched=false;
  }
  if(rx) {i2s_del_channel(rx);rx=nullptr;}
  claimed=false;waveshareAudioRelease();
}
bool waveshareMicStart(uint32_t rate) {
  i2s_mclk_multiple_t multiple;
  uint8_t mainClock=0;
  if(!clockFor(rate,multiple,mainClock)) {
    WARN_SYSTEMF("[HAL_AUDIO] ES7210 unsupported sample rate: %lu",(unsigned long)rate);
    return false;
  }
  if(!waveshareAudioTryClaim()) {
    WARN_SYSTEMF("[HAL_AUDIO] onboard audio busy; stop speaker playback first");
    return false;
  }
  claimed=true;
  if(!i2cPingAddress(WS_MIC_I2C_ADDRESS,100000,500,WS_MIC_I2C_BUS)) {
    WARN_SYSTEMF("[HAL_AUDIO] ES7210 not responding at 0x40");
    waveshareMicStop();return false;
  }
  i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
  channel.dma_desc_num=8;channel.dma_frame_num=512;
  esp_err_t err=i2s_new_channel(&channel,nullptr,&rx);
  if(err==ESP_OK) {
    i2s_std_config_t config={};
    config.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(rate);
    config.clk_cfg.mclk_multiple=multiple;
    config.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO);
    config.slot_cfg.slot_mask=I2S_STD_SLOT_LEFT;
    config.gpio_cfg.mclk=static_cast<gpio_num_t>(WS_SPEAKER_MCLK_PIN);
    config.gpio_cfg.bclk=static_cast<gpio_num_t>(WS_SPEAKER_BCLK_PIN);
    config.gpio_cfg.ws=static_cast<gpio_num_t>(WS_SPEAKER_WS_PIN);
    config.gpio_cfg.dout=GPIO_NUM_NC;
    config.gpio_cfg.din=static_cast<gpio_num_t>(WS_MIC_DATA_PIN);
    err=i2s_channel_init_std_mode(rx,&config);
  }
  if(err==ESP_OK) {
    i2s_event_callbacks_t callbacks={};callbacks.on_recv_q_ovf=overrun;
    __atomic_store_n(&overruns,0u,__ATOMIC_RELAXED);
    err=i2s_channel_register_event_callback(rx,&callbacks,&overruns);
  }
  if(err==ESP_OK) {err=i2s_channel_enable(rx);enabled=err==ESP_OK;}
  if(err!=ESP_OK) {
    WARN_SYSTEMF("[HAL_AUDIO] ES7210 I2S start failed: %s",esp_err_to_name(err));
    waveshareMicStop();return false;
  }
  // Slave, 16-bit Philips I2S. The schematic puts the two onboard mics on
  // ADC1/2 (SDOUT1); ADC3/4 are speaker feedback. Enable/gain only the mics.
  const Register setup[]={
    {0x00,0xff},{0x00,0x41},{0x01,0x1f},{0x09,0x30},{0x0a,0x30},
    {0x08,0x00},{0x40,0xc3},{0x41,0x70},{0x42,0x70},{0x07,0x20},
    {0x02,0xc1},{0x02,mainClock},{0x04,uint8_t(multiple/256)},{0x05,0x00},
    {0x11,0x60},{0x12,0x00},
    {0x43,0x1e},{0x44,0x1e},{0x45,0x00},{0x46,0x00},
    {0x4b,0x00},{0x4c,0xff},{0x01,0x14},{0x06,0x00},
    {0x47,0x00},{0x48,0x00},{0x49,0xff},{0x4a,0xff}
  };
  codecTouched=true;
  if(!registers(setup,sizeof(setup)/sizeof(setup[0]))) {
    WARN_SYSTEMF("[HAL_AUDIO] ES7210 configuration failed");
    waveshareMicStop();return false;
  }
  // Discard bias/ADC startup transients without saving or publishing audio.
  int16_t discard[256];unsigned complete=0;
  for(unsigned attempt=0;attempt<12 && complete<4;++attempt) {
    size_t bytes=0;
    err=i2s_channel_read(rx,discard,sizeof(discard),&bytes,100);
    if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT)break;
    if(bytes==sizeof(discard))++complete;
    else if(bytes)break;
  }
  for(auto& sample:discard)sample=0;
  if(complete!=4) {
    WARN_SYSTEMF("[HAL_AUDIO] ES7210 startup PCM unavailable");
    waveshareMicStop();return false;
  }
  INFO_SYSTEMF("[HAL_AUDIO] ES7210 onboard mic started: %lu Hz",(unsigned long)rate);
  return true;
}
size_t waveshareMicRead(int16_t* out,size_t samples,uint32_t timeoutMs) {
  if(!enabled || !rx || !out || !samples)return 0;
  size_t bytes=0;
  const esp_err_t err=i2s_channel_read(rx,out,samples*sizeof(int16_t),&bytes,timeoutMs);
  return err==ESP_OK || err==ESP_ERR_TIMEOUT ? bytes/sizeof(int16_t):0;
}
uint32_t waveshareMicOverruns() { return __atomic_load_n(&overruns,__ATOMIC_RELAXED); }
#endif
#endif
