// Host-side adversarial tests for the exact production dictation gates.
#include <cstdio>

#include "../../System_DictationPolicy.h"

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void test_exact_session_capability() {
  constexpr uint32_t oldEpoch = 41;
  constexpr uint32_t liveEpoch = 42;

  CHECK(!dictationHostCapabilityReady(0, 0, true, true, true),
        "epoch zero must never authorize dictation");
  CHECK(!dictationHostCapabilityReady(liveEpoch, 0, true, true, true),
        "no hostready command must hide dictation");
  CHECK(!dictationHostCapabilityReady(liveEpoch, oldEpoch, true, true, true),
        "a reconnect must revoke the previous session capability");
  CHECK(!dictationHostCapabilityReady(liveEpoch, liveEpoch, false, true, true),
        "presence from no exact session must not substitute for hostready");
  CHECK(!dictationHostCapabilityReady(liveEpoch, liveEpoch, true, false, true),
        "stale CM5 presence must hide dictation");
  CHECK(!dictationHostCapabilityReady(liveEpoch, liveEpoch, true, true, false),
        "starting/degraded/unknown CM5 must hide dictation");
  CHECK(dictationHostCapabilityReady(liveEpoch, liveEpoch, true, true, true),
        "exact hostready plus fresh Ready/Busy presence must authorize");
}

static void test_request_after_publication_only() {
  CHECK(!dictationRequestPublicationReady(true, true, false, false),
        "a merely closed WAV must not emit dictate_request");
  CHECK(!dictationRequestPublicationReady(true, true, true, false),
        "a published result while recorder is busy must not emit");
  CHECK(!dictationRequestPublicationReady(true, true, false, true),
        "IDLE without an owner-scoped result must not emit");
  CHECK(!dictationRequestPublicationReady(false, true, true, true),
        "a cancelled/replaced owner must suppress the event");
  CHECK(!dictationRequestPublicationReady(true, false, true, true),
        "failed/discarded capture must suppress the event");

  // New-firmware zero-delay contract: once the event gate opens, the exact
  // result is already published and recorderBusy is false. Therefore a host
  // that voicefetches in its EVT callback passes its first lifecycle check.
  const bool resultPublished = true;
  const bool recorderIdle = true;
  CHECK(dictationRequestPublicationReady(true, true, resultPublished,
                                        recorderIdle),
        "post-publication event must be admitted");
  CHECK(resultPublished && recorderIdle,
        "zero-delay voicefetch must see result+IDLE on first attempt");
}

static void test_response_is_request_session_only() {
  CHECK(!dictationResponseSessionReady(0, 9),
        "an unstamped request must reject a response");
  CHECK(!dictationResponseSessionReady(9, 0),
        "a stateless command must reject a response");
  CHECK(!dictationResponseSessionReady(9, 10),
        "replacement UART session must not deliver an old transcript");
  CHECK(dictationResponseSessionReady(9, 9),
        "the exact request UART session must be accepted");

}

static void test_cancel_race_policy() {
  CHECK(!dictationRequestPushFenceReady(false, true),
        "cancel that wins before the final fence must suppress request TX");
  CHECK(!dictationRequestPushFenceReady(true, false),
        "request must not cross into a replacement UART epoch");
  CHECK(dictationRequestPushFenceReady(true, true),
        "exact owner/session may begin request TX");
  CHECK(!dictationCancelEventNeeded(false, false),
        "cancel-before-push must suppress request without a cancel EVT");
  CHECK(dictationCancelEventNeeded(true, false),
        "cancel-during-push needs a tombstone even if TX later reports false");
  CHECK(dictationCancelEventNeeded(false, true),
        "cancel-after-push must cancel host work");
  CHECK(dictationCancelEventNeeded(true, true),
        "in-flight/pushed overlap must still request exactly one cancel");
}

static void test_waiting_host_lease() {
  CHECK(!dictationWaitingHostReady(false, 9, 9, true),
        "closed UART must terminate the waiting exchange");
  CHECK(!dictationWaitingHostReady(true, 9, 10, true),
        "replacement login must terminate the old exchange");
  CHECK(!dictationWaitingHostReady(true, 9, 9, false),
        "lost capability/presence must terminate the exchange");
  CHECK(dictationWaitingHostReady(true, 9, 9, true),
        "exact live ready host may continue processing");
  CHECK(!dictationHostResultExpired(89999),
        "host lease must remain live immediately below its boundary");
  CHECK(dictationHostResultExpired(90000),
        "host lease must expire at the documented boundary");
}

static void test_direct_uart_wire_policy() {
  CHECK(dictationUartLineIsProtocol("dictate"),
        "bare dictate must be claimed by the direct UART protocol");
  CHECK(dictationUartLineIsProtocol("DiCtAtE status"),
        "the protocol root is case-insensitive");
  CHECK(dictationUartLineIsProtocol("\t dictate\tresult"),
        "ASCII whitespace around the root must be accepted");
  CHECK(!dictationUartLineIsProtocol(nullptr), "null is not a protocol line");
  CHECK(!dictationUartLineIsProtocol(""), "empty is not a protocol line");
  CHECK(!dictationUartLineIsProtocol("dictatefoo result"),
        "a root lookalike must not be consumed");
  CHECK(!dictationUartLineIsProtocol("dictated result"),
        "the root requires a word boundary");

  uint64_t id = 0;
  CHECK(dictationParseWireId("12345678abcdef01", 16, &id) &&
            id == UINT64_C(0x12345678abcdef01),
        "a valid exact wire id must parse");
  CHECK(!dictationParseWireId("00000000abcdef01", 16, &id),
        "a zero high half must be rejected");
  CHECK(!dictationParseWireId("1234567800000000", 16, &id),
        "a zero low half must be rejected");
  CHECK(!dictationParseWireId("12345678abcdef0g", 16, &id),
        "non-hex ids must be rejected");
  CHECK(!dictationParseWireId("12345678abcdef01", 15, &id),
        "ids must be exactly sixteen bytes");

  CHECK(dictationPrintableAscii("hello, world!", 13, false),
        "ordinary transcript punctuation must be accepted");
  CHECK(!dictationPrintableAscii("", 0, false),
        "an empty transcript must be rejected");
  CHECK(dictationPrintableAscii("", 0, true),
        "an omitted failure reason is legal");
  const char newlineText[] = {'a', '\n', 'b', '\0'};
  CHECK(!dictationPrintableAscii(newlineText, 3, false),
        "control characters must not enter the UART reply grammar");
  const char utf8Text[] = {static_cast<char>(0xc3), static_cast<char>(0xa9),
                           '\0'};
  CHECK(!dictationPrintableAscii(utf8Text, 2, false),
        "non-ASCII bytes must be normalized by the host first");
}

int main() {
  test_exact_session_capability();
  test_request_after_publication_only();
  test_response_is_request_session_only();
  test_cancel_race_policy();
  test_waiting_host_lease();
  test_direct_uart_wire_policy();
  if (failures) {
    std::fprintf(stderr, "%d dictation policy test(s) failed\n", failures);
    return 1;
  }
  std::puts("dictation policy tests passed");
  return 0;
}
