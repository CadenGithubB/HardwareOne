#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Handlers_Crypto.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

#include <esp_now.h>
#include <esp_wifi.h>

#include "System_Debug.h"
#include "System_ESPNow.h"          // EspNowState (gEspNow), V4RxCtx is in .cpp — see note
#include "System_ESPNow_Crypto.h"
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_MeshKeys.h"
#include "System_ESPNow_Sessions.h"
#include "System_ESPNow_Wire.h"
#include "System_CommandTypes.h"   // ExecReq::DeferredFn
#include "System_MemUtil.h"        // ps_alloc
#include "System_Settings.h"

#include <sodium.h>  // sodium_memzero

// submitDeferredToCmdExec — implemented in System_Utils.cpp. Pushes a
// callback onto cmd_exec_task's queue so heavy crypto runs there instead
// of on espnow_task's tighter stack. Declared inline (no header) matching
// the existing convention used by submitCommandAsync.
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);
// Defined in System_ESPNow.cpp. Notifies bond logic the moment a session with
// `peerMac` reaches ACTIVE so the bonded relationship can use the encrypted
// session for discovery/sync (replaces plaintext-heartbeat discovery). No-op
// when bond mode is off or the peer isn't our bonded peer.
extern void bondNotifySessionEstablished(const uint8_t* peerMac);
// NOTE: the bond auth token is derived inside sessionDeriveAeadKeys (for every
// session, from the same X25519 shared secret) and stored in SessionState. The
// handlers below no longer derive it explicitly — it's already in place by the
// time sessionDeriveAeadKeys returns.

// V4RxCtx is defined in System_ESPNow.cpp as a private struct. To keep handlers
// in a separate translation unit, we duplicate its declaration here — must
// stay byte-for-byte in sync with the original. If you change one, change both.
struct V4RxCtx {
  const esp_now_recv_info* recv_info;
  const EspNowV4Header*    h;
  const uint8_t*           payload;
  uint16_t                 payloadLen;
  bool                     isPaired;
  const char*              deviceName;
};

// v4_send_frame is a non-static, file-scope function in System_ESPNow.cpp.
// Phase 3.5 task #32: flags widened uint8_t -> uint16_t to carry the
// high-byte flag bits (BROADCAST_AUTH/SESSION_FRAME/HANDSHAKE).
extern bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags,
                          uint32_t msgId, const uint8_t* payload,
                          uint16_t payloadLen, uint8_t ttl);
extern uint32_t generateMessageId();

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
  info.channel = gEspNow ? gEspNow->channel : 0;
  info.ifidx   = WIFI_IF_STA;
  info.encrypt = false;
  esp_err_t rc = esp_now_add_peer(&info);
  if (rc != ESP_OK) {
    WARN_ESPNOWF("KEY_EX: esp_now_add_peer failed for %02X:%02X:%02X:%02X:%02X:%02X (rc=%d)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)rc);
    return false;
  }
  return true;
}

void logMac(const char* tag, const uint8_t mac[6]) {
  INFO_ESPNOWF("%s %02X:%02X:%02X:%02X:%02X:%02X",
               tag, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

}  // namespace

// KEY_EX retry/timeout (F6) — clear the in-flight retry record for a peer once
// the handshake resolves. Forward-declared here so the REPLY/CONFIRM handlers
// (above the table definition) can call it; defined alongside the table below.
static void keyExClearInFlight(const uint8_t peerMac[6]);

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
  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  memcpy(conf.confirmerMac, selfMac, 6);
  conf.status = status;
  const auto& self = espnowIdentityGet();
  if (self.valid) memcpy(conf.pubFingerprint, self.pub, 8);
  v4_send_frame(peerMac, ESPNOW_V4_TYPE_KEY_EX_CONFIRM,
                ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&conf), sizeof(conf), 1);
}

// ============================================================================
// Receive handlers
// ============================================================================

