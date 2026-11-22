#include "OLED_MessageText.h"
#if HW1_HAVE_VALUE_SCROLL
#include "OLED_ValueScroll.h"  // the production coalescing rule of a value-scroll row
#endif
// Host harness for test_oled_command_feedback.py. The driver splices production
// code in at the INSERT_* markers: the OLED action helpers (OLED_Utils.cpp
// oledRunCommand, oledCommandReplyText, and the one OLED command in flight
// behind oledRunCommandAsync and the background executeOledCommand, with its
// frame-loop delivery, when the tree has them), System_PendingCommand as is,
// and real callers: the Automations toggle/delete actions and their shared
// toast, the Quick Settings rows and the Network screen's HTTP confirm
// callbacks. Everything else is a double: the OLED command executor and queue,
// the global toast, the Automations list rows and the radio / Wi-Fi /
// Bluetooth / HTTP state.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

// ---- Arduino String (only what the spliced code uses) ----------------------
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }

 private:
  std::string value_;
};

// ---- checks --------------------------------------------------------------------
static int gFailures = 0;
static const char* gCase = "";
#define EXPECT(cond, msg)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase,                \
                   std::string(msg).c_str(), #cond, __LINE__);                    \
      ++gFailures;                                                                \
    }                                                                             \
  } while (0)

// ---- platform --------------------------------------------------------------------
#define ENABLE_G2_GLASSES 1
#define ENABLE_BLUETOOTH 1
#define ENABLE_BLE_PHONE_PIPE 1
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// ---- OLED command executor doubles ------------------------------------------------
// Every submitted line is recorded; gExecOk/gExecText script the executor's
// verdict and reply (the reply is truncated to the caller's buffer like the real
// executeOledCommandWithResult).
static std::vector<std::string> gChecked;       // executeOledCommandWithResult lines
static std::vector<std::string> gFireAndForget; // executeOledCommand lines (reply dropped)
static void recordChecked(const char* line) { gChecked.emplace_back(line); }
static bool gExecOk = true;
static std::string gExecText = "OK";
bool executeOledCommandWithResult(const String& cmd, char* out, size_t outSize) {
  recordChecked(cmd.c_str());
  if (out && outSize) std::snprintf(out, outSize, "%s", gExecText.c_str());
  return gExecOk;
}
#if !HW1_HAVE_ASYNC_COMMAND
// A tree without the background path: the reply is dropped.
void executeOledCommand(const String& cmd) { gFireAndForget.emplace_back(cmd.c_str()); }
#endif

// ---- global toast (OLED_UI.h) -------------------------------------------------------
enum class OledUiIcon : uint8_t { NONE, INFO, SUCCESS, WARNING, ERROR, QUESTION };
static std::string gToast;
static int gToastCount = 0;
static uint32_t gToastDuration = 0;
static OledUiIcon gToastIcon = OledUiIcon::NONE;
static void oledToastShow(const char* message, uint32_t duration = 3000, OledUiIcon icon = OledUiIcon::NONE) {
  gToastDuration = duration; gToastIcon = icon;
  gToast = message ? message : "";
  ++gToastCount;
}

// ---- the command in flight: executor and queue doubles ------------------------------
// submitCommandAsync queues; the executor runs a queued line only when a case
// calls finishCommand(), on the case's own schedule, as cmd_exec would.
// System_PendingCommand.h/.cpp are spliced in unchanged below, against these
// doubles.
#if HW1_HAVE_ASYNC_COMMAND
using TransportSessionEpoch = uint32_t;
inline constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);
typedef void (*OledCommandDoneFn)(bool ok, const void* context);
struct CommandContext { TransportSessionEpoch transportSessionEpoch = kNoTransportSessionEpoch; };
struct Command {
  String line;
  CommandContext ctx;
};
#include "System_CommandLimits.h"
#define DEBUG_CMD_FLOWF(...) do {} while (0)
#define ERROR_COMMANDF(...) do {} while (0)
enum class AllocPolicy { PreferPsram };
static long gLiveAllocs = 0;
void* ps_alloc(size_t size, AllocPolicy, const char*) { ++gLiveAllocs; return ::operator new(size); }
void ps_free(void* p) { if (p) { --gLiveAllocs; ::operator delete(p); } }
static std::vector<std::string> gBroadcasts;
void broadcastOutput(const String& line) { gBroadcasts.emplace_back(line.c_str()); }
static std::vector<std::string> gWiped;  // lines the in-flight slot wiped when it ended
void secureClearString(String& s) { gWiped.emplace_back(s.c_str()); s = String(); }
static TransportSessionEpoch gUiEpoch = 7;    // the display session a command is built for
static TransportSessionEpoch gLiveEpoch = 7;  // the display session live now
static Command buildOledCommand(const String& line) {
  Command command;
  command.line = line;
  command.ctx.transportSessionEpoch = gUiEpoch;
  return command;
}
struct QueuedCommand { std::string line; ExecAsyncCallback callback; void* userData; };
static std::vector<QueuedCommand> gQueued;
static bool gQueueAccepts = true;
static void recordChecked(const char* line);
bool submitCommandAsync(const Command& command, ExecAsyncCallback callback, void* userData) {
  if (!gQueueAccepts) return false;
  recordChecked(command.line.c_str());
  gQueued.push_back({command.line.c_str(), callback, userData});
  return true;
}
bool commandQueueFull() { return !gQueueAccepts; }
static bool oledDisplayEpochLiveUnderWriter(TransportSessionEpoch epoch) {
  return epoch != kNoTransportSessionEpoch && epoch == gLiveEpoch;
}
static bool gOwnerTask = true;  // the caller is the display (owner) task
static bool oledIsOwnerTask() { return gOwnerTask; }
static int gDirtyMarks = 0;
static void oledMarkDirty() { ++gDirtyMarks; }
// INSERT_PENDING_COMMAND
#endif

