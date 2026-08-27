#!/usr/bin/env python3
"""Source-contract checks for the production command ingress boundaries."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


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
uart = (ROOT / "System_UartLink.cpp").read_text()
bluetooth = (ROOT / "Bluetooth.cpp").read_text()
ble_secure = (ROOT / "System_BleSecureChannel.cpp").read_text()
ble_characteristic_h = (
    ROOT.parent / "arduino" / "libraries" / "BLE" / "src" / "BLECharacteristic.h"
).read_text()
ble_characteristic_cpp = (
    ROOT.parent / "arduino" / "libraries" / "BLE" / "src" / "BLECharacteristic.cpp"
).read_text()

assert "CMD_INPUT_MAX = 2047" in limits
assert "CMD_RESULT_MAX = 4096" in limits
assert "commandInputLengthAccepted" in limits
assert "char line[CMD_INPUT_MAX + 1]" in types
assert "char out[CMD_RESULT_MAX]" in types

sync = function_body(utils, "bool submitAndExecuteSync(")
async_body = function_body(utils, "bool submitCommandAsync(")

sync_guard = sync.index("commandInputLengthAccepted")
assert sync_guard < sync.index("if (gCmdExecQ == nullptr)")
assert sync_guard < sync.index("ps_alloc(")
assert 'out = "Error: Command exceeds input limit"' in sync
assert "memcpy(r->line, cmd.line.c_str(), inputLength + 1)" in sync
assert "strncpy(r->line" not in sync

async_guard = async_body.index("commandInputLengthAccepted")
assert async_guard < async_body.index("if (gCmdExecQ == nullptr)")
assert async_guard < async_body.index("ps_alloc(")
assert "memcpy(r->line, cmd.line.c_str(), inputLength + 1)" in async_body
assert "strncpy(r->line" not in async_body

assert '#include "System_CommandLimits.h"' in uart
assert "kUartLineCap = CMD_INPUT_MAX" in uart
assert "sDiscardingLine = true" in uart
assert "line too long" in uart and "discarded" in uart

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
    "if (bleScRequired())"
)
assert "isFrame && len <=" not in ble_ingress
assert "BLE secure frame exceeds 517-byte input limit; discarded" in ble_ingress

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

print("command ingress source guards passed")
