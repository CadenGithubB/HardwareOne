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
#include "System_TaskUtils.h"

#if ENABLE_FM_RADIO

#include <Arduino.h>
#include <radio.h>
#include <RDA5807M.h>
#include <RDSParser.h>
#include <Wire.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#endif

// The RDA5807 is on Wire1 (STEMMA QT bus)
extern TwoWire Wire1;

// Forward declarations
extern bool createFMRadioTask();
void updateFMRadio();

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// FM Radio I2C clock speed (100kHz for bus stability with other devices)
static const uint32_t FM_RADIO_I2C_CLOCK = 100000;

// FM Radio I2C address (RDA5807M uses 0x11, defined in System_I2C.h as I2C_ADDR_FM_RADIO)

// ============================================================================
// FM Radio State (Global Variables)
// ============================================================================

bool gFmRadioEnabled = false;
bool gFmRadioConnected = false;
TaskHandle_t gFmRadioTaskHandle = nullptr;
unsigned long gFmRadioLastStopTime = 0;

// Cache struct: all data fields + mutex. Initialized with defaults via
// FMRadioCache's default member initializers.
FMRadioCache gFmRadioCache;

// RDA5807M radio object (mathertel library)
static RDA5807M radio;
static RDSParser rds;
bool gRadioInitialized = false;

static volatile bool fmRadioInitRequested = false;
static volatile bool fmRadioInitDone = false;
static bool fmRadioInitResult = false;

// RDS callback to update station name (mutex-protected to prevent readers from
// seeing torn strings mid-write).
static void RDS_ServiceNameCallback(const char* name) {
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] RDS Station Name callback: '%s'", name ? name : "null");

  if (name != nullptr && strlen(name) > 0) {
    if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (strncmp(gFmRadioCache.stationName, name, 8) != 0) {
        DEBUG_FMRADIO_VALUESF("[FM_RADIO] Station name changed from '%s' to '%s'", gFmRadioCache.stationName, name);
        strncpy(gFmRadioCache.stationName, name, 8);
        gFmRadioCache.stationName[8] = '\0';
      }
      xSemaphoreGive(gFmRadioCache.mutex);
    }
  }
}

// RDS callback to update radio text (mutex-protected).
static void RDS_TextCallback(const char* text) {
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] RDS Text callback: '%s'", text ? text : "null");

  if (text != nullptr && strlen(text) > 0) {
    if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (strncmp(gFmRadioCache.stationText, text, 64) != 0) {
        DEBUG_FMRADIO_VALUESF("[FM_RADIO] Station text changed from '%s' to '%s'", gFmRadioCache.stationText, text);
        strncpy(gFmRadioCache.stationText, text, 64);
        gFmRadioCache.stationText[64] = '\0';
      }
      xSemaphoreGive(gFmRadioCache.mutex);
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

bool fmRadioInit() {
  INFO_FMRADIO_LIFECYCLEF("fmRadioInit() called - gFmRadioConnected=%s, gRadioInitialized=%s",
                gFmRadioConnected ? "true" : "false", gRadioInitialized ? "true" : "false");
  
  if (gFmRadioConnected && gRadioInitialized) {
    INFO_FMRADIO_LIFECYCLEF("FM Radio already initialized");
    return true;  // Already initialized
  }
  
  bool success = false;
  INFO_FMRADIO_LIFECYCLEF("Starting FM Radio I2C initialization");
  
  i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 1000, [&]() {
    DEBUG_FMRADIO_LIFECYCLEF("I2C transaction started, calling radio.initWire(Wire1)");
    // Initialize the radio with Wire1 (STEMMA QT bus)
    if (!radio.initWire(Wire1)) {
      ERROR_FMRADIOF("FM Radio initWire() failed - check I2C connections");
      return;  // Init failed
    }
    INFO_FMRADIO_LIFECYCLEF("FM Radio initWire() success - RDA5807M chip detected");
    radio.debugEnable(false);
    
    // Set band and initial frequency
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Setting band to FM and frequency to %.1f MHz", gFmRadioCache.frequency / 10.0);
    radio.setBandFrequency(RADIO_BAND_FM, gFmRadioCache.frequency);
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Band/frequency set successfully");
    
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Setting volume to %d/15", gFmRadioCache.volume);
    radio.setVolume(gFmRadioCache.volume);
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Volume set successfully");
    
    radio.setMono(false);  // Stereo mode
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Stereo mode enabled");
    
    radio.setMute(false);
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Unmuted - audio should be active");
    
    // Setup RDS callbacks
    radio.attachReceiveRDS(RDS_Process);
    rds.attachServiceNameCallback(RDS_ServiceNameCallback);
    rds.attachTextCallback(RDS_TextCallback);
    
    gRadioInitialized = true;
    gFmRadioConnected = true;
    success = true;
  });
  
  if (success) {
    INFO_FMRADIO_LIFECYCLEF("FM Radio initialized successfully - RDA5807M ready at %.1f MHz, volume %d",
                  gFmRadioCache.frequency / 100.0, gFmRadioCache.volume);
    sensorStatusBumpWith("fmradio initialized");
  } else {
    ERROR_FMRADIOF("FM Radio initialization failed - I2C mutex acquisition failed or transaction error");
  }
  return success;
}

