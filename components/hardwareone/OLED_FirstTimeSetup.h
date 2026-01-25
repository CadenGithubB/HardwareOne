#ifndef OLED_FIRST_TIME_SETUP_H
#define OLED_FIRST_TIME_SETUP_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

/**
 * @file oled_first_time_setup.h
 * @brief OLED-based UI for first-time device setup
 * 
 * Provides interactive setup screens using OLED display and gamepad/joystick input.
 * Falls back to serial console if OLED is not available.
 */

// ============================================================================
// OLED Input Functions
// ============================================================================

/**
 * Get text input from user via OLED keyboard
 * Falls back to serial console if OLED unavailable
 * 
 * @param prompt Text prompt to display
 * @param isPassword If true, displays asterisks instead of characters
 * @param initialText Initial text to populate (optional)
 * @param maxLength Maximum input length
 * @param wasCancelled Output parameter - set to true if user cancelled (optional)
 * @return User's input string (empty if cancelled)
 */
String getOLEDTextInput(const char* prompt, bool isPassword = false, 
                        const char* initialText = nullptr, int maxLength = 32,
                        bool* wasCancelled = nullptr);

/**
 * Show yes/no prompt on OLED
 * Falls back to serial console if OLED unavailable
 * 
 * @param prompt Question to ask user
 * @param defaultYes If true, default selection is "Yes"
 * @return true if user selected Yes, false if No
 */
bool getOLEDYesNoPrompt(const char* prompt, bool defaultYes = true);

/**
 * Show WiFi network selection menu on OLED
 * Scans for networks and allows user to select one
 * Falls back to serial console if OLED unavailable
 * 
 * @param outSSID Selected network SSID (output parameter)
 * @return true if network selected, false if skipped/cancelled
 */
bool getOLEDWiFiSelection(String& outSSID);

/**
 * Display a message on OLED and wait for user acknowledgment
 * 
 * @param message Message to display
 * @param waitForButton If true, wait for button press before continuing
 */
void showOLEDMessage(const char* message, bool waitForButton = false);

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_FIRST_TIME_SETUP_H
