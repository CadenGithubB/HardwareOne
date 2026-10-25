// Runs BLE_LinkStatusCore.h as shipped. Pipe labels are inserted by
// test_ble_link_status.py from BLE_PhonePipe.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "BLE_LinkStatusCore.h"
#include "BLE_PhonePipeCore.h"  // the slot classes and error codes the block names

using namespace hw1link;
static unsigned checks = 0;
#define CHECK(c)                                                                \
  do {                                                                          \
    ++checks;                                                                   \
    if (!(c)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)
static bool eq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// INSERT_LABELS

static void roles() {
  // Every combination of the four live flags.
  for (int m = 0; m < 16; ++m) {
    RoleInputs in;
    in.server = m & 1;
    in.glasses = m & 2;
    in.pipe = m & 4;
    in.pipeOwnsHost = m & 8;
    const Role r = roleOf(in);
    if (in.server) CHECK(r == Role::Console);  // the server wins: it excludes the others
    else if (in.pipe && (in.pipeOwnsHost || !in.glasses)) CHECK(r == Role::PhonePipe);
    else if (in.glasses) CHECK(r == (in.pipe ? Role::GlassesPipe : Role::Glasses));
    else CHECK(r == Role::Off);
  }
  CHECK(roleOfMode(0, true) == Role::Console && roleOfMode(1, true) == Role::GlassesPipe &&
        roleOfMode(1, false) == Role::Glasses && roleOfMode(2, true) == Role::Off &&  // the phone-client mode is gone
        roleOfMode(7, true) == Role::Off);
  CHECK(eq(roleText(Role::Console), "App console") && eq(roleText(Role::GlassesPipe), "Glasses+pipe") &&
        eq(roleText(Role::PhonePipe), "Phone pipe") && eq(roleText(Role::Glasses), "Glasses") &&
        eq(roleText(Role::Off), "Off"));
  CHECK(eq(roleId(Role::Console), "console") && eq(roleId(Role::GlassesPipe), "glasses-pipe") &&
        eq(roleId(Role::PhonePipe), "phone-pipe") && eq(roleId(Role::Off), "off"));
  // Glasses with or without the pipe is the same saved mode; Off always shows it.
  CHECK(!savedDiffers(Role::GlassesPipe, Role::Glasses) && !savedDiffers(Role::Glasses, Role::GlassesPipe));
  CHECK(!savedDiffers(Role::Console, Role::Console) && !savedDiffers(Role::PhonePipe, Role::PhonePipe));
  CHECK(savedDiffers(Role::PhonePipe, Role::Console) && savedDiffers(Role::Off, Role::Off) &&
        !savedDiffers(Role::Glasses, Role::PhonePipe));  // one client family
  char buf[48];
  CHECK(eq(roleLine(Role::GlassesPipe, Role::GlassesPipe, buf, sizeof(buf)), "Glasses+pipe"));
  CHECK(eq(roleLine(Role::PhonePipe, Role::Console, buf, sizeof(buf)), "Phone pipe (saved: App console)"));
  CHECK(eq(roleLine(Role::Off, Role::Glasses, buf, sizeof(buf)), "Off (saved: Glasses)"));
  char tiny[6];
  CHECK(eq(roleLine(Role::PhonePipe, Role::Console, tiny, sizeof(tiny)), "Phone"));  // bounded
  CHECK(eq(roleLine(Role::Off, Role::Off, nullptr, 0), ""));
}

static PipeView pipeAt(PipeStep step, const char* label, bool active = true, bool enrolled = true,
                       bool enrolling = false, bool ready = false) {
  PipeInputs in;
  in.built = true;
  in.active = active;
  in.ready = ready;
  in.enrolling = enrolling;
  in.enrolled = enrolled;
  in.step = step;
  in.label = label;
  return pipeOf(in);
}

static void pipes() {
  PipeInputs none;
  CHECK(pipeOf(none).phase == Pipe::NotBuilt);
  // Ready wins over everything else.
  CHECK(pipeAt(PipeStep::Link, "phone link lost", true, false, true, true).phase == Pipe::Ready);
  // Not running: off, or no secret.
  CHECK(pipeAt(PipeStep::Idle, "stopped", false).phase == Pipe::Off);
  CHECK(pipeAt(PipeStep::Idle, "phone not found", false).phase == Pipe::Off);
  CHECK(pipeAt(PipeStep::Idle, "configure phone secret first", false).phase == Pipe::NoSecret);
  // Running: the step decides; pairing covers the whole enroll attempt.
  CHECK(pipeAt(PipeStep::Waiting, "waiting for radio").phase == Pipe::Waiting);
  CHECK(pipeAt(PipeStep::ScanParams, "scanning").phase == Pipe::Searching);
  CHECK(pipeAt(PipeStep::Scanning, "scanning").phase == Pipe::Searching);
  CHECK(pipeAt(PipeStep::Link, "authenticating pipe").phase == Pipe::Linking);
  for (PipeStep st : {PipeStep::Waiting, PipeStep::ScanParams, PipeStep::Scanning, PipeStep::Link})
    CHECK(pipeAt(st, "waiting to enroll", true, false, true).phase == Pipe::Pairing);
  // Closing and fault keep their reason.
  PipeView v = pipeAt(PipeStep::Retiring, "phone not found");
  CHECK(v.phase == Pipe::Closing && eq(v.reason, "not found"));
  v = pipeAt(PipeStep::Retiring, "phone close requested");
  CHECK(v.phase == Pipe::Closing && !v.reason);
  v = pipeAt(PipeStep::Fault, "phone cleanup incomplete");
  CHECK(v.phase == Pipe::Fault && eq(v.reason, "cleanup"));
  {
    char line[24];
    CHECK(eq(pipeLine(v, line, sizeof(line)), "fault"));  // the reason stays in JSON
  }
  // Idle: not paired, failed with a reason, or idle.
  CHECK(pipeAt(PipeStep::Idle, "disconnected", true, false).phase == Pipe::NotPaired);
  v = pipeAt(PipeStep::Idle, "BLE setup timed out");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "timed out"));
  v = pipeAt(PipeStep::Idle, "disconnected");
  CHECK(v.phase == Pipe::Idle && !v.reason);
  v = pipeAt(PipeStep::Idle, nullptr);
  CHECK(v.phase == Pipe::Idle && !v.reason);
  // `phone pair new`'s window (phone link v2) reads as enrolling: pairing
  // between its attempts too, whatever the last attempt left as the label;
  // once it ends, its own short reason.
  for (const char* label : {"phone not found", "disconnected", "BLE setup timed out", "pairing code closed"})
    CHECK(pipeAt(PipeStep::Idle, label, true, false, true).phase == Pipe::Pairing);
  v = pipeAt(PipeStep::Idle, "pairing code expired");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "expired"));
  v = pipeAt(PipeStep::Idle, "pairing code closed");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "cancelled"));
  CHECK(pipeAt(PipeStep::Idle, "pairing code expired", true, false).phase == Pipe::NotPaired);
  // Version mismatch (phone link v2): a strike, then the hold, each with its
  // own short reason.
  v = pipeAt(PipeStep::Idle, "phone app version mismatch");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "version"));
  v = pipeAt(PipeStep::Idle, "phone app too old");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "app old"));
  {
    char line[32];
    CHECK(eq(pipeLine(v, line, sizeof(line)), "fail: app old"));
  }
  // A label nobody classified never reads as success.
  v = pipeAt(PipeStep::Idle, "some new failure");
  CHECK(v.phase == Pipe::Failed && eq(v.reason, "error"));
  // Words.
  char buf[32];
  CHECK(eq(pipeLine(pipeAt(PipeStep::Idle, "phone not found"), buf, sizeof(buf)), "fail: not found"));
  CHECK(eq(pipeLine(pipeAt(PipeStep::Idle, "x", true, true, false, true), buf, sizeof(buf)), "ready"));
  for (int p = 0; p <= int(Pipe::Fault); ++p) {
    CHECK(std::strlen(pipeBrief(Pipe(p))) <= 6 && std::strlen(pipeText(Pipe(p))) <= 12);
    CHECK(std::strlen(pipeId(Pipe(p))) > 0 && std::strchr(pipeId(Pipe(p)), ' ') == nullptr);
  }
  CHECK(eq(pipeBrief(Pipe::Ready), "ready") && eq(pipeBrief(Pipe::Linking), "link") &&
        eq(pipeBrief(Pipe::Searching), "scan") && eq(pipeBrief(Pipe::Off), "off"));
  // Every label BLE_PhonePipe.cpp can set is classified: neutral, or a reason
  // other than "error". The longest line still fits the OLED's 21 columns
  // after "Pipe: ".
  for (const char* label : kPipeLabels) {
    const char* reason = pipeReason(label);
    CHECK(pipeLabelNeutral(label) == (reason == nullptr));
    CHECK(!reason || !eq(reason, "error"));
    if (!reason) continue;
    char line[32];
    CHECK(std::strlen(reason) <= 9);
    pipeLine(PipeView{Pipe::Failed, reason}, line, sizeof(line));
    CHECK(std::strlen(line) + 6 <= 21);  // "Pipe: fail: not found"
    CHECK(eq(pipeLine(PipeView{Pipe::Closing, reason}, line, sizeof(line)), "closing"));
  }
}

