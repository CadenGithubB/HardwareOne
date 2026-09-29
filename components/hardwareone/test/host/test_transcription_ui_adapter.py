#!/usr/bin/env python3
"""Compile shipping command adapter/parser/normalizer with guarded storage and lease fakes."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    source=(COMPONENT/'System_TranscriptionUI.cpp').read_text()
    header=(COMPONENT/'System_Dictation.h').read_text()
    types=extract_block((COMPONENT/'System_Transcript.h').read_text(),'struct TranscriptStatus')+';\n'
    for name in ('enum class DictationState','struct DictationSnapshot','struct DictationTextReceipt','struct DictationAppLease','struct DictationAppSnapshot'):
        types+=extract_block(header,name)+';\n'
    parser=extract_block((COMPONENT/'System_Command.h').read_text(),'class CommandArgs')+';\nconst String CommandArgs::empty_;\n'
    args=(COMPONENT/'System_Command.cpp').read_text()
    parser+='\n'.join(function(args,s) for s in ('CommandArgs::CommandArgs(', 'void CommandArgs::parse(', 'const String& CommandArgs::arg('))
    adapter=source[source.index('namespace {'):source.index('const CommandEntry transcriptionUICommands')]
    unit=(HERE/'transcription_ui_adapter_harness.cpp').read_text().replace('// INSERT_TYPES',types).replace('// INSERT_PARSER',parser).replace('// INSERT_NORMALIZER',function((COMPONENT/'System_Filesystem.cpp').read_text(),'bool normalizeFsPath(')).replace('// INSERT_ADAPTER',adapter)
    with tempfile.TemporaryDirectory(prefix='hw1-transcription-adapter-') as t:
        t=Path(t);cpp=t/'test.cpp';exe=t/'test';cpp.write_text(unit)
        # Same bounded host String as the HTTP suite, extended only for the
        # production filesystem normalizer's reserve/character append surface.
        arduino=(HERE/'web_batch_stubs/Arduino.h').read_text().replace('  String& operator+=(const String& other)', '  void reserve(size_t n) { text_.reserve(n); }\n  String& operator+=(char c) { text_ += c; return *this; }\n  String& operator+=(const String& other)')
        (t/'Arduino.h').write_text(arduino)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-Wno-sign-compare','-I',str(t),'-I',str(COMPONENT.parent/'hardwareone_libs/ArduinoJson/src'),'-I',str(COMPONENT),str(cpp),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
