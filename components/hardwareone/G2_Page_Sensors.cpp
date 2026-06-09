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
#include "System_Mutex.h"               // SensorCacheGuard (was pulled in transitively via
                                        // i2csensor_* headers; include directly so it survives
                                        // disabling individual sensors)
#include "G2_HijackCmd.h"               // g2SubmitHijackCommand — Group A migration
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
#include "i2csensor_bno055.h"     // gImuCache + gImuEnabled / gImuConnected
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"   // gTofCache + gTofEnabled / gTofConnected
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"   // gThermalCache + gThermalEnabled / gThermalConnected
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"     // gInputCache + gInputEnabled / gInputConnected
#endif
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h" // gAnoEncoderCache + ANO_BTN_* + axis consts
// The ANO driver defines these gamepad-shaped proxies (kept in sync with
// gAnoEncoderEnabled/Connected) but, unlike the seesaw header, doesn't export
// them — declare them here for the input row's enabled/connected state.
extern bool gInputEnabled;
extern bool gInputConnected;
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"   // gApdsCache + gApdsEnabled / gApdsConnected
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"     // gRtcCache + gRtcEnabled / gRtcConnected
#endif
#if ENABLE_FM_RADIO
#include "i2csensor_rda5807.h"    // gFmRadioCache + gFmRadioEnabled / gFmRadioConnected
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"    // gGpsCache + gGpsEnabled / gGpsConnected
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h" // gPresenceCache + gPresenceEnabled / gPresenceConnected
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
  SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(5), "g2.imuFormat");
  if (g.held) {
    if (gImuCache.imuDataValid) {
      snprintf(out, cap, "Y%d P%d R%d",
               (int)gImuCache.oriYaw, (int)gImuCache.oriPitch, (int)gImuCache.oriRoll);
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_TOF_SENSOR
static void tofG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(5), "g2.tofFormat");
  if (g.held) {
    if (gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
      snprintf(out, cap, "%dmm", gTofCache.tofObjects[0].distance_mm);
    } else if (gTofCache.tofDataValid) {
      snprintf(out, cap, "no obj");
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_THERMAL_SENSOR
static void thermalG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gThermalCache.mutex, pdMS_TO_TICKS(5), "g2.thermalFormat");
  if (g.held) {
    if (gThermalCache.thermalDataValid) {
      snprintf(out, cap, "%d/%dC",
               (int)gThermalCache.thermalMinTemp, (int)gThermalCache.thermalMaxTemp);
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_APDS_SENSOR
static void apdsG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(5), "g2.apdsFormat");
  if (g.held) {
    if (gApdsCache.apdsDataValid) {
      snprintf(out, cap, "px:%u", (unsigned)gApdsCache.apdsProximity);
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_GAMEPAD_SENSOR
static void gamepadG2FormatValue(char* out, size_t cap) {
  // Landing-list overview value — kept generic ("ready"). Raw button hex was
  // unreadable here; the specific Joy X / Joy Y / Btns readout lives in the
  // sensor's live page (g2BuildSensorReadout).
  SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(5), "g2.gamepadFormat");
  snprintf(out, cap, (g.held && gInputCache.dataValid) ? "ready" : "...");
}
#endif

#if ENABLE_ANO_ENCODER
static void anoG2FormatValue(char* out, size_t cap) {
  // Landing-list overview value — kept generic ("ready"). The live wheel /
  // D-pad readout lives in the sensor's live page (g2BuildAnoArt).
  SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(5), "g2.anoFormat");
  snprintf(out, cap, (g.held && gAnoEncoderCache.dataValid) ? "ready" : "...");
}
#endif

#if ENABLE_RTC_SENSOR
static void rtcG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gRtcCache.mutex, pdMS_TO_TICKS(5), "g2.rtcFormat");
  if (g.held) {
    if (gRtcCache.dataValid) {
      // Show clock time HH:MM — the temperature reading is a
      // secondary feature on this part and the date eats too much
      // of the value column.
      snprintf(out, cap, "%02u:%02u",
               (unsigned)gRtcCache.dateTime.hour,
               (unsigned)gRtcCache.dateTime.minute);
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_GPS_SENSOR
static void gpsG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(5), "g2.gpsFormat");
  if (g.held) {
    if (gGpsCache.dataValid) {
      if (gGpsCache.hasFix) {
        snprintf(out, cap, "fix %u", (unsigned)gGpsCache.satellites);
      } else {
        snprintf(out, cap, "no fix %u", (unsigned)gGpsCache.satellites);
      }
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_PRESENCE_SENSOR
static void presenceG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(5), "g2.presenceFormat");
  if (g.held) {
    if (gPresenceCache.dataValid) {
      const char* tag = gPresenceCache.presenceDetected ? "yes" :
                        gPresenceCache.motionDetected   ? "mot" : "no";
      snprintf(out, cap, "%s", tag);
    } else {
      snprintf(out, cap, "...");
    }
  } else {
    snprintf(out, cap, "busy");
  }
}
#endif

#if ENABLE_FM_RADIO
static void fmG2FormatValue(char* out, size_t cap) {
  SensorCacheGuard g(gFmRadioCache.mutex, pdMS_TO_TICKS(5), "g2.fmRadioFormat");
  if (g.held) {
    if (gFmRadioCache.dataValid) {
      // Frequency stored in 10 kHz units (10390 = 103.9 MHz).
      snprintf(out, cap, "%u.%uMHz",
               (unsigned)(gFmRadioCache.frequency / 100u),
               (unsigned)(gFmRadioCache.frequency % 100u) / 10u);
    } else {
      snprintf(out, cap, "...");
    }
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
  add("GAMEP", "Seesaw",   "gamepad",  true,  gInputEnabled,  gInputConnected, gamepadG2FormatValue);
#elif ENABLE_ANO_ENCODER
  // Same row id ("gamepad") so the unified input commands (openinput /
  // inputautostart) and the registry remap to "input" keep working; only the
  // label, hardware name, formatter and live readout differ for the ANO.
  add("ANO",   "Encoder",  "gamepad",  true,  gInputEnabled,  gInputConnected, anoG2FormatValue);
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
  add("MIC",   "PDM",      "microphone", true,  gMicEnabled,      micConnected,      nullptr);
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
  SENSORS_LEVEL_DETAIL = 1,   // camera (static multi-action list)
  SENSORS_LEVEL_LIVE   = 2,   // generic non-camera sensor: live readout compound
};
static SensorsLevel gSensorsLevel    = SENSORS_LEVEL_LIST;
static size_t       gSensorsDetailIdx = 0;   // index into the LIST-level rows

#if ENABLE_CAMERA_SENSOR
// Set when CAM ON was queued to cam_pwr; cleared in g2SensorsOnCameraPowerDone.
// Avoids "not connected" on Capture/Stream while initCamera() is still running.
static bool sG2SensorsCamAsyncStartPending = false;
#endif

// Ignore repeat taps on the detail "Auto Start" row (idx 1) for this long.
// Lens list events can fire twice (L/R) or in quick bursts; camera/mic
// start/stop is expensive and should not thrash.
static constexpr uint32_t kSensorsDetailToggleDebounceMs = 600;
static uint32_t           sSensorsDetailToggleLastMs     = 0;

// Row buffers for both levels — large enough for any of the layouts.
// 16 sensors × 32 chars + headroom is ample; sized by the existing
// buildRows() max output.
#define SENSORS_MAX_ROWS  16
#define SENSORS_ROW_LEN   40
EXT_RAM_BSS_ATTR static char        gSensorsRows[SENSORS_MAX_ROWS][SENSORS_ROW_LEN];
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
#if ENABLE_CAMERA_SENSOR
  const bool isCameraLive = (strcmp(r.featureId, "camera") == 0);
#else
  const bool isCameraLive = false;
#endif
  // Non-camera sensors render as a LIVE compound (selectable list + a per-tick
  // live readout) instead of the static list built below. Camera keeps its
  // multi-action static detail (Capture/Stream/Settings). The DETAIL-level tap
  // routing (idx 0 = back, idx 1 = Auto-Start toggle) is reused as-is, so we
  // set the level + hijack page here before spawning the live worker. On spawn
  // failure we fall through to the static render so the page never goes blank.
  if (!isCameraLive) {
    gSensorsLevel = SENSORS_LEVEL_LIVE;
    g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
    if (g2ShowSensorLive()) return;
    DEBUG_G2F("[G2] sensor-live start failed for '%s' — static detail fallback",
              r.label);
    gSensorsLevel = SENSORS_LEVEL_DETAIL;  // fall through to static render
  }
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
  bool isMic            = false;
#if ENABLE_MICROPHONE_SENSOR
  isMic = (strcmp(r.featureId, "microphone") == 0);
  if (isMic) labelAsLiveState = true;
#endif

  if (autoStartPtr) {
    if (isMic) {
      // MIC toggle row — live I2S state only. PDM has no probeable
      // "connected" signal (no chip ID, no I2C address — just a
      // clock+data slave), so we don't try to express hardware
      // presence here. The legacy "Connected | Disconnected" half
      // of this row was driven by the warmup-data heuristic, which
      // was misleading whenever the mic was off (warmup had never
      // run → row claimed "Disconnected" even when the mic chip
      // was physically present and would work fine on toggle).
      // Plain "MIC: On" / "MIC: Off" matches what tap actually
      // controls and stops over-promising what we know.
      snprintf(gSensorsRows[out], SENSORS_ROW_LEN,
               "MIC: %s",
               r.enabled ? "On" : "Off");
    } else if (labelAsLiveState) {
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
    // value row. Tap on the value row is info-only for now. MIC
    // skips the value row — its info is already folded into the
    // merged toggle row above (see the isMic branch).
    //
    //   1: Auto Start: ON|OFF (or "<LABEL>: ON|OFF" for live-toggle features)
    //   2: HW: <chip>
    //   3: <LABEL>: <live value>           — skipped for MIC
    snprintf(gSensorsRows[out], SENSORS_ROW_LEN, "HW: %s", r.hardware);
    gSensorsRowPtrs[out] = gSensorsRows[out];
    out++;

    if (!isMic) {
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
  }

  gSensorsLevel = SENSORS_LEVEL_DETAIL;
  g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
  g2ShowListPage(gSensorsRowPtrs, out);
  DEBUG_G2F("[G2] Sensors detail shown for '%s' (autoStart=%s, layout=%s)",
            r.label,
            autoStartPtr ? (*autoStartPtr ? "on" : "off") : "n/a",
            isCamera ? "camera" : "generic");
}

// Map a G2 sensor-row featureId to its feature-registry entry. The input
// device's row id is "gamepad", but the registry entry is the unified "input"
// (gamepad/ANO) entry that owns gSettings.inputAutoStart — so remap it.
// Everything else matches the registry id 1:1.
static const FeatureEntry* sensorFeature(const char* featureId) {
  if (featureId && strcmp(featureId, "gamepad") == 0) return getFeatureById("input");
  return getFeatureById(featureId);
}

// Build a sensor's runtime start/stop CLI command. Most i2c sensors use
// open<id>/close<id>; the input device is the exception (openinput/closeinput),
// and FM radio uses open/closefm.
static void sensorRuntimeCmd(const char* featureId, bool open, char* out, size_t cap) {
  const char* verb = open ? "open" : "close";
  if (strcmp(featureId, "gamepad") == 0)       snprintf(out, cap, "%sinput", verb);
  else if (strcmp(featureId, "fmradio") == 0)  snprintf(out, cap, "%sfm", verb);
  else                                         snprintf(out, cap, "%s%s", verb, featureId);
}

// Build a sensor's PERSISTED auto-start command. The command + value format
// vary by device: the input device uses inputautostart, camera/mic their own
// autostart commands, OLED uses "oledenabled <1|0>" (it has no autostart cmd —
// gSettings.oledEnabled IS the boot state), and the i2c data sensors use
// "sensorautostart <id> <on|off>". Using the generic form for OLED is what
// produced "Unknown sensor".
static void sensorAutostartCmd(const char* featureId, bool on, char* out, size_t cap) {
  const char* v = on ? "on" : "off";
  if (strcmp(featureId, "gamepad") == 0)        snprintf(out, cap, "inputautostart %s", v);
  else if (strcmp(featureId, "camera") == 0)    snprintf(out, cap, "cameraautostart %s", v);
  else if (strcmp(featureId, "microphone") == 0)snprintf(out, cap, "micautostart %s", v);
  else if (strcmp(featureId, "oled") == 0)      snprintf(out, cap, "oledenabled %d", on ? 1 : 0);
  else                                          snprintf(out, cap, "sensorautostart %s %s", featureId, v);
}

#if ENABLE_GAMEPAD_SENSOR
// Render one diamond button into a fixed 3-char field: "[X]" pressed,
// " X " released — equal width keeps the ASCII layout aligned.
static void gpCell(bool pressed, char letter, char* dst4) {
  dst4[0] = pressed ? '[' : ' ';
  dst4[1] = letter;
  dst4[2] = pressed ? ']' : ' ';
  dst4[3] = '\0';
}

// ASCII-art gamepad for the live readout — a 3x3 joystick grid (the 'o' moves
// with the stick), an X/Y/A/B diamond, and Sel/Start, echoing the web card.
// Buttons are active-low (bit clear == pressed). Joystick is raw 0..1023,
// centre ~512; the trailing "x.. y.." line is for tuning the deadzone /
// up-down-left-right mapping (tell me if a direction is flipped).
static void g2BuildGamepadArt(char* out, size_t cap) {
  SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(5), "g2.gamepadArt");
  if (!(g.held && gInputCache.dataValid)) {
    snprintf(out, cap, "Gamepad\n(reading...)");
    return;
  }
  const uint32_t b = gInputCache.buttons;
  char X[4], Y[4], A[4], B[4];
  gpCell((b & GAMEPAD_BUTTON_X) == 0, 'X', X);
  gpCell((b & GAMEPAD_BUTTON_Y) == 0, 'Y', Y);
  gpCell((b & GAMEPAD_BUTTON_A) == 0, 'A', A);
  gpCell((b & GAMEPAD_BUTTON_B) == 0, 'B', B);
  const char* sel = ((b & GAMEPAD_BUTTON_SELECT) == 0) ? "[Sel]" : " Sel ";
  const char* sta = ((b & GAMEPAD_BUTTON_START)  == 0) ? "[Start]" : " Start ";

  // Joystick zone grid. GRID×GRID zones via an even linear split of the raw
  // 0..1023 range — finer = more accurate. Columns are spaced (". . ." not
  // "...") so the dot pattern is ~square on the lens: the font's line-height
  // is ~1.7x the char width, so without horizontal spacing GRID columns render
  // much narrower than GRID rows are tall. Bump GRID to taste.
  constexpr int GRID = 5;
  constexpr int DOT_GAP = 3;                        // spaces between dots — squares the grid
  constexpr int GW = GRID + (GRID - 1) * DOT_GAP;   // grid-line width in chars
  auto zone = [](int v) {
    int z = (v * GRID) / 1024;
    return z < 0 ? 0 : (z >= GRID ? GRID - 1 : z);
  };
  const int col = zone(gInputCache.joyX);
  const int row = (GRID - 1) - zone(gInputCache.joyY);   // invert Y: up = top

  // Sel/Start get their own header line (off the diamond) so X/Y/A/B can be a
  // roomy, evenly-spaced diamond beside the centre rows of the zone grid:
  //    [Sel]   [Start]
  //   . . . . .
  //   . . . . .     X
  //   . . o . .   Y     A
  //   . . . . .     B
  //   . . . . .
  size_t off = 0;
  if (off < cap) off += snprintf(out + off, cap - off, " %s   %s\n", sel, sta);
  for (int r = 0; r < GRID; r++) {
    // Dots spaced DOT_GAP apart so the grid reads ~square (the lens font's
    // line-height spans several char-widths, so unspaced columns look skinny).
    char gline[GW + 1];
    for (int k = 0; k < GW; k++) gline[k] = ' ';
    for (int c = 0; c < GRID; c++) gline[c * (1 + DOT_GAP)] = (r == row && c == col) ? 'o' : '.';
    gline[GW] = '\0';

    // X/Y/A/B as a roomy diamond beside the grid's centre rows (X top, Y/A
    // spread, B bottom — spread wide so it doesn't collapse to a vertical line).
    char seg[24] = "";
    if      (r == 1) snprintf(seg, sizeof seg, "    %s", X);         //       X    (top)
    else if (r == 2) snprintf(seg, sizeof seg, "%s    %s", Y, A);    //   Y     A  (mid)
    else if (r == 3) snprintf(seg, sizeof seg, "    %s", B);         //       B    (bottom)

    if (off < cap) off += snprintf(out + off, cap - off, "%s  %s\n", gline, seg);
  }
  if (off < cap) snprintf(out + off, cap - off, "x%d y%d", gInputCache.joyX, gInputCache.joyY);
}
#endif  // ENABLE_GAMEPAD_SENSOR

#if ENABLE_ANO_ENCODER
// Render one button into a fixed 3-char field: "[U]" pressed, " U " released —
// equal width keeps the cross aligned (mirror of the gamepad's gpCell).
static void anoCell(bool pressed, char letter, char* dst4) {
  dst4[0] = pressed ? '[' : ' ';
  dst4[1] = letter;
  dst4[2] = pressed ? ']' : ' ';
  dst4[3] = '\0';
}

// ASCII-art ANO rotary encoder for the live readout — the rotary wheel (live
// absolute position, which axis it drives, and spin direction) over a 5-way
// D-pad cross (Up/Down/Left/Right + centre IN press) plus the virtual START
// chord, echoing the gamepad card's structure. Buttons are active-high in the
// ANO_BTN_* layout. The trailing "d.. b.." line is raw tuning info.
//   Wheel:42 +  axis:V
//        [U]
//    [L] [O] [R]
//        [D]
//       [Start]
//   d+1 b0x02
static void g2BuildAnoArt(char* out, size_t cap) {
  SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(5), "g2.anoArt");
  if (!(g.held && gAnoEncoderCache.dataValid)) {
    snprintf(out, cap, "Encoder\n(reading...)");
    return;
  }
  const long     pos  = (long)gAnoEncoderCache.encoderPosition;
  const uint32_t b    = gAnoEncoderCache.buttons;
  const uint8_t  axis = gAnoEncoderCache.currentAxis;

  // Spin direction since the last render. Unlike a joystick the wheel has no
  // absolute "rest" position — movement is the signal — so we diff against the
  // previous render. The static cache survives across the 1 Hz live ticks;
  // the first render after open shows no direction.
  static long sLastPos = 0;
  static bool sHavePos = false;
  char dir = ' ';
  if (sHavePos) dir = (pos > sLastPos) ? '+' : (pos < sLastPos) ? '-' : ' ';
  sLastPos = pos;
  sHavePos = true;

  char U[4], D[4], L[4], R[4], C[4];
  anoCell(b & ANO_BTN_UP,    'U', U);
  anoCell(b & ANO_BTN_DOWN,  'D', D);
  anoCell(b & ANO_BTN_LEFT,  'L', L);
  anoCell(b & ANO_BTN_RIGHT, 'R', R);
  anoCell(b & ANO_BTN_IN,    'O', C);   // centre / select press
  const char* start = (b & ANO_VIRT_START) ? "[Start]" : " Start ";

  snprintf(out, cap,
           "Wheel:%ld %c  axis:%c\n"
           "    %s\n"
           "%s %s %s\n"
           "    %s\n"
           "   %s\n"
           "d%+ld b0x%02lX",
           pos, dir, (axis == ANO_AXIS_HORIZONTAL) ? 'H' : 'V',
           U,
           L, C, R,
           D,
           start,
           (long)gAnoEncoderCache.encoderDelta, (unsigned long)(b & 0xFFu));
}
#endif  // ENABLE_ANO_ENCODER

#if ENABLE_RTC_SENSOR
// Dedicated multi-line RTC readout — weekday + full date, HH:MM:SS (the live
// page ticks at 1 Hz, so the seconds advance), and the chip temperature.
// Richer than rtcG2FormatValue's HH:MM landing-list value, which has to fit a
// narrow column. No "°" glyph — the lens font drops uncommon symbols.
//   Sun 2026-06-07
//      14:30:52
//   Temp: 24.5 C
static const char* kRtcDow[8] = { "?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static void g2BuildRtcReadout(char* out, size_t cap) {
  SensorCacheGuard g(gRtcCache.mutex, pdMS_TO_TICKS(5), "g2.rtcReadout");
  if (!(g.held && gRtcCache.dataValid)) {
    snprintf(out, cap, "RTC\n(reading...)");
    return;
  }
  const RTCDateTime& t = gRtcCache.dateTime;
  const uint8_t dow = (t.dayOfWeek >= 1 && t.dayOfWeek <= 7) ? t.dayOfWeek : 0;
  snprintf(out, cap,
           "%s %04u-%02u-%02u\n"
           "   %02u:%02u:%02u\n"
           "Temp: %.1f C",
           kRtcDow[dow], (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
           (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
           (double)gRtcCache.temperature);
}
#endif  // ENABLE_RTC_SENSOR

#if ENABLE_IMU_SENSOR
// Orientation (Euler yaw/pitch/roll), accel & gyro vectors, chip temp.
static void g2BuildImuReadout(char* out, size_t cap) {
  SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(5), "g2.imuReadout");
  if (!(g.held && gImuCache.imuDataValid)) { snprintf(out, cap, "IMU\n(reading...)"); return; }
  snprintf(out, cap,
           "Yaw  : %d\n"
           "Pitch: %d\n"
           "Roll : %d\n"
           "Acc %.1f %.1f %.1f\n"
           "Gyr %.1f %.1f %.1f\n"
           "Temp: %.0f C",
           (int)gImuCache.oriYaw, (int)gImuCache.oriPitch, (int)gImuCache.oriRoll,
           (double)gImuCache.accelX, (double)gImuCache.accelY, (double)gImuCache.accelZ,
           (double)gImuCache.gyroX, (double)gImuCache.gyroY, (double)gImuCache.gyroZ,
           (double)gImuCache.imuTemp);
}
#endif  // ENABLE_IMU_SENSOR

#if ENABLE_TOF_SENSOR
// Per-object distance (mm) + range status, first 3 detected objects.
static void g2BuildTofReadout(char* out, size_t cap) {
  SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(5), "g2.tofReadout");
  if (!(g.held && gTofCache.tofDataValid)) { snprintf(out, cap, "TOF\n(reading...)"); return; }
  if (gTofCache.tofTotalObjects <= 0) { snprintf(out, cap, "Objects: 0\n(nothing in range)"); return; }
  size_t off = 0;
  off += snprintf(out + off, cap - off, "Objects: %d", gTofCache.tofTotalObjects);
  int shown = gTofCache.tofTotalObjects > 3 ? 3 : gTofCache.tofTotalObjects;
  for (int i = 0; i < shown && off < cap; i++) {
    off += snprintf(out + off, cap - off, "\n#%d %dmm st%d",
                    i + 1, gTofCache.tofObjects[i].distance_mm, gTofCache.tofObjects[i].status);
  }
}
#endif  // ENABLE_TOF_SENSOR

#if ENABLE_APDS_SENSOR
// Proximity, RGBC colour channels, and last gesture code.
static void g2BuildApdsReadout(char* out, size_t cap) {
  SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(5), "g2.apdsReadout");
  if (!(g.held && gApdsCache.apdsDataValid)) { snprintf(out, cap, "APDS\n(reading...)"); return; }
  snprintf(out, cap,
           "Prox: %u\n"
           "R:%u G:%u B:%u\n"
           "Clear: %u\n"
           "Gesture: %u",
           (unsigned)gApdsCache.apdsProximity,
           (unsigned)gApdsCache.apdsRed, (unsigned)gApdsCache.apdsGreen,
           (unsigned)gApdsCache.apdsBlue, (unsigned)gApdsCache.apdsClear,
           (unsigned)gApdsCache.apdsGesture);
}
#endif  // ENABLE_APDS_SENSOR

#if ENABLE_GPS_SENSOR
// Fix status + satellites, and (when fixed) lat/lon/altitude/speed.
static void g2BuildGpsReadout(char* out, size_t cap) {
  SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(5), "g2.gpsReadout");
  if (!(g.held && gGpsCache.dataValid)) { snprintf(out, cap, "GPS\n(reading...)"); return; }
  if (!gGpsCache.hasFix) {
    snprintf(out, cap, "Fix: no\nSats: %u\n(acquiring...)", (unsigned)gGpsCache.satellites);
    return;
  }
  snprintf(out, cap,
           "Fix: yes (%u)\n"
           "Sats: %u\n"
           "%.4f\n"
           "%.4f\n"
           "Alt:%.0fm Sp:%.1f",
           (unsigned)gGpsCache.fixQuality, (unsigned)gGpsCache.satellites,
           (double)gGpsCache.latitude, (double)gGpsCache.longitude,
           (double)gGpsCache.altitude, (double)gGpsCache.speed);
}
#endif  // ENABLE_GPS_SENSOR

#if ENABLE_PRESENCE_SENSOR
// Presence / motion flags + raw values, plus ambient & object temps.
static void g2BuildPresenceReadout(char* out, size_t cap) {
  SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(5), "g2.presenceReadout");
  if (!(g.held && gPresenceCache.dataValid)) { snprintf(out, cap, "Presence\n(reading...)"); return; }
  snprintf(out, cap,
           "Presence: %s\n"
           "Motion: %s\n"
           "Pval:%d Mval:%d\n"
           "Amb: %.1f C\n"
           "Obj: %.1f C",
           gPresenceCache.presenceDetected ? "yes" : "no",
           gPresenceCache.motionDetected ? "yes" : "no",
           (int)gPresenceCache.presenceValue, (int)gPresenceCache.motionValue,
           (double)gPresenceCache.ambientTemp, (double)gPresenceCache.compObjectTemp);
}
#endif  // ENABLE_PRESENCE_SENSOR

