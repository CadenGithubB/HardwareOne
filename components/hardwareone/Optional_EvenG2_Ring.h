#ifndef OPTIONAL_EVEN_G2_RING_H
#define OPTIONAL_EVEN_G2_RING_H

#include "System_BuildConfig.h"
#include <Arduino.h>

// =============================================================================
// Even Realities R1 Ring — BLE central module
// =============================================================================
// The R1 ring is a SEPARATE BLE peripheral from the G2 glasses. When paired
// with Even's phone app, the phone acts as the hub — it's the BLE central
// for both the ring AND the glasses, and it routes gesture events between
// them. When our ESP32 replaces the phone as the glasses' central, the ring
// is left without a hub.
//
// Path 1 implementation (info-only; see chat logs 2026-04-24): connect to
// the ring as a third BLE peripheral, subscribe to its notify stream, dump
// every health push (heart rate, steps, battery, etc.) for visibility. Do
// NOT attempt the pairAuth handshake — that requires a server-issued pkey
// from Even's API which we don't have. Ring-to-glasses gesture relay will
// NOT work in this mode; that's a separate (Path 3) project.
//
// Reference: ble/ring.ts in https://github.com/Commute773/g2-kit-unofficial
//
// Requires: ENABLE_BLUETOOTH=1 AND ENABLE_G2_GLASSES=1 (ring lives under
// the G2 umbrella — no separate compile flag because nobody has a ring
// without glasses).
// =============================================================================

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Ring BLE UUIDs (reference ble/ring.ts:25-27).
#define G2RING_SERVICE_UUID    "bae80001-4f05-4503-8e65-3af1f7329d1f"
#define G2RING_CHAR_WRITE_UUID "bae80012-4f05-4503-8e65-3af1f7329d1f"
#define G2RING_CHAR_NOTIFY_UUID "bae80013-4f05-4503-8e65-3af1f7329d1f"

// Advert name format: "EVEN R1_XXXXXX" where XXXXXX = last 3 bytes of MAC
// in hex (e.g. mac F8:29:CA:BA:AC:1C → name "EVEN R1_BAAC1C").

// Ring command opcodes (reference ble/ring.ts §R1_CMD). Not exhaustive —
// we only document the ones we might plausibly use in the info-only mode.
#define G2RING_CMD_HEARTRATE   0x01
#define G2RING_CMD_FIRMWARE    0x02
#define G2RING_CMD_TEMPERATURE 0x03
#define G2RING_CMD_HRV         0x04
#define G2RING_CMD_ACTIVITY    0x05
#define G2RING_CMD_SLEEP       0x06
#define G2RING_CMD_PAIR_AUTH   0x08
#define G2RING_CMD_HEALTH_SET  0x09
#define G2RING_CMD_LINK_G2     0x0A
#define G2RING_CMD_ALGO_KEY    0x0B
#define G2RING_CMD_CONFIG1     0x0E
#define G2RING_CMD_CONFIG2     0x0F
#define G2RING_CMD_SERIAL      0x11
#define G2RING_CMD_PHONE_STAT  0x7E
#define G2RING_CMD_PHONE_ACK   0x7F

// Ring flag values (envelope bytes [9..10] BE).
#define G2RING_FLAG_REQUEST    0x0000
#define G2RING_FLAG_SET        0x0001
#define G2RING_FLAG_PUSH       0x0002
#define G2RING_FLAG_RESPONSE   0x0003

// -----------------------------------------------------------------------------
// Public API (all return false / 0 / null when ring subsystem is disabled)
// -----------------------------------------------------------------------------

// Initialise the ring client (idempotent). Does NOT scan or connect —
// that happens via the CLI command or g2RingConnect().
bool g2RingInit();

// Scan for the ring advert (or use one already discovered by the main
// glasses scan) and connect to it. Non-blocking: spawns a background
// task and returns immediately, mirroring how g2Connect works for the
// glasses. Use g2RingIsConnected() / g2RingGetStatus() to poll.
bool g2RingConnect();

// Saved-MAC reconnect. Reads gSettings.bleRingMAC and connects directly
// without requiring a prior scan-cached advertisement. Used by the boot
// auto-reconnect hook. Returns false if no MAC saved or a connect is
// already in flight.
bool g2RingConnectSaved();

// Tear down the BLE connection. Safe to call from any context.
void g2RingDisconnect();

// True if we currently have a live BLE link to the ring.
bool g2RingIsConnected();

// Fill `buf` with a short human-readable status line. Used by the
// `ringstatus` CLI command and the web UI Ring panel.
void g2RingGetStatus(char* buf, size_t cap);

// Ring state shared with the G2 scan callback (defined in
// Optional_EvenG2_Ring.cpp). Declared extern here so the scanner can
// stash a discovered ring advert before our module's own scan runs.
// Guarded behind the compile flag so stubs don't drag these in.
class BLEAdvertisedDevice;
extern BLEAdvertisedDevice* gRingAdvertisedDevice;
extern String               gRingDeviceName;
extern String               gRingDeviceAddress;
extern volatile bool        gRingScanFound;

// Command table entry — wired into System_Utils.cpp alongside g2Commands.
// The ring lives under the G2 umbrella but keeps its own command array for
// clarity (ringstatus, ringconnect, ringdisconnect, ringdump).
struct CommandEntry;
extern const CommandEntry g2RingCommands[];
extern const size_t       g2RingCommandsCount;

#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

inline bool g2RingInit()         { return false; }
inline bool g2RingConnect()      { return false; }
inline bool g2RingConnectSaved() { return false; }
inline void g2RingDisconnect()   {}
inline bool g2RingIsConnected()  { return false; }
inline void g2RingGetStatus(char* buf, size_t cap) {
  if (buf && cap > 0) buf[0] = '\0';
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // OPTIONAL_EVEN_G2_RING_H