void fmRadioDeinit() {
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] fmRadioDeinit() called - gRadioInitialized=%s", gRadioInitialized ? "true" : "false");
  
  if (gRadioInitialized) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Starting I2C transaction for deinitialization");
    i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 500, [&]() {
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Muting radio before termination");
      radio.setMute(true);
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Calling radio.term() to power down chip");
      radio.term();
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Radio terminated successfully");
    });
  } else {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Radio not initialized, skipping termination");
  }
  
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Resetting all state variables");
  gRadioInitialized = false;
  gFmRadioConnected = false;
  gFmRadioEnabled = false;
  memset(gFmRadioCache.stationName, 0, sizeof(gFmRadioCache.stationName));
  memset(gFmRadioCache.stationText, 0, sizeof(gFmRadioCache.stationText));
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Deinitialization completed");
}

// ============================================================================
// FM Radio Task Implementation
// ============================================================================

void fmRadioTask(void* parameter) {
  INFO_FMRADIO_LIFECYCLEF("FM Radio task started (handle=%p)", gFmRadioTaskHandle);
  unsigned long lastStackLog = 0;
  unsigned long loopCount = 0;
  bool initWatermarkLogged = false;
  
  while (true) {
    loopCount++;

    // Deferred initialization on the FM radio task stack (keeps sensor_queue stack safe)
    if (gFmRadioEnabled && !gRadioInitialized && fmRadioInitRequested) {
      INFO_FMRADIO_LIFECYCLEF("Performing deferred FM Radio init on task stack");
      bool ok = fmRadioInit();
      fmRadioInitResult = ok;
      fmRadioInitDone = true;
      fmRadioInitRequested = false;

      if (!ok) {
        ERROR_FMRADIOF("FM Radio fmRadioInit() failed");
        broadcastOutput("FM Radio init failed");
        sensorStatusBumpWith("fmradio@init_failed");
        gFmRadioEnabled = false;
        // Loop will delete the task on next iteration
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      // Unmute now that init succeeded
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Unmuting radio for audio output");
      i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
        radio.setMute(false);
        gFmRadioCache.muted = false;
        DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Radio unmuted successfully");
      });
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] FM Radio started successfully at %.1f MHz", gFmRadioCache.frequency / 100.0);
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
          DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Post-init stack watermark: %u bytes (%.1f%% used of %u bytes)",
                         (unsigned)(watermark * 4),
                         (float)(fmRadioStackWords - watermark) * 100.0f / (float)fmRadioStackWords,
                         (unsigned)(fmRadioStackWords * 4));
        }
      }
    }
    
    // Check if radio is disabled - delete task if so
    if (!gFmRadioEnabled) {
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Radio disabled, deleting task (loop %lu)", loopCount);
      // NOTE: Do NOT clear gFmRadioTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }
    
    // Skip polling if sensor polling is paused (to avoid I2C bus conflicts)
    if (gSensorPollingPaused) {
      if (loopCount % 20 == 0) {  // Log every 10 seconds when paused
        DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Sensor polling paused, waiting (loop %lu)", loopCount);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    // Update FM radio data (RDS, signal strength, headphone detection)
    if (loopCount % 4 == 0) {  // Log every 1 second during normal operation
      DEBUG_FMRADIO_POLLINGF("[FM_RADIO_TASK] Updating radio data (loop %lu)", loopCount);
    }
    updateFMRadio();
    
    // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
    if (shouldStreamSensorToRemote()) {
      char fmJson[512];
      int jsonLen = fmRadioBuildDataJSON(fmJson, sizeof(fmJson));
      if (jsonLen > 0) {
        sendSensorDataUpdate(REMOTE_SENSOR_FMRADIO, fmJson, jsonLen);
      }
    }
#endif
    
    // Log stack usage every 30 seconds (for debugging stack overflow issues)
    unsigned long now = millis();
    if (now - lastStackLog > 30000) {
      lastStackLog = now;
      if (checkTaskStackSafety("fmradio", FMRADIO_STACK_WORDS, &gFmRadioEnabled)) break;
      if (isDebugFlagSet(DEBUG_FMRADIO)) {
        const uint32_t fmRadioStackWords = FMRADIO_STACK_WORDS;
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_FMRADIO_POLLINGF("[FM_RADIO_TASK] Stack watermark: %u bytes (%.1f%% used of %u bytes)",
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

bool fmRadioStartInternal() {
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] fmRadioStartInternal() called - gFmRadioEnabled=%s", gFmRadioEnabled ? "true" : "false");

  if (gFmRadioEnabled) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Already enabled, skipping initialization");
    return true;
  }

  // Check memory before creating FM Radio task
  if (!checkMemoryAvailable("fmradio", nullptr)) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Insufficient memory for FM Radio sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gFmRadioCache.mutex) {
    gFmRadioCache.mutex = xSemaphoreCreateMutex();
    if (!gFmRadioCache.mutex) {
      ERROR_FMRADIOF("Failed to create cache mutex");
      return false;
    }
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Cache mutex created");
  }

  // Enable first, then let fmradio_task perform initialization on its own stack
  gFmRadioEnabled = true;
  fmRadioInitRequested = true;
  fmRadioInitDone = false;
  fmRadioInitResult = false;
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Radio enabled flag set; init will run on fmradio_task");

  // Create FM Radio task if not already running
  if (gFmRadioTaskHandle == nullptr) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Creating FM Radio task...");
    if (!createFMRadioTask()) {
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] ERROR: Failed to create FM Radio task");
      gFmRadioEnabled = false;
      return false;
    }
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] FM Radio task created successfully (handle=%p)", gFmRadioTaskHandle);
  } else {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] FM Radio task already running (handle=%p)", gFmRadioTaskHandle);
  }
  return true;
}

