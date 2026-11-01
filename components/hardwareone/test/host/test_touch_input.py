#!/usr/bin/env python3
"""Exercise production single-contact queue, tap hit-testing and UI handoff."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]

class TouchTests(unittest.TestCase):
    def compile(self, source, includes):
        with tempfile.TemporaryDirectory(prefix='hw1-touch-') as temp:
            binary = Path(temp) / 'test'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', *includes, str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_existing_controller_ids_are_stable(self):
        header = (SOURCE / 'HAL_Input.h').read_text()
        enum = re.search(r'enum InputControllerType \{.*?\n\};', header, re.S).group()
        with tempfile.TemporaryDirectory(prefix='hw1-input-ids-') as temp:
            source = Path(temp) / 'ids.cpp'
            source.write_text(enum + '\nstatic_assert(INPUT_CONTROLLER_GAMEPAD_SEESAW == 0);'
                              '\nstatic_assert(INPUT_CONTROLLER_ANO_ENCODER == 3);'
                              '\nstatic_assert(INPUT_CONTROLLER_GPIO_ENCODER == 4);'
                              '\nstatic_assert(INPUT_CONTROLLER_GT911_TOUCH == 5);\nint main() {}\n')
            self.compile(source, [])

    def test_contact_queue_and_tap(self):
        self.compile(HERE / 'touch_core_harness.cpp', ['-I', str(SOURCE)])

    def test_ui_owner_handoff(self):
        with tempfile.TemporaryDirectory(prefix='hw1-touch-ui-') as temp:
            work = Path(temp)
            for name in ('OLED_Touch.cpp', 'OLED_Touch.h', 'OLED_TouchLayout.h', 'Input_TouchCore.h'):
                shutil.copy2(SOURCE / name, work / name)
            (work / 'HAL_Input.h').write_text('''#pragma once
#include "Input_TouchCore.h"
#define ENABLE_GT911_TOUCH 1
#define ENABLE_OLED_DISPLAY 1
enum { INPUT_BUTTON_A, INPUT_BUTTON_B, INPUT_BUTTON_X, INPUT_BUTTON_Y, INPUT_BUTTON_START, INPUT_BUTTON_SELECT };
#define INPUT_MASK(b) (1u << (b))
extern TouchQueue queue;
inline bool inputTouchConsume(TouchEvent& out) { return queue.pop(out); }
inline void inputTouchDiscard() { queue.discard(0); }
''')
            (work / 'System_SetupWizard.h').write_text('extern int page; inline int getWizardCurrentPage() { return page; }\n')
            unit = work / 'test.cpp'
            unit.write_text('''#include "OLED_Touch.cpp"
#include <assert.h>
TouchQueue queue; int page = 0;
void tapAt(int x, int y) { queue.sample(1,0,x,y,1); queue.sample(0,0,0,0,2); }
int main() {
  queue.sample(0,0,0,0,0);
  oledTouchContext(1);
  tapAt(500,880);
  assert(oledTouchRead().buttons == INPUT_MASK(INPUT_BUTTON_A));
  assert(oledTouchRead().buttons == 0);
  // A held contact cannot finish on another screen.
  queue.sample(1,0,500,880,1); oledTouchRead(); oledTouchContext(2);
  queue.sample(0,0,0,0,2); assert(oledTouchRead().buttons == 0);
  // A pending completed tap also cannot cross an ownership boundary.
  tapAt(500,880); oledTouchContext(3); assert(oledTouchRead().buttons == 0);
  uint32_t previous=0; bool initialized=false;
  oledWizardButtonPresses(previous,initialized);
  tapAt(300,880);
  assert(oledWizardButtonPresses(previous,initialized)==0);
  assert(oledWizardTouchNavigation().up);
  assert(!oledWizardTouchNavigation().up);
  tapAt(500,880);
  assert(!oledWizardTouchNavigation().up);
  assert(oledWizardButtonPresses(previous,initialized)==INPUT_MASK(INPUT_BUTTON_A));
  tapAt(500,880); ++page;
  assert(oledWizardButtonPresses(previous,initialized)==0);
}
''')
            self.compile(unit, ['-I', str(work)])

if __name__ == '__main__':
    unittest.main()
