#include "System_RamFlush.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <string.h>

#include "System_AuthIdentity.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_Settings.h"
#include "System_Utils.h"

// Live-state getters. Each include is the one that owns the flag we read; see the
// per-feature rulings in docs/RAMFLUSH_IMPLEMENTATION_MAP.md §1.7 — several of the
// obvious-looking getters are wrong and were pinned deliberately.
#include "System_Automation.h"
#include "System_I2C.h"
#include "System_SensorStubs.h"
#include "WebServer_Handle.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_Display.h"
#endif

// Per-sensor enabled-flag externs. System_SensorStubs.h above only supplies the
// flags for sensors compiled OUT; the real declarations live in each sensor's own
// header (gated on its ENABLE_*). Without these the file builds only on configs
// where every sensor happens to be disabled — e.g. the XIAO Sense
// (I2C_FEATURE_LEVEL 0) it was developed against — and fails the moment a sensor
// is turned on (FeatherS3 → gThermalRunning/gGpsRunning/gRtcRunning undeclared).
// Same include block System_TaskUtils.cpp uses for the same reason.
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_FM_RADIO
#include "i2csensor_rda5807.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif

#if ENABLE_MQTT
#include "System_MQTT.h"
#endif
#if ENABLE_BLUETOOTH
#include "Bluetooth.h"
#endif
#if ENABLE_ONDEVICE_LLM
#include "System_LLM.h"
#endif
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"
#endif
#if ENABLE_MICROPHONE
#include "System_Microphone.h"
#endif
#if ENABLE_ESP_SR
#include "System_ESPSR.h"
#endif
#if ENABLE_ESPNOW
#include "System_ESPNow_Sensors.h"
#endif
#include "System_SensorLogging.h"

// ============================================================================
// RTC-backed overlay
// ============================================================================
// Separate RTC_NOINIT scalars rather than a packed struct, matching the existing
// style at HardwareOne.cpp:1146-1154 and sidestepping padding surprises.
RTC_NOINIT_ATTR static uint32_t rtcRfMagic;
RTC_NOINIT_ATTR static uint16_t rtcRfLayoutVersion;
RTC_NOINIT_ATTR static uint16_t rtcRfCrc;
RTC_NOINIT_ATTR static uint32_t rtcRfDivergeMask;  // bit i => feature i diverged from intent
RTC_NOINIT_ATTR static uint32_t rtcRfLiveMask;     // bit i => its observed live value

static const uint32_t RAMFLUSH_OVERLAY_MAGIC  = 0x52414D46;  // 'RAMF'
static const uint16_t RAMFLUSH_LAYOUT_VERSION = 2;           // bump on ANY enum change

static_assert(RF_FEATURE_COUNT <= 32, "RamFlushFeatureId overflows the uint32_t masks");

// Session state, plain RAM. Process lifetime, NOT a setup() local: the OLED input
// path re-resolves long after boot (OLED_Utils.cpp:6004).
static bool     sOverlayActive     = false;
static uint32_t sDivergeMask       = 0;
static uint32_t sLiveMask          = 0;
static uint32_t sAutostartFailed   = 0;

