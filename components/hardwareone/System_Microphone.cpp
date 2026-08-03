/**
 * Microphone Sensor Module - ESP32-S3 PDM Microphone Implementation
 * 
 * Uses I2S peripheral to interface with PDM microphone on XIAO ESP32S3 Sense.
 */

#include "System_Microphone.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Filesystem.h"  // requireQuotedPath (uniform quoted-path rule)
#include <esp_attr.h>

// Gate on the mic SUBSYSTEM (PDM silicon OR a G2-capable build), not on the PDM
// silicon flag — this module is source-agnostic (all capture flows through
// HAL_Audio) so it must compile and run on PDM-less boards where the G2 glasses
// mic is the only source. See System_SensorStubs (gated on !ENABLE_MICROPHONE in
// lockstep, or these definitions collide).
#if ENABLE_MICROPHONE

#include <Arduino.h>
#include "System_VFS.h"
#include "System_MemUtil.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_Command.h"
#include "System_Utils.h"   // argWantsJson
#include <ArduinoJson.h>
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_I2C.h"
#include "System_Microphone_OLED.h"
#include "System_AuthIdentity.h"  // currentAuthContext (recording path checks)
#include "HAL_Audio.h"            // single PDM/I2S capture owner (audioCaptureStart/audioReadPcm)

// XIAO ESP32S3 Sense PDM Microphone Pins
#define MIC_PDM_CLK_PIN     42        // PDM CLK (GPIO42 on XIAO Sense)
#define MIC_PDM_DATA_PIN    41        // PDM DATA (GPIO41 on XIAO Sense)

// PDM I2S capture is owned by HAL_Audio — no local channel handle here.

// Default audio settings
#define DEFAULT_SAMPLE_RATE   16000
#define DEFAULT_BIT_DEPTH     16
#define DEFAULT_CHANNELS      1

// Buffer for audio capture
#define AUDIO_BUFFER_SIZE     1024
#define RECORDING_CHUNK_SIZE  4096
#define MAX_RECORDING_SEC     60

// Defaults: new recordings go to SD when writable; legacy/internal copies may remain on LittleFS.
static const char kMicRecLittleFS[] = "/recordings";
static const char kMicRecSD[]       = "/sd/recordings";

static String micPrimaryRecordingsFolder() {
  if (VFS::isSDWritable()) {
    return String(kMicRecSD);
  }
  return String(kMicRecLittleFS);
}

// Microphone state
bool gMicRunning = false;
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
EXT_RAM_BSS_ATTR static char gMicCmdBuffer[512];

// ── Source helpers (unified PDM / G2 mic) ────────────────────────────────────
// HAL_Audio delivers int16 mono for every source. PDM runs at micSampleRate; the
// G2 ring is always 16 kHz. Use this for the WAV header + duration math so a G2
// recording is never mis-stamped.
static uint32_t micEffectiveSampleRate() {
  return (audioGetSource() == AUDIO_SRC_G2_LEFT) ? AUDIO_HAL_SAMPLE_RATE
                                                 : (uint32_t)micSampleRate;
}
static const char* micSourceName() {
  switch (audioGetSource()) {
    case AUDIO_SRC_LOCAL_PDM: return "pdm";
    case AUDIO_SRC_G2_LEFT:   return "g2";
    default:                  return "none";
  }
}
// The DSP chain (DC removal, HPF, pre-emphasis, gain) is tuned for the PDM mic;
// the G2 feed is already clean decoded LC3 PCM and the SR path consumes it raw,
// so skip processing for G2 to keep the level meter + recordings consistent with
// what SR hears.
static void micProcessForSource(int16_t* buf, size_t n) {
  if (audioGetSource() == AUDIO_SRC_LOCAL_PDM) applyMicAudioProcessing(buf, n);
}
// Reconcile this module's cached flags with the HAL. If the underlying capture
// vanished (e.g. the G2 dropped mid-session and onDisconnect released the lease),
// flip enabled/recording off so the UI stops claiming a live mic. micConnected
// tracks "capture active AND active source still available".
static void micReconcileState() {
  const bool active = audioCaptureActive() &&
                      audioSourceAvailable(audioGetSource());
  if (gMicRunning && !active) {
    gMicRunning  = false;
    micRecording = false;
  }
  micConnected = active;
}

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
  header.sampleRate = micEffectiveSampleRate();   // G2 is always 16 kHz; PDM uses micSampleRate
  header.bitsPerSample = 16;   // HAL delivers int16 for every source — micBitDepth is cosmetic; a 32 here corrupts the WAV
  header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
  header.byteRate = header.sampleRate * header.blockAlign;
  header.dataSize = dataSize;
  header.fileSize = dataSize + sizeof(WavHeader) - 8;
  
  f.seek(0);
  f.write((uint8_t*)&header, sizeof(header));
}

