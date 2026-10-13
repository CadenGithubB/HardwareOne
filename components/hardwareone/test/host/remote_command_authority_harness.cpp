// Drives the production remote:/@ routing of executeCommand (System_Utils.cpp)
// with the production authorizeCommand, its guest/password-change/session
// classifiers, CommandArgs, the remote reply owner (formatRemoteCommandSent /
// parseRemoteCommandSentReqId) and the registry's name matcher
// (System_CommandLookupCore.h). The accounts, the registry rows, redaction,
// the audit/event sinks and the bond transport are doubles; the transport
// double records every frame the routing hands it. Run with one case name.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

static unsigned checks = 0;
static const char* gCase = "";
static void checked(bool ok, const char* text, const std::string& detail, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "%s: check failed at line %d: %s%s%s\n", gCase, line, text,
               detail.empty() ? "" : " -- ", detail.c_str());
  std::exit(1);
}
#define check(condition) checked((condition), #condition, std::string(), __LINE__)
#define checkd(condition, detail) checked((condition), #condition, (detail), __LINE__)

// Harness-local String: the members the extracted code calls, over std::string.
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  char operator[](size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
  const char* c_str() const { return text_.c_str(); }
  unsigned length() const { return static_cast<unsigned>(text_.length()); }
  int indexOf(char value, size_t from = 0) const {
    const auto pos = text_.find(value, from);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t from) const { return from >= text_.size() ? std::string() : text_.substr(from); }
  String substring(size_t from, size_t to) const {
    if (from >= text_.size() || to <= from) return String();
    return text_.substr(from, to - from);
  }
  void trim() {
    const char* ws = " \t\r\n\v\f";
    const auto first = text_.find_first_not_of(ws);
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(ws) - first + 1);
  }
  void toLowerCase() {
    for (char& c : text_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool startsWith(const char* prefix) const { return text_.rfind(prefix, 0) == 0; }
  bool startsWith(const String& prefix) const { return text_.rfind(prefix.text_, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const size_t n = std::strlen(suffix);
    return text_.size() >= n && text_.compare(text_.size() - n, n, suffix) == 0;
  }
  String operator+(const char* other) const { return text_ + other; }
  String operator+(const String& other) const { return text_ + other.text_; }
  friend String operator+(const char* left, const String& right) { return std::string(left) + right.text_; }
  String& operator+=(char c) { text_ += c; return *this; }
  String& operator+=(const char* other) { text_ += other; return *this; }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator!=(const String& other) const { return text_ != other.text_; }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator!=(const char* other) const { return !(*this == other); }
  bool equalsIgnoreCase(const String& other) const {
    return strcasecmp(text_.c_str(), other.text_.c_str()) == 0;
  }
  int toInt() const { return std::atoi(text_.c_str()); }
  float toFloat() const { return static_cast<float>(std::atof(text_.c_str())); }
  const std::string& str() const { return text_; }

 private:
  std::string text_;
};

// Production types: kBondAdminUser, CommandSource, AuthContext, the bond
// opcode and flag.
// INSERT_TYPES

// ---------------------------------------------------------------------------
// Registry double. Rows carry the flags the shipping registry declares for
// these names (set 1 made the file verbs user-level); names resolve through
// the production matcher. A name this build does not compile is absent.
// ---------------------------------------------------------------------------
// INSERT_LOOKUP
struct CommandEntry { const char* name; bool requiresAdmin; bool requiresSuperAdmin; };
struct CommandResolution {
  const CommandEntry* entry = nullptr;
  size_t matchedLength = 0;
  size_t lineOffset = 0;
  size_t registryOrdinal = static_cast<size_t>(-1);
};
static const CommandEntry kRegistry[] = {
    {"files", false, false},       {"fileread", false, false},    {"fileview", false, false},
    {"filewrite", false, false},   {"filecreate", false, false},  {"mkdir", false, false},
    {"rmdir", false, false},       {"filedelete", false, false},  {"filerename", false, false},
    {"sensorlog", false, false},   {"whoami", false, false},      {"login", false, false},
    {"logout", false, false},      {"opensr", false, false},      {"closesr", false, false},
    {"openthermal", false, false}, {"closethermal", false, false}, {"opentof", false, false},
    {"closetof", false, false},    {"openimu", false, false},     {"closeimu", false, false},
    {"openinput", false, false},   {"closeinput", false, false},  {"openapds", false, false},
    {"closeapds", false, false},   {"opengps", false, false},     {"closegps", false, false},
    {"openrtc", false, false},     {"closertc", false, false},    {"openpresence", false, false},
    {"closepresence", false, false}, {"openfmradio", false, false}, {"closefmradio", false, false},
    {"oledbrightness", false, false}, {"imagedelete", true, false}, {"openwifi", true, false},
    {"bondrole", true, false},     {"userdelete", true, false},   {"factoryreset", true, true},
};
static const size_t kRegistrySize = sizeof(kRegistry) / sizeof(kRegistry[0]);
CommandResolution resolveCommand(const String& line) {
  const hw1_command_lookup::Match match = hw1_command_lookup::resolve(
      line.c_str(), line.length(), kRegistrySize,
      [](size_t index) -> const char* { return kRegistry[index].name; });
  CommandResolution resolution;
  resolution.matchedLength = match.matchedLength;
  resolution.lineOffset = match.lineOffset;
  resolution.registryOrdinal = match.registryOrdinal;
  if (match.found()) resolution.entry = &kRegistry[match.registryOrdinal];
  return resolution;
}

// Redaction double: credential verbs keep only the verb, as the shipping
// redactor does for login.
String redactCmdForAudit(const String& line, const CommandResolution&) {
  String copy = line;
  copy.trim();
  const int space = copy.indexOf(' ');
  const String verb = space > 0 ? copy.substring(0, space) : copy;
  if (space > 0 && (verb.equalsIgnoreCase("login") || verb.equalsIgnoreCase("userchangepassword"))) {
    return verb + " ***";
  }
  return copy;
}

// ---------------------------------------------------------------------------
// Accounts: root is an administrator, owner the superadmin, alice a user,
// gina a guest. AuthBypass and the trusted "system" identity are not roster
// accounts (authorizeCommand handles them itself).
// ---------------------------------------------------------------------------
static bool gBondLive = true;
static bool roleOf(const std::string& user, std::string& role, bool& super) {
  super = false;
  if (user == "root") { role = "admin"; return true; }
  if (user == "owner") { role = "admin"; super = true; return true; }
  if (user == "alice") { role = "user"; return true; }
  if (user == "gina") { role = "guest"; return true; }
  return false;
}
static bool getUserAuthorizationRole(const String& user, String& out) {
  std::string role;
  bool super = false;
  if (!roleOf(user.str(), role, super)) return false;
  out = role.c_str();
  return true;
}
static bool isSuperAdminUser(const String& user) {
  if (user.str() == kBondAdminUser) return gBondLive;
  std::string role;
  bool super = false;
  return roleOf(user.str(), role, super) && super;
}
static bool hasAdminPrivilege(const AuthContext& ctx) {
  if (ctx.user.str() == kBondAdminUser) return gBondLive && ctx.transport == SOURCE_ESPNOW;
  std::string role;
  bool super = false;
  return roleOf(ctx.user.str(), role, super) && (role == "admin" || super);
}
static bool hasSuperAdminPrivilege(const AuthContext& ctx) {
  if (ctx.user.str() == kBondAdminUser) return gBondLive && ctx.transport == SOURCE_ESPNOW;
  return isSuperAdminUser(ctx.user);
}
static bool userMustChangePassword(const String&) { return false; }
static bool callerMayTargetAnotherSession(const AuthContext&, const char** denialOut) {
  if (denialOut) *denialOut = "Error: targeted session control denied.";
  return false;
}

// Audit and event sinks.
enum { SYSEVT_COMMAND_DENIED = 7 };
struct Audit { bool success; std::string path, user, ip, detail; };
static std::vector<Audit> gAudits;
static void logAuthAttempt(bool success, const char* path, const String& user, const String& ip,
                           const char* detail) {
  gAudits.push_back({success, path ? path : "", user.str(), ip.str(), detail ? detail : ""});
}
struct Event { int kind; std::string subject, detail; };
static std::vector<Event> gEvents;
static void systemEventPost(int kind, const char* subject = nullptr, const char* detail = nullptr) {
  gEvents.push_back({kind, subject ? subject : "", detail ? detail : ""});
}

// ---------------------------------------------------------------------------
// Bond transport doubles (the externs the remote block declares).
// ---------------------------------------------------------------------------
struct Settings { String bondPeerMac; };
Settings gSettings;
static bool gTokenValid = true;
static uint32_t gNextMsgId = 41;
struct Frame { uint8_t type; uint16_t flags; uint32_t msgId; std::string payload; uint8_t ttl; };
static std::vector<Frame> gFrames;
static std::vector<std::string> gBroadcasts;
bool isBondSynced() { return gTokenValid; }
bool isBondSessionTokenValid() { return gTokenValid; }
String buildBondedCommandPayload(const String& command) {
  return String("@BOND:0123456789abcdef0123456789abcdef:") + command;
}
bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t baseFlags, uint32_t msgId,
                                const uint8_t* plaintext, uint16_t plaintextLen, uint8_t ttl,
                                char* outStatus, size_t outStatusLen) {
  (void)dst; (void)outStatus; (void)outStatusLen;
  gFrames.push_back({type, baseFlags, msgId,
                     std::string(reinterpret_cast<const char*>(plaintext), plaintextLen), ttl});
  return true;
}
uint32_t generateMessageId() { return ++gNextMsgId; }
bool parseMacAddress(const String& text, uint8_t mac[6]) {
  if (text.length() == 0) return false;
  for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(i + 1);
  return true;
}
void broadcastOutput(const String& line) { gBroadcasts.push_back(line.str()); }

// INSERT_PRODUCTION

// The remote routing of executeCommand, unchanged, after the line was
// trimmed. Returns 1/0 for the routing's own true/false, 2 when the line is
// not a remote command (executeCommand continues locally).
static int remoteRouting(AuthContext& ctx, const char* cmd, char* out, size_t outSize) {
  out[0] = '\0';
  String command = cmd;
  command.trim();
  // INSERT_REMOTE_ROUTING
  return 2;
}

// Sensor ids the bond page (WebPage_Bond.cpp) and the OLED bond screen
// (OLED_Mode_Remote.cpp) start and stop on the peer.
// INSERT_SENDER_SENSORS

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static AuthContext who(CommandSource transport, const char* user, const char* path = "/api/bond/exec") {
  AuthContext ctx{};
  ctx.transport = transport;
  ctx.user = user;
  ctx.path = path;
  ctx.ip = transport == SOURCE_WEB ? "192.0.2.9" : "local";
  ctx.opaque = nullptr;
  return ctx;
}
static void reset() {
  gAudits.clear();
  gEvents.clear();
  gFrames.clear();
  gBroadcasts.clear();
  gTokenValid = true;
  gBondLive = true;
  gSettings.bondPeerMac = "bonded-peer";
}
struct Outcome { int result; std::string reply; };
static Outcome run(AuthContext ctx, const char* line) {
  char out[256];
  const int result = remoteRouting(ctx, line, out, sizeof(out));
  return {result, out};
}
static bool contains(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}
static std::string describe(const char* line, const Outcome& o) {
  return std::string("line '") + line + "' -> " + std::to_string(o.result) + " '" + o.reply +
         "', frames=" + std::to_string(gFrames.size());
}

// The command reached the bond: one CMD frame carrying exactly the unwrapped
// line, the reply names the request id, and the reply owner parses it back.
static void expectSent(AuthContext ctx, const char* line, const char* unwrapped,
                       const char* echoed = nullptr) {
  reset();
  const Outcome o = run(ctx, line);
  checkd(o.result == 1, describe(line, o));
  checkd(gFrames.size() == 1, describe(line, o));
  const Frame& frame = gFrames[0];
  check(frame.type == ESPNOW_V4_TYPE_CMD);
  check(frame.flags == ESPNOW_V4_FLAG_ACK_REQ);
  check(frame.ttl == 1);
  checkd(frame.payload == std::string("@BOND:0123456789abcdef0123456789abcdef:") + unwrapped,
         frame.payload);
  char expected[256];
  formatRemoteCommandSent(expected, sizeof(expected), echoed ? echoed : unwrapped, frame.msgId);
  checkd(o.reply == expected, o.reply);
  uint32_t parsed = 0;
  check(parseRemoteCommandSentReqId(o.reply.c_str(), &parsed) && parsed == frame.msgId);
  check(gBroadcasts.size() == 1);
  check(gAudits.empty() && gEvents.empty());
}

// Refused before the bond: nothing sent, an admin refusal, one failed audit
// entry with the redacted line and, for a named caller, one denial event.
static void expectAdminRefusal(AuthContext ctx, const char* line, const char* forbidden = nullptr) {
  reset();
  const Outcome o = run(ctx, line);
  checkd(o.result == 0, describe(line, o));
  checkd(gFrames.empty() && gBroadcasts.empty(), describe(line, o));
  checkd(o.reply.rfind("Error: Admin access required", 0) == 0, describe(line, o));
  checkd(gAudits.size() == 1 && !gAudits[0].success, describe(line, o));
  const std::string expectedUser = ctx.user.length() ? ctx.user.str() : "(anonymous)";
  checkd(gAudits[0].user == expectedUser, gAudits[0].user);
  checkd(gAudits[0].path == ctx.path.str(), gAudits[0].path);
  if (forbidden) {
    checkd(!contains(gAudits[0].detail, forbidden), gAudits[0].detail);
    checkd(!contains(o.reply, forbidden), o.reply);
  }
  if (ctx.user.length() > 0) {
    checkd(gEvents.size() == 1 && gEvents[0].kind == SYSEVT_COMMAND_DENIED &&
               gEvents[0].subject == ctx.user.str(),
           describe(line, o));
  } else {
    check(gEvents.empty());
  }
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

// A signed-in user cannot run the user-level verbs whose access is decided at
// run time from the caller: on the peer that caller is bond-admin (super).
static void caseUserCannotUseBondAuthority() {
  const AuthContext alice = who(SOURCE_WEB, "alice");
  const char* lines[] = {
      "remote:filedelete \"/system/users/users.json\" confirm",
      "remote:filedelete /system/users/users.json",
      "@files \"/system\"",
      "remote fileread \"/system/certs/device.key\"",
      "remote:fileview \"/system/users/users.json\"",
      "remote:filewrite \"/system/users/users.json\" 0 e30= final",
      "remote:filecreate \"/system/x.json\"",
      "remote:mkdir \"/system/x\"",
      "remote:rmdir \"/system/ota\"",
      "remote:filerename \"/system/users/users.json\" \"old.json\"",
      "remote:sensorlog start /system/users/users.json",
      "remote:opensr",
      "remote:whoami",
      "remote:oledbrightness 7",
      "@  filedelete \"/system/users/users.json\"",
  };
  for (const char* line : lines) expectAdminRefusal(alice, line);
  // The refusal reaches the audit redacted and names the remote path.
  expectAdminRefusal(alice, "remote:filedelete /system/users/users.json");
  check(gAudits[0].detail == "remote cmd=filedelete /system/users/users.json");
  check(gEvents[0].detail == "remote");
  // Every transport a user reaches the executor from is gated the same way.
  const CommandSource transports[] = {SOURCE_SERIAL, SOURCE_LOCAL_DISPLAY, SOURCE_BLUETOOTH,
                                      SOURCE_MQTT, SOURCE_UART, SOURCE_G2_GLASSES};
  for (CommandSource transport : transports) {
    expectAdminRefusal(who(transport, "alice", "cli"), "remote:filedelete /system/users/users.json");
  }
  // An automation stored by a user runs as that user (SOURCE_INTERNAL).
  expectAdminRefusal(who(SOURCE_INTERNAL, "alice", "/automation"), "@filedelete /system/users/users.json");
  // A legacy automation row without an owner keeps the non-admin surface.
  expectAdminRefusal(who(SOURCE_INTERNAL, "", "/automation"), "@files /system");
  // The require-auth-off identity is not an administrator.
  expectAdminRefusal(who(SOURCE_LOCAL_DISPLAY, "AuthBypass", "oled"), "remote:filedelete /system/users/users.json");
  expectAdminRefusal(who(SOURCE_SERIAL, "AuthBypass", "serial"), "@opensr");
}

// A command this build does not compile has no row to check here; the peer
// would run it as bond-admin. A user cannot send it.
static void caseUnknownHereRefused() {
  const AuthContext alice = who(SOURCE_WEB, "alice");
  expectAdminRefusal(alice, "remote:camerasave \"/system/users/users.json\"");
  expectAdminRefusal(alice, "@notesdelete all");
  expectAdminRefusal(alice, "remote:transcriptdelete 3");
  // Not a sensor toggle: an argument, another verb, a quoted verb.
  expectAdminRefusal(alice, "remote:openthermal now");
  expectAdminRefusal(alice, "remote:openthermalx");
  expectAdminRefusal(alice, "remote:opensensor");
  expectAdminRefusal(alice, "remote:\"openthermal\"");
  expectAdminRefusal(alice, "remote:open");
  expectAdminRefusal(alice, "remote:closesr");
}

// The peer sensor toggles stay open to every signed-in, non-guest caller.
static void caseSensorTogglesStayOpen() {
  const AuthContext callers[] = {
      who(SOURCE_WEB, "alice"),
      who(SOURCE_LOCAL_DISPLAY, "AuthBypass", "oled"),
      who(SOURCE_LOCAL_DISPLAY, "alice", "oled"),
      who(SOURCE_INTERNAL, "alice", "/automation"),
      who(SOURCE_WEB, "root"),
  };
  for (const AuthContext& ctx : callers) {
    for (const char* id : kSenderSensorIds) {
      const std::string open = std::string("remote:open") + id;
      const std::string close = std::string("remote:close") + id;
      expectSent(ctx, open.c_str(), open.c_str() + 7);
      expectSent(ctx, close.c_str(), close.c_str() + 7);
    }
  }
  const AuthContext alice = who(SOURCE_WEB, "alice");
  expectSent(alice, "@opentof", "opentof");
  expectSent(alice, "remote closeimu", "closeimu");
  expectSent(alice, "remote:  openthermal  ", "openthermal");
  expectSent(alice, "remote:OPENGPS", "OPENGPS");
  expectSent(alice, "remote:openapds", "openapds");
  expectSent(alice, "remote:closefmradio", "closefmradio");
}

// Administrators keep the whole remote surface; the registry flags still
// decide super-only rows.
static void caseAdministratorsKeepRemote() {
  const AuthContext root = who(SOURCE_WEB, "root");
  expectSent(root, "remote:filedelete \"/system/old.json\" confirm", "filedelete \"/system/old.json\" confirm");
  expectSent(root, "@files \"/sd\"", "files \"/sd\"");
  expectSent(root, "remote:bondrole master", "bondrole master");
  expectSent(root, "remote:whoami", "whoami");
  expectSent(root, "remote:camerasave x", "camerasave x");
  expectSent(who(SOURCE_SERIAL, "root", "cli"), "remote:sensorlog status", "sensorlog status");
  expectSent(who(SOURCE_INTERNAL, "root", "/automation"), "@mkdir \"/sd/a\"", "mkdir \"/sd/a\"");
  // The trusted internal identity passes authorizeCommand; unchanged.
  expectSent(who(SOURCE_INTERNAL, "system", "/internal"), "remote:files", "files");

  reset();
  Outcome o = run(root, "remote:factoryreset");
  checkd(o.result == 0 && gFrames.empty() &&
             o.reply == "Error: Super-admin access required for command 'factoryreset'.",
         describe("remote:factoryreset", o));
  expectSent(who(SOURCE_WEB, "owner"), "remote:factoryreset", "factoryreset");

  // A user still gets the registry's own refusal for an admin row.
  reset();
  o = run(who(SOURCE_WEB, "alice"), "remote:bondrole master");
  checkd(o.result == 0 && gFrames.empty() &&
             o.reply == "Error: Admin access required for command 'bondrole'. Contact an administrator.",
         describe("remote:bondrole master", o));
  check(gAudits.size() == 1 && gEvents.size() == 1 && gEvents[0].detail == "bondrole");
}

// Guests stay view-only (the guest gate answers first, unchanged), and an
// unknown account fails closed before any remote decision.
static void caseGuestAndUnknownAccount() {
  const char* lines[] = {"remote:openthermal", "remote:filedelete x", "@files"};
  for (const char* line : lines) {
    reset();
    const Outcome o = run(who(SOURCE_WEB, "gina"), line);
    checkd(o.result == 0 && gFrames.empty() &&
               o.reply == "Error: Guest accounts are view-only. Only local login/logout and whoami are allowed." &&
               gAudits.size() == 1 && gEvents.size() == 1,
           describe(line, o));
  }
  // whoami is on the guest surface locally; on the peer it would run as
  // bond-admin, so the remote gate refuses it.
  expectAdminRefusal(who(SOURCE_WEB, "gina"), "remote:whoami");
  reset();
  Outcome o = run(who(SOURCE_WEB, "ghost"), "remote:openthermal");
  checkd(o.result == 0 && gFrames.empty() &&
             o.reply == "Error: unable to verify the current account role.",
         describe("ghost", o));
}

// The nested wrapper refusal still comes first, for administrators too.
static void caseNestedRefusalFirst() {
  const char* lines[] = {"remote:remote:filedelete x", "remote:@files", "@remote:whoami",
                         "@@files", "remote remote openthermal", "@ remote:openthermal"};
  const AuthContext callers[] = {who(SOURCE_WEB, "alice"), who(SOURCE_WEB, "owner")};
  for (const AuthContext& ctx : callers) {
    for (const char* line : lines) {
      reset();
      const Outcome o = run(ctx, line);
      checkd(o.result == 0 && gFrames.empty() && gAudits.empty() &&
                 o.reply == "Error: nested remote commands are not allowed.",
             describe(line, o));
    }
  }
}

// The refusal audit, the reply and the "sent" echo carry the redacted line.
static void caseRedaction() {
  expectAdminRefusal(who(SOURCE_WEB, "alice"), "remote:login bob hunter2", "hunter2");
  check(contains(gAudits[0].detail, "login ***"));
  // An anonymous pre-auth login cannot ride the bond either.
  expectAdminRefusal(who(SOURCE_WEB, ""), "remote:login bob hunter2", "hunter2");
  expectAdminRefusal(who(SOURCE_BLUETOOTH, ""), "@login bob hunter2", "hunter2");
  // An administrator's remote login is sent; only the wire carries the secret.
  expectSent(who(SOURCE_SERIAL, "root", "cli"), "@login bob hunter2", "login bob hunter2", "login ***");
  check(!contains(gBroadcasts[0], "hunter2"));
}

// Not a remote line: the routing does not answer and sends nothing.
static void caseLocalLinesUntouched() {
  const char* lines[] = {"filedelete x", "files", "@", "remotefiles", "REMOTE:filedelete x",
                         "openthermal"};
  for (const char* line : lines) {
    reset();
    const Outcome o = run(who(SOURCE_WEB, "alice"), line);
    checkd(o.result == 2 && gFrames.empty() && gAudits.empty() && o.reply.empty(), describe(line, o));
  }
}

// Bond readiness is still checked after authorization, unchanged.
static void caseBondNotReady() {
  reset();
  gTokenValid = false;
  Outcome o = run(who(SOURCE_WEB, "root"), "remote:files");
  checkd(o.result == 0 && gFrames.empty() &&
             o.reply == "Error: No session token - bond not ready or passphrase mismatch",
         describe("no token", o));
  reset();
  gSettings.bondPeerMac = "";
  o = run(who(SOURCE_WEB, "alice"), "remote:openthermal");
  checkd(o.result == 0 && gFrames.empty() && o.reply == "Error: Invalid bonded peer MAC",
         describe("no peer", o));
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return 2;
  }
  struct Case { const char* name; void (*run)(); };
  static const Case kCases[] = {
      {"user_cannot_use_bond_authority", caseUserCannotUseBondAuthority},
      {"unknown_here_refused", caseUnknownHereRefused},
      {"sensor_toggles_stay_open", caseSensorTogglesStayOpen},
      {"administrators_keep_remote", caseAdministratorsKeepRemote},
      {"guest_and_unknown_account", caseGuestAndUnknownAccount},
      {"nested_refusal_first", caseNestedRefusalFirst},
      {"redaction", caseRedaction},
      {"local_lines_untouched", caseLocalLinesUntouched},
      {"bond_not_ready", caseBondNotReady},
  };
  for (const Case& c : kCases) {
    if (std::strcmp(argv[1], c.name) != 0) continue;
    gCase = c.name;
    c.run();
    std::printf("%u checks\n", checks);
    return 0;
  }
  std::fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
