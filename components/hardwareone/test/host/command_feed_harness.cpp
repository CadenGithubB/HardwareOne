// Executes the shipping originPrefix (forward declaration and definition) and
// appendCommandToFeed from HardwareOne.cpp. The CM5/live-audio command
// classifiers, audit redaction and the web mirror are recorded boundaries.
#include "Arduino.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

#define ENABLE_HTTP_SERVER 1
bool cm5PresenceIsProtocolCommand(const char* line) { return std::strcmp(line, "cm5 status") == 0; }
bool liveAudioIsHousekeepingCommand(const char* line) { return std::strcmp(line, "liveaudio poll") == 0; }
String redactCmdForAudit(const String& argsInput) { return String(std::string("R(") + argsInput.c_str() + ")"); }
struct HostMirror {
  char* buf = nullptr;
  std::vector<std::string> lines;
  std::vector<bool> newlines;
  void appendDirect(const char* s, size_t slen, bool needNewline) {
    lines.emplace_back(s, slen);
    newlines.push_back(needNewline);
  }
} gWebMirror;

// INSERT_PRODUCTION

static std::string reply(const char* source, const std::string& user, const std::string& ip) {
  return originPrefix(source, String(user), String(ip)).c_str();
}
static std::vector<std::string> feed(const char* source, const char* cmd, const std::string& user,
                                     const std::string& ip) {
  gWebMirror.lines.clear();
  gWebMirror.newlines.clear();
  appendCommandToFeed(source, String(cmd), String(user), String(ip));
  for (bool newline : gWebMirror.newlines) CHECK(newline);
  return gWebMirror.lines;
}

int main() {
  static char mirror[16];
  gWebMirror.buf = mirror;

  // Reply prefix and feed line share one origin format; the feed adds "$ ".
  CHECK(reply("web", "alice", "10.0.0.2") == "[web alice@10.0.0.2] ");
  CHECK(feed("web", "status", "alice", "10.0.0.2") == std::vector<std::string>{"[web alice@10.0.0.2] $ R(status)"});
  CHECK(reply("serial", "", "") == "[serial] ");
  CHECK(feed("serial", "help", "", "") == std::vector<std::string>{"[serial] $ R(help)"});
  gWebMirror.lines.clear();
  appendCommandToFeed("serial", String("help"));  // default user/ip
  CHECK(gWebMirror.lines == std::vector<std::string>{"[serial] $ R(help)"});
  CHECK(reply("bluetooth", "bob", "") == "[bluetooth bob] ");
  CHECK(feed("bluetooth", "x", "bob", "") == std::vector<std::string>{"[bluetooth bob] $ R(x)"});
  CHECK(reply("web", "", "10.0.0.9") == "[web @10.0.0.9] ");
  CHECK(feed("web", "x", "", "10.0.0.9") == std::vector<std::string>{"[web @10.0.0.9] $ R(x)"});

  // Long identities: both callers truncate inside the same 128-byte buffer,
  // so the marker is cut with the prefix and is never appended after it.
  for (size_t length = 90; length <= 200; ++length) {
    const std::string user(length, 'u');
    const std::string untruncated = "[web " + user + "@10.0.0.2] ";
    const std::string prefix = reply("web", user, "10.0.0.2");
    CHECK(prefix == untruncated.substr(0, 127));
    const auto lines = feed("web", "status", user, "10.0.0.2");
    CHECK(lines.size() == 1);
    CHECK(lines[0] == (untruncated + "$ ").substr(0, 127) + "R(status)");
    if (length == 200) CHECK(lines[0] == prefix + "R(status)");
  }

  // Machine polls never enter the human feed; no mirror means no write.
  CHECK(feed("uart", "cm5 status", "", "").empty());
  CHECK(feed("web", "liveaudio poll", "alice", "10.0.0.2").empty());
  gWebMirror.buf = nullptr;
  CHECK(feed("web", "status", "alice", "10.0.0.2").empty());

  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("COMMAND_FEED PASS");
  return 0;
}
