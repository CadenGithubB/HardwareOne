/**
 * Microphone Sensor Module - ESP32-S3 PDM Microphone Implementation
 * 
 * Uses I2S peripheral to interface with PDM microphone on XIAO ESP32S3 Sense.
 */

#include "System_Microphone.h"

#if ENABLE_MICROPHONE_SENSOR

#include <Arduino.h>
#include <driver/i2s_pdm.h>
#include <LittleFS.h>
#include "System_MemUtil.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_Microphone_OLED.h"

// SSE status broadcast
extern void sensorStatusBumpWith(const char* reason);

// XIAO ESP32S3 Sense PDM Microphone Pins
#define MIC_PDM_CLK_PIN     42        // PDM CLK (GPIO42 on XIAO Sense)
#define MIC_PDM_DATA_PIN    41        // PDM DATA (GPIO41 on XIAO Sense)

// I2S PDM RX channel handle (new driver)
static i2s_chan_handle_t rx_handle = NULL;

// Default audio settings
#define DEFAULT_SAMPLE_RATE   16000
#define DEFAULT_BIT_DEPTH     16
#define DEFAULT_CHANNELS      1

// Buffer for audio capture
#define AUDIO_BUFFER_SIZE     1024
#define RECORDING_CHUNK_SIZE  4096
#define RECORDINGS_FOLDER     "/recordings"
#define MAX_RECORDING_SEC     60

// Microphone state
bool micEnabled = false;
bool micConnected = false;
bool micRecording = false;

// Microphone info
int micSampleRate = DEFAULT_SAMPLE_RATE;
int micBitDepth = DEFAULT_BIT_DEPTH;
int micChannels = DEFAULT_CHANNELS;
int micGain = 50;  // Software gain 0-100%

// Recording state
static TaskHandle_t recordingTaskHandle = nullptr;
static File recordingFile;
static uint32_t recordingStartTime = 0;
static uint32_t recordingSamples = 0;
static char currentRecordingPath[64] = {0};

// Command buffer
static char gMicCmdBuffer[512];

// Audio level tracking
static int lastAudioLevel = 0;
static uint32_t lastAudioLevelMs = 0;

// Audio preprocessing state (shared between mic and ESP-SR)
static int32_t gMicDcOffset = 0;
static bool gMicDcOffsetInitialized = false;
static float gMicBaseSoftwareGain = 24.0f;

// High-pass filter state (~50Hz cutoff at 16kHz sample rate)
// alpha = 1 / (1 + 2*pi*fc/fs) where fc=50Hz, fs=16000Hz
static const float kHighPassAlpha = 0.9806f;
static float gMicHighPassState = 0.0f;
static int16_t gMicHighPassPrevIn = 0;

// Pre-emphasis filter coefficient (boosts high frequencies for speech clarity)
static const float kPreEmphCoeff = 0.97f;
static int16_t gMicPreEmphPrevSample = 0;

float getMicSoftwareGainMultiplier() {
  if (micGain <= 0) return 0.0f;
  return gMicBaseSoftwareGain * ((float)micGain / 50.0f);
}

int32_t getMicDcOffset() {
  return gMicDcOffset;
}

void resetMicAudioProcessingState() {
  gMicDcOffset = 0;
  gMicDcOffsetInitialized = false;
  gMicHighPassState = 0.0f;
  gMicHighPassPrevIn = 0;
  gMicPreEmphPrevSample = 0;
}

void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier, bool filtersEnabled) {
  if (!buf || sampleCount == 0) return;

  // Use provided gain or calculate from micGain setting
  if (gainMultiplier <= 0.0f) {
    gainMultiplier = getMicSoftwareGainMultiplier();
  }

  // Calculate DC offset from this chunk (running average)
  int64_t sum = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    sum += buf[i];
  }
  int32_t chunkDc = (int32_t)(sum / (int64_t)sampleCount);

  // Slowly adapt DC offset estimate (EMA with alpha=0.1)
  if (!gMicDcOffsetInitialized) {
    gMicDcOffset = chunkDc;
    gMicDcOffsetInitialized = true;
  } else {
    gMicDcOffset = gMicDcOffset + (chunkDc - gMicDcOffset) / 10;
  }

  // Apply audio preprocessing pipeline:
  // 1. DC offset removal (always)
  // 2. High-pass filter (~50Hz cutoff) - optional
  // 3. Pre-emphasis filter (boost high frequencies) - optional
  // 4. Software gain (always)
  for (size_t i = 0; i < sampleCount; i++) {
    // Step 1: Remove DC offset
    float sample = (float)(buf[i] - gMicDcOffset);
    
    if (filtersEnabled) {
      // Step 2: High-pass filter (removes low-freq rumble/hum)
      // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
      float hpOut = kHighPassAlpha * (gMicHighPassState + sample - (float)gMicHighPassPrevIn);
      gMicHighPassState = hpOut;
      gMicHighPassPrevIn = (int16_t)sample;
      sample = hpOut;
      
      // Step 3: Pre-emphasis filter (boosts high frequencies for speech clarity)
      // y[n] = x[n] - alpha * x[n-1]
      float preEmphOut = sample - kPreEmphCoeff * (float)gMicPreEmphPrevSample;
      gMicPreEmphPrevSample = (int16_t)sample;
      sample = preEmphOut;
    }
    
    // Step 4: Apply software gain
    sample *= gainMultiplier;
    
    // Clamp to 16-bit range
    int32_t sampleInt = (int32_t)sample;
    if (sampleInt > 32767) sampleInt = 32767;
    if (sampleInt < -32768) sampleInt = -32768;
    buf[i] = (int16_t)sampleInt;
  }
}

