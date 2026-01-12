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

// Run the setup wizard via OLED - returns result structure
SetupWizardResult runOLEDSetupWizard();

// Individual page renderers (for testing/reuse)
void drawWizardHeader(int pageNum, int totalPages, const char* title);
void drawHeapBar(int y);
void drawWizardFooter(const char* leftAction, const char* rightAction, const char* backAction);

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_SETUP_WIZARD_H
