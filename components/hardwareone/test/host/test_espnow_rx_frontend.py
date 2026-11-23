#!/usr/bin/env python3
"""Run the production ESP-NOW V4 receive front end against forged, duplicate and relayed frames.

Extracts from System_ESPNow.cpp the whole of v4_try_handle_incoming, the
duplicate rings and their functions, the opcode table with v4_dispatch_lookup
and v4_dispatch_table_try, the real v4h_heartbeat and v4h_relay_data, the flood
forwarder (v4_relay_maybe_forward) with both relay allow-lists and the relay hop list, the peer slot
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
tagged or sealed frame (a plaintext one stops at the plaintext gate); plaintext
frames create no health or metadata slot, and neither does a captured group-tagged HEARTBEAT replayed
from an invented radio MAC; unpair releases the metadata slot; a full metadata table
gives a slot to a newly paired peer and never takes one from a paired peer.
The 1.0 ACK frame (step 5a): a sealed frame with ACK_REQ gets one sealed ACK
in its session, a group-tagged frame one tagged ACK, each naming msgId and
fragment in the 8-byte payload; an ACK is never answered; no plaintext frame
is ever ACKed (all 256 opcodes), and only the six handshake frames pass the
plaintext gate; a waiter takes only its peer's ACK in the class it expects (a
tagged ACK where a sealed one is expected, another device's sealed ACK and a
tagged ACK whose origin is not its radio sender are refused and counted); an
ACK with a status reaches the waiter as a refusal; malformed ACKs are dropped.
The relay hop rule (1.0): a hop passes on any inner opcode but the exact list
of 1.0 link-local opcodes (unknown and later opcodes included, never a range),
keeps its version, magic, ttl, route and meshRelay checks, and the final
destination refuses what it does not take routed and counts routedDropPolicy.

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
GROUP_MARKERS = (
    'static espnowgroup::Membership groupMembershipOf(',
    'static void groupMembershipStore(',
    'static EspNowDevice* groupRecordOf(',
    'static bool groupSnapshotOf(',
)
ANNOUNCE_REGION = ('// ---- Room and tag membership: what a peer announces -----',
                   '// ---- Room and tag membership: acceptance -----')
HOP_RULE = 'static bool v4_relay_hop_refused(uint8_t type)'
UNPAIR_SLOT_COMMENT = "// Clear the peer's mesh-liveness slot regardless of mode."



def verdict_src(rxctx_text):
    """System_ESPNow_Verdict.h (1.0 step 5b) as text, for a harness that pastes
    V4RxCtx in: its helpers return V4Verdict. Empty for a tree without it."""
    p = Path(__file__).resolve().parents[2] / 'System_ESPNow_Verdict.h'
    if 'System_ESPNow_Verdict.h' not in rxctx_text or not p.exists():
        return ''
    return p.read_text() + '\n'

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
    dup_flag = re.search(r'^#define ESPNOW_V4_ACK_FLAG_DUPLICATE\b.*$', wire, re.MULTILINE)
    if dup_flag:
        defines.append(dup_flag.group(0))
    # The receive table's minimum lengths (one per opcode).
    defines += re.findall(r'^#define ESPNOW_V4_MINLEN_\w+\b.*$', wire, re.MULTILINE)
    hops = re.search(r'^#define MESH_ROUTE_MAX_HOPS\b.*$', router, re.MULTILINE)
    if not hops:
        sys.exit('FAIL extraction: MESH_ROUTE_MAX_HOPS not found in System_ESPNow_Router.h')
    wire_types = '\n'.join(defines + [hops.group(0)] + [one(wire, marker) + ';' for marker in (
        'enum EspNowV4Type', 'enum EspNowV4Flags',
        'struct __attribute__((packed)) EspNowV4Header',
        'struct __attribute__((packed)) V4PayloadRelayData',
        'struct __attribute__((packed)) V4PayloadHeartbeat',
        'struct __attribute__((packed)) V4PayloadMetadata',
        'struct __attribute__((packed)) V4PayloadAck')])

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
        verdict_src(rxctx) + one(rxctx, 'struct V4RxCtx') + ';',
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
    handlers = sorted(set(re.findall(r'\{\s*ESPNOW_V4_TYPE_\w+\s*,[^,]*,\s*ESPNOW_V4_MINLEN_\w+\s*,\s*(\w+)\s*\}', table)))
    for name in REAL_HANDLERS:
        if name not in handlers:
            sys.exit(f'FAIL extraction: kV4HandlerTable does not name {name}')
    stubs = '\n'.join(f'static V4Verdict {name}(const V4RxCtx& ctx) {{ noteHandler("{name}", ctx); '
                      f'return stubVerdict("{name}", ctx); }}'
                      for name in handlers if name not in REAL_HANDLERS)

    # Peer slots. The reclaim and release helpers exist only after the fix; a
    # tree without them still compiles and is judged by the behavioral cases.
    have_reclaim = RECLAIM in source
    slots = [one(source, 'static bool isPairedDevice('),
             one(source, 'void removeEspNowDevice(')]
    if have_reclaim:
        slots.append('static int meshPeerMetaReclaimSlot(const uint8_t* forMac);')
    # processMetadata hands a paired peer's room and tags to the announcement
    # part of room and tag membership (test_espnow_group_membership.py); a tree
    # before it has none.
    if ANNOUNCE_REGION[0] in source:
        if source.count(ANNOUNCE_REGION[0]) != 1 or source.count(ANNOUNCE_REGION[1]) != 1:
            sys.exit('FAIL extraction: the announcement region is not unique in System_ESPNow.cpp')
        slots.append('#include "System_ESPNow_GroupCore.h"  // production rules (copied beside this unit)')
        slots += [one(source, marker) for marker in GROUP_MARKERS]
        slots.append(source[source.index(ANNOUNCE_REGION[0]):source.index(ANNOUNCE_REGION[1])])
    slots += [one(source, 'static void processMetadata('),
              one(source, 'MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing)'),
              one(source, 'MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing)')]
    if have_reclaim:
        slots.append(last(source, RECLAIM))
    # The unpair teardown (shared by espnowunpair and espnowrevoke), or the
    # command itself on a tree before it.
    unpair = one(source, 'static bool espnowUnpairTeardown(' if 'static bool espnowUnpairTeardown(' in source
                 else 'const char* cmd_espnow_unpair(')
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

    relay = [one(source, 'static bool v4_group_tag('),
             one(source, 'static inline bool v4_flood_type_allowed('),
             last(source, 'static bool v4_routed_type_allowed(')]
    # The relay hop rule (1.0, W1): a tree before it has none and is judged
    # by the behavioral cases.
    if HOP_RULE in source:
        relay.append(one(source, HOP_RULE))
    relay += [one(source, 'static void v4_relay_maybe_forward('),
              one(source, 'static V4Verdict v4h_relay_data(')]
    incoming = source[source.rindex('static bool v4_try_handle_incoming('):]
    incoming = extract_block(incoming, 'static bool v4_try_handle_incoming(')
    # The must-understand drop counter (1.0 header flag classes), where the tree has it.
    must = re.search(r'^static uint32_t gEspNowRxMustUnderstandDrops = 0;', source, re.MULTILINE)
    if must:
        incoming = '#define HW1_MUST_UNDERSTAND 1\n' + must.group(0) + '\n\n' + incoming
    # The revoked-list drop counter (1.0 step 3), where the tree has it.
    revoked = re.search(r'^static uint32_t gEspNowRxRevokedDrops = 0;', source, re.MULTILINE)
    if revoked:   # ahead of the relay handler, which counts too
        relay.insert(0, '#define HW1_REVOKED_GATES 1\n' + revoked.group(0))

    # The mesh gate's counters and the function that counts and warns.
    counters_at = source.find('static uint32_t gEspNowRxUnknownMeshIdDrops')
    counters_end = source.find('static uint8_t gEspNowRxUnknownMeshIdLastMac[6] = {};')
    if counters_at < 0 or counters_end < counters_at:
        sys.exit('FAIL extraction: the unknown-mesh-id counters are not in System_ESPNow.cpp')
    gate = (source[counters_at:counters_end] + 'static uint8_t gEspNowRxUnknownMeshIdLastMac[6] = {};\n\n'
            + one(source, 'static void espnowNoteUnknownMeshId('))
    # The 1.0 ACK (step 5a): the plaintext rule, the queued sealed/tagged ACK
    # (into the espnowtx::sendAck double), the fragment waiter record and the
    # ACK handler v4_rx_ack.
    ack_wait = one(source, 'struct V4FragAckWait') + ';'
    ack = '\n\n'.join([one(source, 'static bool v4_queue_ack('),
                        one(source, 'static void v4_rx_ack(')])
    plaintext_rule = one(source, 'static inline bool v4_plaintext_type_allowed(')
    relay.insert(0, plaintext_rule)
    # The real reassembler (1.0 step 5b answers every fragment): its constants,
    # entry, table pointer, helpers, v4_reasm_accept and the dispatcher's step.
    reasm_constants = re.findall(r'^#define V4_(?:MAX_FRAGMENT_PAYLOAD|FRAG_MAX|REASM_MAX_MSG|REASM_MAX|'
                                 r'REASM_TIMEOUT_MS)\b.*$', source, re.MULTILINE)
    if len(reasm_constants) != 5:
        sys.exit(f'FAIL extraction: expected five V4 reassembly #defines, found {len(reasm_constants)}')
    table_ptr = re.search(r'^static V4ReasmEntry\* gV4Reasm = nullptr;.*$', source, re.MULTILINE)
    reasm = '\n\n'.join(reasm_constants + [one(source, 'struct V4ReasmEntry') + ';', table_ptr.group(0)] + [
        one(source, m) for m in ('static void v4_reasm_reset(V4ReasmEntry& e)', 'static void v4_reasm_gc(uint32_t nowMs)',
                                 'static V4ReasmEntry* v4_reasm_find_or_alloc(', 'static void v4_reasm_release(',
                                 'static V4ReasmOutcome v4_reasm_accept(', 'static bool v4_reasm_step(')])
    harness = (HERE / 'espnow_rx_frontend_harness.cpp').read_text()
    for tag, value in [('// INSERT_WIRE', wire_types),
                       ('// INSERT_PEER_TYPES', peer_types),
                       ('// INSERT_CRC', one(source, 'static uint16_t v4_crc16_ccitt(')),
                       ('// INSERT_DEDUP', dedup),
                       ('// INSERT_SLOTS', '\n\n'.join(slots)),
                       ('// INSERT_POLICY', policy),
                       ('// INSERT_RELAY', '\n\n'.join(relay)),
                       ('// INSERT_HEARTBEAT', one(source, 'static V4Verdict v4h_heartbeat(')),
                       ('// INSERT_HANDLER_STUBS', stubs),
                       ('// INSERT_TABLE', table + '\n' + size),
                       ('// INSERT_LOOKUP', one(source, 'static const V4OpcodeEntry* v4_dispatch_lookup(')),
                       ('// INSERT_DISPATCH', one(source, 'static V4Verdict v4_dispatch_table_try(')),
                       ('// INSERT_OUTCOME', one(source, 'enum class V4ReasmOutcome') + ';'),
                       ('// INSERT_REASM', reasm),
                       ('// INSERT_REFUSAL_HELPER', one(source, 'static V4Verdict v4_policy_refusal(')),
                       ('// INSERT_MESH_ID_GATE', gate),
                       ('// INSERT_ACK_WAIT', ack_wait),
                       ('// INSERT_ACK', ack),
                       ('// INSERT_INCOMING', incoming)]:
        if harness.count(tag) != 1:
            sys.exit(f'FAIL: harness marker {tag} must appear exactly once')
        harness = harness.replace(tag, value)

    failures = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-rx-frontend-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        (unit.parent / 'System_MacText.h').write_text((HERE.parents[1] / 'System_MacText.h').read_text())
        (unit.parent / 'System_DeviceName.h').write_text((HERE.parents[1] / 'System_DeviceName.h').read_text())
        (unit.parent / 'System_ESPNow_AnswerCore.h').write_text(
            (HERE.parents[1] / 'System_ESPNow_AnswerCore.h').read_text())
        (unit.parent / 'System_ESPNow_HandshakeCore.h').write_text(
            (HERE.parents[1] / 'System_ESPNow_HandshakeCore.h').read_text())
        (unit.parent / 'System_ESPNow_GroupCore.h').write_text(
            (HERE.parents[1] / 'System_ESPNow_GroupCore.h').read_text())
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
    if 'v4_accept_key(recv_info->src_addr, h, wasAuthenticated' not in incoming:
        failures.append('v4_try_handle_incoming does not choose the accept-memory ring by what the frame proved')
    # Secrets are compared in constant time: the group tag (the harness maps
    # sodium_memcmp to memcmp, so only the source can show which one is called)
    # and the bond session token.
    if 'sodium_memcmp(expectedTag, receivedTag, ESPNOW_V4_BROADCAST_AUTH_TAG_LEN) != 0' not in incoming:
        failures.append('v4_try_handle_incoming does not compare the group tag with sodium_memcmp')
    token = source[source.find('static bool validateBondSessionToken('):]
    token = token[:token.find('\n}\n')]
    if 'sodium_memcmp(token, s->bondToken, 16)' not in token or re.search(r'(?<!sodium_)memcmp\(token', token):
        failures.append('validateBondSessionToken does not compare the bond token with sodium_memcmp')
    if 'meshPeerSlotsRelease(mac)' not in unpair:
        failures.append('the unpair teardown does not release the peer slots through meshPeerSlotsRelease')
    # Revocation (1.0 step 3), the parts outside this harness: the SESSION_UNKNOWN
    # hint goes to registry peers only, and the router takes no advert from, and
    # installs no route to, a revoked device.
    hint = extract_block(read('System_ESPNow_Sessions.cpp'), 'bool sessionUnknownFrameSeen(')
    if 'espnowIsRegistryPeer(srcMac)' not in hint:
        failures.append('sessionUnknownFrameSeen answers a sender that is not in the registry')
    advert = extract_block(read('System_ESPNow_Router.cpp'), 'void meshRouterIngestAdvert(')
    if advert.count('espnowRevokedMac(') != 2:
        failures.append('meshRouterIngestAdvert does not refuse revoked advertisers and revoked destinations')
    heartbeat = extract_block(source[source.rindex('bool v4_broadcast_heartbeat(uint16_t flags'):],
                              'bool v4_broadcast_heartbeat(uint16_t flags')
    if 'espnowRevokedMac(gMeshPeers[i].mac)' not in heartbeat:
        failures.append('v4_broadcast_heartbeat may send a heartbeat to a revoked device')
    # The 1.0 ACK frame (step 5a), the parts outside this harness: the plaintext
    # ACK senders are gone, the ACK builder seals with the REKEYING exception or
    # group-tags, no frame but the handshake is ever sent plaintext, the
    # handshake frames carry no ACK_REQ, and TOPO_* is group-tagged.
    for gone in ('v4_send_ack(', 'v4_send_frag_ack('):
        if gone in source:
            failures.append(f'the plaintext ACK path {gone[:-1]} is still in System_ESPNow.cpp')
    emit = extract_block(source, 'bool v4_emit_ack(const uint8_t* dst, uint16_t classFlag')
    if 'sessionWrapAck(s, h, ack' not in emit or 'ESPNOW_V4_FLAG_BROADCAST_AUTH, generateMessageId()' not in emit:
        failures.append('v4_emit_ack does not seal with sessionWrapAck or tag the ACK under its own msgId')
    frame_ex = extract_block(source[source.rindex('static bool v4_send_frame_ex('):], 'static bool v4_send_frame_ex(')
    if '!v4_plaintext_type_allowed(type)' not in frame_ex or 'strip the flag' in frame_ex:
        failures.append('v4_send_frame_ex can still put a non-handshake frame on the air plaintext')
    if 'ESPNOW_V4_FLAG_ACK_REQ' in read('System_ESPNow_Handlers_Crypto.cpp'):
        failures.append('a handshake frame (System_ESPNow_Handlers_Crypto.cpp) still asks for an ACK')
    for topo in ('TOPO_REQ', 'TOPO_START', 'TOPO_PEER'):
        if not re.search(r'v4_send_frame\(dst, ESPNOW_V4_TYPE_' + topo + r', ESPNOW_V4_FLAG_BROADCAST_AUTH,', source):
            failures.append(f'{topo} is not sent group-tagged')
    # Review of step 5a: the ACKs queued while a fragmented send waits go out
    # from inside the wait (on the dispatcher), and a refusal ACK counts the
    # session as alive.
    attempt = 'static V4SendResult v4_send_chunked_attempt('
    chunked = (extract_block(source, attempt) if attempt in source else
               extract_block(source[source.rindex('bool v4_send_encrypted_chunked('):], 'bool v4_send_encrypted_chunked('))
    wait = chunked[chunked.find('while ((millis() - waitStart) < ACK_TIMEOUT_MS)'):]
    if 'espnowtx::drainAcksOnDispatcher();' not in wait[:wait.find('vTaskDelay')]:
        failures.append('the fragment-wait loop of v4_send_encrypted_chunked does not send the queued ACKs')
    refused = extract_block(read('System_ESPNow_Sessions.cpp'), 'void sendStatusMarkRefused(')
    if 'SEND_STATUS_FAILED' not in refused or 'sessionNoteSendResult(s->peerMac, /*delivered=*/true)' not in refused:
        failures.append('sendStatusMarkRefused does not mark the send failed and the session alive')
    for line in failures:
        print(f'FAIL: {line}')
    if failures:
        sys.exit(1)
    print('PASS espnow_rx_frontend')


if __name__ == '__main__':
    main()
