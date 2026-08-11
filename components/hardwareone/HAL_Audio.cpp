/**
 * HAL_Audio.cpp — single owner of microphone PCM capture.
 * See HAL_Audio.h for the abstraction overview.
 *
 * Gated on ENABLE_MICROPHONE (PDM silicon OR a G2 that can supply audio), so the
 * source-agnostic layer survives on PDM-less boards (FeatherS3) where the G2
 * glasses mic is the only source. The PDM I2S backend is inner-gated on
 * ENABLE_MICROPHONE_SENSOR; the G2 branches call the G2_Glasses ring/stream API
 * (which resolves to no-op stubs when ENABLE_G2_GLASSES is off).
 */

#include "HAL_Audio.h"

#if ENABLE_MICROPHONE

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "System_Debug.h"
#include "G2_Glasses.h"       // g2LeftConnected / g2MicStreamEnable / g2MicSetAfeFeedActive / g2MicReadPcmSamples (stubs if G2 off)

#if ENABLE_MICROPHONE_SENSOR
#include <driver/i2s_pdm.h>
#include "System_Mutex.h"     // I2sMicLockGuard — shared I2S-mic hardware lock (PDM only)
// PDM mic pins. BuildConfig defines these for the XIAO ESP32S3 Sense; keep a
// fallback so the file is self-contained.
#ifndef MIC_CLK_PIN
#define MIC_CLK_PIN   42   // GPIO42 (PDM clock)
#endif
#ifndef MIC_DATA_PIN
#define MIC_DATA_PIN  41   // GPIO41 (PDM data)
#endif
#endif // ENABLE_MICROPHONE_SENSOR

// ── State ────────────────────────────────────────────────────────────────────
// gAudioSource + owner/phase are guarded by gAudioStateMutex, a mutex distinct
// from the I2S hardware lock so G2-only builds (no I2S) and the source selector
// are coherent and never serialize on an I2S-scoped lock. audioReadPcm reads
// gAudioSource locklessly: it only changes when no owner holds capture, so it is
// stable for the life of a capture session. STOPPING remains owned/busy until
// backend teardown finishes, so a replacement start cannot race old cleanup.
enum class AudioCapturePhase : uint8_t { IDLE, STARTING, ACTIVE, STOPPING };

static volatile AudioSource       gAudioSource  = AUDIO_SRC_NONE; // no compile-time default
static volatile AudioCapturePhase gCapturePhase = AudioCapturePhase::IDLE;
static const char*                gCaptureOwner = nullptr;        // null only in IDLE
static uint32_t                   gCaptureRate  = AUDIO_HAL_SAMPLE_RATE;
static StaticSemaphore_t          gAudioStateMutexStorage;
static SemaphoreHandle_t          gAudioStateMutex = nullptr;
static portMUX_TYPE               gAudioStateInitMux = portMUX_INITIALIZER_UNLOCKED;

static bool ensureAudioStateMutex() {
  // First use can come from the BLE host task or a command task. Serialize the
  // one-time construction and use static storage so allocation failure cannot
  // turn into xSemaphoreTake(nullptr) or let two callers protect different
  // copies of the state.
  portENTER_CRITICAL(&gAudioStateInitMux);
  if (!gAudioStateMutex) {
    gAudioStateMutex = xSemaphoreCreateMutexStatic(&gAudioStateMutexStorage);
  }
  const bool ready = gAudioStateMutex != nullptr;
  portEXIT_CRITICAL(&gAudioStateInitMux);
  if (!ready) WARN_SYSTEMF("[HAL_AUDIO] state mutex initialization failed");
  return ready;
}

// ── PDM backend (onboard I2S mic — inner-gated on ENABLE_MICROPHONE_SENSOR) ────
#if ENABLE_MICROPHONE_SENSOR
static i2s_chan_handle_t gPdmRx = nullptr;

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
#endif // ENABLE_MICROPHONE_SENSOR

// ── Availability (runtime; no compile-time default source) ────────────────────
bool audioSourceAvailable(AudioSource src) {
  switch (src) {
    case AUDIO_SRC_LOCAL_PDM:
#if ENABLE_MICROPHONE_SENSOR
      return true;   // I2S/PDM is unprobeable — the soldered mic is present iff compiled
#else
      return false;
#endif
    case AUDIO_SRC_G2_LEFT:
      return g2LeftConnected();   // LEFT temple connected AND audio-notify subscribed
    default:
      return false;               // AUDIO_SRC_NONE and anything else
  }
}

bool audioAnySourceAvailable() {
  return audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ||
         audioSourceAvailable(AUDIO_SRC_G2_LEFT);
}

size_t audioListAvailableSources(AudioSource* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  if (audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) && n < cap) out[n++] = AUDIO_SRC_LOCAL_PDM;
  if (audioSourceAvailable(AUDIO_SRC_G2_LEFT)   && n < cap) out[n++] = AUDIO_SRC_G2_LEFT;
  return n;
}

// ── Public API ───────────────────────────────────────────────────────────────
AudioSource audioGetSource() { return gAudioSource; }

