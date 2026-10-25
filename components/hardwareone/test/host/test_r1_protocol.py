#!/usr/bin/env python3
"""Compile the complete production R1 codec and run its fixtures on the host."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block


def check_public_profile_mirror(component: Path) -> None:
    """Every exact firmware profile has a public mirror, a mapping and a name.

    The harness compiles only the codec, so G2_Ring's mirror is checked here:
    a missed case would compile (each switch has a default) and report
    "unknown" with protocolProfileKnown=true.
    """
    header = (component/'System_R1_Protocol.h').read_text()
    enum = extract_block(header, 'enum R1ProtocolProfile')
    profiles = re.findall(r'R1_PROFILE_(FW_\w+)\s*=', enum)
    assert profiles, 'no firmware profiles found'
    ring_h = (component/'G2_Ring.h').read_text()
    ring_cpp = (component/'G2_Ring.cpp').read_text()
    public_enum = extract_block(ring_h, 'enum G2RingProtocolProfile')
    public_name = extract_block(ring_h, 'inline const char* g2RingProtocolProfileName(')
    mapping = extract_block(ring_cpp, 'static G2RingProtocolProfile ringPublicProfile(')
    for name in profiles:
        assert f'G2_RING_PROFILE_{name}' in public_enum, name
        assert f'case G2_RING_PROFILE_{name}:' in public_name, name
        assert f'case R1_PROFILE_{name}:' in mapping and f'return G2_RING_PROFILE_{name};' in mapping, name
        assert f'static_cast<uint8_t>(R1_PROFILE_{name})' in ring_cpp, name

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    component = here.parents[1]
    check_public_profile_mirror(component)
    with tempfile.TemporaryDirectory(prefix='hw1-r1-protocol-') as tmp:
        root = Path(tmp)
        for name in ('System_R1_Protocol.cpp', 'System_R1_Protocol.h'):
            shutil.copyfile(component/name, root/name)
        (root/'System_BuildConfig.h').write_text('#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n')
        (root/'Arduino.h').write_text('#include <stdint.h>\n#include <stddef.h>\n')
        (root/'System_MemUtil.h').write_text('''#include <cstdlib>
enum class AllocPref {RequireInternal, PreferPSRAM};
inline void* ps_alloc(size_t size, AllocPref, const char*) {return malloc(size);}
''')
        (root/'esp_heap_caps.h').write_text('#include <cstdlib>\n#define heap_caps_free free\n')
        (root/'System_Debug.h').write_text('''#include <cstdio>
#define DEBUG_RING_SETUPF(...) do { if (false) std::printf(__VA_ARGS__); } while (0)
''')
        binary = root/'test'
        cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-I', str(root), str(root/'System_R1_Protocol.cpp'),
               str(here/'r1_protocol_harness.cpp'), '-o', str(binary)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)

        # The setup dark-clock probe, spliced from G2_Ring.cpp, against the
        # same real codec.
        ring = (component/'G2_Ring.cpp').read_text()
        probe = extract_block(ring, 'static void ringSetupServiceDarkProbe(')
        harness = (here/'ring_dark_probe_harness.cpp').read_text()
        assert harness.count('// INSERT_PROBE') == 1
        (root/'ring_dark_probe.cpp').write_text(harness.replace('// INSERT_PROBE', probe))
        probe_binary = root/'probe'
        probe_cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                     '-I', str(root), str(root/'System_R1_Protocol.cpp'),
                     str(root/'ring_dark_probe.cpp'), '-o', str(probe_binary)]
        if args.sanitize:
            probe_cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(probe_cmd, check=True)
        subprocess.run([str(probe_binary)], check=True)

        # advStart temple identity: live temples first, the saved G2 pair for
        # a temple that is down. Resolver and parser are spliced from production.
        utils = (component/'System_Utils.h').read_text()
        identity = (here/'ring_adv_identity_harness.cpp').read_text()
        for marker, body in (('// INSERT_MACPARSE', extract_block(utils, 'inline bool macParse(')),
                             ('// INSERT_RESOLVE', extract_block(ring, 'static bool ringResolveAdvIdentity('))):
            assert identity.count(marker) == 1
            identity = identity.replace(marker, body)
        (root/'ring_adv_identity.cpp').write_text(identity)
        identity_binary = root/'identity'
        identity_cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root), str(root/'System_R1_Protocol.cpp'),
                        str(root/'ring_adv_identity.cpp'), '-o', str(identity_binary)]
        if args.sanitize:
            identity_cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(identity_cmd, check=True)
        subprocess.run([str(identity_binary)], check=True)
