#ifndef BLE_PEERS_H
#define BLE_PEERS_H

// =============================================================================
// BLE peer registry — runtime, plug-in style
// =============================================================================
// Single registry for "things this device connects to over BLE as a central"
// — currently the G2 glasses pair, the R1 ring, and a placeholder phone
// peer. Each owning module registers itself at init time via
// bleRegisterPeer(); the registry then drives:
//
//   * Boot-time auto-reconnect (bleBootReconnect)
//   * MAC auto-save on first successful connect (bleSavePeerMac)
//   * The generic `bleautoreconnect` / `blepeers` CLI commands
//   * Settings serialization (the `peers` object under bluetooth.* in
//     settings.json)
//
// Why exists: before consolidation, every BLE peer carried its own
// per-MAC field, its own auto-connect bool, its own boot logic block, and
// its own auto-save snippet. Adding a fourth peer would have meant
// copying the same plumbing for the fourth time. The registry collapses
// it to one struct definition + one bleRegisterPeer call per peer.
//
// Schema in settings.json:
//   "bluetooth": {
//     ...
//     "peers": {
//       "g2-glasses": { "mac1": "...", "mac2": "...", "autoReconnect": true },
//       "r1-ring":    { "mac1": "...",                 "autoReconnect": false },
//       "phone":      { "mac1": "" }
//     }
//   }
//
#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH

// -----------------------------------------------------------------------------
// Peer identity
// -----------------------------------------------------------------------------

// Stable identity of every known peer kind. The numeric values are
// persisted indirectly (via the JSON name → kind lookup), so don't
// renumber once a release is out — add new values at the end.
enum BlePeerKind : uint8_t {
  BLE_PEER_G2_GLASSES = 0,
  BLE_PEER_R1_RING    = 1,
  BLE_PEER_PHONE      = 2,
  // Add new peers above this line.
  BLE_PEER_MAX        = 8,   // hard cap on the registry table
};

// Per-peer connect ops, supplied by the owning module at registration.
// All callbacks must be safe to call from any context (CLI thread, web
// handler thread, BLE notify task) — they typically spawn their own
// background task to do real work and return quickly.
struct BlePeerOps {
  // Begin a saved-MAC reconnect. Reads gBlePeerData[kind].mac1/mac2 and
  // initiates connection. Returns true if the attempt was started (not
  // necessarily that it'll succeed — connect is async).
  bool (*connectSaved)(void);
  // Tear down any active connection. Idempotent; safe when not connected.
  void (*disconnect)(void);
  // True when the peer is currently linked at the BLE level.
  bool (*isConnected)(void);
};

// Compile-time descriptor for a peer. Each owning module fills one of
// these and passes it to bleRegisterPeer().
struct BlePeerSpec {
  BlePeerKind       kind;          // identity in the registry
  const char*       name;          // CLI/JSON key: "g2-glasses", "r1-ring"
  const char*       displayName;   // UI label: "G2 Glasses", "R1 Ring"
  uint8_t           macCount;      // 1 (most peers) or 2 (G2 = L+R)
  bool              connectable;   // false = metadata-only (phone for now)
  const BlePeerOps* ops;           // null when !connectable
};

// Per-peer mutable data. Persisted in settings.json under the peer name.
// Indexed by BlePeerKind. All BLE_PEER_MAX slots exist; only the
// registered ones have meaningful data.
struct BlePeerData {
  String mac1;
  String mac2;        // empty unless macCount==2
  bool   autoReconnect;
  // Username of whoever first paired this peer (turned autoReconnect on
  // or saved the MAC). Used by callers that act *on behalf of* the
  // peer — most notably G2_HijackCmd, which needs an AuthContext.user
  // when submitting tap-driven commands through cmd_exec. Stamped once
  // by bleStampPairedByIfBlank and never overwritten in-session; to
  // re-assign ownership, clear the peer (bondrm or settings edit).
  // When blank, callers should treat the action as unauthenticated.
  String pairedByUser;
};

extern BlePeerData gBlePeerData[BLE_PEER_MAX];

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

// Register (or re-register) a peer. Idempotent: a second call with the
// same kind overwrites the first registration. Returns true on success;
// false if `kind >= BLE_PEER_MAX` or `spec.name` collides with a different
// kind already registered. Safe to call after settings have loaded — the
// registry just binds the spec; gBlePeerData is already populated.
bool bleRegisterPeer(const BlePeerSpec& spec);

// Register the built-in metadata-only peers (currently just "phone") that
// don't have a dedicated owning module. Call once early in boot, before
// any module that wants `phone` to appear in `blepeers` runs. Idempotent.
void bleRegisterBuiltinPeers(void);

// True if the given kind is currently registered.
bool bleIsPeerRegistered(BlePeerKind kind);

// Lookup by kind / name. Returns nullptr if not registered.
const BlePeerSpec* bleFindPeer(BlePeerKind kind);
const BlePeerSpec* bleFindPeerByName(const char* name);

