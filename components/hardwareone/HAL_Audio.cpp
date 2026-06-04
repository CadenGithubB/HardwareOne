/**
 * HAL_Audio.cpp — single owner of microphone PCM capture.
 * See HAL_Audio.h for the abstraction overview.
 *
 * Phase 1: owns the onboard PDM I2S channel (the lifecycle that used to be
 * duplicated in System_Microphone.cpp and System_ESPSR.cpp). The G2 source is
 * wired through in Phase 2 — its branches here already call the existing
 * G2_Glasses ring API (stubbed to no-ops when ENABLE_G2_GLASSES is off).
 */

#include "HAL_Audio.h"

#if ENABLE_MICROPHONE_SENSOR

#include <string.h>
#include <driver/i2s_pdm.h>
#include "System_Mutex.h"     // I2sMicLockGuard — shared I2S-mic hardware lock
#include "System_Debug.h"
#include "G2_Glasses.h"       // g2MicSetAfeFeedActive / g2MicReadPcmSamples (stubs if G2 off)

// PDM mic pins. BuildConfig defines these for the XIAO ESP32S3 Sense; keep a
// fallback so the file is self-contained.
#ifndef MIC_CLK_PIN
#define MIC_CLK_PIN   42   // GPIO42 (PDM clock)
#endif
#ifndef MIC_DATA_PIN
#define MIC_DATA_PIN  41   // GPIO41 (PDM data)
#endif

// ── State ────────────────────────────────────────────────────────────────────
static AudioSource        gAudioSource  = AUDIO_SRC_LOCAL_PDM;
static i2s_chan_handle_t  gPdmRx        = nullptr;
static const char*        gCaptureOwner = nullptr;   // null = idle
static uint32_t           gCaptureRate  = AUDIO_HAL_SAMPLE_RATE;

// ── PDM backend ──────────────────────────────────────────────────────────────
// Reconciled from the two former inits: identical pins/DMA, parameterized rate,
// and the more-robust PCM-format slot config (a superset of the plain one).
// Caller must hold I2sMicLockGuard.
static bool pdmStartLocked(uint32_t rate) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 4;
  chan_cfg.dma_frame_num = 1024;

  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &gPdmRx);
  if (err != ESP_OK) {
    WARN_SYSTEMF("[HAL_AUDIO] i2s_new_channel failed: 0x%x (%s)", err, esp_err_to_name(err));
    gPdmRx = nullptr;
    return false;
  }

  i2s_pdm_rx_slot_config_t slot_cfg =
#ifdef I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG
      I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#else
      I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
#if !defined(I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG) && defined(I2S_PDM_DATA_FMT_PCM)
  slot_cfg.data_fmt = I2S_PDM_DATA_FMT_PCM;
#endif

  i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate),
    .slot_cfg = slot_cfg,
    .gpio_cfg = {
      .clk = (gpio_num_t)MIC_CLK_PIN,
      .din = (gpio_num_t)MIC_DATA_PIN,
      .invert_flags = { .clk_inv = false },
    },
  };

  err = i2s_channel_init_pdm_rx_mode(gPdmRx, &pdm_cfg);
  if (err != ESP_OK) {
    WARN_SYSTEMF("[HAL_AUDIO] i2s_channel_init_pdm_rx_mode failed at %u Hz: 0x%x (%s)",
                 (unsigned)rate, err, esp_err_to_name(err));
    i2s_del_channel(gPdmRx);
    gPdmRx = nullptr;
    return false;
  }

  err = i2s_channel_enable(gPdmRx);
  if (err != ESP_OK) {
    WARN_SYSTEMF("[HAL_AUDIO] i2s_channel_enable failed: 0x%x (%s)", err, esp_err_to_name(err));
    i2s_del_channel(gPdmRx);
    gPdmRx = nullptr;
    return false;
  }

  // PDM warm-up flush (the sensor needs a few reads before stable samples).
  int16_t flushBuf[256];
  size_t  flushBytes = 0;
  for (int i = 0; i < 10; i++) {
    i2s_channel_read(gPdmRx, flushBuf, sizeof(flushBuf), &flushBytes, pdMS_TO_TICKS(100));
  }
  INFO_SYSTEMF("[HAL_AUDIO] PDM mic started: %u Hz, CLK=%d DATA=%d", (unsigned)rate,
               (int)MIC_CLK_PIN, (int)MIC_DATA_PIN);
  return true;
}

