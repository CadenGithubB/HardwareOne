// Host substitutes around the extracted OLED incoming-pair-request prompt
// (OLED_ESPNow.cpp: the prompt statics, Accept/Reject callbacks and
// oledEspNowPollPairRequest, plus the file's owner-command helper when it has
// one) and the shipping confirm overlay from OLED_Utils.cpp. The pending request
// (System_ESPNow's gPendingPairIn), the owner commands espnowaccept/espnowreject
// behind the executor's admin gate, the session role answer and the toast are
// recorded doubles. A "frame" is one oledEspNowPollPairRequest() call, as
// oledUpdate() makes it on every pass.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
};

#define EXT_RAM_BSS_ATTR
#define INPUT_BUTTON_A 1u
#define INPUT_BUTTON_B 2u
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)
#define DEBUG_DISPLAYF(...) do {} while (false)

// INSERT_TYPES

static NavEvents gNavEvents{};
static OLEDMode currentOLEDMode = OLED_MENU;
static int dirtyMarks = 0;
static void oledMarkDirty() { ++dirtyMarks; }
typedef void (*OLEDConfirmCallback)(void* userData);
bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes, void* userData,
                        bool defaultYes = true, OLEDConfirmCallback onNo = nullptr);
bool oledConfirmIsActive();

// INSERT_CONFIRM

// --- System_ESPNow double: the single pending incoming request -------------
struct PendingRequest { bool active; uint8_t mac[6]; char name[24]; uint32_t atMs; };
static PendingRequest pending{};

static bool espnowGetIncomingPairRequest(char* name, size_t nameCap,
                                         uint8_t* mac = nullptr, uint32_t* atMs = nullptr) {
  if (!pending.active) return false;
  if (name && nameCap) { std::strncpy(name, pending.name, nameCap - 1); name[nameCap - 1] = '\0'; }
  if (mac) std::memcpy(mac, pending.mac, 6);
  if (atMs) *atMs = pending.atMs;
  return true;
}

// --- Owner commands (cmd_espnow_accept / cmd_espnow_reject) behind the gate --
static std::vector<std::string> commands;   // every line handed to either executor
static std::vector<std::string> accepted;   // names the owner paired
static std::vector<std::string> rejected;   // names the owner rejected
static bool ownerRefuses = false;           // executor's admin gate refuses (stale role cache)

static bool parseMac(const std::string& text, uint8_t out[6]) {
  unsigned v[6];
  if (text.size() != 17 ||
      std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) out[i] = (uint8_t)v[i];
  return true;
}

static bool ownerRun(const std::string& line, std::string& reply) {
  commands.push_back(line);
  const size_t space = line.find(' ');
  const std::string verb = line.substr(0, space);
  const std::string arg = space == std::string::npos ? std::string() : line.substr(space + 1);
  if (ownerRefuses) {
    reply = "Error: Admin access required for command '" + verb + "'. Contact an administrator.";
    return false;
  }
  if (!pending.active) {
    reply = verb == "espnowaccept"
                ? "No pending pair request. (Open discovery with 'espnowpairmode' on both.)"
                : "No pending pair request.";
    return true;
  }
  if (!arg.empty()) {   // pendingInMatchesArg: a MAC or the name must match the pending request
    uint8_t m[6];
    const bool match = parseMac(arg, m) ? std::memcmp(m, pending.mac, 6) == 0 : arg == pending.name;
    if (!match) {
      reply = "Error: that name/MAC doesn't match the pending request.";
      return true;
    }
  }
  pending.active = false;
  if (verb == "espnowaccept") {
    accepted.emplace_back(pending.name);
    reply = std::string("Accepted '") + pending.name + "'. Paired.";
  } else {
    rejected.emplace_back(pending.name);
    reply = std::string("Rejected pair request from '") + pending.name + "'.";
  }
  return true;
}

[[maybe_unused]] static void executeOLEDCommand(const String& line) {
  std::string reply;
  ownerRun(line, reply);
}
[[maybe_unused]] static bool executeOLEDCommandWithResult(const String& line, char* out, size_t outSize) {
  std::string reply;
  const bool ok = ownerRun(line, reply);
  std::snprintf(out, outSize, "%s", reply.c_str());
  return ok;
}

