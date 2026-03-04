/**
 * OLED Setup Wizard
 * 
 * OLED-specific rendering for the setup wizard.
 * Core logic is in System_SetupWizard.cpp
 */

#ifndef OLED_SETUP_WIZARD_H
#define OLED_SETUP_WIZARD_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "System_SetupWizard.h"

#if ENABLE_OLED_DISPLAY

// Joystick navigation state (used by unified wizard loop in System_SetupWizard.cpp)
struct JoystickNav {
  bool up;
  bool down;
  bool left;
  bool right;
};

// Joystick state management
void resetWizardJoystickState();
JoystickNav readWizardJoystickNav();

// Page renderers (OLED output only - called from unified loop when OLED connected)
void drawWizardHeader(int pageNum, int totalPages, const char* title);
void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction);
void renderFeaturesPage();
void renderSensorsPage();
void renderNetworkPage();
void renderSystemPage();
bool renderWiFiPage(SetupWizardResult& result);

// Input handlers (joystick/button - called from unified loop when OLED connected)
bool handleFeaturesInput(uint32_t buttons, JoystickNav& nav);
bool handleSensorsInput(uint32_t buttons, JoystickNav& nav);
bool handleNetworkInput(uint32_t buttons, JoystickNav& nav);
bool handleSystemInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result);

// Delegate to runSetupWizard() - kept for any existing call sites
SetupWizardResult runOLEDSetupWizard();

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_SETUP_WIZARD_H
