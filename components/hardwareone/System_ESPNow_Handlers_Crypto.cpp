#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Handlers_Crypto.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <esp_now.h>
#include <esp_wifi.h>

#include "System_Debug.h"
#include "System_ESPNow.h"          // EspNowState (gEspNow); V4RxCtx comes from System_ESPNow_RxCtx.h
#include "System_ESPNow_Crypto.h"
#include "System_ESPNow_HandshakeBackoff.h"
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_MeshKeys.h"
#include "System_ESPNow_Sensors.h"         // sensorTypeInThisBuild: the sensor_send capability
#include "System_ESPNow_SessionCapsCore.h"  // the session capability word
#include "System_ESPNow_Sessions.h"
#include "System_ESPNow_Wire.h"
#include "System_CommandTypes.h"   // ExecReq::DeferredFn
#include "System_Utils.h"  // submitDeferredToCmdExec
#include "System_MemUtil.h"        // ps_alloc
#include "System_Mutex.h"          // EspNowTxGuard
#include "System_Settings.h"

#include <sodium.h>  // sodium_memzero

// NOTE: the bond auth token is derived inside sessionDeriveAeadKeys (for every
// session, from the same X25519 shared secret) and stored in SessionState. The
// handlers below no longer derive it explicitly — it's already in place by the
// time sessionDeriveAeadKeys returns.

// V4RxCtx comes from the shared header now (Phase 0 relay groundwork). This
// TU used to carry a hand-copied duplicate that had drifted out of sync — an
// ODR violation that made deviceName (and anything after isPaired) read
// garbage here. With the single shared definition, every field is safe.
#include "System_ESPNow_RxCtx.h"

namespace {

// Resolve mesh slot from a label, "" / nullptr → first enabled+default mesh,
// else -1. Returns slot index in gSettings.meshes[].
int resolveMeshSlot(const char* label) {
  if (label && *label) {
    for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
      if (gSettings.meshes[i].enabled && gSettings.meshes[i].label == label) {
        return i;
      }
    }
    return -1;
  }
  // Default lookup: first enabled+default, else first enabled.
  int fallback = -1;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (!gSettings.meshes[i].enabled) continue;
    if (fallback < 0) fallback = i;
    if (gSettings.meshes[i].isDefault) return i;
  }
  return fallback;
}

// Compute HMAC over (meshFingerprint LE || senderMac || senderPub) with the
// mesh bootstrap key. 40 bytes input, 32 bytes output.
bool computeKeyExHmac(uint16_t meshFingerprint,
                      const uint8_t senderMac[6],
                      const uint8_t senderPub[32],
                      const uint8_t bootstrapKey[32],
                      uint8_t outHmac[32]) {
  uint8_t fpLE[2] = { (uint8_t)(meshFingerprint & 0xFF),
                      (uint8_t)((meshFingerprint >> 8) & 0xFF) };
  return espnowCryptoHmacSha256(outHmac,
                                bootstrapKey, 32,
                                fpLE, 2,
                                senderMac, 6,
                                senderPub, 32);
}

// Make sure the ESPNOW peer table knows about this MAC. We add as unencrypted
// (LMK off) — KEY_EX frames travel as plaintext-but-authenticated. Idempotent;
// reuses the existing slot if already present.
bool ensureUnencryptedPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  // channel 0 = follow-the-radio, matching every other add_peer site. Pinning
  // gEspNow->channel here froze this peer on the init-time channel, so a later
  // AP roam / `espnowchannel` change silently broke KEY_EX to it.
  info.channel = 0;
  info.ifidx   = WIFI_IF_STA;
  info.encrypt = false;
  esp_err_t rc = esp_now_add_peer(&info);
  if (rc != ESP_OK) {
    WARN_ESPNOWF("KEY_EX: esp_now_add_peer failed for " MAC_FMT " (rc=%d)",
                 MAC_ARGS(mac), (int)rc);
    return false;
  }
  return true;
}

void logMac(const char* tag, const uint8_t mac[6]) {
  INFO_ESPNOWF("%s " MAC_FMT,
               tag, MAC_ARGS(mac));
}

// Constant-time 32-byte compare. Avoids leaking how many leading bytes matched
// when an attacker sprays HMAC guesses. (libsodium provides sodium_memcmp
// but we don't pull it in here; trivial constant-time loop.)
bool ctMemcmp32(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// Common verification step for HELLO and REPLY: payload size, mesh lookup,
// HMAC verify, sender MAC sanity check. On success, *outBootstrapKey is set
// to point into the mesh derived-key cache.
//
// Returns:
//   -1 = invalid payload (size mismatch)
//   -2 = unknown mesh (fingerprint doesn't match any configured slot)
//   -3 = HMAC verification failed
//   -4 = sender MAC mismatch between header.origin and payload
//   >=0 = OK, value is the mesh slot index
int verifyKeyExPayload(const V4RxCtx& ctx, uint16_t expectedFp,
                       const uint8_t senderMac[6],
                       const uint8_t senderPub[32],
                       const uint8_t hmac[32],
                       const uint8_t** outBootstrapKey) {
  // Header origin must match the sender MAC field inside the payload.
  if (memcmp(ctx.h->origin, senderMac, 6) != 0) return -4;

  // Look up mesh by fingerprint.
  const MeshDerivedKeys* mk = meshKeysFindByFingerprint(expectedFp);
  if (!mk) return -2;

  // Recompute HMAC and constant-time compare.
  uint8_t expected[32];
  if (!computeKeyExHmac(expectedFp, senderMac, senderPub,
                        mk->bootstrapKey, expected)) {
    return -3;
  }
  if (!ctMemcmp32(expected, hmac)) return -3;

  *outBootstrapKey = mk->bootstrapKey;
  // Find the meshIdx (slot) that matches this fingerprint.
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].fingerprint == expectedFp) {
      return i;
    }
  }
  return -2;
}

// Accepted-HELLO rate limit. A HELLO's HMAC covers the mesh fingerprint, the
// sender MAC and the sender's public key and nothing fresh, so a recorded
// HELLO stays valid, and a copy with a new header msgId passes the
// dispatcher's duplicate ring. Acting on one costs a REPLY (or a CONFIRM) on
// the air and can use up a pairing window, so at most one verified HELLO per
// sender MAC and mesh is acted on per kKeyExHelloMinIntervalMs; the others are
// dropped right after the HMAC check. The mesh is part of the key because one
// pairing action can send two HELLOs tens of milliseconds apart for two meshes
// both devices share (the seed heartbeat's and the command's own), and the
// second must not be mistaken for a copy of the first. The interval is shorter than the initiator's own
// HELLO retry (kKeyExRetryMs, asserted where that is defined), so the
// retransmit after a lost REPLY is always answered, and a first HELLO is
// answered at once.
//
// One slot per sender MAC and mesh, as many as there are peer-identity slots. A slot is
// taken over only after its interval has passed; while every slot is inside its
// interval a HELLO from one more MAC is dropped, which bounds the HELLOs acted
// on to kKeyExHelloSlots per interval however many sender MACs are replayed.
// Only v4hKeyExHello (espnow_task) touches the table: no lock.
constexpr uint8_t  kKeyExHelloSlots         = 16;
constexpr uint32_t kKeyExHelloMinIntervalMs = 1000;
struct KeyExHelloSeen {
  uint8_t  mac[6];
  bool     used;
  uint16_t meshFp;    // mesh fingerprint the HELLO was for
  uint32_t actedMs;   // millis() of the last HELLO from this MAC and mesh that was acted on
};
EXT_RAM_BSS_ATTR KeyExHelloSeen gKeyExHelloSeen[kKeyExHelloSlots] = {};

// True when a verified HELLO from `mac` for mesh `meshFp` may be acted on now;
// notes it if so.
bool keyExHelloAdmit(const uint8_t mac[6], uint16_t meshFp, uint32_t nowMs) {
  KeyExHelloSeen* spare = nullptr;
  for (auto& e : gKeyExHelloSeen) {
    const bool lapsed = !e.used || (uint32_t)(nowMs - e.actedMs) >= kKeyExHelloMinIntervalMs;
    if (e.used && e.meshFp == meshFp && memcmp(e.mac, mac, 6) == 0) {
      if (!lapsed) return false;
      e.actedMs = nowMs;
      return true;
    }
    if (lapsed && !spare) spare = &e;
  }
  if (!spare) return false;
  memcpy(spare->mac, mac, 6);
  spare->meshFp  = meshFp;
  spare->actedMs = nowMs;
  spare->used    = true;
  return true;
}

}  // namespace

// KEY_EX retry/timeout (F6) — clear the in-flight retry record for a peer once
// the handshake resolves. Forward-declared here so the REPLY/CONFIRM handlers
// (above the table definition) can call it; defined alongside the table below.
static void keyExClearInFlight(const uint8_t peerMac[6]);
static bool keyExCancelled(const uint8_t peerMac[6]);

// Build + send a KEY_EX_CONFIRM to peerMac with the given status
// (0=ok, 1=hmac-fail, 2=pub-conflict). CONFIRM carries no HMAC — the prior
// REPLY's HMAC already authenticated the peer — so this needs no mesh key,
// just our own identity for the OOB pubkey fingerprint. Shared by the
// responder (HELLO handler) and the initiator (REPLY handler) so the
// conflict-rejection path is symmetric by construction rather than by
// duplicated inline builds (was: HELLO sent status=2 on conflict, REPLY
// silently dropped — fixed by routing both through here).
static void sendKeyExConfirm(const uint8_t peerMac[6], uint16_t meshFingerprint,
                             uint8_t status) {
  V4PayloadKeyExConfirm conf = {};
  conf.meshFingerprint = meshFingerprint;
  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  memcpy(conf.confirmerMac, selfMac, 6);
  conf.status = status;
  const auto& self = espnowIdentityGet();
  if (self.valid) memcpy(conf.pubFingerprint, self.pub, 8);
  v4_send_frame(peerMac, ESPNOW_V4_TYPE_KEY_EX_CONFIRM,
                ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&conf), sizeof(conf), 1);
}

// Per-peer throttle for the repeating SESSION_OPEN-reject WARN. A re-flashed or
// mis-keyed peer re-sends SESSION_OPEN every heartbeat (~5s); unthrottled the
// reject floods the log. Returns the rejects accumulated for this peer since the
// last emit when it is time to log again (one line per ~30s per peer), else 0.
// Shared by both reject branches so the count is per-peer regardless of branch.
// Single-task (runDeferredSessionOpen runs on cmd_exec) so the ring needs
// no lock; logCooldownOk is the shared throttle primitive.
static uint16_t sessionRejectLogCount(const uint8_t mac[6]) {
  static struct { uint8_t mac[6]; uint32_t lastMs; uint16_t n; bool used; } slots[4] = {};
  int idx = -1, oldest = 0;
  for (int i = 0; i < 4; i++) {
    if (slots[i].used && memcmp(slots[i].mac, mac, 6) == 0) { idx = i; break; }
    if (!slots[i].used && idx < 0) idx = i;
    if (slots[i].lastMs < slots[oldest].lastMs) oldest = i;
  }
  if (idx < 0) idx = oldest;  // no match and no free slot — evict the stalest
  auto& s = slots[idx];
  if (!s.used || memcmp(s.mac, mac, 6) != 0) {
    memcpy(s.mac, mac, 6); s.lastMs = 0; s.n = 0; s.used = true;
  }
  s.n++;
  if (logCooldownOk(s.lastMs, 30000)) { uint16_t c = s.n; s.n = 0; return c; }
  return 0;
}

