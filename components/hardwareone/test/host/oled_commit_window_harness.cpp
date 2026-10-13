// Host harness for the OLED display-session writer windows
// (test_oled_commit_window.py).
//
// The suite pastes from OLED_Utils.cpp: the command-result path
// (executeOLEDCommandWithResultImpl, with oledDisplayEpochLiveUnderWriter where
// the tree has it) and, when the tree has them, the frame-commit fence
// (oledFrameCommitAllowed, oledCommitFrame). The fakes model the display
// lifecycle writer as a mutex another task's publisher has to wait for, the
// live-session registry, the boundary generation and the panel push. A
// publication can be injected during command execution, during the result copy
// and during the push: it happens only if the writer is free, as on the device.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define DEBUG_DISPLAYF(...) do { } while (false)

// Arduino String, reduced to what the pasted code uses.
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.length(); }
  void clear() { text_.clear(); }
  friend bool operator==(const String& a, const String& b) { return a.text_ == b.text_; }

 private:
  std::string text_;
};
static void secureClearString(String& value) { value.clear(); }

using TransportSessionEpoch = uint32_t;
static constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;
enum CommandSource { SOURCE_SERIAL = 0, SOURCE_LOCAL_DISPLAY = 6 };

// --- Session authority (System_User.cpp stand-in) ---------------------------
static bool gRequireAuth = true;
static TransportSessionEpoch gAuthEpoch = 0;   // the display's published epoch
static bool gAuthAuthed = false;
static std::string gAuthUser;
static TransportSessionEpoch gNextEpoch = 100;
static std::set<TransportSessionEpoch> gLive;  // the session registry
static int gOpens = 0;                          // sessions opened by a reader

static bool gWriterHeld = false;
static void writerTake() { CHECK(!gWriterHeld); gWriterHeld = true; }
static void writerGive() { CHECK(gWriterHeld); gWriterHeld = false; }

static TransportSessionEpoch openSession() {
  const TransportSessionEpoch epoch = ++gNextEpoch;
  gLive.insert(epoch);
  return epoch;
}

static uint32_t gBoundaryRequested = 0;
static uint32_t gBoundaryApplied = 0;
static bool oledSessionBoundaryPending() { return gBoundaryRequested != gBoundaryApplied; }

static bool policyLive(TransportSessionEpoch epoch) {
  return epoch != 0 && epoch == gAuthEpoch && (gAuthAuthed || !gRequireAuth) &&
         gLive.count(epoch) != 0;
}

bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  return source == SOURCE_LOCAL_DISPLAY && policyLive(epoch);
}

bool localDisplayTransportSessionBeginDelivery(TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  writerTake();
  if (!policyLive(epoch)) {
    writerGive();
    return false;
  }
  return true;
}
void localDisplayTransportSessionEndDelivery() { writerGive(); }

// Like the device: logged out with display auth off opens a bypass session.
static TransportSessionEpoch snapshotOrOpen(String& user, bool& authed) {
  if (gAuthEpoch == 0 && !gRequireAuth) {
    gAuthEpoch = openSession();
    gAuthAuthed = false;
    gAuthUser.clear();
    ++gOpens;
  }
  user = String(gAuthAuthed ? gAuthUser : std::string());
  authed = gAuthAuthed && gAuthEpoch != 0;
  return gAuthEpoch;
}

TransportSessionEpoch localDisplayTransportSessionBeginUiSync(String& user, bool& authed) {
  writerTake();
  return snapshotOrOpen(user, authed);
}
void localDisplayTransportSessionEndUiSync() { writerGive(); }

// The lock-free snapshot also opens when logged out with display auth off.
TransportSessionEpoch localDisplayTransportSessionSnapshot(String& user, bool& authed) {
  return snapshotOrOpen(user, authed);
}

// A publication from another task (remote login, logout or replacement). It
// needs the writer; while the OLED task holds it, the publisher waits, which
// this single-threaded model records as "blocked" and does not publish.
enum class Publish { None, Replace, Logout };
static Publish gPublishAtExec = Publish::None;
static Publish gPublishAtCopy = Publish::None;
static Publish gPublishAtPush = Publish::None;
static int gPublished = 0;
static int gPublisherBlocked = 0;

static void publish(Publish kind) {
  if (kind == Publish::None) return;
  if (gWriterHeld) {
    ++gPublisherBlocked;
    return;
  }
  writerTake();
  gLive.erase(gAuthEpoch);
  if (kind == Publish::Replace) {
    gAuthEpoch = openSession();
    gAuthAuthed = true;
    gAuthUser = "bob";
  } else {
    gAuthEpoch = 0;
    gAuthAuthed = false;
    gAuthUser.clear();
  }
  // oledNotifyLocalDisplayAuthChanged(): the boundary generation. Its own
  // oledMarkDirty() is left out so the commit's re-check is observed alone.
  ++gBoundaryRequested;
  ++gPublished;
  writerGive();
}

