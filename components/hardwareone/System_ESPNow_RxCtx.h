// System_ESPNow_RxCtx.h - Shared V4 RX dispatch context
//
// Phase 0 relay groundwork (docs/ESPNOW_RELAY_RESTORE_PLAN.md): V4RxCtx used
// to be defined privately in System_ESPNow.cpp and DUPLICATED — out of sync,
// missing the two auth bools — in System_ESPNow_Handlers_Crypto.cpp. That was
// an ODR violation both TUs compiled clean through: every field after isPaired
// read garbage in the handlers TU. One definition, included by both, ends that
// class of bug and lets later phases extend the struct in exactly one place.
#ifndef SYSTEM_ESPNOW_RXCTX_H
#define SYSTEM_ESPNOW_RXCTX_H

#include <esp_now.h>
#include <string.h>
#include <type_traits>
#include "System_ESPNow_Wire.h"  // EspNowV4Header
#include "System_ESPNow_Verdict.h"     // V4Verdict, the handlers' return type

static_assert(kV4VerdictRefused == espnowanswer::kRefused && kV4VerdictSharedLast == espnowanswer::kSharedLast,
              "System_ESPNow_Verdict.h uses the shared statuses");

struct V4RxCtx {
  const esp_now_recv_info* recv_info;
  const EspNowV4Header*    h;
  const uint8_t*           payload;
  uint16_t                 payloadLen;
  bool                     isPaired;
  // 2026-05-19: `isAuthenticated` is true when this frame proves the sender
  // holds either our session key (SESSION_FRAME unwrap succeeded) or the mesh
  // group key (BROADCAST_AUTH HMAC verified). Plaintext unicast and plain-
  // broadcast (no BROADCAST_AUTH tag) leave it false. Handlers that mutate
  // device-level state from the frame (clock, master role, peer identity
  // claims) MUST gate on this — otherwise anyone in radio range can spoof.
  bool                     isAuthenticated;
  // `isSessionEncrypted` is the narrower signal: true ONLY when the frame
  // arrived AEAD-wrapped in a SESSION_FRAME (confidential). A BROADCAST_AUTH
  // frame is authenticated-but-plaintext, so it sets isAuthenticated=true but
  // isSessionEncrypted=false. Use this (the old ENCRYPTED header bit was never
  // set and is gone at 1.0) for any "was this encrypted?" report.
  bool                     isSessionEncrypted;
  const char*              deviceName;

  // The one receive style (System_ESPNow_Wire.h, "Frame rules"). The
  // dispatcher has already dropped a frame shorter than the row's minLen, so
  // a fixed frame is complete; a longer frame (a later sender) is read up to
  // the fields this firmware knows. Fields the frame lacks read as 0 (an
  // older sender, or a direct call that bypasses the table).
  template <class T>
  void read(T& out) const {
    static_assert(std::is_trivially_copyable<T>::value, "read() copies wire structs only");
    memset(&out, 0, sizeof(T));
    const size_t n = payloadLen < sizeof(T) ? payloadLen : sizeof(T);
    if (payload && n) memcpy(&out, payload, n);
  }
  // The bytes after the first `off` (a header-plus-tail frame: text, entries,
  // file data). nullptr and *len 0 when the payload has no bytes past `off`,
  // so a handler never subtracts from payloadLen itself.
  const uint8_t* tail(size_t off, size_t* len) const {
    if (!payload || payloadLen <= off) {
      if (len) *len = 0;
      return nullptr;
    }
    if (len) *len = (size_t)payloadLen - off;
    return payload + off;
  }

  // The verdict helpers handlers return (DESIGN 3.1 G2/G3).
  // accepted(): the frame was taken.
  V4Verdict accepted() const { return v4Accepted(); }
  // refuse(): a policy refusal (status 2, kRefused). Answered only to an
  // authenticated session peer (a sealed frame): it already proved who it is.
  // Toward anyone else it is silence, so a refusal is no oracle.
  V4Verdict refuse() const { return isSessionEncrypted ? v4RefusedStatus(kV4VerdictRefused) : v4Silent(); }
  // refuseWith(): a capacity or support refusal (busy, no slot, too large,
  // unsupported, bad request), answered to any sender whose frame earns an
  // ACK (sealed or group-tagged; a plaintext frame is never answered).
  V4Verdict refuseWith(uint8_t status) const { return v4RefusedStatus(status); }
  // silent(): no answer, and a retry stays unanswered.
  V4Verdict silent() const { return v4Silent(); }
};

#endif // SYSTEM_ESPNOW_RXCTX_H
