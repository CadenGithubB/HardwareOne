// The framed command channel of the UART host link, link protocol 2
// (System_UartLinkFraming.h, owner decision 2026-11-21 "Full framing now"):
// the "@<id> " request prefix, the "@<id> ok|err <len>" reply header and the
// whole reply line the link's fixed-buffer replies use, on the production
// header. Each framed reply is read back the way the Pi daemon reads it
// (HardwareOne_RaspPi_CoProcessor link/transport.py): an exact header line,
// then exactly <len> body bytes, then one '\n'.
#include "System_UartLinkFraming.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned checks = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    ++checks;                                                                     \
    if (!(condition)) {                                                           \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);        \
      std::exit(1);                                                               \
    }                                                                             \
  } while (0)

// ---- The host's reader ---------------------------------------------------------
struct HostReply {
  bool parsed = false;
  std::string id;
  bool ok = false;
  std::string body;
  size_t consumed = 0;  // wire bytes the reply took, header to final '\n'
};

static bool isLowerHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Mirrors the daemon: header regex @([0-9a-f]{1,8}) (ok|err) (0|[1-9][0-9]*)
// as the whole line, then <len> bytes, then exactly '\n'.
static HostReply hostRead(const std::string& wire) {
  HostReply r;
  const size_t nl = wire.find('\n');
  if (nl == std::string::npos) return r;
  const std::string header = wire.substr(0, nl);
  if (header.size() < 2 || header[0] != '@') return r;
  size_t i = 1;
  while (i < header.size() && isLowerHex(header[i])) ++i;
  const size_t idLen = i - 1;
  if (idLen == 0 || idLen > 8 || i >= header.size() || header[i] != ' ') return r;
  r.id = header.substr(1, idLen);
  ++i;
  if (header.compare(i, 3, "ok ") == 0) {
    r.ok = true;
    i += 3;
  } else if (header.compare(i, 4, "err ") == 0) {
    r.ok = false;
    i += 4;
  } else {
    return r;
  }
  const std::string len = header.substr(i);
  if (len.empty() || (len.size() > 1 && len[0] == '0')) return r;
  for (char c : len) if (c < '0' || c > '9') return r;
  const size_t bodyLen = std::strtoul(len.c_str(), nullptr, 10);
  if (wire.size() < nl + 1 + bodyLen + 1) return r;
  if (wire[nl + 1 + bodyLen] != '\n') return r;
  r.body = wire.substr(nl + 1, bodyLen);
  r.consumed = nl + 1 + bodyLen + 1;
  r.parsed = true;
  return r;
}

// ---- Request prefix ------------------------------------------------------------
static size_t prefix(const char* line, char* id) {
  return uartFramedRequestPrefix(line, std::strlen(line), id);
}

static void testRequestPrefix() {
  char id[kUartFrameIdStorage];

  // Valid ids: 1-8 lowercase hex digits, one space, then the command.
  CHECK(prefix("@1 help", id) == 3 && std::strcmp(id, "1") == 0);
  CHECK(prefix("@ffffffff login a b", id) == 10 && std::strcmp(id, "ffffffff") == 0);
  CHECK(prefix("@0 x", id) == 3 && std::strcmp(id, "0") == 0);
  CHECK(prefix("@00a1 x", id) == 6 && std::strcmp(id, "00a1") == 0);  // echoed verbatim
  CHECK(prefix("@9 cm5 heartbeat 1 4 ready", id) == 3 && std::strcmp(id, "9") == 0);
  // The command is the rest, byte for byte (the link trims it afterwards).
  {
    const char* line = "@2a  sensors  ";
    const size_t n = prefix(line, id);
    CHECK(n == 4 && std::strcmp(id, "2a") == 0 && std::strcmp(line + n, " sensors  ") == 0);
  }
  // A framed empty command: the prefix is valid, nothing follows it.
  CHECK(prefix("@5 ", id) == 3 && std::strcmp(id, "5") == 0);

  // Invalid: the line stays unframed and the id empty.
  const char* invalid[] = {
      "help",            // no prefix
      "",                // empty line
      "@",               // no id
      "@ help",          // no digit
      "@1",              // no space
      "@1help",          // no space before the command
      "@1\thelp",        // a tab is not the space
      "@ABC help",       // uppercase
      "@1F help",        // mixed case
      "@g1 help",        // not hex
      "@12x help",       // a non-hex digit inside the id
      "@123456789 help", // nine digits: too long
      " @1 help",        // anything before the '@'
      "#1 help",         // another lead
      "@-1 help",        // a sign
  };
  for (const char* line : invalid) {
    std::strcpy(id, "junk");
    CHECK(prefix(line, id) == 0);
    CHECK(id[0] == '\0');
  }
  // Only the first `len` bytes count.
  CHECK(uartFramedRequestPrefix("@12 x", 3, id) == 0);
  CHECK(uartFramedRequestPrefix("@12 x", 4, id) == 4 && std::strcmp(id, "12") == 0);
  CHECK(uartFramedRequestPrefix(nullptr, 5, id) == 0 && id[0] == '\0');
  // idOut may be null when only the length matters.
  CHECK(uartFramedRequestPrefix("@abc x", 6, nullptr) == 5);
  // A line of exactly eight digits and a space at the cap edge.
  CHECK(uartFramedRequestPrefix("@12345678 ", 10, id) == 10 && std::strcmp(id, "12345678") == 0);
}

