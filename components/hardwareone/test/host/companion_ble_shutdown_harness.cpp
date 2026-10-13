// Host harness for System_RadioCompanion.cpp shutDownBleForCompanionReset().
//
// The production function is spliced in at the marker below and runs against a
// fake BLE owner layer. deinitBluetooth() fails while the fake Ring owner has
// not quiesced (the server stays up with its host, as in Bluetooth.cpp), and
// succeeds once it has, taking the host down itself. bluetoothHalDeinit() while
// the Ring owner is still busy is the use-after-free the Ring prepare exists to
// prevent; the harness records it.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define ENABLE_G2_GLASSES 1
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

struct RadioUsers {
  bool espnow = false;
  bool wifi = false;
  bool bleServer = false;
  bool g2Client = false;
  bool radioPoweredOff = false;
};

enum class BleRoleTransition { NONE, SERVER_START, SERVER_STOP, G2_START, G2_STOP, RECOVERING };

struct BluetoothHalStatus {
  bool hostStopped;
  bool stopped() const { return hostStopped; }
};
struct BluetoothHalShutdownResult {
  bool success;
  unsigned phase;
};

namespace world {
bool serverUp = false;
bool g2Up = false;
bool halStopped = true;
bool roleBusy = false;
int roleBegins = 0, roleEnds = 0;
int ringBusyAttempts = 0;   // deinitBluetooth() calls that find the Ring owner busy; -1 = forever
int deinitCalls = 0, halDeinits = 0, forcedWhileRingBusy = 0;
bool fault = false;
uint32_t delayedMs = 0;
std::vector<std::string> logs;
bool ringBusy() { return ringBusyAttempts < 0 || deinitCalls < ringBusyAttempts; }
}  // namespace world

bool bleRoleTransitionBegin(BleRoleTransition) {
  if (world::roleBusy) return false;
  ++world::roleBegins;
  return true;
}
void bleRoleTransitionEnd() { ++world::roleEnds; }
BluetoothHalStatus bluetoothHalStatus() { return {world::halStopped}; }
bool isBleServerInitialized() { return world::serverUp; }
bool deinitBluetooth() {
  const bool busy = world::ringBusy();
  ++world::deinitCalls;
  if (busy) return false;  // session admission and advertising closed; server and host up
  world::serverUp = false;
  world::halStopped = true;  // the owner took the host down itself
  return true;
}
bool isG2ClientInitialized() { return world::g2Up; }
bool deinitG2Client() {
  world::g2Up = false;
  return true;
}
BluetoothHalShutdownResult bluetoothHalDeinit() {
  ++world::halDeinits;
  if (world::serverUp && world::ringBusy()) ++world::forcedWhileRingBusy;
  world::halStopped = true;
  return {true, 0};
}
void bleCentralClientsTerminalTeardownAcknowledged() {}
void bleStackSetLifecycleFault(bool faulted) { world::fault = faulted; }
void logSystemEvent(const char*, const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  world::logs.push_back(line);
}
void vTaskDelay(TickType_t ms) { world::delayedMs += ms; }

/*@SHUTDOWN@*/

static int failures = 0;
static const char* currentCase = "";
#define EXPECT(cond, what)                                                   \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s: %s  [%s]\n", currentCase, what, #cond); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static RadioUsers reset(const char* name, bool server, bool g2, bool halUp) {
  currentCase = name;
  world::serverUp = server;
  world::g2Up = g2;
  world::halStopped = !halUp;
  world::roleBusy = false;
  world::roleBegins = world::roleEnds = 0;
  world::ringBusyAttempts = 0;
  world::deinitCalls = world::halDeinits = world::forcedWhileRingBusy = 0;
  world::fault = false;
  world::delayedMs = 0;
  world::logs.clear();
  RadioUsers users;
  users.bleServer = server;
  users.g2Client = g2;
  return users;
}
static bool logged(const char* text) {
  for (const auto& line : world::logs)
    if (line.find(text) != std::string::npos) return true;
  return false;
}

int main() {
  // K1: the Ring owner never quiesces. The server stop is retried a bounded
  // number of times, the host is never forced down, and Bluetooth stays off
  // until a reboot (the lifecycle fault), with the reason logged.
  RadioUsers users = reset("K1 Ring owner never quiesces", true, false, true);
  world::ringBusyAttempts = -1;
  shutDownBleForCompanionReset(users);
  EXPECT(world::forcedWhileRingBusy == 0 && world::halDeinits == 0,
         "the host was forced down while the Ring owner had not quiesced");
  EXPECT(world::deinitCalls > 1 && world::deinitCalls <= 5,
         "the server stop was not retried, or the retries are unbounded");
  EXPECT(world::delayedMs <= 5000, "the retries wait too long before the companion reset");
  EXPECT(world::fault, "Bluetooth was not kept off after the server stop never completed");
  EXPECT(logged("did not stop"), "the skipped teardown was not logged");
  EXPECT(world::roleBegins == 1 && world::roleEnds == 1, "the RECOVERING claim was not released");

  // K2: the Ring owner quiesces during the retries: the owner's own stop takes
  // the host down, nothing is forced and no fault is raised.
  users = reset("K2 Ring owner quiesces on the second attempt", true, false, true);
  world::ringBusyAttempts = 1;
  shutDownBleForCompanionReset(users);
  EXPECT(world::deinitCalls == 2, "the server stop was not retried after the Ring owner deferred it");
  EXPECT(world::halDeinits == 0 && world::halStopped && !world::serverUp,
         "the host did not come down through the server's own stop");
  EXPECT(!world::fault && world::logs.empty(), "a completed stop raised a fault or logged a failure");
  EXPECT(world::roleEnds == 1, "the RECOVERING claim was not released");

  // K3: an idle Ring owner: one stop, nothing forced (unchanged).
  users = reset("K3 server stops at once", true, false, true);
  shutDownBleForCompanionReset(users);
  EXPECT(world::deinitCalls == 1 && world::halDeinits == 0 && !world::fault,
         "a server stop that completed at once was retried or followed by a forced teardown");

  // K4: no server, the host is up (a G2 client stopped or a bare host): the
  // checked HAL teardown still runs (unchanged).
  users = reset("K4 G2 client, host up", false, true, true);
  shutDownBleForCompanionReset(users);
  EXPECT(world::deinitCalls == 0 && world::halDeinits == 1 && world::halStopped && !world::fault,
         "the host teardown without a server changed");
  EXPECT(world::roleEnds == 1, "the RECOVERING claim was not released");

  // K5: another role transition holds the claim: nothing is touched (unchanged).
  users = reset("K5 role transition busy", true, false, true);
  world::roleBusy = true;
  shutDownBleForCompanionReset(users);
  EXPECT(world::deinitCalls == 0 && world::halDeinits == 0 && world::roleEnds == 0,
         "the shutdown ran without the RECOVERING claim");
  EXPECT(logged("role transition busy"), "the busy claim was not logged");

  // K6: nothing runs and the host is stopped: an early return (unchanged).
  users = reset("K6 nothing to stop", false, false, false);
  shutDownBleForCompanionReset(users);
  EXPECT(world::roleBegins == 0 && world::deinitCalls == 0 && world::halDeinits == 0,
         "an idle stack was touched");

  if (failures) {
    std::fprintf(stderr, "companion BLE shutdown: %d expectation(s) failed\n", failures);
    return 1;
  }
  std::puts("companion BLE shutdown: the server stops only through its owner (bounded retry), the host is "
            "never forced down under a busy Ring owner, and a stop that never completes keeps Bluetooth "
            "off until a reboot");
  return 0;
}
