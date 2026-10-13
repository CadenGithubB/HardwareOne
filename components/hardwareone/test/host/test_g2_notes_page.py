#!/usr/bin/env python3
"""The production Notes lens page (G2_Page_Notes.cpp), run, plus its wiring.

1. Splices G2_Page_Notes.cpp (includes stripped) with the real transcription UI
   policy and text pager into g2_notes_page_harness.cpp and runs it: command
   strings, JSON replies, views, holds on the page and through the global hold
   action, no container rebuild while a note records, release before start,
   stopAfterStart, stale replies, refusal without navigation, headless notes,
   privacy and the tier row (with a card in, the other tier's notes).
2. Compiles the production Apps launcher rows (9 with every app; Notes only
   with QuickNote), g2PageHoldsHijackAlive() (its truth table on local-STT
   and CM5 builds), the container-ready mic re-arm (a hold note's start can
   arm the mic before the Notes CREATE lands) and the tap-label cache (Notes
   rows are never cached) spliced out of G2_Glasses.cpp.
3. Pins the registration: page id 24, the page registry at 22 of 23, the hold
   handler, the tick, the watchdog keep-alive and the build list; and that no
   log line in G2_Glasses.cpp can name a Notes row (R22).
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

APPS_HARNESS = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define ENABLE_DICTATION 1
#define ENABLE_MAPS 1
#define ENABLE_LLM_BACKEND 1
#define ENABLE_AUTOMATION 1
#define ENABLE_R1_HEALTH 1
namespace MapCore {
inline bool hasValidMap() { return true; }
inline int getAvailableMaps(char (*)[96], int) { return 1; }
}
// INSERT_PRODUCTION
int main() {
  const char* labels[G2_APPS_MAX_ROWS + 2] = {};
  G2AppRow ids[G2_APPS_MAX_ROWS + 2] = {};
  const size_t n = g2AppsBuildRows(labels, ids, G2_APPS_MAX_ROWS + 2);
  size_t notes = 0, at = 0;
  for (size_t i = 0; i < n; ++i)
    if (ids[i] == APP_ROW_NOTES) { ++notes; at = i; }
  const bool ok = n <= G2_APPS_MAX_ROWS &&
      (ENABLE_QUICKNOTE ? n == 9 && G2_APPS_MAX_ROWS == 9 && notes == 1 &&
                          !std::strcmp(labels[at], "Notes") && ids[at - 1] == APP_ROW_TRANSCRIPTION
                        : n == 8 && notes == 0);
  if (!ok) std::fprintf(stderr, "FAIL apps rows: n=%u notes=%u\n", (unsigned)n, (unsigned)notes);
  return ok ? 0 : 1;
}
'''

ALIVE_HARNESS = r'''
#include <cstdio>
#include <initializer_list>
enum G2HijackPage { G2_HIJACK_PAGE_APPS = 11, G2_HIJACK_PAGE_TRANSCRIPTION = 23, G2_HIJACK_PAGE_NOTES = 24 };
static G2HijackPage page = G2_HIJACK_PAGE_APPS;
static bool stt = false, note = false;
G2HijackPage g2GetHijackPage() { return page; }
bool sttAnySessionActive() { return stt; }
bool quickNoteActive() { return note; }
// INSERT_PRODUCTION
int main() {
  int failures = 0;
  for (G2HijackPage p : {G2_HIJACK_PAGE_APPS, G2_HIJACK_PAGE_TRANSCRIPTION, G2_HIJACK_PAGE_NOTES})
    for (int s = 0; s < 2; ++s)
      for (int n = 0; n < 2; ++n) {
        page = p; stt = s; note = n;
        const bool want = (ENABLE_LOCAL_STT && p == G2_HIJACK_PAGE_TRANSCRIPTION && s) || n;
        if (g2PageHoldsHijackAlive() != want) {
          std::fprintf(stderr, "FAIL keep-alive page=%d stt=%d note=%d\n", (int)p, s, n);
          ++failures;
        }
      }
  return failures ? 1 : 0;
}
'''

# A hold from another page submits the note's start while the Notes CREATE is
# still in flight, so the start can arm the mic into the torn-down lens (which
# sets gMicStreamOn). The container-ready edge must re-arm regardless.
REARM_HARNESS = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#define ENABLE_MICROPHONE 1
#define DEBUG_G2F(...) do {} while (0)
struct { bool containerReady = false, containerIsList = false; uint32_t containerWidgetId = 0; } gLens;
[[maybe_unused]] static volatile bool gMicStreamOn = true;
static bool wanted = false;
static int gives = 0, beatSem = 0;
static int* gBeatSem = &beatSem;
static void xSemaphoreGive(int*) { ++gives; }
static bool g2MicCaptureWanted() { return wanted; }
static std::atomic<bool> gMicReassertNow{false};
// INSERT_PRODUCTION
int main() {
  int failures = 0;
  for (int ready = 0; ready < 2; ++ready)
    for (int w = 0; w < 2; ++w) {
      wanted = w; gives = 0; gMicReassertNow.store(false); gMicStreamOn = true;
      g2LensApplyContainer(ready, true, 7);
      const bool want = ready && w;
      if (gMicReassertNow.load() != want || gives != (want ? 1 : 0) || gLens.containerReady != (bool)ready) {
        std::fprintf(stderr, "FAIL re-arm ready=%d wanted=%d\n", ready, w);
        ++failures;
      }
    }
  return failures ? 1 : 0;
}
'''

# The tap-label cache never holds a Notes row ("hh:mm <the note's first
# words>"); every other page's rows still name the tapped row in the log.
LABELS_HARNESS = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define EXT_RAM_BSS_ATTR
enum G2HijackPage { G2_HIJACK_PAGE_APPS = 11, G2_HIJACK_PAGE_NOTES = 24 };
static G2HijackPage page = G2_HIJACK_PAGE_APPS;
G2HijackPage g2GetHijackPage() { return page; }
// INSERT_PRODUCTION
int main() {
  int failures = 0;
  const char* apps[] = {"<- Back", "Notes"};
  const char* notes[] = {"<- Apps", "14:05 Call the dentist about"};
  char out[kTapLabelWidth];
  tapLabelCacheStore(apps, 2);
  tapLabelCachePeek(1, out, sizeof(out));
  if (std::strcmp(out, "Notes") || g2HijackRowsPrivate()) { std::fprintf(stderr, "FAIL apps label\n"); ++failures; }
  page = G2_HIJACK_PAGE_NOTES;
  tapLabelCacheStore(notes, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    tapLabelCachePeek(i, out, sizeof(out));
    if (std::strcmp(out, "?")) { std::fprintf(stderr, "FAIL notes row %u cached\n", (unsigned)i); ++failures; }
  }
  for (const auto& row : gTapLabels)
    if (std::strstr(row, "dentist")) { std::fprintf(stderr, "FAIL notes text in cache\n"); ++failures; }
  if (!g2HijackRowsPrivate()) { std::fprintf(stderr, "FAIL notes rows not private\n"); ++failures; }
  return failures ? 1 : 0;
}
'''

LOG_CALL = re.compile(r'\b(?:BROADCAST_PRINTF\w*|DEBUG_\w+|INFO_\w+|WARN_\w+|ERROR_\w+)\(')


def log_calls(text):
    """(offset, argument text) of every log macro call, strings and nested
    parens kept."""
    for m in LOG_CALL.finditer(text):
        pos, depth, quote = m.end(), 1, ''
        while depth:
            ch = text[pos]
            if quote:
                if ch == '\\':
                    pos += 1
                elif ch == quote:
                    quote = ''
            elif ch in '"\'':
                quote = ch
            elif ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            pos += 1
        yield m.start(), text[m.end():pos - 1]


def ordered(text, *needles):
    pos = -1
    for needle in needles:
        nxt = text.find(needle, pos + 1)
        assert nxt > pos, f'expected {needle!r} after offset {pos}'
        pos = nxt


def build_and_run(cxx, unit_text, tmp, name, extra, sanitize):
    unit = Path(tmp) / f'{name}.cpp'
    exe = Path(tmp) / name
    unit.write_text(unit_text)
    cmd = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *extra, str(unit), '-o', str(exe)]
    if sanitize:
        cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(cmd, check=True, timeout=180)
    subprocess.run([str(exe)], check=True, timeout=60)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    c = a.component.resolve()
    sys.path.insert(0, str(c / 'test/host'))
    from test_web_batch_handlers import extract_block

    # A source-only tree (the base copy) builds with this tree's libraries.
    libs = c.parent / 'hardwareone_libs'
    if not libs.is_dir():
        libs = HERE.parents[2] / 'hardwareone_libs'
    page = (c / 'G2_Page_Notes.cpp').read_text()
    glasses = (c / 'G2_Glasses.cpp').read_text()

    # 1. The page itself.
    harness = (HERE / 'g2_notes_page_harness.cpp').read_text()
    production = re.sub(r'^#include.*\n', '', page, flags=re.M)
    for tag, value in (('// INSERT_PRODUCTION', production),
                       ('// INSERT_POLICY', (c / 'Transcription_UI_Policy.h').read_text().replace('#pragma once', '')),
                       ('// INSERT_PAGER', (c / 'System_TextPager.h').read_text().replace('#pragma once', ''))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    # 2. Apps rows and the watchdog keep-alive, compiled from G2_Glasses.cpp.
    start = glasses.index('enum G2AppRow : uint8_t {')
    rows_fn = extract_block(glasses, 'static size_t g2AppsBuildRows(const char** labels, G2AppRow* ids, size_t cap) ')
    apps = glasses[start:glasses.index(rows_fn) + len(rows_fn)]
    alive = extract_block(glasses, 'static bool g2PageHoldsHijackAlive() ')
    rearm = extract_block(glasses, 'void g2LensApplyContainer(bool ready, bool isList, uint32_t widgetId) ')
    peek = extract_block(glasses, 'static void tapLabelCachePeek(uint32_t idx, char* out, size_t cap) ')
    labels = glasses[glasses.index('static constexpr size_t kTapLabelRows'):glasses.index(peek) + len(peek)]
    with tempfile.TemporaryDirectory(prefix='hw1-g2-notes-') as tmp:
        build_and_run(a.cxx, harness, tmp, 'notes_page',
                      ['-I', str(c), '-I', str(libs / 'ArduinoJson/src')],
                      a.sanitize)
        for quicknote in (1, 0):
            build_and_run(a.cxx, f'#define ENABLE_QUICKNOTE {quicknote}\n' +
                          APPS_HARNESS.replace('// INSERT_PRODUCTION', apps),
                          tmp, f'apps_rows_{quicknote}', [], a.sanitize)
        for local in (1, 0):
            build_and_run(a.cxx, f'#define ENABLE_LOCAL_STT {local}\n' +
                          ALIVE_HARNESS.replace('// INSERT_PRODUCTION', alive),
                          tmp, f'keep_alive_{local}', [], a.sanitize)
        build_and_run(a.cxx, REARM_HARNESS.replace('// INSERT_PRODUCTION', rearm),
                      tmp, 'mic_rearm', [], a.sanitize)
        build_and_run(a.cxx, LABELS_HARNESS.replace('// INSERT_PRODUCTION', labels),
                      tmp, 'tap_labels', [], a.sanitize)

    # 3. Registration and wiring.
    header = (c / 'G2_Glasses.h').read_text()
    assert 'G2_HIJACK_PAGE_NOTES           = 24,' in header
    notes_h = (c / 'G2_Page_Notes.h').read_text()
    for decl in ('bool g2NotesHandleHold(uint8_t phase, uint8_t source);',
                 'bool g2NotesHoldBegin(uint32_t pressId, const char** why);',
                 'void g2NotesHoldEnd(uint32_t pressId, uint32_t heldMs);',
                 'void g2NotesHoldAbort(uint32_t pressId, G2HoldReason reason);'):
        assert decl in notes_h, decl
    assert '*why = "quicknote-not-built";' in notes_h, 'a build without QuickNote refuses like Off'
    module = extract_block(glasses, 'static const G2PageModule kNotesPage = ')
    assert '"notes", nullptr,' in module and \
        'g2BuildNotesInfo, g2ShowNotesMenu, g2NotesHandleTap,' in module and \
        'G2_HIJACK_PAGE_NOTES, 0, "<- Apps", false, nullptr,' in module
    register = extract_block(glasses, 'static void registerG2Pages(void) ')
    assert len(re.findall(r'^\s*g2RegisterPage\(k', register, re.M)) == 22, \
        'every page on a full board: 22 of G2_PAGE_REGISTRY_MAX'
    assert glasses.count('#define G2_PAGE_REGISTRY_MAX 23') == 1
    ordered(register, 'g2RegisterPage(kTranscriptionPage);', '#if ENABLE_QUICKNOTE',
            'g2RegisterPage(kNotesPage);',
            'g2RegisterPageHold(G2_HIJACK_PAGE_NOTES, g2NotesHandleHold);', '#endif')
    assert 'static_assert(G2_HIJACK_PAGE_NOTES < kG2PageHoldSlots,' in glasses
    tap = extract_block(glasses, 'static void g2AppsHandleTap(uint32_t idx) ')
    ordered(tap, '#if ENABLE_QUICKNOTE', 'case APP_ROW_NOTES:  g2ShowNotesMenu();', '#endif')
    tick = extract_block(glasses, 'void g2Tick() ')
    ordered(tick, 'g2TranscriptionTick();', 'g2NotesTick();', 'g2HoldExpireTick();')
    beat = extract_block(glasses, 'static void heartbeatWorkerTask(void* /*arg*/) ')
    ordered(beat, 'if (g2FsmHijackActive() && gHijackStartedMs > 0 && g2PageHoldsHijackAlive())',
            'gHijackStartedMs = millis();', '(millis() - gHijackStartedMs) > HIJACK_SAFETY_MS',
            'sendHijackShutdown("safety-timeout");')
    assert 'sttAnySessionActive' not in beat, 'the keep-alive policy lives in g2PageHoldsHijackAlive'
    assert glasses.count('g2PageHoldsHijackAlive()') == 2   # definition + the watchdog
    # No log line names a Notes row (R22). The tap log's label is chosen,
    # private-aware, before any line prints it; the ListEvent's wire item name
    # is redacted on that page; nothing else logs a wire name or cached label.
    assert 'return g2GetHijackPage() == G2_HIJACK_PAGE_NOTES;' in \
        extract_block(glasses, 'static bool g2HijackRowsPrivate() ')
    tap_fn = extract_block(glasses, 'static void handleHijackMenuTap(uint32_t idx, const char* wireName,\n'
                           '                                uint32_t presentationEpoch) ')
    ordered(tap_fn, 'char lbl[kTapLabelWidth];', 'if (g2HijackRowsPrivate()) {',
            'snprintf(lbl, sizeof(lbl), "private");', '} else if (wireName && wireName[0]) {',
            'tapLabelCachePeek(idx, lbl, sizeof(lbl));')
    chosen = tap_fn.index('tapLabelCachePeek(idx, lbl, sizeof(lbl));')
    for at, args in log_calls(tap_fn):
        assert not re.search(r'\blbl\b', args) or at > chosen, args
    list_event = [args for _, args in log_calls(glasses) if 'ListEvent: container=' in args]
    assert len(list_event) == 1 and \
        'iname[0] && g2HijackRowsPrivate() ? "private" : iname,' in list_event[0]
    for _, args in log_calls(glasses):
        if 'ListEvent: container=' not in args:
            assert not re.search(r'\b(?:iname|wireName|gTapLabels)\b', args), args
    cmake = (c / 'CMakeLists.txt').read_text()
    ordered(cmake, 'G2_Page_Transcription.cpp', 'G2_Page_Notes.cpp')
    assert re.search(r'set_source_files_properties\([^)]*G2_Page_Notes\.cpp[^)]*"-Os"', cmake)

    # The page draws only through text-child patches while a note may be
    # capturing; its one container (re)build is gated on that.
    render = extract_block(page, 'static void render() ')
    assert page.count('g2ShowMixedListText2(') == 1
    ordered(render, 'const bool micBusy = s.live || s.rec == Rec::Recording;',
            'if (sameRows || (drawn && micBusy)) {', 'g2UpdateMixedTextChild("nthead"',
            'g2UpdateMixedTextChild("notes"', 'if (!shown && !(drawn && micBusy) &&',
            'g2ShowMixedListText2(')
    # Mutations only through the wearer's `note` commands; nothing on the page
    # logs, posts or writes glasses settings (R1, R22).
    for forbidden in ('systemEventPost', 'DEBUG_', 'BROADCAST_PRINTF', 'INFO_', 'WARN_',
                      'Serial', 'quickNoteStart', 'quickNoteStop', 'dictationApp',
                      'g2BuildSettingInfoWrite', 'QuickNoteStore'):
        assert forbidden not in production, forbidden
    # Times come from `note list` ("when"); the page formats none itself and
    # never shows "--:--".
    assert 'localtime_r' not in production and '--:--' not in production
    assert 'str(row, "when", "earlier")' in production
    print('G2 Notes page: views, holds, no-rebuild recording, time labels, keep-warm, wiring passed')


if __name__ == '__main__':
    main()