// ============================================================================
// Receive handlers
// ============================================================================

void v4hKeyExHello(const V4RxCtx& ctx) {
  // The receive table admits exactly 72 bytes (V4_OPC_LEN_EXACT).
  V4PayloadKeyExHello hello;
  ctx.read(hello);
  const V4PayloadKeyExHello* msg = &hello;

  const uint8_t* bootKey = nullptr;
  int meshSlot = verifyKeyExPayload(ctx, msg->meshFingerprint,
                                    msg->senderMac, msg->senderPubEd25519,
                                    msg->hmac, &bootKey);
  if (meshSlot < 0) {
    const char* why = (meshSlot == -2) ? "unknown mesh - this device has no mesh matching that label/fingerprint"
                    : (meshSlot == -3) ? "HMAC FAIL = PASSPHRASE MISMATCH - the two devices do NOT share the same mesh passphrase"
                    : (meshSlot == -4) ? "sender MAC mismatch (frame header vs payload)"
                    : "invalid payload";
    WARN_ESPNOWF("KEY_EX_HELLO from " MAC_FMT " rejected: reason=%d (%s) meshFp=0x%04X",
                 MAC_ARGS(msg->senderMac),
                 meshSlot, why, (unsigned)msg->meshFingerprint);
    return;
  }

  // Verified, but a copy of a recorded HELLO verifies too: act on one per
  // sender MAC and mesh per interval (see keyExHelloAdmit). Checked before the
  // pairing window below, which a dropped HELLO must not use up.
  if (!keyExHelloAdmit(msg->senderMac, msg->meshFingerprint, (uint32_t)millis())) {
    DEBUG_ESPNOWF("KEY_EX_HELLO from " MAC_FMT " ignored: one answered less than %ums ago",
                  MAC_ARGS(msg->senderMac), (unsigned)kKeyExHelloMinIntervalMs);
    return;
  }

  // Check for conflicting existing identity. If we already know a different
  // pubkey for this MAC, refuse to overwrite — operator must espnowforget
  // first. Without this, an attacker who learns the passphrase later can
  // silently replace a paired peer's identity.
  const PeerIdentity* existing = peerIdentityFindByMac(msg->senderMac);
  uint8_t confirmStatus = 0;
  bool conflict = existing && memcmp(existing->longTermPub, msg->senderPubEd25519, 32) != 0;
  if (conflict) {
    // A deliberate local `espnowpairsecure` opens a one-shot re-key window for
    // this MAC — honor it (peer's key rotated). Otherwise refuse the overwrite.
    if (espnowConsumePairingWindow(msg->senderMac)) {
      WARN_ESPNOWF("KEY_EX_HELLO: peer " MAC_FMT " presented new pubkey; "
                   "re-pair window open — replacing stored identity",
                   MAC_ARGS(msg->senderMac));
      conflict = false;  // operator-authorized re-key
    }
  }
  if (conflict) {
    WARN_ESPNOWF("KEY_EX_HELLO: peer " MAC_FMT " presented new pubkey, "
                 "refusing overwrite — run 'espnowforget' or re-run 'espnowpairsecure'",
                 MAC_ARGS(msg->senderMac));
    // Event log is min-interval gated: repeated HELLOs from the same rogue
    // sender must not flood the event stream.
    static uint32_t sHelloConflictEventMs = 0;
    uint32_t nowMs = millis();
    if (sHelloConflictEventMs == 0 || (uint32_t)(nowMs - sHelloConflictEventMs) >= 60000UL) {
      sHelloConflictEventMs = nowMs ? nowMs : 1;
      logSystemEvent("ESPNOW", "SECURITY: peer " MAC_FMT " presented a DIFFERENT pubkey — identity overwrite refused",
                     MAC_ARGS(msg->senderMac));
    }
    confirmStatus = 2;
  } else {
    time_t now = time(nullptr);
    uint32_t bondedAt = (uint32_t)((now > 0) ? now : 0);
    // Event-log gate: a re-sent HELLO with the same key comes through here
    // routinely (peerIdentityPersist then writes nothing, bar the hourly
    // lastSeenSec refresh). Only log the event when the stored identity
    // actually changes (first pairing or an operator-authorized key
    // replacement). Computed BEFORE persist mutates the slot `existing`
    // points into.
    bool identityNewOrReplaced = !existing ||
        memcmp(existing->longTermPub, msg->senderPubEd25519, 32) != 0;
    if (!peerIdentityPersist(msg->senderMac, (uint8_t)meshSlot,
                             msg->senderPubEd25519, bondedAt)) {
      ERROR_ESPNOWF("KEY_EX_HELLO: identity persist failed; not replying");
      return;
    }
    if (identityNewOrReplaced) {
      logSystemEvent("ESPNOW", "paired: new peer identity persisted " MAC_FMT,
                     MAC_ARGS(msg->senderMac));
      // Another key at this address may be another device: it does not
      // inherit the room and tags accepted for the one before it.
      espnowGroupAcceptanceClear(msg->senderMac, "its identity key is new");
    }
    logMac("KEY_EX_HELLO accepted from", msg->senderMac);
  }

  // Add to ESPNOW peer table so we can send REPLY.
  if (!ensureUnencryptedPeer(msg->senderMac)) return;

  // If status != 0 (conflict), emit a CONFIRM with the status instead of REPLY.
  if (confirmStatus != 0) {
    sendKeyExConfirm(msg->senderMac, msg->meshFingerprint, confirmStatus);
    return;
  }

  // Build REPLY: our pubkey + HMAC over (fp || ourMac || ourPub).
  const auto& self = espnowIdentityGet();
  if (!self.valid) {
    ERROR_ESPNOWF("KEY_EX_HELLO: own identity not loaded; cannot REPLY");
    return;
  }
  V4PayloadKeyExReply reply = {};
  reply.meshFingerprint = msg->meshFingerprint;
  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  memcpy(reply.responderMac, selfMac, 6);
  memcpy(reply.responderPubEd25519, self.pub, 32);
  if (!computeKeyExHmac(msg->meshFingerprint, selfMac, self.pub, bootKey, reply.hmac)) {
    ERROR_ESPNOWF("KEY_EX_HELLO: HMAC compute failed for REPLY");
    return;
  }

  v4_send_frame(msg->senderMac, ESPNOW_V4_TYPE_KEY_EX_REPLY,
                ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&reply), sizeof(reply), 1);
}

void v4hKeyExReply(const V4RxCtx& ctx) {
  // The receive table admits exactly 72 bytes (V4_OPC_LEN_EXACT).
  V4PayloadKeyExReply replyMsg;
  ctx.read(replyMsg);
  const V4PayloadKeyExReply* msg = &replyMsg;

  const uint8_t* bootKey = nullptr;
  int meshSlot = verifyKeyExPayload(ctx, msg->meshFingerprint,
                                    msg->responderMac, msg->responderPubEd25519,
                                    msg->hmac, &bootKey);
  if (meshSlot < 0) {
    WARN_ESPNOWF("KEY_EX_REPLY rejected (reason=%d) from " MAC_FMT,
                 meshSlot,
                 MAC_ARGS(msg->responderMac));
    return;
  }

  // The peer was unpaired while our HELLO was out (espnowKeyExCancel): its
  // answer must not bring the identity back.
  if (keyExCancelled(msg->responderMac)) {
    INFO_ESPNOWF("KEY_EX_REPLY from " MAC_FMT " ignored: that key exchange was called off (unpaired)",
                 MAC_ARGS(msg->responderMac));
    return;
  }

  const PeerIdentity* existing = peerIdentityFindByMac(msg->responderMac);
  bool conflict = existing && memcmp(existing->longTermPub, msg->responderPubEd25519, 32) != 0;
  if (conflict) {
    // Operator-initiated re-pair window authorizes a one-shot key replacement
    // for this MAC; otherwise refuse (symmetric with the HELLO-side guard).
    if (espnowConsumePairingWindow(msg->responderMac)) {
      WARN_ESPNOWF("KEY_EX_REPLY: peer presented new pubkey; re-pair window open — "
                   "replacing stored identity");
      conflict = false;
    }
  }
  if (conflict) {
    WARN_ESPNOWF("KEY_EX_REPLY: peer presented new pubkey, refusing overwrite — "
                 "signaling reject (status=2). Run 'espnowforget' or re-run 'espnowpairsecure'.");
    // Event log is min-interval gated — symmetric with the HELLO-side guard.
    static uint32_t sReplyConflictEventMs = 0;
    uint32_t nowMs = millis();
    if (sReplyConflictEventMs == 0 || (uint32_t)(nowMs - sReplyConflictEventMs) >= 60000UL) {
      sReplyConflictEventMs = nowMs ? nowMs : 1;
      logSystemEvent("ESPNOW", "SECURITY: peer " MAC_FMT " presented a DIFFERENT pubkey — identity overwrite refused",
                     MAC_ARGS(msg->responderMac));
    }
    // Tell the responder we rejected them — symmetric with the HELLO-side
    // conflict path. The peer is already in our hw table (we sent the HELLO
    // that triggered this REPLY), but ensure it before sending defensively.
    if (ensureUnencryptedPeer(msg->responderMac)) {
      sendKeyExConfirm(msg->responderMac, msg->meshFingerprint, 2);
    }
    return;
  }

  time_t now = time(nullptr);
  uint32_t bondedAt = (uint32_t)((now > 0) ? now : 0);
  // Event-log gate: only log when the stored identity actually changes (first
  // pairing or authorized key replacement) — a duplicate REPLY presents the
  // same key routinely (nothing is rewritten then) and must not log. Computed
  // BEFORE persist mutates the slot `existing` points into.
  bool identityNewOrReplaced = !existing ||
      memcmp(existing->longTermPub, msg->responderPubEd25519, 32) != 0;
  if (!peerIdentityPersist(msg->responderMac, (uint8_t)meshSlot,
                           msg->responderPubEd25519, bondedAt)) {
    ERROR_ESPNOWF("KEY_EX_REPLY: identity persist failed");
    return;
  }
  if (identityNewOrReplaced) {
    logSystemEvent("ESPNOW", "paired: new peer identity persisted " MAC_FMT,
                   MAC_ARGS(msg->responderMac));
    // As for a HELLO: a new key does not inherit the accepted room and tags.
    espnowGroupAcceptanceClear(msg->responderMac, "its identity key is new");
  }
  logMac("KEY_EX_REPLY accepted from", msg->responderMac);

  // Handshake resolved for us (initiator) — stop the HELLO retry timer (F6).
  keyExClearInFlight(msg->responderMac);
  // The peer answered: its automatic starts need no back-off any more.
  espnowHandshakeBackoffClear(msg->responderMac);

  if (!ensureUnencryptedPeer(msg->responderMac)) return;

  // Send CONFIRM (status=0) — KEY_EX complete from our (initiator) side.
  sendKeyExConfirm(msg->responderMac, msg->meshFingerprint, 0);

  // Encrypt-or-wait foundation (2026-05): if v4_send_encrypted_or_queue parked
  // any frames for this peer while we were handshaking (because there was no
  // peer identity at send time), kick SESSION_OPEN now. The pending frames
  // will drain via the existing pendingFrameDrainForPeer hook in the
  // SESSION_CONFIRM handler. No-op when there's nothing pending.
  // A second REPLY to the same HELLO (a retransmit answered twice) finds the
  // first one's SESSION_OPEN in flight; the kick then leaves it alone. One that
  // finds the session already ACTIVE (a late second answer, or a replay: a
  // REPLY's HMAC covers nothing fresh either) must not replace it, which a kick
  // on an ACTIVE session does: what is waiting goes out over the live session.
  const SessionState* live = sessionFindByPeer(msg->responderMac, (uint8_t)meshSlot);
  if (live && live->state == SESSION_ACTIVE) {
    pendingFrameDrainForPeer(msg->responderMac);
  } else if (pendingFrameHasForPeer(msg->responderMac)) {
    bool alreadyInFlight = false;
    if (!espnowSessionOpenInitiate(msg->responderMac, nullptr, &alreadyInFlight)) {
      WARN_ESPNOWF("KEY_EX_REPLY: pending frames exist but SESSION_OPEN kick failed "
                   "— frames will time out in pending-frame sweep");
    } else if (!alreadyInFlight) {
      INFO_ESPNOWF("KEY_EX_REPLY: pending frames for peer — kicked SESSION_OPEN to drain");
    }
  }
}

