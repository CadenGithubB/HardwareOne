#ifndef SYSTEM_SETTINGS_H
#define SYSTEM_SETTINGS_H

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
    : wifiEnabled(true),
      wifiAutoReconnect(true),
      webCliHistorySize(10),
      oledCliHistorySize(50),
      ntpServer("pool.ntp.org"),
      tzOffsetMinutes(0),
      // Must stay identical to the schema default in System_Settings.cpp:
      // applyRegisteredDefaults() overwrites every value here at boot. Ctor
      // values are NOT dead — they are live from static init until
      // settingsDefaults() runs, i.e. early boot before the filesystem.
      outSerial(true),
      notifBanners(true),
      notifToasts(true),
      notifQueue(true),
      notifG2(true),
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
      i2cClockThermalHz(400000),
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
      inputDevicePollMs(90),
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
      debugHttps(false),
      debugSse(false),
      debugCli(false),
      debugAuth(false),
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
      debugNotifications(false),
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
      debugInput(false),
      debugAnoEncoder(false),
      debugApds(false),
      debugPresence(false),
      debugThermalLifecycle(false),
      debugThermalPolling(false),
      debugThermalValues(false),
      debugTofLifecycle(false),
      debugTofPolling(false),
      debugTofValues(false),
      debugInputLifecycle(false),
      debugInputPolling(false),
      debugInputValues(false),
      debugAnoEncoderLifecycle(false),
      debugAnoEncoderPolling(false),
      debugAnoEncoderValues(false),
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
      espnowChannel(0),
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
      espnowAcceptSensorControl(false),
      espnowMasterFingerprint(""),
      espnowBackupMasterFingerprint(""),
#if ENABLE_BONDED_MODE
      bondModeEnabled(false),
      bondRole(0),
      bondPeerMac(""),
      bondStreamThermal(false),
      bondStreamTof(false),
      bondStreamImu(false),
      bondStreamGps(false),
      bondStreamInput(false),
      bondStreamFmradio(false),
      bondStreamRtc(false),
      bondStreamPresence(false),
#endif
#if ENABLE_AUTOMATION
      automationsEnabled(true),
#endif
      i2cBusEnabled(true),
      i2c2BusEnabled(I2C2_BUS_ENABLED_DEFAULT),  // auto-on for boards with valid I2C2 pins
      // Per-device bus assignments — all default to bus 0 (primary), so an
      // existing single-bus config is unchanged. Bumped to bus 1 only when
      // the user explicitly moves a device to the secondary STEMMA QT port.
      oledBus(OLED_BUS_DEFAULT),  // FeatherS3[D] → 1 (LDO2-gated rail); other boards → 0
      inputBus(0), gpsBus(0), rtcBus(0), fmRadioBus(0),
      presenceBus(0), imuBus(0), thermalBus(0), tofBus(0), apdsBus(0), servoBus(0),
      fuelGaugeBus(0),
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
      oledFlipped(true),          // True keeps the historical hardcoded setRotation(2) behavior
      oledThermalScale(2.5f),
      oledThermalColorMode("3level"),
      inputAutoStart(false),
      anoEncoderI2cAddr(0x49),
      anoEncoderInvert(false),
      anoEncoderSwapUpDown(true),     // Adafruit ANO breakout's silkscreen UP/DOWN map inverted by default
      anoEncoderSwapLeftRight(true),  // ditto LEFT/RIGHT — matches the typical assembled orientation
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
      sensorLogPath(CAPTURE_SENSORLOG_DEFAULT),
      sensorLogIntervalMs(5000),
      sensorLogMask(0),
      sensorLogFormat(0),
      sensorLogMaxSize(256000),
      sensorLogMaxRotations(3),
      systemLogAutoStart(false),
      eventLogEnabled(true),
      systemLogPath(""),
      systemLogCategoryTags(true),
      systemLogFlags(""),
      cameraAutoStart(false),  // Camera does NOT auto-start by default
      microphoneAutoStart(false),  // Microphone does NOT auto-start by default
      llmAutoStart(false),  // On-device LLM does NOT auto-load a model by default
      microphoneSampleRate(16000),
      microphoneGain(70),
      microphoneBitDepth(16),
      micSource("auto"),  // Mic source preference: "auto" | "pdm" | "g2" (resolved at capture time)
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
      sessionIdleWeb(60),
      sessionIdleSerial(60),
      sessionIdleBle(15),
      sessionIdleDisplay(60),
      bluetoothAutoStart(false),  // BLE server is opt-in; the "G2 Companion" archetype (or Advanced setup) enables it
      bluetoothRequireAuth(true),
      bleDeviceName("HardwareOne"),
      bleTxPower(3),
      bleMode(0),
      bleRequireSecureChannel(true),
      bleSecureChannelSecret(""),
      // BLE peer fields moved to gBlePeerData[] (see BLE_Peers.h)
      powerMode(0),
      powerAutoMode(false),
      powerBatteryThreshold(20),
      powerDisplayDimLevel(30),
      powerTransitionCooldownMs(5000),  // 5s anti-flap guard on sleep entry; 0 = disabled
      powerSaveTimeoutMinutes(10),      // Idle power-save: blank OLED + 80MHz downclock after 10 min idle; 0 = disabled
      batteryLogEnabled(true),          // Auto-append battery time-series CSV to /battery.csv
      batteryLogIntervalMs(60000),      // Battery-log sample period (ms)
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
      mqttPublishInput(false),
      crashCount(0),
      lastResetReason(0)
