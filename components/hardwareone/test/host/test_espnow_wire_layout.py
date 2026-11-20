#!/usr/bin/env python3
"""The 1.0 frame rules of System_ESPNow_Wire.h, checked layout by layout.

1.0 is a full-erase install; after it a format only grows at its end. This
suite pins what 1.0 ships so a later change to a layout is a decision, not an
accident.

Compiled (System_ESPNow_Wire.h itself, with its four rules headers, over a
stub Arduino.h and build config): every static_assert in the header holds on
the host, and the layouts equal the golden table below, field by field.

Source rules:
  * every struct in System_ESPNow_Wire.h is __attribute__((packed));
  * every struct has a static_assert on its size and an offsetof() assert on
    every field it declares;
  * the header states little-endian once and asserts it;
  * CapabilitySummary and BondPeerStatus live in System_ESPNow_Wire.h and no
    longer in System_ESPNow.h;
  * one receive style: no receive code casts a payload to a wire struct
    pointer (System_ESPNow*.cpp; the FS list entries read in place are the one
    allow-listed exception), and no handler compares ctx.payloadLen with a
    sizeof or subtracts from it (the receive table's minLen does that; copies
    of a text tail into a local buffer are allowed);
  * fixed-header frames (a payload that ends in data or fixed-stride entries)
    never grow: the rules block and docs/COMPATIBILITY.md name them, each
    struct is marked, and every ctx.tail() read starts after one of them;
  * sensor masks are 32 bits: no 16-bit field or variable named *mask/*Mask in
    the sensor and forwarding code (the peer bitmap receivedPeerMask, which is
    not a sensor mask and never on the wire, is allowed);
  * back-capture: 82-85 file catch-up are allocated (2026-11-19) with the
    four CATCHUP_* layouts pinned above, their minLens and the core's
    numbers; 156-158 BACKLOG_* stay earmarked (named in the opcode map and at
    the enum, unallocated); 86 and 159 stay free; the wire stream-id table
    (0-127 sensor type, 0xF0 battery, 0xF1 events) is in the header, and
    docs/COMPATIBILITY.md states them.

Options: --cxx <compiler>, --component <dir> (--sanitize is accepted and unused).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# The 1.0 layouts: struct -> (size, [(field, offset), ...]). Part B (names) and
# Part C (session capabilities) change only the rows they own, in their commit.
GOLDEN = {
    'EspNowV4Header': (32, [('magic', 0), ('ver', 2), ('type', 3), ('flags', 4), ('headerLen', 6),
                            ('reserved1', 7), ('msgId', 8), ('origin', 12), ('ttl', 18), ('fragIndex', 19),
                            ('fragCount', 20), ('reserved2', 21), ('meshFingerprint', 22), ('sessionId', 24),
                            ('frameSeq', 26), ('crc16', 30)]),
    'V4PayloadRelayData': (8, [('finalDst', 0), ('hopsUsed', 6), ('reserved', 7)]),
    'V4PayloadPeerListHeader': (4, [('entryCount', 0), ('role', 1), ('seq', 2), ('reserved', 3)]),
    'V4PayloadPeerListEntry': (8, [('mac', 0), ('hops', 6), ('metric', 7)]),
    'V4PayloadHeartbeat': (44, [('role', 0), ('peerCount', 1), ('rssi', 2), ('chanInfo', 3), ('uptimeSec', 4),
                                ('freeHeap', 8), ('deviceName', 12)]),
    'V4PayloadPairBeacon': (36, [('role', 0), ('flags', 1), ('reserved', 2), ('deviceName', 4)]),
    'V4PayloadPairCtrl': (40, [('targetMac', 0), ('role', 6), ('reserved', 7), ('deviceName', 8)]),
    'V4PayloadTimeSync': (16, [('epochMs', 0), ('source', 8), ('flags', 9), ('sinceSetMin', 10), ('reserved', 12)]),
    'V4PayloadTopoReq': (8, [('reqId', 0), ('reserved', 4)]),
    'V4PayloadAnswerHeader': (5, [('reqId', 0), ('status', 4)]),
    'V4PayloadTopoStart': (8, [('reqId', 0), ('status', 4), ('peerCount', 5), ('reserved', 6)]),
    'V4PayloadTopoPeer': (47, [('reqId', 0), ('status', 4), ('peerIndex', 5), ('isLast', 6), ('mac', 7),
                               ('rssi', 13), ('encrypted', 14), ('name', 15)]),
    'V4PayloadBondHeartbeat': (24, [('role', 0), ('rssi', 1), ('reserved', 2), ('uptimeSec', 4), ('freeHeap', 8),
                                    ('seqNum', 12), ('bootCounter', 16), ('settingsHash', 20)]),
    'V4PayloadStreamCtrl': (4, [('sensorType', 0), ('enable', 1), ('reserved', 2)]),
    'V4PayloadBondRequest': (8, [('role', 0), ('flags', 1), ('seq', 2), ('reserved', 4)]),
    'V4PayloadBondNotice': (8, [('reason', 0), ('role', 1), ('seq', 2), ('flags', 4), ('reserved', 5)]),
    'CapabilitySummary': (84, [('reserved', 0), ('role', 1), ('inputDeviceType', 2), ('chipModel', 3),
                               ('featureMask', 4), ('serviceMask', 8), ('sensorMask', 12), ('mac', 16),
                               ('wifiChannel', 22), ('reserved0', 23), ('flashSizeMb', 24), ('psramSizeMb', 28),
                               ('uptimeSeconds', 32), ('fwHash', 36), ('deviceName', 52)]),
    'BondPeerStatus': (28, [('uptimeSec', 0), ('freeHeap', 4), ('minFreeHeap', 8), ('sensorEnabledMask', 12),
                            ('sensorConnectedMask', 16), ('wifiConnected', 20), ('bluetoothActive', 21),
                            ('httpActive', 22), ('bondHandshakeState', 23), ('reserved', 24)]),
    'V4PayloadSensorStatus': (16, [('presentMask', 0), ('streamingMask', 4), ('bootTag', 8), ('fwdLink', 10),
                                   ('reserved', 11)]),
    'V4PayloadSensorReading': (12, [('sensorType', 0), ('flags', 1), ('bootTag', 2), ('counter', 4), ('parts', 6),
                                    ('fwdLink', 7), ('ageMs', 8)]),
    'V4PayloadSensorReq': (20, [('reqId', 0), ('mode', 4), ('fwdLink', 5), ('reserved', 6), ('sensorMask', 8),
                                ('intervalMs', 12), ('leaseMs', 16)]),
    'V4PayloadPowerStatus': (24, [('counter', 0), ('bootTag', 2), ('status', 4), ('backend', 5), ('known', 6),
                                  ('voltageMv', 8), ('percentX10', 10), ('rateX10', 12), ('sampleAgeMs', 14),
                                  ('fwdLink', 16), ('reserved', 17)]),
    'V4PayloadEventPush': (128, [('counter', 0), ('bootTag', 2), ('flags', 4), ('tier', 5), ('source', 6),
                                 ('fwdLink', 7), ('skipped', 8), ('reserved1', 10), ('eventSeq', 12), ('ageMs', 16),
                                 ('kind', 20), ('subject', 52), ('detail', 84), ('who', 116)]),
    'V4PayloadForwardLink': (96, [('op', 0), ('linkGen', 1), ('viaBootTag', 2), ('streams', 4), ('result', 5),
                                  ('answers', 6), ('announceGen', 7), ('partnerMac', 8), ('reserved1', 14),
                                  ('sensorMask', 16), ('partnerKey', 20), ('partnerName', 52), ('reserved2', 84)]),
    'V4PayloadMetadata': (202, [('deviceName', 0), ('friendlyName', 32), ('room', 80), ('zone', 112), ('tags', 144),
                                ('stationary', 198), ('reserved', 199)]),
    'V4PayloadCmdRespHeader': (5, [('reqId', 0), ('status', 4)]),
    'V4PayloadStreamHeader': (5, [('reqId', 0), ('status', 4)]),
    'V4PayloadKeyExHello': (72, [('meshFingerprint', 0), ('senderMac', 2), ('senderPubEd25519', 8), ('hmac', 40)]),
    'V4PayloadKeyExReply': (72, [('meshFingerprint', 0), ('responderMac', 2), ('responderPubEd25519', 8),
                                 ('hmac', 40)]),
    'V4PayloadKeyExConfirm': (18, [('meshFingerprint', 0), ('confirmerMac', 2), ('status', 8), ('reserved', 9),
                                   ('pubFingerprint', 10)]),
    'V4PayloadSessionOpen': (130, [('sessionId', 0), ('initiatorMac', 2), ('responderMac', 8), ('ephX25519Pub', 14),
                                   ('nonceA', 46), ('sessionCaps', 62), ('signature', 66)]),
    'V4PayloadSessionConfirm': (146, [('sessionId', 0), ('responderMac', 2), ('initiatorMac', 8),
                                      ('ephX25519Pub', 14), ('nonceA', 46), ('nonceB', 62), ('sessionCaps', 78),
                                      ('signature', 82)]),
    'V4PayloadSessionRekey': (130, [('sessionId', 0), ('senderMac', 2), ('receiverMac', 8), ('newEphX25519Pub', 14),
                                    ('nonceRekey', 46), ('prevTxSeqAtRekey', 62), ('signature', 66)]),
    'V4PayloadSubscribe': (16, [('requestedEvents', 0), ('reserved', 4)]),
    'V4PayloadFileStart': (72, [('fileSize', 0), ('chunkCount', 4), ('chunkSize', 6), ('filename', 8)]),
    'V4PayloadFileData': (202, [('chunkIndex', 0), ('data', 2)]),
    'V4PayloadFileEnd': (5, [('crc32', 0), ('status', 4)]),
    'V4PayloadFileCancel': (5, [('reqId', 0), ('status', 4)]),
    'V4PayloadFsListReq': (144, [('reqId', 0), ('startIndex', 4), ('maxEntries', 6), ('path', 8), ('reserved', 136)]),
    'V4PayloadFsListReplyHeader': (140, [('reqId', 0), ('status', 4), ('entryCount', 5), ('hasMore', 6),
                                         ('reserved', 7), ('totalEntries', 8), ('nextStartIndex', 10), ('path', 12)]),
    'V4PayloadFsEntry': (76, [('name', 0), ('size', 64), ('isFolder', 68), ('perms', 69), ('reserved', 70)]),
    'V4PayloadFsStatReq': (140, [('reqId', 0), ('path', 4), ('reserved', 132)]),
    'V4PayloadFsStatReply': (164, [('reqId', 0), ('status', 4), ('reserved1', 5), ('totalBytes', 8),
                                   ('usedBytes', 16), ('freeBytes', 24), ('percentUsedX10', 32), ('reserved2', 34),
                                   ('path', 36)]),
    'V4PayloadFsGetReq': (140, [('reqId', 0), ('path', 4), ('reserved', 132)]),
    'V4PayloadFsGetAck': (140, [('reqId', 0), ('status', 4), ('reserved', 5), ('fileSize', 8), ('path', 12)]),
    'V4PayloadSessionUnknown': (12, [('sessionId', 0), ('reserved', 2), ('frameSeq', 4), ('msgId', 8)]),
    # File catch-up (82-85, 2026-11-19). The two answers carry headerLen (and
    # LIST entries entryLen / entryFixed), so they grow; not fixed-header.
    'V4PayloadCatchupListReq': (16, [('reqId', 0), ('fromFileId', 4), ('kindMask', 8), ('link', 10),
                                     ('maxEntries', 11), ('maxFrame', 12), ('reserved', 13)]),
    'V4PayloadCatchupListRespHeader': (24, [('reqId', 0), ('status', 4), ('link', 5), ('entryCount', 6),
                                            ('headerLen', 7), ('nextFileId', 8), ('ledgerGen', 12),
                                            ('ledgerId', 16), ('bootTag', 20), ('flags', 22), ('entryFixed', 23)]),
    'V4PayloadCatchupReadReq': (24, [('reqId', 0), ('fileId', 4), ('offset', 8), ('maxBytes', 12), ('link', 16),
                                     ('mode', 17), ('maxFrame', 18), ('flags', 19), ('ledgerId', 20)]),
    'V4PayloadCatchupReadRespHeader': (20, [('reqId', 0), ('status', 4), ('flags', 5), ('link', 6),
                                            ('headerLen', 7), ('fileId', 8), ('offset', 12), ('end', 16)]),
}

# Status values on the wire (rule F3: answers share one status table, 0 ok,
# 1-15 shared, 16+ per frame; System_ESPNow_AnswerCore.h). Frozen at 1.0.
STATUS_GOLDEN = {
    'FS_LIST_STATUS_OK': 0, 'FS_LIST_STATUS_TOO_BUSY': 1, 'FS_LIST_STATUS_PERM_DENIED': 2,
    'FS_LIST_STATUS_NOT_READY': 7, 'FS_LIST_STATUS_IO_ERROR': 8, 'FS_LIST_STATUS_NOT_FOUND': 9,
    'FS_LIST_STATUS_NOT_A_DIR': 16,
    'FILE_CANCEL_REJECTED': 2, 'FILE_CANCEL_WRITE_FAILED': 8, 'FILE_CANCEL_TIMEOUT': 10,
    'FILE_CANCEL_INCOMPLETE': 16, 'FILE_CANCEL_CRC_MISMATCH': 17,
    'espnowanswer::kOk': 0, 'espnowanswer::kBusy': 1, 'espnowanswer::kRefused': 2,
    'espnowanswer::kUnsupported': 3, 'espnowanswer::kTooLarge': 4, 'espnowanswer::kNoSlot': 5,
    'espnowanswer::kBadRequest': 6, 'espnowanswer::kNotReady': 7, 'espnowanswer::kIoError': 8,
    'espnowanswer::kNotFound': 9, 'espnowanswer::kTimeout': 10, 'espnowanswer::kCancelled': 11,
    'espnowanswer::kCmdFailed': 16,
    'espnowanswer::kCatchupNoLink': 16, 'espnowanswer::kCatchupGone': 16, 'espnowanswer::kCatchupTrimmed': 17,
    'espnowanswer::kCatchupPastEnd': 18,
}

CORE_HEADERS = ('System_ESPNow_SensorCore.h', 'System_ESPNow_PowerEventCore.h', 'System_ESPNow_ForwardCore.h',
                'System_ESPNow_TimeCore.h', 'System_ESPNow_AnswerCore.h', 'System_ESPNow_CatchupCore.h',
                'System_DeviceName.h')

# Receive code may read FS list entries in place (V4PayloadFsEntry is packed,
# alignment 1, and the count is checked first).
CAST_ALLOWED = ('reinterpret_cast<const V4PayloadFsEntry*>(payload + sizeof(hdr))',)
MASK_FILES = ('System_ESPNow_ForwardCore.h', 'System_ESPNow_SensorCore.h', 'System_ESPNow_ForwardRx.cpp',
              'System_ESPNow_BondForward.cpp', 'System_ESPNow_Sensors.cpp', 'OLED_Mode_Remote.cpp', 'WebPage_Bond.cpp',
              'System_ESPNow.h', 'System_ESPNow.cpp', 'System_ESPNow_Wire.h', 'System_Settings.h')
MASK_ALLOWED = ('receivedPeerMask',  # one bit per validated peer index, RAM only
                'kindMask')          # CATCHUP_LIST_REQ: file kinds 1-7, not sensors

# Fixed-header frames: the payload ends in data running to the end or in a run
# of fixed-size entries, so an appended field would be read as data by an older
# receiver. They never grow; the rules block names them and each struct is marked.
FIXED_HEADER = {
    'V4PayloadRelayData': 'RELAY_DATA', 'V4PayloadPeerListHeader': 'PEER_LIST',
    'V4PayloadPeerListEntry': 'PEER_LIST', 'V4PayloadSensorReading': 'BOND_SENSOR_DATA',
    'V4PayloadCmdRespHeader': 'CMD_RESP', 'V4PayloadStreamHeader': 'STREAM', 'V4PayloadFileData': 'FILE_DATA',
    'V4PayloadFsListReplyHeader': 'FS_LIST_REPLY', 'V4PayloadFsEntry': 'FS_LIST_REPLY',
}
TEXT_ONLY_FRAMES = ('SENSOR_ENVELOPE', 'TEXT', 'BOOT', 'CMD', 'USER_SYNC')
FIXED_MARK = '// Fixed header: cannot grow'

# Sites whose limit follows from the group tag's length (System_ESPNow.cpp).
BUDGET_SITES = (
    ('the signed-frame fit check on send',
     r'payloadLen \+ ESPNOW_V4_BROADCAST_AUTH_TAG_LEN > ESPNOW_V4_MAX_PAYLOAD\)'),
    ('the flood forward size check',
     r'payloadLen \+ ESPNOW_V4_BROADCAST_AUTH_TAG_LEN \+ sizeof\(EspNowV4Header\) > 250\) return;'),
    ('the relay wrap limit', r'innerLen > ESPNOW_V4_RELAY_MAX_INNER_FRAME\)'),
    ('the PEER_LIST receive limit', r'hdr\.entryCount > ESPNOW_V4_PEER_LIST_MAX_ENTRIES\) return;'),
    ('the signed BOOT limit', r'len <= ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT\)'),
)

failures: list[str] = []


def expect(ok: bool, what: str) -> None:
    if not ok:
        failures.append(what)


def strip_comments(source: str) -> str:
    source = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), source, flags=re.S)
    return re.sub(r'//[^\n]*', '', source)


def struct_blocks(wire: str) -> dict[str, str]:
    """Every struct declared in the header: name -> body (comments removed)."""
    code = strip_comments(wire)
    out = {}
    for m in re.finditer(r'\bstruct\s+((?:__attribute__\(\(packed\)\)\s+)?)(\w+)\s*\{', code):
        expect(m.group(1) != '', f'System_ESPNow_Wire.h: struct {m.group(2)} is packed')
        depth, i = 1, m.end()
        while depth:
            depth += {'{': 1, '}': -1}.get(code[i], 0)
            i += 1
        out[m.group(2)] = code[m.end():i - 1]
    return out


def fields(body: str) -> list[str]:
    names = []
    for decl in body.split(';'):
        decl = decl.strip()
        if not decl:
            continue
        m = re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*$', decl)
        if m:
            names.append(m.group(1))
    return names


def check_asserts(wire: str, blocks: dict[str, str]) -> None:
    code = strip_comments(wire)
    for name, body in blocks.items():
        expect(re.search(rf'sizeof\({name}\)\s*==', code) is not None,
               f'{name}: a static_assert on its size')
        for field in fields(body):
            expect(re.search(rf'offsetof\({name},\s*{field}\)\s*==', code) is not None,
                   f'{name}.{field}: a static_assert on its offset')
    expect('static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__' in code,
           'System_ESPNow_Wire.h asserts little-endian')
    expect('little-endian' in wire.split('#include')[0], 'the header block states little-endian once')


def check_golden(blocks: dict[str, str]) -> str:
    """Golden-table checks; returns static_asserts for the host compile."""
    expect(sorted(blocks) == sorted(GOLDEN),
           f'the structs of System_ESPNow_Wire.h are the golden table\'s: '
           f'new {sorted(set(blocks) - set(GOLDEN))}, gone {sorted(set(GOLDEN) - set(blocks))}')
    lines = [f'static_assert({name} == {value}, "golden: {name} is {value} on the wire");'
             for name, value in STATUS_GOLDEN.items()]
    for name, (size, layout) in GOLDEN.items():
        if name not in blocks:
            continue
        expect([f for f, _ in layout] == fields(blocks[name]),
               f'{name}: fields {fields(blocks[name])} are the golden {[f for f, _ in layout]}')
        lines.append(f'static_assert(sizeof({name}) == {size}, "golden: {name} is {size} bytes");')
        for field, offset in layout:
            lines.append(f'static_assert(offsetof({name}, {field}) == {offset}, "golden: {name}.{field} at {offset}");')
    return '\n'.join(lines)


def compile_header(component: Path, cxx: str, golden: str) -> None:
    with tempfile.TemporaryDirectory(prefix='hw1-wire-layout-') as tmp:
        t = Path(tmp)
        (t / 'Arduino.h').write_text('#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n')
        (t / 'System_BuildConfig.h').write_text('#pragma once\n#define ENABLE_ESPNOW 1\n#define ENABLE_BONDED_MODE 1\n')
        for name in ('System_ESPNow_Wire.h',) + CORE_HEADERS:
            shutil.copy(component / name, t / name)
        unit = t / 'wire_layout.cpp'
        unit.write_text('#include <cstddef>\n#include "System_ESPNow_Wire.h"\n' + golden + '\nint main() { return 0; }\n')
        r = subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-I', str(t), str(unit)],
                           capture_output=True, text=True, timeout=120)
        expect(r.returncode == 0, 'System_ESPNow_Wire.h and the golden layouts compile on the host:\n' + r.stderr[-3000:])


def check_sources(component: Path) -> None:
    header = (component / 'System_ESPNow.h').read_text()
    wire = (component / 'System_ESPNow_Wire.h').read_text()
    for name in ('CapabilitySummary', 'BondPeerStatus'):
        expect(f'struct __attribute__((packed)) {name} {{' in wire, f'{name} is a packed struct in System_ESPNow_Wire.h')
        expect(re.search(rf'\bstruct\s+(?:__attribute__\(\(packed\)\)\s+)?{name}\s*\{{', strip_comments(header)) is None,
               f'{name} is no longer declared in System_ESPNow.h')
    # One receive style.
    cast = re.compile(r'\(const\s+(?:V4Payload\w*|CapabilitySummary|BondPeerStatus)\s*\*\)|'
                      r'reinterpret_cast<const\s+(?:V4Payload\w*|CapabilitySummary|BondPeerStatus)\s*\*>\s*\([^;]*')
    length = re.compile(r'ctx\.payloadLen\s*(?:<|<=|>|>=|==|!=)\s*sizeof|'
                        r'sizeof\([^)]*\)\s*(?:<|<=|>|>=|==|!=)\s*ctx\.payloadLen|ctx\.payloadLen\s*-')
    for path in sorted(component.glob('System_ESPNow*.cpp')):
        for no, line in enumerate(strip_comments(path.read_text()).splitlines(), 1):
            for m in cast.finditer(line):
                if not any(a in line for a in CAST_ALLOWED):
                    failures.append(f'{path.name}:{no}: payload cast to a wire struct pointer: {line.strip()}')
            if length.search(line) and 'copyLen' not in line:
                failures.append(f'{path.name}:{no}: a handler checks ctx.payloadLen itself '
                                f'(the receive table\'s minLen does): {line.strip()}')
    # Sensor masks are 32 bits.
    for name in MASK_FILES:
        code = strip_comments((component / name).read_text())
        for no, line in enumerate(code.splitlines(), 1):
            for m in re.finditer(r'\buint16_t\s+(\w*[Mm]ask)\b', line):
                if m.group(1) not in MASK_ALLOWED:
                    failures.append(f'{name}:{no}: 16-bit mask {m.group(1)} (sensor masks are 32 bits)')
            if re.search(r'\(uint16_t\)\s*\(?\s*1u?\s*<<', line) and 'peerIndex' not in line:
                failures.append(f'{name}:{no}: a 16-bit sensor bit: {line.strip()}')
    # Fixed-header frames are named in the rules block and marked at their struct,
    # and every tail read (ctx.tail) starts after one of their headers.
    rules = wire.split('#include')[0]
    expect('Fixed-header frames cannot grow' in rules, 'the rules block states that fixed-header frames cannot grow')
    for name, opcode in FIXED_HEADER.items():
        expect(re.search(rf'\b{name}\b', rules) is not None and re.search(rf'\b{opcode}\b', rules) is not None,
               f'the rules block lists {opcode} ({name}) as a fixed-header frame')
        expect(f'{FIXED_MARK}' in wire.split(f'struct __attribute__((packed)) {name} {{')[0].rsplit('\n', 2)[-2],
               f'{name} is marked "{FIXED_MARK[3:]}" on the line before it')
    for opcode in TEXT_ONLY_FRAMES:
        expect(re.search(rf'\b{opcode}\b', rules.split('Fixed-header frames cannot grow')[-1].split('Reserved bytes')[0])
               is not None, f'the fixed-header rule lists {opcode}')
    for name in sorted(set(re.findall(r'struct __attribute__\(\(packed\)\) (\w+) \{', wire)) - set(FIXED_HEADER)):
        expect(f'{FIXED_MARK}' not in wire.split(f'struct __attribute__((packed)) {name} {{')[0].rsplit('\n', 2)[-2],
               f'{name} is marked fixed-header but is not in this suite\'s list')
    for path in sorted(component.glob('System_ESPNow*.cpp')):
        for no, line in enumerate(strip_comments(path.read_text()).splitlines(), 1):
            for m in re.finditer(r'ctx\.tail\(\s*(?:sizeof|offsetof)\(\s*(\w+)', line):
                arg = m.group(1)
                # sizeof(local) names a variable: find its declared type on the lines above
                if arg not in FIXED_HEADER:
                    decl = re.findall(rf'\b(V4Payload\w+)\s+{arg}\b', strip_comments(path.read_text()))
                    arg = decl[-1] if decl else arg
                expect(arg in FIXED_HEADER, f'{path.name}:{no}: a tail read after {arg}, which is not a fixed-header '
                       f'frame (an appended field would be read as data)')
    # The budgets that depend on the 16-byte group tag are checked against the
    # named constants at every site, never a number written out (a 186- or
    # 130-byte literal left from the 32-byte tag would pass every static_assert).
    espnow = strip_comments((component / 'System_ESPNow.cpp').read_text())
    for what, pattern in BUDGET_SITES:
        expect(re.search(pattern, espnow) is not None, f'System_ESPNow.cpp: {what} uses the named budget')
    # Reserved bytes are sent as 0: a sender starts from a zeroed struct. A
    # payload filled field by field is declared "= {}", and the shared builders
    # clear the whole struct first.
    wire_types = r'(?:V4Payload\w+|CapabilitySummary|BondPeerStatus)'
    for path in sorted(component.glob('System_ESPNow*.cpp')):
        lines = [l for l in strip_comments(path.read_text()).splitlines()]
        for no, line in enumerate(lines):
            m = re.match(rf'\s*({wire_types})\s+(\w+)\s*;\s*$', line)
            if not m:
                continue
            nxt = next((l for l in lines[no + 1:] if l.strip()), '')
            if re.match(rf'\s*{m.group(2)}\.\w+(?:\[[^\]]*\])?\s*=', nxt):
                failures.append(f'{path.name}:{no + 1}: {m.group(1)} {m.group(2)} is filled field by field without '
                                f'being zeroed first (reserved bytes are sent as 0)')
    for builder, start in (('static void v4FillHeartbeat(V4PayloadHeartbeat& hb)', 'memset(&hb, 0, sizeof(hb));'),
                           ('static void buildMetadataPayload(V4PayloadMetadata* payload)',
                            'memset(payload, 0, sizeof(V4PayloadMetadata));'),
                           ('static void buildCapabilitySummary(CapabilitySummary& cap)',
                            'memset(&cap, 0, sizeof(CapabilitySummary));'),
                           ('void buildLocalBondStatus(BondPeerStatus& status)',
                            'memset(&status, 0, sizeof(BondPeerStatus));')):
        body = espnow[espnow.find(builder + ' {'):]
        body = body[:body.find('\n}\n')]
        expect(builder in espnow and start in body[:400], f'{builder.split("(")[0].split()[-1]} clears the struct first')
    compat = (component.parents[1] / 'docs' / 'COMPATIBILITY.md').read_text()
    expect('fixed-header frames' in compat and all(o in compat for o in set(FIXED_HEADER.values()) | set(TEXT_ONLY_FRAMES)),
           'docs/COMPATIBILITY.md states the fixed-header exception to the growth rule')
    core = (component / 'System_ESPNow_SensorCore.h').read_text()
    expect('using SensorMask = uint32_t;' in core and 'static_assert(REMOTE_SENSOR_MAX <= 30' in core,
           'System_ESPNow_SensorCore.h: espnowsensor::SensorMask is 32 bits, with room for the resume bits')
    settings = (component / 'System_Settings.h').read_text()
    expect(re.search(r'uint32_t\s+bondForwardSensors\s*;', settings) is not None,
           'System_Settings.h: bondForwardSensors is 32 bits')


# Back-capture (design W5): the outbox numbers are named in the map and at the
# enum, and stay unallocated until their frames land with their rows. File
# catch-up landed on 2026-11-19: 82-85 are allocated with their layouts.
EARMARKS = {156: 'BACKLOG_REQ', 157: 'BACKLOG_RESP', 158: 'BACKLOG_DATA'}
ALLOCATED = {82: 'CATCHUP_LIST_REQ', 83: 'CATCHUP_LIST_RESP', 84: 'CATCHUP_READ_REQ', 85: 'CATCHUP_READ_RESP'}
CATCHUP_MINLEN = {'CATCHUP_LIST_REQ': 16, 'CATCHUP_LIST_RESP': 24, 'CATCHUP_READ_REQ': 24, 'CATCHUP_READ_RESP': 20}
EARMARK_FREE = (86, 87, 88, 89, 159)


def check_earmarks(component: Path) -> None:
    wire = (component / 'System_ESPNow_Wire.h').read_text()
    code = strip_comments(wire)
    enum = code[code.index('enum EspNowV4Type'):]
    enum = enum[:enum.index('};')]
    taken = {int(v) for v in re.findall(r'ESPNOW_V4_TYPE_\w+\s*=\s*(\d+)', enum)}
    for number in sorted(set(EARMARKS) | set(EARMARK_FREE)):
        expect(number not in taken, f'opcode {number} is earmarked for back-capture or kept free, not allocated')
    named = dict(re.findall(r'ESPNOW_V4_TYPE_(\w+)\s*=\s*(\d+)', enum))
    for number, name in ALLOCATED.items():
        expect(named.get(name) == str(number), f'opcode {number} is allocated as ESPNOW_V4_TYPE_{name}')
        expect(re.search(rf'#define ESPNOW_V4_MINLEN_{name}\s+{CATCHUP_MINLEN[name]}\b', code) is not None,
               f'ESPNOW_V4_MINLEN_{name} is {CATCHUP_MINLEN[name]}')
    ranges = wire[wire.index('// Ranges:'):wire.index('// Allocation policy')]
    for number, name in list(EARMARKS.items()) + list(ALLOCATED.items()):
        expect(f'{name}={number}' in ranges, f'the opcode map names {name}={number}')
    expect('82–85 live file catch-up' in ranges and '86–89 free' in ranges,
           'the opcode map shows 82–85 live for file catch-up and keeps 86–89 free')
    expect('156–158 earmarked device outbox' in ranges and '159 free' in ranges,
           'the opcode map earmarks 156–158 for the device outbox and keeps 159 free')
    enum_text = wire[wire.index('enum EspNowV4Type'):]
    enum_text = enum_text[:enum_text.index('};')]
    expect('156–158 earmarked device outbox' in enum_text and '86–89 free' in enum_text,
           'the enum marks the earmarked numbers where they would go')
    streams = wire[wire.index('// ---- Wire stream ids'):wire.index('// ---- Flag bits')]
    for row in ('0–127   the sensor type', '0xF0    battery', '0xF1    events'):
        expect(row in streams, f'the wire stream-id table has the row "{row}"')
    compat = (component.parents[1] / 'docs' / 'COMPATIBILITY.md').read_text()
    expect('82-85' in compat and '156-158' in compat and 'backlog_serve' in compat and '0xF0' in compat,
           'docs/COMPATIBILITY.md states the back-capture earmarks')
    expect('CATCHUP_LIST_REQ' in compat and 'files_serve' in compat,
           'docs/COMPATIBILITY.md states the CATCHUP_* layouts and the files_serve bit')
    # The rows: paired + session, outside any bond block, with the W minLens.
    espnow = strip_comments((component / 'System_ESPNow.cpp').read_text())
    for name, handler in (('CATCHUP_LIST_REQ', 'v4hCatchupListReq'), ('CATCHUP_LIST_RESP', 'v4hCatchupListResp'),
                          ('CATCHUP_READ_REQ', 'v4hCatchupReadReq'), ('CATCHUP_READ_RESP', 'v4hCatchupReadResp')):
        expect(re.search(rf'\{{\s*ESPNOW_V4_TYPE_{name},\s*V4_OPC_FLAG_REQ_PAIRED \| V4_OPC_FLAG_REQ_SESSION_ENC,'
                         rf'\s*ESPNOW_V4_MINLEN_{name},\s*{handler}\s*\}}', espnow) is not None,
               f'System_ESPNow.cpp: the {name} row is paired + session with its minLen')


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--component', type=Path, default=COMPONENT)
    p.add_argument('--sanitize', action='store_true', help='accepted for the shared ctest arguments (compile only)')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    wire = (a.component / 'System_ESPNow_Wire.h').read_text()
    blocks = struct_blocks(wire)
    check_asserts(wire, blocks)
    golden = check_golden(blocks)
    compile_header(a.component, a.cxx, golden)
    check_sources(a.component)
    check_earmarks(a.component)
    for line in failures:
        print(f'FAIL: {line}')
    if failures:
        print(f'espnow_wire_layout: {len(failures)} failure(s)')
        return 1
    print(f'espnow_wire_layout: {len(blocks)} structs, every size and offset asserted and pinned; '
          f'receive style and 32-bit masks hold')
    return 0


if __name__ == '__main__':
    sys.exit(main())
