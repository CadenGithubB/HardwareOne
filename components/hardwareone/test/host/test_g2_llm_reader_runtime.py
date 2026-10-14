#!/usr/bin/env python3
"""Execute the production LLM reader worker and intent callbacks without BLE."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>
#include "G2_HoldGesture.h"
#include "G2_RenderTypes.h"
#include "G2_LlmReader.h"
#include "System_LLMReplyLimits.h"
#define LLM_CHAT_TURN_MAX_BYTES kLlmReplyCapacityBytes
#define DEBUG_G2F(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
enum class AllocPref { PreferPSRAM };
static bool allocationFails = false;
static void* ps_realloc(void* p, size_t n, AllocPref, const char*) {
  return allocationFails ? nullptr : realloc(p, n);
}
static void* ps_alloc(size_t n, AllocPref, const char*) {
  return allocationFails ? nullptr : malloc(n);
}
static void ps_free(void* p) { free(p); }
// INSERT_BUFFER
enum G2HijackPage { G2_HIJACK_PAGE_MAIN = 0, G2_HIJACK_PAGE_LLM_MENU = 17,
                   G2_HIJACK_PAGE_LLM_READER = 25 };
enum G2TapKind { G2_TAP_SELECT, G2_TAP_PAGE_PREV, G2_TAP_PAGE_NEXT };
using G2TapFn = void (*)(G2TapKind);
struct G2HijackCtxGuard { G2HijackCtxGuard() {} ~G2HijackCtxGuard() {} };
static bool completionAllowed = true, completionHeld = false;
struct G2CompletionGuard {
  bool held = false;
  G2CompletionGuard() {
    assert(!completionHeld);
    held = completionHeld = completionAllowed;
  }
  ~G2CompletionGuard() { if (held) completionHeld = false; }
  explicit operator bool() const { return held; }
};
struct G2Temple { uint32_t connectionGeneration = 4; };
static G2Temple temple;
static uint32_t gWidgetLifecycleEpoch = 2, presentationEpoch = 5;
static uint32_t gHijackStartedMs = 0, nowMs = 100;
static bool gImgProbeAbort = false, lifecycleOpen = true, linked = true;
static bool overlay = false, generating = false, sourceAllowed = true;
static bool createOK = true, publishOK = true, submitOK = true, promptOK = true;
static bool textActive = false;
static bool rebuildAck = true;
static int rebuildAckCalls = 0;
static int updateFailures = 0, updateCalls = 0, teardowns = 0, promptCalls = 0, menuCalls = 0;
static uint32_t textLife = 0;
static G2HijackPage page = G2_HIJACK_PAGE_LLM_MENU;
using ChatRunToken = uint64_t;
static ChatRunToken runToken = 1;
static std::string answer;
static std::vector<std::string> frames, actions;
static std::function<void()> tick, afterTeardown, afterCreate, beforeRead;
static std::function<void()> afterEncode, afterTransmit, duringAck;
static void (*pendingWorker)(void*) = nullptr;
static void (*pendingDrop)(void*) = nullptr;
static void* pendingArg = nullptr;
static void (*textExit)() = nullptr;
static G2TapFn textNav = nullptr;
static bool g2LlmReaderHold(uint8_t, uint8_t);
static void g2LlmReaderExit();
static void g2LlmReaderNav(G2TapKind);
// INSERT_GLOBALS

static uint32_t millis() { return nowMs; }
static void vTaskDelay(uint32_t ms) {
  nowMs += ms;
  assert(nowMs < 10000); // A fixture must end the production worker.
  if (tick) tick();
}
static G2HijackPage g2GetHijackPage() { return page; }
static void g2SetHijackPage(G2HijackPage p) { assert(completionHeld); page = p; }
static bool g2HoldSourceAdmitted(uint8_t) { return sourceAllowed; }
static bool chatIsGenerating() { return generating; }
static ChatRunToken chatGetRunToken() { return runToken; }
static int chatReadRun(ChatRunToken run, int off, char* out, int cap, bool* done) {
  if (beforeRead) beforeRead();
  if (run != runToken) return -1;
  assert(off == 0 && cap == int(kLlmReplyCapacityBytes));
  const size_t n = std::min(answer.size(), size_t(cap - 1));
  memcpy(out, answer.data(), n); out[n] = 0; *done = !generating;
  return int(n);
}
static bool g2WidgetLifecycleOpen(uint32_t life) {
  return lifecycleOpen && life == gWidgetLifecycleEpoch;
}
static bool g2FsmHijackActive() { return lifecycleOpen; }
static uint32_t g2PresentationEpochCurrent() { return presentationEpoch; }
static bool g2LensTxFenceCurrent(const G2Temple& arm, uint32_t gen, uint32_t life) {
  return linked && gen == arm.connectionGeneration && g2WidgetLifecycleOpen(life);
}
static bool firmwareOverlayActive() { return overlay; }
static G2Temple* pickEvenAIArm(const char*) { return linked ? &temple : nullptr; }
static constexpr uint32_t BLOCKS_WIDGET_ID = 3;
static bool tearDownActiveContainerAtGeneration(G2Temple& arm, uint32_t gen,
                                               bool, uint32_t life) {
  if (!g2LensTxFenceCurrent(arm, gen, life)) return false;
  ++teardowns;
  if (++presentationEpoch == 0) ++presentationEpoch;
  actions.push_back("teardown");
  if (afterTeardown) afterTeardown();
  return true;
}
static bool sendCreateTextAndWait(G2Temple&, const char*, uint32_t,
                                 const G2ContainerGeom& geom, bool capture, const char* name) {
  assert(geom.w == 560 && capture && std::string(name) == "llmChat");
  actions.push_back("create");
  if (afterCreate) afterCreate();
  return createOK;
}
static bool g2NoteCreateSuccess(G2Temple&, bool list, uint32_t, uint32_t life,
                               uint32_t* presentation) {
  assert(!list); *presentation = presentationEpoch;
  return g2WidgetLifecycleOpen(life);
}
static bool g2TextViewPublishForLifecycle(void (*exitFn)(), G2TapFn navFn,
                                        uint32_t life, uint32_t present, bool text) {
  assert(text && completionHeld);
  if (!publishOK || !g2WidgetLifecycleOpen(life) || present != presentationEpoch) return false;
  textActive = true; textExit = exitFn; textNav = navFn; textLife = life; return true;
}
static void g2TextViewDisarmIfOwned(void (*exitFn)(), uint32_t life) {
  if (textActive && textExit == exitFn && textLife == life) textActive = false;
}
static constexpr uint32_t G2_MAGIC_REBUILD = 202;
static constexpr uint8_t G2_SID_EVEN_CORE = 0xe0, G2_FLAG_REQUEST = 1;
static uint8_t allocSeq() { return 42; }
static bool armRebuildSlot(const char*) { return g2WidgetLifecycleOpen(gWidgetLifecycleEpoch); }
static bool waitRebuildAck(const char*) {
  ++rebuildAckCalls;
  if (duringAck) duringAck();
  return rebuildAck;
}
static size_t g2BuildRebuildTextPb(uint32_t magic, const char* name,
                                 const char* frame, const G2ContainerGeom& geom,
                                 bool capture, uint8_t* pb, size_t cap) {
  assert(magic == G2_MAGIC_REBUILD && std::string(name) == "llmChat");
  assert(geom.x == 8 && geom.y == 8 && geom.w == 560 && geom.h == 272 && capture);
  const size_t bytes = strlen(frame) + 1;
  assert(bytes <= cap);
  memcpy(pb, frame, bytes);
  if (afterEncode) afterEncode();
  return bytes;
}
static bool sendPbFragmented(G2Temple& arm, uint8_t, uint8_t sid, uint8_t flag,
                             const uint8_t* pb, size_t len, uint32_t gen,
                             bool* finalAttempt, uint32_t createSerial, uint32_t life,
                             uint32_t presentation) {
  assert(gen == temple.connectionGeneration && life == gWidgetLifecycleEpoch);
  assert(g2LensTxFenceCurrent(arm, gen, life));
  assert(!presentation || presentation == presentationEpoch);
  assert(sid == G2_SID_EVEN_CORE && flag == G2_FLAG_REQUEST && !finalAttempt && !createSerial);
  assert(len == strlen(reinterpret_cast<const char*>(pb)) + 1);
  ++updateCalls;
  if (updateFailures) { --updateFailures; return false; }
  frames.emplace_back(reinterpret_cast<const char*>(pb)); actions.push_back("frame");
  if (afterTransmit) afterTransmit();
  return true;
}
// INSERT_REBUILD
enum class LlmPromptReturn { MENU, VIEWER };
static bool llmMenuStartPromptEntry(LlmPromptReturn where, bool startWithMic,
                                    uint32_t expectedLifecycleEpoch) {
  assert(where == LlmPromptReturn::VIEWER && startWithMic);
  assert(expectedLifecycleEpoch == gWidgetLifecycleEpoch);
  assert(teardowns == 2 && !gLlmPageActive.load() && !textActive);
  ++promptCalls; actions.push_back("mic-prompt"); return promptOK;
}
static bool g2SessionSubmit(void (*worker)(void*), void* arg, void (*drop)(void*),
                            const char*, uint32_t life) {
  assert(life == gWidgetLifecycleEpoch);
  if (!submitOK) return false;
  pendingWorker = worker; pendingArg = arg; pendingDrop = drop; return true;
}
static size_t copyText(char* out, const char* in, size_t cap) {
  const size_t n = std::min(strlen(in), cap - 1);
  memcpy(out, in, n); out[n] = 0; return strlen(in);
}
#define strlcpy copyText
// INSERT_PRODUCTION

static void menu() { ++menuCalls; actions.push_back("menu"); }
static void reset() {
  assert(!pendingArg);
  allocationFails = false;
  assert(!completionHeld); completionAllowed = true;
  gWidgetLifecycleEpoch = 2; presentationEpoch = 5; nowMs = 100;
  gImgProbeAbort = overlay = generating = textActive = false;
  lifecycleOpen = linked = sourceAllowed = createOK = publishOK = submitOK = promptOK = true;
  updateFailures = updateCalls = teardowns = promptCalls = menuCalls = 0;
  rebuildAck = true; rebuildAckCalls = 0;
  page = G2_HIJACK_PAGE_LLM_MENU; runToken = 1;
  answer = "A conversational reply."; frames.clear(); actions.clear();
  tick = afterTeardown = afterCreate = beforeRead = nullptr;
  afterEncode = afterTransmit = duringAck = nullptr;
  textExit = nullptr; textNav = nullptr;
  gLlmPageActive = gLlmPageExit = gLlmPageAsk = gLlmPageBusyNotice = false;
  gLlmPageNav = 0;
}
static void run() {
  assert(g2ShowLlmPage(menu));
  auto worker = pendingWorker; auto* arg = pendingArg;
  pendingArg = nullptr; worker(arg);
}
static void runQueued() {
  assert(pendingArg);
  auto worker = pendingWorker; auto* arg = pendingArg;
  pendingArg = nullptr; worker(arg);
}
static void testIntents() {
  reset();
  assert(!g2LlmReaderCurrent());
  assert(!g2LlmReaderHold(9, 2));
  gLlmPageActive = true; page = G2_HIJACK_PAGE_LLM_READER;
  sourceAllowed = false; assert(!g2LlmReaderHold(9, 2) && !gLlmPageAsk.load());
  sourceAllowed = true; generating = true;
  assert(g2LlmReaderHold(9, 2)); // Consumed: cannot fall through to QuickNote.
  assert(gLlmPageBusyNotice.load() && !gLlmPageAsk.load());
  generating = false; assert(g2LlmReaderHold(9, 2) && gLlmPageAsk.load());
  gLlmPageAsk = false;
  assert(g2LlmReaderHold(10, 2) && !gLlmPageAsk.load());
  assert(g2LlmReaderHold(0, 2) && !gLlmPageAsk.load());
  for (int n = 0; n < 30; ++n) g2LlmReaderNav(G2_TAP_PAGE_NEXT);
  assert(gLlmPageNav.load() == 8);
  for (int n = 0; n < 30; ++n) g2LlmReaderNav(G2_TAP_PAGE_PREV);
  assert(gLlmPageNav.load() == -8);
  page = G2_HIJACK_PAGE_MAIN;
  g2LlmReaderExit(); g2LlmReaderNav(G2_TAP_PAGE_NEXT);
  assert(!gLlmPageExit.load() && gLlmPageNav.load() == -8);
}
int main() {
  testIntents();
  reset(); tick = [] { assert(textExit); textExit(); }; run();
  assert(updateCalls == 1 && menuCalls == 1 && !promptCalls && teardowns == 2);
  assert(!textActive && !gLlmPageActive.load());

  // Production named REBUILD carries the same child geometry/capture, and
  // preserves lifecycle admission before encoding, transmission and ACK.
  reset(); ++gWidgetLifecycleEpoch;
  assert(!sendRebuildTextNamedAndWait(temple, "llmChat", "test", G2_GEOM_LARGE, true, 4, 2));
  assert(!updateCalls && !rebuildAckCalls);
  for (int edge = 0; edge < 3; ++edge) {
    reset();
    const auto invalidate = [] { ++gWidgetLifecycleEpoch; };
    if (edge == 0) afterEncode = invalidate;
    if (edge == 1) afterTransmit = invalidate;
    if (edge == 2) duringAck = invalidate;
    assert(!sendRebuildTextNamedAndWait(temple, "llmChat", "test", G2_GEOM_LARGE, true, 4, 2));
    assert(updateCalls == (edge ? 1 : 0));
    assert(rebuildAckCalls == (edge == 2 ? 1 : 0));
  }
  for (int edge = 0; edge < 3; ++edge) {
    reset();
    const auto replace = [] { ++presentationEpoch; };
    if (edge == 0) afterEncode = replace;
    if (edge == 1) afterTransmit = replace;
    if (edge == 2) duringAck = replace;
    assert(!sendRebuildTextNamedAndWait(temple, "llmChat", "test", G2_GEOM_LARGE, true, 4, 2, 5));
    assert(updateCalls == (edge ? 1 : 0));
    assert(rebuildAckCalls == (edge == 2 ? 1 : 0));
  }

  reset(); answer.clear();
  for (int line = 0; line < 7; ++line) answer += std::string(48, 'A' + line) + '\n';
  tick = [] { textExit(); }; run();
  assert(frames.size() == 1 && frames[0].size() > 300);
  assert(frames[0].find(std::string(48, 'F')) != std::string::npos);
  assert(frames[0].find(std::string(48, 'G')) == std::string::npos);

  reset(); tick = [] { assert(g2LlmReaderHold(9, 2)); }; run();
  assert(promptCalls == 1 && !menuCalls && runToken == 1);
  assert(actions == (std::vector<std::string>{"teardown", "create", "frame", "teardown", "mic-prompt"}));

  reset(); generating = true; int ticks = 0;
  tick = [&] {
    if (++ticks == 1) assert(g2LlmReaderHold(9, 2));
    else textExit();
  }; run();
  assert(!promptCalls && menuCalls == 1);
  assert(frames.back().find("Reply in progress") != std::string::npos);

  reset(); answer.clear(); for (int n = 0; n < 24; ++n) answer += "A different line in a long answer.\n";
  ticks = 0; tick = [&] {
    if (++ticks == 1) textNav(G2_TAP_PAGE_NEXT);
    else if (ticks == 2) textNav(G2_TAP_PAGE_PREV);
    else textExit();
  }; run();
  assert(frames.size() == 3 && frames[0].find("[1/") != std::string::npos);
  assert(frames[1].find("[2/") != std::string::npos && frames[2] == frames[0]);

  reset(); ticks = 0; generating = true; answer = "First words.";
  tick = [&] {
    if (++ticks == 1) { answer += " Added words."; generating = false; }
    else textExit();
  }; run();
  assert(frames.size() == 2 && frames[1].find("Added words.") != std::string::npos);

  // A new exact run replaces the old reader snapshot even at identical size.
  reset(); ticks = 0; answer = std::string(300, 'A');
  tick = [&] {
    if (++ticks == 1) textNav(G2_TAP_PAGE_NEXT);
    else if (ticks == 2) { ++runToken; answer = std::string(300, 'B'); }
    else textExit();
  }; run();
  assert(frames.size() == 3 && frames[1].find("[2/") != std::string::npos);
  assert(frames[2].find("[1/") != std::string::npos);
  assert(frames[2].find("BBB") != std::string::npos && frames[2].find("AAA") == std::string::npos);

  reset(); bool replaced = false;
  beforeRead = [&] { if (!replaced) { replaced = true; ++runToken; answer = "New run."; } };
  tick = [] { if (!frames.empty()) textExit(); }; run();
  assert(frames.size() == 1 && frames[0].find("New run.") != std::string::npos);

  reset(); runToken = 0; tick = [] { textExit(); }; run();
  assert(frames.size() == 1 && frames[0].find("No reply yet.") != std::string::npos);

  reset(); overlay = true; ticks = 0;
  tick = [&] { if (++ticks == 1) { assert(frames.empty()); overlay = false; } else textExit(); }; run();
  assert(frames.size() == 1);

  reset(); tick = [] { ++gWidgetLifecycleEpoch; page = G2_HIJACK_PAGE_MAIN; }; run();
  assert(teardowns == 1 && !promptCalls && !menuCalls);
  reset(); tick = [] { ++presentationEpoch; page = G2_HIJACK_PAGE_MAIN; }; run();
  assert(teardowns == 1 && !promptCalls && !menuCalls);

  // Terminal/replacement edges during final teardown must not open a new
  // prompt or menu in the replacement lifecycle/page.
  for (int edge = 0; edge < 4; ++edge) {
    reset(); tick = [] { assert(g2LlmReaderHold(9, 2)); };
    afterTeardown = [edge] {
      if (teardowns != 2) return;
      if (edge == 0) ++gWidgetLifecycleEpoch;
      else if (edge == 1) linked = false;
      else if (edge == 2) page = G2_HIJACK_PAGE_MAIN;
      else ++presentationEpoch; // Same page enum, different accepted surface.
    }; run();
    assert(teardowns == 2 && !promptCalls && !menuCalls);
  }
  reset(); presentationEpoch = UINT32_MAX - 1;
  tick = [] { assert(g2LlmReaderHold(9, 2)); }; run();
  assert(presentationEpoch == 1 && promptCalls == 1 && !menuCalls);

  reset(); afterCreate = [] { ++gWidgetLifecycleEpoch; page = G2_HIJACK_PAGE_MAIN; }; run();
  assert(teardowns == 1 && frames.empty() && !promptCalls && !menuCalls && !textActive);
  for (int failure = 0; failure < 2; ++failure) {
    reset();
    if (failure == 0) completionAllowed = false;
    else publishOK = false;
    run();
    assert(teardowns == 1 && frames.empty() && !promptCalls && !menuCalls);
    assert(!textActive && !gLlmPageActive.load() && !completionHeld);
    assert(page == G2_HIJACK_PAGE_LLM_MENU);
  }
  reset(); allocationFails = true; run();
  assert(!teardowns && !promptCalls && !menuCalls);
  reset(); assert(g2ShowLlmPage(menu)); ++gWidgetLifecycleEpoch; runQueued();
  assert(!teardowns && !promptCalls && !menuCalls);

  reset(); updateFailures = 2; tick = [] { if (!frames.empty()) textExit(); }; run();
  assert(updateCalls == 3 && frames.size() == 1 && menuCalls == 1 && !promptCalls);
  reset(); updateFailures = 3; run();
  assert(updateCalls == 3 && frames.empty() && menuCalls == 1);
  reset(); rebuildAck = false; run();
  assert(updateCalls == 3 && rebuildAckCalls == 3 && menuCalls == 1);

  reset(); promptOK = false; tick = [] { assert(g2LlmReaderHold(9, 2)); }; run();
  assert(promptCalls == 1 && menuCalls == 1);
  reset(); submitOK = false; assert(!g2ShowLlmPage(menu) && !pendingArg);
  reset(); assert(g2ShowLlmPage(menu)); pendingDrop(pendingArg); pendingArg = nullptr;
  assert(!promptCalls && !menuCalls && !teardowns);
  puts("G2 LLM reader: production callbacks, full-width worker, scroll, busy hold, mic handoff, retry and lifecycle fences passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (COMPONENT / 'G2_Glasses.cpp').read_text()
    start = source.index('struct LlmPageArgs {')
    production = source[start:source.index('#endif // ENABLE_LLM_BACKEND', start)]
    globals_text = '\n'.join(re.findall(r'^static std::atomic<[^>]+>\s+gLlmPage\w+[^;]*;', source, re.M))
    buffer = (COMPONENT / 'System_PsramBuffer.h').read_text()
    buffer = buffer.replace('#pragma once', '').replace('#include "System_MemUtil.h"', '')
    unit = (HARNESS.replace('// INSERT_BUFFER', buffer)
            .replace('// INSERT_GLOBALS', globals_text)
            .replace('// INSERT_REBUILD', extract_block(source, 'static bool sendRebuildTextNamedAndWait('))
            .replace('// INSERT_PRODUCTION', production))
    with tempfile.TemporaryDirectory(prefix='hw1-g2-llm-reader-') as directory:
        cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(unit)
        command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', f'-I{COMPONENT}', str(cpp), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
