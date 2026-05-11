#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include "System_BuildConfig.h"

// CommandEntry is defined in system_utils.h (included by files that need it)
// Forward declare here for header-only usage
struct CommandEntry;

// Settings structure - shared between .ino and .cpp files
struct Settings {
  // Constructor to ensure all String members are initialized
  Settings()
    : wifiSSID(""),
      wifiPassword(""),
      wifiEnabled(true),
      wifiAutoReconnect(true),
      webCliHistorySize(10),
      oledCliHistorySize(50),
      ntpServer("pool.ntp.org"),
      tzOffsetMinutes(0),
      outSerial(true),
      outWeb(false),
      outDisplay(false),
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
      outG2(false),
#endif
      thermalPollingMs(250),
      tofPollingMs(220),
      tofStabilityThreshold(3),
      thermalPaletteDefault("grayscale"),
      thermalInterpolationEnabled(true),
      thermalInterpolationSteps(5),
      thermalInterpolationBufferSize(2),
      thermalUpscaleFactor(1),
      thermalEWMAFactor(0.2f),
      thermalTransitionMs(80),
      tofTransitionMs(200),
      tofUiMaxDistanceMm(3400),
      i2cClockThermalHz(800000),
      i2cClockToFHz(200000),
      thermalTargetFps(8),
      thermalWebMaxFps(10),
      thermalRollingMinMaxEnabled(true),
      thermalRollingMinMaxAlpha(0.6f),
      thermalRollingMinMaxGuardC(0.3f),
      thermalTemporalAlpha(0.5f),
      thermalRotation(0),
      thermalDevicePollMs(100),
      tofDevicePollMs(220),
      imuDevicePollMs(200),
      gpsDevicePollMs(200),
      apdsDevicePollMs(200),
      gamepadDevicePollMs(90),
      fmRadioDevicePollMs(250),
      imuPollingMs(200),
      imuEWMAFactor(0.1f),
      imuTransitionMs(100),
      imuWebMaxFps(15),
      imuOrientationCorrectionEnabled(true),
      imuOrientationMode(8),
      imuPitchOffset(0.0f),
      imuRollOffset(0.0f),
      imuYawOffset(0.0f),
      debugHttp(false),
      debugSse(false),
      debugCli(false),
      debugAuth(false),
      debugEspNow(false),
      debugWifi(false),
      debugWifiConnection(false),
      debugWifiConfig(false),
      debugWifiScanning(false),
      debugWifiDriver(false),
      debugStorage(false),
      debugPerformance(false),
      debugDateTime(false),
      debugDatetimeSync(false),
      debugDatetimeSetup(false),
      debugDatetimeAnchor(false),
      debugDatetimeResolve(false),
      debugCommandFlow(false),
      debugUsers(false),
      debugSystem(false),
      debugAutomations(false),
      debugLogger(false),
      debugEspNowStream(false),
      debugMqtt(false),
      debugMqttConnection(false),
      debugMqttPubsub(false),
      debugMqttDiscovery(false),
      debugMqttCommands(false),
      debugEspNowCore(false),
      debugEspNowRouter(false),
      debugEspNowMesh(false),
      debugEspNowTopo(false),
      debugEspNowEncryption(false),
      debugEspNowMetadata(false),
      debugAutoScheduler(false),
      debugAutoExec(false),
      debugAutoCondition(false),
      debugAutoTiming(false),
      debugMemory(false),
      debugMemoryHeap(false),
      debugMemoryStack(false),
      debugMemoryBuffers(false),
      debugCommandSystem(false),
      debugBluetooth(false),
      debugBluetoothCore(false),
      debugBluetoothGatt(false),
      debugBluetoothData(false),
      debugFmRadio(false),
      debugG2(false),  // G2 smart glasses BLE connection (master)
      debugG2Lifecycle(false),
      debugG2Protocol(false),
      debugG2Events(false),
      debugG2Pages(false),
      debugG2Heartbeat(false),
      debugG2Dump(false),
      debugCamera(false),
      debugCameraLifecycle(false),
      debugCameraCapture(false),
      debugCameraSettings(false),
      debugCameraVideo(false),
      debugDisplay(false),
      debugMicrophone(false),
      debugI2C(false),  // I2C bus transactions, mutex, clock changes
      debugI2CBus(false),
      debugI2CDiscovery(false),
      debugI2CAutoStart(false),
      debugGps(false),        // Individual sensor flags disabled by default
      debugRtc(false),
      debugImu(false),
      debugThermal(false),
      debugTof(false),
      debugGamepad(false),
      debugApds(false),
      debugPresence(false),
      debugThermalLifecycle(false),
      debugThermalPolling(false),
      debugThermalValues(false),
      debugTofLifecycle(false),
      debugTofPolling(false),
      debugTofValues(false),
      debugGamepadLifecycle(false),
      debugGamepadPolling(false),
      debugGamepadValues(false),
      debugImuLifecycle(false),
      debugImuPolling(false),
      debugImuValues(false),
      debugApdsLifecycle(false),
      debugApdsPolling(false),
      debugApdsValues(false),
      debugGpsLifecycle(false),
      debugGpsPolling(false),
      debugGpsValues(false),
      debugRtcLifecycle(false),
      debugRtcPolling(false),
      debugRtcValues(false),
      debugFmRadioLifecycle(false),
      debugFmRadioPolling(false),
      debugFmRadioValues(false),
      debugMicLifecycle(false),
      debugMicPolling(false),
      debugMicValues(false),
      debugPresenceLifecycle(false),
      debugPresencePolling(false),
      debugPresenceValues(false),
      debugMaps(false),
      debugMapsLoading(false),
      debugMapsRendering(false),
      debugMapsPerf(false),
      debugLlm(false),
      debugLlmLoad(false),
      debugLlmTokenizer(false),
      debugLlmForward(false),
      debugLlmGenerate(false),
      debugLlmMemory(false),
      debugSr(false),                 // ESP-SR speech recognition (master)
      debugSrWake(false),
      debugSrCommand(false),
      debugSrAfe(false),
      debugSrLifecycle(false),
      debugSrTuning(false),
      logLevel(3),                    // Default: LOG_LEVEL_DEBUG (show everything)
      memorySampleIntervalSec(30),
      espnowenabled(false),
      espnowmesh(false),
      espnowUserSyncEnabled(false),
      espnowCaptureToSd(false),
      espnowCaptureSkipHeartbeats(true),
      espnowDeviceName(""),
      espnowRoom(""),
      espnowZone(""),
      espnowTags(""),
      espnowFriendlyName(""),
      espnowStationary(false),
      espnowFirstTimeSetup(false),
      espnowPassphrase(""),
      meshRole(0),
      meshMasterMAC(""),
      meshBackupMAC(""),
      meshBackupEnabled(false),
      meshMasterHeartbeatInterval(10000),
      meshFailoverTimeout(20000),
      meshWorkerStatusInterval(30000),
      meshTopoDiscoveryInterval(0),
      meshTopoAutoRefresh(false),
      meshHeartbeatBroadcast(true),
      meshTTL(3),
      meshAdaptiveTTL(false),
      meshPeerMax(8),
      sensorBroadcastIntervalMs(1000),
#if ENABLE_BONDED_MODE
      bondModeEnabled(false),
      bondRole(0),
      bondPeerMac(""),
      bondStreamThermal(false),
      bondStreamTof(false),
      bondStreamImu(false),
      bondStreamGps(false),
      bondStreamGamepad(false),
      bondStreamFmradio(false),
      bondStreamRtc(false),
      bondStreamPresence(false),
#endif
#if ENABLE_AUTOMATION
      automationsEnabled(true),
#endif
      i2cBusEnabled(true),
      ledBrightness(100),
      ledStartupEnabled(true),
      ledStartupEffect("rainbow"),
      ledStartupColor("cyan"),
      ledStartupColor2("magenta"),
      ledStartupDuration(1000),
      oledEnabled(false),
      localDisplayRequireAuth(true),
      oledBootMode("logo"),
      oledDefaultMode("status"),
      oledBootDuration(2000),
      oledUpdateInterval(125),
      oledBrightness(255),
      oledThermalScale(2.5f),
      oledThermalColorMode("3level"),
      gamepadAutoStart(false),
      thermalAutoStart(false),
      tofAutoStart(false),
      imuAutoStart(false),
      gpsAutoStart(false),
      fmRadioAutoStart(false),
      apdsAutoStart(false),
      rtcAutoStart(true),
      rtcTimeHasBeenSet(false),  // Track if RTC time has been set by NTP or manual
      presenceAutoStart(false),
      presenceDevicePollMs(100),
      sensorLogAutoStart(false),
      sensorLogPath("/logs/sensors/sensors.txt"),
      sensorLogIntervalMs(5000),
      sensorLogMask(0),
      sensorLogFormat(0),
      systemLogAutoStart(false),
      systemLogPath(""),
      systemLogCategoryTags(true),
      cameraAutoStart(false),  // Camera does NOT auto-start by default
      microphoneAutoStart(false),  // Microphone does NOT auto-start by default
      microphoneSampleRate(16000),
      microphoneGain(70),
      microphoneBitDepth(16),
      cameraBrightness(2),
      cameraContrast(2),
      cameraSaturation(2),
      cameraSharpness(0),
      cameraAELevel(0),
      cameraWBMode(0),
      cameraDenoise(0),
      cameraSpecialEffect(0),
      cameraHMirror(false),
      cameraVFlip(false),
      cameraQuality(12),
      cameraFramesize(10),  // 240x240 — see cameraFramesizeFromSetting in System_Camera_DVP.cpp for the index map
      cameraStreamFps(5),
      g2StreamWidth(96),
      g2StreamHeight(96),
      g2StreamToneMap(true),
      g2PackRateMs(80),
      cameraStorageLocation(1),  // 1 = SD card (default — LittleFS is too small for photos)
      cameraCaptureFolder("/photos"),
      cameraAutoCapture(false),
      cameraAutoCaptureIntervalSec(60),
      cameraMaxStoredImages(100),
      cameraSendAfterCapture(false),
      cameraTargetDevice(""),
      edgeImpulseEnabled(false),
      edgeImpulseRequireLabels(true),
      edgeImpulseMinConfidence(0.6f),
      edgeImpulseMaxDetections(5),
      edgeImpulseInputSize(96),
      edgeImpulseContinuous(false),
      edgeImpulseIntervalMs(1000),
      httpAutoStart(true),
      httpsEnabled(false),
      serialRequireAuth(true),
      bluetoothAutoStart(true),
      bluetoothRequireAuth(true),
      bleDeviceName("HardwareOne"),
      bleTxPower(3),
      bleMode(0),
      // BLE peer fields moved to gBlePeerData[] (see BLE_Peers.h)
      powerMode(0),
      powerAutoMode(false),
      powerBatteryThreshold(20),
      powerDisplayDimLevel(30),
      srAutoStart(false),
      srModelSource(0),
      srCommandTimeout(6000),
      srAfeGain(1.0f),
      srAgcMode(2),
      srVadMode(3),
      mqttClientEnabled(false),
      mqttAutoStart(false),
      mqttHost(""),
      mqttPort(1883),
      mqttTLSMode(0),  // 0=None, 1=TLS (no verify), 2=TLS+Verify
      mqttCACertPath("/system/certs/mqtt_ca.crt"),
      mqttSubscribeExternal(false),
      mqttSubscribeTopics(""),
      mqttUser(""),
      mqttPassword(""),
      mqttBaseTopic(""),
      mqttDiscoveryPrefix("homeassistant"),
      mqttPublishIntervalMs(10000),
      mqttPublishWiFi(false),
      mqttPublishSystem(false),
      mqttPublishThermal(false),
      mqttPublishToF(false),
      mqttPublishIMU(false),
      mqttPublishPresence(false),
      mqttPublishGPS(false),
      mqttPublishAPDS(false),
      mqttPublishRTC(false),
      mqttPublishGamepad(false),
      crashCount(0),
      lastResetReason(0)
#if ENABLE_ONDEVICE_LLM
      ,llmTemperature(0.5f)
      ,llmTopP(0.8f)
      ,llmMaxTokens(256)
      ,llmSentenceLimit(2)
      ,llmHardCap(80)
      ,llmRepPenalty(1.3f)
      ,llmRepWindow(32)
      ,llmMaxContext(0)
      ,llmUseMirostat2(false)
      ,llmMirostatTau(5.0f)
      ,llmMirostatEta(0.1f)
      ,llmDynTemp(false)
      ,llmDefaultModel("model.bin")
#endif
      // Maps app
      ,mapZoom(1.0f)
      ,mapVisibleLayers(0x3FF)   // LAYER_ALL
      ,mapCacheSizeKB(1024)      // 1 MB pool
    {
    // String members are now initialized in initializer list
  }

