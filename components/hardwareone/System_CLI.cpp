#include <string.h>

#include "OLED_Display.h"
#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Utils.h"
#include "System_WiFi.h"
#include "WebServer_Utils.h"

// External dependencies from main .ino
extern bool gCLIValidateOnly;

// Forward declarations
static void renderModuleHelp(const CommandModule* module, bool showAll);

// WebMirrorBuf, gWebMirror, gWebMirrorCap defined in WebServer_Utils.h

// ============================================================================
// Global CLI State
// ============================================================================

CLIState gCLIState = CLI_NORMAL;
bool gShowAllCommands = false;
volatile bool gInHelpRender = false;

// ============================================================================
// Help Rendering Functions
// ============================================================================

const char* renderHelpMain(bool showAll) {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  BROADCAST_PRINTF("  CLI Help Menu%s", showAll ? " (All Commands)" : "");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");
  broadcastOutput("Available Modules:");
  broadcastOutput("");

  // Get command modules and list them dynamically using metadata
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  for (size_t i = 0; i < moduleCount; i++) {
    // Skip core modules (CLI internal commands)
    if (modules[i].flags & CMD_MODULE_CORE) {
      continue;
    }
    
    const char* moduleName = modules[i].name;
    const char* description = modules[i].description ? modules[i].description : "No description";
    
    // Show all modules, or filter sensors by connection status
    if (showAll) {
      // Show all modules with descriptions
      BROADCAST_PRINTF("  %-12s - %s", moduleName, description);
    } else {
      // For sensor modules, show connection status
      if (modules[i].flags & CMD_MODULE_SENSOR) {
        bool connected = modules[i].isConnected ? modules[i].isConnected() : false;
        if (connected) {
          BROADCAST_PRINTF("  %-12s - %s (Connected)", moduleName, description);
        }
        // Skip disconnected sensors unless showAll is true
      } else {
        // Non-sensor modules always shown
        BROADCAST_PRINTF("  %-12s - %s", moduleName, description);
      }
    }
  }
  
  broadcastOutput("");
  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Core Commands:");
  broadcastOutput("  help [module]  - Show help (optionally for specific module)");
  broadcastOutput("  help all       - Show all commands (including hidden)");
  broadcastOutput("  back           - Return to main help menu");
  broadcastOutput("  exit           - Exit help mode");
  broadcastOutput("  clear          - Clear screen");
  broadcastOutput("");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type a module name to view its commands (e.g., 'wifi')");
  broadcastOutput("  • Type 'help all' to see all commands (including disconnected)");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

// Forward declaration (defined below)
static void renderHelpModuleByName(const char* moduleName, const char* title);

const char* renderHelpSystem() {
  renderHelpModuleByName("system", "System Commands");
  return "OK";
}

const char* renderHelpSettings() {
  renderHelpModuleByName("settings", "Settings & Configuration");
  return "OK";
}

const char* renderHelpAutomations() {
  renderHelpModuleByName("automation", "Automations - Scheduled Tasks & Conditional Commands");
  return "OK";
}

const char* renderHelpEspnow() {
  renderHelpModuleByName("espnow", "ESP-NOW - Wireless Peer-to-Peer Communication");
  return "OK";
}

const char* renderHelpWifi() {
  renderHelpModuleByName("wifi", "WiFi Network Management");
  return "OK";
}

const char* renderHelpSensors() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  BROADCAST_PRINTF("  Sensor Commands%s", gShowAllCommands ? " (All Available)" : " (Connected Only)");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render sensor modules dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Render all sensor modules dynamically using CMD_MODULE_SENSOR flag
  for (size_t i = 0; i < moduleCount; i++) {
    if (modules[i].flags & CMD_MODULE_SENSOR) {
      renderModuleHelp(&modules[i], gShowAllCommands);
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help sensors' to refresh this list");
  broadcastOutput("  • Type 'help all' to see disconnected sensors too");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

// ============================================================================
// CLI Navigation Functions
// ============================================================================

bool handleHelpNavigation(const String& cmd, char* out, size_t outSize) {
  DEBUGF(DEBUG_CLI, "[handleHelpNavigation] cmd='%s', gCLIState=%d", cmd.c_str(), (int)gCLIState);
  if (gCLIState == CLI_NORMAL) return false;

  gInHelpRender = true;

  // Helper lambda to write OK to out and clear the render flag
  auto respond = [&]() {
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
  };

  String lc = cmd;
  lc.toLowerCase();
  lc.trim();

  // ── Utility commands ─────────────────────────────────────────────────────
  if (lc == "back" || lc == "exit") {
    broadcastOutput(exitToNormalBanner());
    respond();
    return true;
  }
  if (lc == "clear") {
    broadcastOutput("\033[2J\033[H");
    respond();
    return true;
  }
  if (lc == "tail") {
    helpSuppressedTailDump();
    respond();
    return true;
  }

  // ── Sensors: special aggregate view (all sensor modules by flag) ──────────
  if (lc == "sensors") {
    gCLIState = CLI_HELP_MODULE;
    renderHelpSensors();
    respond();
    return true;
  }

  // ── Dynamic module lookup: any registered module name ────────────────────
  // Only match single-word inputs (no arguments)
  if (lc.indexOf(' ') == -1) {
    size_t moduleCount;
    const CommandModule* modules = getCommandModules(moduleCount);

    for (size_t i = 0; i < moduleCount; i++) {
      // Skip internal-only modules
      if (modules[i].flags & CMD_MODULE_CORE) continue;

      // Case-insensitive comparison without heap allocation
      const char* mname = modules[i].name;
      if (lc.equalsIgnoreCase(mname)) {
        gCLIState = CLI_HELP_MODULE;
        renderHelpModuleByName(mname, nullptr);
        respond();
        return true;
      }
    }
  }

  gInHelpRender = false;
  return false;  // Not a help navigation command
}

String exitToNormalBanner() {
  gCLIState = CLI_NORMAL;
  gShowAllCommands = false;  // Reset show all flag
  // Restore hidden history when leaving help
  String banner = "Returned to normal CLI mode.";
  return banner;
}

// ============================================================================
// CLI Command Implementations
// ============================================================================

static void broadcastHelpUsageIndented(const char* usage) {
  if (!usage || !usage[0]) {
    return;
  }

  const char* p = usage;
  while (*p) {
    const char* eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[200];
    if (len >= sizeof(line)) {
      len = sizeof(line) - 1;
    }
    memcpy(line, p, len);
    line[len] = '\0';

    if (line[0]) {
      bool hasPercent = (strchr(line, '%') != nullptr);
      bool canFormat = false;
      if (hasPercent) {
        canFormat = true;
        for (const char* q = line; *q; ++q) {
          if (*q != '%') continue;
          ++q;
          if (!*q) { canFormat = false; break; }
          if (*q == '%') continue;
          if (*q == 'd') continue;
          canFormat = false;
          break;
        }
      }

      if (hasPercent && canFormat) {
        char formatted[220];
        formatted[0] = '\0';

#if ENABLE_WIFI
        snprintf(formatted, sizeof(formatted), line, gWifiNetworkCount);
#else
        strncpy(formatted, line, sizeof(formatted) - 1);
        formatted[sizeof(formatted) - 1] = '\0';
#endif

        BROADCAST_PRINTF("  %-28s   %s", "", formatted);
      } else {
        BROADCAST_PRINTF("  %-28s   %s", "", line);
      }
    }

    if (!eol) {
      break;
    }
    p = eol + 1;
  }
}

// Helper function to render help for a specific module
static void renderModuleHelp(const CommandModule* module, bool showAll) {
  const char* moduleName = module->name;
  const CommandEntry* commands = module->commands;
  size_t count = module->count;
  bool isSensorModule = (module->flags & CMD_MODULE_SENSOR) != 0;
  
  bool isConnected = true;
  if (isSensorModule && module->isConnected) {
    isConnected = module->isConnected();
  }
  
  // Show module if connected or if showing all commands
  if (showAll || isConnected || !isSensorModule) {
    // Module header — stack buffer, no heap allocation
    char upperName[32];
    size_t nameLen = strlen(moduleName);
    if (nameLen >= sizeof(upperName)) nameLen = sizeof(upperName) - 1;
    for (size_t j = 0; j < nameLen; j++) upperName[j] = toupper((unsigned char)moduleName[j]);
    upperName[nameLen] = '\0';

    if (isSensorModule) {
      BROADCAST_PRINTF("%s Commands%s:",
                       upperName,
                       isConnected ? " (Connected)" : " (Not Connected)");
    } else {
      BROADCAST_PRINTF("%s Commands:", upperName);
    }
    
    // Show connection status for sensors
    if (isSensorModule) {
      if (isConnected) {
        broadcastOutput("  • Module is active and ready");
      } else {
        broadcastOutput("  • Module not detected or not initialized");
      }
    }
    
    // List all commands in this module
    for (size_t i = 0; i < count; i++) {
      if (commands[i].help) {
        BROADCAST_PRINTF("  %-28s - %s", commands[i].name, commands[i].help);
        broadcastHelpUsageIndented(commands[i].usage);
      }
    }
    broadcastOutput("");
  }
}

// ============================================================================
// Shared Help Helpers
// ============================================================================

// Standard navigation footer printed at the bottom of every module help page.
static void broadcastHelpNavFooter(const char* moduleName) {
  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  BROADCAST_PRINTF("  • Type 'help %s' to refresh this page", moduleName);
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");
}

// Render help for a single named module looked up from the command registry.
// title may be nullptr — if so the title is derived from the module name.
// All output is broadcast directly; nothing is returned.
static void renderHelpModuleByName(const char* moduleName, const char* title) {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  if (title) {
    BROADCAST_PRINTF("  %s", title);
  } else {
    char upper[32];
    size_t j = 0;
    for (; moduleName[j] && j < sizeof(upper) - 1; j++)
      upper[j] = toupper((unsigned char)moduleName[j]);
    upper[j] = '\0';
    BROADCAST_PRINTF("  %s Module", upper);
  }
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);

  bool found = false;
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcmp(modules[i].name, moduleName) == 0) {
      renderModuleHelp(&modules[i], true);
      found = true;
      break;
    }
  }

  if (!found) {
    BROADCAST_PRINTF("  (No commands registered for module '%s')", moduleName);
    broadcastOutput("");
  }

  broadcastHelpNavFooter(moduleName);
}