void fmRadioStopInternal() {
  // Note: gFmRadioEnabled is set to false by handleDeviceStopped() before this is called
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] fmRadioStopInternal() called - hardware cleanup");
  
  // Properly deinitialize the radio hardware
  if (gRadioInitialized) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Calling fmRadioDeinit() to shut down hardware");
    fmRadioDeinit();
  } else {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Radio not initialized, skipping hardware shutdown");
  }
  
  gFmRadioCache.muted = true;
  
  broadcastOutput("FM Radio stopped");
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] FM Radio stopped successfully");
}

// ============================================================================
// FM Radio Polling (for RDS updates)
// ============================================================================

void updateFMRadio() {
  static unsigned long lastUpdateLog = 0;
  static int lastRSSI = -999;
  static bool lastStereo = false;
  
  if (!gRadioInitialized || !gFmRadioEnabled) {
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Skipping update - radio not ready");
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
      gFmRadioCache.rssi = ri.rssi;
      gFmRadioCache.stereo = ri.stereo;
      gFmRadioCache.snr = ri.snr;
      
      // Only log when signal changes significantly (reduces flood)
      unsigned long now = millis();
      if (abs(gFmRadioCache.rssi - lastRSSI) >= 2 || gFmRadioCache.stereo != lastStereo || (now - lastUpdateLog > 30000)) {
        DEBUG_FMRADIO_VALUESF("[FM_RADIO] Signal: RSSI=%d, SNR=%d, Stereo=%s",
                       gFmRadioCache.rssi, gFmRadioCache.snr, gFmRadioCache.stereo ? "true" : "false");
        lastRSSI = gFmRadioCache.rssi;
        lastStereo = gFmRadioCache.stereo;
        lastUpdateLog = now;
      }
      
      // Update headphone detection based on RSSI
      gFmRadioCache.headphonesConnected = (gFmRadioCache.rssi >= 15);
    });
    return true;  // Assume success for void operation
  });
}

