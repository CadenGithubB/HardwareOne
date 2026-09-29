#!/usr/bin/env python3
"""Test actual shared Dictation admission/delivery with fake STT and storage."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]
ROOT=SOURCE.parents[1]
from test_espsr_runtime import function
from test_web_batch_handlers import extract_block

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--staged',type=Path,help='Optional review-only component directory')
    p.add_argument('--sanitize',action='store_true')
    args=p.parse_args()
    base=args.staged or SOURCE
    source=(base/'System_Dictation.cpp').read_text()
    stt_header=(base/'System_STT.h').read_text()
    local_header=(base/'System_STTLocal.h').read_text()
    transcript_header=(base/'System_Transcript.h').read_text()
    headers=re.sub(r'^#include.*\n|^#pragma once\n','',transcript_header+'\n'+local_header+'\n'+stt_header,flags=re.M)
    controls=source[source.index('struct DictationPublishedCapture'):source.index('// Capability is deliberately')]
    local=source[source.index('#if ENABLE_LOCAL_STT\n// A local exchange'):source.index('#endif // ENABLE_LOCAL_STT',source.index('// A local exchange'))]+'\n#endif\n'
    harness=(HERE/'dictation_local_harness.cpp').read_text()
    dict_header=(base/'System_Dictation.h').read_text()
    headers+='\n'+'\n'.join(extract_block(dict_header,sig)+';' for sig in ('enum class DictationState','struct DictationTextReceipt','struct DictationSnapshot','struct DictationAppLease','struct DictationAppSnapshot','enum class DictationUartIntrinsicResult'))+'\n'
    off_prefix=harness.split('// INSERT_HEADERS')[0].replace('#define ENABLE_DICTATION 1','#define ENABLE_DICTATION 0')
    off_unit=off_prefix+headers+dict_header.split('\n#else\n',1)[1].split('#endif  // ENABLE_DICTATION',1)[0]+'''
int main(){
 DictationAppLease lease{SOURCE_WEB,7,123}; DictationAppSnapshot snap;
 DictationTextReceipt receipt{123,1,0,3}; char text[8]="private";
 assert(!dictationAppBegin(SOURCE_WEB,7,&lease)&&!lease.exchange);
 lease={SOURCE_WEB,7,123};assert(!dictationAppCurrent(SOURCE_WEB,7,&lease)&&!lease.exchange);
 assert(!dictationAppSnapshot(lease,&snap)&&!snap.valid&&!snap.busy);
 assert(!dictationAppPeekText(lease,text,sizeof(text),&receipt)&&!text[0]&&!receipt.exchange);
 assert(!dictationAppCommitText(lease,receipt,1));
 assert(!dictationAppRequestStop(lease)&&!dictationAppCancel(lease));
 puts("Dictation feature-off App stubs passed");
}
'''
    harness=harness.replace('// INSERT_HEADERS',headers)
    harness=harness.replace('// INSERT_CONTROL',controls)
    harness=harness.replace('// INSERT_LOCAL',local)
    harness=harness.replace('// INSERT_LIVE',function(source,'static bool displaySessionStillLive('))
    shared='\n'.join(function(source, sig) for sig in ['static bool dictFailOwned(', 'static bool dictationPeekTextImpl(', 'static bool dictationCommitTextImpl(', 'bool dictationPeekTextFor(', 'bool dictationCommitTextFor(', 'bool dictationAppPeekText(', 'bool dictationAppCommitText(', 'bool dictationTakeTextFor(', 'static bool dictationCancelImpl(', 'bool dictationAppCancel(', 'void dictationFieldFullFor(', 'static const char* dictDeliver('])
    harness=harness.replace('// INSERT_DRAIN',shared)
    harness=harness.replace('// INSERT_STOP','\n'.join(function(source,sig) for sig in ('static bool dictationRequestStopImpl(', 'void dictationRequestStopFor(', 'bool dictationAppRequestStop(')))
    harness=harness.replace('// INSERT_BEGIN','\n'.join(function(source,sig) for sig in ('static bool dictationBeginImpl(', 'bool dictationBeginFor(', 'bool dictationAppBegin(')))
    harness=harness.replace('// INSERT_SAVE', '\n'.join(function(source, sig) for sig in ('static void dictationProcessSave(', 'static bool dictationWorkerHasWork(', 'DictationSnapshot dictationSnapshotNow(', 'bool dictationAppCurrent(', 'bool dictationAppSnapshot(', 'static void dictationSupervise(')))
    # Verify the old host body remains selected by the compile-time provider
    # branch, and local tokens cannot enter either UART completion operation.
    assert 'return dictationBeginLocal(displaySource, displayEpoch, app);\n#endif' in function(source,'static bool dictationBeginImpl(')
    assert 'gDict.requestHostEpoch != 0' in function(source,'static const char* dictDeliver(')
    assert 'gDict.requestHostEpoch != 0' in function(source,'static const char* dictationHandleArgs(')
    assert 'dictationSupervise();' in function(source,'static void dictationWorkerBody(')
    assert 'dictationWakeWorker' not in function(source,'static void dictationSupervise(')
    with tempfile.TemporaryDirectory(prefix='hw1-local-dictation-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-missing-field-initializers',str(unit),'-o',str(exe),'-I',str(ROOT/'components/hardwareone')]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        for provider in (0,1):
            subprocess.run(cmd+['-DENABLE_LOCAL_STT='+str(provider)],check=True)
            subprocess.run([str(exe)],check=True)
        unit.write_text(off_unit)
        subprocess.run(cmd+['-DENABLE_LOCAL_STT=0'],check=True)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()
