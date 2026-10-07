#!/usr/bin/env python3
"""G2 RX: unmapped sids (e.g. 0x12 from the glasses' own QuickNote on 2.3.2.14)
are logged with their payload so they can be decoded, and nothing else.

Compiles the production g2HexCompact() spliced out of G2_Glasses.cpp and
g2sidName() out of System_G2_Protocol.cpp, checks the hex at the edges (0, 1,
11, 64 and over 64 bytes, short buffers), that sid 0x12 is named for
g2protostats, and that the longest possible line fits one debug message.
Then pins the `default:` branch of the RX sid switch: only under DEBUG_G2
(the work is skipped otherwise), a single DEBUG_G2F line, and no reply, send,
state change or event (play-nice contract R20, R22).
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

HARNESS = r'''
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
// INSERT_PRODUCTION
static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL %s\n", m); ++failures; } } while (0)
static std::string hex(const uint8_t* data, size_t len, size_t cap, size_t* shown) {
  char out[256];
  std::memset(out, 'x', sizeof(out));
  *shown = g2HexCompact(data, len, out, cap);
  return out;
}
int main() {
  uint8_t payload[100];
  for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i * 37 + 0x0b);
  char buffer[2 * kG2UnknownSidHexMax + 1];
  size_t shown = 0;
  // 11- and 9-byte payloads (the sizes sid 0x12 had on the bench; the bytes
  // here are made up) are shown in full.
  const uint8_t eleven[11] = {0x08, 0x01, 0x10, 0x9c, 0x01, 0x1a, 0x04, 0x08, 0x02, 0x10, 0xff};
  CHECK(hex(eleven, 11, sizeof(buffer), &shown) == "0801109C011A04080210FF" && shown == 11, "11 B in full");
  CHECK(hex(eleven, 9, sizeof(buffer), &shown) == "0801109C011A040802" && shown == 9, "9 B in full");
  CHECK(hex(payload, 0, sizeof(buffer), &shown).empty() && shown == 0, "empty payload");
  CHECK(hex(nullptr, 5, sizeof(buffer), &shown).empty() && shown == 0, "no payload");
  CHECK(hex(payload, 1, sizeof(buffer), &shown) == "0B" && shown == 1, "one byte, uppercase");
  // Capped at 64 bytes, one line, whatever the payload length.
  const std::string full = hex(payload, 64, sizeof(buffer), &shown);
  CHECK(full.size() == 128 && shown == 64, "64 bytes");
  CHECK(hex(payload, 100, sizeof(buffer), &shown) == full && shown == 64, "over 64: the first 64");
  for (char c : full) CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'), "hex digits only, no separators");
  // Never past the buffer: an odd or tiny buffer holds whole bytes only.
  CHECK(hex(payload, 11, 7, &shown) == "0B3055" && shown == 3, "short buffer: three bytes");
  CHECK(hex(payload, 11, 6, &shown) == "0B30" && shown == 2, "odd room: whole bytes only");
  CHECK(hex(payload, 11, 1, &shown).empty() && shown == 0, "room for the terminator only");
  CHECK(g2HexCompact(payload, 11, nullptr, 8) == 0 && g2HexCompact(payload, 11, buffer, 0) == 0, "no buffer");
  // The sid naming g2protostats prints.
  CHECK(!std::strcmp(g2sidName(0x12), "QuickNote?"), "sid 0x12 is named");
  CHECK(!std::strcmp(g2sidName(0x11), "Unknown") && !std::strcmp(g2sidName(0x13), "Unknown"), "neighbours unknown");
  CHECK(!std::strcmp(g2sidName(0x10), "Onboarding"), "existing names unchanged");
  // The longest line the default branch can print fits one 256-byte debug message.
  char line[512];
  const char* name = g2sidName(0x12);
  for (int sid = 0; sid < 256; ++sid)
    if (std::strlen(g2sidName(static_cast<uint8_t>(sid))) > std::strlen(name)) name = g2sidName(static_cast<uint8_t>(sid));
  const int n = std::snprintf(line, sizeof(line), FORMAT, 'R', 0xFFu, name, 0xFFu, 4294967295u, full.c_str(), "...");
  CHECK(n > 0 && n < 256, "one debug message (DEBUG_MSG_SIZE 256)");
  if (failures) return 1;
  std::puts("G2 unknown-sid hex log and sid naming passed");
  return 0;
}
'''


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    c = a.component.resolve()
    sys.path.insert(0, str(c / 'test/host'))
    from test_web_batch_handlers import extract_block

    glasses = (c / 'G2_Glasses.cpp').read_text()
    protocol = (c / 'System_G2_Protocol.cpp').read_text()

    # 1. The hex helper and the sid names, compiled and executed.
    cap = re.search(r'static constexpr size_t kG2UnknownSidHexMax = 64;[^\n]*\n', glasses)
    assert cap, 'the cap is 64 bytes'
    helper = extract_block(glasses, 'static size_t g2HexCompact(const uint8_t* data, size_t len, char* out, size_t cap) ')
    names = extract_block(protocol, 'const char* g2sidName(uint8_t sid) ')
    assert names.count('case 0x12: return "QuickNote?";') == 1

    # 2. The default branch of the RX sid switch.
    start = glasses.index('    default:\n      // An unmapped sid, e.g. 0x12')
    end = glasses.index('      break;\n  }\n}\n', start)
    branch = glasses[start:end]
    gate = branch.index('if (isDebugOutputEnabled(DEBUG_G2)) {')
    assert gate < branch.index('char hex[2 * kG2UnknownSidHexMax + 1];') < \
        branch.index('g2HexCompact(env.payload, env.payloadLen, hex, sizeof(hex));') < \
        branch.index('DEBUG_G2F('), 'the hex is built only when the line will print'
    assert branch.count('DEBUG_G2F(') == 1 and 'BROADCAST' not in branch and 'INFO_' not in branch
    fmt = re.search(r'DEBUG_G2F\(("(?:[^"\\]|\\.)*")', branch)
    assert fmt and fmt.group(1) == '"[G2-%c] sid=0x%02X (%s) flag=0x%02X len=%u pb=%s%s"', fmt
    assert 'env.payloadLen > kG2UnknownSidHexMax ? "..." : ""' in branch
    for forbidden in ('send', 'Send', 'g2Build', 'reply', 'Reply', 'systemEventPost', '= ', '++', 'Queue',
                      'notify', 'Notify'):
        code = re.sub(r'//[^\n]*', '', branch)
        assert forbidden not in code.replace('== ', '').replace('!= ', '').replace('char hex[', ''), forbidden
    # The cap and the helper sit above the RX dispatcher that uses them.
    assert glasses.index('static size_t g2HexCompact(') < start

    unit = HARNESS.replace('// INSERT_PRODUCTION', cap.group(0) + helper + '\n' + names + '\n'
                           + '#define FORMAT ' + fmt.group(1) + '\n')
    with tempfile.TemporaryDirectory(prefix='hw1-g2-sid-') as tmp:
        src = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        src.write_text(unit)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-format-security', str(src), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)
    print('G2 RX default branch: DEBUG_G2-only payload hex, sid 0x12 naming, logging only passed')


if __name__ == '__main__':
    main()