#if ENABLE_ONDEVICE_LLM
      ,llmTemperature(0.5f)
      ,llmTopP(0.8f)
      ,llmMinP(0.0f)
      ,llmKvPrecision(1)     // FP16 default: same PSRAM auto-fit pool holds ~2x context, ~lossless
      ,llmNoRepeatNgram(0)   // built + reviewed but shipped OFF per user (llmnorepeatngram 3 to arm)
      ,llmConfThreshold(-1.0f)
      ,llmContentBoost(1.5f)
      ,llmMaxTokens(256)
      ,llmSentenceLimit(2)
      ,llmHardCap(80)
      ,llmRepPenalty(1.3f)
      ,llmRepWindow(32)
      ,llmMaxContext(0)
      ,llmDefaultModel("model.bin")
      ,llmDomainGate(true)
      ,llmProfile(false)
#endif
      // Maps app
      ,mapZoom(1.0f)
      ,mapVisibleLayers(0x3FF)   // LAYER_ALL
      ,mapCacheSizeKB(1280)      // 1.25 MB pool (holds 20 KB tiles with headroom)
    {
    // String members are now initialized in initializer list
  }

  // wifiSSID / wifiPassword removed 2026-07-20 — dead legacy single-network
  // fields; saved credentials live in gWifiNetworks[] (network.wifi.networks).
  bool wifiEnabled;        // Enable/disable WiFi at boot (default: true)
  bool wifiAutoReconnect;
  int webCliHistorySize;   // Web CLI history buffer size
  int oledCliHistorySize;  // OLED CLI history buffer size (lines)
  String ntpServer;
  int tzOffsetMinutes;
  // The one persisted output lane: quiet the UART. The other gOutputFlags
  // lanes are runtime state (WEB tracks the HTTP server lifecycle, FILE
  // follows `log start`/`log stop`, BLE/G2 are opt-in per session via
  // outble/outg2). outWeb/outDisplay/outG2 persisted lanes were removed
  // pre-1.0: delivery never honored them — see the System_Debug.h charter.
  bool outSerial;
  // Notification presentation (System_Notifications): per-sink master
  // switches. Per-kind device levels live in /system/notifications.json and
  // per-user mutes in each user's settings file — see System_Notifications.h.
  bool notifBanners;   // OLED transient banners
  bool notifToasts;    // web SSE toasts
  bool notifQueue;     // notification-center queue view
  bool notifG2;        // G2 lens cards
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
  int inputDevicePollMs;
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
  bool debugHttps;   // Gates ESP-IDF TLS/HTTPS log verbosity (see DEBUG_HTTPS)
  bool debugSse;
  bool debugCli;
  bool debugAuth;
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
  bool debugNotifications;    // Notification pipeline diagnostics (ring lag/skips, SSE saturation)
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
  bool debugInput;       // Input abstraction layer (HAL_Input + OLED dispatch)
  bool debugAnoEncoder;  // ANO Rotary Encoder driver internals
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
  bool debugInputLifecycle;
  bool debugInputPolling;
  bool debugInputValues;
  bool debugAnoEncoderLifecycle;
  bool debugAnoEncoderPolling;
  bool debugAnoEncoderValues;
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
  // Preferred ESP-NOW radio channel. ESP-NOW peers can only hear each other on
  // the same channel, and on a single-radio ESP32 the STA association owns the
  // channel. 0 = auto (follow the joined AP; pin the fallback channel when NOT
  // joined). 1..13 = force this channel whenever the STA is NOT associated, so
  // two off-grid devices set to the same value deterministically find each
  // other away from a shared home AP. Ignored while the STA is joined to an AP
  // (the AP's channel wins — a radio can't be on two channels at once).
  uint8_t espnowChannel;               // 0=auto, 1-13=preferred channel when offline
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
  // Secure sensor fetcher (worker role) — docs/ESPNOW_SENSOR_FETCHER_DESIGN.md
  bool   espnowAcceptSensorControl;       // opt-in: honor SENSOR_REQ from master/backup (default false)
  String espnowMasterFingerprint;         // authorized PRIMARY-master Ed25519 pubkey (64-hex; empty=deny)
  String espnowBackupMasterFingerprint;   // authorized BACKUP-master Ed25519 pubkey (64-hex; empty=deny)
  // ============================================================================
  // Multi-mesh data model (Phase 2 of docs/ESPNOW_V4_PLAN.md)
  // ============================================================================
  // A device can belong to up to N_MESHES independent meshes simultaneously
  // (home + work + lab, etc.). Each mesh has its own passphrase, group key,
  // and peer set. The V4 frame header carries a `meshFingerprint` (CRC16 of
  // the label) so receivers know which mesh a frame belongs to regardless
  // of local-index differences between devices.
  //
  // meshes[0] is the "primary" mesh. A device can participate in up to N_MESHES
  // independent meshes simultaneously.
  static constexpr uint8_t N_MESHES = 4;

  struct MeshIdentity {
    String   label;                    // Human-readable name, e.g. "primary", "work"
    String   passphrase;               // User-typed (raw). 3.6 will remove this from persistence — only ever
                                       //   held in RAM long enough to (re)compute passphraseStretchedKey.
    uint16_t fingerprint;              // CRC16-CCITT of label — stamped in V4 header meshFingerprint
    bool     enabled;                  // Inactive meshes are skipped in heartbeat emission, RX validation
    bool     isDefault;                // The mesh new pairings join unless overridden
    // 32-byte mesh master key: PBKDF2-HMAC-SHA256(passphrase,
    //   salt=SHA256("espnow-v4-mesh-salt:"||label), 100k iters). Cached so boot
    //   skips the slow stretch; re-derived on passphrase change or label rename.
    //   Persisted AES-encrypted at rest (putSecret), same as `passphrase`. This is
    //   a derived KEY (the source of the bootstrap/group KDFs) — NOT a one-way
    //   verification hash, despite the old "passphraseHashPbkdf2" name.
    uint8_t  passphraseStretchedKey[32];
    bool     passphraseStretchedKeyValid;      // false until first stretch completes (lazy boot-time fill)
    MeshIdentity()
      : label(""), passphrase(""), fingerprint(0), enabled(false), isDefault(false),
        passphraseStretchedKey{0}, passphraseStretchedKeyValid(false) {}
  };
  MeshIdentity meshes[N_MESHES];