// ---- Reply header --------------------------------------------------------------
static void testHeader() {
  char h[kUartFrameHeaderMax + 1];
  CHECK(uartFrameHeader(h, sizeof(h), "1", true, 0) == 8 && std::strcmp(h, "@1 ok 0\n") == 0);
  CHECK(uartFrameHeader(h, sizeof(h), "a", false, 4095) == 12 && std::strcmp(h, "@a err 4095\n") == 0);
  CHECK(uartFrameHeader(h, sizeof(h), "00a1", true, 17) == 12 && std::strcmp(h, "@00a1 ok 17\n") == 0);
  // The longest header fits the declared bound exactly.
  CHECK(uartFrameHeader(h, sizeof(h), "ffffffff", false, 4294967295UL) == kUartFrameHeaderMax);
  CHECK(std::strcmp(h, "@ffffffff err 4294967295\n") == 0);
  // The flag decides ok/err, never the text the body carries.
  CHECK(uartFrameHeader(h, sizeof(h), "7", false, 2) && std::strcmp(h, "@7 err 2\n") == 0);
  CHECK(uartFrameHeader(h, sizeof(h), "7", true, 2) && std::strcmp(h, "@7 ok 2\n") == 0);
  // Invalid ids and a buffer too small are refused, never cut.
  CHECK(uartFrameHeader(h, sizeof(h), "", true, 1) == 0 && h[0] == '\0');
  CHECK(uartFrameHeader(h, sizeof(h), nullptr, true, 1) == 0 && h[0] == '\0');
  CHECK(uartFrameHeader(h, sizeof(h), "123456789", true, 1) == 0);
  CHECK(uartFrameHeader(h, sizeof(h), "AB", true, 1) == 0);
  CHECK(uartFrameHeader(h, 8, "1", true, 0) == 0 && h[0] == '\0');  // needs 9 with the NUL
  CHECK(uartFrameHeader(h, 9, "1", true, 0) == 8);
}

// ---- Whole reply lines ---------------------------------------------------------
static std::string compose(const char* id, bool ok, const char* body, size_t cap = 512) {
  std::string buf(cap, '\0');
  const size_t n = uartFrameComposeLine(&buf[0], cap, id, ok, body);
  CHECK(n < cap && buf[n] == '\0');
  buf.resize(n);
  return buf;
}

