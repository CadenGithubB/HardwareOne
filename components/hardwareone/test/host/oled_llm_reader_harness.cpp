// Runs the OLED LLM chat view's reply reader (OLED_Mode_LLM.cpp: rebuildReader,
// syncReader, readerLine, scrollReader, foldKeepingNewlines) over a scripted
// chat module, with the shared wrap core (System_TextPager.h) and the ASCII
// fold (System_LLMUtf8.h) compiled as they ship.
//
// Cases:
//   long_reply   a multi-paragraph remote reply far longer than 12 lines opens
//                at its first line under the question, keeps its paragraph
//                breaks, fits 20 columns, and every byte is reachable by
//                scrolling (the old view kept only the newest 12 lines).
//   streaming    a live run is re-read only when it grows or finishes; the
//                reading position survives growth; a new run reopens at line 0.
//   states       nothing asked, waiting, empty reply, cleared, replaced run.
//   line_cap     an all-newline reply stops at the line table, in bounds.
//   live         the screen asks for its next frame while a reply is still
//                arriving, a model loads or a command is out, and stops once
//                idle (the OLED redraws only a dirty screen).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "System_LLMUtf8.h"
#include "System_TextPager.h"

#define EXT_RAM_BSS_ATTR
#ifndef OLED_CONTENT_HEIGHT
#define OLED_CONTENT_HEIGHT 43   // 128x64 OLED: 64 - 10 header - 10 footer - 1
#endif
#define LLM_CHAT_TURN_MAX_BYTES (16u * 1024u)

using ChatRunToken = uint64_t;
enum class ChatTurnRole : uint8_t { USER = 0, ASSISTANT = 1 };
struct ChatTurnInfo {
  ChatTurnRole role;
  uint32_t textLen;
  uint32_t tokenCount;
  uint16_t tokensPerSecX10;
  bool isStreaming;
};

// Scripted chat module: one question, one run.
static std::string gQuestion;
static std::string gReply;
static ChatRunToken gRun = 0;
static bool gDone = false;
static unsigned gFullReads = 0;   // whole-reply chatReadRun calls (not 1-byte probes)

int chatGetTurnCount() { return gRun ? 2 : 0; }
bool chatGetTurnInfo(int index, ChatTurnInfo* out) {
  if (index < 0 || index >= chatGetTurnCount()) return false;
  *out = ChatTurnInfo{};
  out->role = index == 0 ? ChatTurnRole::USER : ChatTurnRole::ASSISTANT;
  return true;
}
static int copyOut(const std::string& text, int offset, char* buf, int maxLen) {
  if (!buf || maxLen <= 0) return 0;
  if (offset < 0 || (size_t)offset >= text.size()) { buf[0] = '\0'; return 0; }
  const int n = (int)std::min(text.size() - (size_t)offset, (size_t)maxLen - 1);
  memcpy(buf, text.data() + offset, (size_t)n);
  buf[n] = '\0';
  return n;
}
int chatReadTurn(int index, int offset, char* buf, int maxLen) {
  return copyOut(index == 0 ? gQuestion : gReply, offset, buf, maxLen);
}
ChatRunToken chatGetRunToken() { return gRun; }
int chatReadRun(ChatRunToken run, int offset, char* buf, int maxLen, bool* done) {
  if (run != gRun) { if (done) *done = true; return -1; }
  if (maxLen > 2) ++gFullReads;
  if (done) *done = gDone;
  return copyOut(gReply, offset, buf, maxLen);
}

// INSERT_PRODUCTION_DEFINITIONS

static std::string line(int i) {
  char out[64];
  readerLine(i, out, sizeof(out));
  return out;
}

static void dump(const char* title) {
  std::printf("  %s: %d lines, top %d\n", title, sReplyLines, sReaderTop);
  for (int i = 0; i < sReplyLines && i < 8; i++) std::printf("    |%s|\n", line(i).c_str());
}