#if ENABLE_BONDED_MODE
  // Bond mode settings (two-device bonded pair)
  // Phase 2: arrays indexed by meshId. Each mesh can have its own bond.
  // [0] is the primary mesh's bond — mirrored from the legacy scalar
  // fields below during Phase 2.1.
  bool    bondModeEnabledMesh[N_MESHES] = {};   // all false by default
  uint8_t bondRoleMesh[N_MESHES] = {};           // per-mesh bond role; 0 = worker
  String  bondPeerMacMesh[N_MESHES];             // per-mesh bond peer MAC (String default-constructs to "")
  // Legacy single-mesh fields (still read by most callers as of Phase 2.1):
  bool bondModeEnabled;              // Enable bond mode (master/worker)
  uint8_t bondRole;                  // 0=worker (compute/network), 1=master (display/gamepad)
  String bondPeerMac;                // MAC address of bonded peer device
  // Bond mode sensor streaming (auto-enable on boot when bonded)
  bool bondStreamThermal;              // Auto-stream thermal data to bonded peer
  bool bondStreamTof;                  // Auto-stream ToF data to bonded peer
  bool bondStreamImu;                  // Auto-stream IMU data to bonded peer
  bool bondStreamGps;                  // Auto-stream GPS data to bonded peer
  bool bondStreamInput;              // Auto-stream input device (gamepad/ANO) data to bonded peer
  bool bondStreamFmradio;              // Auto-stream FM radio data to bonded peer
  bool bondStreamRtc;                  // Auto-stream RTC data to bonded peer
  bool bondStreamPresence;             // Auto-stream presence sensor data to bonded peer
