/**
 * FM Radio Sensor - ScoutMakes FM Radio Board (RDA5807M)
 * fm_radio.cpp
 * 
 * STEMMA QT / Qwiic I2C FM Radio breakout board
 * I2C Address: 0x11
 * Library: mathertel Radio (install via Arduino Library Manager)
 * 
 * Product: https://www.adafruit.com/product/5765
 * Library: https://github.com/mathertel/Radio
 */

#include "i2csensor-rda5807.h"
#include "System_BuildConfig.h"
#include "System_Utils.h"

#if ENABLE_FM_RADIO

#include <Arduino.h>
#include <radio.h>
#include <RDA5807M.h>
#include <RDSParser.h>
#include <Wire.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Settings.h"

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#endif

// The RDA5807 is on Wire1 (STEMMA QT bus)
extern TwoWire Wire1;

// External dependencies
extern void broadcastOutput(const char* s);
extern void broadcastOutput(const String& s);

// Forward declarations
extern bool createFMRadioTask();
extern volatile bool gSensorPollingPaused;
void updateFMRadio();

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// FM Radio I2C clock speed (100kHz for bus stability with other devices)
static const uint32_t FM_RADIO_I2C_CLOCK = 100000;

// ============================================================================
// FM Radio State (Global Variables)
// ============================================================================

bool fmRadioEnabled = false;
bool fmRadioConnected = false;
TaskHandle_t fmRadioTaskHandle = nullptr;
unsigned long fmRadioLastStopTime = 0;
uint16_t fmRadioFrequency = 10390;  // Default 103.9 MHz (in 10kHz units)
uint8_t fmRadioVolume = 6;          // Default volume (0-15)
bool fmRadioMuted = false;
bool fmRadioStereo = true;

// RDS data
char fmRadioStationName[9] = {0};
char fmRadioStationText[65] = {0};

// Signal quality
uint8_t fmRadioRSSI = 0;
uint8_t fmRadioSNR = 0;

// Headphone detection (based on RSSI threshold)
bool fmRadioHeadphonesConnected = false;

// RDA5807M radio object (mathertel library)
static RDA5807M radio;
static RDSParser rds;
bool radioInitialized = false;

static volatile bool fmRadioInitRequested = false;
static volatile bool fmRadioInitDone = false;
static bool fmRadioInitResult = false;

// RDS callback to update station name
static void RDS_ServiceNameCallback(const char* name) {
  DEBUG_FMRADIOF("[FM_RADIO] RDS Station Name callback: '%s'", name ? name : "null");
  
  if (name != nullptr && strlen(name) > 0) {
    if (strncmp(fmRadioStationName, name, 8) != 0) {
      DEBUG_FMRADIOF("[FM_RADIO] Station name changed from '%s' to '%s'", fmRadioStationName, name);
      strncpy(fmRadioStationName, name, 8);
      fmRadioStationName[8] = '\0';
    }
  }
}

// RDS callback to update radio text
static void RDS_TextCallback(const char* text) {
  DEBUG_FMRADIOF("[FM_RADIO] RDS Text callback: '%s'", text ? text : "null");
  
  if (text != nullptr && strlen(text) > 0) {
    if (strncmp(fmRadioStationText, text, 64) != 0) {
      DEBUG_FMRADIOF("[FM_RADIO] Station text changed from '%s' to '%s'", fmRadioStationText, text);
      strncpy(fmRadioStationText, text, 64);
      fmRadioStationText[64] = '\0';
    }
  }
}

// RDS data processor callback
static void RDS_Process(uint16_t block1, uint16_t block2, uint16_t block3, uint16_t block4) {
  rds.processData(block1, block2, block3, block4);
}

// ============================================================================
// FM Radio Initialization
// ============================================================================