// WAV header structure
struct WavHeader {
  char riff[4] = {'R','I','F','F'};
  uint32_t fileSize;
  char wave[4] = {'W','A','V','E'};
  char fmt[4] = {'f','m','t',' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;  // PCM
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4] = {'d','a','t','a'};
  uint32_t dataSize;
};

static void writeWavHeader(File& f, uint32_t dataSize) {
  WavHeader header;
  header.numChannels = micChannels;
  header.sampleRate = micSampleRate;
  header.bitsPerSample = micBitDepth;
  header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
  header.byteRate = header.sampleRate * header.blockAlign;
  header.dataSize = dataSize;
  header.fileSize = dataSize + sizeof(WavHeader) - 8;
  
  f.seek(0);
  f.write((uint8_t*)&header, sizeof(header));
}

static void recordingTask(void* param) {
  DEBUG_MICF("[MIC_REC_TASK] ========== recordingTask() ENTRY ==========");
  DEBUG_MICF("[MIC_REC_TASK] Task running on core %d", xPortGetCoreID());
  DEBUG_MICF("[MIC_REC_TASK] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_SENSORSF("[Microphone] Recording task started");
  
  DEBUG_MICF("[MIC_REC_TASK] Allocating %d byte recording buffer...", RECORDING_CHUNK_SIZE);
  int16_t* buffer = (int16_t*)ps_alloc(RECORDING_CHUNK_SIZE, AllocPref::PreferPSRAM, "mic.rec.buf");
  DEBUG_MICF("[MIC_REC_TASK] ps_alloc returned: %p", buffer);
  
  if (!buffer) {
    DEBUG_MICF("[MIC_REC_TASK] *** BUFFER ALLOCATION FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to allocate recording buffer");
    micRecording = false;
    recordingTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }
  
  uint32_t maxSamples = micSampleRate * MAX_RECORDING_SEC;
  DEBUG_MICF("[MIC_REC_TASK] Max samples: %lu (sampleRate=%d, maxSec=%d)", maxSamples, micSampleRate, MAX_RECORDING_SEC);
  
  uint32_t loopCount = 0;
  while (micRecording && micEnabled && recordingSamples < maxSamples) {
    size_t bytesRead = 0;
    esp_err_t err;
    {
      I2sMicLockGuard i2sGuard("mic.record.read");
      err = i2s_channel_read(rx_handle, buffer, RECORDING_CHUNK_SIZE, &bytesRead, pdMS_TO_TICKS(100));
    }
    
    if (err == ESP_OK && bytesRead > 0 && recordingFile) {
      {
        int32_t sum = 0;
        size_t sampleCount = bytesRead / sizeof(int16_t);

        applyMicAudioProcessing(buffer, sampleCount);

        for (size_t i = 0; i < sampleCount; i++) {
          int16_t v = buffer[i];
          sum += (v < 0) ? -v : v;
        }
        int32_t avg = (sampleCount > 0) ? (sum / (int32_t)sampleCount) : 0;
        int level = map(avg, 0, 16384, 0, 100);
        level = constrain(level, 0, 100);
        lastAudioLevel = level;
        lastAudioLevelMs = millis();
      }
      FsLockGuard fsGuard("mic.record.write");
      size_t written = recordingFile.write((uint8_t*)buffer, bytesRead);
      recordingSamples += bytesRead / sizeof(int16_t);
      
      // Log every 100 iterations
      if (loopCount % 100 == 0) {
        DEBUG_MICF("[MIC_REC_TASK] Loop %lu: read=%u, written=%u, totalSamples=%lu",
                   loopCount, bytesRead, written, recordingSamples);
      }
    } else if (err != ESP_OK) {
      DEBUG_MICF("[MIC_REC_TASK] i2s_channel_read error: 0x%x", err);
    } else if (bytesRead == 0) {
      // Log zero-byte reads periodically
      if (loopCount % 50 == 0) {
        DEBUG_MICF("[MIC_REC_TASK] Loop %lu: i2s_channel_read returned 0 bytes (no data from mic)", loopCount);
      }
    }
    
    loopCount++;
    // Don't add extra delay - i2s_channel_read already blocks for up to 100ms
    taskYIELD();
  }
  
  DEBUG_MICF("[MIC_REC_TASK] Recording loop ended: micRecording=%d micEnabled=%d samples=%lu",
             micRecording, micEnabled, recordingSamples);
  
  free(buffer);
  DEBUG_MICF("[MIC_REC_TASK] Buffer freed");
  
  // Finalize WAV file
  if (recordingFile) {
    uint32_t dataSize = recordingSamples * sizeof(int16_t);
    DEBUG_MICF("[MIC_REC_TASK] Finalizing WAV: dataSize=%lu", dataSize);
    {
      FsLockGuard fsGuard("mic.record.finalize");
      writeWavHeader(recordingFile, dataSize);
      recordingFile.close();
    }
    DEBUG_MICF("[MIC_REC_TASK] WAV file closed");
    INFO_SENSORSF("[Microphone] Recording saved: %s (%lu samples)", currentRecordingPath, recordingSamples);
  } else {
    DEBUG_MICF("[MIC_REC_TASK] WARNING: recordingFile is invalid!");
  }
  
  micRecording = false;
  recordingTaskHandle = nullptr;
  DEBUG_MICF("[MIC_REC_TASK] ========== recordingTask() EXIT ==========");
  vTaskDelete(NULL);
}

bool startRecording() {
  DEBUG_MICF("[MIC_START_REC] ========== startRecording() ENTRY ==========");
  DEBUG_MICF("[MIC_START_REC] micEnabled=%d micRecording=%d", micEnabled, micRecording);
  DEBUG_MICF("[MIC_START_REC] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  if (!micEnabled) {
    DEBUG_MICF("[MIC_START_REC] FAILED: mic not enabled");
    INFO_SENSORSF("[Microphone] Cannot record - mic not enabled");
    return false;
  }
  if (micRecording) {
    DEBUG_MICF("[MIC_START_REC] FAILED: already recording");
    INFO_SENSORSF("[Microphone] Already recording");
    return false;
  }
  
  // Ensure recordings folder exists
  DEBUG_MICF("[MIC_START_REC] Checking recordings folder: %s", RECORDINGS_FOLDER);
  {
    FsLockGuard fsGuard("mic.record.mkdir");
    if (!LittleFS.exists(RECORDINGS_FOLDER)) {
      DEBUG_MICF("[MIC_START_REC] Creating recordings folder...");
      bool created = LittleFS.mkdir(RECORDINGS_FOLDER);
      DEBUG_MICF("[MIC_START_REC] mkdir returned: %d", created);
    } else {
      DEBUG_MICF("[MIC_START_REC] Recordings folder exists");
    }
  }
  
  // Generate filename with timestamp
  snprintf(currentRecordingPath, sizeof(currentRecordingPath), 
           "%s/rec_%lu.wav", RECORDINGS_FOLDER, millis());
  DEBUG_MICF("[MIC_START_REC] Recording path: %s", currentRecordingPath);
  
  DEBUG_MICF("[MIC_START_REC] Opening file for write...");
  {
    FsLockGuard fsGuard("mic.record.open");
    recordingFile = LittleFS.open(currentRecordingPath, "w");
  }
  if (!recordingFile) {
    DEBUG_MICF("[MIC_START_REC] *** FAILED to create file! ***");
    INFO_SENSORSF("[Microphone] Failed to create recording file");
    return false;
  }
  DEBUG_MICF("[MIC_START_REC] File opened successfully");
  
  // Write placeholder header (will be updated at end)
  DEBUG_MICF("[MIC_START_REC] Writing placeholder WAV header...");
  {
    FsLockGuard fsGuard("mic.record.header");
    writeWavHeader(recordingFile, 0);
  }
  DEBUG_MICF("[MIC_START_REC] Header written, file position: %lu", recordingFile.position());
  
  recordingStartTime = millis();
  recordingSamples = 0;
  micRecording = true;
  sensorStatusBumpWith("micrecstart");
  
  // Start recording task
  DEBUG_MICF("[MIC_START_REC] Creating recording task (stack=4096, priority=5, core=1)...");
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    recordingTask,
    "mic_record",
    4096,
    nullptr,
    5,
    &recordingTaskHandle,
    1
  );
  DEBUG_MICF("[MIC_START_REC] xTaskCreatePinnedToCore returned: %d, handle=%p", taskCreated, recordingTaskHandle);
  
  if (taskCreated != pdPASS) {
    DEBUG_MICF("[MIC_START_REC] *** TASK CREATION FAILED! ***");
    micRecording = false;
    sensorStatusBumpWith("micrecstop");
    recordingFile.close();
    return false;
  }
  
  DEBUG_MICF("[MIC_START_REC] ========== startRecording() SUCCESS ==========");
  INFO_SENSORSF("[Microphone] Recording started: %s", currentRecordingPath);
  return true;
}

