// =============================================================================
// G2 glasses — "Power" page implementation
// =============================================================================
// Three-level layout, all rendered as plain list-mode pages so the transport
// stays inside a single CREATE-list fragment (rows <=~22 chars each).
//
//   Level 1 (ACTIONS):
//     [0] <- Main Menu
//     [1] <mode> <MHz> >   (live status; opens the CPU-preset picker)
//     [2] Restart
//     [3] RAM Flush
//     [4] Power Off
//
//   Level 2 (CONFIRM):
//     [0] <- Cancel
//     [1] Confirm Restart / Confirm RAM Flush / Confirm Power Off
//
//   Level 3 (CPU presets — mirrors the OLED "Adjust CPU Power" menu):
//     [0] <- Back
//     [1..4] [X] Performance/Balanced/PowerSaver/UltraSaver <MHz>
//
// Presets apply DIRECTLY (non-destructive, no confirm — matching the OLED CPU
// menu and Camera Settings). Everything dispatches through g2SubmitHijackCommand
// so authorizeCommand enforces admin (same gates as CLI / OLED Power). Never
// call rebootDevice / setCpuFrequencyMhz inline on the tap task.
//
// NOTE (known firmware gap, NOT introduced here): `power mode X` → applyPowerMode
// calls setCpuFrequencyMhz raw, WITHOUT the I2C-drain guard that wraps the
// power-save downclock (setCpuFrequencyDrained, HardwareOne.cpp). The OLED and
// CLI already carry the same gap; hardening the command path is a separate
// firmware change. (Note: applyPowerMode now only ever sets >=80 MHz — the raw
// jump to UltraSaver's 40 MHz is gone; 40 is applied solely by the drain-guarded
// idle power-save path, so the riskiest transition is already covered.)

#include "G2_Page_Power.h"
#include "System_Utils.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_HijackCmd.h"
#include "System_Debug.h"
#include "System_Power.h"      // getPowerModeName / getPowerModeCpuFreq / POWER_MODE_*
#include "System_Settings.h"   // gSettings.powerMode
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

enum PowerLevel : uint8_t {
  POWER_LEVEL_ACTIONS = 0,
  POWER_LEVEL_CONFIRM = 1,
  POWER_LEVEL_CPU     = 2,   // CPU-freq / power-mode preset picker
};

// The four presets, in gSettings.powerMode order, and the command each
// dispatches. `power mode X` (not raw `cpufreq N`) is the OLED-established
// path — it also sets display brightness and selects UltraSaver, whose
// idle-only 40 MHz floor `cpufreq` (80/160/240) cannot express. (UltraSaver's
// ACTIVE clock is 80 MHz; the 40 MHz kicks in only when idle power-save
// blanks the screen — see powerSaveTick in HardwareOne.cpp.)
static const char* const kPowerModeCmds[4] = {
  "power mode perf",      // POWER_MODE_PERFORMANCE
  "power mode balanced",  // POWER_MODE_BALANCED
  "power mode saver",     // POWER_MODE_POWERSAVER
  "power mode ultra",     // POWER_MODE_ULTRASAVER
};

enum PowerAction : uint8_t {
  POWER_ACTION_NONE     = 0,
  POWER_ACTION_RESTART  = 1,
  POWER_ACTION_RAMFLUSH = 2,
  POWER_ACTION_POWEROFF = 3,
};

static PowerLevel  gLevel   = POWER_LEVEL_ACTIONS;
static PowerAction gPending = POWER_ACTION_NONE;

#define POWER_ROW_LEN  24
#define POWER_MAX_ROWS  6

EXT_RAM_BSS_ATTR static char        gRows[POWER_MAX_ROWS][POWER_ROW_LEN];
static const char* gRowPtrs[POWER_MAX_ROWS];

// -----------------------------------------------------------------------------
// Row builders
// -----------------------------------------------------------------------------

// ACTIONS rows. Rebuilt on every (re)entry so row 1 reflects live state — the
// current power mode and the ACTUAL core clock (getCpuFrequencyMhz reads the
// live HAL, so a power-save downclock shows too). Row 1 doubles as the entry
// to the preset picker.
static size_t buildActionRows() {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Main Menu");
  snprintf(gRows[1], POWER_ROW_LEN, "%s %uMHz >",
           getPowerModeName(gSettings.powerMode), (unsigned)getCpuFrequencyMhz());
  snprintf(gRows[2], POWER_ROW_LEN, "Restart");
  snprintf(gRows[3], POWER_ROW_LEN, "RAM Flush");
  snprintf(gRows[4], POWER_ROW_LEN, "Power Off");
  for (size_t i = 0; i < 5; i++) gRowPtrs[i] = gRows[i];
  return 5;
}

