#!/usr/bin/env python3
"""Exercise production speaker admission/drain/worker/join with bounded RTOS fakes.
No model inference or device access. Extracts implementation rather than copying policy.
"""
import argparse
from pathlib import Path
import subprocess,tempfile
HERE=Path(__file__).resolve().parent

def block(text,signature):
    start=text.index(signature);brace=text.index('{',start);depth=0
    for i in range(brace,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[start:i+1]
    raise ValueError('unclosed '+signature)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true');p.add_argument('--source',type=Path,default=HERE.parents[1]);a=p.parse_args()
    text=(a.source/'System_STTLocal.cpp').read_text()
    state=text[text.index('struct SpeakerJob {'):text.index('void SpeakerState::stopTask() {')]
    functions='\n'.join(block(text,s) for s in ['void SpeakerState::stopTask()', 'SpeakerState::~SpeakerState()',
        'void speakerPostLabel(', 'void speakerLabelAndLog(', 'void speakerTask(', 'bool sttSpeakerSubmit(',
        'bool sttSpeakerBackground(', 'bool sttSpeakerTakeLabel(', 'void sttSpeakerDrain(', 'void sttSpeakerEnd('])
    unit=(HERE/'stt_speaker_queue_harness.cpp').read_text().replace('// INSERT_STATE',state).replace('// INSERT_FUNCTIONS',functions).replace('// INSERT_RELEASE',block(text,'void releaseSpeaker()'))
    with tempfile.TemporaryDirectory(prefix='hw1-speaker-queue-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function',
             '-DCONFIG_IDF_TARGET_ESP32P4=1','-DENABLE_STT_SPEAKER_LABELS=1','-I'+str(a.source),str(cpp),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=120);subprocess.run([str(exe)],check=True,timeout=20)
if __name__=='__main__':main()
