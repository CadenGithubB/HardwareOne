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

#include "i2csensor_rda5807.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"
#include "System_Utils.h"
#include "System_TaskUtils.h"

#if ENABLE_FM_RADIO

#include <Arduino.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <freertos/portmacro.h>
#include <radio.h>
#include <RDA5807M.h>
#include <RDSParser.h>
#include <Wire.h>

#include "OLED_Display.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"  // oledNotificationBannerShow — direct volume banner
#endif
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"


// The RDA5807 used to be hardcoded to Wire1; with dual-bus it lives on
// whatever gSettings.fmRadioBus selects (default bus 0 = Wire1). The
// mathertel Radio library binds the TwoWire at initWire(port&), so we
// pass *myWire at fmRadioInit. All transactions also pass fmRadioBus so
// the bus-aware helpers acquire the right mutex/clock.
extern TwoWire Wire1;

// Resolve which bus the FM radio is on. Returns false if unavailable
// (e.g., fmRadioBus=1 but i2c2Enabled=false).
static bool fmRadioResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.fmRadioBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

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

bool gFmRadioRunning = false;
bool gFmRadioConnected = false;
TaskHandle_t gFmRadioTaskHandle = nullptr;
unsigned long gFmRadioLastStopTime = 0;

// Fixed-size reading storage. Critical sections copy/compare data only; radio
// I/O, allocation and presentation always happen outside this short lock.
static FMRadioCache sFmRadioCache;
static portMUX_TYPE sFmRadioCacheMux = portMUX_INITIALIZER_UNLOCKED;
struct FmRadioCacheGuard {
  FmRadioCacheGuard() { portENTER_CRITICAL(&sFmRadioCacheMux); }
  ~FmRadioCacheGuard() { portEXIT_CRITICAL(&sFmRadioCacheMux); }
};

bool fmRadioCacheSnapshot(FMRadioCache& out, uint32_t timeoutMs) {
  (void)timeoutMs;  // Compatibility with other sensor snapshot interfaces.
  FmRadioCacheGuard guard;
  out = sFmRadioCache;
  return true;
}

static void fmRadioSetMuted(bool muted) {
  FmRadioCacheGuard guard;
  sFmRadioCache.muted = muted;
}

static void fmRadioInvalidate() {
  FmRadioCacheGuard guard;
  sFmRadioCache.seekInProgress = false;
  sFmRadioCache.dataValid = false;
  sFmRadioCache.muted = true;
  memset(sFmRadioCache.stationName, 0, sizeof(sFmRadioCache.stationName));
  memset(sFmRadioCache.stationText, 0, sizeof(sFmRadioCache.stationText));
}

// RDA5807M radio object (mathertel library)
static RDA5807M radio;
static RDSParser rds;
bool gRadioInitialized = false;

static volatile bool fmRadioInitRequested = false;
static volatile bool fmRadioInitDone = false;
static bool fmRadioInitResult = false;

// Scrub RDS text to printable ASCII before it enters the cache.
//
// RDS bytes are broadcaster-controlled AND unvalidated on the way in: the
// library stores raw octets straight off the wire (RDSParser.cpp — RadioText
// gets no stability vote at all, unlike the PS name), and the driver's
// block-error check is commented out (RDA5807M.cpp checkRDS — RADIO_REG_RA_RDSBLOCK
// is defined but never tested). So ANY octet 0x00-0xFF can land here on a weak or
// multipath signal, without needing a hostile broadcaster.
//
// stationName/stationText are the ONLY remote-controlled string fields in any
// sensor envelope builder, and fmRadioBuildDataJSON prints them with a bare %s
// inside JSON string literals. An unescaped '"' does not merely corrupt the
// payload — radioText is the last field before the closing brace, so 64 chars are
// enough to close the string and append well-formed keys, overriding the
// envelope's own `valid`. Downstream that silently drops the whole fmradio
// reading from `sensors json` (addSensorEntry's deserialize fails), poisons the
// spliced ESP-NOW remote-sensor API response, and freezes the FM web panel.
//
// Scrubbing on INGEST (rather than escaping at the builder) fixes every consumer
// at one point: the JSON, the G2/OLED lens render which prints the cache with %s,
// and the SYSEVT_FM_RDS_STATION event subject. Mirrors sanitizeField() in
// System_Events.cpp. RDS is display text, so a space is a faithful substitute.
static void rdsScrub(char* dst, const char* src, size_t maxChars) {
  size_t i = 0;
  for (; src[i] && i < maxChars; i++) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = (c < 0x20 || c > 0x7E || c == '"' || c == '\\') ? ' ' : (char)c;
  }
  dst[i] = '\0';
}