#endif
  // ESP-NOW buffer size settings (runtime tuning)
#if ENABLE_AUTOMATION
  bool automationsEnabled;  // Enable/disable automation scheduler (runs from main loop)
#endif
  // I2C Hardware system
  // bus 0 = primary STEMMA QT — Wire1 internally, "I2C1" in the UI.
  bool i2cBusEnabled;       // Enable/disable bus 0 hardware (Wire1 init and transactions)
  int i2cSdaPin = I2C_SDA_PIN_DEFAULT;  // bus 0 SDA pin (board-specific, see System_BuildConfig.h)
  int i2cSclPin = I2C_SCL_PIN_DEFAULT;  // bus 0 SCL pin (board-specific, see System_BuildConfig.h)
  // bus 1 = secondary STEMMA QT — Wire internally, "I2C2" in the UI. Only the
  // FeatherS3[D] currently has a second port wired. On boards without one,
  // I2C2_*_PIN_DEFAULT is -1, i2c2BusEnabled stays false, and the dual-bus
  // system in System_I2C_Manager.cpp skips bus 1 init / scans / transactions.
  bool i2c2BusEnabled;      // Enable/disable bus 1 hardware (Wire init and transactions)
  int i2c2SdaPin = I2C2_SDA_PIN_DEFAULT;  // bus 1 SDA pin (-1 = unavailable on this board)
  int i2c2SclPin = I2C2_SCL_PIN_DEFAULT;  // bus 1 SCL pin (-1 = unavailable on this board)
  // Per-device bus assignment (0 = I2C1 / Wire1 / primary STEMMA QT;
  //                            1 = I2C2 / Wire / secondary STEMMA QT).
  // All default to 0 so existing single-bus configurations work unchanged.
  // Setting any of these to 1 requires (a) i2c2BusEnabled = true and (b) the
  // board to have valid I2C2 pins. Reboot required after change — sensor
  // tasks cache their bus assignment at init time and the underlying
  // TwoWire / library object is bound at construction.
  //
  // Stored as `int` (not uint8_t) to match the existing SETTING_INT registry
  // type — the registry writes through a void* assuming 4-byte storage, so
  // a uint8_t field would corrupt adjacent bytes.
  int oledBus;              // SSD1306 display (HAL_Display / OLED_Utils)
  int inputBus;             // Input device (gamepad or ANO encoder — only one compiled in)
  int gpsBus;               // PA1010D mini GPS
  int rtcBus;               // DS3231 precision RTC
  int fmRadioBus;           // RDA5807 FM radio
  int presenceBus;          // STHS34PF80 IR presence/motion
  int imuBus;               // BNO055 9-axis IMU (compiled out by default)
  int thermalBus;           // MLX90640 thermal camera (compiled out)
  int tofBus;               // VL53L4CX time-of-flight (compiled out)
  int apdsBus;              // APDS9960 gesture/proximity (compiled out)
  int servoBus;             // PCA9685 16-channel servo (compiled out)
  int fuelGaugeBus;         // MAX17048G LiPo fuel gauge (FeatherS3[D] on-board)
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
  bool oledFlipped;             // Rotate display 180° (true → setRotation(2), false → setRotation(0))
  float oledThermalScale;       // Scaling factor for thermal image (2.5 = 80x60)
  String oledThermalColorMode;  // Visualization style: 3level, 2level, gradient
  // Sensor Auto-Start settings (all I2C sensors)
  bool inputAutoStart;          // Auto-start the input device (gamepad or ANO) after boot
  int  anoEncoderI2cAddr;       // ANO I2C address override (default 0x49 = I2C_ADDR_ANO_ENCODER)
  bool anoEncoderInvert;        // Flip rotation direction if encoder is mounted backwards
  bool anoEncoderSwapUpDown;    // Swap UP/DOWN button bits (for vertically-mirrored mounts)
  bool anoEncoderSwapLeftRight; // Swap LEFT/RIGHT button bits (for horizontally-mirrored mounts)
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
  String sensorLogPath;         // Last-used log file path (default: CAPTURE_SENSORLOG_DEFAULT)
  int sensorLogIntervalMs;      // Last-used polling interval in ms (default: 5000)
  int sensorLogMask;            // Last-used sensor bitmask (0=none)
  int sensorLogFormat;          // Last-used format (0=text, 1=csv, 2=track)
  int sensorLogMaxSize;         // Max log file size before rotation (bytes; default: 256000)
  int sensorLogMaxRotations;    // Old rotated logs to keep (0-9; default: 3)
  // System Logging settings
  bool systemLogAutoStart;      // Auto-start system logging after boot
  bool eventLogEnabled;         // Structured event-ring history → /system/sys_logs/events.log
  String systemLogPath;         // Log file path (empty = auto-generate with timestamp)
  bool systemLogCategoryTags;   // Prefix log lines with [CATEGORY] tags
  String systemLogFlags;        // Last-used debug flag mask for system log (empty = leave gDebugFlags)
  bool cameraAutoStart;         // Auto-start ESP32-S3 camera after boot
  bool microphoneAutoStart;     // Auto-start ESP32-S3 PDM microphone after boot
  // NOTE: kept OUTSIDE the #if ENABLE_ONDEVICE_LLM block below so the always-
  // compiled feature registry can reference &gSettings.llmAutoStart in any build.
  bool llmAutoStart;            // Auto-load the default LLM model at boot (default: false)
  // Microphone settings
  int microphoneSampleRate;     // Sample rate in Hz (8000, 16000, 22050, 44100, 48000) — PDM only
  int microphoneGain;           // Software gain 0-100% (default 90)
  int microphoneBitDepth;       // Bit depth (cosmetic; HAL/WAV are always 16-bit int)
  String micSource;             // Preferred mic source: "auto" | "pdm" | "g2" (resolved lazily against availability)
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
  // Per-transport idle-logout windows (minutes). Auto-logout an authenticated
  // session after N min with no REAL interaction; 0 = disabled. Same policy
  // everywhere (sessionIdleExpired in System_User.cpp), independent knobs.
  uint32_t sessionIdleWeb;      // Web session idle window (default 60)
  uint32_t sessionIdleSerial;   // Serial session idle window (default 60)
  uint32_t sessionIdleBle;      // BLE per-connection idle window (default 15)
  uint32_t sessionIdleDisplay;  // OLED/local-display session idle window (default 60)
  // Bluetooth settings
  bool bluetoothAutoStart;      // Auto-start Bluetooth at boot (enables BLE server)
  bool bluetoothRequireAuth;    // Require login before accepting BLE commands (always required, per-connection)
  String bleDeviceName;         // BLE advertised device name (default: "HardwareOne")
  int bleTxPower;               // BLE TX power level 0-7 (0=min, 7=max, default: 3)
  int bleMode;                  // BLE role: 0=server (phone peripheral), 1=G2 client (central). Mutually exclusive at runtime.
  bool bleRequireSecureChannel; // Require the app-layer Secure Channel (server mode); refuse plaintext commands when set
  String bleSecureChannelSecret;// Pre-shared passphrase for the Secure Channel (PBKDF2 -> PSK). Secret.
  // BLE peer MACs and auto-reconnect flags now live in BLE_Peers.cpp's
  // gBlePeerData[BLE_PEER_MAX] array, persisted under "bluetooth.peers"
  // in settings.json. See BLE_Peers.h. Removed flat keys:
  //   bleGlassesLeftMAC, bleGlassesRightMAC, bleRingMAC, blePhoneMAC,
  //   g2AutoConnect, ringAutoConnect
  // Power Management settings
  uint8_t powerMode;            // 0=Performance(240MHz), 1=Balanced(160MHz), 2=PowerSaver(80MHz), 3=UltraSaver(80MHz active / 40MHz idle-only)
  bool powerAutoMode;           // Auto-adjust power mode based on battery level
  uint8_t powerBatteryThreshold; // Switch to power saver below this battery % (default: 20)
  uint8_t powerDisplayDimLevel; // Brightness % in power saver modes (0-100, default: 30)
  uint32_t powerTransitionCooldownMs; // Anti-flap cooldown between sleep entries (ms); 0 disables
  uint32_t powerSaveTimeoutMinutes;   // Idle power-save: blank OLED + 80MHz downclock after N min idle; 0 = disabled
  bool     batteryLogEnabled;         // Auto-log battery time-series CSV to /battery.csv (default on)
  uint32_t batteryLogIntervalMs;      // Battery-log sample period in ms (default 60000)
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
  bool mqttPublishInput;      // Include input device data (default: false)

  // Crash / reset tracking (persisted from RTC memory on next healthy boot)
  uint32_t crashCount;          // Accumulated abnormal resets (WDT, panic, brownout)
  uint32_t lastResetReason;     // esp_reset_reason_t value from last boot