bool initFMRadio() {
  INFO_SENSORSF("initFMRadio() called - fmRadioConnected=%s, radioInitialized=%s", 
                fmRadioConnected ? "true" : "false", radioInitialized ? "true" : "false");
  
  if (fmRadioConnected && radioInitialized) {
    INFO_SENSORSF("FM Radio already initialized");
    return true;  // Already initialized
  }
  
  bool success = false;
  INFO_SENSORSF("Starting FM Radio I2C initialization");
  
  i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 1000, [&]() {
    DEBUG_FMRADIOF("I2C transaction started, calling radio.initWire(Wire1)");
    // Initialize the radio with Wire1 (STEMMA QT bus)
    if (!radio.initWire(Wire1)) {
      ERROR_SENSORSF("FM Radio initWire() failed - check I2C connections");
      return;  // Init failed
    }
    INFO_SENSORSF("FM Radio initWire() success - RDA5807M chip detected");
    radio.debugEnable(false);
    
    // Set band and initial frequency
    DEBUG_FMRADIOF("[FM_RADIO] Setting band to FM and frequency to %.1f MHz", fmRadioFrequency / 10.0);
    radio.setBandFrequency(RADIO_BAND_FM, fmRadioFrequency);
    DEBUG_FMRADIOF("[FM_RADIO] Band/frequency set successfully");
    
    DEBUG_FMRADIOF("[FM_RADIO] Setting volume to %d/15", fmRadioVolume);
    radio.setVolume(fmRadioVolume);
    DEBUG_FMRADIOF("[FM_RADIO] Volume set successfully");
    
    radio.setMono(false);  // Stereo mode
    DEBUG_FMRADIOF("[FM_RADIO] Stereo mode enabled");
    
    radio.setMute(false);
    DEBUG_FMRADIOF("[FM_RADIO] Unmuted - audio should be active");
    
    // Setup RDS callbacks
    radio.attachReceiveRDS(RDS_Process);
    rds.attachServiceNameCallback(RDS_ServiceNameCallback);
    rds.attachTextCallback(RDS_TextCallback);
    
    radioInitialized = true;
    fmRadioConnected = true;
    success = true;
  });
  
  if (success) {
    // Register FM radio for I2C health tracking
    i2cRegisterDevice(I2C_ADDR_FM_RADIO, "FM_Radio");
    INFO_SENSORSF("FM Radio initialized successfully - RDA5807M ready at %.1f MHz, volume %d", 
                  fmRadioFrequency / 100.0, fmRadioVolume);
    sensorStatusBumpWith("fmradio initialized");
  } else {
    ERROR_SENSORSF("FM Radio initialization failed - I2C mutex acquisition failed or transaction error");
  }
  return success;
}

void deinitFMRadio() {
  DEBUG_FMRADIOF("[FM_RADIO] deinitFMRadio() called - radioInitialized=%s", radioInitialized ? "true" : "false");
  
  if (radioInitialized) {
    DEBUG_FMRADIOF("[FM_RADIO] Starting I2C transaction for deinitialization");
    i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 500, [&]() {
      DEBUG_FMRADIOF("[FM_RADIO] Muting radio before termination");
      radio.setMute(true);
      DEBUG_FMRADIOF("[FM_RADIO] Calling radio.term() to power down chip");
      radio.term();
      DEBUG_FMRADIOF("[FM_RADIO] Radio terminated successfully");
    });
  } else {
    DEBUG_FMRADIOF("[FM_RADIO] Radio not initialized, skipping termination");
  }
  
  DEBUG_FMRADIOF("[FM_RADIO] Resetting all state variables");
  radioInitialized = false;
  fmRadioConnected = false;
  fmRadioEnabled = false;
  memset(fmRadioStationName, 0, sizeof(fmRadioStationName));
  memset(fmRadioStationText, 0, sizeof(fmRadioStationText));
  DEBUG_FMRADIOF("[FM_RADIO] Deinitialization completed");
}

// ============================================================================
// FM Radio Task Implementation
// ============================================================================

