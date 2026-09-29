#!/usr/bin/env python3
"""Exercise shipping Transcription HTTP handlers and browser JavaScript."""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--node', default=os.environ.get('NODE') or shutil.which('node'))
    args=parser.parse_args()
    if not args.node:
        raise SystemExit('Pass --node /path/to/node for JavaScript behavior tests')
    source=(COMPONENT/'WebPage_Sensors.cpp').read_text()
    http=source.split('// TRANSCRIPTION_HTTP_BEGIN',1)[1].split('\n',1)[1].split('// TRANSCRIPTION_HTTP_END',1)[0]
    types=(COMPONENT/'System_Dictation.h').read_text()
    transcript=(COMPONENT/'System_Transcript.h').read_text()
    definitions=extract_block(transcript,'struct TranscriptStatus')+';\n'
    for marker in ('enum class DictationState','struct DictationSnapshot','struct DictationTextReceipt','struct DictationAppLease','struct DictationAppSnapshot'):
        definitions+=extract_block(types,marker)+';\n'
    harness=(HERE/'transcription_web_harness.cpp').read_text().replace('// INSERT_PRODUCTION_TYPES',definitions).replace('// INSERT_PRODUCTION_HTTP',http)
    header=(COMPONENT/'System_Transcription_Web.h').read_text()
    js=header.split('R"STTJS(\n<script>\n',1)[1].split('\n</script>\n)STTJS"',1)[0]
    mic=(COMPONENT/'System_Microphone_Web.h').read_text()
    actual_js_function=extract_block(mic,'inline void streamMicrophoneSensorJS(')
    assert 'streamTranscriptionJS(req)' in actual_js_function, 'Panel JS must load on Sensors, not just Dashboard'
    assert 'streamTranscriptionPanel(req)' in extract_block(mic,'inline void streamMicrophoneSensorCard(')
    with tempfile.TemporaryDirectory(prefix='hw1-transcription-web-') as temp:
        temp=Path(temp); unit=temp/'test.cpp'; exe=temp/'test'; script=temp/'panel.js'
        unit.write_text(harness);script.write_text(js)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-Wno-sign-compare','-I',str(HERE/'web_batch_stubs'),'-I',str(COMPONENT.parent/'hardwareone_libs/ArduinoJson/src'),'-I',str(COMPONENT),str(unit),'-o',str(exe)]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
        subprocess.run([args.node,'--check',str(script)],check=True)
        subprocess.run([args.node,str(HERE/'transcription_web_test.js'),str(script)],check=True)

if __name__=='__main__':main()