static void recordingTask(void* param) {
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ========== recordingTask() ENTRY ==========");
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Task running on core %d", xPortGetCoreID());
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_MIC_LIFECYCLEF("Recording task started");
  
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Allocating %d byte recording buffer...", RECORDING_CHUNK_SIZE);
  int16_t* buffer = (int16_t*)ps_alloc(RECORDING_CHUNK_SIZE, AllocPref::PreferPSRAM, "mic.rec.buf");
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ps_alloc returned: %p", buffer);

  if (!buffer) {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] *** BUFFER ALLOCATION FAILED! ***");
    INFO_MIC_LIFECYCLEF("Failed to allocate recording buffer");
    micRecording = false;
    recordingTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }
  
  uint32_t maxSamples = micEffectiveSampleRate() * MAX_RECORDING_SEC;
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Max samples: %lu (sampleRate=%u, maxSec=%d)", maxSamples, (unsigned)micEffectiveSampleRate(), MAX_RECORDING_SEC);
  
  uint32_t loopCount = 0;
  while (micRecording && gMicRunning && recordingSamples < maxSamples) {
    size_t bytesRead = 0;
    esp_err_t err = ESP_OK;
    {
      // Read PCM via HAL_Audio (single I2S owner); errors fold into 0 samples.
      size_t got = audioReadPcm(buffer, RECORDING_CHUNK_SIZE / sizeof(int16_t), 100);
      bytesRead = got * sizeof(int16_t);
    }
    
    if (err == ESP_OK && bytesRead > 0 && recordingFile) {
      {
        int32_t sum = 0;
        size_t sampleCount = bytesRead / sizeof(int16_t);

        micProcessForSource(buffer, sampleCount);

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
        DEBUG_MIC_POLLINGF("[MIC_REC_TASK] Loop %lu: read=%u, written=%u, totalSamples=%lu",
                   loopCount, bytesRead, written, recordingSamples);
      }
    } else if (err != ESP_OK) {
      DEBUG_MIC_POLLINGF("[MIC_REC_TASK] i2s_channel_read error: 0x%x", err);
    } else if (bytesRead == 0) {
      // Log zero-byte reads periodically
      if (loopCount % 50 == 0) {
        DEBUG_MIC_POLLINGF("[MIC_REC_TASK] Loop %lu: i2s_channel_read returned 0 bytes (no data from mic)", loopCount);
      }
    }
    
    loopCount++;
    // Don't add extra delay - i2s_channel_read already blocks for up to 100ms
    taskYIELD();
  }
  
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Recording loop ended: micRecording=%d gMicRunning=%d samples=%lu",
             micRecording, gMicRunning, recordingSamples);
  
  free(buffer);
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Buffer freed");
  
  // Finalize WAV file
  if (recordingFile) {
    uint32_t dataSize = recordingSamples * sizeof(int16_t);
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Finalizing WAV: dataSize=%lu", dataSize);
    {
      FsLockGuard fsGuard("mic.record.finalize");
      writeWavHeader(recordingFile, dataSize);
      recordingFile.close();
    }
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] WAV file closed");
    INFO_MIC_LIFECYCLEF("Recording saved: %s (%lu samples)", currentRecordingPath, recordingSamples);
    {
      const char* slash = strrchr(currentRecordingPath, '/');
      systemEventPost(SYSEVT_MIC_SAVED, slash ? slash + 1 : currentRecordingPath);
    }
  } else {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] WARNING: recordingFile is invalid!");
  }
  
  micRecording = false;
  recordingTaskHandle = nullptr;
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ========== recordingTask() EXIT ==========");
  vTaskDelete(NULL);
}