#if ENABLE_ONDEVICE_LLM
  // On-device LLM generation defaults (System_LLM)
  float llmTemperature;         // Sampling temperature (default: 0.5)
  float llmTopP;                // Nucleus sampling threshold (default: 0.8)
  float llmMinP;                // Min-p relative floor (0 = off; typical 0.05-0.1). When >0, replaces top-p.
  int llmKvPrecision;           // KV cache precision: 0=FP32, 1=FP16 (default: 2x context in the same PSRAM, ~no quality loss), 2=INT8. Applied at model load.
  int llmNoRepeatNgram;         // Ban tokens completing an already-generated n-gram (default 0=off; 3 typical). Breaks verbatim phrase loops the rep penalty exempts.
  float llmConfThreshold;       // Confidence gate: prefix "I'm not sure, but" when mean logprob of the first tokens < this (0=off, default -1.0). Calibrated for temp 0.5.
  float llmContentBoost;        // Logit bonus for prompt content tokens (default 1.5, 0=off). Keeps answers on-topic; co-tune with repPenalty/noRepeatNgram (it exempts+favors these tokens).
  int llmMaxTokens;             // Max tokens per generation (default: 256)
  int llmSentenceLimit;         // Stop after N sentences, 0=disabled (default: 2)
  int llmHardCap;               // Hard token cap, 0=disabled (default: 80)
  float llmRepPenalty;          // Repetition penalty divisor (default: 1.3)
  int llmRepWindow;             // Look-back window for rep penalty (default: 32)
  int llmMaxContext;            // KV cache context window, 0=use compile-time default
  String llmDefaultModel;       // Default model filename for auto-load (default: "model.bin")
  bool llmDomainGate;           // Refuse generation when the prompt matches none of the model's embedded domain vocab (default: on). Only enforced when the loaded .bin carries a domain vocab section.
  bool llmProfile;              // Per-section forward-pass timing breakdown, dumped at end of each generation (default: off). Diagnostic only; leave off for real tok/s.
