// System_CommandTypes.h - Shared command execution types
// Eliminates duplication of CommandOrigin, CommandContext, ExecReq
// across HardwareOne.cpp, System_Utils.cpp, System_ESPNow.cpp, etc.
#ifndef SYSTEM_COMMANDTYPES_H
#define SYSTEM_COMMANDTYPES_H

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
enum CommandOrigin {
  ORIGIN_SERIAL,
  ORIGIN_WEB,
  ORIGIN_AUTOMATION,
  ORIGIN_SYSTEM,
  ORIGIN_BLUETOOTH,
  ORIGIN_G2_HIJACK    // G2 glasses hijack UI — taps that mutate system state
                      // are routed through cmd_exec_task via g2SubmitHijackCommand()
                      // (see G2_HijackCmd.h). Distinguished from ORIGIN_BLUETOOTH
                      // because G2 hijack has no logged-in user and a different
                      // audit path (/g2/hijack/...) than the BLE CLI characteristic.
};

// Per-command output routing mask.
// Bit values are aligned with MSG_ROUTE_* (System_Debug.h) by design,
// so the mapping from CMD_OUT_* to MSG_ROUTE_* is a direct passthrough.
enum CmdOutputMask { CMD_OUT_SERIAL = 1 << 0,   // 0x01 = MSG_ROUTE_SERIAL
                     CMD_OUT_WEB    = 1 << 1,    // 0x02 = MSG_ROUTE_WEB
                     CMD_OUT_LOG    = 1 << 2,    // 0x04 = MSG_ROUTE_FILE
                     CMD_OUT_BLE    = 1 << 4 };  // 0x10 = MSG_ROUTE_BLE

// Full execution context for a command
struct CommandContext {
  CommandOrigin origin;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint32_t outputMask;
  bool validateOnly;
  bool captureOutput = false;  // capture broadcastOutput into HTTP response
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
  char out[4096];          // Result buffer (4KB)
  SemaphoreHandle_t done;  // Signals completion (NULL for async mode)
  bool ok;                 // Success flag from executeCommand()

  // Async callback mode (alternative to semaphore)
  ExecAsyncCallback asyncCallback;  // If non-NULL, called instead of semaphore
  void* asyncUserData;              // Passed to callback
};

#endif // SYSTEM_COMMANDTYPES_H