static const char* cmd_help(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
  args.trim();

  if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }

  // ── Plain "help" — enter / return to main help menu ──────────────────────
  if (args.length() == 0) {
    gWebMirror.clear();
    gCLIState = CLI_HELP_MAIN;
    DEBUGF(DEBUG_CLI, "[cmd_help] gCLIState -> CLI_HELP_MAIN");
    gInHelpRender = true;
    renderHelpMain(gShowAllCommands);
    gInHelpRender = false;
    return "OK";
  }

  gInHelpRender = true;

  // ── Meta-commands ─────────────────────────────────────────────────────────
  if (args == "all") {
    gShowAllCommands = true;
    gCLIState = CLI_HELP_MAIN;
    renderHelpMain(true);
    gInHelpRender = false;
    return "OK";
  }
  if (args == "tail") {
    helpSuppressedTailDump();
    gInHelpRender = false;
    return "OK";
  }

  // ── Sensors: aggregate view across all sensor modules ─────────────────────
  if (args == "sensors") {
    gCLIState = CLI_HELP_MODULE;
    gShowAllCommands = false;
    renderHelpSensors();
    gInHelpRender = false;
    return "OK";
  }

  // ── Dynamic lookup: match any registered module name ─────────────────────
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);

  for (size_t i = 0; i < moduleCount; i++) {
    if (args.equalsIgnoreCase(modules[i].name)) {
      gCLIState = CLI_HELP_MODULE;
      renderHelpModuleByName(modules[i].name, nullptr);
      gInHelpRender = false;
      return "OK";
    }
  }

  // ── Unknown topic — list available modules ────────────────────────────────
  broadcastOutput("Unknown help topic. Available modules:");
  for (size_t i = 0; i < moduleCount; i++) {
    if (modules[i].flags & CMD_MODULE_CORE) continue;
    BROADCAST_PRINTF("  %s", modules[i].name);
  }
  broadcastOutput("Special topics: sensors, all, tail");
  gInHelpRender = false;
  return "ERROR";
}

static const char* cmd_back(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (gCLIState != CLI_NORMAL) {
    gCLIState = CLI_HELP_MAIN;
    gInHelpRender = true;
    renderHelpMain(gShowAllCommands);
    gInHelpRender = false;
    return "OK";
  }
  return "Not in help mode.";
}

static const char* cmd_exit(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gCLIState != CLI_NORMAL) {
    String banner = exitToNormalBanner();
    broadcastOutput(banner);
    helpSuppressedPrintAndReset();
    return "OK";
  }
  return "Already in normal CLI mode.";
}

static const char* cmd_clear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }
  gWebMirror.clear();
  return "\033[2J\033[H"
         "CLI history cleared.";
}

// ============================================================================
// CLI Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry cliCommands[] = {
  { "help", "Display help menu (help [topic])", false, cmd_help },
  { "back", "Return to main help menu", false, cmd_back },
  { "exit", "Exit help mode", false, cmd_exit },
  { "clear", "Clear CLI history", false, cmd_clear },
};

const size_t cliCommandsCount = sizeof(cliCommands) / sizeof(cliCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp
