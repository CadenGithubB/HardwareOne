#!/usr/bin/env python3
"""Source-contract checks for the production command ingress boundaries.

--component <dir> points the hardwareone reads at another copy of the sources;
the vendored Arduino BLE library is always read from this checkout.
"""

import argparse
from pathlib import Path


HERE = Path(__file__).resolve().parent
_parser = argparse.ArgumentParser(description=__doc__)
_parser.add_argument("--component", type=Path, default=HERE.parents[1])
ROOT = _parser.parse_args().component
ARDUINO = HERE.parents[2] / "arduino"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


limits = (ROOT / "System_CommandLimits.h").read_text()
types = (ROOT / "System_CommandTypes.h").read_text()
utils = (ROOT / "System_Utils.cpp").read_text()
hardwareone = (ROOT / "HardwareOne.cpp").read_text()
uart = (ROOT / "System_UartLink.cpp").read_text()
bluetooth = (ROOT / "Bluetooth.cpp").read_text()
ble_secure = (ROOT / "System_BleSecureChannel.cpp").read_text()
web_handle = (ROOT / "WebServer_Handle.h").read_text()
web_server = (ROOT / "WebServer_Server.cpp").read_text()
ble_characteristic_h = (
    ARDUINO / "libraries" / "BLE" / "src" / "BLECharacteristic.h"
).read_text()
ble_characteristic_cpp = (
    ARDUINO / "libraries" / "BLE" / "src" / "BLECharacteristic.cpp"
).read_text()

assert "CMD_INPUT_MAX = 2047" in limits
assert "CMD_RESULT_MAX = 4096" in limits
assert "commandInputLengthAccepted" in limits
assert "char line[CMD_INPUT_MAX + 1]" in types
assert "char out[CMD_RESULT_MAX]" in types

sync = function_body(utils, "bool submitAndExecuteSync(")
async_body = function_body(utils, "bool submitCommandAsync(")
executor = function_body(hardwareone, "static void commandExecTask(void* pv) {")

# The bounded receive is the executor's idle wait and OTA heartbeat cadence.
# Once it times out, loop directly back to housekeeping and another blocking
# receive; a second delay creates an avoidable command-arrival blackout. Keep
# the distinct post-command yield that protects Core 0 under sustained work.
assert "xQueueReceive(gCmdExecQ, &item, pdMS_TO_TICKS(1000))" in executor
receive_success = function_body(executor, "if (receiveResult == pdTRUE)")
assert receive_success.rstrip().endswith(
    "vTaskDelay(pdMS_TO_TICKS(1));\n    }"
)
success_offset = executor.index(receive_success)
assert executor[success_offset + len(receive_success) :].strip() == "}\n}"

sync_guard = sync.index("commandInputLengthAccepted")
assert sync_guard < sync.index("if (gCmdExecQ == nullptr)")
assert sync_guard < sync.index("ps_alloc(")
assert 'out = "Error: Command exceeds input limit"' in sync
assert "memcpy(r->line, cmd.line.c_str(), inputLength + 1)" in sync
assert "strncpy(r->line" not in sync

# An HTTP shutdown command can start on cmd_exec as soon as its pointer is
# queued. Publish the blocked-httpd guard first so closehttp/closewifi cannot
# enter httpd_stop() in the cross-core window between enqueue and waiter setup.
waiter_publish = sync.index("gWebCmdWaiters.fetch_add(1, std::memory_order_release)")
queue_publish = sync.index("xQueueSend(gCmdExecQ")
assert waiter_publish < queue_publish
queue_failure = sync.index("if (queueResult != pdTRUE)", queue_publish)
waiter_rollback = sync.index(
    "gWebCmdWaiters.fetch_sub(1, std::memory_order_acq_rel)", queue_failure
)
assert queue_failure < waiter_rollback < sync.index(
    'broadcastOutput("[ERROR] Command queue full - try again")', queue_failure
)
assert "extern std::atomic<int> gWebCmdWaiters;" in web_handle
assert "std::atomic<int> gWebCmdWaiters{0};" in web_server
assert "static std::atomic<bool> sHttpStopPending{false};" in web_server

async_guard = async_body.index("commandInputLengthAccepted")
assert async_guard < async_body.index("if (gCmdExecQ == nullptr)")
assert async_guard < async_body.index("ps_alloc(")
assert "memcpy(r->line, cmd.line.c_str(), inputLength + 1)" in async_body
assert "strncpy(r->line" not in async_body

assert '#include "System_CommandLimits.h"' in uart
assert "kUartLineCap = CMD_INPUT_MAX" in uart
assert "sDiscardingLine = true" in uart
assert "line too long" in uart and "discarded" in uart

# The USB console buffers match the command limits and over-limit lines are
# discarded whole, the same rule the UART link applies.
assert "Serial.setRxBufferSize(CMD_RESULT_MAX)" in hardwareone
assert "Serial.setTxBufferSize(CMD_RESULT_MAX)" in hardwareone
assert "gSerialCLI.reserve(CMD_INPUT_MAX + 1)" in hardwareone
assert "gSerialCLIOverflow = true" in hardwareone
assert "line too long" in hardwareone and "discarded" in hardwareone

ble_line = function_body(bluetooth, "static void processBleCommandLine(")
raw_length_guard = ble_line.index("if (len > sizeof(cmdBuf) - 1)")
ble_overflow = ble_line.index("if (lineTooLong)")
assert raw_length_guard < ble_line.index("for (size_t i = 0; i < len; i++)")
assert "BLE command exceeds 511-byte input limit; discarded" in ble_line[
    raw_length_guard:ble_overflow
]
assert "for (size_t i = 0; i < len; i++)" in ble_line
assert "outIdx < sizeof(cmdBuf) - 1" not in ble_line
assert "outIdx >= sizeof(cmdBuf) - 1" in ble_line
assert "BLE command exceeds 511-byte input limit; discarded" in ble_line
assert ble_overflow < ble_line.index("Command ucmd;")
assert ble_overflow < ble_line.index("submitCommandAsync(")

ble_ingress = function_body(
    bluetooth,
    "static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {",
)
frame_branch = ble_ingress.index("if (isFrame)")
frame_length_guard = ble_ingress.index(
    "len > sizeof(((BleScDeferred*)0)->buf)", frame_branch
)
assert frame_branch < frame_length_guard < ble_ingress.index(
    "submitDeferredToCmdExec(bleScDeferredInbound, d)"
)
assert "isFrame && len <=" not in ble_ingress
assert "BLE secure frame exceeds 517-byte input limit; discarded" in ble_ingress

# The GATT write entry runs on BTC_TASK, which serves the GATT/GAP callbacks of
# every link. A reply over an established channel waits for the secure-channel tx
# mutex (held for a whole paced reply), so the entry only copies and defers:
# both kinds of write run on cmd_exec, and every notice the entry itself sends is
# best-effort (blocking=false never waits for that mutex).
for sender in ("sendBLEResponseToSession(", "bleRawNotifyToSession("):
    pos = ble_ingress.find(sender)
    while pos >= 0:
        call = ble_ingress[pos : ble_ingress.index(";", pos)]
        assert "/*blocking=*/false)" in call, f"BTC_TASK reply may wait: {call}"
        pos = ble_ingress.find(sender, pos + 1)
for forbidden in ("processBleCommandLine(", "bleSendAuthRequired(",
                  "sendBLEResponse(", "bleScSendEncrypted"):
    assert forbidden not in ble_ingress, f"BTC_TASK ingress calls {forbidden}"
assert "submitDeferredToCmdExec(blePlaintextDeferredInbound, d)" in ble_ingress
on_write = function_body(bluetooth, "void onWrite(BLECharacteristic* pCharacteristic,")
for sender in ("sendBLEResponse", "bleRawNotifyToSession(", "bleScSendEncrypted",
               "processBleCommandLine("):
    assert sender not in on_write, f"the GATT write callback calls {sender}"

assert "setPreparedWriteEnabled(false)" in bluetooth
assert "void setPreparedWriteEnabled(bool value);" in ble_characteristic_h
prepared_reject = ble_characteristic_cpp.index(
    "param->write.is_prep && !m_preparedWriteEnabled"
)
assert prepared_reject < ble_characteristic_cpp.index(
    "m_value.addPart(param->write.value", prepared_reject
)
assert "ESP_GATT_NOT_LONG" in ble_characteristic_cpp[
    prepared_reject : prepared_reject + 1200
]
assert "return;" in ble_characteristic_cpp[prepared_reject : prepared_reject + 1200]

ble_decrypt = function_body(ble_secure, "BleScResult bleScHandleInbound(")
assert "if (ctLen + 1 > outCap) return BLE_SC_ERROR;" in ble_decrypt
data_length_guard = ble_decrypt.index("ctLen + 1 > outCap")
data_decrypt = ble_decrypt.index(
    "crypto_aead_chacha20poly1305_ietf_decrypt_detached(", data_length_guard
)
assert data_length_guard < data_decrypt

