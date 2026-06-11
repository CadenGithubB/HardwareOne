/**
 * OLED Setup Wizard
 * 
 * OLED-specific rendering for the setup wizard.
 * Core logic is in System_SetupWizard.cpp
 */

#ifndef OLED_SETUPWIZARD_H
#define OLED_SETUPWIZARD_H

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

// Render a titled info "card" with the standard header + footer separator rules
// (matches the wizard's other screens). body word-wraps; footer is a free-form
// action hint. When pageCount > 0 a "n/N" indicator is drawn top-right and the
// title is clipped to the space left of it (so it never overlaps). OLED-only.
void drawSetupInfoPage(const char* title, const char* body, const char* footer,
                       int pageNum = 0, int pageCount = 0);
void renderFeaturesPage();
void renderSensorsPage();
void renderNetworkPage();
void renderSystemPage();
bool renderWiFiPage(SetupWizardResult& result);

// OLED handlers for text-input pages (own event loops)
void handleOLEDESPNowPage(SetupWizardResult& result, bool& running);
void handleOLEDMQTTPage(SetupWizardResult& result, bool& running);

// Input handlers (joystick/button - called from unified loop when OLED connected)
bool handleFeaturesInput(uint32_t buttons, JoystickNav& nav);
bool handleSensorsInput(uint32_t buttons, JoystickNav& nav);
bool handleNetworkInput(uint32_t buttons, JoystickNav& nav);
bool handleSystemInput(uint32_t buttons, JoystickNav& nav, SetupWizardResult& result);

// Conditional mode-picker page (WEBMODE -> HTTP/HTTPS, BTMODE -> Server/G2).
// Self-contained blocking sub-flow (OLED + serial) navigated via the wizard's
// page model. Called from runSetupWizard() when currentPage is one of those.
void handleModePage(SetupWizardPage page, SetupWizardResult& result, bool& running);

// Delegate to runSetupWizard() - kept for any existing call sites
SetupWizardResult runOLEDSetupWizard();

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_SETUPWIZARD_H