// ============================================================================
// Command Handlers
// ============================================================================

const char* cmd_fmradio(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Command received: '%s'", argsInput.c_str());
  
  // Parse subcommand
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    // No subcommand - show status
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] No subcommand, showing status");
    return cmd_fmradio_status(argsInput);
  }

  String subCmd = a.arg(0);
  subCmd.toLowerCase();
  String subArgs = a.remaining(0);
  
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Parsed subcommand: '%s'", subCmd.c_str());
  
  if (subCmd == "start") {
    return cmd_fmradio_start(subArgs);
  } else if (subCmd == "stop") {
    return cmd_fmradio_stop(subArgs);
  } else if (subCmd == "tune") {
    return cmd_fmradio_tune(subArgs);
  } else if (subCmd == "seek") {
    return cmd_fmradio_seek(subArgs);
  } else if (subCmd == "volume" || subCmd == "vol") {
    return cmd_fmradio_volume(subArgs);
  } else if (subCmd == "mute") {
    return cmd_fmradio_mute(subArgs);
  } else if (subCmd == "status") {
    return cmd_fmradio_status(subArgs);
  }
  
  DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Unknown subcommand: '%s'", subCmd.c_str());
  return "Usage: fmradio [start|stop|tune <freq>|seek [up|down]|volume <0-15>|mute|status]";
}

const char* cmd_fmradio_start(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gFmRadioEnabled) {
    return "FM Radio already running";
  }

  if (isInQueue(I2C_DEVICE_FMRADIO)) {
    return "FM Radio already queued";
  }

  if (!i2cPingAddress(I2C_ADDR_FM_RADIO, 100000, 50)) {
    return "[FM Radio] Not detected on I2C bus";
  }

  // Queue the FM radio start request
  enqueueDeviceStart(I2C_DEVICE_FMRADIO);
  
  return "FM Radio start queued";
}

const char* cmd_fmradio_stop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gFmRadioEnabled) {
    return "FM Radio not running";
  }
  
  handleDeviceStopped(I2C_DEVICE_FMRADIO);
  fmRadioStopInternal();  // Sensor-specific: hardware deinit
  sensorStatusBumpWith("fmradio stopped");
  
  return "OK";
}

const char* cmd_fmradio_tune(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse frequency: "103.9" or "10390"
  String freqStr = argsInput;
  freqStr.trim();
  
  if (freqStr.length() == 0) {
    return "Usage: fmradio tune <frequency> (e.g., 103.9 or 10390)";
  }
  
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
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    if (!fmRadioInit()) {
      return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
    }
  }
  
  i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 500, [&]() {
    radio.setFrequency(freqInt);
    gFmRadioCache.frequency = freqInt;
    
    // Clear RDS data on frequency change
    memset(gFmRadioCache.stationName, 0, sizeof(gFmRadioCache.stationName));
    memset(gFmRadioCache.stationText, 0, sizeof(gFmRadioCache.stationText));
  });
  
  BROADCAST_PRINTF("Tuned to %.1f MHz", freqInt / 100.0);
  return "OK";
}