  String wifiSSID;
  String wifiPassword;
  bool wifiEnabled;        // Enable/disable WiFi at boot (default: true)
  bool wifiAutoReconnect;
  int webCliHistorySize;   // Web CLI history buffer size
  int oledCliHistorySize;  // OLED CLI history buffer size (lines)
  String ntpServer;
  int tzOffsetMinutes;
  bool outSerial;  // persist output lanes
  bool outWeb;
  bool outDisplay;
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  bool outG2;
#endif
  // Sensors UI (non-advanced)
  int thermalPollingMs;
  int tofPollingMs;
  int tofStabilityThreshold;
  String thermalPaletteDefault;
  // Thermal interpolation settings
  bool thermalInterpolationEnabled;
  int thermalInterpolationSteps;
  int thermalInterpolationBufferSize;
  int thermalUpscaleFactor;     // 1x, 2x, 4x upscaling on-device
  // Advanced UI + firmware-affecting
  float thermalEWMAFactor;
  int thermalTransitionMs;
  int tofTransitionMs;
  int tofUiMaxDistanceMm;
  int i2cClockThermalHz;
  int i2cClockToFHz;
  int thermalTargetFps;
  int thermalWebMaxFps;
  // Thermal rolling min/max stabilization
  bool thermalRollingMinMaxEnabled;
  float thermalRollingMinMaxAlpha;
  float thermalRollingMinMaxGuardC;
  // Temporal frame smoothing (reduces interpolation jitter, but can cause ghosting)
  float thermalTemporalAlpha;  // 0.0-1.0, higher = more smoothing but more ghosting
  // Thermal rotation (0=0°, 1=90°, 2=180°, 3=270°)
  int thermalRotation;
  // Device-side sensor settings (affect firmware runtime)
  int thermalDevicePollMs;
  int tofDevicePollMs;
  int imuDevicePollMs;
  int gpsDevicePollMs;
  int apdsDevicePollMs;
  int gamepadDevicePollMs;
  int fmRadioDevicePollMs;
  // IMU UI settings (client-side visualization)
  int imuPollingMs;
  float imuEWMAFactor;
  int imuTransitionMs;
  int imuWebMaxFps;
  // IMU orientation correction settings (device-side)
  bool imuOrientationCorrectionEnabled;
  int imuOrientationMode;  // 0=normal, 1=flip_pitch, 2=flip_roll, 3=flip_yaw, 4=flip_pitch_roll, etc.
  float imuPitchOffset;    // Pitch correction offset in degrees
  float imuRollOffset;     // Roll correction offset in degrees
  float imuYawOffset;      // Yaw correction offset in degrees
  // Debug settings (parent flags - kept for backward compatibility)
  bool debugHttp;
  bool debugSse;
  bool debugCli;
  bool debugAuth;
  bool debugEspNow;
  bool debugWifi;
  bool debugWifiConnection;
  bool debugWifiConfig;
  bool debugWifiScanning;
  bool debugWifiDriver;
  bool debugStorage;
  bool debugPerformance;
  bool debugDateTime;
  bool debugDatetimeSync;
  bool debugDatetimeSetup;
  bool debugDatetimeAnchor;
  bool debugDatetimeResolve;
  bool debugCommandFlow;
  bool debugUsers;
  bool debugSystem;
  bool debugAutomations;
  bool debugLogger;
  bool debugEspNowStream;
  bool debugMqtt;
  bool debugMqttConnection; // connect/disconnect, TLS config, broker errors, init
  bool debugMqttPubsub;     // subscribe events, publish results, JSON buffer alloc, received messages
  bool debugMqttDiscovery;  // Home Assistant auto-discovery configs, base topic
  bool debugMqttCommands;   // inbound MQTT command parsing, auth, response
  bool debugEspNowCore;
  bool debugEspNowRouter;
  bool debugEspNowMesh;
  bool debugEspNowTopo;
  bool debugEspNowEncryption;
  bool debugEspNowMetadata;
  bool debugAutoScheduler;
  bool debugAutoExec;
  bool debugAutoCondition;
  bool debugAutoTiming;
  bool debugMemory;
  bool debugMemoryHeap;     // [HEAP] per-task free/min/largest, [HEAP_MONITOR]
  bool debugMemoryStack;    // [STACK] per-task watermark + peak reports
  bool debugMemoryBuffers;  // [JSON_RESP_BUF], [COOKIE_BUF] sizing diagnostics
  bool debugCommandSystem;
  bool debugBluetooth;
  bool debugBluetoothCore;
  bool debugBluetoothGatt;
  bool debugBluetoothData;
  bool debugFmRadio;
  bool debugG2;  // G2 smart glasses BLE connection (master)
  bool debugG2Lifecycle;  // Scan, BLE connect/disconnect, MTU, service enumeration
  bool debugG2Protocol;   // Envelope TX/RX, CRC, fragmentation, parse errors
  bool debugG2Events;     // DevEvents, ListEvents, SysEvents, gestures, taps
  bool debugG2Pages;      // Page-swap worker, hijack, CREATE-list/text, lens state
  bool debugG2Heartbeat;  // Heartbeat TX + HeartbeatAck (every ~5 s; loud)
  bool debugG2Dump;       // [G2-DUMP] diagnostic ring buffer dumps on errors
  bool debugCamera;
  bool debugCameraLifecycle;  // initCamera/stopCamera, PWDN/RESET, GPIO state
  bool debugCameraCapture;    // captureFrame, JPEG validation, fb buffer, recovery
  bool debugCameraSettings;   // Runtime resolution/quality/sensor register changes
  bool debugCameraVideo;      // Video recording start/finalize, frame writing
  bool debugDisplay;          // OLED init/probe/boot-animation/mode-transitions
  bool debugMicrophone;
  bool debugI2C;  // I2C bus transactions, mutex, clock changes
  bool debugI2CBus;        // [I2C] bus lifecycle, polling pause/resume, status bumps
  bool debugI2CDiscovery;  // [Discovery] / registry / scan results
  bool debugI2CAutoStart;  // [AutoStart] sensor auto-start orchestration + init results
  // Individual I2C sensor debug flags
  bool debugGps;        // GPS (PA1010D)
  bool debugRtc;        // RTC (DS3231)
  bool debugImu;        // IMU (BNO055)
  bool debugThermal;    // Thermal (MLX90640)
  bool debugTof;        // ToF (VL53L4CX)
  bool debugGamepad;    // Gamepad (Seesaw)
  bool debugApds;       // APDS (APDS9960)
  bool debugPresence;   // Presence (STHS34PF80)
  // Per-sensor frame/data debug flags (granular timing and data processing)
  // Per-sensor sub-flags (Lifecycle / Polling / Values).
  // Lifecycle: init, connect/disconnect, recovery, error retries.
  // Polling:   poll/sample cadence, capture timing, FPS, frame events.
  // Values:    parsed readings, value-change events, data processing.
  bool debugThermalLifecycle;
  bool debugThermalPolling;
  bool debugThermalValues;
  bool debugTofLifecycle;
  bool debugTofPolling;
  bool debugTofValues;
  bool debugGamepadLifecycle;
  bool debugGamepadPolling;
  bool debugGamepadValues;
  bool debugImuLifecycle;
  bool debugImuPolling;
  bool debugImuValues;
  bool debugApdsLifecycle;
  bool debugApdsPolling;
  bool debugApdsValues;
  bool debugGpsLifecycle;
  bool debugGpsPolling;
  bool debugGpsValues;
  bool debugRtcLifecycle;
  bool debugRtcPolling;
  bool debugRtcValues;
  bool debugFmRadioLifecycle;
  bool debugFmRadioPolling;
  bool debugFmRadioValues;
  bool debugMicLifecycle;
  bool debugMicPolling;
  bool debugMicValues;
  bool debugPresenceLifecycle;
  bool debugPresencePolling;
  bool debugPresenceValues;
  // Maps debug flags
  bool debugMaps;           // Maps (parent flag)
  bool debugMapsLoading;    // Map file loading, tile directory parsing
  bool debugMapsRendering;  // Map render pipeline, feature drawing, viewport
  bool debugMapsPerf;       // Map performance timing (render ms, tile I/O, cache, FPS)
  // On-device LLM debug flags (System_LLM — parent + sub-flags)
  bool debugLlm;            // All LLM
  bool debugLlmLoad;        // Checkpoint load, validation, weight mapping
  bool debugLlmTokenizer;   // Tokenizer / BPE
  bool debugLlmForward;       // Transformer forward (verbose)
  bool debugLlmGenerate;      // Generation loop, sampling
  bool debugLlmMemory;        // PSRAM budget, context cap
  // ESP-SR debug flags (System_ESPSR — parent + sub-flags). Replaces the
  // legacy gSrDebugLevel integer; the runtime level is derived from the
  // bools so old log sites keep working unchanged.
  bool debugSr;             // All SR (parent)
  bool debugSrWake;         // Wake word detection events
  bool debugSrCommand;      // MultiNet command recognition + matching
  bool debugSrAfe;          // AFE chain — VAD, noise suppression, gain
  bool debugSrLifecycle;    // init / start / stop verbose
  bool debugSrTuning;       // Auto-tune sweeps, confidence threshold
  // Auth sub-flags
  bool debugAuthSessions;
  bool debugAuthCookies;
  bool debugAuthLogin;
  bool debugAuthBootId;
  // HTTP sub-flags
  bool debugHttpHandlers;
  bool debugHttpRequests;
  bool debugHttpResponses;
  bool debugHttpStreaming;
  // Storage sub-flags
  bool debugStorageFiles;
  bool debugStorageJson;
  bool debugStorageSettings;
  bool debugStorageMigration;
  // [PERM] DENY audit lines from VFS::*Guarded. Defaults to true so the
  // pre-subflag "always-on" audit behavior is preserved out of the box;
  // user can `debugstoragepermissions 0` to mute denial spam while keeping
  // other Storage subflags enabled.
  bool debugStoragePermissions{true};
  // System sub-flags
  bool debugSystemBoot;
  bool debugSystemConfig;
  bool debugSystemTasks;
  bool debugSystemHardware;
  // Users sub-flags
  bool debugUsersMgmt;
  bool debugUsersRegister;
  bool debugUsersQuery;
  // CLI sub-flags
  bool debugCliExecution;
  bool debugCliQueue;
  bool debugCliValidation;
  // Performance sub-flags
  bool debugPerfStack;
  bool debugPerfHeap;
  bool debugPerfTiming;
  // SSE sub-flags
  bool debugSseConnection;
  bool debugSseEvents;
  bool debugSseBroadcast;
  // Command Flow sub-flags
  bool debugCmdflowRouting;
  bool debugCmdflowQueue;
  bool debugCmdflowContext;
  bool webConsoleDebug;                // Enable browser console.log/warn/debug output in web UI (default: false)
  int logLevel;                        // Severity-based logging level (0=error, 1=warn, 2=info, 3=debug)
  int memorySampleIntervalSec;  // Periodic memory sampling interval in seconds (0=disabled, default: 30)
  // ESP-NOW settings
  bool espnowenabled;
  bool espnowmesh;
  bool espnowUserSyncEnabled;          // Enable user credential sync across devices (default: false, admin-only)
  // Capture incoming + outgoing ESP-NOW frames to /sd/espnow/capture-*.log when
  // SD is mounted. Encrypted payloads are stored as-is (base64). Heartbeats
  // dominate volume, so an optional sub-filter can skip them.
  bool espnowCaptureToSd;              // Master toggle: save ESP-NOW frames to SD
  bool espnowCaptureSkipHeartbeats;    // Skip type 7 HEARTBEAT and 14 BOND_HEARTBEAT
  // ESP-NOW device identity
  String espnowDeviceName;             // Device name for ESP-NOW topology (user-configurable)
  // ESP-NOW device metadata (for mesh organization and HA discovery)
  String espnowRoom;                   // Room assignment: "Kitchen", "Bedroom", etc.
  String espnowZone;                   // Sub-location within room: "Counter", "Door", "Ceiling"
  String espnowTags;                   // Comma-separated tags: "stationary,thermal,hallway"
  String espnowFriendlyName;           // Longer display name: "Kitchen Thermal Cam"
  bool espnowStationary;               // true = mounted/fixed, false = mobile/wearable
  bool espnowFirstTimeSetup;           // True if ESP-NOW setup has been completed
  String espnowPassphrase;             // Encryption passphrase for secure pairing (persisted)
  // Mesh role settings
  uint8_t meshRole;                    // 0=worker, 1=master, 2=backup_master
  String meshMasterMAC;                // MAC of current master (empty if this is master)
  String meshBackupMAC;                // MAC of designated backup master
  bool meshBackupEnabled;              // Show/use backup master feature (default: false)
  uint32_t meshMasterHeartbeatInterval;  // Master heartbeat interval (ms, default: 10000)
  uint32_t meshFailoverTimeout;        // Backup promotes after this timeout (ms, default: 20000)
  uint32_t meshWorkerStatusInterval;   // Worker status report interval (ms, default: 30000)
  uint32_t meshTopoDiscoveryInterval;  // Topology discovery interval (ms, 0=disabled)
  bool meshTopoAutoRefresh;            // Enable automatic topology refresh
  bool meshHeartbeatBroadcast;         // Broadcast heartbeats to FF:FF:FF:FF:FF:FF (public mode)
  uint8_t meshTTL;                     // Mesh TTL (1-10, default: 3)
  bool meshAdaptiveTTL;                // Enable adaptive TTL based on peer count
  uint8_t meshPeerMax;                 // Max mesh peer slots (1-16, default: 8, changes on reboot)
  uint16_t sensorBroadcastIntervalMs;  // Sensor broadcast interval in ms (100-10000, default: 1000)
#if ENABLE_BONDED_MODE
  // Bond mode settings (two-device bonded pair)
  bool bondModeEnabled;              // Enable bond mode (master/worker)
  uint8_t bondRole;                  // 0=worker (compute/network), 1=master (display/gamepad)
  String bondPeerMac;                // MAC address of bonded peer device
  // Bond mode sensor streaming (auto-enable on boot when bonded)
  bool bondStreamThermal;              // Auto-stream thermal data to bonded peer
  bool bondStreamTof;                  // Auto-stream ToF data to bonded peer
  bool bondStreamImu;                  // Auto-stream IMU data to bonded peer
  bool bondStreamGps;                  // Auto-stream GPS data to bonded peer
  bool bondStreamGamepad;              // Auto-stream gamepad data to bonded peer
  bool bondStreamFmradio;              // Auto-stream FM radio data to bonded peer
  bool bondStreamRtc;                  // Auto-stream RTC data to bonded peer
  bool bondStreamPresence;             // Auto-stream presence sensor data to bonded peer
#endif
  // ESP-NOW buffer size settings (runtime tuning)
  uint16_t espnowTxQueueSize;          // TX retry queue size (1-16, default: 8)
  uint16_t espnowRxBufferSize;         // RX deferred message buffer size (64-512, default: 256)
  uint16_t espnowChunkSize;            // Chunk size for large messages (100-220, default: 200)
  uint16_t espnowFileChunkSize;        // File transfer chunk size (100-224, default: 224)
#if ENABLE_AUTOMATION
  bool automationsEnabled;  // Enable/disable automation scheduler (runs from main loop)
#endif
  // I2C Hardware system
  bool i2cBusEnabled;       // Enable/disable I2C bus hardware (Wire/Wire1 init and transactions)
  int i2cSdaPin = I2C_SDA_PIN_DEFAULT;  // I2C SDA pin (board-specific, see System_BuildConfig.h)
  int i2cSclPin = I2C_SCL_PIN_DEFAULT;  // I2C SCL pin (board-specific, see System_BuildConfig.h)
  // Hardware settings (LED)
  int ledBrightness;        // 0-100%
  bool ledStartupEnabled;   // Enable startup effect
  String ledStartupEffect;  // Effect type: none, rainbow, pulse, fade, blink, strobe
  String ledStartupColor;   // Primary color name
  String ledStartupColor2;  // Secondary color (for fade)
  int ledStartupDuration;   // Duration in ms
  // OLED Display settings
  bool oledEnabled;             // Enable/disable OLED at boot
  bool localDisplayRequireAuth; // Require login before accessing display modes
  String oledBootMode;          // Initial mode during boot: logo, status, sensors, thermal, off
  String oledDefaultMode;       // Mode to switch to after boot completes
  int oledBootDuration;         // Milliseconds to show boot mode before switching to default
  int oledUpdateInterval;       // Update interval in milliseconds (8 Hz = 125ms)
  int oledBrightness;           // Display brightness/contrast 0-255
  float oledThermalScale;       // Scaling factor for thermal image (2.5 = 80x60)
  String oledThermalColorMode;  // Visualization style: 3level, 2level, gradient
  // Sensor Auto-Start settings (all I2C sensors)
  bool gamepadAutoStart;        // Auto-start gamepad after boot completes
  bool thermalAutoStart;        // Auto-start thermal camera after boot
  bool tofAutoStart;            // Auto-start ToF distance sensor after boot
  bool imuAutoStart;            // Auto-start IMU after boot
  bool gpsAutoStart;            // Auto-start GPS after boot
  bool fmRadioAutoStart;        // Auto-start FM radio after boot
  bool apdsAutoStart;           // Auto-start APDS gesture/color sensor after boot
  bool rtcAutoStart;            // Auto-start RTC (DS3231) after boot
  bool rtcTimeHasBeenSet;       // Has RTC time been set by NTP or manual? (false = trust NTP first at boot)
  bool presenceAutoStart;       // Auto-start STHS34PF80 presence/motion sensor after boot
  int presenceDevicePollMs;     // STHS34PF80 polling interval (default: 100ms)
  // Sensor Logging auto-start settings
  bool sensorLogAutoStart;      // Auto-start sensor logging after boot with last-used parameters
  String sensorLogPath;         // Last-used log file path (default: /logs/sensors/sensors.txt)
  int sensorLogIntervalMs;      // Last-used polling interval in ms (default: 5000)
  int sensorLogMask;            // Last-used sensor bitmask (0=none)
  int sensorLogFormat;          // Last-used format (0=text, 1=csv, 2=track)
  // System Logging settings
  bool systemLogAutoStart;      // Auto-start system logging after boot
  String systemLogPath;         // Log file path (empty = auto-generate with timestamp)
  bool systemLogCategoryTags;   // Prefix log lines with [CATEGORY] tags
  bool cameraAutoStart;         // Auto-start ESP32-S3 camera after boot
  bool microphoneAutoStart;     // Auto-start ESP32-S3 PDM microphone after boot
  // Microphone settings
  int microphoneSampleRate;     // Sample rate in Hz (8000, 16000, 22050, 44100, 48000)
  int microphoneGain;           // Software gain 0-100% (default 90)
  int microphoneBitDepth;       // Bit depth (16 or 32)
  // Camera image settings (persisted) - use int for settings system compatibility
  // OV3660 ships flat/washed-out, so we default brightness/contrast/saturation
  // to +2 (the API max). User-validated empirically — see camerafx command +
  // notes in System_Camera_DVP.cpp::initCamera.
  int cameraBrightness;         // -2 to 2 (default +2)
  int cameraContrast;           // -2 to 2 (default +2)
  int cameraSaturation;         // -2 to 2 (default +2)
  int cameraSharpness;          // -2 to 2 (default 0, OV3660 only)
  int cameraAELevel;            // Auto-exposure level/compensation -2 to 2 (default 0)
  int cameraWBMode;             // White balance mode 0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home (default 0)
  int cameraDenoise;            // Denoise level 0-8 (default 0)
  int cameraSpecialEffect;      // Special effect 0=None,1=Neg,2=Gray,3=Red,4=Green,5=Blue,6=Sepia (default 0)
  bool cameraHMirror;           // Horizontal mirror
  bool cameraVFlip;             // Vertical flip
  int cameraQuality;            // JPEG quality 0-63 (lower=better, default 12)
  int cameraFramesize;          // Setting-index (NOT esp_camera framesize_t). 10 = 240x240 default. See cameraFramesizeFromSetting().
  int cameraStreamFps;          // Camera stream + recording FPS target (1-20, default 5). Drivers convert to ms internally.
  // G2 lens stream resolution. Caps at 288x144 (lens panel native size) and
  // floors at 16. Wider/taller = more Cmd=3 fragments per frame = lower fps;
  // user picks from a small preset list in the lens Camera Settings page.
  // Defaults match the legacy hardcoded gG2StreamW/H values (96x96).
  int g2StreamWidth;
  int g2StreamHeight;
  // G2 lens stream auto-levels tone mapping. When true, the BMP build path
  // does a per-frame luma min/max scan and linearly remaps to full 0..255
  // range before quantizing to 4-bpp nibbles — recovers dynamic range on
  // washed-out OV3660 frames that would otherwise quantize to a narrow
  // band of similar-looking shades on the green-tinted lens panel.
  bool g2StreamToneMap;
  // Frame cadence (ms) for Q25 SD-pack BMP animations (gif→bmp packs).
  // Independent of g2liverate (which paces the live-tile test probes) so
  // tuning playback speed for animations doesn't interfere with the
  // cinematic cadence used for test-suite comparisons. For small frames
  // (32×32, 64×64) this is the actual frame-time floor; for larger frames
  // the BLE push itself dominates and this value is effectively ignored.
  // Default 80 ms ≈ 12 fps; CLI: g2packrate <ms>.
  int g2PackRateMs;
  // Camera storage settings
  int cameraStorageLocation;    // 0=LittleFS, 1=SD, 2=Both
  String cameraCaptureFolder;   // Folder for saved images (default: "/photos")
  bool cameraAutoCapture;       // Enable periodic auto-capture
  int cameraAutoCaptureIntervalSec; // Interval for auto-capture (default: 60)
  int cameraMaxStoredImages;    // Max images before rotation (0=unlimited, default: 100)
  bool cameraSendAfterCapture;  // Auto-send to target device after capture
  String cameraTargetDevice;    // ESP-NOW name/MAC of target device for auto-send
  // Edge Impulse ML settings
  bool edgeImpulseEnabled;      // Enable Edge Impulse inference
  bool edgeImpulseRequireLabels;
  float edgeImpulseMinConfidence; // Minimum confidence threshold (0.0-1.0)
  int edgeImpulseMaxDetections; // Max objects to report per frame (1-10)
  int edgeImpulseInputSize;     // Model input size (96, 128, etc)
  bool edgeImpulseContinuous;   // Continuous inference mode
  int edgeImpulseIntervalMs;    // Interval between inferences in continuous mode
  // HTTP server settings
  bool httpAutoStart;           // Auto-start HTTP server at boot if WiFi connected
  bool httpsEnabled;            // Use HTTPS instead of HTTP when certs are present (requires reboot)
  bool serialRequireAuth;       // Require login before accepting serial CLI commands (default: true)
  // Bluetooth settings
  bool bluetoothAutoStart;      // Auto-start Bluetooth at boot (enables BLE server)
  bool bluetoothRequireAuth;    // Require login before accepting BLE commands (always required, per-connection)
  String bleDeviceName;         // BLE advertised device name (default: "HardwareOne")
  int bleTxPower;               // BLE TX power level 0-7 (0=min, 7=max, default: 3)
  int bleMode;                  // BLE role: 0=server (phone peripheral), 1=G2 client (central). Mutually exclusive at runtime.
  // BLE peer MACs and auto-reconnect flags now live in BLE_Peers.cpp's
  // gBlePeerData[BLE_PEER_MAX] array, persisted under "bluetooth.peers"
  // in settings.json. See BLE_Peers.h. Removed flat keys:
  //   bleGlassesLeftMAC, bleGlassesRightMAC, bleRingMAC, blePhoneMAC,
  //   g2AutoConnect, ringAutoConnect
  // Power Management settings
  uint8_t powerMode;            // 0=Performance(240MHz), 1=Balanced(160MHz), 2=PowerSaver(80MHz), 3=UltraSaver(40MHz)
  bool powerAutoMode;           // Auto-adjust power mode based on battery level
  uint8_t powerBatteryThreshold; // Switch to power saver below this battery % (default: 20)
  uint8_t powerDisplayDimLevel; // Brightness % in power saver modes (0-100, default: 30)
  // ESP-SR Speech Recognition settings
  bool srAutoStart;             // Auto-start SR at boot (default: false)
  int srModelSource;  // 0=partition (default), 1=SD card, 2=LittleFS
  int srCommandTimeout;         // Command listening timeout in ms after wake word (default: 6000)
  float srAfeGain;              // AFE linear gain multiplier (0.1-10.0, default: 1.0)
  int srAgcMode;                // AGC mode: 0=off, 1=-9dB, 2=-6dB, 3=-3dB (default: 2)
  int srVadMode;                // VAD sensitivity: 0-4, higher=more sensitive (default: 3)
  // MQTT Home Assistant Integration settings
  bool mqttClientEnabled;       // Master enable/disable for MQTT subsystem (default: false)
  bool mqttAutoStart;           // Auto-start MQTT client at boot if WiFi connected (default: false)
  String mqttHost;              // MQTT broker hostname or IP (default: "")
  int mqttPort;                 // MQTT broker port (default: 1883, 8883 for TLS)
  int mqttTLSMode;              // 0=None, 1=TLS (no verify), 2=TLS+Verify (default: 0)
  String mqttCACertPath;        // Path to CA certificate file for verification (default: "")
  bool mqttSubscribeExternal;   // Subscribe to external topics for sensor data (default: false)
  String mqttSubscribeTopics;   // Comma-separated list of topics to subscribe (default: "")
  String mqttUser;              // MQTT username (default: "")
  String mqttPassword;          // MQTT password - stored encrypted (default: "")
  String mqttBaseTopic;         // Base topic for publishing (default: hardwareone/<mac>, auto-generated if empty)
  String mqttDiscoveryPrefix;   // HA discovery prefix (default: "homeassistant")
  int mqttPublishIntervalMs;    // Sensor data publish interval in ms (default: 10000)
  // MQTT publish data configuration
  bool mqttPublishWiFi;         // Include WiFi info in published data (default: false)
  bool mqttPublishSystem;       // Include system info (uptime, heap, etc.) (default: false)
  bool mqttPublishThermal;      // Include thermal sensor data (default: false)
  bool mqttPublishToF;          // Include ToF sensor data (default: false)
  bool mqttPublishIMU;          // Include IMU sensor data (default: false)
  bool mqttPublishPresence;     // Include presence/motion sensor data (default: false)
  bool mqttPublishGPS;          // Include GPS location data (default: false)
  bool mqttPublishAPDS;         // Include gesture/proximity data (default: false)
  bool mqttPublishRTC;          // Include RTC time data (default: false)
  bool mqttPublishGamepad;      // Include gamepad input data (default: false)

