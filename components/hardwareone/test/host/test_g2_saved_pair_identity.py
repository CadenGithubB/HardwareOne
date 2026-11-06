#!/usr/bin/env python3
"""Run the production learned-pair replacement policy and R1 identity resolver.

BLE state, mutexes and the clock are host boundaries. The production MAC
matching, replacement decision, registry commit and R1 identity resolver run
unchanged, including generation/authority fencing and address-type retention.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate(registry_source=None):
    g2 = (COMPONENT / 'G2_Glasses.cpp').read_text()
    ring = (COMPONENT / 'G2_Ring.cpp').read_text()
    peers = (COMPONENT / 'BLE_Peers.h').read_text()
    registry = registry_source if registry_source is not None else (COMPONENT / 'BLE_Peers.cpp').read_text()
    header = (COMPONENT / 'G2_Glasses.h').read_text()
    utils = (COMPONENT / 'System_Utils.h').read_text()
    types = '\n'.join(extract_block(source, marker) + ';' for source, marker in (
        (header, 'enum G2Eye'), (g2, 'struct G2LinkSnapshot'),
        (peers, 'enum BlePeerKind'), (peers, 'struct BlePeerSavedTarget'),
        (registry, 'struct BlePeerData'), (registry, 'struct PeerReconnectRuntime')))
    constants = '\n'.join(re.search(r'^constexpr .* ' + name + r' = [^;\n]*;', peers, re.M)[0]
                          for name in ('BLE_PEER_ADDRESS_TEXT_CAPACITY', 'BLE_PEER_DEFAULT_ADDRESS_TYPE'))
    functions = '\n\n'.join(extract_block(source, marker) for source, marker in (
        (utils, 'inline bool macParse('), (g2, 'static bool macEqualsIgnoreCase('),
        (g2, 'static bool templeMatchesSavedTarget('),
        (g2, 'static uint8_t g2LearnedTargetReplaceMask('),
        (ring, 'static bool ringResolveAdvIdentity('),
        (registry, 'uint32_t nextReconnectGeneration('),
        (registry, 'void reconnectBumpIntentLocked('),
        (registry, 'void reconnectBumpIdentityLocked('),
        (registry, 'bool copySavedAddress('),
        (registry, 'bool savedTargetMatches('),
        (registry, 'void noteLinkUpLocked('),
        (registry, 'bool blePeerCommitLearnedTargetIfCurrent(')))
    connect = extract_block(g2, 'static bool g2ConnectSync(')
    assert re.search(r'const uint8_t replaceMask = g2LearnedTargetReplaceMask\(\s*eye, after, expectedRequest->savedTarget\)', connect)
    assert connect.count('blePeerCommitLearnedTargetIfCurrent(') == 1
    commit = connect[connect.index('const uint8_t replaceMask ='):]
    commit = commit[:commit.index('&persistNeeded);')]
    assert '(replaceMask & 0x01) && after.left' in commit
    assert '(replaceMask & 0x02) && after.right' in commit
    assert re.search(r'replaceMask,\s*/\*completeTopology=\*/after.left && after.right,', commit)
    harness = (HERE / 'g2_saved_pair_identity_harness.cpp').read_text()
    return harness.replace('// INSERT_TYPES', constants + '\n' + types).replace('// INSERT_PRODUCTION', functions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-g2-saved-pair-') as temp:
        root = Path(temp); source = root / 'test.cpp'; binary = root / 'test'
        source.write_text(generate())
        command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   str(source), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
