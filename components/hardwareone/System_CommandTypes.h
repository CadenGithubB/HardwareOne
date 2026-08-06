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
  ORIGIN_G2_HIJACK,   // G2 glasses hijack UI — taps that mutate system state
                      // are routed through cmd_exec_task via g2SubmitHijackCommand()
                      // (see G2_HijackCmd.h). Distinguished from ORIGIN_BLUETOOTH
                      // because G2 hijack has no logged-in user and a different
                      // audit path (/g2/hijack/...) than the BLE CLI characteristic.
  ORIGIN_ESPNOW,      // remote command received from another node over ESP-NOW
                      // (transport SOURCE_ESPNOW). Runs as the authenticated remote
                      // user (ctx.auth.user), NOT system — origin is audit-only.
                      // Split from ORIGIN_SYSTEM so remote command execution is
                      // attributable in the audit trail.
  ORIGIN_LOCAL_DISPLAY, // command issued from the on-device OLED + gamepad UI
                       // (transport SOURCE_LOCAL_DISPLAY). Split from ORIGIN_SYSTEM
                       // for the same audit-attribution reason.
  ORIGIN_MQTT,         // command received over MQTT (transport SOURCE_MQTT), run as the
                       // authenticated MQTT user. Routed through cmd_exec_task via
                       // submitAndExecuteSync so it serializes with all other commands
                       // instead of racing them on the esp-mqtt event task.
  ORIGIN_VOICE         // command issued by on-device voice recognition (SOURCE_VOICE),
                       // armed-user identity. Also routed through cmd_exec_task.
};

// Per-command output routing uses the MSG_ROUTE_* sink bits directly
// (System_Debug.h) — outputMask below holds them verbatim and broadcastOutput
// passes them through as the message route. (The former CMD_OUT_* enum was an
// identical-by-contract copy of those bits; unified 2026-07.)

// Full execution context for a command
struct CommandContext {
  // Defaulted deliberately, and deliberately NOT to ORIGIN_SERIAL.
  //
  // ORIGIN_SERIAL is the first enumerator, so its value is 0 - which means a
  // context that was zero-filled or value-initialized and never assigned an
  // origin would read as "typed at the physical console". Two commands treat
  // that as proof of physical presence and allow otherwise-forbidden work
  // (cmdOtaPin and cmdOtaResetJournal in System_OTA.cpp), so the zero value is
  // the permissive one and the fail direction was open.
  //
  // Every transport assigns this explicitly today, so this changes no current
  // behaviour; it only decides which way a future omission fails. ORIGIN_WEB is
  // the least-privileged choice that is still a real transport: it carries no
  // implicit trust and is subject to the ordinary auth checks.
  CommandOrigin origin = ORIGIN_WEB;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint32_t outputMask;
  bool validateOnly;
  bool captureOutput = false;  // capture broadcastOutput into HTTP response
  void* replyHandle;     // placeholder for future sync replies
  httpd_req_t* httpReq;  // used by web origin if needed
  // Non-empty when the command is an automation sub-command. Used by
  // executeCommand to write COMMAND/OUTPUT lines to the autolog, attributed
  // to the named automation. Stamped at queue time in queueAutomationSubCommand
  // so there's no race between the scheduler advancing to the next automation
  // and cmd_exec_task actually running the command.
  char automationName[64] = {};
};

// Simple wrapper: command line + context
struct Command {
  String line;
  CommandContext ctx;
};

// ---------------------------------------------------------------------------
// Command result ceiling — the OUTBOUND contract
// ---------------------------------------------------------------------------
// The largest result a command handler may return. This is the delivery-side
// twin of broadcastOutput()'s 255 B per-line rule (System_Debug.h DEBUG_MSG_SIZE):
// there, a caller proves its line fits and an over-long one is marked [CUT];
// here, a handler proves its result fits and an over-long one becomes an
// explicit error at the executeCommand chokepoint (System_Utils.cpp).
//
// Transports size their own `out` buffer and may declare LESS than this — the
// effective limit is min(CMD_RESULT_MAX, that buffer). What is uniform is not
// the number but the BEHAVIOUR: the limit is always knowable and exceeding it
// always fails loudly. Known capacities:
//   ExecReq::out (web, BLE, ESP-NOW, serial)  CMD_RESULT_MAX
//   MQTT (System_MQTT.cpp), voice (System_ESPSR.cpp)  CMD_RESULT_MAX
//   help-exit (HardwareOne.cpp)  2048 — a STACK array; deliberately not raised
//
// A handler that cannot fit its document belongs behind a dedicated endpoint
// (see /api/devices -> buildDeviceRegistryJson) rather than the CLI channel.
// This is NOT an inbound limit: ESP-NOW reassembly is sized from the wire
// budget instead, because a remote peer's output is not ours to bound.
#define CMD_RESULT_MAX 4096

// Async callback type for fire-and-forget command execution
// Called on cmd_exec task with result - caller must NOT block
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);

// Execution request - queued to the cmd_exec task
struct ExecReq {
  char line[2048];         // Command string (full size for ESP-NOW chunking)
  CommandContext ctx;      // Full execution context
  char out[CMD_RESULT_MAX];  // Result buffer — the reference capacity
  SemaphoreHandle_t done;  // Signals completion (NULL for async mode)
  bool ok;                 // Success flag from executeCommand()

  // Async callback mode (alternative to semaphore)
  ExecAsyncCallback asyncCallback;  // If non-NULL, called instead of semaphore
  void* asyncUserData;              // Passed to callback

  // Deferred-work mode — used by espnow_task to push heavy crypto onto
  // cmd_exec_task's deeper stack. When deferredFn is set, commandExecTask
  // bypasses the CLI executeCommand() path entirely and just invokes
  // deferredFn(deferredArg). The deferred fn is responsible for freeing
  // its own arg. line/ctx/out/done/asyncCallback are all ignored.
  //
  // Why this instead of a separate task: cmd_exec_task is single-threaded,
  // so the new HWM is max(existing CLI peak, deferred crypto peak) — and
  // crypto peak (~5 KB) is far below the existing CLI peak (~17 KB), so
  // total stack budget stays unchanged. Adds zero new task overhead.
  typedef void (*DeferredFn)(void* arg);
  DeferredFn deferredFn;            // If non-NULL, called instead of executeCommand
  void*      deferredArg;

  // 2026-05-18 — Use-after-free fix.
  // submitSync used to free `r` when its 10s wait timed out. cmd_exec_task,
  // which may still be inside executeCommand on a long-running command
  // (PBKDF2 ~12 s, or a future even-slower op), would then double-free OR
  // ps_alloc would hand the same address back to the next submit, racing the
  // old cmd_exec_task against new ExecReq data. Caused String::buffer
  // corruption that surfaced as strlen(NULL) inside ArduinoJson.
  //
  // Flag is set by the caller (submitSync) when it gives up. cmd_exec_task
  // checks it after executeCommand returns; if true, cmd_exec_task owns
  // the cleanup. If false, the caller still owns it (current behaviour).
  // `volatile` is enough — the only mutation is a single-byte write under
  // observation, no rmw, no atomicity needed.
  volatile bool abandoned;
};

#endif // SYSTEM_COMMANDTYPES_H
