// System_CommandTypes.h - Shared command execution types
// Eliminates duplication of CommandOrigin, CommandContext, ExecReq
// across HardwareOne.cpp, System_Utils.cpp, System_ESPNow.cpp, etc.
#ifndef SYSTEM_COMMANDTYPES_H
#define SYSTEM_COMMANDTYPES_H

#include <Arduino.h>
#include <atomic>
#include "System_User.h"    // AuthContext
#include "System_CommandLimits.h"

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
  ORIGIN_VOICE,        // command issued by on-device voice recognition (SOURCE_VOICE),
                       // armed-user identity. Also routed through cmd_exec_task.
  ORIGIN_UART          // command received on the UART host link (SOURCE_UART) —
                       // the board-to-board channel a Linux host (CM5 carrier)
                       // drives. Appended at the END of this enum deliberately:
                       // the zero value stays ORIGIN_SERIAL (see the
                       // CommandContext note below), and ORIGIN_UART must NEVER
                       // be treated as physical presence — the OTA pin/journal
                       // commands key on ORIGIN_SERIAL exactly so a machine on
                       // the UART link cannot reach them. Results are written
                       // by the UART drain itself (System_UartLink.cpp), not
                       // by deliverCommandResult.
};

// Per-command output routing uses the MSG_ROUTE_* sink bits directly
// (System_Debug.h) — outputMask below holds them verbatim and broadcastOutput
// passes them through as the message route. (The former CMD_OUT_* enum was an
// identical-by-contract copy of those bits; unified 2026-07.)

// Full execution context for a command
enum CommandContextBehavior : uint8_t {
  COMMAND_CONTEXT_DEFAULT = 0,
  // Machine-protocol traffic must remain in the normal registry/executor
  // pipeline, but it must never be interpreted as an answer to a human CLI
  // prompt or open an interactive mode of its own.
  COMMAND_CONTEXT_MODE_INDEPENDENT = 1u << 0,
  // The producer authenticated a stateful transport session and therefore
  // requires an exact, still-live generation at execution time. This bit
  // distinguishes a failed epoch capture (which must fail closed) from an
  // intentionally stateless caller such as HTTP Basic Auth or firmware-
  // internal work.
  COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1u << 1,
  // A firmware-internal G2 EvenAI command is authorized by one exact active
  // exchange and one exact named UART login. The typed fields below prevent a
  // queued open/start operation from surviving logout/re-login and attaching
  // itself to a replacement host session.
  COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY = 1u << 2,
};

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
  uint32_t id = 0;
  uint32_t timestampMs = 0;
  uint32_t outputMask = 0;
  // Boot-local value shared by every member of one settings batch request.
  // Zero means this command is not part of a request-scoped batch. This is a
  // value (never an HTTP/request pointer) because CommandContext is copied into
  // queued ExecReq objects and may outlive the submitting task on timeout.
  uint32_t settingsBatchId = 0;
  // Boot-local incarnation of the transport session that admitted this
  // command. This is an incrementing generation, not Unix/NTP time. Zero
  // means the invocation is intentionally stateless/unbound.
  uint32_t transportSessionEpoch = 0;
  uint32_t authoritySessionEpoch = 0;
  uint64_t authorityId = 0;
  uint8_t behaviorFlags = COMMAND_CONTEXT_DEFAULT;
  bool validateOnly = false;
  bool captureOutput = false;  // capture broadcastOutput into HTTP response
  void* replyHandle = nullptr;     // placeholder for future sync replies
  httpd_req_t* httpReq = nullptr;  // used by web origin if needed
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

// Async callback type for fire-and-forget command execution
// Called on cmd_exec task with result - caller must NOT block
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);

// Execution request - queued to the cmd_exec task
struct ExecReq {
  char line[CMD_INPUT_MAX + 1];  // Complete command plus trailing NUL
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

  // Synchronous-request lifetime. Before queue publication, submitSync gives
  // one reference to the caller and one to cmd_exec_task. Each side releases
  // exactly once; the last release deletes `done` and destroys/frees this
  // request. This keeps both objects alive across the timeout-vs-completion
  // boundary without a check-then-act ownership handoff.
  //
  // These atomics may live in PSRAM. The current ESP32/ESP32-S3 IDF configs
  // enable CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND, which implements them
  // under an internal-RAM portMUX when their address is external.
  std::atomic<uint32_t> syncOwnerRefs{0};
  std::atomic<bool> syncTimedOut{false};  // diagnostic only; not ownership
};

/*
 * What actually travels on gCmdExecQ.
 *
 * The queue used to carry a bare `ExecReq*`, which meant deferred work had to
 * allocate a whole ExecReq (6,384 B, measured) to carry two pointers -- and a
 * deferred job uses ONLY deferredFn/deferredArg; line[2048], ctx and out[4096]
 * are dead weight on that path. At ~8,700 BLE OTA frames per 4 MB image that is
 * ~55 MB of pointless allocator traffic per update, and it forced the queue to
 * stay shallow (8) because each in-flight slot cost 6.4 KB.
 *
 * Carrying this 12-byte tagged item by value instead means a deferred job
 * allocates NOTHING, so queue depth costs 12 B/slot rather than 6.4 KB/slot.
 * Exactly one of `req` / `deferredFn` is set.
 */
struct CmdExecItem {
  ExecReq*             req = nullptr;          // full CLI command (owns its buffers)
  ExecReq::DeferredFn  deferredFn = nullptr;   // deferred job; `req` is null
  void*                deferredArg = nullptr;  // callee owns this arg's lifetime
};

// Drop one synchronous-request owner. Safe from either the submitting task or
// cmd_exec_task; the last owner performs all request/semaphore destruction.
void releaseSyncExecReqOwner(ExecReq* request);

#endif // SYSTEM_COMMANDTYPES_H