// --- OLED owner state (OLED_Utils.cpp) --------------------------------------
static TransportSessionEpoch sOledUiSessionEpoch = 0;
static bool sOledUiSessionAuthed = false;
static String gLocalDisplayUser;
static int gDirtyMarks = 0;
static void oledMarkDirty() { ++gDirtyMarks; }

// The panel push.
static int gPushes = 0;
static int gPushesUnderWriter = 0;
static void displayUpdate() {
  ++gPushes;
  if (gWriterHeld) ++gPushesUnderWriter;
  publish(gPublishAtPush);
}

// --- The OLED command path ---------------------------------------------------
struct CommandContext {
  TransportSessionEpoch transportSessionEpoch = 0;
};
struct Command {
  String line;
  CommandContext ctx;
};
static Command buildOLEDCommand(const String& line) {
  Command command;
  command.line = line;
  command.ctx.transportSessionEpoch = sOledUiSessionEpoch;
  return command;
}
static int gSubmits = 0;
static bool gExecOk = true;
static std::string gExecOut;
static bool submitAndExecuteSync(Command& command, String& out) {
  (void)command;
  ++gSubmits;
  out = String(gExecOut);
  publish(gPublishAtExec);
  return gExecOk;
}

// The result copy into the caller's buffer.
static int gCopies = 0;
static int gCopiesUnderWriter = 0;
static char* harness_strncpy(char* dst, const char* src, size_t n) {
  ++gCopies;
  if (gWriterHeld) ++gCopiesUnderWriter;
  char* result = std::strncpy(dst, src, n);
  publish(gPublishAtCopy);
  return result;
}
#define strncpy harness_strncpy

// INSERT_OLED_UTILS

#undef strncpy

// --- Scenarios ----------------------------------------------------------------
// A logged-in display whose owner mirror matches the authority.
static void loggedIn(const char* user = "alice") {
  gRequireAuth = true;
  gLive.clear();
  gAuthEpoch = openSession();
  gAuthAuthed = true;
  gAuthUser = user;
  gBoundaryRequested = gBoundaryApplied = 7;
  sOledUiSessionEpoch = gAuthEpoch;
  sOledUiSessionAuthed = true;
  gLocalDisplayUser = String(user);
  gOpens = gPublished = gPublisherBlocked = 0;
  gDirtyMarks = gPushes = gPushesUnderWriter = 0;
  gSubmits = gCopies = gCopiesUnderWriter = 0;
  gPublishAtExec = gPublishAtCopy = gPublishAtPush = Publish::None;
  gExecOk = true;
  gExecOut = "OK: done";
  gWriterHeld = false;
}

static bool allZero(const char* buffer, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (buffer[i] != '\0') return false;
  }
  return true;
}

static void resultDelivered() {
  loggedIn();
  char out[32];
  std::memset(out, 'x', sizeof(out));
  CHECK(executeOLEDCommandWithResultImpl(String("filelist /"), 0, false, out, sizeof(out)));
  CHECK(std::strcmp(out, "OK: done") == 0);
  CHECK(gSubmits == 1 && gCopies == 1);
  // The copy runs with the writer released, and nothing is left held.
  CHECK(gCopiesUnderWriter == 0);
  CHECK(!gWriterHeld);
  CHECK(gOpens == 0);

  // Truncated to the buffer, always terminated.
  loggedIn();
  gExecOut = "OK: a long result that does not fit";
  char small[8];
  CHECK(executeOLEDCommandWithResultImpl(String("x"), 0, false, small, sizeof(small)));
  CHECK(std::strlen(small) == sizeof(small) - 1 && std::strncmp(small, "OK: a l", 7) == 0);

  // A handler error is delivered with the executor's flag.
  loggedIn();
  gExecOk = false;
  gExecOut = "Error: nope";
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), 0, false, out, sizeof(out)));
  CHECK(std::strcmp(out, "Error: nope") == 0);
}

