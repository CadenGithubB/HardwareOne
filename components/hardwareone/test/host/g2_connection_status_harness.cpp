// Shipping definitions are inserted by test_g2_connection_status.py.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "G2_RepairCore.h"

using String = std::string;
static unsigned assertions = 0, entries = 0, exits = 0, depth = 0;
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
// INSERT_TYPES
#if TEST_RUNTIME
static G2ClientRuntime gClientRuntime{};
static G2ClientRuntime& client = gClientRuntime;
#else
static G2ClientState client{};
static G2ClientState* gG2State = &client;
#endif
static void selectLifecycle(int lifecycle) {
#if !TEST_RUNTIME
  gG2State = lifecycle == 0 ? nullptr : &client;
#endif
  client.initialized = lifecycle == 2;
}
static G2Temple gL{}, gR{};
static int8_t gBatteryL = -1, gBatteryR = -1;
static int gTopologyMux = 0, gControlLifecycleMux = 0, gRecoveryMux = 0;
static g2repair::Schedule gRepair{};
static uint8_t gClientInitters = 0;
static std::vector<int*> held;
static void portENTER_CRITICAL(int* mux) {
  check(mux == &gTopologyMux || mux == &gControlLifecycleMux || mux == &gRecoveryMux,
        "only existing topology/lifecycle/repair guards are used");
  check(held.empty() || (held.size() == 1 && held.back() == &gControlLifecycleMux &&
                        mux == &gTopologyMux),
        "lock nesting follows lifecycle then topology; the repair lock is taken alone");
  held.push_back(mux); ++entries; ++depth;
}
static void portEXIT_CRITICAL(int* mux) {
  check(!held.empty() && held.back() == mux, "matching critical section exit");
  held.pop_back(); ++exits; --depth;
}

// INSERT_PRODUCTION

// The one-temple repair: 0 off, 1 fast try 3, 2 slow, 3 held.
static void selectRepair(int r) {
  gRepair = g2repair::Schedule{};
  if (r == 0) return;
  gRepair.restart(1000, 0);
  if (r == 1) gRepair.failed = 2;
  if (r == 2) gRepair.failed = g2repair::kFastTries;
  if (r == 3) gRepair.held = true;
}
// What status shows: the schedule only while running with exactly one temple up.
static const char* repairText(int r, int lifecycle) {
  static const char* const names[] = {"none", "3/7", "slow", "held"};
  return lifecycle == 2 && gL.connected != gR.connected ? names[r] : "none";
}

static void checkText(const std::string& expected) {
  char text[256];
  getG2Status(text, sizeof(text));
  check(text == expected, "text preserves state/link/numeric contract");
  std::cout << text << '\n';
  for (size_t cap = 0; cap <= expected.size() + 2; ++cap) {
    std::array<char, 256> bounded;
    bounded.fill('!');
    const unsigned before = entries;
    getG2Status(bounded.data(), cap);
    if (cap == 0) {
      check(entries == before, "zero capacity skips snapshot");
    } else {
      check(std::string(bounded.data()) == expected.substr(0, cap - 1),
            "short buffer receives exact snprintf prefix");
    }
    check(std::all_of(bounded.begin() + cap, bounded.end(),
                     [](char c) { return c == '!'; }), "text never writes beyond capacity");
  }
  const unsigned before = entries;
  getG2Status(nullptr, 0);
  getG2Status(nullptr, 100);
  check(entries == before, "null output skips status capture");
}