// --- Display identity ------------------------------------------------------
[[maybe_unused]] static bool gLocalDisplayAuthed = false;
static bool sessionAdmin = false;   // the OLED session role cache's admin answer
static int roleQueries = 0;         // oledSessionIsAdmin() calls (cached: no roster read)
static int rosterReads = 0;         // direct users.json reads from the prompt code
[[maybe_unused]] static bool oledSessionIsAdmin() { ++roleQueries; return gLocalDisplayAuthed && sessionAdmin; }
[[maybe_unused]] static bool isAdminUser(const String&) { ++rosterReads; return sessionAdmin; }
[[maybe_unused]] static bool getUserAuthorizationRole(const String&, String& role) {
  ++rosterReads;
  role = sessionAdmin ? "admin" : "user";
  return true;
}

static std::vector<std::string> toasts;
[[maybe_unused]] static void oledToastShow(const char* message, uint32_t = 3000) { toasts.emplace_back(message); }

// INSERT_HELPER

// INSERT_PROMPT

// --- Cases -----------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static const char* currentCase = "";
#define CHECK(condition, ...) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s: ", currentCase); std::fprintf(stderr, __VA_ARGS__); \
  std::fprintf(stderr, "\n  (%s:%d: %s)\n", __FILE__, __LINE__, #condition); } } while (false)

static const uint8_t kMacX[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
static const uint8_t kMacY[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const char* kAcceptX = "espnowaccept AA:BB:CC:01:02:03";
static const char* kRejectX = "espnowreject AA:BB:CC:01:02:03";
static const char* kAcceptY = "espnowaccept 02:11:22:33:44:55";

static int opens = 0;
static std::string lastPrompt;
static bool lastDefaultYes = true;

// v4h_pair_request: each arrival (a first or a re-sent request) stamps millis().
// The clock only moves forward, so every request in the run is distinct.
static uint32_t clockMs = 1000;
static void request(const uint8_t mac[6], const char* name) {
  pending.active = true;
  std::memcpy(pending.mac, mac, 6);
  std::snprintf(pending.name, sizeof(pending.name), "%s", name);
  clockMs += 1000;
  pending.atMs = clockMs;
}

static void frame() {
  const bool was = oledConfirmIsActive();
  oledEspNowPollPairRequest();
  if (!was && oledConfirmIsActive()) {
    ++opens;
    lastPrompt = gOledConfirmState.line2 ? gOledConfirmState.line2 : "";
    lastDefaultYes = gOledConfirmState.selectYes;
  }
}

static void pressB() { gNavEvents = NavEvents{}; oledConfirmHandleInput(INPUT_BUTTON_B); }
static void pressYes() { gNavEvents = NavEvents{}; gNavEvents.up = true; oledConfirmHandleInput(INPUT_BUTTON_A); }
static void pressA() { gNavEvents = NavEvents{}; oledConfirmHandleInput(INPUT_BUTTON_A); }

// A user who answers whatever the panel shows, every frame.
static void framesAnswering(int n, void (*press)()) {
  for (int i = 0; i < n; ++i) {
    frame();
    if (oledConfirmIsActive()) press();
  }
}

static void beginCase(const char* name, bool authed, bool admin, OLEDMode mode) {
  currentCase = name;
  oledConfirmResetForSessionBoundary();
  commands.clear();
  accepted.clear();
  rejected.clear();
  toasts.clear();
  ownerRefuses = !(authed && admin);   // the executor's gate: AuthBypass and non-admins are refused
  pending = PendingRequest{};
  gLocalDisplayAuthed = authed;
  sessionAdmin = admin;
  currentOLEDMode = mode;
  opens = 0;
  roleQueries = 0;
  lastPrompt.clear();
  lastDefaultYes = true;
}

static bool onlyCommand(const char* line) { return commands.size() == 1 && commands[0] == line; }

int main() {
  // R1: an admin whose answer the executor refuses (stale role cache): B once,
  // then the panel is free; the refusal is shown.
  beginCase("R1 refused reject", true, true, OLED_MENU);
  ownerRefuses = true;
  request(kMacX, "peerX");
  framesAnswering(50, pressB);
  CHECK(opens == 1, "the dialog opened %d times for one refused request over 50 frames; it must open once", opens);
  CHECK(onlyCommand(kRejectX), "expected exactly one '%s', got %zu command(s)%s%s", kRejectX, commands.size(),
        commands.empty() ? "" : ", first: ", commands.empty() ? "" : commands[0].c_str());
  CHECK(toasts.size() == 1 && toasts[0] == "Admin required",
        "the owner's refusal must be toasted once as 'Admin required' (%zu toast(s))", toasts.size());
  CHECK(pending.active, "the refused owner keeps the request pending (double sanity)");

  // R2: the same with Accept.
  beginCase("R2 refused accept", true, true, OLED_MENU);
  ownerRefuses = true;
  request(kMacX, "peerX");
  framesAnswering(50, pressYes);
  CHECK(opens == 1, "the dialog opened %d times for one refused request over 50 frames; it must open once", opens);
  CHECK(onlyCommand(kAcceptX), "expected exactly one '%s', got %zu command(s)", kAcceptX, commands.size());
  CHECK(toasts.size() == 1 && toasts[0] == "Admin required",
        "the owner's refusal must be toasted once as 'Admin required' (%zu toast(s))", toasts.size());

  // L1: nobody logged in, login screen: the prompt must not cover it.
  beginCase("L1 login screen", false, false, OLED_LOGIN);
  request(kMacX, "peerX");
  framesAnswering(50, pressB);
  CHECK(opens == 0, "the prompt opened %d times over the login screen; it must not open", opens);
  CHECK(commands.empty(), "no answer can be sent from the login screen (%zu sent)", commands.size());

  // L2: display auth off (AuthBypass, not logged in): the identity cannot answer.
  beginCase("L2 auth bypass", false, false, OLED_MENU);
  request(kMacX, "peerX");
  framesAnswering(50, pressB);
  CHECK(opens == 0, "the prompt opened %d times for the AuthBypass identity; it must not open", opens);
  CHECK(commands.empty(), "no answer is sent for AuthBypass (%zu sent)", commands.size());

  // L3: a logged-in user or guest session cannot answer.
  beginCase("L3 non-admin session", true, false, OLED_MENU);
  request(kMacX, "peerX");
  framesAnswering(50, pressB);
  CHECK(opens == 0, "the prompt opened %d times for a non-admin session; it must not open", opens);
  CHECK(commands.empty(), "no answer is sent for a non-admin session (%zu sent)", commands.size());

  // L4: a logged-in admin who opened the Login screen (switching accounts).
  beginCase("L4 admin on login screen", true, true, OLED_LOGIN);
  request(kMacX, "peerX");
  framesAnswering(20, pressB);
  CHECK(opens == 0, "the prompt opened %d times over the Login screen; it must not open there", opens);

  // L5: the request waits through the login screen and appears once for the admin.
  beginCase("L5 login then admin", false, false, OLED_LOGIN);
  request(kMacX, "peerX");
  for (int i = 0; i < 10; ++i) frame();
  CHECK(opens == 0, "the prompt opened over the login screen before the admin logged in");
  gLocalDisplayAuthed = true;
  sessionAdmin = true;
  ownerRefuses = false;
  currentOLEDMode = OLED_MENU;
  for (int i = 0; i < 10; ++i) frame();
  CHECK(opens == 1 && oledConfirmIsActive(), "after an admin login the pending request opens once (%d)", opens);
  CHECK(lastPrompt == "'peerX'", "the dialog names the sender ('%s')", lastPrompt.c_str());
  CHECK(!lastDefaultYes, "the dialog defaults to No");

  // A1: admin Accept names the shown sender and clears the request; no toast.
  beginCase("A1 admin accept", true, true, OLED_MENU);
  request(kMacX, "peerX");
  frame();
  CHECK(opens == 1 && lastPrompt == "'peerX'" && !lastDefaultYes, "the admin sees 'peerX', default No");
  pressYes();
  for (int i = 0; i < 20; ++i) frame();
  CHECK(onlyCommand(kAcceptX), "Accept must send '%s' (the shown sender), got %s", kAcceptX,
        commands.empty() ? "nothing" : commands[0].c_str());
  CHECK(accepted.size() == 1 && accepted[0] == "peerX" && !pending.active, "the owner paired peerX");
  CHECK(toasts.empty(), "an accepted request shows no toast (%zu)", toasts.size());
  CHECK(opens == 1, "an answered request does not re-open (%d)", opens);

  // A2: admin Reject (default No + A) names the shown sender.
  beginCase("A2 admin reject", true, true, OLED_MENU);
  request(kMacX, "peerX");
  frame();
  pressA();
  for (int i = 0; i < 20; ++i) frame();
  CHECK(onlyCommand(kRejectX), "Reject must send '%s' (the shown sender), got %s", kRejectX,
        commands.empty() ? "nothing" : commands[0].c_str());
  CHECK(rejected.size() == 1 && !pending.active && toasts.empty() && opens == 1,
        "the owner rejected peerX, no toast, no re-open");

  // A3: another sender replaced the request while the dialog showed peerX.
  beginCase("A3 replaced while open", true, true, OLED_MENU);
  request(kMacX, "peerX");
  frame();
  request(kMacY, "peerY");   // v4h_pair_request: a newer request replaces an unanswered one
  pressYes();
  CHECK(accepted.empty(), "Accept on the 'peerX' dialog paired %s, a sender the dialog did not show",
        accepted.empty() ? "" : accepted[0].c_str());
  CHECK(!commands.empty() && commands[0] == kAcceptX, "the answer names peerX's MAC (%s)",
        commands.empty() ? "nothing" : commands[0].c_str());
  CHECK(toasts.size() == 1 && toasts[0] == "Accept failed", "the mismatch is toasted as 'Accept failed' (%zu)",
        toasts.size());
  frame();
  CHECK(opens == 2 && lastPrompt == "'peerY'", "the newer request opens next with its own name (%d, %s)", opens,
        lastPrompt.c_str());
  pressYes();
  CHECK(commands.size() == 2 && commands[1] == kAcceptY && accepted.size() == 1 && accepted[0] == "peerY",
        "Accept on the 'peerY' dialog pairs peerY");

  // A4: a re-sent request (new arrival time) is a new request: it opens once more.
  beginCase("A4 resent after refusal", true, true, OLED_MENU);
  ownerRefuses = true;
  request(kMacX, "peerX");
  framesAnswering(10, pressB);
  request(kMacX, "peerX");   // the initiator re-sent: atMs restarts
  framesAnswering(10, pressB);
  CHECK(opens == 2, "two requests must open the dialog twice, opened %d times", opens);
  CHECK(commands.size() == 2 && toasts.size() == 2, "each request is answered and refused once (%zu, %zu)",
        commands.size(), toasts.size());

  // A5: the request expired while the dialog was up: the owner says so, no toast.
  beginCase("A5 expired while open", true, true, OLED_MENU);
  request(kMacX, "peerX");
  frame();
  pending.active = false;           // the owner's 45 s TTL
  pressYes();
  for (int i = 0; i < 10; ++i) frame();
  CHECK(toasts.empty() && opens == 1 && accepted.empty(), "an expired request answers quietly");

  // S1: a session boundary drops the dialog without an answer: the next admin
  // session still sees the request, once.
  beginCase("S1 session boundary", true, true, OLED_MENU);
  request(kMacX, "peerX");
  frame();
  oledConfirmResetForSessionBoundary();
  for (int i = 0; i < 10; ++i) frame();
  CHECK(opens == 2 && oledConfirmIsActive(), "the dropped prompt is shown once to the next session (%d)", opens);
  CHECK(commands.empty(), "a dropped prompt sends no answer (%zu)", commands.size());

  // N1: nothing pending: a frame costs no role query and no roster read.
  beginCase("N1 idle frames", true, true, OLED_MENU);
  const int readsBefore = rosterReads;
  for (int i = 0; i < 100; ++i) frame();
  CHECK(opens == 0 && roleQueries == 0 && rosterReads == readsBefore,
        "idle frames queried the role %d times and read the roster %d times", roleQueries,
        rosterReads - readsBefore);

  currentCase = "all";
  CHECK(rosterReads == 0, "the prompt read users.json %d times; it must use the cached session role", rosterReads);
  (void)dirtyMarks;

  std::printf("oled_pair_prompt: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
