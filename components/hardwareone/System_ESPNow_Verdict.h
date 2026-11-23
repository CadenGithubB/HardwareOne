// System_ESPNow_Verdict.h - what a receive handler did with its frame (ESP-NOW
// 1.0 step 5b, DESIGN 3.1/3.2). Self-contained (stdint only): host tests that
// extract V4RxCtx paste it in as it is. The two status numbers it needs are
// the shared ones of System_ESPNow_AnswerCore.h (asserted in
// System_ESPNow_RxCtx.h).
#ifndef SYSTEM_ESPNOW_VERDICT_H
#define SYSTEM_ESPNOW_VERDICT_H

#include <stdint.h>

// espnowanswer::kRefused and espnowanswer::kSharedLast.
constexpr uint8_t kV4VerdictRefused = 2;
constexpr uint8_t kV4VerdictSharedLast = 15;

// What a receive handler did with its frame (1.0 step 5b, DESIGN 3.1/3.2). The
// receive path answers ACK_REQ only after this verdict:
//   Accepted  the frame was taken (queued, stored or acted on); ACK status 0.
//             A duplicate of it is answered again (flags.duplicate) and never
//             processed twice.
//   Refused   the frame was not taken and the sender is told why: an ACK with
//             `status` (a shared status 1-15, System_ESPNow_AnswerCore.h). Not
//             remembered: a retry is processed again (busy, no slot).
//   Silent    no answer (a sender that is not an authenticated session peer and
//             a policy refusal: no oracle, G3). Remembered: a retry stays silent.
enum class V4VerdictKind : uint8_t { Accepted = 0, Refused = 1, Silent = 2 };
struct V4Verdict {
  V4VerdictKind kind;
  uint8_t status;    // Refused only; 0 otherwise
  uint8_t ackFlags;  // the ACK's flags byte (ESPNOW_V4_ACK_FLAG_DUPLICATE); set by the receive path only
};
inline V4Verdict v4Accepted() { return V4Verdict{V4VerdictKind::Accepted, 0, 0}; }
inline V4Verdict v4Silent() { return V4Verdict{V4VerdictKind::Silent, 0, 0}; }
inline V4Verdict v4RefusedStatus(uint8_t status) {
  // Only the shared statuses 1-15 mean something on an ACK (DESIGN 2.2).
  return V4Verdict{V4VerdictKind::Refused,
                   (status >= 1 && status <= kV4VerdictSharedLast) ? status : kV4VerdictRefused, 0};
}

#endif // SYSTEM_ESPNOW_VERDICT_H
