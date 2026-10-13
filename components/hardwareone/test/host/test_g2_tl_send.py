#!/usr/bin/env python3
"""Compile the Conversate TL send bookkeeping (G2_Glasses.cpp) unchanged with host stubs.

The pump notes each final caption it writes, the RX path notes the glasses'
COMM_RSP for it, and g2ConvTLFlush prints both as numeric TL send lines; this
checks the pairing, duplicate/foreign/stale acknowledgements, reuse of the
pending slots and the overflow count. No board or BLE involved.
"""
import re, subprocess, sys, tempfile
from pathlib import Path
SRC = (Path(__file__).resolve().parents[2] / 'G2_Glasses.cpp').read_text()
start = SRC.index('// Live timing (TL send), all on g2_ctrl_owner')
end = SRC.index('}  // namespace', start)
state = SRC[start:end]
def func(marker):
    i = SRC.index(marker); j = SRC.index('\n}\n', i) + 3
    return SRC[i:j]
ack, flush = func('static __attribute__((noinline)) void g2ConvTLNoteAck(uint32_t magic, uint32_t error) {'), func('static __attribute__((noinline)) void g2ConvTLFlush(')
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static uint32_t clockMs = 0;
static uint32_t millis() { return clockMs; }
static std::vector<std::string> lines;
#define ESP_LOGI(tag, fmt, ...) do { char b[160]; snprintf(b, sizeof(b), fmt, __VA_ARGS__); lines.push_back(b); } while (0)
#define ESP_LOGW ESP_LOGI
#define EXT_RAM_BSS_ATTR
namespace {
''' + state + '''}  // namespace
static void g2ConvTLNoteSend(uint32_t seq, uint32_t magic) { gConvTLSendSeq = seq; g2ConvTLNoteSendDone(magic); }
''' + ack + flush + r'''
int main() {
  clockMs = 1000; (gConvTLSendStartMs = 1000, clockMs = 1000 + 20, g2ConvTLNoteSend(1, 5));
  g2ConvTLFlush();
  assert(lines.size() == 1 && lines[0] == "TL send seq=1 t=1000 ack_ms=-1 tx_ms=20 ok=1");
  clockMs = 1060; g2ConvTLNoteAck(5, 0); g2ConvTLNoteAck(5, 0);  // a duplicate echo is ignored
  g2ConvTLNoteAck(99, 0);                                         // a draft's or menu's ACK: not ours
  g2ConvTLFlush();
  assert(lines.size() == 2 && lines[1] == "TL send seq=1 t=1000 ack_ms=60 err=0");
  (gConvTLSendStartMs = 2000, clockMs = 2000 + 3, g2ConvTLNoteSend(2, 0));                                // TX failed: no ack expected
  clockMs = 2005; g2ConvTLNoteAck(0, 0);
  (gConvTLSendStartMs = 3000, clockMs = 3000 + 15, g2ConvTLNoteSend(3, 7));
  clockMs = 3000 + 10000; g2ConvTLNoteAck(7, 0);                  // stale (magics wrap at 250)
  g2ConvTLFlush();
  assert(lines.size() == 4 && lines[2] == "TL send seq=2 t=2000 ack_ms=-1 tx_ms=3 ok=0" &&
         lines[3] == "TL send seq=3 t=3000 ack_ms=-1 tx_ms=15 ok=1");
  // Nine finals in flight: the oldest pending entry is reused; acks come out of order.
  for (uint32_t i = 0; i < 9; ++i) (gConvTLSendStartMs = 20000 + i, clockMs = 20000 + i + 1, g2ConvTLNoteSend(10 + i, 100 + i));
  clockMs = 20100; g2ConvTLNoteAck(108, 4); g2ConvTLNoteAck(100, 0); g2ConvTLNoteAck(101, 0);
  g2ConvTLFlush();
  assert(lines.size() == 4 + 9 + 2);
  assert(lines[13] == "TL send seq=18 t=20008 ack_ms=92 err=4" && lines[14] == "TL send seq=11 t=20001 ack_ms=99 err=0");
  // More than 16 unprinted events: counted, never overwritten.
  for (uint32_t i = 0; i < 20; ++i) (gConvTLSendStartMs = 30000, clockMs = 30000 + 1, g2ConvTLNoteSend(50 + i, 0));
  lines.clear(); g2ConvTLFlush();
  assert(lines.size() == 17 && lines[0] == "TL send seq=50 t=30000 ack_ms=-1 tx_ms=1 ok=0" && lines[16] == "TL send_lost n=4");
  lines.clear(); g2ConvTLFlush(); assert(lines.empty());
  puts("TL send bookkeeping: write/ack lines, duplicates, foreign and stale acks, wrap and overflow passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    unit = Path(tmp) / 't.cpp'; unit.write_text(harness)
    flags = ['-fsanitize=address,undefined', '-g'] if '--sanitize' in sys.argv else []
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, str(unit), '-o', str(Path(tmp) / 't')], check=True)
    subprocess.run([str(Path(tmp) / 't')], check=True)