void v4hKeyExConfirm(const V4RxCtx& ctx) {
  // The receive table admits exactly 18 bytes (V4_OPC_LEN_EXACT).
  V4PayloadKeyExConfirm confirmMsg;
  ctx.read(confirmMsg);
  const V4PayloadKeyExConfirm* msg = &confirmMsg;

  // No HMAC on CONFIRM — it's just an acknowledgement, the prior REPLY's
  // HMAC already authenticated this peer. What we check here is that the
  // header origin matches the payload's confirmerMac, and that the sender is a
  // peer we already know. We do NOT read msg->meshFingerprint at all: mesh
  // scoping rides entirely on the dispatcher's header-level fingerprint gate,
  // which is itself skipped when the header fingerprint is 0.
  if (memcmp(ctx.h->origin, msg->confirmerMac, 6) != 0) {
    WARN_ESPNOWF("KEY_EX_CONFIRM: header.origin / payload.confirmerMac mismatch");
    return;
  }
  const PeerIdentity* p = peerIdentityFindByMac(msg->confirmerMac);
  if (!p) {
    WARN_ESPNOWF("KEY_EX_CONFIRM: from unknown peer; ignored");
    return;
  }

  // Terminal message — whether status=0 (ok) or status=2 (peer rejected our
  // pubkey), the handshake is resolved; stop any HELLO retry timer (F6). A
  // rejected handshake will never succeed, so retrying would be pointless.
  keyExClearInFlight(msg->confirmerMac);

  if (msg->status == 0) {
    // Finished on our (responder) side. A refusal (status 1 or 2) does not
    // end the back-off: such a peer answers every time and never completes.
    espnowHandshakeBackoffClear(msg->confirmerMac);
    INFO_ESPNOWF("KEY_EX with " MAC_FMT " complete "
                 "(remote pub fp: %02X%02X%02X%02X%02X%02X%02X%02X)",
                 MAC_ARGS(msg->confirmerMac),
                 msg->pubFingerprint[0], msg->pubFingerprint[1], msg->pubFingerprint[2],
                 msg->pubFingerprint[3], msg->pubFingerprint[4], msg->pubFingerprint[5],
                 msg->pubFingerprint[6], msg->pubFingerprint[7]);
  } else {
    WARN_ESPNOWF("KEY_EX with " MAC_FMT " reported status=%u "
                 "(0=ok, 1=hmac-fail, 2=pub-conflict)",
                 MAC_ARGS(msg->confirmerMac),
                 (unsigned)msg->status);
  }
}

// ============================================================================
// Initiator
// ============================================================================

// ----------------------------------------------------------------------------
// KEY_EX retry / timeout (F6)
//
// KEY_EX was previously fire-and-forget: espnowKeyExInitiate sent one HELLO and
// gave up if the REPLY never arrived (radio loss, peer busy). Now more visible
// since espnowpairsecure auto-kicks KEY_EX. We adopt the same shape as the
// pending-frame ring: a tiny fixed table of in-flight initiations + a sweep
// called from the periodic espnow tick that re-sends the HELLO on timeout and
// gives up after a bounded number of tries.
//
// We only need to retry the HELLO, not all three handshake messages: the
// responder persists the initiator's identity on HELLO receipt and is fully
// idempotent (a re-sent HELLO finds the same key stored and re-emits the
// REPLY). If the final CONFIRM is lost the responder already stored the
// identity, so a single HELLO-retry makes the whole exchange robust.
// ----------------------------------------------------------------------------

// Build + send a single KEY_EX_HELLO for an already-resolved mesh slot. Shared
// by the initial initiation and the retry sweep so neither duplicates the
// keygen/HMAC. Returns true on radio-send success.
static bool keyExSendHello(const uint8_t peerMac[6], int slot) {
  const MeshDerivedKeys* mk = meshKeysGet((uint8_t)slot);
  if (!mk) {
    ERROR_ESPNOWF("KEY_EX: mesh slot %d has no derived keys (passphrase not set?)", slot);
    return false;
  }
  const auto& self = espnowIdentityGet();
  if (!self.valid) {
    ERROR_ESPNOWF("KEY_EX: own identity not loaded");
    return false;
  }
  if (!ensureUnencryptedPeer(peerMac)) return false;

  uint8_t selfMac[6];
  espnowSelfStaMac(selfMac);

  V4PayloadKeyExHello hello = {};
  hello.meshFingerprint = gSettings.meshes[slot].fingerprint;
  memcpy(hello.senderMac, selfMac, 6);
  memcpy(hello.senderPubEd25519, self.pub, 32);
  if (!computeKeyExHmac(hello.meshFingerprint, selfMac, self.pub,
                        mk->bootstrapKey, hello.hmac)) {
    ERROR_ESPNOWF("KEY_EX: HMAC compute failed");
    return false;
  }

  bool ok = v4_send_frame(peerMac, ESPNOW_V4_TYPE_KEY_EX_HELLO,
                          ESPNOW_V4_FLAG_ACK_REQ,
                          generateMessageId(),
                          reinterpret_cast<const uint8_t*>(&hello), sizeof(hello), 1);
  if (ok) {
    INFO_ESPNOWF("KEY_EX_HELLO sent to " MAC_FMT " (mesh '%s', fp=0x%04X)",
                 MAC_ARGS(peerMac),
                 gSettings.meshes[slot].label.c_str(),
                 (unsigned)hello.meshFingerprint);
  } else {
    ERROR_ESPNOWF("KEY_EX_HELLO send failed");
  }
  return ok;
}

namespace {

constexpr uint8_t  kKeyExInFlightSlots = 4;     // matches pending-frame sizing
constexpr uint8_t  kKeyExMaxRetries    = 2;     // re-sends after the initial HELLO
constexpr uint32_t kKeyExRetryMs       = 1500;  // re-send if unresolved this long
static_assert(kKeyExHelloMinIntervalMs < kKeyExRetryMs,
              "the responder must answer the initiator's HELLO retry");

struct KeyExInFlight {
  bool     inUse;
  uint8_t  peerMac[6];
  int8_t   meshSlot;     // resolved slot, to rebuild the HELLO on retry
  uint8_t  retriesLeft;
  uint32_t lastSentMs;
  bool     automatic;    // not asked for by a user: give-up warning is rate-limited
  bool     cancelled;    // called off by an unpair: no retry, its REPLY is ignored
};

// Tiny table (4 × ~16 B). Registered on cmd_exec (espnowKeyExInitiate via the
// CLI / pairsecure); swept + cleared on the espnow task (RX handlers + tick).
// Lock-free per the codebase convention for these small tables: register fills
// all fields before setting inUse=true, so a concurrent sweep never observes a
// half-populated in-use slot; clear is a single-bool write.
KeyExInFlight gKeyExInFlight[kKeyExInFlightSlots] = {};

void keyExInFlightArm(const uint8_t peerMac[6], int slot, bool automatic) {
  KeyExInFlight* e = nullptr;
  for (auto& k : gKeyExInFlight)
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) { e = &k; break; }
  if (!e)
    for (auto& k : gKeyExInFlight)
      if (!k.inUse) { e = &k; break; }
  if (!e) {
    WARN_ESPNOWF("KEY_EX: in-flight table full — no retry coverage for this request");
    return;
  }
  memcpy(e->peerMac, peerMac, 6);
  e->meshSlot     = (int8_t)slot;
  e->retriesLeft  = kKeyExMaxRetries;
  e->lastSentMs   = (uint32_t)millis();
  e->automatic    = automatic;
  e->cancelled    = false;  // a new exchange replaces one that was called off
  e->inUse        = true;   // publish last
}

// Automatic key exchanges to a peer that is not there give up again and
// again; their warning comes once per 15 minutes with the count held back.
constexpr uint32_t kKeyExGiveUpWarnWindowMs = 15UL * 60UL * 1000UL;
WarnLimiter gKeyExGiveUpWarn = {};

// Back-off table for automatic handshake starts. Taken on espnow_task (bond
// heartbeat) and cmd_exec (bond commands); cleared from RX handlers on
// espnow_task, session completion on cmd_exec and user commands. Every access
// holds the spinlock: a few compares on 16 entries, no calls inside.
static_assert(kHandshakeBackoffSlots >= MESH_PEER_MAX,
              "one back-off entry per paired device, so a full table never gives up a waiting one");
HandshakeBackoffTable gHandshakeBackoff = {};
portMUX_TYPE gHandshakeBackoffLock = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

static void keyExClearInFlight(const uint8_t peerMac[6]) {
  for (auto& k : gKeyExInFlight)
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) { k.inUse = false; return; }
}

// Unpair (cmd_exec): the entry stays, marked, until one retry interval after
// its last HELLO, so a REPLY to that HELLO is still recognised and ignored.
// Single-bool write, read by the sweep and the REPLY handler on espnow_task.
void espnowKeyExCancel(const uint8_t peerMac[6]) {
  if (!peerMac) return;
  for (auto& k : gKeyExInFlight)
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) { k.cancelled = true; return; }
}

static bool keyExCancelled(const uint8_t peerMac[6]) {
  for (auto& k : gKeyExInFlight)
    if (k.inUse && k.cancelled && memcmp(k.peerMac, peerMac, 6) == 0) return true;
  return false;
}

// Public accessor — see header for contract.
// One called off by an unpair does not count: a new start replaces it.
bool keyExIsInFlight(const uint8_t peerMac[6]) {
  if (!peerMac) return false;
  for (auto& k : gKeyExInFlight)
    if (k.inUse && !k.cancelled && memcmp(k.peerMac, peerMac, 6) == 0) return true;
  return false;
}