const char* cmd_fmradio_seek(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
  }
  
  bool seekUp = true;  // Default seek up
  
  String dir = argsInput;
  dir.trim();
  dir.toLowerCase();
  if (dir == "down") {
    seekUp = false;
  }
  
  i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 6000, [&]() {
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
    
    gFmRadioCache.frequency = radio.getFrequency();
    
    // Clear RDS data on frequency change
    memset(gFmRadioCache.stationName, 0, sizeof(gFmRadioCache.stationName));
    memset(gFmRadioCache.stationText, 0, sizeof(gFmRadioCache.stationText));
  });
  
  BROADCAST_PRINTF("Seeked %s to %.1f MHz", seekUp ? "up" : "down", gFmRadioCache.frequency / 100.0);
  return "OK";
}

const char* cmd_fmradio_volume(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse volume: "8"
  String volStr = argsInput;
  volStr.trim();
  
  if (volStr.length() == 0) {
    BROADCAST_PRINTF("Current volume: %d", gFmRadioCache.volume);
    return "OK";
  }
  
  int vol = volStr.toInt();
  
  if (vol < 0 || vol > 15) {
    return "[FM Radio] Error: Volume must be 0-15";
  }
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    gFmRadioCache.volume = vol;  // Store for when radio starts
    BROADCAST_PRINTF("Volume set to %d (will apply when radio starts)", vol);
    return "OK";
  }
  
  i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setVolume(vol);
    gFmRadioCache.volume = vol;
  });
  
  BROADCAST_PRINTF("Volume set to %d", vol);
  
  notifyVolumeChanged(vol, 15);
  
  return "OK";
}

const char* cmd_fmradio_mute(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    return "[FM Radio] Error: Not initialized - use 'fmradio start' first";
  }
  
  // Check if this is "mute" or "unmute" command
  String arg = argsInput;
  arg.trim();
  arg.toLowerCase();
  bool shouldMute = (arg != "off" && arg != "unmute");  // Default to mute unless explicitly unmute
  gFmRadioCache.muted = shouldMute;
  
  i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setMute(gFmRadioCache.muted);
  });
  
  BROADCAST_PRINTF("FM Radio %s", gFmRadioCache.muted ? "muted" : "unmuted");
  return "OK";
}

const char* cmd_fmradio_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation
  broadcastOutput("FM Radio Status:");
  BROADCAST_PRINTF("  Connected: %s", gFmRadioConnected ? "Yes" : "No");
  BROADCAST_PRINTF("  Enabled: %s", gFmRadioEnabled ? "Yes" : "No");
  
  if (!gFmRadioConnected) {
    BROADCAST_PRINTF("  Stored Frequency: %.1f MHz", gFmRadioCache.frequency / 100.0);
    BROADCAST_PRINTF("  Stored Volume: %d", gFmRadioCache.volume);
  } else {
    // Update signal info
    if (gRadioInitialized) {
      i2cDeviceTransactionVoid(I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
        RADIO_INFO ri;
        radio.getRadioInfo(&ri);
        gFmRadioCache.rssi = ri.rssi;
        gFmRadioCache.stereo = ri.stereo;
      });
    }
    
    BROADCAST_PRINTF("  Frequency: %.1f MHz", gFmRadioCache.frequency / 100.0);
    BROADCAST_PRINTF("  Volume: %d/15", gFmRadioCache.volume);
    BROADCAST_PRINTF("  Muted: %s", gFmRadioCache.muted ? "Yes" : "No");
    BROADCAST_PRINTF("  Stereo: %s", gFmRadioCache.stereo ? "Yes" : "No");
    BROADCAST_PRINTF("  RSSI: %d", gFmRadioCache.rssi);
    BROADCAST_PRINTF("  Headphones: %s", gFmRadioCache.headphonesConnected ? "Yes" : "No");
    BROADCAST_PRINTF("  Station: %s", strlen(gFmRadioCache.stationName) > 0 ? gFmRadioCache.stationName : "(none)");
    BROADCAST_PRINTF("  Radio Text: %s", strlen(gFmRadioCache.stationText) > 0 ? gFmRadioCache.stationText : "(none)");
  }
  
  return "OK";
}

