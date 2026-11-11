#pragma once
// The part of Bluetooth.h that System_BleSecureChannel.cpp uses. The harness
// defines these functions (some spliced in from Bluetooth.cpp).
#include <stddef.h>
#include <stdint.h>
#include "System_User.h"  // TransportSessionEpoch
bool isBleConnected();
TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId);
bool bleSessionEpochMatches(uint16_t connId, TransportSessionEpoch epoch);
bool bleDataDebugEnabled();
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len, bool blocking);