void stopRecording() {
  DEBUG_MICF("[MIC_STOP_REC] stopRecording() called, micRecording=%d", micRecording);
  if (!micRecording) {
    DEBUG_MICF("[MIC_STOP_REC] Not recording, returning");
    return;
  }
  
  DEBUG_MICF("[MIC_STOP_REC] Setting micRecording=false to signal task");
  micRecording = false;  // Signal task to stop
  sensorStatusBumpWith("micrecstop");
  
  // Wait for task to finish
  int timeout = 50;
  DEBUG_MICF("[MIC_STOP_REC] Waiting for task to finish (timeout=%d iterations)...", timeout);
  while (recordingTaskHandle && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  
  if (timeout <= 0 && recordingTaskHandle) {
    DEBUG_MICF("[MIC_STOP_REC] WARNING: Task did not finish within timeout!");
  } else {
    DEBUG_MICF("[MIC_STOP_REC] Task finished, remaining timeout=%d", timeout);
  }
  
  DEBUG_MICF("[MIC_STOP_REC] Recording stopped");
  INFO_SENSORSF("[Microphone] Recording stopped");
}

int getRecordingCount() {
  FsLockGuard fsGuard("mic.record.count");
  if (!LittleFS.exists(RECORDINGS_FOLDER)) return 0;
  
  int count = 0;
  File dir = LittleFS.open(RECORDINGS_FOLDER);
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory() && String(f.name()).endsWith(".wav")) {
        count++;
      }
      f = dir.openNextFile();
    }
  }
  return count;
}

