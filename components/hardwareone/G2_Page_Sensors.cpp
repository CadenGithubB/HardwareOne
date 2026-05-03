// =============================================================================
// G2 glasses — "Sensors" page implementation
// =============================================================================
// See G2_Page_Sensors.h for the contract. This file owns:
//   1. The sensor registry table — one row per known sensor with the matching
//      build flag, runtime-state extern names, and a short display label.
//   2. The formatter that walks that table and produces the rendered text.
//   3. The high-level `g2ShowSensorList()` helper that builds + ships in one
//      call (used by both the CLI and the hijack-menu tap handler).
//
// Adding a new sensor: add an entry to kSensorRows[] below, plus its
// extern declaration if the build flag is on. No changes needed in
// G2_Glasses.cpp.

#include "G2_Page_Sensors.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"            // g2ShowText, g2ShowListPage, etc.
#include "System_Debug.h"
#include "System_FeatureRegistry.h"     // getFeatureById, FeatureEntry
#include "System_Settings.h"            // setSetting, gSettings
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
// Per-sensor extern declarations + cache headers
// -----------------------------------------------------------------------------
// Each sensor module owns its own gXEnabled / gXConnected pair AND a
// gXCache struct populated by its polling task. The OLED and web
// renderers already consume those caches; we do the same here to
// surface the most useful single value per sensor on the lens.
// Guarded by the same compile flags the sensor uses, so a stripped
// build doesn't reference the cache symbols.

#if ENABLE_IMU_SENSOR
#include "i2csensor-bno055.h"     // ImuCache, gImuCache
extern bool gImuEnabled;       extern bool gImuConnected;
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx.h"   // TofCache, gTofCache
extern bool gTofEnabled;       extern bool gTofConnected;
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640.h"   // ThermalCache, gThermalCache
extern bool gThermalEnabled;   extern bool gThermalConnected;
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"     // GamepadCache, gGamepadCache
extern bool gGamepadEnabled;   extern bool gGamepadConnected;
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor-apds9960.h"   // APDSCache, gAPDSCache
extern bool gApdsEnabled;      extern bool gApdsConnected;
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor-ds3231.h"     // RTCCache, gRTCCache
extern bool gRtcEnabled;       extern bool gRtcConnected;
#endif
#if ENABLE_FM_RADIO
#include "i2csensor-rda5807.h"    // FMRadioCache, gFmRadioCache
extern bool gFmRadioEnabled;   extern bool gFmRadioConnected;
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor-pa1010d.h"    // GPSCache, gGPSCache
extern bool gGpsEnabled;       extern bool gGpsConnected;
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor-sths34pf80.h" // PresenceCache, gPresenceCache
extern bool gPresenceEnabled;  extern bool gPresenceConnected;
#endif
#if ENABLE_OLED_DISPLAY
extern bool gOledEnabled;
#endif
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"     // initCamera, stopCamera, gCameraEnabled
#include "G2_Page_CameraSettings.h" // g2ShowCameraSettingsMenu
#endif
#if ENABLE_MICROPHONE_SENSOR
#include "System_Microphone.h"     // initMicrophone, stopMicrophone, gMicEnabled
#endif

// -----------------------------------------------------------------------------
// Per-sensor "current value" formatters
// -----------------------------------------------------------------------------
// One formatter per compiled-in sensor. Each takes the cache mutex with a
// short timeout (5 ms) — same pattern as the OLED renderers. Output is
// limited to ~10 chars to keep total list pb body under the
// single-fragment CREATE ceiling (~253 B).
//
// Returns the buffer populated with either:
//   - the live value (e.g. "243mm", "Y91 P-3"),
//   - "..." while the polling task hasn't published its first sample,
//   - "busy" if the mutex is held elsewhere right now.
// Only called when the sensor reports state="on" (compiledIn && connected).