bool startRecording() {
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] ========== startRecording() ENTRY ==========");
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] gMicRunning=%d micRecording=%d", gMicRunning, micRecording);
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!gMicRunning) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] FAILED: mic not enabled");
    INFO_MIC_LIFECYCLEF("Cannot record - mic not enabled");
    return false;
  }
  if (micRecording) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] FAILED: already recording");
    INFO_MIC_LIFECYCLEF("Already recording");
    return false;
  }
  
  String recDir = micPrimaryRecordingsFolder();
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Checking recordings folder: %s", recDir.c_str());
  {
    FsLockGuard fsGuard("mic.record.mkdir");
    if (!VFS::existsGuarded(recDir, currentAuthContext())) {
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Creating recordings folder...");
      bool created = VFS::mkdirGuarded(recDir, currentAuthContext());
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] mkdir returned: %d", created);
    } else {
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Recordings folder exists");
    }
  }
  
  // Generate filename with timestamp
  snprintf(currentRecordingPath, sizeof(currentRecordingPath),
           "%s/rec_%lu.wav", recDir.c_str(), (unsigned long)millis());
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Recording path: %s", currentRecordingPath);
  
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Opening file for write...");
  {
    FsLockGuard fsGuard("mic.record.open");
    recordingFile = VFS::openGuarded(String(currentRecordingPath), "w", currentAuthContext(), true);
  }
  if (!recordingFile) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] *** FAILED to create file! ***");
    INFO_MIC_LIFECYCLEF("Failed to create recording file");
    return false;
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] File opened successfully");
  
  // Write placeholder header (will be updated at end)
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Writing placeholder WAV header...");
  {
    FsLockGuard fsGuard("mic.record.header");
    writeWavHeader(recordingFile, 0);
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Header written, file position: %lu", recordingFile.position());
  
  recordingStartTime = millis();
  recordingSamples = 0;
  micRecording = true;
  sensorStatusBumpWith("micrecstart");
  
  // Start recording task
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    recordingTask,
    "mic_record",
    MIC_RECORD_STACK_WORDS,
    nullptr,
    TASK_PRIORITY_HIGH,
    &recordingTaskHandle,
    1
  );
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] xTaskCreatePinnedToCore returned: %d, handle=%p", taskCreated, recordingTaskHandle);
  
  if (taskCreated != pdPASS) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] *** TASK CREATION FAILED! ***");
    micRecording = false;
    sensorStatusBumpWith("micrecstop");
    {
      FsLockGuard fsGuard("mic.record.task_create_cleanup");
      recordingFile.close();
    }
    return false;
  }
  
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] ========== startRecording() SUCCESS ==========");
  INFO_MIC_LIFECYCLEF("Recording started: %s", currentRecordingPath);
  {
    const char* slash = strrchr(currentRecordingPath, '/');
    systemEventPost(SYSEVT_MIC_RECORD_STARTED, slash ? slash + 1 : currentRecordingPath);
  }
  return true;
}

void stopRecording() {
  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] stopRecording() called, micRecording=%d", micRecording);
  if (!micRecording) {
    DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Not recording, returning");
    return;
  }
  
  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Setting micRecording=false to signal task");
  micRecording = false;  // Signal task to stop
  sensorStatusBumpWith("micrecstop");
  
  // Wait for task to finish
  int timeout = 50;
  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Waiting for task to finish (timeout=%d iterations)...", timeout);
  while (recordingTaskHandle && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  
  if (timeout <= 0 && recordingTaskHandle) {
    DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] WARNING: Task did not finish within timeout!");
  } else {
    DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Task finished, remaining timeout=%d", timeout);
  }
  
  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Recording stopped");
  INFO_MIC_LIFECYCLEF("Recording stopped");
}

int getRecordingCount() {
  FsLockGuard fsGuard("mic.record.count");
  int count = 0;
  String seen = ",";
  auto walk = [&](const String& folder) {
    if (!VFS::existsGuarded(folder, currentAuthContext())) return;
    File dir = VFS::openGuarded(folder, "r", currentAuthContext());
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String name = f.name();
      if (!name.endsWith(".wav")) continue;
      String token = "," + name + ",";
      if (seen.indexOf(token) >= 0) continue;
      seen += name + ",";
      count++;
    }
  };
  walk(String(kMicRecSD));
  walk(String(kMicRecLittleFS));
  return count;
}

String getRecordingsList() {
  String list = "";
  FsLockGuard fsGuard("mic.record.list");
  String seen = ",";
  auto walk = [&](const String& folder) {
    if (!VFS::existsGuarded(folder, currentAuthContext())) return;
    File dir = VFS::openGuarded(folder, "r", currentAuthContext());
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String name = f.name();
      if (!name.endsWith(".wav")) continue;
      String token = "," + name + ",";
      if (seen.indexOf(token) >= 0) continue;
      seen += name + ",";
      if (list.length() > 0) list += ",";
      char entryBuf[80];
      snprintf(entryBuf, sizeof(entryBuf), "%s:%d", name.c_str(), (int)f.size());
      list += entryBuf;
    }
  };
  walk(String(kMicRecSD));
  walk(String(kMicRecLittleFS));
  return list;
}

bool deleteRecording(const char* filename) {
  String sdPath = String(kMicRecSD) + "/" + filename;
  String lfPath = String(kMicRecLittleFS) + "/" + filename;
  FsLockGuard fsGuard("mic.record.delete");
  if (VFS::existsGuarded(sdPath, currentAuthContext())) return VFS::removeGuarded(sdPath, currentAuthContext());
  if (VFS::existsGuarded(lfPath, currentAuthContext())) return VFS::removeGuarded(lfPath, currentAuthContext());
  return false;
}