// ---- production: the OLED action helper ---------------------------------------------
#if HW1_HAVE_RUN_COMMAND
// INSERT_RUN_COMMAND
#endif

// ---- Automations doubles (rows carry only what the actions read) ----------------------
struct HarnessAutoRow { long id; bool enabled; };
using AutomationSummary = HarnessAutoRow;  // current tree's row type
using AutoListItem = HarnessAutoRow;       // older tree's row type
struct AutomationRenderData {
  HarnessAutoRow items[4];
  int count;
  int selectedIdx;
  bool valid;
  unsigned long lastRefresh;
};
static AutomationRenderData autoRenderData;
static bool autoForceRefresh = false;
static long sPendingDeleteId = 0;
static unsigned long gDirtyUntil = 0;
static void oledMarkDirtyUntil(unsigned long untilMs) { if (untilMs > gDirtyUntil) gDirtyUntil = untilMs; }

// ---- Quick Settings / Network doubles -------------------------------------------------
static char quickStatusMsg[32] = "";
static bool gRadioOn = true;
bool wifiRadioOn() { return gRadioOn; }
struct HarnessWiFi {
  bool connected = true;
  bool isConnected() const { return connected; }
};
static HarnessWiFi WiFi;
static bool gBleServer = false, gG2Client = false, gPhonePipe = false;
static bool isBleServerInitialized() { return gBleServer; }
static bool isG2ClientInitialized() { return gG2Client; }
static bool phonePipeActive() { return gPhonePipe; }
static bool bleSubsystemActive() { return gBleServer || gG2Client || gPhonePipe; }
static const int BLE_MODE_SERVER = 1, BLE_MODE_G2_CLIENT = 2;  // the phone-client mode is gone
struct HarnessSettings { int bleMode = BLE_MODE_SERVER; };
static HarnessSettings gSettings;
static int gHttpHandle = 0;
static void* server = nullptr;

// ---- production: callers ------------------------------------------------------------
// INSERT_AUTOMATIONS
// INSERT_QUICK_SETTINGS
// INSERT_NETWORK

// ---- cases ------------------------------------------------------------------------------
static const char* kAdminRefusal =
    "Error: Admin access required for command 'radiopower'. Contact an administrator.";

static void resetWorld() {
  gChecked.clear();
  gFireAndForget.clear();
  gExecOk = true;
  gExecText = "OK";
  gToast.clear();
  gToastCount = 0;
  autoRenderData = AutomationRenderData();
  autoForceRefresh = false;
  sPendingDeleteId = 0;
  gDirtyUntil = 0;
  quickStatusMsg[0] = '\0';
  gRadioOn = true;
  WiFi.connected = true;
  gBleServer = gG2Client = gPhonePipe = false;
  gSettings.bleMode = BLE_MODE_SERVER;
  server = nullptr;
  gNowMs += 10000;
#if HW1_HAVE_ASYNC_COMMAND
  // A case leaves nothing in flight: whatever is still queued finishes (a
  // late reply frees its abandoned slot) and the reply is taken, then the
  // world is reset.
  gQueueAccepts = true;
  for (int i = 0; i < 8; ++i) {
    while (!gQueued.empty()) {
      const QueuedCommand next = gQueued.front();
      gQueued.erase(gQueued.begin());
      next.callback(true, "OK", next.userData);
    }
    if (!sOledCommand.pending.busy()) break;
    gNowMs += PendingCommand::kQueueTryEveryMs;
    oledDeliverCommandReply();
  }
  EXPECT(!sOledCommand.pending.busy() && gLiveAllocs == 0, "the previous case left a command in flight");
  gQueued.clear();
  gUiEpoch = gLiveEpoch = 7;
  gDirtyMarks = 0;
  gOwnerTask = true;
  gBroadcasts.clear();
  gWiped.clear();
  gToast.clear();
  gToastCount = 0;
  gChecked.clear();
#endif
}

// cmd_exec runs the oldest queued line with the scripted verdict and reply.
static void finishCommand() {
#if HW1_HAVE_ASYNC_COMMAND
  if (gQueued.empty()) return;
  const QueuedCommand next = gQueued.front();
  gQueued.erase(gQueued.begin());
  char out[512];  // cmd_exec's reply buffer
  std::snprintf(out, sizeof(out), "%s", gExecText.c_str());
  next.callback(gExecOk, out, next.userData);
#endif
}

