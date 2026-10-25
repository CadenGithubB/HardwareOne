#include "BLE_CentralStack.h"

#if ENABLE_BLUETOOTH

#include <Arduino.h>
#include <BLEDevice.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BLE_CentralCoordinator.h"
#include "BLE_CentralTx.h"
#include "BLE_CoexRecords.h"
#include "BLE_Peers.h"
#include "BLE_PhonePipe.h"
#include "BLE_SetupWorker.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "G2_Ring.h"
#include "HAL_Bluetooth.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_MemoryMonitor.h"
#include "System_RadioCompanion.h"
#include "System_Settings.h"

#if ENABLE_G2_GLASSES
// Defined in G2_Glasses.cpp; declared here rather than in G2_Glasses.h so the
// GAP types stay out of that widely included header.
void g2GapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif

namespace {

// Published by bleStackUp()/bleStackDown() only. The admission gates read it
// on every reconnect tick, so it is a flag, never a HAL query.
std::atomic<bool> sReady{false};
BLEScan* sScan = nullptr;
const char* sRefusal = "";

// The local name of the client-role host. The client role never advertises
// (the console service on the client host is reachable over the phone link
// only), so the name is informational; the server role names itself from the
// setting when `openconsole` starts it.
constexpr const char* kClientHostName = "HardwareOne";

// The process-global local ATT MTU of the client role. The glasses negotiate
// against it (G2_BLE_LOCAL_MTU_PREF); the ring and the phone take whatever the
// peer settles on. Server mode sets 517 on its own host.
constexpr uint16_t kClientLocalMtu = 244;
#if ENABLE_G2_GLASSES
static_assert(kClientLocalMtu == G2_BLE_LOCAL_MTU_PREF,
              "the client-role MTU is the glasses' preferred local MTU");
#endif

struct RoleClaim {
  explicit RoleClaim(BleRoleTransition state) : held(bleRoleTransitionBegin(state)) {}
  ~RoleClaim() { if (held) bleRoleTransitionEnd(); }
  explicit operator bool() const { return held; }
  bool held;
};

bool refuse(const char* why) {
  sRefusal = why ? why : "";
  return false;
}

// One verified terminal host/controller teardown, shared by the normalize
// step of bleStackUp() (a stale generation left by a faulted init or a
// recovery) and by bleStackDown(). This is the sequence the stack recoveries
// already prove on hardware: ring owner quiesce first (its GATT pointers die
// with the host), GAP handler off, the checked HAL teardown, the retired-client
// acknowledgement, then the phone pipe's host-stopped handshake. Any failure
// sets the lifecycle fault: nothing is re-initialised and nothing is released
// until a later verified teardown clears it (a retried `closeble`, a recovery)
// or a reboot, which is the documented last resort.
bool hostTeardown() {
  if (!g2RingPrepareForStackTeardown(2000)) {
    return refuse("the ring owner did not quiesce; retry shortly");
  }
  BLEDevice::setCustomGapHandler(nullptr);
  const BluetoothHalShutdownResult result = bluetoothHalDeinit();
  vTaskDelay(pdMS_TO_TICKS(200));
  if (!result.success) {
    bleStackSetLifecycleFault(true);
    BROADCAST_PRINTF("[BLE] Host teardown incomplete (phase=%u host=%ld->%ld "
                     "controller=%ld->%ld quarantined=%d) - reboot required",
                     (unsigned)result.phase,
                     (long)result.hostStatusBefore, (long)result.hostStatusAfter,
                     (long)result.controllerStatusBefore, (long)result.controllerStatusAfter,
                     result.clientQuarantined ? 1 : 0);
    return refuse("host teardown incomplete; reboot required");
  }
  // Terminal teardown is the lifetime barrier for every retired BLEClient.
  // The G2 acknowledgement also releases the coordinator's reservations; the
  // explicit call covers builds without the G2 runtime.
  bleCentralClientsTerminalTeardownAcknowledged();
  bleCentralLinksHostStopped();
  if (!bluetoothHalStatus().stopped()) {
    bleStackSetLifecycleFault(true);
    broadcastOutput("[BLE] Host did not reach a stopped state - reboot required");
    return refuse("host did not stop; reboot required");
  }
  if (!phonePipeHostStopped()) {
    bleStackSetLifecycleFault(true);
    broadcastOutput("[BLE] Phone pipe did not acknowledge the stopped host - reboot required");
    return refuse("the phone pipe did not acknowledge the stopped host; reboot required");
  }
  return true;
}

// Rolls back a host that came up only partially: the HAL may have enabled the
// controller/host before failing to publish the Arduino initialized flag.
void rollbackPartialHost(const char* what) {
  BLEDevice::setCustomGapHandler(nullptr);
  const BluetoothHalShutdownResult rollback = bluetoothHalDeinit();
  if (rollback.success) {
    bleCentralClientsTerminalTeardownAcknowledged();
    bleCentralLinksHostStopped();
    (void)phonePipeHostStopped();
  } else {
    bleStackSetLifecycleFault(true);
  }
  BROADCAST_PRINTF("[BLE] %s failed (%s)", what,
                   rollback.success ? "rolled back" : "teardown incomplete - reboot required");
}

// Every device owner's peer spec is registered before the auto-reconnect arm,
// so a boot or `openble` with only one device enabled opens only that one.
// Registration is idempotent: a repeat replaces the spec in place.
void registerDevicePeers() {
#if ENABLE_G2_GLASSES
  g2RegisterBlePeer();
#endif
#if ENABLE_G2_GLASSES && ENABLE_R1_HEALTH
  g2RingRegisterPeer();
#endif
#if ENABLE_BLE_PHONE_PIPE
  phonePipeRegisterPeer();
#endif
}

}  // namespace

