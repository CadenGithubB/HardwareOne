#!/usr/bin/env python3
"""Exercise production text-page readiness and callback publication without BLE I/O."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using G2TapFn = void (*)(int);
struct G2Temple {
  bool connected = false;
  bool pluginDead = false;
  void* writeChar = nullptr;
  uint32_t connectionGeneration = 0;
  bool containerReady = false;
  bool containerIsList = false;
};
static G2Temple gR, gL;
static uint32_t gPresentationEpoch = 1, gWidgetLifecycleEpoch = 1;
static uint32_t gTextViewLifecycleEpoch, gTextViewPresentationEpoch, gTextViewSlotSerial;
static uint32_t gTextNavPendingSerial, gTextExitPendingSerial;
static uint8_t gPageSwapAdmission, gInteractiveLensLease;
static bool gWidgetTerminalLatched, gTextViewActive, gTextViewIsTextPage;
static bool overlay, fsmSwapping, injectSwapOnTopologyLock;
static void (*gTextViewExitFn)();
static G2TapFn gTextViewTapFn;
static int gSyncLifecycleMux, gTopologyMux, writable;
static uint32_t millis() { return 100; }
static bool g2FsmPageSwapping() { return fsmSwapping; }
static bool firmwareOverlayActiveLocked(uint32_t) {
  assert(gSyncLifecycleMux == 1);
  return overlay;
}
static void portENTER_CRITICAL(int* mux) {
  assert(*mux == 0);
  *mux = 1;
  if (mux == &gTopologyMux) {
    assert(gSyncLifecycleMux == 1);
    if (injectSwapOnTopologyLock) gPageSwapAdmission = 1;
  }
}
static void portEXIT_CRITICAL(int* mux) { assert(*mux == 1); *mux = 0; }
static void onExit() {}
static void foreignExit() {}
static void onTap(int) {}
static void foreignTap(int) {}
// INSERT_PRODUCTION
static void reset() {
  gR = {true, false, &writable, 4, true, false};
  gL = {false, false, &writable, 5, true, false};
  gPresentationEpoch = gWidgetLifecycleEpoch = 1;
  gTextViewLifecycleEpoch = gTextViewPresentationEpoch = gTextViewSlotSerial = 0;
  gTextNavPendingSerial = gTextExitPendingSerial = 0;
  gPageSwapAdmission = gInteractiveLensLease = 0;
  gWidgetTerminalLatched = gTextViewActive = gTextViewIsTextPage = false;
  overlay = fsmSwapping = injectSwapOnTopologyLock = false;
  gTextViewExitFn = nullptr;
  gTextViewTapFn = nullptr;
  assert(g2TextViewPublishForLifecycle(onExit, onTap, 1, 1, true));
}
static void expect(bool ready, bool waiting) {
  bool pending = !waiting;
  assert(g2TextPageReady(onExit, onTap, &pending) == ready);
  assert(pending == waiting);
  assert(gSyncLifecycleMux == 0 && gTopologyMux == 0);
}
int main() {
  reset(); expect(true, false);
  assert(g2TextPageReady(onExit, onTap, nullptr));
  assert(!g2TextPageReady(foreignExit, onTap, nullptr));
  assert(!g2TextPageReady(onExit, foreignTap, nullptr));
  reset(); gPageSwapAdmission = 1; expect(false, true);
  reset(); gInteractiveLensLease = 1; expect(false, true);
  reset(); fsmSwapping = true; expect(false, true);
  reset(); injectSwapOnTopologyLock = true; expect(false, true);
  reset(); overlay = true; expect(false, true);
  overlay = false; expect(true, false);
  reset(); gWidgetTerminalLatched = true; expect(false, true);
  assert(!g2TextViewPublishForLifecycle(onExit, onTap, 1, 1, true));
  reset(); ++gWidgetLifecycleEpoch; expect(false, false);
  assert(!g2TextViewPublishForLifecycle(onExit, onTap, 1, 1, true));
  reset(); ++gPresentationEpoch; expect(false, false);
  assert(!g2TextViewPublishForLifecycle(onExit, onTap, 1, 1, true));
  reset(); gTextViewSlotSerial = 0; expect(false, false);
  reset(); gTextViewActive = false; expect(false, false);
  reset(); gTextViewIsTextPage = false; expect(false, false);
  reset(); gR.containerReady = false; expect(false, false);
  reset(); gR.containerIsList = true; expect(false, false);
  reset(); gR.writeChar = nullptr; expect(false, false);
  reset(); gR.connectionGeneration = 0; expect(false, false);
  reset(); gR.connected = false; expect(false, false);
  gL.connected = true; expect(true, false);
  reset(); gR.pluginDead = true; expect(false, false);
  gL.connected = true; expect(true, false);
  reset(); gR.containerReady = false; gL.connected = true;
  expect(false, false); // UPDATE prefers right; never borrow left readiness.
  reset();
  // An accepted CREATE stays pending before and after callback publication,
  // until swap completion releases admission. A failed CREATE can be retried.
  gPageSwapAdmission = 1;
  portENTER_CRITICAL(&gSyncLifecycleMux);
  g2TextViewDisarmLocked(true);
  portEXIT_CRITICAL(&gSyncLifecycleMux);
  expect(false, true);
  ++gPresentationEpoch;
  assert(g2TextViewPublishForLifecycle(onExit, onTap, 1, 2, true));
  expect(false, true);
  gPageSwapAdmission = 0; expect(true, false);
  portENTER_CRITICAL(&gSyncLifecycleMux);
  g2TextViewDisarmLocked(true);
  portEXIT_CRITICAL(&gSyncLifecycleMux);
  expect(false, false);
  assert(!gTextViewIsTextPage);
  assert(g2TextViewPublishForLifecycle(onExit, onTap, 1, 2, false));
  expect(false, false); // Other live text/mixed producers are not ordinary pages.
  std::puts("G2 text-page readiness: publication, pending, replacement, overlay and transport guards passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', type=Path, default=HERE.parents[1])
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    component = args.component.resolve()
    sys.path.insert(0, str(component / 'test/host'))
    from test_web_batch_handlers import extract_block
    source = (component / 'G2_Glasses.cpp').read_text()
    code = '\n\n'.join(extract_block(source[source.rindex(marker):], marker) for marker in (
        'static inline uint32_t g2PresentationEpochCurrent(',
        'static inline bool pageSwapInFlight(',
        'static uint32_t g2TextViewNextSerialLocked(',
        'static void g2TextViewDisarmLocked(',
        'static bool g2TextViewPublishForLifecycle(',
        'bool g2TextPageReady('))
    with tempfile.TemporaryDirectory(prefix='hw1-g2-text-ready-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(HARNESS.replace('// INSERT_PRODUCTION', code))
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
