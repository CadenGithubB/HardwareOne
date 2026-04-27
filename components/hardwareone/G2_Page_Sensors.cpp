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
// Optional_EvenG2.cpp.

#include "G2_Page_Sensors.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "Optional_EvenG2.h"   // g2ShowText
#include "System_Debug.h"

// -----------------------------------------------------------------------------
// Per-sensor extern declarations
// -----------------------------------------------------------------------------
// Each sensor module owns its own gXEnabled / gXConnected pair. We re-declare
// them here as extern so we can probe their state without including each
// sensor's full header (most of which drag in unrelated includes). Guarded
// by the same compile flag the sensor itself uses, so a build with
// ENABLE_X=0 simply won't reference them — the table iteration below
// renders them as "stub" using the table-only metadata.

#if ENABLE_IMU_SENSOR
extern bool gImuEnabled;       extern bool gImuConnected;
#endif
#if ENABLE_TOF_SENSOR
extern bool gTofEnabled;       extern bool gTofConnected;
#endif
#if ENABLE_THERMAL_SENSOR
extern bool gThermalEnabled;   extern bool gThermalConnected;
#endif
#if ENABLE_GAMEPAD_SENSOR
extern bool gGamepadEnabled;   extern bool gGamepadConnected;
#endif
#if ENABLE_APDS_SENSOR
extern bool gApdsEnabled;      extern bool gApdsConnected;
#endif
#if ENABLE_RTC_SENSOR
extern bool gRtcEnabled;       extern bool gRtcConnected;
#endif
#if ENABLE_FM_RADIO
extern bool gFmRadioEnabled;   extern bool gFmRadioConnected;
#endif
#if ENABLE_GPS_SENSOR
extern bool gGpsEnabled;       extern bool gGpsConnected;
#endif
#if ENABLE_PRESENCE_SENSOR
extern bool gPresenceEnabled;  extern bool gPresenceConnected;
#endif
#if ENABLE_OLED_DISPLAY
extern bool gOledEnabled;
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

struct G2SensorRow {
  const char* label;         // 5-char short identifier (e.g. "IMU", "THERM")
  const char* hardware;      // canonical part name (e.g. "BNO055")
  bool        compiledIn;    // true if ENABLE_<X> at build time
  bool        enabled;       // gXEnabled at this moment (false if not compiled)
  bool        connected;     // gXConnected at this moment (false if not compiled)
};

// Helper to read a state pair when the flag is on, otherwise return false
// without touching the (non-existent) symbol. Branchless at runtime.
#define SENSOR_PAIR(en, conn) (en), (conn)

static void buildRows(G2SensorRow* rows, size_t maxRows, size_t* outCount) {
  size_t i = 0;
  auto add = [&](const char* lbl, const char* hw, bool compiled,
                 bool en, bool conn) {
    if (i >= maxRows) return;
    rows[i++] = { lbl, hw, compiled, en, conn };
  };

#if ENABLE_IMU_SENSOR
  add("IMU",   "BNO055",   true,  gImuEnabled,      gImuConnected);
#else
  add("IMU",   "BNO055",   false, false, false);
#endif

#if ENABLE_TOF_SENSOR
  add("TOF",   "VL53L4CX", true,  gTofEnabled,      gTofConnected);
#else
  add("TOF",   "VL53L4CX", false, false, false);
#endif

#if ENABLE_THERMAL_SENSOR
  add("THERM", "MLX90640", true,  gThermalEnabled,  gThermalConnected);
#else
  add("THERM", "MLX90640", false, false, false);
#endif

#if ENABLE_APDS_SENSOR
  add("APDS",  "APDS9960", true,  gApdsEnabled,     gApdsConnected);
#else
  add("APDS",  "APDS9960", false, false, false);
#endif

#if ENABLE_GAMEPAD_SENSOR
  add("GAMEP", "Seesaw",   true,  gGamepadEnabled,  gGamepadConnected);
#else
  add("GAMEP", "Seesaw",   false, false, false);
#endif

#if ENABLE_RTC_SENSOR
  add("RTC",   "DS3231",   true,  gRtcEnabled,      gRtcConnected);
#else
  add("RTC",   "DS3231",   false, false, false);
#endif

#if ENABLE_GPS_SENSOR
  add("GPS",   "PA1010D",  true,  gGpsEnabled,      gGpsConnected);
#else
  add("GPS",   "PA1010D",  false, false, false);
#endif

#if ENABLE_PRESENCE_SENSOR
  add("PRES",  "STHS34",   true,  gPresenceEnabled, gPresenceConnected);
#else
  add("PRES",  "STHS34",   false, false, false);
#endif

#if ENABLE_FM_RADIO
  add("FM",    "RDA5807",  true,  gFmRadioEnabled,  gFmRadioConnected);
#else
  add("FM",    "RDA5807",  false, false, false);
#endif

#if ENABLE_OLED_DISPLAY
  add("OLED",  "SSD1306",  true,  gOledEnabled,     gOledEnabled);
#else
  add("OLED",  "SSD1306",  false, false, false);
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

  // One line per sensor, columns: LABEL  STATE
  // Hardware name dropped from on-lens rendering — keeping it pushes
  // the encoded pb body over the 253-byte single-fragment ceiling
  // (firmware doesn't reassemble multi-fragment CREATE; see
  // docs/G2_PROTOCOL.md). Hardware mapping is still in the CLI
  // (`g2sensors`) and web UI for users who need it. State word is
  // picked for at-a-glance scanning, matches what the OLED sensor
  // mode shows so the user can switch between displays without
  // mental retranslation.
  for (size_t i = 0; i < count; i++) {
    const G2SensorRow& r = rows[i];
    const char* state =
        !r.compiledIn ? "stub" :
        r.connected   ? "on"   :
        r.enabled     ? "miss" :  // enabled by user but probe failed
                        "off";
    char line[24];
    // 6-char label column for alignment. G2 ASCII font is roughly
    // monospace; tabs render unpredictably so spaces it is.
    snprintf(line, sizeof(line), "%-6s %s\n", r.label, state);
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

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