// RDS callback to update station name (protected to prevent readers from
// seeing torn strings mid-write).
static void RDS_ServiceNameCallback(const char* name) {
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] RDS Station Name callback: '%s'", name ? name : "null");
  if (!name || !name[0]) return;
  char clean[sizeof(sFmRadioCache.stationName)]{};
  char previous[sizeof(sFmRadioCache.stationName)]{};
  rdsScrub(clean, name, sizeof(clean) - 1);
  uint16_t frequency = 0;
  {
    FmRadioCacheGuard guard;
    if ( strcmp(sFmRadioCache.stationName, clean) == 0) return;
    memcpy(previous, sFmRadioCache.stationName, sizeof(previous));
    memcpy(sFmRadioCache.stationName, clean, sizeof(clean));
    frequency = sFmRadioCache.frequency;
  }
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] Station name changed from '%s' to '%s'", previous, clean);
  char freq[16];
  snprintf(freq, sizeof(freq), "%.1f MHz", frequency / 100.0);
  systemEventPost(SYSEVT_FM_RDS_STATION, clean, freq);
}

// RDS callback to update radio text (fixed-size critical copy).
static void RDS_TextCallback(const char* text) {
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] RDS Text callback: '%s'", text ? text : "null");
  if (!text || !text[0]) return;
  char clean[sizeof(sFmRadioCache.stationText)]{};
  char previous[sizeof(sFmRadioCache.stationText)]{};
  rdsScrub(clean, text, sizeof(clean) - 1);
  {
    FmRadioCacheGuard guard;
    if ( strcmp(sFmRadioCache.stationText, clean) == 0) return;
    memcpy(previous, sFmRadioCache.stationText, sizeof(previous));
    memcpy(sFmRadioCache.stationText, clean, sizeof(clean));
  }
  DEBUG_FMRADIO_VALUESF("[FM_RADIO] Station text changed from '%s' to '%s'", previous, clean);
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
  
  FMRadioCache reading;
  if (!fmRadioCacheSnapshot(reading, 50)) return false;
  bool success = false;
  INFO_FMRADIO_LIFECYCLEF("Starting FM Radio I2C initialization");

  // Resolve which bus the FM radio is on. The radio library binds the
  // TwoWire at initWire(), so we must pass *myWire here. Bail closed if
  // the bus isn't initialized.
  uint8_t fmBus; TwoWire* fmWire;
  if (!fmRadioResolveBus(&fmBus, &fmWire)) {
    ERROR_FMRADIOF("FM Radio: bus %d not initialized", gSettings.fmRadioBus);
    return false;
  }

  i2cDeviceTransactionVoid(fmBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 1000, [&]() {
    DEBUG_FMRADIO_LIFECYCLEF("I2C transaction started, calling radio.initWire on bus %u", fmBus);
    // Initialize the radio with the resolved TwoWire reference for the
    // configured bus (dereferenced — initWire takes TwoWire&).
    if (!radio.initWire(*fmWire)) {
      ERROR_FMRADIOF("FM Radio initWire() failed - check I2C connections");
      return;  // Init failed
    }
    INFO_FMRADIO_LIFECYCLEF("FM Radio initWire() success - RDA5807M chip detected on bus %u", fmBus);
    radio.debugEnable(false);

    // Set band and initial frequency
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Setting band to FM and frequency to %.1f MHz", reading.frequency / 10.0);
    radio.setBandFrequency(RADIO_BAND_FM, reading.frequency);
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Band/frequency set successfully");
    
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Setting volume to %d/15", reading.volume);
    radio.setVolume(reading.volume);
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
                  reading.frequency / 100.0, reading.volume);
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
    i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 500, [&]() {
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
  gFmRadioRunning = false;
  fmRadioInvalidate();
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
    if (gFmRadioRunning && !gRadioInitialized && fmRadioInitRequested) {
      INFO_FMRADIO_LIFECYCLEF("Performing deferred FM Radio init on task stack");
      bool ok = fmRadioInit();
      fmRadioInitResult = ok;
      fmRadioInitDone = true;
      fmRadioInitRequested = false;

      if (!ok) {
        ERROR_FMRADIOF("FM Radio fmRadioInit() failed");
        broadcastOutput("FM Radio init failed");
        sensorStatusBumpWith("fmradio@init_failed");
        gFmRadioRunning = false;
        // Loop will delete the task on next iteration
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      // fmRadioInit already unmutes. A stop or timeout during initialization
      // must proceed directly to task-owned teardown, never announce a start.
      if (!gFmRadioRunning) continue;
      fmRadioSetMuted(false);

      // One-time watermark log right after init completes (captures peak without waiting 30s)
      if (!initWatermarkLogged) {
        initWatermarkLogged = true;
        if (isDebugFlagSet(DEBUG_FMRADIO)) {
          constexpr uint32_t fmRadioStackBytes = FMRADIO_STACK_WORDS;  // BYTES (misnomer name); single source of truth
          UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
          DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Post-init stack watermark: %u bytes (%.1f%% used of %u bytes)",
                         (unsigned)watermark,
                         (float)(fmRadioStackBytes - watermark) * 100.0f / (float)fmRadioStackBytes,
                         (unsigned)fmRadioStackBytes);
        }
      }
    }
    
    // Check if radio is disabled - delete task if so
    if (!gFmRadioRunning) {
      fmRadioDeinit();
      fmRadioInitRequested = false;
      fmRadioInitDone = false;
      fmRadioInitResult = false;
      DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO_TASK] Radio disabled, deleting task (loop %lu)", loopCount);
      SENSOR_TASK_EXIT(FMRADIO, gFmRadioTaskHandle);
    }
    
    // Skip polling if sensor polling is paused (to avoid I2C bus conflicts)
    if (pollPaused((uint8_t)gSettings.fmRadioBus)) {
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

    // ESP-NOW broadcaster reads fmRadioBuildDataJSON() on demand from sFmRadioCache.
    
    // Log stack usage every 30 seconds (for debugging stack overflow issues)
    unsigned long now = millis();
    if (now - lastStackLog > 30000) {
      lastStackLog = now;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // vTaskDelete path instead of returning from the task function with a
      // near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("fmradio", FMRADIO_STACK_WORDS, &gFmRadioRunning)) continue;
      if (isDebugFlagSet(DEBUG_FMRADIO)) {
        const uint32_t fmRadioStackBytes = FMRADIO_STACK_WORDS;  // BYTES (misnomer name); single source of truth
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_FMRADIO_POLLINGF("[FM_RADIO_TASK] Stack watermark: %u bytes (%.1f%% used of %u bytes)",
                       (unsigned)watermark,
                       (float)(fmRadioStackBytes - watermark) * 100.0f / (float)fmRadioStackBytes,
                       (unsigned)fmRadioStackBytes);
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
  if (gFmRadioRunning) return true;
  // The old task must finish deinitializing before a new task can start.
  if (gFmRadioTaskHandle != nullptr) return false;
  if (!checkMemoryAvailable("fmradio", nullptr)) return false;

  // Initialization runs on the radio task's stack; the worker waits for its
  // existing handshake before reporting the lifecycle result.
  fmRadioInitRequested = !gRadioInitialized || !gFmRadioConnected;
  fmRadioInitDone = !fmRadioInitRequested;
  fmRadioInitResult = !fmRadioInitRequested;
  gFmRadioRunning = true;
  if (!createFMRadioTask()) {
    gFmRadioRunning = false;
    fmRadioInitRequested = false;
    fmRadioInitDone = false;
    fmRadioInitResult = false;
    fmRadioDeinit();
    return false;
  }
  const unsigned long startedAt = millis();
  while (gFmRadioRunning && !fmRadioInitDone && millis() - startedAt < 3000UL) {
    delay(10);
  }
  if (!gFmRadioRunning || !fmRadioInitDone || !fmRadioInitResult) {
    // The task owns cleanup, including an init that finishes after this timeout.
    gFmRadioRunning = false;
    return false;
  }
  return true;
}


// ============================================================================
// FM Radio Polling (for RDS updates)
// ============================================================================

void updateFMRadio() {
  static unsigned long lastUpdateLog = 0;
  static int lastRSSI = -999;
  static bool lastStereo = false;
  if (!gRadioInitialized || !gFmRadioRunning || pollPaused((uint8_t)gSettings.fmRadioBus)) return;

  RADIO_INFO ri{};
  bool read = false, finalized = false, timedOut = false;
  uint16_t tunedFrequency = 0;
  unsigned long now = 0;
  // Hardware reads and their cache commits share bus ordering with commands.
  // The cache critical section never spans a radio call or output formatting.
  i2cTransactionNACKTolerant((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO,
                           FM_RADIO_I2C_CLOCK, 100, [&]() {
    radio.checkRDS();
    radio.getRadioInfo(&ri);
    FMRadioCache before;
    fmRadioCacheSnapshot(before);
    now = millis();
    bool seekFinished = false;
    if (before.seekInProgress) {
      const unsigned long elapsed = now - before.seekStartMs;
      timedOut = elapsed > 6000;
      seekFinished = (elapsed >= 300 && ri.tuned) || timedOut;
      if (seekFinished) tunedFrequency = radio.getFrequency();
    }
    {
      FmRadioCacheGuard guard;
      if (!gRadioInitialized || !gFmRadioRunning) return;
      sFmRadioCache.rssi = ri.rssi;
      sFmRadioCache.stereo = ri.stereo;
      sFmRadioCache.snr = ri.snr;
      if (seekFinished && sFmRadioCache.seekInProgress) {
        sFmRadioCache.frequency = tunedFrequency;
        memset(sFmRadioCache.stationName, 0, sizeof(sFmRadioCache.stationName));
        memset(sFmRadioCache.stationText, 0, sizeof(sFmRadioCache.stationText));
        sFmRadioCache.seekInProgress = false;
        finalized = true;
      }
      sFmRadioCache.headphonesConnected = ri.rssi >= 15;
      sFmRadioCache.lastUpdate = now;
      sFmRadioCache.dataValid = true;
    }
    read = true;
  });
  if (!read) return;
  if (finalized) {
    char mhz[12];
    snprintf(mhz, sizeof(mhz), "%.1f", tunedFrequency / 100.0);
    systemEventPost(SYSEVT_FM_TUNED, mhz, timedOut ? "seek timeout" : "seek");
    DEBUG_FMRADIO_LIFECYCLEF("[FM_RADIO] Seek finalized at %s MHz%s", mhz, timedOut ? " (timeout)" : "");
  }
  if (abs(ri.rssi - lastRSSI) >= 2 || ri.stereo != lastStereo || now - lastUpdateLog > 30000) {
    DEBUG_FMRADIO_VALUESF("[FM_RADIO] Signal: RSSI=%d, SNR=%d, Stereo=%s", ri.rssi, ri.snr, ri.stereo ? "true" : "false");
    lastRSSI = ri.rssi;
    lastStereo = ri.stereo;
    lastUpdateLog = now;
  }
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
  return "Error: invalid arguments — Usage: fmradio [start|stop|tune <freq>|seek [up|down]|volume <0-15>|mute|status]";
}

const char* cmd_fmradio_start(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* result = cmd_sensorstart_queued(I2C_DEVICE_FMRADIO, "FM Radio", "openfmradio@enqueue");
  cliHint("the radio powers up in the background - check it with 'fmradioread', then tune with 'fmradiotune <freq>'");
  return result;
}

const char* cmd_fmradio_stop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!requestSensorStop(I2C_DEVICE_FMRADIO)) return "Error: [FM Radio] Could not queue stop";
  return "[FM Radio] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_fmradio_tune(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse frequency: "103.9" or "10390"
  String freqStr = argsInput;
  freqStr.trim();
  
  if (freqStr.length() == 0) {
    return "Error: invalid arguments — Usage: fmradio tune <frequency> (e.g., 103.9 or 10390)";
  }
  
  char* end = nullptr;
  errno = 0;
  const double freq = std::strtod(freqStr.c_str(), &end);
  if (end == freqStr.c_str() || end != freqStr.c_str() + freqStr.length() ||
      errno == ERANGE || !std::isfinite(freq)) {
    return "Error: [FM Radio] Expected a frequency in MHz or 10 kHz units";
  }
  const double freqUnits = freq < 200 ? freq * 100 : freq;
  // Validate before narrowing or initializing hardware. Round to 10 kHz units
  // so decimal MHz input does not lose a unit through floating-point truncation.
  if (freqUnits < 7600 || freqUnits > 10800) {
    return "Error: [FM Radio] Frequency must be 76.0-108.0 MHz";
  }
  const uint16_t freqInt = static_cast<uint16_t>(std::lround(freqUnits));
  
  if (!gFmRadioRunning && gFmRadioTaskHandle != nullptr) {
    return "Error: [FM Radio] Cleanup pending - retry shortly";
  }
  if (!gFmRadioConnected || !gRadioInitialized) {
    if (!fmRadioInit()) {
      return "Error: [FM Radio] Not initialized - use 'fmradio start' first";
    }
  }
  
  // The library's setters return void: issued means the driver call ran, not
  // that the chip acknowledged the write or has finished tuning.
  bool issued = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 500, [&]() {
    radio.setFrequency(freqInt);
    FmRadioCacheGuard guard;
    sFmRadioCache.frequency = freqInt;
    memset(sFmRadioCache.stationName, 0, sizeof(sFmRadioCache.stationName));
    memset(sFmRadioCache.stationText, 0, sizeof(sFmRadioCache.stationText));
    issued = true;
  });
  if (!issued) return "Error: [FM Radio] Tune command could not run";

  char mhz[12];
  snprintf(mhz, sizeof(mhz), "%.1f", freqInt / 100.0);
  systemEventPost(SYSEVT_FM_TUNED, mhz);

  BROADCAST_PRINTF("Tune command sent: %.1f MHz", freqInt / 100.0);
  return "OK";
}

const char* cmd_fmradio_seek(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String dir = argsInput;
  dir.trim();
  dir.toLowerCase();
  if (dir.length() && dir != "up" && dir != "down")
    return "Error: invalid arguments — Usage: fmradio seek [up|down]";
  if (!gFmRadioConnected || !gRadioInitialized)
    return "Error: [FM Radio] Not initialized - use 'fmradio start' first";
  const bool seekUp = dir != "down";
  uint16_t frequency = 0;
  bool issued = false, busy = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 500, [&]() {
    {
      FmRadioCacheGuard guard;
      if (sFmRadioCache.seekInProgress) { busy = true; return; }
      sFmRadioCache.seekInProgress = true;
      sFmRadioCache.seekDirUp = seekUp;
      frequency = sFmRadioCache.frequency;
      memset(sFmRadioCache.stationName, 0, sizeof(sFmRadioCache.stationName));
      memset(sFmRadioCache.stationText, 0, sizeof(sFmRadioCache.stationText));
    }
    if (seekUp) radio.seekUp(false);
    else radio.seekDown(false);
    const unsigned long startedAt = millis();
    {
      FmRadioCacheGuard guard;
      sFmRadioCache.seekStartMs = startedAt;
    }
    // Polling shares this bus, so it cannot finalize a reserved seek before
    // hardware starts and its arm-delay timestamp is committed.
    issued = true;
  });
  if (busy) return "Error: [FM Radio] Seek already in progress";
  if (!issued) return "Error: [FM Radio] Seek could not start";
  BROADCAST_PRINTF("Seeking %s from %.1f MHz...", seekUp ? "up" : "down", frequency / 100.0);
  return "OK";
}