bool bleStackUp() {
  RoleClaim role(BleRoleTransition::STACK_UP);
  if (!role) return refuse("another BLE role transition is active");
  if (sReady.load(std::memory_order_acquire) && bluetoothHalStatus().ready() &&
      !isBleServerInitialized()) {
    return true;
  }
  if (!gSettings.bleEnabled) return refuse("Bluetooth is disabled - run 'bleenabled 1' first");
  if (bleStackLifecycleFaulted() &&
      bleRoleTransitionState() != BleRoleTransition::RECOVERING) {
    return refuse("a prior Bluetooth teardown was incomplete; reboot required");
  }
  {
    const char* companionReason = nullptr;
    if (radioCompanionBlocksRadio(&companionReason)) {
      return refuse(companionReason ? companionReason : "the radio companion blocks the radio");
    }
  }
  // The console server uses the same controller and never coexists with
  // central scanning. Roles switch explicitly: `closeconsole` first.
  if (isBleServerInitialized()) {
    return refuse("the app console server owns the Bluetooth host; run 'closeconsole' first");
  }

  DEBUG_G2F("[BLE] Stack memory before start: free=%u largest=%u",
            (unsigned)hw1InternalFreeBytes(), (unsigned)hw1InternalLargestBlock());
  bleCentralTxInit();
  // A disable or role change may already have completed the terminal teardown
  // a quarantined client asked for; do not carry its admission block into a
  // clean generation.
  if (bleCentralClientsRetirementPending() && bluetoothHalStatus().stopped()) {
    bleCentralClientsTerminalTeardownAcknowledged();
  }
  if (!bluetoothHalStatus().hostAbsent()) {
    // A stale generation: a faulted init, a recovery that stopped halfway, or
    // a host the companion reset left behind. Normalize with the checked
    // host-first teardown; a controller-only bounce would keep exactly the
    // stale TCB / BLE_CONN_CANCEL state the recovery work exists to remove.
    if (g2RingConnectInFlight()) {
      return refuse("a ring connection is still active; retry shortly");
    }
    // Admission closes before the stale host goes, and no stale scan pointer
    // survives a failed teardown.
    sReady.store(false, std::memory_order_release);
    sScan = nullptr;
    if (!hostTeardown()) return false;   // its handshake includes the phone pipe
  }
  // No separate phone-pipe handshake here: an absent host was either never
  // started (nothing for the pipe to acknowledge) or acknowledged when
  // bleStackDown() took it down, and on Hosted a never-started controller is
  // absent but not "stopped", so the pipe's stopped-only check would refuse
  // the very first start.
  // The stack's own budget (host, controller link, setup worker). The device
  // owners check theirs after the stack is up; see the "blestack" / "g2client"
  // rows in System_MemoryMonitor.cpp.
  {
    String reason;
    if (!checkMemoryAvailable("blestack", &reason)) {
      BROADCAST_PRINTF("[BLE] Stack start refused: %s", reason.c_str());
      broadcastOutput("[BLE] Free internal DRAM first ('closehttp' / 'closewifi') or reboot.");
      return refuse("not enough internal DRAM for the Bluetooth host");
    }
  }
  if (!bluetoothHalInit(kClientHostName)) {
    rollbackPartialHost("BLE host initialization");
    return refuse("BLE host initialization failed");
  }
  DEBUG_G2F("[BLE] Stack memory after host init: free=%u largest=%u",
            (unsigned)hw1InternalFreeBytes(), (unsigned)hw1InternalLargestBlock());
  // Respect the shared setting where the backend can apply it. Hosted keeps
  // its companion default and explicitly lacks the capability.
  if (bluetoothHalSupportsTxPower()) {
    const esp_err_t powerErr = bluetoothHalSetTxPower(
        (uint8_t)constrain(gSettings.bleTxPower, 0, 7));
    if (powerErr != ESP_OK) {
      BROADCAST_PRINTF("[BLE] TX power setting failed: %s", esp_err_to_name(powerErr));
    }
  } else {
    broadcastOutput("[BLE] Radio backend controls BLE TX power; using companion default");
  }
  BLEDevice::setMTU(kClientLocalMtu);
#if ENABLE_G2_GLASSES
  // Arduino's BLEDevice owns the single esp_ble_gap_register_callback slot and
  // does not handle UPDATE_CONN_PARAMS itself, so the applied connection
  // interval is only observable through this hook.
  BLEDevice::setCustomGapHandler(g2GapEventHandler);
#endif
#if ENABLE_BLE_COEX_RECORDS
  bleCoexInstallGattcObserver();  // passive: catches the HCI reason of failed connects
#endif
  sScan = BLEDevice::getScan();
  if (!sScan) {
    rollbackPartialHost("BLE scan object");
    return refuse("BLE scan object unavailable");
  }
  // The glasses' scan profile is the shared default (active, 100 ms interval,
  // 99 ms window); a ring-only or phone-only open inherits it, and the phone's
  // scan sets its own passive 60/60 profile per scan.
  sScan->setActiveScan(true);
  sScan->setInterval(100);
  sScan->setWindow(99);
#if ENABLE_BLE_PHONE_PIPE
  // The app console rides the phone pipe's link. Its command service is
  // registered on the fresh host before any connect and never later: a
  // service started while links are up sends Service Changed to every peer.
  // Its owner logs a failure; the devices never depend on it.
  (void)bleConsoleRegisterOnClientHost();
#endif
  if (!bleSetupWorkerStart()) {
    sScan = nullptr;
    rollbackPartialHost("BLE setup worker start");
    return refuse("BLE setup worker unavailable (memory)");
  }
  sReady.store(true, std::memory_order_release);
  registerDevicePeers();
  // Peers with auto-reconnect on come back with the stack, also after a role
  // switch or a boot in another mode left them unarmed.
  bleArmAutoReconnectPeers();
  DEBUG_G2F("[BLE] Stack ready: free=%u largest=%u",
            (unsigned)hw1InternalFreeBytes(), (unsigned)hw1InternalLargestBlock());
  sRefusal = "";
  return true;
}