int main() {
  static_assert(LLM_READER_COLS == 20, "one column is left for the scroll marks");

  {  // long_reply
    gQuestion = "What does the gateway dashboard show when an agent finishes a task?";
    gReply =
        "The gateway dashboard shows each run as a card. When an agent finishes, "
        "its card turns green and lists the tools it called, how long each took "
        "and what it returned.\n\n"
        "Open a card to see the full transcript \xe2\x80\x94 the prompt, every "
        "intermediate message and the final answer.\n"
        "\xe2\x80\x9cRetry\xe2\x80\x9d re-runs the same request; \xe2\x80\x9cStop\xe2\x80\x9d "
        "cancels one that is still going. Logs older than a week are archived.";
    gRun = 7;
    gDone = true;
    sReplyStale = true;
    syncReader();
    dump("long_reply");
    assert(sReaderTop == 0);
    assert(sReplyLines > 12);   // more than the old view could ever show
    assert(line(0).rfind("> What does", 0) == 0);
    assert(line(1).rfind("  ", 0) == 0);           // question continues indented
    assert(line(2).rfind("The gateway", 0) == 0);  // two question rows, then the reply
    std::string joined;
    for (int i = 0; i < sReplyLines; i++) {
      const std::string l = line(i);
      assert(l.size() <= 20);
      for (char c : l) assert(c >= 0x20 && c < 0x7f);
      joined += l + "\n";
    }
    assert(joined.find("\n\nOpen a card") != std::string::npos);   // blank line kept
    std::string flat;   // the reply without its wraps, to find text across them
    for (int i = 2; i < sReplyLines; i++) flat += line(i);
    for (const char* word : {"card turns", "transcript", "archived.",
                             " - the prompt",          // em dash folded
                             "\"Retry\" re-runs"}) {    // curly quotes folded
      std::string w = word;
      bool found = false;   // a word may straddle a wrap, so compare unbroken text
      for (size_t k = 0; k + w.size() <= flat.size() && !found; k++)
        found = flat.compare(k, w.size(), w) == 0;
      assert(found);
    }
    scrollReader(1000);
    assert(sReaderTop == sReplyLines - LLM_VISIBLE_LINES);
    assert(line(sReplyLines - 1).find("archived.") != std::string::npos);
    scrollReader(-1000);
    assert(sReaderTop == 0);
  }

  {  // streaming
    gQuestion = "Count";
    gReply = "";
    gRun = 8;
    gDone = false;
    syncReader();
    assert(line(1) == "Waiting for reply...");
    const unsigned reads = gFullReads;
    syncReader();
    syncReader();
    assert(gFullReads == reads);   // nothing new: no re-read
    for (int i = 1; i <= 30; i++) gReply += "line " + std::to_string(i) + "\n";
    syncReader();
    assert(gFullReads == reads + 1);
    assert(line(1) == "line 1");
    scrollReader(5);
    const int top = sReaderTop;
    gReply += "line 31\n";
    syncReader();
    assert(sReaderTop == top);     // growth keeps the reading position
    gDone = true;
    syncReader();
    assert(sReplyDone);
    const unsigned settled = gFullReads;
    syncReader();
    assert(gFullReads == settled); // a finished run is left alone
    gRun = 9;
    gReply = "next";
    syncReader();
    assert(sReaderTop == 0 && line(1) == "next");
  }

  {  // states
    gRun = 0;
    syncReader();
    assert(sReplyLines == 0);      // nothing asked: the view shows its prompt
    gRun = 10;
    gReply = "";
    gDone = true;
    syncReader();
    assert(line(1) == "(empty reply)");
    gRun = 11;                     // replaced between frames: the old token reads -1
    sReplyRun = 10;
    sReplyDone = false;
    syncReader();
    assert(sReplyRun == 11);
  }

  {  // line_cap
    gQuestion = "";
    gReply.assign(5000, '\n');
    gReply += "end";
    gRun = 12;
    gDone = true;
    syncReader();
    assert(sReplyLines <= LLM_READER_MAX_LINES);
    scrollReader(100000);
    assert(sReaderTop == sReplyLines - LLM_VISIBLE_LINES);
  }

  {  // live
    sUIState = LLMUIState::HOME;
    sCmdInFlight = LLM_CMD_NONE;
    gRun = 0;
    syncReader();
    assert(!llmScreenIsLive());          // idle: wait for input
    gQuestion = "Hi";
    gReply = "";
    gRun = 13;
    gDone = false;
    syncReader();
    assert(llmScreenIsLive());           // waiting for the reply
    gReply = "Hello";
    syncReader();
    assert(llmScreenIsLive());           // arriving
    gDone = true;
    syncReader();
    assert(!llmScreenIsLive());          // finished and drawn
    sUIState = LLMUIState::LOADING;
    assert(llmScreenIsLive());
    sUIState = LLMUIState::GENERATING;
    assert(llmScreenIsLive());
    sUIState = LLMUIState::READY;
    sCmdInFlight = LLM_CMD_UNLOAD;
    assert(llmScreenIsLive());
    sCmdInFlight = LLM_CMD_NONE;
    assert(!llmScreenIsLive());
  }

  std::puts("OLED LLM reader checks passed");
  return 0;
}
