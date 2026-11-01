#!/usr/bin/env python3
"""Exercise production Waveshare wake methods with mocked SDK boundaries."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (HERE.parents[1] / 'HAL_Display_Waveshare.cpp').read_text()
    methods = '\n'.join(extract_block(source, 'void WaveshareDisplay::' + name)
                        for name in ('display()', 'backlight()', 'setBrightness(', 'setPower('))
    methods += '\n' + extract_block(source, 'bool WaveshareDisplay::setLandscape(')
    preamble = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>
#include "HAL_Display_WavesharePixels.h"
constexpr int kWidth=720, kHeight=1280, WS_LCD_BACKLIGHT_PIN=26;
constexpr size_t kBytes=kWidth*kHeight*sizeof(uint16_t);
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_CACHE_MSYNC_FLAG_DIR_C2M=1;
int cacheResult=0, commandResult=0, duty=720;
std::vector<int> events;
struct Guard { explicit Guard(int) {} };
void ledcWrite(int, int value) { duty=value; events.push_back(value ? 4 : 0); }
void delay(int ms) { assert(ms==80); events.push_back(3); }
int sendCommand(int, int command, const void*, int) {
    assert(duty==0); events.push_back(command); return commandResult;
}
int esp_cache_msync(void*, unsigned long, int) { events.push_back(2); return cacheResult; }
const char* esp_err_to_name(int) { return "fault"; }
#define ESP_LOGE(...) ((void)0)
class WaveshareDisplay {
public:
 int lock_=1, io_=1;
 std::atomic<bool> landscape_{false};
 uint16_t* pixels_=nullptr;
 uint8_t* buffer=nullptr;
 uint8_t presentedCanvas_[waveshare_display::kCanvasBytes]={};
 bool canvasPresented_=true, wakePending_=false, pwm_=true, powered_=true, inverted_=false;
 uint8_t brightness_=255;
 bool setLandscape(bool);
 void display(); void backlight(); void setBrightness(uint8_t); void setPower(bool);
};
'''
    tests = r'''
int main() {
 std::vector<uint16_t> pixels(kWidth*kHeight);
 uint8_t canvas[waveshare_display::kCanvasBytes]={};
 WaveshareDisplay d; d.pixels_=pixels.data(); d.buffer=canvas;
 d.setPower(false);
 assert(!d.powered_ && duty==0 && events[0]==0 && events[1]==0x28);
 events.clear(); d.setPower(true);
 assert(d.powered_ && d.wakePending_ && duty==0);
 d.setBrightness(255); assert(duty==0);
 // A repeated DISPLAYON must not bypass the pending frame gate.
 d.setPower(true); assert(duty==0 && d.wakePending_);
 cacheResult=1; d.display(); assert(duty==0 && d.wakePending_);
 cacheResult=0; events.clear(); d.display();
 assert(!d.wakePending_ && duty==720);
 assert((events==std::vector<int>{2,3,4}));
 // Unchanged subsequent frames do not delay or rewrite PWM.
 events.clear(); d.display(); assert(events.empty());
 // Changes in the extra bottom pages must not be skipped by the frame cache.
 canvas[sizeof(canvas)-1]=0x80; d.display(); assert((events==std::vector<int>{2}));
 auto corner=waveshare_display::toPanel(40+127*5,380+87*5,false);
 assert(pixels[corner.y*kWidth+corner.x]==0xffff);
 events.clear(); d.display(); assert(events.empty());
 // A layout change clears obsolete geometry and forces a fresh frame.
 assert(d.setLandscape(true)); assert(!d.setLandscape(true));
 assert(!d.canvasPresented_);
 for(auto value:pixels) assert(value==0);
 d.display(); assert((events==std::vector<int>{2}));
 corner=waveshare_display::toPanel(127*8,8+87*8,true);
 assert(pixels[corner.y*kWidth+corner.x]==0xffff);
 events.clear(); d.display(); assert(events.empty());
 // Cancellation while a wake is pending leaves the panel dark.
 d.setPower(false); d.setPower(true); d.setPower(false); d.display();
 assert(!d.powered_ && !d.wakePending_ && duty==0);
 // Failed DISPLAYON cannot illuminate a panel that remained off.
 commandResult=1; d.setPower(true); assert(!d.powered_ && duty==0);
}
'''
    with tempfile.TemporaryDirectory(prefix='hw1-waveshare-wake-') as tmp:
        src=Path(tmp)/'wake.cpp'; binary=Path(tmp)/'wake'
        src.write_text(preamble+'\n'+methods+'\n'+tests)
        flags=['-I',str(HERE.parents[1]),'-std=c++17','-Wall','-Wextra','-Werror']
        if args.sanitize: flags+=['-fsanitize=address,undefined','-g']
        subprocess.run([args.cxx,*flags,str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('waveshare_wake: sequencing, brightness gate, cache failure and cancellation pass')

if __name__=='__main__': main()