static void consoles() {
  // Every combination of the server, client-host and stream inputs, in every role.
  for (int m = 0; m < (1 << 12) * 5; ++m) {
    ConsoleInputs in;
    in.up = m & 1;
    in.advertising = m & 2;
    in.clients = (m & 4) ? 1 : 0;
    in.signedIn = m & 8;
    in.registered = m & 16;
    in.attachable = m & 32;
    in.attached = m & 64;
    in.pipeReady = m & 128;
    in.secret = m & 256;
    in.streamOffered = m & 512;
    in.streamOpen = m & 1024;
    in.streamSignedIn = m & 2048;
    in.role = Role(m >> 12);
    const bool pipeRole = in.role == Role::GlassesPipe || in.role == Role::PhonePipe;
    const Console c = consoleOf(in);
    if (in.up && in.clients) CHECK(c == (in.signedIn ? Console::SignedIn : Console::Connected));
    else if (in.up) CHECK(c == (in.advertising ? Console::Waiting : Console::Hidden));
    else if (in.role == Role::Off || in.role == Role::Console) CHECK(c == Console::Off);
    // The pipe's console stream first: open on a ready pipe is the console.
    else if (pipeRole && in.streamOpen && in.pipeReady)
      CHECK(c == (in.streamSignedIn ? Console::SignedIn : Console::Connected));
    // A design-A slot the app joined keeps working beside the stream.
    else if (in.attachable && in.attached && in.pipeReady)
      CHECK(c == (in.signedIn ? Console::SignedIn : Console::Connected));
    // Offered, not open: it waits for the phone; design A's words never show.
    else if (pipeRole && in.streamOffered) CHECK(c == Console::WaitingPhone);
    // The phone pipe's own host has no other console.
    else if (in.role == Role::PhonePipe) CHECK(c == Console::NotHere);
    // Design A: the console rides the phone pipe's link where registered.
    else if (!in.attachable) CHECK(c == (in.registered ? Console::Off : Console::NotHere));
    else CHECK(c == (in.secret ? Console::WaitingPhone : Console::NoSecret));
    // The advertising words are the server's alone; the phone-link words a client role's.
    if (c == Console::Waiting || c == Console::Hidden) CHECK(in.up);
    if (c == Console::WaitingPhone) CHECK(!in.up && in.role != Role::Off && in.role != Role::Console &&
                                          ((pipeRole && in.streamOffered) || in.attachable));
    if (c == Console::NoSecret) CHECK(!in.up && in.attachable && !in.secret && in.role != Role::PhonePipe &&
                                      !(pipeRole && in.streamOffered));
    // A stream not offered never says blesecret; one offered never reads "not here".
    if (pipeRole && in.streamOffered) CHECK(c != Console::NoSecret && c != Console::NotHere);
    // Nothing that is not open on a ready pipe reads as an app.
    if (!in.up && (c == Console::Connected || c == Console::SignedIn))
      CHECK(in.pipeReady && ((pipeRole && in.streamOpen) || (in.attachable && in.attached)));
    // A stream open on a dropped pipe is not connected (the driver reports Off first).
    if (!in.pipeReady && !in.up) CHECK(c != Console::Connected && c != Console::SignedIn);
  }
  // Nothing wired on the client host (the defaults) keeps the words of before;
  // the glasses without a pipe build are never registered: n/a here.
  for (int r = 0; r <= int(Role::PhonePipe); ++r) {
    ConsoleInputs in;
    in.role = Role(r);
    CHECK(consoleOf(in) == (in.role == Role::Off || in.role == Role::Console ? Console::Off : Console::NotHere));
    in.attached = in.pipeReady = in.secret = in.signedIn = true;
    if (r != int(Role::Off) && r != int(Role::Console)) CHECK(consoleOf(in) == Console::NotHere);
  }
  {
    ConsoleInputs in;
    in.role = Role::GlassesPipe;
    in.registered = true;
    CHECK(consoleOf(in) == Console::Off);              // registered, setting off
    in.attachable = true;
    CHECK(consoleOf(in) == Console::NoSecret);         // Secure Channel only
    in.secret = true;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // the pipe is not ready
    in.pipeReady = true;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // ready, the slot not made yet
    in.attached = true;
    CHECK(consoleOf(in) == Console::Connected);
    in.signedIn = true;
    CHECK(consoleOf(in) == Console::SignedIn);
    in.pipeReady = false;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // the pipe went down first
  }
  // Phone link v2: the console stream, in both pipe roles.
  for (Role role : {Role::GlassesPipe, Role::PhonePipe}) {
    ConsoleInputs in;
    in.role = role;
    in.registered = in.attachable = role == Role::GlassesPipe;  // design A beside the glasses
    // Not offered (no device code): the phone pipe's host has nothing else;
    // beside the glasses design A's words (no blesecret).
    CHECK(consoleOf(in) == (role == Role::PhonePipe ? Console::NotHere : Console::NoSecret));
    in.streamOffered = true;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // offered, the link not ready
    in.pipeReady = true;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // ready, the app has not opened it
    in.streamOpen = true;
    CHECK(consoleOf(in) == Console::Connected);        // open, signed out
    in.streamSignedIn = true;
    CHECK(consoleOf(in) == Console::SignedIn);
    in.pipeReady = false;
    CHECK(consoleOf(in) == Console::WaitingPhone);     // the link went down first
    // Open but no longer offered (bleconsole off while it drains) still shows open.
    in.pipeReady = true; in.streamOffered = false;
    CHECK(consoleOf(in) == Console::SignedIn);
    // A design-A slot the app joined, beside an offered stream it did not open.
    in.streamOpen = in.streamSignedIn = false; in.streamOffered = true;
    in.attached = in.secret = true; in.signedIn = false;
    CHECK(consoleOf(in) == (role == Role::GlassesPipe ? Console::Connected : Console::WaitingPhone));
  }
  // The glasses alone (the pipe not running): a stale offered stream is ignored.
  {
    ConsoleInputs in;
    in.role = Role::Glasses;
    in.streamOffered = in.streamOpen = in.streamSignedIn = in.pipeReady = true;
    CHECK(consoleOf(in) == Console::NotHere);
  }
  CHECK(eq(consoleText(Console::NotHere), "n/a here") && eq(consoleLongText(Console::NotHere), "not available in this mode"));
  CHECK(eq(consoleText(Console::SignedIn), "signed in") && eq(consoleId(Console::SignedIn), "signed-in"));
  CHECK(eq(consoleText(Console::Waiting), "waiting") && eq(consoleLongText(Console::Waiting), "waiting for the app") &&
        eq(consoleId(Console::Waiting), "waiting"));
  CHECK(eq(consoleLongText(Console::Hidden), "not visible (advertising off)") &&
        eq(consoleLongText(Console::Connected), "app connected, signed out") &&
        eq(consoleText(Console::Connected), "signed out") && eq(consoleId(Console::Connected), "signed-out") && eq(consoleLongText(Console::SignedIn), "app signed in"));
  CHECK(eq(consoleText(Console::WaitingPhone), "waiting") &&
        eq(consoleLongText(Console::WaitingPhone), "waiting for the phone link") &&
        eq(consoleId(Console::WaitingPhone), "waiting-phone"));
  CHECK(eq(consoleText(Console::NoSecret), "no secret") && eq(consoleLongText(Console::NoSecret), "set blesecret first") &&
        eq(consoleId(Console::NoSecret), "no-secret"));
  for (int c = 0; c <= int(Console::NoSecret); ++c) {
    CHECK(std::strlen(consoleText(Console(c))) + 9 <= 21);  // "Console: " on the OLED
    CHECK(std::strchr(consoleId(Console(c)), ' ') == nullptr);
    for (int d = 0; d < c; ++d) CHECK(!eq(consoleId(Console(c)), consoleId(Console(d))));  // JSON tells them apart
  }
}

