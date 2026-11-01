#!/usr/bin/env python3
"""Exercise the UI-owned physical control renderer and its disabled stub."""
from pathlib import Path
import shutil
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]

def main():
    for enabled in (0, 1):
        with tempfile.TemporaryDirectory(prefix='hw1-touch-controls-') as directory:
            root = Path(directory)
            for name in ('OLED_TouchControls.cpp', 'OLED_Touch.h', 'OLED_TouchLayout.h', 'Input_TouchCore.h'):
                shutil.copy2(SOURCE/name, root/name)
            (root/'System_BuildConfig.h').write_text(f'#define ENABLE_GT911_TOUCH {enabled}\n#define ENABLE_OLED_DISPLAY 1\n')
            (root/'Adafruit_GFX.h').write_text('''#pragma once
#include <cassert>
#include <cstring>
#include "OLED_TouchLayout.h"
class Adafruit_GFX {
 public:
  unsigned count=0;
  void setTextSize(int value) { assert(value==3); }
  void setTextColor(unsigned value) { assert(value==0xffff); }
  void drawRoundRect(int x,int y,int w,int h,int r,unsigned color) {
    const auto& control=touchControls[count];
    assert(x==control.x && y==control.y && w==control.w && h==control.h);
    assert(r==8 && color==0xffff);
  }
  void setCursor(int,int) {}
  void print(const char* label) { assert(!strcmp(label,touchControls[count].label)); ++count; }
};
''')
            (root/'HAL_Display.h').write_text('''#include "Adafruit_GFX.h"
extern bool ready; extern unsigned painted;
inline bool displayPaintSurface(void (*paint)(Adafruit_GFX&)) {
  if (!ready) return false;
  Adafruit_GFX canvas; paint(canvas); painted=canvas.count; return true;
}
''')
            (root/'test.cpp').write_text('''#include <cassert>
#include "OLED_TouchControls.cpp"
bool ready=false; unsigned painted=0;
int main() {
  assert(!oledTouchDrawControls() && painted==0);
  ready=true;
#if ENABLE_GT911_TOUCH
  assert(oledTouchDrawControls());
  assert(painted==sizeof(touchControls)/sizeof(touchControls[0]));
#else
  assert(!oledTouchDrawControls() && painted==0);
#endif
}
''')
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I',str(root),str(root/'test.cpp'),
                            '-o',str(root/'test')],check=True)
            subprocess.run([str(root/'test')],check=True)
    print('Touch control drawing and disabled backend passed')
if __name__ == '__main__': main()