  // Crash / reset tracking (persisted from RTC memory on next healthy boot)
  uint32_t crashCount;          // Accumulated abnormal resets (WDT, panic, brownout)
  uint32_t lastResetReason;     // esp_reset_reason_t value from last boot
#if ENABLE_ONDEVICE_LLM
  // On-device LLM generation defaults (System_LLM)
  float llmTemperature;         // Sampling temperature (default: 0.5)
  float llmTopP;                // Nucleus sampling threshold (default: 0.8)
  int llmMaxTokens;             // Max tokens per generation (default: 256)
  int llmSentenceLimit;         // Stop after N sentences, 0=disabled (default: 2)
  int llmHardCap;               // Hard token cap, 0=disabled (default: 80)
  float llmRepPenalty;          // Repetition penalty divisor (default: 1.3)
  int llmRepWindow;             // Look-back window for rep penalty (default: 32)
  int llmMaxContext;            // KV cache context window, 0=use compile-time default
  bool llmUseMirostat2;         // Enable Mirostat 2 sampling (default: false)
  float llmMirostatTau;         // Mirostat target surprise in bits (default: 5.0)
  float llmMirostatEta;         // Mirostat learning rate (default: 0.1)
  bool llmDynTemp;              // Dynamic temperature scaling (default: false)
  String llmDefaultModel;       // Default model filename for auto-load (default: "model.bin")
#endif

