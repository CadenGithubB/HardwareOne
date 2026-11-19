// The ESP-NOW channel for the Wi-Fi state and the 'espnowchannel' checker
// (System_ESPNow_ChannelCore.h, the production dependency-free header).
//
// Bench 2026-11-18: the P4, joined to an AP on channel 11 with ESP-NOW stopped
// (no device name), printed "Expected right now: 0 (following the joined AP)"
// and "Checker: MISMATCH". The expected channel was 0 whenever ESP-NOW was not
// running. The rules run here; the command's use of them cannot run on the
// host, so S1-S3 read System_ESPNow.cpp (passed as argv[1]) and pin that the
// checker computes the expected channel whether or not ESP-NOW runs and takes
// its verdict from espnowch::check (S1/S2 fail on the bench build's checker).
#include "System_ESPNow_ChannelCore.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      gFailures++;                                     \
    }                                                  \
  } while (0)

using espnowch::Verdict;

// The text from `start` up to the first `end` after it, or "".
static std::string between(const std::string& text, const char* start, const char* end) {
  const size_t a = text.find(start);
  if (a == std::string::npos) return "";
  const size_t b = text.find(end, a);
  return b == std::string::npos ? "" : text.substr(a, b - a);
}

static void checkSource(const char* path) {
  std::ifstream in(path);
  CHECK(in.good(), "S0 cannot read %s", path);
  std::stringstream all;
  all << in.rdbuf();
  const std::string src = all.str();
  const std::string checker = between(src, "const char* cmd_espnow_channel(", "String arg = a.arg(0);");
  CHECK(!checker.empty(), "S0 cmd_espnow_channel's report not found");
  CHECK(checker.find("const uint8_t expected = espnowComputeChannel();") != std::string::npos &&
            checker.find("espnowComputeChannel() : 0") == std::string::npos,
        "S1 the checker's expected channel depends on ESP-NOW running");
  CHECK(checker.find("switch (espnowch::check(joined, running, (uint8_t)hwCh, expected))") != std::string::npos,
        "S2 the checker's verdict is not espnowch::check");
  const std::string compute = between(src, "static uint8_t espnowComputeChannel() {", "\n}\n");
  CHECK(compute.find("return espnowch::expected(WiFi.status() == WL_CONNECTED, staCh, gSettings.espnowChannel);") !=
            std::string::npos,
        "S3 espnowComputeChannel does not use espnowch::expected");
}

int main(int argc, char** argv) {
  if (argc > 1) checkSource(argv[1]);
  else CHECK(false, "S0 usage: espnow_channel_core_tests <System_ESPNow.cpp>");

  // C1 joined: the AP's channel, whatever the setting.
  CHECK(espnowch::expected(true, 11, 0) == 11, "C1 joined on 11, auto: 11");
  CHECK(espnowch::expected(true, 11, 6) == 11, "C1 joined on 11, setting 6: the AP wins");
  // C2 offline: the setting, else the last-known channel, else 1.
  CHECK(espnowch::expected(false, 11, 6) == 6, "C2 offline with setting 6: 6");
  CHECK(espnowch::expected(false, 11, 0) == 11, "C2 offline, auto, last known 11: 11");
  CHECK(espnowch::expected(false, 0, 0) == 1, "C2 offline, auto, nothing known: 1");
  CHECK(espnowch::expected(true, 0, 0) == 1, "C2 joined but no channel reported: the fallback");
  CHECK(espnowch::expected(false, 14, 0) == 1 && espnowch::expected(false, 0, 14) == 1,
        "C2 channels outside 1..13 are ignored");

  // C3 the bench case: joined on 11, ESP-NOW stopped, radio on 11.
  const uint8_t want = espnowch::expected(true, 11, 0);
  CHECK(espnowch::check(true, false, 11, want) == Verdict::Ok, "C3 joined, ESP-NOW stopped, radio on the AP: OK");
  CHECK(espnowch::check(true, true, 11, want) == Verdict::Ok, "C3 joined, ESP-NOW running: OK");
  CHECK(espnowch::check(true, false, 6, want) == Verdict::Mismatch, "C3 joined, radio elsewhere: MISMATCH");
  // C4 offline: only a running ESP-NOW pins the radio.
  CHECK(espnowch::check(false, false, 3, espnowch::expected(false, 0, 6)) == Verdict::NotPinned,
        "C4 offline and stopped: not a mismatch");
  CHECK(espnowch::check(false, true, 6, espnowch::expected(false, 0, 6)) == Verdict::Ok, "C4 offline, pinned: OK");
  CHECK(espnowch::check(false, true, 3, espnowch::expected(false, 0, 6)) == Verdict::Mismatch,
        "C4 offline, running, radio drifted: MISMATCH");

  if (gFailures) {
    std::printf("espnow_channel_core_tests: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("espnow_channel_core_tests: all passed\n");
  return 0;
}
