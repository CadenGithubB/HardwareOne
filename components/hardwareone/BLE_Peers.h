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
#include "System_User.h"  // TransportSessionEpoch
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

// Result of asking a peer module to admit one saved-target connect attempt.
// This reports admission only; STARTED does not imply that the asynchronous
// BLE operation will ultimately connect.
enum class BlePeerConnectAdmission : uint8_t {
  STARTED = 0,
  COALESCED,
  BUSY,
  ALREADY_UP,
  NO_TARGET,
  ROLE_BLOCKED,
};

// Persisted peers currently store address text only. Keep the coordinator
// handoff fixed-size and allocation-free; address type 0 matches the ESP-IDF
// BLE_ADDR_TYPE_PUBLIC default, but `addressType*Known` remains false until the
// persistence schema records the observed type.
constexpr size_t BLE_PEER_ADDRESS_TEXT_CAPACITY = 18;  // 17 chars + NUL
constexpr uint8_t BLE_PEER_DEFAULT_ADDRESS_TYPE = 0;

struct BlePeerSavedTarget {
  char mac1[BLE_PEER_ADDRESS_TEXT_CAPACITY] = {};
  char mac2[BLE_PEER_ADDRESS_TEXT_CAPACITY] = {};
  uint8_t addressType1 = BLE_PEER_DEFAULT_ADDRESS_TYPE;
  uint8_t addressType2 = BLE_PEER_DEFAULT_ADDRESS_TYPE;
  bool addressType1Known = false;
  bool addressType2Known = false;
};

// Exact scheduler incarnation being offered to the peer coordinator. A peer
// must carry these generations into asynchronous work and reject/cancel work
// for which blePeerConnectRequestIsCurrent() becomes false.
struct BlePeerConnectRequest {
  uint32_t intentGeneration = 0;
  uint32_t identityGeneration = 0;
  bool autoReconnect = false;
  bool explicitReseek = false;
  BlePeerSavedTarget savedTarget;
};

using BlePeerConnectSavedAdmissionFn =
    BlePeerConnectAdmission (*)(const BlePeerConnectRequest& request);

// Per-peer connect ops, supplied by the owning module at registration.
// All callbacks must be safe to call from task context (CLI/web/main-loop/BLE
// host tasks). They must enqueue or coalesce real work and return quickly.
struct BlePeerOps {
  // Legacy admission callback. Kept as a compatibility bridge while peer
  // modules migrate to connectSavedAdmission below. true maps to STARTED;
  // false maps conservatively to BUSY.
  bool (*connectSaved)(void);
  // Tear down any active connection. Idempotent; safe when not connected.
  void (*disconnect)(void);
  // True when the peer is currently linked at the BLE level.
  bool (*isConnected)(void);
  // Preferred admission callback. When present, the scheduler uses this
  // explicit result and never calls the legacy bool callback. This trailing
  // field preserves existing three-entry aggregate initializers.
  BlePeerConnectSavedAdmissionFn connectSavedAdmission;
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

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

// Register (or re-register) a peer. Idempotent: a second call with the
// same kind overwrites the first registration. Returns true on success;
// false if `kind >= BLE_PEER_MAX` or `spec.name` collides with a different
// kind already registered. Safe to call after settings have loaded — the
// registry just binds the spec; the private persisted mirror is already loaded.
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
// Generation-conditioned variant for an asynchronous repair that discovered
// a previously missing address. It cannot overwrite a target/owner identity
// replaced after the repair was admitted.
bool bleSavePeerMacIfIdentityCurrent(
    BlePeerKind kind, uint32_t expectedIdentityGeneration,
    const String& mac1, const String& mac2 = String());

// Stamp pairedByUser if blank. Resolves identity in order:
//   1) calling task TLS user
//   2) live OLED / serial session (non-guest)
//   3) device founder (first users.json username)
// Idempotent when already owned. Only WARNs when no usable owner exists
// at all (pre-FTS). Call only from explicit authenticated pairing-intent sites
// (`openg2`, `bleautoreconnect on`, OLED connect); never from boot/automatic
// saved-MAC recovery.
void bleStampPairedByIfBlank(BlePeerKind kind);

// Coherent runtime authority for a peer that acts on behalf of its owner.
// `generation` changes on every owner publication/clear. `transportEpoch` is
// the central boot-local session token used by queued G2 commands; it is zero
// for an unowned peer (and for peer kinds that do not execute commands).
struct BlePeerOwnerSession {
  String user;
  uint32_t generation = 0;
  TransportSessionEpoch transportEpoch = kNoTransportSessionEpoch;

