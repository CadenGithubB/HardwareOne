#include "../../System_LLMHttpCore.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

using hw1http::Stream;
static const std::string hello =
    ": keepalive\r\n\r\n"
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\r\n\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \\u263a \xf0\x9f\x8c\x8d\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\r\n\r\n";

static void fragmented() {
  for (size_t split = 0; split <= hello.size(); ++split) {
    auto s = std::make_unique<Stream>();
    assert(s->feed(hello.data(), split));
    assert(s->feed(hello.data() + split, hello.size() - split));
    assert(s->done && !s->error);
    assert(std::string(s->text.bytes) == "Hello \xe2\x98\xba \xf0\x9f\x8c\x8d");
  }
  auto s = std::make_unique<Stream>();
  for (char c : hello) assert(s->feed(&c, 1));
  assert(s->done);
  auto multiline = std::make_unique<Stream>();
  std::string wire = "data: {\"choices\":\n"
                     "data: [{\"delta\":{\"content\":\"yes\"}}]}\n\n"
                     "data: [DONE]\n\n";
  assert(multiline->feed(wire.data(), wire.size()));
  assert(multiline->done && std::string(multiline->text.bytes) == "yes");
}

static void failedStreams() {
  for (const std::string wire : {
         "data: {bad json}\n\n", "data: 42\n\n",
         "data: {\"error\":{\"message\":\"SECRET SHOULD NOT LEAK\"}}\n\n",
         "event: error\ndata: {\"message\":\"SECRET SHOULD NOT LEAK\"}\n\n",
         "data: {\"choices\":[{\"delta\":{\"content\":[1]}}]}\n\n"}) {
    auto s = std::make_unique<Stream>();
    assert(!s->feed(wire.data(), wire.size()));
    assert(s->error && !s->done);
    assert(!std::strstr(s->error, "SECRET"));
  }
  auto partial = std::make_unique<Stream>();
  std::string wire = "data: {\"choices\":[{\"delta\":{\"content\":\"already shown\"}}]}\n\n"
                     "data: {\"error\":\"fail\"}\n\n";
  assert(!partial->feed(wire.data(), wire.size()));
  assert(std::string(partial->text.bytes) == "already shown");
  auto missing = std::make_unique<Stream>();
  wire = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":\"stop\"}]}\n\n";
  assert(missing->feed(wire.data(), wire.size()));
  missing->eof(); assert(missing->error && !missing->done);
  auto oversized = std::make_unique<Stream>();
  wire = "data: " + std::string(hw1http::kEventMax + 1, 'x');
  assert(!oversized->feed(wire.data(), wire.size()));
  auto manyLines = std::make_unique<Stream>();
  wire = "data: " + std::string(5000, 'x') + "\ndata: " + std::string(5000, 'x') + "\n";
  assert(!manyLines->feed(wire.data(), wire.size()));
}

static void utf8AndTruncation() {
  hw1http::Text t;
  t.append("\xf0\x9f", 2); assert(t.size == 0);
  t.append("\x8c\x8d", 2); assert(t.size == 4);
  t.append("\xed\xa0\x80\xc0\xaf\0", 6);
  assert(std::string(t.bytes) == "\xf0\x9f\x8c\x8d\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd ");
  t.append("\xe2", 1); t.finish(false);
  assert(std::string(t.bytes).substr(t.size - 3) == "\xef\xbf\xbd");
  hw1http::Text large;
  std::string previous;
  for (unsigned i = 0; i < hw1http::kResultCap / 4 + 128; ++i) {
    large.append("\xf0\x9f\x8c\x8d", 4);
    assert(std::string(large.bytes).compare(0, previous.size(), previous) == 0);
    previous = large.bytes;
  }
  assert(large.truncated);
  assert(std::strstr(large.bytes, hw1http::kTruncated));
  large.finish(true);
  assert(std::strstr(large.bytes, hw1http::kInterrupted));
  assert(large.size < hw1http::kResultCap);
  assert(std::string(large.bytes).compare(0, previous.size(), previous) == 0);
}

static void longReply() {
  auto stream = std::make_unique<Stream>();
  const std::string expected(12000, 'r');
  for (size_t offset = 0; offset < expected.size(); offset += 4000) {
    const std::string wire = "data: {\"choices\":[{\"delta\":{\"content\":\"" +
        expected.substr(offset, 4000) + "\"}}]}\n\n";
    assert(stream->feed(wire.data(), wire.size()));
    assert(stream->text.size == offset + 4000 && !stream->text.truncated);
  }
  assert(stream->feed("data: [DONE]\n\n", 14));
  assert(stream->done && !stream->error);
  assert(std::string(stream->text.bytes) == expected);
}

static void urls() {
  char out[512];
  assert(hw1http::endpoint("https://agent.lan:18789/v1/", false, out, sizeof(out)));
  assert(std::string(out) == "https://agent.lan:18789/v1/chat/completions");
  assert(hw1http::endpoint("http://192.0.2.1/v1", true, out, sizeof(out)));
  for (auto url : {"http://agent/v1", "ftp://agent/v1", "https:///v1", "https://a/v1?q=x",
                   "https://user:pass@agent/v1", "https://a/v1#part", "https://a/v1\r\nX: value",
                   "https://a/completions", "https://a/../v1"})
    assert(!hw1http::endpoint(url, false, out, sizeof(out)));
  assert(!hw1http::headerSafe("token\r\nInjected: yes"));
}

int main() {
  fragmented(); failedStreams(); utf8AndTruncation(); longReply(); urls();
  std::cout << "HTTP source wire parser tests passed\n";
}