// CPU preset picker rows — an "[X] " marker on the row matching the current
// gSettings.powerMode (picker precedent from Camera Settings' resolution list).
// Modes show their interactive clock; UltraSaver shows "80/40" because it runs
// at the 80 MHz floor while used and only sinks to 40 MHz once idle/asleep.
static size_t buildCpuRows() {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Back");
  for (uint8_t m = 0; m < 4; m++) {
    const char* mark = (m == gSettings.powerMode) ? "[X]" : "[ ]";
    const unsigned act  = (unsigned)getPowerModeActiveCpuFreq(m);
    const unsigned idle = (unsigned)getPowerModeIdleCpuFreq(m);
    if (idle < act) {
      snprintf(gRows[m + 1], POWER_ROW_LEN, "%s %s %u/%uMHz",
               mark, getPowerModeName(m), act, idle);
    } else {
      snprintf(gRows[m + 1], POWER_ROW_LEN, "%s %s %uMHz",
               mark, getPowerModeName(m), act);
    }
  }
  for (size_t i = 0; i < 5; i++) gRowPtrs[i] = gRows[i];
  return 5;
}

static const char* confirmLabel(PowerAction action) {
  switch (action) {
    case POWER_ACTION_POWEROFF: return "Confirm Power Off";
    case POWER_ACTION_RAMFLUSH: return "Confirm RAM Flush";
    default:                    return "Confirm Restart";
  }
}

static const char* actionDebugName(PowerAction action) {
  switch (action) {
    case POWER_ACTION_POWEROFF: return "Power Off";
    case POWER_ACTION_RAMFLUSH: return "RAM Flush";
    case POWER_ACTION_RESTART:  return "Restart";
    default:                    return "None";
  }
}

static size_t buildConfirmRows(PowerAction action) {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Cancel");
  snprintf(gRows[1], POWER_ROW_LEN, "%s", confirmLabel(action));
  gRowPtrs[0] = gRows[0];
  gRowPtrs[1] = gRows[1];
  return 2;
}

// -----------------------------------------------------------------------------
// CLI text
// -----------------------------------------------------------------------------

void g2BuildPowerInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "Power - %s %uMHz\n"
           "CPU Power  pick a mode (Perf/Bal/Saver/Ultra)\n"
           "Restart    reboot the device\n"
           "RAM Flush  reboot, restore running features\n"
           "Power Off  enter deep sleep",
           getPowerModeName(gSettings.powerMode), (unsigned)getCpuFrequencyMhz());
}

// -----------------------------------------------------------------------------
// cmd_exec path — admin-gated via authorizeCommand
// -----------------------------------------------------------------------------

static void onPowerCmdDone(bool ok, const char* result,
                           const G2CmdCookie& /*cookie*/, void* /*userData*/) {
  // reboot / ramflush / deepsleep success paths do not return to us.
  // Failures: admin deny, cooldown refuse, or other Error: lines.
  if (ok && result && strstr(result, "refused")) {
    ok = false;  // deepsleep cooldown returns a non-Error string
  }
  if (ok) return;

  const char* msg = (result && result[0]) ? result : "Command failed";
  // Lens list rows are short — strip a leading "Error: " for fit.
  if (strncmp(msg, "Error: ", 7) == 0) msg += 7;
  char banner[48];
  snprintf(banner, sizeof(banner), "%.44s", msg);
  g2ShowText(banner);
  vTaskDelay(pdMS_TO_TICKS(1400));
  gLevel   = POWER_LEVEL_ACTIONS;
  gPending = POWER_ACTION_NONE;
  size_t n = buildActionRows();
  g2ShowListPage(gRowPtrs, n);
  g2SetHijackPage(G2_HIJACK_PAGE_POWER);
}

// Preset completion — re-render the CPU picker so the "[X]" reflects the now-
// updated mode (the command ran setSetting on cmd_exec before this fires), so
// the user can compare/re-pick, mirroring the OLED CPU list. Runs on
// cmd_exec_task; direct lens draws here are the established Power-page pattern.
static void onPowerPresetDone(bool ok, const char* result,
                              const G2CmdCookie& /*cookie*/, void* /*userData*/) {
  const bool failed = !ok || (result && strncmp(result, "Error", 5) == 0);
  if (failed) {
    const char* msg = (result && result[0]) ? result : "Set failed";
    if (strncmp(msg, "Error: ", 7) == 0) msg += 7;
    char banner[48];
    snprintf(banner, sizeof(banner), "%.44s", msg);
    g2ShowText(banner);
    vTaskDelay(pdMS_TO_TICKS(1400));
  }
  if (g2GetHijackPage() != G2_HIJACK_PAGE_POWER) return;  // user navigated away
  gLevel = POWER_LEVEL_CPU;
  size_t n = buildCpuRows();
  g2ShowListPage(gRowPtrs, n);
  g2SetHijackPage(G2_HIJACK_PAGE_POWER);
}