void keyExRetrySweep(uint32_t nowMs) {
  for (auto& k : gKeyExInFlight) {
    if (!k.inUse) continue;
    if ((uint32_t)(nowMs - k.lastSentMs) < kKeyExRetryMs) continue;
    if (k.cancelled) {   // unpaired: nothing is re-sent, and the last HELLO had its interval
      k.inUse = false;
      continue;
    }
    if (k.retriesLeft == 0) {
      uint32_t held = 0;
      if (!k.automatic) {
        WARN_ESPNOWF("KEY_EX: no REPLY from " MAC_FMT " after %u attempts — "
                     "giving up (run espnowkeyex to retry)",
                     MAC_ARGS(k.peerMac),
                     (unsigned)(kKeyExMaxRetries + 1));
      } else if (warnLimiterTake(gKeyExGiveUpWarn, nowMs, kKeyExGiveUpWarnWindowMs, &held)) {
        WARN_ESPNOWF("KEY_EX: no REPLY from " MAC_FMT " after %u attempts — "
                     "giving up; automatic tries back off up to 5 min "
                     "(run espnowkeyex to retry now; %lu more not shown in the last 15 min)",
                     MAC_ARGS(k.peerMac),
                     (unsigned)(kKeyExMaxRetries + 1),
                     (unsigned long)held);
      }
      k.inUse = false;
      continue;
    }
    k.retriesLeft--;
    INFO_ESPNOWF("KEY_EX: no REPLY from " MAC_FMT " yet — retrying HELLO (%u left)",
                 MAC_ARGS(k.peerMac),
                 (unsigned)k.retriesLeft);
    keyExSendHello(k.peerMac, k.meshSlot);   // failure just retries next tick / eventually gives up
    k.lastSentMs = nowMs;
  }
}

static bool keyExInitiate(const uint8_t peerMac[6], const char* meshLabel, bool automatic) {
  if (!gEspNow || !gEspNow->initialized) {
    ERROR_ESPNOWF("KEY_EX initiate: ESPNOW not initialized");
    return false;
  }
  int slot = resolveMeshSlot(meshLabel);
  if (slot < 0) {
    ERROR_ESPNOWF("KEY_EX initiate: no enabled mesh matches label '%s'",
                  meshLabel ? meshLabel : "(default)");
    return false;
  }
  bool ok = keyExSendHello(peerMac, slot);
  if (ok) keyExInFlightArm(peerMac, slot, automatic);  // arm retry/timeout
  return ok;
}

bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel) {
  return keyExInitiate(peerMac, meshLabel, true);
}

bool espnowKeyExInitiateManual(const uint8_t peerMac[6], const char* meshLabel) {
  espnowHandshakeBackoffClear(peerMac);
  return keyExInitiate(peerMac, meshLabel, false);
}

void espnowHandshakeBackoffClear(const uint8_t peerMac[6]) {
  if (!peerMac) return;
  portENTER_CRITICAL(&gHandshakeBackoffLock);
  handshakeBackoffClear(gHandshakeBackoff, peerMac);
  portEXIT_CRITICAL(&gHandshakeBackoffLock);
}

void espnowHandshakeHeard(const uint8_t peerMac[6]) {
  if (!peerMac) return;
  const uint32_t nowMs = (uint32_t)millis();
  portENTER_CRITICAL(&gHandshakeBackoffLock);
  handshakeBackoffHeard(gHandshakeBackoff, peerMac, nowMs);
  portEXIT_CRITICAL(&gHandshakeBackoffLock);
}

bool espnowHandshakeAutoStart(const uint8_t peerMac[6]) {
  if (!gEspNow || !gEspNow->initialized || !peerMac) return false;
  const PeerIdentity* pid = peerIdentityFindByMac(peerMac);
  if (!pid) {
    if (keyExIsInFlight(peerMac)) return false;
  } else {
    const SessionState* s = sessionFindByPeer(peerMac, pid->meshId);
    // ACTIVE needs nothing; ESTABLISHING / REKEYING is already in flight.
    if (s && s->state != SESSION_FREE && s->state != SESSION_CLOSED) return false;
  }
  const uint32_t nowMs = (uint32_t)millis();
  portENTER_CRITICAL(&gHandshakeBackoffLock);
  const bool due = handshakeBackoffTake(gHandshakeBackoff, peerMac, nowMs);
  portEXIT_CRITICAL(&gHandshakeBackoffLock);
  if (!due) return false;
  return pid ? espnowSessionOpenInitiate(peerMac, nullptr)
             : keyExInitiate(peerMac, nullptr, true);
}

bool espnowHandshakeStartNow(const uint8_t peerMac[6]) {
  if (!gEspNow || !gEspNow->initialized || !peerMac) return false;
  espnowHandshakeBackoffClear(peerMac);
  const PeerIdentity* pid = peerIdentityFindByMac(peerMac);
  // No identity: a manual key exchange (one in flight is re-sent and re-armed
  // as manual, so its give-up is printed).
  if (!pid) return keyExInitiate(peerMac, nullptr, false);
  const SessionState* s = sessionFindByPeer(peerMac, pid->meshId);
  if (s && s->state == SESSION_ACTIVE) return false;   // up already: nothing to start
  // Starts SESSION_OPEN, or leaves one in flight alone; either way its
  // timeout is reported.
  const bool ok = espnowSessionOpenInitiate(peerMac, nullptr);
  if (ok) sessionMarkUserStarted(peerMac);
  return ok;
}

// ============================================================================
// Phase 3.4 — SESSION handshake
// ============================================================================

// This image's capability word (System_ESPNow_SessionCapsCore.h), sent in our
// SESSION_OPEN and SESSION_CONFIRM. It describes the build only, never a
// setting, so it is the same for the whole boot. Any task (a read).
uint32_t espnowLocalSessionCaps() {
  bool sensors = false;
  for (int t = 0; t < REMOTE_SENSOR_MAX && !sensors; t++) {
    sensors = sensorTypeInThisBuild(static_cast<RemoteSensorType>(t));
  }
  return sessioncaps::buildCaps(ENABLE_BONDED_MODE != 0, sensors, /*collect=*/ENABLE_ESPNOW_COLLECT != 0,
                                /*filesServe=*/ENABLE_ESPNOW_SEND != 0);
}

namespace {

constexpr const char* kOpenLabel    = "v4-sopen:";   // 9 bytes including ':'
constexpr const char* kConfirmLabel = "v4-sconf:";   // 9 bytes
constexpr const char* kRekeyLabel   = "v4-rekey:";   // 9 bytes (Phase 3.6)
constexpr size_t      kLabelLen     = 9;

// Signed transcript lengths: the label, then the frame's fields up to its
// signature (System_ESPNow_Wire.h, "SESSION establishment payloads").
constexpr size_t kOpenTranscriptLen    = kLabelLen + 2 + 6 + 6 + 32 + 16 + 4;
constexpr size_t kConfirmTranscriptLen = kLabelLen + 2 + 6 + 6 + 32 + 16 + 16 + 4;
static_assert(kOpenTranscriptLen == 75, "OPEN transcript is 75 bytes");
static_assert(kConfirmTranscriptLen == 91, "CONFIRM transcript is 91 bytes");

// Writes the transcript fields in order; integers little endian.
struct TranscriptCursor {
  uint8_t* p;
  void bytes(const void* src, size_t n) { memcpy(p, src, n); p += n; }
  void u16(uint16_t v) { *p++ = (uint8_t)(v & 0xFF); *p++ = (uint8_t)(v >> 8); }
  void u32(uint32_t v) { for (int i = 0; i < 4; i++) *p++ = (uint8_t)(v >> (8 * i)); }
};

// Build the OPEN transcript for sign/verify:
//   "v4-sopen:" || sessionId(2 LE) || initMac(6) || respMac(6) || eph(32) || nonceA(16) ||
//   sessionCaps(4 LE)
// Total 9 + 2 + 6 + 6 + 32 + 16 + 4 = 75 bytes.
void buildOpenTranscript(uint8_t out[kOpenTranscriptLen],
                         uint16_t sessionId,
                         const uint8_t initMac[6],
                         const uint8_t respMac[6],
                         const uint8_t eph[32],
                         const uint8_t nonceA[16],
                         uint32_t sessionCaps) {
  TranscriptCursor c{out};
  c.bytes(kOpenLabel, kLabelLen);
  c.u16(sessionId);
  c.bytes(initMac, 6);
  c.bytes(respMac, 6);
  c.bytes(eph, 32);
  c.bytes(nonceA, 16);
  c.u32(sessionCaps);
}

// Build the CONFIRM transcript:
//   "v4-sconf:" || sessionId(2 LE) || respMac(6) || initMac(6) || eph(32) ||
//   nonceA(16) || nonceB(16) || sessionCaps(4 LE)
// Total 9 + 2 + 6 + 6 + 32 + 16 + 16 + 4 = 91 bytes.
void buildConfirmTranscript(uint8_t out[kConfirmTranscriptLen],
                            uint16_t sessionId,
                            const uint8_t respMac[6],
                            const uint8_t initMac[6],
                            const uint8_t eph[32],
                            const uint8_t nonceA[16],
                            const uint8_t nonceB[16],
                            uint32_t sessionCaps) {
  TranscriptCursor c{out};
  c.bytes(kConfirmLabel, kLabelLen);
  c.u16(sessionId);
  c.bytes(respMac, 6);
  c.bytes(initMac, 6);
  c.bytes(eph, 32);
  c.bytes(nonceA, 16);
  c.bytes(nonceB, 16);
  c.u32(sessionCaps);
}

// Build the REKEY transcript:
//   "v4-rekey:" || sessionId(2 LE) || senderMac(6) || receiverMac(6) ||
//   newEphX25519Pub(32) || nonceRekey(16) || prevTxSeqAtRekey(4 LE)
// Total 9 + 2 + 6 + 6 + 32 + 16 + 4 = 75 bytes.
void buildRekeyTranscript(uint8_t out[75],
                          uint16_t sessionId,
                          const uint8_t senderMac[6],
                          const uint8_t receiverMac[6],
                          const uint8_t newEph[32],
                          const uint8_t nonceRekey[16],
                          uint32_t prevTxSeqAtRekey) {
  memcpy(out, kRekeyLabel, kLabelLen);
  out[9]  = (uint8_t)(sessionId & 0xFF);
  out[10] = (uint8_t)((sessionId >> 8) & 0xFF);
  memcpy(out + 11, senderMac, 6);
  memcpy(out + 17, receiverMac, 6);
  memcpy(out + 23, newEph, 32);
  memcpy(out + 55, nonceRekey, 16);
  out[71] = (uint8_t)(prevTxSeqAtRekey & 0xFF);
  out[72] = (uint8_t)((prevTxSeqAtRekey >> 8)  & 0xFF);
  out[73] = (uint8_t)((prevTxSeqAtRekey >> 16) & 0xFF);
  out[74] = (uint8_t)((prevTxSeqAtRekey >> 24) & 0xFF);
}

}  // namespace

// ----------------------------------------------------------------------------
// Deferred-to-cmd_exec work structs + worker functions.
//
// Ed25519 sign+verify each use ~3 KB of libsodium internal stack. espnow_task's
// whole budget is 6656 B (ESPNOW_HB_STACK_WORDS — the values in
// System_TaskUtils.h are BYTES, not words, despite the name), so one signature
// is most of the task's stack; on top of the ambient RX depth it overflowed
// during SESSION_OPEN handling. cmd_exec has 8192 B and is single-threaded
// w.r.t. the existing CLI peak (~4.25 KB), so deferring keeps everything under
// budget without bumping any task's stack. The margin is real but not roomy —
// ~3 KB of libsodium against an 8 KB budget is worth re-measuring if the
// deferred work below grows.
//
// On-wire RX handlers (v4hSessionOpen/Confirm) do only size validation +
// PSRAM copy + enqueue. Heavy lifting runs in runDeferredSessionOpen/Confirm
// on cmd_exec. The work struct + callback own their own lifetime.
// ----------------------------------------------------------------------------

