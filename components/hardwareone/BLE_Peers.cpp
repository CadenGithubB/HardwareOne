// =============================================================================
// BLE_Peers.cpp — runtime peer registry implementation
// =============================================================================
//
// IDENTITY-GENERATION BUMP SITE
// -----------------------------
// `bleStampPairedByIfBlank` bumps gIdentityGeneration when it successfully
// stamps a previously-empty pairedByUser. That signals to consumers (e.g.
// FileManager's directory cache) that the G2 hijack identity just gained
// a real user, and any cache that was filled under the previous (anon /
// blank) identity should re-fill. The bump is one line, immediately after
// setSetting succeeds — see CASE C inside the helper. The corresponding
// consumer protocol lives in System_AuthIdentity.h.

#include "BLE_Peers.h"

#if ENABLE_BLUETOOTH

#include "System_Settings.h"   // setSetting, persisted peer rows
#include "Bluetooth.h"         // live server/client role predicates
#include "System_Debug.h"
#include "System_Utils.h"      // RETURN_VALID_IF_VALIDATE_CSTR, parseBoolArg
#include "System_Command.h"    // CommandEntry, ensureDebugBuffer, getDebugBuffer
#include "System_MemUtil.h"    // PSRAM_JSON_DOC
#include <ArduinoJson.h>
#include "System_User.h"
#include "System_AuthIdentity.h"  // currentAuthContext + bumpIdentityGeneration

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

// -----------------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------------

struct BlePeerData {
  String mac1;
  String mac2;
  bool autoReconnect = false;
};
// PSRAM: every access holds the recursive PeerDataGuard mutex (task context
// only), and peerConfigApply snapshots effAuto before taking sReconnectMux,
// so no byte of this array is read or written inside a spinlock window.
// The rollback writes in the bail paths run AFTER portEXIT — keep it that way.
static EXT_RAM_BSS_ATTR BlePeerData gBlePeerData[BLE_PEER_MAX] = {};