void v4hKeyExHello(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadKeyExHello)) {
    WARN_ESPNOWF("KEY_EX_HELLO: bad payload size %u (want %u)",
                 (unsigned)ctx.payloadLen, (unsigned)sizeof(V4PayloadKeyExHello));
    return;
  }
  const auto* msg = reinterpret_cast<const V4PayloadKeyExHello*>(ctx.payload);

  const uint8_t* bootKey = nullptr;
  int meshSlot = verifyKeyExPayload(ctx, msg->meshFingerprint,
                                    msg->senderMac, msg->senderPubEd25519,
                                    msg->hmac, &bootKey);
  if (meshSlot < 0) {
    WARN_ESPNOWF("KEY_EX_HELLO rejected (reason=%d) from %02X:%02X:%02X:%02X:%02X:%02X meshFp=0x%04X",
                 meshSlot,
                 msg->senderMac[0], msg->senderMac[1], msg->senderMac[2],
                 msg->senderMac[3], msg->senderMac[4], msg->senderMac[5],
                 (unsigned)msg->meshFingerprint);
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
    extern bool espnowConsumePairingWindow(const uint8_t mac[6]);
    if (espnowConsumePairingWindow(msg->senderMac)) {
      WARN_ESPNOWF("KEY_EX_HELLO: peer %02X:%02X:%02X:%02X:%02X:%02X presented new pubkey; "
                   "re-pair window open — replacing stored identity",
                   msg->senderMac[0], msg->senderMac[1], msg->senderMac[2],
                   msg->senderMac[3], msg->senderMac[4], msg->senderMac[5]);
      conflict = false;  // operator-authorized re-key
    }
  }
  if (conflict) {
    WARN_ESPNOWF("KEY_EX_HELLO: peer %02X:%02X:%02X:%02X:%02X:%02X presented new pubkey, "
                 "refusing overwrite — run 'espnowforget' or re-run 'espnowpairsecure'",
                 msg->senderMac[0], msg->senderMac[1], msg->senderMac[2],
                 msg->senderMac[3], msg->senderMac[4], msg->senderMac[5]);
    confirmStatus = 2;
  } else {
    time_t now = time(nullptr);
    uint32_t bondedAt = (uint32_t)((now > 0) ? now : 0);
    if (!peerIdentityPersist(msg->senderMac, (uint8_t)meshSlot,
                             msg->senderPubEd25519, bondedAt)) {
      ERROR_ESPNOWF("KEY_EX_HELLO: identity persist failed; not replying");
      return;
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
  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  memcpy(reply.responderMac, selfMac, 6);
  memcpy(reply.responderPubEd25519, self.pub, 32);
  if (!computeKeyExHmac(msg->meshFingerprint, selfMac, self.pub, bootKey, reply.hmac)) {
    ERROR_ESPNOWF("KEY_EX_HELLO: HMAC compute failed for REPLY");
    return;
  }

  v4_send_frame(msg->senderMac, ESPNOW_V4_TYPE_KEY_EX_REPLY,
                ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&reply), sizeof(reply), 1);
}

