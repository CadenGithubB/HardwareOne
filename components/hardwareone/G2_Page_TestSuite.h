#ifndef G2_PAGE_TESTSUITE_H
#define G2_PAGE_TESTSUITE_H

// =============================================================================
// G2 glasses — "Tests" hijack page
// =============================================================================
// On-glasses test bench for the G2 transport. Each menu entry sends a
// payload of a known size through the regular CREATE-list path so we can
// observe firmware behaviour at single-fragment, near-ceiling, and multi-
// fragment scales without recompiling.
//
// The test pattern is "if you can see the page, the transport worked":
// each tap rebuilds the lens with a list whose total pb body approximates
// the requested size. Successful render → multi-frag CREATE landed.
// Failed render → auto-recovery in pageSwapListWorker drops the user back
// at the test-suite menu and the failure is visible in the serial log.
//
// Build-guarded behind ENABLE_BLUETOOTH + ENABLE_G2_GLASSES + ENABLE_G2_TESTSUITE
// (bring-up tooling; off in shipping builds) so trimmed
// builds get harmless no-op stubs.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_TESTSUITE

// CLI-direct text dump describing the available size brackets (for
// `g2tests` invocation when not in a hijack). Same shape as the other
// page modules' buildText callbacks — fills `out` with a newline-
// separated summary, capped to `cap`.
void g2BuildTestSuiteInfo(char* out, size_t cap);

// Render the test-suite menu (back + size brackets) on the lens. Called
// by the hijack dispatcher when the user taps "Tests" in the root menu.
void g2ShowTestSuiteMenu();

// Tap dispatcher for the test-suite page. idx 0 = back, idx N>0 picks a
// size bracket and re-renders the lens with a payload of that size.
void g2TestSuiteHandleTap(uint32_t idx);

#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_TESTSUITE)

inline void g2BuildTestSuiteInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowTestSuiteMenu() {}
inline void g2TestSuiteHandleTap(uint32_t /*idx*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_TESTSUITE
#endif  // G2_PAGE_TESTSUITE_H