#endif

  // Maps app — persisted defaults applied at boot to gMapZoom / gVisibleLayers
  // and used to size the tile cache pool. Cache size requires a reboot to
  // take effect; zoom and layers apply live via their setters.
  float mapZoom;             // Default zoom level (1.0 = identity)
  int   mapVisibleLayers;    // Layer-visibility bitmask (10 bits, default 0x3FF = LAYER_ALL)
  int   mapCacheSizeKB;      // Tile cache pool size in KB (default 1280)
};

// Global settings instance (defined in .ino)
extern Settings gSettings;

// ============================================================================
// Settings Management Functions (implemented in settings.cpp)
// ============================================================================

// Settings initialization and defaults
void settingsDefaults();

// Debug flags persist in their OWN file, split out of settings.json: the 157
// debug flags were ~half of the shared 5120-byte settings doc and every toggle
// rewrote the whole (large) settings.json. A compile-time literal (not a
// const char* like SETTINGS_JSON_FILE) so it can be used in the static
// debugSettingsModule initializer with no static-init-order dependency.
#define DEBUG_JSON_FILE "/system/debug.json"

// Settings JSON serialization/deserialization.
// mainFileOnly=true excludes own-file modules (debug) — used only by the
// settings.json writer. The web/bond/G2 builders keep the default (all modules)
// so those in-RAM payloads still carry system.debug after the split.
void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords = false, bool mainFileOnly = false);
bool readSettingsJson();
bool writeSettingsJson();
// Mark this boot's settings state trustworthy for full-array rewrites when
// there is no settings.json to load (first boot / post-erase). readSettingsJson
// sets the same flag itself on a successful load; while unset, the save path
// skips the wifi-networks rebuild so it can't wipe the file after a failed load.
void settingsMarkLoadedOk();
bool writeDebugJson();   // persist the debug module to DEBUG_JSON_FILE
bool readDebugJson();    // load the debug module from DEBUG_JSON_FILE

