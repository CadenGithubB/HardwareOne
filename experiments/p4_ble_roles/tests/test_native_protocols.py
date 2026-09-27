"""Host checks of the prepared copy's real G2/R1 codecs and role token.

Run with `python3 -B -m unittest discover -s experiments/p4_ble_roles/tests -v`.
No device, BLE API, network, credential or firmware build is accessed. Generated
translation units and executables live in a temporary directory. Set
HW1_BLE_ROLES_APP to test another reconstructed copy. Missing sources/compiler
fail rather than silently skip. C++17, AddressSanitizer and UBSan are required.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest


EXPERIMENT = Path(__file__).resolve().parents[1]
COMPONENT = Path(os.environ.get('HW1_BLE_ROLES_APP', EXPERIMENT / 'private/app')) / 'components/hardwareone'
COMPILER = shutil.which(os.environ.get('CXX', 'c++'))

PLATFORM = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
uint32_t millis() { return 1000; }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
enum class AllocPref {PreferPSRAM, RequireInternal};
void* ps_alloc(size_t n, AllocPref, const char*) { return malloc(n); }
void heap_caps_free(void* p) { free(p); }
#define DEBUG_RING_SETUPF(...) do { printf(__VA_ARGS__); puts(""); } while (0)
'''

ROLE_PLATFORM = r'''
#include <stdint.h>
#include <cassert>
#include <mutex>
#include <thread>
#include <cstdio>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
using TaskHandle_t = void*;
thread_local int taskToken;
thread_local bool validTask = true;
TaskHandle_t xTaskGetCurrentTaskHandle() { return validTask ? &taskToken : nullptr; }
void portENTER_CRITICAL(portMUX_TYPE* mux) { mux->lock(); }
void portEXIT_CRITICAL(portMUX_TYPE* mux) { mux->unlock(); }
'''

ROLE_CHECKS = r'''
int main() {
  using R = BleRoleTransition;
  assert(!bleRoleTransitionBegin(R::IDLE));
  validTask = false;
  assert(!bleRoleTransitionBegin(R::SERVER_START));
  validTask = true;
  assert(!bleRoleTransitionHeldByCurrentTask());
  assert(bleRoleTransitionBegin(R::RECOVERING));
  assert(bleRoleTransitionHeldByCurrentTask());
  assert(bleRoleTransitionBegin(R::G2_STOP));
  assert(bleRoleTransitionState() == R::RECOVERING);
  std::thread other([] {
    assert(!bleRoleTransitionHeldByCurrentTask());
    assert(!bleRoleTransitionBegin(R::SERVER_START));
    bleRoleTransitionEnd();
    assert(bleRoleTransitionState() == R::RECOVERING);
  });
  other.join();
  bleRoleTransitionEnd();
  assert(bleRoleTransitionState() == R::RECOVERING);
  bleRoleTransitionEnd();
  assert(bleRoleTransitionState() == R::IDLE);
  assert(!bleRoleTransitionHeldByCurrentTask());
  assert(bleRoleTransitionBegin(R::SERVER_START));
  bleRoleTransitionEnd();
  bleStackSetLifecycleFault(true);
  assert(bleStackLifecycleFaulted());
  std::thread clear([] {
    assert(bleStackLifecycleFaulted());
    bleStackSetLifecycleFault(false);
  });
  clear.join();
  assert(!bleStackLifecycleFaulted());
  puts("Role owner and lifecycle fault checks passed");
}
'''


def between(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


class NativeProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not COMPILER or not COMPONENT.is_dir():
            raise RuntimeError('Prepare the roles app and provide a C++17 compiler before running these checks')

    def compile_and_run(self, text=None, source=None):
        with tempfile.TemporaryDirectory(prefix='hw1-roles-offline-') as temporary:
            work = Path(temporary)
            if text is not None:
                source = work / 'check.cpp'
                source.write_text(text)
            executable = work / 'check'
            compiled = subprocess.run([
                COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-Wno-unused-const-variable', '-fsanitize=address,undefined', '-g',
                str(source), '-o', str(executable),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def codec(self, source, entry):
        # Compile complete current codec bodies. Only firmware-only includes
        # are removed; wire algorithms and embedded vectors are unchanged.
        includes = ('System_BuildConfig.h', 'Arduino.h', 'System_MemUtil.h',
                    'System_Debug.h', 'esp_heap_caps.h', source + '.h')
        pattern = r'^#include ["<](?:' + '|'.join(map(re.escape, includes)) + r')[">].*$'
        header = (COMPONENT / (source + '.h')).read_text()
        body = (COMPONENT / (source + '.cpp')).read_text()
        text = PLATFORM + re.sub(pattern, '', header + '\n' + body, flags=re.M)
        return self.compile_and_run(text + f'\nint main() {{ return {entry}() ? 0 : 1; }}\n')

    def test_g2_capture_vectors_and_fragment_reassembly(self):
        self.codec('System_G2_Protocol', 'g2ProtocolGoldenSelfTest')

    def test_r1_embedded_protocol_vectors(self):
        output = self.codec('System_R1_Protocol', 'r1ProtocolSelfTest')
        self.assertNotIn('[R1-selftest] FAIL', output)
        self.assertGreaterEqual(output.count('[R1-selftest] PASS'), 59)

    def test_g2_health_graph_core(self):
        self.compile_and_run(source=COMPONENT / 'test/host/test_g2_health_graph_core.cpp')

    def test_actual_role_token_exclusion_and_fault_publication(self):
        source = (COMPONENT / 'Bluetooth.cpp').read_text()
        header = (COMPONENT / 'Bluetooth.h').read_text()
        enum = between(header, 'enum class BleRoleTransition', 'bool bleRoleTransitionBegin')
        state = between(source, 'static portMUX_TYPE sBleRoleTransitionMux', 'class BleRoleTransitionClaim')
        functions = between(source, 'bool bleRoleTransitionBegin', 'bool isBleControllerEnabled()')
        self.compile_and_run(ROLE_PLATFORM + enum + state + functions + ROLE_CHECKS)


if __name__ == '__main__':
    unittest.main()
