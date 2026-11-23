#!/usr/bin/env python3
"""ESP-NOW 1.0 step 6: one word per meaning, and every remote command tracked.

Compiled from System_ESPNow_Sessions.cpp, unchanged: the tracked-send table
(sendStatusRegister, sendStatusMarkDelivered, sendStatusMarkRefused,
sendStatusMarkFailed, sendStatusSweep) with the SendStatus types of
System_ESPNow_Sessions.h, over doubles for the clock, the allocator and the
durable history stamp (espnowUpdateSentDeliveryState). A TEXT whose receiver's
ring is full is answered ACK(busy): the tracked send ends BUSY with status busy
and the history row is stamped 4 ("Busy"), never "Delivered"; no slot is Busy
too; another status is REFUSED (5) with its status; no ACK in 10 s is TIMEOUT
(2, "No answer"); an ACK from another device changes nothing.

Source rules: the web snapshot names the states pending, delivered, timeout,
failed, busy, refused and gives a refusal its reason; the web page's words are
Busy, Refused and No answer (never "No ACK" or "Failed (handshake)"); the OLED
conversation draws Delivered / No answer / Busy / Refused and "Result (late)";
every CMD and USER_SYNC sender registers its request (espnowRemoteCmdBegin)
before the send and cancels it when the send fails; a sealed ACK, a STREAM line
and the espnow_task loop reach the requester table (remoteCmdOnAck,
remoteCmdOnStream, remoteCmdDrain); v4h_cmd sends no CMD_RESP(busy).

Options: --cxx <compiler>, --sanitize, --component <dir>.
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

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "System_ESPNow_AnswerCore.h"
enum class AllocPref { PreferPsram };
static void* ps_alloc(size_t n, AllocPref, const char*) { return std::calloc(1, n); }
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }
#define ERROR_ESPNOWF(...) do {} while (0)
#define WARN_ESPNOWF(...) do {} while (0)
// sessionNoteSendResult's table: none here (it returns at once).
struct SessionState { int state; uint8_t peerMac[6]; uint8_t consecutiveSendTimeouts; uint16_t sessionId; };
static SessionState* gSessions = nullptr;
static constexpr uint8_t kSessionSlots = 0;
static constexpr uint8_t kSessionSendTimeoutsBeforeReestablish = 2;
enum { SESSION_ACTIVE = 3 };
static void sessionClear(SessionState*) {}
struct Stamp { uint32_t msgId; uint8_t state; };
static std::vector<Stamp> gStamps;
// INSERT_TYPES
// INSERT_SENDSTATUS
static unsigned checks = 0, failures = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
static const SendStatus* slotOf(uint32_t msgId) {
  for (uint8_t i = 0; i < sendStatusSlotCount(); i++) {
    const SendStatus* s = sendStatusAt(i);
    if (s && s->inUse && s->msgId == msgId) return s;
  }
  return nullptr;
}
static bool stamped(uint32_t msgId, uint8_t state) {
  return !gStamps.empty() && gStamps.back().msgId == msgId && gStamps.back().state == state;
}
int main() {
  const uint8_t peer[6] = {2, 0, 0, 0, 0, 1}, other[6] = {2, 0, 0, 0, 0, 2};
  // TEXT ring full on the receiver: ACK busy.
  sendStatusRegister(10, peer);
  sendStatusMarkRefused(10, peer, espnowanswer::kBusy);
  check(slotOf(10) && slotOf(10)->state == SEND_STATUS_BUSY && slotOf(10)->status == espnowanswer::kBusy &&
        stamped(10, 4), "TEXT ring full: the send ends Busy (state 4), stamped in the history");
  sendStatusMarkDelivered(10, peer);
  check(slotOf(10)->state == SEND_STATUS_BUSY, "a later ACK does not turn Busy into Delivered");
  sendStatusRegister(11, peer);
  sendStatusMarkRefused(11, peer, espnowanswer::kNoSlot);
  check(slotOf(11)->state == SEND_STATUS_BUSY && stamped(11, 4), "no slot is Busy too");
  sendStatusRegister(12, peer);
  sendStatusMarkRefused(12, peer, espnowanswer::kTooLarge);
  check(slotOf(12)->state == SEND_STATUS_REFUSED && slotOf(12)->status == espnowanswer::kTooLarge &&
        stamped(12, 5), "too large is Refused (state 5) with its status");
  sendStatusRegister(13, peer);
  sendStatusMarkRefused(13, other, espnowanswer::kBusy);
  check(slotOf(13)->state == SEND_STATUS_PENDING, "an ACK from another device changes nothing");
  sendStatusMarkDelivered(13, peer);
  check(slotOf(13)->state == SEND_STATUS_DELIVERED && stamped(13, 1), "ACK(0): Delivered");
  sendStatusRegister(14, peer);
  gNow += kSendStatusPendingTimeoutMs;
  sendStatusSweep(gNow);
  check(slotOf(14)->state == SEND_STATUS_TIMEOUT && stamped(14, 2), "no ACK in 10 s: timeout (No answer)");
  std::printf("espnow_send_words: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def source_rules(read) -> list:
    failures = []
    web = read('WebPage_ESPNow.cpp')
    if '{ "pending", "delivered", "timeout", "failed", "busy", "refused" }' not in web:
        failures.append('the web deliveries snapshot does not name busy and refused')
    if '",\\"reason\\":\\""' not in web:
        failures.append('a refused delivery carries no reason')
    page = read('WebPage_ESPNow.h')
    word = extract_block(page, 'function sendStateWord(')
    for w in ("'No answer'", "'Busy'", "'Refused'"):
        if w not in word:
            failures.append(f'the web page has no {w} word')
    for old in ('No ACK (timeout)', 'Failed (handshake)'):
        if old in page:
            failures.append(f'the web page still says {old!r}')
    if "result === 'late'" not in page or "'Result (late): '" not in page:
        failures.append('the web page does not mark a late result')
    oled = read('OLED_ESPNow.cpp')
    for label in ('label = "No answer";', 'label = "Busy";', 'label = "Refused";', '"Result (late)"'):
        if label not in oled:
            failures.append(f'the OLED conversation lacks {label}')
    if 'label = "No ACK"' in oled or 'label = "Failed"' in oled:
        failures.append('the OLED conversation still says No ACK / Failed')

    source = read('System_ESPNow.cpp')
    utils = read('System_Utils.cpp')
    # Every CMD / USER_SYNC send: the request is registered first.
    for text, name in ((source, 'System_ESPNow.cpp'), (utils, 'System_Utils.cpp')):
        for m in re.finditer(r'(sendAeadSync|v4_send_encrypted_or_queue|v4_send_payload_smart)\(\s*[^,]+,\s*'
                             r'ESPNOW_V4_TYPE_(CMD|USER_SYNC)\s*,', text):
            before = text[max(0, m.start() - 900):m.start()]
            line = text.count('\n', 0, m.start()) + 1
            if 'espnowRemoteCmdBegin(' not in before:
                failures.append(f'{name}:{line}: a {m.group(2)} send is not registered (espnowRemoteCmdBegin) first')
            after = text[m.start():m.start() + 900]
            if 'espnowRemoteCmdCancel(' not in after:
                failures.append(f'{name}:{line}: a failed {m.group(2)} send does not cancel its request')
    ack = extract_block(source, 'static void v4_rx_ack(')
    if 'remoteCmdOnAck(peer, a.ackFor, a.status, a.fragIndex + 1 >= a.fragCount);' not in ack:
        failures.append('a sealed ACK does not reach the remote-command requests')
    if 'remoteCmdOnStream(ctx.recv_info->src_addr, sh.reqId);' not in extract_block(source, 'static V4Verdict v4h_stream('):
        failures.append('a STREAM line does not move its command\'s deadline')
    if source.count('remoteCmdDrain((uint32_t)millis());') != 1:
        failures.append('the espnow_task loop does not drain the remote-command results once per pass')
    cmd = extract_block(source, 'static V4Verdict v4h_cmd(')
    if 'v4_send_cmd_resp_async' in cmd or 'ESPNOW_V4_TYPE_CMD_RESP' in cmd:
        failures.append('v4h_cmd still sends a CMD_RESP(busy): the ACK is the one answer')
    return failures


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def read(name: str) -> str:
        return (a.component / name).read_text()

    failures = source_rules(read)
    sessions_h = read('System_ESPNow_Sessions.h')
    sessions = read('System_ESPNow_Sessions.cpp')
    types = '\n'.join([
        re.search(r'^constexpr uint32_t kSendStatusPendingTimeoutMs\b.*$', sessions_h, re.M).group(0),
        re.search(r'^constexpr uint32_t kSendStatusKeepResolvedMs\b.*$', sessions_h, re.M).group(0),
        extract_block(sessions_h, 'enum SendStatusState : uint8_t') + ';',
        extract_block(sessions_h, 'struct SendStatus') + ';',
        'enum class V4SendOutcome : uint8_t { Delivered = 0, Refused = 1, NoAnswer = 2, NotSent = 3 };',
        'static void espnowUpdateSentDeliveryState(const uint8_t*, uint32_t msgId, uint8_t state) '
        '{ gStamps.push_back({msgId, state}); }',
    ])
    start = sessions.index('namespace {\n\nconstexpr uint8_t kSendStatusSlots')
    end = sessions.index('// ====', sessions.index('extern "C" const SendStatus* sendStatusAt('))
    block = sessions[start:end]
    # The forward declaration of the history stamp is the harness's double.
    block = re.sub(r'^void espnowUpdateSentDeliveryState\([^;]*;\n', '', block, flags=re.M)
    block = block.replace('extern "C" ', 'static ')
    harness = HARNESS.replace('// INSERT_TYPES', types).replace('// INSERT_SENDSTATUS', block)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-words-') as tmp:
        d = Path(tmp)
        shutil.copyfile(a.component / 'System_ESPNow_AnswerCore.h', d / 'System_ESPNow_AnswerCore.h')
        unit, exe = d / 'words.cpp', d / 'words'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
               '-Wno-unused-const-variable', '-I', str(d), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        if subprocess.run([str(exe)], timeout=60).returncode:
            failures.append('the tracked-send cases reported failures')
    for f in failures:
        print(f'FAIL {f}', file=sys.stderr)
    if failures:
        return 1
    print('PASS espnow_send_words')
    return 0


if __name__ == '__main__':
    sys.exit(main())
