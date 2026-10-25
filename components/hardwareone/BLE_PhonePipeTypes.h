#pragma once
// Plain values shared by the phone pipe owner and its HTTP transport caller.
// No Arduino, ESP-IDF or project dependencies, so host tests include it as is.
#include <cstdint>
// corr is the session's pipe HELLO nonce: a non-secret correlation tag both
// ends can log (F3). openedAt is the OPEN_OK time.
struct PhonePipeStream { uint32_t generation=0; uint16_t id=0; uint32_t openedAt=0; uint64_t corr=0; };
// A-1: the OPEN is fully submitted within 2 s, then the phone has 4 s from local
// acceptance of the last OPEN fragment to answer. Neither phase extends the
// caller's deadline, and an OPEN is not started with less than 4 s left.
constexpr uint32_t kPhonePipeOpenSubmitMs=2000,kPhonePipeOpenResponseMs=4000;
enum class PhonePipeOpenStatus : uint8_t {
  Opened,          // OPEN_OK: the phone has a TCP connection to the target
  Refused,         // OPEN_ERR (or RESET) from the phone before OPEN_OK; see code
  NotReady,        // no READY session, or the phone's relay is off (RELAY_ON 0); nothing submitted
  Rejected,        // refused locally (no free stream, invalid target); nothing submitted
  NoBudget,        // less than the response allowance left; nothing submitted
  SubmitTimeout,   // the OPEN was not fully submitted within 2 s
  ResponseTimeout, // no OPEN_OK or OPEN_ERR within 4 s of the last fragment
  LinkLost,        // the session ended while waiting
  Cancelled,       // caller cancellation
};
struct PhonePipeOpenResult { PhonePipeOpenStatus status=PhonePipeOpenStatus::NotReady; uint16_t code=0; };