namespace {

struct DeferredSessionOpenWork {
  V4PayloadSessionOpen msg;
  uint8_t              headerOrigin[6];
};

struct DeferredSessionConfirmWork {
  V4PayloadSessionConfirm msg;
  uint8_t                 headerOrigin[6];
};

// Send the SESSION_OPEN of slot `s` (ESTABLISHING, our initiation) once more,
// unchanged: same sessionId, ephemeral key and nonce, so the peer derives the
// same keys whichever copy it answers, and a peer that already answered the
// first copy ignores this one as re-delivered (sessionOpenIdStale). The slot
// keeps the ephemeral private key in aeadKeyRx and nonceA in aeadKeyTx[0..15]
// until the CONFIRM (espnowSessionOpenInitiate); the public key is rebuilt
// from the private one. The CONFIRM budget restarts with the copy.
bool sessionOpenSendAgain(SessionState* s, const uint8_t peerMac[6]) {
  if (!s || s->state != SESSION_ESTABLISHING) return false;
  const auto& self = espnowIdentityGet();
  if (!self.valid) return false;
  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  V4PayloadSessionOpen open = {};
  open.sessionId = s->sessionId;
  memcpy(open.initiatorMac, selfMac, 6);
  memcpy(open.responderMac, peerMac, 6);
  if (!espnowCryptoX25519Public(open.ephX25519Pub, s->aeadKeyRx)) return false;
  memcpy(open.nonceA, s->aeadKeyTx, 16);
  open.sessionCaps = espnowLocalSessionCaps();   // fixed for the boot: the same word as the first copy
  uint8_t transcript[kOpenTranscriptLen];
  buildOpenTranscript(transcript, open.sessionId, selfMac, peerMac, open.ephX25519Pub, open.nonceA,
                      open.sessionCaps);
  if (!espnowCryptoEd25519Sign(open.signature, transcript, sizeof(transcript), self.sec)) return false;
  s->openResent = 1;
  s->lastUseMs = (uint32_t)millis();
  return v4_send_frame(peerMac, ESPNOW_V4_TYPE_SESSION_OPEN,
                       ESPNOW_V4_FLAG_ACK_REQ,
                       generateMessageId(),
                       reinterpret_cast<const uint8_t*>(&open), sizeof(open), 1);
}

void runDeferredSessionOpen(void* arg) {
  auto* w = static_cast<DeferredSessionOpenWork*>(arg);
  const V4PayloadSessionOpen* msg = &w->msg;

  if (memcmp(w->headerOrigin, msg->initiatorMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_OPEN: header.origin / payload.initiatorMac mismatch");
    free(w);
    return;
  }

  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  if (memcmp(msg->responderMac, selfMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_OPEN: responderMac is not us — ignoring");
    free(w);
    return;
  }

  const PeerIdentity* peer = peerIdentityFindByMac(msg->initiatorMac);
  if (!peer) {
    uint16_t n = sessionRejectLogCount(msg->initiatorMac);
    if (n) {
      // Split across two queued lines so the actionable fix isn't lost to the
      // 256-byte DEBUG_MSG_SIZE [CUT] truncation.
      WARN_ESPNOWF("SESSION_OPEN from " MAC_FMT " rejected (x%u in 30s): no stored "
                   "identity for this peer - it was re-flashed/factory-erased (identity key rotated) "
                   "or KEY_EX never completed.",
                   MAC_ARGS(msg->initiatorMac), (unsigned)n);
      WARN_ESPNOWF("  -> IDENTITY problem, NOT a passphrase one (passphrase isn't checked here). "
                   "Fix: run 'espnowforget' then 'espnowpairsecure' on BOTH devices to re-pair.");
    }
    free(w);
    return;
  }

  uint8_t transcript[kOpenTranscriptLen];
  buildOpenTranscript(transcript, msg->sessionId,
                      msg->initiatorMac, msg->responderMac,
                      msg->ephX25519Pub, msg->nonceA, msg->sessionCaps);
  if (!espnowCryptoEd25519Verify(msg->signature, transcript, sizeof(transcript),
                                 peer->longTermPub)) {
    uint16_t n = sessionRejectLogCount(msg->initiatorMac);
    if (n) {
      // Split across two queued lines (see 256-byte DEBUG_MSG_SIZE note above).
      WARN_ESPNOWF("SESSION_OPEN from " MAC_FMT " rejected (x%u in 30s): signature "
                   "does NOT match the identity key we stored - the peer's key CHANGED "
                   "(re-flash/factory-erase/re-setup).",
                   MAC_ARGS(msg->initiatorMac), (unsigned)n);
      WARN_ESPNOWF("  -> Not a passphrase issue; we won't auto-accept a changed key "
                   "(anti-key-substitution). Authorize it: 'espnowpairsecure' on THIS device, then "
                   "re-handshake from the peer.");
    }
    free(w);
    return;
  }

  SessionState* existing = sessionFindByPeer(msg->initiatorMac, peer->meshId);

  // --- Freshness: a re-delivered SESSION_OPEN never resets a live session ---
  // The signed OPEN stays valid as long as the identity keys do, and the
  // dispatcher's dedup ring forgets a frame after 5 s. A genuine initiator
  // proposes a fresh random sessionId every time (a peer that rebooted or
  // dropped its slot included), so an OPEN carrying this slot's current id, or
  // one it held before, is a copy of an old frame. Accepting it would zero the
  // live keys and install ones the initiator cannot derive.
  if (existing && existing->state != SESSION_ESTABLISHING &&
      sessionOpenIdStale(existing, msg->sessionId)) {
    uint16_t n = sessionRejectLogCount(msg->initiatorMac);
    if (n) {
      WARN_ESPNOWF("SESSION_OPEN from " MAC_FMT " ignored (x%u in 30s): sessionId=%u "
                   "is this peer's live or a recent session - re-delivered OPEN; live session kept",
                   MAC_ARGS(msg->initiatorMac),
                   (unsigned)n, (unsigned)msg->sessionId);
    }
    free(w);
    return;
  }

  // --- Glare resolution (simultaneous SESSION_OPEN) ---
  // If we already have our OWN in-flight initiated handshake to this peer — a
  // slot still in ESTABLISHING (the responder path below flips to ACTIVE
  // synchronously, so ESTABLISHING here means an initiation we sent that is
  // awaiting its CONFIRM) — then both sides opened at once. Without a tiebreaker
  // each side would accept the other's OPEN and clobber its own initiation, so
  // the two ends would settle on DIFFERENT sessionIds and every later
  // SESSION_FRAME would be dropped ("no active session"). Break the tie with the
  // same MAC ordering used everywhere else: the A-side (numerically lower MAC) is
  // the canonical initiator and KEEPS its own session (ignore the peer's OPEN —
  // our CONFIRM will win on both ends); the B-side falls through and yields,
  // accepting the peer's OPEN. Both then converge on the A-initiated session.
  //
  // The peer's OPEN also proves it hears now. Our own OPEN may have gone out
  // before it could (it was restarting, or its radio was busy): the peer then
  // never saw it, and both sides would sit out the CONFIRM budget (bench
  // 2026-11-18: glare, then a 6 s ESTABLISHING timeout). So the A side sends
  // its OPEN once more, unchanged (sessionOpenSendAgain), at most once per
  // attempt: a peer that missed it yields to it as above; one that already
  // answered it ignores the copy as re-delivered.
  if (existing && existing->state == SESSION_ESTABLISHING &&
      sessionIsASide(selfMac, msg->initiatorMac)) {
    const bool resend = !existing->openResent;
    WARN_ESPNOWF("SESSION_OPEN glare with " MAC_FMT " — we are A-side, "
                 "keeping our initiation (ignoring peer sessionId=%u)%s",
                 MAC_ARGS(msg->initiatorMac),
                 (unsigned)msg->sessionId,
                 resend ? "; sending ours once more" : "");
    if (resend) (void)sessionOpenSendAgain(existing, msg->initiatorMac);
    free(w);
    return;
  }

  SessionState* s = sessionAllocate(msg->initiatorMac, peer->meshId);
  if (!s) { free(w); return; }
  s->sessionId    = msg->sessionId;
  s->myDirection  = sessionIsASide(selfMac, msg->initiatorMac) ? 0 : 1;
  // The initiator's capability word, verified above. Stored before the slot
  // goes ACTIVE, so nothing that sees the session up sees it without the word.
  // Confirmed only once a sealed frame of this session opens (an OPEN can be
  // a replayed copy; SessionCapsCore.h).
  s->peerSessionCaps = msg->sessionCaps;
  s->peerSessionCapsConfirmed = 0;

  uint8_t ephPub[32], ephSec[32], nonceB[16];
  if (!espnowCryptoX25519Keygen(ephPub, ephSec)) {
    ERROR_ESPNOWF("SESSION_OPEN: X25519 keygen failed");
    sessionClear(s);
    free(w);
    return;
  }
  espnowCryptoRandomBytes(nonceB, sizeof(nonceB));

  uint8_t shared[32];
  if (!espnowCryptoX25519Shared(shared, ephSec, msg->ephX25519Pub)) {
    ERROR_ESPNOWF("SESSION_OPEN: X25519 ECDH failed (bad peer eph?)");
    sodium_memzero(ephSec, sizeof(ephSec));
    sodium_memzero(shared, sizeof(shared));
    sessionClear(s);
    free(w);
    return;
  }
  sodium_memzero(ephSec, sizeof(ephSec));
  if (!sessionDeriveAeadKeys(s, shared)) {
    sodium_memzero(shared, sizeof(shared));
    sessionClear(s);
    free(w);
    return;
  }
  // Bond token already derived inside sessionDeriveAeadKeys above (per-session).
  sodium_memzero(shared, sizeof(shared));

  V4PayloadSessionConfirm conf = {};
  conf.sessionId = msg->sessionId;
  memcpy(conf.responderMac, selfMac, 6);
  memcpy(conf.initiatorMac, msg->initiatorMac, 6);
  memcpy(conf.ephX25519Pub, ephPub, 32);
  memcpy(conf.nonceA, msg->nonceA, 16);
  memcpy(conf.nonceB, nonceB, 16);
  conf.sessionCaps = espnowLocalSessionCaps();

  uint8_t confTranscript[kConfirmTranscriptLen];
  buildConfirmTranscript(confTranscript, msg->sessionId,
                         selfMac, msg->initiatorMac,
                         ephPub, msg->nonceA, nonceB, conf.sessionCaps);
  const auto& self = espnowIdentityGet();
  if (!self.valid ||
      !espnowCryptoEd25519Sign(conf.signature, confTranscript,
                               sizeof(confTranscript), self.sec)) {
    ERROR_ESPNOWF("SESSION_OPEN: own signature failed");
    sessionClear(s);
    free(w);
    return;
  }

  s->state           = SESSION_ACTIVE;
  s->establishedAtMs = (uint32_t)millis();
  s->lastUseMs       = s->establishedAtMs;
  INFO_ESPNOWF("SESSION established (responder) with " MAC_FMT " sessionId=%u dir=%c",
               MAC_ARGS(msg->initiatorMac),
               (unsigned)msg->sessionId, s->myDirection == 0 ? 'A' : 'B');

  v4_send_frame(msg->initiatorMac, ESPNOW_V4_TYPE_SESSION_CONFIRM,
                ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&conf), sizeof(conf), 1);
  // Frames this device queued for the peer (our own SESSION_OPEN lost the
  // glare, or crossed this one) are not drained here: the initiator has not
  // handled the CONFIRM yet and would drop them. The periodic tick sends them
  // once the session is settled (pendingFrameDrainSettled).
  // Session is ACTIVE on our side (responder) — tell bond logic so it can drive
  // the bonded relationship over the encrypted session (discovery via session,
  // not plaintext heartbeat).
  bondNotifySessionEstablished(msg->initiatorMac);
  free(w);
}

void runDeferredSessionConfirm(void* arg) {
  auto* w = static_cast<DeferredSessionConfirmWork*>(arg);
  const V4PayloadSessionConfirm* msg = &w->msg;

  if (memcmp(w->headerOrigin, msg->responderMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_CONFIRM: header.origin / responderMac mismatch");
    free(w);
    return;
  }
  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  if (memcmp(msg->initiatorMac, selfMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_CONFIRM: initiatorMac is not us");
    free(w);
    return;
  }

  SessionState* s = sessionFindBySessionId(msg->sessionId, msg->responderMac);
  if (!s || s->state != SESSION_ESTABLISHING) {
    WARN_ESPNOWF("SESSION_CONFIRM: no in-flight session for sessionId=%u peer=" MAC_FMT,
                 (unsigned)msg->sessionId,
                 MAC_ARGS(msg->responderMac));
    free(w);
    return;
  }

  if (memcmp(msg->nonceA, s->aeadKeyTx /*storage reused for stashed nonceA*/, 16) != 0) {
    WARN_ESPNOWF("SESSION_CONFIRM: nonceA mismatch — replay or wrong session");
    sessionClear(s);
    free(w);
    return;
  }

  const PeerIdentity* peer = peerIdentityFindByMac(msg->responderMac);
  if (!peer) {
    WARN_ESPNOWF("SESSION_CONFIRM: no peer identity for responder");
    sessionClear(s);
    free(w);
    return;
  }

  uint8_t transcript[kConfirmTranscriptLen];
  buildConfirmTranscript(transcript, msg->sessionId,
                         msg->responderMac, msg->initiatorMac,
                         msg->ephX25519Pub, msg->nonceA, msg->nonceB, msg->sessionCaps);
  if (!espnowCryptoEd25519Verify(msg->signature, transcript, sizeof(transcript),
                                 peer->longTermPub)) {
    WARN_ESPNOWF("SESSION_CONFIRM: Ed25519 verify failed");
    sessionClear(s);
    free(w);
    return;
  }

  uint8_t shared[32];
  if (!espnowCryptoX25519Shared(shared, s->aeadKeyRx /*stashed ephSec*/,
                                msg->ephX25519Pub)) {
    ERROR_ESPNOWF("SESSION_CONFIRM: X25519 ECDH failed");
    sodium_memzero(shared, sizeof(shared));
    sessionClear(s);
    free(w);
    return;
  }
  sodium_memzero(s->aeadKeyTx, sizeof(s->aeadKeyTx));
  sodium_memzero(s->aeadKeyRx, sizeof(s->aeadKeyRx));

  if (!sessionDeriveAeadKeys(s, shared)) {
    sodium_memzero(shared, sizeof(shared));
    sessionClear(s);
    free(w);
    return;
  }
  // Bond token already derived inside sessionDeriveAeadKeys above (per-session).
  sodium_memzero(shared, sizeof(shared));

  // The responder's capability word, verified above and bound to our fresh
  // nonceA: confirmed at once. Stored before the slot goes ACTIVE, so the
  // bond tick (espnow_task) never sees this session up without it.
  s->peerSessionCaps = msg->sessionCaps;
  s->peerSessionCapsConfirmed = 1;
  s->state           = SESSION_ACTIVE;
  s->establishedAtMs = (uint32_t)millis();
  s->lastUseMs       = s->establishedAtMs;
  INFO_ESPNOWF("SESSION established (initiator) with " MAC_FMT " sessionId=%u dir=%c",
               MAC_ARGS(msg->responderMac),
               (unsigned)msg->sessionId, s->myDirection == 0 ? 'A' : 'B');
  // Phase 3.5 step 3 — drain any frames the app queued while we were handshaking.
  // Runs on cmd_exec (same task that handled the heavy crypto above), so
  // the drain's session-wrap + esp_now_send fits in this task's stack.
  pendingFrameDrainForPeer(msg->responderMac);
  // Session ACTIVE on our side (initiator) — notify bond logic (discovery via
  // the encrypted session; master kicks the capability sync from here).
  bondNotifySessionEstablished(msg->responderMac);
  free(w);
}

}  // namespace

// On-RX handlers — espnow_task scope. Lightweight: size-check + PSRAM copy +
// enqueue. All heavy crypto runs in the deferred function on cmd_exec.
void v4hSessionOpen(const V4RxCtx& ctx) {
  // The receive table admits exactly sizeof(V4PayloadSessionOpen) (V4_OPC_LEN_EXACT).
  auto* w = static_cast<DeferredSessionOpenWork*>(
      ps_alloc(sizeof(DeferredSessionOpenWork), AllocPref::PreferPsram, "espnow.sopen.defer"));
  if (!w) {
    ERROR_ESPNOWF("SESSION_OPEN: PSRAM alloc failed (defer drop)");
    return;
  }
  ctx.read(w->msg);
  memcpy(w->headerOrigin, ctx.h->origin, 6);
  if (!submitDeferredToCmdExec(runDeferredSessionOpen, w)) {
    ERROR_ESPNOWF("SESSION_OPEN: cmd_exec queue full, dropping");
    free(w);
  }
}

void v4hSessionConfirm(const V4RxCtx& ctx) {
  // The receive table admits exactly sizeof(V4PayloadSessionConfirm) (V4_OPC_LEN_EXACT).
  auto* w = static_cast<DeferredSessionConfirmWork*>(
      ps_alloc(sizeof(DeferredSessionConfirmWork), AllocPref::PreferPsram, "espnow.sconf.defer"));
  if (!w) {
    ERROR_ESPNOWF("SESSION_CONFIRM: PSRAM alloc failed (defer drop)");
    return;
  }
  ctx.read(w->msg);
  memcpy(w->headerOrigin, ctx.h->origin, 6);
  if (!submitDeferredToCmdExec(runDeferredSessionConfirm, w)) {
    ERROR_ESPNOWF("SESSION_CONFIRM: cmd_exec queue full, dropping");
    free(w);
  }
}

bool espnowSessionOpenInitiate(const uint8_t peerMac[6], const char* meshLabel,
                               bool* outAlreadyInFlight) {
  if (outAlreadyInFlight) *outAlreadyInFlight = false;
  if (!gEspNow || !gEspNow->initialized) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: ESPNOW not initialized");
    return false;
  }
  if (!sessionsInit()) return false;

  // Peer must already have an identity record from KEY_EX (3.3).
  const PeerIdentity* peer = peerIdentityFindByMac(peerMac);
  if (!peer) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: no peer identity for " MAC_FMT " "
                  "— run 'espnowkeyex' first",
                  MAC_ARGS(peerMac));
    return false;
  }