// Fire SYSEVT_SETTING_CHANGED for a field mutated via setSetting(). setSetting() is
// name-agnostic (it only has the field reference), so this reverse-looks-up the
// registered SettingEntry by valuePtr for the setting's name + type. A field with no
// registered entry (internal/runtime state) fires nothing. Defined in System_Settings.cpp.
void notifySettingChanged(const void* fieldPtr);

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
    notifySettingChanged(&field);  // fire setting_changed for the registered setting (if any)
  }
}

// String overload (avoids unnecessary copy when equal)
inline void setSetting(String& field, const String& value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeSettingsJson();
    notifySettingChanged(&field);
  }
}

// const char* convenience overload
inline void setSetting(String& field, const char* value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeSettingsJson();
    notifySettingChanged(&field);
  }
}

// Debug-module fields persist to DEBUG_JSON_FILE, not settings.json. This mirrors
// setSetting() but routes the auto-write to writeDebugJson(), so a debug toggle
// only rewrites the small debug file. Used by every debug command in
// System_Debug.cpp. (writeDebugJson() is declared above.)
template<typename T>
inline void setDebugSetting(T& field, const T& value) {
  if (field != value) {
    field = value;
    if (!gDeferWrites) writeDebugJson();
  }
}

// AES-128-CBC string encryption/decryption (device-bound key)
String encryptString(const String& plaintext);
String decryptString(const String& encrypted);
String getDeviceEncryptionKey();

// JSON-field secret helpers — the uniform way to (de)serialize a *recoverable*
// secret inside an ArduinoJson object: encrypt-on-write / decrypt-on-read with
// the device key. This is the array/struct-field equivalent of a SettingEntry's
// `isSecret` flag (which only covers scalar registered settings). Use these for
// any secret stored inside a JSON array/object — WiFi network passwords, mesh
// passphrases, etc. — so the at-rest encryption can't be forgotten or drift.
// (For binary keys / manually-written files, call encryptString directly.)
void   putSecret(JsonObject obj, const char* key, const String& plaintext);
String getSecret(JsonObjectConst obj, const char* key);

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

