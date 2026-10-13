#!/usr/bin/env python3
"""Exercise production guided-menu APIs with a retained onboard menu after source switches."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def structure(source, name):
    return re.search(r'struct ' + name + r' \{.*?\n\};', source, re.S).group(0)


HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#define ENABLE_LLM_SOURCE_ONBOARD 1
#define EXT_RAM_BSS_ATTR
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
using SemaphoreHandle_t = std::mutex*;
static std::mutex menuMutex;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return &menuMutex; }
static int xSemaphoreTake(SemaphoreHandle_t p, unsigned) { p->lock(); return 1; }
static void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
static void llmPsramFree(void** p) { free(*p); *p = nullptr; }
static size_t testStrlcpy(char* out, const char* in, size_t cap) {
  const size_t len = strlen(in);
  if (cap) { const size_t n = len < cap - 1 ? len : cap - 1; memcpy(out, in, n); out[n] = 0; }
  return len;
}
#define strlcpy testStrlcpy
enum class LlmBackendKind { None, Onboard, Cm5, Http };
static LlmBackendKind active = LlmBackendKind::Onboard;
static LlmBackendKind llmBackendActiveKind() { return active; }
static bool llmBackendSupportsRetry() { return active != LlmBackendKind::None && active != LlmBackendKind::Http; }
struct ChatParamOverride { int hardCap = 0, sentenceLimit = 0; };
static int requests = 0;
static std::string lastPrompt;
static int chatBeginTurn(const char* prompt, const ChatParamOverride*) {
  lastPrompt = prompt; return ++requests;
}
// INSERT_TYPES
struct MenuRuntime {
  uint8_t* menuBlob = nullptr;
  size_t menuBytes = 0;
  uint8_t menuGroupCount = 0;
  uint16_t menuGeneration = 0;
  LLMMenuGroupDesc menuGroups[8]{};
} gLLM;
// INSERT_IMPLEMENTATION
int main() {
  const uint8_t data[] = {'H', 'e', 'l', 'p', 3, 'A', 's', 'k', 4, 't', 'e', 's', 't'};
  auto* blob = static_cast<uint8_t*>(malloc(sizeof(data)));
  memcpy(blob, data, sizeof(data));
  LLMMenuGroupDesc group{};
  group.nameOff = 0; group.nameLen = 4; group.tplOff = 4; group.entOff = 8;
  group.tplCount = 1; group.entCount = 1;
  llmMenuPublish(blob, sizeof(data), &group, 1);
  const uint16_t generation = llmMenuGeneration();
  assert(generation != 0 && llmMenuGroupCount() == 1);
  assert(llmMenuAsk(generation, 0, 0, -1, nullptr) == 1);
  assert(lastPrompt == "Ask" && llmMenuRepeatLast() == 2);
  int session = 0;
  assert(llmMenuLastAskInfo(&session) && session == 2);

  // Switching retains the local model/menu in RAM; no stale API may submit
  // that previous guided question or expose its controls to the agent route.
  active = LlmBackendKind::Http;
  char text[32] = "stale";
  LLMMenuGroupInfo info{};
  assert(llmMenuGeneration() == 0 && llmMenuGroupCount() == 0);
  assert(!llmMenuGroupInfo(0, &info));
  assert(llmMenuTemplate(0, 0, text, sizeof(text), nullptr) < 0 && text[0] == 0);
  assert(llmMenuEntity(0, 0, text, sizeof(text)) < 0 && text[0] == 0);
  assert(llmMenuCompose(0, 0, -1, text, sizeof(text)) == -3 && text[0] == 0);
  assert(llmMenuAsk(generation, 0, 0, -1, nullptr) == -3);
  assert(llmMenuRepeatLast() == -3 && !llmMenuLastAskInfo(&session));
  assert(requests == 2);

  // This change leaves the pre-existing CM5 guided behavior intact.
  active = LlmBackendKind::Cm5;
  assert(llmMenuGroupCount() == 1 && llmMenuRepeatLast() == 3);
  active = LlmBackendKind::Onboard;
  assert(llmMenuGeneration() == generation && llmMenuRepeatLast() == 4);
  llmMenuClear();
  assert(llmMenuGroupCount() == 0);
  puts("Guided source isolation tests passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    component = Path(__file__).resolve().parents[2]
    source = (component / 'System_LLM_Menu.cpp').read_text()
    # Compile all production lifecycle, composition and accessors; the command
    # formatting section needs Arduino String but is downstream of these APIs.
    source = source.split('// Commands (registered NON-admin', 1)[0] + '\n#endif\n'
    source = re.sub(r'^#include.*\n', '', source, flags=re.M)
    types = structure((component / 'System_LLM_Internal.h').read_text(), 'LLMMenuGroupDesc')
    types += '\n' + structure((component / 'System_LLMTypes.h').read_text(), 'LLMMenuGroupInfo')
    unit = HARNESS.replace('// INSERT_TYPES', types).replace('// INSERT_IMPLEMENTATION', source)
    with tempfile.TemporaryDirectory(prefix='hw1-guided-source-') as tmp:
        cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        cpp.write_text(unit)
        command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', str(cpp), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
