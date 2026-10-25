"""Shared pieces for the host suites that splice Bluetooth.cpp's phone-link
console row (phone link v2) into their harnesses.

The console is stream slot 2 of the phone pipe. Bluetooth.cpp keeps its
session as a gBLEState row with its own id and writes its output through the
console driver (BLE_PhonePipeConsole.h), which other suites test. This module
extracts, as shipped:

- pipe_types(): the driver's kinds, modes, results and target, and Bluetooth.h's
  BlePipeConsoleIdentity / blePipeConsoleRowId;
- holder_state(): gBLEState's holder bits and the server state's mirror;
- slot_state(): the row's slot, its leaf snapshot and blePipeIdentitySyncLocked
  (what the epoch helpers call);
- row_helpers(): blePipeRow, bleIsPipeRowId, the close reasons and the
  forward declaration of blePipeRowEndLocked;
- row_definitions() / router_definitions(): the row lifecycle (holders, bind,
  end, teardown, reconcile) and the output router (TEXT, BLOB, CARD, MIRROR);
- declarations(): forward declarations for all of those, so a harness can put
  the definitions after the functions that call them;

and supplies PIPE_DOUBLES, recording doubles of the driver's writer and close
mailbox and of the owner's lock probe. A harness defines
PIPE_HOST_LIFECYCLE_HELD() (whether its lifecycle-lock double is held) before
the doubles, so every call into the pipe records whether the lifecycle lock was
held (the lock-order rule: never, except the close request).
"""
from __future__ import annotations

from pathlib import Path
import re

from test_web_batch_handlers import extract_block, require


def _found(pattern: str, text: str, what: str) -> str:
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    require(match is not None, what)
    return match[0]


def pipe_types(component: Path) -> str:
    console = (component / "BLE_PhonePipeConsole.h").read_text()
    header = (component / "Bluetooth.h").read_text()
    parts = [_found(rf"^enum class {name} : uint8_t \{{[^}}]*\}};", console,
                    f"BLE_PhonePipeConsole.h declares {name}")
             for name in ("PipeConsoleKind", "PipeConsoleMode", "PipeConsoleWrite")]
    parts.append(_found(r"^struct PipeConsoleTarget \{[^}]*\};", console,
                        "BLE_PhonePipeConsole.h declares PipeConsoleTarget"))
    parts.append(extract_block(header, "struct BlePipeConsoleIdentity") + ";")
    parts.append(extract_block(header, "inline constexpr uint16_t blePipeConsoleRowId("))
    return "\n\n".join(parts)


def link_record(component: Path) -> str:
    """BLE_PhonePipe.h's PhonePipeLinkRecord (the console row's reconcile reads it)."""
    return _found(r"^struct PhonePipeLinkRecord \{[^}]*\};", (component / "BLE_PhonePipe.h").read_text(),
                  "BLE_PhonePipe.h declares PhonePipeLinkRecord")


def holder_state(owner: str) -> str:
    return "\n".join((
        _found(r"^constexpr uint8_t kBleHoldServer = [^;]+;", owner, "Bluetooth.cpp names the holders"),
        _found(r"^static uint8_t sBleStateHolders = 0;", owner, "Bluetooth.cpp keeps the holder bits"),
        # isBleServerInitialized()'s mirror, which retiring the state clears.
        _found(r"^static std::atomic<bool> sBleServerStateInitialized\{false\};", owner,
               "Bluetooth.cpp mirrors the server state's initialized flag"),
        # A harness may splice only some of the holders' users.
        "[[maybe_unused]] static const uint8_t kPipeHostHolders[] = "
        "{kBleHoldServer, kBleHoldClientConsole, kBleHoldPipe};",
    ))


def slot_state(owner: str) -> str:
    return "\n".join((
        _found(r"^int sBlePipeRowSlot = -1;", owner, "Bluetooth.cpp keeps the console row's slot"),
        _found(r"^portMUX_TYPE sBlePipeIdentityMux = portMUX_INITIALIZER_UNLOCKED;", owner,
               "Bluetooth.cpp has the identity snapshot's leaf lock"),
        _found(r"^BlePipeConsoleIdentity sBlePipeIdentity;", owner,
               "Bluetooth.cpp keeps the identity snapshot"),
        extract_block(owner, "void blePipeIdentitySyncLocked() "),
    ))


ROW_END_DECL = r"^static bool blePipeRowEndLocked\(bool match = false,[^;]*;"


def row_helpers(owner: str) -> str:
    return "\n\n".join((
        extract_block(owner, "static inline bool blePipeRow("),
        extract_block(owner, "static bool bleIsPipeRowId("),
        _found(ROW_END_DECL, owner, "Bluetooth.cpp declares blePipeRowEndLocked with its defaults"),
        _found(r"^constexpr uint16_t kBlePipeCloseRenewed = [^;]+;", owner,
               "Bluetooth.cpp names the console CLOSE reasons"),
        # A harness may splice only the callers of one of them.
        "[[maybe_unused]] static const uint16_t kPipeHostCloseReasons[] = "
        "{kBlePipeCloseRenewed, kBlePipeCloseRevoked};",
    ))


