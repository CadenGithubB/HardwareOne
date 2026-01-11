#ifndef FIRST_TIME_SETUP_H
#define FIRST_TIME_SETUP_H

#include <Arduino.h>

/**
 * @file first_time_setup.h
 * @brief First-time device setup and initialization
 * 
 * This module handles the initial device setup when no user configuration exists.
 * It prompts for admin credentials and WiFi settings via serial console.
 */

// ============================================================================
// First-Time Setup State Management
// ============================================================================

enum FirstTimeSetupState {
  SETUP_NOT_NEEDED = 0,    // Settings file exists, no setup required
  SETUP_REQUIRED = 1,      // Settings missing, setup needed
  SETUP_IN_PROGRESS = 2,   // Currently collecting user input
  SETUP_COMPLETE = 3       // Setup finished, ready for reboot
};

enum SetupProgressStage {
  SETUP_PROMPT_USERNAME = 0,
  SETUP_PROMPT_PASSWORD = 1,
  SETUP_PROMPT_WIFI = 2,
  SETUP_PROMPT_HARDWARE = 3,
  SETUP_SAVING_CONFIG = 4,
  SETUP_FINISHED = 5
};

// Global state variables (written once during setup, read-only afterward)
// Thread-safe: single writer (setup) + multiple readers (OLED animation)
extern volatile FirstTimeSetupState gFirstTimeSetupState;
extern volatile SetupProgressStage gSetupProgressStage;

// Global flag to indicate first-time setup was just performed
// Used to skip WiFi connection during initial boot (connection happens on next boot)
extern bool gFirstTimeSetupPerformed;

// ============================================================================
// State Management Functions
// ============================================================================

// Early state detection (called immediately after filesystem init)
void detectFirstTimeSetupState();

// State access functions
inline bool isFirstTimeSetup();
inline void setFirstTimeSetupState(FirstTimeSetupState state);
inline void setSetupProgressStage(SetupProgressStage stage);

// Progress message functions
const char* getSetupProgressMessage(SetupProgressStage stage);

// Main first-time setup function
// Checks if users.json exists; if not, prompts for admin user and WiFi credentials
// Sets gFirstTimeSetupPerformed to true if setup was performed
void firstTimeSetupIfNeeded();

#endif // FIRST_TIME_SETUP_H
