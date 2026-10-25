// Production stt/stt_transcript.cpp (the user vocabulary and display
// formatter ported from the standalone STT handoff): casing, whitespace, "I"
// contractions, exact whole-word display aliases, immutable raw input, output
// bounds with sentinels, vocabulary validation and capacity. Built with
// ASan/UBSan when HW1_SANITIZE is on.
#include "stt/stt_transcript.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
using namespace hw1::transcript;
int main() {
  Vocabulary v; char error[160], out[4096];
  assert(v.put("HardwareOne", nullptr, error, sizeof(error)));
  assert(v.put("OpenAI", nullptr, error, sizeof(error)));
  assert(v.put("ESP32", "esp", error, sizeof(error)));
  assert(!v.put("ESP32", nullptr, error, sizeof(error)) && error[0]);   // digits need a spoken form
  assert(!v.put("bad word", "bad", error, sizeof(error)));
  assert(!v.put("Bad", "Bad", error, sizeof(error)));
  assert(!v.put("Bad", "bad word", error, sizeof(error)));
  assert(!v.put("Bad", "'bad", error, sizeof(error)));
  assert(!v.put("Bad", "ba''d", error, sizeof(error)));
  assert(!v.put("a|b", nullptr, error, sizeof(error)));
  assert(!v.put("", nullptr, error, sizeof(error)));
  assert(!v.put(nullptr, nullptr, error, sizeof(error)));
  assert(v.count() == 3);
  assert(format("  i think\t hardwareone\n and openai  ", v, true, out, sizeof(out)));
  assert(!std::strcmp(out, "I think HardwareOne and OpenAI"));
  assert(format("i'm going and i've said i'll do it i'd like", v, false, out, sizeof(out)));
  assert(!std::strcmp(out, "I'm going and I've said I'll do it I'd like"));
  assert(format("hardwareoneish esp", v, false, out, sizeof(out)));
  assert(!std::strcmp(out, "hardwareoneish ESP32"));          // whole words only
  assert(format("HARDWAREONE Esp", v, false, out, sizeof(out)));
  assert(!std::strcmp(out, "HardwareOne ESP32"));             // case-insensitive match
  assert(format("this is", v, true, out, sizeof(out)));
  assert(!std::strcmp(out, "This is"));
  assert(format("still the same paragraph", v, false, out, sizeof(out)));
  assert(!std::strcmp(out, "still the same paragraph"));
  assert(format("it's i'min isn't", v, true, out, sizeof(out)));
  assert(!std::strcmp(out, "It's i'min isn't"));              // no partial contraction casing
  assert(format("  \n\t ", v, true, out, sizeof(out)) && !out[0]);
  assert(!format("hardwareone", v, true, out, 5) && !out[0]);
  assert(format("a", v, false, out, 2) && !std::strcmp(out, "a"));
  assert(!format("a", v, false, out, 1) && !out[0]);
  assert(!format("a", v, false, out, 0) && !format(nullptr, v, false, out, 4) && !format("a", v, false, nullptr, 4));
  assert(!format(out, v, false, out, sizeof(out)));           // aliasing refused
  assert(v.put("OPENAI", "openai", error, sizeof(error)) && v.count() == 3);  // replace keeps one entry
  assert(format("openai", v, false, out, sizeof(out)) && !std::strcmp(out, "OPENAI"));
  assert(v.remove("openai") && v.count() == 2 && !v.remove("openai") && !v.remove(nullptr));
  assert(!v.display("openai", 6));
  Vocabulary full;
  for (size_t i = 0; i < kMaxWords; ++i) {
    std::string word = "word"; size_t n = i;
    for (int j = 0; j < 3; ++j) { word += char('a' + n % 26); n /= 26; }
    assert(full.put(word.c_str(), nullptr, error, sizeof(error)));
  }
  assert(!full.put("extra", nullptr, error, sizeof(error)) && std::strstr(error, "full"));
  assert(full.put("WORDaaa", "wordaaa", error, sizeof(error)) && full.count() == kMaxWords);
  // Raw is immutable and output growth cannot corrupt caller sentinels.
  const std::string raw = "i hardwareone ";
  for (size_t cap = 1; cap < 40; ++cap) {
    char guarded[48]; std::memset(guarded, '#', sizeof(guarded));
    const bool ok = format(raw.c_str(), v, true, guarded + 1, cap);
    assert(guarded[0] == '#' && guarded[cap + 1] == '#');
    if (ok) assert(!std::strcmp(guarded + 1, "I HardwareOne")); else assert(!guarded[1]);
    assert(raw == "i hardwareone ");
  }
  // A longest alias in every slot of a 512-byte raw phrase stays bounded.
  Vocabulary wide; char longest[kDisplayBytes + 1]; std::memset(longest, 'X', kDisplayBytes); longest[kDisplayBytes] = 0;
  assert(wide.put(longest, "a", error, sizeof(error)));
  std::string many; for (int i = 0; i < 256; ++i) many += "a ";
  assert(!format(many.c_str(), wide, false, out, 4096) && !out[0]);
  char big[256 * (kDisplayBytes + 1) + 1];
  assert(format(many.c_str(), wide, false, big, sizeof(big)) && std::strlen(big) == 256 * (kDisplayBytes + 1) - 1);
  std::puts("stt_transcript: formatting, vocabulary bounds and aliases passed");
}