// The next display frame.
static void nextFrame() {
#if HW1_HAVE_ASYNC_COMMAND
  oledDeliverCommandReply();
#endif
#if HW1_HAVE_VALUE_SCROLL
  oledValueScrollPump();
#endif
}

// A quick row's command runs and its reply reaches the screen.
static void land() {
  finishCommand();
  nextFrame();
}

static void refuse(const char* reply) {
  gExecOk = false;
  gExecText = reply;
}

static std::string ran() {
  std::string all;
  for (const auto& l : gChecked) all += (all.empty() ? "" : ", ") + l;
  for (const auto& l : gFireAndForget) all += (all.empty() ? "" : ", ") + l;
  return all.empty() ? "(none)" : all;
}

static std::string msg(const char* text) { return text ? text : "(none)"; }

#if HW1_HAVE_RUN_COMMAND
static void case_helper_admin_refusal() {
  gCase = "helper_admin_refusal";
  resetWorld();
  refuse(kAdminRefusal);
  EXPECT(!oledRunCommand("radiopower off"), "a refused command reports failure");
  EXPECT(gToast == "Needs an admin" && gToastCount == 1,
         "an admin denial toasts 'Needs an admin', toast: '" + gToast + "'");
  EXPECT(gChecked.size() == 1 && gChecked[0] == "radiopower off" && gFireAndForget.empty(),
         "the helper runs the line once through the checked executor, ran: " + ran());

  resetWorld();
  refuse("Error: Super-admin access required for command 'factoryreset'.");
  EXPECT(!oledRunCommand("factoryreset") && gToast == "Needs a super admin",
         "a super-admin denial toasts 'Needs a super admin', toast: '" + gToast + "'");

  resetWorld();
  refuse("Error: Admin access required to start, stop or change logging. Contact an administrator.");
  EXPECT(!oledRunCommand("log start") && gToast == "Needs an admin",
         "the logging gate toasts 'Needs an admin', toast: '" + gToast + "'");

  resetWorld();
  refuse("Error: This recording belongs to alice. Only alice or an administrator can stop it.");
  EXPECT(!oledRunCommand("cameravideo stop") && gToast == "This recording belongs to alice.",
         "a recording owner's refusal keeps its first sentence, toast: '" + gToast + "'");
}

static void case_helper_error_replies() {
  gCase = "helper_error_replies";
  const struct { const char* reply; const char* toast; } kCases[] = {
    {"Error: Not connected to WiFi", "Not connected to WiFi"},
    {"ERROR: sensor busy", "sensor busy"},
    {"[ERROR] list full", "list full"},
    {"Error: Command timed out after 60 s. It may still finish; check the result before "
     "repeating it.",
     "Command timed out after 60 s."},
    {"Error: Not a whole number. Give the value in digits, for example 250.",
     "Not a whole number. Give the value in digits, for example 250."},  // fits: whole line
    {"Error: Saved to v1.2. done", "Saved to v1.2. done"},
    {"Error: first line\nsecond line", "first line"},
    {"Unknown command: frob\nType 'help' for available commands", "Unknown command: frob"},
    {"ERROR", "Command failed. Check logs."},
    {"Error:", "Command failed. Check logs."},
    {"", "Command failed. Check logs."},
  };
  for (const auto& c : kCases) {
    resetWorld();
    refuse(c.reply);
    EXPECT(!oledRunCommand("cmd") && gToast == c.toast && gToastCount == 1,
           std::string("reply '") + c.reply + "' toasts '" + c.toast + "', got '" + gToast + "'");
  }

  EXPECT(gToastDuration == 8000 && gToastIcon == OledUiIcon::ERROR,
         "command failures share the longer error toast style");
  for (const char* prefix : {"[ERROR]", "error:", "Usage:", "Unknown command:"}) {
    resetWorld(); gExecOk = true; gExecText = std::string(prefix) + " refused";
    EXPECT(!oledRunCommand("cmd"), "error prefixes cannot turn into success feedback");
  }
  // The executor's verdict is double-checked against the reply's error token.
  resetWorld();
  gExecOk = true;
  gExecText = "Error: refused";
  EXPECT(!oledRunCommand("cmd") && gToast == "refused",
         "an 'Error' reply is a failure even when the executor says ok, toast: '" + gToast + "'");

  // A reply longer than the helper's buffer stays bounded (ASan watches).
  resetWorld();
  refuse(("Error: " + std::string(300, 'x')).c_str());
  EXPECT(!oledRunCommand("cmd") && !gToast.empty() && gToast.size() < oled_message::kMessageBytes,
         "an over-long reply is bounded to the toast buffer");
}