bool initMicrophone() {
  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_INIT] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  WARN_SYSTEMF("[MIC_INIT] Current state: gMicRunning=%d, micConnected=%d", gMicRunning, micConnected);

  gMicDcOffset = 0;
  gMicDcOffsetInitialized = false;

  I2sMicLockGuard i2sGuard("mic.init");
  
  if (gMicRunning) {
    WARN_SYSTEMF("[MIC_INIT] Already initialized - returning true");
    INFO_MIC_LIFECYCLEF("Already initialized");
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
  INFO_MIC_LIFECYCLEF("Initializing PDM microphone...");
  STACK_TRACEF("initMicrophone.enter rate=%d bitDepth=%d channels=%d",
               micSampleRate, micBitDepth, micChannels);

  // Resolve the persisted source PREFERENCE against what is actually connected —
  // never assume a source exists. 'auto' (or an unavailable preference) falls
  // through to audioCaptureStart's PDM-first resolution.
  if (!audioAnySourceAvailable()) {
    WARN_SYSTEMF("[MIC_INIT] no mic source available (no PDM, no G2 connected) — cannot start");
    INFO_MIC_LIFECYCLEF("No mic source available");
    return false;
  }
  if (gSettings.micSource == "pdm" && audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) {
    audioSetSource(AUDIO_SRC_LOCAL_PDM);
  } else if (gSettings.micSource == "g2" && audioSourceAvailable(AUDIO_SRC_G2_LEFT)) {
    audioSetSource(AUDIO_SRC_G2_LEFT);
  }

  // Capture is owned by HAL_Audio (the single owner). audioCaptureStart resolves
  // the source (PDM-first if the selection is unavailable), starts the PDM I2S
  // channel + warm-up flush OR arms the G2 ring and enables the glasses stream.
  if (!audioCaptureStart("mic", (uint32_t)micSampleRate)) {
    WARN_SYSTEMF("[MIC_INIT] *** audioCaptureStart(\"mic\") FAILED at rate=%d Hz ***", micSampleRate);
    INFO_MIC_LIFECYCLEF("Failed to start PDM capture");
    return false;
  }
  gMicRunning = true;
  micReconcileState();  // set micConnected from live HAL state (source + capture)
  WARN_SYSTEMF("[MIC_INIT] source=%s", micSourceName());
  sensorStatusBumpWith("openmic");
  systemEventPost(SYSEVT_SENSOR_STARTED, "Microphone");

  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() SUCCESS ##########");
  WARN_SYSTEMF("[MIC_INIT] gMicRunning=%d, micConnected=%d", gMicRunning, micConnected);
  WARN_SYSTEMF("[MIC_INIT] Final heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_MIC_LIFECYCLEF("Initialized: %dHz, %d-bit, %d channel(s)", 
                micSampleRate, micBitDepth, micChannels);
  return true;
}