void v4hKeyExReply(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadKeyExReply)) {
    WARN_ESPNOWF("KEY_EX_REPLY: bad payload size %u", (unsigned)ctx.payloadLen);
    return;
  }
  const auto* msg = reinterpret_cast<const V4PayloadKeyExReply*>(ctx.payload);

  const uint8_t* bootKey = nullptr;
  int meshSlot = verifyKeyExPayload(ctx, msg->meshFingerprint,
                                    msg->responderMac, msg->responderPubEd25519,
                                    msg->hmac, &bootKey);
  if (meshSlot < 0) {
    WARN_ESPNOWF("KEY_EX_REPLY rejected (reason=%d) from %02X:%02X:%02X:%02X:%02X:%02X",
                 meshSlot,
                 msg->responderMac[0], msg->responderMac[1], msg->responderMac[2],
                 msg->responderMac[3], msg->responderMac[4], msg->responderMac[5]);
    return;
  }

  const PeerIdentity* existing = peerIdentityFindByMac(msg->responderMac);
  bool conflict = existing && memcmp(existing->longTermPub, msg->responderPubEd25519, 32) != 0;
  if (conflict) {
    // Operator-initiated re-pair window authorizes a one-shot key replacement
    // for this MAC; otherwise refuse (symmetric with the HELLO-side guard).
    extern bool espnowConsumePairingWindow(const uint8_t mac[6]);
    if (espnowConsumePairingWindow(msg->responderMac)) {
      WARN_ESPNOWF("KEY_EX_REPLY: peer presented new pubkey; re-pair window open — "
                   "replacing stored identity");
      conflict = false;
    }
  }
  if (conflict) {
    WARN_ESPNOWF("KEY_EX_REPLY: peer presented new pubkey, refusing overwrite — "
                 "signaling reject (status=2). Run 'espnowforget' or re-run 'espnowpairsecure'.");
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
  if (!peerIdentityPersist(msg->responderMac, (uint8_t)meshSlot,
                           msg->responderPubEd25519, bondedAt)) {
    ERROR_ESPNOWF("KEY_EX_REPLY: identity persist failed");
    return;
  }
  logMac("KEY_EX_REPLY accepted from", msg->responderMac);

  // Handshake resolved for us (initiator) — stop the HELLO retry timer (F6).
  keyExClearInFlight(msg->responderMac);

  if (!ensureUnencryptedPeer(msg->responderMac)) return;

  // Send CONFIRM (status=0) — KEY_EX complete from our (initiator) side.
  sendKeyExConfirm(msg->responderMac, msg->meshFingerprint, 0);

  // Encrypt-or-wait foundation (2026-05): if v4_send_encrypted_or_queue parked
  // any frames for this peer while we were handshaking (because there was no
  // peer identity at send time), kick SESSION_OPEN now. The pending frames
  // will drain via the existing pendingFrameDrainForPeer hook in the
  // SESSION_CONFIRM handler. No-op when there's nothing pending.
  if (pendingFrameHasForPeer(msg->responderMac)) {
    if (!espnowSessionOpenInitiate(msg->responderMac, nullptr)) {
      WARN_ESPNOWF("KEY_EX_REPLY: pending frames exist but SESSION_OPEN kick failed "
                   "— frames will time out in pending-frame sweep");
    } else {
      INFO_ESPNOWF("KEY_EX_REPLY: pending frames for peer — kicked SESSION_OPEN to drain");
    }
  }
}

void v4hKeyExConfirm(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadKeyExConfirm)) {
    WARN_ESPNOWF("KEY_EX_CONFIRM: bad payload size %u", (unsigned)ctx.payloadLen);
    return;
  }
  const auto* msg = reinterpret_cast<const V4PayloadKeyExConfirm*>(ctx.payload);

  // No HMAC on CONFIRM — it's just an acknowledgement, the prior REPLY's
  // HMAC already authenticated this peer. We do sanity-check the mesh and
  // the sender MAC is a known peer.
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
    INFO_ESPNOWF("KEY_EX with %02X:%02X:%02X:%02X:%02X:%02X complete "
                 "(remote pub fp: %02X%02X%02X%02X%02X%02X%02X%02X)",
                 msg->confirmerMac[0], msg->confirmerMac[1], msg->confirmerMac[2],
                 msg->confirmerMac[3], msg->confirmerMac[4], msg->confirmerMac[5],
                 msg->pubFingerprint[0], msg->pubFingerprint[1], msg->pubFingerprint[2],
                 msg->pubFingerprint[3], msg->pubFingerprint[4], msg->pubFingerprint[5],
                 msg->pubFingerprint[6], msg->pubFingerprint[7]);
  } else {
    WARN_ESPNOWF("KEY_EX with %02X:%02X:%02X:%02X:%02X:%02X reported status=%u "
                 "(0=ok, 1=hmac-fail, 2=pub-conflict)",
                 msg->confirmerMac[0], msg->confirmerMac[1], msg->confirmerMac[2],
                 msg->confirmerMac[3], msg->confirmerMac[4], msg->confirmerMac[5],
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
// idempotent (a re-sent HELLO just re-persists the same key and re-emits the
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
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);

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
                          ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                          generateMessageId(),
                          reinterpret_cast<const uint8_t*>(&hello), sizeof(hello), 1);
  if (ok) {
    INFO_ESPNOWF("KEY_EX_HELLO sent to %02X:%02X:%02X:%02X:%02X:%02X (mesh '%s', fp=0x%04X)",
                 peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
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

struct KeyExInFlight {
  bool     inUse;
  uint8_t  peerMac[6];
  int8_t   meshSlot;     // resolved slot, to rebuild the HELLO on retry
  uint8_t  retriesLeft;
  uint32_t lastSentMs;
};

// Tiny table (4 × ~16 B). Registered on cmd_exec (espnowKeyExInitiate via the
// CLI / pairsecure); swept + cleared on the espnow task (RX handlers + tick).
// Lock-free per the codebase convention for these small tables: register fills
// all fields before setting inUse=true, so a concurrent sweep never observes a
// half-populated in-use slot; clear is a single-bool write.
KeyExInFlight gKeyExInFlight[kKeyExInFlightSlots] = {};

void keyExInFlightArm(const uint8_t peerMac[6], int slot) {
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
  e->inUse        = true;   // publish last
}

}  // namespace

static void keyExClearInFlight(const uint8_t peerMac[6]) {
  for (auto& k : gKeyExInFlight)
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) { k.inUse = false; return; }
}

