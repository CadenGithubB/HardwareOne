// Runs System_LLMHomeMenuCore.h, the LLM app's home rows shared by the glasses
// (width 51, with the Terminal row) and the local display (width 19: 21 glyphs
// less the "> " cursor).
//
// Cases:
//   minimal       nothing loaded: Open chat, Ask, New chat, Select Model.
//   full_glasses  every optional row, in the glasses' order, long labels intact.
//   oled_width    every label fits 19 glyphs, long model names shortened in the
//                 middle, the low-context row in its short form at the largest
//                 context that still warns (23 < LLM_MIN_USABLE_CONTEXT).
//   fit_name      extensions dropped; short names untouched; tiny widths cut.
//   cap           a short output array is filled, never overrun.
#include <cassert>
#include <cstdio>
#include <cstring>

#include "System_LLMHomeMenuCore.h"

using A = LlmHomeAction;

static void expectRows(const llmhome::Row* rows, size_t n, const A* actions,
                       const char* const* labels, size_t want) {
  for (size_t i = 0; i < n; i++) std::printf("    %zu %s\n", i, rows[i].label);
  assert(n == want);
  for (size_t i = 0; i < n; i++) {
    assert(rows[i].action == actions[i]);
    assert(std::strcmp(rows[i].label, labels[i]) == 0);
  }
}

int main() {
  llmhome::Row rows[12];

  {  // minimal
    llmhome::State s;
    const size_t n = llmhome::rows(s, 51, rows, 12);
    const A actions[] = {A::Open, A::Ask, A::NewChat, A::Model};
    const char* labels[] = {"Open chat", "Ask (Mic / Keys)", "New chat", "Select Model"};
    expectRows(rows, n, actions, labels, 4);
  }

  {  // full_glasses
    llmhome::State s;
    s.terminal = s.guided = s.retry = true;
    std::snprintf(s.model, sizeof(s.model), "openclaw");
    s.lowContext = true;
    s.context = 20;
    const size_t n = llmhome::rows(s, 51, rows, 12);
    const A actions[] = {A::Open, A::Terminal, A::Ask, A::Guided, A::NewChat, A::Rerun,
                         A::Model, A::Model};
    const char* labels[] = {"Open chat", "Open Terminal", "Ask (Mic / Keys)", "Ask (guided)",
                            "New chat", "Re-run last", "Model: openclaw",
                            "! ctx 20 low - restart"};
    expectRows(rows, n, actions, labels, 8);
  }

  {  // oled_width
    llmhome::State s;
    s.guided = s.retry = true;
    std::snprintf(s.model, sizeof(s.model), "LFM2-8B-A1B-UD-Q3_K_XL.gguf");
    s.lowContext = true;
    s.context = 23;
    const size_t n = llmhome::rows(s, 19, rows, 12);
    const A actions[] = {A::Open, A::Ask, A::Guided, A::NewChat, A::Rerun, A::Model, A::Model};
    const char* labels[] = {"Open chat", "Ask (Mic / Keys)", "Ask (guided)", "New chat",
                            "Re-run last", "Model: LFM2-8~_K_XL", "!ctx 23 low-restart"};
    expectRows(rows, n, actions, labels, 7);
    for (size_t i = 0; i < n; i++) assert(std::strlen(rows[i].label) <= 19);
  }

  {  // fit_name
    char out[32];
    llmhome::fitName("tiny.bin", 12, out, sizeof(out));
    assert(std::strcmp(out, "tiny") == 0);
    llmhome::fitName("ABCDEFGHIJKLMNOP", 9, out, sizeof(out));
    assert(std::strcmp(out, "ABCD~MNOP") == 0);
    llmhome::fitName("ABCDEFGHIJKLMNOP", 2, out, sizeof(out));
    assert(std::strcmp(out, "AB") == 0);
    llmhome::fitName(nullptr, 9, out, sizeof(out));
    assert(out[0] == '\0');
    llmhome::fitName("ABCDEFGHIJKLMNOP", 40, out, 5);   // the buffer bounds the width
    assert(std::strcmp(out, "AB~P") == 0);
  }

  {  // cap
    llmhome::State s;
    s.terminal = s.guided = s.retry = true;
    llmhome::Row few[3];
    assert(llmhome::rows(s, 51, few, 3) == 3);
    assert(few[2].action == A::Ask);
    assert(llmhome::rows(s, 51, nullptr, 3) == 0);
  }

  std::puts("LLM home menu checks passed");
  return 0;
}
