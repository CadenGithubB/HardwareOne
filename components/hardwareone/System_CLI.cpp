#include <string.h>

#include "OLED_Display.h"
#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Utils.h"
#include "WebServer_Utils.h"

// External dependencies from main .ino
extern bool gCLIValidateOnly;
extern bool ensureDebugBuffer();
extern void broadcastOutput(const String& s);
// isSensorConnected declared in cli_system.h

// Forward declarations
static void renderModuleHelp(const char* moduleName, const CommandEntry* commands, size_t count, bool showAll);

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
  broadcastOutput("  help search    - Search help topics (future)");
  broadcastOutput("  tail           - Show last 32 suppressed messages");
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

const char* renderHelpSystem() {
  return "\033[2J\033[H"
         "════════════════════════════════════════════════════════════════\n"
         "  System Commands\n"
         "════════════════════════════════════════════════════════════════\n\n"
         "Status & Monitoring:\n"
         "  status              - Show system status\n"
         "  uptime              - Show system uptime\n"
         "  memory              - Show heap/PSRAM usage\n"
         "  memsum              - Print one-line memory summary (low-churn)\n"
         "  memreport           - Print comprehensive memory report (Task Manager style)\n"
         "  memtrack <on|off|reset|status>\n"
         "                      - Control allocation tracking\n"
         "  psram               - Show PSRAM usage details\n\n"
         "Filesystem:\n"
         "  fsusage             - Show filesystem usage (total/used/free)\n"
         "  files [path]        - List files in LittleFS (default '/')\n"
         "                        Example: files /logs\n"
         "  mkdir <path>        - Create a new folder\n"
         "                        Example: mkdir /data\n"
         "  rmdir <path>        - Remove an empty folder\n"
         "  filecreate <path>   - Create an empty file at path\n"
         "                        Example: filecreate /config/test.txt\n"
         "  fileview <path>     - View text file content (truncated)\n"
         "                        Example: fileview /logs/automation.log\n"
         "  filedelete <path>   - Delete the specified file\n\n"
         "Communication:\n"
         "  broadcast <message> (admin)\n"
         "                      - Send a message to all users\n"
         "                        Example: broadcast System maintenance in 5 minutes\n"
         "  broadcast --user <username> <message> (admin)\n"
         "                      - Send a message to a specific user\n"
         "                        Example: broadcast --user pop Task completed\n\n"
         "Other:\n"
         "  reboot              - Restart the system\n"
         "  clear               - Clear CLI history\n\n"
         "Type 'back' to return to help menu or 'exit' to return to CLI.";
}

const char* renderHelpSettings() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("  Settings & Configuration");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render settings module dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Find and render the settings module
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcmp(modules[i].name, "settings") == 0) {
      renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, true);
      break;
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help settings' to refresh this list");
  broadcastOutput("  • Type 'help debug' for debug commands");
  broadcastOutput("  • Type 'help users' for user management");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

const char* renderHelpAutomations() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("  Automations - Scheduled Tasks & Conditional Commands");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render automation module dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Find and render the automation module
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcmp(modules[i].name, "automation") == 0) {
      renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, true);
      break;
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help automation' to refresh this list");
  broadcastOutput("  • Type 'help all' to see all commands");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

const char* renderHelpEspnow() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("  ESP-NOW - Wireless Peer-to-Peer Communication");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render espnow module dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Find and render the espnow module
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcmp(modules[i].name, "espnow") == 0) {
      renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, true);
      break;
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help espnow' to refresh this list");
  broadcastOutput("  • Type 'help wifi' for WiFi network commands");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

