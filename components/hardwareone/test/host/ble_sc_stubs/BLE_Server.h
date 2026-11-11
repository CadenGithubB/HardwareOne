#pragma once
// The part of BLE_Server.h that BLE_SecureChannel.cpp uses. The harness
// defines these functions (some spliced in from BLE_Server.cpp).
#include <stddef.h>
#include <stdint.h>
#include "System_User.h"  // TransportSessionEpoch
bool isBleConnected();
TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId);
bool bleSessionEpochMatches(uint16_t connId, TransportSessionEpoch epoch);
bool bleDataDebugEnabled();
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len, bool blocking);
