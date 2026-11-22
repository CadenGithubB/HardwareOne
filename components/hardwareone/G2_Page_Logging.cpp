// =============================================================================
// G2 glasses: the sensor logging page
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"
#include "System_SensorLogging.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "G2_HijackCmd.h"

#include "System_Settings.h"
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// Logging app — start/stop the sensor logger + pick which sensors it
// captures. Reached from the Apps launcher. sensorlog is device-wide (a
// mask of sensors → one file), which is why it lives here as its own app
// rather than as a per-sensor row. Plain list page (own showMenu); each
// action dispatches a real sensorlog command via cmd_exec, and the
// completion callback re-renders the list so the [x] markers / Start/Stop
// label reflect the new state. Sensor toggles are resolved by the logger
// when their commands run, using its shared sensor catalog.
// ─────────────────────────────────────────────────────────────────────
#define G2_LOG_ROW_LEN 24
EXT_RAM_BSS_ATTR static char        gLogRows[2 + kSensorLogSensorCount][G2_LOG_ROW_LEN];
static const char* gLogRowPtrs[2 + kSensorLogSensorCount];

// Rows: 0="<- System", 1=Start/Stop (live state), 2..N=[x] sensor toggles.
// Starting, stopping and choosing sensors need an admin: after a refusal the
// redraw keeps the live state and row 0 says why ("<- Needs an admin").
static size_t g2BuildLoggingRows() {
  g2LensNoticeBackRow(G2_HIJACK_PAGE_LOGGING, "<- System", gLogRows[0], G2_LOG_ROW_LEN);
  snprintf(gLogRows[1], G2_LOG_ROW_LEN, "%s",
           gSensorLoggingRunning ? "Stop logging" : "Start logging");
  const uint8_t selectedMask = sensorLogSelectedMask();
  for (size_t i = 0; i < kSensorLogSensorCount; i++) {
    snprintf(gLogRows[2 + i], G2_LOG_ROW_LEN, "[%c] %s",
             (selectedMask & kSensorLogSensors[i].bit) ? 'x' : ' ',
             kSensorLogSensors[i].label);
  }
  for (size_t i = 0; i < (2 + kSensorLogSensorCount); i++) gLogRowPtrs[i] = gLogRows[i];
  return 2 + kSensorLogSensorCount;
}

void g2ShowLoggingMenu() {
  size_t n = g2BuildLoggingRows();
  if (g2ShowListPage(gLogRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_LOGGING);
    DEBUG_G2F("[G2] Logging menu shown (%u rows, %s)", (unsigned)n,
              gSensorLoggingRunning ? "running" : "stopped");
  } else {
    DEBUG_G2F("[G2] Logging menu show FAILED");
  }
}

// The original command generation is checked by the lens applier before any
// row construction or display work. Recheck the page at dispatch as well.
static void g2RefreshLoggingMenu() {
  if (g2GetHijackPage() == G2_HIJACK_PAGE_LOGGING) g2ShowLoggingMenu();
}

static void onLoggingCmdDone(bool ok, const char* result,
                             const G2CmdCookie& cookie, void* /*userData*/) {
  if (!ok) {
    DEBUG_G2F("[G2] Logging: cmd result '%s'", result ? result : "");
  }
  (void)g2EnqueueRedraw(cookie, g2RefreshLoggingMenu, "Logging result");
}

static void g2BuildLoggingInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "Logging (%s) - start/stop + sensor select on lens; 'sensorlog' for CLI",
           gSensorLoggingRunning ? "running" : "stopped");
}

static void g2LoggingHandleTap(uint32_t idx) {
  if (idx == 0) { g2ShowSystemMenu(); return; }   // <- System

  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = 0;

  if (idx == 1) {   // Start / Stop
    if (gSensorLoggingRunning) {
      (void)g2SubmitHijackCommand("sensorlog stop", cookie, onLoggingCmdDone, nullptr);
    } else if (sensorLogSelectedMask() == 0) {
      // A bare start with no sensors selected succeeds but logs only
      // timestamps — nudge the user to pick a sensor instead.
      g2ShowText("Pick a sensor first");
      vTaskDelay(pdMS_TO_TICKS(1200));
      size_t n = g2BuildLoggingRows();
      g2ShowListPage(gLogRowPtrs, n);
      g2SetHijackPage(G2_HIJACK_PAGE_LOGGING);
    } else {
      // Default path from settings; quote it in case it ever holds a space.
      String line = String("sensorlog start \"") + gSettings.sensorLogPath + "\"";
      (void)g2SubmitHijackCommand(line.c_str(), cookie, onLoggingCmdDone, nullptr);
    }
    return;
  }

  size_t si = idx - 2;
  if (si < kSensorLogSensorCount) {
    String line = String("sensorlog sensors toggle ") + kSensorLogSensors[si].name;
    (void)g2SubmitHijackCommand(line.c_str(), cookie, onLoggingCmdDone, nullptr);
    return;
  }
}

const G2PageModule kLoggingPage = {
  // Hidden — reached via the System launcher (g2ShowLoggingMenu).
  "logging", nullptr,
  "Sensor logging (start/stop + sensor select on lens; 'sensorlog' for CLI)",
  /*buildText=*/  g2BuildLoggingInfo,
  /*showMenu=*/   g2ShowLoggingMenu,
  /*handleTap=*/  g2LoggingHandleTap,
  G2_HIJACK_PAGE_LOGGING,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/      "<- System",
  /*prefersTextWidget=*/ false,   // own showMenu — plain list, not live-text
  /*liveRender=*/     nullptr,
};

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