static void characterize() {
  const char* names[] = {"idle", "scanning", "connecting", "authenticating",
                         "connected", "disconnecting", "error"};
  for (int lifecycle : {0, 1, 2}) {
    selectLifecycle(lifecycle);
    for (int fsm = 0; fsm < 7; ++fsm) {
      client.state = lifecycle == 0 ? G2_STATE_IDLE : static_cast<G2State>(fsm);
      for (int links = 0; links < 4; ++links) for (int r = 0; r < (TEST_REPAIR ? 4 : 1); ++r) {
        gL.connected = (links & 1) != 0;
        gR.connected = (links & 2) != 0;
        selectRepair(r);
        const bool repairRead = TEST_REPAIR && lifecycle == 2 && gL.connected != gR.connected;
        const char* expectedState = lifecycle != 2 ? "off" :
            fsm == G2_STATE_CONNECTED ? links == 3 ? "connected" :
            links ? "connecting" : "idle" : names[fsm];
        check(std::strcmp(getG2StateString(), expectedState) == 0,
              "all FSM, initialization and partial-link state names preserved");
        check(getG2State() == (lifecycle == 0 ? G2_STATE_IDLE : client.state),
              "owner enum retains uninitialized idle fallback");
        for (int values : {0, 1}) {
          gL.mtu = values ? UINT16_MAX : 23; gR.mtu = values ? 517 : 0;
          gL.packetsSent = values ? UINT32_MAX : 0;
          gR.packetsSent = values ? 1234567 : 0;
          gL.packetsReceived = values ? 7654321 : 0;
          gR.packetsReceived = values ? UINT32_MAX : 0;
          gBatteryL = values ? 100 : -1; gBatteryR = values ? 0 : -1;
#if TEST_TYPED
          const unsigned before = entries;
          check(!g2ConnectionStatusSnapshot(nullptr) && before == entries,
                "null snapshot is unavailable without state access");
          G2ConnectionStatus status;
          check(g2ConnectionStatusSnapshot(&status), "compiled owner always offers status");
          check(entries == before + (TEST_RUNTIME ? 2 : 1) + (repairRead ? 1 : 0) &&
                entries == exits && depth == 0,
                "snapshot takes and releases existing ownership guards");
#if TEST_REPAIR
          check(std::strcmp(status.repair, repairText(r, lifecycle)) == 0,
                "repair shows only while running with one temple up");
#endif
          check(std::strcmp(status.state, expectedState) == 0 &&
                status.running == (lifecycle == 2), "running distinguishes off from initialized idle");
          check(status.left.connected == gL.connected && status.right.connected == gR.connected &&
                status.left.mtu == gL.mtu && status.right.mtu == gR.mtu &&
                status.left.battery == gBatteryL && status.right.battery == gBatteryR &&
                status.left.tx == gL.packetsSent && status.right.tx == gR.packetsSent &&
                status.left.rx == gL.packetsReceived && status.right.rx == gR.packetsReceived,
                "snapshot retains temple identities, counters, MTUs and unknown battery");
          char formatted[256];
          const unsigned beforeFormat = entries;
          formatG2Status(status, formatted, sizeof(formatted));
          check(entries == beforeFormat, "formatting a captured snapshot does not resample owner state");
#endif
          char expected[256];
          std::snprintf(expected, sizeof(expected),
              "state=%s L=%s R=%s mtu=L%u/R%u batt=L%d/R%d tx=L%lu/R%lu rx=L%lu/R%lu",
              expectedState, gL.connected ? "up" : "down", gR.connected ? "up" : "down",
              unsigned(gL.mtu), unsigned(gR.mtu), int(gBatteryL), int(gBatteryR),
              static_cast<unsigned long>(gL.packetsSent), static_cast<unsigned long>(gR.packetsSent),
              static_cast<unsigned long>(gL.packetsReceived), static_cast<unsigned long>(gR.packetsReceived));
#if TEST_REPAIR
          std::strcat(expected, " repair=");
          std::strcat(expected, repairText(r, lifecycle));
#endif
#if TEST_TYPED
          check(std::strcmp(formatted, expected) == 0, "explicit snapshot formatter matches legacy text");
          for (size_t cap = 0; cap <= std::strlen(expected) + 2; ++cap) {
            std::array<char, 256> output;
            output.fill('!');
            formatG2Status(status, output.data(), cap);
            if (cap > 0) check(std::string(output.data()) == std::string(expected).substr(0, cap - 1),
                               "explicit formatter respects every bounded prefix");
            check(std::all_of(output.begin() + cap, output.end(), [](char c) { return c == '!'; }),
                  "explicit formatter does not write beyond capacity");
          }
          formatG2Status(status, nullptr, 0);
          formatG2Status(status, nullptr, 100);
#endif
          checkText(expected);
        }
      }
    }
  }
}

#if TEST_TYPED
#if TEST_RUNTIME
static void startupPublication() {
  selectLifecycle(0); client.state = G2_STATE_IDLE;
  gL.connected = gR.connected = true;
  gL.mtu = gR.mtu = 247; gL.packetsSent = gR.packetsSent = 123;
  gClientInitters = 1;
  const unsigned before = entries;
  G2ConnectionStatus status;
  check(g2ConnectionStatusSnapshot(&status) && !status.running &&
        std::strcmp(status.state, "off") == 0, "initializing runtime remains off until publication");
  check(entries == before + 1, "startup capture does not enter topology or access rebuilding temples");
  check(!status.left.connected && !status.right.connected && status.left.mtu == 0 &&
        status.right.mtu == 0 && status.left.battery == -1 && status.right.battery == -1 &&
        status.left.tx == 0 && status.right.tx == 0, "startup status uses default unavailable telemetry");
  const unsigned stateBefore = entries;
  check(std::strcmp(getG2StateString(), "off") == 0 && entries == stateBefore + 1,
        "state-name read also avoids rebuilding temples");
  selectLifecycle(2); client.state = G2_STATE_CONNECTED;
  check(g2ConnectionStatusSnapshot(&status) && status.running && status.left.connected &&
        status.right.connected && status.left.tx == 123 &&
        std::strcmp(status.state, "connected") == 0,
        "completed publication exposes runtime even before init claim returns");
  gClientInitters = 0;
}
#endif