bool bleStackDown() {
  RoleClaim role(BleRoleTransition::STACK_DOWN);
  if (!role) return refuse("another BLE role transition is active");
  if (isBleServerInitialized()) {
    return refuse("the app console server is running; 'closeconsole' stops it");
  }
  if (isG2ClientInitialized()) return refuse("the glasses are open; run 'closeg2' first");
  if (g2RingIsOpen()) return refuse("the ring is open; run 'closering' first");
  if (phonePipeActive()) return refuse("the phone pipe is open; run 'closephone' first");

  // Admission closes before the host goes. A reconnect tick that races this
  // sees the flag down and returns ROLE_BLOCKED; nothing re-arms the stack
  // until the next bleStackUp().
  sReady.store(false, std::memory_order_release);
  if (!bluetoothHalStatus().hostAbsent()) {
    if (!hostTeardown()) return false;
  } else {
    // Never started, or already torn down by a recovery: nothing to stop, but
    // a phone worker that outlived its host still runs its reset pass.
    (void)phonePipeHostStopped();
  }
  sScan = nullptr;
  // The phone worker parks across `closephone` (its reset pass needs the
  // stopped host); now that the host is down it may leave. Busy: kept, reused.
  if (!phonePipeWorkerRelease(1500)) {
    broadcastOutput("[BLE] Phone worker stayed parked (busy); the next open reuses it");
  }
  // The setup worker is shared central infrastructure and the last thing to
  // go: every connect body and phone turn has ended by now. A busy worker is
  // kept (fail closed) and reused by the next bleStackUp().
  if (!bleSetupWorkerRelease(2000)) {
    broadcastOutput("[BLE] Setup worker stayed parked (busy); the next open reuses it");
  }
  bleCentralRecoveryBlock(false);
  bleStackSetLifecycleFault(false);
  // Deleted tasks are reclaimed by the idle task; give it a tick so a memreport
  // issued right after `closeble` sees the stacks returned.
  vTaskDelay(pdMS_TO_TICKS(20));
  sRefusal = "";
  return true;
}

bool bleStackReady() {
  return sReady.load(std::memory_order_acquire);
}

BLEScan* bleStackScan() {
  return sReady.load(std::memory_order_acquire) ? sScan : nullptr;
}

const char* bleStackLastRefusal() {
  return sRefusal;
}

void bleStackNoteHostLost() {
  sReady.store(false, std::memory_order_release);
  sScan = nullptr;
}

#endif  // ENABLE_BLUETOOTH