  const auto& self = espnowIdentityGet();
  if (!self.valid) return false;
  if (!ensureUnencryptedPeer(peerMac)) return false;

  // Claim the slot, unless a handshake to this peer is already in flight.
  // Callers kick this whenever they find the session not ACTIVE (the
  // multi-frame sender, the bond sender, the file and long-text senders), and
  // that includes the moments a SESSION_OPEN of ours awaits its CONFIRM
  // (ESTABLISHING) or a REKEY awaits its answer (REKEYING, still on working
  // keys). sessionAllocate on such a slot zeroes its keys and takes a new
  // sessionId, so the CONFIRM or REKEY reply on its way would no longer match:
  // leave the slot exactly as it is and report it. The check and the claim
  // share one lock hold, so two callers on different tasks cannot both start
  // one. An ACTIVE session is still replaced: asking for a handshake over it
  // is the explicit re-handshake ('espnowsessionopen').
  SessionState* s = nullptr;
  bool inFlight = false;
  {
    EspNowTxGuard claim("sessOpenClaim");
    inFlight = sessionHandshakeInFlight(sessionFindByPeer(peerMac, peer->meshId),
                                        (uint32_t)millis());
    if (!inFlight) s = sessionAllocate(peerMac, peer->meshId);
  }
  if (inFlight) {
    if (outAlreadyInFlight) *outAlreadyInFlight = true;
    DEBUG_ESPNOWF("SESSION_OPEN initiate: handshake already in flight to " MAC_FMT
                  " - left alone",
                  MAC_ARGS(peerMac));
    return true;
  }
  if (!s) return false;

  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  s->myDirection = sessionIsASide(selfMac, peerMac) ? 0 : 1;

  // Random sessionId in [1, 0xFFFE]. randombytes_uniform returns [0, n).
  uint8_t idBytes[4];
  espnowCryptoRandomBytes(idBytes, 4);
  uint16_t sessionId = (uint16_t)((idBytes[0] | (idBytes[1] << 8)) & 0xFFFF);
  if (sessionId == 0)      sessionId = 1;
  if (sessionId == 0xFFFF) sessionId = 0xFFFE;
  s->sessionId = sessionId;

  uint8_t ephPub[32], ephSec[32], nonceA[16];
  if (!espnowCryptoX25519Keygen(ephPub, ephSec)) {
    sessionClear(s);
    return false;
  }
  espnowCryptoRandomBytes(nonceA, sizeof(nonceA));

  // Stash ephSec in aeadKeyRx and nonceA in aeadKeyTx until CONFIRM arrives.
  // These fields are written-but-not-yet-used at this stage; they get
  // overwritten by sessionDeriveAeadKeys in v4hSessionConfirm.
  memcpy(s->aeadKeyRx, ephSec, 32);
  memcpy(s->aeadKeyTx, nonceA, 16);
  // Zero the upper half of aeadKeyTx so we don't accidentally treat
  // uninitialised stack data as the stashed nonce on lookup.
  memset(s->aeadKeyTx + 16, 0, 16);
  sodium_memzero(ephSec, sizeof(ephSec));

  // Build OPEN payload + signature.
  V4PayloadSessionOpen open = {};
  open.sessionId = sessionId;
  memcpy(open.initiatorMac, selfMac, 6);
  memcpy(open.responderMac, peerMac, 6);
  memcpy(open.ephX25519Pub, ephPub, 32);
  memcpy(open.nonceA, nonceA, 16);
  open.sessionCaps = espnowLocalSessionCaps();

