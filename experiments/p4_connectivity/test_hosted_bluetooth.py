#!/usr/bin/env python3
"""Exercise the actual isolated Arduino Hosted Bluetooth controller helper.

Run: python3 -m unittest discover -s experiments/p4_connectivity \
         -p 'test_hosted_bluetooth.py' -v

The test extracts the real controller branch and shared-transport ownership
functions, compiles them with deterministic RPC/HCI stubs, and executes failure
and lifecycle scenarios. It does not duplicate the helper implementation or use
hardware. Missing private source/compiler skips these tests. This qualifies
sequential lifecycle calls, not SDIO/HCI delivery or concurrent Wi-Fi/BLE changes.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ARDUINO = Path(__file__).resolve().parent / "private/app/components/arduino"
CORE = ARDUINO / "cores/esp32"


def extract_function(source, name):
    match = re.search(rf"^(?:bool|void) {name}\([^\n]*\)\s*\{{", source, re.M)
    if not match:
        raise AssertionError(f"Actual implementation missing {name}")
    opening = source.index("{", match.start())
    depth = 1
    cursor = opening + 1
    while depth and cursor < len(source):
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    if depth:
        raise AssertionError(f"Unterminated function {name}")
    return source[match.start():cursor]


STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=0x103 };
enum { ESP_BLUEDROID_STATUS_UNINITIALIZED=0,
       ESP_BLUEDROID_STATUS_INITIALIZED=1, ESP_BLUEDROID_STATUS_ENABLED=2 };
typedef struct {
  void (*send)(uint8_t *, uint16_t);
  bool (*check_send_available)(void);
  esp_err_t (*register_host_callback)(const void *);
} esp_bluedroid_hci_driver_operations_t;

static int hostState, initRc, enableRc, attachRc, disableRc, deinitRc, detachRc;
static bool remoteInitialized, remoteEnabled, enableReplyLost;
static bool hosted_initialized, hosted_ble_active, hosted_wifi_active;
static int transportShutdowns;
static char calls[256];
static void record(char c) {
  size_t n = strlen(calls);
  assert(n + 1 < sizeof(calls));
  calls[n] = c; calls[n+1] = 0;
}
static int esp_bluedroid_get_status(void) { return hostState; }
static int esp_hosted_bt_controller_init(void) {
  assert(hosted_initialized && hosted_ble_active);
  record('i');
  if (initRc == ESP_OK) remoteInitialized = true;
  return initRc;
}
static int esp_hosted_bt_controller_enable(void) {
  assert(remoteInitialized && hosted_ble_active);
  record('e');
  if (enableRc == ESP_OK || enableReplyLost) remoteEnabled = true;
  return enableRc;
}
static int esp_hosted_bt_controller_disable(void) {
  assert(hostState == ESP_BLUEDROID_STATUS_UNINITIALIZED);
  record('d');
  if (disableRc == ESP_OK) remoteEnabled = false;
  return disableRc;
}
static int esp_hosted_bt_controller_deinit(bool release) {
  assert(!release);  // Controller memory must remain reusable.
  assert(!remoteEnabled);
  record('x');
  if (deinitRc == ESP_OK) remoteInitialized = false;
  return deinitRc;
}
static void hosted_hci_bluedroid_open(void) {
  assert(remoteEnabled && hosted_ble_active); record('o');
}
static void hosted_hci_bluedroid_close(void) { record('c'); }
static void hosted_hci_bluedroid_send(uint8_t *data, uint16_t len) {
  (void)data; (void)len;
}
static bool hosted_hci_bluedroid_check_send_available(void) { return true; }
static esp_err_t hosted_hci_bluedroid_register_host_callback(const void *cb) {
  (void)cb; return ESP_OK;
}
static esp_err_t esp_bluedroid_attach_hci_driver(
    const esp_bluedroid_hci_driver_operations_t *ops) {
  assert(ops->send == hosted_hci_bluedroid_send);
  assert(ops->check_send_available == hosted_hci_bluedroid_check_send_available);
  assert(ops->register_host_callback == hosted_hci_bluedroid_register_host_callback);
  record('a'); return attachRc;
}
static esp_err_t esp_bluedroid_detach_hci_driver(void) {
  record('t'); return detachRc;
}
static bool hostedDeinit(void) {
  // An active BLE owner must prevent Wi-Fi from destroying shared SDIO.
  assert(!hosted_ble_active);
  transportShutdowns++;
  hosted_initialized = false;
  return true;
}
#define log_e(...) ((void)0)
#define log_i(...) ((void)0)
"""


