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
    preamble = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
constexpr int kWidth=720, kHeight=1280, WS_LCD_BACKLIGHT_PIN=26;
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
 uint16_t* pixels_=nullptr;
 uint8_t* buffer=nullptr;
 uint8_t presentedCanvas_[1024]={};
 bool canvasPresented_=true, wakePending_=false, pwm_=true, powered_=true, inverted_=false;
 uint8_t brightness_=255;
 void display(); void backlight(); void setBrightness(uint8_t); void setPower(bool);
};
'''
    tests = r'''
int main() {
 std::vector<uint16_t> pixels(kWidth*kHeight);
 uint8_t canvas[1024]={};
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
        flags=['-std=c++17','-Wall','-Wextra','-Werror']
        if args.sanitize: flags+=['-fsanitize=address,undefined','-g']
        subprocess.run([args.cxx,*flags,str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('waveshare_wake: sequencing, brightness gate, cache failure and cancellation pass')

if __name__=='__main__': main()
