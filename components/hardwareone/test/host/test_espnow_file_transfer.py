#!/usr/bin/env python3
"""Run the ESP-NOW file transfer, both ends, against host doubles.

The suite compiles the production receive slot table (the whole body of
System_ESPNow_Files.cpp), the FILE_START/DATA/END/CANCEL handlers and the
streaming finalize job, the sender pump (sendFileToMac) with its session gate,
and the callers that report a send to a person (espnowsendfile, espnowsend,
imagesend, the OLED send job, with the real CommandArgs) into
espnow_file_transfer_harness.cpp. The filesystem is the shared in-memory fake,
the wire structs and MAC text helpers are the real headers. The doubles are the
slot-table mutex, the filesystem lock, the command-task queue, the session
table, the pending-frame store (one frame per peer, newest wins) and the radio,
which loops the sender's frames into the receive handlers.

One thread plays every task. A task that waits (for the filesystem lock, or in
a pacing delay) runs a hook in which the test plays the other task, and the
mutex double answers a take the way FreeRTOS would: a bounded take of a mutex
whose holder is waiting further up the stack misses, an unbounded one is a
deadlock and is counted.

It checks that a healthy transfer arrives byte for byte on the RAM path and the
streaming path; that a send without an ACTIVE session is refused before the
first frame, starts the handshake once, and that every caller says so; that a
session lost mid-transfer is reported; that a receive is not cancelled because
the slot table was busy; that release and abort do no filesystem work, and do
not wait for the filesystem lock, with the slot lock held; and that an abort,
a drain or a parked teardown that meets lock contention does not pin the slot
or write a buffer twice. On-air loss, pacing against a real radio and the real
session handshake are not modeled.

--source-dir <component-directory> reads production files from another tree
(for example the pre-fix base) while the harness always comes from this folder.
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

BUILD_CONFIG = '''#pragma once
#define ENABLE_ESPNOW 1
#define ENABLE_BONDED_MODE 0
#define ENABLE_OLED_DISPLAY 0
#define ENABLE_AUTOMATION 0
'''

# esp_now.h for System_ESPNow_RxCtx.h: the receive-info struct the handlers read.
ESP_NOW = '''#pragma once
#include <cstdint>
struct esp_now_recv_info {
  uint8_t* src_addr;
  uint8_t* des_addr;
  void*    rx_ctrl;
};
using esp_now_recv_info_t = esp_now_recv_info;
'''


def string_double():
    """The shared host String plus the members the transfer code calls."""
    stub = (HERE / 'web_batch_stubs' / 'Arduino.h').read_text()
    anchor = '  bool concat(const char* text) {'
    tail = '\n private:\n  std::string text_;\n};\n'
    if stub.count(anchor) != 1 or stub.count(tail) != 1:
        raise SystemExit('web_batch_stubs/Arduino.h: String double anchors not found')
    extra = '''  String(const char* text, size_t length) : text_(text ? std::string(text, length) : std::string()) {}
  bool concat(const String& other) { return concat(other.c_str()); }
  bool reserve(size_t capacity) { text_.reserve(capacity); return true; }
'''
    stub = stub.replace(anchor, extra + anchor)
    return stub.replace(tail, tail + 'inline String operator+(const char* left, const String& right) '
                                     '{ return String(left) + right; }\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    p.add_argument('--case', help='run one harness case by name (for per-case evidence)')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block
    from reply_words_source import module_reply_constants

    def path_of(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return path_of(name).read_text()

    def body(text, marker, where, suffix=''):
        if text.count(marker) != 1:
            raise SystemExit(f'{where}: {marker!r} found {text.count(marker)} times')
        return extract_block(text, marker) + suffix

    def optional(text, marker, where, suffix=''):
        return body(text, marker, where, suffix) if marker in text else ''

    main_cpp = read('System_ESPNow.cpp')
    main_h = read('System_ESPNow.h')
    files_cpp = read('System_ESPNow_Files.cpp')
    files_h = read('System_ESPNow_Files.h')
    command_h = read('System_Command.h')
    command_cpp = read('System_Command.cpp')
    filesystem_cpp = read('System_Filesystem.cpp')
    image_cpp = read('System_ImageManager.cpp')
    oled_cpp = read('OLED_Mode_FileBrowser.cpp')
    E = 'System_ESPNow.cpp'

    # ---- source contracts: the callers that do not format a reply themselves ----
    failures = []
    for marker in ('static void sendBondSettings(', 'static void sendBondSchema(',
                   'static void sendBondManifest('):
        text = body(main_cpp, marker, E)
        if not re.search(r'\bsendFileToMac(Result)?\(', text):
            failures.append(f'{marker[:-1]} no longer sends through sendFileToMac, '
                            'so it does not inherit the session gate')
    # The bond's own sync files raise no user-visible file event (bench
    # 2026-11-18): logFileTransferEvent returns with a debug line before the
    # history line, the console line and the file_rx / file_rx_failed post,
    # and the two toasts skip them.
    log = body(main_cpp, 'void logFileTransferEvent(', E)
    quiet = log.find('fileRxBondSync(peerMac, filename)')
    if not (0 <= quiet < log.find('storeMessageInPeerHistory(') and quiet < log.find('systemEventPost(SYSEVT_FILE_RX') and
            quiet < log.find('BROADCAST_PRINTF(') and
            'eventType == MSG_FILE_RECV_SUCCESS || eventType == MSG_FILE_RECV_FAILED' in log[:quiet + 200]):
        failures.append('logFileTransferEvent does not keep the bond sync files out of the events and history')
    glue = body(main_cpp, 'static bool fileRxBondSync(', E)
    if ('fileRxIsBondSync(gSettings.bondModeEnabled, fromPartner, filename)' not in glue or
            'parseMacAddress(gSettings.bondPeerMac, partner)' not in glue):
        failures.append('fileRxBondSync does not ask the rule with bond mode and the bond partner')
    for marker in ('static void fileWriterFinalizeJob(', 'static void v4h_file_end('):
        if 'if (!fileRxBondSync(sndMac, filename)) {' not in body(main_cpp, marker, E):
            failures.append(f'{marker[12:-1]} shows a toast for a bond sync file')
    fslist = read('System_ESPNow_FsList.cpp')
    get_marker = 'static void processGetDeferred('   # a forward declaration comes first
    fs_get = extract_block(fslist[fslist.rindex(get_marker):], get_marker)
    if not re.search(r'\bsendFileToMac(Result)?\(', fs_get):
        failures.append('processGetDeferred no longer sends through sendFileToMac')
    if failures:
        for f in failures:
            print(f'espnow_file_transfer: {f}', file=sys.stderr)
        return 1

    # ---- receive side ----------------------------------------------------------
    # The slot module's public header without its project include, and the whole
    # implementation after its include block.
    slots_header = re.sub(r'^#include "[^"\n]*"\n', '', files_h, flags=re.M)
    start = 'namespace {\n\nconstexpr uint8_t kFileSlots'
    end = '#endif  // ENABLE_ESPNOW'
    if files_cpp.count(start) != 1 or files_cpp.count(end) != 1:
        raise SystemExit('System_ESPNow_Files.cpp: implementation anchors not unique')
    slots_source = files_cpp[files_cpp.index(start):files_cpp.index(end)]

    rx = '\n\n'.join(filter(None, (
        body(main_cpp, 'static void v4h_file_start(', E),
        optional(main_cpp, 'static void v4FileNoteSlotTableBusy(', E),
        body(main_cpp, 'static void v4h_file_data(', E),
        body(main_cpp, 'static void fileWriterFinalizeJob(', E),
        body(main_cpp, 'static void v4h_file_end(', E),
        body(main_cpp, 'static void v4h_file_cancel(', E),
    )))

    # ---- send side ---------------------------------------------------------------
    fixed = 'FileSendResult sendFileToMacResult(' in main_cpp
    tx_parts = [body(main_cpp, 'struct FileSendActiveGuard', E, ';')]
    if fixed:
        tx_parts += [
            body(main_cpp, 'static bool espnowSessionActiveWith(', E),
            body(main_cpp, 'static bool espnowSessionActiveOrKick(', E),
            body(main_cpp, 'FileSendResult sendFileToMacResult(', E),
        ]
    tx_parts.append(body(main_cpp, 'bool sendFileToMac(', E))
    tx = '\n\n'.join(tx_parts)

    result_types = ''
    if fixed:
        result_types = body(main_h, 'enum class FileSendResult', 'System_ESPNow.h', ';')
        fmt = re.search(r'^#define ESPNOW_NO_SESSION_REPLY_FMT \\\n[^\n]*\n', main_h, flags=re.M)
        if not fmt:
            raise SystemExit('System_ESPNow.h: ESPNOW_NO_SESSION_REPLY_FMT not found')
        result_types += '\n' + fmt.group(0)

    commands = '\n\n'.join((
        module_reply_constants(a.component),  # cmd_imagesend returns kFileReplyMissingOrDenied
        body(command_h, 'class CommandArgs', 'System_Command.h', ';'),
        'const String CommandArgs::empty_;',
        body(command_cpp, 'CommandArgs::CommandArgs(', 'System_Command.cpp'),
        body(command_cpp, 'void CommandArgs::parse(', 'System_Command.cpp'),
        body(command_cpp, 'const String& CommandArgs::arg(', 'System_Command.cpp'),
        body(command_cpp, 'String CommandArgs::remaining(', 'System_Command.cpp'),
        body(filesystem_cpp, 'const char* requireQuotedToken(', 'System_Filesystem.cpp'),
        body(main_cpp, 'const char* cmd_espnow_sendfile(', E),
        body(main_cpp, 'const char* cmd_espnow_send(', E),
        body(image_cpp, 'const char* cmd_imagesend(', 'System_ImageManager.cpp'),
        body(oled_cpp, 'struct EspNowSendFileJob', 'OLED_Mode_FileBrowser.cpp', ';'),
        body(oled_cpp, 'static void runEspNowSendFile(', 'OLED_Mode_FileBrowser.cpp'),
    ))

    harness = (HERE / 'espnow_file_transfer_harness.cpp').read_text()
    for tag, value in (('// INSERT_RESULT_TYPES', result_types), ('// INSERT_SLOTS_HEADER', slots_header),
                       ('// INSERT_SLOTS_SOURCE', slots_source), ('// INSERT_RX', rx),
                       ('// INSERT_TX', tx), ('// INSERT_COMMANDS', commands)):
        if harness.count(tag) != 1:
            raise SystemExit(f'harness: {tag} must appear once')
        harness = harness.replace(tag, value + '\n')

    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if a.sanitize else []
    with tempfile.TemporaryDirectory(prefix='hw1-file-transfer-') as tmp:
        d = Path(tmp)
        (d / 'Arduino.h').write_text(string_double())
        (d / 'System_BuildConfig.h').write_text(BUILD_CONFIG)
        (d / 'esp_now.h').write_text(ESP_NOW)
        # Real headers, from the tree under test: wire structs, the receive
        # context and the MAC text helpers.
        # (the wire header includes the four dependency-free rules headers)
        for name in ('System_ESPNow_Wire.h', 'System_ESPNow_RxCtx.h', 'System_MacText.h',
                     'System_ESPNow_SensorCore.h', 'System_ESPNow_TimeCore.h',
                     'System_ESPNow_PowerEventCore.h', 'System_ESPNow_ForwardCore.h',
                     'System_DeviceName.h'):
            shutil.copyfile(path_of(name), d / name)
        unit, exe = d / 'espnow_file_transfer.cpp', d / 'espnow_file_transfer'
        unit.write_text(harness)
        # No -pedantic: the real wire header has a flexible array member.
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter',  # production handlers keep unused parameters
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               *sanitize, '-I', str(d), '-I', str(HERE), '-I', str(json_include),
               str(unit), '-o', str(exe)]
        if not fixed:
            cmd.insert(1, '-DHW1_FILE_SEND_BOOL_ONLY=1')
        subprocess.run(cmd, check=True, timeout=300)
        result = subprocess.run([str(exe)] + ([a.case] if a.case else []), timeout=180)
        if result.returncode:
            print('espnow_file_transfer: FAILED', file=sys.stderr)
            return 1
    print('espnow_file_transfer: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