SCENARIOS = r"""
static void reset_fixture(void) {
  hostState = initRc = enableRc = attachRc = disableRc = deinitRc = detachRc = 0;
  remoteInitialized = remoteEnabled = enableReplyLost = false;
  hosted_initialized = hosted_wifi_active = true;
  hosted_ble_active = false;
  transportShutdowns = 0; calls[0] = 0;
  sHostedControllerState = BT_HOSTED_CONTROLLER_UNKNOWN;
  sHostedHciAttached = false;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  reset_fixture();
  if (strcmp(argv[1], "startup") == 0) {
    assert(btHostedControllerStatus() == -1 && !btStarted());
    assert(!btStartMode(BT_MODE_CLASSIC_BT) && !btStartMode(BT_MODE_BTDM));
    assert(!calls[0] && !hosted_ble_active);
    hosted_initialized = false;
    assert(!btStart() && !calls[0] && !hosted_ble_active);
    hosted_initialized = true;
    assert(btStart() && btStarted() && hosted_ble_active);
    assert(strcmp(calls, "ieoa") == 0);
    assert(btStart()); assert(strcmp(calls, "ieoa") == 0);
  } else if (strcmp(argv[1], "reopen") == 0) {
    assert(btStart()); assert(btStop());
    assert(strcmp(calls, "ieoactdx") == 0);
    assert(btHostedControllerStatus() == 0 && !btStarted());
    assert(!hosted_ble_active && hosted_initialized && transportShutdowns == 0);
    assert(btStart()); assert(btStarted()); assert(btStop());
    assert(!remoteInitialized && !remoteEnabled && transportShutdowns == 0);
  } else if (strcmp(argv[1], "host_alive") == 0) {
    assert(btStart());
    for (int state=1; state<=2; state++) {
      hostState = state;
      assert(!btStop());
      assert(btHostedControllerDeinit() == ESP_ERR_INVALID_STATE);
      assert(strcmp(calls, "ieoa") == 0);
      assert(hosted_ble_active && remoteEnabled && btHostedControllerStatus() == 2);
    }
    hostState = 0; assert(btStop());
  } else if (strcmp(argv[1], "start_failures") == 0) {
    initRc = -9;
    assert(!btStart() && btHostedControllerStatus() == -1);
    assert(strcmp(calls, "i") == 0 && hosted_ble_active);
    assert(btStop() && !hosted_ble_active);
    reset_fixture(); enableRc = -9;
    assert(!btStart() && btHostedControllerStatus() == -1);
    assert(strcmp(calls, "ie") == 0 && hosted_ble_active);
    assert(btStop() && !hosted_ble_active);
    reset_fixture(); attachRc = -9;
    assert(!btStart()); assert(strcmp(calls, "ieoac") == 0);
    assert(btHostedControllerStatus() == 2 && hosted_ble_active);
    assert(btStop() && !hosted_ble_active);
  } else if (strcmp(argv[1], "ambiguous_rpc") == 0) {
    // The remote enable succeeded, but its response failed to arrive.
    enableRc = -9; enableReplyLost = true;
    assert(!btStart() && remoteEnabled);
    assert(btHostedControllerStatus() == -1 && hosted_ble_active);
    assert(hostedDeinitWiFi());
    assert(hosted_initialized && transportShutdowns == 0);
    assert(btStop());
    assert(btHostedControllerStatus() == 0 && !remoteEnabled && !hosted_ble_active);
    enableRc = 0; enableReplyLost = false;
    assert(btStart()); assert(btStop());
  } else if (strcmp(argv[1], "stop_failures") == 0) {
    assert(btStart()); disableRc = -7;
    assert(!btStop()); assert(btHostedControllerStatus() == -1);
    assert(strcmp(calls, "ieoactd") == 0 && hosted_ble_active && remoteEnabled);
    assert(hostedDeinitWiFi() && transportShutdowns == 0);
    disableRc = 0; assert(btStop() && btHostedControllerStatus() == 0);
    reset_fixture(); assert(btStart()); deinitRc = -8;
    assert(!btStop() && btHostedControllerStatus() == -1 && hosted_ble_active);
    deinitRc = 0; assert(btStop() && !hosted_ble_active);
    reset_fixture(); assert(btStart()); detachRc = -6;
    assert(!btStop() && hosted_ble_active && remoteEnabled);
    assert(strcmp(calls, "ieoact") == 0);
    detachRc = 0; assert(btStop());
  } else if (strcmp(argv[1], "ownership") == 0) {
    assert(btStart());
    assert(hostedDeinitWiFi());
    assert(!hosted_wifi_active && hosted_ble_active && hosted_initialized);
    assert(remoteEnabled && btStarted() && transportShutdowns == 0);
    assert(btStop());
    assert(!hosted_ble_active && hosted_initialized && transportShutdowns == 0);
    assert(btStart() && btStop());  // Reopen with Wi-Fi still off.
    assert(hostedDeinitWiFi());
    assert(!hosted_initialized && transportShutdowns == 1);
    assert(!btStart());  // Board/transport must be prepared again.
  } else if (strcmp(argv[1], "unknown_stop") == 0) {
    assert(btHostedControllerStatus() == -1);
    assert(btHostedControllerDisable() == ESP_OK);
    assert(btHostedControllerStatus() == -1); // disable alone cannot prove init
    assert(btHostedControllerDeinit() == ESP_OK);
    assert(btHostedControllerStatus() == 0);
    assert(hosted_initialized && transportShutdowns == 0);
  } else {
    assert(!"unknown test scenario");
  }
  puts("PASS");
  return 0;
}
"""


class HostedBluetoothControllerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        paths = [CORE / name for name in
                 ("esp32-hal-bt.c", "esp32-hal-bt.h", "esp32-hal-hosted.c")]
        if not all(path.is_file() for path in paths):
            raise unittest.SkipTest("Recreate the isolated connectivity app to test its actual helper")
        compiler = next((shutil.which(c) for c in ("clang", "cc", "gcc") if shutil.which(c)), None)
        if not compiler:
            raise unittest.SkipTest("A host C compiler is required")
        controller, header, hosted = [path.read_text() for path in paths]
        branch = controller.split("#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID\n", 1)[1]
        branch = branch.split("#elif SOC_BT_SUPPORTED", 1)[0]
        branch = re.sub(r"^#include .*\n", "", branch, flags=re.M)
        mode = re.search(r"typedef enum \{[^}]*\} bt_mode;", header, re.S).group()
        states = re.search(r"enum \{\s*BT_HOSTED_CONTROLLER_UNKNOWN[^}]*\};", header, re.S).group()
        ownership = "\n".join(extract_function(hosted, name) for name in
                              ("hostedClaimBLE", "hostedReleaseBLE", "hostedDeinitWiFi"))
        cls.work = tempfile.TemporaryDirectory(prefix="hw1-hosted-bt-test-")
        cls.addClassCleanup(cls.work.cleanup)
        source = Path(cls.work.name) / "controller_test.c"
        cls.binary = Path(cls.work.name) / "controller_test"
        source.write_text(STUBS + mode + states + "\nbool btStartMode(bt_mode mode);\n" +
                          ownership + branch + SCENARIOS)
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(source), "-o", str(cls.binary)],
                               text=True, capture_output=True, timeout=30)
        if built.returncode:
            raise AssertionError(f"Actual helper failed to compile:\n{built.stdout}{built.stderr}")

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, f"{case}: {result.stdout}{result.stderr}")

    def test_checked_startup_and_transport_precondition(self):
        self.run_case("startup")

    def test_shutdown_and_reopen_preserve_shared_transport(self):
        self.run_case("reopen")

    def test_active_host_blocks_controller_teardown(self):
        self.run_case("host_alive")

    def test_init_enable_and_hci_attach_failures(self):
        self.run_case("start_failures")

    def test_lost_rpc_response_retains_ownership_until_checked_stop(self):
        self.run_case("ambiguous_rpc")

    def test_disable_deinit_and_hci_detach_failures(self):
        self.run_case("stop_failures")

    def test_wifi_shutdown_cannot_remove_active_ble_transport(self):
        self.run_case("ownership")

    def test_unknown_controller_state_is_not_assumed_idle(self):
        self.run_case("unknown_stop")


if __name__ == "__main__":
    unittest.main()