static uint16_t ramFlushCrc(uint32_t diverge, uint32_t live, uint16_t layout) {
  // Not a checksum for integrity against an attacker — just enough to reject RTC
  // that is uninitialised garbage rather than something we wrote.
  uint32_t h = 0x811C9DC5u;
  const uint32_t parts[3] = { diverge, live, (uint32_t)layout };
  for (int i = 0; i < 3; i++) {
    h ^= parts[i];
    h *= 0x01000193u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

// ---------------------------------------------------------------------------
// Live-state read
// ---------------------------------------------------------------------------
// An explicit switch, not a table of pointers: a pointer table would reintroduce
// the aliasing hazard that blocks consting FeatureEntry::enabledSetting.
// Returns false for features not compiled into this build — paired with an intent
// that is likewise false, so they never diverge and never enter the overlay.
static bool ramFlushReadLive(RamFlushFeatureId f) {
  switch (f) {
    case RF_THERMAL:    return gThermalRunning;
    case RF_TOF:        return gTofRunning;
    case RF_IMU:        return gImuRunning;
    case RF_GPS:        return gGpsRunning;
    case RF_FMRADIO:    return gFmRadioRunning;

    // NEVER gApdsRunning — it is defined and externed but never assigned true, so
    // it would read false forever and record a spurious "user turned APDS off" on
    // every capture. The sub-flags are the ones actually maintained.
#if ENABLE_APDS_SENSOR
    case RF_APDS:       return (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning);
#else
    case RF_APDS:       return false;
#endif

    case RF_INPUT:      return gInputRunning;
    case RF_RTC:        return gRtcRunning;
    case RF_PRESENCE:   return gPresenceRunning;

#if ENABLE_CAMERA_SENSOR
    case RF_CAMERA:     return gCameraRunning;
#else
    case RF_CAMERA:     return false;
#endif

#if ENABLE_ESP_SR
    case RF_SR:         return isESPSRRunning();
#else
    case RF_SR:         return false;
#endif

#if ENABLE_MICROPHONE
    case RF_MICROPHONE: return gMicRunning;
#else
    case RF_MICROPHONE: return false;
#endif

    case RF_HTTP:       return isHttpServerRunning();

#if ENABLE_ONDEVICE_LLM
    // READY alone is insufficient: GENERATING is a distinct state, so a capture
    // taken mid-generation would read a loaded model as unloaded.
    case RF_LLM: {
      LLMState s = llmGetStatus().state;   // llmGetStatus() returns LLMStatus{ state, ... }
      return (s == LLMState::READY || s == LLMState::GENERATING);
    }
#else
    case RF_LLM:        return false;
#endif

#if ENABLE_MQTT
    // isMqttStarted(), never isMqttConnected() — the latter is broker-connected,
    // which is async and reads false mid-handshake.
    case RF_MQTT:       return isMqttStarted();
#else
    case RF_MQTT:       return false;
#endif

#if ENABLE_BLUETOOTH
    // NEVER isBLERunning(): it deliberately reports true for bare controller
    // activity, so a G2/ring client auto-reconnect (bleAutoStart == false)
    // would look like divergence and the next boot would replay SERVER init,
    // silently changing the user's BLE role. gBLEState->initialized is
    // server-mode-only, which is what bleAutoStart actually controls.
    case RF_BLUETOOTH:  return (gBLEState && gBLEState->initialized);
#else
    case RF_BLUETOOTH:  return false;
#endif

    case RF_SENSORLOG:  return gSensorLoggingRunning;
    case RF_SYSTEMLOG:  return gSystemLogRunning;

#if ENABLE_ESPNOW
    case RF_ESPNOW:     return isEspNowInitialized();
#else
    case RF_ESPNOW:     return false;
#endif

    case RF_WIFI:       return WiFi.isConnected();

#if ENABLE_OLED_DISPLAY
    // gOledRunning, not oledConnected: the latter is "a panel answered on I2C",
    // which stays true after oledstop. gOledRunning is what oledAutoStart
    // actually controls.
    case RF_OLED:       return gOledRunning;
#else
    case RF_OLED:       return false;
#endif

#if ENABLE_AUTOMATION
    // The scheduler's own runtime flag, not automationEnabled: the setting is
    // the permission, this is whether the cache is live and the loop is
    // ticking — which is what automationAutoStart controls.
    case RF_AUTOMATION: return gAutomationSchedulerRunning;
#else
    case RF_AUTOMATION: return false;
#endif

    default:            return false;
  }
}

// The intent each feature's live state is diffed against. For most this is the
// *AutoStart flag; WiFi is keyed to wifiAutoStart because wifiEnabled is inert
// (persisted and read back, but nothing applies it — boot gates on
// wifiAutoStart at HardwareOne.cpp:1606), and espnow is a master-enable.
static bool ramFlushReadIntent(RamFlushFeatureId f) {
  switch (f) {
    case RF_THERMAL:    return gSettings.thermalAutoStart;
    case RF_TOF:        return gSettings.tofAutoStart;
    case RF_IMU:        return gSettings.imuAutoStart;
    case RF_GPS:        return gSettings.gpsAutoStart;
    case RF_FMRADIO:    return gSettings.fmRadioAutoStart;
    case RF_APDS:       return gSettings.apdsAutoStart;
    case RF_INPUT:      return gSettings.inputAutoStart;
    case RF_RTC:        return gSettings.rtcAutoStart;
    case RF_PRESENCE:   return gSettings.presenceAutoStart;
    case RF_CAMERA:     return gSettings.cameraAutoStart;
    case RF_SR:         return gSettings.srAutoStart;
    case RF_MICROPHONE: return gSettings.micAutoStart;
    case RF_HTTP:       return gSettings.httpAutoStart;
    case RF_LLM:        return gSettings.llmAutoStart;
    case RF_MQTT:       return gSettings.mqttAutoStart;
    case RF_BLUETOOTH:  return gSettings.bleAutoStart;
    case RF_SENSORLOG:  return gSettings.sensorLogAutoStart;
    case RF_SYSTEMLOG:  return gSettings.systemLogAutoStart;
    case RF_ESPNOW:     return gSettings.espnowEnabled;
    case RF_WIFI:       return gSettings.wifiAutoStart;
    case RF_OLED:       return gSettings.oledAutoStart;
    case RF_AUTOMATION: return gSettings.automationAutoStart;
    default:            return false;
  }
}

static const char* ramFlushFeatureName(RamFlushFeatureId f) {
  switch (f) {
    case RF_THERMAL:    return "thermal";
    case RF_TOF:        return "tof";
    case RF_IMU:        return "imu";
    case RF_GPS:        return "gps";
    case RF_FMRADIO:    return "fmradio";
    case RF_APDS:       return "apds";
    case RF_INPUT:      return "input";
    case RF_RTC:        return "rtc";
    case RF_PRESENCE:   return "presence";
    case RF_CAMERA:     return "camera";
    case RF_SR:         return "sr";
    case RF_MICROPHONE: return "microphone";
    case RF_HTTP:       return "http";
    case RF_LLM:        return "llm";
    case RF_MQTT:       return "mqtt";
    case RF_BLUETOOTH:  return "bluetooth";
    case RF_SENSORLOG:  return "sensorlog";
    case RF_SYSTEMLOG:  return "systemlog";
    case RF_ESPNOW:     return "espnow";
    case RF_WIFI:       return "wifi";
    case RF_OLED:       return "oled";
    case RF_AUTOMATION: return "automation";
    default:            return "?";
  }
}

// ---------------------------------------------------------------------------
// Capture / consume / resolve
// ---------------------------------------------------------------------------

void ramFlushCaptureOverlay(void) {
  uint32_t diverge = 0;
  uint32_t live = 0;

  for (uint8_t i = 0; i < RF_FEATURE_COUNT; i++) {
    RamFlushFeatureId f = (RamFlushFeatureId)i;

    // A feature that failed to autostart this session is not the user's doing.
    // Recording it would read as "turned off" and suppress a configured autostart
    // on every future boot, with replugging the hardware doing nothing to fix it.
    if (sAutostartFailed & (1u << i)) continue;

    const bool isLive = ramFlushReadLive(f);
    const bool intent = ramFlushReadIntent(f);
    if (isLive == intent) continue;  // untouched — falls through to intent

    diverge |= (1u << i);
    if (isLive) live |= (1u << i);
  }

  rtcRfDivergeMask   = diverge;
  rtcRfLiveMask      = live;
  rtcRfLayoutVersion = RAMFLUSH_LAYOUT_VERSION;
  rtcRfCrc           = ramFlushCrc(diverge, live, RAMFLUSH_LAYOUT_VERSION);
  rtcRfMagic         = RAMFLUSH_OVERLAY_MAGIC;  // last — nothing above is valid without it

  INFO_SYSTEMF("[RamFlush] captured overlay: %u feature(s) diverged from intent",
               (unsigned)__builtin_popcount(diverge));
}

void ramFlushConsumeOverlay(void) {
  const esp_reset_reason_t reason = esp_reset_reason();

  // Two conditions, mirroring HardwareOne.cpp:1301-1303. The magic alone is not
  // enough: RTC_NOINIT is retained across deep sleep, and G2 "Power Off" is an
  // esp_deep_sleep_start() with no wake source (G2_Page_Power.cpp:141-144). On
  // wake that reports ESP_RST_DEEPSLEEP, so a magic-only guard would reapply an
  // overlay to someone who powered the device off and back on. Requiring
  // ESP_RST_SW also means a panic mid-flush drops the overlay — the desired
  // fail-to-intent behaviour.
  const bool swReset  = (reason == ESP_RST_SW);
  const bool haveMagic = (rtcRfMagic == RAMFLUSH_OVERLAY_MAGIC);

  if (swReset && haveMagic &&
      rtcRfLayoutVersion == RAMFLUSH_LAYOUT_VERSION &&
      rtcRfCrc == ramFlushCrc(rtcRfDivergeMask, rtcRfLiveMask, rtcRfLayoutVersion)) {
    sDivergeMask   = rtcRfDivergeMask;
    sLiveMask      = rtcRfLiveMask;
    sOverlayActive = (sDivergeMask != 0);
  } else {
    sDivergeMask   = 0;
    sLiveMask      = 0;
    sOverlayActive = false;
  }

  // Invalidate BEFORE anything is applied. If applying the overlay panics, the
  // next boot finds no overlay and comes up on pure intent, so a feature that
  // wedged the device cannot be restored into a boot loop. This is the whole
  // safety argument — do not move it after the apply sites.
  rtcRfMagic       = 0;
  rtcRfDivergeMask = 0;
  rtcRfLiveMask    = 0;
  rtcRfCrc         = 0;
}

bool ramFlushResolve(RamFlushFeatureId f, bool intent) {
  if (!sOverlayActive || f >= RF_FEATURE_COUNT) return intent;
  const uint32_t bit = (1u << (uint8_t)f);
  if (!(sDivergeMask & bit)) return intent;
  return (sLiveMask & bit) != 0;
}

bool ramFlushOverlayActive(void) { return sOverlayActive; }

void ramFlushClearOverlay(void) {
  rtcRfMagic       = 0;
  rtcRfDivergeMask = 0;
  rtcRfLiveMask    = 0;
  rtcRfCrc         = 0;
  sDivergeMask     = 0;
  sLiveMask        = 0;
  sOverlayActive   = false;
}

RamFlushFeatureId ramFlushIdForModule(const char* moduleName) {
  if (!moduleName) return RF_FEATURE_COUNT;
  // Names as passed to isSensorAvailableForAutoStart() (System_I2C.cpp:3002-3090).
  // The input device's module name is "input" (post-unification) — it must match
  // the string System_I2C.cpp passes to isSensorAvailableForAutoStart(), which is
  // this function's only caller. A mismatch returns RF_FEATURE_COUNT and silently
  // no-ops ramFlushMarkAutostartFailed().
  if (!strcmp(moduleName, "thermal"))  return RF_THERMAL;
  if (!strcmp(moduleName, "tof"))      return RF_TOF;
  if (!strcmp(moduleName, "imu"))      return RF_IMU;
  if (!strcmp(moduleName, "gps"))      return RF_GPS;
  if (!strcmp(moduleName, "fmradio"))  return RF_FMRADIO;
  if (!strcmp(moduleName, "apds"))     return RF_APDS;
  if (!strcmp(moduleName, "input"))    return RF_INPUT;
  if (!strcmp(moduleName, "rtc"))      return RF_RTC;
  if (!strcmp(moduleName, "presence")) return RF_PRESENCE;
  return RF_FEATURE_COUNT;
}

void ramFlushMarkAutostartFailed(RamFlushFeatureId f) {
  if (f < RF_FEATURE_COUNT) sAutostartFailed |= (1u << (uint8_t)f);
}

void ramFlushClearAutostartFailed(RamFlushFeatureId f) {
  if (f < RF_FEATURE_COUNT) sAutostartFailed &= ~(1u << (uint8_t)f);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static const char* ramFlushStatusReport() {
  static String out;
  out = "";

  if (!sOverlayActive) {
    out = "OK: [RamFlush] No overlay in effect — this boot used configured autostart only.";
    return out.c_str();
  }

  out = "OK: [RamFlush] Overlay applied this boot (";
  out += String(__builtin_popcount(sDivergeMask));
  out += " feature(s) differ from configured autostart):\n";
  for (uint8_t i = 0; i < RF_FEATURE_COUNT; i++) {
    if (!(sDivergeMask & (1u << i))) continue;
    const bool on = (sLiveMask & (1u << i)) != 0;
    out += "  ";
    out += ramFlushFeatureName((RamFlushFeatureId)i);
    out += " = ";
    out += (on ? "on" : "off");
    out += "  (autostart says ";
    out += (ramFlushReadIntent((RamFlushFeatureId)i) ? "on" : "off");
    out += ")\n";
  }
  out += "This applies to this boot only. A normal reboot returns to configured autostart.";
  return out.c_str();
}

const char* cmd_ramflush(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
  args.trim();
  if (args.equalsIgnoreCase("status")) {
    return ramFlushStatusReport();
  }
  if (args.length() > 0) {
    return "Error: [RamFlush] Unknown argument. Usage: ramflush [status]";
  }

  broadcastOutput("Capturing running features and rebooting...");

  // The only difference from cmd_reboot. Deliberately here and not inside
  // rebootDevice(), so a plain reboot — and factoryreset, which reboots on its own
  // deferred timer — never acquire resume semantics.
  ramFlushCaptureOverlay();

  char detail[96];
  snprintf(detail, sizeof(detail), "commanded RAM-flush restart by '%s'", currentExecUser().c_str());
  rebootDevice("ramflush", detail, 1000);
  return "[System] RAM flush reboot";  // Won't actually return due to restart
}

// Registered in systemCommands[] (System_Utils.cpp) next to `reboot`.
