#ifndef SYSTEM_ESPNOW_HANDSHAKE_CORE_H
#define SYSTEM_ESPNOW_HANDSHAKE_CORE_H

// ============================================================================
// The byte strings the ESP-NOW handshake authenticates (1.0, WP2 + revocation
// C). A cross-device contract: both boards must build exactly these bytes, so
// every layout is here, in one pure header the host tests compile, and
// test_espnow_handshake_kat.py pins them against vectors computed
// independently (espnow_handshake_kat_vectors.py, Python hashlib/hmac).
//
// Labels follow the hw1-<proto>-1-<purpose> rule. The libsodium KDF contexts
// ("esp-boot", "esp-grup", "esp-mid1", "esp-AtoB", "esp-BtoA", "esp-bond")
// are exactly 8 bytes and hold no version, so they stay (owner answer E17).
//
// Every integer is little endian. Every tag is the first 16 bytes of an
// HMAC-SHA256 (rule A7), compared in constant time.
//
//   KEY_EX_HELLO   hmac = HMAC(bootstrapKey, "hw1-espnow-1-kxhello" || meshId(2) ||
//                              senderMac(6) || senderPub(32) || nonce(16))
//   KEY_EX_REPLY   hmac = HMAC(bootstrapKey, "hw1-espnow-1-kxreply" || meshId(2) ||
//                              responderMac(6) || responderPub(32) || replyNonce(16) ||
//                              initiatorMac(6) || initiatorPub(32) || helloNonce(16))
//                  A REPLY answers one HELLO: a recorded REPLY verifies only
//                  against the nonce of the HELLO it answered.
//   KEY_EX_CONFIRM hmac = HMAC(bootstrapKey, "hw1-espnow-1-kxconf" || confirm[0..18) ||
//                              initiatorMac(6) || responderMac(6) || helloNonce(16) ||
//                              replyNonce(16) || initiatorPub(32) || responderPub(32))
//                  confirm[0..18) is the frame up to its hmac (meshId, confirmerMac,
//                  status, reserved, keyId). A CONFIRM that answers a HELLO (a
//                  refusal: no REPLY was sent) has no reply nonce and no responder
//                  key the initiator knows: both are 16 / 32 zero bytes.
//   SESSION_OPEN    transcript "hw1-espnow-1-sopen" || sessionId(2) || initMac(6) ||
//                   respMac(6) || eph(32) || nonceA(16) || sessionCaps(4) || openEpochMs(8)  = 92
//   SESSION_CONFIRM transcript "hw1-espnow-1-sconf" || sessionId(2) || respMac(6) ||
//                   initMac(6) || eph(32) || nonceA(16) || nonceB(16) || sessionCaps(4)      = 100
//   SESSION_REKEY   transcript "hw1-espnow-1-rekey" || sessionId(2) || senderMac(6) ||
//                   receiverMac(6) || newEph(32) || nonceRekey(16) || prevTxSeqAtRekey(4)    = 84
//   meshTag (C1)    HMAC(bootstrapKey of the identity's mesh slot, "<label>-mk" || meshId(2) ||
//                   transcript || signature(64)), labels hw1-espnow-1-sopen-mk / -sconf-mk /
//                   -rekey-mk. Placed after the signature; checked before it.
//   session keys (C2) prk = HMAC(bootstrapKey, "hw1-espnow-1-session" || X25519 shared(32));
//                   AtoB = KDF(prk, 1, "esp-AtoB"), BtoA = KDF(prk, 2, "esp-BtoA"),
//                   bond token = KDF(prk, 3, "esp-bond")[0..16)
//   group tag       HMAC(groupKey, "hw1-espnow-1-group" || header[0..30) || payload)
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espnowhs {

constexpr char kLabelKxHello[]    = "hw1-espnow-1-kxhello";
constexpr char kLabelKxReply[]    = "hw1-espnow-1-kxreply";
constexpr char kLabelKxConf[]     = "hw1-espnow-1-kxconf";
constexpr char kLabelOpen[]       = "hw1-espnow-1-sopen";
constexpr char kLabelConfirm[]    = "hw1-espnow-1-sconf";
constexpr char kLabelRekey[]      = "hw1-espnow-1-rekey";
constexpr char kLabelOpenMk[]     = "hw1-espnow-1-sopen-mk";
constexpr char kLabelConfirmMk[]  = "hw1-espnow-1-sconf-mk";
constexpr char kLabelRekeyMk[]    = "hw1-espnow-1-rekey-mk";
constexpr char kLabelGroup[]      = "hw1-espnow-1-group";
constexpr char kLabelSessionKey[] = "hw1-espnow-1-session";

// Length of a label without its NUL.
template <size_t N>
constexpr size_t labelLen(const char (&)[N]) { return N - 1; }

constexpr size_t kTagLen   = 16;   // every handshake tag: the first 16 bytes of an HMAC-SHA256
constexpr size_t kNonceLen = 16;
constexpr size_t kConfirmHeadLen = 18;  // KEY_EX_CONFIRM bytes before its hmac

constexpr size_t kHelloInputLen   = labelLen(kLabelKxHello) + 2 + 6 + 32 + 16;
constexpr size_t kReplyInputLen   = labelLen(kLabelKxReply) + 2 + 6 + 32 + 16 + 6 + 32 + 16;
constexpr size_t kConfirmInputLen = labelLen(kLabelKxConf) + kConfirmHeadLen + 6 + 6 + 16 + 16 + 32 + 32;
constexpr size_t kOpenTranscriptLen    = labelLen(kLabelOpen) + 2 + 6 + 6 + 32 + 16 + 4 + 8;
constexpr size_t kConfirmTranscriptLen = labelLen(kLabelConfirm) + 2 + 6 + 6 + 32 + 16 + 16 + 4;
constexpr size_t kRekeyTranscriptLen   = labelLen(kLabelRekey) + 2 + 6 + 6 + 32 + 16 + 4;
constexpr size_t kMeshTagInputMax = labelLen(kLabelConfirmMk) + 2 + kConfirmTranscriptLen + 64;
constexpr size_t kSessionKeyInputLen = labelLen(kLabelSessionKey) + 32;
static_assert(kHelloInputLen == 76 && kReplyInputLen == 130 && kConfirmInputLen == 145, "KEY_EX inputs");
static_assert(kOpenTranscriptLen == 92 && kConfirmTranscriptLen == 100 && kRekeyTranscriptLen == 84,
              "session transcripts");
static_assert(kMeshTagInputMax == 187 && kSessionKeyInputLen == 52, "mesh tag and session key inputs");

// Writes fields in order, little endian.
struct Writer {
  uint8_t* p;
  size_t n;
  void bytes(const void* src, size_t len) {
    if (src) memcpy(p + n, src, len);
    else memset(p + n, 0, len);   // a value the writer does not know: zero bytes
    n += len;
  }
  void label(const char* s, size_t len) { bytes(s, len); }
  void u16(uint16_t v) { p[n++] = (uint8_t)(v & 0xFF); p[n++] = (uint8_t)(v >> 8); }
  void u32(uint32_t v) { for (int i = 0; i < 4; i++) p[n++] = (uint8_t)(v >> (8 * i)); }
  void u64(uint64_t v) { for (int i = 0; i < 8; i++) p[n++] = (uint8_t)(v >> (8 * i)); }
};

// KEY_EX_HELLO's HMAC input.
inline size_t helloInput(uint8_t out[kHelloInputLen], uint16_t meshId, const uint8_t senderMac[6],
                         const uint8_t senderPub[32], const uint8_t nonce[16]) {
  Writer w{out, 0};
  w.label(kLabelKxHello, labelLen(kLabelKxHello));
  w.u16(meshId);
  w.bytes(senderMac, 6);
  w.bytes(senderPub, 32);
  w.bytes(nonce, 16);
  return w.n;
}

// KEY_EX_REPLY's HMAC input: the responder's fields, then the HELLO it answers.
inline size_t replyInput(uint8_t out[kReplyInputLen], uint16_t meshId, const uint8_t respMac[6],
                         const uint8_t respPub[32], const uint8_t replyNonce[16], const uint8_t initMac[6],
                         const uint8_t initPub[32], const uint8_t helloNonce[16]) {
  Writer w{out, 0};
  w.label(kLabelKxReply, labelLen(kLabelKxReply));
  w.u16(meshId);
  w.bytes(respMac, 6);
  w.bytes(respPub, 32);
  w.bytes(replyNonce, 16);
  w.bytes(initMac, 6);
  w.bytes(initPub, 32);
  w.bytes(helloNonce, 16);
  return w.n;
}

// One key exchange as a CONFIRM's sender and receiver know it. Unknown values
// stay zero (a refusal of a HELLO: no reply nonce, no responder key).
struct KeyExContext {
  uint8_t initMac[6];
  uint8_t respMac[6];
  uint8_t helloNonce[16];
  uint8_t replyNonce[16];
  uint8_t initPub[32];
  uint8_t respPub[32];
};

inline size_t confirmInput(uint8_t out[kConfirmInputLen], const uint8_t head[kConfirmHeadLen],
                           const KeyExContext& c) {
  Writer w{out, 0};
  w.label(kLabelKxConf, labelLen(kLabelKxConf));
  w.bytes(head, kConfirmHeadLen);
  w.bytes(c.initMac, 6);
  w.bytes(c.respMac, 6);
  w.bytes(c.helloNonce, 16);
  w.bytes(c.replyNonce, 16);
  w.bytes(c.initPub, 32);
  w.bytes(c.respPub, 32);
  return w.n;
}

inline void openTranscript(uint8_t out[kOpenTranscriptLen], uint16_t sessionId, const uint8_t initMac[6],
                           const uint8_t respMac[6], const uint8_t eph[32], const uint8_t nonceA[16],
                           uint32_t sessionCaps, uint64_t openEpochMs) {
  Writer w{out, 0};
  w.label(kLabelOpen, labelLen(kLabelOpen));
  w.u16(sessionId);
  w.bytes(initMac, 6);
  w.bytes(respMac, 6);
  w.bytes(eph, 32);
  w.bytes(nonceA, 16);
  w.u32(sessionCaps);
  w.u64(openEpochMs);
}

inline void confirmTranscript(uint8_t out[kConfirmTranscriptLen], uint16_t sessionId, const uint8_t respMac[6],
                              const uint8_t initMac[6], const uint8_t eph[32], const uint8_t nonceA[16],
                              const uint8_t nonceB[16], uint32_t sessionCaps) {
  Writer w{out, 0};
  w.label(kLabelConfirm, labelLen(kLabelConfirm));
  w.u16(sessionId);
  w.bytes(respMac, 6);
  w.bytes(initMac, 6);
  w.bytes(eph, 32);
  w.bytes(nonceA, 16);
  w.bytes(nonceB, 16);
  w.u32(sessionCaps);
}

inline void rekeyTranscript(uint8_t out[kRekeyTranscriptLen], uint16_t sessionId, const uint8_t senderMac[6],
                            const uint8_t receiverMac[6], const uint8_t newEph[32], const uint8_t nonceRekey[16],
                            uint32_t prevTxSeqAtRekey) {
  Writer w{out, 0};
  w.label(kLabelRekey, labelLen(kLabelRekey));
  w.u16(sessionId);
  w.bytes(senderMac, 6);
  w.bytes(receiverMac, 6);
  w.bytes(newEph, 32);
  w.bytes(nonceRekey, 16);
  w.u32(prevTxSeqAtRekey);
}

// The three frames that carry a mesh tag (C1).
enum class MeshTagKind : uint8_t { Open, Confirm, Rekey };

inline const char* meshTagLabel(MeshTagKind k, size_t* len) {
  switch (k) {
    case MeshTagKind::Open:    *len = labelLen(kLabelOpenMk);    return kLabelOpenMk;
    case MeshTagKind::Confirm: *len = labelLen(kLabelConfirmMk); return kLabelConfirmMk;
    case MeshTagKind::Rekey:   break;
  }
  *len = labelLen(kLabelRekeyMk);
  return kLabelRekeyMk;
}

// The mesh tag's HMAC input; returns its length (at most kMeshTagInputMax).
inline size_t meshTagInput(uint8_t out[kMeshTagInputMax], MeshTagKind kind, uint16_t meshId,
                           const uint8_t* transcript, size_t transcriptLen, const uint8_t signature[64]) {
  size_t len = 0;
  const char* label = meshTagLabel(kind, &len);
  if (transcriptLen > kConfirmTranscriptLen) return 0;
  Writer w{out, 0};
  w.label(label, len);
  w.u16(meshId);
  w.bytes(transcript, transcriptLen);
  w.bytes(signature, 64);
  return w.n;
}

// The session key input (C2): the HMAC message under the bootstrap key.
inline void sessionKeyInput(uint8_t out[kSessionKeyInputLen], const uint8_t shared[32]) {
  Writer w{out, 0};
  w.label(kLabelSessionKey, labelLen(kLabelSessionKey));
  w.bytes(shared, 32);
}

// Constant-time compare of two 16-byte tags.
inline bool tagEqual(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kTagLen; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace espnowhs

#endif  // SYSTEM_ESPNOW_HANDSHAKE_CORE_H
