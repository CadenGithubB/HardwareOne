#!/usr/bin/env python3
"""Run the production text-entry lifecycle with microphone-entry fault injection."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define DEBUG_G2F(...) ((void)0)
// INSERT_HEADERS
using portMUX_TYPE = int;
using TaskHandle_t = void*;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
static int task;
TaskHandle_t xTaskGetCurrentTaskHandle() { return &task; }
void portENTER_CRITICAL(int* lock) { assert(!*lock); *lock = 1; }
void portEXIT_CRITICAL(int* lock) { assert(*lock); *lock = 0; }

static bool live = false, padOK = true, micOK = true, publishOK = true;
static bool revokeDuringPad = false, compoundOK = true, micReady = false;
static bool padEligible = true;
struct PadState { bool active, surfaceLive, dictationPermitted; uint8_t page; };
static PadState gKbdPad{};
constexpr uint8_t kKbdPageMic = 3;
constexpr uint32_t kKbdRowMic = 6;
static uint32_t activeSerial = 0, nextSerial = 0;
static uint32_t lifecycle = 2;
static int padStarts = 0, micStarts = 0, padEnds = 0, compounds = 0;
static int commits = 0, cancels = 0, abandons = 0;
static std::string committed;
uint32_t g2TextEntrySessionAllocateSerial() { return ++nextSerial; }
bool g2TextEntryCaptureAdmissionFence(uint32_t* life, uint32_t* gen, char* side) {
  *life = lifecycle; *gen = 3; *side = 'R'; return true;
}
bool g2TextEntrySessionPublish(uint32_t serial, uint32_t, uint32_t, char) {
  if (!publishOK) return false;
  activeSerial = serial; live = true; return true;
}
bool g2TextEntrySessionIsCurrent(uint32_t serial, uint32_t, uint32_t, char) {
  return live && serial == activeSerial;
}
bool g2TextEntrySessionFinishClaim(uint32_t serial, uint32_t, uint32_t, char) {
  if (!live || serial != activeSerial) return false;
  live = false; return true;
}
bool g2TextEntrySessionCleanupClaim(uint32_t serial, uint32_t, uint32_t, char) {
  return !live && serial == activeSerial;
}
void g2TextEntrySessionCleanupComplete(uint32_t, uint32_t, uint32_t, char) {}
void g2BumpMenuGen() {}
bool g2KbdPadBegin(const char*) {
  ++padStarts;
  if (revokeDuringPad) live = false;
  gKbdPad = {padOK, padOK, !g2TextEntryIsSecret(), 0};
  micReady = padOK; return padOK;
}
void g2KbdPadEnd() { ++padEnds; micReady = false; gKbdPad = {}; }
bool kbdPadSurfaceEligible() { return padEligible; }
void g2KbdPadHandleTap(uint32_t row) {
  assert(row == kKbdRowMic);
  assert(micReady && live && g2TextEntryIsActive());
  assert(!g2TextEntryIsSecret());
  ++micStarts;
  if (micOK) gKbdPad.page = kKbdPageMic;
}
// INSERT_PAD_START
void g2KbdPadNoteBufferText(const char*) {}
bool g2ShowMixedListText(const char* const*, size_t, const G2ContainerGeom&,
                       const G2TextChildSpec&) { ++compounds; return compoundOK; }
bool g2RelistMixedListText(const char* const*, size_t, const G2ContainerGeom&,
                         const G2TextChildSpec&) { return true; }
bool g2UpdateMixedTextChild(const char*, uint32_t, const char*) { return true; }
// INSERT_PRODUCTION

static void onCommit(const char* text) { ++commits; committed = text; }
static void onCancel() { ++cancels; }
static void onAbandon() { ++abandons; }
static void reset() {
  // Each case starts with no transport session or allocated pad.
  clearLocalSession();
  assert(gTextEntryExecDepth == 0);
  live = false; padOK = micOK = publishOK = compoundOK = true;
  revokeDuringPad = micReady = false;
  padEligible = true; gKbdPad = {};
  activeSerial = nextSerial = 0;
  lifecycle = 2;
  padStarts = micStarts = padEnds = compounds = commits = cancels = abandons = 0;
  committed.clear();
}
static TextEntryConfig config() {
  TextEntryConfig cfg;
  cfg.prompt = "Ask LLM";
  cfg.maxLen = 256;
  cfg.onCommit = onCommit;
  cfg.onCancel = onCancel;
  cfg.onAbandon = onAbandon;
  return cfg;
}
int main() {
  reset(); auto cfg = config();
  assert(!cfg.startWithMic && cfg.expectedLifecycleEpoch == 0);
  assert(g2BeginTextEntry(cfg) && padStarts == 1 && micStarts == 0);

  reset(); cfg = config(); cfg.startWithMic = true;
  assert(g2BeginTextEntry(cfg) && micStarts == 1 && !commits && !cancels);
  assert(g2TextEntryPadAppendText("hello Mingus") == 12);
  assert(!commits);  // Dictation still requires the wearer's Done action.
  g2TextEntryPadEvent('\n');
  assert(commits == 1 && committed == "hello Mingus" && padEnds == 1 && !live);

  reset(); cfg = config(); cfg.startWithMic = true; cfg.isSecret = true;
  assert(g2BeginTextEntry(cfg) && micStarts == 0);
  assert(g2TextEntryPadAppendText("private") == 0);

  reset(); cfg = config(); cfg.startWithMic = true; padOK = false;
  assert(g2BeginTextEntry(cfg) && compounds == 1 && micStarts == 0);
  g2TextEntryHandleTap(3);  // The legacy keyboard's ordinary Done row.
  assert(commits == 1);

  reset(); cfg = config(); cfg.startWithMic = true; micOK = false;
  assert(g2BeginTextEntry(cfg) && micStarts == 1 && g2TextEntryIsActive());
  assert(!cancels && !commits && !padEnds);
  g2TextEntryPadEvent('x'); g2TextEntryPadEvent('\n');
  assert(commits == 1 && committed == "x"); // Declined shortcut stays editable.

  reset(); cfg = config(); cfg.startWithMic = true; revokeDuringPad = true;
  assert(!g2BeginTextEntry(cfg) && micStarts == 0 && compounds == 0);
  assert(!g2TextEntryIsActive() && !commits && !cancels);
  const G2TextEntryCleanupToken token{activeSerial, 2, 3, 'R', {0,0,0}};
  assert(g2TextEntryCleanupAbandoned(token));
  assert(abandons == 1 && padEnds == 1 && !commits && !cancels);

  reset(); cfg = config(); cfg.startWithMic = true; publishOK = false;
  assert(!g2BeginTextEntry(cfg) && padStarts == 0 && micStarts == 0);

  reset(); cfg = config(); cfg.startWithMic = true;
  assert(g2BeginTextEntry(cfg));
  g2TextEntryPadEvent('\x1b');
  assert(cancels == 1 && commits == 0 && padEnds == 1 && !live);

  reset(); cfg = config(); cfg.startWithMic = true;
  assert(g2BeginTextEntry(cfg));
  cfg.startWithMic = false;
  assert(g2BeginTextEntry(cfg));
  assert(micStarts == 1 && abandons == 1 && padEnds == 1 && !commits && !cancels);

  reset(); cfg = config(); cfg.expectedLifecycleEpoch = 1; cfg.startWithMic = true;
  assert(!g2BeginTextEntry(cfg) && !padStarts && !micStarts && !live);
  cfg.expectedLifecycleEpoch = 2;
  assert(g2BeginTextEntry(cfg) && micStarts == 1);
  g2TextEntryPadEvent('x');
  const uint32_t currentSerial = activeSerial;
  cfg.expectedLifecycleEpoch = 1;
  assert(!g2BeginTextEntry(cfg));
  assert(activeSerial == currentSerial && g2TextEntryIsActive());
  assert(padStarts == 1 && micStarts == 1 && !padEnds && !abandons);
  g2TextEntryPadEvent('\n');
  assert(commits == 1 && committed == "x"); // Stale caller cannot replace a new entry.

  reset(); cfg = config(); assert(g2BeginTextEntry(cfg));
  gKbdPad.active = false; assert(!g2KbdPadStartMic() && !micStarts);
  gKbdPad.active = true; gKbdPad.surfaceLive = false;
  assert(!g2KbdPadStartMic() && !micStarts);
  gKbdPad.surfaceLive = true; gKbdPad.dictationPermitted = false;
  assert(!g2KbdPadStartMic() && !micStarts);
  gKbdPad.dictationPermitted = true; padEligible = false;
  assert(!g2KbdPadStartMic() && !micStarts);
  padEligible = true;
  assert(g2KbdPadStartMic() && micStarts == 1);
  assert(!g2KbdPadStartMic() && micStarts == 1); // Never toggle/stop an open mic page.
  puts("G2 text entry: mic opt-in, readiness, secrets, fallback, review/send and terminal cleanup passed");
}
'''


def strip_local_includes(text):
    return re.sub(r'^\s*#(?:include "[^"]+"|pragma once).*$', '', text,
                  flags=re.MULTILINE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    headers = strip_local_includes((COMPONENT / 'G2_Page_TextEntry.h').read_text())
    disabled_header = headers
    headers += f'\n#include "{COMPONENT / "G2_RenderTypes.h"}"\n'
    production = strip_local_includes((COMPONENT / 'G2_Page_TextEntry.cpp').read_text())
    pad_start = function((COMPONENT / 'G2_Glasses.cpp').read_text(), 'bool g2KbdPadStartMic(')
    unit = (HARNESS.replace('// INSERT_HEADERS', headers)
            .replace('// INSERT_PRODUCTION', production)
            .replace('// INSERT_PAD_START', pad_start))
    with tempfile.TemporaryDirectory(prefix='hw1-g2-text-entry-') as directory:
        cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(unit)
        command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   str(cpp), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)
        # Both unavailable-feature combinations retain the configuration API
        # and decline entry without invoking user callbacks.
        for bluetooth, glasses in [(0, 1), (1, 0)]:
            cpp.write_text(
                '#include <cassert>\n'
                f'#define ENABLE_BLUETOOTH {bluetooth}\n'
                f'#define ENABLE_G2_GLASSES {glasses}\n' + disabled_header + r'''
static void commit(const char*) { assert(false); }
int main() {
  TextEntryConfig cfg;
  assert(!cfg.startWithMic && cfg.expectedLifecycleEpoch == 0);
  cfg.startWithMic = true; cfg.expectedLifecycleEpoch = 2;
  cfg.onCommit = commit; cfg.maxLen = G2_TEXT_ENTRY_MAX_LEN;
  assert(!g2BeginTextEntry(cfg) && !g2TextEntryIsActive());
}
''')
            subprocess.run(command, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