const char* cmd_fmradio_volume(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse volume: "8"
  String volStr = argsInput;
  volStr.trim();
  
  if (volStr.length() == 0) {
    FMRadioCache reading;
    if (!fmRadioCacheSnapshot(reading, 50)) return "Error: [FM Radio] Snapshot unavailable";
    BROADCAST_PRINTF("Current volume: %d", reading.volume);
    return "OK";
  }
  
  char* end = nullptr;
  errno = 0;
  const long parsedVolume = std::strtol(volStr.c_str(), &end, 10);
  if (end == volStr.c_str() || end != volStr.c_str() + volStr.length() ||
      errno == ERANGE || parsedVolume < 0 || parsedVolume > 15) {
    return "Error: [FM Radio] Volume must be 0-15";
  }
  const int vol = static_cast<int>(parsedVolume);
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    {
      FmRadioCacheGuard guard;
      sFmRadioCache.volume = vol;
    }  // Store for when radio starts
    BROADCAST_PRINTF("Volume set to %d (will apply when radio starts)", vol);
    return "OK";
  }
  
  bool issued = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setVolume(vol);
    FmRadioCacheGuard guard;
    sFmRadioCache.volume = vol;
    issued = true;
  });
  if (!issued) return "Error: [FM Radio] Volume command could not run";

  BROADCAST_PRINTF("Volume command sent: %d", vol);
  
  // Volume feedback is UI, not a notification (reclassified in the
  // Phase-1 cutover) — render the banner directly.
