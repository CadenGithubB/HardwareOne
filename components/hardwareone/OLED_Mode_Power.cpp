// OLED_Mode_Power.cpp - Power management display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"  // For executeOLEDCommand, OLEDScrollState
#include "HAL_Input.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_Power.h"
#include "System_Debug.h"

// External references
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

// Power menu scroll states (replaces manual selection variables)
static OLEDScrollState sPowerMainScroll;
static OLEDScrollState sPowerCpuScroll;
static OLEDScrollState sPowerSleepScroll;
static bool sPowerScrollInitialized = false;

static void initPowerScrollStates() {
  if (sPowerScrollInitialized) return;
  oledScrollInit(&sPowerMainScroll, nullptr, 4);
  oledScrollInit(&sPowerCpuScroll, nullptr, 4);
  oledScrollInit(&sPowerSleepScroll, nullptr, 4);
  sPowerScrollInitialized = true;
}

static void populatePowerMainMenu() {
  initPowerScrollStates();
  int savedSel = sPowerMainScroll.selectedIndex;
  int savedOff = sPowerMainScroll.scrollOffset;
  oledScrollClear(&sPowerMainScroll);
  oledScrollAddItem(&sPowerMainScroll, "Adjust CPU Power");
  oledScrollAddItem(&sPowerMainScroll, "Sleep Settings");
  sPowerMainScroll.selectedIndex = savedSel < sPowerMainScroll.itemCount ? savedSel : 0;
  sPowerMainScroll.scrollOffset = savedOff;
}

static void populatePowerCpuMenu() {
  initPowerScrollStates();
  int savedSel = sPowerCpuScroll.selectedIndex;
  int savedOff = sPowerCpuScroll.scrollOffset;
  oledScrollClear(&sPowerCpuScroll);
  oledScrollAddItem(&sPowerCpuScroll, "Performance 240MHz");
  oledScrollAddItem(&sPowerCpuScroll, "Balanced 160MHz");
  oledScrollAddItem(&sPowerCpuScroll, "PowerSaver 80MHz");
  oledScrollAddItem(&sPowerCpuScroll, "UltraSaver 40MHz");
  sPowerCpuScroll.selectedIndex = savedSel < sPowerCpuScroll.itemCount ? savedSel : 0;
  sPowerCpuScroll.scrollOffset = savedOff;
}

static void populatePowerSleepMenu() {
  initPowerScrollStates();
  int savedSel = sPowerSleepScroll.selectedIndex;
  int savedOff = sPowerSleepScroll.scrollOffset;
  oledScrollClear(&sPowerSleepScroll);
  oledScrollAddItem(&sPowerSleepScroll, "Light Sleep");
  oledScrollAddItem(&sPowerSleepScroll, "Screen Off");
  oledScrollAddItem(&sPowerSleepScroll, "Restart Device");
  sPowerSleepScroll.selectedIndex = savedSel < sPowerSleepScroll.itemCount ? savedSel : 0;
  sPowerSleepScroll.scrollOffset = savedOff;
}

// ============================================================================
// Power Menu Display Functions
// ============================================================================

void displayPower() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerMainMenu();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print(getPowerModeName(gSettings.powerMode));
  oledDisplay->print(": ");
  oledDisplay->print(getCpuFrequencyMhz());
  oledDisplay->println("MHz");
  oledDisplay->println();
  
  for (int i = 0; i < sPowerMainScroll.itemCount; i++) {
    oledDisplay->print(i == sPowerMainScroll.selectedIndex ? "> " : "  ");
    oledDisplay->println(sPowerMainScroll.items[i].line1);
  }
}

void displayPowerCPU() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerCpuMenu();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  for (int i = 0; i < sPowerCpuScroll.itemCount; i++) {
    oledDisplay->print(i == sPowerCpuScroll.selectedIndex ? "> " : "  ");
    oledDisplay->println(sPowerCpuScroll.items[i].line1);
  }
}

void displayPowerSleep() {
  if (!oledDisplay || !oledConnected) return;
  populatePowerSleepMenu();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  for (int i = 0; i < sPowerSleepScroll.itemCount; i++) {
    oledDisplay->print(i == sPowerSleepScroll.selectedIndex ? "> " : "  ");
    oledDisplay->println(sPowerSleepScroll.items[i].line1);
  }
}

// ============================================================================
// Power Menu Actions
// ============================================================================

static void executePowerAction() {
  int sel = sPowerMainScroll.selectedIndex;
  if (sel == 0) {
    pushOLEDMode(currentOLEDMode);
    setOLEDMode(OLED_POWER_CPU);
  } else if (sel == 1) {
    pushOLEDMode(currentOLEDMode);
    setOLEDMode(OLED_POWER_SLEEP);
  }
}

static void executePowerCpuAction() {
  const char* cmds[] = { "power mode perf", "power mode balanced", "power mode saver", "power mode ultra" };
  int sel = sPowerCpuScroll.selectedIndex;
  if (sel >= 0 && sel < 4) {
    DEBUG_SYSTEMF("[POWER_OLED] Executing: %s (selection=%d)", cmds[sel], sel);
    DEBUG_SYSTEMF("[POWER_OLED] Current CPU freq before command: %lu MHz", (unsigned long)getCpuFrequencyMhz());
    executeOLEDCommand(cmds[sel]);
    delay(50);
    DEBUG_SYSTEMF("[POWER_OLED] Current CPU freq after command: %lu MHz", (unsigned long)getCpuFrequencyMhz());
  }
}

static void executePowerSleepAction() {
  switch (sPowerSleepScroll.selectedIndex) {
    case 0: executeOLEDCommand("lightsleep 20"); break;
    case 1: executeOLEDCommand("oledmode off"); break;
    case 2: executeOLEDCommand("reboot"); break;
  }
}

// ============================================================================
// Power Input Handlers (registered via OLEDModeEntry)
// ============================================================================

static bool powerMainInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
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

static const OLEDModeEntry sPowerModes[] = {
  { OLED_POWER,       "Power",    "power", displayPower,      nullptr, powerMainInputHandler,  false, -1 },
  { OLED_POWER_CPU,   "CPU Power","power", displayPowerCPU,   nullptr, powerCpuInputHandler,   false, -1 },
  { OLED_POWER_SLEEP, "Sleep",    "power", displayPowerSleep, nullptr, powerSleepInputHandler, false, -1 },
};

REGISTER_OLED_MODE_MODULE(sPowerModes, sizeof(sPowerModes) / sizeof(sPowerModes[0]), "Power");

#endif // ENABLE_OLED_DISPLAY