  uint8_t transcript[kOpenTranscriptLen];
  buildOpenTranscript(transcript, sessionId, selfMac, peerMac, ephPub, nonceA, open.sessionCaps);
  if (!espnowCryptoEd25519Sign(open.signature, transcript, sizeof(transcript), self.sec)) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: signing failed");
    sessionClear(s);
    return false;
  }

  bool ok = v4_send_frame(peerMac, ESPNOW_V4_TYPE_SESSION_OPEN,
                          ESPNOW_V4_FLAG_ACK_REQ,
                          generateMessageId(),
                          reinterpret_cast<const uint8_t*>(&open), sizeof(open), 1);
  if (ok) {
    INFO_ESPNOWF("SESSION_OPEN sent to " MAC_FMT " sessionId=%u dir=%c "
                 "(awaiting CONFIRM)",
                 MAC_ARGS(peerMac),
                 (unsigned)sessionId, s->myDirection == 0 ? 'A' : 'B');
  } else {
    ERROR_ESPNOWF("SESSION_OPEN send failed");
    sessionClear(s);
  }
  return ok;
}

// ============================================================================
// Phase 3.6 — SESSION_REKEY handler + initiator
// ============================================================================

namespace {

struct DeferredRekeyWork {
  V4PayloadSessionRekey msg;
  uint8_t               srcMac[6];   // recv_info->src_addr
};

// Compute the new AEAD keys from a freshly-derived shared secret. Mirrors
// sessionDeriveAeadKeys but writes into caller-provided buffers instead of
// SessionState — we apply the swap atomically via sessionApplyRekeyedKeys.
bool deriveRekeyedKeysFromShared(SessionState* s,
                                 const uint8_t shared[32],
                                 uint8_t outTx[32],
                                 uint8_t outRx[32]) {
  // Same KDF as initial session derivation: Blake2b with contexts
  // "esp-AtoB" / "esp-BtoA" keyed off the X25519 shared. A-side TX = AtoB,
  // B-side TX = BtoA (and vice versa for RX).
  uint8_t kAtoB[32], kBtoA[32];
  if (!espnowCryptoKdfSubkey(kAtoB, shared, 1, "esp-AtoB")) return false;
  if (!espnowCryptoKdfSubkey(kBtoA, shared, 2, "esp-BtoA")) return false;
  if (s->myDirection == 0) {
    memcpy(outTx, kAtoB, 32);
    memcpy(outRx, kBtoA, 32);
  } else {
    memcpy(outTx, kBtoA, 32);
    memcpy(outRx, kAtoB, 32);
  }
  sodium_memzero(kAtoB, sizeof(kAtoB));
  sodium_memzero(kBtoA, sizeof(kBtoA));
  return true;
}

// Initiator-side REKEY retry table (the REKEY counterpart of the KEY_EX
// in-flight table). A REKEY is one plaintext signed frame with no transport
// retry, and a REKEYING session refuses every encrypted send, so an
// unanswered REKEY is handled here: rekeyRetrySweep re-sends the identical
// signed frame with its original msgId every kRekeyRetryMs, up to
// kRekeyMaxRetries times. A peer that already holds the first copy drops a
// retransmit in its (origin,msgId) dedup ring (5 s TTL); the parked eph priv
// always matches the eph pub the peer answers, and prevTxSeqAtRekey stays
// current because nothing is wrapped while REKEYING. Send at 0 s, re-send at
// 2 s and 4 s, give up at 6 s — the same budget the SESSION_OPEN handshake
// gets (kSessionEstablishTimeoutMs). On give-up the session is cleared, so the
// next send or received peer heartbeat re-handshakes; it is not reverted to
// the old keys, because a peer that applied the REKEY and lost only its reply
// already holds the new ones.
constexpr uint8_t  kRekeyInFlightSlots = 16;    // = session slots: one outstanding REKEY per session
constexpr uint8_t  kRekeyMaxRetries    = 2;     // re-sends after the initial REKEY
constexpr uint32_t kRekeyRetryMs       = 2000;  // re-send if still REKEYING this long

struct RekeyInFlight {
  bool                  inUse;
  uint8_t               retriesLeft;
  uint16_t              sessionId;
  uint8_t               peerMac[6];
  uint32_t              msgId;        // reused on every retransmit (the peer's dedup key)
  uint32_t              lastSentMs;
  V4PayloadSessionRekey frame;        // the signed REKEY exactly as first sent
};

// 16 x 152 B in PSRAM, allocated on the first initiator REKEY. Arming
// (cmd_exec via espnowrekey, or espnow_task via the threshold trigger) and the
// sweep (espnow_task) serialize on EspNowTxGuard, like the pending-frame ring.
// An entry is released by the sweep as soon as its session is no longer
// REKEYING (answered, aborted, cleared or replaced).
RekeyInFlight* gRekeyInFlight = nullptr;

// Caller holds EspNowTxGuard.
bool ensureRekeyInFlightAllocated() {
  if (gRekeyInFlight) return true;
  gRekeyInFlight = (RekeyInFlight*)ps_alloc(sizeof(RekeyInFlight) * kRekeyInFlightSlots,
                                            AllocPref::PreferPsram, "espnow.rekey.inflight");
  if (!gRekeyInFlight) {
    ERROR_ESPNOWF("REKEY: failed to allocate %u-slot in-flight table — no retry coverage",
                  (unsigned)kRekeyInFlightSlots);
    return false;
  }
  memset(gRekeyInFlight, 0, sizeof(RekeyInFlight) * kRekeyInFlightSlots);
  return true;
}

void rekeyInFlightArm(const uint8_t peerMac[6], uint16_t sessionId, uint32_t msgId,
                      const V4PayloadSessionRekey* frame) {
  EspNowTxGuard g("rekeyArm");
  if (!ensureRekeyInFlightAllocated()) return;
  RekeyInFlight* e = nullptr;
  for (uint8_t i = 0; i < kRekeyInFlightSlots; i++) {
    RekeyInFlight& k = gRekeyInFlight[i];
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) { e = &k; break; }  // one REKEY per peer: newest wins
  }
  if (!e) {
    for (uint8_t i = 0; i < kRekeyInFlightSlots; i++) {
      if (!gRekeyInFlight[i].inUse) { e = &gRekeyInFlight[i]; break; }
    }
  }
  if (!e) {
    WARN_ESPNOWF("REKEY: in-flight table full — no retry coverage for " MAC_FMT,
                 MAC_ARGS(peerMac));
    return;
  }
  memcpy(e->peerMac, peerMac, 6);
  e->sessionId   = sessionId;
  e->msgId       = msgId;
  e->frame       = *frame;
  e->retriesLeft = kRekeyMaxRetries;
  e->lastSentMs  = (uint32_t)millis();
  e->inUse       = true;   // publish last
}

// Build + send a REKEY payload to peerMac. Generates a fresh ephemeral
// X25519 keypair and parks the priv key in SessionState for the eventual
// ECDH. Used both for initiator-side first REKEY and responder-side reply.
// Returns true on send success; refuses (false, nothing sent) unless the
// session is ACTIVE. `armRetry` is true only on the initiator
// path: the frame is then re-sent by rekeyRetrySweep until the peer answers.
// The responder's reply is never retransmitted (the initiator's retry covers
// a lost reply). If the radio refuses the frame, a session this call moved to
// REKEYING goes back to ACTIVE on its current keys, with the re-init guard
// (kRekeyMinIntervalMs) still counting from this attempt.
bool sendRekey(SessionState* s, const uint8_t peerMac[6], bool armRetry) {
  if (!s) return false;
  const auto& self = espnowIdentityGet();
  if (!self.valid) return false;

  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);

  // Fresh ephemeral keypair.
  uint8_t newEphPub[32], newEphSec[32];
  if (!espnowCryptoX25519Keygen(newEphPub, newEphSec)) {
    ERROR_ESPNOWF("REKEY: X25519 keygen failed");
    return false;
  }

  uint8_t nonceRekey[16];
  espnowCryptoRandomBytes(nonceRekey, sizeof(nonceRekey));

  // Snapshot txSeq before we send — the signature commits to it for transcript
  // uniqueness. The REKEY frame itself does NOT bump it: it ships via
  // v4_send_frame as a plaintext frame signed under our identity key, and
  // txSeqNext is only ever incremented by sessionWrapFrame on the
  // session-wrapped data path.
  uint32_t txSeqAtSign = s->txSeqNext;

  V4PayloadSessionRekey rk = {};
  rk.sessionId = s->sessionId;
  memcpy(rk.senderMac, selfMac, 6);
  memcpy(rk.receiverMac, peerMac, 6);
  memcpy(rk.newEphX25519Pub, newEphPub, 32);
  memcpy(rk.nonceRekey, nonceRekey, 16);
  rk.prevTxSeqAtRekey = txSeqAtSign;

  uint8_t transcript[75];
  buildRekeyTranscript(transcript, s->sessionId, selfMac, peerMac,
                       newEphPub, nonceRekey, txSeqAtSign);
  if (!espnowCryptoEd25519Sign(rk.signature, transcript, sizeof(transcript), self.sec)) {
    ERROR_ESPNOWF("REKEY: signing failed");
    sodium_memzero(newEphSec, sizeof(newEphSec));
    return false;
  }

  // Park our eph priv in the session (ACTIVE -> REKEYING) for the ECDH: the
  // initiator completes it when the peer's reply arrives, the responder right
  // after this send (runDeferredRekey). Only an ACTIVE session starts an
  // exchange — both callers check that first — so a refusal here means
  // another exchange owns the parked key, and it is never overwritten.
  if (!sessionMarkRekeyInitiated(s, newEphSec, txSeqAtSign)) {
    ERROR_ESPNOWF("REKEY: session not ACTIVE (state=%u) - not sending", (unsigned)s->state);
    sodium_memzero(newEphSec, sizeof(newEphSec));
    return false;
  }
  sodium_memzero(newEphSec, sizeof(newEphSec));

  const uint32_t msgId = generateMessageId();
  bool ok = v4_send_frame(peerMac, ESPNOW_V4_TYPE_SESSION_REKEY,
                          ESPNOW_V4_FLAG_ACK_REQ,
                          msgId,
                          reinterpret_cast<const uint8_t*>(&rk), sizeof(rk), 1);
  if (ok) {
    INFO_ESPNOWF("REKEY sent to " MAC_FMT " sessionId=%u "
                 "(txSeqAtSign=%lu)",
                 MAC_ARGS(peerMac),
                 (unsigned)s->sessionId, (unsigned long)txSeqAtSign);
    if (armRetry) rekeyInFlightArm(peerMac, s->sessionId, msgId, &rk);
  } else {
    ERROR_ESPNOWF("REKEY send failed");
    // Nothing reached the peer: undo this call's ACTIVE -> REKEYING flip
    // (current keys untouched) instead of leaving the session refusing
    // sends. The attempt time stays in rekeyInitiatedAtMs so the threshold
    // trigger waits kRekeyMinIntervalMs before signing and sending again.
    const uint32_t attemptMs = s->rekeyInitiatedAtMs;
    sessionAbortRekey(s);
    s->rekeyInitiatedAtMs = attemptMs;
  }
  return ok;
}