ROW_MARKERS = (
    "static void bleStateRetireLocked(",
    "static void bleStateReleaseLocked(",
    "static bool bleStateHeldByHostRole(",
    "static bool blePipeRowEndLocked(bool match, uint32_t",
    "static void blePipeHostTeardown(",
    "static void blePipeConsoleReconcile(",
    "bool blePipeConsoleStateAcquire(",
    "void blePipeConsoleStateRelease(",
    "bool blePipeConsoleBind(",
    "void blePipeConsoleEnd(",
    "void blePipeConsoleIdentity(",
)

ROUTER_MARKERS = (
    "static bool blePipeWrite(",
    "static bool bleSendToSession(",
    "bool bleSendBinaryToSession(",
    "bool bleSessionConfidential(",
)


def row_definitions(owner: str) -> list[str]:
    return [extract_block(owner, marker) for marker in ROW_MARKERS]


def router_definitions(owner: str) -> list[str]:
    return [_found(r"^constexpr size_t kBlePipeMirrorMax = \d+;", owner,
                   "Bluetooth.cpp bounds a MIRROR record")] + \
        [extract_block(owner, marker) for marker in ROUTER_MARKERS]


def _declaration(definition: str) -> str:
    return definition[:definition.index("{")].rstrip() + ";"


def declarations(owner: str, markers: tuple[str, ...]) -> str:
    """Forward declarations of the given definitions (never blePipeRowEndLocked's,
    which row_helpers() supplies with its default arguments)."""
    return "\n".join(_declaration(extract_block(owner, marker)) for marker in markers
                     if not marker.startswith("static bool blePipeRowEndLocked("))


PIPE_DOUBLES = r'''
// ---- the console driver (BLE_PhonePipeConsole.h) and the owner's lock probe ----
#ifndef PIPE_HOST_LIFECYCLE_HELD
#define PIPE_HOST_LIFECYCLE_HELD() false
#endif
struct PipeHostCall {
  char op = 0;              // 'W' a record write, 'C' a close request
  uint32_t generation = 0;
  uint16_t streamId = 0;
  uint32_t epoch = 0;
  PipeConsoleKind kind = PipeConsoleKind::Text;
  PipeConsoleMode mode = PipeConsoleMode::TryNow;
  std::string data;
  uint16_t reason = 0;
  bool sent = false;
  bool underLifecycle = false;  // the BLE lifecycle lock was held
};
struct PipeHost {
  std::vector<PipeHostCall> calls;
  int room = -1;  // writes the stream still takes (-1: any)
  PipeConsoleWrite full = PipeConsoleWrite::Dropped;
  bool chainHeld = false, ownerHeld = false, onCmdExec = true;
};
static PipeHost gPipeHost;
static PipeConsoleWrite pipeConsoleWrite(const PipeConsoleTarget& target, uint32_t epoch,
                                         PipeConsoleKind kind, const uint8_t* data, size_t len,
                                         PipeConsoleMode mode) {
  PipeHostCall call;
  call.op = 'W';
  call.generation = target.generation;
  call.streamId = target.streamId;
  call.epoch = epoch;
  call.kind = kind;
  call.mode = mode;
  call.data.assign(reinterpret_cast<const char*>(data), len);
  call.underLifecycle = PIPE_HOST_LIFECYCLE_HELD();
  call.sent = gPipeHost.room != 0;
  if (gPipeHost.room > 0) --gPipeHost.room;
  gPipeHost.calls.push_back(call);
  return call.sent ? PipeConsoleWrite::Sent : gPipeHost.full;
}
static void pipeConsoleRequestClose(uint32_t generation, uint16_t streamId, uint16_t reason) {
  PipeHostCall call;
  call.op = 'C';
  call.generation = generation;
  call.streamId = streamId;
  call.reason = reason;
  call.underLifecycle = PIPE_HOST_LIFECYCLE_HELD();
  gPipeHost.calls.push_back(call);
}
static bool pipeConsoleHeldByCurrentTask() { return gPipeHost.chainHeld; }
static bool phonePipeOwnerHeldByCurrentTask() { return gPipeHost.ownerHeld; }
// Bluetooth.cpp's cmd_exec probe (gCmdExecTaskHandle is HardwareOne.cpp's).
static bool bleOnCmdExecTask() { return gPipeHost.onCmdExec; }
static std::vector<PipeHostCall> pipeHostWrites() {
  std::vector<PipeHostCall> out;
  for (const PipeHostCall& call : gPipeHost.calls) if (call.op == 'W') out.push_back(call);
  return out;
}
static std::vector<PipeHostCall> pipeHostCloses() {
  std::vector<PipeHostCall> out;
  for (const PipeHostCall& call : gPipeHost.calls) if (call.op == 'C') out.push_back(call);
  return out;
}
'''


def router_block(component: Path, source: str, lifecycle_held: str) -> str:
    """For harnesses that splice the session senders into slots: the pipe types,
    the driver doubles (lifecycle_held: the harness's lock-held expression) and
    the router the senders call. Empty for a tree without the router."""
    if "static bool bleSendToSession(" not in source:
        return ""
    return "\n\n".join((
        "#ifndef ENABLE_BLE_PHONE_PIPE\n#define ENABLE_BLE_PHONE_PIPE 1\n#endif",
        pipe_types(component),
        f"#define PIPE_HOST_LIFECYCLE_HELD() ({lifecycle_held})",
        PIPE_DOUBLES,
        extract_block(source, "static inline bool blePipeRow("),
        *router_definitions(source)[:3],  # the MIRROR bound, blePipeWrite, bleSendToSession
    ))