void stopMicrophone() {
  STACK_TRACEF("stopMicrophone.enter gMicRunning=%d micRecording=%d",
               gMicRunning, micRecording);
  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_STOP] Current state: gMicRunning=%d", gMicRunning);

  STACK_TRACEF("stopMicrophone.before_mutex");
  I2sMicLockGuard i2sGuard("mic.stop");
  STACK_TRACEF("stopMicrophone.got_mutex");

  if (!gMicRunning) {
    WARN_SYSTEMF("[MIC_STOP] Already stopped - returning");
    INFO_MIC_LIFECYCLEF("Already stopped");
    STACK_TRACEF("stopMicrophone.exit_already_stopped");
    return;
  }

  // Clear gMicRunning BEFORE the I2S teardown so any concurrent caller that
  // only does a null-check-on-rx_handle (without taking the mutex) will see
  // us going down.
  gMicRunning = false;
  STACK_TRACEF("stopMicrophone.cleared_gMicEnabled");

  WARN_SYSTEMF("[MIC_STOP] Heap before stop: free=%u, PSRAM_free=%u",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // HAL_Audio owns the I2S channel — release our capture lease.
  audioCaptureStop("mic");

  gMicRunning = false;
  micRecording = false;
  sensorStatusBumpWith("closemic");
  systemEventPost(SYSEVT_SENSOR_STOPPED, "Microphone");

  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() COMPLETE ##########");
  WARN_SYSTEMF("[MIC_STOP] Heap after stop: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_MIC_LIFECYCLEF("Stopped");
}

int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen) {
  WARN_SYSTEMF("[MIC_CAPTURE] captureAudioSamples(count=%u) called", (unsigned)sampleCount);
  WARN_SYSTEMF("[MIC_CAPTURE] gMicRunning=%d", gMicRunning);
  
  if (!gMicRunning) {
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
    INFO_MIC_POLLINGF("Failed to allocate %u bytes", bufferSize);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  WARN_SYSTEMF("[MIC_CAPTURE] Reading %u bytes via HAL_Audio...", (unsigned)bufferSize);
  unsigned long startMs = millis();
  size_t bytesRead = 0;
  esp_err_t err = ESP_OK;
  {
    // Read PCM via HAL_Audio (single I2S owner). A generous finite timeout
    // replaces the old portMAX_DELAY block; at 16 kHz a full read fills in ms.
    size_t got = audioReadPcm(buffer, bufferSize / sizeof(int16_t), 5000);
    bytesRead = got * sizeof(int16_t);
    if (got == 0) err = ESP_FAIL;
  }
  unsigned long elapsed = millis() - startMs;
  
  WARN_SYSTEMF("[MIC_CAPTURE] i2s_channel_read returned 0x%x (%s) in %lu ms, bytesRead=%u", 
               err, esp_err_to_name(err), elapsed, (unsigned)bytesRead);
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_CAPTURE] *** I2S READ FAILED! ***");
    INFO_MIC_POLLINGF("Failed to read samples: 0x%x", err);
    free(buffer);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  micProcessForSource(buffer, bytesRead / sizeof(int16_t));

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
    DEBUG_MIC_VALUESF("[MIC_LEVEL] getAudioLevel() call #%lu, gMicRunning=%d", callCount, gMicRunning);
  }

  // The VU meter is the most-frequent caller — piggyback the HAL reconcile here
  // so a G2 mid-session disconnect flips the mic off within one poll.
  micReconcileState();

  if (!gMicRunning) {
    if (shouldLog) DEBUG_MIC_VALUESF("[MIC_LEVEL] Mic not enabled - returning 0");
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
      DEBUG_MIC_VALUESF("[MIC_LEVEL] i2sMicMutex busy; returning cached last=%d", lastAudioLevel);
    }
    return lastAudioLevel;
  }

  size_t gotSamples = audioReadPcm(samples, sizeof(samples) / sizeof(int16_t), 50);
  bytesRead = gotSamples * sizeof(int16_t);
  esp_err_t err = (gotSamples > 0) ? ESP_OK : ESP_FAIL;

  if (took && i2sMicMutex) {
    xSemaphoreGive(i2sMicMutex);
  }
  
  if (err != ESP_OK || bytesRead == 0) {
    if (shouldLog) {
      DEBUG_MIC_VALUESF("[MIC_LEVEL] i2s_channel_read failed or no data: err=0x%x bytesRead=%u, returning last=%d",
                 err, bytesRead, lastAudioLevel);
    }
    return lastAudioLevel;
  }

  size_t sampleCount = bytesRead / sizeof(int16_t);
  micProcessForSource(samples, sampleCount);

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
    DEBUG_MIC_VALUESF("[MIC_LEVEL] samples=%u avg=%ld min=%d max=%d level=%d%%",
               sampleCount, avg, minVal, maxVal, level);
  }
  
  lastAudioLevel = level;
  lastAudioLevelMs = now;
  return level;
}

const char* buildMicrophoneStatusJson() {
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "{\"enabled\":%s,\"connected\":%s,\"recording\":%s,"
    "\"source\":\"%s\",\"pdmAvailable\":%s,\"g2Available\":%s,"
    "\"sampleRate\":%u,\"bitDepth\":16,\"channels\":%d,\"level\":%d}",
    gMicRunning ? "true" : "false",
    micConnected ? "true" : "false",
    micRecording ? "true" : "false",
    micSourceName(),
    audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "true" : "false",
    audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "true" : "false",
    (unsigned)micEffectiveSampleRate(), micChannels,
    gMicRunning ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_mic(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
      "{\"schema\":1,\"enabled\":%s,\"connected\":%s,\"recording\":%s,"
      "\"source\":\"%s\",\"pdmAvailable\":%s,\"g2Available\":%s,"
      "\"sampleRate\":%u,\"bitDepth\":16,\"channels\":%d,\"level\":%d}",
      gMicRunning ? "true" : "false", micConnected ? "true" : "false", micRecording ? "true" : "false",
      micSourceName(),
      audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "true" : "false",
      audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "true" : "false",
      (unsigned)micEffectiveSampleRate(), micChannels, gMicRunning ? getAudioLevel() : 0);
    return gMicCmdBuffer;
  }
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "Microphone Status:\n"
    "  Enabled: %s\n"
    "  Connected: %s\n"
    "  Recording: %s\n"
    "  Source: %s (pdm:%s g2:%s)\n"
    "  Sample Rate: %u Hz\n"
    "  Bit Depth: 16\n"
    "  Channels: %d\n"
    "  Level: %d%%",
    gMicRunning ? "yes" : "no",
    micConnected ? "yes" : "no",
    micRecording ? "yes" : "no",
    micSourceName(),
    audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "yes" : "no",
    audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "yes" : "no",
    (unsigned)micEffectiveSampleRate(), micChannels,
    gMicRunning ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

