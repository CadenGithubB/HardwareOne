#!/usr/bin/env python3
"""Actual CM5 recorder/microphone source choice for a requested (dictation) source.

Splices the source-selection blocks of initMicrophone() and startRecordingInternal()
into controlled HAL fakes: NONE must keep the micsource behaviour exactly, and a
requested source must be used or refused, never swapped for another microphone.
The recorder never re-sources a running mic for a request: that mic belongs to
whoever opened it, and nothing would hand it back when the exchange ends (R15).
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def between(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


HARNESS = r'''#include <cassert>
#include <cstdio>
#include <string>
// Format-checked like the firmware's printf-style log macros.
#define WARN_SYSTEMF(...) ((void)snprintf(nullptr, 0, __VA_ARGS__))
#define INFO_MIC_LIFECYCLEF(...) ((void)snprintf(nullptr, 0, __VA_ARGS__))
enum AudioSource { AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
static struct { std::string micSource = "auto"; } gSettings;
static bool pdm = true, g2 = true, busy = false, stopOK = true, initOK = true;
static AudioSource active = AUDIO_SRC_NONE, initRequested = AUDIO_SRC_NONE;
static int stops = 0, inits = 0;
static bool audioSourceAvailable(AudioSource s) {
  return s == AUDIO_SRC_LOCAL_PDM ? pdm : s == AUDIO_SRC_G2_LEFT && g2;
}
static bool audioSetSource(AudioSource s) {
  if (s != AUDIO_SRC_NONE && !audioSourceAvailable(s)) return false;
  active = s; return true;
}
static AudioSource audioGetSource() { return active; }
static bool micRecordingBusy() { return busy; }
static const char* micSourceName() { return "fake"; }
static bool stopMicrophone() { ++stops; return stopOK; }
// initMicrophone(): the real selection, then the HAL's PDM-first start.
static bool initSelect(AudioSource preferred) {
INIT_BLOCK
  return true;
}
// Same default as System_Microphone.h: NONE = the micsource setting.
static bool initMicrophone(AudioSource preferred = AUDIO_SRC_NONE) {
  ++inits; initRequested = preferred;
  if (!initOK || !initSelect(preferred)) return false;
  if (active == AUDIO_SRC_NONE) active = pdm ? AUDIO_SRC_LOCAL_PDM : AUDIO_SRC_G2_LEFT;
  return true;
}
// startRecordingInternal(): setting re-resolve and request check, before the claim.
static bool recorderSelect(AudioSource preferred) {
RECORDER_BLOCK
  return true;
}
static void reset(const char* setting, AudioSource incumbent) {
  gSettings.micSource = setting; pdm = g2 = stopOK = initOK = true; busy = false;
  active = incumbent; initRequested = AUDIO_SRC_NONE; stops = inits = 0;
}
int main() {
  // NONE: exactly the persisted preference (pdm/g2 when available, else auto).
  const char* settings[] = {"auto", "pdm", "g2"};
  for (const char* setting : settings) for (int mask = 0; mask < 4; ++mask) {
    reset(setting, AUDIO_SRC_LOCAL_PDM); pdm = mask & 1; g2 = mask & 2;
    assert(initSelect(AUDIO_SRC_NONE));
    const std::string s = setting;
    const AudioSource expect = s == "pdm" && pdm ? AUDIO_SRC_LOCAL_PDM
        : s == "g2" && g2 ? AUDIO_SRC_G2_LEFT : AUDIO_SRC_NONE;
    assert(active == expect);
  }
  // A request replaces the setting, either way round, and never falls back.
  reset("pdm", AUDIO_SRC_NONE); assert(initSelect(AUDIO_SRC_G2_LEFT) && active == AUDIO_SRC_G2_LEFT);
  reset("g2", AUDIO_SRC_NONE); assert(initSelect(AUDIO_SRC_LOCAL_PDM) && active == AUDIO_SRC_LOCAL_PDM);
  reset("auto", AUDIO_SRC_NONE); g2 = false; assert(!initSelect(AUDIO_SRC_G2_LEFT) && active == AUDIO_SRC_NONE);

  // Recorder, NONE: only a g2 setting re-resolves (an absent G2 never takes PDM away).
  reset("g2", AUDIO_SRC_LOCAL_PDM); assert(recorderSelect(AUDIO_SRC_NONE) && stops == 1 && inits == 1);
  assert(initRequested == AUDIO_SRC_NONE && active == AUDIO_SRC_G2_LEFT);
  reset("g2", AUDIO_SRC_LOCAL_PDM); g2 = false; assert(recorderSelect(AUDIO_SRC_NONE) && !stops && active == AUDIO_SRC_LOCAL_PDM);
  reset("g2", AUDIO_SRC_LOCAL_PDM); busy = true; assert(recorderSelect(AUDIO_SRC_NONE) && !stops);
  reset("g2", AUDIO_SRC_LOCAL_PDM); stopOK = false; assert(recorderSelect(AUDIO_SRC_NONE) && active == AUDIO_SRC_LOCAL_PDM);
  reset("g2", AUDIO_SRC_LOCAL_PDM); initOK = false; assert(!recorderSelect(AUDIO_SRC_NONE));
  for (const char* setting : {"auto", "pdm"}) {
    reset(setting, AUDIO_SRC_G2_LEFT); assert(recorderSelect(AUDIO_SRC_NONE) && !stops && active == AUDIO_SRC_G2_LEFT);
    reset(setting, AUDIO_SRC_LOCAL_PDM); assert(recorderSelect(AUDIO_SRC_NONE) && !stops && active == AUDIO_SRC_LOCAL_PDM);
  }
  // Recorder, requested: the running mic is used only when it is already on that
  // source. Otherwise the start is refused with the incumbent untouched (never
  // stopped or restarted), whatever the setting, availability or recorder state.
  const AudioSource sources[] = {AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT};
  for (const char* setting : settings) for (AudioSource incumbent : sources)
  for (AudioSource requested : sources) for (int mask = 0; mask < 32; ++mask) {
    reset(setting, incumbent);
    pdm = mask & 1; g2 = mask & 2; busy = mask & 4; stopOK = mask & 8; initOK = mask & 16;
    assert(recorderSelect(requested) == (incumbent == requested));
    assert(!stops && !inits && active == incumbent);
  }
  puts("Recorder requested source: setting path unchanged, request used or refused, never re-sourced");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    mic = (SOURCE / 'System_Microphone.cpp').read_text()
    init_block = between(mic, '  if (preferred != AUDIO_SRC_NONE && !audioSourceAvailable(preferred)) {',
                         '  if (!audioCaptureStart("mic"')
    recorder_block = between(mic, '  if (preferred == AUDIO_SRC_NONE &&\n      gSettings.micSource == "g2"',
                             '\n  // This claim MUST precede every write')
    # The spliced blocks must stay ahead of the hardware start and the claim.
    init_body = mic[mic.index('bool initMicrophone(AudioSource preferred) {'):]
    assert init_body.index(init_block) < init_body.index('audioCaptureStart("mic"')
    assert 'micRecordingClaimStart' not in recorder_block
    unit = HARNESS.replace('INIT_BLOCK', init_block).replace('RECORDER_BLOCK', recorder_block)
    with tempfile.TemporaryDirectory(prefix='hw1-mic-requested-') as tmp:
        cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        cpp.write_text(unit)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
