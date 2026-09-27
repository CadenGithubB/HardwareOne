"""Offline validation of persistence evidence; no serial access."""

import copy
import unittest

from test_mesh import CheckFailed
from test_persistence import autostart_value, compare_saved_state, prove_reboot, public_identity


class PersistenceEvidenceTests(unittest.TestCase):
    def test_public_identity_excludes_private_material_and_requires_valid_key(self):
        document = {"schema": 1, "valid": True, "mac": "FC:01:2C:E0:B9:A8", "pub": "AB" * 32,
                    "createdAtSec": 0, "regenCount": 0}
        actual = public_identity(document, "fc:01:2c:e0:b9:a8")
        self.assertEqual(actual["pub"], "ab" * 32)
        self.assertEqual(set(actual), {"mac", "pub", "createdAtSec", "regenCount"})
        with self.assertRaises(CheckFailed):
            public_identity({**document, "pub": "invalid"}, document["mac"])

    def test_autostart_query_with_barrier_and_prompt(self):
        self.assertTrue(autostart_value("$ OK: espnowAutoStart = true\n$ You are user (admin)\n"))
        self.assertFalse(autostart_value("espnowAutoStart = false\n"))
        with self.assertRaises(CheckFailed):
            autostart_value("Configuration updated")

    def reboot_fixture(self):
        before = {"schema": 1, "boot_count_store": "nvs", "boot_count": 10, "crash_count": 0,
                  "reset_reason": "USB", "reset_reason_code": 11}
        after = {**before, "boot_count": 11}
        output = ("ESP-ROM:esp32p4-eco7-20260109\n"
                  "rst:0x17 (CHIP_USB_UART_RESET),boot:0x308 (SPI_FAST_FLASH_BOOT)\n"
                  "[3078] [EVENT][BOOT] boot #11 | reset=usb(11) | crashCount=0\n"
                  "[5791] [Boot] Setup complete\n")
        return before, after, output

    def test_usb_reset_requires_fresh_logs_and_increased_nvs_counter(self):
        before, after, output = self.reboot_fixture()
        evidence = prove_reboot(before, after, output, "esp32p4")
        self.assertEqual(evidence["bootCountDelta"], 1)
        for bad in ({**after, "boot_count": 10}, {**after, "crash_count": 1},
                    {**after, "reset_reason_code": 4}, {**after, "boot_count_store": "ram"}):
            with self.assertRaises(CheckFailed):
                prove_reboot(before, bad, output, "esp32p4")
        for missing in ("ESP-ROM:", "rst:0x", "[EVENT][BOOT]", "[Boot] Setup complete"):
            with self.assertRaises(CheckFailed):
                prove_reboot(before, after, output.replace(missing, "omitted"), "esp32p4")

    def test_configuration_registry_and_identity_changes_fail(self):
        before = {"identities": {"p4": {"pub": "a"}}, "configuration": {"p4": {"channel": 6}},
                  "registries": {"p4": {"mac": "peer"}}}
        self.assertTrue(compare_saved_state(before, copy.deepcopy(before))["publicIdentitiesUnchanged"])
        for key in before:
            changed = copy.deepcopy(before)
            changed[key] = {}
            with self.assertRaises(CheckFailed):
                compare_saved_state(before, changed)


if __name__ == "__main__":
    unittest.main()