  // Maps app — persisted defaults applied at boot to gMapZoom / gVisibleLayers
  // and used to size the tile cache pool. Cache size requires a reboot to
  // take effect; zoom and layers apply live via their setters.
  float mapZoom;             // Default zoom level (1.0 = identity)
  int   mapVisibleLayers;    // Layer-visibility bitmask (10 bits, default 0x3FF = LAYER_ALL)
  int   mapCacheSizeKB;      // Tile cache pool size in KB (default 1024)
};

// Global settings instance (defined in .ino)
extern Settings gSettings;

// ============================================================================
// Settings Management Functions (implemented in settings.cpp)
// ============================================================================

// Settings initialization and defaults
void settingsDefaults();

// Settings JSON serialization/deserialization
void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords = false);
bool readSettingsJson();
bool writeSettingsJson();

// Apply settings to runtime flags
void applySettings();

// ============================================================================
// Centralized Setting Mutator — auto-persists on change
// ============================================================================
// Use setSetting(gSettings.field, newValue) instead of direct assignment
// to ensure writeSettingsJson() is called automatically.
// Only writes to flash when the value actually changes (no churn).
//
// Batch mode: call beginwrite before a group of setSetting() calls, then
// savesettings after. This defers the flash write to a single call at the end.
// From the web UI, save buttons use this pattern automatically.
// From the serial CLI, each setSetting() writes immediately as before.
extern volatile bool gDeferWrites;