// Iterate all registered peers in registration order. The pointer is
// stable until deregistration (which we don't currently support).
size_t              bleRegisteredPeerCount(void);
const BlePeerSpec*  bleRegisteredPeerAt(size_t i);

// -----------------------------------------------------------------------------
// MAC auto-save (called by the owning module on connect success)
// -----------------------------------------------------------------------------

// Persist the peer's MAC(s). For single-MAC peers, leave mac2 empty. The
// helper writes via setSetting (which is a no-op when the values match
// what's already stored), so calling on every connect is cheap.
void bleSavePeerMac(BlePeerKind kind,
                    const String& mac1,
                    const String& mac2 = String());

// Stamp pairedByUser if blank. Resolves identity in order:
//   1) calling task TLS user
//   2) live OLED / serial session (non-guest)
//   3) device founder (first users.json username)
// Idempotent when already owned. Only WARNs when no usable owner exists
// at all (pre-FTS). Call from pairing-intent sites (`openg2`,
// `bleautoreconnect on`, OLED connect, boot reconnect heal).
void bleStampPairedByIfBlank(BlePeerKind kind);

// -----------------------------------------------------------------------------
// Boot + runtime reconnect orchestrator
// -----------------------------------------------------------------------------

// True if any registered peer wants auto-reconnect at boot AND has a
// saved MAC to reconnect to. Used by HardwareOne.cpp to decide whether
// to bring up the BLE central stack at boot when bleAutoStart is
// off but a peer wants to come up.
bool bleAnyPeerWantsAutoReconnect(void);

// Iterate the registry and trigger connectSaved() for every peer whose
// autoReconnect flag is set AND that has a saved mac1. Each connect is
// non-blocking (spawns its own task), but we pace them so multiple
// peers don't fight the same BLE radio for scan windows: 2 s stagger
// between kicks. Call AFTER initG2Client() so peer modules have
// registered.
void bleBootReconnect(void);

// Mid-session drop recovery (when autoReconnect is on + saved MAC):
//   blePeerNoteUserDisconnect — call from intentional disconnect paths
//     (ringdisconnect / closeg2) so we do NOT reseek after a user tear-down.
//   blePeerNoteLinkLost — call from BLE onDisconnect when the link was up;
//     schedules a reconnect attempt if autoReconnect is on and the drop was
//     not user-initiated.
//   blePeerNoteLinkUp — call on connect success; clears backoff / user flag.
//   bleAutoReconnectTick — call from the main loop; fires due reconnects
//     with exponential backoff (5s → ~3 min).
void blePeerNoteUserDisconnect(BlePeerKind kind);
void blePeerNoteLinkLost(BlePeerKind kind);
void blePeerNoteLinkUp(BlePeerKind kind);
void bleAutoReconnectTick(void);

// One-shot / schedule a reseek even when autoReconnect is off (e.g. Health
// Track mine due while the ring is down). No-ops if the user intentionally
// disconnected, there is no saved MAC, or the peer is already linked.
// Non-blocking — the next bleAutoReconnectTick fires connectSaved.
void blePeerRequestReseek(BlePeerKind kind);

// -----------------------------------------------------------------------------
// CLI handlers
// -----------------------------------------------------------------------------

// `bleautoreconnect <peer-name> [on|off]`
//   no [on|off] → print the peer's current state + saved MACs.
//   with        → set autoReconnect flag, persist, return confirmation.
const char* cmd_bleautoreconnect(const String& argsInput);

// `blepeers` — print one line per registered peer:
//   <name> connect=<yes|no> auto=<on|off> mac1=<...> [mac2=<...>]
const char* cmd_blepeers(const String& argsInput);

// -----------------------------------------------------------------------------
// Settings JSON serialization
// -----------------------------------------------------------------------------
// The peers section nests one level deeper than the SettingsModule registry
// supports cleanly (bluetooth.peers.<name>.{mac1,mac2,autoReconnect}), so
// BLE_Peers handles its own load/save. Called inline from
// buildSettingsJsonDoc / readSettingsJson (see System_Settings.cpp).
#include <ArduinoJson.h>
void blePeersWriteJson(JsonDocument& doc);
void blePeersReadJson(JsonDocument& doc);

#else  // !ENABLE_BLUETOOTH

// Stubs so callers in non-BLE builds compile cleanly.
inline bool bleRegisterPeer(const struct BlePeerSpec&) { return false; }
inline bool bleAnyPeerWantsAutoReconnect(void)            { return false; }
inline void bleBootReconnect(void)                      {}
inline void bleSavePeerMac(int, const String&, const String& = String()) {}
inline void blePeerNoteUserDisconnect(int)              {}
inline void blePeerNoteLinkLost(int)                    {}
inline void blePeerNoteLinkUp(int)                      {}
inline void bleAutoReconnectTick(void)                  {}
inline void blePeerRequestReseek(int)                   {}

#endif  // ENABLE_BLUETOOTH
#endif  // BLE_PEERS_H
