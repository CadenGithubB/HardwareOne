#!/usr/bin/env python3
"""Exercise the actual G2 microphone redraw signature across local STT phases."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true')
    args=p.parse_args()
    source=(SOURCE/'G2_Glasses.cpp').read_text()
    header=(SOURCE/'System_Dictation.h').read_text()
    unit='''#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using TransportSessionEpoch=uint32_t;
constexpr uint32_t kNoTransportSessionEpoch=0;
enum CommandSource { SOURCE_INTERNAL, SOURCE_G2_GLASSES };
'''
    unit+=extract_block(header,'enum class DictationState')+';\n'
    unit+=extract_block(header,'struct DictationSnapshot')+';\n'
    unit+=extract_block(source,'struct KbdPadState')+';\n'
    unit+='''static KbdPadState gKbdPad{};
static DictationSnapshot current{};
static bool available=true, live=true;
DictationSnapshot dictationSnapshotNow(){return current;}
bool dictationAvailable(const char**){return available;}
bool transportSessionEpochIsLive(CommandSource,uint32_t){return live;}
'''
    unit+=extract_block(source,'static bool kbdPadMicStatusChanged(')
    unit+='''
int main(){
  gKbdPad.dictationEpoch=7;
  current.state=DictationState::RECORDING;
  current.ownerSource=SOURCE_G2_GLASSES;
  current.sourceName="PDM";
  current.preparing=true;
  current.bufferedLocal=true;
  assert(kbdPadMicStatusChanged(true));
  assert(!kbdPadMicStatusChanged(false));
  current.preparing=false;
  assert(kbdPadMicStatusChanged(false)); // Same source/state; prompt must change.
  assert(kbdPadMicStatusChanged(true));
  assert(!kbdPadMicStatusChanged(false));
  current.bufferedLocal=false;
  assert(kbdPadMicStatusChanged(true)); // Silence-VAD versus bounded-capture label.
  assert(!kbdPadMicStatusChanged(false));
  current.state=DictationState::WAITING;
  assert(kbdPadMicStatusChanged(true));
  live=false;
  assert(kbdPadMicStatusChanged(true));
  assert(!kbdPadMicStatusChanged(false));
  puts("G2 local STT status: preparing/capture and provider label changes repaint passed");
}
'''
    with tempfile.TemporaryDirectory(prefix='hw1-stt-g2-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()
