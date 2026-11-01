#include "HAL_Speaker.h"
#include "System_BuildConfig.h"
#if ENABLE_SPEAKER
#include "System_I2C.h"
#include "HAL_Audio_Waveshare.h"
#include "driver/i2s_std.h"
#include "third_party/es8311/es8311.h"
#include <Arduino.h>
#include <cmath>

namespace {
constexpr uint32_t rate = 16000;
struct Playback {
  i2s_chan_handle_t tx = nullptr;
  es8311_handle_t codec = nullptr;
  bool enabled = false;
  ~Playback() {
    digitalWrite(WS_SPEAKER_AMP_PIN, LOW);
    if (enabled)
      i2s_channel_disable(tx);
    if (tx)
      i2s_del_channel(tx);
    if (codec)
      es8311_delete(codec);
    waveshareAudioRelease();
  }
};
} // namespace

esp_err_t speakerPlayTone(uint32_t frequencyHz, uint32_t durationMs,
                          uint8_t volume) {
  if (frequencyHz < 100 || frequencyHz > 4000 || durationMs < 50 ||
      durationMs > 2000 || volume > 60)
    return ESP_ERR_INVALID_ARG;
  if (!waveshareAudioTryClaim())
    return ESP_ERR_INVALID_STATE;
  Playback playback;
  pinMode(WS_SPEAKER_AMP_PIN, OUTPUT);
  digitalWrite(WS_SPEAKER_AMP_PIN, LOW);
  if (!i2cPingAddress(ES8311_ADDRESS_0, 100000, 500, WS_SPEAKER_I2C_BUS))
    return ESP_ERR_NOT_FOUND;
  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = 4;
  channel.dma_frame_num = 128;
  esp_err_t err = i2s_new_channel(&channel, &playback.tx, nullptr);
  if (err != ESP_OK)
    return err;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = static_cast<gpio_num_t>(WS_SPEAKER_MCLK_PIN);
  config.gpio_cfg.bclk = static_cast<gpio_num_t>(WS_SPEAKER_BCLK_PIN);
  config.gpio_cfg.ws = static_cast<gpio_num_t>(WS_SPEAKER_WS_PIN);
  config.gpio_cfg.dout = static_cast<gpio_num_t>(WS_SPEAKER_DATA_PIN);
  config.gpio_cfg.din = GPIO_NUM_NC;
  err = i2s_channel_init_std_mode(playback.tx, &config);
  if (err != ESP_OK)
    return err;
  err = i2s_channel_enable(playback.tx);
  if (err != ESP_OK)
    return err;
  playback.enabled = true;
  playback.codec = es8311_create(WS_SPEAKER_I2C_PORT, ES8311_ADDRESS_0);
  if (!playback.codec)
    return ESP_ERR_NO_MEM;
  const bool configured = i2cDeviceTransaction(
      WS_SPEAKER_I2C_BUS, ES8311_ADDRESS_0, 100000, 500, [&]() -> bool {
        const es8311_clock_config_t clock = {false, false, true, rate * 256,
                                             rate};
        return es8311_init(playback.codec, &clock, ES8311_RESOLUTION_16,
                           ES8311_RESOLUTION_16) == ESP_OK &&
               es8311_voice_volume_set(playback.codec, volume, nullptr) ==
                   ESP_OK;
      });
  if (!configured)
    return ESP_FAIL;
  digitalWrite(WS_SPEAKER_AMP_PIN, HIGH);
  const uint32_t samples = rate * durationMs / 1000;
  int16_t frame[128 * 2];
  for (uint32_t offset = 0; offset < samples + 1024; offset += 128) {
    for (uint32_t i = 0; i < 128; ++i) {
      const uint32_t n = offset + i;
      float envelope = 0;
      if (n < samples)
        envelope = fminf(1.f, fminf(n / 320.f, (samples - n) / 320.f));
      const int16_t value = static_cast<int16_t>(
          6000.f * envelope * sinf(6.2831853f * frequencyHz * n / rate));
      frame[i * 2] = frame[i * 2 + 1] = value;
    }
    size_t written = 0;
    err = i2s_channel_write(playback.tx, frame, sizeof(frame), &written, 500);
    if (err != ESP_OK || written != sizeof(frame))
      return err == ESP_OK ? ESP_FAIL : err;
  }
  delay(40);
  return ESP_OK;
}
#endif