# Secure-channel frames are never characteristic-wide: no raw broadcast sender
# exists, the response characteristic is never set-and-notified, and every
# handshake reply and DATA fragment is addressed to one connection and session
# epoch through the directed, lifecycle-guarded sender.
assert "bleRawNotify(" not in ble_secure
assert "bool bleRawNotify(" not in bluetooth
assert "gLastNotifyResult" not in bluetooth
assert "pCmdResponseChar->notify()" not in bluetooth
assert "pCmdResponseChar->setValue(" not in bluetooth
assert ble_decrypt.count("bleRawNotifyToSession(connId, epoch,") == 2  # HELLO_ACK, CONFIRM_ACK
assert ble_decrypt.count("scSendReject(connId, epoch,") == 4
assert "bleRawNotifyToSession(connId, epoch," in function_body(
    ble_secure, "static void scSendReject("
)
assert "bleRawNotifyToSession(binding.connId, binding.epoch," in function_body(
    ble_secure, "bool bleScSendEncryptedBound("
)
assert "bleScHandleInbound(d->connId, d->sessionEpoch," in function_body(
    bluetooth, "static void bleScDeferredInbound("
)

# The file-browser gate follows how this command arrived, not whether the
# connection also holds a channel: only the deferred DATA-frame job passes
# arrivedEncrypted=true; the plaintext path passes false (test_ble_sc_policy.py
# runs both paths).
line_start = bluetooth.index("static void processBleCommandLine(")
assert "bool arrivedEncrypted)" in bluetooth[line_start : bluetooth.index("{", line_start)]
assert "if (bleIsFileBrowserCommand(cmdStart) && !arrivedEncrypted)" in ble_line
assert "bleScEstablished" not in ble_line
ble_deferred = function_body(bluetooth, "static void bleScDeferredInbound(")
assert ble_deferred.count("processBleCommandLine(") == 1
assert "/*arrivedEncrypted=*/true)" in ble_deferred
ble_plain_deferred = function_body(
    bluetooth, "static void blePlaintextDeferredInbound(void* arg)"
)
assert ble_plain_deferred.count("processBleCommandLine(") == 1
assert "/*arrivedEncrypted=*/false)" in ble_plain_deferred
assert "bleScPoolRelease(d);" in ble_plain_deferred
assert bluetooth.count("processBleCommandLine(d->connId, d->sessionEpoch,") == 2
# In required mode (and always on the G2 client host, whose console serves
# Secure Channel only) a plaintext writer without a channel gets the fixed
# refusal through the directed per-session sender; the session sender would
# drop it.
required_branch = function_body(ble_plain_deferred, "if (bleSecureChannelOnly())")
secure_only = function_body(bluetooth, "static bool bleSecureChannelOnly()")
assert "bleScRequired()" in secure_only and "bleConsoleClientHost()" in secure_only
# Every session sender goes through one router (bleSendToSession): the phone
# link's console row first (console records), then the channel and plaintext
# rules below, unchanged for every other row; a binary body is never plaintext.
assert "return bleSendToSession(connId, expectedEpoch, PipeConsoleKind::Text," in function_body(
    bluetooth, "bool sendBLEResponseToSession(")
ble_router = function_body(bluetooth, "static bool bleSendToSession(")
assert "if (!secure && bleSecureChannelOnly()) return false;" in ble_router
assert "if (!secure && kind == PipeConsoleKind::Blob) return false;" in ble_router
assert ble_router.index("pipe = blePipeRow(c);") < ble_router.index("secure = bleScEstablished(connId);")
# The phone link's console stream (phone link v2): its records arrive through
# the pipe's authenticated encryption, so its COMMAND ingress passes
# arrivedEncrypted=true (the file-browser gate above is unchanged), and only
# the console row takes them.
ble_pipe_inbound = function_body(bluetooth, "void blePipeConsoleInbound(")
assert ble_pipe_inbound.count("processBleCommandLine(") == 1
assert "/*arrivedEncrypted=*/true);" in ble_pipe_inbound
assert "!blePipeRow(gBLEState->connections[findConnectionSlotByConnIdLocked(rowConnId)])" in ble_pipe_inbound
assert ble_pipe_inbound.index("bleConnectionEpochMatchesLocked(rowConnId, epoch)") < ble_pipe_inbound.index(
    "processBleCommandLine(")
assert ble_pipe_inbound.index("bleMarkActivity(rowConnId);") < ble_pipe_inbound.index(
    "otaBleHandleEncryptedFrame(rowConnId, bytes, len)")
assert bluetooth.count("/*arrivedEncrypted=*/true)") == 2  # the SC DATA job and the console stream
assert "if (bleScEstablished(d->connId))" in required_branch
assert "bleRawNotifyToSession(d->connId, d->sessionEpoch," in required_branch
# The Bluetooth security-posture commands share the superadmin tier.
for posture_command in ("blerequireauth", "blesecret", "blesecure"):
    row_start = bluetooth.index('{ "' + posture_command + '",')
    row = bluetooth[row_start : bluetooth.index("\n", row_start)]
    assert row.rstrip().endswith("/*requiresSuperAdmin=*/true },"), row

print("command ingress source guards passed")
