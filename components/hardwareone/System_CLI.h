#ifndef CLI_SYSTEM_H
#define CLI_SYSTEM_H

#include <Arduino.h>

#include "System_I2C.h"
#include "System_Utils.h"
#include "WebServer_Utils.h"

// ============================================================================
// CLI System - Help Interface and State Management
// ============================================================================

// CLI State enum - tracks current help/normal mode
enum CLIState {
  CLI_NORMAL,
  CLI_HELP_MAIN,
  CLI_HELP_SYSTEM,
  CLI_HELP_WIFI,
  CLI_HELP_SENSORS,
  CLI_HELP_SETTINGS,
  CLI_HELP_AUTOMATIONS,
  CLI_HELP_ESPNOW
};

// Global CLI state variables
extern CLIState gCLIState;
extern bool gShowAllCommands;
extern volatile bool gInHelpRender;

// ============================================================================
// Help Rendering Functions
// ============================================================================

/**
 * @brief Render main help menu
 * @param showAll If true, show all commands including disconnected sensors
 * @return Status string
 */
const char* renderHelpMain(bool showAll);

/**
 * @brief Render help for System commands
 * @return Help text for system commands
 */
const char* renderHelpSystem();

/**
 * @brief Render help for WiFi commands
 * @return Help text for WiFi commands
 */
const char* renderHelpWifi();

/**
 * @brief Render help for Automation commands
 * @return Help text for automation commands
 */
const char* renderHelpAutomations();

/**
 * @brief Render help for ESP-NOW commands
 * @return Help text for ESP-NOW commands
 */
const char* renderHelpEspnow();

/**
 * @brief Render help for Settings commands
 * @return Help text for settings commands
 */
const char* renderHelpSettings();

/**
 * @brief Render help for Sensor commands
 * @return Help text for sensor commands (dynamic based on connected sensors)
 */
const char* renderHelpSensors();

// ============================================================================
// CLI Navigation Functions
// ============================================================================

/**
 * @brief Handle help navigation commands (system, wifi, sensors, etc.)
 * Called from executeCommand() when in help mode
 * @param cmd Command string
 * @param out Output buffer
 * @param outSize Size of output buffer
 * @return True if command was handled as help navigation
 */
bool handleHelpNavigation(const String& cmd, char* out, size_t outSize);

/**
 * @brief Exit from help mode back to normal CLI
 * @return Banner message
 */
String exitToNormalBanner();

// ============================================================================
// CLI Command Registry
// ============================================================================

// Forward declaration from system_utils.h
struct CommandEntry;

// CLI command registry (help, clear, etc.)
extern const CommandEntry cliCommands[];
extern const size_t cliCommandsCount;

#endif // CLI_SYSTEM_H