String getRecordingsList() {
  String list = "";
  FsLockGuard fsGuard("mic.record.list");
  if (!LittleFS.exists(RECORDINGS_FOLDER)) return list;
  
  File dir = LittleFS.open(RECORDINGS_FOLDER);
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory() && String(f.name()).endsWith(".wav")) {
        if (list.length() > 0) list += ",";
        list += String(f.name()) + ":" + String(f.size());
      }
      f = dir.openNextFile();
    }
  }
  return list;
}

bool deleteRecording(const char* filename) {
  String path = String(RECORDINGS_FOLDER) + "/" + filename;
  FsLockGuard fsGuard("mic.record.delete");
  if (LittleFS.exists(path)) {
    return LittleFS.remove(path);
  }
  return false;
}

bool initMicrophone() {
  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_INIT] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  WARN_SYSTEMF("[MIC_INIT] Current state: micEnabled=%d, micConnected=%d", micEnabled, micConnected);

  gMicDcOffset = 0;
  gMicDcOffsetInitialized = false;

  I2sMicLockGuard i2sGuard("mic.init");
  
  if (micEnabled) {
    WARN_SYSTEMF("[MIC_INIT] Already initialized - returning true");
    INFO_SENSORSF("[Microphone] Already initialized");
    return true;
  }

  // Load settings from saved values
  WARN_SYSTEMF("[MIC_INIT] Loading settings from gSettings...");
  if (gSettings.microphoneSampleRate >= 8000 && gSettings.microphoneSampleRate <= 48000) {
    micSampleRate = gSettings.microphoneSampleRate;
  }
  if (gSettings.microphoneGain >= 0 && gSettings.microphoneGain <= 100) {
    micGain = gSettings.microphoneGain;
  }
  if (gSettings.microphoneBitDepth == 16 || gSettings.microphoneBitDepth == 32) {
    micBitDepth = gSettings.microphoneBitDepth;
  }

  WARN_SYSTEMF("[MIC_INIT] Audio settings: sampleRate=%d, bitDepth=%d, channels=%d, gain=%d%%",
               micSampleRate, micBitDepth, micChannels, micGain);
  WARN_SYSTEMF("[MIC_INIT] Pin config: CLK=%d, DATA=%d", MIC_PDM_CLK_PIN, MIC_PDM_DATA_PIN);
  INFO_SENSORSF("[Microphone] Initializing PDM microphone...");

  // Configure I2S channel for PDM RX (new driver API)
  WARN_SYSTEMF("[MIC_INIT] Creating I2S channel config...");
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = AUDIO_BUFFER_SIZE;
  
  WARN_SYSTEMF("[MIC_INIT] Channel config: i2s_num=0, dma_desc_num=%d, dma_frame_num=%d",
               (int)chan_cfg.dma_desc_num, (int)chan_cfg.dma_frame_num);
  
  WARN_SYSTEMF("[MIC_INIT] Calling i2s_new_channel()...");
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  WARN_SYSTEMF("[MIC_INIT] i2s_new_channel returned: 0x%x (%s), handle=%p", 
               err, esp_err_to_name(err), rx_handle);
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_INIT] *** I2S CHANNEL CREATE FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to create I2S channel: 0x%x", err);
    return false;
  }

  // Configure PDM RX mode
  WARN_SYSTEMF("[MIC_INIT] Configuring PDM RX mode...");
  i2s_pdm_rx_config_t pdm_rx_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)micSampleRate),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)MIC_PDM_CLK_PIN,
      .din = (gpio_num_t)MIC_PDM_DATA_PIN,
      .invert_flags = {
        .clk_inv = false,
      },
    },
  };
  
  WARN_SYSTEMF("[MIC_INIT] PDM clk_cfg: sample_rate_hz=%u, clk_src=%d, mclk_mult=%d, bclk_div=%u",
               (unsigned)pdm_rx_cfg.clk_cfg.sample_rate_hz,
               (int)pdm_rx_cfg.clk_cfg.clk_src,
               (int)pdm_rx_cfg.clk_cfg.mclk_multiple,
               (unsigned)pdm_rx_cfg.clk_cfg.bclk_div);
  WARN_SYSTEMF("[MIC_INIT] PDM gpio_cfg: clk=%d, din=%d, clk_inv=%d",
               (int)pdm_rx_cfg.gpio_cfg.clk, (int)pdm_rx_cfg.gpio_cfg.din,
               (int)pdm_rx_cfg.gpio_cfg.invert_flags.clk_inv);
  WARN_SYSTEMF("[MIC_INIT] PDM slot_cfg: data_bit_width=16, slot_mode=MONO");
  
  WARN_SYSTEMF("[MIC_INIT] Calling i2s_channel_init_pdm_rx_mode()...");
  err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
  WARN_SYSTEMF("[MIC_INIT] i2s_channel_init_pdm_rx_mode returned: 0x%x (%s)", err, esp_err_to_name(err));
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_INIT] *** PDM RX INIT FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to init PDM RX: 0x%x", err);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
    return false;
  }

  // Enable the channel
  WARN_SYSTEMF("[MIC_INIT] Calling i2s_channel_enable()...");
  err = i2s_channel_enable(rx_handle);
  WARN_SYSTEMF("[MIC_INIT] i2s_channel_enable returned: 0x%x (%s)", err, esp_err_to_name(err));
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_INIT] *** I2S CHANNEL ENABLE FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to enable I2S channel: 0x%x", err);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
    return false;
  }

  // Flush initial samples (PDM needs warm-up time)
  WARN_SYSTEMF("[MIC_INIT] Starting PDM warm-up flush (10 reads of 512 bytes)...");
  int16_t flushBuf[256];
  size_t bytesRead = 0;
  int flushCount = 0;
  int successCount = 0;
  for (int i = 0; i < 10; i++) {
    uint32_t readStart = millis();
    esp_err_t readErr = i2s_channel_read(rx_handle, flushBuf, sizeof(flushBuf), &bytesRead, pdMS_TO_TICKS(100));
    uint32_t readMs = millis() - readStart;
    flushCount++;
    if (readErr == ESP_OK && bytesRead > 0) {
      successCount++;
      if (i == 9) {
        int16_t mn = 32767, mx = -32768;
        for (size_t j = 0; j < bytesRead / 2; j++) {
          if (flushBuf[j] < mn) mn = flushBuf[j];
          if (flushBuf[j] > mx) mx = flushBuf[j];
        }
        WARN_SYSTEMF("[MIC_INIT] Flush[%d]: %u bytes in %u ms, min=%d, max=%d", i, (unsigned)bytesRead, readMs, mn, mx);
      }
    } else {
      WARN_SYSTEMF("[MIC_INIT] Flush[%d]: err=0x%x, bytes=%u, took %u ms", i, readErr, (unsigned)bytesRead, readMs);
    }
  }
  WARN_SYSTEMF("[MIC_INIT] Warm-up flush complete: %d/%d successful reads", successCount, flushCount);
  
  if (successCount == 0) {
    WARN_SYSTEMF("[MIC_INIT] WARNING: No data received from microphone during flush!");
    INFO_SENSORSF("[Microphone] WARNING: Microphone may not be connected or responding");
  }

  micEnabled = true;
  micConnected = (successCount > 0);  // Only mark connected if we got data
  sensorStatusBumpWith("openmic");

  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() SUCCESS ##########");
  WARN_SYSTEMF("[MIC_INIT] micEnabled=%d, micConnected=%d", micEnabled, micConnected);
  WARN_SYSTEMF("[MIC_INIT] Final heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_SENSORSF("[Microphone] Initialized: %dHz, %d-bit, %d channel(s)", 
                micSampleRate, micBitDepth, micChannels);
  return true;
}