#if ENABLE_FM_RADIO
// Tuned frequency, volume, stereo/mute, signal (RSSI/SNR), RDS station name.
static void g2BuildFmReadout(char* out, size_t cap) {
  SensorCacheGuard g(gFmRadioCache.mutex, pdMS_TO_TICKS(5), "g2.fmReadout");
  if (!(g.held && gFmRadioCache.dataValid)) { snprintf(out, cap, "FM\n(reading...)"); return; }
  const char* mode = gFmRadioCache.muted ? "Muted"
                   : gFmRadioCache.stereo ? "Stereo" : "Mono";
  snprintf(out, cap,
           "%u.%u MHz\n"
           "Vol:%u %s\n"
           "RSSI:%u SNR:%u\n"
           "Name:%.8s",
           (unsigned)(gFmRadioCache.frequency / 100u),
           (unsigned)((gFmRadioCache.frequency % 100u) / 10u),
           (unsigned)gFmRadioCache.volume, mode,
           (unsigned)gFmRadioCache.rssi, (unsigned)gFmRadioCache.snr,
           gFmRadioCache.stationName[0] ? gFmRadioCache.stationName : "--");
}
#endif  // ENABLE_FM_RADIO

// Multi-line LIVE readout for the sensor the user drilled into
// (gSensorsDetailIdx). Consumed by renderSensorDetailLive() in
// G2_Glasses.cpp and refreshed every live tick (UPDATE_TEXT on the readout
// child only — the list child / selection is untouched). This is the
// content layer; the compound/transport lives in G2_Glasses.cpp next to
// the MIC-detail equivalent (renderMicDetailLive).
//
// Gamepad gets a full multi-value readout; the other sensors currently
// fall back to "<label>: <single value>" via their existing one-line
// formatters until each gets a fleshed-out multi-line readout.
void g2BuildSensorReadout(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  G2SensorRow rows[SENSORS_MAX_ROWS];
  size_t count = 0;
  buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
  if (gSensorsDetailIdx >= count) { snprintf(out, cap, "(no sensor)"); return; }
  const G2SensorRow& r = rows[gSensorsDetailIdx];

  // Not running → there is no live data, and the per-sensor cache mutex may
  // not even be created yet (it's made when the task starts). Don't read it;
  // tell the user to start the sensor via the Run row.
  if (!r.enabled) {
    snprintf(out, cap, "%s\nstopped\ntap Run to start", r.label);
    return;
  }

#if ENABLE_GAMEPAD_SENSOR
  if (strcmp(r.featureId, "gamepad") == 0) {
    g2BuildGamepadArt(out, cap);
    return;
  }
#elif ENABLE_ANO_ENCODER
  if (strcmp(r.featureId, "gamepad") == 0) {   // same row id; ANO is the active input driver
    g2BuildAnoArt(out, cap);
    return;
  }
#endif

#if ENABLE_RTC_SENSOR
  if (strcmp(r.featureId, "rtc") == 0) {
    g2BuildRtcReadout(out, cap);
    return;
  }
#endif
#if ENABLE_IMU_SENSOR
  if (strcmp(r.featureId, "imu") == 0)      { g2BuildImuReadout(out, cap);      return; }
#endif
#if ENABLE_TOF_SENSOR
  if (strcmp(r.featureId, "tof") == 0)      { g2BuildTofReadout(out, cap);      return; }
#endif
#if ENABLE_APDS_SENSOR
  if (strcmp(r.featureId, "apds") == 0)     { g2BuildApdsReadout(out, cap);     return; }
#endif
#if ENABLE_GPS_SENSOR
  if (strcmp(r.featureId, "gps") == 0)      { g2BuildGpsReadout(out, cap);      return; }
#endif
#if ENABLE_PRESENCE_SENSOR
  if (strcmp(r.featureId, "presence") == 0) { g2BuildPresenceReadout(out, cap); return; }
#endif
#if ENABLE_FM_RADIO
  if (strcmp(r.featureId, "fmradio") == 0)  { g2BuildFmReadout(out, cap);       return; }
#endif

  // Generic fallback — single most-useful value via the existing formatter.
  char value[24];
  if (r.connected && r.format) {
    r.format(value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "%s",
             r.connected ? "connected" : r.enabled ? "missing" : "off");
  }
  snprintf(out, cap, "%s: %s", r.label, value);
}

