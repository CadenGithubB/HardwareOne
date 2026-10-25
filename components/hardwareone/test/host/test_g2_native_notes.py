#!/usr/bin/env python3
"""Production native QuickNote adapter, codec, draft lifecycle and worker release with IO doubles."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    component=here.parents[1]
    def source(name):
        return re.sub(r'^#include.*\n|^#pragma once\n','',(component/name).read_text(),flags=re.M)
    unit=(here/'g2_native_notes_harness.cpp').read_text()
    parts={
        '// INSERT_POLICY': '\n'.join(source(n) for n in ('Transcript_PathPolicy.h','Transcript_TextPolicy.h','Transcription_UI_Policy.h','QuickNote_Policy.h')),
        '// INSERT_DECL': source('QuickNote_Store.h')+source('G2_NativeNotes.h'),
        '// INSERT_PROTOCOL': source('G2_NativeNotesProtocol.h'),
        '// INSERT_RUNTIME': source('G2_NativeNotes.cpp'),
    }
    for marker,text in parts.items():
        assert unit.count(marker)==1
        unit=unit.replace(marker,text)
    with tempfile.TemporaryDirectory(prefix='hw1-native-notes-') as tmp:
        root=Path(tmp);cpp=root/'test.cpp';exe=root/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-const-variable',str(cpp),'-o',str(exe)]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True)
        subprocess.run([str(exe)],check=True)
        # The worker leaves only through leaveOnStop(), as the first statement
        # of its loop. Private text and the STT token go before the null handle
        # is published, and nothing but the critical-section exit and the
        # return sit between that publish and vTaskDelete(nullptr).
        runtime=(component/'G2_NativeNotes.cpp').read_text()
        leave=re.search(r'\nbool leaveOnStop\(\) \{\n(.*?)\n\}\n',runtime,flags=re.S).group(1)
        assert leave.index('reset();')<leave.index('portENTER_CRITICAL(&taskMux);')
        assert re.search(r'task=nullptr;stopWanted\.store\(false\);\}\n\s*portEXIT_CRITICAL\(&taskMux\);\n\s*return leave;$',leave)
        assert re.search(r'\nvoid worker\(void\*\) \{\n\s*for\(;;\) \{\n\s*if\(leaveOnStop\(\)\)vTaskDelete\(nullptr\);\n',runtime)
        assert runtime.count('vTaskDelete(')==1 and runtime.count('{task=nullptr;')==1
        # Each prerequisite can be absent independently. Compile the real
        # disabled API and translation unit, with no SDK or STT definitions.
        flags=('ENABLE_BLUETOOTH','ENABLE_G2_GLASSES','ENABLE_QUICKNOTE','ENABLE_LOCAL_STT')
        disabled=('#include <cstddef>\n#include <cstdint>\n#include <cassert>\n'+
                  source('G2_NativeNotes.h')+source('G2_NativeNotes.cpp')+
                  '\nint main(){G2NoteContext c;g2NativeNotesTick();'
                  'g2NativeNotesReceive(c,nullptr,0);assert(!g2NativeNotesActive());'
                  'assert(g2NativeNotesRelease(0));'
                  'assert(!g2NativeNotesContextLive(c));}\n')
        cpp.write_text(disabled)
        for off in flags:
            subprocess.run(cmd+[f'-D{flag}={int(flag!=off)}' for flag in flags],check=True)
            subprocess.run([str(exe)],check=True)
        print('Native G2 notes: all four disabled prerequisites passed')
if __name__=='__main__': main()