// Public accessor — see header for contract.
bool keyExIsInFlight(const uint8_t peerMac[6]) {
  if (!peerMac) return false;
  for (auto& k : gKeyExInFlight)
    if (k.inUse && memcmp(k.peerMac, peerMac, 6) == 0) return true;
  return false;
}

void keyExRetrySweep(uint32_t nowMs) {
  for (auto& k : gKeyExInFlight) {
    if (!k.inUse) continue;
    if ((uint32_t)(nowMs - k.lastSentMs) < kKeyExRetryMs) continue;
    if (k.retriesLeft == 0) {
      WARN_ESPNOWF("KEY_EX: no REPLY from %02X:%02X:%02X:%02X:%02X:%02X after %u attempts — "
                   "giving up (run espnowkeyex to retry)",
                   k.peerMac[0], k.peerMac[1], k.peerMac[2], k.peerMac[3], k.peerMac[4], k.peerMac[5],
                   (unsigned)(kKeyExMaxRetries + 1));
      k.inUse = false;
      continue;
    }
    k.retriesLeft--;
    INFO_ESPNOWF("KEY_EX: no REPLY from %02X:%02X:%02X:%02X:%02X:%02X yet — retrying HELLO (%u left)",
                 k.peerMac[0], k.peerMac[1], k.peerMac[2], k.peerMac[3], k.peerMac[4], k.peerMac[5],
                 (unsigned)k.retriesLeft);
    keyExSendHello(k.peerMac, k.meshSlot);   // failure just retries next tick / eventually gives up
    k.lastSentMs = nowMs;
  }
}

bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel) {
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
  if (ok) keyExInFlightArm(peerMac, slot);  // arm retry/timeout
  return ok;
}

// ============================================================================
// Phase 3.4 — SESSION handshake
// ============================================================================

