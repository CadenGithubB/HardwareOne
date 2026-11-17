#!/usr/bin/env python3
"""Run the production ESP-NOW V4 receive front end against forged, duplicate and relayed frames.

Extracts from System_ESPNow.cpp the whole of v4_try_handle_incoming, the
duplicate rings and their functions, the opcode table with v4_dispatch_lookup
and v4_dispatch_table_try, the real v4h_heartbeat and v4h_relay_data, the flood
forwarder (v4_relay_maybe_forward) with both relay allow-lists, the peer slot
owners (getMeshPeerHealth, getMeshPeerMeta, noteMeshPeerRxActivity,
isPairedDevice, removeEspNowDevice, processMetadata and, when the tree has
them, meshPeerMetaReclaimSlot and meshPeerSlotsRelease) and v4_crc16_ccitt;
from the headers the wire enums, header and payload structs, V4RxCtx,
MeshPeerHealth, MeshPeerMeta, EspNowInlineText and MeshRelayMetrics. Every
other handler in the table is a recording double. Sessions (a keyed toy seal
with the 64-frame replay window), the group-key tag, the radio, the route
table and the reassembler are host doubles: this suite is about which frames
reach a handler, an ACK, a forward or a peer slot, not about the ciphers.

Cases: a forged plaintext frame carrying a peer's origin and msgId does not
make that peer's sealed frame drop; neither does another paired peer's sealed
frame or a group-tagged frame naming that origin; a re-sealed retry of a single
frame or of a TEXT fragment is still delivered once; a duplicate flooded
broadcast is suppressed and forwarded once, also after a burst of forged
plaintext; a duplicate relayed envelope is suppressed and a forged direct frame
does not shadow the relayed one; a frame whose fragIndex does not fit its
fragCount is refused before it is opened or acknowledged; HEARTBEAT needs a
tagged or sealed frame once the device has a mesh passphrase and still works
without one; plaintext frames from a MAC that is not paired create no health
or metadata slot, and neither does a captured group-tagged HEARTBEAT replayed
from an invented radio MAC; unpair releases the metadata slot; a full metadata table
gives a slot to a newly paired peer and never takes one from a paired peer.

`--component <dir>` or `--source-dir <overlay>` replays another tree's
production sources (such as the saved base tree). A tree without
meshPeerSlotsRelease has the slot release of cmd_espnow_unpair wrapped as that
function, so the cases name the former defects. extract_block is imported from
the directory of this runner on purpose, not from `<component>/test/host`.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

REAL_HANDLERS = ('v4h_heartbeat', 'v4h_relay_data')
WIRE_DEFINES = re.compile(
    r'^#define ESPNOW_V4_(MAGIC|VERSION|HEADER_LEN|MAX_PAYLOAD|AEAD_TAG_LEN|BROADCAST_AUTH_TAG_LEN)\b.*$',
    re.MULTILINE)
RECLAIM = 'static int meshPeerMetaReclaimSlot(const uint8_t* forMac)'
RELEASE = 'static void meshPeerSlotsRelease('
UNPAIR_SLOT_COMMENT = "// Clear the peer's mesh-liveness slot regardless of mode."


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    header = read('System_ESPNow.h')
    wire = read('System_ESPNow_Wire.h')
    rxctx = read('System_ESPNow_RxCtx.h')
    router = read('System_ESPNow_Router.h')

    def one(text, marker):
        if text.count(marker) != 1:
            sys.exit(f'FAIL extraction: marker {marker!r} found {text.count(marker)} times, expected 1')
        return extract_block(text, marker)

    def last(text, marker):
        # A forward declaration precedes the definition: take the last match.
        if marker not in text:
            sys.exit(f'FAIL extraction: marker {marker!r} not found')
        return extract_block(text[text.rindex(marker):], marker)

    defines = [m.group(0) for m in WIRE_DEFINES.finditer(wire)]
    if len(defines) != 6:
        sys.exit(f'FAIL extraction: expected six ESPNOW_V4_* #defines, found {len(defines)}')
    hops = re.search(r'^#define MESH_ROUTE_MAX_HOPS\b.*$', router, re.MULTILINE)
    if not hops:
        sys.exit('FAIL extraction: MESH_ROUTE_MAX_HOPS not found in System_ESPNow_Router.h')
    wire_types = '\n'.join(defines + [hops.group(0)] + [one(wire, marker) + ';' for marker in (
        'enum EspNowV4Type', 'enum EspNowV4Flags',
        'struct __attribute__((packed)) EspNowV4Header',
        'struct __attribute__((packed)) V4PayloadRelayData',
        'struct __attribute__((packed)) V4PayloadHeartbeat',
        'struct __attribute__((packed)) V4PayloadMetadata')])

    # System_ESPNow.h holds an enabled and a disabled (stub) half; the first
    # definition of each type is the enabled one.
    note_decl = re.search(r'^void noteMeshPeerRxActivity\(const uint8_t\* mac, EspNowMeshRxKind kind[^;]*;',
                          header, re.MULTILINE)
    if not note_decl:
        sys.exit('FAIL extraction: noteMeshPeerRxActivity declaration not found in System_ESPNow.h')
    peer_types = '\n'.join([
        extract_block(header, 'enum class EspNowMeshRxKind') + ';',
        'template <size_t Capacity>\n' + one(header, 'class EspNowInlineText') + ';',
        extract_block(header, 'struct MeshPeerHealth') + ';',
        extract_block(header, 'struct MeshPeerMeta') + ';',
        one(router, 'struct MeshRelayMetrics') + ';',
        one(rxctx, 'struct V4RxCtx') + ';',
        note_decl.group(0)])

    # Duplicate rings: storage, then TTL + functions through v4_dedup_flush_origin.
    storage = source[source.index('// EspNowV4Header, V4PayloadCmdResp, V4PayloadFile* moved to System_ESPNow_Wire.h'):
                     source.index('// Broadcast ACK tracking.')]
    flush = one(source, 'static void v4_dedup_flush_origin(')
    start = source.index('static constexpr uint32_t V4_DEDUP_TTL_MS')
    dedup = storage + source[start:source.index(flush) + len(flush)]

    policy = (source[source.index('using V4OpcodeHandler'):source.index('struct V4OpcodeEntry')]
              + one(source, 'struct V4OpcodeEntry') + ';')
    table = one(source, 'static const V4OpcodeEntry kV4HandlerTable[]') + ';'
    size = 'static constexpr size_t kV4HandlerTableSize = sizeof(kV4HandlerTable) / sizeof(kV4HandlerTable[0]);'
    handlers = sorted(set(re.findall(r'\{\s*ESPNOW_V4_TYPE_\w+\s*,[^,]*,\s*(\w+)\s*\}', table)))
    for name in REAL_HANDLERS:
        if name not in handlers:
            sys.exit(f'FAIL extraction: kV4HandlerTable does not name {name}')
    stubs = '\n'.join(f'static void {name}(const V4RxCtx& ctx) {{ noteHandler("{name}", ctx); }}'
                      for name in handlers if name not in REAL_HANDLERS)

    # Peer slots. The reclaim and release helpers exist only after the fix; a
    # tree without them still compiles and is judged by the behavioral cases.
    have_reclaim = RECLAIM in source
    slots = [one(source, 'static bool isPairedDevice('),
             one(source, 'void removeEspNowDevice(')]
    if have_reclaim:
        slots.append('static int meshPeerMetaReclaimSlot(const uint8_t* forMac);')
    slots += [one(source, 'static void processMetadata('),
              one(source, 'MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing)'),
              one(source, 'MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing)')]
    if have_reclaim:
        slots.append(last(source, RECLAIM))
    unpair = one(source, 'const char* cmd_espnow_unpair(')
    if RELEASE in source:
        slots.append(one(source, RELEASE))
        release = 'static void releasePeerSlotsForTest(const uint8_t* mac) { meshPeerSlotsRelease(mac); }'
    else:
        if unpair.count(UNPAIR_SLOT_COMMENT) != 1:
            sys.exit('FAIL extraction: cmd_espnow_unpair has neither meshPeerSlotsRelease nor the inline slot release')
        inline = extract_block(unpair[unpair.index(UNPAIR_SLOT_COMMENT):], 'if (gMeshPeers)')
        release = 'static void releasePeerSlotsForTest(const uint8_t* mac) {\n  ' + inline + '\n}'
    slots += [one(source, 'void noteMeshPeerRxActivity(const uint8_t* mac, EspNowMeshRxKind kind, int8_t hbRssi, int8_t linkRssi'),
              release]

    relay = [one(source, 'static inline bool v4_flood_type_allowed('),
             last(source, 'static bool v4_routed_type_allowed('),
             one(source, 'static void v4_relay_maybe_forward('),
             one(source, 'static void v4h_relay_data(')]
    incoming = source[source.rindex('static bool v4_try_handle_incoming('):]
    incoming = extract_block(incoming, 'static bool v4_try_handle_incoming(')

    harness = (HERE / 'espnow_rx_frontend_harness.cpp').read_text()
    for tag, value in [('// INSERT_WIRE', wire_types),
                       ('// INSERT_PEER_TYPES', peer_types),
                       ('// INSERT_CRC', one(source, 'static uint16_t v4_crc16_ccitt(')),
                       ('// INSERT_DEDUP', dedup),
                       ('// INSERT_SLOTS', '\n\n'.join(slots)),
                       ('// INSERT_POLICY', policy),
                       ('// INSERT_RELAY', '\n\n'.join(relay)),
                       ('// INSERT_HEARTBEAT', one(source, 'static void v4h_heartbeat(')),
                       ('// INSERT_HANDLER_STUBS', stubs),
                       ('// INSERT_TABLE', table + '\n' + size),
                       ('// INSERT_LOOKUP', one(source, 'static const V4OpcodeEntry* v4_dispatch_lookup(')),
                       ('// INSERT_DISPATCH', one(source, 'static bool v4_dispatch_table_try(')),
                       ('// INSERT_OUTCOME', one(source, 'enum class V4ReasmOutcome') + ';'),
                       ('// INSERT_INCOMING', incoming)]:
        if harness.count(tag) != 1:
            sys.exit(f'FAIL: harness marker {tag} must appear exactly once')
        harness = harness.replace(tag, value)

    failures = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-rx-frontend-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        (unit.parent / 'System_MacText.h').write_text((HERE.parents[1] / 'System_MacText.h').read_text())
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter',
               '-DENABLE_BONDED_MODE=1', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        sys.stdout.flush()
        if subprocess.run([str(exe)], timeout=60).returncode:
            failures.append('the receive front-end cases reported failures')

    # Source contracts (the behavioral run above is the evidence on a tree that
    # predates the helpers).
    if 'v4_dedup_frame_seen(recv_info->src_addr, h, wasAuthenticated' not in incoming:
        failures.append('v4_try_handle_incoming does not choose the duplicate ring by what the frame proved')
    if 'meshPeerSlotsRelease(mac)' not in unpair:
        failures.append('cmd_espnow_unpair does not release the peer slots through meshPeerSlotsRelease')
    for line in failures:
        print(f'FAIL: {line}')
    if failures:
        sys.exit(1)
    print('PASS espnow_rx_frontend')


if __name__ == '__main__':
    main()