const char* renderHelpWifi() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("  WiFi Network Management");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render wifi module dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Find and render the wifi module
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcmp(modules[i].name, "wifi") == 0) {
      renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, true);
      break;
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help wifi' to refresh this list");
  broadcastOutput("  • Type 'help espnow' for ESP-NOW wireless commands");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

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
  
  // Render sensor modules in a logical order
  const char* sensorModules[] = {"thermal", "tof", "imu", "gamepad", "apds", "gps", "fmradio", "camera"};
  const int numSensorModules = sizeof(sensorModules) / sizeof(sensorModules[0]);
  
  for (int i = 0; i < numSensorModules; i++) {
    // Find the module in the registry
    for (size_t j = 0; j < moduleCount; j++) {
      if (strcmp(modules[j].name, sensorModules[i]) == 0) {
        renderModuleHelp(modules[j].name, modules[j].commands, modules[j].count, gShowAllCommands);
        break;
      }
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help sensors' to refresh this list");
  broadcastOutput("  • Type 'help all' to see all sensors (including disconnected)");
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
  if (gCLIState == CLI_NORMAL) {
    return false;  // Not in help mode
  }
  gInHelpRender = true;  // allow help output to pass through gating

  String lc = cmd;
  lc.toLowerCase();

  // Handle help section navigation
  if (lc == "system") {
    gCLIState = CLI_HELP_SYSTEM;
    broadcastOutput(renderHelpSystem());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "wifi") {
    gCLIState = CLI_HELP_WIFI;
    broadcastOutput(renderHelpWifi());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "automations") {
    gCLIState = CLI_HELP_AUTOMATIONS;
    broadcastOutput(renderHelpAutomations());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "espnow") {
    gCLIState = CLI_HELP_ESPNOW;
    broadcastOutput(renderHelpEspnow());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "sensors") {
    gCLIState = CLI_HELP_SENSORS;
    broadcastOutput(renderHelpSensors());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "settings") {
    gCLIState = CLI_HELP_SETTINGS;
    broadcastOutput(renderHelpSettings());
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  }

  // Handle dynamic module navigation for all registered modules
  if (cmd.indexOf(' ') == -1) {  // Only single words (no arguments)
    size_t moduleCount;
    const CommandModule* modules = getCommandModules(moduleCount);
    
    for (size_t i = 0; i < moduleCount; i++) {
      String moduleName = String(modules[i].name);
      moduleName.toLowerCase();
      
      // Skip core modules that don't need help navigation
      if (strcmp(modules[i].name, "core") == 0 || strcmp(modules[i].name, "cli") == 0) {
        continue;
      }
      
      // Check if command matches module name
      if (lc == moduleName) {
        // Show module help using the existing help system
        renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, false);
        strncpy(out, "OK", outSize - 1);
        out[outSize - 1] = '\0';
        gInHelpRender = false;
        return true;
      }
    }
  }

  // Handle back/exit/clear navigation
  if (lc == "back" || lc == "exit") {
    String exitBanner = exitToNormalBanner();
    broadcastOutput(exitBanner);
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "clear") {
    broadcastOutput("\033[2J\033[H");
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
  } else if (lc == "tail") {
    // Show suppressed output tail while staying in help
    gInHelpRender = true;
    helpSuppressedTailDump();
    strncpy(out, "OK", outSize - 1);
    out[outSize - 1] = '\0';
    gInHelpRender = false;
    return true;
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
        extern int gWifiNetworkCount;
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
static void renderModuleHelp(const char* moduleName, const CommandEntry* commands, size_t count, bool showAll) {
  // Check if this is a sensor module that might not be connected
  bool isSensorModule = (strcmp(moduleName, "thermal") == 0 || 
                         strcmp(moduleName, "tof") == 0 || 
                         strcmp(moduleName, "imu") == 0 || 
                         strcmp(moduleName, "gamepad") == 0 || 
                         strcmp(moduleName, "apds") == 0 || 
                         strcmp(moduleName, "gps") == 0 ||
                         strcmp(moduleName, "fmradio") == 0 ||
                         strcmp(moduleName, "camera") == 0);
  
  bool isConnected = true;
  if (isSensorModule) {
    isConnected = isSensorConnected(moduleName);  // Now takes const char* directly
  }
  
  // Show module if connected or if showing all commands
  if (showAll || isConnected || !isSensorModule) {
    // Module header
    String upperName = String(moduleName);
    upperName.toUpperCase();
    
    if (isSensorModule) {
      BROADCAST_PRINTF("%s Commands%s:", 
                       upperName.c_str(),
                       isConnected ? " (Connected)" : " (Not Connected)");
    } else {
      BROADCAST_PRINTF("%s Commands:", upperName.c_str());
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

static const char* cmd_help(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = cmd;
  args.trim();

  if (args.length() == 0) {
    // Plain "help" command
    if (gCLIState == CLI_NORMAL) {
      // Enter help mode
      if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }
      gWebMirror.clear();
      gCLIState = CLI_HELP_MAIN;
      DEBUGF(DEBUG_CLI, "[cmd_help] Set gCLIState to CLI_HELP_MAIN (%d)", (int)gCLIState);
      gInHelpRender = true;
      broadcastOutput(renderHelpMain(false));
      gInHelpRender = false;
      return "OK";
    } else {
      // Already in help - re-render main
      gCLIState = CLI_HELP_MAIN;
      DEBUGF(DEBUG_CLI, "[cmd_help] Re-set gCLIState to CLI_HELP_MAIN (%d)", (int)gCLIState);
      gInHelpRender = true;
      broadcastOutput(renderHelpMain(false));
      gInHelpRender = false;
      return "OK";
    }
  }

  // Check if argument matches any module name for module-specific help
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  for (size_t i = 0; i < moduleCount; i++) {
    if (args.equals(modules[i].name)) {
      // Module-specific help
      gInHelpRender = true;
      broadcastOutput("\033[2J\033[H");
      broadcastOutput("════════════════════════════════════════════════════════════════");
      String upperName = String(modules[i].name);
      upperName.toUpperCase();
      BROADCAST_PRINTF("  %s Module Commands", upperName.c_str());
      broadcastOutput("════════════════════════════════════════════════════════════════");
      broadcastOutput("");
      
      renderModuleHelp(modules[i].name, modules[i].commands, modules[i].count, true);
      
      broadcastOutput("────────────────────────────────────────────────────────────────");
      broadcastOutput("Navigation:");
      BROADCAST_PRINTF("  • Type 'help %s' to refresh this module", modules[i].name);
      broadcastOutput("  • Type 'help sensors' to see all sensor modules");
      broadcastOutput("  • Type 'back' to return to help menu");
      broadcastOutput("  • Type 'exit' to return to CLI");
      broadcastOutput("────────────────────────────────────────────────────────────────");
      
      gCLIState = CLI_HELP_SENSORS;  // Use sensors state for module-specific help
      gInHelpRender = false;
      return "OK";
    }
  }

  // Parse traditional help subcommands
  if (args == "system") {
    gCLIState = CLI_HELP_SYSTEM;
    gInHelpRender = true;
    broadcastOutput(renderHelpSystem());
    gInHelpRender = false;
    return "OK";
  } else if (args == "wifi") {
    gCLIState = CLI_HELP_WIFI;
    gInHelpRender = true;
    broadcastOutput(renderHelpWifi());
    gInHelpRender = false;
    return "OK";
  } else if (args == "sensors") {
    gCLIState = CLI_HELP_SENSORS;
    gShowAllCommands = false;  // Reset to show connected sensors only
    gInHelpRender = true;
    broadcastOutput(renderHelpSensors());
    gInHelpRender = false;
    return "OK";
  } else if (args == "settings") {
    gCLIState = CLI_HELP_SETTINGS;
    gInHelpRender = true;
    broadcastOutput(renderHelpSettings());
    gInHelpRender = false;
    return "OK";
  } else if (args == "automations") {
    gCLIState = CLI_HELP_AUTOMATIONS;
    gInHelpRender = true;
    broadcastOutput(renderHelpAutomations());
    gInHelpRender = false;
    return "OK";
  } else if (args == "espnow") {
    gCLIState = CLI_HELP_ESPNOW;
    gInHelpRender = true;
    broadcastOutput(renderHelpEspnow());
    gInHelpRender = false;
    return "OK";
  } else if (args == "tail") {
    // Dump suppressed output tail
    gInHelpRender = true;
    helpSuppressedTailDump();
    gInHelpRender = false;
    return "OK";
  } else if (args == "all") {
    gShowAllCommands = true;
    gInHelpRender = true;
    broadcastOutput(renderHelpMain(true));
    gInHelpRender = false;
    return "OK";
  } else {
    // Build list of available modules for error message
    String moduleList = "";
    for (size_t i = 0; i < moduleCount; i++) {
      if (moduleList.length() > 0) moduleList += ", ";
      moduleList += modules[i].name;
    }
    gInHelpRender = true;
    broadcastOutput("Unknown help topic.");
    broadcastOutput("Available topics: system, wifi, sensors, settings, automations, espnow, all");
    BROADCAST_PRINTF("Available modules: %s", moduleList.c_str());
    gInHelpRender = false;
    return "ERROR";
  }
}

static const char* cmd_back(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gCLIState != CLI_NORMAL) {
    gCLIState = CLI_HELP_MAIN;
    broadcastOutput(renderHelpMain(gShowAllCommands));
    return "OK";
  }
  return "Not in help mode.";
}

static const char* cmd_exit(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gCLIState != CLI_NORMAL) {
    String banner = exitToNormalBanner();
    broadcastOutput(banner);
    helpSuppressedPrintAndReset();
    return "OK";
  }
  return "Already in normal CLI mode.";
}

static const char* cmd_clear(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }
  gWebMirror.clear();
  return "\033[2J\033[H"
         "CLI history cleared.";
}

// ============================================================================
// CLI Command Registry
// ============================================================================

const CommandEntry cliCommands[] = {
  { "help", "Display help menu (help [topic])", false, cmd_help },
  { "back", "Return to main help menu", false, cmd_back },
  { "exit", "Exit help mode", false, cmd_exit },
  { "clear", "Clear CLI history", false, cmd_clear },
};

const size_t cliCommandsCount = sizeof(cliCommands) / sizeof(cliCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _cli_cmd_registrar(cliCommands, cliCommandsCount, "cli");