static void testComposeLine() {
  // Unframed lines are unchanged: the text and its newline, nothing more.
  CHECK(compose(nullptr, true, "OK: logged out") == "OK: logged out\n");
  CHECK(compose("", false, "Error: authentication failed") == "Error: authentication failed\n");
  CHECK(compose(nullptr, false, "") == "\n");

  // Framed: header, exact body length, body, newline; the host reads it back.
  {
    const std::string w = compose("1f", true, "OK: logged in as admin (admin) link=2");
    CHECK(w == "@1f ok 37\nOK: logged in as admin (admin) link=2\n");
    const HostReply r = hostRead(w);
    CHECK(r.parsed && r.id == "1f" && r.ok && r.body == "OK: logged in as admin (admin) link=2");
    CHECK(r.consumed == w.size());
  }
  // A multi-line body: <len> counts its newlines; the reader takes them verbatim.
  {
    const char* body = "line one\nline two\n\nline four";
    const std::string w = compose("2", true, body);
    CHECK(w == std::string("@2 ok 28\n") + body + "\n");
    const HostReply r = hostRead(w);
    CHECK(r.parsed && r.body == body && r.consumed == w.size());
  }
  // An empty body: length 0, then the one newline.
  {
    const std::string w = compose("3", true, "");
    CHECK(w == "@3 ok 0\n\n");
    const HostReply r = hostRead(w);
    CHECK(r.parsed && r.body.empty() && r.consumed == w.size());
  }
  // UTF-8: the length counts bytes, not characters.
  {
    const char* body = "Error: line too long \xE2\x80\x94 discarded";
    const std::string w = compose("4", false, body);
    CHECK(w == std::string("@4 err ") + std::to_string(std::strlen(body)) + "\n" + body + "\n");
    CHECK(std::strlen(body) == 34);
    const HostReply r = hostRead(w);
    CHECK(r.parsed && !r.ok && r.body == body);
  }
  // The flag decides, not the text: an "OK" body refused, an "Error" body passed.
  CHECK(hostRead(compose("5", false, "OK: but refused")).ok == false);
  CHECK(hostRead(compose("5", true, "Error: but the producer said ok")).ok == true);
  // Intrinsic replies, as the link writes them for a framed request.
  {
    const HostReply locked = hostRead(compose("6", false, "Error: login locked out. Retry in 30 seconds."));
    CHECK(locked.parsed && locked.id == "6" && !locked.ok &&
          locked.body == "Error: login locked out. Retry in 30 seconds.");
    const HostReply nag = hostRead(compose("7", false, "Error: authentication required. Use: login <username> <password>"));
    CHECK(nag.parsed && !nag.ok && nag.body.find("authentication required") != std::string::npos);
    const HostReply idle = hostRead(compose("8", false, "[uart] Signed out due to inactivity. Please log in again."));
    CHECK(idle.parsed && !idle.ok);
    const HostReply hb = hostRead(compose("9", true, "OK: cm5 heartbeat 1 4 accepted"));
    CHECK(hb.parsed && hb.ok && hb.id == "9");
  }
  // Two replies back to back: each header says where the next one starts.
  {
    const std::string w = compose("a", true, "first\nreply") + compose("b", false, "second");
    const HostReply one = hostRead(w);
    CHECK(one.parsed && one.id == "a" && one.body == "first\nreply");
    const HostReply two = hostRead(w.substr(one.consumed));
    CHECK(two.parsed && two.id == "b" && two.body == "second" && one.consumed + two.consumed == w.size());
  }
  // A body too long for the buffer is cut and the header counts what was kept.
  {
    std::string longBody(300, 'x');
    const size_t cap = 64;
    const std::string w = compose("ffffffff", true, longBody.c_str(), cap);
    CHECK(w.size() <= cap - 1 && w.size() > 32);  // filled, with its NUL after it
    const HostReply r = hostRead(w);
    CHECK(r.parsed && r.consumed == w.size() && r.body == std::string(r.body.size(), 'x'));
    // A cut that changes the length's digit count still adds up.
    const std::string w2 = compose("1", true, std::string(1000, 'y').c_str(), 106);
    const HostReply r2 = hostRead(w2);
    CHECK(r2.parsed && r2.consumed == w2.size() && w2.size() <= 105);
    CHECK(w2.compare(0, 9, "@1 ok 93\n") == 0);
    // Unframed lines are cut the same way, newline kept.
    const std::string u = compose(nullptr, true, longBody.c_str(), 16);
    CHECK(u == std::string(14, 'x') + "\n");
  }
  // Too small for even an empty framed reply, or an invalid id: nothing.
  {
    char tiny[9];
    CHECK(uartFrameComposeLine(tiny, sizeof(tiny), "1", true, "") == 0 && tiny[0] == '\0');
    char ten[10];
    CHECK(uartFrameComposeLine(ten, sizeof(ten), "1", true, "") == 9);
    CHECK(std::strcmp(ten, "@1 ok 0\n\n") == 0);
    char buf[64];
    CHECK(uartFrameComposeLine(buf, sizeof(buf), "XYZ", true, "OK") == 0);
    CHECK(uartFrameComposeLine(buf, 1, nullptr, true, "OK") == 0);
    CHECK(uartFrameComposeLine(buf, sizeof(buf), "1", true, nullptr) == 9);
  }
  CHECK(kUartLinkProtocolVersion == 2);
}

int main() {
  testRequestPrefix();
  testHeader();
  testComposeLine();
  std::printf("uart_link_framing_tests: %u checks passed\n", checks);
  return 0;
}
