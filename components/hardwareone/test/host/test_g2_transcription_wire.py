#!/usr/bin/env python3
"""Check Live text's actual layout with the complete production wire encoder.

Only Arduino/FreeRTOS boundaries are shimmed. The page harness separately
checks rendering and the four-child, single-input-owner contract; this test
decodes CREATE capture flags and guards packet sizes and CRC envelopes.
No BLE, device, or filesystem service I/O is performed.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block


HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def declaration(source, pattern):
    matches = re.findall(pattern, source, flags=re.MULTILINE)
    assert len(matches) == 1, f"expected one production declaration: {pattern}"
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-dir', type=Path, help='optional page-source overlay for regression checks')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('a host C++17 compiler is required')

    page = ((args.source_dir or COMPONENT) / 'G2_Page_Transcription.cpp').read_text()
    owner = (COMPONENT / 'G2_Glasses.cpp').read_text()
    header = (COMPONENT / 'System_G2_Protocol.h').read_text()
    header = header.replace('#include "System_BuildConfig.h"', '')
    header = header.replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text()
    protocol = protocol.replace('#include "System_G2_Protocol.h"', '')
    protocol = protocol.replace('#include <Arduino.h>', '')

    constants = '\n'.join(declaration(page, pattern) for pattern in (
        r'^constexpr size_t kSidebarUpdateMax = [^;]+;',
        r'^constexpr size_t kLiveLines = [^;]+;',
        r'^constexpr size_t kLiveLineBytes = [^;]+;',
    ))
    constants += '\n' + declaration(owner, r'^static constexpr uint32_t BLOCKS_WIDGET_ID\s*= [^;]+;')
    for name, kind in (('kLiveNames', 'char* const'),
                       ('kLiveGeoms', 'G2ContainerGeom'),
                       ('kLiveInitial', 'char* const')):
        constants += '\n' + extract_block(page, f'static const {kind} {name}[') + ';'
    children = extract_block(page, 'G2TextChildSpec children[kLiveChildren];')

    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string>

// The observed lens MTU is 244, leaving 241 bytes for one ATT write.
static constexpr size_t kQualifiedWriteBytes = 244 - 3;

static void checkEnvelope(const uint8_t* envelope, size_t length) {
  assert(length && length <= kQualifiedWriteBytes);
  G2EnvelopeView parsed{};
  assert(g2ParseEnvelope(envelope, length, &parsed));
  assert(parsed.totalFrags == 1 && parsed.fragIdx == 1);
  assert(parsed.sid == G2_SID_EVEN_CORE && parsed.flag == G2_FLAG_REQUEST);
}

// Inspect what the production encoder actually emits, not just input specs.
static void checkTextCapture(const uint8_t* data, size_t bytes, size_t child) {
  uint64_t capture = UINT64_MAX, cid = UINT64_MAX;
  size_t pos = 0;
  while (pos < bytes) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(data, bytes, &pos, &field, &wire));
    if (field == G2_TEXT_F_EVCAP || field == G2_TEXT_F_CID) {
      assert(wire == G2_PB_WIRE_VARINT);
      uint64_t value; assert(g2PbReadVarint(data, bytes, &pos, &value));
      if (field == G2_TEXT_F_EVCAP) { assert(capture == UINT64_MAX); capture = value; }
      else { assert(cid == UINT64_MAX); cid = value; }
    } else assert(g2PbSkipField(data, bytes, &pos, wire));
  }
  assert(cid == 110 + child);
  assert(capture == (child == 0 ? 1u : 0u)); // header only: 1, 0, 0, 0
}
static void checkPageCapture(const uint8_t* data, size_t bytes) {
  size_t pos = 0, children = 0;
  uint64_t total = UINT64_MAX;
  while (pos < bytes) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(data, bytes, &pos, &field, &wire));
    if (field == G2_PAGE_F_TEXT_OBJ) {
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      uint64_t length; assert(g2PbReadVarint(data, bytes, &pos, &length));
      assert(length <= bytes - pos && children < 4);
      checkTextCapture(data + pos, size_t(length), children++); pos += size_t(length);
    } else if (field == G2_PAGE_F_TOTAL) {
      assert(wire == G2_PB_WIRE_VARINT && total == UINT64_MAX);
      assert(g2PbReadVarint(data, bytes, &pos, &total));
    } else assert(g2PbSkipField(data, bytes, &pos, wire));
  }
  assert(total == 4 && children == 4);
}
static void checkCreateCapture(const uint8_t* data, size_t bytes) {
  size_t pos = 0, pages = 0;
  while (pos < bytes) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(data, bytes, &pos, &field, &wire));
    if (field == G2_WRAP_F_CREATE) {
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      uint64_t length; assert(g2PbReadVarint(data, bytes, &pos, &length));
      assert(length <= bytes - pos);
      checkPageCapture(data + pos, size_t(length)); pos += size_t(length); ++pages;
    } else assert(g2PbSkipField(data, bytes, &pos, wire));
  }
  assert(pages == 1);
}

int main() {
  // INSERT_CHILDREN
  uint8_t protobuf[1024], envelope[256];
  size_t largestCreate = 0, largestUpdate = 0;
  // Exercise every uint8_t CREATE magic, including its varint boundary.
  for (uint32_t magic = 0; magic <= UINT8_MAX; ++magic) {
    const size_t bytes = g2BuildCreateMultiTextPb(
        magic, children, kLiveChildren, BLOCKS_WIDGET_ID,
        protobuf, sizeof(protobuf));
    // Equal to the chunk cap requires an extra CRC-only fragment, so '<'.
    assert(bytes && bytes < G2_FRAG_CHUNK_PB);
    const size_t length = g2BuildCreateMultiText(
        42, magic, children, kLiveChildren, BLOCKS_WIDGET_ID,
        envelope, sizeof(envelope));
    checkEnvelope(envelope, length);
    G2EnvelopeView parsed{};
    assert(g2ParseEnvelope(envelope, length, &parsed));
    assert(parsed.payloadLen == bytes);
    assert(memcmp(parsed.payload, protobuf, bytes) == 0);
    checkCreateCapture(parsed.payload, parsed.payloadLen);
    if (length > largestCreate) largestCreate = length;
  }

  constexpr size_t longestBand =
      kLinesPerBand * kLiveLineBytes + kLinesPerBand - 1;
  static_assert(longestBand <= kSidebarUpdateMax, "band exceeds update budget");
  // Empty clears, the longest line band, and the full declared update cap.
  for (size_t bytes : {size_t(0), longestBand, kSidebarUpdateMax}) {
    const std::string content(bytes, 'x');
    for (const auto& child : children) {
      const size_t length = g2BuildUpdateText(
          42, 220, child.containerName, child.containerId, content.c_str(),
          envelope, sizeof(envelope));
      checkEnvelope(envelope, length);
      if (length > largestUpdate) largestUpdate = length;
    }
  }
  printf("G2 transcription wire: CREATE <= %zu B, UPDATE <= %zu B; "
         "capture flags 1,0,0,0; one fragment, CRC valid, ATT limit %zu B\n",
         largestCreate, largestUpdate, kQualifiedWriteBytes);
}
'''.replace('// INSERT_CHILDREN', children)

    platform = ('#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n'
                '#include <stdint.h>\nuint32_t millis() { return 0; }\n'
                'using portMUX_TYPE = int;\n'
                '#define portMUX_INITIALIZER_UNLOCKED 0\n'
                'void portENTER_CRITICAL(int*) {}\nvoid portEXIT_CRITICAL(int*) {}\n')
    with tempfile.TemporaryDirectory(prefix='hw1-g2-transcription-wire-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        executable = Path(tmp) / 'test'
        unit.write_text(platform + header + '\n' + protocol + '\n' + constants + '\n' + harness)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-pedantic', '-Wno-unused-const-variable', '-I', str(COMPONENT),
                   str(unit), '-o', str(executable)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == '__main__':
    main()
