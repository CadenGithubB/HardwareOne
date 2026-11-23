// The ESP-NOW channel for the Wi-Fi state and the 'espnowchannel' checker
// (System_ESPNow_ChannelCore.h, the production dependency-free header).
//
// Bench 2026-11-18: the P4, joined to an AP on channel 11 with ESP-NOW stopped
// (no device name), printed "Expected right now: 0 (following the joined AP)"
// and "Checker: MISMATCH". The expected channel was 0 whenever ESP-NOW was not
// running. The rules run here; the command's use of them cannot run on the
// host, so S1-S5 read System_ESPNow.cpp (passed as argv[1]; S6 reads System_WiFi.cpp,
// argv[2]) and pin that the
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
  CHECK(compute.find("const bool joined = WiFi.status() == WL_CONNECTED;") != std::string::npos &&
            compute.find("return espnowch::expected(joined, staCh, gSettings.espnowChannel);") != std::string::npos,
        "S3 espnowComputeChannel does not use espnowch::expected");
  // Channel follow (System_ESPNow_ChannelFollowCore.h): only a worker in auto
  // that is not joined takes the follow target, and never a sweep dwell.
  CHECK(compute.find("if (!joined && gSettings.espnowChannel == 0 && !wifiStaBusyForChannelHunt()) {") != std::string::npos &&
            compute.find("gChFollowTarget.load(std::memory_order_acquire)") != std::string::npos,
        "S4 espnowComputeChannel does not take the follow target for an unjoined worker in auto, and only while "
        "Wi-Fi is not associated, connecting or scanning");

  // S5: the channel follow's hooks are wired (the rx-frontend and
  // dispatch-policy harnesses stub them, so only these checks notice a
  // deleted call).
  const std::string hb = between(src, "static V4Verdict v4h_heartbeat(", "\n}\n");
  CHECK(hb.find("espnowChFollowOnHeartbeat(ctx, hb, senderProven);") != std::string::npos,
        "S5 v4h_heartbeat does not feed the channel follow");
  CHECK(hb.find("if (!espnowChFollowHunting()) {") != std::string::npos,
        "S5 the session pre-warm is not skipped while the channel follow hunts");
  // 1.0 step 5a: the ACK carries its own msgId and names the probe in its
  // payload; the ACK handler v4_rx_ack reports ackFor and the class, and the
  // follow takes only a tagged ACK (the probe is group-tagged).
  const std::string incoming = between(src, "static bool v4_try_handle_incoming(const esp_now_recv_info* recv_info, const uint8_t* data, int len) {", "\n}\n");
  CHECK(incoming.find("v4_rx_ack(recv_info, h, payload, payloadLen, wasSessionEncrypted);") != std::string::npos,
        "S5 the receive path does not hand ACK frames to v4_rx_ack");
  const std::string rxAck = between(src, "static void v4_rx_ack(", "\n}\n");
  CHECK(rxAck.find("espnowChFollowNoteAck(recv_info, a.ackFor, sealed);") != std::string::npos,
        "S5 the ACK handler does not report probe ACKs (ackFor, class) to the channel follow");
  const std::string noteAck = between(src, "static void espnowChFollowNoteAck(", "\n}\n");
  CHECK(noteAck.find("if (sealed) return;") != std::string::npos &&
            noteAck.find("ackFor != gChFollowProbeMsgId") != std::string::npos,
        "S5 the follow's ACK hook does not match the probe on ackFor and take only a tagged ACK");
  const std::string dispatch = between(src, "static V4Verdict v4_dispatch_table_try(", "\n}\n");
  CHECK(dispatch.find("if (isSessionEncrypted) espnowChFollowOnSealedFrame(recv_info, h->type);") != std::string::npos,
        "S5 the dispatcher does not report sealed frames to the channel follow");
  const std::string beats = between(src, "void processMeshHeartbeats() {", "\n}\n");
  CHECK(beats.find("const espnowfollow::Out out = gChFollow.tick(in);") != std::string::npos,
        "S5 processMeshHeartbeats does not tick the channel follow (step 0a)");
  const std::string deinit = between(src, "static bool deinitEspNow(bool publishOffEvent) {", "\n}\n");
  CHECK(deinit.find("espnowChFollowStopped();") != std::string::npos,
        "S5 deinitEspNow leaves the channel follow's Wi-Fi hold and target set");
  const std::string rx = between(src, "static void onEspNowDataReceived(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len) {", "\n}\n");
  CHECK(rx.find(": (gEspNow ? gEspNow->channel : 0);") != std::string::npos,
        "S5 the RX callback does not stamp the radio channel when rx_ctrl has none");
}

// S6: the Wi-Fi rehunt waits while a mesh worker holds it (System_WiFi.cpp).
static void checkWifiSource(const char* path) {
  std::ifstream in(path);
  CHECK(in.good(), "S6 cannot read %s", path);
  std::stringstream all;
  all << in.rdbuf();
  const std::string src = all.str();
  const std::string poll = between(src, "void wifiRehuntPoll() {", "\n}\n");
  const size_t held = poll.find("in.held =");
  CHECK(held != std::string::npos && poll.find("espnowWorkerHoldsWifiRetry()", held) != std::string::npos &&
            poll.find("espnowWorkerHoldsWifiRetry()", held) < poll.find(";", held),
        "S6 wifiRehuntPoll's held does not include espnowWorkerHoldsWifiRetry()");
  const std::string busy = between(src, "bool wifiStaBusyForChannelHunt() {", "\n}\n");
  CHECK(busy.find("WiFi.STA.connected()") != std::string::npos && busy.find("WifiCoex::Associated") != std::string::npos,
        "S6 wifiStaBusyForChannelHunt does not count an association without an IP as busy");
}

int main(int argc, char** argv) {
  if (argc > 2) {
    checkSource(argv[1]);
    checkWifiSource(argv[2]);
  } else {
    CHECK(false, "S0 usage: espnow_channel_core_tests <System_ESPNow.cpp> <System_WiFi.cpp>");
  }

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