#if HW1_HAVE_REPLY_TEXT
// The shared reply-to-toast mapping that oledRunCommand and the Add-WiFi flow
// (OLED_Mode_Network.cpp) both use.
static void case_reply_text() {
  gCase = "reply_text";
  resetWorld();
  const struct { bool ok; const char* reply; const char* text; } kCases[] = {
    {true, "OK: Saved network 'HomeNet' with priority 1", "Network saved"},
    {false, "Error: Admin access required for command 'wifiadd'. Contact an administrator.",
     "Needs an admin"},
    {false, "Error: Super-admin access required for command 'factoryreset'.", "Needs a super admin"},
    {false, "[ERROR] list full", "list full"},
    // The executor's timeout reply, cut to the caller's 40-byte buffer.
    {false, "Error: Command timed out after 60 s. It may still finish; check the result before "
            "repeating it.",
     "Command timed out after 60 s."},
    {false, "ERROR: list full\nsecond line", "list full"},
    {false, "Unknown command: frob", "Unknown command: frob"},
    {false, "ERROR", "Add failed"},
    {false, "", "Add failed"},
  };
  for (const auto& c : kCases) {
    char out[40];
    std::memset(out, '#', sizeof(out));
    oledCommandReplyText(c.ok, c.reply, "Network saved", "Add failed", out, sizeof(out));
    EXPECT(std::string(out) == c.text,
           std::string("ok=") + (c.ok ? "1" : "0") + " reply '" + c.reply + "' gives '" + c.text +
               "', got '" + out + "'");
  }
  char out[40];
  oledCommandReplyText(true, "OK", nullptr, "Failed", out, sizeof(out));
  EXPECT(out[0] == '\0', std::string("success without okText leaves the text empty, got '") + out + "'");
  oledCommandReplyText(false, nullptr, nullptr, "Failed", out, sizeof(out));
  EXPECT(std::string(out) == "Failed", std::string("a missing reply gives failText, got '") + out + "'");

  // A reply longer than the caller's buffer is cut and stays NUL-terminated (ASan watches).
  char small[8];
  std::memset(small, '#', sizeof(small));
  oledCommandReplyText(false, "Error: a long refusal reason", nullptr, "Failed", small, sizeof(small));
  EXPECT(std::string(small) == "a long ", std::string("the text is bounded by outSize, got '") + small + "'");
  EXPECT(gToastCount == 0 && gChecked.empty(), "the mapping neither toasts nor runs a command");
}
#endif

static void case_helper_success() {
  gCase = "helper_success";
  resetWorld();
  gExecText = "OK: Radio powered off";
  EXPECT(oledRunCommand("radiopower off"), "an accepted command reports success");
  EXPECT(gToastCount == 0, "success leaves the toast to the caller's own text, toast: '" + gToast + "'");
}
#endif

static void case_automations_toggle() {
  gCase = "automations_toggle";
  resetWorld();
  autoRenderData.items[0] = {7, true};
  autoRenderData.count = 1;
  refuse(kAdminRefusal);
  autoToggleSelected();
  EXPECT(ran() == "automation disable id=7", "the toggle runs the owner command, ran: " + ran());
  EXPECT(gToastIcon == OledUiIcon::ERROR, "a refused toggle shows error feedback");
  EXPECT(gToast == "Needs an admin", "a refused toggle shows the refusal, toast: '" + gToast + "'");
  EXPECT(autoForceRefresh, "the list refreshes after the action either way");

  resetWorld();
  autoRenderData.items[0] = {7, false};
  autoRenderData.count = 1;
  gExecText = "OK: Automation enabled";
  autoToggleSelected();
  EXPECT(gToast == "Enabled" && gToastIcon == OledUiIcon::SUCCESS,
         "an accepted toggle shows 'Enabled' in the list footer, shown: " + gToast);
}

static void case_automations_delete() {
  gCase = "automations_delete";
  resetWorld();
  sPendingDeleteId = 9;
  refuse(kAdminRefusal);
  autoDeleteConfirmed(nullptr);
  EXPECT(ran() == "automation delete id=9", "delete runs the owner command, ran: " + ran());
  EXPECT(gToastIcon == OledUiIcon::ERROR && gToast == "Needs an admin",
         "a refused delete shows the refusal, not 'Deleted', shown: " + gToast +
             ", toast: '" + gToast + "'");

  resetWorld();
  sPendingDeleteId = 9;
  autoDeleteConfirmed(nullptr);
  EXPECT(gToast == "Deleted" && gToastIcon == OledUiIcon::SUCCESS && autoForceRefresh,
         "an accepted delete shows 'Deleted', shown: " + gToast);
}

