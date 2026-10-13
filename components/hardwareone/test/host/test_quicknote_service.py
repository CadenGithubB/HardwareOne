#!/usr/bin/env python3
"""The production QuickNote service and `note` CLI, run against fakes.

Splices System_QuickNote.cpp (service, worker pass and CLI) with its real
header types, the QuickNote/transcript policies, the production CommandArgs
parser and the dictation lease types into quicknote_service_harness.cpp, and
runs it once per transcription backend (on-device and Pi/CM5). Then checks
the source-level wiring the harness cannot see: event rows and their
queue-only defaults (R23), the CLI/settings registration and the native-voice
stand-down export (R16).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_espsr_runtime import function
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def clean(text):
    return re.sub(r'^#include.*\n|^#pragma once\n', '', text, flags=re.M)


def between(text, start, end):
    assert text.count(start) == 1, start
    begin = text.index(start)
    stop = text.index(end, begin)
    return text[begin:stop]


def ordered(text, *needles):
    pos = -1
    for needle in needles:
        nxt = text.find(needle, pos + 1)
        assert nxt > pos, f'expected {needle!r} after offset {pos}'
        pos = nxt


def unit_source():
    header = (COMPONENT / 'System_QuickNote.h').read_text()
    source = (COMPONENT / 'System_QuickNote.cpp').read_text()
    dictation = (COMPONENT / 'System_Dictation.h').read_text()
    transcript = (COMPONENT / 'System_Transcript.h').read_text()
    store = (COMPONENT / 'QuickNote_Store.h').read_text()
    types = extract_block(transcript, 'struct TranscriptStatus') + ';\n'
    for name in ('enum class DictationState', 'struct DictationSnapshot', 'struct DictationTextReceipt',
                 'enum class DictationAppPurpose', 'struct DictationAppLease',
                 'struct DictationAppSnapshot', 'struct DictationAppOptions'):
        types += extract_block(dictation, name) + ';\n'
    policies = '\n'.join(clean((COMPONENT / name).read_text()) for name in
                         ('Transcript_PathPolicy.h', 'Transcription_UI_Policy.h', 'QuickNote_Policy.h'))
    policies += '\n' + between(header, '// How the last note ended.', '#else')
    parser = extract_block((COMPONENT / 'System_Command.h').read_text(), 'class CommandArgs') + \
        ';\nconst String CommandArgs::empty_;\n'
    command = (COMPONENT / 'System_Command.cpp').read_text()
    parser += '\n'.join(function(command, s) for s in
                        ('CommandArgs::CommandArgs(', 'void CommandArgs::parse(', 'const String& CommandArgs::arg('))
    store_api = between(store, 'namespace QuickNoteStore {', '}  // namespace QuickNoteStore') + \
        '}  // namespace QuickNoteStore\n'
    production = between(source, 'using namespace QuickNotePolicy;', '#endif  // ENABLE_QUICKNOTE')
    ble = extract_block((COMPONENT / 'BLE_Peers.h').read_text(), 'struct BlePeerOwnerSession') + ';\n'
    unit = (HERE / 'quicknote_service_harness.cpp').read_text()
    for marker, value in (('// INSERT_BLE_OWNER', ble), ('// INSERT_TYPES', types),
                          ('// INSERT_POLICIES', policies), ('// INSERT_PARSER', parser),
                          ('// INSERT_STORE_API', store_api), ('// INSERT_PRODUCTION', production)):
        assert unit.count(marker) == 1, marker
        unit = unit.replace(marker, value)
    return unit


def check_wiring():
    c = COMPONENT
    source = (c / 'System_QuickNote.cpp').read_text()
    # The note is QuickNote-purpose, keeps no transcript copy, and only the
    # worker ends it; every dictation call for a note runs on that worker.
    start = function(source, 'bool quickNoteStart(')
    assert 'options.purpose = DictationAppPurpose::QuickNote;' in start
    assert 'options.saveTranscript = false;' in start
    assert 'xTaskCreateLogged(workerBody, "qnote", kWorkerStackBytes' in source
    for call in ('dictationAppRequestStop(', 'dictationAppCancel(', 'dictationAppPeekText(',
                 'dictationAppCommitText('):
        for name in ('bool quickNoteStop(', 'bool quickNoteCancel(', 'bool quickNoteSnapshot(',
                     'const char* commandNote('):
            assert call not in function(source, name), (call, name)
    # R22: note text never reaches a log line or an event.
    for line in source.splitlines():
        if re.search(r'INFO_SYSTEMF|WARN_SYSTEMF|systemEventPost', line):
            for private in ('gQnText', 'piece', 'title', 'sttText'):
                assert private not in line, line

    rows = (c / 'System_EventCatalogRows.h').read_text()
    ordered(rows, '"g2_long_release", SYSEVT_FAM_GLASSES)',
            'HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_NOTE_STARTED,        "note_started", SYSEVT_FAM_MEDIA)',
            'HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_NOTE_SAVED,          "note_saved", SYSEVT_FAM_MEDIA)',
            'HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_NOTE_FAILED,         "note_failed", SYSEVT_FAM_MEDIA)',
            '#endif')
    # R23: queue-only outcomes, an event-only start; never a banner or toast
    # (those also grant the G2 card sink, drawn over HW1's own page).
    rules = extract_block((c / 'System_Notifications.cpp').read_text(),
                          'static NotifRule notifDefaultRuleFor(uint8_t kind)')
    assert 'case SYSEVT_NOTE_SAVED:           return {NSINK_QUEUE, 1, 0, 0};' in rules
    assert 'case SYSEVT_NOTE_FAILED:          return {NSINK_QUEUE, 2, 0, 0};' in rules
    assert 'SYSEVT_NOTE_STARTED' not in rules
    for line in rules.splitlines():
        if 'SYSEVT_NOTE_' in line:
            assert 'BANNER' not in line and 'TOAST' not in line and 'ALL' not in line, line

    registry = (c / 'System_CommandRegistry.cpp').read_text()
    table = registry[registry.index('static constexpr CommandModule gCommandModules[] = {'):]
    ordered(table, 'transcriptionUICommands, &transcriptionUICommandsCount, CMD_MODULE_SENSOR, nullptr },',
            '#endif', '#if ENABLE_QUICKNOTE', '{ "note", ',
            'noteCommands, &noteCommandsCount, CMD_MODULE_SENSOR, nullptr },', '#endif')
    entry = source[source.index('const CommandEntry noteCommands[] = {'):]
    assert '{ "note", "Private spoken notes (QuickNote)", false, commandNote,' in entry, \
        'note is non-admin; authority is checked per action (R21)'

    settings = (c / 'System_Settings.cpp').read_text()
    for row in ('{ "notemaxsec", SETTING_U16, &gSettings.noteMaxSec, HW1_NOTE_MAX_SEC_DEFAULT, 0, nullptr, 10, 120,',
                '{ "notemic", SETTING_U8, &gSettings.noteMic, 0, 0, nullptr, 0, 2, "Note microphone", "0|Auto,1|Glasses,2|Board mic",'):
        assert settings.count(row) == 1, row
    ordered(settings, 'registerSettingsModule(&g2HoldSettingsModule);', '#endif', '#if ENABLE_QUICKNOTE',
            'registerSettingsModule(&quickNoteSettingsModule);')
    editors = settings[settings.index('const CommandEntry settingEditorCommands[]'):]
    for key in ('notemaxsec', 'notemic'):
        assert f'SETTING_EDITOR_CMD(cmd_set_{key},' in settings, key
        assert f'{{ "{key}",' in editors, key
    build = (c / 'System_BuildConfig.h').read_text()
    assert '#define HW1_NOTE_MAX_SEC_DEFAULT (ENABLE_LOCAL_STT ? 60 : 30)' in build

    g2 = (c / 'G2_Glasses.cpp').read_text()
    native = function(g2, 'bool g2NativeVoiceActive()')
    assert 'return g2EvenAiSessionIsActive() || g2ConversateActive();' in native
    header = (c / 'G2_Glasses.h').read_text()
    assert header.count('bool g2NativeVoiceActive();') == 1
    assert 'inline bool g2NativeVoiceActive() { return false; }' in header
    cmake = (c / 'CMakeLists.txt').read_text()
    ordered(cmake, 'QuickNote_Store.cpp', 'System_QuickNote.cpp')

    # Restamping: the clock-step chokepoint's resolve duty (main loop) wakes
    # the worker, which alone touches note files; builds without QuickNote
    # get an inline no-op. The worker's idle wait stays timed while the boot's
    # unfinished-file sweep is due, so a first note that never began still
    # gets it.
    clock = extract_block((c / 'System_Clock.cpp').read_text(), 'static void drainDuties() ')
    ordered(clock, 'if (pend & PEND_RESOLVE) {', 'resolvePendingUserCreationTimes();',
            'quickNoteClockSet();', '}')
    qn_header = (c / 'System_QuickNote.h').read_text()
    assert 'inline void quickNoteClockSet() {}' in qn_header.split('#else', 1)[1]
    assert function(source, 'void quickNoteClockSet(') .strip().endswith('{ wake(); }')
    worker = extract_block(source, 'void workerBody(void*) ')
    ordered(worker, 'gRestampDue || gSweepDue ? pdMS_TO_TICKS(kRestampRetryMs)', 'servicePass();',
            'restampPass();', 'sweepPass();')
    # Keep-warm exists only on local-STT builds and never loads from a note pass.
    service = extract_block(source, 'void servicePass() ')
    ordered(service, '#if ENABLE_LOCAL_STT', 'sttKeepWarm(kWarmHoldMs, false);', '#endif')
    warm = function(source, 'void quickNoteWarm(')
    ordered(warm, '#if ENABLE_LOCAL_STT', 'sttKeepWarm(kWarmHoldMs, true);', '#endif')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    check_wiring()
    unit = unit_source()
    with tempfile.TemporaryDirectory(prefix='hw1-quicknote-service-') as tmp:
        tmp = Path(tmp)
        # Same bounded host String as the HTTP suite, extended only for the
        # production CommandArgs parser's reserve/character append surface.
        arduino = (HERE / 'web_batch_stubs/Arduino.h').read_text().replace(
            '  String& operator+=(const String& other)',
            '  void reserve(size_t n) { text_.reserve(n); }\n'
            '  String& operator+=(char c) { text_ += c; return *this; }\n'
            '  String& operator+=(const String& other)')
        (tmp / 'Arduino.h').write_text(arduino)
        cpp = tmp / 'test.cpp'
        cpp.write_text(unit)
        for local in (1, 0):
            exe = tmp / f'test_local{local}'
            cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                   '-Wno-missing-field-initializers', '-Wno-unused-const-variable',
                   f'-DENABLE_LOCAL_STT={local}', '-I', str(tmp),
                   '-I', str(COMPONENT.parent / 'hardwareone_libs/ArduinoJson/src'), str(cpp), '-o', str(exe)]
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True, timeout=180)
            subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
