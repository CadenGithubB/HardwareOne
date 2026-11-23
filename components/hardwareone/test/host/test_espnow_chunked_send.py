#!/usr/bin/env python3
"""Run the production ESP-NOW encrypted multi-frame sender against a scripted
receiver (ESP-NOW 1.0 step 5b, DESIGN 3.3).

Compiled: V4FragAckWait and v4_frag_ack_alloc, v4_send_chunked_attempt,
v4_chunked_wait, kChunkedRekeyWaitMs and v4_send_encrypted_chunked from
System_ESPNow.cpp, the V4SendOutcome / V4SendResult types from
System_ESPNow_Sessions.h and the header, flags and size constants from
System_ESPNow_Wire.h, over doubles for the clock, one peer and its session,
the radio and the tracked-send and session-liveness hooks
(espnow_chunked_send_harness.cpp). Each emitted fragment is answered by the
script through the fragment waiter, as the RX ACK handler does.

Checks: every fragment accepted is Delivered after one attempt, the session
hears Delivered and the tracked send is marked by the caller's msgId; a busy
refusal of the last fragment sends the whole message once more under a new
msgId after at least 300 ms and is Delivered; busy twice is Refused(kBusy)
after exactly two attempts, and the session hears Refused (alive) and the
tracked send is marked refused; kRefused is final; no ACK is NoAnswer after
three tries of the first fragment, the only outcome the session counts as a
strike; a rekey this device starts in the middle of the send is waited for and
the message sent again on the new keys under a new msgId; a rekey that does
not apply within 1 s is NotSent and counts toward nothing; kNoSlot is retried
once like busy; no copy leaving the radio is NotSent (no strike); a long TEXT
(stored per fragment on the receiver) is never sent again as a whole.
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


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    source = (a.component / 'System_ESPNow.cpp').read_text()
    wire = (a.component / 'System_ESPNow_Wire.h').read_text()
    sessions_h = (a.component / 'System_ESPNow_Sessions.h').read_text()

    def one(text, marker):
        if text.count(marker) < 1:
            sys.exit(f'FAIL extraction: {marker!r} not found')
        return extract_block(text, marker)

    defines = re.findall(r'^#define ESPNOW_V4_(?:MAGIC|VERSION|MAX_PAYLOAD|AEAD_TAG_LEN|HEADER_LEN)\b.*$', wire, re.M)
    relay = re.search(r'^#define ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT \\\n.*$', wire, re.M)
    local = re.findall(r'^#define V4_(?:MAX_FRAGMENT_PAYLOAD|FRAG_MAX|REASM_MAX_MSG)\b.*$', source, re.M)
    if not relay or len(local) != 3:
        sys.exit('FAIL extraction: relay budget or the fragment constants not found')
    text_op = re.search(r'\bESPNOW_V4_TYPE_TEXT\s*=\s*(\d+)', wire).group(1)
    cmd_op = re.search(r'\bESPNOW_V4_TYPE_CMD\s*=\s*(\d+)', wire).group(1)
    file_start_op = re.search(r'\bESPNOW_V4_TYPE_FILE_START\s*=\s*(\d+)', wire).group(1)
    wire_part = '\n'.join(['static constexpr size_t CMD_RESULT_MAX = 4096;',
                           f'static constexpr uint8_t ESPNOW_V4_TYPE_TEXT = {text_op};',
                           f'static constexpr uint8_t ESPNOW_V4_TYPE_CMD = {cmd_op};',
                           f'static constexpr uint8_t ESPNOW_V4_TYPE_FILE_START = {file_start_op};'] + defines + local)
    # The relay budget refers to wire constants this harness does not need; the
    # sender only reads it on a routed send, which the doubles never make.
    wire_part += '\n#define ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT 146\n'
    wire_part += '\n'.join(extract_block(wire, m) + ';' for m in (
        'enum EspNowV4Flags', 'struct __attribute__((packed)) EspNowV4Header'))
    outcome = '\n'.join(re.search(pat, sessions_h, re.M | re.S).group(0) for pat in (
        r'^enum class V4SendOutcome[^;]*;', r'^struct V4SendResult \{.*?\n\};'))
    ack_wait = (one(source, 'struct V4FragAckWait') + ';\n'
                + re.search(r'^EXT_RAM_BSS_ATTR static V4FragAckWait gV4FragAckWait\[[^;]*;', source, re.M).group(0)
                + '\n' + one(source, 'static V4FragAckWait* v4_frag_ack_alloc('))
    wait_const = re.search(r'^static constexpr uint32_t kChunkedRekeyWaitMs = \d+;', source, re.M).group(0)
    sender = '\n\n'.join([one(source, 'static V4SendResult v4_send_chunked_attempt('),
                          one(source, 'static void v4_chunked_wait('), wait_const,
                          one(source[source.rindex('V4SendResult v4_send_encrypted_chunked('):],
                              'V4SendResult v4_send_encrypted_chunked(')])

    harness = (HERE / 'espnow_chunked_send_harness.cpp').read_text()
    for tag, value in [('// INSERT_WIRE', wire_part), ('// INSERT_OUTCOME', outcome),
                       ('// INSERT_ACK_WAIT', ack_wait), ('// INSERT_SENDER', sender)]:
        if harness.count(tag) != 1:
            sys.exit(f'FAIL: harness marker {tag} must appear exactly once')
        harness = harness.replace(tag, value)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-chunked-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        (Path(tmp) / 'System_ESPNow_AnswerCore.h').write_text((a.component / 'System_ESPNow_AnswerCore.h').read_text())
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
               '-Wno-unused-const-variable', '-I', tmp, str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        rc = subprocess.run([str(exe)], timeout=60).returncode
    if rc:
        print('FAIL espnow_chunked_send')
        return 1
    print('PASS espnow_chunked_send')
    return 0


if __name__ == '__main__':
    sys.exit(main())
