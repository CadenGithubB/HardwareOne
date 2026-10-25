#!/usr/bin/env python3
"""Run the real peer registry and both reconnect loops in boot order.

Boot registers the metadata-only phone; the pipe later re-registers its
connectable spec. Boot reconnect, the tick and name lookup must then reach the
pipe's admission.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

def through(source,start,last):
    """Slice from `start` to the end of the definition that opens at `last`."""
    begin=source.index(start);block=extract_block(source[begin:],last)
    return source[begin:source.index(block,begin)+len(block)]+';'

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    header=(ROOT/'BLE_Peers.h').read_text();source=(ROOT/'BLE_Peers.cpp').read_text()
    pipe=(ROOT/'BLE_PhonePipe.cpp').read_text()
    pipeSpec='\n'.join(re.search(r'^const BlePeer%s\{[^;]*\};$'%k,pipe,re.M).group(0) for k in ('Ops ops','Spec spec'))
    types='\n'.join([
        through(header,'enum BlePeerKind : uint8_t {','struct BlePeerSpec'),
        extract_block(header,'struct BlePeerReconnectSnapshot')+';',
        # The registry's declarations, as callers see them.
        header[header.index('bool bleRegisterPeer(const BlePeerSpec& spec);'):header.index('// MAC auto-save')],
    ])
    production='\n'.join([
        extract_block(source,'struct PeerReconnectRuntime')+';',
        'PeerReconnectRuntime sReconnect[BLE_PEER_MAX];',
        'portMUX_TYPE sReconnectMux = portMUX_INITIALIZER_UNLOCKED;',
        extract_block(source,'uint32_t nextReconnectGeneration('),
        extract_block(source,'void reconnectBumpIntentLocked('),
        extract_block(source,'void noteLinkUpLocked('),
        extract_block(source,'bool blePeerReconnectSnapshot('),
        source[source.index('static const BlePeerSpec* gPeerByKind'):source.index('// MAC auto-save')],
        source[source.index('static constexpr uint32_t kAdmissionBusyRetryMs'):source.index('struct ReconnectAdmissionClaim')],
        extract_block(source,'struct ReconnectAdmissionClaim')+';',
        *(extract_block(source,k) for k in (
            'static uint32_t bleReconnectBackoffMs(','static bool peerHasConnectAdmission(',
            'static BlePeerConnectAdmission sanitizeAdmission(','static const char* admissionName(',
            'static BlePeerConnectAdmission requestPeerConnectAdmission(',
            'static void fillAdmissionClaimLocked(','static bool claimDueAdmission(',
            'static bool claimBootAdmission(','static bool commitAdmission(',
            'void bleBootReconnect(void)','void blePeerNoteLinkLost(','void blePeerNoteLinkUp(',
            'void blePeerRequestReseek(BlePeerKind kind) ','bool blePeerRestoreDroppedReseek(BlePeerKind kind, ',
            'void bleAutoReconnectTick(void)')),
    ])
    unit=(HERE/'ble_peer_registry_harness.cpp').read_text()
    for marker,text in (('// INSERT_TYPES',types),('// INSERT_PRODUCTION',production),('// INSERT_PIPE_SPEC',pipeSpec)):
        assert unit.count(marker)==1,marker;unit=unit.replace(marker,text)
    with tempfile.TemporaryDirectory(prefix='hw1-peer-registry-') as tmp:
        path=Path(tmp)/'registry.cpp';binary=Path(tmp)/'registry';path.write_text(unit)
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-const-variable',str(path),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True,timeout=180)