void stopMicrophone() {
  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_STOP] Current state: micEnabled=%d, rx_handle=%p", micEnabled, rx_handle);

  I2sMicLockGuard i2sGuard("mic.stop");
  
  if (!micEnabled) {
    WARN_SYSTEMF("[MIC_STOP] Already stopped - returning");
    INFO_SENSORSF("[Microphone] Already stopped");
    return;
  }

  WARN_SYSTEMF("[MIC_STOP] Heap before stop: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  if (rx_handle) {
    WARN_SYSTEMF("[MIC_STOP] Calling i2s_channel_disable()...");
    esp_err_t err = i2s_channel_disable(rx_handle);
    WARN_SYSTEMF("[MIC_STOP] i2s_channel_disable returned: 0x%x (%s)", err, esp_err_to_name(err));
    
    WARN_SYSTEMF("[MIC_STOP] Calling i2s_del_channel()...");
    err = i2s_del_channel(rx_handle);
    WARN_SYSTEMF("[MIC_STOP] i2s_del_channel returned: 0x%x (%s)", err, esp_err_to_name(err));
    rx_handle = NULL;
  }

  micEnabled = false;
  micRecording = false;
  sensorStatusBumpWith("closemic");

  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() COMPLETE ##########");
  WARN_SYSTEMF("[MIC_STOP] Heap after stop: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_SENSORSF("[Microphone] Stopped");
}