#if ENABLE_OLED_DISPLAY
  {
    char vmsg[24];
    snprintf(vmsg, sizeof(vmsg), "Vol: %d/15", vol);
    oledNotificationBannerShow(vmsg, PairingRibbonIcon::INFO_ICON, 1500);
  }
#endif
  
  return "OK";
}

const char* cmd_fmradio_mute(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String arg = argsInput;
  arg.trim();
  arg.toLowerCase();
  if (arg.length() && arg != "on" && arg != "mute" && arg != "off" && arg != "unmute") {
    return "Error: invalid arguments — Usage: fmradio mute [on|off|mute|unmute]";
  }
  const bool shouldMute = arg != "off" && arg != "unmute";
  if (!gFmRadioConnected || !gRadioInitialized) {
    return "Error: [FM Radio] Not initialized - use 'fmradio start' first";
  }
  
  bool issued = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
    radio.setMute(shouldMute);
    fmRadioSetMuted(shouldMute);
    issued = true;
  });
  if (!issued) return "Error: [FM Radio] Mute command could not run";
  
  BROADCAST_PRINTF("FM Radio %s command sent", shouldMute ? "mute" : "unmute");
  return "OK";
}

// Dedicated unmute shares the same hardware operation and outcome handling.
const char* cmd_fmradio_unmute(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length()) {
    return "Error: invalid arguments — Usage: fmradiounmute";
  }
  return cmd_fmradio_mute("off");
}