// ============================================================================
// JSON Data Builder (for web API)
// ============================================================================

int fmRadioBuildDataJSON(char* buf, size_t bufSize) {
  // Take a consistent snapshot under the mutex so RDS callbacks can't tear
  // strings mid-serialize and callers get coherent values.
  FMRadioCache snap;
  if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = gFmRadioCache;  // value copy
    xSemaphoreGive(gFmRadioCache.mutex);
  } else {
    // Best-effort: mutex unavailable or contended. Read directly; strings may
    // tear in rare races but values stay in valid ranges.
    snap = gFmRadioCache;
  }

  int len = snprintf(buf, bufSize,
    "{\"connected\":%s,\"enabled\":%s,\"frequency\":%.1f,\"volume\":%d,"
    "\"muted\":%s,\"stereo\":%s,\"rssi\":%d,\"headphones\":%s,\"station\":\"%s\",\"radioText\":\"%s\"}",
    gFmRadioConnected ? "true" : "false",
    gFmRadioEnabled ? "true" : "false",
    snap.frequency / 100.0,
    snap.volume,
    snap.muted ? "true" : "false",
    snap.stereo ? "true" : "false",
    snap.rssi,
    snap.headphonesConnected ? "true" : "false",
    snap.stationName,
    snap.stationText);
  return len;
}

// ============================================================================
// Command Registration
// ============================================================================

const char* cmd_fmradioautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.fmRadioAutoStart ? "[FM Radio] Auto-start: enabled" : "[FM Radio] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.fmRadioAutoStart, true);
    return "[FM Radio] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.fmRadioAutoStart, false);
    return "[FM Radio] Auto-start disabled";
  }
  return "Usage: fmradioautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry fmRadioCommands[] = {
  // 3-level voice: "sensor" -> "radio" -> "open/close"
  { "openfmradio", "Start FM Radio sensor.", false, cmd_fmradio_start, nullptr, "sensor", "radio", "open" },
  { "closefmradio", "Stop FM Radio sensor.", false, cmd_fmradio_stop, nullptr, "sensor", "radio", "close" },
  { "fmradioread", "Read FM Radio status.", false, cmd_fmradio_status },
  { "fmradiotune", "Tune to frequency: <freq>", false, cmd_fmradio_tune, "Usage: fmradiotune <frequency> (e.g., 103.9 or 10390)" },
  { "fmradioseek", "Seek next station [up|down]", false, cmd_fmradio_seek },
  { "fmradiovolume", "Set volume: <0-15>", false, cmd_fmradio_volume, "Usage: fmradiovolume <0-15>" },
  { "fmradiomute", "Mute audio", false, cmd_fmradio_mute },
  { "fmradiounmute", "Unmute audio", false, cmd_fmradio_mute },
  
  // Auto-start
  { "fmradioautostart", "Enable/disable FM Radio auto-start after boot [on|off]", false, cmd_fmradioautostart, "Usage: fmradioautostart [on|off]" },
};

const size_t fmRadioCommandsCount = sizeof(fmRadioCommands) / sizeof(fmRadioCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// FM Radio Modular Settings Registration (for safety and reliability)
// ============================================================================

// FM Radio settings entries
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry fmRadioSettingEntries[] = {
  { "fmRadioAutoStart", SETTING_BOOL, &gSettings.fmRadioAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr },
  { "fmRadioDevicePollMs", SETTING_INT, &gSettings.fmRadioDevicePollMs, 250, 0, nullptr, 100, 5000, "Poll Interval (ms)", nullptr, false, nullptr, nullptr }
};

static bool isFMRadioConnected() {
  return gFmRadioConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule fmRadioSettingsModule = {
  "fmradio",
  "hardware.sensors.fmradio",
  fmRadioSettingEntries,
  sizeof(fmRadioSettingEntries) / sizeof(fmRadioSettingEntries[0]),
  isFMRadioConnected,
  "RDA5807 FM radio receiver"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// FM Radio OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-rda5807-oled.h"
#endif

#endif // ENABLE_FM_RADIO
