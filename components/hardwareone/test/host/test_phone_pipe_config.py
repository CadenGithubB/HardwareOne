#!/usr/bin/env python3
"""Preprocess the production feature rules; complements real profile builds."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class PhoneConfigTest(unittest.TestCase):
    def resolve(self, choices, sdk=None):
        with tempfile.TemporaryDirectory(prefix='hw1-phone-config-') as tmp:
            folder = Path(tmp)
            # Unrelated UART consumers need board wiring. This test exercises
            # the shared capability rules without claiming any board geometry.
            flags = dict(ENABLE_LLM_SOURCE_CM5=0, ENABLE_RASPBERRY_PI_HOST_POWER=0,
                         ENABLE_RASPBERRY_PI_HOST_FAN=0)
            flags.update(choices)
            feature = folder / 'features.h'
            feature.write_text(''.join(f'#undef {k}\n#define {k} {v}\n' for k, v in flags.items()))
            if sdk is not None:
                (folder / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET "synthetic"\n' +
                    ''.join(f'#define {k} {v}\n' for k, v in sdk.items()))
            result = subprocess.run([os.environ.get('CXX', 'clang++'), '-x', 'c++', '-E', '-dM',
                '-I', str(folder), f'-DHW1_FEATURE_CONFIG_HEADER="{feature}"',
                str(ROOT / 'System_BuildConfig.h')], capture_output=True, text=True)
            macros = dict(line.split(maxsplit=2)[1:] for line in result.stdout.splitlines()
                          if line.startswith('#define ') and len(line.split(maxsplit=2)) == 3)
            return result, macros

    def test_default_disabled(self):
        result, macros = self.resolve({})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '0')

    def test_phone_without_g2(self):
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_G2_GLASSES=0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '1')
        self.assertEqual(macros['ENABLE_G2_GLASSES'], '0')

    def test_requires_bluetooth_application(self):
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_BLUETOOTH=0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '0')

    def test_requires_sdk_bluetooth(self):
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1), {})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '0')

    def test_three_link_topology_reserves_ring_slot(self):
        sdk = dict(CONFIG_BT_ENABLED=1, CONFIG_BT_BLUEDROID_ENABLED=1, CONFIG_BT_ACL_CONNECTIONS=3)
        result, _ = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_G2_GLASSES=1), sdk)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('G2 plus phone requires four BLE ACL slots', result.stderr)
        sdk['CONFIG_BT_ACL_CONNECTIONS'] = 4
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_G2_GLASSES=1), sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '1')

    def test_p4_pipe_needs_psram_bss(self):
        # The ~29 KB Session is EXT_RAM_BSS_ATTR; a P4 pipe build without PSRAM
        # .bss must stop instead of landing it in internal RAM.
        sdk = dict(CONFIG_BT_ENABLED=1, CONFIG_BT_BLUEDROID_ENABLED=1, CONFIG_BT_ACL_CONNECTIONS=4,
                   CONFIG_IDF_TARGET_ESP32P4=1)
        pipe = dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_G2_GLASSES=0)
        for psram in (None, 0):
            if psram is not None:
                sdk['CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY'] = psram
            result, _ = self.resolve(pipe, sdk)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y', result.stderr)
        sdk['CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY'] = 1
        result, macros = self.resolve(pipe, sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '1')

    def test_psram_rule_only_for_p4_pipe(self):
        sdk = dict(CONFIG_BT_ENABLED=1, CONFIG_BT_BLUEDROID_ENABLED=1, CONFIG_BT_ACL_CONNECTIONS=4)
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=1, ENABLE_G2_GLASSES=0), sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '1')
        sdk['CONFIG_IDF_TARGET_ESP32P4'] = 1
        result, macros = self.resolve(dict(ENABLE_BLE_PHONE_PIPE=0, ENABLE_G2_GLASSES=0), sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(macros['ENABLE_BLE_PHONE_PIPE'], '0')

if __name__ == '__main__':
    unittest.main()