#if ENABLE_IMU_SENSOR
static void imuG2FormatValue(char* out, size_t cap) {
  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gImuCache.imuDataValid) {
      snprintf(out, cap, "Y%d P%d R%d",
               (int)gImuCache.oriYaw, (int)gImuCache.oriPitch, (int)gImuCache.oriRoll);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gImuCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_TOF_SENSOR
static void tofG2FormatValue(char* out, size_t cap) {
  if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
      snprintf(out, cap, "%dmm", gTofCache.tofObjects[0].distance_mm);
    } else if (gTofCache.tofDataValid) {
      snprintf(out, cap, "no obj");
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gTofCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_THERMAL_SENSOR
static void thermalG2FormatValue(char* out, size_t cap) {
  if (gThermalCache.mutex && xSemaphoreTake(gThermalCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gThermalCache.thermalDataValid) {
      snprintf(out, cap, "%d/%dC",
               (int)gThermalCache.thermalMinTemp, (int)gThermalCache.thermalMaxTemp);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gThermalCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_APDS_SENSOR
static void apdsG2FormatValue(char* out, size_t cap) {
  if (gAPDSCache.mutex && xSemaphoreTake(gAPDSCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gAPDSCache.apdsDataValid) {
      snprintf(out, cap, "px:%u", (unsigned)gAPDSCache.apdsProximity);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gAPDSCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_GAMEPAD_SENSOR
static void gamepadG2FormatValue(char* out, size_t cap) {
  if (gGamepadCache.mutex && xSemaphoreTake(gGamepadCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gGamepadCache.gamepadDataValid) {
      snprintf(out, cap, "btn:%lx", (unsigned long)gGamepadCache.gamepadButtons);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gGamepadCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_RTC_SENSOR
static void rtcG2FormatValue(char* out, size_t cap) {
  if (gRTCCache.mutex && xSemaphoreTake(gRTCCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gRTCCache.dataValid) {
      // Show clock time HH:MM — the temperature reading is a
      // secondary feature on this part and the date eats too much
      // of the value column.
      snprintf(out, cap, "%02u:%02u",
               (unsigned)gRTCCache.dateTime.hour,
               (unsigned)gRTCCache.dateTime.minute);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gRTCCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_GPS_SENSOR
static void gpsG2FormatValue(char* out, size_t cap) {
  if (gGPSCache.mutex && xSemaphoreTake(gGPSCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gGPSCache.dataValid) {
      if (gGPSCache.hasFix) {
        snprintf(out, cap, "fix %u", (unsigned)gGPSCache.satellites);
      } else {
        snprintf(out, cap, "no fix %u", (unsigned)gGPSCache.satellites);
      }
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gGPSCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_PRESENCE_SENSOR
static void presenceG2FormatValue(char* out, size_t cap) {
  if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gPresenceCache.dataValid) {
      const char* tag = gPresenceCache.presenceDetected ? "yes" :
                        gPresenceCache.motionDetected   ? "mot" : "no";
      snprintf(out, cap, "%s", tag);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gPresenceCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_FM_RADIO
static void fmG2FormatValue(char* out, size_t cap) {
  if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (gFmRadioCache.dataValid) {
      // Frequency stored in 10 kHz units (10390 = 103.9 MHz).
      snprintf(out, cap, "%u.%uMHz",
               (unsigned)(gFmRadioCache.frequency / 100u),
               (unsigned)(gFmRadioCache.frequency % 100u) / 10u);
    } else {
      snprintf(out, cap, "...");
    }
    xSemaphoreGive(gFmRadioCache.mutex);
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

// -----------------------------------------------------------------------------
// Sensor registry table
// -----------------------------------------------------------------------------
// Each row carries: short display label, hardware part name, and a getter
// pair returning the current enabled/connected state. When the sensor is
// compiled OUT, both getters return false and the row renders as "stub".
//
// `enabled` (gXEnabled) means "the user toggled this on at runtime via
// settings/UI" — set by the sensor module's own init code after a
// successful probe. `connected` means the I2C probe found it, regardless
// of user toggle. Display priority: connected > !connected & enabled >
// stub.

// Live-value formatter signature. Called with a small buffer (<= 12 chars
// recommended) to write a one-shot snapshot of the sensor's primary
// reading. Always called *only* when the row reports compiledIn &&
// connected — the formatter doesn't need to defend against the
// gXCache symbols not existing (it's compile-gated by the same flag).
typedef void (*G2SensorFormatter)(char* out, size_t cap);

struct G2SensorRow {
  const char* label;            // 5-char short identifier (e.g. "IMU", "THERM")
  const char* hardware;         // canonical part name (e.g. "BNO055")
  const char* featureId;        // feature-registry ID (lowercase, e.g. "imu",
                                // "thermal") — used to look up the auto-start
                                // setting pointer for the toggle row.
  bool        compiledIn;       // true if ENABLE_<X> at build time
  bool        enabled;          // gXEnabled at this moment (false if not compiled)
  bool        connected;        // gXConnected at this moment (false if not compiled)
  G2SensorFormatter format;     // live-value formatter, or nullptr if no live data
};

// Helper to read a state pair when the flag is on, otherwise return false
// without touching the (non-existent) symbol. Branchless at runtime.
#define SENSOR_PAIR(en, conn) (en), (conn)

// `includeStubs` controls whether non-compiled sensors get listed too.
// The legacy text-dump path keeps them (the user wanted "build: off"
// rows visible). The new interactive list path filters them out
// entirely so the user can't tap into a stub.
static void buildRows(G2SensorRow* rows, size_t maxRows, size_t* outCount,
                      bool includeStubs = true) {
  size_t i = 0;
  auto add = [&](const char* lbl, const char* hw, const char* fid,
                 bool compiled, bool en, bool conn, G2SensorFormatter fmt) {
    if (i >= maxRows) return;
    if (!compiled && !includeStubs) return;
    rows[i++] = { lbl, hw, fid, compiled, en, conn, fmt };
  };

#if ENABLE_IMU_SENSOR
  add("IMU",   "BNO055",   "imu",      true,  gImuEnabled,      gImuConnected,     imuG2FormatValue);
#else
  add("IMU",   "BNO055",   "imu",      false, false, false,                         nullptr);
#endif

#if ENABLE_TOF_SENSOR
  add("TOF",   "VL53L4CX", "tof",      true,  gTofEnabled,      gTofConnected,     tofG2FormatValue);
#else
  add("TOF",   "VL53L4CX", "tof",      false, false, false,                         nullptr);
#endif

#if ENABLE_THERMAL_SENSOR
  add("THERM", "MLX90640", "thermal",  true,  gThermalEnabled,  gThermalConnected, thermalG2FormatValue);
#else
  add("THERM", "MLX90640", "thermal",  false, false, false,                         nullptr);
#endif

#if ENABLE_APDS_SENSOR
  add("APDS",  "APDS9960", "apds",     true,  gApdsEnabled,     gApdsConnected,    apdsG2FormatValue);
#else
  add("APDS",  "APDS9960", "apds",     false, false, false,                         nullptr);
#endif

#if ENABLE_GAMEPAD_SENSOR
  add("GAMEP", "Seesaw",   "gamepad",  true,  gGamepadEnabled,  gGamepadConnected, gamepadG2FormatValue);
#else
  add("GAMEP", "Seesaw",   "gamepad",  false, false, false,                         nullptr);
#endif

#if ENABLE_RTC_SENSOR
  add("RTC",   "DS3231",   "rtc",      true,  gRtcEnabled,      gRtcConnected,     rtcG2FormatValue);
#else
  add("RTC",   "DS3231",   "rtc",      false, false, false,                         nullptr);
#endif

#if ENABLE_GPS_SENSOR
  add("GPS",   "PA1010D",  "gps",      true,  gGpsEnabled,      gGpsConnected,     gpsG2FormatValue);
#else
  add("GPS",   "PA1010D",  "gps",      false, false, false,                         nullptr);
#endif

#if ENABLE_PRESENCE_SENSOR
  add("PRES",  "STHS34",   "presence", true,  gPresenceEnabled, gPresenceConnected, presenceG2FormatValue);
#else
  add("PRES",  "STHS34",   "presence", false, false, false,                         nullptr);
#endif

#if ENABLE_FM_RADIO
  add("FM",    "RDA5807",  "fmradio",  true,  gFmRadioEnabled,  gFmRadioConnected, fmG2FormatValue);
#else
  add("FM",    "RDA5807",  "fmradio",  false, false, false,                         nullptr);
#endif

#if ENABLE_CAMERA_SENSOR
  add("CAM",   "DVP",      "camera",     true,  gCameraEnabled,   gCameraEnabled,    nullptr);
#else
  add("CAM",   "DVP",      "camera",     false, false, false,                         nullptr);
#endif

#if ENABLE_MICROPHONE_SENSOR
  add("MIC",   "PDM",      "microphone", true,  gMicEnabled,      gMicEnabled,       nullptr);
#else
  add("MIC",   "PDM",      "microphone", false, false, false,                         nullptr);
#endif

#if ENABLE_OLED_DISPLAY
  add("OLED",  "SSD1306",  "oled",     true,  gOledEnabled,     gOledEnabled,      nullptr);
#else
  add("OLED",  "SSD1306",  "oled",     false, false, false,                         nullptr);
#endif

  if (outCount) *outCount = i;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void g2BuildSensorList(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  G2SensorRow rows[16];
  size_t count = 0;
  buildRows(rows, sizeof(rows) / sizeof(rows[0]), &count);

  // Count how many are "live" (probed successfully) for the header summary.
  size_t live = 0;
  size_t compiled = 0;
  for (size_t i = 0; i < count; i++) {
    if (rows[i].compiledIn) compiled++;
    if (rows[i].connected)  live++;
  }

  String s;
  s.reserve(256);
  {
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "Sensors %u/%u of %u\n",
             (unsigned)live, (unsigned)compiled, (unsigned)count);
    s += hdr;
  }

  // One line per sensor, columns: LABEL  VALUE_OR_STATE
  // When the sensor is "on" and has a registered formatter, the second
  // column holds the live reading (e.g. "243mm", "Y91 P-3 R5", "fix 9").
  // Otherwise it falls back to the at-a-glance state word that matches
  // what the OLED sensor mode shows so the user can switch between
  // displays without mental retranslation.
  // Hardware part name dropped from on-lens rendering — keeping it
  // pushed the encoded pb body over the 253-byte single-fragment
  // ceiling (firmware doesn't reassemble multi-fragment CREATE; see
  // docs/G2_PROTOCOL.md). Hardware mapping stays in the CLI
  // (`g2sensors`) and web UI for users who need it.
  for (size_t i = 0; i < count; i++) {
    const G2SensorRow& r = rows[i];
    char value[14];
    if (r.compiledIn && r.connected && r.format) {
      r.format(value, sizeof(value));
    } else {
      // Status words chosen for clarity within the 12-char column.
      // "build: off" pairs symmetrically with the runtime "off" so a
      // user reading the column sees at a glance which is fixable from
      // the lens (toggle on) vs which requires a re-flash (rebuild
      // with ENABLE_<X>=1):
      //   build: off → ENABLE_<X> off at compile time (no driver in image)
      //   missing    → driver compiled in, user toggled on, probe failed
      //   off        → driver compiled in, user toggled off
      //   connected  → fallback if compiledIn && connected but no formatter
      const char* state =
          !r.compiledIn ? "build: off" :
          r.connected   ? "connected"  :
          r.enabled     ? "missing"    :
                          "off";
      snprintf(value, sizeof(value), "%s", state);
    }
    char line[24];
    // 6-char label column for alignment. G2 ASCII font is roughly
    // monospace; tabs render unpredictably so spaces it is.
    snprintf(line, sizeof(line), "%-6s %s\n", r.label, value);
    s += line;
  }

  // Truncate cleanly into caller's buffer. snprintf-style guarantee.
  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

bool g2ShowSensorList() {
  char buf[512];
  g2BuildSensorList(buf, sizeof(buf));
  DEBUG_G2F("[G2] Sensor list (%u B):\n%s", (unsigned)strlen(buf), buf);
  return g2ShowText(buf);
}

// -----------------------------------------------------------------------------
// Interactive Sensors page — landing list + per-sensor detail
// -----------------------------------------------------------------------------
// Two-level navigation:
//   LIST   — one row per compiled sensor, tap drills in.
//   DETAIL — back / Auto Start: ON|OFF / live value (single sample). Tap
//            "Auto Start" toggles the auto-start setting via the feature
//            registry, persists, and re-renders.
//
// Non-compiled sensors are filtered out by buildRows(includeStubs=false)
// so the operator can't tap into a row that wraps a no-op.

enum SensorsLevel : uint8_t {
  SENSORS_LEVEL_LIST   = 0,
  SENSORS_LEVEL_DETAIL = 1,
};
static SensorsLevel gSensorsLevel    = SENSORS_LEVEL_LIST;
static size_t       gSensorsDetailIdx = 0;   // index into the LIST-level rows

// Row buffers for both levels — large enough for any of the layouts.
// 16 sensors × 32 chars + headroom is ample; sized by the existing
// buildRows() max output.
#define SENSORS_MAX_ROWS  16
#define SENSORS_ROW_LEN   40
static char        gSensorsRows[SENSORS_MAX_ROWS][SENSORS_ROW_LEN];
static const char* gSensorsRowPtrs[SENSORS_MAX_ROWS];

// Format a list-level row: "IMU   on" / "TOF   missing" / etc. The
// state column matches the legacy text-dump's vocabulary so a user
// switching between displays sees the same words.
static void formatListRow(char* dst, size_t cap, const G2SensorRow& r) {
  char value[14];
  if (r.connected && r.format) {
    r.format(value, sizeof(value));
  } else {
    const char* state =
        r.connected ? "connected" :
        r.enabled   ? "missing"   :
                      "off";
    snprintf(value, sizeof(value), "%s", state);
  }
  snprintf(dst, cap, "%-6s %s", r.label, value);
}

void g2ShowSensorsMenu() {
  // Filter mode: only compiled-in sensors get a row. The wizard / web
  // UI / legacy text dump can still surface "build: off" entries; the
  // interactive lens page only shows what the user can actually open.
  G2SensorRow rows[SENSORS_MAX_ROWS];
  size_t count = 0;
  buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);

  // Stash the filtered set so handleTap can look up by row index later
  // — buildRows is cheap, but using the stashed copy guarantees the
  // dispatcher and the renderer agree on indexing.
  static G2SensorRow gSensorsListCache[SENSORS_MAX_ROWS];
  static size_t      gSensorsListCount = 0;
  for (size_t i = 0; i < count; i++) gSensorsListCache[i] = rows[i];
  gSensorsListCount = count;

  size_t out = 0;
  strncpy(gSensorsRows[out], "<- Main Menu", SENSORS_ROW_LEN - 1);
  gSensorsRows[out][SENSORS_ROW_LEN - 1] = '\0';
  gSensorsRowPtrs[out] = gSensorsRows[out];
  out++;

  if (count == 0) {
    snprintf(gSensorsRows[out], SENSORS_ROW_LEN,
             "(no sensors compiled in)");
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;
  } else {
    for (size_t i = 0; i < count && out < SENSORS_MAX_ROWS; i++) {
      formatListRow(gSensorsRows[out], SENSORS_ROW_LEN, rows[i]);
      gSensorsRowPtrs[out] = gSensorsRows[out];
      out++;
    }
  }

  gSensorsLevel = SENSORS_LEVEL_LIST;
  g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
  g2ShowListPage(gSensorsRowPtrs, out);
  // Expose the cached count via a file-static accessor so the tap
  // handler doesn't have to re-run buildRows on each tap. Just store
  // it via a local helper that closes over the file-static cache.
  // (Already done above — gSensorsListCount + gSensorsListCache.)
  (void)gSensorsListCache;  // silence unused-static warning when
                            //   SENSORS_LEVEL_DETAIL is the only reader
  DEBUG_G2F("[G2] Sensors menu shown (%u compiled rows)", (unsigned)count);
}

// Detail page — kicks in when the user taps a sensor row from the
// landing list. Layout (toggle at top so the actionable row is the
// first thing under the back button):
//   0: <- Sensors
//   1: Auto Start: ON|OFF (toggle)
//   2: <hardware name> (e.g. BNO055 — info-only)
//   3: <label>: <live value> | <state word>
static void showSensorDetail(const G2SensorRow& r) {
  size_t out = 0;
  strncpy(gSensorsRows[out], "<- Sensors", SENSORS_ROW_LEN - 1);
  gSensorsRows[out][SENSORS_ROW_LEN - 1] = '\0';
  gSensorsRowPtrs[out] = gSensorsRows[out];
  out++;

  // Toggle row — look up the feature's persisted bool. Some sensors
  // don't have an auto-start setting wired in the registry (legacy /
  // always-on); render "n/a" for those so the row still shows up
  // consistently but doesn't pretend to be tappable.
  const FeatureEntry* feat = getFeatureById(r.featureId);
  bool* autoStartPtr = (feat && feat->enabledSetting) ? feat->enabledSetting
                                                      : nullptr;

#if ENABLE_CAMERA_SENSOR
  const bool isCamera = (strcmp(r.featureId, "camera") == 0);
#else
  const bool isCamera = false;
#endif

  // For sensors with a runtime start/stop wired (camera, microphone),
  // the toggle row reflects live state directly: "<LABEL>: ON|OFF".
  // For everything else the user is really setting a boot preference,
  // so keep the explicit "Auto Start: ..." wording so they don't
  // expect the toggle to take effect immediately on those.
  bool labelAsLiveState = isCamera;
#if ENABLE_MICROPHONE_SENSOR
  if (strcmp(r.featureId, "microphone") == 0) labelAsLiveState = true;
#endif

  if (autoStartPtr) {
    if (labelAsLiveState) {
      // Show live state (r.enabled = gCameraEnabled / gMicEnabled),
      // not the auto-start preference. The toggle action flips both
      // the live state AND the boot preference, so the visible row
      // also reflects starts/stops triggered from the web UI, the
      // CLI, ESP-NOW peers, or anything else that calls
      // initCamera() / stopCamera() directly.
      snprintf(gSensorsRows[out], SENSORS_ROW_LEN,
               "%s: %s", r.label, r.enabled ? "ON" : "OFF");
    } else {
      snprintf(gSensorsRows[out], SENSORS_ROW_LEN,
               "Auto Start: %s", *autoStartPtr ? "ON" : "OFF");
    }
  } else {
    snprintf(gSensorsRows[out], SENSORS_ROW_LEN, "Auto Start: n/a");
  }
  gSensorsRowPtrs[out] = gSensorsRows[out];
  out++;

  if (isCamera) {
    // CAM-specific layout. Capture pushes a single frame; Stream
    // loops capture+push until the user double-taps to stop. The
    // hardware row stays informational.
    //
    //   1: CAM: ON|OFF      (toggle)
    //   2: Capture          (one-shot frame)
    //   3: Stream           (continuous, ~0.4 fps)
    //   4: HW: <chip>
    //   5: Settings >
    strncpy(gSensorsRows[out], "Capture", SENSORS_ROW_LEN - 1);
    gSensorsRows[out][SENSORS_ROW_LEN - 1] = '\0';
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;

    strncpy(gSensorsRows[out], "Stream", SENSORS_ROW_LEN - 1);
    gSensorsRows[out][SENSORS_ROW_LEN - 1] = '\0';
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;

    snprintf(gSensorsRows[out], SENSORS_ROW_LEN, "HW: %s", r.hardware);
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;

    strncpy(gSensorsRows[out], "Settings >", SENSORS_ROW_LEN - 1);
    gSensorsRows[out][SENSORS_ROW_LEN - 1] = '\0';
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;
  } else {
    // Generic layout for non-camera sensors: HW row then the live
    // value row. Tap on the value row is info-only for now.
    //
    //   1: Auto Start: ON|OFF (or "<LABEL>: ON|OFF" for live-toggle features)
    //   2: HW: <chip>
    //   3: <LABEL>: <live value>
    snprintf(gSensorsRows[out], SENSORS_ROW_LEN, "HW: %s", r.hardware);
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;

    char value[24];
    if (r.connected && r.format) {
      r.format(value, sizeof(value));
    } else {
      snprintf(value, sizeof(value), "%s",
               r.connected ? "connected" :
               r.enabled   ? "missing"   :
                             "off");
    }
    snprintf(gSensorsRows[out], SENSORS_ROW_LEN, "%s: %s", r.label, value);
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;
  }

  gSensorsLevel = SENSORS_LEVEL_DETAIL;
  g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
  g2ShowListPage(gSensorsRowPtrs, out);
  DEBUG_G2F("[G2] Sensors detail shown for '%s' (autoStart=%s, layout=%s)",
            r.label,
            autoStartPtr ? (*autoStartPtr ? "on" : "off") : "n/a",
            isCamera ? "camera" : "generic");
}

void g2ReshowSensorsDetail() {
  // Rebuild the filtered roster so stale stub-flips don't strand the
  // cached index, then re-enter showSensorDetail with the same row.
  G2SensorRow rows[SENSORS_MAX_ROWS];
  size_t count = 0;
  buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
  if (gSensorsDetailIdx >= count) {
    // Cached index no longer valid — fall back to the landing list.
    g2ShowSensorsMenu();
    return;
  }
  showSensorDetail(rows[gSensorsDetailIdx]);
}

void g2SensorsHandleTap(uint32_t idx) {
  if (gSensorsLevel == SENSORS_LEVEL_LIST) {
    if (idx == 0) {
      // <- Main Menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      extern void g2RedrawHijackMainMenu();
      g2RedrawHijackMainMenu();
      return;
    }
    // Re-derive the filtered list so the indices stay stable even if
    // some compile-time flag flipped between renders. Cheap and avoids
    // a stash-vs-stale concern.
    G2SensorRow rows[SENSORS_MAX_ROWS];
    size_t count = 0;
    buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
    const size_t pos = idx - 1;
    if (pos >= count) {
      DEBUG_G2F("[G2] Sensors: tap idx=%u out of range (count=%u)",
                (unsigned)idx, (unsigned)count);
      return;
    }
    gSensorsDetailIdx = pos;
    showSensorDetail(rows[pos]);
    return;
  }

  // SENSORS_LEVEL_DETAIL
  if (idx == 0) {
    g2ShowSensorsMenu();
    return;
  }
  if (idx == 1) {
    // Auto Start toggle — look up the cached row and flip the setting
    // through setSetting so it persists to NVS and triggers any
    // registered apply hooks. For features that have a runtime
    // start/stop path (camera, microphone), also call init/stop now so
    // the user gets immediate feedback instead of waiting for a reboot.
    G2SensorRow rows[SENSORS_MAX_ROWS];
    size_t count = 0;
    buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
    if (gSensorsDetailIdx >= count) {
      DEBUG_G2F("[G2] Sensors detail toggle: stale idx %u (count=%u)",
                (unsigned)gSensorsDetailIdx, (unsigned)count);
      return;
    }
    const G2SensorRow& r = rows[gSensorsDetailIdx];
    const FeatureEntry* feat = getFeatureById(r.featureId);
    if (!feat || !feat->enabledSetting) {
      DEBUG_G2F("[G2] Sensors detail toggle: '%s' has no auto-start "
                "setting wired", r.label);
      return;
    }
    const bool prev = *feat->enabledSetting;
    const bool next = !prev;
    setSetting(*feat->enabledSetting, next);
    BROADCAST_PRINTF("[G2] Sensors: %s Auto Start %s -> %s (from glasses)",
                     r.label, prev ? "ON" : "OFF",
                     next ? "ON" : "OFF");

    // Runtime apply for hardware that supports hot start/stop. The
    // auto-start setting persists the user's preference; the calls
    // below make the change visible right now.
#if ENABLE_CAMERA_SENSOR
    if (strcmp(r.featureId, "camera") == 0) {
      if (next && !gCameraEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: starting camera...");
        if (!initCamera()) {
          BROADCAST_PRINTF("[G2] Sensors: camera init FAILED");
        }
      } else if (!next && gCameraEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: stopping camera...");
        stopCamera();
      }
    }
#endif
#if ENABLE_MICROPHONE_SENSOR
    if (strcmp(r.featureId, "microphone") == 0) {
      if (next && !gMicEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: starting microphone...");
        if (!initMicrophone()) {
          BROADCAST_PRINTF("[G2] Sensors: microphone init FAILED");
        }
      } else if (!next && gMicEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: stopping microphone...");
        stopMicrophone();
      }
    }
#endif

    // Re-derive rows after the start/stop call so the freshly-updated
    // gXEnabled flag is reflected in the connected/value columns.
    buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
    if (gSensorsDetailIdx < count) {
      showSensorDetail(rows[gSensorsDetailIdx]);
    } else {
      showSensorDetail(r);
    }
    return;
  }
  // idx 2..4 — layout differs by sensor (camera has View / HW / Settings,
  // others have HW / live-value). Resolve which row was actually tapped
  // by re-reading the cached sensor row.
  G2SensorRow rows[SENSORS_MAX_ROWS];
  size_t count = 0;
  buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
  if (gSensorsDetailIdx >= count) {
    DEBUG_G2F("[G2] Sensors detail tap: stale idx %u (count=%u)",
              (unsigned)gSensorsDetailIdx, (unsigned)count);
    return;
  }
  const G2SensorRow& r = rows[gSensorsDetailIdx];

#if ENABLE_CAMERA_SENSOR
  if (strcmp(r.featureId, "camera") == 0) {
    // CAM detail layout:
    //   idx 2 = Capture      (one-shot frame — push, hold, double-tap to dismiss)
    //   idx 3 = Stream       (continuous frames until double-tap)
    //   idx 4 = HW: <chip>   (info-only)
    //   idx 5 = Settings >   (open camera settings sub-page)
    if (idx == 2) {
      if (r.connected) {
        DEBUG_G2F("[G2] Sensors detail: CAM Capture tapped — opening viewer");
        g2ShowCameraViewer([]() { g2ShowSensorsMenu(); });
      } else {
        DEBUG_G2F("[G2] Sensors detail: CAM Capture tapped but camera not "
                  "connected — turn it ON first");
      }
      return;
    }
    if (idx == 3) {
      if (r.connected) {
        DEBUG_G2F("[G2] Sensors detail: CAM Stream tapped — opening stream");
        g2ShowCameraStream([]() { g2ShowSensorsMenu(); });
      } else {
        DEBUG_G2F("[G2] Sensors detail: CAM Stream tapped but camera not "
                  "connected — turn it ON first");
      }
      return;
    }
    if (idx == 4) {
      DEBUG_G2F("[G2] Sensors detail: CAM hardware row tapped (info-only)");
      return;
    }
    if (idx == 5) {
      DEBUG_G2F("[G2] Sensors detail: CAM Settings tapped — opening sub-page");
      g2ShowCameraSettingsMenu();
      return;
    }
    return;
  }
#endif

  // Generic non-camera layout:
  //   idx 2 = HW: <chip>            (info-only)
  //   idx 3 = <LABEL>: <live value> (info-only for now)
  if (idx == 2) {
    DEBUG_G2F("[G2] Sensors detail: hardware row tapped (info-only)");
    return;
  }
  DEBUG_G2F("[G2] Sensors detail: value row %u tapped (info-only)",
            (unsigned)idx);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