namespace {

// Arduino String fields are retained only as the settings-file compatibility
// mirror. Runtime policy/targets live in fixed storage below. Guard every
// mirror mutation/snapshot with a task-level mutex; never hold it across flash
// or filesystem I/O.
StaticSemaphore_t sPeerDataMutexStorage;
SemaphoreHandle_t sPeerDataMutex = nullptr;
portMUX_TYPE sPeerDataInitMux = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t peerDataMutex() {
  portENTER_CRITICAL(&sPeerDataInitMux);
  if (!sPeerDataMutex) {
    sPeerDataMutex =
        xSemaphoreCreateRecursiveMutexStatic(&sPeerDataMutexStorage);
  }
  SemaphoreHandle_t mutex = sPeerDataMutex;
  portEXIT_CRITICAL(&sPeerDataInitMux);
  return mutex;
}

class PeerDataGuard {
 public:
  explicit PeerDataGuard(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = peerDataMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~PeerDataGuard() {
    if (locked_) xSemaphoreGiveRecursive(sPeerDataMutex);
  }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_ = false;
};

struct PeerReconnectRuntime {
  bool userDisconnect = false;
  bool wantReconnect = false;
  bool reseekEvenIfNoAuto = false;
  bool autoReconnect = false;
  bool hasSavedTarget = false;
  bool ownerAuthorityAvailable = false;
  bool admissionInFlight = false;
  uint8_t launchedAttempts = 0;
  uint32_t dueMs = 0;
  uint32_t intentGeneration = 0;
  uint32_t identityGeneration = 0;
  uint32_t ownerGeneration = 0;
  BlePeerSavedTarget savedTarget;
};

PeerReconnectRuntime sReconnect[BLE_PEER_MAX];
portMUX_TYPE sReconnectMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t nextReconnectGeneration(uint32_t current) {
  do {
    ++current;
  } while (current == 0);
  return current;
}

void reconnectBumpIntentLocked(PeerReconnectRuntime& state) {
  state.intentGeneration = nextReconnectGeneration(state.intentGeneration);
}

void reconnectBumpIdentityLocked(PeerReconnectRuntime& state) {
  state.identityGeneration =
      nextReconnectGeneration(state.identityGeneration);
}

// Publish fixed-size mirrors of persisted reconnect configuration. Arduino
// Strings remain the persistence representation, but callback/main-loop
// scheduling never reads them concurrently after this publication.
bool copySavedAddress(const String& source,
                      char (&destination)[BLE_PEER_ADDRESS_TEXT_CAPACITY]) {
  destination[0] = '\0';
  if (source.length() == 0) return true;
  if (source.length() >= BLE_PEER_ADDRESS_TEXT_CAPACITY) return false;
  strlcpy(destination, source.c_str(), sizeof(destination));
  return true;
}

struct PeerConfigApplyResult {
  bool applied = false;
  bool targetChanged = false;
  bool policyChanged = false;
  uint32_t identityGeneration = 0;
};

enum class PeerIntentAction : uint8_t {
  Preserve = 0,
  UserDisconnect,
  UserConnect,
};

// One coherent persistence/runtime transaction. Every target or auto-policy
// writer takes PeerDataGuard first and publishes the fixed scheduler mirror
// before releasing it. Readers therefore observe either the old tuple or the
// new tuple; they can no longer combine a new persisted MAC with an old
// reconnect policy (or vice versa).
PeerConfigApplyResult peerConfigApply(
    BlePeerKind kind,
    bool replaceMac1, const String& requestedMac1,
    bool replaceMac2, const String& requestedMac2,
    bool replaceAutoReconnect, bool requestedAutoReconnect,
    uint32_t expectedIdentityGeneration = 0,
    PeerIntentAction intentAction = PeerIntentAction::Preserve,
    uint32_t expectedOwnerGeneration = 0,
    bool requireOwnerAuthority = false) {
  PeerConfigApplyResult result;
  if (kind >= BLE_PEER_MAX) return result;
  PeerDataGuard guard;
  if (!guard) return result;

  BlePeerData& data = gBlePeerData[kind];
  const String oldMac1 = data.mac1;
  const String oldMac2 = data.mac2;
  const bool oldAutoReconnect = data.autoReconnect;
  if (replaceMac1) data.mac1 = requestedMac1;
  if (replaceMac2) data.mac2 = requestedMac2;
  if (replaceAutoReconnect) data.autoReconnect = requestedAutoReconnect;
  // gBlePeerData lives in PSRAM: capture the effective (post-update) policy
  // before portENTER_CRITICAL so the spinlock window never touches external
  // RAM. Must stay AFTER the conditional write above.
  const bool effAuto = data.autoReconnect;

  BlePeerSavedTarget target;
  const bool mac1Representable = copySavedAddress(data.mac1, target.mac1);
  const bool mac2Representable = copySavedAddress(data.mac2, target.mac2);
  // G2 can legitimately have only one temple persisted. The right temple is
  // stored in mac2, so treating mac1 as the universal gate strands a
  // right-only pairing even though the coordinator can reconnect it.
  const bool hasSavedTarget =
      (mac1Representable && target.mac1[0] != '\0') ||
      (kind == BLE_PEER_G2_GLASSES && mac2Representable &&
       target.mac2[0] != '\0');
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if ((expectedIdentityGeneration != 0 &&
       state.identityGeneration != expectedIdentityGeneration) ||
      (expectedOwnerGeneration != 0 &&
       state.ownerGeneration != expectedOwnerGeneration) ||
      (requireOwnerAuthority && !state.ownerAuthorityAvailable)) {
    portEXIT_CRITICAL(&sReconnectMux);
    data.mac1 = oldMac1;
    data.mac2 = oldMac2;
    data.autoReconnect = oldAutoReconnect;
    return result;
  }
  const bool firstIdentity = state.identityGeneration == 0;
  result.targetChanged =
      memcmp(state.savedTarget.mac1, target.mac1,
             sizeof(target.mac1)) != 0 ||
      memcmp(state.savedTarget.mac2, target.mac2,
             sizeof(target.mac2)) != 0 ||
      state.savedTarget.addressType1 != target.addressType1 ||
      state.savedTarget.addressType2 != target.addressType2 ||
      state.savedTarget.addressType1Known != target.addressType1Known ||
      state.savedTarget.addressType2Known != target.addressType2Known;
  result.policyChanged = state.autoReconnect != effAuto;
  state.autoReconnect = effAuto;
  state.hasSavedTarget = hasSavedTarget;
  state.savedTarget = target;
  if (!hasSavedTarget ||
      (!effAuto && !state.reseekEvenIfNoAuto)) {
    state.wantReconnect = false;
    state.launchedAttempts = 0;
    state.dueMs = 0;
    if (!hasSavedTarget) state.reseekEvenIfNoAuto = false;
  }
  if (result.targetChanged || firstIdentity) reconnectBumpIdentityLocked(state);
  if (intentAction == PeerIntentAction::UserDisconnect) {
    state.userDisconnect = true;
    state.wantReconnect = false;
    state.reseekEvenIfNoAuto = false;
    state.launchedAttempts = 0;
    state.dueMs = 0;
  } else if (intentAction == PeerIntentAction::UserConnect) {
    state.userDisconnect = false;
    state.wantReconnect = false;
    state.reseekEvenIfNoAuto = false;
    state.launchedAttempts = 0;
    state.dueMs = 0;
  }
  if (intentAction != PeerIntentAction::Preserve || result.policyChanged ||
      state.intentGeneration == 0) {
    reconnectBumpIntentLocked(state);
  }
  result.identityGeneration = state.identityGeneration;
  result.applied = true;
  portEXIT_CRITICAL(&sReconnectMux);
  if (!mac1Representable || !mac2Representable) {
    WARN_BLUETOOTHF("[BLE-Peers] saved address for kind=%u exceeds %u text bytes; reconnect target rejected",
                    (unsigned)kind,
                    (unsigned)(BLE_PEER_ADDRESS_TEXT_CAPACITY - 1));
  }
  return result;
}

void reconnectPublishOwnerAuthority(BlePeerKind kind, bool available,
                                    uint32_t ownerGeneration) {
  if (kind >= BLE_PEER_MAX) return;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (state.ownerAuthorityAvailable != available ||
      state.ownerGeneration != ownerGeneration ||
      state.identityGeneration == 0) {
    state.ownerAuthorityAvailable = available;
    state.ownerGeneration = ownerGeneration;
    reconnectBumpIdentityLocked(state);
    if (!available) {
      state.userDisconnect = true;
      state.wantReconnect = false;
      state.reseekEvenIfNoAuto = false;
      state.launchedAttempts = 0;
      state.dueMs = 0;
      reconnectBumpIntentLocked(state);
    }
  }
  portEXIT_CRITICAL(&sReconnectMux);
}

// pairedByUser is persisted as an Arduino String for settings compatibility,
// but it is also an execution authority read from several tasks. Keep the
// runtime authority in fixed storage behind one mutex; the String is only a
// mirror updated under that same mutex and serialized via snapshots below.
// No peer-owner lock is ever held across writeSettingsJson()/user-database I/O.
struct PeerOwnerAuthority {
  char user[kPublicUsernameMaxLen + 1] = {};
  uint32_t generation = 0;
  TransportSessionEpoch transportEpoch = kNoTransportSessionEpoch;
};

// PSRAM: every access takes the recursive PeerOwnerGuard mutex itself (verified
// 2026-08-19). The adjacent mutex storage + init spinlock MUST stay internal.
EXT_RAM_BSS_ATTR PeerOwnerAuthority sPeerOwner[BLE_PEER_MAX];
StaticSemaphore_t sPeerOwnerMutexStorage;
SemaphoreHandle_t sPeerOwnerMutex = nullptr;
portMUX_TYPE sPeerOwnerInitMux = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t peerOwnerMutex() {
  SemaphoreHandle_t mutex = nullptr;
  portENTER_CRITICAL(&sPeerOwnerInitMux);
  if (!sPeerOwnerMutex) {
    sPeerOwnerMutex =
        xSemaphoreCreateRecursiveMutexStatic(&sPeerOwnerMutexStorage);
  }
  mutex = sPeerOwnerMutex;
  portEXIT_CRITICAL(&sPeerOwnerInitMux);
  return mutex;
}

class PeerOwnerGuard {
 public:
  explicit PeerOwnerGuard(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = peerOwnerMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~PeerOwnerGuard() {
    if (locked_) xSemaphoreGiveRecursive(sPeerOwnerMutex);
  }
  explicit operator bool() const { return locked_; }

 private:
  bool locked_ = false;
};

uint32_t nextPeerOwnerGeneration(uint32_t current) {
  do {
    ++current;
  } while (current == 0);
  return current;
}

// Publish one owner incarnation. The caller has already resolved/validated
// `user`; this function performs no filesystem work. G2 is the only peer kind
// that currently submits commands, so it alone consumes a central transport
// session slot. Returns true only when the authority actually changed.
bool peerOwnerPublish(BlePeerKind kind, const String& user,
                      bool onlyIfBlank) {
  if (kind >= BLE_PEER_MAX || user.length() > kPublicUsernameMaxLen) {
    return false;
  }
  PeerOwnerGuard guard;
  if (!guard) return false;

  PeerOwnerAuthority& authority = sPeerOwner[kind];
  if (onlyIfBlank && authority.user[0] != '\0') return false;

  const bool sameUser = user == authority.user;
  const bool liveG2Epoch =
      kind == BLE_PEER_G2_GLASSES &&
      authority.transportEpoch != kNoTransportSessionEpoch &&
      transportSessionEpochIsLive(SOURCE_G2_GLASSES,
                                  authority.transportEpoch);
  if (sameUser &&
      (user.length() == 0 || kind != BLE_PEER_G2_GLASSES || liveG2Epoch)) {
    return false;
  }

  const TransportSessionEpoch oldEpoch = authority.transportEpoch;
  authority.transportEpoch = kNoTransportSessionEpoch;
  transportSessionClose(SOURCE_G2_GLASSES, oldEpoch);

  authority.generation = nextPeerOwnerGeneration(authority.generation);
  strlcpy(authority.user, user.c_str(), sizeof(authority.user));
  if (kind == BLE_PEER_G2_GLASSES && user.length() > 0) {
    authority.transportEpoch = transportSessionOpen(SOURCE_G2_GLASSES);
  }
  const bool authorityAvailable =
      user.length() > 0 &&
      (kind != BLE_PEER_G2_GLASSES ||
       authority.transportEpoch != kNoTransportSessionEpoch);
  reconnectPublishOwnerAuthority(kind, authorityAvailable,
                                 authority.generation);
  return true;
}

void peerOwnerPersistAfterUnlock(BlePeerKind kind, const char* reason) {
  // Deliberately outside PeerOwnerGuard: writeSettingsJson() takes FsLock and
  // calls blePeersWriteJson(), which takes a fresh owner snapshot.
  if (!gDeferWrites) (void)writeSettingsJson();
  bumpIdentityGeneration(reason ? reason : "ble.peer_owner");
}

bool savedTargetMatches(const BlePeerSavedTarget& left,
                        const BlePeerSavedTarget& right) {
  return memcmp(left.mac1, right.mac1, sizeof(left.mac1)) == 0 &&
         memcmp(left.mac2, right.mac2, sizeof(left.mac2)) == 0 &&
         left.addressType1 == right.addressType1 &&
         left.addressType2 == right.addressType2 &&
         left.addressType1Known == right.addressType1Known &&
         left.addressType2Known == right.addressType2Known;
}

bool connectRequestIsCurrentLocked(
    const PeerReconnectRuntime& state,
    const BlePeerConnectRequest& request) {
  const bool policyStillAllows =
      request.autoReconnect ? state.autoReconnect : request.explicitReseek;
  return state.intentGeneration == request.intentGeneration &&
         state.identityGeneration == request.identityGeneration &&
         policyStillAllows && !state.userDisconnect &&
         state.hasSavedTarget && state.ownerAuthorityAvailable &&
         savedTargetMatches(state.savedTarget, request.savedTarget);
}

void noteLinkUpLocked(PeerReconnectRuntime& state) {
  state.wantReconnect = false;
  state.reseekEvenIfNoAuto = false;
  state.launchedAttempts = 0;
  state.dueMs = 0;
  reconnectBumpIntentLocked(state);
}
}  // namespace

bool blePeerReconnectSnapshot(BlePeerKind kind,
                              BlePeerReconnectSnapshot& out) {
  out = BlePeerReconnectSnapshot{};
  if (kind >= BLE_PEER_MAX) return false;
  portENTER_CRITICAL(&sReconnectMux);
  const PeerReconnectRuntime& state = sReconnect[kind];
  out.intentGeneration = state.intentGeneration;
  out.identityGeneration = state.identityGeneration;
  out.userDisconnect = state.userDisconnect;
  out.wantsReconnect = state.wantReconnect;
  out.explicitReseek = state.reseekEvenIfNoAuto;
  out.autoReconnect = state.autoReconnect;
  out.hasSavedTarget = state.hasSavedTarget;
  out.ownerAuthorityAvailable = state.ownerAuthorityAvailable;
  out.admissionInFlight = state.admissionInFlight;
  out.launchedAttempts = state.launchedAttempts;
  out.dueMs = state.dueMs;
  portEXIT_CRITICAL(&sReconnectMux);
  return true;
}

bool blePeerAutoReconnectEnabled(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return false;
  portENTER_CRITICAL(&sReconnectMux);
  const bool enabled = sReconnect[kind].autoReconnect;
  portEXIT_CRITICAL(&sReconnectMux);
  return enabled;
}

bool blePeerSavedTargetSnapshot(BlePeerKind kind,
                                BlePeerSavedTargetSnapshot& out) {
  out = BlePeerSavedTargetSnapshot{};
  if (kind >= BLE_PEER_MAX) return false;
  portENTER_CRITICAL(&sReconnectMux);
  const PeerReconnectRuntime& state = sReconnect[kind];
  out.identityGeneration = state.identityGeneration;
  out.target = state.savedTarget;
  portEXIT_CRITICAL(&sReconnectMux);
  return true;
}

bool blePeerConnectRequestIsCurrent(
    BlePeerKind kind, const BlePeerConnectRequest& request) {
  if (kind >= BLE_PEER_MAX || request.intentGeneration == 0 ||
      request.identityGeneration == 0 ||
      (!request.autoReconnect && !request.explicitReseek)) {
    return false;
  }
  bool current = false;
  portENTER_CRITICAL(&sReconnectMux);
  const PeerReconnectRuntime& state = sReconnect[kind];
  current = connectRequestIsCurrentLocked(state, request);
  portEXIT_CRITICAL(&sReconnectMux);
  return current;
}

bool blePeerBeginManualConnectRequest(
    BlePeerKind kind, BlePeerConnectRequest& out) {
  out = BlePeerConnectRequest{};
  if (kind >= BLE_PEER_MAX) return false;

  bool admitted = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (state.hasSavedTarget && state.ownerAuthorityAvailable) {
    // A manual saved-target request supersedes any scheduler incarnation and
    // carries the exact target/identity it admitted. A later OFF, disconnect,
    // owner change, or target replacement advances one of these generations
    // and makes the queued job harmless.
    state.userDisconnect = false;
    // Preserve persistent retry intent until the asynchronous job actually
    // succeeds. If queue allocation/admission fails after this snapshot, the
    // normal scheduler must still have work to retry instead of silently
    // stranding a down peer. A manual gesture also re-enables retry after an
    // older explicit disconnect when auto-reconnect remains configured.
    if (state.autoReconnect && !state.wantReconnect) {
      state.wantReconnect = true;
      state.dueMs = millis() + 5000;
    }
    reconnectBumpIntentLocked(state);
    out.intentGeneration = state.intentGeneration;
    out.identityGeneration = state.identityGeneration;
    out.autoReconnect = false;
    out.explicitReseek = true;
    out.savedTarget = state.savedTarget;
    admitted = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return admitted;
}

bool blePeerOwnerSessionSnapshot(BlePeerKind kind,
                                 BlePeerOwnerSession& out) {
  out = BlePeerOwnerSession{};
  if (kind >= BLE_PEER_MAX) return false;
  PeerOwnerGuard guard;
  if (!guard) return false;
  const PeerOwnerAuthority& authority = sPeerOwner[kind];
  out.user = authority.user;
  out.generation = authority.generation;
  out.transportEpoch = authority.transportEpoch;
  return true;
}

bool blePeerOwnerSessionIsCurrent(BlePeerKind kind,
                                  const BlePeerOwnerSession& expected) {
  if (kind >= BLE_PEER_MAX || !expected.live()) return false;
  bool matches = false;
  {
    PeerOwnerGuard guard;
    if (!guard) return false;
    const PeerOwnerAuthority& authority = sPeerOwner[kind];
    matches = authority.generation == expected.generation &&
              authority.transportEpoch == expected.transportEpoch &&
              expected.user == authority.user;
  }
  return matches &&
         transportSessionEpochIsLive(SOURCE_G2_GLASSES,
                                     expected.transportEpoch);
}

bool blePeerOwnerSessionClearIfCurrent(
    BlePeerKind kind, const BlePeerOwnerSession& expected) {
  if (kind >= BLE_PEER_MAX || !expected.live()) return false;
  {
    PeerOwnerGuard guard;
    if (!guard) return false;
    PeerOwnerAuthority& authority = sPeerOwner[kind];
    if (authority.generation != expected.generation ||
        authority.transportEpoch != expected.transportEpoch ||
        expected.user != authority.user) {
      return false;
    }

    const TransportSessionEpoch oldEpoch = authority.transportEpoch;
    authority.transportEpoch = kNoTransportSessionEpoch;
    transportSessionClose(SOURCE_G2_GLASSES, oldEpoch);
    authority.generation = nextPeerOwnerGeneration(authority.generation);
    authority.user[0] = '\0';
    reconnectPublishOwnerAuthority(kind, false, authority.generation);
  }
  peerOwnerPersistAfterUnlock(kind, "ble.cas_clear.pairedByUser");
  return true;
}

bool blePeerOwnerSessionClear(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return false;
  const bool changed = peerOwnerPublish(kind, String(), false);
  if (changed) {
    peerOwnerPersistAfterUnlock(kind, "ble.clear.pairedByUser");
  }
  return changed;
}

// Registration table — indexed by kind. Slots are nullptr until
// bleRegisterPeer fills them. We keep the specs themselves stable
// (caller-owned, typically a static const) and just store pointers.
static const BlePeerSpec* gPeerByKind[BLE_PEER_MAX] = { nullptr };

// Insertion-ordered list for iteration. Up to BLE_PEER_MAX entries.
static const BlePeerSpec* gPeerInOrder[BLE_PEER_MAX] = { nullptr };
static size_t             gPeerCount               = 0;

// -----------------------------------------------------------------------------
// Built-in metadata-only peers
// -----------------------------------------------------------------------------
// Phone has no owning module yet — when phone integration arrives, this
// registration moves to that module. Until then, BLE_Peers owns the spec
// so the peer appears in `blepeers` and the JSON schema with macCount=1.

static const BlePeerSpec kPhonePeerSpec = {
  BLE_PEER_PHONE,
  "phone",
  "Phone",
  /*macCount=*/1,
  /*connectable=*/false,   // no connect ops yet
  /*ops=*/nullptr,
};

void bleRegisterBuiltinPeers(void) {
  bleRegisterPeer(kPhonePeerSpec);
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

bool bleRegisterPeer(const BlePeerSpec& spec) {
  if (spec.kind >= BLE_PEER_MAX) {
    DEBUG_G2F("[BLE-Peers] Reject: kind=%u out of range", (unsigned)spec.kind);
    return false;
  }
  if (!spec.name) {
    DEBUG_G2F("[BLE-Peers] Reject: kind=%u name is null", (unsigned)spec.kind);
    return false;
  }
  // Name collision check: a different kind already using this name?
  for (size_t i = 0; i < gPeerCount; i++) {
    if (gPeerInOrder[i] &&
        gPeerInOrder[i]->kind != spec.kind &&
        strcmp(gPeerInOrder[i]->name, spec.name) == 0) {
      DEBUG_G2F("[BLE-Peers] Reject: name '%s' already used by kind=%u",
                spec.name, (unsigned)gPeerInOrder[i]->kind);
      return false;
    }
  }

  const bool firstTime = (gPeerByKind[spec.kind] == nullptr);
  gPeerByKind[spec.kind] = &spec;
  if (firstTime && gPeerCount < BLE_PEER_MAX) {
    gPeerInOrder[gPeerCount++] = &spec;
  }
  DEBUG_G2F("[BLE-Peers] Registered '%s' (kind=%u, %s%s, macCount=%u)",
            spec.name, (unsigned)spec.kind,
            spec.connectable ? "connectable" : "metadata-only",
            firstTime ? "" : ", re-reg",
            (unsigned)spec.macCount);
  return true;
}

bool bleIsPeerRegistered(BlePeerKind kind) {
  return (kind < BLE_PEER_MAX) && (gPeerByKind[kind] != nullptr);
}

const BlePeerSpec* bleFindPeer(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return nullptr;
  return gPeerByKind[kind];
}

const BlePeerSpec* bleFindPeerByName(const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < gPeerCount; i++) {
    if (gPeerInOrder[i] && strcmp(gPeerInOrder[i]->name, name) == 0) {
      return gPeerInOrder[i];
    }
  }
  return nullptr;
}

size_t bleRegisteredPeerCount(void) { return gPeerCount; }

const BlePeerSpec* bleRegisteredPeerAt(size_t i) {
  return (i < gPeerCount) ? gPeerInOrder[i] : nullptr;
}

// -----------------------------------------------------------------------------
// MAC auto-save
// -----------------------------------------------------------------------------

// Stamp the running user's identity onto the peer record if it isn't already
// set. Callers must be explicit authenticated ownership gestures such as
// `bleautoreconnect <peer> on`, `openg2`, or an OLED scan-and-pair action.
// bleSavePeerMac deliberately does not call this helper because it also runs
// from anonymous automatic reconnect workers.
// Idempotent: once a user owns the peer, re-pairing under a different
// account doesn't silently transfer ownership. To re-assign, clear the
// peer first (e.g. via bondrm or settings edit). This avoids surprise
// privilege swaps if a non-admin briefly handles the device.
// Resolve who should own a peer when stamping. Prefer the calling task's
// TLS identity, then live serial/OLED sessions, then the device founder.
// Never returns guests or synthetic names (AuthBypass / bond-admin).
static String bleResolveStampUsername(BlePeerKind kind) {
  String who = currentAuthContext().user;
  auto usable = [](const String& u) -> bool {
    if (u.length() == 0) return false;
    if (u.equalsIgnoreCase("AuthBypass")) return false;
    if (u.equalsIgnoreCase(kBondAdminUser)) return false;
    // Positive account resolution is intentional. `isGuestUser()` falls back
    // to ordinary User when users.json is unavailable; pair ownership is
    // persistent authority, so unknown/malformed/missing identities fail
    // closed just like targeted session control.
    String role;
    return getUserAuthorizationRole(u, role) && role != "guest";
  };

  // The UART host link is a machine channel: its account must never become
  // the lens's persistent pair-time owner (the stamp outlives the session and
  // grants the lens that identity until re-pair). Fall through to the live
  // human sessions / device owner below instead.
  if (currentAuthContext().transport == SOURCE_UART) who = String();

  if (usable(who)) return who;

  String displayUser;
  bool displayAuthed = false;
  (void)localDisplayTransportSessionSnapshot(displayUser, displayAuthed);
  if (displayAuthed && usable(displayUser)) return displayUser;

  String serialUser;
  bool serialAuthed = false;
  (void)serialTransportSessionSnapshot(serialUser, serialAuthed);
  if (serialAuthed && usable(serialUser)) return serialUser;

  // An explicit ownership gesture can originate from a UI/command path whose
  // task-local identity is empty. After checking the live human sessions,
  // fall back to the non-guest device founder for that explicit gesture.
  (void)kind;
  String owner = getDeviceOwnerUsername();
  if (usable(owner)) return owner;
  return String();
}

void bleStampPairedByIfBlank(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  const BlePeerSpec* spec = bleFindPeer(kind);
  const char* name = spec ? spec->name : "?";
  const char* task = pcTaskGetName(xTaskGetCurrentTaskHandle());

  // CASE A — peer already owned. Idempotent re-pair gestures land here.
  BlePeerOwnerSession existing;
  (void)blePeerOwnerSessionSnapshot(kind, existing);
  if (existing.user.length() > 0) {
    // A G2 owner whose central epoch could not be allocated remains
    // fail-closed. A later explicit stamp is allowed to retry publication.
    if (kind == BLE_PEER_G2_GLASSES && !existing.live() &&
        peerOwnerPublish(kind, existing.user, false)) {
      (void)blePeerOwnerSessionSnapshot(kind, existing);
      if (existing.live()) {
        // The durable username did not change, so do not rewrite settings.
        // The boot-local authority incarnation did change; invalidate caches
        // that may have observed the earlier fail-closed state.
        bumpIdentityGeneration("ble.reopen.pairedByUser");
      }
    }
    DEBUG_G2F("[BLE-Peers] stamp '%s': already owned by '%s' — no-op (task='%s')",
              name, existing.user.c_str(), task ? task : "?");
    return;
  }

  // CASE B — resolve an owner (TLS → live session → device founder).
  // Historically TLS-empty paths (boot reconnect workers, G2 stuck-stamp)
  // silently skipped and left mac/autoReconnect without pairedByUser.
  String who = bleResolveStampUsername(kind);
  if (who.length() == 0) {
    WARN_BLUETOOTHF("stamp '%s' SKIPPED: no usable owner (task='%s', no TLS/session/founder).",
                    name, task ? task : "?");
    WARN_BLUETOOTHF("  Effect: peer '%s' remains UNOWNED until a real user pairs it.",
                    name);
    WARN_BLUETOOTHF("  Recovery: log in (web/serial/OLED) and run `bleautoreconnect %s on` or `openg2`.",
                    name);
    return;
  }

  // CASE C — stamping happens here.
  const bool fromTls = (currentAuthContext().user == who);
  // Resolve outside the peer lock, then publish only if the owner is still
  // blank. Concurrent pair/reconnect workers cannot overwrite one another.
  if (!peerOwnerPublish(kind, who, true)) {
    BlePeerOwnerSession winner;
    (void)blePeerOwnerSessionSnapshot(kind, winner);
    DEBUG_G2F("[BLE-Peers] stamp '%s': owner race won by '%s' (task='%s')",
              name,
              winner.user.length() ? winner.user.c_str() : "(none)",
              task ? task : "?");
    return;
  }
  peerOwnerPersistAfterUnlock(kind, "ble.stamp.pairedByUser");
#if ENABLE_HTTP_SERVER
  // One-time security-audit record — G2 only. The glasses have no credential
  // login, so pair-time is when their owning user is captured (this is the
  // login-equivalent for that transport). Other BLE peers (ring/phone) aren't
  // audited here: pairing them isn't a login and doesn't grant command rights.
  if (kind == BLE_PEER_G2_GLASSES) {
    logAuthAttempt(true, "g2/pair", who, String("ble"),
                   fromTls ? "G2 glasses paired" : "G2 glasses owner healed");
  }
#endif
  DEBUG_G2F("[BLE-Peers] stamp '%s': pairedByUser='%s' (from task='%s'%s)",
            name, who.c_str(), task ? task : "?",
            fromTls ? "" : ", healed/fallback");
}

void bleSavePeerMac(BlePeerKind kind, const String& mac1, const String& mac2) {
  if (kind >= BLE_PEER_MAX) return;
  const BlePeerSpec* spec = bleFindPeer(kind);
  BlePeerOwnerSession owner;
  (void)blePeerOwnerSessionSnapshot(kind, owner);
  DEBUG_G2F("[BLE-Peers] savePeerMac '%s' enter: mac1='%s' mac2='%s' currentUser='%s' priorPairedBy='%s' task='%s'",
            spec ? spec->name : "?",
            mac1.c_str(), mac2.c_str(),
            currentAuthContext().user.c_str(),
            owner.user.c_str(),
            pcTaskGetName(xTaskGetCurrentTaskHandle()));
  const PeerConfigApplyResult applied = peerConfigApply(
      kind, mac1.length() > 0, mac1, mac2.length() > 0, mac2,
      false, false);
  if (!applied.applied) return;
  if (applied.targetChanged && !gDeferWrites) (void)writeSettingsJson();
  // Deliberately do not establish owner authority here. This function also
  // runs on anonymous auto-reconnect workers; ownership must be captured at
  // an explicit authenticated pairing or autoReconnect-enable intent site.
  // Don't auto-flip autoReconnect — that's an explicit user opt-in. Pairing
  // saves the MAC; turning on auto-reconnect is a separate gesture.
  DEBUG_G2F("[BLE-Peers] Saved MAC for '%s': mac1='%s'%s%s",
            spec ? spec->name : "?",
            mac1.c_str(),
            mac2.length() > 0 ? " mac2='" : "",
            mac2.length() > 0 ? mac2.c_str() : "");
}

bool bleSavePeerMacIfIdentityCurrent(
    BlePeerKind kind, uint32_t expectedIdentityGeneration,
    const String& mac1, const String& mac2) {
  if (kind >= BLE_PEER_MAX || expectedIdentityGeneration == 0) return false;
  const PeerConfigApplyResult applied = peerConfigApply(
      kind, mac1.length() > 0, mac1, mac2.length() > 0, mac2,
      false, false, expectedIdentityGeneration);
  if (!applied.applied) return false;
  if (applied.targetChanged && !gDeferWrites) (void)writeSettingsJson();
  return true;
}

bool blePeerCommitLearnedTargetIfCurrent(
    BlePeerKind kind, uint32_t intentGeneration,
    uint32_t identityGeneration, const String& mac1,
    const String& mac2, uint8_t replaceMask, bool completeTopology,
    bool* persistNeeded) {
  if (persistNeeded) *persistNeeded = false;
  if (kind >= BLE_PEER_MAX || intentGeneration == 0 ||
      identityGeneration == 0) return false;

  bool targetChanged = false;
  {
    PeerDataGuard guard;
    if (!guard) return false;
    BlePeerData& data = gBlePeerData[kind];
    const String oldMac1 = data.mac1;
    const String oldMac2 = data.mac2;
    // Bit 0/1 are explicit replacement authority for primary/secondary.
    // A set bit with an empty address deliberately clears an unseen side;
    // this prevents a first-time AUTO scan from manufacturing new-L/old-R.
    if (replaceMask & 0x01) data.mac1 = mac1;
    if (replaceMask & 0x02) data.mac2 = mac2;

    BlePeerSavedTarget target;
    const bool mac1Representable = copySavedAddress(data.mac1, target.mac1);
    const bool mac2Representable = copySavedAddress(data.mac2, target.mac2);
    if (!mac1Representable || !mac2Representable) {
      data.mac1 = oldMac1;
      data.mac2 = oldMac2;
      return false;
    }

    portENTER_CRITICAL(&sReconnectMux);
    PeerReconnectRuntime& state = sReconnect[kind];
    const bool current = state.intentGeneration == intentGeneration &&
        state.identityGeneration == identityGeneration &&
        !state.userDisconnect && state.ownerAuthorityAvailable;
    if (!current) {
      portEXIT_CRITICAL(&sReconnectMux);
      data.mac1 = oldMac1;
      data.mac2 = oldMac2;
      return false;
    }

    // Preserve an observed address type while the corresponding address did
    // not change; the current JSON schema persists address text only.
    if (memcmp(state.savedTarget.mac1, target.mac1,
               sizeof(target.mac1)) == 0) {
      target.addressType1 = state.savedTarget.addressType1;
      target.addressType1Known = state.savedTarget.addressType1Known;
    }
    if (memcmp(state.savedTarget.mac2, target.mac2,
               sizeof(target.mac2)) == 0) {
      target.addressType2 = state.savedTarget.addressType2;
      target.addressType2Known = state.savedTarget.addressType2Known;
    }
    targetChanged = !savedTargetMatches(state.savedTarget, target);
    state.savedTarget = target;
    state.hasSavedTarget = target.mac1[0] != '\0' ||
        (kind == BLE_PEER_G2_GLASSES && target.mac2[0] != '\0');
    if (targetChanged) reconnectBumpIdentityLocked(state);
    if (completeTopology) {
      noteLinkUpLocked(state);
    } else if (state.autoReconnect && state.hasSavedTarget) {
      // A first-time G2 scan may learn only one temple. Keep persistent retry
      // armed so the missing side is not silently stranded; the finite
      // topology-repair FSM can add its MAC without this worker consuming the
      // scheduler intent prematurely.
      state.wantReconnect = true;
      if (state.dueMs == 0) state.dueMs = millis() + 5000;
    }
    portEXIT_CRITICAL(&sReconnectMux);
  }
  if (persistNeeded) *persistNeeded = targetChanged && !gDeferWrites;
  return true;
}

bool blePeerCommitRepairIfCurrent(
    BlePeerKind kind, uint32_t intentGeneration,
    uint32_t identityGeneration, const String& mac1,
    const String& mac2, bool* persistNeeded) {
  return blePeerCommitLearnedTargetIfCurrent(
      kind, intentGeneration, identityGeneration, mac1, mac2,
      (uint8_t)((mac1.length() > 0 ? 0x01 : 0) |
                (mac2.length() > 0 ? 0x02 : 0)),
      /*completeTopology=*/true, persistNeeded);
}

// -----------------------------------------------------------------------------
// Boot reconnect
// -----------------------------------------------------------------------------

bool bleAnyPeerWantsAutoReconnect(void) {
  // Peer modules register after this early boot gate, so consult the fixed
  // mirrors published by blePeersReadJson rather than the registry or the
  // cross-task Arduino Strings. Missing owner authority is intentionally not
  // healed here: automatic recovery may preserve an owner, never create one.
  bool wants = false;
  portENTER_CRITICAL(&sReconnectMux);
  for (size_t i = 0; i < BLE_PEER_MAX; i++) {
    const PeerReconnectRuntime& state = sReconnect[i];
    if (state.autoReconnect && state.hasSavedTarget &&
        state.ownerAuthorityAvailable) {
      wants = true;
      break;
    }
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return wants;
}

// -----------------------------------------------------------------------------
// Reconnect admission and synchronized intent state
// -----------------------------------------------------------------------------

static uint32_t bleReconnectBackoffMs(uint8_t attempt) {
  // 5s, 15s, 45s, 90s, then 180s capped.
  static const uint32_t kSteps[] = { 5000, 15000, 45000, 90000, 180000 };
  const size_t n = sizeof(kSteps) / sizeof(kSteps[0]);
  if (attempt >= n) return kSteps[n - 1];
  return kSteps[attempt];
}

static constexpr uint32_t kAdmissionBusyRetryMs = 2000;
static constexpr uint32_t kAdmissionRoleRetryMs = 15000;
static constexpr uint32_t kAdmissionCoalescedRetryMs = 30000;

struct ReconnectAdmissionClaim {
  BlePeerKind kind = BLE_PEER_G2_GLASSES;
  uint32_t intentGeneration = 0;
  uint32_t identityGeneration = 0;
  uint8_t launchedAttempts = 0;
  bool autoReconnect = false;
  bool explicitReseek = false;
  bool hasSavedTarget = false;
  bool ownerAuthorityAvailable = false;
  BlePeerSavedTarget savedTarget;
};

static bool peerHasConnectAdmission(const BlePeerSpec* peer) {
  return peer && peer->connectable && peer->ops &&
      (peer->ops->connectSavedAdmission || peer->ops->connectSaved);
}

static BlePeerConnectAdmission sanitizeAdmission(
    BlePeerConnectAdmission result) {
  switch (result) {
    case BlePeerConnectAdmission::STARTED:
    case BlePeerConnectAdmission::COALESCED:
    case BlePeerConnectAdmission::BUSY:
    case BlePeerConnectAdmission::ALREADY_UP:
    case BlePeerConnectAdmission::NO_TARGET:
    case BlePeerConnectAdmission::ROLE_BLOCKED:
      return result;
  }
  return BlePeerConnectAdmission::BUSY;
}

static const char* admissionName(BlePeerConnectAdmission result) {
  switch (result) {
    case BlePeerConnectAdmission::STARTED: return "started";
    case BlePeerConnectAdmission::COALESCED: return "coalesced";
    case BlePeerConnectAdmission::BUSY: return "busy";
    case BlePeerConnectAdmission::ALREADY_UP: return "already-up";
    case BlePeerConnectAdmission::NO_TARGET: return "no-target";
    case BlePeerConnectAdmission::ROLE_BLOCKED: return "role-blocked";
  }
  return "busy";
}

static BlePeerConnectAdmission requestPeerConnectAdmission(
    const BlePeerSpec* peer, const ReconnectAdmissionClaim& claim,
    bool roleAllowed) {
  if (!roleAllowed || !claim.ownerAuthorityAvailable) {
    return BlePeerConnectAdmission::ROLE_BLOCKED;
  }
  if (peer && peer->ops && peer->ops->isConnected &&
      peer->ops->isConnected()) {
    return BlePeerConnectAdmission::ALREADY_UP;
  }
  if (!claim.hasSavedTarget) return BlePeerConnectAdmission::NO_TARGET;
  if (!peerHasConnectAdmission(peer)) return BlePeerConnectAdmission::BUSY;
  if (peer->ops->connectSavedAdmission) {
    const BlePeerConnectRequest request = {
      claim.intentGeneration,
      claim.identityGeneration,
      claim.autoReconnect,
      claim.explicitReseek,
      claim.savedTarget,
    };
    return sanitizeAdmission(peer->ops->connectSavedAdmission(request));
  }
  return peer->ops->connectSaved()
             ? BlePeerConnectAdmission::STARTED
             : BlePeerConnectAdmission::BUSY;
}

static void fillAdmissionClaimLocked(BlePeerKind kind,
                                     const PeerReconnectRuntime& state,
                                     ReconnectAdmissionClaim& claim) {
  claim.kind = kind;
  claim.intentGeneration = state.intentGeneration;
  claim.identityGeneration = state.identityGeneration;
  claim.launchedAttempts = state.launchedAttempts;
  claim.autoReconnect = state.autoReconnect;
  claim.explicitReseek = state.reseekEvenIfNoAuto;
  claim.hasSavedTarget = state.hasSavedTarget;
  claim.ownerAuthorityAvailable = state.ownerAuthorityAvailable;
  claim.savedTarget = state.savedTarget;
}

static bool claimDueAdmission(BlePeerKind kind, uint32_t now,
                              ReconnectAdmissionClaim& claim) {
  if (kind >= BLE_PEER_MAX) return false;
  bool claimed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  const bool due = state.dueMs == 0 ||
      (int32_t)(now - state.dueMs) >= 0;
  if (state.wantReconnect && !state.userDisconnect &&
      (state.autoReconnect || state.reseekEvenIfNoAuto) && due &&
      !state.admissionInFlight) {
    state.admissionInFlight = true;
    fillAdmissionClaimLocked(kind, state, claim);
    claimed = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return claimed;
}

static bool claimBootAdmission(BlePeerKind kind, uint32_t now,
                               ReconnectAdmissionClaim& claim) {
  if (kind >= BLE_PEER_MAX) return false;
  bool claimed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (state.autoReconnect && state.hasSavedTarget &&
      state.ownerAuthorityAvailable && !state.userDisconnect &&
      !state.admissionInFlight) {
    // The scheduler state is boot-local, so any suppression visible here was
    // issued during this boot and must win over automatic recovery.
    state.wantReconnect = true;
    state.reseekEvenIfNoAuto = false;
    state.launchedAttempts = 0;
    state.dueMs = now;
    reconnectBumpIntentLocked(state);
    state.admissionInFlight = true;
    fillAdmissionClaimLocked(kind, state, claim);
    claimed = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return claimed;
}

// Commit only against the exact intent+identity incarnation that made the
// call. A callback/user action may replace either while the peer admission
// function runs; that newer state always wins.
static bool commitAdmission(const ReconnectAdmissionClaim& claim,
                            BlePeerConnectAdmission result, uint32_t now) {
  if (claim.kind >= BLE_PEER_MAX) return false;
  bool committed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[claim.kind];
  const bool sameIncarnation =
      state.intentGeneration == claim.intentGeneration &&
      state.identityGeneration == claim.identityGeneration;
  // No second claim can start while this bit is true. Always release it even
  // when a newer callback invalidated the captured generations.
  state.admissionInFlight = false;
  if (sameIncarnation) {
    switch (result) {
      case BlePeerConnectAdmission::STARTED:
        if (state.launchedAttempts < 250) state.launchedAttempts++;
        // Explicit non-persistent nudges are consumed only after real work was
        // admitted. Persistent auto-reconnect retains intent until LinkUp.
        if (!state.autoReconnect && state.reseekEvenIfNoAuto) {
          state.wantReconnect = false;
          state.reseekEvenIfNoAuto = false;
          state.dueMs = 0;
          // Do not advance the user-intent generation here: the admitted
          // asynchronous job owns exactly claim.intentGeneration until a
          // later user/link/config event replaces or cancels it.
        } else {
          state.dueMs = now +
              bleReconnectBackoffMs(state.launchedAttempts);
        }
        break;

      case BlePeerConnectAdmission::COALESCED:
        // The peer attached this intent to existing work. Preserve the intent
        // but avoid hammering its admission callback while that work completes.
        state.dueMs = now + kAdmissionCoalescedRetryMs;
        break;

      case BlePeerConnectAdmission::BUSY:
        state.dueMs = now + kAdmissionBusyRetryMs;
        break;

      case BlePeerConnectAdmission::ROLE_BLOCKED:
        state.dueMs = now + kAdmissionRoleRetryMs;
        break;

      case BlePeerConnectAdmission::ALREADY_UP:
        state.userDisconnect = false;
        state.wantReconnect = false;
        state.reseekEvenIfNoAuto = false;
        state.launchedAttempts = 0;
        state.dueMs = 0;
        reconnectBumpIntentLocked(state);
        break;

      case BlePeerConnectAdmission::NO_TARGET:
        state.wantReconnect = false;
        state.reseekEvenIfNoAuto = false;
        state.launchedAttempts = 0;
        state.dueMs = 0;
        reconnectBumpIntentLocked(state);
        break;
    }
    committed = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return committed;
}

void bleBootReconnect(void) {

  // Pace newly started work so two peers do not attack the radio together.
  // Non-started results remain represented by synchronized scheduler intent.
  bool anyStarted = false;
  for (size_t i = 0; i < gPeerCount; i++) {
    const BlePeerSpec* p = gPeerInOrder[i];
    if (!peerHasConnectAdmission(p)) continue;
    BlePeerReconnectSnapshot snapshot;
    if (!blePeerReconnectSnapshot(p->kind, snapshot) ||
        !snapshot.autoReconnect || !snapshot.hasSavedTarget) {
      continue;
    }
    if (!snapshot.ownerAuthorityAvailable) {
      DEBUG_G2F("[BLE-Peers] Skip boot auto-reconnect '%s' — owner authority unavailable",
                p->name);
      continue;
    }
    if (anyStarted) {
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    const uint32_t now = millis();
    ReconnectAdmissionClaim claim;
    if (!claimBootAdmission(p->kind, now, claim)) continue;
    // Boot reaches here only after the central/client stack was initialized by
    // HardwareOne. Do not reject solely because persisted bleMode was coerced
    // at boot without being rewritten.
    const BlePeerConnectAdmission result =
        requestPeerConnectAdmission(p, claim, true);
    const bool committed = commitAdmission(claim, result, now);
    DEBUG_G2F("[BLE-Peers] Boot reconnect '%s': admission=%s committed=%d",
              p->name, admissionName(result), (int)committed);
    if (committed && result == BlePeerConnectAdmission::STARTED) {
      anyStarted = true;
    }
  }
  if (!anyStarted) {
    DEBUG_G2F("[BLE-Peers] No new boot reconnect work admitted");
  }
}

// -----------------------------------------------------------------------------
// Mid-session drop → reseek (autoReconnect peers with a saved MAC)
// -----------------------------------------------------------------------------
// WiFi has wifiautoreconnect for link drops; BLE peers historically only
// reconnected at boot (bleBootReconnect). These helpers close that gap:
// unexpected onDisconnect schedules connectSaved with exponential backoff.
// Intentional disconnect (CLI/OLED/Web) stamps userDisconnect so we stay down.

void blePeerNoteUserDisconnect(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  state.userDisconnect = true;
  state.wantReconnect = false;
  state.reseekEvenIfNoAuto = false;
  state.launchedAttempts = 0;
  state.dueMs = 0;
  reconnectBumpIntentLocked(state);
  portEXIT_CRITICAL(&sReconnectMux);
  DEBUG_G2F("[BLE-Peers] User disconnect stamped for kind=%u — no auto-reseek",
            (unsigned)kind);
}

void blePeerNoteUserConnectIntent(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  state.userDisconnect = false;
  // Do not destroy persistent retry state before an asynchronous manual job is
  // actually admitted. If its queue/gate loses a race, configured auto-retry
  // must remain available rather than stranding the peer. The generation bump
  // still fences any older job; a successful new job consumes retry at commit.
  if (state.autoReconnect && state.hasSavedTarget && !state.wantReconnect) {
    state.wantReconnect = true;
    state.dueMs = millis() + 5000;
  }
  reconnectBumpIntentLocked(state);
  portEXIT_CRITICAL(&sReconnectMux);
  DEBUG_G2F("[BLE-Peers] User connect intent stamped for kind=%u",
            (unsigned)kind);
}

bool blePeerBeginManualLearn(BlePeerKind kind,
                             uint32_t& intentGeneration,
                             uint32_t& identityGeneration) {
  intentGeneration = 0;
  identityGeneration = 0;
  if (kind >= BLE_PEER_MAX) return false;

  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (!state.ownerAuthorityAvailable) {
    portEXIT_CRITICAL(&sReconnectMux);
    return false;
  }
  state.userDisconnect = false;
  if (state.autoReconnect && state.hasSavedTarget && !state.wantReconnect) {
    state.wantReconnect = true;
    state.dueMs = millis() + 5000;
  }
  reconnectBumpIntentLocked(state);
  intentGeneration = state.intentGeneration;
  identityGeneration = state.identityGeneration;
  portEXIT_CRITICAL(&sReconnectMux);
  DEBUG_G2F("[BLE-Peers] Manual learned-target intent stamped for kind=%u",
            (unsigned)kind);
  return true;
}

void blePeerNoteLinkLost(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  if (!bleIsPeerRegistered(kind)) return;
  const BlePeerSpec* p = bleFindPeer(kind);
  if (!peerHasConnectAdmission(p)) return;

  const uint32_t delayMs = bleReconnectBackoffMs(0);
  const uint32_t now = millis();
  bool userSuppressed = false;
  bool scheduled = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  userSuppressed = state.userDisconnect;
  if (!userSuppressed && state.autoReconnect && state.hasSavedTarget &&
      state.ownerAuthorityAvailable) {
    const bool newEpisode = !state.wantReconnect || state.dueMs == 0;
    if (newEpisode) {
      state.wantReconnect = true;
      state.launchedAttempts = 0;
      state.dueMs = now + delayMs;
      // Duplicate callbacks for the same down episode (for example, both G2
      // temples reporting loss) must not invalidate work already admitted for
      // that episode. LinkUp clears wantReconnect, so a later real drop still
      // creates a fresh generation here.
      reconnectBumpIntentLocked(state);
    }
    scheduled = newEpisode;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  if (userSuppressed) {
    DEBUG_G2F("[BLE-Peers] Link lost kind=%u ignored (user disconnect)",
              (unsigned)kind);
    return;
  }
  if (scheduled) {
    DEBUG_G2F("[BLE-Peers] Link lost '%s' — reseek in %lums (autoReconnect)",
              p->name, (unsigned long)delayMs);
  }
}

void blePeerNoteLinkUp(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  // A late manual/internal success must not undo a newer explicit disconnect.
  if (!state.userDisconnect) noteLinkUpLocked(state);
  portEXIT_CRITICAL(&sReconnectMux);
}

bool blePeerNoteLinkUpIfCurrent(
    BlePeerKind kind, const BlePeerConnectRequest& request) {
  if (kind >= BLE_PEER_MAX || request.intentGeneration == 0 ||
      request.identityGeneration == 0 ||
      (!request.autoReconnect && !request.explicitReseek)) {
    return false;
  }
  bool committed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (connectRequestIsCurrentLocked(state, request)) {
    noteLinkUpLocked(state);
    committed = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return committed;
}

bool blePeerIntentIsCurrent(BlePeerKind kind, uint32_t intentGeneration,
                            uint32_t identityGeneration) {
  if (kind >= BLE_PEER_MAX || intentGeneration == 0 ||
      identityGeneration == 0) return false;
  portENTER_CRITICAL(&sReconnectMux);
  const PeerReconnectRuntime& state = sReconnect[kind];
  const bool current = state.intentGeneration == intentGeneration &&
      state.identityGeneration == identityGeneration &&
      !state.userDisconnect && state.ownerAuthorityAvailable;
  portEXIT_CRITICAL(&sReconnectMux);
  return current;
}

bool blePeerNoteLinkUpIfIntentCurrent(BlePeerKind kind,
                                      uint32_t intentGeneration,
                                      uint32_t identityGeneration) {
  if (kind >= BLE_PEER_MAX || intentGeneration == 0 ||
      identityGeneration == 0) return false;
  bool committed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  if (state.intentGeneration == intentGeneration &&
      state.identityGeneration == identityGeneration &&
      !state.userDisconnect && state.ownerAuthorityAvailable) {
    noteLinkUpLocked(state);
    committed = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  return committed;
}

void blePeerRequestReseek(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  if (!bleIsPeerRegistered(kind)) return;
  const BlePeerSpec* p = bleFindPeer(kind);
  if (!peerHasConnectAdmission(p)) return;
  if (p->ops->isConnected && p->ops->isConnected()) return;

  const uint32_t now = millis();
  bool accepted = false;
  bool userSuppressed = false;
  portENTER_CRITICAL(&sReconnectMux);
  PeerReconnectRuntime& state = sReconnect[kind];
  userSuppressed = state.userDisconnect;
  if (!userSuppressed && state.hasSavedTarget &&
      state.ownerAuthorityAvailable) {
    state.wantReconnect = true;
    state.reseekEvenIfNoAuto = true;
    state.launchedAttempts = 0;
    state.dueMs = now;
    reconnectBumpIntentLocked(state);
    accepted = true;
  }
  portEXIT_CRITICAL(&sReconnectMux);
  if (userSuppressed) {
    DEBUG_G2F("[BLE-Peers] Reseek request kind=%u ignored (user disconnect)",
              (unsigned)kind);
  } else if (accepted) {
    DEBUG_G2F("[BLE-Peers] Reseek requested for '%s' (saved target)",
              p->name);
  }
}

void bleAutoReconnectTick(void) {
  const uint32_t now = millis();
  for (size_t i = 0; i < gPeerCount; i++) {
    const BlePeerSpec* p = gPeerInOrder[i];
    if (!peerHasConnectAdmission(p)) continue;
    const BlePeerKind kind = p->kind;
    ReconnectAdmissionClaim claim;
    if (!claimDueAdmission(kind, now, claim)) continue;
    DEBUG_G2F("[BLE-Peers] Reseek '%s' attempt=%u",
              p->name, (unsigned)claim.launchedAttempts + 1);
    // Dispatch against the live application owner, not the persisted desired
    // mode. Boot may intentionally coerce server-configured settings into the
    // client role for a saved-peer recovery without rewriting the setting.
    const bool roleAllowed =
        bleSubsystemActive() && !isBleServerInitialized();
    const BlePeerConnectAdmission result =
        requestPeerConnectAdmission(p, claim, roleAllowed);
    const bool committed = commitAdmission(claim, result, now);
    DEBUG_G2F("[BLE-Peers] Reseek '%s': admission=%s committed=%d",
              p->name, admissionName(result), (int)committed);
  }
}

// -----------------------------------------------------------------------------
// CLI: bleautoreconnect <peer-name> [on|off]
// -----------------------------------------------------------------------------

const char* cmd_bleautoreconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[160];

  // Parse "<name> [on|off]". Empty arg → list all peers' state.
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return cmd_blepeers(arg);
  }

  // First token is the peer name; the rest is the optional on/off.
  int sp = arg.indexOf(' ');
  String name = (sp > 0) ? arg.substring(0, sp) : arg;
  String rest = (sp > 0) ? arg.substring(sp + 1) : String();
  rest.trim();

  const BlePeerSpec* p = bleFindPeerByName(name.c_str());
  if (!p) {
    snprintf(buf, sizeof(buf),
             "[BLE] Unknown peer '%s'. Use `blepeers` to list.",
             name.c_str());
    return buf;
  }
  if (!p->connectable) {
    snprintf(buf, sizeof(buf),
             "[BLE] Peer '%s' is metadata-only (no connect implemented)",
             p->name);
    return buf;
  }

  BlePeerReconnectSnapshot live;
  BlePeerSavedTargetSnapshot target;
  (void)blePeerReconnectSnapshot(p->kind, live);
  (void)blePeerSavedTargetSnapshot(p->kind, target);

  if (rest.length() == 0) {
    snprintf(buf, sizeof(buf),
             "[BLE] %s auto-reconnect: %s (mac1='%s'%s%s)",
             p->displayName ? p->displayName : p->name,
             live.autoReconnect ? "enabled" : "disabled",
             target.target.mac1[0] ? target.target.mac1 : "(none)",
             target.target.mac2[0] ? " mac2='" : "",
             target.target.mac2[0] ? target.target.mac2 : "");
    return buf;
  }

  int on = parseBoolArg(rest);
  if (on < 0) {
    snprintf(buf, sizeof(buf),
             "Usage: bleautoreconnect %s [on|off]", p->name);
    return buf;
  }
  if (!on) {
    // Cancellation is execution policy, not a persistence side effect. Publish
    // it before setSetting() can block in writeSettingsJson(), otherwise the
    // main-loop tick can admit one last stale attempt during that flash write.
    const PeerConfigApplyResult config = peerConfigApply(
        p->kind, false, String(), false, String(), true, false, 0,
        PeerIntentAction::UserDisconnect);
    if (config.applied && config.policyChanged && !gDeferWrites) {
      (void)writeSettingsJson();
    }
  } else {
    // Resolve owner authority before publishing or persisting auto=true. A
    // power loss can no longer leave an ownerless enabled record on disk.
    bleStampPairedByIfBlank(p->kind);
    // Never leave autoReconnect on for an unowned peer — that is the stuck
    // state (MAC reconnects, hijack submits reject). Roll back if stamp
    // still failed (no founder / no session yet).
    PeerConfigApplyResult config;
    bool ownerAvailable = false;
    {
      // Keep the owner incarnation stable through the config transaction.
      // Merely snapshotting its generation leaves a window where revocation
      // can race an old `on` command and persist ownerless auto-reconnect.
      PeerOwnerGuard ownerGuard;
      if (ownerGuard) {
        const PeerOwnerAuthority& authority = sPeerOwner[p->kind];
        ownerAvailable = authority.user[0] != '\0' &&
            (p->kind != BLE_PEER_G2_GLASSES ||
             (authority.transportEpoch != kNoTransportSessionEpoch &&
              transportSessionEpochIsLive(SOURCE_G2_GLASSES,
                                          authority.transportEpoch)));
        if (ownerAvailable) {
          config = peerConfigApply(
              p->kind, false, String(), false, String(), true, true, 0,
              PeerIntentAction::UserConnect, authority.generation,
              /*requireOwnerAuthority=*/true);
        }
      }
    }
    if (!ownerAvailable) {
      (void)peerConfigApply(p->kind, false, String(), false, String(),
                            true, false, 0,
                            PeerIntentAction::UserDisconnect);
      snprintf(buf, sizeof(buf),
               "[BLE] %s auto-reconnect NOT enabled — owner authority is "
               "unavailable (log in and retry, or create the device owner first)",
               p->displayName ? p->displayName : p->name);
      return buf;
    }
    if (!config.applied) {
      snprintf(buf, sizeof(buf),
               "[BLE] %s auto-reconnect update failed",
               p->displayName ? p->displayName : p->name);
      return buf;
    }
    if (config.policyChanged && !gDeferWrites) (void)writeSettingsJson();
    // The config transaction above also clears prior user-disconnect
    // suppression. If currently down, schedule the first retry afterwards;
    // a concurrent OFF transaction will win and LinkLost will observe it.
    const bool linked = (p->ops && p->ops->isConnected) ? p->ops->isConnected() : false;
    if (!linked) blePeerNoteLinkLost(p->kind);
  }
  snprintf(buf, sizeof(buf),
           "[BLE] %s auto-reconnect %s%s",
           p->displayName ? p->displayName : p->name,
           on ? "enabled" : "disabled",
           on ? " (boot + mid-session drop reseek)" : "");
  return buf;
}

// -----------------------------------------------------------------------------
// CLI: blepeers — list every registered peer with state
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// JSON load/save
// -----------------------------------------------------------------------------
// Settings load runs before peer modules call bleRegisterPeer (the
// modules typically register inside their init function which runs
// after settings load). So serialization can't iterate the registry —
// it walks a hardcoded name↔kind table. This is the single source of
// truth for the JSON schema; if you add a new BlePeerKind, add a row
// here at the same time.
struct PeerJsonEntry {
  BlePeerKind kind;
  const char* name;
};
static const PeerJsonEntry kPeerJsonTable[] = {
  { BLE_PEER_G2_GLASSES, "g2-glasses" },
  { BLE_PEER_R1_RING,    "r1-ring"    },
  { BLE_PEER_PHONE,      "phone"      },
};

void blePeersWriteJson(JsonDocument& doc) {
  // Materialise network.bluetooth.peers as a nested object keyed by peer
  // name. Each value is { mac1, [mac2], autoReconnect, [pairedByUser] }.
  JsonObject peers = doc["network"]["bluetooth"]["peers"].to<JsonObject>();
  for (const auto& row : kPeerJsonTable) {
    String mac1;
    String mac2;
    bool autoReconnect = false;
    {
      PeerDataGuard guard;
      if (!guard) continue;
      mac1 = gBlePeerData[row.kind].mac1;
      mac2 = gBlePeerData[row.kind].mac2;
      autoReconnect = gBlePeerData[row.kind].autoReconnect;
    }
    BlePeerOwnerSession owner;
    (void)blePeerOwnerSessionSnapshot(row.kind, owner);
    JsonObject e = peers[row.name].to<JsonObject>();
    e["mac1"] = mac1;
    // Only emit mac2 if it has content — keeps single-MAC peers tidy.
    if (mac2.length() > 0) e["mac2"] = mac2;
    e["autoReconnect"] = autoReconnect;
    // Only emit pairedByUser if set — legacy peers paired before this
    // field existed leave it blank.
    if (owner.user.length() > 0) e["pairedByUser"] = owner.user;
    DEBUG_G2F("[BLE-Peers] writeJson peer='%s' mac1='%s' autoReconnect=%d pairedByUser='%s'%s",
              row.name,
              mac1.c_str(),
              (int)autoReconnect,
              owner.user.c_str(),
              owner.user.length() == 0 ? " (OMITTED from JSON)" : "");
  }
}

void blePeersReadJson(JsonDocument& doc) {
  JsonObjectConst peers = doc["network"]["bluetooth"]["peers"].as<JsonObjectConst>();
  if (peers.isNull()) return;
  for (const auto& row : kPeerJsonTable) {
    JsonObjectConst e = peers[row.name].as<JsonObjectConst>();
    if (e.isNull()) continue;
    String oldMac1;
    String oldMac2;
    bool oldAutoReconnect = false;
    String newMac1;
    String newMac2;
    bool newAutoReconnect = false;
    {
      PeerDataGuard guard;
      if (!guard) continue;
      oldMac1 = gBlePeerData[row.kind].mac1;
      oldMac2 = gBlePeerData[row.kind].mac2;
      oldAutoReconnect = gBlePeerData[row.kind].autoReconnect;
      newMac1 = !e["mac1"].isNull()
          ? String(e["mac1"].as<const char*>()) : oldMac1;
      newMac2 = !e["mac2"].isNull()
          ? String(e["mac2"].as<const char*>()) : oldMac2;
      newAutoReconnect = !e["autoReconnect"].isNull()
          ? e["autoReconnect"].as<bool>() : oldAutoReconnect;
    }
    String loadedOwner;
    if (!e["pairedByUser"].isNull()) {
      loadedOwner = e["pairedByUser"].as<const char*>();
    }
    // Settings load precedes full account/filesystem availability, so this
    // is schema validation only. Runtime command authorization still
    // resolves the current account role fail-closed before execution.
    if (loadedOwner.length() > kPublicUsernameMaxLen) {
      WARN_BLUETOOTHF("[BLE-Peers] readJson '%s': rejecting oversized pairedByUser",
                      row.name);
      loadedOwner = String();
    }
    if (peerOwnerPublish(row.kind, loadedOwner, false)) {
      bumpIdentityGeneration("ble.load.pairedByUser");
    }
    (void)peerConfigApply(row.kind, true, newMac1, true, newMac2,
                          true, newAutoReconnect);
  }
  // A legacy record with a saved MAC but no pairedByUser remains deliberately
  // unowned. Automatic settings-load/boot recovery may preserve authority,
  // but only an explicit authenticated pairing or autoReconnect-enable intent
  // may establish it.
}

const char* cmd_blepeers(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: (no buffer)";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["peers"].to<JsonArray>();
    for (size_t i = 0; i < gPeerCount; i++) {
      const BlePeerSpec* p = gPeerInOrder[i];
      if (!p) continue;
      BlePeerReconnectSnapshot reconnect;
      BlePeerSavedTargetSnapshot target;
      (void)blePeerReconnectSnapshot(p->kind, reconnect);
      (void)blePeerSavedTargetSnapshot(p->kind, target);
      BlePeerOwnerSession owner;
      (void)blePeerOwnerSessionSnapshot(p->kind, owner);
      const bool linked = (p->ops && p->ops->isConnected) ? p->ops->isConnected() : false;
      JsonObject o = arr.add<JsonObject>();
      o["name"]        = p->name;
      o["displayName"] = p->displayName ? p->displayName : "";
      o["connectable"] = p->connectable;
      o["connected"]   = linked;
      o["autoReconnect"] = reconnect.autoReconnect;
      o["mac1"]        = target.target.mac1;
      if (p->macCount > 1) o["mac2"] = target.target.mac2;
      o["pairedBy"]    = owner.user;
    }
    doc["count"] = (int)gPeerCount;
    doc["hint"] = "to see a bonded peer's data, run 'bondstatus' (or 'bondrequestcap' for its capabilities)";
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  char* out = getDebugBuffer();
  size_t cap = 1024;
  size_t pos = 0;
  pos += snprintf(out + pos, cap - pos, "BLE peers (%u registered):\n",
                  (unsigned)gPeerCount);
  for (size_t i = 0; i < gPeerCount && pos < cap; i++) {
    const BlePeerSpec* p = gPeerInOrder[i];
    if (!p) continue;
    BlePeerReconnectSnapshot reconnect;
    BlePeerSavedTargetSnapshot target;
    (void)blePeerReconnectSnapshot(p->kind, reconnect);
    (void)blePeerSavedTargetSnapshot(p->kind, target);
    BlePeerOwnerSession owner;
    (void)blePeerOwnerSessionSnapshot(p->kind, owner);
    const bool linked = (p->ops && p->ops->isConnected) ? p->ops->isConnected() : false;
    pos += snprintf(out + pos, cap - pos,
                    "  %-12s %-12s %s auto=%s mac1=%s",
                    p->name,
                    p->displayName ? p->displayName : "",
                    p->connectable ? (linked ? "[CONNECTED]" : "[disconn]")
                                   : "[metadata]",
                    reconnect.autoReconnect ? "on" : "off",
                    target.target.mac1[0] ? target.target.mac1 : "(none)");
    if (p->macCount > 1) {
      pos += snprintf(out + pos, cap - pos, " mac2=%s",
                      target.target.mac2[0] ? target.target.mac2 : "(none)");
    }
    pos += snprintf(out + pos, cap - pos, " pairedBy=%s",
                    owner.user.length() ? owner.user.c_str() : "(none)");
    pos += snprintf(out + pos, cap - pos, "\n");
  }
  cliHint("to see a bonded peer's data, run 'bondstatus' (or 'bondrequestcap' for its capabilities)");
  return out;
}

#endif  // ENABLE_BLUETOOTH