template<typename T>
inline void setSetting(T& field, const T& value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeSettingsJson();
  }
}

// String overload (avoids unnecessary copy when equal)
inline void setSetting(String& field, const String& value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeSettingsJson();
  }
}

// const char* convenience overload
inline void setSetting(String& field, const char* value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeSettingsJson();
  }
}

// AES-128-CBC string encryption/decryption (device-bound key)
String encryptString(const String& plaintext);
String decryptString(const String& encrypted);
String getDeviceEncryptionKey();

// Device fingerprint for backup compatibility checking
// One-way hash derived from device encryption key — safe to include in backups
String getDeviceFingerprint();

// Settings command registry
extern const CommandEntry settingsCommands[];
extern const size_t settingsCommandsCount;

// ============================================================================
// Settings Command Handlers (implemented in settings.cpp)
// ============================================================================

// WiFi Settings
const char* cmd_wifitxpower(const String& argsInput);
const char* cmd_wifiautoreconnect(const String& argsInput);
const char* cmd_tzoffsetminutes(const String& argsInput);
const char* cmd_ntpserver(const String& argsInput);

// Thermal Settings
const char* cmd_thermalpalettedefault(const String& argsInput);
const char* cmd_thermalewmafactor(const String& argsInput);
const char* cmd_thermaltransitionms(const String& argsInput);
const char* cmd_thermalupscalefactor(const String& argsInput);
const char* cmd_thermalrollingminmaxenabled(const String& argsInput);
const char* cmd_thermalrollingminmaxalpha(const String& argsInput);
const char* cmd_thermalrollingminmaxguardc(const String& argsInput);
const char* cmd_thermaltemporalalpha(const String& argsInput);
const char* cmd_thermalrotation(const String& argsInput);

