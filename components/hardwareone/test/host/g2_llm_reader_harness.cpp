#include "G2_LlmReader.h"
#include "System_LLMReplyLimits.h"
#include <assert.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

static std::vector<uint8_t> sendAndReassemble(const uint8_t* pb, size_t length) {
  G2Temple arm;
  testArm = &arm;
  testFrames.clear();
  testFault = WireFault::None;
  testLifecycleOpen = true;
  testLifecycle = kTestLifecycle;
  testPresentation = kTestPresentation;
  testDelayCount = testWriteAttempts = 0;
  bool finalAttempted = false;
  assert(sendPbFragmented(arm, 42, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                          pb, length, arm.connectionGeneration,
                          &finalAttempted, 0, kTestLifecycle, kTestPresentation));
  assert(finalAttempted && !testCentralHeld && !testPeerHeld);
  assert(!testFrames.empty() && testDelayCount == testFrames.size() - 1);
  std::vector<uint8_t> joined;
  uint16_t wireCrc = 0;
  for (size_t i = 0; i < testFrames.size(); ++i) {
    const auto& frame = testFrames[i];
    const bool last = i + 1 == testFrames.size();
    assert(frame.size() <= 241 && frame.size() >= G2_ENVELOPE_HDR_LEN);
    assert(frame[0] == G2_PREAMBLE_0 && frame[1] == G2_PREAMBLE_TX);
    assert(frame[2] == 42 && frame[3] + G2_ENVELOPE_HDR_LEN == frame.size());
    assert(frame[4] == testFrames.size() && frame[5] == i + 1);
    assert(frame[6] == G2_SID_EVEN_CORE && frame[7] == G2_FLAG_REQUEST);
    const size_t end = frame.size() - (last ? 2 : 0);
    joined.insert(joined.end(), frame.begin() + G2_ENVELOPE_HDR_LEN, frame.begin() + end);
    if (last) wireCrc = uint16_t(frame[end]) | uint16_t(frame[end + 1]) << 8;
  }
  assert(joined.size() == length && memcmp(joined.data(), pb, length) == 0);
  assert(wireCrc == g2CrcCcittFalse(joined.data(), joined.size()));
  return joined;
}

static void checkTextObject(const uint8_t* data, size_t len, const char* text) {
  size_t pos = 0, found = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(data, len, &pos, &field, &wire));
    if (field == G2_TEXT_F_CONTENT || field == G2_TEXT_F_CNAME) {
      uint64_t bytes;
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      assert(g2PbReadVarint(data, len, &pos, &bytes));
      const char* expected = field == G2_TEXT_F_CONTENT ? text : "llmChat";
      assert(bytes == strlen(expected) && bytes <= len - pos);
      assert(memcmp(data + pos, expected, size_t(bytes)) == 0);
      pos += size_t(bytes);
      ++found;
    } else if (field == G2_TEXT_F_CID || field == G2_TEXT_F_EVCAP ||
               field == G2_TEXT_F_X || field == G2_TEXT_F_Y ||
               field == G2_TEXT_F_W || field == G2_TEXT_F_H) {
      uint64_t value;
      assert(wire == G2_PB_WIRE_VARINT && g2PbReadVarint(data, len, &pos, &value));
      const uint64_t expected = field == G2_TEXT_F_W ? 560 : field == G2_TEXT_F_H ? 272 :
          field == G2_TEXT_F_X || field == G2_TEXT_F_Y ? 8 : 1;
      assert(value == expected);
      ++found;
    } else assert(g2PbSkipField(data, len, &pos, wire));
  }
  assert(found == 8); // Exact child name/content, identity, capture and geometry.
}

static void checkRebuildPage(const uint8_t* data, size_t len, const char* text) {
  size_t pos = 0, children = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(data, len, &pos, &field, &wire));
    if (field == G2_PAGE_F_TEXT_OBJ) {
      uint64_t bytes;
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      assert(g2PbReadVarint(data, len, &pos, &bytes) && bytes <= len - pos);
      checkTextObject(data + pos, size_t(bytes), text);
      pos += size_t(bytes);
      ++children;
    } else if (field == G2_PAGE_F_TOTAL) {
      uint64_t count;
      assert(wire == G2_PB_WIRE_VARINT && g2PbReadVarint(data, len, &pos, &count) && count == 1);
    } else assert(g2PbSkipField(data, len, &pos, wire));
  }
  assert(children == 1);
}

