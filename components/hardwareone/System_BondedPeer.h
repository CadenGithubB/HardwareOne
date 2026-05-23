#ifndef SYSTEM_BONDED_PEER_H
#define SYSTEM_BONDED_PEER_H

#include "System_BuildConfig.h"

#if ENABLE_BONDED_MODE

#include <Arduino.h>
#include <ArduinoJson.h>

// =============================================================================
// BondedPeer — unified accessor for the bonded worker device
// =============================================================================
//
// One namespace any UI can call to interact with the bonded peer: web UI today,
// OLED status screen / G2 menu tomorrow. Wraps the scattered gEspNow / gSettings
// fields, the file-cache reads (settings.json + schema.json under
// /system/espnow/peers/<MAC>/), and the sync-trigger handshake that was
// previously inlined in the /api/bond/* HTTP handlers.
//
// Master-only. On a worker, isPaired() may still be true (settings.bondModeEnabled
// + bondPeerMac populated), but the "pull peer settings" and sync-trigger calls
// are meaningless and will return false — the worker is the *source* of those
// payloads, not a consumer. Callers should gate UI affordances on
// `isMaster() && isPaired()`.
//
// Threading: read accessors are lock-free reads of gEspNow/gSettings fields
// (which are written from the ESP-NOW RX task without locks already — same
// risk profile as every other reader of gEspNow). The sync trigger methods
// block the calling task on a vTaskDelay poll loop and must not be called from
// the ESP-NOW RX task itself (would deadlock the very flag they wait on).
// =============================================================================

namespace BondedPeer {

// ----- State (lock-free reads) --------------------------------------------

// True if bond mode is enabled in settings AND a peer MAC is configured.
// Says nothing about whether the peer is currently reachable — see isOnline().
bool isPaired();

// True if this device is configured as the bond master (settings.bondRole).
// On the master, the BondedPeer namespace is the API for talking to the worker;
// on the worker, most operations no-op and return false.
bool isMaster();

// True if a heartbeat from the bonded peer was received within the timeout
// window (gEspNow->bondPeerOnline). Goes false on link loss/reboot.
bool isOnline();

// Bonded peer's MAC as a colon-separated string ("AA:BB:CC:DD:EE:FF") or "".
String peerMacString();

// Fill the 6-byte peer MAC. Returns false if no peer is configured.
bool peerMacBytes(uint8_t out[6]);

// Human-friendly name if known (from gEspNow->devices[].name where mac matches),
// otherwise the MAC string. Useful for UI labels ("Pull from <name>").
String peerName();

// ----- Settings / schema cache state --------------------------------------

// Peer's currently-advertised settings hash (via heartbeat). 0 = unknown.
uint32_t peerSettingsHash();

// Hash of the settings.json bytes we last cached to disk. 0 = nothing cached.
uint32_t cachedSettingsHash();

// True when bondPeerSettingsHash != bondCachedPeerSettingsHash AND we have a
// cache. UI uses this to render a "settings have changed since last sync" hint.
bool isSettingsDirty();

// ----- Cached file reads (master only) ------------------------------------

// Read /system/espnow/peers/<MAC>/settings.json into `out`. Returns empty
// string if no cached copy exists or filesystem isn't ready. (loadSettingsFromCache
// already lives in System_ESPNow.cpp; this is a forwarding wrapper.)
String readCachedSettingsJson();

// Read /system/espnow/peers/<MAC>/schema.json into `out`. Returns empty string
// if no cached copy exists. The schema is what the worker advertised on its
// last `schemaReq` reply — what's renderable in a remote settings editor.
String readCachedSchemaJson();

// ----- Sync triggers (master only, blocking) ------------------------------
//
// Each sync method clears the relevant `received` flag, sends the appropriate
// V4 request opcode to the peer, then polls the flag for up to timeoutMs.
// Returns true on success and writes elapsedMs (if non-null). Returns false
// on preconditions (not master / not paired / peer offline) and on timeout.
//
// The flag-clear must happen BEFORE the send so a stale "already received"
// from an earlier sync doesn't mask the new completion.

bool requestSettingsSync(uint32_t timeoutMs = 6000, uint32_t* outElapsedMs = nullptr);
bool requestSchemaSync  (uint32_t timeoutMs = 6000, uint32_t* outElapsedMs = nullptr);

// ----- Last-error -----------------------------------------------------------
// Set by the sync triggers + readers when they return false/"". Stable until
// the next BondedPeer call; safe to fetch immediately after a failed call to
// build a user-facing error message.
const char* lastError();

} // namespace BondedPeer

#endif // ENABLE_BONDED_MODE
#endif // SYSTEM_BONDED_PEER_H
