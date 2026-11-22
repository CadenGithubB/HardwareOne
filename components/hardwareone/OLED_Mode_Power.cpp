// OLED_Mode_Power.cpp - Power management display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"  // For executeOLEDCommand, oledRunCommand, OLEDScrollState
#include "HAL_Input.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_Power.h"
#include "System_Debug.h"

// External references

// Power menu scroll states (replaces manual selection variables)
EXT_RAM_BSS_ATTR static OledScrollState sPowerMainScroll;
EXT_RAM_BSS_ATTR static OledScrollState sPowerCpuScroll;
EXT_RAM_BSS_ATTR static OledScrollState sPowerSleepScroll;
static bool sPowerScrollInitialized = false;

static void initPowerScrollStates() {
  if (sPowerScrollInitialized) return;
  // Single-line (8px) rows — same math as Network / LLM. Main menu leaves one
  // line for the live CPU status above the list.
  const int fullVis = OLED_CONTENT_HEIGHT / 8;
  const int mainVis = (OLED_CONTENT_HEIGHT - 8) / 8;
  oledScrollInit(&sPowerMainScroll, nullptr, mainVis > 0 ? mainVis : 1);
  oledScrollInit(&sPowerCpuScroll, nullptr, fullVis > 0 ? fullVis : 1);
  oledScrollInit(&sPowerSleepScroll, nullptr, fullVis > 0 ? fullVis : 1);
  sPowerScrollInitialized = true;
}

static void populatePowerMainMenu() {
  initPowerScrollStates();
  oledScrollClearKeepSelection(&sPowerMainScroll);
  oledScrollAddItem(&sPowerMainScroll, "Adjust CPU Power");
  oledScrollAddItem(&sPowerMainScroll, "Sleep Settings");
  oledScrollAddItem(&sPowerMainScroll, "Restart Device");
  oledScrollAddItem(&sPowerMainScroll, "RAM Flush");
  oledScrollClampSelection(&sPowerMainScroll);
}

static void populatePowerCpuMenu() {
  initPowerScrollStates();
  oledScrollClearKeepSelection(&sPowerCpuScroll);
  // Scroll items borrow these pointers; each row needs persistent storage.
  EXT_RAM_BSS_ATTR static char modeLabels[POWER_MODE_COUNT][40];  // PSRAM: drawn into the framebuffer
  // The unit is in the header title ("CPU (MHz)"): with it on every row,
  // "> Performance 400/100MHz" is 24 characters on a 21-character line.
  for (uint8_t mode = 0; mode < POWER_MODE_COUNT; ++mode) {
    const unsigned long active = getPowerModeActiveCpuFreq(mode);
    const unsigned long idle = getPowerModeIdleCpuFreq(mode);
    if (idle < active) {
      snprintf(modeLabels[mode], sizeof(modeLabels[mode]), "%s %lu/%lu",
               getPowerModeName(mode), active, idle);
    } else {
      snprintf(modeLabels[mode], sizeof(modeLabels[mode]), "%s %lu",
               getPowerModeName(mode), active);
    }
    oledScrollAddItem(&sPowerCpuScroll, modeLabels[mode]);
  }
  oledScrollClampSelection(&sPowerCpuScroll);
}

static void populatePowerSleepMenu() {
  initPowerScrollStates();
  oledScrollClearKeepSelection(&sPowerSleepScroll);
  // Live label for the power-save timeout. Scroll items store the char* (not a
  // copy), so the buffer must outlive the render — static gives it program
  // lifetime, and we rewrite it every frame so the value stays current.
  static char powerSaveLabel[24];
  if (gSettings.powerSaveTimeoutMinutes == 0) {
    snprintf(powerSaveLabel, sizeof(powerSaveLabel), "Power Saving: Off");
  } else {
    snprintf(powerSaveLabel, sizeof(powerSaveLabel), "Power Saving: %lum",
             (unsigned long)gSettings.powerSaveTimeoutMinutes);
  }
  oledScrollAddItem(&sPowerSleepScroll, powerSaveLabel);
  oledScrollAddItem(&sPowerSleepScroll, "Sleep 20s");
  oledScrollAddItem(&sPowerSleepScroll, "Screen Off");
  // "Restart Device" now lives on the main Power menu (with a confirmation).
  oledScrollClampSelection(&sPowerSleepScroll);
}

// ============================================================================
// Power Menu Display Functions
// ============================================================================

// Main Power keeps a 1-line status above the menu; the shared compact renderer
// draws the list one row below it with bounded labels and the scrollbar
// (OLED_TextField.h), the same list the Power submenus use.
static void renderPowerMainList() {
  oledScrollRenderSimple(oledDisplay, &sPowerMainScroll, true, OLED_CONTENT_START_Y + 8);
}

void displayPower() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerMainMenu();

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print(getPowerModeName(gSettings.powerMode));
  oledDisplay->print(": ");
  oledDisplay->print(getCpuFrequencyMhz());
  oledDisplay->print("MHz");

  renderPowerMainList();
}

void displayPowerCpu() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerCpuMenu();
  oledScrollRenderSimple(oledDisplay, &sPowerCpuScroll);
}

void displayPowerSleep() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerSleepMenu();
  oledScrollRenderSimple(oledDisplay, &sPowerSleepScroll);
}

