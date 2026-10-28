#ifndef SYSTEM_LLM_HOME_MENU_CORE_H
#define SYSTEM_LLM_HOME_MENU_CORE_H
// The LLM app's home menu: which actions it offers, in what order, and how they
// are labelled. The glasses (G2_Glasses.cpp) and the local display
// (OLED_Mode_LLM.cpp) both build their home list from this, so the two cannot
// drift apart. The caller gathers the backend state (llmHomeMenuRowsNow in
// System_LLMBackend) and adds its own presentation rows, such as the lens'
// "<- Back". No OS dependencies, so the host test runs this file as is.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

enum class LlmHomeAction : uint8_t {
  Back,      // the lens' own back row; never produced here
  Open,      // the chat view
  Terminal,  // the glasses' native Terminal app
  Ask,       // a typed or dictated question
  Guided,    // guided questions from the model's menu
  NewChat,   // clear the conversation
  Rerun,     // ask the last question again
  Model,     // the model picker (the low-context warning row leads here too)
};

namespace llmhome {
constexpr size_t kModelNameCap = 32;   // LLM_MODEL_NAME_LEN; System_LLMBackend.cpp checks it
constexpr size_t kLabelCap = 48;

struct State {
  bool terminal = false;           // the surface offers its native Terminal app
  bool guided = false;             // the loaded model ships guided questions
  bool retry = false;              // the active source can re-run the last question
  char model[kModelNameCap] = {};  // the ready model's display name; empty = none ready
  bool lowContext = false;         // the context was fitted too small to be usable
  int context = 0;                 // that context size, for the warning row
};

struct Row {
  LlmHomeAction action;
  char label[kLabelCap];
};

// Fits a model name in `width` characters: drops a .gguf/.bin extension, then
// characters from the middle ("head~tail"), because variants of one model
// differ at the end ("...-Q3_K_XL" vs "...-Q4_0").
inline void fitName(const char* name, size_t width, char* out, size_t cap) {
  if (!out || cap == 0) return;
  char base[kModelNameCap];
  snprintf(base, sizeof(base), "%s", name ? name : "");
  char* dot = strrchr(base, '.');
  if (dot && (strcasecmp(dot, ".gguf") == 0 || strcasecmp(dot, ".bin") == 0)) *dot = '\0';
  if (width >= cap) width = cap - 1;
  const size_t len = strlen(base);
  if (len <= width || width < 3) {
    snprintf(out, width + 1, "%s", base);
    return;
  }
  const size_t head = width / 2;
  const size_t tail = width - 1 - head;
  memcpy(out, base, head);
  out[head] = '~';
  memcpy(out + head + 1, base + len - tail, tail);
  out[width] = '\0';
}

// Fills `out` with up to `cap` rows and returns how many. `width` is the
// surface's usable characters per row, after any cursor.
inline size_t rows(const State& s, size_t width, Row* out, size_t cap) {
  if (!out) return 0;
  if (width > kLabelCap - 1) width = kLabelCap - 1;
  size_t n = 0;
  auto add = [&](LlmHomeAction action, const char* text) {
    if (n >= cap) return;
    out[n].action = action;
    snprintf(out[n].label, width + 1, "%s", text);
    ++n;
  };
  add(LlmHomeAction::Open, "Open chat");
  if (s.terminal) add(LlmHomeAction::Terminal, "Open Terminal");
  add(LlmHomeAction::Ask, "Ask (Mic / Keys)");
  if (s.guided) add(LlmHomeAction::Guided, "Ask (guided)");
  add(LlmHomeAction::NewChat, "New chat");
  if (s.retry) add(LlmHomeAction::Rerun, "Re-run last");
  if (s.model[0]) {
    static constexpr char kPrefix[] = "Model: ";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    char name[kLabelCap];
    fitName(s.model, width > kPrefixLen ? width - kPrefixLen : 0, name, sizeof(name));
    char label[kLabelCap + kPrefixLen];
    snprintf(label, sizeof(label), "%s%s", kPrefix, name);
    add(LlmHomeAction::Model, label);
  } else {
    add(LlmHomeAction::Model, "Select Model");
  }
  if (s.lowContext) {
    char warn[kLabelCap];
    snprintf(warn, sizeof(warn), "! ctx %d low - restart", s.context);
    if (strlen(warn) > width) snprintf(warn, sizeof(warn), "!ctx %d low-restart", s.context);
    add(LlmHomeAction::Model, warn);
  }
  return n;
}
}  // namespace llmhome

#endif  // SYSTEM_LLM_HOME_MENU_CORE_H