void runDeferredRekey(void* arg) {
  auto* w = static_cast<DeferredRekeyWork*>(arg);
  if (!w) return;
  const V4PayloadSessionRekey* msg = &w->msg;

  // Locate the session being rekeyed.
  SessionState* s = sessionFindBySessionId(msg->sessionId, w->srcMac);
  if (!s || (s->state != SESSION_ACTIVE && s->state != SESSION_REKEYING)) {
    WARN_ESPNOWF("REKEY rx: no ACTIVE session for sessionId=%u from " MAC_FMT " — dropping",
                 (unsigned)msg->sessionId,
                 MAC_ARGS(w->srcMac));
    free(w);
    return;
  }

  // Verify the peer's Ed25519 signature over the REKEY transcript.
  const PeerIdentity* peer = peerIdentityFindByMac(w->srcMac);
  if (!peer) {
    WARN_ESPNOWF("REKEY rx: no peer identity for sender — dropping");
    free(w);
    return;
  }
  uint8_t selfMac[6]; espnowSelfStaMac(selfMac);
  uint8_t transcript[75];
  buildRekeyTranscript(transcript, msg->sessionId,
                       msg->senderMac, msg->receiverMac,
                       msg->newEphX25519Pub, msg->nonceRekey, msg->prevTxSeqAtRekey);
  if (!espnowCryptoEd25519Verify(msg->signature, transcript, sizeof(transcript),
                                 peer->longTermPub)) {
    WARN_ESPNOWF("REKEY rx: signature verify FAILED for sessionId=%u — dropping",
                 (unsigned)msg->sessionId);
    free(w);
    return;
  }

  // F16 — reject a malformed (all-zero) peer ephemeral pubkey BEFORE we change
  // any session state or send our REKEY reply. crypto_scalarmult rejects
  // low-order points on recent libsodium, but the all-zero point is the cheap
  // explicit guard. Validating here (pre-state-change) also closes the F3
  // desync window: a forged/garbage REKEY can no longer push us into REKEYING
  // and then strand us when the ECDH on a bad point fails.
  if (sodium_is_zero(msg->newEphX25519Pub, 32)) {
    WARN_ESPNOWF("REKEY rx: peer ephemeral pubkey is all-zero — rejecting (sessionId=%u)",
                 (unsigned)msg->sessionId);
    free(w);
    return;
  }

  // Freshness. The signed REKEY stays valid for the life of the session and
  // the dispatcher's dedup ring forgets it after 5 s, so a re-delivered copy
  // is recognised against the session's live state instead (nonce of the last
  // REKEY acted on, applied-at minimum interval, sequence window). Answering
  // one would rekey to keys the peer does not hold and, with the peer
  // answering back, never end.
  const uint32_t nowMs = (uint32_t)millis();
  const SessionPeerRekeyVerdict verdict =
      sessionClassifyPeerRekey(s, msg->nonceRekey, msg->prevTxSeqAtRekey, nowMs);
  if (verdict != SESSION_REKEY_COUNTERPART && verdict != SESSION_REKEY_INITIATION) {
    static uint32_t sRekeyIgnoredWarnMs = 0;  // one line per 5 s however many copies arrive
    if (logCooldownOk(sRekeyIgnoredWarnMs, 5000)) {
      WARN_ESPNOWF("REKEY rx: ignored (%s) from " MAC_FMT " sessionId=%u "
                   "prevTxSeq=%lu rxHwm=%lu keyedAgeMs=%lu - keys unchanged, no reply",
                   verdict == SESSION_REKEY_NONCE_SEEN ? "nonce already seen" :
                   verdict == SESSION_REKEY_TOO_SOON   ? "session keyed <30s ago" : "stale sequence",
                   MAC_ARGS(w->srcMac),
                   (unsigned)s->sessionId, (unsigned long)msg->prevTxSeqAtRekey,
                   (unsigned long)s->rxSeqHighWater, (unsigned long)(nowMs - s->establishedAtMs));
    }
    free(w);
    return;
  }

  // COUNTERPART: the peer answers a REKEY we sent — our initiation (REKEYING)
  // or, inside the reply grace, the reply we just sent — so the parked priv
  // completes the ECDH and nothing is sent. INITIATION: generate and send our
  // reply now (the responder path); sendRekey parks its fresh eph priv in
  // s->rekeyEphPrivKey.
  const bool counterpart = (verdict == SESSION_REKEY_COUNTERPART);
  const bool weInitiated = (s->state == SESSION_REKEYING);
  if (!counterpart) {
    if (!sendRekey(s, w->srcMac, /*armRetry=*/false)) {
      ERROR_ESPNOWF("REKEY rx: responder failed to send our REKEY reply");
      free(w);
      return;
    }
  }

  // Now we have both ends' fresh eph pubkeys. Derive shared secret + new
  // AEAD keys, swap atomically.
  uint8_t shared[32];
  if (!espnowCryptoX25519Shared(shared, s->rekeyEphPrivKey, msg->newEphX25519Pub)) {
    // F3 — we may have already sent our REKEY reply (responder) or initiated
    // (initiator), so we're in REKEYING. Abort back to ACTIVE rather than
    // stranding the session; current keys are untouched and stay valid.
    // (Inside the reply grace the session is already ACTIVE: a no-op.)
    ERROR_ESPNOWF("REKEY rx: X25519 shared computation failed — aborting rekey");
    sessionAbortRekey(s);
    free(w);
    return;
  }
  uint8_t newKeyTx[32], newKeyRx[32];
  bool kdfOk = deriveRekeyedKeysFromShared(s, shared, newKeyTx, newKeyRx);
  sodium_memzero(shared, sizeof(shared));
  if (!kdfOk) {
    ERROR_ESPNOWF("REKEY rx: KDF failed — aborting rekey");
    sessionAbortRekey(s);
    free(w);
    return;
  }

  // Responder: keep our reply's priv for the reply grace, so that if the peer
  // takes the reply for an initiation (its own REKEY was a re-delivered copy)
  // its answer is applied as the counterpart and the exchange ends there.
  uint8_t replyEphPriv[32];
  memcpy(replyEphPriv, s->rekeyEphPrivKey, sizeof(replyEphPriv));
  sessionNotePeerRekey(s, msg->nonceRekey);
  sessionApplyRekeyedKeys(s, newKeyTx, newKeyRx);
  if (!counterpart) sessionHoldRekeyReply(s, replyEphPriv);
  sodium_memzero(replyEphPriv, sizeof(replyEphPriv));
  sodium_memzero(newKeyTx, sizeof(newKeyTx));
  sodium_memzero(newKeyRx, sizeof(newKeyRx));

  INFO_ESPNOWF("SESSION rekeyed with " MAC_FMT " sessionId=%u "
               "(role=%s) — new keys active, prev keys valid %ums",
               MAC_ARGS(w->srcMac),
               (unsigned)s->sessionId,
               weInitiated ? "initiator" : counterpart ? "responder, peer answered our reply" : "responder",
               (unsigned)kRekeyPrevKeysWindowMs);
  free(w);
}

}  // namespace

void v4hSessionRekey(const V4RxCtx& ctx) {
  // The receive table admits exactly sizeof(V4PayloadSessionRekey) (V4_OPC_LEN_EXACT).
  // PSRAM-copy + defer to cmd_exec (verify+ECDH+KDF+optional sign+send
  // is the same crypto profile as SESSION_OPEN/CONFIRM, so the same defer
  // architecture applies — keeps it off espnow_task's 6656 B stack).
  auto* w = (DeferredRekeyWork*)ps_alloc(sizeof(DeferredRekeyWork),
                                         AllocPref::PreferPsram, "espnow.rekey.defer");
  if (!w) {
    ERROR_ESPNOWF("REKEY: PSRAM alloc failed");
    return;
  }
  ctx.read(w->msg);
  memcpy(w->srcMac, ctx.recv_info->src_addr, 6);
  if (!submitDeferredToCmdExec(runDeferredRekey, w)) {
    ERROR_ESPNOWF("REKEY: cmd_exec queue full — dropping");
    free(w);
  }
}

bool espnowRekeyInitiate(const uint8_t peerMac[6]) {
  if (!gEspNow || !gEspNow->initialized) return false;
  const PeerIdentity* peer = peerIdentityFindByMac(peerMac);
  if (!peer) {
    ERROR_ESPNOWF("REKEY initiate: no peer identity");
    return false;
  }
  SessionState* s = sessionFindByPeer(peerMac, peer->meshId);
  if (!s || s->state != SESSION_ACTIVE) {
    ERROR_ESPNOWF("REKEY initiate: no ACTIVE session");
    return false;
  }
  uint32_t nowMs = (uint32_t)millis();
  // Applied-at guard: the peer ignores an initiation that arrives less than
  // kRekeyMinIntervalMs after the keys last changed (sessionClassifyPeerRekey),
  // so never send one. This also keeps a responder's parked reply key (the
  // reply grace) from being overwritten by a local initiation.
  if ((uint32_t)(nowMs - s->establishedAtMs) < kRekeyMinIntervalMs) {
    DEBUG_ESPNOWF("REKEY initiate: skipping - session keyed %ums ago (min interval %ums)",
           (unsigned)(nowMs - s->establishedAtMs), (unsigned)kRekeyMinIntervalMs);
    return false;
  }
  // Re-init guard: if we recently initiated, skip to let the in-flight one
  // converge before re-firing on the same trigger.
  if (s->rekeyInitiatedAtMs != 0 &&
      (nowMs - s->rekeyInitiatedAtMs) < kRekeyMinIntervalMs) {
    DEBUG_ESPNOWF("REKEY initiate: skipping — last attempt was %ums ago",
           (unsigned)(nowMs - s->rekeyInitiatedAtMs));
    return false;
  }
  return sendRekey(s, peerMac, /*armRetry=*/true);
}

void rekeyRetrySweep(uint32_t nowMs) {
  if (!gRekeyInFlight) return;  // no initiator REKEY sent since boot
  for (uint8_t i = 0; i < kRekeyInFlightSlots; i++) {
    RekeyInFlight resend;
    {
      EspNowTxGuard g("rekeySweep");
      RekeyInFlight& r = gRekeyInFlight[i];
      if (!r.inUse) continue;
      SessionState* s = sessionFindBySessionId(r.sessionId, r.peerMac);
      if (!s || s->state != SESSION_REKEYING) {
        // Answered (ACTIVE on the new keys), aborted, or torn down / replaced
        // by a fresh handshake — nothing left to re-send.
        memset(&r, 0, sizeof(r));
        continue;
      }
      if ((uint32_t)(nowMs - r.lastSentMs) < kRekeyRetryMs) continue;
      if (r.retriesLeft == 0) {
        WARN_ESPNOWF("REKEY: no reply from " MAC_FMT " (sessionId=%u) after %u attempts — "
                     "dropping the session; the next send re-handshakes",
                     MAC_ARGS(r.peerMac),
                     (unsigned)r.sessionId, (unsigned)(kRekeyMaxRetries + 1));
        sessionClear(s);
        memset(&r, 0, sizeof(r));
        continue;
      }
      r.retriesLeft--;
      r.lastSentMs = nowMs;
      resend = r;
    }
    // Sent from the stack copy after the lock is released. Same bytes, same
    // msgId: a peer that already holds the first copy dedups it. A refused
    // retransmit just waits for the next interval.
    INFO_ESPNOWF("REKEY: no reply from " MAC_FMT " yet — re-sending (%u left)",
                 MAC_ARGS(resend.peerMac),
                 (unsigned)resend.retriesLeft);
    v4_send_frame(resend.peerMac, ESPNOW_V4_TYPE_SESSION_REKEY,
                  ESPNOW_V4_FLAG_ACK_REQ, resend.msgId,
                  reinterpret_cast<const uint8_t*>(&resend.frame), sizeof(resend.frame), 1);
  }
}

#endif  // ENABLE_ESPNOW
