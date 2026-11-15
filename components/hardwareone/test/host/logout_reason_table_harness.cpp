// Host harness for the logout reason table (WebServer_Server.cpp).
//
// test_logout_reason_table.py inserts MAX_LOGOUT_REASONS and LogoutReason from
// WebServer_Server.h and the production storeLogoutReason. Host boundaries:
// the Arduino String stub, a settable 32-bit millis() (unsigned long is 64
// bits here, so the double returns uint32_t as the device does) and the log
// macro. The table sits between two guard rows, so a write one element before
// or after it is seen without a sanitizer.
#include <Arduino.h>
#include <cstdint>
#include <cstdio>
#include <string>

void webBatchTestStringAppend() {}

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }
#define DEBUG_AUTHF(...) ((void)0)

// INSERT_TYPES

static LogoutReason gStorage[MAX_LOGOUT_REASONS + 2];
LogoutReason* gLogoutReasons = gStorage + 1;

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static void clearTable() {
  for (LogoutReason& row : gStorage) row = LogoutReason();
}
static bool guardsUntouched() {
  const LogoutReason& before = gStorage[0];
  const LogoutReason& after = gStorage[MAX_LOGOUT_REASONS + 1];
  return before.ip.length() == 0 && before.reason.length() == 0 && before.timestamp == 0 &&
         after.ip.length() == 0 && after.reason.length() == 0 && after.timestamp == 0;
}
static int slotOf(const char* ip) {
  for (int i = 0; i < MAX_LOGOUT_REASONS; ++i) {
    if (gLogoutReasons[i].ip == ip) return i;
  }
  return -1;
}
static std::string address(int n) { return "10.0.0." + std::to_string(n); }

// Eight addresses that never come back, stamped `stepMs` apart from `firstMs`.
static void fill(uint32_t firstMs, uint32_t stepMs) {
  clearTable();
  for (int i = 0; i < MAX_LOGOUT_REASONS; ++i) {
    gNowMs = firstMs + stepMs * static_cast<uint32_t>(i);
    storeLogoutReason(String(address(i).c_str()), String("expired"));
  }
}

int main() {
  // A free slot is used while there is one.
  fill(1000, 1000);
  for (int i = 0; i < MAX_LOGOUT_REASONS; ++i) check(slotOf(address(i).c_str()) == i, "slot " + std::to_string(i) + " holds its address");
  check(guardsUntouched(), "filling the table stays inside it");

  // Full table, no wrap: the ninth address replaces the oldest entry.
  gNowMs = 20000;
  storeLogoutReason(String("10.0.0.99"), String("limit"));
  check(slotOf("10.0.0.99") == 0, "the ninth address takes the oldest slot");
  check(slotOf("10.0.0.0") == -1 && slotOf("10.0.0.1") == 1, "only the oldest entry was replaced");
  check(gLogoutReasons[0].timestamp == 20000 && gLogoutReasons[0].reason == "limit", "the new entry is complete");
  check(guardsUntouched(), "a full table is not written past");

  // Full table whose stamps all lie before the millis() wrap, a ninth logout
  // after it: every stored stamp is above `now`. The oldest entry is replaced
  // and nothing is written one element before the table.
  fill(0xFFFF0000u, 1000);
  gNowMs = 5000;
  storeLogoutReason(String("10.0.0.99"), String("limit"));
  check(guardsUntouched(), "after the wrap the write stays inside the table");
  check(slotOf("10.0.0.99") == 0, "after the wrap the ninth address takes the oldest slot");
  check(slotOf("10.0.0.0") == -1, "after the wrap the oldest entry is the one replaced");
  for (int i = 1; i < MAX_LOGOUT_REASONS; ++i) check(slotOf(address(i).c_str()) == i, "after the wrap entry " + std::to_string(i) + " is kept");

  // Stamps on both sides of the wrap: slots 0 to 5 before it, 6 and 7 after
  // it. The oldest by age is slot 0, not the entry with the smallest number.
  fill(0xFFFFE000u, 1500);  // slot 6 lands on 0x00000400, slot 7 on 0x000009DC
  check(gLogoutReasons[5].timestamp > 0xFFFF0000u && gLogoutReasons[6].timestamp < 0x00010000u, "the fill crosses the wrap");
  gNowMs = 0x00002000u;
  storeLogoutReason(String("10.0.0.99"), String("limit"));
  check(slotOf("10.0.0.99") == 0, "with stamps on both sides of the wrap the oldest by age is replaced");
  check(slotOf("10.0.0.6") == 6 && slotOf("10.0.0.7") == 7, "the two newest entries are kept");
  check(guardsUntouched(), "mixed stamps: the write stays inside the table");

  // An address that is already in the table keeps its slot.
  gNowMs = 0x00009000u;
  storeLogoutReason(String("10.0.0.3"), String("another device"));
  check(slotOf("10.0.0.3") == 3 && gLogoutReasons[3].reason == "another device", "a known address is updated in place");
  check(guardsUntouched(), "updating a known address stays inside the table");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d logout reason table checks failed\n", failures, checks);
    return 1;
  }
  std::printf("logout reason table: %d checks passed\n", checks);
  return 0;
}
