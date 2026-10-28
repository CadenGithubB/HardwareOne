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
//     [1..5] [X] Performance/Balanced/Adaptive/UltraSaver/Locked <MHz>
//
// Presets apply DIRECTLY (non-destructive, no confirm — matching the OLED CPU
// menu and Camera Settings). Everything dispatches through g2SubmitHijackCommand
// so authorizeCommand enforces admin (same gates as CLI / OLED Power). Never
// call rebootDevice / setCpuFrequencyMhz inline on the tap task.
//
// CPU clock and I2C-drain policy remain in the shared power command owner.

#include "G2_Page_Power.h"
#include "System_Utils.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_HijackCmd.h"
#include "G2_Page_Common.h"    // g2ShowListPageAndPublish
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

// The presets, in gSettings.powerMode order, and the command each dispatches.
// `power mode X` (not raw `cpufreq N`) is the OLED-established path — it also
// sets display brightness and selects UltraSaver, whose idle-only XTAL clock
// the interactive `cpufreq` choices cannot express. Active/idle values below
// come from the shared target power policy.
static const char* const kPowerModeCmds[] = {
  "power mode perf",      // POWER_MODE_PERFORMANCE
  "power mode balanced",  // POWER_MODE_BALANCED
  "power mode adaptive",  // POWER_MODE_ADAPTIVE
  "power mode ultra",     // POWER_MODE_ULTRASAVER
  "power mode locked",    // POWER_MODE_LOCKED
};
// A mode added to System_Power.h without a row here would otherwise compile
// as a null command and crash on the tap.
static_assert(sizeof(kPowerModeCmds) / sizeof(kPowerModeCmds[0]) == POWER_MODE_COUNT,
              "kPowerModeCmds needs one command per power mode");

enum PowerAction : uint8_t {
  POWER_ACTION_NONE     = 0,
  POWER_ACTION_RESTART  = 1,
  POWER_ACTION_RAMFLUSH = 2,
  POWER_ACTION_POWEROFF = 3,
};

static PowerLevel  gLevel = POWER_LEVEL_ACTIONS;
static PowerAction gPending = POWER_ACTION_NONE;

#define POWER_ROW_LEN  24
#define POWER_MAX_ROWS  (1 + POWER_MODE_COUNT)   // back + one row per mode (the ACTIONS level uses 5)

EXT_RAM_BSS_ATTR static char        gRows[POWER_MAX_ROWS][POWER_ROW_LEN];
static const char* gRowPtrs[POWER_MAX_ROWS];

// The executor copies its short result; the lens worker consumes completions.
// Navigation and newer requests invalidate old results through menuGen.
static portMUX_TYPE gPowerResultMux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bool ready;
  uint32_t menuGen;
  PowerLevel level;
  char message[POWER_ROW_LEN];
} gPowerResult{};

static void powerBeginView(PowerLevel level) {
  g2BumpMenuGen();
  gLevel = level;
  portENTER_CRITICAL(&gPowerResultMux);
  gPowerResult.ready = false;
  portEXIT_CRITICAL(&gPowerResultMux);
}

// Draw the `n` rows just built for `level`. The level, and the action its
// Confirm row arms, become live only once the lens accepted those rows; a
// refused render leaves the visible rows with their current meaning.
static bool powerShowLevel(PowerLevel level, size_t n,
                           PowerAction pending = POWER_ACTION_NONE) {
  return g2ShowListPageAndPublish(gRowPtrs, n, [level, pending] {
    powerBeginView(level);
    gPending = pending;
  });
}

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
// Each mode displays the active and idle clocks from the shared target policy.
static size_t buildCpuRows() {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Back");
  for (uint8_t m = 0; m < POWER_MODE_COUNT; m++) {
    const char* mark = (m == gSettings.powerMode) ? "[X]" : "[ ]";
    // Keep complete three-digit active/idle clocks inside the 23-char row.
    const char* name = m == POWER_MODE_PERFORMANCE ? "Perf" :
                       m == POWER_MODE_ULTRASAVER ? "Ultra" : getPowerModeName(m);
    const unsigned act  = (unsigned)getPowerModeActiveCpuFreq(m);
    const unsigned idle = (unsigned)getPowerModeIdleCpuFreq(m);
    if (idle < act) {
      snprintf(gRows[m + 1], POWER_ROW_LEN, "%s %s %u/%uMHz",
               mark, name, act, idle);
    } else {
      snprintf(gRows[m + 1], POWER_ROW_LEN, "%s %s %uMHz",
               mark, name, act);
    }
  }
  for (size_t i = 0; i < 1 + POWER_MODE_COUNT; i++) gRowPtrs[i] = gRows[i];
  return 1 + POWER_MODE_COUNT;
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
           "CPU Power  pick a mode (Perf/Bal/Adaptive/Ultra/Locked)\n"
           "Restart    reboot the device\n"
           "RAM Flush  reboot, restore running features\n"
           "Power Off  enter deep sleep",
           getPowerModeName(gSettings.powerMode), (unsigned)getCpuFrequencyMhz());
}

