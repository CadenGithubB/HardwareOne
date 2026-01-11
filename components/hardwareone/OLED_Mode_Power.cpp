// OLED_Mode_Power.cpp - Power management display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"  // For executeOLEDCommand
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_Power.h"
#include "i2csensor-seesaw.h"  // For JOYSTICK_DEADZONE

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

// Power menu state
static int powerMenuSelection = 0;
static const int POWER_MAIN_ITEMS = 2;
static int powerCpuSelection = 0;
static const int POWER_CPU_ITEMS = 4;
static int powerSleepSelection = 0;
static const int POWER_SLEEP_ITEMS = 3;

// ============================================================================
// Power Menu Display Functions
// ============================================================================

void displayPower() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("POWER");
  oledDisplay->print(getPowerModeName(gSettings.powerMode));
  oledDisplay->print(": ");
  oledDisplay->print(getCpuFrequencyMhz());
  oledDisplay->println("MHz");
  oledDisplay->println();
  
  const char* options[] = { "Adjust CPU Power", "Sleep Settings" };
  for (int i = 0; i < POWER_MAIN_ITEMS; i++) {
    oledDisplay->print(i == powerMenuSelection ? "> " : "  ");
    oledDisplay->println(options[i]);
  }
}

void displayPowerCPU() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("CPU Power");
  oledDisplay->println();
  
  const char* options[] = { "Performance 240MHz", "Balanced 160MHz", "PowerSaver 80MHz", "UltraSaver 40MHz" };
  for (int i = 0; i < POWER_CPU_ITEMS; i++) {
    oledDisplay->print(i == powerCpuSelection ? "> " : "  ");
    oledDisplay->println(options[i]);
  }
}

void displayPowerSleep() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("Sleep Settings");
  oledDisplay->println();
  
  const char* options[] = { "Light Sleep", "Screen Off", "Restart Device" };
  for (int i = 0; i < POWER_SLEEP_ITEMS; i++) {
    oledDisplay->print(i == powerSleepSelection ? "> " : "  ");
    oledDisplay->println(options[i]);
  }
}

// ============================================================================
// Power Menu Navigation
// ============================================================================

void powerMenuUp() {
  if (powerMenuSelection > 0) powerMenuSelection--;
  else powerMenuSelection = POWER_MAIN_ITEMS - 1;
}

void powerMenuDown() {
  if (powerMenuSelection < POWER_MAIN_ITEMS - 1) powerMenuSelection++;
  else powerMenuSelection = 0;
}

void powerCpuUp() {
  if (powerCpuSelection > 0) powerCpuSelection--;
  else powerCpuSelection = POWER_CPU_ITEMS - 1;
}

void powerCpuDown() {
  if (powerCpuSelection < POWER_CPU_ITEMS - 1) powerCpuSelection++;
  else powerCpuSelection = 0;
}

void powerSleepUp() {
  if (powerSleepSelection > 0) powerSleepSelection--;
  else powerSleepSelection = POWER_SLEEP_ITEMS - 1;
}

void powerSleepDown() {
  if (powerSleepSelection < POWER_SLEEP_ITEMS - 1) powerSleepSelection++;
  else powerSleepSelection = 0;
}

// ============================================================================
// Power Menu Actions
// ============================================================================

void executePowerAction() {
  // Main power menu - navigate to submenus using mode stack
  if (powerMenuSelection == 0) {
    pushOLEDMode(currentOLEDMode);
    currentOLEDMode = OLED_POWER_CPU;
  } else if (powerMenuSelection == 1) {
    pushOLEDMode(currentOLEDMode);
    currentOLEDMode = OLED_POWER_SLEEP;
  }
}

void executePowerCpuAction() {
  const char* cmds[] = { "power mode perf", "power mode balanced", "power mode saver", "power mode ultra" };
  if (powerCpuSelection < POWER_CPU_ITEMS) {
    Serial.printf("[POWER_OLED] Executing: %s (selection=%d)\n", cmds[powerCpuSelection], powerCpuSelection);
    Serial.printf("[POWER_OLED] Current CPU freq before command: %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
    executeOLEDCommand(cmds[powerCpuSelection]);
    // Small delay to allow frequency change to take effect
    delay(50);
    Serial.printf("[POWER_OLED] Current CPU freq after command: %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
  }
}

void executePowerSleepAction() {
  switch (powerSleepSelection) {
    case 0: executeOLEDCommand("lightsleep 20"); break;
    case 1: executeOLEDCommand("oledmode off"); break;
    case 2: executeOLEDCommand("reboot"); break;
  }
}

// ============================================================================
// Power Input Handler
// ============================================================================

bool powerInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Handle main power menu
  if (currentOLEDMode == OLED_POWER) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      executePowerAction();
      return true;
    }
    if (deltaY < -JOYSTICK_DEADZONE) {
      powerMenuUp();
      return true;
    }
    if (deltaY > JOYSTICK_DEADZONE) {
      powerMenuDown();
      return true;
    }
  }
  // Handle CPU power submenu
  else if (currentOLEDMode == OLED_POWER_CPU) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      executePowerCpuAction();
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      popOLEDMode();
      return true;
    }
    if (deltaY < -JOYSTICK_DEADZONE) {
      powerCpuUp();
      return true;
    }
    if (deltaY > JOYSTICK_DEADZONE) {
      powerCpuDown();
      return true;
    }
  }
  // Handle sleep settings submenu
  else if (currentOLEDMode == OLED_POWER_SLEEP) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      executePowerSleepAction();
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      popOLEDMode();
      return true;
    }
    if (deltaY < -JOYSTICK_DEADZONE) {
      powerSleepUp();
      return true;
    }
    if (deltaY > JOYSTICK_DEADZONE) {
      powerSleepDown();
      return true;
    }
  }
  return false;
}

#endif // ENABLE_OLED_DISPLAY