const char* cmd_fmradio_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = fmRadioBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  if (gFmRadioConnected && gRadioInitialized) {
    RADIO_INFO ri{};
    i2cDeviceTransactionVoid((uint8_t)gSettings.fmRadioBus, I2C_ADDR_FM_RADIO, FM_RADIO_I2C_CLOCK, 200, [&]() {
      radio.getRadioInfo(&ri);
      FmRadioCacheGuard guard;
      sFmRadioCache.rssi = ri.rssi;
      sFmRadioCache.stereo = ri.stereo;
    });
  }
  FMRadioCache reading;
  if (!fmRadioCacheSnapshot(reading, 50)) return "Error: [FM Radio] Snapshot unavailable";
  broadcastOutput("FM Radio Status:");
  BROADCAST_PRINTF("  Connected: %s", gFmRadioConnected ? "Yes" : "No");
  BROADCAST_PRINTF("  Enabled: %s", gFmRadioRunning ? "Yes" : "No");
  if (!gFmRadioConnected) {
    BROADCAST_PRINTF("  Stored Frequency: %.1f MHz", reading.frequency / 100.0);
    BROADCAST_PRINTF("  Stored Volume: %d", reading.volume);
  } else {
    BROADCAST_PRINTF("  Frequency: %.1f MHz", reading.frequency / 100.0);
    BROADCAST_PRINTF("  Volume: %d/15", reading.volume);
    BROADCAST_PRINTF("  Muted: %s", reading.muted ? "Yes" : "No");
    BROADCAST_PRINTF("  Stereo: %s", reading.stereo ? "Yes" : "No");
    BROADCAST_PRINTF("  RSSI: %d", reading.rssi);
    BROADCAST_PRINTF("  Headphones: %s", reading.headphonesConnected ? "Yes" : "No");
    BROADCAST_PRINTF("  Station: %s", reading.stationName[0] ? reading.stationName : "(none)");
    BROADCAST_PRINTF("  Radio Text: %s", reading.stationText[0] ? reading.stationText : "(none)");
  }
  return "OK";
}