// ESP-NOW Settings
const char* cmd_espnowenabled(const String& argsInput);

// OLED Settings
const char* cmd_oled_enabled(const String& argsInput);
const char* cmd_oled_bootmode(const String& argsInput);
const char* cmd_oled_defaultmode(const String& argsInput);
const char* cmd_oled_bootduration(const String& argsInput);
const char* cmd_oled_updateinterval(const String& argsInput);
const char* cmd_oled_brightness(const String& argsInput);
const char* cmd_oled_thermalscale(const String& argsInput);
const char* cmd_oled_thermalcolormode(const String& argsInput);

// LED Settings
const char* cmd_hardwareled_brightness(const String& argsInput);
const char* cmd_hardwareled_startupenabled(const String& argsInput);
const char* cmd_hardwareled_startupeffect(const String& argsInput);
const char* cmd_hardwareled_startupcolor(const String& argsInput);
const char* cmd_hardwareled_startupcolor2(const String& argsInput);
const char* cmd_hardwareled_startupduration(const String& argsInput);

// CLI Settings
const char* cmd_webclihistorysize(const String& argsInput);
const char* cmd_oledclihistorysize(const String& argsInput);

// ============================================================================
// Modular Settings Registry System
// ============================================================================

// Setting data types
enum SettingType {
  SETTING_INT,
  SETTING_FLOAT,
  SETTING_BOOL,
  SETTING_STRING
};