void fmRadioTask(void* parameter) {
  INFO_SENSORSF("FM Radio task started (handle=%p)", fmRadioTaskHandle);
  unsigned long lastStackLog = 0;
  unsigned long loopCount = 0;
  bool initWatermarkLogged = false;
  
  while (true) {
    loopCount++;

    // Deferred initialization on the FM radio task stack (keeps sensor_queue stack safe)
    if (fmRadioEnabled && !radioInitialized && fmRadioInitRequested) {
      INFO_SENSORSF("Performing deferred FM Radio init on task stack");
      bool ok = initFMRadio();
      fmRadioInitResult = ok;
      fmRadioInitDone = true;
      fmRadioInitRequested = false;

      if (!ok) {
        ERROR_SENSORSF("FM Radio initFMRadio() failed");
        broadcastOutput("FM Radio init failed");
        sensorStatusBumpWith("fmradio@init_failed");
        fmRadioEnabled = false;
        // Loop will delete the task on next iteration
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      // Unmute now that init succeeded
      DEBUG_FMRADIOF("[FM_RADIO_TASK] Unmuting radio for audio output");
      i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 200, [&]() {
        radio.setMute(false);
        fmRadioMuted = false;
        DEBUG_FMRADIOF("[FM_RADIO_TASK] Radio unmuted successfully");
      });
      DEBUG_FMRADIOF("[FM_RADIO_TASK] FM Radio started successfully at %.1f MHz", fmRadioFrequency / 100.0);
      sensorStatusBumpWith("fmradio started");
      
      // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
      broadcastSensorStatus(REMOTE_SENSOR_FMRADIO, true);
#endif

      // One-time watermark log right after init completes (captures peak without waiting 30s)
      if (!initWatermarkLogged) {
        initWatermarkLogged = true;
        if (isDebugFlagSet(DEBUG_FMRADIO)) {
          const uint32_t fmRadioStackWords = 4608;  // keep in sync with createFMRadioTask()
          UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
          DEBUG_FMRADIOF("[FM_RADIO_TASK] Post-init stack watermark: %u bytes (%.1f%% used of %u bytes)",
                         (unsigned)(watermark * 4),
                         (float)(fmRadioStackWords - watermark) * 100.0f / (float)fmRadioStackWords,
                         (unsigned)(fmRadioStackWords * 4));
        }
      }
    }
    
    // Check if radio is disabled - delete task if so
    if (!fmRadioEnabled) {
      DEBUG_FMRADIOF("[FM_RADIO_TASK] Radio disabled, deleting task (loop %lu)", loopCount);
      // NOTE: Do NOT clear fmRadioTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }
    
    // Skip polling if sensor polling is paused (to avoid I2C bus conflicts)
    if (gSensorPollingPaused) {
      if (loopCount % 20 == 0) {  // Log every 10 seconds when paused
        DEBUG_FMRADIOF("[FM_RADIO_TASK] Sensor polling paused, waiting (loop %lu)", loopCount);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    // Update FM radio data (RDS, signal strength, headphone detection)
    if (loopCount % 4 == 0) {  // Log every 1 second during normal operation
      DEBUG_FMRADIOF("[FM_RADIO_TASK] Updating radio data (loop %lu)", loopCount);
    }
    updateFMRadio();
    
    // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
    if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
      char fmJson[512];
      int jsonLen = buildFMRadioDataJSON(fmJson, sizeof(fmJson));
      if (jsonLen > 0) {
        sendSensorDataUpdate(REMOTE_SENSOR_FMRADIO, String(fmJson));
      }
    }
#endif
    
    // Log stack usage every 30 seconds (for debugging stack overflow issues)
    unsigned long now = millis();
    if (now - lastStackLog > 30000) {
      lastStackLog = now;
      if (isDebugFlagSet(DEBUG_FMRADIO)) {
        const uint32_t fmRadioStackWords = 4608;  // keep in sync with createFMRadioTask()
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_FMRADIOF("[FM_RADIO_TASK] Stack watermark: %u bytes (%.1f%% used of %u bytes)", 
                       (unsigned)(watermark * 4),
                       (float)(fmRadioStackWords - watermark) * 100.0f / (float)fmRadioStackWords,
                       (unsigned)(fmRadioStackWords * 4));
      }
    }
    
    // Poll every 250ms for responsive RDS updates
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ============================================================================
// FM Radio Internal Functions (for sensor queue system)
// ============================================================================

void startFMRadioInternal() {
  DEBUG_FMRADIOF("[FM_RADIO] startFMRadioInternal() called - fmRadioEnabled=%s", fmRadioEnabled ? "true" : "false");
  
  if (fmRadioEnabled) {
    DEBUG_FMRADIOF("[FM_RADIO] Already enabled, skipping initialization");
    return;
  }

  // Enable first, then let fmradio_task perform initialization on its own stack
  fmRadioEnabled = true;
  fmRadioInitRequested = true;
  fmRadioInitDone = false;
  fmRadioInitResult = false;
  DEBUG_FMRADIOF("[FM_RADIO] Radio enabled flag set; init will run on fmradio_task");
  
  // Create FM Radio task if not already running
  if (fmRadioTaskHandle == nullptr) {
    DEBUG_FMRADIOF("[FM_RADIO] Creating FM Radio task...");
    if (!createFMRadioTask()) {
      DEBUG_FMRADIOF("[FM_RADIO] ERROR: Failed to create FM Radio task");
      fmRadioEnabled = false;
      return;
    }
    DEBUG_FMRADIOF("[FM_RADIO] FM Radio task created successfully (handle=%p)", fmRadioTaskHandle);
  } else {
    DEBUG_FMRADIOF("[FM_RADIO] FM Radio task already running (handle=%p)", fmRadioTaskHandle);
  }
}

void stopFMRadioInternal() {
  DEBUG_FMRADIOF("[FM_RADIO] stopFMRadioInternal() called - fmRadioEnabled=%s", fmRadioEnabled ? "true" : "false");
  
  if (!fmRadioEnabled) {
    DEBUG_FMRADIOF("[FM_RADIO] Already stopped, skipping shutdown");
    return;
  }
  
  // Clear enabled flag first - task will see this and delete itself
  fmRadioEnabled = false;
  DEBUG_FMRADIOF("[FM_RADIO] Radio enabled flag cleared - task will self-destruct");
  
  // Properly deinitialize the radio hardware
  if (radioInitialized) {
    DEBUG_FMRADIOF("[FM_RADIO] Calling deinitFMRadio() to shut down hardware");
    deinitFMRadio();
  } else {
    DEBUG_FMRADIOF("[FM_RADIO] Radio not initialized, skipping hardware shutdown");
  }
  
  fmRadioMuted = true;
  
  // Trigger sensor status update to notify web UI
  sensorStatusBumpWith("fmradio stopped");
  
  broadcastOutput("FM Radio stopped");
  DEBUG_FMRADIOF("[FM_RADIO] FM Radio stopped successfully");
}

// ============================================================================
// FM Radio Polling (for RDS updates)
// ============================================================================

void updateFMRadio() {
  static unsigned long lastUpdateLog = 0;
  static int lastRSSI = -999;
  static bool lastStereo = false;
  
  if (!radioInitialized || !fmRadioEnabled) {
    DEBUG_FMRADIOF("[FM_RADIO] Skipping update - radio not ready");
    return;
  }
  
  // Skip polling if sensor polling is paused (to avoid I2C bus conflicts)
  if (gSensorPollingPaused) {
    return;  // Don't log this - too frequent
  }
  
  // Use task timeout wrapper to catch FM radio performance issues
  // Note: Still NACK-tolerant since RDA5807M legitimately NACKs when no RDS data available
  auto result = i2cTaskWithTimeout(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 1000, [&]() -> bool {
    // Wrap the NACK-tolerant transaction within timeout monitoring
    i2cTransactionNACKTolerant(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 100, [&]() {
      // Check for RDS data (this triggers callbacks)
      radio.checkRDS();
      
      // Update signal quality and stereo status
      RADIO_INFO ri;
      radio.getRadioInfo(&ri);
      fmRadioRSSI = ri.rssi;
      fmRadioStereo = ri.stereo;
      fmRadioSNR = ri.snr;
      
      // Only log when signal changes significantly (reduces flood)
      unsigned long now = millis();
      if (abs(fmRadioRSSI - lastRSSI) >= 2 || fmRadioStereo != lastStereo || (now - lastUpdateLog > 30000)) {
        DEBUG_FMRADIOF("[FM_RADIO] Signal: RSSI=%d, SNR=%d, Stereo=%s", 
                       fmRadioRSSI, fmRadioSNR, fmRadioStereo ? "true" : "false");
        lastRSSI = fmRadioRSSI;
        lastStereo = fmRadioStereo;
        lastUpdateLog = now;
      }
      
      // Update headphone detection based on RSSI
      fmRadioHeadphonesConnected = (fmRadioRSSI >= 15);
    });
    return true;  // Assume success for void operation
  });
}

// ============================================================================
// Command Handlers
// ============================================================================

const char* cmd_fmradio(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_FMRADIOF("[FM_RADIO] Command received: '%s'", cmd.c_str());
  
  // Parse subcommand
  int spaceIdx = cmd.indexOf(' ');
  if (spaceIdx < 0) {
    // No subcommand - show status
    DEBUG_FMRADIOF("[FM_RADIO] No subcommand, showing status");
    return cmd_fmradio_status(cmd);
  }
  
  String sub = cmd.substring(spaceIdx + 1);
  sub.trim();
  sub.toLowerCase();
  
  DEBUG_FMRADIOF("[FM_RADIO] Parsed subcommand: '%s'", sub.c_str());
  
  if (sub.startsWith("start")) {
    return cmd_fmradio_start(cmd);
  } else if (sub.startsWith("stop")) {
    return cmd_fmradio_stop(cmd);
  } else if (sub.startsWith("tune ")) {
    return cmd_fmradio_tune(cmd);
  } else if (sub.startsWith("seek")) {
    return cmd_fmradio_seek(cmd);
  } else if (sub.startsWith("volume ") || sub.startsWith("vol ")) {
    return cmd_fmradio_volume(cmd);
  } else if (sub.startsWith("mute")) {
    return cmd_fmradio_mute(cmd);
  } else if (sub.startsWith("status")) {
    return cmd_fmradio_status(cmd);
  }
  
  DEBUG_FMRADIOF("[FM_RADIO] Unknown subcommand: '%s'", sub.c_str());
  return "Usage: fmradio [start|stop|tune <freq>|seek [up|down]|volume <0-15>|mute|status]";
}

const char* cmd_fmradio_start(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (fmRadioEnabled) {
    return "FM Radio already running";
  }
  
  // Queue the FM radio start request
  enqueueSensorStart(SENSOR_FMRADIO);
  
  return "FM Radio start queued";
}

const char* cmd_fmradio_stop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!fmRadioEnabled) {
    return "FM Radio not running";
  }
  
  // Stop immediately (no need to queue for stop)
  stopFMRadioInternal();
  
  return "OK";
}

const char* cmd_fmradio_tune(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse frequency from command: "fmradio tune 103.9" or "fmradio tune 10390"
  int tuneIdx = cmd.indexOf("tune");
  if (tuneIdx < 0) {
    return "Usage: fmradio tune <frequency> (e.g., 103.9 or 10390)";
  }
  
  String freqStr = cmd.substring(tuneIdx + 5);
  freqStr.trim();
  
  float freq = freqStr.toFloat();
  uint16_t freqInt;
  
  if (freq < 200) {
    // Assume MHz format (e.g., 103.9)
    freqInt = (uint16_t)(freq * 100);
  } else {
    // Assume 10kHz format (e.g., 10390)
    freqInt = (uint16_t)freq;
  }
  
  // Validate frequency range (76-108 MHz)
  if (freqInt < 7600 || freqInt > 10800) {
    return "[FM Radio] Error: Frequency must be 76.0-108.0 MHz";
  }
  
  if (!fmRadioConnected || !radioInitialized) {
    if (!initFMRadio()) {
      return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
    }
  }
  
  i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 500, [&]() {
    radio.setFrequency(freqInt);
    fmRadioFrequency = freqInt;
    
    // Clear RDS data on frequency change
    memset(fmRadioStationName, 0, sizeof(fmRadioStationName));
    memset(fmRadioStationText, 0, sizeof(fmRadioStationText));
  });
  
  BROADCAST_PRINTF("Tuned to %.1f MHz", freqInt / 100.0);
  return "OK";
}

const char* cmd_fmradio_seek(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!fmRadioConnected || !radioInitialized) {
    return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
  }
  
  bool seekUp = true;  // Default seek up
  
  if (cmd.indexOf("down") >= 0) {
    seekUp = false;
  }
  
  i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 6000, [&]() {
    // mathertel library seek methods
    if (seekUp) {
      radio.seekUp(false);  // false = don't wrap
    } else {
      radio.seekDown(false);
    }
    
    // Wait for seek to complete (with timeout)
    unsigned long start = millis();
    RADIO_INFO ri;
    do {
      vTaskDelay(pdMS_TO_TICKS(100));
      radio.getRadioInfo(&ri);
    } while (!ri.tuned && (millis() - start) < 5000);
    
    fmRadioFrequency = radio.getFrequency();
    
    // Clear RDS data on frequency change
    memset(fmRadioStationName, 0, sizeof(fmRadioStationName));
    memset(fmRadioStationText, 0, sizeof(fmRadioStationText));
  });
  
  BROADCAST_PRINTF("Seeked %s to %.1f MHz", seekUp ? "up" : "down", fmRadioFrequency / 100.0);
  return "OK";
}