// ============================================================================
// Power Menu Actions
// ============================================================================

// Confirm-dialog callbacks: fired only when the user picks "Yes". Routed
// through the command system so they get a normal [CMD] audit-log entry.
static void powerRebootConfirmed(void* /*userData*/) {
  DEBUG_SYSTEMF("[POWER_OLED] Reboot confirmed - restarting device");
  executeOledCommand("reboot");
}

static void powerRamFlushConfirmed(void* /*userData*/) {
  DEBUG_SYSTEMF("[POWER_OLED] RAM flush confirmed - capturing features and restarting");
  executeOledCommand("ramflush");
}

static void executePowerAction() {
  int sel = sPowerMainScroll.selectedIndex;
  if (sel == 0) {
    requestOledMode(OLED_POWER_CPU, "power.submenu.cpu");
  } else if (sel == 1) {
    requestOledMode(OLED_POWER_SLEEP, "power.submenu.sleep");
  } else if (sel == 2) {
    // Guard the reboot behind a confirmation, defaulting to "No", so a stray
    // A/X press can't restart the device.
    oledConfirmRequest("Restart device?", nullptr, powerRebootConfirmed, nullptr, false);
  } else if (sel == 3) {
    // Same confirmation pattern as Restart — ramflush also reboots.
    oledConfirmRequest("RAM flush restart?", nullptr, powerRamFlushConfirmed, nullptr, false);
  }
}

static void executePowerCpuAction() {
  // One command per gPowerModes row, in mode order; the rows above come from
  // the same table, so a mode without a command here would select nothing.
  static const char* const cmds[] = {
    "power mode perf",
    "power mode balanced",
    "power mode adaptive",
    "power mode ultra",
    "power mode locked",
  };
  static_assert(sizeof(cmds) / sizeof(cmds[0]) == POWER_MODE_COUNT,
                "OLED CPU menu needs one command per power mode");
  int sel = sPowerCpuScroll.selectedIndex;
  if (sel >= 0 && sel < (int)(sizeof(cmds) / sizeof(cmds[0]))) {
    DEBUG_SYSTEMF("[POWER_OLED] Executing: %s (selection=%d)", cmds[sel], sel);
    DEBUG_SYSTEMF("[POWER_OLED] Current CPU freq before command: %lu MHz", (unsigned long)getCpuFrequencyMhz());
    executeOledCommand(cmds[sel]);  // runs in the background; its reply is the toast
  }
}

static void executePowerSleepAction() {
  switch (sPowerSleepScroll.selectedIndex) {
    case 0: {
      // Cycle the power-save timeout through a preset ladder. The next step is
      // submitted as the admin-only `powersave <n>` command (admin check, [CMD]
      // audit; System_Power applies and persists it) in the background. A
      // refusal shows on the toast and leaves the value; the row label shows
      // the value every frame.
      // A value set off-ladder (e.g. via web) lands back on the first step.
      static const uint32_t presets[] = { 0, 1, 2, 5, 10, 15, 30, 60 };
      const int n = (int)(sizeof(presets) / sizeof(presets[0]));
      const uint32_t cur = gSettings.powerSaveTimeoutMinutes;
      int idx = 0;
      for (int i = 0; i < n; i++) { if (presets[i] == cur) { idx = i; break; } }
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "powersave %lu", (unsigned long)presets[(idx + 1) % n]);
      executeOledCommand(cmd);
      break;
    }
    case 1: executeOledCommand("lightsleep 20"); break;
    case 2: executeOledCommand("oledmode off"); break;
  }
}

// ============================================================================
// Power Input Handlers (registered via OledModeEntry)
// ============================================================================

static bool powerMainInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initPowerScrollStates();
  if (oledScrollHandleNav(&sPowerMainScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executePowerAction();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;  // Let global handler call oledMenuBack()
  }
  return false;
}

static bool powerCpuInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initPowerScrollStates();
  if (oledScrollHandleNav(&sPowerCpuScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executePowerCpuAction();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;  // Let global handler call oledMenuBack()
  }
  return false;
}

static bool powerSleepInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initPowerScrollStates();
  if (oledScrollHandleNav(&sPowerSleepScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executePowerSleepAction();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;  // Let global handler call oledMenuBack()
  }
  return false;
}

// ============================================================================
// Power Mode Registration
// ============================================================================

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OledModeEntry sPowerModes[] = {
  { OLED_POWER,       "Power",    "power", displayPower,      nullptr, powerMainInputHandler,  false, -1, "A:Select B:Back" },
  { OLED_POWER_CPU,   "CPU Power","power", displayPowerCpu,   nullptr, powerCpuInputHandler,   false, -1, "A:Execute B:Back" },
  { OLED_POWER_SLEEP, "Sleep",    "power", displayPowerSleep, nullptr, powerSleepInputHandler, false, -1, "A:Execute B:Back" },
};

REGISTER_OLED_MODE_MODULE(sPowerModes, sizeof(sPowerModes) / sizeof(sPowerModes[0]), "Power");

// Force linker to include this file - called from OLED_Utils.cpp
void oledPowerModeInit() {}

#endif // ENABLE_OLED_DISPLAY
