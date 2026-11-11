// Feature-neutral command metadata and registry queries. A command table needs
// no transport, sensor, execution context, or Arduino implementation details.
#ifndef SYSTEM_COMMANDREGISTRY_H
#define SYSTEM_COMMANDREGISTRY_H

#include <stddef.h>
#include <stdint.h>

class String;

// ============================================================================
// Command Registry System
// ============================================================================

// Command entry structure - used by all modules to define their commands.
// Voice phrases for ESP-SR do NOT live here: they live in the SR-owned route
// table (kVoiceRoutes in System_ESPSR.cpp), keyed by command name and filtered
// against the live registry at grammar-build time. Adding a voice alias for a
// command is one row there; this struct stays feature-neutral.

struct CommandEntry {
  const char* name;                           // canonical command name
  const char* help;                           // short help text
  bool requiresAdmin;                         // whether admin is required
  const char* (*handler)(const String& argsInput);  // function pointer to command handler
  const char* usage;                          // optional longer usage string (may be nullptr)
  bool requiresSuperAdmin;                     // top tier: ordinary admin is not enough (see authorizeCommand)

  // Trailing + defaulted so the ~hundreds of existing entries are untouched;
  // only the handful of super-admin commands pass the final `true`. It must be
  // LAST: a stray string in this position would bind pointer→bool and silently
  // mark the command super — the deleted overload below makes that a compile
  // error instead.
  constexpr CommandEntry(const char* name_,
                         const char* help_,
                         bool requiresAdmin_,
                         const char* (*handler_)(const String& cmd_),
                         const char* usage_ = nullptr,
                         bool requiresSuperAdmin_ = false)
      : name(name_),
        help(help_),
        requiresAdmin(requiresAdmin_),
        handler(handler_),
        usage(usage_),
        requiresSuperAdmin(requiresSuperAdmin_) {}

  // Guard: a const char* in the super-admin slot (e.g. a voice phrase left
  // over from the pre-route-table era) must fail loudly, not convert to bool.
  constexpr CommandEntry(const char*, const char*, bool,
                         const char* (*)(const String&),
                         const char*, const char*) = delete;
};

// Command module flags
#define CMD_MODULE_SENSOR    0x01  // Module controls a sensor/peripheral
#define CMD_MODULE_CORE      0x02  // Core system module (skip in help menu)

// Command module structure - each module registers its command table
struct CommandModule {
  const char* name;                           // module name (for help categories)
  const char* description;                    // short one-line blurb for the `help` module list (can be nullptr)
  const char* long_description;               // full subsystem overview shown atop `help <module>` (can be nullptr)
  const CommandEntry* commands;               // pointer to command array
  const size_t* countPtr;                     // address of immutable command-array count
  uint8_t flags;                              // module flags (CMD_MODULE_*)
  bool (*isConnected)();                      // optional connection check (can be nullptr)

  size_t commandCount() const {
    return countPtr ? *countPtr : 0;
  }
};