// List rows for the LIVE sensor-detail compound. Index order MUST match the
// SENSORS_LEVEL_LIVE tap handler:
//   idx 0 = <- Sensors (back)
//   idx 1 = Run: ON/OFF        (runtime start/stop — reflects live state)
//   idx 2 = Auto Start: ON/OFF (persisted boot preference)
// Pointers reference the shared gSensorsRows buffers — valid until the next
// call. Returns the row count. Consumed by renderSensorDetailLive().
size_t g2BuildSensorLiveList(const char** outRows, size_t maxRows) {
  if (!outRows || maxRows == 0) return 0;
  size_t n = 0;

  G2SensorRow rows[SENSORS_MAX_ROWS];
  size_t count = 0;
  buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
  const G2SensorRow* r = (gSensorsDetailIdx < count) ? &rows[gSensorsDetailIdx]
                                                     : nullptr;

  strncpy(gSensorsRows[0], "<- Sensors", SENSORS_ROW_LEN - 1);
  gSensorsRows[0][SENSORS_ROW_LEN - 1] = '\0';
  outRows[n++] = gSensorsRows[0];

  if (n < maxRows) {  // idx 1 — runtime Start/Stop (live enabled state)
    snprintf(gSensorsRows[1], SENSORS_ROW_LEN, "Run: %s",
             (r && r->enabled) ? "ON" : "OFF");
    outRows[n++] = gSensorsRows[1];
  }

  if (n < maxRows) {  // idx 2 — auto-start (persisted boot preference)
    const FeatureEntry* feat = r ? sensorFeature(r->featureId) : nullptr;
    bool* autoPtr = (feat && feat->enabledSetting) ? feat->enabledSetting : nullptr;
    if (autoPtr) snprintf(gSensorsRows[2], SENSORS_ROW_LEN, "Auto Start: %s",
                          *autoPtr ? "ON" : "OFF");
    else         snprintf(gSensorsRows[2], SENSORS_ROW_LEN, "Auto Start: n/a");
    outRows[n++] = gSensorsRows[2];
  }
  return n;
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
#if ENABLE_CAMERA_SENSOR
      // Lens can still be showing the CAM detail page (indices 2..5 = Capture ..
      // Settings) while our FSM is LIST — e.g. race during page-swap or duplicate
      // RX. Re-sync to CAM detail and handle the tap as detail.
      if (idx >= 2 && idx <= 5) {
        int camPos = -1;
        for (size_t i = 0; i < count; i++) {
          if (strcmp(rows[i].featureId, "camera") == 0) {
            camPos = (int)i;
            break;
          }
        }
        if (camPos >= 0) {
          DEBUG_G2F("[G2] Sensors: LIST idx=%u vs count=%u — recovering CAM detail "
                    "tap (glasses/host desync)",
                    (unsigned)idx,
                    (unsigned)count);
          gSensorsDetailIdx = (size_t)camPos;
          showSensorDetail(rows[(size_t)camPos]);
          g2SensorsHandleTap(idx);
          return;
        }
      }
#endif
      DEBUG_G2F("[G2] Sensors: tap idx=%u out of range (count=%u)",
                (unsigned)idx, (unsigned)count);
      return;
    }
    gSensorsDetailIdx = pos;