const char* cmd_fmradio_volume(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse volume: "fmradio volume 8" or "fmradio vol 8"
  int volIdx = cmd.indexOf("vol");
  if (volIdx < 0) {
    return "Usage: fmradio volume <0-15>";
  }
  
  // Find the number after "volume" or "vol"
  String rest = cmd.substring(volIdx);
  int spaceIdx = rest.indexOf(' ');
  if (spaceIdx < 0) {
    BROADCAST_PRINTF("Current volume: %d", fmRadioVolume);
    return "OK";
  }
  
  String volStr = rest.substring(spaceIdx + 1);
  volStr.trim();
  int vol = volStr.toInt();
  
  if (vol < 0 || vol > 15) {
    return "[FM Radio] Error: Volume must be 0-15";
  }
  
  if (!fmRadioConnected || !radioInitialized) {
    fmRadioVolume = vol;  // Store for when radio starts
    BROADCAST_PRINTF("Volume set to %d (will apply when radio starts)", vol);
    return "OK";
  }
  
  i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setVolume(vol);
    fmRadioVolume = vol;
  });
  
  BROADCAST_PRINTF("Volume set to %d", vol);
  return "OK";
}

const char* cmd_fmradio_mute(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!fmRadioConnected || !radioInitialized) {
    return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
  }
  
  // Check if this is "mute" or "unmute" command
  bool shouldMute = (cmd.indexOf("unmute") < 0);  // If "unmute" not found, then mute
  fmRadioMuted = shouldMute;
  
  i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setMute(fmRadioMuted);
  });
  
  BROADCAST_PRINTF("FM Radio %s", fmRadioMuted ? "muted" : "unmuted");
  return "OK";
}