static void case_quick_radio_wifi() {
  gCase = "quick_radio_wifi";
  resetWorld();
  refuse(kAdminRefusal);
  toggleQuickRadio();
  land();
  EXPECT(ran() == "radiopower off", "the radio row runs radiopower, ran: " + ran());
  EXPECT(quickStatusMsg[0] == '\0' && gToast == "Needs an admin",
         "a refused radio toggle shows the refusal, not a status, status: '" +
             std::string(quickStatusMsg) + "', toast: '" + gToast + "'");

  resetWorld();
  toggleQuickRadio();
  land();
  EXPECT(gToast == "Radio OFF" && gToastCount == 1,
         "an accepted radio toggle shows 'Radio OFF', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  refuse(kAdminRefusal);
  toggleQuickWifi();
  land();
  EXPECT(ran() == "closewifi" && quickStatusMsg[0] == '\0' && gToast == "Needs an admin",
         "a refused Wi-Fi toggle shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  WiFi.connected = false;
  toggleQuickWifi();
  land();
  EXPECT(ran() == "openwifi --best" && gToast == "WiFi ON",
         "an accepted Wi-Fi connect shows 'WiFi ON', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  gRadioOn = false;
  toggleQuickWifi();
  land();
  EXPECT(ran() == "(none)" && gToast == "Radio is off. Turn it on first.",
         "with the radio off the Wi-Fi row refuses locally without a command");
}

static void case_quick_bluetooth_http() {
  gCase = "quick_bluetooth_http";
  resetWorld();
  refuse(kAdminRefusal);
  bluetoothToggleConfirmedQuick(nullptr);
  land();
  EXPECT(ran() == "openconsole" && quickStatusMsg[0] == '\0' && gToast == "Needs an admin",
         "a refused Bluetooth start shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  gBleServer = true;
  bluetoothToggleConfirmedQuick(nullptr);
  land();
  EXPECT(ran() == "closeble" && gToast == "Bluetooth OFF",
         "an accepted Bluetooth stop shows 'Bluetooth OFF', status: '" + std::string(quickStatusMsg) + "'");

  // The toggle is the whole radio: anything running (server, glasses, phone)
  // is closed with `closeble`; off, it starts the saved boot role (`openble`
  // for the client stack, `openconsole` for the app console server). It never
  // opens or closes one device.
  struct RoleCase { bool server, g2, phone; int mode; const char* command; const char* status; };
  const RoleCase roles[] = {
    {false, true, false, BLE_MODE_G2_CLIENT, "closeble", "Bluetooth OFF"},
    {false, true, true, BLE_MODE_G2_CLIENT, "closeble", "Bluetooth OFF"},
    {false, false, true, BLE_MODE_G2_CLIENT, "closeble", "Bluetooth OFF"},
    {true, false, false, BLE_MODE_G2_CLIENT, "closeble", "Bluetooth OFF"},
    {false, false, false, BLE_MODE_G2_CLIENT, "openble", "Bluetooth ON"},
    {false, false, false, BLE_MODE_SERVER, "openconsole", "Bluetooth ON"},
  };
  for (const RoleCase& role : roles) {
    resetWorld();
    gBleServer = role.server; gG2Client = role.g2; gPhonePipe = role.phone;
    gSettings.bleMode = role.mode;
    bluetoothToggleConfirmedQuick(nullptr);
    land();
    EXPECT(ran() == role.command && gToast == role.status,
           std::string("the Bluetooth toggle runs ") + role.command + ", ran: " + ran());
  }

  resetWorld();
  refuse(kAdminRefusal);
  httpToggleConfirmedQuick(nullptr);
  land();
  EXPECT(ran() == "openhttp" && quickStatusMsg[0] == '\0' && gToast == "Needs an admin",
         "a refused HTTP start shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  server = &gHttpHandle;
  httpToggleConfirmedQuick(nullptr);
  land();
  EXPECT(ran() == "closehttp" && gToast == "HTTP OFF",
         "an accepted HTTP stop shows 'HTTP OFF', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  WiFi.connected = false;
  httpToggleConfirmedQuick(nullptr);
  land();
  EXPECT(ran() == "(none)" && gToast == "Connect Wi-Fi first.",
         "without Wi-Fi the HTTP row refuses locally without a command");
}

#if HW1_HAVE_ASYNC_COMMAND
// The quick rows hand their command to cmd_exec and return: the frame loop
// keeps running while radiopower or openwifi works (the P4 radio toggle held
// the display task for 637 ms).
static void case_quick_async() {
  gCase = "quick_async";
  resetWorld();
  gExecText = "Radio off";
  toggleQuickRadio();
  EXPECT(ran() == "radiopower off" && gQueued.size() == 1,
         "the radio row queues radiopower, ran: " + ran());
  EXPECT(std::string(quickStatusMsg) == "Working..." && gToastCount == 0,
         "while it runs the status line says so, status: '" + std::string(quickStatusMsg) + "'");
  nextFrame();
  EXPECT(std::string(quickStatusMsg) == "Working...",
         "a frame before the command finishes changes nothing");

  // One request at a time: another row is refused locally and queues nothing.
  toggleQuickWifi();
  EXPECT(gQueued.size() == 1 && gChecked.size() == 1 && gToast == "Busy",
         "a second row while one runs toasts 'Busy', ran: " + ran() + ", toast: '" + gToast + "'");

  const int marks = gDirtyMarks;
  finishCommand();
  EXPECT(gDirtyMarks == marks && std::string(quickStatusMsg) == "Working..." && gToast == "Busy",
         "the executor only stores the reply; the frame loop picks it up");
  nextFrame();
  EXPECT(gToast == "Radio OFF" && quickStatusMsg[0] == '\0',
         "the next frame shows the result, status: '" + std::string(quickStatusMsg) + "'");
  nextFrame();
  EXPECT(gToast == "Radio OFF" && gToastCount == 2 && !oledCommandBusy(),
         "a reply is delivered once and the slot is free");

  // After it lands the next row runs.
  WiFi.connected = false;
  toggleQuickWifi();
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "openwifi --best",
         "the next row runs after the first finished, ran: " + ran());
  land();
  EXPECT(gToast == "WiFi ON", "and shows its own result");

  // A full command queue is retried for 2 s without holding the frame loop,
  // then ends as a refusal: its reason on the toast, the row free again.
  resetWorld();
  gQueueAccepts = false;
  toggleQuickRadio();
  EXPECT(gQueued.empty() && std::string(quickStatusMsg) == "Working..." && oledCommandBusy(),
         "a full queue keeps the press waiting for room, status: '" + std::string(quickStatusMsg) + "'");
  gNowMs += PendingCommand::kQueueRetryMs - 1;
  nextFrame();
  EXPECT(oledCommandBusy() && gToastCount == 0, "still retrying inside 2 s");
  gNowMs += PendingCommand::kQueueTryEveryMs;  // the next try after 2 s
  nextFrame();
  EXPECT(!oledCommandBusy() && quickStatusMsg[0] == '\0' &&
             gToast == "Command queue full. Try again in a few seconds." && gToastIcon == OledUiIcon::ERROR,
         "after 2 s the full queue is the refusal, toast: '" + gToast + "'");
  gQueueAccepts = true;
  toggleQuickRadio();
  land();
  EXPECT(gToast == "Radio OFF", "the next press runs normally");

  // The display session changed while the command ran (logout, another user):
  // its reply is not shown to the new session, and the row is free again.
  resetWorld();
  refuse(kAdminRefusal);
  toggleQuickRadio();
  gLiveEpoch = 8;
  land();
  EXPECT(gToastCount == 0 && quickStatusMsg[0] == '\0',
         "a reply from a replaced session shows nothing, toast: '" + gToast + "', status: '" +
             std::string(quickStatusMsg) + "'");
  gUiEpoch = 8;
  gExecOk = true;
  gExecText = "OK";
  toggleQuickRadio();
  land();
  EXPECT(gToast == "Radio OFF", "the new session's own press runs");

  // An executor 'ok' whose reply is an error is still a failure.
  resetWorld();
  gExecText = "Error: radio busy";
  toggleQuickRadio();
  land();
  EXPECT(quickStatusMsg[0] == '\0' && gToast == "radio busy",
         "an 'Error' reply toasts its reason, toast: '" + gToast + "'");

  // A reply longer than the mailbox stays bounded (ASan watches).
  resetWorld();
  refuse(("Error: " + std::string(400, 'x')).c_str());
  toggleQuickRadio();
  land();
  EXPECT(!gToast.empty() && gToast.size() < oled_message::kMessageBytes, "an over-long reply is bounded");
}
#endif


#if HW1_HAVE_ASYNC_COMMAND
// The background executeOledCommand (OLED background commands, step 1): the
// press returns at once, the busy flag shows while it runs, a second press is
// refused with "Busy" and queues nothing, and a refusal's reason is the toast
// when it ends. A success shows nothing (owner, 2026-11-22: the screen shows
// it); executeOledCommandShowResult, for a screen that does not, toasts what
// the command did.
static void case_background_actions() {
  gCase = "background_actions";
  resetWorld();
  gExecText = "OK: Radio powered off";
  executeOledCommand("radiopower off");
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "radiopower off" && oledCommandBusy() &&
             gToastCount == 0 && gDirtyMarks == 1,
         "the press queues the line at once and the busy mark asks for a frame, ran: " + ran());
  nextFrame();
  EXPECT(oledCommandBusy() && gToastCount == 0, "a frame before the reply changes nothing");

  // Busy: refused at once, nothing queued, for both entry points.
  executeOledCommand("ledclear");
  EXPECT(gQueued.size() == 1 && gChecked.size() == 1 && gToast == "Busy" && gToastCount == 1,
         "a press while one runs toasts 'Busy' and queues nothing, ran: " + ran());
  static int doneCalls = 0;
  doneCalls = 0;
  EXPECT(!oledRunCommandAsync("openwifi --best", [](bool, const void*) { ++doneCalls; }, nullptr) &&
             gQueued.size() == 1 && gToastCount == 2 && doneCalls == 0,
         "oledRunCommandAsync shares the one slot: refused, done not called");

  land();
  EXPECT(!oledCommandBusy() && gToast == "Busy" && gToastCount == 2,
         "a success shows no toast (the screen shows the new state), toast: '" + gToast + "'");
  EXPECT(gWiped.size() == 1 && gWiped[0] == "radiopower off", "the line is wiped when it ends");

  // A screen that does not show the result toasts it.
  resetWorld();
  gExecText = "OK: Radio powered off";
  executeOledCommandShowResult("radiopower off");
  EXPECT(gQueued.size() == 1 && oledCommandBusy() && gToastCount == 0, "it runs in the background too");
  land();
  EXPECT(!oledCommandBusy() && gToast == "Radio powered off" && gToastIcon == OledUiIcon::SUCCESS &&
             gToastCount == 1,
         "success shows the reply without its OK stamp, toast: '" + gToast + "'");
  nextFrame();
  EXPECT(gToastCount == 1, "a reply is shown once");
  resetWorld();
  refuse("Error: Not connected to WiFi");
  executeOledCommandShowResult("openhttp");
  land();
  EXPECT(gToast == "Not connected to WiFi" && gToastIcon == OledUiIcon::ERROR && gToastCount == 1,
         "its refusal is the same toast, toast: '" + gToast + "'");

  // Admin and super-admin refusals keep their short reason.
  resetWorld();
  refuse(kAdminRefusal);
  executeOledCommand("radiopower off");
  land();
  EXPECT(gToast == "Needs an admin" && gToastIcon == OledUiIcon::ERROR && gToastCount == 1,
         "an admin refusal toasts 'Needs an admin', toast: '" + gToast + "'");
  resetWorld();
  refuse("Error: Super-admin access required for command 'espnowsetpassphrase'.");
  executeOledCommand("espnowsetpassphrase primary \"x\"");
  land();
  EXPECT(gToast == "Needs a super admin", "a super-admin refusal, toast: '" + gToast + "'");
  resetWorld();
  refuse("Error: Not connected to WiFi");
  executeOledCommand("openhttp");
  land();
  EXPECT(gToast == "Not connected to WiFi", "any other refusal keeps its reason, toast: '" + gToast + "'");

  // What a success says.
  const struct { const char* reply; const char* toast; } kSuccess[] = {
    {"OK", "Done"},
    {"", "Done"},
    {"OK: ", "Done"},
    {"SUCCESS: Sensor logging started", "Sensor logging started"},
    {"{\"schema\":1,\"ok\":true}", "Done"},
    {"OK: Wi-Fi connected\nIP 10.0.0.2", "Wi-Fi connected"},
    {"Thermal camera opened. Frames every 500 ms on the OLED and the web page while it runs.",
     "Thermal camera opened."},
  };
  for (const auto& c : kSuccess) {
    resetWorld();
    gExecText = c.reply;
    executeOledCommandShowResult("cmd");
    land();
    EXPECT(gToast == c.toast && gToastIcon == OledUiIcon::SUCCESS,
           std::string("success reply '") + c.reply + "' toasts '" + c.toast + "', got '" + gToast + "'");
  }

  // No reply in 60 s: the timeout is the toast, the slot is free, and the late
  // reply is dropped (its mailbox freed by the executor).
  resetWorld();
  executeOledCommand("openwifi --best");
  gNowMs += PendingCommand::kReplyWaitMs - 1;
  nextFrame();
  EXPECT(oledCommandBusy() && gToastCount == 0, "still waiting inside 60 s");
  gNowMs += 1;
  nextFrame();
  EXPECT(!oledCommandBusy() && gToast == "Command timed out after 60 s." &&
             gToastIcon == OledUiIcon::ERROR,
         "after 60 s the timeout is the toast, toast: '" + gToast + "'");
  gExecText = "OK: Connected";
  land();
  EXPECT(gToastCount == 1 && gLiveAllocs == 0, "the late reply shows nothing and frees its mailbox");
  executeOledCommand("ledclear");
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "ledclear", "the next press runs");
  land();

  // The display session changed while it ran: nothing is shown.
  resetWorld();
  refuse(kAdminRefusal);
  executeOledCommand("radiopower off");
  gLiveEpoch = 8;
  land();
  EXPECT(gToastCount == 0 && !oledCommandBusy(), "a replaced session's reply shows nothing");

  // A line the executor can never take ends at the press with its reason.
  resetWorld();
  executeOledCommand("");
  EXPECT(!oledCommandBusy() && gQueued.empty() && gToastIcon == OledUiIcon::ERROR &&
             gToast == "Empty command. Type a command, or help for the list.",
         "an empty line is refused at once, toast: '" + gToast + "'");

  // Another task than the display's waits on the checked executor as before.
  resetWorld();
  gOwnerTask = false;
  refuse(kAdminRefusal);
  executeOledCommand("radiopower off");
  EXPECT(gQueued.empty() && !oledCommandBusy() && gToast == "Needs an admin" &&
             gChecked.size() == 1 && gChecked[0] == "radiopower off",
         "off the display task the action waits and toasts its refusal, ran: " + ran());
}
#endif

#if HW1_HAVE_VALUE_SCROLL
// A value-scroll row (OLED background commands, step 2): FM tune and the LED
// brightness nudge. A step moves the row at once and never waits or says
// "Busy"; while a command runs the newest value waits and replaces older ones;
// only that one is sent when the slot frees; then the row follows the device.
static OledValueScroll gRow;
static int32_t gDevice = 50;
static void scrollStep(int32_t delta) {
  oledValueScrollStep(gRow, "ledbrightness %ld", oledValueScrollShown(gRow, gDevice) + delta);
}
static void case_value_scroll() {
  gCase = "value_scroll";
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  EXPECT(oledValueScrollShown(gRow, gDevice) == 50, "an idle row shows the device's value");

  scrollStep(+5);
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "ledbrightness 55" && oledCommandBusy() &&
             oledValueScrollShown(gRow, gDevice) == 55 && gToastCount == 0,
         "the first step is sent at once and shown at once, ran: " + ran());
  scrollStep(+5);
  scrollStep(+5);
  scrollStep(+5);
  EXPECT(gQueued.size() == 1 && gToastCount == 0 && oledValueScrollShown(gRow, gDevice) == 70,
         "steps while one runs are neither sent nor refused 'Busy'; the row shows 70, toast: '" + gToast + "'");
  gDevice = 55;  // the first command lands
  land();
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "ledbrightness 70" && oledCommandBusy() &&
             gToastCount == 0,
         "when it ends only the newest value is sent, on the same frame, ran: " + ran());
  EXPECT(oledValueScrollShown(gRow, gDevice) == 70, "the row keeps its value while the device lags");
  gDevice = 70;
  land();
  EXPECT(!oledCommandBusy() && gChecked.size() == 2 && gToastCount == 0,
         "two commands for four steps, no toast on success, ran: " + ran());
  EXPECT(oledValueScrollShown(gRow, gDevice) == 70, "the device shows the wanted value");
  gDevice = 40;  // changed elsewhere (the web page)
  EXPECT(oledValueScrollShown(gRow, gDevice) == 40, "with nothing left to send the row follows the device");

  // Another screen's command holds the slot: the step waits, no "Busy".
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  executeOledCommand("ledclear");
  scrollStep(-5);
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "ledclear" && gToastCount == 0 &&
             oledValueScrollShown(gRow, gDevice) == 45,
         "a step while another command runs waits silently and is shown, toast: '" + gToast + "'");
  nextFrame();
  EXPECT(gQueued.size() == 1, "nothing is sent while the slot is busy");
  land();
  EXPECT(gQueued.size() == 1 && gQueued[0].line == "ledbrightness 45",
         "the waiting value goes out when the other command ends, ran: " + ran());
  gDevice = 45;
  land();
  EXPECT(!oledCommandBusy() && oledValueScrollShown(gRow, gDevice) == 45 && gToastCount == 0, "and lands");

  // A refusal: its reason is the toast and the row goes back to the device.
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  refuse(kAdminRefusal);
  scrollStep(+5);
  land();
  EXPECT(gToast == "Needs an admin" && gToastIcon == OledUiIcon::ERROR &&
             oledValueScrollShown(gRow, gDevice) == 50,
         "a refused step shows why and the row shows the device again, toast: '" + gToast + "'");

  // A refusal while a newer value waits: that value is still sent.
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  refuse("Error: LED busy");
  scrollStep(+5);
  scrollStep(+5);
  land();
  EXPECT(gToast == "LED busy" && gQueued.size() == 1 && gQueued[0].line == "ledbrightness 60" &&
             oledValueScrollShown(gRow, gDevice) == 60,
         "the newer value still goes out after a refusal, ran: " + ran());
  gExecOk = true;
  gExecText = "OK";
  gDevice = 60;
  land();

  // A success the device has not reported yet: the row holds its value for
  // the settle time, then follows the device.
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  scrollStep(+5);
  land();
  EXPECT(oledValueScrollShown(gRow, gDevice) == 55, "inside the settle time the row keeps 55");
  gNowMs += OledValueScroll::kSettleMs;
  EXPECT(oledValueScrollShown(gRow, gDevice) == 50, "after it the row follows the device");

  // A replaced display session: done(false), the row follows the device.
  resetWorld();
  gRow = OledValueScroll();
  gDevice = 50;
  scrollStep(+5);
  gLiveEpoch = 8;
  land();
  EXPECT(gToastCount == 0 && !oledCommandBusy() && oledValueScrollShown(gRow, gDevice) == 50,
         "a replaced session's step shows nothing and the row follows the device");
}
#endif

