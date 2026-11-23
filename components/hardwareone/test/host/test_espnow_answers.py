#!/usr/bin/env python3
"""Rule F3 of the 1.0 ESP-NOW format: answers, end to end on the host.

Every answer frame starts {u32 reqId, u8 status} (System_ESPNow_AnswerCore.h)
and goes under the answerer's own fresh header msgId. This suite compiles,
unchanged from System_ESPNow.cpp, the CMD_RESP senders
(v4_send_command_response, v4_send_cmd_resp_async), the STREAM line sender
(v4_send_stream_line) and the two receivers (v4h_cmd_resp, v4h_stream) against
the real System_ESPNow_Wire.h and System_ESPNow_RxCtx.h, records each frame a
sender puts on the air and hands it to the receiver with its own header msgId.

The command path is compiled unchanged too: v4h_cmd (the one-slot busy
answer), v4CmdResultCallback (every remote command's result, its text and its
out-of-memory fallback) and v4_handle_cmd from its authentication decision on
(the no-slot, could-not-queue and too-long refusals); the session table, the
command queue and the executor are doubles. Each answer there must carry the
CMD's msgId as reqId and the status the call site means (busy, no_slot,
too_large, failed, ok), and the receiver must read it back by name. The two
web bond page's FS error tag (fsStatusTag, WebPage_Bond.cpp) must be the one
name table's word for every status. The two
topology answer receivers (v4h_topo_start, v4h_topo_peer) are inserted with a
one-slot stream table double: an answer whose status is not ok records
nothing (a later firmware's busy TOPO_START is not an edge device).

It checks that a refusal and a result carry the CMD's number as reqId and a
status (busy, failed by name, ok), under a msgId of the answerer's own; that
the receiver correlates by the payload's reqId; that each STREAM line has a
msgId of its own and is filed under its command's reqId (0 for startstream);
that a line is cut to one frame for its path with the header included; and
that a header-only STREAM files nothing. Source rules: no CMD_RESP or STREAM
sender in System_ESPNow.cpp passes the request's msgId as the frame msgId, and
the duplicate check's bypass list no longer names STREAM, BOND_STREAM_CTRL or
CMD_RESP.

Options: --cxx <compiler>, --sanitize, --component <dir>, --source-dir <dir>.
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
from test_espnow_file_transfer import BUILD_CONFIG, ESP_NOW, string_double  # noqa: E402

DEFINITIONS = (
    'bool v4_send_command_response(const uint8_t* dst, uint32_t reqId, uint8_t status, '
    'const char* resultText, uint16_t textLen)',
    'static bool v4_send_cmd_resp_async(const uint8_t* dst, uint32_t reqId, uint8_t status, const char* text)',
    'static bool v4_send_stream_line(const uint8_t* dst, uint32_t reqId, const char* data, size_t len)',
    'static void v4h_cmd_resp(const V4RxCtx& ctx)',
    'static void v4h_stream(const V4RxCtx& ctx)',
    'static void v4h_cmd(const V4RxCtx& ctx)',
    'static String redactEspNowCommandResult(const String& output)',
    'static void v4CmdResultCallback(bool ok, const char* result, void* userData)',
    'static void v4h_topo_start(const V4RxCtx& ctx)',
    'static void v4h_topo_peer(const V4RxCtx& ctx)',
)
HANDLE_CMD = ('static void v4_handle_cmd(const uint8_t* srcMac, const char* deviceName, uint32_t msgId, '
              'const char* cmdPayload, bool wasSessionEncrypted)')
AUTH_DECISION = 'if (!authOk) return;'


def definition(source: str, signature: str) -> str:
    """The definition (a forward declaration of the same signature may come first)."""
    if source.count(signature + ' {') != 1:
        raise SystemExit(f'System_ESPNow.cpp: expected one definition of {signature!r}')
    return extract_block(source[source.index(signature + ' {'):], signature)


def handle_cmd_after_auth(source: str) -> str:
    """v4_handle_cmd from its authentication decision to its end, as a function
    of the locals the authentication part sets."""
    body = definition(source, HANDLE_CMD)
    if body.count(AUTH_DECISION) != 1:
        raise SystemExit(f'v4_handle_cmd: expected one {AUTH_DECISION!r}')
    return ('static void v4_handle_cmd_after_auth(const uint8_t* srcMac, const char* deviceName, uint32_t msgId,\n'
            '                                     const char* actualCmd, const char* username,\n'
            '                                     const String& safeActualCmd) {\n'
            '  bool authOk = true;\n  ' + body[body.index(AUTH_DECISION):])


def console_rules(source: str) -> list[str]:
    """The requester's console names a failed command's status (CHANGELOG:
    "Command FAILED (busy)"), from the one status-name table."""
    failures = []
    if 'const char* statusWord = espnowanswer::statusName(ESPNOW_V4_TYPE_CMD_RESP, ' \
            'gEspNow->deferredCmdRespStatus);' not in source:
        failures.append('the CMD_RESP console line does not name the status from espnowanswer::statusName')
    if '"[ESP-NOW] Command FAILED (%s) from %s: %s", statusWord,' not in source:
        failures.append('the console line is no longer "Command FAILED (<status>) from <peer>: <text>"')
    return failures


def source_rules(source: str) -> list[str]:
    failures = []
    # Every CMD_RESP / STREAM send names generateMessageId() (or goes through
    # one of the two helpers): never ctx->cmdMsgId, msgId or ctx.h->msgId.
    for m in re.finditer(r'(sendAead(?:Sync)?|v4_send_payload_smart)\(\s*[^,]+,\s*ESPNOW_V4_TYPE_(CMD_RESP|STREAM)\s*,'
                         r'\s*[^,]+,\s*([^,]+),', source):
        if m.group(3).strip() != 'generateMessageId()':
            line = source.count('\n', 0, m.start()) + 1
            failures.append(f'System_ESPNow.cpp:{line}: a {m.group(2)} frame is sent under '
                            f'{m.group(3).strip()!r}, not a fresh generateMessageId()')
    incoming = source[source.index('// Dedup check'):]
    incoming = incoming[:incoming.index('v4_dedup_frame_seen(recv_info->src_addr')]
    for opcode in ('STREAM', 'BOND_STREAM_CTRL', 'CMD_RESP'):
        if f'ESPNOW_V4_TYPE_{opcode} &&' in incoming or f'h->type != ESPNOW_V4_TYPE_{opcode}' in incoming:
            failures.append(f'the duplicate check still skips {opcode}')
    return failures


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def path_of(name: str) -> Path:
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    source = path_of('System_ESPNow.cpp').read_text()
    failures = source_rules(source)
    for f in failures:
        print(f'FAIL {f}', file=sys.stderr)

    failures += console_rules(source)
    if source.count('struct V3CmdAsyncCtx {') != 1:
        raise SystemExit('System_ESPNow.cpp: expected one struct V3CmdAsyncCtx')
    production = extract_block(source, 'struct V3CmdAsyncCtx') + ';\n\n'
    production += '\n\n'.join(definition(source, sig) for sig in DEFINITIONS)
    production += '\n\n' + handle_cmd_after_auth(source)
    web_bond = path_of('WebPage_Bond.cpp').read_text()
    tag = 'static const char* fsStatusTag(uint8_t status)'
    if web_bond.count(tag) != 1:
        raise SystemExit(f'WebPage_Bond.cpp: expected one {tag!r}')
    production += '\n\n' + extract_block(web_bond, tag)
    harness = (HERE / 'espnow_answers_harness.cpp').read_text()
    if harness.count('// INSERT_PRODUCTION') != 1:
        raise SystemExit('harness: // INSERT_PRODUCTION must appear once')
    harness = harness.replace('// INSERT_PRODUCTION', production)

    sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if a.sanitize else []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-answers-') as tmp:
        d = Path(tmp)
        (d / 'Arduino.h').write_text(string_double())
        (d / 'System_BuildConfig.h').write_text(BUILD_CONFIG)
        (d / 'esp_now.h').write_text(ESP_NOW)
        for name in ('System_ESPNow_Wire.h', 'System_ESPNow_RxCtx.h', 'System_ESPNow_SensorCore.h',
                     'System_ESPNow_TimeCore.h', 'System_ClockCore.h', 'System_ESPNow_PowerEventCore.h',
                     'System_ESPNow_ForwardCore.h', 'System_ESPNow_AnswerCore.h', 'System_ESPNow_CatchupCore.h',
                     'System_DeviceName.h'):
            shutil.copyfile(path_of(name), d / name)
        unit, exe = d / 'espnow_answers.cpp', d / 'espnow_answers'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-Wno-unused-function', *sanitize, '-I', str(d), str(unit), '-o', str(exe)]
        subprocess.run(cmd, check=True, timeout=300)
        if subprocess.run([str(exe)], timeout=120).returncode:
            failures.append('the answers harness reported failures')
    if failures:
        print(f'espnow_answers: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print('espnow_answers: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