// ============================================================================
// JSON Data Builder (for web API)
// ============================================================================

int fmRadioBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || !bufSize) return 0;
  FMRadioCache snap;
  if (!fmRadioCacheSnapshot(snap, 50)) return 0;

  // valid = fresh poll data (set in updateFMRadio, cleared on deinit),
  // connected = radio present on the bus, ts from the last poll. Dropped:
  // top-level "connected" (envelope carries it) AND "enabled" (duplicate of the
  // sensors-json discovery-layer enabled).
  int pos = sensorEnvelopeBegin(buf, bufSize, snap.dataValid, gFmRadioConnected, snap.lastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
    ",\"frequency\":%.1f,\"volume\":%d,"
    "\"muted\":%s,\"stereo\":%s,\"rssi\":%d,\"headphones\":%s,\"seeking\":%s,\"station\":\"%s\",\"radioText\":\"%s\"}",
    snap.frequency / 100.0,
    snap.volume,
    snap.muted ? "true" : "false",
    snap.stereo ? "true" : "false",
    snap.rssi,
    snap.headphonesConnected ? "true" : "false",
    snap.seekInProgress ? "true" : "false",
    snap.stationName,
    snap.stationText);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
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
  return "Error: invalid arguments — Usage: fmradioautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_fmradiobus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.fmRadioBus, a, "fmRadioBus");
}