int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen) {
  WARN_SYSTEMF("[MIC_CAPTURE] captureAudioSamples(count=%u) called", (unsigned)sampleCount);
  WARN_SYSTEMF("[MIC_CAPTURE] micEnabled=%d, rx_handle=%p", micEnabled, rx_handle);
  
  if (!micEnabled) {
    WARN_SYSTEMF("[MIC_CAPTURE] Mic not enabled - returning NULL");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  size_t bufferSize = sampleCount * sizeof(int16_t);
  WARN_SYSTEMF("[MIC_CAPTURE] Allocating %u bytes for %u samples...", (unsigned)bufferSize, (unsigned)sampleCount);
  WARN_SYSTEMF("[MIC_CAPTURE] Heap before alloc: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  int16_t* buffer = (int16_t*)ps_alloc(bufferSize, AllocPref::PreferPSRAM, "mic.samples");
  WARN_SYSTEMF("[MIC_CAPTURE] ps_alloc returned: %p", buffer);
  
  if (!buffer) {
    WARN_SYSTEMF("[MIC_CAPTURE] *** ALLOCATION FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to allocate %u bytes", bufferSize);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  WARN_SYSTEMF("[MIC_CAPTURE] Calling i2s_channel_read(handle=%p, bufSize=%u, timeout=MAX)...", rx_handle, (unsigned)bufferSize);
  unsigned long startMs = millis();
  size_t bytesRead = 0;
  esp_err_t err;
  {
    I2sMicLockGuard i2sGuard("mic.capture.read");
    err = i2s_channel_read(rx_handle, buffer, bufferSize, &bytesRead, portMAX_DELAY);
  }
  unsigned long elapsed = millis() - startMs;
  
  WARN_SYSTEMF("[MIC_CAPTURE] i2s_channel_read returned 0x%x (%s) in %lu ms, bytesRead=%u", 
               err, esp_err_to_name(err), elapsed, (unsigned)bytesRead);
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_CAPTURE] *** I2S READ FAILED! ***");
    INFO_SENSORSF("[Microphone] Failed to read samples: 0x%x", err);
    free(buffer);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  applyMicAudioProcessing(buffer, bytesRead / sizeof(int16_t));

  // Log sample statistics
  if (bytesRead >= 4) {
    int16_t minVal = buffer[0], maxVal = buffer[0];
    int64_t sumAbs = 0;
    size_t numSamples = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < numSamples; i++) {
      if (buffer[i] < minVal) minVal = buffer[i];
      if (buffer[i] > maxVal) maxVal = buffer[i];
      sumAbs += (buffer[i] < 0) ? -buffer[i] : buffer[i];
    }
    float avgAbs = (float)sumAbs / (float)numSamples;
    WARN_SYSTEMF("[MIC_CAPTURE] Sample stats: min=%d, max=%d, range=%d, avg_abs=%.1f", 
                 minVal, maxVal, maxVal - minVal, avgAbs);
  }

  if (outLen) *outLen = bytesRead;
  WARN_SYSTEMF("[MIC_CAPTURE] Returning buffer=%p, len=%u", buffer, (unsigned)bytesRead);
  return buffer;
}

int getAudioLevel() {
  static uint32_t callCount = 0;
  callCount++;
  
  // Only log every 50th call to avoid spam
  bool shouldLog = (callCount % 50 == 1);
  
  if (shouldLog) {
    DEBUG_MICF("[MIC_LEVEL] getAudioLevel() call #%lu, micEnabled=%d", callCount, micEnabled);
  }
  
  if (!micEnabled) {
    if (shouldLog) DEBUG_MICF("[MIC_LEVEL] Mic not enabled - returning 0");
    return 0;
  }

  uint32_t now = millis();
  if (micRecording) {
    return lastAudioLevel;
  }
  if (lastAudioLevelMs != 0 && (now - lastAudioLevelMs) < 150) {
    return lastAudioLevel;
  }

  // Read a small sample to calculate level
  int16_t samples[256];
  size_t bytesRead = 0;

  bool took = false;
  if (i2sMicMutex && xSemaphoreTake(i2sMicMutex, 0) == pdTRUE) {
    took = true;
  } else {
    if (shouldLog) {
      DEBUG_MICF("[MIC_LEVEL] i2sMicMutex busy; returning cached last=%d", lastAudioLevel);
    }
    return lastAudioLevel;
  }

  esp_err_t err = i2s_channel_read(rx_handle, samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(50));

  if (took && i2sMicMutex) {
    xSemaphoreGive(i2sMicMutex);
  }
  
  if (err != ESP_OK || bytesRead == 0) {
    if (shouldLog) {
      DEBUG_MICF("[MIC_LEVEL] i2s_channel_read failed or no data: err=0x%x bytesRead=%u, returning last=%d",
                 err, bytesRead, lastAudioLevel);
    }
    return lastAudioLevel;
  }

  size_t sampleCount = bytesRead / sizeof(int16_t);
  applyMicAudioProcessing(samples, sampleCount);

  // Calculate RMS level
  int32_t sum = 0;
  int16_t minVal = samples[0], maxVal = samples[0];
  
  for (size_t i = 0; i < sampleCount; i++) {
    sum += abs(samples[i]);
    if (samples[i] < minVal) minVal = samples[i];
    if (samples[i] > maxVal) maxVal = samples[i];
  }
  int32_t avg = sum / sampleCount;
  
  // Map to 0-100 range
  int level = map(avg, 0, 16384, 0, 100);
  level = constrain(level, 0, 100);
  
  if (shouldLog) {
    DEBUG_MICF("[MIC_LEVEL] samples=%u avg=%ld min=%d max=%d level=%d%%",
               sampleCount, avg, minVal, maxVal, level);
  }
  
  lastAudioLevel = level;
  lastAudioLevelMs = now;
  return level;
}

const char* buildMicrophoneStatusJson() {
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "{\"enabled\":%s,\"connected\":%s,\"recording\":%s,"
    "\"sampleRate\":%d,\"bitDepth\":%d,\"channels\":%d,\"level\":%d}",
    micEnabled ? "true" : "false",
    micConnected ? "true" : "false",
    micRecording ? "true" : "false",
    micSampleRate, micBitDepth, micChannels,
    micEnabled ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_mic(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "Microphone Status:\n"
    "  Enabled: %s\n"
    "  Connected: %s\n"
    "  Recording: %s\n"
    "  Sample Rate: %d Hz\n"
    "  Bit Depth: %d\n"
    "  Channels: %d\n"
    "  Level: %d%%",
    micEnabled ? "yes" : "no",
    micConnected ? "yes" : "no",
    micRecording ? "yes" : "no",
    micSampleRate, micBitDepth, micChannels,
    micEnabled ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

const char* cmd_micstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (initMicrophone()) {
    return "Microphone started successfully";
  }
  return "Failed to start microphone";
}

const char* cmd_micstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  stopMicrophone();
  return "Microphone stopped";
}

const char* cmd_miclevel(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!micEnabled) {
    return "Microphone not enabled";
  }
  int level = getAudioLevel();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Audio level: %d%%", level);
  return gMicCmdBuffer;
}