static void checkWire(const char* text) {
  uint8_t protobuf[8192];
  const size_t length = g2BuildRebuildTextPb(G2_MAGIC_REBUILD, "llmChat", text,
      G2_GEOM_LARGE, true, protobuf, sizeof(protobuf));
  assert(length);
  const auto rebuilt = sendAndReassemble(protobuf, length);
  size_t pos = 0, pages = 0, commands = 0;
  while (pos < rebuilt.size()) {
    uint32_t field; uint8_t wire;
    assert(g2PbReadTag(rebuilt.data(), rebuilt.size(), &pos, &field, &wire));
    if (field == G2_WRAP_F_REBUILD) {
      uint64_t bytes;
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      assert(g2PbReadVarint(rebuilt.data(), rebuilt.size(), &pos, &bytes));
      assert(bytes <= rebuilt.size() - pos);
      checkRebuildPage(rebuilt.data() + pos, size_t(bytes), text);
      pos += size_t(bytes);
      ++pages;
    } else if (field == G2_WRAP_F_CMD) {
      uint64_t cmd;
      assert(wire == G2_PB_WIRE_VARINT);
      assert(g2PbReadVarint(rebuilt.data(), rebuilt.size(), &pos, &cmd));
      assert(cmd == G2_CMD_REBUILD_PAGE);
      ++commands;
    } else assert(g2PbSkipField(rebuilt.data(), rebuilt.size(), &pos, wire));
  }
  assert(pages == 1 && commands == 1);
}

static void fragmentBoundariesAndFences() {
  for (size_t length : {size_t(1), size_t(230), size_t(231), size_t(232), size_t(233),
                       size_t(463), size_t(464), size_t(465), size_t(695), size_t(696)}) {
    std::vector<uint8_t> bytes(length);
    for (size_t i = 0; i < length; ++i) bytes[i] = uint8_t(i);
    sendAndReassemble(bytes.data(), bytes.size());
  }
  for (WireFault fault : {WireFault::DisconnectAfterFirst, WireFault::TerminalAfterFirst,
                         WireFault::PresentationAfterFirst, WireFault::FailSecondWrite}) {
    G2Temple arm;
    testArm = &arm;
    testFrames.clear();
    testFault = fault;
    testLifecycleOpen = true;
    testPresentation = kTestPresentation;
    testWriteAttempts = testDelayCount = 0;
    std::vector<uint8_t> bytes(700, 'x');
    bool finalAttempted = true;
    assert(!sendPbFragmented(arm, 42, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                             bytes.data(), bytes.size(), 52, &finalAttempted, 0,
                             kTestLifecycle, kTestPresentation));
    assert(!finalAttempted && testFrames.size() == 1);
    assert(testWriteAttempts == (fault == WireFault::FailSecondWrite ? 2u : 1u));
    assert(!testCentralHeld && !testPeerHeld);
  }
  testFault = WireFault::None;
  testLifecycleOpen = true;
}

// Validate every rendered UTF-8 string independently, including scalar bounds.
static bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = s[i++];
    if (c < 0x80) continue;
    unsigned cp = 0, min = 0;
    size_t remaining = 0;
    if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; min = 0x80; remaining = 1; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; min = 0x800; remaining = 2; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 7; min = 0x10000; remaining = 3; }
    else return false;
    if (remaining > s.size() - i) return false;
    while (remaining--) {
      const unsigned char next = s[i++];
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}

static std::string withoutNewlines(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
  return s;
}

struct Fixture {
  std::vector<char> wrapped;
  std::vector<uint16_t> offsets;
  TextPager pager;
  Fixture(size_t inputCap = kLlmReplyCapacityBytes)
      : wrapped(G2LlmReader::wrappedCapacity(inputCap), '#'),
        offsets(G2LlmReader::pageOffsetsCapacity(inputCap), 0xffff),
        pager{wrapped.data(), offsets.data(), static_cast<int>(offsets.size() - 1),
              G2LlmReader::kBodyBytes, 0, 0, false} {}
  size_t rebuild(const std::string& raw, bool done = true) {
    return G2LlmReader::rebuild(raw.data(), raw.size(), done, pager, wrapped.data(), wrapped.size());
  }
};

