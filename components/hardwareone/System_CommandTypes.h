// System_CommandTypes.h - Shared command execution types
// Eliminates duplication of CommandOrigin, CommandContext, ExecReq
// across HardwareOne.cpp, System_Utils.cpp, System_ESPNow.cpp, etc.
#ifndef SYSTEM_COMMAND_TYPES_H
#define SYSTEM_COMMAND_TYPES_H

#include <Arduino.h>
#include "System_User.h"    // AuthContext

// Forward declare httpd_req_t to avoid pulling in full HTTP server headers
#ifndef HW_HTTPD_TYPES_DEFINED
  #define HW_HTTPD_TYPES_DEFINED 1
  struct httpd_req;
  typedef struct httpd_req httpd_req_t;
  typedef void* httpd_handle_t;
#endif

// Command origin - where a command was initiated from
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_SYSTEM };

// Output routing mask (avoid name collision with device OUTPUT_* macros)
enum CmdOutputMask { CMD_OUT_SERIAL = 1 << 0,
                     CMD_OUT_WEB = 1 << 1,
                     CMD_OUT_LOG = 1 << 2,
                     CMD_OUT_BROADCAST = 1 << 3 };

// Full execution context for a command
struct CommandContext {
  CommandOrigin origin;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint32_t outputMask;
  bool validateOnly;
  void* replyHandle;     // placeholder for future sync replies
  httpd_req_t* httpReq;  // used by web origin if needed
};

// Simple wrapper: command line + context
struct Command {
  String line;
  CommandContext ctx;
};

// Async callback type for fire-and-forget command execution
// Called on cmd_exec task with result - caller must NOT block
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);

// Execution request - queued to the cmd_exec task
struct ExecReq {
  char line[2048];         // Command string (full size for ESP-NOW chunking)
  CommandContext ctx;      // Full execution context
  char out[2048];          // Result buffer (2KB)
  SemaphoreHandle_t done;  // Signals completion (NULL for async mode)
  bool ok;                 // Success flag from executeCommand()

  // Async callback mode (alternative to semaphore)
  ExecAsyncCallback asyncCallback;  // If non-NULL, called instead of semaphore
  void* asyncUserData;              // Passed to callback
};

#endif // SYSTEM_COMMAND_TYPES_H