// Apply a CPU preset directly (no confirm). Fire the command; the picker
// re-renders on completion. On a submit failure, stay on the picker.
static void submitPowerPreset(const char* line) {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = 0;
  DEBUG_G2F("[G2] Power: preset '%s' via cmd_exec", line);
  if (!g2SubmitHijackCommand(line, cookie, onPowerPresetDone, nullptr)) {
    DEBUG_G2F("[G2] Power: preset '%s' submit FAILED", line);
    g2ShowText("Busy - try again");
    vTaskDelay(pdMS_TO_TICKS(1000));
    gLevel = POWER_LEVEL_CPU;
    size_t n = buildCpuRows();
    g2ShowListPage(gRowPtrs, n);
    g2SetHijackPage(G2_HIJACK_PAGE_POWER);
  }
}

static void submitPowerCmd(const char* line, const char* pendingBanner) {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = 0;
  g2ShowText(pendingBanner);
  // Brief breath so the CREATE-text can hit the air before cmd_exec may
  // tear down BLE / reboot (rebootDevice itself also delays).
  vTaskDelay(pdMS_TO_TICKS(150));
  DEBUG_G2F("[G2] Power: submit '%s' via cmd_exec", line);
  if (!g2SubmitHijackCommand(line, cookie, onPowerCmdDone, nullptr)) {
    DEBUG_G2F("[G2] Power: '%s' submit FAILED — no inline mutate", line);
    g2ShowText("Busy - try again");
    vTaskDelay(pdMS_TO_TICKS(1200));
    gLevel   = POWER_LEVEL_ACTIONS;
    gPending = POWER_ACTION_NONE;
    size_t n = buildActionRows();
    g2ShowListPage(gRowPtrs, n);
    g2SetHijackPage(G2_HIJACK_PAGE_POWER);
  }
}

// -----------------------------------------------------------------------------
// Public — show menu
// -----------------------------------------------------------------------------

void g2ShowPowerMenu() {
  gLevel   = POWER_LEVEL_ACTIONS;
  gPending = POWER_ACTION_NONE;

  size_t n = buildActionRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_POWER);
    DEBUG_G2F("[G2] Power menu shown (rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Power menu show FAILED");
  }
}

// -----------------------------------------------------------------------------
// Public — tap dispatch
// -----------------------------------------------------------------------------

void g2PowerHandleTap(uint32_t idx) {
  switch (gLevel) {

    case POWER_LEVEL_ACTIONS: {
      if (idx == 0) {
        // <- Back to root hijack menu.
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        extern void g2RedrawHijackMainMenu();
        g2RedrawHijackMainMenu();
        return;
      }
      if (idx == 1) {
        // Live status row → open the CPU preset picker (applies directly).
        gLevel = POWER_LEVEL_CPU;
        size_t n = buildCpuRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Power: CPU preset picker opened");
        return;
      }
      if (idx == 2) {
        gPending = POWER_ACTION_RESTART;
      } else if (idx == 3) {
        gPending = POWER_ACTION_RAMFLUSH;
      } else if (idx == 4) {
        gPending = POWER_ACTION_POWEROFF;
      } else {
        return;
      }
      gLevel = POWER_LEVEL_CONFIRM;
      size_t n = buildConfirmRows(gPending);
      g2ShowListPage(gRowPtrs, n);
      DEBUG_G2F("[G2] Power: confirm prompt for %s", actionDebugName(gPending));
      return;
    }

    case POWER_LEVEL_CONFIRM: {
      if (idx == 0) {
        // <- Cancel: back to action list.
        gLevel   = POWER_LEVEL_ACTIONS;
        PowerAction was = gPending;
        gPending = POWER_ACTION_NONE;
        size_t n = buildActionRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Power: cancelled (was %s)", actionDebugName(was));
        return;
      }
      if (idx == 1) {
        if (gPending == POWER_ACTION_POWEROFF) {
          submitPowerCmd("deepsleep", "Powering off...");
        } else if (gPending == POWER_ACTION_RAMFLUSH) {
          submitPowerCmd("ramflush", "Flushing RAM...");
        } else {
          submitPowerCmd("reboot", "Restarting...");
        }
        gPending = POWER_ACTION_NONE;
      }
      return;
    }

    case POWER_LEVEL_CPU: {
      if (idx == 0) {
        // <- Back to the action list (rebuilt so the status row is fresh).
        gLevel = POWER_LEVEL_ACTIONS;
        size_t n = buildActionRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if (idx >= 1 && idx <= 4) {
        submitPowerPreset(kPowerModeCmds[idx - 1]);   // rows 1..4 → modes 0..3
      }
      return;
    }
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