#if ENABLE_MICROPHONE_SENSOR
    // MIC drills into a dedicated compound page (list + live-readout
    // text widget) instead of the generic list-only detail. Provides
    // a per-tick UPDATE_TEXT readout (level, sample rate, recording)
    // that doesn't disturb the list row selection — see g2ShowMicDetail
    // and renderMicDetailLive in G2_Glasses.cpp. All other sensors
    // still use the legacy showSensorDetail() path.
    if (strcmp(rows[pos].featureId, "microphone") == 0) {
      if (g2ShowMicDetail()) return;
      DEBUG_G2F("[G2] Sensors: MIC detail compound start failed — "
                "falling back to generic showSensorDetail");
    }
#endif
    showSensorDetail(rows[pos]);
    return;
  }

  if (gSensorsLevel == SENSORS_LEVEL_LIVE) {
    // Generic non-camera sensor live page. Rows (see g2BuildSensorLiveList):
    //   0 = back, 1 = Run (runtime start/stop), 2 = Auto Start (persisted).
    if (idx == 0) { g2ShowSensorsMenu(); return; }

    G2SensorRow rows[SENSORS_MAX_ROWS];
    size_t count = 0;
    buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
    if (gSensorsDetailIdx >= count) { g2ShowSensorsMenu(); return; }
    const G2SensorRow& r = rows[gSensorsDetailIdx];

    const uint32_t now = millis();
    if ((uint32_t)(now - sSensorsDetailToggleLastMs) <
        kSensorsDetailToggleDebounceMs) {
      return;  // shared debounce
    }
    sSensorsDetailToggleLastMs = now;

    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;

    if (idx == 1) {
      // Runtime Start/Stop — the same open/close path the web/CLI uses.
      char cmd[24];
      sensorRuntimeCmd(r.featureId, /*open=*/ !r.enabled, cmd, sizeof(cmd));
      BROADCAST_PRINTF("[G2] Sensors: %s runtime %s (from glasses)",
                       r.label, r.enabled ? "STOP" : "START");
      (void)g2SubmitHijackCommand(cmd, cookie, nullptr, nullptr);
      // Re-CREATE so the Run row + readout refresh. Start/stop is async, so
      // the row label can lag one redraw; the live readout self-corrects per
      // tick once the sensor task comes up.
      showSensorDetail(r);
      return;
    }
    if (idx == 2) {
      // Auto-Start toggle (persisted boot preference). sensorautostart
      // accepts the row's featureId, incl. "gamepad" (-> inputAutoStart).
      const FeatureEntry* feat = sensorFeature(r.featureId);
      const bool cur = (feat && feat->enabledSetting) ? *feat->enabledSetting
                                                      : false;
      char cmd[48];
      sensorAutostartCmd(r.featureId, /*on=*/ !cur, cmd, sizeof(cmd));
      BROADCAST_PRINTF("[G2] Sensors: %s Auto Start %s -> %s (from glasses)",
                       r.label, cur ? "ON" : "OFF", cur ? "OFF" : "ON");
      (void)g2SubmitHijackCommand(cmd, cookie, nullptr, nullptr);
      showSensorDetail(r);
      return;
    }
    return;
  }

  // SENSORS_LEVEL_DETAIL
  if (idx == 0) {
    g2ShowSensorsMenu();
    return;
  }
  if (idx == 1) {
    const uint32_t now = millis();
    if ((uint32_t)(now - sSensorsDetailToggleLastMs) <
        kSensorsDetailToggleDebounceMs) {
      DEBUG_G2F("[G2] Sensors: detail toggle debounced (%ums)",
                (unsigned)kSensorsDetailToggleDebounceMs);
      return;
    }
    sSensorsDetailToggleLastMs = now;

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
    BROADCAST_PRINTF("[G2] Sensors: %s Auto Start %s -> %s (from glasses)",
                     r.label, prev ? "ON" : "OFF",
                     next ? "ON" : "OFF");

    // Runtime apply for hardware that supports hot start/stop is kept
    // INLINE on tap_disp rather than routed through cmd_exec. Two
    // reasons:
    //   (a) `cmd_camerastart` / `opencamera` is the SYNC variant
    //       (cameraPowerRequestStartSync, 60-s timeout) — running it on
    //       cmd_exec would stall the whole command queue. The inline
    //       `cameraPowerRequestStartAsync` returns instantly.
    //   (b) UX feedback. The redraw below depends on the new gXEnabled
    //       flag; doing it inline gives immediate visual confirmation.
    // The AUTHORITATIVE write — flipping the persisted auto-start flag
    // — IS routed through cmd_exec further down (the `*autostart` CLI
    // commands), so the auth check still gates persistence.
    bool skipImmediateRedraw = false;
    bool runtimeApplyOk      = true;
#if ENABLE_CAMERA_SENSOR
    if (strcmp(r.featureId, "camera") == 0) {
      if (next && !gCameraEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: starting camera (queued)...");
        if (cameraPowerRequestStartAsync()) {
          skipImmediateRedraw = true;
          sG2SensorsCamAsyncStartPending = true;
        } else {
          BROADCAST_PRINTF("[G2] Sensors: camera start queue full — abort persist");
          runtimeApplyOk = false;
        }
      } else if (!next && gCameraEnabled) {
        BROADCAST_PRINTF("[G2] Sensors: stopping camera (queued)...");
        if (cameraPowerRequestStopAsync()) {
          skipImmediateRedraw = true;
        } else {
          BROADCAST_PRINTF("[G2] Sensors: camera stop queue full — abort persist");
          runtimeApplyOk = false;
        }
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
    // Camera power runs asynchronously on `cam_pwr`; hook redraws when done.
    if (!skipImmediateRedraw) {
      buildRows(rows, SENSORS_MAX_ROWS, &count, /*includeStubs=*/ false);
      if (gSensorsDetailIdx < count) {
        showSensorDetail(rows[gSensorsDetailIdx]);
      } else {
        showSensorDetail(r);
      }
    }

    // Persist the auto-start flag via the per-feature CLI command
    // (route through cmd_exec). Each *autostart command is a thin
    // setSetting wrapper, so this is one queue submit + one NVS write —
    // no separate `savesettings` needed (the previous code's batched
    // writeSettingsJson is no longer required because we're not making
    // multiple setSetting calls inline).
    //
    // If runtime apply failed (e.g. camera queue full) we DON'T persist
    // — leaves the user's preference matching the actual hardware state.
    if (!runtimeApplyOk) {
      return;
    }

    char line[80];
    if (strcmp(r.featureId, "camera") == 0) {
      snprintf(line, sizeof(line), "cameraautostart %s", next ? "on" : "off");
    } else if (strcmp(r.featureId, "microphone") == 0) {
      snprintf(line, sizeof(line), "micautostart %s", next ? "on" : "off");
    } else {
      // Falls through to the generic per-sensor command, which covers
      // thermal/tof/imu/gps/fmradio/apds/gamepad. Unknown featureIds will
      // get a "Value must be on/off..." or similar error from the
      // command, which the cmd_exec log will surface.
      snprintf(line, sizeof(line), "sensorautostart %s %s",
               r.featureId, next ? "on" : "off");
    }

    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    // No completion callback — the inline showSensorDetail() above
    // already re-rendered (or the cam_pwr-done hook will). Persistence
    // is fire-and-forget; if the submit fails we fall back inline so
    // the flag still flips even if cmd_exec is wedged.
    if (!g2SubmitHijackCommand(line, cookie, nullptr, nullptr)) {
      DEBUG_G2F("[G2] Sensors: %s submit FAILED — inline setSetting fallback", line);
      setSetting(*feat->enabledSetting, next);
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
        // Dismiss returns to the CAM detail page (NOT the sensors landing
        // list) so the user lands back where they started. Same pattern
        // Camera Settings uses for its back row — see G2_Page_CameraSettings
        // handleListTap idx==0.
        g2ShowCameraViewer([]() { g2ReshowSensorsDetail(); });
      } else if (sG2SensorsCamAsyncStartPending) {
        DEBUG_G2F("[G2] Sensors detail: CAM Capture ignored — camera still "
                  "initializing (wait for CAM row to show ON)");
      } else {
        DEBUG_G2F("[G2] Sensors detail: CAM Capture tapped but camera not "
                  "connected — turn it ON first");
      }
      return;
    }
    if (idx == 3) {
      if (r.connected) {
        DEBUG_G2F("[G2] Sensors detail: CAM Stream tapped — opening stream");
        // Same dismiss target as Capture — return to CAM detail.
        g2ShowCameraStream([]() { g2ReshowSensorsDetail(); });
      } else if (sG2SensorsCamAsyncStartPending) {
        DEBUG_G2F("[G2] Sensors detail: CAM Stream ignored — camera still "
                  "initializing (wait for CAM row to show ON)");
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

#if ENABLE_CAMERA_SENSOR
static void g2SensorsOnCameraPowerDone() {
  sG2SensorsCamAsyncStartPending = false;
  if (gSensorsLevel != SENSORS_LEVEL_DETAIL) {
    return;
  }
  if (g2GetHijackPage() != G2_HIJACK_PAGE_SENSORS) {
    return;
  }
  g2ReshowSensorsDetail();
}

void g2RegisterSensorsCameraPowerHook() {
  cameraPowerWorkerEnsureStarted();
  cameraPowerSetPostHook(g2SensorsOnCameraPowerDone);
}
#endif

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