const char* cmd_fmradio_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  static char statusBuf[512];
  
  if (!fmRadioConnected) {
    snprintf(statusBuf, sizeof(statusBuf),
      "FM Radio Status:\n"
      "  Connected: No\n"
      "  Enabled: %s\n"
      "  Stored Frequency: %.1f MHz\n"
      "  Stored Volume: %d",
      fmRadioEnabled ? "Yes" : "No",
      fmRadioFrequency / 100.0,
      fmRadioVolume);
  } else {
    // Update signal info
    if (radioInitialized) {
      i2cTransactionVoid(FM_RADIO_I2C_CLOCK, 200, [&]() {
        RADIO_INFO ri;
        radio.getRadioInfo(&ri);
        fmRadioRSSI = ri.rssi;
        fmRadioStereo = ri.stereo;
      });
    }
    
    snprintf(statusBuf, sizeof(statusBuf),
      "FM Radio Status:\n"
      "  Connected: Yes\n"
      "  Enabled: %s\n"
      "  Frequency: %.1f MHz\n"
      "  Volume: %d/15\n"
      "  Muted: %s\n"
      "  Stereo: %s\n"
      "  RSSI: %d\n"
      "  Headphones: %s\n"
      "  Station: %s\n"
      "  Radio Text: %s",
      fmRadioEnabled ? "Yes" : "No",
      fmRadioFrequency / 100.0,
      fmRadioVolume,
      fmRadioMuted ? "Yes" : "No",
      fmRadioStereo ? "Yes" : "No",
      fmRadioRSSI,
      fmRadioHeadphonesConnected ? "Yes" : "No",
      strlen(fmRadioStationName) > 0 ? fmRadioStationName : "(none)",
      strlen(fmRadioStationText) > 0 ? fmRadioStationText : "(none)");
  }
  
  broadcastOutput(statusBuf);
  return "OK";
}