static void case_network_http_callbacks() {
  gCase = "network_http_callbacks";
  resetWorld();
  refuse(kAdminRefusal);
  webServerStartConfirmed(nullptr);
  land();
  EXPECT(ran() == "openhttp" && gToast == "Needs an admin",
         "a refused HTTP start from the Web menu shows the refusal, ran: " + ran() +
             ", toast: '" + gToast + "'");
  resetWorld();
  refuse(kAdminRefusal);
  webServerStopConfirmed(nullptr);
  land();
  EXPECT(ran() == "closehttp" && gToast == "Needs an admin",
         "a refused HTTP stop from the Web menu shows the refusal, toast: '" + gToast + "'");
}

int main() {
#if HW1_HAVE_RUN_COMMAND
  case_helper_admin_refusal();
  case_helper_error_replies();
  case_helper_success();
#endif
#if HW1_HAVE_REPLY_TEXT
  case_reply_text();
#endif
  case_automations_toggle();
  case_automations_delete();
  case_quick_radio_wifi();
  case_quick_bluetooth_http();
#if HW1_HAVE_ASYNC_COMMAND
  case_quick_async();
  case_background_actions();
#endif
#if HW1_HAVE_VALUE_SCROLL
  case_value_scroll();
#endif
  case_network_http_callbacks();
  resetWorld();  // checks the last case left nothing in flight
  if (gFailures) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("oled_command_feedback: all cases passed\n");
  return 0;
}