const char* cmd_micstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gSettings.micEnabled) {
    return "ERROR: Microphone is disabled - run 'micenabled 1' first";
  }
  if (initMicrophone()) {
    return "Microphone started successfully";
  }
  return "Error: Failed to start microphone";
}

const char* cmd_micstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  stopMicrophone();
  return "Microphone stopped";
}

const char* cmd_miclevel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "{\"schema\":1,\"enabled\":%s,\"level\":%d}",
      gMicRunning ? "true" : "false", gMicRunning ? getAudioLevel() : 0);
    return gMicCmdBuffer;
  }
  if (!gMicRunning) {
    return "Error: Microphone not enabled";
  }
  int level = getAudioLevel();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Audio level: %d%%", level);
  return gMicCmdBuffer;
}

const char* cmd_micrecord(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gMicRunning) {
    return "Error: Microphone not enabled. Use 'openmic' first.";
  }

  String arg = argsInput;
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
      return "Error: Failed to start recording";
    }
  } else if (arg == "0" || arg.equalsIgnoreCase("stop")) {
    bool wasRecording = micRecording;
    stopRecording();
    if (!wasRecording) return "Recording stopped";
    // Integer math for "N.Ns" (avoid float printf on newlib-nano).
    uint32_t ms = (micSampleRate > 0)
                    ? (uint32_t)((uint64_t)recordingSamples * 1000 / (uint32_t)micSampleRate)
                    : 0;
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording stopped — %s (%lu.%lus)",
             currentRecordingPath, (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
    return gMicCmdBuffer;
  }
  
  return "Error: invalid arguments — Usage: micrecord <start|stop|1|0>";
}

const char* cmd_miclist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["count"] = getRecordingCount();
    JsonArray arr = doc["recordings"].to<JsonArray>();
    String list = getRecordingsList();  // "name:size,name:size,..."
    int start = 0;
    while (start < (int)list.length()) {
      int comma = list.indexOf(',', start);
      String entry = (comma < 0) ? list.substring(start) : list.substring(start, comma);
      entry.trim();
      if (entry.length()) {
        int colon = entry.lastIndexOf(':');
        JsonObject o = arr.add<JsonObject>();
        if (colon > 0) { o["filename"] = entry.substring(0, colon); o["size"] = entry.substring(colon + 1).toInt(); }
        else           { o["filename"] = entry; }
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  int count = getRecordingCount();
  if (count == 0) {
    return "No recordings found";
  }

  String list = getRecordingsList();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recordings (%d):\n%s", count, list.c_str());
  return gMicCmdBuffer;
}

const char* cmd_micdelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // `micdelete all` (bare keyword) wipes recordings; otherwise the filename is a
  // quoted token (uniform quoted-path rule).
  CommandArgs a(argsInput);
  bool isAll = (a.has(0) && !a.argWasQuoted(0) && a.arg(0).equalsIgnoreCase("all"));
  String arg;
  if (!isAll) {
    const char* qerr = requireQuotedToken(a, 0, arg);
    if (qerr) return qerr;
    if (a.has(1)) return "Error: unexpected argument — micdelete \"<filename>\" or micdelete all";
  }

  if (isAll) {
    auto wipeWavs = [](const char* folder) -> int {
      int deleted = 0;
      if (!VFS::existsGuarded(folder, currentAuthContext())) return 0;
      File dir = VFS::openGuarded(String(folder), "r", currentAuthContext());
      if (!dir || !dir.isDirectory()) return 0;
      File f = dir.openNextFile();
      while (f) {
        String name = f.name();
        bool isWav = name.endsWith(".wav");
        f.close();
        if (isWav) {
          String path = String(folder) + "/" + name;
          if (VFS::removeGuarded(path, currentAuthContext())) deleted++;
        }
        f = dir.openNextFile();
      }
      return deleted;
    };
    int deleted = wipeWavs(kMicRecSD) + wipeWavs(kMicRecLittleFS);
    if (deleted == 0) {
      return "No recordings found";
    }
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted %d recording(s)", deleted);
    return gMicCmdBuffer;
  }
  
  if (deleteRecording(arg.c_str())) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted: %s", arg.c_str());
    return gMicCmdBuffer;
  }
  return "Error: File not found";
}