// Setting data types.
//
// SETTING_INT means "the field is an int32_t / int". For fields with smaller
// widths (uint8_t / uint16_t / uint32_t) use the explicit-width tags below.
//
// History: prior to the U8/U16/U32 tags being added (2026-05-18), every
// integer field was tagged SETTING_INT and the dispatch code did
// `*((int*)valuePtr) = v` — a 4-byte write through a void*. For a uint8_t
// field that wrote 3 bytes past the intended field, corrupting whatever
// followed it in the struct (typically the next setting field — or worse, a
// String's internal buffer pointer, which crashed at destruct with "free()
// target pointer is outside heap areas"). The fix is to dispatch on actual
// width; the explicit-width tags make sure future entries can't regress.
enum SettingType {
  SETTING_INT,      // int / int32_t (4 bytes, signed)
  SETTING_FLOAT,    // float
  SETTING_BOOL,     // bool (1 byte)
  SETTING_STRING,   // Arduino String
  SETTING_U8,       // uint8_t  (1 byte) — use for fields declared as uint8_t
  SETTING_U16,      // uint16_t (2 bytes)
  SETTING_U32       // uint32_t (4 bytes, unsigned)
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
  // Select / bitmask UI hints (nullptr = none):
  //   "0|Text,1|CSV"              → <select> (value|label pairs; ':' also accepted in older UI)
  //   "bitmask:1|Thermal,2|ToF"   → checkbox grid; values may be decimal or 0xhex.
  //   Optional headers: "bitmask:#|I2C Sensors,0x1|GPS"
  // Stored value is unchanged (int mask or hex string) — only the web renderer differs.
  const char* options;
  bool isSecret = false;      // If true: encrypt on disk, exclude from web API, blank input = unchanged
  const char* group = nullptr;  // Sub-section group for JSON nesting + UI grouping (nullptr = ungrouped)
  const char* cmdKey = nullptr; // CLI command name override (nullptr = use jsonKey as command)
  bool readOnly = false;      // If true: UI renders as display-only text (no input). For system-managed
                              // counters (crashCount, lastResetReason, etc.) that the user reads but never edits.
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
  const char* persistFile = nullptr; // Own persistence file (nullptr = /system/settings.json).
                                     // Set only for modules split into their own file (debug → DEBUG_JSON_FILE).
};

// Maximum number of settings modules that can be registered
#define MAX_SETTINGS_MODULES 32

// Register a settings module (call during setup or static init)
void registerSettingsModule(const SettingsModule* module);

// Get all registered modules
const SettingsModule** getSettingsModules(size_t& count);

// Apply defaults from all registered modules
void applyRegisteredDefaults();

// Read/write settings via the registered modules.
// Scope filter for the settings.json/debug.json split:
//   allFiles=true  → every module (web/bond/G2/schema payloads — the default)
//   allFiles=false → one persistence target: onlyPersistFile==nullptr selects the
//                    main settings.json modules (skips debug); otherwise only
//                    modules whose persistFile matches (debug → DEBUG_JSON_FILE).
// Returns the number of settings read/written.
size_t readRegisteredSettings(JsonDocument& doc, const char* onlyPersistFile = nullptr, bool allFiles = true);
size_t writeRegisteredSettings(JsonDocument& doc, const char* onlyPersistFile = nullptr, bool allFiles = true);

// Register ALL settings modules explicitly (called once early in boot)
// Ensures all compiled modules are available before applying defaults
void registerAllSettingsModules();

// Debug: print a summary of all registered settings modules
void printSettingsModuleSummary();

// Generic setting command handler - parses value and updates setting
// Returns result message
const char* handleSettingCommand(const SettingEntry* entry, const String& argsInput);

// Look up an editable setting by its (unique) editor cmdKey.
const SettingEntry* findSettingByCmdKey(const char* cmdKey);

// Static per-field settings-editor commands (one per editable setting that has
// no dedicated module command). Registered via gCommandModules.
extern const CommandEntry settingEditorCommands[];
extern const size_t settingEditorCommandsCount;

// Build the settings schema JSON into a JsonDocument. Shared by the local
// /api/settings/schema web handler (in WebServer_Server.cpp) and the worker's
// sendBondSchema() which serializes this to a file for the bond transport
// (in System_ESPNow.cpp). Two consumers, one source of truth — the master
// and worker emit identical JSON shapes by construction.
void buildSettingsSchemaJson(JsonDocument& doc);

// Persist current settings to JSON file
bool writeSettingsJson();

// Apply persisted settings to runtime flags/state
void applySettings();

#endif // SYSTEM_SETTINGS_H