// ============================================================================
// JSON Data Builder (for web API)
// ============================================================================

int buildFMRadioDataJSON(char* buf, size_t bufSize) {
  int len = snprintf(buf, bufSize,
    "{\"connected\":%s,\"enabled\":%s,\"frequency\":%.1f,\"volume\":%d,"
    "\"muted\":%s,\"stereo\":%s,\"rssi\":%d,\"headphones\":%s,\"station\":\"%s\",\"radioText\":\"%s\"}",
    fmRadioConnected ? "true" : "false",
    fmRadioEnabled ? "true" : "false",
    fmRadioFrequency / 100.0,
    fmRadioVolume,
    fmRadioMuted ? "true" : "false",
    fmRadioStereo ? "true" : "false",
    fmRadioRSSI,
    fmRadioHeadphonesConnected ? "true" : "false",
    fmRadioStationName,
    fmRadioStationText);
  return len;
}

// ============================================================================
// Command Registration
// ============================================================================

const CommandEntry fmRadioCommands[] = {
  { "fmstatus", "Show FM Radio status", false, cmd_fmradio, "Usage: fmradio [start|stop|tune <freq>|seek [up|down]|volume <0-15>|mute|status]" },
  { "fmradio start", "Start FM Radio", false, cmd_fmradio_start },
  { "fmradio stop", "Stop FM Radio", false, cmd_fmradio_stop },
  { "fmradio tune", "Tune to frequency (e.g., fmradio tune 103.9)", false, cmd_fmradio_tune, "Usage: fmradio tune <frequency> (e.g., 103.9 or 10390)" },
  { "fmradio seek", "Seek next station (up/down)", false, cmd_fmradio_seek },
  { "fmradio volume", "Set volume 0-15", false, cmd_fmradio_volume, "Usage: fmradio volume <0-15>" },
  { "fmradio mute", "Mute audio", false, cmd_fmradio_mute },
  { "fmradio unmute", "Unmute audio", false, cmd_fmradio_mute },
  { "fmradio status", "Show FM Radio status", false, cmd_fmradio_status },
};

