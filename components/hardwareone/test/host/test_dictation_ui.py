#!/usr/bin/env python3
"""Compile actual shared OLED/G2 dictation delivery and G2 service functions."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    g2=(SOURCE/'G2_Glasses.cpp').read_text();te=(SOURCE/'G2_Page_TextEntry.cpp').read_text();oled=(SOURCE/'OLED_Utils.cpp').read_text();header=(SOURCE/'System_Dictation.h').read_text()
    unit=(HERE/'dictation_ui_harness.cpp').read_text()
    transcript=(SOURCE/'System_Transcript.h').read_text()
    types=extract_block(transcript,'struct TranscriptStatus')+';\n'+'\n'.join(extract_block(header,s)+';' for s in ('enum class DictationState','struct DictationSnapshot','struct DictationTextReceipt'))+'\n'+extract_block(g2,'struct KbdPadState')+';'
    unit=unit.replace('// INSERT_TYPES',types)
    unit=unit.replace('// INSERT_FIELD','\n'.join(function(te,s) for s in ('size_t g2TextEntryPadRemaining(', 'size_t g2TextEntryPadAppendText(')))
    unit=unit.replace('// INSERT_G2','\n'.join(function(g2,s) for s in ('static bool kbdPadConsumeDictationText(', 'static void kbdPadDictationServiceOnTap(')))
    unit=unit.replace('// INSERT_OLED','\n'.join(function(oled,s) for s in ('static bool oledKeyboardConsumeDictationText(', 'void oledKeyboardDictationTick(')))
    for signature in ('static bool kbdPadConsumeDictationText(', 'static void kbdPadDictationServiceOnTap('):
        assert 'ENABLE_LOCAL_STT' not in function(g2,signature)
    assert 'ENABLE_LOCAL_STT' not in function(oled,'void oledKeyboardDictationTick(')
    with tempfile.TemporaryDirectory(prefix='hw1-dictation-ui-') as d:
        cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-missing-field-initializers',str(cpp),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