bool audioSetSource(AudioSource src) {
  if (!ensureAudioStateMutex()) return false;
  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  if (gCapturePhase != AudioCapturePhase::IDLE) {
    WARN_SYSTEMF("[HAL_AUDIO] refusing source switch while '%s' is capturing", gCaptureOwner);
    xSemaphoreGive(gAudioStateMutex);
    return false;
  }
  // Allow NONE (explicit deselect); any real source must be currently available.
  if (src != AUDIO_SRC_NONE && !audioSourceAvailable(src)) {
    WARN_SYSTEMF("[HAL_AUDIO] refusing unavailable source %d", (int)src);
    xSemaphoreGive(gAudioStateMutex);
    return false;
  }
  gAudioSource = src;
  xSemaphoreGive(gAudioStateMutex);
  return true;
}

bool audioCaptureActive() {
  const AudioCapturePhase phase = gCapturePhase;
  return phase == AudioCapturePhase::STARTING ||
         phase == AudioCapturePhase::ACTIVE;
}
bool audioCaptureBusy() { return gCapturePhase != AudioCapturePhase::IDLE; }
bool audioCaptureOwnedBy(const char* owner) {
  if (!ensureAudioStateMutex()) return false;
  const char* requestedOwner = owner ? owner : "audio";
  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  const bool owned = gCapturePhase != AudioCapturePhase::IDLE &&
                     gCaptureOwner &&
                     strcmp(requestedOwner, gCaptureOwner) == 0;
  xSemaphoreGive(gAudioStateMutex);
  return owned;
}
const char* audioCaptureOwner(){ return gCaptureOwner ? gCaptureOwner : ""; }

bool audioCaptureStart(const char* owner, uint32_t sampleRate) {
  if (!ensureAudioStateMutex()) return false;
  const char* requestedOwner = owner ? owner : "audio";

  // ── Claim ownership + resolve the source under the state lock (fast) ─────────
  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  if (gCapturePhase != AudioCapturePhase::IDLE) {
    // Only an already ACTIVE same-owner call is idempotent. Returning success
    // for STARTING would expose a backend that has not actually armed yet.
    const bool sameOwner = strcmp(requestedOwner, gCaptureOwner) == 0;
    const bool alreadyActive = sameOwner &&
                               gCapturePhase == AudioCapturePhase::ACTIVE;
    if (!alreadyActive) {
      WARN_SYSTEMF("[HAL_AUDIO] capture busy (held by '%s'), '%s' denied",
                   gCaptureOwner, requestedOwner);
    }
    xSemaphoreGive(gAudioStateMutex);
    return alreadyActive;
  }

  // Resolve: honor the selected source if still available, else fall back to any
  // available source (PDM-first). No implicit assumption that a source exists.
  AudioSource src = gAudioSource;
  if (src == AUDIO_SRC_NONE || !audioSourceAvailable(src)) {
    if      (audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) src = AUDIO_SRC_LOCAL_PDM;
    else if (audioSourceAvailable(AUDIO_SRC_G2_LEFT))   src = AUDIO_SRC_G2_LEFT;
    else {
      xSemaphoreGive(gAudioStateMutex);
      WARN_SYSTEMF("[HAL_AUDIO] '%s' start failed — no mic source available",
                   requestedOwner);
      return false;
    }
  }
  if (sampleRate == 0) sampleRate = AUDIO_HAL_SAMPLE_RATE;
  gAudioSource  = src;               // latch resolved source for audioReadPcm dispatch
  gCaptureRate  = sampleRate;
  gCaptureOwner = requestedOwner;            // provisional claim; rolled back on failure
  gCapturePhase = AudioCapturePhase::STARTING;
  xSemaphoreGive(gAudioStateMutex);

  // ── Start the backend OUTSIDE the state lock (BLE send / PDM warm-up block) ──
  bool ok = false;
  if (src == AUDIO_SRC_LOCAL_PDM) {
#if ENABLE_MICROPHONE_SENSOR
    I2sMicLockGuard guard("audio.start");
    ok = pdmStartLocked(sampleRate);
#endif
  } else if (src == AUDIO_SRC_G2_LEFT) {
    // Arm the local ring/decoder AND tell the glasses to actually stream (the
    // AFE feed alone never sends AudioCtrCmd → zero frames → silent dead mic).
    ok = g2MicStreamEnable(true) && g2MicSetAfeFeedActive(true);
    if (!ok) g2MicStreamEnable(false);   // undo a half-armed stream
  }

  // Revalidate the provisional claim. A source-loss callback may have moved it
  // to STOPPING while BLE/PDM startup blocked. STARTING cannot be reused, so a
  // cancelled starter can safely tear down its own backend before publishing
  // IDLE; no newer owner can be admitted in between.
  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  const bool sameOwner = gCaptureOwner &&
                         strcmp(requestedOwner, gCaptureOwner) == 0;
  if (ok && sameOwner && gCapturePhase == AudioCapturePhase::STARTING) {
    gCapturePhase = AudioCapturePhase::ACTIVE;
    xSemaphoreGive(gAudioStateMutex);
    // NOTE: no FAST-interval hold here. The HAL claim includes the idle-open
    // mic (autostart at boot), which must NOT pin the glasses' radio or paint
    // a page — the RECORDER acquires FAST + the capture container around each
    // actual recording (System_Microphone startRecordingInternal/finalize).
    return true;
  }
  const bool cleanupStartedBackend = ok;
  if (gCapturePhase == AudioCapturePhase::STARTING) {
    gCapturePhase = AudioCapturePhase::STOPPING;
  }
  xSemaphoreGive(gAudioStateMutex);

  if (cleanupStartedBackend) {
    if (src == AUDIO_SRC_LOCAL_PDM) {
#if ENABLE_MICROPHONE_SENSOR
      I2sMicLockGuard guard("audio.start.cancel");
      pdmStopLocked();
#endif
    } else if (src == AUDIO_SRC_G2_LEFT) {
      g2MicStreamEnable(false);
      g2MicSetAfeFeedActive(false);
    }
  }

  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  // STOPPING excludes replacement claims until cleanup above is complete.
  if (gCapturePhase == AudioCapturePhase::STOPPING) {
    gCaptureOwner = nullptr;
    gCapturePhase = AudioCapturePhase::IDLE;
  }
  xSemaphoreGive(gAudioStateMutex);
  return false;
}