  bool live() const {
    return user.length() > 0 && generation != 0 && transportEpoch != 0;
  }
};

// Snapshot owner + generation + central transport epoch under one lock.
bool blePeerOwnerSessionSnapshot(BlePeerKind kind,
                                 BlePeerOwnerSession& out);

// Exact post-snapshot fence. All four values must still describe the same
// live owner incarnation. This is used by G2 completion/revocation paths in
// addition to cmd_exec's central transport-session fence.
bool blePeerOwnerSessionIsCurrent(BlePeerKind kind,
                                  const BlePeerOwnerSession& expected);

// Compare-and-clear: succeeds only if the exact owner incarnation captured in
// `expected` is still current. Used by account revoke/ban paths so they cannot
// accidentally clear a replacement owner that won a concurrent publication.
bool blePeerOwnerSessionClearIfCurrent(
    BlePeerKind kind, const BlePeerOwnerSession& expected);

// Clear the persistent owner and close its central authority epoch. The peer
// lock is released before settings are written, so this is safe from user/
// settings mutation paths. Returns true only when state changed.
bool blePeerOwnerSessionClear(BlePeerKind kind);

// -----------------------------------------------------------------------------
// Boot + runtime reconnect orchestrator
// -----------------------------------------------------------------------------

// True if any persisted known peer wants auto-reconnect at boot, has a saved
// MAC, and already has owner authority. Used by HardwareOne.cpp to decide
// whether to bring up the BLE central stack at boot when bleAutoStart is off
// but a peer wants to come up.
bool bleAnyPeerWantsAutoReconnect(void);

// Iterate the registry and request saved-target admission for every peer whose
// autoReconnect flag is set, whose owner authority already exists, and that
// has at least one usable saved target. Requests are non-blocking and newly
// STARTED attempts are paced by 2 s. Failed/coalesced admissions remain
// scheduled for the periodic tick; boot recovery never manufactures peer
// ownership. Call AFTER initG2Client() so peer modules have registered.
void bleBootReconnect(void);

// Mid-session drop recovery (when autoReconnect is on + saved MAC):
//   blePeerNoteUserDisconnect — call from intentional disconnect paths
//     (ringdisconnect / closeg2) so we do NOT reseek after a user tear-down.
//   blePeerNoteLinkLost — call from BLE onDisconnect when the link was up;
//     schedules a reconnect attempt if autoReconnect is on and the drop was
//     not user-initiated.
//   blePeerNoteUserConnectIntent — call when an authenticated user explicitly
//     starts a manual pairing/connect; clears older disconnect suppression and
//     invalidates queued automatic work before the async operation begins.
//   blePeerNoteLinkUp — call on a manual/untracked connect success; clears
//     backoff only if no newer user disconnect superseded that operation.
//   blePeerNoteLinkUpIfCurrent — scheduler-worker completion variant; commits
//     only if the exact admitted request is still current, so a late success
//     cannot undo a newer user disconnect or target/owner change.
//   bleAutoReconnectTick — call from the main loop; fires due reconnects
//     with exponential backoff (5s → ~3 min).
void blePeerNoteUserDisconnect(BlePeerKind kind);
void blePeerNoteUserConnectIntent(BlePeerKind kind);
void blePeerNoteLinkLost(BlePeerKind kind);
void blePeerNoteLinkUp(BlePeerKind kind);
bool blePeerNoteLinkUpIfCurrent(
    BlePeerKind kind, const BlePeerConnectRequest& request);
// Finite/manual repair fence. Unlike a scheduler request, a half-pair repair
// is allowed while autoReconnect is off, but it must still be cancelled by a
// newer user disconnect, owner/target replacement, or manual intent.
bool blePeerIntentIsCurrent(BlePeerKind kind, uint32_t intentGeneration,
                            uint32_t identityGeneration);
bool blePeerNoteLinkUpIfIntentCurrent(BlePeerKind kind,
                                      uint32_t intentGeneration,
                                      uint32_t identityGeneration);
// Begin a manual/name-or-MAC discovery which may learn a new saved target.
// The returned generations fence the asynchronous job against a later user
// disconnect, owner revocation, or target replacement without requiring a
// saved target to exist yet.
bool blePeerBeginManualLearn(BlePeerKind kind,
                             uint32_t& intentGeneration,
                             uint32_t& identityGeneration);
// Atomically validate a manual-learning fence, apply the discovered address,
// and (only for a complete logical topology) consume reconnect intent. The
// caller persists after releasing its family completion mutex when requested.
bool blePeerCommitLearnedTargetIfCurrent(
    BlePeerKind kind, uint32_t intentGeneration,
    uint32_t identityGeneration, const String& mac1,
    const String& mac2, uint8_t replaceMask, bool completeTopology,
    bool* persistNeeded = nullptr);
// Atomically validates a finite repair's intent+identity, applies any newly
// discovered MAC, and commits LinkUp under the PeerData→reconnect lock order.
// No separate policy or persistence window remains after this returns true.
bool blePeerCommitRepairIfCurrent(
    BlePeerKind kind, uint32_t intentGeneration,
    uint32_t identityGeneration, const String& mac1,
    const String& mac2 = String(), bool* persistNeeded = nullptr);
void bleAutoReconnectTick(void);

// One-shot / schedule a reseek even when autoReconnect is off (e.g. Health
// Track mine due while the ring is down). No-ops if the user intentionally
// disconnected, there is no saved MAC, or the peer is already linked.
// Non-blocking — the next bleAutoReconnectTick fires connectSaved.
void blePeerRequestReseek(BlePeerKind kind);

// Synchronized scheduler snapshot. intentGeneration changes whenever user or
// link intent is created, replaced, or cancelled. Handing an admitted one-shot
// to its asynchronous job does not invalidate that job's generation.
// identityGeneration changes when
// the saved target or owner-authority incarnation changes. Callers can use the
// pair as an ABA fence around asynchronous work; zero means not initialized.
struct BlePeerReconnectSnapshot {
  uint32_t intentGeneration = 0;
  uint32_t identityGeneration = 0;
  bool userDisconnect = false;
  bool wantsReconnect = false;
  bool explicitReseek = false;
  bool autoReconnect = false;
  bool hasSavedTarget = false;
  bool ownerAuthorityAvailable = false;
  bool admissionInFlight = false;
  uint8_t launchedAttempts = 0;
  uint32_t dueMs = 0;
};

bool blePeerReconnectSnapshot(BlePeerKind kind,
                              BlePeerReconnectSnapshot& out);
bool blePeerAutoReconnectEnabled(BlePeerKind kind);

// Immutable saved-target snapshot copied from the same synchronized runtime
// publication used by reconnect admission. A valid kind returns true even if
// no target is configured; single-address peers test target.mac1[0], while G2
// may legitimately have only target.mac2[0] for a right-temple-only pairing.
struct BlePeerSavedTargetSnapshot {
  uint32_t identityGeneration = 0;
  BlePeerSavedTarget target;
};

bool blePeerSavedTargetSnapshot(BlePeerKind kind,
                                BlePeerSavedTargetSnapshot& out);

// True while an admitted asynchronous request still names the current saved
// target/owner/user intent. Fails on owner or target loss, identity change,
// user disconnect, and (for persistent requests) autoReconnect disable. An
// admitted one-shot remains current after the scheduler hands it off; a later
// user/link/config event advances its generation and fences it.
bool blePeerConnectRequestIsCurrent(
    BlePeerKind kind, const BlePeerConnectRequest& request);

// Start an explicit saved-target connect as one generation-fenced request.
// This atomically clears older user suppression, invalidates scheduler work,
// and snapshots the current owner/target incarnation into `out`.
bool blePeerBeginManualConnectRequest(
    BlePeerKind kind, BlePeerConnectRequest& out);

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
inline void blePeerNoteUserConnectIntent(int)           {}
inline void blePeerNoteLinkLost(int)                    {}
inline void blePeerNoteLinkUp(int)                      {}
inline bool blePeerIntentIsCurrent(int, uint32_t, uint32_t) { return false; }
inline bool blePeerNoteLinkUpIfIntentCurrent(int, uint32_t, uint32_t) { return false; }
inline bool blePeerBeginManualLearn(int, uint32_t&, uint32_t&) { return false; }
inline bool blePeerCommitLearnedTargetIfCurrent(
    int, uint32_t, uint32_t, const String&, const String&, uint8_t, bool,
    bool* = nullptr) { return false; }
inline bool bleSavePeerMacIfIdentityCurrent(int, uint32_t,
                                             const String&,
                                             const String& = String()) { return false; }
inline bool blePeerCommitRepairIfCurrent(int, uint32_t, uint32_t,
                                          const String&,
                                          const String& = String(),
                                          bool* = nullptr) { return false; }
inline void bleAutoReconnectTick(void)                  {}
inline void blePeerRequestReseek(int)                   {}
inline bool blePeerAutoReconnectEnabled(int)            { return false; }

#endif  // ENABLE_BLUETOOTH
#endif  // BLE_PEERS_H