const char* cmd_micrecord(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!micEnabled) {
    return "Microphone not enabled. Use 'openmic' first.";
  }

  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    if (micRecording) {
      uint32_t elapsed = (millis() - recordingStartTime) / 1000;
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: active (%lus, %lu samples)", 
        elapsed, recordingSamples);
    } else {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: stopped");
    }
    return gMicCmdBuffer;
  }

  if (arg == "1" || arg.equalsIgnoreCase("start")) {
    if (startRecording()) {
      return "Recording started";
    } else {
      return "Failed to start recording";
    }
  } else if (arg == "0" || arg.equalsIgnoreCase("stop")) {
    stopRecording();
    return "Recording stopped";
  }
  
  return "Usage: micrecord <start|stop|1|0>";
}

const char* cmd_miclist(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  int count = getRecordingCount();
  if (count == 0) {
    return "No recordings found";
  }
  
  String list = getRecordingsList();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recordings (%d):\n%s", count, list.c_str());
  return gMicCmdBuffer;
}

const char* cmd_micdelete(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    return "Usage: micdelete <filename> or micdelete all";
  }
  
  if (arg.equalsIgnoreCase("all")) {
    if (!LittleFS.exists(RECORDINGS_FOLDER)) {
      return "No recordings folder";
    }
    int deleted = 0;
    File dir = LittleFS.open(RECORDINGS_FOLDER);
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        String name = f.name();
        f.close();
        if (name.endsWith(".wav")) {
          String path = String(RECORDINGS_FOLDER) + "/" + name;
          if (LittleFS.remove(path)) deleted++;
        }
        f = dir.openNextFile();
      }
    }
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted %d recording(s)", deleted);
    return gMicCmdBuffer;
  }
  
  if (deleteRecording(arg.c_str())) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted: %s", arg.c_str());
    return gMicCmdBuffer;
  }
  return "File not found";
}

const char* cmd_micsamplerate(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate: %d Hz", micSampleRate);
    return gMicCmdBuffer;
  }

  int rate = arg.toInt();
  if (rate < 8000 || rate > 48000) {
    return "Sample rate must be 8000-48000 Hz";
  }

  // Need to reinitialize if already running
  bool wasEnabled = micEnabled;
  if (wasEnabled) {
    stopMicrophone();
  }
  
  micSampleRate = rate;
  gSettings.microphoneSampleRate = rate;
  
  if (wasEnabled) {
    initMicrophone();
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate set to %d Hz (saved)", micSampleRate);
  return gMicCmdBuffer;
}

const char* cmd_micgain(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain: %d%%", micGain);
    return gMicCmdBuffer;
  }

  int gain = arg.toInt();
  if (gain < 0 || gain > 100) {
    return "Gain must be 0-100%";
  }
  
  micGain = gain;
  gSettings.microphoneGain = gain;
  
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain set to %d%% (saved)", micGain);
  return gMicCmdBuffer;
}

const char* cmd_micbitdepth(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Bit depth: %d-bit", micBitDepth);
    return gMicCmdBuffer;
  }

  int depth = arg.toInt();
  if (depth != 16 && depth != 32) {
    return "Bit depth must be 16 or 32";
  }

  // Need to reinitialize if already running
  bool wasEnabled = micEnabled;
  if (wasEnabled) {
    stopMicrophone();
  }
  
  micBitDepth = depth;
  gSettings.microphoneBitDepth = depth;
  
  if (wasEnabled) {
    initMicrophone();
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Bit depth set to %d-bit (saved)", micBitDepth);
  return gMicCmdBuffer;
}