// Individual setting entry - describes one setting field
struct SettingEntry {
  const char* jsonKey;        // Key in JSON file (e.g., "thermalPollingMs")
  SettingType type;           // Data type
  void* valuePtr;             // Pointer to actual value in gSettings
  int intDefault;             // Default for int (also used for bool: 0/1)
  float floatDefault;         // Default for float
  const char* stringDefault;  // Default for string
  int minVal;                 // Min value for int/float validation (0 to skip)
  int maxVal;                 // Max value for int/float validation (0 to skip)
  const char* label;          // Human-readable label for UI display (nullptr = use jsonKey)
  const char* options;        // Comma-separated options for select fields (nullptr = none)
  bool isSecret = false;      // If true: encrypt on disk, exclude from web API, blank input = unchanged
  const char* group = nullptr;  // Sub-section group for JSON nesting + UI grouping (nullptr = ungrouped)
  const char* cmdKey = nullptr; // CLI command name override (nullptr = use jsonKey as command)
};

// Connection check callback - returns true if module is available/connected
typedef bool (*ConnectionCheckFunc)();

// Settings module - a group of related settings
struct SettingsModule {
  const char* name;             // Module name (e.g., "thermal", "wifi")
  const char* jsonSection;      // JSON section name (nullptr for root level)
  const SettingEntry* entries;  // Array of setting entries
  size_t count;                 // Number of entries
  ConnectionCheckFunc isConnected; // Optional: check if module is available (nullptr = always available)
  const char* description;      // Optional: human-readable description for UI
};

// Maximum number of settings modules that can be registered
#define MAX_SETTINGS_MODULES 32

// Register a settings module (call during setup or static init)
void registerSettingsModule(const SettingsModule* module);

// Get all registered modules
const SettingsModule** getSettingsModules(size_t& count);

// Apply defaults from all registered modules
void applyRegisteredDefaults();

// Read settings from JSON using registered modules
// Returns number of settings successfully read
size_t readRegisteredSettings(JsonDocument& doc);

// Write settings to JSON using registered modules
// Returns number of settings written
size_t writeRegisteredSettings(JsonDocument& doc);

// Register ALL settings modules explicitly (called once early in boot)
// Ensures all compiled modules are available before applying defaults
void registerAllSettingsModules();

// Debug: print a summary of all registered settings modules
void printSettingsModuleSummary();

// Generic setting command handler - parses value and updates setting
// Returns result message
const char* handleSettingCommand(const SettingEntry* entry, const String& argsInput);

// Persist current settings to JSON file
bool writeSettingsJson();

// Apply persisted settings to runtime flags/state
void applySettings();

#endif // SETTINGS_H
