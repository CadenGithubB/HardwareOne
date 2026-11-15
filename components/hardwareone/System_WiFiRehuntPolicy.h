#ifndef HARDWAREONE_SYSTEM_WIFI_REHUNT_POLICY_H
#define HARDWAREONE_SYSTEM_WIFI_REHUNT_POLICY_H

#include <cstdint>

// When a lost Wi-Fi connection is followed by a hunt across every saved
// network. The driver's own auto-reconnect retries only the network that
// dropped, so a device carried from one saved network to another stayed
// offline until a reboot or `openwifi`.
//
// Dependency-free (host test: test_wifi_rehunt_policy.cpp). System_WiFi.cpp
// feeds it from the main loop and runs the hunt on a short-lived task.
//
// Rules:
//  - Only an involuntary drop of an established connection starts it. A
//    disconnect the device asked for (wifidisconnect, closewifi, a switch to
//    another network) and a boot that never connected do not.
//  - The driver gets kFirstDelayMs to rejoin the dropped network by itself.
//  - With one saved network there is nothing else to hunt for, so the driver's
//    retry is left alone. Once a hunt has failed the driver no longer retries
//    (the hunt's last attempt ends in a commanded disconnect), so from then on
//    the hunt is the only retry and runs with a single saved network too.
//  - Failed hunts back off: kGapMs[0], kGapMs[1], then kGapMs[2] for good.
//  - It ends when the link is back, or when reconnecting is no longer wanted
//    (wifienabled 0, wifiautoreconnect 0, closewifi, radiopower off,
//    wifidisconnect).
struct WifiRehuntPolicy {
  static constexpr uint32_t kFirstDelayMs = 60000;
  static constexpr uint32_t kGapMs[3] = {60000, 120000, 300000};
  // One hunt's budget. Shorter than a commanded connect: an absent network is
  // given up after one scan, and the next hunt is at most kGapMs[2] away.
  static constexpr uint32_t kHuntBudgetMs = 90000;

  struct Inputs {
    bool connected = false;  // associated with an address
    bool armed = false;      // Wi-Fi enabled and auto-reconnect on
    bool held = false;       // radio not ours right now (boot connect, a command connecting, companion busy)
    int  savedCount = 0;
  };

  bool     wanted = false;      // a connection was lost and has not come back
  bool     driverIdle = false;  // a hunt failed since then: the driver is not retrying
  bool     hunting = false;     // the hunt task is running or about to
  uint8_t  failures = 0;        // failed hunts since the drop, saturating
  uint32_t sinceMs = 0;         // start of the current wait

  void clear() {
    wanted = false;
    driverIdle = false;
    failures = 0;
    sinceMs = 0;
    // `hunting` is cleared only by huntEnded: the task still runs.
  }

  // An established connection was lost and the device did not ask for it.
  void noteDropped(uint32_t nowMs) {
    if (wanted) return;  // already waiting; a brief rejoin does not restart the clock
    wanted = true;
    driverIdle = false;
    failures = 0;
    sinceMs = nowMs;
  }

  uint32_t waitMs() const {
    if (failures == 0) return kFirstDelayMs;
    const uint8_t i = failures > 3 ? 2 : uint8_t(failures - 1);
    return kGapMs[i];
  }

  // True when the caller must start a hunt now. The caller then either starts
  // the task or calls huntEnded(false, false, now) if it could not.
  bool poll(const Inputs& in, uint32_t nowMs) {
    if (!wanted || hunting) return false;
    if (in.connected || !in.armed) {
      clear();
      return false;
    }
    if (in.held) return false;
    if (in.savedCount < (driverIdle ? 1 : 2)) return false;
    if (uint32_t(nowMs - sinceMs) < waitMs()) return false;
    hunting = true;
    return true;
  }

  // The hunt returned. `superseded`: a Wi-Fi command took the radio over; it
  // decides the outcome, and the wait starts again without counting a failure.
  void huntEnded(bool connected, bool superseded, uint32_t nowMs) {
    hunting = false;
    if (!wanted) return;  // cancelled while the hunt ran
    if (connected) {
      clear();
      return;
    }
    driverIdle = true;
    sinceMs = nowMs;
    if (!superseded && failures < 255) ++failures;
  }
};

#endif  // HARDWAREONE_SYSTEM_WIFI_REHUNT_POLICY_H