namespace {

constexpr const char* kOpenLabel    = "v4-sopen:";   // 9 bytes including ':'
constexpr const char* kConfirmLabel = "v4-sconf:";   // 9 bytes
constexpr const char* kRekeyLabel   = "v4-rekey:";   // 9 bytes (Phase 3.6)
constexpr size_t      kLabelLen     = 9;

// Build the OPEN transcript for sign/verify:
//   "v4-sopen:" || sessionId(2 LE) || initMac(6) || respMac(6) || eph(32) || nonceA(16)
// Total 9 + 2 + 6 + 6 + 32 + 16 = 71 bytes.
void buildOpenTranscript(uint8_t out[71],
                         uint16_t sessionId,
                         const uint8_t initMac[6],
                         const uint8_t respMac[6],
                         const uint8_t eph[32],
                         const uint8_t nonceA[16]) {
  memcpy(out, kOpenLabel, kLabelLen);
  out[9]  = (uint8_t)(sessionId & 0xFF);
  out[10] = (uint8_t)((sessionId >> 8) & 0xFF);
  memcpy(out + 11, initMac, 6);
  memcpy(out + 17, respMac, 6);
  memcpy(out + 23, eph, 32);
  memcpy(out + 55, nonceA, 16);
}

// Build the CONFIRM transcript:
//   "v4-sconf:" || sessionId(2 LE) || respMac(6) || initMac(6) || eph(32) ||
//   nonceA(16) || nonceB(16)
// Total 9 + 2 + 6 + 6 + 32 + 16 + 16 = 87 bytes.
void buildConfirmTranscript(uint8_t out[87],
                            uint16_t sessionId,
                            const uint8_t respMac[6],
                            const uint8_t initMac[6],
                            const uint8_t eph[32],
                            const uint8_t nonceA[16],
                            const uint8_t nonceB[16]) {
  memcpy(out, kConfirmLabel, kLabelLen);
  out[9]  = (uint8_t)(sessionId & 0xFF);
  out[10] = (uint8_t)((sessionId >> 8) & 0xFF);
  memcpy(out + 11, respMac, 6);
  memcpy(out + 17, initMac, 6);
  memcpy(out + 23, eph, 32);
  memcpy(out + 55, nonceA, 16);
  memcpy(out + 71, nonceB, 16);
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
// Deferred-to-cmd_exec_task work structs + worker functions.
//
// Ed25519 sign+verify each use ~3 KB of libsodium internal stack — adding
// that to espnow_task (22 KB budget, ~9 KB idle HWM) caused overflows during
// SESSION_OPEN handling. cmd_exec_task has 24 KB and is single-threaded
// w.r.t. the existing CLI peak (17 KB), so deferring keeps everything under
// budget without bumping any task's stack.
//
// On-wire RX handlers (v4hSessionOpen/Confirm) do only size validation +
// PSRAM copy + enqueue. Heavy lifting runs in runDeferredSessionOpen/Confirm
// on cmd_exec_task. The work struct + callback own their own lifetime.
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

void runDeferredSessionOpen(void* arg) {
  auto* w = static_cast<DeferredSessionOpenWork*>(arg);
  const V4PayloadSessionOpen* msg = &w->msg;

  if (memcmp(w->headerOrigin, msg->initiatorMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_OPEN: header.origin / payload.initiatorMac mismatch");
    free(w);
    return;
  }

  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  if (memcmp(msg->responderMac, selfMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_OPEN: responderMac is not us — ignoring");
    free(w);
    return;
  }

  const PeerIdentity* peer = peerIdentityFindByMac(msg->initiatorMac);
  if (!peer) {
    WARN_ESPNOWF("SESSION_OPEN from %02X:%02X:%02X:%02X:%02X:%02X — no KEY_EX identity, rejecting",
                 msg->initiatorMac[0], msg->initiatorMac[1], msg->initiatorMac[2],
                 msg->initiatorMac[3], msg->initiatorMac[4], msg->initiatorMac[5]);
    free(w);
    return;
  }

  uint8_t transcript[71];
  buildOpenTranscript(transcript, msg->sessionId,
                      msg->initiatorMac, msg->responderMac,
                      msg->ephX25519Pub, msg->nonceA);
  if (!espnowCryptoEd25519Verify(msg->signature, transcript, sizeof(transcript),
                                 peer->longTermPub)) {
    WARN_ESPNOWF("SESSION_OPEN: Ed25519 verify failed (sessionId=%u peer=%02X:%02X:%02X:%02X:%02X:%02X)",
                 (unsigned)msg->sessionId,
                 msg->initiatorMac[0], msg->initiatorMac[1], msg->initiatorMac[2],
                 msg->initiatorMac[3], msg->initiatorMac[4], msg->initiatorMac[5]);
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
  {
    SessionState* existing = sessionFindByPeer(msg->initiatorMac, peer->meshId);
    if (existing && existing->state == SESSION_ESTABLISHING &&
        sessionIsASide(selfMac, msg->initiatorMac)) {
      WARN_ESPNOWF("SESSION_OPEN glare with %02X:%02X:%02X:%02X:%02X:%02X — we are A-side, "
                   "keeping our initiation (ignoring peer sessionId=%u)",
                   msg->initiatorMac[0], msg->initiatorMac[1], msg->initiatorMac[2],
                   msg->initiatorMac[3], msg->initiatorMac[4], msg->initiatorMac[5],
                   (unsigned)msg->sessionId);
      free(w);
      return;
    }
  }

  SessionState* s = sessionAllocate(msg->initiatorMac, peer->meshId);
  if (!s) { free(w); return; }
  s->sessionId    = msg->sessionId;
  s->myDirection  = sessionIsASide(selfMac, msg->initiatorMac) ? 0 : 1;

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

  uint8_t confTranscript[87];
  buildConfirmTranscript(confTranscript, msg->sessionId,
                         selfMac, msg->initiatorMac,
                         ephPub, msg->nonceA, nonceB);
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
  INFO_ESPNOWF("SESSION established (responder) with %02X:%02X:%02X:%02X:%02X:%02X sessionId=%u dir=%c",
               msg->initiatorMac[0], msg->initiatorMac[1], msg->initiatorMac[2],
               msg->initiatorMac[3], msg->initiatorMac[4], msg->initiatorMac[5],
               (unsigned)msg->sessionId, s->myDirection == 0 ? 'A' : 'B');

  v4_send_frame(msg->initiatorMac, ESPNOW_V4_TYPE_SESSION_CONFIRM,
                ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                generateMessageId(),
                reinterpret_cast<const uint8_t*>(&conf), sizeof(conf), 1);
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
  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  if (memcmp(msg->initiatorMac, selfMac, 6) != 0) {
    WARN_ESPNOWF("SESSION_CONFIRM: initiatorMac is not us");
    free(w);
    return;
  }

  SessionState* s = sessionFindBySessionId(msg->sessionId, msg->responderMac);
  if (!s || s->state != SESSION_ESTABLISHING) {
    WARN_ESPNOWF("SESSION_CONFIRM: no in-flight session for sessionId=%u peer=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)msg->sessionId,
                 msg->responderMac[0], msg->responderMac[1], msg->responderMac[2],
                 msg->responderMac[3], msg->responderMac[4], msg->responderMac[5]);
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

  uint8_t transcript[87];
  buildConfirmTranscript(transcript, msg->sessionId,
                         msg->responderMac, msg->initiatorMac,
                         msg->ephX25519Pub, msg->nonceA, msg->nonceB);
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

  s->state           = SESSION_ACTIVE;
  s->establishedAtMs = (uint32_t)millis();
  s->lastUseMs       = s->establishedAtMs;
  INFO_ESPNOWF("SESSION established (initiator) with %02X:%02X:%02X:%02X:%02X:%02X sessionId=%u dir=%c",
               msg->responderMac[0], msg->responderMac[1], msg->responderMac[2],
               msg->responderMac[3], msg->responderMac[4], msg->responderMac[5],
               (unsigned)msg->sessionId, s->myDirection == 0 ? 'A' : 'B');
  // Phase 3.5 step 3 — drain any frames the app queued while we were handshaking.
  // Runs on cmd_exec_task (same task that handled the heavy crypto above), so
  // the drain's session-wrap + esp_now_send fits in this task's stack.
  pendingFrameDrainForPeer(msg->responderMac);
  // Session ACTIVE on our side (initiator) — notify bond logic (discovery via
  // the encrypted session; master kicks the capability sync from here).
  bondNotifySessionEstablished(msg->responderMac);
  free(w);
}

}  // namespace

// On-RX handlers — espnow_task scope. Lightweight: size-check + PSRAM copy +
// enqueue. All heavy crypto runs in the deferred function on cmd_exec_task.
void v4hSessionOpen(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadSessionOpen)) {
    WARN_ESPNOWF("SESSION_OPEN: bad payload size %u", (unsigned)ctx.payloadLen);
    return;
  }
  auto* w = static_cast<DeferredSessionOpenWork*>(
      ps_alloc(sizeof(DeferredSessionOpenWork), AllocPref::PreferPSRAM, "espnow.sopen.defer"));
  if (!w) {
    ERROR_ESPNOWF("SESSION_OPEN: PSRAM alloc failed (defer drop)");
    return;
  }
  memcpy(&w->msg, ctx.payload, sizeof(w->msg));
  memcpy(w->headerOrigin, ctx.h->origin, 6);
  if (!submitDeferredToCmdExec(runDeferredSessionOpen, w)) {
    ERROR_ESPNOWF("SESSION_OPEN: cmd_exec queue full, dropping");
    free(w);
  }
}

void v4hSessionConfirm(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadSessionConfirm)) {
    WARN_ESPNOWF("SESSION_CONFIRM: bad payload size %u", (unsigned)ctx.payloadLen);
    return;
  }
  auto* w = static_cast<DeferredSessionConfirmWork*>(
      ps_alloc(sizeof(DeferredSessionConfirmWork), AllocPref::PreferPSRAM, "espnow.sconf.defer"));
  if (!w) {
    ERROR_ESPNOWF("SESSION_CONFIRM: PSRAM alloc failed (defer drop)");
    return;
  }
  memcpy(&w->msg, ctx.payload, sizeof(w->msg));
  memcpy(w->headerOrigin, ctx.h->origin, 6);
  if (!submitDeferredToCmdExec(runDeferredSessionConfirm, w)) {
    ERROR_ESPNOWF("SESSION_CONFIRM: cmd_exec queue full, dropping");
    free(w);
  }
}

bool espnowSessionOpenInitiate(const uint8_t peerMac[6], const char* meshLabel) {
  if (!gEspNow || !gEspNow->initialized) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: ESPNOW not initialized");
    return false;
  }
  if (!sessionsInit()) return false;