const char* cmd_micsamplerate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate: %d Hz", micSampleRate);
    return gMicCmdBuffer;
  }

  int rate = arg.toInt();
  if (rate < 8000 || rate > 48000) {
    return "Error: Sample rate must be 8000-48000 Hz";
  }

  STACK_TRACEF("cmd_micsamplerate.enter requested=%d current=%d gMicRunning=%d",
               rate, micSampleRate, gMicRunning);

  // Need to reinitialize if already running
  bool wasEnabled = gMicRunning;
  if (wasEnabled) {
    STACK_TRACEF("cmd_micsamplerate.before_stop");
    stopMicrophone();
    STACK_TRACEF("cmd_micsamplerate.after_stop");
  }

  micSampleRate = rate;
  setSetting(gSettings.microphoneSampleRate, rate);
  STACK_TRACEF("cmd_micsamplerate.rate_saved_to_settings");

  if (wasEnabled) {
    STACK_TRACEF("cmd_micsamplerate.before_reinit");
    bool ok = initMicrophone();
    STACK_TRACEF("cmd_micsamplerate.after_reinit ok=%d gMicRunning=%d",
                 ok ? 1 : 0, gMicRunning);
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate set to %d Hz (saved)%s", micSampleRate,
           audioGetSource() == AUDIO_SRC_G2_LEFT ? " (ignored while source=G2 — the glasses mic is fixed 16 kHz)" : "");
  return gMicCmdBuffer;
}

const char* cmd_micgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain: %d%%", micGain);
    return gMicCmdBuffer;
  }

  int gain = arg.toInt();
  if (gain < 0 || gain > 100) {
    return "Error: Gain must be 0-100%";
  }
  
  micGain = gain;
  setSetting(gSettings.microphoneGain, gain);
  
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain set to %d%% (saved)", micGain);
  return gMicCmdBuffer;
}

const char* cmd_micbitdepth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Bit depth: %d-bit", micBitDepth);
    return gMicCmdBuffer;
  }

  int depth = arg.toInt();
  if (depth != 16 && depth != 32) {
    return "Error: Bit depth must be 16 or 32";
  }

  STACK_TRACEF("cmd_micbitdepth.enter requested=%d current=%d gMicRunning=%d",
               depth, micBitDepth, gMicRunning);

  // Need to reinitialize if already running
  bool wasEnabled = gMicRunning;
  if (wasEnabled) {
    STACK_TRACEF("cmd_micbitdepth.before_stop");
    stopMicrophone();
    STACK_TRACEF("cmd_micbitdepth.after_stop");
  }

  micBitDepth = depth;
  setSetting(gSettings.microphoneBitDepth, depth);
  STACK_TRACEF("cmd_micbitdepth.depth_saved_to_settings");

  if (wasEnabled) {
    STACK_TRACEF("cmd_micbitdepth.before_reinit");
    bool ok = initMicrophone();
    STACK_TRACEF("cmd_micbitdepth.after_reinit ok=%d gMicRunning=%d",
                 ok ? 1 : 0, gMicRunning);
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
           "Bit depth set to %d-bit (saved) — note: WAV recordings are always 16-bit (HAL canonical format)", micBitDepth);
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
  
  while (gMicVisualizerRunning && gMicRunning) {
    size_t bytesRead = 0;
    size_t got = audioReadPcm(samples, bufSize, 100);
    bytesRead = got * sizeof(int16_t);
    esp_err_t err = (got > 0) ? ESP_OK : ESP_FAIL;
    
    if (err == ESP_OK && bytesRead > 0) {
      size_t sampleCount = bytesRead / sizeof(int16_t);
      micProcessForSource(samples, sampleCount);
      
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

const char* cmd_micviz(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gMicRunning) {
    return "Error: Microphone not enabled. Use 'openmic' first.";
  }
  
  if (gMicVisualizerRunning) {
    gMicVisualizerRunning = false;
    return "Stopping visualizer...";
  }
  
  gMicVisualizerRunning = true;
  // No core affinity: NORMAL prio on Core 0 was getting preempted by WiFi/BT.
  // mic_record (Core 1, HIGH) owns the audio buffer; viz is a consumer that
  // can run wherever the scheduler has cycles.
  // APP_CORE: waveform-render compute; keep it off the Wi-Fi/BLE core.
  xTaskCreatePinnedToCore(micVisualizerTaskFunc, "mic_viz", MIC_VIZ_STACK_WORDS, nullptr, TASK_PRIORITY_NORMAL, &gMicVisualizerTask, APP_CORE);
  return "Visualizer started (press any key to stop)";
}

const char* cmd_micautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.micAutoStart ? "[Mic] Auto-start: enabled" : "[Mic] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.micAutoStart, true);
    return "[Mic] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.micAutoStart, false);
    return "[Mic] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: micautostart [on|off]";
}

