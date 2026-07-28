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
#include "System_ESPNow_Wire.h"  // EspNowV4Header

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
  // isSessionEncrypted=false. Use this (not the legacy, now-never-set
  // ESPNOW_V4_FLAG_ENCRYPTED header bit) for any "was this encrypted?" report.
  bool                     isSessionEncrypted;
  const char*              deviceName;
};

#endif // SYSTEM_ESPNOW_RXCTX_H