// Real-time audio visualizer state
static volatile bool gMicVisualizerRunning = false;
static TaskHandle_t gMicVisualizerTask = nullptr;

static void micVisualizerTaskFunc(void* param) {
  const size_t bufSize = 512;
  int16_t* samples = (int16_t*)heap_caps_malloc(bufSize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!samples) {
    gMicVisualizerRunning = false;
    gMicVisualizerTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  
  Serial.println("\n=== AUDIO VISUALIZER (press any key to stop) ===");
  Serial.println("Level: [--------------------] Peak | Min/Max samples");
  
  while (gMicVisualizerRunning && micEnabled) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rx_handle, samples, bufSize * sizeof(int16_t), &bytesRead, pdMS_TO_TICKS(100));
    
    if (err == ESP_OK && bytesRead > 0) {
      size_t sampleCount = bytesRead / sizeof(int16_t);
      applyMicAudioProcessing(samples, sampleCount);
      
      // Calculate stats
      int16_t minVal = 32767, maxVal = -32768;
      int64_t sumAbs = 0;
      for (size_t i = 0; i < sampleCount; i++) {
        if (samples[i] < minVal) minVal = samples[i];
        if (samples[i] > maxVal) maxVal = samples[i];
        sumAbs += abs(samples[i]);
      }
      int avgAbs = (int)(sumAbs / sampleCount);
      
      // Map to 0-100 scale (32767 = max amplitude)
      int level = (avgAbs * 100) / 32767;
      if (level > 100) level = 100;
      
      // Create ASCII bar (40 chars wide)
      char bar[45];
      int barLen = (level * 40) / 100;
      for (int i = 0; i < 40; i++) {
        if (i < barLen) {
          if (i < 20) bar[i] = '=';
          else if (i < 32) bar[i] = '#';
          else bar[i] = '!';  // Clipping warning
        } else {
          bar[i] = '-';
        }
      }
      bar[40] = '\0';
      
      // Print with carriage return to overwrite line
      Serial.printf("\r[%s] %3d%% | %6d / %6d", bar, level, minVal, maxVal);
    }
    
    // Check for key press to stop
    if (Serial.available()) {
      while (Serial.available()) Serial.read();
      break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // ~20 updates/sec
  }
  
  Serial.println("\n=== VISUALIZER STOPPED ===");
  heap_caps_free(samples);
  gMicVisualizerRunning = false;
  gMicVisualizerTask = nullptr;
  vTaskDelete(nullptr);
}

const char* cmd_micviz(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!micEnabled) {
    return "Microphone not enabled. Use 'openmic' first.";
  }
  
  if (gMicVisualizerRunning) {
    gMicVisualizerRunning = false;
    return "Stopping visualizer...";
  }
  
  gMicVisualizerRunning = true;
  xTaskCreatePinnedToCore(micVisualizerTaskFunc, "mic_viz", 4096, nullptr, 3, &gMicVisualizerTask, 0);
  return "Visualizer started (press any key to stop)";
}

// Command registry
const CommandEntry micCommands[] = {
  { "mic", "Microphone sensor status and control.", false, cmd_mic, "Usage: mic" },
  { "openmic", "Start microphone sensor.", false, cmd_micstart, nullptr, "microphone", "open" },
  { "closemic", "Stop microphone sensor.", false, cmd_micstop, nullptr, "microphone", "close" },
  { "miclevel", "Get current audio level.", false, cmd_miclevel, "Usage: miclevel" },
  { "micviz", "Real-time audio level visualizer.", false, cmd_micviz, "Usage: micviz (press any key to stop)" },
  { "micrecord", "Start/stop recording to WAV file.", false, cmd_micrecord, "Usage: micrecord <start|stop>" },
  { "miclist", "List saved recordings.", false, cmd_miclist, "Usage: miclist" },
  { "micdelete", "Delete recording(s).", false, cmd_micdelete, "Usage: micdelete <filename|all>" },
  { "micsamplerate", "Get/set sample rate.", false, cmd_micsamplerate, "Usage: micsamplerate [8000-48000]" },
  { "micgain", "Get/set microphone gain.", false, cmd_micgain, "Usage: micgain [0-100]" },
  { "micbitdepth", "Get/set bit depth.", false, cmd_micbitdepth, "Usage: micbitdepth [16|32]" },
};

const size_t micCommandsCount = sizeof(micCommands) / sizeof(micCommands[0]);

// Settings module registration
static const SettingEntry micSettingEntries[] = {
  { "microphoneAutoStart", SETTING_BOOL, &gSettings.microphoneAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
};

static bool isMicConnected() {
  if (!micEnabled) return true;
  return micConnected;
}

extern const SettingsModule micSettingsModule = {
  "microphone",
  "microphone",
  micSettingEntries,
  sizeof(micSettingEntries) / sizeof(micSettingEntries[0]),
  isMicConnected,
  "ESP32-S3 PDM microphone"
};

// Auto-register with command system
static CommandModuleRegistrar _mic_cmd_registrar(micCommands, micCommandsCount, "microphone");

#endif // ENABLE_MICROPHONE_SENSOR