// The phone link block: the stream console words, relay, end reasons, lines.
static void phoneLink() {
  // The block's slot counts and reason table are the pipe's own.
  CHECK(kRelayStreams == hw1pipe::kRelaySlots &&
        kRelayStreams + kConsoleStreams == hw1pipe::kMaxStreams);
  CHECK(kEndReasons == unsigned(hw1pipe::Error::Paused) + 1);
  // Every pipe error code has its own name; 16-18 are phone link v2's.
  const char* const names[] = {"other", "protocol", "version", "unauthorized", "target", "vpn", "dns",
                               "connect", "connect-timeout", "busy", "resources", "tcp", "flow",
                               "deadline", "cancelled", "display", "service-off", "unsupported", "paused"};
  for (unsigned i = 0; i < kEndReasons; ++i) {
    CHECK(eq(endReasonId(uint16_t(i)), names[i]));
    CHECK(std::strchr(endReasonId(uint16_t(i)), ' ') == nullptr);
    for (unsigned j = 0; j < i; ++j) CHECK(!eq(endReasonId(uint16_t(i)), endReasonId(uint16_t(j))));
  }
  CHECK(eq(endReasonId(uint16_t(hw1pipe::Error::ServiceOff)), "service-off") &&
        eq(endReasonId(uint16_t(hw1pipe::Error::Unsupported)), "unsupported") &&
        eq(endReasonId(uint16_t(hw1pipe::Error::Paused)), "paused"));
  CHECK(eq(endReasonId(19), "other") && eq(endReasonId(0xffff), "other"));  // never folded onto a known one

  // The console stream: the driver's state while READY, else why it is not offered.
  for (int st = 0; st <= int(StreamState::Stopped); ++st) {
    for (int m = 0; m < 8; ++m) {
      PhoneLinkInputs in;
      in.stream = StreamState(st);
      in.consoleEnabled = m & 1;
      in.consoleOffered = m & 2;
      in.streamSignedIn = m & 4;
      const LinkConsole c = linkConsoleOf(in);
      switch (in.stream) {
        case StreamState::Open: CHECK(c == (in.streamSignedIn ? LinkConsole::SignedIn : LinkConsole::Open)); break;
        case StreamState::Opening: CHECK(c == LinkConsole::Opening); break;
        case StreamState::Closing: CHECK(c == LinkConsole::Closing); break;
        case StreamState::Backoff: CHECK(c == LinkConsole::Backoff); break;
        case StreamState::Stopped: CHECK(c == LinkConsole::Stopped); break;
        case StreamState::Waiting: CHECK(c == LinkConsole::NotWanted); break;
        case StreamState::Off:
        case StreamState::NotOffered:
          CHECK(c == (!in.consoleOffered ? (in.consoleEnabled ? LinkConsole::NeedsCode : LinkConsole::Off)
                                         : LinkConsole::Waiting));
          break;
      }
    }
  }
  for (int c = 0; c <= int(LinkConsole::Stopped); ++c) {
    CHECK(std::strchr(linkConsoleId(LinkConsole(c)), ' ') == nullptr);
    CHECK(std::strlen(linkConsoleText(LinkConsole(c))) <= 21);
    for (int d = 0; d < c; ++d) {
      CHECK(!eq(linkConsoleId(LinkConsole(c)), linkConsoleId(LinkConsole(d))));
      CHECK(!eq(linkConsoleText(LinkConsole(c)), linkConsoleText(LinkConsole(d))));
    }
  }
  CHECK(eq(linkConsoleId(LinkConsole::NeedsCode), "needs-code") &&
        eq(linkConsoleText(LinkConsole::NeedsCode), "needs a device code") &&
        eq(linkConsoleId(LinkConsole::SignedIn), "signed-in") &&
        eq(linkConsoleText(LinkConsole::Open), "open, signed out") &&
        eq(linkConsoleId(LinkConsole::NotWanted), "not-wanted") &&
        eq(linkConsoleId(LinkConsole::Backoff), "backoff") && eq(linkConsoleId(LinkConsole::Off), "off"));
  CHECK(eq(secretId(Secret::Code), "code") && eq(secretId(Secret::Passphrase), "passphrase") &&
        eq(secretId(Secret::None), "none") && eq(secretText(Secret::Code), "device code") &&
        eq(secretText(Secret::None), "no secret"));

  // Relay: what the phone reports, unknown until READY.
  PhoneLinkInputs in;
  CHECK(eq(relayId(in), "unknown"));
  in.relayOn = true;
  CHECK(eq(relayId(in), "unknown"));
  in.ready = true;
  CHECK(eq(relayId(in), "on"));
  in.relayOn = false;
  CHECK(eq(relayId(in), "off"));

  // The text block.
  char buf[1024];
  PhoneLinkInputs ready;
  ready.ready = true; ready.consoleEnabled = ready.consoleOffered = true;
  ready.stream = StreamState::Open; ready.streamSignedIn = true; ready.secret = Secret::Code;
  ready.consoleStreams = 1; ready.ended = 3; ready.lastEnd = 18;
  uint32_t ends[kEndReasons] = {};
  ends[14] = 2; ends[18] = 1;
  int n = phoneLinkLines(buf, sizeof(buf), PipeView{Pipe::Ready, nullptr}, true, ready, ends);
  CHECK(n == int(std::strlen(buf)));
  CHECK(eq(buf, "Phone link: ready\n  Paired: yes (device code)\n  Console: signed in\n  Relay: off\n"
                "  Streams: relay 0/2, console 1/1\n  Ended: 3 (last: paused)\n"
                "  End reasons: cancelled=2 paused=1"));
  // A failed search, no end counts given, nothing ended: no End reasons line.
  PhoneLinkInputs idle;
  idle.consoleEnabled = true; idle.secret = Secret::Passphrase;
  phoneLinkLines(buf, sizeof(buf), PipeView{Pipe::Failed, "not found"}, true, idle, nullptr);
  CHECK(eq(buf, "Phone link: fail: not found\n  Paired: yes (passphrase)\n  Console: needs a device code\n"
                "  Relay: unknown\n  Streams: relay 0/2, console 0/1\n  Ended: 0"));
  // An unknown code ends in "other"; zero counts are not listed.
  idle.ended = 1; idle.lastEnd = 40; idle.secret = Secret::None; idle.consoleEnabled = false;
  uint32_t other[kEndReasons] = {};
  other[0] = 1;
  phoneLinkLines(buf, sizeof(buf), PipeView{Pipe::Off, nullptr}, false, idle, other);
  CHECK(eq(buf, "Phone link: off\n  Paired: no (no secret)\n  Console: off\n  Relay: unknown\n"
                "  Streams: relay 0/2, console 0/1\n  Ended: 1 (last: other)\n  End reasons: other=1"));
  // Every count of every reason, at its largest, stays bounded: a short buffer
  // holds a prefix of the full text and its terminator.
  uint32_t all[kEndReasons];
  for (uint32_t& v : all) v = 0xffffffffu;
  ready.ended = 0xffffffffu; ready.relayStreams = 2;
  // The worst case (every reason at its largest count) stays well inside the
  // 1024-byte slices bleinfo and blestatus write it into, beside their lines.
  const int full = phoneLinkLines(buf, sizeof(buf), PipeView{Pipe::Failed, "link lost"}, true, ready, all);
  CHECK(full == int(std::strlen(buf)) && full <= 640);
  std::printf("BLE link status: phone link block worst case %d bytes\n", full);
  const std::string whole(buf);
  for (size_t size : {size_t(1), size_t(2), size_t(17), size_t(64), size_t(200), whole.size(), whole.size() + 1}) {
    char small[700];
    std::memset(small, 'x', sizeof(small));
    const int m = phoneLinkLines(small, size, PipeView{Pipe::Failed, "link lost"}, true, ready, all);
    CHECK(m == int(std::strlen(small)) && size_t(m) <= size - 1 && small[size] == 'x');
    CHECK(whole.compare(0, size_t(m), small) == 0);
  }
  CHECK(phoneLinkLines(nullptr, 8, PipeView{}, false, ready, all) == 0);
}

int main() {
  roles();
  pipes();
  consoles();
  phoneLink();
  std::printf("BLE link status: %u checks\n", checks);
}