static void resultFenced() {
  // Replaced while cmd_exec ran: no result, nothing copied.
  loggedIn();
  gPublishAtExec = Publish::Replace;
  char out[32];
  std::memset(out, 'x', sizeof(out));
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), 0, false, out, sizeof(out)));
  CHECK(gPublished == 1 && out[0] == '\0' && gCopies == 0);
  CHECK(!gWriterHeld);

  // Replaced during the copy: the publisher is not held off by the copy, and
  // the re-check wipes the old identity's result.
  loggedIn();
  gPublishAtCopy = Publish::Replace;
  std::memset(out, 'x', sizeof(out));
  const bool delivered = executeOLEDCommandWithResultImpl(String("x"), 0, false, out, sizeof(out));
  CHECK(gPublisherBlocked == 0);
  CHECK(gPublished == 1);
  CHECK(!delivered);
  CHECK(allZero(out, sizeof(out)));
  CHECK(!gWriterHeld);

  // Logged out during the copy (display auth on): same.
  loggedIn();
  gPublishAtCopy = Publish::Logout;
  std::memset(out, 'x', sizeof(out));
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), 0, false, out, sizeof(out)));
  CHECK(allZero(out, sizeof(out)));

  // Logged out during the copy with display auth off: refused, and the
  // re-check opens no bypass session.
  loggedIn();
  gRequireAuth = false;
  gPublishAtCopy = Publish::Logout;
  std::memset(out, 'x', sizeof(out));
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), 0, false, out, sizeof(out)));
  CHECK(allZero(out, sizeof(out)));
  CHECK(gOpens == 0);

  // The caller's session epoch is no longer the display's: refused unsent.
  loggedIn();
  const TransportSessionEpoch stale = sOledUiSessionEpoch - 1;
  std::memset(out, 'x', sizeof(out));
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), stale, true, out, sizeof(out)));
  CHECK(gSubmits == 0 && out[0] == '\0');
  CHECK(!executeOLEDCommandWithResultImpl(String("x"), kNoTransportSessionEpoch, true, out, sizeof(out)));
  CHECK(gSubmits == 0);
  CHECK(executeOLEDCommandWithResultImpl(String("x"), sOledUiSessionEpoch, true, out, sizeof(out)));
  CHECK(gSubmits == 1 && std::strcmp(out, "OK: done") == 0);
}

#if HARNESS_HAS_COMMIT_FENCE
static void frameCommitted() {
  loggedIn();
  oledCommitFrame();
  CHECK(gPushes == 1);
  CHECK(gPushesUnderWriter == 0);
  CHECK(gDirtyMarks == 0);
  CHECK(!gWriterHeld && gOpens == 0);

  // The logged-out login screen (epoch zero, display auth on) commits every
  // frame without re-marking itself dirty.
  loggedIn();
  gLive.clear();
  gAuthEpoch = 0;
  gAuthAuthed = false;
  gAuthUser.clear();
  sOledUiSessionEpoch = 0;
  sOledUiSessionAuthed = false;
  gLocalDisplayUser = String();
  for (int frame = 0; frame < 3; ++frame) oledCommitFrame();
  CHECK(gPushes == 3 && gDirtyMarks == 0 && gOpens == 0);

  // Display auth off: the bypass session the boundary adopted commits.
  loggedIn();
  gRequireAuth = false;
  gAuthAuthed = false;
  gAuthUser.clear();
  sOledUiSessionAuthed = false;
  gLocalDisplayUser = String();
  oledCommitFrame();
  CHECK(gPushes == 1 && gDirtyMarks == 0 && gOpens == 0);
}

static void frameRefused() {
  // A boundary is pending: nothing reaches the panel, the next pass redraws.
  loggedIn();
  ++gBoundaryRequested;
  oledCommitFrame();
  CHECK(gPushes == 0 && gDirtyMarks == 1 && !gWriterHeld);

  // The owner mirror is another epoch, another user or another auth state.
  loggedIn();
  sOledUiSessionEpoch = gAuthEpoch + 1;
  oledCommitFrame();
  CHECK(gPushes == 0 && gDirtyMarks == 1 && !gWriterHeld);
  loggedIn();
  gLocalDisplayUser = String("mallory");
  oledCommitFrame();
  CHECK(gPushes == 0 && gDirtyMarks == 1 && !gWriterHeld);
  loggedIn();
  sOledUiSessionAuthed = false;
  oledCommitFrame();
  CHECK(gPushes == 0 && gDirtyMarks == 1 && !gWriterHeld);
}

static void publicationDuringPush() {
  // A replacement during the push is not held off by it, and the frame is
  // marked for an immediate redraw.
  loggedIn();
  gPublishAtPush = Publish::Replace;
  oledCommitFrame();
  CHECK(gPushes == 1 && gPushesUnderWriter == 0);
  CHECK(gPublisherBlocked == 0 && gPublished == 1);
  CHECK(gDirtyMarks == 1);
  CHECK(!gWriterHeld && gOpens == 0);

  // A logout during the push with display auth off: redraw, and the re-check
  // opens no bypass session (the next boundary does that on the owner).
  loggedIn();
  gRequireAuth = false;
  gPublishAtPush = Publish::Logout;
  oledCommitFrame();
  CHECK(gPushes == 1 && gPublished == 1);
  CHECK(gDirtyMarks == 1);
  CHECK(gOpens == 0);

  // The epoch closed without a generation bump (registry only): still redrawn.
  loggedIn();
  gPublishAtPush = Publish::None;
  oledCommitFrame();
  CHECK(gDirtyMarks == 0);
  gLive.clear();
  oledCommitFrame();  // validation still passes on the mirror; the re-check sees it
  CHECK(gDirtyMarks == 1);
}
#endif

int main() {
  resultDelivered();
  resultFenced();
#if HARNESS_HAS_COMMIT_FENCE
  frameCommitted();
  frameRefused();
  publicationDuringPush();
#endif
  if (failures) {
    std::fprintf(stderr, "OLED commit window: %u of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("checks=%u\n", checks);
  return 0;
}