void audioCaptureStop(const char* owner) {
  if (!ensureAudioStateMutex()) return;

  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  if (gCapturePhase == AudioCapturePhase::IDLE || !gCaptureOwner) {
    xSemaphoreGive(gAudioStateMutex);
    return;
  }
  if (owner && strcmp(owner, gCaptureOwner) != 0) {
    WARN_SYSTEMF("[HAL_AUDIO] stop by '%s' ignored — capture held by '%s'",
                 owner, gCaptureOwner);
    xSemaphoreGive(gAudioStateMutex);
    return;
  }
  if (gCapturePhase == AudioCapturePhase::STARTING) {
    // The starter owns backend startup and will observe this cancellation,
    // unwind any half-started backend, then publish IDLE. This path must remain
    // non-blocking for BLE disconnect callbacks.
    gCapturePhase = AudioCapturePhase::STOPPING;
    xSemaphoreGive(gAudioStateMutex);
    return;
  }
  if (gCapturePhase == AudioCapturePhase::STOPPING) {
    xSemaphoreGive(gAudioStateMutex);
    return;
  }
  const AudioSource src = gAudioSource;
  gCapturePhase = AudioCapturePhase::STOPPING;
  xSemaphoreGive(gAudioStateMutex);

  // Tear down the backend outside the lock.
  if (src == AUDIO_SRC_LOCAL_PDM) {
#if ENABLE_MICROPHONE_SENSOR
    I2sMicLockGuard guard("audio.stop");
    pdmStopLocked();
#endif
  } else if (src == AUDIO_SRC_G2_LEFT) {
    g2MicLinkFastRelease();              // safety net (latched no-op if the
                                         // recorder already released)
    g2MicStreamEnable(false);            // AudioCtrCmd{en=0} — stop the glasses stream
    g2MicSetAfeFeedActive(false);        // disarm the ring/decoder
  }

  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  gCaptureOwner = nullptr;
  gCapturePhase = AudioCapturePhase::IDLE;
  xSemaphoreGive(gAudioStateMutex);
}

size_t audioReadPcm(int16_t* out, size_t maxSamples, uint32_t timeoutMs) {
  if (!out || maxSamples == 0) return 0;
  if (gCapturePhase != AudioCapturePhase::ACTIVE) return 0;

  if (gAudioSource == AUDIO_SRC_G2_LEFT) {
    return g2MicReadPcmSamples(out, maxSamples, timeoutMs);
  }

#if ENABLE_MICROPHONE_SENSOR
  if (gAudioSource != AUDIO_SRC_LOCAL_PDM) return 0;
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
#else
  return 0;   // no PDM backend on this board; G2 handled above
#endif
}

size_t audioTrimBufferedPcm(const char* owner, size_t keepNewestSamples) {
  if (!owner || !owner[0] || !ensureAudioStateMutex()) return 0;

  AudioSource source = AUDIO_SRC_NONE;
  xSemaphoreTake(gAudioStateMutex, portMAX_DELAY);
  if (gCapturePhase == AudioCapturePhase::ACTIVE && gCaptureOwner &&
      strcmp(owner, gCaptureOwner) == 0) {
    source = gAudioSource;
  }
  xSemaphoreGive(gAudioStateMutex);

  // Never nest the HAL state mutex with a backend mutex. A concurrent stop can
  // make this a harmless no-op; it cannot transfer ownership while the
  // recorder FSM is still entering CAPTURING.
  if (source == AUDIO_SRC_G2_LEFT) {
    return g2MicTrimAfeRingToNewest(keepNewestSamples);
  }
  return 0;
}

#endif // ENABLE_MICROPHONE