// Get/set the persisted mic-source PREFERENCE {auto,pdm,g2}. The preference is
// resolved lazily against runtime availability at capture start — 'g2' is a
// valid preference even with no glasses connected (it applies when they do). If
// the mic is running, apply the switch live (stop → set → start) since
// audioSetSource refuses a source change mid-capture.
const char* cmd_micsource(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
      "Mic source: preference=%s, active=%s (available: pdm=%s g2=%s). Set with: micsource <auto|pdm|g2>",
      gSettings.micSource.c_str(), micSourceName(),
      audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "yes" : "no",
      audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "yes" : "no");
    return gMicCmdBuffer;
  }
  if (arg != "auto" && arg != "pdm" && arg != "g2") {
    return "Error: invalid arguments — Usage: micsource <auto|pdm|g2>";
  }
  setSetting(gSettings.micSource, arg);
  const bool wasEnabled = gMicRunning;
  if (wasEnabled) {
    stopMicrophone();     // release the lease so the new preference can resolve
    initMicrophone();     // re-resolves source from the updated preference
  }
  const char* note = "";
  if (arg == "g2" && !audioSourceAvailable(AUDIO_SRC_G2_LEFT)) {
    note = " (glasses not connected — applies when they connect)";
  } else if (arg == "pdm" && !audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) {
    note = " (no onboard PDM mic on this board)";
  }
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
           "Mic source preference set to '%s'%s (active=%s)", arg.c_str(), note, micSourceName());
  return gMicCmdBuffer;
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry micCommands[] = {
  { "micread", "Read microphone sensor status.", false, cmd_mic, "Usage: micread [json]" },
  { "openmic", "Start microphone sensor.", false, cmd_micstart, nullptr, "sensor", "microphone", "open" },
  { "closemic", "Stop microphone sensor.", false, cmd_micstop, nullptr, "sensor", "microphone", "close" },
  { "miclevel", "Get current audio level.", false, cmd_miclevel, "Usage: miclevel [json]" },
  { "micviz", "Real-time audio level visualizer.", false, cmd_micviz, "Usage: micviz (press any key to stop)" },
  { "micrecord", "Start/stop recording to WAV file (bare = show recording status).", false, cmd_micrecord, "Usage: micrecord [start|stop|1|0]" },
  { "miclist", "List saved recordings.", false, cmd_miclist, "Usage: miclist [json]" },
  { "micdelete", "Delete recording(s).", true, cmd_micdelete, "Usage: micdelete \"<filename>\" | micdelete all" },
  { "micsamplerate", "Get/set sample rate.", false, cmd_micsamplerate, "Usage: micsamplerate [8000-48000]" },
  { "micgain", "Get/set microphone gain.", false, cmd_micgain, "Usage: micgain [0-100]" },
  { "micbitdepth", "Get/set bit depth.", false, cmd_micbitdepth, "Usage: micbitdepth [16|32]" },
  { "micsource", "Get/set mic source: onboard PDM or G2 glasses.", false, cmd_micsource, "Usage: micsource [auto|pdm|g2]" },

  // Auto-start
  { "micautostart", "Enable/disable microphone auto-start after boot [on|off]", false, cmd_micautostart, "Usage: micautostart [on|off]" },
};

const size_t micCommandsCount = sizeof(micCommands) / sizeof(micCommands[0]);

// Settings module registration
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry micSettingEntries[] = {
  { "micEnabled", SETTING_BOOL, &gSettings.micEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "micenabled" },
  { "microphoneAutoStart", SETTING_BOOL, &gSettings.micAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "micautostart" },
  // Source preference {auto,pdm,g2}. Resolved lazily against availability.
  { "micSource", SETTING_STRING, &gSettings.micSource, 0, 0, "auto", 0, 0, "Mic source", nullptr, false, nullptr, "micsource" },
  // These three were previously reported as "(saved)" but never registered, so
  // they silently reset to defaults on reboot. Registering them fixes that.
  { "microphoneSampleRate", SETTING_INT, &gSettings.microphoneSampleRate, 16000, 0, nullptr, 8000, 48000, "Sample rate (Hz, PDM only)", nullptr, false, nullptr, "micsamplerate" },
  { "microphoneGain", SETTING_INT, &gSettings.microphoneGain, 70, 0, nullptr, 0, 100, "Software gain (%)", nullptr, false, nullptr, "micgain" },
  { "microphoneBitDepth", SETTING_INT, &gSettings.microphoneBitDepth, 16, 0, nullptr, 16, 32, "Bit depth (cosmetic; WAV is always 16-bit)", nullptr, false, nullptr, "micbitdepth" },
};

// Module "connected" = a mic source is actually available (onboard PDM present
// OR G2 glasses connected). Drives whether the settings module is shown/active.
static bool isMicConnected() {
  return audioAnySourceAvailable();
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule micSettingsModule = {
  "microphone",
  "hardware.sensors.microphone",
  micSettingEntries,
  sizeof(micSettingEntries) / sizeof(micSettingEntries[0]),
  isMicConnected,
  "Microphone (onboard PDM or G2 glasses)"
};

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_MICROPHONE
