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
  CLI_HELP_MAIN,    // main help menu
  CLI_HELP_MODULE,  // any specific module or sensor help page
  // Legacy aliases kept for ABI compatibility; all map to CLI_HELP_MODULE internally
  CLI_HELP_SYSTEM      = CLI_HELP_MODULE,
  CLI_HELP_WIFI        = CLI_HELP_MODULE,
  CLI_HELP_SENSORS     = CLI_HELP_MODULE,
  CLI_HELP_SETTINGS    = CLI_HELP_MODULE,
  CLI_HELP_AUTOMATIONS = CLI_HELP_MODULE,
  CLI_HELP_ESPNOW      = CLI_HELP_MODULE,
};

// Global CLI state variables
extern CLIState gCLIState;
extern bool gShowAllCommands;
extern volatile bool gInHelpRender;

// ============================================================================
// Help Rendering Functions
// ============================================================================

/**
 * @brief Render main help menu (broadcasts output directly).
 * @param showAll If true, show all commands including disconnected sensors.
 * @return "OK"
 */
const char* renderHelpMain(bool showAll);

/**
 * @brief Render help for connected sensor modules (broadcasts output directly).
 * @return "OK"
 */
const char* renderHelpSensors();

// The functions below are thin wrappers around renderHelpModuleByName().
// They are kept for backward compatibility; prefer calling renderHelpModuleByName()
// directly for new module help pages — no broadcastOutput() wrapping needed.
const char* renderHelpSystem();
const char* renderHelpWifi();
const char* renderHelpAutomations();
const char* renderHelpEspnow();
const char* renderHelpSettings();

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