// -----------------------------------------------------------------------------
// cmd_exec path — admin-gated via authorizeCommand
// -----------------------------------------------------------------------------

// Completions run this on the lens applier; submission refusals may use it on
// the tap dispatcher. Failure text stays in the back row until the next action.
static void renderPowerResult() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_POWER) return;
  PowerLevel level;
  char message[POWER_ROW_LEN];
  bool ready = false;
  portENTER_CRITICAL(&gPowerResultMux);
  if (gPowerResult.ready && gPowerResult.menuGen == g2CurrentMenuGen()) {
    level = gPowerResult.level;
    memcpy(message, gPowerResult.message, sizeof(message));
    gPowerResult.ready = false;
    ready = true;
  }
  portEXIT_CRITICAL(&gPowerResultMux);
  if (!ready) return;

  const size_t n = level == POWER_LEVEL_CPU ? buildCpuRows() : buildActionRows();
  if (message[0]) snprintf(gRows[0], POWER_ROW_LEN, "<- %.20s", message);
  g2ShowListPageAndPublish(gRowPtrs, n, [level] {
    if (gLevel != level) powerBeginView(level);
    gPending = POWER_ACTION_NONE;
  });
}

static void powerClearResult(uint32_t menuGen) {
  portENTER_CRITICAL(&gPowerResultMux);
  if (gPowerResult.menuGen == menuGen) gPowerResult.ready = false;
  portEXIT_CRITICAL(&gPowerResultMux);
}

static bool powerPublishResult(const G2CmdCookie& cookie, PowerLevel level,
                               const char* message) {
  char copy[POWER_ROW_LEN] = {};
  if (message) {
    if (strncmp(message, "Error: ", 7) == 0) message += 7;
    snprintf(copy, sizeof(copy), "%s", message);
  }
  bool published = false;
  portENTER_CRITICAL(&gPowerResultMux);
  if (g2GetHijackPage() == G2_HIJACK_PAGE_POWER &&
      cookie.menuGen == g2CurrentMenuGen()) {
    gPowerResult.menuGen = cookie.menuGen;
    gPowerResult.level = level;
    memcpy(gPowerResult.message, copy, sizeof(copy));
    gPowerResult.ready = true;
    published = true;
  }
  portEXIT_CRITICAL(&gPowerResultMux);
  return published;
}

static void powerQueueResult(const G2CmdCookie& cookie, PowerLevel level,
                             const char* message) {
  if (!powerPublishResult(cookie, level, message)) return;
  if (!g2EnqueueRedraw(cookie, renderPowerResult, "Power result")) {
    powerClearResult(cookie.menuGen);
  }
}

static bool powerCommandFailed(bool ok, const char* result) {
  return !ok || (result && (strncmp(result, "Error", 5) == 0 ||
                            strncmp(result, "ERROR", 5) == 0 ||
                            strstr(result, "refused")));
}

static void onPowerCmdDone(bool ok, const char* result,
                           const G2CmdCookie& cookie, void* /*userData*/) {
  // reboot / ramflush / deepsleep success paths do not return to us.
  if (!powerCommandFailed(ok, result)) return;
  DEBUG_G2F("[G2] Power: command failed: %s", result ? result : "no result");
  powerQueueResult(cookie, POWER_LEVEL_ACTIONS,
                   result && result[0] ? result : "Command failed");
}

