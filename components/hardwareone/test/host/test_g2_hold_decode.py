#!/usr/bin/env python3
"""G2 long-press (hold) decode: event names, RX hooks and the dispatch split.

Compiles the production osEventTypeName() spliced out of G2_Glasses.cpp and
checks every OsEventTypeList name 0-10, then pins the source order of the hold
hooks in handleDevEvent: each hold copy is taken before any gesture handling
that keys on 0..3 (List containers, TextEvent's `etype > 2` drop, the Sys
keyboard select). The producers (BLE control owner, main loop, cmd_exec) only
pair, record, gate and queue; page handlers, the global action and events run
only on g2_tap_disp; nothing writes glasses settings. Behaviour is exercised by
test_g2_hold_dispatch.py.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
// INSERT_PRODUCTION
int main() {
  static const char* const kExpect[] = {
    "CLICK", "SCROLL_TOP", "SCROLL_BOTTOM", "DOUBLE_CLICK", "FG_ENTER",
    "FG_EXIT", "ABNORMAL_EXIT", "SYSTEM_EXIT", "IMU_DATA_REPORT",
    "LONG_PRESS", "LONG_RELEASE",
  };
  int failures = 0;
  for (uint32_t i = 0; i < sizeof(kExpect) / sizeof(kExpect[0]); ++i) {
    if (std::strcmp(osEventTypeName(i), kExpect[i]) != 0) {
      std::fprintf(stderr, "FAIL type %u: %s\n", (unsigned)i, osEventTypeName(i));
      ++failures;
    }
  }
  static const uint32_t kUnnamed[] = {11u, 12u, 0x109u, 0xFFFFFFFFu};
  for (uint32_t i : kUnnamed) {
    if (std::strcmp(osEventTypeName(i), "?") != 0) {
      std::fprintf(stderr, "FAIL type %u must be unnamed\n", (unsigned)i);
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
'''


def section(source: str, start_marker: str, end_marker: str) -> str:
    start = source.index(start_marker)
    return source[start:source.index(end_marker, start)]


def ordered(text: str, *needles: str) -> None:
    pos = -1
    for needle in needles:
        nxt = text.find(needle, pos + 1)
        assert nxt > pos, f"expected {needle!r} after offset {pos}"
        pos = nxt


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    c = a.component.resolve()
    sys.path.insert(0, str(c / 'test/host'))
    from test_web_batch_handlers import extract_block

    source = (c / 'G2_Glasses.cpp').read_text()

    # 1. Production event names, compiled and executed.
    code = extract_block(source, 'static const char* osEventTypeName(uint32_t ev) ')
    assert code.count('LONG_PRESS') == 1 and code.count('LONG_RELEASE') == 1
    with tempfile.TemporaryDirectory(prefix='hw1-g2-hold-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(HARNESS.replace('// INSERT_PRODUCTION', code))
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit),
               '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)

    # 2. Hook placement inside handleDevEvent.
    dev = extract_block(source, 'static void handleDevEvent(')
    list_branch = section(dev, 'if (field == 1) {', '} else if (field == 2) {')
    text_branch = section(dev, '} else if (field == 2) {', '} else if (field == 3) {')
    sys_start = dev.index('} else if (field == 3) {')
    sys_branch = dev[sys_start:]

    list_hook = 'g2HoldIngest(t, etype, 0, G2HoldVia::List, rxGeneration,'
    text_hook = 'g2HoldIngest(t, etype, 0, G2HoldVia::Text, rxGeneration,'
    sys_hook = 'g2HoldIngest(t, etype, src, G2HoldVia::Sys, rxGeneration,'
    for branch, hook in ((list_branch, list_hook), (text_branch, text_hook),
                         (sys_branch, sys_hook)):
        assert branch.count(hook) == 1, hook
        assert dev.count(hook) == 1, hook
        guard = branch.rindex('if (g2HoldIsEvent(etype)) {', 0, branch.index(hook))
        tail = branch[branch.index(hook):]
        assert tail.index('continue;') < tail.index('}'), \
            'a hold hook must skip the rest of its branch'
        assert branch.index(hook) - guard < 120

    # List: after the event line, before every container dispatch.
    ordered(list_branch, 'DEBUG_G2_EVENTSF("[G2-%c] ListEvent:', list_hook,
            'strcmp(cname, "lstCam")', 'strcmp(cname, "lstHealth")',
            'tapDispatcherEnqueue(idx, iname)')
    # Text: after the event line, immediately before the `etype > 2` drop.
    ordered(text_branch, 'DEBUG_G2_EVENTSF("[G2-%c] TextEvent:', text_hook,
            'if (etype > 2) {', 'g2TextViewSnapshotForPacket(')
    between = text_branch[text_branch.index(text_hook):text_branch.index('if (etype > 2) {')]
    assert 'g2TextViewSnapshotForPacket' not in between and 'tapDispatcher' not in between
    # Sys: after the event line, FG overlay fusion and ring stamp; before the
    # keyboard select, image-probe, live-page and text-view gesture paths.
    ordered(sys_branch, 'DEBUG_G2_EVENTSF("[G2-%c] SysEvent:',
            'g2OverlayObserveE0(',
            'if (src == 2) gRingViaGlassesLastSeenMs = millis();', sys_hook,
            'const bool keyboardOwnsDouble = kbdPadSysDoubleOwned();',
            'tapDispatcherEnqueueKeyboardSelect(keyboardEpoch)',
            'if (gImgProbeHoldActive &&', 'livePageKickRefresh();',
            'if (etype == 7 &&', 'g2HoldAbortFromPacket(')

    # Event lines moved to the events sub-flag (which ORs the G2 master).
    for kind in ('ListEvent', 'TextEvent', 'SysEvent'):
        assert f'DEBUG_G2_EVENTSF("[G2-%c] {kind}: ' in dev, kind
        assert f'DEBUG_G2F("[G2-%c] {kind}: ' not in dev, kind

    # 3. Implicit releases: a firmware overlay rising over the page, page exits
    # (current packets only), terminal loss, main-loop expiry. HW1's overlay
    # fusion (hardware-observed) is FG_ENTER(4) = overlay up, FG_EXIT(5) =
    # dismissed, so FG_ENTER is what ends a hold the firmware covered (R11).
    assert sys_branch.count('t, rxGeneration, rxLifecycleEpoch, etype == 4);') == 1, \
        'the overlay fusion must keep FG_ENTER as the rise'
    # HW1's own replacement Shutdown echo (reason-1 SYSTEM_EXIT on the
    # expected-echo lease) is decided once and is no hold exit (E-3, probe
    # H2); test_g2_hold_dispatch.py runs this tail.
    ordered(sys_branch, 'bool ownShutdownEcho = false;', 'if (etype == 7 &&',
            'ownShutdownEcho = reason == 1 &&\n'
            '            g2ExpectedEchoForGeneration(t.side, rxGeneration);',
            'if (ownShutdownEcho) {', 'g2OverlayResetForPacket(',
            'g2TerminalInvalidate("fw-system-exit",',
            'if ((etype == 4 || etype == 5 || etype == 6 || etype == 7) &&\n'
            '          !ownShutdownEcho) {')
    assert sys_branch.count('ownShutdownEcho = ') == 2   # declaration + decision
    assert sys_branch.count('g2ExpectedEchoForGeneration(') == 1
    abort_call = sys_branch[sys_branch.index(
        'if ((etype == 4 || etype == 5 || etype == 6 || etype == 7) &&'):]
    assert abort_call.index('g2HoldAbortFromPacket(t, rxGeneration, rxLifecycleEpoch,') < 120
    for reason in ('etype == 4 ? G2HoldReason::Overlay :',
                   'etype == 5 ? G2HoldReason::FgExit :',
                   'etype == 6 ? G2HoldReason::AbnormalExit :',
                   'G2HoldReason::SystemExit'):
        assert reason in abort_call[:440], reason
    assert sys_branch.count('g2HoldAbortFromPacket(') == 1
    from_packet = extract_block(source, 'static void g2HoldAbortFromPacket(G2Temple& t, uint32_t rxGeneration,\n'
                                        '                                  uint32_t rxLifecycleEpoch,\n'
                                        '                                  G2HoldReason reason) ')
    ordered(from_packet, 'g2LifecyclePacketCurrent(t, rxGeneration, rxLifecycleEpoch)',
            'return;', 'g2HoldAbort(reason, t.side);')
    terminal = extract_block(source, 'static bool g2TerminalInvalidate(const char* reason, char side,\n'
                                     '                                 uint32_t generation,\n'
                                     '                                 uint32_t lifecycleEpoch,\n'
                                     '                                 uint32_t authorityEpoch,\n'
                                     '                                 bool sendShutdownWire) ')
    ordered(terminal, 'gWidgetTerminalLatched = 1;', 'g2HoldAbort(G2HoldReason::Terminal, side);')
    tick = extract_block(source, 'void g2Tick() ')
    assert 'g2HoldExpireTick();' in tick
    assert 'return' not in tick[:tick.index('g2HoldExpireTick();')], \
        'the hold tick must run on every main-loop pass, linked or not'
    # B2.6: an open press expires with the longest note (notemaxsec) + 15 s.
    assert 'static uint32_t g2HoldMaxOpenMs() { return quickNoteMaxMs() + 15000u; }' in source
    assert 'kHoldMaxOpenMs' not in source

    # 4. Producers (g2_ctrl_owner, main loop, cmd_exec) only pair, record,
    # sample the cheap gates, refresh the watchdog and queue a job: freshness
    # first, the pairing under its own spinlock, logging and queueing only
    # after it, and no handler, event, settings or wire traffic (R1/R3, R22).
    hold_section = section(source, '// Long-press ("hold") input: decode, pair, record and dispatch',
                           '// Called by g2_tap_disp after g2_ctrl_owner sees')
    for forbidden in ('g2BuildSettingInfoWrite', 'gestureControl', 'g2SubmitHijackCommand',
                      'vTaskDelay', 'xQueueSend', 'g2SendEnvelope', 'sendEnvelope',
                      'BROADCAST_PRINTF', '"2.2.9.22"', '"2.3.2.14"'):
        assert forbidden not in hold_section, forbidden
    ingest = extract_block(source, 'static void g2HoldIngest(G2Temple& t, uint32_t etype, uint32_t src,\n'
                                   '                         G2HoldVia via, uint32_t rxGeneration,\n'
                                   '                         uint32_t rxLifecycleEpoch,\n'
                                   '                         uint32_t rxPresentationEpoch) ')
    ordered(ingest, 'g2LifecyclePacketCurrent(t, rxGeneration, rxLifecycleEpoch)',
            'g2HoldAccept(t.side, etype,', '/*profileBypass=*/false);')
    accept = extract_block(source, 'static void g2HoldAccept(')
    ordered(accept, 'const bool onPage = current && g2LensGetState().hijackActive &&',
            'presentationEpoch == g2PresentationEpochCurrent();',
            'g2HoldIngestGate(presentationEpoch, profileBypass)',
            'portENTER_CRITICAL(&gHoldMux);', 'gHold.onPress(', 'gHold.onRelease(',
            'portEXIT_CRITICAL(&gHoldMux);', 'g2HoldLogRecord(rec);',
            'if (onPage && g2FsmHijackActive()) gHijackStartedMs = millis();',
            'g2HoldQueue(jobs[i],')
    gate = extract_block(source, 'static G2HoldReason g2HoldIngestGate(')
    ordered(gate, 'in.trusted = profileBypass || g2HoldProfileCurrent() != nullptr;',
            'in.allowUnprofiled = gSettings.g2HoldUnprofiled;',
            'in.overlay = firmwareOverlayActive();',
            'in.hijackActive = g2LensGetState().hijackActive;',
            'in.presentationCurrent = presentationEpoch == g2PresentationEpochCurrent();',
            'in.pageHasHandler = true;', 'g2HoldRouteDecide(in, nullptr, &reason);')
    producers = {
        'accept': accept, 'ingest': ingest, 'gate': gate, 'from_packet': from_packet,
        'abort': extract_block(source, 'static bool g2HoldAbort(G2HoldReason reason, char side) '),
        'abort_locked': extract_block(source, 'static bool g2HoldAbortLocked('),
        'expire': extract_block(source, 'static void g2HoldExpireTick() '),
        'queue': extract_block(source, 'static void g2HoldQueue('),
    }
    for name, body in producers.items():
        for forbidden in ('systemEventPost', 'g2HoldPostEvent', 'gPageHold', 'handler(',
                          'g2HoldGlobalBegin', 'g2HoldDeliver', 'g2HoldEndOwner', 'g2Notes'):
            assert forbidden not in body, (name, forbidden)
    log_record = extract_block(source, 'void g2HoldLogRecord(const G2HoldRecord& r) ')
    assert 'DEBUG_G2_EVENTSF("[G2-%c] Hold %s via=%s src=%u(%s) verdict=%s' in log_record
    for block in hold_section.split('portENTER_CRITICAL(&gHoldMux);')[1:]:
        locked = block.split('portEXIT_CRITICAL(&gHoldMux);')[0]
        for forbidden in ('DEBUG_', 'g2HoldLogRecord', 'g2HoldQueue', 'tapDispatcherEnqueueHold',
                          'systemEventPost', 'handler(', 'fn(', 'g2HoldGlobal', 'g2Notes',
                          'quickNote'):
            assert forbidden not in locked, ('inside the hold spinlock', forbidden)
    assert 'DEBUG_' not in producers['abort_locked'] and 'g2HoldLogRecord' not in producers['abort_locked']

    # 4b. R10: every widget lifecycle boundary closes an open press. BLE
    # disconnect, closeg2/deinit, topology changes and a fresh Blocks start
    # advance gWidgetLifecycleEpoch without g2TerminalInvalidate, so the open
    # press carries the lifecycle that admitted it. Pin that the epoch has a
    # single writer (so the stamp sees every boundary), that ingest stamps and
    # pre-closes, and that the main-loop tick closes a press from an older
    # lifecycle on every pass, all with the clock read under gHoldMux.
    assert source.count('__atomic_add_fetch(&gWidgetLifecycleEpoch,') == 2
    advance = extract_block(source, 'static uint32_t g2AdvanceWidgetLifecycleEpoch() ')
    assert advance.count('__atomic_add_fetch(&gWidgetLifecycleEpoch,') == 2
    for writer in ('gWidgetLifecycleEpoch++', '++gWidgetLifecycleEpoch',
                   '__atomic_store_n(&gWidgetLifecycleEpoch',
                   '__atomic_fetch_add(&gWidgetLifecycleEpoch'):
        assert writer not in source, writer
    assert 'gWidgetLifecycleEpoch = ' not in source.replace(
        'static volatile uint32_t gWidgetLifecycleEpoch = 1;', '')
    assert 'static uint32_t gHoldOpenLifecycle = 0;' in source
    ordered(accept, 'portENTER_CRITICAL(&gHoldMux);', 'const uint32_t now = millis();',
            'if (gHold.open && gHoldOpenLifecycle != lifecycleEpoch) {',
            'g2HoldAbortLocked(G2HoldReason::Lifecycle, side, now,',
            'gHold.onPress(', 'gHold.onRelease(',
            'verdict == G2HoldVerdict::PressAfterLostRelease',
            '__atomic_store_n(&gHoldOpenLifecycle, lifecycleEpoch, __ATOMIC_RELEASE);',
            '} else if (!gHold.open) {',
            '__atomic_store_n(&gHoldOpenLifecycle, 0u, __ATOMIC_RELEASE);',
            'portEXIT_CRITICAL(&gHoldMux);', 'if (goneAborted) g2HoldLogRecord(gone);',
            'g2HoldLogRecord(rec);')
    assert accept.count('millis()') == 2   # the stamp under the lock + the watchdog
    ordered(producers['abort_locked'], 'gHold.abort(now, &openMs) != G2HoldVerdict::Aborted',
            '__atomic_store_n(&gHoldOpenLifecycle, 0u, __ATOMIC_RELEASE);')
    ordered(producers['abort'], 'portENTER_CRITICAL(&gHoldMux);',
            'g2HoldAbortLocked(reason, side, millis(), &rec, &job);',
            'portEXIT_CRITICAL(&gHoldMux);', 'g2HoldLogRecord(rec);', 'g2HoldQueue(job, 0);')
    expire = producers['expire']
    # R11: a press still open under a firmware overlay (a SyncInfo-only rise
    # or a lost FG_ENTER) closes on the next tick. The open press is sampled
    # before the overlay, and only that same press, still open, is the victim.
    ordered(expire, '__atomic_load_n(&gHoldOpenLifecycle, __ATOMIC_ACQUIRE)',
            'const bool overlayHint = openLifecycle != 0 && gFirmwareOverlayUp;',
            'if (!expiryDue && !lifecycleGone && !overlayHint) return;',
            'if (overlayHint) {', 'portENTER_CRITICAL(&gHoldMux);',
            'const uint32_t openId = gHold.open ? gHold.pressId : 0;',
            'portEXIT_CRITICAL(&gHoldMux);',
            'if (openId != 0 && firmwareOverlayActive()) coveredId = openId;',
            'if (!expiryDue && !lifecycleGone && coveredId == 0) return;',
            'const uint32_t maxOpenMs = g2HoldMaxOpenMs();',
            'portENTER_CRITICAL(&gHoldMux);', 'const uint32_t now = millis();',
            '__atomic_load_n(&gWidgetLifecycleEpoch, __ATOMIC_ACQUIRE);',
            'if (gHold.open && gHoldOpenLifecycle != lifecycle) {',
            'g2HoldAbortLocked(G2HoldReason::Lifecycle,',
            '} else if (coveredId != 0 && gHold.open && gHold.pressId == coveredId) {',
            'g2HoldAbortLocked(G2HoldReason::Overlay,',
            '} else if (gHold.expired(now, maxOpenMs)) {',
            'g2HoldAbortLocked(G2HoldReason::Expired,',
            'portEXIT_CRITICAL(&gHoldMux);', 'g2HoldLogRecord(rec);', 'g2HoldQueue(job, 0);')
    assert expire.count('portENTER_CRITICAL(&gHoldMux);') == 2
    snapshot = expire[expire.index('portENTER_CRITICAL(&gHoldMux);'):
                      expire.index('portEXIT_CRITICAL(&gHoldMux);')]
    assert 'firmwareOverlayActive' not in snapshot, \
        'the overlay is sampled outside gHoldMux, after the open press'
    main_lock = expire.rindex('portENTER_CRITICAL(&gHoldMux);')
    locked_expire = expire[main_lock:expire.index('portEXIT_CRITICAL(&gHoldMux);', main_lock)]
    assert 'tickMs' not in locked_expire, \
        'expiry must age the press with a clock read taken under gHoldMux'
    assert 'firmwareOverlayActive' not in locked_expire

    # 5. Dispatch (B1.3): one tap-dispatch kind, delivered on g2_tap_disp ahead
    # of the generic presentation filter (a release or abort must reach the
    # consumer of its press whatever the lens shows); the layout guards stand.
    assert 'TAP_DISPATCH_HOLD = 7,' in source
    entry = extract_block(source, 'struct TapDispatchEntry ')
    assert 'G2HoldJob hold;' in entry
    for guard in ('static_assert(sizeof(TapDispatchEntry) == 56,',
                  'static_assert(sizeof(TapDispatchEntry::Payload) == 36,',
                  'static_assert(kTapQueueDepth * sizeof(TapDispatchEntry) == 448,'):
        assert source.count(guard) == 1, guard
    worker = extract_block(source, 'static void tapDispatcherWorkerLoop(void* arg) ')
    ordered(worker, 'if (e.kind == TAP_DISPATCH_HOLD) {',
            'g2HoldDeliver(e.payload.hold, e.presentationEpoch);', 'continue;',
            'if (e.kind == TAP_DISPATCH_BARRIER) {',
            'const uint32_t liveEpoch = g2PresentationEpochCurrent();',
            'case TAP_DISPATCH_HOLD:')
    assert source.count('g2HoldDeliver(e.payload.hold, e.presentationEpoch);') == 1
    assert source.count('g2HoldDeliver(') == 2   # definition + the worker
    producer = extract_block(source, 'static bool tapDispatcherEnqueueHold(const G2HoldJob& job,\n'
                                     '                                     uint32_t presentationEpoch) ')
    ordered(producer, 'if (job.phase == (uint8_t)G2HoldPhase::Press) (void)tapDispatcherInit();',
            'tapRuntimeSnapshot(&queue)', 'e.kind              = TAP_DISPATCH_HOLD;',
            'e.payload.hold      = job;', 'xQueueSend(queue, &e, 0)')
    # Events are posted only by the dispatcher, through the rate limiter, and
    # only as a source name plus a consumer/reason/duration detail (R22).
    assert hold_section.count('systemEventPost(') == 1
    post = extract_block(source, 'static void g2HoldPostEvent(')
    assert 'systemEventPost(kind, g2HoldSourceName(source), detail);' in post
    assert 'sRate.admit(millis())' in post
    deliver = extract_block(source, 'static void g2HoldDeliver(')
    assert deliver.count('g2HoldPostEvent(SYSEVT_G2_LONG_PRESS, source, detail);') == 1
    assert deliver.count('g2HoldPostEvent(SYSEVT_G2_LONG_RELEASE, job.source, detail);') == 1
    assert hold_section.count('g2HoldPostEvent(') == 3   # definition + 2 in deliver
    ordered(deliver, 'sDelivery.end(job.pressId, &seen)', 'g2HoldEndOwner(',
            'if (!seen) return;', 'sDelivery.beginPress(job.pressId, &superseded)',
            'gPageHold.get((uint32_t)page)', 'g2HoldProfileCurrent()',
            'in.trusted = job.via == (uint8_t)G2HoldVia::Sim || profile != nullptr;',
            'in.keyboard = kbdPadSysDoubleOwned();', 'in.camStream = gCamStreamActive;',
            'in.imageProbe = gImgProbeHoldActive;', 'in.swapInFlight = pageSwapInFlight();',
            'in.sourceAllowed = g2HoldSourceAllowed(', 'in.action = gSettings.g2HoldAction;',
            'g2HoldRouteDecide(in, nullptr, &reason);',
            'handler((uint8_t)G2HoldPhase::Press, source)',
            'g2HoldGlobalBegin(job.pressId, source, &why)')
    # B2.6: the global action is the Notes page's hold consumer; a build
    # without QuickNote gets its refusing stub ("quicknote-not-built" = Off).
    # A page that declines a source the policy excludes is a "source" drop,
    # whatever the global action is set to.
    # It never leaves a page a session job or an image probe owns (that
    # worker's post-session SHUTDOWN and onDone would hit the Notes page);
    # live list/text pages are stopped by the page swap itself.
    begin = extract_block(source, 'static bool g2HoldGlobalBegin(')
    ordered(begin, 'if (g2HoldPageBusy()) {', '*why = "page-busy";', 'return false;',
            'return g2NotesHoldBegin(pressId, why);')
    busy = extract_block(source, 'static bool g2HoldPageBusy() ')
    assert 'if (hijackFsmState() == HijackState::ImageProbing) return true;' in busy
    assert 'return g2SessionWorkerBusy() && !livePageIsActive() && !liveTextIsActive();' in busy
    assert 'g2NotesHoldEnd(pressId, heldMs);' in extract_block(source, 'static void g2HoldGlobalEnd(')
    assert 'g2NotesHoldAbort(pressId, reason);' in extract_block(source, 'static void g2HoldGlobalAbort(')
    assert '*why = "quicknote-not-built";' in (c / 'G2_Page_Notes.h').read_text()
    ordered(deliver, 'handler((uint8_t)G2HoldPhase::Press, source)',
            '} else if (!in.sourceAllowed) {', 'reason = G2HoldReason::Source;',
            'in.pageHasHandler = false;', 'g2HoldGlobalBegin(job.pressId, source, &why)')

    # 6. Settings (B1.3 + live source policy) and the catalog rows (R23).
    header = (c / 'G2_Glasses.h').read_text()
    assert 'typedef bool (*G2PageHoldFn)(uint8_t phase, uint8_t source);' in header
    assert 'bool g2RegisterPageHold(G2HijackPage page, G2PageHoldFn fn);' in header
    assert 'bool g2HoldSourceAdmitted(uint8_t source);' in header
    settings = (c / 'System_Settings.cpp').read_text()
    for row in ('{ "g2holdaction", SETTING_U8, &gSettings.g2HoldAction, HW1_G2_HOLD_ACTION_DEFAULT, 0, nullptr, 0, 1, "Hold action on HW1 lens pages", "0|Off,1|QuickNote",',
                '{ "g2holdsource", SETTING_U8, &gSettings.g2HoldSource, 0, 0, nullptr, 0, 1, "Hold action source", "0|Ring only,1|Ring + temples",',
                '{ "g2holdunprofiled", SETTING_BOOL, &gSettings.g2HoldUnprofiled, 0, 0, nullptr, 0, 1, "Allow hold actions on unprofiled glasses firmware (testing)",'):
        assert settings.count(row) == 1, row
    assert settings.count('registerSettingsModule(&g2HoldSettingsModule);') == 1
    for key in ('g2holdaction', 'g2holdsource', 'g2holdunprofiled'):
        assert f'SETTING_EDITOR_CMD(cmd_set_{key},' in settings, key
        assert f'{{ "{key}",' in settings[settings.index('const CommandEntry settingEditorCommands[]'):], key
    build = (c / 'System_BuildConfig.h').read_text()
    # The default follows the QuickNote build flag, resolved just above it.
    ordered(build, '#define ENABLE_DICTATION', '#ifndef ENABLE_QUICKNOTE',
            '#define ENABLE_QUICKNOTE (ENABLE_DICTATION && ENABLE_G2_GLASSES && ENABLE_BLUETOOTH)',
            '#ifndef HW1_G2_HOLD_ACTION_DEFAULT',
            '#define HW1_G2_HOLD_ACTION_DEFAULT (ENABLE_QUICKNOTE ? 1 : 0)')
    rows = (c / 'System_EventCatalogRows.h').read_text()
    ordered(rows, 'SYSEVT_COMPANION_REBOOT', 'HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_LONG_PRESS,       "g2_long_press", SYSEVT_FAM_GLASSES)',
            'HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_LONG_RELEASE,     "g2_long_release", SYSEVT_FAM_GLASSES)',
            '#endif')
    notifications = (c / 'System_Notifications.cpp').read_text()
    assert 'SYSEVT_G2_LONG_PRESS' not in notifications and \
        'SYSEVT_G2_LONG_RELEASE' not in notifications, \
        'hold events stay event-only: no default banner/toast rule (R23)'

    # 7. CLI: registered next to g2status, non-admin, status/clear/sim; sim is
    # admin-checked in the handler; profiles come from the header table.
    table = extract_block(source, 'extern const CommandEntry g2Commands[] = ')
    ordered(table, '{ "g2status",', '{ "g2hold",', '{ "g2info",')
    row = table[table.index('{ "g2hold",'):]
    row = row[:row.index('},') + 1]
    assert ', false, cmd_g2hold,' in row, 'g2hold must be non-admin (sim checks itself)'
    assert 'Usage: g2hold [status|clear|sim <press [ring|right|left|unknown]|release|abort>]' in row
    cmd = extract_block(source, 'static const char* cmd_g2hold(const String& argsInput) ')
    # R21: a synthetic press can start a recording, so it binds like `note
    # start glasses` - no automation/ESP-NOW/MQTT/voice transport, the paired
    # wearer only, no admin override - before g2HoldSim() runs.
    sim = cmd[cmd.index('sub == "sim"'):cmd.index('return g2HoldSim(what, src);')]
    ordered(sim, 'if (!currentExecIsAdmin()) {', 'if (what == "press") {',
            'currentAuthContext().transport',
            'via == SOURCE_INTERNAL', 'via == SOURCE_ESPNOW', 'via == SOURCE_MQTT',
            'via == SOURCE_VOICE', 'return "Error: g2hold sim press: not from',
            'blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner)', 'owner.live()',
            'owner.user == currentExecUser()', 'secureClearString(owner.user);',
            'if (!wearer) {', 'only the paired wearer may simulate a hold')
    assert sim.count('currentExecIsAdmin()') == 1 and 'isAdminUser' not in sim, \
        'the wearer binding has no admin override'
    ordered(cmd, 'RETURN_VALID_IF_VALIDATE_CSTR();', 'sub == "clear"', 'sub == "sim"',
            'if (!currentExecIsAdmin()) {', 'only the paired wearer may simulate a hold',
            'return g2HoldSim(what, src);',
            'g2HoldFirmwareSnapshot(left, right);',
            'g2HoldProfileFor(left, right);', 'portENTER_CRITICAL(&gHoldMux);',
            'portEXIT_CRITICAL(&gHoldMux);', 'g2HoldAppendf(')
    assert 'sourceReliable=%s' in cmd and 'trusted=yes' in cmd and 'trusted=no (unprofiled)' in cmd
    assert '"2.2.9.22"' not in cmd and 'g2BothFirmwareExact' not in cmd

    print('G2 hold decode: event names, hook order, producer/dispatcher split passed')


if __name__ == '__main__':
    main()