static std::string verifyPages(Fixture& f, bool done = true) {
  auto& p = f.pager;
  std::string joined;
  for (int i = 0; i < p.pageCount; ++i) {
    p.curPage = i;
    const std::string body(p.body + p.pageOff[i], p.pageOff[i + 1] - p.pageOff[i]);
    assert(!body.empty());
    assert(body.size() <= G2LlmReader::kBodyBytes);
    assert(validUtf8(body));
    const size_t lines = std::count(body.begin(), body.end(), '\n') + (body.back() == '\n' ? 0 : 1);
    assert(lines <= G2LlmReader::kBodyLines);
    char frame[G2LlmReader::kFrameCapacity];
    const size_t n = G2LlmReader::compose(p, done, frame, sizeof(frame));
    assert(n && n < G2LlmReader::kFrameCapacity && strlen(frame) == n && validUtf8(frame));
    checkWire(frame);
    const std::string rendered(frame);
    assert(rendered.find("Reply [" + std::to_string(i + 1) + "/" + std::to_string(p.pageCount) + "]") == 0);
    assert(rendered.find(body) != std::string::npos);
    const std::string footer = "Hold: ask | 2x: back";
    assert(rendered.substr(rendered.size() - footer.size()) == footer);
    joined += body;
  }
  return joined;
}

static void fullReply() {
  Fixture f;
  std::string raw = "THE-BEGINNING ";
  while (raw.size() < 16000) raw += "All of this answer remains available. ";
  raw += " THE-END";
  assert(raw.size() < kLlmReplyCapacityBytes);
  const size_t n = f.rebuild(raw);
  assert(!f.pager.truncated && n == strlen(f.wrapped.data()));
  const std::string joined = verifyPages(f);
  assert(joined == f.wrapped.data());
  assert(withoutNewlines(joined) == raw);
  assert(joined.find("THE-BEGINNING") == 0);
  assert(joined.substr(joined.size() - 7) == "THE-END");
  assert(f.pager.pageCount > 12);  // Exercises substantially more than old 2 KiB.
}

static void shortLines() {
  Fixture f;
  std::string raw(kLlmReplyCapacityBytes - 1, '\n');
  f.rebuild(raw);
  assert(!f.pager.truncated && f.pager.pageCount > 256);
  assert(verifyPages(f, false) == raw);
  raw.clear();
  while (raw.size() + 3 < kLlmReplyCapacityBytes) raw += "a\n\n";
  f.rebuild(raw);
  assert(!f.pager.truncated && verifyPages(f) == raw);
}

static void unicodeStreaming() {
  Fixture f;
  const std::string unit = u8"Café — 汉字 😀 ";
  std::string raw;
  for (int i = 0; i < 180; ++i) raw += unit;
  f.rebuild(raw);
  assert(!f.pager.truncated);
  assert(withoutNewlines(verifyPages(f)) == raw);
  // Check every split of a stream containing all UTF-8 widths. An incomplete
  // final character must be absent until its last byte arrives, then restored.
  const std::string prefix(47, 'x');
  const std::string sample = prefix + unit + prefix + unit;
  std::string lastComplete;
  for (size_t cut = 0; cut <= sample.size(); ++cut) {
    const std::string partial = sample.substr(0, cut);
    if (validUtf8(partial)) lastComplete = partial;
    f.rebuild(partial, false);
    assert(!f.pager.truncated);
    assert(withoutNewlines(verifyPages(f, false)) == lastComplete);
  }
  f.rebuild(std::string("ok ") + "\xf0\x9f", true);
  assert(std::string(f.wrapped.data()) == "ok ?");
  f.rebuild(std::string("a\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80z"), true);
  assert(validUtf8(f.wrapped.data()));
  assert(f.wrapped.front() == 'a');
  assert(f.wrapped[strlen(f.wrapped.data()) - 1] == 'z');
}