// Caller must hold I2sMicLockGuard.
static void pdmStopLocked() {
  if (gPdmRx) {
    i2s_channel_disable(gPdmRx);
    i2s_del_channel(gPdmRx);
    gPdmRx = nullptr;
  }
}

// ── Public API ───────────────────────────────────────────────────────────────
AudioSource audioGetSource() { return gAudioSource; }

bool audioSetSource(AudioSource src) {
  if (gCaptureOwner) {
    WARN_SYSTEMF("[HAL_AUDIO] refusing source switch while '%s' is capturing", gCaptureOwner);
    return false;
  }
  gAudioSource = src;
  return true;
}

bool audioCaptureActive()      { return gCaptureOwner != nullptr; }
const char* audioCaptureOwner(){ return gCaptureOwner ? gCaptureOwner : ""; }

bool audioCaptureStart(const char* owner, uint32_t sampleRate) {
  I2sMicLockGuard guard("audio.start");

  if (gCaptureOwner) {
    // Idempotent re-start by the same owner; reject a different owner.
    const bool sameOwner = owner && strcmp(owner, gCaptureOwner) == 0;
    if (!sameOwner) {
      WARN_SYSTEMF("[HAL_AUDIO] capture busy (held by '%s'), '%s' denied",
                   gCaptureOwner, owner ? owner : "?");
    }
    return sameOwner;
  }

  if (sampleRate == 0) sampleRate = AUDIO_HAL_SAMPLE_RATE;
  gCaptureRate = sampleRate;

  bool ok;
  if (gAudioSource == AUDIO_SRC_LOCAL_PDM) {
    ok = pdmStartLocked(sampleRate);
  } else {  // AUDIO_SRC_G2_LEFT
    ok = g2MicSetAfeFeedActive(true);
  }
  if (!ok) return false;

  gCaptureOwner = owner ? owner : "audio";
  return true;
}

void audioCaptureStop(const char* owner) {
  I2sMicLockGuard guard("audio.stop");
  if (!gCaptureOwner) return;
  if (owner && strcmp(owner, gCaptureOwner) != 0) {
    WARN_SYSTEMF("[HAL_AUDIO] stop by '%s' ignored — capture held by '%s'",
                 owner, gCaptureOwner);
    return;
  }
  if (gAudioSource == AUDIO_SRC_LOCAL_PDM) pdmStopLocked();
  else                                     g2MicSetAfeFeedActive(false);
  gCaptureOwner = nullptr;
}

size_t audioReadPcm(int16_t* out, size_t maxSamples, uint32_t timeoutMs) {
  if (!out || maxSamples == 0) return 0;

  if (gAudioSource == AUDIO_SRC_G2_LEFT) {
    return g2MicReadPcmSamples(out, maxSamples, timeoutMs);
  }

  // LOCAL_PDM
  if (!gPdmRx) return 0;
  size_t bytesRead = 0;
  esp_err_t err;
  {
    I2sMicLockGuard guard("audio.read");
    if (!gPdmRx) return 0;
    err = i2s_channel_read(gPdmRx, out, maxSamples * sizeof(int16_t),
                           &bytesRead, pdMS_TO_TICKS(timeoutMs));
  }
  if (err != ESP_OK) return 0;
  return bytesRead / sizeof(int16_t);
}

#endif // ENABLE_MICROPHONE_SENSOR