const size_t fmRadioCommandsCount = sizeof(fmRadioCommands) / sizeof(fmRadioCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _fmradio_cmd_registrar(fmRadioCommands, fmRadioCommandsCount, "fmradio");

// ============================================================================
// FM Radio Modular Settings Registration (for safety and reliability)
// ============================================================================

// FM Radio settings entries
static const SettingEntry fmRadioSettingEntries[] = {
  // Core settings
  { "autoStart", SETTING_BOOL, &gSettings.fmRadioAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  // Device-level settings (sensor hardware behavior)
  { "device.devicePollMs", SETTING_INT, &gSettings.fmRadioDevicePollMs, 250, 0, nullptr, 100, 5000, "Poll Interval (ms)", nullptr }
};

static bool isFMRadioConnected() {
  return fmRadioConnected;
}

extern const SettingsModule fmRadioSettingsModule = {
  "fmradio",
  nullptr,
  fmRadioSettingEntries,
  sizeof(fmRadioSettingEntries) / sizeof(fmRadioSettingEntries[0]),
  isFMRadioConnected,
  "RDA5807 FM radio settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// FM Radio OLED Mode (Display Function + Registration)
// ============================================================================
#include "i2csensor-rda5807-oled.h"

#endif // ENABLE_FM_RADIO