// Preset completion only publishes its result and queues a redraw. The picker
// reads the actual power setting when the lens worker renders it.
static void onPowerPresetDone(bool ok, const char* result,
                              const G2CmdCookie& cookie, void* /*userData*/) {
  const bool failed = powerCommandFailed(ok, result);
  if (failed) DEBUG_G2F("[G2] Power: preset failed: %s", result ? result : "no result");
  powerQueueResult(cookie, POWER_LEVEL_CPU,
                   failed ? (result && result[0] ? result : "Set failed") : nullptr);
}

// A rejected submission is still on the tap dispatcher. Reuse the ordinary
// result row without queueing a command or waiting on a transient banner.
static void powerShowSubmitFailure(const G2CmdCookie& cookie, PowerLevel level) {
  if (powerPublishResult(cookie, level, "Busy - try again")) renderPowerResult();
}

// Apply a CPU preset directly (no confirm). Fire the command; the picker
// re-renders on completion. On a submit failure, stay on the picker.
static void submitPowerPreset(const char* line) {
  powerBeginView(POWER_LEVEL_CPU);
  G2CmdCookie cookie{};
  cookie.menuGen = g2CurrentMenuGen();
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gLevel;
  DEBUG_G2F("[G2] Power: preset '%s' via cmd_exec", line);
  if (!g2SubmitHijackCommand(line, cookie, onPowerPresetDone, nullptr)) {
    DEBUG_G2F("[G2] Power: preset '%s' submit FAILED", line);
    powerShowSubmitFailure(cookie, POWER_LEVEL_CPU);
  }
}

static void submitPowerCmd(const char* line, const char* pendingBanner) {
  powerBeginView(POWER_LEVEL_CONFIRM);
  G2CmdCookie cookie{};
  cookie.menuGen = g2CurrentMenuGen();
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gLevel;
  g2ShowText(pendingBanner);
  // Brief breath so the CREATE-text can hit the air before cmd_exec may
  // tear down BLE / reboot (rebootDevice itself also delays).
  vTaskDelay(pdMS_TO_TICKS(150));
  DEBUG_G2F("[G2] Power: submit '%s' via cmd_exec", line);
  if (!g2SubmitHijackCommand(line, cookie, onPowerCmdDone, nullptr)) {
    DEBUG_G2F("[G2] Power: '%s' submit FAILED — no inline mutate", line);
    powerShowSubmitFailure(cookie, POWER_LEVEL_ACTIONS);
  }
}

// -----------------------------------------------------------------------------
// Public — show menu
// -----------------------------------------------------------------------------

void g2ShowPowerMenu() {
  size_t n = buildActionRows();
  if (powerShowLevel(POWER_LEVEL_ACTIONS, n)) {
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
        size_t n = buildCpuRows();
        if (powerShowLevel(POWER_LEVEL_CPU, n)) {
          DEBUG_G2F("[G2] Power: CPU preset picker opened");
        }
        return;
      }
      PowerAction action;
      if (idx == 2) {
        action = POWER_ACTION_RESTART;
      } else if (idx == 3) {
        action = POWER_ACTION_RAMFLUSH;
      } else if (idx == 4) {
        action = POWER_ACTION_POWEROFF;
      } else {
        return;
      }
      size_t n = buildConfirmRows(action);
      if (powerShowLevel(POWER_LEVEL_CONFIRM, n, action)) {
        DEBUG_G2F("[G2] Power: confirm prompt for %s", actionDebugName(action));
      }
      return;
    }

    case POWER_LEVEL_CONFIRM: {
      if (idx == 0) {
        // <- Cancel: back to action list.
        const PowerAction was = gPending;
        size_t n = buildActionRows();
        if (powerShowLevel(POWER_LEVEL_ACTIONS, n)) {
          DEBUG_G2F("[G2] Power: cancelled (was %s)", actionDebugName(was));
        }
        return;
      }
      if (idx == 1) {
        if (gPending == POWER_ACTION_NONE) return;  // command already submitted
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
        size_t n = buildActionRows();
        powerShowLevel(POWER_LEVEL_ACTIONS, n);
        return;
      }
      if (idx >= 1 && idx <= POWER_MODE_COUNT) {
        submitPowerPreset(kPowerModeCmds[idx - 1]);   // rows 1..N → modes 0..N-1
      }
      return;
    }
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