static void positionAndNavigation() {
  Fixture f;
  f.rebuild(std::string(2600, 'a'), false);
  f.pager.curPage = 4;
  const auto firstOffset = f.pager.pageOff[4];
  f.rebuild(std::string(4300, 'a'), false);
  assert(f.pager.curPage == 4 && f.pager.pageOff[4] == firstOffset);
  G2LlmReader::navigate(f.pager, false);
  assert(f.pager.curPage == 3);
  f.pager.curPage = 0;
  G2LlmReader::navigate(f.pager, false);
  assert(f.pager.curPage == 0);
  f.pager.curPage = f.pager.pageCount - 1;
  G2LlmReader::navigate(f.pager, true);
  assert(f.pager.curPage == f.pager.pageCount - 1);
  f.rebuild("short replacement");
  assert(f.pager.curPage == 0);
  f.rebuild("");
  G2LlmReader::navigate(f.pager, true);
  assert(f.pager.curPage == 0);
}

static void boundsAndEmpty() {
  Fixture f;
  char frame[G2LlmReader::kFrameCapacity];
  f.rebuild("", false);
  assert(G2LlmReader::compose(f.pager, false, frame, sizeof(frame)));
  assert(std::string(frame).find("Reply [1/1] ...\nWaiting for reply...") == 0);
  assert(G2LlmReader::compose(f.pager, true, frame, sizeof(frame)));
  assert(std::string(frame).find("(empty reply)") != std::string::npos);
  assert(!G2LlmReader::compose(f.pager, true, frame, 4) && !frame[0]);

  const std::string raw = u8"Hello café — 汉字 😀 and more.";
  for (size_t cap = 1; cap < raw.size(); ++cap) {
    std::vector<char> tiny(cap + 1, '#');
    const size_t n = G2LlmReader::rebuild(raw.data(), raw.size(), true, f.pager, tiny.data(), cap);
    assert(n < cap && tiny[cap] == '#' && tiny[n] == '\0');
    assert(validUtf8(tiny.data()) && f.pager.truncated);
  }
  f.pager.maxPages = 1;
  std::fill(f.offsets.begin(), f.offsets.end(), 0xbeef);
  f.rebuild(std::string(600, 'a'));
  assert(f.pager.truncated && f.pager.pageCount == 1);
  assert(f.offsets[2] == 0xbeef);
  verifyPages(f);
  f.rebuild("A\r\nB\tC\x01\x7f");
  assert(std::string(f.wrapped.data()) == "A\nB C");
}

static void fullWireBudget() {
  // The declared maximum itself must fit production REBUILD and its actual
  // fragmenter; checking a smaller prose frame would miss byte-budget errors.
  checkWire(std::string(G2LlmReader::kFrameCapacity - 1, 'x').c_str());
  assert(testFrames.size() >= 3);
  Fixture f(65536);
  std::string raw(60000, '\n');
  for (size_t line = 0; line < G2LlmReader::kBodyLines; ++line) {
    for (size_t column = 0; column < G2LlmReader::kColumns / 2; ++column) raw += u8"😀";
    raw += '\n';
  }
  f.rebuild(raw, false);
  assert(!f.pager.truncated && f.pager.pageCount == 10001);
  f.pager.curPage = f.pager.pageCount - 1;
  assert(f.pager.pageOff[f.pager.pageCount] - f.pager.pageOff[f.pager.curPage] == G2LlmReader::kBodyBytes);
  f.pager.truncated = true;
  char frame[G2LlmReader::kFrameCapacity];
  const size_t n = G2LlmReader::compose(f.pager, false, frame, sizeof(frame));
  assert(n > 600 && n < G2LlmReader::kFrameCapacity && validUtf8(frame));
  checkWire(frame);
  // Ordinary ASCII now fills all six 48-column rows on one page.
  f.rebuild(std::string(6 * 48, 'x'));
  assert(f.pager.pageCount == 1);
  assert(verifyPages(f).size() == 6 * 48 + 5);
}

int main() {
  fullReply();
  shortLines();
  unicodeStreaming();
  positionAndNavigation();
  boundsAndEmpty();
  fullWireBudget();
  fragmentBoundariesAndFences();
  std::cout << "G2 LLM reader: complete retained replies, UTF-8 streams, page bounds, navigation, full-height REBUILD and fenced <=241-byte wire fragments passed\n";
}
