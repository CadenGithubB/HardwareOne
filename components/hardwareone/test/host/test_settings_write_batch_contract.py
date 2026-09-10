#!/usr/bin/env python3
"""Source-contract guards for owner-scoped settings write batching.

These checks intentionally sit above the dependency-free batch-core unit test.
The core proves the state machine; this file proves that production mutation,
transport, and persistence adapters have not bypassed that state machine.
"""

from __future__ import annotations

import re
from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def brace_block(source: str, opening_brace: int) -> str:
    """Return a balanced C/C++ brace block, ignoring comments and literals."""

    depth = 0
    pos = opening_brace
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""

        if state == "line_comment":
            if current == "\n":
                state = "code"
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                pos += 1
        elif state == "string":
            if current == "\\":
                pos += 1
            elif current == '"':
                state = "code"
        elif state == "character":
            if current == "\\":
                pos += 1
            elif current == "'":
                state = "code"
        else:
            if current == "/" and following == "/":
                state = "line_comment"
                pos += 1
            elif current == "/" and following == "*":
                state = "block_comment"
                pos += 1
            elif current == '"':
                state = "string"
            elif current == "'":
                state = "character"
            elif current == "{":
                depth += 1
            elif current == "}":
                depth -= 1
                if depth == 0:
                    return source[opening_brace : pos + 1]
        pos += 1
    raise AssertionError("unterminated C/C++ brace block")


