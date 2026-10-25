/**
 * BLE_CentralStack — the one owner of the shared BLE central stack.
 *
 * The stack is the Bluedroid host plus its controller link (local on the S3 /
 * ESP32, the C6 over ESP-Hosted on the P4), the setup worker that serialises
 * every central connect, the central TX gate, the GAP/GATTC observers and the
 * scan object. The glasses, the ring and the phone are device owners on top of
 * it: `openg2` / `openring` / `openphone` call bleStackUp() when the stack is
 * down and never bring it down. Only `closeble` and `bleenabled 0`, the switch
 * to the app-console server (`openconsole`) and the stack recoveries call
 * bleStackDown(). A soft disconnect (link loss, a device close) therefore never
 * powers Bluetooth down.
 *
 * "Up" means the host runs in the client role and a device owner may connect.
 * "Down" means a verified terminal host/controller teardown: no BLE callback
 * can arrive and the setup worker task is gone. Both operations are idempotent
 * and fail closed: a refused or incomplete teardown leaves every task alive and
 * reports why.
 */
#ifndef BLE_CENTRAL_STACK_H
#define BLE_CENTRAL_STACK_H

#include "System_BuildConfig.h"
#include <stdint.h>

class BLEScan;

#if ENABLE_BLUETOOTH

// Bring the client-role stack up (host, setup worker, observers, scan object,
// the console service on the client host where the pipe is built) and arm the
// peers whose auto-reconnect is on. True when the stack is ready, also when it
// already was. Refuses (false, see bleStackLastRefusal()) when Bluetooth is
// disabled, a prior teardown faulted, the radio companion blocks the radio or
// the app-console server owns the host. Caller task context only; takes the
// re-entrant BLE role-transition claim itself.
bool bleStackUp();

// Verified terminal teardown of the client-role stack. Requires that no device
// is open (glasses runtime, ring owner, phone pipe) and no server runs; the
// device owners close themselves first. Skips the HAL when no host of any
// generation is up (BluetoothHalStatus::hostAbsent()). On success the setup
// worker is released as well. A HAL failure sets the stack lifecycle fault and
// returns false without releasing anything.
bool bleStackDown();

// The host is up in the client role and usable by a device owner. A pure flag
// published by bleStackUp()/bleStackDown(); never a HAL query, so it is cheap
// enough for the reconnect tick and the ring/phone admission gates.
bool bleStackReady();

// The shared scan object while the stack is up; nullptr otherwise. The G2
// runtime caches it in gScan while open; the ring and phone scans borrow it.
BLEScan* bleStackScan();

// Why the last bleStackUp() or bleStackDown() refused, for command replies.
// Static text; never nullptr.
const char* bleStackLastRefusal();

// The companion transport died under a live host (System_RadioCompanion):
// the host is gone without a teardown of ours. Clears the ready flag so the
// owners stop admitting; the companion reset path then runs bleStackDown().
void bleStackNoteHostLost();

#else  // !ENABLE_BLUETOOTH

inline bool bleStackUp() { return false; }
inline bool bleStackDown() { return true; }
inline bool bleStackReady() { return false; }
inline BLEScan* bleStackScan() { return nullptr; }
inline const char* bleStackLastRefusal() { return "Bluetooth is not compiled into this build"; }
inline void bleStackNoteHostLost() {}

#endif  // ENABLE_BLUETOOTH
#endif  // BLE_CENTRAL_STACK_H
