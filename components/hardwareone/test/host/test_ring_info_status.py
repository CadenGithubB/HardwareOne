#!/usr/bin/env python3
"""Exercise the production ring status formatter/CLI with vendored ArduinoJson.

The old CLI's real CompactJson implementation is used for --baseline. Compare
ordinary JSON/text bytes and parsed escaped string values, and use Python's
strict JSON decoder to catch invalid control-character serialization. Driver,
RTOS stack sampling and allocation failures are host boundaries; this does not
qualify ring connection concurrency or a physical peripheral.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require, JSON_INCLUDE

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(component: Path, baseline: bool) -> str:
    source = (component / 'G2_Ring.cpp').read_text()
    # The owner's stack figure: the budget constant and, since the owner task
    # became releasable, the high-water mark the owner publishes about itself.
    owner_stack = [re.search(r'static constexpr uint32_t RING_OWNER_STACK_BYTES = \d+;',
                             source).group()]
    status = extract_block(source, 'void g2RingGetStatus(')
    if not baseline:
        owner_stack.append(re.search(
            r'^static std::atomic<uint32_t> sRingOwnerStackFreeMin\{RING_OWNER_STACK_BYTES\};',
            source, re.M).group())
        require('sRingOwnerTask' not in status and 'uxTaskGetStackHighWaterMark' not in status,
                'the status text reads the published high-water mark, never the owner handle')
    definitions = [extract_macro((COMPONENT / 'System_Utils.h').read_text(),
                                 'RETURN_VALID_IF_VALIDATE_CSTR'),
                   extract_block((COMPONENT / 'System_Utils.cpp').read_text(),
                                 'bool argWantsJson('),
                   status]
    if baseline:
        events = (component / 'BLE_Events.cpp').read_text()
        definitions += [extract_block((component / 'BLE_Events.h').read_text(),
                                      'class CompactJson') + ';',
                        events[events.index('CompactJson::CompactJson('):
                               events.index('// blePushEvent\n')]]
    else:
        definitions[-1:-1] = [re.search(pattern, source, re.M).group() for pattern in (
            r'^static portMUX_TYPE sRingIdentityMux[^\n]+', r'^static char sRingIdentityName[^\n]+')]
        definitions[-1:-1] = [extract_block(source, 'static void ringCopyBounded('),
                              extract_block(source, 'void g2RingSetIdentity('),
                              extract_block(source, 'void g2RingCopyIdentity(')]
        builder = extract_block(source, 'bool g2RingBuildStatusJson(')
        # Source guards (the event and the connect reply need the BLE owner to
        # run): neither the ring-status event nor the direct-connect reply
        # carries the ring's address.
        event = extract_block(source, 'static void ringPushStatusEvent(')
        require('gRingDeviceAddress' not in event and '.kv("a"' not in event,
                'the ring-status event must not carry the ring address')
        connect = extract_block(source, 'static const char* cmd_openring(')
        require('mac.c_str()' not in connect, 'the openring connect reply must not echo the address')
        require('g2RingConnectInFlight()' in builder,
                'ring pending status must use its existing synchronized reader')
        require('sRingOwnerTask' not in builder and 'uxTaskGetStackHighWaterMark' not in builder,
                'the status JSON reads the published high-water mark, never the owner handle')
        definitions += [extract_block(source, 'bool g2RingConnectInFlight('), builder]
    definitions.append(extract_block(source, 'static const char* cmd_ringstatus('))
    return (HERE / 'ring_info_status_harness.cpp').read_text().replace(
        '// INSERT_STATE', extract_block(source, 'struct G2RingState') + ';').replace(
        '// INSERT_OWNER_STACK', '\n'.join(owner_stack)).replace(
        '// INSERT_PRODUCTION', '\n\n'.join(definitions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
             '-Wno-unused-function', '-Wno-unused-variable']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-ring-info-') as directory:
        work = Path(directory)
        string_stub = (HERE / 'web_batch_stubs/Arduino.h').read_text()
        string_stub = string_stub.replace('text_ = text ? text : "";',
                                          'invalid_ = false; text_ = text ? text : "";')
        string_stub = string_stub.replace('return text_.c_str();',
                                          'return invalid_ ? nullptr : text_.c_str();')
        string_stub = string_stub.replace(' private:',
            '  void invalidateForTest() { text_.clear(); invalid_ = true; }\n private:')
        string_stub = string_stub.replace('std::string text_;',
                                          'std::string text_; bool invalid_ = false;')
        (work / 'Arduino.h').write_text(string_stub)

        def run(name: str, source: str, extra: list[str]) -> str:
            path = work / (name + '.cpp')
            path.write_text(source)
            subprocess.run([args.cxx, *flags, '-I', str(work), '-I', str(JSON_INCLUDE),
                            *extra, str(path), '-o', str(work / name)], check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)

        def validate(output: str) -> list[tuple[str, object]]:
            records = []
            for line in output.splitlines():
                kind, payload = line.split('\t', 1)
                if kind == 'SERIALIZER':
                    decoded = bytes.fromhex(payload).decode('utf-8')
                    value = ''.join(chr(c) for c in range(32)) + '\"/\\café'
                    require(json.loads(decoded) == {value: value},
                            'compact/pretty keys and values must preserve every control byte and UTF-8')
                    continue
                if kind != 'TEXT':
                    parsed = json.loads(payload)  # rejects raw control characters
                    require(len(parsed) == 11 and 'addr' not in parsed,
                            'ring status schema must contain all eleven fields and no address')
                require('00:11:22' not in payload, 'no ring status output carries the address')
                records.append((kind, json.loads(payload) if kind == 'ESCAPE' else payload))
            return records

        current = validate(run('ring-current', generated(COMPONENT, False), ['-DTEST_BASELINE=0']))
        if args.baseline:
            previous = validate(run('ring-baseline', generated(args.baseline, True),
                                    ['-DTEST_BASELINE=1']))
            require(current == previous, 'ring CLI values/text changed from characterized baseline')
            print('Ring status: ordinary CLI bytes/text and escaped values match pre-change production')

        shutil.copyfile(COMPONENT / 'G2_Ring.h', work / 'G2_Ring.h')
        for bluetooth, glasses in ((0, 1), (1, 0), (0, 0)):
            (work / 'System_BuildConfig.h').write_text(
                f'#define ENABLE_BLUETOOTH {bluetooth}\n#define ENABLE_G2_GLASSES {glasses}\n')
            run(f'disabled-{bluetooth}-{glasses}', '''
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "G2_Ring.h"
#include <cassert>
#include <type_traits>
void webBatchTestStringAppend() {}
static_assert(std::is_same_v<decltype(&g2RingBuildStatusJson), bool (*)(JsonObject)>);
int main() {
  JsonDocument doc;
  JsonObject object = doc.to<JsonObject>();
  object["sentinel"] = 9;
  assert(!g2RingBuildStatusJson(object));
  assert(object.size() == 1 && object["sentinel"] == 9);
  assert(!g2RingBuildStatusJson(JsonObject()));
}
''', [])
    print('Ring status: production owner/CLI, disabled gates and allocation/size failures passed')


if __name__ == '__main__':
    main()