def cpp_code(source: str) -> str:
    """Blank comments/literals while preserving offsets and newlines."""

    output: list[str] = []
    pos = 0
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""
        if state == "code":
            if current == "/" and following == "/":
                output.extend((" ", " "))
                pos += 2
                state = "line_comment"
                continue
            if current == "/" and following == "*":
                output.extend((" ", " "))
                pos += 2
                state = "block_comment"
                continue
            if current == '"':
                output.append(" ")
                pos += 1
                state = "string"
                continue
            if current == "'":
                output.append(" ")
                pos += 1
                state = "character"
                continue
            output.append(current)
            pos += 1
            continue

        if state == "line_comment":
            output.append("\n" if current == "\n" else " ")
            pos += 1
            if current == "\n":
                state = "code"
            continue

        if state == "block_comment":
            if current == "*" and following == "/":
                output.extend((" ", " "))
                pos += 2
                state = "code"
            else:
                output.append("\n" if current == "\n" else " ")
                pos += 1
            continue

        # String/character literal. Blank escaped pairs together so an escaped
        # quote cannot terminate the state early.
        if current == "\\" and following:
            output.extend((" ", "\n" if following == "\n" else " "))
            pos += 2
        else:
            output.append("\n" if current == "\n" else " ")
            pos += 1
            if (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                state = "code"
    return "".join(output)


def block_after(source: str, marker: str, start: int = 0) -> str:
    marker_at = source.find(marker, start)
    require(marker_at >= 0, f"missing production marker: {marker!r}")
    brace_at = source.find("{", marker_at + len(marker))
    require(brace_at >= 0, f"missing body after marker: {marker!r}")
    return brace_block(source, brace_at)


def last_block_after(source: str, marker: str) -> str:
    marker_at = source.rfind(marker)
    require(marker_at >= 0, f"missing production marker: {marker!r}")
    brace_at = source.find("{", marker_at + len(marker))
    require(brace_at >= 0, f"missing body after marker: {marker!r}")
    return brace_block(source, brace_at)


def all_blocks_after(source: str, marker: str) -> list[str]:
    blocks: list[str] = []
    cursor = 0
    while True:
        marker_at = source.find(marker, cursor)
        if marker_at < 0:
            return blocks
        brace_at = source.find("{", marker_at + len(marker))
        require(brace_at >= 0, f"missing body after marker: {marker!r}")
        body = brace_block(source, brace_at)
        blocks.append(body)
        cursor = brace_at + len(body)


settings_h = (COMPONENT / "System_Settings.h").read_text(encoding="utf-8")
settings_cpp = (COMPONENT / "System_Settings.cpp").read_text(encoding="utf-8")
command_types = (COMPONENT / "System_CommandTypes.h").read_text(encoding="utf-8")
web = (COMPONENT / "WebServer_Server.cpp").read_text(encoding="utf-8")
ble_peers = (COMPONENT / "BLE_Peers.cpp").read_text(encoding="utf-8")
g2_ring = (COMPONENT / "G2_Ring.cpp").read_text(encoding="utf-8")
g2_glasses = (COMPONENT / "G2_Glasses.cpp").read_text(encoding="utf-8")
hardwareone = (COMPONENT / "HardwareOne.cpp").read_text(encoding="utf-8")
rtc = (COMPONENT / "i2csensor_ds3231.cpp").read_text(encoding="utf-8")


# The old switch was process-global: one transport's `beginwrite` suppressed
# unrelated background and transport writes. Do not let either code or stale
# declarations reintroduce it anywhere in the production component sources.
for production_path in COMPONENT.iterdir():
    if production_path.is_file() and production_path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
        production = production_path.read_text(encoding="utf-8")
        require(
            "gDeferWrites" not in production,
            f"process-global settings deferral returned in {production_path.name}",
        )


# Every common scalar/String mutator must both request persistence and retain
# its setting_changed notification. Debug fields use their separate file.
setting_templates = all_blocks_after(settings_h, "inline void setSetting(")
require(len(setting_templates) == 3, "expected the three setSetting overloads")
for body in setting_templates:
    require(
        body.count("requestSettingsPersist(") == 1,
        "setSetting overload bypasses centralized persistence",
    )
    require(
        "notifySettingChanged(&field)" in body,
        "setSetting overload lost its setting_changed notification",
    )
    require("writeSettingsJson(" not in cpp_code(body), "setSetting writes flash directly")

debug_template = block_after(settings_h, "inline void setDebugSetting(")
require(
    debug_template.count("requestDebugSettingsPersist(") == 1,
    "setDebugSetting bypasses debug persistence batching",
)
require("writeDebugJson(" not in cpp_code(debug_template), "setDebugSetting writes flash directly")


# The table-driven command handler has four mutating paths: integer-family,
# float, bool, and String. All four must enter the same owner-aware adapter.
generic_handler = block_after(
    settings_cpp, "const char* handleSettingCommand("
)
require(
    generic_handler.count("requestSettingsPersist(") == 4,
    "not all four generic setting mutation branches request persistence",
)
require(
    "writeSettingsJson(" not in cpp_code(generic_handler),
    "generic setting handler bypasses owner-scoped batching",
)


# BLE peer changes happen in workers as well as command handlers. They must use
# the centralized request adapter, while learned-target code only reports the
# dirty result to callers so persistence happens after its completion lock.
owner_persist = block_after(ble_peers, "void peerOwnerPersistAfterUnlock(")
require("requestSettingsPersist(" in owner_persist, "BLE owner change bypasses batching")
require("bumpIdentityGeneration(" in owner_persist, "BLE owner change lost identity invalidation")

for signature in (
    "void bleSavePeerMac(",
    "bool bleSavePeerMacIfIdentityCurrent(",
):
    body = block_after(ble_peers, signature)
    require("targetChanged" in body, f"{signature} lost change-only persistence")
    require("requestSettingsPersist(" in body, f"{signature} bypasses batching")
    require("writeSettingsJson(" not in cpp_code(body), f"{signature} writes flash directly")

learned_target = block_after(
    ble_peers, "bool blePeerCommitLearnedTargetIfCurrent("
)
require(
    re.search(
        r"if\s*\(\s*persistNeeded\s*\)\s*\*persistNeeded\s*=\s*targetChanged\s*;",
        learned_target,
    )
    is not None,
    "learned BLE targets must report every real change, including during a batch",
)
require(
    "requestSettingsPersist(" not in cpp_code(learned_target)
    and "writeSettingsJson(" not in cpp_code(learned_target),
    "learned-target commit performs persistence while its completion scope is active",
)

auto_reconnect = block_after(ble_peers, "const char* cmd_bleautoreconnect(")
require(
    auto_reconnect.count("requestSettingsPersist(") >= 2,
    "BLE auto-reconnect mutation paths bypass centralized persistence",
)
require("writeSettingsJson(" not in cpp_code(auto_reconnect), "BLE auto-reconnect writes directly")

for source_name, source in (("G2_Ring.cpp", g2_ring), ("G2_Glasses.cpp", g2_glasses)):
    dirty_results = len(re.findall(r"\bbool\s+persistNeeded\s*=\s*false\s*;", source))
    deferred_flushes = len(
        re.findall(
            r"if\s*\(\s*persistNeeded\s*\)\s*\(void\)\s*requestSettingsPersist\s*\(\s*\)\s*;",
            source,
        )
    )
    require(dirty_results > 0, f"{source_name} lost its learned-target dirty handoff")
    require(
        deferred_flushes == dirty_results,
        f"{source_name} does not persist every learned target after unlocking",
    )


# A queued CommandContext carries a boot-local value, never an HTTP request or
# pointer whose lifetime can end before cmd_exec_task consumes the command.
command_context = block_after(command_types, "struct CommandContext")
require(
    re.search(r"\buint32_t\s+settingsBatchId\s*=\s*0\s*;", command_context)
    is not None,
    "CommandContext lacks a zero-defaulted value settings batch token",
)
require(
    re.search(r"[*&]\s*settingsBatchId\b", command_context) is None,
    "settings batch ownership must not be represented by a pointer/reference",
)


# The local HTTP batch recognizes the explicit begin/write/save wrapper,
# allocates one nonzero token, stamps that same value on every queued command,
# and installs an RAII finalizer before executing the loop.
cli_batch = block_after(web, "esp_err_t handleCliBatch(")
require('"beginwrite"' in cli_batch and '"savesettings"' in cli_batch,
        "local CLI batch no longer recognizes the settings wrapper")

allocation = re.search(
    r"(?:const\s+)?uint32_t\s+(\w*[Bb]atch\w*(?:Id|Token)\w*)\s*=\s*"
    r"[^;]{0,180}allocateSettingsWriteBatchId\s*\(\s*\)[^;]*;",
    cli_batch,
)
require(allocation is not None, "local CLI batch does not allocate a request token")
local_batch_id = allocation.group(1)
require(
    re.search(
        rf"\buc\.ctx\.settingsBatchId\s*=\s*{re.escape(local_batch_id)}\s*;",
        cli_batch,
    )
    is not None,
    "local CLI commands do not share the request's settings batch token",
)

allocator = block_after(settings_cpp, "uint32_t allocateSettingsWriteBatchId(")
require(
    re.search(r"\b\w*[Bb]atch\w*\s*==\s*0", allocator) is not None,
    "settings batch token allocator does not explicitly exclude zero",
)
require(
    allocator.find("== 0") < allocator.rfind("return"),
    "settings batch token can return before its zero check",
)

cleanup_types: list[str] = []
for declaration in re.finditer(r"\b(?:class|struct)\s+(\w+)[^{;]*\{", web):
    type_name = declaration.group(1)
    body = brace_block(web, web.find("{", declaration.start()))
    if (
        'line = "savesettings"' in body
        and "submitAndExecuteSync(" in body
        and "settingsBatchId" in body
        and re.search(
        rf"~\s*{re.escape(type_name)}\s*\(", body
        )
    ):
        cleanup_types.append(type_name)
require(
    cleanup_types,
    "web settings batch cleanup is not an executor-affine RAII finalizer",
)
cleanup_installations = [
    match
    for type_name in cleanup_types
    if (
        match := re.search(
            rf"\b{re.escape(type_name)}\s+\w+\s*(?:\{{|\()[^;]*\b{re.escape(local_batch_id)}\b",
            cli_batch,
        )
    )
]
require(
    cleanup_installations,
    "local CLI batch does not install its cleanup finalizer",
)
command_loop = re.search(r"for\s*\(\s*JsonVariant\s+\w+\s*:", cli_batch)
require(command_loop is not None, "local CLI batch command loop is missing")
require(
    min(match.start() for match in cleanup_installations) < command_loop.start(),
    "local CLI batch installs cleanup only after command execution begins",
)
require(
    "flushSettingsWriteBatch(" not in web,
    "web finalizer can race timed-out commands by flushing outside cmd_exec FIFO",
)
require(
    re.search(r"if\s*\(\s*containsBeginWrite\s*&&\s*!wrappedSettingsBatch\s*\)", cli_batch)
    is not None,
    "web batch must reject an unbounded beginwrite without rejecting standalone savesettings",
)


# Command-level markers are exact, argument-free boundaries. A second
# executor-affine request finalizer is harmless after the user's terminal save,
# while an ordinary standalone savesettings retains its historical full flush.
begin_handler = last_block_after(settings_cpp, "const char* cmd_beginwrite(")
save_handler = last_block_after(settings_cpp, "const char* cmd_savesettings(")
for name, handler in (("beginwrite", begin_handler), ("savesettings", save_handler)):
    require("args.trim()" in handler, f"{name} no longer normalizes marker arguments")
    require(
        "args.length() != 0" in handler,
        f"{name} accepts arguments that evade exact web marker detection",
    )
require(
    "hw1_settings_batch::ALL" in save_handler,
    "standalone savesettings lost its compatibility full flush",
)
require(
    "explicitRequestBatch" in save_handler
    and "Settings batch already finalized" not in save_handler,
    "missing explicit request slot can falsely report success during expiry I/O",
)
require(
    "disarmAfterConfirmedTerminalSave" in cli_batch,
    "normal web batches do not disarm cleanup after a confirmed terminal save",
)
require(
    "batchCommandsCompleted" in cli_batch
    and "batch_executor_unconfirmed" in cli_batch
    and "commandCompleted" in cli_batch,
    "web batch does not distinguish executor completion from handler success",
)
require(
    'output == "OK: Settings saved"' in web
    and 'output == "OK: Settings saved — no changed files"' in web,
    "web cleanup disarm ignores executeCommand's successful OK status stamp",
)


# Full-file replacement uses shared temporary paths. Both writers therefore
# must hold the same recursive single-flight guard for their complete bodies.
main_writer = block_after(settings_cpp, "bool writeSettingsJson(")
debug_writer = block_after(settings_cpp, "bool writeDebugJson(")
guard_pattern = re.compile(r"\b(\w*(?:Write|File)\w*Guard)\s+\w+")
main_guards = set(guard_pattern.findall(main_writer))
debug_guards = set(guard_pattern.findall(debug_writer))
common_guards = main_guards & debug_guards
require(common_guards, "main/debug settings writers do not share a single-flight guard")
require(
    "xSemaphoreCreateRecursiveMutex" in settings_cpp
    and "xSemaphoreTakeRecursive" in settings_cpp
    and "xSemaphoreGiveRecursive" in settings_cpp,
    "settings writer single-flight lock is not recursive",
)
for writer_name, writer in (("main", main_writer), ("debug", debug_writer)):
    compact_writer = re.sub(r"\s+", " ", cpp_code(writer))
    write_order = [
        compact_writer.find("expectedBytes = measureJson(doc)"),
        compact_writer.find("bytesWritten = serializeJson(doc, file)"),
        compact_writer.find("file.flush()"),
        compact_writer.find("observedBytes = file.size()"),
        compact_writer.find("file.close()"),
        compact_writer.find(
            "bytesWritten != expectedBytes || observedBytes != expectedBytes"
        ),
        compact_writer.find("renameGuarded("),
        compact_writer.find("directBytesWritten = serializeJson(doc, directFile)"),
        compact_writer.find("directFile.flush()"),
        compact_writer.find("directObservedBytes = directFile.size()"),
        compact_writer.find("directFile.close()"),
        compact_writer.find(
            "directBytesWritten != expectedBytes || directObservedBytes != expectedBytes"
        ),
    ]
    require(
        all(position >= 0 for position in write_order)
        and write_order == sorted(write_order),
        f"{writer_name} rename fallback can report success after a partial direct write",
    )

# Batch bookkeeping uses a short portMUX section. Flash/VFS work must always be
# after the matching exit: filesystem calls can block and are illegal inside a
# FreeRTOS critical section.
batch_critical_sections = re.findall(
    r"portENTER_CRITICAL\s*\(\s*&(?P<mux>\w*(?:Settings|settings)\w*(?:Batch|batch)\w*)\s*\)"
    r"(?P<body>[\s\S]*?)"
    r"portEXIT_CRITICAL\s*\(\s*&(?P=mux)\s*\)",
    settings_cpp,
)
require(batch_critical_sections, "settings batch core is not protected by a bounded critical section")
for _mux, critical_body in batch_critical_sections:
    critical_code = cpp_code(critical_body)
    for forbidden in (
        "writeSettingsJson(",
        "writeDebugJson(",
        "VFS::",
        "fsLock(",
        "File ",
        "serializeJson(",
    ):
        require(
            forbidden not in critical_code,
            f"filesystem work {forbidden!r} occurs under the settings batch critical section",
        )


# Explicit web batches win first; a live transport-session generation wins
# next; stateless Basic Auth and ESP-NOW bond batches fall back to a stable
# owner derived from command source plus auth identity. In particular, user
# and IP are required because Basic Auth has no SID and ESP-NOW encodes the
# authenticated sender MAC in its auth/IP context.
owner_context_at = settings_cpp.find("currentCommandContext()")
require(owner_context_at >= 0, "settings persistence does not inspect command ownership")
owner_window = settings_cpp[max(0, owner_context_at - 500) : owner_context_at + 5000]
for required_identity_part in (
    "settingsBatchId",
    "transportSessionEpoch",
    "origin",
    "auth.transport",
    "auth.user",
    "auth.sid",
    "auth.ip",
):
    require(
        required_identity_part in owner_window,
        f"fallback settings owner omits {required_identity_part}",
    )

# Direct/background callers have no request boundary. They must write now,
# never inherit cmd_exec_task as a shared owner. Abandoned command owners are
# reaped from the main loop and failed durability intent is retained.
owner_resolver = block_after(settings_cpp, "static bool currentSettingsBatchOwner(")
require(
    "xTaskGetCurrentTaskHandle" not in cpp_code(owner_resolver),
    "no-context settings calls still collapse onto shared executor task identity",
)
for signature, writer in (
    ("bool requestSettingsPersist(", "writeSettingsJson("),
    ("bool requestDebugSettingsPersist(", "writeDebugJson("),
):
    body = block_after(settings_cpp, signature)
    require(
        body.find("currentSettingsBatchOwner(") < body.find(writer) < body.find(".note("),
        f"{signature} does not write immediately when it lacks an owner",
    )

expiry_tick = block_after(settings_cpp, "void settingsWriteBatchTick(")
require("takeExpired(" in expiry_tick, "abandoned settings batches are never reaped")
require(
    "sSettingsWritePendingRetryMask" in expiry_tick,
    "failed settings writes have no bounded retry path",
)
require(
    "sSettingsWriteRetryNotBeforeMs" in expiry_tick
    and "settingsWriteRetryDue(" in expiry_tick,
    "failed settings writes can retry on every main-loop tick without backoff",
)
require(
    expiry_tick.find("SettingsFileWriteGuard tickWriteGuard(0)")
    < expiry_tick.find("sSettingsWritePendingRetryMask"),
    "retry bits are claimed before the tick owns the serialized writer lane",
)
expiry_tick_code = cpp_code(expiry_tick)
tick_guard_at = expiry_tick_code.find("SettingsFileWriteGuard tickWriteGuard(0)")
batch_mux_at = expiry_tick_code.find("portENTER_CRITICAL", tick_guard_at)
locked_now_at = expiry_tick_code.find("millis()", batch_mux_at)
retry_claim_at = expiry_tick_code.find("sSettingsWritePendingRetryMask", batch_mux_at)
take_expired_at = expiry_tick_code.find("takeExpired(", batch_mux_at)
batch_mux_exit_at = expiry_tick_code.find("portEXIT_CRITICAL", batch_mux_at)
require(
    tick_guard_at >= 0
    and batch_mux_at > tick_guard_at
    and locked_now_at > batch_mux_at
    and retry_claim_at > locked_now_at
    and take_expired_at > retry_claim_at
    and batch_mux_exit_at > take_expired_at,
    "expiry age is not sampled under the batch mux immediately before bookkeeping",
)
require(
    "clearSettingsWriteRetry(hw1_settings_batch::MAIN)" in main_writer
    and "clearSettingsWriteRetry(hw1_settings_batch::DEBUG)" in debug_writer,
    "a later successful full write leaves an obsolete retained retry pending",
)
require(
    "settingsWriteBatchTick();" in hardwareone,
    "settings batch expiry/retry tick is not wired into the main loop",
)

# The RTC worker can persist calibration. Force-deleting it while the shared
# writer guard is live would skip C++ destructors and strand the mutex forever.
rtc_stop = block_after(rtc, "void rtcStop(")
rtc_start = block_after(rtc, "bool rtcStartInternal(")
require(
    "vTaskDelete(gRtcTaskHandle)" not in cpp_code(rtc_stop),
    "RTC stop can force-delete a task that owns the settings writer guard",
)
require(
    "gRtcTaskHandle != nullptr" in rtc_start,
    "RTC start can revive/create a poller while the previous task is still exiting",
)
require(
    rtc_start.find("gRtcRunning && gRtcConnected")
    < rtc_start.find("gRtcTaskHandle != nullptr"),
    "RTC idempotent-start success must precede the stale exiting-task guard",
)
require(
    "eDeleted" in rtc_start and "eInvalid" in rtc_start,
    "RTC start no longer recovers a genuinely stale deleted task handle",
)


print("settings write batch integration source guards passed")