// Command tables. Each module defines its own table and count; all of them are
// declared here and nowhere else. A file that defines a table must include
// this header: a `const` object at namespace scope has internal linkage unless
// an extern declaration comes first. A table whose module is compiled out
// needs no definition, because only compiled catalog rows refer to it.
extern const CommandEntry cliCommands[];
extern const size_t cliCommandsCount;
extern const CommandEntry commands[];
extern const size_t commandsCount;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;
extern const CommandEntry espNowCommands[];
extern const size_t espNowCommandsCount;
extern const CommandEntry companionCommands[];
extern const size_t companionCommandsCount;
extern const CommandEntry mqttCommands[];
extern const size_t mqttCommandsCount;
extern const CommandEntry bluetoothCommands[];
extern const size_t bluetoothCommandsCount;
extern const CommandEntry phoneCommands[];
extern const size_t phoneCommandsCount;
extern const CommandEntry relayCommands[];
extern const size_t relayCommandsCount;
extern const CommandEntry filesystemCommands[];
extern const size_t filesystemCommandsCount;
extern const CommandEntry sdCommands[];
extern const size_t sdCommandsCount;
extern const CommandEntry oledCommands[];
extern const size_t oledCommandsCount;
extern const CommandEntry speakerCommands[];
extern const size_t speakerCommandsCount;
extern const CommandEntry neopixelCommands[];
extern const size_t neopixelCommandsCount;
extern const CommandEntry ledCommands[];
extern const size_t ledCommandsCount;
extern const CommandEntry matrixCommands[];
extern const size_t matrixCommandsCount;
extern const CommandEntry servoCommands[];
extern const size_t servoCommandsCount;
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;
extern const CommandEntry imuCommands[];
extern const size_t imuCommandsCount;
extern const CommandEntry inputCommands[];
extern const size_t inputCommandsCount;
extern const CommandEntry gamepadCommands[];
extern const size_t gamepadCommandsCount;
extern const CommandEntry anoEncoderCommands[];
extern const size_t anoEncoderCommandsCount;
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;
extern const CommandEntry rtcCommands[];
extern const size_t rtcCommandsCount;
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;
extern const CommandEntry cameraCommands[];
extern const size_t cameraCommandsCount;
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;
extern const CommandEntry edgeImpulseCommands[];
extern const size_t edgeImpulseCommandsCount;
extern const CommandEntry transcriptionUiCommands[];
extern const size_t transcriptionUiCommandsCount;
extern const CommandEntry noteCommands[];
extern const size_t noteCommandsCount;
extern const CommandEntry sttCommands[];
extern const size_t sttCommandsCount;
extern const CommandEntry espsrCommands[];
extern const size_t espsrCommandsCount;
extern const CommandEntry i2cCommands[];
extern const size_t i2cCommandsCount;
extern const CommandEntry automationCommands[];
extern const size_t automationCommandsCount;
extern const CommandEntry batteryCommands[];
extern const size_t batteryCommandsCount;
extern const CommandEntry debugCommands[];
extern const size_t debugCommandsCount;
extern const CommandEntry settingsCommands[];
extern const size_t settingsCommandsCount;
extern const CommandEntry sensorLoggingCommands[];
extern const size_t sensorLoggingCommandsCount;
extern const CommandEntry healthCommands[];
extern const size_t healthCommandsCount;
extern const CommandEntry userSystemCommands[];
extern const size_t userSystemCommandsCount;
extern const CommandEntry featureCommands[];
extern const size_t featureCommandsCount;
extern const CommandEntry imageCommands[];
extern const size_t imageCommandsCount;
extern const CommandEntry mapCommands[];
extern const size_t mapCommandsCount;
extern const CommandEntry mapsSettingCommands[];
extern const size_t mapsSettingCommandsCount;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;
extern const CommandEntry liveAudioCommands[];
extern const size_t liveAudioCommandsCount;
extern const CommandEntry cm5PresenceCommands[];
extern const size_t cm5PresenceCommandsCount;
extern const CommandEntry otaCommands[];
extern const size_t otaCommandsCount;
extern const CommandEntry versionsCommands[];
extern const size_t versionsCommandsCount;
extern const CommandEntry setPatternCommands[];
extern const size_t setPatternCommandsCount;
extern const CommandEntry g2Commands[];
extern const size_t g2CommandsCount;
extern const CommandEntry g2RingCommands[];
extern const size_t g2RingCommandsCount;
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;
extern const CommandEntry settingEditorCommands[];
extern const size_t settingEditorCommandsCount;

// Get the compiled module catalog (defined in System_CommandRegistry.cpp).
// Returns pointer to array, sets count via output parameter
const CommandModule* getCommandModules(size_t& count);

// Find a command entry by name (searches all modules, returns longest match)
// Returns nullptr if not found
const CommandEntry* findCommand(const String& cmdLine);


// Opt-in structured (JSON) output detector. Returns true if `args` contains a
// standalone `json` token at a word boundary (so it won't false-match a value
// that merely contains the substring). Per the output contract (see
// executeCommand in System_Utils.cpp), a command in JSON mode emits its payload
// ONLY via the return value (one verbatim document) and calls NO broadcastOutput.
bool argWantsJson(const String& args);

// argLeadingTokenIsJson() — true iff the FIRST space-delimited token is exactly
// "json". The positional counterpart to argWantsJson(): use it where "json" must
// lead because later tokens are real args that may contain a "json" token
// (e.g. `llm json <prompt>`, `files json <path>`, `files /logs json`).
bool argLeadingTokenIsJson(const String& args);

#endif // SYSTEM_COMMANDREGISTRY_H