  // Peer must already have an identity record from KEY_EX (3.3).
  const PeerIdentity* peer = peerIdentityFindByMac(peerMac);
  if (!peer) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: no peer identity for %02X:%02X:%02X:%02X:%02X:%02X "
                  "— run 'espnowkeyex' first",
                  peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
    return false;
  }

  const auto& self = espnowIdentityGet();
  if (!self.valid) return false;
  if (!ensureUnencryptedPeer(peerMac)) return false;

  // Allocate the slot and generate keys/nonces.
  SessionState* s = sessionAllocate(peerMac, peer->meshId);
  if (!s) return false;

  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
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

  uint8_t transcript[71];
  buildOpenTranscript(transcript, sessionId, selfMac, peerMac, ephPub, nonceA);
  if (!espnowCryptoEd25519Sign(open.signature, transcript, sizeof(transcript), self.sec)) {
    ERROR_ESPNOWF("SESSION_OPEN initiate: signing failed");
    sessionClear(s);
    return false;
  }

  bool ok = v4_send_frame(peerMac, ESPNOW_V4_TYPE_SESSION_OPEN,
                          ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                          generateMessageId(),
                          reinterpret_cast<const uint8_t*>(&open), sizeof(open), 1);
  if (ok) {
    INFO_ESPNOWF("SESSION_OPEN sent to %02X:%02X:%02X:%02X:%02X:%02X sessionId=%u dir=%c "
                 "(awaiting CONFIRM)",
                 peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
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

// Build + send a REKEY payload to peerMac. Generates a fresh ephemeral
// X25519 keypair and parks the priv key in SessionState for the eventual
// ECDH. Used both for initiator-side first REKEY and responder-side reply.
// Returns true on send success.
bool sendRekey(SessionState* s, const uint8_t peerMac[6]) {
  if (!s) return false;
  const auto& self = espnowIdentityGet();
  if (!self.valid) return false;

  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);

  // Fresh ephemeral keypair.
  uint8_t newEphPub[32], newEphSec[32];
  if (!espnowCryptoX25519Keygen(newEphPub, newEphSec)) {
    ERROR_ESPNOWF("REKEY: X25519 keygen failed");
    return false;
  }

  uint8_t nonceRekey[16];
  espnowCryptoRandomBytes(nonceRekey, sizeof(nonceRekey));

  // Snapshot txSeq before we send (the REKEY itself bumps it via wrap, but
  // the signature commits to the pre-send count for transcript uniqueness).
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

  // Park our eph priv in the session for the eventual ECDH on REKEY reply.
  // Even if we're the *responder* here (we received a REKEY first), parking
  // is still the right thing: the peer might have a stale view of us and
  // initiate yet another REKEY before our reply arrives — having our priv
  // already in the slot makes that benign.
  if (!sessionMarkRekeyInitiated(s, newEphSec, txSeqAtSign)) {
    // Already REKEYING — keep the previously-parked priv; just use the new
    // outbound one for this round. Stash via direct memcpy.
    memcpy(s->rekeyEphPrivKey, newEphSec, 32);
    s->rekeyTxSeqAtInit = txSeqAtSign;
  }
  sodium_memzero(newEphSec, sizeof(newEphSec));

  bool ok = v4_send_frame(peerMac, ESPNOW_V4_TYPE_SESSION_REKEY,
                          ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ,
                          generateMessageId(),
                          reinterpret_cast<const uint8_t*>(&rk), sizeof(rk), 1);
  if (ok) {
    INFO_ESPNOWF("REKEY sent to %02X:%02X:%02X:%02X:%02X:%02X sessionId=%u "
                 "(txSeqAtSign=%lu)",
                 peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
                 (unsigned)s->sessionId, (unsigned long)txSeqAtSign);
  } else {
    ERROR_ESPNOWF("REKEY send failed");
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
    WARN_ESPNOWF("REKEY rx: no ACTIVE session for sessionId=%u from %02X:%02X:%02X:%02X:%02X:%02X — dropping",
                 (unsigned)msg->sessionId,
                 w->srcMac[0], w->srcMac[1], w->srcMac[2], w->srcMac[3], w->srcMac[4], w->srcMac[5]);
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
  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
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

  // If we don't already have an outstanding REKEY of our own, generate one
  // now and send it. This is the "responder" path. The sendRekey() call
  // parks our fresh eph priv in s->rekeyEphPrivKey.
  bool weHadOutstanding = (s->state == SESSION_REKEYING && s->rekeyInitiatedAtMs != 0);
  if (!weHadOutstanding) {
    if (!sendRekey(s, w->srcMac)) {
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

  sessionApplyRekeyedKeys(s, newKeyTx, newKeyRx);
  sodium_memzero(newKeyTx, sizeof(newKeyTx));
  sodium_memzero(newKeyRx, sizeof(newKeyRx));

  INFO_ESPNOWF("SESSION rekeyed with %02X:%02X:%02X:%02X:%02X:%02X sessionId=%u "
               "(role=%s) — new keys active, prev keys valid %ums",
               w->srcMac[0], w->srcMac[1], w->srcMac[2], w->srcMac[3], w->srcMac[4], w->srcMac[5],
               (unsigned)s->sessionId,
               weHadOutstanding ? "initiator" : "responder",
               (unsigned)kRekeyPrevKeysWindowMs);
  free(w);
}

}  // namespace

void v4hSessionRekey(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(V4PayloadSessionRekey)) {
    WARN_ESPNOWF("REKEY: bad payload size %u (expected %u)",
                 (unsigned)ctx.payloadLen, (unsigned)sizeof(V4PayloadSessionRekey));
    return;
  }
  // PSRAM-copy + defer to cmd_exec_task (verify+ECDH+KDF+optional sign+send
  // is the same crypto profile as SESSION_OPEN/CONFIRM, so the same defer
  // architecture applies — keeps espnow_task's stack at 22 KB).
  auto* w = (DeferredRekeyWork*)ps_alloc(sizeof(DeferredRekeyWork),
                                         AllocPref::PreferPSRAM, "espnow.rekey.defer");
  if (!w) {
    ERROR_ESPNOWF("REKEY: PSRAM alloc failed");
    return;
  }
  memcpy(&w->msg, ctx.payload, sizeof(V4PayloadSessionRekey));
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
  // Re-init guard: if we recently initiated, skip to let the in-flight one
  // converge before re-firing on the same trigger.
  uint32_t nowMs = (uint32_t)millis();
  if (s->rekeyInitiatedAtMs != 0 &&
      (nowMs - s->rekeyInitiatedAtMs) < kRekeyMinIntervalMs) {
    DEBUGF(DEBUG_ESPNOW_CORE, "REKEY initiate: skipping — last attempt was %ums ago",
           (unsigned)(nowMs - s->rekeyInitiatedAtMs));
    return false;
  }
  return sendRekey(s, peerMac);
}

#endif  // ENABLE_ESPNOW