const CommandEntry fmRadioCommands[] = {
  // 3-level voice: "sensor" -> "radio" -> "open/close"
  { "openfmradio", "Start FM Radio sensor.", false, cmd_fmradio_start },
  { "closefmradio", "Stop FM Radio sensor.", false, cmd_fmradio_stop },
  { "fmradioread", "Read FM Radio status. (add 'json' for JSON output)", false, cmd_fmradio_status },
  { "fmradiotune", "Tune to frequency: <freq>", false, cmd_fmradio_tune, "Usage: fmradiotune <frequency> (e.g., 103.9 or 10390)" },
  { "fmradioseek", "Start seeking the next station [up|down] (async; 'fmradioread' shows the result)", false, cmd_fmradio_seek, "Usage: fmradioseek [up|down]" },
  { "fmradiovolume", "Set volume: <0-15>", false, cmd_fmradio_volume, "Usage: fmradiovolume <0-15>" },
  { "fmradiomute", "Mute audio", false, cmd_fmradio_mute },
  { "fmradiounmute", "Unmute audio", false, cmd_fmradio_unmute },
  
  // Auto-start
  { "fmradioautostart", "Enable/disable FM Radio auto-start after boot [on|off]", false, cmd_fmradioautostart, "Usage: fmradioautostart [on|off]" },
  { "fmradiobus",  "Route RDA5807 FM radio to bus: <0|1> (reboot required)", true, cmd_fmradiobus, "Usage: fmRadioBus <0|1>" },
};

const size_t fmRadioCommandsCount = sizeof(fmRadioCommands) / sizeof(fmRadioCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// FM Radio Modular Settings Registration (for safety and reliability)
// ============================================================================

// FM Radio settings entries
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry fmRadioSettingEntries[] = {
  { "fmRadioEnabled", SETTING_BOOL, &gSettings.fmRadioEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "fmradioenabled" },
  { "fmRadioAutoStart", SETTING_BOOL, &gSettings.fmRadioAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "fmradioautostart" },
  { "fmRadioDevicePollMs", SETTING_INT, &gSettings.fmRadioDevicePollMs, 250, 0, nullptr, 100, 5000, "Poll Interval (ms)", nullptr, false, nullptr, "fmradiodevicepollms" },
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
#include "i2csensor_rda5807_oled.h"
#endif

#endif // ENABLE_FM_RADIO
