#pragma once
// OtaBleWire: the head of one OTA upload frame sent from the phone over BLE.
//   frame = marker 00 'HW1OTA' 01 (8 B) | offset u32 big-endian | chunk
// The one definition of the marker and the head length. Three readers must
// classify the same frames with it: the secure channel (BLE_SC_BINARY_READY in
// BLE_SecureChannel.cpp), the phone link v2 console records (BINARY magic and
// kBinaryMin in BLE_PhonePipeConsoleCore.h) and the OTA upload handler
// (handleBleUploadFrame in System_OTA.cpp).
// Pure and dependency-free (no BLE, OS or Arduino): the console record layer
// and host tests include it.
#include <cstddef>
#include <cstdint>

namespace hw1otawire {
inline constexpr uint8_t kOtaMagic[8] = {0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01};
constexpr size_t kOffsetSize = 4;                               // offset_be32
constexpr size_t kHeaderSize = sizeof(kOtaMagic) + kOffsetSize;  // the head before the chunk
static_assert(kHeaderSize == 12, "OtaBleWire head: 8 B marker + 4 B offset");
}  // namespace hw1otawire