static void topologyAndCopies() {
  selectLifecycle(2); client.state = G2_STATE_CONNECTED;
  for (int links = 0; links < 4; ++links) {
    gL.connected = (links & 1) != 0; gR.connected = (links & 2) != 0;
    gL.connectionGeneration = 17; gR.connectionGeneration = UINT32_MAX;
    for (int ready = 0; ready < 4; ++ready) {
      gL.sessionReadyGeneration = ready & 1 ? 17 : 16;
      gR.sessionReadyGeneration = ready & 2 ? UINT32_MAX : 0;
      const G2LinkSnapshot linksRead = g2LinkSnapshot();
      check(linksRead.left == gL.connected && linksRead.right == gR.connected &&
            linksRead.leftReady == (gL.connected && (ready & 1)) &&
            linksRead.rightReady == (gR.connected && (ready & 2)) &&
            linksRead.leftGeneration == 17 && linksRead.rightGeneration == UINT32_MAX,
            "real topology snapshot preserves readiness and generation semantics");
      G2ConnectionStatus status;
      check(g2ConnectionStatusSnapshot(&status) &&
            status.left.connected == gL.connected && status.right.connected == gR.connected,
            "public status shows physical connection independent of promotion readiness");
    }
  }
  gL.connected = gR.connected = true; gL.packetsSent = 42;
  G2ConnectionStatus first;
  check(g2ConnectionStatusSnapshot(&first), "first caller captures status");
  char firstText[256]; formatG2Status(first, firstText, sizeof(firstText));
  gL.connected = false; gL.packetsSent = 999; client.initialized = false;
  G2ConnectionStatus second;
  check(g2ConnectionStatusSnapshot(&second) && !second.running && !second.left.connected,
        "later caller captures changed owner values");
  check(first.running && first.left.connected && first.left.tx == 42 &&
        std::strcmp(first.state, "connected") == 0,
        "caller-owned scalars and static state literal survive owner mutation");
  char retainedText[256]; formatG2Status(first, retainedText, sizeof(retainedText));
  check(std::strcmp(firstText, retainedText) == 0,
        "captured status text survives later owner mutations");
#if TEST_REPAIR
  // The repair text is copied into the caller's status, not pointed at.
  client.initialized = true;
  gL.connected = true; gR.connected = false;
  selectRepair(2);
  G2ConnectionStatus half;
  check(g2ConnectionStatusSnapshot(&half) && std::strcmp(half.repair, "slow") == 0, "slow repair captured");
  char halfText[256]; formatG2Status(half, halfText, sizeof(halfText));
  selectRepair(1);
  char laterText[256]; formatG2Status(half, laterText, sizeof(laterText));
  check(std::strcmp(halfText, laterText) == 0 && std::strstr(halfText, " repair=slow") != nullptr,
        "captured repair state survives a later schedule change");
  check(g2ConnectionStatusSnapshot(&half) && std::strcmp(half.repair, "3/7") == 0, "fresh capture sees it");
  selectRepair(0);
#endif
  G2ConnectionStatus empty;
#if TEST_REPAIR
  check(std::strcmp(empty.repair, "none") == 0, "repair defaults to none");
#endif
  check(!empty.running && std::strcmp(empty.state, "off") == 0 &&
        !empty.left.connected && !empty.right.connected &&
        empty.left.mtu == 0 && empty.right.mtu == 0 &&
        empty.left.battery == -1 && empty.right.battery == -1 &&
        empty.left.tx == 0 && empty.right.tx == 0 && empty.left.rx == 0 && empty.right.rx == 0,
        "public status defaults are complete and conservative");
}
#endif

int main() {
  characterize();
#if TEST_TYPED
  topologyAndCopies();
#if TEST_RUNTIME
  startupPublication();
#endif
#endif
  check(entries == exits && depth == 0, "all topology reads release guard");
  std::cerr << "G2 connection status (baseline=" << TEST_BASELINE << "): "
            << assertions << " assertions passed\n";
}
